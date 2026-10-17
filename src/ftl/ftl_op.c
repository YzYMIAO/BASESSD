/*
 * ftl_op.c — 完成派发实现：服务函数数组 + 提交 + 派发
 *
 * 职责：
 *   ① 持有 g_op_service[]（op tag → 完成服务函数）——**唯一的完成处理入口**；
 *   ② ftl_op_submit()  —— 打 tag、登记 ctx、提交（登记早于提交）；
 *   ③ ftl_op_dispatch()—— 取 ctx、解析实例、按 tag 调服务函数。
 *
 * 设计要点见 include/ftl/ftl_op.h 的文件头（尤其 op tag 的语义与 release 归属）。
 *
 * 文件结构：
 *   1. 服务函数数组    g_op_service
 *   2. 提交            ftl_op_submit
 *   3. 派发            ftl_op_dispatch
 *   4. 同步等待服务    ftl_op_sync_wait（原 ftl_sync_waiter_handler）
 */

#include "ftl/ftl_op.h"

#include "common/assertion.h"
#include "common/log.h"
#include "fc/fc.h" /* fc_submit / fc_command_id_to_command / fc_command_release */
#include "ftl/completion_table.h"
#include "ftl/ftl.h"              /* ftl_context_t */
#include "ftl/ftl_fc_interface.h" /* ftl_cmd_id */

/* 前置声明：服务函数定义在本文件末尾（数组初始化器须先见到声明）。 */
static void ftl_op_sync_wait(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx);

/* ── 1. 服务函数数组 ──
 *
 * C99 designated initializer 的 const 数组（非"注册函数"）：编译期即可发现重复/
 * 越界，且无 init-order 依赖。未列出的槽恒为 NULL（= 孤儿 tag）。
 *
 * 已点亮的槽：
 *   · FTL_OP_SYNC_WAIT：所有 ftl_sync_* 经 ftl_sync_submit_wait 登记 ctx 后走这里；
 *   · FTL_OP_HOST_IO  ：host 数据读写子操作（原 ftl_op_host_io 的 FTL 分支）；
 *   · FTL_OP_JOURNAL  ：Journal 刷页 job（阶段 2，ctx = &ftl->journal）；
 *   · FTL_OP_READ_RECOVERY：Read Recovery job（阶段 3，ctx = &ftl->rr）；
 *   · FTL_OP_GC / FTL_OP_WL：搬移 job（阶段 4，ctx = &ftl->mig）——**同一台引擎**，
 *     tag 分开只为日志/调试可辨（GC 回收 vs WL/预测退役）。
 *   · FTL_OP_L2P      ：L2P region 加载 / 写回（阶段 5，ctx = 该 cache line）；
 *   · FTL_OP_CHECKPOINT：Checkpoint job（阶段 6，ctx = &ftl->cp_job）；
 *   · FTL_OP_BBT / FTL_OP_WEAR：元数据表持久化 job（阶段 7，ctx = &ftl->alloc）。
 *   · FTL_OP_METADATA：L2P 日志块整理 job（阶段 8，ctx = &ftl->metadata_dir.job）。
 * **全部槽已点亮**（阶段 8 是最后一阶段）。 */
static ftl_op_service_fn_t const g_op_service[FTL_OP_COUNT] = {
    [FTL_OP_SYNC_WAIT]     = ftl_op_sync_wait,
    [FTL_OP_HOST_IO]       = ftl_op_host_io,
    [FTL_OP_JOURNAL]       = ftl_op_journal,
    [FTL_OP_READ_RECOVERY] = ftl_op_read_recovery,
    [FTL_OP_GC]            = ftl_op_migrate,
    [FTL_OP_WL]            = ftl_op_migrate,
    [FTL_OP_L2P]           = ftl_op_l2p,
    [FTL_OP_CHECKPOINT]    = ftl_op_checkpoint,
    [FTL_OP_BBT]           = ftl_op_bbt,
    [FTL_OP_WEAR]          = ftl_op_wear,
    [FTL_OP_METADATA]      = ftl_op_metadata,
};

/* ── 2. 提交 ── */

ftl_status_t ftl_op_submit(fc_command_t *cmd, ftl_op_id_t op, void *ctx)
{
    uint16_t cmd_id;

    ASSERT(cmd != NULL);
    ASSERT((uint32_t)op < (uint32_t)FTL_OP_COUNT);

    cmd_id = ftl_cmd_id(cmd);

    /* ① ID 上命令（FC 只做原样拷贝，完成时回传到 completion.op_tag）。 */
    cmd->op_tag = (DWORD)op;

    /* ② 登记必须**早于**提交（见头文件）。ctx == NULL 时主动清表项：cmd_id 是池下标、
     *    会复用，上一轮若有 job 因故没摘链而泄漏了表项，会毒到新命令——主动清是廉价保险。 */
    if (ctx != NULL)
    {
        if (completion_table_add(cmd_id, ctx) != FTL_OK)
        {
            return FTL_ERROR; /* 旁表满（编程/容量错误，非背压） */
        }
    }
    else
    {
        completion_table_remove(cmd_id);
    }

    /* ③ 提交。失败 ⇒ 回滚登记（瞬态背压，调用者下轮重试）。 */
    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        completion_table_remove(cmd_id);
        return FTL_AGAIN;
    }

    return FTL_OK;
}

/* ── 3. 派发 ── */

/* 派发深度（见 ftl_op_in_dispatch）。单线程顺序主循环 ⇒ 普通变量即可，无需原子。 */
static uint8_t g_dispatch_depth = 0U;

bool ftl_op_in_dispatch(void)
{
    return (g_dispatch_depth != 0U);
}

void ftl_op_dispatch(const fc_command_status_t *completion)
{
    fc_command_t       *cmd;
    ftl_op_service_fn_t fn;
    struct ftl_context *ftl;
    void               *ctx;
    uint8_t             op;

    ASSERT(completion != NULL);

    op = (uint8_t)completion->op_tag;
    fn = (op < (uint8_t)FTL_OP_COUNT) ? g_op_service[op] : NULL;

    /* command_id 由 NAND 从本命令回填；公共壳已断言 ID 在池内。 */
    cmd = fc_command_id_to_command((uint16_t)completion->command_id);

    if (fn == NULL)
    {
        /* 孤儿命令：tag 越界 / 该功能的服务函数尚未点亮（阶段 2~8 占位）。
         * 不清旁表则可能泄漏一个槽位，故一并 remove（幂等）。 */
        LOGE("[FTL] orphan completion cmd=%u op=%u status=0x%02x\r\n",
             (unsigned)completion->command_id,
             (unsigned)op,
             (unsigned)completion->status);
        completion_table_remove((uint16_t)completion->command_id);
        (void)fc_command_release(cmd);
        return;
    }

    /* 先摘链再回调（同 completion_table_drain 的既有约定）——服务函数内部可能
     * 重入登记/释放表项，先摘可避免它在自己的回调里踩到本项。 */
    ctx = completion_table_take((uint16_t)completion->command_id);

    /* 实例归属按**命令自身**的 ch/ce/lun 解析（不是"谁在 drain"）：单线程顺序主循环下
     * "实例 A 的 drain 遇到实例 B 的 completion"是必然事件。
     * 不变量：路由表由 main.c 在**逐实例 boot 之前**设好（见 ssd_ftl_boot_all ①：
     * 先 completion_table_init 清一次、再 set_route），因为 boot 期 F2 重建就会经
     * l2p_cache_read_sync 抽完成表。故此处解析出 NULL 只可能是编程错误，
     * 不再是"开机早期"的正常状态。 */
    ftl = completion_table_route((uint8_t)cmd->config.ch,
                                 (uint8_t)cmd->config.ce,
                                 (uint8_t)cmd->config.lun);

    /* 深度计数包住服务函数（含它内部的嵌套派发）：ftl_op_in_dispatch() 据此判断
     * "现在不能同步等待"。溢出（255 层嵌套）不可能发生——每层都要消费一条命令。 */
    g_dispatch_depth++;
    fn(ftl, completion, ctx);
    g_dispatch_depth--;
}

/* ── 4. 同步等待服务函数 ── */

/* ftl_op_sync_wait — 同步等待的完成服务：把 completion 存入 waiter、置 done=1。
 *
 * **不 release 命令**——调用者（ftl_sync_submit_wait）要在 wait 返回后读 cmd 数据
 * （如 ftl_cmd_read_data）才 release，这里释放即悬垂。 */
static void ftl_op_sync_wait(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx)
{
    ftl_sync_waiter_t *waiter = (ftl_sync_waiter_t *)ctx;

    (void)ftl; /* 同步等待与实例无关（waiter 由调用者持栈） */

    waiter->completion = *completion;
    waiter->done       = 1U;
}

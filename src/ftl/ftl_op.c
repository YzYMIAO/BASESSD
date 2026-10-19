/*
 * ftl_op.c — 完成派发实现：服务函数数组 + 提交 + 派发
 *
 * 职责：
 *   ① 持有 g_op_callbacks[]（op tag → 完成服务函数）——**唯一的完成处理入口**；
 *   ② ftl_op_submit()  —— 打 tag、登记 ctx、提交（登记早于提交）；
 *   ③ ftl_op_dispatch()—— 取 ctx、解析实例、按 tag 调服务函数。
 *
 * 设计要点见 include/ftl/ftl_op.h 的文件头（尤其 op tag 的语义与 release 归属）。
 *
 * 文件结构：
 *   1. 服务函数数组    g_op_callbacks
 *   2. 提交            ftl_op_submit
 *   3. 派发            ftl_op_dispatch
 *   4. 同步等待服务    ftl_sync_wait_callback
 */

#include "ftl_op.h"

#include "assertion.h"
#include "completion_table.h"
#include "fc.h"               /* fc_submit / fc_command_id_to_command / fc_command_release */
#include "ftl.h"              /* ftl_context_t */
#include "ftl_fc_interface.h" /* ftl_cmd_id */
#include "log.h"

/* 前置声明：服务函数定义在本文件末尾（数组初始化器须先见到声明）。 */
static void ftl_sync_wait_callback(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx);

/* ── 1. 服务函数数组 ──
 *
 * C99 designated initializer 的 const 数组（非"注册函数"）：编译期即可发现重复/
 * 越界，且无 init-order 依赖。**每个非 NONE 的 ftl_op_id_t 都必须在这里有实现**——
 * 未列出的槽恒为 NULL，派发时判为孤儿（release + LOGE）。
 *
 *   · FTL_OP_SYNC_WAIT：所有 ftl_sync_* 经 ftl_sync_submit_wait 登记 ctx 后走这里；
 *   · FTL_OP_HOST_IO  ：host 数据读写子操作（ctx 不用，服务函数自己 pending_lookup）；
 *   · FTL_OP_JOURNAL  ：Journal 刷页 job（ctx = ftl->journal）；
 *   · FTL_OP_READ_RECOVERY：Read Recovery job（ctx = &ftl->read_recovery_handle）；
 *   · FTL_OP_GC / FTL_OP_WL：搬移 job（ctx = ftl->migrate_handle）——**同一台引擎**，
 *     tag 分开只为日志/调试可辨（GC/WL 回收 vs 慢死预测退役）；
 *   · FTL_OP_L2P      ：L2P region 加载 / 写回（ctx = 该 cache line）；
 *   · FTL_OP_CHECKPOINT：Checkpoint job（ctx = ftl->checkpoint_handle）；
 *   · FTL_OP_BBT      ：元数据表持久化 job（ctx = &ftl->alloc）；
 *   · FTL_OP_METADATA ：L2P 日志块整理 job（ctx = ftl->metadata_dir->handle）。 */
static ftl_op_callback_fn_t const g_op_callbacks[FTL_OP_COUNT] = {
    [FTL_OP_SYNC_WAIT]     = ftl_sync_wait_callback,
    [FTL_OP_HOST_IO]       = ftl_host_io_callback,
    [FTL_OP_JOURNAL]       = ftl_journal_callback,
    [FTL_OP_READ_RECOVERY] = ftl_read_recovery_callback,
    [FTL_OP_GC]            = ftl_migrate_callback,
    [FTL_OP_WL]            = ftl_migrate_callback,
    [FTL_OP_L2P]           = ftl_l2p_callback,
    [FTL_OP_CHECKPOINT]    = ftl_checkpoint_callback,
    [FTL_OP_BBT]           = ftl_bbt_callback,
    [FTL_OP_METADATA]      = ftl_metadata_callback,
};

/* ── 2. 提交 ── */

ftl_status_t ftl_op_submit(fc_command_t *cmd, ftl_op_id_t op, void *ctx)
{
    uint16_t cmd_id = ftl_cmd_id(cmd);

    /* ① ID 上命令（FC 只做原样拷贝，完成时回传到 completion.op_tag）。 */
    cmd->op_tag = op;

    /* ② 登记必须**早于**提交（见头文件）。
     *    ctx == NULL（host I/O）：主动清表项——cmd_id 是池下标、会复用，主动清是廉价保险。
     *    ctx != NULL：由 completion_table_add 保证同一 id 不出现第二条表项（见该函数）。 */
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
    uint8_t              op = (uint8_t)completion->op_tag;
    ftl_op_callback_fn_t fn = (op < (uint8_t)FTL_OP_COUNT) ? g_op_callbacks[op] : NULL;

    /* command_id 由 NAND 从本命令回填；公共壳已断言 ID 在池内。 */
    fc_command_t *cmd = fc_command_id_to_command((uint16_t)completion->command_id);

    if (fn == NULL)
    {
        /* 孤儿完成：tag 越界（≥ FTL_OP_COUNT）或 FTL_OP_NONE。
         * 不清旁表则可能泄漏一个槽位，故一并 remove（幂等）。 */
        LOGE("[FTL] orphan completion cmd=%u op=%u status=0x%02x\r\n",
             (unsigned)completion->command_id,
             (unsigned)op,
             completion->status);
        completion_table_remove((uint16_t)completion->command_id);
        (void)fc_command_release(cmd);
        return;
    }

    /* 先摘链再回调（同 completion_table_drain 的既有约定）——服务函数内部可能
     * 重入登记/释放表项，先摘可避免它在自己的回调里踩到本项。 */
    void *ctx = completion_table_take((uint16_t)completion->command_id);

    /* 实例归属按**命令自身**的 ch/ce/lun 解析（不是"谁在 drain"）：单线程顺序主循环下
     * "实例 A 的 drain 遇到实例 B 的 completion"是必然事件。
     * 不变量：路由表由 main.c 在**逐实例 boot 之前**设好（见 ssd_ftl_boot_all ①：
     * 先 completion_table_init 清一次、再 set_route），因为 boot 期 F2 重建就会经
     * l2p_cache_read_sync 抽完成表。故此处解析出 NULL 只可能是编程错误，
     * 不再是"开机早期"的正常状态。 */
    struct ftl_context *ftl = completion_table_route((uint8_t)cmd->config.ch,
                                                     (uint8_t)cmd->config.ce,
                                                     (uint8_t)cmd->config.lun);

    /* 深度计数包住服务函数（含它内部的嵌套派发）：ftl_op_in_dispatch() 据此判断
     * "现在不能同步等待"。溢出（255 层嵌套）不可能发生——每层都要消费一条命令。 */
    g_dispatch_depth++;
    fn(ftl, completion, ctx);
    g_dispatch_depth--;
}

/* ── 4. 同步等待服务函数 ── */

/* ftl_sync_wait_callback — 同步等待的完成服务：把 completion 存入 waiter、置 done=1。
 *
 * **不 release 命令**——调用者（ftl_sync_submit_wait）要在 wait 返回后读 cmd 数据
 * （如 ftl_cmd_read_data）才 release，这里释放即悬垂。 */
static void ftl_sync_wait_callback(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx)
{
    (void)ftl; /* 同步等待与实例无关（waiter 由调用者持栈） */

    /* ctx == NULL = 超时路径已把旁表项摘掉，这是**迟到的完成**：waiter 的栈帧
     * 早已失效，绝不能再解引用；但命令槽位仍必须回收，否则命令池泄漏。 */
    if (ctx == NULL)
    {
        LOGE("[FTL] late sync completion cmd=%u status=0x%02x — released\r\n",
             (unsigned)completion->command_id,
             completion->status);
        (void)fc_command_release(fc_command_id_to_command((uint16_t)completion->command_id));
        return;
    }

    ftl_sync_waiter_t *waiter = (ftl_sync_waiter_t *)ctx;

    waiter->completion = *completion;
    waiter->done       = 1U;
}

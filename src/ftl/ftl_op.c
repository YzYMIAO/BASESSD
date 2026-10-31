/*
 * ftl_op.c — 完成派发面：ctx 旁表 + 实例路由 + op 回调 + CQ 排空
 *
 * 职责（全局单例，非 per-instance）：
 *   ① ctx 旁表      command_id → ctx；
 *   ② 实例路由表    (ch,ce,lun) → ftl_ctx_t；
 *   ③ 服务函数数组  op_tag → 完成服务函数；
 *   ④ 提交          ftl_op_submit / ftl_op_submit_batch；
 *   ⑤ 派发/排空     ftl_op_dispatch / ftl_op_drain。
 *
 * ⚠ command_id 由 FC 全局池铸出、CQ 是全局的 ⇒ 完成必须先按命令自身的
 *   ch/ce/lun 路由到实例，再按 op_tag 调服务函数。两张表不能拆成 per-instance。
 *
 * ⚠ 初始化顺序：ftl_op_init / ftl_op_route_set 必须在所有实例 boot 之前调用
 *   （main.c 的 ssd_ftl_boot_all ①），boot 期 full metadata rebuild 就会抽完成表。
 *
 * 文件结构：
 *   1. 静态资源     g_op_ctx / g_op_route / g_op_callbacks
 *   2. 初始化       ftl_op_init
 *   3. ctx 旁表     add / remove / take / count
 *   4. 实例路由     set / get / get_region
 *   5. 提交         submit / submit_batch
 *   6. 派发/排空    is_in_dispatch / dispatch / drain
 *   7. 同步等待服务 ftl_sync_wait_callback
 */

#include "ftl_op.h"

#include "assertion.h"
#include "fc.h"               /* fc_submit / fc_poll_completion / fc_command_release */
#include "ftl.h"              /* ftl_ctx_t */
#include "ftl_fc_interface.h" /* ftl_cmd_id */
#include "l2p_cache.h"        /* ftl_dim_ch/ce/lun（dim → 坐标的唯一分解点） */
#include "log.h"

#include <string.h>

// 旁表按 command_id 键 ⇒ 容量上界 = 同时在飞命令数 ≤ FC 命令池深度。
// 池深是 FC 层的事实上界，写成常量会在 FC 扩容时静默变小（H4）。
_Static_assert(FTL_OP_CTX_TABLE_SIZE >= FC_COMMAND_POOL_DEPTH, "旁表容量必须 ≥ FC 命令池深度（推导见 ftl_op.h）");

/**
 * @brief 前置声明：服务函数定义在本文件末尾。
 */
static void ftl_sync_wait_callback(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx);

/* ── 1. 静态资源 ── */

static ftl_op_ctx_entry_t g_op_ctx[FTL_OP_CTX_TABLE_SIZE];

// 实例路由表：按命令 ch/ce/lun → FTL 实例。
static struct ftl_context *g_op_route[CH_NUM][CE_NUM][LUN_NUM];

// 派发深度（见 ftl_op_is_in_dispatch）。单线程顺序主循环 ⇒ 普通变量即可。
static BYTE g_dispatch_depth = 0U;

/* ── 服务函数数组 ──
 *
 * C99 designated initializer 的 const 数组（非"注册函数"）：编译期即可发现重复/
 * 越界，且无 init-order 依赖。**每个非 NONE 的 ftl_op_id_e 都必须在这里有实现**——
 * 未列出的槽恒为 NULL，派发时判为孤儿（release + LOGE）。
 *
 *   · FTL_OP_SYNC_WAIT：所有 ftl_sync_* 经 ftl_sync_submit_wait 登记 ctx 后走这里；
 *   · FTL_OP_HOST_IO  ：host 数据读写子操作（ctx 不用，服务函数自己 pend_find）；
 *   · FTL_OP_JOURNAL  ：Journal 刷页 job（ctx = ftl->journal）；
 *   · FTL_OP_READ_RECOVERY：Read Recovery job（ctx = &ftl->read_recovery_handle）；
 *   · FTL_OP_GC / FTL_OP_WL：搬移 job（ctx = ftl->mig_handle）——**同一台引擎**，
 *     tag 分开只为日志/调试可辨（GC/WL 回收 vs 慢死预测退役）；
 *   · FTL_OP_L2P      ：L2P region 加载 / 写回（ctx = 该 cache line）；
 *   · FTL_OP_CHECKPOINT：Checkpoint job（ctx = ftl->cp_handle）；
 *   · FTL_OP_BBT      ：元数据表持久化 job（ctx = &ftl->alloc）；
 *   · FTL_OP_METADATA ：L2P 日志块整理 job（ctx = ftl->metadata_dir->handle）；
 *   · FTL_OP_CACHE_PAD：cache 写序列的 padding END（ctx = NULL）——用一条不映射的
 *     零页给提前回执的最后一页提供 FAIL_CACHE 结算点，然后关闭序列。 */
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
    [FTL_OP_CACHE_PAD]     = ftl_cache_pad_callback,
};

/* ── 2. 初始化 ── */

// ftl_op_init — 清空 ctx 旁表 + 实例路由表（开机调用一次）。
//
// ⚠ 清的是**全局**资源（两个 file-static），只能调一次：由 main.c 的
// ssd_ftl_boot_all 在**所有实例 boot 之前**调用。**不可**放进逐实例的 ftl_init ——
// 那样实例 N 的 init 会抹掉实例 N-1 在飞的 ctx 登记，以及已设好的路由表。
void ftl_op_init(void)
{
    memset(g_op_ctx, 0, sizeof(g_op_ctx));
    memset(g_op_route, 0, sizeof(g_op_route));
}

/* ── 3. ctx 旁表 ── */

// 实现要点：本闸门保证**同一 cmd_id 至多一条表项**——命中已有项即**原地覆盖** ctx，
// 不追加第二条。这个不变量必须在这里守：命令池只有 FC_COMMAND_POOL_DEPTH
// 个 id，会被复用；若旧项因完成从未被观测而残留（CQ 满时 fc.c 强制置完成、
// FTL 感知不到那条 completion），再 add 同一 id 就会让表里出现两条同 id 项，
// 而 ftl_op_ctx_take 按**首个**匹配摘链 ⇒ 服务函数拿到**陈旧**的 ctx
// 并据此改状态，本命令自己的表项则永久泄漏。
// 覆盖时记 LOGW：这条路径一旦发生必须自己报出来，否则就是静默失效（C8）。
// ctx 允许为 NULL（表示"无私有状态"，服务函数自行派生）。
ftl_status_e ftl_op_ctx_add(WORD cmd_id, void *ctx)
{
    for (BYTE i = 0U; i < FTL_OP_CTX_TABLE_SIZE; i++)
    {
        if ((g_op_ctx[i].used == true) && (g_op_ctx[i].cmd_id == cmd_id))
        {
            LOGW(LOG_TAG_FTL "completion entry cmd=%hu reused — stale ctx overwritten\r\n", cmd_id);
            g_op_ctx[i].ctx = ctx;
            return FTL_OK;
        }
    }

    for (BYTE i = 0U; i < FTL_OP_CTX_TABLE_SIZE; i++)
    {
        if (g_op_ctx[i].used == false)
        {
            g_op_ctx[i].used   = true;
            g_op_ctx[i].cmd_id = cmd_id;
            g_op_ctx[i].ctx    = ctx;
            return FTL_OK;
        }
    }

    return FTL_ERROR;
}

// ftl_op_ctx_remove — 按 command_id 释放表项（未找到则 no-op，幂等）
void ftl_op_ctx_remove(WORD cmd_id)
{
    for (BYTE i = 0U; i < FTL_OP_CTX_TABLE_SIZE; i++)
    {
        if ((g_op_ctx[i].used == true) && (g_op_ctx[i].cmd_id == cmd_id))
        {
            g_op_ctx[i].used = false;
            return;
        }
    }
}

// ftl_op_ctx_take — 查 + 摘链 + 返回 ctx（未登记返回 NULL）
void *ftl_op_ctx_take(WORD cmd_id)
{
    for (BYTE i = 0U; i < FTL_OP_CTX_TABLE_SIZE; i++)
    {
        if ((g_op_ctx[i].used == true) && (g_op_ctx[i].cmd_id == cmd_id))
        {
            void *ctx = g_op_ctx[i].ctx;

            g_op_ctx[i].used = false;
            return ctx;
        }
    }

    return NULL;
}

// ftl_op_ctx_count — 旁表当前占用项数（观测/自检用）
WORD ftl_op_ctx_count(void)
{
    WORD count = 0U;

    for (BYTE i = 0U; i < FTL_OP_CTX_TABLE_SIZE; i++)
    {
        if (g_op_ctx[i].used == true)
        {
            count++;
        }
    }

    return count;
}

/* ── 4. 实例路由 ── */

// 实现要点：逐格拷贝而非存指针——调用者的数组可能是栈/局部量，逐格拷贝让
// g_op_route 成为唯一持有者，避免悬垂。
void ftl_op_route_set(struct ftl_context *route[][CE_NUM][LUN_NUM])
{
    for (BYTE ch = 0U; ch < CH_NUM; ch++)
    {
        for (BYTE ce = 0U; ce < CE_NUM; ce++)
        {
            for (BYTE lun = 0U; lun < LUN_NUM; lun++)
            {
                g_op_route[ch][ce][lun] = (route != NULL) ? route[ch][ce][lun] : NULL;
            }
        }
    }
}

// 实现要点：越界属编程错误（坐标由 FTL 从 PBA 灌入并经 pba_is_valid 校验）⇒ ASSERT。
// 返回 NULL 同属编程错误（route_set 漏调）——但**不在此处断言**：判据在
// **被调方**（各服务函数入口；实例绑定由 ftl_contract_audit 在 boot 期证明），
// 此处再断一次只是把同一判据写两遍（C7：它不会独立失败）。
// 不用 ftl 的服务函数（SYNC_WAIT / JOURNAL / BBT）收到 NULL 也无需处理。
struct ftl_context *ftl_op_route_get(BYTE ch, BYTE ce, BYTE lun)
{
    ASSERT((ch < CH_NUM) && (ce < CE_NUM) && (lun < LUN_NUM));
    return g_op_route[ch][ce][lun];
}

// 实现要点：dim = region % FTL_DIM_TOTAL，再按 ftl_dim_ch/ce/lun 分解——分解式与
// lba_route_* **共用同一份实现**（l2p_cache.h 的 ftl_dim_*），不是各写一套（C9）。
struct ftl_context *ftl_op_route_get_region(DWORD global_region)
{
    const DWORD dim = global_region % FTL_DIM_TOTAL;

    return ftl_op_route_get(ftl_dim_ch(dim), ftl_dim_ce(dim), ftl_dim_lun(dim));
}

bool ftl_op_cache_seq_active(void)
{
    for (BYTE ch = 0U; ch < CH_NUM; ch++)
    {
        for (BYTE ce = 0U; ce < CE_NUM; ce++)
        {
            for (BYTE lun = 0U; lun < LUN_NUM; lun++)
            {
                const struct ftl_context *ftl = g_op_route[ch][ce][lun];

                // draining 也算「序列未结束」：门控在排空期间仍然关着，后台提交同样会被
                // 拒（见 ftl_op_seq_gate），所以 deadline 挂起判据必须把它算进来。
                if ((ftl != NULL) && ((ftl->cache_seq.active == true) || (ftl->cache_seq.draining == true)))
                {
                    return true;
                }
            }
        }
    }

    return false;
}

/* ── 5. 提交 ── */

/**
 * @brief 该 op 被门控时是否应请求序列断点。
 *
 * @param op 提交者登记的操作类型（FTL_OP_*）
 * @return true = 前台 / 同步 / 恢复类提交：拒绝即请求下一窗口 END（D4）
 *         false = 后台元数据：只被挡住、不请求断点，关在段边界外
 *
 * @note 分解依据（docs/design/ftl_design.md §3.16.5）：顺序序列占用每个 plane 的 cache 寄存器，
 *       Journal / L2P / GC / WL / CP / 元数据目录在此期间**本来就写不进去**。若它们
 *       每次重试都置 break_requested，序列就会被后台任务切回逐窗口长度（这正是
 *       传输段大小成为序列上限的机制）。设计 A 让它们等在段边界（配额 / block 头 /
 *       region 不驻留）由 FTL 主动收尾后落盘。
 */
static bool ftl_op_seq_gate_breaks(ftl_op_id_e op)
{
    return (op == FTL_OP_HOST_IO) || (op == FTL_OP_SYNC_WAIT) || (op == FTL_OP_READ_RECOVERY);
}

// 序列门控：cache 序列 `active || draining` 期间，只允许所有者提交该 LUN 的命令；其余返回
// FTL_AGAIN（序列在下一窗口末尾 END，后台/其他 host 命令随后执行）。
// draining = 窗口已提交完但命令仍在飞：门控必须继续关到排空，否则器件侧的 31h/3Fh 管线
// 会被别的提交劈开（RISK-CACHE-14）。
static bool ftl_op_seq_gate(const fc_command_t *cmd, ftl_op_id_e op)
{
    struct ftl_context *ftl = ftl_op_route_get(cmd->config.ch, cmd->config.ce, cmd->config.lun);

    if ((ftl != NULL) && ((ftl->cache_seq.active == true) || (ftl->cache_seq.draining == true)) && (ftl->cache_seq.owner_submit == false))
    {
        if (ftl_op_seq_gate_breaks(op) == true)
        {
            ftl->cache_seq.break_requested = true;
        }
        return true;
    }

    return false;
}

ftl_status_e ftl_op_submit(fc_command_t *cmd, ftl_op_id_e op, void *ctx)
{
    WORD cmd_id = ftl_cmd_id(cmd);

    if (ftl_op_seq_gate(cmd, op) == true)
    {
        return FTL_AGAIN;
    }

    // ① ID 上命令（FC 只做原样拷贝，完成时回传到 completion.op_tag）。
    cmd->op_tag = op;

    // ② 登记必须**早于**提交（见头文件）。
    // ctx == NULL（host I/O）：主动清表项——cmd_id 是池下标、会复用，主动清是廉价保险。
    // ctx != NULL：由 ftl_op_ctx_add 保证同一 id 不出现第二条表项（见该函数）。
    if (ctx != NULL)
    {
        if (ftl_op_ctx_add(cmd_id, ctx) != FTL_OK)
        {
            return FTL_ERROR; /* 旁表满（编程/容量错误，非背压） */
        }
    }
    else
    {
        ftl_op_ctx_remove(cmd_id);
    }

    // ③ 提交。失败 ⇒ 回滚登记（瞬态背压，调用者下轮重试）。
    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        ftl_op_ctx_remove(cmd_id);
        return FTL_AGAIN;
    }

    return FTL_OK;
}

/* ── 批量提交（cache program 批次）── */
ftl_status_e ftl_op_submit_batch(fc_command_t *const cmds[], DWORD n, ftl_op_id_e op, void *ctx)
{
    ASSERT(cmds != NULL);
    ASSERT(n > 0U);

    for (DWORD i = 0U; i < n; i++)
    {
        if (ftl_op_seq_gate(cmds[i], op) == true)
        {
            return FTL_AGAIN;
        }
    }

    DWORD added = 0U;

    for (DWORD i = 0U; i < n; i++)
    {
        const WORD cmd_id = ftl_cmd_id(cmds[i]);

        cmds[i]->op_tag = op;

        if (ctx != NULL)
        {
            if (ftl_op_ctx_add(cmd_id, ctx) != FTL_OK)
            {
                for (DWORD k = 0U; k < added; k++)
                {
                    ftl_op_ctx_remove(ftl_cmd_id(cmds[k]));
                }
                return FTL_ERROR;
            }
        }
        else
        {
            ftl_op_ctx_remove(cmd_id);
        }
        added++;
    }

    const fc_status_e submit_status = fc_submit_batch(cmds, n);

    if (submit_status != FC_STATUS_OK)
    {
        if (ctx != NULL)
        {
            for (DWORD i = 0U; i < n; i++)
            {
                ftl_op_ctx_remove(ftl_cmd_id(cmds[i]));
            }
        }

        // QUEUE_FULL 是瞬态资源（下轮重试）；其余是链契约违反等编程错误，永久失败。
        return (submit_status == FC_STATUS_QUEUE_FULL) ? FTL_AGAIN : FTL_ERROR;
    }

    return FTL_OK;
}

/* ── 6. 派发 / 排空 ── */

bool ftl_op_is_in_dispatch(void)
{
    return (g_dispatch_depth != 0U);
}

void ftl_op_dispatch(const fc_command_status_t *completion)
{
    BYTE                 op = completion->op_tag;
    ftl_op_callback_fn_t fn = (op < FTL_OP_COUNT) ? g_op_callbacks[op] : NULL;

    // command_id 由 NAND 从本命令回填；公共壳已断言 ID 在池内。
    fc_command_t *cmd = fc_command_id_to_command(completion->command_id);

    if (fn == NULL)
    {
        // 孤儿完成：tag 越界（≥ FTL_OP_COUNT）或 FTL_OP_NONE。
        // 不清旁表则可能泄漏一个槽位，故一并 remove（幂等）。
        LOGE(LOG_TAG_FTL "orphan completion cmd=%hu op=%hhu status=0x%02hx\r\n", completion->command_id, op, completion->status);
        ftl_op_ctx_remove(completion->command_id);
        (void)fc_command_release(cmd);
        return;
    }

    // 先摘链再回调——服务函数内部可能重入登记/释放表项，先摘可避免它在自己的回调里踩到本项。
    void *ctx = ftl_op_ctx_take(completion->command_id);

    // 实例归属按**命令自身**的 ch/ce/lun 解析（不是"谁在 drain"）：单线程顺序主循环下
    // "实例 A 的 drain 遇到实例 B 的 completion"是必然事件。
    // 不变量：路由表由 main.c 在**逐实例 boot 之前**设好（见 ssd_ftl_boot_all ①：
    // 先 ftl_op_init 清一次、再 route_set），因为 boot 期 full metadata rebuild 就会经
    // l2p_read_sync 抽完成表。故此处解析出 NULL 只可能是编程错误，
    // 不再是"开机早期"的正常状态。
    struct ftl_context *ftl = ftl_op_route_get(cmd->config.ch, cmd->config.ce, cmd->config.lun);

    // 深度计数包住服务函数（含它内部的嵌套派发）：ftl_op_is_in_dispatch() 据此判断
    // "现在不能同步等待"。溢出（255 层嵌套）不可能发生——每层都要消费一条命令。
    g_dispatch_depth++;
    fn(ftl, completion, ctx);
    g_dispatch_depth--;
}

// ftl_op_drain — 消费所有 CQ entry 并派发（ftl_service_pump 的 Phase 2）。
//
// 每条 completion 交 ftl_op_dispatch()：按 op_tag 查 g_op_callbacks[]，从旁表摘 ctx，
// 再调对应服务函数。循环持续到 CQ 抽干为止。
void ftl_op_drain(void)
{
    fc_command_status_t completion;

    while (fc_poll_completion(&completion) == FC_STATUS_OK)
    {
        ftl_op_dispatch(&completion);
    }
}

/* ── 7. 同步等待服务函数 ── */

/**
 * @brief ftl_sync_wait_callback — 同步等待的完成服务：把 completion 存入 waiter、置 done=1。
 *
 * **不 release 命令**——调用者（ftl_sync_submit_wait）要在 wait 返回后读 cmd 数据
 * （如 ftl_cmd_read_data）才 release，这里释放即悬垂。
 *
 * @param ftl        实例（本函数不用）。
 * @param completion FC 完成状态。
 * @param ctx        `ftl_sync_waiter_t *`；NULL = 迟到的完成（旁表项已摘）。
 */
static void ftl_sync_wait_callback(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx)
{
    (void)ftl; /* 同步等待与实例无关（waiter 由调用者持栈） */

    // ctx == NULL = 超时路径已把旁表项摘掉，这是**迟到的完成**：waiter 的栈帧
    // 早已失效，绝不能再解引用；但命令槽位仍必须回收，否则命令池泄漏。
    if (ctx == NULL)
    {
        LOGE(LOG_TAG_FTL "late sync completion cmd=%hu status=0x%02hx — released\r\n", completion->command_id, completion->status);
        (void)fc_command_release(fc_command_id_to_command(completion->command_id));
        return;
    }

    ftl_sync_waiter_t *waiter = (ftl_sync_waiter_t *)ctx;

    waiter->completion = *completion;
    waiter->done       = true;
}

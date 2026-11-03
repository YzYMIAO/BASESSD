/*
 * ftl_op.c — 完成派发面：request 表 + 实例路由 + op 回调 + CQ 排空
 *
 * 模块职责（全局单例，非 per-instance）：
 *   ① request 表    command_id 映射到 {ctx, op}；
 *   ② 实例路由表    (ch,ce,lun) 经 compact die 映射到 ftl_ctx_t；
 *   ③ 服务函数数组  request.op 映射到完成服务函数；
 *   ④ 提交          ftl_op_submit / ftl_op_submit_batch；
 *   ⑤ 派发/排空     ftl_op_dispatch / ftl_op_drain。
 *
 * command_id 由 FC 全局池分配，CQ 是全局的。完成必须先按命令自身的
 * (ch,ce,lun) 路由到实例，再按 request.op 调服务函数；这些表不能拆成 per-instance。
 *
 * 初始化顺序：ftl_op_init / ftl_op_route_set 必须在所有实例 boot 之前调用
 * （main.c 的 ssd_ftl_boot_all ①）。boot 期的 full metadata rebuild 会抽完成表。
 *
 * 文件结构：
 *   1. 静态资源     g_op_req / g_op_route / g_op_callbacks
 *   2. 初始化       ftl_op_init
 *   3. request 表   add / remove / take / count
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
#include "l2p_cache.h"        /* die_2_ch/ce/lun（die → 坐标的唯一分解点） */
#include "log.h"

#include <string.h>

// request 表按 FC command_id 直接索引；容量 = 命令池深度，不再有“表满”。
// 前置声明：服务函数 / op 属性定义在本文件后面。
static void ftl_sync_wait_callback(ftl_ctx_t *ftl, const fc_command_status_t *completion, void *ctx);
static bool ftl_op_requires_ctx(BYTE op);

/* ── 1. 静态资源 ── */

static ftl_op_req_t g_op_req[FTL_OP_REQ_TABLE_SIZE];

// 实例路由表：按紧凑 die 号 → FTL 实例。命令完成仍带 (ch,ce,lun)，
// 由 chcelun_2_die() 唯一漏斗转换后查表。
static ftl_ctx_t *g_op_route[FTL_DIE_MAX];

// 派发深度（见 ftl_op_is_in_dispatch）。单线程顺序主循环 → 普通变量即可。
static BYTE g_dispatch_depth = 0U;

/* ── 服务函数数组 ──
 *
 * C99 designated initializer 的 const 数组（非"注册函数"）：编译期即可发现重复/
 * 越界，且无 init-order 依赖。每个非 NONE 的 ftl_op_id_e 都必须在这里有实现——
 * 未列出的槽恒为 NULL，派发时判为孤儿（release + LOGE）。
 *
 *   · FTL_OP_SYNC_WAIT：所有 ftl_sync_* 经 ftl_sync_submit_wait 登记 ctx 后走这里；
 *   · FTL_OP_HOST_IO  ：host 数据读写子操作（ctx 不用，服务函数自己 pend_find）；
 *   · FTL_OP_JOURNAL  ：Journal 刷页 job（ctx = ftl->journal）；
 *   · FTL_OP_READ_RECOVERY：Read Recovery job（ctx = &ftl->read_recovery_handle）；
 *   · FTL_OP_GC / FTL_OP_WL：搬移 job（ctx = ftl->mig_handle）——同一台引擎，
 *     tag 分开只为日志/调试可辨（GC/WL 回收 vs 预测性退役退役）；
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

// 全局资源只能清一次：调用顺序由 main.c 的 boot 流程保证（见 ftl_op.h）。
// 放进逐实例 ftl_init 会抹掉其它实例在飞的登记和已设好的路由表。
void ftl_op_init(void)
{
    memset(g_op_req, 0, sizeof(g_op_req));
    memset(g_op_route, 0, sizeof(g_op_route));
}

/* ── 3. request 表 ── */

// command_id 是命令池槽位号，直接索引。每个在飞命令一条表项：op + ctx。
// 槽位已有 pending 项时覆盖并 LOGW：旧项只可能来自未被观测的完成
// （例如 CQ 满时 fc.c 强制置完成），属于异常但可恢复。
ftl_status_e ftl_op_req_add(WORD cmd_id, ftl_op_id_e op, void *ctx)
{
    if ((cmd_id >= FTL_OP_REQ_TABLE_SIZE) || (op <= FTL_OP_NONE) || (op >= FTL_OP_COUNT))
    {
        return FTL_ERROR;
    }

    if ((ctx == NULL) && ftl_op_requires_ctx((BYTE)op))
    {
        return FTL_ERROR;
    }

    if (g_op_req[cmd_id].pending == true)
    {
        LOGW(LOG_TAG_FTL "completion entry cmd=%hu reused — stale request overwritten\r\n", cmd_id);
    }

    g_op_req[cmd_id].ctx     = ctx;
    g_op_req[cmd_id].op      = op;
    g_op_req[cmd_id].pending = true;
    return FTL_OK;
}

// 幂等：未登记直接返回。
void ftl_op_req_remove(WORD cmd_id)
{
    if (cmd_id >= FTL_OP_REQ_TABLE_SIZE)
    {
        return;
    }

    g_op_req[cmd_id].ctx     = NULL;
    g_op_req[cmd_id].op      = FTL_OP_NONE;
    g_op_req[cmd_id].pending = false;
}

// 先清表项再返回，允许服务函数在自己的回调里重入登记/释放表项。
bool ftl_op_req_take(WORD cmd_id, void **ctx_out, ftl_op_id_e *op_out)
{
    ASSERT(ctx_out != NULL);
    ASSERT(op_out != NULL);

    if ((cmd_id >= FTL_OP_REQ_TABLE_SIZE) || (g_op_req[cmd_id].pending == false))
    {
        return false;
    }

    *ctx_out = g_op_req[cmd_id].ctx;
    *op_out  = g_op_req[cmd_id].op;

    g_op_req[cmd_id].ctx     = NULL;
    g_op_req[cmd_id].op      = FTL_OP_NONE;
    g_op_req[cmd_id].pending = false;
    return true;
}

// 所有未决 request（含 host IO）；用于 UT/quiesce 判断全部完成。
WORD ftl_op_req_count(void)
{
    WORD count = 0U;

    for (WORD i = 0U; i < FTL_OP_REQ_TABLE_SIZE; i++)
    {
        if (g_op_req[i].pending == true)
        {
            count++;
        }
    }

    return count;
}

/* ── 4. 实例路由 ── */

// 实现要点：逐格拷贝而非存指针——调用者的数组可能是栈/局部量，逐格拷贝让
// g_op_route 成为唯一持有者，避免悬垂。
void ftl_op_route_set(ftl_ctx_t *const *route)
{
    for (BYTE die = 0U; die < FTL_DIE_MAX; die++)
    {
        g_op_route[die] = (route != NULL) ? route[die] : NULL;
    }
}

// 越界坐标由 chcelun_2_die() 返回 FTL_DIE_INVALID；稀疏空洞同样是 INVALID。
// 两者都按“无路由”返回 NULL，由被调方处理。
// route_set 漏调同样返回 NULL；不在本函数断言，判据留给服务函数入口
// （实例绑定由 ftl_contract_audit 在 boot 期证明）。
// 不用 ftl 的服务函数（SYNC_WAIT / JOURNAL / BBT）收到 NULL 也无需处理。
ftl_ctx_t *ftl_op_route_get(BYTE ch, BYTE ce, BYTE lun)
{
    const BYTE die = chcelun_2_die(ch, ce, lun);

    if ((die == FTL_DIE_INVALID) || (die >= DIE_MAX))
    {
        return NULL;
    }
    return g_op_route[die];
}

// 路由表按 compact die 号索引；调用者保证 die < DIE_MAX，入口 ASSERT 兜底。
ftl_ctx_t *ftl_op_route_get_die(BYTE die)
{
    ASSERT(die < DIE_MAX);
    return g_op_route[die];
}

// 实现要点：全局 region 的 die 坐标 = region % DIE_MAX（与 lba_2_die 同一公式）。
ftl_ctx_t *ftl_op_route_get_region(DWORD global_region)
{
    ASSERT(DIE_MAX != 0U);

    return g_op_route[global_region % DIE_MAX];
}

// 任一实例有 cache 序列未收尾（active 或 draining）；deadline 挂起判据必须把 draining 算进来。
bool ftl_op_cache_seq_active(void)
{
    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        const ftl_ctx_t *ftl = g_op_route[die];

        // draining 也算「序列未结束」：门控在排空期间仍然关着，后台提交同样会被
        // 拒（见 ftl_op_seq_gate），所以 deadline 挂起判据必须把它算进来。
        if ((ftl != NULL) && ((ftl->cache_stream.seq.active == true) || (ftl->cache_stream.seq.draining == true)))
        {
            return true;
        }
    }

    return false;
}

// 除 cache 序列外还要看主机组是否在飞：主机持续提交时 FC 命令池/SQ 被占满是
// 预期（不是卡死）。若每 10 ms 把 Checkpoint / L2P 写回 / 迁移打回去，元数据
// 永远排不上队，进而把写确认拖成 host 写失败（RISK-IF-13 / RISK-HOST-05）。
// 主机空闲后本判据自动为假，真正的卡死仍会被 deadline 暴露。
bool ftl_op_bg_submit_deferred(void)
{
    if (ftl_op_cache_seq_active() == true)
    {
        return true;
    }

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        const ftl_ctx_t *ftl = g_op_route[die];

        if (ftl == NULL)
        {
            continue;
        }

        for (BYTE gi = 0U; gi < FTL_GROUP_MAX; gi++)
        {
            const pend_group_t *g = &ftl->pending.groups[gi];

            if ((g->used == true) && ((g->outstanding != 0U) || (g->next < g->total)))
            {
                return true;
            }
        }
    }

    return false;
}

/* ── 5. 提交 ── */

// 序列门控：cache 序列 `active || draining` 期间，只允许所有者提交该 LUN 的命令；其余返回
// FTL_AGAIN（序列在下一窗口末尾 END，后台/其他 host 命令随后执行）。
// draining = 窗口已提交完但命令仍在飞：门控必须继续关到排空，否则器件侧的 31h/3Fh 管线
// 会被别的提交劈开（RISK-CACHE-14）。
//
// 门控期间的断点请求策略（见 docs/design/ftl_design.md §3.16.5）：
// 前台/同步/恢复类提交被挡住时请求下一窗口 END（D4）；后台元数据只等待、不请求断点。
// 顺序序列占用 plane cache 寄存器期间，Journal/L2P/GC/WL/CP/元数据目录本来就写不进去；
// 若它们每次重试都置 break_requested，序列会被后台任务切回逐窗口长度。
// 设计 A 让它们等在段边界，由 FTL 主动收尾后落盘。
static bool ftl_op_seq_gate(const fc_command_t *cmd, ftl_op_id_e op)
{
    ftl_ctx_t *ftl = ftl_op_route_get(cmd->config.ch, cmd->config.ce, cmd->config.lun);

    if ((ftl != NULL) && ((ftl->cache_stream.seq.active == true) || (ftl->cache_stream.seq.draining == true)) && (ftl->cache_stream.seq.owner_submit == false))
    {
        if ((op == FTL_OP_HOST_IO) || (op == FTL_OP_SYNC_WAIT) || (op == FTL_OP_READ_RECOVERY))
        {
            ftl->cache_stream.seq.break_requested = true;
        }
        return true;
    }

    return false;
}

// 提交唯一入口：先过序列门控；放行后登记 request（op + ctx），最后下发。
ftl_status_e ftl_op_submit(fc_command_t *cmd, ftl_op_id_e op, void *ctx)
{
    if (ftl_op_seq_gate(cmd, op) == true)
    {
        return FTL_AGAIN;
    }

    const WORD cmd_id = ftl_cmd_id(cmd);

    // 登记必须早于提交；每个在飞命令一条 request 表项，ctx == NULL 也登记。
    if (ftl_op_req_add(cmd_id, op, ctx) != FTL_OK)
    {
        return FTL_ERROR; /* id 越界 / 该 op 要求 ctx */
    }

    // 提交失败只回滚登记；命令仍归调用者（瞬态背压，下轮重试）。
    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        ftl_op_req_remove(cmd_id);
        return FTL_AGAIN;
    }

    return FTL_OK;
}

/* ── 批量提交（cache program 批次）── */

// 先整体门控，再逐条登记；任一步失败都回滚已登记项。
// fc_submit_batch 保证提交原子性；QUEUE_FULL 是瞬态，其余是永久错误。
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

        if (ftl_op_req_add(cmd_id, op, ctx) != FTL_OK)
        {
            for (DWORD k = 0U; k < added; k++)
            {
                ftl_op_req_remove(ftl_cmd_id(cmds[k]));
            }
            return FTL_ERROR;
        }
        added++;
    }

    const fc_status_e submit_status = fc_submit_batch(cmds, n);

    if (submit_status != FC_STATUS_OK)
    {
        for (DWORD i = 0U; i < n; i++)
        {
            ftl_op_req_remove(ftl_cmd_id(cmds[i]));
        }

        // QUEUE_FULL 是瞬态资源（下轮重试）；其余是链契约违反等编程错误，永久失败。
        return (submit_status == FC_STATUS_QUEUE_FULL) ? FTL_AGAIN : FTL_ERROR;
    }

    return FTL_OK;
}

/* ── 6. 派发 / 排空 ── */

// 是否正处在完成派发中（服务函数据此拒绝重入 ftl_service）。
bool ftl_op_is_in_dispatch(void)
{
    return (g_dispatch_depth != 0U);
}

/**
 * @brief 该 op 的 request 是否必须携带私有 ctx。
 *
 * HOST_IO / CACHE_PAD 不用 ctx（服务函数自行 pend_find / 读 cache_stream.seq）；
 * 其余 op（含 SYNC_WAIT 的 waiter）都必须在 request 表里带真实 ctx。
 *
 * @param op op tag。
 * @return true —— 必须带 ctx；
 * @return false —— 允许 ctx == NULL。
 */
static bool ftl_op_requires_ctx(BYTE op)
{
    switch (op)
    {
        case FTL_OP_SYNC_WAIT:
        case FTL_OP_JOURNAL:
        case FTL_OP_READ_RECOVERY:
        case FTL_OP_GC:
        case FTL_OP_WL:
        case FTL_OP_L2P:
        case FTL_OP_CHECKPOINT:
        case FTL_OP_BBT:
        case FTL_OP_METADATA:
            return true;
        default:
            return false;
    }
}

// 完成派发：先从 request 表取 {ctx,op} 并清项，再回调。
// 缺 request = 孤儿/迟到完成（如 sync wait 超时已摘项）：只释放命令并留痕。
void ftl_op_dispatch(const fc_command_status_t *completion)
{
    // command_id 由 NAND 从本命令回填；公共壳已断言 ID 在池内。
    fc_command_t *cmd = fc_command_id_to_command(completion->command_id);

    void       *ctx = NULL;
    ftl_op_id_e op  = FTL_OP_NONE;

    if (ftl_op_req_take(completion->command_id, &ctx, &op) == false)
    {
        LOGE(LOG_TAG_FTL "completion without request cmd=%hu status=0x%02hx — released\r\n", completion->command_id, completion->status);
        (void)fc_command_release(cmd);
        return;
    }

    const BYTE           op_byte = (BYTE)op;
    ftl_op_callback_fn_t fn      = (op_byte < FTL_OP_COUNT) ? g_op_callbacks[op_byte] : NULL;

    if (fn == NULL)
    {
        LOGE(LOG_TAG_FTL "orphan request cmd=%hu op=%hhu status=0x%02hx — released\r\n", completion->command_id, op_byte, completion->status);
        (void)fc_command_release(cmd);
        return;
    }

    // 实例归属按命令自身的 ch/ce/lun 解析（不是"谁在 drain"）：单线程顺序主循环下
    // "实例 A 的 drain 遇到实例 B 的 completion"是必然事件。
    // 不变量：路由表由 main.c 在逐实例 boot 之前设好（见 ssd_ftl_boot_all ①：
    // 先 ftl_op_init 清一次、再 route_set），因为 boot 期 full metadata rebuild 就会经
    // l2p_read_sync 抽完成表。故此处解析出 NULL 只可能是编程错误，
    // 不再是"开机早期"的正常状态。
    ftl_ctx_t *ftl = ftl_op_route_get(cmd->config.ch, cmd->config.ce, cmd->config.lun);

    // 深度计数包住服务函数（含它内部的嵌套派发）：ftl_op_is_in_dispatch() 据此判断
    // "现在不能同步等待"。溢出（255 层嵌套）不可能发生——每层都要消费一条命令。
    g_dispatch_depth++;
    fn(ftl, completion, ctx);
    g_dispatch_depth--;
}

// ftl_op_drain — 消费所有 CQ entry 并派发（ftl_service_pump 的完成泵步）。
//
// 每条 completion 交 ftl_op_dispatch()：用 command_id 取命令与 request 表项，
// 按 request 的 op 查 g_op_callbacks[]，再调对应服务函数。循环持续到 CQ 抽干为止。
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
 * @brief 同步等待的完成服务：把 completion 存入 waiter、置 done = true。
 *
 * 不 release 命令——调用者（ftl_sync_submit_wait）要在 wait 返回后读 cmd 数据
 * （如 ftl_cmd_read_data）才 release，这里释放即悬垂。
 *
 * @param ftl        实例（本函数不用）。
 * @param completion FC 完成状态。
 * @param ctx        `ftl_sync_waiter_t *`；request 表保证非 NULL。
 *
 * @note 超时后的迟到完成不会到这里：超时已把 request 表项摘掉，dispatch 走
 *       “completion without request” 分支，只 release 命令。
 */
static void ftl_sync_wait_callback(ftl_ctx_t *ftl, const fc_command_status_t *completion, void *ctx)
{
    (void)ftl; /* 同步等待与实例无关（waiter 由调用者持栈） */

    ftl_sync_waiter_t *waiter = (ftl_sync_waiter_t *)ctx;

    ASSERT(waiter != NULL);

    waiter->completion = *completion;
    waiter->done       = true;
}

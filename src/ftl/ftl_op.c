/*
 * ftl_op.c — 完成派发面：request 表 + 实例路由 + op 回调 + CQ 排空
 *
 * 职责：
 *   · request 表：cmd_id → {ctx, op}；实例路由：(ch,ce,lun) → ftl_ctx_s；
 *   · 提交：ftl_op_submit / ftl_op_submit_batch（含 cache 序列门控）；
 *   · 派发 / 排空：ftl_op_drain（按 request.op 调服务函数）。
 *
 * 关键机制：
 *   · cmd_id / CQ 是全局单例；完成先按命令自身坐标路由到实例，再按 op 派发；
 *   · 服务函数数组是编译期 const 表，未列出槽恒 NULL（孤儿完成只 release + LOGE）；
 *   · 提交门控：序列 active/draining 只放行 owner；前台 / 同步 / L2P 被挡时请求段边界收尾。
 *
 * 与其他模块：
 *   · 上游：各 ftl_sync_* / ftl_async_* 提交、ftl_service_pump（drain）；
 *   · 下游：fc_interface（CQ / 释放）、各模块 op 回调、lifetime（终身计数接线）。
 */

#include "ftl_op.h"

#include "assertion.h"
#include "cache_batch.h"      /* ftl_cache_seq_gate_blocks（L2P 失败归因） */
#include "fc_interface.h"     /* fc_submit / fc_poll_completion / fc_command_release */
#include "ftl.h"              /* ftl_ctx_s */
#include "ftl_fc_interface.h" /* ftl_cmd_id */
#include "l2p_cache.h"        /* die_2_ch/ce/lun（die → 坐标的唯一分解点） */
#include "lifetime.h"         /* lifetime_note_*（终身计数） */
#include "log.h"
#include "onfi_1_0.h" /* nand_operation_is_page_*（终身计数分类） */
#include "stats.h"
#include "timer.h" /* platform_now_ms（后台让行超时） */

#include <string.h>

#if SSD_STATS_ENABLE
typedef struct FTL_OP_STATISTICS_S
{
    DWORD gate_block;
    DWORD break_request;
    DWORD yield_request;
    DWORD yield_clear;
    DWORD req_add_fail;
    DWORD host_submitted;
    DWORD drain_rounds;
    DWORD drain_completed;
} ftl_op_stats_s;

static ftl_op_stats_s s_stats_ftl_op;

#    define FTL_OP_STATS_LIST(X)                    \
        X(gate_block, STATS_U32, STATS_COUNTER)     \
        X(break_request, STATS_U32, STATS_COUNTER)  \
        X(yield_request, STATS_U32, STATS_COUNTER)  \
        X(yield_clear, STATS_U32, STATS_COUNTER)    \
        X(req_add_fail, STATS_U32, STATS_COUNTER)   \
        X(host_submitted, STATS_U32, STATS_COUNTER) \
        X(drain_rounds, STATS_U32, STATS_COUNTER)   \
        X(drain_completed, STATS_U32, STATS_COUNTER)

#    define FTL_OP_STATS_NAME(field, width, kind) \
        {STATS_NAME_STR(field), STATS_OFFSET(ftl_op_stats_s, field), width, kind},
static const stats_name_s s_stats_ftl_op_names[] = {
    FTL_OP_STATS_LIST(FTL_OP_STATS_NAME)};
#    undef FTL_OP_STATS_NAME
#    define FTL_OP_STATS_CHECK(field, width, kind) STATS_CHECK_FIELD(ftl_op_stats_s, field, width);
FTL_OP_STATS_LIST(FTL_OP_STATS_CHECK)
#    undef FTL_OP_STATS_CHECK
#endif /* SSD_STATS_ENABLE */

/* 静态资源 */

// request 表按 FC cmd_id 直接索引；容量 = 命令池深度，没有“表满”路径。
static ftl_op_req_s g_op_req[FTL_OP_REQ_TABLE_SIZE];

// 实例路由表：按紧凑 die 号 → FTL 实例。命令完成仍带 (ch,ce,lun)，
// 由 chcelun_2_die() 唯一漏斗转换后查表。
static ftl_ctx_s *g_op_route[FTL_DIE_MAX];

// 派发深度（见 ftl_op_is_in_dispatch）。单线程顺序主循环 → 普通变量即可。
static BYTE g_dispatch_depth = 0U;

/* 注册表前向声明：表体放文件最末（数据附录），drain 先用到它（H51）。 */
static ftl_op_callback_fn_t const g_op_callbacks[FTL_OP_COUNT];

/* ── route ── */

/**
 * @brief 绑定 die 路由表
 *
 * 绑定后 g_op_route 成为唯一持有者，避免悬垂。
 *
 * @param route die → 实例表
 */
void ftl_op_route_set(ftl_ctx_s *const *route)
{
    for (BYTE die = 0U; die < FTL_DIE_MAX; die++)
    {
        g_op_route[die] = (route != NULL) ? route[die] : NULL;
    }
}

/**
 * @brief 按 ch/ce/lun 取实例
 *
 * 不用 ftl 的服务函数（SYNC_WAIT / JOURNAL / BBT）收到 NULL 也无需处理。
 *
 * @param ch  目标 ch
 * @param ce  目标 ce
 * @param lun 目标 lun
 * @return 实例；NULL = 未绑定
 */
ftl_ctx_s *ftl_op_route_get(BYTE ch, BYTE ce, BYTE lun)
{
    const BYTE die = chcelun_2_die(ch, ce, lun);

    if (die == FTL_DIE_INVALID)
    {
        return NULL;
    }

    // 有效表项由拓扑扫描按 g_die_count 顺序编号，恒 < DIE_MAX；越界即扫描表被
    // 写坏（编程错误），debug 由 ASSERT 停机。调用路径上不存在运行期分支。
    ASSERT(die < DIE_MAX);

    return g_op_route[die];
}

/**
 * @brief 按全局 region 取 owner 实例
 *
 * die 坐标 = region % DIE_MAX（与 lba_2_die 同一公式）。
 *
 * @param global_region 全局 region
 * @return 实例；NULL = 未绑定
 */
ftl_ctx_s *ftl_op_route_get_region(DWORD global_region)
{
    ASSERT(DIE_MAX != 0U);

    return g_op_route[global_region % DIE_MAX];
}

/**
 * @brief 路由表自检（boot 用）
 *
 * 路由表由本模块私有持有；不对外暴露 die 访问器。
 *
 * @return true 一致；false 不一致
 */
BOOL ftl_op_route_verify(void)
{
    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        const ftl_ctx_s *owner = g_op_route[die];

        if ((owner == NULL) || (topo_2_die(owner->topo) != die))
        {
            LOGE(LOG_TAG_FTL "route/die mismatch die=%hhu\r\n", die);
            return false;
        }
    }

    return true;
}

/* ── ctx ── */

/**
 * @brief 该 op 的 request 是否必须携带私有 ctx。
 *
 * HOST_IO / CACHE_PAD 不用 ctx（服务函数自行 pend_find / 读 cache_stream.seq）；
 * 其余 op（含 SYNC_WAIT 的 waiter）都必须在 request 表里带真实 ctx。
 *
 * @param op op ID；取值已由 req_add 入口 ASSERT 限定在 (NONE, COUNT)，故按枚举
 *           存储宽度传 BYTE。
 * @return true —— 必须带 ctx；
 * @return false —— 允许 ctx == NULL。
 *
 * @note 仅用于 ASSERT，故标 __attribute__((unused))（NDEBUG 下 ASSERT 被裁空，
 *       否则触发 -Wunused-function）。
 */
static BOOL __attribute__((unused)) ftl_op_ctx_is_required(BYTE op)
{
    switch (op)
    {
        case FTL_OP_SYNC_WAIT:
        case FTL_OP_JOURNAL:
        case FTL_OP_READ_RECOVERY:
        case FTL_OP_GC:
        case FTL_OP_WL:
        case FTL_OP_L2P:
        case FTL_OP_L2P_PREFETCH:
        case FTL_OP_CHECKPOINT:
        case FTL_OP_BBT:
        case FTL_OP_METADATA:
            return true;
        default:
            return false;
    }
}

/* ── seq ── */

/**
 * @brief 后台让行达成：清 yield_pending
 *
 * 只认 L2P 提交成功：让行就是为一个被拒的 L2P 加载而要的，别的后台/前台提交
 * （尤其 CACHE_PAD / HOST_IO）可能抢在它前面，不能替它清标志。
 */
static void ftl_op_seq_yield_clear(ftl_ctx_s *ftl, ftl_op_id_e op)
{
    if ((ftl != NULL) && (op == FTL_OP_L2P) && (ftl_cache_seq_yield_pending(&ftl->cache_stream) != false))
    {
        STATS_INC(s_stats_ftl_op.yield_clear);
        ftl_cache_seq_yield_clear(ftl);
    }
}

/**
 * @brief cache 写序列的门控：该命令此刻是否必须被拒。
 *
 * 序列 `active || draining` 期间只放行 owner（`seq.owner_submit`）；其余提交
 * 返回 true，由 ftl_op_submit* 折成 FTL_AGAIN。
 *
 * @note draining = 窗口已提交完但命令仍在飞：门控必须继续关到排空，否则器件侧
 *       的 31h/3Fh 管线会被别的提交劈开。
 * @note 断点请求策略（docs/design/ftl_design.md §3.16.5）：前台/同步/恢复类被挡
 *       时置 break_requested，请求下一窗口 END；后台元数据只等待、不请求断点。
 *       顺序序列占用 plane cache 寄存器期间，Journal/L2P/GC/WL/CP/元数据目录
 *       本来就写不进去；若它们每次重试都请求断点，序列会被后台任务切回逐窗口
 *       长度。设计 A 让它们等在段边界，由 FTL 主动收尾后落盘。
 *
 * @param cmd 待提交命令（用其 ch/ce/lun 定位实例）。
 * @param op  完成服务函数选择器（决定被挡时是否请求断点）。
 * @return true —— 被门控拒绝；
 * @return false —— 放行。
 */
static BOOL ftl_op_seq_gate(const fc_cmd_s *cmd, ftl_op_id_e op)
{
    ftl_ctx_s *ftl = ftl_op_route_get(cmd->ch, cmd->ce, cmd->lun);

    if ((ftl != NULL) && (ftl_cache_seq_gate_blocks(&ftl->cache_stream) != false))
    {
        STATS_INC(s_stats_ftl_op.gate_block);
        // L2P 加载也可能是 host 写确认的依赖：gate 拒绝它却不请求断点，会让
        // confirm park 到 100 ms 后把已编程的写结算成 IO_ERROR。
        // 因此把 FTL_OP_L2P 与前台 HOST_IO / SYNC_WAIT 同等对待：请求段边界收尾。
        // ⚠ FTL_OP_READ_RECOVERY **不能**置 break_requested：它恢复的正是本序列
        // 自己的读组。置断点会触发 close_step → ftl_cache_seq_release 重算
        // draining，把刚放开的门控立刻重新关上，恢复读永远提交不进来。
        if ((op == FTL_OP_HOST_IO) || (op == FTL_OP_SYNC_WAIT) || (op == FTL_OP_L2P))
        {
            STATS_INC(s_stats_ftl_op.break_request);
            ftl_cache_seq_request_break(ftl);
        }

        // L2P 加载是前台的确定依赖（写确认 / GC 搬移）：只请求断点不够，host 连续流
        // 会在下一个窗口立刻重新占住门控，加载反复被拒直到调用者超时。这里再请求
        // 让行，host 暂不开新序列，让 L2P 在本轮拿到一次提交机会。
        if (op == FTL_OP_L2P)
        {
            STATS_INC(s_stats_ftl_op.yield_request);
            ftl_cache_seq_request_yield(ftl, platform_now_ms());
        }
        return true;
    }

    return false;
}

/* ── req ── */

/**
 * @brief 未决 request 数（含 host IO）
 *
 * 用于 UT / quiesce 判断全部完成。
 *
 * @return 未决数
 */
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

/**
 * @brief 按 op 类型统计未决 request 数（诊断：池被吃光时看持有者）
 *
 * @param out 长度 `FTL_OP_COUNT` 的数组；本函数先清零再填各 op 的 pending 数。
 */
void ftl_op_req_counts(WORD out[FTL_OP_COUNT])
{
    ASSERT(out != NULL);

    memset(out, 0, FTL_OP_COUNT * sizeof(out[0]));

    for (WORD i = 0U; i < FTL_OP_REQ_TABLE_SIZE; i++)
    {
        if (g_op_req[i].pending == true)
        {
            const ftl_op_id_e op = g_op_req[i].op;

            if ((op > FTL_OP_NONE) && (op < FTL_OP_COUNT))
            {
                out[op]++;
            }
        }
    }
}

/**
 * @brief 登记一条未决 request（cmd_id → op + ctx）
 *
 * 重复登记不会由调用链产生：命令槽位释放前 request 必已摘除（drain 先清项，
 * 同步超时先 remove）。这里只保留 LOGW + 覆盖旧项作为不变量损坏时的现场，
 * 不加 ASSERT（H2：内部入口默认调用者已保证）。
 *
 * @param cmd_id 命令 id
 * @param op     操作类型
 * @param ctx    服务上下文
 * @return FTL_OK；FTL_ERROR 重复 / 越界
 */
ftl_status_e ftl_op_req_add(WORD cmd_id, ftl_op_id_e op, void *ctx)
{
    // op / ctx 由内部调用者按 op 语义选定，违反即编程错误：debug 停机。release 下
    // 越界的 op 也不会写坏内存 —— dispatch 按 op >= FTL_OP_COUNT 判孤儿并释放命令。
    ASSERT(op > FTL_OP_NONE);
    ASSERT(op < FTL_OP_COUNT);
    ASSERT((ctx != NULL) || (ftl_op_ctx_is_required((BYTE)op) == false));

    // cmd_id 来自 ftl_cmd_id()；池外指针会得到 FC_CMD_ID_INVALID (0xFFFF)。
    // 登记要写 g_op_req[cmd_id]，越界即踩相邻静态表 —— release 下不能只靠 ASSERT
    // 裁空（H14），这里同时保留运行期拒绝。
    ASSERT(cmd_id < FTL_OP_REQ_TABLE_SIZE);
    if (cmd_id >= FTL_OP_REQ_TABLE_SIZE)
    {
        return FTL_ERROR;
    }

    LOGW_IF(g_op_req[cmd_id].pending == true, LOG_TAG_FTL "completion entry cmd=%hu reused — stale request overwritten\r\n", cmd_id);

    g_op_req[cmd_id].ctx     = ctx;
    g_op_req[cmd_id].op      = op;
    g_op_req[cmd_id].pending = true;
    return FTL_OK;
}

/**
 * @brief 摘除一条 request
 *
 * 调用链保证 cmd_id 刚登记成功（提交/批量回滚、同步超时摘项），未登记不会
 * 出现；本函数不加 ASSERT（H2），只清项以保持幂等。
 *
 * @param cmd_id 命令 id
 */
void ftl_op_req_remove(WORD cmd_id)
{
    ASSERT(cmd_id < FTL_OP_REQ_TABLE_SIZE);

    g_op_req[cmd_id].ctx     = NULL;
    g_op_req[cmd_id].op      = FTL_OP_NONE;
    g_op_req[cmd_id].pending = false;
}

/* ── cache ── */

/**
 * @brief 任一实例是否有 cache 序列未收尾（active 或 draining）
 *
 * deadline 挂起判据必须把 draining 算进来。
 *
 * @return true 有未收尾序列
 */
BOOL ftl_op_cache_seq_is_active(void)
{
    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        const ftl_ctx_s *ftl = g_op_route[die];

        // draining 也算「序列未结束」：门控在排空期间仍然关着，后台提交同样会被
        // 拒（见 ftl_op_seq_gate），所以 deadline 挂起判据必须把它算进来。
        if ((ftl != NULL) && (ftl_cache_seq_is_open(&ftl->cache_stream) != false))
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 打开的 cache 序列是否仍有真实进展
 *
 * 只看序列所有者组：仍在发子操作，或有 READ/WRITE 类型的在飞命令。托管给 RR 的项
 * 命令已脱钩，不算进展。
 *
 * @return true 有进展；false 无序列 / 序列只剩等待恢复项
 */
BOOL ftl_op_cache_seq_is_progressing(void)
{
    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        const ftl_ctx_s *ftl = g_op_route[die];

        if ((ftl == NULL) || (ftl_cache_seq_is_open(&ftl->cache_stream) == false))
        {
            continue;
        }

        const WORD gi = ftl_cache_seq_owner_gi(&ftl->cache_stream);

        if (gi >= FTL_GROUP_MAX)
        {
            continue;
        }

        if (pend_group_is_used(&ftl->pending, gi) == false)
        {
            continue;
        }
        if (pend_group_has_remaining(&ftl->pending, gi) != false)
        {
            return true; /* 所有者仍在下发 */
        }
        if ((pend_group_has_op(&ftl->pending, gi, PENDING_OP_READ) == true) ||
            (pend_group_has_op(&ftl->pending, gi, PENDING_OP_WRITE) == true) ||
            (pend_group_has_op(&ftl->pending, gi, PENDING_OP_WRITE_CONFIRM) == true))
        {
            return true; /* 有真实命令/确认在推进：都有确定完成点或 100ms 超时 */
        }

        // 序列正在有序收尾（已请求 padding END / END 已在飞）：等它完成，不算结构性卡死。
        // ⚠ 不看 break_requested：它只是“请求下一窗口收尾”，若请求一直未被兑现，
        // 继续把它当进展会让 deadline 永远被清掉。
        if (ftl_cache_seq_is_closing(&ftl->cache_stream) != false)
        {
            return true;
        }
    }

    return false;
}

/* ── bg ── */

/**
 * @brief 后台提交是否应挂起 deadline
 *
 * 主机空闲后本判据自动为假；真正的卡死仍会被 deadline 暴露。
 *
 * @return true 应挂起
 */
BOOL ftl_op_bg_submit_is_deferred(void)
{
    if (ftl_op_cache_seq_is_active() == true)
    {
        return true;
    }

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        const ftl_ctx_s *ftl = g_op_route[die];

        if (ftl == NULL)
        {
            continue;
        }

        for (BYTE gi = 0U; gi < FTL_GROUP_MAX; gi++)
        {
            if (pend_group_is_busy(&ftl->pending, gi) != false)
            {
                return true;
            }
        }
    }

    return false;
}

/* ── gate ── */

/**
 * @brief 该命令的目标实例当前是否被 cache 序列门控挡住
 *
 * 供 L2P 加载失败归因使用（gate vs 队列满/额度满）。只读，不改门控状态。
 * 日志与统计共用本判据，避免「日志说 submit_backpressure、计数说 gate」的口径分叉。
 *
 * @param cmd 命令（用其 ch/ce/lun 路由到实例）
 * @return true 门控会拒绝该命令；false 不会被门控拒绝
 */
BOOL ftl_op_gate_is_blocking(const fc_cmd_s *cmd)
{
    ASSERT(cmd != NULL);

    const ftl_ctx_s *ftl = ftl_op_route_get(cmd->ch, cmd->ce, cmd->lun);

    return ((ftl != NULL) && (ftl_cache_seq_gate_blocks(&ftl->cache_stream) != false)) ? true : false;
}

/* ── is ── */

/**
 * @brief 当前是否在完成派发中
 *
 * 嵌套派发会在外层服务函数尚未返回时重入同一 job 状态机。
 * 用途：给「服务函数内不得同步等待」一条可判定依据；深度计数（不是布尔）。
 * 已落地：jrn_append 页满且异步刷页起不来时，不在派发上下文同步回退（见 journal.c）。
 *
 * @return true 正在派发
 */
BOOL ftl_op_is_in_dispatch(void)
{
    return (g_dispatch_depth != 0U);
}

/* ── sync ── */

/**
 * @brief 同步等待的完成服务：把 completion 存入 waiter、置 done = true。
 *
 * 不 release 命令——调用者（ftl_sync_submit_wait）要在 wait 返回后读 cmd 数据
 * （如 ftl_cmd_read_data）才 release，这里释放即悬垂。
 *
 * @param ftl        实例（本函数不用）。
 * @param completion FC 完成状态。
 * @param ctx        `ftl_sync_waiter_s *`；request 表保证非 NULL。
 *
 * @note 超时后的迟到完成不会到这里：超时已把 request 表项摘掉，ftl_op_drain 走
 *       “completion without request” 分支，只 release 命令。
 */
static void ftl_sync_wait_op_callback(ftl_ctx_s *ftl, const fc_cmd_status_s *completion, void *ctx)
{
    (void)ftl; /* 同步等待与实例无关（waiter 由调用者持栈） */

    ftl_sync_waiter_s *waiter = (ftl_sync_waiter_s *)ctx;

    ASSERT(waiter != NULL);

    waiter->completion = *completion;
    waiter->done       = true;
}

/* ── stats ── */

#if SSD_STATS_ENABLE
/**
 * @brief 完成派发侧的进展签名（主循环空转判据）
 *
 * = 已消费完成数 + 成功下发的 host 子操作数；只用于比较两次采样是否变化，
 * u32 回绕不影响相等比较。
 *
 * @return 进展签名
 */
DWORD ftl_op_stats_progress(void)
{
    return s_stats_ftl_op.drain_completed + s_stats_ftl_op.host_submitted;
}
#endif /* SSD_STATS_ENABLE */

/* ── lifetime ── */

#if SSD_STATS_PERSIST_ENABLE
/**
 * @brief 本实例的紧凑 die 号；不可用时返回 FTL_DIE_INVALID
 *
 * @param ftl 实例
 * @return die 号
 */
static BYTE op_lifetime_die(const ftl_ctx_s *ftl)
{
    ASSERT(ftl != NULL);
    return topo_2_die(ftl->topo);
}

/**
 * @brief host I/O 成功完成的字节记账（排除内部 page0 屏障写）
 *
 * @param ftl     实例
 * @param pending 已完成的子操作
 * @param write   true = 写，false = 读
 */
void ftl_op_lifetime_note_host_io(const ftl_ctx_s *ftl, const pend_req_s *pending, BOOL write)
{
    ASSERT(ftl != NULL);
    ASSERT(pending != NULL);

    if ((ftl == NULL) || (pending == NULL))
    {
        return;
    }

    // page0 是内部屏障写（缺失 plane 写零），不是 host 负载。
    if (pending->op == PENDING_OP_WRITE_PAGE0)
    {
        return;
    }

    const BYTE inst = op_lifetime_die(ftl);

    if (inst != FTL_DIE_INVALID)
    {
        lifetime_note_host(inst, write, (DWORD)pending->n_cw * FTL_IU_BYTES);
    }
}

/**
 * @brief 一条 FC 命令完成的介质健康记账（ECC / UNC / 编程失败 / 擦除失败）
 *
 * @param ftl        实例
 * @param cmd        已完成的命令（desc 链仍有效）
 * @param completion 完成状态
 */
void ftl_op_lifetime_note_completion(const ftl_ctx_s *ftl, const fc_cmd_s *cmd, const fc_cmd_status_s *completion)
{
    ASSERT(ftl != NULL);
    ASSERT(cmd != NULL);
    ASSERT(completion != NULL);

    if ((ftl == NULL) || (cmd == NULL) || (completion == NULL))
    {
        return;
    }

    const BYTE inst = op_lifetime_die(ftl);

    if (inst == FTL_DIE_INVALID)
    {
        return;
    }

    const BYTE  nand_op = (BYTE)cmd->config.operation;
    const DWORD status  = completion->status;
    const BOOL  failed  = fc_cmd_status_has_error(status);

    if (nand_operation_is_page_read(nand_op) != 0U)
    {
        lifetime_note_read_health(inst, fc_cmd_ecc_max_get(completion->cmd_id), fc_cmd_has_unc_get(completion->cmd_id));
    }
    if ((nand_operation_is_page_write(nand_op) != 0U) && (failed != false))
    {
        lifetime_note_program_fail(inst);
    }
    // 擦除没有 ONFI op flag；FC 完成后落的 operation 只有这两个块擦除码。
    if (((nand_op == OP_ERASE_BLOCK) || (nand_op == OP_ERASE_BLOCK_MULTI_PLANE)) && (failed != false))
    {
        lifetime_note_erase_fail(inst);
    }
}
#endif /* SSD_STATS_PERSIST_ENABLE */

/* ── init ── */

/**
 * @brief 初始化 op 分发表、路由表与 request 索引
 *
 * 设备级只调一次；放进逐实例 ftl_init 会抹掉其它实例在飞的登记。
 */

void ftl_op_init(void)
{
    memset(g_op_req, 0, sizeof(g_op_req));
    memset(g_op_route, 0, sizeof(g_op_route));
#if SSD_STATS_ENABLE
    memset(&s_stats_ftl_op, 0, sizeof(s_stats_ftl_op));
    (void)stats_register("ftl.op", STATS_INST_NONE, &s_stats_ftl_op, s_stats_ftl_op_names, STATS_ENTRY_COUNT(s_stats_ftl_op_names));
#endif
}

/* ── submit ── */

/**
 * @brief 提交一条命令并登记 request
 *
 * 三步固定顺序，任一步失败都不留半登记状态：
 *   ① 序列门控：cache 写序列 active/draining 期间，非 owner 的提交被拒；
 *   ② 登记 request（op + ctx）：按 cmd_id 直接索引，ctx == NULL 也登记；
 *   ③ fc_submit 下发。
 * 返回 FTL_OK 只表示「已提交」，不表示完成；release 归调用者（SYNC_WAIT）
 * 或完成服务函数（异步 job）。
 *
 * 池满 / 完成表满属瞬态背压，返回 FTL_AGAIN 让调用者下轮重试。
 *
 * @param cmd 命令
 * @param op  操作类型
 * @param ctx 服务上下文
 * @return FTL_OK；FTL_AGAIN 背压；FTL_ERROR 编程错误
 */
ftl_status_e ftl_op_submit(fc_cmd_s *cmd, ftl_op_id_e op, void *ctx)
{
    // cmd 由调用者从命令池取得（池外指针 ftl_cmd_id() 会给
    // FC_CMD_ID_INVALID，登记入口的 cmd_id 断言会拦下）。
    ASSERT(cmd != NULL);

    if (ftl_op_seq_gate(cmd, op) == true)
    {
        return FTL_AGAIN;
    }

    /* 只有 read recovery 进 HSQ。L2P region 加载留在 SQ：它最多有 L2P_CACHE_SIZE
     * 个并发，超过 HSQ 深度（16）；且 cache sequence 正在占用 LUN 时，HSQ 命令会被
     * 回推并在 HSQ 里堆积到序列结束，HSQ 满后新加载（含 GC 自己的依赖）提交不进来。 */
    if (op == FTL_OP_READ_RECOVERY)
    {
        cmd->high_priority = 1U;
    }

    const WORD cmd_id = ftl_cmd_id(cmd);

    // ctx == NULL 也登记（HOST_IO / CACHE_PAD 无私有状态）。
    if (ftl_op_req_add(cmd_id, op, ctx) != FTL_OK)
    {
        STATS_INC(s_stats_ftl_op.req_add_fail);
        return FTL_ERROR; /* 池外命令指针（cmd_id 越界） */
    }

    // 回滚登记，命令仍归调用者。
    if (fc_submit(cmd->cmd_id) != FC_STATUS_OK)
    {
        ftl_op_req_remove(cmd_id);
        return FTL_AGAIN;
    }

    if (op == FTL_OP_HOST_IO)
    {
        STATS_INC(s_stats_ftl_op.host_submitted);
    }
    ftl_op_seq_yield_clear(ftl_op_route_get(cmd->ch, cmd->ce, cmd->lun), op);
    return FTL_OK;
}

/* ── 批量提交（cache program 批次）── */

/**
 * @brief 批量提交同 op 的命令链
 *
 * 与 ftl_op_submit 相同的顺序，但两段都按整批处理：先对全部命令过序列门控，
 * 再逐条登记 request，最后用 fc_submit_batch 原子入队。登记或入队失败都回滚
 * 已登记项；门控失败发生在登记之前，无需回滚。
 * 链契约违反等编程错误属永久失败。
 *
 * @param cmds 命令数组
 * @param n    条数
 * @param op   操作类型
 * @param ctx  服务上下文
 * @return FTL_OK；FTL_AGAIN 背压；FTL_ERROR 编程错误
 */
ftl_status_e ftl_op_submit_batch(fc_cmd_s *const cmds[], DWORD n, ftl_op_id_e op, void *ctx)
{
    ASSERT(cmds != NULL);
    ASSERT(n > 0U);
    ASSERT(n <= FC_COMMAND_POOL_DEPTH);

    fc_cmd_id_t cmd_ids[FC_COMMAND_POOL_DEPTH];

    // 逐个命令都是调用者已配置好的池内命令；先在本函数入口断言，避免
    // seq_gate 先解引用空元素。fc_submit_batch 的同类断言在其后，来不及兜。
    for (DWORD i = 0U; i < n; i++)
    {
        ASSERT(cmds[i] != NULL);
        cmd_ids[i] = cmds[i]->cmd_id;

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

    const fc_status_e submit_status = fc_submit_batch(cmd_ids, n);

    if (submit_status != FC_STATUS_OK)
    {
        for (DWORD i = 0U; i < n; i++)
        {
            ftl_op_req_remove(ftl_cmd_id(cmds[i]));
        }

        // QUEUE_FULL 是瞬态资源（下轮重试）；其余是链契约违反等编程错误，永久失败。
        return (submit_status == FC_STATUS_QUEUE_FULL) ? FTL_AGAIN : FTL_ERROR;
    }

    ftl_op_seq_yield_clear(ftl_op_route_get(cmds[0]->ch, cmds[0]->ce, cmds[0]->lun), op);
    return FTL_OK;
}

/* ── drain ── */

/**
 * @brief 从 CQ 取完成并按 request 分派
 *
 * 循环 fc_poll_completion() 到 CQ 抽干为止；每条完成：
 *   ① 按 cmd_id 取回命令与 request 表项（先清表项再回调，容许服务函数重入
 *      登记/释放）；
 *   ② 按 op 查 g_op_callbacks[]；
 *   ③ 按命令自身的 ch/ce/lun 解析归属实例，调服务函数。
 * 缺 request（迟到完成）或未知 op（孤儿）只 release 命令并 LOGE。
 * 循环不能只看进入时的 CQ 深度：服务函数可能在本轮里提交新命令并立即完成。
 */
void ftl_op_drain(void)
{
    fc_cmd_status_s completion;

    STATS_INC(s_stats_ftl_op.drain_rounds);

    while (fc_poll_completion(&completion) == FC_STATUS_OK)
    {
        STATS_INC(s_stats_ftl_op.drain_completed);
        // cmd_id 由 NAND 从本命令回填；公共壳已断言 ID 在池内。
        fc_cmd_s *cmd = fc_command_lookup(completion.cmd_id);

        // 与上面同一个 id 索引 request 表（容量 = 命令池深度）。
        ftl_op_req_s *req = &g_op_req[completion.cmd_id];

        // 先取 {ctx,op} 并清项，再回调：服务函数可以在自己的回调里重入登记/释放
        // 表项。未登记 = 孤儿/迟到完成（如 sync wait 超时已摘项），只释放命令并留痕。
        if (req->pending == false)
        {
            LOGE(LOG_TAG_FTL "completion without request cmd=%hu status=0x%02hx — released\r\n", completion.cmd_id, completion.status);
            fc_command_release(cmd->cmd_id);
            continue;
        }

        void       *ctx = req->ctx;
        ftl_op_id_e op  = req->op;

        req->ctx     = NULL;
        req->op      = FTL_OP_NONE;
        req->pending = false;

        const BYTE           op_byte = (BYTE)op;
        ftl_op_callback_fn_t fn      = (op_byte < FTL_OP_COUNT) ? g_op_callbacks[op_byte] : NULL;

        if (fn == NULL)
        {
            LOGE(LOG_TAG_FTL "orphan request cmd=%hu op=%hhu status=0x%02hx — released\r\n", completion.cmd_id, op_byte, completion.status);
            fc_command_release(cmd->cmd_id);
            continue;
        }

        // 实例归属按命令自身的 ch/ce/lun 解析（不是"谁在 drain"）：单线程顺序主循环下
        // "实例 A 的 drain 遇到实例 B 的 completion"是必然事件。
        // 路由表由 main.c 在逐实例 boot 之前设好（见 ssd_ftl_boot_all ①：先
        // ftl_op_init 清一次、再 route_set），boot 期 full metadata rebuild 会经
        // l2p_read_sync 抽完成表；故此处解析出 NULL 只可能是编程错误。
        ftl_ctx_s *ftl = ftl_op_route_get(cmd->ch, cmd->ce, cmd->lun);

#if SSD_STATS_PERSIST_ENABLE
        ftl_op_lifetime_note_completion(ftl, cmd, &completion);
#endif

        // 深度计数包住服务函数（含它内部的嵌套派发）：ftl_op_is_in_dispatch() 据此判断
        // "现在不能同步等待"。溢出（255 层嵌套）不可能发生——每层都要消费一条命令。
        g_dispatch_depth++;
        fn(ftl, &completion, ctx);
        g_dispatch_depth--;

        // 所有 FC 完成进入 scheduler 的唯一钩子：回收 host/L2P item，并唤醒 MD_SLOT。
        ftl_sched_note_completion(ftl);
    }
}

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
 *     op ID 分开只为日志/调试可辨（GC/WL 回收 vs 预测性退役）；
 *   · FTL_OP_L2P      ：L2P region 加载 / 写回（ctx = 该 cache line）；
 *   · FTL_OP_L2P_PREFETCH：L2P region 预取加载（ctx = 该 cache line；回调与 L2P 相同，
 *     但 op 独立 ⇒ 不请求 cache 序列断点/让行）；
 *   · FTL_OP_CHECKPOINT：Checkpoint job（ctx = ftl->cp_handle）；
 *   · FTL_OP_BBT      ：元数据表持久化 job（ctx = &ftl->alloc）；
 *   · FTL_OP_METADATA ：L2P 日志块整理 job（ctx = ftl->metadata_dir->handle）；
 *   · FTL_OP_CACHE_PAD：cache 写序列的 padding END（ctx = NULL）——用一条不映射的
 *     零页给提前回执的最后一页提供 FAIL_CACHE 结算点，然后关闭序列。 */

static ftl_op_callback_fn_t const g_op_callbacks[FTL_OP_COUNT] = {
    [FTL_OP_SYNC_WAIT]     = ftl_sync_wait_op_callback,
    [FTL_OP_HOST_IO]       = ftl_host_io_op_callback,
    [FTL_OP_JOURNAL]       = ftl_journal_op_callback,
    [FTL_OP_READ_RECOVERY] = ftl_read_recovery_op_callback,
    [FTL_OP_GC]            = ftl_migrate_op_callback,
    [FTL_OP_WL]            = ftl_migrate_op_callback,
    [FTL_OP_L2P]           = ftl_l2p_op_callback,
    [FTL_OP_L2P_PREFETCH]  = ftl_l2p_op_callback,
    [FTL_OP_CHECKPOINT]    = ftl_checkpoint_op_callback,
    [FTL_OP_BBT]           = ftl_bbt_op_callback,
    [FTL_OP_METADATA]      = ftl_metadata_op_callback,
    [FTL_OP_CACHE_PAD]     = ftl_cache_pad_op_callback,
};

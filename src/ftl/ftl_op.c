/*
 * ftl_op.c — 完成派发面：request 表 + 实例路由 + op 回调 + CQ 排空
 *
 * 模块职责（全局单例，非 per-instance）：
 *   ① request 表    command_id 映射到 {ctx, op}；
 *   ② 实例路由表    (ch,ce,lun) 经 compact die 映射到 ftl_ctx_s；
 *   ③ 服务函数数组  request.op 映射到完成服务函数；
 *   ④ 提交          ftl_op_submit / ftl_op_submit_batch；
 *   ⑤ 派发/排空     ftl_op_drain（每条完成的派发在循环体内）。
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
 *   3. request 表   requires_ctx / add / remove / count
 *   4. 实例路由     set / get / verify / get_region / cache_seq_active /
 *                   bg_submit_deferred
 *   5. 提交         seq_gate / submit / submit_batch
 *   6. 派发/排空    is_in_dispatch / drain
 *   7. 同步等待服务 ftl_sync_wait_callback
 */

#include "ftl_op.h"

#include "assertion.h"
#include "fc.h"               /* fc_submit / fc_poll_completion / fc_command_release */
#include "ftl.h"              /* ftl_ctx_s */
#include "ftl_fc_interface.h" /* ftl_cmd_id */
#include "l2p_cache.h"        /* die_2_ch/ce/lun（die → 坐标的唯一分解点） */
#include "log.h"

#include <string.h>

// 服务函数定义在本文件末尾；g_op_callbacks 初始化时必须先见到它。
static void ftl_sync_wait_callback(ftl_ctx_s *ftl, const fc_cmd_status_s *completion, void *ctx);

/* ── 1. 静态资源 ── */

// request 表按 FC command_id 直接索引；容量 = 命令池深度，没有“表满”路径。
static ftl_op_req_s g_op_req[FTL_OP_REQ_TABLE_SIZE];

// 实例路由表：按紧凑 die 号 → FTL 实例。命令完成仍带 (ch,ce,lun)，
// 由 chcelun_2_die() 唯一漏斗转换后查表。
static ftl_ctx_s *g_op_route[FTL_DIE_MAX];

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
 *     op ID 分开只为日志/调试可辨（GC/WL 回收 vs 预测性退役）；
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

/**
 * @brief 初始化 op 分发表、路由表与 request 索引
 *
 * 设备级只调一次；放进逐实例 ftl_init 会抹掉其它实例在飞的登记。
 */
void ftl_op_init(void)
{
    memset(g_op_req, 0, sizeof(g_op_req));
    memset(g_op_route, 0, sizeof(g_op_route));
}

/* ── 3. request 表 ── */

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
static BOOL __attribute__((unused)) ftl_op_requires_ctx(BYTE op)
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
    ASSERT((ctx != NULL) || (ftl_op_requires_ctx((BYTE)op) == false));

    // cmd_id 来自 ftl_cmd_id()；池外指针会得到 FC_CMD_ID_INVALID (0xFFFF)。
    // 登记要写 g_op_req[cmd_id]，越界即踩相邻静态表 —— release 下不能只靠 ASSERT
    // 裁空（H14），这里同时保留运行期拒绝。
    ASSERT(cmd_id < FTL_OP_REQ_TABLE_SIZE);
    if (cmd_id >= FTL_OP_REQ_TABLE_SIZE)
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

/* ── 4. 实例路由 ── */

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
 * @brief 任一实例是否有 cache 序列未收尾（active 或 draining）
 *
 * deadline 挂起判据必须把 draining 算进来。
 *
 * @return true 有未收尾序列
 */
BOOL ftl_op_cache_seq_active(void)
{
    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        const ftl_ctx_s *ftl = g_op_route[die];

        // draining 也算「序列未结束」：门控在排空期间仍然关着，后台提交同样会被
        // 拒（见 ftl_op_seq_gate），所以 deadline 挂起判据必须把它算进来。
        if ((ftl != NULL) && ((ftl->cache_stream.seq.active == true) || (ftl->cache_stream.seq.draining == true)))
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 后台提交是否应挂起 deadline
 *
 * 主机空闲后本判据自动为假；真正的卡死仍会被 deadline 暴露。
 *
 * @return true 应挂起
 */
BOOL ftl_op_bg_submit_deferred(void)
{
    if (ftl_op_cache_seq_active() == true)
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
            const pend_group_s *g = &ftl->pending.groups[gi];

            if ((g->used == true) && ((g->outstanding != 0U) || (g->next < g->total)))
            {
                return true;
            }
        }
    }

    return false;
}

/* ── 5. 提交 ── */

/**
 * @brief cache 写序列的门控：该命令此刻是否必须被拒。
 *
 * 序列 `active || draining` 期间只放行 owner（`seq.owner_submit`）；其余提交
 * 返回 true，由 ftl_op_submit* 折成 FTL_AGAIN。
 *
 * @note draining = 窗口已提交完但命令仍在飞：门控必须继续关到排空，否则器件侧
 *       的 31h/3Fh 管线会被别的提交劈开（RISK-CACHE-14）。
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
    ftl_ctx_s *ftl = ftl_op_route_get(cmd->route.ch, cmd->route.ce, cmd->route.lun);

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

/**
 * @brief 提交一条命令并登记 request
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
    // cmd 由调用者从 fc_command_alloc() 取得（池外指针 ftl_cmd_id() 会给
    // FC_CMD_ID_INVALID，登记入口的 cmd_id 断言会拦下）。
    ASSERT(cmd != NULL);

    if (ftl_op_seq_gate(cmd, op) == true)
    {
        return FTL_AGAIN;
    }

    const WORD cmd_id = ftl_cmd_id(cmd);

    // ctx == NULL 也登记（HOST_IO / CACHE_PAD 无私有状态）。
    if (ftl_op_req_add(cmd_id, op, ctx) != FTL_OK)
    {
        return FTL_ERROR; /* 池外命令指针（cmd_id 越界） */
    }

    // 回滚登记，命令仍归调用者。
    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        ftl_op_req_remove(cmd_id);
        return FTL_AGAIN;
    }

    return FTL_OK;
}

/* ── 批量提交（cache program 批次）── */

/**
 * @brief 批量提交同 op 的命令链
 *
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

    // 逐个命令都是调用者已配置好的池内命令；先在本函数入口断言，避免
    // seq_gate 先解引用空元素。fc_submit_batch 的同类断言在其后，来不及兜。
    for (DWORD i = 0U; i < n; i++)
    {
        ASSERT(cmds[i] != NULL);

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

/**
 * @brief 当前是否在完成派发中
 *
 * 嵌套派发会在外层服务函数尚未返回时重入同一 job 状态机。
 *
 * @return true 正在派发
 */
BOOL ftl_op_is_in_dispatch(void)
{
    return (g_dispatch_depth != 0U);
}

/**
 * @brief 从 CQ 取完成并按 request 分派
 *
 * 每轮调用；深度会漏掉这些完成。
 */
void ftl_op_drain(void)
{
    fc_cmd_status_s completion;

    while (fc_poll_completion(&completion) == FC_STATUS_OK)
    {
        // command_id 由 NAND 从本命令回填；公共壳已断言 ID 在池内。
        fc_cmd_s *cmd = fc_command_id_to_command(completion.command_id);

        // 与上面同一个 id 索引 request 表（容量 = 命令池深度）。
        ftl_op_req_s *req = &g_op_req[completion.command_id];

        // 先取 {ctx,op} 并清项，再回调：服务函数可以在自己的回调里重入登记/释放
        // 表项。未登记 = 孤儿/迟到完成（如 sync wait 超时已摘项），只释放命令并留痕。
        if (req->pending == false)
        {
            LOGE(LOG_TAG_FTL "completion without request cmd=%hu status=0x%02hx — released\r\n", completion.command_id, completion.status);
            (void)fc_command_release(cmd);
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
            LOGE(LOG_TAG_FTL "orphan request cmd=%hu op=%hhu status=0x%02hx — released\r\n", completion.command_id, op_byte, completion.status);
            (void)fc_command_release(cmd);
            continue;
        }

        // 实例归属按命令自身的 ch/ce/lun 解析（不是"谁在 drain"）：单线程顺序主循环下
        // "实例 A 的 drain 遇到实例 B 的 completion"是必然事件。
        // 路由表由 main.c 在逐实例 boot 之前设好（见 ssd_ftl_boot_all ①：先
        // ftl_op_init 清一次、再 route_set），boot 期 full metadata rebuild 会经
        // l2p_read_sync 抽完成表；故此处解析出 NULL 只可能是编程错误。
        ftl_ctx_s *ftl = ftl_op_route_get(cmd->route.ch, cmd->route.ce, cmd->route.lun);

        // 深度计数包住服务函数（含它内部的嵌套派发）：ftl_op_is_in_dispatch() 据此判断
        // "现在不能同步等待"。溢出（255 层嵌套）不可能发生——每层都要消费一条命令。
        g_dispatch_depth++;
        fn(ftl, &completion, ctx);
        g_dispatch_depth--;
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
 * @param ctx        `ftl_sync_waiter_s *`；request 表保证非 NULL。
 *
 * @note 超时后的迟到完成不会到这里：超时已把 request 表项摘掉，ftl_op_drain 走
 *       “completion without request” 分支，只 release 命令。
 */
static void ftl_sync_wait_callback(ftl_ctx_s *ftl, const fc_cmd_status_s *completion, void *ctx)
{
    (void)ftl; /* 同步等待与实例无关（waiter 由调用者持栈） */

    ftl_sync_waiter_s *waiter = (ftl_sync_waiter_s *)ctx;

    ASSERT(waiter != NULL);

    waiter->completion = *completion;
    waiter->done       = true;
}

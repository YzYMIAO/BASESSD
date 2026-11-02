/*
 * pending.c — Pending Request 表 + 完成组（pend_group_t）实现
 *
 * 职责：
 *   ① 跟踪异步请求的 command_id → 请求映射，供 FC 完成回调据以做 caller 通知
 *      与写回滚簿记（ftl_host_io_callback → pend_find）；
 *   ② 实现「完成组」——把一条 host 请求拆出的多条 FC 子操作聚成一个完成单元，
 *      通知字段（complete_flag/status_out/on_complete）只在最后一个子操作完成后
 *      触发一次（见 pend_group_* 族）。
 *
 * 与 pending.h 的分工：
 *   pending.h —— 表结构（pend_req_t / pend_group_t）+ 容量宏 + 语义约定；
 *   本文件     —— 两张表的增删查 + 组完成编排，无新数据结构。
 *
 * 存储：pending[] / groups[] 由 pend_table_t（per-instance）持有；文件级 static
 *   只放进程级 handler 与日志限流时间戳，不持有表数据。
 *
 * 文件结构：
 *   1. pending 表基础操作   init / alloc / find / remove / find_op / view_op / release_index / count
 *   2. 完成组操作           alloc / count / add·drop_subop / complete_subop[_multi] /
 *                           maybe_complete / has_window_pending
 *   3. write-confirm 停泊   set_handler / park / service
 *
 * ⚠ 读写判别必须**显式携带** op（pend_op_e），不可从 buffer 字段反推——
 *   写请求的 buffer 是数据源、非 NULL ⇒ `buffer != NULL` 对写恒真（见 pending.h 文件头）。
 */

#include "pending.h"

#include "assertion.h"
#include "ftl_job.h"    /* ftl_job_now_ms（confirm 停泊超时） */
#include "ftl_module.h" /* FTL_GROUP_DRAIN_TIMEOUT_MS */
#include "log.h"

#include <string.h>

/* ── 文件级静态数据 ── */

// write-confirm 事务 handler（ftl_init 注册一次，之后只读调用）。
static pending_confirm_handler_t s_confirm_handler;

// 停泊超时日志限流（单线程 superloop；多实例共享一个时间戳即可）。
static DWORD s_park_log_ms;

// pend_init — 清空本实例的 pending 表与完成组表
//
// 全零即「无占用」（used==0），开机/重建时调用。表是 per-instance 的：每个 FTL
// 实例各有一份 pending/groups（多 LUN 下完成按命令的 ch/ce/lun 路由回本实例）。
void pend_init(pend_table_t *table)
{
    memset(table->slots, 0, sizeof(table->slots));
    memset(table->groups, 0, sizeof(table->groups));
}

// 实现要点：表满属预期内的资源结果，保留返回码（判据 3）。
// ⚠ **不存 old_pba**：完成时按 LBA 重查 L2P（见 pend_req_t 注释）。
// ⚠ 认领即置 used=1，调用者填完须调 pend_validate()（pending.h）。
pend_req_t *pend_alloc(pend_table_t *table)
{
    for (BYTE i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if (table->slots[i].used == false)
        {
            memset(&table->slots[i], 0, sizeof(table->slots[i]));
            table->slots[i].used = true;
            return &table->slots[i];
        }
    }

    return NULL; /* 表满 */
}

// pend_find — 按 command_id 查本实例 pending 项；未找到返回 NULL（线性扫描，表深仅 32）
pend_req_t *pend_find(pend_table_t *table, WORD cmd_id)
{
    for (BYTE i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if ((table->slots[i].used == true) && (table->slots[i].command_id == cmd_id))
        {
            return &table->slots[i];
        }
    }

    return NULL;
}

// pend_remove — 置 used=0 释放槽位；未找到则 no-op（幂等，供完成路径重复调用）
void pend_remove(pend_table_t *table, WORD cmd_id)
{
    pend_req_t *req = pend_find(table, cmd_id);

    if (req != NULL)
    {
        req->used = false;
    }
}

// 实现要点：供 Read Recovery 取走被托管的子操作——那些项的 command_id 已置
// FTL_PENDING_CMD_NONE，pend_find 查不到，只能按 op 找。
// 按下标升序 = 先进先出，与「托管顺序」一致（append-only，无移动）。
bool pend_find_op(const pend_table_t *table, pend_op_e op, BYTE *out_index)
{
    for (BYTE i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if ((table->slots[i].used == true) && (table->slots[i].op == op))
        {
            if (out_index != NULL)
            {
                *out_index = i;
            }
            return true;
        }
    }

    return false;
}

// 按 op 查槽，把调用者需要的字段拷进出参；只读，不改槽状态。
bool pend_view_op(const pend_table_t *table, pend_op_e op, pend_op_view_t *out)
{
    BYTE idx = 0U;

    if (pend_find_op(table, op, &idx) == false)
    {
        return false;
    }

    const pend_req_t *slot = &table->slots[idx];

    out->index  = idx;
    out->owners = slot->owners;
    out->buffer = slot->buffer;
    out->lba    = slot->lba;
    out->n_cw   = slot->n_cw;
    return true;
}

// 实现要点：与 pend_remove 的差别只在寻址方式——托管项脱钩后无 command_id 可查，
// 收尾只能按下标。⚠ 下标由 pend_find_op 产出且表无移动 ⇒ 恒有效，
// 故越界一律 ASSERT，不静默 no-op。
void pend_release_index(pend_table_t *table, BYTE index)
{
    ASSERT(table->slots[index].used == true); /* 重复释放即簿记错误，留现场 */

    table->slots[index].used = false;
}

// 是否存在不属于 cache 序列的写在飞（cache_role == NONE）：有则本 LBA 流不能再按序列续。
bool pend_has_foreign_write(const pend_table_t *table)
{
    for (BYTE i = 0U; i < FTL_PENDING_MAX; i++)
    {
        const pend_req_t *p = &table->slots[i];

        if ((p->used == false) || (p->cache_role != PEND_CACHE_ROLE_NONE))
        {
            continue;
        }
        if ((p->op == PENDING_OP_WRITE) || (p->op == PENDING_OP_WRITE_PAGE0) || (p->op == PENDING_OP_WRITE_CONFIRM))
        {
            return true;
        }
    }

    return false;
}

// pend_slot_get_count — 本实例占用槽位数（ftl_service 档位判定用它近似 host IO 空闲度）
WORD pend_slot_get_count(const pend_table_t *table)
{
    WORD count = 0U;

    for (BYTE i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if (table->slots[i].used == true)
        {
            count++;
        }
    }

    return count;
}

/* ── 完成组（pend_group_t）── */

// 实现要点：组容量 = FTL_GROUP_MAX；清零保证 total/next/outstanding/agg_status
// 从已知态起步。状态与数据分离（H38）：失败不得用非负的「组号」冒充。
bool pend_group_alloc(pend_table_t *table, BYTE *out_gi)
{
    for (BYTE i = 0U; i < FTL_GROUP_MAX; i++)
    {
        if (table->groups[i].used == false)
        {
            memset(&table->groups[i], 0, sizeof(table->groups[i]));
            table->groups[i].used = true;
            *out_gi               = i;
            return true;
        }
    }

    return false; /* 组槽满 */
}

// pend_group_get_count — 占用（未完成）组数（有未完成组 = host 工作饿着 → 归 BUSY）
WORD pend_group_get_count(const pend_table_t *table)
{
    WORD count = 0U;

    for (BYTE i = 0U; i < FTL_GROUP_MAX; i++)
    {
        if (table->groups[i].used == true)
        {
            count++;
        }
    }

    return count;
}

// 实现要点：outstanding 是「已下发、未完成」的组不变量。写点分散在 host 读
// （ftl_issue_subop）与页打包写（wb_handle(APPEND)）两处、递减点分散在
// 完成服务与提交失败回滚——任一处漏掉即 host 写组永不完成、挂死。
// 故所有增减都收敛到本对函数；完整完成走
// pend_group_complete_subop*（带聚合状态与完成通知），
// 提交失败回滚走 drop（不触发完成通知）。
void pend_group_add_subop(pend_table_t *table, WORD gi)
{
    pend_group_t *g = &table->groups[gi];
    ASSERT(g->used == true);

    g->outstanding++;
}

// 撤销一个尚未提交的子操作：只把组在飞数 -1，不触发完成判定（由调用者决定何时结算）。
void pend_group_drop_subop(pend_table_t *table, WORD gi)
{
    pend_group_t *g = &table->groups[gi];
    ASSERT(g->used == true);
    ASSERT(g->outstanding > 0U);

    g->outstanding--;
}

// 实现要点：· caller 通知字段（complete_flag / status_out / on_complete）的**唯一写点**：
// 由 ftl_host_io_callback 在子操作簿记后调用。
// · agg_status **粘性**：仅当 sub_status != OK 时写入，一旦失败即保留首个错误，
// 后续成功不覆盖（组整体语义 = 任一子操作失败则整组 IO_ERROR）。
// · outstanding-- 后交 pend_group_maybe_complete 判是否整组完成。
void pend_group_complete_subop(pend_table_t *table, WORD gi, ssd_status_e sub_status)
{
    pend_group_t *g = &table->groups[gi];
    ASSERT(g->used == true); /* 组已释放 = 重复完成/簿记错误，debug 当场炸 */

    if (sub_status != SSD_STATUS_OK)
    {
        g->agg_status = sub_status; /* 粘性：一旦失败即保留 */
    }
    if (g->outstanding > 0U)
    {
        g->outstanding--;
    }

    pend_group_maybe_complete(table, gi);
}

// 实现要点：owners 按值传入即局部副本，遍历它安全——回调（on_complete）可能重入
// 复用组槽，但本循环只认传入的位掩码，不受槽位变动影响。
void pend_group_complete_subop_multi(pend_table_t *table, DWORD owners, ssd_status_e sub_status)
{
    for (WORD gi = 0U; gi < FTL_GROUP_MAX; gi++)
    {
        if ((owners & ftl_group_bit(gi)) != 0U)
        {
            pend_group_complete_subop(table, gi, sub_status);
        }
    }
}

// 实现要点：· 完成条件：used 且 next==total（子操作已全部下发）且 outstanding==0
// （已下发的都已完成）。任一不满足即直接返回——这是**负例守卫**（判据 8），
// 保证通知绝不早发。
// · 通知顺序：写 status_out → 置 complete_flag → 调 on_complete。
// · ⚠ used=0 必须在**通知之后**：on_complete 回调可能重入 ftl_submit_*
// 再申请组槽，若先清零则本槽可能被该重入立刻复用、又被这里清零。
void pend_group_maybe_complete(pend_table_t *table, WORD gi)
{
    pend_group_t *g = &table->groups[gi];
    if ((g->used == false) || (g->next < g->total) || (g->outstanding != 0U))
    {
        return;
    }

    if (g->status_out != NULL)
    {
        *g->status_out = g->agg_status;
    }
    if (g->complete_flag != NULL)
    {
        *g->complete_flag = 1U;
    }
    if (g->on_complete != NULL)
    {
        g->on_complete(g->on_complete_ctx);
    }

    // 回调可能重入 ftl_submit_* 再申请组槽 ⇒ used 清零必须在通知之后
    g->used = false;
}

// 实现要点：只读扫描 groups[]，数组本身不暴露给 wbuf。
bool pend_group_has_window_pending(const pend_table_t *table, DWORD window)
{
    for (BYTE i = 0U; i < FTL_GROUP_MAX; i++)
    {
        const pend_group_t *g = &table->groups[i];

        if ((g->used == false) || (pend_op_is_write(g->op) == false) || (g->next >= g->total))
        {
            continue;
        }
        if (ftl_lba_pair_base(g->lba + g->next) == window)
        {
            return true;
        }
    }

    return false;
}

/* ── write-confirm park（停泊列表 = pending 表里的 WRITE_CONFIRM 项）──
 *
 * 不另建一张列表：停泊项本来就在 pending 表里（op = WRITE_CONFIRM、command_id
 * 已脱钩为哨兵），另建列表会变成第二套坐标（C9）。pending 模块负责扫描/超时/结算，
 * 数据面事务由 FTL 注入的 handler 执行（C2/R9：不复制 Journal→L2P→BST 顺序）。 */

// 注册写确认事务 handler（ftl_init 一次性），之后只读调用。
void pend_confirm_set_handler(pending_confirm_handler_t handler)
{
    s_confirm_handler = handler;
}

pending_confirm_handler_t pend_confirm_get_handler(void)
{
    return s_confirm_handler;
}

// 把「已编程、确认未完成」的子操作转成停泊项（契约见 include/ftl/pending.h）。
// 槽本身由写路径持有，这里只改 op / owners 两个字段；release 由结算侧（成功、
// 失败或超时）唯一负责 —— 两侧都释放就是重复归还（C12）。
void pend_confirm_park(pend_table_t *table, WORD pending_idx, DWORD owners)
{
    pend_req_t *pending = &table->slots[pending_idx];

    ASSERT(pending->used == true);

    pending->op         = PENDING_OP_WRITE_CONFIRM;
    pending->command_id = FTL_PENDING_CMD_NONE;
    pending->owners     = owners;
    pending->park_ms    = ftl_job_now_ms();
}

/**
 * @brief 槽是否是「已编程、确认未完成」的停泊项。
 */
static bool pend_is_confirm_parked(const pend_req_t *p)
{
    return (p->used == true) && pend_op_is_write_confirm(p->op);
}

// 写确认结算（[svc] 步骤之一，调用点见 ftl_service）。
// 契约（时机 / 入参）见 include/ftl/pending.h；此处只记实现要点：
//   · 逐条调确认 handler：FTL_AGAIN = region 仍在加载 ⇒ 停泊保留、下轮再来；
//     停泊超过 FTL_GROUP_DRAIN_TIMEOUT_MS（100 ms）按 IO_ERROR 结算，避免元数据区
//     卡死时 host 写永久挂起（它只兜「确认停泊」，兜不住别的等待）。
//   · 结算顺序：先取走 owners、再释放槽——组完成通知可能重入复用本槽。
//   · 只有真正结算（成功或失败）才释放槽并触发组完成；FTL_AGAIN 不动任何状态。
void pend_confirm_service(pend_table_t *table, void *confirm_ctx)
{
    const DWORD now_ms = ftl_job_now_ms();

    // 本轮进入时的停泊总数。限流日志必须报「同时有多少条在等」——只打扫描到的
    // 第一条（常是 idx=0）会被误读成「只有一个 region 卡住」（RISK-HOST-05）。
    WORD parked = 0U;

    for (BYTE i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if (pend_is_confirm_parked(&table->slots[i]))
        {
            parked++;
        }
    }

    for (BYTE i = 0U; i < FTL_PENDING_MAX; i++)
    {
        pend_req_t *pending = &table->slots[i];

        if (pend_is_confirm_parked(pending) == false)
        {
            continue;
        }

        if (pending->park_ms == 0U)
        {
            // 懒初始化停泊时刻。⚠ 平台时钟缺失时 now_ms 恒 0 ⇒ park_ms 保持 0、
            // 超时永不触发（有界失败依赖平台时钟已在 main 的装配阶段注册）。
            pending->park_ms = now_ms;
        }

        const ftl_status_e st = s_confirm_handler(confirm_ctx, pending);

        if (st == FTL_AGAIN)
        {
            if ((now_ms - pending->park_ms) < FTL_GROUP_DRAIN_TIMEOUT_MS)
            {
                continue; /* region 仍在加载：保留停泊，下轮再来 */
            }

            // 限流：元数据日志区卡死时，每个 host 写都会超时（100ms 一个），
            // 逐条 ERR 会刷屏；5s 一行仍能看见问题。
            if ((s_park_log_ms == 0U) || ((now_ms - s_park_log_ms) >= 5000U))
            {
                LOGE(LOG_TAG_FTL "confirm park timeout lba=%lu idx=%hhu parked=%hu\r\n", pending->lba, i, parked);
                s_park_log_ms = now_ms;
            }
        }
        else if (st != FTL_OK)
        {
            LOGE(LOG_TAG_FTL "confirm failed lba=%lu idx=%hhu\r\n", pending->lba, i);
        }

        // 结算：owners 必须在释放**之前**取走（组完成通知可能重入复用本槽）。
        const DWORD        owners = pending->owners;
        const ssd_status_e status = (st == FTL_OK) ? SSD_STATUS_OK : SSD_STATUS_IO_ERROR;

        pend_release_index(table, i);
        pend_group_complete_subop_multi(table, owners, status);
    }
}

/*
 * pending.c — Pending Request 表 + 完成组（ftl_io_group_t）实现
 *
 * 职责：
 *   ① 跟踪异步请求的 command_id → 请求映射，供 FC 完成回调据以做 caller 通知
 *      与写回滚簿记（ftl_host_io_callback → pend_find）；
 *   ② 实现「完成组」——把一条 host 请求拆出的多条 FC 子操作聚成一个完成单元，
 *      通知字段（complete_flag/status_out/on_complete）只在最后一个子操作完成后
 *      触发一次（见 pending_group_* 族）。
 *
 * 与 pending.h 的分工：
 *   pending.h —— 表结构（ftl_pending_req_t / ftl_io_group_t）+ 容量宏 + 语义约定；
 *   本文件     —— 两张表的增删查 + 组完成编排，无新数据结构。
 *
 * 存储：pending[] / groups[] 由 ftl_pending_table_t（per-instance）持有；
 *   本文件不持有静态存储，全部操作挂在传入的 table 上。
 *
 * 文件结构：
 *   1. pending 表基础操作   init / alloc / lookup / remove / find_op / release_index / count
 *   2. 完成组操作           alloc / count / maybe_complete / complete_subop[_multi]
 *
 * ⚠ 读写判别必须**显式携带** op（ftl_pending_op_t），不可从 io.buffer 反推——
 *   写请求的 buffer 是数据源、非 NULL ⇒ `buffer != NULL` 对写恒真（见 pending.h 文件头）。
 */

#include "pending.h"

#include "assertion.h"
#include "ftl_job.h" /* ftl_job_now_ms（confirm 停泊超时） */
#include "log.h"

#include <string.h>

/*
 * pend_init — 清空本实例的 pending 表与完成组表
 *
 * 全零即「无占用」（used==0），开机/重建时调用。表是 per-instance 的：每个 FTL
 * 实例各有一份 pending/groups（多 LUN 下完成按命令的 ch/ce/lun 路由回本实例）。
 */
void pend_init(ftl_pending_table_t *table)
{
    memset(table->slots, 0, sizeof(table->slots));
    memset(table->groups, 0, sizeof(table->groups));
}

/* ── write-confirm park（停泊列表 = pending 表里的 WRITE_CONFIRM 项）──
 *
 * 不另建一张列表：停泊项本来就在 pending 表里（op = WRITE_CONFIRM、command_id
 * 已脱钩为哨兵），另建列表会变成第二套坐标（C9）。pending 模块负责扫描/超时/结算，
 * 数据面事务由 FTL 注入的 handler 执行（C2/R9：不复制 Journal→L2P→BST 顺序）。 */

static pending_confirm_handler_t s_confirm_handler;

void pend_confirm_set_handler(pending_confirm_handler_t handler)
{
    s_confirm_handler = handler;
}

void pend_confirm_park(ftl_pending_table_t *table, WORD pending_idx, DWORD owners)
{
    ftl_pending_req_t *pending = &table->slots[pending_idx];

    ASSERT(pending->used != 0U);

    pending->op         = PENDING_OP_WRITE_CONFIRM;
    pending->command_id = FTL_PENDING_CMD_NONE;
    pending->owners     = owners;
    pending->park_ms    = ftl_job_now_ms();
}

void pend_confirm_service(ftl_pending_table_t *table, void *confirm_ctx, const ftl_bg_info_t *info)
{
    const DWORD now_ms = ((info != NULL) && (info->now_ms != 0U)) ? info->now_ms : ftl_job_now_ms();

    for (BYTE i = 0U; i < FTL_PENDING_MAX; i++)
    {
        ftl_pending_req_t *pending = &table->slots[i];

        if ((pending->used == 0U) || (!pend_op_is_write_confirm(pending->op)))
        {
            continue;
        }

        if (pending->park_ms == 0U)
        {
            pending->park_ms = now_ms; /* 懒初始化：停泊时刻（沿用 ftl_job 的时钟口径） */
        }

        const ftl_status_t st = s_confirm_handler(confirm_ctx, pending);

        if (st == FTL_AGAIN)
        {
            if ((now_ms - pending->park_ms) < FTL_GROUP_DRAIN_TIMEOUT_MS)
            {
                continue; /* region 仍在加载：保留停泊，下轮再来 */
            }
            LOGE("[FTL] confirm park timeout lba=%lu idx=%u\r\n", pending->lba, (unsigned)i);
        }
        else if (st != FTL_OK)
        {
            LOGE("[FTL] confirm failed lba=%lu idx=%u\r\n", pending->lba, (unsigned)i);
        }

        /* 结算：owners 必须在释放**之前**取走（组完成通知可能重入复用本槽）。 */
        const DWORD        owners = pending->owners;
        const ssd_status_t status = (st == FTL_OK) ? SSD_STATUS_OK : SSD_STATUS_IO_ERROR;

        pend_release_index(table, i);
        pend_group_complete_subop_multi(table, owners, status);
    }
}

/* --------------------------------------------------------------------
 * pend_alloc
 *   功能：认领首个空闲 pending 槽（used=1、其余字段清零），字段由调用者就地填。
 *   参数：ftl —— 上下文。
 *   返回：空闲槽指针；NULL = 表满（FTL_PENDING_MAX=32）。
 *   实现要点：表满属预期内的资源结果，保留返回码（判据 3）。
 *             ⚠ **不存 old_pba**：完成时按 LBA 重查 L2P（见 ftl_pending_req_t 注释）。
 *             ⚠ 认领即置 used=1，调用者填完须调 pend_validate()（pending.h）。
 * -------------------------------------------------------------------- */
ftl_pending_req_t *pend_alloc(ftl_pending_table_t *table)
{
    for (BYTE i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if (table->slots[i].used == 0U)
        {
            memset(&table->slots[i], 0, sizeof(table->slots[i]));
            table->slots[i].used = 1U;
            return &table->slots[i];
        }
    }

    return NULL; /* 表满 */
}

/* pend_find — 按 command_id 查本实例 pending 项；未找到返回 NULL（线性扫描，表深仅 32） */
ftl_pending_req_t *pend_find(ftl_pending_table_t *table, WORD cmd_id)
{
    for (BYTE i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if ((table->slots[i].used != 0U) &&
            (table->slots[i].command_id == cmd_id))
        {
            return &table->slots[i];
        }
    }

    return NULL;
}

/* pend_remove — 置 used=0 释放槽位；未找到则 no-op（幂等，供完成路径重复调用） */
void pend_remove(ftl_pending_table_t *table, WORD cmd_id)
{
    ftl_pending_req_t *req = pend_find(table, cmd_id);

    if (req != NULL)
    {
        req->used = 0U;
    }
}

/* --------------------------------------------------------------------
 * pend_find_op
 *   功能：找第一个 op 类型匹配的 pending 项（按下标升序），出参其下标。
 *   参数：ftl —— 上下文；op —— 目标操作类型；out_index —— 出参下标（可 NULL）。
 *   返回：true 找到；false 无。
 *   实现要点：供 Read Recovery 取走被托管的子操作——那些项的 command_id 已置
 *             FTL_PENDING_CMD_NONE，pend_find 查不到，只能按 op 找。
 *             按下标升序 = 先进先出，与「托管顺序」一致（append-only，无移动）。
 * -------------------------------------------------------------------- */
bool pend_find_op(const ftl_pending_table_t *table, ftl_pending_op_t op, BYTE *out_index)
{
    for (BYTE i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if ((table->slots[i].used != 0U) && (table->slots[i].op == op))
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

bool pend_view_op(const ftl_pending_table_t *table, ftl_pending_op_t op, pending_op_view_t *out)
{
    ASSERT(table != NULL);
    ASSERT(out != NULL);

    BYTE idx = 0U;

    if (!pend_find_op(table, op, &idx))
    {
        return false;
    }

    const ftl_pending_req_t *slot = &table->slots[idx];

    out->index  = idx;
    out->owners = slot->owners;
    out->buffer = slot->buffer;
    out->lba    = slot->lba;
    out->n_cw   = slot->n_cw;
    return true;
}

/* --------------------------------------------------------------------
 * pend_release_index
 *   功能：按下标释放 pending 项（used=0）。
 *   参数：ftl —— 上下文；index —— 槽下标。
 *   返回：无。
 *   实现要点：与 pend_remove 的差别只在寻址方式——托管项脱钩后无 command_id 可查，
 *             收尾只能按下标。⚠ 下标由 pend_find_op 产出且表无移动 ⇒ 恒有效，
 *             故越界一律 ASSERT（判据 2：调用者保证的前置条件），不静默 no-op。
 * -------------------------------------------------------------------- */
void pend_release_index(ftl_pending_table_t *table, BYTE index)
{
    ASSERT(table->slots[index].used != 0U); /* 重复释放即簿记错误，留现场 */

    table->slots[index].used = 0U;
}

/* pend_get_count — 本实例占用槽位数（ftl_service 档位判定用它近似 host IO 空闲度） */
WORD pend_get_count(const ftl_pending_table_t *table)
{
    WORD count = 0U;

    for (BYTE i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if (table->slots[i].used != 0U)
        {
            count++;
        }
    }

    return count;
}

/* ── 完成组（ftl_io_group_t）── */

/* --------------------------------------------------------------------
 * pend_group_alloc
 *   功能：申请一个空闲完成组槽，used=1 并字段清零。
 *   参数：ftl —— 上下文。
 *   返回：组下标（0..FTL_GROUP_MAX-1），无空闲槽返回 FTL_ERROR。
 *   实现要点：组容量 = pending 容量（一个组至少占一个 pending 槽，组不会比在飞
 *             子操作多）；清零保证 total/next/outstanding/agg_status 从已知态起步。
 * -------------------------------------------------------------------- */
int pend_group_alloc(ftl_pending_table_t *table)
{
    for (BYTE i = 0U; i < FTL_GROUP_MAX; i++)
    {
        if (table->groups[i].used == 0U)
        {
            memset(&table->groups[i], 0, sizeof(table->groups[i]));
            table->groups[i].used = 1U;
            return (int)i;
        }
    }

    return FTL_ERROR; /* 组槽满 */
}

/* pend_get_group_count — 占用（未完成）组数（有未完成组 = host 工作饿着 → 归 BUSY） */
WORD pend_get_group_count(const ftl_pending_table_t *table)
{
    WORD count = 0U;

    for (BYTE i = 0U; i < FTL_GROUP_MAX; i++)
    {
        if (table->groups[i].used != 0U)
        {
            count++;
        }
    }

    return count;
}

/* --------------------------------------------------------------------
 * pend_group_maybe_complete
 *   功能：若组「已全部下发且全部完成」则一次性发通知并释放槽。
 *   参数：ftl —— 上下文；gi —— 组下标。
 *   返回：无。
 *   实现要点：
 *     · 完成条件：used 且 next==total（子操作已全部下发）且 outstanding==0
 *       （已下发的都已完成）。任一不满足即直接返回——这是**负例守卫**（判据 8），
 *       保证通知绝不早发。
 *     · 通知顺序：写 status_out → 置 complete_flag → 调 on_complete。
 *     · ⚠ used=0 必须在**通知之后**：on_complete 回调可能重入 ftl_submit_*
 *       再申请组槽，若先清零则本槽可能被该重入立刻复用、又被这里清零。
 * -------------------------------------------------------------------- */
void pend_group_maybe_complete(ftl_pending_table_t *table, WORD gi)
{
    ftl_io_group_t *g = &table->groups[gi];
    if ((g->used == 0U) || (g->next < g->total) || (g->outstanding != 0U))
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

    /* 回调可能重入 ftl_submit_* 再申请组槽 ⇒ used 清零必须在通知之后 */
    g->used = 0U;
}

/* --------------------------------------------------------------------
 * pend_group_add_subop / pend_group_drop_subop
 *   功能：组在飞子操作数的**唯一增减闸门**（H10 / §3.1）。
 *   参数：ftl —— 上下文；gi —— 组下标。
 *   返回：无。
 *   实现要点：outstanding 是「已下发、未完成」的组不变量。写点分散在 host 读
 *             （ftl_issue_subop）与页打包写（wb_handle(APPEND)）两处、递减点分散在
 *             完成服务与提交失败回滚——任一处漏掉即 host 写组永不完成、挂死。
 *             故所有增减都收敛到本对函数；完整完成走
 *             pend_group_complete_subop*（带聚合状态与完成通知），
 *             提交失败回滚走 drop（不触发完成通知）。
 * -------------------------------------------------------------------- */
void pend_group_add_subop(ftl_pending_table_t *table, WORD gi)
{
    ftl_io_group_t *g = &table->groups[gi];
    ASSERT(g->used != 0U);
    ASSERT(g->outstanding < UINT16_MAX);

    g->outstanding++;
}

void pend_group_drop_subop(ftl_pending_table_t *table, WORD gi)
{
    ftl_io_group_t *g = &table->groups[gi];
    ASSERT(g->used != 0U);
    ASSERT(g->outstanding > 0U);

    g->outstanding--;
}

/* --------------------------------------------------------------------
 * pend_group_complete_subop
 *   功能：一个子操作完成时的簿记——聚合状态、递减在飞数、试触发组完成。
 *   参数：ftl —— 上下文；gi —— 组下标；sub_status —— 本子操作完成状态。
 *   返回：无。
 *   实现要点：
 *     · caller 通知字段（complete_flag / status_out / on_complete）的**唯一写点**：
 *       由 ftl_host_io_callback 在子操作簿记后调用。
 *     · agg_status **粘性**：仅当 sub_status != OK 时写入，一旦失败即保留首个错误，
 *       后续成功不覆盖（组整体语义 = 任一子操作失败则整组 IO_ERROR）。
 *     · outstanding-- 后交 pend_group_maybe_complete 判是否整组完成。
 * -------------------------------------------------------------------- */
void pend_group_complete_subop(ftl_pending_table_t *table, WORD gi, ssd_status_t sub_status)
{
    ftl_io_group_t *g = &table->groups[gi];
    ASSERT(g->used != 0U); /* 组已释放 = 重复完成/簿记错误，debug 当场炸 */

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

/* --------------------------------------------------------------------
 * pend_group_complete_subop_multi
 *   功能：一个子操作完成、归属**多个组**（页打包下一页由多个组拼成）——逐组走一遍
 *         pend_group_complete_subop（聚合状态 + 递减 + 试触发）。
 *   参数：ftl —— 上下文；owners —— 贡献组位掩码；sub_status —— 完成状态。
 *   返回：无。
 *   实现要点：owners 按值传入即局部副本，遍历它安全——回调（on_complete）可能重入
 *             复用组槽，但本循环只认传入的位掩码，不受槽位变动影响。
 * -------------------------------------------------------------------- */
void pend_group_complete_subop_multi(ftl_pending_table_t *table, DWORD owners, ssd_status_t sub_status)
{
    for (WORD gi = 0U; gi < FTL_GROUP_MAX; gi++)
    {
        if ((owners & ftl_group_bit(gi)) != 0U)
        {
            pend_group_complete_subop(table, gi, sub_status);
        }
    }
}

/* --------------------------------------------------------------------
 * pend_group_has_window_pending
 *   功能：是否还有写组的下一个 LBA 落在指定 pair 窗口（wbuf 空闲 flush 判据）。
 *   参数：table —— pending 表；window —— pair 窗口基址。
 *   返回：true = 有写组的 next 落在本窗口。
 *   实现要点：只读扫描 groups[]，数组本身不暴露给 wbuf；判据与旧 wbuf 端口实现逐字一致。
 * -------------------------------------------------------------------- */
bool pend_group_has_window_pending(const ftl_pending_table_t *table, DWORD window)
{
    for (BYTE i = 0U; i < FTL_GROUP_MAX; i++)
    {
        const ftl_io_group_t *g = &table->groups[i];

        if ((g->used == 0U) || (!pend_op_is_write(g->op)) || (g->next >= g->total))
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

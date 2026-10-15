/*
 * pending.c — Pending Request 表 + 完成组（ftl_io_group_t）实现
 *
 * 职责：
 *   ① 跟踪异步请求的 command_id → 请求映射，供 FC 完成回调据以做 caller 通知
 *      与写回滚簿记（ftl_op_host_io → pending_lookup）；
 *   ② 实现「完成组」——把一条 host 请求拆出的多条 FC 子操作聚成一个完成单元，
 *      通知字段（complete_flag/status_out/on_complete）只在最后一个子操作完成后
 *      触发一次（见 pending_group_* 族）。
 *
 * 与 pending.h 的分工：
 *   pending.h —— 表结构（ftl_pending_req_t / ftl_io_group_t）+ 容量宏 + 语义约定；
 *   本文件     —— 两张表的增删查 + 组完成编排，无新数据结构。
 *
 * 存储：pending[] / groups[] 均内嵌于 ftl_context_t（per-instance，F6），由调用者提供；
 *   本文件不持有静态存储，全部操作挂在传入的 ftl 上。
 *
 * 文件结构：
 *   1. pending 表基础操作   init / add / lookup / remove / find_op / release_index / count
 *   2. 完成组操作           alloc / free / count / maybe_complete / complete_subop
 *
 * ⚠ 读写判别必须**显式携带** op（ftl_pending_op_t），不可从 io.buffer 反推——
 *   写请求的 buffer 是数据源、非 NULL ⇒ `buffer != NULL` 对写恒真（见 pending.h 文件头）。
 */

#include "ftl/pending.h"

#include "common/assertion.h"
#include "ftl/ftl.h" /* ftl_context_t */

#include <string.h>

/*
 * pending_init — 清空本实例的 pending 表与完成组表
 *
 * 全零即「无占用」（used==0），开机/重建时调用。F6 起 per-instance：每个 FTL 实例
 * 各有一份 pending/groups，不再共用全局表。
 */
void pending_init(ftl_context_t *ftl)
{
    memset(ftl->pending, 0, sizeof(ftl->pending));
    memset(ftl->groups, 0, sizeof(ftl->groups));
}

/* --------------------------------------------------------------------
 * pending_add
 *   功能：注册一个 pending 子操作（首个空闲槽），字段按值拷入。
 *   参数：ftl —— 上下文；cmd_id —— FC command_id；owners —— 贡献组位掩码（读=1<<gi，
 *         写页打包=wbuf.owners）；lba/buffer/n_cw —— 子操作起始 LBA / 缓冲 / 搬运的 CW 数
 *         （512B 单元）；new_pba/new_pba1 —— 写：新分配的 plane0 / plane1 **页** PBA
 *         （未覆盖传 INVALID）；cache_intermediate —— Cache Program 中间页标志；
 *         op —— 操作类型（完成路径判别依据）；cw_bitmap —— 写页的有效 CW 位图（读传 0）。
 *   返回：FTL_OK 成功，FTL_ERROR 表满（FTL_PENDING_MAX=32）。
 *   实现要点：子操作字段按值拷入 ⇒ submit 返回后调用者可复用其局部量；表满属
 *             预期内的资源结果，保留返回码（判据 3）。
 *             ⚠ **不存 old_pba**：完成时按 LBA 重查 L2P（见 ftl_pending_req_t 注释）。
 * -------------------------------------------------------------------- */
int pending_add(ftl_context_t *ftl, uint16_t cmd_id, uint32_t owners, lba_t lba, void *buffer, uint8_t n_cw, nand_pba_t new_pba, nand_pba_t new_pba1, uint8_t cache_intermediate, ftl_pending_op_t op, uint8_t cw_bitmap)
{
    uint8_t i;

    ASSERT(owners != 0U); /* 至少一个贡献组（读恒单组；写页打包 ≥1） */
    ASSERT((n_cw > 0U) && (n_cw <= FTL_LBA_PER_PAIR));

    for (i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if (ftl->pending[i].used == 0U)
        {
            ftl->pending[i].used               = 1U;
            ftl->pending[i].command_id         = cmd_id;
            ftl->pending[i].owners             = owners;
            ftl->pending[i].op                 = (uint8_t)op;
            ftl->pending[i].n_cw               = n_cw;
            ftl->pending[i].cw_bitmap          = cw_bitmap;
            ftl->pending[i].lba                = lba;
            ftl->pending[i].buffer             = buffer;
            ftl->pending[i].new_pba            = new_pba;
            ftl->pending[i].new_pba1           = new_pba1;
            ftl->pending[i].cache_intermediate = cache_intermediate;
            return FTL_OK;
        }
    }

    return FTL_ERROR; /* 表满 */
}

/* pending_lookup — 按 command_id 查本实例 pending 项；未找到返回 NULL（线性扫描，表深仅 32） */
ftl_pending_req_t *pending_lookup(ftl_context_t *ftl, uint16_t cmd_id)
{
    uint8_t i;

    for (i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if ((ftl->pending[i].used != 0U) &&
            (ftl->pending[i].command_id == cmd_id))
        {
            return &ftl->pending[i];
        }
    }

    return NULL;
}

/* pending_remove — 置 used=0 释放槽位；未找到则 no-op（幂等，供完成路径重复调用） */
void pending_remove(ftl_context_t *ftl, uint16_t cmd_id)
{
    ftl_pending_req_t *req;

    req = pending_lookup(ftl, cmd_id);
    if (req != NULL)
    {
        req->used = 0U;
    }
}

/* --------------------------------------------------------------------
 * pending_find_op
 *   功能：找第一个 op 类型匹配的 pending 项（按下标升序），出参其下标。
 *   参数：ftl —— 上下文；op —— 目标操作类型；out_index —— 出参下标（可 NULL）。
 *   返回：1 找到；0 无。
 *   实现要点：供 Read Recovery 取走被托管的子操作——那些项的 command_id 已置
 *             FTL_PENDING_CMD_NONE，pending_lookup 查不到，只能按 op 找。
 *             按下标升序 = 先进先出，与「托管顺序」一致（append-only，无移动）。
 * -------------------------------------------------------------------- */
int pending_find_op(const ftl_context_t *ftl, ftl_pending_op_t op, uint8_t *out_index)
{
    uint8_t i;

    for (i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if ((ftl->pending[i].used != 0U) && (ftl->pending[i].op == (uint8_t)op))
        {
            if (out_index != NULL)
            {
                *out_index = i;
            }
            return 1;
        }
    }

    return 0;
}

/* --------------------------------------------------------------------
 * pending_release_index
 *   功能：按下标释放 pending 项（used=0）。
 *   参数：ftl —— 上下文；index —— 槽下标。
 *   返回：无。
 *   实现要点：与 pending_remove 的差别只在寻址方式——托管项脱钩后无 command_id 可查，
 *             收尾只能按下标。⚠ 下标由 pending_find_op 产出且表无移动 ⇒ 恒有效，
 *             故越界一律 ASSERT（判据 2：调用者保证的前置条件），不静默 no-op。
 * -------------------------------------------------------------------- */
void pending_release_index(ftl_context_t *ftl, uint8_t index)
{
    ASSERT(index < FTL_PENDING_MAX);
    ASSERT(ftl->pending[index].used != 0U); /* 重复释放即簿记错误，留现场 */

    ftl->pending[index].used = 0U;
}

/* pending_count — 本实例占用槽位数（B3：ftl_service 三级模式判定用它近似 host IO 空闲度） */
uint16_t pending_count(const ftl_context_t *ftl)
{
    uint16_t count = 0U;
    uint8_t  i;

    for (i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if (ftl->pending[i].used != 0U)
        {
            count++;
        }
    }

    return count;
}

/* ── 完成组（ftl_io_group_t）── */

/* --------------------------------------------------------------------
 * pending_group_alloc
 *   功能：申请一个空闲完成组槽，used=1 并字段清零。
 *   参数：ftl —— 上下文。
 *   返回：组下标（0..FTL_GROUP_MAX-1），无空闲槽返回 FTL_ERROR。
 *   实现要点：组容量 = pending 容量（一个组至少占一个 pending 槽，组不会比在飞
 *             子操作多）；清零保证 total/next/outstanding/agg_status 从已知态起步。
 * -------------------------------------------------------------------- */
int pending_group_alloc(ftl_context_t *ftl)
{
    uint8_t i;

    for (i = 0U; i < FTL_GROUP_MAX; i++)
    {
        if (ftl->groups[i].used == 0U)
        {
            memset(&ftl->groups[i], 0, sizeof(ftl->groups[i]));
            ftl->groups[i].used = 1U;
            return (int)i;
        }
    }

    return FTL_ERROR; /* 组槽满 */
}

/* pending_group_free — 释放组槽（used=0）；下标越界是调用者 bug ⇒ ASSERT（判据 2） */
void pending_group_free(ftl_context_t *ftl, uint16_t gi)
{
    ASSERT(gi < FTL_GROUP_MAX);

    ftl->groups[gi].used = 0U;
}

/* pending_group_count — 占用（未完成）组数（B3：有未完成组 = host 工作饿着 → 归 BUSY） */
uint16_t pending_group_count(const ftl_context_t *ftl)
{
    uint16_t count = 0U;
    uint8_t  i;

    for (i = 0U; i < FTL_GROUP_MAX; i++)
    {
        if (ftl->groups[i].used != 0U)
        {
            count++;
        }
    }

    return count;
}

/* --------------------------------------------------------------------
 * pending_group_maybe_complete
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
void pending_group_maybe_complete(ftl_context_t *ftl, uint16_t gi)
{
    ftl_io_group_t *g;

    ASSERT(gi < FTL_GROUP_MAX);

    g = &ftl->groups[gi];
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
 * pending_group_complete_subop
 *   功能：一个子操作完成时的簿记——聚合状态、递减在飞数、试触发组完成。
 *   参数：ftl —— 上下文；gi —— 组下标；sub_status —— 本子操作完成状态。
 *   返回：无。
 *   实现要点：
 *     · 由 ftl_op_host_io 在子操作簿记后调用，替代原先直接写 caller
 *       通知字段（收敛点）。
 *     · agg_status **粘性**：仅当 sub_status != OK 时写入，一旦失败即保留首个错误，
 *       后续成功不覆盖（组整体语义 = 任一子操作失败则整组 IO_ERROR）。
 *     · outstanding-- 后交 pending_group_maybe_complete 判是否整组完成。
 * -------------------------------------------------------------------- */
void pending_group_complete_subop(ftl_context_t *ftl, uint16_t gi, ssd_status_t sub_status)
{
    ftl_io_group_t *g;

    ASSERT(gi < FTL_GROUP_MAX);

    g = &ftl->groups[gi];
    ASSERT(g->used != 0U); /* 组已释放 = 重复完成/簿记错误，debug 当场炸 */

    if (sub_status != SSD_STATUS_OK)
    {
        g->agg_status = sub_status; /* 粘性：一旦失败即保留 */
    }
    if (g->outstanding > 0U)
    {
        g->outstanding--;
    }

    pending_group_maybe_complete(ftl, gi);
}

/* --------------------------------------------------------------------
 * pending_group_complete_subop_multi
 *   功能：一个子操作完成、归属**多个组**（页打包下一页由多个组拼成）——逐组走一遍
 *         pending_group_complete_subop（聚合状态 + 递减 + 试触发）。
 *   参数：ftl —— 上下文；owners —— 贡献组位掩码；sub_status —— 完成状态。
 *   返回：无。
 *   实现要点：owners 按值传入即局部副本，遍历它安全——回调（on_complete）可能重入
 *             复用组槽，但本循环只认传入的位掩码，不受槽位变动影响。
 * -------------------------------------------------------------------- */
void pending_group_complete_subop_multi(ftl_context_t *ftl, uint32_t owners, ssd_status_t sub_status)
{
    uint16_t gi;

    for (gi = 0U; gi < FTL_GROUP_MAX; gi++)
    {
        if ((owners & (uint32_t)(1U << gi)) != 0U)
        {
            pending_group_complete_subop(ftl, gi, sub_status);
        }
    }
}

/*
 * pending.c — Pending Request 表 + 完成组（ftl_io_group_t）实现
 *
 * 职责：
 *   ① 跟踪异步请求的 command_id → 请求映射，供 FC 完成回调据以做 caller 通知
 *      与写回滚簿记（ftl_handle_completion → pending_lookup）；
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
 *   1. pending 表基础操作   init / add / lookup / remove / count
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
    /* 层内接口：唯一调用者 ftl_init 已在入口判过 ftl != NULL */
    ASSERT(ftl != NULL);

    memset(ftl->pending, 0, sizeof(ftl->pending));
    memset(ftl->groups, 0, sizeof(ftl->groups));
}

/* --------------------------------------------------------------------
 * pending_add
 *   功能：注册一个 pending 子操作（首个空闲槽），字段按值拷入。
 *   参数：ftl —— 上下文；cmd_id —— FC command_id；group —— 所属完成组下标；
 *         lba/buffer/planes —— 子操作起始 LBA / 缓冲 / 页数（1 单 plane，2 multi-plane）；
 *         old_pba/new_pba(/1) —— 写前旧 PBA、新分配 PBA（plane0/plane1，读传 INVALID）；
 *         cache_intermediate —— Cache Program 中间页标志（1 跳过 L2P/Journal 更新）；
 *         op —— 操作类型（完成路径判别依据）。
 *   返回：FTL_OK 成功，FTL_ERROR 表满（FTL_PENDING_MAX=32）。
 *   实现要点：子操作字段按值拷入 ⇒ submit 返回后调用者可复用其局部量；表满属
 *             预期内的资源结果，保留返回码（判据 3）。
 * -------------------------------------------------------------------- */
int pending_add(ftl_context_t *ftl, uint16_t cmd_id, uint16_t group, lba_t lba, void *buffer, uint8_t planes, nand_pba_t old_pba, nand_pba_t new_pba, nand_pba_t old_pba1, nand_pba_t new_pba1, uint8_t cache_intermediate, ftl_pending_op_t op)
{
    uint8_t i;

    /* 层内接口：调用者 ftl_submit_read/write 已在入口判过 ftl / req 非 NULL */
    ASSERT(ftl != NULL);
    ASSERT(group < FTL_GROUP_MAX);

    for (i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if (ftl->pending[i].used == 0U)
        {
            ftl->pending[i].used               = 1U;
            ftl->pending[i].command_id         = cmd_id;
            ftl->pending[i].group              = group;
            ftl->pending[i].op                 = (uint8_t)op;
            ftl->pending[i].planes             = planes;
            ftl->pending[i].lba                = lba;
            ftl->pending[i].buffer             = buffer;
            ftl->pending[i].old_pba            = old_pba;
            ftl->pending[i].new_pba            = new_pba;
            ftl->pending[i].old_pba1           = old_pba1;
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

    ASSERT(ftl != NULL);

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

    ASSERT(ftl != NULL);

    req = pending_lookup(ftl, cmd_id);
    if (req != NULL)
    {
        req->used = 0U;
    }
}

/* pending_count — 本实例占用槽位数（B3：ftl_service 三级模式判定用它近似 host IO 空闲度） */
uint16_t pending_count(const ftl_context_t *ftl)
{
    uint16_t count = 0U;
    uint8_t  i;

    ASSERT(ftl != NULL);

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

    ASSERT(ftl != NULL);

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
    ASSERT(ftl != NULL);
    ASSERT(gi < FTL_GROUP_MAX);

    ftl->groups[gi].used = 0U;
}

/* pending_group_count — 占用（未完成）组数（B3：有未完成组 = host 工作饿着 → 归 BUSY） */
uint16_t pending_group_count(const ftl_context_t *ftl)
{
    uint16_t count = 0U;
    uint8_t  i;

    ASSERT(ftl != NULL);

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

    ASSERT(ftl != NULL);
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
 *     · 由 ftl_handle_completion 在子操作簿记后调用，替代原先直接写 caller
 *       通知字段（收敛点）。
 *     · agg_status **粘性**：仅当 sub_status != OK 时写入，一旦失败即保留首个错误，
 *       后续成功不覆盖（组整体语义 = 任一子操作失败则整组 IO_ERROR）。
 *     · outstanding-- 后交 pending_group_maybe_complete 判是否整组完成。
 * -------------------------------------------------------------------- */
void pending_group_complete_subop(ftl_context_t *ftl, uint16_t gi, ssd_status_t sub_status)
{
    ftl_io_group_t *g;

    ASSERT(ftl != NULL);
    ASSERT(gi < FTL_GROUP_MAX);

    g = &ftl->groups[gi];
    if (g->used == 0U)
    {
        return; /* 组已释放（防御：不应发生） */
    }

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

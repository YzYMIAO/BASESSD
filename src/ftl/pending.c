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
 * 存储：pending[] / groups[] 由 pending_ctx_t（per-instance）持有；文件级 static
 *   只放下溢留痕的限流时间戳，不持有表数据。
 *
 * 文件结构：
 *   1. pending 表基础操作   init / alloc / find / remove / find_op / view_op / release_index / count
 *   2. 完成组操作           alloc / count / add·drop_subop / complete_subop[_multi] /
 *                           maybe_complete / has_window_pending
 *   3. write-confirm 待入账   park / find_next / is_expired / settle / get_count
 *
 * ⚠ 读写判别必须**显式携带** op（pend_op_e），不可从 buffer 字段反推——
 *   写请求的 buffer 是数据源、非 NULL ⇒ `buffer != NULL` 对写恒真（见 pending.h 文件头）。
 */

#include "pending.h"

#include "assertion.h"
#include "log.h"
#include "timer.h"

#include <string.h>

/* ── 文件级静态数据 ── */

// 组 outstanding 下溢（重复完成 / 重复回滚）的 release 留痕：debug 由 ASSERT 拦住，
// release 必须阻止 WORD 回绕（回绕 ⇒ 组永不完成 ⇒ host 写挂死）。
static DWORD s_group_underflow_log_ms;
static DWORD s_group_underflow_n;

/**
 * @brief 清除 pending 项与 command_id 的映射
 *
 * @param table pending 表
 * @param req   待脱钩的请求槽；command_id 为 NONE 时跳过
 */
static void pend_unbind(pending_ctx_t *table, pend_req_t *req)
{
    const WORD cmd_id = req->command_id;
    const BYTE index  = (BYTE)(req - table->slots);

    if ((cmd_id < FC_COMMAND_POOL_DEPTH) && (table->slot_of[cmd_id] == index))
    {
        table->slot_of[cmd_id] = 0xFF;
    }
}

/**
 * @brief 清空本实例的 pending 表、完成组表与 command_id 映射
 *
 * @param table pending 表
 */
void pend_init(pending_ctx_t *table)
{
    memset(table->slots, 0, sizeof(table->slots));
    memset(table->groups, 0, sizeof(table->groups));
    memset(table->slot_of, 0xFF, sizeof(table->slot_of));
}

/**
 * @brief 把已有 pending 槽绑定到新的 command_id（写重试替换命令时用）
 *
 * @param table  pending 表
 * @param req    pending 槽
 * @param cmd_id 新 command_id
 */
void pend_bind(pending_ctx_t *table, pend_req_t *req, WORD cmd_id)
{
    ASSERT(table != NULL);
    ASSERT(req != NULL);
    ASSERT((req >= table->slots) && (req < &table->slots[FTL_PENDING_MAX]));
    ASSERT(cmd_id < FC_COMMAND_POOL_DEPTH);

    const BYTE index = (BYTE)(req - table->slots);

    pend_unbind(table, req);
    ASSERT(table->slot_of[cmd_id] == 0xFF); /* 同一 command_id 不得同时绑两个槽 */
    req->command_id        = cmd_id;
    table->slot_of[cmd_id] = index;
}

/**
 * @brief 与 FC 命令脱钩：清映射并把 command_id 置哨兵
 *
 * 脱钩项只能按 op 查找。
 *
 * @param table pending 表
 * @param req   待脱钩的槽
 */
void pend_detach_command(pending_ctx_t *table, pend_req_t *req)
{
    ASSERT(table != NULL);
    ASSERT(req != NULL);

    pend_unbind(table, req);
    req->command_id = FTL_PENDING_CMD_NONE;
}

/**
 * @brief 分配一个 pending 槽并绑定 command_id
 *
 * 认领即置 used=1，调用者填完须调 pend_validate()（pending.h）。
 *
 * @param table  pending 表
 * @param cmd_id 绑定的 command_id
 * @return 槽指针；NULL = 池满
 */
pend_req_t *pend_alloc(pending_ctx_t *table, WORD cmd_id)
{
    ASSERT(cmd_id < FC_COMMAND_POOL_DEPTH);

    if (cmd_id >= FC_COMMAND_POOL_DEPTH)
    {
        return NULL;
    }

    for (BYTE i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if (table->slots[i].used == false)
        {
            memset(&table->slots[i], 0, sizeof(table->slots[i]));
            table->slots[i].used = true;
            pend_bind(table, &table->slots[i], cmd_id);
            return &table->slots[i];
        }
    }

    return NULL; /* 表满 */
}

/**
 * @brief 按 command_id 直接索引 pending 槽
 *
 * 映射为空 / 槽已释放 / id 不匹配都返回 NULL。
 *
 * @param table  pending 表
 * @param cmd_id command_id
 * @return 槽指针；NULL = 未找到
 */
pend_req_t *pend_find(pending_ctx_t *table, WORD cmd_id)
{
    if (cmd_id >= FC_COMMAND_POOL_DEPTH)
    {
        return NULL;
    }

    const BYTE index = table->slot_of[cmd_id];

    if (index >= FTL_PENDING_MAX)
    {
        return NULL;
    }

    pend_req_t *req = &table->slots[index];

    if ((req->used == false) || (req->command_id != cmd_id))
    {
        return NULL;
    }

    return req;
}

/**
 * @brief 释放一个 pending 槽并清映射
 *
 * 未找到则 no-op（幂等，供完成路径重复调用）。
 *
 * @param table  pending 表
 * @param cmd_id command_id
 */
void pend_remove(pending_ctx_t *table, WORD cmd_id)
{
    pend_req_t *req = pend_find(table, cmd_id);

    if (req != NULL)
    {
        pend_unbind(table, req);
        req->used = false;
    }
}

/**
 * @brief 按 op 找托管项（command_id 已置 NONE）
 *
 * 下标升序 = FIFO，与托管顺序一致。
 *
 * @param table     pending 表
 * @param op        操作类型
 * @param out_index 输出：槽下标
 * @return true 找到；false 无托管项
 */
bool pend_find_op(const pending_ctx_t *table, pend_op_e op, BYTE *out_index)
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

/**
 * @brief 按 op 查槽并拷出只读视图
 *
 * 不改槽状态。
 *
 * @param table pending 表
 * @param op    操作类型
 * @param out   输出：视图
 * @return true 找到；false 无托管项
 */
bool pend_view_op(const pending_ctx_t *table, pend_op_e op, pend_op_view_t *out)
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

/**
 * @brief 按索引释放托管项
 *
 * 下标由 pend_find_op 产出且表无移动 ⇒ 越界 ASSERT。
 *
 * @param table pending 表
 * @param index 槽下标
 */
void pend_release_index(pending_ctx_t *table, BYTE index)
{
    ASSERT(index < FTL_PENDING_MAX);
    ASSERT(table->slots[index].used == true); /* 重复释放即簿记错误，留现场 */

    pend_unbind(table, &table->slots[index]);
    table->slots[index].used = false;
}

/**
 * @brief 是否有不属于 cache 序列的写在飞（cache_role == NONE）
 *
 * 有则本 LBA 流不能再按序列续。
 *
 * @param table pending 表
 * @return true 有外部写；false 无
 */
bool pend_has_foreign_write(const pending_ctx_t *table)
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

/**
 * @brief 本实例占用的 pending 槽位数
 *
 * ftl_service 档位判定用它近似 host IO 空闲度。
 *
 * @param table pending 表
 * @return 占用槽位数
 */
WORD pend_slot_get_count(const pending_ctx_t *table)
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

/**
 * @brief 分配一个完成组
 *
 * 组容量 FTL_GROUP_MAX；清零保证从已知态起步。
 *
 * @param table  pending 表
 * @param out_gi 输出：组下标
 * @return true 已分配；false 组表满
 */
bool pend_group_alloc(pending_ctx_t *table, BYTE *out_gi)
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

/**
 * @brief 未完成组数
 *
 * 有未完成组 = host 工作饿着 → 归 BUSY。
 *
 * @param table pending 表
 * @return 组数
 */
WORD pend_group_get_count(const pending_ctx_t *table)
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

/**
 * @brief 组 outstanding 下溢的限流日志
 *
 * @param who 触发者（静态字符串，日志用）
 * @param gi  组下标
 */
static void pend_note_underflow(const char *who, WORD gi)
{
    LOGE_IF(log_rate_due(&s_group_underflow_log_ms, &s_group_underflow_n, LOG_RATE_SLOW_INTERVAL_MS), LOG_TAG_FTL "group outstanding underflow (%s gi=%hu) — bookkeeping bug\r\n", who, gi);
}

/**
 * @brief 撤销一个尚未提交的子操作（组在飞数 -1）
 *
 * 不触发完成通知；任一路径漏掉即 host 写组永不完成。
 *
 * @param table pending 表
 * @param gi    组下标
 */
void pend_group_drop_subop(pending_ctx_t *table, WORD gi)
{
    ASSERT(gi < FTL_GROUP_MAX);

    pend_group_t *g = &table->groups[gi];

    ASSERT(g->used == true);

    // 与 complete_subop 同口径：debug 断言；release 不下溢（WORD 回绕 ⇒ 组永不完成）。
    ASSERT(g->outstanding > 0U);

    if (g->outstanding > 0U)
    {
        g->outstanding--;
    }
    else
    {
        pend_note_underflow("drop", gi);
    }
}

/**
 * @brief 一条命令完成多个组的子操作
 *
 * agg_status 粘性；outstanding-- 后交 maybe_complete。
 *
 * @param table      pending 表
 * @param owners     组位图
 * @param sub_status 本子操作状态
 */
void pend_group_complete_subop_multi(pending_ctx_t *table, DWORD owners, ssd_status_e sub_status)
{
    for (WORD gi = 0U; gi < FTL_GROUP_MAX; gi++)
    {
        if ((owners & ftl_group_bit(gi)) == 0U)
        {
            continue;
        }

        pend_group_t *g = &table->groups[gi];

        ASSERT(g->used == true); /* 组已释放 = 重复完成/簿记错误 */

        if (sub_status != SSD_STATUS_OK)
        {
            g->agg_status = sub_status; /* 粘性：一旦失败即保留 */
        }

        // 与 drop_subop 同口径：debug 断言；release 不下溢（WORD 回绕 ⇒ 组永不完成）。
        ASSERT(g->outstanding > 0U);

        if (g->outstanding > 0U)
        {
            g->outstanding--;
        }
        else
        {
            pend_note_underflow("complete", gi);
        }

        pend_group_maybe_complete(table, gi);
    }
}

/**
 * @brief 若组已归零则结算
 *
 * used=0 必须在通知之后（回调可能重入再申请槽）。
 *
 * @param table pending 表
 * @param gi    组下标
 */
void pend_group_maybe_complete(pending_ctx_t *table, WORD gi)
{
    ASSERT(gi < FTL_GROUP_MAX);

    pend_group_t *g = &table->groups[gi];
    if ((g->used == false) || (g->next < g->total) || (g->outstanding != 0U))
    {
        return;
    }

    if (g->completion.status_out != NULL)
    {
        *g->completion.status_out = g->agg_status;
    }
    if (g->completion.complete_flag != NULL)
    {
        *g->completion.complete_flag = 1U;
    }
    if (g->completion.on_complete != NULL)
    {
        g->completion.on_complete(g->completion.on_complete_ctx);
    }

    // 回调可能重入 ftl_submit_* 再申请组槽 ⇒ used 清零必须在通知之后
    g->used = false;
}

/**
 * @brief 该窗口是否还有未完成的组
 *
 * 只读扫描 groups[]；数组不暴露给 wbuf。
 *
 * @param table  pending 表
 * @param window 窗口号
 * @return true 仍有未完成组
 */
bool pend_group_has_window_pending(const pending_ctx_t *table, DWORD window)
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

/* ── write-confirm park（待入账列表 = pending 表里的 WRITE_CONFIRM 项）──
 *
 * 不另建一张列表：待入账项本来就在 pending 表里（op = WRITE_CONFIRM、command_id
 * 已脱钩为哨兵），另建列表会变成第二套坐标（C9）。
 * 本模块只提供表原语（遍历 / 判超时 / 结算）；数据面事务由调用者的写确认服务
 * 直调执行（C2/R9：不复制 Journal→L2P→BST 顺序）。
 * 顺序不变量（owners → release → complete）只在本文件的 pend_confirm_settle 内
 * 维护，调用者不碰 owners 与 release。 */

/**
 * @brief 把一个 pending 槽挂到组的待入账集合
 *
 * 失败或超时的释放由本函数唯一负责——两侧都释放就是重复归还（C12）。
 *
 * @param table       pending 表
 * @param pending_idx pending 槽下标
 * @param owners      组位图
 */
void pend_confirm_park(pending_ctx_t *table, WORD pending_idx, DWORD owners)
{
    ASSERT(pending_idx < FTL_PENDING_MAX);

    pend_req_t *pending = &table->slots[pending_idx];

    ASSERT(pending->used == true);

    pend_detach_command(table, pending);
    pending->op      = PENDING_OP_WRITE_CONFIRM;
    pending->owners  = owners;
    pending->park_ms = platform_now_ms();
}

/**
 * @brief 结算一条待入账项（成功 / 失败）
 *
 * @param table  pending 表
 * @param index  pending 槽下标
 * @param status 结算状态
 */
void pend_confirm_settle(pending_ctx_t *table, BYTE index, ssd_status_e status)
{
    ASSERT(table != NULL);
    ASSERT(index < FTL_PENDING_MAX);

    pend_req_t *pending = &table->slots[index];

    ASSERT(pend_confirm_is_parked(table, index));

    // owners 必须在释放**之前**取走：组完成通知可能重入提交并复用本槽。
    const DWORD owners = pending->owners;

    pend_release_index(table, index);
    pend_group_complete_subop_multi(table, owners, status);
}

/**
 * @brief 待入账项数量
 *
 * @param table pending 表
 * @return 数量
 */
WORD pend_confirm_get_count(const pending_ctx_t *table)
{
    ASSERT(table != NULL);

    WORD count = 0U;

    for (BYTE i = 0U; i < FTL_PENDING_MAX; i++)
    {
        if (pend_confirm_is_parked(table, i) == true)
        {
            count++;
        }
    }

    return count;
}

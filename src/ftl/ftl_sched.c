/*
 * ftl_sched.c — FTL 请求调度器（L2P + host + 后台 job）
 *
 * 职责：
 *   · 静态 work item 池 + 按 class 的 credit 记账（host / L2P_LOAD / L2P_FLUSH / 后台 job）；
 *   · 把各模块请求归一为 item，并按 FTL_SCHED_PRIO_E 唯一顺序分派；
 *   · 后台策略：按负载选档 → token bucket 预算 → 按 ftl_bg_job_e 表序提交；
 *   · host 组回收 / 补发、metadata slot 依赖唤醒。
 *
 * 关键机制：
 *   · item 状态机 FREE → READY → WAIT_DEP / INFLIGHT；release 是计数回退的唯一出口；
 *   · 后台 job 用池头部预留项，不参与普通分配；L2P load 与 flush 分别计 credit；
 *   · 后台任务表下标 = ftl_bg_job_e，表序 = 提交序 = 执行优先级（唯一坐标）；
 *   · 损坏检测靠 item magic + workspace 指针绑定，同一 die 只报一次。
 *
 * 与其他模块：
 *   · 上游：l2p_cache（load / 写回）、checkpoint（dirty region）、host_io（host 组）；
 *   · 下游：各模块 handle / ftl_host_group_drain 启动 I/O；
 *   · FC 完成经 ftl_op_drain → ftl_sched_note_completion 进入本模块回收。
 */

#include "ftl_sched.h"

#include "assertion.h"
#include "bbt.h"
#include "checkpoint.h"
#include "field_mask.h"
#include "ftl.h"
#include "journal.h"
#include "l2p_cache.h"
#include "log.h"
#include "metadata.h"
#include "metadata_job.h"
#include "migrate.h"
#include "pending.h"
#include "platform.h" /* ssd_platform_get / time_us（后台单轮墙钟兜底） */
#include "read_recovery.h"
#include "ssd_compiler.h"

#include <string.h>

_Static_assert(sizeof(ftl_work_item_s) == 20U, "scheduler item 布局漂移（prefetch 标志改变了体积）");
_Static_assert(FTL_SCHED_HOST_MAX == FTL_GROUP_MAX, "host item 上限必须与 pending 组表一致");
_Static_assert(FTL_SCHED_HOST_READ_RESERVE < FTL_SCHED_HOST_MAX, "host 读保留量必须小于 host 上限");
_Static_assert(FTL_SCHED_MAX_WORK >= (FTL_SCHED_L2P_FLUSH_MAX + FTL_SCHED_L2P_LOAD_CREDIT + FTL_SCHED_L2P_PREFETCH_CREDIT + FTL_SCHED_HOST_MAX + FTL_BG_JOB_NUM), "scheduler item 池小于各 class 上限之和");
_Static_assert(FTL_SCHED_PRIO_HOST_READ < FTL_SCHED_PRIO_HOST_WRITE, "host 读必须先于写派发");
_Static_assert(FTL_SCHED_PRIO_HOST_WRITE + 1U == FTL_SCHED_PRIO_BG_BASE, "优先级表：后台 job 紧跟 host 写");
_Static_assert(FTL_SCHED_PRIO_BG_BASE + FTL_BG_JOB_NUM == FTL_SCHED_PRIO_L2P_FLUSH, "优先级表：L2P flush 在后台 job 之后");

/* 后台单轮墙钟异常兜底：预算任务累计耗时超过此值即截断（只在任务之间检查，不能抢占）。 */
#define BG_ROUND_HARD_LIMIT_US (4000U)

/* 档位预设：只列 budget 任务位（1U << job）；must-run 不看掩码。 */
#define BG_BUDGETED_ALL ((1U << FTL_BG_JOB_L2P_FLUSH) | (1U << FTL_BG_JOB_JOURNAL_FLUSH) | (1U << FTL_BG_JOB_BBT) | (1U << FTL_BG_JOB_CP_TRIGGER))

/* 统计表生成宏（表体在文件级变量区展开） */
#if SSD_STATS_ENABLE
#    define FTL_SCHED_STATS_LIST(X)                    \
        X(host_begin, STATS_U32, STATS_COUNTER)        \
        X(host_reaped, STATS_U32, STATS_COUNTER)       \
        X(load_begin, STATS_U32, STATS_COUNTER)        \
        X(load_complete, STATS_U32, STATS_COUNTER)     \
        X(prefetch_begin, STATS_U32, STATS_COUNTER)    \
        X(prefetch_complete, STATS_U32, STATS_COUNTER) \
        X(flush_request, STATS_U32, STATS_COUNTER)     \
        X(flush_reaped, STATS_U32, STATS_COUNTER)      \
        X(bg_request, STATS_U32, STATS_COUNTER)        \
        X(completion_note, STATS_U32, STATS_COUNTER)

#    define FTL_SCHED_STATS_NAME(field, width, kind) \
        {STATS_NAME_STR(field), STATS_OFFSET(ftl_sched_stats_s, field), width, kind},
#    define FTL_SCHED_STATS_CHECK(field, width, kind) \
        STATS_CHECK_FIELD(ftl_sched_stats_s, field, width);
#endif

/* 文件级变量 */
static ftl_work_item_s s_sched_items[FTL_DIE_MAX][FTL_SCHED_MAX_WORK] SRAM3_DATA;

static BOOL s_sched_corrupt_reported[FTL_DIE_MAX];

#if SSD_STATS_ENABLE
static const stats_name_s s_stats_sched_names[] = {
    FTL_SCHED_STATS_LIST(FTL_SCHED_STATS_NAME)};
#    undef FTL_SCHED_STATS_NAME

FTL_SCHED_STATS_LIST(FTL_SCHED_STATS_CHECK)
#    undef FTL_SCHED_STATS_CHECK
#endif

/* 后台任务表：下标 = ftl_bg_job_e；表序 = 提交序 = 执行优先级（唯一坐标）。 */
static const bg_task_s s_bg_tasks[FTL_BG_JOB_NUM] = {
    [FTL_BG_JOB_CP_ADVANCE]    = {.must_run = true}, /* 在飞 CP 跨轮推进，必须最先 */
    [FTL_BG_JOB_RECOVERY]      = {.must_run = true}, /* host 读在等结果，不推进即死等 */
    [FTL_BG_JOB_GC]            = {.must_run = true}, /* 写路径紧急 GC 在 BUSY 起，不推进即死锁 */
    [FTL_BG_JOB_METADATA]      = {.must_run = true}, /* 前台写路径置的压缩请求，不推进即永久 FTL_AGAIN */
    [FTL_BG_JOB_JOURNAL_ERASE] = {.must_run = true}, /* 新环形块没擦不能写，BUSY 也要补 */
    [FTL_BG_JOB_CP_EMERGENCY]  = {.must_run = true}, /* Journal ≥95%，任何档位随时响应 */
    [FTL_BG_JOB_L2P_FLUSH]     = {.units = 1U, .gated = true},
    [FTL_BG_JOB_JOURNAL_FLUSH] = {.units = 1U},
    [FTL_BG_JOB_BBT]           = {.units = 1U},
    [FTL_BG_JOB_CP_TRIGGER]    = {.units = 1U},
};

_Static_assert((sizeof(s_bg_tasks) / sizeof(s_bg_tasks[0])) == FTL_BG_JOB_NUM, "后台任务表项数必须等于 ftl_bg_job_e 枚举数");

/* 档位 → 每个 token interval 的 credit 配额；IDLE/NORMAL 放行全部预算任务，
 * BUSY 只放行 L2P 写回（must-run 不看掩码）。 */
static const bg_level_cfg_s s_bg_level_cfg[BG_LEVEL_NUM] = {
    [BG_LEVEL_IDLE]   = {.grant_units = 4U,              .task_mask = BG_BUDGETED_ALL},
    [BG_LEVEL_NORMAL] = {.grant_units = 4U,              .task_mask = BG_BUDGETED_ALL},
    [BG_LEVEL_BUSY]   = {.grant_units = 1U, .task_mask = (1U << FTL_BG_JOB_L2P_FLUSH)},
};

/* ── corrupt ── */

/**
 * @brief 报告一次本 die 的 scheduler item 表/字段损坏（每 die 只报一次）
 *
 * @param sched 调度器
 * @param what  损坏点
 */
static void sched_corrupt_report(const ftl_sched_s *sched, const char *what)
{
    const BYTE die = (sched->die < FTL_DIE_MAX) ? sched->die : 0U;

    if (s_sched_corrupt_reported[die] == false)
    {
        s_sched_corrupt_reported[die] = true;
        LOGE(LOG_TAG_FTL "sched corrupt: d=%hhu %s\r\n", die, what);
    }
}

/* ── workspace ── */

/**
 * @brief 取指定 die 的 item workspace（SRAM3）
 *
 * @param die 紧凑 die 号（调用者已校验 < FTL_DIE_MAX）
 * @return workspace 行
 */
static ftl_work_item_s *sched_workspace_get(BYTE die)
{
    return s_sched_items[die];
}

/* ── items ── */

/**
 * @brief item workspace 指针是否落在静态池内
 *
 * @param sched 调度器
 * @return true = 指针合法
 */
static BOOL sched_items_is_valid(const ftl_sched_s *sched)
{
    // 必须正好是本 die 那一行的起点：只查“落在池内”会把别的 die 的 workspace
    // 当成合法，掩盖绑定错误。
    return (sched->die < FTL_DIE_MAX) && (sched->items == s_sched_items[sched->die]);
}

/* ── class ── */

/**
 * @brief 某 class 的活动 item 数
 *
 * @param sched    调度器
 * @param class_id class
 * @return item 数
 */
static WORD sched_class_count(const ftl_sched_s *sched, ftl_work_class_e class_id)
{
    WORD n = 0U;

    for (WORD i = 0U; i < FTL_SCHED_MAX_WORK; i++)
    {
        const ftl_work_item_s *it = &sched->items[i];

        if ((it->state != FTL_WORK_FREE) && (it->class_id == class_id))
        {
            n++;
        }
    }
    return n;
}

/* ── bg ── */

/**
 * @brief 后台 job → class
 *
 * @param job 后台 job
 * @return class
 */
static ftl_work_class_e sched_bg_class(ftl_bg_job_e job)
{
    switch (job)
    {
        case FTL_BG_JOB_JOURNAL_ERASE:
        case FTL_BG_JOB_JOURNAL_FLUSH:
            return FTL_WORK_CLASS_JOURNAL;
        case FTL_BG_JOB_GC:
            return FTL_WORK_CLASS_GC;
        case FTL_BG_JOB_RECOVERY:
            return FTL_WORK_CLASS_RECOVERY;
        case FTL_BG_JOB_L2P_FLUSH:
            return FTL_WORK_CLASS_L2P_FLUSH;
        default:
            return FTL_WORK_CLASS_CP_META;
    }
}

/**
 * @brief 执行一个后台 job（由统一分派按优先级表调用）
 *
 * @param ftl FTL 实例
 * @param job 后台 job
 */
static void sched_bg_job_run(ftl_ctx_s *ftl, BYTE job)
{
    switch ((ftl_bg_job_e)job)
    {
        case FTL_BG_JOB_CP_ADVANCE:
        {
            const cp_trigger_s trigger = ftl_cp_trigger_build(ftl, CP_REASON_ADVANCE, false);

            cp_handle(ftl->cp_handle, &trigger);
            break;
        }
        case FTL_BG_JOB_CP_EMERGENCY:
        {
            const cp_trigger_s trigger = ftl_cp_trigger_build(ftl, CP_REASON_EMERGENCY, false);

            cp_handle(ftl->cp_handle, &trigger);
            break;
        }
        case FTL_BG_JOB_CP_TRIGGER:
        {
            const cp_trigger_s trigger = ftl_cp_trigger_build(ftl, CP_REASON_TIMER, false);

            cp_handle(ftl->cp_handle, &trigger);
            break;
        }
        case FTL_BG_JOB_METADATA:
            md_handle(ftl->metadata_dir);
            break;
        case FTL_BG_JOB_JOURNAL_ERASE:
        {
            const jrn_trigger_e trigger = JRN_TRIGGER_ERASE_ONLY;

            jrn_handle(ftl->journal, &trigger);
            break;
        }
        case FTL_BG_JOB_JOURNAL_FLUSH:
            jrn_handle(ftl->journal, NULL);
            break;
        case FTL_BG_JOB_RECOVERY:
            rr_handle(&ftl->read_recovery_handle);
            break;
        case FTL_BG_JOB_GC:
            mig_handle(ftl->mig_handle, ftl->sched.bg.level);
            break;
        case FTL_BG_JOB_L2P_FLUSH:
            l2p_handle(ftl->cache, topo_2_die(ftl->topo), ftl->sched.bg.level);
            break;
        case FTL_BG_JOB_BBT:
            bbt_handle(&ftl->alloc);
            break;
        default:
            ASSERT_DEFAULT(ftl_bg_job_e);
            break;
    }
}

/**
 * @brief 提交一个后台 job 请求（幂等；后台策略只提交请求）
 *
 * 后台 job 使用预留 item，不会因池满失败。
 *
 * @param sched 调度器
 * @param job   后台 job
 */
void ftl_sched_bg_request(ftl_sched_s *sched, ftl_bg_job_e job)
{
    ASSERT(sched != NULL);
    ASSERT(job < FTL_BG_JOB_NUM);

    ftl_work_item_s *it = &sched->items[job];

    if (it->state != FTL_WORK_FREE)
    {
        return; /* 本轮已登记 / 已执行前重复请求 */
    }

    it->state    = FTL_WORK_READY;
    it->dep_mask = 0U;
    it->region   = 0U;
    it->group    = 0U;
    it->prefetch = false;
    it->magic    = FTL_SCHED_ITEM_MAGIC;
    STATS_INC(sched->stats.bg_request);
}

/**
 * @brief 负载 → 后台档位（纯函数）
 *
 * pending 与组计数全空 ⇒ IDLE；pending 过半池或有未决组 ⇒ BUSY；其余 NORMAL。
 *
 * @param ftl FTL 实例（只读 pending）
 * @return 档位（< BG_LEVEL_NUM）
 */
bg_level_e ftl_sched_bg_level_select(const ftl_ctx_s *ftl)
{
    const WORD pending = pend_slot_count(&ftl->pending);
    const WORD queued  = pend_group_count(&ftl->pending);

    if ((pending == 0U) && (queued == 0U))
    {
        return BG_LEVEL_IDLE;
    }
    if ((pending > (FTL_PENDING_MAX / 2U)) || (queued != 0U))
    {
        return BG_LEVEL_BUSY;
    }
    return BG_LEVEL_NORMAL;
}

/**
 * @brief 补充预算 credit（timer 节拍调用）
 *
 * 按本轮档位 grant_units 补充，上限 grant_units × BG_TOKEN_CAP_TICKS；
 * cfg 尚未发布时不补。
 *
 * @param sched 调度器
 */
void ftl_sched_bg_refill(ftl_sched_s *sched)
{
    ASSERT(sched != NULL);

    if (sched->bg.cfg == NULL)
    {
        return;
    }

    const DWORD grant = sched->bg.cfg->grant_units;
    const DWORD cap   = grant * BG_TOKEN_CAP_TICKS;

    if (sched->bg.credits >= (cap - grant))
    {
        sched->bg.credits = cap;
    }
    else
    {
        sched->bg.credits += grant;
    }
}

/**
 * @brief 选档并发布本轮配置（模块入口读 sched.bg.level，不各自重选）
 *
 * @param ftl FTL 实例
 */
static void sched_bg_publish(ftl_ctx_s *ftl)
{
    const bg_level_e level = ftl_sched_bg_level_select(ftl);

    ASSERT(level < BG_LEVEL_NUM);
    ftl->sched.bg.level = level;
    ftl->sched.bg.cfg   = &s_bg_level_cfg[level];
}

/**
 * @brief 提交一轮后台任务：先 must-run，再在 token bucket 内按表序提交预算任务
 *
 * 本函数只**提交**；执行顺序由 ftl_sched_dispatch 按 FTL_SCHED_PRIO_E 决定。
 *
 * @param ftl FTL 实例（sched.bg.cfg 已由 sched_bg_publish 发布）
 */
static void sched_bg_submit(ftl_ctx_s *ftl)
{
    ftl_sched_s *sched = &ftl->sched;

    ASSERT(sched->bg.cfg != NULL);

    // must-run：正确性关键集（不 gate、不占预算）。
    for (BYTE job = 0U; job < FTL_BG_JOB_NUM; job++)
    {
        if (s_bg_tasks[job].must_run)
        {
            ftl_sched_bg_request(sched, (ftl_bg_job_e)job);
        }
    }

    // 预算任务：表序 + token bucket；credit 不足截断（表序即优先级）。
    const ssd_platform_ops_s *platform   = ssd_platform_get();
    const BOOL                clock_ok   = (platform != NULL) && (platform->time_us != NULL);
    const QWORD               start_us   = (clock_ok == true) ? platform->time_us() : 0U;
    DWORD                     units_left = sched->bg.credits;

    for (BYTE job = 0U; job < FTL_BG_JOB_NUM; job++)
    {
        const bg_task_s *task = &s_bg_tasks[job];

        if (task->must_run)
        {
            continue;
        }
        if ((sched->bg.cfg->task_mask & (1U << job)) == 0U)
        {
            continue; /* 本档位不跑该任务 */
        }
        if ((task->gated != false) && (ftl->l2p_flush_due == 0U))
        {
            continue; /* 节拍未到：不提交、不占 credit */
        }
        if (task->units > units_left)
        {
            break; /* 配额不足：截断（表序即优先级） */
        }

        if (task->gated != false)
        {
            ftl->l2p_flush_due = 0U; /* 只消费一次：下一次由 timer 重新置位 */
        }
        ftl_sched_bg_request(sched, (ftl_bg_job_e)job);
        units_left -= task->units;

        // 异常兜底：单轮墙钟超限即截断；时钟缺失时失效。
        if ((clock_ok == true) && ((platform->time_us() - start_us) >= BG_ROUND_HARD_LIMIT_US))
        {
            break;
        }
    }

    // 只把实际消费后的余额写回：被掩码跳过 / 未到节拍 / credit 不足的任务不扣。
    sched->bg.credits = units_left;
}

/**
 * @brief 绑定内置任务表并复位档位 / credit
 *
 * 必须在 ftl_sched_init 之后调用（init 会 memset 整个 sched）。
 *
 * @param sched 调度器
 */
void ftl_sched_bg_init(ftl_sched_s *sched)
{
    ASSERT(sched != NULL);

    sched->bg.tasks      = s_bg_tasks;
    sched->bg.task_count = FTL_BG_JOB_NUM;
    sched->bg.cfg        = NULL;
    sched->bg.level      = BG_LEVEL_IDLE;
    sched->bg.credits    = 0U;
}

#if defined(FC_NAND_TEST_ENABLE)
/**
 * @brief 测试钩子：只做选档 + 发布 cfg，不提交任务
 *
 * @param ftl FTL 实例
 */
void ftl_sched_bg_test_publish(ftl_ctx_s *ftl)
{
    sched_bg_publish(ftl);
}
#endif

/* ── item ── */

/**
 * @brief 取一个空闲 item
 *
 * @param sched 调度器
 * @return item；池满返回 NULL
 */
static ftl_work_item_s *sched_item_free(ftl_sched_s *sched)
{
    // 前 FTL_BG_JOB_NUM 个是后台 job 的永久预留项，不参与普通 item 分配。
    for (WORD i = FTL_BG_JOB_NUM; i < FTL_SCHED_MAX_WORK; i++)
    {
        if (sched->items[i].state == FTL_WORK_FREE)
        {
            return &sched->items[i];
        }
    }
    return NULL;
}

/**
 * @brief 释放一个 item（唯一出口：同步维护 host/flush 计数）
 *
 * @param sched 调度器
 * @param it    非 NULL item
 */
static void sched_item_release(ftl_sched_s *sched, ftl_work_item_s *it)
{
    if (it->class_id == FTL_WORK_CLASS_HOST_READ || it->class_id == FTL_WORK_CLASS_HOST_WRITE)
    {
        ASSERT(sched->host_items > 0U);
        sched->host_items--;
    }
    else if ((it->class_id == FTL_WORK_CLASS_L2P_FLUSH) && (it->bg_job == false))
    {
        ASSERT(sched->flush_items > 0U);
        sched->flush_items--;
    }
    it->state = FTL_WORK_FREE;
}

/**
 * @brief 回收已完成 item
 *
 * @param ftl   FTL 实例
 * @param sched 调度器
 */
static void sched_item_reap(ftl_ctx_s *ftl, ftl_sched_s *sched)
{
    if (sched->items == NULL)
    {
        return; /* 启动早期 completion：scheduler 尚未 init */
    }
    if (sched_items_is_valid(sched) == false)
    {
        sched_corrupt_report(sched, "flush reap: items pointer");
        return;
    }
    if (sched->flush_items == 0U)
    {
        return;
    }

    for (WORD i = 0U; i < FTL_SCHED_MAX_WORK; i++)
    {
        ftl_work_item_s *it = &sched->items[i];

        if ((it->state == FTL_WORK_FREE) || (it->class_id != FTL_WORK_CLASS_L2P_FLUSH) || (it->bg_job != false))
        {
            continue;
        }
        if (it->magic != FTL_SCHED_ITEM_MAGIC)
        {
            sched_corrupt_report(sched, "flush reap: item magic");
            sched_item_release(sched, it);
            continue;
        }

        if (l2p_region_dirty_live(ftl->cache, it->region) == false)
        {
            if (it->state == FTL_WORK_INFLIGHT)
            {
                ASSERT(sched->credit[it->class_id] > 0U);
                sched->credit[it->class_id]--;
            }
            sched_item_release(sched, it);
            STATS_INC(sched->stats.flush_reaped);
            continue;
        }

        // 只有 INFLIGHT 才需要查行状态；READY/WAIT_DEP 查了也没用，且是一次
        // 64 行扫描。完成钩子每个完成都会走到这里。
        if ((it->state == FTL_WORK_INFLIGHT) && (l2p_region_state_get(ftl->cache, it->region) != L2P_CACHE_FLUSHING))
        {
            /* 写回命令已结束但 region 仍 dirty（例如刚完成的是加载）：退回 READY
             * 让本轮重新 kick，不占 class credit。 */
            ASSERT(sched->credit[it->class_id] > 0U);
            sched->credit[it->class_id]--;
            it->state = FTL_WORK_READY;
            continue;
        }

        if ((it->state == FTL_WORK_WAIT_DEP) && (SSD_FLAGS_ANY(it->dep_mask, FTL_WORK_DEP_MD_SLOT) != false) && (md_handle_is_idle(ftl->metadata_dir) != false) && (ftl->metadata_dir->handle.compact_wanted == false))
        {
            it->dep_mask = 0U;
            it->state    = FTL_WORK_READY;
        }
    }
}

/* ── l2p ── */

/**
 * @brief 提交一个 L2P dirty-region 写回请求（幂等）
 *
 * @param sched  调度器
 * @param region 全局 region
 * @return true 已登记；false 静态池满（CP 下一轮重试）
 */
BOOL ftl_sched_l2p_flush_request(ftl_sched_s *sched, DWORD region)
{
    ASSERT(sched != NULL);
    ASSERT(region < L2P_GLOBAL_REGIONS);

    for (WORD i = 0U; i < FTL_SCHED_MAX_WORK; i++)
    {
        const ftl_work_item_s *it = &sched->items[i];

        if ((it->state != FTL_WORK_FREE) && (it->class_id == FTL_WORK_CLASS_L2P_FLUSH) && (it->bg_job == false) && (it->region == region))
        {
            return true; /* 幂等：已在 READY/WAIT_DEP/INFLIGHT */
        }
    }

    if (sched->flush_items >= FTL_SCHED_L2P_FLUSH_MAX)
    {
        return false;
    }

    ftl_work_item_s *it = sched_item_free(sched);

    if (it == NULL)
    {
        return false;
    }

    it->region   = region;
    it->class_id = FTL_WORK_CLASS_L2P_FLUSH;
    it->state    = FTL_WORK_READY;
    it->dep_mask = 0U;
    it->bg_job   = false;
    it->prefetch = false;
    it->magic    = FTL_SCHED_ITEM_MAGIC;
    sched->flush_items++;
    STATS_INC(sched->stats.flush_request);
    return true;
}

/**
 * @brief 登记一个 L2P_LOAD（已由 L2P 层启动）
 *
 * demand 与预取分开计 credit：预取不能占用 demand 的保底额度。
 *
 * @param sched    调度器
 * @param region   全局 region
 * @param prefetch true = 预取加载
 * @return true 已登记；false class credit / 静态池满
 */
BOOL ftl_sched_l2p_load_begin(ftl_sched_s *sched, DWORD region, BOOL prefetch)
{
    ASSERT(sched != NULL);
    ASSERT(region < L2P_GLOBAL_REGIONS);

    for (WORD i = 0U; i < FTL_SCHED_MAX_WORK; i++)
    {
        const ftl_work_item_s *it = &sched->items[i];

        if ((it->state != FTL_WORK_FREE) && (it->class_id == FTL_WORK_CLASS_L2P_LOAD) && (it->region == region) &&
            (it->prefetch == prefetch))
        {
            return true; /* 同一 region 的重试沿用同一 item */
        }
    }

    if (prefetch != false)
    {
        if (sched->prefetch_items >= FTL_SCHED_L2P_PREFETCH_CREDIT)
        {
            return false;
        }
    }
    else if (sched->credit[FTL_WORK_CLASS_L2P_LOAD] >= FTL_SCHED_L2P_LOAD_CREDIT)
    {
        return false;
    }

    ftl_work_item_s *it = sched_item_free(sched);

    if (it == NULL)
    {
        return false;
    }

    it->region   = region;
    it->class_id = FTL_WORK_CLASS_L2P_LOAD;
    it->state    = FTL_WORK_INFLIGHT;
    it->dep_mask = 0U;
    it->bg_job   = false;
    it->prefetch = prefetch;
    it->magic    = FTL_SCHED_ITEM_MAGIC;
    if (prefetch != false)
    {
        sched->prefetch_items++;
        STATS_INC(sched->stats.prefetch_begin);
    }
    else
    {
        sched->credit[FTL_WORK_CLASS_L2P_LOAD]++;
        STATS_INC(sched->stats.load_begin);
    }
    return true;
}

/**
 * @brief 结束一个 L2P_LOAD
 *
 * @param sched    调度器
 * @param region   全局 region
 * @param prefetch true = 预取加载（必须与 begin 时一致）
 */
void ftl_sched_l2p_load_finish(ftl_sched_s *sched, DWORD region, BOOL prefetch)
{
    ASSERT(sched != NULL);

    for (WORD i = 0U; i < FTL_SCHED_MAX_WORK; i++)
    {
        ftl_work_item_s *it = &sched->items[i];

        if ((it->state != FTL_WORK_FREE) && (it->class_id == FTL_WORK_CLASS_L2P_LOAD) && (it->region == region) &&
            (it->prefetch == prefetch))
        {
            if (prefetch != false)
            {
                ASSERT(sched->prefetch_items > 0U);
                sched->prefetch_items--;
                STATS_INC(sched->stats.prefetch_complete);
            }
            else
            {
                ASSERT(sched->credit[FTL_WORK_CLASS_L2P_LOAD] > 0U);
                sched->credit[FTL_WORK_CLASS_L2P_LOAD]--;
                STATS_INC(sched->stats.load_complete);
            }
            sched_item_release(sched, it);
            return;
        }
    }

    /* 没有对应 item：credit/计数会永久少还一份，属于静默泄漏。 */
    sched_corrupt_report(sched, "load complete: item missing");
}

/**
 * @brief 是否仍有活动 L2P flush item
 *
 * @param sched 调度器
 * @return true 仍有 READY / WAIT_DEP / INFLIGHT item
 */
BOOL ftl_sched_l2p_flush_is_pending(const ftl_sched_s *sched)
{
    ASSERT(sched != NULL);

    return sched->flush_items != 0U;
}

/**
 * @brief 清空 L2P_FLUSH 登记（新 CP job 起点；保留在飞 L2P_LOAD）
 *
 * @param sched 调度器
 */
void ftl_sched_l2p_flush_reset(ftl_sched_s *sched)
{
    ASSERT(sched != NULL);

    for (WORD i = 0U; i < FTL_SCHED_MAX_WORK; i++)
    {
        ftl_work_item_s *it = &sched->items[i];

        // FREE item 的 class_id 是上次使用的残留；必须先判 state，否则会把
        // 已释放的 item 再减一次 flush_items（release 下静默回绕）。
        if ((it->state != FTL_WORK_FREE) && (it->class_id == FTL_WORK_CLASS_L2P_FLUSH) && (it->bg_job == false))
        {
            sched_item_release(sched, it);
        }
    }
    ASSERT(sched->flush_items == 0U);
    sched->credit[FTL_WORK_CLASS_L2P_FLUSH] = 0U;
    sched->l2p_flush_failed                 = false;
}

/* ── host ── */

/**
 * @brief 登记一个 host 完成组（同一 group 幂等）
 *
 * @param sched 调度器
 * @param group pending 组下标
 * @param is_read true = HOST_READ；false = HOST_WRITE
 * @return true 已登记；false item 池 / host 上限满
 */
BOOL ftl_sched_host_begin(ftl_sched_s *sched, WORD group, BOOL is_read)
{
    ASSERT(sched != NULL);
    ASSERT(group < FTL_GROUP_MAX);

    for (WORD i = 0U; i < FTL_SCHED_MAX_WORK; i++)
    {
        const ftl_work_item_s *it = &sched->items[i];

        if ((it->state != FTL_WORK_FREE) && ((it->class_id == FTL_WORK_CLASS_HOST_READ) || (it->class_id == FTL_WORK_CLASS_HOST_WRITE)) &&
            (it->group == group))
        {
            return true;
        }
    }

    if (sched->host_items >= FTL_SCHED_HOST_MAX)
    {
        return false;
    }
    if ((is_read == false) && (sched_class_count(sched, FTL_WORK_CLASS_HOST_WRITE) >= (FTL_SCHED_HOST_MAX - FTL_SCHED_HOST_READ_RESERVE)))
    {
        return false; /* 写组不得吃光读保留槽 */
    }

    ftl_work_item_s *it = sched_item_free(sched);

    if (it == NULL)
    {
        return false;
    }

    it->group    = group;
    it->class_id = (is_read != false) ? FTL_WORK_CLASS_HOST_READ : FTL_WORK_CLASS_HOST_WRITE;
    it->state    = FTL_WORK_READY;
    it->dep_mask = 0U;
    it->bg_job   = false;
    it->prefetch = false;
    it->magic    = FTL_SCHED_ITEM_MAGIC;
    it->region   = 0U;
    sched->host_items++;
    STATS_INC(sched->stats.host_begin);
    return true;
}

/**
 * @brief host 写启动许可
 *
 * 读路径不调用。CMT dirty 达到主动写回水位，或 free 降到紧急水位时暂缓新
 * host 写；读仍可提交。
 *
 * @param ftl FTL 实例
 * @return true 允许；false 暂缓
 */
BOOL ftl_sched_host_write_is_admitted(const ftl_ctx_s *ftl)
{
    ASSERT(ftl != NULL);

    if (ftl->cache->stats.cmt_dirty >= L2P_CMT_WB_WATERMARK)
    {
        return false;
    }
    if (ftl->alloc.free_count < FREE_POOL_EMERGENCY)
    {
        return false;
    }
    return true;
}

/**
 * @brief 只推进一个 host 组（提交快路径；不扫其它组）
 *
 * 提交快路径只 kick 本组，避免一次 submit 顺带推进所有组的 stall 计数
 * （无时间流逝却判卡死）；服务循环仍用 ftl_sched_run_host 批量派发。
 *
 * @param ftl   FTL 实例
 * @param group pending 组下标
 */
void ftl_sched_host_kick(ftl_ctx_s *ftl, WORD group)
{
    ASSERT(ftl != NULL);
    ASSERT(group < FTL_GROUP_MAX);

    ftl_sched_s *sched = &ftl->sched;

    if (sched->items == NULL)
    {
        return;
    }
    if (sched_items_is_valid(sched) == false)
    {
        sched_corrupt_report(sched, "host kick: items pointer");
        return;
    }

    for (WORD i = 0U; i < FTL_SCHED_MAX_WORK; i++)
    {
        ftl_work_item_s *it = &sched->items[i];

        if ((it->state != FTL_WORK_READY) || (it->class_id != FTL_WORK_CLASS_HOST_READ && it->class_id != FTL_WORK_CLASS_HOST_WRITE) || (it->group != group))
        {
            continue;
        }
        if ((it->magic != FTL_SCHED_ITEM_MAGIC) || (it->group >= FTL_GROUP_MAX))
        {
            sched_corrupt_report(sched, "host kick: item magic/group");
            sched_item_release(sched, it);
            return;
        }

        ftl_host_group_drain(ftl, it->group);

        if (pend_group_is_used(&ftl->pending, it->group) == false)
        {
            sched_item_release(sched, it);
            STATS_INC(sched->stats.host_reaped);
        }
        return;
    }
}

/**
 * @brief 回收已完成 host 组的 item（提交新组前调用，防止 group 槽复用）
 *
 * @param ftl FTL 实例
 */
void ftl_sched_host_reap(ftl_ctx_s *ftl)
{
    ASSERT(ftl != NULL);

    ftl_sched_s *sched = &ftl->sched;

    // 启动早期（ftl_init 的 bbt/metadata 初始化）就会抽完成表；
    // 此时 scheduler 尚未 init，items == NULL 是正常状态，不能当损坏。
    if (sched->items == NULL)
    {
        return;
    }
    if (sched_items_is_valid(sched) == false)
    {
        sched_corrupt_report(sched, "host reap: items pointer");
        return;
    }
    if (sched->host_items == 0U)
    {
        return;
    }

    for (WORD i = 0U; i < FTL_SCHED_MAX_WORK; i++)
    {
        ftl_work_item_s *it = &sched->items[i];

        if ((it->state == FTL_WORK_FREE) || ((it->class_id != FTL_WORK_CLASS_HOST_READ) && (it->class_id != FTL_WORK_CLASS_HOST_WRITE)))
        {
            continue;
        }
        if ((it->magic != FTL_SCHED_ITEM_MAGIC) || (it->group >= FTL_GROUP_MAX))
        {
            sched_corrupt_report(sched, "host reap: item magic/group");
            sched_item_release(sched, it);
            continue;
        }
        if (pend_group_is_used(&ftl->pending, it->group) == false)
        {
            sched_item_release(sched, it);
            STATS_INC(sched->stats.host_reaped);
        }
    }
}

/* ── note ── */

/**
 * @brief FC 完成通知：回收已完成 item 并唤醒 MD_SLOT 依赖
 *
 * 由 ftl_op_drain 在模块完成回调返回后调用；它是所有 FC 完成进入 scheduler 的
 * 唯一钩子，不替代模块自己的数据/状态收尾。
 *
 * @param ftl FTL 实例
 */
void ftl_sched_note_completion(ftl_ctx_s *ftl)
{
    ASSERT(ftl != NULL);

    STATS_INC(ftl->sched.stats.completion_note);
    ftl_sched_host_reap(ftl);
    sched_item_reap(ftl, &ftl->sched);
}

/* ── dispatch ── */

/**
 * @brief 分派一个 host class 的 READY item（调用者已持 host_running）
 *
 * @param ftl FTL 实例
 * @param cls HOST_READ 或 HOST_WRITE
 */
static void sched_dispatch_host_class(ftl_ctx_s *ftl, ftl_work_class_e cls)
{
    ftl_sched_s *sched = &ftl->sched;

    for (WORD i = 0U; i < FTL_SCHED_MAX_WORK; i++)
    {
        ftl_work_item_s *it = &sched->items[i];

        if ((it->state != FTL_WORK_READY) || (it->class_id != cls))
        {
            continue;
        }
        if ((it->magic != FTL_SCHED_ITEM_MAGIC) || (it->group >= FTL_GROUP_MAX))
        {
            sched_corrupt_report(sched, "host dispatch: item magic/group");
            sched_item_release(sched, it);
            continue;
        }

        ftl_host_group_drain(ftl, it->group);

        if (pend_group_is_used(&ftl->pending, it->group) == false)
        {
            sched_item_release(sched, it);
            STATS_INC(sched->stats.host_reaped);
        }
    }
}

/**
 * @brief host 阶段分派：读先于写，带重入保护
 *
 * @param ftl FTL 实例
 */
static void sched_dispatch_host(ftl_ctx_s *ftl)
{
    ftl_sched_s *sched = &ftl->sched;

    if ((sched->host_running != false) || (sched->host_items == 0U))
    {
        return; /* 重入 submit：外层调度继续；无 host item：不扫表 */
    }

    sched->host_running = true;
    ftl_sched_host_reap(ftl);
    sched_dispatch_host_class(ftl, FTL_WORK_CLASS_HOST_READ);
    sched_dispatch_host_class(ftl, FTL_WORK_CLASS_HOST_WRITE);
    sched->host_running = false;
}

/**
 * @brief 分派 READY 的 L2P_FLUSH item（调用者已检查 bg_suspend）
 *
 * @param ftl FTL 实例
 */
static void sched_dispatch_flush(ftl_ctx_s *ftl)
{
    ftl_sched_s *sched    = &ftl->sched;
    BYTE         attempts = 0U;

    for (WORD i = 0U; i < FTL_SCHED_MAX_WORK; i++)
    {
        ftl_work_item_s *it = &sched->items[i];

        if ((it->state != FTL_WORK_READY) || (it->class_id != FTL_WORK_CLASS_L2P_FLUSH) || (it->bg_job != false))
        {
            continue;
        }
        if (it->magic != FTL_SCHED_ITEM_MAGIC)
        {
            sched_corrupt_report(sched, "flush run: item magic");
            sched_item_release(sched, it);
            continue;
        }
        if (sched->credit[it->class_id] >= L2P_FLUSH_MAX_INFLIGHT)
        {
            break; /* 本 class 在飞额度已满；等完成回收 */
        }
        const l2p_cache_state_e state = l2p_region_state_get(ftl->cache, it->region);

        if (state == L2P_CACHE_FLUSHING)
        {
            /* 写回已在飞（可能由本 scheduler 或 owner flush 启动）：纳入 class credit。 */
            it->state = FTL_WORK_INFLIGHT;
            sched->credit[it->class_id]++;
            continue;
        }
        if (state == L2P_CACHE_LOADING)
        {
            continue; /* region 页加载中：完成后再 kick flush */
        }
        if (attempts >= L2P_FLUSH_MAX_NORMAL)
        {
            break; /* 本轮预算已用完；留下的 item 下一轮继续 */
        }

        const ftl_status_e st = l2p_region_flush_kick(ftl->cache, it->region);

        attempts++;

        if (st == FTL_ERROR)
        {
            sched->l2p_flush_failed = true;
            sched_item_release(sched, it);
            STATS_INC(sched->stats.flush_reaped);
            continue;
        }
        if ((st == FTL_OK) && (l2p_region_dirty_live(ftl->cache, it->region) == false))
        {
            sched_item_release(sched, it);
            STATS_INC(sched->stats.flush_reaped);
            continue;
        }
        if (st == FTL_AGAIN)
        {
            if ((md_handle_is_idle(ftl->metadata_dir) == false) || (ftl->metadata_dir->handle.compact_wanted != false))
            {
                it->dep_mask = FTL_WORK_DEP_MD_SLOT;
                it->state    = FTL_WORK_WAIT_DEP;
            }
            continue; /* 其它背压：保持 READY，下一轮重试 */
        }

        if (l2p_region_state_get(ftl->cache, it->region) == L2P_CACHE_FLUSHING)
        {
            it->state = FTL_WORK_INFLIGHT;
            sched->credit[it->class_id]++;
        }
        else
        {
            it->state = FTL_WORK_READY; /* 本次 kick 只起了加载/合并，flush 尚未在飞 */
        }
    }
}

/* ── stats ── */

#if SSD_STATS_ENABLE
/**
 * @brief 注册 scheduler 计数器（`stats ftl.sched`）
 *
 * @param sched 调度器
 * @param inst  实例号
 */
void ftl_sched_stats_register(ftl_sched_s *sched, BYTE inst)
{
    ASSERT(sched != NULL);

    (void)stats_register("ftl.sched", inst, &sched->stats, s_stats_sched_names, STATS_ENTRY_COUNT(s_stats_sched_names));
}
#endif

/* ── init ── */

/**
 * @brief 初始化本 die 的调度器（绑定 workspace、清空 item / credit）
 *
 * 前 FTL_BG_JOB_NUM 个 item 永久预留给后台 job，避免每轮分配/回收。
 *
 * @param sched 调度器（非空）
 * @param die   紧凑 die 号；决定使用哪一行静态 workspace
 */
void ftl_sched_init(ftl_sched_s *sched, BYTE die)
{
    ASSERT(sched != NULL);
    ASSERT(die < FTL_DIE_MAX);

    ftl_work_item_s *items = sched_workspace_get(die);

    memset(sched, 0, sizeof(*sched));
    sched->die   = die;
    sched->items = items;
    memset(items, 0, FTL_SCHED_MAX_WORK * sizeof(*items));

    for (BYTE job = 0U; job < FTL_BG_JOB_NUM; job++)
    {
        items[job].job      = job;
        items[job].class_id = sched_bg_class((ftl_bg_job_e)job);
        items[job].bg_job   = true;
        items[job].state    = FTL_WORK_FREE;
        items[job].magic    = FTL_SCHED_ITEM_MAGIC;
    }
}

/* ── run ── */

/**
 * @brief host 调度一步：回收已完成的组，读组先于写组补发
 *
 * 提交快路径专用：只碰 host item，不执行后台 job。
 *
 * @param ftl FTL 实例
 */
void ftl_sched_run_host(ftl_ctx_s *ftl)
{
    ASSERT(ftl != NULL);

    ftl_sched_s *sched = &ftl->sched;

    if (sched->items == NULL)
    {
        return;
    }
    if (sched_items_is_valid(sched) == false)
    {
        sched_corrupt_report(sched, "host run: items pointer");
        return;
    }

    sched_dispatch_host(ftl);
}

/* ── debug ── */

/**
 * @brief 打印本实例 scheduler 现场（CLI `ftl_ctx sched` / 断言终端）
 *
 * @param ftl FTL 实例（只读）
 * @param die 实例号（打印用）
 */
void ftl_sched_debug_dump(const ftl_ctx_s *ftl, BYTE die)
{
    ASSERT(ftl != NULL);

    const ftl_sched_s *sched  = &ftl->sched;
    WORD               free_n = 0U;
    WORD               wait_n = 0U;
    WORD               bg_n   = 0U;

    for (WORD i = 0U; i < FTL_SCHED_MAX_WORK; i++)
    {
        if (sched->items[i].state == FTL_WORK_FREE)
        {
            free_n++;
            continue;
        }
        if (sched->items[i].state == FTL_WORK_WAIT_DEP)
        {
            wait_n++;
        }
        if (sched->items[i].bg_job != false)
        {
            bg_n++;
        }
    }

    LOGI(LOG_TAG_FTL "DBG d=%hhu sched free=%hu used=%hu wr=%hu rd=%hu load=%hu pf=%hu flush=%hu cp=%hu jrn=%hu gc=%hu rec=%hu bg=%hu wait=%hu credit_ld=%lu credit_fl=%lu host_run=%d flush_fail=%d\r\n",
         die,
         free_n,
         FTL_SCHED_MAX_WORK - free_n,
         sched_class_count(sched, FTL_WORK_CLASS_HOST_WRITE),
         sched_class_count(sched, FTL_WORK_CLASS_HOST_READ),
         sched_class_count(sched, FTL_WORK_CLASS_L2P_LOAD),
         sched->prefetch_items,
         sched->flush_items,
         sched_class_count(sched, FTL_WORK_CLASS_CP_META),
         sched_class_count(sched, FTL_WORK_CLASS_JOURNAL),
         sched_class_count(sched, FTL_WORK_CLASS_GC),
         sched_class_count(sched, FTL_WORK_CLASS_RECOVERY),
         bg_n,
         wait_n,
         sched->credit[FTL_WORK_CLASS_L2P_LOAD],
         sched->credit[FTL_WORK_CLASS_L2P_FLUSH],
         sched->host_running,
         sched->l2p_flush_failed);

#if SSD_STATS_ENABLE
    LOGI(LOG_TAG_FTL "DBG d=%hhu sched stats host=%lu/%lu load=%lu/%lu pf=%lu/%lu flush=%lu/%lu bg=%lu comp=%lu\r\n",
         die,
         sched->stats.host_begin,
         sched->stats.host_reaped,
         sched->stats.load_begin,
         sched->stats.load_complete,
         sched->stats.prefetch_begin,
         sched->stats.prefetch_complete,
         sched->stats.flush_request,
         sched->stats.flush_reaped,
         sched->stats.bg_request,
         sched->stats.completion_note);
#endif
}

/* ── dispatch（主入口） ── */

/**
 * @brief 唯一仲裁入口：host 读 → host 写 → 后台 job → L2P flush
 *
 * 后台 job 的先后由 `FTL_SCHED_PRIO_E` 决定（`ftl_bg_job_e` 表序）；本入口先做
 * 后台策略（选档 + token bucket + 按表序提交），再执行。host 阶段保留重入保护。
 *
 * @param ftl FTL 实例
 */
void ftl_sched_dispatch(ftl_ctx_s *ftl)
{
    ASSERT(ftl != NULL);

    ftl_sched_s *sched = &ftl->sched;

    if (sched->items == NULL)
    {
        return;
    }
    if (sched_items_is_valid(sched) == false)
    {
        sched_corrupt_report(sched, "dispatch: items pointer");
        return;
    }

    // 后台策略：选档 → 发布 cfg → 按表序提交（只提交，执行在下面）。
    if (ftl->flags.bg_suspend == false)
    {
        sched_bg_publish(ftl);
        sched_bg_submit(ftl);
    }

    // host 阶段：读先于写（无 host item / 重入时 helper 内部直接返回）。
    sched_dispatch_host(ftl);

    if (ftl->flags.bg_suspend != false)
    {
        return; /* 与后台策略同口径：暂停后台时不执行后台 job / L2P 写回 */
    }

    // 后台 job：job 下标 = 优先级 - BG_BASE，天然按表序；只执行本轮已提交的 READY 项。
    for (BYTE prio = FTL_SCHED_PRIO_BG_BASE; prio < FTL_SCHED_PRIO_L2P_FLUSH; prio++)
    {
        const BYTE       job = prio - FTL_SCHED_PRIO_BG_BASE;
        ftl_work_item_s *it  = &sched->items[job];

        if (it->state != FTL_WORK_READY)
        {
            continue;
        }
        if (it->magic != FTL_SCHED_ITEM_MAGIC)
        {
            sched_corrupt_report(sched, "bg dispatch: item magic");
            it->state = FTL_WORK_FREE;
            continue;
        }

        sched_bg_job_run(ftl, job);
        it->state = FTL_WORK_FREE;
    }

    // 普通 L2P_FLUSH item 在后台 job 之后。
    if (sched->flush_items != 0U)
    {
        sched_item_reap(ftl, sched);
        sched_dispatch_flush(ftl);
    }
}

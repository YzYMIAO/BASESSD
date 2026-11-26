#ifndef FTL_WBUF_H
#define FTL_WBUF_H

/*
 * wbuf.h — 页打包写缓冲（page-packing write buffer）
 *
 * CW 级映射下子操作常只覆盖半个 pair 窗口（如一次 512B 写只落 plane0 的 CW0），
 * 而器件**按页编程**（参数页 Byte86-89 部分页单位 = 1 CW；Byte110 每 ECC user area
 * 只能编一次）。逐子操作分配页会让每次写都吃掉一整对页 ⇒ 利用率 12.5%~50%。
 *
 * 本缓冲把**同一 pair 窗口**内的多次 host 写攒成一页，窗口填满 / 切到别的窗口 /
 * 显式 flush / 空闲时**一次编程**（两 plane 都有数据即一条 multi-plane）⇒ 写指针
 * 按页推进，页内利用率回到 100%。
 *
 * data[slot] 的 slot = plane*CW_NUM + cw（与 LBA 低 3 位同构，见 ftl_lba_pair_slot），
 * occupied 逐位标记有效槽。owners 记录「哪些组向本页贡献过」——一页可能由多个组拼成，
 * 完成时必须**逐组**递减在飞数（见 pend_group_subop_finish_multi）。
 *
 * ⚠ 掉电语义（无电容）：缓冲里的数据**不落 journal/L2P**——只有 flush 编程成功的
 *   completion 才确认映射（与既有顺序一致）。掉电时这些写组仍 outstanding（从未完成），
 *   丢失即正确契约；不变量「L2P/journal 只引用已编程页」由构造保证。
 *
 * ⚠ 不变量：`occupied` 清空（TRIM 掉光 / 显式复位）时**必须**退掉 `owners` 的在飞数，
 *   否则那些组永不完成（host 写挂死）。该动作收在 DROP / RESET 触发内部，
 *   **不**由调用者手工维护（见 CLAUDE.md §3.1）。
 *
 * ⚠ 模块边界（docs/archive/ftl_module_contracts.md §4.8）：wbuf 不持有 ftl_context；
 *   只持有 allocator / pending 表 / migrate handle 三个窄指针，调用它们的 owner API。
 */

#include "allocator.h"  /* alc_s */
#include "ftl_config.h" /* FTL_LBA_PER_PAIR / FTL_IU_BYTES / DWORD / ftl_status_e */
#include "pending.h"    /* pend_req_s（端口出参类型） */
#include "ssd_types.h"  /* ssd_status_e */

#include <stdbool.h>
#include <stdint.h>

/* ── 模块状态（wb_status_s 的公共前缀；当前唯一消费者是本模块）──
 *
 * 只描述「本轮调用之后模块处于什么状态」，不是模块内部的 phase 编号。 */
typedef enum FTL_MODULE_STATE_E
{
    FTL_MODULE_IDLE = 0,
    FTL_MODULE_RUNNING,
    FTL_MODULE_WAITING,
    FTL_MODULE_BLOCKED,
    FTL_MODULE_FAILED,
} ftl_mod_state_e;

typedef struct FTL_MODULE_STATUS_S
{
    ftl_mod_state_e state;
    BOOL            critical;  /* must-run liveness；正确性关键集 */
    BOOL            wants_run; /* 下一轮是否还需要被调 */
    DWORD           progress;  /* 单调计数：区分「在推进」与「卡死」 */
    DWORD           next_ms;   /* 0 = 随时可调；否则最早下次调用时间 */
    int32_t         error;     /* 仅 state == FAILED 时有意义 */
} ftl_mod_status_s;

/* 取不到目的页对的兜底上限：连续失败超过该时长即判真无块，按 FTL_ERROR 结算
 * host 写，避免 host 命令永远等（USB BOT 超时 → 设备消失）。按容量上界，
 * 正常卷写满时 free+OP 仍有余量，触发说明真的无块可用（或 OP 被异常耗尽）。 */
#define WBUF_ALLOC_STALL_TIMEOUT_MS (1000U)

/* ── 类型 ── */

/* 触发类型（wb_handle 的事件）。 */
typedef enum WB_TRIGGER_KIND_E
{
    WBUF_TRIGGER_APPEND = 0,       /* 把窗口内 n 个 CW 攒进缓冲 */
    WBUF_TRIGGER_FLUSH,            /* 把当前缓冲页编程到 NAND */
    WBUF_TRIGGER_DROP,             /* 丢弃某个 LBA（TRIM） */
    WBUF_TRIGGER_RESET,            /* 丢弃整页并按写失败结算 owners */
    WBUF_TRIGGER_MAYBE_FLUSH_IDLE, /* 无后续 append 时编程 */
} wb_trigger_kind_e;

/* 观测状态（wb_status_s.state）。 */
typedef enum WB_STATE_E
{
    WBUF_STATE_IDLE = 0,
    WBUF_STATE_OPEN, /* 持有一个窗口，尚未 flush */
} wb_state_e;

/* ── 写缓冲状态（handle）── */
typedef struct WB_HANDLE_S
{
    BYTE  data[FTL_LBA_PER_PAIR * FTL_IU_BYTES]; /* 8 CW × 512B = 4096B 页镜像 */
    BYTE  occupied;                              /* bit i = 槽 i 有数据（i = plane*CW_NUM+cw） */
    BOOL  open;                                  /* true = 正持有某窗口 */
    DWORD base;                                  /* 持有页的 pair 窗口基址（低 3 位清零） */
    DWORD owners;                                /* bit gi = 组 gi 贡献过本页（FTL_GROUP_MAX=32） */

    /* 观测计数（单调） */
    DWORD   flushed_pages;
    DWORD   drop_count;
    int32_t error;

    /* 取不到页对的首次时刻（0 = 本期未失败）。连续失败超过
     * `WBUF_ALLOC_STALL_TIMEOUT_MS` ⇒ 判真无块，按 FTL_ERROR 结算 host 写，
     * 避免 host 命令永远等（USB BOT 超时/设备消失）。 */
    DWORD alloc_fail_ms;

    /* 窄上下文：只持有实际使用的 owner 指针，不持有 ftl_context / 端口结构体。 */
    alc_s                   *alloc;
    pend_ctx_s              *pending;
    struct MIGRATE_HANDLE_S *migrate;
} wb_handle_s;

/* 触发参数。 */
typedef struct WB_TRIGGER_S
{
    wb_trigger_kind_e kind;

    DWORD       lba;  /* APPEND / DROP */
    const void *data; /* APPEND：数据源（n × FTL_IU_BYTES） */
    BYTE        n;    /* APPEND：CW 数 */
    WORD        gi;   /* APPEND：贡献组号 */
} wb_trigger_s;

/* 状态上报。 */
typedef struct WB_STATUS_S
{
    ftl_mod_status_s base;

    wb_state_e state;
    BOOL       open;
    BYTE       occupied;
    DWORD      owners;
    DWORD      flushed_pages;
    DWORD      drop_count;
} wb_status_s;

/**
 * @brief wbuf 是否完全空闲（无打开窗口、无未编程数据）。
 * 不变量：open/occupied 必须同时为空；外部 batch 只在空闲时接管写路径。
 *
 * @param h wbuf handle
 * @return true = 完全空闲；false = 仍有打开窗口或未编程数据
 */
static inline BOOL wb_is_idle(const wb_handle_s *h)
{
    return (!h->open) && (h->occupied == 0U);
}

/** @brief 取本实例的页打包写缓冲工作区（定义在 wbuf.c，DTCM）。 */
wb_handle_s *wb_workspace_get(BYTE die);

/* ── 查询（读路径；只判不取） ── */

/** @brief 该 LBA 是否在缓冲里（当前打开窗口内且槽位有效）。 */
BOOL wb_has_lba(const wb_handle_s *h, DWORD lba);

/** @brief 从缓冲读取一个 IU 的数据（未命中时 out 不被写）。 */
BOOL wb_read(const wb_handle_s *h, DWORD lba, void *out);

/* ── 接口 ── */

/**
 * @brief 绑定窄上下文并复位 handle。
 *
 * @note `migrate` **允许 NULL**（纯 RAM 单测）：无后台迁移器时只做背压，不登记
 *       紧急 GC；生产路径由 `ftl.c` 绑定真实 handle。
 */
void wb_init(wb_handle_s *h, alc_s *alloc, pend_ctx_s *pending, struct MIGRATE_HANDLE_S *migrate);

/**
 * @brief 事件驱动入口（docs §4.8）。
 *
 * @note 返回操作结果（偏离 §4.8 的 void 签名：写路径必须区分 AGAIN 背压与 ERROR
 *       永久失败）；`out` 可为 NULL。
 * @note APPEND / FLUSH / DROP / RESET / MAYBE_FLUSH_IDLE 失败时缓冲**原样保留**（可
 *       重试）；只有编程成功的 FLUSH 才清空缓冲。
 */
ftl_status_e wb_handle(wb_handle_s *h, const wb_trigger_s *trigger, wb_status_s *out);

#endif /* FTL_WBUF_H */

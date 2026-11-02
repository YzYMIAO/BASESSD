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
 * 完成时必须**逐组**递减在飞数（见 pend_group_complete_subop_multi）。
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

#include "allocator.h"  /* alc_t */
#include "ftl_config.h" /* FTL_LBA_PER_PAIR / FTL_IU_BYTES / DWORD / ftl_status_e */
#include "ftl_module.h" /* ftl_mod_status_t */
#include "pending.h"    /* pend_req_t（端口出参类型） */
#include "ssd_types.h"  /* ssd_status_e */

#include <stdbool.h>
#include <stdint.h>

/* ── 类型 ── */

/* 触发类型（wb_handle 的事件）。 */
typedef enum
{
    WBUF_TRIGGER_APPEND = 0,       /* 把窗口内 n 个 CW 攒进缓冲 */
    WBUF_TRIGGER_FLUSH,            /* 把当前缓冲页编程到 NAND */
    WBUF_TRIGGER_DROP,             /* 丢弃某个 LBA（TRIM） */
    WBUF_TRIGGER_RESET,            /* 丢弃整页并按写失败结算 owners */
    WBUF_TRIGGER_MAYBE_FLUSH_IDLE, /* 无后续 append 时编程 */
} wb_trigger_kind_e;

/* 观测状态（wb_status_t.state）。 */
typedef enum
{
    WBUF_STATE_IDLE = 0,
    WBUF_STATE_OPEN,   /* 持有一个窗口，尚未 flush */
    WBUF_STATE_FAILED, /* 最近一次操作硬失败 */
} wb_state_e;

/* ── 写缓冲状态（handle）── */
typedef struct
{
    BYTE  data[FTL_LBA_PER_PAIR * FTL_IU_BYTES]; /* 8 CW × 512B = 4096B 页镜像 */
    BYTE  occupied;                              /* bit i = 槽 i 有数据（i = plane*CW_NUM+cw） */
    bool  open;                                  /* true = 正持有某窗口 */
    DWORD base;                                  /* 持有页的 pair 窗口基址（低 3 位清零） */
    DWORD owners;                                /* bit gi = 组 gi 贡献过本页（FTL_GROUP_MAX=32） */

    /* 观测计数（单调） */
    DWORD   flushed_pages;
    DWORD   drop_count;
    int32_t error;

    /* 窄上下文：只持有实际使用的 owner 指针，不持有 ftl_context / 端口结构体。 */
    alc_t             *alloc;
    pend_table_t      *pending;
    struct mig_handle *migrate;
} wb_handle_t;

/* 触发参数。 */
typedef struct
{
    wb_trigger_kind_e kind;

    DWORD       lba;  /* APPEND / DROP */
    const void *data; /* APPEND：数据源（n × FTL_IU_BYTES） */
    BYTE        n;    /* APPEND：CW 数 */
    WORD        gi;   /* APPEND：贡献组号 */
} wb_trigger_t;

/* 状态上报。 */
typedef struct
{
    ftl_mod_status_t base;

    wb_state_e state;
    bool       open;
    BYTE       occupied;
    DWORD      owners;
    DWORD      flushed_pages;
    DWORD      drop_count;
} wb_status_t;

/* ── 接口 ── */

/**
 * @brief 绑定窄上下文并复位 handle。
 *
 * @param h       wbuf handle（整块清零后重新绑定上下文）
 * @param alloc   allocator（取页对 / 归还页对 / 查 Open 页状态）
 * @param pending pending 表（组在飞数结算 / 窗口占用查询 / 子操作登记）
 * @param migrate migrate handle（Free Pool 耗尽时登记紧急 GC）；**允许 NULL** ——
 *                无后台迁移器时（纯 RAM 单测）只做背压，不登记 GC。生产路径由
 *                `ftl.c` 绑定真实 handle，不传 NULL。
 */
void wb_init(wb_handle_t *h, alc_t *alloc, pend_table_t *pending, struct mig_handle *migrate);

/**
 * @brief 事件驱动入口（docs §4.8）
 *
 * 返回操作结果（**偏离 §4.8 的 void 签名**：写路径必须据此区分 FTL_AGAIN 背压与
 * FTL_ERROR 永久失败，P5 留痕）。`out` 非 NULL 时填本轮上报；调用者只关心返回值时
 * 可传 NULL（状态上报丢弃，不分配 dummy status）。
 *
 * APPEND / FLUSH / DROP / RESET / MAYBE_FLUSH_IDLE 失败时缓冲**原样保留**（可重试）；
 * 只有编程成功的 FLUSH 才清空缓冲。
 *
 * @param h       wbuf handle
 * @param trigger 触发参数（kind + 对应载荷：LBA / 数据 / CW 数 / 组号）
 * @param out     出参（可为 NULL）：非 NULL 时填本轮状态上报
 * @return FTL_OK    操作完成 / 无需操作
 *         FTL_AGAIN APPEND / FLUSH 遇到瞬态背压（缓冲保留，可重试）
 *         FTL_ERROR FLUSH 建链失败 / trigger->kind 非法
 */
ftl_status_e wb_handle(wb_handle_t *h, const wb_trigger_t *trigger, wb_status_t *out);

/* ── 查询（读路径；只判不取） ── */

/**
 * @brief 该 LBA 是否在缓冲里（当前打开窗口内且槽位有效）。
 *
 * @param h   wbuf handle
 * @param lba 逻辑地址
 * @return true = 命中当前窗口的有效槽位
 *         false = 未命中（无打开窗口 / pair 窗口不同 / 槽位为空）
 */
bool wb_has(const wb_handle_t *h, DWORD lba);

/**
 * @brief 从缓冲读取一个 IU 的数据。
 *
 * @param h   wbuf handle
 * @param lba 逻辑地址
 * @param out 输出缓冲（至少 FTL_IU_BYTES 字节）
 * @return true = 命中并已拷贝 FTL_IU_BYTES 字节
 *         false = 未命中（out 不被写）
 */
bool wb_read(const wb_handle_t *h, DWORD lba, void *out);

/**
 * @brief wbuf 是否完全空闲（无打开窗口、无未编程数据）。
 * 不变量：open/occupied 必须同时为空；外部 batch 只在空闲时接管写路径。
 *
 * @param h wbuf handle
 * @return true = 完全空闲；false = 仍有打开窗口或未编程数据
 */
static inline bool wb_is_idle(const wb_handle_t *h)
{
    return (!h->open) && (h->occupied == 0U);
}

#endif /* FTL_WBUF_H */

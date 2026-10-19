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
 * 完成时必须**逐组**递减在飞数（见 pending_group_complete_subop_multi）。
 *
 * ⚠ 掉电语义（无电容）：缓冲里的数据**不落 journal/L2P**——只有 flush 编程成功的
 *   completion 才确认映射（与既有顺序一致）。掉电时这些写组仍 outstanding（从未完成），
 *   丢失即正确契约；不变量「L2P/journal 只引用已编程页」由构造保证。
 *
 * ⚠ 不变量：`occupied` 清空（TRIM 掉光 / 显式复位）时**必须**退掉 `owners` 的在飞数，
 *   否则那些组永不完成（host 写挂死）。该动作收在 DROP / RESET 触发内部，
 *   **不**由调用者手工维护（见 CLAUDE.md §3.1）。
 *
 * ⚠ 模块边界（docs/design/ftl_module_contracts.md §4.8）：wbuf 不持有 ftl_context；
 *   alloc / pending / migrate 全部走 `wbuf_port_t`，由装配层在 ftl_init 绑定。
 */

#include "allocator.h"  /* ftl_allocator_t */
#include "ftl_config.h" /* FTL_LBA_PER_PAIR / FTL_IU_BYTES / lba_t / ftl_status_t */
#include "ftl_module.h" /* ftl_bg_info_t / ftl_module_status_t */
#include "pending.h"    /* ftl_pending_req_t（端口出参类型） */
#include "ssd_types.h"  /* ssd_status_t */

#include <stdbool.h>
#include <stdint.h>

/* ── 端口 ──
 *
 * 全部是**共享数据面服务**的窄接口：
 *   alloc      —— 页对分配 / 归还；
 *   pending    —— 表余量、槽申请/摘除、组在飞数增减、窗口是否有待写组；
 *   gc_urgent  —— 取不到页对时登记紧急 GC（不就地启动）。
 */
typedef struct
{
    ftl_allocator_t *alloc;

    void *pending_ctx;
    uint16_t (*pending_count)(void *ctx);
    ftl_pending_req_t *(*pending_alloc)(void *ctx);
    void (*pending_remove)(void *ctx, uint16_t command_id);
    void (*pending_validate)(ftl_pending_req_t *pending);
    void (*group_add_subop)(void *ctx, uint16_t gi);
    void (*group_complete_multi)(void *ctx, uint32_t owners, ssd_status_t status);
    bool (*group_window_pending)(void *ctx, lba_t window);

    void *migrate_ctx;
    void (*gc_urgent)(void *ctx);
} wbuf_port_t;

/* ── 写缓冲状态（handle）── */
typedef struct
{
    uint8_t  data[FTL_LBA_PER_PAIR * FTL_IU_BYTES]; /* 8 CW × 512B = 4096B 页镜像 */
    uint8_t  occupied;                              /* bit i = 槽 i 有数据（i = plane*CW_NUM+cw） */
    uint8_t  open;                                  /* 1 = 正持有某窗口 */
    lba_t    base;                                  /* 持有页的 pair 窗口基址（低 3 位清零） */
    uint32_t owners;                                /* bit gi = 组 gi 贡献过本页（FTL_GROUP_MAX=32） */

    /* 观测计数（单调） */
    uint32_t flushed_pages;
    uint32_t drop_count;
    int32_t  error;

    wbuf_port_t port; /* 装配层绑定 */
} wbuf_handle_t;

/* ── typed 入口 ── */
typedef enum
{
    WBUF_TRIGGER_APPEND = 0,       /* 把窗口内 n 个 CW 攒进缓冲 */
    WBUF_TRIGGER_FLUSH,            /* 把当前缓冲页编程到 NAND */
    WBUF_TRIGGER_DROP,             /* 丢弃某个 LBA（TRIM） */
    WBUF_TRIGGER_RESET,            /* 丢弃整页并按写失败结算 owners */
    WBUF_TRIGGER_MAYBE_FLUSH_IDLE, /* 无后续 append 时编程 */
} wbuf_trigger_kind_t;

typedef struct
{
    wbuf_trigger_kind_t kind;

    lba_t       lba;  /* APPEND / DROP */
    const void *data; /* APPEND：数据源（n × FTL_IU_BYTES） */
    uint8_t     n;    /* APPEND：CW 数 */
    uint16_t    gi;   /* APPEND：贡献组号 */
} wbuf_trigger_t;

typedef enum
{
    WBUF_STATE_IDLE = 0,
    WBUF_STATE_OPEN,     /* 持有一个窗口，尚未 flush */
    WBUF_STATE_FLUSHING, /* 一页正在编程（同步提交即返回；由完成服务收尾） */
    WBUF_STATE_FAILED,   /* 最近一次操作硬失败 */
} wbuf_state_t;

typedef struct
{
    ftl_module_status_t base;

    wbuf_state_t state;
    bool         open;
    uint8_t      occupied;
    uint32_t     owners;
    uint32_t     flushed_pages;
    uint32_t     drop_count;
    bool         timed_out;
} wbuf_status_t;

/* wbuf_init - 绑定端口并复位 handle。 */
void wbuf_init(wbuf_handle_t *h, const wbuf_port_t *port);

/*
 * wbuf_handle - 事件驱动入口（docs §4.8）
 *
 * 返回操作结果（**偏离 §4.8 的 void 签名**：写路径必须据此区分 FTL_AGAIN 背压与
 * FTL_ERROR 永久失败，P5 留痕）。`out` 恒填本轮上报。
 *
 * APPEND / FLUSH / DROP / RESET / MAYBE_FLUSH_IDLE 失败时缓冲**原样保留**（可重试）；
 * 只有编程成功的 FLUSH 才清空缓冲。`info` 当前不参与决策（保留以统一 typed 入口）。
 */
ftl_status_t wbuf_handle(wbuf_handle_t *h, const wbuf_trigger_t *trigger, const ftl_bg_info_t *info, wbuf_status_t *out);

/* ── 查询（读路径；只判不取） ── */
bool ftl_wbuf_has(const wbuf_handle_t *h, lba_t lba);
bool ftl_wbuf_read(const wbuf_handle_t *h, lba_t lba, void *out);

#endif /* FTL_WBUF_H */

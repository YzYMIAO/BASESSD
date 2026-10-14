#ifndef FTL_WEAR_H
#define FTL_WEAR_H

/*
 * Wear（块磨损计数表）— 计数持久化
 *
 * 背景：BST（ftl_block_t）里的 erase_count / read_count / rr（ECC 健康）是
 * 纯 RAM 状态，ftl_init 的 memset 会把它们清零 ⇒ 跨重启后
 *   - 动态 WL（allocator_select_plane_block_from_pool 选擦除最少块）
 *   - 静态 WL（wl_should_trigger 的 max_ec - min_ec ≥ 阈值）
 *   - read-disturb 预测（rr_predictive_scan 的 read_count ≥ 阈值）
 *   - 慢死预测（rr_predictive_scan 的 rr.health ≥ CRITICAL，v0.65）
 * 全部失效。本模块把这些 per-Block 状态落到固定块 WEAR_BLOCK 上，开机回填 BST。
 *
 * ── 介质格式：块内**页式追加日志** ──
 * 与 Anchor / Journal 同一套日志式元数据惯例（sequence + 页序），而不是
 * "每次整表重写"。理由：块 26 只有 64 页，全量快照 25 页 ⇒ 整表重写会让块 26
 * 在 3.8 次落盘后被擦一次，磨损远超普通数据块。增量日志把常态落盘压到 1–4 页。
 *
 *   页序 == sequence 序（块内只追加；块满则擦块 + 从头写全量快照）。
 *   语义 = **绝对值 + 最新胜**（不是增量累加）⇒ 同一块出现多次只有最后一页生效。
 *   全量快照 = ⌈FTL_BLOCK_TOTAL / WEAR_ENTRY_MAX⌉ = 25 页 ≤ PAGES_PER_BLOCK。
 *
 * 只用 plane 0 的 64 页（与 Anchor 同风格）。
 *
 * ⚠ Wear 是**软状态**：两次落盘之间掉电只丢 ≤ WEAR_FLUSH_ERASE_DELTA 次擦除 /
 *   ≤ WEAR_FLUSH_INTERVAL_MS 的读计数，退化的只是 WL 精度，不丢用户数据。
 */

#include "ftl/allocator.h"
#include "ftl/ftl_config.h" /* §一 几何量（FTL_BLOCK_TOTAL —— 快照页数/脏位图自检）*/
#include "ftl/metadata.h"   /* WEAR_BLOCK / WEAR_BLOCKS / FTL_WEAR_ENTRY_BYTES */

#include <stdint.h>

/* ── 表格式 ── */
#define WEAR_MAGIC (0x57454152UL) /* "WEAR" */

/* 每条目字节（= wear_entry_t 尺寸，见 ftl/metadata.h「逻辑表字节」）*/
#define WEAR_ENTRY_BYTES (FTL_WEAR_ENTRY_BYTES)

/* 一条目 = 一个 Block 的**持久化**状态（擦除/读计数 + ECC 健康）。
 * block 存**全局块号 gblock**（= plane × BLOCKS_PER_PLANE + block），
 * 这样快照天然覆盖两个 plane，不需要按 plane 分开存表。
 *
 * v0.56：`program_count` 槽已删（BST 侧字段无消费方，一并撤出持久化）；
 *        原 `_reserved` 对齐填充也随之不再需要（block 2B + read_count 2B +
 *        erase_count 4B 天然 4 B 对齐）⇒ 条目 12 B → 8 B。
 * v0.65：加 rr 三字段（序 8「rr 持久化」）——不加则慢死预测跨重启归零：
 *        health 回到 HEALTHY ⇒ rr_predictive_scan 要重新攒 RR_ECC_RISING_THRESHOLD
 *        次连续上升才再触发，期间那块一直在劣化。三字段都必要：
 *          last_ecc      —— "本次是否上升"的比较基准（丢了 ⇒ 首次读必判上升，虚高）；
 *          rising_count  —— 连续上升的进度（丢了 ⇒ 趋势从头数）；
 *          health        —— 派生态（丢了 ⇒ 已判定的 WARNING/CRITICAL 白丢）。
 *        ⇒ 条目 8 B → 12 B（`_reserved` 补齐 4 B 对齐），介质格式变 ⇒ 布局版本 +1。 */
typedef struct
{
    uint16_t block;        /* 全局块号 gblock（0..FTL_BLOCK_TOTAL-1）*/
    uint16_t read_count;   /* 主机读 + GC/WL 搬移读累加 */
    uint32_t erase_count;  /* 擦除计数：动态/静态 WL 的唯一输入 */
    uint8_t  last_ecc;     /* 上次读的 ECC 纠正 bit 数（rr_ecc_track_t.last_ecc）*/
    uint8_t  rising_count; /* 连续上升次数（rr_ecc_track_t.rising_count）*/
    uint8_t  health;       /* rr_block_health_t */
    uint8_t  _reserved;    /* 对齐填充，写 0 */
} wear_entry_t;            /* 12 B */

/* 每页条目数：头 16 B + N×8 ≤ PAGE_MAIN_SIZE（无独立 CRC，见 ADR v0.62）。
 * ⚠ 多减一条目（sizeof(wear_entry_t)）是刻意的：否则 N 会取到刚好整除的值，
 *   padding[] 退化成零长数组（非法 C）。余量写进 padding。 */
#define WEAR_ENTRY_MAX \
    (((uint32_t)PAGE_MAIN_SIZE - 16U - (uint32_t)sizeof(wear_entry_t)) / (uint32_t)sizeof(wear_entry_t))

/* 全量快照页数（块满时 compact 一次写这么多页）*/
#define WEAR_SNAPSHOT_PAGES (((uint32_t)FTL_BLOCK_TOTAL + WEAR_ENTRY_MAX - 1U) / WEAR_ENTRY_MAX)

typedef struct
{
    uint32_t     magic;       /* WEAR_MAGIC */
    uint32_t     sequence;    /* 单调递增；页序 == sequence 序 */
    uint32_t     entry_count; /* 本页有效条目数（≤ WEAR_ENTRY_MAX）*/
    uint32_t     _reserved;
    wear_entry_t entries[WEAR_ENTRY_MAX];
    uint8_t      padding[PAGE_MAIN_SIZE - 16U - (WEAR_ENTRY_MAX * sizeof(wear_entry_t))];
} wear_page_t;

/* ── 编译期自检 ── */
_Static_assert(sizeof(wear_entry_t) == WEAR_ENTRY_BYTES, "Wear 条目尺寸与 metadata.h 不一致");
_Static_assert(sizeof(wear_page_t) == PAGE_MAIN_SIZE, "Wear 页结构体不等于一页主区");
_Static_assert(WEAR_SNAPSHOT_PAGES <= PAGES_PER_BLOCK, "Wear 全量快照装不下一个块");
/* 脏位图按字节填满（wear.c 的 compact 失败路径 memset 0xFF）：块数必须是 8 的倍数，
 * 否则末字节的填充位会被 wear_dirty_count 计入 ⇒ pages_needed 高估。 */
_Static_assert((FTL_BLOCK_TOTAL % 8U) == 0U, "块数不是 8 的倍数，Wear 脏位图会有填充位");

/* ── 落盘节拍（唯一的磨损/精度旋钮）──
 * 定时 **或** 写入增量任一满足即落盘：
 *   WEAR_FLUSH_INTERVAL_MS   —— 最长多久落一次（掉电窗口的读计数上界）
 *   WEAR_FLUSH_ERASE_DELTA   —— 累计多少次写入落一次（掉电窗口的擦除上界）
 * 门限量是 ftl->total_writes（主机写成功数），作为"擦除活跃度"的**代理**：
 * 没有独立的擦除计数器可用，而主机写入是 GC 擦除的唯一驱动。纯读负载下
 * total_writes 不增长 ⇒ 只有定时门限生效（这正是想要的：读负载只需保 read_count）。
 * 调大 = 磨损更低、掉电窗口更大；调小反之。阈值取 1024 的理由：
 * 静态 WL 的判定阈值是 5000 次擦除，1024 的窗口 = 20%，不影响判定有效性。 */
#define WEAR_FLUSH_INTERVAL_MS (120000U)
#define WEAR_FLUSH_ERASE_DELTA (1024U)

/* ── 接口 ── */

/*
 * wear_init - 开机加载 Wear 表并回填 BST
 *
 * 逐页读 WEAR_BLOCK（plane 0，页 0..PAGES_PER_BLOCK-1），校验 magic，
 * 按页序回放（绝对值 + 最新胜），把计数写回 alloc->blocks[]。
 * 第一个无效页即日志末尾（⇒ 下次落盘从该页续写）。
 *
 * **非致命**：读失败/无有效页只当冷启动（计数保持 ftl_init 的 memset 零值），
 * 不 brick 开机（照 v60 ONFI 参数页的先例）。
 *
 * ⚠ 必须在 ftl_init 的 memset(ftl->tables.blocks) **之后**调用，否则回填被清零。
 *
 * @param alloc  Allocator 上下文（ch/ce/lun 路由 + blocks[] 输出）
 * @return FTL_OK / FTL_ERROR（参数非法）
 */
ftl_status_t wear_init(ftl_allocator_t *alloc);

/*
 * wear_kick - 把脏计数落盘（后台调用，**提交即返回**，v0.75 阶段 7）
 *
 * ① 无脏位 → 空转；
 * ② 节拍未到（elapsed < WEAR_FLUSH_INTERVAL_MS 且 total_writes - last <
 *    WEAR_FLUSH_ERASE_DELTA）→ 空转；
 * ③ 剩余页装不下脏条目 → 先 compact（异步擦块 → 写全量快照，next_page 归 0）；
 * ④ 否则按**起点快照的脏集合**流式写增量日志页（每页一条异步写命令）；
 * ⑤ 本批走完 → 更新节拍基准（脏位图**不清**：起点已整体清零，此刻剩下的位正是
 *    「批期间新变、本批没写到」的块，留给下一批）。
 *
 * 每步只提交**一条**命令即返回；完成由 g_op_service[FTL_OP_WEAR]（ftl_op_wear）
 * 收尾并提交下一页。故本函数的返回值不再表示"落盘成败"——判据是「job 走完且
 * 快照脏位已清」。
 *
 * ⚠ **单飞行**：job 未空闲（擦/写在飞，或上一步提交被背压拒而待续跑）⇒ 本函数
 *   只做续跑或直接返回，不会另起一批。
 * ⚠ BUSY 模式不调用本函数（照 bbt_kick 惯例）：批中某页提交被背压拒时会停在
 *   WEAR_JOB_WRITE/waiting=0，等档位回落再续跑（不占任何命令/描述符资源）。
 *
 * @param alloc        Allocator 上下文
 * @param total_writes 当前累计写入量（节拍基准，由 ftl->total_writes 传入）
 */
void wear_kick(ftl_allocator_t *alloc, uint32_t total_writes);

/*
 * wear_mark_dirty - 标记某块的计数已变更（等待下次 wear_kick 落盘）
 *
 * 在**每一个**改动 erase_count / read_count 的地方调用。
 * 只置位，不做 I/O（落盘由后台 wear_kick 批量做）。
 *
 * ⚠ 批在飞时置位是**安全**的：批起点已把脏位图整体清零、且本批只写起点快照里的块
 *   ⇒ 批期间置的位正好表示「批开始后才变的块」，收尾不清脏、留给下一批
 *   （见 allocator.h 的 wear_state_t.dirty_snapshot）。
 *
 * @param alloc  Allocator 上下文（脏位图在 alloc->wear，序 4 起 per-instance）
 * @param gblock 全局块号（0..FTL_BLOCK_TOTAL-1；越界静默忽略）
 */
void wear_mark_dirty(ftl_allocator_t *alloc, uint16_t gblock);

#endif /* FTL_WEAR_H */

#ifndef FTL_METADATA_H
#define FTL_METADATA_H

/*
 * Metadata — 固定元数据区布局（单一事实来源）+ Metadata Directory（动态位置追踪）
 *
 * 本头文件有两部分，前者是后者的前提：
 *   ① **固定元数据区布局**（见下方「固定元数据区布局」大区）——哪个分区占哪些块、
 *      每个池多大、每条 record 占几页、介质布局版本。全部由 ftl_config.h §一 的几何量
 *      推出，**没有任何块号是写死的**。运行期分区表 `g_ftl_partitions[]` 在 metadata.c。
 *   ② **Metadata Directory** ——L2P region 页放在**固定预留的 L2P 日志区**（见下），
 *      通过 Directory 追踪每份元数据的当前物理位置。Directory SRAM 常驻
 *      （14.4KB = 2048 条 × 7B），启动时从 Anchor Block 加载。
 *
 * Anchor 池（固定元数据区内的 ANCHOR_POOL_BLOCKS 个块，出厂好块预留）：
 *   只存 Metadata Directory。一份 Directory 有 ANCHOR_PAGE_COUNT 页（chunk），
 *   每页自包含 Magic + Sequence + ChunkIndex + EntryCount + Entries；
 *   **一条 record = 一整份 Directory（ANCHOR_PAGE_COUNT 页）**，任一条可独立重建
 *   整个 Directory。池内块顺序轮转（见 §元数据写模型），最新有效 record 胜。
 *
 * 元数据（固定预留区；L2P region 日志在固定 L2P 日志区内追加）:
 *   L2P Region       — 日志块（通过 Directory 查找）；目录条目 = NAND_PBA_INVALID
 *                      即该 region 从未写过（未映射），读取不经 NAND
 *   Journal          — 固定元数据区（块位置见上方「固定元数据区布局」）
 *   Checkpoint/BBT   — 固定元数据区（见上方「固定元数据区布局」）
 *
 * ── 元数据写模型（Anchor / Checkpoint / BBT / L2P 日志共用）──
 *  ① **只追加**：元数据页只写擦除态页。对已编程页二次编程 = 新数据 AND 旧数据
 *     = 静默损坏（NAND 只能 1→0）。就地覆盖一律禁止。
 *  ② **带 sequence**：每条 record 带单调递增 sequence；池内写序 == sequence 序。
 *  ③ **池内轮转**：写满一块 ⇒ 擦池内**下一块** ⇒ 从该块页 0 续写。
 *  ④ **最新有效胜**：读取 = 扫描池内所有 record，取「整条校验通过」中 sequence 最大者。
 *     整条校验 = 一条 record 的全部页有效且 sequence 一致（不取半条）。
 *
 * 崩溃安全：先擦后写 ⇒ 任意时刻「擦完未写」只让池里少一份，其余份仍可读；
 * 每条 record 是完整快照 ⇒ 擦掉最旧一份不损失任何信息。
 */

#include "allocator.h"
#include "ftl_config.h" /* §一 几何量（FTL_BLOCK_TOTAL / L2P_REGIONS_PER_INSTANCE）*/
#include "ftl_module.h" /* bg_info_t / ftl_mod_status_t */
#include "ftl_type.h"
#include "nand_config.h" /* PAGE_MAIN_SIZE（Anchor 页打包自检）*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ftl_ctx_t 的前向声明（md_pool_service 只用到指针；完整定义在 ftl.h，
 * 而 ftl.h 又 include 本头，不能反向 include）。 */
struct ftl_context;

/* ══════════════════════════════════════════════════════════════════
 * 固定元数据区布局（单一事实来源）
 *
 * 回答两件事：**元数据放在哪些块上**、**每个分区多大**。分四层推导：
 *   ① 几何量来自 ftl_config.h §一（页/块/plane/LUN 数、页主区字节、CW 数）；
 *   ② **每个分区的块数** = f(几何量, 策略常量)。策略常量（副本数、每副本冗余页数、
 *      Journal 容量系数）在下面显式列出——这是本区唯一允许"写死"的旋钮，
 *      且每个都写明了策略理由；
 *   ③ 每个分区的**起始块** = 前一分区起点 + 前一分区块数（顺序累积）；
 *   ④ 运行期分区表 `g_ftl_partitions[]`（metadata.c）把 ②③ 实例化，供按区间查询。
 * 换芯片（改 nand_config.h）或改策略（改本区）⇒ 全表自动重排。
 *
 * ── 尺寸公式（块数怎么算出来）──
 *   Anchor      ANCHOR_POOL_BLOCKS（Directory 的块轮转日志池）
 *   Checkpoint  CHECKPOINT_POOL_BLOCKS（Header 的块轮转日志池）
 *   BBT         BBT_POOL_BLOCKS（坏块位图的块轮转日志池）
 *   Journal     JOURNAL_BLOCKS（策略：容纳两次 checkpoint 之间的映射更新）
 *
 * ── 当前几何下展开的布局（由下方公式算出，非手写）──
 *   块号        分区        块数  使用 plane  来历
 *   0..31       Anchor      32    P0 + P1     元数据池：每块 4 份 Directory（16 页/份，主+备）；**P1 镜像**
 *   32..39      Checkpoint  8     P0 + P1     元数据池：每块 3 份 record；**P1 镜像**
 *   40..41      BBT         2     P0 + P1     元数据池：每块 64 份坏块位图；**P1 镜像**
 *   42..45      Journal     4     P0 + P1     策略 = CW_NUM；页地址按页号奇偶跨 plane（单副本交错）
 *   46..93      L2P log     48    P0 + P1     **固定预留**的 L2P region 日志区（见下；双副本）
 *   94..97      meta spare  4     P0 + P1     **共享**坏块替换池（仅补元数据分区 BAD 缺额）
 *   98..       用户数据    —     P0 + P1     FTL_METADATA_AREA_END 之后（磨损计数在 CP 快照内）
 *
 * ── L2P region 日志区（为什么固定预留）──
 * L2P 表本体 = L2P_REGIONS_PER_INSTANCE × 2 KB = 4 MiB（= 32 块）；每个 region 存
 * **主 + 备两份副本**（RISK-BOOT-18 的 L2）⇒ 活数据 64 块 = 8 MiB；日志区再留 **1×**
 * 工作余量（current 块 / 压缩目标 / 坏块），共 L2P_LOG_BLOCKS_TOTAL = 96 块 = 12 MiB，
 * 平均每 plane L2P_LOG_BLOCKS_PER_PLANE = 48 块。该区与 Anchor/CP/BBT/Journal 一样
 * 属于**固定预留**；固定区满时从 **OP 池**有界借用（`L2P_LOG_BORROW_MAX=8`，借还闭环、
 * 不动用户容量），容量公式已扣除固定区。
 * 依据：主流主控把元数据放在独立预留区/独立介质（SPDK FTL 的 nvcache + band tail
 * metadata；spare 在 device formation 时从 surfaced capacity 扣除），不让元数据
 * 日志与用户数据动态争抢同一个空闲池。本仓旧做法（动态从 Free Pool 取 LOG 块）
 * 已被 RISK-MD-07 / RISK-CAP-02 证明会把容量吃光。
 *
 * ⚠ **固定分区只预留 plane_mask 里的 plane**：`md_block_is_metadata()` 同时比较
 *   (plane & plane_mask)。Anchor/CP/BBT/Journal 在 P1 都有元数据（镜像或交错）；
 *   介质版本不符（例如老布局介质）时首启自动全片格式化。
 *
 * ── 元数据写模型（全部元数据统一）──
 *   ① 只写**擦除态**页。NAND 只能 1→0，对已编程页二次编程 = 新数据 AND 旧数据
 *      = **静默损坏**。全仓元数据块一律"块满才擦、擦完从头写"，绝不就地覆盖。
 *   ② 每条 record 带单调 sequence；读取 = 扫池内全部 record，取"整条校验通过"
 *      （多页 record 须每页有效且 sequence 一致）中 sequence 最大者。
 *   ③ 块满 ⇒ 擦**池内下一块**再续写（先擦后写 ⇒ 崩在中间只少一份，其余份可读）。
 *   ④ **磨损旋钮 = 池内块数**：每块擦一次所需主机写
 *      = 65,536 × 池块数 ÷ 每份页数（数据块基准 = 2,097,152）。
 *      Anchor：16 页/份 ÷ 32 块 ⇒ 131,072（数据块的 1/16）。
 *      选 32 不是为了磨损（本仓远未到该量级），而是**主控形态**：固定根 +
 *      专用元数据块池（见 docs/design/storage_controller_design_essentials.md §一/§5.7）。
 *
 * ⚠ **每个 (ch,ce,lun) 用同一份块号**：本 FTL 是"分片模型"——每个 die 一个完整
 *   独立实例（各自的 L2P 表/Anchor/BBT/Journal/Checkpoint），主机 LBA 空间
 *   按 region 切分到各实例（lba_route_* 用全局 region；映射页落盘位置用
 *   global_region / FTL_DIM_TOTAL 的局部 region，见 l2p_cache.h）。
 *   分区**不随 LUN/CE/CH 错开**。
 *
 * ⚠ 介质上的布局版本（`FTL_MEDIA_LAYOUT_VERSION`，写在 Checkpoint 头与 Anchor 页内）
 *   与当前固件不符（或根本没有当前版本记录）⇒ `ssd_ftl_boot` 自动全片格式化一次并
 *   写入当前版本标记（见 power_failure_recovery.c `pfr_media_format`）。
 *   **任何改变介质格式的改动都必须把 `FTL_MEDIA_LAYOUT_VERSION` +1。**
 * ══════════════════════════════════════════════════════════════════ */

/* ── 池尺寸：策略常量 → 每个分区的块数 ── */

/* ── Anchor（Metadata Directory）池 ──
 * Directory 有 L2P_REGIONS_PER_INSTANCE 条（每条 7 B）⇒ **一页装不下**，须跨页。
 * 每页固定装 ANCHOR_ENTRIES_PER_PAGE 条（256×7 = 1792 ≤ 主区 2048）。
 * 一份完整 Directory = 一条 record = ANCHOR_PAGE_COUNT 页（chunk）。
 * 池内每块装 ANCHOR_RECORDS_PER_BLOCK 条 record；块满换池内下一块（**先擦后写**）。*/
#define ANCHOR_ENTRIES_PER_PAGE (256U)
/* 一份 Directory 分**主/备两半**（RISK-BOOT-18 的 L2 冗余）：
 * chunk 0..PRIMARY-1 = 主 PBA（md_entry_t.pba），chunk PRIMARY..COUNT-1 = 备份 PBA
 * （md_entry_t.pba 承载 backup_pba[id]）。主半必须是**完整、同 sequence** 才算一条有效
 * record；备份半按页 best-effort（缺页只让对应 region 暂无双副本，不废整条记录）。 */
#define ANCHOR_DIR_PRIMARY_PAGES ((L2P_REGIONS_PER_INSTANCE + ANCHOR_ENTRIES_PER_PAGE - 1U) / ANCHOR_ENTRIES_PER_PAGE)
#define ANCHOR_DIR_BACKUP_PAGES  ANCHOR_DIR_PRIMARY_PAGES
#define ANCHOR_PAGE_COUNT        (ANCHOR_DIR_PRIMARY_PAGES + ANCHOR_DIR_BACKUP_PAGES) /* 16 */
#define ANCHOR_POOL_BLOCKS       (32U)                                                /* 策略：池内块数（唯一磨损旋钮，见上）*/
#define ANCHOR_RECORDS_PER_BLOCK (PAGES_PER_BLOCK / ANCHOR_PAGE_COUNT)                /* 4 */

/* ── BBT（坏块表）池 ──
 * 坏块位图 = BBT_BITMAP_SIZE 字节（512）⇒ **一条 record = 1 页**。*/
#define BBT_MAX_BLOCKS  (FTL_BLOCK_TOTAL)
#define BBT_BITMAP_SIZE (BBT_MAX_BLOCKS / 8U)
#define BBT_POOL_BLOCKS (2U) /* 策略：池内块数 */

/* ── Checkpoint（L2P 快照 + 块元数据 + 磨损快照）池 ──
 * 一条 record = Header（1 页）+ 块元数据快照（4 页，16-bit/块 × 4096 = 8 KB）
 *             + 磨损快照（16 页，8 B/块 × 4096 = 32 KB）。
 * ⚠ Header 是 commit 页：块元数据 + 磨损快照必须**先写**，Header 最后写。
 * 磨损计数（erase/read/ECC 健康）随 CP 原子落盘，无独立固定块。 */
#define FTL_CHECKPOINT_BLOCK_WORD_BYTES     (2U)
#define FTL_CHECKPOINT_BLOCK_WORDS_PER_PAGE (PAGE_MAIN_SIZE / FTL_CHECKPOINT_BLOCK_WORD_BYTES)
#define FTL_CHECKPOINT_BLOCK_PAGES          ((FTL_BLOCK_TOTAL + FTL_CHECKPOINT_BLOCK_WORDS_PER_PAGE - 1U) / FTL_CHECKPOINT_BLOCK_WORDS_PER_PAGE)

/* 磨损快照：每块 8 B（erase 4 / read 2 / last_ecc 1 / flags 1）。 */
#define FTL_CHECKPOINT_WEAR_ENTRY_BYTES      (8U)
#define FTL_CHECKPOINT_WEAR_ENTRIES_PER_PAGE (PAGE_MAIN_SIZE / FTL_CHECKPOINT_WEAR_ENTRY_BYTES)
#define FTL_CHECKPOINT_WEAR_PAGES            ((FTL_BLOCK_TOTAL + FTL_CHECKPOINT_WEAR_ENTRIES_PER_PAGE - 1U) / FTL_CHECKPOINT_WEAR_ENTRIES_PER_PAGE)

#define FTL_CHECKPOINT_SNAPSHOT_PAGES    (FTL_CHECKPOINT_BLOCK_PAGES + FTL_CHECKPOINT_WEAR_PAGES)
#define FTL_CHECKPOINT_RECORD_PAGES      (1U + FTL_CHECKPOINT_SNAPSHOT_PAGES)
#define FTL_CHECKPOINT_RECORDS_PER_BLOCK (PAGES_PER_BLOCK / FTL_CHECKPOINT_RECORD_PAGES)
#define CHECKPOINT_POOL_BLOCKS           (8U) /* 策略：池内块数 */

_Static_assert((FTL_CHECKPOINT_BLOCK_PAGES * FTL_CHECKPOINT_BLOCK_WORDS_PER_PAGE) == FTL_BLOCK_TOTAL, "块元数据快照页数必须恰好覆盖全部块");
_Static_assert((FTL_CHECKPOINT_WEAR_PAGES * FTL_CHECKPOINT_WEAR_ENTRIES_PER_PAGE) == FTL_BLOCK_TOTAL, "磨损快照页数必须恰好覆盖全部块");
_Static_assert(FTL_CHECKPOINT_WEAR_PAGES <= UINT8_MAX, "磨损快照页数必须装进 BYTE");
_Static_assert(FTL_CHECKPOINT_RECORD_PAGES <= PAGES_PER_BLOCK, "一条 Checkpoint record 必须装得进一个块");
_Static_assert(FTL_CHECKPOINT_RECORDS_PER_BLOCK >= 2U, "每块至少容纳 2 条 record（块满轮转需要）");

/* ── Journal（环形日志）──
 * 容量策略：Journal 必须容纳"两次 checkpoint 之间"的全部映射更新。
 * CW 级映射下，一次页写产生 CW_NUM 条 L2P 更新（IU = CW）
 * ⇒ 容量取 CW_NUM 块。*/
#define JOURNAL_BLOCKS      (CW_NUM)
#define JOURNAL_BLOCK_COUNT (JOURNAL_BLOCKS)

/* ── 偏移：从元数据区起点顺序累积（不写死任何块号）── */

/* 元数据区起点 */
#define FTL_METADATA_BASE_BLOCK (0U)

/* Anchor 池：池 = [ANCHOR_BLOCK0, ANCHOR_BLOCK0 + ANCHOR_POOL_BLOCKS) */
#define ANCHOR_BLOCK0 (FTL_METADATA_BASE_BLOCK)

/* Checkpoint 池：接在 Anchor 池之后 */
#define CHECKPOINT_BLOCK0 (ANCHOR_BLOCK0 + ANCHOR_POOL_BLOCKS)

/* BBT 池：接在 Checkpoint 池之后 */
#define BBT_BLOCK (CHECKPOINT_BLOCK0 + CHECKPOINT_POOL_BLOCKS)

/* Journal：第 b 块 = JOURNAL_BLOCK0 + b（**环形 + 擦除**，唯一本来就正确的元数据）*/
#define JOURNAL_BLOCK0 (BBT_BLOCK + BBT_POOL_BLOCKS)

/* 固定元数据区末尾 = L2P 日志区的起点。
 * 磨损计数在 Checkpoint 快照内，不占独立块。 */
#define FTL_METADATA_BLOCK_END (JOURNAL_BLOCK0 + JOURNAL_BLOCK_COUNT)

/* ── L2P region 日志区（固定预留，RISK-MD-07 / RISK-CAP-02）──
 * 理想块数 = ceil(region 数 / 每块页数)（每页一个 region）；预留 2×（current /
 * 压缩目标 / 坏块余量），按 plane 均分。该区是固定元数据的一部分：
 *   · 固定区为主；满时从 OP 池有界借用（L2P_LOG_BORROW_MAX），不动 Free Pool；
 *   · 容量公式用 FTL_METADATA_AREA_END 扣掉它；
 *   · 恢复/重建时按固定分区跳过（md_block_is_metadata 分区表已含它）。 */
#define L2P_LOG_IDEAL_BLOCKS ((L2P_REGIONS_PER_INSTANCE + PAGES_PER_BLOCK - 1U) / PAGES_PER_BLOCK)
/* 每个 region 的**副本数**（RISK-BOOT-18 的 L2）：主 + 备。
 * 活数据 = COPIES × ideal；日志区取 (COPIES+1) × ideal（多一份工作余量：
 * current 块 / 压缩目标 / 坏块）。 */
#define L2P_REGION_COPIES        (2U)
#define L2P_LOG_BLOCKS_TOTAL     (3U * L2P_LOG_IDEAL_BLOCKS) /* 96 = 2 份活数据 + 1 份余量 */
#define L2P_LOG_BLOCKS_PER_PLANE (((L2P_LOG_BLOCKS_TOTAL + PLANE_NUM - 1U) / PLANE_NUM))
#define L2P_LOG_BLOCK0           (FTL_METADATA_BLOCK_END)

/* 压缩触发水位：活日志块**超过**两份副本的理想块数（> 64）就启动压缩。
 * ⚠ 不额外留 25% 碎片余量：固定日志区 3× ideal（96 块），活数据 = 2× ideal（64 块），
 *   越过 64 块即压缩，余量整 1× ideal（32 块）。一旦压缩跟不上，日志区会被吃满
 *   ⇒ md_log_rotate 无空闲块、append_slot 永远 INVALID ⇒ CP 写不成 Header、
 *   掉电后每次全量重建（RISK-CP-06 / RISK-MD-08 / RISK-MD-11）。 */
#define L2P_LOG_COMPACT_TRIGGER (L2P_LOG_IDEAL_BLOCKS * L2P_REGION_COPIES) /* 64：两份活数据的理想水位 */

/* 压缩的**主判据**：空闲日志块低于此水位就启动压缩（不论 live 数）。
 * 固定区 96 块；保留 8 块 ≈ 8×256 = 2048 个槽位，足够一轮把多个稀疏 victim
 * 搬空。free 触发比 live 触发早，保证压缩永远有目标块（RISK-MD-08）。 */
#define L2P_LOG_FREE_WATERMARK (8U)

/* 固定日志区满时，允许从 **OP 池**动态借用的日志块上限（总块数）。
 * 有界溢出：8 块 = 512 KiB，足够打破“没有目标块可搬”的死锁；再多会侵蚀用户 OP。
 * 借出的块由 l2p_live / md_block_is_metadata 动态识别，擦空后回 OP。 */
#define L2P_LOG_BORROW_MAX (8U)

/* L2P 日志区末尾（= shared metadata spare 起点）。 */
#define L2P_LOG_AREA_END (L2P_LOG_BLOCK0 + L2P_LOG_BLOCKS_PER_PLANE)

/* ── 共享 metadata spare（仅补固定元数据分区的 BAD 缺额）──
 * 4 块/plane 是用户选定的预算（= 2 MiB）；按 BBT BAD 集分配给
 * BBT / Checkpoint / Journal（Anchor 与 L2P 日志固定区足够大，只跳过 BAD）。
 * 这是「BBT + spare 替换」的主流坏块管理形态：不做 logical→physical remap 表
 *（判据见 docs/design/ftl_design.md §3.3.8）。
 * ⚠ 本仓自拟的具体算法（nominal 优先 + 共享 spare 补缺 + 按 sequence 扫描定位）
 *   不冒充行业统一数值。 */
#define MD_SPARE_BLOCK0           (L2P_LOG_AREA_END)
#define MD_SPARE_BLOCKS_PER_PLANE (4U)

/* 固定元数据区（含 L2P 日志区 + shared spare）末尾 = 第一个用户块。 */
#define FTL_METADATA_AREA_END (MD_SPARE_BLOCK0 + MD_SPARE_BLOCKS_PER_PLANE)

/* 块号是否落在固定 L2P 日志区（各 plane 区间相同）。 */
static inline bool ftl_is_l2p_log_block(WORD block)
{
    return (block >= L2P_LOG_BLOCK0) && (block < L2P_LOG_AREA_END);
}

/* 块号是否落在 shared metadata spare 区。 */
static inline bool ftl_is_metadata_spare_block(WORD block)
{
    return (block >= MD_SPARE_BLOCK0) && (block < FTL_METADATA_AREA_END);
}

/* gblock 是否落在固定 L2P 日志区。 */
static inline bool ftl_is_l2p_log_gblock(WORD g)
{
    return ftl_is_l2p_log_block(ftl_gblock_block(g));
}

/* 固定池使用的 plane 集合：Anchor/Checkpoint/BBT 用 multi-plane program 把同一页
 * 写全部 plane；Journal 用页号奇偶交错（i & 1）落在前两个 plane。
 * 由 PLANE_NUM 派生，不写死 0x03。 */
#define METADATA_MIRROR_PLANE_MASK (((1U << PLANE_NUM) - 1U))

/* ── 介质布局版本（NAND 上持久化格式的版本号）──
 * 任何改变**介质格式或块布局**的改动（结构体字段 / 块号 / 记录格式 / 字段位序语义）
 * 都必须把 BASE +1。开机 cp_pool_find_latest / anchor_page_valid 都校验它；不符
 * （或介质上根本没有当前版本的 Checkpoint）⇒ pfr_media_format() 自动全片格式化
 * （见 power_failure_recovery.c），不需要上板前手动擦芯片。
 * 版本演进表见 docs/design/ftl_revision_history.md「介质布局版本」。 */
#define FTL_MEDIA_LAYOUT_VERSION_BASE (12U) /* v12：L2P region 双副本（日志区 46..93/plane，spare 94..97） */

/*
 * 数据通路用**全局 LBA**（页 META / journal 存 host 全局 LBA）。
 * - DIM=1：global == local，介质上的值逐位不变 ⇒ 版本不额外 +1，**不额外格式化**。
 * - DIM>1：同一个 LBA 的编码与单实例不同 ⇒ +1，首次启用自动全片格式化。
 * 这是本仓自定的格式策略。
 */
#define FTL_MEDIA_LAYOUT_VERSION (FTL_MEDIA_LAYOUT_VERSION_BASE + ((FTL_DIM_TOTAL > 1U) ? 1U : 0U))

/* ── 逻辑表字节（公式①：覆盖实体数 × 每实体字节）── */

/* Anchor：每份 Directory = ANCHOR_PAGE_COUNT 页 */
#define FTL_ANCHOR_TABLE_BYTES (ANCHOR_PAGE_COUNT * PAGE_MAIN_SIZE)

/* BBT：每块 1 bit */
#define FTL_BBT_TABLE_BYTES (BBT_BITMAP_SIZE)

/* Checkpoint：Header 1 页 */
#define FTL_CHECKPOINT_TABLE_BYTES (FTL_CHECKPOINT_RECORD_PAGES * PAGE_MAIN_SIZE)

/* Journal：块数 × 每块页数 × 页主区 */
#define FTL_JOURNAL_TABLE_BYTES (JOURNAL_BLOCK_COUNT * PAGES_PER_BLOCK * PAGE_MAIN_SIZE)

/* ── 分区表（固定元数据区的可枚举视图）──
 * 只在 metadata.c 内部使用：`md_block_is_metadata` 与文档化布局共用同一份数据。 */

typedef struct
{
    const char *name;        /* 分区名 */
    WORD        first_block; /* 起始块（per-plane 号；各 die 相同）*/
    WORD        block_count; /* 块数 */
    BYTE        plane_mask;  /* 本分区**实际使用**的 plane：bit0=P0, bit1=P1。
                              * 固定池只预留 mask 里的 plane；其余 plane 的同号块
                              * 是用户数据（不被 md_block_is_metadata 排除）。*/
    BYTE redundancy;         /* 每块容纳的 record 数（池内块轮转）；
                              * Journal 不是 record 池，填 1 */
    DWORD table_bytes;       /* 逻辑表大小 */
} md_partition_t;

_Static_assert(PLANE_NUM <= 8U, "plane_mask 是 BYTE，PLANE_NUM 不能超过 8");

/* ── 编译期自检（池几何与分区不重叠）── */

/* 固定元数据区不越界 */
_Static_assert(FTL_METADATA_BLOCK_END <= BLOCKS_PER_PLANE, "固定元数据区超出每 plane 块数");
_Static_assert(MD_SPARE_BLOCK0 == L2P_LOG_AREA_END, "shared spare 必须紧接 L2P 日志区");
_Static_assert((MD_SPARE_BLOCK0 + MD_SPARE_BLOCKS_PER_PLANE) == FTL_METADATA_AREA_END, "FTL_METADATA_AREA_END 必须等于 shared spare 末尾");
_Static_assert(FTL_METADATA_AREA_END <= BLOCKS_PER_PLANE, "固定元数据区（含 L2P 日志 + shared spare）超出每 plane 块数");
_Static_assert(L2P_LOG_BLOCKS_PER_PLANE >= (L2P_LOG_IDEAL_BLOCKS / PLANE_NUM + 1U), "L2P 日志区至少要容纳理想表 + 1 个 current 块");
_Static_assert((L2P_LOG_BLOCKS_PER_PLANE * PLANE_NUM) >= (L2P_LOG_IDEAL_BLOCKS + 2U), "L2P 日志区预留不足（需理想表 + current + 压缩目标）");

/* 元数据池：一条 record 装得进一块，每块至少一条；池至少 2 块
 * （换块流程是"先擦目标块再写" ⇒ 1 块的池会把自己擦掉）。*/
_Static_assert(ANCHOR_PAGE_COUNT <= PAGES_PER_BLOCK, "Anchor 一份 Directory 装不下一个块");
_Static_assert((PAGES_PER_BLOCK % ANCHOR_PAGE_COUNT) == 0U, "Anchor 每条 record 的页数必须整除每块页数");
_Static_assert(ANCHOR_RECORDS_PER_BLOCK >= 1U, "Anchor 池每块装不下一条 record");
_Static_assert(BBT_BITMAP_SIZE + 16U + 4U <= PAGE_MAIN_SIZE, "BBT 一条 record 装不下一页主区");
_Static_assert(ANCHOR_POOL_BLOCKS >= 2U, "Anchor 池至少 2 块");
_Static_assert(CHECKPOINT_POOL_BLOCKS >= 2U, "Checkpoint 池至少 2 块");
_Static_assert(BBT_POOL_BLOCKS >= 2U, "BBT 池至少 2 块");

/* 分区按块号升序、互不重叠 */
_Static_assert(ANCHOR_BLOCK0 + ANCHOR_POOL_BLOCKS <= CHECKPOINT_BLOCK0, "Anchor 与 Checkpoint 重叠");
_Static_assert(CHECKPOINT_BLOCK0 + CHECKPOINT_POOL_BLOCKS <= BBT_BLOCK, "Checkpoint 与 BBT 重叠");
_Static_assert(BBT_BLOCK + BBT_POOL_BLOCKS <= JOURNAL_BLOCK0, "BBT 与 Journal 重叠");
_Static_assert((JOURNAL_BLOCK0 + JOURNAL_BLOCK_COUNT) == FTL_METADATA_BLOCK_END, "固定区末尾必须紧接 Journal 之后");

/* ── 元数据类型 ──
 * Directory 当前只登记 METADATA_TYPE_L2P_REGION；其余类型是固定分区，位置由布局
 * 常量决定，不经过 Directory lookup。类型字段保留完整命名空间，供介质格式演进。 */
#define METADATA_TYPE_L2P_REGION (0U) /* L2P Region 数据 */
#define METADATA_TYPE_JOURNAL    (1U) /* Journal Block */
#define METADATA_TYPE_CHECKPOINT (2U) /* Checkpoint */
#define METADATA_TYPE_BBT        (3U) /* Bad Block Table */
#define METADATA_TYPE_COUNT      (4U)

/* ── Directory 容量 ──
 * = 每实例 L2P region 数：**每个 region 都必须能登记自己的位置**，否则
 * md_entry_update 的"满则丢弃"分支会把已落到动态块上的 region 位置丢掉
 * （重启后回落到"未映射" = 更新永久丢失）。容量取满即按构造消除该分支。*/
#define METADATA_DIR_ENTRIES          (L2P_REGIONS_PER_INSTANCE)
#define METADATA_DIR_ENTRIES_PER_PAGE (ANCHOR_ENTRIES_PER_PAGE)

/* ── 序列号锚定 ── */
#define ANCHOR_MAGIC (0x524F444EUL) /* "ANDIR" */

/* ── Entry: 7 字节（packed） ── */
#define METADATA_ENTRY_BYTES (7U)

typedef struct
{
    BYTE  type; /* METADATA_TYPE_* */
    WORD  id;   /* L2P region_id / 0 for others */
    DWORD pba;  /* 物理位置（PBA，自带 CH/CE ⇒ 不依赖"本实例元数据必在本实例 ch/ce"假设）*/
} __attribute__((packed)) md_entry_t;

_Static_assert(sizeof(md_entry_t) == METADATA_ENTRY_BYTES, "md_entry_t 必须是 7 字节");

/* ── NAND 上持久化格式：**每页自包含** ──
 * Directory 有 METADATA_DIR_ENTRIES 条（14KB）⇒ 一份跨 ANCHOR_PAGE_COUNT 页，
 * 每页装 METADATA_DIR_ENTRIES_PER_PAGE 条，各自带 magic/sequence。
 * 单页损坏只丢该页（其余页仍可用），且读写只需一个 2KB 缓冲。
 * ⚠ 无本层 CRC——页完整性由下层 FC 的 per-CW CRC + ECC 保证。*/
#define METADATA_ANCHOR_HEADER_BYTES (20U)

typedef struct
{
    DWORD      magic;                                                                                                           /* ANCHOR_MAGIC */
    DWORD      sequence;                                                                                                        /* 单调递增（同一份的各 chunk 相同）*/
    WORD       chunk_index;                                                                                                     /* 0..ANCHOR_PAGE_COUNT-1 */
    WORD       chunk_count;                                                                                                     /* = ANCHOR_PAGE_COUNT */
    DWORD      entry_count;                                                                                                     /* 本 chunk 有效 entry 数 */
    DWORD      layout_version;                                                                                                  /* FTL_MEDIA_LAYOUT_VERSION（版本不符 ⇒ 该页作废）*/
    md_entry_t entries[METADATA_DIR_ENTRIES_PER_PAGE];                                                                          /* 256 × 7 = 1792B */
    BYTE       padding[PAGE_MAIN_SIZE - METADATA_ANCHOR_HEADER_BYTES - (METADATA_DIR_ENTRIES_PER_PAGE * METADATA_ENTRY_BYTES)]; /* 填满一页主区 */
} md_anchor_page_t;

/* 页头前缀：廉价探针只读这 20 字节（整页 CRC/ECC 仍由 FC 校验）。 */
typedef struct
{
    DWORD magic;
    DWORD sequence;
    WORD  chunk_index;
    WORD  chunk_count;
    DWORD entry_count;
    DWORD layout_version;
} md_anchor_header_t;

/* 一页必须装得下：头 20B + 每页条目（无独立 CRC，页完整性由 FC 的 per-CW CRC + ECC 保证）*/
_Static_assert(sizeof(md_anchor_header_t) == METADATA_ANCHOR_HEADER_BYTES, "Anchor 页头前缀必须与结构体头部一致");
_Static_assert(offsetof(md_anchor_page_t, entries) == METADATA_ANCHOR_HEADER_BYTES, "Anchor 页头前缀与 entries 偏移不一致");
_Static_assert(sizeof(md_anchor_page_t) == PAGE_MAIN_SIZE, "Anchor 页结构体不等于一页主区");
_Static_assert((METADATA_DIR_ENTRIES % METADATA_DIR_ENTRIES_PER_PAGE) == 0U, "每页条目数不能整除总条目数");

/* ══════════════════════════════════════════════════════════════════
 * 元数据 record 池（环形轮转）—— Anchor / Checkpoint / BBT 共用
 *
 * 池 = **候选物理块列表** `blocks[0..block_count)`。候选来自名义区间
 * `[first_block, first_block + nominal_count)` 中在 plane_mask 所有 plane 上都
 * 非 BAD 的块；不足 nominal_count 时由 `md_pool_build()` 从 shared metadata
 * spare 补足（BBT/CP/Journal）。候选表**不落盘**：每次开机由 BBT 重算，
 * 顺序确定（nominal 升序 → spare 升序）。
 *
 * 每块容纳 PAGES_PER_BLOCK / pages_per_record 条 record；一条 record 占
 * pages_per_record 个**连续页**（页序 = 记录内序）。
 *
 * 全池共 total = block_count × records_per_block 条槽位，**按 ring 序**顺序写。
 * 写序 == sequence 序。
 *
 * ⚠ block_count >= 2 是硬要求（BBT/CP）：写点换块时要先擦目标块，若池只有 1 块，
 *   擦的正是唯一持有效 record 的块 ⇒ 全丢。
 * ══════════════════════════════════════════════════════════════════ */

/* 最大的候选池 = Anchor（其余分区都比它小）。 */
#define MD_RING_MAX_BLOCKS (ANCHOR_POOL_BLOCKS)

typedef struct
{
    WORD first_block;                /* 名义区间首块（per-plane 块号；文档/校验用）*/
    WORD nominal_count;              /* 名义区间块数（不含 spare 补缺）*/
    WORD block_count;                /* 实际候选数（= blocks[] 有效项，>= 2）*/
    WORD blocks[MD_RING_MAX_BLOCKS]; /* 候选物理块号（per-plane）*/
    BYTE pages_per_record;           /* 一条 record 占几页（向下取整到每块 record 数；
                                      * 不能整除时块尾剩余页不用——metadata_ring 的地址公式已按 floor 处理）*/
    BYTE plane_mask;                 /* 池实际使用的 plane：bit0=P0, bit1=P1。
                                      * 0x03 = 镜像（同一 record 写两个 plane）；0x01 = 单副本。*/
} md_ring_t;

/* 候选池是否包含某个 per-plane 块号。 */
static inline bool md_ring_has_block(const md_ring_t *ring, WORD block)
{
    for (WORD i = 0U; i < ring->block_count; i++)
    {
        if (ring->blocks[i] == block)
        {
            return true;
        }
    }
    return false;
}

/* plane_mask 里第 idx 个置位的 plane 号（idx 从 0 起）；越界返回 PLANE_NUM。 */
static inline BYTE md_ring_plane_at(BYTE plane_mask, BYTE idx)
{
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        if ((plane_mask & (1U << p)) != 0U)
        {
            if (idx == 0U)
            {
                return p;
            }
            idx--;
        }
    }
    return PLANE_NUM;
}

/* plane_mask 里的 plane 个数。 */
static inline BYTE md_ring_plane_count(BYTE plane_mask)
{
    BYTE n = 0U;

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        if ((plane_mask & (1U << p)) != 0U)
        {
            n++;
        }
    }
    return n;
}

/* 每块容纳的 record 数 */
static inline WORD md_ring_records_per_block(const md_ring_t *ring)
{
    return (PAGES_PER_BLOCK / ring->pages_per_record);
}

/* 池内 record 槽位总数 */
static inline WORD md_ring_total_records(const md_ring_t *ring)
{
    return (ring->block_count * md_ring_records_per_block(ring));
}

/* 各池槽位数必须装得进 WORD（写游标 / record 下标都是 16 位）*/
_Static_assert((ANCHOR_POOL_BLOCKS * ANCHOR_RECORDS_PER_BLOCK) <= UINT16_MAX, "Anchor 池槽位数超出 WORD");
_Static_assert((CHECKPOINT_POOL_BLOCKS * PAGES_PER_BLOCK) <= UINT16_MAX, "Checkpoint 池槽位数超出 WORD");
_Static_assert((BBT_POOL_BLOCKS * PAGES_PER_BLOCK) <= UINT16_MAX, "BBT 池槽位数超出 WORD");

/* ── 四个固定元数据分区的候选池（每个实例一份，放 md_dir_cache_t）──
 * 只在 `md_pool_build_all()` 里生成；其余模块只读（H10/H13）。
 * ⚠ 不落盘：每次开机按 BBT BAD 集重算，顺序确定（nominal 升序 → spare 升序）。 */
typedef struct md_pools
{
    md_ring_t anchor;
    md_ring_t cp;
    md_ring_t bbt;
    md_ring_t jrn;
    bool      dirty; /* 运行期元数据 BAD ⇒ 需 md_pool_service 重算（唯一写点：bbt_mark_bad） */
} md_pools_t;

/*
 * md_pool_build_bbt_bootstrap - BBT 自举候选池（未知 BAD 集时使用）
 *
 * 名义 BBT 块 + 全部 shared spare 候选（不做 BAD 过滤）；BBT record 自带
 * magic/sequence，坏块/空块的读失败会被 `md_ring_locate` 自然忽略。
 */
void md_pool_build_bbt_bootstrap(md_ring_t *ring);

/*
 * md_pool_build_all - 用 BBT BAD 集重建四个固定元数据分区的候选池
 *
 * 候选 = 名义区间内（plane_mask 所有 plane 都）非 BAD 的块；不足名义数时从
 * shared metadata spare 按固定顺序补。硬约束：
 *   BBT >= 2、Checkpoint >= 2、Journal == JOURNAL_BLOCK_COUNT；
 *   Anchor 只跳过 BAD（32 块固定区足够）。
 *
 * @return true = 全部满足；false = 某分区凑不齐（release 下调用者必须拒绝启动）
 */
bool md_pool_build_all(alc_t *alloc);

/*
 * record 槽位 → 芯片地址
 *
 * record_idx ∈ [0, total)；page_in_record ∈ [0, pages_per_record)。
 */
DWORD md_ring_addr(const md_ring_t *ring, const ftl_topo_t *topo, WORD record_idx, BYTE page_in_record);

/* 指定 plane 的同一个 record 槽地址（镜像池的读回退 / 写镜像用）。
 * plane 必须落在 ring->plane_mask 内；越界是调用者 bug ⇒ ASSERT。 */
DWORD md_ring_addr_plane(const md_ring_t *ring, const ftl_topo_t *topo, WORD record_idx, BYTE page_in_record, BYTE plane);

/*
 * metadata_ring_probe_fn - 调用方的 record 探针（由 ring 模块回调）
 *
 * full == 0：**廉价探针** —— 只读 record 的第 0 页取 sequence（用于全池扫一遍找
 *            最大 sequence；每条只花 1 次页读）。
 * full != 0：**整条校验** —— 校验一条 record 的**全部**页（页内 magic、
 *            各页 sequence 一致、chunk 序完整），通过才给 sequence。
 *
 * @param ctx           调用方上下文
 * @param record_idx    槽位号
 * @param full          false = 廉价探针，true = 整条校验
 * @param out_sequence  输出：该 record 的 sequence
 * @return FTL_OK = 该 record 在本次探针强度下有效
 *
 * ⚠ 契约：**失败时不写 *out_sequence**（调用者据此把"探针失败"与"sequence=0"分开）。
 *   写侧用 sequence 从 1 起，故 0 恒表示"未写"。
 */
typedef ftl_status_e (*metadata_ring_probe_fn)(void *ctx, WORD record_idx, bool full, DWORD *out_sequence);

/* md_ring_locate 的输出：最新有效 record + 写游标。 */
typedef struct
{
    WORD  newest_idx; /* 最新有效 record 槽位；池空 = UINT16_MAX */
    DWORD sequence;   /* 该 record 的 sequence；池空 = 0 */
    WORD  write_idx;  /* 下一个可写槽位 */
} md_ring_locate_out_t;

/*
 * md_ring_locate - 定位池内最新有效 record 与下一个可写槽
 *
 * 算法（利用「record 在块内页对齐 + 池内顺序写」两个不变量，只读 O(块数) 页）：
 *   ① 逐块读该块**末条** record 的廉价探针 → 得每块末条的 sequence（或"该块未写满"）；
 *   ② 第一个末条无效的块 = **游标块**；若所有块末条都有效（池写满一圈）
 *      ⇒ 游标块 = (sequence 最大的块 + 1) mod block_count；
 *   ③ 游标块内降序找最后一条有效 record = 游标块末条；
 *   ④ 写点 = 游标块末条的下一条（末条是块内最后一条时跨到下一块槽 0）；
 *      游标块内无有效 record ⇒ 写点 = 游标块的槽 0；
 *   ⑤ 候选 = {全部块中 sequence 最大的末条, 游标块末条}，按 sequence 降序做**整条
 *      校验**，返回第一个通过者。
 *   ⑥ 快路径候选整条校验全失败（局部损坏）⇒ 慢路径：全池廉价探针取 sequence 最大者
 *      整条校验，失败则排除该 sequence 再找次新。**只在损坏时**付出全池代价。
 *
 * 快路径代价 = block_count 次页读 + 游标块内至多 records_per_block 次页读。
 *
 * @param ring           池几何
 * @param probe          调用方探针
 * @param ctx            探针上下文
 * @param out 输出：最新有效 record 槽位 / sequence / 写游标（池空时 newest=UINT16_MAX、seq=0）
 * @return FTL_OK = 定位完成（含"池为空"）；FTL_ERROR = ring 几何非法
 */
ftl_status_e md_ring_locate(const md_ring_t *ring, metadata_ring_probe_fn probe, void *ctx, md_ring_locate_out_t *out);

/*
 * md_ring_prepare_addr - 算写点槽位地址 + 判定是否需先擦该块（**不动介质**）
 *
 * 写点是块内槽 0 时**需要先擦该块**（池内轮转：该块持的是全池最旧的 record，且每条
 * record 都是完整快照 ⇒ 擦掉不丢信息）。本函数只做**判定**，擦除留给调用者：
 * 同步站点用 md_ring_prepare_write（内部即刻擦），异步 job 按 *out_need_erase
 * 自行提交异步擦命令——把"擦"放进状态机，而不是在服务循环里阻塞。
 *
 * @param ring          池几何
 * @param topo          ch/ce/lun
 * @param write_idx     写点槽位（来自 md_ring_locate 或上次写点 + 1）
 * @param out_addr      输出：record 第 0 页地址
 * @param out_need_erase 输出：true = 写之前必须先擦 *out_addr 所在块
 * @return FTL_OK / FTL_ERROR（几何非法或槽位越界；ring/topo/出参由调用者提供）
 */
ftl_status_e md_ring_prepare_addr(const md_ring_t *ring, const ftl_topo_t *topo, WORD write_idx, DWORD *out_addr, bool *out_need_erase);

/*
 * md_ring_prepare_write - 把写点槽位准备好（含换块擦除），返回该 record 第 0 页地址
 *
 * = md_ring_prepare_addr + 需要时同步擦除该块。**同步**原语：只许结构性同步
 * 上下文调用（开机 / 格式化 / 同步回退）；异步 job 请用 md_ring_prepare_addr。
 *
 * @param ring      池几何
 * @param topo      ch/ce/lun
 * @param write_idx 写点槽位（来自 md_ring_locate 或上次写点 + 1）
 * @param out_addr  输出：record 第 0 页地址
 * @return FTL_OK / FTL_ERROR（擦除失败）
 */
ftl_status_e md_ring_prepare_write(const md_ring_t *ring, const ftl_topo_t *topo, WORD write_idx, DWORD *out_addr);

/* ── 元数据目录后台 job ──
 *
 * 承载 L2P 日志块的两件"要碰 NAND 的整理活"，让它们不在服务循环里阻塞：
 *   ① **回收**——某个日志块的活 region 数归零 ⇒ 擦块 + 归还固定 L2P 日志区；
 *   ② **压缩**——抗碎片：挑活 region 最少的块做 victim，把它的活 region 搬进当前
 *      块，落 Anchor 后擦除归还。
 *
 * 阶段序列（每条命令一个 phase，完成由 g_op_callbacks[FTL_OP_METADATA] 收尾）：
 *
 *   IDLE ──reclaim_pending──▶ RECLAIM（擦 + 归还）──▶ IDLE
 *   IDLE ──compact_wanted──▶ CP_READ ⇄ CP_WRITE（逐 region 搬移）
 *                              └─▶ CP_ANCHOR_PREP ─▶ [CP_ANCHOR_ERASE] ─▶
 *                                  CP_ANCHOR_WRITE ×N ─▶ CP_RECLAIM（擦 victim）──▶ DONE ─▶ IDLE
 *
 * ⚠ **顺序即掉电安全契约**（不可倒）：
 *   ① 搬移 → ② 登记 → ③ Anchor 落盘 → ④ 擦 victim → ⑤ 归还。
 *   ③ 必须早于 ④——否则掉电后 Anchor 仍指向已擦块，那些 region 的映射永久丢失。
 *   job 里就是 phase 顺序：CP_ANCHOR_* 全在 CP_RECLAIM 之前。
 *
 * ⚠ **失败不回滚**：任一 phase 失败 ⇒ 中止，已登记的 region 已指向新页、未搬的仍指向
 *   victim（**victim 不擦**）⇒ 两边都有效，下次换块时重来即可。
 *
 * ⚠ **服务函数不推进状态机**（同 ftl_checkpoint_callback）：CP_ANCHOR_PREP 要调
 *   md_anchor_prepare（游标未建立时会**同步**扫池定位），在完成泵里跑就是
 *   嵌套泵。故 ftl_metadata_callback 只记结果 + release + 改 phase，推进一律留给下一轮
 *   md_handle（在 ftl_service 各档位里跑）。
 *
 * ⚠ 本结构不跨重启：phase / 游标 / 请求位全是 RAM 态，冷启动恒 IDLE（见 md_init）。 */
typedef enum
{
    MD_JOB_IDLE = 0,        /* 无 job 在跑 */
    MD_JOB_RECLAIM,         /* 在飞：擦一个「已无引用」的日志块 */
    MD_JOB_CP_READ,         /* 在飞：读 victim 的下一个活 region 页 → cp_buf */
    MD_JOB_CP_WRITE,        /* 在飞：把 cp_buf 写进当前块的新槽 */
    MD_JOB_CP_ANCHOR_PREP,  /* 纯 RAM：定位 Anchor 写点（冷启动时同步扫池）*/
    MD_JOB_CP_ANCHOR_ERASE, /* 在飞：Anchor 换块擦 */
    MD_JOB_CP_ANCHOR_WRITE, /* 在飞：Anchor 逐页写 */
    MD_JOB_CP_RECLAIM,      /* 在飞：擦 victim */
    MD_JOB_DONE             /* 收尾（纯 RAM）→ IDLE */
} md_handle_phase_e;

/*
 * md_compact_step_e - 压缩 job「搬一条 region」一步的结果
 *
 * md_compact_read / md_compact_write 共用本契约（write 永不返回 MD_COMPACT_SCANNED：
 * 写一条就回去读下一条）。调用者按**具名值**分派，不写数值魔法。
 */
typedef enum
{
    MD_COMPACT_BACKPRESSURE = 0U, /* 背压未提交：保留游标/槽，**下轮重试**（调用者 return）*/
    MD_COMPACT_SUBMITTED    = 1U, /* 已提交异步命令：等完成服务函数 */
    MD_COMPACT_SCANNED      = 2U, /* 全表扫完、无更多 region 待搬（read 专有）*/
    MD_COMPACT_FAILED       = 3U, /* 硬失败（md_handle_stall 无进展）：调用者 md_handle_abort */
} md_compact_step_e;

/*
 * md_compact_start_e - 压缩 job 能否启动
 *
 * ⚠ 与 md_compact_step_e 不共用：本处 0 = **已启动**，step 的 0 = 背压未提交。
 * 调用者用它区分「无活可搬」与「有活但当前块装不下」——后者需要换块才有目标槽。
 */
typedef enum
{
    MD_COMPACT_STARTED    = 0U, /* 已进入 CP_READ */
    MD_COMPACT_NO_CURRENT = 1U, /* 无当前写块：无处可搬 */
    MD_COMPACT_NO_VICTIM  = 2U, /* 无候选（零活块归 reclaim 管 / 只剩当前块） */
    MD_COMPACT_NO_SLOTS   = 3U, /* 当前块空槽装不下活 region 最少的 victim */
} md_compact_start_e;

/*
 * md_anchor_step_e - md_anchor_prepare 的结果
 *
 * ⚠ 与 md_compact_step_e **不能合并**：本处 0（无需写）时调用者**推进 phase**（跳过
 *   Anchor 直接擦 victim），而 md_compact_* 的 0（背压）时调用者 **return 等下一轮**
 *   —— 同值反义，共用一套名字会把两处调用点读反。
 */
typedef enum
{
    MD_ANCHOR_CLEAN  = 0U, /* dir 不脏、无需写 Anchor：调用者整段跳过 */
    MD_ANCHOR_READY  = 1U, /* 出参全部有效，可以写 Anchor */
    MD_ANCHOR_FAILED = 2U, /* 定位/取槽失败：调用者中止 job */
} md_anchor_step_e;

/* md_anchor_prepare 的输出：READY 时全部有效。 */
typedef struct
{
    md_ring_t ring;       /* Anchor 池几何（commit 要用） */
    DWORD     base;       /* 本 record 第 0 页地址 */
    WORD      idx;        /* 本 record 的池内槽位号（commit 要用） */
    DWORD     seq;        /* 本 record 的 sequence（prepare 内自增 anchor_sequence） */
    bool      need_erase; /* true = 写之前必须先擦 base 所在块 */
} md_anchor_prepare_out_t;

/* 布尔量直接用 bool；**不压位域**——位域布局是 implementation-defined，
 * `_Static_assert` 证不了（C4），且这里没有 RAM 压力。 */
typedef struct
{
    md_handle_phase_e phase;               /* 异步 job 状态机 */
    bool              waiting;             /* 有命令在飞（完成服务函数清）*/
    bool              compact_wanted;      /* append_slot 换块后池吃紧 ⇒ 请求压缩 */
    bool              anchor_ok;           /* Anchor 分页写是否全部成功（false = 整条作废）*/
    bool              anchor_need_erase;   /* Anchor 写点是否需先擦块 */
    bool              anchor_for_reclaim;  /* 本次 Anchor 仅为「回收前落盘」：完成后回 RECLAIM */
    BYTE              anchor_chunk;        /* 已写的 Anchor 页序 */
    WORD              victim;              /* 压缩 victim 的 gblock（UINT16_MAX = 无）*/
    WORD              region_id;           /* 搬移游标（下一个待查 region_id）*/
    WORD              cur_region;          /* 当前搬移中的 region_id（CP_READ 命中时设定）*/
    BYTE              cur_copy;            /* 当前搬移的副本（0=主 / 1=备，RISK-BOOT-18 L2）*/
    WORD              moved;               /* 本次压缩已搬 region 数（观测）*/
    WORD              reclaim_gblock;      /* 在飞擦除的块（完成时归还它）*/
    WORD              reclaim_fail_gblock; /* 连续擦除失败计数对应的块（RISK-MD-01）*/
    BYTE              reclaim_fail_count;  /* 该块连续擦除失败次数；达上限即标坏并清位 */
    DWORD             stall_ms;            /* 连续「提交不出去」的起点（0 = 未计时） */
    bool              timed_out;           /* true = 本次 job 因 deadline 中止 */
    md_handle_phase_e timeout_phase;       /* 超时发生的 phase */
    WORD              anchor_idx;          /* 本 Anchor record 的池内槽位 */
    DWORD             anchor_seq;          /* 本 Anchor record 的序号 */
    DWORD             anchor_base;         /* 本 Anchor record 第 0 页地址 */
    DWORD             slot;                /* 压缩搬移已取到、待写的槽（NAND_PBA_INVALID = 未取）*/
    md_ring_t         anchor_ring;         /* Anchor 池几何（prepare 出参，commit 要用）*/
    DWORD             reclaimed_count;     /* 累计擦除成功并归还的日志块数（单调进度） */
    int32_t           error;               /* 最近一次中止的 FTL_*（回 IDLE 时清） */

    /* 心跳基准（见 ftl_job.h）：job 在飞期间每满 1s 打一行 phase/victim/age */
    DWORD start_ms;
    DWORD log_ms;

    /* victim region 页搬运缓冲：**必须常驻**——异步读完成与随后的写之间隔着若干轮
     * 服务循环，栈帧早已返回。放 job 而非栈（同 mig_handle.page_buf / rr_page_scratch）。 */
    BYTE cp_buf[L2P_REGION_SIZE];
} md_handle_t;

typedef struct
{
    ftl_mod_status_t base;

    md_handle_phase_e phase;           /* 观测；外部不要据此驱动 */
    WORD              victim;          /* 压缩 victim（UINT16_MAX = 无） */
    WORD              moved;           /* 本次压缩已搬 region 数 */
    DWORD             reclaimed;       /* 累计擦除成功的日志块数（单调进度） */
    bool              compact_wanted;  /* 池吃紧，请求压缩 */
    bool              anchor_ok;       /* Anchor 分页写是否全部成功 */
    bool              reclaim_pending; /* 有已登记待回收的日志块 */
    bool              timed_out;       /* 本次 job 因 deadline 中止 */
    md_handle_phase_e timeout_phase;   /* 超时发生的 phase */
} md_status_t;

/* ── 常驻 Directory Cache（33,752 B，`g_ftl_md[]` 落 SRAM4，见 src/ftl/metadata.c） ── */
typedef struct md_dir_cache
{
    md_entry_t entries[METADATA_DIR_ENTRIES];
    /* L2P region 的**第二份副本** PBA（RISK-BOOT-18 的 L2）。
     * 与 entries[i].pba 同一代、同一次 commit 写入；Anchor 的 backup chunks 承载它。
     * NAND_PBA_INVALID = 该 region 还没有副本（未写过 / 旧介质单副本）。
     * ⚠ 副本也计入 l2p_live（回收判据是“所有占用者都走了”）。 */
    DWORD backup_pba[METADATA_DIR_ENTRIES];
    WORD  count;           /* 有效 entry 数 */
    DWORD anchor_sequence; /* 当前 Anchor sequence */
    bool  dirty;           /* 是否有未写回 Anchor 的变更 */
    DWORD dirty_gen;       /* Directory 变更代数：每次 update 自增 */
    DWORD anchor_gen;      /* 本次 Anchor 写入所覆盖的 dirty_gen 快照 */

    /* ── L2P 日志块（元数据写模型：只追加 + 块池轮转）──
     * 每个 region 写进「当前日志块」的下一空槽（一 region = 一页）。块满则从
     * **固定 L2P 日志区**取新块（NONE = 已擦净）。l2p_live 记每个日志块上还有多少
     * region 的**最新位置**落在这里 —— 归零即置 reclaim_pending，由后台 job 异步擦除。
     * l2p_live 同时是 `md_block_is_metadata` 的唯一动态块判据（C9：不维护第二套坐标）。 */
    WORD l2p_live[FTL_BLOCK_TOTAL]; /* 每 gblock 的活 region 数 */
    WORD cur_metadata_block;        /* 当前日志块 gblock（cur_block_valid=false 时无意义）*/
    BYTE cur_metadata_page;         /* 当前日志块内下一可用页 */
    bool cur_block_valid;           /* 游标是否已建立（false = 尚未分配日志块）*/
    BYTE cur_metadata_plane;        /* 下一个新日志块选哪个 plane（轮转）*/
    WORD log_borrow_count;          /* 从 OP 池借用的日志块数（≤ L2P_LOG_BORROW_MAX）*/

    /* ── 待回收日志块位图（RAM 态，不跨重启）──
     * commit_region 在活计数归零时置位；append_slot 换走旧当前块时若其已空也置位。
     * 运行期擦除全部由 md_handle 的 MD_JOB_RECLAIM 异步做——append_slot 不做
     * 同步擦除（3 ms tERASE 会击穿服务预算）。
     * 位在"擦除在飞"期间保持置位：完成成功才清；失败保留，下轮重试。 */
    DWORD reclaim_pending[FTL_BLOCK_TOTAL / 32U];
    WORD  reclaim_pending_count;
    WORD  reclaim_scan; /* 轮转起点：擦除持续失败的块不再永久挡住后续块 */

    /* 后台整理 job（回收 + 压缩，见 md_handle_t）。两个作用：
     *   ① **抑制逐次立即回收**（搬移 victim 的最后一个活 region 时 l2p_live[victim]
     *      归零，若就地擦掉就是**在 Anchor 落盘之前**擦 victim ⇒ 掉电丢映射）；
     *   ② **防重入 / 防抢槽**（压缩搬移要往当前块写，期间再发放槽位会打乱空槽预算）。
     * 判据分别是 md_handle_is_compacting()（①）与 job.phase != IDLE（②）。 */
    md_handle_t handle;

    /* Anchor 池游标（ring 写点槽位；anchor_cursor_valid=false 时尚未定位）*/
    WORD anchor_write_idx;
    bool anchor_cursor_valid;

    /* 固定元数据分区的候选块池（BBT + shared spare；只在 md_pool_build_all 写） */
    md_pools_t pools;

    /* 拓扑信息（元数据所在芯片路由） */
    ftl_topo_t topo;

    /* allocator / total_writes：只持有指针，操作走 allocator owner API。 */
    alc_t       *alloc;
    const DWORD *total_writes;
} md_dir_cache_t;

/* 待回收位图按 32 位字切分，块总数必须能整除一个字。 */
_Static_assert((FTL_BLOCK_TOTAL % 32U) == 0U, "FTL_BLOCK_TOTAL 必须是 32 的倍数");

/* ── API ── */

/*
 * md_init - 初始化 Metadata Directory
 *
 * 首次使用时为全部 L2P Region 登记 `NAND_PBA_INVALID`（= 未映射；读取该 region
 * 不需要访问 NAND）。写过并刷盘的 region 由 md_commit_region 覆盖成
 * 日志块上的真实位置。
 *
 * 游标全部置**无效**（cur_block_valid = 0 / anchor_cursor_valid = 0）：首次追加
 * 才从固定 L2P 日志区取块，避免"没拿块就用"。
 *
 * @param dir  Directory cache（由调用者提供存储）
 */
void md_init(md_dir_cache_t *dir, alc_t *alloc, const DWORD *total_writes);

/*
 * md_entry_find - 查找元数据物理位置
 *
 * @param dir  Directory cache
 * @param type 元数据类型
 * @param id   L2P region_id / 0 for others
 * @return PBA；NAND_PBA_INVALID = 未找到。对 L2P region 而言"未找到"与"未映射"
 *         同义：`fill_fixed` 为每个 region 预登记一条，之后只在刷盘时覆盖。
 */
DWORD md_entry_find(const md_dir_cache_t *dir, BYTE type, WORD id);

/*
 * md_live_count_get - 某个 gblock 上还有多少 region 的**最新位置**落在它上面。
 *
 * 供开机恢复（PFR）判断「该块是否仍被 Directory 引用」。越界返回 0（不是 ASSERT：
 * 调用者的 g 来自介质/快照，属不可信输入，H29）。
 */
WORD md_live_count_get(const md_dir_cache_t *dir, WORD gblock);

/*
 * md_entry_update - 添加或更新元数据位置
 *
 * @param dir    Directory cache
 * @param type   元数据类型
 * @param id     L2P region_id / 0 for others
 * @param new_pba 新物理位置（PBA）
 */
void md_entry_update(md_dir_cache_t *dir, BYTE type, WORD id, DWORD new_pba);

/*
 * md_slot_pair_append - 一次预留**两个连续槽**（主/备副本，RISK-BOOT-18 的 L2）
 *
 * **只取槽，不登记**：调用方先往返回的地址写 region 页，两份都写成功后再调
 * md_commit_region(MD_COMMIT_PAIR) 登记（写失败则位置不变，槽被跳过——已编程页不可复用）。
 * 两份副本落在同一当前块的两个连续页上：剩余页不足 2 时**先换块再取**，避免
 * “第一个槽已消耗、第二个换块失败”烧掉半对。整理 job 在飞 / 换块失败 ⇒ false
 * （未消耗任何槽）。
 *
 * 块满（或尚无日志块）⇒ 从**固定 L2P 日志区**取一整块已擦除的空闲块
 * （`md_log_alloc`；NONE = 已擦净）。无活 region 的旧块由后台整理 job 异步擦除
 * 归还——本函数只登记，不同步擦除。**绝不回退到用户 Free Pool**：分配失败返回
 * false，由上层重试（待压缩/回收腾出块后自然成功）。
 *
 * 日志块在哪个 plane 由 dir->cur_metadata_plane 轮转决定（写入目标与 region 归属
 * 无关：位置由 pba 自身携带，读回一律经 Directory 查 pba）。
 *
 * ⚠ **本函数不做压缩**：换块后若活日志块数超水位，只登记 `job.compact_wanted`——
 *   压缩由 md_handle 起异步 job 做，避免在写回路径上同步阻塞数十毫秒。
 * ⚠ **整理 job 在飞时一律返回 false**：压缩的空槽预算按发放那一刻的
 *   cur_metadata_page 核定，期间再发放会打乱预算与 ③④ 顺序。调用者
 *   （l2p_flush_kick）据此返回 FTL_AGAIN，下轮重试。
 *
 * @param dir  Directory cache
 * @param alloc Allocator
 * @param out  输出两个 PBA（主、备）
 * @return true = 两槽都取到；false = 背压，未取槽
 */
bool md_slot_pair_append(md_dir_cache_t *dir, alc_t *alloc, DWORD out[L2P_REGION_COPIES]);

/* ── 固定 L2P 日志区（RISK-MD-07 / RISK-CAP-02）──
 * 这 64 个 gblock 是**固定预留**的元数据区：只由 metadata 模块在
 * [L2P_LOG_BLOCK0, FTL_METADATA_AREA_END) 内部分配/归还，不经过 allocator 的
 * Free/OP/Spare 池。块状态约定：
 *   LOG   = 当前写块 / 有活 region / 残留旧数据（待回收）
 *   NONE  = 已擦除、空闲可用
 *   BAD   = 终态，池容量永久减一
 * 依据：主流主控把元数据放在独立预留区（SPDK FTL 的 nvcache + band tail），
 * 元数据空间有界、可预测；旧版从 Free Pool 动态取块导致日志碎片吃掉用户容量。 */

/*
 * md_log_area_restore - 开机按 Directory 恢复固定日志区的块状态
 * @param dir   已由 Anchor 加载好的 Directory（l2p_live / cur_metadata_block 有效）
 * @param alloc 分配器（读 BST；本函数不改 Free/OP/Spare 池）
 *
 * 活块与当前写块置 LOG；非活块读 page 0 判空：空 ⇒ NONE（空闲），非空 ⇒ LOG +
 * 登记回收（后台擦除后由 md_log_release 置 NONE）。只在开机路径调用（允许
 * 同步 NAND 读）；每块一次 page 0 读，最多 L2P_LOG_BLOCKS_TOTAL 次。
 */
void md_log_area_restore(md_dir_cache_t *dir, alc_t *alloc);

/*
 * md_log_alloc - 取一个日志块（置 LOG，独占）
 *
 * 顺序：① 固定 L2P 日志区的空闲块（NONE）；
 *       ② 固定区满时从 **OP 池**有界借用（log_borrow_count < L2P_LOG_BORROW_MAX）。
 * 借来的块由 l2p_live / md_block_is_metadata 动态识别，擦空后回 OP。
 * @return gblock；固定区与 Free Pool 都无可用块 ⇒ FTL_GBLOCK_INVALID（调用者按背压处理）
 */
WORD md_log_alloc(md_dir_cache_t *dir, alc_t *alloc, BYTE plane);

/*
 * md_log_rotate - 换一个新的当前写块
 *
 * 两个调用者：
 *   · 追加路径（md_slot_pair_append）：当前块写满或剩余页不足 2 时换块；
 *   · 整理 job（md_handle_step）：压缩因当前块空槽不足起不动时**强制换块**。
 *     压缩的 destination 就是当前块，而换块原本只由追加触发 ⇒ host 写停止后
 *     没有追加 ⇒ 不换块 ⇒ 压缩永远拿不到整块空槽，日志区锁死。
 *
 * 旧当前块若已无活 region ⇒ 登记回收；换块时扫描全池只登记不擦。
 * @return true = 已换到新块（cur_metadata_page == 0）；
 *         false = 没换（固定区无空闲块且借用已满，或刚换块但活块数超水位、本轮不发放槽位）
 */
bool md_log_rotate(md_dir_cache_t *dir, alc_t *alloc);

/*
 * md_log_release - 擦除成功后归还一个日志块
 * @param dir    Directory（取 alloc 与借用计数）
 * @param gblock 固定日志区内的块 ⇒ 置 NONE 留在固定区；
 *               借来的块 ⇒ 回 OP 池并减 log_borrow_count。
 */
void md_log_release(md_dir_cache_t *dir, WORD gblock);

/* 固定日志区的空闲块数（state == NONE 且非 BAD）。压缩按它触发（free 水位）。 */
WORD md_log_area_free_count(const alc_t *alloc);

/* 当前仍有活 region 的日志块数（观测：应保持有界，理想 ~32）。 */
WORD md_live_block_count_get(const md_dir_cache_t *dir);

/* md_commit_region 的提交粒度（单副本 / 成对）。 */
typedef enum
{
    MD_COMMIT_PRIMARY = 0U, /* 只提交主副本 */
    MD_COMMIT_BACKUP  = 1U, /* 只提交备副本 */
    MD_COMMIT_PAIR    = 2U, /* 主/备同批提交：两份都写成功后调用 */
    MD_COMMIT_MODE_NUM,
} md_commit_mode_e;

/*
 * md_commit_region - 写成功后登记 region 的副本位置
 *
 * 按 mode 提交一个或两个副本：
 *   · MD_COMMIT_PRIMARY / MD_COMMIT_BACKUP —— 只提交对应副本；
 *   · MD_COMMIT_PAIR —— 主/备都提交，要求两个 PBA 都有效且不相等
 *     （同一次 flush 两份都写成功后才调用，不做半对提交）。
 *
 * 更新 entries / backup_pba + l2p_live（旧日志块 -1、新日志块 +1）+ dirty。
 * 旧块活计数归零且非当前块 ⇒ 把块号置进 `reclaim_pending` 位图，
 * 由后台 job 异步擦除后**归还固定 L2P 日志区**（置 NONE，不回用户 Free Pool）。
 *
 * ⚠ **本函数不擦 NAND**：它被 `ftl_l2p_callback`（L2P region 写回完成）调用，**运行在
 *   完成派发里** —— 就地擦会在完成泵内嵌套泵。登记后 `ftl_l2p_callback` 全路径无 NAND 访问。
 * ⚠ 压缩 job 在飞时**不登记**：压缩自己会把 victim 的活计数搬到 0，若此时就地
 *   登记，job 的 CP_RECLAIM 会在 Anchor 落盘**之前**擦 victim（③ 早于 ④ 契约）。
 *   压缩期间归零的块由 job 自己在 CP_RECLAIM 里擦。
 *
 * @param dir       Directory cache
 * @param region_id L2P region_id（内部调用者保证 < METADATA_DIR_ENTRIES）
 * @param mode      提交粒度（见 md_commit_mode_e）
 * @param new_primary 主副本新位置；MD_COMMIT_BACKUP 时忽略（传 NAND_PBA_INVALID 占位）
 * @param new_backup  备副本新位置；MD_COMMIT_PRIMARY 时忽略（传 NAND_PBA_INVALID 占位）
 */
void md_commit_region(md_dir_cache_t *dir, WORD region_id, md_commit_mode_e mode, DWORD new_primary, DWORD new_backup);

/* ── 第二副本（RISK-BOOT-18 的 L2）──
 * `md_backup_find` 返回该 region 的备份 PBA（NAND_PBA_INVALID = 无副本）。 */
DWORD md_backup_find(const md_dir_cache_t *dir, WORD region_id);

/* ── Anchor 落盘的三段式（唯一形态）──
 *
 * 一条 Anchor record = ANCHOR_PAGE_COUNT 页。**所有**落盘站点（Checkpoint job、
 * 目录压缩 job）都拆成 prepare →（可选换块擦）→ 逐页写 → commit，每步之间可以回到
 * 服务循环。三段式把「Directory 格式」留在 metadata.c，「状态机」留给调用者。
 */

/*
 * md_anchor_prepare - 定位游标 + 取槽 + 算是否需换块擦（**不动介质**）
 *
 * @param dir Directory
 * @param out 出参：池几何 / 地址 / 槽位 / sequence / 是否需先擦（仅 READY 时全部有效）
 * @return md_anchor_step_e：MD_ANCHOR_READY 需要写（出参全部有效）；
 *         MD_ANCHOR_CLEAN 无需写（dir 不脏）；MD_ANCHOR_FAILED 失败（定位失败）
 */
md_anchor_step_e md_anchor_prepare(md_dir_cache_t *dir, md_anchor_prepare_out_t *out);

/*
 * md_anchor_page - 组第 chunk 页 Anchor 数据（纯 RAM）
 *
 * @param dir    Directory
 * @param seq    本 record 的 sequence（prepare 的回传值）
 * @param chunk  页序（0..ANCHOR_PAGE_COUNT-1）
 * @param page   出参：组好的页（调用者提供 2KB 缓冲）
 */
void md_anchor_page(const md_dir_cache_t *dir, DWORD seq, BYTE chunk, md_anchor_page_t *page);

/*
 * md_anchor_commit - 收尾：推进游标（无论成败），成功才清 dirty
 *
 * @param dir   Directory
 * @param ring  prepare 回传的池几何
 * @param idx   prepare 回传的槽位号
 * @param ok    false = 有条分页写失败（整条作废，保留 dirty）
 */
void md_anchor_commit(md_dir_cache_t *dir, const md_ring_t *ring, WORD idx, bool ok);

/*
 * md_load - 从 Anchor 池加载 Directory
 *
 * ① 先 fill_fixed 铺基础层（全部 region = `NAND_PBA_INVALID`，即未映射）；
 * ② md_ring_locate 定位池内最新**整条校验通过**的 record；
 * ③ 读胜者各页装配覆盖对应区间（逐页再校验 chunk_index/sequence）；
 * ④ 由装配后的 entries 重建 l2p_live。
 *
 * **日志块游标不跨重启恢复**：cur_block_valid 置 false，首次追加时从固定 L2P 日志区
 * 新取一块（绝不复用可能已编程的旧页）。旧日志块仍被 Directory 引用 ⇒ 由 l2p_live 保住，
 * 待其 region 全部改写后由后台整理 job 异步擦除归还。
 * Anchor 游标则**由 locate 结果恢复**（anchor_write_idx / anchor_sequence），避免首次
 * flush 再扫一遍池。
 *
 * @param dir  Directory cache（输出）
 * @return FTL_OK / FTL_ERROR（无有效 Anchor：调用者转 md_init）
 */
ftl_status_e md_load(md_dir_cache_t *dir, alc_t *alloc, const DWORD *total_writes);

/*
 * md_pool_print - 打印四个固定分区的候选池与「被 spare 顶替」的块（CLI `pool`）
 *
 * @param dir 本实例 Directory（含 pools）；NULL 时打印警告
 */
void md_pool_print(const md_dir_cache_t *dir);

/*
 * md_pool_service - 运行期元数据 BAD 后的候选池重算（主循环调用，每个实例每轮一次）
 *
 * 只在 bbt_mark_bad 置 `alloc->pools->dirty` 后工作；等 CP/Journal/BBT 都没有
 * 在飞命令时重建候选池，并把各池游标置为「需重新定位」。纯 RAM 操作（BBT 持久化
 * 仍走后台 job），不在这里同步写介质。
 */
void md_pool_service(struct ftl_context *ftl);

/*
 * md_handle - 推进元数据目录整理 job（typed handle；提交即返回）
 *
 * 事件驱动（reclaim_pending / compact_wanted 由 Directory 函数置位），`info` 当前
 * 不参与决策；保留形参以统一 typed 入口。
 *
 * @param dir   Directory cache；**前置条件**（实例绑定由 ftl_contract_audit 证明）
 * @param info  本轮动态信息（当前未用，可传 NULL）
 * @param out   出参：本轮结束后的上报（**非空**）
 */
void md_handle(md_dir_cache_t *dir, const bg_info_t *info, md_status_t *out);

/*
 * md_is_busy - 整理 job 是否在跑（有命令在飞或处在中间 phase）。
 *
 * 用途：让**调用者**区分"我的重试为什么没进展"与"卡死"。当前唯一读者是
 * checkpoint.c 的 cp_regions_flush：脏 region 取不到槽（FTL_AGAIN）时若 job 在推进，
 * 就**不该**累计 CP 的 stall 计数——否则一次正常的长压缩会把 Checkpoint 打回去。
 *
 * @return true = 有 job 在跑；false = 空闲（或 dir == NULL）。
 */
static inline bool md_is_busy(const md_dir_cache_t *dir)
{
    return (dir != NULL) && (dir->handle.phase != MD_JOB_IDLE);
}

/*
 * md_block_is_metadata - 判断物理 Block 是否属于固定/动态元数据区
 *
 * 掉电恢复重建 closed_sb_list 时，L2P region 日志块、Anchor、BBT、Checkpoint、
 * Journal 等元数据块可能写满全部页（尤其 Journal 环形回切），逐页判空会把它们
 * 误判为满 Super Block。本谓词统一排除：
 *
 *   固定块：`block < FTL_METADATA_AREA_END`（0..97 全保留，含 shared metadata spare；
 *           名义 BAD 块也不回归用户池）
 *   游标块：当前正在追加的 L2P 日志块（可能尚无任何 region 指向它，Directory 扫不到）
 *   日志块：`l2p_live[g] != 0` —— 该账本的定义就是"最新位置落在 g 的 region 数"，
 *           与 Directory 条目一一对应，判据 O(1)
 *
 * free pool 回填（main.c）与重建（PFR 内部 full metadata rebuild）共用同一判据。
 *
 * ⚠ 游标块/日志块从用户区动态取，**必须带 plane 比较**；只比 per-plane 块号会连带
 *   掩掉另一 plane 的同号块（既出不了 free pool、又被 full metadata rebuild 跳过，
 *   白丢一个可用块）。固定块按整个保留区判定，不含未使用 plane 的同号块。
 *
 * @param md_dir Metadata Directory cache（可为 NULL：仅固定块区间判断）
 * @param plane  物理 plane 号（0..PLANE_NUM-1）
 * @param block  物理 Block 号（per-plane 号，0..BLOCKS_PER_PLANE-1）
 * @return true = 元数据块，false = 用户数据块
 */
bool md_block_is_metadata(const md_dir_cache_t *md_dir, BYTE plane, WORD block);

#endif /* FTL_METADATA_H */
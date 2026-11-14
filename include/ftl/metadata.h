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
 *      （14.4KB = 2048 条 × 7B），启动时从目录检查点 Block 加载。
 *
 * 目录检查点池（固定元数据区内的 DIR_CKPT_POOL_BLOCKS 个块，出厂好块预留）：
 *   只存 Metadata Directory。一份 Directory 有 DIR_CKPT_PAGE_COUNT 页（chunk），
 *   每页自包含 Magic + Sequence + ChunkIndex + EntryCount + Entries；
 *   **一条 record = 一整份 Directory（DIR_CKPT_PAGE_COUNT 页）**，任一条可独立重建
 *   整个 Directory。池内块顺序轮转（见 §元数据写模型），最新有效 record 胜。
 *
 * 元数据（固定预留区；L2P region 日志在固定 L2P 日志区内追加）:
 *   L2P Region       — 日志块（通过 Directory 查找）；目录条目 = NAND_PBA_INVALID
 *                      即该 region 从未写过（未映射），读取不经 NAND
 *   Journal          — 固定元数据区（块位置见上方「固定元数据区布局」）
 *   Checkpoint/BBT   — 固定元数据区（见上方「固定元数据区布局」）
 *
 * ── 元数据写模型（目录检查点 / Checkpoint / BBT / L2P 日志共用）──
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
#include "ftl_type.h"
#include "log.h"
#include "nand_config.h" /* PAGE_MAIN_SIZE（目录检查点页打包自检）*/
#include "timer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
 *   目录检查点      DIR_CKPT_POOL_BLOCKS（Directory 的块轮转日志池）
 *   Checkpoint  CHECKPOINT_POOL_BLOCKS（Header 的块轮转日志池）
 *   BBT         BBT_POOL_BLOCKS（坏块位图的块轮转日志池）
 *   Journal     JOURNAL_BLOCKS（策略：容纳两次 checkpoint 之间的映射更新）
 *
 * ── 当前几何下展开的布局（由下方公式算出，非手写）──
 *   块号        分区        块数  使用 plane  来历
 *   0..31       目录检查点      32    P0 + P1     元数据池：每块 4 份 Directory（16 页/份，主+备）；**P1 镜像**
 *   32..39      Checkpoint  8     P0 + P1     元数据池：每块 3 份 record；**P1 镜像**
 *   40..41      BBT         2     P0 + P1     元数据池：每块 64 份坏块位图；**P1 镜像**
 *   42..45      Journal     4     P0 + P1     策略 = CW_NUM；页地址按页号奇偶跨 plane（单副本交错）
 *   46..93      L2P log     48    P0 + P1     **固定预留**的 L2P region 日志区（见下；双副本）
 *   94..97      meta spare  4     P0 + P1     **共享**坏块替换池（仅补元数据分区 BAD 缺额）
 *   98..       用户数据    —     P0 + P1     FTL_METADATA_AREA_END 之后（磨损计数在 CP 快照内）
 *
 * ── L2P region 日志区（为什么固定预留）──
 * L2P 表本体 = L2P_REGIONS_PER_INSTANCE × 2 KB = 4 MiB（= 32 块）；每个 region 存
 * **主 + 备两份副本**（L2）⇒ 活数据 64 块 = 8 MiB；日志区再留 **1×**
 * 工作余量（current 块 / 压缩目标 / 坏块），共 L2P_LOG_BLOCKS_TOTAL = 96 块 = 12 MiB，
 * 平均每 plane L2P_LOG_BLOCKS_PER_PLANE = 48 块。该区与目录检查点/CP/BBT/Journal 一样
 * 属于**固定预留**；固定区满时从 **OP 池**有界借用（`L2P_LOG_BORROW_MAX=8`，借还闭环、
 * 不动用户容量），容量公式已扣除固定区。
 * 依据：主流主控把元数据放在独立预留区/独立介质（SPDK FTL 的 nvcache + band tail
 * metadata；spare 在 device formation 时从 surfaced capacity 扣除），不让元数据
 * 日志与用户数据动态争抢同一个空闲池。本仓旧做法（动态从 Free Pool 取 LOG 块）
 * 已被证明会把容量吃光。
 *
 * ⚠ **固定分区只预留 plane_mask 里的 plane**：`md_block_is_metadata()` 同时比较
 *   (plane & plane_mask)。目录检查点/CP/BBT/Journal 在 P1 都有元数据（镜像或交错）；
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
 *      目录检查点：16 页/份 ÷ 32 块 ⇒ 131,072（数据块的 1/16）。
 *      选 32 不是为了磨损（本仓远未到该量级），而是**主控形态**：固定根 +
 *      专用元数据块池（见 docs/design/storage_controller_design_essentials.md §一/§5.7）。
 *
 * ⚠ **每个 (ch,ce,lun) 用同一份块号**：本 FTL 是"分片模型"——每个 die 一个完整
 *   独立实例（各自的 L2P 表/目录检查点/BBT/Journal/Checkpoint），主机 LBA 空间
 *   按 region 切分到各实例（lba_2_* 用全局 region；映射页落盘位置用
 *   global_region / DIE_MAX 的局部 region，见 l2p_cache.h）。
 *   分区**不随 LUN/CE/CH 错开**。
 *
 * ⚠ 介质上的布局版本（`FTL_MEDIA_LAYOUT_VERSION`，写在 Checkpoint 头与目录检查点页内）
 *   与当前固件不符（或根本没有当前版本记录）⇒ `ssd_ftl_boot` 自动全片格式化一次并
 *   写入当前版本标记（见 power_failure_recovery.c `pfr_media_format`）。
 *   **任何改变介质格式的改动都必须把 `FTL_MEDIA_LAYOUT_VERSION` +1。**
 * ══════════════════════════════════════════════════════════════════ */

/* ── 池尺寸：策略常量 → 每个分区的块数 ── */

/* ── 目录检查点（Metadata Directory）池 ──
 * Directory 有 L2P_REGIONS_PER_INSTANCE 条（每条 7 B）⇒ **一页装不下**，须跨页。
 * 每页固定装 DIR_CKPT_ENTRIES_PER_PAGE 条（256×7 = 1792 ≤ 主区 2048）。
 * 一份完整 Directory = 一条 record = DIR_CKPT_PAGE_COUNT 页（chunk）。
 * 池内每块装 DIR_CKPT_RECORDS_PER_BLOCK 条 record；块满换池内下一块（**先擦后写**）。*/
#define DIR_CKPT_ENTRIES_PER_PAGE (256U)
/* 一份 Directory 分**主/备两半**（L2 冗余）：
 * chunk 0..PRIMARY-1 = 主 PBA（md_entry_s.pba），chunk PRIMARY..COUNT-1 = 备份 PBA
 * （md_entry_s.pba 承载 backup_pba[id]）。主半必须是**完整、同 sequence** 才算一条有效
 * record；备份半按页 best-effort（缺页只让对应 region 暂无双副本，不废整条记录）。 */
#define DIR_CKPT_DIR_PRIMARY_PAGES ((L2P_REGIONS_PER_INSTANCE + DIR_CKPT_ENTRIES_PER_PAGE - 1U) / DIR_CKPT_ENTRIES_PER_PAGE)
#define DIR_CKPT_DIR_BACKUP_PAGES  DIR_CKPT_DIR_PRIMARY_PAGES
#define DIR_CKPT_PAGE_COUNT        (DIR_CKPT_DIR_PRIMARY_PAGES + DIR_CKPT_DIR_BACKUP_PAGES) /* 16 */
#define DIR_CKPT_POOL_BLOCKS       (32U)                                                    /* 策略：池内块数（唯一磨损旋钮，见上）*/
#define DIR_CKPT_RECORDS_PER_BLOCK (PAGES_PER_BLOCK / DIR_CKPT_PAGE_COUNT)                  /* 4 */

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

/* 目录检查点池：池 = [DIR_CKPT_BLOCK0, DIR_CKPT_BLOCK0 + DIR_CKPT_POOL_BLOCKS) */
#define DIR_CKPT_BLOCK0 (FTL_METADATA_BASE_BLOCK)

/* Checkpoint 池：接在目录检查点池之后 */
#define CHECKPOINT_BLOCK0 (DIR_CKPT_BLOCK0 + DIR_CKPT_POOL_BLOCKS)

/* BBT 池：接在 Checkpoint 池之后 */
#define BBT_BLOCK (CHECKPOINT_BLOCK0 + CHECKPOINT_POOL_BLOCKS)

/* Journal：第 b 块 = JOURNAL_BLOCK0 + b（**环形 + 擦除**，唯一本来就正确的元数据）*/
#define JOURNAL_BLOCK0 (BBT_BLOCK + BBT_POOL_BLOCKS)

/* 固定元数据区末尾 = L2P 日志区的起点。
 * 磨损计数在 Checkpoint 快照内，不占独立块。 */
#define FTL_METADATA_BLOCK_END (JOURNAL_BLOCK0 + JOURNAL_BLOCK_COUNT)

/* ── L2P region 日志区（固定预留）──
 * 理想块数 = ceil(region 数 / 每块页数)（每页一个 region）；预留 2×（current /
 * 压缩目标 / 坏块余量），按 plane 均分。该区是固定元数据的一部分：
 *   · 固定区为主；满时从 OP 池有界借用（L2P_LOG_BORROW_MAX），不动 Free Pool；
 *   · 容量公式用 FTL_METADATA_AREA_END 扣掉它；
 *   · 恢复/重建时按固定分区跳过（md_block_is_metadata 分区表已含它）。 */
#define L2P_LOG_IDEAL_BLOCKS ((L2P_REGIONS_PER_INSTANCE + PAGES_PER_BLOCK - 1U) / PAGES_PER_BLOCK)
/* 每个 region 的**副本数**（L2）：主 + 备。
 * 活数据 = COPIES × ideal；日志区取 (COPIES+1) × ideal（多一份工作余量：
 * current 块 / 压缩目标 / 坏块）。 */
#define L2P_REGION_COPIES (2U)
/* 主/备各占**一个 plane** 的当前块（L2）⇒ 副本数不得超过 plane 数。 */
_Static_assert(L2P_REGION_COPIES <= PLANE_NUM, "L2P_REGION_COPIES 超过 PLANE_NUM（副本无法分 plane）");
#define L2P_LOG_BLOCKS_TOTAL     (3U * L2P_LOG_IDEAL_BLOCKS) /* 96 = 2 份活数据 + 1 份余量 */
#define L2P_LOG_BLOCKS_PER_PLANE (((L2P_LOG_BLOCKS_TOTAL + PLANE_NUM - 1U) / PLANE_NUM))
#define L2P_LOG_BLOCK0           (FTL_METADATA_BLOCK_END)

/* 压缩触发水位：活日志块**超过**两份副本的理想块数（> 64）就启动压缩。
 * ⚠ 不额外留 25% 碎片余量：固定日志区 3× ideal（96 块），活数据 = 2× ideal（64 块），
 *   越过 64 块即压缩，余量整 1× ideal（32 块）。一旦压缩跟不上，日志区会被吃满
 *   ⇒ md_log_rotate 无空闲块、append_slot 永远 INVALID ⇒ CP 写不成 Header、
 *   掉电后每次全量重建。 */
#define L2P_LOG_COMPACT_TRIGGER (L2P_LOG_IDEAL_BLOCKS * L2P_REGION_COPIES) /* 64：两份活数据的理想水位 */

/* 压缩的**主判据**：空闲日志块低于此水位就启动压缩（不论 live 数）。
 * 固定区 96 块；保留 8 块 ≈ 8×256 = 2048 个槽位，足够一轮把多个稀疏 victim
 * 搬空。free 触发比 live 触发早，保证压缩永远有目标块。 */
#define L2P_LOG_FREE_WATERMARK (8U)

/* 固定日志区满时，允许从 **OP 池**动态借用的日志块上限（总块数）。
 * 有界溢出：8 块 = 512 KiB，足够打破“没有目标块可搬”的死锁；再多会侵蚀用户 OP。
 * 借出的块由 l2p_live / md_block_is_metadata 动态识别，擦空后回 OP。 */
#define L2P_LOG_BORROW_MAX (8U)

/* L2P 日志区末尾（= shared metadata spare 起点）。 */
#define L2P_LOG_AREA_END (L2P_LOG_BLOCK0 + L2P_LOG_BLOCKS_PER_PLANE)

/* 在飞写回的槽位预留表容量。
 * 每次 L2P region 写回在 `md_slot_pair_append` 取两个槽后，到 commit/失败之前占两条
 * 表项；这块**不能被换出或回收**——槽里的数据还没进 Directory，`l2p_live` 看不见它。
 * 容量按写回并发上限取；`l2p_cache.c` 用静态断言钉住
 * `MD_SLOT_RESV_MAX >= L2P_FLUSH_MAX_INFLIGHT`（两层各有自己的数，靠断言防漂移）。 */
#define MD_SLOT_RESV_MAX (8U)

/* ── 共享 metadata spare（仅补固定元数据分区的 BAD 缺额）──
 * 4 块/plane 是用户选定的预算（= 2 MiB）；按 BBT BAD 集分配给
 * BBT / Checkpoint / Journal（目录检查点与 L2P 日志固定区足够大，只跳过 BAD）。
 * 这是「BBT + spare 替换」的主流坏块管理形态：不做 logical→physical remap 表
 *（判据见 docs/design/ftl_design.md §3.3.8）。
 * ⚠ 本仓自拟的具体算法（nominal 优先 + 共享 spare 补缺 + 按 sequence 扫描定位）
 *   不冒充行业统一数值。 */
#define MD_SPARE_BLOCK0           (L2P_LOG_AREA_END)
#define MD_SPARE_BLOCKS_PER_PLANE (4U)

/* 固定元数据区（含 L2P 日志区 + shared spare）末尾 = 第一个用户块。 */
#define FTL_METADATA_AREA_END (MD_SPARE_BLOCK0 + MD_SPARE_BLOCKS_PER_PLANE)

/**
 * @brief 块号是否落在固定 L2P 日志区（各 plane 区间相同）。
 * @param block per-plane 块号
 * @return true = 在固定日志区；false = 不在
 */
static inline BOOL ftl_is_l2p_log_block(WORD block)
{
    return (block >= L2P_LOG_BLOCK0) && (block < L2P_LOG_AREA_END);
}

/**
 * @brief 块号是否落在 shared metadata spare 区。
 * @param block per-plane 块号
 * @return true = 在 shared spare 区；false = 不在
 */
static inline BOOL ftl_is_metadata_spare_block(WORD block)
{
    return (block >= MD_SPARE_BLOCK0) && (block < FTL_METADATA_AREA_END);
}

/**
 * @brief gblock 是否落在固定 L2P 日志区。
 * @param g 全局块号（gblock）
 * @return true = 在固定日志区；false = 不在
 */
static inline BOOL ftl_is_l2p_log_gblock(WORD g)
{
    return ftl_is_l2p_log_block(ftl_gblock_block(g));
}

/* 固定池使用的 plane 集合：目录检查点/Checkpoint/BBT 用 multi-plane program 把同一页
 * 写全部 plane；Journal 用页号奇偶交错（i & 1）落在前两个 plane。
 * 由 PLANE_NUM 派生，不写死 0x03。 */
#define METADATA_MIRROR_PLANE_MASK (((1U << PLANE_NUM) - 1U))

/* ── 介质布局版本（NAND 上持久化格式的版本号）──
 * 任何改变**介质格式或块布局**的改动（结构体字段 / 块号 / 记录格式 / 字段位序语义）
 * 都必须把 BASE +1。开机 cp_pool_find_latest / dir_ckpt_page_valid 都校验它；不符
 * （或介质上根本没有当前版本的 Checkpoint）⇒ pfr_media_format() 自动全片格式化
 * （见 power_failure_recovery.c），不需要上板前手动擦芯片。
 * 版本演进表见 docs/design/ftl_revision_history.md「介质布局版本」。 */
#define FTL_MEDIA_LAYOUT_VERSION_BASE (14U) /* v14：目录检查点页头 +dirty_gen（整条记录的页间一致性校验） */

/*
 * 数据通路用**全局 LBA**（页 META / journal 存 host 全局 LBA）。
 * - DIE_MAX == 1：global == local，介质上的值逐位不变 ⇒ 版本保持 BASE，
 *   **不额外格式化**（与现有单 die 介质兼容）。
 * - DIE_MAX > 1：把 die 数 + topology map 哈希编入版本 ⇒ 不同拓扑（哪怕 die 数
 *   相同）也会被识别为版本不符，首次启用自动全片格式化。
 * 这是本仓自定的格式策略。
 */
DWORD ftl_media_layout_version(void);
#define FTL_MEDIA_LAYOUT_VERSION (ftl_media_layout_version())

/* ── 逻辑表字节（公式①：覆盖实体数 × 每实体字节）── */

/* 目录检查点：每份 Directory = DIR_CKPT_PAGE_COUNT 页 */
#define FTL_DIR_CKPT_TABLE_BYTES (DIR_CKPT_PAGE_COUNT * PAGE_MAIN_SIZE)

/* BBT：每块 1 bit */
#define FTL_BBT_TABLE_BYTES (BBT_BITMAP_SIZE)

/* Checkpoint：Header 1 页 */
#define FTL_CHECKPOINT_TABLE_BYTES (FTL_CHECKPOINT_RECORD_PAGES * PAGE_MAIN_SIZE)

/* Journal：块数 × 每块页数 × 页主区 */
#define FTL_JOURNAL_TABLE_BYTES (JOURNAL_BLOCK_COUNT * PAGES_PER_BLOCK * PAGE_MAIN_SIZE)

/* ── 分区表（固定元数据区的可枚举视图）──
 * 只在 metadata.c 内部使用：`md_block_is_metadata` 与文档化布局共用同一份数据。 */

typedef struct METADATA_PARTITION_S
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
} md_partition_s;

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
_Static_assert(DIR_CKPT_PAGE_COUNT <= PAGES_PER_BLOCK, "目录检查点一份 Directory 装不下一个块");
_Static_assert((PAGES_PER_BLOCK % DIR_CKPT_PAGE_COUNT) == 0U, "目录检查点每条 record 的页数必须整除每块页数");
_Static_assert(DIR_CKPT_RECORDS_PER_BLOCK >= 1U, "目录检查点池每块装不下一条 record");
_Static_assert(BBT_BITMAP_SIZE + 16U + 4U <= PAGE_MAIN_SIZE, "BBT 一条 record 装不下一页主区");
_Static_assert(DIR_CKPT_POOL_BLOCKS >= 2U, "目录检查点池至少 2 块");
_Static_assert(CHECKPOINT_POOL_BLOCKS >= 2U, "Checkpoint 池至少 2 块");
_Static_assert(BBT_POOL_BLOCKS >= 2U, "BBT 池至少 2 块");

/* 分区按块号升序、互不重叠 */
_Static_assert(DIR_CKPT_BLOCK0 + DIR_CKPT_POOL_BLOCKS <= CHECKPOINT_BLOCK0, "目录检查点与 Checkpoint 重叠");
_Static_assert(CHECKPOINT_BLOCK0 + CHECKPOINT_POOL_BLOCKS <= BBT_BLOCK, "Checkpoint 与 BBT 重叠");
_Static_assert(BBT_BLOCK + BBT_POOL_BLOCKS <= JOURNAL_BLOCK0, "BBT 与 Journal 重叠");
_Static_assert((JOURNAL_BLOCK0 + JOURNAL_BLOCK_COUNT) == FTL_METADATA_BLOCK_END, "固定区末尾必须紧接 Journal 之后");

/* ── 元数据类型 ──
 * Directory 当前只登记 METADATA_TYPE_L2P_REGION；其余类型是固定分区，位置由布局
 * 常量决定，不经过 Directory lookup。类型字段保留完整命名空间，供介质格式演进。 */
typedef enum METADATA_TYPE_E
{
    METADATA_TYPE_L2P_REGION = 0, /* L2P Region 数据 */
    METADATA_TYPE_JOURNAL    = 1, /* Journal Block */
    METADATA_TYPE_CHECKPOINT = 2, /* Checkpoint */
    METADATA_TYPE_BBT        = 3, /* Bad Block Table */
    METADATA_TYPE_COUNT      = 4,
} md_type_e;

/* ── Directory 容量 ──
 * = 每实例 L2P region 数：**每个 region 都必须能登记自己的位置**，否则
 * md_entry_update 的"满则丢弃"分支会把已落到动态块上的 region 位置丢掉
 * （重启后回落到"未映射" = 更新永久丢失）。容量取满即按构造消除该分支。*/
#define METADATA_DIR_ENTRIES          (L2P_REGIONS_PER_INSTANCE)
#define METADATA_DIR_ENTRIES_PER_PAGE (DIR_CKPT_ENTRIES_PER_PAGE)

/* ── 序列号锚定 ── */
#define DIR_CKPT_MAGIC (0x524F444EUL) /* "ANDIR" */

/* ── Entry: 7 字节（packed） ── */
#define METADATA_ENTRY_BYTES (7U)

typedef struct MD_ENTRY_S
{
    md_type_e type; /* METADATA_TYPE_* */
    WORD      id;   /* L2P region_id / 0 for others */
    DWORD     pba;  /* 物理位置（PBA，自带 CH/CE ⇒ 不依赖"本实例元数据必在本实例 ch/ce"假设）*/
} __attribute__((packed)) md_entry_s;

_Static_assert(sizeof(md_entry_s) == METADATA_ENTRY_BYTES, "md_entry_s 必须是 7 字节");

/* ── NAND 上持久化格式：**每页自包含** ──
 * Directory 有 METADATA_DIR_ENTRIES 条（14KB）⇒ 一份跨 DIR_CKPT_PAGE_COUNT 页，
 * 每页装 METADATA_DIR_ENTRIES_PER_PAGE 条，各自带 magic/sequence。
 * 单页损坏只丢该页（其余页仍可用），且读写只需一个 2KB 缓冲。
 * ⚠ 无本层 CRC——页完整性由下层 FC 的 per-CW CRC + ECC 保证。*/
#define METADATA_DIR_CKPT_HEADER_BYTES (24U)

typedef struct METADATA_DIRECTORY_CKPT_PAGE_S
{
    DWORD      magic;                                                                                                             /* DIR_CKPT_MAGIC */
    DWORD      sequence;                                                                                                          /* 单调递增（同一份的各 chunk 相同）*/
    WORD       chunk_index;                                                                                                       /* 0..DIR_CKPT_PAGE_COUNT-1 */
    WORD       chunk_count;                                                                                                       /* = DIR_CKPT_PAGE_COUNT */
    DWORD      entry_count;                                                                                                       /* 本 chunk 有效 entry 数 */
    DWORD      layout_version;                                                                                                    /* FTL_MEDIA_LAYOUT_VERSION（版本不符 ⇒ 该页作废）*/
    DWORD      dirty_gen;                                                                                                         /* 本页**填充时刻**的 Directory 代数*/
    md_entry_s entries[METADATA_DIR_ENTRIES_PER_PAGE];                                                                            /* 256 × 7 = 1792B */
    BYTE       padding[PAGE_MAIN_SIZE - METADATA_DIR_CKPT_HEADER_BYTES - (METADATA_DIR_ENTRIES_PER_PAGE * METADATA_ENTRY_BYTES)]; /* 填满一页主区 */
} md_dir_ckpt_page_s;

/* 页头前缀：廉价探针只读这 24 字节（整页 CRC/ECC 仍由 FC 校验）。 */
typedef struct METADATA_DIRECTORY_CKPT_HEADER_S
{
    DWORD magic;
    DWORD sequence;
    WORD  chunk_index;
    WORD  chunk_count;
    DWORD entry_count;
    DWORD layout_version;
    DWORD dirty_gen; /* 填充时刻的 Directory 代数：整条记录要求 16 页**全部相同***/
} md_dir_ckpt_hdr_s;

/* 一页必须装得下：头 24B + 每页条目（无独立 CRC，页完整性由 FC 的 per-CW CRC + ECC 保证）
 * ⚠ `dirty_gen` 的目的：一条 record 的 16 页跨多个服务循环写，期间目录可能更新。
 *   记录**每页填充时刻**的代数后，加载侧要求 16 页代数一致 ⇒ 混代记录整条作废、
 *   回退上一条一致记录（而不是把"页 0 旧代 / 页 N 新代"当有效目录加载）。*/
_Static_assert(sizeof(md_dir_ckpt_hdr_s) == METADATA_DIR_CKPT_HEADER_BYTES, "目录检查点页头前缀必须与结构体头部一致");
_Static_assert(offsetof(md_dir_ckpt_page_s, entries) == METADATA_DIR_CKPT_HEADER_BYTES, "目录检查点页头前缀与 entries 偏移不一致");
_Static_assert(sizeof(md_dir_ckpt_page_s) == PAGE_MAIN_SIZE, "目录检查点页结构体不等于一页主区");
_Static_assert((METADATA_DIR_ENTRIES % METADATA_DIR_ENTRIES_PER_PAGE) == 0U, "每页条目数不能整除总条目数");

/* ══════════════════════════════════════════════════════════════════
 * 元数据 record 池（环形轮转）—— 目录检查点 / Checkpoint / BBT 共用
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

/* 最大的候选池 = 目录检查点（其余分区都比它小）。 */
#define MD_RING_MAX_BLOCKS (DIR_CKPT_POOL_BLOCKS)

typedef struct METADATA_RING_S
{
    WORD first_block;                /* 名义区间首块（per-plane 块号；文档/校验用）*/
    WORD nominal_count;              /* 名义区间块数（不含 spare 补缺）*/
    WORD block_count;                /* 实际候选数（= blocks[] 有效项，>= 2）*/
    WORD blocks[MD_RING_MAX_BLOCKS]; /* 候选物理块号（per-plane）*/
    BYTE pages_per_record;           /* 一条 record 占几页（向下取整到每块 record 数；
                                      * 不能整除时块尾剩余页不用——metadata_ring 的地址公式已按 floor 处理）*/
    BYTE plane_mask;                 /* 池实际使用的 plane：bit0=P0, bit1=P1。
                                      * 0x03 = 镜像（同一 record 写两个 plane）；0x01 = 单副本。*/
} md_ring_s;

/**
 * @brief 候选池是否包含某个 per-plane 块号。
 * @param ring  池几何
 * @param block per-plane 块号
 * @return true = 包含；false = 不包含
 */
static inline BOOL md_ring_has_block(const md_ring_s *ring, WORD block)
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

/**
 * @brief plane_mask 里第 idx 个置位的 plane 号（idx 从 0 起）；越界返回 PLANE_NUM。
 * @param plane_mask plane 位掩码（bit0 = P0）
 * @param idx        置位 plane 的序号（从 0 起）
 * @return 对应 plane 号；越界返回 PLANE_NUM
 */
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

/**
 * @brief plane_mask 里的 plane 个数。
 * @param plane_mask plane 位掩码（bit0 = P0）
 * @return 置位的 plane 数
 */
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

/**
 * @brief 每块容纳的 record 数
 * @param ring 池几何
 * @return 每块 record 数（PAGES_PER_BLOCK / pages_per_record，向下取整）
 */
static inline WORD md_ring_records_per_block(const md_ring_s *ring)
{
    return (PAGES_PER_BLOCK / ring->pages_per_record);
}

/**
 * @brief 池内 record 槽位总数
 * @param ring 池几何
 * @return 槽位总数（block_count × 每块 record 数）
 */
static inline WORD md_ring_total_records(const md_ring_s *ring)
{
    return (ring->block_count * md_ring_records_per_block(ring));
}

/* 各池槽位数必须装得进 WORD（写游标 / record 下标都是 16 位）*/
_Static_assert((DIR_CKPT_POOL_BLOCKS * DIR_CKPT_RECORDS_PER_BLOCK) <= UINT16_MAX, "目录检查点池槽位数超出 WORD");
_Static_assert((CHECKPOINT_POOL_BLOCKS * PAGES_PER_BLOCK) <= UINT16_MAX, "Checkpoint 池槽位数超出 WORD");
_Static_assert((BBT_POOL_BLOCKS * PAGES_PER_BLOCK) <= UINT16_MAX, "BBT 池槽位数超出 WORD");

/* ── 四个固定元数据分区的候选池（每个实例一份，放 md_dir_cache_s）──
 * 只在 `md_pool_build_all()` 里生成；其余模块只读（H10/H13）。
 * ⚠ 不落盘：每次开机按 BBT BAD 集重算，顺序确定（nominal 升序 → spare 升序）。 */
typedef struct MD_POOLS_S
{
    md_ring_s dir_ckpt;
    md_ring_s cp;
    md_ring_s bbt;
    md_ring_s jrn;
    BOOL      dirty; /* 运行期元数据 BAD ⇒ 需 md_pool_service 重算（唯一写点：bbt_mark_bad） */
} md_pools_s;

/**
 * @brief 用 BBT BAD 集重建四个固定元数据分区的候选池
 *
 * 候选 = 名义区间内（plane_mask 所有 plane 都）非 BAD 的块；不足名义数时从
 * shared metadata spare 按固定顺序补。硬约束：
 *   BBT >= 2、Checkpoint >= 2、Journal == JOURNAL_BLOCK_COUNT；
 *   目录检查点只跳过 BAD（32 块固定区足够）。
 *
 * @param alloc 分配器（读 BST 坏块信息；其 pools 为候选池输出目标）
 * @return true = 全部满足
 *         false = 某分区凑不齐（release 下调用者必须拒绝启动）
 */
BOOL md_pool_build_all(alc_s *alloc);

/**
 * @brief record 槽位 → 芯片地址
 *
 * record_idx ∈ [0, total)；page_in_record ∈ [0, pages_per_record)。
 *
 * @param ring           池几何
 * @param topo           ch/ce/lun
 * @param record_idx     池内槽位号
 * @param page_in_record record 内的页序
 * @return 该槽位的 PBA（取 plane_mask 中第一个 plane）；几何或槽位非法返回 NAND_PBA_INVALID
 */
DWORD md_ring_addr(const md_ring_s *ring, const ftl_topo_s *topo, WORD record_idx, BYTE page_in_record);

/**
 * @brief 指定 plane 的同一个 record 槽地址（镜像池的读回退 / 写镜像用）。
 * plane 必须落在 ring->plane_mask 内；越界是调用者 bug ⇒ ASSERT。
 *
 * @param ring           池几何
 * @param topo           ch/ce/lun
 * @param record_idx     池内槽位号
 * @param page_in_record record 内的页序
 * @param plane          目标 plane 号
 * @return 该槽位的 PBA；几何或槽位非法返回 NAND_PBA_INVALID
 */
DWORD md_ring_addr_plane(const md_ring_s *ring, const ftl_topo_s *topo, WORD record_idx, BYTE page_in_record, BYTE plane);

/**
 * @brief 调用方的 record 探针（由 ring 模块回调）
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
typedef ftl_status_e (*metadata_ring_probe_fn_t)(void *ctx, WORD record_idx, BOOL full, DWORD *out_sequence);

/* md_ring_locate 的输出：最新有效 record + 写游标。 */
typedef struct METADATA_RING_LOCATE_OUT_S
{
    WORD  newest_idx; /* 最新有效 record 槽位；池空 = UINT16_MAX */
    DWORD sequence;   /* 该 record 的 sequence；池空 = 0 */
    WORD  write_idx;  /* 下一个可写槽位 */
} md_ring_locate_out_s;

/**
 * @brief 定位池内最新有效 record 与下一个可写槽
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
 * @return FTL_OK = 定位完成（含"池为空"）
 *         FTL_ERROR = ring 几何非法
 */
ftl_status_e md_ring_locate(const md_ring_s *ring, metadata_ring_probe_fn_t probe, void *ctx, md_ring_locate_out_s *out);

/**
 * @brief 算写点槽位地址 + 判定是否需先擦该块（**不动介质**）
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
ftl_status_e md_ring_prepare_addr(const md_ring_s *ring, const ftl_topo_s *topo, WORD write_idx, DWORD *out_addr, BOOL *out_need_erase);

/**
 * @brief 把写点槽位准备好（含换块擦除），返回该 record 第 0 页地址
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
ftl_status_e md_ring_prepare_write(const md_ring_s *ring, const ftl_topo_s *topo, WORD write_idx, DWORD *out_addr);

/* ── 元数据目录后台 job ──
 *
 * 承载 L2P 日志块的两件"要碰 NAND 的整理活"，让它们不在服务循环里阻塞：
 *   ① **回收**——某个日志块的活 region 数归零 ⇒ 擦块 + 归还固定 L2P 日志区；
 *   ② **压缩**——抗碎片：挑活 region 最少的块做 victim，把它的活 region 搬进当前
 *      块，落目录检查点后擦除归还。
 *
 * 阶段序列（每条命令一个 phase，完成由 g_op_callbacks[FTL_OP_METADATA] 收尾）：
 *
 *   IDLE ──reclaim_pending──▶ RECLAIM（擦 + 归还）──▶ IDLE
 *   IDLE ──compact_wanted──▶ CP_READ ⇄ CP_WRITE（逐 region 搬移）
 *                              └─▶ CP_DIR_CKPT_PREP ─▶ [CP_DIR_CKPT_ERASE] ─▶
 *                                  CP_DIR_CKPT_WRITE ×N ─▶ CP_RECLAIM（擦 victim）──▶ DONE ─▶ IDLE
 *
 * ⚠ **顺序即掉电安全契约**（不可倒）：
 *   ① 搬移 → ② 登记 → ③ 目录检查点落盘 → ④ 擦 victim → ⑤ 归还。
 *   ③ 必须早于 ④——否则掉电后目录检查点仍指向已擦块，那些 region 的映射永久丢失。
 *   job 里就是 phase 顺序：CP_DIR_CKPT_* 全在 CP_RECLAIM 之前。
 *
 * ⚠ **失败不回滚**：任一 phase 失败 ⇒ 中止，已登记的 region 已指向新页、未搬的仍指向
 *   victim（**victim 不擦**）⇒ 两边都有效，下次换块时重来即可。
 *
 * ⚠ **服务函数不推进状态机**（同 ftl_checkpoint_callback）：CP_DIR_CKPT_PREP 要调
 *   md_dir_ckpt_prepare（游标未建立时会**同步**扫池定位），在完成泵里跑就是
 *   嵌套泵。故 ftl_metadata_callback 只记结果 + release + 改 phase，推进一律留给下一轮
 *   md_handle（在 ftl_service 各档位里跑）。
 *
 * ⚠ 本结构不跨重启：phase / 游标 / 请求位全是 RAM 态，冷启动恒 IDLE（见 md_init）。 */
typedef enum METADATA_HANDLE_PHASE_E
{
    MD_JOB_IDLE = 0,          /* 无 job 在跑 */
    MD_JOB_RECLAIM,           /* 在飞：擦一个「已无引用」的日志块 */
    MD_JOB_CP_READ,           /* 在飞：读 victim 的下一个活 region 页 → cp_buf */
    MD_JOB_CP_WRITE,          /* 在飞：把 cp_buf 写进当前块的新槽 */
    MD_JOB_CP_DIR_CKPT_PREP,  /* 纯 RAM：定位目录检查点写点（冷启动时同步扫池）*/
    MD_JOB_CP_DIR_CKPT_ERASE, /* 在飞：目录检查点换块擦 */
    MD_JOB_CP_DIR_CKPT_WRITE, /* 在飞：目录检查点逐页写 */
    MD_JOB_CP_RECLAIM,        /* 在飞：擦 victim */
    MD_JOB_DONE               /* 收尾（纯 RAM）→ IDLE */
} md_handle_phase_e;

/**
 * @brief 压缩 job「搬一条 region」一步的结果
 *
 * md_compact_read / md_compact_write 共用本契约（write 永不返回 MD_COMPACT_SCANNED：
 * 写一条就回去读下一条）。调用者按**具名值**分派，不写数值魔法。
 */
typedef enum METADATA_COMPACT_STEP_E
{
    MD_COMPACT_BACKPRESSURE = 0U, /* 背压未提交：保留游标/槽，**下轮重试**（调用者 return）*/
    MD_COMPACT_SUBMITTED    = 1U, /* 已提交异步命令：等完成服务函数 */
    MD_COMPACT_SCANNED      = 2U, /* 全表扫完、无更多 region 待搬（read 专有）*/
    MD_COMPACT_FAILED       = 3U, /* 硬失败（md_handle_stall 无进展）：调用者 md_handle_abort */
} md_compact_step_e;

/**
 * @brief 压缩 job 能否启动
 *
 * ⚠ 与 md_compact_step_e 不共用：本处 0 = **已启动**，step 的 0 = 背压未提交。
 * 调用者用它区分「无活可搬」与「有活但当前块装不下」——后者需要换块才有目标槽。
 */
typedef enum METADATA_COMPACT_START_E
{
    MD_COMPACT_STARTED    = 0U, /* 已进入 CP_READ */
    MD_COMPACT_NO_CURRENT = 1U, /* 无当前写块：无处可搬 */
    MD_COMPACT_NO_VICTIM  = 2U, /* 无候选（零活块归 reclaim 管 / 只剩当前块） */
    MD_COMPACT_NO_SLOTS   = 3U, /* 当前块空槽装不下活 region 最少的 victim */
} md_compact_start_e;

/**
 * @brief md_dir_ckpt_prepare 的结果
 *
 * ⚠ 与 md_compact_step_e **不能合并**：本处 0（无需写）时调用者**推进 phase**（跳过
 *   目录检查点直接擦 victim），而 md_compact_* 的 0（背压）时调用者 **return 等下一轮**
 *   —— 同值反义，共用一套名字会把两处调用点读反。
 */
typedef enum METADATA_DIRECTORY_CKPT_STEP_E
{
    MD_DIR_CKPT_CLEAN  = 0U, /* dir 不脏、无需写目录检查点：调用者整段跳过 */
    MD_DIR_CKPT_READY  = 1U, /* 出参全部有效，可以写目录检查点 */
    MD_DIR_CKPT_FAILED = 2U, /* 定位/取槽失败：调用者中止 job */
    MD_DIR_CKPT_BUSY   = 3U, /* 另一个 writer（CP job / metadata job）正在写目录检查点：
                              * 背压重试，**不推进 sequence、不取槽** */
} md_dir_ckpt_step_e;

/* md_dir_ckpt_prepare 的输出：READY 时全部有效。 */
typedef struct METADATA_DIRECTORY_CKPT_PREPARE_OUT_S
{
    md_ring_s ring;       /* 目录检查点池几何（commit 要用） */
    DWORD     base;       /* 本 record 第 0 页地址 */
    WORD      idx;        /* 本 record 的池内槽位号（commit 要用） */
    DWORD     seq;        /* 本 record 的 sequence（prepare 内自增 dir_ckpt_sequence） */
    BOOL      need_erase; /* true = 写之前必须先擦 base 所在块 */
} md_dir_ckpt_prepare_out_s;

/* 目录检查点一次 16 页 record 的舞台（CP job 与 metadata job 共用）。
 * 只保存 RAM 状态；页提交/擦除提交仍由各 job 用自己的命令上下文完成。 */
typedef struct METADATA_DIRECTORY_CKPT_WRITE_S
{
    md_ring_s ring;
    DWORD     base;       /* record 第 0 页地址 */
    DWORD     seq;        /* prepare 时已自增的 sequence */
    WORD      idx;        /* 池内槽位 */
    BYTE      chunk;      /* 下一页要写的页序 */
    BOOL      ok;         /* 全部分页写成功 */
    BOOL      need_erase; /* 写之前先擦 base 所在块 */
    BOOL      skip;       /* Directory 无变更 ⇒ 整段跳过（CP 用） */
    BYTE      owner;      /* 单飞 owner（MD_DIR_CKPT_OWNER_*） */
} md_dir_ckpt_write_s;

/* 压缩搬移的副本号。 */
typedef enum METADATA_COPY_E
{
    MD_COPY_PRIMARY = 0, /* 主副本 */
    MD_COPY_BACKUP  = 1, /* 备份副本 */
} md_copy_e;

/* 布尔量直接用 BOOL；**不压位域**——位域布局是 implementation-defined，
 * `_Static_assert` 证不了（C4），且这里没有 RAM 压力。 */
typedef struct METADATA_HANDLE_S
{
    md_handle_phase_e   phase;                /* 异步 job 状态机 */
    BOOL                waiting;              /* 有命令在飞（完成服务函数清）*/
    BOOL                compact_wanted;       /* append_slot 换块后池吃紧 ⇒ 请求压缩 */
    BOOL                dir_ckpt_for_reclaim; /* 本次目录检查点仅为「回收前落盘」：完成后回 RECLAIM */
    WORD                victim;               /* 压缩 victim 的 gblock（UINT16_MAX = 无）*/
    WORD                region_id;            /* 搬移游标（下一个待查 region_id）*/
    WORD                cur_region;           /* 当前搬移中的 region_id（CP_READ 命中时设定）*/
    md_copy_e           cur_copy;             /* 当前搬移的副本（主 / 备）*/
    WORD                moved;                /* 本次压缩已搬 region 数（观测）*/
    WORD                reclaim_gblock;       /* 在飞擦除的块（完成时归还它）*/
    WORD                reclaim_fail_gblock;  /* 连续擦除失败计数对应的块*/
    BYTE                reclaim_fail_count;   /* 该块连续擦除失败次数；达上限即标坏并清位 */
    timer_deadline_s    submit_deadline;      /* 连续「提交不出去」deadline */
    BOOL                timed_out;            /* true = 本次 job 因 deadline 中止 */
    md_handle_phase_e   timeout_phase;        /* 超时发生的 phase */
    md_dir_ckpt_write_s dir_ckpt;             /* 目录检查点 16 页 record（CP/metadata 共用）*/
    DWORD               slot;                 /* 压缩搬移已取到、待写的槽（NAND_PBA_INVALID = 未取）*/
    DWORD               cur_source;           /* 当前搬移副本的源 PBA（commit 前 CAS 校验）*/
    BOOL                cur_fallback;         /* true = 已因源副本读失败切到另一副本 */
    DWORD               reclaimed_count;      /* 累计擦除成功并归还的日志块数（单调进度） */
    int32_t             error;                /* 最近一次中止的 FTL_*（回 IDLE 时清） */

    /* 心跳基准（见 log.h / timer.h）：job 在飞期间每满 1s 打一行 phase/victim/age */
    log_heartbeat_s hb;

    /* victim region 页搬运缓冲：**必须常驻**——异步读完成与随后的写之间隔着若干轮
     * 服务循环，栈帧早已返回。放 job 而非栈（同 mig_handle.page_buf / rr_page_scratch）。 */
    BYTE cp_buf[L2P_REGION_SIZE];
} md_handle_s;

/* ── 常驻 Directory Cache（33,752 B，`g_ftl_md[]` 落 SRAM4，见 src/ftl/metadata.c） ── */
/* 目录检查点的状态机（单飞 + 代数 + 游标）。 */
typedef struct METADATA_DIRECTORY_CKPT_S
{
    DWORD sequence;       /* 当前目录检查点 sequence */
    DWORD dirty_gen;      /* Directory 变更代数：每次 update 自增 */
    DWORD covered_gen;    /* 本次写入所覆盖的 dirty_gen 快照 */
    WORD  write_idx;      /* 池内下一个待写槽 */
    BOOL  cursor_valid;   /* 游标是否已建立 */
    BOOL  dirty;          /* 是否有未写回变更 */
    BOOL  busy;           /* 写入单飞 */
    BOOL  erase_required; /* 本次槽位在块首、写前需擦；abort 时据此跳过整块 */
    BYTE  owner;          /* 单飞持有者（MD_DIR_CKPT_OWNER_*） */
} md_dir_ckpt_s;

/* 每 plane 的 L2P 日志当前写块游标。 */
typedef struct METADATA_L2P_CUR_S
{
    WORD block[PLANE_NUM]; /* 当前日志块 gblock */
    BYTE page[PLANE_NUM];  /* 块内下一可用页 */
    BOOL valid[PLANE_NUM]; /* 游标是否已建立 */
} md_l2p_cur_s;

/* 页游标用 BYTE；每块 256 页时游标会回绕到 0。 */
_Static_assert(PAGES_PER_BLOCK < 256U, "PAGES_PER_BLOCK 超出 BYTE 页游标值域");

/* 待回收日志块位图 + 轮转起点。 */
typedef struct METADATA_RECLAIM_S
{
    DWORD bitmap[FTL_BLOCK_TOTAL / 32U];
    WORD  count;
    WORD  scan; /* 轮转起点：擦除持续失败的块不再永久挡住后续块 */
} md_reclaim_s;

typedef struct MD_DIRECTORY_CACHE_S
{
    md_entry_s entries[METADATA_DIR_ENTRIES];
    /* L2P region 的第二份副本 PBA（L2）。 */
    DWORD backup_pba[METADATA_DIR_ENTRIES];
    WORD  count; /* 有效 entry 数 */

    md_dir_ckpt_s ckpt; /* 目录检查点状态机 */

    /* L2P 日志块活计数（每个 gblock 上还有多少 region 的最新位置落在这里）。 */
    WORD l2p_live[FTL_BLOCK_TOTAL];

    md_l2p_cur_s cur;              /* 每 plane 的当前写块游标 */
    WORD         log_borrow_count; /* 从 OP 池借用的日志块数（≤ L2P_LOG_BORROW_MAX） */

    /* 在飞写回的槽位预留：写命令还在飞、Directory 尚未 commit。 */
    DWORD slot_resv[MD_SLOT_RESV_MAX];

    md_reclaim_s reclaim; /* 待回收日志块位图 + 轮转起点 */

    md_handle_s handle; /* 后台整理 job（回收 + 压缩） */

    md_pools_s   pools; /* 固定元数据分区的候选块池 */
    ftl_topo_s   topo;  /* 元数据所在芯片路由 */
    alc_s       *alloc;
    const DWORD *total_writes;
} md_dir_cache_s;

/* 待回收位图按 32 位字切分，块总数必须能整除一个字。 */
_Static_assert((FTL_BLOCK_TOTAL % 32U) == 0U, "FTL_BLOCK_TOTAL 必须是 32 的倍数");

/* ── API ── */

/**
 * @brief 初始化 Metadata Directory
 *
 * 首次使用时为全部 L2P Region 登记 `NAND_PBA_INVALID`（= 未映射；读取该 region
 * 不需要访问 NAND）。写过并刷盘的 region 由 md_commit_region 覆盖成
 * 日志块上的真实位置。
 *
 * 每 plane 游标全部置**无效**（cur_valid[p] = false / dir_ckpt_cursor_valid = 0）：首次追加
 * 才从固定 L2P 日志区取块，避免"没拿块就用"。
 *
 * @param dir          Directory cache（由调用者提供存储）
 * @param alloc        分配器
 * @param total_writes 累计主机写计数指针（仅持有）
 */
void md_init(md_dir_cache_s *dir, alc_s *alloc, const DWORD *total_writes);

/**
 * @brief 查找元数据物理位置
 *
 * @param dir  Directory cache
 * @param type 元数据类型
 * @param id   L2P region_id / 0 for others
 * @return PBA；NAND_PBA_INVALID = 未找到。对 L2P region 而言"未找到"与"未映射"
 *         同义：`fill_fixed` 为每个 region 预登记一条，之后只在刷盘时覆盖。
 */
DWORD md_entry_find(const md_dir_cache_s *dir, md_type_e type, WORD id);

/**
 * @brief 某个 gblock 上还有多少 region 的**最新位置**落在它上面。
 *
 * 供开机恢复（PFR）判断「该块是否仍被 Directory 引用」。越界返回 0（不是 ASSERT：
 * 调用者的 g 来自介质/快照，属不可信输入，H29）。
 *
 * @param dir    Directory cache
 * @param gblock 全局块号（gblock）
 * @return 该块上的活 region 数；gblock 越界返回 0
 */
WORD md_live_count_get(const md_dir_cache_s *dir, WORD gblock);

/**
 * @brief 添加或更新元数据位置
 *
 * @param dir    Directory cache
 * @param type   元数据类型
 * @param id     L2P region_id / 0 for others
 * @param new_pba 新物理位置（PBA）
 */
void md_entry_update(md_dir_cache_s *dir, md_type_e type, WORD id, DWORD new_pba);

/**
 * @brief 一次预留**两个连续槽**（主/备副本，的 L2）
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
 * 主副本固定写 plane0 的当前块、备副本固定写 plane1 的当前块（写入目标与 region 归属
 * 无关：位置由 pba 自身携带，读回一律经 Directory 查 pba）。
 *
 * ⚠ **本函数不做压缩**：换块后若活日志块数超水位，只登记 `job.compact_wanted`——
 *   压缩由 md_handle 起异步 job 做，避免在写回路径上同步阻塞数十毫秒。
 * ⚠ **整理 job 在飞时一律返回 false**：压缩的空槽预算按发放那一刻的
 *   cur_page[plane] 核定，期间再发放会打乱预算与 ③④ 顺序。调用者
 *   （l2p_flush_kick）据此返回 FTL_AGAIN，下轮重试。
 *
 * @param dir  Directory cache
 * @param alloc Allocator
 * @param out  输出两个 PBA（主、备）
 * @return true = 两槽都取到
 *         false = 背压，未取槽
 *
 * @note 成功返回即占用一个 `slot_resv` 表项（主副本 PBA 标识）。调用者**必须**在该
 *       次写回的终态（commit 成功，或建链/提交/写失败）调用一次 `md_slot_release`。
 *       漏释放的方向是"块暂时不可回收"（容量损失，可见），不会写坏介质。
 */
BOOL md_slot_pair_append(md_dir_cache_s *dir, alc_s *alloc, DWORD out[L2P_REGION_COPIES]);

/**
 * @brief 释放在飞写回的槽位预留（与 `md_slot_pair_append` 严格配对）。
 *
 * @param dir     Directory cache
 * @param primary 该次写回 `md_slot_pair_append` 返回的主副本 PBA
 * @param backup  同一次写回返回的备副本 PBA（两个副本在不同 plane 的块上，各有一条预留）
 *
 * @note 找不到对应表项（重复释放 / 传错 PBA）只触发 debug 断言，不改账本。
 */
void md_slot_release(md_dir_cache_s *dir, DWORD primary, DWORD backup);

/**
 * @brief 该 gblock 上是否有尚未 commit/失败的在飞写回槽位。
 *
 * 纯读谓词，供回收/换块判据使用（`md_log_block_is_idle` / `md_log_rotate`）。
 * 实现见 `metadata.c` 第九节（`SSD_CODE_FAST`，非 ITCM 热路径）。
 *
 * @param dir Directory cache
 * @param g   候选 gblock 编号
 * @return true = 有在飞槽位（块不可换出/回收）
 *         false = 无（或 g 越界）
 */
BOOL md_slot_reserved(const md_dir_cache_s *dir, WORD g);

/**
 * @brief 清空槽位预留表（开机加载 / 格式化时调用：RAM 态，不跨重启）。
 *
 * @param dir Directory cache
 */
void md_slot_reset(md_dir_cache_s *dir);

/* ── 固定 L2P 日志区──
 * 这 64 个 gblock 是**固定预留**的元数据区：只由 metadata 模块在
 * [L2P_LOG_BLOCK0, FTL_METADATA_AREA_END) 内部分配/归还，不经过 allocator 的
 * Free/OP/Spare 池。块状态约定：
 *   LOG   = 当前写块 / 有活 region / 残留旧数据（待回收）
 *   NONE  = 已擦除、空闲可用
 *   BAD   = 终态，池容量永久减一
 * 依据：主流主控把元数据放在独立预留区（SPDK FTL 的 nvcache + band tail），
 * 元数据空间有界、可预测；若从 Free Pool 动态取块，日志碎片会吃掉用户容量。 */

/**
 * @brief 开机按 Directory 恢复固定日志区的块状态
 * @param dir   已由目录检查点加载好的 Directory（l2p_live / cur_block[] 有效）
 * @param alloc 分配器（读 BST；本函数不改 Free/OP/Spare 池）
 *
 * 活块与当前写块置 LOG；非活块**一律**置 LOG + 登记回收（后台擦除后由 md_log_release
 * 置 NONE）——**不读介质判「是否已擦净」**：FTL 只看得见 USER 512B，判不出
 * 「编程过没有」，判错就是在未擦页上编程。擦除按空闲水位懒执行
 * （`md_reclaim_is_eager`），开机只补到水位，不会把日志区重擦一遍。
 */
void md_log_area_restore(md_dir_cache_s *dir, alc_s *alloc);

/**
 * @brief 取一个日志块（置 LOG，独占）
 *
 * 顺序：① 固定 L2P 日志区的空闲块（NONE）；
 *       ② 固定区满时从 **OP 池**有界借用（log_borrow_count < L2P_LOG_BORROW_MAX）。
 * 借来的块由 l2p_live / md_block_is_metadata 动态识别，擦空后回 OP。
 *
 * @param dir   Directory cache
 * @param alloc 分配器（读/改 BST 块状态）
 * @param plane 首选 plane 号
 * @return gblock；固定区与 Free Pool 都无可用块 ⇒ FTL_GBLOCK_INVALID（调用者按背压处理）
 */
WORD md_log_alloc(md_dir_cache_s *dir, alc_s *alloc, BYTE plane);

/**
 * @brief 换**指定 plane** 的当前写块（每 plane 一个当前块，L2）
 *
 * 两个调用者：
 *   · 追加路径（md_slot_pair_append）：该 plane 的当前块写满/无游标时换块；
 *   · 整理 job（md_handle_step）：压缩因该 plane 当前块空槽不足起不动时**强制换块**。
 *     压缩的 destination 就是 victim 所在 plane 的当前块；换块原本只由追加触发
 *     ⇒ host 写停止后没有追加 ⇒ 不换块 ⇒ 压缩永远拿不到整块空槽，日志区锁死。
 *
 * 旧当前块若已无活 region ⇒ 登记回收；换块时扫描全池只登记不擦。
 *
 * @param dir   Directory cache（换块并更新该 plane 的游标）
 * @param alloc 分配器（取块 / 读块状态）
 * @param plane 目标 plane（主副本 → 0、备副本 → 1）
 * @return true = 已换到新块（cur_page[plane] == 0）；
 *         false = 没换（该 plane 固定区无空闲块且借用已满，或刚换块但活块数超水位）
 */
BOOL md_log_rotate(md_dir_cache_s *dir, alc_s *alloc, BYTE plane);

/**
 * @brief 擦除成功后归还一个日志块
 * @param dir    Directory（取 alloc 与借用计数）
 * @param gblock 固定日志区内的块 ⇒ 置 NONE 留在固定区；
 *               借来的块 ⇒ 回 OP 池并减 log_borrow_count。
 */
void md_log_release(md_dir_cache_s *dir, WORD gblock);

/**
 * @brief 固定日志区的空闲块数（state == NONE 且非 BAD）。压缩按它触发（free 水位）。
 * @param alloc 分配器（读 BST 块状态）
 * @return 空闲日志块数（全部 plane 累计）
 */
WORD md_log_area_free_count(const alc_s *alloc);

/**
 * @brief 当前仍有活 region 的日志块数（观测：应保持有界，理想 ~32）。
 * @param dir Directory cache
 * @return 活日志块数（扫描固定 L2P 日志区全部 plane）
 */
WORD md_live_block_count_get(const md_dir_cache_s *dir);

/* md_commit_region 的提交粒度（单副本 / 成对）。 */
typedef enum METADATA_COMMIT_MODE_E
{
    MD_COMMIT_PRIMARY = 0U, /* 只提交主副本 */
    MD_COMMIT_BACKUP  = 1U, /* 只提交备副本 */
    MD_COMMIT_PAIR    = 2U, /* 主/备同批提交：两份都写成功后调用 */
    MD_COMMIT_MODE_NUM,
} md_commit_mode_e;

/**
 * @brief 写成功后登记 region 的副本位置
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
 *   登记，job 的 CP_RECLAIM 会在目录检查点落盘**之前**擦 victim（③ 早于 ④ 契约）。
 *   压缩期间归零的块由 job 自己在 CP_RECLAIM 里擦。
 *
 * @param dir       Directory cache
 * @param region_id L2P region_id（内部调用者保证 < METADATA_DIR_ENTRIES）
 * @param mode      提交粒度（见 md_commit_mode_e）
 * @param new_primary 主副本新位置；MD_COMMIT_BACKUP 时忽略（传 NAND_PBA_INVALID 占位）
 * @param new_backup  备副本新位置；MD_COMMIT_PRIMARY 时忽略（传 NAND_PBA_INVALID 占位）
 */
void md_commit_region(md_dir_cache_s *dir, WORD region_id, md_commit_mode_e mode, DWORD new_primary, DWORD new_backup);

/* ── 第二副本（L2）── */
/**
 * @brief 每 plane 的当前写块游标是否在界内（纯读；上板自检用）。
 *
 * @param dir Directory cache
 * @return true = 全部 plane 的游标合法（未建立也算合法）
 */
static inline BOOL md_cursor_bounds_ok(const md_dir_cache_s *dir)
{
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        if (dir->cur.valid[p] && ((dir->cur.block[p] >= FTL_BLOCK_TOTAL) || (dir->cur.page[p] > PAGES_PER_BLOCK)))
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 查该 region 的备份副本 PBA
 * @param dir       Directory cache
 * @param region_id L2P region_id
 * @return 备份 PBA；NAND_PBA_INVALID = 无副本（或 region_id 越界）
 */
static inline DWORD md_backup_find(const md_dir_cache_s *dir, WORD region_id)
{
    return (region_id < METADATA_DIR_ENTRIES) ? dir->backup_pba[region_id] : NAND_PBA_INVALID;
}

/* ── 目录检查点落盘的三段式（唯一形态）──
 *
 * 一条目录检查点记录 = DIR_CKPT_PAGE_COUNT 页。**所有**落盘站点（Checkpoint job、
 * 目录压缩 job）都拆成 prepare →（可选换块擦）→ 逐页写 → commit，每步之间可以回到
 * 服务循环。三段式把「Directory 格式」留在 metadata.c，「状态机」留给调用者。
 */

/**
 * @brief 定位游标 + 取槽 + 算是否需换块擦（**不动介质**）
 *
 * @param dir Directory
 * @param out 出参：池几何 / 地址 / 槽位 / sequence / 是否需先擦（仅 READY 时全部有效）
 * @return md_dir_ckpt_step_e：MD_DIR_CKPT_READY 需要写（出参全部有效）；
 *         MD_DIR_CKPT_CLEAN 无需写（dir 不脏）；MD_DIR_CKPT_FAILED 失败（定位失败）
 */
/* 目录检查点单飞持有者：abort 只释放**自己**的锁。 */
#define MD_DIR_CKPT_OWNER_NONE (0U)
#define MD_DIR_CKPT_OWNER_CP   (1U) /* Checkpoint job（CP_DIR_CKPT_*） */
#define MD_DIR_CKPT_OWNER_MD   (2U) /* metadata 整理 job（MD_JOB_CP_DIR_CKPT_*） */
#define MD_DIR_CKPT_OWNER_BOOT (3U) /* 格式化 / 开机初始目录检查点 */

md_dir_ckpt_step_e md_dir_ckpt_prepare(md_dir_cache_s *dir, md_dir_ckpt_prepare_out_s *out, BYTE owner);

/**
 * @brief 放弃一次已 prepare 的目录检查点写（清 `dir_ckpt_busy`）。
 *
 * @param dir 目录。
 *
 * @note 供 job 的中止 / 复位路径调用：prepare 之后没走到 commit 就放弃时，必须清
 *       单飞标志，否则另一个 writer 永远拿不到锁。
 */
void md_dir_ckpt_abort(md_dir_cache_s *dir, BYTE owner);

/**
 * @brief 组第 chunk 页目录检查点数据（纯 RAM）
 *
 * @param dir    Directory
 * @param seq    本 record 的 sequence（prepare 的回传值）
 * @param chunk  页序（0..DIR_CKPT_PAGE_COUNT-1）
 * @param page   出参：组好的页（调用者提供 2KB 缓冲）
 */
void md_dir_ckpt_page(const md_dir_cache_s *dir, DWORD seq, BYTE chunk, md_dir_ckpt_page_s *page);

/**
 * @brief 开始一次目录检查点 record 写入（prepare + 填充共享上下文）
 *
 * @param dir   Directory
 * @param w     共享上下文（出参）
 * @param owner 单飞持有者
 * @return md_dir_ckpt_step_e（CLEAN/READY/BUSY/FAILED）
 */
md_dir_ckpt_step_e md_dir_ckpt_write_begin(md_dir_cache_s *dir, md_dir_ckpt_write_s *w, BYTE owner);

/**
 * @brief 一页写完：结算 ok/chunk，返回整条 record 是否写完
 *
 * @param w      共享上下文
 * @param failed 本页是否失败
 * @return true = 16 页已全部处理完（调用者随后 commit）；false = 还需写下一页
 */
BOOL md_dir_ckpt_write_advance(md_dir_ckpt_write_s *w, BOOL failed);

/**
 * @brief 中止共享上下文持有的目录检查点单飞
 *
 * @param dir Directory
 * @param w   共享上下文
 *
 * @note 持有者有效时**先推进写游标再释放锁**：中止的那条 record 可能只写了半条，
 *       同一槽不可复用（未擦页二次编程）。块首槽（begin 时需擦块）中止后跳到
 *       下一块的槽 0。owner 已被 commit 清空时本函数是 no-op。
 */
void md_dir_ckpt_write_abort(md_dir_cache_s *dir, md_dir_ckpt_write_s *w);

/**
 * @brief record 基址 + chunk → 该 chunk 的页地址（单一转换漏斗）
 *
 * @param base  record 第 0 页 PBA
 * @param chunk 页序（0..DIR_CKPT_PAGE_COUNT-1）
 * @return 页 PBA
 */
static inline DWORD md_dir_ckpt_page_addr(DWORD base, BYTE chunk)
{
    return pba_with_page(base, pba_to_page(base) + chunk);
}

/**
 * @brief 收尾：推进游标（无论成败），成功才清 dirty
 *
 * 同时清空上下文的 owner ⇒ 之后的 abort 是 no-op（不会再推进一次游标）。
 *
 * @param dir Directory
 * @param w   共享上下文（begin 填充；ok == false 表示有条分页写失败，整条作废）
 */
void md_dir_ckpt_commit(md_dir_cache_s *dir, md_dir_ckpt_write_s *w);

/**
 * @brief 从目录检查点池加载 Directory
 *
 * ① 先 fill_fixed 铺基础层（全部 region = `NAND_PBA_INVALID`，即未映射）；
 * ② md_ring_locate 定位池内最新**整条校验通过**的 record；
 * ③ 读胜者各页装配覆盖对应区间（逐页再校验 chunk_index/sequence）；
 * ④ 由装配后的 entries 重建 l2p_live。
 *
 * **日志块游标不跨重启恢复**：cur_valid[] 置 false，首次追加时从固定 L2P 日志区
 * 新取一块（绝不复用可能已编程的旧页）。旧日志块仍被 Directory 引用 ⇒ 由 l2p_live 保住，
 * 待其 region 全部改写后由后台整理 job 异步擦除归还。
 * 目录检查点游标则**由 locate 结果恢复**（dir_ckpt_write_idx / dir_ckpt_sequence），避免首次
 * flush 再扫一遍池。
 *
 * @param dir          Directory cache（输出）
 * @param alloc        分配器
 * @param total_writes 累计主机写计数指针（仅持有）
 * @return FTL_OK / FTL_ERROR（无有效目录检查点：调用者转 md_init）
 */
ftl_status_e md_load(md_dir_cache_s *dir, alc_s *alloc, const DWORD *total_writes);

/**
 * @brief 打印四个固定分区的候选池与「被 spare 顶替」的块（CLI `pool`）
 *
 * @param dir 本实例 Directory（含 pools）；NULL 时打印警告
 */
void md_pool_print(const md_dir_cache_s *dir);

/**
 * @brief 运行期元数据 BAD 后的候选池重算（主循环调用，每个实例每轮一次）
 *
 * 只在 bbt_mark_bad 置 `alloc->pools->dirty` 后工作；等 CP/Journal/BBT 都没有
 * 在飞命令时重建候选池，并把各池游标置为「需重新定位」。纯 RAM 操作（BBT 持久化
 * 仍走后台 job），不在这里同步写介质。
 *
 * @param ftl FTL 实例上下文（提供 alloc / metadata_dir / journal）
 *
 * @note ⚠ 调用方必须把它排在**本轮新 I/O 之前**：游标与候选表要先一致，随后才允许
 *       分配与提交；否则同一轮里会出现「按旧候选表分配、按新表定位」。
 */
void md_pool_service(ftl_ctx_s *ftl);

/**
 * @brief 推进元数据目录整理 job（typed handle；提交即返回）
 *
 * 事件驱动（reclaim_pending / compact_wanted 由 Directory 函数置位）。
 *
 * @param dir   Directory cache；**前置条件**（实例绑定由 ftl_contract_audit 证明）
 */
void md_handle(md_dir_cache_s *dir);

/**
 * @brief 整理 job 是否在跑（有命令在飞或处在中间 phase）。
 *
 * 用途：让**调用者**区分"我的重试为什么没进展"与"卡死"。当前唯一读者是
 * checkpoint.c 的 cp_regions_flush：脏 region 取不到槽（FTL_AGAIN）时若 job 在推进，
 * 就**不该**累计 CP 的 stall 计数——否则一次正常的长压缩会把 Checkpoint 打回去。
 *
 * @param dir Directory cache（可为 NULL）
 * @return true = 有 job 在跑
 *         false = 空闲（或 dir == NULL）。
 */
static inline BOOL md_is_busy(const md_dir_cache_s *dir)
{
    return (dir != NULL) && (dir->handle.phase != MD_JOB_IDLE);
}

/**
 * @brief 判断物理 Block 是否属于固定/动态元数据区
 *
 * 掉电恢复重建 closed_sb_list 时，L2P region 日志块、目录检查点、BBT、Checkpoint、
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
BOOL md_block_is_metadata(const md_dir_cache_s *md_dir, BYTE plane, WORD block);

#endif /* FTL_METADATA_H */
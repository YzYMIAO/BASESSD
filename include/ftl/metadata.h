#ifndef FTL_METADATA_H
#define FTL_METADATA_H

/*
 * Metadata — 固定元数据区布局（单一事实来源）+ Metadata Directory（动态位置追踪）
 *
 * 本头文件有两部分，前者是后者的前提：
 *   ① **固定元数据区布局**（见下方「固定元数据区布局」大区）——哪个分区占哪些块、
 *      每个池多大、每条 record 占几页、介质布局版本。全部由 ftl_config.h §一 的几何量
 *      推出，**没有任何块号是写死的**。运行期分区表 `g_ftl_partitions[]` 在 metadata.c。
 *   ② **Metadata Directory** ——元数据和用户数据共享 Free Pool，通过 Directory 追踪
 *      每份元数据的当前物理位置。Directory SRAM 常驻（14.4KB = 2048 条 × 7B），
 *      启动时从 Anchor Block 加载。
 *
 * Anchor 池（固定元数据区内的 ANCHOR_POOL_BLOCKS 个块，出厂好块预留）：
 *   只存 Metadata Directory。一份 Directory 有 ANCHOR_PAGE_COUNT 页（chunk），
 *   每页自包含 Magic + Sequence + ChunkIndex + EntryCount + Entries；
 *   **一条 record = 一整份 Directory（ANCHOR_PAGE_COUNT 页）**，任一条可独立重建
 *   整个 Directory。池内块顺序轮转（见 §元数据写模型），最新有效 record 胜。
 *
 * 元数据（从 Free Pool 动态分配）:
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
#include "ftl_module.h" /* ftl_bg_info_t / ftl_module_status_t */
#include "ftl_type.h"
#include "nand_config.h" /* PAGE_MAIN_SIZE（Anchor 页打包自检）*/

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
 *   Anchor      ANCHOR_POOL_BLOCKS（Directory 的块轮转日志池）
 *   Checkpoint  CHECKPOINT_POOL_BLOCKS（Header 的块轮转日志池）
 *   BBT         BBT_POOL_BLOCKS（坏块位图的块轮转日志池）
 *   Journal     JOURNAL_BLOCKS（策略：容纳两次 checkpoint 之间的映射更新）
 *
 * ── 当前几何下展开的布局（由下方公式算出，非手写）──
 *   块号        分区        块数  使用 plane  来历
 *   0..31       Anchor      32    P0 + P1     元数据池：每块 8 份 Directory；**P1 镜像**
 *   32..39      Checkpoint  8     P0 + P1     元数据池：每块 3 份 record；**P1 镜像**
 *   40..41      BBT         2     P0 + P1     元数据池：每块 64 份坏块位图；**P1 镜像**
 *   42..45      Journal     4     P0 + P1     策略 = CW_NUM；页地址按页号奇偶跨 plane（单副本交错）
 *   46..       用户数据    —     P0 + P1     FTL_METADATA_BLOCK_END 之后（磨损计数在 CP 快照内）
 *
 * ⚠ **固定分区只预留 plane_mask 里的 plane**：`ftl_is_metadata_block()` 同时比较
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
 *      Anchor：8 页/份 ÷ 32 块 ⇒ 8×（≈3120 DW，数据块 ≈25000 DW）。
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
 *   写入当前版本标记（见 power_failure_recovery.c `ftl_media_format`）。
 *   **任何改变介质格式的改动都必须把 `FTL_MEDIA_LAYOUT_VERSION` +1。**
 * ══════════════════════════════════════════════════════════════════ */

/* ── 池尺寸：策略常量 → 每个分区的块数 ── */

/* ── Anchor（Metadata Directory）池 ──
 * Directory 有 L2P_REGIONS_PER_INSTANCE 条（每条 7 B）⇒ **一页装不下**，须跨页。
 * 每页固定装 ANCHOR_ENTRIES_PER_PAGE 条（256×7 = 1792 ≤ 主区 2048）。
 * 一份完整 Directory = 一条 record = ANCHOR_PAGE_COUNT 页（chunk）。
 * 池内每块装 ANCHOR_RECORDS_PER_BLOCK 条 record；块满换池内下一块（**先擦后写**）。*/
#define ANCHOR_ENTRIES_PER_PAGE  (256U)
#define ANCHOR_PAGE_COUNT        ((L2P_REGIONS_PER_INSTANCE + ANCHOR_ENTRIES_PER_PAGE - 1U) / ANCHOR_ENTRIES_PER_PAGE)
#define ANCHOR_POOL_BLOCKS       (32U) /* 策略：池内块数（唯一磨损旋钮，见上）*/
#define ANCHOR_RECORDS_PER_BLOCK (PAGES_PER_BLOCK / ANCHOR_PAGE_COUNT)

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

_Static_assert((FTL_CHECKPOINT_BLOCK_PAGES * FTL_CHECKPOINT_BLOCK_WORDS_PER_PAGE) == FTL_BLOCK_TOTAL,
               "块元数据快照页数必须恰好覆盖全部块");
_Static_assert((FTL_CHECKPOINT_WEAR_PAGES * FTL_CHECKPOINT_WEAR_ENTRIES_PER_PAGE) == FTL_BLOCK_TOTAL,
               "磨损快照页数必须恰好覆盖全部块");
_Static_assert(FTL_CHECKPOINT_WEAR_PAGES <= UINT8_MAX, "磨损快照页数必须装进 uint8_t");
_Static_assert(FTL_CHECKPOINT_RECORD_PAGES <= PAGES_PER_BLOCK,
               "一条 Checkpoint record 必须装得进一个块");
_Static_assert(FTL_CHECKPOINT_RECORDS_PER_BLOCK >= 2U,
               "每块至少容纳 2 条 record（块满轮转需要）");

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

/* 固定元数据区末尾 = 第一个用户块；也用作动态元数据分配起点。
 * 磨损计数在 Checkpoint 快照内，不占独立块。 */
#define FTL_METADATA_BLOCK_END (JOURNAL_BLOCK0 + JOURNAL_BLOCK_COUNT)

/* 固定池使用的 plane 集合：Anchor/Checkpoint/BBT 用 multi-plane program 把同一页
 * 写全部 plane；Journal 用页号奇偶交错（i & 1）落在前两个 plane。
 * 由 PLANE_NUM 派生，不写死 0x03。 */
#define METADATA_MIRROR_PLANE_MASK ((uint8_t)((1U << PLANE_NUM) - 1U))

/* ── 介质布局版本（NAND 上持久化格式的版本号）──
 * 任何改变**介质格式或块布局**的改动（结构体字段 / 块号 / 记录格式 / 字段位序语义）
 * 都必须把 BASE +1。开机 checkpoint_find_latest / anchor_page_valid 都校验它；不符
 * （或介质上根本没有当前版本的 Checkpoint）⇒ ftl_media_format() 自动全片格式化
 * （见 power_failure_recovery.c），不需要上板前手动擦芯片。
 * 版本演进表见 docs/design/ftl_revision_history.md「介质布局版本」。 */
#define FTL_MEDIA_LAYOUT_VERSION_BASE (9U)

/*
 * 数据通路用**全局 LBA**（页 META / journal 存 host 全局 LBA）。
 * - DIM=1：global == local，介质上的值逐位不变 ⇒ 版本不额外 +1，**不额外格式化**。
 * - DIM>1：同一个 LBA 的编码与单实例不同 ⇒ +1，首次启用自动全片格式化。
 * 这是本仓自定的格式策略。
 */
#define FTL_MEDIA_LAYOUT_VERSION (FTL_MEDIA_LAYOUT_VERSION_BASE + ((FTL_DIM_TOTAL > 1U) ? 1U : 0U))

/* ── 逻辑表字节（公式①：覆盖实体数 × 每实体字节）── */

/* Anchor：每份 Directory = ANCHOR_PAGE_COUNT 页 */
#define FTL_ANCHOR_TABLE_BYTES ((uint32_t)ANCHOR_PAGE_COUNT * PAGE_MAIN_SIZE)

/* BBT：每块 1 bit */
#define FTL_BBT_TABLE_BYTES (BBT_BITMAP_SIZE)

/* Checkpoint：Header 1 页 */
#define FTL_CHECKPOINT_TABLE_BYTES (FTL_CHECKPOINT_RECORD_PAGES * PAGE_MAIN_SIZE)

/* Journal：块数 × 每块页数 × 页主区 */
#define FTL_JOURNAL_TABLE_BYTES ((uint32_t)JOURNAL_BLOCK_COUNT * PAGES_PER_BLOCK * PAGE_MAIN_SIZE)

/* ── 分区表（固定元数据区的可枚举视图）──
 * 只在 metadata.c 内部使用：`ftl_is_metadata_block` 与文档化布局共用同一份数据。 */

typedef struct
{
    const char *name;        /* 分区名 */
    uint16_t    first_block; /* 起始块（per-plane 号；各 die 相同）*/
    uint16_t    block_count; /* 块数 */
    uint8_t     plane_mask;  /* 本分区**实际使用**的 plane：bit0=P0, bit1=P1。
                              * 固定池只预留 mask 里的 plane；其余 plane 的同号块
                              * 是用户数据（不被 ftl_is_metadata_block 排除）。*/
    uint8_t redundancy;      /* 每块容纳的 record 数（池内块轮转）；
                              * Journal 不是 record 池，填 1 */
    uint32_t table_bytes;    /* 逻辑表大小 */
} ftl_partition_t;

_Static_assert(PLANE_NUM <= 8U, "plane_mask 是 uint8_t，PLANE_NUM 不能超过 8");

/* ── 编译期自检（池几何与分区不重叠）── */

/* 固定元数据区不越界 */
_Static_assert(FTL_METADATA_BLOCK_END <= BLOCKS_PER_PLANE, "固定元数据区超出每 plane 块数");

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
_Static_assert((JOURNAL_BLOCK0 + JOURNAL_BLOCK_COUNT) == FTL_METADATA_BLOCK_END,
               "固定区末尾必须紧接 Journal 之后");

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
 * metadata_dir_update 的"满则丢弃"分支会把已落到动态块上的 region 位置丢掉
 * （重启后回落到"未映射" = 更新永久丢失）。容量取满即按构造消除该分支。*/
#define METADATA_DIR_ENTRIES          (L2P_REGIONS_PER_INSTANCE)
#define METADATA_DIR_ENTRIES_PER_PAGE (ANCHOR_ENTRIES_PER_PAGE)

/* ── 序列号锚定 ── */
#define ANCHOR_MAGIC (0x524F444EUL) /* "ANDIR" */

/* ── Entry: 7 字节（packed） ── */
#define METADATA_ENTRY_BYTES (7U)

typedef struct
{
    uint8_t    type; /* METADATA_TYPE_* */
    uint16_t   id;   /* L2P region_id / 0 for others */
    nand_pba_t pba;  /* 物理位置（PBA，自带 CH/CE ⇒ 不依赖"本实例元数据必在本实例 ch/ce"假设）*/
} __attribute__((packed)) metadata_entry_t;

_Static_assert(sizeof(metadata_entry_t) == METADATA_ENTRY_BYTES, "metadata_entry_t 必须是 7 字节");

/* ── NAND 上持久化格式：**每页自包含** ──
 * Directory 有 METADATA_DIR_ENTRIES 条（14KB）⇒ 一份跨 ANCHOR_PAGE_COUNT 页，
 * 每页装 METADATA_DIR_ENTRIES_PER_PAGE 条，各自带 magic/sequence。
 * 单页损坏只丢该页（其余页仍可用），且读写只需一个 2KB 缓冲。
 * ⚠ 无本层 CRC——页完整性由下层 FC 的 per-CW CRC + ECC 保证。*/
#define METADATA_ANCHOR_HEADER_BYTES (20U)

typedef struct
{
    uint32_t         magic;                                  /* ANCHOR_MAGIC */
    uint32_t         sequence;                               /* 单调递增（同一份的各 chunk 相同）*/
    uint16_t         chunk_index;                            /* 0..ANCHOR_PAGE_COUNT-1 */
    uint16_t         chunk_count;                            /* = ANCHOR_PAGE_COUNT */
    uint32_t         entry_count;                            /* 本 chunk 有效 entry 数 */
    uint32_t         layout_version;                         /* FTL_MEDIA_LAYOUT_VERSION（版本不符 ⇒ 该页作废）*/
    metadata_entry_t entries[METADATA_DIR_ENTRIES_PER_PAGE]; /* 256 × 7 = 1792B */
    uint8_t          padding[PAGE_MAIN_SIZE - METADATA_ANCHOR_HEADER_BYTES -
                    (METADATA_DIR_ENTRIES_PER_PAGE * METADATA_ENTRY_BYTES)]; /* 填满一页主区 */
} metadata_anchor_page_t;

/* 页头前缀：廉价探针只读这 20 字节（整页 CRC/ECC 仍由 FC 校验）。 */
typedef struct
{
    uint32_t magic;
    uint32_t sequence;
    uint16_t chunk_index;
    uint16_t chunk_count;
    uint32_t entry_count;
    uint32_t layout_version;
} metadata_anchor_header_t;

/* 一页必须装得下：头 20B + 每页条目（无独立 CRC，页完整性由 FC 的 per-CW CRC + ECC 保证）*/
_Static_assert(sizeof(metadata_anchor_header_t) == METADATA_ANCHOR_HEADER_BYTES,
               "Anchor 页头前缀必须与结构体头部一致");
_Static_assert(offsetof(metadata_anchor_page_t, entries) == METADATA_ANCHOR_HEADER_BYTES,
               "Anchor 页头前缀与 entries 偏移不一致");
_Static_assert(sizeof(metadata_anchor_page_t) == PAGE_MAIN_SIZE, "Anchor 页结构体不等于一页主区");
_Static_assert((METADATA_DIR_ENTRIES % METADATA_DIR_ENTRIES_PER_PAGE) == 0U, "每页条目数不能整除总条目数");

/* ══════════════════════════════════════════════════════════════════
 * 元数据 record 池（环形轮转）—— Anchor / Checkpoint / BBT 共用
 *
 * 池 = 连续 block_count 个**同 plane** 的物理块（per-plane 块号 first_block..）。
 * 每块容纳 PAGES_PER_BLOCK / pages_per_record 条 record；一条 record 占
 * pages_per_record 个**连续页**（页序 = 记录内序）。
 *
 * 全池共 total = block_count × records_per_block 条槽位，**按 ring 序**（块 0 的
 * 槽 0..R-1 → 块 1 的槽 0..R-1 → … → 块 N-1 的槽 R-1 → 回块 0）顺序写。
 * 写序 == sequence 序。
 *
 * ⚠ block_count >= 2 是硬要求：写点换块时要先擦目标块，若池只有 1 块，
 *   擦的正是唯一持有效 record 的块 ⇒ 全丢。
 * ══════════════════════════════════════════════════════════════════ */
typedef struct
{
    uint16_t first_block;      /* 池首块（per-plane 块号）*/
    uint16_t block_count;      /* 池块数（>= 2）*/
    uint8_t  pages_per_record; /* 一条 record 占几页（向下取整到每块 record 数；
                                * 不能整除时块尾剩余页不用——metadata_ring 的地址公式已按 floor 处理）*/
    uint8_t plane_mask;        /* 池实际使用的 plane：bit0=P0, bit1=P1。
                                * 0x03 = 镜像（同一 record 写两个 plane）；0x01 = 单副本。*/
} metadata_ring_t;

/* plane_mask 里第 idx 个置位的 plane 号（idx 从 0 起）；越界返回 PLANE_NUM。 */
static inline uint8_t metadata_ring_plane_at(uint8_t plane_mask, uint8_t idx)
{
    for (uint8_t p = 0U; p < PLANE_NUM; p++)
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
    return (uint8_t)PLANE_NUM;
}

/* plane_mask 里的 plane 个数。 */
static inline uint8_t metadata_ring_plane_count(uint8_t plane_mask)
{
    uint8_t n = 0U;

    for (uint8_t p = 0U; p < PLANE_NUM; p++)
    {
        if ((plane_mask & (1U << p)) != 0U)
        {
            n++;
        }
    }
    return n;
}

/* 每块容纳的 record 数 */
static inline uint16_t metadata_ring_records_per_block(const metadata_ring_t *ring)
{
    return (uint16_t)(PAGES_PER_BLOCK / ring->pages_per_record);
}

/* 池内 record 槽位总数 */
static inline uint16_t metadata_ring_total_records(const metadata_ring_t *ring)
{
    return (uint16_t)(ring->block_count * metadata_ring_records_per_block(ring));
}

/* 各池槽位数必须装得进 uint16_t（写游标 / record 下标都是 16 位）*/
_Static_assert((ANCHOR_POOL_BLOCKS * ANCHOR_RECORDS_PER_BLOCK) <= UINT16_MAX, "Anchor 池槽位数超出 uint16_t");
_Static_assert((CHECKPOINT_POOL_BLOCKS * PAGES_PER_BLOCK) <= UINT16_MAX, "Checkpoint 池槽位数超出 uint16_t");
_Static_assert((BBT_POOL_BLOCKS * PAGES_PER_BLOCK) <= UINT16_MAX, "BBT 池槽位数超出 uint16_t");

/*
 * record 槽位 → 芯片地址
 *
 * record_idx ∈ [0, total)；page_in_record ∈ [0, pages_per_record)。
 */
nand_pba_t metadata_ring_addr(const metadata_ring_t *ring,
                              const ftl_topology_t  *topo,
                              uint16_t               record_idx,
                              uint8_t                page_in_record);

/* 指定 plane 的同一个 record 槽地址（镜像池的读回退 / 写镜像用）。
 * plane 必须落在 ring->plane_mask 内；越界是调用者 bug ⇒ ASSERT。 */
nand_pba_t metadata_ring_addr_plane(const metadata_ring_t *ring,
                                    const ftl_topology_t  *topo,
                                    uint16_t               record_idx,
                                    uint8_t                page_in_record,
                                    uint8_t                plane);

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
typedef ftl_status_t (*metadata_ring_probe_fn)(void     *ctx,
                                               uint16_t  record_idx,
                                               bool      full,
                                               uint32_t *out_sequence);

/*
 * metadata_ring_locate - 定位池内最新有效 record 与下一个可写槽
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
 * @param out_newest_idx 输出：最新有效 record 槽位；**池内无有效 record 时为 UINT16_MAX**
 * @param out_sequence   输出：该 record 的 sequence；**池内无有效 record 时为 0**
 * @param out_write_idx  输出：下一个可写槽位
 * @return FTL_OK = 定位完成（含"池为空"）；FTL_ERROR = ring 几何非法
 */
ftl_status_t metadata_ring_locate(const metadata_ring_t *ring,
                                  metadata_ring_probe_fn probe,
                                  void                  *ctx,
                                  uint16_t              *out_newest_idx,
                                  uint32_t              *out_sequence,
                                  uint16_t              *out_write_idx);

/*
 * metadata_ring_prepare_addr - 算写点槽位地址 + 判定是否需先擦该块（**不动介质**）
 *
 * 写点是块内槽 0 时**需要先擦该块**（池内轮转：该块持的是全池最旧的 record，且每条
 * record 都是完整快照 ⇒ 擦掉不丢信息）。本函数只做**判定**，擦除留给调用者：
 * 同步站点用 metadata_ring_prepare_write（内部即刻擦），异步 job 按 *out_need_erase
 * 自行提交异步擦命令——把"擦"放进状态机，而不是在服务循环里阻塞。
 *
 * @param ring          池几何
 * @param topo          ch/ce/lun
 * @param write_idx     写点槽位（来自 metadata_ring_locate 或上次写点 + 1）
 * @param out_addr      输出：record 第 0 页地址
 * @param out_need_erase 输出：true = 写之前必须先擦 *out_addr 所在块
 * @return FTL_OK / FTL_ERROR（几何非法或槽位越界；ring/topo/出参由调用者提供）
 */
ftl_status_t metadata_ring_prepare_addr(const metadata_ring_t *ring,
                                        const ftl_topology_t  *topo,
                                        uint16_t               write_idx,
                                        nand_pba_t            *out_addr,
                                        bool                  *out_need_erase);

/*
 * metadata_ring_prepare_write - 把写点槽位准备好（含换块擦除），返回该 record 第 0 页地址
 *
 * = metadata_ring_prepare_addr + 需要时同步擦除该块。**同步**原语：只许结构性同步
 * 上下文调用（开机 / 格式化 / 同步回退）；异步 job 请用 metadata_ring_prepare_addr。
 *
 * @param ring      池几何
 * @param topo      ch/ce/lun
 * @param write_idx 写点槽位（来自 metadata_ring_locate 或上次写点 + 1）
 * @param out_addr  输出：record 第 0 页地址
 * @return FTL_OK / FTL_ERROR（擦除失败）
 */
ftl_status_t metadata_ring_prepare_write(const metadata_ring_t *ring,
                                         const ftl_topology_t  *topo,
                                         uint16_t               write_idx,
                                         nand_pba_t            *out_addr);

/* ── 元数据目录后台 job ──
 *
 * 承载 L2P 日志块的两件"要碰 NAND 的整理活"，让它们不在服务循环里阻塞：
 *   ① **回收**——某个日志块的活 region 数归零 ⇒ 擦块 + 归还 Free Pool；
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
 *   metadata_dir_anchor_prepare（游标未建立时会**同步**扫池定位），在完成泵里跑就是
 *   嵌套泵。故 ftl_metadata_callback 只记结果 + release + 改 phase，推进一律留给下一轮
 *   metadata_handle（在 ftl_service 各档位里跑）。
 *
 * ⚠ 本结构不跨重启：phase / 游标 / 请求位全是 RAM 态，冷启动恒 IDLE（见 metadata_dir_init）。 */
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
} metadata_handle_phase_t;

/*
 * md_compact_step_t - 压缩 job「搬一条 region」一步的结果
 *
 * md_compact_read / md_compact_write 共用本契约（write 永不返回 MD_COMPACT_SCANNED：
 * 写一条就回去读下一条）。调用者按**具名值**分派，不再靠 `r < 0` / `r != 2` 这类魔法数字。
 */
typedef enum
{
    MD_COMPACT_FAILED       = -1, /* 硬失败（md_stall 无进展）：调用者 metadata_handle_abort */
    MD_COMPACT_BACKPRESSURE = 0,  /* 背压未提交：保留游标/槽，**下轮重试**（调用者 return）*/
    MD_COMPACT_SUBMITTED    = 1,  /* 已提交异步命令：等完成服务函数 */
    MD_COMPACT_SCANNED      = 2,  /* 全表扫完、无更多 region 待搬（read 专有）*/
} md_compact_step_t;

/*
 * md_anchor_step_t - metadata_dir_anchor_prepare 的结果
 *
 * ⚠ 与 md_compact_step_t **不能合并**：本处 0（无需写）时调用者**推进 phase**（跳过
 *   Anchor 直接擦 victim），而 md_compact_* 的 0（背压）时调用者 **return 等下一轮**
 *   —— 同值反义，共用一套名字会把两处调用点读反。
 */
typedef enum
{
    MD_ANCHOR_FAILED = -1, /* 定位/取槽失败：调用者中止 job */
    MD_ANCHOR_CLEAN  = 0,  /* dir 不脏、无需写 Anchor：调用者整段跳过 */
    MD_ANCHOR_READY  = 1,  /* 出参全部有效，可以写 Anchor */
} md_anchor_step_t;

/* 布尔量直接用 bool；**不压位域**——位域布局是 implementation-defined，
 * `_Static_assert` 证不了（C4），且这里没有 RAM 压力。 */
typedef struct
{
    uint8_t         phase;               /* metadata_handle_phase_t */
    bool            waiting;             /* 有命令在飞（完成服务函数清）*/
    bool            compact_wanted;      /* append_slot 换块后池吃紧 ⇒ 请求压缩 */
    bool            anchor_ok;           /* Anchor 分页写是否全部成功（false = 整条作废）*/
    bool            anchor_need_erase;   /* Anchor 写点是否需先擦块 */
    bool            anchor_for_reclaim;  /* 本次 Anchor 仅为「回收前落盘」：完成后回 RECLAIM */
    uint8_t         anchor_chunk;        /* 已写的 Anchor 页序 */
    uint16_t        victim;              /* 压缩 victim 的 gblock（UINT16_MAX = 无）*/
    uint16_t        region_id;           /* 搬移游标（下一个待查 region_id）*/
    uint16_t        cur_region;          /* 当前搬移中的 region_id（CP_READ 命中时设定）*/
    uint16_t        moved;               /* 本次压缩已搬 region 数（观测）*/
    uint16_t        reclaim_gblock;      /* 在飞擦除的块（完成时归还它）*/
    uint16_t        reclaim_fail_gblock; /* 连续擦除失败计数对应的块（RISK-MD-01）*/
    uint8_t         reclaim_fail_count;  /* 该块连续擦除失败次数；达上限即标坏并清位 */
    uint32_t        stall_ms;            /* 连续「提交不出去」的起点（0 = 未计时） */
    uint8_t         timed_out;           /* 1 = 本次 job 因 deadline 中止 */
    uint8_t         timeout_phase;       /* 超时发生的 phase */
    uint16_t        anchor_idx;          /* 本 Anchor record 的池内槽位 */
    uint32_t        anchor_seq;          /* 本 Anchor record 的序号 */
    nand_pba_t      anchor_base;         /* 本 Anchor record 第 0 页地址 */
    nand_pba_t      slot;                /* 压缩搬移已取到、待写的槽（NAND_PBA_INVALID = 未取）*/
    metadata_ring_t anchor_ring;         /* Anchor 池几何（prepare 出参，commit 要用）*/
    uint32_t        reclaimed_count;     /* 累计擦除成功并归还的日志块数（单调进度） */
    int32_t         error;               /* 最近一次中止的 FTL_*（回 IDLE 时清） */

    /* 心跳基准（见 ftl_job.h）：job 在飞期间每满 1s 打一行 phase/victim/age */
    uint32_t start_ms;
    uint32_t log_ms;

    /* victim region 页搬运缓冲：**必须常驻**——异步读完成与随后的写之间隔着若干轮
     * 服务循环，栈帧早已返回。放 job 而非栈（同 migrate_handle.page_buf / rr_page_scratch）。 */
    uint8_t cp_buf[L2P_REGION_SIZE];
} metadata_handle_t;

/* ── 模块端口（docs/design/ftl_module_contracts.md §4.3）──
 *
 * metadata job 需要：allocator（取新日志块 / 归还 victim / 标坏 / 池状态）与
 * total_writes（策略计数）。两者都由装配层在 init 时绑定；模块不持有 ftl_context。
 */
typedef struct
{
    ftl_allocator_t *alloc;
    const uint32_t  *total_writes;
} metadata_port_t;

/* ── 常驻 Directory Cache（约 24.6 KiB，`g_ftl_md[]` 落 SRAM4，见 src/main.c） ── */
typedef struct
{
    metadata_entry_t entries[METADATA_DIR_ENTRIES];
    uint16_t         count;           /* 有效 entry 数 */
    uint32_t         anchor_sequence; /* 当前 Anchor sequence */
    bool             dirty;           /* 是否有未写回 Anchor 的变更 */
    uint32_t         dirty_gen;       /* Directory 变更代数：每次 update 自增 */
    uint32_t         anchor_gen;      /* 本次 Anchor 写入所覆盖的 dirty_gen 快照 */

    /* ── L2P 日志块（元数据写模型：只追加 + 块池轮转）──
     * 每个 region 写进「当前日志块」的下一空槽（一 region = 一页）。块满则从
     * Free Pool 取新块（Free Pool 保证已擦除）。l2p_live 记每个日志块上还有多少
     * region 的**最新位置**落在这里 —— 归零即置 reclaim_pending，由后台 job 异步擦除。
     * l2p_live 同时是 `ftl_is_metadata_block` 的唯一动态块判据（C9：不维护第二套坐标）。 */
    uint16_t l2p_live[FTL_BLOCK_TOTAL]; /* 每 gblock 的活 region 数 */
    uint16_t cur_metadata_block;        /* 当前日志块 gblock（cur_block_valid=false 时无意义）*/
    uint8_t  cur_metadata_page;         /* 当前日志块内下一可用页 */
    bool     cur_block_valid;           /* 游标是否已建立（false = 尚未分配日志块）*/
    uint8_t  cur_metadata_plane;        /* 下一个新日志块选哪个 plane（轮转）*/

    /* ── 待回收日志块位图（RAM 态，不跨重启）──
     * commit_region 在活计数归零时置位；append_slot 换走旧当前块时若其已空也置位。
     * 运行期擦除全部由 metadata_handle 的 MD_JOB_RECLAIM 异步做——append_slot 不做
     * 同步擦除（3 ms tERASE 会击穿服务预算）。
     * 位在"擦除在飞"期间保持置位：完成成功才清；失败保留，下轮重试。 */
    uint32_t reclaim_pending[FTL_BLOCK_TOTAL / 32U];
    uint16_t reclaim_pending_count;
    uint16_t reclaim_scan; /* 轮转起点：擦除持续失败的块不再永久挡住后续块 */

    /* 后台整理 job（回收 + 压缩，见 metadata_handle_t）。两个作用：
     *   ① **抑制逐次立即回收**（搬移 victim 的最后一个活 region 时 l2p_live[victim]
     *      归零，若就地擦掉就是**在 Anchor 落盘之前**擦 victim ⇒ 掉电丢映射）；
     *   ② **防重入 / 防抢槽**（压缩搬移要往当前块写，期间再发放槽位会打乱空槽预算）。
     * 判据分别是 md_compacting()（①）与 job.phase != IDLE（②）。 */
    metadata_handle_t handle;

    /* Anchor 池游标（ring 写点槽位；anchor_cursor_valid=false 时尚未定位）*/
    uint16_t anchor_write_idx;
    bool     anchor_cursor_valid;

    /* 拓扑信息（元数据所在芯片路由） */
    ftl_topology_t topo;

    /* allocator / total_writes 端口（装配层绑定；docs §4.3） */
    metadata_port_t port;
} metadata_dir_cache_t;

/* 待回收位图按 32 位字切分，块总数必须能整除一个字。 */
_Static_assert((FTL_BLOCK_TOTAL % 32U) == 0U, "FTL_BLOCK_TOTAL 必须是 32 的倍数");

/* ── API ── */

/*
 * metadata_dir_init - 初始化 Metadata Directory
 *
 * 首次使用时为全部 L2P Region 登记 `NAND_PBA_INVALID`（= 未映射；读取该 region
 * 不需要访问 NAND）。写过并刷盘的 region 由 metadata_dir_commit_region 覆盖成
 * 日志块上的真实位置。
 *
 * 游标全部置**无效**（cur_block_valid = 0 / anchor_cursor_valid = 0）：首次追加
 * 才从 Free Pool 取块，避免"没拿块就用"。
 *
 * @param dir  Directory cache（由调用者提供存储）
 */
void metadata_dir_init(metadata_dir_cache_t *dir);

/*
 * metadata_dir_lookup - 查找元数据物理位置
 *
 * @param dir  Directory cache
 * @param type 元数据类型
 * @param id   L2P region_id / 0 for others
 * @return PBA；NAND_PBA_INVALID = 未找到。对 L2P region 而言"未找到"与"未映射"
 *         同义：`fill_fixed` 为每个 region 预登记一条，之后只在刷盘时覆盖。
 */
nand_pba_t metadata_dir_lookup(const metadata_dir_cache_t *dir,
                               uint8_t                     type,
                               uint16_t                    id);

/*
 * metadata_live_count - 某个 gblock 上还有多少 region 的**最新位置**落在它上面。
 *
 * 供开机恢复（PFR）判断「该块是否仍被 Directory 引用」。越界返回 0（不是 ASSERT：
 * 调用者的 g 来自介质/快照，属不可信输入，H29）。
 */
uint16_t metadata_live_count(const metadata_dir_cache_t *dir, uint16_t gblock);

/*
 * metadata_dir_update - 添加或更新元数据位置
 *
 * @param dir    Directory cache
 * @param type   元数据类型
 * @param id     L2P region_id / 0 for others
 * @param new_pba 新物理位置（PBA）
 */
void metadata_dir_update(metadata_dir_cache_t *dir,
                         uint8_t               type,
                         uint16_t              id,
                         nand_pba_t            new_pba);

/*
 * metadata_dir_append_slot - 取当前日志块的下一个空槽（L2P region 的写点）
 *
 * **只取槽，不登记**：调用方先往返回的地址写 region 页，写成功后再调
 * metadata_dir_commit_region 登记（写失败则位置不变，槽被跳过——已编程页不可复用）。
 *
 * 块满（或尚无日志块）⇒ 直接从 Free Pool 取**整块**新日志块
 * （allocator_alloc_log_block；Free Pool 保证已擦除，故无需擦）。无活 region 的旧块
 * 由后台整理 job 异步擦除归还——本函数只登记，不同步擦除。**绝不回退到固定位置**：
 * 分配失败返回 NAND_PBA_INVALID，由上层重试（待后台回收完成后自然成功）。
 *
 * 日志块在哪个 plane 由 dir->cur_metadata_plane 轮转决定（写入目标与 region 归属
 * 无关：位置由 pba 自身携带，读回一律经 Directory 查 pba）。
 *
 * ⚠ **本函数不做压缩**：换块后若 Free Pool 吃紧，只登记 `job.compact_wanted` 并返回
 *   NAND_PBA_INVALID——压缩由 metadata_handle 起异步 job 做，避免在写回路径上同步
 *   阻塞数十毫秒。
 * ⚠ **整理 job 在飞时一律返回 NAND_PBA_INVALID**（见函数内注释）：压缩的空槽预算按
 *   发放那一刻的 cur_metadata_page 核定，期间再发放会打乱预算与 ③④ 顺序。调用者
 *   （l2p_cache_flush_kick）据此返回 FTL_AGAIN，下轮重试。
 *
 * @param dir    Directory cache
 * @param alloc  Allocator（取 Free Pool 块）
 * @return 写点 PBA；失败返回 NAND_PBA_INVALID
 */
nand_pba_t metadata_dir_append_slot(metadata_dir_cache_t *dir,
                                    ftl_allocator_t      *alloc);

/*
 * metadata_dir_commit_region - 写成功后登记 region 的新位置
 *
 * 更新 entries[region_id].pba + l2p_live（旧日志块 -1、新日志块 +1）+ dirty。
 * 旧块活计数归零且非当前块 ⇒ 把块号置进 `reclaim_pending` 位图，
 * 由后台 job 异步擦除 + 归还 Free Pool。
 *
 * ⚠ **本函数不擦 NAND**：它被 `ftl_l2p_callback`（L2P region 写回完成）调用，**运行在
 *   完成派发里** —— 就地擦会在完成泵内嵌套泵。登记后 `ftl_l2p_callback` 全路径无 NAND 访问。
 * ⚠ 压缩 job 在飞时**不登记**：压缩自己会把 victim 的活计数搬到 0，若此时就地
 *   登记，job 的 CP_RECLAIM 会在 Anchor 落盘**之前**擦 victim（③ 早于 ④ 契约）。
 *   压缩期间归零的块由 job 自己在 CP_RECLAIM 里擦。
 *
 * @param dir       Directory cache
 * @param region_id L2P region_id（内部调用者保证 < METADATA_DIR_ENTRIES）
 * @param new_pba   刚写成功的位置（PBA，内部调用者保证非 NAND_PBA_INVALID）
 */
void metadata_dir_commit_region(metadata_dir_cache_t *dir,
                                uint16_t              region_id,
                                nand_pba_t            new_pba);

/*
 * metadata_handle - 推进元数据目录后台 job（回收 / 压缩）一步
 *
 * 每轮服务循环（ftl_service_idle / normal / busy）调一次；**没有 job 在跑时**它就是
 * 起 job 的地方：先看 `reclaim_pending_count`（有 ⇒ 从位图取一个块起回收），再看
 * job.compact_wanted（有 ⇒ 起压缩）。起不动（提交背压 / 无候选 / 空槽不够）只记账，
 * 下轮再来。
 *
 * ⚠ **所有状态推进都在这里**（不在完成服务函数里）：压缩的 CP_ANCHOR_PREP 阶段要调
 *   metadata_dir_anchor_prepare，而它在游标未建立时会**同步**扫池定位 —— 在完成泵里
 *   跑就是嵌套泵（同 ftl_checkpoint_callback 的取舍）。服务函数只记结果 + release + 改
 *   phase，本函数负责"下一轮接着走"。
 * ⚠ 幂等且廉价：无 job 且无请求时立即返回（一次 load + 比较）。
 *
 * @param dir   Directory cache
 * @param alloc Allocator（取新日志块 / 归还 victim）
 */
typedef struct
{
    ftl_module_status_t base;

    uint8_t  phase;           /* metadata_handle_phase_t（观测；外部不要据此驱动） */
    uint16_t victim;          /* 压缩 victim（UINT16_MAX = 无） */
    uint16_t moved;           /* 本次压缩已搬 region 数 */
    uint32_t reclaimed;       /* 累计擦除成功的日志块数（单调进度） */
    bool     compact_wanted;  /* 池吃紧，请求压缩 */
    bool     anchor_ok;       /* Anchor 分页写是否全部成功 */
    bool     reclaim_pending; /* 有已登记待回收的日志块 */
    bool     timed_out;       /* 本次 job 因 deadline 中止 */
    uint8_t  timeout_phase;   /* 超时发生的 phase */
} metadata_status_t;

/*
 * metadata_dir_bind_port - 绑定 allocator / total_writes 端口。
 * 由装配层（ftl.c）在 metadata 初始化之后、后台调度之前调用一次。
 */
void metadata_dir_bind_port(metadata_dir_cache_t *dir, const metadata_port_t *port);

/*
 * metadata_handle - 推进元数据目录整理 job（typed handle；提交即返回）
 *
 * 事件驱动（reclaim_pending / compact_wanted 由 Directory 函数置位），`info` 当前
 * 不参与决策；保留形参以统一 typed 入口。
 *
 * @param dir   Directory cache；**前置条件**（实例绑定由 ftl_contract_audit 证明）
 * @param info  本轮动态信息（当前未用，可传 NULL）
 * @param out   出参：本轮结束后的上报（**非空**）
 */
void metadata_handle(metadata_dir_cache_t *dir, const ftl_bg_info_t *info, metadata_status_t *out);

/*
 * metadata_dir_busy - 整理 job 是否在跑（有命令在飞或处在中间 phase）。
 *
 * 用途：让**调用者**区分"我的重试为什么没进展"与"卡死"。当前唯一读者是
 * checkpoint.c 的 cp_flush_regions：脏 region 取不到槽（FTL_AGAIN）时若 job 在推进，
 * 就**不该**累计 CP 的 stall 计数——否则一次正常的长压缩会把 Checkpoint 打回去。
 *
 * @return true = 有 job 在跑；false = 空闲（或 dir == NULL）。
 */
static inline bool metadata_dir_busy(const metadata_dir_cache_t *dir)
{
    return (dir != NULL) && (dir->handle.phase != (uint8_t)MD_JOB_IDLE);
}

/* ── Anchor 落盘的三段式（唯一形态）──
 *
 * 一条 Anchor record = ANCHOR_PAGE_COUNT 页。**所有**落盘站点（Checkpoint job、
 * 目录压缩 job）都拆成 prepare →（可选换块擦）→ 逐页写 → commit，每步之间可以回到
 * 服务循环。三段式把「Directory 格式」留在 metadata.c，「状态机」留给调用者。
 */

/*
 * metadata_dir_anchor_prepare - 定位游标 + 取槽 + 算是否需换块擦（**不动介质**）
 *
 * @param dir            Directory
 * @param out_ring       出参：Anchor 池几何
 * @param out_base       出参：本 record 第 0 页地址
 * @param out_idx        出参：本 record 的池内槽位号（commit 要用）
 * @param out_seq        出参：本 record 的 sequence（本函数内自增 anchor_sequence）
 * @param out_need_erase 出参：true = 写之前必须先擦 *out_base 所在块
 * @return md_anchor_step_t：MD_ANCHOR_READY 需要写（出参全部有效）；
 *         MD_ANCHOR_CLEAN 无需写（dir 不脏）；MD_ANCHOR_FAILED 失败（定位失败）
 */
md_anchor_step_t metadata_dir_anchor_prepare(metadata_dir_cache_t *dir,
                                             metadata_ring_t      *out_ring,
                                             nand_pba_t           *out_base,
                                             uint16_t             *out_idx,
                                             uint32_t             *out_seq,
                                             bool                 *out_need_erase);

/*
 * metadata_dir_anchor_page - 组第 chunk 页 Anchor 数据（纯 RAM）
 *
 * @param dir    Directory
 * @param seq    本 record 的 sequence（prepare 的回传值）
 * @param chunk  页序（0..ANCHOR_PAGE_COUNT-1）
 * @param page   出参：组好的页（调用者提供 2KB 缓冲）
 */
void metadata_dir_anchor_page(const metadata_dir_cache_t *dir, uint32_t seq, uint8_t chunk, metadata_anchor_page_t *page);

/*
 * metadata_dir_anchor_commit - 收尾：推进游标（无论成败），成功才清 dirty
 *
 * @param dir   Directory
 * @param ring  prepare 回传的池几何
 * @param idx   prepare 回传的槽位号
 * @param ok    false = 有条分页写失败（整条作废，保留 dirty）
 */
void metadata_dir_anchor_commit(metadata_dir_cache_t *dir, const metadata_ring_t *ring, uint16_t idx, bool ok);

/*
 * metadata_dir_load - 从 Anchor 池加载 Directory
 *
 * ① 先 fill_fixed 铺基础层（全部 region = `NAND_PBA_INVALID`，即未映射）；
 * ② metadata_ring_locate 定位池内最新**整条校验通过**的 record；
 * ③ 读胜者各页装配覆盖对应区间（逐页再校验 chunk_index/sequence）；
 * ④ 由装配后的 entries 重建 l2p_live。
 *
 * **日志块游标不跨重启恢复**：cur_block_valid 置 false，首次追加时从 Free Pool 新取
 * 一块（绝不复用可能已编程的旧页）。旧日志块仍被 Directory 引用 ⇒ 由 l2p_live 保住，
 * 待其 region 全部改写后由后台整理 job 异步擦除归还。
 * Anchor 游标则**由 locate 结果恢复**（anchor_write_idx / anchor_sequence），避免首次
 * flush 再扫一遍池。
 *
 * @param dir  Directory cache（输出）
 * @return FTL_OK / FTL_ERROR（无有效 Anchor：调用者转 metadata_dir_init）
 */
ftl_status_t metadata_dir_load(metadata_dir_cache_t *dir);

/*
 * ftl_is_metadata_block - 判断物理 Block 是否属于固定/动态元数据区
 *
 * 掉电恢复重建 closed_sb_list 时，L2P region 日志块、Anchor、BBT、Checkpoint、
 * Journal 等元数据块可能写满全部页（尤其 Journal 环形回切），逐页判空会把它们
 * 误判为满 Super Block。本谓词统一排除：
 *
 *   固定块：遍历分区表，按 first_block/block_count + plane_mask 判区间
 *   游标块：当前正在追加的 L2P 日志块（可能尚无任何 region 指向它，Directory 扫不到）
 *   日志块：`l2p_live[g] != 0` —— 该账本的定义就是"最新位置落在 g 的 region 数"，
 *           与 Directory 条目一一对应，判据 O(1)
 *
 * free pool 回填（main.c）与重建（PFR 内部 full metadata rebuild）共用同一判据。
 *
 * ⚠ 固定块分支也**必须比较 plane**：固定池只占 plane_mask 里的 plane，其余 plane 的
 *   同号块是用户数据。日志块/游标块从用户区动态取，同样必须带 plane 比较；只比
 *   per-plane 块号会连带掩掉另一 plane 的同号块（既出不了 free pool、又被 full metadata rebuild
 *   跳过，白丢一个可用块）。
 *
 * @param md_dir Metadata Directory cache（可为 NULL：仅固定块区间判断）
 * @param plane  物理 plane 号（0..PLANE_NUM-1）
 * @param block  物理 Block 号（per-plane 号，0..BLOCKS_PER_PLANE-1）
 * @return true = 元数据块，false = 用户数据块
 */
bool ftl_is_metadata_block(const metadata_dir_cache_t *md_dir, uint8_t plane, uint16_t block);

#endif /* FTL_METADATA_H */
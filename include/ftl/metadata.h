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
 *   L2P Region       — 固定表区（哨兵，永不写入）+ 日志块（通过 Directory 查找）
 *   Journal          — 固定元数据区（块位置见上方「固定元数据区布局」）
 *   Checkpoint/BBT   — 固定元数据区（见上方「固定元数据区布局」）
 *
 * ── 元数据写模型（P0-2 统一；Anchor / Checkpoint / BBT / L2P 日志共用）──
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

#include "ftl/allocator.h"
#include "ftl/ftl_config.h" /* §一 几何量（FTL_BLOCK_TOTAL / FTL_SLOTS_PER_BLOCK / L2P_REGIONS_PER_INSTANCE）*/
#include "ftl/ftl_type.h"
#include "nand/nand_config.h" /* PAGE_MAIN_SIZE（Anchor 页打包自检）*/

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
 *   L2P Table   ⌈每实例 region 数 ÷ 每块槽位数⌉，每块槽位 = PAGES_PER_BLOCK × PLANE_NUM
 *               ⚠ 这是**哨兵层**：只给"从未写过的 region"一个确定位置，该区页
 *                 **永不写入**（保持擦除态 ⇒ 读回全 0xFF ⇒ 未映射）。写过的 region
 *                 一律落动态日志块（见下方 Metadata Directory）。
 *   Anchor      ANCHOR_POOL_BLOCKS（Directory 的块轮转日志池）
 *   Checkpoint  CHECKPOINT_POOL_BLOCKS（Header 的块轮转日志池）
 *   BBT         BBT_POOL_BLOCKS（坏块位图的块轮转日志池）
 *   Journal     JOURNAL_BLOCKS（策略：容纳两次 checkpoint 之间的映射更新）
 *   Wear        WEAR_COPIES（软状态单副本）
 *
 * ── 当前几何下展开的布局（由下方公式算出，非手写）──
 *   块号        分区        块数  来历
 *   0..15       L2P Table   16    ⌈2048 region ÷ 128 region/块⌉（哨兵层）
 *   16..47      Anchor      32    元数据池：每块 8 份 Directory，块满换块
 *   48..55      Checkpoint  8     元数据池：每块 64 份 Header
 *   56..57      BBT         2     元数据池：每块 64 份坏块位图
 *   58..61      Journal     4     策略 = CW_NUM（CW 级 4× 映射流量）
 *   62          Wear        1     软状态单副本
 *   63..       用户数据    —     FTL_METADATA_BLOCK_END 之后
 *
 * ── 元数据写模型（P0-2：全部元数据统一）──
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
 *   独立实例（各自的 L2P 表/Anchor/BBT/Journal/Checkpoint/Wear），主机 LBA 空间
 *   按 region 切分到各实例（lba_route_* / lba_to_local，见 l2p_cache.h）。
 *   分区**不随 LUN/CE/CH 错开**。
 *
 * ⚠ 本区相对旧版（L2P 0..31 / Journal 40..41 / 无 Wear）**变更了介质格式**。
 *   v0.61 起**不再需要手动擦除芯片**：介质上的布局版本（`FTL_MEDIA_LAYOUT_VERSION`，
 *   写在 Checkpoint 头与 Anchor 页内）与当前固件不符（或根本没有当前版本记录）⇒
 *   `ssd_ftl_boot` 自动全片格式化一次并写入当前版本标记（见 power_failure_recovery.c
 *   `ftl_media_format`）。**任何改变介质格式的改动都必须把 `FTL_MEDIA_LAYOUT_VERSION` +1。**
 * ══════════════════════════════════════════════════════════════════ */

/* ── 池尺寸：策略常量 → 每个分区的块数 ── */

/* L2P Table：region 打包进块（向上取整）
 * 每实例一份，块号各 die 相同（不随 LUN/CE/CH 错开）
 * ⚠ `+ L2P_REGION_PER_BLOCK - 1U` 是**向上取整**惯用法（ceil(a/b) ≡ (a+b-1)/b）：
 *   整数除法截断成 floor，除不尽时会少算一块、丢掉尾部 region。
 *   当前几何 2048 ÷ 128 恰好整除 ⇒ 该项不起作用；但两个操作数都由器件几何推出，
 *   改 PAGES_PER_BLOCK / CW_NUM / PAGE_MAIN_SIZE 任一个就可能不再整除 ⇒ 须写通用形式。
 *   下方 `_Static_assert(L2P_BLOCKS * L2P_REGION_PER_BLOCK >= L2P_REGIONS_PER_INSTANCE)`
 *   要求的正是这个 ceil 语义，两者配套。*/
#define L2P_REGION_PER_BLOCK (FTL_SLOTS_PER_BLOCK)
#define L2P_BLOCKS           ((L2P_REGIONS_PER_INSTANCE + L2P_REGION_PER_BLOCK - 1U) / L2P_REGION_PER_BLOCK)

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

/* ── Checkpoint（L2P 快照头）池 ──
 * Header 恰好 2048 B = 一页主区 ⇒ **一条 record = 1 页**。*/
#define CHECKPOINT_POOL_BLOCKS (8U) /* 策略：池内块数 */

/* ── Journal（环形日志）──
 * 容量策略：Journal 必须容纳"两次 checkpoint 之间"的全部映射更新。
 * CW 级映射下，一次页写产生 CW_NUM 条 L2P 更新（IU = CW）
 * ⇒ 容量取 CW_NUM 块（旧版 2 块 → CW 级 4 块）。*/
#define JOURNAL_BLOCKS      (CW_NUM)
#define JOURNAL_BLOCK_COUNT (JOURNAL_BLOCKS)

/* ── Wear（块磨损计数持久化）──
 * 块内**页式追加日志**（见 ftl/wear.h）：全量快照 = ⌈FTL_BLOCK_TOTAL ÷ WEAR_ENTRY_MAX⌉
 * = ⌈4096 ÷ 168⌉ = 25 页 ≤ PAGES_PER_BLOCK，块满才擦一次。
 * 软状态 ⇒ 单副本，不做主备。*/
#define WEAR_COPIES (1U) /* 软状态：单副本 */
#define WEAR_BLOCKS (WEAR_COPIES)

/* ── 偏移：从元数据区起点顺序累积（不写死任何块号）── */

/* 元数据区起点 */
#define FTL_METADATA_BASE_BLOCK (0U)

/* L2P Table：接在元数据区起点，打包放置（见下方 ftl_l2p_region_placement）。
 * ⚠ 哨兵层：本区页**永不写入** ⇒ 只提供"未映射"语义，不承载数据。*/
#define L2P_BASE_BLOCK (FTL_METADATA_BASE_BLOCK)

/* Anchor 池：接在 L2P 之后；池 = [ANCHOR_BLOCK0, ANCHOR_BLOCK0 + ANCHOR_POOL_BLOCKS) */
#define ANCHOR_BLOCK0 (L2P_BASE_BLOCK + L2P_BLOCKS)

/* Checkpoint 池：接在 Anchor 池之后 */
#define CHECKPOINT_BLOCK0 (ANCHOR_BLOCK0 + ANCHOR_POOL_BLOCKS)

/* BBT 池：接在 Checkpoint 池之后 */
#define BBT_BLOCK (CHECKPOINT_BLOCK0 + CHECKPOINT_POOL_BLOCKS)

/* Journal：第 b 块 = JOURNAL_BLOCK0 + b（**环形 + 擦除**，唯一本来就正确的元数据）*/
#define JOURNAL_BLOCK0 (BBT_BLOCK + BBT_POOL_BLOCKS)

/* Wear：接在 Journal 之后 */
#define WEAR_BLOCK (JOURNAL_BLOCK0 + JOURNAL_BLOCK_COUNT)

/* 固定元数据区末尾 = 第一个用户块；也用作动态元数据分配起点 */
#define FTL_METADATA_BLOCK_END (WEAR_BLOCK + WEAR_BLOCKS)

/* ── 介质布局版本（NAND 上持久化格式的版本号）──
 * 任何改变**介质格式或块布局**的改动（结构体字段/块号/记录格式）都必须把本值 +1。
 * 开机 checkpoint_find_latest / anchor_page_valid 都校验它；不符（或介质上根本没有当前
 * 版本的 Checkpoint）⇒ ftl_media_format() 自动全片格式化（见 power_failure_recovery.c）。
 * 这样就不再需要"上板前先手动 bbt --erase"。
 * v0.62：元数据页完整性改由下层 FC 的 per-CW CRC + ECC 承担，FTL 记录内的 CRC 字段
 *        全部删除（Checkpoint/Anchor/BBT/Wear 四结构体尺寸随之变化）⇒ 版本 +1。
 * v0.65：Wear 条目加 rr 健康字段（last_ecc / rising_count / health，8B → 12B）⇒ 版本 +1。
 *        老格式的 Wear 页 magic 仍匹配、条目布局却不同 ⇒ 必须靠版本拦下（否则会把
 *        计数读成垃圾）。
 * v0.73：Directory 条目（metadata_entry_t）的 4 字节位置字段由 PAA 改存 PBA —— 字节数不变
 *        （两者都是 uint32_t），但**字节语义变了**（位序重排）⇒ 必须 +1，否则老介质上读回
 *        的 PAA 会被当成 PBA 解出错误的块/页。
 * v0.74：OP / Spare 池改按 plane 预留（每 plane 各 204 OP + 50 Spare，见 allocator.h）。
 *        结构体布局没变，但**预留块集合变了**：老固件把 plane 1 的块当用户数据区（已写过
 *        用户数据、被 L2P 引用），新固件把它们当预留池 —— 而预留池的契约是"块已擦除"，
 *        消费点会直接把它们丢回 free pool，下次写即覆盖活数据。且 F2 重建会把这些"写过的
 *        块"也建成 Closed 条目 ⇒ 同一块同时进预留池与 closed 列表，账本错乱。⇒ 必须 +1，
 *        让首启自动全片格式化一次。 */
#define FTL_MEDIA_LAYOUT_VERSION (5U)

/* ── 逻辑表字节（公式①：覆盖实体数 × 每实体字节）── */

/* L2P：**每实例** region × 每 region 条目 × 4 B = 4 MiB（每 die 一份；设备总量再 × FTL_DIM_TOTAL）*/
#define FTL_L2P_TABLE_BYTES ((uint32_t)L2P_REGIONS_PER_INSTANCE * L2P_ENTRIES_PER_REGION * sizeof(uint32_t))

/* Anchor：每份 Directory = ANCHOR_PAGE_COUNT 页 */
#define FTL_ANCHOR_TABLE_BYTES ((uint32_t)ANCHOR_PAGE_COUNT * PAGE_MAIN_SIZE)

/* BBT：每块 1 bit */
#define FTL_BBT_TABLE_BYTES (BBT_BITMAP_SIZE)

/* Checkpoint：Header 1 页 */
#define FTL_CHECKPOINT_TABLE_BYTES (PAGE_MAIN_SIZE)

/* Journal：块数 × 每块页数 × 页主区 */
#define FTL_JOURNAL_TABLE_BYTES ((uint32_t)JOURNAL_BLOCK_COUNT * PAGES_PER_BLOCK * PAGE_MAIN_SIZE)

/* Wear：每条目字节（block + read 计数 + 擦除计数 + rr 健康三字段，见 ftl/wear.h）*/
#define FTL_WEAR_ENTRY_BYTES (12U)

/* Wear：每块一条目 */
#define FTL_WEAR_TABLE_BYTES ((uint32_t)FTL_BLOCK_TOTAL * FTL_WEAR_ENTRY_BYTES)

/* ── 分区表（运行期可枚举：谓词判据 + 按名查询）── */

#define FTL_PART_FIXED   (0U) /* 固定位置（块号由上面推出）*/
#define FTL_PART_DYNAMIC (1U) /* 从 Free Pool 动态分配 */

typedef struct
{
    const char *name;        /* 分区名 */
    uint16_t    first_block; /* 起始块（per-plane 号；各 die 相同）*/
    uint16_t    block_count; /* 块数 */
    uint8_t     redundancy;  /* 每块容纳的 record 数（池内块轮转，见上方元数据写模型；
                              * Journal/Wear 不是 record 池，填 1）*/
    uint8_t  kind;           /* FTL_PART_FIXED / FTL_PART_DYNAMIC */
    uint32_t table_bytes;    /* 逻辑表大小 */
} ftl_partition_t;

extern const ftl_partition_t g_ftl_partitions[];
extern const uint32_t        g_ftl_partition_count;

/* ── L2P Region 打包放置（唯一映射公式，metadata 与 l2p_cache 共用）── */

/*
 * ftl_l2p_region_placement - Region r → (block, page, plane) 打包映射
 *
 *   block = L2P_BASE_BLOCK + r / L2P_REGION_PER_BLOCK
 *   page  = (r % L2P_REGION_PER_BLOCK) / PLANE_NUM   ← 同页各 plane 都放 region
 *   plane = r % PLANE_NUM
 *
 * region_id 是**实例内**编号；各 (ch,ce,lun) 的块号相同（分片模型），故无 lun 形参。
 * 输出指针可为 NULL（只取需要的一项）。
 *
 * ⚠ 本公式归 metadata 所有（"第 r 个 region 放在元数据区的哪个物理位置"是**布局**），
 *   l2p_cache.h 的 `l2p_region_pba()` 只是把它包成带拓扑的 PBA。反过来让 metadata
 *   去 include L2P 缓存头拿这个公式是方向错的。
 */
static inline void ftl_l2p_region_placement(uint16_t  region_id,
                                            uint16_t *block,
                                            uint8_t  *page,
                                            uint8_t  *plane)
{
    if (block != NULL)
    {
        *block = (uint16_t)(L2P_BASE_BLOCK + (region_id / L2P_REGION_PER_BLOCK));
    }
    if (page != NULL)
    {
        *page = (uint8_t)((region_id % L2P_REGION_PER_BLOCK) / PLANE_NUM);
    }
    if (plane != NULL)
    {
        *plane = (uint8_t)(region_id % PLANE_NUM);
    }
}

/* ── 编译期自检（池几何与分区不重叠）── */

/* L2P 块数足以覆盖每实例的 region 数 */
_Static_assert(L2P_BLOCKS *L2P_REGION_PER_BLOCK >= L2P_REGIONS_PER_INSTANCE, "L2P 块数不足");

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
_Static_assert(L2P_BASE_BLOCK + L2P_BLOCKS <= ANCHOR_BLOCK0, "L2P 与 Anchor 重叠");
_Static_assert(ANCHOR_BLOCK0 + ANCHOR_POOL_BLOCKS <= CHECKPOINT_BLOCK0, "Anchor 与 Checkpoint 重叠");
_Static_assert(CHECKPOINT_BLOCK0 + CHECKPOINT_POOL_BLOCKS <= BBT_BLOCK, "Checkpoint 与 BBT 重叠");
_Static_assert(BBT_BLOCK + BBT_POOL_BLOCKS <= JOURNAL_BLOCK0, "BBT 与 Journal 重叠");
_Static_assert(JOURNAL_BLOCK0 + JOURNAL_BLOCK_COUNT <= WEAR_BLOCK, "Journal 与 Wear 重叠");

/* ── 元数据类型 ── */
#define METADATA_TYPE_L2P_REGION  (0U) /* L2P Region 数据 */
#define METADATA_TYPE_JOURNAL     (1U) /* Journal Block */
#define METADATA_TYPE_CHECKPOINT  (2U) /* Checkpoint */
#define METADATA_TYPE_BBT         (3U) /* Bad Block Table */
#define METADATA_TYPE_ERASE_COUNT (4U) /* 擦除计数表 */
#define METADATA_TYPE_COUNT       (5U)

/* ── Directory 容量 ──
 * = 每实例 L2P region 数：**每个 region 都必须能登记自己的位置**，否则
 * metadata_dir_update 的"满则丢弃"分支会把已落到动态块上的 region 位置丢掉
 * （重启后回落到固定位置 = 更新永久丢失）。容量取满即按构造消除该分支。*/
#define METADATA_DIR_ENTRIES          (L2P_REGIONS_PER_INSTANCE)
#define METADATA_DIR_ENTRIES_PER_PAGE (ANCHOR_ENTRIES_PER_PAGE)

/* ── 序列号锚定 ── */
#define ANCHOR_MAGIC (0x524F444EUL) /* "ANDIR" */

/* ── Entry: 7 字节（packed） ── */
typedef struct
{
    uint8_t    type;                        /* METADATA_TYPE_* */
    uint16_t   id;                          /* L2P region_id / 0 for others */
    nand_pba_t pba;                         /* 物理位置（PBA，自带 CH/CE ⇒ 不依赖"本实例元数据必在本实例 ch/ce"假设）*/
} __attribute__((packed)) metadata_entry_t; /* 7 bytes */

/* ── NAND 上持久化格式：**每页自包含** ──
 * Directory 有 METADATA_DIR_ENTRIES 条（14KB）⇒ 一份跨 ANCHOR_PAGE_COUNT 页，
 * 每页装 METADATA_DIR_ENTRIES_PER_PAGE 条，各自带 magic/sequence。
 * 单页损坏只丢该页（其余页仍可用），且读写只需一个 2KB 缓冲。
 * ⚠ 无本层 CRC——页完整性由下层 FC 的 per-CW CRC + ECC 保证（ADR v0.62）。*/
typedef struct
{
    uint32_t         magic;                                                                /* ANCHOR_MAGIC */
    uint32_t         sequence;                                                             /* 单调递增（同一份的各 chunk 相同）*/
    uint16_t         chunk_index;                                                          /* 0..ANCHOR_PAGE_COUNT-1 */
    uint16_t         chunk_count;                                                          /* = ANCHOR_PAGE_COUNT */
    uint32_t         entry_count;                                                          /* 本 chunk 有效 entry 数 */
    uint32_t         layout_version;                                                       /* FTL_MEDIA_LAYOUT_VERSION（版本不符 ⇒ 该页作废）*/
    metadata_entry_t entries[METADATA_DIR_ENTRIES_PER_PAGE];                               /* 256 × 7 = 1792B */
    uint8_t          padding[PAGE_MAIN_SIZE - 20U - (METADATA_DIR_ENTRIES_PER_PAGE * 7U)]; /* 填满一页主区 */
} metadata_anchor_page_t;

/* 一页必须装得下：头 20B + 每页条目（无独立 CRC，见 ADR v0.62）*/
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
    uint8_t  pages_per_record; /* 一条 record 占几页（须整除 PAGES_PER_BLOCK）*/
    uint8_t  plane;            /* 池所在 plane */
} metadata_ring_t;

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
 * @param full          0 = 廉价探针，非 0 = 整条校验
 * @param out_sequence  输出：该 record 的 sequence
 * @return FTL_OK = 该 record 在本次探针强度下有效
 *
 * ⚠ 契约：**失败时不写 *out_sequence**（调用者据此把"探针失败"与"sequence=0"分开）。
 *   写侧用 sequence 从 1 起，故 0 恒表示"未写"。
 */
typedef ftl_status_t (*metadata_ring_probe_fn)(void     *ctx,
                                               uint16_t  record_idx,
                                               int       full,
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
 * @param topo           ch/ce/lun
 * @param probe          调用方探针
 * @param ctx            探针上下文
 * @param out_newest_idx 输出：最新有效 record 槽位；**池内无有效 record 时为 UINT16_MAX**
 * @param out_sequence   输出：该 record 的 sequence；**池内无有效 record 时为 0**
 * @param out_write_idx  输出：下一个可写槽位
 * @return FTL_OK = 定位完成（含"池为空"）；FTL_ERROR = 参数非法
 */
ftl_status_t metadata_ring_locate(const metadata_ring_t *ring,
                                  const ftl_topology_t  *topo,
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
 * 同步站点用 metadata_ring_prepare_write（内部即刻擦），异步 job（阶段 6 起）按
 * *out_need_erase 自行提交异步擦命令——把"擦"放进状态机，而不是在服务循环里阻塞。
 *
 * @param ring          池几何
 * @param topo          ch/ce/lun
 * @param write_idx     写点槽位（来自 metadata_ring_locate 或上次写点 + 1）
 * @param out_addr      输出：record 第 0 页地址
 * @param out_need_erase 输出：非 0 = 写之前必须先擦 *out_addr 所在块
 * @return FTL_OK / FTL_ERROR（参数非法 / 槽位越界）
 */
ftl_status_t metadata_ring_prepare_addr(const metadata_ring_t *ring,
                                        const ftl_topology_t  *topo,
                                        uint16_t               write_idx,
                                        nand_pba_t            *out_addr,
                                        int                   *out_need_erase);

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

/* ── 元数据目录后台 job（v0.75 阶段 8）──
 *
 * 承载 L2P 日志块的两件"要碰 NAND 的整理活"，让它们不再阻塞服务循环：
 *   ① **回收**——某个日志块的活 region 数归零 ⇒ 擦块 + 归还 Free Pool；
 *   ② **压缩**——抗碎片：挑活 region 最少的块做 victim，把它的活 region 搬进当前
 *      块，落 Anchor 后擦除归还。
 *
 * 阶段序列（每条命令一个 phase，完成由 g_op_service[FTL_OP_METADATA] 收尾）：
 *
 *   IDLE ──reclaim_target──▶ RECLAIM（擦 + 归还）──▶ IDLE
 *   IDLE ──compact_wanted──▶ CP_READ ⇄ CP_WRITE（逐 region 搬移）
 *                              └─▶ CP_ANCHOR_PREP ─▶ [CP_ANCHOR_ERASE] ─▶
 *                                  CP_ANCHOR_WRITE ×N ─▶ CP_RECLAIM（擦 victim）──▶ DONE ─▶ IDLE
 *
 * ⚠ **顺序即掉电安全契约**（与旧同步实现逐条对应，不可倒）：
 *   ① 搬移 → ② 登记 → ③ Anchor 落盘 → ④ 擦 victim → ⑤ 归还。
 *   ③ 必须早于 ④——否则掉电后 Anchor 仍指向已擦块，那些 region 的映射永久丢失。
 *   job 里就是 phase 顺序：CP_ANCHOR_* 全在 CP_RECLAIM 之前。
 *
 * ⚠ **失败不回滚**：任一 phase 失败 ⇒ 中止，已登记的 region 已指向新页、未搬的仍指向
 *   victim（**victim 不擦**）⇒ 两边都有效，下次换块时重来即可。
 *
 * ⚠ **服务函数不推进状态机**（同 ftl_op_checkpoint）：CP_ANCHOR_PREP 要调
 *   metadata_dir_anchor_prepare（游标未建立时会**同步**扫池定位），在完成泵里跑就是
 *   嵌套泵。故 ftl_op_metadata 只记结果 + release + 改 phase，推进一律留给下一轮
 *   metadata_dir_kick（在 ftl_service 各档位里跑）。
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
} md_job_phase_t;

typedef struct
{
    uint8_t         phase;             /* md_job_phase_t */
    uint8_t         waiting;           /* 有命令在飞（完成服务函数清）*/
    uint8_t         compact_wanted;    /* append_slot 换块后池吃紧 ⇒ 请求压缩 */
    uint8_t         anchor_ok;         /* Anchor 分页写是否全部成功（0 = 整条作废）*/
    uint8_t         anchor_need_erase; /* Anchor 写点是否需先擦块 */
    uint8_t         anchor_chunk;      /* 已写的 Anchor 页序 */
    uint16_t        victim;            /* 压缩 victim 的 gblock（UINT16_MAX = 无）*/
    uint16_t        region_id;         /* 搬移游标（下一个待查 region_id）*/
    uint16_t        cur_region;        /* 当前搬移中的 region_id（CP_READ 命中时设定）*/
    uint16_t        moved;             /* 本次压缩已搬 region 数（观测）*/
    uint16_t        reclaim_target;    /* 待回收块（commit_region 归零即登记；UINT16_MAX = 无）*/
    uint16_t        reclaim_gblock;    /* 在飞擦除的块（完成时归还它）*/
    uint16_t        stall_rounds;      /* 连续"提交不出去"轮数（超限中止，同 MIG/CP）*/
    uint16_t        anchor_idx;        /* 本 Anchor record 的池内槽位 */
    uint32_t        anchor_seq;        /* 本 Anchor record 的序号 */
    nand_pba_t      anchor_base;       /* 本 Anchor record 第 0 页地址 */
    nand_pba_t      slot;              /* 压缩搬移已取到、待写的槽（NAND_PBA_INVALID = 未取）*/
    metadata_ring_t anchor_ring;       /* Anchor 池几何（prepare 出参，commit 要用）*/

    /* victim region 页搬运缓冲：**必须常驻**——异步读完成与随后的写之间隔着若干轮
     * 服务循环，栈帧早已返回。放 job 而非栈（同 mig.page_buf / rr_page_scratch）。 */
    uint8_t cp_buf[L2P_REGION_SIZE];
} md_job_t;

/* ── SRAM 常驻 Directory Cache（约 18KB，随 ftl_context_t 落 AXI_RAM） ── */
typedef struct
{
    metadata_entry_t entries[METADATA_DIR_ENTRIES];
    uint16_t         count;           /* 有效 entry 数 */
    uint32_t         anchor_sequence; /* 当前 Anchor sequence */
    uint8_t          dirty;           /* 是否有未写回 Anchor 的变更 */
    uint32_t         change_count;    /* 自上次 Anchor flush 后的变更数（仅节流用）*/

    /* 是否已有 region 落到动态块（block >= FTL_METADATA_BLOCK_END）。
     * entries 现在有 2048 条，而 ftl_is_metadata_block() 在开机回填里按块调用
     * 数千次 ⇒ 无条件线性扫全表会明显拖慢开机。为 0 时直接跳过动态扫描。 */
    uint8_t have_dynamic;

    /* ── L2P 日志块（元数据写模型：只追加 + 块池轮转）──
     * 每个 region 写进「当前日志块」的下一空槽（一 region = 一页）。块满则从
     * Free Pool 取新块（Free Pool 保证已擦除）。l2p_live 记每个日志块上还有多少
     * region 的**最新位置**落在这里 —— 归零即可擦除归还（metadata_dir_reclaim）。 */
    uint16_t l2p_live[FTL_BLOCK_TOTAL]; /* 每 gblock 的活 region 数 */
    uint16_t cur_metadata_block;        /* 当前日志块 gblock（cur_block_valid=0 时无意义）*/
    uint8_t  cur_metadata_page;         /* 当前日志块内下一可用页 */
    uint8_t  cur_block_valid;           /* 游标是否已建立（0 = 尚未分配日志块）*/
    uint8_t  cur_metadata_plane;        /* 下一个新日志块选哪个 plane（轮转）*/

    /* 后台整理 job（v0.75 阶段 8：回收 + 压缩，见 md_job_t）。
     * ⚠ 取代原 `compacting` 标志——它的两个作用现在都由 job 状态表达：
     *   ① **抑制逐次立即回收**（搬移 victim 的最后一个活 region 时 l2p_live[victim]
     *      归零，若就地擦掉就是**在 Anchor 落盘之前**擦 victim ⇒ 掉电丢映射）；
     *   ② **防重入 / 防抢槽**（压缩搬移要往当前块写，期间再发放槽位会打乱空槽预算）。
     * 判据分别是 md_compacting()（①）与 job.phase != IDLE（②）。 */
    md_job_t job;

    /* Anchor 池游标（ring 写点槽位；anchor_cursor_valid=0 时尚未定位）*/
    uint16_t anchor_write_idx;
    uint8_t  anchor_cursor_valid;

    /* 拓扑信息（元数据所在芯片路由） */
    ftl_topology_t topo;
} metadata_dir_cache_t;

/* ── API ── */

/*
 * metadata_dir_init - 初始化 Metadata Directory
 *
 * 首次使用时建立固定的 L2P Region 映射（哨兵层：永不写入，读回 0xFF ⇒ 未映射；
 * 打包放置：Block/Page/Plane 由 ftl_l2p_region_placement() 给出，见本文件上方同名小节）。
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
 * @return PBA，NAND_PBA_INVALID 表示未找到
 */
nand_pba_t metadata_dir_lookup(const metadata_dir_cache_t *dir,
                               uint8_t                     type,
                               uint16_t                    id);

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
 * 块满（或尚无日志块）⇒ 先 metadata_dir_reclaim 回收无活 region 的日志块，再从
 * Free Pool 取**整块**新日志块（allocator_alloc_reserved_block；Free Pool 保证
 * 已擦除，故无需擦）。**绝不回退到固定位置**：分配失败返回 NAND_PBA_INVALID，由上层重试。
 *
 * 日志块在哪个 plane 由 dir->cur_metadata_plane 轮转决定（写入目标与 region 归属
 * 无关：位置由 pba 自身携带，读回一律经 Directory 查 pba）。
 *
 * ⚠ **阶段 8 起本函数不再就地压缩**：换块后若 Free Pool 吃紧，只登记
 *   `job.compact_wanted` 并返回 NAND_PBA_INVALID——压缩由 metadata_dir_kick 起异步
 *   job 做（旧实现在这里同步跑完整个压缩，可阻塞服务循环数十毫秒）。
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
 * 更新 entries[region_id].pba + l2p_live（旧日志块 -1、新日志块 +1）+ dirty +
 * have_dynamic。旧块活计数归零且非当前块 ⇒ 把块号登记进 `job.reclaim_target`，
 * 由后台 job 异步擦除 + 归还 Free Pool。
 *
 * ⚠ **阶段 8 起本函数不再擦 NAND**（旧实现在这里同步擦）：本函数被
 *   `ftl_op_l2p`（L2P region 写回完成）调用，**运行在完成派发里** —— 就地擦会
 *   在完成泵内嵌套泵。改登记后 `ftl_op_l2p` 全路径无 NAND 访问。
 * ⚠ 压缩 job 在飞时**不登记**：压缩自己会把 victim 的活计数搬到 0，若此时就地
 *   登记，job 的 CP_RECLAIM 会在 Anchor 落盘**之前**擦 victim（③ 早于 ④ 契约）。
 *   压缩期间归零的块由 job 自己在 CP_RECLAIM 里擦。
 *
 * @param dir       Directory cache
 * @param alloc     Allocator（当前未使用，保留形参：回收已改由 job 做）
 * @param region_id L2P region_id
 * @param new_pba   刚写成功的位置（PBA）
 */
void metadata_dir_commit_region(metadata_dir_cache_t *dir,
                                ftl_allocator_t      *alloc,
                                uint16_t              region_id,
                                nand_pba_t            new_pba);

/*
 * metadata_dir_reclaim - 回收无活 region 的 L2P 日志块
 *
 * 遍历全部 gblock：l2p_live == 0、非当前日志块、且 BST 带 RESERVED 位（= 确实是
 * 本池独占的日志块，而非恰好活计数为 0 的用户块）⇒ 擦块 + allocator_free_reserved_block。
 *
 * 没有它会在稳态泄漏：每填满一块就永久占一块。
 *
 * ⚠ 本函数**只**回收 live == 0 的块。若某日志块混着永不再写的冷 region（如 63 冷 + 1 热），
 *   它永不归零 ⇒ 永久占块；最坏情况吃掉近半个 die（FTL_BLOCK_TOTAL/2 块）。
 *   抗碎片的「取 live 最小块做 victim、搬活 region 后擦」是后台压缩 job
 *   （`md_job_t` 的 CP_* 阶段，由 metadata_dir_kick 驱动）。
 *
 * ⚠ 本函数**保持同步**（残留项）：它只在 append_slot 换块分支与开机重建时调用，
 *   是后台路径且通常一个块都不擦。改成异步要额外引入"扫描请求位"，与分配顺序
 *   耦合，收益不成比例。**唯一调用者是 append_slot（已在后台 kick 路径之外）**。
 *
 * @param dir   Directory cache
 * @param alloc Allocator
 * @return FTL_OK（含无可回收块）/ FTL_ERROR（参数非法）
 */
ftl_status_t metadata_dir_reclaim(metadata_dir_cache_t *dir, ftl_allocator_t *alloc);

/*
 * metadata_dir_kick - 推进元数据目录后台 job（回收 / 压缩）一步
 *
 * 每轮服务循环（ftl_service_idle / normal / busy）调一次；**没有 job 在跑时**它就是
 * 起 job 的地方：先看 job.reclaim_target（有 ⇒ 起回收），再看 job.compact_wanted
 * （有 ⇒ 起压缩）。起不动（提交背压 / 无候选 / 空槽不够）只记账，下轮再来。
 *
 * ⚠ **所有状态推进都在这里**（不在完成服务函数里）：压缩的 CP_ANCHOR_PREP 阶段要调
 *   metadata_dir_anchor_prepare，而它在游标未建立时会**同步**扫池定位 —— 在完成泵里
 *   跑就是嵌套泵（同 ftl_op_checkpoint 的取舍）。服务函数只记结果 + release + 改
 *   phase，本函数负责"下一轮接着走"。
 * ⚠ 幂等且廉价：无 job 且无请求时立即返回（一次 load + 比较）。
 *
 * @param dir   Directory cache
 * @param alloc Allocator（取新日志块 / 归还 victim）
 */
void metadata_dir_kick(metadata_dir_cache_t *dir, ftl_allocator_t *alloc);

/*
 * metadata_dir_busy - 整理 job 是否在跑（有命令在飞或处在中间 phase）。
 *
 * 用途：让**调用者**区分"我的重试为什么没进展"与"卡死"。当前唯一读者是
 * checkpoint.c 的 cp_flush_regions：脏 region 取不到槽（FTL_AGAIN）时若 job 在推进，
 * 就**不该**累计 CP 的 stall 计数——否则一次正常的长压缩会把 Checkpoint 打回去。
 *
 * @return 1 = 有 job 在跑；0 = 空闲（或 dir == NULL）。
 */
static inline int metadata_dir_busy(const metadata_dir_cache_t *dir)
{
    return ((dir != NULL) && (dir->job.phase != (uint8_t)MD_JOB_IDLE)) ? 1 : 0;
}

/* ── Anchor 落盘的三段式（唯一形态；阶段 6 起）──
 *
 * 一条 Anchor record = ANCHOR_PAGE_COUNT 页。**所有**落盘站点（Checkpoint job、
 * 目录压缩 job）都拆成 prepare →（可选换块擦）→ 逐页写 → commit，每步之间可以回到
 * 服务循环。三段式把「Directory 格式」留在 metadata.c，「状态机」留给调用者。
 * （阶段 8 删掉了同步一次性版本 metadata_dir_flush_to_anchor——最后两个调用者都已
 * job 化。）
 */

/*
 * metadata_dir_anchor_prepare - 定位游标 + 取槽 + 算是否需换块擦（**不动介质**）
 *
 * @param dir            Directory
 * @param out_ring       出参：Anchor 池几何
 * @param out_base       出参：本 record 第 0 页地址
 * @param out_idx        出参：本 record 的池内槽位号（commit 要用）
 * @param out_seq        出参：本 record 的 sequence（本函数内自增 anchor_sequence）
 * @param out_need_erase 出参：非 0 = 写之前必须先擦 *out_base 所在块
 * @return 1 需要写（出参全部有效）；0 无需写（dir 不脏）；-1 失败（参数非法 / 定位失败）
 */
int metadata_dir_anchor_prepare(metadata_dir_cache_t *dir,
                                metadata_ring_t      *out_ring,
                                nand_pba_t           *out_base,
                                uint16_t             *out_idx,
                                uint32_t             *out_seq,
                                int                  *out_need_erase);

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
 * @param ok    0 = 有条分页写失败（整条作废，保留 dirty）
 */
void metadata_dir_anchor_commit(metadata_dir_cache_t *dir, const metadata_ring_t *ring, uint16_t idx, int ok);

/*
 * metadata_dir_load - 从 Anchor 池加载 Directory
 *
 * ① 先 fill_fixed 铺基础层（全部 region 的固定位置 = 哨兵层，读回即"未映射"）；
 * ② metadata_ring_locate 定位池内最新**整条校验通过**的 record；
 * ③ 读胜者各页装配覆盖对应区间（逐页再校验 chunk_index/sequence）；
 * ④ 由装配后的 entries 重建 l2p_live / have_dynamic。
 *
 * **游标不跨重启恢复**：cur_block_valid 置 0，首次追加时从 Free Pool 新取一块
 * （绝不复用可能已编程的旧页）。旧日志块仍被 Directory 引用 ⇒ 由 l2p_live 保住，
 * 待其 region 全部改写后由 metadata_dir_reclaim 归还。
 *
 * @param dir  Directory cache（输出）
 * @return FTL_OK / FTL_ERROR（无有效 Anchor：调用者转 metadata_dir_init）
 */
ftl_status_t metadata_dir_load(metadata_dir_cache_t *dir);

/*
 * ftl_is_metadata_block - 判断物理 Block 是否属于固定/动态元数据区
 *
 * 掉电恢复重建 closed_sb_list 时，L2P Table（固定表区 + 动态 region）、
 * Anchor、BBT、Checkpoint、Journal、Wear 等元数据块可能写满全部页（尤其
 * Journal 环形回切），逐页判空会把它们误判为满 Super Block。本谓词统一排除：
 *
 *   固定块：遍历 g_ftl_partitions[]（单一事实来源，见本文件「分区表」+ metadata.c），
 *           按 first_block/block_count 判区间（各 die 同一份块号）
 *   日志块：Metadata Directory 中登记的 L2P region 块（扫 entries）
 *   游标块：当前正在追加的 L2P 日志块（可能尚无任何 region 指向它，entries 扫不到）
 *
 * free pool 回填（main.c）与重建（ftl_rebuild_closed_sb_list）共用同一判据。
 *
 * ⚠ 固定块分支**与 plane 无关**（分片模型：每 die 一套完整元数据，块号不错开），
 *   但日志块/游标块是**从用户区动态取的** ⇒ 那两条分支必须带 plane 比较。
 *   旧实现一律只比 per-plane 块号：一个 plane 的日志块会连带把另一 plane 的同号块
 *   一起排除——该块既进不了 free pool、又被 F2 重建跳过，落在任何池之外。虽不丢数据
 *   （重建不认领 ⇒ GC 也不会擦它），但白丢一个可用块（上板可见 `free` 每有一个活
 *   日志块就少 1，且被掩块号随日志块迁移而漂移）。
 *
 * @param md_dir Metadata Directory cache（可为 NULL：仅固定块区间判断）
 * @param plane  物理 plane 号（0..PLANE_NUM-1）；仅日志块/游标块两条分支使用
 * @param block  物理 Block 号（per-plane 号，0..BLOCKS_PER_PLANE-1）
 * @return 1 = 元数据块，0 = 用户数据块
 */
int ftl_is_metadata_block(const metadata_dir_cache_t *md_dir, uint8_t plane, uint16_t block);

#endif /* FTL_METADATA_H */
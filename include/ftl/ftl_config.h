#ifndef FTL_CONFIG_H
#define FTL_CONFIG_H

/*
 * ftl_config.h — FTL 闪存分区布局（单一事实来源 / single source of truth）
 *
 * ══════════════════════════════════════════════════════════════════════
 * 设计原则：**没有任何块号/容量是写死的**。四层推导：
 *   ① 几何量（页/块/plane/LUN 数、页主区字节、CW 数）全部来自 nand_config.h；
 *   ② **容量** = f(几何量)：CW 总数 ÷ 每 region 条目数 = region 数；条目数 × 每条目
 *      用户字节 = 用户容量（§一）。容量不是输入，是几何的推论；
 *   ③ 每个分区的**块数** = f(几何量, 策略常量)。策略常量（副本数、每副本冗余
 *      页数、Journal 容量系数）在 §二 显式列出——这是本文件唯一允许"写死"的
 *      旋钮，且每个都写明了策略理由；
 *   ④ 每个分区的**起始块** = 前一分区起点 + 前一分区块数（§三 顺序累积）。
 * 换芯片（改 nand_config.h）或改策略（改 §二）→ 容量与全表自动重排。
 * ══════════════════════════════════════════════════════════════════════
 *
 * ── 几何量（include/nand/nand_config.h）──
 *   PAGE_MAIN_SIZE     2048 B   页主区（一页可放 512 条 4B PBA）
 *   PAGES_PER_BLOCK    64       每块页数
 *   PLANE_NUM          2        每 die plane 数
 *   BLOCKS_PER_PLANE   2048     每 plane 块数
 *   LUN_NUM            1        每片选 LUN 数
 *   CW_NUM             4        每页 codeword 数（= 每页扇区数 = 映射单元数）
 *   FC_BUFFER_USER_SIZE 512 B   每 CW 用户数据区（fc.h；= CW_SIZE − 2×8B 头尾）
 *
 * ── 容量公式（怎么算出来）──
 *   CW 总数    = FTL_BLOCK_TOTAL × PAGES_PER_BLOCK × CW_NUM        （每实例）
 *   region 数  = CW 总数 ÷ 每 region 条目数                        （每实例）
 *   条目总数   = region 数 × 每 region 条目数 × FTL_DIM_TOTAL       （= FTL_IU_TOTAL）
 *   用户容量   = 条目总数 × 每 CW 用户字节
 *
 * ── 尺寸公式（块数怎么算出来）──
 *   L2P Table   ⌈每实例 region 数 ÷ 每块槽位数⌉，每块槽位 = PAGES_PER_BLOCK × PLANE_NUM
 *   Anchor      ANCHOR_COPIES（主备）
 *   BBT         BBT_COPIES（主备）
 *   Checkpoint  CHECKPOINT_COPIES（主备）
 *   Journal     JOURNAL_BLOCKS（策略：容纳两次 checkpoint 之间的映射更新）
 *   Wear        WEAR_COPIES（软状态单副本）
 *
 * ── 当前几何下展开的布局（由 §二/§三 的公式算出，非手写）──
 *   块号        分区        块数  来历
 *   0..15       L2P Table   16    ⌈2048 region ÷ 128 region/块⌉
 *   16..17      Anchor      2     Directory 主备
 *   18..19      BBT         2     主 + 备
 *   20..21      Checkpoint  2     主备
 *   22..25      Journal     4     策略 = CW_NUM（CW 级 4× 映射流量）
 *   26          Wear        1     软状态单副本
 *   27..       用户数据    —     FTL_METADATA_BLOCK_END 之后
 *
 * ⚠ **每个 (ch,ce,lun) 用同一份块号**：本 FTL 是"分片模型"——每个 die 一个完整
 *   独立实例（各自的 L2P 表/Anchor/BBT/Journal/Checkpoint/Wear），主机 LBA 空间
 *   按 region 切分到各实例（lba_route_* / lba_to_local，见 l2p_cache.h）。
 *   分区**不随 LUN/CE/CH 错开**。
 *
 * ── 逻辑表字节（公式①：覆盖实体数 × 每实体字节）──
 *
 * ⚠ 本布局相对旧版（L2P 0..31 / Journal 40..41 / 无 Wear）**变更了介质格式**，
 *   首次上板须先擦除芯片（v24 教训：介质格式变更必须重新格式化）。
 * ⚠ 本文件只依赖 nand_config.h（几何）与 fc.h（CW 用户区大小），不引 l2p_cache/metadata，
 *   以免循环包含。
 */

#include "fc/fc.h" /* FC_BUFFER_USER_SIZE —— 映射单元的用户数据字节数 */
#include "nand/nand_config.h"

#include <stddef.h>
#include <stdint.h>

/* ══════════════════════════════════════════════════════════════════
 * 一、几何量（全部由 nand_config.h 推出，不写死）
 * ══════════════════════════════════════════════════════════════════ */

/* 全芯片块数（per-LUN）= 每 plane 块数 × plane 数 */
#define FTL_BLOCK_TOTAL (BLOCKS_PER_PLANE * PLANE_NUM)

/* 每块"页槽位"数 = 每块页数 × plane 数（同页两 plane 都放一个 region）*/
#define FTL_SLOTS_PER_BLOCK (PAGES_PER_BLOCK * PLANE_NUM)

/* 映射单元（IU）= 1 个 CW；每条 L2P 条目覆盖该 CW 的**用户数据区**。
 * ⚠ 不是 CW_SIZE(528)：CW 槽 = [META:4][CRC:4][USER:512][PARITY:8]，其中 16B
 *   由 FC 内部管理、不占 LBA 空间。用户区 = CW_SIZE − 2×FC_BUFFER_USER_OFFSET。
 *   （拿 CW_SIZE 当 IU 会算出 1985 region，破坏条目数 ↔ 容量的 1:1。）*/
#define FTL_IU_BYTES (FC_BUFFER_USER_SIZE)

/* 每 region 条目数 = 一页主区 ÷ 每条 4 B（一个 PBA）*/
#define L2P_ENTRIES_PER_REGION (PAGE_MAIN_SIZE / sizeof(uint32_t))

/* 每 region 字节数 = 一页主区 */
#define L2P_REGION_SIZE (L2P_ENTRIES_PER_REGION * sizeof(uint32_t))

/* 设备维度总数 = 通道 × 片选 × 每片选 LUN 数
 * —— 每个 (ch,ce,lun) 是一个**独立 FTL 实例**（分片模型，见 l2p_cache.h 路由）*/
#define FTL_DIM_TOTAL (CH_NUM * CE_NUM * LUN_NUM)

/* 每 (ch,ce,lun) 实例的 CW 总数 = 块数 × 每块页数 × 每页 CW 数 */
#define FTL_CW_TOTAL (FTL_BLOCK_TOTAL * PAGES_PER_BLOCK * CW_NUM)

/* 每 (ch,ce,lun) 实例的 region 数 = CW 总数 ÷ 每 region 条目数
 * （一条目映射一个 CW ⇒ 一个 region 覆盖 L2P_ENTRIES_PER_REGION 个 CW）
 * ⚠ **每实例**量，不含 × 维度数——每个 die 一份自己的 L2P 表。*/
#define L2P_REGIONS_PER_INSTANCE (FTL_CW_TOTAL / L2P_ENTRIES_PER_REGION)

/* 全设备映射单元总数（= 可寻址条目总数 = 最大 LBA + 1）
 * —— 每实例条目数 × 设备维度数 */
#define FTL_IU_TOTAL ((uint64_t)L2P_REGIONS_PER_INSTANCE * L2P_ENTRIES_PER_REGION * FTL_DIM_TOTAL)

/* 用户容量 = 条目总数 × 每条目用户字节
 * —— 由几何推出，非策略常量；当前几何下 = 器件主区容量 = 512 MiB。 */
#define FTL_USER_CAPACITY_BYTES (FTL_IU_TOTAL * FTL_IU_BYTES)

/* lba → (region_id, entry_idx) 分解：1 << L2P_REGION_SHIFT == 每 region 条目数 */
#define L2P_REGION_SHIFT (9U)
#define L2P_REGION_MASK  (L2P_ENTRIES_PER_REGION - 1U)

/* ══════════════════════════════════════════════════════════════════
 * 二、尺寸策略：策略常量 → 每个分区的块数
 * ══════════════════════════════════════════════════════════════════ */

/* ── L2P Table：region 打包进块（向上取整）──
 * 每实例一份，块号各 die 相同（不随 LUN/CE/CH 错开）*/
#define L2P_REGION_PER_BLOCK (FTL_SLOTS_PER_BLOCK)
#define L2P_BLOCKS           ((L2P_REGIONS_PER_INSTANCE + L2P_REGION_PER_BLOCK - 1U) / L2P_REGION_PER_BLOCK)

/* ── Anchor（Metadata Directory）：主备块 + 每块冗余份 ──
 * Directory 有 L2P_REGIONS_PER_INSTANCE 条（每条 7 B）⇒ **一页装不下**，须跨页。
 * 每页固定装 ANCHOR_ENTRIES_PER_PAGE 条（256×7 = 1792 ≤ 主区 2048）。
 * 索引：chunk = id / ANCHOR_ENTRIES_PER_PAGE；页 = copy × ANCHOR_PAGE_COUNT + chunk。*/
#define ANCHOR_COPIES           (2U) /* 策略：块级主备 */
#define ANCHOR_REDUNDANCY       (3U) /* 策略：每块内整份 Directory 的冗余份数 */
#define ANCHOR_BLOCKS           (ANCHOR_COPIES)
#define ANCHOR_ENTRIES_PER_PAGE (256U)
#define ANCHOR_PAGE_COUNT       ((L2P_REGIONS_PER_INSTANCE + ANCHOR_ENTRIES_PER_PAGE - 1U) / ANCHOR_ENTRIES_PER_PAGE)
#define ANCHOR_PAGE_PER_BLOCK   (ANCHOR_REDUNDANCY * ANCHOR_PAGE_COUNT)

/* ── BBT（坏块表）── */
#define BBT_COPIES      (2U)
#define BBT_REDUNDANCY  (3U)
#define BBT_BLOCKS      (BBT_COPIES)
#define BBT_TOTAL_PAGES (BBT_BLOCKS * BBT_REDUNDANCY)
#define BBT_MAX_BLOCKS  (FTL_BLOCK_TOTAL)
#define BBT_BITMAP_SIZE (BBT_MAX_BLOCKS / 8U)

/* ── Checkpoint（L2P 快照头）── */
#define CHECKPOINT_COPIES      (2U)
#define CHECKPOINT_REDUNDANCY  (3U)
#define CHECKPOINT_BLOCKS      (CHECKPOINT_COPIES)
#define CHECKPOINT_TOTAL_PAGES (CHECKPOINT_BLOCKS * CHECKPOINT_REDUNDANCY)

/* ── Journal（环形日志）──
 * 容量策略：Journal 必须容纳"两次 checkpoint 之间"的全部映射更新。
 * CW 级映射下，一次页写产生 CW_NUM 条 L2P 更新（IU = CW）
 * ⇒ 容量取 CW_NUM 块（旧版 2 块 → CW 级 4 块）。*/
#define JOURNAL_BLOCKS      (CW_NUM)
#define JOURNAL_BLOCK_COUNT (JOURNAL_BLOCKS)

/* ── Wear（擦除计数持久化；本版只预留块位，持久化见 P0-1）── */
#define WEAR_COPIES (1U) /* 软状态：单副本 */
#define WEAR_BLOCKS (WEAR_COPIES)

/* ══════════════════════════════════════════════════════════════════
 * 三、偏移：从元数据区起点顺序累积（不写死任何块号）
 * ══════════════════════════════════════════════════════════════════ */

/* 元数据区起点 */
#define FTL_METADATA_BASE_BLOCK (0U)

/* L2P Table：接在元数据区起点，打包放置（见 §六）*/
#define L2P_BASE_BLOCK (FTL_METADATA_BASE_BLOCK)

/* Anchor：接在 L2P 之后；ANCHOR_BLOCKS 块，块 0/1 = 主/备 */
#define ANCHOR_BLOCK0 (L2P_BASE_BLOCK + L2P_BLOCKS)
#define ANCHOR_BLOCK1 (ANCHOR_BLOCK0 + 1U)

/* BBT：接在 Anchor 之后 */
#define BBT_BLOCK        (ANCHOR_BLOCK0 + ANCHOR_BLOCKS)
#define BBT_BLOCK_BACKUP (BBT_BLOCK + 1U)

/* Checkpoint：接在 BBT 之后 */
#define CHECKPOINT_BLOCK0 (BBT_BLOCK + BBT_BLOCKS)
#define CHECKPOINT_BLOCK1 (CHECKPOINT_BLOCK0 + 1U)

/* Journal：第 b 块 = JOURNAL_BLOCK0 + b */
#define JOURNAL_BLOCK0 (CHECKPOINT_BLOCK0 + CHECKPOINT_BLOCKS)

/* Wear：接在 Journal 之后 */
#define WEAR_BLOCK (JOURNAL_BLOCK0 + JOURNAL_BLOCK_COUNT)

/* 固定元数据区末尾 = 第一个用户块；也用作动态元数据分配起点 */
#define FTL_METADATA_BLOCK_END (WEAR_BLOCK + WEAR_BLOCKS)

/* ══════════════════════════════════════════════════════════════════
 * 四、逻辑表字节（公式①：覆盖实体数 × 每实体字节）
 * ══════════════════════════════════════════════════════════════════ */

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

/* Wear：每块 4 B 擦除计数 */
#define FTL_WEAR_TABLE_BYTES ((uint32_t)FTL_BLOCK_TOTAL * sizeof(uint32_t))

/* ══════════════════════════════════════════════════════════════════
 * 五、分区表（运行期可枚举：谓词判据 + 开机打印）
 * ══════════════════════════════════════════════════════════════════ */

#define FTL_PART_FIXED   (0U) /* 固定位置（块号由 §三 推出）*/
#define FTL_PART_DYNAMIC (1U) /* 从 Free Pool 动态分配 */

typedef struct
{
    const char *name;        /* 分区名 */
    uint16_t    first_block; /* 起始块（per-plane 号；各 die 相同）*/
    uint16_t    block_count; /* 块数（§二）*/
    uint8_t     redundancy;  /* 冗余倍数（每块冗余页数/份数；软状态 1）*/
    uint8_t     kind;        /* FTL_PART_FIXED / FTL_PART_DYNAMIC */
    uint32_t    table_bytes; /* 逻辑表大小（§四）*/
} ftl_partition_t;

extern const ftl_partition_t g_ftl_partitions[];
extern const uint32_t        g_ftl_partition_count;

/* ══════════════════════════════════════════════════════════════════
 * 六、L2P Region 打包放置（唯一映射公式，l2p_cache 与 metadata 共用）
 * ══════════════════════════════════════════════════════════════════ */

/*
 * ftl_l2p_region_placement - Region r → (block, page, plane) 打包映射
 *
 *   block = L2P_BASE_BLOCK + r / L2P_REGION_PER_BLOCK
 *   page  = (r % L2P_REGION_PER_BLOCK) / PLANE_NUM   ← 同页各 plane 都放 region
 *   plane = r % PLANE_NUM
 *
 * region_id 是**实例内**编号；各 (ch,ce,lun) 的块号相同（分片模型），故无 lun 形参。
 * 输出指针可为 NULL（只取需要的一项）。
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

/* ══════════════════════════════════════════════════════════════════
 * 七、编译期自检（公式与策略的一致性）
 * ══════════════════════════════════════════════════════════════════ */

/* 分解移位与每 region 条目数一致 */
_Static_assert((1UL << L2P_REGION_SHIFT) == L2P_ENTRIES_PER_REGION, "L2P_REGION_SHIFT 与每 region 条目数不一致");

/* 映射单元（CW 用户区）与 CW 槽几何自洽：用户区 + 前 8B(META+CRC) + 后 8B(PARITY) == CW_SIZE */
_Static_assert(FTL_IU_BYTES + (2U * FC_BUFFER_USER_OFFSET) == CW_SIZE, "CW 用户区与 CW_SIZE 不自洽");

/* 维度数至少为 1 */
_Static_assert(FTL_DIM_TOTAL >= 1U, "设备维度数非法");

/* L2P 块数足以覆盖每实例的 region 数 */
_Static_assert(L2P_BLOCKS *L2P_REGION_PER_BLOCK >= L2P_REGIONS_PER_INSTANCE, "L2P 块数不足");

/* CW 总数必须整除每 region 条目数（否则最后一个 region 不满）*/
_Static_assert((FTL_CW_TOTAL % L2P_ENTRIES_PER_REGION) == 0U, "CW 总数不能被每 region 条目数整除");

/* 固定元数据区不越界 */
_Static_assert(FTL_METADATA_BLOCK_END <= BLOCKS_PER_PLANE, "固定元数据区超出每 plane 块数");

/* Anchor 每块占用的页数不超过每块页数（冗余份 × 每份页数）*/
_Static_assert(ANCHOR_PAGE_PER_BLOCK <= PAGES_PER_BLOCK, "Anchor 每块页数超出每块页数");

/* 分区按块号升序、互不重叠 */
_Static_assert(L2P_BASE_BLOCK + L2P_BLOCKS <= ANCHOR_BLOCK0, "L2P 与 Anchor 重叠");
_Static_assert(ANCHOR_BLOCK0 + ANCHOR_BLOCKS <= BBT_BLOCK, "Anchor 与 BBT 重叠");
_Static_assert(BBT_BLOCK + BBT_BLOCKS <= CHECKPOINT_BLOCK0, "BBT 与 Checkpoint 重叠");
_Static_assert(CHECKPOINT_BLOCK0 + CHECKPOINT_BLOCKS <= JOURNAL_BLOCK0, "Checkpoint 与 Journal 重叠");
_Static_assert(JOURNAL_BLOCK0 + JOURNAL_BLOCK_COUNT <= WEAR_BLOCK, "Journal 与 Wear 重叠");

#endif /* FTL_CONFIG_H */

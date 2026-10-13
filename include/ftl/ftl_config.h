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
 *               ⚠ 这是**哨兵层**：只给"从未写过的 region"一个确定位置，该区页
 *                 **永不写入**（保持擦除态 ⇒ 读回全 0xFF ⇒ 未映射）。写过的 region
 *                 一律落动态日志块（见 ftl/metadata.h 的追加日志）。
 *   Anchor      ANCHOR_POOL_BLOCKS（Directory 的块轮转日志池）
 *   Checkpoint  CHECKPOINT_POOL_BLOCKS（Header 的块轮转日志池）
 *   BBT         BBT_POOL_BLOCKS（坏块位图的块轮转日志池）
 *   Journal     JOURNAL_BLOCKS（策略：容纳两次 checkpoint 之间的映射更新）
 *   Wear        WEAR_COPIES（软状态单副本）
 *
 * ── 当前几何下展开的布局（由 §二/§三 的公式算出，非手写）──
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
 * ── 逻辑表字节（公式①：覆盖实体数 × 每实体字节）──
 *
 * ⚠ 本布局相对旧版（L2P 0..31 / Journal 40..41 / 无 Wear）**变更了介质格式**。
 *   v0.61 起**不再需要手动擦除芯片**：介质上的布局版本（`FTL_MEDIA_LAYOUT_VERSION`，
 *   写在 Checkpoint 头与 Anchor 页内）与当前固件不符（或根本没有当前版本记录）⇒
 *   `ssd_ftl_boot` 自动全片格式化一次并写入当前版本标记（见 power_failure_recovery.c
 *   `ftl_media_format`）。**任何改变介质格式的改动都必须把 `FTL_MEDIA_LAYOUT_VERSION` +1。**
 * ⚠ 本文件只依赖 nand_config.h（几何）与 fc.h（CW 用户区大小），不引 l2p_cache/metadata，
 *   以免循环包含。
 */

#include "common/assertion.h" /* ASSERT —— CW 级地址辅助的参数自检 */
#include "fc/fc.h"            /* FC_BUFFER_USER_SIZE —— 映射单元的用户数据字节数 */
#include "ftl/ftl_type.h"     /* lba_t —— CW 级地址辅助的形参/返回类型 */
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

/* ── Anchor（Metadata Directory）池 ──
 * Directory 有 L2P_REGIONS_PER_INSTANCE 条（每条 7 B）⇒ **一页装不下**，须跨页。
 * 每页固定装 ANCHOR_ENTRIES_PER_PAGE 条（256×7 = 1792 ≤ 主区 2048）。
 * 一份完整 Directory = 一条 record = ANCHOR_PAGE_COUNT 页（chunk）。
 * 池内每块装 ANCHOR_RECORDS_PER_BLOCK 条 record；块满换池内下一块（**先擦后写**）。*/
#define ANCHOR_ENTRIES_PER_PAGE  (256U)
#define ANCHOR_PAGE_COUNT        ((L2P_REGIONS_PER_INSTANCE + ANCHOR_ENTRIES_PER_PAGE - 1U) / ANCHOR_ENTRIES_PER_PAGE)
#define ANCHOR_POOL_BLOCKS       (32U) /* 策略：池内块数（唯一磨损旋钮，见文件头）*/
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
 * 块内**页式追加日志**（见 ftl/wear.h）：全量快照 17 页 ≤ PAGES_PER_BLOCK，
 * 块满才擦一次。软状态 ⇒ 单副本，不做主备。*/
#define WEAR_COPIES (1U) /* 软状态：单副本 */
#define WEAR_BLOCKS (WEAR_COPIES)

/* ══════════════════════════════════════════════════════════════════
 * 三、偏移：从元数据区起点顺序累积（不写死任何块号）
 * ══════════════════════════════════════════════════════════════════ */

/* 元数据区起点 */
#define FTL_METADATA_BASE_BLOCK (0U)

/* L2P Table：接在元数据区起点，打包放置（见 §六）。
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
 *        计数读成垃圾）。 */
#define FTL_MEDIA_LAYOUT_VERSION (3U)

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

/* Wear：每条目字节（block + read 计数 + 擦除计数 + rr 健康三字段，见 ftl/wear.h）*/
#define FTL_WEAR_ENTRY_BYTES (12U)

/* Wear：每块一条目 */
#define FTL_WEAR_TABLE_BYTES ((uint32_t)FTL_BLOCK_TOTAL * FTL_WEAR_ENTRY_BYTES)

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
    uint8_t     redundancy;  /* 每块容纳的 record 数（池内块轮转，见文件头 §元数据写模型；
                              * Journal/Wear 不是 record 池，填 1）*/
    uint8_t  kind;           /* FTL_PART_FIXED / FTL_PART_DYNAMIC */
    uint32_t table_bytes;    /* 逻辑表大小（§四）*/
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

/* ══════════════════════════════════════════════════════════════════
 * 八、CW 级地址辅助（IU = CW = 512B；LBA 与 PBA 低 3 位同构）
 *
 * 映射粒度 = 映射单元（IU）= 1 个 CW = 512 B（§一 FTL_IU_BYTES）。
 * LBA 与 PBA 的低 3 位同一套编码 [plane:1][cw:2]（nand_config.h §4.1）：
 *
 *     LBA 低位   [plane:1][cw:2]   ← 页内位置，与 PBA 逐位相同
 *     页基址     LBA & ~(CW_NUM-1) ← 低 2 位（cw）清零，plane 位仍在 bit CW_WIDTH
 *     页索引     LBA >> CW_WIDTH   ← 页粒度地址（bit 0 = plane，与 internal_lba 同）
 *
 * ⚠ 「页基址」不是页号：bit CW_WIDTH 仍是 plane 位。同一 LBA 段内相邻 4 个
 *   LBA（同一 plane 位）共用一页；相邻 8 个 LBA 覆盖一对 plane 页。
 *
 * 物理 plane 必须与 LBA 的 plane 位一致：FC 侧 desc->meta 的低 3 位由**物理**
 * plane/cw 铸出（fc_command.h），页内 META 存的就是「本 CW 的扇区 LBA」。若两者
 * 不一致，写进去的 META 会指向另一个 LBA，GC 反查/读校验都会错。
 * ══════════════════════════════════════════════════════════════════ */

/* 每页 CW 数 = 每页映射单元数 */
#define FTL_CW_PER_PAGE (CW_NUM)

/* 一对 plane 页覆盖的 LBA 数（8 = 2 plane × 4 CW） */
#define FTL_LBA_PER_PAIR (PLANE_NUM * CW_NUM)

/* LBA → 页基址（低 CW_WIDTH 位 = cw 清零；plane 位保留） */
static inline lba_t ftl_lba_page_base(lba_t lba)
{
    return (lba_t)(lba & ~(lba_t)(CW_NUM - 1U));
}

/* LBA → 页内 CW 号（0..CW_NUM-1） */
static inline uint8_t ftl_lba_cw(lba_t lba)
{
    return (uint8_t)(lba & (lba_t)(CW_NUM - 1U));
}

/* LBA → 该 LBA 应落在的**物理 plane**（bit CW_WIDTH 起，与 PBA 的 plane 位同源）。
 * 写路径按它选 plane、GC/WL 按它校验搬移目标 plane——「物理 plane == LBA 的 plane 位」
 * 是页内 META 可自洽还原 LBA 的前提（见本节头）。 */
static inline uint8_t ftl_lba_plane(lba_t lba)
{
    return (uint8_t)((lba >> CW_WIDTH) & (lba_t)(PLANE_NUM - 1U));
}

/* 页基址 + CW 号 → LBA（ftl_lba_page_base 的逆） */
static inline lba_t ftl_lba_make(lba_t page_base, uint8_t cw)
{
    return (lba_t)(page_base | (lba_t)cw);
}

/* LBA → 页索引（bit 0 = plane，与 PBA 的 plane 位对齐） */
static inline lba_t ftl_lba_page(lba_t lba)
{
    return (lba_t)(lba >> CW_WIDTH);
}

/* LBA → 所属「pair 窗口」基址（8 个 LBA 一组：低 3 位全清零） */
static inline lba_t ftl_lba_pair_base(lba_t lba)
{
    return (lba_t)(lba & ~(lba_t)(FTL_LBA_PER_PAIR - 1U));
}

/* 取 PBA 的**页**（CW 位清零）——同一物理页的 4 个 CW 共享它 */
static inline nand_pba_t pba_page(nand_pba_t pba)
{
    return (nand_pba_t)(pba & ~(nand_pba_t)(CW_NUM - 1U));
}

/* 单 plane 段内、CW [cw_lo, cw_hi] 的 flat 位图（段起点由调用者左移） */
static inline uint32_t ftl_cw_span_mask(uint8_t cw_lo, uint8_t cw_hi)
{
    ASSERT(cw_lo <= cw_hi);
    ASSERT(cw_hi < CW_NUM);
    return (uint32_t)((1U << (uint32_t)(cw_hi + 1U)) - 1U) & ~((1U << (uint32_t)cw_lo) - 1U);
}

#endif /* FTL_CONFIG_H */

#ifndef FTL_CONFIG_H
#define FTL_CONFIG_H

/*
 * ftl_config.h — FTL 几何推导 + 地址编码（LBA / PBA）
 *
 * 本文件回答三类问题，**不含任何元数据区布局**：
 *   ① **几何推导**（§一）——容量 = f(几何量)。几何量（页/块/plane/LUN 数、页主区字节、
 *      CW 数）全部来自 nand_config.h；容量不是输入，是几何的推论。
 *   ② **编译期自检**（§二）——几何量之间的一致性。
 *   ③ **地址编码**（§三）——FTL 的两套地址：
 *        §三.1 LBA 侧：映射单元（IU = 1 CW = 512 B）、页基址、页内 CW、物理 plane、pair 窗口；
 *        §三.2 PBA 侧：系统物理地址 `nand_pba_t` 的位序、字段提取、与 PAA 的字段重排。
 *      LBA 与 PBA 的低 3 位 [plane:1][cw:2] 是同一套编码（编码宏在 nand_config.h）。
 *
 * ⚠ 芯片内地址 PAA（`nand_paa_t`）与展开式地址 `nand_addr_t` **不在本文件**——它们是
 *   器件域的事实，在 **nand/nand_config.h**。PBA↔PAA 的转换函数在本文件，因为 PBA 是
 *   FTL 的地址货币（`pba_to_paa()` 一次转出 PAA 才跨进 FC）。
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
 * ⚠ **元数据区布局不在这里**——哪个分区占哪些块、每个池多大、每条 record 占几页、
 *   运行期分区表 `g_ftl_partitions[]`、以及 region 打包放置公式
 *   `ftl_l2p_region_placement()`，全部在 **ftl/metadata.h**（运行期表在
 *   src/ftl/metadata.c）。那是 metadata 模块的事实来源，不是"配置"。
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

/* 每块"页槽位"数 = 每块页数 × plane 数（同页两 plane 都放一个 region）
 * ⚠ 供 metadata.h 的 L2P 打包公式使用（L2P_REGION_PER_BLOCK）。*/
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
 * 二、编译期自检（几何量之间的一致性）
 *
 * ⚠ 元数据池几何与分区不重叠的 assert 已随布局宏搬到 ftl/metadata.h。
 * ══════════════════════════════════════════════════════════════════ */

/* 分解移位与每 region 条目数一致 */
_Static_assert((1UL << L2P_REGION_SHIFT) == L2P_ENTRIES_PER_REGION, "L2P_REGION_SHIFT 与每 region 条目数不一致");

/* 映射单元（CW 用户区）与 CW 槽几何自洽：用户区 + 前 8B(META+CRC) + 后 8B(PARITY) == CW_SIZE */
_Static_assert(FTL_IU_BYTES + (2U * FC_BUFFER_USER_OFFSET) == CW_SIZE, "CW 用户区与 CW_SIZE 不自洽");

/* 维度数至少为 1 */
_Static_assert(FTL_DIM_TOTAL >= 1U, "设备维度数非法");

/* CW 总数必须整除每 region 条目数（否则最后一个 region 不满）*/
_Static_assert((FTL_CW_TOTAL % L2P_ENTRIES_PER_REGION) == 0U, "CW 总数不能被每 region 条目数整除");

/* ══════════════════════════════════════════════════════════════════
 * 三、地址编码（LBA 侧 + PBA 侧）
 *
 * ── §三.1 LBA 侧（IU = CW = 512 B）──
 *
 * 映射粒度 = 映射单元（IU）= 1 个 CW = 512 B（§一 FTL_IU_BYTES）。
 * LBA 与 PBA 的低 3 位同一套编码 [plane:1][cw:2]（nand_config.h「共享低位编码」）：
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

/* ══════════════════════════════════════════════════════════════════
 * §三.2 PBA 侧（系统物理地址）
 *
 * 本工程有三套地址，位序**刻意不同**，各司其职：
 *
 *   PAA（nand_paa_t）  芯片内地址，低位优先 [CW:2][PAGE:6][PLANE:1][BLOCK:11][LUN]
 *                      —— 位序由 MT29F4G08 的 BA6/PA 规则决定，**不可随意改**；
 *                         paa_to_nand_row() 负责把它编成器件的行地址。
 *                         **定义于 nand/nand_config.h（器件域）**。
 *   PBA（nand_pba_t）  系统物理地址，低位优先 [CW:2][PLANE:1][PAGE:6][BLOCK:11][LUN][CE][CH]
 *                      —— 位序由本工程自定，**按 LBA 的结构排**（见下）；
 *                         L2P 表 / Journal / BST 存的就是它。**就是本节**。
 *   LBA               逻辑地址，与 PBA 逐位同构：[CH][CE][LUN][BLOCK][PAGE][PLANE][CW]
 *                      —— 低 3 位 = [plane:1][cw:2]，与 PBA 的低 3 位同一套编码。
 *
 * 为什么 PBA 不跟 PAA 共用位序：PAA 的 plane 在 page 之上（BA6 决定），
 * 而 FTL 的页号 internal_lba 里 plane 是最低位（plane1 = internal_lba + 1）。
 * 让 PBA 跟随 LBA 而不是 PAA，`LBA >> CW_WIDTH` 才逐位等于 internal_lba，
 * 今天所有 `+1` 约定与 L2P 编号才不用改。
 *
 * 代价：pba_to_paa() / paa_to_pba() 是**真正的字段重排**，不是掩码截取。
 * 收益：desc->paa 与 desc->meta 可用同一个 plane_cw 公式造出（基值不同）。
 *
 * PBA 位宽根据芯片实际参数（CH_NUM、CE_NUM、LUN_ADDR_WIDTH 等）自动计算。
 * 某宽度为 0 时对应位域不占用 PBA 编码空间。
 * ══════════════════════════════════════════════════════════════════ */

/* PBA 拓扑位宽：按硬件配置的最大值预留 */
#define PBA_CE_WIDTH (2U) /* CE 位宽，支持最多 4 CE */
#define PBA_CH_WIDTH (2U) /* CH 位宽，支持最多 4 CH */

/* PBA 移位（低位优先）。⚠ plane 在 page **之下**，与 PAA 相反。
 * ⚠ plane 的移位就是 PLANE_CW_SHIFT（= CW_WIDTH），与 LBA 的低 3 位同源
 *   —— 该宏在 nand_config.h，因为 FC 侧铸 desc->meta 也要用它。 */
#define PBA_CW_SHIFT    (0U)
#define PBA_PAGE_SHIFT  (PLANE_CW_SHIFT + PLANE_ADDR_WIDTH)  /* = 3 */
#define PBA_BLOCK_SHIFT (PBA_PAGE_SHIFT + PAGE_ADDR_WIDTH)   /* = 9 */
#define PBA_LUN_SHIFT   (PBA_BLOCK_SHIFT + BLOCK_ADDR_WIDTH) /* = 20 */
#define PBA_CE_SHIFT    (PBA_LUN_SHIFT + LUN_ADDR_WIDTH)
#define PBA_CH_SHIFT    (PBA_CE_SHIFT + PBA_CE_WIDTH)

/* PBA 掩码 */
#define PBA_PLANE_MASK NAND_FIELD_MASK(PLANE_ADDR_WIDTH)
#define PBA_PAGE_MASK  NAND_FIELD_MASK(PAGE_ADDR_WIDTH)
#define PBA_BLOCK_MASK NAND_FIELD_MASK(BLOCK_ADDR_WIDTH)
#define PBA_LUN_MASK   NAND_FIELD_MASK(LUN_ADDR_WIDTH)
#define PBA_CE_MASK    NAND_FIELD_MASK(PBA_CE_WIDTH)
#define PBA_CH_MASK    NAND_FIELD_MASK(PBA_CH_WIDTH)

typedef uint32_t nand_pba_t;

/* 无效 PBA 值。PBA 编码最多 26 bit，全 F 不可能是合法地址。 */
#define NAND_PBA_INVALID (0xFFFFFFFFU)

/* ── PBA 字段提取（直接按 PBA 位序取，不经 PAA）── */

static inline uint32_t pba_to_ch(nand_pba_t pba)
{
    return (pba >> PBA_CH_SHIFT) & PBA_CH_MASK;
}

static inline uint32_t pba_to_ce(nand_pba_t pba)
{
    return (pba >> PBA_CE_SHIFT) & PBA_CE_MASK;
}

static inline uint32_t pba_to_lun(nand_pba_t pba)
{
    return (pba >> PBA_LUN_SHIFT) & PBA_LUN_MASK;
}

static inline uint32_t pba_to_block(nand_pba_t pba)
{
    return (pba >> PBA_BLOCK_SHIFT) & PBA_BLOCK_MASK;
}

static inline uint32_t pba_to_page(nand_pba_t pba)
{
    return (pba >> PBA_PAGE_SHIFT) & PBA_PAGE_MASK;
}

static inline uint32_t pba_to_plane(nand_pba_t pba)
{
    return (pba >> PLANE_CW_SHIFT) & PBA_PLANE_MASK;
}

static inline uint16_t pba_to_cw(nand_pba_t pba)
{
    return (uint16_t)((pba >> PBA_CW_SHIFT) & NAND_FIELD_MASK(CW_WIDTH));
}

/* ── PBA 字段视图（4 字节位域，位序与 nand_pba_t 一致）──
 *
 * 用途 = **具名构造 / 读取 FTL 地址**，取代原先借道 nand_addr_t 的
 * pba_from_addr() / addr_from_pba()。FTL 层的地址自此只有 nand_pba_t 一种表示，
 * nand_addr_t 整体退出 FTL（只在 pba_to_paa() 这一个跨层转换器内部出现）。
 *
 * ⚠ 位域布局是 implementation-defined：本仓只跑 arm-none-eabi-gcc + 小端
 *   （config.mk 的 -mlittle-endian），该组合下位域自 LSB 起、按声明序分配 ⇒ 与
 *   PBA_*_SHIFT 一致。换编译器 / 大端即失效 ⇒ 由 pba_fields_layout_matches()
 *   在 ftl_init 里开机自检一次，不一致即断言，绝不静默算错地址。
 * ⚠ 只作**内存内视图**，不落介质 —— 介质一律写 nand_pba_t 的裸 4 字节。
 * ⚠ LUN_ADDR_WIDTH == 0 时**不能**声明 `lun : 0`（C11 6.7.2.1p4 禁止零宽位域），
 *   故用 #if 隐去；此时 CE 恰好落在 LUN 本应在的位上（PBA_CE_SHIFT == PBA_LUN_SHIFT）。
 */
typedef struct
{
    uint32_t cw : CW_WIDTH;
    uint32_t plane : PLANE_ADDR_WIDTH;
    uint32_t page : PAGE_ADDR_WIDTH;
    uint32_t block : BLOCK_ADDR_WIDTH;
#if (LUN_ADDR_WIDTH > 0U)
    uint32_t lun : LUN_ADDR_WIDTH;
#endif
    uint32_t ce : PBA_CE_WIDTH;
    uint32_t ch : PBA_CH_WIDTH;
} nand_pba_fields_t;

_Static_assert(sizeof(nand_pba_fields_t) == sizeof(nand_pba_t), "PBA 字段视图必须与 nand_pba_t 同为 4 字节");

/* PBA 实际占用的低位位数 —— 屏蔽位域结构体尾部的填充位 */
#define PBA_USED_BITS (PBA_CH_SHIFT + PBA_CH_WIDTH)
#define PBA_USED_MASK NAND_FIELD_MASK(PBA_USED_BITS)

/* 具名构造：按 PBA 的字段序填一个 PBA 字（cw 缺省 0 = 页基址）。
 * **按 PBA_*_SHIFT 宏打包**，不依赖位域布局（尾部位恒 0，无填充位垃圾）。
 * 这是 FTL 层「手搭地址」的唯一入口——位域结构体不能在 LUN_ADDR_WIDTH == 0 时
 * 声明 lun 成员，故把那一处 #if 收敛在本函数里（PBA_LUN_MASK 为 0 ⇒ lun 自然被忽略）。 */
static inline nand_pba_t pba_make(uint32_t ch, uint32_t ce, uint32_t lun, uint32_t block, uint32_t plane, uint32_t page, uint32_t cw)
{
    return ((nand_pba_t)(cw & NAND_FIELD_MASK(CW_WIDTH)) << PBA_CW_SHIFT) |
           ((nand_pba_t)(plane & PBA_PLANE_MASK) << PLANE_CW_SHIFT) |
           ((nand_pba_t)(page & PBA_PAGE_MASK) << PBA_PAGE_SHIFT) |
           ((nand_pba_t)(block & PBA_BLOCK_MASK) << PBA_BLOCK_SHIFT) |
           ((nand_pba_t)(lun & PBA_LUN_MASK) << PBA_LUN_SHIFT) |
           ((nand_pba_t)(ce & PBA_CE_MASK) << PBA_CE_SHIFT) |
           ((nand_pba_t)(ch & PBA_CH_MASK) << PBA_CH_SHIFT);
}

/* 字段视图 → PBA 字（值语义，pba_fields 的逆）。 */
static inline nand_pba_t pba_pack(nand_pba_fields_t f)
{
#if (LUN_ADDR_WIDTH > 0U)
    return pba_make(f.ch, f.ce, f.lun, f.block, f.plane, f.page, f.cw);
#else
    return pba_make(f.ch, f.ce, 0U, f.block, f.plane, f.page, f.cw);
#endif
}

/* PBA 字 → 字段视图（值语义）。逐字段走 pba_to_*（编译期可证的解码路径）。 */
static inline nand_pba_fields_t pba_fields(nand_pba_t pba)
{
    nand_pba_fields_t f;

    f.cw    = pba_to_cw(pba);
    f.plane = pba_to_plane(pba);
    f.page  = pba_to_page(pba);
    f.block = pba_to_block(pba);
    f.ce    = pba_to_ce(pba);
    f.ch    = pba_to_ch(pba);
#if (LUN_ADDR_WIDTH > 0U)
    f.lun = pba_to_lun(pba);
#endif

    return f;
}

/*
 * pba_fields_layout_matches - 位域视图布局自检（开机一次，见 ftl_init）
 *
 * 位域布局 implementation-defined ⇒ **无法**用 _Static_assert 证明：复合字面量的
 * union 读不是 C11 整型常量表达式，GCC 报 "expression in static assertion is not
 * constant"（实测）。退化为运行期一次性校验：逐字段置单比特，双关读出的字必须等于
 * 该字段在 PBA_*_SHIFT 上的单比特值。
 *
 * 值语义（pba_pack / pba_fields）本身**不依赖**本结论 —— 它们走宏。本检查保证的是
 * **强转可用**：`*(nand_pba_t *)&fields` 与 pba_pack(fields) 同值。
 *
 * @return 1 = 布局与 PBA_*_SHIFT 一致；0 = 不一致（调用者应断言）
 */
static inline uint8_t pba_fields_layout_matches(void)
{
    union
    {
        nand_pba_fields_t f;
        nand_pba_t        u;
    } v;
    uint8_t ok = 1U;

    v.f = (nand_pba_fields_t){.cw = 1U};
    ok  = (uint8_t)(ok & (uint8_t)((v.u & PBA_USED_MASK) == (1U << PBA_CW_SHIFT)));
    v.f = (nand_pba_fields_t){.plane = 1U};
    ok  = (uint8_t)(ok & (uint8_t)((v.u & PBA_USED_MASK) == (1U << PLANE_CW_SHIFT)));
    v.f = (nand_pba_fields_t){.page = 1U};
    ok  = (uint8_t)(ok & (uint8_t)((v.u & PBA_USED_MASK) == (1U << PBA_PAGE_SHIFT)));
    v.f = (nand_pba_fields_t){.block = 1U};
    ok  = (uint8_t)(ok & (uint8_t)((v.u & PBA_USED_MASK) == (1U << PBA_BLOCK_SHIFT)));
    v.f = (nand_pba_fields_t){.ce = 1U};
    ok  = (uint8_t)(ok & (uint8_t)((v.u & PBA_USED_MASK) == (1U << PBA_CE_SHIFT)));
    v.f = (nand_pba_fields_t){.ch = 1U};
    ok  = (uint8_t)(ok & (uint8_t)((v.u & PBA_USED_MASK) == (1U << PBA_CH_SHIFT)));
#if (LUN_ADDR_WIDTH > 0U)
    v.f = (nand_pba_fields_t){.lun = 1U};
    ok  = (uint8_t)(ok & (uint8_t)((v.u & PBA_USED_MASK) == (1U << PBA_LUN_SHIFT)));
#endif

    /* 值语义往返（走宏，与位域布局无关）：pba_pack(pba_fields(p)) == p */
    ok = (uint8_t)(ok & (uint8_t)(pba_pack(pba_fields((nand_pba_t)0x00FFFFFFU)) == (nand_pba_t)0x00FFFFFFU));

    return ok;
}

/* 从 PBA 提取芯片内地址（PAA）。两套位序不同 ⇒ **真正的字段重排**，不是掩码截取。 */
static inline nand_paa_t pba_to_paa(nand_pba_t pba)
{
    return paa_from_addr(&(nand_addr_t){.lun   = (uint8_t)pba_to_lun(pba),
                                        .block = (uint16_t)pba_to_block(pba),
                                        .plane = (uint8_t)pba_to_plane(pba),
                                        .page  = (uint8_t)pba_to_page(pba),
                                        .cw    = (uint8_t)pba_to_cw(pba)});
}

/* ── 便利构造 ── */

/* 从芯片内地址 PAA + CH/CE 构造 PBA（字段重排，pba_to_paa 的逆）。
 * 仅作为跨层桥接：PAA 从 FC 侧回来时用它还原 PBA（见 ftl_fc_interface.h 的 ftl_cmd_pba）。 */
static inline nand_pba_t paa_to_pba(uint32_t ch, uint32_t ce, nand_paa_t paa)
{
    return pba_make(ch, ce, paa_to_lun(paa), paa_to_block(paa), paa_to_plane(paa), paa_to_page(paa), paa_to_cw(paa));
}

/* 取 PBA 的**页**（CW 位清零）——同一物理页的 4 个 CW 共享它 */
static inline nand_pba_t pba_page(nand_pba_t pba)
{
    return (nand_pba_t)(pba & ~(nand_pba_t)(CW_NUM - 1U));
}

/* 获取 Page 基址 PBA（CW=0，页级操作基址）——与 pba_page 数值恒等，收敛到单漏斗 */
static inline nand_pba_t pba_page_base(nand_pba_t pba)
{
    return pba_page(pba);
}

/* 同 page 内换 CW */
static inline nand_pba_t pba_with_cw(nand_pba_t pba, uint32_t cw)
{
    return (pba & ~(nand_pba_t)NAND_FIELD_MASK(CW_WIDTH)) |
           ((cw & NAND_FIELD_MASK(CW_WIDTH)) << CW_WIDTH_SHIFT);
}

/* 替换 PBA 中的各字段（直接按 PBA 移位，不经 PAA 往返） */
static inline nand_pba_t pba_with_page(nand_pba_t pba, uint32_t page)
{
    return (nand_pba_t)((pba & ~(nand_pba_t)(PBA_PAGE_MASK << PBA_PAGE_SHIFT)) |
                        ((nand_pba_t)(page & PBA_PAGE_MASK) << PBA_PAGE_SHIFT));
}

static inline nand_pba_t pba_with_plane(nand_pba_t pba, uint32_t plane)
{
    return (nand_pba_t)((pba & ~(nand_pba_t)(PBA_PLANE_MASK << PLANE_CW_SHIFT)) |
                        ((nand_pba_t)(plane & PBA_PLANE_MASK) << PLANE_CW_SHIFT));
}

static inline nand_pba_t pba_with_block(nand_pba_t pba, uint32_t block)
{
    return (nand_pba_t)((pba & ~(nand_pba_t)(PBA_BLOCK_MASK << PBA_BLOCK_SHIFT)) |
                        ((nand_pba_t)(block & PBA_BLOCK_MASK) << PBA_BLOCK_SHIFT));
}

static inline nand_pba_t pba_with_lun(nand_pba_t pba, uint32_t lun)
{
    return (nand_pba_t)((pba & ~(nand_pba_t)(PBA_LUN_MASK << PBA_LUN_SHIFT)) |
                        ((nand_pba_t)(lun & PBA_LUN_MASK) << PBA_LUN_SHIFT));
}

static inline nand_pba_t pba_with_ch(nand_pba_t pba, uint32_t ch)
{
    return (nand_pba_t)((pba & ~(PBA_CH_MASK << PBA_CH_SHIFT)) |
                        ((ch & PBA_CH_MASK) << PBA_CH_SHIFT));
}

static inline nand_pba_t pba_with_ce(nand_pba_t pba, uint32_t ce)
{
    return (nand_pba_t)((pba & ~(PBA_CE_MASK << PBA_CE_SHIFT)) |
                        ((ce & PBA_CE_MASK) << PBA_CE_SHIFT));
}

/* ── 比较与校验 ── */

/* 是否同 Page（无视 CW） */
static inline uint8_t pba_same_page(nand_pba_t a, nand_pba_t b)
{
    return (uint8_t)((pba_page_base(a)) == (pba_page_base(b)));
}

/* 各字段是否在芯片规格范围内 */
static inline uint8_t pba_is_valid(nand_pba_t pba)
{
    return (uint8_t)((pba_to_page(pba) < PAGES_PER_BLOCK) &&
                     (pba_to_plane(pba) < PLANE_NUM) &&
                     (pba_to_block(pba) < BLOCKS_PER_PLANE) &&
                     (pba_to_lun(pba) < LUN_NUM) &&
                     (pba_to_cw(pba) < CW_NUM) &&
                     (pba_to_ch(pba) < CH_NUM) &&
                     (pba_to_ce(pba) < CE_NUM));
}

/* ══════════════════════════════════════════════════════════════════
 * §三.3 CW 段位图
 * ══════════════════════════════════════════════════════════════════ */

/* 单 plane 段内、CW [cw_lo, cw_hi] 的 flat 位图（段起点由调用者左移） */
static inline uint32_t ftl_cw_span_mask(uint8_t cw_lo, uint8_t cw_hi)
{
    ASSERT(cw_lo <= cw_hi);
    ASSERT(cw_hi < CW_NUM);
    return (uint32_t)((1U << (uint32_t)(cw_hi + 1U)) - 1U) & ~((1U << (uint32_t)cw_lo) - 1U);
}

/* 单个 plane 的全 CW 掩码（已左移到该 plane 的段）。
 * 一位一段：plane p 占 bit [p*CW_NUM, (p+1)*CW_NUM)——见 §三.3 的 flat 编号约定。 */
static inline uint32_t ftl_cw_plane_mask(uint8_t plane)
{
    ASSERT(plane < PLANE_NUM);
    return (uint32_t)(ftl_cw_span_mask(0U, CW_NUM - 1U) << ((uint32_t)plane * CW_NUM));
}

/* 位图覆盖了几个 plane（= 本命令的 multi-plane 数；全空 → 0，调用者自判非法）。
 * ⚠ 界走 PLANE_NUM（几何常量），与 fc_command_plane_span 的上界同源。 */
static inline uint8_t ftl_cw_bitmap_plane_count(uint32_t bitmap)
{
    uint8_t plane;
    uint8_t n = 0U;

    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        if ((bitmap & ftl_cw_plane_mask(plane)) != 0U)
        {
            n++;
        }
    }
    return n;
}

/* 位图的置位数（= 本页实际 CW 数，即 n_cw）。只看前 FTL_LBA_PER_PAIR 位——
 * 那是一页的总 CW 数，段外的高位不参与（与旧手写循环口径一致）。 */
static inline uint8_t ftl_cw_bitmap_popcount(uint32_t bitmap)
{
    uint8_t n = 0U;

    bitmap &= (uint32_t)((1U << (uint32_t)FTL_LBA_PER_PAIR) - 1U);
    while (bitmap != 0U)
    {
        n++;
        bitmap &= bitmap - 1U; /* 清最低置位 */
    }
    return n;
}

#endif /* FTL_CONFIG_H */

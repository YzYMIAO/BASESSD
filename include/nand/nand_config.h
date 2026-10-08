#ifndef NAND_CONFIG_H
#define NAND_CONFIG_H

#include <stdint.h>

/*
 * nand_config.h — NAND 逻辑几何 + 地址编码（PAA/PBA/nand_addr_t）
 *
 * 本头文件定义：
 *   - 逻辑几何宏（页/块/plane/lun/ch/ce 数量）
 *   - 软件地址编码：PAA（芯片内地址）、PBA（系统物理地址，含 CH/CE）、
 *     LBA（逻辑地址，与 PBA 逐位同构）—— 三者的位序与用途见下方 PBA 段
 *   - nand_addr_t（展开式地址，遍历路径用）
 *
 * 与芯片物理几何（nand_mt29f4g08abadawp.h）区分：本文件是 FC/FTL 的
 * 逻辑视角，物理视角见芯片头文件。
 */

/* 页大小：2112 = 主区 2048 + 备区 64 */
#define PAGE_SIZE (2112)

/* 通道页缓冲大小：单 plane 操作只用 PAGE_SIZE，多 plane 操作需要 PLANE_NUM 倍缓冲。 */
#define PAGE_BUFFER_SIZE (PAGE_SIZE * PLANE_NUM) /* 2112×2=4224 */
#define PAGE_MAIN_SIZE   (2048)                  /* 主区字节数 */
#define PAGE_SPARE_SIZE  (64)                    /* 备区字节数 */
#define PAGES_PER_BLOCK  (64)
#define BLOCKS_PER_PLANE (2048)
#define PLANE_NUM        (2)
#define DIE_NUM          (1)
#define LUN_NUM          (1) /* 每片选 LUN 数（MT29F4G08 单 die；换多 die 芯片只改此宏） */
#define CH_NUM           (1)
#define CE_NUM           (1)
#define CW_NUM           (4U)

/* 单 CW 字节数：整页缓冲按 CW 等分（CW 偏移 → 列地址、CW 拼接/拆分的步长）。
 * 528 = [META:4][CRC:4][USER:512][ECC:8]，布局见 include/fc/fc.h。 */
#define CW_SIZE (PAGE_SIZE / CW_NUM) /* 2112/4 = 528 */

/* 行地址数组深度（nand_fc_request_t.row[] 的长度）。与 PLANE_NUM 是两个独立维度：
 *   PLANE_NUM      —— 当前芯片实际 plane 数（MT29F4G08 = 2），是数据模型的"宽度"，
 *                     牵动 CW_TOTAL_MAX / PAGE_BUFFER_SIZE / plane_paa 的长度 /
 *                     FTL 的 valid_map[] 与 plane_blocks[]。
 *   NAND_PLANE_MAX —— 行地址数组的静态预留深度，只影响 row[] 的容量。
 * ⚠ 改 plane 数时两处都要动，别只改一个。
 * ⚠ 当前 NAND_PLANE_MAX(4) > PLANE_NUM(2)，row[2..3] 是永久未用槽位
 *   （nand_mt29f4g08abadawp.c 只拷前 2 个 plane）。
 * ⚠ 深度 4 覆盖不了 6Plane NAND —— 原注释写"为 4Plane/6Plane 预留"与取值自相矛盾，已更正。 */
#define NAND_PLANE_MAX (4U)

/* 多 plane 操作时 CW 总数上限 = max plane × CW per plane */
#define CW_TOTAL_MAX (CW_NUM * PLANE_NUM) /* 4×2=8 */

/*
 * ONFI 时序模式控制
 *
 * CONFIG_NAND_TIMING_MODE_4
 *   0 = 低速模式（NAND 默认时序，FMC 全速保守时序）
 *       所有 ONFI 操作在 NAND 默认 Mode 0（tRC=50ns）下运行，
 *       nand_init() 中跳过 SET_FEATURES 切换 Mode 4 的步骤。
 *       用于功能验证初期，排除时序问题。
 *
 *   1 = 高速模式（ONFI Mode 4，tRC=20ns）
 *       nand_init() 中会：
 *       ① 低速初始化 FMC + 访问 NAND 完成 RESET/READ_ID
 *       ② SET_FEATURES 切 NAND 到 Mode 4
 *       ③ 重新配置 FMC 硬件时序寄存器为 Mode 4 参数
 *       NAND 和 FMC 均运行在 50MHz 接口速率。
 *
 * 默认：0（低速模式），功能验证稳定后改为 1。
 */
#ifndef CONFIG_NAND_TIMING_MODE_4
#    define CONFIG_NAND_TIMING_MODE_4 (0U)
#endif

/* ── NAND 数据搬移模式 ──
 *
 * CONFIG_NAND_DMA
 *   0 = PIO（默认），CPU 逐字节读写 FMC 寄存器。
 *   1 = DMA，使用 FMC 专用 DMA 请求线（DMA_REQUEST_FMC），
 *       memory-to-peripheral / peripheral-to-memory 非阻塞模式。
 *
 * DMA 模式下 DATA_IN/DATA_OUT 阶段用 DMA 启动后 CPU 可处理其他事务，
 * engine step 轮询完成标志。Command/Address 阶段仍走 PIO。
 *
 * 运行时可通过全局变量 g_nand_dma_enable 动态切换。
 */
#ifndef CONFIG_NAND_DMA
#    define CONFIG_NAND_DMA (0U)
#endif

#define CW_WIDTH         (2U)  /* 4个codeword,各个cw的起始位置在4等分处。 */
#define PAGE_ADDR_WIDTH  (6U)  /* PA0~PA5，每 Block 64 页。 */
#define PLANE_ADDR_WIDTH (1U)  /* BA6，Plane 选择。 */
#define BLOCK_ADDR_WIDTH (11U) /* BA7~BA17，每 Plane BLOCKS_PER_PLANE 个 Block。 */
#define DIE_ADDR_WIDTH   (0U)
#define LUN_ADDR_WIDTH   (0U) /* MT29F4G08 单 die；换多 die 芯片改此宏 + LUN_NUM */

/*
 * 软件 PAA 打包布局（低位优先）：
 * [CW][PAGE][PLANE][BLOCK][LUN]
 *
 * 注意：这是 FC/FTL 的逻辑地址编码，不是 NAND 五个地址周期的原始排列。
 * NAND 行地址由 paa_to_nand_row() 按 MT29F4G08 的 BA/PA 规则重新编码。
 *
 * ⚠ PBA 与 LBA **不跟这套位序**（plane 在 page 之上），而是另立一套
 *   [CW][PLANE][PAGE][BLOCK][LUN][CE][CH] —— 见下方 PBA 段。
 *   本段（PAA）一个字节都不要动：它的位序由 BA6/PA 规则决定。
 */
#define CW_WIDTH_SHIFT  (0U)
#define PAA_PAGE_SHIFT  (CW_WIDTH_SHIFT + CW_WIDTH)
#define PAA_PLANE_SHIFT (PAA_PAGE_SHIFT + PAGE_ADDR_WIDTH)
#define PAA_BLOCK_SHIFT (PAA_PLANE_SHIFT + PLANE_ADDR_WIDTH)
#define PAA_LUN_SHIFT   (PAA_BLOCK_SHIFT + BLOCK_ADDR_WIDTH)

#define NAND_FIELD_MASK(width) ((width) == 0U ? 0UL : ((1UL << (width)) - 1UL))
#define PAA_PAGE_MASK          NAND_FIELD_MASK(PAGE_ADDR_WIDTH)
#define PAA_PLANE_MASK         NAND_FIELD_MASK(PLANE_ADDR_WIDTH)
#define PAA_BLOCK_MASK         NAND_FIELD_MASK(BLOCK_ADDR_WIDTH)
#define PAA_LUN_MASK           NAND_FIELD_MASK(LUN_ADDR_WIDTH)

typedef uint32_t nand_paa_t;

static inline uint32_t paa_to_page(nand_paa_t paa)
{
    return (paa >> PAA_PAGE_SHIFT) & PAA_PAGE_MASK;
}

static inline uint32_t paa_to_plane(nand_paa_t paa)
{
    return (paa >> PAA_PLANE_SHIFT) & PAA_PLANE_MASK;
}

static inline uint32_t paa_to_block(nand_paa_t paa)
{
    return (paa >> PAA_BLOCK_SHIFT) & PAA_BLOCK_MASK;
}

static inline uint32_t paa_to_lun(nand_paa_t paa)
{
    return (paa >> PAA_LUN_SHIFT) & PAA_LUN_MASK;
}

static inline nand_paa_t paa_make(uint32_t page, uint32_t plane, uint32_t block, uint32_t lun, uint32_t cw)
{
    return ((cw & NAND_FIELD_MASK(CW_WIDTH)) << CW_WIDTH_SHIFT) |
           ((page & PAA_PAGE_MASK) << PAA_PAGE_SHIFT) |
           ((plane & PAA_PLANE_MASK) << PAA_PLANE_SHIFT) |
           ((block & PAA_BLOCK_MASK) << PAA_BLOCK_SHIFT) |
           ((lun & PAA_LUN_MASK) << PAA_LUN_SHIFT);
}

static inline uint8_t paa_is_valid(uint32_t page, uint32_t plane, uint32_t block, uint32_t lun, uint32_t cw)
{
    return (uint8_t)((page < PAGES_PER_BLOCK) && (plane < PLANE_NUM) && (block < BLOCKS_PER_PLANE) &&
                     (lun < LUN_NUM) && (cw < CW_NUM));
}

/* MT29F4G08 行地址：PA[5:0] -> bit[5:0]，Plane/BA6 -> bit[6]，BA[17:7] -> bit[17:7]。 */
static inline uint32_t paa_to_nand_row(nand_paa_t paa)
{
    return paa_to_page(paa) |
           (paa_to_plane(paa) << PAGE_ADDR_WIDTH) |
           (paa_to_block(paa) << (PAGE_ADDR_WIDTH + PLANE_ADDR_WIDTH));
}

static inline uint16_t paa_to_cw(nand_paa_t paa)
{
    return (uint16_t)((paa >> CW_WIDTH_SHIFT) & NAND_FIELD_MASK(CW_WIDTH));
}

/* ── PAA 便利函数 ── */

/* Page 基址（CW=0，页级操作基址） */
static inline nand_paa_t paa_page_base(nand_paa_t paa)
{
    return paa & ~(nand_paa_t)NAND_FIELD_MASK(CW_WIDTH);
}

/* 同 page 内换 CW */
static inline nand_paa_t paa_with_cw(nand_paa_t paa, uint32_t cw)
{
    const uint32_t cw_mask = NAND_FIELD_MASK(CW_WIDTH);
    return (paa & ~(nand_paa_t)cw_mask) | ((cw & cw_mask) << CW_WIDTH_SHIFT);
}

/* 是否同 Page（无视 CW） */
static inline uint8_t paa_same_page(nand_paa_t a, nand_paa_t b)
{
    return (uint8_t)((paa_page_base(a)) == (paa_page_base(b)));
}

/* 各字段是否在芯片规格范围内（nand_paa_t 版本） */
static inline uint8_t paa_is_valid_paa(nand_paa_t paa)
{
    return (uint8_t)((paa_to_page(paa) < PAGES_PER_BLOCK) &&
                     (paa_to_plane(paa) < PLANE_NUM) &&
                     (paa_to_block(paa) < BLOCKS_PER_PLANE) &&
                     (paa_to_lun(paa) < LUN_NUM) &&
                     (paa_to_cw(paa) < CW_NUM));
}

/* ── PAA 替换字段 ── */

static inline nand_paa_t paa_with_page(nand_paa_t paa, uint32_t page)
{
    return (nand_paa_t)((paa & ~(PAA_PAGE_MASK << PAA_PAGE_SHIFT)) |
                        ((page & PAA_PAGE_MASK) << PAA_PAGE_SHIFT));
}

static inline nand_paa_t paa_with_plane(nand_paa_t paa, uint32_t plane)
{
    return (nand_paa_t)((paa & ~(PAA_PLANE_MASK << PAA_PLANE_SHIFT)) |
                        ((plane & PAA_PLANE_MASK) << PAA_PLANE_SHIFT));
}

static inline nand_paa_t paa_with_block(nand_paa_t paa, uint32_t block)
{
    return (nand_paa_t)((paa & ~(PAA_BLOCK_MASK << PAA_BLOCK_SHIFT)) |
                        ((block & PAA_BLOCK_MASK) << PAA_BLOCK_SHIFT));
}

static inline nand_paa_t paa_with_lun(nand_paa_t paa, uint32_t lun)
{
    return (nand_paa_t)((paa & ~(PAA_LUN_MASK << PAA_LUN_SHIFT)) |
                        ((lun & PAA_LUN_MASK) << PAA_LUN_SHIFT));
}

/* ── PBA（系统物理地址）定义 ──
 *
 * 本工程有三套地址，位序**刻意不同**，各司其职：
 *
 *   PAA（nand_paa_t）  芯片内地址，低位优先 [CW:2][PAGE:6][PLANE:1][BLOCK:11][LUN]
 *                      —— 位序由 MT29F4G08 的 BA6/PA 规则决定，**不可随意改**；
 *                         paa_to_nand_row() 负责把它编成器件的行地址。
 *   PBA（nand_pba_t）  系统物理地址，低位优先 [CW:2][PLANE:1][PAGE:6][BLOCK:11][LUN][CE][CH]
 *                      —— 位序由本工程自定，**按 LBA 的结构排**（见下）；
 *                         L2P 表 / Journal / BST 存的就是它。
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
 */

/* PBA 拓扑位宽：按硬件配置的最大值预留 */
#define PBA_CE_WIDTH (2U) /* CE 位宽，支持最多 4 CE */
#define PBA_CH_WIDTH (2U) /* CH 位宽，支持最多 4 CH */

/* PBA 移位（低位优先）。⚠ plane 在 page **之下**，与 PAA 相反。 */
#define PBA_CW_SHIFT    (0U)
#define PBA_PLANE_SHIFT (PBA_CW_SHIFT + CW_WIDTH)            /* = 2 */
#define PBA_PAGE_SHIFT  (PBA_PLANE_SHIFT + PLANE_ADDR_WIDTH) /* = 3 */
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

/* LBA 与 PBA 共用的低位掩码：[plane:1][cw:2]。
 *
 *   const uint32_t plane_cw = (plane << PBA_PLANE_SHIFT) | cw;
 *   desc->paa  = pba_to_paa((row_pba     & ~PLANE_CW_MASK) | plane_cw);
 *   desc->meta =            (command->meta & ~PLANE_CW_MASK) | plane_cw;
 *
 * —— 同一个 plane_cw 造出两个值，基值不同（物理 PBA vs 逻辑 LBA）。 */
#define PLANE_CW_MASK ((uint32_t)((NAND_FIELD_MASK(PLANE_ADDR_WIDTH) << PBA_PLANE_SHIFT) | NAND_FIELD_MASK(CW_WIDTH)))

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
    return (pba >> PBA_PLANE_SHIFT) & PBA_PLANE_MASK;
}

static inline uint16_t pba_to_cw(nand_pba_t pba)
{
    return (uint16_t)((pba >> PBA_CW_SHIFT) & NAND_FIELD_MASK(CW_WIDTH));
}

/* 从 PBA 提取芯片内地址（PAA）。两套位序不同 ⇒ **真正的字段重排**，不是掩码截取。 */
static inline nand_paa_t pba_to_paa(nand_pba_t pba)
{
    return paa_make(pba_to_page(pba), pba_to_plane(pba), pba_to_block(pba), pba_to_lun(pba), pba_to_cw(pba));
}

static inline uint32_t pba_to_nand_row(nand_pba_t pba)
{
    return paa_to_nand_row(pba_to_paa(pba));
}

/* 构造 PBA（按 PBA 位序打包，**不经 paa_make**）。签名与旧版一致。 */
static inline nand_pba_t pba_make(uint32_t ch, uint32_t ce, uint32_t page, uint32_t plane, uint32_t block, uint32_t lun, uint32_t cw)
{
    return ((nand_pba_t)(cw & NAND_FIELD_MASK(CW_WIDTH)) << PBA_CW_SHIFT) |
           ((nand_pba_t)(plane & PBA_PLANE_MASK) << PBA_PLANE_SHIFT) |
           ((nand_pba_t)(page & PBA_PAGE_MASK) << PBA_PAGE_SHIFT) |
           ((nand_pba_t)(block & PBA_BLOCK_MASK) << PBA_BLOCK_SHIFT) |
           ((nand_pba_t)(lun & PBA_LUN_MASK) << PBA_LUN_SHIFT) |
           ((nand_pba_t)(ce & PBA_CE_MASK) << PBA_CE_SHIFT) |
           ((nand_pba_t)(ch & PBA_CH_MASK) << PBA_CH_SHIFT);
}

/* ── 便利构造 ── */

/* 从芯片内地址 PAA + CH/CE 构造 PBA（字段重排，pba_to_paa 的逆） */
static inline nand_pba_t paa_to_pba(uint32_t ch, uint32_t ce, nand_paa_t paa)
{
    return pba_make(ch, ce, paa_to_page(paa), paa_to_plane(paa), paa_to_block(paa), paa_to_lun(paa), paa_to_cw(paa));
}

/* ── LBA → CW META ──
 *
 * META 存的是**本 CW 的扇区 LBA**：一个 page 有 4 CW，每个 CW 对应一个扇区 LBA。
 *   低 3 位 = [plane:1][cw:2]，高位保留 LBA 原有值。
 *
 * ⚠ 入参 lba 是**扇区** LBA（不是页 LBA）；plane 必须由调用者给
 *   —— 单靠 `lba & ~3` 会连 plane 位一起抹掉（plane0 CW1 与 plane1 CW0 会撞值）。
 * 与 PBA 的低 3 位同一套编码（PLANE_CW_MASK / PBA_PLANE_SHIFT），
 * 故 desc->meta 与 desc->paa 可用同一个 plane_cw 造出。 */
static inline uint32_t lba_to_meta(uint32_t lba, uint32_t plane, uint32_t cw)
{
    return (lba & ~PLANE_CW_MASK) |
           ((plane & NAND_FIELD_MASK(PLANE_ADDR_WIDTH)) << PBA_PLANE_SHIFT) |
           (cw & NAND_FIELD_MASK(CW_WIDTH));
}

/* 获取 Page 基址 PBA（CW=0，页级操作基址） */
static inline nand_pba_t pba_page_base(nand_pba_t pba)
{
    return pba & ~(nand_pba_t)NAND_FIELD_MASK(CW_WIDTH);
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
    return (nand_pba_t)((pba & ~(nand_pba_t)(PBA_PLANE_MASK << PBA_PLANE_SHIFT)) |
                        ((nand_pba_t)(plane & PBA_PLANE_MASK) << PBA_PLANE_SHIFT));
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

/* ══════════════════════════════════════════════════════════════
 * nand_addr_t — 展开的芯片内地址（遍历/迭代用）
 *
 * 存储主控内部地址分两层：
 *   nand_addr_t（展开） — 遍历路径用，字段直接访问不需要 shift
 *   nand_pba_t  (packed) — L2P 表存储接口用，32-bit 紧凑
 *
 * 遍历路径不再先 pba_make 打包再 ftl_cmd_setup_ppa 拆包，
 * 而是直接用 addr_to_cmd（定义于 ftl_fc_interface.h）填入 FC 命令。
 * ══════════════════════════════════════════════════════════════ */

typedef struct
{
    uint8_t  ch;    /* Channel */
    uint8_t  ce;    /* Chip Enable */
    uint8_t  lun;   /* LUN (Die) */
    uint16_t block; /* Block number (0..2047) */
    uint8_t  plane; /* Plane (0/1) */
    uint8_t  page;  /* Page in block (0..63) */
    uint8_t  cw;    /* Code Word (0..3, 0=全部) */
} nand_addr_t;

/* nand_addr_t → nand_pba_t */
static inline nand_pba_t pba_from_addr(const nand_addr_t *addr)
{
    return pba_make(addr->ch, addr->ce, addr->page, addr->plane, addr->block, addr->lun, addr->cw);
}

/* nand_pba_t → nand_addr_t（展开）。allocator/L2P 返回紧凑 PBA，
 * 遍历路径填 FC 命令前展开一次。 */
static inline nand_addr_t addr_from_pba(nand_pba_t pba)
{
    nand_addr_t addr;

    addr.ch    = (uint8_t)pba_to_ch(pba);
    addr.ce    = (uint8_t)pba_to_ce(pba);
    addr.lun   = (uint8_t)pba_to_lun(pba);
    addr.block = (uint16_t)pba_to_block(pba);
    addr.plane = (uint8_t)pba_to_plane(pba);
    addr.page  = (uint8_t)pba_to_page(pba);
    addr.cw    = (uint8_t)pba_to_cw(pba);

    return addr;
}

/* 从 PAA + CH/CE 构造 nand_addr_t（元数据固定位置 / Directory 查找结果用） */
static inline nand_addr_t addr_from_paa(uint8_t ch, uint8_t ce, nand_paa_t paa)
{
    nand_addr_t addr;

    addr.ch    = ch;
    addr.ce    = ce;
    addr.lun   = (uint8_t)paa_to_lun(paa);
    addr.block = (uint16_t)paa_to_block(paa);
    addr.plane = (uint8_t)paa_to_plane(paa);
    addr.page  = (uint8_t)paa_to_page(paa);
    addr.cw    = (uint8_t)paa_to_cw(paa);

    return addr;
}

#endif /* NAND_CONFIG_H */

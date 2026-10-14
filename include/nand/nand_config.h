#ifndef NAND_CONFIG_H
#define NAND_CONFIG_H

#include <stdint.h>

/*
 * nand_config.h — NAND 器件几何 + 芯片内地址（PAA / nand_addr_t）
 *
 * 本头文件只定义**器件域**的事实：
 *   - 逻辑几何宏（页/块/plane/lun/ch/ce 数量）与 ONFI 时序开关
 *   - 字段位宽（CW/PAGE/PLANE/BLOCK/LUN）
 *   - 芯片内地址 PAA（nand_paa_t）—— 位序由 MT29F4G08 的 BA6/PA 规则决定
 *   - nand_addr_t（展开式地址；三套打包地址共用的构造入口）
 *   - LBA/PBA 共用的低位编码 [plane:1][cw:2]（PLANE_CW_SHIFT / PLANE_CW_MASK /
 *     lba_to_meta）—— FC 侧铸 desc->meta 要用，故留在本层
 *
 * ⚠ **系统物理地址 PBA（nand_pba_t）不在这里**——它含 CH/CE 拓扑、位序按 LBA 的结构
 *   排、是 FTL 的地址货币（`pba_to_paa()` 一次转出 PAA 才跨进 FC），整体在
 *   **ftl/ftl_config.h §三**。
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
 *   [CW][PLANE][PAGE][BLOCK][LUN][CE][CH] —— 见 ftl/ftl_config.h §三。
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

/* ══════════════════════════════════════════════════════════════
 * nand_addr_t — 展开式地址（具名字段）
 *
 * 三套打包地址（PAA / PBA / LBA）都以它为**唯一的构造入口**：
 *   paa_from_addr() / pba_from_addr()   展开 → 打包
 *   addr_from_paa() / addr_from_pba()   打包 → 展开
 *
 * 为什么用具名字段而不是位置形参：三个打包地址的字段都是同型小整数，
 * 位置形参写反了**依然编译通过、依然落在合法值域内**（page<64、plane<2、
 * block<2048 全都成立）⇒ 静默算出错地址。具名字段把这类错误变成编译错误。
 *
 * 为什么定义在此处（而非文件末尾）：paa_from_addr() 定义在 PAA 段内。
 *
 * 为什么用普通字段而不是位域：① 访问更快；② C 编译器无位域可移植性问题
 * （设计原则见 include/common/ftl_types.h）；③ 本构建 LUN_ADDR_WIDTH == 0，
 * 而 C11 6.7.2.1p4 规定 0 宽位域不得有声明符 ⇒ 具名 `lun : 0` 直接编译不过。
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

/* 由展开地址构造 PAA（芯片内地址）。
 * ⚠ PAA 不含 CH/CE —— 片选是命令级信息，不属于芯片内地址，
 *   故本函数**忽略 addr->ch / addr->ce**。 */
static inline nand_paa_t paa_from_addr(const nand_addr_t *addr)
{
    return ((nand_paa_t)(addr->cw & NAND_FIELD_MASK(CW_WIDTH)) << CW_WIDTH_SHIFT) |
           ((nand_paa_t)(addr->page & PAA_PAGE_MASK) << PAA_PAGE_SHIFT) |
           ((nand_paa_t)(addr->plane & PAA_PLANE_MASK) << PAA_PLANE_SHIFT) |
           ((nand_paa_t)(addr->block & PAA_BLOCK_MASK) << PAA_BLOCK_SHIFT) |
           ((nand_paa_t)(addr->lun & PAA_LUN_MASK) << PAA_LUN_SHIFT);
}

/* 展开地址的各字段是否在芯片规格范围内（nand_addr_t 版本）。
 * 打包版本见下方 paa_is_valid_paa()。 */
static inline uint8_t paa_is_valid_addr(const nand_addr_t *addr)
{
    return (uint8_t)((addr->page < PAGES_PER_BLOCK) && (addr->plane < PLANE_NUM) && (addr->block < BLOCKS_PER_PLANE) &&
                     (addr->lun < LUN_NUM) && (addr->cw < CW_NUM));
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

/* ══════════════════════════════════════════════════════════════
 * 共享低位编码 —— LBA 与 PBA 的 [plane:1][cw:2]
 *
 * LBA 与 PBA 的低 3 位是**同一套编码**：低 2 位 = cw，bit 2 = plane。
 *   PBA 侧：pba_to_plane() / pba_to_cw() 取的就是这两位；
 *   LBA 侧：页内位置（plane 位在 bit CW_WIDTH，见 ftl_config.h §三）。
 * FC 侧铸命令的 desc->meta 也用这两位（fc_command.h / fc_data_path.c），
 * 所以**本段留在 NAND 层**，不随 PBA 搬走。
 * ══════════════════════════════════════════════════════════════ */

/* plane 在 [plane:1][cw:2] 中的移位（= cw 位宽）。名字中性：LBA 侧同样适用。 */
#define PLANE_CW_SHIFT (CW_WIDTH)

/* LBA 与 PBA 共用的低位掩码：[plane:1][cw:2]。
 *
 *   const uint32_t plane_cw = (plane << PLANE_CW_SHIFT) | cw;
 *   desc->paa  = pba_to_paa((row_pba     & ~PLANE_CW_MASK) | plane_cw);
 *   desc->meta =            (command->meta & ~PLANE_CW_MASK) | plane_cw;
 *
 * —— 同一个 plane_cw 造出两个值，基值不同（物理 PBA vs 逻辑 LBA）。 */
#define PLANE_CW_MASK ((uint32_t)((NAND_FIELD_MASK(PLANE_ADDR_WIDTH) << PLANE_CW_SHIFT) | NAND_FIELD_MASK(CW_WIDTH)))

/* plane 位必须紧邻 cw 位之上，中间不留空档 */
_Static_assert(PLANE_CW_MASK == (uint32_t)NAND_FIELD_MASK(CW_WIDTH + PLANE_ADDR_WIDTH),
               "PLANE_CW_MASK 必须是连续的 [plane:1][cw:2]");

/* ── LBA → CW META ──
 *
 * META 存的是**本 CW 的扇区 LBA**：一个 page 有 4 CW，每个 CW 对应一个扇区 LBA。
 *   低 3 位 = [plane:1][cw:2]，高位保留 LBA 原有值。
 *
 * ⚠ 入参 lba 是**扇区** LBA（不是页 LBA）；plane 必须由调用者给
 *   —— 单靠 `lba & ~3` 会连 plane 位一起抹掉（plane0 CW1 与 plane1 CW0 会撞值）。
 * 与 PBA 的低 3 位同一套编码（PLANE_CW_MASK / PLANE_CW_SHIFT），
 * 故 desc->meta 与 desc->paa 可用同一个 plane_cw 造出。 */
static inline uint32_t lba_to_meta(uint32_t lba, uint32_t plane, uint32_t cw)
{
    return (lba & ~PLANE_CW_MASK) |
           ((plane & NAND_FIELD_MASK(PLANE_ADDR_WIDTH)) << PLANE_CW_SHIFT) |
           (cw & NAND_FIELD_MASK(CW_WIDTH));
}

/* ══════════════════════════════════════════════════════════════
 * 打包地址 → 展开地址 的转换器（PAA 侧）
 *
 * nand_addr_t（展开）  — 定义于文件开头；遍历路径用，字段直接访问不需要 shift
 * nand_pba_t  (packed) — L2P 表存储接口用，32-bit 紧凑（**定义于 ftl/ftl_config.h**）
 *
 * 反向的 paa_from_addr() 定义在 PAA 段内；PBA 侧的 pba_from_addr() / addr_from_pba()
 * 随 PBA 一并落在 ftl/ftl_config.h §三。
 *
 * 遍历路径不再先打包再 ftl_cmd_setup_ppa 拆包，
 * 而是直接用 addr_to_cmd（定义于 ftl_fc_interface.h）填入 FC 命令。
 * ══════════════════════════════════════════════════════════════ */

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

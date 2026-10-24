#ifndef NAND_CONFIG_H
#define NAND_CONFIG_H

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

#include <stdint.h>

/*
 * nand_config.h — NAND 通用层：器件选择 + FC/FTL 软件地址布局
 *
 * 本文件只放**器件无关**的软件布局：
 *   - 按 makefile 的 NAND_DEVICE 宏选择器件头（几何/拓扑/位宽/行编码/ID）
 *   - CW 布局（CW_NUM / CW_SIZE / PAGE_BUFFER_SIZE / NAND_PLANE_MAX）
 *   - LUN 位宽推导 + PAA 打包布局（[CW][PAGE][PLANE][BLOCK][LUN]）
 *   - nand_addr_t 与 PAA 访问器 / 替换函数
 *   - LBA/PBA 共用的低位编码 [plane:1][cw:2]
 *
 * 器件事实（PAGE_SIZE / PAGES_PER_BLOCK / PLANE_NUM / LUN_NUM / CH_NUM /
 * CE_NUM / 地址位宽 / NAND_PAA_TO_ROW / FMC / ID）在
 * `nand/devices/<family>.h`，由宏选择：
 *
 *     make NAND_DEVICE=MT29F___G08A      # 默认
 *
 * 将来加器件：在 devices/ 下新增器件头，并在下面加一个 #elif 分支。
 *
 * ⚠ 系统物理地址 PBA（DWORD）不在这里——它含 CH/CE 拓扑、位序按 LBA 排、
 *   是 FTL 的地址货币（`pba_to_paa()` 一次转出 PAA 才跨进 FC），整体在
 *   **ftl/ftl_config.h §三**。
 */

#if defined(NAND_DEVICE_MT29F___G08A)
#    include "mt29f___g08a.h"
#else
#    error "NAND device not selected: make NAND_DEVICE=<family>"
#endif

/* ══════════════════════════════════════════════════════════════
 * 一、位宽推导与软件布局
 * ══════════════════════════════════════════════════════════════ */

/* LUN 位宽：由器件头的 LUN_NUM 推导（ceil-log2，最多 4 bit）。
 * LUN_NUM 是 2 的幂时恰好相等；非 2 的幂时上位补零（值域仍由
 * paa_is_valid_* 检查）。 */
#if LUN_NUM <= 1
#    define LUN_ADDR_WIDTH (0U)
#elif LUN_NUM <= 2
#    define LUN_ADDR_WIDTH (1U)
#elif LUN_NUM <= 4
#    define LUN_ADDR_WIDTH (2U)
#elif LUN_NUM <= 8
#    define LUN_ADDR_WIDTH (3U)
#elif LUN_NUM <= 16
#    define LUN_ADDR_WIDTH (4U)
#else
#    error "LUN_NUM > 16 is not supported"
#endif

/* CW 布局（FC 层软件选择，不是器件几何）：每页 4 个 CW，码字 528B。
 * 528 = [META:4][CRC:4][USER:512][ECC:8]，布局见 include/fc/fc.h。 */
#define CW_NUM   (4U)
#define CW_WIDTH (2U) /* 4 个 codeword ⇒ 2 bit 编号 */
#define CW_SIZE  (PAGE_SIZE / CW_NUM)

/* 通道页缓冲大小：单 plane 操作只用 PAGE_SIZE，多 plane 操作需要 PLANE_NUM 倍。 */
#define PAGE_BUFFER_SIZE (PAGE_SIZE * PLANE_NUM)

/* 行地址数组深度（nand_command_request_t.row[] 的长度）。与 PLANE_NUM 是两个独立维度：
 *   PLANE_NUM      —— 当前器件实际 plane 数，是数据模型的"宽度"，
 *                     牵动 CW_TOTAL_MAX / PAGE_BUFFER_SIZE / plane_paa 的长度 /
 *                     FTL 的 valid_map[] 与 plane_blocks[]。
 *   NAND_PLANE_MAX —— 行地址数组的静态预留深度，只影响 row[] 的容量。
 * ⚠ 改 plane 数时两处都要动，别只改一个。
 * ⚠ 当前 NAND_PLANE_MAX(4) > PLANE_NUM(2)，row[2..3] 是未用槽位
 *   （request 包装按 NAND_PLANE_MAX 整体拷贝）。
 * ⚠ 深度 4 覆盖不了 6Plane NAND。 */
#define NAND_PLANE_MAX (4U)

/* 多 plane 操作时 CW 总数上限 = max plane × CW per plane */
#define CW_TOTAL_MAX (CW_NUM * PLANE_NUM)

/* ══════════════════════════════════════════════════════════════
 * 二、PAA 打包布局（低位优先）
 *
 * [CW][PAGE][PLANE][BLOCK][LUN]
 *
 * 注意：这是 FC/FTL 的逻辑地址编码，不是 NAND 行地址的原始排列。
 * NAND 行地址由器件头提供的 NAND_PAA_TO_ROW() 重新编码（MT29F 的 BA/PA 规则）。
 *
 * ⚠ PBA 与 LBA **不跟这套位序**（plane 在 page 之上），而是另立一套
 *   [CW][PLANE][PAGE][BLOCK][LUN][CE][CH] —— 见 ftl/ftl_config.h §三。
 *   本段（PAA）一个字节都不要动：它的位序由器件地址映射规则决定。
 * ══════════════════════════════════════════════════════════════ */

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

/* ── 编译期自检（器件事实 ↔ 软件布局）── */
_Static_assert((1U << LUN_ADDR_WIDTH) >= LUN_NUM, "LUN_ADDR_WIDTH 覆盖不了 LUN_NUM");
_Static_assert(PAGE_ADDR_WIDTH + PLANE_ADDR_WIDTH + BLOCK_ADDR_WIDTH + LUN_ADDR_WIDTH <= 24U, "行地址超过 3 字节：ROW_0..2 只发 3 个行周期");
_Static_assert(PAA_LUN_SHIFT + LUN_ADDR_WIDTH <= 32U, "PAA 超出 32 位");
_Static_assert(PAGE_MAIN_SIZE + PAGE_SPARE_SIZE == PAGE_SIZE, "主区 + 备区 != PAGE_SIZE");
_Static_assert((1U << CW_WIDTH) == CW_NUM, "CW_WIDTH 与 CW_NUM 不一致");

/* NAND 地址周期数（供开机与 ONFI 参数页的 addrcyc 比对）：
 *   列地址 = 页内字节偏移的宽度，PAGE_SIZE=2112 ⇒ 12 bit ⇒ 2 字节；
 *   行地址 = page + plane + block + lun 的位宽，18 bit ⇒ 3 字节。
 * H4：这里的推导只依赖器件头给的几何/位宽宏。 */
#define NAND_COLUMN_ADDR_CYCLES ((PAGE_SIZE <= 256U) ? 1U : ((PAGE_SIZE <= 65536U) ? 2U : 3U))
#define NAND_ROW_ADDR_BITS      (PAGE_ADDR_WIDTH + PLANE_ADDR_WIDTH + BLOCK_ADDR_WIDTH + LUN_ADDR_WIDTH)
#define NAND_ROW_ADDR_CYCLES    ((NAND_ROW_ADDR_BITS + 7U) / 8U)

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
 * （设计原则见 include/ftl/ftl_type.h）；③ 本构建 LUN_ADDR_WIDTH == 0，
 * 而 C11 6.7.2.1p4 规定 0 宽位域不得有声明符 ⇒ 具名 `lun : 0` 直接编译不过。
 * ══════════════════════════════════════════════════════════════ */

typedef struct
{
    BYTE ch;    /* Channel */
    BYTE ce;    /* Chip Enable */
    BYTE lun;   /* LUN (Die) */
    WORD block; /* Block number (0..2047) */
    BYTE plane; /* Plane (0/1) */
    BYTE page;  /* Page in block (0..63) */
    BYTE cw;    /* Code Word (0..3, 0=全部) */
} nand_addr_t;

static inline DWORD paa_to_page(DWORD paa)
{
    return (paa >> PAA_PAGE_SHIFT) & PAA_PAGE_MASK;
}

static inline DWORD paa_to_plane(DWORD paa)
{
    return (paa >> PAA_PLANE_SHIFT) & PAA_PLANE_MASK;
}

static inline DWORD paa_to_block(DWORD paa)
{
    return (paa >> PAA_BLOCK_SHIFT) & PAA_BLOCK_MASK;
}

static inline DWORD paa_to_lun(DWORD paa)
{
    return (paa >> PAA_LUN_SHIFT) & PAA_LUN_MASK;
}

/* 由展开地址构造 PAA（芯片内地址）。
 * ⚠ PAA 不含 CH/CE —— 片选是命令级信息，不属于芯片内地址，
 *   故本函数**忽略 addr->ch / addr->ce**。 */
static inline DWORD paa_from_addr(const nand_addr_t *addr)
{
    return ((DWORD)(addr->cw & NAND_FIELD_MASK(CW_WIDTH)) << CW_WIDTH_SHIFT) | ((DWORD)(addr->page & PAA_PAGE_MASK) << PAA_PAGE_SHIFT) | ((DWORD)(addr->plane & PAA_PLANE_MASK) << PAA_PLANE_SHIFT) | ((DWORD)(addr->block & PAA_BLOCK_MASK) << PAA_BLOCK_SHIFT) | ((DWORD)(addr->lun & PAA_LUN_MASK) << PAA_LUN_SHIFT);
}

/* 展开地址的各字段是否在芯片规格范围内（nand_addr_t 版本）。
 * 打包版本见下方 paa_is_valid_paa()。 */
static inline BYTE paa_is_valid_addr(const nand_addr_t *addr)
{
    return ((addr->page < PAGES_PER_BLOCK) && (addr->plane < PLANE_NUM) && (addr->block < BLOCKS_PER_PLANE) && (addr->lun < LUN_NUM) && (addr->cw < CW_NUM));
}

/* 芯片行地址：具体编码由器件头提供（NAND_PAA_TO_ROW）。 */
static inline DWORD paa_to_nand_row(DWORD paa)
{
    return NAND_PAA_TO_ROW(paa);
}

static inline WORD paa_to_cw(DWORD paa)
{
    return ((paa >> CW_WIDTH_SHIFT) & NAND_FIELD_MASK(CW_WIDTH));
}

/* ── PAA 便利函数 ── */

/* Page 基址（CW=0，页级操作基址） */
static inline DWORD paa_page_base(DWORD paa)
{
    return paa & ~NAND_FIELD_MASK(CW_WIDTH);
}

/* 同 page 内换 CW */
static inline DWORD paa_with_cw(DWORD paa, DWORD cw)
{
    const DWORD cw_mask = NAND_FIELD_MASK(CW_WIDTH);
    return (paa & ~cw_mask) | ((cw & cw_mask) << CW_WIDTH_SHIFT);
}

/* 是否同 Page（无视 CW） */
static inline BYTE paa_same_page(DWORD a, DWORD b)
{
    return ((paa_page_base(a)) == (paa_page_base(b)));
}

/* 各字段是否在芯片规格范围内（DWORD 版本） */
static inline BYTE paa_is_valid_paa(DWORD paa)
{
    return ((paa_to_page(paa) < PAGES_PER_BLOCK) && (paa_to_plane(paa) < PLANE_NUM) && (paa_to_block(paa) < BLOCKS_PER_PLANE) && (paa_to_lun(paa) < LUN_NUM) && (paa_to_cw(paa) < CW_NUM));
}

/* ── PAA 替换字段 ── */

static inline DWORD paa_with_page(DWORD paa, DWORD page)
{
    return ((paa & ~(PAA_PAGE_MASK << PAA_PAGE_SHIFT)) | ((page & PAA_PAGE_MASK) << PAA_PAGE_SHIFT));
}

static inline DWORD paa_with_plane(DWORD paa, DWORD plane)
{
    return ((paa & ~(PAA_PLANE_MASK << PAA_PLANE_SHIFT)) | ((plane & PAA_PLANE_MASK) << PAA_PLANE_SHIFT));
}

static inline DWORD paa_with_block(DWORD paa, DWORD block)
{
    return ((paa & ~(PAA_BLOCK_MASK << PAA_BLOCK_SHIFT)) | ((block & PAA_BLOCK_MASK) << PAA_BLOCK_SHIFT));
}

static inline DWORD paa_with_lun(DWORD paa, DWORD lun)
{
    return ((paa & ~(PAA_LUN_MASK << PAA_LUN_SHIFT)) | ((lun & PAA_LUN_MASK) << PAA_LUN_SHIFT));
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
 *   const DWORD plane_cw = (plane << PLANE_CW_SHIFT) | cw;
 *   desc->paa  = pba_to_paa((row_pba     & ~PLANE_CW_MASK) | plane_cw);
 *   desc->meta =            (command->meta & ~PLANE_CW_MASK) | plane_cw;
 *
 * —— 同一个 plane_cw 造出两个值，基值不同（物理 PBA vs 逻辑 LBA）。 */
#define PLANE_CW_MASK ((DWORD)((NAND_FIELD_MASK(PLANE_ADDR_WIDTH) << PLANE_CW_SHIFT) | NAND_FIELD_MASK(CW_WIDTH)))

/* plane 位必须紧邻 cw 位之上，中间不留空档 */
_Static_assert(PLANE_CW_MASK == NAND_FIELD_MASK(CW_WIDTH + PLANE_ADDR_WIDTH), "PLANE_CW_MASK 必须是连续的 [plane:1][cw:2]");

/* ── LBA → CW META ──
 *
 * META 存的是**本 CW 的扇区 LBA**：一个 page 有 4 CW，每个 CW 对应一个扇区 LBA。
 *   低 3 位 = [plane:1][cw:2]，高位保留 LBA 原有值。
 *
 * ⚠ 入参 lba 是**扇区** LBA（不是页 LBA）；plane 必须由调用者给
 *   —— 单靠 `lba & ~3` 会连 plane 位一起抹掉（plane0 CW1 与 plane1 CW0 会撞值）。
 * 与 PBA 的低 3 位同一套编码（PLANE_CW_MASK / PLANE_CW_SHIFT），
 * 故 desc->meta 与 desc->paa 可用同一个 plane_cw 造出。 */
static inline DWORD lba_to_meta(DWORD lba, DWORD plane, DWORD cw)
{
    return (lba & ~PLANE_CW_MASK) | ((plane & NAND_FIELD_MASK(PLANE_ADDR_WIDTH)) << PLANE_CW_SHIFT) | (cw & NAND_FIELD_MASK(CW_WIDTH));
}

/* ══════════════════════════════════════════════════════════════
 * 打包地址 → 展开地址 的转换器（PAA 侧）
 *
 * nand_addr_t（展开）  — 定义于文件开头；遍历路径用，字段直接访问不需要 shift
 * DWORD  (packed) — L2P 表存储接口用，32-bit 紧凑（**定义于 ftl/ftl_config.h**）
 *
 * 反向的 paa_from_addr() 定义在 PAA 段内；PBA 侧的构造/转换（pba_make / pba_pack /
 * pba_fields / pba_to_paa）落在 ftl/ftl_config.h §三。**v0.73 起 `nand_addr_t` 已退出
 * FTL 层** ⇒ 本文件的 addr_from_paa() 在 `src/` 下暂无调用者（保留作 NAND/FC 侧原语）。
 *
 * FTL 遍历路径不再打包再拆包，而是直接用 pba_to_cmd（定义于 ftl_fc_interface.h）
 * 由 PBA 填入 FC 命令。
 * ══════════════════════════════════════════════════════════════ */

/* 从 PAA + CH/CE 构造 nand_addr_t（元数据固定位置 / Directory 查找结果用） */
static inline nand_addr_t addr_from_paa(BYTE ch, BYTE ce, DWORD paa)
{
    nand_addr_t addr;

    addr.ch    = ch;
    addr.ce    = ce;
    addr.lun   = paa_to_lun(paa);
    addr.block = paa_to_block(paa);
    addr.plane = paa_to_plane(paa);
    addr.page  = paa_to_page(paa);
    addr.cw    = paa_to_cw(paa);

    return addr;
}

#endif /* NAND_CONFIG_H */

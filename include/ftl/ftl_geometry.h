#ifndef FTL_GEOMETRY_H
#define FTL_GEOMETRY_H

/*
 * ftl_geometry.h — FTL 公共几何 / 拓扑契约（HIL 与 FTL 共用）
 *
 * 本头只放**跨层可见的几何量与拓扑入口**：
 *   - 映射空间：FTL_IU_BYTES / L2P region 分解 / 每实例 CW 总数；
 *   - 地址拓扑：紧凑 die 表、die↔(ch,ce,lun)、LBA→die 路由；
 *   - 编译期几何自检。
 *
 * 它刻意不 include fc_interface.h：HIL 经 ftl_interface.h 看到本头时，不会
 * 顺带拿到 FC 命令/描述符/队列结构。PBA 编码与元数据布局仍属 FTL 私有，
 * 留在 FTL 私有配置 / metadata.h。
 *
 * 依赖方向：本头 → nand_config.h（器件几何）+ media_layout.h（CW 槽布局）。
 * 反向依赖（下级 include 本头）禁止。
 */

#include "assertion.h"    /* ASSERT —— die 分解的参数自检 */
#include "ftl_type.h"     /* DWORD / QWORD / ftl_topo_s */
#include "media_layout.h" /* MEDIA_CW_USER_* —— 映射单元用户区 */
#include "nand_config.h"  /* 几何量 + CH_NUM / CE_NUM / LUN_NUM */

/* 静态容量（数组上限）与运行期实际 die 数（扫描后只读）。
 * FTL_DIE_MAX 只用于数组维度 / 静态断言；DIE_MAX 是运行时调度坐标。 */
#define FTL_DIE_MAX (CH_NUM * CE_NUM * LUN_NUM)
extern BYTE g_die_count;
#define DIE_MAX (g_die_count)

/* ══════════════════════════════════════════════════════════════════
 * 一、几何量（全部由 nand_config.h 推出，不写死）
 * ══════════════════════════════════════════════════════════════════ */

/* 全芯片块数（per-LUN）= 每 plane 块数 × plane 数（当前 2048 × 2 = 4096） */
#define FTL_BLOCK_TOTAL (BLOCKS_PER_PLANE * PLANE_NUM)

/* 映射单元（IU）= 1 个 CW；每条 L2P 条目覆盖该 CW 的**用户数据区**。
 * ⚠ 不是 CW_SIZE(528)：CW 槽 = [META:4][CRC:4][USER:512][PARITY:8]，其中 16B
 *   由 FC 内部管理、不占 LBA 空间。用户区 = CW_SIZE − 2×MEDIA_CW_USER_OFFSET。
 *   （拿 CW_SIZE 当 IU 会算出 1985 region，破坏条目数 ↔ 容量的 1:1。）*/
#define FTL_IU_BYTES (MEDIA_CW_USER_SIZE)

/* 每 region 条目数 = 一页主区 ÷ 每条 4 B（一个 PBA）*/
#define L2P_ENTRIES_PER_REGION (PAGE_MAIN_SIZE / sizeof(DWORD))

/* 每 region 字节数 = 一页主区 */
#define L2P_REGION_SIZE (L2P_ENTRIES_PER_REGION * sizeof(DWORD))

/* 设备维度总数在运行时由扫描决定（DIE_MAX = g_die_count）；
 * 每个实际存在的 die 是一个独立 FTL 实例（分片模型，见 l2p_cache.h 路由）。 */

/* 每 (ch,ce,lun) 实例的 CW 总数 = 块数 × 每块页数 × 每页 CW 数 */
#define FTL_CW_TOTAL (FTL_BLOCK_TOTAL * PAGES_PER_BLOCK * CW_NUM)

/* 每 (ch,ce,lun) 实例的 region 数 = CW 总数 ÷ 每 region 条目数
 * （一条目映射一个 CW ⇒ 一个 region 覆盖 L2P_ENTRIES_PER_REGION 个 CW）
 * ⚠ **每实例**量，不含 × 维度数——每个 die 一份自己的 L2P 表。*/
#define L2P_REGIONS_PER_INSTANCE (FTL_CW_TOTAL / L2P_ENTRIES_PER_REGION)

/* 全设备**映射表寻址空间**（= 可寻址条目总数 = 最大 LBA + 1）
 * —— 每实例条目数 × 设备维度数。
 * ⚠ 这不是主机可见容量：它包含固定元数据块、OP 预留与 Spare。
 *   主机容量见 ftl_capacity.h 的 FTL_LBA_MAX（扣除这些块之后的 437 MiB）。 */
#define FTL_IU_TOTAL ((QWORD)L2P_REGIONS_PER_INSTANCE * L2P_ENTRIES_PER_REGION * DIE_MAX)

/* 映射表寻址容量（物理 CW 覆盖的字节数）——同样不是主机可见容量。
 * 主机可见容量用 ftl_capacity.h 的 FTL_HOST_CAPACITY_BYTES。 */
#define FTL_MAPPING_CAPACITY_BYTES (FTL_IU_TOTAL * FTL_IU_BYTES)

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
_Static_assert(FTL_IU_BYTES + (2U * MEDIA_CW_USER_OFFSET) == CW_SIZE, "CW 用户区与 CW_SIZE 不自洽");

/* 静态容量至少为 1 */
_Static_assert(FTL_DIE_MAX >= 1U, "设备维度数非法");

/* LBA 低位按「2 的幂」做掩码/移位（ftl_lba_page_base 的 ~(CW_NUM-1)、
 * ftl_lba_plane 的 (PLANE_NUM-1)）——几何量不满足时静默算错地址，故编译期钉死。 */
_Static_assert((CW_NUM & (CW_NUM - 1U)) == 0U, "CW_NUM 必须是 2 的幂");
_Static_assert((PLANE_NUM & (PLANE_NUM - 1U)) == 0U, "PLANE_NUM 必须是 2 的幂");

/* cache batch 假设一条 multi-plane 命令覆盖全部 plane；器件必须支持。 */
_Static_assert(PLANE_NUM <= NAND_MAX_MULTI_PLANE, "器件 multi-plane 能力不足：PLANE_NUM > NAND_MAX_MULTI_PLANE");

/* CW 总数必须整除每 region 条目数（否则最后一个 region 不满）*/
_Static_assert((FTL_CW_TOTAL % L2P_ENTRIES_PER_REGION) == 0U, "CW 总数不能被每 region 条目数整除");

/* ══════════════════════════════════════════════════════════════
 * 三、紧凑 die 表（扫描结果的 FTL 调度坐标）
 *
 * 板级 target 扫描只给出“哪些 (ch,ce) target 存在、logical_units
 * 多少”；FTL 据此给实际存在的 LUN 分配紧凑 die 号，并维护两张互为逆映射的表：
 *
 *   g_chcelun_table[ch][ce][lun] → 紧凑 die 号；FTL_DIE_INVALID = 不存在
 *   g_die_table[die]             → ftl_topo_s{ch,ce,lun}；字段全 0xFF = 未分配
 *
 * die 是软件调度坐标；FC/NAND 提交仍用 (ch,ce,lun)，在提交边界由 g_die_table 转换。
 *
 * 编号顺序 = ch 最快、lun 最慢（与 topo_2_die 一致）：满配矩形拓扑下
 * die == topo_2_die(topo)，避免第二套扁平坐标（C9）。稀疏拓扑时 die 紧凑。
 * ══════════════════════════════════════════════════════════════ */

/* BYTE 元素 + 0xFF 哨兵：最多 255 个 die（ID 0..254，255 = 无效）。
 * 该上限对本 MCU 没有实际损失：单个 die 的 BST 已 20B × 4096 = 80 KiB，
 * 255 个 die 仅 BST 就 ~20 MiB，远超 SRAM12 的 256 KiB。 */
#define FTL_DIE_INVALID ((BYTE)0xFFU)
_Static_assert(FTL_DIE_MAX <= FTL_DIE_INVALID, "FTL_DIE_MAX 超出 BYTE 哨兵范围（最多 255 个 die）");

/* g_die_table 未分配尾项的 ch/ce/lun 字段值（BYTE）。
 * 与 FTL_DIE_INVALID 值相同但语义不同（C11）：一个是 die 坐标、一个是物理坐标字段。 */
#define FTL_TOPO_FIELD_INVALID ((BYTE)0xFFU)

extern BYTE       g_chcelun_table[CH_NUM][CE_NUM][LUN_NUM];
extern ftl_topo_s g_die_table[FTL_DIE_MAX];

/* die → 物理坐标：唯一分解点；未分配尾项字段为 0xFF，不得访问。 */

/**
 * @brief die → 物理 ch（唯一分解点之一）
 *
 * @param die 紧凑 die 号（< DIE_MAX；未分配尾项不得访问）
 * @return 物理 ch
 */
static inline BYTE die_2_ch(BYTE die)
{
    ASSERT(die < DIE_MAX);
    return g_die_table[die].ch;
}

/**
 * @brief die → 物理 ce（唯一分解点之一）
 *
 * @param die 紧凑 die 号（< DIE_MAX；未分配尾项不得访问）
 * @return 物理 ce
 */
static inline BYTE die_2_ce(BYTE die)
{
    ASSERT(die < DIE_MAX);
    return g_die_table[die].ce;
}

/**
 * @brief die → 物理 lun（唯一分解点之一）
 *
 * @param die 紧凑 die 号（< DIE_MAX；未分配尾项不得访问）
 * @return 物理 lun
 */
static inline BYTE die_2_lun(BYTE die)
{
    ASSERT(die < DIE_MAX);
    return g_die_table[die].lun;
}

/* ── 全局 LBA / 拓扑路由 helper（HIL 与 FTL 共用）── */
/**
 * @brief (ch,ce,lun) → 紧凑 die 号（查扫描表的唯一漏斗）。
 *
 * @param ch/ce/lun 物理坐标
 * @return 紧凑 die 号（0..DIE_MAX-1）；越界或不存在 ⇒ FTL_DIE_INVALID
 */
static inline BYTE chcelun_2_die(BYTE ch, BYTE ce, BYTE lun)
{
    if ((ch >= CH_NUM) || (ce >= CE_NUM) || (lun >= LUN_NUM))
    {
        return FTL_DIE_INVALID;
    }
    return g_chcelun_table[ch][ce][lun];
}

/**
 * @brief 拓扑 → 紧凑 die 号（查扫描表，不用矩形公式）。
 *
 * @param topo 通道/片选/LUN 三维坐标
 * @return 紧凑 die 号（0..DIE_MAX-1）；不存在 ⇒ FTL_DIE_INVALID
 */
static inline BYTE topo_2_die(ftl_topo_s topo)
{
    return chcelun_2_die(topo.ch, topo.ce, topo.lun);
}

/**
 * @brief 全局 LBA → 全局 region。
 *
 * @param glba 全局 LBA
 * @return 全局 region 序号（glba >> L2P_REGION_SHIFT）
 */
static inline DWORD lba_to_global_region(DWORD glba)
{
    return (glba >> L2P_REGION_SHIFT);
}

/**
 * @brief 全局 LBA → 实例编号（die）路由。
 *
 * 路由键 = **region 序号**（lba >> L2P_REGION_SHIFT），不是页号也不是条目号：
 * 一个 region（512 个 CW）整体落在同一个 die 上，region 的 flush 只碰一个实例。
 * 再把 region 序号按设备维度**分解**：
 * die = region % DIE_MAX
 * ch/ce/lun = g_die_table[die]
 * DIE_MAX = 1 时恒为 (0,0,0)。
 *
 * @param lba 全局 LBA
 * @return 紧凑 die 号（region % DIE_MAX）
 */
static inline BYTE lba_2_die(QWORD lba)
{
    ASSERT(DIE_MAX != 0U);
    return (BYTE)(((lba >> L2P_REGION_SHIFT) % DIE_MAX));
}

/**
 * @brief 全局 LBA → 通道号。
 *
 * @param lba 全局 LBA
 * @return 通道号
 */
static inline BYTE lba_2_ch(QWORD lba)
{
    return die_2_ch(lba_2_die(lba));
}

/**
 * @brief 全局 LBA → 片选号。
 *
 * @param lba 全局 LBA
 * @return 片选号
 */
static inline BYTE lba_2_ce(QWORD lba)
{
    return die_2_ce(lba_2_die(lba));
}

/**
 * @brief 全局 LBA → LUN 号。
 *
 * @param lba 全局 LBA
 * @return LUN 号
 */
static inline BYTE lba_2_lun(QWORD lba)
{
    return die_2_lun(lba_2_die(lba));
}

#endif /* FTL_GEOMETRY_H */

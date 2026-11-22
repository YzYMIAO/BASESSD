#ifndef FTL_CAPACITY_H
#define FTL_CAPACITY_H

/*
 * ftl_capacity.h — FTL 容量与固定元数据区布局的公共派生量
 *
 * 这些量由 nand_config / ftl_geometry 的几何推导；集中在本头，避免
 * controller / usb / 诊断代码为了容量宏直接 include metadata / allocator 内部头。
 */

#include "ftl_geometry.h"

/* ── 固定元数据区块布局（per-plane 块号）── */
#define FTL_METADATA_BASE_BLOCK (0U)
#define DIR_CKPT_POOL_BLOCKS    (32U)
#define CHECKPOINT_POOL_BLOCKS  (8U)
#define BBT_POOL_BLOCKS         (2U)
#define JOURNAL_BLOCKS          (CW_NUM)
#define JOURNAL_BLOCK_COUNT     (JOURNAL_BLOCKS)

#define DIR_CKPT_BLOCK0        (FTL_METADATA_BASE_BLOCK)
#define CHECKPOINT_BLOCK0      (DIR_CKPT_BLOCK0 + DIR_CKPT_POOL_BLOCKS)
#define BBT_BLOCK              (CHECKPOINT_BLOCK0 + CHECKPOINT_POOL_BLOCKS)
#define JOURNAL_BLOCK0         (BBT_BLOCK + BBT_POOL_BLOCKS)
#define FTL_METADATA_BLOCK_END (JOURNAL_BLOCK0 + JOURNAL_BLOCK_COUNT)

/* L2P region 日志区（固定预留）。 */
#define L2P_LOG_IDEAL_BLOCKS     ((L2P_REGIONS_PER_INSTANCE + PAGES_PER_BLOCK - 1U) / PAGES_PER_BLOCK)
#define L2P_REGION_COPIES        ((DWORD)(PLANE_NUM))
#define L2P_LOG_BLOCKS_TOTAL     (3U * L2P_LOG_IDEAL_BLOCKS)
#define L2P_LOG_BLOCKS_PER_PLANE (((L2P_LOG_BLOCKS_TOTAL + PLANE_NUM - 1U) / PLANE_NUM))
#define L2P_LOG_BLOCK0           (FTL_METADATA_BLOCK_END)
#define L2P_LOG_AREA_END         (L2P_LOG_BLOCK0 + L2P_LOG_BLOCKS_PER_PLANE)

/* flush_pba[] 的副本位置 = 承载 plane。当前 Directory 格式只存主 + 备两个 PBA，
 * metadata.h 用断言把副本数钉在 2；改 PLANE_NUM 必须同步扩展格式。 */
#define L2P_COPY_PRIMARY (0U)
#define L2P_COPY_BACKUP  (1U)

/* 共享 metadata spare（仅补固定元数据分区的 BAD 缺额）。 */
#define MD_SPARE_BLOCK0           (L2P_LOG_AREA_END)
#define MD_SPARE_BLOCKS_PER_PLANE (4U)
#define FTL_METADATA_AREA_END     (MD_SPARE_BLOCK0 + MD_SPARE_BLOCKS_PER_PLANE)

/* ── OP / Spare 保留池 ── */
#define FTL_OP_PERCENT           (10U)
#define FTL_OP_BLOCKS_PER_PLANE  ((BLOCKS_PER_PLANE * FTL_OP_PERCENT) / 100U)
#define FTL_OP_POOL_CAP          ((FTL_OP_BLOCKS_PER_PLANE) > 0U ? (FTL_OP_BLOCKS_PER_PLANE) : 1U)
#define FTL_SPARE_POOL_PER_PLANE (50U)

/* ── 用户容量 ── */
#define FTL_HOST_DATA_BLOCKS_PER_INSTANCE (FTL_BLOCK_TOTAL - ((FTL_METADATA_AREA_END + FTL_OP_BLOCKS_PER_PLANE + FTL_SPARE_POOL_PER_PLANE) * PLANE_NUM))

/* 运行时 L2P 映射空间上限（不扣除实际坏块）。 */
#define FTL_LBA_MAX ((QWORD)FTL_HOST_DATA_BLOCKS_PER_INSTANCE * PAGES_PER_BLOCK * CW_NUM * DIE_MAX)

/* 静态上限版本（仅编译期断言）。 */
#define FTL_LBA_MAX_AT_MAX ((QWORD)FTL_HOST_DATA_BLOCKS_PER_INSTANCE * PAGES_PER_BLOCK * CW_NUM * FTL_DIE_MAX)

/* 主机可见容量（字节，编译期上界）。 */
#define FTL_HOST_CAPACITY_BYTES ((QWORD)FTL_LBA_MAX * FTL_IU_BYTES)

#endif /* FTL_CAPACITY_H */

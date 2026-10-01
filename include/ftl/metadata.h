#ifndef FTL_METADATA_H
#define FTL_METADATA_H

/*
 * Metadata Directory — 动态元数据位置追踪
 *
 * 元数据和用户数据共享 Free Pool，通过 Metadata Directory 追踪每份元数据的
 * 当前物理位置。Directory SRAM 常驻（~1.4KB），启动时从 Anchor Block 加载。
 *
 * Anchor Block（固定位置 Block 33-34，出厂好块预留）：
 *   只存 Metadata Directory，每份冗余 3 页（Page 0/1/2）
 *   Magic + Sequence + EntryCount + Entries + CRC
 *
 * 元数据（从 Free Pool 动态分配）:
 *   L2P Region       — 任意物理 Block 中（通过 Directory 查找）
 *   Journal          — Block 40-41（固定，不改动）
 *   Checkpoint/BBT   — 后续扩展
 */

#include "ftl/allocator.h"
#include "ftl/ftl_type.h"
#include "nand/nand_config.h"

#include <stdint.h>

/* ── Anchor Block 位置（每 (ch,ce,lun) 独立一套，按 lun 错开 stride）── */
#define ANCHOR_BLOCK0         (33U) /* Anchor Block 0（LUN 0） */
#define ANCHOR_BLOCK1         (34U) /* Anchor Block 1（LUN 0） */
#define ANCHOR_BLOCK_STRIDE   (2U)  /* 每 LUN Anchor 占 2 Block，跨 LUN 错开 */
#define ANCHOR_REDUNDANCY     (3U)  /* 每 Block 冗余页数 */
#define ANCHOR_PAGES          (6U)  /* 共 2 Block × 3 页 = 6 */
#define ANCHOR_PAGE_PER_BLOCK (3U)  /* 每 Block 3 页 */

/* ── 元数据类型 ── */
#define METADATA_TYPE_L2P_REGION  (0U) /* L2P Region 数据 */
#define METADATA_TYPE_JOURNAL     (1U) /* Journal Block */
#define METADATA_TYPE_CHECKPOINT  (2U) /* Checkpoint */
#define METADATA_TYPE_BBT         (3U) /* Bad Block Table */
#define METADATA_TYPE_ERASE_COUNT (4U) /* 擦除计数表 */
#define METADATA_TYPE_COUNT       (5U)

/* ── Directory 容量 ── */
#define METADATA_DIR_ENTRIES (128U)

/* ── 序列号锚定 ── */
#define ANCHOR_MAGIC (0x524F444EUL) /* "ANDIR" */

/* ── Entry: 7 字节（packed） ── */
typedef struct
{
    uint8_t    type;                        /* METADATA_TYPE_* */
    uint16_t   id;                          /* L2P region_id / 0 for others */
    nand_paa_t paa;                         /* 物理 NAND 位置 */
} __attribute__((packed)) metadata_entry_t; /* 7 bytes */

/* ── NAND 上持久化格式（~904B < 2048B = 1 page） ── */
typedef struct
{
    uint32_t         magic;                               /* ANCHOR_MAGIC */
    uint32_t         sequence;                            /* 单调递增 */
    uint32_t         entry_count;                         /* 有效 entry 数 */
    metadata_entry_t entries[METADATA_DIR_ENTRIES];       /* 128 × 7 = 896B */
    uint32_t         crc;                                 /* 校验（覆盖前面全部） */
    uint8_t          padding[2048 - 4 - 4 - 4 - 896 - 4]; /* 填满 2048B */
} metadata_directory_t;

/* ── SRAM 常驻 Directory Cache（~1.4KB） ── */
typedef struct
{
    metadata_entry_t entries[METADATA_DIR_ENTRIES];
    uint16_t         count;           /* 有效 entry 数 */
    uint32_t         anchor_sequence; /* 当前 Anchor sequence */
    uint8_t          dirty;           /* 是否有未写回 Anchor 的变更 */
    uint8_t          change_count;    /* 自上次 Anchor flush 后的变更数 */

    /* L2P Region 块分配跟踪：当前正在写入的 metadata block 及下一可用页 */
    uint16_t cur_metadata_block; /* 当前分配的 metadata block */
    uint8_t  cur_metadata_page;  /* 下一可用 page */
    uint8_t  cur_metadata_plane; /* 当前 plane */

    /* 拓扑信息（Anchor Block 所在芯片路由） */
    ftl_topology_t topo;
} metadata_dir_cache_t;

/* ── API ── */

/*
 * metadata_dir_init - 初始化 Metadata Directory
 *
 * 首次使用时建立固定的 L2P Region 映射
 * （Region r → Block 0 + r/64, Page r%64, Plane r&1）
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
 * @return PAA，0xFFFFFFFF 表示未找到
 */
nand_paa_t metadata_dir_lookup(const metadata_dir_cache_t *dir,
                               uint8_t                     type,
                               uint16_t                    id);

/*
 * metadata_dir_update - 添加或更新元数据位置
 *
 * @param dir    Directory cache
 * @param type   元数据类型
 * @param id     L2P region_id / 0 for others
 * @param new_paa 新物理位置
 */
void metadata_dir_update(metadata_dir_cache_t *dir,
                         uint8_t               type,
                         uint16_t              id,
                         nand_paa_t            new_paa);

/*
 * metadata_dir_alloc_region - 为 L2P Region 分配新物理位置
 *
 * 从 Free Pool 取一个好块，更新 Directory。
 * 偶数 region_id → Plane 0，奇数 → Plane 1（双 Plane 交错）。
 *
 * @param dir       Directory cache
 * @param alloc     Allocator（用于取 Free Pool block）
 * @param region_id L2P region_id
 * @return 分配的 PAA，0xFFFFFFFF 失败
 */
nand_paa_t metadata_dir_alloc_region(metadata_dir_cache_t *dir,
                                     ftl_allocator_t      *alloc,
                                     uint16_t              region_id);

/*
 * metadata_dir_flush_to_anchor - 将 Directory 写回 Anchor Block
 *
 * 变更 ≥ 64 次或 Checkpoint/ftl_service(IDLE) 时调用。
 * 同步等待写入完成。
 *
 * @param dir  Directory cache
 * @return 0 成功，-1 失败
 */
ftl_status_t metadata_dir_flush_to_anchor(metadata_dir_cache_t *dir);

/*
 * metadata_dir_load - 从 Anchor Block 加载 Directory
 *
 * 扫描 2 Block × 3 页 = 6 页，选 magic 正确 + CRC 正确 + sequence 最大的。
 *
 * @param dir  Directory cache（输出）
 * @return 0 成功，-1 无有效 Anchor（需首次初始化）
 */
ftl_status_t metadata_dir_load(metadata_dir_cache_t *dir);

/*
 * metadata_dir_anchor_paa - 返回第 n 个 Anchor 页的 PAA（不含 lun）
 *
 * 固定块/页/plane 由 idx 推导；lun 由调用侧（anchor_page_addr / 各模块 topo）带出。
 *
 * @param idx 0..5
 * @return PAA（lun 为 0，调用方用 paa_with_lun 补 lun）
 */
nand_paa_t metadata_dir_anchor_paa(uint8_t idx);

#endif /* FTL_METADATA_H */
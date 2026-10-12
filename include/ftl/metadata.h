#ifndef FTL_METADATA_H
#define FTL_METADATA_H

/*
 * Metadata Directory — 动态元数据位置追踪
 *
 * 元数据和用户数据共享 Free Pool，通过 Metadata Directory 追踪每份元数据的
 * 当前物理位置。Directory SRAM 常驻（14.4KB = 2048 条 × 7B），启动时从 Anchor Block 加载。
 *
 * Anchor Block（固定元数据区内的 Anchor 块，出厂好块预留）：
 *   只存 Metadata Directory。一份 Directory 有 ANCHOR_PAGE_COUNT 页（chunk），
 *   每页自包含 Magic + Sequence + ChunkIndex + EntryCount + Entries + CRC；
 *   主备 2 块 × 每块 3 份冗余 = 6 份，任一份可独立重建整个 Directory。
 *
 * 元数据（从 Free Pool 动态分配）:
 *   L2P Region       — 固定表区 + 任意物理 Block（通过 Directory 查找）
 *   Journal          — 固定元数据区（块位置见 ftl/ftl_config.h）
 *   Checkpoint/BBT   — 固定元数据区（见 ftl/ftl_config.h）
 */

#include "ftl/allocator.h"
#include "ftl/ftl_config.h" /* ANCHOR_* 块位置 + 固定元数据区边界 */
#include "ftl/ftl_type.h"
#include "nand/nand_config.h"

#include <stdint.h>

/* ── Anchor Block 位置与页几何（ANCHOR_BLOCK0/1、ANCHOR_COPIES、ANCHOR_REDUNDANCY、
 *    ANCHOR_ENTRIES_PER_PAGE、ANCHOR_PAGE_COUNT、ANCHOR_PAGE_PER_BLOCK）见 ftl/ftl_config.h ── */

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
    nand_paa_t paa;                         /* 物理 NAND 位置 */
} __attribute__((packed)) metadata_entry_t; /* 7 bytes */

/* ── NAND 上持久化格式：**每页自包含** ──
 * Directory 有 METADATA_DIR_ENTRIES 条（14KB）⇒ 一份跨 ANCHOR_PAGE_COUNT 页，
 * 每页装 METADATA_DIR_ENTRIES_PER_PAGE 条，各自带 magic/sequence/CRC。
 * 单页损坏只丢该页（其余页仍可用），且读写只需一个 2KB 缓冲。 */
typedef struct
{
    uint32_t         magic;                                                                     /* ANCHOR_MAGIC */
    uint32_t         sequence;                                                                  /* 单调递增（同一份的各 chunk 相同）*/
    uint16_t         chunk_index;                                                               /* 0..ANCHOR_PAGE_COUNT-1 */
    uint16_t         chunk_count;                                                               /* = ANCHOR_PAGE_COUNT */
    uint32_t         entry_count;                                                               /* 本 chunk 有效 entry 数 */
    metadata_entry_t entries[METADATA_DIR_ENTRIES_PER_PAGE];                                    /* 256 × 7 = 1792B */
    uint32_t         crc;                                                                       /* 校验（覆盖本页 crc 之前全部）*/
    uint8_t          padding[PAGE_MAIN_SIZE - 16U - (METADATA_DIR_ENTRIES_PER_PAGE * 7U) - 4U]; /* 填满一页主区 */
} metadata_anchor_page_t;

/* 一页必须装得下：头 16B + 每页条目 + CRC 4B */
_Static_assert(sizeof(metadata_anchor_page_t) == PAGE_MAIN_SIZE, "Anchor 页结构体不等于一页主区");
_Static_assert((METADATA_DIR_ENTRIES % METADATA_DIR_ENTRIES_PER_PAGE) == 0U, "每页条目数不能整除总条目数");

/* ── SRAM 常驻 Directory Cache（14.4KB，随 ftl_context_t 落 AXI_RAM） ── */
typedef struct
{
    metadata_entry_t entries[METADATA_DIR_ENTRIES];
    uint16_t         count;           /* 有效 entry 数 */
    uint32_t         anchor_sequence; /* 当前 Anchor sequence */
    uint8_t          dirty;           /* 是否有未写回 Anchor 的变更 */
    uint8_t          change_count;    /* 自上次 Anchor flush 后的变更数 */

    /* 是否已有 region 落到动态块（block >= FTL_METADATA_BLOCK_END）。
     * entries 现在有 2048 条，而 ftl_is_metadata_block() 在开机回填里按块调用
     * 数千次 ⇒ 无条件线性扫全表会明显拖慢开机。为 0 时直接跳过动态扫描。 */
    uint8_t have_dynamic;

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
 * （打包放置：Block/Page/Plane 由 ftl_l2p_region_placement() 给出，见 ftl_config.h）
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
 * **按页写**：ANCHOR_COPIES 块 × ANCHOR_REDUNDANCY 份 × ANCHOR_PAGE_COUNT 页，
 * 每页在一个 2KB 栈缓冲里组好后同步写入（勿在栈上放 14KB 的整份 Directory）。
 *
 * @param dir  Directory cache
 * @return 0 成功，-1 失败
 */
ftl_status_t metadata_dir_flush_to_anchor(metadata_dir_cache_t *dir);

/*
 * metadata_dir_load - 从 Anchor Block 加载 Directory
 *
 * ① 先读每份的 chunk 0 页（ANCHOR_COPIES × ANCHOR_REDUNDANCY 份），校验
 *    magic + CRC，取 sequence 最大的那份；
 * ② 再读胜者剩余的 ANCHOR_PAGE_COUNT 页，逐页校验 CRC 后装配。
 *
 * @param dir  Directory cache（输出）
 * @return 0 成功，-1 无有效 Anchor（需首次初始化）
 */
ftl_status_t metadata_dir_load(metadata_dir_cache_t *dir);

/*
 * metadata_dir_anchor_paa - 返回第 (份, chunk) 个 Anchor 页的 PAA（不含 lun）
 *
 * idx = copy_index × ANCHOR_PAGE_COUNT + chunk_index，
 * copy_index ∈ [0, ANCHOR_COPIES × ANCHOR_REDUNDANCY)（前 ANCHOR_REDUNDANCY 份在
 * Block0、其余在 Block1），chunk_index ∈ [0, ANCHOR_PAGE_COUNT)。
 * 固定块/页由 idx 推导；lun 由调用侧（anchor_page_addr / 各模块 topo）带出。
 *
 * @param idx 0..(ANCHOR_COPIES × ANCHOR_REDUNDANCY × ANCHOR_PAGE_COUNT)-1
 * @return PAA（lun 为 0，调用方用 paa_with_lun 补 lun）
 */
nand_paa_t metadata_dir_anchor_paa(uint8_t idx);

/*
 * ftl_is_metadata_block - 判断物理 Block 是否属于固定/动态元数据区
 *
 * 掉电恢复重建 closed_sb_list 时，L2P Table（固定表区 + 动态 region）、
 * Anchor、BBT、Checkpoint、Journal、Wear 等元数据块可能写满全部页（尤其
 * Journal 环形回切），逐页判空会把它们误判为满 Super Block。本谓词统一排除：
 *
 *   固定块：遍历 g_ftl_partitions[]（单一事实来源，见 ftl/ftl_config.h），
 *           按 first_block/block_count 判区间（各 die 同一份块号）
 *   动态块：Metadata Directory 中登记的 L2P region 块（扫 entries）
 *
 * free pool 回填（main.c）与重建（ftl_rebuild_closed_sb_list）共用同一判据。
 *
 * @param md_dir Metadata Directory cache（可为 NULL：仅固定块区间判断）
 * @param block  物理 Block 号（per-plane 号，0..BLOCKS_PER_PLANE-1）
 * @return 1 = 元数据块，0 = 用户数据块
 */
int ftl_is_metadata_block(const metadata_dir_cache_t *md_dir, uint16_t block);

#endif /* FTL_METADATA_H */
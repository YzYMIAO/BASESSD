#ifndef FTL_BBT_H
#define FTL_BBT_H

/*
 * BBT（Bad Block Table）— 坏块表持久化
 *
 * 三个阶段：
 *   1. 出厂扫描：遍历所有 Block，读第 0 页判断坏块标记
 *   2. 运行时隔离：PROGRAM/ERASE 失败 → BBT 标记 + allocator 跳过
 *   3. 替换机制：Block Sparing — 从 Spare Pool 取好块补充 Free Pool
 *
 * BBT 存储在固定元数据区的 BBT 池（`BBT_POOL_BLOCKS` 块轮转，
 * **一条 record = 1 页**，块满换池内下一块并先擦）——池首块/块数由
 * ftl/metadata.h 按几何推导（「固定元数据区布局」）。
 * bad_map 覆盖全部 FTL_BLOCK_TOTAL 个 Block（NAND 持久化格式，保持不变）。
 * RAM 运行时坏块位统一存于 bst_block_s.state（Block State Table 单数据源），
 * bbt_store 时从 BST 重建 bad_map。
 *
 * 写模型与目录检查点 / Checkpoint 一致（见 ftl/metadata.h §元数据写模型）：
 * **只追加、环形轮转、最新有效胜**。对同一份 record 就地重写会在第二个坏块出现时
 * 损坏 BBT ⇒ bbt_load 失败 ⇒ 开机不知道哪些块坏 ⇒ 用到坏块丢数据。
 */

#include "allocator.h"
#include "assertion.h"
#include "metadata.h" /* BBT_BLOCK / BBT_POOL_BLOCKS / BBT_MAX_BLOCKS / BBT_BITMAP_SIZE */

#include <stddef.h>
#include <stdint.h>

/* ── BBT 尺寸与块位置（BBT_BLOCK / BBT_POOL_BLOCKS / BBT_MAX_BLOCKS /
 *    BBT_BITMAP_SIZE）见 ftl/metadata.h「固定元数据区布局」── */

/* ── BBT Header 格式 ── */
#define BBT_MAGIC (0x424254ABUL) /* "BBT\xAB" */

/* reserved[0] 的格式进行中标志：两阶段格式化在擦除用户块之前先落 BBT 时置位，
 * 擦完并写完 CP 标记后清除。旧固件写 0（不进行中），语义兼容。 */
#define BBT_FLAG_FORMAT_IN_PROGRESS (1UL)

/* reserved[1] 的「下次开机必须 full metadata rebuild」标志（本仓自拟）。
 * Journal / L2P 日志块运行期变坏时置位：丢失的映射无法在当前上电周期内恢复，
 * 只能靠全量重建数据页 META 兜底。成功重建后清除。 */
#define BBT_FLAG_REBUILD_REQUIRED (1UL)

typedef struct BBT_HEADER_S
{
    DWORD magic;                        /* BBT_MAGIC */
    DWORD sequence;                     /* 单调递增，最新有效 */
    DWORD total_blocks;                 /* 总 Block 数 */
    DWORD bad_count;                    /* 坏块数 */
    DWORD reserved[3];                  /* [0] = FORMAT_IN_PROGRESS；[1] = REBUILD_REQUIRED；其余写 0 */
    BYTE  bad_map[BBT_BITMAP_SIZE];     /* 4096 bit = 512B */
} __attribute__((packed)) bbt_header_s; /* 4+4+4+4+12+512 = 540B（无独立 CRC，页完整性由 FC 保证）*/

_Static_assert(sizeof(bbt_header_s) == 540U, "BBT header 布局漂移（期望 540B）");
_Static_assert(sizeof(bbt_header_s) <= PAGE_MAIN_SIZE, "BBT header 必须装得进一页主区");

/* ── bitmap / 候选块原语 ── */

/**
 * @brief 查询 Block 是否坏块
 *
 * @param bad_map  BBT bitmap
 * @param block    Block 编号
 * @return true 坏块，false 好块
 */
static inline BOOL bbt_is_bad(const BYTE *bad_map, WORD block)
{
    ASSERT(block < BBT_MAX_BLOCKS);

    if (block >= BBT_MAX_BLOCKS)
    {
        return true; /* 越界是编程错误：release 下保守判坏，避免把非法块当好块使用 */
    }
    if (bad_map == NULL)
    {
        return false;
    }
    return ((bad_map[block >> 3] >> (block & 7U)) & 1U) != 0U;
}

/**
 * @brief 在 bitmap 中将 Block 标记为坏块
 *
 * @param bad_map  BBT bitmap
 * @param block    Block 编号
 */
static inline void bbt_set_bad(BYTE *bad_map, WORD block)
{
    if ((bad_map != NULL) && (block < BBT_MAX_BLOCKS))
    {
        bad_map[block >> 3] |= (1U << (block & 7U));
    }
}

/**
 * @brief 该物理块是否属于当前 BBT 候选池
 *
 * BBT record 的候选池优先来自 `pools->bbt` 的实时候选表（可能含
 * shared metadata spare）；ring 未建立或不足 2 块时才退回名义区间
 * `[BBT_BLOCK, BBT_BLOCK + BBT_POOL_BLOCKS)`。
 *
 * 纯函数；格式化/重建路径按块调用，放头文件供 bbt.c 与 PFR 共用，避免三份手写
 * 条件在后续修改时漂移。
 *
 * @param ring  BBT 池轮转描述（未建立或不足 2 块时可为 NULL，退回名义区间）
 * @param block 物理块号（plane 内块号）
 * @return true 属于候选池，false 不属于
 */
static inline BOOL bbt_block_is_candidate(const md_ring_s *ring, WORD block)
{
    if ((ring != NULL) && (ring->block_count >= 2U))
    {
        return md_ring_has_block(ring, block);
    }

    return (block >= BBT_BLOCK) && (block < (BBT_BLOCK + BBT_POOL_BLOCKS));
}

/* ── 查询 / 诊断 ── */

/**
 * @brief 介质固定元数据区里是否已有合法的 FTL 页。
 *
 * @note 调用者应先用 raw 读确认命令通路正常，否则「总线读死」也会返回 false。
 */
BOOL bbt_medium_has_ftl_data(alc_s *alloc);

/* ── 运行时坏块 ── */

/**
 * @brief 运行时标记坏块（幂等，BAD 终态）。
 *
 * @note 只置 dirty，写回 NAND 由后台 bbt_handle 攒批执行。
 */
ftl_status_e bbt_mark_bad(alc_s *alloc, WORD gblock);

/**
 * @brief 标记 BBT 有未落盘变更（唯一 dirty 闸门）。
 *
 * @note 每次置位推进 `dirty_gen`；写 job 用它检测「写飞行期间又标坏」的丢更新。
 */
void bbt_dirty_note(alc_s *alloc);

/**
 * @brief 从指定 plane 的 Spare Pool 补充 Free Pool。
 *
 * @note 必须按 plane 取：坏块属于哪个 plane 就补哪个 plane 的块；补错 plane
 *       不恢复该 plane 的配对能力。
 */
ftl_status_e bbt_spare_refill(alc_s *alloc, BYTE plane);

/* ── 持久化 / 格式化 ── */

/** @brief 将 BBT 追加写入 NAND BBT 池（同步）。 */
ftl_status_e bbt_store(alc_s *alloc);

/**
 * @brief 格式化用的两阶段擦除：先落 BBT，再擦 BBT 池之外的块。
 *
 * @note 必须在 bbt_init 之后调用（出厂坏块标记的读取职责唯一归 bbt_init）。
 */
ftl_status_e bbt_format_erase(alc_s *alloc);

/* ── 诊断 ── */

/**
 * @brief 逐块探测并打印坏块图（诊断用）。
 *
 * @note 默认只读；do_erase == true 时额外擦除本实例每个 plane 的每一个块
 *       （含元数据区）⇒ 抹掉该实例全部 FTL 元数据与用户数据，绝不可放进开机路径。
 */
void bbt_print_bad_map(alc_s *alloc, BOOL do_erase);

/* ── 入口 ── */

/**
 * @brief BBT 初始化：从 BBT 池加载，坏块位写入 BST。
 *
 * @note 无有效 BBT 时只在空片执行出厂扫描；已用介质拒绝重扫（原因见 bbt.c）。
 */
void bbt_init(alc_s *alloc);

/**
 * @brief BBT 后台持久化入口（typed handle；提交即返回）。
 *
 * @note 单飞行：命令在飞 ⇒ 空操作；提交被背压拒时只续提交写，不重走换块判定。
 * @note 只由后台调度器调用；开机 / 格式化路径走同步 bbt_store。
 */
void bbt_handle(alc_s *alloc);

#endif /* FTL_BBT_H */
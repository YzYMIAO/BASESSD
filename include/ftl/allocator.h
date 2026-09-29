#ifndef FTL_ALLOCATOR_H
#define FTL_ALLOCATOR_H

/*
 * FTL Allocator — Block 分配器
 *
 * 职责：
 *   1. 维护三级 Free Pool（Pre-conditioned / Normal / Emergency）
 *   2. 从 Free Pool 取 Block 配对为 Super Block（跨双 Plane）
 *   3. 跟踪每个温度（Hot/Cold）的 Open Super Block 写指针
 *   4. 写满时关闭 Super Block 并加入 Closed 列表
 *   5. 磨损均衡：分配时选擦除次数最少的 Block
 *   6. 跳过坏块，通过 Spare Pool 保水位
 *
 * 地址体系：
 *   allocator_alloc() 返回 nand_pba_t（含 CH/CE 拓扑 + PAA 芯片内地址）。
 *   PBA 的 page 字段由 allocator 管理，block/plane 从 Free Pool 分配。
 */

#include "ftl/ftl_type.h"   /* ftl_status_t */
#include "nand/nand_config.h" /* nand_pba_t, nand_paa_t, PLANE_NUM, PAGES_PER_BLOCK, BLOCKS_PER_PLANE */

#include <stdint.h>

/* ── 温度等级 ── */
#define FTL_TEMP_HOT  0U /* 用户 IO 数据 */
#define FTL_TEMP_COLD 1U /* GC 搬移 / 元数据 */
#define FTL_TEMP_NUM  2U /* 温度等级数 */

/* ── 三级 Free Pool 水位 ── */
#define FREE_POOL_TARGET    (32U) /* Pre-conditioned 目标水位 */
#define FREE_POOL_EMERGENCY (8U)  /* 紧急水位 */
#define FTL_OP_PERCENT      (10U) /* OP 预留百分比（编译期默认） */
#define FTL_SPARE_POOL_SIZE (50U) /* 坏块替换池大小 */

/* ── Closed Super Block 列表最大容量 ── */
#define SUPER_BLOCK_LIST_MAX (1024U)

/* ── Super Block ──
 *
 * Super Block 是跨双 Plane 的逻辑写入单元。
 * 双 Plane 各取一个物理 Block 配对，Multi-Plane PROGRAM 同步写入。
 * 两个温度（Hot/Cold）各有一个 Open Super Block。
 * 写满后从 Open → Closed，等待 GC 回收。
 */

typedef struct
{
    uint16_t plane_blocks[PLANE_NUM]; /* Plane 0 和 Plane 1 的 Block 号 */
    uint8_t  next_page;               /* 当前写入页（0~63，双 Plane 同步） */
    uint8_t  temperature;             /* FTL_TEMP_HOT / FTL_TEMP_COLD */
    uint8_t  valid;                   /* 1=正在使用（Open），0=已关闭 */
} super_block_t;

/* 返回 Super Block 中指定 Plane/Page 的 PBA（用户数据使用 FTL_DATA_CH/CE） */
static inline nand_pba_t super_block_page_pba(uint8_t ch, uint8_t ce, const super_block_t *sb, uint8_t plane, uint8_t page)
{
    return pba_make(ch, ce, page, plane, sb->plane_blocks[plane], 0, 0);
}

/* ── Allocator 上下文 ──
 *
 * 所有大数组由调用者提供存储（指针模式），当前放在 SRAM。
 */

typedef struct
{
    /* ── 拓扑参数 ── */
    uint8_t  ch;           /* 本 Allocator 绑定的 CH */
    uint8_t  ce;           /* 本 Allocator 绑定的 CE */
    uint16_t total_blocks; /* 总好 Block 数（出厂扫描后确定） */

    /* ── Free Pool（三级） ── */
    uint16_t *free_blocks;     /* free block 编号数组（由调用者提供） */
    uint16_t  free_count;      /* 空闲 block 数 */
    uint16_t  pre_cond_count;  /* 预擦除池 Block 数 */
    uint16_t  emergency_count; /* 紧急池 Block 数 */

    /* ── Open Super Block（每温度一个） ── */
    super_block_t hot_sb;  /* Hot Open Super Block */
    super_block_t cold_sb; /* Cold Open Super Block */

    /* ── Closed Super Block 列表（等待 GC） ── */
    super_block_t *closed_sb_list;  /* 已写满的 Super Block 数组（由调用者提供） */
    uint16_t       closed_sb_count; /* 已关闭 Super Block 数 */

    /* ── 磨损信息 ── */
    uint32_t *erase_counts; /* 每 Block 擦除次数数组（由调用者提供） */

    /* ── 坏块 ── */
    uint16_t *bad_blocks;       /* 坏 Block 编号数组（由调用者提供） */
    uint16_t  bad_count;        /* 坏 Block 数 */
    uint16_t  spare_pool_count; /* Spare Pool 剩余 Block 数 */
} ftl_allocator_t;

/* ── 接口函数 ── */

/*
 * allocator_init - 初始化 Allocator
 *
 * @param alloc      Allocator 上下文（由调用者提供存储）
 * @param ch         Allocator 绑定的 CH
 * @param ce         Allocator 绑定的 CE
 * @param free_blocks     free block 数组（由调用者提供）
 * @param free_block_count free block 数组大小
 * @param bad_blocks      bad block 数组（由调用者提供）
 * @param erase_counts    擦除次数数组（由调用者提供）
 * @param closed_sb_list  closed Super Block 数组（由调用者提供）
 *
 * @return ftl_status_t — FTL_OK 成功
 */
ftl_status_t allocator_init(ftl_allocator_t *alloc,
                            uint8_t          ch,
                            uint8_t          ce,
                            uint16_t        *free_blocks,
                            uint16_t         free_block_count,
                            uint16_t        *bad_blocks,
                            uint32_t        *erase_counts,
                            super_block_t   *closed_sb_list);

/*
 * allocator_alloc - 分配一个 PBA（一页，4 CW）
 *
 * 内部：
 *   ① 从对应温度的 Open Super Block 取当前写入位置
 *   ② Super Block 已写满 → 关闭它（→ Closed），从 Free Pool 分配新的 Super Block
 *   ③ 返回当前页的 PBA，next_page++
 *   ④ Free Pool 耗尽 → 返回 FTL_PBA_INVALID（#warning 待 GC 实现）
 *
 * @param alloc       Allocator 上下文
 * @param temperature FTL_TEMP_HOT / FTL_TEMP_COLD
 *
 * @return nand_pba_t，NAND_PBA_INVALID 表示分配失败
 */
nand_pba_t allocator_alloc(ftl_allocator_t *alloc, uint8_t temperature);

/*
 * allocator_free_block - GC 擦除后将 Block 归还到 Free Pool
 *
 * Step 3：桩，仅 #warning 和增加计数。
 *
 * @param alloc Allocator 上下文
 * @param block 要归还的 Block 编号
 */
void allocator_free_block(ftl_allocator_t *alloc, uint16_t block);

/*
 * allocator_mark_bad - 运行时标记坏块
 *
 * Step 3：桩，仅 #warning。
 *
 * @param alloc Allocator 上下文
 * @param block 坏 Block 编号
 */
void allocator_mark_bad(ftl_allocator_t *alloc, uint16_t block);

/*
 * allocator_needs_gc - 检查是否需要触发 GC
 *
 * @param alloc Allocator 上下文
 * @return 1 需要 GC，0 不需要
 */
int allocator_needs_gc(const ftl_allocator_t *alloc);

#endif /* FTL_ALLOCATOR_H */
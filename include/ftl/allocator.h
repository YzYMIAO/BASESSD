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

#include "ftl/bst.h"         /* ftl_block_t, FTL_BLOCK_TOTAL, ftl_gblock, ftl_block_is_bad */
#include "ftl/ftl_type.h"   /* ftl_status_t, ftl_topology_t */
#include "nand/nand_config.h" /* nand_pba_t, nand_addr_t, nand_paa_t, PLANE_NUM, PAGES_PER_BLOCK, BLOCKS_PER_PLANE */

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
    ftl_topology_t topo;                  /* 本 Super Block 绑定的 NAND 芯片路由（ch/ce/lun） */
    uint16_t plane_blocks[PLANE_NUM];     /* Plane 0 和 Plane 1 的 Block 号 */
    uint8_t  next_page;                   /* 当前写入页（0~63，双 Plane 同步） */
    uint8_t  temperature;                 /* FTL_TEMP_HOT / FTL_TEMP_COLD */
    uint8_t  valid;                       /* 1=正在使用（Open），0=已关闭 */
} super_block_t;

/* ── 初始化参数（ftl_init / allocator_init 共用）──
 *
 * 大数组存储（free/closed/BST 等）由调用者（host 层）提供，
 * 本结构体只传递指针与计数，指针模式避免 FTL 自身持有大数组。
 * 定义于此（而非 common/ftl_types.h）：依赖 super_block_t（本文件定义），
 * 放 common 会形成 ftl_types.h ↔ allocator.h 循环 include。
 */
typedef struct
{
    ftl_topology_t  topo;           /* 本 FTL 绑定的 NAND 芯片路由 */
    uint16_t       *free_blocks;    /* free block 数组（全局 Block 号，由调用者提供） */
    uint16_t        free_count;     /* free block 数 */
    ftl_block_t    *blocks;         /* Block State Table（由调用者提供，容量 FTL_BLOCK_TOTAL） */
    super_block_t  *closed_sb_list; /* closed Super Block 数组（由调用者提供） */
} ftl_init_params_t;

/* 返回 Super Block 中指定 Plane/Page 的展开地址（ch/ce/lun 取自 sb->topo） */
static inline nand_addr_t super_block_page_addr(const super_block_t *sb, uint8_t plane, uint8_t page)
{
    nand_addr_t addr;

    addr.ch     = sb->topo.ch;
    addr.ce     = sb->topo.ce;
    addr.lun    = sb->topo.lun;
    addr.block  = sb->plane_blocks[plane];
    addr.plane  = plane;
    addr.page   = page;
    addr.cw     = 0U; /* 0 = 整页（页级操作基址） */

    return addr;
}

/* 返回 Super Block 中指定 Plane/Page 的 PBA（用户数据使用 FTL_DATA_CH/CE） */
static inline nand_pba_t super_block_page_pba(uint8_t ch, uint8_t ce, const super_block_t *sb, uint8_t plane, uint8_t page)
{
    nand_addr_t addr = super_block_page_addr(sb, plane, page);
    addr.ch = ch; /* 显式覆盖，保持旧语义（调用点自带的 ch/ce） */
    addr.ce = ce;
    return pba_from_addr(&addr);
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
    uint8_t  lun;          /* 本 Allocator 绑定的 LUN */
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

    /* ── Block State Table（统一 per-Block 状态） ── */
    ftl_block_t *blocks;        /* 每 Block 状态数组（由调用者提供） */
    uint16_t     bad_count;     /* 坏 Block 数 */
    uint16_t     spare_pool_count; /* Spare Pool 剩余 Block 数 */
} ftl_allocator_t;

/* ── 接口函数 ── */

/*
 * allocator_init - 初始化 Allocator
 *
 * @param alloc  Allocator 上下文（由调用者提供存储）
 * @param params 初始化参数（拓扑 + 大数组存储，见 ftl_init_params_t 定义）
 *               free_blocks / free_count / blocks / closed_sb_list
 *               均为调用者提供的存储指针
 *
 * @return ftl_status_t — FTL_OK 成功
 */
ftl_status_t allocator_init(ftl_allocator_t *alloc, const ftl_init_params_t *params);

/*
 * allocator_alloc - 分配一个 PBA（一页，4 CW）
 *
 * 内部：
 *   ① 从对应温度的 Open Super Block 取当前写入位置
 *   ② Super Block 已写满 → 关闭它（→ Closed），从 Free Pool 分配新的 Super Block
 *   ③ 返回当前页的 PBA，next_page++
 *   ④ Free Pool 耗尽 → 返回 NAND_PBA_INVALID（write 路径触发紧急 GC 重试）
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
 * @param alloc  Allocator 上下文
 * @param gblock 要归还的 Block 编号（全局 Block 号）
 */
void allocator_free_block(ftl_allocator_t *alloc, uint16_t gblock);

/*
 * allocator_close_open_sb - 运行时关闭当前 Open Super Block（写失败等场景）
 *
 * 行业坏块替换流程：Super Block 中某 plane PROGRAM 失败后，①关闭该 SB
 * （不再写入新数据）→ 进 closed_sb_list → GC 优先回收（VPC 低）。坏块
 * 标记仍由 bbt_mark_bad 完成（标 block 号，同号双 plane 下两 plane 一起退役）。
 *
 * @param alloc      Allocator 上下文
 * @param temperature 要关闭的 Open SB 温度（FTL_TEMP_HOT / FTL_TEMP_COLD）
 * @return FTL_OK 成功，FTL_ERROR 无对应 Open SB
 */
ftl_status_t allocator_close_open_sb(ftl_allocator_t *alloc, uint8_t temperature);

/*
 * allocator_remove_closed_sb - GC 回收后从 Closed 列表移除条目（swap-remove，O(1)）
 *
 * 修 closed_sb_count 只增不减导致的列表爆炸 + 已回收 SB 被再次选为 victim 的无限 GC 空转。
 *
 * @param alloc      Allocator 上下文
 * @param closed_idx 要移除的 closed 列表索引
 */
void allocator_remove_closed_sb(ftl_allocator_t *alloc, uint16_t closed_idx);

/*
 * allocator_add_closed_sb - 掉电恢复重建时，把双 Plane 对直接塞进 Closed 列表
 *
 * 重建（ftl_rebuild_closed_sb_list）扫描 NAND 判出已满 Super Block 后，
 * 不能复用 allocator_close_sb（那是 Open SB 的关闭流程，前半段只清 OPEN
 * 不清 FREE，而重建块的 RAM 态是 FREE）。本函数仿照 close_sb 的后半段：
 *
 *   sb = { topo=alloc 维度, plane_blocks=入参, next_page=PAGES_PER_BLOCK,
 *          temperature=入参, valid=0 }
 *   双 Plane 置 CLOSED（清 FREE/OPEN）+ 温度 + sb_id = closed 列表索引
 *   追加进 closed_sb_list，closed_sb_count++
 *
 * @param alloc          Allocator 上下文
 * @param plane_blocks   双 Plane 同号 Block 号（plane0/plane1）
 * @param temperature    FTL_TEMP_HOT / FTL_TEMP_COLD（掉电无落盘，调用方给默认）
 * @return 塞入的 closed 列表索引；列表满返回 UINT16_MAX
 */
uint16_t allocator_add_closed_sb(ftl_allocator_t *alloc,
                                 const uint16_t plane_blocks[PLANE_NUM],
                                 uint8_t          temperature);

/*
 * allocator_remove_from_free - 从 Free Pool 移除一个 Block（掉电重建用）
 *
 * 对内部 allocator_pool_remove 的公共薄包装（后者为 static）。重建扫描发现
 * 写过（非全 FF）的块必须从 Free Pool 拉出，防止开机后被当作空白块重新分配
 * （覆盖未擦除的半满块）。
 *
 * @param alloc   Allocator 上下文
 * @param gblock 全局 Block 号（0..FTL_BLOCK_TOTAL-1）
 */
void allocator_remove_from_free(ftl_allocator_t *alloc, uint16_t gblock);

/* 说明：allocator_mark_bad 已删除——坏块标记统一走 bbt_mark_bad（BBT 单数据源），
 * BBT 内部将 is_bad 位写入 ftl_block_t.state 并维护 alloc->bad_count。 */

/*
 * allocator_needs_gc - 检查是否需要触发 GC
 *
 * @param alloc Allocator 上下文
 * @return 1 需要 GC，0 不需要
 */
int allocator_needs_gc(const ftl_allocator_t *alloc);

#endif /* FTL_ALLOCATOR_H */
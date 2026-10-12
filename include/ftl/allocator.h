#ifndef FTL_ALLOCATOR_H
#define FTL_ALLOCATOR_H

/*
 * FTL Allocator — Block 分配器
 *
 * 职责：
 *   1. 维护 Free Pool（一个已擦除池，三级**水位**见下）
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

#include "common/assertion.h" /* ASSERT（退化单 plane 条目展开地址是编码错误） */
#include "ftl/bst.h"          /* ftl_block_t, FTL_BLOCK_TOTAL, ftl_gblock, ftl_block_is_bad */
#include "ftl/ftl_type.h"     /* ftl_status_t, ftl_topology_t */
#include "nand/nand_config.h" /* nand_pba_t, nand_addr_t, nand_paa_t, PLANE_NUM, PAGES_PER_BLOCK, BLOCKS_PER_PLANE */

#include <stdint.h>

/* ── 温度等级 ── */
#define FTL_TEMP_HOT  0U /* 用户 IO 数据 */
#define FTL_TEMP_COLD 1U /* GC 搬移 / 元数据 */
#define FTL_TEMP_NUM  2U /* 温度等级数 */

/* ── Free Pool 三级水位（P1-3 定案；设计 §3.7）──
 *
 * ⚠ **三级是水位，不是分区**。池只有**一个**（`free_blocks[]`）：`gc_erase_and_free`
 *   是**先擦后归还**，所以池内每个块都已是擦除态——不存在「预擦除块」与「普通 free 块」
 *   的物理区分，也就没有东西可以分区。
 *
 *   真实主控同样如此：free pool 是**一个已擦除池 + 软/硬水位**，另外配**独立的保留池**
 *   （本仓即 OP 池 = 性能缓冲、Spare 池 = 坏块替换，见 §3.6）。没有任何主流主控在 free
 *   池内部再切「预擦除 / 未擦除」两级——擦除落在关键路径上本身就是要避免的事，
 *   那才是 OP 池与后台擦除存在的理由。
 *
 *   三级水位与各自的动作：
 *     ① free ≥ FREE_POOL_TARGET(32)          正常，不做后台 GC
 *     ② FREE_POOL_EMERGENCY(8) < free < 32   后台 GC 补池（IDLE / NORMAL）
 *     ③ free ≤ FREE_POOL_EMERGENCY(8)        紧急：强制 GC，前台写不再等
 *
 *   旧字段 `pre_cond_count` / `emergency_count` 与状态位 PRE_COND / EMERGENCY 已删除
 *   （前者恒等于 free_count，后两者从无读者）。 */
#define FREE_POOL_TARGET    (32U) /* 水位①/②分界：低于此值启动后台 GC 补池 */
#define FREE_POOL_EMERGENCY (8U)  /* 水位②/③分界：低于等于此值转紧急 GC */
#define FTL_OP_PERCENT      (10U) /* OP 预留百分比（编译期默认） */
#define FTL_SPARE_POOL_SIZE (50U) /* 坏块替换池大小 */

/* ── Closed Super Block 列表最大容量 ── */
#define SUPER_BLOCK_LIST_MAX (1024U)

/* ── Closed 条目「退化单 plane」哨兵 ──
 * Policy B 下 SB 两 plane 是**独立分配**的物理块（不再同号）。掉电重建按物理块逐个归位
 * （见 power_failure_recovery.c），无法、也不需要恢复「哪两块曾是一对」，故 closed_sb_list
 * 条目允许只含一个 plane：另一个 plane 记本哨兵。Open SB（hot_sb/cold_sb，multi-plane 写的
 * 单元）恒为双 plane，不受影响。凡按 plane 展开地址的读者都要先判哨兵。 */
#define FTL_BLOCK_NO_PLANE (UINT16_MAX)

/* ── Super Block ──
 *
 * Super Block 是跨双 Plane 的逻辑写入单元。
 * 双 Plane 各取一个物理 Block 配对，Multi-Plane PROGRAM 同步写入。
 * 两个温度（Hot/Cold）各有一个 Open Super Block。
 * 写满后从 Open → Closed，等待 GC 回收。
 */

typedef struct
{
    ftl_topology_t topo;                    /* 本 Super Block 绑定的 NAND 芯片路由（ch/ce/lun） */
    uint16_t       plane_blocks[PLANE_NUM]; /* 各 Plane 的 Block 号；可为 FTL_BLOCK_NO_PLANE（退化单 plane 条目） */
    uint8_t        next_page;               /* 当前写入页（0~63，双 Plane 同步） */
    uint8_t        temperature;             /* FTL_TEMP_HOT / FTL_TEMP_COLD */
    uint8_t        valid;                   /* 1=正在使用（Open），0=已关闭 */
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
    ftl_topology_t topo;              /* 本 FTL 绑定的 NAND 芯片路由 */
    uint16_t      *free_blocks;       /* free block 数组（全局 Block 号，由调用者提供） */
    uint16_t       free_count;        /* free block 数 */
    uint16_t      *op_blocks;         /* OP 预留 block 数组（容量不动：GC 缓冲池，由调用者提供） */
    uint16_t       op_count;          /* OP 池 Block 数 */
    uint16_t      *spare_blocks;      /* Spare Pool（坏块替换池）block 数组（由调用者提供） */
    uint16_t       spare_count;       /* Spare Pool 初始 Block 数 */
    ftl_block_t   *blocks;            /* Block State Table 条目（由调用者提供，容量 FTL_BLOCK_TOTAL） */
    uint64_t (*valid_map)[PLANE_NUM]; /* 逐页有效位图（独立表，每块 PLANE_NUM 个 u64；由调用者提供） */
    super_block_t *closed_sb_list;    /* closed Super Block 数组（由调用者提供） */
} ftl_init_params_t;

/* 该 plane 是否持有物理块（Policy B：Closed 条目允许退化单 plane，另一 plane 记哨兵）。
 * 凡按 plane 展开地址 / 索引 BST / 擦除的读者都要先判——哨兵不是合法块号。 */
static inline uint8_t super_block_has_plane(const super_block_t *sb, uint8_t plane)
{
    return (sb->plane_blocks[plane] != FTL_BLOCK_NO_PLANE) ? 1U : 0U;
}

/* 返回 Super Block 中指定 Plane/Page 的展开地址（ch/ce/lun 取自 sb->topo） */
static inline nand_addr_t super_block_page_addr(const super_block_t *sb, uint8_t plane, uint8_t page)
{
    nand_addr_t addr;

    /* 退化单 plane 条目的哨兵 plane 不可展开（plane_blocks[plane] 是哨兵，非块号） */
    ASSERT(super_block_has_plane(sb, plane) != 0U);
    addr.ch    = sb->topo.ch;
    addr.ce    = sb->topo.ce;
    addr.lun   = sb->topo.lun;
    addr.block = sb->plane_blocks[plane];
    addr.plane = plane;
    addr.page  = page;
    addr.cw    = 0U; /* 0 = 整页（页级操作基址） */

    return addr;
}

/* 返回 Super Block 中指定 Plane/Page 的 PBA（用户数据使用 FTL_DATA_CH/CE） */
static inline nand_pba_t super_block_page_pba(uint8_t ch, uint8_t ce, const super_block_t *sb, uint8_t plane, uint8_t page)
{
    nand_addr_t addr = super_block_page_addr(sb, plane, page);
    addr.ch          = ch; /* 显式覆盖，保持旧语义（调用点自带的 ch/ce） */
    addr.ce          = ce;
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

    /* ── Free Pool（一个已擦除池；三级水位见文件头） ── */
    uint16_t *free_blocks; /* free block 编号数组（由调用者提供） */
    uint16_t  free_count;  /* 空闲（且已擦除）block 数——三级水位都建在它上面 */

    /* ── OP / Spare Pool（容量不动） ── */
    uint16_t *op_blocks;    /* OP 预留 block 数组（GC 缓冲池，由调用者提供） */
    uint16_t  op_count;     /* OP 池剩余 Block 数 */
    uint16_t *spare_blocks; /* Spare Pool（坏块替换池）block 数组（由调用者提供） */
    uint16_t  spare_count;  /* Spare Pool 剩余 Block 数 */

    /* ── Open Super Block（每温度一个） ── */
    super_block_t hot_sb;  /* Hot Open Super Block */
    super_block_t cold_sb; /* Cold Open Super Block */

    /* ── Closed Super Block 列表（等待 GC） ── */
    super_block_t *closed_sb_list;  /* 已写满的 Super Block 数组（由调用者提供） */
    uint16_t       closed_sb_count; /* 已关闭 Super Block 数 */

    /* ── Block State Table（统一 per-Block 状态） ── */
    ftl_block_t *blocks;              /* 每 Block 条目数组（由调用者提供） */
    uint64_t (*valid_map)[PLANE_NUM]; /* 逐页有效位图（独立表，每块 PLANE_NUM 个 u64；由调用者提供） */
    uint16_t bad_count;               /* 坏 Block 数 */
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
 * allocator_alloc_pair - 分配一页的**两个 plane** PBA（Multi-Plane 写用）
 *
 * 内部：
 *   ① 从对应温度的 Open Super Block 取当前写入位置
 *   ② Super Block 已写满 → 关闭它（→ Closed），从 Free Pool 分配新的 Super Block
 *      （**两 plane 各自独立选块**——Policy B：不再要求 block 同号）
 *   ③ out[plane] 填当前页在两 plane 的 PBA，next_page++
 *   ④ Free Pool 耗尽 → 返回 NAND_PBA_INVALID（write 路径触发紧急 GC 重试）
 *
 * @param alloc       Allocator 上下文
 * @param temperature FTL_TEMP_HOT / FTL_TEMP_COLD
 * @param out         输出：out[plane] = 该 plane 当前页 PBA（调用者保证 PLANE_NUM 长）
 *
 * @return out[0]（= plane 0 的 PBA），NAND_PBA_INVALID 表示分配失败
 */
nand_pba_t allocator_alloc_pair(ftl_allocator_t *alloc, uint8_t temperature, nand_pba_t out[PLANE_NUM]);

/*
 * allocator_alloc - 分配一个 PBA（一页，4 CW）——单 plane 便捷
 *
 * = allocator_alloc_pair() 取 plane 0 的那一半。GC / WL / metadata 的单 plane 写路径用它。
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
 * allocator_alloc_reserved_block - 独占分配一个**整块**（元数据专用）
 *
 * 用途：L2P 日志块等「整块归元数据所有」的分配。从该 plane 的 Free Pool 取一块
 * （磨损最少者），把它从 Free Pool **移除**并标记 RESERVED。
 *
 * ⚠ 与 allocator_alloc(FTL_TEMP_COLD) 的区别：后者取的是**冷 Open SB 的一页**，
 *   而冷 SB 与 GC 迁移共享 ⇒ 元数据绝不能走那条路（会把 GC 的写指针搅乱）。
 *
 * ⚠ 三件事必须一起做——只置 RESERVED 挡不住分配，因为选块以
 *   allocator_select_plane_block_from_pool() 的 **Free Pool 数组**为准：
 *     ① allocator_pool_remove()（从 Free Pool 拿掉）
 *     ② 置 RESERVED
 *     ③ 清 FREE
 *
 * ⚠ **plane 固定**：Policy B 下两个 plane 各自独立选块，拿不到「同号双 plane 都空闲」
 *   的保证，故不跨 plane 配对；调用者给出目标 plane，返回单个 gblock。
 *
 * 归还走 allocator_free_reserved_block()（调用者保证已擦除）。
 *
 * @param alloc      Allocator 上下文
 * @param plane      目标 plane（0..PLANE_NUM-1）
 * @param out_gblock 输出：全局 Block 号（plane × BLOCKS_PER_PLANE + block）
 * @return FTL_OK / FTL_ERROR（参数非法或该 plane 无空闲块）
 */
ftl_status_t allocator_alloc_reserved_block(ftl_allocator_t *alloc, uint8_t plane, uint16_t *out_gblock);

/*
 * allocator_free_reserved_block - 归还独占块到 Free Pool（调用者保证已擦除）
 *
 * 与 allocator_free_block 同一入口语义（置 FREE、清 RESERVED/OPEN/CLOSED/TEMP、
 * 清反向索引），只是多一个「调用者必须持有 RESERVED 块」的前置约定。
 *
 * @param alloc  Allocator 上下文
 * @param gblock 要归还的全局 Block 号
 */
void allocator_free_reserved_block(ftl_allocator_t *alloc, uint16_t gblock);

/*
 * allocator_close_open_sb - 运行时关闭当前 Open Super Block（写失败等场景）
 *
 * 行业坏块替换流程：Super Block 中某 plane PROGRAM 失败后，①关闭该 SB
 * （不再写入新数据）→ 进 closed_sb_list → GC 优先回收（VPC 低）。坏块
 * 标记仍由 bbt_mark_bad 完成（标 block 号）。Policy B 下两 plane 是独立物理块，
 * 只退役真正失败的那个 plane 的块；另一 plane 块随 Closed SB 进 GC，擦除后复用。
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
 * allocator_add_closed_sb - 掉电恢复重建时，把已写过的块直接塞进 Closed 列表
 *
 * 重建（ftl_rebuild_closed_sb_list）逐 plane 扫描 NAND 判出写过的物理块后，
 * 不能复用 allocator_close_sb（那是 Open SB 的关闭流程，前半段只清 OPEN
 * 不清 FREE，而重建块的 RAM 态是 FREE）。本函数仿照 close_sb 的后半段：
 *
 *   sb = { topo=alloc 维度, plane_blocks=入参, next_page=PAGES_PER_BLOCK,
 *          temperature=入参, valid=0 }
 *   各 plane 的块置 CLOSED（清 FREE/OPEN）+ 温度 + sb_id = closed 列表索引
 *   追加进 closed_sb_list，closed_sb_count++
 *
 * @param alloc          Allocator 上下文
 * @param plane_blocks   [PLANE_NUM] 每 plane 的 Block 号；Policy B 下逐 plane 独立分配，
 *                       重建产生的**退化单 plane 条目**把另一 plane 记 FTL_BLOCK_NO_PLANE
 * @param temperature    FTL_TEMP_HOT / FTL_TEMP_COLD（掉电无落盘，调用方给默认）
 * @return 塞入的 closed 列表索引；列表满返回 UINT16_MAX
 */
uint16_t allocator_add_closed_sb(ftl_allocator_t *alloc,
                                 const uint16_t   plane_blocks[PLANE_NUM],
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
/* 说明：allocator_needs_gc 已删除（P1-3）——零调用者，且其判定（水位 / 紧急）与
 * gc_check_triggers() 的 T1/T4 重复。GC 触发判定**单一数据源** = `ftl/gc.h`。 */

#endif /* FTL_ALLOCATOR_H */
#ifndef FTL_ALLOCATOR_H
#define FTL_ALLOCATOR_H

/*
 * allocator.h — 块分配器：账本结构 + 对外接口
 *
 * 只管账本，不碰介质：块的擦除由调用者在 allocator_free_block 之前完成。
 *
 * 不变量：
 *   ① 四本账各有唯一闸门：Free Pool = allocator_free_block / allocator_remove_from_free；
 *      OP/Spare = allocator_*_try_push/pop；Closed = allocator_add_closed_sb /
 *      allocator_remove_closed_sb。标量计数（free_count / closed_sb_count）直接读；
 *      数组/边界/借出访问走 allocator_op_count / spare_count / closed_at。
 *   ② state == FTL_BLOCK_STATE_FREE ⟺ 在 Free Pool。BAD 是终态，永不进池；
 *      无归属块（固定元数据 / OP / Spare / 未初始化）为 NONE。
 *   ③ closed_sb_list 是 swap-remove 活列表：跨调用持有条目必须按值拷贝；
 *      消费方按 !is_open 过滤。
 *   ④ 指针 / 索引实参由调用者保证；实例 allocator 的绑定由 ftl_contract_audit 在 boot 期证明，
 *      运行期入口不逐次复检；init/bind 与 release 关键边界仍用 ASSERT。仅池空 / 幂等早退返回错误码。
 */

#include "assertion.h"   /* ASSERT（inline 访问器与地址展开的入参检查） */
#include "bst.h"         /* ftl_block_t, FTL_BLOCK_TOTAL, ftl_gblock */
#include "ftl_config.h"  /* DWORD, pba_make */
#include "ftl_type.h"    /* ftl_status_t, ftl_topology_t */
#include "nand_config.h" /* PLANE_NUM, PAGES_PER_BLOCK, BLOCKS_PER_PLANE */

#include <stdbool.h>
#include <stdint.h>

/* ── 温度流 ── */
#define FTL_TEMP_HOT  0U /* 用户 IO */
#define FTL_TEMP_COLD 1U /* GC 搬移 */
#define FTL_TEMP_NUM  2U

/* ── Free Pool 三级水位 ──
 * 一个已擦除池 + 软水位，不是三个分区：≥32 正常；8<free<32 后台 GC；≤8 紧急。
 * 32 / 8 为本仓经验值。 */
#define FREE_POOL_TARGET    (32U)
#define FREE_POOL_EMERGENCY (8U)

/* ── OP / Spare 保留池（按 plane）── */
#define FTL_OP_PERCENT          (10U) /* OP 预留百分比 */
#define FTL_OP_BLOCKS_PER_PLANE ((BLOCKS_PER_PLANE * FTL_OP_PERCENT) / 100U)

/* 数组维度钳到 ≥1：FTL_OP_PERCENT=0 时 [PLANE_NUM][0] 非法。 */
#define FTL_OP_POOL_CAP          ((FTL_OP_BLOCKS_PER_PLANE) > 0U ? (FTL_OP_BLOCKS_PER_PLANE) : 1U)
#define FTL_SPARE_POOL_PER_PLANE (50U) /* 每 plane 坏块替换池 */

/* ── Closed Super Block 列表容量 ──
 * = 全部物理块数：重建时每写过一块最多建一条，取更小会溢出
 * （重建 ASSERT；运行时静默漏账）。 */
#define SUPER_BLOCK_LIST_MAX (FTL_BLOCK_TOTAL)

/* ── 两个同值哨兵，语义不同（C11：同值 ≠ 同一坐标）──
 * FTL_BLOCK_NO_PLANE = plane 内块号槽位的「无块」（存储态）；
 * FTL_GBLOCK_INVALID = gblock 坐标的「选块失败」（瞬时返回值）。 */
#define FTL_BLOCK_NO_PLANE (UINT16_MAX)
#define FTL_GBLOCK_INVALID (UINT16_MAX)

/* ── Super Block ──
 * 跨 plane 的逻辑写入单元；退化条目可只含一个 plane（另一 plane 记哨兵）。 */
typedef struct
{
    WORD plane_blocks[PLANE_NUM]; /* 各 plane 的块号；可含 FTL_BLOCK_NO_PLANE */
    BYTE next_page;               /* 当前写入页（0..PAGES_PER_BLOCK） */
    BYTE temperature;             /* FTL_TEMP_HOT / FTL_TEMP_COLD */
    bool is_open;                 /* true = 仍在写；false = 已关闭 */
} super_block_t;

/* ── FTL 表的引用 ──
 * 五张大数组由 HIL（main.c）**各自独立定义、独立选内存域**（section 属性），
 * 本结构只持有它们的指针；表本体不在本结构里，故放置不受本类型约束。
 * 由 ftl_storage_t 传入，内存预算在 main.c 的 _Static_assert 单点可见。 */
typedef struct
{
    ftl_block_t *blocks;      /* BST */
    WORD        *free_blocks; /* Free Pool */
    WORD(*op_blocks)
    [FTL_OP_POOL_CAP]; /* OP 预留池 */
    WORD(*spare_blocks)
    [FTL_SPARE_POOL_PER_PLANE];    /* Spare 池 */
    super_block_t *closed_sb_list; /* Closed 列表 */
} ftl_table_refs_t;

/* ── BBT 持久化状态（per-instance）──
 * 放在本头：bbt.c 使用，且 bbt.h 已 include 本文件，反向包含会成环。 */
typedef enum
{
    BBT_JOB_IDLE = 0, /* 无 job */
    BBT_JOB_ERASE,    /* 换块擦在飞 */
    BBT_JOB_WRITE,    /* record 写在飞 */
} bbt_handle_phase_t;

typedef struct
{
    BYTE  phase;     /* bbt_handle_phase_t */
    BYTE  waiting;   /* 1 = 有命令在飞 */
    DWORD dirty_gen; /* 本条 record 组 header 时的 dirty 代际快照 */
    DWORD start_ms;
    DWORD log_ms;
} bbt_handle_t;

typedef struct
{
    DWORD        sequence;           /* 本池单调序号 */
    WORD         write_idx;          /* 池内下一个待写槽 */
    BYTE         cursor_valid;       /* 游标是否已建立 */
    BYTE         dirty;              /* 是否有未写回变更 */
    bool         format_in_progress; /* 最新 BBT record 带 FORMAT_IN_PROGRESS 标志 */
    DWORD        dirty_gen;          /* dirty 每次置位 +1；检测写飞行期间的新变更 */
    bbt_handle_t handle;             /* 异步持久化 handle */
} bbt_state_t;

/* ── Allocator 上下文 ── */
typedef struct
{
    /* 拓扑 */
    BYTE ch;
    BYTE ce;
    BYTE lun;
    WORD total_blocks; /* 用户区好块数（恢复路径置定） */

    /* Free Pool */
    WORD *free_blocks;
    WORD  free_count;

    /* OP / Spare（per-plane） */
    WORD(*op_blocks)
    [FTL_OP_POOL_CAP];
    WORD op_count[PLANE_NUM];
    WORD(*spare_blocks)
    [FTL_SPARE_POOL_PER_PLANE];
    WORD spare_count[PLANE_NUM];

    /* Open Super Block */
    super_block_t hot_sb;
    super_block_t cold_sb;

    /* Closed 列表 */
    super_block_t *closed_sb_list;
    WORD           closed_sb_count;

    /* BST */
    ftl_block_t *blocks;
    WORD         bad_count;

    /* 元数据池游标 */
    bbt_state_t bbt;
} ftl_allocator_t;

/* ── 地址 / 拓扑辅助 ── */

/* plane 是否持有物理块（退化条目在另一 plane 记哨兵）。 */
static inline bool super_block_has_plane(const super_block_t *sb, BYTE plane)
{
    return (sb->plane_blocks[plane] != FTL_BLOCK_NO_PLANE);
}

/* Super Block 指定 plane/page 的页基址 PBA（cw = 0）。 */
static inline DWORD super_block_page_pba(ftl_topology_t topo, const super_block_t *sb, BYTE plane, BYTE page)
{
    return pba_make(topo.ch, topo.ce, topo.lun, sb->plane_blocks[plane], plane, page, 0U);
}

/* 本实例 ch/ce/lun → 标准拓扑（只有 alloc 在手时用）。 */
static inline ftl_topology_t allocator_topo(const ftl_allocator_t *alloc)
{
    ftl_topology_t topo;

    topo.ch  = alloc->ch;
    topo.ce  = alloc->ce;
    topo.lun = alloc->lun;
    return topo;
}

/* ── 账本只读访问器 ──
 * 数组/边界/借出语义走这里；标量计数（free_count / closed_sb_count）直接读字段。 */

/* 各 plane OP 池剩余块数（带 plane 边界检查）。 */
static inline WORD allocator_op_count(const ftl_allocator_t *alloc, BYTE plane)
{
    return alloc->op_count[plane];
}

/* 各 plane Spare 池剩余块数（带 plane 边界检查）。 */
static inline WORD allocator_spare_count(const ftl_allocator_t *alloc, BYTE plane)
{
    return alloc->spare_count[plane];
}

/*
 * allocator_op_total - 全 plane OP 池剩余块数
 * 参数：alloc — 分配器。
 * 返回：各 plane 之和。
 */
WORD allocator_op_total(const ftl_allocator_t *alloc);

/*
 * allocator_spare_total - 全 plane Spare 池剩余块数
 * 参数：alloc — 分配器。
 * 返回：各 plane 之和。
 */
WORD allocator_spare_total(const ftl_allocator_t *alloc);

/* closed_sb_list 第 idx 项（借出，不转移所有权）。
 * swap-remove 会让先前指针失效：跨调用持有请按值拷贝。 */
static inline const super_block_t *allocator_closed_at(const ftl_allocator_t *alloc, WORD idx)
{
    return &alloc->closed_sb_list[idx];
}

/* ── OP / Spare 池闸门 ──
 * push 满 / pop 空返回 false（资源类结果，不 ASSERT）；池操作不改 BST 状态。
 * 定义在 allocator.c，保证唯一实现；OP 出池只在 .c 内部使用，不对外声明。 */

/*
 * allocator_op_try_push - OP 池入池
 * 功能：按 plane 追加；满则返回 false。
 * 参数：alloc — 分配器；plane — 目标 plane；gblock — 全局块号。
 * 返回：true 入池；false 池满。
 */
bool allocator_op_try_push(ftl_allocator_t *alloc, BYTE plane, WORD gblock);

/*
 * allocator_spare_try_push - Spare 池入池
 * 功能：按 plane 追加；满则返回 false。
 * 参数：alloc — 分配器；plane — 目标 plane；gblock — 全局块号。
 * 返回：true 入池；false 池满。
 */
bool allocator_spare_try_push(ftl_allocator_t *alloc, BYTE plane, WORD gblock);

/*
 * allocator_spare_try_pop - Spare 池出池
 * 功能：按 plane 弹出一个块；空则返回 false。
 * 参数：alloc — 分配器；plane — 目标 plane；out_gblock — 出参。
 * 返回：true 取到；false 池空。
 */
bool allocator_spare_try_pop(ftl_allocator_t *alloc, BYTE plane, WORD *out_gblock);

/*
 * allocator_spare_refill_from_free - 从 Free Pool 补 Spare 池到目标（逐 plane）
 *
 * 开机恢复把 CP 快照的 BAD 并集应用后，原本在 Spare 池的块可能已变成 BAD，
 * 池计数低于 FTL_SPARE_POOL_PER_PLANE。本函数按 plane 从 Free Pool 移块补足：
 * 摘 Free → state 置 NONE → push Spare；无同 plane 空闲块时自然停止（容量下降）。
 *
 * @param alloc 分配器；plane 目标 plane
 * @return true = 至少移了一块；false = 已满 / 无可补
 */
bool allocator_spare_refill_from_free(ftl_allocator_t *alloc, BYTE plane);

/* ── 对外接口 ── */

/*
 * allocator_init - 初始化分配器
 * 功能：绑定本实例的五个表指针，清零计数与游标。
 * 参数：alloc — 分配器；topo — 本实例路由；refs — 表引用（五项非空）。
 * 返回：无。
 */
void allocator_init(ftl_allocator_t *alloc, ftl_topology_t topo, const ftl_table_refs_t *refs);

/*
 * allocator_free_block - 归还一个已擦除块到 Free Pool
 * 功能：入池并置 FREE、清 sb_id。
 * 参数：alloc — 分配器；gblock — 全局块号。
 * 返回：无。
 */
void allocator_free_block(ftl_allocator_t *alloc, WORD gblock);

/*
 * allocator_remove_from_free - 从 Free Pool 移除一个块（出池不改状态）
 * 功能：swap-remove；块不在池内是合法 no-op。
 * 参数：alloc — 分配器；gblock — 全局块号。
 * 返回：无。
 */
void allocator_remove_from_free(ftl_allocator_t *alloc, WORD gblock);

/*
 * allocator_note_erase - 记录一次成功的擦除
 * 功能：erase_count++、last_pe_ts = now、valid_count 与 read_count 清零
 *       （read disturb 按“自上次擦除以来”计；rr 健康趋势保留）。
 * 参数：alloc — 分配器；gblock — 全局块号；now — 当前时间戳。
 * 返回：无。
 */
void allocator_note_erase(ftl_allocator_t *alloc, WORD gblock, DWORD now);

/*
 * allocator_alloc_pair - 分配各 plane 当前页的 PBA
 * 功能：推进指定温度的 Open SB 写指针；SB 未开/写满/含坏块时先换新 SB。
 * 参数：alloc — 分配器；temperature — FTL_TEMP_HOT / FTL_TEMP_COLD；
 *       out[PLANE_NUM] — 各 plane 页基址 PBA。
 * 返回：FTL_OK；FTL_AGAIN = 某 plane 无块（瞬态，调用者触发 GC）。
 */
ftl_status_t allocator_alloc_pair(ftl_allocator_t *alloc, BYTE temperature, DWORD out[PLANE_NUM]);

/*
 * allocator_rewind_pair - 撤销**紧邻的**一次 allocator_alloc_pair
 * 功能：把指定温度 Open SB 的写指针退一页（next_page--），供「已取到页对、但在编程之前
 *       失败」的调用者归还该页。
 * 参数：alloc — 分配器；temperature — FTL_TEMP_HOT / FTL_TEMP_COLD（须与那次 alloc 相同）。
 * 返回：无。
 *
 * ⚠ 两条前提（违反会**还错页**，且编译期完全无声）：
 *   ① 自那次 allocator_alloc_pair 起**没有**其他页对分配发生——本函数只回退写指针，
 *      不回退「可能刚发生过的开新 SB」；
 *   ② 归还的页**从未被编程**（已下发的命令引用着它）。
 *   ⚠ 若那次 alloc 顺带开了一个新 SB，归还后该 SB 处于 next_page == 0 的**合法态**
 *     （等价于「刚开、未写」）：块仍由 Open SB 持有，无容量损失，且下次重试直接命中页 0。
 *
 * 用途：`wbuf_handle(FLUSH)` 的取页后失败路径——不归还则每次重试白烧一对页，
 *       表现为**容量无声流失**（CLAUDE.md §3.1）。
 */
void allocator_rewind_pair(ftl_allocator_t *alloc, BYTE temperature);

/*
 * allocator_close_open_sb - 关闭指定温度的 Open SB
 * 功能：置 CLOSED、进 closed_sb_list；已关闭时幂等早退。
 * 参数：alloc — 分配器；temperature — FTL_TEMP_HOT / FTL_TEMP_COLD。
 * 返回：FTL_OK；FTL_ERROR = 该温度无 Open SB。
 */
ftl_status_t allocator_close_open_sb(ftl_allocator_t *alloc, BYTE temperature);

/*
 * allocator_add_closed_sb - 掉电重建时直接追加一个 Closed SB 条目
 * 功能：按 plane 块号组装条目并置 CLOSED；允许退化单 plane。
 * 参数：alloc — 分配器；plane_blocks[PLANE_NUM] — 各 plane 块号；
 *       temperature — 温度流。
 * 返回：新条目在 closed_sb_list 的索引；列表满（release 兜底）返回 FTL_BLOCK_NO_SB。
 */
WORD allocator_add_closed_sb(ftl_allocator_t *alloc,
                             const WORD       plane_blocks[PLANE_NUM],
                             BYTE             temperature);

/*
 * allocator_remove_closed_sb - 从 Closed 列表移除条目（swap-remove）
 * 功能：修正被搬移条目的 sb_id，O(1)。
 * 参数：alloc — 分配器；closed_idx — 条目索引。
 * 返回：无。
 */
void allocator_remove_closed_sb(ftl_allocator_t *alloc, WORD closed_idx);

/*
 * allocator_closed_drop_plane - 从 Closed 条目摘除一个已擦除/已标坏的 plane 块
 *
 * GC/WL/predict 逐 plane 擦 victim。若擦完一个 plane 后 job 中止，旧条目仍引用
 * 已归还 Free Pool 的块；该块被重新分配并再次 Closed 后会被旧条目误认
 * （RISK-GC-04）。擦完即摘除，条目只在仍有未处理 plane 时存在。
 *
 * @param alloc  分配器
 * @param plane  plane 号
 * @param block  该 plane 内的块号
 * @return true = 条目已无任何 plane（条目被移除）；false = 仍有其他 plane 或未找到
 */
bool allocator_closed_drop_plane(ftl_allocator_t *alloc, BYTE plane, WORD block);

/* ── 只读统计 ──
 * 口径：跳过 BAD 与 state == NONE（固定元数据 / OP / Spare / 未初始化）；
 * 退化单 plane 条目只按实际 plane 计算；valid_count 单位是 CW。 */

typedef struct
{
    WORD  valid_cw;    /* 各 plane valid_count 之和 */
    WORD  plane_count; /* 实际 plane 数 */
    DWORD erase_max;   /* 最大 erase_count */
    DWORD erase_sum;   /* erase_count 之和 */
    DWORD pe_min;      /* 最小 last_pe_ts；无 plane 时 UINT32_MAX */
    DWORD read_sum;    /* read_count 之和 */
} allocator_sb_stats_t;

/*
 * allocator_sb_stats - 计算一个 Super Block 的聚合值
 * 参数：alloc — 分配器；sb — 目标条目；out — 出参。
 * 返回：无。
 */
void allocator_sb_stats(const ftl_allocator_t *alloc, const super_block_t *sb, allocator_sb_stats_t *out);

typedef struct
{
    DWORD erase_min;  /* 最小擦除次数；无块时为 0 */
    DWORD erase_max;  /* 最大擦除次数；无块时为 0 */
    DWORD erase_sum;  /* 擦除次数之和 */
    DWORD max_pe_ts;  /* 最大 last_pe_ts */
    WORD  good_count; /* 已归属好块数 */
} allocator_erase_stats_t;

/*
 * allocator_erase_stats - 已归属好块的磨损统计（一次扫描）
 * 参数：alloc — 分配器；out — 出参。
 * 返回：无。
 */
void allocator_erase_stats(const ftl_allocator_t *alloc, allocator_erase_stats_t *out);

/*
 * allocator_count_blocks_below - 擦除次数严格小于 threshold 的已归属好块数
 * 参数：alloc — 分配器；threshold — 上界（不含）。
 * 返回：块数。
 */
WORD allocator_count_blocks_below(const ftl_allocator_t *alloc, DWORD threshold);

/*
 * allocator_bbt_format_in_progress - BBT 是否处于格式化中
 * 参数：alloc — 分配器。
 * 返回：true = 最新 BBT record 带 FORMAT_IN_PROGRESS。
 */
bool allocator_bbt_format_in_progress(const ftl_allocator_t *alloc);

/*
 * allocator_block_restore_wear - 从 Checkpoint 磨损快照恢复一个块的磨损/ECC 状态
 * 参数：alloc — 分配器；gblock — 全局块号；其余为快照字段。
 * 返回：无。
 */
void allocator_block_restore_wear(ftl_allocator_t *alloc,
                                  WORD             gblock,
                                  DWORD            erase_count,
                                  DWORD            read_count,
                                  BYTE             last_ecc,
                                  BYTE             rising_count,
                                  BYTE             health);

/*
 * allocator_block_wear_get - 读取一个块的磨损/ECC 快照字段
 * 参数：alloc — 分配器；gblock — 全局块号；其余为出参。
 * 返回：无。
 */
void allocator_block_wear_get(const ftl_allocator_t *alloc,
                              WORD                   gblock,
                              DWORD                 *erase_count,
                              DWORD                 *read_count,
                              BYTE                  *last_ecc,
                              BYTE                  *rising_count,
                              BYTE                  *health);

/*
 * allocator_op_at / allocator_spare_at - OP / Spare 池元素只读访问
 */
WORD allocator_op_at(const ftl_allocator_t *alloc, BYTE plane, WORD index);
WORD allocator_spare_at(const ftl_allocator_t *alloc, BYTE plane, WORD index);

/*
 * allocator_find_closed_sb - 按全部 plane 块号在 Closed 列表定位条目
 * 参数：alloc — 分配器；sb — 目标条目；out_idx — 出参索引。
 * 返回：true = 找到；false = 不在列表。
 */
bool allocator_find_closed_sb(const ftl_allocator_t *alloc, const super_block_t *sb, WORD *out_idx);

#endif /* FTL_ALLOCATOR_H */

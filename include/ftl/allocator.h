#ifndef FTL_ALLOCATOR_H
#define FTL_ALLOCATOR_H

/*
 * allocator.h — 块分配器：账本结构 + 对外接口
 *
 * 只管账本，不碰介质：块的擦除由调用者在 alc_free_push 之前完成。
 *
 * 不变量：
 *   ① 四类池（Free/OP/Spare/Closed）各有唯一闸门：Free Pool = alc_free_push / alc_free_remove；
 *      OP = alc_op_push（出池由 alc_free_refill_from_op 内部完成）；Spare = alc_spare_push/pop；
 *      Closed = alc_super_closed_add /
 *      alc_super_closed_remove。标量计数（free_count / closed_sb_count）直接读；
 *      数组/边界/借出访问走 alc_op_get_count / alc_spare_get_count / alc_super_closed_get_at。
 *   ② state == FTL_BLOCK_STATE_FREE ⟺ 在 Free Pool。进池/出池两个闸门各自维护这半边：
 *      push 置 FREE，remove 置 NONE（目标态由调用者随后赋值）。BAD 是终态，永不进池；
 *      无归属块（固定元数据 / OP / Spare / 未初始化）为 NONE。
 *   ③ closed_sb_list 是 swap-remove 活列表：跨调用持有条目必须按值拷贝；
 *      消费方按 !is_open 过滤。
 *   ④ 指针 / 索引实参由调用者保证；实例 allocator 的绑定由 ftl_contract_audit 在 boot 期证明，
 *      运行期入口不逐次复检；init/bind 与 release 关键边界仍用 ASSERT。仅池空 / 幂等早退返回错误码。
 */

#include "assertion.h"  /* ASSERT（inline 访问器与地址展开的入参检查） */
#include "bst.h"        /* bst_block_t, FTL_BLOCK_TOTAL, ftl_gblock */
#include "ftl_config.h" /* DWORD, pba_make */
#include "ftl_type.h"   /* ftl_status_e, ftl_topo_t */
#include "log.h"
#include "nand_config.h" /* PLANE_NUM, PAGES_PER_BLOCK, BLOCKS_PER_PLANE */

#include <stdbool.h>
#include <stdint.h>

/* ── 温度流 ── */
#define FTL_TEMP_HOT  0U /* 用户 IO */
#define FTL_TEMP_COLD 1U /* GC 搬移 */
#define FTL_TEMP_NUM  2U

/* 分配策略：温度流 + 选块方向 + 是否允许 OP 兜底。
 * 合在一个闭枚举里，避免 `temperature` 与 `high_wear` 组合出非法状态。 */
typedef enum
{
    ALC_SUPER_ALLOC_HOT            = 0, /* HOT SB；动态 WL 取擦除最少；允许 OP 兜底 */
    ALC_SUPER_ALLOC_COLD           = 1, /* COLD SB；动态 WL 取擦除最少；允许 OP 兜底 */
    ALC_SUPER_ALLOC_COLD_HIGH_WEAR = 2, /* COLD SB；取擦除最多；禁止 OP 兜底（静态 WL） */
} alc_super_alloc_mode_e;

/* ── Free Pool 三级水位 ──
 * 一个已擦除池 + 软水位，不是三个分区：≥32 正常；8<free<32 后台 GC；≤8 紧急。
 * 32 ≈ 实例数据块（3392）的 1%，给 GC/写放大留出提前量；8 是池见底的最后手段。
 * 这两个值是本仓经验值，不是行业标准；调整需同时看写放大、稳态吞吐与 host 延迟。 */
#define FREE_POOL_TARGET    (32U)
#define FREE_POOL_EMERGENCY (8U)

/* 单 plane 空闲水位（本仓自拟）。成对分配要求「每个 plane 各一块」——只看总水位会出现
 * 「free=264 但一侧为 0」的分配失败（板端 2026-10-31 RISK-ALLOC-09）。任一 plane 低于
 * 该值即视为饥饿，触发 GC 并让 victim 选择优先回收该 plane。
 * 取 4：够一次成对分配 + GC 目的页对 + CP/journal 的短时余量；经验值，不是行业标准。 */
#define FREE_POOL_PLANE_LOW (4U)

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
    BYTE page0_programmed;        /* bit p = plane p 的 page0 写命令已成功（异步写屏障） */
} super_block_t;

/* per-plane 池表：用数组 typedef 保持 `name[plane][index]` 的二维语义，
 * 同时避免 clang-format 把 pointer-to-array 声明拆成两行。 */
typedef WORD alc_op_block_table_t[FTL_OP_POOL_CAP];
typedef WORD alc_spare_block_table_t[FTL_SPARE_POOL_PER_PLANE];

/* ── FTL 表的引用 ──
 * 五张大数组由 HIL（main.c）**各自独立定义、独立选内存域**（section 属性），
 * 本结构只持有它们的指针；表本体不在本结构里，故放置不受本类型约束。
 * 由 ftl_storage_t 传入，内存预算在 main.c 的 _Static_assert 单点可见。 */
typedef struct
{
    bst_block_t             *blocks;         /* BST */
    WORD                    *free_blocks;    /* Free Pool */
    alc_op_block_table_t    *op_blocks;      /* OP 预留池 */
    alc_spare_block_table_t *spare_blocks;   /* Spare 池 */
    super_block_t           *closed_sb_list; /* Closed 列表 */
} ftl_table_refs_t;

/* ── BBT 持久化状态（per-instance）──
 * 放在本头：bbt.c 使用，且 bbt.h 已 include 本文件，反向包含会成环。 */
typedef enum
{
    BBT_JOB_IDLE = 0, /* 无 job */
    BBT_JOB_ERASE,    /* 换块擦在飞 */
    BBT_JOB_WRITE,    /* record 写在飞 */
} bbt_handle_phase_e;

typedef struct
{
    bbt_handle_phase_e phase;     /* 异步持久化状态机 */
    bool               waiting;   /* true = 有命令在飞 */
    DWORD              dirty_gen; /* 本条 record 组 header 时的 dirty 代际快照 */
    log_heartbeat_t    hb;        /* job 心跳（start/log + active） */
} bbt_handle_t;

/* BBT 后台持久化「起新 job」的节拍；在飞续跑不受此限（bbt_handle 先处理在飞态）。
 * 由 timer 任务置 flush_due，bbt_handle 消费。 */
#define BBT_FLUSH_INTERVAL_MS (1000U)

typedef struct
{
    DWORD        sequence;           /* 本池单调序号 */
    WORD         write_idx;          /* 池内下一个待写槽 */
    bool         cursor_valid;       /* 游标是否已建立 */
    bool         dirty;              /* 是否有未写回变更 */
    bool         format_in_progress; /* 最新 BBT record 带 FORMAT_IN_PROGRESS 标志 */
    bool         rebuild_required;   /* 最新 BBT record 带 REBUILD_REQUIRED 标志（本仓自拟） */
    DWORD        dirty_gen;          /* dirty 每次置位 +1；检测写飞行期间的新变更 */
    BYTE         flush_due;          /* 1 = timer 节拍到，允许起新 job；在飞续跑不看它 */
    bbt_handle_t handle;             /* 异步持久化 handle */
} bbt_state_t;

/* ── 写轨迹飞行记录器（诊断）──
 * 最近 N 次 host 写/程序失败的 (lba, pba, plane/block/page/cw)。平时不打印；
 * 首次 UNC / program-fail 时 dump 与失败 LBA/PBA 相关的条目：
 *   · 同一 PBA 出现两次 ⇒ 同页二次编程；
 *   · 只有一次 ⇒ 写后读坏（时序/介质）；
 *   · kind=1 ⇒ 该次 program 失败。
 * 64 × 20B ≈ 1.3 KB，放 alc_t（AXI RAM）。 */
#define ALC_TRACE_DEPTH (64U)

typedef enum
{
    ALC_TRACE_WRITE_OK     = 0, /* 写确认成功 */
    ALC_TRACE_PROGRAM_FAIL = 1, /* program fail */
    ALC_TRACE_GC_WRITE     = 2, /* GC 搬移写 */
} alc_trace_kind_e;

typedef struct
{
    DWORD            seq;   /* 记录序号（单调） */
    DWORD            lba;   /* host LBA */
    DWORD            pba;   /* 目标 PBA（cw 位保留） */
    BYTE             plane; /* 以下为 pba 解码，便于日志直接打印 */
    WORD             block;
    BYTE             page;
    BYTE             cw;
    alc_trace_kind_e kind; /* 写确认成功 / program fail / GC 写 */
} alc_trace_t;

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
    /* per-plane 空闲块数：由唯一闸门 alc_free_push / alc_free_remove 维护（README 的「四类池」）。
     * 成对分配与 GC 的 plane 平衡判据都读它，不再扫 free_blocks 推导。 */
    WORD free_plane_count[PLANE_NUM];

    /* OP / Spare（per-plane） */
    alc_op_block_table_t    *op_blocks;
    WORD                     op_count[PLANE_NUM];
    alc_spare_block_table_t *spare_blocks;
    WORD                     spare_count[PLANE_NUM];

    /* Open Super Block */
    super_block_t hot_sb;
    super_block_t cold_sb;

    /* Closed 列表 */
    super_block_t *closed_sb_list;
    WORD           closed_sb_count;

    /* BST */
    bst_block_t *blocks;
    WORD         bad_count;

    /* 元数据池游标 */
    bbt_state_t bbt;

    /* 固定元数据分区候选池（md_pools_t，定义在 metadata.h）。
     * 由 ftl_init 绑定到本实例的 metadata_dir；只读，唯一写点是
     * md_pool_build_all()。跨层前向声明避免 allocator.h ↔ metadata.h 环。 */
    struct md_pools *pools;

    /* Directory（只读回指）：bbt_mark_bad 需要判断「从 OP 借用的 L2P 日志块」
     * 也是元数据块（不在固定保留区但 l2p_live/当前日志块指向它）。 */
    struct md_dir_cache *md_dir;

    /* 写轨迹飞行记录器（见 ALC_TRACE_DEPTH） */
    alc_trace_t trace[ALC_TRACE_DEPTH];
    DWORD       trace_seq;
} alc_t;

/* ── 地址 / 拓扑辅助 ── */

/**
 * @brief plane 是否持有物理块（退化条目在另一 plane 记哨兵）。
 *
 * @param sb    Super Block
 * @param plane plane 号
 * @return true = 该 plane 有块
 *         false = 槽位为 FTL_BLOCK_NO_PLANE
 */
static inline bool super_block_has_plane(const super_block_t *sb, BYTE plane)
{
    return (sb->plane_blocks[plane] != FTL_BLOCK_NO_PLANE);
}

/**
 * @brief Super Block 指定 plane/page 的页基址 PBA（cw = 0）。
 *
 * @param topo  本实例拓扑
 * @param sb    Super Block
 * @param plane plane 号
 * @param page  页号
 * @return 该 plane/page 的页基址 PBA（cw = 0）。
 */
static inline DWORD super_block_page_pba(ftl_topo_t topo, const super_block_t *sb, BYTE plane, BYTE page)
{
    return pba_make(topo.ch, topo.ce, topo.lun, sb->plane_blocks[plane], plane, page, 0U);
}

/**
 * @brief 本实例 ch/ce/lun → 标准拓扑（只有 alloc 在手时用）。
 *
 * @param alloc 分配器
 * @return 本实例的 ch/ce/lun 拓扑。
 */
static inline ftl_topo_t alc_topo(const alc_t *alloc)
{
    ftl_topo_t topo;

    topo.ch  = alloc->ch;
    topo.ce  = alloc->ce;
    topo.lun = alloc->lun;
    return topo;
}

/* ── 通用工具 ── */

/**
 * @brief 对 per-plane 计数数组求和
 *
 * @param counts 首地址（WORD[]）
 * @param count 元素个数。
 *
 * @return 各元素之和。调用点直接传 `alloc->op_count` / `alloc->spare_count` + `PLANE_NUM`；
 *         元素宽度由 `WORD *` 类型确定，不再为两个池各包一层 getter。
 */
WORD alc_pool_sum_counts(const WORD *counts, BYTE count);

/* ── 1. 初始化 ── */

/**
 * @brief 初始化分配器
 *
 * @param alloc 分配器
 * @param topo 本实例路由
 * @param refs 表引用（五项非空）。
 */
void alc_init(alc_t *alloc, ftl_topo_t topo, const ftl_table_refs_t *refs);

/* ── 2. Free Pool ── */

/**
 * @brief Spare 耗尽时的第二道坏块替换：从 OP 池取一块补 Free
 *
 * 主流 spare/OP 共用同一块预留区；本仓分两池，Spare 是坏块替换的第一道，
 * OP 是 GC 余量。Spare 空时允许从 OP 借一块补 Free（不改变已枚举容量），
 * 避免运行期坏块直接吃用户可用块。
 *
 * @param alloc 分配器
 * @param plane 目标 plane
 * @return 取到的 gblock（已入 Free）
 *         FTL_GBLOCK_INVALID = OP 池也空
 */
WORD alc_free_refill_from_op(alc_t *alloc, BYTE plane);

/**
 * @brief 归还一个已擦除块到 Free Pool
 *
 * @param alloc 分配器
 * @param gblock 全局块号。
 */
void alc_free_push(alc_t *alloc, WORD gblock);

/**
 * @brief 从 Free Pool 移除一个块（出池置 NONE）
 *
 * 不在池内是文档化 no-op（状态不变）；命中时由本闸门把 state 置 NONE，
 * 调用者在返回后赋目标态（OPEN / LOG / CLOSED / BAD）。
 *
 * @param alloc 分配器
 * @param gblock 全局块号。
 */
void alc_free_remove(alc_t *alloc, WORD gblock);

/**
 * @brief 各 plane 的 Free Pool 块数（O(1)，由 push/remove 闸门维护）
 *
 * 用途：成对分配的单 plane 水位判定、GC victim 的 plane 平衡偏好。
 *
 * @param alloc 分配器
 * @param plane 目标 plane
 * @return 该 plane 的空闲块数（0..BLOCKS_PER_PLANE）
 */
static inline WORD alc_free_get_count(const alc_t *alloc, BYTE plane)
{
    ASSERT(plane < PLANE_NUM);
    return alloc->free_plane_count[plane];
}

/**
 * @brief 最饥渴 plane 的空闲块数（成对分配真正可用的余量）
 *
 * 总 `free_count` 不能代表成对分配能力——一侧为空即无法分配（RISK-ALLOC-09）。
 * 诊断 / 水位判据都看这个值；只用它做观测，不替代 per-plane 判定。
 *
 * @param alloc 分配器
 * @return 各 plane 空闲块数的最小值
 */
static inline WORD alc_free_min_plane_count(const alc_t *alloc)
{
    WORD m = alloc->free_plane_count[0];

    for (BYTE p = 1U; p < PLANE_NUM; p++)
    {
        if (alloc->free_plane_count[p] < m)
        {
            m = alloc->free_plane_count[p];
        }
    }

    return m;
}

/**
 * @brief Free Pool 中「各 plane 最大擦除次数」的最小值
 *
 * 静态 WL 的触发判据（UBI 的 e2）：高擦除空闲块是否足以接收冷数据。
 * 目的页对每 plane 各取一块，受最紧的 plane 限制 ⇒ 取 per-plane 最大值的 min。
 *
 * @param alloc 分配器
 * @return 各 plane 最大擦除次数的最小值；任一 plane 无空闲块时 0（此时也无法分配目标页对）。
 */
DWORD alc_free_get_max_erase(const alc_t *alloc);

/* ── 3. OP Pool ── */

/**
 * @brief 各 plane OP 池剩余块数（带 plane 边界检查）。
 *
 * @param alloc 分配器
 * @param plane 目标 plane
 * @return 该 plane 的 OP 池剩余块数。
 */
static inline WORD alc_op_get_count(const alc_t *alloc, BYTE plane)
{
    return alloc->op_count[plane];
}

/**
 * @brief OP 池入池
 *
 * @param alloc 分配器
 * @param plane 目标 plane
 * @param gblock 全局块号。
 *
 * @return true = 入池成功
 *         false = 池满
 */
bool alc_op_push(alc_t *alloc, BYTE plane, WORD gblock);

/**
 * @brief 从 OP 池借一块作动态 L2P 日志块
 *
 * @param alloc 分配器
 * @param plane 目标 plane。
 *
 * @return gblock（已出池、state=LOG）
 *         FTL_GBLOCK_INVALID = 该 plane Free Pool 空。用途：固定 L2P 日志区满时的**有界溢出**。
 *         从 OP（GC headroom）借还闭环，不动用户容量；借出的块由 metadata 的 `l2p_live` / `md_block_is_metadata`动态识别，擦空后回 OP。
 */
WORD alc_op_alloc_log_block(alc_t *alloc, BYTE plane);

/**
 * @brief OP 池元素只读访问；调用者保证 plane < PLANE_NUM、index < op_count[plane]
 *        （偏离 H14：现有唯一调用链已保证，不另加断言）。
 *
 * @param alloc 分配器
 * @param plane 目标 plane
 * @param index 池内下标
 * @return 该槽位的 gblock。
 */
static inline WORD alc_op_get_at(const alc_t *alloc, BYTE plane, WORD index)
{
    return alloc->op_blocks[plane][index];
}

/* ── 4. Spare Pool ── */

/**
 * @brief 各 plane Spare 池剩余块数（带 plane 边界检查）。
 *
 * @param alloc 分配器
 * @param plane 目标 plane
 * @return 该 plane 的 Spare 池剩余块数。
 */
static inline WORD alc_spare_get_count(const alc_t *alloc, BYTE plane)
{
    return alloc->spare_count[plane];
}

/**
 * @brief Spare 池入池
 *
 * @param alloc 分配器
 * @param plane 目标 plane
 * @param gblock 全局块号。
 *
 * @return true = 入池成功
 *         false = 池满
 */
bool alc_spare_push(alc_t *alloc, BYTE plane, WORD gblock);

/**
 * @brief Spare 池出池
 *
 * @param alloc 分配器
 * @param plane 目标 plane
 * @param out_gblock 出参。
 *
 * @return true = 取到
 *         false = 池空
 */
bool alc_spare_pop(alc_t *alloc, BYTE plane, WORD *out_gblock);

/**
 * @brief 从 Free Pool 补 Spare 池到目标（逐 plane）
 *
 * 开机恢复把 CP 快照的 BAD 并集应用后，原本在 Spare 池的块可能已变成 BAD，
 * 池计数低于 FTL_SPARE_POOL_PER_PLANE。本函数按 plane 从 Free Pool 移块补足：
 * 摘 Free → state 置 NONE → push Spare；无同 plane 空闲块时自然停止（容量下降）。
 *
 * @param alloc 分配器
 * @param plane 目标 plane
 * @return true = 至少移了一块
 *         false = 已满 / 无可补
 */
bool alc_spare_refill_from_free(alc_t *alloc, BYTE plane);

/**
 * @brief Spare 池元素只读访问；调用者保证 plane < PLANE_NUM、index < spare_count[plane]
 *        （偏离 H14：现有唯一调用链已保证，不另加断言）。
 *
 * @param alloc 分配器
 * @param plane 目标 plane
 * @param index 池内下标
 * @return 该槽位的 gblock。
 */
static inline WORD alc_spare_get_at(const alc_t *alloc, BYTE plane, WORD index)
{
    return alloc->spare_blocks[plane][index];
}

/* ── 5. Super Block：Open ── */

/* PBA 相对指定温度 Open SB 的位置/状态。 */
typedef enum
{
    ALC_SUPER_PBA_NOT_OPEN           = 0, /* 不属于当前 Open SB（旧 SB / 非法 PBA） */
    ALC_SUPER_PBA_OPEN_PAGE0_PENDING = 1, /* 属于 Open SB，但该 plane 的 page0 未成功 */
    ALC_SUPER_PBA_OPEN_PAGE0_DONE    = 2, /* 属于 Open SB，且该 plane 的 page0 已成功 */
} alc_super_pba_state_e;

/**
 * @brief 当前 Open SB 还能连续写多少页对
 *
 * 返回各 **存在 plane** 的 `PAGES_PER_BLOCK - next_page` 的最小值；无 Open SB、
 * 某 plane 槽位无块（FTL_BLOCK_NO_PLANE）、或该 plane 的 page0 尚未成功编程 ⇒ 0。
 *
 * 用途：cache program 批次要求同一 Open SB 内至少 N 个连续页对；先查余量可
 * 避免一次 batch 分配跨块（跨块会让失败清理与 page0 约束复杂化）。
 *
 * @param alloc      分配器
 * @param temperature FTL_TEMP_HOT / FTL_TEMP_COLD
 * @return 可连续分配的页对数（0..PAGES_PER_BLOCK）
 */
BYTE alc_super_open_get_room(const alc_t *alloc, BYTE temperature);

/**
 * @brief cache 写序列专用的 Open SB 余量（含「新块由本序列自己打开」的语义）
 *
 * 与 `alc_super_open_get_room` 的差异只有三种状态：
 *   · **无 Open SB / SB 已满** ⇒ 返回 `PAGES_PER_BLOCK`：下一次
 *     `alc_super_open_alloc_pair` 会关闭旧块并开新块，新块的 page0 由**本序列**
 *     在第一页程序时承担（写流程会在成功结算时 `note_page_programmed`）。
 *   · **SB 已开、page0 尚未成功**：
 *       `page0_from_seq == true`（page0 是本序列当前在飞/已提交的页）⇒ 返回
 *       `PAGES_PER_BLOCK - next_page`：cache program 是**有序管线**，page0 的失败
 *       会中止整个序列（其后页一律不确认），因此不会出现「page0 失败但 page>0
 *       被确认」而击穿 rebuild 的 page0 空白捷径；
 *       否则 ⇒ 0（page0 属于别的写者，必须等它确认）。
 *   · 其余（plane 不全 / 含 BAD / page0 已成功）与 `get_room` 相同。
 *
 * ⚠ 只有**同一 cache 序列**可以传 `page0_from_seq == true`；独立程序者
 *   （wbuf / GC / WL）仍必须用 `get_pba_state` 的 page0 屏障。
 *
 * @param alloc          分配器
 * @param temperature    FTL_TEMP_HOT / FTL_TEMP_COLD
 * @param page0_from_seq 当前页（若为 page0）是否由本 cache 序列写入
 * @return 可连续分配的页对数（0..PAGES_PER_BLOCK）
 */
BYTE alc_super_open_get_room_seq(const alc_t *alloc, BYTE temperature, bool page0_from_seq);

/**
 * @brief 记录某个 Open SB 的页编程成功（page0 异步写屏障）
 *
 * 只关心 page 0：它成功前，**独立程序者**（wbuf / GC / WL）在提交点被
 * `alc_super_open_get_pba_state` 的 page0 屏障挡住（`alloc_pair` 本身不拒绝分配），
 * 防止「page0 失败但后续页已确认」击穿掉电恢复的 page0 空白捷径；
 * cache 写序列则靠**有序管线**保证同一不变量（见 `alc_super_open_get_room_seq`）。
 * 失败/关闭由 alc_super_close 处理（新 SB 的 flag 为 false）。
 *
 * @param alloc       分配器
 * @param temperature FTL_TEMP_HOT / FTL_TEMP_COLD
 * @param pba         刚编程成功的页基址 PBA（仅 page 0 生效）
 */
void alc_super_open_note_page_programmed(alc_t *alloc, BYTE temperature, DWORD pba);

/**
 * @brief PBA 相对指定温度 Open SB 的状态
 *
 * 按 plane+block 判断（不是按页号）：
 *   · NOT_OPEN：旧 SB 的迟到命令 / 非法 PBA，batch 失败时不得因此关闭当前 SB；
 *   · PAGE0_PENDING：属于当前 SB，但 page0 写屏障未通过 ⇒ page>0 的写必须等待；
 *   · PAGE0_DONE：属于当前 SB 且 page0 已成功。
 *
 * @param alloc       分配器
 * @param temperature FTL_TEMP_HOT / FTL_TEMP_COLD
 * @param pba         待判断的页基址 PBA
 *
 * @return ALC_SUPER_PBA_NOT_OPEN = 不属于当前 Open SB（旧 SB / 非法 PBA）
 *         ALC_SUPER_PBA_OPEN_PAGE0_PENDING = 属于当前 SB，但 page0 写屏障未通过
 *         ALC_SUPER_PBA_OPEN_PAGE0_DONE = 属于当前 SB 且 page0 已成功
 */
alc_super_pba_state_e alc_super_open_get_pba_state(const alc_t *alloc, BYTE temperature, DWORD pba);

/**
 * @brief 只读判断：现在能不能开出一个新的 SB（不改变任何状态）
 *
 * 用途：写序列跨 block 续写的准入——吃完当前 SB 之前必须确认下一个 SB 开得出来，
 * 否则序列会停在「有打开的设备序列、没有可提交的数据页」，门控挡住 GC、padding END
 * 也无页可分配（RISK-CACHE-15 的盘满例外会中止一条已回执的序列）。
 *
 * @param alloc 分配器
 * @param mode  与即将使用的 `alc_super_open_alloc_pair` 相同的策略
 *
 * @return true = 每个 plane 都能选到块（Free Pool，或 OP 池里有非 BAD 块）
 */
bool alc_super_can_open(const alc_t *alloc, alc_super_alloc_mode_e mode);

/**
 * @brief 按策略分配各 plane 当前页的 PBA
 *
 * @param alloc 分配器
 * @param mode  分配策略（温度流 + 选块方向 + OP 兜底）
 * @param out   出参：各 plane 页基址 PBA（PLANE_NUM 项）
 *
 * @return FTL_OK = 分配成功
 *         FTL_AGAIN = 某 plane 无块（瞬态，调用者触发 GC）
 *
 * @note `ALC_SUPER_ALLOC_COLD_HIGH_WEAR` 是静态 WL 目标：强制新开 cold SB、每 plane 取 erase_count 最大的空闲块、不允许 OP 兜底（OP 块擦除次数不受该策略控制）。
 */
ftl_status_e alc_super_open_alloc_pair(alc_t *alloc, alc_super_alloc_mode_e mode, DWORD out[PLANE_NUM]);

/**
 * @brief 静态 WL 启动前摘掉当前 cold SB
 *
 * 保证 WL 的第一次分配会**新开**一个 high-wear SB（否则会续写动态 WL 选出的
 * 低擦除块）。已写过 ⇒ 关闭入 Closed 列表；未写过 ⇒ 块归还 Free Pool。
 * 只有 `mig_wl_begin` 在 host 空闲时调用。
 *
 * @param alloc 分配器
 * @return true  —— 当前没有 cold SB，或已成功关闭/归还；
 *         false —— closed 列表满，关不掉：调用者必须放弃本次 WL。此时 cold SB
 *                  仍 open，下一个分配会继续写它（普通选块），高磨损目的块根本不
 *                  会打开，搬移退化为纯写放大。
 */
bool alc_super_prepare_wl_destination(alc_t *alloc);

/**
 * @brief 撤销**紧邻的**一次 alc_super_open_alloc_pair
 *
 * @param alloc 分配器
 * @param temperature FTL_TEMP_HOT / FTL_TEMP_COLD（须与那次 alloc 相同）。
 *
 * @note next_page == 0 时拒绝并留日志（BYTE 下溢会静默指向本块最后一页）。
 */
void alc_super_open_rewind_pair(alc_t *alloc, BYTE temperature);

/* ── 6. Super Block：Closed ── */

/**
 * @brief closed_sb_list 第 idx 项（借出，不转移所有权）。
 *
 * swap-remove 会让先前指针失效：跨调用持有请按值拷贝。
 *
 * @param alloc 分配器
 * @param idx   条目索引（调用者保证 idx < closed_sb_count）
 * @return 条目只读指针。
 */
static inline const super_block_t *alc_super_closed_get_at(const alc_t *alloc, WORD idx)
{
    return &alloc->closed_sb_list[idx];
}

/**
 * @brief 关闭指定温度的 Open SB
 *
 * @param alloc 分配器
 * @param temperature FTL_TEMP_HOT / FTL_TEMP_COLD。
 *
 * @return FTL_OK = 关闭成功
 *         FTL_ERROR = 该温度无 Open SB
 */
ftl_status_e alc_super_close(alc_t *alloc, BYTE temperature);

/**
 * @brief 掉电重建时直接追加一个 Closed SB 条目
 *
 * @param alloc        分配器
 * @param plane_blocks 各 plane 的块号（PLANE_NUM 项）
 * @param temperature  温度流。
 *
 * @return 新条目在 closed_sb_list 的索引
 *         FTL_BLOCK_NO_SB = 列表满（release 兜底），条目被拒绝
 */
WORD alc_super_closed_add(alc_t *alloc, const WORD plane_blocks[PLANE_NUM], BYTE temperature);

/**
 * @brief 从 Closed 列表移除条目（swap-remove）
 *
 * @param alloc 分配器
 * @param closed_idx 条目索引。
 */
void alc_super_closed_remove(alc_t *alloc, WORD closed_idx);

/**
 * @brief 从 Closed 条目摘除一个已擦除/已标坏的 plane 块
 *
 * GC/WL/predict 逐 plane 擦 victim。若擦完一个 plane 后 job 中止，旧条目仍引用
 * 已归还 Free Pool 的块；该块被重新分配并再次 Closed 后会被旧条目误认
 * （RISK-GC-04）。擦完即摘除，条目只在仍有未处理 plane 时存在。
 *
 * @param alloc  分配器
 * @param plane  plane 号
 * @param block  该 plane 内的块号
 * @return true = 条目已无任何 plane（条目被移除）
 *         false = 仍有其他 plane 或未找到
 */
bool alc_super_closed_drop_plane(alc_t *alloc, BYTE plane, WORD block);

/**
 * @brief 按全部 plane 块号在 Closed 列表定位条目
 *
 * @param alloc 分配器
 * @param sb 目标条目
 * @param out_idx 出参索引。
 *
 * @return true = 找到
 *         false = 不在列表。
 */
bool alc_super_closed_find(const alc_t *alloc, const super_block_t *sb, WORD *out_idx);

typedef struct
{
    WORD  valid_cw;    /* 各 plane valid_count 之和 */
    WORD  plane_count; /* 实际 plane 数 */
    DWORD erase_max;   /* 最大 erase_count */
    DWORD erase_sum;   /* erase_count 之和 */
    DWORD pe_min;      /* 最小 last_pe_ts；无 plane 时 UINT32_MAX */
    DWORD read_sum;    /* read_count 之和 */
} alc_super_stats_t;

/**
 * @brief 计算一个 Super Block 的聚合值
 *
 * @param alloc 分配器
 * @param sb 目标条目
 * @param out 出参。
 */
void alc_super_get_stats(const alc_t *alloc, const super_block_t *sb, alc_super_stats_t *out);

/* ── 7. BST ── */

/**
 * @brief 记录一次成功的擦除
 *
 * @param alloc 分配器
 * @param gblock 全局块号
 * @param now 当前时间戳。
 */
void alc_bst_note_erase(alc_t *alloc, WORD gblock, DWORD now);

typedef struct
{
    DWORD erase_min;  /* 最小擦除次数；无块时为 0 */
    DWORD erase_max;  /* 最大擦除次数；无块时为 0 */
    DWORD erase_sum;  /* 擦除次数之和 */
    DWORD max_pe_ts;  /* 最大 last_pe_ts */
    WORD  good_count; /* 已归属好块数 */
} alc_bst_erase_stats_t;

/**
 * @brief 已归属好块的磨损统计（一次扫描）
 *
 * @param alloc 分配器
 * @param out 出参。
 */
void alc_bst_get_erase_stats(const alc_t *alloc, alc_bst_erase_stats_t *out);

/**
 * @brief 从 Checkpoint 磨损快照恢复一个块的磨损/ECC 状态
 *
 * @param alloc 分配器
 * @param gblock 全局块号
 * @param wear 快照。
 */
void alc_bst_set_wear(alc_t *alloc, WORD gblock, const bst_wear_t *wear);

/**
 * @brief 读取一个块的磨损/ECC 快照
 *
 * @param alloc 分配器
 * @param gblock 全局块号
 * @param out 输出快照（由唯一调用者 checkpoint.c 传本地地址；模块内部不重复守卫）。
 */
static inline void alc_bst_get_wear(const alc_t *alloc, WORD gblock, bst_wear_t *out)
{
    const bst_block_t *blk = &alloc->blocks[gblock];

    out->erase_count = blk->erase_count;
    out->read_count  = blk->read_count;
    out->rr          = blk->rr;
}

/* ── 8. BBT ── */

/**
 * @brief BBT 状态谓词
 *
 * 纯 getter：不检查值域、不隐藏数组，但跨模块读的是 allocator 的 BBT 子状态，
 * 按 RISK-ALLOC-03 的判定保留访问器；放头文件 static inline，避免每次调用跨 TU。
 *
 * @param alloc 分配器
 * @return true = 最新 BBT record 带 FORMAT_IN_PROGRESS 标志。
 */
static inline bool alc_bbt_is_format_in_progress(const alc_t *alloc)
{
    return alloc->bbt.format_in_progress;
}

/**
 * @brief 最新 BBT record 是否带 REBUILD_REQUIRED 标志
 *
 * @param alloc 分配器
 * @return true = 需要重建。
 */
static inline bool alc_bbt_is_rebuild_required(const alc_t *alloc)
{
    return alloc->bbt.rebuild_required;
}

/* ── 9. Trace ── */

/**
 * @brief 记录一次 host 写/程序失败到飞行记录器（环形覆盖）
 * @param alloc 分配器
 * @param lba   host LBA
 * @param pba   目标 PBA
 * @param kind  0 = 写确认成功；1 = program fail
 */
void alc_trace_record(alc_t *alloc, DWORD lba, DWORD pba, alc_trace_kind_e kind);

/**
 * @brief 打印与 (lba, pba) 相关的最近轨迹（首次 UNC/program-fail 时调用）
 *
 * 最多打印 8 条匹配项；同一 PBA 出现多次时标注 DUP（二次编程判据）。
 * 只读，不分配资源。
 *
 * @param alloc 分配器
 * @param lba   待匹配的 host LBA
 * @param pba   待匹配的目标 PBA
 */
void alc_trace_dump(const alc_t *alloc, DWORD lba, DWORD pba);

#endif /* FTL_ALLOCATOR_H */

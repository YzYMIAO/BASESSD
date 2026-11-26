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
 *   ④ 指针 / 索引实参由调用者保证；实例 allocator 的绑定由 ftl_contract_verify 在 boot 期证明，
 *      运行期入口不逐次复检；init/bind 与 release 关键边界仍用 ASSERT。仅池空 / 幂等早退返回错误码。
 */

#include "assertion.h" /* ASSERT（inline 访问器与地址展开的入参检查） */
#include "bst.h"       /* bst_block_s, FTL_BLOCK_TOTAL, ftl_gblock */
#include "ftl_capacity.h"
#include "ftl_config.h" /* DWORD, pba_make */
#include "ftl_type.h"   /* ftl_status_e, ftl_topo_s */
#include "log.h"
#include "nand_config.h" /* PLANE_NUM, PAGES_PER_BLOCK, BLOCKS_PER_PLANE */

#include <stdbool.h>
#include <stdint.h>

/* ── 温度流 ── */
#define FTL_TEMP_HOT 0U /* 用户 IO */

#define FTL_TEMP_COLD 1U /* GC 搬移 */

#define FTL_TEMP_NUM 2U

/* ── Free Pool 三级水位 ──
 * 一个已擦除池 + 软水位，不是三个分区：≥32 正常；8<free<32 后台 GC；≤8 紧急。
 * 32 ≈ 实例数据块（3392）的 1%，给 GC/写放大留出提前量；8 是池见底的最后手段。
 * 这两个值是本仓经验值，不是行业标准；调整需同时看写放大、稳态吞吐与 host 延迟。 */
#define FREE_POOL_TARGET (32U)

#define FREE_POOL_EMERGENCY (8U)

/* 单 plane 空闲水位（本仓自拟）。成对分配要求「每个 plane 各一块」——只看总水位会出现
 * 「free=264 但一侧为 0」的分配失败（板端 2026-10-31）。任一 plane 低于
 * 该值即视为饥饿，触发 GC 并让 victim 选择优先回收该 plane。
 * 取 4：够一次成对分配 + GC 目的页对 + CP/journal 的短时余量；经验值，不是行业标准。 */
#define FREE_POOL_PLANE_LOW (4U)

/* ── OP / Spare 保留池（按 plane）── */

/* 数组维度钳到 ≥1：FTL_OP_PERCENT=0 时 [PLANE_NUM][0] 非法。 */

/* ── Closed Super Block 列表容量 ──
 * = 全部物理块数：重建时每写过一块最多建一条，取更小会溢出
 * （重建 ASSERT；运行时静默漏账）。 */
#define SUPER_BLOCK_LIST_MAX (FTL_BLOCK_TOTAL)

/* ── 两个同值哨兵，语义不同（C11：同值 ≠ 同一坐标）──
 * FTL_BLOCK_NO_PLANE = plane 内块号槽位的「无块」（存储态）；
 * FTL_GBLOCK_INVALID = gblock 坐标的「选块失败」（瞬时返回值）。 */
#define FTL_BLOCK_NO_PLANE (UINT16_MAX)

#define FTL_GBLOCK_INVALID (UINT16_MAX)

/* BBT 后台持久化「起新 job」的节拍；在飞续跑不受此限（bbt_handle 先处理在飞态）。
 * 由 timer 任务置 flush_due，bbt_handle 消费。 */
#define BBT_FLUSH_INTERVAL_MS (1000U)

/* ── 写轨迹飞行记录器（诊断）──
 * 最近 N 次 host 写/程序失败的 (lba, pba, plane/block/page/cw)。平时不打印；
 * 首次 UNC / program-fail 时 dump 与失败 LBA/PBA 相关的条目：
 *   · 同一 PBA 出现两次 ⇒ 同页二次编程；
 *   · 只有一次 ⇒ 写后读坏（时序/介质）；
 *   · kind=1 ⇒ 该次 program 失败。
 * 64 × 20B ≈ 1.3 KB，放 alc_s（AXI RAM）。 */
#define ALC_TRACE_DEPTH (64U)

/* 分配策略：温度流 + 选块方向 + 是否允许 OP 兜底。
 * 合在一个闭枚举里，避免 `temperature` 与 `high_wear` 组合出非法状态。 */
typedef enum ALC_SUPER_ALLOCATOR_MODE_E
{
    ALC_SUPER_ALLOC_HOT            = 0, /* HOT SB（host 数据）；取擦除最少；允许 OP 兜底 */
    ALC_SUPER_ALLOC_COLD           = 1, /* COLD SB（GC/WL 目的）；取擦除最少；允许 OP 兜底 */
    ALC_SUPER_ALLOC_COLD_HIGH_WEAR = 2, /* COLD SB；取擦除最多；禁止 OP 兜底（静态 WL） */
} alc_super_alloc_mode_e;

/* ── Super Block ──
 * 跨 plane 的逻辑写入单元；退化条目可只含一个 plane（另一 plane 记哨兵）。 */
typedef struct SUPER_BLOCK_S
{
    WORD plane_blocks[PLANE_NUM]; /* 各 plane 的块号；可含 FTL_BLOCK_NO_PLANE */
    BYTE next_page;               /* 当前写入页（0..PAGES_PER_BLOCK） */
    BYTE temperature;             /* FTL_TEMP_HOT / FTL_TEMP_COLD */
    BOOL is_open;                 /* true = 仍在写；false = 已关闭 */
    BYTE page0_programmed;        /* bit p = plane p 的 page0 写命令已成功（异步写屏障） */
} super_block_s;

/* per-plane 池表：用数组 typedef 保持 `name[plane][index]` 的二维语义，
 * 同时避免 clang-format 把 pointer-to-array 声明拆成两行。 */
typedef WORD alc_op_block_arr_t[FTL_OP_POOL_CAP];

typedef WORD alc_spare_block_arr_t[FTL_SPARE_POOL_PER_PLANE];

/* ── FTL 表的引用 ──
 * 五张大数组由**本模块**（allocator.c）定义并各自选内存域（SRAM12 section 属性），
 * 本结构只持有它们的指针；表本体不在本结构里，故放置不受本类型约束。
 * 装配入口 alc_bind_workspace() 按 die 填引用，ftl_storage_s 只做内部传递。 */
typedef struct FTL_TABLE_REFS_S
{
    bst_block_s           *blocks;         /* BST */
    WORD                  *free_blocks;    /* Free Pool */
    alc_op_block_arr_t    *op_blocks;      /* OP 预留池 */
    alc_spare_block_arr_t *spare_blocks;   /* Spare 池 */
    super_block_s         *closed_sb_list; /* Closed 列表 */
} ftl_table_refs_s;

/* ── BBT 持久化状态（per-instance）──
 * 放在本头：bbt.c 使用，且 bbt.h 已 include 本文件，反向包含会成环。 */
typedef enum BBT_HANDLE_PHASE_E
{
    BBT_JOB_IDLE = 0, /* 无 job */
    BBT_JOB_ERASE,    /* 换块擦在飞 */
    BBT_JOB_WRITE,    /* record 写在飞 */
} bbt_handle_phase_e;

typedef struct BBT_HANDLE_S
{
    bbt_handle_phase_e phase;     /* 异步持久化状态机 */
    BOOL               waiting;   /* true = 有命令在飞 */
    DWORD              dirty_gen; /* 本条 record 组 header 时的 dirty 代际快照 */
    log_heartbeat_s    hb;        /* job 心跳（start/log + active） */
} bbt_handle_s;

typedef struct BBT_STATE_S
{
    DWORD        sequence;           /* 本池单调序号 */
    WORD         write_idx;          /* 池内下一个待写槽 */
    BOOL         cursor_valid;       /* 游标是否已建立 */
    BOOL         dirty;              /* 是否有未写回变更 */
    BOOL         format_in_progress; /* 最新 BBT record 带 FORMAT_IN_PROGRESS 标志 */
    BOOL         rebuild_required;   /* 最新 BBT record 带 REBUILD_REQUIRED 标志（本仓自拟） */
    DWORD        dirty_gen;          /* dirty 每次置位 +1；检测写飞行期间的新变更 */
    BYTE         flush_due;          /* 1 = timer 节拍到，允许起新 job；在飞续跑不看它 */
    bbt_handle_s handle;             /* 异步持久化 handle */
} bbt_state_s;

typedef enum ALC_TRACE_KIND_E
{
    ALC_TRACE_WRITE_OK     = 0, /* 写确认成功 */
    ALC_TRACE_PROGRAM_FAIL = 1, /* program fail */
    ALC_TRACE_GC_WRITE     = 2, /* GC 搬移写 */
} alc_trace_kind_e;

typedef struct ALC_TRACE_S
{
    DWORD            seq;   /* 记录序号（单调） */
    DWORD            lba;   /* host LBA */
    DWORD            pba;   /* 目标 PBA（cw 位保留） */
    BYTE             plane; /* 以下为 pba 解码，便于日志直接打印 */
    WORD             block;
    BYTE             page;
    BYTE             cw;
    alc_trace_kind_e kind; /* 写确认成功 / program fail / GC 写 */
} alc_trace_s;

/* ── Allocator 上下文 ── */
typedef struct ALC_ALLOCATOR_S
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
    alc_op_block_arr_t    *op_blocks;
    WORD                   op_count[PLANE_NUM];
    alc_spare_block_arr_t *spare_blocks;
    WORD                   spare_count[PLANE_NUM];

    /* Open Super Block */
    super_block_s hot_sb;
    super_block_s cold_sb;

    /* Closed 列表 */
    super_block_s *closed_sb_list;
    WORD           closed_sb_count;

    /* BST */
    bst_block_s *blocks;
    WORD         bad_count;

    /* 元数据池游标 */
    bbt_state_s bbt;

    /* 固定元数据分区候选池（md_pools_s，定义在 metadata.h）。
     * 由 ftl_init 绑定到本实例的 metadata_dir；只读，唯一写点是
     * md_pool_build_all()。跨层前向声明避免 allocator.h ↔ metadata.h 环。 */
    struct MD_POOLS_S *pools;

    /* Directory（只读回指）：bbt_mark_bad 需要判断「从 OP 借用的 L2P 日志块」
     * 也是元数据块（不在固定保留区但 l2p_live/当前日志块指向它）。 */
    struct MD_DIRECTORY_CACHE_S *md_dir;

    /* 写轨迹飞行记录器（见 ALC_TRACE_DEPTH） */
    alc_trace_s trace[ALC_TRACE_DEPTH];
    DWORD       trace_seq;
} alc_s;

/* ── super block：open ── */

/* PBA 相对指定温度 Open SB 的位置/状态。 */
typedef enum ALC_SUPER_PBA_STATE_E
{
    ALC_SUPER_PBA_NOT_OPEN           = 0, /* 不属于当前 Open SB（旧 SB / 非法 PBA） */
    ALC_SUPER_PBA_OPEN_PAGE0_PENDING = 1, /* 属于 Open SB，但该 plane 的 page0 未成功 */
    ALC_SUPER_PBA_OPEN_PAGE0_DONE    = 2, /* 属于 Open SB，且该 plane 的 page0 已成功 */
} alc_super_pba_state_e;

typedef struct ALC_SUPER_STATISTICS_S
{
    WORD  valid_cw;    /* 各 plane valid_count 之和 */
    WORD  plane_count; /* 实际 plane 数 */
    DWORD erase_max;   /* 最大 erase_count */
    DWORD erase_sum;   /* erase_count 之和 */
    DWORD pe_min;      /* 最小 last_pe_ts；无 plane 时 UINT32_MAX */
    DWORD read_sum;    /* read_count 之和 */
} alc_super_stats_s;

typedef struct ALC_BST_ERASE_STATISTICS_S
{
    DWORD erase_min;  /* 最小擦除次数；无块时为 0 */
    DWORD erase_max;  /* 最大擦除次数；无块时为 0 */
    DWORD erase_sum;  /* 擦除次数之和 */
    DWORD max_pe_ts;  /* 最大 last_pe_ts */
    WORD  good_count; /* 已归属好块数 */
} alc_bst_erase_stats_s;

/* ── 地址 / 拓扑辅助 ── */

/**
 * @brief plane 是否持有物理块（退化条目在另一 plane 记哨兵）。
 *
 * @param sb    Super Block
 * @param plane plane 号
 * @return true = 该 plane 有块
 *         false = 槽位为 FTL_BLOCK_NO_PLANE
 */
static inline BOOL super_block_has_plane(const super_block_s *sb, BYTE plane)
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
static inline DWORD super_block_page_pba(ftl_topo_s topo, const super_block_s *sb, BYTE plane, BYTE page)
{
    return pba_make(topo.ch, topo.ce, topo.lun, sb->plane_blocks[plane], plane, page, 0U);
}

/**
 * @brief 本实例 ch/ce/lun → 标准拓扑（只有 alloc 在手时用）。
 *
 * @param alloc 分配器
 * @return 本实例的 ch/ce/lun 拓扑。
 */
static inline ftl_topo_s alc_topo(const alc_s *alloc)
{
    ftl_topo_s topo;

    topo.ch  = alloc->ch;
    topo.ce  = alloc->ce;
    topo.lun = alloc->lun;
    return topo;
}

/**
 * @brief 各 plane 的 Free Pool 块数（O(1)，由 push/remove 闸门维护）
 *
 * 用途：成对分配的单 plane 水位判定、GC victim 的 plane 平衡偏好。
 *
 * @param alloc 分配器
 * @param plane 目标 plane
 * @return 该 plane 的空闲块数（0..BLOCKS_PER_PLANE）
 */
static inline WORD alc_free_get_count(const alc_s *alloc, BYTE plane)
{
    ASSERT(plane < PLANE_NUM);
    return alloc->free_plane_count[plane];
}

/**
 * @brief 最饥渴 plane 的空闲块数（成对分配真正可用的余量）
 *
 * 总 `free_count` 不能代表成对分配能力——一侧为空即无法分配。
 * 诊断 / 水位判据都看这个值；只用它做观测，不替代 per-plane 判定。
 *
 * @param alloc 分配器
 * @return 各 plane 空闲块数的最小值
 */
static inline WORD alc_free_min_plane_count(const alc_s *alloc)
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

/* ── op pool ── */

/**
 * @brief 各 plane OP 池剩余块数（调用者保证 plane < PLANE_NUM）。
 *
 * @param alloc 分配器
 * @param plane 目标 plane
 * @return 该 plane 的 OP 池剩余块数。
 */
static inline WORD alc_op_get_count(const alc_s *alloc, BYTE plane)
{
    return alloc->op_count[plane];
}

/**
 * @brief OP 池元素只读访问；调用者保证 plane < PLANE_NUM、index < op_count[plane]
 *        （偏离 H14：现有唯一调用链已保证，不另加断言）。
 *
 * @param alloc 分配器
 * @param plane 目标 plane
 * @param index 池内下标
 * @return 该槽位的 gblock。
 */
static inline WORD alc_op_get_at(const alc_s *alloc, BYTE plane, WORD index)
{
    return alloc->op_blocks[plane][index];
}

/* ── spare pool ── */

/**
 * @brief 各 plane Spare 池剩余块数（调用者保证 plane < PLANE_NUM）。
 *
 * @param alloc 分配器
 * @param plane 目标 plane
 * @return 该 plane 的 Spare 池剩余块数。
 */
static inline WORD alc_spare_get_count(const alc_s *alloc, BYTE plane)
{
    return alloc->spare_count[plane];
}

/**
 * @brief Spare 池元素只读访问；调用者保证 plane < PLANE_NUM、index < spare_count[plane]
 *        （偏离 H14：现有唯一调用链已保证，不另加断言）。
 *
 * @param alloc 分配器
 * @param plane 目标 plane
 * @param index 池内下标
 * @return 该槽位的 gblock。
 */
static inline WORD alc_spare_get_at(const alc_s *alloc, BYTE plane, WORD index)
{
    return alloc->spare_blocks[plane][index];
}

/* ── super block：closed ── */

/**
 * @brief closed_sb_list 第 idx 项（借出，不转移所有权）。
 *
 * swap-remove 会让先前指针失效：跨调用持有请按值拷贝。
 *
 * @param alloc 分配器
 * @param idx   条目索引（调用者保证 idx < closed_sb_count）
 * @return 条目只读指针。
 */
static inline const super_block_s *alc_super_closed_get_at(const alc_s *alloc, WORD idx)
{
    return &alloc->closed_sb_list[idx];
}

/**
 * @brief 读取一个块的磨损/ECC 快照
 *
 * @param alloc 分配器
 * @param gblock 全局块号
 * @param out 输出快照（由唯一调用者 checkpoint.c 传本地地址；模块内部不重复守卫）。
 */
static inline void alc_bst_get_wear(const alc_s *alloc, WORD gblock, bst_wear_s *out)
{
    const bst_block_s *blk = &alloc->blocks[gblock];

    out->erase_count = blk->erase_count;
    out->read_count  = blk->read_count;
    out->rr          = blk->rr;
}

/* ── bbt ── */

/**
 * @brief BBT 状态谓词
 *
 * 纯 getter：不检查值域、不隐藏数组，但跨模块读的是 allocator 的 BBT 子状态，
 * 按 的判定保留访问器；放头文件 static inline，避免每次调用跨 TU。
 *
 * @param alloc 分配器
 * @return true = 最新 BBT record 带 FORMAT_IN_PROGRESS 标志。
 */
static inline BOOL alc_bbt_is_format_in_progress(const alc_s *alloc)
{
    return alloc->bbt.format_in_progress;
}

/**
 * @brief 最新 BBT record 是否带 REBUILD_REQUIRED 标志
 *
 * @param alloc 分配器
 * @return true = 需要重建。
 */
static inline BOOL alc_bbt_is_rebuild_required(const alc_s *alloc)
{
    return alloc->bbt.rebuild_required;
}

/* ── bind ── */

/**
 * @brief 绑定本实例的五张表（workspace 的唯一定义点在本模块）。
 *
 * @note 表定义在 allocator.c 并以 SRAM12_DATA 放置；启动代码不清零，
 *       调用者（ftl_init）负责逐表 memset。
 */
void alc_bind_workspace(BYTE die, ftl_table_refs_s *refs);

/* ── pool ── */

/** @brief 对 per-plane 计数数组求和。 */
WORD alc_pool_counts_sum(const WORD *counts, BYTE count);

/* ── free / op / spare / super ── */

/** @brief Spare 耗尽时的第二道坏块替换：从 OP 池取一块补 Free。 */
WORD alc_free_refill_from_op(alc_s *alloc, BYTE plane);

/** @brief 归还一个已擦除块到 Free Pool（唯一入池点）。 */
void alc_free_push(alc_s *alloc, WORD gblock);

/** @brief 从 Free Pool 移除一个块（唯一出池点；出池即置 NONE）。 */
void alc_free_remove(alc_s *alloc, WORD gblock);

/** @brief Free Pool 中「各 plane 最大擦除次数」的最小值（静态 WL 触发判据）。 */
DWORD alc_free_erase_limit(const alc_s *alloc);

/** @brief OP 池入池。 */
BOOL alc_op_push(alc_s *alloc, BYTE plane, WORD gblock);

/** @brief 从 OP 池借一块作动态 L2P 日志块（固定日志区满时的有界溢出）。 */
WORD alc_op_alloc_log_block(alc_s *alloc, BYTE plane);

/** @brief Spare 池入池。 */
BOOL alc_spare_push(alc_s *alloc, BYTE plane, WORD gblock);

/** @brief Spare 池出池（LIFO）。 */
BOOL alc_spare_pop(alc_s *alloc, BYTE plane, WORD *out_gblock);

/** @brief 从 Free Pool 补 Spare 池到目标（逐 plane）。 */
BOOL alc_spare_refill_from_free(alc_s *alloc, BYTE plane);

/** @brief 当前 Open SB 还能连续写多少页对。 */
BYTE alc_super_open_room(const alc_s *alloc, BYTE temperature);

/**
 * @brief cache 写序列专用的 Open SB 余量（含「新块由本序列自己打开」的语义）。
 *
 * @note 只有**同一 cache 序列**可以传 `page0_from_seq == true`；独立程序者
 *       （wbuf / GC / WL）仍必须用 `alc_super_open_pba_state` 的 page0 屏障。
 */
BYTE alc_super_open_room_seq(const alc_s *alloc, BYTE temperature, BOOL page0_from_seq);

/** @brief 记录某个 Open SB 的 page0 编程成功（异步写屏障）。 */
void alc_super_open_note_page_programmed(alc_s *alloc, BYTE temperature, DWORD pba);

/** @brief PBA 相对指定温度 Open SB 的 page0 状态。 */
alc_super_pba_state_e alc_super_open_pba_state(const alc_s *alloc, BYTE temperature, DWORD pba);

/** @brief 只读判断：现在能不能开出一个新的 SB（不改变任何状态）。 */
BOOL alc_super_is_openable(const alc_s *alloc, alc_super_alloc_mode_e mode);

/**
 * @brief 按策略分配各 plane 当前页的 PBA。
 *
 * @note `ALC_SUPER_ALLOC_COLD_HIGH_WEAR` 是静态 WL 目标：强制新开 cold SB、
 *       每 plane 取 erase_count 最大的空闲块、不允许 OP 兜底。
 */
ftl_status_e alc_super_open_alloc_pair(alc_s *alloc, alc_super_alloc_mode_e mode, DWORD out[PLANE_NUM]);

/** @brief 静态 WL 启动前摘掉当前 cold SB。 */
BOOL alc_super_prepare_wl_destination(alc_s *alloc);

/** @brief 撤销紧邻的一次 alc_super_open_alloc_pair。 */
void alc_super_open_rewind_pair(alc_s *alloc, BYTE temperature);

/** @brief 关闭指定温度的 Open SB。 */
ftl_status_e alc_super_close(alc_s *alloc, BYTE temperature);

/**
 * @brief 掉电重建时直接追加一个 Closed SB 条目。
 *
 * @note 全部 plane 都不可用时**不追加条目**，返回当前 count；调用者只应判
 *       `== FTL_BLOCK_NO_SB`，不得把它当有效下标使用。
 */
WORD alc_super_closed_add(alc_s *alloc, const WORD plane_blocks[PLANE_NUM], BYTE temperature);

/** @brief 从 Closed 列表移除条目（swap-remove）。 */
void alc_super_closed_remove(alc_s *alloc, WORD closed_idx);

/** @brief 从 Closed 条目摘除一个已擦除/已标坏的 plane 块。 */
BOOL alc_super_closed_drop_plane(alc_s *alloc, BYTE plane, WORD block);

/** @brief 按全部 plane 块号在 Closed 列表定位条目。 */
BOOL alc_super_closed_find(const alc_s *alloc, const super_block_s *sb, WORD *out_idx);

/** @brief 计算一个 Super Block 的聚合值。 */
void alc_super_get_stats(const alc_s *alloc, const super_block_s *sb, alc_super_stats_s *out);

/* ── bst ── */

/** @brief 记录一次成功的擦除。 */
void alc_bst_note_erase(alc_s *alloc, WORD gblock, DWORD now);

/** @brief 已归属好块的磨损统计（一次扫描）。 */
void alc_bst_get_erase_stats(const alc_s *alloc, alc_bst_erase_stats_s *out);

/** @brief 从 Checkpoint 磨损快照恢复一个块的磨损/ECC 状态。 */
void alc_bst_set_wear(alc_s *alloc, WORD gblock, const bst_wear_s *wear);

/* ── trace ── */

/** @brief 记录一次 host 写/程序失败到飞行记录器（环形覆盖）。 */
void alc_trace_record(alc_s *alloc, DWORD lba, DWORD pba, alc_trace_kind_e kind);

/** @brief 打印与 (lba, pba) 相关的最近轨迹（首次 UNC/program-fail 时调用）。 */
void alc_trace_dump(const alc_s *alloc, DWORD lba, DWORD pba);

/* ── init ── */

/** @brief 初始化分配器。 */
void alc_init(alc_s *alloc, ftl_topo_s topo, const ftl_table_refs_s *refs);

#endif /* FTL_ALLOCATOR_H */

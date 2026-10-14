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
 *   allocator_alloc_pair() 返回 nand_pba_t（FTL 物理地址：CH/CE/LUN 拓扑 + 块/页/plane/CW）。
 *   PBA 的 page 字段由 allocator 管理，block/plane 从 Free Pool 分配。
 */

#include "common/assertion.h" /* ASSERT（退化单 plane 条目展开地址是编码错误） */
#include "ftl/bst.h"          /* ftl_block_t, FTL_BLOCK_TOTAL, ftl_gblock, ftl_block_is_bad */
#include "ftl/ftl_config.h"   /* nand_pba_t, pba_make, NAND_PBA_INVALID */
#include "ftl/ftl_type.h"     /* ftl_status_t, ftl_topology_t */
#include "nand/nand_config.h" /* PLANE_NUM, PAGES_PER_BLOCK, BLOCKS_PER_PLANE */

#include <stdint.h>

/* ── 温度等级 ── */
#define FTL_TEMP_HOT  0U /* 用户 IO 数据 */
#define FTL_TEMP_COLD 1U /* GC 搬移 / 元数据 */
#define FTL_TEMP_NUM  2U /* 温度等级数 */

/* ── Free Pool 三级水位（P1-3 定案；设计 §3.7）──
 *
 * ⚠ **三级是水位，不是分区**。池只有**一个**（`free_blocks[]`）：搬移引擎的擦除分支
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
#define FREE_POOL_TARGET    (32U) /* 后台 GC 水位（IDLE 补池目标） */
#define FREE_POOL_EMERGENCY (8U)  /* 水位②/③分界：低于等于此值转紧急 GC */
#define FTL_OP_PERCENT      (10U) /* OP 预留百分比（编译期默认；**按 plane 计**） */

/* 每 plane OP 块数 = 2048 × 10% = 204（floor）⇒ 总 408。
 * 预留按 **super block 粒度**（每 plane 各一份），因为写入/擦除/GC 的最小单元是跨双 plane 的
 * super block——只留单 plane 的 OP 对另一 plane 的池耗尽毫无帮助。 */
#define FTL_OP_BLOCKS_PER_PLANE ((BLOCKS_PER_PLANE * FTL_OP_PERCENT) / 100U)

/* 数组维度钳到 ≥1：FTL_OP_PERCENT=0（设计允许"禁用 OP"）会让 [PLANE_NUM][0] 非法。
 * 填池/消费循环一律用 FTL_OP_BLOCKS_PER_PLANE（=0 ⇒ 不预留），本宏只用于定数组尺寸。 */
#define FTL_OP_POOL_CAP          ((FTL_OP_BLOCKS_PER_PLANE) > 0U ? (FTL_OP_BLOCKS_PER_PLANE) : 1U)
#define FTL_SPARE_POOL_PER_PLANE (50U) /* 每 plane 坏块替换池大小（总 100） */

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

/* ── FTL 自持表（P2-5）──
 *
 * 大数组**归 FTL 实例所有**：由 `ftl_context_t.tables` 内含，HIL（main.c）不再声明数组、
 * 也不再传指针。与主流同构——OpenSSD 用全局数组、SPDK 把表放进 `spdk_ftl_dev`；
 * 表的所有权属于 FTL 本体，不属于调用者。
 *
 * 全部放**一处**（而不是各散在 allocator / bbt / gc 里）是为了让内存预算**单点可见**：
 * `src/main.c` 的 `_Static_assert` 只看 `sizeof(ftl_context_t)` 一项即可挡住维度扩展。
 *
 * 定义于此（而非 ftl.h）：依赖 super_block_t（本文件定义），放 ftl.h 会形成
 * ftl.h ↔ allocator.h 循环 include。 */
typedef struct
{
    ftl_block_t   blocks[FTL_BLOCK_TOTAL];                           /* BST：per-Block 状态（erase_count / valid_count / state / sb_id / rr） */
    uint16_t      free_blocks[FTL_BLOCK_TOTAL];                      /* Free Pool（全局块号；三级水位见文件头） */
    uint16_t      op_blocks[PLANE_NUM][FTL_OP_POOL_CAP];             /* OP 预留池（每 plane 独立；GC 缓冲池，容量不动） */
    uint16_t      spare_blocks[PLANE_NUM][FTL_SPARE_POOL_PER_PLANE]; /* Spare Pool（每 plane 独立；坏块替换，只出不进） */
    super_block_t closed_sb_list[SUPER_BLOCK_LIST_MAX];              /* Closed Super Block 列表（等待 GC） */
} ftl_tables_t;

/* 该 plane 是否持有物理块（Policy B：Closed 条目允许退化单 plane，另一 plane 记哨兵）。
 * 凡按 plane 展开地址 / 索引 BST / 擦除的读者都要先判——哨兵不是合法块号。 */
static inline uint8_t super_block_has_plane(const super_block_t *sb, uint8_t plane)
{
    return (sb->plane_blocks[plane] != FTL_BLOCK_NO_PLANE) ? 1U : 0U;
}

/* 返回 Super Block 中指定 Plane/Page 的 PBA（用户数据使用 FTL_DATA_CH/CE） */
static inline nand_pba_t super_block_page_pba(uint8_t ch, uint8_t ce, const super_block_t *sb, uint8_t plane, uint8_t page)
{
    /* 退化单 plane 条目的哨兵 plane 不可展开（plane_blocks[plane] 是哨兵，非块号） */
    ASSERT(super_block_has_plane(sb, plane) != 0U);

    /* ch/ce 取形参（调用点自带的 FTL_DATA_CH/CE）；lun 取 sb->topo —— sb->topo 就是
     * 本实例的 topo（见 allocator.c 的「绑定真实拓扑」）。cw = 0 = 整页（页级基址）。 */
    return pba_make(ch, ce, sb->topo.lun, sb->plane_blocks[plane], plane, page, 0U);
}

/* ── 元数据池游标（序 4：per-instance）──
 *
 * BBT 池 / Wear 日志各有自己的 sequence 与写游标（环形轮转，开机由各自的 load/init
 * 恢复）。**原先是 bbt.c / wear.c 的 file-scope static** —— 单实例下无害，多实例
 * （多 LUN，`g_ftl[ch][ce][lun]`）下所有实例共享一份游标/序号，互相覆盖 ⇒ 池写错槽。
 * 收进 ftl_allocator_t ⇒ 每实例一份。
 *
 * 放这里（而非各自的 bbt.h / wear.h）的两个理由：
 *   ① 这两个模块的**全部**接口都以 `ftl_allocator_t *` 为首参（状态随分配器走最自然）；
 *      而 bbt.h / wear.h 已 include 本文件，反向包含会成环。
 *   ② 语义上同属「块账本」——BBT = 坏块图，Wear = 每块擦除/读计数，与 BST 同一维度。
 *
 * ⚠ 两者都是**软状态**：掉电只丢「最后一次落盘之后」的增量，不丢用户数据（见 wear.h 头注）。
 */
/* ── BBT 持久化 job（v0.75 阶段 7：异步化）──
 *
 * 落盘从「一次调用内同步写完」改为 job：`bbt_kick` 起步（提交第一条命令即返回），
 * 完成由 g_op_service[FTL_OP_BBT]（ftl_op_bbt）按 phase 收尾并提交下一条。
 * 状态与池游标同处（per-instance）。 */
typedef enum
{
    BBT_JOB_IDLE = 0, /* 无 job */
    BBT_JOB_ERASE,    /* 换块擦在飞（池内轮转到的下一块） */
    BBT_JOB_WRITE,    /* record 写在飞 */
} bbt_job_phase_t;

typedef struct
{
    uint8_t phase;   /* bbt_job_phase_t */
    uint8_t waiting; /* 1 = 有一条本 job 的命令在飞；0 = 待续跑（提交被背压拒） */

    /* 心跳基准（见 ftl_job.h）：job 在飞期间每满 1s 打一行 phase/age */
    uint32_t start_ms;
    uint32_t log_ms;
} bbt_job_t;

typedef struct
{
    uint32_t  sequence;     /* 本池自己的单调序号（最新有效 record） */
    uint16_t  write_idx;    /* 池内下一个待写槽（环形） */
    uint8_t   cursor_valid; /* 游标是否已建立；未建立则首次落盘先扫池定位 */
    uint8_t   dirty;        /* 是否有未写回 NAND 的变更 */
    bbt_job_t job;          /* 阶段 7：异步持久化 job */
} bbt_state_t;

/* ── Wear 持久化 job（v0.75 阶段 7：异步化）──
 *
 * 一批落盘 = 1~25 个日志页（增量 1~4 页 / compact 全量 25 页）+ 可能的整块擦。
 * `wear_kick` 起步，完成由 g_op_service[FTL_OP_WEAR]（ftl_op_wear）收尾并提交下一页。 */
typedef enum
{
    WEAR_JOB_IDLE = 0, /* 无 job */
    WEAR_JOB_ERASE,    /* compact 前的整块擦在飞 */
    WEAR_JOB_WRITE,    /* 日志页写在飞 */
} wear_job_phase_t;

typedef struct
{
    uint8_t  phase;        /* wear_job_phase_t */
    uint8_t  waiting;      /* 1 = 有一条本 job 的命令在飞；0 = 待续跑（提交被背压拒） */
    uint8_t  dirty_only;   /* 1 = 增量（只写快照里的脏块）；0 = compact 后的全量快照 */
    uint16_t cursor;       /* 下一个待考察的 gblock（= FTL_BLOCK_TOTAL 表示本批走完） */
    uint32_t flush_ms;     /* 起点时刻（收尾写回节拍基准，与旧 wear_flush 同位置） */
    uint32_t flush_writes; /* 起点 total_writes（同上） */

    /* 心跳基准（见 ftl_job.h）：job 在飞期间每满 1s 打一行 phase/cursor/age */
    uint32_t start_ms;
    uint32_t log_ms;
} wear_job_t;

typedef struct
{
    uint32_t sequence;                    /* 最新已写页的 sequence */
    uint16_t next_page;                   /* 下一个可写页（= 已回放的有效页数） */
    uint32_t last_flush_writes;           /* 上次落盘时的 total_writes（节拍基准） */
    uint32_t last_flush_ms;               /* 上次落盘时刻（ms，节拍基准） */
    uint8_t  dirty[FTL_BLOCK_TOTAL / 8U]; /* 脏位图：哪些块的计数变了 */

    /* ── 阶段 7：异步 job ──
     * `dirty_snapshot` = **本批起点**的脏位图副本。批跨多轮，期间 mig/RR job 的完成
     * 会调 wear_mark_dirty 置新脏位 ⇒ 必须把「本批写哪些块」钉死，否则批会写出超过
     * compact 判定所用的页数，next_page 可能越过块尾。
     * 配对的写法是**批起点把 dirty 整体清零**：批期间 mark_dirty 置的位于是只表示
     * 「批开始后才变的块」。收尾什么都不清（剩下的位留给下一批）；批失败则
     * `dirty |= snapshot` 并回。这样「批前就脏、批中被写、批中又变」的块不会丢最后一次
     * 变更——同步版 wear_flush 整批原子，没有这个窗口，清零式让异步版拿到同样的原子性。 */
    wear_job_t job;
    uint8_t    dirty_snapshot[FTL_BLOCK_TOTAL / 8U];
} wear_state_t;

/* ── Allocator 上下文 ──
 *
 * 各指针在 allocator_init 里绑到**本实例自持的** ftl_tables_t（P2-5）——表归 FTL 所有。
 */

typedef struct
{
    /* ── 拓扑参数 ── */
    uint8_t  ch;           /* 本 Allocator 绑定的 CH */
    uint8_t  ce;           /* 本 Allocator 绑定的 CE */
    uint8_t  lun;          /* 本 Allocator 绑定的 LUN */
    uint16_t total_blocks; /* 总好 Block 数（由 HIL 回填 free pool 后置定，见 main.c） */

    /* ── Free Pool（一个已擦除池；三级水位见文件头） ── */
    uint16_t *free_blocks; /* → tables.free_blocks */
    uint16_t  free_count;  /* 空闲（且已擦除）block 数——三级水位都建在它上面 */

    /* ── OP / Spare Pool（per-plane；容量不动） ── */
    uint16_t (*op_blocks)[FTL_OP_POOL_CAP];             /* → tables.op_blocks（GC 缓冲池，每 plane 独立） */
    uint16_t op_count[PLANE_NUM];                       /* 各 plane OP 池剩余 Block 数 */
    uint16_t (*spare_blocks)[FTL_SPARE_POOL_PER_PLANE]; /* → tables.spare_blocks（坏块替换池，每 plane 独立） */
    uint16_t spare_count[PLANE_NUM];                    /* 各 plane Spare Pool 剩余 Block 数 */

    /* ── Open Super Block（每温度一个） ── */
    super_block_t hot_sb;  /* Hot Open Super Block */
    super_block_t cold_sb; /* Cold Open Super Block */

    /* ── Closed Super Block 列表（等待 GC） ── */
    super_block_t *closed_sb_list;  /* → tables.closed_sb_list */
    uint16_t       closed_sb_count; /* 已关闭 Super Block 数 */

    /* ── Block State Table（统一 per-Block 状态） ── */
    ftl_block_t *blocks;    /* → tables.blocks */
    uint16_t     bad_count; /* 坏 Block 数 */

    /* ── 元数据池游标（序 4：per-instance，原 bbt.c / wear.c 的 file-scope static） ── */
    bbt_state_t  bbt;  /* 坏块表池：sequence / write_idx / cursor_valid / dirty */
    wear_state_t wear; /* 磨损计数日志：sequence / next_page / 节拍基准 / 脏位图 */
} ftl_allocator_t;

/* ── 接口函数 ── */

/*
 * allocator_init - 初始化 Allocator
 *
 * @param alloc   Allocator 上下文（本实例自持存储，见 ftl_context_t）
 * @param topo    本实例绑定的 NAND 芯片路由
 * @param tables  FTL 自持表（P2-5：由 ftl_context_t 内含；本函数只绑定指针，不复制）
 *
 * @return ftl_status_t — FTL_OK 成功
 */
ftl_status_t allocator_init(ftl_allocator_t *alloc, ftl_topology_t topo, ftl_tables_t *tables);

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
 * ⚠ 与 allocator_alloc_pair(FTL_TEMP_COLD) 的区别：后者取的是**冷 Open SB 的一页**，
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
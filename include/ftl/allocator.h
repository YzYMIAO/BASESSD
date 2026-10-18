#ifndef FTL_ALLOCATOR_H
#define FTL_ALLOCATOR_H

/*
 * FTL Allocator — Block 分配器
 *
 * 职责：
 *   1. 维护 Free Pool（一个已擦除池，三级**水位**见下）
 *   2. 从 Free Pool 取各 plane 的 Block 组成 Super Block
 *   3. 跟踪每个温度（Hot/Cold）的 Open Super Block 写指针
 *   4. 写满时关闭 Super Block 并加入 Closed 列表
 *   5. 磨损均衡：分配时选擦除次数最少的 Block
 *   6. 跳过坏块，通过 Spare Pool 保水位
 *
 * 地址体系：
 *   allocator_alloc_pair() 通过 out[] 返回 nand_pba_t（FTL 物理地址：CH/CE/LUN 拓扑 +
 *   块/页/plane/CW）；函数返回值为 ftl_status_t。PBA 的 page 字段由 allocator 管理，
 *   block/plane 从 Free Pool 分配。
 *
 * ══ 不变量（读本文件前先读这四条；每条都有 API 兜着，别再靠调用点纪律）══
 *
 * ① **四本账，各有唯一的进出闸门。** Free Pool（`free_blocks/free_count`）只走
 *    `allocator_free_block` / `allocator_remove_from_free`；OP / Spare 池只走
 *    `allocator_*_try_push/pop`；Closed 列表只走 `allocator_add_closed_sb` /
 *    `allocator_remove_closed_sb`。外部模块不得裸写这些数组。**读**一律走
 *    `allocator_*_count()` 与 `allocator_closed_at()`。
 *
 * ② **`state == FTL_BLOCK_STATE_FREE` ⟺ 该块在 Free Pool 里**。
 *    **坏块（BAD）永不进池**：BAD 是状态机终态，唯一入池闸门 `allocator_free_block`
 *    拒绝它（debug `ASSERT` + release 运行期拒绝）。
 *    两个方向各有归属者，别的路径不许碰：
 *      · 入池 `allocator_free_block` —— 置 `FREE`；
 *      · 出池 `allocator_remove_from_free` —— **只校验**「在池 ⇒ state == FREE」（`ASSERT`），
 *        状态由**转移的归属者**紧跟其后赋值（open_sb→OPEN / alloc_log→LOG /
 *        migrate 退役→RETIRED / rebuild→CLOSED / bbt_mark_bad→BAD）。
 *    **无归属的块一律 `state = FTL_BLOCK_STATE_NONE`（0）**：固定元数据块（per-plane 0..62，
 *    按几何地址寻址、永不参与分配）与 Spare / OP 池块。`allocator_init` 早于元数据目录加载
 *    （`ftl.c` 的 init 顺序），拿不到「哪些是元数据块」——所以它**什么都不置**，
 *    而不是"先全部置上再说"。
 *
 *    ⚠ **选块以 Free Pool 数组为准**：它给出「选中 ⇒ `allocator_remove_from_free` 必然成功」
 *      这条**结构性**保证（避免"选中 → 移除失败 → 重选同一块"的死循环），
 *      与 FREE 位是否诚实无关。见 `allocator_select_plane_block_from_pool` 的长注释。
 *
 * ③ **`closed_sb_list` 是 swap-remove 的活列表，借出的指针不跨调用有效。**
 *    任何一次 `allocator_add_closed_sb` / `allocator_remove_closed_sb` 都可能把末位条目
 *    搬到别处 ⇒ 跨调用持有请**按值拷贝**（`migrate.h` 的 victim 副本就是这条约定）。
 *    列表实际只收**已关闭**的条目（`allocator_close_sb` 在拷贝前就把 `is_open` 清 false），
 *    但消费方仍应判 `!sb->is_open` 过滤——这条过滤是 UT 钉住的契约
 *    （`test_gc_victim_skips_open`），不是可有可无的冗余。
 *
 * ④ **指针 / 索引类实参是前置条件，由调用者保证；非法即 `ASSERT`。**
 *    本模块全部入口按 `docs/design/assertion_guidelines.md` 判据 2 处理（判据 2 的层内模块头
 *    名单里逐字列了 `allocator.h`）：实参都是调用者自己持有的指针（`&ftl->alloc`）或自己
 *    算出的索引（victim / 出厂扫描 / Spare 取块），不存在"外部输入" ⇒ **不返回错误码**。
 *    **保留返回码的只有三处**：`allocator_alloc_pair` / `allocator_alloc_log_block`
 *    的池空（`FTL_AGAIN`，判据 3 资源类结果）、`allocator_close_open_sb` 对已关闭槽的
 *    幂等早退（§十三 `fc_command_release` 先例）。
 *    ⚠ 例外：`allocator_remove_from_free` 的「不在池内 ⇒ no-op」是判据 4 **明文举的**
 *    文档化契约（UT 钉住），不要"顺手"转成 ASSERT。
 */

#include "common/assertion.h" /* ASSERT（退化单 plane 条目展开地址是编码错误） */
#include "ftl/bst.h"          /* ftl_block_t, FTL_BLOCK_TOTAL, ftl_gblock, ftl_block_is_bad */
#include "ftl/ftl_config.h"   /* nand_pba_t, pba_make, NAND_PBA_INVALID */
#include "ftl/ftl_type.h"     /* ftl_status_t, ftl_topology_t */
#include "nand/nand_config.h" /* PLANE_NUM, PAGES_PER_BLOCK, BLOCKS_PER_PLANE */

#include <stdbool.h>
#include <stdint.h>

/* ── 温度等级 ── */
#define FTL_TEMP_HOT  0U /* 用户 IO 数据 */
#define FTL_TEMP_COLD 1U /* GC 搬移 */
#define FTL_TEMP_NUM  2U /* 温度等级数 */

/* ── Free Pool 三级水位（设计 §3.7）──
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
 *   ⚠ 32 / 8 是**本仓经验值**（设计 §3.7），随 OP 预留比例与芯片容量调整，
 *     不是行业标准数值。 */
#define FREE_POOL_TARGET    (32U) /* 后台 GC 水位（IDLE 补池目标） */
#define FREE_POOL_EMERGENCY (8U)  /* 水位②/③分界：低于等于此值转紧急 GC */
#define FTL_OP_PERCENT      (10U) /* OP 预留百分比（编译期默认；**按 plane 计**） */

/* 每 plane OP 块数 = 2048 × 10% = 204（floor）⇒ 总 408。
 * 预留按 **super block 粒度**（每 plane 各一份），因为写入/擦除/GC 的最小单元是跨各 plane 的
 * super block——只留单 plane 的 OP 对另一 plane 的池耗尽毫无帮助。 */
#define FTL_OP_BLOCKS_PER_PLANE ((BLOCKS_PER_PLANE * FTL_OP_PERCENT) / 100U)

/* 数组维度钳到 ≥1：FTL_OP_PERCENT=0（设计允许"禁用 OP"）会让 [PLANE_NUM][0] 非法。
 * 填池/消费循环一律用 FTL_OP_BLOCKS_PER_PLANE（=0 ⇒ 不预留），本宏只用于定数组尺寸。 */
#define FTL_OP_POOL_CAP          ((FTL_OP_BLOCKS_PER_PLANE) > 0U ? (FTL_OP_BLOCKS_PER_PLANE) : 1U)
#define FTL_SPARE_POOL_PER_PLANE (50U) /* 每 plane 坏块替换池大小（总 100） */

/* ── Closed Super Block 列表最大容量 ──
 *
 * 容量取 **全部物理块数**（FTL_BLOCK_TOTAL）——这是结构的真实上界，不是估值：
 *   · 掉电重建（`allocator_add_closed_sb`）**每写过一个物理块就建一条**退化单 plane 条目
 *     ⇒ 满盘重启最多要 FTL_BLOCK_TOTAL 条；
 *   · 运行时（`allocator_close_sb`）每条 SB 占 1~2 个块 ⇒ 条数必 < FTL_BLOCK_TOTAL。
 * 取更小的值会让满盘重启时列表溢出——两条路径的失败形态都不可接受：
 * 重建路径当场 ASSERT（debug 下开机即 HardFault），运行时路径**静默丢条目**（块的 BST
 * 已置 CLOSED、`sb_id` 已写成越界索引，却不在列表里 ⇒ GC 永不可见 ⇒ 永久漏账）。
 * 取到上界后，两条路径的「列表满」都成了**结构上不可能**，守卫才配称不变量。
 *
 * 代价：每实例 32 KB（`super_block_t` 8 B × 4096），一次性。
 */
#define SUPER_BLOCK_LIST_MAX (FTL_BLOCK_TOTAL)

/* ── Closed 条目「退化单 plane」哨兵 ──
 * Policy B 下 SB 各 plane 是**独立分配**的物理块（不要求同号）。掉电重建按物理块逐个归位
 * （见 power_failure_recovery.c），无法、也不需要恢复「哪两块曾是一对」，故 closed_sb_list
 * 条目允许只含一个 plane：另一个 plane 记本哨兵。Open SB（hot_sb/cold_sb，multi-plane 写的
 * 单元）恒为双 plane，不受影响。凡按 plane 展开地址的读者都要先判哨兵。 */
#define FTL_BLOCK_NO_PLANE (UINT16_MAX)

/* ── 选块失败哨兵（gblock 坐标的「无此块」）──
 *
 * 选块（`allocator_select_plane_block_from_pool`）返回的是 **gblock**（跨 plane 全局编号）；
 * 该 plane 的 Free Pool 里没有可用块时返回本哨兵。取 `UINT16_MAX` 的判据是它**超出 gblock
 * 值域**——`ftl_gblock_plane()` 会解出 ≥ PLANE_NUM 的越界 plane（`bst.h`），可判、不与合法块号相撞。
 *
 * ⚠ **与 `FTL_BLOCK_NO_PLANE` 值相同、语义不同，不可互换**（C11：同值 ≠ 同一坐标；
 *   C9 管反方向「同一语义不许两套坐标」）：
 *     · `FTL_BLOCK_NO_PLANE` —— **plane 内块号**槽位（`super_block_t.plane_blocks[]`）的
 *       「该 plane 无块」**存储态**哨兵，会被写进 closed_sb_list **持久化**；
 *     · `FTL_GBLOCK_INVALID` —— **gblock** 坐标的「选块失败」**瞬时返回值**，从不落盘。
 */
#define FTL_GBLOCK_INVALID (UINT16_MAX)

/* ── Super Block ──
 *
 * Super Block 是跨各 plane 的逻辑写入单元。
 * 每个 plane 各取一个物理 Block，Multi-Plane PROGRAM 同步写入。
 * 两个温度（Hot/Cold）各有一个 Open Super Block。
 * 写满后从 Open → Closed，等待 GC 回收。
 */

typedef struct
{
    uint16_t plane_blocks[PLANE_NUM]; /* 各 Plane 的 Block 号；可为 FTL_BLOCK_NO_PLANE（退化单 plane 条目） */
    uint8_t  next_page;               /* 当前写入页（0~63，各 plane 同步） */
    uint8_t  temperature;             /* FTL_TEMP_HOT / FTL_TEMP_COLD */
    bool     is_open;                 /* true = Open（仍在写），false = Closed（写满待回收） */
} super_block_t;

/* ── FTL 表存储（阶段 C：由 ftl_storage_t 提供并按域放置） ──
 *
 * 大数组**归 FTL 实例所有**：由 `ftl_context_t.tables` 内含，HIL（main.c）不声明数组、
 * 不传指针。与主流同构——OpenSSD 用全局数组、SPDK 把表放进 `spdk_ftl_dev`；
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
static inline bool super_block_has_plane(const super_block_t *sb, uint8_t plane)
{
    ASSERT(plane < PLANE_NUM);
    return (sb->plane_blocks[plane] != FTL_BLOCK_NO_PLANE);
}

/* 返回 Super Block 中指定 Plane/Page 的 PBA（用户数据使用本实例的 ch/ce/lun）
 *
 * 拓扑由形参给出（`ftl_topology_t` 是 FTL 层 ch/ce/lun 的唯一坐标）—— Super Block 自身
 * **不带路由**：它永属同一个实例（Policy B），路由就是拥有它的那个实例的拓扑。 */
static inline nand_pba_t super_block_page_pba(ftl_topology_t topo, const super_block_t *sb, uint8_t plane, uint8_t page)
{
    /* 退化单 plane 条目的哨兵 plane 不可展开（plane_blocks[plane] 是哨兵，非块号） */
    ASSERT(super_block_has_plane(sb, plane));

    /* cw = 0 = 整页（页级基址）。 */
    return pba_make(topo.ch, topo.ce, topo.lun, sb->plane_blocks[plane], plane, page, 0U);
}

/* ── 元数据池游标（per-instance）──
 *
 * BBT 池有自己的 sequence 与写游标（环形轮转，开机由 bbt_load 恢复）。它**归实例所有**：
 * 多实例（多 LUN，`g_ftl[ch][ce][lun]`）下若用 file-scope static，所有实例会共享一份
 * 游标/序号、互相覆盖 ⇒ 池写错槽。
 *
 * 放这里（而非 bbt.h）的两个理由：
 *   ① 本模块的**全部**接口都以 `ftl_allocator_t *` 为首参（状态随分配器走最自然）；
 *      而 bbt.h 已 include 本文件，反向包含会成环。
 *   ② 语义上属「块账本」，与 BST 同一维度。
 */
/* ── BBT 持久化 job（异步）──
 *
 * 落盘走 job：`bbt_kick` 起步（提交第一条命令即返回），完成由
 * g_op_service[FTL_OP_BBT]（ftl_op_bbt）按 phase 收尾并提交下一条。
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
    bbt_job_t job;          /* 异步持久化 job */
} bbt_state_t;

/* ── Allocator 上下文 ──
 *
 * 各指针在 allocator_init 里绑到**本实例的** ftl_tables_t（由 ftl_storage_t 提供，落 SRAM12）。
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

    /* ── 元数据池游标（per-instance） ── */
    bbt_state_t bbt; /* 坏块表池：sequence / write_idx / cursor_valid / dirty */
} ftl_allocator_t;

/* 本实例的拓扑（allocator 自持的那份 ch/ce/lun 副本 → 标准 ftl_topology_t）。
 * 只有 alloc 在手、拿不到 ftl_context_t 的调用点（allocator.c / bbt.c）用它；
 * 有 ftl 的调用点直接传 ftl->topo 即可，不必绕这一层。 */
static inline ftl_topology_t allocator_topo(const ftl_allocator_t *alloc)
{
    return (ftl_topology_t){.ch = alloc->ch, .ce = alloc->ce, .lun = alloc->lun};
}

/* ── 账本只读访问器 ──
 *
 * 外部模块（GC / WL / migrate / 掉电恢复 / CLI / 元数据）一律经这里读账本，
 * 不裸摸 `alloc->free_count` / `alloc->closed_sb_list[i]`：
 *   · 计数口径只在一处定义——以后账本换存储只改这里；
 *   · 越界读当场停机，而不是静默读到脏条目。
 *
 * 写侧**不经**这里——池的进出统一走 allocator_* 的 push/pop 族（见文件头「不变量」）。 */

static inline uint16_t allocator_free_count(const ftl_allocator_t *alloc)
{
    return alloc->free_count;
}

static inline uint16_t allocator_op_count(const ftl_allocator_t *alloc, uint8_t plane)
{
    ASSERT(plane < PLANE_NUM);
    return alloc->op_count[plane];
}

static inline uint16_t allocator_spare_count(const ftl_allocator_t *alloc, uint8_t plane)
{
    ASSERT(plane < PLANE_NUM);
    return alloc->spare_count[plane];
}

/* 全 plane 合计（日志/观测用；维度变化时自动跟随，不写死 [0]/[1]）。 */
static inline uint16_t allocator_op_total(const ftl_allocator_t *alloc)
{
    uint16_t n = 0U;

    for (uint8_t p = 0U; p < PLANE_NUM; p++)
    {
        n = (uint16_t)(n + alloc->op_count[p]);
    }
    return n;
}

static inline uint16_t allocator_spare_total(const ftl_allocator_t *alloc)
{
    uint16_t n = 0U;

    for (uint8_t p = 0U; p < PLANE_NUM; p++)
    {
        n = (uint16_t)(n + alloc->spare_count[p]);
    }
    return n;
}

static inline uint16_t allocator_closed_count(const ftl_allocator_t *alloc)
{
    return alloc->closed_sb_count;
}

/* closed_sb_list 第 i 项（**借出**，不转移所有权）。
 *
 * ⚠ 返回值是列表内的**活指针**：allocator_add_closed_sb / allocator_remove_closed_sb
 *   是 swap-remove，一次调用就可能让先前取到的指针指向别的条目。跨调用持有请**按值
 *   拷贝**（`migrate.h` 的 victim 副本就是这条约定）。
 *
 * ⚠ 消费方仍须先判 `!sb->is_open` 再回收——见文件头不变量 ③。 */
static inline super_block_t *allocator_closed_at(const ftl_allocator_t *alloc, uint16_t idx)
{
    ASSERT(idx < alloc->closed_sb_count);
    return &alloc->closed_sb_list[idx];
}

/* ── OP / Spare 池的对称 push / pop ──
 *
 * 三个池（Free / OP / Spare）的进出方式从此**只有一种**：push 入池、pop 出池，
 * 且都只动「数组 + 计数」。池的 push/pop **不碰 BST state**——块状态由
 * `allocator_free_block`（入池 ⇒ FREE）与**各转移的归属者**（出池后赋新态）维护。
 * ⚠ 出池闸门 `allocator_remove_from_free` **不赋新态**，只**校验**「在池 ⇒ state == FREE」
 *   （见不变量②）；新态由转移的归属者紧随其后赋值。
 *
 * 满 / 空一律**返回 0 而不是 ASSERT**（判据 3：资源类结果）：
 *   · `FTL_OP_BLOCKS_PER_PLANE` 由 `FTL_OP_PERCENT` 算出，设计上允许为 0（禁用 OP 池）
 *     ⇒「OP 池满」在合法配置下可达，不是「不该发生」；
 *   · Spare 池的入池发生在掉电重建逐块扫描时，好块数可能超过池容量 ⇒ 满也是常态。
 * 调用者据此决定回退（如 main.c 回填：池满 ⇒ 改送 Free Pool）。 */

static inline bool allocator_op_try_push(ftl_allocator_t *alloc, uint8_t plane, uint16_t gblock)
{
    ASSERT(plane < PLANE_NUM);
    if (alloc->op_count[plane] >= (uint16_t)FTL_OP_BLOCKS_PER_PLANE)
    {
        return false;
    }
    alloc->op_blocks[plane][alloc->op_count[plane]] = gblock;
    alloc->op_count[plane]++;
    return true;
}

static inline bool allocator_op_try_pop(ftl_allocator_t *alloc, uint8_t plane, uint16_t *out_gblock)
{
    ASSERT(plane < PLANE_NUM);
    ASSERT(out_gblock != NULL);
    if (alloc->op_count[plane] == 0U)
    {
        return false;
    }
    alloc->op_count[plane]--;
    *out_gblock = alloc->op_blocks[plane][alloc->op_count[plane]];
    return true;
}

static inline bool allocator_spare_try_push(ftl_allocator_t *alloc, uint8_t plane, uint16_t gblock)
{
    ASSERT(plane < PLANE_NUM);
    if (alloc->spare_count[plane] >= (uint16_t)FTL_SPARE_POOL_PER_PLANE)
    {
        return false;
    }
    alloc->spare_blocks[plane][alloc->spare_count[plane]] = gblock;
    alloc->spare_count[plane]++;
    return true;
}

static inline bool allocator_spare_try_pop(ftl_allocator_t *alloc, uint8_t plane, uint16_t *out_gblock)
{
    ASSERT(plane < PLANE_NUM);
    ASSERT(out_gblock != NULL);
    if (alloc->spare_count[plane] == 0U)
    {
        return false;
    }
    alloc->spare_count[plane]--;
    *out_gblock = alloc->spare_blocks[plane][alloc->spare_count[plane]];
    return true;
}

/* ── 接口函数 ── */

/*
 * allocator_init - 初始化 Allocator
 *
 * @param alloc   Allocator 上下文（本实例自持存储，见 ftl_context_t）
 * @param topo    本实例绑定的 NAND 芯片路由
 * @param tables  FTL 表存储（由 ftl_storage_t 提供；本函数只绑定指针，不复制）
 *
 * @return 无（`alloc`/`tables` 是前置条件，非法即 ASSERT，见不变量 ④）
 */
void allocator_init(ftl_allocator_t *alloc, ftl_topology_t topo, ftl_tables_t *tables);

/*
 * allocator_alloc_pair - 分配一页的**各 plane** PBA（Multi-Plane 写用）
 *
 * 内部：
 *   ① 从对应温度的 Open Super Block 取当前写入位置
 *   ② Super Block 已写满 → 关闭它（→ Closed），从 Free Pool 分配新的 Super Block
 *      （**各 plane 各自独立选块**——Policy B）
 *   ③ out[plane] 填当前页在该 plane 的 PBA，next_page++
 *   ④ Free Pool 耗尽 → 返回 FTL_AGAIN（write 路径触发紧急 GC 重试）
 *
 * @param alloc       Allocator 上下文
 * @param temperature FTL_TEMP_HOT / FTL_TEMP_COLD
 * @param out         输出：out[plane] = 该 plane 当前页 PBA（调用者保证 PLANE_NUM 长）
 *
 * @return FTL_OK 成功；FTL_AGAIN 某 plane 无块可分配（瞬态，调用者触发 GC / 背压）
 */
ftl_status_t allocator_alloc_pair(ftl_allocator_t *alloc, uint8_t temperature, nand_pba_t out[PLANE_NUM]);

/*
 * allocator_free_block - GC 擦除后将 Block 归还到 Free Pool
 *
 * **前置条件**：块已擦除、且**不是 BAD**（BAD 是状态机终态，不能回到 Free Pool——
 * 否则会把 BAD 覆盖成 FREE，选块不再跳过它）。BAD 在 debug 用 `ASSERT`、release 用
 * 运行期拒绝（判据 9：后果是静默介质破坏）。
 *
 * @param alloc  Allocator 上下文
 * @param gblock 要归还的 Block 编号（全局 Block 号）。**前置条件：`< FTL_BLOCK_TOTAL`**——
 *               越界索引会踩进相邻的 `free_blocks[]`，故 debug 用 ASSERT、release 用
 *               运行期拒绝（判据 9：后果是静默漏账而非崩溃）。
 */
void allocator_free_block(ftl_allocator_t *alloc, uint16_t gblock);

/*
 * allocator_alloc_log_block - 独占分配一个**整块**（元数据专用）
 *
 * 用途：L2P 日志块等「整块归元数据所有」的分配。从该 plane 的 Free Pool 取一块
 * （磨损最少者），把它从 Free Pool **移除**并标记 LOG。
 *
 * ⚠ 与 allocator_alloc_pair(FTL_TEMP_COLD) 的区别：后者取的是**冷 Open SB 的一页**，
 *   而冷 SB 与 GC 迁移共享 ⇒ 元数据绝不能走那条路（会把 GC 的写指针搅乱）。
 *
 * ⚠ 两件事必须一起做——只置 LOG 挡不住分配，因为选块以
 *   allocator_select_plane_block_from_pool() 的 **Free Pool 数组**为准：
 *     ① allocator_remove_from_free()（从 Free Pool 拿掉）
 *     ② 置 LOG（出池只校验「在池 ⇒ state == FREE」，赋态由转移归属者完成）
 *
 * ⚠ **plane 固定**：Policy B 下各 plane 各自独立选块，拿不到「各 plane 同号都空闲」
 *   的保证，故不跨 plane 配对；调用者给出目标 plane，返回单个 gblock。
 *
 * 归还走 allocator_free_block()（调用者保证该 LOG 块已擦除）。
 *
 * @param alloc      Allocator 上下文
 * @param plane      目标 plane（0..PLANE_NUM-1）
 * @param out_gblock 输出：全局 Block 号（plane × BLOCKS_PER_PLANE + block）
 * @return FTL_OK / FTL_AGAIN（**仅**表示该 plane 无空闲块——资源类结果，判据 3；
 *         实参非法是前置条件，即 ASSERT，见不变量 ④）
 */
ftl_status_t allocator_alloc_log_block(ftl_allocator_t *alloc, uint8_t plane, uint16_t *out_gblock);

/*
 * allocator_close_open_sb - 运行时关闭当前 Open Super Block（写失败等场景）
 *
 * 行业坏块替换流程：Super Block 中某 plane PROGRAM 失败后，①关闭该 SB
 * （不再写入新数据）→ 进 closed_sb_list → GC 优先回收（VPC 低）。坏块
 * 标记仍由 bbt_mark_bad 完成（标 block 号）。Policy B 下各 plane 是独立物理块，
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
 * 不能复用 allocator_close_sb（那要求块处于 Open SB，重建块不是）。本函数直接组装：
 *
 *   sb = { plane_blocks=入参, next_page=PAGES_PER_BLOCK,
 *          temperature=入参, is_open=false }
 *   各 plane 的块置 CLOSED + 温度 + sb_id = closed 列表索引
 *   追加进 closed_sb_list，closed_sb_count++
 *
 * @param alloc          Allocator 上下文
 * @param plane_blocks   [PLANE_NUM] 每 plane 的 Block 号；Policy B 下逐 plane 独立分配，
 *                       重建产生的**退化单 plane 条目**把另一 plane 记 FTL_BLOCK_NO_PLANE
 * @param temperature    FTL_TEMP_HOT / FTL_TEMP_COLD（掉电无落盘，调用方给默认）
 * @return 塞入的 closed 列表索引。**无失败返回**——实参是前置条件（不变量 ④），列表满在
 *         结构上不可能（见 SUPER_BLOCK_LIST_MAX），真满了是 ASSERT 停机
 */
uint16_t allocator_add_closed_sb(ftl_allocator_t *alloc,
                                 const uint16_t   plane_blocks[PLANE_NUM],
                                 uint8_t          temperature);

/*
 * allocator_remove_from_free - 从 Free Pool 移除一个 Block（**出池不改状态**）
 *
 * **Free Pool 的唯一出池闸门**（swap-remove，O(1)；内部 open_sb / alloc_log 也走它）。
 * 外部用途：
 *   ① 掉电重建：写过（非全 FF）的块必须拉出，防止开机后被当作空白块重新分配
 *      （覆盖未擦除的半满块）；
 *   ② `bbt_mark_bad` 驱逐坏块（否则留下「在池却不是 FREE」的反向不一致）；
 *   ③ `migrate.c` 退役块（RETIRED，不再分配新数据）。
 *
 * ⚠ 出池只**校验**「在池 ⇒ state == FREE」（`ASSERT`），**不赋新态**——赋态是转移的归属者
 *   的事，全部出池调用点都在出池后紧跟一行 `ftl_block_set_state()`。
 *
 * ⚠ 块不在池内 ⇒ no-op，是**文档化契约**（判据 4 明文举例），不是调用者错误。
 *
 * @param alloc   Allocator 上下文
 * @param gblock 全局 Block 号（0..FTL_BLOCK_TOTAL-1）
 */
void allocator_remove_from_free(ftl_allocator_t *alloc, uint16_t gblock);

/* ── 统计访问器 ──
 *
 * 「一个 Super Block / 全表的磨损与有效性怎么算」只在本模块定义：GC / WL / 搬移引擎
 * 都消费同一份口径，不再各自手抄逐 plane 遍历。口径固定为：
 *   · 坏块不计入全表统计；
 *   · 退化单 plane 条目（重建产物）跳过哨兵 plane，按实际 plane 数计算；
 *   · valid_count 单位是 **CW**（CW 级映射）。
 *
 * 统计是**只读**的派生量，不进账本、不参与不变量维护。 */

/* 单个 Super Block 的聚合值。 */
typedef struct
{
    uint16_t valid_cw;    /* 各 plane valid_count 之和（CW 粒度） */
    uint16_t plane_count; /* 实际存在的 plane 数（退化单 plane 条目 = 1；全哨兵 = 0） */
    uint32_t erase_max;   /* 各 plane erase_count 取大 */
    uint32_t erase_sum;   /* 各 plane erase_count 之和（平均 = erase_sum / plane_count） */
    uint32_t pe_min;      /* 各 plane last_pe_ts 取小（plane_count == 0 时为 UINT32_MAX） */
    uint32_t read_sum;    /* 各 plane read_count 之和 */
} allocator_sb_stats_t;

/*
 * allocator_sb_stats - 计算一个 Super Block 的各 plane 聚合值
 *
 * @param alloc    Allocator 上下文（前置条件，非法即 ASSERT）
 * @param sb       目标 Super Block（前置条件，非法即 ASSERT）
 * @param out      输出（前置条件，非法即 ASSERT）
 */
void allocator_sb_stats(const ftl_allocator_t *alloc, const super_block_t *sb, allocator_sb_stats_t *out);

/* 全表（好块）磨损统计，一次扫描得到全部派生量。 */
typedef struct
{
    uint32_t erase_min;  /* 最小擦除次数；good_count == 0 时为 0 */
    uint32_t erase_max;  /* 最大擦除次数；good_count == 0 时为 0 */
    uint32_t erase_sum;  /* 擦除次数总和（平均 = erase_sum / good_count） */
    uint32_t max_pe_ts;  /* 最大 last_pe_ts（0 = 本上电周期尚无块被擦） */
    uint16_t good_count; /* 参与统计的好块数 */
} allocator_erase_stats_t;

/*
 * allocator_erase_stats - 全表好块磨损统计（一次扫描）
 *
 * @param alloc  Allocator 上下文（前置条件，非法即 ASSERT）
 * @param out    输出（前置条件，非法即 ASSERT）
 */
void allocator_erase_stats(const ftl_allocator_t *alloc, allocator_erase_stats_t *out);

/*
 * allocator_count_blocks_below - 擦除次数严格小于 threshold 的好块数
 *
 * @param alloc      Allocator 上下文（前置条件，非法即 ASSERT）
 * @param threshold  上界（不含）
 * @return 好块数（0..FTL_BLOCK_TOTAL）
 */
uint16_t allocator_count_blocks_below(const ftl_allocator_t *alloc, uint32_t threshold);

/*
 * allocator_find_closed_sb - 按 Super Block 各 plane 块号在 Closed 列表里定位条目
 *
 * 比对**全部 plane**：Policy B 下不同 SB 的哨兵 plane 可能相同，只比一个 plane
 * 会命中另一条同哨兵条目。供搬移收尾（migrate.c）在回收后定位并移除条目。
 *
 * @param alloc    Allocator 上下文（前置条件，非法即 ASSERT）
 * @param sb       目标 Super Block（前置条件，非法即 ASSERT）
 * @param out_idx  输出：Closed 列表索引（找到时才写）
 * @return true 找到（*out_idx 有效）；false 不在列表内
 */
bool allocator_find_closed_sb(const ftl_allocator_t *alloc, const super_block_t *sb, uint16_t *out_idx);

#endif /* FTL_ALLOCATOR_H */

#ifndef FTL_BST_H
#define FTL_BST_H

/*
 * Block State Table（BST）— 统一 per-Block 结构体数组
 *
 * 背景：
 *   v0.31 接入 read recovery 后，per-block 元数据分散在四处
 *   （allocator->erase_counts / bad_blocks / bbt g_bbt_bitmap /
 *   rr_block_track），无统一承载，且 read recovery 的 ECC 追踪数组
 *   从未分配（空转）。本表把三类状态合并为单一数据源。
 *
 * 索引语义：
 *   按「全局物理 Block 号」索引，gblock = plane * BLOCKS_PER_PLANE + block
 *   （0 .. FTL_BLOCK_TOTAL-1）。PBA 的 block 字段是 per-plane 号，
 *   通过 ftl_pba_gblock 统一转换为全局号——
 *   避免 plane1 与 plane0 的 block 号碰撞（历史 bug）。
 *
 * 存储位置：
 *   表**自持**（P2-5）：数组在 `ftl_context_t.tables.blocks` 内（见 ftl_tables_t），
 *   与 free_blocks / op_blocks / spare_blocks / closed_sb_list 同处一份。
 *   多 LUN 前置：每实例一份表 = 每实例一个 ftl_context_t，本表代码零改动。
 *
 * 有效位（DRAM-less approach ③，2026-10-13 定）：
 *   本仓**不**维护任何常驻有效位图。有效性 = 逐 CW 读**页内 META**（即本仓的「OOB」
 *   反向映射：每个 CW 头 4B 存本 CW 的扇区 LBA）→ **反查 L2P** 判该 CW 是否仍指向本
 *   物理位置。这就是主流 DRAM-less 的 flash reverse-mapping
 *   （DFTL / arXiv 1504.01666 / LeaFTL / VPFTL 同构）：逐页位图在 DRAM-less 下装不下
 *   （RAM ∝ 页数），且落 flash 后失效清位会引发大量 flash I/O ⇒ 主流不做。
 *   故本仓：
 *     · ftl_block_t = 块级聚合条目（valid_count 是 **CW 级真值**），20B/块，全常驻；
 *     · 无独立位图表——GC/WL 迁移逐页读 META 反查 L2P 判定，不靠任何 RAM 位图。
 *   块级聚合（valid_count）供 GC/WL 选 victim（对齐主流 validity ratio）；逐 CW 真值现查 L2P。
 */

#include "common/assertion.h" /* ASSERT（本文件全部访问器的入参前置条件 + 计数守卫） */
#include "ftl/ftl_config.h"   /* nand_pba_t, pba_to_*, NAND_PBA_INVALID */
#include "nand/nand_config.h" /* BLOCKS_PER_PLANE, PLANE_NUM, CW_NUM */

#include <stddef.h> /* NULL */
#include <stdint.h>

/* 全局物理 Block 总数（当前单 LUN = 2048 × 2 = 4096） */
#define FTL_BLOCK_TOTAL (BLOCKS_PER_PLANE * PLANE_NUM)

/* ── 预测性坏块健康状态（自 read_recovery.h 迁入，避免循环 include）── */
typedef enum
{
    RR_BLOCK_HEALTHY  = 0, /* 正常 */
    RR_BLOCK_WARNING  = 1, /* ECC 上升趋势中 */
    RR_BLOCK_CRITICAL = 2, /* ECC 接近上限 */
    RR_BLOCK_DYING    = 3, /* 预计即将坏块，准备搬移 */
} rr_block_health_t;

/* ── ECC 健康记录（per-Block，自 read_recovery.h 迁入）──
 * 消费方：ftl_op_host_io 读成功后 rr_record_ecc；
 *         rr_check_predictive / rr_get_health 查询健康度。
 *
 * 只保留「上一次 ECC bit 数」而非 N 深度环形缓冲：趋势判定（连续上升）
 * 只需与上一次比较，8 格环形缓冲实际供给的是最老那格的值（本次一并修复）。
 * 16B → 3B，4096 块省 52KB。 */
typedef struct
{
    uint8_t last_ecc;     /* 最近一次读的 ECC bit 数 */
    uint8_t rising_count; /* 连续上升计数（不上升即清零） */
    uint8_t health;       /* rr_block_health_t */
} rr_ecc_track_t;         /* 3B */

/* ── ftl_block_t 状态机（互斥态，枚举而非位图）──
 * 块**位置**同一时刻只能有一个，故用枚举而非位图：
 *   · 位图允许非法组合——原 RESERVED 位被「L2P 日志块」与「退役块」两种身份共用，
 *     且 bbt_mark_bad 不清它 ⇒「BAD|RESERVED」可表达且无断言拦（今天是不可达的
 *     潜在缺口，靠调用图碰巧如此，不是任何不变量保证的）；
 *   · 位图每次转换都要手写清位掩码——14 个转换点出现 7 种掩码，漏一位**编译期
 *     完全无声**（bbt_mark_bad 清温度而 bbt_scan_factory 不清，同文件内已不一致）。
 * 温度与位置**正交**（OPEN 块也有冷热），故单列字段，见 ftl_block_t.temp。
 *
 * ⚠ 无「预擦除 / 未擦除」分级态（P1-3）：Free Pool 是**一个**已擦除池（GC 先擦后归还），
 *   三级是**水位**不是**分区**——见 allocator.h 的「Free Pool 三级水位」。 */
typedef enum
{
    FTL_BLOCK_STATE_NONE    = 0U, /* 无归属：固定元数据区 / OP 池 / Spare 池 / BSS 未初始化 */
    FTL_BLOCK_STATE_FREE    = 1U, /* 在 Free Pool（已擦除） */
    FTL_BLOCK_STATE_OPEN    = 2U, /* 属于某 Open SB（正在写） */
    FTL_BLOCK_STATE_CLOSED  = 3U, /* 属于某 Closed SB（等待 GC） */
    FTL_BLOCK_STATE_LOG     = 4U, /* L2P 日志块（动态元数据块，本池独占；metadata 回收判据） */
    FTL_BLOCK_STATE_RETIRED = 5U, /* 退役：不再分配新数据、不标坏、仍可读（read_recovery） */
    FTL_BLOCK_STATE_BAD     = 6U, /* 坏块（终态；合并 bad_blocks[] + bbt bitmap） */
} ftl_block_state_t;

/* 状态存于 uint8_t ⇒ 值域必须装得下（枚举成员一多就会静默截断） */
_Static_assert((unsigned)FTL_BLOCK_STATE_BAD <= (unsigned)UINT8_MAX,
               "ftl_block_state_t 超出 uint8_t 值域");

/* ── Block State Table 每行（BIT 条目）──
 * 全字段常驻 SRAM（纯块级聚合，**无**位图表；有效性现查 META+L2P，见文件头）。
 * 只保留**有消费方**的字段——无读者的字段已删除（v0.56）：
 *   · program_count (2B) —— 写成功++，从无读者；连带 Wear 表的持久化槽一并删
 *   · bad_reason    (1B) —— 仅 bbt_mark_bad 写入，无读者；`FTL_BLOCK_BAD_*` 枚举与
 *                            `bbt_mark_bad` 的 `reason` 形参一并删除（BBT 持久化本就是
 *                            bitmap，重启后原因必丢，属纯 RAM 诊断残留）
 *   · gc_generation (1B) —— GC 迁移成功++，从无读者
 *
 *   erase_count   合并 allocator->erase_counts[]（allocator/gc/wl/wear 消费）
 *   last_pe_ts    最近擦除时间戳（GC victim youth_penalty，P1-1 消费）
 *   read_count    read disturb 计数（读成功++，read recovery / wear 消费）
 *   valid_count   双 Plane 合计有效 **CW** 0..512（GC/WL victim 选真值，O(1)）
 *   state         状态机（ftl_block_state_t，互斥态：NONE/FREE/OPEN/CLOSED/LOG/RETIRED/BAD）
 *   temp          温度流（0=HOT / 1=COLD，与位置**正交**）
 *   sb_id         block→SB 反向索引（UINT16_MAX=未归属，allocator 维护）
 *   rr            ECC 健康（last_ecc + rising_count + health，read recovery 供电）
 *
 *   维护点（valid_count 全真值，单位 CW）：
 *     ① 写成功：新位置每有效 CW add_valid；覆盖写旧位置每 CW sub_valid
 *     ② 读成功：read_count++（ECC 记录 rr）
 *     ③ GC/WL 迁移成功：源页每有效 CW sub_valid；新页每有效 CW add_valid
 *     ④ 擦除成功：valid_count=0 + last_pe_ts 写 + 状态 FREE
 *     ⑤ trim：每 LBA sub_valid
 *     ⑥ 坏块：置 BAD + clear_valid
 *     ⑦ 掉电恢复：逐页 L2P 反查重建（power_failure_recovery）
 *
 *   页内哪些 CW 真正有效，一律由**页内 META + L2P 反查**逐 CW 现判（无位图，见文件头）。
 *
 *   sizeof = 20B → 4096 项 = 80KB（无位图表）。
 */
typedef struct
{
    /* ── 磨损 ── */
    uint32_t erase_count; /* 4B  @0   擦除次数 */
    uint32_t last_pe_ts;  /* 4B  @4   最近擦除时间戳（WL 冷热避聚） */
    uint16_t read_count;  /* 2B  @8   read disturb 计数 */

    /* ── 有效页聚合 + 状态 ── */
    uint16_t valid_count; /* 2B  @10  双 Plane 合计有效 **CW** 0..512（逐 CW 真值，现查 META+L2P） */
    uint8_t  state;       /* 1B  @12  状态机（ftl_block_state_t，互斥） */
    uint8_t  temp;        /* 1B  @13  温度流（0=HOT / 1=COLD，同 FTL_TEMP_* 值域；正交于状态） */
    uint16_t sb_id;       /* 2B  @14  block→SB 反向索引（UINT16_MAX=无） */

    /* ── ECC 健康 ── */
    rr_ecc_track_t rr; /* 3B  @16  ECC 健康 */
} ftl_block_t;         /* 19B → 20B（u32 对齐） */

/* 布局守卫：字段顺序/类型漂移会让结构体悄悄变大，编译期拦住。 */
_Static_assert(sizeof(ftl_block_t) == 20, "ftl_block_t 布局漂移（期望 20B）");

/* ── 全局 Block 号转换（单点，修复 per-plane 碰撞的关键）── */

/* per-plane block + plane → 全局 0..4095 */
static inline uint16_t ftl_gblock(uint8_t plane, uint16_t block)
{
    return (uint16_t)((uint32_t)plane * BLOCKS_PER_PLANE + block);
}

/* PBA → 全局块号（直接按 PBA 位序解，不经 PAA） */
static inline uint16_t ftl_pba_gblock(nand_pba_t pba)
{
    return ftl_gblock((uint8_t)pba_to_plane(pba), (uint16_t)pba_to_block(pba));
}

/* gblock → plane / per-plane block（ftl_gblock 的逆，单点解码）。 */
static inline uint8_t ftl_gblock_plane(uint16_t g)
{
    return (uint8_t)((uint32_t)g / BLOCKS_PER_PLANE);
}

static inline uint16_t ftl_gblock_block(uint16_t g)
{
    return (uint16_t)((uint32_t)g % BLOCKS_PER_PLANE);
}

/* ── 本文件全部访问器的入参约定 ──
 *
 * `b` 是**前置条件**（判据 2：`bst.h` 在层内模块头名单里），不是可空参数：
 * 调用者一律传 `&alloc->blocks[<索引>]`，而 `blocks` 指向 `ftl_context_t.tables.blocks`
 * 这个**内嵌数组**（非指针）⇒ 取元素地址**结构性恒非空**。全仓 11 个 .c 的调用点
 * 已逐一核实无一传 NULL。
 * ⇒ 用 `ASSERT(b != NULL)`（H14）：debug 停机留现场，release 裁空 ⇒ 零开销，
 *   不保留 `if (b != NULL) { … }` 那种**恒真的静默吞**（§十附判据 1）。 */

/* ── 状态机访问器（枚举赋值，无位运算）──
 *
 * ⚠ H15（位运算与位判断走公共宏）在本字段**不适用**：拍平后没有任何位掩码/位标志运算
 *   ⇒「任一位置位 vs 全部位置位」那类歧义消失（原 `FTL_BLOCK_STATE_MASK` 零调用者，已删）。 */

static inline ftl_block_state_t ftl_block_state(const ftl_block_t *b)
{
    ASSERT(b != NULL);
    return (ftl_block_state_t)b->state;
}

static inline void ftl_block_set_state(ftl_block_t *b, ftl_block_state_t st)
{
    ASSERT(b != NULL);
    ASSERT((unsigned)st <= (unsigned)FTL_BLOCK_STATE_BAD); /* 漏斗唯一入口，越界即停机 */
    b->state = (uint8_t)st;
}

/* ── 坏块（BST 单数据源；NAND 持久化仍走 bbt_header_t.bad_map）──
 * 坏块是状态机的**终态**，不再是独立位 ⇒ 与其余 6 态天然互斥
 * （原「BAD|RESERVED」并存不再可表达）。 */

static inline bool ftl_block_is_bad(const ftl_block_t *b)
{
    ASSERT(b != NULL);
    return ftl_block_state(b) == FTL_BLOCK_STATE_BAD;
}

static inline void ftl_block_set_bad(ftl_block_t *b)
{
    ASSERT(b != NULL);
    ftl_block_set_state(b, FTL_BLOCK_STATE_BAD);
}

/* 温度流：分配时记录 hot/cold（0=HOT / 1=COLD，与 allocator 的 FTL_TEMP_* 同值域）。
 * 不在本头引用 FTL_TEMP_*（避免 allocator.h 循环 include），直接用值语义；
 * **不另立 `ftl_block_temp_t` 枚举**——同值域再来一套名字就是第二套坐标（C9）。
 * 温度与状态**正交** ⇒ `ftl_block_set_state` 不碰本字段；每次分配都显式写入，
 * 故 FREE/BAD 块上残留的温度不会被任何读者读到（`ftl_block_get_temp` 生产侧零读者）。 */
static inline void ftl_block_set_temp(ftl_block_t *b, uint8_t temperature)
{
    ASSERT(b != NULL);
    ASSERT(temperature <= 1U); /* 0=HOT / 1=COLD，同 FTL_TEMP_* */
    b->temp = temperature;
}

/* ⚠ 返回值 0=HOT / 1=COLD 是**两值域不是真值域**，不是 bool（UT 逐条钉住该值域）。 */
static inline uint8_t ftl_block_get_temp(const ftl_block_t *b)
{
    ASSERT(b != NULL);
    return b->temp;
}

/* 状态名（诊断日志用）。两个调用者：ftl.c 的 dangling 守卫、controller/cli.c 的 map。
 * H7：switch 必须有 default（非枚举值只可能来自内存损坏，返回 "?" 而不是静默取一支）。 */
static inline const char *ftl_block_state_name(ftl_block_state_t st)
{
    switch (st)
    {
        case FTL_BLOCK_STATE_NONE:
            return "NONE";
        case FTL_BLOCK_STATE_FREE:
            return "FREE";
        case FTL_BLOCK_STATE_OPEN:
            return "OPEN";
        case FTL_BLOCK_STATE_CLOSED:
            return "CLOSED";
        case FTL_BLOCK_STATE_LOG:
            return "LOG";
        case FTL_BLOCK_STATE_RETIRED:
            return "RETIRED";
        case FTL_BLOCK_STATE_BAD:
            return "BAD";
        default:
            return "?";
    }
}

/* 反向索引 sb_id（UINT16_MAX = 未归属任何 SB） */
#define FTL_BLOCK_NO_SB (UINT16_MAX)

static inline uint16_t ftl_block_sb_id(const ftl_block_t *b)
{
    ASSERT(b != NULL);
    return b->sb_id;
}

static inline void ftl_block_set_sb(ftl_block_t *b, uint16_t sb_id)
{
    ASSERT(b != NULL);
    b->sb_id = sb_id;
}

/* ── 有效 CW 计数（块级聚合真值）──
 *
 * CW 级映射（IU = 1 CW = 512B）下，一个物理页内 4 个 CW 各自独立有效：
 * 主机只改写其中 1 个 CW 时，同页另 3 个 CW 仍然有效。
 *
 * valid_count（在 ftl_block_t 内）是**真值**，单位 **CW**，值域
 * 0 .. PLANE_NUM × PAGES_PER_BLOCK × CW_NUM（当前 0..512）。每次「某 LBA 的映射
 * 指向本块某页某 CW」+1、「某 LBA 不再指向本块某页某 CW」-1，与 L2P 逐条同步。
 * GC/WL 的 victim 评分分母必须是**满块 CW 数**（不是页数）。
 *
 * ⚠ 本仓**无**「写过」位图（v0.60 删）：页内哪些 CW 有效由**页内 META + L2P 反查**
 *   逐 CW 现判（DRAM-less approach ③，见文件头）；本计数只服务 victim 选择。 */

/* 有效 CW 计数 +1（写成功、GC/WL 搬入新页时各 CW 调一次） */
static inline void ftl_block_add_valid(ftl_block_t *b)
{
    ASSERT(b != NULL);
    ASSERT(b->valid_count < (uint16_t)(PLANE_NUM * PAGES_PER_BLOCK * CW_NUM));
    b->valid_count++;
}

/* 有效 CW 计数 -1（覆盖写旧位置、GC/WL 搬离、TRIM、擦除前清账） */
static inline void ftl_block_sub_valid(ftl_block_t *b)
{
    ASSERT(b != NULL);
    ASSERT(b->valid_count > 0U);
    b->valid_count--;
}

/* 清账（擦除/坏块后调用）：valid_count=0（无位图可清） */
static inline void ftl_block_clear_valid(ftl_block_t *b)
{
    ASSERT(b != NULL);
    b->valid_count = 0U;
}

#endif /* FTL_BST_H */

#ifndef FTL_BST_H
#define FTL_BST_H

/*
 * Block State Table（BST）— 统一 per-Block 结构体数组
 *
 * 背景：
 *   块的擦除次数、坏块状态、read recovery 的 ECC 追踪分属不同模块，
 *   没有统一承载。本表把它们合并为单一数据源，避免同一块的状态多份维护。
 *
 * 索引语义：
 *   按「全局物理 Block 号」索引，gblock = plane * BLOCKS_PER_PLANE + block
 *   （0 .. FTL_BLOCK_TOTAL-1）。PBA 的 block 字段是 per-plane 号，
 *   通过 ftl_pba_gblock 统一转换为全局号——
 *   避免 plane1 与 plane0 的 block 号碰撞（历史 bug）。
 *
 * 存储位置：
 *   五张表由 HIL（main.c）各自独立定义、按实例独立放置；FTL 只持有
 *   `ftl_table_refs_s`（见 allocator.h）里的指针。
 *   多 LUN 前置：每实例一份表 = 每实例一组独立对象，本表代码零改动。
 *
 * 有效位（DRAM-less approach ③）：
 *   本仓**不**维护任何常驻有效位图。有效性 = 逐 CW 读**页内 META**（即本仓的「OOB」
 *   反向映射：每个 CW 头 4B 存本 CW 的扇区 LBA）→ **反查 L2P** 判该 CW 是否仍指向本
 *   物理位置。这就是主流 DRAM-less 的 flash reverse-mapping
 *   （DFTL / arXiv 1504.01666 / LeaFTL / VPFTL 同构）：逐页位图在 DRAM-less 下装不下
 *   （RAM ∝ 页数），且落 flash 后失效清位会引发大量 flash I/O ⇒ 主流不做。
 *   故本仓：
 *     · bst_block_s = 块级聚合条目（valid_count 是 **CW 级真值**），20B/块，全常驻；
 *     · 无独立位图表——GC/WL 迁移逐页读 META 反查 L2P 判定，不靠任何 RAM 位图。
 *   块级聚合（valid_count）供 GC/WL 选 victim（对齐主流 validity ratio）；逐 CW 真值现查 L2P。
 */

#include "assertion.h"   /* ASSERT（计数守卫；指针非空由内嵌数组结构性保证） */
#include "ftl_config.h"  /* DWORD, pba_to_*, NAND_PBA_INVALID */
#include "nand_config.h" /* BLOCKS_PER_PLANE, PLANE_NUM, CW_NUM */

#include <stddef.h> /* NULL */
#include <stdint.h>

/* ── 预测性坏块健康状态（自 read_recovery.h 迁入，避免循环 include）── */
typedef enum RR_BLOCK_HEALTH_E
{
    RR_BLOCK_HEALTHY  = 0, /* 正常 */
    RR_BLOCK_WARNING  = 1, /* ECC 上升趋势中 */
    RR_BLOCK_CRITICAL = 2, /* ECC 接近上限 */
    RR_BLOCK_DYING    = 3, /* 预计即将坏块，准备搬移 */
} rr_block_health_e;

/* ── ECC 健康记录（per-Block，自 read_recovery.h 迁入）──
 * 消费方：ftl_host_io_callback 读成功后 rr_record_ecc；
 *         rr_get_health 查询健康度。
 *
 * 只保留「上一次 ECC bit 数」而非 N 深度环形缓冲：趋势判定（连续上升）
 * 只需与上一次比较，8 格环形缓冲实际供给的是最老那格的值（本次一并修复）。
 * 16B → 3B，4096 块省 52KB。 */
typedef struct RR_ECC_TRACK_S
{
    BYTE              last_ecc;     /* 最近一次读的 ECC bit 数 */
    BYTE              rising_count; /* 连续上升计数（不上升即清零） */
    rr_block_health_e health;       /* 预测性退役趋势 */
} rr_ecc_track_s;                   /* 3B */

/* 磨损/ECC 快照：Checkpoint 的 per-block wear 条目与 BST 之间的搬运单元。
 * 把原本 5 个同类型/异类型出参收成一个结构体，避免调用点按位置传参写反。 */
typedef struct BST_WEAR_S
{
    DWORD          erase_count; /* 擦除次数 */
    WORD           read_count;  /* read disturb 计数 */
    rr_ecc_track_s rr;          /* ECC 健康趋势 */
} bst_wear_s;

/* ── bst_block_s 状态机（互斥态，枚举而非位图）──
 * 块**位置**同一时刻只能有一个，故用枚举而非位图：
 *   · 枚举保证互斥，非法组合不可表达；
 *   · 状态转移集中到 ftl_block_set_state，不需要手写清位掩码（漏一位编译期完全无声）。
 * BAD 是终态；`ftl_block_set_bad` 一并清 valid_count / sb_id（唯一 setter，
 * 调用点不再手写三行——原先 5 处各写一遍，漏一处就是静默账本分叉）。
 * 温度与位置**正交**（OPEN 块也有冷热），故单列字段，见 bst_block_s.temp。
 *
 * ⚠ 无「预擦除 / 未擦除」分级态：Free Pool 是**一个**已擦除池（GC 先擦后归还），
 *   三级是**水位**不是**分区**——见 allocator.h 的「Free Pool 三级水位」。 */
typedef enum BST_STATE_E
{
    FTL_BLOCK_STATE_NONE    = 0U, /* 无归属：固定元数据区 / OP 池 / Spare 池 / BSS 未初始化 */
    FTL_BLOCK_STATE_FREE    = 1U, /* 在 Free Pool（已擦除） */
    FTL_BLOCK_STATE_OPEN    = 2U, /* 属于某 Open SB（正在写） */
    FTL_BLOCK_STATE_CLOSED  = 3U, /* 属于某 Closed SB（等待 GC） */
    FTL_BLOCK_STATE_LOG     = 4U, /* L2P 日志块（动态元数据块，本池独占；metadata 回收判据） */
    FTL_BLOCK_STATE_RETIRED = 5U, /* 退役：不再分配新数据、不标坏、仍可读（read_recovery） */
    FTL_BLOCK_STATE_BAD     = 6U, /* 坏块（终态；合并 bad_blocks[] + bbt bitmap） */
} bst_state_e;

/* 状态存于 BYTE ⇒ 值域必须装得下（枚举成员一多就会静默截断） */
_Static_assert((unsigned)FTL_BLOCK_STATE_BAD <= (unsigned)UINT8_MAX, "bst_state_e 超出 BYTE 值域");

/* 枚举必须是 1 字节（-fshort-enums，见 config.mk）：否则 bst_block_s 会从 20B 涨到 24B，
 * 4096 个块多占 16KB，多 LUN 影子构建会超 SRAM12。 */
_Static_assert(sizeof(bst_state_e) == 1, "枚举不是 1 字节：检查 -fshort-enums");

/* ── Block State Table 每行（per-block 条目）──
 * 全字段常驻 SRAM（纯块级聚合，**无**位图表；有效性现查 META+L2P，见文件头）。
 * 只保留**有消费方**的字段：
 *
 *   erase_count   合并 allocator->erase_counts[]（allocator/gc/wl 消费）
 *   last_pe_ts    最近擦除时间戳（GC victim youth_penalty 消费）
 *   read_count    read disturb 计数（读成功++，擦除成功清零；read recovery 消费）
 *   valid_count   双 Plane 合计有效 **CW** 0..512（GC/WL victim 选真值，O(1)）
 *   state         状态机（bst_state_e，互斥态：NONE/FREE/OPEN/CLOSED/LOG/RETIRED/BAD）
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
typedef struct BST_BLOCK_S
{
    /* ── 磨损 ── */
    DWORD erase_count; /* 4B  @0   擦除次数 */
    DWORD last_pe_ts;  /* 4B  @4   最近擦除时间戳（WL 冷热避聚） */
    WORD  read_count;  /* 2B  @8   read disturb 计数 */

    /* ── 有效页聚合 + 状态 ── */
    WORD        valid_count; /* 2B  @10  双 Plane 合计有效 **CW** 0..512（逐 CW 真值，现查 META+L2P） */
    bst_state_e state;       /* 1B @12 状态机（互斥） */
    BYTE        temp;        /* 1B  @13  温度流（0=HOT / 1=COLD，同 FTL_TEMP_* 值域；正交于状态） */
    WORD        sb_id;       /* 2B  @14  block→SB 反向索引（UINT16_MAX=无） */

    /* ── ECC 健康 ── */
    rr_ecc_track_s rr; /* 3B  @16  ECC 健康 */
} bst_block_s;         /* 19B → 20B（u32 对齐） */

/* 布局守卫：字段顺序/类型漂移会让结构体悄悄变大，编译期拦住。 */
_Static_assert(sizeof(bst_block_s) == 20, "bst_block_s 布局漂移（期望 20B）");

/* ── 全局 Block 号转换（单点，修复 per-plane 碰撞的关键）── */

/**
 * @brief plane block + plane → 全局 0..4095。
 *
 * @param plane plane 号。
 * @param block 该 plane 内的块号。
 *
 * @return 全局块号。
 *
 * @note 前置条件：plane < PLANE_NUM 且 block < BLOCKS_PER_PLANE。
 *       传入 FTL_BLOCK_NO_PLANE / UINT16_MAX 等哨兵会截断成看似合法的 gblock；
 *       调用者必须先判 super_block_has_plane()，本 helper 不做运行期哨兵检查。
 */
static inline WORD ftl_gblock(BYTE plane, WORD block)
{
    ASSERT(plane < PLANE_NUM);
    ASSERT(block < BLOCKS_PER_PLANE);
    return (plane * BLOCKS_PER_PLANE + block);
}

/**
 * @brief PBA → 全局块号（直接按 PBA 位序解，不经 PAA）。
 *
 * @param pba 物理地址。
 *
 * @return 全局块号。
 */
static inline WORD ftl_pba_gblock(DWORD pba)
{
    return ftl_gblock(pba_to_plane(pba), pba_to_block(pba));
}

/**
 * @brief gblock → plane（ftl_gblock 的逆，单点解码）。
 *
 * @param g 全局块号。
 *
 * @return plane 号。
 */
static inline BYTE ftl_gblock_plane(WORD g)
{
    return (g / BLOCKS_PER_PLANE);
}

/**
 * @brief gblock → plane 内块号（ftl_gblock 的逆）。
 *
 * @param g 全局块号。
 *
 * @return plane 内块号（0..BLOCKS_PER_PLANE-1）。
 */
static inline WORD ftl_gblock_block(WORD g)
{
    return (g % BLOCKS_PER_PLANE);
}

/* ── 本文件全部访问器的入参约定 ──
 *
 * `b` 是**前置条件**（调用者传入，非可空参数）：
 * 调用者一律传 `&alloc->blocks[<索引>]`，而 `blocks` 指向分配到本实例的独立数组
 * （由 `ftl_table_refs_s` 绑定，生命周期覆盖全程）⇒ 取元素地址**结构性恒非空**。
 * 全仓调用点已逐一核实无一传 NULL。
 * ⇒ 指针非空由内嵌数组结构性保证，不保留 `if (b != NULL)` 恒真守卫；
 *   计数越界仍用 `ASSERT`（H14：debug 停机留现场，release 裁空 ⇒ 零开销）。 */

/* ── 状态机访问器（枚举赋值，无位运算）──
 *
 * ⚠ H15（位运算与位判断走公共宏）在本字段**不适用**：拍平后没有任何位掩码/位标志运算
 *   ⇒「任一位置位 vs 全部位置位」那类歧义消失（原 `FTL_BLOCK_STATE_MASK` 零调用者，已删）。 */

/**
 * @brief 取块状态（互斥枚举）。
 *
 * @param b 块条目。
 *
 * @return 当前状态。
 */
static inline bst_state_e ftl_block_state(const bst_block_s *b)
{
    return (bst_state_e)b->state;
}

/**
 * @brief 设置块状态（状态机唯一 setter）。
 *
 * @param b  块条目。
 * @param st 目标状态。
 */
static inline void ftl_block_set_state(bst_block_s *b, bst_state_e st)
{
    b->state = st;
}

/* ── 坏块（BST 单数据源；NAND 持久化仍走 bbt_header_s.bad_map）──
 * 坏块是状态机的**终态**，不再是独立位 ⇒ 与其余 6 态天然互斥
 * （原「BAD|RESERVED」并存不再可表达）。 */

/**
 * @brief 是否坏块（BAD 为终态）。
 *
 * @param b 块条目。
 *
 * @return true = BAD。
 */
static inline BOOL ftl_block_is_bad(const bst_block_s *b)
{
    return ftl_block_state(b) == FTL_BLOCK_STATE_BAD;
}

/**
 * @brief 温度流：分配时记录 hot/cold（0=HOT / 1=COLD，与 allocator 的 FTL_TEMP_* 同值域）。
 * 不在本头引用 FTL_TEMP_*（避免 allocator.h 循环 include），直接用值语义；
 * **不另立 `ftl_block_temp_t` 枚举**——同值域再来一套名字就是第二套坐标（C9）。
 * 温度与状态**正交** ⇒ `ftl_block_set_state` 不碰本字段；每次分配都显式写入，
 * 故 FREE/BAD 块上残留的温度不会被任何读者读到（`ftl_block_get_temp` 生产侧零读者）。
 *
 * @param b           块条目。
 * @param temperature 温度流（0=HOT / 1=COLD）。
 */
static inline void ftl_block_set_temp(bst_block_s *b, BYTE temperature)
{
    b->temp = temperature;
}

/**
 * @brief 取块的温度流（0=HOT / 1=COLD）。
 *
 * ⚠ 返回值是**两值域不是真值域**，不是 BOOL（UT 逐条钉住该值域）。
 *
 * @param b 块条目。
 *
 * @return 温度流（0=HOT / 1=COLD）。
 */
static inline BYTE ftl_block_get_temp(const bst_block_s *b)
{
    return b->temp;
}

/**
 * @brief 状态名（诊断日志用）。两个调用者：ftl.c 的 dangling 守卫、controller/cli.c 的 map。
 * H7：switch 必须有 default（非枚举值只可能来自内存损坏，返回 "?" 而不是静默取一支）。
 *
 * @param st 块状态。
 *
 * @return 状态名（非法值返回 "?"）。
 */
static inline const char *ftl_block_state_name(bst_state_e st)
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

/**
 * @brief 取块所属 Super Block 下标。
 *
 * @param b 块条目。
 *
 * @return SB 下标；FTL_BLOCK_NO_SB = 未归属。
 */
static inline WORD ftl_block_sb_id(const bst_block_s *b)
{
    return b->sb_id;
}

/**
 * @brief 设置块所属 Super Block 下标。
 *
 * @param b     块条目。
 * @param sb_id SB 下标；FTL_BLOCK_NO_SB = 未归属。
 */
static inline void ftl_block_set_sb(bst_block_s *b, WORD sb_id)
{
    b->sb_id = sb_id;
}

/* ── 有效 CW 计数（块级聚合真值）──
 *
 * CW 级映射（IU = 1 CW = 512B）下，一个物理页内 4 个 CW 各自独立有效：
 * 主机只改写其中 1 个 CW 时，同页另 3 个 CW 仍然有效。
 *
 * valid_count（在 bst_block_s 内）是**真值**，单位 **CW**，值域
 * 0 .. FTL_CW_PER_BLOCK（单物理块，当前 0..256）。每次「某 LBA 的映射
 * 指向本块某页某 CW」+1、「某 LBA 不再指向本块某页某 CW」-1，与 L2P 逐条同步。
 * SB 层的 `valid_cw` 是各 plane 之和（0..FTL_CW_PER_SB，当前 0..512），只用于
 * victim 评分；**不要**拿它作为单块 `valid_count` 的上界。
 *
 * ⚠ 本仓**无**「写过」位图：页内哪些 CW 有效由**页内 META + L2P 反查**
 *   逐 CW 现判（DRAM-less approach ③，见文件头）；本计数只服务 victim 选择。 */

/**
 * @brief 有效 CW 计数 +1（写成功、GC/WL 搬入新页时各 CW 调一次）。
 *
 * @param b 块条目。
 */
static inline void ftl_block_add_valid(bst_block_s *b)
{
    ASSERT(b->valid_count < FTL_CW_PER_BLOCK);
    b->valid_count++;
}

/**
 * @brief 有效 CW 计数 -1（覆盖写旧位置、GC/WL 搬离、TRIM、擦除前清账）。
 *
 * @param b 块条目。
 */
static inline void ftl_block_sub_valid(bst_block_s *b)
{
    ASSERT(b->valid_count > 0U);
    b->valid_count--;
}

/**
 * @brief 仅在块仍可记账时 -1
 *
 * 为什么不能直接调 ftl_block_sub_valid：
 *   - `bbt_mark_bad()` 会把 `valid_count` 清 0（坏块不再产生有效 CW）；
 *   - 但 L2P 可能仍指向该块（预测性迁移读失败后标坏 / full metadata rebuild 漏认）。
 * 这时覆盖写旧位置或 TRIM 再减一次，debug 下会撞 `valid_count > 0` 断言，
 * release 下 `WORD` 回绕成 65535 ⇒ valid_count 虚高，污染 GC/WL 的 victim 评分。
 *
 * 判据 = 块不是 BAD **且** count > 0；否则跳过（调用者负责把 dangling 记进日志）。
 * 与 migrate.c / power_failure_recovery.c 的源块减计数守卫同一口径（C2/R9）。
 *
 * @param b 块条目。
 */
static inline void ftl_block_sub_valid_if_live(bst_block_s *b)
{
    if ((!ftl_block_is_bad(b)) && (b->valid_count > 0U))
    {
        ftl_block_sub_valid(b);
    }
}

/**
 * @brief 清账（擦除/坏块后调用）：valid_count=0（无位图可清）。
 *
 * @param b 块条目。
 */
static inline void ftl_block_clear_valid(bst_block_s *b)
{
    b->valid_count = 0U;
}

/**
 * @brief 直接设置 `valid_count`（块元数据快照恢复 / 重建用；日常增量仍走 add/sub）。
 * ⚠ 上界是**单物理块**的 CW 数（FTL_CW_PER_BLOCK），不是 SB 合计。
 *
 * @param b     块条目。
 * @param valid 有效 CW 数（0..FTL_CW_PER_BLOCK）。
 */
static inline void ftl_block_set_valid(bst_block_s *b, WORD valid)
{
    b->valid_count = valid;
}

/**
 * @brief 置坏块（终态）：同时清有效计数与 SB 归属。
 *
 * BAD 是状态机的**终态**：残留 `valid_count` 会让 GC/WL 误判该块可回收，
 * 残留 `sb_id` 会让 Closed 条目指向已退役块。三件事必须同时发生 ⇒ 收进唯一
 * setter（C2；原先 5 个调用点各写三行，漏一处就是静默账本分叉）。
 *
 * @param b 块条目。
 */
static inline void ftl_block_set_bad(bst_block_s *b)
{
    ftl_block_set_state(b, FTL_BLOCK_STATE_BAD);
    ftl_block_clear_valid(b);
    ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
}

#endif /* FTL_BST_H */

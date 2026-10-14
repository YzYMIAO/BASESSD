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

#include "common/assertion.h" /* ASSERT（add/sub_valid 的计数守卫） */
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

/* ── ftl_block_t 状态位（bitmask 宏，非 C 位域——
 *   嵌入式可移植（位域布局实现定义），支持位运算，配访问器） ── */
enum
{
    FTL_BLOCK_STATE_BAD       = 0x0001U, /* 坏块（合并 bad_blocks[] + bbt bitmap） */
    FTL_BLOCK_STATE_OPEN      = 0x0002U, /* 属于某 Open SB（正在写） */
    FTL_BLOCK_STATE_CLOSED    = 0x0004U, /* 属于 Closed SB（等待 GC） */
    FTL_BLOCK_STATE_FREE      = 0x0008U, /* 在 Free Pool（已擦除） */
    FTL_BLOCK_STATE_RESERVED  = 0x0010U, /* Spare Pool / 元数据保留块 */
    FTL_BLOCK_STATE_TEMP_HOT  = 0x0020U, /* 温度流：Hot（super_block_t.temperature 下沉） */
    FTL_BLOCK_STATE_TEMP_COLD = 0x0040U, /* 温度流：Cold */
    /* ⚠ bit7/bit8（原 PRE_COND / EMERGENCY）已删除（P1-3）：Free Pool 是一个已擦除池，
     *   池内无「预擦除 / 未擦除」之分（GC 先擦后归还），故这两个分级位从未有过读者。
     *   三级是**水位**不是**分区**——见 allocator.h 的「Free Pool 三级水位」。 */
};

/* 全部状态位掩码（bit0..6） */
#define FTL_BLOCK_STATE_MASK (0x007FU)

/* 温度掩码/提取（bit5-6，显式两 bit，可扩展多温区） */
#define FTL_BLOCK_TEMP_MASK (FTL_BLOCK_STATE_TEMP_HOT | FTL_BLOCK_STATE_TEMP_COLD)

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
 *   state         状态机（BAD/OPEN/CLOSED/FREE/RESERVED）
 *                 + 温度（HOT/COLD），bitmask 宏 + 访问器
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
    uint16_t state;       /* 2B  @12  状态机+温度（bitmask 宏 + 访问器） */
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

/* ── 坏块位操作（BST 单数据源；NAND 持久化仍走 bbt_header_t.bad_map）── */

static inline int ftl_block_is_bad(const ftl_block_t *b)
{
    return (b != NULL) && ((b->state & FTL_BLOCK_STATE_BAD) != 0);
}

static inline void ftl_block_set_bad(ftl_block_t *b)
{
    if (b != NULL)
    {
        b->state |= FTL_BLOCK_STATE_BAD;
    }
}

/* ── 状态机访问器（bitmask 宏 + 可读调用，替代位域/联合体）── */

static inline int ftl_block_state_is(const ftl_block_t *b, uint16_t st)
{
    return (b != NULL) && ((b->state & st) == st);
}

static inline void ftl_block_state_set(ftl_block_t *b, uint16_t st)
{
    if (b != NULL)
    {
        b->state |= st;
    }
}

static inline void ftl_block_state_clr(ftl_block_t *b, uint16_t st)
{
    if (b != NULL)
    {
        b->state &= (uint16_t)~st;
    }
}

/* 温度流：分配时记录 hot/cold（0=HOT / 1=COLD，与 allocator FTL_TEMP_* 对齐）。
 * 不在本头引用 FTL_TEMP_*（避免 allocator.h 循环 include），直接用值语义。 */
static inline void ftl_block_set_temp(ftl_block_t *b, uint8_t temperature)
{
    if (b != NULL)
    {
        b->state &= (uint16_t)~FTL_BLOCK_TEMP_MASK;
        b->state |= (temperature == 1U) ? FTL_BLOCK_STATE_TEMP_COLD
                                        : FTL_BLOCK_STATE_TEMP_HOT;
    }
}

static inline uint8_t ftl_block_get_temp(const ftl_block_t *b)
{
    if (b == NULL)
    {
        return 0U; /* HOT */
    }
    return ((b->state & FTL_BLOCK_STATE_TEMP_COLD) != 0U) ? 1U : 0U;
}

/* 反向索引 sb_id（UINT16_MAX = 未归属任何 SB） */
#define FTL_BLOCK_NO_SB (UINT16_MAX)

static inline uint16_t ftl_block_sb_id(const ftl_block_t *b)
{
    return (b != NULL) ? b->sb_id : FTL_BLOCK_NO_SB;
}

static inline void ftl_block_set_sb(ftl_block_t *b, uint16_t sb_id)
{
    if (b != NULL)
    {
        b->sb_id = sb_id;
    }
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
    if (b != NULL)
    {
        ASSERT(b->valid_count < (uint16_t)(PLANE_NUM * PAGES_PER_BLOCK * CW_NUM));
        b->valid_count++;
    }
}

/* 有效 CW 计数 -1（覆盖写旧位置、GC/WL 搬离、TRIM、擦除前清账） */
static inline void ftl_block_sub_valid(ftl_block_t *b)
{
    if (b != NULL)
    {
        ASSERT(b->valid_count > 0U);
        b->valid_count--;
    }
}

/* 清账（擦除/坏块后调用）：valid_count=0（无位图可清） */
static inline void ftl_block_clear_valid(ftl_block_t *b)
{
    if (b != NULL)
    {
        b->valid_count = 0U;
    }
}

#endif /* FTL_BST_H */

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
 *   （0 .. FTL_BLOCK_TOTAL-1）。PBA/PAA 的 block 字段是 per-plane 号，
 *   通过 ftl_paa_gblock / ftl_pba_gblock 统一转换为全局号——
 *   避免 plane1 与 plane0 的 block 号碰撞（历史 bug）。
 *
 * 存储位置：
 *   数组由调用者（host 层）通过 ftl_init_params_t.blocks 提供，
 *   与 free_blocks / closed_sb_list 的「调用者提供大数组」契约一致。
 *   多 LUN 前置：每 LUN 一份表 = host 声明 g_blocks[LUN_NUM]，
 *   params 按 LUN 传入，本表代码零改动。
 */

#include "nand/nand_config.h" /* BLOCKS_PER_PLANE, PLANE_NUM, nand_paa_t, nand_pba_t, paa_to_* */

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
 * 消费方：ftl_handle_completion 读成功后 rr_record_ecc；
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
    FTL_BLOCK_STATE_PRE_COND  = 0x0080U, /* Pre-conditioned Pool 分级（三级 Free Pool 预铺） */
    FTL_BLOCK_STATE_EMERGENCY = 0x0100U, /* Emergency Pool 分级 */
};

/* 全部状态位掩码（bit0..8） */
#define FTL_BLOCK_STATE_MASK (0x01FFU)

/* 温度掩码/提取（bit5-6，显式两 bit，可扩展多温区） */
#define FTL_BLOCK_TEMP_MASK (FTL_BLOCK_STATE_TEMP_HOT | FTL_BLOCK_STATE_TEMP_COLD)

/* ── 坏块原因（独立 1B 字段 bad_reason） ── */
enum
{
    FTL_BLOCK_BAD_FACTORY = 0, /* 出厂坏块 */
    FTL_BLOCK_BAD_PROGRAM,     /* PROGRAM 失败 */
    FTL_BLOCK_BAD_ERASE,       /* ERASE 失败 */
    FTL_BLOCK_BAD_READ,        /* 读失败 / ECC 超限 */
    FTL_BLOCK_BAD_GC_MOVE,     /* GC 迁移失败 */
    FTL_BLOCK_BAD_TRIM,        /* TRIM 后坏块 */
};

/* ── Block State Table 每行 ──
 * 全字段常驻 SRAM（行业 Block Info Table 标准形态，功能优先）。
 * 字段全部有明确消费方 / 后续扩展铺垫：
 *
 *   erase_count   合并 allocator->erase_counts[]（allocator/gc/wl 消费）
 *   last_pe_ts    最近擦除时间戳（企业级 WL 冷热避聚，gc_erase 写）
 *   read_count    read disturb 计数（读成功++，后续 read disturb 搬移预铺）
 *   program_count program 计数（健康模型，写成功++）
 *   valid_count   双 Plane 合计有效页 0..128（GC/WL victim 选真值，O(1)）
 *   state         状态机（BAD/OPEN/CLOSED/FREE/RESERVED/PRE_COND/EMERGENCY）
 *                 + 温度（HOT/COLD），bitmask 宏 + 访问器
 *   bad_reason    坏块原因（FACTORY/PROGRAM/ERASE/READ/GC_MOVE/TRIM）
 *   gc_generation GC 搬移代数（迁移成功++，新块继承 0）
 *   sb_id         block→SB 反向索引（UINT16_MAX=未归属，allocator 维护）
 *   valid_map     per-Plane 逐页有效位图（bit p = 该 plane 第 p 页有效），
 *                 GC/WL 迁移直接扫置位页搬移，免 NAND 读——落地
 *                 设计文档 P-C.5「反向映射 bitmap」。
 *   rr            ECC 健康（last_ecc + rising_count + health，read recovery 供电）
 *
 *   维护点（全真值）：
 *     ① 写成功：新页置位 + VPC++；覆盖写清旧页（old_pba）+ VPC--；program_count++
 *     ② 读成功：read_count++（ECC 记录 rr）
 *     ③ GC/WL 迁移成功：旧页清位 + VPC--；新页置位 + VPC++；gc_generation++
 *     ④ 擦除成功：bitmap 清空 + VPC=0 + last_pe_ts 写 + 状态 FREE
 *     ⑤ trim：清旧页 bitmap/VPC
 *     ⑥ 坏块：置 BAD + clear_valid + bad_reason
 *     ⑦ 掉电恢复：写指针扫描重建 bitmap（power_failure_recovery）
 *
 *   字段顺序：rr 必须排在 valid_map **之前**。valid_map 是 uint64_t 数组，
 *   需要 8 字节对齐；rr 只有 3B，若留在数组之后，结构体会被尾部对齐从 40B
 *   撑到 48B（白丢 8B/块 = 32KB）。前移后 valid_map 仍落在 @24。
 *
 *   sizeof = 40B → 4096 项 = 160KB（全常驻 SRAM；档三分层见文档 §19）。
 */
typedef struct
{
    /* ── 磨损 ── */
    uint32_t erase_count;   /* 4B  @0   擦除次数 */
    uint32_t last_pe_ts;    /* 4B  @4   最近擦除时间戳（WL 冷热避聚） */
    uint16_t read_count;    /* 2B  @8   read disturb 计数 */
    uint16_t program_count; /* 2B  @10  program 计数（健康模型） */

    /* ── 有效页 + 状态 ── */
    uint16_t valid_count;   /* 2B  @12  双 Plane 合计有效页 0..128 */
    uint16_t state;         /* 2B  @14  状态机+温度（bitmask 宏 + 访问器） */
    uint8_t  bad_reason;    /* 1B  @16  坏块原因（独立字段） */
    uint8_t  gc_generation; /* 1B  @17  GC 搬移代数 */
    uint16_t sb_id;         /* 2B  @18  block→SB 反向索引（UINT16_MAX=无） */

    /* ── ECC 健康（必须在 valid_map 之前，见上方字段顺序说明）── */
    rr_ecc_track_t rr;   /* 3B  @20  ECC 健康 */
    uint8_t        _pad; /* 1B  @23  对齐补（valid_map 需 8B 对齐） */

    /* ── 逐页有效位图 ── */
    uint64_t valid_map[PLANE_NUM]; /* 16B @24  per-Plane 逐页有效位图 */
} ftl_block_t;

/* 布局守卫：字段顺序/类型漂移会让结构体悄悄变大，编译期拦住。 */
_Static_assert(sizeof(ftl_block_t) == 40, "ftl_block_t 布局漂移（期望 40B）");

/* ── 全局 Block 号转换（单点，修复 per-plane 碰撞的关键）── */

/* per-plane block + plane → 全局 0..4095 */
static inline uint16_t ftl_gblock(uint8_t plane, uint16_t block)
{
    return (uint16_t)((uint32_t)plane * BLOCKS_PER_PLANE + block);
}

static inline uint16_t ftl_paa_gblock(nand_paa_t paa)
{
    return ftl_gblock((uint8_t)paa_to_plane(paa), (uint16_t)paa_to_block(paa));
}

static inline uint16_t ftl_pba_gblock(nand_pba_t pba)
{
    return ftl_paa_gblock(pba_to_paa(pba));
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

/* 坏块原因（独立 1B 字段） */
static inline uint8_t ftl_block_bad_reason(const ftl_block_t *b)
{
    return (b != NULL) ? b->bad_reason : (uint8_t)FTL_BLOCK_BAD_FACTORY;
}

static inline void ftl_block_set_bad_reason(ftl_block_t *b, uint8_t reason)
{
    if (b != NULL)
    {
        b->bad_reason = reason;
    }
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

/* ── 有效页位图操作（bitmap + valid_count 单点维护，防漂移）── */

/* 置位一页（第 plane 的 page 页有效）。pages=2 时对两个 plane 各调一次。 */
static inline void ftl_block_set_valid(ftl_block_t *b, uint8_t plane, uint8_t page)
{
    if ((b != NULL) && (plane < PLANE_NUM) && (page < PAGES_PER_BLOCK))
    {
        if ((b->valid_map[plane] & (1ULL << page)) == 0U)
        {
            b->valid_map[plane] |= (1ULL << page);
            b->valid_count++;
        }
    }
}

/* 清位一页（该页被覆盖/迁移/擦除后无效） */
static inline void ftl_block_clr_valid(ftl_block_t *b, uint8_t plane, uint8_t page)
{
    if ((b != NULL) && (plane < PLANE_NUM) && (page < PAGES_PER_BLOCK))
    {
        if ((b->valid_map[plane] & (1ULL << page)) != 0U)
        {
            b->valid_map[plane] &= ~(1ULL << page);
            b->valid_count--;
        }
    }
}

/* 查询一页是否有效（GC/WL 迁移直接用它，免 NAND 读） */
static inline int ftl_block_is_valid(const ftl_block_t *b, uint8_t plane, uint8_t page)
{
    return (b != NULL) && (plane < PLANE_NUM) && (page < PAGES_PER_BLOCK) &&
           ((b->valid_map[plane] & (1ULL << page)) != 0U);
}

/* 清除整块位图（擦除/坏块后调用），并同步 valid_count=0 */
static inline void ftl_block_clear_valid(ftl_block_t *b)
{
    if (b != NULL)
    {
        uint8_t p;
        for (p = 0U; p < PLANE_NUM; p++)
        {
            b->valid_map[p] = 0ULL;
        }
        b->valid_count = 0U;
    }
}

#endif /* FTL_BST_H */

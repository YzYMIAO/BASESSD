/*
 * metadata.c — 元数据落盘基建：record 池环形轮转 + Anchor 池 + L2P 日志块
 *
 * 职责：
 *   提供 FTL 全部持久化元数据（Anchor / Checkpoint / BBT / L2P region）共用的
 *   落盘原语：环形 record 池的寻址/定位/换块擦除；Anchor（Metadata Directory 快照）
 *   的组页/落盘/加载；L2P 日志块的追加槽/登记；以及"这块是不是元数据块"的判定。
 *   日志块的**回收与压缩**状态机在 metadata_job.c（本文件只负责登记待回收位图）。
 *
 * 与 metadata.h 的分工：
 *   metadata.h —— 结构（md_ring_t / md_anchor_page_t / md_dir_cache_t）+
 *                 固定元数据区布局（池尺寸 / 块偏移 / 表字节 / 分区表声明）；
 *   本文件     —— 上述结构的全部算法实现 + 分区表本体（单一事实来源）。
 *
 * L2P region 的位置只有一层来源（见 metadata.h「固定元数据区布局」）：
 *   Directory 条目 = `NAND_PBA_INVALID` ⇒ region 从未写过（未映射，读取不碰 NAND）；
 *   非 INVALID ⇒ region 页在日志块上的真实 PBA（从固定 L2P 日志区取块、追加写）。
 *
 * 文件结构：
 *   〇、文件级类型与常量（探针上下文 / 池策略表 / 固定元数据分区表）
 *   一、元数据 record 池（环形轮转）—— Anchor / Checkpoint / BBT 共用
 *   二、Anchor 池（Metadata Directory 的 record 池）
 *   三、Directory 基础层 / 查询 / 更新
 *   四、L2P 日志块：追加槽 / 登记
 *   五、Anchor 落盘 / 加载
 *   六、元数据块判定
 *   七、池诊断与服务
 *
 * ⚠ 同步 / 异步的分界：
 *   · **异步**：日志块的回收与压缩（`md_handle_t` + g_op_callbacks[FTL_OP_METADATA]，
 *      状态机在 metadata_job.c）。待回收块由 commit_region / append_slot 登记进
 *      `reclaim_pending` 位图，运行期没有任何同步擦除。
 *   · **同步（刻意）**：开机加载 / 格式化（md_load / init）、Anchor 池定位
 *      （md_ring_locate）——前者在 superloop 之前跑（无泵可等），后者只在
 *      冷启动后首次整理出现。
 *
 * 断言口径：init/bind 入口与 release 关键边界用 ASSERT；运行期模块入口的实例句柄/
 * 出参由调用链保证，不再逐次复检；ring 几何非法、槽位越界等配置错误在 release 也
 * 必须拒绝，因此保留 FTL_ERROR 返回。
 */

#include "metadata.h"

#include "assertion.h"
#include "bbt.h"
#include "checkpoint.h"
#include "fc.h"
#include "ftl.h" /* ftl_ctx_t（服务函数要从实例取 &ftl->alloc） */
#include "ftl_fc_interface.h"
#include "ftl_job.h" /* 心跳（[JOB] 观测口）*/
#include "ftl_op.h"  /* ftl_op_submit / FTL_OP_METADATA */
#include "journal.h"
#include "log.h"
#include "metadata_job.h" /* slot_now / job_reset / 待回收位图（与 metadata_job.c 共用） */
#include "onfi_1_0.h"
#include "ssd_compiler.h" /* SSD_CODE_FAST：本节函数级放置到 Flash */

#include <stddef.h>
#include <string.h>

// 每实例 Metadata Directory cache（CPU-only）。链接脚本按整个 metadata.o 的 .bss
// 放进 SRAM4（与 BCH 交换：BCH 在 DTCM）；`.sram4_data` 是 NOLOAD ⇒ ftl_init
// 绑定后对该实例显式 memset。
md_dir_cache_t g_ftl_md[FTL_DIM_TOTAL];

// 候选池重建日志节流：第 1 次立即打印，此后每 N 次一条（元数据坏块风暴不刷屏）。
// N=16：重建本身较贵且罕见，16 条足以覆盖一次故障风暴。
// ⚠ 位于 DTCM NOLOAD ⇒ 必须在 md_init 显式复位。
#define MD_POOL_REBUILT_LOG_EVERY (16U)
static DWORD s_pool_rebuilt_logged;

/* ══════════════════════════════════════════════════════════════════
 * 文件级类型与常量
 * ══════════════════════════════════════════════════════════════════ */

// Anchor 池探针上下文：record 扫描同时需要 Directory 与池几何。
typedef struct
{
    const md_dir_cache_t *dir;
    const md_ring_t      *ring;
} anchor_probe_ctx_t;

// 单个固定池的几何/补齐策略（池内容在 md_pool_build_all 里构建）。
typedef struct
{
    WORD first;
    WORD nominal;
    BYTE pages_per_record;
    BYTE plane_mask;
    WORD min_blocks;
    WORD spare_budget;
} md_pool_spec_t;

// 固定池的行号（表下标）。
typedef enum
{
    MD_POOL_BBT = 0,
    MD_POOL_CP,
    MD_POOL_JRN,
    MD_POOL_ANCHOR,
    MD_POOL_NUM,
} md_pool_id_e;

// 固定池的几何/补齐策略（数组表；加池时只加一行）。
static const md_pool_spec_t s_pool_specs[] = {
  // 列序：first / nominal / pages_per_record / plane_mask / min_blocks / spare_budget
    {        BBT_BLOCK,        BBT_POOL_BLOCKS,                          1U, METADATA_MIRROR_PLANE_MASK,                  2U, 2U},
    {CHECKPOINT_BLOCK0, CHECKPOINT_POOL_BLOCKS, FTL_CHECKPOINT_RECORD_PAGES, METADATA_MIRROR_PLANE_MASK,                  2U, 1U},
    {   JOURNAL_BLOCK0,    JOURNAL_BLOCK_COUNT,                          1U, METADATA_MIRROR_PLANE_MASK, JOURNAL_BLOCK_COUNT, 2U},
    {    ANCHOR_BLOCK0,     ANCHOR_POOL_BLOCKS,           ANCHOR_PAGE_COUNT, METADATA_MIRROR_PLANE_MASK,                  2U, 0U},
};

// 加池时枚举与表必须同时加一行：行数不符即编译失败。
_Static_assert((sizeof(s_pool_specs) / sizeof(s_pool_specs[0])) == (size_t)MD_POOL_NUM, "s_pool_specs rows must match md_pool_id_e");

// 一次池扫描的中间结果（①~③ 步）。
typedef struct
{
    WORD  cursor_block;  /* 第一个未写满的块；全满时为"最新满块之后" */
    WORD  best_full_idx; /* 全部满块末条中 sequence 最大者（UINT16_MAX = 无）*/
    DWORD best_full_seq;
    WORD  cursor_last_idx; /* 游标块内最后一条有效 record（UINT16_MAX = 无）*/
    DWORD cursor_last_seq;
    bool  ring_full; /* 池已写满一圈 */
} md_ring_scan_t;

// 固定元数据分区表 —— 单一事实来源（类型声明在 metadata.h）
//
// 区间为 [first_block, first_block + block_count)；**各 (ch,ce,lun) 用同一份块号**
// （分片模型：每个 die 一套完整元数据，分区不随维度错开）。
//
// Anchor / Checkpoint / BBT 都是**多块轮转池**：池内每块容纳 `records_per_block`
// 条 record，块满换池内下一块（先擦后写）。
//
// 每项完全由 metadata.h 的宏派生——块号 / 块数 / 每块 record 数 / 表字节均无手写数字。
static const md_partition_t s_md_partitions[] = {
  // name, first_block, block_count, plane_mask, redundancy, table_bytes
    {    "Anchor",     ANCHOR_BLOCK0,       ANCHOR_POOL_BLOCKS, METADATA_MIRROR_PLANE_MASK,         ANCHOR_RECORDS_PER_BLOCK,                       FTL_ANCHOR_TABLE_BYTES},
    {"Checkpoint", CHECKPOINT_BLOCK0,   CHECKPOINT_POOL_BLOCKS, METADATA_MIRROR_PLANE_MASK, FTL_CHECKPOINT_RECORDS_PER_BLOCK,                   FTL_CHECKPOINT_TABLE_BYTES},
    {       "BBT",         BBT_BLOCK,          BBT_POOL_BLOCKS, METADATA_MIRROR_PLANE_MASK,                  PAGES_PER_BLOCK,                          FTL_BBT_TABLE_BYTES},
    {   "Journal",    JOURNAL_BLOCK0,      JOURNAL_BLOCK_COUNT, METADATA_MIRROR_PLANE_MASK,                               1U,                      FTL_JOURNAL_TABLE_BYTES},
 // L2P region 日志区：固定预留（RISK-MD-07/CAP-02）。redundancy 填 1（不是 record 池）；
  // table_bytes = region 数 × 每个 region 的 2 KB。
    {   "L2P log",    L2P_LOG_BLOCK0, L2P_LOG_BLOCKS_PER_PLANE, METADATA_MIRROR_PLANE_MASK,                               1U, (L2P_REGIONS_PER_INSTANCE * L2P_REGION_SIZE)},
};

// 分区数（表长）。sizeof 派生 ⇒ 加/删分区项时自动跟随。
static const DWORD s_md_partition_count = (sizeof(s_md_partitions) / sizeof(s_md_partitions[0]));

/* ══════════════════════════════════════════════════════════════════
 * 一、元数据 record 池（环形轮转）—— Anchor / Checkpoint / BBT 共用
 * ══════════════════════════════════════════════════════════════════ */

/**
 * @brief ring 几何自检：任何一条不满足都会让地址公式除零或产生越界槽位。
 * 这是**配置错误** ⇒ 调用者在 release 也必须拿到 FTL_ERROR（判据 9）。
 * 不要求 PAGES_PER_BLOCK % pages_per_record == 0：Checkpoint 的 21 页/record
 * 刻意向下取整，块尾剩余页不用。
 *
 * @param ring 待校验的池几何
 * @return true = 几何合法；false = 非法
 */
static bool md_ring_geom_valid(const md_ring_t *ring)
{
    const BYTE valid_plane_bits = ((1U << PLANE_NUM) - 1U);

    if ((ring->block_count < 2U) || (ring->pages_per_record == 0U) || (ring->pages_per_record > PAGES_PER_BLOCK))
    {
        return false;
    }
    if ((ring->plane_mask == 0U) || ((ring->plane_mask & ~valid_plane_bits) != 0U))
    {
        return false; /* 必须至少用一个 plane，且不得指向不存在的 plane */
    }

    const WORD rpb = (PAGES_PER_BLOCK / ring->pages_per_record);

    if ((ring->block_count * rpb) > UINT16_MAX)
    {
        return false; /* 槽位总数必须装进 WORD（写游标 / record 下标都是 16 位）*/
    }
    if ((ring->block_count > MD_RING_MAX_BLOCKS) || (ring->nominal_count > MD_RING_MAX_BLOCKS))
    {
        return false; /* 候选表容量 */
    }
    for (WORD i = 0U; i < ring->block_count; i++)
    {
        if (ring->blocks[i] >= BLOCKS_PER_PLANE)
        {
            return false; /* 候选块号越出本 plane */
        }
    }

    return true;
}

// 实现要点：先按 records_per_block 整除得**候选下标**、取余得块内记录号，
// 再查 `ring->blocks[]` 得到物理块号，乘 pages_per_record 得块内页号。
// 地址公式的唯一出口；候选池让同一 record_idx 在不同 BAD 集下落到
// 不同物理块，但 **sequence 扫描定位**不依赖固定块号。
// `md_ring_addr` 是"取 plane_mask 里第一个 plane"的薄入口，
// 镜像池的读回退/写镜像用 `_plane` 版本。
DWORD md_ring_addr_plane(const md_ring_t *ring, const ftl_topo_t *topo, WORD record_idx, BYTE page_in_record, BYTE plane)
{
    if (md_ring_geom_valid(ring) == false)
    {
        return NAND_PBA_INVALID; /* 配置错误：不产生"看起来合法"的错误地址 */
    }

    const WORD rpb = md_ring_records_per_block(ring);

    if (record_idx >= (ring->block_count * rpb))
    {
        return NAND_PBA_INVALID; /* 槽位越界：release 也必须拒绝（判据 9）*/
    }

    const DWORD record_in_block = (DWORD)(record_idx % rpb);
    const DWORD block           = ring->blocks[record_idx / rpb];
    const DWORD page            = (record_in_block * ring->pages_per_record) + page_in_record;

    return pba_make(topo->ch, topo->ce, topo->lun, block, plane, page, 0U);
}

/* ══════════════════════════════════════════════════════════════════
 * 固定元数据分区的候选块池（BBT + shared spare）
 * ══════════════════════════════════════════════════════════════════ */

/**
 * @brief 物理块在 plane_mask 的所有 plane 上都非 BAD。
 * 镜象/多 plane 擦写要求**同一物理块号**在每个 plane 上都可用，否则
 * ftl_async_write_page_mirror / multi-plane erase 会写到坏 plane 的同号块。
 *
 * @param alloc      分配器（读 BST 坏块标记）
 * @param block      per-plane 块号
 * @param plane_mask 需要全部可用的 plane 位掩码
 * @return true = mask 内所有 plane 的同号块都非 BAD；false = 至少一个 BAD
 */
static bool md_block_good_on_mask(const alc_t *alloc, WORD block, BYTE plane_mask)
{
    const BYTE n = md_ring_plane_count(plane_mask);

    for (BYTE i = 0U; i < n; i++)
    {
        const BYTE plane = md_ring_plane_at(plane_mask, i);
        const WORD g     = ftl_gblock(plane, block);

        if (ftl_block_is_bad(&alloc->blocks[g]))
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 收名义区间的好块；不足 min_blocks 时从 shared spare 按升序补（受 spare_budget
 * 与全局 spare_used 双重限制）。**只补到硬下界，不强行补满名义数**：CP 少一块
 * 仍可轮转，把 spare 留给更紧的 BBT/Journal。
 *
 * @param ring       输出：构建好的池几何
 * @param alloc      分配器（读 BST 坏块标记）
 * @param spec       该池的几何/补齐策略
 * @param spare_used spare 占用标记（跨池共享；函数会置位已用项）
 * @return 实际收集到的候选块数（ring->block_count）
 */
static WORD md_pool_collect(md_ring_t *ring, const alc_t *alloc, const md_pool_spec_t *spec, bool spare_used[MD_SPARE_BLOCKS_PER_PLANE])
{
    ring->first_block      = spec->first;
    ring->nominal_count    = spec->nominal;
    ring->block_count      = 0U;
    ring->pages_per_record = spec->pages_per_record;
    ring->plane_mask       = spec->plane_mask;

    const WORD end = (WORD)(spec->first + spec->nominal);

    for (WORD b = spec->first; b < end; b++)
    {
        if (ring->block_count >= MD_RING_MAX_BLOCKS)
        {
            break; /* candidates 数组容量 */
        }
        if (md_block_good_on_mask(alloc, b, spec->plane_mask))
        {
            ring->blocks[ring->block_count++] = b;
        }
    }

    // 停止点 = min(硬下界, candidates 数组容量)：两者任一到达后继续扫也不会再收。
    const WORD spare_end = (WORD)FTL_METADATA_AREA_END;
    const WORD target    = (spec->min_blocks < MD_RING_MAX_BLOCKS) ? spec->min_blocks : MD_RING_MAX_BLOCKS;
    WORD       used      = 0U;

    for (WORD s = MD_SPARE_BLOCK0; s < spare_end; s++)
    {
        if ((used >= spec->spare_budget) || (ring->block_count >= target))
        {
            break;
        }

        const WORD si = (WORD)(s - MD_SPARE_BLOCK0);

        if (spare_used[si] || (md_block_good_on_mask(alloc, s, spec->plane_mask) == false))
        {
            continue;
        }
        ring->blocks[ring->block_count++] = s;
        spare_used[si]                    = true;
        used++;
    }

    return ring->block_count;
}

// BBT 未定位时的兜底池：名义 BBT 块 + 全部 shared spare（record 自带 magic/sequence，坏块靠探针忽略）。
void md_pool_build_bbt_bootstrap(md_ring_t *ring)
{
    // 未知 BAD 集：名义 BBT 块 + 全部 shared spare。BBT record 自带 magic/sequence，
    // 坏块/空块读失败由 md_ring_locate 的探针自然忽略。
    ring->first_block      = BBT_BLOCK;
    ring->nominal_count    = BBT_POOL_BLOCKS;
    ring->block_count      = 0U;
    ring->pages_per_record = 1U;
    ring->plane_mask       = METADATA_MIRROR_PLANE_MASK;

    for (WORD b = BBT_BLOCK; b < (BBT_BLOCK + BBT_POOL_BLOCKS); b++)
    {
        ring->blocks[ring->block_count++] = b;
    }
    for (WORD s = MD_SPARE_BLOCK0; s < FTL_METADATA_AREA_END; s++)
    {
        ring->blocks[ring->block_count++] = s;
    }
}

// 重算全部元数据池候选（Anchor/CP/BBT/journal/L2P 日志）；绑定缺失返回 false，调用者停止重试。
bool md_pool_build_all(alc_t *alloc)
{
    if ((alloc == NULL) || (alloc->pools == NULL))
    {
        return false; /* 绑定缺失是配置错误：release 也必须拒绝 */
    }

    md_pools_t *p = alloc->pools;

    // 先在局部构建，全部成功才整体替换：失败时保留上一份可用候选池，
    // 避免「BBT/CP 已换、Journal 失败」的半更新状态被在飞模块读到。
    md_pools_t next;

    (void)memset(&next, 0, sizeof(next));

    // 分配顺序固定：先紧后松。BBT/Journal 的名义池最小，优先拿 spare。
    bool spare_used[MD_SPARE_BLOCKS_PER_PLANE];

    (void)memset(spare_used, 0, sizeof(spare_used));

    if (md_pool_collect(&next.bbt, alloc, &s_pool_specs[MD_POOL_BBT], spare_used) < s_pool_specs[MD_POOL_BBT].min_blocks)
    {
        LOGE(LOG_TAG_POOL "BBT candidates < %hu — metadata area unusable\r\n", s_pool_specs[MD_POOL_BBT].min_blocks);
        return false;
    }

    if (md_pool_collect(&next.cp, alloc, &s_pool_specs[MD_POOL_CP], spare_used) < s_pool_specs[MD_POOL_CP].min_blocks)
    {
        LOGE(LOG_TAG_POOL "Checkpoint candidates < %hu\r\n", s_pool_specs[MD_POOL_CP].min_blocks);
        return false;
    }

    if (md_pool_collect(&next.jrn, alloc, &s_pool_specs[MD_POOL_JRN], spare_used) < s_pool_specs[MD_POOL_JRN].min_blocks)
    {
        LOGE(LOG_TAG_POOL "Journal candidates < %hu\r\n", s_pool_specs[MD_POOL_JRN].min_blocks);
        return false;
    }

    if (md_pool_collect(&next.anchor, alloc, &s_pool_specs[MD_POOL_ANCHOR], spare_used) < s_pool_specs[MD_POOL_ANCHOR].min_blocks)
    {
        LOGE(LOG_TAG_POOL "Anchor candidates < %hu\r\n", s_pool_specs[MD_POOL_ANCHOR].min_blocks);
        return false;
    }

    next.dirty = false;
    *p         = next;

    // 候选表可能变了 ⇒ BBT 的 record 下标到物理块的映射也可能变。
    // 下一次 bbt_save 必须在新池上重新定位（不能沿用旧 write_idx）。
    alloc->bbt.cursor_valid = false;

    return true;
}

// 池内 (record, page) → PBA：固定取 plane_mask 的第一个 plane（镜像页的规范副本）。
DWORD md_ring_addr(const md_ring_t *ring, const ftl_topo_t *topo, WORD record_idx, BYTE page_in_record)
{
    const BYTE plane = md_ring_plane_at(ring->plane_mask, 0U);

    return md_ring_addr_plane(ring, topo, record_idx, page_in_record, plane);
}

/**
 * @brief ①~③：逐块探末条 → 找游标块 → 游标块内找末条。
 *
 * @param ring  池几何
 * @param probe 调用方探针
 * @param ctx   探针上下文
 * @param rpb   每块 record 数
 * @param s     输出：扫描中间结果
 */
static void md_ring_scan(const md_ring_t *ring, metadata_ring_probe_fn probe, void *ctx, WORD rpb, md_ring_scan_t *s)
{
    s->cursor_block    = UINT16_MAX;
    s->best_full_idx   = UINT16_MAX;
    s->best_full_seq   = 0U;
    s->cursor_last_idx = UINT16_MAX;
    s->cursor_last_seq = 0U;
    s->ring_full       = false;

    // ① 逐块廉价探针该块**末条** record：末条有效 = 该块写满
    for (WORD b = 0U; b < ring->block_count; b++)
    {
        DWORD seq = 0U;
        WORD  idx = ((b * rpb) + (rpb - 1U));

        if (probe(ctx, idx, false, &seq) == FTL_OK)
        {
            if ((s->best_full_idx == UINT16_MAX) || (seq > s->best_full_seq))
            {
                s->best_full_idx = idx;
                s->best_full_seq = seq;
            }
        }
        else if (s->cursor_block == UINT16_MAX)
        {
            s->cursor_block = b; /* 第一个未写满的块 = 游标块 */
        }
    }

    // ② 所有块都写满 ⇒ 池转满一圈：游标 = 最新块之后，且该块整体擦掉重写
    if (s->cursor_block == UINT16_MAX)
    {
        s->cursor_block = (((s->best_full_idx / rpb) + 1U) % ring->block_count);
        s->ring_full    = true;
    }

    // ③ 游标块内降序找最后一条有效 record（池转满时游标块本身就是满的，不参与）
    if (s->ring_full == false)
    {
        for (WORD b = 0U; b < rpb; b++)
        {
            WORD  idx = ((s->cursor_block * rpb) + (rpb - 1U - b));
            DWORD seq = 0U;

            if (probe(ctx, idx, false, &seq) == FTL_OK)
            {
                s->cursor_last_idx = idx;
                s->cursor_last_seq = seq;
                break;
            }
        }
    }
}

/**
 * @brief ⑥ 慢路径：快路径候选整条校验全失败（局部损坏）⇒ 全池廉价探针取 sequence 最大者，
 * 整条校验；失败则排除该 sequence 继续找次新。仅在损坏时付出全池代价。
 *
 * @param probe 调用方探针
 * @param ctx   探针上下文
 * @param total 池内槽位总数
 * @param out   输出：最新有效 record 槽位与 sequence
 * @return true = 找到有效 record；false = 池内已无更多候选
 */
static bool md_ring_locate_fallback(metadata_ring_probe_fn probe, void *ctx, WORD total, md_ring_locate_out_t *out)
{
    DWORD limit = UINT32_MAX;

    for (;;)
    {
        WORD  ci = UINT16_MAX;
        DWORD cs = 0U;

        for (WORD k = 0U; k < total; k++)
        {
            DWORD seq = 0U;

            if (probe(ctx, k, false, &seq) != FTL_OK)
            {
                continue;
            }
            if (seq >= limit)
            {
                continue; /* 已排除（含同 sequence 的重复份）*/
            }
            if ((ci == UINT16_MAX) || (seq > cs))
            {
                ci = k;
                cs = seq;
            }
        }

        if ((ci == UINT16_MAX) || (cs == 0U))
        {
            return false; /* 池内已无更多候选 */
        }

        // 整条校验用独立出参：探针契约是「失败时不写 *out_sequence」，
        // 不能把 cs（下一步的排除界）押在这个隐含约定上。
        DWORD full_seq = 0U;

        if (probe(ctx, ci, true, &full_seq) == FTL_OK)
        {
            out->newest_idx = ci;
            out->sequence   = full_seq;
            return true;
        }
        limit = cs;
    }
}

// 在池内定位最新 record：出参先给确定值；几何非法返回 FTL_ERROR（配置错误，release 也拒绝）。
ftl_status_e md_ring_locate(const md_ring_t *ring, metadata_ring_probe_fn probe, void *ctx, md_ring_locate_out_t *out)
{
    // 出参先给确定值：几何非法时也能安全读；池空时 newest=UINT16_MAX、seq=0
    // （调用者据此续序号）。
    out->newest_idx = UINT16_MAX;
    out->sequence   = 0U;
    out->write_idx  = 0U;

    // ring 自身几何是**配置**（构造错误 ⇒ 寻址全错）⇒ 保留运行期拒绝（判据 9）。
    if (md_ring_geom_valid(ring) == false)
    {
        return FTL_ERROR;
    }

    WORD rpb   = md_ring_records_per_block(ring);
    WORD total = md_ring_total_records(ring);

    md_ring_scan_t scan;

    md_ring_scan(ring, probe, ctx, rpb, &scan);

    // ④ 写点 = 游标块末条的下一条（末条是块内最后一条时跨到下一块槽 0）；
    // 游标块内无有效 record ⇒ 写点 = 游标块的槽 0
    if (scan.cursor_last_idx == UINT16_MAX)
    {
        out->write_idx = (scan.cursor_block * rpb);
    }
    else if ((scan.cursor_last_idx % rpb) == (rpb - 1U))
    {
        out->write_idx = ((scan.cursor_last_idx + 1U) % total);
    }
    else
    {
        out->write_idx = (scan.cursor_last_idx + 1U);
    }

    // ⑤ 快路径候选：全部块末条中 sequence 最大者 + 游标块末条（按 seq 降序整条校验）
    WORD  cand_idx[2];
    DWORD cand_seq[2];
    BYTE  cand_n = 0U;

    if (scan.best_full_idx != UINT16_MAX)
    {
        cand_idx[cand_n] = scan.best_full_idx;
        cand_seq[cand_n] = scan.best_full_seq;
        cand_n++;
    }
    if ((scan.cursor_last_idx != UINT16_MAX) && (scan.cursor_last_idx != scan.best_full_idx))
    {
        cand_idx[cand_n] = scan.cursor_last_idx;
        cand_seq[cand_n] = scan.cursor_last_seq;
        cand_n++;
    }
    if ((cand_n == 2U) && (cand_seq[1] > cand_seq[0]))
    {
        WORD  ti = cand_idx[0];
        DWORD ts = cand_seq[0];

        cand_idx[0] = cand_idx[1];
        cand_seq[0] = cand_seq[1];
        cand_idx[1] = ti;
        cand_seq[1] = ts;
    }

    for (BYTE i = 0U; i < cand_n; i++)
    {
        DWORD seq = 0U;

        if (probe(ctx, cand_idx[i], true, &seq) == FTL_OK)
        {
            out->newest_idx = cand_idx[i];
            out->sequence   = seq;
            return FTL_OK;
        }
    }

    // ⑥ 局部损坏时的全池回退。找不到也返回 FTL_OK（池内无有效 record）。
    (void)md_ring_locate_fallback(probe, ctx, total, out);

    return FTL_OK;
}

// 实现要点：仅当 write_idx 落在块首（% rpb == 0）才需擦块。池内轮转 ⇒ 该块持全池
// 最旧 record，且每条 record 是完整快照 ⇒ 擦掉不丢信息；块内槽 0 恒走本
// 分支（新取/刚擦的块再擦一次是幂等无害代价）。
// **不动介质**：擦除由调用者做（同步站点即刻擦；异步 job 提交异步擦）。
ftl_status_e md_ring_prepare_addr(const md_ring_t *ring, const ftl_topo_t *topo, WORD write_idx, DWORD *out_addr, bool *out_need_erase)
{
    if (md_ring_geom_valid(ring) == false)
    {
        return FTL_ERROR;
    }

    const WORD total = md_ring_total_records(ring);
    const WORD rpb   = md_ring_records_per_block(ring);

    if (write_idx >= total)
    {
        return FTL_ERROR;
    }

    *out_addr       = md_ring_addr(ring, topo, write_idx, 0U);
    *out_need_erase = ((write_idx % rpb) == 0U);
    if (*out_addr == NAND_PBA_INVALID)
    {
        return FTL_ERROR;
    }

    return FTL_OK;
}

// 实现要点：= md_ring_prepare_addr + 同步擦除。**同步原语，仅开机/格式化用**
// （运行期 job 走 md_ring_prepare_addr + 异步擦除）。
// 镜像池（plane_mask 含多个 plane）用 multi-plane 擦，所有副本一起擦。
ftl_status_e md_ring_prepare_write(const md_ring_t *ring, const ftl_topo_t *topo, WORD write_idx, DWORD *out_addr)
{
    DWORD addr;
    bool  need_erase = false;

    if (md_ring_prepare_addr(ring, topo, write_idx, &addr, &need_erase) != FTL_OK)
    {
        return FTL_ERROR;
    }

    if (need_erase)
    {
        // 换块：先擦目标块（池内轮转 ⇒ 该块持全池最旧 record，擦掉不丢信息）
        const int st = (md_ring_plane_count(ring->plane_mask) > 1U) ? ftl_sync_erase_block_multi_plane(addr) : ftl_sync_erase_block(addr);
        if (st != 0)
        {
            return FTL_ERROR;
        }
    }

    *out_addr = addr;
    return FTL_OK;
}

/* ══════════════════════════════════════════════════════════════════
 * 二、Anchor 池（Metadata Directory 的 record 池）
 * ══════════════════════════════════════════════════════════════════ */

/**
 * @brief 页头字段校验：magic + chunk_count + 布局版本。
 *
 * @param magic          页头 magic
 * @param chunk_count    页头 chunk 总数
 * @param layout_version 页头介质布局版本
 * @return true = 三个字段均与当前格式一致；false = 任一不符
 */
static bool md_anchor_header_valid(DWORD magic, WORD chunk_count, DWORD layout_version)
{
    return (magic == ANCHOR_MAGIC) && (chunk_count == ANCHOR_PAGE_COUNT) && (layout_version == FTL_MEDIA_LAYOUT_VERSION);
}

/**
 * @brief Anchor 池几何：优先用 md_pool_build_all() 生成的候选池（跳过 BAD）；
 * 池尚未建立时退回名义区间（开机早期/测试），BAD 由后续 rebuild 修正。
 *
 * @param dir  Directory cache（取 alloc->pools）
 * @param ring 输出：Anchor 池几何
 */
static void md_anchor_ring_init(const md_dir_cache_t *dir, md_ring_t *ring)
{
    if ((dir->alloc != NULL) && (dir->alloc->pools != NULL))
    {
        *ring = dir->alloc->pools->anchor;
        return;
    }

    ring->first_block      = ANCHOR_BLOCK0;
    ring->nominal_count    = ANCHOR_POOL_BLOCKS;
    ring->block_count      = ANCHOR_POOL_BLOCKS;
    ring->pages_per_record = ANCHOR_PAGE_COUNT;
    ring->plane_mask       = METADATA_MIRROR_PLANE_MASK;
    for (WORD i = 0U; i < ANCHOR_POOL_BLOCKS; i++)
    {
        ring->blocks[i] = (WORD)(ANCHOR_BLOCK0 + i);
    }
}

/**
 * @brief 廉价探针：只读 chunk 0 的 20 字节页头取 sequence。
 * FC 读整页并校验 per-CW CRC + ECC，`length` 只限制拷出字节数。
 *
 * @param c            探针上下文（Directory + 池几何）
 * @param record_idx   槽位号
 * @param out_sequence 输出：该 record 的 sequence（成功时写）
 * @return FTL_OK = 某个 plane 上读到有效页头；FTL_ERROR = 所有 plane 都失败
 */
static ftl_status_e md_anchor_record_probe_cheap(const anchor_probe_ctx_t *c, WORD record_idx, DWORD *out_sequence)
{
    const BYTE plane_count = md_ring_plane_count(c->ring->plane_mask);

    for (BYTE pi = 0U; pi < plane_count; pi++)
    {
        const BYTE         plane = md_ring_plane_at(c->ring->plane_mask, pi);
        md_anchor_header_t hdr;
        DWORD              addr = md_ring_addr_plane(c->ring, &c->dir->topo, record_idx, 0U, plane);

        if (ftl_sync_read_page(addr, &hdr, sizeof(hdr), FTL_PAGE_CFG_META()) != 0)
        {
            continue;
        }
        if ((md_anchor_header_valid(hdr.magic, hdr.chunk_count, hdr.layout_version) == false) || (hdr.chunk_index != 0U) || (hdr.sequence == 0U))
        {
            continue;
        }

        *out_sequence = hdr.sequence;
        return FTL_OK;
    }

    return FTL_ERROR;
}

/**
 * @brief 整条校验：全部页有效、chunk 序完整、各页 sequence 一致；不跨 plane 拼半条。
 *
 * @param c            探针上下文（Directory + 池几何）
 * @param record_idx   槽位号
 * @param out_sequence 输出：整条校验通过时的 sequence
 * @return FTL_OK = 某个 plane 上整条有效；FTL_ERROR = 无 plane 通过
 */
static ftl_status_e md_anchor_record_probe_full(const anchor_probe_ctx_t *c, WORD record_idx, DWORD *out_sequence)
{
    md_anchor_page_t page;
    const BYTE       plane_count = md_ring_plane_count(c->ring->plane_mask);

    for (BYTE pi = 0U; pi < plane_count; pi++)
    {
        const BYTE plane      = md_ring_plane_at(c->ring->plane_mask, pi);
        DWORD      seq        = 0U;
        DWORD      dgen       = 0U; /* 本 record 主半的目录代数（RISK-MD-06：必须全条一致）*/
        bool       primary_ok = true;

        for (BYTE chunk = 0U; chunk < ANCHOR_PAGE_COUNT; chunk++)
        {
            const bool backup = (chunk >= ANCHOR_DIR_PRIMARY_PAGES);
            DWORD      addr   = md_ring_addr_plane(c->ring, &c->dir->topo, record_idx, chunk, plane);

            // 备份半是 best-effort：单页坏/旧代只让对应 region 暂无双副本，
            // 不能让整条最新 Directory 作废（否则一次备份页失败就回退到旧快照）。
            if (ftl_sync_read_page(addr, &page, sizeof(page), FTL_PAGE_CFG_META()) != 0)
            {
                if (backup)
                {
                    continue;
                }
                primary_ok = false;
                break;
            }
            const bool header_ok = md_anchor_header_valid(page.magic, page.chunk_count, page.layout_version);

            if ((header_ok == false) || (page.chunk_index != chunk))
            {
                if (backup)
                {
                    continue;
                }
                primary_ok = false;
                break;
            }

            if (chunk == 0U)
            {
                seq = page.sequence;
                if (seq == 0U)
                {
                    primary_ok = false; /* sequence 从 1 起；0 = 未写 */
                    break;
                }
            }
            else if (page.sequence != seq)
            {
                if (backup)
                {
                    continue;
                }                   /* 旧代备份页：忽略 */
                primary_ok = false; /* 主半必须同 sequence */
                break;
            }

            if (backup == false)
            {
                // 整条记录的页间一致性（RISK-MD-06）：同一 sequence 下，各页还必须是**同一
                // 目录代数**。写入期间目录更新过 ⇒ 早写的页是旧代、晚写的是新代，这种
                // "混代记录"在媒体上看起来完全合法；被当作最新目录加载时，不同代次的
                // 条目会在复用过的页上互相碰撞（两个 region 指向同一页）。这里整条作废，
                // 回退到上一条一致记录；丢失的增量由 Journal 回放补回。
                if (chunk == 0U)
                {
                    dgen = page.dirty_gen;
                }
                else if (page.dirty_gen != dgen)
                {
                    primary_ok = false;
                    break;
                }
            }
        }

        if (primary_ok)
        {
            *out_sequence = seq;
            return FTL_OK;
        }
    }

    return FTL_ERROR;
}

/**
 * @brief 探针入口：按 full 分派到廉价/整条两档。
 *
 * @param ctx          探针上下文（anchor_probe_ctx_t）
 * @param record_idx   槽位号
 * @param full         false = 廉价探针，true = 整条校验
 * @param out_sequence 输出：该 record 的 sequence
 * @return 探针函数的返回值（见 metadata_ring_probe_fn）
 */
static ftl_status_e md_anchor_record_probe(void *ctx, WORD record_idx, bool full, DWORD *out_sequence)
{
    const anchor_probe_ctx_t *c = (const anchor_probe_ctx_t *)ctx;

    return full ? md_anchor_record_probe_full(c, record_idx, out_sequence) : md_anchor_record_probe_cheap(c, record_idx, out_sequence);
}

/* ══════════════════════════════════════════════════════════════════
 * 三、Directory 基础层 / 查询 / 更新
 * ══════════════════════════════════════════════════════════════════ */

/**
 * @brief 为全部 region 登记"未映射"（见 metadata.h「固定元数据区布局」）
 *
 * 每个实例的 L2P_REGIONS_PER_INSTANCE 个 region **全部登记为 NAND_PBA_INVALID**；
 * 某个 region 刷新到日志块后由 md_commit_region 就地覆盖成真实 PBA。
 * "全部预登记"同时保证 Directory 容量（= region 数）按构造不会溢出 ——
 * md_entry_update 的"满则丢弃"分支对 L2P region 不可达。
 *
 * 共用于 md_init（首次格式化）与 md_load（基础层）：
 * Anchor 损坏时该层即"全部未映射"，不需要任何闪存读。
 *
 * @param dir Directory cache（输出：全部 region 预登记为未映射）
 */
static void md_fixed_fill(md_dir_cache_t *dir)
{
    dir->count = 0U;

    for (WORD i = 0U; i < L2P_REGIONS_PER_INSTANCE; i++)
    {
        md_entry_t *entry = &dir->entries[dir->count];

        entry->type        = METADATA_TYPE_L2P_REGION;
        entry->id          = i;
        entry->pba         = NAND_PBA_INVALID;
        dir->backup_pba[i] = NAND_PBA_INVALID;
        dir->count++;
    }
}

/**
 * @brief 把一个 region 副本的 PBA 计入 l2p_live（主/备共用同一套校验）。
 * `*warned` 保证整轮只警告一次；越界/固定非日志区一律跳过。
 *
 * @param dir       Directory cache（输出：l2p_live 计数）
 * @param pba       某个 region 副本的 PBA
 * @param entry_idx 该副本所属的 Directory 条目下标（仅用于告警定位）
 * @param warned    输入/输出：本轮是否已告警（防止刷屏）
 */
static void md_live_copy_add(md_dir_cache_t *dir, DWORD pba, WORD entry_idx, bool *warned)
{
    if (pba == NAND_PBA_INVALID)
    {
        return;
    }

    const BYTE plane = pba_to_plane(pba);
    const WORD block = pba_to_block(pba);

    if ((plane >= PLANE_NUM) || (block >= BLOCKS_PER_PLANE))
    {
        if (*warned == false)
        {
            LOGW(LOG_TAG_META "directory entry %hu: plane=%hhu block=%hu out of range — skipped\r\n", entry_idx, plane, block);
            *warned = true;
        }
        return;
    }

    // 只排除**非日志的**固定元数据区（0..L2P_LOG_BLOCK0−1）。
    if (block < L2P_LOG_BLOCK0)
    {
        return;
    }

    const WORD g = ftl_gblock(plane, block);

    if (g < FTL_BLOCK_TOTAL)
    {
        dir->l2p_live[g]++;
    }
    else if (*warned == false)
    {
        LOGW(LOG_TAG_META "directory entry %hu: gblock %hu out of range — skipped\r\n", entry_idx, g);
        *warned = true;
    }
}

/**
 * @brief 由 entries 重建 l2p_live（日志块 = 非固定元数据块）。
 *
 * @param dir Directory cache（输出：l2p_live 与 log_borrow_count）
 */
static void md_live_rebuild(md_dir_cache_t *dir)
{
    memset(dir->l2p_live, 0, sizeof(dir->l2p_live));

    bool warned = false;

    for (WORD i = 0U; i < dir->count; i++)
    {
        if (dir->entries[i].type != METADATA_TYPE_L2P_REGION)
        {
            continue;
        }

        // 主副本 + 备副本都计入 live：回收判据是“所有占用者都走了”。
        // ⚠ INVALID/越界/固定非日志区的校验在 helper 里统一（介质输入不可信，H29）。
        const WORD  rid         = dir->entries[i].id;
        const DWORD primary_pba = dir->entries[i].pba;

        md_live_copy_add(dir, primary_pba, i, &warned);
        if (rid < METADATA_DIR_ENTRIES)
        {
            const DWORD backup_pba = dir->backup_pba[rid];

            md_live_copy_add(dir, backup_pba, i, &warned);
        }
    }

    // 借用计数按**块**计，不按 entry 计（一个借来的日志块可承载多个 region）。
    dir->log_borrow_count = 0U;

    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        if (dir->l2p_live[g] == 0U)
        {
            continue;
        }

        const BYTE plane = ftl_gblock_plane(g);
        const WORD block = ftl_gblock_block(g);

        if (md_block_is_metadata(NULL, plane, block) == false)
        {
            dir->log_borrow_count++;
        }
    }
}

/**
 * @brief 开机观测口（判据②「走动态分配」/ 判据③「Anchor 真的落盘」的唯一观测口）
 *
 * @param dir Directory cache（只读，打一条开机观测行）
 */
static void md_boot_status_log(const md_dir_cache_t *dir)
{
    DWORD live_blocks = 0U;

    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        if (dir->l2p_live[g] != 0U)
        {
            live_blocks++;
        }
    }

    const unsigned dyn = (live_blocks != 0U) ? 1U : 0U;

    // 当前写块按 plane 打印（每 plane 一个，RISK-BOOT-18 L2）。
    LOGI(LOG_TAG_META "anchor_seq=%lu dyn=%u cur_blk=%hu/%hu live_blks=%lu count=%hu\r\n",
         dir->anchor_sequence,
         dyn,
         (dir->cur_valid[0]) ? dir->cur_block[0] : (WORD)UINT16_MAX,
         (PLANE_NUM > 1U && dir->cur_valid[1]) ? dir->cur_block[1] : (WORD)UINT16_MAX,
         live_blocks,
         dir->count);
}

// 实现要点：游标置无效是"绝不复用可能已编程的旧页"的保证——首次追加才从 Free
// Pool 新取块。
void md_init(md_dir_cache_t *dir, alc_t *alloc, const DWORD *total_writes)
{
    // 前置条件：唯一调用点 ftl_init 传 &ftl->alloc / &ftl->total_writes（调用链已保证）。
    dir->alloc        = alloc;
    dir->total_writes = total_writes;

    s_pool_rebuilt_logged = 0U; /* DTCM NOLOAD：文件级静态必须显式复位 */

    dir->anchor_sequence = 0U;
    dir->anchor_busy     = false;
    dir->anchor_owner    = MD_ANCHOR_OWNER_NONE;
    dir->dirty           = false;
    dir->dirty_gen       = 0U;
    dir->anchor_gen      = 0U;

    md_slot_reset(dir);

    md_fixed_fill(dir);
    md_live_rebuild(dir);

    // 游标全部置无效：首次追加才从固定 L2P 日志区取块（绝不复用可能已编程的旧页）
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        dir->cur_block[p] = 0U;
        dir->cur_page[p]  = 0U;
        dir->cur_valid[p] = false;
    }
    dir->anchor_write_idx    = 0U;
    dir->anchor_cursor_valid = false;

    // 待回收位图 / 后台整理 job 归零（RAM 态，不跨重启；
    // 不清 job ⇒ append_slot 永久让路）。
    md_reclaim_reset(dir);
    md_handle_reset(dir);
}

/**
 * @brief 查 (type,id) 的下标；未找到返回 dir->count（调用者用它判 < count）。
 *
 * ⚠ 必须先走**稠密快路径**：Directory 的 L2P_REGION 条目由 `md_fixed_fill` 按
 * `entries[id].id == id` 稠密预登记（Anchor 也按同一顺序读写），查找是 O(1)。
 * 只做线性扫描时，`md_compact_read` 对每个 region_id 各查一次 ⇒ O(N²)
 * （N = 2048，约 400 万次比较 ≈ 100 ms），元数据压缩期间主循环被拖住、USB
 * 事件得不到服务（表现为设备消失）。慢路径保留给非稠密/历史格式。
 *
 * @param dir  Directory cache
 * @param type 元数据类型
 * @param id   元数据 id
 * @return 匹配条目的下标；未找到返回 dir->count
 */
static WORD md_entry_index_find(const md_dir_cache_t *dir, BYTE type, WORD id)
{
    if ((id < dir->count) && (dir->entries[id].type == type) && (dir->entries[id].id == id))
    {
        return id;
    }

    for (WORD i = 0U; i < dir->count; i++)
    {
        if ((dir->entries[i].type == type) && (dir->entries[i].id == id))
        {
            return i;
        }
    }

    return dir->count;
}

// md_entry_find — 线性查 (type,id) 的登记位置；未找到返回 NAND_PBA_INVALID。
// Directory 当前只登记 L2P_REGION；其余元数据是固定位置，不经过本函数。
DWORD md_entry_find(const md_dir_cache_t *dir, metadata_type_e type, WORD id)
{
    const WORD i = md_entry_index_find(dir, type, id);

    return (i < dir->count) ? dir->entries[i].pba : NAND_PBA_INVALID;
}

// 块被多少 L2P region 引用（活计数）；越界块号按「无引用」处理（来源可能是介质/快照）。
WORD md_live_count_get(const md_dir_cache_t *dir, WORD gblock)
{
    if (gblock >= FTL_BLOCK_TOTAL)
    {
        return 0U; /* 来源可能是介质/快照（H29）：越界按“无引用”处理 */
    }
    return dir->l2p_live[gblock];
}

// 实现要点：容量满时**不追加**并 LOGE。对本项目的 L2P_REGION 不可达
// （容量 == region 数，全部在 fill_fixed 已登记）。
void md_entry_update(md_dir_cache_t *dir, metadata_type_e type, WORD id, DWORD new_pba)
{
    // 查找现有 entry（覆盖旧位置）
    const WORD i = md_entry_index_find(dir, type, id);

    if (i < dir->count)
    {
        dir->entries[i].pba = new_pba;
        dir->dirty          = true;
        dir->dirty_gen++;
        return;
    }

    // 新增 entry（往 entries 数组尾部追加）
    if (dir->count < METADATA_DIR_ENTRIES)
    {
        md_entry_t *entry = &dir->entries[dir->count];

        entry->type = type;
        entry->id   = id;
        entry->pba  = new_pba;
        dir->count++;
        dir->dirty = true;
        dir->dirty_gen++;
        return;
    }

    // 满：不追加。L2P_REGION 按构造不可达；新增元数据类型前必须先扩容量。
    // 静默丢弃会让调用者以为位置已记录 ⇒ 至少留下错误日志。
    LOGE(LOG_TAG_META "directory full: type=%hhu id=%hu dropped\r\n", type, id);
}

/* ══════════════════════════════════════════════════════════════════
 * 四、L2P 日志块：追加槽 / 登记
 *（回收 / 压缩在 metadata_job.c；本区只维护写指针与 Directory 登记）
 * ══════════════════════════════════════════════════════════════════ */

/* ── 固定 L2P 日志区：分配 / 归还 / 开机恢复（RISK-MD-07 / RISK-CAP-02）── */

// 在固定 L2P 日志区找一块「已擦净、空闲」的块，标 LOG 后返回；没有返回 FTL_GBLOCK_INVALID。
WORD md_log_alloc(md_dir_cache_t *dir, alc_t *alloc, BYTE plane)
{
    ASSERT(plane < PLANE_NUM);

    for (WORD block = L2P_LOG_BLOCK0; block < L2P_LOG_AREA_END; block++)
    {
        const WORD   g = ftl_gblock(plane, block);
        bst_block_t *b = &alloc->blocks[g];

        if (ftl_block_is_bad(b))
        {
            continue;
        }
        // 只有“已擦净、空闲”的固定日志块可分配（NONE 是本区的 free 标记）。
        if (ftl_block_state(b) != FTL_BLOCK_STATE_NONE)
        {
            continue;
        }
        if (dir->cur_valid[plane] && (g == dir->cur_block[plane]))
        {
            continue;
        }

        ftl_block_set_state(b, FTL_BLOCK_STATE_LOG);
        ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
        return g;
    }

    // 固定区无空闲块：从 **OP 池**有界借用动态日志块（L2P_LOG_BORROW_MAX）。
    // 不借则压缩没有目标块、CP 永远拿不到槽（RISK-MD-08）。借出的块由 l2p_live /
    // md_block_is_metadata 动态识别，擦空后由 md_log_release 回 OP 池。
    if (dir->log_borrow_count < L2P_LOG_BORROW_MAX)
    {
        const WORD borrowed = alc_op_alloc_log_block(alloc, plane);

        if (borrowed != FTL_GBLOCK_INVALID)
        {
            dir->log_borrow_count++;
            return borrowed;
        }
    }

    return FTL_GBLOCK_INVALID;
}

// 归还日志块：固定区内留 NONE，借来的回 OP 池；BAD 是终态，不回收。
void md_log_release(md_dir_cache_t *dir, WORD gblock)
{
    ASSERT(gblock < FTL_BLOCK_TOTAL);

    alc_t *alloc = dir->alloc;

    bst_block_t *b = &alloc->blocks[gblock];

    // 固定日志区内的块 ⇒ 留在固定区（NONE）；借来的块 ⇒ 回 OP 池。
    const bool borrowed = (ftl_is_l2p_log_block(ftl_gblock_block(gblock)) == false);

    if (borrowed && (dir->log_borrow_count > 0U))
    {
        dir->log_borrow_count--;
    }

    // BAD 是终态：擦除失败后不得“释放”回可用池。
    if (ftl_block_is_bad(b))
    {
        return;
    }

    if (borrowed)
    {
        // 借来的动态日志块回 OP 池（与 alc_op_alloc_log_block 成对）；OP 满则回 Free Pool。
        ftl_block_set_state(b, FTL_BLOCK_STATE_NONE);
        ftl_block_clear_valid(b);
        ftl_block_set_sb(b, FTL_BLOCK_NO_SB);

        const BYTE plane = ftl_gblock_plane(gblock);

        if (alc_op_push(alloc, plane, gblock) == false)
        {
            alc_free_push(alloc, gblock);
        }
        return;
    }

    ftl_block_set_state(b, FTL_BLOCK_STATE_NONE);
    ftl_block_clear_valid(b);
    ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
}

// 固定 L2P 日志区的**空闲块数**（state == NONE 且非 BAD）。
// 压缩触发的主判据：按 free 水位压，而不是等 live 涨到接近区上限——
// live 接近上限时压缩已经没有目标块，只能死锁（RISK-MD-08）。
WORD md_log_area_free_count(const alc_t *alloc)
{
    WORD n = 0U;

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        for (WORD block = L2P_LOG_BLOCK0; block < L2P_LOG_AREA_END; block++)
        {
            const WORD         g = ftl_gblock(plane, block);
            const bst_block_t *b = &alloc->blocks[g];

            if (ftl_block_is_bad(b))
            {
                continue;
            }
            if (ftl_block_state(b) == FTL_BLOCK_STATE_NONE)
            {
                n++;
            }
        }
    }

    return n;
}

// 统计固定 L2P 日志区内活计数非 0 的块数（诊断 / info 用）。
WORD md_live_block_count_get(const md_dir_cache_t *dir)
{
    WORD n = 0U;

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        for (WORD block = L2P_LOG_BLOCK0; block < L2P_LOG_AREA_END; block++)
        {
            if (dir->l2p_live[ftl_gblock(plane, block)] > 0U)
            {
                n++;
            }
        }
    }

    return n;
}

// 开机恢复日志区：按活计数分「存活 / 待回收」，非活块全部登记进 reclaim 位图。
void md_log_area_restore(md_dir_cache_t *dir, alc_t *alloc)
{
    WORD n_live    = 0U;
    WORD n_reclaim = 0U;

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        for (WORD block = L2P_LOG_BLOCK0; block < L2P_LOG_AREA_END; block++)
        {
            const WORD   g = ftl_gblock(plane, block);
            bst_block_t *b = &alloc->blocks[g];

            if (ftl_block_is_bad(b))
            {
                continue;
            }

            const bool cur  = dir->cur_valid[plane] && (g == dir->cur_block[plane]);
            const bool live = (dir->l2p_live[g] > 0U);

            if (live || cur)
            {
                ftl_block_set_state(b, FTL_BLOCK_STATE_LOG);
                ftl_block_clear_valid(b);
                ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
                n_live++;
                continue;
            }

            // 非活块：Directory 不引用它 ⇒ **一律登记回收**，擦完由后台 md_log_release 置 NONE。
            // ⚠ 不读介质判「是否已擦净」：块状态不该由页面内容反推——FTL 只看得见
            // USER 512B，判不出「编程过没有」（RISK-IF-15）。擦除是幂等的，多擦一次
            // 只花 P/E；而判错一次就是在未擦页上编程 = 静默介质损坏。
            // 擦除按空闲水位**懒执行**（metadata_job.c 的 MD_JOB_RECLAIM 门控），
            // 所以开机不会把整个日志区重擦一遍。
            ftl_block_set_state(b, FTL_BLOCK_STATE_LOG);
            ftl_block_clear_valid(b);
            ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
            (void)md_reclaim_mark(dir, g);
            n_reclaim++;
        }
    }

    /* ── 动态借用块对账（用户区、非固定区）──
     * ① live>0 但 state 不是 LOG（CP 快照早于借用）：补置 LOG，防止被池化/重建误用；
     * ② live==0 且 state==LOG（借用块已搬空、掉电前没来得及擦/归还）：登记回收，
     *    擦除后 md_log_release 回 OP；在此之前占用借用配额，避免无限借。
     * 没有这一遍，② 会成为“有借不还”的孤儿块（不在任何池、也无回收登记）。 */
    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        const BYTE   plane = ftl_gblock_plane(g);
        const WORD   block = ftl_gblock_block(g);
        bst_block_t *b     = &alloc->blocks[g];

        if (md_block_is_metadata(NULL, plane, block) || ftl_block_is_bad(b))
        {
            continue;
        }

        if (dir->l2p_live[g] > 0U)
        {
            if (ftl_block_state(b) != FTL_BLOCK_STATE_LOG)
            {
                ftl_block_set_state(b, FTL_BLOCK_STATE_LOG);
                ftl_block_clear_valid(b);
                ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
            }
        }
        else if (ftl_block_state(b) == FTL_BLOCK_STATE_LOG)
        {
            if (md_reclaim_mark(dir, g) && (dir->log_borrow_count < UINT16_MAX))
            {
                dir->log_borrow_count++;
            }
        }
    }

    // 这里不报 free 数：非活块此刻全部是「待擦」而非「空闲」，空闲数由后台擦除按水位补足
    // （metadata_job.c 的 MD_JOB_RECLAIM 门控）。
    LOGI(LOG_TAG_META "L2P log area: live=%hu reclaim=%hu borrow=%hu (total=%u/plane)\r\n", n_live, n_reclaim, dir->log_borrow_count, L2P_LOG_BLOCKS_PER_PLANE);
}

// false = 没换（固定日志区无空闲块，或活块数超水位请求了压缩）。
// 实现要点：
// · 旧当前块已无活 region ⇒ 登记待回收；扫描全池只登记、不擦。
// · 每个 plane 一个当前块：本函数只换**指定 plane** 的块，只从**该 plane 的固定
// L2P 日志区**取块（md_log_alloc），不碰用户 Free Pool。
// · 活日志块数超过 L2P_LOG_COMPACT_TRIGGER 时只登记 `job.compact_wanted` 并返回
// false——新块整块空槽，正好把多个稀疏 victim 一次搬空。
// · 压缩 job 也调用本函数：当前块空槽装不下 victim 时，新块的整块空槽是压缩唯一
// 的 destination；换块原本只由追加触发，host 写一停就会永久缺目标槽。

/**
 * @brief 单个 PBA 的账本迁移：旧位置 -1、新位置 +1、旧块归零则登记回收。
 * 主/备副本共用（RISK-BOOT-18 的 L2 双副本）。
 *
 * @param dir     Directory cache（输出：l2p_live 账本）
 * @param old     旧位置 PBA（NAND_PBA_INVALID = 未映射）
 * @param new_pba 新位置 PBA
 */
static void md_region_copy_commit(md_dir_cache_t *dir, DWORD old, DWORD new_pba)
{
    WORD old_g = UINT16_MAX;

    // 旧位置必须真的可能是 L2P region 槽位：只排除非日志固定区（0..L2P_LOG_BLOCK0−1）。
    // Anchor 条目来自介质（H29），损坏条目可能指向 Anchor/CP/BBT/Journal ⇒ 跳过。
    // ⚠ **不能**用 `md_block_is_metadata(NULL, ...)`：L2P 日志区是固定分区，会被一起
    // 排除 ⇒ 旧位置的活计数永不递减、`l2p_live` 只增不减（RISK-MD-13）。
    if ((old != NAND_PBA_INVALID) && (pba_to_plane(old) < PLANE_NUM) && (pba_to_block(old) >= L2P_LOG_BLOCK0))
    {
        WORD g = ftl_pba_gblock(old);

        if ((g < FTL_BLOCK_TOTAL) && (dir->l2p_live[g] > 0U))
        {
            dir->l2p_live[g]--;
            old_g = g;
        }
    }

    // 新位置的活计数 +1
    {
        WORD g = ftl_pba_gblock(new_pba);

        if (g < FTL_BLOCK_TOTAL)
        {
            dir->l2p_live[g]++;
        }
    }

    // 旧块活计数归零且非当前块 ⇒ 登记进待回收位图，由后台 job 异步擦除。
    // ⚠ 压缩期间**抑制**本登记：搬移 victim 的最后一个 region 时它会归零，此时 Anchor
    // 还没指向新位置 ⇒ 擦了就会掉电丢映射（③④ 契约）。压缩期间归零的块由 job 自己
    // 在 CP_RECLAIM 里擦。
    if ((old_g != UINT16_MAX) && (md_handle_is_compacting(dir) == false) && md_log_block_is_idle(dir, old_g))
    {
        (void)md_reclaim_mark(dir, old_g);
    }
}

// 读某个 region 的备份 PBA（INVALID = 无副本）。
DWORD md_backup_find(const md_dir_cache_t *dir, WORD region_id)
{
    return (region_id < METADATA_DIR_ENTRIES) ? dir->backup_pba[region_id] : NAND_PBA_INVALID;
}

// md_commit_region - 按 mode 提交一个或两个副本。
//
// 单副本模式只走一个 copy；MD_COMMIT_PAIR 走两个 copy。账本迁移原语是
// md_region_copy_commit（旧位置 -1、新位置 +1、旧块归零登记回收）。
// 实现要点：只对**动态日志块**位置（非固定元数据块）增减活计数——INVALID（未映射）
// 不计。旧块归零且非当前块 ⇒ 置进 `reclaim_pending` 位图，由后台 job
// 异步擦除；位图不会像单槽那样被后来的块覆盖。
// ⚠ **本函数不擦 NAND**：它运行在完成派发里（`ftl_l2p_callback` 的 FLUSHING 分支），
// 就地擦块 = 在完成泵里嵌套泵。
void md_commit_region(md_dir_cache_t *dir, WORD region_id, md_commit_mode_e mode, DWORD new_primary, DWORD new_backup)
{
    ASSERT(region_id < METADATA_DIR_ENTRIES);
    ASSERT(mode < MD_COMMIT_MODE_NUM);

    if (region_id >= METADATA_DIR_ENTRIES)
    {
        return;
    }

    BYTE first;
    BYTE count;

    switch (mode)
    {
        case MD_COMMIT_PRIMARY:
            first = 0U;
            count = 1U;
            break;
        case MD_COMMIT_BACKUP:
            first = 1U;
            count = 1U;
            break;
        case MD_COMMIT_PAIR:
            first = 0U;
            count = L2P_REGION_COPIES;
            break;
        default:
            ASSERT(0); /* H40：枚举入口断言；release 下静默 no-op */
            return;
    }

    // 成对提交先整体校验：避免只提交了主副本才发现备份参数无效（半对不是原子提交）。
    if (mode == MD_COMMIT_PAIR)
    {
        ASSERT(new_primary != new_backup);
        ASSERT(new_primary != NAND_PBA_INVALID);
        ASSERT(new_backup != NAND_PBA_INVALID);

        if ((new_primary == new_backup) || (new_primary == NAND_PBA_INVALID) || (new_backup == NAND_PBA_INVALID))
        {
            return;
        }
    }
    else
    {
        const DWORD pba = (mode == MD_COMMIT_PRIMARY) ? new_primary : new_backup;

        ASSERT(pba != NAND_PBA_INVALID);
        if (pba == NAND_PBA_INVALID)
        {
            return;
        }
    }

    const BYTE end = (BYTE)(first + count);

    for (BYTE copy = first; copy < end; copy++)
    {
        const DWORD new_pba = (copy == 0U) ? new_primary : new_backup;

        if (copy == 0U)
        {
            const DWORD old = md_entry_find(dir, METADATA_TYPE_L2P_REGION, region_id);

            md_region_copy_commit(dir, old, new_pba);
            md_entry_update(dir, METADATA_TYPE_L2P_REGION, region_id, new_pba);
        }
        else
        {
            const DWORD old = md_backup_find(dir, region_id);

            md_region_copy_commit(dir, old, new_pba);

            // 备份副本没有额外条目结构，只更新账本并置脏。
            dir->backup_pba[region_id] = new_pba;
            dir->dirty                 = true;
            dir->dirty_gen++;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════
 * 五、Anchor 落盘 / 加载
 * ══════════════════════════════════════════════════════════════════ */

/* ── Anchor 落盘的三段式（唯一形态）──
 *
 * 一条 Anchor record = ANCHOR_PAGE_COUNT 页。**所有**落盘站点（Checkpoint job、
 * 日志块整理 job）都拆成 prepare →（可选换块擦）→ 逐页写 → commit 四步，每步之间
 * 可以回到服务循环。三段式把「Directory 格式」留在本文件，「状态机」留给调用者。
 */

// MD_ANCHOR_FAILED = 失败（定位失败 / 池定位失败 / 槽位越界）。
// 副作用：游标未建立时定位并置 anchor_cursor_valid；**anchor_sequence 自增**
// （序号先取，再取槽）。
// ⚠ 不动介质：换块擦除由调用者做（同步站点即刻擦；异步 job 提交异步擦命令）。
md_anchor_step_e md_anchor_prepare(md_dir_cache_t *dir, md_anchor_prepare_out_t *out, BYTE owner)
{
    if (dir->dirty == false)
    {
        return MD_ANCHOR_CLEAN; /* 无变更早退：Anchor 每写一条都消耗池内槽位，空转会让池无谓轮转 */
    }

    // 单飞（RISK-MD-06）：CP job 与 metadata job 都会写 Anchor，两者共享
    // `anchor_sequence` / `anchor_write_idx`。并发 prepare 会把两个不同 sequence 的
    // 记录写进**同一个槽**（16 页按时间交错）⇒ 整条记录校验失败 ⇒ 开机 locate 只能
    // 回退到更旧的记录 ⇒ 目录陈旧 ⇒ 指向已回收的 L2P 日志块。同一时刻只允许一个
    // writer 处于 prepare→commit 之间。
    if (dir->anchor_busy == true)
    {
        return MD_ANCHOR_BUSY;
    }

    // 记录本次 Anchor 覆盖的变更代数：写入期间若有新的 commit，commit 时不得把
    // dirty 清掉（否则 on-media Anchor 落后于 RAM Directory，旧块可能被误回收）。
    dir->anchor_gen = dir->dirty_gen;

    md_anchor_ring_init(dir, &out->ring);

    // 游标未建立（冷启动后首次 flush）⇒ 先定位池内最新有效 record 的下一槽
    if (dir->anchor_cursor_valid == false)
    {
        anchor_probe_ctx_t   ctx;
        md_ring_locate_out_t loc;

        ctx.dir  = dir;
        ctx.ring = &out->ring;

        if (md_ring_locate(&out->ring, md_anchor_record_probe, &ctx, &loc) != FTL_OK)
        {
            return MD_ANCHOR_FAILED;
        }
        dir->anchor_sequence     = loc.sequence;
        dir->anchor_write_idx    = loc.write_idx;
        dir->anchor_cursor_valid = true;
    }

    dir->anchor_sequence++;
    out->idx = dir->anchor_write_idx;
    out->seq = dir->anchor_sequence;

    if (md_ring_prepare_addr(&out->ring, &dir->topo, out->idx, &out->base, &out->need_erase) != FTL_OK)
    {
        return MD_ANCHOR_FAILED;
    }

    dir->anchor_busy  = true; /* 单飞：commit / 自己 abort 才释放 */
    dir->anchor_owner = owner;

    return MD_ANCHOR_READY;
}

// padding 归零 ⇒ 介质内容确定；本页有效条目数 = 全局有效范围 [0,count) 与本页区间的交。
void md_anchor_page(const md_dir_cache_t *dir, DWORD seq, BYTE chunk, md_anchor_page_t *page)
{
    // chunk 0..PRIMARY-1 承载主 PBA；PRIMARY..COUNT-1 承载 backup_pba[]（同一 id 区间）。
    const bool backup      = (chunk >= ANCHOR_DIR_PRIMARY_PAGES);
    const BYTE local_chunk = backup ? (BYTE)(chunk - ANCHOR_DIR_PRIMARY_PAGES) : chunk;

    const WORD base_entry = (WORD)(local_chunk * METADATA_DIR_ENTRIES_PER_PAGE);
    WORD       n;

    if (base_entry >= dir->count)
    {
        n = 0U;
    }
    else
    {
        n = (dir->count - base_entry);
        if (n > METADATA_DIR_ENTRIES_PER_PAGE)
        {
            n = METADATA_DIR_ENTRIES_PER_PAGE;
        }
    }

    memset(page, 0, sizeof(*page));
    page->magic          = ANCHOR_MAGIC;
    page->sequence       = seq;
    page->chunk_index    = chunk;
    page->chunk_count    = ANCHOR_PAGE_COUNT;
    page->entry_count    = n;
    page->layout_version = FTL_MEDIA_LAYOUT_VERSION;
    // 填充时刻的目录代数（RISK-MD-06）：整条 record 的 16 页必须同代，否则加载侧作废
    // 整条记录。页在写的那一刻才取值 ⇒ 写入期间若有 commit，这一页与先写的页代数不同。
    page->dirty_gen      = dir->dirty_gen;

    // 有效条目：本页区间与全局有效范围 [0,count) 的交。
    for (WORD k = 0U; k < n; k++)
    {
        const WORD entry_idx = (WORD)(base_entry + k);

        page->entries[k] = dir->entries[entry_idx];
        if (backup)
        {
            // 备份半：type/id 沿用主条目，pba 换成本 region 的第二副本位置。
            page->entries[k].pba = dir->backup_pba[entry_idx];
        }
    }

    // 尾部 padding：⚠ memset 只给 0，不能替代这里的 NAND_PBA_INVALID 哨兵——
    // 否则会被当成"指向 block 0"的映射。
    for (WORD k = n; k < METADATA_DIR_ENTRIES_PER_PAGE; k++)
    {
        page->entries[k].type = 0U;
        page->entries[k].id   = 0U;
        page->entries[k].pba  = NAND_PBA_INVALID;
    }
}

// ⚠ **无论成败都推进游标**：已写过的槽（可能半编程）不可复用。ok == false 或
// `dirty_gen` 在写入期间又前进（期间有新的 commit）时保留 dirty ⇒ 下次在新槽重试整条。
void md_anchor_commit(md_dir_cache_t *dir, const md_ring_t *ring, WORD idx, bool ok)
{
    dir->anchor_write_idx = ((idx + 1U) % md_ring_total_records(ring));

    if (ok && (dir->dirty_gen == dir->anchor_gen))
    {
        dir->dirty = false;
    }

    dir->anchor_busy  = false; /* 单飞释放（成败都释放：残缺记录不会再被续写） */
    dir->anchor_owner = MD_ANCHOR_OWNER_NONE;
}

// 释放 Anchor 单飞锁：只释放自己持有的那一个（别人的超时中止不能清本 writer 的锁）。
void md_anchor_abort(md_dir_cache_t *dir, BYTE owner)
{
    // 只释放**自己**的锁：另一个 job 的超时中止不能把本 writer 的单飞清掉。
    if (dir->anchor_owner == owner)
    {
        dir->anchor_busy  = false;
        dir->anchor_owner = MD_ANCHOR_OWNER_NONE;
    }
}

/**
 * @brief 读胜者的全部页，逐页校验后覆盖对应区间。
 * 镜像池：每页先读首选 plane；该页不可信时回退到另一个 plane（sequence 必须仍等于
 * 被选中的 record sequence，绝不跨 record 拼页）。
 *
 * @param dir    Directory cache（输出：被覆盖的 entries / backup_pba）
 * @param ring   池几何
 * @param newest 胜出 record 的槽位号
 */
static void md_load_assemble(md_dir_cache_t *dir, const md_ring_t *ring, WORD newest)
{
    md_anchor_page_t page;
    const BYTE       plane_count = md_ring_plane_count(ring->plane_mask);
    DWORD            dgen        = 0U; /* chunk 0 主半的目录代数（RISK-MD-06）*/
    bool             dgen_set    = false;

    for (BYTE chunk = 0U; chunk < ANCHOR_PAGE_COUNT; chunk++)
    {
        const bool backup      = (chunk >= ANCHOR_DIR_PRIMARY_PAGES);
        const BYTE local_chunk = backup ? (BYTE)(chunk - ANCHOR_DIR_PRIMARY_PAGES) : chunk;
        const WORD base_entry  = (WORD)(local_chunk * METADATA_DIR_ENTRIES_PER_PAGE);

        for (BYTE pi = 0U; pi < plane_count; pi++)
        {
            const BYTE plane = md_ring_plane_at(ring->plane_mask, pi);
            DWORD      addr  = md_ring_addr_plane(ring, &dir->topo, newest, chunk, plane);

            if (ftl_sync_read_page(addr, &page, sizeof(page), FTL_PAGE_CFG_META()) != 0)
            {
                continue;
            }
            const bool header_ok = md_anchor_header_valid(page.magic, page.chunk_count, page.layout_version);

            if ((header_ok == false) || (page.sequence != dir->anchor_sequence) || (page.chunk_index != chunk))
            {
                continue; /* 该副本不可信：试另一个 plane；都失败则保留基础层的"未映射" */
            }

            // 防御性二次校验（RISK-MD-06）：主半各页必须同代。探针已整条校验过，
            // 这里再挡一次 —— 混代页宁可不加载（回落"未映射"），也不能把两个代次的
            // 条目拼成一份目录。
            if (backup == false)
            {
                if (dgen_set == false)
                {
                    dgen     = page.dirty_gen;
                    dgen_set = true;
                }
                else if (page.dirty_gen != dgen)
                {
                    continue;
                }
            }

            // entry_count 来自介质（不可信）：先夹到本页容量再窄化到 WORD。
            const WORD n = (WORD)((page.entry_count < METADATA_DIR_ENTRIES_PER_PAGE) ? page.entry_count : METADATA_DIR_ENTRIES_PER_PAGE);

            for (WORD k = 0U; k < n; k++)
            {
                const WORD entry_idx = (WORD)(base_entry + k);

                // entry_count 来自介质（不可信，H29）：越界即停。
                if (entry_idx >= METADATA_DIR_ENTRIES)
                {
                    break;
                }

                if (backup == false)
                {
                    dir->entries[entry_idx] = page.entries[k];
                }
                else if ((page.entries[k].type == METADATA_TYPE_L2P_REGION) && (page.entries[k].id == entry_idx))
                {
                    // 备份半 best-effort：缺页/损坏的页保留基础层的 INVALID（单副本运行）。
                    dir->backup_pba[entry_idx] = page.entries[k].pba;
                }
                else
                {
                    // 非本 region 的条目：忽略（介质输入不可信，H29）。
                }
            }
            break; /* 本页已从某个 plane 取到有效副本 */
        }
    }
}

// 实现要点：先 fill_fixed 铺基础层（Anchor 缺页/损坏时该区间回落"未映射"，不出现空洞）；
// 定位失败或无有效记录 ⇒ 清游标返回错误；读胜者逐页校验，坏页保留基础层；
// **日志块游标不跨重启恢复**；Anchor 游标由 locate 结果恢复。
ftl_status_e md_load(md_dir_cache_t *dir, alc_t *alloc, const DWORD *total_writes)
{
    // 前置条件：唯一调用点传 ftl->metadata_dir / &ftl->alloc / &ftl->total_writes。
    dir->alloc        = alloc;
    dir->total_writes = total_writes;

    // 在飞快照位是 RAM 态、不跨重启：上一次运行的写回全部随掉电终止。
    md_slot_reset(dir);

    // 基础层：全部 region = NAND_PBA_INVALID（未映射）。
    // Anchor 缺页/损坏时该区间回落"未映射"，不会出现"未登记"的空洞。
    md_fixed_fill(dir);
    md_live_rebuild(dir); /* 失败路径也要给出确定的活计数（开机日志用）*/

    md_ring_t            ring;
    md_ring_locate_out_t loc;
    anchor_probe_ctx_t   ctx;

    md_anchor_ring_init(dir, &ring);
    ctx.dir  = dir;
    ctx.ring = &ring;

    {
        const ftl_status_e status = md_ring_locate(&ring, md_anchor_record_probe, &ctx, &loc);

        dir->anchor_sequence  = loc.sequence;
        dir->anchor_write_idx = loc.write_idx;

        // 观测口（**INFO**，随开机必打印）：Anchor 读侧结论。
        // loc=0 且 newest=65535 ⇒ 池内一条有效 record 都没有（写没落盘 / 读不回来？）。
        // loc=0 且 newest<N ⇒ 找到了，看 seq 是否与写侧对得上。
        // ⚠ 必须 INFO：本函数在 ftl_init 里跑，早于 CLI ⇒ `log level debug` 是开机后
        // 才敲的，LOGD 在此处永远看不到。
        LOGI(LOG_TAG_META "anchor load: loc=%d newest=%hu seq=%lu\r\n", (int)status, loc.newest_idx, dir->anchor_sequence);

        if ((status != FTL_OK) || (loc.newest_idx == UINT16_MAX))
        {
            dir->anchor_sequence     = 0U;
            dir->anchor_write_idx    = 0U;
            dir->anchor_cursor_valid = false;
            dir->dirty               = false;
            dir->dirty_gen           = 0U;
            dir->anchor_gen          = 0U;
            md_boot_status_log(dir);
            return FTL_ERROR; /* 无有效 Anchor：调用者转 md_init */
        }
    }

    dir->anchor_cursor_valid = true;

    md_load_assemble(dir, &ring, loc.newest_idx);

    dir->dirty      = false;
    dir->dirty_gen  = 0U;
    dir->anchor_gen = 0U;

    // 由装配后的 entries 重建活计数；**日志块游标不跨重启恢复**：首次追加时从**固定
    // L2P 日志区**新取一块（绝不复用可能已编程的旧页）。旧日志块仍被 Directory 引用 ⇒
    // 由 l2p_live 保住，待其 region 全部改写后由后台整理 job 异步擦除归还。
    md_live_rebuild(dir);
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        dir->cur_block[p] = 0U;
        dir->cur_page[p]  = 0U;
        dir->cur_valid[p] = false;
    }
    md_reclaim_reset(dir); /* 待回收位图不跨重启（同 job：全是 RAM 态）*/
    md_handle_reset(dir);  /* 整理 job 不跨重启（同游标：phase / 游标 / 请求位全是 RAM 态）*/

    md_boot_status_log(dir);

    return FTL_OK;
}

/* ══════════════════════════════════════════════════════════════════
 * 六、元数据块判定
 * ══════════════════════════════════════════════════════════════════ */

// 实现要点：三分支——
// ① 固定保留区：`block < FTL_METADATA_AREA_END`（含 shared spare；名义 BAD 块也保留）；
// ② 当前日志块（刚分配还没 commit，Directory 扫不到，必须显式排除）；
// ③ 已登记日志块：用 `l2p_live[g] != 0` 做 O(1) 判定（该账本的定义就是
// "最新位置落在 g 的 region 数"）。debug 档保留全表扫描做差分自检。
// ②③ 是**从用户区动态取的**块 ⇒ 必须带 plane 比较；只比块号会连带掩掉另一 plane
// 的同号块（既出不了 free pool、又被 full metadata rebuild 跳过，白丢一个可用块）。
bool md_block_is_metadata(const md_dir_cache_t *md_dir, BYTE plane, WORD block)
{
    /* ── 固定元数据块：0..FTL_METADATA_AREA_END-1 **全部保留** ──
     * 包含 shared metadata spare；名义块即使 BAD 也不回归数据池（否则 BBT 记录的
     * 坏块会被 carve 当用户块再分出去）。这比逐分区区间判断更简单，且与候选池
     * 的物理块集合一致（C9：一个分类坐标）。 */
    if (block < FTL_METADATA_AREA_END)
    {
        return true;
    }

    // md_dir == NULL：只判固定分区（开机 carve / recovery 的"固定块"判据用）。
    // 这不是错误——调用者明确只要固定分区视图。
    if (md_dir == NULL)
    {
        return false;
    }

    /* ── 当前日志块：可能尚无任何 region 指向它（刚分配、还没 commit），
     *    Directory 扫不到 ⇒ 必须显式排除，否则会被 Free Pool 回填当用户块收走。 ── */
    if ((plane < PLANE_NUM) && (block < BLOCKS_PER_PLANE))
    {
        const WORD g_cur = ftl_gblock(plane, block);

        if (md_dir->cur_valid[plane] && (g_cur == md_dir->cur_block[plane]))
        {
            return true;
        }
    }

    /* ── 已登记日志块：l2p_live 是 Directory 条目的账本，O(1) 判定 ──
     * block 可能来自损坏介质：先挡掉越界值，避免 ftl_gblock 截断后误判。 */
    const bool in_range = (block < BLOCKS_PER_PLANE);
    const WORD g        = in_range ? ftl_gblock(plane, block) : FTL_BLOCK_TOTAL;
    const bool fast     = in_range && (g < FTL_BLOCK_TOTAL) && (md_dir->l2p_live[g] != 0U);

#ifndef NDEBUG
    // 差分自检：证明 O(1) 账本判据与 Directory 逐条扫描完全等价。release 编译期消失。
    // 扫描使用与 md_live_rebuild 相同的过滤条件（挡越界 / 固定块），
    // 避免损坏介质上的条目让调试版误报。
    bool slow = false;

    for (WORD i = 0U; i < md_dir->count; i++)
    {
        if (md_dir->entries[i].type != METADATA_TYPE_L2P_REGION)
        {
            continue;
        }

        // 主/备两份都参与差分（副本只占备份位置时，只扫主会让 fast != slow）。
        const WORD rid = md_dir->entries[i].id;

        for (BYTE copy = 0U; copy < L2P_REGION_COPIES; copy++)
        {
            DWORD pba;

            if (copy == 0U)
            {
                pba = md_dir->entries[i].pba;
            }
            else if (rid < METADATA_DIR_ENTRIES)
            {
                pba = md_dir->backup_pba[rid];
            }
            else
            {
                pba = NAND_PBA_INVALID;
            }

            if (pba == NAND_PBA_INVALID)
            {
                continue;
            }

            const BYTE ep = pba_to_plane(pba);
            const WORD eb = pba_to_block(pba);

            if ((ep >= PLANE_NUM) || (eb >= BLOCKS_PER_PLANE) || md_block_is_metadata(NULL, ep, eb))
            {
                continue;
            }
            if ((ep == plane) && (eb == block))
            {
                slow = true;
                break;
            }
        }

        if (slow)
        {
            break;
        }
    }
    ASSERT(fast == slow);
#endif

    return fast;
}

/* ══════════════════════════════════════════════════════════════════
 * 七、池诊断与服务
 * ══════════════════════════════════════════════════════════════════ */

// 输出每个分区的 nominal / 实际候选数，以及因 BAD 被 shared spare 顶替的块。
// 只读 RAM，不碰介质。
void md_pool_print(const md_dir_cache_t *dir)
{
    if ((dir == NULL) || (dir->alloc == NULL) || (dir->alloc->pools == NULL))
    {
        LOGW(LOG_TAG_POOL "not initialized\r\n");
        return;
    }

    for (DWORD p = 0U; p < s_md_partition_count; p++)
    {
        const md_partition_t *part = &s_md_partitions[p];

        if (part->first_block == L2P_LOG_BLOCK0)
        {
            DWORD good = 0U;

            for (WORD block = L2P_LOG_BLOCK0; block < L2P_LOG_AREA_END; block++)
            {
                for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
                {
                    if (ftl_block_is_bad(&dir->alloc->blocks[ftl_gblock(plane, block)]) == false)
                    {
                        good++;
                    }
                }
            }

            LOGI(LOG_TAG_POOL "%-10s range=%u..%u good(plane-blocks)=%lu\r\n", part->name, L2P_LOG_BLOCK0, (L2P_LOG_AREA_END - 1U), good);
            continue;
        }

        const md_ring_t *ring = NULL;

        if (part->first_block == ANCHOR_BLOCK0)
        {
            ring = &dir->pools.anchor;
        }
        else if (part->first_block == CHECKPOINT_BLOCK0)
        {
            ring = &dir->pools.cp;
        }
        else if (part->first_block == BBT_BLOCK)
        {
            ring = &dir->pools.bbt;
        }
        else if (part->first_block == JOURNAL_BLOCK0)
        {
            ring = &dir->pools.jrn;
        }

        if (ring == NULL)
        {
            continue;
        }

        LOGI(LOG_TAG_POOL "%-10s nominal=%hu candidates=%hu\r\n", part->name, ring->nominal_count, ring->block_count);

        for (WORD i = 0U; i < ring->block_count; i++)
        {
            const WORD b = ring->blocks[i];

            if ((b < ring->first_block) || (b >= (ring->first_block + ring->nominal_count)))
            {
                LOGI(LOG_TAG_POOL "%-10s remapped -> spare block=%hu (candidate_idx=%hu)\r\n", part->name, b, i);
            }
        }
    }
}

// 元数据 BAD 后的候选池重算（[svc] 步骤之一，调用点见 ftl_service）。
// 契约（时机 / 前置 / 返回）见 include/ftl/metadata.h；此处只记实现要点：
//   · 三个入口门，任一不满足即本轮跳过、下轮再来：pools 未置脏 / bg_suspend /
//     有元数据类命令在飞（CP、flush、migrate、journal、BBT、目录整理）——
//     换游标可能让在飞命令的落点与新候选表不一致。
//   · 重算成功后作废三条游标（Anchor / BBT / CP）：下一次定位按新候选表重扫。
//   · 重算失败（元数据区已无足够好块）⇒ 清脏位 + 置 bbt.rebuild_required，
//     停止每轮重试；下次开机由 BBT / 全量重建兜底。
//   · 必须排在**本轮新 I/O 之前**（它是 ftl_service 的第一步）：游标与候选表先一致，
//     随后才允许分配与提交。
void md_pool_service(ftl_ctx_t *ftl)
{
    alc_t          *alloc   = &ftl->alloc;
    md_dir_cache_t *dir     = ftl->metadata_dir;
    l2p_jrn_t      *journal = ftl->journal;

    if ((alloc->pools == NULL) || (alloc->pools->dirty == false) || ftl->bg_suspend)
    {
        return; /* bg_suspend：bbt --erase / format 期间 RAM 状态已不可信，不重建 */
    }

    // 等元数据写路径空闲：换游标可能让在飞命令的落点与新候选表不一致。
    // 单线程 superloop ⇒ 这里不会与完成回调并发；只是在下一轮再试。
    if (ftl->cp_handle->active || ftl->flush_active || ftl->mig_handle->active || (journal->handle.phase != JRN_IDLE) || alloc->bbt.handle.waiting || md_is_busy(dir))
    {
        return;
    }

    // 旧 Journal 候选表快照：只有它真的变了才复位环形游标（否则不应丢弃缓冲页）。
    WORD old_jrn[JOURNAL_BLOCK_COUNT];

    for (BYTE b = 0U; b < JOURNAL_BLOCK_COUNT; b++)
    {
        old_jrn[b] = journal->blocks[b];
    }

    if (md_pool_build_all(alloc) == false)
    {
        // 元数据区已无足够好块：停止重试（避免每轮刷屏），下次开机由 BBT/rebuild 兜底。
        alloc->pools->dirty         = false;
        alloc->bbt.rebuild_required = true;
        bbt_dirty_note(alloc);
        LOGE(LOG_TAG_POOL "rebuild failed — metadata area exhausted, rebuild required\r\n");
        return;
    }

    // 游标全部作废：下一次提交/定位会按新候选表重新扫池。
    dir->anchor_cursor_valid              = false;
    alloc->bbt.cursor_valid               = false;
    ftl->cp_handle->cp_state.cursor_valid = false;

    // Journal 候选表变了 ⇒ 环形下标到物理块的映射已变。不能把游标简单归零
    // （新下标可能指向未擦块；直接写 = 二次编程）。按新候选表重新扫环、接回
    // 最新好页（jrn_page_scan_latest + jrn_resume），坏块上的条目由 REBUILD_REQUIRED
    // 在下次开机 full rebuild 恢复。
    bool jrn_changed = false;

    for (BYTE b = 0U; b < JOURNAL_BLOCK_COUNT; b++)
    {
        if (old_jrn[b] != alloc->pools->jrn.blocks[b])
        {
            jrn_changed = true;
        }
        journal->blocks[b] = alloc->pools->jrn.blocks[b];
    }

    if (jrn_changed)
    {
        jrn_page_ref_t latest;
        jrn_handle_t  *jrn        = &journal->handle;
        bool           read_error = false;

        // 旧下标的失败记录先清掉，否则 jrn_resume 会把新候选误判成坏块。
        jrn->bad_mask         = 0U;
        jrn->pre_erased_block = JRN_BLOCK_NONE;
        jrn->erase_fail_count = 0U;
        jrn->erase_fail_block = JRN_BLOCK_NONE;
        jrn->ring_full        = false; /* 新候选池可能已补回 spare */
        (void)memset(jrn->page_buf, 0xFF, sizeof(jrn->page_buf));

        (void)jrn_page_scan_latest(journal, &latest, &read_error);

        // floor 取当前序号，保证 resume 不回退；merge_floor 取最近 CP 水位，
        // 阻止跨 CP 的旧条目就地合并。
        const DWORD floor = jrn->sequence_counter;

        jrn_resume(journal, &latest, floor);
        jrn->merge_floor = ftl->cp_handle->cp_state.last_journal_sequence;

        // 环里扫不到有效页时，jrn_resume 会把写点放到第一个可用块的 page0——
        // 该块在运行期不保证已擦（与开机 fresh 的假设不同）。挂起一次换块擦除，
        // 避免对未擦页二次编程。
        if (latest.page_seq == 0U)
        {
            jrn->target_block = jrn->current_block;
            jrn->need_erase   = true;
        }
    }

    // 任一 plane 的当前 L2P 日志块若已 BAD ⇒ 该 plane 下次 append 重新分配。
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        if (dir->cur_valid[p] && ftl_block_is_bad(&alloc->blocks[dir->cur_block[p]]))
        {
            dir->cur_valid[p] = false;
        }
    }

    // 触发一次 Anchor 提交 + Checkpoint：把 RAM 里的权威状态重新落到新候选池。
    dir->dirty = true;
    jrn_checkpoint_request(journal);
    bbt_dirty_note(alloc);

    if ((s_pool_rebuilt_logged++ % MD_POOL_REBUILT_LOG_EVERY) == 0U)
    {
        LOGW(LOG_TAG_POOL "rebuilt after metadata BAD (jrn_changed=%u rebuild_required=%u count=%lu)\r\n", (jrn_changed ? 1U : 0U), (alloc->bbt.rebuild_required ? 1U : 0U), s_pool_rebuilt_logged);
    }
}

/* ══════════════════════════════════════════════════════════════════
 * 九、L2P region 写回槽位账本（RISK-MD-06）
 *
 * ⚠ 本节的函数**全部标 `SSD_CODE_FAST`（内部 Flash）**：本文件其余部分整份进
 *   `.ssd_code_high`（ITCM，64 KB 硬上限），而这里每次 2 KiB 写回只调一次、
 *   不是热路径。函数级放置靠 `-ffunction-sections` + 链接脚本按段收集实现
 *   （`KEEP(*obj/ftl/metadata.o(.text .text.*))` 不含 `.ssd_code_fast`）。
 *
 * 不变量（单一来源）：`slot_resv[]` 的一项 = 一次「已发槽、尚未 commit/失败」的写回。
 * 这段时间该槽所在块**不得换出/回收**——槽里的数据还没进 Directory，`l2p_live`
 * 看不见它；只凭 `l2p_live == 0` 擦块会让写完成后的 commit 把 Directory 指向已擦页。
 *
 * 配对（H1）：`md_slot_pair_append` 成功一次，调用者必须恰好 `md_slot_release` 一次
 * （commit 成功，或建链/提交/写失败）。漏释放的方向是"块暂时不可回收"（容量损失，
 * 可见），不会写坏介质；重复释放由 debug 断言暴露。
 * ══════════════════════════════════════════════════════════════════ */

SSD_CODE_FAST void md_slot_reset(md_dir_cache_t *dir)
{
    for (BYTE i = 0U; i < MD_SLOT_RESV_MAX; i++)
    {
        dir->slot_resv[i] = NAND_PBA_INVALID; /* RAM 态，不跨重启 */
    }
}

SSD_CODE_FAST bool md_slot_pair_append(md_dir_cache_t *dir, alc_t *alloc, DWORD out[L2P_REGION_COPIES])
{
    // 前置条件：调用者传 ftl->metadata_dir / &ftl->alloc / 栈上数组（调用链已保证）。
    if (md_handle_is_idle(dir) == false)
    {
        return false; /* 整理 job 在飞：背压语义（见 md_log_rotate 的 job 让路说明） */
    }

    // 预算：主副本 → plane0 当前块、备副本 → plane1 当前块，**每个 plane 各需 1 页**。
    // 先逐 plane 确认（必要时换该 plane 的块）：任一 plane 换不出块 ⇒ 背压，
    // **未消耗任何槽**（两副本不会出现"取到一个、另一个失败"的半对）。
    for (BYTE p = 0U; p < L2P_REGION_COPIES; p++)
    {
        if (p >= PLANE_NUM)
        {
            return false; /* 编译期断言保证不可达 */
        }

        if (dir->cur_valid[p])
        {
            const bst_block_t *b = &alloc->blocks[dir->cur_block[p]];

            if (ftl_block_is_bad(b))
            {
                dir->cur_valid[p] = false; /* 块被标坏：游标作废，下面换块 */
            }
        }

        if ((dir->cur_valid[p] == false) || ((WORD)dir->cur_page[p] + 1U > PAGES_PER_BLOCK))
        {
            if (md_log_rotate(dir, alloc, p) == false)
            {
                return false;
            }
        }
    }

    for (BYTE p = 0U; p < L2P_REGION_COPIES; p++)
    {
        out[p] = md_slot_take(dir, p);
        if (out[p] == NAND_PBA_INVALID)
        {
            return false; /* 预检保证不可达；已取到的槽作废（不登记 = 不会被误擦） */
        }
    }

    // 在飞快照位：主/备**各登记一条**（两个副本落在不同 plane 的块上，各自都要防误擦）。
    // 表容量 = 写回并发上限 × 副本数（l2p_cache.c 静态断言）。**先确认有足够空位**再登记：
    // 否则只登记了主副本、备副本失败返回 false 时，主副本那条会永久泄漏（块不可回收）。
    {
        BYTE free_n = 0U;

        for (BYTE i = 0U; i < MD_SLOT_RESV_MAX; i++)
        {
            if (dir->slot_resv[i] == NAND_PBA_INVALID)
            {
                free_n++;
            }
        }
        if (free_n < L2P_REGION_COPIES)
        {
            return false; /* 背压；槽作废但没有数据落盘 */
        }

        for (BYTE p = 0U; p < L2P_REGION_COPIES; p++)
        {
            for (BYTE i = 0U; i < MD_SLOT_RESV_MAX; i++)
            {
                if (dir->slot_resv[i] == NAND_PBA_INVALID)
                {
                    dir->slot_resv[i] = out[p];
                    break;
                }
            }
        }
    }

    return true;
}

SSD_CODE_FAST void md_slot_release(md_dir_cache_t *dir, DWORD primary, DWORD backup)
{
    BYTE released = 0U;

    for (BYTE i = 0U; i < MD_SLOT_RESV_MAX; i++)
    {
        if ((dir->slot_resv[i] == primary) || (dir->slot_resv[i] == backup))
        {
            dir->slot_resv[i] = NAND_PBA_INVALID;
            released++;
        }
    }

    // 期望恰好两条（主 + 备）。少 = 重复释放/漏登记，多 = 传错；账本其余项不动。
    ASSERT(released == L2P_REGION_COPIES);
}

SSD_CODE_FAST bool md_log_rotate(md_dir_cache_t *dir, alc_t *alloc, BYTE plane)
{
    if (plane >= PLANE_NUM)
    {
        return false;
    }

    // 该 plane 的当前块已写满且无活 region：先从"当前写块"身份上摘下来，否则
    // mark_all 会把它永远排除；日志区没有空闲块时就再也换不了块（死等）。
    // ⚠ 有在飞写回的槽位时不能摘身份：槽里的数据还没进 Directory，l2p_live 为 0
    //   不代表块已空（RISK-MD-06）——摘掉后它会被 mark_all 登记并擦除。
    if (dir->cur_valid[plane] && (dir->cur_page[plane] >= PAGES_PER_BLOCK) && (dir->l2p_live[dir->cur_block[plane]] == 0U))
    {
        const WORD gblock = dir->cur_block[plane];

        if (md_slot_reserved(dir, gblock) == false)
        {
            (void)md_reclaim_mark(dir, gblock);
            dir->cur_valid[plane] = false;
        }
    }

    // 换块时机扫一遍全池：只**登记**零活 LOG 块，绝不在这里擦。
    // 擦除由后台 job 异步做——同步 tERASE ~3 ms 会击穿服务预算。
    md_reclaim_mark_all(dir, alloc);

    // 只剩最后 1 个空闲块：先登记压缩再把它用作当前块——新块有整块空槽，
    // 压缩才有目标可搬。free 水位判据见 L2P_LOG_FREE_WATERMARK。
    if (md_log_area_free_count(alloc) <= 1U)
    {
        dir->handle.compact_wanted = true;
    }

    // 只从**本 plane 的固定 L2P 日志区**取块（md_log_alloc），不碰用户 Free Pool。
    const WORD gblock = md_log_alloc(dir, alloc, plane);

    if (gblock == FTL_GBLOCK_INVALID)
    {
        // 该 plane 的固定日志区无空闲块：所有块都是活/当前/待擦/BAD。**必须请求压缩**——
        // 只返回 false 而不置 compact_wanted 时，后台整理 job 无事可做，append_slot_pair
        // 永远返回 INVALID，CP 在 CP_FLUSH_REGIONS 每 10~25ms abort 一次、写不成 Header
        // ⇒ 掉电后每次开机全量重建（实测 `free=3241 cmd=0 buf=0` 仍 stall）。
        dir->handle.compact_wanted = true;
        LOGD(LOG_TAG_META "log area exhausted (plane=%hhu): live=%hu reclaim=%hu cur_page=%hhu — request compact\r\n", plane, md_live_block_count_get(dir), dir->reclaim_pending_count, dir->cur_page[plane]);
        return false;
    }

    const WORD old_cur = dir->cur_block[plane];
    const bool had_cur = dir->cur_valid[plane];

    dir->cur_block[plane] = gblock;
    dir->cur_page[plane]  = 0U;
    dir->cur_valid[plane] = true;

    // 旧当前块可能已被搬空（压缩只搬活 region，零活块不会成为 victim）：
    // 换走后它再也不会被追加，登记给后台回收。
    // ⚠ 有在飞写回槽位时不能登记（RISK-MD-06）：槽里的数据还没进 Directory。
    if (had_cur && (dir->l2p_live[old_cur] == 0U) && (md_slot_reserved(dir, old_cur) == false))
    {
        (void)md_reclaim_mark(dir, old_cur);
    }

    // 抗碎片：刚换到新日志块 ⇒ 块内有整 PAGES_PER_BLOCK 个空槽，正好够把多个稀疏
    // victim 一次搬空。触发判据是**元数据空间水位**（live 日志块数），不是用户
    // Free Pool——旧实现只在 free≤32 时压，运行期大部分时间不压，日志碎片从理想
    // 32 块一路涨到上千块，把用户容量吃光（RISK-MD-07 / RISK-CAP-02）。
    // 只**登记请求**，本次不发放槽位：调用者收到 NAND_PBA_INVALID 返回 FTL_AGAIN，
    // 下轮重试；若 job 已起则继续让路直到压缩走完。
    if (md_live_block_count_get(dir) > L2P_LOG_COMPACT_TRIGGER)
    {
        dir->handle.compact_wanted = true;
        return false;
    }

    return true;
}

/* ⚠ 标 `noinline` 是**放置约束**，不是性能选择：本文件其余部分在 ITCM，而本函数在
 * Flash；若让 GCC 把它内联进同 TU 的调用者，那份循环会被复制进 ITCM（实测 +100 B，
 * release ITCM 只剩 ~300 B 余量）。保持外联 = 一次 Flash 调用，换 ITCM 预算。 */
SSD_CODE_FAST __attribute__((noinline)) bool md_slot_reserved(const md_dir_cache_t *dir, WORD g)
{
    if (g >= FTL_BLOCK_TOTAL)
    {
        return false;
    }

    for (BYTE i = 0U; i < MD_SLOT_RESV_MAX; i++)
    {
        const DWORD pba = dir->slot_resv[i];

        if ((pba != NAND_PBA_INVALID) && (ftl_pba_gblock(pba) == g))
        {
            return true;
        }
    }

    return false;
}

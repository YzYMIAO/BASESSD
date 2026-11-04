/*
 * checkpoint.c — L2P 全量快照（Checkpoint 池环形轮转 + 触发/水位）
 *
 * 职责：
 *   把 L2P 缓存里的脏 region 固化进 NAND L2P 日志块、flush Metadata Directory（目录检查点）、
 *   再写一条 Checkpoint Header 记录「此刻 L2P 已落盘到哪」，最后截断 Journal；
 *   开机由 cp_pool_find_latest 在池内挑最新一条有效记录恢复游标/序号。
 *
 * 与 journal 的分工：
 *   Journal    —— L2P 的**增量**日志（每笔映射一条，掉电回放的最小单位）；
 *   Checkpoint —— L2P 的**全量**快照（把 journal 已写条目固化进 NAND L2P 表并截断 journal）。
 *   两者用各自的 sequence：Journal.sequence_counter 是全局单调；本池 h->cp_state.sequence 独立。
 *
 * 池几何（由 ftl/metadata.h 推导，见 cp_ring_init）：
 *   CHECKPOINT_POOL_BLOCKS 个块、一条 record = 21 页、P0+P1 镜像；
 *   写游标 h->cp_state.write_idx 在 [0, 总记录数) 环形推进，块满由 md_ring_prepare_addr
 *   判定（同步站点当场擦；异步 job 提交异步擦，见 CP_HDR_ERASE）。
 *
 * 掉电安全顺序（不可倒，见下方"异步 job"的长注释）：
 *   先 flush region → 再 flush 目录检查点 → 最后写 Header 声称「已落盘」。
 *   Header 是"承诺书"：它一旦落盘，boot 就信 bitmap 里置位的 region 在 NAND 上，
 *   故任何被 Header 声称的落盘必须**先于** Header 完成。
 *
 * 文件结构：
 *   1. 内部辅助        cp_header_is_valid / cp_record_probe
 *                      （单调毫秒 / 心跳基准见 log.h / timer.h）
 *   2. 异步 job        cp_job_step（static）/ cp_job_begin /
 *                      cp_handle / ftl_checkpoint_callback
 *   3. 创建/恢复       cp_pool_find_latest / cp_initial_record_write
 *   4. 触发/上报       cp_should_start / cp_handle
 *
 * 游标/序号是 **per-instance**（ftl_ctx_t.cp，见 cp_state_t）——
 * 多实例（多 LUN）下每实例一份，互不干扰。
 */

#include "checkpoint.h"

#include "assertion.h"
#include "bbt.h"         /* bbt_medium_has_ftl_data（CP 缺失时的空片判定） */
#include "boot_policy.h" /* boot_crc32（块元数据快照跨页校验和） */
#include "fc.h"
#include "ftl.h"
#include "ftl_fc_interface.h"
#include "ftl_op.h" /* ftl_op_submit / FTL_OP_CHECKPOINT */
#include "log.h"
#include "metadata.h"
#include "onfi_1_0.h"
#include "timer.h"

#include <string.h>

// 池扫描用的 Header 暂存：单线程 superloop 串行调用，放文件级 static（AXI .bss），
// 避免 `cp_record_probe` 每次在栈上放 2 KB 的 cp_header_t。
static cp_header_t s_probe_hdr;
static BYTE        s_probe_raw[PAGE_SIZE];

/* ── 内部定义（仅本编译单元使用）── */

// cp_regions_flush 一步的结果（调用点按具名值分派，不写数值魔法）。
typedef enum
{
    CP_FLUSH_PENDING = 0U, /* 仍有脏行在飞：调用者 return，下轮再来 */
    CP_FLUSH_DONE    = 1U, /* 全部 region 已持久：推进 phase */
    CP_FLUSH_FAILED  = 2U, /* 硬失败：调用者 cp_job_abort */
} cp_flush_step_e;

// 本模块对 metadata.h 记录几何的短别名。
#define CP_BLOCK_WORD_BYTES     FTL_CHECKPOINT_BLOCK_WORD_BYTES
#define CP_BLOCK_WORDS_PER_PAGE FTL_CHECKPOINT_BLOCK_WORDS_PER_PAGE /* 1024 */
#define CP_BLOCK_PAGES          FTL_CHECKPOINT_BLOCK_PAGES          /* 4 */
#define CP_RECORD_PAGES         FTL_CHECKPOINT_RECORD_PAGES         /* 21 */

// 磨损快照条目：每块 8 B，按 gblock 索引（数组下标即块号，条目里不再存 block 字段）：
// erase_count 4 / read_count 2 / last_ecc 1 / flags 1
// flags: bit0..3 = rising_count, bit4..5 = health, bit6..7 = reserved
// 软状态：丢失只退化 WL/read-disturb 精度，不影响用户数据。
#define CP_WEAR_RISING_SHIFT (0U)
#define CP_WEAR_RISING_MASK  (0x0FU)
#define CP_WEAR_HEALTH_SHIFT (4U)
#define CP_WEAR_HEALTH_MASK  (0x03U)

typedef struct
{
    DWORD erase_count;                     /* 4 B */
    WORD  read_count;                      /* 2 B */
    BYTE  last_ecc;                        /* 1 B */
    BYTE  flags;                           /* 1 B：rising_count | health */
} __attribute__((packed)) cp_wear_entry_t; /* 8 B */

_Static_assert(sizeof(cp_wear_entry_t) == FTL_CHECKPOINT_WEAR_ENTRY_BYTES, "磨损快照条目尺寸必须与 metadata.h 的页几何一致");
_Static_assert((sizeof(cp_wear_entry_t) * FTL_CHECKPOINT_WEAR_ENTRIES_PER_PAGE) == PAGE_MAIN_SIZE, "磨损快照页必须恰好装满一页主区");

// T3 定时的最小 journal 增量：一条 CP record 是 21 页，加此门后轻载下不会每个
// 定时拍都写一条 record（数值为本仓经验值）。
#define CP_T3_MIN_JOURNAL_DELTA (64U)

// 池扫描上下文（cp_record_probe 的回调上下文）。
typedef struct
{
    ftl_topo_t topo;
    md_ring_t  ring;
    bool       any_version; /* true = 只校验 magic（格式化判据探针），不校验 layout_version */
} checkpoint_probe_ctx_t;

/* ── 内部辅助 ── */

/* ── Checkpoint 池：环形轮转（一条 record = 21 页、P0+P1 镜像）──
 *
 * 池几何由 ftl/metadata.h 推导；sequence 与写游标是**本池自己的**（与 journal /
 * 目录检查点 / BBT 的 sequence 互不相干），开机由 cp_pool_find_latest 恢复。
 *
 * 游标/序号是 per-instance 的 h->cp_state（cp_state_t）：file-scope static 在多实例
 * 下会互相覆盖。 */

/**
 * @brief cp_ring_init — 优先用候选池（跳过 BAD）；池未建立时退回名义区间
 *
 * @param alloc  allocator（取其 pools->cp 候选表；可为 NULL）
 * @param ring   输出：初始化后的 Checkpoint 环
 */
static void cp_ring_init(const alc_t *alloc, md_ring_t *ring)
{
    if ((alloc != NULL) && (alloc->pools != NULL))
    {
        *ring = alloc->pools->cp;
        return;
    }

    ring->first_block      = CHECKPOINT_BLOCK0;
    ring->nominal_count    = CHECKPOINT_POOL_BLOCKS;
    ring->block_count      = CHECKPOINT_POOL_BLOCKS;
    ring->pages_per_record = CP_RECORD_PAGES; /* Header(1) + 块元数据(4) + 磨损(16) */
    ring->plane_mask       = METADATA_MIRROR_PLANE_MASK;
    for (WORD i = 0U; i < CHECKPOINT_POOL_BLOCKS; i++)
    {
        ring->blocks[i] = (WORD)(CHECKPOINT_BLOCK0 + i);
    }
}

/**
 * @brief Header 校验（record 的 commit 页）：magic + 布局版本。
 * 无本层 CRC——页完整性由下层 FC 的 per-CW CRC + ECC 保证。
 *
 * @param hdr  待校验的 Header
 * @return true = magic / 版本 / sequence 均有效；false = 无效（含 sequence 0 或 UINT32_MAX）
 */
static bool cp_header_is_valid(const cp_header_t *hdr)
{
    if (hdr->magic != CP_MAGIC)
    {
        return false;
    }
    if (hdr->layout_version != FTL_MEDIA_LAYOUT_VERSION)
    {
        return false; /* 旧格式介质的 Checkpoint ⇒ 作废（触发开机自动格式化）*/
    }
    // sequence 0 = 未写；UINT32_MAX = 半编程 Header（擦除态 0xFF…）。
    // ⚠ 接受 UINT32_MAX 会让下一条 CP 的 `++sequence` 回绕成 0，之后所有 Header 都因
    // “sequence==0”在开机被忽略 ⇒ 每次启动仍全量重建（实测 `[CP  ] done seq=0`）。
    if ((hdr->sequence == 0U) || (hdr->sequence == UINT32_MAX))
    {
        return false;
    }

    return true;
}

/**
 * @brief Checkpoint record 探针（见 metadata_ring_probe_fn）。
 * full 与廉价探针等价（一条 record 只有一页），故忽略。
 *
 * @param ctx           探针上下文（checkpoint_probe_ctx_t *）
 * @param record_idx    待探测的记录槽位
 * @param full          是否完整探测（本池恒等价，忽略）
 * @param out_sequence  输出：命中记录的 sequence
 * @return FTL_OK 命中；FTL_ERROR 无镜像 plane 可读 / 校验不符
 */
static ftl_status_e cp_record_probe(void *ctx, WORD record_idx, bool full, DWORD *out_sequence)
{
    checkpoint_probe_ctx_t *c = (checkpoint_probe_ctx_t *)ctx;

    (void)full;

    // 镜像池：任一 plane 的副本有效即可。
    const BYTE plane_count = md_ring_plane_count(c->ring.plane_mask);

    for (BYTE pi = 0U; pi < plane_count; pi++)
    {
        const BYTE   plane = md_ring_plane_at(c->ring.plane_mask, pi);
        DWORD        addr  = md_ring_addr_plane(&c->ring, &c->topo, record_idx, 0U, plane);
        cp_header_t *hdr   = &s_probe_hdr;

        if (ftl_sync_read_page(addr, hdr, sizeof(*hdr), FTL_PAGE_CFG_META()) != FTL_OK)
        {
            continue;
        }
        if ((hdr->magic != CP_MAGIC) || (hdr->sequence == 0U) || (hdr->sequence == UINT32_MAX))
        {
            continue; /* 0 = 未写；UINT32_MAX = 半编程（擦除态），见 cp_header_is_valid */
        }
        if ((c->any_version == false) && (hdr->layout_version != FTL_MEDIA_LAYOUT_VERSION))
        {
            continue; /* 正常定位只认当前版本；格式化判据探针放宽到任意版本 */
        }

        *out_sequence = hdr->sequence;
        return FTL_OK;
    }

    return FTL_ERROR;
}

/* ── 创建 / 恢复 ── */

/* ══════════════════════════════════════════════════════════════════
 * Checkpoint 异步 job
 *
 * 同一份**顺序契约**由 phase 承载：每个阶段提交一条命令就返回，完成由
 * g_op_callbacks[FTL_OP_CHECKPOINT]（ftl_checkpoint_callback）按 phase 收尾，下一轮
 * ftl_service 调 cp_handle 续跑。同步流水线会阻塞数十毫秒
 * （脏 region 写回 + 8 页目录检查点 + Header + 预擦），期间 host I/O 的完成泵停摆。
 *
 * ⚠ **顺序即掉电安全契约（不可倒）**：
 *     ① 快照 region 脏标记 + journal 水位（**必须在任何 flush 之前**：flush 成功会清位）
 *     ② flush 脏 region（任一失败 ⇒ 中止，不写 Header）
 *     ③ flush 目录检查点（**必须早于 Header**）
 *     ④ 写 Header（**无论成败都推进池游标**：半编程槽不可复用）
 *     ⑤ 截断 Journal（预擦环形最旧块）+ 复位水位标志
 *
 * ⚠ **快照点必须在 job 起点**：CP 跨多轮，若把 `journal.sequence_counter` 取在
 *   目录检查点之后（CP 末尾），则在 CP_DIR_CKPT 期间完成的 host 写会同时满足：
 *   ① region 脏位在 RAM 里置了但没进本次快照（不会被 flush）；② 其 journal 条目
 *   seq ≤ 末尾水位（boot 视为"已进 L2P 表"而跳过回放）⇒ **两边都不认，静默丢映射**。
 *   故水位与脏位图**一起在 job 起点取定**：此后任何 host 写都有 seq > 快照水位，
 *   boot 必然回放它。代价是 journal_truncate 的水位判据更容易不满足（更少预擦）——
 *   预擦只是磨损优化，跳过它只是让 jrn_handle 换块时自己异步擦（每块每圈仍只擦一次）。
 *
 * ⚠ **服务函数不推进状态机**（与 migrate 的 ftl_migrate_callback 不同）：本 job 的
 *   CP_DIR_CKPT_PREP / CP_HDR_PREP 里有同步原语（游标未建立时要 md_ring_locate
 *   读池定位），在完成泵里跑就是嵌套泵（l2p_sync_pump 有 ASSERT 挡）。故
 *   ftl_checkpoint_callback 只记结果 + release + 改 phase，推进一律留给下一轮 ftl_service。
 * ══════════════════════════════════════════════════════════════════ */

/**
 * @brief 中止本次 CP：记**首次**失败相位、转 CP_DONE（**不**写 Header / **不**截断 journal）。
 * ⚠ 幂等：cp_job_stall 内部已调用一次，cp_regions_flush 的 FAILED 返回路径会再调一次；
 * 后面那次看到 phase 已是 CP_DONE，若覆盖 fail_phase 就会把真实相位（如 1）记成 12。
 *
 * @param h  Checkpoint handle
 */
static void cp_job_abort(cp_handle_t *h)
{
    // 放弃本 job 可能持有的目录检查点单飞锁（prepare 之后没走到 commit）。
    md_dir_ckpt_abort(h->metadata_dir, MD_DIR_CKPT_OWNER_CP);

    if (h->failed == false)
    {
        h->failed     = true;
        h->fail_phase = h->phase;
    }
    h->phase = CP_DONE;
}

/**
 * @brief abort 日志限流：CP 在资源饥饿时每 10~25ms 重试一次，逐次 WRN 会刷屏。
 * 每个实例每 LOG_RATE_SLOW_INTERVAL_MS 最多一行；时间戳跨 job 保留
 * （cp_job_begin 不重置）。
 *
 * @param h  Checkpoint handle（读并更新 rl_abort）
 * @return true = 到点可打印；false = 仍在限流窗口内
 */
static bool cp_job_abort_log_due(cp_handle_t *h)
{
    // 5s：CP 饥饿时每 10~25ms 重试一次，1s 一行仍偏密；卡死是持续状态，5s 足够可见。
    return log_ratelimit_due(&h->rl_abort);
}

/**
 * @brief 提交不出去超过 FTL_SUBMIT_TIMEOUT_MS ⇒ 中止 job（与 MIG/RR 同族）。
 * 返回 true = 已中止。时钟缺失时 deadline 不生效。
 *
 * @param h  Checkpoint handle（读/更新 stall_ms、timed_out）
 * @return true = 已中止本次 job；false = 未到 deadline 或时钟缺失
 */
static bool cp_job_stall(cp_handle_t *h)
{
    const DWORD now_ms = platform_now_ms();

    if (now_ms == 0U)
    {
        return false;
    }

    // cache 序列打开时被门控拒绝是预期等待（序列下一窗口收尾），不计提交 deadline。
    if (ftl_op_bg_submit_deferred() == true)
    {
        timer_deadline_clear(&h->submit_deadline);
        return false;
    }

    if (h->submit_deadline.armed == false)
    {
        timer_deadline_arm(&h->submit_deadline, now_ms, FTL_SUBMIT_TIMEOUT_MS);
        return false;
    }
    if (timer_deadline_expired(&h->submit_deadline, now_ms))
    {
        if (cp_job_abort_log_due(h))
        {
            fc_resource_stats_t rs;

            (void)fc_get_resource_stats(&rs);
            LOGW(LOG_TAG_CP "submit stalled %lums — abort phase=%d free=%hu cmd=%hu buf=%hu\r\n", (now_ms - h->submit_deadline.at) + FTL_SUBMIT_TIMEOUT_MS, h->phase, h->alloc->free_count, rs.command_used, rs.buffer_used);
        }
        h->timed_out     = true;
        h->timeout_phase = h->phase;
        cp_job_abort(h);
        return true;
    }
    return false;
}

/**
 * @brief 提交结果处理：true = 调用者必须 return（已中止，或已计卡死等待重试）。
 * FTL_ERROR = 配置/地址硬失败 ⇒ 立即中止；FTL_AGAIN = 瞬态背压 ⇒ 下轮重试。
 *
 * @param h   Checkpoint handle
 * @param st  提交/建链的返回码
 * @return true = 调用者必须 return；false = 提交成功，可继续推进
 */
static bool cp_command_result_handle(cp_handle_t *h, ftl_status_e st)
{
    if (st == FTL_OK)
    {
        return false;
    }
    if (st == FTL_ERROR)
    {
        cp_job_abort(h);
        return true;
    }

    (void)cp_job_stall(h);
    return true;
}

/**
 * @brief 提交一条异步写（目录检查点分页 / Checkpoint Header 共用）。同步镜像到 P0+P1：
 * 同一页数据用 multi-plane 一次写两个 plane；data 在本函数内已拷进 FC buffer
 * ⇒ 调用者的栈缓冲随即可复用。
 *
 * @param h       Checkpoint handle
 * @param addr    目标页地址（PBA）
 * @param data    待写数据（返回后调用者缓冲可复用）
 * @param length  写入长度（字节）
 * @return FTL_OK 已提交；FTL_AGAIN 背压；FTL_ERROR 建链失败
 */
static ftl_status_e cp_write_submit(cp_handle_t *h, DWORD addr, const void *data, DWORD length)
{
    fc_command_t *cmd;

    const ftl_status_e st = ftl_async_write_page_mirror(&cmd, addr, data, length, FTL_PAGE_CFG_META());

    if (st != FTL_OK)
    {
        return st; /* FTL_AGAIN 池空 / FTL_ERROR 建链失败 */
    }

    // 先置 waiting、提交失败再回滚：完成服务函数以 waiting 判在飞。
    h->waiting = true;
    if (ftl_op_submit(cmd, FTL_OP_CHECKPOINT, h) != FTL_OK)
    {
        h->waiting = false;
        (void)fc_command_release(cmd);
        return FTL_AGAIN;
    }

    timer_deadline_clear(&h->submit_deadline); /* 有命令交出去 ⇒ 卡死计时清零 */
    return FTL_OK;
}

/**
 * @brief 提交一条异步擦除（目录检查点换块 / Checkpoint 换块 / Journal 预擦共用）。
 * 固定元数据池是 P0+P1 镜像 ⇒ 换块必须 multi-plane 擦，否则副本 plane 不会清。
 *
 * @param h     Checkpoint handle
 * @param addr  待擦块的基地址（PBA）
 * @return FTL_OK 已提交；FTL_AGAIN 背压；FTL_ERROR 建链失败
 */
static ftl_status_e cp_erase_submit(cp_handle_t *h, DWORD addr)
{
    fc_command_t *cmd;

    const ftl_status_e st = ftl_async_erase_block_multi_plane(&cmd, addr);

    if (st != FTL_OK)
    {
        return st;
    }

    // 先置 waiting、提交失败再回滚：完成服务函数以 waiting 判在飞。
    h->waiting = true;
    if (ftl_op_submit(cmd, FTL_OP_CHECKPOINT, h) != FTL_OK)
    {
        h->waiting = false;
        (void)fc_command_release(cmd);
        return FTL_AGAIN;
    }

    timer_deadline_clear(&h->submit_deadline); /* 有命令交出去 ⇒ 卡死计时清零 */
    return FTL_OK;
}

/**
 * @brief 填写 Checkpoint Header 的固定字段；调用者随后按需补 timestamp。
 * region_bitmap == NULL 表示无 region 快照（initial record）。
 *
 * @param cp            输出：待填充的 Header
 * @param h             来源 handle（读 block_words / wear_page_crc）
 * @param sequence      本池的单调序号
 * @param journal_seq   本次 job 起点的 journal 水位
 * @param region_bitmap region 快照位图（可为 NULL = 无快照）
 */
static void cp_header_fill(cp_header_t *cp, const cp_handle_t *h, DWORD sequence, DWORD journal_seq, const DWORD *region_bitmap)
{
    memset(cp, 0, sizeof(*cp));

    if (region_bitmap != NULL)
    {
        memcpy(cp->region_bitmap, region_bitmap, sizeof(cp->region_bitmap));
    }

    // region_count = 非零 bitmap word 数（无消费方，仅信息量）。
    DWORD region_words = 0U;

    for (BYTE i = 0U; i < L2P_LOCAL_DIRTY_WORDS; i++)
    {
        if (cp->region_bitmap[i] != 0U)
        {
            region_words++;
        }
    }
    cp->region_count = region_words;

    cp->magic            = CP_MAGIC;
    cp->sequence         = sequence;    /* 本池自己的单调序号 */
    cp->journal_sequence = journal_seq; /* 截断到此 seq（job 起点快照） */
    cp->layout_version   = FTL_MEDIA_LAYOUT_VERSION;
    cp->block_word_count = FTL_BLOCK_TOTAL;
    cp->block_word_bytes = CP_BLOCK_WORD_BYTES;
    cp->block_page_count = CP_BLOCK_PAGES;
    cp->block_crc32      = boot_crc32(h->hdr.block_words, FTL_BLOCK_TOTAL * sizeof(WORD));
    cp->wear_entry_count = FTL_BLOCK_TOTAL;
    cp->wear_entry_bytes = FTL_CHECKPOINT_WEAR_ENTRY_BYTES;
    cp->wear_page_count  = FTL_CHECKPOINT_WEAR_PAGES;
    memcpy(cp->wear_page_crc, h->hdr.wear_page_crc, sizeof(cp->wear_page_crc));
    cp->record_pages = CP_RECORD_PAGES;
}

/**
 * @brief 按**快照**脏位图把每个脏 region 落盘（阶段②）。返回 CP_FLUSH_DONE = 全部已持久； CP_FLUSH_PENDING = 仍有在飞（下轮再来）；
 *        CP_FLUSH_FAILED = 硬失败（调用者中止，不写 Header）。
 *
 * @param h  Checkpoint handle（读快照位图、cache、metadata_dir）
 * @return CP_FLUSH_DONE    全部 region 已持久
 *         CP_FLUSH_PENDING 仍有在飞，下轮再来
 *         CP_FLUSH_FAILED  硬失败，调用者中止
 *
 * @note 判据是**快照位图**，但每个位的完成状态看**实时脏位**：dirty CMT 可能在
 * 没有 resident line 的 region 里，行也可能已被后台写回并驱逐。实时脏位已清 =
 * 本次 CP 已 commit；仍置则驱动 `l2p_region_flush_kick`（合并 CMT + line 后写回）。
 * Header 是承诺书：DONE 前用 `l2p_owner_snapshot_covered` 再验一遍，杜绝
 * "bit 置位但无行无 dirty" 的孤儿位被当成已持久。
 */
static cp_flush_step_e cp_regions_flush(cp_handle_t *h)
{
    const BYTE die     = topo_2_die(h->topo);
    bool       pending = false;
    BYTE       started = 0U;

    // 按 snapshot 位置驱动：dirty 可能在 line，也可能只在 CMT（无 resident line）。
    // 实时脏位已清 = 本次 CP 已 commit（行可能已被驱逐），直接放过。
    for (DWORD local = 0U; local < L2P_REGIONS_PER_INSTANCE; local++)
    {
        if ((h->region_bitmap[local / 32U] & (1UL << (local & 31U))) == 0U)
        {
            continue; /* 本次 CP 起点时不脏（此后新脏的由 journal 兜底） */
        }

        const DWORD global_region = (local * DIE_MAX) + die;

        if (l2p_region_dirty_live(h->cache, global_region) == false)
        {
            continue; /* 已 commit */
        }

        pending = true;

        if (started < L2P_FLUSH_MAX_NORMAL)
        {
            const ftl_status_e st = l2p_region_flush_kick(h->cache, global_region);

            if (st == FTL_ERROR)
            {
                return CP_FLUSH_FAILED; /* 无依赖 / 建链失败：中止 */
            }
            // FTL_AGAIN 是背压/在飞，不是失败；下轮重试。
            if (st == FTL_OK)
            {
                started++;
            }
        }
    }

    if (pending == false)
    {
        // Header 是承诺书：每个 snapshot 位必须已 commit 或行已 CLEAN。
        if (l2p_owner_snapshot_covered(h->cache, die, h->region_bitmap) == false)
        {
            LOGW(LOG_TAG_CP "snapshot region not covered — abort\r\n");
            return CP_FLUSH_FAILED;
        }
        timer_deadline_clear(&h->submit_deadline);
        return CP_FLUSH_DONE;
    }

    // 有在飞命令（含 CMT 合并加载）= 有进展。共享 credit 池是全局的，别的 LUN
    // 的在飞写回同样代表系统在推进；目录整理 job 在飞也同理。
    if (l2p_cache_any_inflight(h->cache) || md_is_busy(h->metadata_dir) || h->metadata_dir->handle.compact_wanted)
    {
        timer_deadline_clear(&h->submit_deadline);
        return CP_FLUSH_PENDING;
    }

    return cp_job_stall(h) ? CP_FLUSH_FAILED : CP_FLUSH_PENDING;
}

/* ── Checkpoint job 的完成服务（g_op_callbacks[FTL_OP_CHECKPOINT]）── */

// 每个提交阶段的完成处理（ftl_checkpoint_callback 按 phase 分派）。
// 共同前提：命令已 release、job.waiting 已清零（由派发处统一做）。

/**
 * @brief Header（commit 页）写完：无论成败都推进游标（已写过的槽可能半编程，不可复用）；
 * 成功才提交 journal 水位并清持久性失败标记。
 *
 * @param h       Checkpoint handle
 * @param failed  Header 写命令是否失败
 */
static void cp_hdr_write_complete(cp_handle_t *h, bool failed)
{
    h->cp_state.write_idx = ((h->hdr.idx + 1U) % md_ring_total_records(&h->hdr.ring));

    if (failed)
    {
        // 失败时不写 journal_truncate、不复位水位标志：脏数据仍由下个 checkpoint 重试。
        cp_job_abort(h);
        return;
    }
    h->cp_state.last_journal_sequence = h->journal_seq; /* T3 空转门 / 增量判据 */
    // Header 是 commit 页：此时快照的脏区都已落盘。若本次 CP 起点覆盖了
    // Journal 的持久性失败点，可清除 pending（见 jrn_write_failure_clear）。
    jrn_write_failure_clear(h->journal, h->journal_seq);

    // 元数据失块后的 full rebuild：**本次 CP 已把重建后的状态落盘**，才可以清
    // REBUILD_REQUIRED（本仓自拟）。掉电在 CP 之前 ⇒ 标志仍在 ⇒ 下次开机重建。
    if (h->alloc->bbt.rebuild_required)
    {
        h->alloc->bbt.rebuild_required = false;
        bbt_dirty_note(h->alloc);
        LOGI(LOG_TAG_CP "rebuild_required cleared — rebuilt metadata committed\r\n");
    }
    h->phase = CP_TRUNC_PREP;
}

/**
 * @brief Checkpoint 命令完成服务：按 phase 记结果并推进
 *
 * 本函数不推进状态机（PREP 里有同步原语，完成泵内会嵌套泵）；Header 池游标
 * 无论成败都推进（半编程槽不可复用）。
 *
 * @param ftl        完成派发解析出的实例（必须与 ctx 一致）
 * @param completion FC 完成状态
 * @param ctx        cp_handle_t *
 */
void ftl_checkpoint_callback(ftl_ctx_t *ftl, const fc_command_status_t *completion, void *ctx)
{
    // 形参由完成派发签名固定；release 下 ASSERT 裁空 ⇒ 显式丢弃（H30）。
    (void)ftl;
    cp_handle_t  *h   = (cp_handle_t *)ctx;
    fc_command_t *cmd = fc_command_id_to_command(completion->command_id);

    // 实例由 ctx 反推；ftl 形参是 dispatch 解析出的归属实例，
    // 二者必须一致——不一致说明派发路由错了。
    ASSERT(ftl->cp_handle == h);

    // 没有在飞命令却收到完成 ⇒ 重复/迟到完成。按当前 phase 再走一遍会误推进
    // chunk/游标（静默 CP 状态破坏），故 release 也必须拒绝（H14 判据 9）。
    ASSERT(h->waiting == true);

    if (h->waiting == false)
    {
        LOGE(LOG_TAG_CP "unexpected completion phase=%d (no command in flight)\r\n", h->phase);
        (void)fc_command_release(cmd);
        return;
    }

    const bool failed = fc_cmd_status_has_error(completion->status);

    // 每个 phase 的完成都以「release + 清在飞标志」开头 ⇒ 提到 switch 之前统一做。
    // cp_job_abort 只改 phase/failed，不碰 waiting ⇒ 与各分支内的原顺序等价。
    (void)fc_command_release(cmd);
    h->waiting = false;

    switch ((cp_phase_e)h->phase)
    {
        case CP_DIR_CKPT_ERASE:
            // 擦失败 ⇒ 中止（不推进游标，dirty 保留 ⇒ 下次在新槽重试整条）。
            if (failed)
            {
                cp_job_abort(h);
            }
            else
            {
                h->phase = CP_DIR_CKPT_WRITE;
            }
            return;

        case CP_DIR_CKPT_WRITE:
            // 无论成败都往后写下一页；失败 ⇒ 整条作废（不取半条）。
            if (failed)
            {
                h->dir.ok = false;
            }
            h->dir.chunk = (h->dir.chunk + 1U);
            return;

        case CP_HDR_ERASE:
            // 擦失败 ⇒ 中止（不推进游标，dirty 保留 ⇒ 下次在新槽重试整条）。
            if (failed)
            {
                cp_job_abort(h);
            }
            else
            {
                h->phase = CP_BLOCK_WRITE;
            }
            return;

        case CP_BLOCK_WRITE:
            if (failed == false)
            {
                h->hdr.block_page = (h->hdr.block_page + 1U);
            }
            else
            {
                // 整条 record 作废（不写 Header）；游标推进一槽——剩余页可能半编程，永不复用。
                h->hdr.block_ok       = false;
                h->cp_state.write_idx = ((h->hdr.idx + 1U) % md_ring_total_records(&h->hdr.ring));
                cp_job_abort(h);
            }
            return;

        case CP_WEAR_WRITE:
            if (failed == false)
            {
                h->hdr.wear_page = (h->hdr.wear_page + 1U);
            }
            else
            {
                // 同 CP_BLOCK_WRITE：作废整条 record 并推进游标。
                h->hdr.block_ok       = false;
                h->cp_state.write_idx = ((h->hdr.idx + 1U) % md_ring_total_records(&h->hdr.ring));
                cp_job_abort(h);
            }
            return;

        case CP_HDR_WRITE:
            cp_hdr_write_complete(h, failed);
            return;

        case CP_TRUNC_ERASE:
            if (failed)
            {
                // **擦成功才**记账：jrn_handle 回切到该块会跳过擦除 ⇒ 写进未擦块
                // （二次编程 = 静默损坏）。commit(NONE) 只复位水位标志，该块保持
                // "未预擦"，journal 回切时由 need_erase 重试。
                LOGW(LOG_TAG_CP "journal pre-erase failed blk=%hhu — 不记账，等待重试\r\n", h->trunc.next);
                jrn_truncate_commit(h->journal, JRN_BLOCK_NONE);
            }
            else
            {
                // commit 记账 + 复位水位标志（设计 §7.5.4：本次 CP 已把 seq ≤ 水位
                // 的条目落盘并截断 ⇒ 清零。不复位会让 T1 永久命中——每圈必 CP）。
                jrn_truncate_commit(h->journal, h->trunc.next);
            }
            h->phase = CP_DONE;
            return;

        case CP_IDLE:
        case CP_FLUSH_REGIONS:
        case CP_DIR_CKPT_PREP:
        case CP_HDR_PREP:
        case CP_TRUNC_PREP:
        case CP_DONE:
        default:
            // 不该发生：一条命令对应一个提交阶段（DIR_CKPT_ERASE / DIR_CKPT_WRITE /
            // HDR_ERASE / HDR_WRITE / TRUNC_ERASE）。
            LOGE(LOG_TAG_CP "unexpected completion phase=%d\r\n", h->phase);
            cp_job_abort(h);
            return;
    }
}

/**
 * @brief 状态机主体——一次只提交**一条**命令就返回；纯计算阶段连续推进到下一个提交阶段或终态。
 *
 * @param h Checkpoint handle。
 *
 * @note 阶段序列见 checkpoint.h 的 cp_phase_e。提交失败（背压）⇒ 原地返回，
 * 由 cp_handle 下轮重试同一阶段（幂等）；连续 FTL_SUBMIT_TIMEOUT_MS
 * 轮无进展 ⇒ 中止（failed=1，走 CP_DONE 且**不**写 Header / 不截断）。
 */
static void cp_job_step(cp_handle_t *h)
{
    for (;;)
    {
        if (h->waiting == true)
        {
            return; /* 有命令在飞：等完成服务函数 */
        }

        switch ((cp_phase_e)h->phase)
        {
            case CP_FLUSH_REGIONS:
            {
                const cp_flush_step_e r = cp_regions_flush(h);

                if (r == CP_FLUSH_FAILED)
                {
                    cp_job_abort(h);
                    break;
                }
                if (r == CP_FLUSH_PENDING)
                {
                    return; /* 仍有脏行在飞 */
                }
                h->phase = CP_DIR_CKPT_PREP;
                break;
            }

            case CP_DIR_CKPT_PREP:
            {
                // ③ 目录检查点：定位游标 + 取槽（游标未建立时会**同步**扫池定位——
                // 这是本 job 唯一可能瞬时阻塞的点，且只在冷启动后首次 CP 出现）。
                md_dir_ckpt_prepare_out_t dir_ckpt;
                md_dir_ckpt_step_e        r = md_dir_ckpt_prepare(h->metadata_dir, &dir_ckpt, MD_DIR_CKPT_OWNER_CP);

                if (r == MD_DIR_CKPT_FAILED)
                {
                    cp_job_abort(h);
                    break;
                }
                if (r == MD_DIR_CKPT_BUSY)
                {
                    // 另一个 writer（metadata job）正在写目录检查点：背压重试同一阶段
                    // （RISK-MD-06：不推进 sequence、不取槽）。
                    (void)cp_job_stall(h);
                    return;
                }
                if (r == MD_DIR_CKPT_CLEAN)
                {
                    // Directory 无变更：整段跳过（目录检查点每写一条都消耗池内槽位，
                    // 空转会让池无谓轮转）。
                    h->dir.skip = true;
                    h->phase    = CP_HDR_PREP;
                    break;
                }

                h->dir.ring       = dir_ckpt.ring;
                h->dir.base       = dir_ckpt.base;
                h->dir.idx        = dir_ckpt.idx;
                h->dir.seq        = dir_ckpt.seq;
                h->dir.need_erase = dir_ckpt.need_erase;
                h->dir.chunk      = 0U;
                h->dir.ok         = true;
                h->phase          = dir_ckpt.need_erase ? CP_DIR_CKPT_ERASE : CP_DIR_CKPT_WRITE;
                break;
            }

            case CP_DIR_CKPT_ERASE:
                (void)cp_command_result_handle(h, cp_erase_submit(h, h->dir.base));
                return;

            case CP_DIR_CKPT_WRITE:
            {
                if (h->dir.chunk >= DIR_CKPT_PAGE_COUNT)
                {
                    // 整条 record 写完：推进游标（无论成败），成功才清 dirty。
                    // 失败 ⇒ 整条残缺（locate 会整条丢弃）⇒ 中止，不写 Header。
                    const bool dir_ckpt_ok = (h->dir.ok == true);

                    md_dir_ckpt_commit(h->metadata_dir, &h->dir.ring, h->dir.idx, dir_ckpt_ok);

                    if (dir_ckpt_ok == false)
                    {
                        LOGW(LOG_TAG_META "dir_ckpt write incomplete: idx=%hu seq=%lu (chunk 写失败)\r\n", h->dir.idx, h->dir.seq);
                        cp_job_abort(h);
                        break;
                    }
                    // 本 job 只写了目录检查点；块元数据 / 磨损快照 / Header 还没写。
                    // Header 是整条 record 的 commit 页 ⇒ 必须继续走 HDR_PREP。
                    h->phase = CP_HDR_PREP;
                    break;
                }

                const DWORD         dir_ckpt_page = pba_to_page(h->dir.base) + h->dir.chunk;
                DWORD               aaddr         = pba_with_page(h->dir.base, dir_ckpt_page);
                md_dir_ckpt_page_t *page          = (md_dir_ckpt_page_t *)h->hdr.hdr_buf;

                md_dir_ckpt_page(h->metadata_dir, h->dir.seq, h->dir.chunk, page);

                const DWORD        page_size = sizeof(*page);
                const ftl_status_e st        = cp_write_submit(h, aaddr, page, page_size);

                (void)cp_command_result_handle(h, st);
                return;
            }

            case CP_HDR_PREP:
            {
                // ④ Header：定位写游标（游标未建立时才扫池——开机 cp_pool_find_latest
                // 已建立时零代价）。⚠ 这里可能同步读池（同 CP_DIR_CKPT_PREP）。
                cp_ring_init(h->alloc, &h->hdr.ring);

                if (h->cp_state.cursor_valid == false)
                {
                    md_ring_locate_out_t   loc;
                    checkpoint_probe_ctx_t probe_ctx;

                    probe_ctx.topo        = h->topo;
                    probe_ctx.ring        = h->hdr.ring;
                    probe_ctx.any_version = false;

                    if (md_ring_locate(&h->hdr.ring, cp_record_probe, &probe_ctx, &loc) != FTL_OK)
                    {
                        cp_job_abort(h);
                        break;
                    }
                    h->cp_state.sequence     = loc.sequence;
                    h->cp_state.write_idx    = loc.write_idx;
                    h->cp_state.cursor_valid = true;
                    // 刚定位到的游标同样可能指向掉电残留的半编程槽 ⇒ 首次写入也跳一槽。
                    h->cp_state.advance_next = true;
                }

                h->hdr.idx = h->cp_state.write_idx;

                bool need_erase = false;
                if (md_ring_prepare_addr(&h->hdr.ring, &h->topo, h->hdr.idx, &h->hdr.addr, &need_erase) != FTL_OK)
                {
                    cp_job_abort(h);
                    break;
                }

                // 本 session 首次 CP：跳过一槽。掉电可能留下「快照页已写、Header 未写」
                // 的半编程槽，locate 会把它当下一可写槽。跳过后 re-prepare：
                // · 同块内 ⇒ 下一槽的 21 页从未被写，安全；
                // · 跨块   ⇒ need_erase=1，先擦目标块，同样安全。
                // 只在 need_erase==0 时跳（块边界本来就会擦，不需要额外浪费）。
                if ((h->cp_state.advance_next == true) && (need_erase == 0))
                {
                    h->hdr.idx = ((h->hdr.idx + 1U) % md_ring_total_records(&h->hdr.ring));
                    if (md_ring_prepare_addr(&h->hdr.ring, &h->topo, h->hdr.idx, &h->hdr.addr, &need_erase) != FTL_OK)
                    {
                        cp_job_abort(h);
                        break;
                    }
                }
                h->cp_state.advance_next = false;

                // sequence 在最终槽位确定后取（池内已有序号时才能续上）。
                // LOGD 留作 seq=0 现场的取证：normal build 静默。
                LOGD(LOG_TAG_CP "hdr_prep seq_before=%lu idx=%hu\r\n", h->cp_state.sequence, h->hdr.idx);
                h->hdr.seq = ++h->cp_state.sequence;

                h->hdr.block_page = 0U;
                h->hdr.block_ok   = true;
                h->hdr.need_erase = need_erase;
                h->phase          = (need_erase != 0) ? CP_HDR_ERASE : CP_BLOCK_WRITE;
                break;
            }

            case CP_HDR_ERASE:
                (void)cp_command_result_handle(h, cp_erase_submit(h, h->hdr.addr));
                return;

            case CP_BLOCK_WRITE:
            {
                if (h->hdr.block_page >= CP_BLOCK_PAGES)
                {
                    if (h->hdr.block_ok == false)
                    {
                        // 半编程槽：游标推进一槽，永不复用本条 record 的剩余页。
                        h->cp_state.write_idx = ((h->hdr.idx + 1U) % md_ring_total_records(&h->hdr.ring));
                        cp_job_abort(h);
                        break;
                    }
                    h->hdr.wear_page = 0U;
                    h->phase         = CP_WEAR_WRITE;
                    break;
                }

                const BYTE         page_index = (1U + h->hdr.block_page);
                const WORD        *words      = &h->hdr.block_words[h->hdr.block_page * CP_BLOCK_WORDS_PER_PAGE];
                DWORD              a          = md_ring_addr(&h->hdr.ring, &h->topo, h->hdr.idx, page_index);
                const ftl_status_e st         = cp_write_submit(h, a, words, PAGE_MAIN_SIZE);

                (void)cp_command_result_handle(h, st);
                return;
            }

            case CP_WEAR_WRITE:
            {
                if (h->hdr.wear_page >= FTL_CHECKPOINT_WEAR_PAGES)
                {
                    if (h->hdr.block_ok == false)
                    {
                        h->cp_state.write_idx = ((h->hdr.idx + 1U) % md_ring_total_records(&h->hdr.ring));
                        cp_job_abort(h);
                        break;
                    }
                    h->phase = CP_HDR_WRITE;
                    break;
                }

                // 磨损快照在**写到哪页时组哪页**（软状态；CP 期间的计数变化允许漏到下一 CP）。
                cp_wear_page_build(h->alloc, h->hdr.wear_page, h->hdr.wear_page_buf);
                h->hdr.wear_page_crc[h->hdr.wear_page] = boot_crc32(h->hdr.wear_page_buf, PAGE_MAIN_SIZE);

                const BYTE wear_page_index = (1U + CP_BLOCK_PAGES + h->hdr.wear_page);
                DWORD      a               = md_ring_addr(&h->hdr.ring, &h->topo, h->hdr.idx, wear_page_index);

                const ftl_status_e st = cp_write_submit(h, a, h->hdr.wear_page_buf, PAGE_MAIN_SIZE);

                (void)cp_command_result_handle(h, st);
                return;
            }

            case CP_HDR_WRITE:
            {
                // 2KB Header 页常驻 handle，避免运行期 CP 路径的 2KB 栈帧。
                cp_header_t *cp = (cp_header_t *)h->hdr.hdr_buf;

                cp_header_fill(cp, h, h->hdr.seq, h->journal_seq, h->region_bitmap);
                cp->timestamp = platform_now_ms();

                const DWORD        cp_size = sizeof(*cp);
                const ftl_status_e st      = cp_write_submit(h, h->hdr.addr, cp, cp_size);

                (void)cp_command_result_handle(h, st);
                return;
            }

            case CP_TRUNC_PREP:
            {
                // ⑤ 截断 Journal：预擦环形最旧块（纯 RAM 判定 + 算址，不动介质）。
                // ⚠ 无论 plan 返回 JRN_TRUNC_ALREADY（已预擦过）还是
                // JRN_TRUNC_NO_TRUNCATE（水位不满足），本 CP 都已**完成**它承诺的事
                // （region 已落盘、目录检查点/Header 已写）⇒ 复位水位标志并收尾；
                // 是否真的擦了只影响磨损优化，不影响本次 CP 的正确性。
                DWORD addr;
                if (jrn_truncate_plan(h->journal, h->journal_seq, &addr, &h->trunc.next) != JRN_TRUNC_NEEDS_ERASE)
                {
                    jrn_truncate_commit(h->journal, JRN_BLOCK_NONE);
                    h->phase = CP_DONE;
                    break;
                }

                h->trunc.addr = addr;
                h->phase      = CP_TRUNC_ERASE;
                break;
            }

            case CP_TRUNC_ERASE:
                (void)cp_command_result_handle(h, cp_erase_submit(h, h->trunc.addr));
                return;

            case CP_DONE:
            {
                if (h->failed)
                {
                    LOGW_IF(cp_job_abort_log_due(h), LOG_TAG_CP "abort at phase=%d (不写 Header / 不截断，脏区与 journal 保留)\r\n", h->fail_phase);
                }
                else if (h->hdr.seq == 0U)
                {
                    // sequence 0 = 本 Header 在开机被忽略（commit 页无效）。
                    // 这是缺陷状态：限流后以 ERR 暴露，不再每 CP 刷一行。
                    LOGE_IF(cp_job_abort_log_due(h), LOG_TAG_CP "done with invalid seq=0 — header ignored at boot\r\n");
                }
                else
                {
                    // 正常完成：INFO 是「Header 真的落盘」的唯一直接证据（`info` 的 cp=
                    // 只反映 HDR_PREP 过的序号，不证明写成功）。
                    // 限流 5 s：水位触发（1024 次写）可能快于 5 s，逐条打印会在
                    // 115200 串口上刷屏（写满 4 KB TX 环后 uart_log_write 会等待）。
                    // 被跳过的 CP 不影响判据——最新序号看 `info` 的 cp= 或下一条 done。
                    LOGD_IF(log_ratelimit_due(&h->rl_done), LOG_TAG_CP "done seq=%lu jrn_seq=%lu dir_ckpt=%s\r\n", h->hdr.seq, h->journal_seq, (h->dir.skip == true) ? "skip" : "written");
                }

                h->active  = false;
                h->phase   = CP_IDLE;
                h->waiting = false;
                h->failed  = false;
                timer_deadline_clear(&h->submit_deadline);
                log_heartbeat_end(&h->hb);
                return;
            }

            case CP_IDLE:
            default:
                // 不该发生：active 置位而 phase 非法
                ASSERT(h->active == false);
                return;
        }
    }
}

/* ── job 状态机入口（模块内部）── */

/**
 * @brief 启动一次 Checkpoint job：复位执行状态、取 job 起点快照、立刻推进第一步
 *
 * @param h  Checkpoint handle
 * @return FTL_OK 已启动（可能停在某个提交阶段）；FTL_AGAIN 上一次 job 仍在跑（单飞行）
 */
static ftl_status_e cp_job_begin(cp_handle_t *h)
{
    if (h->active == true)
    {
        return FTL_AGAIN; /* 单飞行：上一次还没跑完 */
    }

    // 窄上下文与池游标跨 job 保留；其余执行状态整体复位。
    l2p_cache_t *const    cache        = h->cache;
    l2p_jrn_t *const      journal      = h->journal;
    md_dir_cache_t *const metadata_dir = h->metadata_dir;
    alc_t *const          alloc        = h->alloc;
    const ftl_topo_t      topo         = h->topo;
    const cp_state_t      cps          = h->cp_state;
    const log_ratelimit_t rl_abort     = h->rl_abort;
    const log_ratelimit_t rl_done      = h->rl_done;

    memset(h, 0, sizeof(*h));

    h->cache        = cache;
    h->journal      = journal;
    h->metadata_dir = metadata_dir;
    h->alloc        = alloc;
    h->topo         = topo;
    h->cp_state     = cps;
    h->rl_abort     = rl_abort;
    h->rl_done      = rl_done;

    // ① 起点快照（**必须早于任何 flush**，见文件头"快照点前移"的长注释）：
    // flush 成功会清 region 脏位 ⇒ 快照必须先行；journal 水位同理。
    // 共享 cache 的脏位图是全局的；本 job 只需要本 die 的局部 2048-bit 快照。
    const BYTE die = topo_2_die(h->topo);

    l2p_owner_bitmap_snapshot(h->cache, die, h->region_bitmap);
    h->journal_seq = jrn_sequence_get(h->journal);

    // 块元数据快照与 region_bitmap / journal_seq 在**同一个协作点**取定：
    // 单线程 superloop 里本函数不会被完成回调打断 ⇒ 三者是同一个 cut。
    // ⚠ 不要把 块元数据快照挪到 flush 之后：CP 期间完成的写会同时改 BST 与 journal，
    // 水位与快照会对不上。
    cp_block_snapshot(h->alloc, h->metadata_dir, h->hdr.block_words);

    // CP 之后的同 LBA append 不得合并进水位之前的条目——否则 sequence 还是旧的、
    // new_pba 却是新的，delta 回放会漏记（见 journal.c 的 merge_floor）。
    jrn_merge_floor_set(h->journal, h->journal_seq);

    h->active = true;
    h->phase  = CP_FLUSH_REGIONS;
    log_heartbeat_begin(&h->hb);

    // CP 是高频元数据维护（每次水位触发），start 不逐次打；当前序号看 info 的 cp=。
    cp_job_step(h); /* 立刻推进第一步（脏区落盘可当场起异步写回） */
    return FTL_OK;
}

/**
 * @brief 复位本实例的 CP 池游标/序号（找不到有效 record 时保持 0/无效）。
 *
 * @param cp  本实例的 cp_state_t（池游标/序号）
 */
static void cp_cursor_reset(cp_state_t *cp)
{
    cp->sequence              = 0U;
    cp->last_journal_sequence = 0U;
    cp->write_idx             = 0U;
    cp->cursor_valid          = false;
    cp->advance_next          = false;
    cp->last_cp_writes        = 0U;
}

/**
 * @brief 扫描 CP 池找最新有效 Header
 *
 * 进入先复位游标；locate 只返回序号，Header 本体按该序号再读一次并复核。
 *
 * @param h              Checkpoint handle
 * @param cp             输出：找到的 Header
 * @param out_record_idx 输出：该 Header 的池内槽位（可 NULL）
 * @return FTL_OK 找到；FTL_ERROR 无有效 CP / 重读失败
 */
ftl_status_e cp_pool_find_latest(cp_handle_t *h, cp_header_t *cp, WORD *out_record_idx)
{
    // 复位本实例池状态：找不到有效 Header 时保持 0/无效（后续 create 从池首开始）
    cp_cursor_reset(&h->cp_state);

    checkpoint_probe_ctx_t probe_ctx;
    cp_ring_init(h->alloc, &probe_ctx.ring);
    probe_ctx.topo        = h->topo;
    probe_ctx.any_version = false;

    md_ring_locate_out_t loc;

    if (md_ring_locate(&probe_ctx.ring, cp_record_probe, &probe_ctx, &loc) != FTL_OK)
    {
        return FTL_ERROR;
    }
    if (loc.newest_idx == UINT16_MAX)
    {
        LOGW(LOG_TAG_CP "no valid record in pool — boot will fall back to rebuild\r\n");
        cp_cursor_reset(&h->cp_state);
        return FTL_ERROR; /* 池内无有效 record */
    }

    h->cp_state.sequence     = loc.sequence;
    h->cp_state.write_idx    = loc.write_idx;
    h->cp_state.cursor_valid = true;

    // 重读胜者（locate 只返回序号，Header 本体要再读一次）。
    // 镜像池：逐个 plane 试；sequence 必须等于 locate 选中的 record sequence。
    bool found = false;
    for (BYTE pi = 0U; pi < md_ring_plane_count(probe_ctx.ring.plane_mask); pi++)
    {
        const BYTE plane = md_ring_plane_at(probe_ctx.ring.plane_mask, pi);
        DWORD      addr  = md_ring_addr_plane(&probe_ctx.ring, &probe_ctx.topo, loc.newest_idx, 0U, plane);

        if (ftl_sync_read_page(addr, cp, sizeof(*cp), FTL_PAGE_CFG_META()) != FTL_OK)
        {
            continue;
        }
        if ((cp_header_is_valid(cp) == false) || (cp->sequence != h->cp_state.sequence))
        {
            continue;
        }
        found = true;
        break;
    }
    if (found == false)
    {
        // 重读胜者失败：调用者会按“无 CP”走 fresh；把游标一并复位，避免留下
        // 一份未经验证的 sequence/write_idx 给后续 CP 使用。
        cp_cursor_reset(&h->cp_state);
        return FTL_ERROR;
    }

    h->cp_state.last_journal_sequence = cp->journal_sequence;
    // 本 session 首次 CP 先跳一槽：避开掉电残留的「快照已写 / Header 未写」半编程槽。
    h->cp_state.advance_next          = true;

    if (out_record_idx != NULL)
    {
        *out_record_idx = loc.newest_idx;
    }

    LOGI(LOG_TAG_CP "find latest seq=%lu idx=%hu jrn_seq=%lu\r\n", cp->sequence, loc.newest_idx, cp->journal_sequence);
    return FTL_OK;
}

/**
 * @brief 开机格式化判据：区分空片 / 版本不符 / 读失败
 *
 * 只读探测，不格式化；避免把一次瞬时读错误变成整片擦除。
 *
 * @param h                  Checkpoint handle
 * @param out_layout_version 输出：读到的 CP layout_version（可 NULL）
 * @return CP_MEDIA_CURRENT / NO_RECORD / VERSION_MISMATCH / IO_ERROR
 */
cp_media_status_e cp_media_status_probe(cp_handle_t *h, DWORD *out_layout_version)
{
    checkpoint_probe_ctx_t ctx;
    cp_ring_init(h->alloc, &ctx.ring);
    ctx.topo        = h->topo;
    ctx.any_version = true; /* 只校验 magic：版本不符也要能定位出来 */

    md_ring_locate_out_t loc;

    if (md_ring_locate(&ctx.ring, cp_record_probe, &ctx, &loc) != FTL_OK)
    {
        return CP_MEDIA_IO_ERROR; /* 池几何/配置错误：不格式化 */
    }

    if (loc.newest_idx != UINT16_MAX)
    {
        const BYTE plane_count = md_ring_plane_count(ctx.ring.plane_mask);

        for (BYTE pi = 0U; pi < plane_count; pi++)
        {
            const BYTE   plane = md_ring_plane_at(ctx.ring.plane_mask, pi);
            DWORD        addr  = md_ring_addr_plane(&ctx.ring, &ctx.topo, loc.newest_idx, 0U, plane);
            cp_header_t *hdr   = &s_probe_hdr; /* 文件级 scratch，避免 2KB 栈帧 */

            if (ftl_sync_read_page(addr, hdr, sizeof(*hdr), FTL_PAGE_CFG_META()) != FTL_OK)
            {
                continue;
            }
            if ((hdr->magic != CP_MAGIC) || (hdr->sequence == 0U))
            {
                continue;
            }

            if (out_layout_version != NULL)
            {
                *out_layout_version = hdr->layout_version;
            }
            return (hdr->layout_version == FTL_MEDIA_LAYOUT_VERSION) ? CP_MEDIA_CURRENT : CP_MEDIA_VERSION_MISMATCH;
        }

        return CP_MEDIA_IO_ERROR; /* locate 看到了 record，重读却读不通 */
    }

    // BBT 最新 record 带 FORMAT_IN_PROGRESS ⇒ 上次格式化在擦除中途掉电：
    // 继续格式化（BBT 坏块图已在擦用户块之前落盘）。
    if (alc_bbt_is_format_in_progress(h->alloc))
    {
        return CP_MEDIA_NO_RECORD;
    }

    // 没有可定位的 record：raw 读 CP 池 slot 0 的 page 0（两 plane）区分三种情形：
    // · 命令走不通        ⇒ IO_ERROR（总线/超时，不格式化）
    // · 读到已编程的页    ⇒ 有介质内容但没有有效 CP（损坏，不格式化）
    // · 读到擦除页        ⇒ 再看固定元数据区是否已写过：空片 ⇒ NO_RECORD；
    // 其它元数据已写 ⇒ IO_ERROR（CP 缺失，不自动擦片）
    // ⚠ 判空走 `ftl_page_blank_check`（解码路径：ECC 是否成码字 + 整条 CW 的 1 比特占比），
    //   不是「RAW 读数 FF」——RAW 读跳过 ECC，也没把 META/CRC/parity 交给 FTL（RISK-IF-15）。
    // ⚠ 不要只用 bbt_medium_has_ftl_data 判空：它对“空片”和“总线读死”都返回 false。
    bool cmd_ok      = false;
    bool any_written = false;

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        const DWORD addr  = md_ring_addr_plane(&ctx.ring, &ctx.topo, 0U, 0U, plane);
        bool        blank = false;

        if (ftl_page_blank_check(addr, &blank) != FTL_OK)
        {
            continue;
        }
        cmd_ok = true;

        if (blank == false)
        {
            any_written = true;
            break;
        }
    }

    if (cmd_ok == false)
    {
        return CP_MEDIA_IO_ERROR;
    }
    if (any_written)
    {
        return CP_MEDIA_IO_ERROR;
    }

    return bbt_medium_has_ftl_data(h->alloc) ? CP_MEDIA_IO_ERROR : CP_MEDIA_NO_RECORD;
}

/**
 * @brief 把 BST + OP/Spare 池归属编码成块元数据快照
 *
 * 与 region_bitmap / journal_seq 在同一协作点取定；动态 LOG 块以 Directory
 * 为准（RISK-BOOT-15）。
 *
 * @param alloc  allocator（BST 与池归属来源）
 * @param md_dir Directory（动态 LOG 块身份；可 NULL）
 * @param words  输出：FTL_BLOCK_TOTAL 个编码 word
 */
void cp_block_snapshot(const alc_t *alloc, const md_dir_cache_t *md_dir, WORD words[FTL_BLOCK_TOTAL])
{
    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        const bst_block_t *blk = &alloc->blocks[g];

        WORD valid = blk->valid_count;
        BYTE st    = (BYTE)ftl_block_state(blk);
        BYTE temp  = ftl_block_get_temp(blk);

        // Directory 是动态 LOG 块身份的唯一权威：它不属 allocator 池，合法编码是
        // LOG + no-pool。若 BST 因历史路径停在 NONE，照实编码会让下一次开机的
        // 第 1 遍校验判为非法并拒绝**整份**快照（RISK-BOOT-15）⇒ 每次冷启动全盘 rebuild。
        // BAD 是终态，不得被 LOG 覆盖。
        if ((md_dir != NULL) && (ftl_block_is_bad(blk) == false) && (md_live_count_get(md_dir, g) > 0U))
        {
            valid = 0U;
            st    = FTL_BLOCK_STATE_LOG;
            temp  = 0U;
        }

        words[g] = cp_block_word_pack(valid, st, temp, CP_BLOCK_POOL_NONE);
    }

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        for (WORD i = 0U; i < alc_op_get_count(alloc, p); i++)
        {
            const WORD g = alc_op_get_at(alloc, p, i);

            if (ftl_block_is_bad(&alloc->blocks[g]) == false)
            {
                words[g] = ((words[g] & ~(CP_BLOCK_WORD_POOL_MASK << CP_BLOCK_WORD_POOL_SHIFT)) | (CP_BLOCK_POOL_OP << CP_BLOCK_WORD_POOL_SHIFT));
            }
        }
        for (WORD i = 0U; i < alc_spare_get_count(alloc, p); i++)
        {
            const WORD g = alc_spare_get_at(alloc, p, i);

            if (ftl_block_is_bad(&alloc->blocks[g]) == false)
            {
                words[g] = ((words[g] & ~(CP_BLOCK_WORD_POOL_MASK << CP_BLOCK_WORD_POOL_SHIFT)) | (CP_BLOCK_POOL_SPARE << CP_BLOCK_WORD_POOL_SHIFT));
            }
        }
    }
}

/**
 * @brief 把第 page_index 页磨损快照从 BST 组进 page
 *
 * 条目按 gblock 稠密排列；rising_count / health 按位截断（软状态）。
 *
 * @param alloc      allocator（磨损计数来源）
 * @param page_index 磨损页号（0..FTL_CHECKPOINT_WEAR_PAGES-1）
 * @param page       输出：一页主区
 */
void cp_wear_page_build(const alc_t *alloc, WORD page_index, BYTE page[PAGE_MAIN_SIZE])
{
    cp_wear_entry_t *entries = (cp_wear_entry_t *)page;

    for (WORD i = 0U; i < FTL_CHECKPOINT_WEAR_ENTRIES_PER_PAGE; i++)
    {
        const DWORD g = (page_index * FTL_CHECKPOINT_WEAR_ENTRIES_PER_PAGE) + i;

        if (g < FTL_BLOCK_TOTAL)
        {
            bst_wear_t wear;

            alc_bst_get_wear(alloc, g, &wear);

            BYTE rising = wear.rr.rising_count;
            BYTE health = (BYTE)wear.rr.health;

            if (rising > CP_WEAR_RISING_MASK)
            {
                rising = CP_WEAR_RISING_MASK;
            }

            entries[i].erase_count = wear.erase_count;
            entries[i].read_count  = wear.read_count;
            entries[i].last_ecc    = wear.rr.last_ecc;
            entries[i].flags       = ((rising & CP_WEAR_RISING_MASK) | ((health & CP_WEAR_HEALTH_MASK) << CP_WEAR_HEALTH_SHIFT));
        }
        else
        {
            entries[i].erase_count = 0U;
            entries[i].read_count  = 0U;
            entries[i].last_ecc    = 0U;
            entries[i].flags       = 0U;
        }
    }
}

/**
 * @brief 读取并校验 CP 记录里的磨损快照
 *
 * 逐页 CRC + 逐页应用；失败只表示软状态不完整，不得触发格式化。
 *
 * @param h          Checkpoint handle
 * @param cp         最近 Header
 * @param record_idx 该 Header 的池内槽位
 * @return true 已加载并回填；false 无快照 / 损坏
 */
bool cp_wear_table_load(cp_handle_t *h, const cp_header_t *cp, WORD record_idx)
{
    // 读缓冲复用 cp_handle 的 wear_page_buf ⇒ 只能在开机（job 未在跑）调用。
    ASSERT(h->active == false);

    if (cp->wear_page_count == 0U)
    {
        return false; /* 旧记录 / v12 前空 initial CP：无磨损快照 */
    }
    if ((cp->wear_entry_count != FTL_BLOCK_TOTAL) || (cp->wear_entry_bytes != FTL_CHECKPOINT_WEAR_ENTRY_BYTES) || (cp->wear_page_count != FTL_CHECKPOINT_WEAR_PAGES) || (cp->record_pages != CP_RECORD_PAGES))
    {
        LOGW(LOG_TAG_WEAR "snapshot fields inconsistent (entries=%lu bytes=%lu pages=%lu rec=%lu)\r\n", cp->wear_entry_count, cp->wear_entry_bytes, cp->wear_page_count, cp->record_pages);
        return false;
    }

    md_ring_t ring;
    cp_ring_init(h->alloc, &ring);

    // 开机路径专用：CP job 未在跑（入口 ASSERT），可复用它的页缓冲作读缓冲。
    BYTE *page = h->hdr.wear_page_buf;

    for (BYTE p = 0U; p < FTL_CHECKPOINT_WEAR_PAGES; p++)
    {
        bool loaded = false;

        for (BYTE pi = 0U; pi < md_ring_plane_count(ring.plane_mask); pi++)
        {
            const BYTE plane      = md_ring_plane_at(ring.plane_mask, pi);
            const BYTE page_index = (1U + CP_BLOCK_PAGES + p);
            DWORD      addr       = md_ring_addr_plane(&ring, &h->topo, record_idx, page_index, plane);

            if (ftl_sync_read_page(addr, page, PAGE_MAIN_SIZE, FTL_PAGE_CFG_META()) != FTL_OK)
            {
                continue;
            }
            if (boot_crc32(page, PAGE_MAIN_SIZE) != cp->wear_page_crc[p])
            {
                LOGW(LOG_TAG_WEAR "page %hhu crc mismatch (plane=%hhu rec=%hu)\r\n", p, plane, record_idx);
                continue;
            }

            const cp_wear_entry_t *entries = (const cp_wear_entry_t *)page;

            for (WORD i = 0U; i < FTL_CHECKPOINT_WEAR_ENTRIES_PER_PAGE; i++)
            {
                const DWORD g = (p * FTL_CHECKPOINT_WEAR_ENTRIES_PER_PAGE) + i;

                if (g >= FTL_BLOCK_TOTAL)
                {
                    break;
                }

                bst_wear_t wear;

                wear.erase_count     = entries[i].erase_count;
                wear.read_count      = entries[i].read_count;
                wear.rr.last_ecc     = entries[i].last_ecc;
                wear.rr.rising_count = (BYTE)(entries[i].flags & CP_WEAR_RISING_MASK);
                wear.rr.health       = (rr_block_health_e)((entries[i].flags >> CP_WEAR_HEALTH_SHIFT) & CP_WEAR_HEALTH_MASK);

                alc_bst_set_wear(h->alloc, g, &wear);
            }

            loaded = true;
            break;
        }

        if (loaded == false)
        {
            // 逐页应用：前面的页已回填 BST，其余块保持默认计数（软状态，可接受）。
            LOGW(LOG_TAG_WEAR "snapshot page %hhu unavailable (rec=%hu) — 已读页保留，其余从 0\r\n", p, record_idx);
            return false;
        }
    }

    return true;
}

/**
 * @brief 读取并校验 CP 记录里的块元数据快照
 *
 * block_page_count==0 是旧记录 / v12 前空 initial CP 的「无快照」标记（当前
 * initial CP 含完整快照）；快照损坏不得格式化，也不得回退旧 CP。
 *
 * @param h           Checkpoint handle
 * @param cp          最近 Header
 * @param record_idx  该 Header 的池内槽位
 * @param block_words 输出：FTL_BLOCK_TOTAL 个 word
 * @return true 已加载且 CRC 通过；false 无快照 / 损坏
 */
bool cp_block_table_load(cp_handle_t *h, const cp_header_t *cp, WORD record_idx, WORD block_words[FTL_BLOCK_TOTAL])
{
    if (cp->block_page_count == 0U)
    {
        return false; /* 旧记录 / v12 前空 initial CP：无快照 */
    }
    if ((cp->block_word_count != FTL_BLOCK_TOTAL) || (cp->block_word_bytes != CP_BLOCK_WORD_BYTES) || (cp->block_page_count != CP_BLOCK_PAGES) || (cp->record_pages != CP_RECORD_PAGES))
    {
        LOGW(LOG_TAG_BLK "header fields inconsistent (words=%lu bytes=%lu pages=%lu rec=%lu)\r\n", cp->block_word_count, cp->block_word_bytes, cp->block_page_count, cp->record_pages);
        return false;
    }

    md_ring_t ring;
    cp_ring_init(h->alloc, &ring);

    // 镜像池：整条快照从**同一个 plane** 读（不在 plane 间拼页）；该副本读失败或
    // CRC 不符时换另一个 plane 重读整条。
    for (BYTE pi = 0U; pi < md_ring_plane_count(ring.plane_mask); pi++)
    {
        const BYTE plane = md_ring_plane_at(ring.plane_mask, pi);

        BYTE fail_page = 0U;
        bool ok        = true;

        for (BYTE p = 0U; p < CP_BLOCK_PAGES; p++)
        {
            const BYTE  page_index  = (1U + p);
            const DWORD word_offset = p * CP_BLOCK_WORDS_PER_PAGE;
            DWORD       addr        = md_ring_addr_plane(&ring, &h->topo, record_idx, page_index, plane);

            if (ftl_sync_read_page(addr, &block_words[word_offset], PAGE_MAIN_SIZE, FTL_PAGE_CFG_META()) != FTL_OK)
            {
                fail_page = p;
                ok        = false;
                break;
            }
        }

        if (ok == false)
        {
            LOGW(LOG_TAG_BLK "read failed plane=%hhu page=%hhu rec=%hu\r\n", plane, fail_page, record_idx);
            continue;
        }

        if (boot_crc32(block_words, sizeof(WORD) * FTL_BLOCK_TOTAL) != cp->block_crc32)
        {
            LOGW(LOG_TAG_BLK "crc mismatch plane=%hhu rec=%hu seq=%lu\r\n", plane, record_idx, cp->sequence);
            continue;
        }

        LOGI(LOG_TAG_BLK "load plane=%hhu rec=%hu seq=%lu pages=%u crc=ok\r\n", plane, record_idx, cp->sequence, CP_BLOCK_PAGES);
        return true;
    }

    return false;
}

/**
 * @brief 复位 checkpoint handle 并绑定窄上下文
 *
 * @param h            handle
 * @param cache        L2P cache
 * @param journal      Journal
 * @param metadata_dir Directory
 * @param alloc        allocator（同时作为本实例拓扑来源）
 */
void cp_init(cp_handle_t *h, l2p_cache_t *cache, l2p_jrn_t *journal, md_dir_cache_t *metadata_dir, alc_t *alloc)
{
    ASSERT(h != NULL);
    ASSERT(cache != NULL);
    ASSERT(journal != NULL);
    ASSERT(metadata_dir != NULL);
    ASSERT(alloc != NULL);

    memset(h, 0, sizeof(*h));
    h->cache        = cache;
    h->journal      = journal;
    h->metadata_dir = metadata_dir;
    h->alloc        = alloc;
    h->topo         = alc_topo(alloc);
    log_ratelimit_init(&h->rl_abort, LOG_RATE_SLOW_INTERVAL_MS);
    log_ratelimit_init(&h->rl_done, LOG_RATE_SLOW_INTERVAL_MS);
    log_heartbeat_end(&h->hb);
    h->cp_state.next_t3_ms = platform_now_ms() + CP_TIME_INTERVAL_MS;
}

/**
 * @brief 写一条 initial CP record 并做回读校验（Header + 块元数据页）。
 * 返回 true 仅当 Header 与 page1 都真的落到介质上。
 *
 * @param ftl    FTL 实例（取 cp_handle 的读缓冲）
 * @param ring   Checkpoint 环
 * @param slot   待写入记录在环内的起始槽位
 * @param words  块元数据快照（FTL_BLOCK_TOTAL 个 word）
 * @param cp     已填充的 Header（commit 页，最后写）
 * @return true = Header 与快照页均已落盘并通过回读校验；false = 任一步写/回读失败
 */
static bool cp_initial_record_write_checked(ftl_ctx_t *ftl, const md_ring_t *ring, WORD slot, const WORD *words, const cp_header_t *cp)
{
    cp_handle_t *h = ftl->cp_handle;

    for (BYTE p = 1U; p <= CP_BLOCK_PAGES; p++)
    {
        const DWORD addr = md_ring_addr(ring, &ftl->topo, slot, p);
        const BYTE *data = (const BYTE *)(words + ((p - 1U) * CP_BLOCK_WORDS_PER_PAGE));

        if (ftl_sync_write_page_mirror(addr, data, PAGE_MAIN_SIZE, FTL_PAGE_CFG_META()) != FTL_OK)
        {
            LOGW(LOG_TAG_CP "init: snapshot page=%hhu write failed (addr=%08lx)\r\n", p, addr);
            return false;
        }
    }

    BYTE *wear_page = h->hdr.wear_page_buf;

    for (BYTE i = 0U; i < FTL_CHECKPOINT_WEAR_PAGES; i++)
    {
        cp_wear_page_build(&ftl->alloc, i, wear_page);

        const DWORD addr = md_ring_addr(ring, &ftl->topo, slot, (1U + CP_BLOCK_PAGES + i));

        if (ftl_sync_write_page_mirror(addr, wear_page, PAGE_MAIN_SIZE, FTL_PAGE_CFG_META()) != FTL_OK)
        {
            return false;
        }
    }

    // 先确认快照页已落盘，再写 Header（commit）；否则“Header 有效但快照空白”
    // 会让 format 的 readback 通过、开机却拿不到块表。
    const DWORD  page1 = md_ring_addr(ring, &ftl->topo, slot, 1U);
    BYTE        *raw   = s_probe_raw;
    ftl_status_e r1    = ftl_sync_read_page(page1, raw, PAGE_MAIN_SIZE, FTL_PAGE_CFG_META());

    if ((r1 != FTL_OK) || ((raw[0] == 0xFFU) && (raw[1] == 0xFFU) && (raw[2] == 0xFFU) && (raw[3] == 0xFFU)))
    {
        LOGW(LOG_TAG_CP "init: snapshot page1 readback %s (addr=%08lx r=%d)\r\n", (r1 != FTL_OK) ? "failed" : "blank", page1, (int)r1);
        return false;
    }

    const DWORD hdr_addr = md_ring_addr(ring, &ftl->topo, slot, 0U);

    if (ftl_sync_write_page_mirror(hdr_addr, cp, sizeof(*cp), FTL_PAGE_CFG_META()) != FTL_OK)
    {
        LOGW(LOG_TAG_CP "init: header write failed (addr=%08lx)\r\n", hdr_addr);
        return false;
    }

    cp_header_t *rb  = &s_probe_hdr;
    ftl_status_e rst = ftl_sync_read_page(hdr_addr, rb, sizeof(*rb), FTL_PAGE_CFG_META());

    return (rst == FTL_OK) && (rb->magic == CP_MAGIC) && (rb->sequence == cp->sequence) && (rb->layout_version == FTL_MEDIA_LAYOUT_VERSION);
}

/**
 * @brief 格式化后写一条完整的 initial Checkpoint
 *
 * 先写块元数据/磨损快照，最后写 Header（commit）；sequence 从 1 起，使下次
 * 开机的版本判据立即命中，自动格式化只发生一次。
 *
 * @param ftl FTL 实例
 * @return FTL_OK；FTL_ERROR 任一分页写失败（不写 Header，无有效 record）
 */
ftl_status_e cp_initial_record_write(ftl_ctx_t *ftl)
{
    cp_handle_t *h     = ftl->cp_handle;
    WORD        *words = cp_block_words_get(h);

    // 完整 per-block metadata snapshot：与 carve 后的池归属一致（由调用者先 carve）。
    cp_block_snapshot(&ftl->alloc, ftl->metadata_dir, words);

    md_ring_t ring;
    cp_ring_init(h->alloc, &ring);

    // 磨损快照与 CRC 先组好；每个候选块的 wear 内容相同。
    BYTE *wear_page = h->hdr.wear_page_buf;

    for (BYTE i = 0U; i < FTL_CHECKPOINT_WEAR_PAGES; i++)
    {
        cp_wear_page_build(&ftl->alloc, i, wear_page);
        h->hdr.wear_page_crc[i] = boot_crc32(wear_page, PAGE_MAIN_SIZE);
    }

    cp_header_t *cp = (cp_header_t *)h->hdr.hdr_buf;

    cp_header_fill(cp, h, 1U, 0U, NULL);

    // 逐个 CP 候选块尝试：写点/坏块/单 plane 不可编程时不能只押第一块。
    // 只有全部失败才报 format 失败；失败块在找到可用块后标 BAD，避免后续 CP 再选它。
    const WORD rpb = md_ring_records_per_block(&ring);
    WORD       failed_blocks[CHECKPOINT_POOL_BLOCKS];
    BYTE       failed_count = 0U;

    for (WORD bi = 0U; bi < ring.block_count; bi++)
    {
        const WORD slot = (WORD)(bi * rpb);
        DWORD      base;

        if (md_ring_prepare_write(&ring, &ftl->topo, slot, &base) != FTL_OK)
        {
            LOGW(LOG_TAG_CP "init candidate block=%hu unusable (prepare failed)\r\n", ring.blocks[bi]);
            if (failed_count < CHECKPOINT_POOL_BLOCKS)
            {
                failed_blocks[failed_count++] = ring.blocks[bi];
            }
            continue;
        }

        if (cp_initial_record_write_checked(ftl, &ring, slot, words, cp))
        {
            LOGI(LOG_TAG_CP "init candidate block=%hu slot=%hu ok (base=%08lx)\r\n", ring.blocks[bi], slot, base);

            for (BYTE fi = 0U; fi < failed_count; fi++)
            {
                const WORD bad_block = failed_blocks[fi];

                LOGW(LOG_TAG_CP "retire CP candidate block=%hu (write/readback failed)\r\n", bad_block);

                // CP 池是 P0/P1 镜像：任一 plane 的副本写坏，同号块在各 plane 都要退役。
                // H39：不写死 plane 下标。
                for (BYTE p = 0U; p < PLANE_NUM; p++)
                {
                    bbt_mark_bad(&ftl->alloc, ftl_gblock(p, bad_block));
                }
            }
            return FTL_OK;
        }

        LOGW(LOG_TAG_CP "init candidate block=%hu slot=%hu write/readback failed, try next\r\n", ring.blocks[bi], slot);
        if (failed_count < CHECKPOINT_POOL_BLOCKS)
        {
            failed_blocks[failed_count++] = ring.blocks[bi];
        }
    }

    LOGE(LOG_TAG_CP "init failed: all %hu CP candidate blocks unusable\r\n", ring.block_count);
    return FTL_ERROR;
}

/* ── 触发 / 水位 ── */

/**
 * @brief 判定是否该做 Checkpoint（三路触发，任一命中即 true）。
 *
 * @param h checkpoint handle
 * @param trigger 本轮触发信息。
 *
 * @return true 触发；false 不触发。
 *
 * @note T1 Journal 水位（checkpoint_needed，由 jrn_watermark_update 置）；
 * T2 写入量（自上次 CP 起 total_writes 增量 ≥ CP_WRITE_INTERVAL）；
 * T3 定时（elapsed_ms ≥ CP_TIME_INTERVAL_MS）。
 */
static bool cp_should_start(const cp_handle_t *h, const cp_trigger_t *trigger)
{
    // 触发原因决定本轮**允许**评估哪些带门。
    // ADVANCE   —— must-run 表首只推进在飞 job，不评估启动；
    // EMERGENCY —— 任何档位只响应紧急水位；
    // EXPLICIT  —— force 立即启动；
    // 其余      —— 走 T1/T2/T3。
    switch ((cp_reason_e)trigger->reason)
    {
        case CP_REASON_ADVANCE:
            return false;
        case CP_REASON_EMERGENCY:
            return trigger->emergency;
        case CP_REASON_EXPLICIT:
            return trigger->force;
        default:
            break;
    }

    if (trigger->force || trigger->emergency)
    {
        return true;
    }

    // T1: Journal 水位线
    if (jrn_needs_checkpoint(h->journal))
    {
        return true;
    }

    // T2: 写入量触发（自上次 CP 起点以来的增量）。
    // ⚠ 不能用 `total_writes % CP_WRITE_INTERVAL == 0`：total_writes 只在 host 写
    // 确认时自增，空闲时不变；若写计数恰好停在整数倍，CP 完成后每轮都会再次命中
    // ⇒ 空闲持续空转写 21 页 record。增量判据在无符号减法下天然处理回绕。
    if ((trigger->total_writes - h->cp_state.last_cp_writes) >= CP_WRITE_INTERVAL)
    {
        return true;
    }

    // T3: 定时触发（绝对到期时刻，由 CP 起点重装）。
    // ⚠ 一条 CP record 是 21 页 ⇒ 轻载下加最小 journal 增量门：定时拍到了但
    // 自上次 CP 没有足够新条目 ⇒ 不写。
    if ((trigger->now_ms != 0U) && timer_time_due(trigger->now_ms, h->cp_state.next_t3_ms))
    {
        const DWORD seq  = trigger->journal_seq;
        const DWORD last = h->cp_state.last_journal_sequence;

        if ((seq >= last) && ((seq - last) >= CP_T3_MIN_JOURNAL_DELTA))
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 推进 Checkpoint：在飞则续跑，未在飞则按 trigger 判定是否启动
 *
 * 在飞时只有 CP_REASON_ADVANCE 推进状态机（一轮只推进一次）；未在飞时按
 * emergency / force / T1 / T2 / T3 判定。
 *
 * @param h       handle
 * @param trigger 本轮触发信息（非空）
 */
void cp_handle(cp_handle_t *h, const cp_trigger_t *trigger)
{
    if (h->active == true)
    {
        // 每轮**只推进一次**：只有表首的 ADVANCE 入口跑状态机；紧急/周期入口在
        // job 在飞时只上报、不再推进（否则一轮会被推进 3 次，行为漂移）。
        if (trigger->reason == CP_REASON_ADVANCE)
        {
            // 心跳（见 log.h / timer.h）：正常 CP 远快于 1s ⇒ 平时静默；只有卡在某 phase
            // 才持续可见。判据 = 两次心跳之间 phase 不动 ⇒ 卡死。
            // ⚠ 起点在 cp_job_begin 显式记（CP 的"job 起点"是快照时刻，不是首次心跳时刻）。
            if (log_heartbeat_due(&h->hb))
            {
                LOGI(LOG_TAG_CP "job phase=%d wait=%d fail=%d age=%lums\r\n", h->phase, h->waiting, h->failed, log_heartbeat_age_ms(&h->hb));
            }

            cp_job_step(h);
        }
    }
    else if (cp_should_start(h, trigger))
    {
        if (cp_job_begin(h) == FTL_OK)
        {
            // T2 的增量基准 = 本次 job 起点快照对应的写计数（与 region / journal
            // 快照同一 cut）。失败不更新，下一轮仍可再触发。
            h->cp_state.last_cp_writes = trigger->total_writes;
        }

        // 定时/水位触发后重装 T3 到期时刻（紧急触发不重装——它不代表周期拍点）。
        // 无论 cp_job_begin 成败都重装，否则一次失败会每轮重触发。
        if (trigger->reason != CP_REASON_EMERGENCY)
        {
            h->cp_state.last_cp_ms = trigger->now_ms;
            h->cp_state.next_t3_ms = trigger->now_ms + CP_TIME_INTERVAL_MS;
        }
    }
}

/**
 * @brief 重装 T3 到期时刻（boot 恢复 / 格式化后调用）
 *
 * @param h Checkpoint handle
 */
void cp_timer_reset(cp_handle_t *h)
{
    h->cp_state.next_t3_ms = platform_now_ms() + CP_TIME_INTERVAL_MS;
}

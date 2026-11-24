/*
 * power_failure_recovery.c — 掉电恢复（scan → plan → execute）
 *
 * 职责：
 *   开机时判定上次关机形态并据此恢复 RAM 状态——扫 Checkpoint 取最近快照，
 *   扫 Journal 取最后 sequence，二者比较决定走哪条路径；以及掉电重建
 *   Closed SB 列表（full metadata rebuild，全 NAND 扫描）与写指针扫描。
 *
 * 与 checkpoint.c / journal.c 的分工：
 *   checkpoint.c —— Checkpoint 的写与查找（cp_pool_find_latest）；
 *   journal.c    —— Journal 的写与回放（jrn_replay）；
 *   本文件       —— 只做**编排**：扫描、判型、按计划执行，自身不解析介质格式。
 *
 * 启动计划（pfr_boot_scan → pfr_boot_plan → pfr_boot_execute）：
 *   PFR_PLAN_FRESH         无有效 Checkpoint（出厂首次启动）
 *                          → cache INVALID + full metadata rebuild → carve
 *   PFR_PLAN_SNAPSHOT      journal 全被 CP 覆盖（干净关机）
 *                          → 快照 restore + probe + build_pools；失败 ⇒ 重建 → carve
 *   PFR_PLAN_REPLAY        journal 有未回放条目（异常掉电）
 *                          → 快照 restore + jrn_replay + F4 + probe + build_pools；
 *                            失败 ⇒ reset → full metadata rebuild → carve
 *   PFR_PLAN_FORCE_REBUILD 运行期元数据日志丢失（REBUILD_REQUIRED）
 *                          → 不尝试快照，reset → rebuild → carve
 *   ★ full metadata rebuild 只在「无快照 / 快照损坏 / 增量记账不一致」时兜底。
 *
 * 文件结构：
 *   1. 介质版本 / 格式化   pfr_media_check_format / pfr_format_write_initial_dir_ckpt /
 *                          pfr_format_reset_bad_map / pfr_format_verify_blocks /
 *                          pfr_format_end_bbt / pfr_media_format
 *   2. 启动编排            pfr_boot_recovery（入口）/ pfr_boot_scan / pfr_boot_plan /
 *                          pfr_boot_execute / pfr_boot_carve_pools /
 *                          pfr_refill_spare_pools / pfr_boot_log_bad_areas
 *   3. 块账本阶段          pfr_ledger_snapshot（restore → replay → bitmap → probe）/
 *                          pfr_ledger_rebuild / pfr_ledger_finish
 *   4. full metadata rebuild   pfr_block_is_fixed / pfr_closed_sb_add（共享）/
 *                          pfr_scan_write_pointer / pfr_close_block /
 *                          pfr_rebuild_closed_sb_list / pfr_stats_report
 *   5. 块元数据快照恢复    pfr_restore_blocks / pfr_probe_blocks /
 *                          pfr_build_pools / pfr_reset_blocks
 *   6. journal replay 端口 pfr_materialize_pba / pfr_valid_add / _sub
 *
 * ⚠ 本文件依赖 ftl_init 已完成 allocator / journal / metadata_dir 的初始化；
 *   步骤用圈码 ①②③ 编号（与 docs/design/ftl_design.md §3.1 对应）。
 *
 * ⚠ 入口前置守卫口径（docs/design/common/assertion_guidelines.md §九）：
 *   对外入口（pfr_media_check_format / pfr_media_format / pfr_boot_recovery）的 ftl/result
 *   非空由 main.c 的调用链保证；启动计划与其内部 helper 只在本文件的启动恢复调用链内
 *   使用，该处的 ftl 非空已由入口保证 ⇒ **不再逐函数复检**。
 */

#include "power_failure_recovery.h"

#include "assertion.h"
#include "bbt.h"
#include "checkpoint.h"
#include "fc_interface.h"
#include "ftl.h"
#include "ftl_fc_interface.h"
#include "gc.h"
#include "log.h"
#include "metadata.h"

#include <string.h>

/* ── 文件级常量 ── */

// page0 空白时补探的页数 = cache program 批次上限（4 页）。
// 超出该范围的 page0 空洞依赖 page0 异步写屏障（allocator）保证。
#define RECOVERY_PAGE0_PROBE_PAGES (4U)

/* ── 启动恢复的类型 ── */

/* 恢复计划：pfr_boot_plan() 是唯一决策点，执行层只按此枚举分派。 */
typedef enum PFR_PLAN_E
{
    PFR_PLAN_FRESH = 0,     /* 无 CP：rebuild → carve */
    PFR_PLAN_SNAPSHOT,      /* clean：快照 → probe → build_pools */
    PFR_PLAN_REPLAY,        /* power failure：快照 → replay → probe → build_pools */
    PFR_PLAN_FORCE_REBUILD, /* REBUILD_REQUIRED：reset → rebuild → carve */
} pfr_plan_e;

/* 一次启动的只读扫描结果。cp 指向 cp_handle 的常驻缓冲，不复制。 */
typedef struct PFR_BOOT_SCAN_S
{
    const cp_header_s *cp;
    WORD               cp_idx;
    jrn_page_ref_s     ring;
    BOOL               has_cp;
    BOOL               rebuild_required;
    BOOL               ring_read_error;
    BOOL               blocks_loaded;
} pfr_boot_scan_s;

/* 块元数据表：显式携带「对账到哪一步」，顺序由阶段函数固定。 */
typedef struct PFR_BLOCK_TABLE_S
{
    WORD *word; /* 指向 cp_handle->hdr.block_words（FTL_BLOCK_TOTAL 项） */
    BOOL  loaded;
    BOOL  restored;
    BOOL  probed;
} pfr_block_table_s;

/* 本次启动的唯一统计结构。计数一律 DWORD：全块扫描的理论上限 > 64K。 */
typedef struct PFR_STATISTICS_S
{
    DWORD closed;          /* Closed 条目数（restore/probe/rebuild 合计） */
    DWORD partial;         /* rebuild 的部分写块数 */
    DWORD log_blocks;      /* 恢复出的活 LOG 块数 */
    DWORD anomalies;       /* 池块却有数据的异常块数 */
    DWORD l2p_read_errors; /* L2P 反查真读失败次数（非 0 ⇒ 快照不可信） */
    DWORD l2p_degraded_cw; /* 命中降级 region 的 CW 数（只丢映射） */
    DWORD first_degraded_region;
    DWORD valid_cw; /* 回填的有效 CW 总数 */
    DWORD sub_zero; /* replay 中 valid_sub 减到 0 的次数 */
    WORD  sub_zero_g;
    BYTE  sub_zero_state;
} pfr_stats_s;

/* journal replay 回调上下文。 */
typedef struct PFR_REPLAY_CONTEXT_S
{
    ftl_ctx_s   *ftl;
    pfr_stats_s *stats;
} pfr_replay_ctx_s;

/* 快照阶段的失败现场（只用于 fallback 日志，不参与控制流）。 */
typedef struct PFR_LEDGER_RESULT_S
{
    BOOL  replay_ok;
    BOOL  ctx_ok;
    BYTE  fail_reason;
    DWORD fail_pba;
} pfr_ledger_result_s;

/* ── 前置声明（按调用关系） ── */

static void pfr_boot_carve_pools(ftl_ctx_s *ftl);

static void pfr_refill_spare_pools(ftl_ctx_s *ftl);

static void pfr_boot_apply_region_bitmap(ftl_ctx_s *ftl, const cp_header_s *cp, pfr_stats_s *stats);

static void pfr_ledger_rebuild(ftl_ctx_s *ftl, BOOL reset_first, pfr_stats_s *stats);

static BOOL pfr_ledger_snapshot(ftl_ctx_s *ftl, const pfr_boot_scan_s *scan, pfr_plan_e plan, pfr_block_table_s *tbl, pfr_stats_s *stats, pfr_ledger_result_s *out, DWORD *out_entries);

static void pfr_ledger_finish(ftl_ctx_s *ftl, const jrn_page_ref_s *ring, DWORD floor_seq);

static BOOL pfr_restore_blocks(ftl_ctx_s *ftl, pfr_block_table_s *tbl);

static void pfr_probe_blocks(ftl_ctx_s *ftl, pfr_block_table_s *tbl, pfr_stats_s *stats);

static BOOL pfr_build_pools(ftl_ctx_s *ftl, const pfr_block_table_s *tbl);

static void pfr_reset_blocks(ftl_ctx_s *ftl);

static void pfr_rebuild_closed_sb_list(ftl_ctx_s *ftl, pfr_stats_s *stats);

static void pfr_stats_report(const pfr_stats_s *stats);

static BOOL pfr_materialize_pba(void *ctx, DWORD pba);

static void pfr_valid_add(void *ctx, DWORD pba, BOOL *ok);

static void pfr_valid_sub(void *ctx, DWORD pba, BOOL *ok);

/**
 * @brief 结束「格式化进行中」，把提交状态落盘
 *
 * 格式化是跨多次 NAND 写的长事务：擦除 →（可选）全片体检 → 初始目录检查点 →
 * initial Checkpoint。FORMAT_IN_PROGRESS 从 `bbt_format_erase` 起点保持到
 * **initial CP 读回成功**；中途复位/掉电 ⇒ 下次开机按「继续格式化」处理
 * （cp_media_status_probe 返回 NO_RECORD），而不是把「有 BBT、无 CP」
 * 判成损坏介质而拒绝启动。
 *
 * 显式失败路径**不**调用本函数：标志保持置位，下次开机重试格式化。
 *
 * @param alloc 分配器（BBT 记录属于它）
 *
 * @return false = BBT 落盘失败 —— 持久化记录仍是「格式化中」，下次开机会重新格式化（安全方向：宁可重擦，也不在半写元数据上启动）。
 */
static BOOL pfr_format_end_bbt(alc_s *alloc)
{
    alloc->bbt.format_in_progress = false;

    // 格式化刚写完全新的 initial CP ⇒ 没有任何「丢失映射需要重建」的旧状态。
    // verify 期间标记 Journal/L2P 坏块可能置过 REBUILD_REQUIRED，这里一并清掉，
    // 否则下次开机会在刚格式化完的介质上白跑一次 full rebuild。
    alloc->bbt.rebuild_required = false;

    bbt_dirty_note(alloc);

    return bbt_save(alloc) == FTL_OK;
}

/**
 * @brief 初始化本次启动的统计
 *
 * @param stats 统计
 */
static void pfr_stats_init(pfr_stats_s *stats)
{
    memset(stats, 0, sizeof(*stats));
    stats->first_degraded_region = UINT32_MAX;
}

/**
 * @brief 开机池建立后：把 CP 快照 BAD 并集 / full metadata rebuild 标坏造成的 Spare 缺口，从 Free Pool 补回。
 *
 * @param ftl FTL 实例
 *
 * @note Free 不足时自然停在低水位，并打一行告警（容量下降是真实结果）。
 */
static void pfr_refill_spare_pools(ftl_ctx_s *ftl)
{
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        (void)alc_spare_refill_from_free(&ftl->alloc, p);

        const WORD n = alc_spare_get_count(&ftl->alloc, p);

        LOGW_IF(n < FTL_SPARE_POOL_PER_PLANE, LOG_TAG_BLK "spare plane%hhu=%hu/%u (Free pool 不足)\r\n", p, n, FTL_SPARE_POOL_PER_PLANE);
    }
}

/**
 * @brief 块账本重建阶段：可选 reset 后跑 full metadata rebuild
 *
 * @param ftl         实例
 * @param reset_first true = 先清空块状态与池账本（保留 BAD）；fresh 路径依赖
 *                    alc_init 的 NONE 初态，传 false
 * @param stats       统计（只累加）
 */
static void pfr_ledger_rebuild(ftl_ctx_s *ftl, BOOL reset_first, pfr_stats_s *stats)
{
    if (reset_first)
    {
        pfr_reset_blocks(ftl);
    }

    pfr_rebuild_closed_sb_list(ftl, stats);
}

/**
 * @brief 三条路径共用的收尾：补 Spare → 接回 Journal 写态 → CP 定时 → L2P 日志区
 *
 * @param ftl       实例
 * @param ring      Journal 环形块池扫描结果
 * @param floor_seq Journal 序号下限（fresh=1，其余=cp->journal_sequence）
 */
static void pfr_ledger_finish(ftl_ctx_s *ftl, const jrn_page_ref_s *ring, DWORD floor_seq)
{
    pfr_refill_spare_pools(ftl);
    jrn_resume(ftl->journal, ring, floor_seq);
    cp_timer_reset(ftl->cp_handle);
    md_log_area_restore(ftl->metadata_dir, &ftl->alloc);
}

/* ══════════════════════════════════════════════════════════════════
 * full metadata rebuild — 掉电重建 closed_sb_list（全 NAND 扫描）
 *
 * 目标：掉电重启后，已写满（Closed）的 Super Block 不在 closed_sb_list，
 * BST 里 state 停在 FREE（alc_init 统一置位）。后果：
 *   ① GC/WL/rr_predictive_scan 看不到这些块 → 容量泄漏
 *   ② main.c free pool 回填把它们当空白块 → 复用未擦除半满块
 *
 * 本函数逐 plane 逐 Block 扫描 NAND，判空 + L2P 反查，重建 closed 列表与 BST 有效位图。
 * Policy B：两 plane 的块独立分配、不再同号，故按**单物理块**建 Closed 条目（另一
 * plane 记 FTL_BLOCK_NO_PLANE 哨兵），配对无需恢复。
 * ══════════════════════════════════════════════════════════════════ */

/* ── 共享块 helper（full rebuild / 快照恢复共用）── */

/**
 * @brief 固定元数据区（分区表几何；含固定预留的 L2P 日志区）。
 *
 * @param g 物理块全局编号（gblock）
 *
 * @return true = 属于固定元数据区；false = 用户数据区。
 */
static BOOL pfr_block_is_fixed(WORD g)
{
    // 必须按分区表的 plane_mask 判：分区只在 mask 覆盖的 plane 上预留，表外同号块
    // 是用户数据。不要写死 "block < 某常量"——布局随介质版本演进。
    return md_block_is_metadata(NULL, ftl_gblock_plane(g), ftl_gblock_block(g));
}

/**
 * @brief 汇总打印 L2P 扫描异常（有计数时才打，避免扫描期逐块刷 UART）
 *
 * @param stats 本次启动的统计
 */
static void pfr_stats_report(const pfr_stats_s *stats)
{
    LOGW_IF(stats->l2p_read_errors != 0U, LOG_TAG_BLK "rebuild: L2P read failed for %lu CW — valid_count 可能少算\r\n", stats->l2p_read_errors);
    LOGW_IF(stats->l2p_degraded_cw != 0U, LOG_TAG_BLK "rebuild: L2P region %lu degraded — %lu CW skipped (mapping lost)\r\n", stats->first_degraded_region, stats->l2p_degraded_cw);
}

/**
 * @brief 把格式化后的**空 Directory** 落成一条目录检查点记录。
 *
 * @param ftl FTL 实例（使用 metadata_dir 与 cp_handle 的 2KB 常驻缓冲）
 *
 * @return FTL_OK / FTL_ERROR。
 *
 * @note `md_init` 只复位 RAM；不落盘的话下次开机
 * md_load 失败，而 initial Checkpoint（seq=1）存在 ⇒
 * pfr_boot_recovery 按 拒绝启动。故必须先把空 Directory
 * 写成目录检查点，再写 initial Checkpoint（后者是整条 record 的 commit）。
 */
static ftl_status_e pfr_format_write_initial_dir_ckpt(ftl_ctx_s *ftl)
{
    md_dir_cache_s     *dir = ftl->metadata_dir;
    md_dir_ckpt_write_s w;

    memset(&w, 0, sizeof(w)); /* begin 非 READY 时上下文不可用；零值 = 未持有 */

    dir->ckpt.dirty = true; /* 空 Directory 也要落一条 record */

    if (md_dir_ckpt_write_begin(dir, &w, MD_DIR_CKPT_OWNER_BOOT) != MD_DIR_CKPT_READY)
    {
        return FTL_ERROR;
    }

    if (w.need_erase)
    {
        // 与运行期 CP 路径同一个口径（md_ring_prepare_write / cp_erase_submit）：
        // 镜像池的块要所有 plane 一起擦，否则另一 plane 上残留的旧 record 会在
        // 本 plane 读失败时被 locate 当成有效记录（seq 更小）载入。
        const ftl_status_e est = (md_ring_plane_count(w.ring.plane_mask) > 1U) ? ftl_sync_erase_block_multi_plane(w.base) : ftl_sync_erase_block(w.base);

        if (est != FTL_OK)
        {
            md_dir_ckpt_write_abort(dir, &w); /* 放弃已 prepare 的目录检查点写（清单飞锁） */
            return FTL_ERROR;
        }
    }

    for (BYTE chunk = 0U; chunk < DIR_CKPT_PAGE_COUNT; chunk++)
    {
        // 2KB 目录检查点页常驻 handle（与 CP 共用 scratch；format 路径串行，无重入）。
        md_dir_ckpt_page_s *page = (md_dir_ckpt_page_s *)ftl->cp_handle->hdr.hdr_buf;
        const DWORD         addr = md_dir_ckpt_page_addr(w.base, chunk);

        md_dir_ckpt_page(dir, w.seq, chunk, page);
        if (ftl_sync_write_page_mirror(addr, page, sizeof(*page), FTL_PAGE_CFG_META()) != FTL_OK)
        {
            w.ok = false;
            break;
        }
    }

    // 安全回读：候选池已跳过 BAD，但配置错误/候选耗尽仍可能让写点落在不可读块上；
    // 编程状态可能 OK 而重启读不回来 ⇒ format 谎报成功。这里当场读回第一页。
    if (w.ok)
    {
        md_dir_ckpt_page_s *probe = (md_dir_ckpt_page_s *)ftl->cp_handle->hdr.hdr_buf;

        if ((ftl_sync_read_page(w.base, probe, sizeof(*probe), FTL_PAGE_CFG_META()) != FTL_OK) || (probe->magic != DIR_CKPT_MAGIC) || (probe->sequence != w.seq) || (probe->layout_version != FTL_MEDIA_LAYOUT_VERSION))
        {
            LOGE(LOG_TAG_META "format: directory checkpoint readback failed — fixed metadata block may be BAD\r\n");
            w.ok = false;
        }
    }

    md_dir_ckpt_commit(dir, &w);
    return w.ok ? FTL_OK : FTL_ERROR;
}

/**
 * @brief 全片体检前清空 BAD 集（保留 BBT 候选池）
 *
 * 「从头开始」的判据：旧 BAD 图不再作为既成事实，所有非 BBT-pool 块都重新
 * 擦/写/读测试。BBT 池持有本轮 FORMAT_IN_PROGRESS record，不能擦，因此保留
 * 其 BAD 状态并跳过测试。
 *
 * @param alloc    分配器
 * @param old_bad  出参：512B 位图，旧 BAD 集（调用者用于统计 recovered）
 */
static void pfr_format_reset_bad_map(alc_s *alloc, BYTE old_bad[BBT_BITMAP_SIZE])
{
    const md_ring_s *bbt_ring = (alloc->pools != NULL) ? &alloc->pools->bbt : NULL;
    WORD             kept     = 0U;

    memset(old_bad, 0, BBT_BITMAP_SIZE);

    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        const WORD block  = ftl_gblock_block(g);
        const BOOL is_bad = ftl_block_is_bad(&alloc->blocks[g]);
        const BOOL keep   = bbt_block_is_candidate(bbt_ring, block);

        if (is_bad)
        {
            bbt_set_bad(old_bad, g);
        }
        if (is_bad == false)
        {
            continue;
        }
        if (keep)
        {
            kept++;
            continue;
        }

        ftl_block_set_state(&alloc->blocks[g], FTL_BLOCK_STATE_NONE);
        ftl_block_clear_valid(&alloc->blocks[g]);
        ftl_block_set_sb(&alloc->blocks[g], FTL_BLOCK_NO_SB);
    }

    alloc->bad_count = kept;

    LOGI(LOG_TAG_FLT "format --verify: BAD map reset for full re-scan (kept BBT-pool bad=%hu)\r\n", kept);
}

/**
 * @brief format --verify 的全片体检
 *
 * 逐物理块**只做擦除**：擦除命令成功 ⇒ 好块；失败 ⇒ bbt_mark_bad 并落盘。
 * 为什么不再「写图样 → 读回比对」：
 * · 出厂坏块标记在二手片上已丢失（AN1819：擦除即不可恢复），open-card 的目的
 * 是重建可信坏块表；**擦除失败**是开卡阶段唯一稳定、且不依赖数据通路的判据。
 * · 编程 / 读失败由运行期覆盖：host 写失败 → `ftl_write_retry` 换块重发一次 →
 * 仍失败则 `ftl_host_io_write_fail` 标坏；读失败 → RR 标坏。两条路径均已上板。
 * · 写/读体检只覆盖**单 plane 顺序写**，测不到运行期的 pair 写路径，它的
 * 「通过」不能证明运行期没问题，收益与耗时不成比例。
 * ⚠ 调用前由 pfr_format_reset_bad_map 清空非 BBT 池的 BAD 集 ⇒ 本函数重新评估
 * **所有**非 BBT-pool 块（包括 BST 中标记为 BAD 的块）：擦除成功即 recovered。
 * BBT 池跳过（本轮坏块 record 在里面）；体检后所有块保持擦净状态。
 *
 * @param ftl FTL 实例（使用 alloc 块表与 BBT；调用前 BAD 集已由 pfr_format_reset_bad_map 清空）
 *
 * @return FTL_OK（体检完成，个别块已标坏）
 *         FTL_ERROR = BBT 落盘失败。
 */
static ftl_status_e pfr_format_verify_blocks(ftl_ctx_s *ftl)
{
    alc_s *alloc  = &ftl->alloc;
    DWORD  tested = 0U;
    DWORD  bad    = 0U;

    LOGW(LOG_TAG_FLT "format --verify: 全片擦除体检开始（擦除成功即判好块）\r\n");

    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        const BYTE plane = ftl_gblock_plane(g);
        const WORD block = ftl_gblock_block(g);

        // BBT 候选池保留：本轮坏块图就在里面（候选可能含 shared metadata spare）。
        {
            const md_ring_s *bbt_ring = (alloc->pools != NULL) ? &alloc->pools->bbt : NULL;
            const BOOL       keep_bbt = bbt_block_is_candidate(bbt_ring, block);

            if (keep_bbt)
            {
                continue;
            }
        }

        // 唯一判据：擦除命令成功。carve 假定 Free 块是空白的，擦成功即为空白。
        const DWORD epba = pba_make(alloc->ch, alloc->ce, alloc->lun, block, plane, 0U, 0U);
        const BOOL  ok   = (ftl_sync_erase_block(epba) == FTL_OK);

        tested++;

        if (ok == false)
        {
            bad++;
            LOGE(LOG_TAG_FLT "format --verify: gblock=%hu (plane=%hhu block=%hu) failed — mark BAD\r\n", g, plane, block);
            (void)bbt_mark_bad(alloc, g);
        }

        LOGI_IF((tested % 512U) == 0U, LOG_TAG_FLT "format --verify: %lu/%d blocks, bad=%lu\r\n", tested, FTL_BLOCK_TOTAL, bad);
    }

    bbt_dirty_note(alloc);

    if (bbt_save(alloc) != FTL_OK)
    {
        LOGE(LOG_TAG_FLT "format --verify: BBT save failed — 体检结果未持久化\r\n");
        return FTL_ERROR;
    }

    LOGI(LOG_TAG_FLT "format --verify: done, tested=%lu bad=%lu\r\n", tested, bad);
    return FTL_OK;
}

/**
 * @brief 只读扫描：journal 环最新页 + 最新 CP + 块/磨损快照
 *
 * @param ftl  实例
 * @param scan 输出：扫描结果
 * @return true 继续恢复；false 必须拒绝启动（有 CP 但 Directory 加载失败）
 */
static BOOL pfr_boot_scan(ftl_ctx_s *ftl, pfr_boot_scan_s *scan)
{
    memset(scan, 0, sizeof(*scan));
    scan->cp = (const cp_header_s *)ftl->cp_handle->hdr.hdr_buf;

    // ① 扫 Journal 环形块池取最新页（只读）。三条路径都用它恢复写指针：
    // jrn_init 把写指针/序号复位到 (block0, page0)/1，而环形块并没有被擦，
    // 不接回去就会二次编程 + 序号回退。
    (void)jrn_page_scan_latest(ftl->journal, &scan->ring, &scan->ring_read_error);

    // ② 找最新 Checkpoint。无有效记录 ⇒ 出厂首次启动。
    cp_header_s *cp = (cp_header_s *)scan->cp;

    if (cp_pool_find_latest(ftl->cp_handle, cp, &scan->cp_idx) != FTL_OK)
    {
        ftl->flags.md_load_failed = false; /* 空 Directory 是预期状态 */
        return true;                       /* has_cp = false */
    }
    scan->has_cp = true;

    // ③ 运行期元数据日志丢失 ⇒ 快照不可信，强制全量重建；不加载块表/磨损表。
    if (alc_bbt_is_rebuild_required(&ftl->alloc))
    {
        LOGE(LOG_TAG_BOOT "rebuild required (metadata log lost) — force full metadata rebuild\r\n");
        scan->rebuild_required = true;
        return true;
    }

    // ④ 有 CP 但 Directory 加载失败 ⇒ 空 Directory 会让旧 region 映射整体丢失
    // 。没有可用的重扫 L2P region 路径，唯一诚实的做法是拒绝
    // 启动并保留介质，由用户决定是否格式化。
    if (ftl->flags.md_load_failed == true)
    {
        LOGE(LOG_TAG_META "Directory load failed but checkpoint seq=%lu exists — refuse to boot\r\n", cp->sequence);
        return false;
    }

    // ⑤ 块元数据快照：失败只影响块账本路径，不触发格式化。
    WORD *words = cp_block_words_get(ftl->cp_handle);

    scan->blocks_loaded = cp_block_table_load(ftl->cp_handle, cp, scan->cp_idx, words);

    LOGW_IF(scan->blocks_loaded == false, LOG_TAG_BLK "unavailable (rec=%hu seq=%lu) — block accounting via full metadata rebuild\r\n", scan->cp_idx, cp->sequence);

    // ⑥ 磨损计数是软状态：失败只让 WL 从零起算。
    if (cp_wear_table_load(ftl->cp_handle, cp, scan->cp_idx) == false)
    {
        LOGW(LOG_TAG_WEAR "snapshot incomplete (rec=%hu seq=%lu) — read pages kept, rest default\r\n", scan->cp_idx, cp->sequence);
    }

    return true;
}

/**
 * @brief 由扫描结果决定恢复计划（纯函数，不碰介质）
 *
 * @param scan 扫描结果
 * @return 恢复计划
 */
static pfr_plan_e pfr_boot_plan(const pfr_boot_scan_s *scan)
{
    if (scan->has_cp == false)
    {
        return PFR_PLAN_FRESH;
    }
    if (scan->rebuild_required)
    {
        return PFR_PLAN_FORCE_REBUILD;
    }

    // 判型必须用整页（page_seq + entry_count），只看页首会把「写→掉电」误判成
    // clean；环内有读失败页 ⇒ 不判 clean，保守走回放。
    if ((scan->ring_read_error == false) && jrn_seq_ge(scan->cp->journal_sequence, (DWORD)(scan->ring.page_seq + scan->ring.entry_count)))
    {
        return PFR_PLAN_SNAPSHOT;
    }
    return PFR_PLAN_REPLAY;
}

/**
 * @brief 按计划执行恢复，最后一次性填满 result
 *
 * @param ftl    实例
 * @param plan   恢复计划
 * @param scan   扫描结果
 * @param result 输出：启动结果
 */
static void pfr_boot_execute(ftl_ctx_s *ftl, pfr_plan_e plan, const pfr_boot_scan_s *scan, pfr_boot_result_s *result)
{
    pfr_stats_s         stats;
    pfr_block_table_s   tbl;
    pfr_ledger_result_s led;

    pfr_stats_init(&stats);
    memset(&tbl, 0, sizeof(tbl));
    memset(&led, 0, sizeof(led));

    tbl.word      = cp_block_words_get(ftl->cp_handle);
    tbl.loaded    = scan->blocks_loaded;
    led.replay_ok = true;
    led.ctx_ok    = false;
    led.fail_pba  = NAND_PBA_INVALID;

    BOOL  snapshot_kept      = false;
    BOOL  request_cp         = false;
    BOOL  clear_md_load_fail = false;
    DWORD journal_entries    = 0U;

    // 诊断：判型所依据的现场，一次开机一行（只读）。
    if (plan == PFR_PLAN_REPLAY)
    {
        const DWORD newest = (DWORD)scan->ring.page_seq + (DWORD)scan->ring.entry_count;
        const DWORD gap    = jrn_seq_after(newest, scan->cp->journal_sequence) ? (newest - scan->cp->journal_sequence) : 0U;

        LOGW(LOG_TAG_BLK "power-failure diag: cp_seq=%lu cp_jseq=%lu ring_pseq=%lu ring_n=%hu ring_err=%d newest=%lu gap=%lu\r\n",
             scan->cp->sequence,
             scan->cp->journal_sequence,
             scan->ring.page_seq,
             scan->ring.entry_count,
             (int)scan->ring_read_error,
             newest,
             gap);
    }

    switch (plan)
    {
        case PFR_PLAN_FRESH:
            l2p_owner_invalidate(ftl->cache, topo_2_die(ftl->topo));
            pfr_ledger_rebuild(ftl, false, &stats);
            pfr_boot_carve_pools(ftl);
            break;

        case PFR_PLAN_SNAPSHOT:
        case PFR_PLAN_REPLAY:
            snapshot_kept = pfr_ledger_snapshot(ftl, scan, plan, &tbl, &stats, &led, &journal_entries);

            if (snapshot_kept && (pfr_build_pools(ftl, &tbl) == false))
            {
                snapshot_kept = false;
            }

            if (snapshot_kept)
            {
                LOGI(LOG_TAG_BLK "restore closed=%lu log=%lu free=%hu op=%hu spare=%hu anomalies=%lu valid_cw=%lu\r\n",
                     stats.closed,
                     stats.log_blocks,
                     ftl->alloc.free_count,
                     alc_pool_sum_counts(ftl->alloc.op_count, PLANE_NUM),
                     alc_pool_sum_counts(ftl->alloc.spare_count, PLANE_NUM),
                     stats.anomalies,
                     stats.valid_cw);
            }
            else
            {
                if (plan == PFR_PLAN_REPLAY)
                {
                    const WORD fg     = (led.fail_pba != NAND_PBA_INVALID) ? ftl_pba_gblock(led.fail_pba) : (WORD)UINT16_MAX;
                    const WORD vc_end = (fg < FTL_BLOCK_TOTAL) ? ftl->alloc.blocks[fg].valid_count : 0U;
                    const WORD lv_end = (fg < FTL_BLOCK_TOTAL) ? md_live_count_get(ftl->metadata_dir, fg) : 0U;

                    LOGW(LOG_TAG_BLK "fallback to full metadata rebuild (power failure, blocks_valid=%d applied=%d ctx_ok=%d replay_ok=%d reason=%hhu pba=%08lX g=%hu vc_end=%hu live=%hu sub0=%lu/%hu/%hhu)\r\n",
                         (int)tbl.loaded,
                         (int)tbl.restored,
                         (int)led.ctx_ok,
                         (int)led.replay_ok,
                         led.fail_reason,
                         led.fail_pba,
                         fg,
                         vc_end,
                         lv_end,
                         stats.sub_zero,
                         stats.sub_zero_g,
                         stats.sub_zero_state);
                }
                else
                {
                    LOGW(LOG_TAG_BLK "fallback to full metadata rebuild (clean start)\r\n");
                }

                // 回退重建用一份干净统计：旧的 probe 结果只作失败现场，
                // 不能混进 rebuild 的汇总日志（两者口径不同）。
                pfr_stats_init(&stats);
                pfr_ledger_rebuild(ftl, true, &stats);
                pfr_boot_carve_pools(ftl);
            }
            break;

        case PFR_PLAN_FORCE_REBUILD:
        default:
            pfr_ledger_rebuild(ftl, true, &stats);
            pfr_boot_carve_pools(ftl);
            clear_md_load_fail = true;
            request_cp         = true;
            break;
    }

    pfr_ledger_finish(ftl, &scan->ring, (plan == PFR_PLAN_FRESH) ? 1U : scan->cp->journal_sequence);

    // 重建后的状态尽快落一条 CP 并清 REBUILD_REQUIRED；放在 finish 之后，
    // 与「先接回 journal 写指针、再让 CP job 启动」的既有顺序一致。
    if (request_cp)
    {
        jrn_checkpoint_request(ftl->journal);
    }
    if (clear_md_load_fail)
    {
        ftl->flags.md_load_failed = false;
    }

    result->boot_type       = (plan == PFR_PLAN_FRESH) ? BOOT_FRESH_START : ((plan == PFR_PLAN_REPLAY) ? BOOT_POWER_FAILURE : BOOT_CLEAN_START);
    result->cp_sequence     = scan->has_cp ? scan->cp->sequence : 0U;
    result->journal_entries = journal_entries;
    result->blocks_restored = snapshot_kept;

    // 范围化恢复只在「快照保留」时成立；降级只丢映射，不影响 valid_count 增量。
    const BOOL degraded = (plan == PFR_PLAN_REPLAY) && (jrn_replay_degraded_entries() != 0U);

    result->scoped = snapshot_kept && (degraded || (stats.l2p_degraded_cw != 0U));
}

/* ── 块账本阶段 / 池构建 ── */

/**
 * @brief 按几何顺序切分 Spare / OP / Free 池（无快照 或 快照失败时用）。
 *
 * @param ftl FTL 实例（调用前 free/op/spare 计数必须为 0）
 *
 * @note 每 plane 先扣 Spare（50）、再扣 OP（10%），其余进 Free；只把 state == NONE 的块入池。
 *       full rebuild / 快照已认领的 CLOSED/LOG/RETIRED 只计入 total_blocks。
 */
static void pfr_boot_carve_pools(ftl_ctx_s *ftl)
{
    alc_s *alloc = &ftl->alloc;

    ASSERT(alloc->free_count == 0U);
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        ASSERT(alloc->op_count[p] == 0U);
        ASSERT(alloc->spare_count[p] == 0U);
    }

    WORD good_n = 0U;

    for (WORD gblock = 0U; gblock < FTL_BLOCK_TOTAL; gblock++)
    {
        const BYTE plane = ftl_gblock_plane(gblock);
        const WORD block = ftl_gblock_block(gblock);

        if (ftl_block_is_bad(&alloc->blocks[gblock]))
        {
            continue;
        }
        if (md_block_is_metadata(ftl->metadata_dir, plane, block))
        {
            continue;
        }

        good_n++; /* 容量口径：所有非坏/非元数据块，包含已认领的 CLOSED/LOG/RETIRED */

        // 只把仍无归属的块入池。full metadata rebuild 已把写过的块置 CLOSED；若此时再 push 进
        // Spare/OP/Free，同一块会同时是池成员与 CLOSED，后续被当“空闲已擦除”
        // 复用 ⇒ 覆盖活数据。
        if (ftl_block_state(&alloc->blocks[gblock]) != FTL_BLOCK_STATE_NONE)
        {
            continue;
        }

        if (alc_spare_push(alloc, plane, gblock))
        {
            continue;
        }
        if (alc_op_push(alloc, plane, gblock))
        {
            continue;
        }
        alc_free_push(alloc, gblock);
    }

    alloc->total_blocks = good_n;
}

/**
 * @brief 按 checkpoint region_bitmap 局部加载 L2P cache（F4）。
 *
 * @param ftl FTL 实例
 * @param cp 最近 Checkpoint 头（含 region_bitmap）。
 *
 * @note ① 只处理 resident CLEAN 行；DIRTY 保持待 flush，FLUSHING/LOADING 跳过。
 *       ② 按 CP bitmap 逐位预载；不能按 INVALID 行的 region_id 反查（init 时全为 0，
 *          按行遍历会全部别名到 region 0）。③ 条目 INVALID ⇒ 无页可预载，留给懒加载。
 *
 * 有 Journal 回放时必须在回放之后调用。
 */
static void pfr_boot_apply_region_bitmap(ftl_ctx_s *ftl, const cp_header_s *cp, pfr_stats_s *stats)
{
    const BYTE die = topo_2_die(ftl->topo);

    // ① 失效本 owner 的 resident CLEAN 行中不在 CP bitmap 的 region。
    for (WORD i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        l2p_line_info_s line;

        l2p_line_get(ftl->cache, i, &line);

        if (line.state != L2P_CACHE_CLEAN)
        {
            continue; /* DIRTY / FLUSHING / LOADING / INVALID 均不在此处理 */
        }
        if (l2p_region_owned_by(line.region_id, die) == false)
        {
            continue; /* 共享 cache：别的 LUN 的行由它们自己的恢复处理 */
        }

        const DWORD local = l2p_global_to_local_region(line.region_id);

        if ((cp->region_bitmap[local / 32U] & (1UL << (local & 31U))) == 0U)
        {
            l2p_line_invalidate(ftl->cache, i);
        }
    }

    // ② 按 bitmap 置位逐 region 预载。
    for (DWORD local = 0U; local < L2P_REGIONS_PER_INSTANCE; local++)
    {
        if ((cp->region_bitmap[local / 32U] & (1UL << (local & 31U))) == 0U)
        {
            continue;
        }

        const DWORD addr = md_entry_find(ftl->metadata_dir, METADATA_TYPE_L2P_REGION, local);

        if (addr == NAND_PBA_INVALID)
        {
            continue; /* 未映射：无页可预载，懒加载合成全 INVALID 行 */
        }

        const DWORD        global = l2p_local_to_global_region(local, die);
        const DWORD        backup = md_backup_find(ftl->metadata_dir, local);
        const ftl_status_e st     = l2p_region_preload(ftl->cache, global, addr, backup);

        if (st == FTL_ERROR)
        {
            /* 提交层/地址硬失败 = 快照里的 L2P 位置不可信；向上传播，
             * 由 pfr_ledger_snapshot 回退 full metadata rebuild（RISK-BOOT-11）。 */
            stats->l2p_read_errors++;
            LOGW(LOG_TAG_BLK "preload region %lu hard-failed — snapshot not trustworthy\r\n", local);
        }
        else if (st == FTL_AGAIN)
        {
            LOGW(LOG_TAG_BLK "preload region %lu skipped: no cache line\r\n", local);
        }
        else if (l2p_is_region_degraded(ftl->cache, global) != false)
        {
            /* 主备完成态都失败：范围化恢复保留快照，旧映射丢失（RISK-BOOT-18）。 */
            stats->l2p_degraded_cw++;
            LOGW(LOG_TAG_BLK "preload region %lu degraded — snapshot kept\r\n", local);
        }
    }
}

/**
 * @brief 块账本快照阶段：restore →（replay）→ bitmap → probe
 *
 * replay 只要进入本阶段就执行——即使块元数据恢复失败，回退的 full rebuild
 * 也要靠回放后的 L2P 反查 valid_count。返回 false 表示快照结果不可用。
 *
 * @param ftl         实例
 * @param scan        扫描结果
 * @param plan        SNAPSHOT 或 REPLAY
 * @param tbl         块表（restore/probe 会写 word，probe 置 probed）
 * @param stats       统计（只累加）
 * @param out         失败现场（供 fallback 日志）
 * @param out_entries 输出：journal 回放条数（SNAPSHOT 保持 0）
 * @return true 快照可用；false 调用者回退 full metadata rebuild
 */
static BOOL pfr_ledger_snapshot(ftl_ctx_s *ftl, const pfr_boot_scan_s *scan, pfr_plan_e plan, pfr_block_table_s *tbl, pfr_stats_s *stats, pfr_ledger_result_s *out, DWORD *out_entries)
{
    const cp_header_s *cp = scan->cp;

    /* Journal 扫描读失败 ⇒ replay 的判定依据不可信（可能把损坏页当成“无更多
     * 条目”）。不能继续声称快照 restored，必须回退 full metadata rebuild
     *（RISK-BOOT-08）。 */
    if (scan->ring_read_error != false)
    {
        LOGW(LOG_TAG_BLK "journal scan read error — snapshot untrusted, fallback rebuild\r\n");
        out->replay_ok = false;
        out->ctx_ok    = false;
        return false;
    }

    // ① 块元数据快照恢复（若已加载）。第 1 遍全量校验，非法 word 不改任何状态。
    tbl->restored = (tbl->loaded != false) && pfr_restore_blocks(ftl, tbl);
    out->ctx_ok   = tbl->restored;

    if (plan == PFR_PLAN_REPLAY)
    {
        pfr_replay_ctx_s rctx;
        jrn_replay_ctx_s block_ctx;
        DWORD            entry_count = 0U;

        memset(&block_ctx, 0, sizeof(block_ctx));
        block_ctx.fail_pba = NAND_PBA_INVALID; /* 0 是合法 PBA：失败位置必须显式置哨兵 */
        rctx.ftl           = ftl;
        rctx.stats         = stats;

        if (tbl->restored)
        {
            block_ctx.materialize = pfr_materialize_pba;
            block_ctx.valid_add   = pfr_valid_add;
            block_ctx.valid_sub   = pfr_valid_sub;
            block_ctx.ctx         = &rctx;
            block_ctx.ok          = true;
        }

        // ② Journal 回放：映射写入 L2P Cache；块表可用时同步维护 valid 增量。
        // 回放必须走 region line：PFR 随后用 l2p_line_invalidate 作废不在 CP bitmap
        // 的 CLEAN 行；若回放把映射写进 dirty CMT，会被这里误清。回放期 CMT 全程旁路。
        l2p_cmt_set_enabled(ftl->cache, false);
        const BOOL replay_ok = (jrn_replay(ftl->journal, ftl->cache, cp->journal_sequence, &entry_count, (tbl->restored != false) ? &block_ctx : NULL) == FTL_OK);
        l2p_cmt_set_enabled(ftl->cache, true);
        *out_entries     = entry_count;
        out->replay_ok   = replay_ok;
        out->fail_pba    = block_ctx.fail_pba;
        out->fail_reason = block_ctx.fail_reason;

        if (replay_ok == false)
        {
            LOGW(LOG_TAG_BLK "journal replay incomplete — force full metadata rebuild\r\n");
            out->ctx_ok = false;
        }

        // ③ 一致性守卫：目录引用的用户块，valid_count 必须 > 0。
        // 判据是现成账本 l2p_live（每块被目录引用的 region 数），O(FTL_BLOCK_TOTAL)，无 I/O。
        if (tbl->restored)
        {
            WORD bad_n     = 0U;
            WORD first_g   = UINT16_MAX;
            WORD first_liv = 0U;

            for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
            {
                const WORD live = md_live_count_get(ftl->metadata_dir, g);

                // 只比用户数据块：固定区 + L2P 日志块本就不维护 valid_count。
                if (md_block_is_metadata(ftl->metadata_dir, ftl_gblock_plane(g), ftl_gblock_block(g)))
                {
                    continue;
                }
                if ((live == 0U) || ftl_block_is_bad(&ftl->alloc.blocks[g]) || (ftl->alloc.blocks[g].valid_count != 0U))
                {
                    continue;
                }
                if (bad_n == 0U)
                {
                    first_g   = g;
                    first_liv = live;
                }
                bad_n++;
            }

            if (bad_n != 0U)
            {
                const bst_state_e st = ftl_block_state(&ftl->alloc.blocks[first_g]);

                LOGE(LOG_TAG_BLK "count/dir mismatch: %hu block(s) referenced but valid_count=0; first g=%hu live=%hu st=%s bad=%hu\r\n", bad_n, first_g, first_liv, ftl_block_state_name(st), ftl->alloc.bad_count);
                out->ctx_ok = false;
            }
        }

        // 回放期「减到 0 被跳过」的现场：非 0 才打一行。
        if (stats->sub_zero != 0U)
        {
            const bst_state_e zst = (stats->sub_zero_g < FTL_BLOCK_TOTAL) ? ftl_block_state(&ftl->alloc.blocks[stats->sub_zero_g]) : FTL_BLOCK_STATE_NONE;

            LOGW(LOG_TAG_BLK "replay: %lu sub-at-zero kept (not fatal); first g=%hu st=%s live=%hu\r\n", stats->sub_zero, stats->sub_zero_g, ftl_block_state_name(zst), (stats->sub_zero_g < FTL_BLOCK_TOTAL) ? md_live_count_get(ftl->metadata_dir, stats->sub_zero_g) : 0U);
        }

        LOGW_IF(jrn_replay_degraded_entries() != 0U, LOG_TAG_BLK "scoped recovery: %lu journal entr(ies) hit degraded L2P region(s) — snapshot kept\r\n", jrn_replay_degraded_entries());

        // ④ 按 CP region_bitmap 局部加载（F4）：跳过 DIRTY 回放行，非 bitmap 区懒加载。
        pfr_boot_apply_region_bitmap(ftl, cp, stats);
    }
    else if (tbl->restored)
    {
        pfr_boot_apply_region_bitmap(ftl, cp, stats);
    }

    if ((tbl->restored == false) || (out->ctx_ok == false))
    {
        return false;
    }

    // ⑤ probe 池块：CP 后分配但 journal 条目可能丢失的块；也是唯一入池前的最后对账。
    pfr_probe_blocks(ftl, tbl, stats);

    if (stats->l2p_read_errors != 0U)
    {
        LOGW(LOG_TAG_BLK "probe: %lu L2P read error(s) — fallback to full metadata rebuild\r\n", stats->l2p_read_errors);
        return false;
    }

    // 降级 region 只丢映射、不影响 valid_count 增量 ⇒ 不让它触发全量重建。
    LOGW_IF((plan == PFR_PLAN_REPLAY) && (stats->l2p_degraded_cw != 0U), LOG_TAG_BLK "scoped recovery: %lu CW in degraded L2P region(s) — snapshot kept, mappings lost\r\n", stats->l2p_degraded_cw);

    return true;
}

/**
 * @brief 把一个物理块发成退化单 plane Closed 条目（与 full metadata rebuild 同口径；Policy B 不要求配对）。
 *
 * @param ftl         FTL 实例
 * @param g           物理块全局编号（gblock）
 * @param temperature 块温度（FTL_TEMP_HOT / FTL_TEMP_COLD）
 *
 * @return true  = 条目已加入 Closed 列表；
 *         false = closed_sb_list 满（调用者据此判恢复失败，回退 full metadata rebuild）。
 */
static BOOL pfr_closed_sb_add(ftl_ctx_s *ftl, WORD g, BYTE temperature)
{
    WORD plane_blocks[PLANE_NUM];

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        plane_blocks[p] = FTL_BLOCK_NO_PLANE;
    }
    plane_blocks[ftl_gblock_plane(g)] = ftl_gblock_block(g);

    if (alc_super_closed_add(&ftl->alloc, plane_blocks, temperature) == FTL_BLOCK_NO_SB)
    {
        LOGE(LOG_TAG_BLK "closed_sb_list full, g=%hu dropped\r\n", g);
        return false;
    }

    return true;
}

// 判空统一走 ftl_fc_interface.c 的 ftl_page_is_blank（C2：唯一实现；
// full rebuild / BBT 探针 / Journal 扫页 / 元数据日志区恢复共用）。
// 它是**解码路径**判据（FC 判 ECC 是否成码字 + 整条 CW 的 1 比特占比），
// 不是「RAW 读数 FF」——RAW 读刻意跳过 ECC，没有判空依据。

/**
 * @brief 扫出某 Block 的写指针（第一个空白页），并检测写失败留下的 hole。
 *
 * @param ftl     FTL 实例（只读 topo，用于构造 PBA）
 * @param plane   物理 plane 号
 * @param block   plane 内的物理块号
 * @return 0               = 整块空白；
 *         k               = 从页 0 到 k-1 连续写过、页 k 起全空；
 *         PAGES_PER_BLOCK = 全写满**或存在 hole**（空白页之后又有写过的页）。
 *
 * @note 存在 hole 时不能把第一个空白页当上界——那会把 hole 之后的页漏出 valid_count
 * ，故返回全块让调用者逐页反查。
 * ⚠ 代价：存在 hole 的块要扫完全部页（正常部分写块仍在第一个空白页停止）。
 * 这类块只可能由写失败产生，属少数；换来的 valid_count 正确性是 GC 选择
 * victim 的前提。
 */
static BYTE pfr_scan_write_pointer(const ftl_ctx_s *ftl, BYTE plane, WORD block)
{
    // 快路径：page 0 是擦除页 ⇒ 整块空白（顺序追加 + page0 失败即关 SB，后续页不可达）。
    // 这把空闲块扫描从 64 页降到 1 页；若将来允许 page0 失败后继续写，必须恢复全块扫描。
    DWORD first = pba_make(ftl->topo.ch, ftl->topo.ce, ftl->topo.lun, block, plane, 0U, 0U);

    if (ftl_page_is_blank(first))
    {
        // page0 空白还不能判定整块空白：cache program 批次 / 异步写可能留下
        // 「page0 失败、page1..3 已编程」的块。探测前 4 页（batch 上限），
        // 任一非空 ⇒ 按有 hole 处理，走全块扫描/关闭，绝不当空闲块复用。
        for (BYTE page = 1U; page < RECOVERY_PAGE0_PROBE_PAGES; page++)
        {
            if (page >= PAGES_PER_BLOCK)
            {
                break;
            }

            DWORD addr = pba_make(ftl->topo.ch, ftl->topo.ce, ftl->topo.lun, block, plane, page, 0U);

            if (ftl_page_is_blank(addr) == false)
            {
                return PAGES_PER_BLOCK;
            }
        }

        return 0U; /* 前 4 页全空白 */
    }

    // page 0 已写过：从 page 1 起继续找第一个空白页，并检测 hole
    // （空白页之后又出现写过的页 ⇒ 返回整块，让调用者逐页反查）。
    BYTE first_blank = PAGES_PER_BLOCK;

    for (BYTE page = 1U; page < PAGES_PER_BLOCK; page++)
    {
        DWORD addr = pba_make(ftl->topo.ch, ftl->topo.ce, ftl->topo.lun, block, plane, page, 0U);

        if (ftl_page_is_blank(addr))
        {
            if (first_blank == PAGES_PER_BLOCK)
            {
                first_blank = page;
            }
            continue;
        }

        if (first_blank != PAGES_PER_BLOCK)
        {
            return PAGES_PER_BLOCK; /* hole：空白页后仍有写过的页 */
        }
    }

    return first_blank;
}

/**
 * @brief 写过的物理块：进 Closed 列表（退化单 plane 条目）+ 逐 CW L2P 双校验回填 valid_count
 * （只计 L2P 仍指向本页本 CW 的真有效 CW，覆盖/torn 页不计；无常驻位图）。
 *
 * @param ftl         FTL 实例
 * @param plane       物理 plane 号
 * @param block       plane 内的物理块号
 * @param pages_written 已写页数（来自 pfr_scan_write_pointer；调用者已跳过 0）
 * @param temperature 块温度（FTL_TEMP_HOT / FTL_TEMP_COLD）
 * @param stats       统计（只累加 closed / l2p_read_errors / l2p_degraded_cw / valid_cw）
 *
 * @note Policy B 下配对不再需要被恢复——两 plane 的块各自独立归位即可，故本函数按
 * **单个物理块** 建一条 Closed 条目，另一 plane 记 FTL_BLOCK_NO_PLANE 哨兵。
 */
static BOOL pfr_close_block(ftl_ctx_s *ftl, BYTE plane, WORD block, BYTE pages_written, BYTE temperature, pfr_stats_s *stats)
{
    alc_s *const alloc = &ftl->alloc;
    const WORD   g     = ftl_gblock(plane, block);

    // 退化单 plane 条目的构造与快照恢复同源（C2）。列表满（结构上不应发生）⇒ 放弃本块。
    if (pfr_closed_sb_add(ftl, g, temperature) == false)
    {
        return false;
    }

    // 逐页：gc_read_page_with_lba 取 LBA + **逐 CW** l2p 反查。
    // ⚠ 只扫已写页 [0, pages_written)：写指针之后的空白页没有 META，
    // 读回来也解不出 LBA，纯白读。
    bst_block_s *b = &alloc->blocks[g];
    for (WORD page = 0U; page < pages_written; page++)
    {
        // 条目只含本 plane 的块号 ⇒ 页基址直接由 (block, plane, page) 构造。
        DWORD pba = pba_make(ftl->topo.ch, ftl->topo.ce, ftl->topo.lun, block, plane, page, 0U);

        // 读失败 / 全页无写过 CW → 该页不可用，跳过。
        // CW 级映射：逐 CW 从页内 META（= OOB 反向映射）取回扇区 LBA。
        DWORD cw_lba[CW_NUM];
        if (gc_read_page_with_lba(pba, cw_lba) != FTL_OK)
        {
            continue;
        }

        for (BYTE cw = 0U; cw < CW_NUM; cw++)
        {
            // ⚠ 先校验不可信 META LBA 再使用：FTL_LBA_INVALID / 越界都不查表
            // （H29；全全局化后坏值可能落进另一个合法区间）。
            if ((cw_lba[cw] == FTL_LBA_INVALID) || ((QWORD)cw_lba[cw] >= L2P_GLOBAL_LBA_MAX) || (lba_routes_to(cw_lba[cw], ftl->topo) == false))
            {
                continue;
            }

            // ⚠ full metadata rebuild = 结构性同步路径（开机、superloop 未建立）：用同步逃生口。
            // 读得 INVALID 与“未映射”同义 ⇒ 该 CW 不计入有效数。
            DWORD cur = NAND_PBA_INVALID;

            if (l2p_read_sync(ftl->cache, cw_lba[cw], &cur) != FTL_OK)
            {
                // 读失败 ⇒ 该 CW 无法确认有效；valid_count 会少算。计数向上传播：
                // 启动快照路径据此回退 full metadata rebuild，full metadata rebuild 路径至少可见。
                stats->l2p_read_errors++;
                continue;
            }

            // 读成功也可能来自“重试耗尽 → 降级为全未映射”：返回值同样是 OK + INVALID。
            // 降级只丢**该 region 未被 Journal 覆盖的旧映射**，不影响 valid_count 增量
            // ⇒ 计入 l2p_degraded_cw 并继续，不再据此回退全量重建；
            // 丢失的映射由 host 读路径按 IO_ERROR 暴露。
            const DWORD global_region = lba_to_global_region(cw_lba[cw]);

            if (l2p_is_region_degraded(ftl->cache, global_region))
            {
                // 降级 = 映射已丢（范围化恢复接受它）；只有真正的读失败才计入
                // l2p_read_errors 并回退全量重建。
                stats->l2p_degraded_cw++;
                if (stats->first_degraded_region == UINT32_MAX)
                {
                    stats->first_degraded_region = global_region;
                }
                continue;
            }

            if ((cur != NAND_PBA_INVALID) && (cur == pba_with_cw(pba, cw)))
            {
                ftl_block_add_valid(b); /* valid_count 单位 = CW */
                stats->valid_cw++;
            }
        }
    }

    return true;
}

/**
 * @brief full metadata rebuild 掉电重建——逐 plane 逐块扫 NAND，把「写过的物理块」拉出 Free Pool 并建 Closed 条目，同时逐 CW L2P 双校验回填 valid_count。
 *
 * @param ftl FTL 实例
 * @param stats 统计（只累加 closed / partial / l2p_* / valid_cw）。
 *
 * @note Policy B 下两 plane 块不同号 ⇒ 按单物理块建 Closed 条目，另一 plane 记哨兵。
 */
static void pfr_rebuild_closed_sb_list(ftl_ctx_s *ftl, pfr_stats_s *stats)
{
    alc_s *alloc = &ftl->alloc;

    // Policy B：**逐 plane 独立扫描**——两 plane 的块不再同号。配对只在写入时由
    // allocator 创建，之后从不被查找（multi-plane 读的两个 PBA 各自从 L2P 查；
    // GC/WL 逐 plane 工作），故重建只需把"写过的物理块各自归位"，无需、也无法
    // 恢复"哪两块曾是一对"（配对取决于 GC 历史，不在任何持久化位图里）。
    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        for (WORD block = 0U; block < BLOCKS_PER_PLANE; block++)
        {
            WORD         g = ftl_gblock(plane, block);
            bst_block_s *b = &alloc->blocks[g];

            // ① 坏块跳过（判据与 main.c free pool 回填一致，per-plane 块号）
            if (ftl_block_is_bad(b))
            {
                continue;
            }

            // 固定元数据块不参与块账本，保持 NONE（恢复校验对固定块整段跳过）。
            if (md_block_is_metadata(NULL, plane, block))
            {
                continue;
            }

            // 动态 LOG 块归 metadata_dir 所有，不在 allocator 任何池里——但 BST state
            // **必须显式写 LOG**，不能留 NONE。`cp_block_snapshot()` 只按
            // BST state + OP/Spare 池归属编码块表；NONE 且无池标签会被下一次开机的
            // `pfr_restore_blocks()` 第 1 遍 判为非法并拒绝**整份**快照
            // ⇒ 又走全盘 build，形成每次冷启动都 rebuild 的循环。
            // Directory 是动态 LOG 块身份的唯一权威，这里按它落 state。
            if (md_block_is_metadata(ftl->metadata_dir, plane, block))
            {
                ftl_block_set_state(b, FTL_BLOCK_STATE_LOG);
                ftl_block_clear_valid(b);
                ftl_block_set_temp(b, 0U);
                ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
                continue;
            }

            // ② 判空找写指针（第一个擦除页）
            BYTE np = pfr_scan_write_pointer(ftl, plane, block);

            // ③ 未写过（整块擦除页）→ 保持 FREE
            if (np == 0U)
            {
                continue;
            }

            // 写过：从 free pool 拉出（防止开机后当作空白块复用覆盖）
            alc_free_remove(alloc, g);

            // ④ 写过的块一律进 Closed——含写满块与掉电时的部分写块。
            // 部分写块不再"续写"（旧 recover_open_sb 行为）：per-block 下无法确定两个
            // 部分写块是否曾是一对，强行恢复 Open SB 会让后续 multi-plane 写落到错误的
            // plane1 块。整块交 GC 迁移其有效页——安全性等价（块不会被当空白复用），
            // 代价是掉电后多一次 GC 搬移。
            if (pfr_close_block(ftl, plane, block, np, FTL_TEMP_HOT, stats))
            {
                stats->closed++;
            }

            if (np < PAGES_PER_BLOCK)
            {
                stats->partial++;
            }
        }
    }

    // ⑤ 写过的块已从 free pool 拉出；free_count 是唯一账本，无派生计数需要重同步。

    // 观测口（上板验证用）：valid_cw 是本轮重建**回填**的有效 CW 数。
    // ⚠ 它是后续覆盖写的安全前提——`ftl_confirm_write` 对旧映射所在块做
    // `ftl_block_sub_valid`（bst.h 内断言 `valid_count > 0`）。若介质上确有数据
    // 而 valid_cw == 0，则重建没认到这些 CW，下一次覆盖写该 LBA 会触发断言。
    LOGI(LOG_TAG_BLK "rebuild blocks=%lu partial=%lu valid_cw=%lu\r\n", stats->closed, stats->partial, stats->valid_cw);
    pfr_stats_report(stats);
}

/* ══════════════════════════════════════════════════════════════════
 * 块元数据快照恢复（Checkpoint 的 16-bit/块 word）
 *
 * 格式编码在 checkpoint.h / checkpoint.c（记录格式的 owner）；本节只做**恢复编排**：
 *   restore（state/valid/temp + Closed 列表 + LOG 对账）
 *   → journal replay 的 materialize 回调
 *   → probe（CP 后分配但 journal 丢页的池块）
 *   → build_pools（唯一入池点）
 * 任一步结构非法 ⇒ 调用者 `pfr_reset_blocks` + full metadata rebuild → carve。
 * ══════════════════════════════════════════════════════════════════ */

/**
 * @brief 按 Checkpoint 块元数据快照恢复 BST 块状态与 Closed 列表。
 *
 * 第 1 遍：先全量校验所有 word，任何非法 word 都在改动状态之前拒绝；随后
 * 第 2 遍：恢复 state/valid/temp/sb；第 3 遍：用 Metadata Directory 修正动态 LOG 块；
 * 第 4 遍：为 CLOSED 块补发 Closed 条目。
 *
 * @param ftl FTL 实例
 * @param tbl 块表；`word` 指向 FTL_BLOCK_TOTAL 项快照，第 3 遍回写对账后的 LOG 块
 *
 * @return true  = 恢复完成；
 *         false = 快照存在非法 word / 池标签冲突 / Closed 列表满，调用者应 reset 并回退 full metadata rebuild。
 */
static BOOL pfr_restore_blocks(ftl_ctx_s *ftl, pfr_block_table_s *tbl)
{
    alc_s *alloc = &ftl->alloc;
    WORD  *words = tbl->word;

    // 第 1 遍：先全量校验。任何非法 word 必须在**改动任何状态之前**拒绝——
    // 否则后面遇到坏 word 时，前面已应用的 BAD/state 会留下（reset 保留 BAD），
    // 语义损坏的快照可以永久标坏好块。
    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        const WORD w     = words[g];
        const WORD valid = cp_block_word_valid(w);
        const BYTE st    = cp_block_word_state(w);
        const BYTE temp  = cp_block_word_temp(w);
        const BYTE pool  = cp_block_word_pool(w);

        if (pfr_block_is_fixed(g) || ftl_block_is_bad(&alloc->blocks[g]))
        {
            continue; /* 这两类在 第 2 遍 不读取 word 的字段 */
        }

        if ((valid > CP_BLOCK_WORD_MAX_VALID) || (st > FTL_BLOCK_STATE_BAD) || (temp > 1U) || (pool > CP_BLOCK_POOL_SPARE))
        {
            LOGE(LOG_TAG_BLK "invalid word g=%hu w=0x%04hx\r\n", g, w);
            return false;
        }
        if ((pool != CP_BLOCK_POOL_NONE) && (st != FTL_BLOCK_STATE_NONE))
        {
            LOGE(LOG_TAG_BLK "g=%hu pool=%hhu state=%hhu conflict\r\n", g, pool, st);
            return false;
        }
        if ((st == FTL_BLOCK_STATE_FREE) && (valid != 0U))
        {
            LOGE(LOG_TAG_BLK "g=%hu FREE with valid=%hu\r\n", g, valid);
            return false;
        }
        // live 动态 LOG 块例外：它不属任何 allocator 池，full rebuild 留下的
        // word 就是 NONE/no-pool。Directory 是身份的唯一权威 ⇒ 放行；第 3 遍 会把它
        // 修正为 LOG（自愈路径，避免为一处旧快照做全盘 rebuild）。
        if ((st == FTL_BLOCK_STATE_NONE) && (pool == CP_BLOCK_POOL_NONE) && (md_live_count_get(ftl->metadata_dir, g) == 0U))
        {
            LOGE(LOG_TAG_BLK "g=%hu NONE with no pool tag\r\n", g);
            return false;
        }
    }

    // 第 2 遍：state / valid / temp / sb_id（不做池、不发 Closed 条目）。
    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        const WORD w     = words[g];
        const WORD valid = cp_block_word_valid(w);
        const BYTE st    = cp_block_word_state(w);
        const BYTE temp  = cp_block_word_temp(w);
        const BYTE pool  = cp_block_word_pool(w);

        bst_block_s *b = &alloc->blocks[g];

        if (pfr_block_is_fixed(g))
        {
            // 固定元数据块不参与块元数据；但 BAD 是终态（BBT），不得被清掉。
            if (ftl_block_is_bad(b))
            {
                ftl_block_clear_valid(b);
                ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
                continue;
            }
            ftl_block_set_state(b, FTL_BLOCK_STATE_NONE);
            ftl_block_clear_valid(b);
            ftl_block_set_temp(b, 0U);
            ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
            continue;
        }

        // BBT 已判 BAD：终态优先，快照不得复活它。
        if (ftl_block_is_bad(b))
        {
            ftl_block_clear_valid(b);
            ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
            continue;
        }

        if ((valid > CP_BLOCK_WORD_MAX_VALID) || (st > FTL_BLOCK_STATE_BAD) || (temp > 1U) || (pool > CP_BLOCK_POOL_SPARE))
        {
            LOGE(LOG_TAG_BLK "invalid word g=%hu w=0x%04hx\r\n", g, w);
            return false;
        }
        if ((pool != CP_BLOCK_POOL_NONE) && (st != FTL_BLOCK_STATE_NONE))
        {
            LOGE(LOG_TAG_BLK "g=%hu pool=%hhu state=%hhu conflict\r\n", g, pool, st);
            return false;
        }
        if ((st == FTL_BLOCK_STATE_FREE) && (valid != 0U))
        {
            LOGE(LOG_TAG_BLK "g=%hu FREE with valid=%hu\r\n", g, valid);
            return false;
        }

        switch ((bst_state_e)st)
        {
            case FTL_BLOCK_STATE_BAD:
                // 快照的 BAD 与 BBT 的并集；bad_count 只对新增项加一，
                // 并置 dirty 让后台 bbt_handle 把并集重新落盘。
                ftl_block_set_bad(b);
                alloc->bad_count++;
                // boot 期并集：不走 bbt_mark_bad（Spare/Free Pool 尚未建），
                // 但 dirty 必须走同一闸门，避免丢掉 dirty_gen。
                bbt_dirty_note(alloc);
                break;

            case FTL_BLOCK_STATE_LOG:
                // Directory 在 第 3 遍 对账（它才是动态块身份的权威）。
                ftl_block_set_state(b, FTL_BLOCK_STATE_LOG);
                ftl_block_clear_valid(b);
                ftl_block_set_temp(b, 0U);
                ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
                break;

            case FTL_BLOCK_STATE_RETIRED:
                ftl_block_set_state(b, FTL_BLOCK_STATE_RETIRED);
                ftl_block_clear_valid(b);
                ftl_block_set_temp(b, temp);
                ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
                break;

            case FTL_BLOCK_STATE_OPEN:
            case FTL_BLOCK_STATE_CLOSED:
                // Open SB 不续写，统一降级为 Closed（与 full metadata rebuild 同语义）。
                ftl_block_set_state(b, FTL_BLOCK_STATE_CLOSED);
                ftl_block_set_valid(b, valid);
                ftl_block_set_temp(b, temp);
                ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
                break;

            case FTL_BLOCK_STATE_FREE:
            case FTL_BLOCK_STATE_NONE:
                // FREE / OP / SPARE 一律先留 NONE：build_pools 之后才入池
                // （alc_free_push 拒绝重复归还，预置 FREE 会触发闸门）。
                ftl_block_set_state(b, FTL_BLOCK_STATE_NONE);
                ftl_block_clear_valid(b);
                ftl_block_set_temp(b, 0U);
                ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
                break;

            default:
                return false;
        }
    }

    // 第 3 遍：Metadata Directory 是动态 LOG 块的唯一权威。
    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        if (pfr_block_is_fixed(g))
        {
            continue;
        }
        bst_block_s *b = &alloc->blocks[g];

        if (ftl_block_is_bad(b))
        {
            continue;
        }
        if (md_live_count_get(ftl->metadata_dir, g) > 0U)
        {
            // NONE = full rebuild 留下的 NONE/no-pool（自愈路径，
            // 可能上百块，不逐块告警）；其他非 LOG 状态才是真冲突，留痕一次。
            const BYTE prev = ftl_block_state(b);

            LOGW_IF((prev != FTL_BLOCK_STATE_LOG) && (prev != FTL_BLOCK_STATE_NONE), LOG_TAG_BLK "g=%hu state=%hhu but Directory says LOG — Directory wins\r\n", g, prev);
            ftl_block_set_state(b, FTL_BLOCK_STATE_LOG);
            ftl_block_clear_valid(b);
            ftl_block_set_temp(b, 0U);
            ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
            words[g] = cp_block_word_pack(0U, FTL_BLOCK_STATE_LOG, 0U, CP_BLOCK_POOL_NONE);
        }
    }

    // 第 4 遍：Closed 列表（退化单 plane；Open 已在 第 2 遍 降级）。
    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        if (pfr_block_is_fixed(g))
        {
            continue;
        }
        bst_block_s *b = &alloc->blocks[g];

        if (ftl_block_is_bad(b) || (ftl_block_state(b) != FTL_BLOCK_STATE_CLOSED))
        {
            continue;
        }
        if (pfr_closed_sb_add(ftl, g, b->temp) == false)
        {
            return false; /* 列表满：恢复结果不完整，调用者回退 full metadata rebuild */
        }
    }

    return true;
}

/**
 * @brief 探测 CP 后分配、但 journal 条目可能丢失的池块，并处理陈旧 LOG 块。
 *
 * 活 LOG 块（Directory 仍引用）只计数；陈旧 LOG 按全页判空：已擦净交回 Free Pool，
 * 仍有残留则发 Closed（valid=0）交 GC。池块 page 0 非空（或 page 1..3 非空）⇒
 * 视为异常块，转 Closed 并按全块补算 valid_count。
 *
 * @param ftl   FTL 实例
 * @param tbl   块表；陈旧 LOG 擦净后回写 word，结束时置 probed
 * @param stats 统计（只累加 log_blocks / closed / anomalies / l2p_* / valid_cw）
 */
static void pfr_probe_blocks(ftl_ctx_s *ftl, pfr_block_table_s *tbl, pfr_stats_s *stats)
{
    alc_s *alloc = &ftl->alloc;
    WORD  *words = tbl->word;

    tbl->probed   = true;
    stats->closed = alloc->closed_sb_count; /* restore / replay 已建的条目 */

    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        if (pfr_block_is_fixed(g))
        {
            continue;
        }

        bst_block_s *b = &alloc->blocks[g];

        if (ftl_block_is_bad(b))
        {
            continue;
        }

        const BYTE        plane = ftl_gblock_plane(g);
        const WORD        block = ftl_gblock_block(g);
        const bst_state_e st    = ftl_block_state(b);

        if (st == FTL_BLOCK_STATE_LOG)
        {
            if (md_live_count_get(ftl->metadata_dir, g) > 0U)
            {
                stats->log_blocks++;
                continue; /* Directory 仍引用：活的 L2P 日志块 */
            }

            // 陈旧 LOG（已不被 Directory 引用）：全页判空。
            // ⚠ 不能只看 page 0——元数据游标在写失败时会跳过该页。
            BOOL any = false;

            for (BYTE page = 0U; page < PAGES_PER_BLOCK; page++)
            {
                DWORD pba = pba_make(ftl->topo.ch, ftl->topo.ce, ftl->topo.lun, block, plane, page, 0U);

                if (ftl_page_is_blank(pba) == false)
                {
                    any = true;
                    break;
                }
            }

            if (any == false)
            {
                // 已被擦除：交回 Free Pool。
                ftl_block_set_state(b, FTL_BLOCK_STATE_NONE);
                ftl_block_clear_valid(b);
                ftl_block_set_temp(b, 0U);
                ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
                words[g] = cp_block_word_pack(0U, FTL_BLOCK_STATE_FREE, 0U, CP_BLOCK_POOL_NONE);
            }
            else
            {
                // 残留的是 L2P 页而非用户数据：**不要**按用户 META 反查
                // （会把元数据页误当映射），直接发 Closed + valid=0 交 GC 擦除。
                ftl_block_set_state(b, FTL_BLOCK_STATE_CLOSED);
                ftl_block_clear_valid(b);
                ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
                pfr_closed_sb_add(ftl, g, FTL_TEMP_HOT);
                stats->closed++;
            }
            continue;
        }

        if (st != FTL_BLOCK_STATE_NONE)
        {
            continue; /* CLOSED / RETIRED 已记账（valid=0 的已擦除块留给 GC 快速路径） */
        }

        const WORD w    = words[g];
        const BYTE pool = cp_block_word_pool(w);
        const BYTE ws   = cp_block_word_state(w);

        if (((ws == FTL_BLOCK_STATE_FREE) || (pool == CP_BLOCK_POOL_OP) || (pool == CP_BLOCK_POOL_SPARE)) == false)
        {
            continue; /* 不是池块（不应发生；build_pools 会拒绝） */
        }

        // CP 后被分配、但 journal 条目可能整页丢失的块：page 0 非空即异常。
        // 写失败会造成页内空洞，故这里只做触发判据；补算 valid 时按全块扫。
        // ⚠ 残余风险：page 0 写失败、后续页写成功、且相关 journal 条目全部丢失时，
        // 本探测会漏判。窗口极窄（要求 GC 首页写失败 + 末页 journal 丢失），
        // 且被复用的块内容已无 L2P 引用；接受并在注释登记。
        DWORD pba0 = pba_make(ftl->topo.ch, ftl->topo.ce, ftl->topo.lun, block, plane, 0U, 0U);

        if (ftl_page_is_blank(pba0))
        {
            BOOL later_written = false;

            // 同 scan_write_pointer：page0 空白时补探 page1..3（cache batch 上限）。
            for (BYTE page = 1U; page < RECOVERY_PAGE0_PROBE_PAGES; page++)
            {
                if (page >= PAGES_PER_BLOCK)
                {
                    break;
                }

                DWORD pba = pba_make(ftl->topo.ch, ftl->topo.ce, ftl->topo.lun, block, plane, page, 0U);

                if (ftl_page_is_blank(pba) == false)
                {
                    later_written = true;
                    break;
                }
            }

            if (later_written == false)
            {
                continue;
            }
        }

        ftl_block_set_state(b, FTL_BLOCK_STATE_CLOSED);
        ftl_block_clear_valid(b);
        ftl_block_set_sb(b, FTL_BLOCK_NO_SB);

        (void)pfr_close_block(ftl, plane, block, PAGES_PER_BLOCK, FTL_TEMP_HOT, stats);
        stats->anomalies++;
    }

    pfr_stats_report(stats);
}

/**
 * @brief 按 BST 状态与快照 word 把 NONE 块放入 Free / OP / Spare 池（唯一入池点）。
 *
 * @param ftl  FTL 实例（调用前 free/op/spare 计数必须为 0）
 * @param tbl  已对账的块表（必须已 probe）
 *
 * @return true  = 全部块归位；
 *         false = OP/Spare 池溢出或遇到非池块（调用者回退 full metadata rebuild）。
 */
static BOOL pfr_build_pools(ftl_ctx_s *ftl, const pfr_block_table_s *tbl)
{
    alc_s *alloc = &ftl->alloc;

    ASSERT(tbl->probed);

    const WORD *words = tbl->word;

    // 此时不应有池条目（restore 不发、replay/probe 只建 Closed）。
    ASSERT(alloc->free_count == 0U);
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        ASSERT(alloc->op_count[p] == 0U);
        ASSERT(alloc->spare_count[p] == 0U);
    }

    WORD good = 0U;

    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        if (pfr_block_is_fixed(g))
        {
            continue;
        }

        bst_block_s *b = &alloc->blocks[g];

        if (ftl_block_is_bad(b))
        {
            continue;
        }

        const BYTE plane = ftl_gblock_plane(g);
        const WORD block = ftl_gblock_block(g);

        if (md_block_is_metadata(ftl->metadata_dir, plane, block))
        {
            continue; /* 固定区（已在上面跳过）/ 活的动态 LOG 块 */
        }

        good++;

        if (ftl_block_state(b) != FTL_BLOCK_STATE_NONE)
        {
            continue; /* CLOSED / RETIRED / 陈旧 LOG（已由 probe 处理） */
        }

        const WORD w    = words[g];
        const BYTE pool = cp_block_word_pool(w);
        const BYTE st   = cp_block_word_state(w);

        if ((st == FTL_BLOCK_STATE_FREE) && (pool == CP_BLOCK_POOL_NONE))
        {
            alc_free_push(alloc, g);
        }
        else if (pool == CP_BLOCK_POOL_OP)
        {
            if (alc_op_push(alloc, plane, g) == false)
            {
                LOGE(LOG_TAG_BLK "OP pool overflow at g=%hu\r\n", g);
                return false;
            }
        }
        else if (pool == CP_BLOCK_POOL_SPARE)
        {
            if (alc_spare_push(alloc, plane, g) == false)
            {
                LOGE(LOG_TAG_BLK "Spare pool overflow at g=%hu\r\n", g);
                return false;
            }
        }
        else
        {
            LOGE(LOG_TAG_BLK "g=%hu state=0x%04hx not a pool block\r\n", g, w);
            return false;
        }
    }

    alloc->total_blocks = good;
    return true;
}

/**
 * @brief 清空 allocator 的块状态与池账本（保留 BAD 终态）。
 *
 * 把非 BAD 块置 NONE 并清 valid/temp/sb，同时把 free/op/spare/closed 计数与
 * hot/cold SB 归零；BAD 来自 BBT，是终态，不清。
 *
 * @param ftl FTL 实例
 */
static void pfr_reset_blocks(ftl_ctx_s *ftl)
{
    alc_s *alloc = &ftl->alloc;

    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        bst_block_s *b = &alloc->blocks[g];

        if (ftl_block_is_bad(b))
        {
            continue; /* BAD 是终态（来自 BBT），保留 */
        }
        ftl_block_set_state(b, FTL_BLOCK_STATE_NONE);
        ftl_block_clear_valid(b);
        ftl_block_set_temp(b, 0U);
        ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
    }

    alloc->free_count = 0U;
    memset(alloc->free_plane_count, 0, sizeof(alloc->free_plane_count));
    memset(alloc->op_count, 0, sizeof(alloc->op_count));
    memset(alloc->spare_count, 0, sizeof(alloc->spare_count));
    alloc->closed_sb_count = 0U;
    alloc->total_blocks    = 0U;

    memset(&alloc->hot_sb, 0, sizeof(alloc->hot_sb));
    memset(&alloc->cold_sb, 0, sizeof(alloc->cold_sb));
    alloc->hot_sb.temperature  = FTL_TEMP_HOT;
    alloc->cold_sb.temperature = FTL_TEMP_COLD;
}

/**
 * @brief jrn_replay 的 materialize 回调：把 PBA 所属块转成「可记账」状态。
 * 池块（FREE/OP/SPARE）⇒ Closed；BAD 忽略；固定/活 LOG 块 ⇒ false（回放不一致）。
 *
 * @param ctx replay 上下文（pfr_replay_ctx_s *）
 * @param pba journal 条目中的物理块地址（来自介质，先校验合法性与实例归属）
 *
 * @return true  = 块已处于可记账状态（含 BAD 块可忽略）；
 *         false = PBA 非法 / 不属于本实例 / 指向固定或活 LOG 块 / 状态非法 /
 *                 Closed 列表满（回放不一致）。
 */
static BOOL pfr_materialize_pba(void *ctx, DWORD pba)
{
    ftl_ctx_s *ftl = ((pfr_replay_ctx_s *)ctx)->ftl;

    // journal 条目来自介质（H29 不可信输入）：先校验 PBA 属于本实例且字段合法，
    // 否则会把 valid_count 记到别的块，而 L2P 存下一个越界地址。
    if ((pba_is_valid(pba) == false) || (pba_to_ch(pba) != ftl->topo.ch) || (pba_to_ce(pba) != ftl->topo.ce) || (pba_to_lun(pba) != ftl->topo.lun))
    {
        return false; /* block_ctx.ok=false ⇒ 调用者回退 full metadata rebuild */
    }

    alc_s     *alloc = &ftl->alloc;
    const WORD g     = ftl_pba_gblock(pba);

    if (g >= FTL_BLOCK_TOTAL || pfr_block_is_fixed(g))
    {
        return false;
    }

    bst_block_s *b = &alloc->blocks[g];

    if (ftl_block_is_bad(b))
    {
        return true; /* 坏块不记账；数据已不可信 */
    }
    if (md_block_is_metadata(ftl->metadata_dir, ftl_gblock_plane(g), ftl_gblock_block(g)))
    {
        return false; /* journal 不应指向 L2P 日志块 */
    }

    switch (ftl_block_state(b))
    {
        case FTL_BLOCK_STATE_CLOSED:
        case FTL_BLOCK_STATE_RETIRED:
        case FTL_BLOCK_STATE_OPEN:
        case FTL_BLOCK_STATE_BAD:
            return true;

        case FTL_BLOCK_STATE_LOG:
            // 快照说 LOG、但 Directory 已不引用（上面的 md_block_is_metadata 为假）
            // ⇒ 该块在 CP 后被回收并复用；journal 指向它就是新用户数据。
            // 按陈旧 LOG 处理：转 Closed（旧 L2P 页交给 GC 擦除）。
            ftl_block_set_state(b, FTL_BLOCK_STATE_CLOSED);
            ftl_block_clear_valid(b);
            ftl_block_set_temp(b, FTL_TEMP_HOT);
            ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
            return pfr_closed_sb_add(ftl, g, FTL_TEMP_HOT);

        case FTL_BLOCK_STATE_FREE:
        case FTL_BLOCK_STATE_NONE:
            // 池块在 CP 后被写入：立即转 Closed，valid_count 从 0 起，
            // 后续 delta 由 journal replay 累加。
            ftl_block_set_state(b, FTL_BLOCK_STATE_CLOSED);
            ftl_block_clear_valid(b);
            ftl_block_set_temp(b, FTL_TEMP_HOT);
            ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
            return pfr_closed_sb_add(ftl, g, FTL_TEMP_HOT);

        default:
            return false;
    }
}

/**
 * @brief jrn_replay 的 valid_count 增量端口（带守卫：越界/上溢置 *ok=false）。
 * materialize 已保证该块处于可记账状态；这里只做范围检查与 BST 单数据源更新。
 *
 * @param ctx replay 上下文（pfr_replay_ctx_s *）
 * @param pba 物理块地址
 * @param ok  出参：块号越界或 valid_count 上溢时置 false，否则不改
 */
static void pfr_valid_add(void *ctx, DWORD pba, BOOL *ok)
{
    ftl_ctx_s *ftl = ((pfr_replay_ctx_s *)ctx)->ftl;

    const WORD g = ftl_pba_gblock(pba);

    if (g >= FTL_BLOCK_TOTAL)
    {
        *ok = false;
        return;
    }

    bst_block_s *b = &ftl->alloc.blocks[g];

    if (ftl_block_is_bad(b))
    {
        return;
    }
    if (b->valid_count >= FTL_CW_PER_BLOCK)
    {
        *ok = false;
        return;
    }
    ftl_block_add_valid(b);
}

/**
 * @brief jrn_replay 的 valid_count 递减端口（带守卫：越界/下溢置 *ok=false）。
 *
 * @param ctx replay 上下文（pfr_replay_ctx_s *）
 * @param pba 物理块地址
 * @param ok  出参：块号越界时置 false，否则不改（减到 0 只计数，不置 false）
 */
static void pfr_valid_sub(void *ctx, DWORD pba, BOOL *ok)
{
    pfr_replay_ctx_s *rctx  = (pfr_replay_ctx_s *)ctx;
    ftl_ctx_s        *ftl   = rctx->ftl;
    pfr_stats_s      *stats = rctx->stats;

    const WORD g = ftl_pba_gblock(pba);

    if (g >= FTL_BLOCK_TOTAL)
    {
        *ok = false;
        return;
    }

    bst_block_s *b = &ftl->alloc.blocks[g];

    if (ftl_block_is_bad(b))
    {
        return;
    }
    if (b->valid_count == 0U)
    {
        // 与运行期 `ftl_block_sub_valid_if_live`（bst.h）**同口径**：计数已为 0 ⇒
        // 无可减，跳过。原来这里判硬失败，等于把「一条陈旧映射」升级成「整份块元数据
        // 快照作废 ⇒ full rebuild ⇒ 丢映射」（放大器）。
        // 真正该作废快照的是「目录仍引用该块」那种，由回放后的不变量守卫判定。
        if (stats->sub_zero == 0U)
        {
            stats->sub_zero_g     = g;
            stats->sub_zero_state = (BYTE)ftl_block_state(b);
        }
        if (stats->sub_zero < UINT32_MAX)
        {
            stats->sub_zero++;
        }
        return;
    }
    ftl_block_sub_valid(b);
}

/* ══════════════════════════════════════════════════════════════════
 * 介质布局版本 —— 开机自动格式化
 * ══════════════════════════════════════════════════════════════════ */

/**
 * @brief 开机格式化判据：决定是否需要格式化
 *
 * IO_ERROR ⇒ 不格式化、返回错误（一次瞬时读错不能变成整片擦除）。
 *
 * @param ftl             实例
 * @param out_need_format 输出：是否需要格式化
 * @return FTL_OK；FTL_ERROR 介质不可读
 */
ftl_status_e pfr_media_check_format(ftl_ctx_s *ftl, BOOL *out_need_format)
{
    DWORD             media_version = 0U;
    cp_media_status_e st            = cp_media_status_probe(ftl->cp_handle, &media_version);

    switch (st)
    {
        case CP_MEDIA_CURRENT:
            *out_need_format = false;
            return FTL_OK;

        case CP_MEDIA_NO_RECORD:
        case CP_MEDIA_VERSION_MISMATCH:
            LOGI(LOG_TAG_BOOT "format required: media_version=%lu current=%lu\r\n", media_version, FTL_MEDIA_LAYOUT_VERSION);
            *out_need_format = true;
            return FTL_OK;

        case CP_MEDIA_IO_ERROR:
        default:
            LOGE(LOG_TAG_BOOT "CP media unreadable — refuse to format\r\n");
            *out_need_format = false;
            return FTL_ERROR;
    }
}

/**
 * @brief 全片格式化（两阶段擦除 + initial CP）
 *
 * 必须在 ftl_init 之后、free pool 回填之前调用。
 *
 * @param ftl    实例
 * @param verify 是否做全片擦除体检
 * @return FTL_OK；FTL_ERROR 格式化失败
 */
ftl_status_e pfr_media_format(ftl_ctx_s *ftl, BOOL verify)
{
    // 格式化是破坏性控制路径：先丢弃残留 cache 序列并强制打开门控。否则
    // active/draining 会让 SYNC_WAIT 的擦写提交被 `ftl_op_seq_gate` 拒绝，
    // 整片格式化退化为 4096 次同步超时（每次约 20 ms）。
    ftl_cache_seq_discard(ftl);

    // 旧 L2P flush 登记属于擦除前的介质状态，格式化后不得再提交。
    ftl_sched_l2p_flush_reset(&ftl->sched);

    // 两阶段擦除：BBT 先在擦用户块之前落盘；若这一步失败，用户数据与工厂标记
    // 都还完整，格式化不完成、可重试。
    if (bbt_format_erase(&ftl->alloc) != FTL_OK)
    {
        LOGE(LOG_TAG_BBT "format: pre-erase BBT save failed — 未擦数据，可重试\r\n");
        return FTL_ERROR;
    }

    if (verify)
    {
        // 从零开始的全片体检：
        // ① 清空 allocator 池（否则清 BAD 后失败块重新标坏会撞 FREE 断言）；
        // ② 清空非 BBT-pool 的 BAD 集（保留 BBT 池里本轮 record 的坏块状态）；
        // ③ 测试**所有**非 BBT-pool 块（包括 BST 中标记 BAD 的块）。
        pfr_reset_blocks(ftl);

        BYTE old_bad[BBT_BITMAP_SIZE];

        pfr_format_reset_bad_map(&ftl->alloc, old_bad);

        if (pfr_format_verify_blocks(ftl) != FTL_OK)
        {
            return FTL_ERROR;
        }

        // 观测判据：旧 BAD 里有多少被本次体检判为好块（recovered），以及新 BAD 总数。
        DWORD old_bad_n = 0U;
        DWORD recovered = 0U;
        DWORD new_bad   = 0U;

        for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
        {
            const BOOL now_bad = ftl_block_is_bad(&ftl->alloc.blocks[g]);

            if (bbt_is_bad(old_bad, g))
            {
                old_bad_n++;
                if (now_bad == false)
                {
                    recovered++;
                }
            }
            if (now_bad)
            {
                new_bad++;
            }
        }

        LOGI(LOG_TAG_FLT "format --verify: full re-scan old_bad=%lu recovered=%lu new_bad=%lu\r\n", old_bad_n, recovered, new_bad);
    }

    // 用 verify 后的最终 BAD 集重建候选池（BBT/CP/Journal 可能从 shared spare 补块）。
    // 失败 ⇒ 元数据区已无足够好块，格式化必须失败（release 拒绝，判据 9）。
    if (md_pool_build_all(&ftl->alloc) == false)
    {
        LOGE(LOG_TAG_FTL "format: metadata candidate pool build failed\r\n");
        return FTL_ERROR;
    }

    // BBT 候选可能因 verify 新发现的 BAD 改变 ⇒ 强制 bbt_save 在新池上重新定位，
    // 把最终 BAD 图落到新的 BBT 候选块；否则下次开机扫旧候选可能读不到最新 record。
    ftl->alloc.bbt.cursor_valid = false;
    bbt_dirty_note(&ftl->alloc);

    if (bbt_save(&ftl->alloc) != FTL_OK)
    {
        LOGE(LOG_TAG_FTL "format: BBT re-save on new candidate pool failed\r\n");
        return FTL_ERROR;
    }

    md_init(ftl->metadata_dir, &ftl->alloc, &ftl->stats.total_writes);

    // 空 Directory 必须先落目录检查点，否则下次开机 CP 存在但 Directory 加载失败。
    if (pfr_format_write_initial_dir_ckpt(ftl) != FTL_OK)
    {
        LOGE(LOG_TAG_META "format: initial directory checkpoint write failed\r\n");
        return FTL_ERROR;
    }

    // 先清 BST/池（保留 BAD）：format 可能从“运行中 FTL”或 fallback 状态调用，
    // 不能假设池是空的；carve 的前置是 free/op/spare 计数全 0。
    pfr_reset_blocks(ftl);

    // 按唯一 carve 规则填池：initial CP 的 per-block snapshot 必须与
    // pfr_build_pools() 的 Free/OP/Spare 契约逐项一致。
    pfr_boot_carve_pools(ftl);

    // 完整 21 页 initial CP：块元数据 + 磨损快照 + Header（最后 commit）。
    if (cp_initial_record_write(ftl) != FTL_OK)
    {
        return FTL_ERROR;
    }

    // 安全回读：初始 Checkpoint 是否真的可读。候选池已跳过 BAD，但若候选耗尽/
    // 写点落错，Header 仍可能“写成功、读失败”。当场读回，避免 format 打印 done、
    // 下次开机却拒绝启动。
    {
        cp_header_s *probe     = (cp_header_s *)ftl->cp_handle->hdr.hdr_buf; /* handle 常驻 2KB */
        WORD         probe_idx = UINT16_MAX;

        if (cp_pool_find_latest(ftl->cp_handle, probe, &probe_idx) != FTL_OK)
        {
            LOGE(LOG_TAG_FTL "format: CP readback failed (check ECC/pool/write point)\r\n");
            return FTL_ERROR;
        }
    }

    // 提交点：initial CP 已可读 ⇒ 清 FORMAT_IN_PROGRESS 并落盘。此后掉电，
    // 下次开机按「干净格式化」启动。落盘失败则保持「格式化中」，重做。
    if (pfr_format_end_bbt(&ftl->alloc) == false)
    {
        LOGE(LOG_TAG_BBT "format: 清 FORMAT_IN_PROGRESS 落盘失败 — 下次开机会重新格式化\r\n");
        return FTL_ERROR;
    }

    // 同一 boot 随后会跑 pfr_boot_recovery；它从刚写的快照恢复，
    // 因此这里必须把 carve 结果清回空池（保留 BAD），避免 build_pools 的前置冲突。
    pfr_reset_blocks(ftl);

    // 格式化后 Directory 就是全新的「全部未映射」，之前的加载失败不再有意义。
    ftl->flags.md_load_failed = false;

    return FTL_OK;
}

/**
 * @brief 掉电恢复唯一入口：扫描 → 判型 → 执行
 *
 * @param ftl    实例
 * @param result 输出：恢复结果
 * @return FTL_OK；FTL_ERROR 恢复失败
 */
ftl_status_e pfr_boot_recovery(ftl_ctx_s *ftl, pfr_boot_result_s *result)
{
    pfr_boot_scan_s scan;

    if (pfr_boot_scan(ftl, &scan) == false)
    {
        return FTL_ERROR;
    }

    const pfr_plan_e plan = pfr_boot_plan(&scan);

    pfr_boot_execute(ftl, plan, &scan, result);
    return FTL_OK;
}

/**
 * @brief 统计各分区的坏块与 reserved 数并打一行日志
 *
 * @param ftl 实例（只读）
 */
void pfr_boot_log_bad_areas(const ftl_ctx_s *ftl)
{
    // 目录检查点是分区表的第一段（基址必须是 0）：这样第一个区间只需判上界，
    // 避免 `block >= 0` 的恒真比较告警。
    _Static_assert(DIR_CKPT_BLOCK0 == 0U, "bad-area stats 假定元数据分区从块 0 开始");

    DWORD dir_ckpt = 0U;
    DWORD cp       = 0U;
    DWORD bbt      = 0U;
    DWORD journal  = 0U;
    DWORD l2p_log  = 0U;
    DWORD spare    = 0U;
    DWORD data     = 0U;

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        for (WORD block = 0U; block < BLOCKS_PER_PLANE; block++)
        {
            const WORD g = ftl_gblock(plane, block);

            if (ftl_block_is_bad(&ftl->alloc.blocks[g]) == false)
            {
                continue;
            }

            if (block < (DIR_CKPT_BLOCK0 + DIR_CKPT_POOL_BLOCKS))
            {
                dir_ckpt++;
            }
            else if ((block >= CHECKPOINT_BLOCK0) && (block < (CHECKPOINT_BLOCK0 + CHECKPOINT_POOL_BLOCKS)))
            {
                cp++;
            }
            else if ((block >= BBT_BLOCK) && (block < (BBT_BLOCK + BBT_POOL_BLOCKS)))
            {
                bbt++;
            }
            else if ((block >= JOURNAL_BLOCK0) && (block < (JOURNAL_BLOCK0 + JOURNAL_BLOCK_COUNT)))
            {
                journal++;
            }
            else if ((block >= L2P_LOG_BLOCK0) && (block < L2P_LOG_AREA_END))
            {
                l2p_log++;
            }
            else if (ftl_is_metadata_spare_block(block))
            {
                spare++;
            }
            else
            {
                data++;
            }
        }
    }

    // 只读 BST；不写介质。总量 = bad_count（BBT 与快照并集后的基线）。
    LOGI(LOG_TAG_BLK "bad areas ch=%hhu ce=%hhu lun=%hhu: dir_ckpt=%lu cp=%lu bbt=%lu journal=%lu l2p_log=%lu spare=%lu data=%lu total=%lu\r\n", ftl->topo.ch, ftl->topo.ce, ftl->topo.lun, dir_ckpt, cp, bbt, journal, l2p_log, spare, data, dir_ckpt + cp + bbt + journal + l2p_log + spare + data);
}

#if defined(FC_NAND_TEST_ENABLE)
BYTE pfr_test_scan_write_pointer(const ftl_ctx_s *ftl, BYTE plane, WORD block)
{
    return pfr_scan_write_pointer(ftl, plane, block);
}
#endif

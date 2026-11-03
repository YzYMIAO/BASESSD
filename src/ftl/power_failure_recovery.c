/*
 * power_failure_recovery.c — 掉电恢复（启动三条路径：fresh / clean / power-failure）
 *
 * 职责：
 *   开机时判定上次关机形态并据此恢复 RAM 状态——扫 Checkpoint 取最近快照，
 *   扫 Journal 取最后 sequence，二者比较决定走哪条路径；以及掉电重建
 *   Closed SB 列表（full metadata rebuild，全 NAND 扫描）与写指针扫描。
 *
 * 与 checkpoint.c / journal.c 的分工：
 *   checkpoint.c —— Checkpoint 的写与查找（cp_pool_find_latest）；
 *   journal.c    —— Journal 的写与回放（jrn_replay）；
 *   本文件       —— 只做**编排**：把两者拼成三条启动路径，自身不解析介质格式。
 *
 * 三条启动路径（状态机）：
 *   BOOT_FRESH_START    无有效 Checkpoint（出厂首次启动）→ cache INVALID + full metadata rebuild → carve
 *   BOOT_CLEAN_START    journal 全被 CP 覆盖（干净关机）
 *                       → 块元数据快照有效：restore + probe + build_pools（**不跑 full metadata rebuild**）
 *                         否则 full metadata rebuild → carve
 *   BOOT_POWER_FAILURE  journal 有未回放条目（异常掉电）
 *                       → 块元数据快照 restore + jrn_replay（映射 + valid 增量）+ F4
 *                         + probe + build_pools；失败 ⇒ reset → full metadata rebuild → carve
 *   ★ full metadata rebuild 只在「无快照 / 快照损坏 / 增量记账不一致」时兜底。
 *
 * 文件结构：
 *   1. 介质版本 / 格式化   pfr_media_check_format / pfr_format_write_initial_dir_ckpt /
 *                          pfr_format_reset_bad_map / pfr_format_verify_blocks /
 *                          pfr_format_end_bbt / pfr_media_format
 *   2. 启动恢复            pfr_boot_recovery（总入口）/ pfr_boot_carve_pools /
 *                          pfr_refill_spare_pools / pfr_boot_fresh_start /
 *                          pfr_boot_apply_region_bitmap / pfr_boot_clean_start /
 *                          pfr_boot_power_failure / pfr_boot_log_bad_areas
 *   3. full metadata rebuild   pfr_block_is_fixed / pfr_closed_sb_add（共享）/
 *                          pfr_rebuild_l2p_stats_* / pfr_scan_write_pointer /
 *                          pfr_close_block / pfr_rebuild_closed_sb_list
 *   4. 块元数据快照恢复    pfr_restore_blocks / pfr_probe_blocks /
 *                          pfr_build_pools / pfr_reset_blocks
 *   5. journal replay 端口 pfr_materialize_pba / pfr_valid_add / _sub
 *
 * ⚠ 本文件依赖 ftl_init 已完成 allocator / journal / metadata_dir 的初始化；
 *   各路径步骤用圈码 ①②③ 编号（与 docs/design/ftl_design.md §8 对应）。
 *
 * ⚠ 入口前置守卫口径（docs/design/assertion_guidelines.md §二十六 / §二十八）：
 *   对外入口（pfr_media_check_format / pfr_media_format / pfr_boot_recovery）的 ftl/result
 *   非空由 main.c 的调用链保证；三条启动路径与其内部 helper 只在本文件的启动恢复调用链内
 *   使用，该处的 ftl 非空已由入口保证 ⇒ **不再逐函数复检**。
 */

#include "power_failure_recovery.h"

#include "assertion.h"
#include "bbt.h"
#include "checkpoint.h"
#include "fc.h"
#include "fc_crc_engine.h" /* FC_CRC_META_SIZE */
#include "ftl.h"
#include "ftl_fc_interface.h"
#include "gc.h"
#include "log.h"
#include "metadata.h"
#include "onfi_1_0.h"

#include <string.h>

/* ── 文件级常量 ── */

// page0 空白时补探的页数 = cache program 批次上限（4 页）。
// 超出该范围的 page0 空洞依赖 page0 异步写屏障（allocator）保证。
#define RECOVERY_PAGE0_PROBE_PAGES (4U)

/* ── 文件级静态数据 ── */

// rebuild / restore 扫描期的 L2P 异常汇总（每轮扫描一行）。
// pfr_close_block 每扫到一个写过块就累加；逐块/逐 CW 打印会写满 UART TX 环
// 并拖慢开机（RISK-JN-05）。计数是文件级：两条扫描入口各自清零、结束时汇总；
// boot 路径单线程、多实例顺序执行。
static DWORD s_rebuild_read_fail_cw;
static DWORD s_rebuild_degraded_cw;
static DWORD s_rebuild_first_region;

/**
 * @brief PFR 内部恢复原语（full metadata rebuild / fresh / clean / PF）。只在本文件使用 ⇒ static，
 * 上层只可见 pfr_media_check_format / pfr_media_format / pfr_boot_recovery
 * （docs/archive/ftl_module_contracts.md §5「PFR boot orchestrator」）。
 * 定义在后续各分节；本文件按「入口/编排在前、机制在后」组织，跨分节调用在此统一前置声明。
 */
static void pfr_boot_carve_pools(ftl_ctx_t *ftl);
static void pfr_boot_fresh_start(ftl_ctx_t *ftl, const jrn_page_ref_t *ring);
static void pfr_boot_clean_start(ftl_ctx_t *ftl, const cp_header_t *cp, BYTE blocks_valid, bool force_rebuild, bool *blocks_restored, const jrn_page_ref_t *ring);
static void pfr_boot_power_failure(ftl_ctx_t *ftl, const cp_header_t *cp, BYTE blocks_valid, pfr_boot_result_t *result);
static void pfr_rebuild_closed_sb_list(ftl_ctx_t *ftl, pfr_rebuild_stats_t *stats);
static bool pfr_restore_blocks(ftl_ctx_t *ftl, WORD words[FTL_BLOCK_TOTAL]);
static void pfr_probe_blocks(ftl_ctx_t *ftl, WORD words[FTL_BLOCK_TOTAL], pfr_block_restore_stats_t *stats);
static bool pfr_build_pools(ftl_ctx_t *ftl, const WORD words[FTL_BLOCK_TOTAL]);
static void pfr_reset_blocks(ftl_ctx_t *ftl);

/**
 * @brief journal replay 端口（定义在文件末尾）。
 */
static bool pfr_materialize_pba(void *ctx, DWORD pba);
static void pfr_valid_add(void *ctx, DWORD pba, bool *ok);
static void pfr_valid_sub(void *ctx, DWORD pba, bool *ok);

/* 回放期「减到 0 以下」的现场（RISK-MD-06）：次数 + 首个 gblock 与它的**快照状态**。
 * FREE/NONE = 该块在 CP 快照里就在池里（journal 的 old 指向已归还块）；
 * LOG       = 指向 L2P 日志块；CLOSED  = 已关闭但计数为 0。 */
static WORD s_sub_zero_n;
static WORD s_sub_zero_g = UINT16_MAX;
static BYTE s_sub_zero_st;

/* ══════════════════════════════════════════════════════════════════
 * 介质布局版本 —— 开机自动格式化
 * ══════════════════════════════════════════════════════════════════ */

// 实现要点：判据来自 cp_media_status_probe：
// · CURRENT            ⇒ 不格式化；
// · NO_RECORD / 版本不符 ⇒ 格式化（空片多擦一次，写标记后自愈）；
// · IO_ERROR            ⇒ 不格式化，返回错误——一次瞬时读错不能变成整片擦除。
ftl_status_e pfr_media_check_format(ftl_ctx_t *ftl, bool *out_need_format)
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
 * @brief 把格式化后的**空 Directory** 落成一条目录检查点记录。
 *
 * @param ftl FTL 实例（使用 metadata_dir 与 cp_handle 的 2KB 常驻缓冲）
 *
 * @return FTL_OK / FTL_ERROR。
 *
 * @note 实现要点：`md_init` 只复位 RAM；不落盘的话下次开机
 * md_load 失败，而 initial Checkpoint（seq=1）存在 ⇒
 * pfr_boot_recovery 按 RISK-BOOT-09 拒绝启动。故必须先把空 Directory
 * 写成目录检查点，再写 initial Checkpoint（后者是整条 record 的 commit）。
 */
static ftl_status_e pfr_format_write_initial_dir_ckpt(ftl_ctx_t *ftl)
{
    md_dir_cache_t           *dir = ftl->metadata_dir;
    md_dir_ckpt_prepare_out_t dir_ckpt;

    dir->ckpt.dirty = true; /* 空 Directory 也要落一条 record */

    if (md_dir_ckpt_prepare(dir, &dir_ckpt, MD_DIR_CKPT_OWNER_BOOT) != MD_DIR_CKPT_READY)
    {
        return FTL_ERROR;
    }

    if (dir_ckpt.need_erase && (ftl_sync_erase_block(dir_ckpt.base) != FTL_OK))
    {
        md_dir_ckpt_abort(dir, MD_DIR_CKPT_OWNER_BOOT); /* 放弃已 prepare 的目录检查点写（清单飞锁，RISK-MD-06） */
        return FTL_ERROR;
    }

    bool ok = true;

    for (BYTE chunk = 0U; chunk < DIR_CKPT_PAGE_COUNT; chunk++)
    {
        // 2KB 目录检查点页常驻 handle（与 CP 共用 scratch；format 路径串行，无重入）。
        md_dir_ckpt_page_t *page          = (md_dir_ckpt_page_t *)ftl->cp_handle->hdr.hdr_buf;
        const DWORD         dir_ckpt_page = pba_to_page(dir_ckpt.base) + chunk;
        const DWORD         addr          = pba_with_page(dir_ckpt.base, dir_ckpt_page);

        md_dir_ckpt_page(dir, dir_ckpt.seq, chunk, page);
        if (ftl_sync_write_page(addr, page, sizeof(*page), FTL_PAGE_CFG_META()) != FTL_OK)
        {
            ok = false;
            break;
        }
    }

    // 安全回读：候选池已跳过 BAD，但配置错误/候选耗尽仍可能让写点落在不可读块上；
    // 编程状态可能 OK 而重启读不回来 ⇒ format 谎报成功。这里当场读回第一页。
    if (ok)
    {
        md_dir_ckpt_page_t *probe = (md_dir_ckpt_page_t *)ftl->cp_handle->hdr.hdr_buf;

        if ((ftl_sync_read_page(dir_ckpt.base, probe, sizeof(*probe), FTL_PAGE_CFG_META()) != FTL_OK) || (probe->magic != DIR_CKPT_MAGIC) || (probe->sequence != dir_ckpt.seq) || (probe->layout_version != FTL_MEDIA_LAYOUT_VERSION))
        {
            LOGE(LOG_TAG_META "format: directory checkpoint readback failed — fixed metadata block may be BAD\r\n");
            ok = false;
        }
    }

    md_dir_ckpt_commit(dir, &dir_ckpt.ring, dir_ckpt.idx, ok);
    return ok ? FTL_OK : FTL_ERROR;
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
static void pfr_format_reset_bad_map(alc_t *alloc, BYTE old_bad[BBT_BITMAP_SIZE])
{
    const md_ring_t *bbt_ring = (alloc->pools != NULL) ? &alloc->pools->bbt : NULL;
    WORD             kept     = 0U;

    (void)memset(old_bad, 0, BBT_BITMAP_SIZE);

    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        const WORD block  = ftl_gblock_block(g);
        const bool is_bad = ftl_block_is_bad(&alloc->blocks[g]);
        const bool keep   = bbt_block_is_candidate(bbt_ring, block);

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
 * 「通过」不能证明运行期没问题（RISK-NAND-06），收益与耗时不成比例。
 * ⚠ 调用前由 pfr_format_reset_bad_map 清空非 BBT 池的 BAD 集 ⇒ 本函数重新评估
 * **所有**非 BBT-pool 块（包括 BST 中标记为 BAD 的块）：擦除成功即 recovered。
 * BBT 池跳过（本轮坏块 record 在里面）；体检后所有块保持擦净状态。
 *
 * @param ftl FTL 实例（使用 alloc 块表与 BBT；调用前 BAD 集已由 pfr_format_reset_bad_map 清空）
 *
 * @return FTL_OK（体检完成，个别块已标坏）
 *         FTL_ERROR = BBT 落盘失败。
 */
static ftl_status_e pfr_format_verify_blocks(ftl_ctx_t *ftl)
{
    alc_t *alloc  = &ftl->alloc;
    DWORD  tested = 0U;
    DWORD  bad    = 0U;

    LOGW(LOG_TAG_FLT "format --verify: 全片擦除体检开始（擦除成功即判好块）\r\n");

    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        const BYTE plane = ftl_gblock_plane(g);
        const WORD block = ftl_gblock_block(g);

        // BBT 候选池保留：本轮坏块图就在里面（候选可能含 shared metadata spare）。
        {
            const md_ring_t *bbt_ring = (alloc->pools != NULL) ? &alloc->pools->bbt : NULL;
            const bool       keep_bbt = bbt_block_is_candidate(bbt_ring, block);

            if (keep_bbt)
            {
                continue;
            }
        }

        // 唯一判据：擦除命令成功。carve 假定 Free 块是空白的，擦成功即为空白。
        const DWORD epba = pba_make(alloc->ch, alloc->ce, alloc->lun, block, plane, 0U, 0U);
        const bool  ok   = (ftl_sync_erase_block(epba) == FTL_OK);

        tested++;

        if (ok == false)
        {
            bad++;
            LOGE(LOG_TAG_FLT "format --verify: gblock=%hu (plane=%hhu block=%hu) failed — mark BAD\r\n", g, plane, block);
            (void)bbt_mark_bad(alloc, g);
        }

        if ((tested % 512U) == 0U)
        {
            LOGI(LOG_TAG_FLT "format --verify: %lu/%d blocks, bad=%lu\r\n", tested, FTL_BLOCK_TOTAL, bad);
        }
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
static bool pfr_format_end_bbt(alc_t *alloc)
{
    alloc->bbt.format_in_progress = false;

    // 格式化刚写完全新的 initial CP ⇒ 没有任何「丢失映射需要重建」的旧状态。
    // verify 期间标记 Journal/L2P 坏块可能置过 REBUILD_REQUIRED，这里一并清掉，
    // 否则下次开机会在刚格式化完的介质上白跑一次 full rebuild。
    alloc->bbt.rebuild_required = false;

    bbt_dirty_note(alloc);

    return bbt_save(alloc) == FTL_OK;
}

// 实现要点：① `bbt_format_erase` 两阶段：**先落 BBT**（当前坏块图 +
// FORMAT_IN_PROGRESS），再擦除 BBT 池之外的块（擦失败标 BAD）；
// ② `md_init` 把 Directory 复位为"全部未映射"；
// ③ `cp_initial_record_write` 写当前版本标记 ⇒ 下次开机版本判据命中，
// 格式化只发生一次；
// ④ **提交点**：initial CP **读回成功**后才清 FORMAT_IN_PROGRESS。
// 中间任一步（含 format --verify 的数分钟体检）复位/掉电，
// 下次开机都会继续格式化，而不是拒绝启动。
// ⚠ 必须在 ftl_init 之后、free pool 回填之前调用（见 main.c ssd_ftl_boot）。
// ⚠ BST 的 erase_count 保留旧值（开机已从 CP 磨损快照回填）——仅影响磨损均衡起点。
ftl_status_e pfr_media_format(ftl_ctx_t *ftl, bool verify)
{
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
            const bool now_bad = ftl_block_is_bad(&ftl->alloc.blocks[g]);

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
        cp_header_t *probe     = (cp_header_t *)ftl->cp_handle->hdr.hdr_buf; /* handle 常驻 2KB */
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

// 实现要点：① 扫 Journal 环形块池取最新页（jrn_page_scan_latest）——**只读不改状态**，
// 故不调 jrn_replay（后者会修改 journal 内部状态）。三条启动路径
// 都要拿这份快照恢复 Journal RAM 写态（jrn_resume）。
// ② cp_pool_find_latest 取最近快照，找不到 = 首次启动走 fresh_start。
// ③ 最新页 page_seq ≤ cp->journal_sequence ⇒ 干净关机；否则异常掉电。
static ftl_status_e pfr_boot_recovery_body(ftl_ctx_t *ftl, pfr_boot_result_t *result)
{
    pfr_boot_result_t local_result;
    memset(&local_result, 0, sizeof(local_result));

    // ① 扫 Journal 环形块池取最新页（只读）。
    // 判型要用它；三条路径也都要用它恢复写指针——jrn_init 把写指针/序号复位到
    // (block0, page0)/1，而环形块**并没有被擦**（journal_truncate 只预擦环形下一块），
    // 不接回去就会二次编程 + 序号回退（详见 jrn_resume）。
    jrn_page_ref_t ring;
    bool           ring_read_error = false;
    (void)jrn_page_scan_latest(ftl->journal, &ring, &ring_read_error);

    // ② 扫描 Checkpoint + 读取 块元数据快照（快照失败不触发格式化，只影响块元数据路径）
    cp_header_t *cp     = (cp_header_t *)ftl->cp_handle->hdr.hdr_buf; /* handle 常驻 2KB */
    WORD         cp_idx = UINT16_MAX;

    if (cp_pool_find_latest(ftl->cp_handle, cp, &cp_idx) != FTL_OK)
    {
        // 无有效 Checkpoint → 出厂首次启动。空 Directory 是预期状态。
        ftl->flags.md_load_failed = false;

        pfr_boot_fresh_start(ftl, &ring);

        local_result.boot_type       = BOOT_FRESH_START;
        local_result.cp_sequence     = 0U;
        local_result.journal_entries = 0U;
        local_result.blocks_restored = false;
        *result                      = local_result;
        return FTL_OK;
    }

    // 运行期 Journal/L2P 日志块丢失后置的 REBUILD_REQUIRED（本仓自拟）：
    // 快照不可信，强制走 full metadata rebuild；新 CP 落盘后才清标志（见
    // cp_hdr_write_complete）。此时即使 Directory 加载失败也继续——重建本身
    // 就不依赖旧 Directory。
    if (alc_bbt_is_rebuild_required(&ftl->alloc))
    {
        LOGE(LOG_TAG_BOOT "rebuild required (metadata log lost) — force full metadata rebuild\r\n");

        bool blocks_restored = false;

        pfr_boot_clean_start(ftl, cp, 0U, true, &blocks_restored, &ring);

        // 让首个 CP 尽快提交重建后的状态并清标志。
        jrn_checkpoint_request(ftl->journal);

        ftl->flags.md_load_failed    = false;
        local_result.boot_type       = BOOT_CLEAN_START;
        local_result.cp_sequence     = cp->sequence;
        local_result.journal_entries = 0U;
        local_result.blocks_restored = false;
        *result                      = local_result;
        return FTL_OK;
    }

    // 介质已有有效 Checkpoint，但 Directory 加载失败 ⇒ 空 Directory 会让旧 region
    // 映射整体丢失（RISK-BOOT-09）。此时没有可用的「重扫 L2P region」路径，唯一
    // 诚实的做法是拒绝启动并保留介质，由用户决定是否格式化。
    if (ftl->flags.md_load_failed == true)
    {
        LOGE(LOG_TAG_META "Directory load failed but checkpoint seq=%lu exists — refuse to boot\r\n", cp->sequence);
        return FTL_ERROR;
    }

    WORD      *block_words  = cp_block_words_get(ftl->cp_handle);
    const bool blocks_valid = cp_block_table_load(ftl->cp_handle, cp, cp_idx, block_words);

    if (blocks_valid == false)
    {
        LOGW(LOG_TAG_BLK "unavailable (rec=%hu seq=%lu) — block accounting via full metadata rebuild\r\n", cp_idx, cp->sequence);
    }

    // 磨损计数（erase/read/ECC 健康）与块元数据一起在 CP 里；软状态，失败只退化 WL。
    if (cp_wear_table_load(ftl->cp_handle, cp, cp_idx) == false)
    {
        LOGW(LOG_TAG_WEAR "snapshot incomplete (rec=%hu seq=%lu) — read pages kept, rest default\r\n", cp_idx, cp->sequence);
    }

    // ③ 判型：最新页**整页**都被 Checkpoint 覆盖 ⇒ 干净关机；否则异常掉电。
    //
    // ⚠ 判据必须是**整页**的 `page_seq + entry_count`，与 jrn_replay 的回放过滤
    // 同口径。`cp->journal_sequence` 的语义是「下一个待分配序号」（Checkpoint job
    // 直接取 sequence_counter，journal_truncate 的注释亦如此声明）⇒ 被 CP 覆盖的条目
    // 是 seq < cp->journal_sequence。一页占 seq [page_seq, page_seq+entry_count-1]，
    // 全被覆盖 ⟺ page_seq + entry_count ≤ cp->journal_sequence。
    //
    // ⚠ 不能只看页首（page_seq ≤ cp->journal_sequence）：Checkpoint 之后写入的第一页
    // page_seq 正好 == cp->journal_sequence，只看页首会把「写→掉电」判成干净、不回放
    // ⇒ 数据静默丢失。上板签名：`boot type=1 journal_entries=0` +
    // `[BLK] rebuild blocks=N valid_cw=0` + `rd FAIL ... got 00000000`。
    // ⚠ 环内有读失败页 ⇒ 最新页与判型都不可信：不能据此判「干净关机」
    // （否则可能少回放条目）。保守走掉电回放路径；回放也会因读失败返回
    // FTL_ERROR，再由 pfr_boot_power_failure 回退 full metadata rebuild。
    if ((ring_read_error == false) && ((ring.page_seq + ring.entry_count) <= cp->journal_sequence))
    {
        // 干净关机
        pfr_boot_clean_start(ftl, cp, blocks_valid ? 1U : 0U, false, &local_result.blocks_restored, &ring);

        local_result.boot_type       = BOOT_CLEAN_START;
        local_result.cp_sequence     = cp->sequence;
        local_result.journal_entries = 0U;
        *result                      = local_result;
        return FTL_OK;
    }

    // 异常掉电
    // 诊断（RISK-MD-06）：判型所依据的现场，一次开机一行（只读）。
    // `newest` = Journal 环里最新一页覆盖到的序号；gap = 它超出 CP 覆盖范围的部分
    // （需要回放且必须仍留在环里的条目数）。与环容量（4 块 × 64 页 × 127~508 条/页）
    // 对比即知「覆盖窗口是否够」。region 级明细见 `l2p_region_diag_log()`。
    {
        const DWORD newest = (DWORD)ring.page_seq + (DWORD)ring.entry_count;
        const DWORD gap    = (newest > cp->journal_sequence) ? (newest - cp->journal_sequence) : 0U;

        (void)LOGW(LOG_TAG_BLK "power-failure diag: cp_seq=%lu cp_jseq=%lu ring_pseq=%lu ring_n=%hu ring_err=%d newest=%lu gap=%lu\r\n",
                   cp->sequence,
                   cp->journal_sequence,
                   ring.page_seq,
                   ring.entry_count,
                   (int)ring_read_error,
                   newest,
                   gap);
    }

    pfr_boot_power_failure(ftl, cp, blocks_valid ? 1U : 0U, &local_result);

    local_result.boot_type   = BOOT_POWER_FAILURE;
    local_result.cp_sequence = cp->sequence;
    *result                  = local_result;
    return FTL_OK;
}

// 掉电恢复唯一入口：跑恢复主体；结束后做一次只读取证（观测用，不改变恢复结果）。
ftl_status_e pfr_boot_recovery(ftl_ctx_t *ftl, pfr_boot_result_t *result)
{
    const ftl_status_e status = pfr_boot_recovery_body(ftl, result);

    // 只读取证（RISK-MD-06）：恢复期间降级的 region，其落点页在介质上是「擦除态」
    // 还是「有数据但 CRC 不符」。放在这里而不是 main.c——UT 的模拟重启直接调本函数，
    // 不经过 ssd_ftl_boot；且恢复已结束、没有在飞命令，同步原样读安全。

    return status;
}

/* ── 三条启动路径（fresh / clean / power-failure）── */

/**
 * @brief 按几何顺序切分 Spare / OP / Free 池（无快照 或 快照失败时用）。
 *
 * @param ftl FTL 实例（调用前 free/op/spare 计数必须为 0）
 *
 * @note 实现要点：每 plane 先扣 Spare（50），再扣 OP（10%），其余进 Free。
 * **只把 state == NONE 的块入池**；full metadata rebuild / 快照已认领的 CLOSED/LOG/RETIRED
 * 只计入 total_blocks。快照路径不调用它（池来自快照），故要求池计数为 0。
 */
static void pfr_boot_carve_pools(ftl_ctx_t *ftl)
{
    alc_t *alloc = &ftl->alloc;

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
 * @brief 开机池建立后：把 CP 快照 BAD 并集 / full metadata rebuild 标坏造成的 Spare 缺口，从 Free Pool 补回。
 *
 * @param ftl FTL 实例
 *
 * @note Free 不足时自然停在低水位，并打一行告警（容量下降是真实结果）。
 */
static void pfr_refill_spare_pools(ftl_ctx_t *ftl)
{
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        (void)alc_spare_refill_from_free(&ftl->alloc, p);

        const WORD n = alc_spare_get_count(&ftl->alloc, p);

        if (n < FTL_SPARE_POOL_PER_PLANE)
        {
            LOGW(LOG_TAG_BLK "spare plane%hhu=%hu/%u (Free pool 不足)\r\n", p, n, FTL_SPARE_POOL_PER_PLANE);
        }
    }
}

/**
 * @brief 出厂首次启动（无 Checkpoint）——cache 全 INVALID + full metadata rebuild → carve 重建，sequence 归零。
 *
 * @param ftl FTL 实例（前置条件，由 pfr_boot_recovery 保证，不在此复检）
 * @param ring Journal 环形块池快照。
 *
 * @note 实现要点：① 失效**本 owner** 的 L2P cache 行与脏位（共享 cache 不能整体 re-init，
 * 否则会抹掉别的 LUN 刚回放的 DIRTY 行）。
 * ② full metadata rebuild Closed SB 列表（认领写过的块）。
 * ③ carve 只把仍为 state NONE 的块放进 Spare/OP/Free。
 * ④ jrn_resume 按环内最新页接回写指针（序号下限 1）。
 * ⑤ checkpoint sequence 归零。
 */
static void pfr_boot_fresh_start(ftl_ctx_t *ftl, const jrn_page_ref_t *ring)
{
    // ① 只失效本 owner 的行/脏位（设备级 init 已清全体；这里覆盖格式化后的场景）
    const BYTE die = topo_2_die(ftl->topo);

    l2p_owner_invalidate(ftl->cache, die);

    // ② full metadata rebuild **先于 carve**：写过的块必须先被认领成 CLOSED；否则 carve 把它们放进
    // Spare/OP 后，full metadata rebuild 再把同块建 Closed ⇒ 池成员与非空闲块重叠。
    {
        pfr_rebuild_stats_t rebuild_stats;
        pfr_rebuild_closed_sb_list(ftl, &rebuild_stats);
    }

    // ③ 池回填：只把 state == NONE 的块入池（CLOSED/LOG/RETIRED 跳过，但仍计入容量）。
    pfr_boot_carve_pools(ftl);
    pfr_refill_spare_pools(ftl);

    // ④ Journal RAM 写态恢复：无 Checkpoint ⇒ 序号下限取 1（0 保留作「无序号」）。
    // 环形块里的旧页仍在（刚格式化过的介质为空，但「找不到 Checkpoint」也可能是
    // Checkpoint 页损坏而环形块完好）⇒ 写指针必须按 NAND 实际内容接回，否则会对
    // 已编程页二次编程；序号从 1 重来还会让下次开机把旧页当新页回放、复活旧映射。
    jrn_resume(ftl->journal, ring, 1U);

    // ⑤ 复位 Checkpoint 定时基准（本实例 cp 游标已由 cp_pool_find_latest 置空）
    cp_timer_reset(ftl->cp_handle);

    // ⑥ 固定 L2P 日志区：活块/当前块置 LOG，其余按 page0 判空置 NONE 或登记回收。
    // 必须在块状态重建（reset / rebuild / carve）之后。
    md_log_area_restore(ftl->metadata_dir, &ftl->alloc);
}

/**
 * @brief 按 checkpoint region_bitmap 局部加载 L2P cache（F4）。
 *
 * @param ftl FTL 实例
 * @param cp 最近 Checkpoint 头（含 region_bitmap）。
 *
 * @note 实现要点：① 只处理 resident **CLEAN** 行：bitmap 未置位 ⇒ INVALID（懒加载 CP 时刻快照）。
 * DIRTY（journal 回放数据）保持待 flush；FLUSHING/LOADING 有命令在飞，跳过。
 * ② 按 CP bitmap **逐位预载**：每个置位 local region 由
 * `l2p_region_preload` 分配一条 INVALID 行、写 region_id、同步读页。
 * **绝不**按 INVALID 行的 region_id 反查 bitmap——init 时所有 INVALID 行
 * region_id=0，按行遍历会让所有行别名到 region 0。
 * ③ 目录条目为 INVALID（region 从未写过 / 刷盘未成功）⇒ 无页可预载，留给懒加载。
 *
 * F4：掉电恢复时，checkpoint 只保证「bitmap 置位的 region」的 NAND 页等于
 * CP 时刻快照。置位区预载 CLEAN（CP 时已 flush 落盘）；未置位区 INVALID，
 * 下一访问懒加载 CP 时刻快照（该区未在本次 CP 落盘，NAND 页为更旧值/从未写）。
 *
 * 必须在 jrn_replay 之后调用。
 */
static void pfr_boot_apply_region_bitmap(ftl_ctx_t *ftl, const cp_header_t *cp)
{
    const BYTE die = topo_2_die(ftl->topo);

    // ① 失效本 owner 的 resident CLEAN 行中不在 CP bitmap 的 region。
    for (WORD i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        l2p_line_info_t line;

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

        const DWORD global = (local * DIE_MAX) + die;

        if (l2p_region_preload(ftl->cache, global, addr) == false)
        {
            LOGW(LOG_TAG_BLK "preload region %lu failed\r\n", local);
        }
    }
}

/**
 * @brief 干净关机恢复——块元数据快照有效时加载块元数据 + 池 + Closed 列表；否则 full metadata rebuild → carve。
 *
 * @param ftl FTL 实例
 * @param cp 最近 Checkpoint Header
 * @param blocks_valid 块元数据快照是否已通过校验
 * @param force_rebuild true = 禁止走「initial CP 跳过重建」捷径（REBUILD_REQUIRED 路径）
 * @param blocks_restored 出参：true = 走快照路径
 * @param ring Journal 环形块池快照。
 *
 * @note 实现要点：clean 下 Checkpoint 覆盖全部 journal 条目，不需要回放；但必须
 * jrn_resume 接回写指针/序号（环形块并未被擦）。
 * ⚠ 即使判定 clean，也要 probe 池块：本仓无关机 journal flush，
 * 「丢最后一页 journal」与 clean 在介质上不可区分。
 */
static void pfr_boot_clean_start(ftl_ctx_t *ftl, const cp_header_t *cp, BYTE blocks_valid, bool force_rebuild, bool *blocks_restored, const jrn_page_ref_t *ring)
{
    // ① 共享 cache 已由设备级 init 置空；本路径只按 CP bitmap 约束本 owner 的行
    // （首次 miss 时按需从 NAND L2P Table 加载）。
    bool restored     = false;
    // 块元数据快照由 pfr_boot_recovery 的 cp_block_table_load 读进
    // cp_handle 的常驻缓冲；clean 路径直接取同一缓冲。
    WORD *block_words = cp_block_words_get(ftl->cp_handle);

    if (blocks_valid != 0U)
    {
        if (pfr_restore_blocks(ftl, block_words))
        {
            // 预载 CP region，供 probe 的逐 CW L2P 反查复用（未覆盖区懒加载）。
            pfr_boot_apply_region_bitmap(ftl, cp);

            pfr_block_restore_stats_t stats;
            memset(&stats, 0, sizeof(stats));
            pfr_probe_blocks(ftl, block_words, &stats);

            if (stats.l2p_read_errors != 0U)
            {
                // L2P 反查失败 ⇒ valid_count 少算，快照结果不完整：回退 full metadata rebuild。
                LOGW(LOG_TAG_BLK "probe: %hu L2P read error(s) — fallback to full metadata rebuild\r\n", stats.l2p_read_errors);
            }
            else if (pfr_build_pools(ftl, block_words))
            {
                restored = true;
                LOGI(LOG_TAG_BLK "restore closed=%hu log=%hu free=%hu op=%hu spare=%hu anomalies=%hu valid_cw=%lu\r\n", stats.closed_blocks, stats.log_blocks, ftl->alloc.free_count, alc_pool_sum_counts(ftl->alloc.op_count, PLANE_NUM), alc_pool_sum_counts(ftl->alloc.spare_count, PLANE_NUM), stats.anomalies, stats.valid_cw);
            }
        }
    }

    if (restored == 0)
    {
        // initial Checkpoint = 全量格式化写的空快照（seq=1 / 无块表 / journal 水位 0）。
        // 此时用户区已由 format 擦净，full metadata rebuild（全盘扫描 4096×64 页）没有信息可恢复，
        // 直接 reset + carve 即可；跳过它把首次启动从几十秒降到一次池切分。
        const bool initial_cp = (force_rebuild == false) && (cp->sequence == 1U) && (cp->journal_sequence == 0U) && (cp->block_word_count == 0U) && (cp->wear_entry_count == 0U);

        if (initial_cp)
        {
            LOGI(LOG_TAG_BLK "initial CP: skip full metadata rebuild, carve from blank\r\n");
        }
        else
        {
            LOGW(LOG_TAG_BLK "fallback to full metadata rebuild (clean start)\r\n");
        }

        pfr_reset_blocks(ftl);

        // full metadata rebuild 先认领写过块，carve 再只把 state==NONE 的块入池（避免池成员重叠）。
        if (initial_cp == false)
        {
            pfr_rebuild_stats_t rebuild_stats;

            pfr_rebuild_closed_sb_list(ftl, &rebuild_stats);
        }
        pfr_boot_carve_pools(ftl);
    }

    pfr_refill_spare_pools(ftl);

    // ② Journal RAM 写态恢复（环形块未被擦，必须接回写指针/序号）。
    jrn_resume(ftl->journal, ring, cp->journal_sequence);

    // ③ Controller → Ready
    cp_timer_reset(ftl->cp_handle);
    *blocks_restored = restored;

    // ④ 固定 L2P 日志区：活块/当前块置 LOG，其余按 page0 判空置 NONE 或登记回收。
    md_log_area_restore(ftl->metadata_dir, &ftl->alloc);
}

/**
 * @brief 异常掉电恢复——cache 全 INVALID → Journal 回放 → F4 bitmap 局部加载 → full metadata rebuild。
 *
 * @param ftl FTL 实例
 * @param cp 最近 Checkpoint 头
 * @param blocks_valid 块元数据快照是否已通过校验（0 = 无有效快照，跳过快照恢复）
 * @param result 出参启动结果。
 *
 * @note 实现要点：② jrn_replay 回放所有**未全被 CP 覆盖**的页（整页判据
 * `page_seq + entry_count > cp_sequence`；只回放最大一页会静默丢
 * ≥2 页条目）；④ full metadata rebuild closed_sb_list（不再恢复 hot_sb/cold_sb 写指针）。
 */
static void pfr_boot_power_failure(ftl_ctx_t *ftl, const cp_header_t *cp, BYTE blocks_valid, pfr_boot_result_t *result)
{
    DWORD cp_sequence = cp->journal_sequence;

    // ① 共享 cache 已由设备级 init 置空；journal 回放把本 owner 的映射写进 cache。
    // ⚠ 不能在此整体 re-init：会抹掉前面 LUN 刚回放的 DIRTY 行与全局脏位。

    // ② 块元数据快照恢复（若有效）：先恢复块元数据，回放时才能同步维护 valid_count。
    BYTE             blocks_applied = 0U;
    WORD            *block_words    = cp_block_words_get(ftl->cp_handle);
    jrn_replay_ctx_t block_ctx;
    memset(&block_ctx, 0, sizeof(block_ctx));
    block_ctx.fail_pba = NAND_PBA_INVALID; /* 0 是合法 PBA：失败位置必须显式置哨兵 */
    s_sub_zero_n       = 0U;               /* 本轮回放的现场计数 */
    s_sub_zero_g       = UINT16_MAX;
    s_sub_zero_st      = 0U;

    if ((blocks_valid != 0U) && pfr_restore_blocks(ftl, block_words))
    {
        blocks_applied        = 1U;
        block_ctx.materialize = pfr_materialize_pba;
        block_ctx.valid_add   = pfr_valid_add;
        block_ctx.valid_sub   = pfr_valid_sub;
        block_ctx.ctx         = ftl;
        block_ctx.ok          = true;
    }

    // ③ Journal 回放：映射写入 L2P Cache；block_ctx 有效时同步记 valid 增量。
    // F10：按 (page_seq, entry_index) 递增序回放所有
    // page_seq + entry_count > cp_sequence 的页。
    DWORD      entry_count = 0U;
    const bool replay_ok   = (jrn_replay(ftl->journal, ftl->cache, cp_sequence, &entry_count, (blocks_applied != 0U) ? &block_ctx : NULL) == FTL_OK);

    if (replay_ok == false)
    {
        // 回放不完整（环内有读失败页）：journal 写态已恢复，但映射可能缺失
        // ⇒ 强制回退 full metadata rebuild（见下方 restored 判定）。
        LOGW(LOG_TAG_BLK "journal replay incomplete — force full metadata rebuild\r\n");
        block_ctx.ok = false;
    }

    // 一致性守卫（RISK-MD-06）：**目录引用的块，其 valid_count 必须 > 0**。
    // 不一致说明块账与目录已分叉（重建少算 / 静默夹账 / 块被误清零）。此时继续使用
    // 这份快照，下一次开机回放会对该块做 `valid_sub` 并撞下溢守卫 ⇒ 整份快照作废
    // （`ctx_ok=0`、`reason=5`）。这里在**回放刚结束、现场还完整**的位置先报出来。
    //
    // 判据是现成账本 `l2p_live`（每块被目录引用的 region 数），O(FTL_BLOCK_TOTAL)，无 I/O。
    {
        WORD bad_n     = 0U;
        WORD first_g   = UINT16_MAX;
        WORD first_liv = 0U;

        for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
        {
            const WORD live = md_live_count_get(ftl->metadata_dir, g);

            // 只比**用户数据块**：固定区 + 当前/借用的 L2P 日志块本来就不维护
            // valid_count（日志块的有效性由 l2p_live 自己表达），比了必误报。
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

            // 这才是「快照不可信」的判据：目录仍引用的块计数为 0。作废快照走全量重建。
            block_ctx.ok = false;
        }
    }

    // 回放期「减到 0 被跳过」的现场（RISK-MD-06）：非 0 才打一行。
    // 这是本次修复是否真正起作用的**唯一直接证据**——旧实现会让同一场景走 full rebuild。
    if (s_sub_zero_n != 0U)
    {
        const bst_state_e zst = (s_sub_zero_g < FTL_BLOCK_TOTAL) ? ftl_block_state(&ftl->alloc.blocks[s_sub_zero_g]) : FTL_BLOCK_STATE_NONE;

        LOGW(LOG_TAG_BLK "replay: %hu sub-at-zero kept (not fatal); first g=%hu st=%s live=%hu\r\n", s_sub_zero_n, s_sub_zero_g, ftl_block_state_name(zst), (s_sub_zero_g < FTL_BLOCK_TOTAL) ? md_live_count_get(ftl->metadata_dir, s_sub_zero_g) : 0U);
    }

    // 降级 region 只丢旧映射，不再让快照失效（范围化恢复）。这里先记标记，
    // probe 阶段即使没扫到相关块也能在 boot 结果里体现（RISK-BOOT-18）。
    if (jrn_replay_degraded_entries() != 0U)
    {
        result->scoped = true;
        LOGW(LOG_TAG_BLK "scoped recovery: %lu journal entr(ies) hit degraded L2P region(s) — snapshot kept\r\n", jrn_replay_degraded_entries());
    }

    // ④ 按 Checkpoint region_bitmap 局部加载（F4）：跳过 DIRTY 回放行，
    // bitmap 区预载 CLEAN，非 bitmap 区 INVALID（懒加载 CP 时刻快照）。
    pfr_boot_apply_region_bitmap(ftl, cp);

    // ⑤ 块元数据快照收尾：探测池块 + 唯一入池点；任一步失败 ⇒ 丢弃快照结果走 full metadata rebuild。
    BYTE restored = 0U;

    if ((blocks_applied != 0U) && block_ctx.ok)
    {
        pfr_block_restore_stats_t stats;
        memset(&stats, 0, sizeof(stats));
        pfr_probe_blocks(ftl, block_words, &stats);

        if (stats.l2p_read_errors != 0U)
        {
            LOGW(LOG_TAG_BLK "probe: %hu L2P read error(s) — fallback to full metadata rebuild\r\n", stats.l2p_read_errors);
        }
        else
        {
            // 降级 region 只丢映射、不影响 valid_count 增量 ⇒ 不让它触发全量重建。
            // 丢失范围由 host 读路径按 IO_ERROR 暴露（RISK-BOOT-18）。
            if (stats.l2p_degraded_cw != 0U)
            {
                result->scoped = true;
                LOGW(LOG_TAG_BLK "scoped recovery: %hu CW in degraded L2P region(s) — snapshot kept, mappings lost\r\n", stats.l2p_degraded_cw);
            }

            if (pfr_build_pools(ftl, block_words))
            {
                restored = 1U;
                LOGI(LOG_TAG_BLK "restore closed=%hu log=%hu free=%hu op=%hu spare=%hu anomalies=%hu valid_cw=%lu\r\n", stats.closed_blocks, stats.log_blocks, ftl->alloc.free_count, alc_pool_sum_counts(ftl->alloc.op_count, PLANE_NUM), alc_pool_sum_counts(ftl->alloc.spare_count, PLANE_NUM), stats.anomalies, stats.valid_cw);
            }
        }
    }

    if (restored == 0)
    {
        // 走了全量重建 ⇒ 本次启动**不是**范围化恢复。清掉可能由 journal/probe
        // 提前置上的 scoped，否则 boot 位会把 fallback 谎报成 scoped（R1）。
        result->scoped = false;

        // 诊断：把「快照是否加载 / 是否应用 / 回放是否成功」一并打出——
        // ctx_ok=false 有三种来源：blocks_valid=0（快照没读成）、applied=0（快照被
        // 第 1 遍 拒绝）、replay_ok=false 或回放内部把 ok 清 0（降级/写失败）。
        // `vc_end` / `live` 是**回放结束后**该块的值，用来区分两种根因：
        //   vc_end > 0 ⇒ sub 跑在对应 add 之前（回放顺序/重复问题）；
        //   vc_end == 0 ⇒ 该块在快照里就没有有效 CW（账本/快照分叉）。
        const WORD fg     = (block_ctx.fail_pba != NAND_PBA_INVALID) ? ftl_pba_gblock(block_ctx.fail_pba) : (WORD)UINT16_MAX;
        const WORD vc_end = (fg < FTL_BLOCK_TOTAL) ? ftl->alloc.blocks[fg].valid_count : 0U;
        const WORD lv_end = (fg < FTL_BLOCK_TOTAL) ? md_live_count_get(ftl->metadata_dir, fg) : 0U;

        LOGW(LOG_TAG_BLK "fallback to full metadata rebuild (power failure, blocks_valid=%hhu applied=%hhu ctx_ok=%d replay_ok=%u reason=%hhu pba=%08lX g=%hu vc_end=%hu live=%hu sub0=%hu/%hu/%hhu)\r\n", blocks_valid, blocks_applied, block_ctx.ok, (replay_ok ? 1U : 0U), block_ctx.fail_reason, block_ctx.fail_pba, fg, vc_end, lv_end, s_sub_zero_n, s_sub_zero_g, s_sub_zero_st);
        pfr_reset_blocks(ftl);

        // full metadata rebuild 用已回放的 L2P 反查；先认领写过块，carve 再只把 state==NONE 的块入池。
        pfr_rebuild_stats_t rebuild_stats;
        pfr_rebuild_closed_sb_list(ftl, &rebuild_stats);
        pfr_boot_carve_pools(ftl);
    }

    pfr_refill_spare_pools(ftl);

    // ⑥ Controller → Ready
    cp_timer_reset(ftl->cp_handle);

    // ⑦ 固定 L2P 日志区：活块/当前块置 LOG，其余按 page0 判空置 NONE 或登记回收。
    md_log_area_restore(ftl->metadata_dir, &ftl->alloc);

    // ⑧ 输出结果
    result->boot_type       = BOOT_POWER_FAILURE;
    result->cp_sequence     = cp_sequence;
    result->journal_entries = entry_count;
    result->blocks_restored = restored;
}

// 统计各分区（目录检查点/CP/BBT/Journal/L2P/spare/User）的坏块与 reserved 数并打一行日志。
void pfr_boot_log_bad_areas(const ftl_ctx_t *ftl)
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
static bool pfr_block_is_fixed(WORD g)
{
    // 必须按分区表的 plane_mask 判：分区只在 mask 覆盖的 plane 上预留，表外同号块
    // 是用户数据。不要写死 "block < 某常量"——布局随介质版本演进。
    return md_block_is_metadata(NULL, ftl_gblock_plane(g), ftl_gblock_block(g));
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
static bool pfr_closed_sb_add(ftl_ctx_t *ftl, WORD g, BYTE temperature)
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
// 不是「RAW 读数 FF」——RAW 读刻意跳过 ECC，没有判空依据（RISK-IF-15）。

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
 * （RISK-BOOT-06），故返回全块让调用者逐页反查。
 * ⚠ 代价：存在 hole 的块要扫完全部页（正常部分写块仍在第一个空白页停止）。
 * 这类块只可能由写失败产生，属少数；换来的 valid_count 正确性是 GC 选择
 * victim 的前提。
 */
static BYTE pfr_scan_write_pointer(const ftl_ctx_t *ftl, BYTE plane, WORD block)
{
    // 快速判空：page 0 是擦除页 ⇒ 整块空白。
    //
    // 正常写模型是**顺序追加**：块从 page 0 开始写；page 0 编程失败时写路径会
    // 关闭 SB（ftl_host_io_callback 的失败分支），不会继续写后续页。因此
    // “page 0 空白、但后面有数据”在当前写模型下不可达。
    //
    // 这一条把空闲块的扫描从 64 页降到 1 页：4096 块 × 64 页 → 约 4k 页，
    // 是 full metadata rebuild 的主要提速点。
    //
    // ⚠ 若将来引入“page 0 失败后继续写”或开放 Open SB 续写，这个假设失效：
    // 必须恢复全块扫描，或在 page 0 空白后补读 page 63（仍是 2 次/空闲块）。
    // 见 known_risks RISK-BOOT-14。
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
 * @brief 清零文件级 L2P 扫描异常汇总（read fail CW / degraded CW / 首个降级 region）。
 */
static void pfr_rebuild_l2p_stats_reset(void)
{
    s_rebuild_read_fail_cw = 0U;
    s_rebuild_degraded_cw  = 0U;
    s_rebuild_first_region = UINT32_MAX;
}

/**
 * @brief 汇总打印文件级 L2P 扫描异常（有计数时才打，避免扫描期逐块刷 UART）。
 */
static void pfr_rebuild_l2p_stats_report(void)
{
    if (s_rebuild_read_fail_cw != 0U)
    {
        LOGW(LOG_TAG_BLK "rebuild: L2P read failed for %lu CW — valid_count 可能少算\r\n", s_rebuild_read_fail_cw);
    }
    if (s_rebuild_degraded_cw != 0U)
    {
        LOGW(LOG_TAG_BLK "rebuild: L2P region %lu degraded — %lu CW skipped (mapping lost)\r\n", s_rebuild_first_region, s_rebuild_degraded_cw);
    }
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
 * @param stats       出参统计（可为 NULL；累加 blocks_rebuilt / l2p_read_errors /
 *                    l2p_degraded_cw / valid_cw）
 *
 * @note Policy B 下配对不再需要被恢复——两 plane 的块各自独立归位即可，故本函数按
 * **单个物理块** 建一条 Closed 条目，另一 plane 记 FTL_BLOCK_NO_PLANE 哨兵。
 */
static void pfr_close_block(ftl_ctx_t *ftl, BYTE plane, WORD block, BYTE pages_written, BYTE temperature, pfr_rebuild_stats_t *stats)
{
    alc_t *const alloc = &ftl->alloc;
    const WORD   g     = ftl_gblock(plane, block);

    // 退化单 plane 条目的构造与快照恢复同源（C2）。列表满（结构上不应发生）⇒ 放弃本块。
    if (pfr_closed_sb_add(ftl, g, temperature) == false)
    {
        return;
    }

    if (stats != NULL)
    {
        stats->blocks_rebuilt++;
    }

    // 逐页：gc_read_page_with_lba 取 LBA + **逐 CW** l2p 反查。
    // ⚠ 只扫已写页 [0, pages_written)：写指针之后的空白页没有 META，
    // 读回来也解不出 LBA，纯白读。
    // ⚠ [待上板] 掉电重启后核对 `[BLK] rebuild ... partial=... valid_cw=...`：
    // valid_cw 应与改前一致（空白页本就不贡献有效 CW），partial 块的读页数减少。
    bst_block_t *b = &alloc->blocks[g];
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
                // 启动快照路径据此回退 full metadata rebuild（RISK-BOOT-11），full metadata rebuild 路径至少可见。
                s_rebuild_read_fail_cw++;
                if (stats != NULL)
                {
                    stats->l2p_read_errors++;
                }
                continue;
            }

            // 读成功也可能来自“重试耗尽 → 降级为全未映射”：返回值同样是 OK + INVALID。
            // 降级只丢**该 region 未被 Journal 覆盖的旧映射**，不影响 valid_count 增量
            // ⇒ 计入 l2p_degraded_cw 并继续，不再据此回退全量重建（RISK-BOOT-18）；
            // 丢失的映射由 host 读路径按 IO_ERROR 暴露。
            const DWORD global_region = lba_to_global_region(cw_lba[cw]);

            if (l2p_is_region_degraded(ftl->cache, global_region))
            {
                s_rebuild_degraded_cw++;
                if (s_rebuild_first_region == UINT32_MAX)
                {
                    s_rebuild_first_region = global_region;
                }
                if (stats != NULL)
                {
                    // 降级 = 映射已丢（范围化恢复接受它）；只有真正的读失败才计入
                    // l2p_read_errors 并回退全量重建（RISK-BOOT-18）。
                    stats->l2p_degraded_cw++;
                }
                continue;
            }

            if ((cur != NAND_PBA_INVALID) && (cur == pba_with_cw(pba, cw)))
            {
                ftl_block_add_valid(b); /* valid_count 单位 = CW */
                if (stats != NULL)
                {
                    stats->valid_cw++;
                }
            }
        }
    }
}

/**
 * @brief full metadata rebuild 掉电重建——逐 plane 逐块扫 NAND，把「写过的物理块」拉出 Free Pool 并建 Closed 条目，同时逐 CW L2P 双校验回填 valid_count。
 *
 * @param ftl FTL 实例
 * @param stats 出参统计（blocks_rebuilt / valid_cw / partial_blocks，可为 NULL）。
 *
 * @note 实现要点：Policy B 下两 plane 的块不再同号 ⇒ 按**单物理块**建 Closed 条目
 * （另一 plane 记 FTL_BLOCK_NO_PLANE 哨兵），配对无需、也无法恢复。
 */
static void pfr_rebuild_closed_sb_list(ftl_ctx_t *ftl, pfr_rebuild_stats_t *stats)
{
    alc_t *alloc = &ftl->alloc;

    if (stats != NULL)
    {
        memset(stats, 0, sizeof(*stats));
    }

    pfr_rebuild_l2p_stats_reset();

    // Policy B：**逐 plane 独立扫描**——两 plane 的块不再同号。配对只在写入时由
    // allocator 创建，之后从不被查找（multi-plane 读的两个 PBA 各自从 L2P 查；
    // GC/WL 逐 plane 工作），故重建只需把"写过的物理块各自归位"，无需、也无法
    // 恢复"哪两块曾是一对"（配对取决于 GC 历史，不在任何持久化位图里）。
    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        for (WORD block = 0U; block < BLOCKS_PER_PLANE; block++)
        {
            WORD         g = ftl_gblock(plane, block);
            bst_block_t *b = &alloc->blocks[g];

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
            // ⇒ 又走全盘 build，形成每次冷启动都 rebuild 的循环（RISK-BOOT-15）。
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
            pfr_close_block(ftl, plane, block, np, FTL_TEMP_HOT, stats);

            if ((stats != NULL) && (np < PAGES_PER_BLOCK))
            {
                stats->partial_blocks++;
            }
        }
    }

    // ⑤ 写过的块已从 free pool 拉出；free_count 是唯一账本，无派生计数需要重同步。

    // 观测口（上板验证用）：valid_cw 是本轮重建**回填**的有效 CW 数。
    // ⚠ 它是后续覆盖写的安全前提——`ftl_confirm_write` 对旧映射所在块做
    // `ftl_block_sub_valid`（bst.h 内断言 `valid_count > 0`）。若介质上确有数据
    // 而 valid_cw == 0，则重建没认到这些 CW，下一次覆盖写该 LBA 会触发断言。
    if (stats != NULL)
    {
        LOGI(LOG_TAG_BLK "rebuild blocks=%hu partial=%hu valid_cw=%lu\r\n", stats->blocks_rebuilt, stats->partial_blocks, stats->valid_cw);
    }

    pfr_rebuild_l2p_stats_report();
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
 * @param ftl   FTL 实例
 * @param words 每块 16-bit 快照 word（FTL_BLOCK_TOTAL 项）；第 3 遍会回写对账后的 LOG 块
 *
 * @return true  = 恢复完成；
 *         false = 快照存在非法 word / 池标签冲突 / Closed 列表满，调用者应 reset 并回退 full metadata rebuild。
 */
static bool pfr_restore_blocks(ftl_ctx_t *ftl, WORD words[FTL_BLOCK_TOTAL])
{
    alc_t *alloc = &ftl->alloc;

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
        // 修正为 LOG（RISK-BOOT-15 的自愈路径，避免为一处旧快照做全盘 rebuild）。
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

        bst_block_t *b = &alloc->blocks[g];

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
                ftl_block_clear_valid(b);
                ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
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
        bst_block_t *b = &alloc->blocks[g];

        if (ftl_block_is_bad(b))
        {
            continue;
        }
        if (md_live_count_get(ftl->metadata_dir, g) > 0U)
        {
            // NONE = full rebuild 留下的 NONE/no-pool（RISK-BOOT-15 自愈路径，
            // 可能上百块，不逐块告警）；其他非 LOG 状态才是真冲突，留痕一次。
            const BYTE prev = ftl_block_state(b);

            if ((prev != FTL_BLOCK_STATE_LOG) && (prev != FTL_BLOCK_STATE_NONE))
            {
                LOGW(LOG_TAG_BLK "g=%hu state=%hhu but Directory says LOG — Directory wins\r\n", g, prev);
            }
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
        bst_block_t *b = &alloc->blocks[g];

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
 * @param words 每块 16-bit 快照 word（FTL_BLOCK_TOTAL 项）；陈旧 LOG 擦净后回写 FREE
 * @param stats 出参统计（可为 NULL；累加 log_blocks / closed_blocks / anomalies /
 *              l2p_read_errors / l2p_degraded_cw / valid_cw）
 */
static void pfr_probe_blocks(ftl_ctx_t *ftl, WORD words[FTL_BLOCK_TOTAL], pfr_block_restore_stats_t *stats)
{
    alc_t *alloc = &ftl->alloc;

    if (stats != NULL)
    {
        memset(stats, 0, sizeof(*stats));
        stats->closed_blocks = alloc->closed_sb_count; /* restore / replay 已建的条目 */
    }

    pfr_rebuild_l2p_stats_reset();

    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        if (pfr_block_is_fixed(g))
        {
            continue;
        }

        bst_block_t *b = &alloc->blocks[g];

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
                if (stats != NULL)
                {
                    stats->log_blocks++;
                }
                continue; /* Directory 仍引用：活的 L2P 日志块 */
            }

            // 陈旧 LOG（已不被 Directory 引用）：全页判空。
            // ⚠ 不能只看 page 0——元数据游标在写失败时会跳过该页。
            bool any = false;

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
                if (stats != NULL)
                {
                    stats->closed_blocks++;
                }
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
            bool later_written = false;

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

        pfr_rebuild_stats_t rebuild;
        memset(&rebuild, 0, sizeof(rebuild));
        pfr_close_block(ftl, plane, block, PAGES_PER_BLOCK, FTL_TEMP_HOT, &rebuild);

        if (stats != NULL)
        {
            stats->anomalies++;
            stats->l2p_read_errors += rebuild.l2p_read_errors;
            stats->l2p_degraded_cw += rebuild.l2p_degraded_cw;
            stats->valid_cw += rebuild.valid_cw;
        }
    }

    pfr_rebuild_l2p_stats_report();
}

/**
 * @brief 按 BST 状态与快照 word 把 NONE 块放入 Free / OP / Spare 池（唯一入池点）。
 *
 * @param ftl   FTL 实例（调用前 free/op/spare 计数必须为 0）
 * @param words 每块 16-bit 快照 word（FTL_BLOCK_TOTAL 项），用于读取池标签
 *
 * @return true  = 全部块归位；
 *         false = OP/Spare 池溢出或遇到非池块（调用者回退 full metadata rebuild）。
 */
static bool pfr_build_pools(ftl_ctx_t *ftl, const WORD words[FTL_BLOCK_TOTAL])
{
    alc_t *alloc = &ftl->alloc;

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

        bst_block_t *b = &alloc->blocks[g];

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
static void pfr_reset_blocks(ftl_ctx_t *ftl)
{
    alc_t *alloc = &ftl->alloc;

    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        bst_block_t *b = &alloc->blocks[g];

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
 * @param ctx FTL 实例（jrn_replay 回调上下文，类型 ftl_ctx_t *）
 * @param pba journal 条目中的物理块地址（来自介质，先校验合法性与实例归属）
 *
 * @return true  = 块已处于可记账状态（含 BAD 块可忽略）；
 *         false = PBA 非法 / 不属于本实例 / 指向固定或活 LOG 块 / 状态非法 /
 *                 Closed 列表满（回放不一致）。
 */
static bool pfr_materialize_pba(void *ctx, DWORD pba)
{
    ftl_ctx_t *ftl = (ftl_ctx_t *)ctx;

    // journal 条目来自介质（H29 不可信输入）：先校验 PBA 属于本实例且字段合法，
    // 否则会把 valid_count 记到别的块，而 L2P 存下一个越界地址。
    if ((pba_is_valid(pba) == false) || (pba_to_ch(pba) != ftl->topo.ch) || (pba_to_ce(pba) != ftl->topo.ce) || (pba_to_lun(pba) != ftl->topo.lun))
    {
        return false; /* block_ctx.ok=false ⇒ 调用者回退 full metadata rebuild */
    }

    alc_t     *alloc = &ftl->alloc;
    const WORD g     = ftl_pba_gblock(pba);

    if (g >= FTL_BLOCK_TOTAL || pfr_block_is_fixed(g))
    {
        return false;
    }

    bst_block_t *b = &alloc->blocks[g];

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
 * @param ctx FTL 实例（jrn_replay 回调上下文，类型 ftl_ctx_t *）
 * @param pba 物理块地址
 * @param ok  出参：块号越界或 valid_count 上溢时置 false，否则不改
 */
static void pfr_valid_add(void *ctx, DWORD pba, bool *ok)
{
    ftl_ctx_t *ftl = (ftl_ctx_t *)ctx;

    const WORD g = ftl_pba_gblock(pba);

    if (g >= FTL_BLOCK_TOTAL)
    {
        *ok = false;
        return;
    }

    bst_block_t *b = &ftl->alloc.blocks[g];

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
 * @param ctx FTL 实例（jrn_replay 回调上下文，类型 ftl_ctx_t *）
 * @param pba 物理块地址
 * @param ok  出参：块号越界或 valid_count 已为 0 时置 false，否则不改
 */
static void pfr_valid_sub(void *ctx, DWORD pba, bool *ok)
{
    ftl_ctx_t *ftl = (ftl_ctx_t *)ctx;

    const WORD g = ftl_pba_gblock(pba);

    if (g >= FTL_BLOCK_TOTAL)
    {
        *ok = false;
        return;
    }

    bst_block_t *b = &ftl->alloc.blocks[g];

    if (ftl_block_is_bad(b))
    {
        return;
    }
    if (b->valid_count == 0U)
    {
        // 与运行期 `ftl_block_sub_valid_if_live`（bst.h）**同口径**：计数已为 0 ⇒
        // 无可减，跳过。原来这里判硬失败，等于把「一条陈旧映射」升级成「整份块元数据
        // 快照作废 ⇒ full rebuild ⇒ 丢映射」（RISK-MD-06 的放大器）。
        // 真正该作废快照的是「目录仍引用该块」那种，由回放后的不变量守卫判定。
        if (s_sub_zero_n == 0U)
        {
            s_sub_zero_g  = g;
            s_sub_zero_st = (BYTE)ftl_block_state(b);
        }
        if (s_sub_zero_n < UINT16_MAX)
        {
            s_sub_zero_n++;
        }
        return;
    }
    ftl_block_sub_valid(b);
}

/*
 * power_failure_recovery.c — 掉电恢复（启动三条路径：fresh / clean / power-failure）
 *
 * 职责：
 *   开机时判定上次关机形态并据此恢复 RAM 状态——扫 Checkpoint 取最近快照，
 *   扫 Journal 取最后 sequence，二者比较决定走哪条路径；以及掉电重建
 *   Closed SB 列表（F2，全 NAND 扫描）与写指针扫描。
 *
 * 与 checkpoint.c / journal.c 的分工：
 *   checkpoint.c —— Checkpoint 的写与查找（checkpoint_find_latest）；
 *   journal.c    —— Journal 的写与回放（journal_replay）；
 *   本文件       —— 只做**编排**：把两者拼成三条启动路径，自身不解析介质格式。
 *
 * 三条启动路径（状态机）：
 *   BOOT_FRESH_START    无有效 Checkpoint（出厂首次启动）→ cache INVALID + carve + F2
 *   BOOT_CLEAN_START    journal 全被 CP 覆盖（干净关机）
 *                       → 块账快照有效：restore + probe + build_pools（**不跑 F2**）
 *                         否则 carve + F2
 *   BOOT_POWER_FAILURE  journal 有未回放条目（异常掉电）
 *                       → 块账快照 restore + journal_replay（映射 + valid 增量）+ F4
 *                         + probe + build_pools；失败 ⇒ reset + carve + F2
 *   ★ F2 只在「无快照 / 快照损坏 / 增量记账不一致」时兜底。
 *
 * 文件结构：
 *   1. 启动恢复总入口   ftl_boot_recovery
 *   2. 三条启动路径     ftl_boot_fresh_start / ftl_boot_clean_start
 *                       / ftl_boot_apply_region_bitmap（static）/ ftl_boot_power_failure
 *   3. F2 全盘重建      ftl_recovery_page_is_blank / ftl_recovery_scan_write_pointer / ftl_recovery_close_block（static）
 *                       / ftl_rebuild_closed_sb_list
 *
 * ⚠ 本文件依赖 ftl_init 已完成 allocator / journal / metadata_dir 的初始化；
 *   各路径步骤用圈码 ①②③ 编号（与 docs/design/ftl_design.md §8 对应）。
 *
 * ⚠ 入口前置守卫口径（docs/design/assertion_guidelines.md §二十六 / §二十八）：
 *   对外入口（ftl_media_needs_format / ftl_media_format / ftl_boot_recovery）的 ftl/result
 *   非空是**前置条件**，用 ASSERT；三条启动路径与其内部 helper（fresh_start / clean_start /
 *   power_failure / apply_region_bitmap / rebuild_closed_sb_list）只被 ftl_boot_recovery 调用，
 *   该处的 ftl 非空已由它保证 ⇒ **不再逐函数复检**（原守卫已删）。
 */

#include "ftl/power_failure_recovery.h"

#include "common/assertion.h"
#include "common/log.h"
#include "fc/fc.h"
#include "ftl/bbt.h"
#include "ftl/checkpoint.h"
#include "ftl/ftl.h"
#include "ftl/ftl_fc_interface.h"
#include "ftl/gc.h"
#include "ftl/metadata.h"
#include "nand/onfi_1_0.h"

#include <string.h>

/* 块账快照恢复的前置声明（定义在文件末尾；journal replay 以回调注入）。 */
static bool recovery_materialize_pba(void *ctx, nand_pba_t pba);

/* ══════════════════════════════════════════════════════════════════
 * 介质布局版本 —— 开机自动格式化
 * ══════════════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * ftl_media_needs_format
 *   功能：判定介质是否需要（重新）格式化。
 *   参数：ftl —— FTL 实例。
 *   返回：true = 需要格式化；false = 介质已是当前布局版本（正常启动）。
 *   实现要点：判据 = 「本实例 checkpoint_find_latest 失败」。因版本校验已在
 *             checkpoint_header_valid 内，故该判据自动涵盖三种情况：
 *             ① 有当前版本 Checkpoint ⇒ 成功 ⇒ 不格式化；
 *             ② 有 Checkpoint 但版本不符（旧格式介质）⇒ 失败 ⇒ 格式化；
 *             ③ 完全没有 Checkpoint（出厂空片）⇒ 失败 ⇒ 格式化（多擦一次，写标记后自愈）。
 * -------------------------------------------------------------------- */
bool ftl_media_needs_format(struct ftl_context *ftl)
{
    /* 前置条件（判据 2）：唯一调用者 main.c 传 &g_ftl[ch][ce][lun]，结构上恒非空。 */
    ASSERT(ftl != NULL);

    l2p_checkpoint_header_t cp;
    return (checkpoint_find_latest(ftl, &cp, NULL) != FTL_OK);
}

/* --------------------------------------------------------------------
 * ftl_media_format
 *   功能：全量格式化——擦本实例全部块，并把易失元数据状态复位到"空片"。
 *   参数：ftl —— FTL 实例。
 *   返回：FTL_OK / FTL_ERROR。
 *   实现要点：① bbt_erase_all_blocks 擦全片（出厂坏块标记已在 bbt_init 读进 BST——
 *                **空片时**才读；用过的介质上 bbt_init 拒绝重扫，见 bbt_medium_has_ftl_data）；
 *             ② bbt_save 重新持久化坏块图（BBT 池刚被擦掉）；
 *             ③ metadata_dir_init 把 Directory 复位为"全部未映射"；④ checkpoint_write_initial
 *                写一条当前版本标记 ⇒ 下次开机版本判据命中，格式化只发生一次。
 *   ⚠ 必须在 ftl_init 之后、free pool 回填之前调用（见 main.c ssd_ftl_boot）。
 *   ⚠ BST 的 erase_count 保留旧值（开机已从 CP 磨损快照回填）——仅影响磨损均衡起点。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_media_format(struct ftl_context *ftl)
{
    /* 前置条件（判据 2）：唯一调用者 main.c 传 &g_ftl[ch][ce][lun]。 */
    ASSERT(ftl != NULL);

    if (bbt_erase_all_blocks(&ftl->alloc) != FTL_OK)
    {
        return FTL_ERROR;
    }
    (void)bbt_save(&ftl->alloc); /* 坏块图重新落盘（BBT 池刚被擦） */

    metadata_dir_init(ftl->metadata_dir);

    if (checkpoint_write_initial(ftl->topo) != FTL_OK)
    {
        return FTL_ERROR;
    }

    return FTL_OK;
}

/* --------------------------------------------------------------------
 * ftl_boot_recovery
 *   功能：启动恢复总入口——判上次关机形态并分发到三条启动路径之一。
 *   参数：ftl —— FTL 实例（前置条件，ASSERT）；result —— 出参启动结果
 *         （boot_type / cp_sequence / journal_entries，出参地址前置条件，ASSERT）。
 *   返回：FTL_OK 成功；FTL_ERROR 子路径失败。
 *   实现要点：① 扫 Journal 环形块池取最新页（journal_scan_latest）——**只读不改状态**，
 *                故不调 journal_replay（后者会修改 journal 内部状态）。三条启动路径
 *                都要拿这份快照恢复 Journal RAM 写态（journal_resume）。
 *             ② checkpoint_find_latest 取最近快照，找不到 = 首次启动走 fresh_start。
 *             ③ 最新页 page_seq ≤ cp.journal_sequence ⇒ 干净关机；否则异常掉电。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_boot_recovery(struct ftl_context *ftl, ftl_boot_result_t *result)
{
    /* 前置条件（判据 2）：唯一调用者 main.c 传 &g_ftl[ch][ce][lun] 与栈上 &result。 */
    ASSERT(ftl != NULL);
    ASSERT(result != NULL);

    ftl_boot_result_t local_result;
    memset(&local_result, 0, sizeof(local_result));

    /* ① 扫 Journal 环形块池取最新页（只读）。
     *    判型要用它；三条路径也都要用它恢复写指针——journal_init 把写指针/序号复位到
     *    (block0, page0)/1，而环形块**并没有被擦**（journal_truncate 只预擦环形下一块），
     *    不接回去就会二次编程 + 序号回退（详见 journal_resume）。 */
    journal_page_ref_t ring;
    (void)journal_scan_latest(ftl->journal, &ring);

    /* ② 扫描 Checkpoint + 读取 块账快照（快照失败不触发格式化，只影响块账路径） */
    l2p_checkpoint_header_t cp;
    uint16_t                cp_idx = UINT16_MAX;

    if (checkpoint_find_latest(ftl, &cp, &cp_idx) != FTL_OK)
    {
        /* 无有效 Checkpoint → 出厂首次启动 */
        int ret = ftl_boot_fresh_start(ftl, &ring);
        if (ret == FTL_OK)
        {
            local_result.boot_type       = BOOT_FRESH_START;
            local_result.cp_sequence     = 0U;
            local_result.journal_entries = 0U;
            local_result.blocks_restored = 0U;
            *result                      = local_result;
        }
        return ret;
    }

    const bool blocks_valid = checkpoint_load_block_table(ftl, &cp, cp_idx, ftl->cp_job->block_words);

    if (!blocks_valid)
    {
        LOGW("[BLK] unavailable (rec=%u seq=%lu) — block accounting via F2\r\n",
             (unsigned)cp_idx,
             (unsigned long)cp.sequence);
    }

    /* 磨损计数（erase/read/ECC 健康）与块账一起在 CP 里；软状态，失败只退化 WL。 */
    if (!checkpoint_load_wear_table(ftl, &cp, cp_idx))
    {
        LOGW("[WEAR] snapshot unavailable (rec=%u seq=%lu) — counters start at 0\r\n",
             (unsigned)cp_idx,
             (unsigned long)cp.sequence);
    }

    /* ③ 判型：最新页**整页**都被 Checkpoint 覆盖 ⇒ 干净关机；否则异常掉电。
     *
     * ⚠ 判据必须是**整页**的 `page_seq + entry_count`，与 journal_replay 的回放过滤
     *   同口径。`cp.journal_sequence` 的语义是「下一个待分配序号」（Checkpoint job
     *   直接取 sequence_counter，journal_truncate 的注释亦如此声明）⇒ 被 CP 覆盖的条目
     *   是 seq < cp.journal_sequence。一页占 seq [page_seq, page_seq+entry_count-1]，
     *   全被覆盖 ⟺ page_seq + entry_count ≤ cp.journal_sequence。
     *
     * 旧判据只看页首（page_seq ≤ cp.journal_sequence）会**把恰在水位上的新页判成干净**：
     * Checkpoint 之后写入的第一页 page_seq 正好 == cp.journal_sequence，于是「写→掉电」
     * 后判 type=1、不回放、L2P 从 NAND 表加载（其中没有这批映射）⇒ 数据静默丢失。
     * 上板签名：`boot type=1 journal_entries=0` + `[F2] rebuild blocks=N valid_cw=0`
     * + `rd FAIL ... got 00000000`（数据页在 NAND 上，映射没了）。 */
    if ((ring.page_seq + (uint32_t)ring.entry_count) <= cp.journal_sequence)
    {
        /* 干净关机 */
        int ret = ftl_boot_clean_start(ftl, &cp, blocks_valid ? 1U : 0U, &local_result.blocks_restored, &ring);
        if (ret == FTL_OK)
        {
            local_result.boot_type       = BOOT_CLEAN_START;
            local_result.cp_sequence     = cp.sequence;
            local_result.journal_entries = 0U;
            *result                      = local_result;
        }
        return ret;
    }
    else
    {
        /* 异常掉电 */
        int ret = ftl_boot_power_failure(ftl, &cp, blocks_valid ? 1U : 0U, &local_result);
        if (ret == FTL_OK)
        {
            local_result.boot_type   = BOOT_POWER_FAILURE;
            local_result.cp_sequence = cp.sequence;
            *result                  = local_result;
        }
        return ret;
    }
}

/* ── 三条启动路径（fresh / clean / power-failure）── */

/* --------------------------------------------------------------------
 * ftl_boot_carve_pools
 *   功能：按几何顺序切分 Spare / OP / Free 池（无快照 或 快照失败时用）。
 *   实现要点：与旧 main.c 回填逐条一致——每 plane 先扣 Spare（50），再扣 OP（10%），
 *             其余进 Free。快照路径不调用它（池来自快照），故这里要求池计数为 0。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_boot_carve_pools(struct ftl_context *ftl)
{
    ASSERT(ftl != NULL);

    ftl_allocator_t *alloc = &ftl->alloc;

    ASSERT(alloc->free_count == 0U);
    for (uint8_t p = 0U; p < PLANE_NUM; p++)
    {
        ASSERT(alloc->op_count[p] == 0U);
        ASSERT(alloc->spare_count[p] == 0U);
    }

    uint16_t good_n = 0U;

    for (uint16_t gblock = 0U; gblock < FTL_BLOCK_TOTAL; gblock++)
    {
        const uint8_t  plane = ftl_gblock_plane(gblock);
        const uint16_t block = ftl_gblock_block(gblock);

        if (ftl_block_is_bad(&alloc->blocks[gblock]))
        {
            continue;
        }
        if (ftl_is_metadata_block(ftl->metadata_dir, plane, block))
        {
            continue;
        }

        good_n++;

        if (allocator_spare_try_push(alloc, plane, gblock))
        {
            continue;
        }
        if (allocator_op_try_push(alloc, plane, gblock))
        {
            continue;
        }
        allocator_free_block(alloc, gblock);
    }

    alloc->total_blocks = good_n;
    return FTL_OK;
}

/* --------------------------------------------------------------------
 * ftl_boot_fresh_start
 *   功能：出厂首次启动（无 Checkpoint）——cache 全 INVALID + carve + F2 重建，sequence 归零。
 *   参数：ftl —— FTL 实例（前置条件，由 ftl_boot_recovery 保证，不在此复检）；
 *         ring —— Journal 环形块池快照。
 *   返回：FTL_OK / FTL_ERROR。
 *   实现要点：① 失效**本 owner** 的 L2P cache 行与脏位（共享 cache 不能整体 re-init，
 *             否则会抹掉别的 LUN 刚回放的 DIRTY 行）。
 *             ② F2 重建把「用户数据 written 块」逐出 Free Pool（见下 ⚠）。
 *             ③ allocator 已由 ftl_init 初始化，此处不重复。
 *             ④ journal_resume 按环内最新页接回写指针（序号下限 1）。
 *             ⑤ checkpoint sequence 归零。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_boot_fresh_start(struct ftl_context *ftl, const journal_page_ref_t *ring)
{
    /* ① 只失效本 owner 的行/脏位（设备级 init 已清全体；这里覆盖格式化后的场景） */
    l2p_cache_invalidate_owner(ftl->cache, ftl_topo_dim(ftl->topo));

    /* ② 池回填（从 main.c 移入——块账快照恢复必须先于 carve，否则池条目重复）。
     *    fresh 无快照 ⇒ 走几何顺序 carve。 */
    (void)ftl_boot_carve_pools(ftl);

    /* ③ 全盘重建 Closed SB 列表（F2）。
     *    Free Pool 里躺着已编程的块，F2 把它们逐出并交 GC 擦除。
     *    ⚠ 代价：逐块读首页判空 + 对写过页读 META 反查（clean/PF 已由块账快照取代）。 */
    {
        ftl_rebuild_stats_t rebuild_stats;
        (void)ftl_rebuild_closed_sb_list(ftl, &rebuild_stats);
    }

    /* ④ Journal RAM 写态恢复：无 Checkpoint ⇒ 序号下限取 1（0 保留作「无序号」）。
     *    环形块里的旧页仍在（刚格式化过的介质为空，但「找不到 Checkpoint」也可能是
     *    Checkpoint 页损坏而环形块完好）⇒ 写指针必须按 NAND 实际内容接回，否则会对
     *    已编程页二次编程；序号从 1 重来还会让下次开机把旧页当新页回放、复活旧映射。 */
    journal_resume(ftl->journal, ring, 1U);

    /* ⑤ 复位 Checkpoint 定时基准（本实例 cp 游标已由 checkpoint_find_latest 置空） */
    ftl->cp_timer_start = 0U;

    return FTL_OK;
}

static void ftl_boot_apply_region_bitmap(ftl_context_t                 *ftl,
                                         const l2p_checkpoint_header_t *cp);

/* --------------------------------------------------------------------
 * ftl_boot_clean_start
 *   功能：干净关机恢复——块账快照有效时加载块账 + 池 + Closed 列表；否则 carve + F2。
 *   参数：ftl —— FTL 实例；cp —— 最近 Checkpoint Header；blocks_valid —— 块账快照是否已通过校验；
 *         blocks_restored —— 出参：1 = 走 快照路径；ring —— Journal 环形块池快照。
 *   返回：FTL_OK / FTL_ERROR。
 *   实现要点：clean 下 Checkpoint 覆盖全部 journal 条目，不需要回放；但必须
 *             journal_resume 接回写指针/序号（环形块并未被擦）。
 *             ⚠ 即使判定 clean，也要 probe 池块：本仓无关机 journal flush，
 *               「丢最后一页 journal」与 clean 在介质上不可区分。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_boot_clean_start(struct ftl_context            *ftl,
                                  const l2p_checkpoint_header_t *cp,
                                  uint8_t                        blocks_valid,
                                  uint8_t                       *blocks_restored,
                                  const journal_page_ref_t      *ring)
{
    ASSERT(ftl != NULL);
    ASSERT(cp != NULL);
    ASSERT(blocks_restored != NULL);
    ASSERT(ring != NULL);

    /* ① 共享 cache 已由设备级 init 置空；本路径只按 CP bitmap 约束本 owner 的行
     *    （首次 miss 时按需从 NAND L2P Table 加载）。 */
    uint8_t restored = 0U;

    if (blocks_valid != 0U)
    {
        if (ftl_recovery_restore_blocks(ftl, ftl->cp_job->block_words))
        {
            /* 预载 CP region，供 probe 的逐 CW L2P 反查复用（未覆盖区懒加载）。 */
            ftl_boot_apply_region_bitmap(ftl, cp);

            ftl_block_restore_stats_t stats;
            memset(&stats, 0, sizeof(stats));
            ftl_recovery_probe_blocks(ftl, ftl->cp_job->block_words, &stats);

            if (ftl_recovery_build_pools(ftl, ftl->cp_job->block_words))
            {
                restored = 1U;
                LOGI("[BLK] restore closed=%u log=%u free=%u op=%u spare=%u anomalies=%u valid_cw=%lu\r\n",
                     (unsigned)stats.closed_blocks,
                     (unsigned)stats.log_blocks,
                     (unsigned)allocator_free_count(&ftl->alloc),
                     (unsigned)allocator_op_total(&ftl->alloc),
                     (unsigned)allocator_spare_total(&ftl->alloc),
                     (unsigned)stats.anomalies,
                     (unsigned long)stats.valid_cw);
            }
        }
    }

    if (restored == 0U)
    {
        LOGW("[BLK] fallback to F2 (clean start)\r\n");
        ftl_recovery_reset_blocks(ftl);
        (void)ftl_boot_carve_pools(ftl);
        ftl_rebuild_stats_t rebuild_stats;
        (void)ftl_rebuild_closed_sb_list(ftl, &rebuild_stats);
    }

    /* ② Journal RAM 写态恢复（环形块未被擦，必须接回写指针/序号）。 */
    journal_resume(ftl->journal, ring, cp->journal_sequence);

    /* ③ Controller → Ready */
    ftl->cp_timer_start = 0U;
    *blocks_restored    = restored;

    return FTL_OK;
}

/* --------------------------------------------------------------------
 * ftl_boot_apply_region_bitmap
 *   功能：按 checkpoint region_bitmap 局部加载 L2P cache（F4）。
 *   参数：ftl —— FTL 实例；cp —— 最近 Checkpoint 头（含 region_bitmap）。
 *   返回：无。
 *   实现要点：对**本 owner 的** cache 行做三条约束——① DIRTY 行跳过；
 *             ② bitmap 置位区预载 CLEAN；③ 未置位区置 INVALID（懒加载）。
 *             共享 cache 下还要跳过别的 LUN 的行与在飞行（FLUSHING/LOADING），
 *             并把全局 region 映射成本 die 的局部 region 再查 CP 位图。
 *             必须在 journal_replay 之后调用。
 *
 * F4：掉电恢复时，checkpoint 只保证「bitmap 置位的 region」的 NAND 页等于
 * CP 时刻快照。boot 后对 cache 行做约束：
 *
 *   ① DIRTY 行（journal_replay 刚写入的最新数据）→ 一律跳过。
 *      CLEAN 声明"cache==NAND"是假的（NAND 是 CP 时刻旧值，cache 是
 *      post-CP 回放值）；INVALID 则直接丢弃回放数据。保持 DIRTY，
 *      由后台 flush 逐步写回。
 *   ② bitmap 置位区：显式从 NAND 加载 region 页 → 置 CLEAN。
 *      该区页 = CP 时刻快照（CP 时已 flush 落盘），预载可复用给
 *      ftl_rebuild_closed_sb_list 的反查。不可直接置 CLEAN——line 的
 *      entries 仍是 init 时全 NAND_PBA_INVALID，read 会对有效映射返回假 INVALID。
 *      目录条目为 INVALID（该 region 从未写过 / 刷盘未成功）⇒ 无页可预载，
 *      保持 INVALID 交给懒加载（合成全 INVALID 行，无需 NAND 读）。
 *   ③ bitmap 未置位区：置 INVALID，下一访问懒加载 CP 时刻快照。
 *      该区未在本次 CP 落盘，NAND 页为更旧值/从未写，懒加载即正确。
 *
 * 必须在 journal_replay 之后调用（replay 前 cache 刚 init，所有 line
 * region_id=0，先强制会误匹配全部行）。
 * -------------------------------------------------------------------- */
static void ftl_boot_apply_region_bitmap(ftl_context_t                 *ftl,
                                         const l2p_checkpoint_header_t *cp)
{
    const uint32_t dim = ftl_topo_dim(ftl->topo);

    for (uint16_t i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        l2p_cache_line_t *line = &ftl->cache->lines[i];

        if (line->state == L2P_CACHE_DIRTY)
        {
            continue; /* ① 回放数据，保持 DIRTY 待后台 flush */
        }
        /* 在飞行有命令在飞：重写状态会让完成找不到行、泄漏在飞计数。 */
        if ((line->state == L2P_CACHE_FLUSHING) || (line->state == L2P_CACHE_LOADING))
        {
            continue;
        }
        /* 共享 cache：只处理本 owner 的行。INVALID 行 region_id=0，只归 dim0，
         * 保持 DIM=1 的既有行为；别的 LUN 的行由它们自己的恢复处理。 */
        if (!l2p_region_owned_by(line->region_id, dim))
        {
            continue;
        }

        const uint32_t local_region = l2p_global_to_local_region(line->region_id);
        ASSERT(local_region < L2P_REGIONS_PER_INSTANCE);

        const uint32_t word      = cp->region_bitmap[local_region / 32U];
        const uint32_t mask      = (1UL << (local_region & 31U));
        const int      in_bitmap = ((word & mask) != 0U);

        if (in_bitmap)
        {
            /* ② 预载 bitmap 区（仅 CLEAN/INVALID 行；DIRTY 已在上方跳过） */
            if (line->state != L2P_CACHE_CLEAN)
            {
                nand_pba_t region_addr = metadata_dir_lookup(ftl->metadata_dir,
                                                             METADATA_TYPE_L2P_REGION,
                                                             (uint16_t)local_region);

                if (region_addr == NAND_PBA_INVALID)
                {
                    /* 目录说未映射：没有页可预载。置 INVALID 交给懒加载——那条路径
                     * 会合成全 NAND_PBA_INVALID 的 CLEAN 行，且不产生 NAND 读。 */
                    line->state = L2P_CACHE_INVALID;
                    continue;
                }

                if (ftl_sync_read_page(region_addr, line->entries, L2P_REGION_SIZE, FTL_PAGE_CFG_META()) == FTL_OK)
                {
                    line->state = L2P_CACHE_CLEAN;
                }
                else
                {
                    /* 读失败保持 INVALID（entries 仍全 INVALID，懒加载兜底） */
                    line->state = L2P_CACHE_INVALID;
                }
            }
        }
        else
        {
            /* ③ 未覆盖区：INVALID → 下一访问懒加载 CP 时刻快照 */
            line->state = L2P_CACHE_INVALID;
        }
    }
}

/* --------------------------------------------------------------------
 * ftl_boot_power_failure
 *   功能：异常掉电恢复——cache 全 INVALID → Journal 回放 → F4 bitmap 局部加载 → F2 重建。
 *   参数：ftl —— FTL 实例；cp —— 最近 Checkpoint 头；result —— 出参启动结果。
 *   返回：FTL_OK / FTL_ERROR 参数非法。
 *   实现要点：② journal_replay 回放所有**未全被 CP 覆盖**的页（整页判据
 *             `page_seq + entry_count > cp_sequence`；F10：只回放最大一页会静默丢
 *             ≥2 页条目）；④ F2 重建顺带替代旧 hot_sb/cold_sb
 *             写指针扫描（原实现被 is_open 门控恒假）。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_boot_power_failure(struct ftl_context            *ftl,
                                    const l2p_checkpoint_header_t *cp,
                                    uint8_t                        blocks_valid,
                                    ftl_boot_result_t             *result)
{
    uint32_t cp_sequence = cp->journal_sequence;

    /* ① 共享 cache 已由设备级 init 置空；journal 回放把本 owner 的映射写进 cache。
     *    ⚠ 不能在此整体 re-init：会抹掉前面 LUN 刚回放的 DIRTY 行与全局脏位。 */

    /* ② 块账快照恢复（若有效）：先恢复块账，回放时才能同步维护 valid_count。 */
    uint8_t                    blocks_applied = 0U;
    journal_replay_block_ctx_t block_ctx;
    memset(&block_ctx, 0, sizeof(block_ctx));

    if ((blocks_valid != 0U) && ftl_recovery_restore_blocks(ftl, ftl->cp_job->block_words))
    {
        blocks_applied        = 1U;
        block_ctx.materialize = recovery_materialize_pba;
        block_ctx.ctx         = ftl;
        block_ctx.ok          = 1U;
    }

    /* ③ Journal 回放：映射写入 L2P Cache；block_ctx 有效时同步记 valid 增量。
     *    F10：按 (page_seq, entry_index) 递增序回放所有
     *    page_seq + entry_count > cp_sequence 的页。 */
    uint32_t entry_count = 0U;
    (void)journal_replay(ftl->journal,
                         ftl->cache,
                         cp_sequence,
                         &entry_count,
                         (blocks_applied != 0U) ? &block_ctx : NULL);

    /* ④ 按 Checkpoint region_bitmap 局部加载（F4）：跳过 DIRTY 回放行，
     *    bitmap 区预载 CLEAN，非 bitmap 区 INVALID（懒加载 CP 时刻快照）。 */
    ftl_boot_apply_region_bitmap(ftl, cp);

    /* ⑤ 块账快照收尾：探测池块 + 唯一入池点；任一步失败 ⇒ 丢弃快照结果走 F2。 */
    uint8_t restored = 0U;

    if ((blocks_applied != 0U) && (block_ctx.ok != 0U))
    {
        ftl_block_restore_stats_t stats;
        memset(&stats, 0, sizeof(stats));
        ftl_recovery_probe_blocks(ftl, ftl->cp_job->block_words, &stats);

        if (ftl_recovery_build_pools(ftl, ftl->cp_job->block_words))
        {
            restored = 1U;
            LOGI("[BLK] restore closed=%u log=%u free=%u op=%u spare=%u anomalies=%u valid_cw=%lu\r\n",
                 (unsigned)stats.closed_blocks,
                 (unsigned)stats.log_blocks,
                 (unsigned)allocator_free_count(&ftl->alloc),
                 (unsigned)allocator_op_total(&ftl->alloc),
                 (unsigned)allocator_spare_total(&ftl->alloc),
                 (unsigned)stats.anomalies,
                 (unsigned long)stats.valid_cw);
        }
    }

    if (restored == 0U)
    {
        LOGW("[BLK] fallback to F2 (power failure, ctx_ok=%u)\r\n", (unsigned)block_ctx.ok);
        ftl_recovery_reset_blocks(ftl);
        (void)ftl_boot_carve_pools(ftl);

        /* F2 用已回放的 L2P 反查；与旧路径同序（replay → F4 → F2）。 */
        ftl_rebuild_stats_t rebuild_stats;
        (void)ftl_rebuild_closed_sb_list(ftl, &rebuild_stats);
    }

    /* ⑥ Controller → Ready */
    ftl->cp_timer_start = 0U;

    /* ⑦ 输出结果 */
    result->boot_type       = BOOT_POWER_FAILURE;
    result->cp_sequence     = cp_sequence;
    result->journal_entries = entry_count;
    result->blocks_restored = restored;

    return FTL_OK;
}

/* ══════════════════════════════════════════════════════════════════
 * F2 — 掉电重建 closed_sb_list（全 NAND 扫描）
 *
 * 目标：掉电重启后，已写满（Closed）的 Super Block 不在 closed_sb_list，
 * BST 里 state 停在 FREE（allocator_init 统一置位）。后果：
 *   ① GC/WL/rr_predictive_scan 看不到这些块 → 容量泄漏
 *   ② main.c free pool 回填把它们当空白块 → 复用未擦除半满块
 *
 * 本函数逐 plane 逐 Block 扫描 NAND，判空 + L2P 反查，重建 closed 列表与 BST 有效位图。
 * Policy B：两 plane 的块独立分配、不再同号，故按**单物理块**建 Closed 条目（另一
 * plane 记 FTL_BLOCK_NO_PLANE 哨兵），配对无需恢复。
 * ══════════════════════════════════════════════════════════════════ */

/* 判一页是否为全 0xFF 空白页。buffer 复用单个大缓冲——ftl_sync_read_page_raw 按
 * PAGE_MAIN_SIZE（2048B）拷贝，缓冲必须够大（历史上的 buf[16] 版本即栈溢出）。
 *
 * ⚠ **读失败一律判「非空」**（保守方向）。ftl_sync_read_page_raw 刻意不判定 NAND 失败
 *   （坏页也要拿到原始位），故它返回 FTL_ERROR 只代表**命令本身**没走通（提交失败 /
 *   超时 / 无 buffer），此时契约规定 buffer 内容**未定义**。若把这种页当空白页，
 *   `ftl_recovery_scan_write_pointer` 会对该块返回 0 ⇒ 块被当成"没写过"**留在 Free Pool**
 *   ⇒ 既可能被 allocator 拿去复用覆盖活数据，又让 L2P 指向一个 valid_count 恒 0 的块
 *   （`bst.h` 断言的成因，与 v0.69 修的 plane 掩码同一类）。
 *   判「非空」的代价：真为空的块会被拉出 Free Pool 交 GC 多擦一次——不丢数据。 */
bool ftl_recovery_page_is_blank(nand_pba_t pba, uint8_t *raw_buf)
{
    memset(raw_buf, 0xFF, PAGE_SIZE);
    if (ftl_sync_read_page_raw(pba, raw_buf) != FTL_OK)
    {
        return false; /* 命令没走通 ⇒ 内容不可知 ⇒ 一律判非空 */
    }

    /* 每 CW 的 META(4B)+CRC(4B)+前 8B 数据，共 16B。全 FF 页 META/CRC 也全 FF。 */
    for (uint8_t i = 0U; i < CW_NUM; i++)
    {
        const uint8_t *p = raw_buf + (uint32_t)i * FC_BUFFER_POOL_CW_SIZE;
        for (uint8_t j = 0U; j < 16U; j++)
        {
            if (p[j] != 0xFFU)
            {
                return false;
            }
        }
    }

    return true;
}

/* 扫出某 Block 写指针：第一个全 FF 页；全非空返回 PAGES_PER_BLOCK */
uint8_t ftl_recovery_scan_write_pointer(const ftl_context_t *ftl, uint8_t plane, uint16_t block, uint8_t *raw_buf)
{
    for (uint8_t page = 0U; page < PAGES_PER_BLOCK; page++)
    {
        nand_pba_t addr = pba_make(ftl->topo.ch, ftl->topo.ce, ftl->topo.lun, block, plane, page, 0U);

        if (ftl_recovery_page_is_blank(addr, raw_buf))
        {
            return page; /* 第一个空白页 = 写指针 */
        }
    }

    return (uint8_t)PAGES_PER_BLOCK; /* 全写满 */
}

/* 写过的物理块：进 Closed 列表（退化单 plane 条目）+ 逐 CW L2P 双校验回填 valid_count
 * （只计 L2P 仍指向本页本 CW 的真有效 CW，覆盖/torn 页不计；v0.60 起无常驻位图）。
 *
 * Policy B 下配对不再需要被恢复——两 plane 的块各自独立归位即可，故本函数按
 * **单个物理块** 建一条 Closed 条目，另一 plane 记 FTL_BLOCK_NO_PLANE 哨兵。 */
void ftl_recovery_close_block(ftl_context_t       *ftl,
                              uint8_t              plane,
                              uint16_t             block,
                              uint8_t              pages_written,
                              uint8_t              temperature,
                              ftl_rebuild_stats_t *stats)
{
    /* 前置条件（判据 2）：pages_written 来自 ftl_recovery_scan_write_pointer，调用者已跳过 0。 */
    ASSERT((pages_written > 0U) && (pages_written <= PAGES_PER_BLOCK));

    /* 退化单 plane 条目：本 plane 记块号，其余 plane 记哨兵 */
    uint16_t plane_blocks[PLANE_NUM];

    for (uint8_t p = 0U; p < PLANE_NUM; p++)
    {
        plane_blocks[p] = FTL_BLOCK_NO_PLANE;
    }
    plane_blocks[plane] = block;

    ftl_allocator_t *alloc = &ftl->alloc;

    uint16_t sb_idx = allocator_add_closed_sb(alloc, plane_blocks, temperature);

    if (stats != NULL)
    {
        stats->blocks_rebuilt++;
    }

    /* 逐页：gc_read_page_with_lba 取 LBA + **逐 CW** l2p 反查。
     * ⚠ 只扫已写页 [0, pages_written)：写指针之后的空白页没有 META，
     *   读回来也解不出 LBA，纯白读。
     * ⚠ [待上板] 掉电重启后核对 `[F2] rebuild ... partial=... valid_cw=...`：
     *   valid_cw 应与改前一致（空白页本就不贡献有效 CW），partial 块的读页数减少。 */
    ftl_block_t *b = &alloc->blocks[ftl_gblock(plane, block)];
    for (uint16_t page = 0U; page < pages_written; page++)
    {
        nand_pba_t pba = super_block_page_pba(ftl->topo, allocator_closed_at(alloc, sb_idx), plane, page);

        /* 读失败 / 全页无写过 CW → 该页不可用，跳过。
         * CW 级映射：逐 CW 从页内 META（= OOB 反向映射）取回扇区 LBA。 */
        lba_t cw_lba[CW_NUM];
        if (gc_read_page_with_lba(pba, cw_lba) != FTL_OK)
        {
            continue;
        }

        for (uint8_t cw = 0U; cw < CW_NUM; cw++)
        {
            /* ⚠ 先校验不可信 META LBA 再使用：FTL_LBA_INVALID / 越界都不查表
             * （H29；全全局化后坏值可能落进另一个合法区间）。 */
            if ((cw_lba[cw] == FTL_LBA_INVALID) || ((uint64_t)cw_lba[cw] >= L2P_GLOBAL_LBA_MAX) ||
                !lba_routes_to(cw_lba[cw], ftl->topo))
            {
                continue;
            }

            /* ⚠ F2 重建 = 结构性同步路径（开机、superloop 未建立）：用同步逃生口。
             * 读得 INVALID 与"未映射"同义 ⇒ 该 CW 不计入有效数（与旧实现同）。 */
            nand_pba_t cur = NAND_PBA_INVALID;

            (void)l2p_cache_read_sync(ftl->cache, cw_lba[cw], &cur);
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

/* --------------------------------------------------------------------
 * ftl_rebuild_closed_sb_list
 *   功能：F2 掉电重建——逐 plane 逐块扫 NAND，把「写过的物理块」拉出 Free Pool 并建
 *         Closed 条目，同时逐 CW L2P 双校验回填 valid_count。
 *   参数：ftl —— FTL 实例；stats —— 出参统计（blocks_rebuilt / valid_cw /
 *         partial_blocks，可为 NULL）。
 *   返回：FTL_OK / FTL_ERROR（ftl 或 alloc->blocks 为空）。
 *   实现要点：Policy B 下两 plane 的块不再同号 ⇒ 按**单物理块**建 Closed 条目
 *             （另一 plane 记 FTL_BLOCK_NO_PLANE 哨兵），配对无需、也无法恢复。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_rebuild_closed_sb_list(struct ftl_context  *ftl,
                                        ftl_rebuild_stats_t *stats)
{
    uint8_t raw_buf[PAGE_SIZE];

    ftl_allocator_t *alloc = &ftl->alloc;

    if (stats != NULL)
    {
        memset(stats, 0, sizeof(*stats));
    }

    /* Policy B：**逐 plane 独立扫描**——两 plane 的块不再同号。配对只在写入时由
     * allocator 创建，之后从不被查找（multi-plane 读的两个 PBA 各自从 L2P 查；
     * GC/WL 逐 plane 工作），故重建只需把"写过的物理块各自归位"，无需、也无法
     * 恢复"哪两块曾是一对"（配对取决于 GC 历史，不在任何持久化位图里）。 */
    for (uint8_t plane = 0U; plane < PLANE_NUM; plane++)
    {
        for (uint16_t block = 0U; block < BLOCKS_PER_PLANE; block++)
        {
            uint16_t     g = ftl_gblock(plane, block);
            ftl_block_t *b = &alloc->blocks[g];

            /* ① 坏块 / 元数据块跳过（判据与 main.c free pool 回填一致，per-plane 块号） */
            if (ftl_block_is_bad(b) || ftl_is_metadata_block(ftl->metadata_dir, plane, block))
            {
                continue;
            }

            /* ② 判空找写指针（第一个全 FF 页） */
            uint8_t np = ftl_recovery_scan_write_pointer(ftl, plane, block, raw_buf);

            /* ③ 未写过（全 FF）→ 保持 FREE */
            if (np == 0U)
            {
                continue;
            }

            /* 写过：从 free pool 拉出（防止开机后当作空白块复用覆盖） */
            allocator_remove_from_free(alloc, g);

            /* ④ 写过的块一律进 Closed——含写满块与掉电时的部分写块。
             *    部分写块不再"续写"（旧 recover_open_sb 行为）：per-block 下无法确定两个
             *    部分写块是否曾是一对，强行恢复 Open SB 会让后续 multi-plane 写落到错误的
             *    plane1 块。整块交 GC 迁移其有效页——安全性等价（块不会被当空白复用），
             *    代价是掉电后多一次 GC 搬移。 */
            ftl_recovery_close_block(ftl, plane, block, np, FTL_TEMP_HOT, stats);

            if ((stats != NULL) && (np < PAGES_PER_BLOCK))
            {
                stats->partial_blocks++;
            }
        }
    }

    /* ⑤ 写过的块已从 free pool 拉出（free_count 由 allocator_remove_from_free 自行维护，
     * 无派生计数需要重同步——P1-3 删除了 pre_cond_count 这个 free_count 的镜像） */

    /* 观测口（上板验证用）：valid_cw 是本轮重建**回填**的有效 CW 数。
     * ⚠ 它是后续覆盖写的安全前提——`ftl_confirm_write` 对旧映射所在块做
     *   `ftl_block_sub_valid`（bst.h 内断言 `valid_count > 0`）。若介质上确有数据
     *   而 valid_cw == 0，则重建没认到这些 CW，下一次覆盖写该 LBA 会触发断言。 */
    if (stats != NULL)
    {
        LOGI("[F2] rebuild blocks=%u partial=%u valid_cw=%u\r\n",
             (unsigned)stats->blocks_rebuilt,
             (unsigned)stats->partial_blocks,
             (unsigned)stats->valid_cw);
    }

    return FTL_OK;
}

/* ══════════════════════════════════════════════════════════════════
 * 块账快照恢复（Checkpoint 的 16-bit/块 word）
 *
 * 格式编码在 checkpoint.h / checkpoint.c（记录格式的 owner）；本节只做**恢复编排**：
 *   restore（state/valid/temp + Closed 列表 + LOG 对账）
 *   → journal replay 的 materialize 回调
 *   → probe（CP 后分配但 journal 丢页的池块）
 *   → build_pools（唯一入池点）
 * 任一步结构非法 ⇒ 调用者 `ftl_recovery_reset_blocks` + carve + F2。
 * ══════════════════════════════════════════════════════════════════ */

/* 固定元数据区（分区表几何；动态 LOG 块不在此列） */
static bool recovery_is_fixed_block(uint16_t g)
{
    /* 必须按分区的 plane_mask 判：plane 1 的 0..41/46.. 已是用户数据，
     * 用 "block < FTL_METADATA_BLOCK_END" 会把它们误当固定元数据块跳过。 */
    return ftl_is_metadata_block(NULL, ftl_gblock_plane(g), ftl_gblock_block(g));
}

/* 把一个物理块发成退化单 plane Closed 条目（与 F2 同口径；Policy B 不要求配对）。 */
static void recovery_add_closed(ftl_context_t *ftl, uint16_t g, uint8_t temperature)
{
    uint16_t plane_blocks[PLANE_NUM];

    for (uint8_t p = 0U; p < PLANE_NUM; p++)
    {
        plane_blocks[p] = FTL_BLOCK_NO_PLANE;
    }
    plane_blocks[ftl_gblock_plane(g)] = ftl_gblock_block(g);

    (void)allocator_add_closed_sb(&ftl->alloc, plane_blocks, temperature);
}

bool ftl_recovery_restore_blocks(ftl_context_t *ftl, uint16_t words[FTL_BLOCK_TOTAL])
{
    ASSERT(ftl != NULL);
    ASSERT(words != NULL);

    ftl_allocator_t *alloc = &ftl->alloc;

    /* Pass 1：state / valid / temp / sb_id（不做池、不发 Closed 条目）。 */
    for (uint16_t g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        const uint16_t w     = words[g];
        const uint16_t valid = checkpoint_block_word_valid(w);
        const uint8_t  st    = checkpoint_block_word_state(w);
        const uint8_t  temp  = checkpoint_block_word_temp(w);
        const uint8_t  pool  = checkpoint_block_word_pool(w);

        ftl_block_t *b = &alloc->blocks[g];

        if (recovery_is_fixed_block(g))
        {
            /* 固定元数据块不参与块账；但 BAD 是终态（BBT），不得被清掉。 */
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

        /* BBT 已判 BAD：终态优先，快照不得复活它。 */
        if (ftl_block_is_bad(b))
        {
            ftl_block_clear_valid(b);
            ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
            continue;
        }

        if ((valid > CP_BLOCK_WORD_MAX_VALID) || (st > (uint8_t)FTL_BLOCK_STATE_BAD) ||
            (temp > 1U) || (pool > CP_BLOCK_POOL_SPARE))
        {
            LOGE("[BLK] invalid word g=%u w=0x%04x\r\n", (unsigned)g, (unsigned)w);
            return false;
        }
        if ((pool != CP_BLOCK_POOL_NONE) && (st != (uint8_t)FTL_BLOCK_STATE_NONE))
        {
            LOGE("[BLK] g=%u pool=%u state=%u conflict\r\n", (unsigned)g, (unsigned)pool, (unsigned)st);
            return false;
        }
        if ((st == (uint8_t)FTL_BLOCK_STATE_FREE) && (valid != 0U))
        {
            LOGE("[BLK] g=%u FREE with valid=%u\r\n", (unsigned)g, (unsigned)valid);
            return false;
        }

        switch ((ftl_block_state_t)st)
        {
            case FTL_BLOCK_STATE_BAD:
                /* 快照的 BAD 与 BBT 的并集；bad_count 只对新增项加一，
                 * 并置 dirty 让后台 bbt_kick 把并集重新落盘。 */
                ftl_block_set_bad(b);
                ftl_block_clear_valid(b);
                ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
                alloc->bad_count++;
                alloc->bbt.dirty = 1U;
                break;

            case FTL_BLOCK_STATE_LOG:
                /* Directory 在 Pass 2 对账（它才是动态块身份的权威）。 */
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
                /* v1：Open SB 不续写，统一降级为 Closed（与 F2 同语义）。 */
                ftl_block_set_state(b, FTL_BLOCK_STATE_CLOSED);
                ftl_block_set_valid(b, valid);
                ftl_block_set_temp(b, temp);
                ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
                break;

            case FTL_BLOCK_STATE_FREE:
            case FTL_BLOCK_STATE_NONE:
                if ((st == (uint8_t)FTL_BLOCK_STATE_NONE) && (pool == CP_BLOCK_POOL_NONE))
                {
                    LOGE("[BLK] g=%u NONE with no pool tag\r\n", (unsigned)g);
                    return false;
                }
                /* FREE / OP / SPARE 一律先留 NONE：build_pools 之后才入池
                 * （allocator_free_block 拒绝重复归还，预置 FREE 会触发闸门）。 */
                ftl_block_set_state(b, FTL_BLOCK_STATE_NONE);
                ftl_block_clear_valid(b);
                ftl_block_set_temp(b, 0U);
                ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
                break;

            default:
                return false;
        }
    }

    /* Pass 2：Metadata Directory 是动态 LOG 块的唯一权威。 */
    for (uint16_t g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        if (recovery_is_fixed_block(g))
        {
            continue;
        }
        ftl_block_t *b = &alloc->blocks[g];

        if (ftl_block_is_bad(b))
        {
            continue;
        }
        if (ftl->metadata_dir->l2p_live[g] > 0U)
        {
            if (ftl_block_state(b) != FTL_BLOCK_STATE_LOG)
            {
                LOGW("[BLK] g=%u state=%u but Directory says LOG — Directory wins\r\n",
                     (unsigned)g,
                     (unsigned)ftl_block_state(b));
            }
            ftl_block_set_state(b, FTL_BLOCK_STATE_LOG);
            ftl_block_clear_valid(b);
            ftl_block_set_temp(b, 0U);
            ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
            words[g] = checkpoint_block_word_pack(0U, (uint8_t)FTL_BLOCK_STATE_LOG, 0U, CP_BLOCK_POOL_NONE);
        }
    }

    /* Pass 3：Closed 列表（退化单 plane；Open 已在 Pass 1 降级）。 */
    for (uint16_t g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        if (recovery_is_fixed_block(g))
        {
            continue;
        }
        ftl_block_t *b = &alloc->blocks[g];

        if (ftl_block_is_bad(b) || (ftl_block_state(b) != FTL_BLOCK_STATE_CLOSED))
        {
            continue;
        }
        recovery_add_closed(ftl, g, b->temp);
    }

    return true;
}

void ftl_recovery_probe_blocks(ftl_context_t *ftl, uint16_t words[FTL_BLOCK_TOTAL], ftl_block_restore_stats_t *stats)
{
    ASSERT(ftl != NULL);
    ASSERT(words != NULL);

    ftl_allocator_t *alloc = &ftl->alloc;

    if (stats != NULL)
    {
        memset(stats, 0, sizeof(*stats));
        stats->closed_blocks = alloc->closed_sb_count; /* restore / replay 已建的条目 */
    }

    uint8_t raw_buf[PAGE_SIZE];

    for (uint16_t g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        if (recovery_is_fixed_block(g))
        {
            continue;
        }

        ftl_block_t *b = &alloc->blocks[g];

        if (ftl_block_is_bad(b))
        {
            continue;
        }

        const uint8_t           plane = ftl_gblock_plane(g);
        const uint16_t          block = ftl_gblock_block(g);
        const ftl_block_state_t st    = ftl_block_state(b);

        if (st == FTL_BLOCK_STATE_LOG)
        {
            if (ftl->metadata_dir->l2p_live[g] > 0U)
            {
                if (stats != NULL)
                {
                    stats->log_blocks++;
                }
                continue; /* Directory 仍引用：活的 L2P 日志块 */
            }

            /* 陈旧 LOG（已不被 Directory 引用）：全页判空。
             * ⚠ 不能只看 page 0——元数据游标在写失败时会跳过该页。 */
            bool any = false;

            for (uint8_t page = 0U; page < PAGES_PER_BLOCK; page++)
            {
                nand_pba_t pba = pba_make(ftl->topo.ch, ftl->topo.ce, ftl->topo.lun, block, plane, page, 0U);

                if (!ftl_recovery_page_is_blank(pba, raw_buf))
                {
                    any = true;
                    break;
                }
            }

            if (!any)
            {
                /* 已被擦除：交回 Free Pool。 */
                ftl_block_set_state(b, FTL_BLOCK_STATE_NONE);
                ftl_block_clear_valid(b);
                ftl_block_set_temp(b, 0U);
                ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
                words[g] = checkpoint_block_word_pack(0U, (uint8_t)FTL_BLOCK_STATE_FREE, 0U, CP_BLOCK_POOL_NONE);
            }
            else
            {
                /* 残留的是 L2P 页而非用户数据：**不要**按用户 META 反查
                 * （会把元数据页误当映射），直接发 Closed + valid=0 交 GC 擦除。 */
                ftl_block_set_state(b, FTL_BLOCK_STATE_CLOSED);
                ftl_block_clear_valid(b);
                ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
                recovery_add_closed(ftl, g, FTL_TEMP_HOT);
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

        const uint16_t w    = words[g];
        const uint8_t  pool = checkpoint_block_word_pool(w);
        const uint8_t  ws   = checkpoint_block_word_state(w);

        if (!((ws == (uint8_t)FTL_BLOCK_STATE_FREE) || (pool == CP_BLOCK_POOL_OP) ||
              (pool == CP_BLOCK_POOL_SPARE)))
        {
            continue; /* 不是池块（不应发生；build_pools 会拒绝） */
        }

        /* CP 后被分配、但 journal 条目可能整页丢失的块：page 0 非空即异常。
         * 写失败会造成页内空洞，故这里只做触发判据；补算 valid 时按全块扫。
         * ⚠ 残余风险：page 0 写失败、后续页写成功、且相关 journal 条目全部丢失时，
         *   本探测会漏判。窗口极窄（要求 GC 首页写失败 + 末页 journal 丢失），
         *   且被复用的块内容已无 L2P 引用；接受并在注释登记。 */
        nand_pba_t pba0 = pba_make(ftl->topo.ch, ftl->topo.ce, ftl->topo.lun, block, plane, 0U, 0U);

        if (ftl_recovery_page_is_blank(pba0, raw_buf))
        {
            continue;
        }

        ftl_block_set_state(b, FTL_BLOCK_STATE_CLOSED);
        ftl_block_clear_valid(b);
        ftl_block_set_sb(b, FTL_BLOCK_NO_SB);

        ftl_rebuild_stats_t rebuild;
        memset(&rebuild, 0, sizeof(rebuild));
        ftl_recovery_close_block(ftl, plane, block, (uint8_t)PAGES_PER_BLOCK, FTL_TEMP_HOT, &rebuild);

        if (stats != NULL)
        {
            stats->anomalies++;
            stats->valid_cw += rebuild.valid_cw;
        }
    }
}

bool ftl_recovery_build_pools(ftl_context_t *ftl, const uint16_t words[FTL_BLOCK_TOTAL])
{
    ASSERT(ftl != NULL);
    ASSERT(words != NULL);

    ftl_allocator_t *alloc = &ftl->alloc;

    /* 此时不应有池条目（restore 不发、replay/probe 只建 Closed）。 */
    ASSERT(alloc->free_count == 0U);
    for (uint8_t p = 0U; p < PLANE_NUM; p++)
    {
        ASSERT(alloc->op_count[p] == 0U);
        ASSERT(alloc->spare_count[p] == 0U);
    }

    uint16_t good = 0U;

    for (uint16_t g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        if (recovery_is_fixed_block(g))
        {
            continue;
        }

        ftl_block_t *b = &alloc->blocks[g];

        if (ftl_block_is_bad(b))
        {
            continue;
        }

        const uint8_t  plane = ftl_gblock_plane(g);
        const uint16_t block = ftl_gblock_block(g);

        if (ftl_is_metadata_block(ftl->metadata_dir, plane, block))
        {
            continue; /* 固定区（已在上面跳过）/ 活的动态 LOG 块 */
        }

        good++;

        if (ftl_block_state(b) != FTL_BLOCK_STATE_NONE)
        {
            continue; /* CLOSED / RETIRED / 陈旧 LOG（已由 probe 处理） */
        }

        const uint16_t w    = words[g];
        const uint8_t  pool = checkpoint_block_word_pool(w);
        const uint8_t  st   = checkpoint_block_word_state(w);

        if ((st == (uint8_t)FTL_BLOCK_STATE_FREE) && (pool == CP_BLOCK_POOL_NONE))
        {
            allocator_free_block(alloc, g);
        }
        else if (pool == CP_BLOCK_POOL_OP)
        {
            if (!allocator_op_try_push(alloc, plane, g))
            {
                LOGE("[BLK] OP pool overflow at g=%u\r\n", (unsigned)g);
                return false;
            }
        }
        else if (pool == CP_BLOCK_POOL_SPARE)
        {
            if (!allocator_spare_try_push(alloc, plane, g))
            {
                LOGE("[BLK] Spare pool overflow at g=%u\r\n", (unsigned)g);
                return false;
            }
        }
        else
        {
            LOGE("[BLK] g=%u state=0x%04x not a pool block\r\n", (unsigned)g, (unsigned)w);
            return false;
        }
    }

    alloc->total_blocks = good;
    return true;
}

void ftl_recovery_reset_blocks(ftl_context_t *ftl)
{
    ASSERT(ftl != NULL);

    ftl_allocator_t *alloc = &ftl->alloc;

    for (uint16_t g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        ftl_block_t *b = &alloc->blocks[g];

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
    memset(alloc->op_count, 0, sizeof(alloc->op_count));
    memset(alloc->spare_count, 0, sizeof(alloc->spare_count));
    alloc->closed_sb_count = 0U;
    alloc->total_blocks    = 0U;

    memset(&alloc->hot_sb, 0, sizeof(alloc->hot_sb));
    memset(&alloc->cold_sb, 0, sizeof(alloc->cold_sb));
    alloc->hot_sb.temperature  = FTL_TEMP_HOT;
    alloc->cold_sb.temperature = FTL_TEMP_COLD;
}

/* journal_replay 的 materialize 回调：把 PBA 所属块转成「可记账」状态。
 * 池块（FREE/OP/SPARE）⇒ Closed；BAD 忽略；固定/活 LOG 块 ⇒ false（回放不一致）。 */
static bool recovery_materialize_pba(void *ctx, nand_pba_t pba)
{
    ftl_context_t *ftl = (ftl_context_t *)ctx;

    ASSERT(ftl != NULL);

    ftl_allocator_t *alloc = &ftl->alloc;
    const uint16_t   g     = ftl_pba_gblock(pba);

    if (g >= FTL_BLOCK_TOTAL || recovery_is_fixed_block(g))
    {
        return false;
    }

    ftl_block_t *b = &alloc->blocks[g];

    if (ftl_block_is_bad(b))
    {
        return true; /* 坏块不记账；数据已不可信 */
    }
    if (ftl_is_metadata_block(ftl->metadata_dir, ftl_gblock_plane(g), ftl_gblock_block(g)))
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
            /* 快照说 LOG、但 Directory 已不引用（上面的 ftl_is_metadata_block 为假）
             * ⇒ 该块在 CP 后被回收并复用；journal 指向它就是新用户数据。
             * 按陈旧 LOG 处理：转 Closed（旧 L2P 页交给 GC 擦除）。 */
            ftl_block_set_state(b, FTL_BLOCK_STATE_CLOSED);
            ftl_block_clear_valid(b);
            ftl_block_set_temp(b, FTL_TEMP_HOT);
            ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
            recovery_add_closed(ftl, g, FTL_TEMP_HOT);
            return true;

        case FTL_BLOCK_STATE_FREE:
        case FTL_BLOCK_STATE_NONE:
            /* 池块在 CP 后被写入：立即转 Closed，valid_count 从 0 起，
             * 后续 delta 由 journal replay 累加。 */
            ftl_block_set_state(b, FTL_BLOCK_STATE_CLOSED);
            ftl_block_clear_valid(b);
            ftl_block_set_temp(b, FTL_TEMP_HOT);
            ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
            recovery_add_closed(ftl, g, FTL_TEMP_HOT);
            return true;

        default:
            return false;
    }
}

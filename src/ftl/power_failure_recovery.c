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
 *   BOOT_FRESH_START    无有效 Checkpoint（出厂首次启动）→ cache 全 INVALID + F2 重建
 *   BOOT_CLEAN_START    journal_seq ≤ cp.journal_sequence（干净关机）→ cache 全 INVALID + F2 重建
 *   BOOT_POWER_FAILURE  journal_seq > cp.journal_sequence（异常掉电）
 *                       → cache 全 INVALID + journal_replay + F4 bitmap 局部加载 + F2 重建
 *   ★ 三条路径**都**走 F2 重建——closed_sb_list 只在 RAM，掉电必丢。
 *
 * 文件结构：
 *   1. 启动恢复总入口   ftl_boot_recovery
 *   2. 三条启动路径     ftl_boot_fresh_start / ftl_boot_clean_start
 *                       / ftl_boot_apply_region_bitmap（static）/ ftl_boot_power_failure
 *   3. F2 全盘重建      page_is_blank / scan_block_write_pointer / close_full_block（static）
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
    l2p_checkpoint_header_t cp;

    /* 前置条件（判据 2）：唯一调用者 main.c 传 &g_ftl[ch][ce][lun]，结构上恒非空。 */
    ASSERT(ftl != NULL);

    return (checkpoint_find_latest(ftl, &cp) != FTL_OK);
}

/* --------------------------------------------------------------------
 * ftl_media_format
 *   功能：全量格式化——擦本实例全部块，并把易失元数据状态复位到"空片"。
 *   参数：ftl —— FTL 实例。
 *   返回：FTL_OK / FTL_ERROR。
 *   实现要点：① bbt_erase_all_blocks 擦全片（出厂坏块标记已在 bbt_init 读进 BST——
 *                **空片时**才读；用过的介质上 bbt_init 拒绝重扫，见 bbt_medium_has_ftl_data）；
 *             ② bbt_save 重新持久化坏块图（BBT 池刚被擦掉）；
 *             ③ metadata_dir_init 复位 Directory 为哨兵层；④ checkpoint_write_initial
 *                写一条当前版本标记 ⇒ 下次开机版本判据命中，格式化只发生一次。
 *   ⚠ 必须在 ftl_init 之后、free pool 回填之前调用（见 main.c ssd_ftl_boot）。
 *   ⚠ BST 的 erase_count 保留旧值（wear_init 已在格式化前加载）——仅影响磨损均衡起点。
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

    metadata_dir_init(&ftl->metadata_dir);

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
    l2p_checkpoint_header_t cp;
    journal_page_ref_t      ring;
    ftl_boot_result_t       local_result;

    /* 前置条件（判据 2）：唯一调用者 main.c 传 &g_ftl[ch][ce][lun] 与栈上 &result。 */
    ASSERT(ftl != NULL);
    ASSERT(result != NULL);

    memset(&local_result, 0, sizeof(local_result));

    /* ① 扫 Journal 环形块池取最新页（只读）。
     *    判型要用它；三条路径也都要用它恢复写指针——journal_init 把写指针/序号复位到
     *    (block0, page0)/1，而环形块**并没有被擦**（journal_truncate 只预擦环形下一块），
     *    不接回去就会二次编程 + 序号回退（详见 journal_resume）。 */
    (void)journal_scan_latest(&ftl->journal, &ring);

    /* ② 扫描 Checkpoint */
    if (checkpoint_find_latest(ftl, &cp) != FTL_OK)
    {
        /* 无有效 Checkpoint → 出厂首次启动 */
        int ret = ftl_boot_fresh_start(ftl, &ring);
        if (ret == FTL_OK)
        {
            local_result.boot_type       = BOOT_FRESH_START;
            local_result.cp_sequence     = 0U;
            local_result.journal_entries = 0U;
            *result                      = local_result;
        }
        return ret;
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
        int ret = ftl_boot_clean_start(ftl, cp.journal_sequence, &ring);
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
        int ret = ftl_boot_power_failure(ftl, &cp, &local_result);
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
 * ftl_boot_fresh_start
 *   功能：出厂首次启动（无 Checkpoint）——cache 全 INVALID + F2 重建，sequence 归零。
 *   参数：ftl —— FTL 实例（前置条件，由 ftl_boot_recovery 保证，不在此复检）；
 *         ring —— Journal 环形块池快照。
 *   返回：FTL_OK / FTL_ERROR。
 *   实现要点：① l2p_cache_init 置全 INVALID（**不清 md_dir/alloc**，见 l2p_cache.c）。
 *             ② F2 重建把「用户数据 written 块」逐出 Free Pool（见下 ⚠）。
 *             ③ allocator 已由 ftl_init 初始化，此处不重复。
 *             ④ journal_resume 按环内最新页接回写指针（序号下限 1）。
 *             ⑤ checkpoint sequence 归零。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_boot_fresh_start(struct ftl_context *ftl, const journal_page_ref_t *ring)
{
    /* ① L2P Cache 全 INVALID */
    l2p_cache_init(&ftl->cache, ftl->topo.ch, ftl->topo.ce, ftl->topo.lun);

    /* ② 全盘重建 Closed SB 列表（F2）。
     *    main.c free pool 回填把「用户数据 written 块」也放进了 Free Pool，其注释
     *    明确依赖「boot_recovery 的 F2 重建会把它们拉出」。clean start 走了 F2，
     *    fresh start 以前没走 ⇒ 那条不变量在此路径上不成立：Free Pool 里躺着已编程
     *    的块，首次分配就把新数据 **AND** 到旧数据上（NAND 只能 1→0）。
     *    fresh start 本就丢了映射，这里只要把 written 块逐出 Free Pool 交给 GC 擦除。
     *    ⚠ 代价：逐块读首页判空（写满块最多 64 页），与 clean start 同一代价。 */
    {
        ftl_rebuild_stats_t rebuild_stats;
        (void)ftl_rebuild_closed_sb_list(ftl, &rebuild_stats);
    }

    /* ③ Allocator 已由 ftl_init 初始化，此处不需重复做 */

    /* ④ Journal RAM 写态恢复：无 Checkpoint ⇒ 序号下限取 1（0 保留作「无序号」）。
     *    环形块里的旧页仍在（刚格式化过的介质为空，但「找不到 Checkpoint」也可能是
     *    Checkpoint 页损坏而环形块完好）⇒ 写指针必须按 NAND 实际内容接回，否则会对
     *    已编程页二次编程；序号从 1 重来还会让下次开机把旧页当新页回放、复活旧映射。 */
    journal_resume(&ftl->journal, ring, 1U);

    /* ⑤ 复位 Checkpoint 定时基准（本实例 cp 游标已由 checkpoint_find_latest 置空） */
    ftl->cp_timer_start = 0U;

    return FTL_OK;
}

/* --------------------------------------------------------------------
 * ftl_boot_clean_start
 *   功能：干净关机恢复——cache 全 INVALID + Journal 写态恢复 + F2 重建，
 *         sequence 取 Checkpoint 值。
 *   参数：ftl —— FTL 实例；cp_journal_sequence —— Checkpoint 的 Journal 截断水位
 *         （下一个待分配序号）；ring —— Journal 环形块池快照（journal_scan_latest 的结果）。
 *   返回：FTL_OK / FTL_ERROR。
 *   实现要点：与 fresh_start 的区别是 Journal 序号下限取 cp_journal_sequence
 *             （本实例 cp 游标/序号已由 checkpoint_find_latest 恢复，故无 cp_sequence 形参）。**「Journal 已由 ftl_init 初始化（空）」是个假
 *             前提**——journal_truncate 只预擦环形下一块，其余块的旧页仍在 NAND 上，
 *             故本路径必须 journal_resume（见其注释；这是「写→掉电→重启丢数据」的根因）。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_boot_clean_start(struct ftl_context       *ftl,
                                  uint32_t                  cp_journal_sequence,
                                  const journal_page_ref_t *ring)
{
    /* ① Cache 全 INVALID（首次 miss 时按需从 NAND L2P Table 加载） */
    l2p_cache_init(&ftl->cache, ftl->topo.ch, ftl->topo.ce, ftl->topo.lun);

    /* ② Journal RAM 写态恢复（**本路径的关键**）。
     *    干净关机下 Checkpoint 已覆盖全部条目（不需要回放），但 **NAND 上的环形块
     *    并没有被擦**——「Journal 是空的」只对刚格式化过的介质成立。必须按环内最新页
     *    把写指针/序号接回 cp_journal_sequence，否则：
     *      ① 序号从 1 重来 ⇒ 新条目 page_seq 排到环内旧页**之后**，下次开机回放的
     *         page_seq 过滤把它们当「已进 L2P 表」整页丢弃 ⇒ 数据丢失；
     *      ② 写指针回到 block0/page0 ⇒ 对已编程页二次编程（NAND 只能 1→0 = AND 损坏）。
     *    详见 journal_resume。 */
    journal_resume(&ftl->journal, ring, cp_journal_sequence);

    /* ③ 掉电重建 Closed SB 列表（F2）：clean start 场景掉电前已满的 SB 同样
     *    只在 RAM closed_sb_list 里，重启后必须全扫 NAND 重建。cache 刚 INIT，
     *    L2P 反查会按需从 NAND region 加载（正确但慢，写进文档）。 */
    {
        ftl_rebuild_stats_t rebuild_stats;
        (void)ftl_rebuild_closed_sb_list(ftl, &rebuild_stats);
    }

    /* ④ 写指针恢复：干净关机时 next_page 由 allocator 在内存中跟踪 */
    /* 当前 allocator 的 open SB 在 init 时未初始化，等首次 alloc 分配新 SB */
    /*  clean_start 场景不需要扫描 NAND */

    /* ⑤ Controller → Ready */
    ftl->cp_timer_start = 0U;

    return FTL_OK;
}

/* --------------------------------------------------------------------
 * ftl_boot_apply_region_bitmap
 *   功能：按 checkpoint region_bitmap 局部加载 L2P cache（F4）。
 *   参数：ftl —— FTL 实例；cp —— 最近 Checkpoint 头（含 region_bitmap）。
 *   返回：无。
 *   实现要点：对 cache 行做三条约束——① DIRTY 行跳过；② bitmap 置位区预载 CLEAN；
 *             ③ 未置位区置 INVALID（懒加载）。必须在 journal_replay 之后调用。
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
 *   ③ bitmap 未置位区：置 INVALID，下一访问懒加载 CP 时刻快照。
 *      该区未在本次 CP 落盘，NAND 页为更旧值/从未写，懒加载即正确。
 *
 * 必须在 journal_replay 之后调用（replay 前 cache 刚 init，所有 line
 * region_id=0，先强制会误匹配全部行）。
 * -------------------------------------------------------------------- */
static void ftl_boot_apply_region_bitmap(ftl_context_t                 *ftl,
                                         const l2p_checkpoint_header_t *cp)
{
    uint16_t i;

    for (i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        l2p_cache_line_t *line = &ftl->cache.lines[i];
        uint32_t          word;
        uint32_t          mask;
        int               in_bitmap;

        if (line->state == L2P_CACHE_DIRTY)
        {
            continue; /* ① 回放数据，保持 DIRTY 待后台 flush */
        }

        word      = cp->region_bitmap[line->region_id / 32U];
        mask      = (1UL << (line->region_id & 31U));
        in_bitmap = ((word & mask) != 0U);

        if (in_bitmap)
        {
            /* ② 预载 bitmap 区（仅 CLEAN/INVALID 行；DIRTY 已在上方跳过） */
            if (line->state != L2P_CACHE_CLEAN)
            {
                nand_pba_t region_addr = NAND_PBA_INVALID;

                if (ftl->cache.md_dir != NULL)
                {
                    region_addr = metadata_dir_lookup(ftl->cache.md_dir,
                                                      METADATA_TYPE_L2P_REGION,
                                                      line->region_id);
                }

                if (region_addr == NAND_PBA_INVALID)
                {
                    region_addr = l2p_region_pba(ftl->cache.ch, ftl->cache.ce, ftl->cache.lun, line->region_id);
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
 *             写指针扫描（原实现被 valid!=0 门控恒假）。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_boot_power_failure(struct ftl_context            *ftl,
                                    const l2p_checkpoint_header_t *cp,
                                    ftl_boot_result_t             *result)
{
    uint32_t entry_count = 0U;
    uint32_t cp_sequence;

    cp_sequence = cp->journal_sequence;

    /* ① Cache 全 INVALID */
    l2p_cache_init(&ftl->cache, ftl->topo.ch, ftl->topo.ce, ftl->topo.lun);

    /* ② Journal 回放（sequence > cp_sequence 的条目 → L2P Cache）。
     *    F10：journal_replay 按 (page_seq, entry_index) 递增序回放所有
     *    page_seq + entry_count > cp_sequence 的页——只回放最大一页会静默丢 ≥2 页条目。 */
    {
        /* ⚠ 回放条数由 journal_replay 在**应用循环**里累加（读失败的页不计）。
         * 旧实现用 journal 的 RAM 写指针反推（`current_entry > 0` → 算 current_page *
         * ENTRIES_PER_PAGE + current_entry）——恒假：journal_replay 末尾必调
         * journal_resume，后者把落点放到最新页之后并 `current_entry = 0`。
         * 于是本字段三条启动路径全是 0，`boot type=N journal_entries=0` 不具判别力。 */
        (void)journal_replay(&ftl->journal, &ftl->cache, cp_sequence, &entry_count);

        /*
         * journal_replay 写入了 L2P Cache，所有 line 标记 DIRTY。
         * 这些 DIRTY entries 会在 ftl_service IDLE 模式中由 background flush
         * 逐步写回 NAND L2P Table。
         */
    }

    /* ③ 按 Checkpoint region_bitmap 局部加载（F4）：跳过 DIRTY 回放行，
     *    bitmap 区预载 CLEAN，非 bitmap 区 INVALID（懒加载 CP 时刻快照） */
    ftl_boot_apply_region_bitmap(ftl, cp);

    /* ④ Allocator 重建（F2：掉电重启后全扫 NAND 重建 Closed SB 列表）。
     *    journal_replay 已把未回放 entry 写入 cache → L2P 反查零 region 加载。
     *    顺带替代原 hot_sb/cold_sb 写指针扫描（原实现被 valid!=0 门控恒假）。 */
    {
        ftl_rebuild_stats_t rebuild_stats;
        (void)ftl_rebuild_closed_sb_list(ftl, &rebuild_stats);
    }

    /* ⑤ Controller → Ready */
    ftl->cp_timer_start = 0U;

    /* ⑥ 输出结果 */
    result->boot_type       = BOOT_POWER_FAILURE;
    result->cp_sequence     = cp_sequence;
    result->journal_entries = entry_count;

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
 *   `scan_block_write_pointer` 会对该块返回 0 ⇒ 块被当成"没写过"**留在 Free Pool**
 *   ⇒ 既可能被 allocator 拿去复用覆盖活数据，又让 L2P 指向一个 valid_count 恒 0 的块
 *   （`bst.h` 断言的成因，与 v0.69 修的 plane 掩码同一类）。
 *   判「非空」的代价：真为空的块会被拉出 Free Pool 交 GC 多擦一次——不丢数据。 */
static bool page_is_blank(nand_pba_t pba, uint8_t *raw_buf)
{
    uint8_t i;
    uint8_t j;

    memset(raw_buf, 0xFF, PAGE_SIZE);
    if (ftl_sync_read_page_raw(pba, raw_buf) != FTL_OK)
    {
        return false; /* 命令没走通 ⇒ 内容不可知 ⇒ 一律判非空 */
    }

    /* 每 CW 的 META(4B)+CRC(4B)+前 8B 数据，共 16B。全 FF 页 META/CRC 也全 FF。 */
    for (i = 0U; i < CW_NUM; i++)
    {
        const uint8_t *p = raw_buf + (uint32_t)i * FC_BUFFER_POOL_CW_SIZE;
        for (j = 0U; j < 16U; j++)
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
static uint8_t scan_block_write_pointer(const ftl_context_t *ftl, uint8_t plane, uint16_t block, uint8_t *raw_buf)
{
    uint8_t page;

    for (page = 0U; page < PAGES_PER_BLOCK; page++)
    {
        nand_pba_t addr = pba_make(ftl->topo.ch, ftl->topo.ce, ftl->topo.lun, block, plane, page, 0U);

        if (page_is_blank(addr, raw_buf))
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
static void close_full_block(ftl_context_t       *ftl,
                             uint8_t              plane,
                             uint16_t             block,
                             uint8_t              temperature,
                             ftl_rebuild_stats_t *stats)
{
    ftl_allocator_t *alloc = &ftl->alloc;
    uint16_t         sb_idx;
    uint16_t         page;
    uint16_t         plane_blocks[PLANE_NUM];
    ftl_block_t     *b;

    /* 退化单 plane 条目：本 plane 记块号，另一 plane 记哨兵 */
    plane_blocks[0]     = FTL_BLOCK_NO_PLANE;
    plane_blocks[1]     = FTL_BLOCK_NO_PLANE;
    plane_blocks[plane] = block;

    sb_idx = allocator_add_closed_sb(alloc, plane_blocks, temperature);

    if (stats != NULL)
    {
        stats->blocks_rebuilt++;
    }

    /* 逐页：LDPC_RAW 判空 + gc_read_page_with_lba 取 LBA + **逐 CW** l2p 反查 */
    b = &alloc->blocks[ftl_gblock(plane, block)];
    for (page = 0U; page < PAGES_PER_BLOCK; page++)
    {
        nand_pba_t pba;
        lba_t      cw_lba[CW_NUM];
        uint8_t    cw;

        pba = super_block_page_pba(ftl->topo, allocator_closed_at(alloc, sb_idx), plane, page);

        /* 读失败 / 全页无写过 CW → 该页不可用，跳过。
         * CW 级映射：逐 CW 从页内 META（= OOB 反向映射）取回扇区 LBA。 */
        if (gc_read_page_with_lba(pba, NULL, cw_lba, NULL) != FTL_OK)
        {
            continue;
        }

        for (cw = 0U; cw < CW_NUM; cw++)
        {
            /* ⚠ F2 重建 = 结构性同步路径（开机、superloop 未建立）：用同步逃生口。
             * 读得 INVALID 与"未映射"同义 ⇒ 该 CW 不计入有效数（与旧实现同）。 */
            nand_pba_t cur = NAND_PBA_INVALID;

            (void)l2p_cache_read_sync(&ftl->cache, cw_lba[cw], &cur);

            if (cw_lba[cw] == FTL_LBA_INVALID)
            {
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
    uint8_t          raw_buf[PAGE_SIZE];
    ftl_allocator_t *alloc;
    uint8_t          plane;
    uint16_t         block;

    alloc = &ftl->alloc;

    if (stats != NULL)
    {
        memset(stats, 0, sizeof(*stats));
    }

    /* Policy B：**逐 plane 独立扫描**——两 plane 的块不再同号。配对只在写入时由
     * allocator 创建，之后从不被查找（multi-plane 读的两个 PBA 各自从 L2P 查；
     * GC/WL 逐 plane 工作），故重建只需把"写过的物理块各自归位"，无需、也无法
     * 恢复"哪两块曾是一对"（配对取决于 GC 历史，不在任何持久化位图里）。 */
    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        for (block = 0U; block < BLOCKS_PER_PLANE; block++)
        {
            uint16_t     g = ftl_gblock(plane, block);
            ftl_block_t *b = &alloc->blocks[g];
            uint8_t      np;

            /* ① 坏块 / 元数据块跳过（判据与 main.c free pool 回填一致，per-plane 块号） */
            if (ftl_block_is_bad(b) || ftl_is_metadata_block(&ftl->metadata_dir, plane, block))
            {
                continue;
            }

            /* ② 判空找写指针（第一个全 FF 页） */
            np = scan_block_write_pointer(ftl, plane, block, raw_buf);

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
            close_full_block(ftl, plane, block, FTL_TEMP_HOT, stats);

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

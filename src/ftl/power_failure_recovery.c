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
 *   1. 启动恢复总入口   journal_page_addr（static）/ ftl_boot_recovery
 *   2. 三条启动路径     ftl_boot_fresh_start / ftl_boot_clean_start
 *                       / ftl_boot_apply_region_bitmap（static）/ ftl_boot_power_failure
 *   3. 写指针扫描       ftl_boot_scan_write_pointer（旧路径，单 Super Block 版）
 *   4. F2 全盘重建      page_is_blank / scan_block_write_pointer / close_full_block（static）
 *                       / ftl_rebuild_closed_sb_list
 *
 * ⚠ 本文件依赖 ftl_init 已完成 allocator / journal / metadata_dir 的初始化；
 *   各路径步骤用圈码 ①②③ 编号（与 docs/design/ftl_design.md §8 对应）。
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

/* ── Journal Block 页地址辅助（双 Block 双 Plane 交错）── */

/* journal_page_addr — Journal 页地址构造：块 = JOURNAL_BLOCK0 + b（双块交错），
 * plane = page & 1（双 plane 交错），cw = 0（元数据页只占 CW0）。 */
static nand_addr_t journal_page_addr(const ftl_context_t *ftl, uint8_t b, uint8_t page)
{
    nand_addr_t addr;

    addr.ch    = ftl->topo.ch;
    addr.ce    = ftl->topo.ce;
    addr.lun   = ftl->topo.lun;
    addr.block = (uint16_t)(JOURNAL_BLOCK0 + b);
    addr.plane = (uint8_t)(page & 1U);
    addr.page  = page;
    addr.cw    = 0U;

    return addr;
}

/* ══════════════════════════════════════════════════════════════════
 * 介质布局版本 —— 开机自动格式化
 * ══════════════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * ftl_media_needs_format
 *   功能：判定介质是否需要（重新）格式化。
 *   参数：ftl —— FTL 实例。
 *   返回：1 = 需要格式化；0 = 介质已是当前布局版本（正常启动）。
 *   实现要点：判据 = 「本实例 checkpoint_find_latest 失败」。因版本校验已在
 *             checkpoint_header_valid 内，故该判据自动涵盖三种情况：
 *             ① 有当前版本 Checkpoint ⇒ 成功 ⇒ 不格式化；
 *             ② 有 Checkpoint 但版本不符（旧格式介质）⇒ 失败 ⇒ 格式化；
 *             ③ 完全没有 Checkpoint（出厂空片）⇒ 失败 ⇒ 格式化（多擦一次，写标记后自愈）。
 * -------------------------------------------------------------------- */
int ftl_media_needs_format(const struct ftl_context *ftl)
{
    l2p_checkpoint_header_t cp;

    if (ftl == NULL)
    {
        return 0;
    }

    return (checkpoint_find_latest(&cp, ftl->topo) != FTL_OK) ? 1 : 0;
}

/* --------------------------------------------------------------------
 * ftl_media_format
 *   功能：全量格式化——擦本实例全部块，并把易失元数据状态复位到"空片"。
 *   参数：ftl —— FTL 实例。
 *   返回：FTL_OK / FTL_ERROR。
 *   实现要点：① bbt_erase_all_blocks 擦全片（出厂坏块标记已在 bbt_init 先读进 BST，
 *                擦除不丢坏块信息）；② bbt_save 重新持久化坏块图（BBT 池刚被擦掉）；
 *             ③ metadata_dir_init 复位 Directory 为哨兵层；④ checkpoint_write_initial
 *                写一条当前版本标记 ⇒ 下次开机版本判据命中，格式化只发生一次。
 *   ⚠ 必须在 ftl_init 之后、free pool 回填之前调用（见 main.c ssd_ftl_boot）。
 *   ⚠ BST 的 erase_count 保留旧值（wear_init 已在格式化前加载）——仅影响磨损均衡起点。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_media_format(struct ftl_context *ftl)
{
    if (ftl == NULL)
    {
        return FTL_ERROR;
    }

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
 *   参数：ftl —— FTL 实例（非 NULL 校验）；result —— 出参启动结果
 *         （boot_type / cp_sequence / journal_entries）。
 *   返回：FTL_OK 成功；FTL_ERROR 参数非法或子路径失败。
 *   实现要点：① checkpoint_find_latest 取最近快照，找不到 = 首次启动走 fresh_start。
 *             ② 扫 Journal 各页 header 取最大 page_seq——**只读不改状态**，故不调
 *                journal_replay（后者会修改 journal 内部状态）。
 *             ③ page_seq ≤ cp.journal_sequence ⇒ 干净关机；否则异常掉电。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_boot_recovery(struct ftl_context *ftl, ftl_boot_result_t *result)
{
    l2p_checkpoint_header_t cp;
    ftl_boot_result_t       local_result;

    if ((ftl == NULL) || (result == NULL))
    {
        return FTL_ERROR;
    }

    memset(&local_result, 0, sizeof(local_result));

    /* ① 扫描 Checkpoint */
    if (checkpoint_find_latest(&cp, ftl->topo) != FTL_OK)
    {
        /* 无有效 Checkpoint → 出厂首次启动 */
        int ret = ftl_boot_fresh_start(ftl);
        if (ret == FTL_OK)
        {
            local_result.boot_type       = BOOT_FRESH_START;
            local_result.cp_sequence     = 0U;
            local_result.journal_entries = 0U;
            *result                      = local_result;
        }
        return ret;
    }

    /* ② 扫描 Journal 获取最后 sequence（直接读 header 不修改状态） */
    {
        uint32_t journal_last_seq = 0U;

        /*
         * 从 Journal 最新有效页读最后 sequence。
         * 不调 journal_replay——后者会修改 journal 内部状态。
         * 用临时 scan 读各页的 header 取最大 page_seq。
         */
        {
            uint8_t b, page;

            for (b = 0U; b < JOURNAL_BLOCK_COUNT; b++)
            {
                for (page = 0U; page < PAGES_PER_BLOCK; page++)
                {
                    uint8_t     page_data[JOURNAL_PAGE_SIZE];
                    nand_addr_t pba;
                    int         ret;
                    uint32_t    magic;

                    pba = journal_page_addr(ftl, b, page);

                    ret = ftl_sync_read_page(pba, page_data, sizeof(page_data), FTL_PAGE_CFG_META(FC_COMMAND_MODULE_METADATA));
                    if (ret != 0)
                    {
                        continue;
                    }

                    memcpy(&magic, page_data + 0, sizeof(magic));
                    if (magic != JOURNAL_MAGIC)
                    {
                        continue;
                    }

                    {
                        uint32_t ps;
                        memcpy(&ps, page_data + 4, sizeof(ps));
                        if (ps > journal_last_seq)
                        {
                            journal_last_seq = ps;
                        }
                    }
                }
            }
        }

        if (journal_last_seq <= cp.journal_sequence)
        {
            /* 干净关机 */
            int ret = ftl_boot_clean_start(ftl, cp.sequence);
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
}

/* ── 三条启动路径（fresh / clean / power-failure）── */

/* --------------------------------------------------------------------
 * ftl_boot_fresh_start
 *   功能：出厂首次启动（无 Checkpoint）——cache 全 INVALID + F2 重建，sequence 归零。
 *   参数：ftl —— FTL 实例（非 NULL 校验）。
 *   返回：FTL_OK / FTL_ERROR。
 *   实现要点：① l2p_cache_init 置全 INVALID（**不清 md_dir/alloc**，见 l2p_cache.c）。
 *             ② F2 重建把「用户数据 written 块」逐出 Free Pool（见下 ⚠）。
 *             ③④⑤ allocator / journal 已由 ftl_init 初始化，此处不重复。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_boot_fresh_start(struct ftl_context *ftl)
{
    if (ftl == NULL)
    {
        return FTL_ERROR;
    }

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

    /* ④ Journal 已由 ftl_init 初始化（空） */
    /* ⑤ checkpoint sequence = 0 */
    ftl->cp_sequence    = 0U;
    ftl->cp_timer_start = 0U;

    return FTL_OK;
}

/* --------------------------------------------------------------------
 * ftl_boot_clean_start
 *   功能：干净关机恢复——cache 全 INVALID + F2 重建，sequence 取 Checkpoint 值。
 *   参数：ftl —— FTL 实例；cp_sequence —— 最近 Checkpoint 的 sequence。
 *   返回：FTL_OK / FTL_ERROR。
 *   实现要点：与 fresh_start 唯一区别是 cp_sequence 取自 CP 而非 0；写指针不需扫
 *             NAND（干净关机下 allocator 内存态可信）。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_boot_clean_start(struct ftl_context *ftl, uint32_t cp_sequence)
{
    if (ftl == NULL)
    {
        return FTL_ERROR;
    }

    /* ① Cache 全 INVALID（首次 miss 时按需从 NAND L2P Table 加载） */
    l2p_cache_init(&ftl->cache, ftl->topo.ch, ftl->topo.ce, ftl->topo.lun);

    /* ② 掉电重建 Closed SB 列表（F2）：clean start 场景掉电前已满的 SB 同样
     *    只在 RAM closed_sb_list 里，重启后必须全扫 NAND 重建。cache 刚 INIT，
     *    L2P 反查会按需从 NAND region 加载（正确但慢，写进文档）。 */
    {
        ftl_rebuild_stats_t rebuild_stats;
        (void)ftl_rebuild_closed_sb_list(ftl, &rebuild_stats);
    }

    /* ③ 写指针恢复：干净关机时 next_page 由 allocator 在内存中跟踪 */
    /* 当前 allocator 的 open SB 在 init 时未初始化，等首次 alloc 分配新 SB */
    /*  clean start 场景不需要扫描 NAND */

    /* ④ Controller → Ready */
    ftl->cp_sequence    = cp_sequence;
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
 *      entries 仍是 init 时全 FTL_PBA_INVALID，read 会对有效映射返回假 INVALID。
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

    if ((ftl == NULL) || (cp == NULL))
    {
        return;
    }

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
                nand_paa_t  region_paa = NAND_PBA_INVALID;
                nand_addr_t region_addr;

                if (ftl->cache.md_dir != NULL)
                {
                    region_paa = metadata_dir_lookup(ftl->cache.md_dir,
                                                     METADATA_TYPE_L2P_REGION,
                                                     line->region_id);
                }

                if (region_paa != NAND_PBA_INVALID)
                {
                    region_addr = addr_from_paa(ftl->cache.ch, ftl->cache.ce, region_paa);
                }
                else
                {
                    region_addr = addr_from_pba(l2p_region_pba(ftl->cache.ch,
                                                               ftl->cache.ce,
                                                               ftl->cache.lun,
                                                               line->region_id));
                }

                if (ftl_sync_read_page(region_addr, line->entries, L2P_REGION_SIZE, FTL_PAGE_CFG_META(FC_COMMAND_MODULE_METADATA)) == FTL_OK)
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
 *   实现要点：② journal_replay 回放所有 page_seq > cp_sequence 的页（F10：只回放
 *             最大一页会静默丢 ≥2 页条目）；④ F2 重建顺带替代旧 hot_sb/cold_sb
 *             写指针扫描（原实现被 valid!=0 门控恒假）。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_boot_power_failure(struct ftl_context            *ftl,
                                    const l2p_checkpoint_header_t *cp,
                                    ftl_boot_result_t             *result)
{
    uint32_t entry_count;
    uint32_t cp_sequence;

    if ((ftl == NULL) || (result == NULL) || (cp == NULL))
    {
        return FTL_ERROR;
    }

    cp_sequence = cp->journal_sequence;

    /* ① Cache 全 INVALID */
    l2p_cache_init(&ftl->cache, ftl->topo.ch, ftl->topo.ce, ftl->topo.lun);

    /* ② Journal 回放（sequence > cp_sequence 的条目 → L2P Cache）。
     *    F10：journal_replay 按 (page_seq, entry_index) 递增序回放所有
     *    page_seq > cp_sequence 的页——只回放最大一页会静默丢 ≥2 页条目。 */
    {
        (void)journal_replay(&ftl->journal, &ftl->cache, cp_sequence);

        /*
         * 估算回放的 entry 数：
         *   replay 后的 current_entry/current_page 减去初始值
         *   简化：如果 current_entry > 0 说明有数据回放
         */
        if (ftl->journal.current_entry > 0U)
        {
            entry_count = (uint32_t)ftl->journal.current_page *
                              JOURNAL_ENTRIES_PER_PAGE +
                          ftl->journal.current_entry;
        }
        else
        {
            entry_count = 0U;
        }

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
    ftl->cp_sequence    = cp_sequence;
    ftl->cp_timer_start = 0U;

    /* ⑥ 输出结果 */
    result->boot_type       = BOOT_POWER_FAILURE;
    result->cp_sequence     = cp_sequence;
    result->journal_entries = entry_count;

    return FTL_OK;
}

/* ── 写指针扫描（旧路径，单 Super Block 版）── */

/* --------------------------------------------------------------------
 * ftl_boot_scan_write_pointer
 *   功能：扫 NAND 找某 Super Block 的写指针（第一个全 0xFF 空白页），并重建 BST 有效页位图。
 *   参数：alloc —— 分配器；ch/ce —— 目标通道/片选；sb —— 目标 Super Block（valid==0 直接返回）。
 *   返回：无（结果写回 sb->next_page）。
 *   实现要点：逐 plane 逐页用 LDPC_RAW 读（跳过 ECC）；全 0xFF = 空页 = 写指针位置。
 *             ⚠ 旧路径（单 Super Block 版），新路径见下方 F2 的 scan_block_write_pointer。
 * -------------------------------------------------------------------- */
void ftl_boot_scan_write_pointer(ftl_allocator_t *alloc,
                                 uint8_t          ch,
                                 uint8_t          ce,
                                 super_block_t   *sb)
{
    uint8_t page;
    uint8_t plane;

    if ((alloc == NULL) || (sb == NULL) || (sb->valid == 0U))
    {
        return;
    }

    /*
     * 扫描 NAND 找第一个全 0xFF 页（= 未写入的空白页）。
     * 从 Page 0 开始逐 Plane 扫描，每种 mode 尝试一次读。
     *
     * 注意：使用标准 BCH ECC 读，如果页已写但 ECC 失败，
     * 返回 -1 表示该页非空白。全 0xFF 的未写页在 LDPC_RAW 下
     * 读出的数据也全 0xFF。
     *
     * 同时重建 BST 有效页位图：扫描到的非空页置位（掉电后
     * bitmap/VPC 是 RAM 态，冷启动为零，必须从这里重建）。
     */
    for (page = 0U; page < PAGES_PER_BLOCK; page++)
    {
        for (plane = 0U; plane < PLANE_NUM; plane++)
        {
            uint8_t     buf[16]; /* 读 16 字节就够了 */
            nand_addr_t pba;
            int         ret;

            pba = addr_from_paa(ch, ce, paa_from_addr(&(nand_addr_t){.block = sb->plane_blocks[plane], .plane = plane, .page = page}));

            memset(buf, 0xFF, sizeof(buf));

            /*
             * 用 LDPC_RAW 模式读（跳过 ECC，获得原始数据）。
             * 如果完全全 0xFF → 空页。
             */
            ret = ftl_sync_read_page_raw(pba, buf, FC_COMMAND_MODULE_BBT);

            /*
             * 即使 LDPC_RAW 返回失败，也检查数据是否全 FF。
             * 全 FF + 读失败 → 出厂坏块或空块
             * 全 FF + 读成功 → 空页
             */
            {
                uint8_t i;
                uint8_t all_ff = 1U;

                for (i = 0U; i < sizeof(buf); i++)
                {
                    if (buf[i] != 0xFFU)
                    {
                        all_ff = 0U;
                        break;
                    }
                }

                /*
                 * 找到第一个全 0xFF 页 → 写指针位置。
                 * 只检查 Plane 0（双 Plane 同步写入）。
                 */
                if (all_ff)
                {
                    sb->next_page = page;
                    return;
                }
            }

            /* 非空页：写指针之前的数据页 ⇒ 置 OPEN 状态。
             * ⚠ 本路径只做裸 NAND 扫描、不查 L2P，故**不**维护 valid_count（CW 级真值
             *   由 F2 路径 close_full_block 的逐 CW L2P 反查回填；v0.60 起无常驻位图）。 */
            if (alloc->blocks != NULL)
            {
                uint16_t     gblock = ftl_gblock(plane, sb->plane_blocks[plane]);
                ftl_block_t *b      = &alloc->blocks[gblock];

                if (!ftl_block_is_bad(b))
                {
                    ftl_block_state_set(b, FTL_BLOCK_STATE_OPEN);
                    ftl_block_state_clr(b, FTL_BLOCK_STATE_FREE | FTL_BLOCK_STATE_CLOSED);
                }
            }

            (void)ret; /* ret 不重要，有数据就继续扫 */
        }
    }

    /*
     * 全 Block 都没找到空页 → Block 已写满。
     * next_page 保持 PAGES_PER_BLOCK（=64，已满）。
     * 调用者会在 allocator_alloc 中检测到写满并分配新 SB。
     */
    sb->next_page = PAGES_PER_BLOCK;
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

/* 判一页是否为全 0xFF 空白页。buffer 复用单个大缓冲，避免 16B 栈溢出（既有
 * ftl_boot_scan_write_pointer 的 buf[16] 传给按 2048B 拷贝的 ftl_sync_read_page_raw）。 */
static uint8_t page_is_blank(const nand_addr_t *addr, uint8_t *raw_buf)
{
    uint8_t i;
    uint8_t j;

    memset(raw_buf, 0xFF, PAGE_SIZE);
    (void)ftl_sync_read_page_raw(*addr, raw_buf, FC_COMMAND_MODULE_BBT); /* 读失败也不影响：数据非全 FF 即判非空 */

    /* 每 CW 的 META(4B)+CRC(4B)+前 8B 数据，共 16B。全 FF 页 META/CRC 也全 FF。 */
    for (i = 0U; i < CW_NUM; i++)
    {
        const uint8_t *p = raw_buf + (uint32_t)i * FC_BUFFER_POOL_CW_SIZE;
        for (j = 0U; j < 16U; j++)
        {
            if (p[j] != 0xFFU)
            {
                return 0U;
            }
        }
    }

    return 1U;
}

/* 扫出某 Block 写指针：第一个全 FF 页；全非空返回 PAGES_PER_BLOCK */
static uint8_t scan_block_write_pointer(const ftl_context_t *ftl, uint8_t plane, uint16_t block, uint8_t *raw_buf)
{
    uint8_t page;

    for (page = 0U; page < PAGES_PER_BLOCK; page++)
    {
        nand_addr_t addr;
        addr.ch    = ftl->topo.ch;
        addr.ce    = ftl->topo.ce;
        addr.lun   = ftl->topo.lun;
        addr.block = block;
        addr.plane = plane;
        addr.page  = page;
        addr.cw    = 0U;

        if (page_is_blank(&addr, raw_buf))
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
    /* allocator_add_closed_sb 内部已断言列表未满，恒返回有效索引 */
    ASSERT(sb_idx != UINT16_MAX);

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

        pba = super_block_page_pba(alloc->ch, alloc->ce, &alloc->closed_sb_list[sb_idx], plane, page);

        /* 读失败 / 全页无写过 CW → 该页不可用，跳过。
         * CW 级映射：逐 CW 从页内 META（= OOB 反向映射）取回扇区 LBA。 */
        if (gc_read_page_with_lba(pba, NULL, cw_lba, NULL, NULL, NULL) != FTL_OK)
        {
            continue;
        }

        for (cw = 0U; cw < CW_NUM; cw++)
        {
            const nand_pba_t cur = l2p_cache_read(&ftl->cache, cw_lba[cw]);

            if (cw_lba[cw] == FTL_LBA_INVALID)
            {
                continue;
            }
            if ((cur != FTL_PBA_INVALID) && (cur == pba_with_cw(pba, cw)))
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

    if (ftl == NULL)
    {
        return FTL_ERROR;
    }

    alloc = &ftl->alloc;
    if (alloc->blocks == NULL)
    {
        return FTL_ERROR;
    }

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
            if (ftl_block_is_bad(b) || ftl_is_metadata_block(&ftl->metadata_dir, block))
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

    return FTL_OK;
}

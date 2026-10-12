/*
 * wear_leveling.c — 静态磨损均衡（触发判定 + 选 victim + 整 SB 搬移）
 *
 * 职责：
 *   在块擦除次数分布不均时，把「擦得少」的冷 SB 的数据搬到「擦得多」的冷块上，
 *   拉平各块磨损；另有一条「慢死预测」旁路：把 ECC 趋势恶化的块整 SB 搬空并退役。
 *
 * 与 gc.c 的分工：
 *   GC —— 回收**无效页多**的块（空间回收，顺带磨损均衡）；
 *   WL —— 搬运**擦得少**的块（磨损拉平，顺带 read disturb 缓解）。两者都用
 *         gc_read_page_with_lba / gc_erase_and_free 等共用原语。
 *
 * 触发三条件（wl_should_trigger，任一命中即搬）：最大/最小差值、最大/平均比值、低擦除区占比。
 *
 * 文件结构：
 *   1. 触发 / 选块     wl_should_trigger / wl_select_victim
 *   2. 搬移            wl_relocate / wl_execute / wl_predictive_relocate
 *   3. 磨损统计        wl_max_erase / wl_min_erase / wl_avg_erase / wl_count_blocks_below
 *
 * ⚠ 全部按**全局块号**索引 BST（跨 plane），退化单 plane 条目跳过哨兵 plane。
 */

#include "ftl/wear_leveling.h"

#include "common/assertion.h"
#include "common/log.h"
#include "fc/fc.h"
#include "ftl/ftl.h"
#include "ftl/ftl_fc_interface.h"
#include "ftl/gc.h"
#include "ftl/read_recovery.h" /* rr_predictive_scan / rr_retire_block */
#include "ftl/wear.h"          /* wear_mark_dirty（计数持久化） */
#include "nand/onfi_1_0.h"

#include <string.h>

/* ── 1. 触发 / 选块 ── */

/* --------------------------------------------------------------------
 * wl_should_trigger
 *   功能：判定是否该做一轮 WL（三条件任一命中即 1）。
 *   参数：ftl —— FTL 实例。
 *   返回：1 触发；FTL_OK(0) 不触发（含 ftl==NULL / 未达最小间隔）。
 *   实现要点：先过 WL_MIN_INTERVAL 最小间隔门（避免刚开机抖动）；条件 1 最大/最小差值
 *             ≥ WL_DELTA_THRESHOLD；条件 2 最大 ≥ 平均 × WL_MAX_AVG_RATIO；
 *             条件 3 低擦除区（min + WL_LOW_ZONE_WIDTH 以下）块占比 > WL_LOW_BLOCK_PCT%。
 * -------------------------------------------------------------------- */
int wl_should_trigger(const struct ftl_context *ftl)
{
    uint32_t max_ec;
    uint32_t min_ec;
    uint32_t avg_ec;
    uint32_t low_zone;
    uint32_t low_count;

    if (ftl == NULL)
    {
        return FTL_OK;
    }

    /* WL 最小间隔检查 */
    if (ftl->total_writes < WL_MIN_INTERVAL)
    {
        return FTL_OK;
    }

    max_ec = wl_max_erase(ftl);
    min_ec = wl_min_erase(ftl);
    avg_ec = wl_avg_erase(ftl);

    /* 条件 1：最大/最小差值 */
    if ((max_ec - min_ec) >= WL_DELTA_THRESHOLD)
    {
        return 1;
    }

    /* 条件 2：最大/平均比值 */
    if ((avg_ec > 0U) && (max_ec >= avg_ec * WL_MAX_AVG_RATIO))
    {
        return 1;
    }

    /* 条件 3：低擦除区 Block 占比 */
    low_zone  = min_ec + WL_LOW_ZONE_WIDTH;
    low_count = wl_count_blocks_below(ftl, low_zone);
    if (low_count > (uint32_t)ftl->alloc.total_blocks * WL_LOW_BLOCK_PCT / 100U)
    {
        return 1;
    }

    return FTL_OK;
}

/* --------------------------------------------------------------------
 * wl_select_victim
 *   功能：从 Closed SB 列表里挑一个搬运候选（擦得少、有效页多、read_count 高者优先）。
 *   参数：ftl —— FTL 实例；victim —— 出参。
 *   返回：1 找到；FTL_OK(0) 未找到 / 参数非法。
 *   实现要点：只考虑擦除次数落在低擦除区（min + WL_LOW_ZONE_WIDTH 内）的 Closed SB；
 *             有效页须 ≥ 最小有效页门（WL_MIN_VALID_RATIO，太碎不值当搬）；
 *             排序键 = 有效页多（越冷越优先）→ 同有效页下 read_count 高（read disturb 风险高优先）。
 * -------------------------------------------------------------------- */
int wl_select_victim(const struct ftl_context *ftl, wl_victim_t *victim)
{
    uint16_t    i;
    uint32_t    min_ec;
    wl_victim_t best;
    uint16_t    total_pages_per_sb;
    uint16_t    min_valid_pages;

    if ((ftl == NULL) || (victim == NULL))
    {
        return FTL_OK;
    }

    memset(&best, 0, sizeof(best));
    min_ec = wl_min_erase(ftl);

    total_pages_per_sb = (uint16_t)(PLANE_NUM * PAGES_PER_BLOCK);
    min_valid_pages    = (uint16_t)(total_pages_per_sb * WL_MIN_VALID_RATIO / 100U);

    for (i = 0U; i < ftl->alloc.closed_sb_count; i++)
    {
        const super_block_t *sb = &ftl->alloc.closed_sb_list[i];
        uint32_t             sb_ec;

        /* 跳过仍为 Open 的 SB */
        if (sb->valid != 0U)
        {
            continue;
        }

        /* 计算该 Super Block 的平均擦除次数（全局 Block 号索引 BST）。
         * 退化单 plane 条目（重建产物）跳过哨兵 plane，按实际 plane 数取平均。 */
        {
            uint32_t ec_sum = 0U;
            uint8_t  p;
            uint8_t  n = 0U;

            for (p = 0U; p < PLANE_NUM; p++)
            {
                if (super_block_has_plane(sb, p) == 0U)
                {
                    continue;
                }
                ec_sum += ftl->alloc.blocks[ftl_gblock(p, sb->plane_blocks[p])].erase_count;
                n++;
            }
            sb_ec = (n > 0U) ? (ec_sum / n) : 0U;
        }

        /* 不在低擦除区 → 跳过（擦除够多了） */
        if (sb_ec > (min_ec + WL_LOW_ZONE_WIDTH))
        {
            continue;
        }

        /*
         * 有效页 = 双 Plane 各物理块 VPC 之和（真实计数）。
         * 有效页越多（越冷）越优先。
         * B5：同一有效页数下，read_count 越高的冷块越优先（read disturb
         * 风险高 → 优先搬移；read_count 来自 host 读 + GC/WL 搬移读累加）。
         */
        {
            uint16_t valid  = 0U;
            uint32_t rc_sum = 0U;
            uint8_t  p;

            for (p = 0U; p < PLANE_NUM; p++)
            {
                const ftl_block_t *blk;

                if (super_block_has_plane(sb, p) == 0U)
                {
                    continue; /* 退化单 plane 条目 */
                }
                blk   = &ftl->alloc.blocks[ftl_gblock(p, sb->plane_blocks[p])];
                valid = (uint16_t)(valid + blk->valid_count);
                rc_sum += blk->read_count;
            }

            if (valid < min_valid_pages)
            {
                continue;
            }

            if ((valid > best.valid_pages) ||
                ((valid == best.valid_pages) && (rc_sum > best.read_count)))
            {
                best.found           = 1U;
                best.avg_erase       = sb_ec;
                best.valid_pages     = valid;
                best.read_count      = rc_sum;
                best.plane_blocks[0] = sb->plane_blocks[0];
                best.plane_blocks[1] = sb->plane_blocks[1];
            }
        }
    }

    if (!best.found)
    {
        return FTL_OK;
    }

    *victim = best;
    return 1;
}

/* ── 2. 搬移 ── */

/* --------------------------------------------------------------------
 * wl_relocate
 *   功能：把 victim SB 的所有有效页搬到 Cold 流新块，更新 L2P/Journal/VPC，最后擦除归还。
 *   参数：ftl —— FTL 实例；victim —— 搬运候选（须 found）。
 *   返回：实际搬移页数（int）；FTL_OK(0) 无搬移 / 参数非法。
 *   实现要点：逐 plane 遍历有效页位图，只搬置位页（免读无效页）；每页先查 L2P 是否仍指向
 *             本 PBA（被覆盖则清位跳过）；搬移读顺带记录 ECC 趋势 + read_count（慢死预测闭环）；
 *             搬完先 journal_flush_page 再擦 victim（与 gc_migrate_sb 同契约，防掉电回放指向已擦块）。
 * -------------------------------------------------------------------- */
int wl_relocate(struct ftl_context *ftl, const wl_victim_t *victim)
{
    uint8_t  plane;
    uint32_t migrated = 0U;

    if ((ftl == NULL) || (victim == NULL) || (!victim->found))
    {
        return FTL_OK;
    }

    /* Step 1: 遍历双 Plane 有效页位图，只搬置位页（免 NAND 读无效页） */
    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        uint16_t gblock_src;
        uint64_t valid_map;

        if (victim->plane_blocks[plane] == FTL_BLOCK_NO_PLANE)
        {
            continue; /* 退化单 plane 条目：该 plane 无块 */
        }
        gblock_src = ftl_gblock(plane, victim->plane_blocks[plane]);
        valid_map  = ftl->alloc.valid_map[gblock_src][plane];

        while (valid_map != 0ULL)
        {
            uint8_t page = (uint8_t)__builtin_ctzll(valid_map);
            valid_map &= (valid_map - 1ULL); /* 清最低置位 bit */

            {
                uint8_t    buf[FTL_READ_USER_SIZE];
                lba_t      lba;
                nand_pba_t pba;
                nand_pba_t old_pba;
                nand_pba_t new_pba;
                int        ret;

                pba = pba_from_addr(&(nand_addr_t){.ch = ftl->topo.ch, .ce = ftl->topo.ce, .block = victim->plane_blocks[plane], .plane = plane, .page = page});

                /* 读页 + 从 META 提取 LBA（复用 GC 辅助函数） */
                memset(buf, 0, sizeof(buf));
                {
                    uint8_t ecc;
                    uint8_t rc;
                    if (gc_read_page_with_lba(pba, buf, &lba, &ecc, &rc) != 0)
                    {
                        continue; /* 读失败，跳过 */
                    }

                    /* B4：WL 搬移读记录 ECC 趋势 + read disturb 计数（慢死预测闭环） */
                    if (rc != 0U)
                    {
                        ftl_block_t *src = &ftl->alloc.blocks[gblock_src];
                        rr_record_ecc(&src->rr, ecc);
                        src->read_count++;
                        wear_mark_dirty(gblock_src);
                    }
                }

                /* 查 L2P：当前映射是否仍指向本 PBA */
                old_pba = l2p_cache_read(&ftl->cache, lba);
                if (old_pba != pba)
                {
                    /* 已被覆盖，无效页 → 清 bitmap 保持同步 */
                    ftl_block_clr_valid(&ftl->alloc.blocks[gblock_src], ftl->alloc.valid_map[gblock_src], plane, page);
                    continue;
                }

                /* 分配 Cold Super Block 新页 */
                new_pba = allocator_alloc(&ftl->alloc, FTL_TEMP_COLD);
                if (new_pba == NAND_PBA_INVALID)
                {
                    break;
                }

                /* 写入新位置 */
                {
                    nand_addr_t waddr = addr_from_pba(new_pba);
                    ret               = ftl_sync_write_page(waddr, buf, FTL_READ_USER_SIZE, FTL_PAGE_CFG_DATA(FC_COMMAND_MODULE_GC, lba));
                }
                if (ret != 0)
                {
                    continue;
                }

                /* 更新 L2P Cache */
                l2p_cache_write(&ftl->cache, lba, new_pba);

                /* Journal 记录 */
                journal_append(&ftl->journal, lba, old_pba, new_pba);

                /* VPC/bitmap 维护：源块清位，新块置位（单点更新防漂移） */
                ftl_block_clr_valid(&ftl->alloc.blocks[gblock_src], ftl->alloc.valid_map[gblock_src], plane, page);
                ftl_block_set_valid(&ftl->alloc.blocks[ftl_pba_gblock(new_pba)],
                                    ftl->alloc.valid_map[ftl_pba_gblock(new_pba)],
                                    (uint8_t)pba_to_plane(new_pba),
                                    (uint8_t)pba_to_page(new_pba));

                migrated++;
            }
        }
    }

    /* F10：搬移条目先落盘再擦除 victim（与 gc_migrate_sb 同契约）。
     * journal 是 write-back，不 flush 则掉电窗口内恢复回放指向已擦除块。 */
    if (migrated > 0U)
    {
        journal_flush_page(&ftl->journal);
    }

    /* Step 2: 构造临时 Super Block，复用 gc_erase_and_free 擦除+归还 */
    {
        super_block_t tmp_sb;

        tmp_sb.plane_blocks[0] = victim->plane_blocks[0];
        tmp_sb.plane_blocks[1] = victim->plane_blocks[1];
        tmp_sb.valid           = 0U; /* gc_erase_and_free 要求 valid==0 */

        gc_erase_and_free(&ftl->alloc, &tmp_sb, ftl->total_writes);
    }

    return (int)migrated;
}

/* wl_execute — 一轮 WL：触发判定 → 选 victim → 搬移；任一步未命中即早退返回 FTL_OK(0) */
int wl_execute(struct ftl_context *ftl)
{
    wl_victim_t victim;

    if (ftl == NULL)
    {
        return FTL_OK;
    }

    if (!wl_should_trigger(ftl))
    {
        return FTL_OK;
    }
    if (!wl_select_victim(ftl, &victim))
    {
        return FTL_OK;
    }

    return wl_relocate(ftl, &victim);
}

/* --------------------------------------------------------------------
 * wl_predictive_relocate
 *   功能：慢死预测旁路——扫出 ECC 趋势恶化的块，整 SB 搬空并**退役**（RESERVED，不标坏）。
 *   参数：ftl —— FTL 实例。
 *   返回：1 命中并处理；0 未命中 / 参数非法。
 *   实现要点：rr_predictive_scan 只返回 CLOSED 块，故可用 sb_id 反查 Closed 列表条目
 *             （ASSERT 保证索引合法）；组装 victim 后复用 wl_relocate 搬移；搬完逐 plane
 *             rr_retire_block 退役（归零 read_count 重计 read disturb），并补 wear_mark_dirty。
 * -------------------------------------------------------------------- */
int wl_predictive_relocate(struct ftl_context *ftl)
{
    uint16_t     gblock;
    uint16_t     closed_idx;
    wl_victim_t  victim;
    uint8_t      plane;
    ftl_block_t *b;

    if (ftl == NULL)
    {
        return 0;
    }

    if (!rr_predictive_scan(&ftl->alloc, &gblock))
    {
        return 0;
    }

    b = &ftl->alloc.blocks[gblock];

    /* 反查 Super Block：慢死块必须在 closed_sb_list 中（scan 已保证 CLOSED） */
    closed_idx = ftl_block_sb_id(b);
    ASSERT(closed_idx < ftl->alloc.closed_sb_count);

    /* 组装 victim（wl_relocate 只用 plane_blocks，其余字段无消费） */
    memset(&victim, 0, sizeof(victim));
    victim.found = 1U;
    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        victim.plane_blocks[plane] =
            ftl->alloc.closed_sb_list[closed_idx].plane_blocks[plane];
    }

    /* 整 SB 搬移（读有效页 → 冷块写入 → L2P/Journal → 擦除归还）。
     * wl_relocate 内部已走 gc_erase_and_free → allocator_remove_closed_sb
     * （gc.c:277-280 按 plane_blocks[0] 自动定位移除 closed 条目），无需外部再清。 */
    (void)wl_relocate(ftl, &victim);

    /* 搬完后各 plane 块退役：不再分配新数据（RESERVED），不标坏（仍可读）。
     * 退化单 plane 条目跳过哨兵 plane。 */
    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        ftl_block_t *retired;

        if (victim.plane_blocks[plane] == FTL_BLOCK_NO_PLANE)
        {
            continue;
        }
        retired = &ftl->alloc.blocks[ftl_gblock(plane, victim.plane_blocks[plane])];
        rr_retire_block(retired);

        /* rr_retire_block 内部把 read_count 归零（read disturb 重计），
         * 它只拿到 ftl_block_t* ⇒ 脏标记放在其唯一调用者这里。 */
        wear_mark_dirty((uint16_t)ftl_gblock(plane, victim.plane_blocks[plane]));
    }

    /* 慢死预测命中：整 SB 已搬空并退役，属生命周期跃迁，留痕一次 */
    LOGW("[FTL] predictive relocate retired p0=%u p1=%u\r\n",
         (unsigned)victim.plane_blocks[0],
         (unsigned)victim.plane_blocks[1]);

    return 1;
}

/* ── 3. 磨损统计（全部跳过坏块）── */

/* wl_max_erase — 全体好块擦除次数的最大值（无块返回 0） */
uint32_t wl_max_erase(const struct ftl_context *ftl)
{
    uint16_t i;
    uint32_t max_ec = 0U;

    if ((ftl == NULL) || (ftl->alloc.blocks == NULL))
    {
        return 0U;
    }

    for (i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        const ftl_block_t *b = &ftl->alloc.blocks[i];
        if (ftl_block_is_bad(b))
        {
            continue; /* 坏块不计入磨损统计 */
        }
        if (b->erase_count > max_ec)
        {
            max_ec = b->erase_count;
        }
    }

    return max_ec;
}

/* wl_min_erase — 全体好块擦除次数的最小值（无好块返回 0） */
uint32_t wl_min_erase(const struct ftl_context *ftl)
{
    uint16_t i;
    uint32_t min_ec = UINT32_MAX;

    if ((ftl == NULL) || (ftl->alloc.blocks == NULL))
    {
        return 0U;
    }

    for (i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        const ftl_block_t *b = &ftl->alloc.blocks[i];
        if (ftl_block_is_bad(b))
        {
            continue; /* 坏块不计入磨损统计 */
        }
        if (b->erase_count < min_ec)
        {
            min_ec = b->erase_count;
        }
    }

    return (min_ec == UINT32_MAX) ? 0U : min_ec;
}

/* wl_avg_erase — 全体好块擦除次数的平均值（无好块返回 0） */
uint32_t wl_avg_erase(const struct ftl_context *ftl)
{
    uint16_t i;
    uint32_t sum   = 0U;
    uint16_t count = 0U;

    if ((ftl == NULL) || (ftl->alloc.blocks == NULL))
    {
        return 0U;
    }

    for (i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        const ftl_block_t *b = &ftl->alloc.blocks[i];
        if (ftl_block_is_bad(b))
        {
            continue; /* 坏块不计入磨损统计 */
        }
        sum += b->erase_count;
        count++;
    }

    if (count == 0U)
    {
        return 0U;
    }
    return sum / count;
}

/* wl_count_blocks_below — 擦除次数严格小于 threshold 的好块数（低擦除区占比统计用） */
uint32_t wl_count_blocks_below(const struct ftl_context *ftl, uint32_t threshold)
{
    uint16_t i;
    uint32_t count = 0U;

    if ((ftl == NULL) || (ftl->alloc.blocks == NULL))
    {
        return 0U;
    }

    for (i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        const ftl_block_t *b = &ftl->alloc.blocks[i];
        if (ftl_block_is_bad(b))
        {
            continue; /* 坏块不计入磨损统计 */
        }
        if (b->erase_count < threshold)
        {
            count++;
        }
    }

    return count;
}

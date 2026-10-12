#include "ftl/gc.h"

#include "common/log.h"
#include "fc/fc.h"
#include "ftl/bbt.h"
#include "ftl/ftl_fc_interface.h"
#include "ftl/journal.h"
#include "ftl/read_recovery.h" /* rr_record_ecc（B4：GC 搬移读记 ECC/read disturb） */
#include "ftl/wear.h"          /* wear_mark_dirty（计数持久化） */
#include "nand/onfi_1_0.h"

#include <string.h>

/*
 * gc_read_page_with_lba - 读取一页 NAND，返回用户数据 + 从 META 提取的 LBA
 *
 * 内部管理 FC 命令生命周期：
 *   alloc → config → submit → sync_wait → read_data → get_meta → release
 *
 * B4：本函数是 GC/WL 搬移读（gc_migrate_sb / wl_relocate）与掉电重建
 * （ftl_rebuild_closed_sb_list 反查）的唯一读入口。此前不记录 ECC/read_count，
 * 慢死预测（rr_record_ecc / read_count++）只挂 host 读 completion，导致
 * GC/WL 搬移读成为 read disturb 盲区。现加 ecc_out / read_count_out 输出。
 *
 * @param pba            物理地址
 * @param buffer         输出：用户数据 buffer（2048 字节，可为 NULL）
 * @param lba_out        输出：从 META 提取的 LBA（可为 NULL）
 * @param ecc_out        输出：本次读 4 CW 最大 ECC 纠正 bit 数（0..4，可为 NULL）
 * @param read_count_out 输出：1=本次读成功（调用方 ++ 该块 read_count，可为 NULL）
 * @return 0 成功, -1 失败
 */
ftl_status_t gc_read_page_with_lba(nand_pba_t pba, uint8_t *buffer, lba_t *lba_out, uint8_t *ecc_out, uint8_t *read_count_out)
{
    fc_command_t *cmd;
    uint32_t      meta_cw0;
    uint8_t       ecc_max = 0U;
    int           ret;

    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    cmd->config.operation   = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
    cmd->config.cw          = 3U;
    cmd->config.crc         = FC_CRC_ENABLE;
    cmd->config.random      = FC_RANDOM_ENABLE;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.multi_plane = 0U;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    cmd->module             = FC_COMMAND_MODULE_GC;
    cmd->meta               = FC_META_UNKNOWN; /* 不知道 LBA——正是要从页内 META 读回来（v24） */

    if (ftl_cmd_setup_ppa(cmd, pba) != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    {
        fc_command_status_t completion;
        ret = ftl_internal_sync_wait(ftl_cmd_id(cmd), &completion, 0U);
        if (ret != 0)
        {
            (void)fc_command_release(cmd);
            return FTL_ERROR;
        }

        if (completion.status & FC_CW_STATUS_NAND_FAIL)
        {
            (void)fc_command_release(cmd);
            return FTL_ERROR;
        }

        /* B4：取 desc 链最大 ECC 纠正 bit 数（与 host 读路径同口径，走 fc_get_cmd_ecc_max）。
         * ⚠ 须在 fc_command_release(cmd) 之前调用——desc 释放后失效。 */
        ecc_max = fc_get_cmd_ecc_max(cmd);
    }

    /* 读取用户数据 */
    if (buffer != NULL)
    {
        (void)ftl_cmd_read_data(cmd, buffer, 2048U);
    }

    /* 从 META 提取**页** LBA（v24）
     * CW0 的 META = 本 CW 的**扇区** LBA = (页 LBA << CW_WIDTH) | [plane][cw=0]
     * 所以 页 LBA = meta_cw0 >> CW_WIDTH（低 3 位 [plane][cw] 丢掉，得到页粒度地址） */
    {
        uint8_t *meta_ptr = fc_buffer_get_meta(cmd, 0U);
        if (meta_ptr != NULL)
        {
            /* meta_ptr 指向 CW buffer，CW 基址 4 字节对齐，memcpy 避免 alignment 警告 */
            memcpy(&meta_cw0, meta_ptr, sizeof(meta_cw0));
        }
        else
        {
            meta_cw0 = 0U;
        }
    }

    (void)fc_command_release(cmd);

    if (lba_out != NULL)
    {
        /* META 低 3 位 = [plane][cw]，高位 = 页 LBA */
        *lba_out = (lba_t)(meta_cw0 >> CW_WIDTH);
    }
    if (ecc_out != NULL)
    {
        *ecc_out = ecc_max;
    }
    if (read_count_out != NULL)
    {
        *read_count_out = 1U; /* 读成功，调用方 ++ 该块 read_count */
    }

    return FTL_OK;
}

void gc_erase_and_free(ftl_allocator_t *alloc, super_block_t *victim, uint32_t now)
{
    uint8_t  plane;
    uint16_t closed_idx;

    if ((alloc == NULL) || (victim == NULL) || (victim->valid != 0U))
    {
        return;
    }

    closed_idx = 0U;
    if (alloc->closed_sb_count > 0U)
    {
        /* 定位 victim 在 closed_sb_list 中的索引。**两 plane 都比对**：Policy B 下
         * 退化单 plane 条目的 plane_blocks[0] 是哨兵，只比 plane0 会命中另一条同哨兵条目。 */
        for (closed_idx = 0U; closed_idx < alloc->closed_sb_count; closed_idx++)
        {
            if ((alloc->closed_sb_list[closed_idx].plane_blocks[0] == victim->plane_blocks[0]) &&
                (alloc->closed_sb_list[closed_idx].plane_blocks[1] == victim->plane_blocks[1]))
            {
                break;
            }
        }
        if (closed_idx >= alloc->closed_sb_count)
        {
            /* 未找到：可能是 WL 临时 SB，无需移除列表 */
            closed_idx = UINT16_MAX;
        }
    }
    else
    {
        closed_idx = UINT16_MAX;
    }

    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        uint16_t    gblock;
        nand_addr_t block_addr;
        int         ret;

        if (super_block_has_plane(victim, plane) == 0U)
        {
            continue; /* 退化单 plane 条目：该 plane 无块可擦 */
        }
        gblock     = ftl_gblock(plane, victim->plane_blocks[plane]);
        block_addr = super_block_page_addr(victim, plane, 0U);

        ret = ftl_sync_erase_block(block_addr);
        if (ret == 0)
        {
            /* 擦除成功：擦除计数 +1，记录最近擦除时间戳（单调写入量），bitmap/VPC 归零，归还 Free Pool */
            alloc->blocks[gblock].erase_count++;
            alloc->blocks[gblock].last_pe_ts = now;
            wear_mark_dirty(gblock);
            ftl_block_clear_valid(&alloc->blocks[gblock], alloc->valid_map[gblock]);
            allocator_free_block(alloc, gblock);
        }
        else
        {
            /* 擦除失败：标记坏块（BBT 单数据源，后台持久化） */
            (void)bbt_mark_bad(alloc, gblock);
        }
    }

    /* 从 Closed 列表移除已回收条目（swap-remove）—— 修复 closed_sb_count 只增不减导致的
     * 列表爆炸 + 已回收 SB 被再次选为 victim 的无限 GC 空转 */
    if (closed_idx != UINT16_MAX)
    {
        allocator_remove_closed_sb(alloc, closed_idx);
    }
}

/*
 * gc_table_extremes - 全表（非坏块）磨损/时间极值，一次扫描
 *
 * 两个消费方共用（避免各扫一遍全表）：
 *   ① gc_select_victim（P1-1）——评分归一化分母（max_erase / max_pe_ts）；
 *   ② gc_check_triggers（P1-2）的 T5——磨损比 max_erase/min_erase。
 *
 * ⚠ 不复用 wear_leveling 的 wl_max_erase/wl_min_erase：它们形参是 ftl_context_t，
 *   而本文件两个调用者只拿得到 ftl_allocator_t（gc_select_victim 调用点
 *   ftl.c:323/389/718 保持不变）。
 *
 * @param alloc      Allocator 上下文（blocks 为 NULL 时输出中性值）
 * @param min_erase  输出：最小擦除次数（无任何可统计块时为 0 ⇒ T5 视为"比值无意义"）
 * @param max_erase  输出：最大擦除次数（兜底 >= 1U，防除零）
 * @param max_pe_ts  输出：最大 last_pe_ts（0 = 本上电周期尚无块被擦 ⇒ 时间项中性）
 */
static void gc_table_extremes(const ftl_allocator_t *alloc, uint32_t *min_erase, uint32_t *max_erase, uint32_t *max_pe_ts)
{
    uint32_t min_e = UINT32_MAX;
    uint32_t max_e = 0U;
    uint32_t max_t = 0U;

    if ((alloc != NULL) && (alloc->blocks != NULL))
    {
        for (uint16_t b = 0U; b < FTL_BLOCK_TOTAL; b++)
        {
            const ftl_block_t *blk = &alloc->blocks[b];

            if (ftl_block_is_bad(blk) != 0)
            {
                continue;
            }
            if (blk->erase_count < min_e)
            {
                min_e = blk->erase_count;
            }
            if (blk->erase_count > max_e)
            {
                max_e = blk->erase_count;
            }
            if (blk->last_pe_ts > max_t)
            {
                max_t = blk->last_pe_ts;
            }
        }
    }

    *min_erase = (min_e == UINT32_MAX) ? 0U : min_e;
    *max_erase = (max_e > 0U) ? max_e : 1U;
    *max_pe_ts = max_t;
}

int gc_select_victim(ftl_allocator_t *alloc, super_block_t **victim)
{
    uint32_t min_erase;
    uint32_t max_erase;
    uint32_t max_pe_ts;
    uint32_t best_cost  = UINT32_MAX;
    uint32_t best_vr    = 0U;
    uint32_t best_er    = 0U;
    uint32_t best_cp    = 0U;
    uint32_t best_yp    = 0U;
    uint16_t best_valid = (uint16_t)(PLANE_NUM * PAGES_PER_BLOCK + 1U);
    int      found      = 0;

    if ((alloc == NULL) || (alloc->blocks == NULL) || (victim == NULL))
    {
        return FTL_OK;
    }

    /* 归一化基准：全表极值（一次扫描，与候选数无关）。min_erase 本函数不用（T5 用）。 */
    gc_table_extremes(alloc, &min_erase, &max_erase, &max_pe_ts);

    for (uint16_t i = 0U; i < alloc->closed_sb_count; i++)
    {
        super_block_t *sb      = &alloc->closed_sb_list[i];
        uint16_t       valid   = 0U;
        uint8_t        present = 0U;
        uint32_t       er_max  = 0U;         /* 本 SB 的磨损代表：各 plane 取大 */
        uint32_t       pe_min  = UINT32_MAX; /* 本 SB 的时间代表：各 plane 取小（越老越优先）*/
        uint32_t       vr;
        uint32_t       er;
        uint32_t       cp;
        uint32_t       yp;
        uint32_t       cost;

        if (sb->valid != 0U)
        {
            continue;
        }

        /* 有效页数 = 各 plane 物理块 VPC 之和（真实计数，替代 next_page 近似）。
         * 退化单 plane 条目（重建产物）跳过哨兵 plane。 */
        for (uint8_t p = 0U; p < PLANE_NUM; p++)
        {
            const ftl_block_t *blk;

            if (super_block_has_plane(sb, p) == 0U)
            {
                continue;
            }
            blk   = &alloc->blocks[ftl_gblock(p, sb->plane_blocks[p])];
            valid = (uint16_t)(valid + blk->valid_count);
            present++;
            if (blk->erase_count > er_max)
            {
                er_max = blk->erase_count;
            }
            if (blk->last_pe_ts < pe_min)
            {
                pe_min = blk->last_pe_ts;
            }
        }
        if (present == 0U)
        {
            continue;
        }

        /* 四项各自归一化到 0..1000；cost 取小 = 越优先回收。
         * erase 项用 uint64_t 中间量防溢出（erase_count 是 uint32_t，×1000 理论上可溢出）。
         * age 项的 `max_pe_ts > pe_min` 同时兜住两种退化：① max_pe_ts == 0（上电后尚无块被擦）；
         * ② 本 SB 的 plane 块已被标坏 ⇒ gc_score_scale 跳过它 ⇒ pe_min 可能大于 max_pe_ts
         * （相减会下溢成大数、把 yp 绕回）。两者一律取 age_ratio = 0（时间项中性）。 */
        vr = ((uint32_t)valid * 1000U) / ((uint32_t)present * PAGES_PER_BLOCK);
        er = (uint32_t)(((uint64_t)er_max * 1000U) / max_erase);
        cp = (sb->temperature == FTL_TEMP_HOT) ? 0U : 1000U;
        yp = 1000U - (((max_pe_ts > pe_min) ? (uint32_t)(((uint64_t)(max_pe_ts - pe_min) * 1000U) / max_pe_ts) : 0U));

        cost = (GC_WEIGHT_VALID * vr) + (GC_WEIGHT_ERASE * er) + (GC_WEIGHT_TEMP * cp) + (GC_WEIGHT_AGE * yp);

        /* cost 相等时取有效页更少者（确定性，且退化为旧的单维贪心） */
        if ((cost < best_cost) || ((cost == best_cost) && (valid < best_valid)))
        {
            best_cost  = cost;
            best_valid = valid;
            best_vr    = vr;
            best_er    = er;
            best_cp    = cp;
            best_yp    = yp;
            *victim    = sb;
            found      = 1;
        }
    }

    if (found != 0)
    {
        /* 观测口：一次空闲 GC 突发最多打 FREE_POOL_TARGET/PLANE_NUM = 16 行
         * （ftl_service_idle 循环调用），相对 GC 自身的 NAND 擦除/搬移耗时可忽略。 */
        LOGI("[GC] victim p0=%u p1=%u vr=%u er=%u cp=%u yp=%u cost=%lu\r\n",
             (unsigned)(*victim)->plane_blocks[0],
             (unsigned)(*victim)->plane_blocks[1],
             (unsigned)best_vr,
             (unsigned)best_er,
             (unsigned)best_cp,
             (unsigned)best_yp,
             (unsigned long)best_cost);
    }

    return found;
}

int gc_migrate_sb(ftl_allocator_t *alloc, l2p_cache_t *cache, l2p_journal_t *journal, super_block_t *victim)
{
    uint8_t  plane;
    uint32_t migrated = 0U;

    if ((alloc == NULL) || (cache == NULL) || (victim == NULL))
    {
        return FTL_OK;
    }

    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        uint16_t gblock_src;
        uint64_t valid_map;

        if (super_block_has_plane(victim, plane) == 0U)
        {
            continue; /* 退化单 plane 条目：该 plane 无块 */
        }
        gblock_src = ftl_gblock(plane, victim->plane_blocks[plane]);
        valid_map  = alloc->valid_map[gblock_src][plane];

        /*
         * 直接扫有效页位图：只搬置位页，跳过无效页（空洞/已覆盖）。
         * 相比逐页读 NAND META + L2P 反查，省去无效页的 NAND 读。
         */
        while (valid_map != 0ULL)
        {
            uint8_t page = (uint8_t)__builtin_ctzll(valid_map);
            valid_map &= (valid_map - 1ULL); /* 清最低置位 bit */

            {
                uint8_t    buf[2048];
                lba_t      lba;
                nand_pba_t pba;
                nand_pba_t old_pba;
                nand_pba_t new_pba;
                int        ret;

                pba = super_block_page_pba(alloc->ch, alloc->ce, victim, plane, page);

                /* 读页 + 获取 LBA + 验证有效性（bitmap 已保证置位，此处只取 LBA） */
                memset(buf, 0, sizeof(buf));
                {
                    uint8_t ecc;
                    uint8_t rc;
                    if (gc_read_page_with_lba(pba, buf, &lba, &ecc, &rc) != 0)
                    {
                        continue; /* 读失败，跳过 */
                    }

                    /* B4：GC 搬移读记录 ECC 趋势 + read disturb 计数（慢死预测闭环）。
                     * 判定只用 ecc_bits 与上一次读数比较，不涉及时间维度。 */
                    if (rc != 0U)
                    {
                        ftl_block_t *src = &alloc->blocks[gblock_src];
                        rr_record_ecc(&src->rr, ecc);
                        src->read_count++;
                        wear_mark_dirty(gblock_src);
                    }
                }

                /* 查 L2P：当前映射是否仍指向本 PBA（bitmap 与 L2P 双校验） */
                old_pba = l2p_cache_read(cache, lba);
                if (old_pba != pba)
                {
                    /* 已被覆盖，无效页 → 清 bitmap 保持同步 */
                    ftl_block_clr_valid(&alloc->blocks[gblock_src], alloc->valid_map[gblock_src], plane, page);
                    continue;
                }

                /* 分配新 PBA（Cold Super Block） */
                new_pba = allocator_alloc(alloc, FTL_TEMP_COLD);
                if (new_pba == NAND_PBA_INVALID)
                {
                    break;
                }

                /* 写用户数据到新位置 */
                {
                    nand_addr_t waddr = addr_from_pba(new_pba);
                    ret               = ftl_sync_write_page(waddr, buf, sizeof(buf), FTL_PAGE_CFG_DATA(FC_COMMAND_MODULE_GC, lba));
                }
                if (ret != 0)
                {
                    continue;
                }

                /* 更新 L2P Cache */
                l2p_cache_write(cache, lba, new_pba);

                /* Journal 记录 */
                if (journal != NULL)
                {
                    journal_append(journal, lba, old_pba, new_pba);
                }

                /* VPC/bitmap 维护：源块清位，新块置位（单点更新防漂移） */
                ftl_block_clr_valid(&alloc->blocks[gblock_src], alloc->valid_map[gblock_src], plane, page);
                ftl_block_set_valid(&alloc->blocks[ftl_pba_gblock(new_pba)],
                                    alloc->valid_map[ftl_pba_gblock(new_pba)],
                                    (uint8_t)pba_to_plane(new_pba),
                                    (uint8_t)pba_to_page(new_pba));

                migrated++;
            }
        }
    }

    /* F10：搬移条目先落盘再擦除 victim。journal 是 write-back（page_buf 攒
     * 满才 flush），若不在此强制 flush，掉电落在"搬移条目未落盘 + victim
     * 已擦除"窗口 → 恢复回放指向已擦除块 = 数据损坏。 */
    if ((journal != NULL) && (migrated > 0U))
    {
        journal_flush_page(journal);
    }

    return (int)migrated;
}

uint32_t gc_check_triggers(const ftl_allocator_t *alloc, uint32_t total_writes, uint32_t elapsed_ms)
{
    uint32_t triggers = 0U;

    if (alloc == NULL)
    {
        return 0U;
    }

    /* T1：空闲块水位（主动补池，还留有余量就开工） */
    if (alloc->free_count < FREE_POOL_TARGET)
    {
        triggers |= GC_TRIGGER_WATERMARK;
    }
    /* T2：写入量（把 GC 压力摊平到整个写入流上，避免攒到水位线才爆发） */
    if ((total_writes % GC_INTERVAL_WRITES) == 0U)
    {
        triggers |= GC_TRIGGER_WRITES;
    }
    /* T3：定时（host 不写也不擦时，让空闲期把池补回去） */
    if (elapsed_ms >= GC_TIMER_INTERVAL_MS)
    {
        triggers |= GC_TRIGGER_TIMER;
    }
    /* T4：紧急（池已见底的最后手段）。与 T1 同源但阈值更低（8 vs 32）——
     * 调用点据此决定"排一次 GC"还是"当场同步回收"。 */
    if (alloc->free_count <= FREE_POOL_EMERGENCY)
    {
        triggers |= GC_TRIGGER_EMERGENCY;
    }
    /* T5：磨损均衡（全表擦除比）。min_erase == 0 时比值无意义（对 0 除）⇒ 不触发：
     * "有块从未被擦过"是静态 WL（wl_execute 搬冷数据）的职责，T5 只管
     * "所有块都在轮转、但轮得不均"。max_erase 地板防早期（max 才几次）空跑。 */
    {
        uint32_t min_erase;
        uint32_t max_erase;
        uint32_t max_pe_ts;

        gc_table_extremes(alloc, &min_erase, &max_erase, &max_pe_ts);
        if ((max_erase >= GC_WEAR_LEVEL_MIN_ERASE) && (min_erase > 0U) &&
            ((max_erase / min_erase) > GC_WEAR_LEVEL_RATIO))
        {
            triggers |= GC_TRIGGER_WEAR;
        }
    }

    return triggers;
}

int gc_check_trigger(const ftl_allocator_t *alloc, uint32_t total_writes, uint32_t elapsed_ms)
{
    return (gc_check_triggers(alloc, total_writes, elapsed_ms) != 0U) ? 1 : FTL_OK;
}
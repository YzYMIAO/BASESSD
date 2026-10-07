#include "ftl/l2p_cache.h"

#include "fc/fc.h"
#include "ftl/ftl_fc_interface.h"
#include "nand/onfi_1_0.h"

#include <string.h>

void l2p_cache_init(l2p_cache_t *cache, uint8_t ch, uint8_t ce, uint8_t lun)
{
    uint16_t i;

    if (cache == NULL)
    {
        return;
    }

    cache->ch  = ch;
    cache->ce  = ce;
    cache->lun = lun;

    /* Region 级脏标记：开机全清（F4） */
    memset(cache->region_dirty_bitmap, 0, sizeof(cache->region_dirty_bitmap));

    for (i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        l2p_cache_line_t *line = &cache->lines[i];

        /* 全部 entry 填 FTL_PBA_INVALID */
        memset(line->entries, 0xFF, sizeof(line->entries));
        line->region_id    = 0U;
        line->state        = L2P_CACHE_INVALID;
        line->access_count = 0U;
    }

    cache->clock_hand = 0U;
    cache->hit_count  = 0U;
    cache->miss_count = 0U;
    cache->md_dir     = NULL;
    cache->alloc      = NULL;
}

/*
 * 内部查找：在 cache 中查找 region_id 对应的 line。
 * 只匹配 CLEAN 或 DIRTY 状态，FLUSHING 和 INVALID 不视为命中。
 * 命中返回 line 指针，未命中返回 NULL。
 */
static l2p_cache_line_t *l2p_cache_find(l2p_cache_t *cache, uint16_t region_id)
{
    uint16_t i;

    for (i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        l2p_cache_line_t *line = &cache->lines[i];

        if ((line->region_id == region_id) &&
            ((line->state == L2P_CACHE_CLEAN) || (line->state == L2P_CACHE_DIRTY)))
        {
            return line;
        }
    }

    return NULL;
}

/*
 * 内部 CLOCK evict：从 clock_hand 开始扫描，选一个可驱逐的 line。
 *
 * CLOCK 规则：
 *   FLUSHING    → 跳过（写回中，不可驱逐）
 *   access_count > 0 → access_count--，检查下一个（第二次机会）
 *   INVALID     → 直接占用
 *   DIRTY       → flush 后再占用
 *   access_count==0（CLEAN）→ 直接占用
 *
 * 返回 line 索引（-1 表示全 FLUSHING 无法驱逐）。
 */
static int l2p_cache_evict_one(l2p_cache_t *cache)
{
    uint16_t start = cache->clock_hand;
    uint16_t i;

    for (i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        uint16_t          idx  = (uint16_t)((start + i) % L2P_CACHE_SIZE);
        l2p_cache_line_t *line = &cache->lines[idx];

        if (line->state == L2P_CACHE_FLUSHING)
        {
            continue; /* 跳过正在写回的 line */
        }

        if (line->state == L2P_CACHE_INVALID)
        {
            /* 空行，直接占用 */
            cache->clock_hand = (uint16_t)((idx + 1U) % L2P_CACHE_SIZE);
            return (int)idx;
        }

        if (line->access_count > 0U)
        {
            /* 第二次机会：减计数，检查下一个 */
            line->access_count--;
            continue;
        }

        /* access_count == 0 且不是 FLUSHING/INVALID → 选中 */
        cache->clock_hand = (uint16_t)((idx + 1U) % L2P_CACHE_SIZE);

        if (line->state == L2P_CACHE_DIRTY)
        {
            /*
             * 脏行先写回再复用（F4 关键前置）：
             * flush 失败 → 脏数据仍在该行，NAND 页仍旧、region 脏标记仍置位。
             * 此刻绝不能复用本行（当前实现直接置 INVALID 会丢掉 RAM 脏数据，
             * 留下"bit 置位但无 resident DIRTY 行"的孤儿 bit，checkpoint 会
             * 按 bitmap 假声明该区已落盘 → boot 必坏读）。视同 FLUSHING 跳过，
             * 继续找下一个可驱逐行。
             */
            if (l2p_cache_flush_one(cache, (int)idx) != FTL_OK)
            {
                continue;
            }
        }

        /* 标记为 INVALID，等待调用者重新初始化 */
        line->state = L2P_CACHE_INVALID;
        return (int)idx;
    }

    return FTL_ERROR; /* 全 FLUSHING / 全部可驱逐行 flush 失败 */
}

/*
 * 内部查找或加载：找到 region_id 对应的 line。
 *   hit  → hit_count++，access_count++，返回 line
 *   miss → evict + 从 NAND 加载 region，miss_count++，返回 line
 *
 * 所有 cache 入口（read/write）都通过此函数访问 line，不存在重复查找。
 */
static l2p_cache_line_t *l2p_cache_find_or_load(l2p_cache_t *cache, uint16_t region_id)
{
    l2p_cache_line_t *line;
    int               victim_idx;

    /* 先查找 */
    line = l2p_cache_find(cache, region_id);
    if (line != NULL)
    {
        /* hit */
        line->access_count++;
        cache->hit_count++;
        return line;
    }

    /* miss */
    cache->miss_count++;

    victim_idx = l2p_cache_evict_one(cache);
    if (victim_idx < 0)
    {
        return NULL;
    }

    line = &cache->lines[victim_idx];

    /* 占位 */
    memset(line->entries, 0xFF, sizeof(line->entries));
    line->region_id    = region_id;
    line->access_count = 1U;

    /*
     * 从 NAND L2P Table 加载 Region 数据。
     *
     * 通过 Metadata Directory 查找 Region 的物理位置（动态分配）。
     * 如果 Directory 中未找到（首次访问且未分配），回退到固定位置映射。
     * CRC/Random DISABLED：Region 数据不经过 CRC/Random，只保护 ECC。
     */
    {
        nand_paa_t  region_paa = NAND_PBA_INVALID;
        nand_addr_t region_addr;

        if (cache->md_dir != NULL)
        {
            region_paa = metadata_dir_lookup(cache->md_dir,
                                             METADATA_TYPE_L2P_REGION,
                                             region_id);
        }

        if (region_paa != NAND_PBA_INVALID)
        {
            /* 通过 Directory 查到位置 → 展开地址（ch/ce 取本 cache 拓扑） */
            region_addr = addr_from_paa(cache->ch, cache->ce, region_paa);
        }
        else
        {
            /* 回退固定位置 → 展开地址（ch/ce/lun 取本 cache 拓扑） */
            region_addr = addr_from_pba(l2p_region_pba(cache->ch, cache->ce, cache->lun, region_id));
        }

        int ret = ftl_sync_read_page(region_addr, line->entries, L2P_REGION_SIZE, FTL_PAGE_CFG_META(FC_COMMAND_MODULE_METADATA));
        if (ret == 0)
        {
            line->state = L2P_CACHE_CLEAN;
        }
        else
        {
            /* 读失败，保持 INVALID，entries 为 FTL_PBA_INVALID */
            line->state = L2P_CACHE_INVALID;
        }
    }

    return line;
}

nand_pba_t l2p_cache_read(l2p_cache_t *cache, lba_t lba)
{
    uint16_t          region_id;
    uint16_t          entry_idx;
    l2p_cache_line_t *line;

    if ((cache == NULL) || (lba >= L2P_REGIONS_PER_LUN * L2P_ENTRIES_PER_REGION))
    {
        return FTL_PBA_INVALID;
    }

    region_id = lba_to_region(lba);
    entry_idx = lba_to_entry(lba);

    line = l2p_cache_find_or_load(cache, region_id);
    if (line == NULL)
    {
        return FTL_PBA_INVALID;
    }

    return line->entries[entry_idx];
}

void l2p_cache_write(l2p_cache_t *cache, lba_t lba, nand_pba_t pba)
{
    uint16_t          region_id;
    uint16_t          entry_idx;
    l2p_cache_line_t *line;

    if ((cache == NULL) || (lba >= L2P_REGIONS_PER_LUN * L2P_ENTRIES_PER_REGION))
    {
        return;
    }

    region_id = lba_to_region(lba);
    entry_idx = lba_to_entry(lba);

    line = l2p_cache_find_or_load(cache, region_id);
    if (line == NULL)
    {
        return;
    }

    line->entries[entry_idx] = pba;
    line->state              = L2P_CACHE_DIRTY;

    /* Region 级脏标记（F4）：置本 region 的 bit。flush 落盘成功后清零。 */
    cache->region_dirty_bitmap[region_id / 32U] |= (1UL << (region_id & 31U));
}

ftl_status_t l2p_cache_flush_one(l2p_cache_t *cache, int line_idx)
{
    l2p_cache_line_t *line;
    nand_addr_t       region_addr;
    int               ret;
    nand_paa_t        region_paa = NAND_PBA_INVALID;

    if ((cache == NULL) || (line_idx < 0) || (line_idx >= (int)L2P_CACHE_SIZE))
    {
        return FTL_ERROR;
    }

    line = &cache->lines[line_idx];

    if (line->state != L2P_CACHE_DIRTY)
    {
        return FTL_OK; /* 不需要 flush */
    }

    /* 标记为 FLUSHING，防止 evict 选中 */
    line->state = L2P_CACHE_FLUSHING;

    /*
     * 计算 Region 在 NAND 上的位置。
     * 优先通过 Metadata Directory 查找（动态分配），
     * 如果还未分配（首次写入），通过 Directory 分配新页。
     */
    {
        if (cache->md_dir != NULL)
        {
            region_paa = metadata_dir_lookup(cache->md_dir,
                                             METADATA_TYPE_L2P_REGION,
                                             line->region_id);
        }

        if (region_paa != NAND_PBA_INVALID)
        {
            /* 已知位置 → 展开地址（ch/ce 取本 cache 拓扑） */
            region_addr = addr_from_paa(cache->ch, cache->ce, region_paa);
        }
        else if ((cache->md_dir != NULL) && (cache->alloc != NULL))
        {
            /* 未分配 → 通过 Directory 动态分配新位置 */
            region_paa = metadata_dir_alloc_region(cache->md_dir,
                                                   cache->alloc,
                                                   line->region_id);
            if (region_paa != NAND_PBA_INVALID)
            {
                region_addr = addr_from_paa(cache->ch, cache->ce, region_paa);
            }
            else
            {
                /* 动态分配失败（Free Pool 空），回退固定位置 */
                region_addr = addr_from_pba(l2p_region_pba(cache->ch, cache->ce, cache->lun, line->region_id));
            }
        }
        else
        {
            /* 回退固定位置（md_dir 或 alloc 不可用） */
            region_addr = addr_from_pba(l2p_region_pba(cache->ch, cache->ce, cache->lun, line->region_id));
        }
    }

    /*
     * 同步写 Region 到 NAND L2P Table。
     * CRC/Random DISABLED（元数据不走加扰/CRC），仅 BCH ECC 保护。
     */
    ret = ftl_sync_write_page(region_addr, line->entries, L2P_REGION_SIZE, FTL_PAGE_CFG_META(FC_COMMAND_MODULE_METADATA));

    if (ret == 0)
    {
        line->state = L2P_CACHE_CLEAN;

        /* 落盘成功：本 region 的脏标记清零（下次 checkpoint 不再重写） */
        cache->region_dirty_bitmap[line->region_id / 32U] &=
            ~(1UL << (line->region_id & 31U));
    }
    else
    {
        /* 写失败，恢复 DIRTY，后续可重试；脏标记保持置位 */
        line->state = L2P_CACHE_DIRTY;
    }

    return ret;
}

ftl_status_t l2p_cache_flush_all(l2p_cache_t *cache)
{
    uint16_t i;

    if (cache == NULL)
    {
        return FTL_ERROR;
    }

    for (i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        if (cache->lines[i].state == L2P_CACHE_DIRTY)
        {
            int ret = l2p_cache_flush_one(cache, (int)i);
            if (ret != 0)
            {
                return ret;
            }
        }
    }

    return FTL_OK;
}
#include "ftl/l2p_cache.h"

#include "ftl/ftl_fc_interface.h"
#include "nand/onfi_1_0.h"

#include <string.h>

void l2p_cache_init(l2p_cache_t *cache)
{
    uint16_t i;

    if (cache == NULL)
    {
        return;
    }

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
            (void)l2p_cache_flush_one(cache, idx);
        }

        /* 标记为 INVALID，等待调用者重新初始化 */
        line->state = L2P_CACHE_INVALID;
        return (int)idx;
    }

    return -1; /* 全 FLUSHING */
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
        nand_paa_t region_paa = NAND_PBA_INVALID;
        nand_pba_t region_pba;

        if (cache->md_dir != NULL)
        {
            region_paa = metadata_dir_lookup(cache->md_dir,
                                             METADATA_TYPE_L2P_REGION,
                                             region_id);
        }

        if (region_paa != NAND_PBA_INVALID)
        {
            /* 通过 Directory 查到位置 */
            region_pba = pba_from_paa(0, 0, region_paa);
        }
        else
        {
            /* 回退固定位置 */
            region_pba = l2p_region_pba(0, 0, region_id);
        }

        int ret = ftl_internal_read_page(region_pba,
                                         line->entries,
                                         L2P_REGION_SIZE,
                                         FC_CRC_DISABLE,
                                         FC_RANDOM_DISABLE,
                                         FC_COMMAND_MODULE_METADATA,
                                         0U);
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

    if ((cache == NULL) || (lba >= L2P_REGIONS_TOTAL * L2P_ENTRIES_PER_REGION))
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

    if ((cache == NULL) || (lba >= L2P_REGIONS_TOTAL * L2P_ENTRIES_PER_REGION))
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
}

int l2p_cache_flush_one(l2p_cache_t *cache, int line_idx)
{
    l2p_cache_line_t *line;
    nand_pba_t        region_pba;
    int               ret;
    nand_paa_t        region_paa = NAND_PBA_INVALID;

    if ((cache == NULL) || (line_idx < 0) || (line_idx >= (int)L2P_CACHE_SIZE))
    {
        return -1;
    }

    line = &cache->lines[line_idx];

    if (line->state != L2P_CACHE_DIRTY)
    {
        return 0; /* 不需要 flush */
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
            region_pba = pba_from_paa(0, 0, region_paa);
        }
        else if (cache->md_dir != NULL)
        {
            /* 未分配 → 分配新页 */
            /* 注意：这里需要 alloc，但 l2p_cache_flush_one 没有 alloc 参数。
             * 简化：先回退到固定位置，后续使用 ftl_context_t 中的 alloc 完善。 */
            region_pba = l2p_region_pba(0, 0, line->region_id);
        }
        else
        {
            region_pba = l2p_region_pba(0, 0, line->region_id);
        }
    }

    /*
     * 同步写 Region 到 NAND L2P Table。
     * CRC/Random DISABLED（元数据不走加扰/CRC），仅 BCH ECC 保护。
     */
    ret = ftl_internal_write_page(region_pba,
                                  line->entries,
                                  L2P_REGION_SIZE,
                                  FC_CRC_DISABLE,
                                  FC_RANDOM_DISABLE,
                                  FC_COMMAND_MODULE_METADATA,
                                  0U);

    if (ret == 0)
    {
        line->state = L2P_CACHE_CLEAN;
    }
    else
    {
        /* 写失败，恢复 DIRTY，后续可重试 */
        line->state = L2P_CACHE_DIRTY;
    }

    return ret;
}

int l2p_cache_flush_all(l2p_cache_t *cache)
{
    uint16_t i;

    if (cache == NULL)
    {
        return -1;
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

    return 0;
}
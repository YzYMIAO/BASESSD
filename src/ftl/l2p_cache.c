#include "ftl/l2p_cache.h"

#include "common/log.h"
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

    /* Thrashing 检测窗口（P1-4）：开机从「非降级」起步 */
    cache->window_access = 0U;
    cache->window_miss   = 0U;
    cache->miss_pct      = 0U;
    cache->thrashing     = 0U;
}

/*
 * Thrashing 窗口结算（P1-4）。
 *
 * 每次 cache 访问（命中或未命中）调用一次；窗口满 L2P_THRASH_WINDOW 时按 miss 率
 * 与滞回阈值更新 thrashing 标志并清零窗口。仅状态**翻转**时打一行观测日志
 * （阈值附近不抖动 ⇒ 不会刷屏）。
 */
static void l2p_cache_note_access(l2p_cache_t *cache, uint8_t is_miss)
{
    uint8_t was_thrashing;

    cache->window_access++;
    if (is_miss != 0U)
    {
        cache->window_miss++;
    }

    if (cache->window_access < L2P_THRASH_WINDOW)
    {
        return;
    }

    cache->miss_pct      = (uint8_t)((cache->window_miss * 100U) / cache->window_access);
    was_thrashing        = cache->thrashing;
    cache->window_access = 0U;
    cache->window_miss   = 0U;

    /* 滞回：进入 ≥90%，退出 <70%，中间保持原态 */
    if (cache->miss_pct >= L2P_THRASH_ENTER_PCT)
    {
        cache->thrashing = 1U;
    }
    else if (cache->miss_pct < L2P_THRASH_EXIT_PCT)
    {
        cache->thrashing = 0U;
    }

    if (cache->thrashing != was_thrashing)
    {
        /* 观测口：miss= 为最近窗口 miss 率；hit/miss= 为开机累计。 */
        LOGI("[L2P] thrash=%u miss=%u%% hit=%lu miss_total=%lu\r\n",
             (unsigned)cache->thrashing,
             (unsigned)cache->miss_pct,
             (unsigned long)cache->hit_count,
             (unsigned long)cache->miss_count);
    }
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
        l2p_cache_note_access(cache, 0U);
        return line;
    }

    /* miss */
    cache->miss_count++;
    l2p_cache_note_access(cache, 1U);

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
     * 从 NAND 加载 Region 数据。
     *
     * 位置一律来自 Metadata Directory（每个 region 在 fill_fixed 就登记好：初始指向
     * **哨兵层**固定页，被刷新后指向日志块）。哨兵页永不写入 ⇒ 读回全 0xFF ⇒
     * 逐项即 PBA_INVALID = "未映射"，正是首次访问应有的语义。
     * 下面的固定位置回退仅作 md_dir 缺失时的兜底（正常路径不可达）。
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

    if ((cache == NULL) || (lba >= L2P_REGIONS_PER_INSTANCE * L2P_ENTRIES_PER_REGION))
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

    if ((cache == NULL) || (lba >= L2P_REGIONS_PER_INSTANCE * L2P_ENTRIES_PER_REGION))
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
     * 追加写（元数据写模型 ①：只写擦除过的空页）。
     *
     * 每次刷新都从当前 L2P 日志块取**下一个空槽**，绝不就地覆盖旧位置 ——
     * NAND 只能 1→0，对已编程页二次编程 = 新数据 AND 旧数据 = 静默的映射损坏。
     * 旧位置由 metadata_dir_commit_region 的活计数记账，归零后整块回收。
     *
     * 分配不到块（Free Pool 空）⇒ FTL_ERROR，**不回退固定位置**：固定层是哨兵
     * （永不写入，读回全 0xFF = 未映射），写进去就等于把"未映射"变成"有映射"，
     * 且下次再写同一页即损坏。
     */
    if ((cache->md_dir == NULL) || (cache->alloc == NULL))
    {
        line->state = L2P_CACHE_DIRTY;
        return FTL_ERROR; /* 无 Directory/Allocator ⇒ 无法持久化，不猜位置 */
    }

    region_paa = metadata_dir_append_slot(cache->md_dir, cache->alloc);
    if (region_paa == NAND_PBA_INVALID)
    {
        line->state = L2P_CACHE_DIRTY; /* 上层重试 / 紧急 GC 后再来 */
        return FTL_ERROR;
    }

    region_addr = addr_from_paa(cache->ch, cache->ce, region_paa);

    /*
     * 同步写 Region 到 NAND L2P 日志块。
     * CRC/Random DISABLED（元数据不走加扰/CRC），仅 BCH ECC 保护。
     */
    ret = ftl_sync_write_page(region_addr, line->entries, L2P_REGION_SIZE, FTL_PAGE_CFG_META(FC_COMMAND_MODULE_METADATA));

    if (ret == 0)
    {
        /* 落盘成功才登记新位置（顺序不可倒：先写后记账，掉电最多多留一份旧记录）*/
        metadata_dir_commit_region(cache->md_dir, cache->alloc, line->region_id, region_paa);

        line->state = L2P_CACHE_CLEAN;

        /* 落盘成功：本 region 的脏标记清零（下次 checkpoint 不再重写） */
        cache->region_dirty_bitmap[line->region_id / 32U] &=
            ~(1UL << (line->region_id & 31U));
    }
    else
    {
        /* 写失败：槽位已在 append_slot 里消耗掉（可能半编程，不可复用），
         * Directory 不登记 ⇒ 该 region 仍指向上一份有效记录，无信息丢失。 */
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
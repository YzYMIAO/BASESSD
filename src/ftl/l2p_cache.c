/*
 * l2p_cache.c — L2P Region 缓存（CLOCK 替换 + 脏行写回 + thrashing 检测）
 *
 * 职责：
 *   把 L2P 映射表按 region（L2P_ENTRIES_PER_REGION 条 entry/region）缓存进 SRAM，
 *   提供 lba→pba 的读/写；未命中时经 Metadata Directory 定位并从 NAND 加载；
 *   脏行写回走「追加写」日志块。
 *
 * 与 l2p_cache.h 的分工：
 *   l2p_cache.h —— 缓存结构（l2p_cache_t / l2p_cache_line_t）+ 尺寸宏 + API；
 *   本文件     —— 查找 / 驱逐 / 加载 / 写回的实现 + thrashing 窗口检测。
 *
 * 替换算法（CLOCK，ADR-5）：
 *   access_count 作引用位，clock_hand 环形扫描；命中置 1，逐行递减给"第二次机会"；
 *   全 FLUSHING 时无法驱逐（返回 -1）。见 l2p_cache_evict_one。
 *
 * 写模型（P0-2）：
 *   脏行写回 = 从 L2P 日志块取**下一个空槽**追加写（绝不就地覆盖——NAND 只能 1→0，
 *   二次编程 = 新数据 AND 旧数据 = 静默损坏）；新位置由 metadata_dir_commit_region 记账。
 *
 * 文件结构：
 *   1. 初始化         l2p_cache_init
 *   2. Thrashing 检测 l2p_cache_note_access（static）
 *   3. 查找/驱逐/加载 l2p_cache_find / evict_one / find_or_load（static）
 *   4. 对外读写       l2p_cache_read / l2p_cache_write
 *   5. 写回           l2p_cache_flush_one / l2p_cache_flush_all
 */

#include "ftl/l2p_cache.h"

#include "common/log.h"
#include "fc/fc.h"
#include "ftl/ftl_fc_interface.h"
#include "nand/onfi_1_0.h"

#include <string.h>

/* --------------------------------------------------------------------
 * l2p_cache_init
 *   功能：初始化 cache——清脏标记、逐行置 INVALID、复位 CLOCK 与统计。
 *   参数：cache —— 待初始化对象；ch/ce/lun —— 本实例拓扑（展开物理地址用）。
 *   返回：无（cache == NULL 直接返回）。
 *   实现要点：⚠ md_dir / alloc **刻意不清**（宿主注入的依赖，见函数内长注释）——
 *             在此清成 NULL 会解绑 ftl_init 刚绑好的 Directory 且无人再绑。
 * -------------------------------------------------------------------- */
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

    /* ⚠ md_dir / alloc **刻意不在这里清**。
     *
     * 这两个指针是**宿主注入的依赖**（由 ftl_init 经 l2p_cache_set_md_dir 绑定），
     * 不是 cache 自己的状态。本函数在三条开机恢复路径（fresh / clean / power-failure）
     * 里都会被调用——若在此清成 NULL，就会把 ftl_init 刚绑好的 Directory 解绑，且
     * **无人再绑**（l2p_cache_set_md_dir 只在 ftl_init 调）。后果：
     *   ① l2p_cache_find_or_load 永远走固定位置回退 ⇒ 读的永远是**哨兵页**（全 0xFF
     *      = 未映射），Directory 形同虚设；
     *   ② l2p_cache_flush_one 恒 FTL_ERROR（依赖为 NULL）⇒ L2P region **从不落盘**
     *      ⇒ metadata_dir_update 不被调用 ⇒ dir->dirty 恒 0 ⇒ 首次 flush_to_anchor
     *      早退 ⇒ Anchor **从不写**（开机 [META] anchor_seq 恒 0）。
     * ftl_init 侧本来就在 init 之后显式 l2p_cache_set_md_dir(NULL, &alloc) 声明"尚未加载"，
     * 故此处无需（也不应）代劳。 */

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
     * Random DISABLED / CRC ENABLED（v0.62）：Region 数据不加扰，但经 FC per-CW CRC + ECC 保护。
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

/* l2p_cache_read — 读 lba 的 PBA；越界 / 未命中 / 未映射一律返回 FTL_PBA_INVALID */
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

/* l2p_cache_write — 更新 lba 的 PBA，置行 DIRTY 并置 region 脏标记（越界 no-op） */
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

/* --------------------------------------------------------------------
 * l2p_cache_flush_one
 *   功能：把一行脏 region 写回 NAND L2P 日志块（追加写），成功后登记新位置。
 *   参数：cache —— 缓存；line_idx —— 行下标。
 *   返回：FTL_OK 成功（含"本行非 DIRTY 无需 flush"）；FTL_ERROR 参数非法 / 无依赖 /
 *         分配不到槽 / 写失败。
 *   实现要点：
 *     · 置 FLUSHING 防 evict 选中；追加写绝不就地覆盖（见文件头写模型）。
 *     · **先写后记账**（顺序不可倒）：掉电最多多留一份旧记录，不会丢映射。
 *     · 分配不到块 ⇒ 直接 FTL_ERROR，**不回退固定位置**（固定层是哨兵，写进去等于
 *       把"未映射"变成"有映射"，且下次再写同一页即损坏）。
 * -------------------------------------------------------------------- */
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
     * Random DISABLED / CRC ENABLED（v0.62）：元数据不加扰，经 FC per-CW CRC + BCH ECC 保护。
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

/* l2p_cache_flush_all — 逐行 flush 全部 DIRTY 行；任一失败立即返回该错误码（不跳过） */
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

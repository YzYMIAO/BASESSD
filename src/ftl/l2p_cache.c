/*
 * l2p_cache.c — 共享 L2P Region 缓存（CLOCK + 异步加载/写回 + thrashing 检测）
 *
 * 职责：
 *   把 L2P 映射表按 region（L2P_ENTRIES_PER_REGION 条 entry/region）缓存进 SRAM，
 *   提供全局 LBA → PBA 的读/写；未命中时经 **owner 实例** 的 Metadata Directory
 *   定位并从 NAND 加载；脏行写回走 owner 的「追加写」日志块。
 *
 * 共享模型：
 *   同一 (ch,ce) 下的所有 LUN 共用一份 cache（g_l2p_cache[ch][ce]）。
 *   cache 行/脏位图的键是**全局 region**；映射页在 NAND 上的位置是
 *   **owner die 内的局部 region**（global_region / FTL_DIM_TOTAL）。
 *   owner 由全局 region 经注入的 owner_of_region 回调解析；load/flush/commit
 *   一律使用 owner 的 metadata_dir / alloc / topo。
 *
 * 与 l2p_cache.h 的分工：
 *   l2p_cache.h —— 缓存结构 + 坐标宏 + API；本文件 —— 查找/驱逐/加载/写回实现 +
 *   thrashing 检测 + 完成服务。
 *
 * 替换算法（CLOCK）：
 *   access_count 作引用位，clock_hand 环形扫描；命中置 1，逐行递减给"第二次机会"；
 *   全在飞（LOADING / FLUSHING）时无法驱逐（返回 -1）。
 *
 * 写模型：
 *   脏行写回 = 从 owner 的 L2P 日志块取下一空槽追加写（绝不就地覆盖——NAND 只能
 *   1→0，二次编程 = 静默损坏）；新位置由 metadata_dir_commit_region 记账。
 *
 * 异步化：
 *   加载与写回都是「提交即返回」——未命中/脏行 ⇒ 建命令 + ftl_op_submit(tag=FTL_OP_L2P,
 *   ctx = 本 cache line) ⇒ 置 LOADING/FLUSHING ⇒ 返回 FTL_AGAIN。完成由
 *   g_op_callbacks[FTL_OP_L2P]（本文件末尾的 ftl_l2p_callback）按 line->state 收尾。
 *   ⇒ 对外 API 三态；结构性同步上下文走 `*_sync` 逃生口（自旋泵）。
 *
 * 文件结构：
 *   1. 初始化         l2p_cache_init / owner 解析
 *   2. Thrashing 检测 l2p_cache_note_access（static）
 *   3. 查找/驱逐      l2p_cache_find / evict_one（static）
 *   4. 加载           l2p_cache_load_start（static）
 *   5. 对外读写       l2p_cache_read / ready / write + *_sync 逃生口
 *   6. 写回           l2p_cache_flush_kick / l2p_flush_kick_owner / flush_*_sync
 *   7. 完成服务       ftl_l2p_callback（g_op_callbacks[FTL_OP_L2P]）
 *   8. 恢复辅助       invalidate_owner / snapshot_owner_bitmap
 */

#include "l2p_cache.h"

#include "assertion.h"
#include "bg_scheduler.h" /* BG_LEVEL_*（档位 → max_kick 表） */
#include "fc.h"
#include "fc_interface.h" /* fc_get_resource_stats（写回前的资源预检） */
#include "ftl.h"          /* ftl_context_t / ftl_service_pump（同步逃生口用） */
#include "ftl_fc_interface.h"
#include "ftl_job.h" /* 心跳（[JOB] 观测口）*/
#include "ftl_op.h"  /* ftl_op_submit / ftl_op_in_dispatch */
#include "log.h"
#include "onfi_1_0.h"

#include <string.h>

/* 同一 region 连续加载失败多少次后降级为「全未映射的 CLEAN 行」（RISK-L2P-01）。
 * 取 3：一次瞬态读错（NAND 忙 / 命令超时）不应永久丢映射；持续失败则必须止损，
 * 否则每次访问都重读坏 region、host 永久停在 FTL_AGAIN。 */
#define L2P_LOAD_RETRY_MAX (3U)

/* ── 位图原语（本文件内部）──
 * 脏位图是 flat 绝对编号（bit = 全局 region）。set/clear/test 集中在此，
 * 避免调用点各自手写 word 下标与移位（H15 / C9）。 */
static void l2p_bitmap_set(uint32_t *bitmap, uint32_t bit)
{
    bitmap[bit / 32U] |= (1UL << (bit & 31U));
}

static void l2p_bitmap_clear(uint32_t *bitmap, uint32_t bit)
{
    bitmap[bit / 32U] &= ~(1UL << (bit & 31U));
}

static bool l2p_bitmap_test(const uint32_t *bitmap, uint32_t bit)
{
    return (bitmap[bit / 32U] & (1UL << (bit & 31U))) != 0U;
}

/* --------------------------------------------------------------------
 * l2p_cache_owner
 *   功能：全局 region → owner FTL 实例（设备级注入的解析回调）。
 *   返回：owner 实例（恒非空；owner 表与 topology 的一致性由 ftl_contract_audit 在 boot 期证明）。
 *   实现要点：共享 cache 的所有 metadata I/O 都以 owner 的目录/分配器为准。
 * -------------------------------------------------------------------- */
static const l2p_owner_t *l2p_cache_owner(const l2p_cache_t *cache, uint32_t global_region)
{
    return cache->owner_of_region(cache->owner_ctx, global_region);
}

/* --------------------------------------------------------------------
 * l2p_cache_init
 *   功能：初始化共享 cache——清全局脏位图、逐行置 INVALID、复位 CLOCK/统计/在飞计数。
 *   参数：cache —— 待初始化对象；ch/ce —— 本 cache 服务的拓扑；
 *         owner_of_region/owner_ctx —— 全局 region → owner 的解析回调。
 *   返回：无。
 *   实现要点：**设备级只调一次**（main.c 在逐实例 boot 之前）。不能放进
 *             per-instance ftl_init——第二个实例会清掉第一个实例的 line。
 *             metadata 依赖（md_dir/alloc）不再存在 cache 里，由 owner 按需解析。
 * -------------------------------------------------------------------- */
void l2p_cache_init(l2p_cache_t *cache, uint8_t ch, uint8_t ce, l2p_owner_fn_t owner_of_region, void *owner_ctx)
{
    /* 前置条件（判据 2）：调用者传设备级存储 + 非空解析回调。 */
    ASSERT(cache != NULL);
    ASSERT(owner_of_region != NULL);

    cache->ch              = ch;
    cache->ce              = ce;
    cache->owner_of_region = owner_of_region;
    cache->owner_ctx       = owner_ctx;

    /* 全局 Region 级脏标记：开机全清（F4） */
    memset(cache->region_dirty_bitmap, 0, sizeof(cache->region_dirty_bitmap));

    for (uint16_t i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        l2p_cache_line_t *line = &cache->lines[i];

        /* 全部 entry 填 NAND_PBA_INVALID */
        memset(line->entries, 0xFF, sizeof(line->entries));
        line->region_id    = 0U;
        line->state        = L2P_CACHE_INVALID;
        line->access_count = 0U;
        line->fresh        = 0U;
    }

    cache->clock_hand = 0U;
    cache->hit_count  = 0U;
    cache->miss_count = 0U;

    /* 在飞计数归零：设备级 init 发生在任何命令之前 */
    cache->load_inflight  = 0U;
    cache->flush_inflight = 0U;

    /* Thrashing 检测窗口：开机从「非降级」起步 */
    cache->window_access = 0U;
    cache->window_miss   = 0U;
    cache->miss_pct      = 0U;
    cache->thrashing     = 0U;
}

/*
 * Thrashing 窗口结算。
 *
 * 每次 cache 访问（命中或未命中）调用一次；窗口满 L2P_THRASH_WINDOW 时按 miss 率
 * 与滞回阈值更新 thrashing 标志并清零窗口。仅状态**翻转**时打一行观测日志
 * （阈值附近不抖动 ⇒ 不会刷屏）。
 */
static void l2p_cache_note_access(l2p_cache_t *cache, uint8_t is_miss)
{
    cache->window_access++;
    if (is_miss != 0U)
    {
        cache->window_miss++;
    }

    if (cache->window_access < L2P_THRASH_WINDOW)
    {
        return;
    }

    cache->miss_pct = (uint8_t)((cache->window_miss * 100U) / cache->window_access);

    uint8_t was_thrashing = cache->thrashing;

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
             cache->hit_count,
             cache->miss_count);
    }
}

/*
 * 命中记账（read / write 共用）。
 *
 * CLOCK 的引用位“命中置 1”；若本行正是为当前这次访问刚加载完的（fresh），
 * 这一击是那次 miss 的重试 ⇒ 只消费 fresh，不计命中。
 */
static void l2p_cache_note_hit(l2p_cache_t *cache, l2p_cache_line_t *line)
{
    line->access_count = 1U;

    if (line->fresh != 0U)
    {
        line->fresh = 0U;
        return;
    }

    cache->hit_count++;
    l2p_cache_note_access(cache, 0U);
}

/*
 * 内部查找：在 cache 中查找 region_id 对应的 line。
 *
 * ⚠ **匹配四种状态**（CLEAN / DIRTY / FLUSHING / LOADING），只排除 INVALID：
 *   异步化后 FLUSHING / LOADING 的窗口是**真实存在**的（跨多轮），若把它们当未命中，
 *   就会为同一 region 再分配一行 ⇒ 同一 region 两行分叉（一行 CLEAN 一行 DIRTY），
 *   后续读按**下标序**取第一行 ⇒ 写进另一行 ⇒ 映射静默不一致。
 *   命中后各调用者再按 state 判"能不能读/写"（见 l2p_cache_read / write）。
 */
static l2p_cache_line_t *l2p_cache_find(l2p_cache_t *cache, uint32_t region_id)
{
    for (uint16_t i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        l2p_cache_line_t *line = &cache->lines[i];

        if ((line->region_id == region_id) && (line->state != L2P_CACHE_INVALID))
        {
            return line;
        }
    }

    return NULL;
}

bool l2p_cache_preload_region(l2p_cache_t *cache, uint32_t global_region, nand_pba_t pba)
{
    ASSERT(global_region < L2P_GLOBAL_REGIONS);
    ASSERT(pba_is_valid(pba));

    /* 已有驻留行（journal 回放的 DIRTY / 已预载）⇒ 维持「一 region 一行」。 */
    if (l2p_cache_find(cache, global_region) != NULL)
    {
        return true;
    }

    for (uint16_t i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        l2p_cache_line_t *line = &cache->lines[i];

        if (line->state != L2P_CACHE_INVALID)
        {
            continue;
        }

        /* 先写 region_id 再读：读失败时置 INVALID，find 会忽略该行，
         * 后续预载/懒加载可安全复用。 */
        line->region_id = global_region;

        if (ftl_sync_read_page(pba, line->entries, L2P_REGION_SIZE, FTL_PAGE_CFG_META()) == FTL_OK)
        {
            line->state = L2P_CACHE_CLEAN;
            return true;
        }

        line->state = L2P_CACHE_INVALID;
        return false;
    }

    return false; /* 无 INVALID 行（全 DIRTY/在飞）：交给懒加载 */
}

/*
 * 内部 CLOCK evict：从 clock_hand 开始扫描，选一个可驱逐的 line。
 *
 * CLOCK 规则：
 *   LOADING / FLUSHING → 跳过（有命令在飞，行内容正在被 NAND 改写）
 *   access_count > 0   → access_count--，检查下一个（第二次机会）
 *   INVALID            → 直接占用
 *   DIRTY              → **起异步写回**后跳过（本轮不可复用）
 *   access_count==0（CLEAN）→ 直接占用
 *
 * 返回被选中的 line；NULL = 当前没有可驱逐行（全在飞 / 脏行都刚起了写回）。
 *
 * ⚠ 本函数**不同步等待**写回：脏行只起 kick 就继续找别的候选。代价是
 *   "全部 64 行都脏"时本轮选不出 victim ⇒ 调用者拿到 FTL_AGAIN，下轮写回完成后即可。
 *   收敛性由 L2P_FLUSH_MAX_INFLIGHT 保证：在飞写回必有上限，完成后必有 CLEAN 行。
 */
static l2p_cache_line_t *l2p_cache_evict_one(l2p_cache_t *cache)
{
    uint16_t start = cache->clock_hand;

    for (uint16_t i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        uint16_t          idx  = (uint16_t)((start + i) % L2P_CACHE_SIZE);
        l2p_cache_line_t *line = &cache->lines[idx];

        if ((line->state == L2P_CACHE_FLUSHING) || (line->state == L2P_CACHE_LOADING))
        {
            continue; /* 有命令在飞：行内容正在被 NAND 改写，不可复用 */
        }

        if (line->state == L2P_CACHE_INVALID)
        {
            /* 空行，直接占用 */
            cache->clock_hand = (uint16_t)((idx + 1U) % L2P_CACHE_SIZE);
            return line;
        }

        if (line->access_count > 0U)
        {
            /* 第二次机会：减计数，检查下一个 */
            line->access_count--;
            continue;
        }

        /* access_count == 0 且不是 INVALID/在飞 → 选中 */
        cache->clock_hand = (uint16_t)((idx + 1U) % L2P_CACHE_SIZE);

        if (line->state == L2P_CACHE_DIRTY)
        {
            /*
             * 脏行：先起异步写回再复用（F4 关键前置）。
             *
             * ⚠ **绝不能**在此丢弃脏行：置 INVALID 会丢 RAM 脏数据，同时留下
             * 「bit 置位但无 resident DIRTY 行」的孤儿 bit——Checkpoint 按 bitmap
             * 假声明该区已落盘 ⇒ 重启后坏读。起 kick 后本行变 FLUSHING ⇒ 本轮跳过，
             * 继续找别的候选；kick 失败（背压/无依赖）⇒ 行仍 DIRTY，同样跳过。
             */
            (void)l2p_cache_flush_kick(cache, idx);
            continue;
        }

        /* 标记为 INVALID，等待调用者重新初始化 */
        line->state = L2P_CACHE_INVALID;
        cache->eviction_count++;
        return line;
    }

    return NULL; /* 无候选（全在飞 / 脏行都已起写回） */
}

/*
 * 内部：为 region_id 启动一次异步加载。
 *
 * 前提：调用者已确认该 region **不在** cache 中（l2p_cache_find 未命中）。
 * 返回：true = 行已就绪（未映射，同步完成）或读命令已提交；false = 未提交（无可驱逐行 /
 *       命令池空 / 提交背压）——调用者一律下轮重试，语义相同（"还没好"）。
 *
 * 位置一律来自 Metadata Directory（每个 region 在 fill_fixed 就登记好：从未写过 =
 * `NAND_PBA_INVALID`，刷过盘 = 日志块上的真实 PBA）。
 * 未映射 ⇒ 直接把行置为"全 INVALID 的 CLEAN 行"，**不提交 NAND 读**。
 * Random DISABLED / CRC ENABLED：Region 数据不加扰，但经 FC per-CW CRC + ECC 保护。
 */
static bool l2p_cache_load_start(l2p_cache_t *cache, uint32_t region_id)
{
    l2p_cache_line_t *line = l2p_cache_evict_one(cache);

    if (line == NULL)
    {
        return false;
    }

    const l2p_owner_t *owner        = l2p_cache_owner(cache, region_id);
    uint32_t           local_region = l2p_global_to_local_region(region_id);

    /* 占位：entries 先全 INVALID（加载失败时保持此态 = 全未映射） */
    memset(line->entries, 0xFF, sizeof(line->entries));
    /* 失败计数只在**换 region** 时清零：同一 region 的重试必须累计，否则
     * L2P_LOAD_RETRY_MAX 永远达不到（每次 load_start 都清零）。 */
    if (line->region_id != region_id)
    {
        line->load_fail_count = 0U;
    }
    line->region_id    = region_id;
    line->access_count = 1U;
    line->state        = L2P_CACHE_LOADING;
    line->fresh        = 0U; /* 完成时置 1（见 l2p_cache_line_t.fresh） */

    nand_pba_t region_addr = metadata_dir_lookup(owner->metadata_dir,
                                                 METADATA_TYPE_L2P_REGION,
                                                 (uint16_t)local_region);

    if (region_addr == NAND_PBA_INVALID)
    {
        /* 未映射：该 region 从未写过，Directory 里没有它的位置。entries 已是全
         * NAND_PBA_INVALID（上面 memset），直接置 CLEAN 并返回——**不提交读命令**。
         * 主流 DRAM-less 的未映射路径同样不产生 NAND I/O；读"某个固定擦除页"会把
         * 介质的空白内容当成映射来源，凭空构造未写过的表项。
         * fresh=1 与异步加载完成后的记账一致：下一次命中是本次 miss 的重试，不计 hit。 */
        line->state = L2P_CACHE_CLEAN;
        line->fresh = 1U;
        return true;
    }

    /* ⚠ 读的数据**留在 desc 链上**（与写不对称）⇒ 目标缓冲须活到完成、
     *   ftl_cmd_read_data 须早于 release —— 二者都在 ftl_l2p_callback 里，缓冲就是 line->entries
     *   （行在 LOADING 期间不可驱逐、不可写 ⇒ 地址稳定）。 */
    fc_command_t *cmd = NULL;
    if (ftl_async_read_page(&cmd, region_addr, FTL_PAGE_CFG_META()) != FTL_OK)
    {
        line->state = L2P_CACHE_INVALID; /* 回滚占位（区域未加载，等于没占用） */
        return false;
    }

    if (ftl_op_submit(cmd, FTL_OP_L2P, line) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        line->state = L2P_CACHE_INVALID;
        return false;
    }

    cache->load_inflight++;
    return true;
}

/* ── 5. 对外读写 ── */

ftl_status_t l2p_cache_read(l2p_cache_t *cache, lba_t lba, nand_pba_t *out)
{
    /* 前置条件（判据 2）：调用者传 ftl->cache 与自己的出参地址。 */
    ASSERT(out != NULL);

    if ((uint64_t)lba >= L2P_GLOBAL_LBA_MAX)
    {
        /* ⚠ **必须**回填 out：三态化后 FTL_ERROR 的调用者仍会读 *out（"读得 INVALID
         * 与未映射同义"是调用点的既有口径，如 ftl_trim / rr_select_plane）。
         * 不回填就是**未初始化变量**——旧单返回值实现此处返回 NAND_PBA_INVALID，
         * 语义必须保持。 */
        *out = NAND_PBA_INVALID;
        return FTL_ERROR;
    }

    /* LBA→(ch,ce) 路由由 ftl_contract_audit 在 boot 期对全部 region 起点证明；
     * 此处不再逐次复检（跨通道/跨 CE 误用是编程错误，boot 审计已覆盖）。 */

    uint32_t region_id = lba_to_global_region(lba);
    uint16_t entry_idx = lba_to_entry(lba);

    l2p_cache_line_t *line = l2p_cache_find(cache, region_id);
    if (line != NULL)
    {
        if (line->state == L2P_CACHE_LOADING)
        {
            return FTL_AGAIN; /* 尚未有效：本次给不出值 */
        }

        /* CLEAN / DIRTY / FLUSHING：entries 都是有效快照（FLUSHING 只是"正在落盘"） */
        l2p_cache_note_hit(cache, line);

        *out = line->entries[entry_idx];
        return FTL_OK;
    }

    /* miss：起异步加载，本轮给不出值 */
    cache->miss_count++;
    l2p_cache_note_access(cache, 1U);
    (void)l2p_cache_load_start(cache, region_id);
    return FTL_AGAIN;
}

bool l2p_cache_ready(l2p_cache_t *cache, lba_t lba)
{
    /* 前置条件（判据 2）：调用者传 ftl->cache。lba 越界是**数据**判断（判据 1），保留。 */
    if ((uint64_t)lba >= L2P_GLOBAL_LBA_MAX)
    {
        return true; /* 越界：调用者的 write 会是 no-op、read 会返 FTL_ERROR ⇒ 无需就绪 */
    }

    l2p_cache_line_t *line = l2p_cache_find(cache, lba_to_global_region(lba));
    if (line != NULL)
    {
        /* ⚠ FLUSHING 也算未就绪：写一行正在落盘的行会让"落盘的是旧快照、RAM 是新值"
         * 分叉（完成时无法区分该置 CLEAN 还是 DIRTY）⇒ 让调用者下轮再来。
         * **不记账、不消费 fresh、不动 access_count**——本函数是"就绪探针"，不是一次
         * 访问：真正的记账交给随后的 read/write（"一次 host 写 = 1 miss + 1 hit"，
         * 见 l2p_cache_line_t.fresh）。 */
        return (line->state == L2P_CACHE_DIRTY) || (line->state == L2P_CACHE_CLEAN);
    }

    /* miss：**本函数起了加载 ⇒ 记一次 miss**（这次访问的"发现未驻留"发生在此）。
     * 随后的重试命中会被 fresh 吃掉、不计 hit ⇒ 一次访问总账 = 1 miss。 */
    cache->miss_count++;
    l2p_cache_note_access(cache, 1U);
    (void)l2p_cache_load_start(cache, lba_to_global_region(lba));
    return false;
}

ftl_status_t l2p_cache_write(l2p_cache_t *cache, lba_t lba, nand_pba_t pba)
{
    /* 前置条件（判据 2）：调用者传 ftl->cache。lba 越界是**数据**判断（判据 1），保留。 */
    if ((uint64_t)lba >= L2P_GLOBAL_LBA_MAX)
    {
        return FTL_OK; /* 越界 no-op：数据通路的调用点都自带范围检查 */
    }

    uint32_t region_id = lba_to_global_region(lba);

    l2p_cache_line_t *line = l2p_cache_find(cache, region_id);
    if (line == NULL)
    {
        /* 未驻留：起加载，本轮不写（调用者应先 l2p_cache_ready 过一遍） */
        (void)l2p_cache_load_start(cache, region_id);
        return FTL_AGAIN;
    }

    if ((line->state == L2P_CACHE_LOADING) || (line->state == L2P_CACHE_FLUSHING))
    {
        return FTL_AGAIN; /* 有命令在飞：不可写（见文件头不变量①） */
    }

    /* ⚠ 必须喂 thrashing 窗口：写路径的一次访问 = l2p_cache_ready（未驻留时记 miss）
     *   + 本函数（记 hit）。只加 hit_count 不发访问事件会让随机写满盘的窗口 miss 率
     *   从 50% 虚升到 100% ⇒ 越过 L2P_THRASH_ENTER_PCT ⇒ 写密集下误触发降级。 */
    l2p_cache_note_hit(cache, line);

    line->entries[lba_to_entry(lba)] = pba;
    line->state                      = L2P_CACHE_DIRTY;

    /* Region 级脏标记（F4）：置本 region 的 bit。flush 落盘成功后清零。 */
    l2p_bitmap_set(cache->region_dirty_bitmap, region_id);
    return FTL_OK;
}

/* ── 5b. 同步逃生口 ──
 *
 * 自旋 ftl_service_pump（推 NAND + 抽干完成表）直到就绪。**只许结构性同步上下文**
 * 调用（开机恢复 / CLI / checkpoint / HIL `fl`）——服务函数里调它就是嵌套泵。
 * 断言把这条约定变成可测的。
 *
 * ⚠ 也必须**顺带推进本实例的元数据目录整理 job**：ftl_service_pump 是**实例无关**的
 *   （它只跑 NAND + 完成表），而 md job 的推进挂在 ftl_service 的各档位里。若这里不推，
 *   CLI `fl` 一旦撞上"池吃紧 ⇒ append_slot 让路"就会**死等**（自旋里没有任何人把
 *   compact 走完）。同步站点知道自己的 owner ⇒ 用 owner 的 metadata_dir/alloc
 *   推进该 die 的目录整理 job。 */
static void l2p_sync_pump(l2p_cache_t *cache, const l2p_owner_t *owner)
{
    ASSERT(!ftl_op_in_dispatch());

    ftl_service_pump();

    if ((cache != NULL) && (owner != NULL))
    {
        metadata_status_t md_status;
        metadata_handle(owner->metadata_dir, NULL, &md_status);
    }
}

ftl_status_t l2p_cache_read_sync(l2p_cache_t *cache, lba_t lba, nand_pba_t *out)
{
    uint32_t           guard = L2P_SYNC_GUARD_MAX;
    const l2p_owner_t *owner = NULL;

    if ((uint64_t)lba < L2P_GLOBAL_LBA_MAX)
    {
        owner = l2p_cache_owner(cache, lba_to_global_region(lba));
    }

    while (guard-- > 0U)
    {
        const ftl_status_t st = l2p_cache_read(cache, lba, out);

        if (st != FTL_AGAIN)
        {
            return st;
        }
        l2p_sync_pump(cache, owner);
    }

    LOGE("[L2P] read_sync timeout lba=%lu\r\n", lba);
    return FTL_ERROR;
}

ftl_status_t l2p_cache_write_sync(l2p_cache_t *cache, lba_t lba, nand_pba_t pba)
{
    uint32_t           guard = L2P_SYNC_GUARD_MAX;
    const l2p_owner_t *owner = NULL;

    if ((uint64_t)lba < L2P_GLOBAL_LBA_MAX)
    {
        owner = l2p_cache_owner(cache, lba_to_global_region(lba));
    }

    while (guard-- > 0U)
    {
        if (l2p_cache_ready(cache, lba))
        {
            return l2p_cache_write(cache, lba, pba);
        }
        l2p_sync_pump(cache, owner);
    }

    LOGE("[L2P] write_sync timeout lba=%lu\r\n", lba);
    return FTL_ERROR;
}

/* ── 6. 写回 ── */

/*
 * 写回资源预检。
 *
 * 一次 region 页写需要 1 条命令 + CW_NUM 个描述符 + CW_NUM 个 buffer，且必须能进
 * SQ/HSQ。这些资源与 metadata 日志槽位来自**不同的池**：不预检就 append_slot，
 * 一旦 FC 池空（FTL_AGAIN），槽位已推进且无法回收 ⇒ 持续背压下白烧日志块。
 * 单线程 superloop 且预检与建链之间不泵 ⇒ 资源不会被别人拿走。
 */
static bool l2p_flush_resources_available(void)
{
    fc_resource_stats_t stats;

    if (fc_get_resource_stats(&stats) != FC_STATUS_OK)
    {
        return false;
    }

    return (stats.command_used < FC_COMMAND_POOL_DEPTH) &&
           ((uint32_t)stats.descriptor_used + CW_NUM <= FC_DESCRIPTOR_POOL_DEPTH) &&
           ((uint32_t)stats.buffer_used + CW_NUM <= FC_BUFFER_POOL_DEPTH) &&
           (stats.sq_used < FC_SQ_DEPTH) && (stats.hsq_used < FC_HSQ_DEPTH);
}

/* --------------------------------------------------------------------
 * l2p_cache_flush_kick
 *   功能：把一行脏 region 的**异步**写回起起来（提交即返回，不等完成）。
 *   参数：cache —— 缓存；line_idx —— 行下标。
 *   返回：FTL_OK 已提交（含"本行非 DIRTY 无需写回"）；FTL_AGAIN 背压（命令池空 /
 *         在飞写回达上限 / 提交被 SQ 拒 / **取不到槽**）；FTL_ERROR 硬失败（参数非法 /
 *         无依赖 / 建链失败）。
 *   实现要点：
 *     · 提交成功才置 FLUSHING——失败必须让行留在 DIRTY（下轮重试），否则脏数据永远
 *       不会被写回（行状态说"在落盘"，实际没有命令在飞）。
 *     · 追加写绝不就地覆盖；**先写后记账**（顺序不可倒）：掉电最多多留一份旧记录，
 *       不会丢映射。
 *     · **取不到槽 ⇒ FTL_AGAIN**：
 *       metadata_dir_append_slot 返回 NAND_PBA_INVALID 有三种来源——
 *       ① 全部 plane 的 Free Pool 都空；② 目录整理 job 在飞（它要独占当前块的空槽）；
 *       ③ 刚换新块且 Free Pool 吃紧，本函数只登记压缩请求、本次不发放槽位。
 *       三者都是**瞬态**（GC 会补池 / job 会走完），报 FTL_ERROR 会被调用者当硬失败
 *       （`l2p_flush_kick_owner` 跳过该行、`cp_flush_regions` 直接**中止 Checkpoint**）。
 *       背压让它们下轮重试即可。**绝不回退任何固定位置**（本仓没有固定的 region
 *       位置；把"未映射"写成"有映射"会让下次再写同一页即损坏）。
 *     · ⚠ 槽位在 append_slot 里就消耗掉了：若其后建链/提交失败，该槽作废（写指针
 *       已推进），但信息不丢——region 仍指向上一份有效记录。
 * -------------------------------------------------------------------- */
ftl_status_t l2p_cache_flush_kick(l2p_cache_t *cache, int line_idx)
{
    /* 前置条件（判据 2）：cache 恒 ftl->cache；line_idx 由调用者自持（下标越界是内部
     * 自铸句柄，非外部输入）——两侧都收口成断言。 */
    ASSERT((line_idx >= 0) && (line_idx < (int)L2P_CACHE_SIZE));

    l2p_cache_line_t *line = &cache->lines[line_idx];

    if (line->state != L2P_CACHE_DIRTY)
    {
        return FTL_OK; /* 不需要写回（含"已在飞"/"已干净"） */
    }

    /* 映射页属于哪个 owner die 由全局 region 解析；写回必须落到 owner 的目录/分配器。 */
    const l2p_owner_t *owner = l2p_cache_owner(cache, line->region_id);

    if (cache->flush_inflight >= L2P_FLUSH_MAX_INFLIGHT)
    {
        return FTL_AGAIN; /* 共享 credit 池满：让 host I/O 先拿 FC 资源 */
    }

    /* 先确认 FC 侧拿得到命令/描述符/buffer/队列槽位，再消耗 metadata 槽位。 */
    if (!l2p_flush_resources_available())
    {
        return FTL_AGAIN;
    }

    nand_pba_t region_addr = metadata_dir_append_slot(owner->metadata_dir, owner->alloc);
    if (region_addr == NAND_PBA_INVALID)
    {
        /* 瞬态：Free Pool 空（等 GC 补池）或目录整理 job 在飞（等它走完）⇒ 背压。
         * ⚠ 不能报 FTL_ERROR：l2p_flush_kick_owner 会跳过本行、cp_flush_regions 会**中止
         *   Checkpoint**——一次正常的后台压缩不该把 CP 打回去。 */
        return FTL_AGAIN;
    }

    /* 建链 + 把 entries 拷进 FC buffer（写的数据在 prepare 时就 memcpy 走了 ⇒
     * 提交后 line->entries 可自由改写，这正是"可读"而不"可写"的由来）。 */
    fc_command_t      *cmd          = NULL;
    const ftl_status_t build_status = ftl_async_write_page(&cmd,
                                                           region_addr,
                                                           line->entries,
                                                           L2P_REGION_SIZE,
                                                           FTL_PAGE_CFG_META());
    if (build_status != FTL_OK)
    {
        /* 命令池空是瞬态背压（FTL_AGAIN）；只有地址/建链的硬失败才是 FTL_ERROR。
         * 混淆二者会让 cp_flush_regions 中止 Checkpoint（H38）。 */
        return build_status;
    }

    line->state = L2P_CACHE_FLUSHING;

    if (ftl_op_submit(cmd, FTL_OP_L2P, line) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        line->state = L2P_CACHE_DIRTY; /* 回滚：行仍脏，下轮重试 */
        return FTL_AGAIN;
    }

    cache->flush_inflight++;
    return FTL_OK;
}

void l2p_flush_kick_owner(l2p_cache_t *cache, uint32_t owner_dim, uint8_t max_kick)
{
    /* 前置条件（判据 2）：调用者传 ftl->cache、本实例 dim 与档位表里的非零条数
     * （IDLE L2P_CACHE_SIZE / NORMAL BG_NORMAL_FLUSH_COUNT / BUSY BG_BUSY_FLUSH_COUNT）。 */
    ASSERT(owner_dim < FTL_DIM_TOTAL);
    ASSERT(max_kick != 0U);

    /* 心跳（见 ftl_job.h）：有在飞 region（load / flush）时每满 1s 打一行计数。
     * ⚠ 本模块没有"单 job"结构（在飞的是若干 cache line）⇒ 只报计数，不报 phase/age。 */
    if ((cache->flush_inflight == 0U) && (cache->load_inflight == 0U))
    {
        cache->job_log_ms = 0U; /* 无在飞：清基准 */
    }
    else if (ftl_job_tick(&cache->job_log_ms))
    {
        LOGI("[L2P] job inflight load=%u flush=%u\r\n",
             (unsigned)cache->load_inflight,
             (unsigned)cache->flush_inflight);
    }

    for (uint16_t i = 0U; (i < L2P_CACHE_SIZE) && (max_kick > 0U); i++)
    {
        const l2p_cache_line_t *line = &cache->lines[i];

        /* 只刷自己的行：共享 cache 里别的 LUN 的行由它们自己的 CP/flush 处理。 */
        if ((line->state != L2P_CACHE_DIRTY) || !l2p_region_owned_by(line->region_id, owner_dim))
        {
            continue;
        }

        const ftl_status_t st = l2p_cache_flush_kick(cache, i);

        if (st == FTL_AGAIN)
        {
            return; /* 闸门满 / 池空：本轮到此为止，下轮再来 */
        }
        if (st == FTL_OK)
        {
            max_kick--;
        }
        /* FTL_ERROR：本行无法写回（无依赖 / 无槽）——跳过它，试下一行 */
    }
}

/* --------------------------------------------------------------------
 * l2p_cache_flush_one_sync
 *   功能：同步写回一行——起 kick 后自旋到该行不再在飞。
 *   参数：cache / line_idx。
 *   返回：FTL_OK（行已 CLEAN，或本就非 DIRTY）；FTL_ERROR（写失败 / 加载中的行 / 超时）。
 *   实现要点：
 *     · 入口若已是 FLUSHING（后台已起），**等它**而不是重复起——否则同一行两条写命令。
 *     · 起过一次后又回到 DIRTY ⇒ 那条写失败了（ftl_l2p_callback 的失败分支置回 DIRTY），
 *       直接报错，不再重试——同步站点要的是"已落盘"，不是无限重试。
 * -------------------------------------------------------------------- */
ftl_status_t l2p_cache_flush_one_sync(l2p_cache_t *cache, int line_idx)
{
    uint32_t guard  = L2P_SYNC_GUARD_MAX;
    uint8_t  kicked = 0U;

    /* 前置条件（判据 2）：cache 恒 ftl->cache；line_idx 由调用者自持。 */
    ASSERT((line_idx >= 0) && (line_idx < (int)L2P_CACHE_SIZE));

    l2p_cache_line_t *line = &cache->lines[line_idx];

    if ((line->state == L2P_CACHE_CLEAN) || (line->state == L2P_CACHE_INVALID))
    {
        return FTL_OK;
    }
    if (line->state == L2P_CACHE_LOADING)
    {
        return FTL_ERROR; /* 加载中的行不能写回（entries 尚未有效） */
    }

    /* 本行的映射页属于哪个 owner die；同步泵要推进该 owner 的目录整理 job。 */
    const l2p_owner_t *owner = l2p_cache_owner(cache, line->region_id);

    while (guard-- > 0U)
    {
        if ((line->state == L2P_CACHE_CLEAN) || (line->state == L2P_CACHE_INVALID))
        {
            return FTL_OK;
        }
        if (line->state == L2P_CACHE_LOADING)
        {
            return FTL_ERROR; /* 加载中的行不能写回（entries 尚未有效） */
        }

        if (line->state == L2P_CACHE_DIRTY)
        {
            const ftl_status_t st = l2p_cache_flush_kick(cache, line_idx);

            if (st == FTL_ERROR)
            {
                return FTL_ERROR;
            }
            if (st == FTL_OK)
            {
                if (kicked != 0U)
                {
                    return FTL_ERROR; /* 起过又回 DIRTY ⇒ 那条写失败 */
                }
                kicked = 1U;
            }
            /* FTL_AGAIN：背压，泵一轮再试 */
        }

        l2p_sync_pump(cache, owner);
    }

    return FTL_ERROR; /* 超时：行一直没能落盘 */
}

ftl_status_t l2p_cache_flush_owner_sync(l2p_cache_t *cache, uint32_t owner_dim)
{
    /* 前置条件（判据 2）：调用者传 ftl->cache 与本实例 dim。 */
    ASSERT(owner_dim < FTL_DIM_TOTAL);

    for (uint16_t i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        const l2p_cache_line_t *line = &cache->lines[i];

        /* 只处理本 owner 的行；别的 LUN 的行由它们自己的 CP/flush 负责。 */
        if (!l2p_region_owned_by(line->region_id, owner_dim))
        {
            continue;
        }

        /* ⚠ 也等 **FLUSHING** 行（后台 kick 已把它起起来了）：只处理 DIRTY 会让
         *   "脏数据已提交、尚未落盘"的行被跳过 ⇒ `fl` 返回时映射其实没落盘，
         *   与它作为显式持久化请求的语义不符。flush_one_sync 对 FLUSHING 是**等**
         *   而非重复起（见其实现），故不会发出第二条写命令。
         *   LOADING 行不在此列：它没有脏数据可写（entries 尚未有效）。 */
        if ((line->state == L2P_CACHE_DIRTY) || (line->state == L2P_CACHE_FLUSHING))
        {
            if (l2p_cache_flush_one_sync(cache, i) != FTL_OK)
            {
                return FTL_ERROR;
            }
        }
    }

    return FTL_OK;
}

/* ── 7. 完成服务（g_op_callbacks[FTL_OP_L2P]）── */

/* --------------------------------------------------------------------
 * ftl_l2p_callback
 *   功能：L2P cache 命令（加载 / 写回）的完成服务函数——按 line->state 收尾。
 *   参数：ftl —— 命令归属实例（ftl_op_dispatch 按命令自身 ch/ce/lun 解析后传入）；
 *         completion —— FC 完成状态；ctx —— `l2p_cache_line_t *`（提交时传的行指针）。
 *   返回：无。命令的 release 归本函数（见 ftl_op.h 的所有权表）。
 *   实现要点：
 *     · LOADING ⇒ 成功：ftl_cmd_read_data 把整页 region 拷进 line->entries（**必须早于
 *       release**——读的数据在 desc 链上），置 CLEAN。失败：**同样置 CLEAN，entries
 *       保持全 INVALID**（= 该 region 全部未映射）——避免每次访问都重试坏 region，
 *       把 host 读永久卡在 FTL_AGAIN（evict→加载→失败→再 evict 的活锁）。
 *     · FLUSHING ⇒ 成功：metadata_dir_commit_region 登记新位置（旧位置活计数 -1）、
 *       置 CLEAN、清 region 脏位。失败：置回 DIRTY（保留重试；槽位已消耗不可复用，
 *       Directory 不登记 ⇒ 该 region 仍指向上一份有效记录，无信息丢失）。
 *     · 其余状态 ⇒ 编程错误（LOADING/FLUSHING 期间行不可被改写状态），debug 断言。
 * -------------------------------------------------------------------- */
void ftl_l2p_callback(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx)
{
    l2p_cache_line_t *line = (l2p_cache_line_t *)ctx;

    ASSERT(completion != NULL);
    ASSERT(line != NULL);

    fc_command_t *cmd = fc_command_id_to_command((uint16_t)completion->command_id);

    /* ftl 恒非空（路由表早于逐实例 boot 设好，见 ftl_op.h）；NULL = 编程错误 ⇒
     * ASSERT 而非 if-guard：release 下解引用留现场，比静默吞完成可定位。 */

    ASSERT(&ftl->cache->lines[0] <= line);
    ASSERT(line < &ftl->cache->lines[L2P_CACHE_SIZE]);

    /* 命令按 PBA 的 ch/ce/lun 路由回 owner；line 的全局 region 必须属于同一个 owner。 */
    ASSERT(ftl_topo_dim(ftl->topo) == (line->region_id % FTL_DIM_TOTAL));
    ASSERT(l2p_cache_owner(ftl->cache, line->region_id)->dim == ftl_topo_dim(ftl->topo));

    /* 完成只可能属于一次在飞的 load/flush；行在这期间不可驱逐、不可改写状态。 */
    ASSERT((line->state == L2P_CACHE_LOADING) || (line->state == L2P_CACHE_FLUSHING));

    const uint8_t failed = fc_cw_status_has_error(completion->status) ? 1U : 0U;

    if (line->state == L2P_CACHE_LOADING)
    {
        uint8_t load_failed = failed;

        if (load_failed == 0U)
        {
            /* 数据在 desc 链上 ⇒ 必须早于 release 拷出。 */
            if (ftl_cmd_read_data(cmd, line->entries, L2P_REGION_SIZE) != FTL_OK)
            {
                LOGW("[L2P] load copy failed region=%lu\r\n", line->region_id);
                load_failed = 1U;
            }
        }

        if (load_failed == 0U)
        {
            line->load_fail_count = 0U;
            line->state           = L2P_CACHE_CLEAN;
            line->fresh           = 1U; /* 下一次命中是本次访问的重试，不计 hit */
        }
        else
        {
            /* 失败：先有限次重试（置 INVALID 让下次访问重新加载），超过上限才降级为
             * 「全未映射的 CLEAN 行」。两个极端都要避免：
             *   · 直接 CLEAN ⇒ 一次瞬态读错被永久当成“从未写过”，映射静默丢失；
             *   · 一直 INVALID ⇒ 每次访问都重读坏 region，host 永久停在 FTL_AGAIN。 */
            memset(line->entries, 0xFF, sizeof(line->entries));
            line->load_fail_count++;

            if (line->load_fail_count < L2P_LOAD_RETRY_MAX)
            {
                line->state = L2P_CACHE_INVALID;
            }
            else
            {
                line->state = L2P_CACHE_CLEAN;
                line->fresh = 1U;
                LOGE("[L2P] region %lu load failed %u times — degrade to unmapped\r\n",
                     line->region_id,
                     (unsigned)line->load_fail_count);
            }
        }

        ASSERT(ftl->cache->load_inflight > 0U);
        ftl->cache->load_inflight--;
    }
    else if (line->state == L2P_CACHE_FLUSHING)
    {
        if (failed == 0U)
        {
            /* 落盘成功才登记新位置（顺序不可倒：先写后记账，掉电最多多留一份旧记录）。
             * 槽位地址取自命令自身（ftl_cmd_pba 是 pba_to_cmd 的逆）——无需在行里存。
             * ⚠ 目录用 owner die 的局部 region 编号；cache 的键是全局 region。 */
            const uint16_t   local_region = (uint16_t)l2p_global_to_local_region(line->region_id);
            const nand_pba_t pba          = ftl_cmd_pba(cmd);

            metadata_dir_commit_region(ftl->metadata_dir, local_region, pba);

            line->state = L2P_CACHE_CLEAN;

            /* 落盘成功：本 region 的**全局**脏位清零（下次 checkpoint 不再重写） */
            l2p_bitmap_clear(ftl->cache->region_dirty_bitmap, line->region_id);
        }
        else
        {
            line->state = L2P_CACHE_DIRTY; /* 保留重试（槽位已废，Directory 未登记） */
        }

        ASSERT(ftl->cache->flush_inflight > 0U);
        ftl->cache->flush_inflight--;
    }
    else
    {
        /* 不该发生：LOADING / FLUSHING 期间行不可被改写状态。 */
        LOGE("[L2P] unexpected completion state=%u\r\n", (unsigned)line->state);
    }

    (void)fc_command_release(cmd);
}

/* ── 8. 恢复辅助 ── */

/*
 * l2p_cache_invalidate_owner - 只失效某个 owner 的行与脏位（恢复/格式化用）
 *
 * 共享 cache 下不能按实例重新 init：那样会抹掉别的 LUN 刚回放的 DIRTY 行和
 * 全局脏位。本函数只动本 owner 的 resident 行，并清本 owner 的全局脏位。
 * 恢复期不应有在飞命令；FLUSHING/LOADING 行重写会泄漏在飞计数 ⇒ ASSERT。
 */
void l2p_cache_invalidate_owner(l2p_cache_t *cache, uint32_t owner_dim)
{
    ASSERT(owner_dim < FTL_DIM_TOTAL);

    for (uint16_t i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        l2p_cache_line_t *line = &cache->lines[i];

        if (line->state == L2P_CACHE_INVALID)
        {
            continue; /* 空行：region_id 无语义，无需处理 */
        }
        if (!l2p_region_owned_by(line->region_id, owner_dim))
        {
            continue; /* 别的 LUN 的行，不能碰 */
        }

        ASSERT((line->state == L2P_CACHE_CLEAN) || (line->state == L2P_CACHE_DIRTY));

        memset(line->entries, 0xFF, sizeof(line->entries));
        line->region_id    = 0U;
        line->state        = L2P_CACHE_INVALID;
        line->access_count = 0U;
        line->fresh        = 0U;
    }

    /* 清本 owner 的全局脏位：local 0..L2P_REGIONS_PER_INSTANCE-1
     * → global = local * DIM + owner_dim。 */
    for (uint32_t local = 0U; local < L2P_REGIONS_PER_INSTANCE; local++)
    {
        const uint32_t global_region = (local * FTL_DIM_TOTAL) + owner_dim;

        l2p_bitmap_clear(cache->region_dirty_bitmap, global_region);
    }
}

/*
 * l2p_cache_snapshot_owner_bitmap - 全局脏位图 → 本 die 的局部 2048-bit 快照
 *
 * Checkpoint 起点调用（先快照再 flush）。DIM=1 时等价于整份位图拷贝。
 */
void l2p_cache_snapshot_owner_bitmap(const l2p_cache_t *cache, uint32_t owner_dim, uint32_t out[L2P_LOCAL_DIRTY_WORDS])
{
    ASSERT(owner_dim < FTL_DIM_TOTAL);
    ASSERT(out != NULL);

    memset(out, 0, L2P_LOCAL_DIRTY_WORDS * sizeof(uint32_t));

    for (uint32_t local = 0U; local < L2P_REGIONS_PER_INSTANCE; local++)
    {
        const uint32_t global = (local * FTL_DIM_TOTAL) + owner_dim;

        if (l2p_bitmap_test(cache->region_dirty_bitmap, global))
        {
            l2p_bitmap_set(out, local);
        }
    }
}

/* ── typed handle（docs §4.7） ── */

/* 档位 → 每轮主动 flush 条数上界（模块内 static const 策略表）。 */
static const uint8_t s_l2p_flush_max[BG_LEVEL_NUM] = {
    [BG_LEVEL_IDLE]   = L2P_CACHE_SIZE,
    [BG_LEVEL_NORMAL] = L2P_FLUSH_MAX_NORMAL,
    [BG_LEVEL_BUSY]   = L2P_FLUSH_MAX_BUSY,
};

static uint16_t l2p_dirty_count(const l2p_cache_t *cache)
{
    uint16_t count = 0U;

    for (uint32_t i = 0U; i < L2P_DIRTY_BITMAP_WORDS; i++)
    {
        count = (uint16_t)(count + (uint16_t)__builtin_popcount(cache->region_dirty_bitmap[i]));
    }
    return count;
}

static void l2p_status_fill(const l2p_cache_t *cache, l2p_status_t *out)
{
    ASSERT(out != NULL);

    uint16_t resident = 0U;

    for (uint16_t i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        if (cache->lines[i].state != L2P_CACHE_INVALID)
        {
            resident++;
        }
    }

    out->base.critical  = false; /* 后台写回属预算任务；同步逃生口自管 deadline */
    out->base.wants_run = (cache->flush_inflight != 0U) || (l2p_dirty_count(cache) != 0U);
    out->base.progress  = cache->hit_count + cache->miss_count;
    out->base.next_ms   = 0U;
    out->base.error     = cache->error;

    if (cache->flush_inflight != 0U)
    {
        out->base.state = FTL_MODULE_WAITING;
    }
    else if (out->base.wants_run)
    {
        out->base.state = FTL_MODULE_RUNNING;
    }
    else
    {
        out->base.state = FTL_MODULE_IDLE;
    }

    out->load_inflight  = cache->load_inflight;
    out->flush_inflight = cache->flush_inflight;
    out->dirty_regions  = l2p_dirty_count(cache);
    out->resident_lines = resident;
    out->thrashing      = (cache->thrashing != 0U);
    out->miss_pct       = cache->miss_pct;
    out->evictions      = cache->eviction_count;
}

void l2p_handle(l2p_cache_t *cache, uint32_t dim, const ftl_bg_info_t *info, l2p_status_t *out)
{
    ASSERT(dim < FTL_DIM_TOTAL);
    ASSERT(info != NULL);
    ASSERT(out != NULL);
    ASSERT(info->load_level < BG_LEVEL_NUM);

    /* thrashing 降级：主动 flush 大概率白写且挤前台带宽 ⇒ 停主动 flush。
     * 判据在模块内，主流程不读 cache->thrashing。 */
    if (cache->thrashing == 0U)
    {
        const uint8_t max_kick = s_l2p_flush_max[info->load_level];

        l2p_flush_kick_owner(cache, dim, max_kick);
    }

    l2p_status_fill(cache, out);
}

void l2p_cache_line_get(const l2p_cache_t *cache, uint16_t idx, l2p_line_info_t *out)
{
    ASSERT(out != NULL);

    if (idx >= L2P_CACHE_SIZE)
    {
        out->region_id = 0U;
        out->state     = (uint8_t)L2P_CACHE_INVALID;
        return;
    }

    out->region_id = cache->lines[idx].region_id;
    out->state     = cache->lines[idx].state;
}

void l2p_cache_invalidate_line(l2p_cache_t *cache, uint16_t idx)
{
    ASSERT(idx < L2P_CACHE_SIZE);
    ASSERT(cache->lines[idx].state != L2P_CACHE_FLUSHING);
    ASSERT(cache->lines[idx].state != L2P_CACHE_LOADING);

    cache->lines[idx].state = L2P_CACHE_INVALID;
}

bool l2p_cache_has_flush_inflight(const l2p_cache_t *cache)
{
    return cache->flush_inflight != 0U;
}

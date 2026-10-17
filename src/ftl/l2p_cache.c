/*
 * l2p_cache.c — 共享 L2P Region 缓存（CLOCK + 异步加载/写回 + thrashing 检测）
 *
 * 职责：
 *   把 L2P 映射表按 region（L2P_ENTRIES_PER_REGION 条 entry/region）缓存进 SRAM，
 *   提供全局 LBA → PBA 的读/写；未命中时经 **owner 实例** 的 Metadata Directory
 *   定位并从 NAND 加载；脏行写回走 owner 的「追加写」日志块。
 *
 * 共享模型（阶段 B）：
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
 *   g_op_service[FTL_OP_L2P]（本文件末尾的 ftl_op_l2p）按 line->state 收尾。
 *   ⇒ 对外 API 三态；结构性同步上下文走 `*_sync` 逃生口（自旋泵）。
 *
 * 文件结构：
 *   1. 初始化         l2p_cache_init / owner 解析
 *   2. Thrashing 检测 l2p_cache_note_access（static）
 *   3. 查找/驱逐      l2p_cache_find / evict_one（static）
 *   4. 加载           l2p_cache_load_start（static）
 *   5. 对外读写       l2p_cache_read / ready / write + *_sync 逃生口
 *   6. 写回           l2p_cache_flush_kick / l2p_flush_kick_owner / flush_*_sync
 *   7. 完成服务       ftl_op_l2p（g_op_service[FTL_OP_L2P]）
 *   8. 恢复辅助       invalidate_owner / snapshot_owner_bitmap
 */

#include "ftl/l2p_cache.h"

#include "common/assertion.h"
#include "common/log.h"
#include "fc/fc.h"
#include "ftl/ftl.h" /* ftl_context_t / ftl_service_pump（同步逃生口用） */
#include "ftl/ftl_fc_interface.h"
#include "ftl/ftl_job.h" /* 心跳（[JOB] 观测口）*/
#include "ftl/ftl_op.h"  /* ftl_op_submit / ftl_op_in_dispatch */
#include "nand/onfi_1_0.h"

#include <string.h>

/* --------------------------------------------------------------------
 * l2p_cache_owner
 *   功能：全局 region → owner FTL 实例（设备级注入的解析回调）。
 *   返回：owner 实例（恒非空；解析失败/拓扑不匹配是内部不变量破坏 ⇒ ASSERT）。
 *   实现要点：共享 cache 的所有 metadata I/O 都以 owner 的目录/分配器为准。
 * -------------------------------------------------------------------- */
static struct ftl_context *l2p_cache_owner(const l2p_cache_t *cache, uint32_t global_region)
{
    ASSERT(cache != NULL);
    ASSERT(cache->owner_of_region != NULL);

    struct ftl_context *owner = cache->owner_of_region(cache->owner_ctx, global_region);

    ASSERT(owner != NULL);
    ASSERT(owner->cache == cache);
    ASSERT(owner->topo.ch == cache->ch);
    ASSERT(owner->topo.ce == cache->ce);
    ASSERT(ftl_topo_dim(owner->topo) == (global_region % FTL_DIM_TOTAL));

    return owner;
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
             (unsigned long)cache->hit_count,
             (unsigned long)cache->miss_count);
    }
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
 * 返回 line 索引（-1 表示当前没有可驱逐行——全在飞 / 脏行都刚起了写回）。
 *
 * ⚠ 异步化后本函数**不再同步等待**写回：脏行只起 kick 就继续找别的候选。代价是
 *   "全部 64 行都脏"时本轮选不出 victim ⇒ 调用者拿到 FTL_AGAIN，下轮写回完成后即可。
 *   收敛性由 L2P_FLUSH_MAX_INFLIGHT 保证：在飞写回必有上限，完成后必有 CLEAN 行。
 */
static int l2p_cache_evict_one(l2p_cache_t *cache)
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
            return (int)idx;
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
             * ⚠ **绝不能**在此丢弃脏行（旧实现直接置 INVALID 会丢 RAM 脏数据，留下
             * "bit 置位但无 resident DIRTY 行"的孤儿 bit，checkpoint 会按 bitmap 假声明
             * 该区已落盘 → boot 必坏读）。起 kick 后本行变 FLUSHING ⇒ 本轮跳过它，
             * 继续找别的候选；kick 失败（背压/无依赖）⇒ 行仍 DIRTY，同样跳过。
             */
            (void)l2p_cache_flush_kick(cache, (int)idx);
            continue;
        }

        /* 标记为 INVALID，等待调用者重新初始化 */
        line->state = L2P_CACHE_INVALID;
        return (int)idx;
    }

    return FTL_ERROR; /* 无候选（全在飞 / 脏行都已起写回） */
}

/*
 * 内部：为 region_id 启动一次异步加载。
 *
 * 前提：调用者已确认该 region **不在** cache 中（l2p_cache_find 未命中）。
 * 返回：true 已提交；false 未提交（无可驱逐行 / 命令池空 / 提交背压）——两者都让调用者
 *       下轮重试，语义相同（"还没好"）。
 *
 * 位置一律来自 Metadata Directory（每个 region 在 fill_fixed 就登记好：初始指向
 * **哨兵层**固定页，被刷新后指向日志块）。哨兵页永不写入 ⇒ 读回全 0xFF ⇒
 * 逐项即 PBA_INVALID = "未映射"，正是首次访问应有的语义。
 * 下面的固定位置回退仅作 owner 的 metadata_dir 查找失败时的兜底（正常路径不可达）。
 * Random DISABLED / CRC ENABLED（v0.62）：Region 数据不加扰，但经 FC per-CW CRC + ECC 保护。
 */
static bool l2p_cache_load_start(l2p_cache_t *cache, uint32_t region_id)
{
    int victim_idx = l2p_cache_evict_one(cache);

    if (victim_idx < 0)
    {
        return false;
    }

    struct ftl_context *owner        = l2p_cache_owner(cache, region_id);
    uint32_t            local_region = l2p_global_to_local_region(region_id);

    l2p_cache_line_t *line = &cache->lines[victim_idx];

    /* 占位：entries 先全 INVALID（加载失败时保持此态 = 全未映射） */
    memset(line->entries, 0xFF, sizeof(line->entries));
    line->region_id    = region_id;
    line->access_count = 1U;
    line->state        = L2P_CACHE_LOADING;
    line->fresh        = 0U; /* 完成时置 1（见 l2p_cache_line_t.fresh） */

    nand_pba_t region_addr = metadata_dir_lookup(owner->metadata_dir,
                                                 METADATA_TYPE_L2P_REGION,
                                                 (uint16_t)local_region);

    if (region_addr == NAND_PBA_INVALID)
    {
        /* 回退固定位置（owner die 的局部 region 编号） */
        region_addr = l2p_region_pba(owner->topo.ch, owner->topo.ce, owner->topo.lun, (uint16_t)local_region);
    }

    /* ⚠ 读的数据**留在 desc 链上**（与写不对称）⇒ 目标缓冲须活到完成、
     *   ftl_cmd_read_data 须早于 release —— 二者都在 ftl_op_l2p 里，缓冲就是 line->entries
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
    ASSERT(cache != NULL);
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

    /* 全局 LBA 必须属于本 cache 服务的 (ch,ce)；跨通道/跨 CE 误用是编程错误。 */
    ASSERT(lba_route_ch(lba) == cache->ch);
    ASSERT(lba_route_ce(lba) == cache->ce);

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
        line->access_count++;

        /* ⚠ 若本行是**为当前这次访问**刚加载完的（fresh），这一击就是那次 miss 的重试
         * ——不计命中（否则一次访问记成 miss + hit，窗口 miss 率虚降一半，见
         * l2p_cache_line_t.fresh）。 */
        if (line->fresh != 0U)
        {
            line->fresh = 0U;
        }
        else
        {
            cache->hit_count++;
            l2p_cache_note_access(cache, 0U);
        }

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
    ASSERT(cache != NULL);
    if ((uint64_t)lba >= L2P_GLOBAL_LBA_MAX)
    {
        return true; /* 越界：调用者的 write 会是 no-op、read 会返 FTL_ERROR ⇒ 无需就绪 */
    }

    ASSERT(lba_route_ch(lba) == cache->ch);
    ASSERT(lba_route_ce(lba) == cache->ce);

    l2p_cache_line_t *line = l2p_cache_find(cache, lba_to_global_region(lba));
    if (line != NULL)
    {
        /* ⚠ FLUSHING 也算未就绪：写一行正在落盘的行会让"落盘的是旧快照、RAM 是新值"
         * 分叉（完成时无法区分该置 CLEAN 还是 DIRTY）⇒ 让调用者下轮再来。
         * **不记账、不消费 fresh、不动 access_count**——本函数是"就绪探针"，不是一次
         * 访问：真正的记账交给随后的 read/write（那样"一次 host 写 = 1 miss + 1 hit"
         * 与旧同步实现逐字一致，见 l2p_cache_line_t.fresh）。 */
        return (line->state == L2P_CACHE_DIRTY) || (line->state == L2P_CACHE_CLEAN);
    }

    /* miss：**本函数起了加载 ⇒ 记一次 miss**（这次访问的"发现未驻留"发生在此）。
     * 随后的重试命中会被 fresh 吃掉、不计 hit ⇒ 总账 = 1 miss，与旧实现同。 */
    cache->miss_count++;
    l2p_cache_note_access(cache, 1U);
    (void)l2p_cache_load_start(cache, lba_to_global_region(lba));
    return false;
}

ftl_status_t l2p_cache_write(l2p_cache_t *cache, lba_t lba, nand_pba_t pba)
{
    /* 前置条件（判据 2）：调用者传 ftl->cache。lba 越界是**数据**判断（判据 1），保留。 */
    ASSERT(cache != NULL);
    if ((uint64_t)lba >= L2P_GLOBAL_LBA_MAX)
    {
        return FTL_OK; /* 越界 no-op（与旧行为一致） */
    }

    ASSERT(lba_route_ch(lba) == cache->ch);
    ASSERT(lba_route_ce(lba) == cache->ce);

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

    line->access_count++;
    if (line->fresh != 0U)
    {
        line->fresh = 0U; /* 同 l2p_cache_read：加载后的首次触碰不计命中 */
    }
    else
    {
        /* ⚠ **必须**喂 thrashing 窗口（旧实现在 find_or_load 里一并做）：
         *   写路径的一次访问 = l2p_cache_ready（未驻留时记 miss）+ 本函数（记 hit）。
         *   若此处只加 hit_count 而不 note_access，窗口里就只剩那一次 miss
         *   ⇒ 随机写满盘从"1 miss + 1 hit = 50%"变成"1 miss + 0 hit = 100%"
         *   ⇒ 越过 L2P_THRASH_ENTER_PCT=90 ⇒ P1-4 降级在写密集下被误触发。 */
        cache->hit_count++;
        l2p_cache_note_access(cache, 0U);
    }
    line->entries[lba_to_entry(lba)] = pba;
    line->state                      = L2P_CACHE_DIRTY;

    /* Region 级脏标记（F4）：置本 region 的 bit。flush 落盘成功后清零。 */
    cache->region_dirty_bitmap[region_id / 32U] |= (1UL << (region_id & 31U));
    return FTL_OK;
}

/* ── 5b. 同步逃生口 ──
 *
 * 自旋 ftl_service_pump（推 NAND + 抽干完成表）直到就绪。**只许结构性同步上下文**
 * 调用（开机恢复 / CLI / checkpoint / HIL `fl`）——服务函数里调它就是嵌套泵。
 * 断言把这条约定变成可测的。
 *
 * ⚠ 阶段 8 起还要**顺带推进本实例的元数据目录整理 job**：ftl_service_pump 是
 *   **实例无关**的（它只跑 NAND + 完成表），而 md job 的推进挂在 ftl_service 的
 *   各档位里。若这里不推，CLI `fl` 一旦撞上"池吃紧 ⇒ append_slot 让路"就会**死等**
 *   （自旋里没有任何人把 compact 走完）。同步站点知道自己的 owner ⇒
 *   用 owner 的 metadata_dir/alloc 推进该 die 的目录整理 job。 */
static void l2p_sync_pump(l2p_cache_t *cache, struct ftl_context *owner)
{
    ASSERT(!ftl_op_in_dispatch());

    ftl_service_pump();

    if ((cache != NULL) && (owner != NULL))
    {
        metadata_dir_kick(owner->metadata_dir, &owner->alloc);
    }
}

ftl_status_t l2p_cache_read_sync(l2p_cache_t *cache, lba_t lba, nand_pba_t *out)
{
    uint32_t            guard = L2P_SYNC_GUARD_MAX;
    struct ftl_context *owner = NULL;

    ASSERT(cache != NULL);
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

    LOGE("[L2P] read_sync timeout lba=%lu\r\n", (unsigned long)lba);
    return FTL_ERROR;
}

ftl_status_t l2p_cache_write_sync(l2p_cache_t *cache, lba_t lba, nand_pba_t pba)
{
    uint32_t            guard = L2P_SYNC_GUARD_MAX;
    struct ftl_context *owner = NULL;

    ASSERT(cache != NULL);
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

    LOGE("[L2P] write_sync timeout lba=%lu\r\n", (unsigned long)lba);
    return FTL_ERROR;
}

/* ── 6. 写回 ── */

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
 *     · **取不到槽 ⇒ FTL_AGAIN（阶段 8 起的刻意变更，原为 FTL_ERROR）**：
 *       metadata_dir_append_slot 返回 NAND_PBA_INVALID 现在有两种来源——
 *       ① 两 plane 的 Free Pool 都空；② 目录整理 job 在飞（它要独占当前块的空槽）。
 *       两者都是**瞬态**（GC 会补池 / job 会走完），报 FTL_ERROR 会被调用者当硬失败
 *       （`l2p_flush_kick_owner` 跳过该行、`cp_flush_regions` 直接**中止 Checkpoint**）。
 *       背压让它们下轮重试即可。**绝不回退固定位置**（固定层是哨兵，写进去等于把
 *       "未映射"变成"有映射"，且下次再写同一页即损坏）。
 *     · ⚠ 槽位在 append_slot 里就消耗掉了：若其后建链/提交失败，该槽作废（写指针
 *       已推进）。与旧实现的"写失败"分支同级——region 仍指向上一份有效记录，无信息丢失。
 * -------------------------------------------------------------------- */
ftl_status_t l2p_cache_flush_kick(l2p_cache_t *cache, int line_idx)
{
    /* 前置条件（判据 2）：cache 恒 ftl->cache；line_idx 由调用者自持（下标越界是内部
     * 自铸句柄，非外部输入）——两侧都收口成断言。 */
    ASSERT(cache != NULL);
    ASSERT((line_idx >= 0) && (line_idx < (int)L2P_CACHE_SIZE));

    l2p_cache_line_t *line = &cache->lines[line_idx];

    if (line->state != L2P_CACHE_DIRTY)
    {
        return FTL_OK; /* 不需要写回（含"已在飞"/"已干净"） */
    }

    /* 映射页属于哪个 owner die 由全局 region 解析；写回必须落到 owner 的目录/分配器。 */
    struct ftl_context *owner = l2p_cache_owner(cache, line->region_id);

    if (cache->flush_inflight >= L2P_FLUSH_MAX_INFLIGHT)
    {
        return FTL_AGAIN; /* 共享 credit 池满：让 host I/O 先拿 FC 资源 */
    }

    nand_pba_t region_addr = metadata_dir_append_slot(owner->metadata_dir, &owner->alloc);
    if (region_addr == NAND_PBA_INVALID)
    {
        /* 瞬态：Free Pool 空（等 GC 补池）或目录整理 job 在飞（等它走完）⇒ 背压。
         * ⚠ 不能报 FTL_ERROR：l2p_flush_kick_owner 会跳过本行、cp_flush_regions 会**中止
         *   Checkpoint**——一次正常的后台压缩不该把 CP 打回去。 */
        return FTL_AGAIN;
    }

    /* 建链 + 把 entries 拷进 FC buffer（写的数据在 prepare 时就 memcpy 走了 ⇒
     * 提交后 line->entries 可自由改写，这正是"可读"而不"可写"的由来）。 */
    fc_command_t *cmd = NULL;
    if (ftl_async_write_page(&cmd, region_addr, line->entries, L2P_REGION_SIZE, FTL_PAGE_CFG_META()) != FTL_OK)
    {
        return FTL_ERROR;
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
    ASSERT(cache != NULL);
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

        const ftl_status_t st = l2p_cache_flush_kick(cache, (int)i);

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
 *     · 起过一次后又回到 DIRTY ⇒ 那条写失败了（ftl_op_l2p 的失败分支置回 DIRTY），
 *       直接报错，不再重试（与旧同步实现的"写失败即返回错误"一致）。
 * -------------------------------------------------------------------- */
ftl_status_t l2p_cache_flush_one_sync(l2p_cache_t *cache, int line_idx)
{
    uint32_t guard  = L2P_SYNC_GUARD_MAX;
    uint8_t  kicked = 0U;

    /* 前置条件（判据 2）：cache 恒 ftl->cache；line_idx 由调用者自持。 */
    ASSERT(cache != NULL);
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
    struct ftl_context *owner = l2p_cache_owner(cache, line->region_id);

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
    ASSERT(cache != NULL);
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
            if (l2p_cache_flush_one_sync(cache, (int)i) != FTL_OK)
            {
                return FTL_ERROR;
            }
        }
    }

    return FTL_OK;
}

/* ── 7. 完成服务（g_op_service[FTL_OP_L2P]）── */

/* --------------------------------------------------------------------
 * ftl_op_l2p
 *   功能：L2P cache 命令（加载 / 写回）的完成服务函数——按 line->state 收尾。
 *   参数：ftl —— 命令归属实例（ftl_op_dispatch 按命令自身 ch/ce/lun 解析后传入）；
 *         completion —— FC 完成状态；ctx —— `l2p_cache_line_t *`（提交时传的行指针）。
 *   返回：无。命令的 release 归本函数（见 ftl_op.h 的所有权表）。
 *   实现要点：
 *     · LOADING ⇒ 成功：ftl_cmd_read_data 把整页 region 拷进 line->entries（**必须早于
 *       release**——读的数据在 desc 链上），置 CLEAN。失败：**置 CLEAN 且保持 entries
 *       全 INVALID**（= 该 region 全部未映射），而不是置 INVALID。
 *       ⚠ 与旧同步实现的**刻意差异**：旧实现失败后置 INVALID，于是**每次**访问都重试
 *       加载（坏 region 无限重读 NAND）；异步化后重试语义会让 host 读永久停在
 *       FTL_AGAIN（evict→加载→失败→再 evict 的活锁）。观察行为一致：都返回"未映射"。
 *     · FLUSHING ⇒ 成功：metadata_dir_commit_region 登记新位置（旧位置活计数 -1）、
 *       置 CLEAN、清 region 脏位。失败：置回 DIRTY（保留重试；槽位已消耗不可复用，
 *       Directory 不登记 ⇒ 该 region 仍指向上一份有效记录，无信息丢失）。
 *     · 其余状态 ⇒ 编程错误（LOADING/FLUSHING 期间行不可被改写状态）。
 * -------------------------------------------------------------------- */
void ftl_op_l2p(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx)
{
    l2p_cache_line_t *line = (l2p_cache_line_t *)ctx;

    ASSERT(completion != NULL);
    ASSERT(line != NULL);

    fc_command_t *cmd = fc_command_id_to_command((uint16_t)completion->command_id);

    /* 不该发生：路由表已在 main.c 逐实例 boot 之前设好（见 ssd_ftl_boot_all ①），
     * 完成派发保证 ftl 恒非空。不留 release-and-return 兜底：debug 当场炸，
     * release（NDEBUG 裁空）下若真走到则 NULL 解引用留现场。 */
    ASSERT(ftl != NULL);

    ASSERT(ftl->cache != NULL);
    ASSERT(&ftl->cache->lines[0] <= line);
    ASSERT(line < &ftl->cache->lines[L2P_CACHE_SIZE]);

    /* 命令按 PBA 的 ch/ce/lun 路由回 owner；line 的全局 region 必须属于同一个 owner。 */
    ASSERT(ftl_topo_dim(ftl->topo) == (line->region_id % FTL_DIM_TOTAL));
    ASSERT(ftl == l2p_cache_owner(ftl->cache, line->region_id));

    uint8_t failed = ((completion->status & FC_CW_STATUS_ERROR_MASK) != 0U) ? 1U : 0U;

    if (line->state == L2P_CACHE_LOADING)
    {
        if (failed == 0U)
        {
            /* 数据在 desc 链上 ⇒ 必须早于 release 拷出 */
            (void)ftl_cmd_read_data(cmd, line->entries, L2P_REGION_SIZE);
        }
        /* 成功与失败都置 CLEAN：失败时 entries 保持全 NAND_PBA_INVALID = 全部未映射
         * （见函数头对"刻意差异"的说明）。 */
        line->state = L2P_CACHE_CLEAN;
        line->fresh = 1U; /* 加载已完成 ⇒ 下一次命中是本次访问的重试，不计 hit */

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
            metadata_dir_commit_region(ftl->metadata_dir,
                                       &ftl->alloc,
                                       (uint16_t)l2p_global_to_local_region(line->region_id),
                                       ftl_cmd_pba(cmd));

            line->state = L2P_CACHE_CLEAN;

            /* 落盘成功：本 region 的**全局**脏位清零（下次 checkpoint 不再重写） */
            ftl->cache->region_dirty_bitmap[line->region_id / 32U] &=
                ~(1UL << (line->region_id & 31U));
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
    ASSERT(cache != NULL);
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
        const uint32_t global = (local * FTL_DIM_TOTAL) + owner_dim;
        cache->region_dirty_bitmap[global / 32U] &= ~(1UL << (global & 31U));
    }
}

/*
 * l2p_cache_snapshot_owner_bitmap - 全局脏位图 → 本 die 的局部 2048-bit 快照
 *
 * Checkpoint 起点调用（先快照再 flush）。DIM=1 时等价于整份位图拷贝。
 */
void l2p_cache_snapshot_owner_bitmap(const l2p_cache_t *cache, uint32_t owner_dim, uint32_t out[L2P_LOCAL_DIRTY_WORDS])
{
    ASSERT(cache != NULL);
    ASSERT(owner_dim < FTL_DIM_TOTAL);
    ASSERT(out != NULL);

    memset(out, 0, L2P_LOCAL_DIRTY_WORDS * sizeof(uint32_t));

    for (uint32_t local = 0U; local < L2P_REGIONS_PER_INSTANCE; local++)
    {
        const uint32_t global = (local * FTL_DIM_TOTAL) + owner_dim;

        if ((cache->region_dirty_bitmap[global / 32U] & (1UL << (global & 31U))) != 0U)
        {
            out[local / 32U] |= (1UL << (local & 31U));
        }
    }
}

/*
 * wear.c — 磨损计数持久化（Wear 表：页式追加日志）
 *
 * 职责：
 *   把每个块的读/擦计数（read_count / erase_count）持久化到固定块 WEAR_BLOCK 上的
 *   **页式追加日志**——常态增量写脏块、写满则 compact 成全量快照；开机逐页回放恢复计数。
 *
 * 与 wear.h / wear_leveling.c 的分工：
 *   wear.h            —— wear_page_t / wear_entry_t 格式 + 宏；
 *   本文件            —— 持久化（日志追加/compact/回放）+ 脏位图；
 *   wear_leveling.c   —— 用这些计数做静态磨损均衡（搬移决策），不碰持久化。
 *
 * 介质模型（P0-1）：
 *   块 WEAR_BLOCK（plane 0）上按页 0..PAGES_PER_BLOCK-1 **只追加**；页序 == sequence 序。
 *   一条记录 = 一页 = 头（magic+sequence+entry_count）+ 最多 WEAR_ENTRY_MAX 条
 *   (block, read_count, erase_count)。回放**绝对值 + 最新胜**（后写覆盖先写）。
 *
 * 文件结构：
 *   1. 脏位图            wear_dirty_set / wear_dirty_count / wear_snapshot_get / wear_mark_dirty
 *   2. 页地址            wear_page_addr
 *   3. 日志写（异步）    wear_submit_next_page / wear_job_finish / wear_kick / ftl_op_wear
 *   4. 开机回放          wear_init
 *
 * 游标/序号/脏位图是 **per-instance**（序 4：收进 ftl_allocator_t.wear，见 wear_state_t）——
 * 多实例（多 LUN）下每实例一份，互不干扰（原为 file-scope static）。
 *
 * ── 落盘是**异步 job**（v0.75 阶段 7）──
 * 旧 `wear_flush` 在一次调用内同步写完 1~25 页（可能含一次整块擦）；现在 `wear_kick`
 * 只提交**第一条**命令即返回，完成由 g_op_service[FTL_OP_WEAR]（ftl_op_wear）收尾并
 * 续提交下一页 ⇒ 后台档每轮最多推进一步，不再把 superloop 钉在一次调用里。
 *
 * ⚠ 批**跨多轮**，期间 mig/RR job 的完成会调 wear_mark_dirty ⇒ 本批只写**起点快照**
 *   里的脏块（`wear.dirty_snapshot`），收尾也只清快照里的位；批期间新标脏的留给下一批。
 *   详见 allocator.h 的 wear_state_t.dirty_snapshot 注释。
 */

#include "ftl/wear.h"

#include "common/log.h"
#include "fc/fc.h"
#include "ftl/ftl_fc_interface.h"
#include "ftl/ftl_job.h" /* 心跳（[JOB] 观测口）*/
#include "ftl/ftl_op.h"  /* ftl_op_submit / FTL_OP_WEAR（阶段 7） */
#include "platform/platform.h"

#include <string.h>

/*
 * Wear 内部状态：**per-instance**，落在 ftl_allocator_t.wear（wear_state_t，序 4）——
 * sequence / next_page / 节拍基准 / 脏位图。原先是 file-scope static ⇒ 多实例
 * （多 LUN）下所有实例共享一份，落盘互相踩。
 */

/* ── 1. 脏位图（512 B，按 gblock 置位；越界忽略）── */

static void wear_dirty_set(ftl_allocator_t *alloc, uint16_t gblock)
{
    alloc->wear.dirty[gblock >> 3] |= (uint8_t)(1U << (gblock & 7U));
}

/* wear_dirty_count — 置位块数（= 本次 flush 需写的条目数，popcount 逐字节累加） */
static uint32_t wear_dirty_count(const ftl_allocator_t *alloc)
{
    uint32_t n = 0U;
    uint16_t i;

    for (i = 0U; i < (uint16_t)sizeof(alloc->wear.dirty); i++)
    {
        n += (uint32_t)__builtin_popcount((unsigned)alloc->wear.dirty[i]);
    }

    return n;
}

/* wear_mark_dirty — 标记某块的计数已变（越界忽略）；计数变更点调用，flush 时只写脏块 */
void wear_mark_dirty(ftl_allocator_t *alloc, uint16_t gblock)
{
    if ((alloc != NULL) && (gblock < FTL_BLOCK_TOTAL))
    {
        wear_dirty_set(alloc, gblock);
    }
}

/* wear_snapshot_get — 本批起点快照里该块是否脏（"本批写哪些块"的唯一判据） */
static bool wear_snapshot_get(const ftl_allocator_t *alloc, uint16_t gblock)
{
    return ((alloc->wear.dirty_snapshot[gblock >> 3] >> (gblock & 7U)) & 1U) != 0U;
}

/* ── 2. 页地址 ── */

/* ── 固定位置页地址（WEAR_BLOCK，plane 0，页 0..PAGES_PER_BLOCK-1）── */
static nand_pba_t wear_page_addr(uint8_t ch, uint8_t ce, uint8_t lun, uint16_t page)
{
    return pba_make(ch, ce, lun, WEAR_BLOCK, 0U, page, 0U);
}

/* ── 3. 日志写（异步 job，v0.75 阶段 7）── */

/* --------------------------------------------------------------------
 * wear_restore_dirty
 *   功能：本批**未走完**（擦失败 / 页写失败）时把起点脏位并回脏位图。
 *   参数：alloc —— 分配器。
 *   返回：无。
 *   实现要点：批起点把 `dirty` **整体清零**（见 wear_kick），批期间 wear_mark_dirty
 *             置的位因此天然只表示"批开始后才变的块"。失败时把 snapshot 并回来，
 *             得到「批前脏 ∪ 批中新脏」——正是下一次重试该写的集合。
 * -------------------------------------------------------------------- */
static void wear_restore_dirty(ftl_allocator_t *alloc)
{
    uint16_t i;

    for (i = 0U; i < (uint16_t)sizeof(alloc->wear.dirty); i++)
    {
        alloc->wear.dirty[i] |= alloc->wear.dirty_snapshot[i];
    }
}

/* --------------------------------------------------------------------
 * wear_job_finish
 *   功能：本批走完的收尾——落节拍基准、job 回 IDLE。
 *   参数：alloc —— 分配器。
 *   返回：无。
 *   实现要点：**不清脏位图**——批起点已把 dirty 清零，此刻 dirty 里剩下的位正好是
 *             「批期间被 wear_mark_dirty 置位、本批没写（或写了旧值）的块」，留给
 *             下一批即可。这比"收尾时 `dirty &= ~snapshot`"更强：后者会丢掉「批前就
 *             脏、批中被写、批中又变」的块的最后一次变更（同步版 wear_flush 整批原子，
 *             没有这个窗口；清零式让异步版重新拿到同样的原子性）。
 *             节拍基准写的是**批起点**的 flush_ms/flush_writes（与旧 wear_flush 在
 *             写完后才记 now_ms 略有差别：旧版把整个批的耗时算进下一次的间隔，
 *             新版不算。批耗时是毫秒级、间隔是分钟级，差异可忽略）。
 * -------------------------------------------------------------------- */
static void wear_job_finish(ftl_allocator_t *alloc)
{
    alloc->wear.last_flush_writes = alloc->wear.job.flush_writes;
    alloc->wear.last_flush_ms     = alloc->wear.job.flush_ms;

    alloc->wear.job.phase   = (uint8_t)WEAR_JOB_IDLE;
    alloc->wear.job.waiting = 0U;
}

/* --------------------------------------------------------------------
 * wear_submit_next_page
 *   功能：从 job.cursor 起**装一页**日志并异步提交（**提交即返回**）。
 *   参数：alloc —— 分配器。
 *   返回：FTL_OK —— 已提交（job.waiting=1）；或本批已走完（job 回 IDLE）；
 *         FTL_ERROR —— 命令建不起来（命令池空），cursor 不动，下轮重试。
 *   实现要点：
 *     · 条目按 gblock 升序流式填；dirty_only != 0 时按**起点快照**过滤。
 *     · `sequence` 在**该页起笔时**自增（同旧 wear_write_range）；提交失败重来时
 *       会再自增一次——旧版整体重跑亦然，回放只按页序、不比较 sequence 值。
 *     · **cursor 只在提交成功后推进** ⇒ 提交被背压拒时天然回到本页起点重来。
 *     · 页缓冲是 2 KB 栈上的：ftl_async_write_page 在 prepare 时已把它拷进 FC
 *       buffer 池（同 journal_freeze 的做法）⇒ 函数返回后源缓冲即可复用。
 *     · 收尾（本批走完）走 wear_job_finish，不在这里散写。
 * -------------------------------------------------------------------- */
static ftl_status_t wear_submit_next_page(ftl_allocator_t *alloc)
{
    wear_page_t   page;
    fc_command_t *cmd;
    uint32_t      n = 0U; /* 当前页已填条目数；0 = 该页尚未起笔 */
    uint16_t      gblock;
    uint16_t      next_cursor = (uint16_t)FTL_BLOCK_TOTAL; /* 默认：本页即最后一块 */

    for (gblock = alloc->wear.job.cursor; gblock < FTL_BLOCK_TOTAL; gblock++)
    {
        if ((alloc->wear.job.dirty_only != 0U) && (!wear_snapshot_get(alloc, gblock)))
        {
            continue;
        }

        if (n == 0U)
        {
            memset(&page, 0, sizeof(page)); /* padding 归零 ⇒ 介质内容确定 */
            page.magic    = WEAR_MAGIC;
            page.sequence = ++alloc->wear.sequence;
        }

        page.entries[n].block        = gblock;
        page.entries[n].read_count   = alloc->blocks[gblock].read_count;
        page.entries[n].erase_count  = alloc->blocks[gblock].erase_count;
        page.entries[n].last_ecc     = alloc->blocks[gblock].rr.last_ecc;
        page.entries[n].rising_count = alloc->blocks[gblock].rr.rising_count;
        page.entries[n].health       = alloc->blocks[gblock].rr.health;
        page.entries[n]._reserved    = 0U;
        n++;

        if (n == WEAR_ENTRY_MAX)
        {
            next_cursor = (uint16_t)(gblock + 1U);
            break;
        }
    }

    if (n == 0U)
    {
        wear_job_finish(alloc); /* 本批走完（含"剩余块全被快照过滤掉"） */
        return FTL_OK;
    }

    page.entry_count = n;

    if (ftl_async_write_page(&cmd,
                             wear_page_addr(alloc->ch, alloc->ce, alloc->lun, alloc->wear.next_page),
                             &page,
                             sizeof(page),
                             FTL_PAGE_CFG_META()) != FTL_OK)
    {
        return FTL_ERROR; /* 命令池空 / 建链失败 ⇒ cursor 不动，下轮重试 */
    }
    if (ftl_op_submit(cmd, FTL_OP_WEAR, alloc) != FTL_OK)
    {
        (void)fc_command_release(cmd); /* ftl_op_submit 失败不代为 release */
        return FTL_ERROR;
    }

    alloc->wear.job.cursor  = next_cursor;
    alloc->wear.job.waiting = 1U;
    return FTL_OK;
}

/* --------------------------------------------------------------------
 * wear_kick
 *   功能：后台落盘的**起步 / 续跑**入口（提交即返回）。
 *   参数：alloc —— 分配器；total_writes —— 当前累计写次数（节拍判据之一）。
 *   返回：无（成败由 ftl_op_wear 收尾）。
 *   实现要点：
 *     ① 在飞（waiting != 0）⇒ 直接返回，等完成服务函数；
 *     ② phase == WRITE 且不在飞 ⇒ 上一步提交被背压拒，续跑本批；
 *     ③ 其余非 IDLE（= ERASE 在飞）⇒ 返回；
 *     ④ IDLE：脏位空 / 节拍未到 ⇒ 空转；
 *     ⑤ 剩余页装不下 ⇒ 先异步擦块（phase = ERASE）；否则按起点快照起增量批。
 *   节拍门与旧 wear_flush 逐字一致：定时（WEAR_FLUSH_INTERVAL_MS）**或** 写入增量
 *   （WEAR_FLUSH_ERASE_DELTA，擦除活跃度的代理），任一满足即落盘。
 * -------------------------------------------------------------------- */
void wear_kick(ftl_allocator_t *alloc, uint32_t total_writes)
{
    const ssd_platform_ops_t *platform;
    uint32_t                  now_ms;
    uint32_t                  dirty_count;
    uint32_t                  pages_needed;

    /* 心跳（见 ftl_job.h）：job 在飞期间每满 1s 打一行 phase/cursor/age */
    if (alloc->wear.job.phase == (uint8_t)WEAR_JOB_IDLE)
    {
        ftl_job_heartbeat_reset(&alloc->wear.job.log_ms, &alloc->wear.job.start_ms);
    }
    else if (ftl_job_heartbeat(&alloc->wear.job.log_ms, &alloc->wear.job.start_ms))
    {
        LOGI("[WEAR] job phase=%u waiting=%u full=%u cursor=%u age=%lums\r\n",
             (unsigned)alloc->wear.job.phase,
             (unsigned)alloc->wear.job.waiting,
             (unsigned)(alloc->wear.job.dirty_only == 0U),
             (unsigned)alloc->wear.job.cursor,
             (unsigned long)ftl_job_age_ms(&alloc->wear.job.start_ms));
    }

    if (alloc->wear.job.waiting != 0U)
    {
        return; /* ① 命令在飞 */
    }
    if (alloc->wear.job.phase == (uint8_t)WEAR_JOB_WRITE)
    {
        (void)wear_submit_next_page(alloc); /* ② 背压后续跑 */
        return;
    }
    if (alloc->wear.job.phase != (uint8_t)WEAR_JOB_IDLE)
    {
        return; /* ③ ERASE 在飞（waiting 必为 1，此处只是防御） */
    }

    dirty_count = wear_dirty_count(alloc);
    if (dirty_count == 0U)
    {
        return; /* ④ 无变更：空转 */
    }

    platform = ssd_platform_get();
    now_ms   = ((platform != NULL) && (platform->time_us != NULL)) ? (uint32_t)(platform->time_us() / 1000U) : 0U;

    if (((now_ms - alloc->wear.last_flush_ms) < WEAR_FLUSH_INTERVAL_MS) &&
        ((total_writes - alloc->wear.last_flush_writes) < WEAR_FLUSH_ERASE_DELTA))
    {
        return; /* ④ 节拍未到 */
    }

    /* 起点快照 + **整体清脏**：批期间 wear_mark_dirty 置的位因此只表示"批开始后才变的
     * 块"（收尾不清脏、失败并回来，见 wear_job_finish / wear_restore_dirty）。 */
    memcpy(alloc->wear.dirty_snapshot, alloc->wear.dirty, sizeof(alloc->wear.dirty_snapshot));
    memset(alloc->wear.dirty, 0, sizeof(alloc->wear.dirty));
    alloc->wear.job.flush_ms     = now_ms;
    alloc->wear.job.flush_writes = total_writes;

    /* 剩余页装不下本次脏条目 ⇒ compact（擦块 + 全量快照）；否则增量追加 */
    pages_needed = (dirty_count + WEAR_ENTRY_MAX - 1U) / WEAR_ENTRY_MAX;

    if (((uint32_t)alloc->wear.next_page + pages_needed) > PAGES_PER_BLOCK)
    {
        fc_command_t *cmd;

        /* ⑤ 先异步擦整块。擦成功才 next_page = 0 并起全量快照（见 ftl_op_wear）。 */
        if (ftl_async_erase_block(&cmd, wear_page_addr(alloc->ch, alloc->ce, alloc->lun, 0U)) != FTL_OK)
        {
            wear_restore_dirty(alloc);
            return; /* 命令池空 ⇒ 下轮重来（next_page 未动，脏位已并回） */
        }
        if (ftl_op_submit(cmd, FTL_OP_WEAR, alloc) != FTL_OK)
        {
            (void)fc_command_release(cmd);
            wear_restore_dirty(alloc);
            return;
        }

        alloc->wear.job.phase   = (uint8_t)WEAR_JOB_ERASE;
        alloc->wear.job.waiting = 1U;
        return;
    }

    /* ⑤ 增量批：先落 phase，提交被拒也停在 WRITE/waiting=0 等续跑 */
    alloc->wear.job.dirty_only = 1U;
    alloc->wear.job.cursor     = 0U;
    alloc->wear.job.phase      = (uint8_t)WEAR_JOB_WRITE;
    (void)wear_submit_next_page(alloc);
}

/* --------------------------------------------------------------------
 * ftl_op_wear
 *   功能：Wear 日志 job 的完成服务函数（g_op_service[FTL_OP_WEAR]）。
 *   参数：ftl —— 实例（**不用**：ctx 已是本实例的 allocator）；
 *         completion —— 完成状态；ctx —— `ftl_allocator_t *`（ftl_op_submit 时传）。
 *   返回：无。命令的 release 归本函数。
 *   实现要点：失败分支**逐条复刻**旧 wear_flush / wear_compact 的语义——
 *     · 擦失败 ⇒ next_page **不动**、脏位并回（旧 wear_compact 的首个 return），job 回
 *       IDLE 让下轮 kick 重来；
 *     · 增量页写失败 ⇒ next_page 不推进、脏位并回（旧 wear_write_page 的 return）；
 *     · compact 快照页写失败 ⇒ **全标脏**（旧 wear_compact 的 memset 0xFF：擦除已抹掉
 *       旧内容，起点时"干净"的块也成了未持久化），job 回 IDLE；
 *     · 写成功 ⇒ next_page++ 并续提交下一页（**不在本函数里 kick**：续提交是提交侧
 *       动作，直接调 wear_submit_next_page，不嵌套泵）。
 * -------------------------------------------------------------------- */
void ftl_op_wear(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx)
{
    ftl_allocator_t *alloc = (ftl_allocator_t *)ctx;
    fc_command_t    *cmd;
    uint8_t          failed;

    (void)ftl; /* 实例无关：ctx 就是本实例的 allocator */

    ASSERT(alloc != NULL);
    ASSERT(completion != NULL);

    cmd    = fc_command_id_to_command((uint16_t)completion->command_id);
    failed = ((completion->status & FC_CW_STATUS_ERROR_MASK) != 0U) ? 1U : 0U;

    if (alloc->wear.job.phase == (uint8_t)WEAR_JOB_ERASE)
    {
        if (failed != 0U)
        {
            LOGE("[WEAR] compact erase failed status=0x%02x — next_page 不动，下轮重试\r\n",
                 (unsigned)completion->status);
            alloc->wear.job.phase   = (uint8_t)WEAR_JOB_IDLE;
            alloc->wear.job.waiting = 0U;
            wear_restore_dirty(alloc);
            (void)fc_command_release(cmd);
            return;
        }

        /* 擦成功 ⇒ 从头写全量快照。先落 phase，提交被拒就停在 WRITE/waiting=0 等续跑。 */
        alloc->wear.next_page      = 0U;
        alloc->wear.job.dirty_only = 0U;
        alloc->wear.job.cursor     = 0U;
        alloc->wear.job.phase      = (uint8_t)WEAR_JOB_WRITE;
        alloc->wear.job.waiting    = 0U;
        (void)wear_submit_next_page(alloc);
        (void)fc_command_release(cmd);
        return;
    }

    if (alloc->wear.job.phase == (uint8_t)WEAR_JOB_WRITE)
    {
        if (failed != 0U)
        {
            if (alloc->wear.job.dirty_only == 0U)
            {
                /* 快照写残：擦除已抹掉旧内容 ⇒ **全部**块都成了"未持久化"（不只是原来脏的那些）。
                 * 全标脏让下次重来；否则快照尾部未写到的块会被静默丢掉
                 * （它们在 RAM 里是对的，只是没落盘，而旧内容已被擦除）。 */
                memset(alloc->wear.dirty, 0xFF, sizeof(alloc->wear.dirty));
                LOGE("[WEAR] snapshot page write failed status=0x%02x — 全标脏重来\r\n",
                     (unsigned)completion->status);
            }
            else
            {
                LOGE("[WEAR] log page write failed status=0x%02x — next_page 不动，下轮重试\r\n",
                     (unsigned)completion->status);
                wear_restore_dirty(alloc);
            }
            alloc->wear.job.phase   = (uint8_t)WEAR_JOB_IDLE;
            alloc->wear.job.waiting = 0U;
            (void)fc_command_release(cmd);
            return;
        }

        alloc->wear.next_page++; /* 写成功才推进（失败不推进，见上） */
        alloc->wear.job.waiting = 0U;
        (void)fc_command_release(cmd);

        /* 续提交下一页；起不来（命令池空）⇒ 停在 WRITE/waiting=0，下轮 kick 续跑 */
        (void)wear_submit_next_page(alloc);
        return;
    }

    /* 不该发生：一条命令对应一个 job，phase 必为 ERASE / WRITE */
    LOGE("[WEAR] unexpected completion phase=%u\r\n", (unsigned)alloc->wear.job.phase);
    alloc->wear.job.phase   = (uint8_t)WEAR_JOB_IDLE;
    alloc->wear.job.waiting = 0U;
    (void)fc_command_release(cmd);
}

/* ── 4. 开机回放 ── */

/* --------------------------------------------------------------------
 * wear_init
 *   功能：开机逐页回放 WEAR_BLOCK 日志，恢复各块读/擦计数；并打印观测口。
 *   参数：alloc —— 分配器（承载计数）。
 *   返回：FTL_OK 成功（含冷启动无有效页）；FTL_ERROR 回放读失败。
 *   实现要点：页序 == sequence 序（只追加），故**第一个无效页即日志末尾**，回放到此为止；
 *             无有效页 = 冷启动（计数保持 ftl_init 的零值），**不 brick 开机**（非致命）。
 * -------------------------------------------------------------------- */
ftl_status_t wear_init(ftl_allocator_t *alloc)
{
    wear_page_t page;
    uint16_t    p;
    uint16_t    valid_pages = 0U;
    uint32_t    max_ec      = 0U;
    uint32_t    min_ec      = 0U;
    int         have_min    = 0;
    uint16_t    gblock;

    memset(alloc->wear.dirty, 0, sizeof(alloc->wear.dirty));
    alloc->wear.sequence          = 0U;
    alloc->wear.next_page         = 0U;
    alloc->wear.last_flush_writes = 0U;
    alloc->wear.last_flush_ms     = 0U;

    /* 异步 job 归零（开机路径不跑 job；allocator_init 的 memset 已覆盖，此处是
     * 「重入 wear_init」的显式保险——与 bbt_init 同惯例） */
    alloc->wear.job.phase        = (uint8_t)WEAR_JOB_IDLE;
    alloc->wear.job.waiting      = 0U;
    alloc->wear.job.dirty_only   = 0U;
    alloc->wear.job.cursor       = 0U;
    alloc->wear.job.flush_ms     = 0U;
    alloc->wear.job.flush_writes = 0U;
    memset(alloc->wear.dirty_snapshot, 0, sizeof(alloc->wear.dirty_snapshot));

    /* 逐页回放：页序 == sequence 序（块内只追加；compact 擦块后从头写全量快照），
     * 故**第一个无效页即日志末尾**，回放到此为止。
     * 非致命：无有效页 = 冷启动（计数保持 ftl_init 的 memset 零值），不 brick 开机。 */
    for (p = 0U; p < PAGES_PER_BLOCK; p++)
    {
        nand_pba_t addr = wear_page_addr(alloc->ch, alloc->ce, alloc->lun, p);
        uint32_t   k;

        memset(&page, 0, sizeof(page));
        if (ftl_sync_read_page(addr, &page, sizeof(page), FTL_PAGE_CFG_META()) != 0)
        {
            break;
        }
        if ((page.magic != WEAR_MAGIC) || (page.entry_count > WEAR_ENTRY_MAX))
        {
            break;
        }

        for (k = 0U; k < page.entry_count; k++)
        {
            const wear_entry_t *e = &page.entries[k];

            if (e->block >= FTL_BLOCK_TOTAL)
            {
                continue;
            }
            alloc->blocks[e->block].erase_count = e->erase_count;
            alloc->blocks[e->block].read_count  = e->read_count;

            /* rr 健康（v0.65，序 8）：慢死预测的输入。health 取值越界即当 HEALTHY
             * （防御坏页——枚举只有 0..3，越界值会让 rr_predictive_scan 的比较行为
             *  未定义；钳到 HEALTHY 等于"这块重头数"，安全侧）。 */
            alloc->blocks[e->block].rr.last_ecc     = e->last_ecc;
            alloc->blocks[e->block].rr.rising_count = e->rising_count;
            alloc->blocks[e->block].rr.health =
                (e->health <= (uint8_t)RR_BLOCK_DYING) ? e->health : (uint8_t)RR_BLOCK_HEALTHY;
        }

        alloc->wear.sequence = page.sequence;
        valid_pages++;
    }

    alloc->wear.next_page = valid_pages;

    /* 开机可观测（判据「重启后计数一致」的观测口）：跳过坏块统计 max/min */
    for (gblock = 0U; gblock < FTL_BLOCK_TOTAL; gblock++)
    {
        uint32_t ec = alloc->blocks[gblock].erase_count;

        if (ftl_block_is_bad(&alloc->blocks[gblock]))
        {
            continue;
        }
        if (ec > max_ec)
        {
            max_ec = ec;
        }
        if ((have_min == 0) || (ec < min_ec))
        {
            min_ec   = ec;
            have_min = 1;
        }
    }

    LOGI("[WEAR] seq=%u pages=%u max_ec=%u min_ec=%u\r\n",
         (unsigned)alloc->wear.sequence,
         (unsigned)valid_pages,
         (unsigned)max_ec,
         (unsigned)min_ec);

    return FTL_OK;
}

/*
 * journal.c — L2P 增量日志（环形块池 + 页追加 + 掉电回放 + 截断）
 *
 * 职责：
 *   把每次 L2P 更新（lba, old_pba, new_pba）以「页追加」写进 Journal 环形块池，
 *   供掉电后回放恢复；与 Checkpoint 协同做日志回收（truncate）。
 *
 * 与 journal.h 的分工：
 *   journal.h —— 页头格式（JOURNAL_HEADER_SIZE / MAGIC）+ 条目结构 + 环形状态
 *                （l2p_journal_t）；
 *   本文件     —— 追加 / 刷页 / 回放 / 截断的实现。
 *
 * 写模型（P1-6，阶段 2 起改异步）：
 *   · 追加进 RAM 页缓冲（page_buf）；页满（JOURNAL_ENTRIES_PER_PAGE）时**不再同步落盘**，
 *     改为交给异步 job（journal_freeze → ftl_op_submit(FTL_OP_JOURNAL)），完成由
 *     g_op_service[FTL_OP_JOURNAL]（ftl_op_journal）按 job.phase 推进；
 *   · 块满换环形下一块——**先擦后写**，换块擦除也是 job 的一个 phase（JRN_ERASE）；
 *   · 同 LBA 就地合并（不消耗 sequence）。
 *
 * 为什么需要屏障时走 journal_flush_page：GC/WL 的 F10 契约要求「搬移条目先落盘再擦
 *   victim」，HIL `fl` 要求「已持久化映射落盘」。这两处**必须**同步 ⇒ journal_flush_page
 *   先排空在飞 job，再同步刷当前页（它是唯一的同步出口，也是同步回退路径）。
 *
 * 掉电保护：Journal 页 Random **关闭**、CRC **开启**（v0.62 起由下层 FC 提供 per-CW CRC +
 *   ECC），外加页头 magic + entry_count 合法性 + page_seq 单调三层结构判据（设计 §7.6）。
 *   异步化**不改变**掉电模型：本仓无电容，acked 写本来就容忍丢最后一页 journal 条目
 *   （write-back 攒批），把「页满时立刻阻塞刷」改成「页满时交给 job 刷」只是把阻塞挪走。
 *
 * ⚠ **本文件整个不转断言**（assertion_guidelines §六 辨析 C）：replay 读的是 NAND 上的
 *   页，页头可能因掉电撕裂而非法——那是掉电的**预期情形**，必须返回错误/跳过，不能断言。
 *   （ftl_op_journal 的入参断言是例外：那是**命令派发**路径，不读 NAND 页头。）
 *
 * 文件结构：
 *   1. 页地址辅助     journal_page_addr / journal_block_addr（static）
 *   2. 初始化         journal_init
 *   3. 追加与刷页     journal_append / journal_freeze / journal_kick /
 *                     journal_start_erase / journal_flush_page（+ journal_flush_now）
 *   4. 完成服务       ftl_op_journal（g_op_service[FTL_OP_JOURNAL]）
 *   5. 回放与写指针恢复 journal_scan_latest / journal_resume / journal_replay
 *   6. 截断           journal_truncate_plan / journal_truncate_commit（两步，异步 CP 用）
 */

#include "journal.h"

#include "assertion.h"
#include "completion_table.h"
#include "fc.h"
#include "ftl.h"
#include "ftl_fc_interface.h"
#include "ftl_job.h" /* 心跳（[JOB] 观测口）*/
#include "ftl_op.h"  /* ftl_op_submit / FTL_OP_JOURNAL */
#include "log.h"
#include "onfi_1_0.h"

#include <string.h>

/* journal_drain 的自旋上限（每轮一次 fc_service + completion_table_drain）。
 * 单页编程在轮询模型下是有限步完成，故这只是「命令丢失 / NAND 卡死」的兜底。 */
#define JOURNAL_DRAIN_SPIN_MAX (100000U)

/* ══════════════════════════════════════════════════════════
 * 1. 页地址辅助
 * ══════════════════════════════════════════════════════════ */

/* ── Journal Block 页地址辅助 ── */
/* journal_page_addr — 第 b 块第 page 页的 NAND 地址（plane 由页号奇偶决定，见下） */
static nand_pba_t journal_page_addr(const l2p_journal_t *journal, uint8_t b, uint8_t page)
{
    /* cw = 0 = 整页；plane 由页号奇偶决定：偶页 P0、奇页 P1（双 Plane 交错） */
    return pba_make(journal->topo.ch, journal->topo.ce, journal->topo.lun, (uint32_t)(JOURNAL_BLOCK0 + b), (uint32_t)(page & 1U), page, 0U);
}

/* journal_block_addr — 第 b 块的首页地址（擦除用的基址；plane/page 归零，cw=0）。
 * ⚠ 页地址跨 P0/P1（见 journal_page_addr）⇒ **所有擦除必须用
 *   ftl_*_erase_block_multi_plane**，否则 P1 的那一半页不会被擦，环形回绕后二次编程。 */
static nand_pba_t journal_block_addr(const l2p_journal_t *journal, uint8_t b)
{
    return pba_make(journal->topo.ch, journal->topo.ce, journal->topo.lun, (uint32_t)(JOURNAL_BLOCK0 + b), 0U, 0U, 0U);
}

/* ══════════════════════════════════════════════════════════
 * 2. 初始化
 * ══════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * journal_init
 *   功能：初始化 Journal 环形块池——记录拓扑、预铸各块 PBA、复位写指针与序号。
 *   参数：journal —— 待初始化对象；topo —— 目标 NAND 芯片路由。
 *   返回：无。
 *   实现要点：sequence_counter 从 1 起（0 保留作「无序号」）；page_buf 填 0xFF 表示
 *             空页（与 NAND 擦除态一致）。
 * -------------------------------------------------------------------- */
void journal_init(l2p_journal_t *journal, ftl_topology_t topo)
{
    /* 前置条件（判据 2 / §二十六「子模块公开入口」）：调用者一律传 ftl->journal。 */
    ASSERT(journal != NULL);

    journal->topo = topo;

    for (uint8_t b = 0U; b < JOURNAL_BLOCK_COUNT; b++)
    {
        journal->block_pba[b] = pba_make(topo.ch, topo.ce, topo.lun, (uint32_t)(JOURNAL_BLOCK0 + b), 0U, 0U, 0U);
    }

    journal->current_block    = 0U;
    journal->current_page     = 0U;
    journal->current_entry    = 0U;
    journal->sequence_counter = 1U;
    journal->merge_floor      = 0U;
    journal->pre_erased_block = JOURNAL_BLOCK_NONE;

    memset(journal->page_buf, 0xFF, sizeof(journal->page_buf));

    journal->checkpoint_needed = 0U;
    journal->emergency         = 0U;

    journal->write_fail_count   = 0U;
    journal->write_fail_seq     = 0U;
    journal->write_fail_pending = 0U;

    /* 异步刷页 job 复位（阶段 2）：无在飞、无待偿擦除 */
    journal->job.phase      = JRN_IDLE;
    journal->job.need_erase = 0U;
}

/* ══════════════════════════════════════════════════════════
 * 3. 追加与刷页
 * ══════════════════════════════════════════════════════════ */

/* 前置声明：journal_append（页满时）要用，定义在本节稍后。 */
static bool journal_freeze(l2p_journal_t *journal);

/* --------------------------------------------------------------------
 * journal_append
 *   功能：向当前页缓冲追加一条 L2P 更新条目（同 LBA 就地合并）。
 *   参数：journal —— Journal；lba —— 逻辑地址；old_pba/new_pba —— 更新前后物理地址。
 *   返回：true = 已追加 / 就地合并；false = 页满且异步刷页起不来，本条目被丢弃。
 *   实现要点：
 *     · 页满（current_entry 达 JOURNAL_ENTRIES_PER_PAGE）起**异步**刷页（journal_freeze）。
 *     · 异步刷页起不来（job 忙 / 命令池空 / 建链失败）时分两种上下文：
 *       - **不在**派发里（GC/WL 的批量搬移、TRIM、开机）⇒ 同步回退 journal_flush_page
 *         ——它们随后本来就要这个屏障，且这些上下文阻塞是既有语义；
 *       - **在**派发里（host 写完成回调）⇒ **绝不**同步回退（那会嵌套完成泵，正是本
 *         阶段要消除的），改为丢弃本条 + LOGE，整页留给 journal_kick 下轮刷。
 *         该分支在 host 写路径上实际不可达：页满需 127 次 append，而单次
 *         completion_table_drain 最多处理 CQ 深度（64）条完成。
 * -------------------------------------------------------------------- */
bool journal_append(l2p_journal_t *journal, lba_t lba, nand_pba_t old_pba, nand_pba_t new_pba)
{
    /* 前置条件（判据 2）：调用者一律传 ftl->journal。 */
    ASSERT(journal != NULL);

    /*
     * P1-6 更新合并：当前页缓冲里已有同一 LBA 的条目 ⇒ 就地更新，不新增条目。
     * 回放按页序 + 页内下标应用（per-LBA last-write-wins），页内同 LBA 只保留最新
     * new_pba 即正确；old_pba 仅首条保留（回放不读 old_pba）。
     * ⚠ **不**消耗 sequence、**不**推进 current_entry：hdr_page_seq 由
     * `sequence_counter − current_entry` 反推本页首条序号，二者必须同步推进，
     * 否则页序错乱会导致回放顺序错误。
     * ⚠ 合并放在「页满守卫」之前：合并**不占新槽位**，页满时仍应更新最新映射。
     */
    l2p_journal_entry_t *entry;
    for (uint16_t i = 0U; i < journal->current_entry; i++)
    {
        entry = (l2p_journal_entry_t *)(journal->page_buf + JOURNAL_HEADER_SIZE +
                                        sizeof(l2p_journal_entry_t) * i);
        /* CP 边界之前的条目不得被合并：merge_floor 之后的覆盖写另起一条，
         * 否则 sequence 仍是旧的、new_pba 却是新的，块元数据 delta 回放会漏记。 */
        if ((entry->lba == (uint32_t)lba) && (entry->sequence >= journal->merge_floor))
        {
            entry->new_pba = new_pba; /* 保留首条 old_pba，更新到最新 new_pba */
            return true;
        }
    }

    /* 页已满且上一轮异步刷页未成功（job 忙 / 池空）⇒ 本条无处可放，丢弃。
     * ⚠ 守卫必须在按 current_entry 取槽位**之前**：current_entry ==
     *   JOURNAL_ENTRIES_PER_PAGE 时 `JOURNAL_HEADER_SIZE + 127×16 = 2060 > 2048`
     *   会写出 page_buf 之外（踩到 journal_job_t）。
     * 丢弃的后果与「掉电丢最后一页 journal」同级（回放少应用一次映射更新）；
     * 整页由 journal_kick 下轮刷掉后自动恢复。 */
    if (journal->current_entry >= JOURNAL_ENTRIES_PER_PAGE)
    {
        LOGE("[JRN] page full & async flush pending — 丢弃条目 lba=%lu\r\n", (unsigned long)lba);
        return false;
    }

    entry = (l2p_journal_entry_t *)(journal->page_buf + JOURNAL_HEADER_SIZE +
                                    sizeof(l2p_journal_entry_t) * journal->current_entry);

    entry->lba      = (uint32_t)lba;
    entry->old_pba  = old_pba;
    entry->new_pba  = new_pba;
    entry->sequence = journal->sequence_counter++;

    journal->current_entry++;

    if (journal->current_entry >= JOURNAL_ENTRIES_PER_PAGE)
    {
        if ((!journal_freeze(journal)) && (!ftl_op_in_dispatch()))
        {
            /* 非派发上下文（GC/WL/TRIM/开机）⇒ 同步回退 = 阶段 2 之前的行为。
             * 页缓冲此时仍是满的、写指针未动，journal_flush_page 会先排空在飞 job
             * 再同步刷这一页。 */
            journal_flush_page(journal);
        }
        /* 派发上下文里 freeze 失败：什么都不做——上面那条守卫会让后续 append 丢弃
         * 并 LOGE，整页由 journal_kick（ftl_service）下轮刷掉。 */
    }

    return true;
}

/* --------------------------------------------------------------------
 * journal_write_header
 *   功能：把页头写进 page_buf（magic + page_seq + entry_count + block_seq）。
 *   参数：journal —— Journal。
 *   返回：无。
 *   实现要点：hdr_page_seq 由 `sequence_counter − current_entry` **反推**本页首条序号
 *             （与 journal_append 的就地合并约定耦合，二者必须同步）。
 *             memcpy 写入以避免 alignment 警告。
 * -------------------------------------------------------------------- */
static void journal_write_header(l2p_journal_t *journal)
{
    uint32_t magic           = JOURNAL_MAGIC;
    uint32_t hdr_page_seq    = journal->sequence_counter - (uint32_t)journal->current_entry;
    uint16_t hdr_entry_count = (uint16_t)journal->current_entry;
    uint16_t hdr_block_seq   = (uint16_t)journal->current_block;

    memcpy(journal->page_buf + 0, &magic, sizeof(magic));
    memcpy(journal->page_buf + 4, &hdr_page_seq, sizeof(hdr_page_seq));
    memcpy(journal->page_buf + 8, &hdr_entry_count, sizeof(hdr_entry_count));
    memcpy(journal->page_buf + 10, &hdr_block_seq, sizeof(hdr_block_seq));
}

/* journal_clear_page — 复位页缓冲为擦除态并清条目计数（准备下一页）。 */
static void journal_clear_page(l2p_journal_t *journal)
{
    journal->current_entry = 0U;
    memset(journal->page_buf, 0xFF, sizeof(journal->page_buf));
}

/* --------------------------------------------------------------------
 * journal_freeze
 *   功能：冻结当前页——起一条**异步**页写，并算出写成功后的写指针落点。
 *   参数：journal —— Journal。
 *   返回：true = 已异步起写（调用者不必再同步刷）；false = 未能起（job 忙 / 池空 / 建链失败），
 *         此时 page_buf 与写指针**原样未动**，调用者应走同步回退。
 *   实现要点：
 *     · **单飞行**：phase != JRN_IDLE 直接拒绝——两条写会争同一个页地址。
 *     · **待偿擦除也不许写**：need_erase 置位说明当前环形块**还没擦过**（换块后
 *       擦命令没起起来），此时往里写就是往没擦的块编程 ⇒ 同样拒绝。
 *       （journal_kick 会先补擦；journal_append 的回退路径由 journal_flush_now 补擦。）
 *     · 页写成功后指针才推进（推进点在 ftl_op_journal），故落点先算好存进 job；
 *       跨块时一并算出 need_erase（先擦后写）。
 *     · 数据在 ftl_async_write_page 内**已拷进 FC buffer 池** ⇒ 提交后立刻腾页缓冲，
 *       不需要 ping-pong 快照缓冲。
 *     · pre_erased_block 的消费放在**提交成功之后**：提交失败时记账不动，下轮重算。
 * -------------------------------------------------------------------- */
static bool journal_freeze(l2p_journal_t *journal)
{
    if ((journal->job.phase != JRN_IDLE) || (journal->job.need_erase != 0U))
    {
        return false;
    }

    journal_write_header(journal);

    nand_pba_t waddr = journal_page_addr(journal, (uint8_t)journal->current_block, journal->current_page);

    fc_command_t *cmd;
    if (ftl_async_write_page(&cmd, waddr, journal->page_buf, JOURNAL_PAGE_SIZE, FTL_PAGE_CFG_META()) != FTL_OK)
    {
        return false; /* 命令池空 / 建链失败 ⇒ 同步回退 */
    }

    /* 落点：写成功后指针推进到 (target_block, target_page)；跨块 ⇒ 需擦新块 */
    uint8_t tpage      = (uint8_t)(journal->current_page + 1U);
    uint8_t tblock     = journal->current_block;
    uint8_t need_erase = 0U;
    if (tpage >= PAGES_PER_BLOCK)
    {
        tpage      = 0U;
        tblock     = (uint8_t)((tblock + 1U) % JOURNAL_BLOCK_COUNT);
        need_erase = 1U;
    }

    journal->job.from_block   = journal->current_block;
    journal->job.from_page    = journal->current_page;
    journal->job.target_block = tblock;
    journal->job.target_page  = tpage;
    journal->job.waddr        = waddr;

    if (ftl_op_submit(cmd, FTL_OP_JOURNAL, journal) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return false;
    }

    /* 提交成功才消费预擦除记账：该块已被 journal_truncate 预擦过 ⇒ 回切时跳过擦除
     * （同一块每圈只擦一次，见 journal_truncate 的 P1-6 说明）。 */
    if ((need_erase != 0U) && (journal->pre_erased_block == tblock))
    {
        journal->pre_erased_block = JOURNAL_BLOCK_NONE;
        need_erase                = 0U;
    }
    journal->job.need_erase = need_erase;
    journal->job.phase      = JRN_WRITE;

    journal_clear_page(journal);
    return true;
}

/* --------------------------------------------------------------------
 * journal_start_erase
 *   功能：为 job.need_erase 起的**异步**换块擦除。
 *   参数：journal —— Journal（job.target_block = 待擦块）。
 *   返回：true = 已起（phase = JRN_ERASE，need_erase 已清）；false = 未起（池空 / 建链失败，
 *         need_erase 保留，由 journal_kick 下轮重试）。
 *   实现要点：擦的是**已推进到**的新环形块，故必须在任何页写进该块之前完成。
 * -------------------------------------------------------------------- */
static bool journal_start_erase(l2p_journal_t *journal)
{
    fc_command_t *cmd;
    if (ftl_async_erase_block_multi_plane(&cmd, journal_block_addr(journal, journal->job.target_block)) != FTL_OK)
    {
        return false;
    }
    if (ftl_op_submit(cmd, FTL_OP_JOURNAL, journal) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return false;
    }

    journal->job.need_erase = 0U;
    journal->job.phase      = JRN_ERASE;
    return true;
}

/* --------------------------------------------------------------------
 * journal_kick
 *   功能：后台推进 Journal——把待写的整页 / 待起的擦除交给异步 job。
 *   参数：journal —— Journal（可为 NULL）。
 *   返回：无。
 *   实现要点：由 ftl_service_idle/normal 每轮调用。job 已在飞 ⇒ 空操作（单飞行）。
 *             有未起的擦除时**优先补擦**：新环形块没擦过就不能往里写。
 * -------------------------------------------------------------------- */
void journal_kick(l2p_journal_t *journal)
{
    /* 前置条件（判据 2）：调用者一律传 ftl->journal。 */
    ASSERT(journal != NULL);

    if (journal->job.phase != JRN_IDLE)
    {
        /* 心跳（见 ftl_job.h）：在飞期间每满 1s 打一行，异步后「卡在哪个 phase」的唯一观测口 */
        if (ftl_job_heartbeat(&journal->job.log_ms, &journal->job.start_ms))
        {
            LOGI("[JRN] job phase=%u waddr=0x%08lx age=%lums\r\n",
                 (unsigned)journal->job.phase,
                 (unsigned long)journal->job.waddr,
                 (unsigned long)ftl_job_age_ms(&journal->job.start_ms));
        }
        return; /* 单飞行：job 已在飞 */
    }

    journal->job.log_ms   = 0U;
    journal->job.start_ms = 0U;

    if (journal->job.need_erase != 0U)
    {
        (void)journal_start_erase(journal);
        return;
    }

    if (journal->current_entry == 0U)
    {
        return; /* 无待写数据 */
    }

    (void)journal_freeze(journal);
}

/* --------------------------------------------------------------------
 * journal_drain
 *   功能：自旋把在飞 journal job（写 / 擦）泵到 IDLE。
 *   参数：journal —— Journal。
 *   返回：true = 已空闲；false = 自旋到上限仍未空闲（命令丢失 / NAND 卡死）。
 *   实现要点：非目标 completion 由 drain 按各自 op_tag 派发，本函数无需处理。
 *             ⚠ 这是一个**嵌套泵**，只允许在同步上下文（journal_flush_page 的屏障语义、
 *             GC/WL 的批量 append 回退）里用；host 写完成回调路径不会走到它。
 * -------------------------------------------------------------------- */
static bool journal_drain(l2p_journal_t *journal)
{
    uint32_t spin = 0U;

    while ((journal->job.phase != JRN_IDLE) && (spin < JOURNAL_DRAIN_SPIN_MAX))
    {
        (void)fc_service();
        completion_table_drain();
        spin++;
    }

    return (journal->job.phase == JRN_IDLE);
}

/* 前置声明：同步写/擦失败也要置持久性 pending（定义在完成服务小节）。 */
static void journal_note_write_failure(l2p_journal_t *journal);

/* --------------------------------------------------------------------
 * journal_erase_pending_sync
 *   功能：**同步**偿还未起的换块擦除（job.need_erase 置位时的 job.target_block）。
 *   参数：journal —— Journal。
 *   返回：无。
 *   实现要点：先查 pre_erased_block——journal_truncate 可能在「页写完成 → 起擦除」
 *             这段窗口里已把该块预擦掉（它擦的正是环形下一块）。此时消费记账跳过，
 *             避免同一块被擦两次（§14.1 的 Journal 寿命瓶颈就是这么来的）。
 * -------------------------------------------------------------------- */
static void journal_erase_pending_sync(l2p_journal_t *journal)
{
    if (journal->pre_erased_block == journal->job.target_block)
    {
        journal->pre_erased_block = JOURNAL_BLOCK_NONE; /* truncate 已代擦，记账消费 */
    }
    else
    {
        const ftl_status_t est = ftl_sync_erase_block_multi_plane(journal_block_addr(journal, journal->job.target_block));

        if (est != FTL_OK)
        {
            /* 擦失败 ⇒ 保留 need_erase，绝不清账；flusher 看到它仍置位就不会写未擦块。 */
            LOGE("[JRN] sync ring erase failed blk=%u status=%d\r\n",
                 (unsigned)journal->job.target_block,
                 (int)est);
            journal_note_write_failure(journal);
            return;
        }
    }
    journal->job.need_erase = 0U;
}

/* --------------------------------------------------------------------
 * journal_flush_now
 *   功能：**同步**把当前页缓冲连同页头写进 NAND 当前页，并推进写指针。
 *   参数：journal —— Journal。
 *   返回：无（空页直接返回）。
 *   实现要点（= 阶段 2 之前 journal_flush_page 的主体，逐字保留）：
 *     · 先偿还未起的换块擦除（job.need_erase 待偿时，目标块 = 已推进到的 current_block）。
 *     · 页满换块：本函数是**唯一**的同步环形块擦除点；若该块已被 journal_truncate
 *       预擦除（pre_erased_block），**跳过擦除**（否则同块每圈擦两次，+25% 磨损）。
 *     · 写后清空页缓冲（0xFF）与 current_entry，准备下一页。
 *     · ⚠ 写失败：置 `write_fail_pending`（阻止 GC 擦 victim）+ LOGE，但仍推进指针
 *       ——已写过的页（可能半编程）不可复用。同步/异步两条路径在这一点上一致：
 *       同步在写后推进；异步在失败完成时推进到 freeze 算好的下一落点。
 * -------------------------------------------------------------------- */
static void journal_flush_now(l2p_journal_t *journal)
{
    if (journal->job.need_erase != 0U)
    {
        journal_erase_pending_sync(journal);

        if (journal->job.need_erase != 0U)
        {
            /* 目标块仍未擦：不能把页写进去（二次编程/脏块写 = 静默损坏）。
             * 页缓冲保留，下次 flush 重试；pending 已置位，GC 不会擦 victim。 */
            return;
        }
    }

    if (journal->current_entry == 0U)
    {
        return; /* 无数据，无需 flush */
    }

    journal_write_header(journal);

    uint8_t    target_page = journal->current_page;
    nand_pba_t waddr       = journal_page_addr(journal, (uint8_t)journal->current_block, target_page);

    /*
     * 同步写 Journal 页到 NAND。
     * Random DISABLED / CRC ENABLED（v0.62）：Journal 有 FC per-CW CRC + ECC，
     * 外加自身结构判据（magic/entry_count/sequence）。
     */
    const ftl_status_t wst = ftl_sync_write_page(waddr, journal->page_buf, JOURNAL_PAGE_SIZE, FTL_PAGE_CFG_META());

    if (wst != FTL_OK)
    {
        LOGE("[JRN] sync page write failed pba=%08lx status=%d — 条目丢失，置 pending\r\n",
             (unsigned long)waddr,
             (int)wst);
        journal_note_write_failure(journal);
    }

    /* 推进指针 */
    journal->current_page++;
    if (journal->current_page >= PAGES_PER_BLOCK)
    {
        /* 当前 Block 写满，切到下一 Block（环形，共 JOURNAL_BLOCK_COUNT 块）*/
        journal->current_block = (uint8_t)((journal->current_block + 1U) % JOURNAL_BLOCK_COUNT);
        journal->current_page  = 0U;

        /* 回切擦除（同步路径的环形块擦除点）。
         * P1-6：若该块已被 journal_truncate 预擦除（水位截断），跳过——否则同一块每圈
         * 被擦两次，把 §14.1 认定的 Journal 寿命瓶颈又放大 ~25%。 */
        if (journal->pre_erased_block == journal->current_block)
        {
            journal->pre_erased_block = JOURNAL_BLOCK_NONE; /* 记账消费掉：本块即将被写 */
        }
        else
        {
            const ftl_status_t est = ftl_sync_erase_block_multi_plane(journal_block_addr(journal, (uint8_t)journal->current_block));

            if (est != FTL_OK)
            {
                /* 新环形块没擦成功 ⇒ 重新挂起 need_erase：下一次 flush 先重试擦除，
                 * 仍失败就不写（绝不能写未擦块）。 */
                LOGE("[JRN] sync ring advance erase failed blk=%u status=%d — 挂起\r\n",
                     (unsigned)journal->current_block,
                     (int)est);
                journal->job.target_block = journal->current_block;
                journal->job.need_erase   = 1U;
                journal_note_write_failure(journal);
            }
        }
    }

    journal_clear_page(journal);
}

/* --------------------------------------------------------------------
 * journal_flush_page
 *   功能：**同步**把 Journal 落盘（屏障语义）——先排空在飞 job，再刷当前页。
 *   参数：journal —— Journal。
 *   返回：无。
 *   实现要点：调用者拿它当屏障——GC/WL 的 F10 契约（搬移条目先落盘再擦 victim）、
 *             HIL `fl`（已持久化映射落盘）。host 写路径的常规刷页走 journal_kick。
 *             ⚠ drain 超时（命令丢失 / NAND 卡死）时**不静默**：LOGE 后仍走同步刷，
 *             因为 job 的指针推进有 from_* 守卫（见 ftl_op_journal），不会造成回退。
 *             ⚠ 阶段 4 起 host 写路径的紧急 GC 已改为**背压**（ftl_wbuf_flush 起 GC job
 *             + 返 FTL_AGAIN），不再在完成回调里跑这个 drain；搬移 job 的 F10 屏障也
 *             改走 journal_kick（异步），不再调本函数。故本函数的**唯一**调用者只剩
 *             结构性同步上下文（HIL `fl`、开机路径、CLI）。
 *             journal_append 自己的同步回退已由 ftl_op_in_dispatch 挡住，不会新增。
 * -------------------------------------------------------------------- */
void journal_flush_page(l2p_journal_t *journal)
{
    /* 前置条件（判据 2）：调用者一律传 ftl->journal。 */
    ASSERT(journal != NULL);

    if (!journal_drain(journal))
    {
        LOGE("[JRN] drain timeout phase=%u — 屏障语义降级，仍走同步刷\r\n", (unsigned)journal->job.phase);
    }

    journal_flush_now(journal);
}

/* ══════════════════════════════════════════════════════════
 * 4. 完成服务（g_op_service[FTL_OP_JOURNAL]）
 * ══════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * ftl_op_journal
 *   功能：Journal 刷页 job 的完成服务函数——按 job.phase 推进状态机。
 *   参数：ftl —— 实例（**不用**：ctx 已是本实例的 journal）；completion —— 完成状态；
 *         ctx —— `l2p_journal_t *`（ftl_op_submit 时传 ftl->journal）。
 *   返回：无。命令的 release 归本函数（见 ftl_op.h 的所有权表）。
 *   实现要点：
 *     · JRN_WRITE 成功 ⇒ 推进写指针（守卫见下）；跨块 ⇒ 起异步擦除，phase = JRN_ERASE。
 *     · JRN_WRITE 失败 ⇒ **推进到失败页之后**（失败页可能半编程，绝不复用；
 *       跨块时保留 need_erase 由 journal_kick 先擦）。条目已丢，置
 *       write_fail_pending + 强制 CP，阻止 GC 在 L2P 落盘前擦 victim。
 *     · JRN_ERASE ⇒ 记 LOGE（环形块擦失败 ⇒ 该块后续写入不可信），phase = JRN_IDLE。
 *     · **指针推进守卫**：drain 超时后 journal_flush_now 可能已把指针推过落点，
 *       此时 job 的落点已过期 ⇒ 只在「指针仍在起写位置」时推进。
 * -------------------------------------------------------------------- */
/* 记录一次 Journal 页写失败：条目已随 page_buf 清空而丢失。
 * 置 pending 阻止任何 victim 擦除，并强制一次 Checkpoint 把当前 L2P 落盘；
 * pending 由 journal_clear_write_failure（CP Header 成功后）清除。 */
static void journal_note_write_failure(l2p_journal_t *journal)
{
    journal->write_fail_count++;
    journal->write_fail_seq     = journal->sequence_counter;
    journal->write_fail_pending = 1U;
    journal->checkpoint_needed  = 1U;
    journal->emergency          = 1U;
}

void ftl_op_journal(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx)
{
    l2p_journal_t *journal = (l2p_journal_t *)ctx;

    (void)ftl; /* 实例无关：ctx 就是本实例的 journal */

    ASSERT(journal != NULL);
    ASSERT(completion != NULL);

    fc_command_t *cmd = fc_command_id_to_command((uint16_t)completion->command_id);

    if (journal->job.phase == JRN_WRITE)
    {
        if ((completion->status & FC_CW_STATUS_ERROR_MASK) != 0U)
        {
            LOGE("[JRN] page write failed pba=%08lx status=0x%02x — 跳过失败页\r\n",
                 (unsigned long)journal->job.waddr,
                 (unsigned)completion->status);
            journal_note_write_failure(journal);

            /* 失败页可能已半编程 ⇒ **绝不复用**。推进到 freeze 算好的下一落点；
             * 跨块时 need_erase 保留，由 journal_kick 先擦目标块再写。
             * 条目已丢（page_buf 在提交时清空），由 write_fail_pending 兜住：
             * GC 不会擦 victim，直到 CP 把当前 L2P 落盘。 */
            if ((journal->current_block == journal->job.from_block) &&
                (journal->current_page == journal->job.from_page))
            {
                journal->current_block = journal->job.target_block;
                journal->current_page  = journal->job.target_page;
            }
            journal->job.phase = JRN_IDLE;
            (void)fc_command_release(cmd);
            return;
        }

        if ((journal->current_block == journal->job.from_block) && (journal->current_page == journal->job.from_page))
        {
            journal->current_block = journal->job.target_block;
            journal->current_page  = journal->job.target_page;
        }
        journal->job.phase = JRN_IDLE;

        /* 换块 ⇒ 起异步擦除；起不来就留着 need_erase，由 journal_kick 下轮重试。
         * ⚠ 先查 pre_erased_block：journal_truncate 可能已在这段窗口里代擦了环形
         *   下一块（正是本 job.target_block）⇒ 消费记账跳过，避免重复擦。 */
        if (journal->job.need_erase != 0U)
        {
            if (journal->pre_erased_block == journal->job.target_block)
            {
                journal->pre_erased_block = JOURNAL_BLOCK_NONE;
                journal->job.need_erase   = 0U;
            }
            else if (!journal_start_erase(journal))
            {
                LOGE("[JRN] erase kick failed blk=%u — 下轮重试\r\n", (unsigned)journal->job.target_block);
            }
        }

        (void)fc_command_release(cmd);
        return;
    }

    if (journal->job.phase == JRN_ERASE)
    {
        if ((completion->status & FC_CW_STATUS_NAND_FAIL) != 0U)
        {
            LOGE("[JRN] ring block erase failed blk=%u status=0x%02x — 重新挂起\r\n",
                 (unsigned)journal->job.target_block,
                 (unsigned)completion->status);
            /* 擦失败 ⇒ 目标块仍是脏的；绝不能把后续页写进去。重新挂起 need_erase，
             * 由 journal_kick 下轮重试；F10 会看到 need_erase != 0 而继续等待/中止。 */
            journal->job.need_erase = 1U;
        }
        journal->job.phase = JRN_IDLE;
        (void)fc_command_release(cmd);
        return;
    }

    /* 不该发生：一条命令对应一个 job，phase 必为 WRITE / ERASE */
    LOGE("[JRN] unexpected completion phase=%u\r\n", (unsigned)journal->job.phase);
    journal->job.phase = JRN_IDLE;
    (void)fc_command_release(cmd);
}

/* ══════════════════════════════════════════════════════════
 * 5. 回放与写指针恢复
 * ══════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * journal_scan_ring
 *   功能：扫一遍环形块池——找**最新页**（最大 page_seq），并按需收集需回放页。
 *   参数：journal —— Journal；cp_sequence —— 回放水位；
 *         out_pages / capacity —— 收集数组与其容量（out_pages == NULL = 只找最新页）；
 *         out_count —— 出参收集条数（可为 NULL）；out_latest —— 出参最新页（可为 NULL）。
 *   返回：无。
 *   实现要点：只读 NAND，**不改 journal 状态**（怎么用由调用方决定）。
 *             需回放判据是**整页**的：`page_seq + entry_count > cp_sequence`。
 *             旧判据只看页首页序号（`page_seq > cp_sequence`），跨水位的那一页会被
 *             整页跳过——它页尾的新条目（> cp_sequence）随页面一起丢。
 *             页头 magic / entry_count 校验失败即跳过（掉电撕裂的预期情形，见文件头 ⚠）。
 * -------------------------------------------------------------------- */
static void journal_scan_ring(const l2p_journal_t *journal,
                              uint32_t             cp_sequence,
                              journal_page_ref_t  *out_pages,
                              uint16_t             capacity,
                              uint16_t            *out_count,
                              journal_page_ref_t  *out_latest)
{
    if (out_latest != NULL)
    {
        out_latest->page_seq    = 0U;
        out_latest->block       = 0U;
        out_latest->page        = 0U;
        out_latest->entry_count = 0U;
    }

    uint16_t count = 0U;
    for (uint8_t b = 0U; b < JOURNAL_BLOCK_COUNT; b++)
    {
        for (uint8_t page = 0U; page < PAGES_PER_BLOCK; page++)
        {
            uint8_t page_data[JOURNAL_PAGE_SIZE];
            memset(page_data, 0xFF, sizeof(page_data));

            nand_pba_t raddr = journal_page_addr(journal, b, (uint8_t)page);
            if (ftl_sync_read_page(raddr, page_data, JOURNAL_PAGE_SIZE, FTL_PAGE_CFG_META()) !=
                0)
            {
                continue; /* 读失败，跳过本页 */
            }

            {
                uint32_t magic;
                memcpy(&magic, page_data + 0, sizeof(magic));
                if (magic != JOURNAL_MAGIC)
                {
                    continue; /* 非 Journal 页 */
                }
            }

            uint32_t page_seq;
            uint16_t entry_count;
            memcpy(&page_seq, page_data + 4, sizeof(page_seq));
            memcpy(&entry_count, page_data + 8, sizeof(entry_count));
            if ((entry_count == 0U) || (entry_count > JOURNAL_ENTRIES_PER_PAGE))
            {
                continue; /* 空页 / 非法 count */
            }

            /* 最新页（用于恢复写指针与序号）：取 page_seq 最大者 */
            if ((out_latest != NULL) && (page_seq >= out_latest->page_seq))
            {
                out_latest->page_seq    = page_seq;
                out_latest->block       = b;
                out_latest->page        = (uint8_t)page;
                out_latest->entry_count = (uint8_t)entry_count;
            }

            /* 需回放页：本页**最后一条**的序号已过水位 ⇒ 页内至少一条没进 L2P 表 */
            if ((out_pages != NULL) && (count < capacity) && ((page_seq + (uint32_t)entry_count) > cp_sequence))
            {
                out_pages[count].page_seq    = page_seq;
                out_pages[count].block       = b;
                out_pages[count].page        = (uint8_t)page;
                out_pages[count].entry_count = (uint8_t)entry_count;
                count++;
            }
        }
    }

    if (out_count != NULL)
    {
        *out_count = count;
    }
}

/* --------------------------------------------------------------------
 * journal_scan_latest
 *   功能：只读扫环取最新页（journal_scan_ring 的「只找最新页」外壳）。
 *   参数：journal —— Journal；out —— 出参（环空时全置 0，可为 NULL）。
 *   返回：true = 环非空；false = 环空。
 * -------------------------------------------------------------------- */
bool journal_scan_latest(const l2p_journal_t *journal, journal_page_ref_t *out)
{
    /* 前置条件（判据 2）：调用者一律传 ftl->journal。out 可为 NULL（文档化契约）。 */
    ASSERT(journal != NULL);

    journal_scan_ring(journal, 0U, NULL, 0U, NULL, out);

    return (out != NULL) && (out->page_seq > 0U);
}

/* --------------------------------------------------------------------
 * journal_resume
 *   功能：恢复 Journal 的 RAM 写态（写指针 + 序号），**不回放条目**。
 *   参数：journal —— Journal；latest —— 最新页（NULL 或 page_seq==0 视为环空）；
 *         floor_sequence —— 序号下限（Checkpoint 的 journal_sequence）。
 *   返回：无。
 *   实现要点：① 落点是最新页**之后一页**——最新页已编程，续填会在页满时对同一页
 *                二次编程（NAND 只能 1→0 ⇒ 页头 entry_count 被 AND 成错值）；
 *             ② 跨块时先擦下一块（那里还留着上一圈的旧页）；**不记账
 *                pre_erased_block**——那块马上会被写满，下一圈回切时仍需按常规擦一次；
 *             ③ 序号取 max(最新页序号 + 1, floor)，0 抬到 1（0 保留作「无序号」，
 *                与 journal_init 同约定；否则 journal_truncate 的水位校验会被 0 挡掉）。
 * -------------------------------------------------------------------- */
void journal_resume(l2p_journal_t *journal, const journal_page_ref_t *latest, uint32_t floor_sequence)
{
    /* 前置条件（判据 2）：调用者一律传 ftl->journal。latest 可为 NULL（文档化契约）。 */
    ASSERT(journal != NULL);

    uint32_t next = floor_sequence;

    if ((latest != NULL) && (latest->page_seq > 0U))
    {
        /* ⚠ 必须加 **entry_count**，不是 +1：本页占了 page_seq .. page_seq+entry_count-1
         *   共 entry_count 个序号（journal_append 逐条 `sequence_counter++`，页头
         *   `hdr_page_seq = sequence_counter − current_entry` 反推）。若只 +1，序号会
         *   退回本页内部 ⇒ 下一页的 page_seq 落进本页的序号区间，回放过滤
         *   `page_seq + entry_count > cp_sequence` 会把已进 L2P 的页**重复回放**。 */
        if ((latest->page_seq + (uint32_t)latest->entry_count) > next)
        {
            next = latest->page_seq + (uint32_t)latest->entry_count;
        }

        uint8_t next_block = latest->block;
        uint8_t next_page  = (uint8_t)(latest->page + 1U);
        if (next_page >= PAGES_PER_BLOCK)
        {
            next_block = (uint8_t)((next_block + 1U) % JOURNAL_BLOCK_COUNT);
            next_page  = 0U;
            (void)ftl_sync_erase_block_multi_plane(journal_block_addr(journal, next_block));
        }

        journal->current_block = next_block;
        journal->current_page  = next_page;
    }
    else
    {
        /* 环空：写指针留在 journal_init 的 (block0, page0) */
        journal->current_block = 0U;
        journal->current_page  = 0U;
    }

    if (next == 0U)
    {
        next = 1U;
    }

    journal->current_entry    = 0U;
    journal->sequence_counter = next;
    journal->merge_floor      = 0U;

    /* 开机没有“未完成迁移”的上下文：上次上电的失败 pending 不跨重启。 */
    journal->write_fail_count   = 0U;
    journal->write_fail_seq     = 0U;
    journal->write_fail_pending = 0U;

    /* 异步刷页 job 复位（阶段 2）：resume 是开机路径，此前不可能有在飞 job；
     * 但复位是廉价的，且能挡住「同一实例被重复 resume」的残留。 */
    journal->job.phase      = JRN_IDLE;
    journal->job.need_erase = 0U;

    memset(journal->page_buf, 0xFF, sizeof(journal->page_buf));
}

/* journal 回放的 valid_count 增量（带守卫：越界/下溢置 *ok=0，不触发 ASSERT）。
 * 只被 journal_replay 调用；materialize 已保证该块处于可记账状态。 */
static void jrn_add_valid_guarded(ftl_context_t *ftl, nand_pba_t pba, uint8_t *ok)
{
    ASSERT(ftl != NULL);
    ASSERT(ok != NULL);

    const uint16_t g = ftl_pba_gblock(pba);

    if (g >= FTL_BLOCK_TOTAL)
    {
        *ok = 0U;
        return;
    }

    ftl_block_t *b = &ftl->alloc.blocks[g];

    if (ftl_block_is_bad(b))
    {
        return;
    }
    if (b->valid_count >= (uint16_t)FTL_CW_PER_BLOCK)
    {
        *ok = 0U;
        return;
    }
    ftl_block_add_valid(b);
}

static void jrn_sub_valid_guarded(ftl_context_t *ftl, nand_pba_t pba, uint8_t *ok)
{
    ASSERT(ftl != NULL);
    ASSERT(ok != NULL);

    const uint16_t g = ftl_pba_gblock(pba);

    if (g >= FTL_BLOCK_TOTAL)
    {
        *ok = 0U;
        return;
    }

    ftl_block_t *b = &ftl->alloc.blocks[g];

    if (ftl_block_is_bad(b))
    {
        return;
    }
    if (b->valid_count == 0U)
    {
        *ok = 0U;
        return;
    }
    ftl_block_sub_valid(b);
}

/* --------------------------------------------------------------------
 * journal_replay
 *   功能：开机回放——扫 Journal 块池，把**未进 L2P 表**的页按
 *         (page_seq, entry_index) 递增序应用到 L2P cache，末尾恢复写指针。
 *   参数：journal —— Journal；cache —— 目标 L2P cache；cp_sequence —— checkpoint
 *         截断水位（≤ 该序号的条目已进 NAND L2P 表，无需回放）。
 *   返回：FTL_OK 成功；FTL_ERROR 入参为 NULL。
 *   实现要点：
 *     · 第一遍（journal_scan_ring）收集「需回放页」进栈上数组 replay_pages[]
 *       （JOURNAL_BLOCK_COUNT 块 × PAGES_PER_BLOCK 页，共 256 项 8B = 2KB；
 *       ⚠ 256 > 255 ⇒ 计数/下标必须 uint16_t，uint8_t 会死循环），
 *       同时取到最新页用于恢复写指针。
 *     · 第二遍插入排序（≤256 项）后按序应用 ⇒ per-LBA last-write-wins，同 LBA 的旧
 *       条目（含 GC/WL 搬移记录）被后写覆盖，不指向已擦除块。
 *     · 末尾 journal_resume 把写指针落到最新页之后并接续序号。
 *     · out_entries 在**应用循环里**逐页累加 entry_count（读失败的页不计）——
 *       不能用 journal 的 RAM 写指针反推：journal_resume 会清 current_entry。
 * -------------------------------------------------------------------- */
ftl_status_t journal_replay(l2p_journal_t              *journal,
                            l2p_cache_t                *cache,
                            uint32_t                    cp_sequence,
                            uint32_t                   *out_entries,
                            journal_replay_block_ctx_t *block_ctx)
{
    /* JOURNAL_BLOCK_COUNT 块 × 每块 PAGES_PER_BLOCK 页 = 256 页；每项 8B → 2KB 栈上数组。 */
    journal_page_ref_t replay_pages[JOURNAL_BLOCK_COUNT * PAGES_PER_BLOCK];

    /* 前置条件（判据 2）：调用者传 ftl->journal 与共享 cache（ftl->cache）。 */
    ASSERT(journal != NULL);
    ASSERT(cache != NULL);

    /* 第一遍：扫环——收集需回放页 + 取最新页 */
    journal_page_ref_t latest;
    uint16_t           replay_count = 0U;
    journal_scan_ring(journal,
                      cp_sequence,
                      replay_pages,
                      (uint16_t)(JOURNAL_BLOCK_COUNT * PAGES_PER_BLOCK),
                      &replay_count,
                      &latest);

    /* 第二遍：按 (page_seq, entry_index) 递增序应用条目。
     * 插入排序（replay_count ≤ 256）：per-LBA last-write-wins——
     * 同 LBA 的旧条目（含 GC/WL 搬移记录）被后写覆盖，不指向已擦除块。 */
    uint32_t applied = 0U;
    {
        for (uint16_t i = 1U; i < replay_count; i++)
        {
            journal_page_ref_t key = replay_pages[i];

            uint16_t n = i;
            while ((n > 0U) && (replay_pages[n - 1U].page_seq > key.page_seq))
            {
                replay_pages[n] = replay_pages[n - 1U];
                n--;
            }
            replay_pages[n] = key;
        }

        for (uint16_t i = 0U; i < replay_count; i++)
        {
            nand_pba_t raddr = journal_page_addr(journal, replay_pages[i].block, replay_pages[i].page);

            uint8_t page_data[JOURNAL_PAGE_SIZE];
            memset(page_data, 0xFF, sizeof(page_data));

            ftl_status_t ret = ftl_sync_read_page(raddr, page_data, JOURNAL_PAGE_SIZE, FTL_PAGE_CFG_META());
            if (ret != FTL_OK)
            {
                continue; /* 页已失效，跳过（应用顺序其余页不受影响） */
            }

            applied += (uint32_t)replay_pages[i].entry_count;

            {
                for (uint16_t k = 0U; k < replay_pages[i].entry_count; k++)
                {
                    uint16_t offset = JOURNAL_HEADER_SIZE + sizeof(l2p_journal_entry_t) * k;

                    uint32_t entry_lba;
                    uint32_t entry_old_pba;
                    uint32_t entry_new_pba;
                    uint32_t entry_seq;
                    memcpy(&entry_lba, page_data + offset + 0, 4);
                    memcpy(&entry_old_pba, page_data + offset + 4, 4);
                    memcpy(&entry_new_pba, page_data + offset + 8, 4);
                    memcpy(&entry_seq, page_data + offset + 12, 4);
                    /* ⚠ 开机回放路径 = 结构性同步：用**同步**逃生口（自旋泵），
                     * 不 park、不返 FTL_AGAIN——此时 superloop 尚未建立，没人推进 job。
                     * ⚠ entry_lba 来自 NAND，是**不可信输入**：先校验再查表（H29）。 */
                    if ((entry_lba != (uint32_t)FTL_LBA_INVALID) && ((uint64_t)entry_lba < L2P_GLOBAL_LBA_MAX) &&
                        lba_routes_to((lba_t)entry_lba, journal->topo))
                    {
                        (void)l2p_cache_write_sync(cache,
                                                   (lba_t)entry_lba,
                                                   (nand_pba_t)entry_new_pba);
                    }

                    /* 块元数据增量：只对**快照之后**的条目记 valid_count。
                     * 整页回放（含水位之前的条目）是幂等的映射写，不能整页记增量。
                     * materialize 由开机恢复模块注入——journal 不反向依赖 pfr。 */
                    if ((block_ctx != NULL) && (block_ctx->ctx != NULL) && (entry_seq >= cp_sequence))
                    {
                        ftl_context_t   *ftl     = (ftl_context_t *)block_ctx->ctx;
                        const nand_pba_t old_pba = (nand_pba_t)entry_old_pba;
                        const nand_pba_t new_pba = (nand_pba_t)entry_new_pba;

                        if (new_pba != old_pba)
                        {
                            if (new_pba != NAND_PBA_INVALID)
                            {
                                if (block_ctx->materialize(block_ctx->ctx, new_pba))
                                {
                                    jrn_add_valid_guarded(ftl, new_pba, &block_ctx->ok);
                                }
                                else
                                {
                                    block_ctx->ok = 0U;
                                }
                            }
                            if (old_pba != NAND_PBA_INVALID)
                            {
                                if (block_ctx->materialize(block_ctx->ctx, old_pba))
                                {
                                    jrn_sub_valid_guarded(ftl, old_pba, &block_ctx->ok);
                                }
                                else
                                {
                                    block_ctx->ok = 0U;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    /* 恢复写指针与序号（落到最新页之后；floor 兜住「环空 / 序号回退」） */
    journal_resume(journal, &latest, cp_sequence);

    if (out_entries != NULL)
    {
        *out_entries = applied;
    }

    return FTL_OK;
}

/* ══════════════════════════════════════════════════════════
 * 6. 截断
 * ══════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * journal_truncate_plan
 *   功能：截断 Journal 的第一步——水位校验 + 算出待预擦的环形块（纯 RAM，不动介质）。
 *   参数：journal —— Journal；up_to_sequence —— 截断水位（CP 时刻的 sequence_counter）；
 *         out_addr / out_block —— 出参：待预擦块的基地址 / 块号。
 *   返回：JRN_TRUNC_NEEDS_ERASE = 需擦 *out_addr；JRN_TRUNC_ALREADY = 无需擦（水位 OK，该块已预擦过）；
 *         JRN_TRUNC_NO_TRUNCATE = 不截断（水位不满足）——**调用者不得据此复位水位标志**。
 *   实现要点：
 *     ① 水位校验：若水位不足以覆盖全部已写条目，**不截断**（最旧块可能仍持有回放
 *        需要的条目，擦掉即丢数据）；sequence_counter == 0 一并挡掉避免下溢。
 *     ② 待擦块 = 环形下一块；已预擦过（pre_erased_block）⇒ 无需重擦。
 * -------------------------------------------------------------------- */
jrn_truncate_step_t journal_truncate_plan(l2p_journal_t *journal, uint32_t up_to_sequence, nand_pba_t *out_addr, uint8_t *out_block)
{
    /* 前置条件（判据 2）：三个指针均由调用者自持。 */
    ASSERT(journal != NULL);
    ASSERT(out_addr != NULL);
    ASSERT(out_block != NULL);

    /*
     * ① 水位校验（P1-6：up_to_sequence 不再是死形参）。
     * 调用者传的是 CP **起点**的 sequence_counter（下一个待分配序号）。若水位**不足以**
     * 覆盖全部已写条目（CP 期间 journal 又前进了 ≥2 条），说明有未 checkpoint 的条目，
     * 而环形里最旧的一块可能仍持有回放需要的条目 ⇒ **不截断**。
     * （sequence_counter == 0 理论上不会出现；一并挡掉，避免 -1 下溢误判。）
     * ⚠ 异步 CP 把快照点从"CP 末尾"提前到"CP 起点"（见 checkpoint.c 的掉电安全论证），
     *   故水位判据比同步实现**更容易不满足**——即更少预擦。这是安全方向的取舍：
     *   预擦只是磨损优化，跳过它只是让 journal_flush_page 自己擦（每块每圈仍只擦一次）。
     */
    if ((journal->sequence_counter == 0U) || ((up_to_sequence + 1U) < journal->sequence_counter))
    {
        return JRN_TRUNC_NO_TRUNCATE;
    }

    /* ② 待预擦块 = 环形下一块。该块整块的条目都 ≤ up_to_sequence（checkpoint 已把它们
     * 写进 NAND L2P 表），擦掉即完成截断。重复调用（两次 checkpoint 之间无写入）
     * 时 next == pre_erased_block ⇒ 跳过重擦。 */
    uint8_t next = (uint8_t)((journal->current_block + 1U) % JOURNAL_BLOCK_COUNT);
    if (next == journal->pre_erased_block)
    {
        return JRN_TRUNC_ALREADY;
    }

    *out_addr  = journal_block_addr(journal, next);
    *out_block = next;
    return JRN_TRUNC_NEEDS_ERASE;
}

/* --------------------------------------------------------------------
 * journal_truncate_commit
 *   功能：截断 Journal 的第二步——擦除完成后记账 + 复位水位标志。
 *   参数：journal —— Journal；erased_block —— 刚擦成功的环形块号，
 *         JOURNAL_BLOCK_NONE = 本次未擦（只复位水位标志）。
 *   返回：无。
 * -------------------------------------------------------------------- */
void journal_truncate_commit(l2p_journal_t *journal, uint8_t erased_block)
{
    /* 前置条件（判据 2）：调用者一律传 ftl->journal。 */
    ASSERT(journal != NULL);

    /* 记账：journal_flush_page 回切到该块时跳过擦除——旧实现在此处与 flush_page 各擦
     * 一次 = **同一块每圈擦两次**，是 Journal 磨损（§14.1 的寿命瓶颈）中凭空多出的
     * ~25%。
     * ⚠ 本次未擦（NONE）时**不动** pre_erased_block：覆写成 NONE 会让已预擦的块被
     *   flush_page 再擦一次（双倍磨损回归）。 */
    if (erased_block != JOURNAL_BLOCK_NONE)
    {
        journal->pre_erased_block = erased_block;
    }

    /* 复位水位标志（设计 §7.5.4）：截断即水位已清。与 checkpoint job 的收尾双保险
     * （后者覆盖 truncate 被弱桩替换的纯 RAM 测试构建）。
     * ⚠ 持久性失败 pending 未清时**保留**标志：说明本次 CP 早于失败点，没有把
     *   迁移后的 L2P 全部落盘，必须再触发一次 CP，而不是把水位门关掉。 */
    if (journal->write_fail_pending == 0U)
    {
        journal->emergency         = 0U;
        journal->checkpoint_needed = 0U;
    }
}

void journal_clear_write_failure(l2p_journal_t *journal, uint32_t cp_journal_seq)
{
    ASSERT(journal != NULL);

    /* CP 起点水位 ≥ 失败点 ⇒ 该 CP 在失败之后快照了脏区并把 L2P 落盘；
     * pending 可以清。否则保留，等下一次 CP。 */
    if ((journal->write_fail_pending != 0U) && (cp_journal_seq >= journal->write_fail_seq))
    {
        journal->write_fail_pending = 0U;
    }
}

/* journal_truncate（= plan + 即刻擦 + commit 的同步包装）已在阶段 6 随 Checkpoint
 * job 化删除：异步 CP 在 CP_TRUNCATE 阶段提交异步擦命令后调 journal_truncate_commit，
 * 唯一调用者消失。结构性同步上下文若将来需要，按 plan → 擦 → commit 三步自行组合。 */

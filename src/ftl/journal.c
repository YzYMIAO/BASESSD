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
 * 写模型：
 *   · 追加进 RAM 页缓冲（page_buf）；页满（JOURNAL_ENTRIES_PER_PAGE）时**不再同步落盘**，
 *     改为交给异步 job（journal_freeze → ftl_op_submit(FTL_OP_JOURNAL)），完成由
 *     g_op_callbacks[FTL_OP_JOURNAL]（ftl_journal_callback）按 job.phase 推进；
 *   · 块满换环形下一块——**先擦后写**，换块擦除也是 job 的一个 phase（JRN_ERASE）；
 *   · 同 LBA 就地合并（不消耗 sequence）。
 *
 * 为什么需要屏障时走 journal_flush_page：GC/WL 的 F10 契约要求「搬移条目先落盘再擦
 *   victim」，HIL `fl` 要求「已持久化映射落盘」。这两处**必须**同步 ⇒ journal_flush_page
 *   先排空在飞 job，再同步刷当前页（它是唯一的同步出口，也是同步回退路径）。
 *
 * 掉电保护：Journal 页 Random **关闭**、CRC **开启**（由下层 FC 提供 per-CW CRC +
 *   ECC），外加页头 magic + entry_count 合法性 + page_seq 单调三层结构判据（设计 §7.6）。
 *   异步化**不改变**掉电模型：本仓无电容，acked 写本来就容忍丢最后一页 journal 条目
 *   （write-back 攒批），把「页满时立刻阻塞刷」改成「页满时交给 job 刷」只是把阻塞挪走。
 *
 * ⚠ **介质读回不转断言**（assertion_guidelines §六 辨析 C）：replay 读的是 NAND 上的
 *   页，页头可能因掉电撕裂而非法——那是掉电的**预期情形**，必须返回错误/跳过，不能断言。
 *   模块入口与命令派发路径（ftl_journal_callback）的前置条件仍用 ASSERT。
 *
 * 文件结构：
 *   1. 页地址辅助     journal_page_addr / journal_block_addr（static）
 *   2. 初始化         journal_init
 *   3. 追加与刷页     journal_append / journal_freeze / journal_handle /
 *                     journal_start_erase / journal_flush_page（+ journal_flush_now）
 *   4. 完成服务       ftl_journal_callback（g_op_callbacks[FTL_OP_JOURNAL]）
 *   5. 回放与写指针恢复 journal_scan_latest / journal_resume / journal_replay
 *   6. 截断           journal_truncate_plan / journal_truncate_commit（两步，异步 CP 用）
 */

#include "journal.h"

#include "assertion.h"
#include "bbt.h" /* bbt_mark_bad（环形块持续擦除失败，RISK-JN-01） */
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

/* 同一环形块连续擦除失败多少次后判为不可用（RISK-JN-01）。
 * 3 次覆盖瞬态擦除失败；持续失败只能来自物理坏块。继续重试没有意义：环只有
 * JOURNAL_BLOCK_COUNT 块，必须标坏并跳过，否则 journal 永久停在 need_erase。 */
#define JRN_ERASE_FAIL_MAX (3U)

/* ══════════════════════════════════════════════════════════
 * 1. 页地址辅助
 * ══════════════════════════════════════════════════════════ */

/* ── Journal Block 页地址辅助 ── */
/* journal_page_addr — 第 b 块第 page 页的 NAND 地址（plane 由页号奇偶决定，见下） */
static nand_pba_t journal_page_addr(const l2p_journal_t *journal, uint8_t b, uint8_t page)
{
    /* cw = 0 = 整页；plane 由页号奇偶决定：偶页 P0、奇页 P1（双 Plane 交错） */
    const uint32_t block = (JOURNAL_BLOCK0 + b);
    const uint32_t plane = (page & 1U);

    return pba_make(journal->topo.ch, journal->topo.ce, journal->topo.lun, block, plane, page, 0U);
}

/* journal_block_addr — 第 b 块的首页地址（擦除用的基址；plane/page 归零，cw=0）。
 * ⚠ 页地址跨 P0/P1（见 journal_page_addr）⇒ **所有擦除必须用
 *   ftl_*_erase_block_multi_plane**，否则 P1 的那一半页不会被擦，环形回绕后二次编程。 */
static nand_pba_t journal_block_addr(const l2p_journal_t *journal, uint8_t b)
{
    const uint32_t block = (JOURNAL_BLOCK0 + b);

    return pba_make(journal->topo.ch, journal->topo.ce, journal->topo.lun, block, 0U, 0U, 0U);
}

/* 环形块是否已不可用：RAM bad_mask ∪ BST BAD（BBT 持久化后重启仍生效）。
 * journal 块跨两个 plane 交错（页号奇偶决定 plane）⇒ 任一 plane 的块 BAD 即跳过整块。 */
static bool journal_ring_block_bad(const l2p_journal_t *journal, uint8_t b)
{
    ASSERT(journal != NULL);
    ASSERT(b < JOURNAL_BLOCK_COUNT);

    if ((journal->handle.bad_mask & (uint8_t)(1U << b)) != 0U)
    {
        return true;
    }
    const journal_bad_block_port_t *port = &journal->bad_block_port;

    if (port->block_is_bad == NULL)
    {
        return false;
    }
    for (uint8_t p = 0U; p < PLANE_NUM; p++)
    {
        const uint16_t gblock = ftl_gblock(p, (uint16_t)(JOURNAL_BLOCK0 + b));

        if (port->block_is_bad(port->ctx, gblock))
        {
            return true;
        }
    }
    return false;
}

/* 从 from 之后找下一个可用环形块；全部不可用返回 JOURNAL_BLOCK_NONE。 */
static uint8_t journal_next_block(const l2p_journal_t *journal, uint8_t from)
{
    ASSERT(journal != NULL);
    ASSERT(from < JOURNAL_BLOCK_COUNT);

    for (uint8_t i = 1U; i <= JOURNAL_BLOCK_COUNT; i++)
    {
        const uint8_t b = (uint8_t)((from + i) % JOURNAL_BLOCK_COUNT);

        if (!journal_ring_block_bad(journal, b))
        {
            return b;
        }
    }
    return JOURNAL_BLOCK_NONE;
}

/* 环内第一个可用块（空环恢复写指针用）；全部不可用返回 JOURNAL_BLOCK_NONE。 */
static uint8_t journal_first_usable(const l2p_journal_t *journal)
{
    ASSERT(journal != NULL);

    for (uint8_t b = 0U; b < JOURNAL_BLOCK_COUNT; b++)
    {
        if (!journal_ring_block_bad(journal, b))
        {
            return b;
        }
    }
    return JOURNAL_BLOCK_NONE;
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
    memset(&journal->bad_block_port, 0, sizeof(journal->bad_block_port)); /* 由装配层绑定 */

    journal->handle.current_block    = 0U;
    journal->handle.current_page     = 0U;
    journal->handle.current_entry    = 0U;
    journal->handle.sequence_counter = 1U;
    journal->handle.merge_floor      = 0U;
    journal->handle.pre_erased_block = JOURNAL_BLOCK_NONE;

    memset(journal->handle.page_buf, 0xFF, sizeof(journal->handle.page_buf));

    journal->handle.checkpoint_needed = 0U;
    journal->handle.emergency         = 0U;

    journal->handle.write_fail_count   = 0U;
    journal->handle.write_fail_seq     = 0U;
    journal->handle.write_fail_pending = 0U;

    /* 异步刷页 job 复位：无在飞、无待偿擦除 */
    journal->handle.phase      = JRN_IDLE;
    journal->handle.need_erase = 0U;
}

void journal_bind_bad_block_port(l2p_journal_t *journal, const journal_bad_block_port_t *port)
{
    ASSERT(journal != NULL);
    ASSERT(port != NULL);
    ASSERT(port->block_is_bad != NULL);
    ASSERT(port->mark_bad != NULL);

    journal->bad_block_port = *port;
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
 *         阶段要消除的），改为丢弃本条 + LOGE，整页留给 journal_handle 下轮刷。
 *         该分支在 host 写路径上实际不可达：页满需 127 次 append，而单次
 *         completion_table_drain 最多处理 CQ 深度（64）条完成。
 * -------------------------------------------------------------------- */
bool journal_append(l2p_journal_t *journal, lba_t lba, nand_pba_t old_pba, nand_pba_t new_pba)
{
    /* 前置条件（判据 2）：调用者一律传 ftl->journal。 */
    ASSERT(journal != NULL);

    /* 环全部不可用：当前页缓冲永远刷不出去，追加只会制造“看起来记下了”的假象。 */
    if (journal->handle.ring_full != 0U)
    {
        return false;
    }

    /*
     * 更新合并：当前页缓冲里已有同一 LBA 的条目 ⇒ 就地更新，不新增条目。
     * 回放按页序 + 页内下标应用（per-LBA last-write-wins），页内同 LBA 只保留最新
     * new_pba 即正确；old_pba 仅首条保留（回放不读 old_pba）。
     * ⚠ **不**消耗 sequence、**不**推进 current_entry：hdr_page_seq 由
     * `sequence_counter − current_entry` 反推本页首条序号，二者必须同步推进，
     * 否则页序错乱会导致回放顺序错误。
     * ⚠ 合并放在「页满守卫」之前：合并**不占新槽位**，页满时仍应更新最新映射。
     */
    l2p_journal_entry_t *entry;
    for (uint16_t i = 0U; i < journal->handle.current_entry; i++)
    {
        entry = (l2p_journal_entry_t *)(journal->handle.page_buf + JOURNAL_HEADER_SIZE +
                                        sizeof(l2p_journal_entry_t) * i);
        /* CP 边界之前的条目不得被合并：merge_floor 之后的覆盖写另起一条，
         * 否则 sequence 仍是旧的、new_pba 却是新的，块元数据 delta 回放会漏记。 */
        if ((entry->lba == (uint32_t)lba) && (entry->sequence >= journal->handle.merge_floor))
        {
            entry->new_pba = new_pba; /* 保留首条 old_pba，更新到最新 new_pba */
            return true;
        }
    }

    /* 页已满且上一轮异步刷页未成功（job 忙 / 池空）⇒ 本条无处可放，丢弃。
     * ⚠ 守卫必须在按 current_entry 取槽位**之前**：current_entry ==
     *   JOURNAL_ENTRIES_PER_PAGE 时 `JOURNAL_HEADER_SIZE + 127×16 = 2060 > 2048`
     *   会写出 page_buf 之外（踩到 journal_handle_t）。
     * 丢弃的后果与「掉电丢最后一页 journal」同级（回放少应用一次映射更新）；
     * 整页由 journal_handle 下轮刷掉后自动恢复。 */
    if (journal->handle.current_entry >= JOURNAL_ENTRIES_PER_PAGE)
    {
        LOGE("[JRN] page full & async flush pending — 丢弃条目 lba=%lu\r\n", lba);
        return false;
    }

    entry = (l2p_journal_entry_t *)(journal->handle.page_buf + JOURNAL_HEADER_SIZE +
                                    sizeof(l2p_journal_entry_t) * journal->handle.current_entry);

    entry->lba      = (uint32_t)lba;
    entry->old_pba  = old_pba;
    entry->new_pba  = new_pba;
    entry->sequence = journal->handle.sequence_counter++;

    journal->handle.current_entry++;

    if (journal->handle.current_entry >= JOURNAL_ENTRIES_PER_PAGE)
    {
        if ((!journal_freeze(journal)) && (!ftl_op_in_dispatch()))
        {
            /* 非派发上下文（GC/WL/TRIM/开机）⇒ 同步回退。
             * 页缓冲此时仍是满的、写指针未动，journal_flush_page 会先排空在飞 job
             * 再同步刷这一页。 */
            journal_flush_page(journal);
        }
        /* 派发上下文里 freeze 失败：什么都不做——上面那条守卫会让后续 append 丢弃
         * 并 LOGE，整页由 journal_handle（ftl_service）下轮刷掉。 */
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
    uint32_t hdr_page_seq    = journal->handle.sequence_counter - journal->handle.current_entry;
    uint16_t hdr_entry_count = journal->handle.current_entry;
    uint16_t hdr_block_seq   = journal->handle.current_block;

    memcpy(journal->handle.page_buf + 0, &magic, sizeof(magic));
    memcpy(journal->handle.page_buf + 4, &hdr_page_seq, sizeof(hdr_page_seq));
    memcpy(journal->handle.page_buf + 8, &hdr_entry_count, sizeof(hdr_entry_count));
    memcpy(journal->handle.page_buf + 10, &hdr_block_seq, sizeof(hdr_block_seq));
}

/* journal_clear_page — 复位页缓冲为擦除态并清条目计数（准备下一页）。 */
static void journal_clear_page(l2p_journal_t *journal)
{
    journal->handle.current_entry = 0U;
    memset(journal->handle.page_buf, 0xFF, sizeof(journal->handle.page_buf));
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
 *       （journal_handle 会先补擦；journal_append 的回退路径由 journal_flush_now 补擦。）
 *     · 页写成功后指针才推进（推进点在 ftl_journal_callback），故落点先算好存进 job；
 *       跨块时一并算出 need_erase（先擦后写）。
 *     · 数据在 ftl_async_write_page 内**已拷进 FC buffer 池** ⇒ 提交后立刻腾页缓冲，
 *       不需要 ping-pong 快照缓冲。
 *     · pre_erased_block 的消费放在**提交成功之后**：提交失败时记账不动，下轮重算。
 * -------------------------------------------------------------------- */
static bool journal_freeze(l2p_journal_t *journal)
{
    if ((journal->handle.phase != JRN_IDLE) || (journal->handle.need_erase != 0U) ||
        (journal->handle.ring_full != 0U))
    {
        return false; /* 环已全部不可用：不再起新页写 */
    }

    journal_write_header(journal);

    nand_pba_t waddr = journal_page_addr(journal, journal->handle.current_block, journal->handle.current_page);

    fc_command_t *cmd;
    if (ftl_async_write_page(&cmd, waddr, journal->handle.page_buf, JOURNAL_PAGE_SIZE, FTL_PAGE_CFG_META()) != FTL_OK)
    {
        return false; /* 命令池空 / 建链失败 ⇒ 同步回退 */
    }

    /* 落点：写成功后指针推进到 (target_block, target_page)；跨块 ⇒ 需擦新块 */
    uint8_t tpage      = (uint8_t)(journal->handle.current_page + 1U);
    uint8_t tblock     = journal->handle.current_block;
    uint8_t need_erase = 0U;
    if (tpage >= PAGES_PER_BLOCK)
    {
        tpage  = 0U;
        tblock = journal_next_block(journal, tblock);

        if (tblock == JOURNAL_BLOCK_NONE)
        {
            /* 全部环形块不可用：本页仍写（它属于当前好块），但之后停止写入。 */
            journal->handle.ring_full = 1U;
            tblock                    = journal->handle.current_block;
            LOGE("[JRN] all ring blocks unusable — journal writes disabled\r\n");
        }
        else
        {
            need_erase = 1U;
        }
    }

    journal->handle.from_block   = journal->handle.current_block;
    journal->handle.from_page    = journal->handle.current_page;
    journal->handle.target_block = tblock;
    journal->handle.target_page  = tpage;
    journal->handle.waddr        = waddr;

    if (ftl_op_submit(cmd, FTL_OP_JOURNAL, journal) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return false;
    }

    /* 提交成功才消费预擦除记账：该块已被 journal_truncate 预擦过 ⇒ 回切时跳过擦除
     * （同一块每圈只擦一次）。 */
    if ((need_erase != 0U) && (journal->handle.pre_erased_block == tblock))
    {
        journal->handle.pre_erased_block = JOURNAL_BLOCK_NONE;
        need_erase                       = 0U;
    }
    journal->handle.need_erase = need_erase;
    journal->handle.phase      = JRN_WRITE;

    journal_clear_page(journal);
    return true;
}

/* --------------------------------------------------------------------
 * journal_start_erase
 *   功能：为 job.need_erase 起的**异步**换块擦除。
 *   参数：journal —— Journal（job.target_block = 待擦块）。
 *   返回：true = 已起（phase = JRN_ERASE，need_erase 已清）；false = 未起（池空 / 建链失败，
 *         need_erase 保留，由 journal_handle 下轮重试）。
 *   实现要点：擦的是**已推进到**的新环形块，故必须在任何页写进该块之前完成。
 * -------------------------------------------------------------------- */
static bool journal_start_erase(l2p_journal_t *journal)
{
    /* 环已全部不可用：不再发擦除（发了也只会永远失败）。 */
    if (journal->handle.ring_full != 0U)
    {
        return false;
    }

    fc_command_t *cmd;
    if (ftl_async_erase_block_multi_plane(&cmd, journal_block_addr(journal, journal->handle.target_block)) != FTL_OK)
    {
        return false;
    }
    if (ftl_op_submit(cmd, FTL_OP_JOURNAL, journal) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return false;
    }

    journal->handle.need_erase = 0U;
    journal->handle.phase      = JRN_ERASE;
    return true;
}

/* --------------------------------------------------------------------
 * journal_erase_failed
 *   功能：记录一次环形块擦除失败；同一块连续失败达上限即标坏并跳过（RISK-JN-01）。
 *   参数：journal —— Journal；block —— 失败的环形块。
 *   返回：无。
 *   实现要点：
 *     · 标坏 = RAM bad_mask + （分配器已绑定时）bbt_mark_bad 两个 plane 的块。
 *       BBT 持久化后，重启时 bbt_init 会把块置 BAD，journal_ring_block_bad 直接跳过。
 *     · 跳到下一个可用块；若全部不可用 ⇒ ring_full，journal 停止写入（不写未擦块、
 *       也不再无限重试擦除）。
 *     · current_block 若正指向该坏块，同步跳到下一块（该块从未写入新数据）。
 * -------------------------------------------------------------------- */
static void journal_erase_failed(l2p_journal_t *journal, uint8_t block)
{
    ASSERT(journal != NULL);
    ASSERT(block < JOURNAL_BLOCK_COUNT);

    if (journal->handle.erase_fail_block != block)
    {
        journal->handle.erase_fail_block = block;
        journal->handle.erase_fail_count = 0U;
    }
    journal->handle.erase_fail_count++;

    if (journal->handle.erase_fail_count < JRN_ERASE_FAIL_MAX)
    {
        LOGW("[JRN] ring block %u erase failed (%u/%u)\r\n",
             (unsigned)block,
             (unsigned)journal->handle.erase_fail_count,
             JRN_ERASE_FAIL_MAX);
        return;
    }

    LOGE("[JRN] ring block %u erase failed x%u — mark BAD\r\n",
         (unsigned)block,
         (unsigned)journal->handle.erase_fail_count);

    journal->handle.bad_mask |= (uint8_t)(1U << block);

    const journal_bad_block_port_t *port = &journal->bad_block_port;

    if (port->mark_bad != NULL)
    {
        for (uint8_t p = 0U; p < PLANE_NUM; p++)
        {
            const uint16_t gblock = ftl_gblock(p, (uint16_t)(JOURNAL_BLOCK0 + block));

            port->mark_bad(port->ctx, gblock);
        }
    }
    journal->handle.erase_fail_count = 0U;
    journal->handle.erase_fail_block = JOURNAL_BLOCK_NONE;
    if (journal->handle.pre_erased_block == block)
    {
        journal->handle.pre_erased_block = JOURNAL_BLOCK_NONE;
    }

    const uint8_t next = journal_next_block(journal, block);

    if (next == JOURNAL_BLOCK_NONE)
    {
        journal->handle.ring_full = 1U;
        LOGE("[JRN] all ring blocks unusable — journal writes disabled\r\n");
        return; /* need_erase 保留：绝不能写未擦块 */
    }

    journal->handle.target_block = next;
    if (journal->handle.current_block == block)
    {
        journal->handle.current_block = next;
        journal->handle.current_page  = 0U;
    }
}

/* ── 状态上报（主流程只读这个，不窥探 handle 内部） ── */
static void journal_status_fill(const l2p_journal_t *journal, journal_status_t *out)
{
    ASSERT(journal != NULL);
    ASSERT(out != NULL);

    out->base.critical  = false; /* 由 must-run 适配器另行调用 ERASE_ONLY 入口保证 liveness */
    out->base.wants_run = (journal->handle.phase != JRN_IDLE) || (journal->handle.need_erase != 0U) ||
                          (journal->handle.current_entry != 0U);
    out->base.progress = journal->handle.sequence_counter;
    out->base.next_ms  = 0U;

    if (journal->handle.ring_full != 0U)
    {
        out->state          = JOURNAL_STATE_RING_FULL;
        out->base.state     = FTL_MODULE_BLOCKED;
        out->base.wants_run = false;
    }
    else if (journal->handle.write_fail_pending != 0U)
    {
        out->state      = JOURNAL_STATE_FAILED;
        out->base.state = FTL_MODULE_FAILED;
    }
    else if (journal->handle.phase == JRN_WRITE)
    {
        out->state      = JOURNAL_STATE_WRITE_WAIT;
        out->base.state = FTL_MODULE_WAITING;
    }
    else if (journal->handle.phase == JRN_ERASE)
    {
        out->state      = JOURNAL_STATE_ERASE_WAIT;
        out->base.state = FTL_MODULE_WAITING;
    }
    else
    {
        out->state      = JOURNAL_STATE_IDLE;
        out->base.state = (out->base.wants_run ? FTL_MODULE_RUNNING : FTL_MODULE_IDLE);
    }

    out->base.error    = (journal->handle.write_fail_pending != 0U) ? (int32_t)FTL_ERROR : (int32_t)FTL_OK;
    out->sequence      = journal->handle.sequence_counter;
    out->write_failed  = (journal->handle.write_fail_pending != 0U);
    out->ring_full     = (journal->handle.ring_full != 0U);
    out->erase_pending = (journal->handle.need_erase != 0U);
    out->current_block = journal->handle.current_block;
    out->current_page  = journal->handle.current_page;
    out->current_entry = journal->handle.current_entry;
}

/* --------------------------------------------------------------------
 * journal_handle
 *   功能：后台推进 Journal——把待写的整页 / 待起的擦除交给异步 job。
 *   参数：journal —— Journal；trigger —— FLUSH / ERASE_ONLY；info —— 本轮动态信息；
 *         out —— 状态上报。
 *   返回：无。
 *   实现要点：job 已在飞 ⇒ 只做心跳（单飞行）。有未起的擦除时**优先补擦**：
 *             新环形块没擦过就不能往里写。ERASE_ONLY 到此为止（host 饱和档让路）。
 * -------------------------------------------------------------------- */
void journal_handle(l2p_journal_t *journal, const journal_trigger_t *trigger, const ftl_bg_info_t *info, journal_status_t *out)
{
    /* 前置条件（判据 2）：调用者一律传 ftl->journal。 */
    ASSERT(journal != NULL);
    ASSERT(out != NULL);

    /* info 当前只用于观测（心跳用自己的平台时钟）；保留形参以统一 typed 入口。 */
    (void)info;

    const bool erase_only = (trigger != NULL) && (*trigger == JOURNAL_TRIGGER_ERASE_ONLY);

    if (journal->handle.ring_full != 0U)
    {
        journal_status_fill(journal, out);
        return; /* 环全部不可用：不重试擦除（已 LOGE 过），也不写未擦块 */
    }

    if (journal->handle.phase != JRN_IDLE)
    {
        /* 心跳（见 ftl_job.h）：在飞期间每满 1s 打一行，异步后「卡在哪个 phase」的唯一观测口 */
        if (ftl_job_heartbeat(&journal->handle.log_ms, &journal->handle.start_ms))
        {
            LOGI("[JRN] job phase=%u waddr=0x%08lx age=%lums\r\n",
                 (unsigned)journal->handle.phase,
                 journal->handle.waddr,
                 ftl_job_age_ms(&journal->handle.start_ms));
        }
        journal_status_fill(journal, out);
        return; /* 单飞行：job 已在飞 */
    }

    journal->handle.log_ms   = 0U;
    journal->handle.start_ms = 0U;

    if (journal->handle.need_erase != 0U)
    {
        (void)journal_start_erase(journal);
        journal_status_fill(journal, out);
        return;
    }

    if (erase_only || (journal->handle.current_entry == 0U))
    {
        journal_status_fill(journal, out);
        return; /* ERASE_ONLY 不起页写；无待写数据也到此为止 */
    }

    (void)journal_freeze(journal);
    journal_status_fill(journal, out);
}

/* ── 查询（替代外部直读 handle 字段） ── */
bool journal_is_idle(const l2p_journal_t *journal)
{
    ASSERT(journal != NULL);
    return journal->handle.phase == JRN_IDLE;
}

bool journal_can_erase_victim(const l2p_journal_t *journal)
{
    ASSERT(journal != NULL);
    return journal->handle.write_fail_pending == 0U;
}

uint32_t journal_sequence(const l2p_journal_t *journal)
{
    ASSERT(journal != NULL);
    return journal->handle.sequence_counter;
}

bool journal_write_failed(const l2p_journal_t *journal)
{
    ASSERT(journal != NULL);
    return journal->handle.write_fail_pending != 0U;
}

bool journal_has_pending_entries(const l2p_journal_t *journal)
{
    ASSERT(journal != NULL);
    return journal->handle.current_entry != 0U;
}

bool journal_erase_pending(const l2p_journal_t *journal)
{
    ASSERT(journal != NULL);
    return journal->handle.need_erase != 0U;
}

bool journal_checkpoint_needed(const l2p_journal_t *journal)
{
    ASSERT(journal != NULL);
    return journal->handle.checkpoint_needed != 0U;
}

bool journal_is_emergency(const l2p_journal_t *journal)
{
    ASSERT(journal != NULL);
    return journal->handle.emergency != 0U;
}

void journal_set_merge_floor(l2p_journal_t *journal, uint32_t sequence)
{
    ASSERT(journal != NULL);
    journal->handle.merge_floor = sequence;
}

uint8_t journal_watermark(const l2p_journal_t *journal)
{
    ASSERT(journal != NULL);

    /* 全环全局口径（精确到条目）：分子必须是「全局写位置」，不是单块内 current_page
     * ——否则 85%/95% 会提前或永不触发。最大 used_units = 256 × 127 ≈ 32512，
     * ×100 不会溢出 uint32_t。 */
    const uint32_t entries_per_page = JOURNAL_ENTRIES_PER_PAGE;
    const uint32_t total_pages      = (uint32_t)JOURNAL_BLOCK_COUNT * PAGES_PER_BLOCK;
    const uint32_t used_units       = ((journal->handle.current_block * PAGES_PER_BLOCK +
                                  journal->handle.current_page) *
                                 entries_per_page) +
                                journal->handle.current_entry;

    return (uint8_t)((used_units * 100U) / (total_pages * entries_per_page));
}

void journal_update_watermark(l2p_journal_t *journal)
{
    ASSERT(journal != NULL);

    const uint8_t used = journal_watermark(journal);

    if (used >= JOURNAL_CRITICAL)
    {
        journal->handle.emergency = 1U; /* ≥95% 紧急 */
    }
    else if (used >= JOURNAL_FULL_THRESHOLD)
    {
        journal->handle.checkpoint_needed = 1U; /* ≥85% 需 Checkpoint */
    }
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

    while ((journal->handle.phase != JRN_IDLE) && (spin < JOURNAL_DRAIN_SPIN_MAX))
    {
        (void)fc_service();
        completion_table_drain();
        spin++;
    }

    return (journal->handle.phase == JRN_IDLE);
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
    if (journal->handle.pre_erased_block == journal->handle.target_block)
    {
        journal->handle.pre_erased_block = JOURNAL_BLOCK_NONE; /* truncate 已代擦，记账消费 */
    }
    else
    {
        const nand_pba_t   addr = journal_block_addr(journal, journal->handle.target_block);
        const ftl_status_t est  = ftl_sync_erase_block_multi_plane(addr);

        if (est != FTL_OK)
        {
            /* 擦失败 ⇒ 保留 need_erase，绝不清账；flusher 看到它仍置位就不会写未擦块。 */
            LOGE("[JRN] sync ring erase failed blk=%u status=%d\r\n",
                 (unsigned)journal->handle.target_block,
                 est);
            journal_erase_failed(journal, journal->handle.target_block);
            journal_note_write_failure(journal);
            return;
        }
        journal->handle.erase_fail_count = 0U;
        journal->handle.erase_fail_block = JOURNAL_BLOCK_NONE;
    }
    journal->handle.need_erase = 0U;
}

/* --------------------------------------------------------------------
 * journal_flush_now
 *   功能：**同步**把当前页缓冲连同页头写进 NAND 当前页，并推进写指针。
 *   参数：journal —— Journal。
 *   返回：true = 缓冲已空或已落盘；false = 未落盘（在飞 / 擦失败 / 写失败）。
 *   实现要点：
 *     · **在飞守卫**：job.phase != IDLE 时当前页地址仍归在飞命令所有，再写就是
 *       双重编程 ⇒ 直接拒绝。
 *     · 先偿还未起的换块擦除（job.need_erase 待偿时，目标块 = 已推进到的 current_block）。
 *     · 页满换块：本函数是**唯一**的同步环形块擦除点；若该块已被 journal_truncate
 *       预擦除（pre_erased_block），**跳过擦除**。
 *     · 写后清空页缓冲（0xFF）与 current_entry，准备下一页。
 *     · ⚠ 写失败：置 `write_fail_pending`（阻止 GC 擦 victim）+ LOGE，但仍推进指针
 *       ——已写过的页（可能半编程）不可复用。同步在写后推进；异步在失败完成时推进到
 *       freeze 算好的下一落点。
 * -------------------------------------------------------------------- */
static bool journal_flush_now(l2p_journal_t *journal)
{
    if (journal->handle.ring_full != 0U)
    {
        return false; /* 环已全部不可用：不再写任何页 */
    }

    if (journal->handle.phase != JRN_IDLE)
    {
        return false; /* 在飞命令拥有当前页地址：再写 = 双重编程 */
    }

    if (journal->handle.need_erase != 0U)
    {
        journal_erase_pending_sync(journal);

        if (journal->handle.need_erase != 0U)
        {
            /* 目标块仍未擦：不能把页写进去（二次编程/脏块写 = 静默损坏）。
             * 页缓冲保留，下次 flush 重试；pending 已置位，GC 不会擦 victim。 */
            return false;
        }
    }

    if (journal->handle.current_entry == 0U)
    {
        return true; /* 无数据，无需 flush */
    }

    journal_write_header(journal);

    uint8_t    target_page = journal->handle.current_page;
    nand_pba_t waddr       = journal_page_addr(journal, journal->handle.current_block, target_page);

    /*
     * 同步写 Journal 页到 NAND。
     * Random DISABLED / CRC ENABLED：Journal 有 FC per-CW CRC + ECC，
     * 外加自身结构判据（magic/entry_count/sequence）。
     */
    const ftl_status_t wst = ftl_sync_write_page(waddr, journal->handle.page_buf, JOURNAL_PAGE_SIZE, FTL_PAGE_CFG_META());

    bool ok = (wst == FTL_OK);

    if (!ok)
    {
        LOGE("[JRN] sync page write failed pba=%08lx status=%d — 条目丢失，置 pending\r\n",
             waddr,
             wst);
        journal_note_write_failure(journal);
    }

    /* 推进指针 */
    journal->handle.current_page++;
    if (journal->handle.current_page >= PAGES_PER_BLOCK)
    {
        /* 当前 Block 写满，切到下一个**可用**环形块（坏块在 journal_erase_failed
         * 里已标进 bad_mask/BST，这里直接跳过）。 */
        const uint8_t next = journal_next_block(journal, journal->handle.current_block);

        if (next == JOURNAL_BLOCK_NONE)
        {
            journal->handle.ring_full = 1U;
            LOGE("[JRN] all ring blocks unusable — journal writes disabled\r\n");
            journal_note_write_failure(journal);
            ok = false;
        }
        else
        {
            journal->handle.current_block = next;
            journal->handle.current_page  = 0U;

            /* 回切擦除（同步路径的环形块擦除点）。
             * 若该块已被 journal_truncate 预擦除（水位截断），跳过——同一块每圈只擦一次。 */
            if (journal->handle.pre_erased_block == journal->handle.current_block)
            {
                journal->handle.pre_erased_block = JOURNAL_BLOCK_NONE; /* 记账消费掉：本块即将被写 */
            }
            else
            {
                const uint8_t      block = journal->handle.current_block;
                const nand_pba_t   addr  = journal_block_addr(journal, block);
                const ftl_status_t est   = ftl_sync_erase_block_multi_plane(addr);

                if (est != FTL_OK)
                {
                    /* 新环形块没擦成功 ⇒ 重新挂起 need_erase：下一次 flush 先重试擦除，
                     * 仍失败就不写（绝不能写未擦块）。持续失败由 journal_erase_failed
                     * 标坏并跳到下一块。 */
                    LOGE("[JRN] sync ring advance erase failed blk=%u status=%d — 挂起\r\n",
                         (unsigned)journal->handle.current_block,
                         est);
                    journal->handle.target_block = journal->handle.current_block;
                    journal_erase_failed(journal, journal->handle.current_block);
                    journal->handle.need_erase = 1U;
                    journal_note_write_failure(journal);
                    ok = false;
                }
                else
                {
                    journal->handle.erase_fail_count = 0U;
                    journal->handle.erase_fail_block = JOURNAL_BLOCK_NONE;
                }
            }
        }
    }

    journal_clear_page(journal);
    return ok;
}

/* --------------------------------------------------------------------
 * journal_flush_page
 *   功能：**同步**把 Journal 落盘（屏障语义）——先排空在飞 job，再刷当前页。
 *   参数：journal —— Journal。
 *   返回：FTL_OK = 屏障达成；FTL_ERROR = drain 超时 / 擦除失败 / 页写失败。
 *   实现要点：调用者拿它当屏障——HIL `fl`（已持久化映射落盘）、journal_append 的
 *             非派发同步回退。host 写路径的常规刷页走 journal_handle。
 *             ⚠ drain 超时（命令丢失 / NAND 卡死）⇒ **拒绝同步刷**：在飞写命令仍
 *             拥有当前页地址，再写同页就是双重编程。置 pending 后返回错误。
 * -------------------------------------------------------------------- */
ftl_status_t journal_flush_page(l2p_journal_t *journal)
{
    /* 前置条件（判据 2）：调用者一律传 ftl->journal。 */
    ASSERT(journal != NULL);

    if (!journal_drain(journal))
    {
        LOGE("[JRN] drain timeout phase=%u — 拒绝同步刷（在飞命令拥有当前页）\r\n",
             (unsigned)journal->handle.phase);
        journal_note_write_failure(journal);
        return FTL_ERROR;
    }

    return journal_flush_now(journal) ? FTL_OK : FTL_ERROR;
}

/* ══════════════════════════════════════════════════════════
 * 4. 完成服务（g_op_callbacks[FTL_OP_JOURNAL]）
 * ══════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * ftl_journal_callback
 *   功能：Journal 刷页 job 的完成服务函数——按 job.phase 推进状态机。
 *   参数：ftl —— 实例（**不用**：ctx 已是本实例的 journal）；completion —— 完成状态；
 *         ctx —— `l2p_journal_t *`（ftl_op_submit 时传 ftl->journal）。
 *   返回：无。命令的 release 归本函数（见 ftl_op.h 的所有权表）。
 *   实现要点：按 job.phase 分派到 journal_complete_write / journal_complete_erase
 *             （见各自注释）。job.phase 只能取 WRITE / ERASE，其余是状态机破坏 ⇒
 *             这里的 ASSERT 只覆盖 debug，release 兜底分支见函数体。
 * -------------------------------------------------------------------- */
/* 记录一次 Journal 页写失败：条目已随 page_buf 清空而丢失。
 * 置 pending 阻止任何 victim 擦除，并强制一次 Checkpoint 把当前 L2P 落盘；
 * pending 由 journal_clear_write_failure（CP Header 成功后）清除。 */
static void journal_note_write_failure(l2p_journal_t *journal)
{
    journal->handle.write_fail_count++;
    journal->handle.write_fail_seq     = journal->handle.sequence_counter;
    journal->handle.write_fail_pending = 1U;
    journal->handle.checkpoint_needed  = 1U;
    journal->handle.emergency          = 1U;
}

/* Journal 页写完成（成功 / 失败共用）。两条路径都要推进落点并置 IDLE：
 * **指针推进守卫** —— drain 超时后 journal_flush_now 可能已把指针推过落点，
 * 此时 job 的落点已过期 ⇒ 只在「指针仍在起写位置」时推进。
 * 失败页可能已半编程 ⇒ **绝不复用**；推进到 freeze 算好的下一落点，跨块时
 * need_erase 保留，由 journal_handle 先擦目标块再写。条目已丢（page_buf 在提交时
 * 清空），由 write_fail_pending 兜住：GC 不会擦 victim，直到 CP 把当前 L2P 落盘。 */
static void journal_complete_write(l2p_journal_t *journal, fc_command_t *cmd, const fc_command_status_t *completion)
{
    const bool failed = fc_cw_status_has_error(completion->status);

    if (failed)
    {
        LOGE("[JRN] page write failed pba=%08lx status=0x%02x — 跳过失败页\r\n",
             journal->handle.waddr,
             completion->status);
        journal_note_write_failure(journal);
    }

    if ((journal->handle.current_block == journal->handle.from_block) && (journal->handle.current_page == journal->handle.from_page))
    {
        journal->handle.current_block = journal->handle.target_block;
        journal->handle.current_page  = journal->handle.target_page;
    }
    journal->handle.phase = JRN_IDLE;

    if (failed)
    {
        /* 写都没成，不在此起换块擦除：need_erase 保留给 journal_handle 下轮处理。 */
        (void)fc_command_release(cmd);
        return;
    }

    /* 换块 ⇒ 起异步擦除；起不来就留着 need_erase，由 journal_handle 下轮重试。
     * ⚠ 先查 pre_erased_block：journal_truncate 可能已在这段窗口里代擦了环形
     *   下一块（正是本 job.target_block）⇒ 消费记账跳过，避免重复擦。 */
    if (journal->handle.need_erase != 0U)
    {
        if (journal->handle.pre_erased_block == journal->handle.target_block)
        {
            journal->handle.pre_erased_block = JOURNAL_BLOCK_NONE;
            journal->handle.need_erase       = 0U;
        }
        else if (!journal_start_erase(journal))
        {
            LOGE("[JRN] erase kick failed blk=%u — 下轮重试\r\n", (unsigned)journal->handle.target_block);
        }
    }

    (void)fc_command_release(cmd);
}

/* Journal 环形块擦完成。失败判据必须是全集：只看 NAND_FAIL 会把 TIMEOUT / CRC
 * 当成擦成功，后续页写进可能未擦的块（二次编程 = 静默损坏）。 */
static void journal_complete_erase(l2p_journal_t *journal, fc_command_t *cmd, const fc_command_status_t *completion)
{
    if (fc_cw_status_has_error(completion->status))
    {
        LOGE("[JRN] ring block erase failed blk=%u status=0x%02x — 重新挂起\r\n",
             (unsigned)journal->handle.target_block,
             completion->status);
        /* 擦失败 ⇒ 目标块仍是脏的；绝不能把后续页写进去。重新挂起 need_erase，
         * 由 journal_handle 下轮重试；持续失败由 journal_erase_failed 标坏并跳过。 */
        journal_erase_failed(journal, journal->handle.target_block);
        journal->handle.need_erase = 1U;
    }
    else
    {
        /* 擦成功：连续失败计数归零（只统计**连续**失败）。 */
        journal->handle.erase_fail_count = 0U;
        journal->handle.erase_fail_block = JOURNAL_BLOCK_NONE;
    }
    journal->handle.phase = JRN_IDLE;
    (void)fc_command_release(cmd);
}

void ftl_journal_callback(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx)
{
    l2p_journal_t *journal = (l2p_journal_t *)ctx;

    (void)ftl; /* 实例无关：ctx 就是本实例的 journal */

    ASSERT(journal != NULL);
    ASSERT(completion != NULL);

    fc_command_t *cmd = fc_command_id_to_command((uint16_t)completion->command_id);

    /* 一条命令对应一个 job：phase 只能是 WRITE / ERASE（残留完成 = 状态机破坏）。 */
    ASSERT((journal->handle.phase == JRN_WRITE) || (journal->handle.phase == JRN_ERASE));

    if (journal->handle.phase == JRN_WRITE)
    {
        journal_complete_write(journal, cmd, completion);
    }
    else if (journal->handle.phase == JRN_ERASE)
    {
        journal_complete_erase(journal, cmd, completion);
    }
    else
    {
        /* 上面的 ASSERT 在 release 下裁空 ⇒ 这里必须留运行期兜底：否则 job 永远
         * 卡在非 IDLE，后续 flush 再也起不来（命令还不回收就是池泄漏）。 */
        LOGE("[JRN] unexpected completion phase=%u\r\n", (unsigned)journal->handle.phase);
        journal->handle.phase = JRN_IDLE;
        (void)fc_command_release(cmd);
    }
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
 *             只看页首页序号会把跨水位的那一页整页跳过——它页尾的新条目
 *             （> cp_sequence）随页面一起丢。
 *             页头 magic / entry_count 校验失败即跳过（掉电撕裂的预期情形，见文件头 ⚠）。
 * -------------------------------------------------------------------- */
static uint16_t journal_scan_ring(const l2p_journal_t *journal,
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

    uint16_t count       = 0U;
    uint16_t read_errors = 0U;

    for (uint8_t b = 0U; b < JOURNAL_BLOCK_COUNT; b++)
    {
        /* 已判坏的环形块不扫：它的读必然失败，会把整次回放标成“不完整”并逼出 F2。
         * 坏块上的条目本来就不该再被信任。 */
        if (journal_ring_block_bad(journal, b))
        {
            continue;
        }

        for (uint8_t page = 0U; page < PAGES_PER_BLOCK; page++)
        {
            uint8_t page_data[JOURNAL_PAGE_SIZE];
            memset(page_data, 0xFF, sizeof(page_data));

            nand_pba_t raddr = journal_page_addr(journal, b, (uint8_t)page);
            if (ftl_sync_read_page(raddr, page_data, JOURNAL_PAGE_SIZE, FTL_PAGE_CFG_META()) != FTL_OK)
            {
                read_errors++; /* 读失败，跳过本页（下面汇总一条日志） */
                continue;
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

    /* 读失败的页 = 看不到的条目。调用者必须据此决定是否还能信任回放结果
     * （RISK-BOOT-08：不能静默当「无更多条目」）。 */
    return read_errors;
}

/* --------------------------------------------------------------------
 * journal_scan_latest
 *   功能：只读扫环取最新页（journal_scan_ring 的「只找最新页」外壳）。
 *   参数：journal —— Journal；out —— 出参（环空时全置 0，可为 NULL）。
 *   返回：true = 环非空；false = 环空。
 * -------------------------------------------------------------------- */
bool journal_scan_latest(const l2p_journal_t *journal, journal_page_ref_t *out, bool *out_read_error)
{
    /* 前置条件（判据 2）：调用者一律传 ftl->journal。out / out_read_error 可为 NULL。 */
    ASSERT(journal != NULL);

    const uint16_t read_errors = journal_scan_ring(journal, 0U, NULL, 0U, NULL, out);

    if (out_read_error != NULL)
    {
        *out_read_error = (read_errors != 0U);
    }
    if (read_errors != 0U)
    {
        LOGW("[JRN] ring scan: %u page(s) unreadable — 判型结果不可信\r\n", (unsigned)read_errors);
    }

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
 *                pre_erased_block**——那块马上会被写满，下一圈回切时仍需按常规擦一次。
 *                擦除失败 ⇒ 重新挂起 `job.need_erase`，首次写前由 kick/flush 补擦；
 *             ③ 序号取 max(最新页序号 + 1, floor)，0 抬到 1（0 保留作「无序号」，
 *                与 journal_init 同约定；否则 journal_truncate 的水位校验会被 0 挡掉）。
 * -------------------------------------------------------------------- */
void journal_resume(l2p_journal_t *journal, const journal_page_ref_t *latest, uint32_t floor_sequence)
{
    /* 前置条件（判据 2）：调用者一律传 ftl->journal。latest 可为 NULL（文档化契约）。 */
    ASSERT(journal != NULL);

    uint32_t next          = floor_sequence;
    uint8_t  pending_erase = 0U;
    uint8_t  erase_block   = 0U;

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
            next_block = journal_next_block(journal, latest->block);
            next_page  = 0U;

            if (next_block == JOURNAL_BLOCK_NONE)
            {
                journal->handle.ring_full = 1U;
                next_block                = latest->block; /* 停在最后好块；ring_full 挡住后续写 */
                LOGE("[JRN] all ring blocks unusable at resume\r\n");
            }
            else
            {
                /* 擦失败绝不能继续：首次写会落进未擦块（二次编程 = 静默损坏）。
                 * 重新挂起 need_erase，由 journal_handle / flush_now 先补擦；
                 * 持续失败由 journal_erase_failed 标坏并跳到下一块。 */
                journal->handle.target_block = next_block;

                const nand_pba_t erase_addr = journal_block_addr(journal, next_block);

                if (ftl_sync_erase_block_multi_plane(erase_addr) != FTL_OK)
                {
                    LOGE("[JRN] resume ring erase failed blk=%u — 挂起补擦\r\n", (unsigned)next_block);
                    journal_erase_failed(journal, next_block);

                    if (journal->handle.ring_full != 0U)
                    {
                        next_block = latest->block; /* 停在最后好块 */
                    }
                    else
                    {
                        next_block    = journal->handle.target_block; /* 可能已跳到下一可用块 */
                        pending_erase = 1U;
                        erase_block   = next_block;
                    }
                }
            }
        }

        journal->handle.current_block = next_block;
        journal->handle.current_page  = next_page;
    }
    else
    {
        /* 环空：从第一个可用块开始（block0 可能已在 BBT 里标坏）。 */
        const uint8_t first = journal_first_usable(journal);

        if (first == JOURNAL_BLOCK_NONE)
        {
            journal->handle.ring_full     = 1U;
            journal->handle.current_block = 0U;
        }
        else
        {
            journal->handle.current_block = first;
        }
        journal->handle.current_page = 0U;
    }

    if (next == 0U)
    {
        next = 1U;
    }

    journal->handle.current_entry    = 0U;
    journal->handle.sequence_counter = next;
    journal->handle.merge_floor      = 0U;

    /* 开机没有“未完成迁移”的上下文：上次上电的失败 pending 不跨重启。 */
    journal->handle.write_fail_count   = 0U;
    journal->handle.write_fail_seq     = 0U;
    journal->handle.write_fail_pending = 0U;

    /* 异步刷页 job 复位：resume 是开机路径，此前不可能有在飞 job；
     * 但复位是廉价的，且能挡住「同一实例被重复 resume」的残留。
     * 若上面换块擦除失败，在此（复位之后）重新挂起，保证首次写前先补擦。 */
    journal->handle.phase        = JRN_IDLE;
    journal->handle.target_block = erase_block;
    journal->handle.need_erase   = pending_erase;

    memset(journal->handle.page_buf, 0xFF, sizeof(journal->handle.page_buf));
}

/*
 * 应用一页 Journal 条目；返回**成功写入 L2P 的条目数**。
 *
 * 开机回放 = 结构性同步：用同步逃生口（自旋泵），不 park、不返 FTL_AGAIN
 * ——此时 superloop 尚未建立，没人推进 job。
 * entry_lba 来自 NAND，是**不可信输入**：先校验再查表（H29）。
 * 写失败 ⇒ LOGW + block_ctx->ok=0（调用者回退 F2）。
 */
static uint32_t jrn_replay_apply_page(l2p_journal_t              *journal,
                                      l2p_cache_t                *cache,
                                      const journal_page_ref_t   *page,
                                      uint32_t                    cp_sequence,
                                      journal_replay_block_ctx_t *block_ctx)
{
    nand_pba_t raddr = journal_page_addr(journal, page->block, page->page);
    uint8_t    page_data[JOURNAL_PAGE_SIZE];

    memset(page_data, 0xFF, sizeof(page_data));

    if (ftl_sync_read_page(raddr, page_data, JOURNAL_PAGE_SIZE, FTL_PAGE_CFG_META()) != FTL_OK)
    {
        LOGW("[JRN] replay: page blk=%u page=%u unreadable\r\n",
             (unsigned)page->block,
             (unsigned)page->page);
        return 0U;
    }

    uint32_t applied = 0U;

    for (uint16_t k = 0U; k < page->entry_count; k++)
    {
        const uint16_t offset = (uint16_t)(JOURNAL_HEADER_SIZE + sizeof(l2p_journal_entry_t) * k);

        uint32_t entry_lba;
        uint32_t entry_old_pba;
        uint32_t entry_new_pba;
        uint32_t entry_seq;
        memcpy(&entry_lba, page_data + offset + 0, 4);
        memcpy(&entry_old_pba, page_data + offset + 4, 4);
        memcpy(&entry_new_pba, page_data + offset + 8, 4);
        memcpy(&entry_seq, page_data + offset + 12, 4);

        if ((entry_lba != (uint32_t)FTL_LBA_INVALID) && ((uint64_t)entry_lba < L2P_GLOBAL_LBA_MAX) &&
            lba_routes_to((lba_t)entry_lba, journal->topo))
        {
            if (l2p_cache_write_sync(cache, (lba_t)entry_lba, (nand_pba_t)entry_new_pba) == FTL_OK)
            {
                applied++;
            }
            else
            {
                LOGW("[JRN] replay: L2P write failed lba=%lu\r\n", entry_lba);
                if (block_ctx != NULL)
                {
                    block_ctx->ok = 0U;
                }
            }
        }

        /* 块元数据增量：只对**快照之后**的条目记 valid_count。
         * 整页回放（含水位之前的条目）是幂等的映射写，不能整页记增量。
         * materialize 由开机恢复模块注入——journal 不反向依赖 pfr。 */
        if ((block_ctx != NULL) && (block_ctx->ctx != NULL) && (entry_seq >= cp_sequence) &&
            (block_ctx->materialize != NULL) && (block_ctx->valid_add != NULL) &&
            (block_ctx->valid_sub != NULL))
        {
            const nand_pba_t old_pba = (nand_pba_t)entry_old_pba;
            const nand_pba_t new_pba = (nand_pba_t)entry_new_pba;

            if (new_pba != old_pba)
            {
                if (new_pba != NAND_PBA_INVALID)
                {
                    if (block_ctx->materialize(block_ctx->ctx, new_pba))
                    {
                        block_ctx->valid_add(block_ctx->ctx, new_pba, &block_ctx->ok);
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
                        block_ctx->valid_sub(block_ctx->ctx, old_pba, &block_ctx->ok);
                    }
                    else
                    {
                        block_ctx->ok = 0U;
                    }
                }
            }
        }
    }

    return applied;
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
 *     · out_entries 由应用循环按**成功写入 L2P 的条目**累加（读失败页 / 越界 LBA
 *       不计）——不能用 journal 的 RAM 写指针反推：journal_resume 会清 current_entry。
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
    const uint16_t     read_errors  = journal_scan_ring(journal,
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
            applied += jrn_replay_apply_page(journal, cache, &replay_pages[i], cp_sequence, block_ctx);
        }
    }

    /* 恢复写指针与序号（落到最新页之后；floor 兜住「环空 / 序号回退」） */
    journal_resume(journal, &latest, cp_sequence);

    if (out_entries != NULL)
    {
        *out_entries = applied;
    }

    /* 读失败的页 = 可能漏回放的条目。**写态恢复已经完成**（journal_resume 必须执行，
     * 否则写指针会回到 block0/page0 二次编程），但调用者不能把本次回放当成完整结果
     * ——应回退 F2 全盘重建（RISK-BOOT-08）。 */
    if (read_errors != 0U)
    {
        LOGW("[JRN] replay incomplete: %u page(s) unreadable\r\n", (unsigned)read_errors);
        return FTL_ERROR;
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
     * ① 水位校验。
     * 调用者传的是 CP **起点**的 sequence_counter（下一个待分配序号）。若水位**不足以**
     * 覆盖全部已写条目（CP 期间 journal 又前进了 ≥2 条），说明有未 checkpoint 的条目，
     * 而环形里最旧的一块可能仍持有回放需要的条目 ⇒ **不截断**。
     * （sequence_counter == 0 理论上不会出现；一并挡掉，避免 -1 下溢误判。）
     * ⚠ CP 的快照点在**起点**（见 checkpoint.c 的掉电安全论证），故水位判据容易不满足
     *   ——即更少预擦。这是安全方向的取舍：预擦只是磨损优化，跳过它只是让
     *   journal_flush_page 自己擦（每块每圈仍只擦一次）。
     */
    if ((journal->handle.sequence_counter == 0U) || (up_to_sequence < (journal->handle.sequence_counter - 1U)))
    {
        return JRN_TRUNC_NO_TRUNCATE;
    }

    /* ② 待预擦块 = 环形下一块。该块整块的条目都 ≤ up_to_sequence（checkpoint 已把它们
     * 写进 NAND L2P 表），擦掉即完成截断。重复调用（两次 checkpoint 之间无写入）
     * 时 next == pre_erased_block ⇒ 跳过重擦。 */
    const uint8_t next = journal_next_block(journal, journal->handle.current_block);

    if (next == JOURNAL_BLOCK_NONE)
    {
        return JRN_TRUNC_NO_TRUNCATE; /* 环全部不可用：没有可预擦的块 */
    }
    if (next == journal->handle.pre_erased_block)
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

    /* 记账：journal_flush_page 回切到该块时跳过擦除——保证同一块每圈只擦一次。
     * ⚠ 本次未擦（NONE）时**不动** pre_erased_block：覆写成 NONE 会让已预擦的块被
     *   flush_page 再擦一次（双倍磨损回归）。 */
    if (erased_block != JOURNAL_BLOCK_NONE)
    {
        journal->handle.pre_erased_block = erased_block;
    }

    /* 复位水位标志（设计 §7.5.4）：截断即水位已清。与 checkpoint job 的收尾双保险
     * （后者覆盖 truncate 被弱桩替换的纯 RAM 测试构建）。
     * ⚠ 持久性失败 pending 未清时**保留**标志：说明本次 CP 早于失败点，没有把
     *   迁移后的 L2P 全部落盘，必须再触发一次 CP，而不是把水位门关掉。 */
    if (journal->handle.write_fail_pending == 0U)
    {
        journal->handle.emergency         = 0U;
        journal->handle.checkpoint_needed = 0U;
    }
}

void journal_clear_write_failure(l2p_journal_t *journal, uint32_t cp_journal_seq)
{
    ASSERT(journal != NULL);

    /* CP 起点水位 ≥ 失败点 ⇒ 该 CP 在失败之后快照了脏区并把 L2P 落盘；
     * pending 可以清。否则保留，等下一次 CP。 */
    if ((journal->handle.write_fail_pending != 0U) && (cp_journal_seq >= journal->handle.write_fail_seq))
    {
        journal->handle.write_fail_pending = 0U;
    }
}

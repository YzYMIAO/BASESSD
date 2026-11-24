/*
 * journal.c — L2P 增量日志（环形块池 + 页追加 + 掉电回放 + 截断）
 *
 * 职责：
 *   把每次 L2P 更新（lba, old_pba, new_pba）以「页追加」写进 Journal 环形块池，
 *   供掉电后回放恢复；与 Checkpoint 协同做日志回收（truncate）。
 *
 * 与 journal.h 的分工：
 *   journal.h —— 页头格式（JRN_HEADER_SIZE / MAGIC）+ 条目结构 + 环形状态
 *                （l2p_jrn_s）；
 *   本文件     —— 追加 / 刷页 / 回放 / 截断的实现。
 *
 * 写模型：
 *   · 追加进 RAM 页缓冲（page_buf）；页满（JRN_ENTRIES_PER_PAGE）时**不同步落盘**，
 *     交给异步 job（jrn_freeze → ftl_op_submit(FTL_OP_JOURNAL)），完成由
 *     g_op_callbacks[FTL_OP_JOURNAL]（ftl_journal_callback）按 job.phase 推进；
 *   · 块满换环形下一块——**先擦后写**，换块擦除也是 job 的一个 phase（JRN_ERASE）；
 *   · 同 LBA 就地合并（不消耗 sequence）。
 *
 * 屏障语义：GC/WL 的 F10 契约要求「搬移条目先落盘再擦 victim」，走 `MIG_FLUSH_JRN`
 *   的**异步**等待（jrn_handle + 超时）；HIL `fl` 走异步 flush job。
 *   Journal 模块**不提供同步刷页出口**：需要容量的调用者先用 jrn_reserve，不足即
 *   返 FTL_AGAIN 待入账，由 jrn_handle 在后续 service 轮推进。
 *
 * 掉电保护：Journal 页 Random **关闭**、CRC **开启**（由下层 FC 提供 per-CW CRC +
 *   ECC），外加页头 magic + entry_count 合法性 + page_seq 单调三层结构判据（设计 §7.6）。
 *   异步化**不改变**掉电模型：本仓无电容，acked 写本来就容忍丢最后一页 journal 条目
 *   （write-back 攒批）；页满时交给 job 刷，阻塞不落在调用栈里。
 *
 * ⚠ **介质读回不转断言**（assertion_guidelines §六 辨析 C）：replay 读的是 NAND 上的
 *   页，页头可能因掉电撕裂而非法——那是掉电的**预期情形**，必须返回错误/跳过，不能断言。
 *   实例 handle 非空由 ftl_contract_audit 在 boot 期证明；命令派发路径
 *   （ftl_journal_callback）的 phase / 内部句柄前置条件仍用 ASSERT。
 *
 * 文件结构：
 *   1. 页地址辅助     jrn_page_addr / jrn_block_addr / 环形块选择（static）
 *   2. 初始化         jrn_init
 *   3. 追加与刷页     jrn_reserve / jrn_append / jrn_header_write / jrn_freeze
 *   4. 环形擦除       jrn_erase_start / jrn_erase_failed
 *   5. handle/查询    jrn_handle / 查询 / jrn_write_failure_clear /
 *                     jrn_watermark_update
 *   6. 失败记账/完成  jrn_write_failure_note / jrn_complete_* /
 *                     ftl_journal_callback
 *   7. 回放与写指针恢复 jrn_page_scan_latest / jrn_resume / jrn_replay
 *   8. 截断           jrn_truncate_plan / jrn_truncate_commit（两步，异步 CP 用）
 */

#include "journal.h"

#include "assertion.h"
#include "bbt.h" /* bbt_mark_bad（环形块持续擦除失败） */
#if defined(FC_NAND_TEST_ENABLE)
#    include "fault_inject.h" /* 测试镜像专属注入点；生产构建整段裁掉（宏隔离） */
#endif
#include "fc_interface.h"
#include "ftl.h"
#include "ftl_fc_interface.h"
#include "ftl_op.h" /* ftl_op_submit / FTL_OP_JOURNAL */
#include "log.h"
#include "onfi_1_0.h"
#include "ssd_compiler.h"

#include <string.h>

// 同一环形块连续擦除失败多少次后判为不可用。
// 3 次覆盖瞬态擦除失败；持续失败只能来自物理坏块。继续重试没有意义：环只有
// JOURNAL_BLOCK_COUNT 块，必须标坏并跳过，否则 journal 永久停在 need_erase。
#define JRN_ERASE_FAIL_MAX (3U)

// CP 水位阈值（只本模块使用）：≥85% 请求 Checkpoint，≥95% 紧急。
#define JRN_FULL_THRESHOLD (85U)

#define JRN_CRITICAL (95U)

// 每实例 Journal 环形工作区：定义与内存域（DTCM，CPU-only 热数据）都归本模块。
// NOLOAD ⇒ 启动不清零，ftl_init 绑定后对该实例显式 memset。
static l2p_jrn_s s_journal[FTL_DIE_MAX] DTCM_DATA;

// 回放期间的异常计数（jrn_replay 起点清零，结束时各打一行）。
//
// ⚠ 不能逐条打印：掉电回放可能有上千条落在同一个降级 region / 写失败的条目，
// 逐条 WRN 会写满 4 KB UART TX 环（环满时 uart_log_write 会等待）⇒ 拖慢开机，
// 且日志本身失去区分力。汇总成「首个 region / 首条 LBA + 条数」即可定位。
static DWORD s_replay_degraded_entries;

static DWORD s_replay_degraded_region;

static DWORD s_replay_write_fail_entries;

static DWORD s_replay_write_fail_lba;

/* 应用遍（第二遍）页读失败数：扫描遍读成功、应用遍才失败的页同样意味着条目被漏，
 * 必须让 jrn_replay 返错，不能只打一行日志。 */
static DWORD s_replay_page_read_fail;

/* erase kick 失败日志限流（时间优先，无时钟时 64 条兜底）。 */
static DWORD s_erase_kick_log_ms;

static DWORD s_erase_kick_n;

/* 回放工作缓冲：jrn_replay 只在开机、单线程、无重入时调用（与 checkpoint 的
 * s_probe_hdr / s_probe_raw 同法）。放文件级 static 而不是栈上：
 *   · replay_pages = 256 × 16 B = 4 KiB；
 *   · 扫描/应用共用一页 2 KiB。
 * 二者叠加会突破本仓 2 KiB 栈预算。 */
static jrn_page_ref_s s_replay_pages[JOURNAL_BLOCK_COUNT * PAGES_PER_BLOCK];

static BYTE s_scan_page[JRN_PAGE_SIZE];

/* ══════════════════════════════════════════════════════════
 * 3. 追加与刷页
 * ══════════════════════════════════════════════════════════ */

// 前置声明：jrn_append / jrn_reserve（页满时）要用，定义在本节稍后。
static BOOL jrn_freeze(l2p_jrn_s *journal);
static BOOL jrn_ring_block_bad(const l2p_jrn_s *journal, BYTE b);

/* ══════════════════════════════════════════════════════════
 * 1. 页地址辅助
 * ══════════════════════════════════════════════════════════ */

/* ── Journal Block 页地址辅助 ── */
/**
 * @brief 第 b 块第 page 页的 NAND 地址（plane 由页号奇偶决定）。
 *
 * b 是**环形下标**，物理块号来自候选列表（跳过 BAD）。
 * ⚠ 候选列表由 jrn_init 从 alloc->pools->jrn 复制；池保证 4 块在构造时都非 BAD。
 *
 * @param journal Journal
 * @param b       环形块下标（0..JOURNAL_BLOCK_COUNT-1）
 * @param page    块内页号（偶页 P0、奇页 P1）
 * @return 整页 PBA（cw=0）
 */
static DWORD jrn_page_addr(const l2p_jrn_s *journal, BYTE b, BYTE page)
{
    // cw = 0 = 整页；plane 由页号奇偶决定：偶页 P0、奇页 P1（双 Plane 交错）
    const DWORD block = journal->blocks[b];
    const DWORD plane = (page & 1U);

    return pba_make(journal->topo.ch, journal->topo.ce, journal->topo.lun, block, plane, page, 0U);
}

/**
 * @brief 第 b 块的首页地址（擦除用的基址；plane/page 归零，cw=0）。
 *
 * ⚠ 页地址跨 P0/P1（见 jrn_page_addr）⇒ **所有擦除必须用
 * ftl_*_erase_block_multi_plane**，否则 P1 的那一半页不会被擦，环形回绕后二次编程。
 *
 * @param journal Journal
 * @param b       环形块下标
 * @return 块首页 PBA（所有擦除命令的基址）
 */
static DWORD jrn_block_addr(const l2p_jrn_s *journal, BYTE b)
{
    const DWORD block = journal->blocks[b];

    return pba_make(journal->topo.ch, journal->topo.ce, journal->topo.lun, block, 0U, 0U, 0U);
}

/**
 * @brief 环内第一个可用块（空环恢复写指针用）。
 *
 * @param journal Journal
 * @return 第一个可用环形块下标；全部不可用 = JRN_BLOCK_NONE
 */
static BYTE jrn_block_first_usable(const l2p_jrn_s *journal)
{
    for (BYTE b = 0U; b < JOURNAL_BLOCK_COUNT; b++)
    {
        if (jrn_ring_block_bad(journal, b) == false)
        {
            return b;
        }
    }
    return JRN_BLOCK_NONE;
}

/**
 * @brief 当前页缓冲可容纳的条目数（按页型）。
 *
 * @param journal Journal
 * @return JRN_PAGE_SEG ⇒ 508；JRN_PAGE_LBA ⇒ 127
 */
static WORD jrn_page_capacity(const l2p_jrn_s *journal)
{
    return (journal->handle.state.page_kind == JRN_PAGE_SEG) ? (WORD)JRN_SEG_ENTRIES_PER_PAGE : (WORD)JRN_ENTRIES_PER_PAGE;
}

/* ══════════════════════════════════════════════════════════
 * 7. 回放与写指针恢复
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 空片页判定：格式化后的 Journal 环形块是擦除页，带 META/CRC 校验的读会失败；
 * 用 `ftl_page_is_blank` 走解码路径判（ECC 是否成码字 + 整条 CW 的 1 比特占比）。
 * 只有**非空且读失败**的页才是真正不可信的页。
 *
 * @param addr 待判页的 NAND 地址
 * @return true = 该页从未编程（空页）
 *         false = 已编程，或读命令没走通
 *
 * @note 不用 RAW 读 + 数 FF：RAW 读刻意跳过 ECC（判空依据全被跳过），
 *       且只交出 USER 512B，看不出 META/CRC/parity 是否写成。
 */
static SSD_CODE_FAST BOOL jrn_page_raw_blank(DWORD addr)
{
    return ftl_page_is_blank(addr);
}

/**
 * @brief 应用一页 Journal 条目。
 *
 * 开机回放 = 结构性同步：用同步逃生口（自旋泵），不 park、不返 FTL_AGAIN
 * ——此时 superloop 尚未建立，没人推进 job。
 * entry_lba 来自 NAND，是**不可信输入**：先校验再查表（H29）。
 * 写失败 ⇒ LOGW + block_ctx->ok=false（调用者回退 full metadata rebuild）。
 *
 * @param journal     Journal
 * @param cache       共享 L2P cache（映射写入目标）
 * @param page        待回放页引用（block / page / entry_count）
 * @param cp_sequence checkpoint 截断水位（块元数据增量只记 sequence ≥ 它的条目）
 * @param block_ctx   块元数据增量端口（可为 NULL = 不记账）
 * @return 成功写入 L2P 的条目数（读失败页 = 0）
 */
static SSD_CODE_FAST void jrn_replay_note_fail(jrn_replay_ctx_s *c, BYTE reason, DWORD pba)
{
    // 只记**首次**：后续失败是首条的连带结果，记多了反而看不出起点。
    if ((c != NULL) && (c->fail_reason == (BYTE)JRN_REPLAY_FAIL_NONE))
    {
        c->fail_reason = reason;
        c->fail_pba    = pba;
    }
}

/**
 * @brief 环形块是否已不可用：RAM bad_mask ∪ BST BAD（BBT 持久化后重启仍生效）。
 *
 * journal 块跨两个 plane 交错（页号奇偶决定 plane）⇒ 任一 plane 的块 BAD 即跳过整块。
 *
 * @param journal Journal
 * @param b       环形块下标
 * @return true = 已不可用（bad_mask 命中，或任一 plane 的 gblock 为 BAD）
 *         false = 可用
 */
static BOOL jrn_ring_block_bad(const l2p_jrn_s *journal, BYTE b)
{
    if ((journal->handle.state.bad_mask & (1U << b)) != 0U)
    {
        return true;
    }
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        const WORD gblock = ftl_gblock(p, journal->blocks[b]);

        if (ftl_block_is_bad(&journal->alloc->blocks[gblock]))
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief 从 from 之后找下一个可用环形块。
 *
 * @param journal Journal
 * @param from    起始环形块下标（从 from+1 开始搜索，可回绕）
 * @return 下一个可用环形块下标；全部不可用 = JRN_BLOCK_NONE
 */
static BYTE jrn_block_next(const l2p_jrn_s *journal, BYTE from)
{
    // ⚠ 循环上界是 `<`，**不**检查 from 自身：只剩一个可用块时选自己会擦掉
    // 「正在写 / 持有最新页」的块。此时返回 NONE，由调用者置
    // ring_full 安全停写。
    for (BYTE i = 1U; i < JOURNAL_BLOCK_COUNT; i++)
    {
        const BYTE b = ((from + i) % JOURNAL_BLOCK_COUNT);

        if (jrn_ring_block_bad(journal, b) == false)
        {
            return b;
        }
    }
    return JRN_BLOCK_NONE;
}

/**
 * @brief 把页头写进 page_buf（magic + page_seq + entry_count + block_seq）。
 *
 * @param journal Journal。
 *
 * @note hdr_page_seq 由 `sequence_counter − current_entry` **反推**本页首条序号
 * （与 jrn_append 的就地合并约定耦合，二者必须同步）。
 * memcpy 写入以避免 alignment 警告。
 */
static void jrn_header_write(l2p_jrn_s *journal)
{
    const BOOL seg             = (journal->handle.state.page_kind == JRN_PAGE_SEG);
    DWORD      magic           = seg ? JRN_MAGIC_SEG : JRN_MAGIC;
    DWORD      hdr_page_seq    = journal->handle.state.sequence_counter - journal->handle.state.current_entry;
    WORD       hdr_entry_count = journal->handle.state.current_entry;
    WORD       hdr_block_seq   = journal->handle.state.current_block;
    DWORD      hdr_base_lba    = seg ? journal->handle.state.page_base : 0U;

    memcpy(journal->handle.job.page_buf + 0, &magic, sizeof(magic));
    memcpy(journal->handle.job.page_buf + 4, &hdr_page_seq, sizeof(hdr_page_seq));
    memcpy(journal->handle.job.page_buf + 8, &hdr_entry_count, sizeof(hdr_entry_count));
    memcpy(journal->handle.job.page_buf + 10, &hdr_block_seq, sizeof(hdr_block_seq));
    memcpy(journal->handle.job.page_buf + 12, &hdr_base_lba, sizeof(hdr_base_lba));
}

/**
 * @brief 冻结当前页——起一条**异步**页写，并算出写成功后的写指针落点。
 *
 * @param journal Journal。
 *
 * @return true = 已异步起写（调用者不必再同步刷）；
 *         false = 未能起（job 忙 / 池空 / 建链失败），此时 page_buf 与写指针**原样未动**，调用者应返 FTL_AGAIN / 下一轮重试。
 *
 * @note · **单飞行**：phase != JRN_IDLE 直接拒绝——两条写会争同一个页地址。
 * · **待偿擦除也不许写**：need_erase 置位说明当前环形块**还没擦过**（换块后
 * 擦命令没起起来），此时往里写就是往没擦的块编程 ⇒ 同样拒绝。
 * （jrn_handle 会先补擦，再起页写。）
 * · 页写成功后指针才推进（推进点在 ftl_journal_callback），故落点先算好存进 job；
 * 跨块时一并算出 need_erase（先擦后写）。
 * · 数据在 ftl_async_write_page 内**已拷进 FC buffer 池** ⇒ 提交后立刻腾页缓冲，
 * 不需要 ping-pong 快照缓冲。
 * · pre_erased_block 的消费放在**提交成功之后**：提交失败时记账不动，下轮重算。
 */
static BOOL jrn_freeze(l2p_jrn_s *journal)
{
    if ((journal->handle.job.phase != JRN_IDLE) || journal->handle.job.need_erase || journal->handle.state.ring_full)
    {
        return false; /* 环已全部不可用：不再起新页写 */
    }

    jrn_header_write(journal);

    DWORD waddr = jrn_page_addr(journal, journal->handle.state.current_block, journal->handle.state.current_page);

    fc_cmd_s *cmd;
    if (ftl_async_write_page(&cmd, waddr, journal->handle.job.page_buf, JRN_PAGE_SIZE, FTL_PAGE_CFG_META()) != FTL_OK)
    {
        return false; /* 命令池空 / 建链失败 ⇒ 同步回退 */
    }

    // 落点：写成功后指针推进到 (target_block, target_page)；跨块 ⇒ 需擦新块
    BYTE tpage      = (journal->handle.state.current_page + 1U);
    BYTE tblock     = journal->handle.state.current_block;
    BOOL need_erase = false;
    if (tpage >= PAGES_PER_BLOCK)
    {
        tpage  = 0U;
        tblock = jrn_block_next(journal, tblock);

        if (tblock == JRN_BLOCK_NONE)
        {
            // 全部环形块不可用：本页仍写（它属于当前好块），但之后停止写入。
            journal->handle.state.ring_full = true;
            tblock                          = journal->handle.state.current_block;
            LOGE(LOG_TAG_JRN "all ring blocks unusable — journal writes disabled\r\n");
        }
        else
        {
            need_erase = true;
        }
    }

    journal->handle.job.from_block   = journal->handle.state.current_block;
    journal->handle.job.from_page    = journal->handle.state.current_page;
    journal->handle.job.target_block = tblock;
    journal->handle.job.target_page  = tpage;
    journal->handle.job.waddr        = waddr;

    if (ftl_op_submit(cmd, FTL_OP_JOURNAL, journal) != FTL_OK)
    {
        (void)fc_command_release(cmd->cmd_id);
        return false;
    }

    // 提交成功才消费预擦除记账：该块已被 journal_truncate 预擦过 ⇒ 回切时跳过擦除
    // （同一块每圈只擦一次）。
    if (need_erase && (journal->handle.state.pre_erased_block == tblock))
    {
        journal->handle.state.pre_erased_block = JRN_BLOCK_NONE;
        need_erase                             = false;
    }
    journal->handle.job.need_erase = need_erase;
    journal->handle.job.phase      = JRN_WRITE;
    journal->handle.state.page_writes++;

    // 换块与逐页 flush 都不打 INFO：JRN 是高频元数据维护，序号看 info 的 jrn=；
    // 只有真正错误（写失败/擦失败/环满）才输出。
    // 记录本页的序号区间：页写成功后用它推进 WAL 水位。
    journal->handle.state.write_page_seq    = journal->handle.state.sequence_counter - journal->handle.state.current_entry;
    journal->handle.state.write_entry_count = journal->handle.state.current_entry;

    journal->handle.state.current_entry = 0U;
    journal->handle.state.page_kind     = JRN_PAGE_LBA;
    journal->handle.state.page_base     = 0U;
    memset(journal->handle.job.page_buf, 0xFF, sizeof(journal->handle.job.page_buf));
    return true;
}

/* ══════════════════════════════════════════════════════════
 * 4. 环形擦除与坏块
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 为 job.need_erase 起的**异步**换块擦除。
 *
 * @param journal Journal（job.target_block = 待擦块）。
 *
 * @return true = 已起（phase = JRN_ERASE，need_erase 已清）
 *         false = 未起（池空 / 建链失败， need_erase 保留，由 jrn_handle 下轮重试）。
 *
 * @note 擦的是**已推进到**的新环形块，故必须在任何页写进该块之前完成。
 */
static BOOL jrn_erase_start(l2p_jrn_s *journal)
{
    // 环已全部不可用：不再发擦除（发了也只会永远失败）。
    if (journal->handle.state.ring_full)
    {
        return false;
    }

    fc_cmd_s *cmd;
    if (ftl_async_erase_block_multi_plane(&cmd, jrn_block_addr(journal, journal->handle.job.target_block)) != FTL_OK)
    {
        return false;
    }
    if (ftl_op_submit(cmd, FTL_OP_JOURNAL, journal) != FTL_OK)
    {
        (void)fc_command_release(cmd->cmd_id);
        return false;
    }

    journal->handle.job.need_erase = false;
    journal->handle.job.phase      = JRN_ERASE;
    return true;
}

/**
 * @brief 记录一次环形块擦除失败；同一块连续失败达上限即标坏并跳过。
 *
 * @param journal Journal
 * @param block 失败的环形块。
 *
 * @note · 标坏 = RAM bad_mask + （分配器已绑定时）bbt_mark_bad 两个 plane 的块。
 * BBT 持久化后，重启时 bbt_init 会把块置 BAD，jrn_ring_block_bad 直接跳过。
 * · 跳到下一个可用块；若全部不可用 ⇒ ring_full，journal 停止写入（不写未擦块、
 * 也不再无限重试擦除）。
 * · current_block 若正指向该坏块，同步跳到下一块（该块从未写入新数据）。
 */
static void jrn_erase_failed(l2p_jrn_s *journal, BYTE block)
{
    if (journal->handle.state.erase_fail_block != block)
    {
        journal->handle.state.erase_fail_block = block;
        journal->handle.state.erase_fail_count = 0U;
    }
    journal->handle.state.erase_fail_count++;

    if (journal->handle.state.erase_fail_count < JRN_ERASE_FAIL_MAX)
    {
        LOGW(LOG_TAG_JRN "ring block %hhu erase failed (%hhu/%u\r\n", block, journal->handle.state.erase_fail_count, JRN_ERASE_FAIL_MAX);
        return;
    }

    LOGE(LOG_TAG_JRN "ring block %hhu erase failed x%hhu — mark BAD\r\n", block, journal->handle.state.erase_fail_count);

    journal->handle.state.bad_mask |= (1U << block);

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        const WORD gblock = ftl_gblock(p, journal->blocks[block]);

        (void)bbt_mark_bad(journal->alloc, gblock);
    }
    journal->handle.state.erase_fail_count = 0U;
    journal->handle.state.erase_fail_block = JRN_BLOCK_NONE;
    if (journal->handle.state.pre_erased_block == block)
    {
        journal->handle.state.pre_erased_block = JRN_BLOCK_NONE;
    }

    BYTE next = jrn_block_next(journal, block);

    // 候选若是当前写块 ⇒ 不能擦（它持有最新条目）。从当前块之后继续找：环里
    // 真正的轮转目标就是「当前块之后的第一个可用块」。
    if (next == journal->handle.state.current_block)
    {
        next = jrn_block_next(journal, next);
    }

    if (next == JRN_BLOCK_NONE)
    {
        journal->handle.state.ring_full = true;
        LOGE(LOG_TAG_JRN "no other usable ring block — journal writes disabled\r\n");
        return; /* need_erase 保留：绝不能写未擦块 */
    }

    journal->handle.job.target_block = next;
    if (journal->handle.state.current_block == block)
    {
        journal->handle.state.current_block = next;
        journal->handle.state.current_page  = 0U;
    }
}

/* ══════════════════════════════════════════════════════════
 * 6. 失败记账与完成服务
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 记录一次 Journal 页写失败：条目已随 page_buf 清空而丢失。
 *
 * 置 pending 阻止任何 victim 擦除，并强制一次 Checkpoint 把当前 L2P 落盘；
 * pending 由 jrn_write_failure_clear（CP Header 成功后）清除。
 *
 * @param journal Journal
 */
static void jrn_write_failure_note(l2p_jrn_s *journal)
{
    journal->handle.state.write_fail_count++;
    journal->handle.state.write_fail_seq     = journal->handle.state.sequence_counter;
    journal->handle.state.write_fail_pending = true;
    journal->handle.state.checkpoint_needed  = true;
    journal->handle.state.emergency          = true;
}

/**
 * @brief Journal 页写完成（成功 / 失败共用）。两条路径都要推进落点并置 IDLE：
 *
 * **指针推进守卫** —— 只在「写指针仍在起写位置」时推进；若指针已被回放/截断恢复
 * 推过落点，job 的落点已过期，不得再推进。
 * 失败页可能已半编程 ⇒ **绝不复用**；推进到 freeze 算好的下一落点，跨块时
 * need_erase 保留，由 jrn_handle 先擦目标块再写。条目已丢（page_buf 在提交时
 * 清空），由 write_fail_pending 兜住：GC 不会擦 victim，直到 CP 把当前 L2P 落盘。
 *
 * @param journal    Journal
 * @param cmd        已完成命令（函数内释放）
 * @param completion 完成状态（按 status 判成败）
 */
static void jrn_write_complete(l2p_jrn_s *journal, fc_cmd_s *cmd, const fc_cmd_status_s *completion)
{
    // 测试镜像专属注入：把本次页写按失败结算，用于验证「失败页不推进 persisted_seq
    // ⇒ L2P 区域写回被 WAL 闸门挡住」与 write_fail_pending 的恢复路径。
    // 生产构建整段裁掉（宏隔离）；先求值再合成（H28）。
#if defined(FC_NAND_TEST_ENABLE)
    const BOOL injected = fi_journal_fail_take();
#else
    const BOOL injected = false;
#endif
    const BOOL failed = fc_cmd_status_has_error(completion->status) || injected;

    if (failed)
    {
        LOGE(LOG_TAG_JRN "page write failed pba=%08lx status=0x%02x — 跳过失败页\r\n", journal->handle.job.waddr, completion->status);
        jrn_write_failure_note(journal);
    }

    if ((journal->handle.state.current_block == journal->handle.job.from_block) && (journal->handle.state.current_page == journal->handle.job.from_page))
    {
        journal->handle.state.current_block = journal->handle.job.target_block;
        journal->handle.state.current_page  = journal->handle.job.target_page;
    }
    journal->handle.job.phase = JRN_IDLE;

    if (failed == false)
    {
        // WAL 水位：本页成功落盘 ⇒ 它覆盖的序号区间全部持久化。
        const DWORD next_seq = journal->handle.state.write_page_seq + (DWORD)journal->handle.state.write_entry_count;

        if (jrn_seq_after(next_seq, journal->handle.state.persisted_seq))
        {
            journal->handle.state.persisted_seq = next_seq;
        }
    }

    if (failed)
    {
        // 写都没成，不在此起换块擦除：need_erase 保留给 jrn_handle 下轮处理。
        (void)fc_command_release(cmd->cmd_id);
        return;
    }

    // 换块 ⇒ 起异步擦除；起不来就留着 need_erase，由 jrn_handle 下轮重试。
    // ⚠ 先查 pre_erased_block：journal_truncate 可能已在这段窗口里代擦了环形
    // 下一块（正是本 job.target_block）⇒ 消费记账跳过，避免重复擦。
    if (journal->handle.job.need_erase)
    {
        if (journal->handle.state.pre_erased_block == journal->handle.job.target_block)
        {
            journal->handle.state.pre_erased_block = JRN_BLOCK_NONE;
            journal->handle.job.need_erase         = false;
        }
        else if (jrn_erase_start(journal) == false)
        {
            // 背压（池空 / 队列满 / cache 序列门控）是**预期**等待，need_erase 保留、下轮重试。
            // 计数不丢；日志用 WRN 并限流 5 s 一行，避免 host 大顺序写 + GC 期间刷屏。
            journal->handle.state.erase_kick_blocked++;
            LOGW_RATE(log_rate_due(&s_erase_kick_log_ms, &s_erase_kick_n, LOG_RATE_SLOW_INTERVAL_MS), s_erase_kick_n, LOG_TAG_JRN "erase kick blocked blk=%hhu — 下轮重试", journal->handle.job.target_block);
        }
    }

    (void)fc_command_release(cmd->cmd_id);
}

/**
 * @brief Journal 环形块擦完成。
 *
 * 失败判据必须是全集：只看 NAND_FAIL 会把 TIMEOUT / CRC 当成擦成功，
 * 后续页写进可能未擦的块（二次编程 = 静默损坏）。
 *
 * @param journal    Journal
 * @param cmd        已完成命令（函数内释放）
 * @param completion 完成状态（按 status 判成败）
 */
static void jrn_erase_complete(l2p_jrn_s *journal, fc_cmd_s *cmd, const fc_cmd_status_s *completion)
{
    if (fc_cmd_status_has_error(completion->status))
    {
        LOGE(LOG_TAG_JRN "ring block erase failed blk=%hhu status=0x%02hx — 重新挂起\r\n", journal->handle.job.target_block, completion->status);
        // 擦失败 ⇒ 目标块仍是脏的；绝不能把后续页写进去。重新挂起 need_erase，
        // 由 jrn_handle 下轮重试；持续失败由 jrn_erase_failed 标坏并跳过。
        jrn_erase_failed(journal, journal->handle.job.target_block);
        journal->handle.job.need_erase = true;
    }
    else
    {
        // 擦成功：连续失败计数归零（只统计**连续**失败）。
        journal->handle.state.erase_fail_count = 0U;
        journal->handle.state.erase_fail_block = JRN_BLOCK_NONE;
    }
    journal->handle.job.phase = JRN_IDLE;
    (void)fc_command_release(cmd->cmd_id);
}

/**
 * @brief 扫一遍环形块池——找**最新页**（最大 page_seq），并按需收集需回放页。
 *
 * @param journal Journal
 * @param cp_sequence 回放水位。
 * @param out_pages   收集数组（NULL = 只找最新页）。
 * @param capacity    收集数组容量。
 * @param out_count 出参收集条数（可为 NULL）
 * @param out_latest 出参最新页（可为 NULL）。
 *
 * @return 环内非空页的读失败数（0 = 全部可读）
 *
 * @note 只读 NAND，**不改 journal 状态**（怎么用由调用方决定）。
 * 需回放判据是**整页**的：`page_seq + entry_count > cp_sequence`。
 * 只看页首页序号会把跨水位的那一页整页跳过——它页尾的新条目
 * （> cp_sequence）随页面一起丢。
 * 页头 magic / entry_count 校验失败即跳过（掉电撕裂的预期情形，见文件头 ⚠）。
 */
static SSD_CODE_FAST WORD jrn_ring_scan(const l2p_jrn_s *journal, DWORD cp_sequence, jrn_page_ref_s *out_pages, WORD capacity, WORD *out_count, jrn_page_ref_s *out_latest)
{
    if (out_latest != NULL)
    {
        out_latest->page_seq    = 0U;
        out_latest->base_lba    = 0U;
        out_latest->block       = 0U;
        out_latest->page        = 0U;
        out_latest->entry_count = 0U;
        out_latest->kind        = JRN_PAGE_LBA;
    }

    WORD count       = 0U;
    WORD read_errors = 0U;

    for (BYTE b = 0U; b < JOURNAL_BLOCK_COUNT; b++)
    {
        // 已判坏的环形块不扫：它的读必然失败，会把整次回放标成“不完整”并逼出 full metadata rebuild。
        // 坏块上的条目本来就不该再被信任。
        if (jrn_ring_block_bad(journal, b))
        {
            continue;
        }

        for (BYTE page = 0U; page < PAGES_PER_BLOCK; page++)
        {
            // 文件级缓冲：本函数只在开机单线程调用（见 s_replay_pages 注释）。
            BYTE *page_data = s_scan_page;

            memset(page_data, 0xFF, JRN_PAGE_SIZE);

            DWORD raddr = jrn_page_addr(journal, b, page);
            if (ftl_sync_read_page(raddr, page_data, JRN_PAGE_SIZE, FTL_PAGE_CFG_META()) != FTL_OK)
            {
                if (jrn_page_raw_blank(raddr) == false)
                {
                    read_errors++; /* 非空页读失败：真正的不可信页 */
                }
                continue;
            }

            jrn_page_kind_e kind;
            DWORD           magic;

            memcpy(&magic, page_data + 0, sizeof(magic));
            if (magic == JRN_MAGIC)
            {
                kind = JRN_PAGE_LBA;
            }
            else if (magic == JRN_MAGIC_SEG)
            {
                kind = JRN_PAGE_SEG;
            }
            else
            {
                continue; /* 非 Journal 页 */
            }

            DWORD page_seq;
            WORD  entry_count;
            memcpy(&page_seq, page_data + 4, sizeof(page_seq));
            memcpy(&entry_count, page_data + 8, sizeof(entry_count));

            const WORD page_capacity = (kind == JRN_PAGE_SEG) ? (WORD)JRN_SEG_ENTRIES_PER_PAGE : (WORD)JRN_ENTRIES_PER_PAGE;

            if ((entry_count == 0U) || (entry_count > page_capacity))
            {
                continue; /* 空页 / 非法 count（介质读回，不可信） */
            }

            DWORD base_lba;
            memcpy(&base_lba, page_data + 12, sizeof(base_lba));

            // 顺序页的隐式 LBA 区间必须整体落在 L2P 键空间内。base_lba 来自介质
            // （不可信输入，H29）：不校验的话 base 接近 UINT32_MAX 时 base+k 回绕成
            // 一个小合法 LBA，回放会把映射写到错误的 LBA（H31 越界判据的同类）。
            if ((kind == JRN_PAGE_SEG) && (((QWORD)base_lba + entry_count) > (QWORD)L2P_GLOBAL_LBA_MAX))
            {
                continue;
            }

            // 最新页（用于恢复写指针与序号）：取 page_seq 最大者（串行号语义，
            // 回绕安全；entry_count == 0 表示尚未见过有效页）。
            if ((out_latest != NULL) && ((out_latest->entry_count == 0U) || jrn_seq_after(page_seq, out_latest->page_seq)))
            {
                out_latest->page_seq    = page_seq;
                out_latest->base_lba    = base_lba;
                out_latest->block       = b;
                out_latest->page        = page;
                out_latest->entry_count = entry_count;
                out_latest->kind        = kind;
            }

            // 需回放页：本页**最后一条**的序号已过水位 ⇒ 页内至少一条没进 L2P 表
            if ((out_pages != NULL) && (count < capacity) && jrn_seq_after((DWORD)(page_seq + entry_count), cp_sequence))
            {
                out_pages[count].page_seq    = page_seq;
                out_pages[count].base_lba    = base_lba;
                out_pages[count].block       = b;
                out_pages[count].page        = page;
                out_pages[count].entry_count = entry_count;
                out_pages[count].kind        = kind;
                count++;
            }
        }
    }

    if (out_count != NULL)
    {
        *out_count = count;
    }

    // 读失败的页 = 看不到的条目。调用者必须据此决定是否还能信任回放结果
    // （不能静默当「无更多条目」）。
    return read_errors;
}

/**
 * @brief 应用单页回放：按 META 的 LBA 写 L2P，跳过读失败/越界条目
 *
 * @param journal     Journal
 * @param cache       L2P cache
 * @param page        待回放页（block/page）
 * @param cp_sequence CP 水位；sequence ≤ 水位的条目不回放
 * @param block_ctx   回放上下文（记录首次失败）
 * @return 成功写入 L2P 的条目数
 */
static SSD_CODE_FAST DWORD jrn_page_replay_apply(l2p_jrn_s *journal, l2p_cache_s *cache, const jrn_page_ref_s *page, DWORD cp_sequence, jrn_replay_ctx_s *block_ctx)
{
    DWORD raddr = jrn_page_addr(journal, page->block, page->page);

    // 与 jrn_ring_scan 共用文件级扫描页：两者不会同时在用（先扫完再逐页应用）。
    BYTE *page_data = s_scan_page;

    memset(page_data, 0xFF, JRN_PAGE_SIZE);

#if defined(FC_NAND_TEST_ENABLE)
    /* D2/JN-08：只打应用遍，模拟“扫描可读、应用遍读失败”的瞬态。 */
    if (fi_take(FI_JRN_REPLAY_APPLY_FAIL) == true)
    {
        s_replay_page_read_fail++;
        return 0U;
    }
#endif

    if (ftl_sync_read_page(raddr, page_data, JRN_PAGE_SIZE, FTL_PAGE_CFG_META()) != FTL_OK)
    {
        LOGW(LOG_TAG_JRN "replay: page blk=%hhu page=%hhu unreadable\r\n", page->block, page->page);
        s_replay_page_read_fail++;
        return 0U;
    }

    DWORD applied = 0U;

    for (WORD k = 0U; k < page->entry_count; k++)
    {
        DWORD entry_lba;
        DWORD entry_old_pba;
        DWORD entry_new_pba;
        DWORD entry_seq;

        if (page->kind == JRN_PAGE_SEG)
        {
            // 顺序页：LBA / 序号都由页头 + 下标隐式给出，只落盘 new_pba。
            entry_lba = page->base_lba + k;
            entry_seq = page->page_seq + k;
            memcpy(&entry_new_pba, page_data + JRN_HEADER_SIZE + (sizeof(l2p_jrn_seg_entry_t) * k), 4);

            // 被替换的映射从 L2P 基线取（与确认路径"完成时重查 L2P"同源）：
            // 回放严格按 (page_seq, 下标) 递增序应用，读到的就是确认时刻的旧值。
            // 只在需要记块元数据增量时读（水位之前的条目是幂等映射写，不记账）。
            entry_old_pba = NAND_PBA_INVALID;
            if ((block_ctx != NULL) && (block_ctx->ctx != NULL) && jrn_seq_ge(entry_seq, cp_sequence))
            {
                if (l2p_read_sync(cache, entry_lba, &entry_old_pba) != FTL_OK)
                {
                    block_ctx->ok = false;
                    jrn_replay_note_fail(block_ctx, (BYTE)JRN_REPLAY_FAIL_L2P_READ, entry_new_pba);
                    entry_old_pba = NAND_PBA_INVALID;
                }
            }
        }
        else
        {
            const WORD offset = (JRN_HEADER_SIZE + sizeof(l2p_jrn_entry_s) * k);

            memcpy(&entry_lba, page_data + offset + 0, 4);
            memcpy(&entry_old_pba, page_data + offset + 4, 4);
            memcpy(&entry_new_pba, page_data + offset + 8, 4);
            memcpy(&entry_seq, page_data + offset + 12, 4);
        }

        if ((entry_lba != FTL_LBA_INVALID) && ((QWORD)entry_lba < L2P_GLOBAL_LBA_MAX) && lba_routes_to(entry_lba, journal->topo))
        {
            if (l2p_write_sync(cache, entry_lba, entry_new_pba) == FTL_OK)
            {
                applied++;

                // 基线若是“降级后的空表”，该 region 的旧映射已不可信：快照结果
                // 不能算 restored。
                const DWORD global_region = lba_to_global_region(entry_lba);

                if (l2p_is_region_degraded(cache, global_region))
                {
                    // 降级基线只丢「该 region 未被 Journal 覆盖的旧映射」，**不影响**
                    // valid_count 增量（old/new PBA 都来自条目本身）⇒ 不再让整份
                    // 块元数据快照失效（范围化恢复）。
                    // 丢失的映射由 host 读路径按 IO_ERROR 暴露（不能静默返回零）。
                    s_replay_degraded_entries++;
                    if (s_replay_degraded_region == UINT32_MAX)
                    {
                        s_replay_degraded_region = global_region;
                    }
                }
            }
            else
            {
                s_replay_write_fail_entries++;
                if (s_replay_write_fail_lba == FTL_LBA_INVALID)
                {
                    s_replay_write_fail_lba = entry_lba;
                }
                if (block_ctx != NULL)
                {
                    block_ctx->ok = false;
                    jrn_replay_note_fail(block_ctx, (BYTE)JRN_REPLAY_FAIL_L2P_WRITE, entry_new_pba);
                }
            }
        }

        // 块元数据增量：只对**快照之后**的条目记 valid_count。
        // 整页回放（含水位之前的条目）是幂等的映射写，不能整页记增量。
        // materialize 由开机恢复模块注入——journal 不反向依赖 pfr。
        if ((block_ctx != NULL) && (block_ctx->ctx != NULL) && jrn_seq_ge(entry_seq, cp_sequence) && (block_ctx->materialize != NULL) && (block_ctx->valid_add != NULL) && (block_ctx->valid_sub != NULL))
        {
            const DWORD old_pba = entry_old_pba;
            const DWORD new_pba = entry_new_pba;

            if (new_pba != old_pba)
            {
                if (new_pba != NAND_PBA_INVALID)
                {
                    if (block_ctx->materialize(block_ctx->ctx, new_pba))
                    {
                        BOOL ok_one = true;

                        block_ctx->valid_add(block_ctx->ctx, new_pba, &ok_one);
                        if (ok_one == false)
                        {
                            block_ctx->ok = false;
                            jrn_replay_note_fail(block_ctx, (BYTE)JRN_REPLAY_FAIL_VALID_ADD, new_pba);
                        }
                    }
                    else
                    {
                        block_ctx->ok = false;
                        jrn_replay_note_fail(block_ctx, (BYTE)JRN_REPLAY_FAIL_MATERIALIZE, new_pba);
                    }
                }
                if (old_pba != NAND_PBA_INVALID)
                {
                    // ⚠ **不**对 old 调 materialize：它会把池块/日志块转成 CLOSED 并清 0
                    // 计数——既让随后的减法撞下溢，又把这颗块从池里摘掉（容量损失）。
                    // old 只做减法；计数已为 0 时由 pfr_valid_sub 按运行期同口径跳过。
                    BOOL ok_one = true;

                    block_ctx->valid_sub(block_ctx->ctx, old_pba, &ok_one);
                    if (ok_one == false)
                    {
                        block_ctx->ok = false;
                        jrn_replay_note_fail(block_ctx, (BYTE)JRN_REPLAY_FAIL_VALID_SUB, old_pba);
                    }
                }
            }
        }
    }

    return applied;
}

/**
 * @brief 取本实例的 Journal 环形工作区（定义在 journal.c，DTCM）
 *
 * @param die 紧凑 die 号（< FTL_DIE_MAX）
 * @return 本实例句柄（非空）
 */
l2p_jrn_s *jrn_workspace_get(BYTE die)
{
    ASSERT(die < FTL_DIE_MAX);
    return &s_journal[die];
}

/* ══════════════════════════════════════════════════════════
 * 2. 初始化
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 初始化 Journal：绑拓扑/allocator、复位环形游标与页缓冲
 *
 * sequence 从 1 起（0 = 无序号）；page_buf 填 0xFF = 空页（与擦除态一致）。
 *
 * @param journal Journal
 * @param topo    本实例拓扑
 * @param alloc   分配器（持续擦除失败 → 标坏）
 */
void jrn_init(l2p_jrn_s *journal, ftl_topo_s topo, alc_s *alloc)
{
    // 前置条件：唯一调用点 ftl_init 传 &ftl->alloc（调用链已保证非空，不重复 ASSERT，H2/H49）。

    journal->topo  = topo;
    journal->alloc = alloc;

    // 环形候选块：优先用 md_pool_build_all() 的跳过-BAD 池；池未建立时退回名义区间。
    // 固定 JOURNAL_BLOCK_COUNT 项——环形容量契约要求 4 块（池构建不满足即失败）。
    if ((alloc->pools != NULL) && (alloc->pools->jrn.block_count >= JOURNAL_BLOCK_COUNT))
    {
        for (BYTE b = 0U; b < JOURNAL_BLOCK_COUNT; b++)
        {
            journal->blocks[b] = alloc->pools->jrn.blocks[b];
        }
    }
    else
    {
        for (BYTE b = 0U; b < JOURNAL_BLOCK_COUNT; b++)
        {
            journal->blocks[b] = (WORD)(JOURNAL_BLOCK0 + b);
        }
    }

    journal->handle.state.current_block     = 0U;
    journal->handle.state.current_page      = 0U;
    journal->handle.state.current_entry     = 0U;
    journal->handle.state.sequence_counter  = 1U;
    journal->handle.state.persisted_seq     = 1U; /* 空环：序号 1 之前“全部已落盘”（无条目） */
    journal->handle.state.write_page_seq    = 1U;
    journal->handle.state.write_entry_count = 0U;
    journal->handle.state.merge_floor       = 0U;
    journal->handle.state.pre_erased_block  = JRN_BLOCK_NONE;
    journal->handle.state.page_kind         = JRN_PAGE_LBA;
    journal->handle.state.page_base         = 0U;

    memset(journal->handle.job.page_buf, 0xFF, sizeof(journal->handle.job.page_buf));

    journal->handle.state.checkpoint_needed = false;
    journal->handle.state.emergency         = false;

    journal->handle.state.write_fail_count   = 0U;
    journal->handle.state.write_fail_seq     = 0U;
    journal->handle.state.write_fail_pending = false;

    // 异步刷页 job 复位：无在飞、无待偿擦除
    journal->handle.job.phase         = JRN_IDLE;
    journal->handle.job.need_erase    = false;
    journal->handle.state.page_writes = 0U;
}

/**
 * @brief 为一页追加预留 entries 条容量
 *
 * 不阻塞：页满只起异步 jrn_freeze；起不来留给 jrn_handle 下轮刷。
 *
 * @param journal Journal
 * @param entries 本次要追加的条数
 * @return true 已预留；false 当前页放不下（调用者待入账后重试）
 */
BOOL jrn_reserve(l2p_jrn_s *journal, WORD entries)
{
    ASSERT(entries < JRN_ENTRIES_PER_PAGE);

    if (entries == 0U)
    {
        return true;
    }

    // 环全部不可用：页缓冲永远刷不出去，预留成功只会制造“看起来记下了”的假象。
    if (journal->handle.state.ring_full)
    {
        return false;
    }

    if (journal->handle.state.page_kind != JRN_PAGE_LBA)
    {
        (void)jrn_freeze(journal);
        return false;
    }

    if (((DWORD)journal->handle.state.current_entry + entries) < JRN_ENTRIES_PER_PAGE)
    {
        return true;
    }

    // 当前页剩余槽位不够：只起异步刷页。失败原因（在飞/待擦/池空）都是瞬态，
    // 由 jrn_handle 在后续轮次推进；调用者待入账重试。
    if (jrn_freeze(journal) == false)
    {
        return false;
    }

    // freeze 成功会清空页缓冲；但若它同时发现环内已无可用块（ring_full），
    // 新页永远不会被 jrn_handle 刷出，不能接受追加。
    if (journal->handle.state.ring_full)
    {
        return false;
    }

    return ((DWORD)journal->handle.state.current_entry + entries) < JRN_ENTRIES_PER_PAGE;
}

/**
 * @brief 追加一条通用映射日志（16B）
 *
 * 页满只异步刷页；起不来时不同步回退（会重入完成派发）。需要不丢条目的调用者
 * 先 jrn_reserve。
 *
 * @param journal Journal
 * @param lba     host LBA
 * @param old_pba 旧 PBA
 * @param new_pba 新 PBA
 * @return true 已追加；false 环满 / 页满且刷不动（后续追加丢弃并 LOGE）
 */
BOOL jrn_append(l2p_jrn_s *journal, DWORD lba, DWORD old_pba, DWORD new_pba)
{
    // 环全部不可用：当前页缓冲永远刷不出去，追加只会制造“看起来记下了”的假象。
    if (journal->handle.state.ring_full)
    {
        return false;
    }

    // 顺序页只装顺序条目（4B，无 LBA）；普通条目必须等它刷出后换页。
    // jrn_reserve 已先处理（含空页刷出），这里只留防御。
    if (journal->handle.state.page_kind != JRN_PAGE_LBA)
    {
        return false;
    }

    // 更新合并：当前页缓冲里已有同一 LBA 的条目 ⇒ 就地更新，不新增条目。
    // 回放按页序 + 页内下标应用（per-LBA last-write-wins），页内同 LBA 只保留最新
    // new_pba 即正确；old_pba 仅首条保留（回放的块元数据增量用它做 valid_sub，
    // 中间被覆盖的 new_pba 在运行时已 +1/−1 抵消，回放只需首条 old 与末条 new）。
    // ⚠ **不**消耗 sequence、**不**推进 current_entry：hdr_page_seq 由
    // `sequence_counter − current_entry` 反推本页首条序号，二者必须同步推进，
    // 否则页序错乱会导致回放顺序错误。
    // ⚠ 合并放在「页满守卫」之前：合并**不占新槽位**，页满时仍应更新最新映射。
    l2p_jrn_entry_s *entry;
    for (WORD i = 0U; i < journal->handle.state.current_entry; i++)
    {
        entry = (l2p_jrn_entry_s *)(journal->handle.job.page_buf + JRN_HEADER_SIZE + sizeof(l2p_jrn_entry_s) * i);
        // CP 边界之前的条目不得被合并：merge_floor 之后的覆盖写另起一条，
        // 否则 sequence 仍是旧的、new_pba 却是新的，块元数据 delta 回放会漏记。
        if ((entry->lba == lba) && jrn_seq_ge(entry->sequence, journal->handle.state.merge_floor))
        {
            entry->new_pba = new_pba; /* 保留首条 old_pba，更新到最新 new_pba */
            return true;
        }
    }

    // 页已满且上一轮异步刷页未成功（job 忙 / 池空）⇒ 本条无处可放，丢弃。
    // ⚠ 守卫必须在按 current_entry 取槽位**之前**：current_entry ==
    // JRN_ENTRIES_PER_PAGE（127）时第 127 条的偏移已是 `16 + 127×16 = 2048`，越界
    // 会写出 page_buf 之外（踩到 jrn_handle_s）。
    // 丢弃的后果与「掉电丢最后一页 journal」同级（回放少应用一次映射更新）；
    // 整页由 jrn_handle 下轮刷掉后自动恢复。
    if (journal->handle.state.current_entry >= JRN_ENTRIES_PER_PAGE)
    {
        LOGE(LOG_TAG_JRN "page full & async flush pending — 丢弃条目 lba=%lu\r\n", lba);
        return false;
    }

    entry = (l2p_jrn_entry_s *)(journal->handle.job.page_buf + JRN_HEADER_SIZE + sizeof(l2p_jrn_entry_s) * journal->handle.state.current_entry);

    entry->lba      = lba;
    entry->old_pba  = old_pba;
    entry->new_pba  = new_pba;
    entry->sequence = journal->handle.state.sequence_counter++;

    journal->handle.state.current_entry++;

    if (journal->handle.state.current_entry >= JRN_ENTRIES_PER_PAGE)
    {
        // 页已满：只尝试起异步刷页，**任何上下文都不同步回退**——本函数会被
        // completion/service 路径调用，同步刷页会重入完成派发。
        // 本条已写入页缓冲；freeze 失败（在飞/待擦/池空）时后续 append 走上面的
        // 页满守卫丢弃并 LOGE，整页由 jrn_handle 后续轮刷出。
        // 需要保证不丢条目的调用者应先 jrn_reserve。
        (void)jrn_freeze(journal);
    }

    return true;
}

/* ── 顺序流映射页 ── */

/**
 * @brief 开启/续用一张顺序映射页
 *
 * 页型对、空间够且 LBA 连续才续；否则先刷旧页再换新页。
 *
 * @param journal Journal
 * @param lba     首条 LBA
 * @param need    本段需要的条目数
 * @return true 可写入；false 需先刷页（调用者下轮重试）
 */
BOOL jrn_seq_begin(l2p_jrn_s *journal, DWORD lba, WORD need)
{
    if (journal->handle.state.ring_full)
    {
        return false;
    }

    const DWORD used = journal->handle.state.current_entry;
    const DWORD room = (used < JRN_SEG_ENTRIES_PER_PAGE) ? (JRN_SEG_ENTRIES_PER_PAGE - used) : 0U;

    // 跨命令续用同一张顺序页：页型对、已有条目、空间够本段、LBA 连续。
    // 这是"一张页填满再断"的落点——不因 host 命令边界刷页。
    if ((journal->handle.state.page_kind == JRN_PAGE_SEG) && (used != 0U) && (room >= need) && (lba == (journal->handle.state.page_base + used)))
    {
        return true;
    }

    if (used != 0U)
    {
        // 其他情况（LBA 不连续 / 空间不够本段 / 上一段尾页）：先异步刷出旧页。
        // freeze 成功后页缓冲归零、页型复位；这个动作就是设计中「END 之后落元数据」。
        if (jrn_freeze(journal) == false)
        {
            return false;
        }
    }

    journal->handle.state.page_kind = JRN_PAGE_SEG;
    journal->handle.state.page_base = 0U; /* 基准由首条 append 落定（见 jrn_handle_s 注释） */
    return true;
}

/**
 * @brief 当前顺序页还能装几条
 *
 * @param journal Journal
 * @return 剩余条数；非顺序页 / 已满返回 0
 */
WORD jrn_seq_room(const l2p_jrn_s *journal)
{
    if (journal->handle.state.page_kind != JRN_PAGE_SEG)
    {
        return 0U;
    }
    if (journal->handle.state.current_entry >= JRN_SEG_ENTRIES_PER_PAGE)
    {
        return 0U;
    }

    return (WORD)(JRN_SEG_ENTRIES_PER_PAGE - journal->handle.state.current_entry);
}

/**
 * @brief 预判本段能否放进当前页（不刷页、不改状态）
 *
 * @param journal Journal
 * @param entries 本段条目数
 * @return true 放得下；false 不足（调用者待入账后重试）
 */
BOOL jrn_seq_reserve(l2p_jrn_s *journal, WORD entries)
{
    ASSERT(entries < JRN_SEG_ENTRIES_PER_PAGE);

    if (entries == 0U)
    {
        return true;
    }
    if (journal->handle.state.ring_full)
    {
        return false;
    }

    if (journal->handle.state.page_kind == JRN_PAGE_SEG)
    {
        return (((DWORD)journal->handle.state.current_entry + entries) < JRN_SEG_ENTRIES_PER_PAGE);
    }

    return (journal->handle.state.current_entry == 0U); /* 空普通页可转顺序页；非空 ⇒ 本窗口必须 END */
}

/**
 * @brief 追加前检查：页型/空间/LBA 连续
 *
 * 任一不满足就先刷页并返回 false（调用者下轮重试）。
 *
 * @param journal Journal
 * @param lba     首条 LBA
 * @param entries 本段条目数
 * @return true 可直接追加；false 需先刷页
 */
BOOL jrn_seq_ready(l2p_jrn_s *journal, DWORD lba, WORD entries)
{
    ASSERT(entries < JRN_SEG_ENTRIES_PER_PAGE);

    if (entries == 0U)
    {
        return true;
    }
    if (journal->handle.state.ring_full)
    {
        return false;
    }

    const DWORD capacity = jrn_page_capacity(journal);

    if (journal->handle.state.page_kind == JRN_PAGE_SEG)
    {
        // 隐式坐标必须全批可追加：首条（本页尚无条目）任意，其余必须紧接已追加条目。
        // 坐标断裂时刷页换页 —— 调用者待入账后重试会落到一张新页（基准重新落定）。
        if ((journal->handle.state.current_entry != 0U) && (lba != (journal->handle.state.page_base + journal->handle.state.current_entry)))
        {
            (void)jrn_freeze(journal);
            return false;
        }
        if (((DWORD)journal->handle.state.current_entry + entries) < capacity)
        {
            return true;
        }
        (void)jrn_freeze(journal);
        return false;
    }

    if (journal->handle.state.current_entry == 0U)
    {
        return true; /* 空页：jrn_append_seq 会把它定为顺序页 */
    }

    // 普通页上有未落盘条目：不能隐式编码，先刷出（调用者下轮重试）。
    (void)jrn_freeze(journal);
    return false;
}

/**
 * @brief 追加一条顺序映射条目（4B，LBA 隐式 = 页基准 + 下标）
 *
 * 页满才起异步刷页，不在此同步刷。
 *
 * @param journal Journal
 * @param lba     host LBA
 * @param new_pba 新 PBA
 * @return true 已追加；false 页满且刷不动（丢弃并 LOGE）
 */
BOOL jrn_append_seq(l2p_jrn_s *journal, DWORD lba, DWORD new_pba)
{
    if (journal->handle.state.ring_full)
    {
        return false;
    }

    if (journal->handle.state.current_entry == 0U)
    {
        // 空白页：本页类型与基准 LBA 由首条条目落定。顺序确认按提交序到达，
        // 因此第一条确认的 LBA 就是本页的隐式起点。
        journal->handle.state.page_kind = JRN_PAGE_SEG;
        journal->handle.state.page_base = lba;
    }

    if ((journal->handle.state.page_kind != JRN_PAGE_SEG) || (lba != (journal->handle.state.page_base + journal->handle.state.current_entry)))
    {
        return false; /* 隐式坐标断裂（jrn_seq_reserve 应已判掉；防御） */
    }

    if (journal->handle.state.current_entry >= JRN_SEG_ENTRIES_PER_PAGE)
    {
        (void)jrn_freeze(journal);
        return false;
    }

    DWORD *slot = (DWORD *)(void *)(journal->handle.job.page_buf + JRN_HEADER_SIZE + (sizeof(l2p_jrn_seg_entry_t) * journal->handle.state.current_entry));

    *slot = new_pba;
    journal->handle.state.current_entry++;
    journal->handle.state.sequence_counter++;

    if (journal->handle.state.current_entry >= JRN_SEG_ENTRIES_PER_PAGE)
    {
        (void)jrn_freeze(journal);
    }

    return true;
}

/* ══════════════════════════════════════════════════════════
 * 5. handle / 查询 / 水位
 * ══════════════════════════════════════════════════════════ */

/* ── 状态上报（主流程只读这个，不窥探 handle 内部） ── */

/**
 * @brief 推进 Journal job：刷页 / 换块擦除
 *
 * 单飞行：在飞只心跳；有未起擦除时优先补擦（新块未擦不能写）。
 *
 * @param journal Journal
 * @param trigger 触发信息（可 NULL = 服务轮）
 */
void jrn_handle(l2p_jrn_s *journal, const jrn_trigger_e *trigger)
{
    const BOOL erase_only = (trigger != NULL) && (*trigger == JRN_TRIGGER_ERASE_ONLY);

    if (journal->handle.state.ring_full)
    {
        return; /* 环全部不可用：不重试擦除（已 LOGE 过），也不写未擦块 */
    }

    if (journal->handle.job.phase != JRN_IDLE)
    {
        // 心跳（见 log.h / timer.h）：只在 age >= 5 s 时打一行；异步后「卡在哪个 phase」的唯一观测口
        if (journal->handle.job.hb.active == false)
        {
            log_heartbeat_begin(&journal->handle.job.hb);
        }
        else if ((log_heartbeat_age_ms(&journal->handle.job.hb) >= LOG_RATE_SLOW_AFTER_MS) && log_heartbeat_due(&journal->handle.job.hb))
        {
            LOGI(LOG_TAG_JRN "job phase=%d waddr=0x%08lx age=%lums\r\n", journal->handle.job.phase, journal->handle.job.waddr, log_heartbeat_age_ms(&journal->handle.job.hb));
        }

        return; /* 单飞行：job 已在飞 */
    }

    log_heartbeat_end(&journal->handle.job.hb);

    if (journal->handle.job.need_erase)
    {
        (void)jrn_erase_start(journal);

        return;
    }

    if (erase_only || (journal->handle.state.current_entry == 0U))
    {
        return; /* ERASE_ONLY 不起页写；无待写数据也到此为止 */
    }

    (void)jrn_freeze(journal);
}

/* ── 查询（替代外部直读 handle 字段） ── */
/**
 * @brief 尝试清除持久性失败标志
 *
 * CP 起点水位 ≥ 失败点才清；否则保留等下一次 CP。
 *
 * @param journal        Journal
 * @param cp_journal_seq 本次 CP 的 journal 水位
 */
void jrn_write_failure_clear(l2p_jrn_s *journal, DWORD cp_journal_seq)
{
    // CP 起点水位 ≥ 失败点 ⇒ 该 CP 在失败之后快照了脏区并把 L2P 落盘；
    // pending 可以清。否则保留，等下一次 CP。
    if (journal->handle.state.write_fail_pending && jrn_seq_ge(cp_journal_seq, journal->handle.state.write_fail_seq))
    {
        journal->handle.state.write_fail_pending = false;
    }
}

/**
 * @brief 置 Checkpoint 请求位（外部触发）
 *
 * 真正起 CP 由调度器决定。
 *
 * @param journal Journal
 */
void jrn_checkpoint_request(l2p_jrn_s *journal)
{
    journal->handle.state.checkpoint_needed = true;
}

/**
 * @brief 设置同 LBA 就地合并的下限序号
 *
 * 更早的条目不再参与合并，避免 CP 后的覆盖写复用旧 sequence。
 *
 * @param journal  Journal
 * @param sequence 合并在该 sequence 之后才允许
 */
void jrn_merge_floor_set(l2p_jrn_s *journal, DWORD sequence)
{
    journal->handle.state.merge_floor = sequence;
}

/**
 * @brief 全环占用百分比（全局写位置口径，封顶 100）
 *
 * 供 85% / 95% 阈值判断。
 *
 * @param journal Journal
 * @return 0..100
 */
BYTE jrn_watermark_get(const l2p_jrn_s *journal)
{
    // 全环全局口径（按已写页 × 每页条目数）：分子是「全局写位置」，不是单块内
    // current_page——否则 85%/95% 会提前或永不触发。×100 不会溢出 DWORD。
    // ⚠ 顺序流映射页每页可装 508 条（> 127），该口径会**高估**占用（每写满一张
    //   顺序页仍只按 127 条计）⇒ 只会提前触发 Checkpoint，方向是安全的；
    //   当前页的 508 条加上已写页基数可能超过分母，故结果封顶 100。
    const DWORD entries_per_page = JRN_ENTRIES_PER_PAGE;
    const DWORD total_pages      = JOURNAL_BLOCK_COUNT * PAGES_PER_BLOCK;
    const DWORD used_units       = ((journal->handle.state.current_block * PAGES_PER_BLOCK + journal->handle.state.current_page) * entries_per_page) + journal->handle.state.current_entry;
    const DWORD percent          = ((used_units * 100U) / (total_pages * entries_per_page));

    return (percent > 100U) ? (BYTE)100U : (BYTE)percent;
}

/**
 * @brief 按当前占用更新紧急 / 需 Checkpoint 标志
 *
 * 只置位；截断后由 commit 复位。
 *
 * @param journal Journal
 */
void jrn_watermark_update(l2p_jrn_s *journal)
{
    const BYTE used = jrn_watermark_get(journal);

    if (used >= JRN_CRITICAL)
    {
        journal->handle.state.emergency = true; /* ≥95% 紧急 */
    }
    else if (used >= JRN_FULL_THRESHOLD)
    {
        journal->handle.state.checkpoint_needed = true; /* ≥85% 需 Checkpoint */
    }
}

/**
 * @brief Journal 命令完成服务
 *
 * 只按 job.phase 分派，不推进状态机（阶段里有同步原语，泵内会嵌套）。
 *
 * @param ftl        完成派发解析出的实例
 * @param completion FC 完成状态
 * @param ctx        l2p_jrn_s *
 */
void ftl_journal_callback(ftl_ctx_s *ftl, const fc_cmd_status_s *completion, void *ctx)
{
    l2p_jrn_s *journal = (l2p_jrn_s *)ctx;

    (void)ftl; /* 实例无关：ctx 就是本实例的 journal */

    fc_cmd_s *cmd = fc_command_id_to_command(completion->cmd_id);

    // 一条命令对应一个 job：phase 只能是 WRITE / ERASE（残留完成 = 状态机破坏）。
    ASSERT((journal->handle.job.phase == JRN_WRITE) || (journal->handle.job.phase == JRN_ERASE));

    if (journal->handle.job.phase == JRN_WRITE)
    {
        jrn_write_complete(journal, cmd, completion);
    }
    else if (journal->handle.job.phase == JRN_ERASE)
    {
        jrn_erase_complete(journal, cmd, completion);
    }
    else
    {
        // 上面的 ASSERT 在 release 下裁空 ⇒ 这里必须留运行期兜底：否则 job 永远
        // 卡在非 IDLE，后续 flush 再也起不来（命令还不回收就是池泄漏）。
        LOGE(LOG_TAG_JRN "unexpected completion phase=%d\r\n", journal->handle.job.phase);
        journal->handle.job.phase = JRN_IDLE;
        (void)fc_command_release(cmd->cmd_id);
    }
}

/**
 * @brief 扫环找最新的有效页
 *
 * @param journal        Journal
 * @param out            输出：最新页引用
 * @param out_read_error 输出：是否遇到读错误
 * @return true 找到；false 环内无有效页
 */
SSD_CODE_FAST BOOL jrn_page_scan_latest(const l2p_jrn_s *journal, jrn_page_ref_s *out, BOOL *out_read_error)
{
    const WORD read_errors = jrn_ring_scan(journal, 0U, NULL, 0U, NULL, out);

    if (out_read_error != NULL)
    {
        *out_read_error = (read_errors != 0U);
    }
    LOGW_IF(read_errors != 0U, LOG_TAG_JRN "ring scan: %hu page(s) unreadable — 判型结果不可信\r\n", read_errors);

    // 有效页必有 ≥1 条条目（entry_count == 0 才是「环空」）；不能用 page_seq > 0
    // 当哨兵——序号回绕后 0 是合法值。
    return (out != NULL) && (out->entry_count > 0U);
}

/**
 * @brief 从最新页恢复写点
 *
 * 写点 = 最新页下一页；跨块先擦但不记 pre_erased（马上会被写满）。
 *
 * @param journal        Journal
 * @param latest         最新页引用
 * @param floor_sequence 回放 / 合并下限
 */
SSD_CODE_FAST void jrn_resume(l2p_jrn_s *journal, const jrn_page_ref_s *latest, DWORD floor_sequence)
{
    DWORD next          = floor_sequence;
    BOOL  pending_erase = false;
    BYTE  erase_block   = 0U;

    if ((latest != NULL) && (latest->entry_count > 0U))
    {
        // ⚠ 必须加 **entry_count**，不是 +1：本页占了 page_seq .. page_seq+entry_count-1
        // 共 entry_count 个序号（jrn_append 逐条 `sequence_counter++`，页头
        // `hdr_page_seq = sequence_counter − current_entry` 反推）。若只 +1，序号会
        // 退回本页内部 ⇒ 下一页的 page_seq 落进本页的序号区间，回放过滤
        // `page_seq + entry_count > cp_sequence` 会把已进 L2P 的页**重复回放**。
        if (jrn_seq_after((DWORD)(latest->page_seq + latest->entry_count), next))
        {
            next = latest->page_seq + latest->entry_count;
        }

        BYTE next_block = latest->block;
        BYTE next_page  = (BYTE)(latest->page + 1U);

        // 最新页之后的一页可能正是掉电时在写的白页（半编程、非空白）。
        // 对已编程页再编程 = AND 损坏，主流做法是「绝不二次编程」⇒ 跳过它；
        // 块尾由下面的换块路径处理（新块先擦再写）。
        if ((next_page < PAGES_PER_BLOCK) && (jrn_page_raw_blank(jrn_page_addr(journal, latest->block, next_page)) == false))
        {
            LOGW(LOG_TAG_JRN "resume: skip torn page blk=%hhu page=%hhu\r\n", latest->block, next_page);
            next_page = (BYTE)(next_page + 1U);
        }

        if (next_page >= PAGES_PER_BLOCK)
        {
            next_block = jrn_block_next(journal, latest->block);
            next_page  = 0U;

            if (next_block == JRN_BLOCK_NONE)
            {
                journal->handle.state.ring_full = true;
                next_block                      = latest->block; /* 停在最后好块；ring_full 挡住后续写 */
                LOGE(LOG_TAG_JRN "all ring blocks unusable at resume\r\n");
            }
            else
            {
                // 擦失败绝不能继续：首次写会落进未擦块（二次编程 = 静默损坏）。
                // 重新挂起 need_erase，由 jrn_handle / flush_now 先补擦；
                // 持续失败由 jrn_erase_failed 标坏并跳到下一块。
                journal->handle.job.target_block = next_block;

                const DWORD erase_addr = jrn_block_addr(journal, next_block);

                if (ftl_sync_erase_block_multi_plane(erase_addr) != FTL_OK)
                {
                    LOGE(LOG_TAG_JRN "resume ring erase failed blk=%hhu — 挂起补擦\r\n", next_block);
                    jrn_erase_failed(journal, next_block);

                    if (journal->handle.state.ring_full)
                    {
                        next_block = latest->block; /* 停在最后好块 */
                    }
                    else
                    {
                        next_block    = journal->handle.job.target_block; /* 可能已跳到下一可用块 */
                        pending_erase = true;
                        erase_block   = next_block;
                    }
                }
            }
        }

        journal->handle.state.current_block = next_block;
        journal->handle.state.current_page  = next_page;
    }
    else
    {
        // 环空：从第一个可用块开始（block0 可能已在 BBT 里标坏）。
        const BYTE first = jrn_block_first_usable(journal);

        if (first == JRN_BLOCK_NONE)
        {
            journal->handle.state.ring_full     = true;
            journal->handle.state.current_block = 0U;
        }
        else
        {
            journal->handle.state.current_block = first;
        }
        journal->handle.state.current_page = 0U;
    }

    if (next == 0U)
    {
        next = 1U;
    }

    journal->handle.state.current_entry     = 0U;
    journal->handle.state.sequence_counter  = next;
    journal->handle.state.persisted_seq     = next; /* 扫描到的页都在介质上：水位直接对齐 */
    journal->handle.state.write_page_seq    = next;
    journal->handle.state.write_entry_count = 0U;
    journal->handle.state.merge_floor       = 0U;
    journal->handle.state.page_kind         = JRN_PAGE_LBA;
    journal->handle.state.page_base         = 0U;

    // 开机没有“未完成迁移”的上下文：上次上电的失败 pending 不跨重启。
    journal->handle.state.write_fail_count   = 0U;
    journal->handle.state.write_fail_seq     = 0U;
    journal->handle.state.write_fail_pending = false;

    // 异步刷页 job 复位：resume 是开机路径，此前不可能有在飞 job；
    // 但复位是廉价的，且能挡住「同一实例被重复 resume」的残留。
    // 若上面换块擦除失败，在此（复位之后）重新挂起，保证首次写前先补擦。
    journal->handle.job.phase        = JRN_IDLE;
    journal->handle.job.target_block = erase_block;
    journal->handle.job.need_erase   = pending_erase;

    memset(journal->handle.job.page_buf, 0xFF, sizeof(journal->handle.job.page_buf));
}

/**
 * @brief 回放 CP 之后的所有 Journal 条目
 *
 * 两遍：扫描收集需回放页（256 项，计数用 WORD）→ 插入排序 → 按 LBA
 * last-write-wins；out_entries 只计成功写入 L2P 的条目。
 *
 * @param journal     Journal
 * @param cache       L2P cache
 * @param cp_sequence CP 水位
 * @param out_entries 输出：成功回放条数
 * @param block_ctx   回放上下文（块账回调 / 失败归因）
 * @return FTL_OK；FTL_ERROR 关键页读失败
 */
SSD_CODE_FAST ftl_status_e jrn_replay(l2p_jrn_s *journal, l2p_cache_s *cache, DWORD cp_sequence, DWORD *out_entries, jrn_replay_ctx_s *block_ctx)
{
    // JOURNAL_BLOCK_COUNT 块 × 每块 PAGES_PER_BLOCK 页 = 256 页；每项 16B → 4 KiB。
    // 放文件级 static（开机单线程、无重入），不占 2 KiB 栈预算。
    jrn_page_ref_s *const replay_pages = s_replay_pages;

    // 本轮回放的异常汇总（见文件内 s_replay_* 的注释）。
    s_replay_degraded_entries   = 0U;
    s_replay_degraded_region    = UINT32_MAX;
    s_replay_write_fail_entries = 0U;
    s_replay_write_fail_lba     = FTL_LBA_INVALID;
    s_replay_page_read_fail     = 0U;

    // 第一遍：扫环——收集需回放页 + 取最新页
    jrn_page_ref_s latest;
    WORD           replay_count = 0U;
    const WORD     read_errors  = jrn_ring_scan(journal, cp_sequence, replay_pages, (JOURNAL_BLOCK_COUNT * PAGES_PER_BLOCK), &replay_count, &latest);

    // 第二遍：按 (page_seq, entry_index) 递增序应用条目。
    // 插入排序（replay_count ≤ 256）：per-LBA last-write-wins——
    // 同 LBA 的旧条目（含 GC/WL 搬移记录）被后写覆盖，不指向已擦除块。
    DWORD applied = 0U;
    {
        for (WORD i = 1U; i < replay_count; i++)
        {
            jrn_page_ref_s key = replay_pages[i];

            WORD n = i;
            while ((n > 0U) && jrn_seq_after(replay_pages[n - 1U].page_seq, key.page_seq))
            {
                replay_pages[n] = replay_pages[n - 1U];
                n--;
            }
            replay_pages[n] = key;
        }

        for (WORD i = 0U; i < replay_count; i++)
        {
            applied += jrn_page_replay_apply(journal, cache, &replay_pages[i], cp_sequence, block_ctx);
        }
    }

    // 恢复写指针与序号（落到最新页之后；floor 兜住「环空 / 序号回退」）
    jrn_resume(journal, &latest, cp_sequence);

    // 异常汇总：每类一行，替代逐条 WRN（见 s_replay_* 注释）。
    if (s_replay_degraded_entries != 0U)
    {
        LOGW(LOG_TAG_JRN "replay: L2P region %lu degraded — %lu entr(ies) applied to unmapped baseline "
                         "(scoped recovery; pre-CP mappings lost)\r\n",
             s_replay_degraded_region,
             s_replay_degraded_entries);
    }
    LOGW_IF(s_replay_write_fail_entries != 0U, LOG_TAG_JRN "replay: L2P write failed for %lu entries (first lba=%lu) — force full metadata rebuild\r\n", s_replay_write_fail_entries, s_replay_write_fail_lba);

    if (out_entries != NULL)
    {
        *out_entries = applied;
    }

    // 读失败的页 = 可能漏回放的条目。**写态恢复已经完成**（jrn_resume 必须执行，
    // 否则写指针会回到 block0/page0 二次编程），但调用者不能把本次回放当成完整结果
    // ——应回退 full metadata rebuild。
    // ⚠ 扫描遍（read_errors）与应用遍（s_replay_page_read_fail）**都要判**：
    //   某页扫描时能读、应用时读失败同样意味着条目被漏。
    if ((read_errors != 0U) || (s_replay_page_read_fail != 0U))
    {
        LOGW(LOG_TAG_JRN "replay incomplete: %hu scan + %lu apply page(s) unreadable\r\n", read_errors, s_replay_page_read_fail);
        return FTL_ERROR;
    }

    return FTL_OK;
}

/**
 * @brief 最近一次回放的「降级 region 条目」数（只读）
 *
 * 供 PFR 判断是否走了范围化恢复。
 *
 * @return 条目数
 */
DWORD jrn_replay_degraded_entries(void)
{
    return s_replay_degraded_entries;
}

/* ══════════════════════════════════════════════════════════
 * 8. 截断
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 计算 Journal 环形预擦计划
 *
 * 水位不足以覆盖全部已写条目 ⇒ 不截断（最旧块可能仍持有回放条目）。
 *
 * @param journal        Journal
 * @param up_to_sequence 截断水位
 * @param out_addr       输出：待擦块基地址
 * @param out_block      输出：待擦环形块号
 * @return NEEDS_ERASE / ALREADY / NO_TRUNCATE
 */
jrn_truncate_step_e jrn_truncate_plan(l2p_jrn_s *journal, DWORD up_to_sequence, DWORD *out_addr, BYTE *out_block)
{
    // ① 水位校验。
    // 调用者传的是 CP **起点**的 sequence_counter（下一个待分配序号）。若水位**不足以**
    // 覆盖全部已写条目（CP 期间 journal 又前进了 ≥2 条），说明有未 checkpoint 的条目，
    // 而环形里最旧的一块可能仍持有回放需要的条目 ⇒ **不截断**。
    // （sequence_counter == 0 理论上不会出现；一并挡掉，避免 -1 下溢误判。）
    // ⚠ CP 的快照点在**起点**（见 checkpoint.c 的掉电安全论证），故水位判据容易不满足
    // ——即更少预擦。这是安全方向的取舍：预擦只是磨损优化，跳过它只是让
    // jrn_handle 在换块时自己异步擦（每块每圈仍只擦一次）。
    if ((journal->handle.state.sequence_counter == 0U) || (!jrn_seq_ge(up_to_sequence, (DWORD)(journal->handle.state.sequence_counter - 1U))))
    {
        return JRN_TRUNC_NO_TRUNCATE;
    }

    // ② 待预擦块 = 环形下一块。该块整块的条目都 ≤ up_to_sequence（checkpoint 已把它们
    // 写进 NAND L2P 表），擦掉即完成截断。重复调用（两次 checkpoint 之间无写入）
    // 时 next == pre_erased_block ⇒ 跳过重擦。
    const BYTE next = jrn_block_next(journal, journal->handle.state.current_block);

    if (next == JRN_BLOCK_NONE)
    {
        return JRN_TRUNC_NO_TRUNCATE; /* 环全部不可用：没有可预擦的块 */
    }
    if (next == journal->handle.state.pre_erased_block)
    {
        return JRN_TRUNC_ALREADY;
    }

    *out_addr  = jrn_block_addr(journal, next);
    *out_block = next;
    return JRN_TRUNC_NEEDS_ERASE;
}

/**
 * @brief 提交截断：记下本圈已擦块、无失败时复位水位标志
 *
 * @param journal      Journal
 * @param erased_block 已擦块号（JRN_BLOCK_NONE = 未擦）
 */
void jrn_truncate_commit(l2p_jrn_s *journal, BYTE erased_block)
{
    // 记账：异步 job 回切到该块时跳过擦除——保证同一块每圈只擦一次。
    // ⚠ 本次未擦（NONE）时**不动** pre_erased_block：覆写成 NONE 会让已预擦的块被
    // 下一次换块再擦一次（双倍磨损回归）。
    if (erased_block != JRN_BLOCK_NONE)
    {
        journal->handle.state.pre_erased_block = erased_block;
    }

    // 复位水位标志（设计 §7.5.4）：截断即水位已清。与 checkpoint job 的收尾双保险
    // （后者覆盖 truncate 被弱桩替换的纯 RAM 测试构建）。
    // ⚠ 持久性失败 pending 未清时**保留**标志：说明本次 CP 早于失败点，没有把
    // 迁移后的 L2P 全部落盘，必须再触发一次 CP，而不是把水位门关掉。
    if (journal->handle.state.write_fail_pending == false)
    {
        journal->handle.state.emergency         = false;
        journal->handle.state.checkpoint_needed = false;
    }
}

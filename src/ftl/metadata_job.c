/*
 * metadata_job.c — 元数据目录整理 job：日志块回收 + 抗碎片压缩
 *
 * 与 metadata.c 的分工：
 *   metadata.c     —— Directory 结构、目录检查点落盘/加载、region 追加/登记（同步原语）；
 *   metadata_job.c —— 两件"要碰 NAND 的整理活"的异步状态机：
 *                     ① 回收：待回收位图（reclaim_pending）里的块 ⇒ 擦 + 归还固定 L2P 日志区；
 *                     ② 压缩：挑活 region 最少的 victim，搬进当前块，落目录检查点后擦除。
 *   共享的内部原语（位图 / md_slot_take / md_handle_reset）也定义在本文件，声明在
 *   include/ftl/metadata_job.h。
 *
 * 设计约束（与 metadata.h 的 md_handle_s 长注释配套）：
 *   · **一次只提交一条命令**：提交阶段设 waiting 就返回；纯 RAM 阶段连续推进到下一个
 *     提交阶段或终态。推进一律由 md_handle 驱动（服务函数不推进，见下）。
 *   · **服务函数不推进状态机**（同 ftl_checkpoint_callback）：CP_DIR_CKPT_PREP 要调
 *     md_dir_ckpt_prepare，而它在游标未建立时会**同步**扫池定位
 *     （md_ring_locate → ftl_sync_read_page）——在完成泵里跑就是嵌套泵
 *     （l2p_sync_pump 首行有 ASSERT 挡）。故 ftl_metadata_callback 只记结果 + release + 改
 *     phase，下一轮 kick 接着走。
 *   · **提交失败（背压）保留现场**：压缩搬移的槽位取到后即消耗（写指针已推进），
 *     故 `job.slot` 在提交失败时必须留着，下轮重试**同一个**槽——绝不能重取
 *     （重取会白烧一页）；待回收位在擦除在飞/失败期间也保持置位。
 *
 * 文件结构：
 *   〇、文件级 helper（块身份复核 / 地址换算）
 *   一、待回收位图 / 写指针 / job 复位
 *   二、job 提交原语（abort / stall / submit）
 *   三、压缩（victim 选择 / 搬移 / 目录检查点 / 回收）
 *   四、job 状态机
 *   五、模块入口与状态
 *   六、完成服务
 */

#include "metadata_job.h"

#include "assertion.h"
#include "bbt.h" /* bbt_mark_bad（持续擦除失败标坏） */
#include "fc.h"
#include "ftl.h" /* ftl_ctx_s（完成回调签名） */
#include "ftl_fc_interface.h"
#include "ftl_op.h" /* ftl_op_submit / FTL_OP_METADATA */
#include "log.h"
#include "onfi_1_0.h"
#include "timer.h"

#include <string.h>

// 提交 deadline：连续提交不出去超过 FTL_SUBMIT_TIMEOUT_MS 即中止 job（同 CP/MIG/RR）。

// 同一块连续擦除失败多少次后标坏。
// 3 次覆盖瞬态擦除失败（忙/超时）；持续失败只能来自物理坏块，继续重试没有意义：
// 保留回收位会让该块永久占着轮转队列、每次后台都白付一次擦除命令。
#define MD_RECLAIM_FAIL_MAX (3U)

// 压缩空槽预算为「扫描期间可能落回 victim 的前台 region 写」预留的槽位数。
// 一条前台 flush 现在**双副本占两个槽**（L2）⇒ 预留 2；见 md_compact_begin。
#define MD_COMPACT_RESERVED_SLOTS (L2P_REGION_COPIES)

/* ══════════════════════════════════════════════════════════════════
 * 〇、文件级 helper（块身份复核 / 地址换算）
 * ══════════════════════════════════════════════════════════════════ */

/**
 * @brief `md_log_block_is_idle` + BST 确认它仍是本池的 LOG 块。
 * 用于回收/压缩的**擦除前**复核，防止把一个恰好活计数为 0 的用户块擦掉。
 *
 * @param dir   Directory（活 region 计数与当前写块）
 * @param alloc 分配器（BST 块状态）
 * @param g     候选 gblock 编号
 * @return true = 可安全擦除
 *         false = 仍被引用 / 是当前写块 / 不是 LOG 块
 */
static BOOL md_log_block_is_reclaimable(const md_dir_cache_s *dir, const alc_s *alloc, WORD g)
{
    if (md_log_block_is_idle(dir, g) == false)
    {
        return false;
    }

    const bst_block_s *b = &alloc->blocks[g];

    return (ftl_block_state(b) == FTL_BLOCK_STATE_LOG);
}

/**
 * @brief gblock + 页号 → PBA（ch/ce/lun 取本 Directory 拓扑）。
 * ⚠ 单一转换漏斗（C9）：block/plane 的字段序只在这里出现一次，调用点不手写 pba_make。
 *
 * @param dir    Directory（提供 ch/ce/lun 拓扑）
 * @param gblock gblock 编号
 * @param page   块内页号
 * @return 该页的 PBA
 */
static DWORD md_block_page_to_pba(const md_dir_cache_s *dir, WORD gblock, BYTE page)
{
    const DWORD block = ftl_gblock_block(gblock);
    const DWORD plane = ftl_gblock_plane(gblock);

    return pba_make(dir->topo.ch, dir->topo.ce, dir->topo.lun, block, plane, page, 0U);
}

/* ══════════════════════════════════════════════════════════════════
 * 一、待回收位图 / 写指针 / job 复位
 * ══════════════════════════════════════════════════════════════════ */

/* ── 待回收位图 / 写指针 / job 状态（H42：改状态的闸门定义在 .c）── */

/**
 * @brief 登记一个待回收块（幂等）
 *
 * 已置位返回 false；越界块号不入图。
 *
 * @param dir Directory
 * @param g   全局块号
 * @return true 本次新登记；false 已登记 / 越界
 */
BOOL md_reclaim_mark(md_dir_cache_s *dir, WORD g)
{
    if (g >= FTL_BLOCK_TOTAL)
    {
        return false;
    }

    DWORD *word = &dir->reclaim.bitmap[g >> 5U];
    DWORD  bit  = 1UL << (g & 31U);

    if ((*word & bit) != 0U)
    {
        return false;
    }

    *word |= bit;
    dir->reclaim.count++;
    return true;
}

/**
 * @brief 从轮转游标起找第一个置位的块号；**保持置位**（擦除在飞期间不丢，完成成功才清）。
 * 游标推进到 g+1：擦除持续失败的块不会永久挡住后续块。
 *
 * @param dir Directory
 * @param out 出参：命中的 gblock（仅返回 true 时有效）
 * @return true = 找到置位块
 *         false = 位图全空
 */
static BOOL md_reclaim_take(md_dir_cache_s *dir, WORD *out)
{
    // 从上次取走的位置继续扫描并环绕：擦除持续失败的块保留位，但不再永久占住
    // 扫描起点，后续块仍能被回收。
    for (WORD i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        const WORD  g    = ((dir->reclaim.scan + i) % FTL_BLOCK_TOTAL);
        const DWORD word = dir->reclaim.bitmap[g >> 5U];
        const DWORD bit  = 1UL << (g & 31U);

        if ((word & bit) != 0U)
        {
            *out              = g;
            dir->reclaim.scan = ((g + 1U) % FTL_BLOCK_TOTAL);
            return true;
        }
    }

    return false;
}

/**
 * @brief 清位（置位过才减计数）。
 *
 * @param dir Directory
 * @param g   gblock 编号；越界直接返回
 */
static void md_reclaim_clear(md_dir_cache_s *dir, WORD g)
{
    if (g >= FTL_BLOCK_TOTAL)
    {
        return;
    }

    DWORD *word = &dir->reclaim.bitmap[g >> 5U];
    DWORD  bit  = 1UL << (g & 31U);

    if ((*word & bit) != 0U)
    {
        *word &= ~bit;
        if (dir->reclaim.count > 0U)
        {
            dir->reclaim.count--;
        }
    }
}

/**
 * @brief 清空待回收位图、计数与扫描游标
 *
 * 格式化 / 目录重建后调用。
 *
 * @param dir Directory
 */
void md_reclaim_reset(md_dir_cache_s *dir)
{
    memset(dir->reclaim.bitmap, 0, sizeof(dir->reclaim.bitmap));
    dir->reclaim.count = 0U;
    dir->reclaim.scan  = 0U;
}

/**
 * @brief 全盘重扫：把所有可回收日志块登记进位图
 *
 * 开机 / 目录重建后调用。
 *
 * @param dir   Directory
 * @param alloc 分配器
 */
void md_reclaim_mark_all(md_dir_cache_s *dir, const alc_s *alloc)
{
    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        if (md_log_block_is_reclaimable(dir, alloc, g) == false)
        {
            continue;
        }
        (void)md_reclaim_mark(dir, g);
    }
}

/**
 * @brief 从当前写块取一个空槽
 *
 * plane 无当前块或块已满返回 INVALID；取到即推进写指针。
 *
 * @param dir   Directory
 * @param plane 目标 plane
 * @return 槽 PBA；NAND_PBA_INVALID = 无可用槽
 */
DWORD md_slot_take(md_dir_cache_s *dir, BYTE plane)
{
    if ((plane >= PLANE_NUM) || (dir->cur.valid[plane] == false) || (dir->cur.page[plane] >= PAGES_PER_BLOCK))
    {
        return NAND_PBA_INVALID;
    }

    const BYTE page = dir->cur.page[plane];

    dir->cur.page[plane]++;

    return md_block_page_to_pba(dir, dir->cur.block[plane], page);
}

/**
 * @brief 复位整理 job 状态
 *
 * 保留单调进度与最近错误，其余清零并重写「无」哨兵。
 *
 * @param dir Directory
 */
void md_handle_reset(md_dir_cache_s *dir)
{
    // reclaimed_count 是**单调进度计数**（跨 job 保留）；其余 job 状态清零。
    // error 也保留到下一次起 job 时再清（status 要能看到最近一次失败）。
    // 哨兵必须显式写（memset 只给 0）：victim / reclaim_gblock / reclaim_fail_gblock
    // 的"无"是 UINT16_MAX，slot 的"未取"是 NAND_PBA_INVALID。
    const DWORD   reclaimed  = dir->handle.reclaimed_count;
    const int32_t last_error = dir->handle.error;

    // 放弃本 job 可能持有的目录检查点单飞锁（prepare 之后没走到 commit）。
    md_dir_ckpt_write_abort(dir, &dir->handle.dir_ckpt);

    memset(&dir->handle, 0, sizeof(dir->handle));
    dir->handle.victim              = UINT16_MAX;
    dir->handle.reclaim_gblock      = UINT16_MAX;
    dir->handle.reclaim_fail_gblock = UINT16_MAX;
    dir->handle.slot                = NAND_PBA_INVALID;
    dir->handle.cur_source          = NAND_PBA_INVALID;
    dir->handle.reclaimed_count     = reclaimed;
    dir->handle.error               = last_error;
}

/* ══════════════════════════════════════════════════════════════════
 * 二、job 提交原语（abort / stall / submit）
 * ══════════════════════════════════════════════════════════════════ */

// 中止日志限流：真异常（读页失败 / 目录检查点写坏 / 账本分叉）会每轮 GC 重复，
// 逐次 WRN 会刷屏；5 s 一行 + 被抑制条数（时钟缺失时 64 次一行）。
static DWORD s_abort_log_ms;
static DWORD s_stall_log_ms; /* 提交 deadline 触发的 abort 日志限流时间戳 */
static DWORD s_abort_log_n;

/**
 * @brief 判断本次中止日志是否该输出：5 s 一行；时钟缺失时每 64 次一行。
 *
 * @return true = 本次输出
 *         false = 限流中，本次只累加抑制计数
 */
static BOOL md_abort_log_is_due(void)
{
    // 时间优先，条数只作无时钟时的兜底（否则高频 abort 一秒撞满 64 条）。
    return log_rate_due(&s_abort_log_ms, &s_abort_log_n, LOG_RATE_SLOW_INTERVAL_MS);
}

/**
 * @brief 中止本次整理：清请求与游标，回 IDLE。
 * ⚠ **victim 不擦**（已登记的 region 已指向新页、未搬的仍指向 victim ⇒ 两边都有效），
 * 下次换块时重来即可——失败不回滚。
 * ⚠ 若 victim 已被搬空（live == 0），登记进待回收位图保持覆盖范围。
 *
 * @param dir Directory
 */
static void md_handle_abort(md_dir_cache_s *dir)
{
    md_handle_s *j = &dir->handle;

    if (j->timed_out)
    {
        // 提交超时 = 资源 starvation 的瞬态中止：victim 不擦、下轮重来，无数据风险。
        // 每轮打 WRN 会被当成故障并刷屏 ⇒ 只在 debug 档可见。
        LOGD(LOG_TAG_META "md job abort (transient timeout) phase=%d victim=%hu moved=%hu\r\n", j->timeout_phase, j->victim, j->moved);
    }
    else
    {
        LOGE_IF(md_abort_log_is_due(), LOG_TAG_META "md job abort phase=%d victim=%hu moved=%hu (victim 不擦)\r\n", j->phase, j->victim, j->moved);
    }

    if ((j->victim != UINT16_MAX) && (dir->l2p_live[j->victim] == 0U))
    {
        (void)md_reclaim_mark(dir, j->victim);
    }

    md_handle_reset(dir);
    dir->handle.error = (int32_t)FTL_ERROR; /* status.error：最近一次中止的归因 */
}

/**
 * @brief 提交不出去超过 FTL_SUBMIT_TIMEOUT_MS ⇒ 中止 job（同 CP/MIG/RR 同族）。
 * 时钟缺失时 deadline 不生效。
 *
 * @param dir Directory
 * @return true = 已中止
 *         false = 未超时 / 首次记录起点 / 时钟缺失
 */
static BOOL md_handle_stall(md_dir_cache_s *dir)
{
    md_handle_s *j      = &dir->handle;
    const DWORD  now_ms = platform_now_ms();

    if (now_ms == 0U)
    {
        return false;
    }

    // cache 序列打开时，后台提交被门控拒绝是**预期**等待：序列在下一窗口/下一 chunk
    // 收尾（空闲守卫 1 s 兜底），不应当作提交卡死而中止 job（也会刷屏）。
    if (ftl_op_bg_submit_deferred() == true)
    {
        timer_deadline_clear(&j->submit_deadline);
        return false;
    }

    if (j->submit_deadline.armed == false)
    {
        timer_deadline_arm(&j->submit_deadline, now_ms, FTL_SUBMIT_TIMEOUT_MS);
        return false;
    }
    if (timer_deadline_expired(&j->submit_deadline, now_ms))
    {
        LOGW_IF(log_rate_due(&s_stall_log_ms, NULL, LOG_RATE_SLOW_INTERVAL_MS), LOG_TAG_META "md job submit stalled %lums — abort\r\n", (now_ms - j->submit_deadline.at) + FTL_SUBMIT_TIMEOUT_MS);
        j->timed_out     = true;
        j->timeout_phase = j->phase;
        md_handle_abort(dir);
        return true;
    }
    return false;
}

/**
 * @brief 提交一条已建好链的异步命令（所有 md_submit_* 的唯一漏斗）。
 * 先把 waiting 置位、提交失败再回滚；stall_ms 只在提交成功后清零。
 *
 * @param dir Directory（job 上下文，同时作为完成服务的 ctx）
 * @param cmd 待提交命令；成功时所有权交给完成服务，失败时本函数 release
 * @return FTL_OK    已提交
 *         FTL_AGAIN cmd 为空或提交背压（已回滚 waiting）
 */
static ftl_status_e md_command_submit(md_dir_cache_s *dir, fc_cmd_s *cmd)
{
    if (cmd == NULL)
    {
        return FTL_AGAIN;
    }

    md_handle_s *j = &dir->handle;

    j->waiting = true;

    // ctx = Directory（模块上下文），完成服务据此取 handle 与端口。
    if (ftl_op_submit(cmd, FTL_OP_METADATA, dir) != FTL_OK)
    {
        j->waiting = false;
        (void)fc_command_release(cmd->cmd_id);
        return FTL_AGAIN;
    }

    timer_deadline_clear(&j->submit_deadline);
    return FTL_OK;
}

/* 元数据 job 的提交操作类型；五类命令共用同一个提交漏斗。 */
typedef enum METADATA_SUBMIT_OPERATION_E
{
    MD_SUBMIT_WRITE = 0,    /* 单 plane 页写（L2P region 搬移） */
    MD_SUBMIT_WRITE_MIRROR, /* multi-plane 镜像页写（目录检查点 / CP / BBT） */
    MD_SUBMIT_ERASE,        /* 单块擦除（日志回收 / victim 擦除） */
    MD_SUBMIT_ERASE_MULTI,  /* multi-plane 镜像块擦除（固定元数据池换块） */
    MD_SUBMIT_READ,         /* 页读（搬移 victim） */
} md_submit_op_e;

/**
 * @brief 元数据 job 的唯一命令提交漏斗
 *
 * @param dir    Directory
 * @param op     操作类型
 * @param addr   目标/源 PBA
 * @param data   页写数据（仅 WRITE / WRITE_MIRROR）
 * @param length 页写长度（仅 WRITE / WRITE_MIRROR）
 * @return FTL_OK 已提交；FTL_AGAIN 命令池空/地址非法/背压；FTL_ERROR 非法 op
 */
static ftl_status_e md_submit(md_dir_cache_s *dir, md_submit_op_e op, DWORD addr, const void *data, DWORD length)
{
    fc_cmd_s    *cmd = NULL;
    ftl_status_e st;

    switch (op)
    {
        case MD_SUBMIT_WRITE:
            st = ftl_async_write_page(&cmd, addr, data, length, FTL_PAGE_CFG_META());
            break;
        case MD_SUBMIT_WRITE_MIRROR:
            st = ftl_async_write_page_mirror(&cmd, addr, data, length, FTL_PAGE_CFG_META());
            break;
        case MD_SUBMIT_ERASE:
            st = ftl_async_erase_block(&cmd, addr);
            break;
        case MD_SUBMIT_ERASE_MULTI:
            st = ftl_async_erase_block_multi_plane(&cmd, addr);
            break;
        case MD_SUBMIT_READ:
            st = ftl_async_read_page(&cmd, addr, FTL_PAGE_CFG_META());
            break;
        default:
            ASSERT_DEFAULT(md_submit_op_e);
            return FTL_ERROR;
    }

    if (st != FTL_OK)
    {
        return FTL_AGAIN; /* 命令池空 / 地址非法 */
    }

    return md_command_submit(dir, cmd);
}

/* ══════════════════════════════════════════════════════════════════
 * 三、压缩（victim 选择 / 搬移 / 目录检查点 / 回收）
 * ══════════════════════════════════════════════════════════════════ */

/**
 * @brief 选压缩 victim —— 活 region 数**最少**（但 > 0）的非当前日志块。
 *
 * @param dir Directory
 * @param alloc 分配器（读 BST 判 LOG 身份）。
 *
 * @return victim 的 gblock；无候选返回 UINT16_MAX。
 *
 * @note 零活块**不在此列**——它们已登记在 reclaim_pending，由 RECLAIM 阶段
 * 直接擦除归还，无需搬移。平手取块号最小者：结果确定（跨复位可复现），
 * 且先分配的块通常收的是更早、更可能变冷的数据。
 */
static WORD md_compact_victim_find(const md_dir_cache_s *dir, const alc_s *alloc)
{
    WORD best      = UINT16_MAX;
    WORD best_live = UINT16_MAX;

    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        const WORD live = dir->l2p_live[g];

        if (live == 0U)
        {
            continue; /* 零活块：reclaim 的活，直接擦，不必搬 */
        }
        {
            const BYTE p = ftl_gblock_plane(g);

            if ((p < PLANE_NUM) && dir->cur.valid[p] && (g == dir->cur.block[p]))
            {
                continue; /* 该 plane 的当前写块正在被追加，不搬 */
            }
        }

        if (md_slot_reserved(dir, g))
        {
            continue; /* 有在飞快照：写完成后的 commit 会把新位置落回本块，先不选 */
        }

        const bst_block_s *b = &alloc->blocks[g];

        if (ftl_block_state(b) != FTL_BLOCK_STATE_LOG)
        {
            continue; /* 只认本池独占的日志块（否则会误搬恰好活计数 > 0 的用户块）*/
        }
        if (live < best_live)
        {
            best_live = live;
            best      = g;
        }
    }

    return best;
}

/**
 * @brief 选 victim、核空槽预算、把 job 推进到 CP_READ（纯 RAM，不碰介质）。
 *
 * @param dir   Directory
 * @param alloc 分配器（读 BST 判 LOG 身份）
 * @return MD_COMPACT_STARTED    已进入 CP_READ
 *         MD_COMPACT_NO_CURRENT 无当前写块：无处可搬
 *         MD_COMPACT_NO_VICTIM  无候选（零活块归 reclaim 管 / 只剩当前块）
 *         MD_COMPACT_NO_SLOTS   当前块空槽装不下活 region 最少的 victim
 *
 * @note 起不动时 phase 留在 IDLE，由调用者决定是否换块。
 *
 * @note **无论成败都先清 compact_wanted**——它由 append_slot 置位而 append_slot
 * 又对 `phase != IDLE` 让路，不清就永远起不来（死锁）。
 * 空槽预算在此**重算**（不在 append_slot 置位时算）：置位到本函数之间可能
 * 又发放过若干槽位（此时 phase 还是 IDLE），按当下 cur_page[victim 的 plane] 核才准。
 * ⚠ 已知良性竞态：job 启动前已发出、尚未 commit 的前台 region 写可能在扫描期间落回
 * victim ⇒ 实际需搬数超过本预算 ⇒ md_compact_write 取不到槽 ⇒ 中止。已登记 region
 * 指向新页、victim 不擦，无数据损失，只是本次压缩重来。
 */
static md_compact_start_e md_compact_begin(md_dir_cache_s *dir, const alc_s *alloc)
{
    md_handle_s *j = &dir->handle;

    j->compact_wanted = false;

    WORD victim = md_compact_victim_find(dir, alloc);

    if (victim == UINT16_MAX)
    {
        return MD_COMPACT_NO_VICTIM; /* 全池零活（reclaim 已管）或只剩当前块 */
    }

    // 记下 victim：失败路径（NO_CURRENT / NO_SLOTS）由调用者按它的 plane 换块。
    j->victim = victim;

    // 搬移目标 = **victim 所在 plane 的当前块**：副本搬完仍留在同一 plane，
    // 主/备各占一个 plane 的布局不被压缩破坏。
    const BYTE victim_plane = ftl_gblock_plane(victim);

    if ((victim_plane >= PLANE_NUM) || (dir->cur.valid[victim_plane] == false))
    {
        return MD_COMPACT_NO_CURRENT; /* 该 plane 无当前写块：无处可搬 */
    }

    // 空槽预算：victim 的全部活 region 必须一次装进当前块（l2p_live 的定义就是
    // "最新位置落在该块的 region 数"）。装不下 ⇒ 留到换块后再来。
    // ⚠ 预留 MD_COMPACT_RESERVED_SLOTS 个槽位给「job 启动前已发出、扫描期间才 commit
    // 的前台 region 写」：双副本下一次 flush 占两槽，故预留 = L2P_REGION_COPIES。
    // 不预留时，在飞写落回 victim 会让实际需搬数超过预算 ⇒ md_slot_take 取不到槽 ⇒
    // 本次压缩中止（良性但白做）。预留槽把该竞态从「中止」降为「容量少两页」。
    // 代价：victim 活 region 数等于剩余槽位时本轮到不了它，换块后再来。
    const WORD free_slots = (PAGES_PER_BLOCK - dir->cur.page[victim_plane]);
    const WORD need_slots = (dir->l2p_live[victim] + MD_COMPACT_RESERVED_SLOTS);

    if (need_slots > free_slots)
    {
        return MD_COMPACT_NO_SLOTS;
    }

    j->victim      = victim;
    j->region_id   = 0U;
    j->cur_region  = 0U;
    j->moved       = 0U;
    j->slot        = NAND_PBA_INVALID;
    j->dir_ckpt.ok = false;
    timer_deadline_clear(&j->submit_deadline);
    j->phase = MD_JOB_CP_READ;

    LOGD(LOG_TAG_META "compact start victim=%hu live=%hu cur_page=%hhu\r\n", victim, dir->l2p_live[victim], dir->cur.page[victim_plane]);

    return MD_COMPACT_STARTED;
}

/**
 * @brief 从 job.region_id 起找 victim 上**下一个**仍有引用的 region，发起异步读。
 *
 * @param dir Directory（job 与 Directory 游标）
 * @return MD_COMPACT_SCANNED      已扫完（未提交任何命令）
 *         MD_COMPACT_SUBMITTED    已提交（等完成）
 *         MD_COMPACT_BACKPRESSURE 背压未提交（下轮重试）
 *         MD_COMPACT_FAILED       硬失败（调用者中止）
 *
 * @note 游标先走后命中，提交失败再把游标退回来（重试同一 region）。
 * 搬移走的是**裸页拷贝**（region 页无头，就是 L2P_REGION_SIZE 字节的 entries
 * 数组，与 l2p_cache 的读写口径逐字节一致）——不经过 l2p_cache，故不触发
 * "写→evict→flush→又要块"的递归。
 */
static md_compact_step_e md_compact_read(md_dir_cache_s *dir)
{
    md_handle_s *j = &dir->handle;

    while (j->region_id < METADATA_DIR_ENTRIES)
    {
        const WORD rid     = j->region_id;
        DWORD      primary = md_entry_find(dir, METADATA_TYPE_L2P_REGION, rid);
        DWORD      backup  = md_backup_find(dir, rid);

        // Directory 是介质读回（H29）：非 INVALID 的 PBA 必须过 pba_is_valid 才能
        // 进 FC（越界路由字段会让 FC 的 suspended[][] 越界）。损坏副本按缺失处理，
        // 交给前台加载路径去降级（RISK-L2P-21）。
        if ((primary != NAND_PBA_INVALID) && (pba_is_valid(primary) == false))
        {
            LOGE(LOG_TAG_META "compact: region %hu primary PBA invalid (0x%08lx) — treated as missing\r\n", rid, primary);
            primary = NAND_PBA_INVALID;
        }
        if ((backup != NAND_PBA_INVALID) && (pba_is_valid(backup) == false))
        {
            LOGE(LOG_TAG_META "compact: region %hu backup PBA invalid (0x%08lx) — treated as missing\r\n", rid, backup);
            backup = NAND_PBA_INVALID;
        }

        DWORD     old  = NAND_PBA_INVALID;
        md_copy_e copy = MD_COPY_PRIMARY;

        // 副本感知：victim 上可能只有主、只有备、或两份都在。
        // 两份都在时先搬主，下一次重扫同一 rid 再搬备（游标不前进）。
        if ((primary != NAND_PBA_INVALID) && (ftl_pba_gblock(primary) == j->victim))
        {
            old  = primary;
            copy = MD_COPY_PRIMARY;
        }
        else if ((backup != NAND_PBA_INVALID) && (ftl_pba_gblock(backup) == j->victim))
        {
            old  = backup;
            copy = MD_COPY_BACKUP;
        }

        if (old == NAND_PBA_INVALID)
        {
            j->region_id = (rid + 1U); /* 两份都不在本 victim：推进 */
            continue;
        }

        j->cur_region   = rid;
        j->cur_copy     = copy;
        j->cur_source   = old;
        j->cur_fallback = false;

        if (md_submit(dir, MD_SUBMIT_READ, old, NULL, 0U) != FTL_OK)
        {
            // 游标不前进：下轮重试同一 region 的同一副本
            if (md_handle_stall(dir))
            {
                return MD_COMPACT_FAILED;
            }
            return MD_COMPACT_BACKPRESSURE;
        }
        return MD_COMPACT_SUBMITTED;
    }

    // 扫完仍找不到任何指向 victim 的 region，但账本声称 live>0 ⇒ 账本与 Directory
    // 分叉。不能返回 SCANNED（caller 会再次选中同一 victim 空转），也不能清零账本
    // （若是被在飞写回刚加上去的，清零会让 CP_RECLAIM 擦掉仍在用的块）。中止本次 job，
    // 下一轮重扫；本函数不擦任何东西。
    if (dir->l2p_live[j->victim] != 0U)
    {
        // Directory 是权威，但这里**不能**直接把账清零：清零后 CP_RECLAIM 会擦块，
        // 若计数是被在飞写回刚加上去的，就会擦掉仍在用的页。中止本次 job，下一轮重扫。
        LOGE(LOG_TAG_META "compact: victim %hu live=%hu but no directory entry — abort (no erase)\r\n", j->victim, dir->l2p_live[j->victim]);
        return MD_COMPACT_FAILED;
    }

    return MD_COMPACT_SCANNED; /* 扫描完毕 */
}

/**
 * @brief 把 job.cp_buf 写进当前块的新槽（槽只取一次，提交失败重试同一槽）。
 *
 * @param dir Directory（job 与写指针）
 * @return 同 md_compact_read。
 */
static md_compact_step_e md_compact_write(md_dir_cache_s *dir)
{
    md_handle_s *j = &dir->handle;

    if (j->slot == NAND_PBA_INVALID)
    {
        // 搬到 **victim 所在 plane** 的当前块（副本留在原 plane）。
        const BYTE plane = ftl_gblock_plane(j->victim);

        j->slot = md_slot_take(dir, plane);
        if (j->slot == NAND_PBA_INVALID)
        {
            // 该 plane 当前块无空槽 ⇒ 预算失准。最可能来源是 md_compact_begin 注释里的
            // "启动前已在飞的前台写落回 victim"：中止是安全的（victim 不擦）。
            LOGW(LOG_TAG_META "compact: 当前块无空槽 (plane=%hhu page=%hhu) — 中止\r\n", plane, (plane < PLANE_NUM) ? dir->cur.page[plane] : 0U);
            return MD_COMPACT_FAILED;
        }
    }

    if (md_submit(dir, MD_SUBMIT_WRITE, j->slot, j->cp_buf, L2P_REGION_SIZE) != FTL_OK)
    {
        if (md_handle_stall(dir))
        {
            return MD_COMPACT_FAILED;
        }
        return MD_COMPACT_BACKPRESSURE; /* 背压：**保留 slot**，下轮重试同一槽（重取会白烧一页）*/
    }

    return MD_COMPACT_SUBMITTED;
}

/**
 * @brief 当前 victim 搬空后，继续挑下一个能装进当前块剩余空槽的稀疏 victim。
 *
 * @param dir   Directory
 * @param alloc 分配器（读 BST 判 LOG 身份）
 * @return true = 已切到下一个 victim（phase=CP_READ）
 *         false = 没有可装下的 victim。
 *
 * @note victim 选择仍是“活 region 最少且 >0”；因为取的是最小值，若它也装不下，
 * 更大的更装不下 ⇒ 直接结束本轮压缩，交给目录检查点/回收。
 * 一次新生块可退多个稀疏块（每个 victim 约 1–2 个活 region），这是把
 * live_log_blocks 从上千块收敛到理想值的关键。
 */
static BOOL md_compact_next(md_dir_cache_s *dir, const alc_s *alloc)
{
    md_handle_s *j = &dir->handle;

    const WORD victim = md_compact_victim_find(dir, alloc);

    if (victim == UINT16_MAX)
    {
        return false;
    }

    const BYTE plane = ftl_gblock_plane(victim);

    if ((plane >= PLANE_NUM) || (dir->cur.valid[plane] == false))
    {
        return false;
    }

    const WORD free_slots = (PAGES_PER_BLOCK - dir->cur.page[plane]);
    const WORD need_slots = (dir->l2p_live[victim] + MD_COMPACT_RESERVED_SLOTS);

    if (need_slots > free_slots)
    {
        return false;
    }

    j->victim     = victim;
    j->region_id  = 0U;
    j->cur_region = 0U;
    j->slot       = NAND_PBA_INVALID;
    j->phase      = MD_JOB_CP_READ;
    return true;
}

/* ══════════════════════════════════════════════════════════════════
 * 四、job 状态机
 * ══════════════════════════════════════════════════════════════════ */

/**
 * @brief 待回收队列是否该**立即**擦（懒擦门控）。
 *
 * 开机时 `md_log_area_restore` 把日志区**全部**非活块登记为回收候选（不读介质判空，
 *），其中多数本来就是擦除态。若立即全擦，等于每次开机白付几十次
 * P/E；而其中只有「空闲块不足水位」的那部分才是真需要补的。
 *
 * @param dir   Directory
 * @param alloc 分配器（读固定日志区空闲块数）
 *
 * @return true  = 该擦（空闲块低于水位，或有借来的日志块待归还）
 *         false = 候选留在队列里，等水位降下来再擦
 *
 * @note 借用块例外：从 OP 池借来的日志块占用有上限的借用配额（L2P_LOG_BORROW_MAX），
 *       必须擦完归还，否则压缩再也拿不到目标块。
 */
static BOOL md_reclaim_is_eager(const md_dir_cache_s *dir, const alc_s *alloc)
{
    // 擦到「多于水位」为止：水位本身正是压缩触发点（`free <= WATERMARK ⇒ compact`），
    // 停在等于水位会让压缩在开机后一直空转。
    if (md_log_area_free_count(alloc) <= L2P_LOG_FREE_WATERMARK)
    {
        return true;
    }

    return (dir->log_borrow_count != 0U);
}

/**
 * @brief 推进整理 job 一步（每轮服务循环调一次）。见 metadata.h 的 md_handle 注释。
 *
 * @param dir Directory（allocator 走已绑定的 dir->alloc 端口）
 *
 * @note `for (;;)` 里每个分支要么设 waiting 后 return（提交阶段），要么推进 phase
 * 后 break（纯 RAM 阶段）。**不允许既不推进也不返回**——那会死循环。
 */
static void md_handle_step(md_dir_cache_s *dir)
{
    alc_s       *alloc = dir->alloc;
    md_handle_s *j     = &dir->handle;

    // 心跳（见 log.h / timer.h）：job 在飞期间每满 1s 打一行 phase/age。
    // 默认档（INFO）不打印；需要现场观测时 `log level debug`。
    if (j->phase == MD_JOB_IDLE)
    {
        log_heartbeat_end(&j->hb);
    }
    else if (j->hb.active == false)
    {
        log_heartbeat_begin(&j->hb);
    }
    else if (log_heartbeat_due(&j->hb))
    {
        LOGD(LOG_TAG_MD "job phase=%d victim=%hu moved=%hu pending=%hu age=%lums\r\n", j->phase, j->victim, j->moved, dir->reclaim.count, log_heartbeat_age_ms(&j->hb));
    }

    for (;;)
    {
        if (j->waiting)
        {
            return; /* 有命令在飞：等完成服务函数 */
        }

        switch (j->phase)
        {
            case MD_JOB_IDLE:
            {
                j->error = (int32_t)FTL_OK; /* 新 job 起点：清上一次的失败归因 */

                if ((dir->reclaim.count != 0U) && md_reclaim_is_eager(dir, alloc))
                {
                    // ③→④ 契约（同压缩路径）：普通 L2P flush 在活计数归零时就登记了
                    // 回收候选，但新位置只在 RAM Directory 里。擦除前必须先把当前
                    // Directory 写成目录检查点；否则掉电后 on-media 目录检查点仍指向该块。
                    if (dir->ckpt.dirty)
                    {
                        j->dir_ckpt_for_reclaim = true;
                        j->victim               = UINT16_MAX;
                        j->phase                = MD_JOB_CP_DIR_CKPT_PREP;
                        break;
                    }
                    j->phase = MD_JOB_RECLAIM;
                    break;
                }
                // free 水位是压缩的**主判据**：空闲日志块低于水位就压，不等 live
                // 涨到接近区上限（那时压缩已无目标块）。每轮扫 64 个块，开销可忽略。
                if ((j->compact_wanted == false) && (md_log_area_free_count(alloc) <= L2P_LOG_FREE_WATERMARK))
                {
                    j->compact_wanted = true;
                }
                if (j->compact_wanted == false)
                {
                    return; /* 无事可做 */
                }

                const md_compact_start_e started = md_compact_begin(dir, alloc);

                if (started != MD_COMPACT_STARTED)
                {
                    // 空槽预算取的是**活 region 最少**的 victim：连它都装不下，其它
                    // victim 只会更装不下，原地重试永远不会成功。日志区见底时强制
                    // 换块——新块的整块空槽是压缩唯一的 destination。
                    // ⚠ 换块原本只由 append_slot 触发：host 写一停就没有追加、不会
                    // 换块，压缩永远缺目标槽 ⇒ 日志槽永不放行、脏行无法驱逐
                    // （板端现场：live=64、job IDLE、`no_evictable`）。
                    const BOOL no_slot  = (started == MD_COMPACT_NO_SLOTS);
                    const BOOL no_cur   = (started == MD_COMPACT_NO_CURRENT);
                    const BOOL at_floor = (md_log_area_free_count(alloc) <= L2P_LOG_FREE_WATERMARK);

                    if ((no_slot || no_cur) && at_floor)
                    {
                        // 换 **victim 所在 plane** 的当前块（搬移目标就在那个 plane）。
                        const BYTE vp = (j->victim != UINT16_MAX) ? ftl_gblock_plane(j->victim) : 0U;

                        (void)md_log_rotate(dir, alloc, (vp < PLANE_NUM) ? vp : 0U);
                    }
                    return;
                }
                break;
            }

            case MD_JOB_RECLAIM:
            {
                // 目录检查点落后于 Directory（登记之后、擦除之前又有 commit）⇒ 不能擦；
                // 回 IDLE，下一轮先写目录检查点（上面的 dirty 分支）。
                if (dir->ckpt.dirty)
                {
                    j->phase = MD_JOB_IDLE;
                    break;
                }

                // 懒擦门控（同 IDLE 分支的判据）：空闲块够用就**不擦**，候选留在队列里。
                // 开机把日志区全部非活块登记为候选，其中多数本来就是擦除态——立即全擦
                // 等于每次开机白付几十次 P/E。
                if (md_reclaim_is_eager(dir, alloc) == false)
                {
                    j->phase = MD_JOB_IDLE;
                    break;
                }

                WORD g = 0U;

                if (md_reclaim_take(dir, &g) == false)
                {
                    // 计数非 0 但位图全空：计数与位图不一致（不该发生）。
                    // 清计数回 IDLE，避免每轮空转。
                    ASSERT(dir->reclaim.count == 0U);
                    dir->reclaim.count = 0U;
                    j->phase           = MD_JOB_IDLE;
                    break;
                }

                // 消费前**重查**（登记到消费之间可能隔了很多轮，块号可能已被重新分配）：
                // ① 活计数非 0 ⇒ 该块又被引用了；
                // ② 成了当前写块 ⇒ 正在被追加；
                // ③ 不再带 LOG ⇒ 已被别处归还/复用。
                // 任一命中即放弃本次（清位），**绝不擦活块**。
                if (md_log_block_is_reclaimable(dir, alloc, g) == false)
                {
                    md_reclaim_clear(dir, g);
                    j->phase = MD_JOB_IDLE;
                    break;
                }

                const DWORD addr = md_block_page_to_pba(dir, g, 0U);

                if (md_submit(dir, MD_SUBMIT_ERASE, addr, NULL, 0U) != FTL_OK)
                {
                    // 背压：位保持置位，下轮重试（md_handle_stall 可能已 abort，位仍保留）。
                    (void)md_handle_stall(dir);
                    return;
                }

                j->reclaim_gblock = g; /* 提交**成功**才认领；位在完成成功时才清 */
                return;
            }

            case MD_JOB_CP_READ:
            {
                const md_compact_step_e r = md_compact_read(dir);

                if (r == MD_COMPACT_FAILED)
                {
                    md_handle_abort(dir);
                    break;
                }
                if (r != MD_COMPACT_SCANNED)
                {
                    return; /* 已提交 / 背压 */
                }
                // 本 victim 已搬空。当前块还有空槽、且还有能装下的稀疏 victim ⇒ 继续搬
                // 下一个（一次新生块退多个块）。旧实现“一个 rotation 只压一个”会让
                // 碎片稳态停在 live≈N，日志区永远收不回来。
                if (md_compact_next(dir, alloc))
                {
                    break; /* phase 已置 CP_READ，循环继续 */
                }
                j->phase = MD_JOB_CP_DIR_CKPT_PREP;
                break;
            }

            case MD_JOB_CP_WRITE:
            {
                // md_compact_write 只返回 MD_COMPACT_SUBMITTED / MD_COMPACT_BACKPRESSURE /
                // MD_COMPACT_FAILED，**不会**返回 MD_COMPACT_SCANNED（"扫完"）
                // ——写一条就回去读下一条。
                const md_compact_step_e r = md_compact_write(dir);

                if (r == MD_COMPACT_FAILED)
                {
                    md_handle_abort(dir);
                    break;
                }
                return;
            }

            case MD_JOB_CP_DIR_CKPT_PREP:
            {
                // ③ 目录检查点：定位游标 + 取槽。游标未建立时会**同步**扫池定位——本 job
                // 唯一可能瞬时阻塞的点，且只在冷启动后首次整理出现。
                const md_dir_ckpt_step_e r = md_dir_ckpt_write_begin(dir, &j->dir_ckpt, MD_DIR_CKPT_OWNER_MD);

                if (r == MD_DIR_CKPT_FAILED)
                {
                    md_handle_abort(dir);
                    break;
                }
                if (r == MD_DIR_CKPT_BUSY)
                {
                    // 另一个 writer（CP job）正在写目录检查点：背压重试。
                    (void)md_handle_stall(dir);
                    return;
                }
                if (r == MD_DIR_CKPT_CLEAN)
                {
                    // Directory 已与 on-media 目录检查点一致：跳过写入，直接进入下一步。
                    // 普通回收路径回 RECLAIM；压缩路径回 CP_RECLAIM 擦 victim。
                    const BOOL for_reclaim = j->dir_ckpt_for_reclaim;

                    j->dir_ckpt_for_reclaim = false;
                    j->phase                = for_reclaim ? MD_JOB_RECLAIM : MD_JOB_CP_RECLAIM;
                    break;
                }

                j->phase = j->dir_ckpt.need_erase ? MD_JOB_CP_DIR_CKPT_ERASE : MD_JOB_CP_DIR_CKPT_WRITE;
                break;
            }

            case MD_JOB_CP_DIR_CKPT_ERASE:
                if (md_submit(dir, MD_SUBMIT_ERASE_MULTI, j->dir_ckpt.base, NULL, 0U) != FTL_OK)
                {
                    (void)md_handle_stall(dir);
                }
                return;

            case MD_JOB_CP_DIR_CKPT_WRITE:
            {
                if (j->dir_ckpt.chunk >= DIR_CKPT_PAGE_COUNT)
                {
                    // 整条 record 写完：推进游标（无论成败），成功才清 dirty。
                    // 失败 ⇒ 整条残缺（locate 会整条丢弃）⇒ 中止，不擦 victim。
                    md_dir_ckpt_commit(dir, &j->dir_ckpt);

                    if (j->dir_ckpt.ok == false)
                    {
                        LOGW(LOG_TAG_META "dir_ckpt write incomplete: idx=%hu seq=%lu (chunk 写失败)\r\n", j->dir_ckpt.idx, j->dir_ckpt.seq);
                        md_handle_abort(dir);
                        break;
                    }

                    // 写入期间又有 commit（混代/未覆盖最新 Directory）⇒ 不能擦 victim，
                    // 也不能登记新候选；回到 PREP 再写一条稳定记录。
                    if (dir->ckpt.dirty)
                    {
                        j->phase = MD_JOB_CP_DIR_CKPT_PREP;
                        break;
                    }

                    // 只有目录检查点确实覆盖了当前 Directory 时才允许登记新的回收候选：
                    // 否则 on-media 目录检查点仍引用「看起来已死」的块，擦了就是掉电丢映射。
                    md_reclaim_mark_all(dir, alloc);

                    if (j->dir_ckpt_for_reclaim)
                    {
                        j->dir_ckpt_for_reclaim = false;
                        j->phase                = MD_JOB_RECLAIM; /* dirty 已在上面挡住 */
                        break;
                    }

                    j->phase = MD_JOB_CP_RECLAIM;
                    break;
                }

                const DWORD aaddr = md_dir_ckpt_page_addr(j->dir_ckpt.base, j->dir_ckpt.chunk);

                md_dir_ckpt_page_s page; /* 2KB 栈缓冲：按页组好后写入 */

                md_dir_ckpt_page(dir, j->dir_ckpt.seq, j->dir_ckpt.chunk, &page);

                const DWORD page_size = sizeof(page);

                if (md_submit(dir, MD_SUBMIT_WRITE_MIRROR, aaddr, &page, page_size) != FTL_OK)
                {
                    (void)md_handle_stall(dir);
                }
                return;
            }

            case MD_JOB_CP_RECLAIM:
            {
                const WORD v = j->victim;

                // ④ 擦 victim —— 只在它**确实已无引用**时（③ 目录检查点已落盘）。
                if (v == UINT16_MAX)
                {
                    j->phase = MD_JOB_DONE;
                    break;
                }
                if (md_log_block_is_reclaimable(dir, alloc, v) == false)
                {
                    // 扫描期间又有 region 落回 victim（在飞的写回完成）⇒ 不能擦，留给下次。
                    LOGD(LOG_TAG_META "compact: victim %hu 仍有 %hu 活 region — 不擦\r\n", v, dir->l2p_live[v]);
                    j->phase = MD_JOB_DONE;
                    break;
                }

                const DWORD addr = md_block_page_to_pba(dir, v, 0U);

                if (md_submit(dir, MD_SUBMIT_ERASE, addr, NULL, 0U) != FTL_OK)
                {
                    (void)md_handle_stall(dir);
                }
                return;
            }

            case MD_JOB_DONE:
                LOGD(LOG_TAG_META "compact done victim=%hu moved=%hu\r\n", j->victim, j->moved);
                md_handle_reset(dir);
                break; /* 回 IDLE：继续循环（可能还有待回收的块）*/

            default:
                // 不该发生：phase 越界 ⇒ 清回 IDLE，否则 append_slot 永久让路。
                LOGE(LOG_TAG_META "md job bad phase=%d — reset\r\n", j->phase);
                md_handle_reset(dir);
                return;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════
 * 五、模块入口与状态
 * ══════════════════════════════════════════════════════════════════ */

/* ── 模块 handle（typed 入口） ── */

/**
 * @brief 元数据整理 job 每轮入口
 *
 * 推进整理状态机一次，再填状态上报。
 *
 * @param dir Directory
 */
void md_handle(md_dir_cache_s *dir)
{
    md_handle_step(dir);
}

/* ══════════════════════════════════════════════════════════════════
 * 六、完成服务（g_op_callbacks[FTL_OP_METADATA]）
 * ══════════════════════════════════════════════════════════════════ */

/* ── 整理 job 的完成服务 ── */

// 每个提交阶段的完成处理（ftl_metadata_callback 按 phase 分派）。
// ⚠ 与 cp_complete_* 不同，**不能**把 release 提到派发处统一做：CP_READ 必须先
// ftl_cmd_read_data 再 release（数据在 desc 链上）。

/**
 * @brief 擦除成功后的统一收尾：元数据日志块不经 migrate 擦除路径，磨损计数在这里补记，
 * 且**先记账再归还**。固定区块回固定区（NONE）；用户区借来的块回 Free Pool。
 *
 * @param dir    Directory
 * @param gblock 刚擦除完成的 gblock
 */
static void md_block_erased_note(md_dir_cache_s *dir, WORD gblock)
{
    const DWORD total_writes = *dir->total_writes;

    alc_bst_note_erase(dir->alloc, gblock, total_writes);
    md_log_release(dir, gblock);
}

/**
 * @brief 待回收块擦完成：失败 ⇒ 位保持置位（下一轮 kick 重新取到它并重试，轮转游标保证
 * 它不会永久挡住后续块）；成功 ⇒ 清位 + 归还。
 *
 * @param dir    Directory
 * @param j      整理 job（waiting / 连续失败计数 / phase）
 * @param cmd    完成命令（本函数 release）
 * @param failed 擦除是否失败
 */
static void md_reclaim_complete(md_dir_cache_s *dir, md_handle_s *j, fc_cmd_s *cmd, BOOL failed)
{
    (void)fc_command_release(cmd->cmd_id);
    j->waiting = false;

    if (failed)
    {
        // 同一块连续失败计数；达上限即标坏并清回收位。
        // 否则该块会被无限重试——数据安全但每次后台白付一条擦除命令，
        // 且坏块永远不会进入 BBT。
        if (j->reclaim_fail_gblock != j->reclaim_gblock)
        {
            j->reclaim_fail_gblock = j->reclaim_gblock;
            j->reclaim_fail_count  = 0U;
        }
        j->reclaim_fail_count++;

        if (j->reclaim_fail_count >= MD_RECLAIM_FAIL_MAX)
        {
            LOGE(LOG_TAG_META "reclaim erase failed blk=%hu x%hhu — mark BAD\r\n", j->reclaim_gblock, j->reclaim_fail_count);
            (void)bbt_mark_bad(dir->alloc, j->reclaim_gblock);
            md_log_release(dir, j->reclaim_gblock); /* BAD 也要结算 borrow 配额 */
            md_reclaim_clear(dir, j->reclaim_gblock);
            j->reclaim_fail_count  = 0U;
            j->reclaim_fail_gblock = UINT16_MAX;
        }
        else
        {
            LOGW(LOG_TAG_META "reclaim erase failed blk=%hu (%hhu/%u) — 位保留，下轮重试\r\n", j->reclaim_gblock, j->reclaim_fail_count, MD_RECLAIM_FAIL_MAX);
        }
    }
    else
    {
        j->reclaim_fail_count  = 0U;
        j->reclaim_fail_gblock = UINT16_MAX;
        md_reclaim_clear(dir, j->reclaim_gblock);
        md_block_erased_note(dir, j->reclaim_gblock);
        j->reclaimed_count++; /* 单调进度：区分「在回收」与「卡死」 */
    }

    j->reclaim_gblock = UINT16_MAX;
    j->phase          = MD_JOB_IDLE;
}

/**
 * @brief 整理 CP_READ 完成：成功把 region 整页拷进 job.cp_buf（**必须早于 release**）；
 * 读不回 ⇒ 中止——该 region 仍指向 victim 旧页，victim 不擦 ⇒ 无损失。
 *
 * @param dir    Directory
 * @param j      整理 job（waiting / phase）
 * @param cmd    完成命令（本函数 release）
 * @param failed 读是否失败
 */
static void md_cp_read_complete(md_dir_cache_s *dir, md_handle_s *j, fc_cmd_s *cmd, BOOL failed)
{
    BOOL read_ok = (failed == false);

    if (read_ok)
    {
        read_ok = (ftl_cmd_read_data(cmd, j->cp_buf, L2P_REGION_SIZE) == FTL_OK);
    }
    (void)fc_command_release(cmd->cmd_id);
    j->waiting = false;

    if (read_ok)
    {
        j->phase = MD_JOB_CP_WRITE;
        return;
    }

    // 源副本读失败/数据拷贝失败：先试另一份副本（主备内容相同），
    // 只降级一次；另一份也失败才中止。这样确定性坏页不会让 victim 永远搬不走。
    if (j->cur_fallback == false)
    {
        const DWORD other = (j->cur_copy == MD_COPY_PRIMARY) ? md_backup_find(dir, j->cur_region) : md_entry_find(dir, METADATA_TYPE_L2P_REGION, j->cur_region);

        // 另一份同样来自 Directory（H29）：非 INVALID 也要过 pba_is_valid 才能进 FC
        // （RISK-L2P-21）。
        if ((other != NAND_PBA_INVALID) && (pba_is_valid(other) == true) && (other != j->cur_source))
        {
            if (md_submit(dir, MD_SUBMIT_READ, other, NULL, 0U) == FTL_OK)
            {
                j->cur_fallback = true;
                return;
            }
        }
    }

    md_handle_abort(dir);
}

/**
 * @brief 整理 CP_WRITE 完成：成功登记新位置；失败 ⇒ 槽已消耗（可能半编程）⇒ **不登记**，
 * region 仍指向 victim 旧页 ⇒ 无损失。
 *
 * @param dir    Directory
 * @param j      整理 job（waiting / 当前搬移的 region 与副本 / phase）
 * @param cmd    完成命令（本函数 release）
 * @param failed 写是否失败
 */
static void md_cp_write_complete(md_dir_cache_s *dir, md_handle_s *j, fc_cmd_s *cmd, BOOL failed)
{
    (void)fc_command_release(cmd->cmd_id);
    j->waiting = false;

    if (failed)
    {
        md_handle_abort(dir);
        return;
    }

    const DWORD current = (j->cur_copy == MD_COPY_PRIMARY) ? md_entry_find(dir, METADATA_TYPE_L2P_REGION, j->cur_region) : md_backup_find(dir, j->cur_region);

    if (current != j->cur_source)
    {
        // 读页到写完成之间，前台写回把该副本移到了更新的位置 ⇒ 不能再拿旧页覆盖。
        // 新槽已写但未登记，只是死页；让下一轮重新扫描当前 Directory。
        LOGD(LOG_TAG_META "compact: region %hu copy=%d superseded (%08lx -> %08lx), skip commit\r\n", j->cur_region, (int)j->cur_copy, j->cur_source, current);
        j->slot  = NAND_PBA_INVALID;
        j->phase = MD_JOB_CP_READ;
        return;
    }

    if (j->cur_copy == MD_COPY_PRIMARY)
    {
        md_commit_region(dir, j->cur_region, MD_COMMIT_PRIMARY, j->slot, NAND_PBA_INVALID);
    }
    else
    {
        md_commit_region(dir, j->cur_region, MD_COMMIT_BACKUP, NAND_PBA_INVALID, j->slot);
    }
    j->slot  = NAND_PBA_INVALID;
    j->moved = (j->moved + 1U);
    j->phase = MD_JOB_CP_READ;
}

/**
 * @brief 目录检查点换块擦完成：失败 ⇒ 不写、**不推进游标**，dirty 保留 ⇒ 下次在新槽重试整条。
 *
 * @param dir    Directory
 * @param j      整理 job（waiting / phase）
 * @param cmd    完成命令（本函数 release）
 * @param failed 擦除是否失败
 */
static void md_dir_ckpt_erase_complete(md_dir_cache_s *dir, md_handle_s *j, fc_cmd_s *cmd, BOOL failed)
{
    (void)fc_command_release(cmd->cmd_id);
    j->waiting = false;

    if (failed)
    {
        md_handle_abort(dir);
        return;
    }
    j->phase = MD_JOB_CP_DIR_CKPT_WRITE;
}

/**
 * @brief 目录检查点一页写完：无论成败都往后写下一页；失败 ⇒ 本条 record 残缺 ⇒ 整条作废。
 *
 * @param j      整理 job（waiting / dir_ckpt 共享上下文）
 * @param cmd    完成命令（本函数 release）
 * @param failed 本页写是否失败
 */
static void md_dir_ckpt_write_complete(md_handle_s *j, fc_cmd_s *cmd, BOOL failed)
{
    (void)fc_command_release(cmd->cmd_id);
    j->waiting = false;

    (void)md_dir_ckpt_write_advance(&j->dir_ckpt, failed); /* 失败整条作废，页序仍前进 */
}

/**
 * @brief Victim 擦完成：失败 ⇒ victim 已无引用（目录检查点已落盘）⇒ 登记给回收阶段重试。
 *
 * @param dir    Directory
 * @param j      整理 job（waiting / victim / phase）
 * @param cmd    完成命令（本函数 release）
 * @param failed 擦除是否失败
 */
static void md_cp_reclaim_complete(md_dir_cache_s *dir, md_handle_s *j, fc_cmd_s *cmd, BOOL failed)
{
    (void)fc_command_release(cmd->cmd_id);
    j->waiting = false;

    if (failed)
    {
        LOGW(LOG_TAG_META "compact: victim erase failed blk=%hu — 登记重试\r\n", j->victim);
        (void)md_reclaim_mark(dir, j->victim);
    }
    else
    {
        md_block_erased_note(dir, j->victim);
    }

    j->phase = MD_JOB_DONE;
}

/**
 * @brief 元数据整理命令完成服务
 *
 * 只记结果 + 改 phase；不推进状态机（阶段里有同步原语，完成泵内会嵌套泵）。
 *
 * @param ftl        实例
 * @param completion FC 完成状态
 * @param ctx        md_dir_cache_s *
 */
void ftl_metadata_callback(ftl_ctx_s *ftl, const fc_cmd_status_s *completion, void *ctx)
{
    // 形参由完成派发签名固定；release 下 ASSERT 裁空 ⇒ 显式丢弃（H30）。
    (void)ftl;
    md_dir_cache_s *dir = (md_dir_cache_s *)ctx;

    fc_cmd_s *cmd = fc_command_id_to_command(completion->cmd_id);

    // 实例由 ctx 反推；ftl 形参是 dispatch 解析出的归属实例，二者必须一致
    // ——不一致说明派发路由错了。
    ASSERT(ftl->metadata_dir == dir);

    md_handle_s *j = &dir->handle;

    const BOOL failed = fc_cmd_status_has_error(completion->status);

    switch (j->phase)
    {
        case MD_JOB_RECLAIM:
            md_reclaim_complete(dir, j, cmd, failed);
            return;

        case MD_JOB_CP_READ:
            md_cp_read_complete(dir, j, cmd, failed);
            return;

        case MD_JOB_CP_WRITE:
            md_cp_write_complete(dir, j, cmd, failed);
            return;

        case MD_JOB_CP_DIR_CKPT_ERASE:
            md_dir_ckpt_erase_complete(dir, j, cmd, failed);
            return;

        case MD_JOB_CP_DIR_CKPT_WRITE:
            md_dir_ckpt_write_complete(j, cmd, failed);
            return;

        case MD_JOB_CP_RECLAIM:
            md_cp_reclaim_complete(dir, j, cmd, failed);
            return;

        case MD_JOB_IDLE:
        case MD_JOB_CP_DIR_CKPT_PREP:
        case MD_JOB_DONE:
        default:
            // 不该发生：一条命令对应一个提交阶段（RECLAIM / CP_READ / CP_WRITE /
            // CP_DIR_CKPT_ERASE / CP_DIR_CKPT_WRITE / CP_RECLAIM）。
            LOGE(LOG_TAG_META "md job unexpected completion phase=%d\r\n", j->phase);
            (void)fc_command_release(cmd->cmd_id);
            j->waiting = false;
            md_handle_abort(dir);
            return;
    }
}

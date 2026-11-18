/*
 * wbuf.c — 页打包写缓冲（page-packing write buffer）
 *
 * 职责：
 *   把**同一 pair 窗口**内的多次 host 写攒成一页，窗口填满 / 切到别的窗口 / 显式 flush /
 *   空闲时**一次编程**（两 plane 都有数据即一条 multi-plane）⇒ 写指针按页推进，消除
 *   「逐子操作分配整对页」的浪费（背景与不变量见 wbuf.h 文件头）。
 *
 * 文件结构：
 *   1. 槽位位 / 查询    wb_lba_bit（static）/ wb_has / wb_read
 *   2. 攒入与编程       wb_flush / wb_append（static）
 *   3. 丢弃 / 复位      wb_drop / wb_reset（static）
 *   4. 空闲 flush       wb_maybe_flush_idle（static）
 *   5. typed 入口       wb_init / wb_status_fill（static）/ wb_handle
 *
 * ⚠ 模块边界：本文件不 include ftl.h。只持有 allocator / pending 表 / migrate handle
 *   三个窄指针，调用它们的 owner API。
 *   由 ftl.c 在 ftl_init 中绑定（docs/archive/ftl_module_contracts.md §4.8）。
 */

#include "wbuf.h"

#include "assertion.h"
#include "fc_interface.h"     /* fc_command_alloc / fc_command_release / FC_* 配置常量 */
#include "ftl_fc_interface.h" /* ftl_cmd_* / ftl_plane_* */
#include "ftl_op.h"           /* ftl_op_submit / FTL_OP_HOST_IO */
#include "log.h"
#include "migrate.h"      /* mig_handle_s / mig_gc_request_urgent */
#include "ssd_compiler.h" /* DTCM_DATA */
#include "timer.h"        /* platform_now_ms（取不到页对的兜底计时） */

#include <string.h>

// 每实例页打包写缓冲：定义与内存域（DTCM，CPU-only 热数据）都归本模块。
// NOLOAD ⇒ 启动不清零，ftl_init 绑定后对该实例显式 memset。
static wb_handle_s s_wbuf[FTL_DIE_MAX] DTCM_DATA;

/**
 * @brief 取本实例的页打包写缓冲工作区（定义在 wbuf.c，DTCM）
 *
 * @param die 紧凑 die 号（< FTL_DIE_MAX）
 * @return 本实例句柄（非空）
 */
wb_handle_s *wb_workspace_get(BYTE die)
{
    ASSERT(die < FTL_DIE_MAX);
    return &s_wbuf[die];
}

/**
 * @brief 该 LBA 在 flat CW 位图（occupied / 命令 cw_bitmap）里的位。
 *
 * ⚠ 走 ftl_cw_bit 而非手写 `1U << ftl_lba_pair_slot(lba)`——两者逐值同构
 * （slot ≡ plane*CW_NUM + cw），而 ftl_cw_bit 已文档化为**所有** CW 位图的唯一构造器
 * （见 ftl_config.h）。ftl_lba_pair_slot 仍用于 data[] 的**数组下标**（不是位）。
 *
 * @param lba 逻辑地址
 * @return 位掩码（bit = plane*CW_NUM + cw）
 */
static BYTE wb_lba_bit(DWORD lba)
{
    return ftl_cw_bit(ftl_lba_plane(lba), ftl_lba_cw(lba));
}

/* ── 1. 查询 ── */

/**
 * @brief 查询该 LBA 在当前窗口的缓冲槽是否有数据
 *
 * @param h   wbuf handle
 * @param lba 全局 host LBA
 * @return true = 命中，可从缓冲直接读；false = 未命中 / 窗口未开
 */
BOOL wb_has(const wb_handle_s *h, DWORD lba)
{
    if (h->open == false)
    {
        return false;
    }
    if (ftl_lba_pair_base(lba) != h->base)
    {
        return false;
    }

    return (h->occupied & wb_lba_bit(lba)) != 0U;
}

/**
 * @brief 从缓冲拷出 1 IU
 *
 * 未命中不写 out；拷出后缓冲仍保持占用。
 *
 * @param h   wbuf handle
 * @param lba 全局 host LBA
 * @param out 输出 IU（调用者保证非 NULL）
 * @return true = 命中并拷出；false = 未命中（out 未写）
 */
BOOL wb_read(const wb_handle_s *h, DWORD lba, void *out)
{
    if (wb_has(h, lba) == false)
    {
        return false;
    }

    const BYTE slot = ftl_lba_pair_slot(lba);

    memcpy(out, &h->data[slot * FTL_IU_BYTES], FTL_IU_BYTES);
    return true;
}

/* ── 2. 攒入与编程 ── */

/**
 * @brief 把当前缓冲页编程成一条 NAND 写命令并注册 pending。
 * 空缓冲直接返回 FTL_OK；资源不足一律背压，缓冲原样保留（下轮重试）。
 *
 * @param h wbuf handle
 * @return FTL_OK    空缓冲 / 已提交（提交成功即清缓冲）
 *         FTL_AGAIN host 写预算不足 / pending 表满 / 取不到页对（已登记紧急 GC）/
 *                   page0 异步写屏障未满足 / 建链资源（命令 / 描述符 / 缓冲池）不足 / SQ 满
 *         FTL_ERROR 建链失败（入参、地址或命令链错误）
 */
static ftl_status_e wb_flush(wb_handle_s *h)
{
    if ((h->open == false) || (h->occupied == 0U))
    {
        return FTL_OK; /* 空：nothing to do */
    }

    // host 写给后台留资源：池接近满时先背压，别把 CP/GC/journal 的命令槽吃光。
    // 页留在 wbuf 里不丢，下一轮继续（FTL_AGAIN 由组重试）。见 ftl_fc_interface.h。
    if (ftl_fc_host_budget_ok() == false)
    {
        return FTL_AGAIN;
    }

    // 资源次序：先查 pending 表余量（不占资源），再取页对；命令/desc/buffer 由
    // ftl_async_write_pair_cmd() 在页对之后分配。取不到页对时无资源占用；页对之后的
    // 任何失败都由统一错误出口 alc_super_open_rewind_pair 归还（否则页对凭空蒸发，
    // 表现为容量无声流失，见 CLAUDE.md §3.1）。
    if (pend_slot_get_count(h->pending) >= FTL_PENDING_MAX)
    {
        return FTL_AGAIN;
    }

    // 分配新页（**逐 plane 独立选块**——Policy B）。Free Pool 耗尽 → **背压**：
    // 登记紧急 GC 后返回 FTL_AGAIN，缓冲原样保留，由 ftl_drain_group 下轮重试。
    //
    // ⚠ 搬移是异步 job：在完成回调/写路径里同步等它会把 superloop 卡死。
    // 这里**只登记请求**，由 must-run 的 mig_handle 下一轮启动。
    DWORD pba[PLANE_NUM];

    const ftl_status_e alc_st = alc_super_open_alloc_pair(h->alloc, ALC_SUPER_ALLOC_HOT, pba);

    if (alc_st != FTL_OK)
    {
        // H38：永久失败（如 Closed 列表满）原样上报，不能伪装成瞬态重试。
        if (alc_st != FTL_AGAIN)
        {
            return alc_st;
        }

        // migrate 是本模块的**可选端口**：纯 RAM 单测用 wb_init(..., NULL) 隔离
        // 后台迁移器，此时只做背压，不登记紧急 GC（见 wbuf.h wb_init 契约）。
        if (h->migrate != NULL)
        {
            mig_gc_request_urgent(h->migrate);
        }

        // 兜底：连续取不到页对超过上限 ⇒ 判真无块，按 FTL_ERROR 结算 host 写。
        // 没有它时 `wb_busy` 会被 drain 当成“有进展”，组无限重试 ⇒ host 命令永不
        // 完成 ⇒ USB BOT 超时/设备消失（无界挂起）。
        const DWORD now_ms = platform_now_ms();

        if (now_ms != 0U)
        {
            if (h->alloc_fail_ms == 0U)
            {
                h->alloc_fail_ms = now_ms;
            }
            else if ((now_ms - h->alloc_fail_ms) > WBUF_ALLOC_STALL_TIMEOUT_MS)
            {
                h->alloc_fail_ms = 0U;
                LOGE(LOG_TAG_FTL "wbuf: no block for %lums — fail host write (free=%hu)\r\n", (unsigned long)WBUF_ALLOC_STALL_TIMEOUT_MS, h->alloc->free_count);
                return FTL_ERROR;
            }
        }
        return alc_st;
    }

    h->alloc_fail_ms = 0U; /* 取到页对：清兜底计时 */

    // page0 异步写屏障：page>0 的写必须等同一 plane 的 page0 成功完成。
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        if (bit_mask_any(h->occupied, ftl_cw_plane_mask(p)) && (pba_to_page(pba[p]) > 0U) && (alc_super_open_get_pba_state(h->alloc, FTL_TEMP_HOT, pba[p]) != ALC_SUPER_PBA_OPEN_PAGE0_DONE))
        {
            alc_super_open_rewind_pair(h->alloc, FTL_TEMP_HOT);
            return FTL_AGAIN;
        }
    }

    // 本页覆盖的 plane 数（决定 operation 码与 plane 段）与实际 CW 数。
    const BYTE nplanes = ftl_cw_bitmap_plane_count(h->occupied);
    const BYTE n_cw    = ftl_cw_bitmap_popcount(h->occupied);
    const BOOL page0   = (pba_to_page(pba[0]) == 0U);

    // page0 补全：缺失 plane 补零，命令按 full pair 写，保证两个 plane 的 page0
    // 都非空白（page0 异步写屏障 / full rebuild 的 page0 捷径前提）。
    // 映射仍只按原 occupied 位图确认，补出的 CW 不指向任何 LBA。
    DWORD write_bitmap = h->occupied;
    BYTE  write_planes = nplanes;

    if (page0)
    {
        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            if (bit_mask_any(h->occupied, ftl_cw_plane_mask(p)) == false)
            {
                memset(&h->data[p * CW_NUM * FTL_IU_BYTES], 0, CW_NUM * FTL_IU_BYTES);
            }
        }
        write_bitmap = FTL_CACHE_FULL_CW;
        write_planes = PLANE_NUM;
    }

    const ftl_page_cfg_s *cfg = FTL_PAGE_CFG_DATA(ftl_lba_page(h->base));
    fc_cmd_s             *cmd = NULL;

    ftl_status_e ret = ftl_async_write_pair_cmd(&cmd, pba, h->data, write_bitmap, ftl_plane_write_op(write_planes), cfg);

    if (ret != FTL_OK)
    {
        goto wbuf_flush_fail;
    }

    // 注册 pending（owners = 贡献本页的组位掩码；lba = 窗口基址；buffer 传 NULL）。
    // 认领槽位后就地逐字段填——字段名即文档（见 pend_req_s 注释）。
    pend_req_s *pending = pend_alloc(h->pending, ftl_cmd_id(cmd));

    if (pending == NULL)
    {
        ret = FTL_AGAIN; /* pending 表满：瞬态 */
        goto wbuf_flush_fail;
    }

    pending->owners     = h->owners;
    pending->op         = page0 ? PENDING_OP_WRITE_PAGE0 : PENDING_OP_WRITE;
    pending->n_cw       = n_cw;
    pending->cw_bitmap  = h->occupied;
    pending->lba        = h->base;
    pending->buffer     = NULL;
    pending->new_pba    = pba[0];
    pending->new_pba1   = pba[1];
    pending->cache_role = PEND_CACHE_ROLE_NONE;
    pend_validate(pending);

    // 提交（tag = HOST_IO ⇒ 完成时由 g_op_callbacks[FTL_OP_HOST_IO] 收尾）。
    // ctx 传 NULL：host I/O 的私有状态在 pending 表里（比request 表 更全）。
    if (ftl_op_submit(cmd, FTL_OP_HOST_IO, NULL) != FTL_OK)
    {
        const WORD cmd_id = ftl_cmd_id(cmd);

        pend_remove(h->pending, cmd_id); /* 先摘 pending */
        ret = FTL_AGAIN;                 /* SQ 满：瞬态（缓冲保留，下次重试） */
        goto wbuf_flush_fail;
    }

    // 观测口：一页一次编程；base/cw/nplanes/owners 即"合并了多少"的判据。
    // ⚠ LOGD（不是 LOGI）：批量写下每 4KB 一行、UART 阻塞 ~5ms，比 NAND 编程慢一个量级。
    LOGD(LOG_TAG_FTL "wbuf flush base=%lu cw=%hhu nplanes=%hhu owners=0x%lx\r\n", h->base, n_cw, write_planes, h->owners);

    // 提交成功才清缓冲
    h->open     = false;
    h->occupied = 0U;
    h->owners   = 0U;
    h->base     = 0U;
    h->flushed_pages++;

    return FTL_OK;

// 统一错误出口（H8 允许的唯一 goto）：释放命令 + **归还页对**。缓冲原样保留 ⇒ 下轮重试；
// 页对尚未编程（命令从未提交）⇒ 归还是安全的。
wbuf_flush_fail:
    if (cmd != NULL)
    {
        (void)fc_command_release(cmd->cmd_id);
    }
    alc_super_open_rewind_pair(h->alloc, FTL_TEMP_HOT);
    return ret;
}

/**
 * @brief 把一个组的 n 个 CW 攒进当前窗口；窗口切换时先编程旧页。
 *
 * @param h      wbuf handle
 * @param gi     贡献组号（登记进 owners / pending 子操作计数）
 * @param lba    起始 LBA（n 个连续 LBA）
 * @param buffer 数据源（n × FTL_IU_BYTES）
 * @param n      CW 数
 * @return FTL_OK    已攒入（满页立即编程失败也返回 OK，留给 idle flush 重试）
 *         FTL_AGAIN 换窗口时旧页 flush 背压，本子操作未消费，需重试
 */
static ftl_status_e wb_append(wb_handle_s *h, WORD gi, DWORD lba, const void *buffer, BYTE n)
{
    const DWORD window = ftl_lba_pair_base(lba);

    // 换窗口：先把旧窗口编程掉。flush 失败（背压）则本子操作不消费，下轮重试。
    if ((h->open) && (h->base != window))
    {
        ftl_status_e st = wb_flush(h);

        if (st != FTL_OK)
        {
            return st;
        }
    }

    if (h->open == false)
    {
        h->open     = true;
        h->base     = window;
        h->occupied = 0U;
        h->owners   = 0U;
    }

    const BYTE *src = (const BYTE *)buffer;

    for (BYTE k = 0U; k < n; k++)
    {
        const DWORD  l          = (lba + k);
        const BYTE   slot       = ftl_lba_pair_slot(l);
        const size_t dst_offset = slot * FTL_IU_BYTES;
        const size_t src_offset = k * FTL_IU_BYTES;

        memcpy(&h->data[dst_offset], src + src_offset, FTL_IU_BYTES);
        h->occupied |= wb_lba_bit(l);
    }
    h->owners |= ftl_group_bit(gi);
    pend_group_add_subop(h->pending, gi);

    // 满：尝试立即编程（失败则留给 idle flush 重试；append 本身已成功 ⇒ 仍返 OK，
    // 否则调用者会重发同一子操作 ⇒ 重复计数）。
    if (h->occupied == FTL_CACHE_FULL_CW)
    {
        (void)wb_flush(h);
    }

    return FTL_OK;
}

/* ── 3. 丢弃 / 复位 ── */

/**
 * @brief 从缓冲丢弃一个 LBA（TRIM）；窗口被清空时结算全部贡献组的在飞数。
 *
 * @param h   wbuf handle
 * @param lba 逻辑地址
 * @return true = 命中并已丢弃
 *         false = 不在缓冲（无打开窗口 / pair 窗口不同 / 槽位为空）
 */
static BOOL wb_drop(wb_handle_s *h, DWORD lba)
{
    if (wb_has(h, lba) == false)
    {
        return false;
    }

    h->occupied &= ~wb_lba_bit(lba);
    h->drop_count++;

    if (h->occupied == 0U)
    {
        // 窗口被清空：本页不再编程，退掉贡献者的 append（它们从未真正写盘）。
        // 每个贡献组对该窗口至多一次 append ⇒ 逐组递减一次即在飞数配平。
        // ⚠ 本动作是 wbuf 的**内部不变量**（occupied 清空 ⟺ owners 必须结算），
        // 收在这里，不由调用者手工维护（CLAUDE.md §3.1）。
        pend_group_complete_subop_multi(h->pending, h->owners, SSD_STATUS_OK);
        h->open   = false;
        h->owners = 0U;
        h->base   = 0U;
    }

    return true;
}

/**
 * @brief 丢弃整页缓冲，并按写失败（IO_ERROR）结算 owners 的在飞数。
 *
 * @param h wbuf handle
 */
static void wb_reset(wb_handle_s *h)
{
    if (h->owners != 0U)
    {
        // 缓冲里的写被整体丢弃（介质已擦净）⇒ 按**写失败**收尾。不能只清零 owners：
        // 那些组仍在等子操作完成，直接丢会让它们的在飞数永不归零、host 写挂死。
        pend_group_complete_subop_multi(h->pending, h->owners, SSD_STATUS_IO_ERROR);
    }

    h->open     = false;
    h->occupied = 0U;
    h->owners   = 0U;
    h->base     = 0U;
}

/* ── 4. 空闲 flush ── */

/**
 * @brief 无打开窗口或仍有写组会 append 本窗口时不动；否则尝试编程。
 *
 * @param h wbuf handle
 * @return FTL_OK 无需编程或已提交；其余同 wb_flush（失败时缓冲原样保留，
 *         错误由 wb_handle 锁存 ⇒ host FLUSH 可快速失败，不等墙钟超时）
 */
static ftl_status_e wb_maybe_flush_idle(wb_handle_s *h)
{
    if (h->open == false)
    {
        return FTL_OK;
    }

    if (pend_group_has_window_pending(h->pending, h->base))
    {
        return FTL_OK; /* 还有写组会把数据 append 进本窗口 → 先别 flush */
    }

    return wb_flush(h);
}

/* ── 5. typed 入口 ── */

/**
 * @brief 初始化 wbuf handle
 *
 * 整块清零（窗口 / 占用 / 组位图 / 计数）后绑定 owner 指针；不碰介质。
 *
 * @param h       wbuf handle
 * @param alloc   allocator
 * @param pending pending 表
 * @param migrate migrate handle
 */
void wb_init(wb_handle_s *h, alc_s *alloc, pend_ctx_s *pending, struct MIGRATE_HANDLE_S *migrate)
{
    // 前置条件：ftl_init 调本函数前已由 contract_check_handles 证明各容器非空
    // （alloc / pending 为实例内嵌对象的地址，migrate 由同一审计保证）⇒ 不重复检查。
    memset(h, 0, sizeof(*h));
    h->alloc   = alloc;
    h->pending = pending;
    h->migrate = migrate;
}

/**
 * @brief 用 handle 当前状态填写上报结构。
 *
 * @param h   wbuf handle
 * @param out 出参：状态上报（调用者保证非 NULL）
 */
static void wb_status_fill(const wb_handle_s *h, wb_status_s *out)
{
    out->base.critical  = false; /* wbuf 是 host 写路径的一部分，由 host 补发步驱动 */
    out->base.wants_run = h->open || (h->occupied != 0U);
    out->base.progress  = h->flushed_pages;
    out->base.next_ms   = 0U;
    out->base.error     = h->error;

    /* 硬失败不作为独立状态：flush 是 best-effort，失败由 `base.error` 与
     * 上层的 deferred error 表达；下一次成功的 FLUSH/RESET 自然清除。 */
    if (h->open)
    {
        out->state      = WBUF_STATE_OPEN;
        out->base.state = FTL_MODULE_RUNNING;
    }
    else
    {
        out->state      = WBUF_STATE_IDLE;
        out->base.state = FTL_MODULE_IDLE;
    }

    out->open          = h->open;
    out->occupied      = h->occupied;
    out->owners        = h->owners;
    out->flushed_pages = h->flushed_pages;
    out->drop_count    = h->drop_count;
}

/**
 * @brief wbuf typed 入口：按 trigger->kind 分发
 *
 * 只做分发，不承载事件逻辑。错误锁存：任一 kind 返回 FTL_ERROR 即置位；
 * 只有成功的 FLUSH / MAYBE_FLUSH_IDLE / RESET 清除（APPEND / DROP 的成功不代表
 * 写路径恢复），经 `wb_status_s.base.error` 上报。不设独立失败状态：FLUSH 继续
 * 尝试把窗口推下去，由 flush 自身的超时兜底。
 *
 * @param h       wbuf handle
 * @param trigger 触发描述
 * @param out     可选状态上报（可为 NULL）
 * @return 各 kind 的返回语义见 wbuf.h
 */
ftl_status_e wb_handle(wb_handle_s *h, const wb_trigger_s *trigger, wb_status_s *out)
{
    ftl_status_e st = FTL_OK;

    switch (trigger->kind)
    {
        case WBUF_TRIGGER_APPEND:
            st = wb_append(h, trigger->gi, trigger->lba, trigger->data, trigger->n);
            break;

        case WBUF_TRIGGER_FLUSH:
            st = wb_flush(h);
            break;

        case WBUF_TRIGGER_DROP:
            (void)wb_drop(h, trigger->lba);
            break;

        case WBUF_TRIGGER_RESET:
            wb_reset(h);
            break;

        case WBUF_TRIGGER_MAYBE_FLUSH_IDLE:
            st = wb_maybe_flush_idle(h);
            break;

        default:
            st = FTL_ERROR;
            break;
    }

    if (st == FTL_ERROR)
    {
        h->error = FTL_ERROR;
    }
    else if ((st == FTL_OK) && ((trigger->kind == WBUF_TRIGGER_FLUSH) || (trigger->kind == WBUF_TRIGGER_MAYBE_FLUSH_IDLE) || (trigger->kind == WBUF_TRIGGER_RESET)))
    {
        h->error = FTL_OK;
    }

    if (out != NULL)
    {
        wb_status_fill(h, out);
    }
    return st;
}

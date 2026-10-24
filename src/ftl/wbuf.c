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
#include "fc.h"               /* fc_command_alloc / fc_command_release / FC_* 配置常量 */
#include "ftl_fc_interface.h" /* ftl_cmd_* / ftl_plane_* */
#include "ftl_op.h"           /* ftl_op_submit / FTL_OP_HOST_IO */
#include "log.h"
#include "migrate.h" /* mig_handle_t / mig_gc_request_urgent */

#include <string.h>

/* 该 LBA 在 flat CW 位图（occupied / 命令 cw_bitmap）里的位。
 * ⚠ 走 ftl_cw_bit 而非手写 `1U << ftl_lba_pair_slot(lba)`——两者逐值同构
 *   （slot ≡ plane*CW_NUM + cw），而 ftl_cw_bit 已文档化为**所有** CW 位图的唯一构造器
 *   （见 ftl_config.h）。ftl_lba_pair_slot 仍用于 data[] 的**数组下标**（不是位）。 */
static BYTE wb_lba_bit(DWORD lba)
{
    return ftl_cw_bit(ftl_lba_plane(lba), ftl_lba_cw(lba));
}

/* ── 1. 查询 ── */

bool wb_has(const wb_handle_t *h, DWORD lba)
{
    if (!h->open)
    {
        return false;
    }
    if (ftl_lba_pair_base(lba) != h->base)
    {
        return false;
    }

    return (h->occupied & wb_lba_bit(lba)) != 0U;
}

bool wb_read(const wb_handle_t *h, DWORD lba, void *out)
{
    if (!wb_has(h, lba))
    {
        return false;
    }

    const BYTE slot = ftl_lba_pair_slot(lba);

    (void)memcpy(out, &h->data[slot * FTL_IU_BYTES], FTL_IU_BYTES);
    return true;
}

/* ── 2. 攒入与编程 ── */

static ftl_status_e wb_flush(wb_handle_t *h)
{
    if ((!h->open) || (h->occupied == 0U))
    {
        return FTL_OK; /* 空：nothing to do */
    }

    /* host 写给后台留资源：池接近满时先背压，别把 CP/GC/journal 的命令槽吃光。
     * 页留在 wbuf 里不丢，下一轮继续（FTL_AGAIN 由组重试）。见 ftl_fc_interface.h。 */
    if (!ftl_fc_host_budget_ok())
    {
        return FTL_AGAIN;
    }

    /* 资源次序：先查 pending 表余量（不占资源），再取页对；命令/desc/buffer 由
     * ftl_async_write_pair_cmd() 在页对之后分配。取不到页对时无资源占用；页对之后的
     * 任何失败都由统一错误出口 alc_super_open_rewind_pair 归还（否则页对凭空蒸发，
     * 表现为容量无声流失，见 CLAUDE.md §3.1）。 */
    if (pend_slot_get_count(h->pending) >= FTL_PENDING_MAX)
    {
        return FTL_AGAIN;
    }

    /* 分配新页（**逐 plane 独立选块**——Policy B）。Free Pool 耗尽 → **背压**：
     * 登记紧急 GC 后返回 FTL_AGAIN，缓冲原样保留，由 ftl_drain_group 下轮重试。
     *
     * ⚠ 搬移是异步 job：在完成回调/写路径里同步等它会把 superloop 卡死。
     *   这里**只登记请求**，由 must-run 的 mig_handle 下一轮启动。 */
    DWORD pba[PLANE_NUM];

    if (alc_super_open_alloc_pair(h->alloc, ALC_SUPER_ALLOC_HOT, pba) != FTL_OK)
    {
        mig_gc_request_urgent(h->migrate);
        return FTL_AGAIN;
    }

    /* page0 异步写屏障：page>0 的写必须等同一 plane 的 page0 成功完成。 */
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        if (bit_mask_any(h->occupied, ftl_cw_plane_mask(p)) && (pba_to_page(pba[p]) > 0U) && (alc_super_open_get_pba_state(h->alloc, FTL_TEMP_HOT, pba[p]) != ALC_SUPER_PBA_OPEN_PAGE0_DONE))
        {
            alc_super_open_rewind_pair(h->alloc, FTL_TEMP_HOT);
            return FTL_AGAIN;
        }
    }

    /* 本页覆盖的 plane 数（决定 operation 码与 plane 段）与实际 CW 数。 */
    const BYTE nplanes = ftl_cw_bitmap_plane_count(h->occupied);
    const BYTE n_cw    = ftl_cw_bitmap_popcount(h->occupied);
    const bool page0   = (pba_to_page(pba[0]) == 0U);

    /* page0 补全：缺失 plane 补零，命令按 full pair 写，保证两个 plane 的 page0
     * 都非空白（page0 异步写屏障 / full rebuild 的 page0 捷径前提）。
     * 映射仍只按原 occupied 位图确认，补出的 CW 不指向任何 LBA。 */
    DWORD write_bitmap = h->occupied;
    BYTE  write_planes = nplanes;

    if (page0)
    {
        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            if (!bit_mask_any(h->occupied, ftl_cw_plane_mask(p)))
            {
                memset(&h->data[p * CW_NUM * FTL_IU_BYTES], 0, CW_NUM * FTL_IU_BYTES);
            }
        }
        write_bitmap = FTL_CACHE_FULL_CW;
        write_planes = PLANE_NUM;
    }

    const ftl_page_cfg_t *cfg = FTL_PAGE_CFG_DATA(ftl_lba_page(h->base));
    fc_command_t         *cmd = NULL;

    ftl_status_e ret = ftl_async_write_pair_cmd(&cmd, pba, h->data, write_bitmap, ftl_plane_write_op(write_planes), cfg);

    if (ret != FTL_OK)
    {
        goto wbuf_flush_fail;
    }

    /* 注册 pending（owners = 贡献本页的组位掩码；lba = 窗口基址；buffer 传 NULL）。
     * 认领槽位后就地逐字段填——字段名即文档（见 pend_req_t 注释）。 */
    pend_req_t *pending = pend_alloc(h->pending);

    if (pending == NULL)
    {
        ret = FTL_AGAIN; /* pending 表满：瞬态 */
        goto wbuf_flush_fail;
    }

    pending->command_id         = ftl_cmd_id(cmd);
    pending->owners             = h->owners;
    pending->op                 = page0 ? PENDING_OP_WRITE_PAGE0 : PENDING_OP_WRITE;
    pending->n_cw               = n_cw;
    pending->cw_bitmap          = h->occupied;
    pending->lba                = h->base;
    pending->buffer             = NULL;
    pending->new_pba            = pba[0];
    pending->new_pba1           = pba[1];
    pending->cache_intermediate = 0U;
    pend_validate(pending);

    /* 提交（tag = HOST_IO ⇒ 完成时由 g_op_callbacks[FTL_OP_HOST_IO] 收尾）。
     * ctx 传 NULL：host I/O 的私有状态在 pending 表里（比旁表 ctx 更全）。 */
    if (ftl_op_submit(cmd, FTL_OP_HOST_IO, NULL) != FTL_OK)
    {
        const WORD cmd_id = ftl_cmd_id(cmd);

        pend_remove(h->pending, cmd_id); /* 先摘 pending */
        ret = FTL_AGAIN;                 /* SQ 满：瞬态（缓冲保留，下次重试） */
        goto wbuf_flush_fail;
    }

    /* 观测口：一页一次编程；base/cw/nplanes/owners 即"合并了多少"的判据。
     * ⚠ LOGD（不是 LOGI）：批量写下每 4KB 一行、UART 阻塞 ~5ms，比 NAND 编程慢一个量级。 */
    LOGD(LOG_TAG_FTL "wbuf flush base=%lu cw=%u nplanes=%u owners=0x%lx\r\n", h->base, (unsigned)n_cw, (unsigned)write_planes, h->owners);

    /* 提交成功才清缓冲 */
    h->open     = false;
    h->occupied = 0U;
    h->owners   = 0U;
    h->base     = 0U;
    h->flushed_pages++;

    return FTL_OK;

/* 统一错误出口（H8 允许的唯一 goto）：释放命令 + **归还页对**。缓冲原样保留 ⇒ 下轮重试；
 * 页对尚未编程（命令从未提交）⇒ 归还是安全的。 */
wbuf_flush_fail:
    if (cmd != NULL)
    {
        (void)fc_command_release(cmd);
    }
    alc_super_open_rewind_pair(h->alloc, FTL_TEMP_HOT);
    return ret;
}

static ftl_status_e wb_append(wb_handle_t *h, WORD gi, DWORD lba, const void *buffer, BYTE n)
{
    const DWORD window = ftl_lba_pair_base(lba);

    /* 换窗口：先把旧窗口编程掉。flush 失败（背压）则本子操作不消费，下轮重试。 */
    if ((h->open) && (h->base != window))
    {
        ftl_status_e st = wb_flush(h);

        if (st != FTL_OK)
        {
            return st;
        }
    }

    if (!h->open)
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

        (void)memcpy(&h->data[dst_offset], src + src_offset, FTL_IU_BYTES);
        h->occupied |= wb_lba_bit(l);
    }
    h->owners |= ftl_group_bit(gi);
    pend_group_add_subop(h->pending, gi);

    /* 满：尝试立即编程（失败则留给 idle flush 重试；append 本身已成功 ⇒ 仍返 OK，
     * 否则调用者会重发同一子操作 ⇒ 重复计数）。 */
    if (h->occupied == FTL_CACHE_FULL_CW)
    {
        (void)wb_flush(h);
    }

    return FTL_OK;
}

/* ── 3. 丢弃 / 复位 ── */

static bool wb_drop(wb_handle_t *h, DWORD lba)
{
    if (!wb_has(h, lba))
    {
        return false;
    }

    h->occupied &= ~wb_lba_bit(lba);
    h->drop_count++;

    if (h->occupied == 0U)
    {
        /* 窗口被清空：本页不再编程，退掉贡献者的 append（它们从未真正写盘）。
         * 每个贡献组对该窗口至多一次 append ⇒ 逐组递减一次即在飞数配平。
         * ⚠ 本动作是 wbuf 的**内部不变量**（occupied 清空 ⟺ owners 必须结算），
         *   收在这里，不由调用者手工维护（CLAUDE.md §3.1）。 */
        pend_group_complete_subop_multi(h->pending, h->owners, SSD_STATUS_OK);
        h->open   = false;
        h->owners = 0U;
        h->base   = 0U;
    }

    return true;
}

static void wb_reset(wb_handle_t *h)
{
    if (h->owners != 0U)
    {
        /* 缓冲里的写被整体丢弃（介质已擦净）⇒ 按**写失败**收尾。不能只清零 owners：
         * 那些组仍在等子操作完成，直接丢会让它们的在飞数永不归零、host 写挂死。 */
        pend_group_complete_subop_multi(h->pending, h->owners, SSD_STATUS_IO_ERROR);
    }

    h->open     = false;
    h->occupied = 0U;
    h->owners   = 0U;
    h->base     = 0U;
}

/* ── 4. 空闲 flush ── */

static void wb_maybe_flush_idle(wb_handle_t *h)
{
    if (!h->open)
    {
        return;
    }

    if (pend_group_has_window_pending(h->pending, h->base))
    {
        return; /* 还有写组会把数据 append 进本窗口 → 先别 flush */
    }

    (void)wb_flush(h);
}

/* ── 5. typed 入口 ── */

void wb_init(wb_handle_t *h, alc_t *alloc, pend_table_t *pending, struct mig_handle *migrate)
{
    /* 前置条件：ftl_init 调本函数前已由 contract_check_handles 证明各容器非空
     * （alloc / pending 为实例内嵌对象的地址，migrate 由同一审计保证）⇒ 不重复检查。 */
    memset(h, 0, sizeof(*h));
    h->alloc   = alloc;
    h->pending = pending;
    h->migrate = migrate;
}

static void wb_status_fill(const wb_handle_t *h, wb_status_t *out)
{
    out->base.critical  = false; /* wbuf 是 host 写路径的一部分，由 Phase 2.5 驱动 */
    out->base.wants_run = h->open || (h->occupied != 0U);
    out->base.progress  = h->flushed_pages;
    out->base.next_ms   = 0U;
    out->base.error     = h->error;

    if (h->error != FTL_OK)
    {
        out->state      = WBUF_STATE_FAILED;
        out->base.state = FTL_MODULE_FAILED;
    }
    else if (h->open)
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

ftl_status_e wb_handle(wb_handle_t *h, const wb_trigger_t *trigger, const bg_info_t *info, wb_status_t *out)
{
    /* info 当前不参与 wbuf 决策；保留形参以统一 typed 入口。 */
    (void)info;

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
            wb_maybe_flush_idle(h);
            break;

        default:
            st = FTL_ERROR;
            break;
    }

    h->error = (st == FTL_ERROR) ? FTL_ERROR : FTL_OK;

    if (out != NULL)
    {
        wb_status_fill(h, out);
    }
    return st;
}

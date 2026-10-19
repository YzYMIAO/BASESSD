/*
 * wbuf.c — 页打包写缓冲（page-packing write buffer）
 *
 * 职责：
 *   把**同一 pair 窗口**内的多次 host 写攒成一页，窗口填满 / 切到别的窗口 / 显式 flush /
 *   空闲时**一次编程**（两 plane 都有数据即一条 multi-plane）⇒ 写指针按页推进，消除
 *   「逐子操作分配整对页」的浪费（背景与不变量见 wbuf.h 文件头）。
 *
 * 文件结构：
 *   1. 槽位位 / 查询    wbuf_lba_bit / ftl_wbuf_has / ftl_wbuf_read（static 辅助）
 *   2. 攒入与编程       wbuf_append / wbuf_flush / wbuf_program_page（static）
 *   3. 丢弃 / 复位      wbuf_drop / wbuf_reset（static）
 *   4. 空闲 flush       wbuf_maybe_flush_idle（static）
 *   5. typed 入口       wbuf_handle（事件分发 + 状态上报）
 *
 * ⚠ 模块边界：本文件不 include ftl.h。alloc / pending / migrate 全部走 `wbuf_port_t`，
 *   由 ftl.c 在 ftl_init 中绑定（docs/design/ftl_module_contracts.md §4.8）。
 */

#include "wbuf.h"

#include "assertion.h"
#include "fc.h"               /* fc_command_alloc / fc_command_release / FC_* 配置常量 */
#include "ftl_fc_interface.h" /* ftl_cmd_* / ftl_plane_* */
#include "ftl_op.h"           /* ftl_op_submit / FTL_OP_HOST_IO */
#include "log.h"

#include <string.h>

/* 该 LBA 在 flat CW 位图（occupied / 命令 cw_bitmap）里的位。
 * ⚠ 走 ftl_cw_bit 而非手写 `1U << ftl_lba_pair_slot(lba)`——两者逐值同构
 *   （slot ≡ plane*CW_NUM + cw），而 ftl_cw_bit 已文档化为**所有** CW 位图的唯一构造器
 *   （见 ftl_config.h）。ftl_lba_pair_slot 仍用于 data[] 的**数组下标**（不是位）。 */
static uint8_t wbuf_lba_bit(lba_t lba)
{
    return (uint8_t)ftl_cw_bit(ftl_lba_plane(lba), ftl_lba_cw(lba));
}

/* ── 1. 查询 ── */

bool ftl_wbuf_has(const wbuf_handle_t *h, lba_t lba)
{
    if (h->open == 0U)
    {
        return false;
    }
    if (ftl_lba_pair_base(lba) != h->base)
    {
        return false;
    }

    return (h->occupied & wbuf_lba_bit(lba)) != 0U;
}

bool ftl_wbuf_read(const wbuf_handle_t *h, lba_t lba, void *out)
{
    ASSERT(out != NULL);

    if (!ftl_wbuf_has(h, lba))
    {
        return false;
    }

    const uint8_t slot = ftl_lba_pair_slot(lba);

    (void)memcpy(out, &h->data[(size_t)slot * FTL_IU_BYTES], FTL_IU_BYTES);
    return true;
}

/* ── 2. 攒入与编程 ── */

static ftl_status_t wbuf_flush(wbuf_handle_t *h);

/* wbuf_program_page — 配置命令头 + 建 desc 链 + 按页内 CW 偏移散射填数据。
 * ⚠ 失败**不**释放 cmd——由 wbuf_flush 的统一错误出口释放（fc_command_release 幂等）。 */
static ftl_status_t wbuf_program_page(wbuf_handle_t   *h,
                                      fc_command_t    *cmd,
                                      const nand_pba_t pba[PLANE_NUM],
                                      uint8_t          nplanes)
{
    cmd->config.operation   = ftl_plane_write_op(nplanes);
    cmd->config.multi_plane = ftl_plane_multi_field(nplanes);
    cmd->config.cw          = CW_NUM - 1U;
    cmd->config.crc         = FC_CRC_ENABLE;
    cmd->config.random      = FC_RANDOM_ENABLE;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    /* 窗口基址：FC 侧 desc->meta = (meta & ~PLANE_CW_MASK) | (plane<<2) | cw
     * ⇒ 逐 CW 铸出该 CW 的扇区 LBA（同 ftl_issue_subop）。 */
    cmd->meta               = h->base;

    /* 建 desc 链（位图 = 本页实际覆盖的 CW；desc/buffer 池空 → 瞬态背压） */
    ftl_status_t ret = ftl_cmd_setup_ppa_pair_cw(cmd, pba, h->occupied);

    if (ret != FTL_OK)
    {
        return ret; /* FTL_AGAIN（池空，瞬态）/ FTL_ERROR 原样上抛 */
    }

    /* 按页内 CW 偏移从整页镜像**散射**取数（稀疏页必须散射——顺序打包会把槽 0 的数据
     * 塞进第一个链槽而非它自己的 CW 偏移）。 */
    ret = ftl_cmd_write_data_cw(cmd, h->data, h->occupied);
    if (ret != FTL_OK)
    {
        return FTL_ERROR;
    }

    return FTL_OK;
}

static ftl_status_t wbuf_flush(wbuf_handle_t *h)
{
    if ((h->open == 0U) || (h->occupied == 0U))
    {
        return FTL_OK; /* 空：nothing to do */
    }

    /* 资源次序：**按有无归还路径排序**——先查 pending 表余量（不占资源），再取**可释放**的
     * 命令槽，最后才取**不可归还**的页对。取不到命令槽时一个资源都还没占用；页对之后的
     * 任何失败都由统一错误出口 allocator_rewind_pair 归还（否则页对凭空蒸发，
     * 表现为容量无声流失，见 CLAUDE.md §3.1）。
     * 单线程 superloop ⇒ 查到这里登记之间没有别的消费者。 */
    const wbuf_port_t *port = &h->port;

    if (port->pending_count(port->pending_ctx) >= FTL_PENDING_MAX)
    {
        return FTL_AGAIN;
    }

    fc_command_t *cmd = fc_command_alloc();

    if (cmd == NULL)
    {
        return FTL_AGAIN;
    }

    /* 分配新页（**逐 plane 独立选块**——Policy B）。Free Pool 耗尽 → **背压**：
     * 登记紧急 GC 后返回 FTL_AGAIN，缓冲原样保留，由 ftl_drain_group 下轮重试。
     *
     * ⚠ 搬移是异步 job：在完成回调/写路径里同步等它会把 superloop 卡死。
     *   这里**只登记**原因，由 must-run 的 migrate_handle 下一轮启动。 */
    nand_pba_t pba[PLANE_NUM];

    if (allocator_alloc_pair(port->alloc, FTL_TEMP_HOT, pba) != FTL_OK)
    {
        (void)fc_command_release(cmd); /* 页对尚未取到 ⇒ 无页可还 */
        if (port->gc_urgent != NULL)
        {
            port->gc_urgent(port->migrate_ctx);
        }
        return FTL_AGAIN;
    }

    /* 本页覆盖的 plane 数（决定 operation 码与 plane 段）与实际 CW 数。
     * nplanes 的值域断言在 ftl_plane_write_op 内（界 = PLANE_NUM，与
     * fc_command_plane_span 同源）——不再在这里复检。 */
    uint8_t nplanes = ftl_cw_bitmap_plane_count(h->occupied);
    uint8_t n_cw    = ftl_cw_bitmap_popcount(h->occupied);

    ASSERT(n_cw > 0U);

    ftl_status_t ret = wbuf_program_page(h, cmd, pba, nplanes);

    if (ret != FTL_OK)
    {
        goto wbuf_flush_fail;
    }

    /* 注册 pending（owners = 贡献本页的组位掩码；lba = 窗口基址；buffer 传 NULL）。
     * 认领槽位后就地逐字段填——字段名即文档（见 ftl_pending_req_t 注释）。 */
    ftl_pending_req_t *pending = port->pending_alloc(port->pending_ctx);

    if (pending == NULL)
    {
        ret = FTL_AGAIN; /* pending 表满：瞬态 */
        goto wbuf_flush_fail;
    }

    pending->command_id         = ftl_cmd_id(cmd);
    pending->owners             = h->owners;
    pending->op                 = (uint8_t)PENDING_OP_WRITE;
    pending->n_cw               = n_cw;
    pending->cw_bitmap          = h->occupied;
    pending->lba                = h->base;
    pending->buffer             = NULL;
    pending->new_pba            = pba[0];
    pending->new_pba1           = pba[1];
    pending->cache_intermediate = 0U;
    if (port->pending_validate != NULL)
    {
        port->pending_validate(pending);
    }

    /* 提交（tag = HOST_IO ⇒ 完成时由 g_op_callbacks[FTL_OP_HOST_IO] 收尾）。
     * ctx 传 NULL：host I/O 的私有状态在 pending 表里（比旁表 ctx 更全）。 */
    if (ftl_op_submit(cmd, FTL_OP_HOST_IO, NULL) != FTL_OK)
    {
        if (port->pending_remove != NULL)
        {
            const uint16_t cmd_id = ftl_cmd_id(cmd);

            port->pending_remove(port->pending_ctx, cmd_id); /* 先摘 pending */
        }
        ret = FTL_AGAIN; /* SQ 满：瞬态（缓冲保留，下次重试） */
        goto wbuf_flush_fail;
    }

    /* 观测口：一页一次编程；base/cw/nplanes/owners 即"合并了多少"的判据。
     * ⚠ LOGD（不是 LOGI）：批量写下每 4KB 一行、UART 阻塞 ~5ms，比 NAND 编程慢一个量级。 */
    LOGD("[FTL] wbuf flush base=%lu cw=%u nplanes=%u owners=0x%lx\r\n",
         h->base,
         (unsigned)n_cw,
         (unsigned)nplanes,
         h->owners);

    /* 提交成功才清缓冲 */
    h->open     = 0U;
    h->occupied = 0U;
    h->owners   = 0U;
    h->base     = 0U;
    h->flushed_pages++;

    return FTL_OK;

/* 统一错误出口（H8 允许的唯一 goto）：释放命令 + **归还页对**。缓冲原样保留 ⇒ 下轮重试；
 * 页对尚未编程（命令从未提交）⇒ 归还是安全的。 */
wbuf_flush_fail:
    (void)fc_command_release(cmd);
    allocator_rewind_pair(port->alloc, FTL_TEMP_HOT);
    return ret;
}

static ftl_status_t wbuf_append(wbuf_handle_t *h, uint16_t gi, lba_t lba, const void *buffer, uint8_t n)
{
    ASSERT(buffer != NULL);
    ASSERT(n > 0U);
    const wbuf_port_t *port = &h->port;

    const lba_t window = ftl_lba_pair_base(lba);

    /* 换窗口：先把旧窗口编程掉。flush 失败（背压）则本子操作不消费，下轮重试。 */
    if ((h->open != 0U) && (h->base != window))
    {
        ftl_status_t st = wbuf_flush(h);

        if (st != FTL_OK)
        {
            return st;
        }
    }

    if (h->open == 0U)
    {
        h->open     = 1U;
        h->base     = window;
        h->occupied = 0U;
        h->owners   = 0U;
    }

    const uint8_t *src = (const uint8_t *)buffer;

    for (uint8_t k = 0U; k < n; k++)
    {
        const lba_t   l          = (lba_t)(lba + (lba_t)k);
        const uint8_t slot       = ftl_lba_pair_slot(l);
        const size_t  dst_offset = (size_t)slot * FTL_IU_BYTES;
        const size_t  src_offset = (size_t)k * FTL_IU_BYTES;

        (void)memcpy(&h->data[dst_offset], src + src_offset, FTL_IU_BYTES);
        h->occupied |= wbuf_lba_bit(l);
    }
    h->owners |= ftl_group_bit(gi);
    port->group_add_subop(port->pending_ctx, gi);

    /* 满：尝试立即编程（失败则留给 idle flush 重试；append 本身已成功 ⇒ 仍返 OK，
     * 否则调用者会重发同一子操作 ⇒ 重复计数）。 */
    if (h->occupied == (uint8_t)((1U << FTL_LBA_PER_PAIR) - 1U))
    {
        (void)wbuf_flush(h);
    }

    return FTL_OK;
}

/* ── 3. 丢弃 / 复位 ── */

static bool wbuf_drop(wbuf_handle_t *h, lba_t lba)
{
    if (!ftl_wbuf_has(h, lba))
    {
        return false;
    }

    h->occupied &= (uint8_t)~wbuf_lba_bit(lba);
    h->drop_count++;

    if (h->occupied == 0U)
    {
        /* 窗口被清空：本页不再编程，退掉贡献者的 append（它们从未真正写盘）。
         * 每个贡献组对该窗口至多一次 append ⇒ 逐组递减一次即在飞数配平。
         * ⚠ 本动作是 wbuf 的**内部不变量**（occupied 清空 ⟺ owners 必须结算），
         *   收在这里，不由调用者手工维护（CLAUDE.md §3.1）。 */
        const wbuf_port_t *port = &h->port;

        port->group_complete_multi(port->pending_ctx, h->owners, SSD_STATUS_OK);
        h->open   = 0U;
        h->owners = 0U;
        h->base   = 0U;
    }

    return true;
}

static void wbuf_reset(wbuf_handle_t *h)
{
    if (h->owners != 0U)
    {
        /* 缓冲里的写被整体丢弃（介质已擦净）⇒ 按**写失败**收尾。不能只清零 owners：
         * 那些组仍在等子操作完成，直接丢会让它们的在飞数永不归零、host 写挂死。 */
        const wbuf_port_t *port = &h->port;

        port->group_complete_multi(port->pending_ctx, h->owners, SSD_STATUS_IO_ERROR);
    }

    h->open     = 0U;
    h->occupied = 0U;
    h->owners   = 0U;
    h->base     = 0U;
}

/* ── 4. 空闲 flush ── */

static void wbuf_maybe_flush_idle(wbuf_handle_t *h)
{
    if (h->open == 0U)
    {
        return;
    }

    const wbuf_port_t *port = &h->port;

    if ((port->group_window_pending != NULL) &&
        port->group_window_pending(port->pending_ctx, h->base))
    {
        return; /* 还有写组会把数据 append 进本窗口 → 先别 flush */
    }

    (void)wbuf_flush(h);
}

/* ── 5. typed 入口 ── */

void wbuf_init(wbuf_handle_t *h, const wbuf_port_t *port)
{
    ASSERT(h != NULL);
    ASSERT(port != NULL);

    memset(h, 0, sizeof(*h));
    h->port = *port;
}

static void wbuf_status_fill(const wbuf_handle_t *h, wbuf_status_t *out)
{
    ASSERT(out != NULL);

    out->base.critical  = false; /* wbuf 是 host 写路径的一部分，由 Phase 2.5 驱动 */
    out->base.wants_run = (h->open != 0U) || (h->occupied != 0U);
    out->base.progress  = h->flushed_pages;
    out->base.next_ms   = 0U;
    out->base.error     = h->error;

    if (h->error != (int32_t)FTL_OK)
    {
        out->state      = WBUF_STATE_FAILED;
        out->base.state = FTL_MODULE_FAILED;
    }
    else if (h->open != 0U)
    {
        out->state      = WBUF_STATE_OPEN;
        out->base.state = FTL_MODULE_RUNNING;
    }
    else
    {
        out->state      = WBUF_STATE_IDLE;
        out->base.state = FTL_MODULE_IDLE;
    }

    out->open          = (h->open != 0U);
    out->occupied      = h->occupied;
    out->owners        = h->owners;
    out->flushed_pages = h->flushed_pages;
    out->drop_count    = h->drop_count;
    out->timed_out     = false;
}

ftl_status_t wbuf_handle(wbuf_handle_t *h, const wbuf_trigger_t *trigger, const ftl_bg_info_t *info, wbuf_status_t *out)
{
    /* info 当前不参与 wbuf 决策；保留形参以统一 typed 入口。 */
    (void)info;

    ASSERT(trigger != NULL);
    ASSERT(out != NULL);

    ftl_status_t st = FTL_OK;

    switch ((wbuf_trigger_kind_t)trigger->kind)
    {
        case WBUF_TRIGGER_APPEND:
            st = wbuf_append(h, trigger->gi, trigger->lba, trigger->data, trigger->n);
            break;

        case WBUF_TRIGGER_FLUSH:
            st = wbuf_flush(h);
            break;

        case WBUF_TRIGGER_DROP:
            (void)wbuf_drop(h, trigger->lba);
            break;

        case WBUF_TRIGGER_RESET:
            wbuf_reset(h);
            break;

        case WBUF_TRIGGER_MAYBE_FLUSH_IDLE:
            wbuf_maybe_flush_idle(h);
            break;

        default:
            st = FTL_ERROR;
            break;
    }

    h->error = (st == FTL_ERROR) ? (int32_t)FTL_ERROR : (int32_t)FTL_OK;

    wbuf_status_fill(h, out);
    return st;
}

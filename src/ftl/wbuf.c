/*
 * wbuf.c — 页打包写缓冲（page-packing write buffer）
 *
 * 职责：
 *   把**同一 pair 窗口**内的多次 host 写攒成一页，窗口填满 / 切到别的窗口 / 显式 flush /
 *   空闲时**一次编程**（两 plane 都有数据即一条 multi-plane）⇒ 写指针按页推进，消除
 *   「逐子操作分配整对页」的浪费（背景与不变量见 wbuf.h 文件头）。
 *
 * 文件结构：
 *   1. 槽位位         wbuf_lba_bit（static）
 *   2. 缓冲读写       ftl_wbuf_has / ftl_wbuf_read / ftl_wbuf_drop
 *   3. 攒入与编程     ftl_wbuf_append / ftl_wbuf_flush（+ static wbuf_program_page）
 *   4. 空闲 flush     ftl_wbuf_maybe_flush_idle
 *   5. 复位           ftl_wbuf_reset
 */

#include "wbuf.h"

#include "allocator.h" /* allocator_alloc_pair / allocator_rewind_pair / FTL_TEMP_HOT */
#include "assertion.h"
#include "fc.h"               /* fc_command_alloc / fc_command_release / FC_* 配置常量 */
#include "ftl.h"              /* ftl_context_t */
#include "ftl_fc_interface.h" /* ftl_cmd_* / ftl_plane_* */
#include "ftl_op.h"           /* ftl_op_submit / FTL_OP_HOST_IO */
#include "log.h"
#include "migrate.h" /* mig_begin_gc */
#include "pending.h" /* pending_* / ftl_group_bit / pending_op_is_write */

#include <string.h>

/* 该 LBA 在 flat CW 位图（wbuf.occupied / 命令 cw_bitmap）里的位。
 * ⚠ 走 ftl_cw_bit 而非手写 `1U << ftl_lba_pair_slot(lba)`——两者逐值同构
 *   （slot ≡ plane*CW_NUM + cw），而 ftl_cw_bit 已文档化为**所有** CW 位图的唯一构造器
 *   （见 ftl_config.h）。ftl_lba_pair_slot 仍用于 data[] 的**数组下标**（不是位）。 */
static uint8_t wbuf_lba_bit(lba_t lba)
{
    return (uint8_t)ftl_cw_bit(ftl_lba_plane(lba), ftl_lba_cw(lba));
}

/* ── 2. 缓冲读写 ── */

bool ftl_wbuf_has(const ftl_context_t *ftl, lba_t lba)
{
    if (ftl->wbuf.open == 0U)
    {
        return false;
    }
    if (ftl_lba_pair_base(lba) != ftl->wbuf.base)
    {
        return false;
    }

    return (ftl->wbuf.occupied & wbuf_lba_bit(lba)) != 0U;
}

bool ftl_wbuf_read(const ftl_context_t *ftl, lba_t lba, void *out)
{
    if (!ftl_wbuf_has(ftl, lba))
    {
        return false;
    }

    const uint8_t slot = ftl_lba_pair_slot(lba);

    (void)memcpy(out, &ftl->wbuf.data[(size_t)slot * FTL_IU_BYTES], FTL_IU_BYTES);
    return true;
}

bool ftl_wbuf_drop(ftl_context_t *ftl, lba_t lba)
{
    if (!ftl_wbuf_has(ftl, lba))
    {
        return false;
    }

    ftl->wbuf.occupied &= (uint8_t)~wbuf_lba_bit(lba);
    if (ftl->wbuf.occupied == 0U)
    {
        /* 窗口被清空：本页不再编程，退掉贡献者的 append（它们从未真正写盘）。
         * 每个贡献组对该窗口至多一次 append ⇒ 逐组递减一次即在飞数配平。
         * ⚠ 本动作是 wbuf 的**内部不变量**（occupied 清空 ⟺ owners 必须结算），
         *   收在这里，不由调用者手工维护（CLAUDE.md §3.1）。 */
        pending_group_complete_subop_multi(ftl, ftl->wbuf.owners, SSD_STATUS_OK);
        ftl->wbuf.open   = 0U;
        ftl->wbuf.owners = 0U;
        ftl->wbuf.base   = 0U;
    }

    return true;
}

/* ── 3. 攒入与编程 ── */

ftl_status_t ftl_wbuf_append(ftl_context_t *ftl, uint16_t gi, lba_t lba, const void *buffer, uint8_t n)
{
    const lba_t window = ftl_lba_pair_base(lba);

    /* 换窗口：先把旧窗口编程掉。flush 失败（背压）则本子操作不消费，下轮重试。 */
    if ((ftl->wbuf.open != 0U) && (ftl->wbuf.base != window))
    {
        ftl_status_t st = ftl_wbuf_flush(ftl);

        if (st != FTL_OK)
        {
            return st;
        }
    }

    if (ftl->wbuf.open == 0U)
    {
        ftl->wbuf.open     = 1U;
        ftl->wbuf.base     = window;
        ftl->wbuf.occupied = 0U;
        ftl->wbuf.owners   = 0U;
    }

    const uint8_t *src = (const uint8_t *)buffer;

    for (uint8_t k = 0U; k < n; k++)
    {
        const lba_t   l    = (lba_t)(lba + (lba_t)k);
        const uint8_t slot = ftl_lba_pair_slot(l);

        (void)memcpy(&ftl->wbuf.data[(size_t)slot * FTL_IU_BYTES],
                     src + ((size_t)k * FTL_IU_BYTES),
                     FTL_IU_BYTES);
        ftl->wbuf.occupied |= wbuf_lba_bit(l);
    }
    ftl->wbuf.owners |= ftl_group_bit(gi);
    pending_group_add_subop(ftl, gi);

    /* 满：尝试立即编程（失败则留给 idle flush 重试；append 本身已成功 ⇒ 仍返 OK，
     * 否则调用者会重发同一子操作 ⇒ 重复计数）。 */
    if (ftl->wbuf.occupied == (uint8_t)((1U << FTL_LBA_PER_PAIR) - 1U))
    {
        (void)ftl_wbuf_flush(ftl);
    }

    return FTL_OK;
}

/* wbuf_program_page — 配置命令头 + 建 desc 链 + 按页内 CW 偏移散射填数据。
 * ⚠ 失败**不**释放 cmd——由 ftl_wbuf_flush 的统一错误出口释放（fc_command_release 幂等）。 */
static ftl_status_t wbuf_program_page(ftl_context_t   *ftl,
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
    cmd->meta               = ftl->wbuf.base;

    /* 建 desc 链（位图 = 本页实际覆盖的 CW；desc/buffer 池空 → 瞬态背压） */
    ftl_status_t ret = ftl_cmd_setup_ppa_pair_cw(cmd, pba, ftl->wbuf.occupied);

    if (ret != FTL_OK)
    {
        return ret; /* FTL_AGAIN（池空，瞬态）/ FTL_ERROR 原样上抛 */
    }

    /* 按页内 CW 偏移从整页镜像**散射**取数（稀疏页必须散射——顺序打包会把槽 0 的数据
     * 塞进第一个链槽而非它自己的 CW 偏移）。 */
    ret = ftl_cmd_write_data_cw(cmd, ftl->wbuf.data, ftl->wbuf.occupied);
    if (ret != FTL_OK)
    {
        return FTL_ERROR;
    }

    return FTL_OK;
}

ftl_status_t ftl_wbuf_flush(ftl_context_t *ftl)
{
    if ((ftl->wbuf.open == 0U) || (ftl->wbuf.occupied == 0U))
    {
        return FTL_OK; /* 空：nothing to do */
    }

    /* 资源次序：**按有无归还路径排序**——先查 pending 表余量（不占资源），再取**可释放**的
     * 命令槽，最后才取**不可归还**的页对。取不到命令槽时一个资源都还没占用；页对之后的
     * 任何失败都由统一错误出口 allocator_rewind_pair 归还（否则页对凭空蒸发，
     * 表现为容量无声流失，见 CLAUDE.md §3.1）。
     * 单线程 superloop ⇒ 查到这里登记之间没有别的消费者。 */
    if (pending_count(ftl) >= (uint16_t)FTL_PENDING_MAX)
    {
        return FTL_AGAIN;
    }

    fc_command_t *cmd = fc_command_alloc();

    if (cmd == NULL)
    {
        return FTL_AGAIN;
    }

    /* 分配新页（**逐 plane 独立选块**——Policy B）。Free Pool 耗尽 → **背压**：
     * 起一个 GC 搬移 job（若未在跑）后返回 FTL_AGAIN，缓冲原样保留，由 ftl_drain_group
     * 下轮重试。
     *
     * ⚠ **搬移是异步 job**：在完成回调/写路径里同步等它会把 superloop 卡死
     *   （一次搬移 = 64 页 × 2 plane 读+写 + 2 次擦）。背压与既有 FTL_AGAIN 机制一致，
     *   也是主流做法（写路径不就地回收，取不到块就挂起主机命令）。
     *   **代价**：池满时写延迟升高（等一个 job 走完）。
     *   **死锁边界**：GC 自身搬移需要 free block ⇒ 靠 **OP 预留池**兜底（OP 池按 plane
     *   预留）；GC job 因池空而中止时（migrate.c 的 alloc 失败分支）不擦 victim、
     *   数据无损，下轮重试。
     *   ⚠ 这里**必须**返回 FTL_AGAIN 而不是 FTL_ERROR：无 victim 可回收只是"暂时没空间"，下轮可能就有
     *   （另一条写完成归还了块），报 IO_ERROR 会让 host 写永久失败。 */
    nand_pba_t pba[PLANE_NUM];

    if (allocator_alloc_pair(&ftl->alloc, FTL_TEMP_HOT, pba) != FTL_OK)
    {
        (void)fc_command_release(cmd); /* 页对尚未取到 ⇒ 无页可还 */
        (void)mig_begin_gc(ftl);
        return FTL_AGAIN;
    }

    /* 本页覆盖的 plane 数（决定 operation 码与 plane 段）与实际 CW 数。
     * nplanes 的值域断言在 ftl_plane_write_op 内（界 = PLANE_NUM，与
     * fc_command_plane_span 同源）——不再在这里复检。 */
    uint8_t nplanes = ftl_cw_bitmap_plane_count(ftl->wbuf.occupied);
    uint8_t n_cw    = ftl_cw_bitmap_popcount(ftl->wbuf.occupied);

    ASSERT(n_cw > 0U);

    ftl_status_t ret = wbuf_program_page(ftl, cmd, pba, nplanes);

    if (ret != FTL_OK)
    {
        goto wbuf_flush_fail;
    }

    /* 注册 pending（owners = 贡献本页的组位掩码；lba = 窗口基址；buffer 传 NULL）。
     * 认领槽位后就地逐字段填——字段名即文档（见 ftl_pending_req_t 注释）。 */
    ftl_pending_req_t *pending = pending_alloc(ftl);

    if (pending == NULL)
    {
        ret = FTL_AGAIN; /* pending 表满：瞬态 */
        goto wbuf_flush_fail;
    }

    pending->command_id         = ftl_cmd_id(cmd);
    pending->owners             = ftl->wbuf.owners;
    pending->op                 = (uint8_t)PENDING_OP_WRITE;
    pending->n_cw               = n_cw;
    pending->cw_bitmap          = ftl->wbuf.occupied;
    pending->lba                = ftl->wbuf.base;
    pending->buffer             = NULL;
    pending->new_pba            = pba[0];
    pending->new_pba1           = pba[1];
    pending->cache_intermediate = 0U;
    pending_validate(pending);

    /* 提交（tag = HOST_IO ⇒ 完成时由 g_op_service[FTL_OP_HOST_IO] 收尾）。
     * ctx 传 NULL：host I/O 的私有状态在 pending 表里（比旁表 ctx 更全）。 */
    if (ftl_op_submit(cmd, FTL_OP_HOST_IO, NULL) != FTL_OK)
    {
        pending_remove(ftl, ftl_cmd_id(cmd)); /* 先摘 pending，再走统一出口 */
        ret = FTL_AGAIN;                      /* SQ 满：瞬态（缓冲保留，下次重试） */
        goto wbuf_flush_fail;
    }

    /* 观测口：一页一次编程；base/cw/nplanes/owners 即"合并了多少"的判据。
     * ⚠ LOGD（不是 LOGI）：批量写下每 4KB 一行、UART 阻塞 ~5ms，比 NAND 编程慢一个量级。
     *   release 档编译掉；debug 档（-DDEBUG）保留，供 CLI `wp` 观察合并。 */
    LOGD("[FTL] wbuf flush base=%lu cw=%u nplanes=%u owners=0x%lx\r\n",
         (unsigned long)ftl->wbuf.base,
         (unsigned)n_cw,
         (unsigned)nplanes,
         (unsigned long)ftl->wbuf.owners);

    /* 提交成功才清缓冲 */
    ftl->wbuf.open     = 0U;
    ftl->wbuf.occupied = 0U;
    ftl->wbuf.owners   = 0U;
    ftl->wbuf.base     = 0U;

    return FTL_OK;

/* 统一错误出口（H8 允许的唯一 goto）：释放命令 + **归还页对**。缓冲原样保留 ⇒ 下轮重试；
 * 页对尚未编程（命令从未提交）⇒ 归还是安全的。 */
wbuf_flush_fail:
    (void)fc_command_release(cmd);
    allocator_rewind_pair(&ftl->alloc, FTL_TEMP_HOT);
    return ret;
}

/* ── 4. 空闲 flush ── */

void ftl_wbuf_maybe_flush_idle(ftl_context_t *ftl)
{
    if (ftl->wbuf.open == 0U)
    {
        return;
    }

    for (uint8_t i = 0U; i < FTL_GROUP_MAX; i++)
    {
        const ftl_io_group_t *g = &ftl->groups[i];

        if ((g->used == 0U) || (!pending_op_is_write(g->op)) || (g->next >= g->total))
        {
            continue;
        }
        if (ftl_lba_pair_base((lba_t)(g->lba + g->next)) == ftl->wbuf.base)
        {
            return; /* 还有写组会把数据 append 进本窗口 → 先别 flush */
        }
    }

    (void)ftl_wbuf_flush(ftl);
}

/* ── 5. 复位 ── */

void ftl_wbuf_reset(ftl_context_t *ftl)
{
    ASSERT(ftl != NULL);

    if (ftl->wbuf.owners != 0U)
    {
        /* 缓冲里的写被整体丢弃（介质已擦净）⇒ 按**写失败**收尾。不能只清零 owners：
         * 那些组仍在等子操作完成，直接丢会让它们的在飞数永不归零、host 写挂死。 */
        pending_group_complete_subop_multi(ftl, ftl->wbuf.owners, SSD_STATUS_IO_ERROR);
    }

    ftl->wbuf.open     = 0U;
    ftl->wbuf.occupied = 0U;
    ftl->wbuf.owners   = 0U;
    ftl->wbuf.base     = 0U;
}

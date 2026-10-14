/*
 * ftl.c — FTL 顶层（初始化 / 三级后台服务主循环 / 子操作下发 / 对外读写入口）
 *
 * 职责：
 *   FTL 的对外门面与主循环——ftl_init 装配各子模块；ftl_service 是**服务主循环**
 *   （NAND 泵 → 完成泵 → 补发 host 子操作 → 三级后台）；ftl_submit_read/write 把
 *   一条 host 请求拆成多个 FC 子操作下发；ftl_flush / ftl_trim 供 HIL 调用。
 *
 * 与子模块的分工：
 *   本文件             —— 编排：调度、拆分、完成聚合、模式切换；不解析介质格式。
 *   pending.c          —— 子操作登记 + 组完成聚合（一条 host 请求 = 一个 group）。
 *   completion_table.c —— completion 路由（精确路由 / 默认 handler）。
 *   其余模块（gc / wl / journal / checkpoint / bbt / wear / l2p_cache）各司其职，
 *   见各自文件头。
 *
 * 服务主循环（ftl_service）四阶段：
 *   Phase 1    fc_service × NAND_SUBMIT_BUDGET —— 推进 NAND
 *   Phase 2    completion_table_drain —— 统一完成泵（路由到 ftl_handle_completion）
 *   Phase 2.5  逐组补发 host 子操作（host IO 优先于后台任务）
 *   Phase 3    三级后台模式：IDLE / NORMAL / BUSY（按未决命令数选档）
 *
 * 后台三级模式（按 inflight 近似空闲度）：
 *   IDLE    inflight == 0                     全力后台（GC 补水 / WL / 巡检 / journal / bbt / wear / CP）
 *   BUSY    pending > MAX/2 或有排队 host 请求 最少后台（让路，否则抢走 drain 要用的 FC buffer）
 *   NORMAL  其余                               按需后台（受节流的 GC / WL / CP）
 *
 * 文件结构：
 *   1. 内部辅助        ftl_now_ms / ftl_handle_completion（static）
 *   2. 三级后台服务    ftl_gc_reclaim_one / ftl_gc_evaluate /
 *                      ftl_service_idle / ftl_service_normal / ftl_service_busy（static）
 *   3. 对外接口        ftl_init / ftl_service / ftl_submit_read / ftl_submit_write
 *   4. 子操作下发      ftl_issue_subop / ftl_drain_group（static）
 *   5. 元数据落盘/TRIM ftl_flush / ftl_trim
 *
 * ⚠ 多实例（F6）：pending / groups / rr_last_scan_ms 均 per-instance；completion 全局
 *   但按命令 ch/ce/lun 路由。main.c 目前只对 g_ftl[0][0][0] 调 ftl_service（结构缺口）。
 */

#include "ftl/ftl.h"

#include "common/assertion.h"
#include "common/log.h"
#include "fc/fc.h"
#include "ftl/bbt.h"
#include "ftl/checkpoint.h"
#include "ftl/completion_table.h"
#include "ftl/ftl_fc_interface.h"
#include "ftl/gc.h" /* gc_select_victim / gc_migrate_sb / gc_erase_and_free（紧急 GC） */
#include "ftl/pending.h"
#include "ftl/power_failure_recovery.h"
#include "ftl/wear.h"
#include "ftl/wear_leveling.h"
#include "nand/onfi_1_0.h"
#include "platform/platform.h"

#include <string.h>

/* ── 内部辅助 ── */

/* 慢死预测巡检节流：≥1s 才扫一轮（read disturb 是慢变量，频繁全扫无谓开销） */
#define RR_SCAN_INTERVAL_MS (1000U)

/* F6：巡检时刻已移入 ftl_context_t（ftl->rr_last_scan_ms），per-instance */

/* 组下发机制（定义在文件后部；ftl_service Phase 2.5 先用到） */
static void ftl_drain_group(ftl_context_t *ftl, uint16_t gi);

/* 页打包写缓冲（§七 方案 A；定义在文件后部）：ftl_service 与 ftl_issue_subop 先用到 */
static ftl_status_t ftl_wbuf_append(ftl_context_t *ftl, uint16_t gi, lba_t internal_lba, const void *buffer, uint8_t n);
static ftl_status_t ftl_wbuf_flush(ftl_context_t *ftl);
static void         ftl_wbuf_maybe_flush_idle(ftl_context_t *ftl);
static int          ftl_wbuf_has(const ftl_context_t *ftl, lba_t internal_lba);
static void         ftl_wbuf_read(const ftl_context_t *ftl, lba_t internal_lba, void *out);

/* --------------------------------------------------------------------
 * ftl_now_ms
 *   功能：当前单调毫秒（经平台抽象 time_us 换算）。
 *   参数：无。
 *   返回：毫秒；平台未注册（time_us 缺失）时返回 0。
 *   实现要点：返回 0 时 elapsed = 0 - start < 阈值 ⇒ T3 定时恒不触发，行为与接线前一致
 *             （不误触发、不破坏启动）。
 * -------------------------------------------------------------------- */
static uint32_t ftl_now_ms(void)
{
    const ssd_platform_ops_t *platform = ssd_platform_get();

    if ((platform == NULL) || (platform->time_us == NULL))
    {
        return 0U;
    }
    return (uint32_t)(platform->time_us() / 1000U);
}

/* --------------------------------------------------------------------
 * ftl_confirm_write
 *   功能：写子操作成功后的**逐 CW** 簿记——Journal + L2P + BST 有效计数/「写过」位。
 *   参数：ftl —— 实例；pending —— 已完成的写子操作记录。
 *   返回：无。
 *   实现要点：
 *     · CW 级映射下 1 LBA = 1 CW：一个子操作覆盖 pending->n_cw 个连续 local LBA，
 *       每个 LBA 各自一条 Journal + 一条 L2P 更新（Journal 流量 ×4，已登记）。
 *     · 目标页按 **LBA 的 plane 位**选：plane0 的 LBA 落 pending->new_pba、plane1 的
 *       落 pending->new_pba1。Policy B 下两 plane 独立选块，且「物理 plane 必须等于
 *       LBA 的 plane 位」是页内 META 能还原出正确 LBA 的前提（ftl_config.h §三）。
 *     · **不存 old_pba**：完成时按 LBA 重查 L2P 得"此刻被替换的映射"。同一 LBA 的两次
 *       写在飞时，先完成的先改 L2P，后完成的重查即得前者；提交时快照会让两条 Journal
 *       都指向最初映射，链断。
 *     · valid_count 单位是 **CW**：新 CW +1、被替换的旧 CW -1。真有效性一律逐 CW
 *       由页内 META + L2P 反查现判（DRAM-less approach ③，无位图）——见 bst.h 文件头。
 * -------------------------------------------------------------------- */
static void ftl_confirm_write(ftl_context_t *ftl, const ftl_pending_req_t *pending)
{
    uint8_t bit;

    ASSERT(ftl != NULL);
    ASSERT(pending != NULL);

    /* 页打包（§七 方案 A）：pending 描述**一整页**——pending->lba = 窗口基址，
     * cw_bitmap 标记本页哪些 CW 有效，逐**置位**确认。逐 CW 的 LBA = base + bit
     * （bit 与 LBA 低 3 位 [plane][cw] 同构：bit = plane*CW_NUM + cw）。 */
    for (bit = 0U; bit < (uint8_t)FTL_LBA_PER_PAIR; bit++)
    {
        const uint8_t    pl   = (uint8_t)(bit / CW_NUM);
        const uint8_t    cw   = (uint8_t)(bit % CW_NUM);
        const lba_t      l    = (lba_t)(pending->lba + (lba_t)bit);
        const nand_pba_t page = (pl == 0U) ? pending->new_pba : pending->new_pba1;
        nand_pba_t       newp;
        nand_pba_t       oldp;

        if ((pending->cw_bitmap & (uint8_t)(1U << bit)) == 0U)
        {
            continue; /* 本槽未写（页打包后一页可只填部分 CW） */
        }

        /* 本页只为本命令覆盖到的 plane 分配了页（未覆盖的 plane 不进本循环体）。
         * 覆盖到的 plane 必有页（allocator_alloc_pair 两 plane 都给）。 */
        ASSERT(page != NAND_PBA_INVALID);
        ASSERT(pba_to_plane(page) == pl);

        newp = pba_with_cw(page, cw);
        oldp = l2p_cache_read(&ftl->cache, l);

        journal_append(&ftl->journal, l, oldp, newp);
        l2p_cache_write(&ftl->cache, l, newp);

        ftl_block_add_valid(&ftl->alloc.blocks[ftl_pba_gblock(newp)]);

        /* 覆盖写：旧 CW 失效。旧页若正是本页（同一 CW 重写）则不动计数——
         * newp == oldp 只在"重写同一物理 CW"时出现，语义上是同一条映射。 */
        if ((oldp != NAND_PBA_INVALID) && (oldp != newp))
        {
            const uint16_t gob = ftl_pba_gblock(oldp);

            /* ⚠ **常驻守卫**（非临时探针，勿删）：L2P 指的旧块在 BST 里 0 有效 ⇒ 下一行
             * 必然撞 `bst.h` 的 `valid_count > 0U` 断言。这是「F2 重建漏认」类缺陷的
             * **唯一故障点**：重建只要静默跳过一个仍被 L2P 指向的块（坏块 / 元数据块判据
             * 过宽 / 页 0 判空），该块就落在所有池之外、valid_count 恒 0，且此后**每次**
             * 覆盖写该 LBA 都会死机——而重启本身看不出任何异常。
             * 打印旧块现场是为了**一次归因**（state 一眼可判）：
             *   BAD(0x01)  = 坏块，重建按判据跳过（数据本已不可读）
             *   FREE(0x08) = 已归还空闲池 ⇒ 块还会被 allocator 复用，**丢数据**
             *   RESERVED   = 元数据日志块 ⇒ 判据过宽（v0.69 已修 plane 掩码）
             *   0x0000     = 重建从未认领（页 0 判空 / 未扫描到）
             * 代价 = 每次覆盖写多一次比较；收益 = 把「重启后写一次就死机」变成一条可读日志。 */
            if (ftl->alloc.blocks[gob].valid_count == 0U)
            {
                (void)LOGE("[FTL] dangling lba=%lu old=%08lx g=%u st=0x%04x sb=%u newg=%u nst=0x%04x\r\n",
                           (unsigned long)l,
                           (unsigned long)oldp,
                           (unsigned)gob,
                           (unsigned)ftl->alloc.blocks[gob].state,
                           (unsigned)ftl->alloc.blocks[gob].sb_id,
                           (unsigned)ftl_pba_gblock(newp),
                           (unsigned)ftl->alloc.blocks[ftl_pba_gblock(newp)].state);
            }

            ftl_block_sub_valid(&ftl->alloc.blocks[gob]);
        }
    }

    checkpoint_journal_watermark(&ftl->journal);
    ftl->total_writes++; /* 计「编程次数」（页打包后一页 = 一次编程），不是 CW 数 */
}

/* --------------------------------------------------------------------
 * ftl_recover_read_subop
 *   功能：读子操作失败后的 Read Recovery——**逐 plane** 恢复并搬回 caller 缓冲。
 *   参数：ftl —— 实例；pending —— 失败的读子操作记录。
 *   返回：无（成败只体现在缓冲内容与坏块标记）。
 *   实现要点：
 *     · 一个子操作可能覆盖**两个 plane 的两张不同物理页**（Policy B 下两 plane 块号
 *       不同），故逐 plane 各恢复一次；目标页由 L2P 反查（读失败不改 L2P，映射仍是
 *       提交时那条）。
 *     · 恢复读是**整页**读、从 CW0 起连续拷出 ⇒ 结果先落 ftl->rr_page_scratch，再按
 *       各 CW 的真实偏移（ftl_lba_cw）搬回 caller 缓冲——子操作可能只覆盖页内部分 CW，
 *       直接让 rr_recover_read 写 caller 缓冲会整段错位。
 *     · 某 plane 恢复失败 ⇒ 该 plane 各 CW 清零 + 标坏块（RAM only，后台 bbt_flush
 *       持久化），交由预测搬移/GC 接手搬走有效页。
 *     · ⚠ 调用者仍一律报 IO_ERROR（沿用原实现语义，见 ftl_handle_completion）。
 * -------------------------------------------------------------------- */
static void ftl_recover_read_subop(ftl_context_t *ftl, const ftl_pending_req_t *pending)
{
    uint8_t p;

    ASSERT(ftl != NULL);
    ASSERT(pending != NULL);

    for (p = 0U; p < PLANE_NUM; p++)
    {
        nand_pba_t page     = NAND_PBA_INVALID;
        lba_t      page_lba = 0U;
        uint8_t    first    = 0U;
        uint8_t    k;
        uint8_t    retry_count = 0U;
        int        recovered;

        /* 本子操作覆盖了 plane p 吗？覆盖则取该 plane 首个 LBA 的映射为目标页 */
        for (k = 0U; k < pending->n_cw; k++)
        {
            const lba_t l = (lba_t)(pending->lba + (lba_t)k);

            if (ftl_lba_plane(l) == p)
            {
                const nand_pba_t cur = l2p_cache_read(&ftl->cache, l);

                if (cur != NAND_PBA_INVALID)
                {
                    page     = pba_page(cur);
                    page_lba = ftl_lba_page(l);
                    first    = k;
                }
                break;
            }
        }
        if (page == NAND_PBA_INVALID)
        {
            continue; /* 本子操作不覆盖 plane p */
        }

        recovered = rr_recover_read(&ftl->rr,
                                    ftl->topo.ch,
                                    ftl->topo.ce,
                                    page,
                                    ftl->rr_page_scratch,
                                    FTL_READ_USER_SIZE,
                                    page_lba,
                                    &retry_count);

        for (k = 0U; k < pending->n_cw; k++)
        {
            const lba_t l = (lba_t)(pending->lba + (lba_t)k);
            uint8_t    *dst;

            if (ftl_lba_plane(l) != p)
            {
                continue;
            }
            dst = (uint8_t *)pending->buffer + ((size_t)k * FTL_IU_BYTES);

            if (recovered == 0)
            {
                /* 恢复成功：按本 CW 在页内的真实偏移从整页镜像搬出 */
                memcpy(dst, ftl->rr_page_scratch + ((size_t)ftl_lba_cw(l) * FTL_IU_BYTES), FTL_IU_BYTES);
            }
            else
            {
                memset(dst, 0, FTL_IU_BYTES);
            }
        }

        if (recovered != 0)
        {
            /* Stage 1（Read Retry）+ Stage 2（邻近页辅助）全失败 → 页面已损坏提升为
             * 块级不可靠，显式标记坏块（RR Stage 3 XOR/RAID 恢复属后续阶段，当前不做）。 */
            (void)retry_count; /* 恢复失败，重试次数无意义 */
            LOGE("[FTL] read unrecoverable lba=%lu gblock=%u\r\n",
                 (unsigned long)(pending->lba + (lba_t)first),
                 (unsigned)ftl_pba_gblock(page));
            (void)bbt_mark_bad(&ftl->alloc, ftl_pba_gblock(page));
        }
    }
}

/* --------------------------------------------------------------------
 * ftl_handle_completion
 *   功能：FC 命令完成回调（completion_table 的默认 handler，ctx = ftl_context_t*）。
 *   参数：completion —— FC 完成状态；ctx —— ftl 上下文（仅作单实例 fallback）。
 *   返回：无。
 *   实现要点：按 completion.module 路由——FTL（用户数据 I/O：查 pending 表 → 读数据 / release）、
 *             METADATA/GC/CHECKPOINT/BBT/default（release）。FTL 模块**按命令自身
 *             ch/ce/lun 解析实例**（不依赖"谁在 drain"）——单线程顺序主循环下"实例 A 的
 *             drain 遇到实例 B 的 completion"是必然事件。
 * -------------------------------------------------------------------- */
static void ftl_handle_completion(const fc_command_status_t *completion,
                                  void                      *ctx)
{
    ftl_context_t *ftl    = (ftl_context_t *)ctx;
    uint16_t       cmd_id = (uint16_t)completion->command_id;
    fc_command_t  *cmd;

    /* command_id 由 NAND 从本命令回填（submit 时铸出）；fc_command_id_to_command
     * 的公共壳已断言 ID 在池内且恒返回非空指针——此处防御不可达，且会静默吞 completion。 */
    cmd = fc_command_id_to_command(cmd_id);
    ASSERT(cmd != NULL);

    switch (completion->module)
    {
        case FC_COMMAND_MODULE_FTL:
            /* 用户数据 I/O 完成 */
            {
                /* F6：路由到命令归属实例（ch/ce/lun 来自命令自身，不依赖"谁在 drain"）。
                 * 单线程顺序主循环下"实例 A 的 drain 遇到实例 B 的 completion"是
                 * 必然事件——必须按 cmd->config 而非 ctx 解析实例。 */
                ftl = completion_table_route((uint8_t)cmd->config.ch,
                                             (uint8_t)cmd->config.ce,
                                             (uint8_t)cmd->config.lun);
                if (ftl == NULL)
                {
                    (void)fc_command_release(cmd);
                    break;
                }

                ftl_pending_req_t *pending = pending_lookup(ftl, cmd_id);

                if (pending != NULL)
                {
                    uint32_t     data_size  = FTL_IU_BYTES * pending->n_cw; /* 子操作字节数 */
                    ssd_status_t sub_status = SSD_STATUS_OK;

                    if ((completion->status & FC_CW_STATUS_NAND_FAIL) == 0U)
                    {
                        /* 判别依据是 pending->op（显式操作类型），不是 buffer——
                         * 写请求的 buffer 是数据源、恒非空，用 buffer 判别会把写全判成读。 */
                        if (pending->op == PENDING_OP_READ)
                        {
                            /* 读成功：拷贝数据到 caller buffer。
                             * desc 链序 == LBA 升序（plane-major + 段内 CW 升序，与
                             * LBA 的 [plane][cw] 布局同序）⇒ 按链序连续拷出即 LBA 序。 */
                            (void)ftl_cmd_read_data(cmd, pending->buffer, data_size);

                            /* Read Recovery：记录本次读的 ECC 纠正 bit 数（desc 链最大值），
                             * 用于慢死预测（连续上升 → WARNING → 主动搬移）。
                             * ECC 历史存于 BST（ftl->alloc.blocks[].rr）。
                             * ⚠ 须在 fc_command_release(cmd) 之前调用——desc 释放后失效。 */
                            {
                                /* cmd 是本 FTL 自己下发的 ⇒ 用它记着的 PBA 反查全局块号
                                 * （FTL 层不直接读 cmd->paa —— 那是 FC/器件域的表示）。 */
                                uint16_t gblock = ftl_pba_gblock(ftl_cmd_pba(cmd));

                                rr_record_ecc(&ftl->alloc.blocks[gblock].rr, fc_get_cmd_ecc_max(cmd));

                                /* read disturb 计数：读成功累加（周期归零由上层维护） */
                                ftl->alloc.blocks[gblock].read_count++;
                                wear_mark_dirty(&ftl->alloc, gblock);
                            }

                            /* 读成功状态由组聚合（sub_status 保持 OK） */
                        }
                        else if (pending->cache_intermediate == 0U)
                        {
                            /* 写成功（非 cache 中间页）：逐 CW 确认 Journal + L2P + 有效计数 */
                            ftl_confirm_write(ftl, pending);
                        }
                        /* cache_intermediate 页：跳过 L2P/Journal 更新，等最终 10h 确认 */
                    }
                    else
                    {
                        /* 同上：按 pending->op 判别，不用 io.buffer */
                        if (pending->op == PENDING_OP_READ)
                        {
                            /* 读失败：逐 plane 尝试 Read Recovery（见该函数文件头）。
                             * ⚠ 读失败一律报 IO_ERROR：沿用原实现——原代码在 if/else
                             *   之后**无条件**写 *status_out = IO_ERROR，覆盖了
                             *   recovered==0 分支的 OK。即"read recovery 成功仍报错"，
                             *   语义存疑、属既有缺陷；本步保持行为等价，未顺手改。 */
                            ftl_recover_read_subop(ftl, pending);
                            sub_status = SSD_STATUS_IO_ERROR;
                        }
                        else
                        {
                            /* 写失败：行业坏块替换流程（ftl_design §3.8）
                             *
                             * ① 关闭当前 Open Super Block（不再写入新数据）→ 进 closed_sb_list
                             *    → GC 优先回收（该 SB 有效 CW 偏低：失败 CW 不计数 + 旧 CW 被覆盖清账）
                             * ② bbt_mark_bad（RAM only，后台 bbt_flush 持久化）——标 plane0 的 block 号。
                             *    Policy B 下两 plane 是独立块，**只退失败的那个 plane**：plane1 块仍
                             *    好，随 Closed SB 进 GC，擦除后回 free pool 复用（不再被同号语义连坐）。
                             *    若失败的是 plane1（本层拿不到逐 CW 状态，只知整命令失败），
                             *    其块会在 GC 擦除失败时被标坏（见 ④）。
                             * ③ L2P 不动 → 自动回滚到旧映射，旧数据完好
                             * ④ GC 回收时：好 plane 擦除回 free pool；坏 plane 擦除失败再标坏
                             */
                            LOGE("[FTL] program failed lba=%lu gblock=%u\r\n",
                                 (unsigned long)pending->lba,
                                 (unsigned)ftl_pba_gblock(pending->new_pba));
                            (void)allocator_close_open_sb(&ftl->alloc,
                                                          (uint8_t)FTL_TEMP_HOT);
                            (void)bbt_mark_bad(&ftl->alloc, ftl_pba_gblock(pending->new_pba));
                            sub_status = SSD_STATUS_IO_ERROR;
                        }
                    }

                    /* 子操作完成 → 组聚合：通知字段（complete_flag/status_out/
                     * on_complete）只在组全部子操作完成后触发一次。
                     * ⚠ 页打包下一页可能由**多个组**拼成 ⇒ 按 owners 位掩码逐组递减。 */
                    pending_remove(ftl, cmd_id);
                    pending_group_complete_subop_multi(ftl, pending->owners, sub_status);
                }

                (void)fc_command_release(cmd);
            }
            break;

        case FC_COMMAND_MODULE_METADATA:
        case FC_COMMAND_MODULE_GC:
        case FC_COMMAND_MODULE_CHECKPOINT:
        case FC_COMMAND_MODULE_BBT:
        default:
            /* 元数据类操作完成后 release。同步等待的 handler 可能已提前释放，
             * 但 fc_command_release 幂等（已在 FREE 直接返回 OK），无需自行判活
             * ——原先那句 `cmd->command_id != FC_COMMAND_ID_INVALID` 恒真：
             * 释放把结构体整体清零，该字段归 0 而不是 0xFFFF。 */
            (void)fc_command_release(cmd);
            break;
    }
}

/* ── 三级后台服务 ── */

/* --------------------------------------------------------------------
 * ftl_gc_reclaim_one
 *   功能：回收一个 victim Super Block（选 → 搬移有效 CW → 擦除归还 Free Pool）。
 *   参数：ftl —— FTL 实例。
 *   返回：1 = 确实回收了一个 SB；0 = 无可用 victim。
 *   实现要点：IDLE / NORMAL 两条后台路径共用同一个回收动作（差别只在"做几个"）。
 *             本函数是 gc_reclaimed_count / gc_novictim_count 的**唯一自增点**——
 *             放在这里（而非两个调用点）⇒ 两条路径的进展统计天然合一。
 * -------------------------------------------------------------------- */
static int ftl_gc_reclaim_one(ftl_context_t *ftl)
{
    super_block_t *victim;

    if (!gc_select_victim(&ftl->alloc, &victim))
    {
        /* 触发但选不出 victim = 卡死嫌疑（观测计数，见 ftl.h）。
         * 生产形态：closed_sb_list 空 / 条目 valid != 0（cf604ba 的缺陷形态）。 */
        ftl->gc_novictim_count++;
        return 0;
    }
    (void)gc_migrate_sb(&ftl->alloc, &ftl->cache, &ftl->journal, victim);
    gc_erase_and_free(&ftl->alloc, victim, ftl->total_writes);
    ftl->gc_reclaimed_count++;
    return 1;
}

/* --------------------------------------------------------------------
 * ftl_gc_evaluate
 *   功能：GC 的**统一触发求值点**——IDLE 与 NORMAL 两条后台路径都从这里过。
 *   参数：ftl —— FTL 实例。
 *   返回：GC_TRIGGER_* 位掩码；0 = 不触发（含节拍未到，此时不打印）。
 *   实现要点：触发只看**空间压力与磨损**（gc_check_triggers；T2 写入量 / T3 定时
 *             已随无出处判定退役，见 gc.h）。命中即打一行观测日志（victim 行由
 *             gc_select_victim 自己打）。
 *   ⚠ **BUSY 不调本函数**（host 饿着，后台让路）；"无块可写"由 ftl_issue_subop 的
 *     紧急 GC 兜底（⚠ 本仓自己的**写路径按需 GC**；SPDK 无此路径——它的 GC 全在
 *     core poller 里、判据是自由 band 水位，写路径取不到块就返 NULL 不就地回收）。
 *   ⚠ **节拍门（GC_EVAL_INTERVAL_MS）**：本函数含 T5 的**全表扫描**
 *     （gc_table_extremes 遍历 FTL_BLOCK_TOTAL 个块），而 Phase 3 每轮 ftl_service
 *     都调它、主循环 `while(1)` 无节拍 ⇒ 加门前是**每轮扫一遍全表**。触发判据本身
 *     是事件（空间压力/磨损），但**成本**远高于主流：SPDK FTL 的判据是 O(1)
 *     （`ftl_needs_reloc` 只读事件驱动的 `dev->num_free` 计数，其 core poller 周期 0
 *     = 每轮求值也无所谓）；本仓 T5 要全表扫描，成本不同 ⇒ 门控是**成本补偿**。
 *     与 rr_last_scan_ms / wear_flush 的节拍门同族。
 *     代价（如实登记）：T1/T4 的空间压力响应也被门到同一周期——IDLE 补池突发与
 *     NORMAL 的「一轮回收一个 SB」最坏延迟 GC_EVAL_INTERVAL_MS。真空闲下按需兜底
 *     仍在写路径（ftl_issue_subop），且 10ms 远小于搬一个 SB 的 NAND 时间。
 * -------------------------------------------------------------------- */
static uint32_t ftl_gc_evaluate(ftl_context_t *ftl)
{
    uint32_t now_ms = ftl_now_ms();
    uint32_t triggers;

    /* 节拍门。⚠ gc_eval_last_ms == 0 是「尚未求值」哨兵：平台时钟缺失时
     * ftl_now_ms() 恒 0，此时**每轮都求值**——GC 不能因缺时钟而停摆。 */
    if ((ftl->gc_eval_last_ms != 0U) && ((now_ms - ftl->gc_eval_last_ms) < GC_EVAL_INTERVAL_MS))
    {
        return 0U;
    }
    ftl->gc_eval_last_ms = now_ms;

    triggers = gc_check_triggers(&ftl->alloc);

    /* 观测口：位图见 gc.h GC_TRIGGER_*（多位可同时命中）。日志走**自己的节拍**
     * （GC_LOG_INTERVAL_MS，与求值同一套时基），不是逐次求值都打。
     * ⚠ 求值节拍不能兼作日志节拍：前者管「多久算一次」，10ms 拍仍是 100 行/秒。
     * 语义 = 进入触发态立即一行 + 触发态内每 GC_LOG_INTERVAL_MS 一行心跳；
     * 退出触发态即归零 ⇒ 下次进入必然立即打（故无需另存触发掩码）。
     * ⚠ 行内带 `reclaimed` / `novictim` 两个**进展计数**（ftl_gc_reclaim_one 自增）：
     * 只有它们能区分「触发但在推进」与「触发但卡死」——`trig` 常亮对两者一视同仁，
     * 而 `[GC] victim` 行（gc.c）在卡死时**根本不出现**，其"消失"也与"已收敛"同形。 */
    if (triggers != 0U)
    {
        if ((ftl->gc_log_last_ms == 0U) || ((now_ms - ftl->gc_log_last_ms) >= GC_LOG_INTERVAL_MS))
        {
            ftl->gc_log_last_ms = now_ms;
            LOGI("[GC] trig=0x%02x free=%u op=%u/%u writes=%lu reclaimed=%lu novictim=%lu\r\n",
                 (unsigned)triggers,
                 (unsigned)ftl->alloc.free_count,
                 (unsigned)ftl->alloc.op_count[0],
                 (unsigned)ftl->alloc.op_count[1],
                 (unsigned long)ftl->total_writes,
                 (unsigned long)ftl->gc_reclaimed_count,
                 (unsigned long)ftl->gc_novictim_count);
        }
    }
    else
    {
        /* 收敛：归零哨兵，使下一次进入触发态的第一行立即出现 */
        ftl->gc_log_last_ms = 0U;
    }

    return triggers;
}

/* --------------------------------------------------------------------
 * ftl_service_idle
 *   功能：IDLE 后台（inflight == 0，系统真空闲）——全力执行 GC/WL/巡检/持久化/CP。
 *   参数：ftl —— FTL 实例。
 *   返回：无。
 * -------------------------------------------------------------------- */
static void ftl_service_idle(ftl_context_t *ftl)
{
    /* flush 所有 dirty cache */
    l2p_cache_flush_all(&ftl->cache);

    /* GC：统一触发判定（与 NORMAL 同一求值点）。命中即**全力**补池到水位①
     * （FREE_POOL_TARGET；三级水位见 allocator.h 文件头）。
     * ⚠ 用「需回收的 SB 数」计数，**不是** `target -= 2U`——后者在 deficit 为奇数时
     *   uint32 下溢成 0xFFFFFFFF，循环只在 gc_select_victim 失败时退出，会把 closed
     *   链上的 SB 全搬全擦（free_count 按 2 递减，本板实测 free=3713 为奇数 ⇒ 必中）。 */
    if (ftl_gc_evaluate(ftl) != 0U)
    {
        uint32_t deficit = (FREE_POOL_TARGET > ftl->alloc.free_count)
                               ? (FREE_POOL_TARGET - ftl->alloc.free_count)
                               : 0U;
        uint32_t budget  = (deficit + (PLANE_NUM - 1U)) / PLANE_NUM;

        /* 仅 T5 命中（池并不缺水）⇒ 不补池，只回收一个 SB */
        if (budget == 0U)
        {
            budget = 1U;
        }
        while ((budget > 0U) && (ftl_gc_reclaim_one(ftl) != 0))
        {
            budget--;
        }
    }

    /* 磨损均衡（IDLE 全力执行） */
    (void)wl_execute(ftl);

    /* 慢死预测巡检（节流 1s；一次只搬一个 SB，防长时间阻塞） */
    {
        uint32_t now_ms = ftl_now_ms();
        if ((now_ms - ftl->rr_last_scan_ms) >= RR_SCAN_INTERVAL_MS)
        {
            ftl->rr_last_scan_ms = now_ms;
            (void)wl_predictive_relocate(ftl);
        }
    }

    /* 强制写 Journal buffer */
    journal_flush_page(&ftl->journal);

    /* BBT 持久化（攒批写 NAND BBT） */
    (void)bbt_flush(&ftl->alloc);

    /* Wear 持久化（攒批写磨损计数；节拍未到则空转） */
    (void)wear_flush(&ftl->alloc, ftl->total_writes);

    /* Checkpoint 检查（T1 Journal 水位 / T2 写入量 / T3 定时 elapsed） */
    {
        uint32_t now_ms  = ftl_now_ms();
        uint32_t elapsed = now_ms - ftl->cp_timer_start;

        if (checkpoint_check_trigger(ftl, elapsed))
        {
            (void)checkpoint_create(ftl);
            ftl->cp_timer_start = now_ms;
        }
    }
}

/* --------------------------------------------------------------------
 * ftl_service_normal
 *   功能：NORMAL 后台（有 IO 但未饱和）——受节流的 GC / WL / 持久化 / CP。
 *   参数：ftl —— FTL 实例。
 *   返回：无。
 * -------------------------------------------------------------------- */
static void ftl_service_normal(ftl_context_t *ftl)
{
    /* 后台 flush：最多 4 条。
     * ⚠ thrashing 降级（P1-4）：工作集远大于 cache 时，这些**主动** flush 大概率是白写
     * （被 flush 的行往往在下次访问前就被驱逐，届时还要再 flush 一次），且挤占前台 IO 的
     * NAND 带宽。检测到 thrashing ⇒ 停主动 flush，脏行只由**驱逐路径按需**写回。
     * 持久性不受影响：L2P 更新已先入 Journal（写完成路径），且 checkpoint_create 会
     * 无条件 flush 全部脏 region（见 checkpoint.c）。 */
    if (!l2p_cache_is_thrashing(&ftl->cache))
    {
        uint16_t flushed = 0U;
        for (uint16_t i = 0U; (i < L2P_CACHE_SIZE) && (flushed < BG_NORMAL_FLUSH_COUNT); i++)
        {
            if (ftl->cache.lines[i].state == L2P_CACHE_DIRTY)
            {
                (void)l2p_cache_flush_one(&ftl->cache, (int)i);
                flushed++;
            }
        }
    }

    /* 检查 GC（**统一求值点**，与 IDLE 同一函数）。命中即回收**一个** SB——
     * NORMAL 是受节流档（有 host IO 在场），不像 IDLE 那样做补池突发。 */
    if (ftl_gc_evaluate(ftl) != 0U)
    {
        (void)ftl_gc_reclaim_one(ftl);
    }

    /* 磨损均衡（NORMAL 模式按需执行） */
    if (wl_should_trigger(ftl))
    {
        (void)wl_execute(ftl);
    }

    /* BBT 持久化（NORMAL 模式周期性刷入） */
    (void)bbt_flush(&ftl->alloc);

    /* Wear 持久化（NORMAL 模式周期性刷入） */
    (void)wear_flush(&ftl->alloc, ftl->total_writes);

    /* Checkpoint 检查（NORMAL 模式也执行） */
    {
        uint32_t now_ms  = ftl_now_ms();
        uint32_t elapsed = now_ms - ftl->cp_timer_start;

        if (checkpoint_check_trigger(ftl, elapsed))
        {
            (void)checkpoint_create(ftl);
            ftl->cp_timer_start = now_ms;
        }
    }
}

/* --------------------------------------------------------------------
 * ftl_service_busy
 *   功能：BUSY 后台（host 饿着）——最少后台，只 flush 1 条防 evict 阻塞 + 紧急 CP。
 *   参数：ftl —— FTL 实例。
 *   返回：无。
 * -------------------------------------------------------------------- */
static void ftl_service_busy(ftl_context_t *ftl)
{
    /* 最少后台：只 flush 1 条防 evict 阻塞 */
    for (uint16_t i = 0U; i < L2P_CACHE_SIZE; i++)
    {
        if (ftl->cache.lines[i].state == L2P_CACHE_DIRTY)
        {
            (void)l2p_cache_flush_one(&ftl->cache, (int)i);
            break;
        }
    }

    /* 紧急 Checkpoint：Journal ≥ 95% 时 BUSY 模式也强制执行 */
    if (checkpoint_is_emergency(ftl))
    {
        (void)checkpoint_create(ftl);
    }
}

/* ── 对外接口 ── */

/* --------------------------------------------------------------------
 * ftl_init
 *   功能：装配一个 FTL 实例——按依赖序初始化路由表 / pending / BST / cache / allocator
 *         / journal / metadata_dir / bbt / wear / 各计时器。
 *   参数：ftl —— 实例（非 NULL，含自持表存储）；topo —— 绑定的 NAND 芯片路由。
 *   返回：FTL_OK / FTL_ERROR（参数非法）。
 *   实现要点：表自持（P2-5，`ftl->tables`）；BST 的 memset 必须早于
 *             wear_init，否则计数回填被清零。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_init(ftl_context_t *ftl, ftl_topology_t topo)
{
    if (ftl == NULL)
    {
        return FTL_ERROR;
    }

    /* ├─ PBA 字段视图自检（一次）：位域布局 implementation-defined，无法用 _Static_assert
     *    证明（见 ftl_config.h §三.2）。若本工具链的布局与 PBA_*_SHIFT 不一致，
     *    强转（*(nand_pba_t *)&fields）会静默算错地址 ⇒ 在此硬失败。 */
    ASSERT(pba_fields_layout_matches() != 0U);

    ftl->topo = topo;

    /* ├─ Completion 路由表（全局单一；F6 实例路由由 main.c boot 后 set_route） ── */
    completion_table_init();

    /* ├─ 注册默认 handler（= ftl_handle_completion）。
     *    F6：handler 按命令 ch/ce/lun 解析实例，ctx 仅作 fallback（单实例兼容）。 ── */
    completion_table_set_default(ftl_handle_completion, ftl);

    /* ├─ Pending Request 表（F6：per-instance） ── */
    pending_init(ftl);

    /* ├─ 页打包写缓冲（§七 方案 A）── */
    memset(&ftl->wbuf, 0, sizeof(ftl->wbuf));

    /* ├─ 慢死巡检时刻（F6：per-instance，原全局 g_rr_last_scan_ms） ── */
    ftl->rr_last_scan_ms = 0U;

    /* ├─ GC 触发求值节拍 + 触发日志节拍（per-instance，见 ftl.h 字段注释） ── */
    ftl->gc_eval_last_ms = 0U;
    ftl->gc_log_last_ms  = 0U;

    /* ├─ GC 进展计数（观测，只增不减；卡死判据见 ftl.h） ── */
    ftl->gc_reclaimed_count = 0U;
    ftl->gc_novictim_count  = 0U;

    /* ├─ Block State Table ├─
     * 表**自持**（P2-5）：条目在 ftl->tables.blocks 内，承载
     * erase_count / valid_count / state / sb_id / rr，read recovery 靠它供电。
     * 无位图表（v0.60 删；有效性逐 CW 由 META + L2P 反查，见 bst.h 文件头）。
     * ⚠ memset 必须早于 wear_init（后者把 NAND 上的计数回填进来，晚了就被清零）。 */
    memset(ftl->tables.blocks, 0, sizeof(ftl->tables.blocks));

    /* ├─ L2P Cache（绑定本 FTL 的 ch/ce/lun，固定位置寻址与路由据此展开）── */
    l2p_cache_init(&ftl->cache, ftl->topo.ch, ftl->topo.ce, ftl->topo.lun);

    /* ├─ Allocator（各指针绑到自持表） ── */
    allocator_init(&ftl->alloc, ftl->topo, &ftl->tables);

    /* ├─ 连接 L2P Cache → Metadata Directory + Allocator ── */
    /* 此时 Metadata Directory 尚未加载，先设 NULL，后续 bbt_init/md_load 后更新 */
    l2p_cache_set_md_dir(&ftl->cache, NULL, &ftl->alloc);

    /* ├─ Journal ── */
    journal_init(&ftl->journal, ftl->topo);

    /* ├─ Metadata Directory ── */
    ftl->metadata_dir.topo = ftl->topo;
    {
        int md_ret = metadata_dir_load(&ftl->metadata_dir);
        if (md_ret != 0)
        {
            metadata_dir_init(&ftl->metadata_dir);
        }
    }

    /* ├─ 重新连接 L2P Cache（此时 md_dir 已就绪）── */
    l2p_cache_set_md_dir(&ftl->cache, &ftl->metadata_dir, &ftl->alloc);

    /* ├─ BBT 初始化（从 NAND 加载或出厂扫描）── */
    (void)bbt_init(&ftl->alloc);

    /* ├─ Wear 表加载（回填 erase/read/program 计数）──
     * 必须在上面 memset(ftl->tables.blocks) 之后、否则回填被清零。 */
    (void)wear_init(&ftl->alloc);

    /* ├─ Checkpoint 池游标/序号 + 定时基准（序 4：per-instance）──
     * 游标在此归零 ⇒ 首次 checkpoint_create 会先扫池定位（cursor_valid==0）。 */
    memset(&ftl->cp, 0, sizeof(ftl->cp));
    ftl->cp_timer_start = 0U;

    /* ├─ 统计 ── */
    ftl->total_writes = 0U;

    return FTL_OK;
}

/* --------------------------------------------------------------------
 * ftl_service_pump
 *   功能：**实例无关**的每轮泵 —— Phase 1（推进 NAND）+ Phase 2（抽干完成表）。
 *   参数：无。
 *   返回：无。
 *   实现要点：这两段不依赖任何实例：NAND 引擎（fc_service）是全局的，完成表
 *             （completion_table）也是全局的、按命令的 ch/ce/lun 路由回实例。
 *             故 superloop 每轮**只调一次**，而不是每个实例调一次
 *             （per-instance 调用会把 fc_service 预算重复消耗 N 遍）。
 * -------------------------------------------------------------------- */
void ftl_service_pump(void)
{
    /* Phase 1: fc_service 推进 NAND。
     * 第 1 次调用即把本轮所有完成抽干（nand_dispatch_poll 全遍历 + sink 逐条消费），
     * 之后几次走填充 pass；预算因此封顶的是"本轮跑几次 fc_service"，不是完成条数。 */
    for (uint8_t i = 0U; i < NAND_SUBMIT_BUDGET; i++)
    {
        if (fc_service() != FC_STATUS_OK)
        {
            break;
        }
    }

    /* Phase 2: 统一完成泵 — 替代原有 fc_poll_completion 循环 */
    completion_table_drain();
}

/* --------------------------------------------------------------------
 * ftl_service
 *   功能：服务**单个** FTL 实例（Phase 2.5 补发子操作 + Phase 3 三级后台）。
 *   参数：ftl —— FTL 实例（NULL 直接返回）。
 *   返回：无。
 *   实现要点：调用方须**先**调 ftl_service_pump()（全局泵，每轮一次），再对**每个**
 *             实例调本函数（序 4：多实例下所有实例都要推进，不能只跑 [0][0][0]）。
 *             Phase 3 的空闲度用 inflight（pending + groups + completion_table 占用）近似，
 *             **不再**用 submitted（旧实现 host 无 IO 时 fc_service 也可能非 0 ⇒ 误判
 *             NORMAL/BUSY）；bg_suspend 置位时整体跳过 Phase 3（`bbt --erase` 后保护）。
 * -------------------------------------------------------------------- */
void ftl_service(ftl_context_t *ftl)
{
    uint16_t inflight;
    uint16_t queued;

    if (ftl == NULL)
    {
        return;
    }

    /* Phase 2.5: 补发 host 子操作。完成先腾出 FC 资源（命令/desc/buffer 池、SQ 空位），
     * 再把各组的待下发子操作补上；顺序保证 host IO 优先于 Phase 3 的后台任务。
     * 只遍历**本实例**的组（每实例一份 groups 表）。 */
    for (uint8_t i = 0U; i < FTL_GROUP_MAX; i++)
    {
        if (ftl->groups[i].used != 0U)
        {
            ftl_drain_group(ftl, (uint16_t)i);
        }
    }

    /* Phase 2.5 尾：页打包写缓冲的**确定性空闲 flush**——补发循环跑完后，若再无写组会
     * 向当前窗口 append，就把缓冲编程掉（否则这些 host 写永不完成）。放在 bg_suspend
     * 判定之前：它是 host IO 的收尾，不是后台任务。 */
    ftl_wbuf_maybe_flush_idle(ftl);

    /* Phase 3: 三级后台模式（B3：空闲度基于未决命令数，非"本轮有无提交 NAND"代理）
     *
     * 旧实现用 submitted（本轮 fc_service 提交了几条）判定，host 无 IO 时
     * fc_service 也可能返回非 0（后台命令推进）→ 误判 NORMAL/BUSY，GC/WL/CP 全推迟。
     * 现在用 pending（host IO 未决）+ groups（未完成的 host 请求）+ completion_table
     * （同步/后台命令未决）占用近似系统空闲度：全空 = 真空闲 → IDLE 全力后台；
     * 有未完成 host 请求 = host 饿着 → BUSY 最少后台（让路，否则抢走 drain 要用的
     * FC buffer）；中间 → NORMAL 按需后台。
     *
     * 近似方案（文档标注）：pending/completion 是"占用快照"，不含队列里排队未 submit
     * 的 host 请求（controller 层不回报其深度）。单实例 LUN_NUM=1 下够用。 */
    queued   = pending_group_count(ftl);
    inflight = (uint16_t)(pending_count(ftl) + queued + completion_table_count());

    /* 后台写闸门：`bbt --erase` 之后介质已空而 RAM 状态陈旧，任何后台写都是往干净
     * 芯片上倒垃圾。Phase 1/2/2.5 已跑完（NAND 泵 + host IO 正常），这里整体跳过
     * Phase 3（checkpoint / bbt / wear / journal / GC 的唯一入口）。 */
    if (ftl->bg_suspend != 0U)
    {
        return;
    }

    if (inflight == 0U)
    {
        ftl_service_idle(ftl);
    }
    else if ((pending_count(ftl) > (FTL_PENDING_MAX / 2U)) || (queued != 0U))
    {
        ftl_service_busy(ftl);
    }
    else
    {
        ftl_service_normal(ftl);
    }
}

/* ── 页打包写缓冲（§七 方案 A）──
 *
 * 同一 pair 窗口内的多次 host 写攒进 ftl->wbuf，窗口填满 / 切到别的窗口 / 显式 flush /
 * 空闲时**一次编程**（两 plane 都有数据即一条 multi-plane）⇒ 写指针按页推进，消除
 * 「逐子操作分配整对页」的浪费（见 ftl_wbuf_t 与 ftl_optimization_plan.md P2-6）。
 *
 * ⚠ 掉电语义（无电容）：缓冲里的数据**不落 journal/L2P**——只有 flush 编程成功的
 *   completion 才确认映射（与既有顺序一致）。掉电时这些写组仍 outstanding（从未完成），
 *   丢失即正确契约；不变量「L2P/journal 只引用已编程页」由构造保证。 */

/* ftl_wbuf_has — 该 local LBA 是否正被写缓冲持有（未编程）。
 * 命中判定：窗口匹配 + 该槽位已置位。供读路径优先取缓冲（避免读到陈旧 L2P）。 */
static int ftl_wbuf_has(const ftl_context_t *ftl, lba_t internal_lba)
{
    const uint8_t slot = (uint8_t)(ftl_lba_plane(internal_lba) * CW_NUM + ftl_lba_cw(internal_lba));

    if (ftl->wbuf.open == 0U)
    {
        return 0;
    }
    if (ftl_lba_pair_base(internal_lba) != ftl->wbuf.base)
    {
        return 0;
    }
    return ((ftl->wbuf.occupied & (uint8_t)(1U << slot)) != 0U) ? 1 : 0;
}

/* ftl_wbuf_read — 从写缓冲读一个 LBA（调用者已用 ftl_wbuf_has 确认命中） */
static void ftl_wbuf_read(const ftl_context_t *ftl, lba_t internal_lba, void *out)
{
    const uint8_t slot = (uint8_t)(ftl_lba_plane(internal_lba) * CW_NUM + ftl_lba_cw(internal_lba));

    (void)memcpy(out, &ftl->wbuf.data[(size_t)slot * FTL_IU_BYTES], FTL_IU_BYTES);
}

/* ftl_wbuf_append — 把窗口内的 n 个 CW 攒进缓冲；换窗口/满时触发编程。
 * 返回 FTL_OK 已攒入 / FTL_AGAIN / FTL_ERROR（换窗口 flush 失败时透传，此时**不消费**
 * 本子操作 ⇒ 调用者 g->next 不前进，下轮重试）。 */
static ftl_status_t ftl_wbuf_append(ftl_context_t *ftl, uint16_t gi, lba_t internal_lba, const void *buffer, uint8_t n)
{
    const lba_t    window = ftl_lba_pair_base(internal_lba);
    const uint8_t *src    = (const uint8_t *)buffer;
    uint8_t        k;

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

    for (k = 0U; k < n; k++)
    {
        const lba_t   l    = (lba_t)(internal_lba + (lba_t)k);
        const uint8_t slot = (uint8_t)(ftl_lba_plane(l) * CW_NUM + ftl_lba_cw(l));

        (void)memcpy(&ftl->wbuf.data[(size_t)slot * FTL_IU_BYTES],
                     src + ((size_t)k * FTL_IU_BYTES),
                     FTL_IU_BYTES);
        ftl->wbuf.occupied |= (uint8_t)(1U << slot);
    }
    ftl->wbuf.owners |= (uint32_t)(1U << gi);
    ftl->groups[gi].outstanding++;

    /* 满：尝试立即编程（失败则留给 idle flush 重试；append 本身已成功 ⇒ 仍返 OK，
     * 否则调用者会重发同一子操作 ⇒ 重复计数）。 */
    if (ftl->wbuf.occupied == (uint8_t)((1U << FTL_LBA_PER_PAIR) - 1U))
    {
        (void)ftl_wbuf_flush(ftl);
    }

    return FTL_OK;
}

/* ftl_wbuf_flush — 把当前缓冲页编程到 NAND（一页 = 一次编程，两 plane 都有数据即 multi-plane）。
 * 返回 FTL_OK 已提交 / FTL_AGAIN 背压（缓冲保留，下次重试）/ FTL_ERROR 无空间。
 * ⚠ 缓冲只在**提交成功后**清空——失败时保留供重试；outstanding 已在 append 记过，此处不动。 */
static ftl_status_t ftl_wbuf_flush(ftl_context_t *ftl)
{
    nand_pba_t    new_pba[PLANE_NUM];
    fc_command_t *cmd;
    uint8_t       nplanes;
    uint8_t       n_cw;
    uint8_t       p;
    int           ret;

    if ((ftl->wbuf.open == 0U) || (ftl->wbuf.occupied == 0U))
    {
        return FTL_OK; /* 空：nothing to do */
    }

    /* 分配新页（**逐 plane 独立选块**——Policy B）。Free Pool 耗尽 → 紧急 GC 重试一次。
     * （原在 ftl_issue_subop 写路径，随页打包一并移到此——每页只分配一次。） */
    if (allocator_alloc_pair(&ftl->alloc, FTL_TEMP_HOT, new_pba) == NAND_PBA_INVALID)
    {
        super_block_t *victim;

        if (gc_select_victim(&ftl->alloc, &victim))
        {
            (void)gc_migrate_sb(&ftl->alloc, &ftl->cache, &ftl->journal, victim);
            gc_erase_and_free(&ftl->alloc, victim, ftl->total_writes);
        }
        if (allocator_alloc_pair(&ftl->alloc, FTL_TEMP_HOT, new_pba) == NAND_PBA_INVALID)
        {
            LOGE("[FTL] write: no free block after GC\r\n");
            return FTL_ERROR; /* 无 victim 可回收 / GC 未归还空间 */
        }
    }

    /* 本页覆盖的 plane 数（决定 operation 码与 plane 段） */
    nplanes = 0U;
    for (p = 0U; p < PLANE_NUM; p++)
    {
        if ((ftl->wbuf.occupied & (uint8_t)(((1U << CW_NUM) - 1U) << ((uint32_t)p * CW_NUM))) != 0U)
        {
            nplanes++;
        }
    }
    ASSERT((nplanes == 1U) || (nplanes == 2U));

    /* 本页实际 CW 数（popcount） */
    n_cw = 0U;
    for (p = 0U; p < (uint8_t)FTL_LBA_PER_PAIR; p++)
    {
        if ((ftl->wbuf.occupied & (uint8_t)(1U << p)) != 0U)
        {
            n_cw++;
        }
    }
    ASSERT(n_cw > 0U);

    /* 分配 FC 命令槽（池空 = 瞬态） */
    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return FTL_AGAIN;
    }

    cmd->config.operation   = (nplanes == 2U) ? OP_PROGRAM_PAGE_MULTI_PLANE : OP_NORMAL_PROGRAM_PAGE;
    cmd->config.multi_plane = (nplanes == 2U) ? 2U : 0U;
    cmd->config.cw          = 3U;
    cmd->config.crc         = FC_CRC_ENABLE;
    cmd->config.random      = FC_RANDOM_ENABLE;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    cmd->module             = FC_COMMAND_MODULE_FTL;
    /* 窗口基址：FC 侧 desc->meta = (meta & ~PLANE_CW_MASK) | (plane<<2) | cw
     * ⇒ 逐 CW 铸出该 CW 的扇区 LBA（同 ftl_issue_subop）。 */
    cmd->meta               = (DWORD)ftl->wbuf.base;

    /* 建 desc 链（位图 = 本页实际覆盖的 CW；desc/buffer 池空 → 瞬态背压） */
    ret = ftl_cmd_setup_ppa_pair_cw(cmd, new_pba, (uint32_t)ftl->wbuf.occupied);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return (ret == FTL_AGAIN) ? FTL_AGAIN : FTL_ERROR;
    }

    /* 按页内 CW 偏移从整页镜像**散射**取数（稀疏页必须散射——顺序打包会把槽 0 的数据
     * 塞进第一个链槽而非它自己的 CW 偏移）。 */
    ret = ftl_cmd_write_data_cw(cmd, ftl->wbuf.data, (uint32_t)ftl->wbuf.occupied);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    /* 注册 pending（owners = 贡献本页的组位掩码；lba = 窗口基址；buffer 传 NULL） */
    ret = pending_add(ftl,
                      ftl_cmd_id(cmd),
                      ftl->wbuf.owners,
                      ftl->wbuf.base,
                      NULL,
                      n_cw,
                      new_pba[0],
                      new_pba[1],
                      0U,
                      PENDING_OP_WRITE,
                      ftl->wbuf.occupied);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_AGAIN; /* pending 表满：瞬态 */
    }

    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        pending_remove(ftl, ftl_cmd_id(cmd));
        (void)fc_command_release(cmd);
        return FTL_AGAIN; /* SQ 满：瞬态（缓冲保留，下次重试） */
    }

    /* 观测口：一页一次编程；base/cw/nplanes/owners 即"合并了多少"的判据 */
    LOGI("[FTL] wbuf flush base=%lu cw=%u nplanes=%u owners=0x%lx\r\n",
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
}

/* ftl_wbuf_maybe_flush_idle — 确定性空闲 flush（ftl_service Phase 2.5 末尾调用）。
 * 若无任何**未下发完的写组**的下一个 LBA 落在当前打开窗口内（不会再有 append 进来），
 * 就把缓冲编程掉。**不引入超时**：串行 host 下组 drain 完当轮即触发；并发组下延迟也有界
 * （该组要么 append 推进、要么被 drain 判失败置 next=total，≤ FTL_DRAIN_STALL_MAX 轮）。 */
static void ftl_wbuf_maybe_flush_idle(ftl_context_t *ftl)
{
    uint8_t i;

    if (ftl->wbuf.open == 0U)
    {
        return;
    }

    for (i = 0U; i < FTL_GROUP_MAX; i++)
    {
        const ftl_io_group_t *g = &ftl->groups[i];

        if ((g->used == 0U) || (g->op != (uint8_t)PENDING_OP_WRITE) || (g->next >= g->total))
        {
            continue;
        }
        if (ftl_lba_pair_base(lba_to_local((lba_t)(g->lba + g->next))) == ftl->wbuf.base)
        {
            return; /* 还有写组会把数据 append 进本窗口 → 先别 flush */
        }
    }

    (void)ftl_wbuf_flush(ftl);
}

/* ── 子操作下发（S2：一条 host 请求 → 多个 FC 子操作）──
 *
 * ftl_issue_subop 下发**一个**子操作 = 一个 **pair 窗口内**的连续 LBA 段
 * （≤ FTL_LBA_PER_PAIR = 8 个 CW = 2 个 plane 页），三态返回：
 *   FTL_OK    已下发（或如未映射读般**同步完成**，此时不进 outstanding）
 *   FTL_AGAIN 瞬态背压（命令/desc/buffer 池满、SQ 满、pending 表满）——排队重试
 *   FTL_ERROR 永久失败（无可用块 / 配置错误 / 映射表损坏）
 *
 * ⚠ 三态分类是全局枢纽：现状把这三类全当致命，正是"大请求在第 N 个子操作崩"的根因。
 *
 * ⚠ n_cw 是**入参兼出参**：调用者给出本窗口内可提供的最大 LBA 数，本函数按
 *   「L2P 可分组性」（读）或「窗口边界」（写）收缩后回填**实际消费的 LBA 数**，
 *   调用者（ftl_drain_group）据此推进进度。
 *
 * ⚠ **一个子操作最多覆盖每个 plane 一个物理页**——这是 FC 建链的硬约束：
 *   fc_build_request_data_planes 断言同 plane 的各 desc 共享 block/page/lun，且
 *   request->plane_paa[plane] 取自该 plane **第一条** desc（release 下只是断言 ⇒
 *   违反会静默用错行地址）。子操作被 pair 窗口（FTL_LBA_PER_PAIR 对齐）夹住 ⇒
 *   连续 LBA 段最多跨两个 plane 段，每段只落在一个页内，约束自动满足。
 */

/* --------------------------------------------------------------------
 * ftl_issue_subop
 *   功能：下发**一个**子操作（pair 窗口内的连续 LBA 段）——查/分配 PBA、建 FC 命令、
 *         注册 pending、submit。
 *   参数：ftl —— 实例；gi —— 组下标；internal_lba —— 片内 local LBA（= 扇区号）；
 *         buffer —— 数据缓冲；n_cw —— 入参兼出参（最大可提供 / 实际消费的 LBA 数）。
 *   返回：三态——FTL_OK 已下发（或如未映射读般**同步完成**，不进 outstanding）；
 *         FTL_AGAIN 瞬态背压（命令/desc/buffer 池满、SQ 满、pending 表满）→ 排队重试；
 *         FTL_ERROR 永久失败（无可用块 / 映射表损坏）。
 *   实现要点：⚠ 三态分类是全局枢纽（见上方分区头）。
 *             · 读：逐 LBA 查 L2P，按 (物理 plane, 页) 分组——未映射、或同 plane 换了页
 *               即收缩；首个 LBA 就未映射则同步零填 1 个 LBA 并消费它。
 *             · 写：按窗口内 LBA 段算 flat 位图（每个 LBA 落自己 plane 位的页 + 自己的
 *               CW），一次 allocator_alloc_pair 拿两个 plane 的页。
 * -------------------------------------------------------------------- */
static ftl_status_t ftl_issue_subop(ftl_context_t *ftl, uint16_t gi, lba_t internal_lba, void *buffer, uint8_t *n_cw)
{
    ftl_io_group_t *g = &ftl->groups[gi];
    fc_command_t   *cmd;
    nand_pba_t      pba[PLANE_NUM];
    uint32_t        cw_bitmap = 0U; /* flat：bit = plane*CW_NUM + cw */
    uint8_t         n;
    uint8_t         nplanes;
    int             ret;

    ASSERT(ftl != NULL);
    ASSERT(buffer != NULL);
    ASSERT(n_cw != NULL);
    ASSERT((*n_cw > 0U) && (*n_cw <= (uint8_t)FTL_LBA_PER_PAIR));

    /* 夹到 pair 窗口：一个子操作不跨窗口（窗口内最多两个 plane 段、每段一个页）。
     * ⚠ L2P region 512 对齐且 lba_to_local 保留低 9 位 ⇒ 窗口绝不跨实例/region。 */
    {
        const lba_t   window_end = ftl_lba_pair_base(internal_lba) + (lba_t)FTL_LBA_PER_PAIR;
        const uint8_t avail      = (uint8_t)(window_end - internal_lba);

        n = (*n_cw < avail) ? *n_cw : avail;
    }
    ASSERT(n > 0U);

    /* 写：攒进页打包缓冲（窗口满 / 切换窗口时内部触发编程），本函数对写**到此为止**。
     * 数据在 ftl_wbuf_flush 里一次编程（§七 方案 A）——分配/建链/提交/确认全在那里。 */
    if (g->op != (uint8_t)PENDING_OP_READ)
    {
        /* 回填窗口夹住后的实际消费数——调用者（ftl_drain_group）据此推进 g->next。
         * ⚠ 不可漏：内部 n 可能被窗口边界夹小（internal_lba 未 8 对齐时 avail<8），
         *   漏回填会让 g->next 越过窗口、跳过未下发的 LBA。 */
        *n_cw = n;
        return ftl_wbuf_append(ftl, gi, internal_lba, buffer, n);
    }

    /* ── 以下为读路径 ── */

    /* 命中写缓冲：该 LBA 尚未编程，从缓冲取（否则会读到陈旧 L2P 映射）。同步完成、
     * 消费 1 个 LBA（不进 outstanding）。 */
    if (ftl_wbuf_has(ftl, internal_lba))
    {
        ftl_wbuf_read(ftl, internal_lba, buffer);
        *n_cw = 1U;
        return FTL_OK;
    }

    pba[0] = NAND_PBA_INVALID;
    pba[1] = NAND_PBA_INVALID;

    {
        /* 逐 LBA 查 L2P 并按 (物理 plane, 页) 分组。收缩条件：
         *   · 该 LBA 仍在写缓冲里（未编程）—— 本 LBA 不进本子操作（下一轮从缓冲取）
         *   · 未映射（INVALID）—— 本 LBA 不进本子操作（下一轮单独处理）
         *   · 同 plane 但换了页 —— 一条命令每 plane 只能一个页，装不下
         *   · 非法 PBA / 物理 plane ≠ LBA 的 plane 位 —— 映射表损坏 → FTL_ERROR */
        uint8_t k;

        for (k = 0U; k < n; k++)
        {
            const lba_t      l = (lba_t)(internal_lba + (lba_t)k);
            const nand_pba_t p = l2p_cache_read(&ftl->cache, l);
            uint8_t          pl;
            nand_pba_t       pg;

            if (ftl_wbuf_has(ftl, l))
            {
                break; /* 还在写缓冲（未编程）：收缩到此为止，下一轮同步从缓冲取 */
            }

            if (p == NAND_PBA_INVALID)
            {
                break; /* 未映射：收缩到此为止 */
            }

            /* 边界不变量（fc.c 的 fc_poll_fill_queue 靠它）：**进 FC 的 PBA 必须合法**。
             * L2P 条目既不是 NAND_PBA_INVALID 又不是合法 PBA ⇒ 映射表已损坏（介质 bit rot、
             * 半编程页被 ECC「纠」过、跨介质格式复用旧页）。**必须在此拦下**：
             * pba_to_ch/ce/lun 会把越界位当路由字段，放行即让 FC 按它直接索引
             * suspended[][][]（栈越界写）；且坏 PBA 还会经 pba_to_paa 掩位后建出一条
             * 读**别的物理页**的 desc 链——静默读到错误数据。
             * 这是**数据**损坏而非编程错误 ⇒ 报错返回（不 ASSERT），由 drain 把本组标
             * IO_ERROR 交给 host，不 brick 设备。 */
            if (pba_is_valid(p) == 0U)
            {
                LOGE("[FTL] rd lba=%lu: L2P entry 0x%08lx is not a valid PBA\r\n",
                     (unsigned long)l,
                     (unsigned long)p);
                return FTL_ERROR;
            }

            pl = (uint8_t)pba_to_plane(p);
            pg = pba_page(p);

            /* CW 级映射的结构不变量：**物理 plane == LBA 的 plane 位**。
             * 违反 ⇒ FC 侧 desc->meta（由**物理** plane/cw 铸出）还原出的扇区 LBA 不是
             * 这一条，读校验必挂 MATA_ERROR；且位图段（按 plane 位算）与 paa 的 plane 位
             * 会分叉 ⇒ 空链。写路径由构造保证（新页从 pair 取），读路径在此拦下。
             * 属映射表损坏（旧介质格式 / bit rot），不是编程错误。 */
            if (pl != ftl_lba_plane(l))
            {
                LOGE("[FTL] rd lba=%lu: plane mismatch pba=0x%08lx\r\n",
                     (unsigned long)l,
                     (unsigned long)p);
                return FTL_ERROR;
            }

            if ((pba[pl] != NAND_PBA_INVALID) && (pba_page(pba[pl]) != pg))
            {
                break; /* 同 plane 跨页：本子操作到此为止，余下交给下一轮 */
            }

            pba[pl] = pg;
            cw_bitmap |= (uint32_t)1U << (((uint32_t)pl * CW_NUM) + (uint32_t)ftl_lba_cw(l));
        }

        if (k == 0U)
        {
            /* 首 LBA 未映射：同步返回全零，消费 1 个 LBA（不进 outstanding）。
             * ⚠ 只覆盖**自己**这一个 LBA：相邻 LBA 的映射与它无关，由 drain 下一轮处理。 */
            memset(buffer, 0, FTL_IU_BYTES);
            *n_cw = 1U;
            return FTL_OK;
        }
        n = k;
    }

    /* 本子操作覆盖的 plane 数（决定 operation 码与 plane 段） */
    {
        uint8_t p;

        nplanes = 0U;
        for (p = 0U; p < PLANE_NUM; p++)
        {
            if ((cw_bitmap & ((uint32_t)((1U << CW_NUM) - 1U) << ((uint32_t)p * CW_NUM))) != 0U)
            {
                nplanes++;
            }
        }
    }
    ASSERT((nplanes == 1U) || (nplanes == 2U));

    /* 分配 FC 命令槽（池空 = 瞬态） */
    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return FTL_AGAIN;
    }

    /* 配置命令头（multi_plane 决定 plane 段，必须早于 ftl_cmd_setup_ppa_pair_cw——
     * 后者按段校验位图） */
    cmd->config.operation   = (nplanes == 2U) ? OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT
                                              : OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
    cmd->config.multi_plane = (nplanes == 2U) ? 2U : 0U;
    cmd->config.cw          = 3U;
    cmd->config.crc         = FC_CRC_ENABLE;
    cmd->config.random      = FC_RANDOM_ENABLE;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    cmd->module             = FC_COMMAND_MODULE_FTL;
    /* 本命令的**窗口基址**（扇区 LBA，低 3 位清零）：FC 侧 desc->meta =
     * (meta & ~PLANE_CW_MASK) | (plane<<2) | cw ⇒ 逐 CW 铸出该 CW 的扇区 LBA。
     * 传窗口基址（而非 internal_lba）语义更直白，两者在 FC 掩掉低 3 位后等价。 */
    cmd->meta               = (DWORD)ftl_lba_pair_base(internal_lba);

    /* 建 desc 链（逐 plane 各自的 PBA，位图 = 本子操作实际覆盖的 CW）；
     * desc/buffer 池空 → 瞬态背压 */
    ret = ftl_cmd_setup_ppa_pair_cw(cmd, pba, cw_bitmap);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return (ret == FTL_AGAIN) ? FTL_AGAIN : FTL_ERROR;
    }

    /* 注册 pending 子操作（必须早于 submit，避免竞态）。读：owners = 单组位、写专用字段传
     * INVALID、cw_bitmap 传 0；lba 传**片内 local**（L2P/Journal 均按 local 索引）。 */
    ret = pending_add(ftl,
                      ftl_cmd_id(cmd),
                      (uint32_t)(1U << gi),
                      internal_lba,
                      buffer,
                      n,
                      NAND_PBA_INVALID,
                      NAND_PBA_INVALID,
                      0U,
                      PENDING_OP_READ,
                      0U);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_AGAIN; /* pending 表满：瞬态 */
    }

    *n_cw = n; /* 回填实际消费的 LBA 数（调用者据此推进） */

    /* 记账（**必须早于 fc_submit**——submit 之后 completion 可能立即被 drain 处理） */
    g->outstanding++;

    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        pending_remove(ftl, ftl_cmd_id(cmd));
        (void)fc_command_release(cmd);
        g->outstanding--;
        return FTL_AGAIN; /* SQ 满：瞬态 */
    }

    return FTL_OK;
}

/* --------------------------------------------------------------------
 * ftl_drain_group
 *   功能：连续下发一个组的待下发子操作，直到全部下发完 / 遇瞬态背压（FTL_AGAIN）/
 *         遇永久失败（FTL_ERROR）。
 *   参数：ftl —— 实例；gi —— 组下标。
 *   返回：无。
 *   实现要点：**进度单位是 LBA 偏移**（g->next / g->total 都以 LBA 计），不是子操作序号：
 *             一个子操作消费 1..FTL_LBA_PER_PAIR（8）个 LBA，取决于 FTL 现场判断
 *             （读路径按 L2P 分组收缩、写路径按 pair 窗口夹住，见 ftl_issue_subop）——
 *             "怎么拆"完全由 FTL 决定，调用者不必预先算出子操作数。每次迭代**重取** g
 *             （下发可能改变组状态甚至释放本组）；全部子操作都在本函数外异步完成，
 *             故循环末尾统一试触发组完成通知——覆盖"整段未映射读"这种从不进 outstanding、
 *             无人触发完成的组。
 * -------------------------------------------------------------------- */
static void ftl_drain_group(ftl_context_t *ftl, uint16_t gi)
{
    ftl_io_group_t *g = &ftl->groups[gi];

    while ((g->used != 0U) && (g->next < g->total))
    {
        const uint32_t off    = g->next; /* 已下发到的 LBA 偏移（g->next 以 LBA 计，非子操作序号） */
        const uint32_t remain = g->count - off;
        lba_t          glba   = (lba_t)(g->lba + off);
        void          *buffer = (uint8_t *)g->buffer + ((size_t)off * FTL_IU_BYTES);
        /* 本窗口内可提供的最大 LBA 数（≤ 一个 pair 窗口）。实际消费几个由
         * ftl_issue_subop 回填——读路径按 L2P 分组收缩，写路径按窗口边界收缩。 */
        uint8_t      n_cw     = (remain >= (uint32_t)FTL_LBA_PER_PAIR) ? (uint8_t)FTL_LBA_PER_PAIR
                                                                       : (uint8_t)remain;
        ftl_status_t st       = ftl_issue_subop(ftl, gi, lba_to_local(glba), buffer, &n_cw);

        g = &ftl->groups[gi]; /* 重取：下发可能改变组状态 */

        if (st == FTL_OK)
        {
            ASSERT((n_cw > 0U) && (n_cw <= (uint8_t)FTL_LBA_PER_PAIR));
            g->stalls = 0U;
            g->next += n_cw;
            continue;
        }

        if (st == FTL_AGAIN)
        {
            /* 瞬态背压：本轮到此为止，下轮 ftl_service Phase 2.5 再补。
             * 连续无进展超预算 → 判真卡死，弃未下发者并报错（已下发的照常完成）。 */
            if (++g->stalls > FTL_DRAIN_STALL_MAX)
            {
                LOGE("[FTL] drain stalled gi=%u next=%lu/%lu\r\n",
                     (unsigned)gi,
                     (unsigned long)g->next,
                     (unsigned long)g->total);
                g->agg_status = SSD_STATUS_IO_ERROR;
                g->next       = g->total;
            }
            break;
        }

        /* FTL_ERROR：永久失败 → 弃未下发者，组最终报错（已下发的照常完成） */
        LOGE("[FTL] subop failed gi=%u next=%lu/%lu\r\n",
             (unsigned)gi,
             (unsigned long)g->next,
             (unsigned long)g->total);
        g->agg_status = SSD_STATUS_IO_ERROR;
        g->next       = g->total;
        break;
    }

    pending_group_maybe_complete(ftl, gi);
}

/* --------------------------------------------------------------------
 * ftl_submit_read
 *   功能：提交一条 host 读请求——建组（一条请求 = 一个组）+ 快路径下发。
 *   参数：ftl —— 实例；req —— 请求（count = 连续 LBA 数，buffer 非空）。
 *   返回：FTL_OK（已建组，可能仍有余量待 Phase 2.5 补发）/ FTL_ERROR（参数非法 /
 *         越界 / 组槽满）。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_submit_read(ftl_context_t *ftl, const ftl_io_request_t *req)
{
    int             gi;
    ftl_io_group_t *g;
    uint32_t        count;

    if ((ftl == NULL) || (req == NULL) || (req->buffer == NULL))
    {
        return FTL_ERROR;
    }

    /* 长度 = 连续 LBA 数（上层 block_count）；FTL 自行决定单/双 plane 拆分 */
    count = req->count;
    if (count == 0U)
    {
        return FTL_ERROR;
    }

    /* LBA 范围检查（含末 LBA） */
    if ((req->lba + count) > FTL_LBA_MAX)
    {
        LOGE("[FTL] read lba out of range lba=%lu count=%lu\r\n",
             (unsigned long)req->lba,
             (unsigned long)count);
        return FTL_ERROR;
    }

    /* 建完成组（一条 host 请求 = 一个组）。通知字段必须**早于任何下发**填入。 */
    gi = pending_group_alloc(ftl);
    if (gi < 0)
    {
        LOGE("[FTL] read: no free group slot\r\n");
        return FTL_ERROR;
    }
    g                  = &ftl->groups[gi];
    g->op              = (uint8_t)PENDING_OP_READ;
    g->total           = count; /* 进度以 **LBA** 计（子操作数由 drain 现场决定，可降级） */
    g->next            = 0U;
    g->outstanding     = 0U;
    g->stalls          = 0U;
    g->lba             = req->lba;
    g->buffer          = req->buffer;
    g->count           = count;
    g->agg_status      = SSD_STATUS_OK;
    g->complete_flag   = req->complete_flag;
    g->status_out      = req->status_out;
    g->on_complete     = req->on_complete;
    g->on_complete_ctx = req->on_complete_ctx;

    /* 快路径：小请求当场下发完；遇背压则余下留待 ftl_service Phase 2.5 补发。
     * 数据在 completion_table_drain 中拷贝回 buffer。 */
    ftl_drain_group(ftl, (uint16_t)gi);

    return FTL_OK;
}

/* --------------------------------------------------------------------
 * ftl_submit_write
 *   功能：提交一条 host 写请求——建组 + 快路径下发；Journal + L2P 在 completion
 *         handler 中确认 NAND PROGRAM 成功后执行。
 *   参数：ftl —— 实例；req —— 请求（count = 连续 LBA 数，buffer 非空）。
 *   返回：FTL_OK（已建组，可能仍有余量待 Phase 2.5 补发）/ FTL_ERROR（参数非法 /
 *         越界 / 组槽满）。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_submit_write(ftl_context_t *ftl, const ftl_io_request_t *req)
{
    int             gi;
    ftl_io_group_t *g;
    uint32_t        count;

    if ((ftl == NULL) || (req == NULL) || (req->buffer == NULL))
    {
        return FTL_ERROR;
    }

    /* 长度 = 连续 LBA 数（上层 block_count）；FTL 自行决定单/双 plane 拆分 */
    count = req->count;
    if (count == 0U)
    {
        return FTL_ERROR;
    }

    /* LBA 范围检查（含末 LBA） */
    if ((req->lba + count) > FTL_LBA_MAX)
    {
        LOGE("[FTL] write lba out of range lba=%lu count=%lu\r\n",
             (unsigned long)req->lba,
             (unsigned long)count);
        return FTL_ERROR;
    }

    /* 建完成组（同 ftl_submit_read：通知字段必须早于任何下发填入）。 */
    gi = pending_group_alloc(ftl);
    if (gi < 0)
    {
        LOGE("[FTL] write: no free group slot\r\n");
        return FTL_ERROR;
    }
    g                  = &ftl->groups[gi];
    g->op              = (uint8_t)PENDING_OP_WRITE;
    g->total           = count; /* 进度以 LBA 计，同 ftl_submit_read */
    g->next            = 0U;
    g->outstanding     = 0U;
    g->stalls          = 0U;
    g->lba             = req->lba;
    g->buffer          = req->buffer;
    g->count           = count;
    g->agg_status      = SSD_STATUS_OK;
    g->complete_flag   = req->complete_flag;
    g->status_out      = req->status_out;
    g->on_complete     = req->on_complete;
    g->on_complete_ctx = req->on_complete_ctx;

    /* 快路径下发；余量留待 Phase 2.5。Journal + L2P 在 completion handler 中
     * 确认 NAND PROGRAM 成功后执行。 */
    ftl_drain_group(ftl, (uint16_t)gi);

    return FTL_OK;
}

/* ── 元数据落盘 / TRIM（供 HIL 调用）── */

/* --------------------------------------------------------------------
 * ftl_flush
 *   功能：把已持久化映射落盘——L2P cache 全部写回 + journal 尾页刷出（供 HIL 调用）。
 *   参数：ftl —— 实例（NULL 直接返回）。
 *   返回：无。
 *   实现要点：**非** NVMe 式 barrier——不等待在飞 / 排队的写（CLI 靠 g_cli_pending.busy
 *             串行化）。
 * -------------------------------------------------------------------- */
void ftl_flush(ftl_context_t *ftl)
{
    if (ftl == NULL)
    {
        return;
    }

    /* 语义 = "把已持久化映射落盘"：先把页打包缓冲里尚未编程的写编程掉（否则它们还没
     * 进 L2P/journal，"落盘"不成立），再 L2P cache 全部写回 + journal 尾页刷出。
     * **非** NVMe 式 barrier（不等待在飞/排队的写；当前 CLI 靠 g_cli_pending.busy 串行化）。
     * ⚠ wbuf flush 遇瞬态背压会保留缓冲、下轮 ftl_service 重试；此处尽力而为。 */
    (void)ftl_wbuf_flush(ftl);

    l2p_cache_flush_all(&ftl->cache);
    journal_flush_page(&ftl->journal);
}

/* --------------------------------------------------------------------
 * ftl_trim
 *   功能：逐 LBA 取消映射（TRIM）——减 CW 有效计数 + 写 L2P INVALID + 记 Journal。
 *   参数：ftl —— 实例；lba —— 起始 LBA；count —— LBA 数（0 视为 1，兼容单 LBA 调用）。
 *   返回：无。
 *   实现要点：CW 级映射下 1 个 LBA = 1 个 CW ⇒ 逐 LBA 减 1 个有效 CW 计数
 *             （valid_count 是**精确**的 CW 计数；v0.60 起无常驻位图，有效性逐 CW
 *             由页内 META + L2P 反查判定——见 bst.h 文件头）。
 * -------------------------------------------------------------------- */
void ftl_trim(ftl_context_t *ftl, uint64_t lba, uint32_t count)
{
    uint32_t i;

    if (ftl == NULL)
    {
        return;
    }
    if (count == 0U)
    {
        count = 1U; /* 兼容"单 LBA"调用 */
    }
    if ((lba + count) > FTL_LBA_MAX)
    {
        return;
    }

    /* 逐 LBA 取消映射（非阻塞、同步完成）。范围上界/分块后续再定。 */
    for (i = 0U; i < count; i++)
    {
        nand_pba_t old_pba;
        lba_t      local_lba = lba_to_local(lba + i);

        /* 该 LBA 还在页打包写缓冲里（未编程）：只清缓冲槽，不碰 L2P/journal——它从未
         * 落盘，无旧映射可失效、也无映射变更需记。 */
        if (ftl_wbuf_has(ftl, local_lba))
        {
            const uint8_t slot = (uint8_t)(ftl_lba_plane(local_lba) * CW_NUM + ftl_lba_cw(local_lba));

            ftl->wbuf.occupied &= (uint8_t) ~(uint8_t)(1U << slot);
            if (ftl->wbuf.occupied == 0U)
            {
                /* 窗口被清空：本页不再编程，退掉贡献者的 append（它们从未真正写盘）。
                 * 每个贡献组对该窗口至多一次 append ⇒ 逐组递减一次即在飞数配平。 */
                pending_group_complete_subop_multi(ftl, ftl->wbuf.owners, SSD_STATUS_OK);
                ftl->wbuf.open   = 0U;
                ftl->wbuf.owners = 0U;
                ftl->wbuf.base   = 0U;
            }
            continue;
        }

        /* 查旧映射：若非 INVALID，同步减该 CW 的有效计数（否则 valid_count 虚高，
         * GC 选 victim 不准）。 */
        old_pba = l2p_cache_read(&ftl->cache, local_lba);
        if (old_pba != NAND_PBA_INVALID)
        {
            uint16_t gblock = (uint16_t)ftl_pba_gblock(old_pba);
            ftl_block_sub_valid(&ftl->alloc.blocks[gblock]);
        }

        l2p_cache_write(&ftl->cache, local_lba, NAND_PBA_INVALID);

        /* ⚠ 取消映射也必须进 Journal（new_pba = INVALID）。L2P 的每一次变更都由
         * Journal 承载，掉电恢复靠回放重建——漏记这一条会让**掉电恢复把已 TRIM
         * 的 LBA 复活**：region 虽被 flush 成 INVALID，但回放按序重放更早的写条目
         * （page_seq 更小但 > cp_sequence）会把它改回有效 PBA，且回放行标 DIRTY
         * 后 ftl_boot_apply_region_bitmap 会跳过预载，NAND 上的 INVALID 也不生效。 */
        journal_append(&ftl->journal, local_lba, old_pba, NAND_PBA_INVALID);
    }
}
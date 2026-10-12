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
 *   2. 三级后台服务    ftl_service_idle / ftl_service_normal / ftl_service_busy（static）
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
                    uint16_t     gi         = pending->group;
                    uint8_t      planes     = (pending->planes > 0U) ? pending->planes : 1U;
                    uint32_t     data_size  = FTL_READ_USER_SIZE * planes;
                    ssd_status_t sub_status = SSD_STATUS_OK;

                    if ((completion->status & FC_CW_STATUS_NAND_FAIL) == 0U)
                    {
                        /* 判别依据是 pending->op（显式操作类型），不是 buffer——
                         * 写请求的 buffer 是数据源、恒非空，用 buffer 判别会把写全判成读。 */
                        if (pending->op == PENDING_OP_READ)
                        {
                            /* 读成功：拷贝数据到 caller buffer */
                            (void)ftl_cmd_read_data(cmd, pending->buffer, data_size);

                            /* Read Recovery：记录本次读的 ECC 纠正 bit 数（desc 链最大值），
                             * 用于慢死预测（连续上升 → WARNING → 主动搬移）。
                             * ECC 历史存于 BST（ftl->alloc.blocks[].rr）。
                             * ⚠ 须在 fc_command_release(cmd) 之前调用——desc 释放后失效。 */
                            {
                                uint16_t gblock = ftl_paa_gblock(cmd->paa);

                                rr_record_ecc(&ftl->alloc.blocks[gblock].rr, fc_get_cmd_ecc_max(cmd));

                                /* read disturb 计数：读成功累加（周期归零由上层维护） */
                                ftl->alloc.blocks[gblock].read_count++;
                                wear_mark_dirty(gblock);
                            }

                            /* 读成功状态由组聚合（sub_status 保持 OK） */
                        }
                        else if (pending->cache_intermediate == 0U)
                        {
                            /* 写成功（非 cache 中间页）：确认 Journal + L2P（modify-after-confirm） */
                            journal_append(&ftl->journal,
                                           pending->lba,
                                           pending->old_pba,
                                           pending->new_pba);
                            l2p_cache_write(&ftl->cache,
                                            pending->lba,
                                            pending->new_pba);
                            checkpoint_journal_watermark(&ftl->journal);
                            ftl->total_writes++;

                            /* VPC/bitmap 维护：新页置位 + 覆盖写清旧页（单点更新防漂移）
                             *
                             * plane0（new_pba 所在 plane）簿记保留。
                             * pages=2 的 Multi-Plane 写：plane1 的实际烧录位置是写入时分配的
                             * pending->new_pba1（Policy B：两 plane 独立选块，**不可**由 plane0
                             * 「同 block + plane=1」派生）。这里对 plane1 页做同样簿记
                             * （set_valid + L2P + Journal），避免 plane1 成为无映射幽灵页
                             * （GC 不搬 → 擦除丢数据）。 */
                            {
                                uint16_t     gnew = (uint16_t)ftl_pba_gblock(pending->new_pba);
                                ftl_block_t *nb   = &ftl->alloc.blocks[gnew];
                                ftl_block_set_valid(nb,
                                                    ftl->alloc.valid_map[gnew],
                                                    (uint8_t)pba_to_plane(pending->new_pba),
                                                    (uint8_t)pba_to_page(pending->new_pba));

                                /* 覆盖写：旧页失效（old_pba 有效时） */
                                if (pending->old_pba != NAND_PBA_INVALID)
                                {
                                    uint16_t     gold = (uint16_t)ftl_pba_gblock(pending->old_pba);
                                    ftl_block_t *ob   = &ftl->alloc.blocks[gold];
                                    ftl_block_clr_valid(ob,
                                                        ftl->alloc.valid_map[gold],
                                                        (uint8_t)pba_to_plane(pending->old_pba),
                                                        (uint8_t)pba_to_page(pending->old_pba));
                                }
                            }

                            /* planes=2 Multi-Plane：plane1 页（lba+1）同样簿记 */
                            if (planes == 2U)
                            {
                                const nand_pba_t pba1 = pending->new_pba1;
                                uint16_t         g1   = (uint16_t)ftl_pba_gblock(pba1);
                                ftl_block_t     *nb1  = &ftl->alloc.blocks[g1];

                                ftl_block_set_valid(nb1,
                                                    ftl->alloc.valid_map[g1],
                                                    (uint8_t)pba_to_plane(pba1),
                                                    (uint8_t)pba_to_page(pba1));

                                /* L2P：lba+1 → plane1 页 */
                                l2p_cache_write(&ftl->cache,
                                                (lba_t)(pending->lba + 1U),
                                                pba1);
                                journal_append(&ftl->journal,
                                               (lba_t)(pending->lba + 1U),
                                               pending->old_pba1,
                                               pba1);

                                /* 覆盖写：plane1 旧页失效 */
                                if (pending->old_pba1 != NAND_PBA_INVALID)
                                {
                                    uint16_t     g1old = (uint16_t)ftl_pba_gblock(pending->old_pba1);
                                    ftl_block_t *ob1   = &ftl->alloc.blocks[g1old];
                                    ftl_block_clr_valid(ob1,
                                                        ftl->alloc.valid_map[g1old],
                                                        (uint8_t)pba_to_plane(pending->old_pba1),
                                                        (uint8_t)pba_to_page(pending->old_pba1));
                                }
                            }

                            /* 写成功状态由组聚合（sub_status 保持 OK） */
                        }
                        /* cache_intermediate 页：跳过 L2P/Journal 更新，等最终 10h 确认 */
                    }
                    else
                    {
                        /* 同上：按 pending->op 判别，不用 io.buffer */
                        if (pending->op == PENDING_OP_READ)
                        {
                            /* 读失败：先尝试 Read Recovery 恢复，恢复成功则数据已就位。
                             * ⚠ rr_recover_read 的形参是 **PBA**（FTL 自己的地址空间，内部
                             *   还要 addr_from_pba）——命令里只有 PAA（v47），此处用拓扑的
                             *   ch/ce 还原，与当初 pba_to_paa() 前的值逐位相同。 */
                            const nand_pba_t read_pba    = paa_to_pba(ftl->topo.ch, ftl->topo.ce, cmd->paa);
                            uint8_t          retry_count = 0U;
                            int              recovered   = rr_recover_read(&ftl->rr,
                                                            ftl->topo.ch,
                                                            ftl->topo.ce,
                                                            read_pba,
                                                            pending->buffer,
                                                            data_size,
                                                            pending->lba,
                                                            &retry_count);
                            if (recovered != 0)
                            {
                                /* 恢复失败：返回全零 + 标记坏块
                                 *
                                 * Stage 1（Read Retry）+ Stage 2（邻近页辅助）全失败 →
                                 * 页面已损坏提升为块级不可靠，显式标记坏块（RAM only，
                                 * 后台 bbt_flush 持久化），交由预测搬移/GC 接手搬走有效页。
                                 * （RR Stage 3 XOR/RAID 恢复属后续阶段，当前不做。） */
                                memset(pending->buffer, 0, data_size);
                                (void)retry_count; /* 恢复失败，重试次数无意义 */
                                LOGE("[FTL] read unrecoverable lba=%lu gblock=%u\r\n",
                                     (unsigned long)pending->lba,
                                     (unsigned)ftl_paa_gblock(cmd->paa));
                                (void)bbt_mark_bad(&ftl->alloc, ftl_paa_gblock(cmd->paa));
                            }
                            /* ⚠ 读失败一律报 IO_ERROR：沿用原实现——原代码在 if/else
                             *   之后**无条件**写 *status_out = IO_ERROR，覆盖了
                             *   recovered==0 分支的 OK。即"read recovery 成功仍报错"，
                             *   语义存疑、属既有缺陷；本步保持行为等价，未顺手改。 */
                            sub_status = SSD_STATUS_IO_ERROR;
                        }
                        else
                        {
                            /* 写失败：行业坏块替换流程（ftl_design §3.8）
                             *
                             * ① 关闭当前 Open Super Block（不再写入新数据）→ 进 closed_sb_list
                             *    → GC 优先回收（该 SB VPC 偏低：失败页不置 valid + 旧页被覆盖清位）
                             * ② bbt_mark_bad（RAM only，后台 bbt_flush 持久化）——标 plane0 的 block 号。
                             *    Policy B 下两 plane 是独立块，**只退失败的那个 plane**：plane1 块仍
                             *    好，随 Closed SB 进 GC，擦除后回 free pool 复用（不再被同号语义连坐）。
                             *    若失败的是 plane1（本层拿不到逐 CW 状态，只知整命令失败），
                             *    其块会在 GC 擦除失败时被标坏（见 ④）。
                             * ③ L2P 不动 → 自动回滚到 old_pba，旧数据完好
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
                     * on_complete）只在组全部子操作完成后触发一次。 */
                    pending_remove(ftl, cmd_id);
                    pending_group_complete_subop(ftl, gi, sub_status);
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
 * ftl_service_idle
 *   功能：IDLE 后台（inflight == 0，系统真空闲）——全力执行 GC/WL/巡检/持久化/CP。
 *   参数：ftl —— FTL 实例。
 *   返回：无。
 * -------------------------------------------------------------------- */
static void ftl_service_idle(ftl_context_t *ftl)
{
    /* flush 所有 dirty cache */
    l2p_cache_flush_all(&ftl->cache);

    /* 补 Free Pool 到水位①（FREE_POOL_TARGET；三级水位见 allocator.h 文件头） */
    if (ftl->alloc.free_count < FREE_POOL_TARGET)
    {
        uint32_t       target = FREE_POOL_TARGET - ftl->alloc.free_count;
        super_block_t *victim;

        while (target > 0U)
        {
            if (!gc_select_victim(&ftl->alloc, &victim))
            {
                break;
            }
            (void)gc_migrate_sb(&ftl->alloc, &ftl->cache, &ftl->journal, victim);
            gc_erase_and_free(&ftl->alloc, victim, ftl->total_writes);
            target -= 2U;
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

    /* 检查 GC（T1 水位 / T2 写入量 / T3 定时 / T4 紧急 / T5 磨损比） */
    {
        uint32_t now_ms   = ftl_now_ms();
        uint32_t elapsed  = now_ms - ftl->gc_timer_start;
        uint32_t triggers = gc_check_triggers(&ftl->alloc, ftl->total_writes, elapsed);

        if (triggers != 0U)
        {
            super_block_t *victim;
            if (gc_select_victim(&ftl->alloc, &victim))
            {
                (void)gc_migrate_sb(&ftl->alloc, &ftl->cache, &ftl->journal, victim);
                gc_erase_and_free(&ftl->alloc, victim, ftl->total_writes);
            }
            /* 观测口：trig 位图见 gc.h GC_TRIGGER_*（多位可同时命中） */
            LOGI("[GC] trig=0x%02x free=%u op=%u writes=%lu\r\n",
                 (unsigned)triggers,
                 (unsigned)ftl->alloc.free_count,
                 (unsigned)ftl->alloc.op_count,
                 (unsigned long)ftl->total_writes);
            ftl->gc_timer_start = now_ms;
        }
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
 *   参数：ftl —— 实例（非 NULL）；params —— 初始化参数（拓扑 + blocks/valid_map 缓冲）。
 *   返回：FTL_OK / FTL_ERROR（参数非法 / blocks 或 valid_map 未提供）。
 *   实现要点：BST 与 valid_map 由调用者提供缓冲（大数组落 AXI/SRAM2）；memset 必须早于
 *             wear_init，否则计数回填被清零。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_init(ftl_context_t *ftl, const ftl_init_params_t *params)
{
    if ((ftl == NULL) || (params == NULL))
    {
        return FTL_ERROR;
    }

    ftl->topo = params->topo;

    /* ├─ Completion 路由表（全局单一；F6 实例路由由 main.c boot 后 set_route） ── */
    completion_table_init();

    /* ├─ 注册默认 handler（= ftl_handle_completion）。
     *    F6：handler 按命令 ch/ce/lun 解析实例，ctx 仅作 fallback（单实例兼容）。 ── */
    completion_table_set_default(ftl_handle_completion, ftl);

    /* ├─ Pending Request 表（F6：per-instance） ── */
    pending_init(ftl);

    /* ├─ 慢死巡检时刻（F6：per-instance，原全局 g_rr_last_scan_ms） ── */
    ftl->rr_last_scan_ms = 0U;

    /* ├─ Block State Table ├─
     * per-Block 状态数组由调用者提供（params->blocks），承载
     * erase_count / valid_count / state / sb_id / rr，read recovery 靠它供电。
     * 逐页有效位图是**独立表**（params->valid_map，主流弹性项，见 bst.h 文件头）。
     * 两者都由调用者提供，任一未提供则初始化失败。 */
    if ((params->blocks == NULL) || (params->valid_map == NULL))
    {
        LOGE("[FTL] init failed: params->blocks / valid_map is NULL\r\n");
        return FTL_ERROR;
    }
    memset(params->blocks, 0, sizeof(ftl_block_t) * FTL_BLOCK_TOTAL);
    memset(params->valid_map, 0, sizeof(uint64_t) * PLANE_NUM * FTL_BLOCK_TOTAL);

    /* ├─ L2P Cache（绑定本 FTL 的 ch/ce/lun，固定位置寻址与路由据此展开）── */
    l2p_cache_init(&ftl->cache, ftl->topo.ch, ftl->topo.ce, ftl->topo.lun);

    /* ├─ Allocator ── */
    allocator_init(&ftl->alloc, params);

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
     * 必须在上面 memset(params->blocks) 之后、否则回填被清零。 */
    (void)wear_init(&ftl->alloc);

    /* ├─ Checkpoint 计时 ── */
    ftl->cp_timer_start = 0U;
    ftl->cp_sequence    = 0U;

    /* ├─ GC 计时 ── */
    ftl->gc_last_run_writes = 0U;
    ftl->gc_timer_start     = 0U;

    /* ├─ 统计 ── */
    ftl->total_writes = 0U;

    return FTL_OK;
}

/* --------------------------------------------------------------------
 * ftl_service
 *   功能：服务主循环（每轮推进一次）——见文件头「服务主循环」四阶段。
 *   参数：ftl —— FTL 实例（NULL 直接返回）。
 *   返回：无。
 *   实现要点：Phase 3 的空闲度用 inflight（pending + groups + completion_table 占用）近似，
 *             **不再**用 submitted（旧实现 host 无 IO 时 fc_service 也可能非 0 ⇒ 误判
 *             NORMAL/BUSY）；bg_suspend 置位时整体跳过 Phase 3（`bbt --erase` 后保护）。
 * -------------------------------------------------------------------- */
void ftl_service(ftl_context_t *ftl)
{
    uint32_t submitted = 0U;
    uint16_t inflight;
    uint16_t queued;

    if (ftl == NULL)
    {
        return;
    }

    /* Phase 1: fc_service 推进 NAND。
     * 第 1 次调用即把本轮所有完成抽干（nand_dispatch_poll 全遍历 + sink 逐条消费），
     * 之后几次走填充 pass；预算因此封顶的是"本轮跑几次 fc_service"，不是完成条数。 */
    for (uint8_t i = 0U; i < NAND_SUBMIT_BUDGET; i++)
    {
        if (fc_service() != FC_STATUS_OK)
        {
            break;
        }
        submitted++;
    }

    /* Phase 2: 统一完成泵 — 替代原有 fc_poll_completion 循环 */
    completion_table_drain();

    /* Phase 2.5: 补发 host 子操作。完成先腾出 FC 资源（命令/desc/buffer 池、SQ 空位），
     * 再把各组的待下发子操作补上；顺序保证 host IO 优先于 Phase 3 的后台任务。
     * ⚠ 仅遍历本实例的组；main.c 目前只对 g_ftl[0][0][0] 调 ftl_service（维度扩展时须
     *   遍历全部实例——既有结构缺口，本步使其显形）。 */
    for (uint8_t i = 0U; i < FTL_GROUP_MAX; i++)
    {
        if (ftl->groups[i].used != 0U)
        {
            ftl_drain_group(ftl, (uint16_t)i);
        }
    }

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

/* ── 子操作下发（S2：一条 host 请求 → 多个 FC 子操作）──
 *
 * ftl_issue_subop 下发**一个**子操作（1 或 2 页），三态返回：
 *   FTL_OK    已下发（或如未映射读般**同步完成**，此时不进 outstanding）
 *   FTL_AGAIN 瞬态背压（命令/desc/buffer 池满、SQ 满、pending 表满）——排队重试
 *   FTL_ERROR 永久失败（无可用块 / 配置错误 / 映射表损坏）
 *
 * ⚠ 三态分类是全局枢纽：现状把这三类全当致命，正是"大请求在第 N 个子操作崩"的根因。
 *
 * ⚠ planes 是**入参兼出参**：读路径查出相邻两 LBA 落在**同一个 plane** 时会把它
 *   降级为 1（见下），调用者（ftl_drain_group）据此只推进 1 个 LBA。
 */

/* --------------------------------------------------------------------
 * ftl_issue_subop
 *   功能：下发**一个**子操作（1 或 2 页）——查/分配 PBA、建 FC 命令、注册 pending、submit。
 *   参数：ftl —— 实例；gi —— 组下标；internal_lba —— 片内 local LBA；
 *         buffer —— 数据缓冲；planes —— 入参兼出参（读路径可能降级为 1）。
 *   返回：三态——FTL_OK 已下发（或如未映射读般**同步完成**，不进 outstanding）；
 *         FTL_AGAIN 瞬态背压（命令/desc/buffer 池满、SQ 满、pending 表满）→ 排队重试；
 *         FTL_ERROR 永久失败（无可用块 / 映射表损坏）。
 *   实现要点：⚠ 三态分类是全局枢纽（见上方分区头）。读路径相邻两 LBA 落在**同一
 *             plane** 时降级为单 plane（GC/WL 搬移会把有效页一律落到 plane 0）。
 * -------------------------------------------------------------------- */
static ftl_status_t ftl_issue_subop(ftl_context_t *ftl, uint16_t gi, lba_t internal_lba, void *buffer, uint8_t *planes)
{
    ftl_io_group_t *g = &ftl->groups[gi];
    fc_command_t   *cmd;
    nand_pba_t      pba[PLANE_NUM];
    nand_pba_t      new_pba[PLANE_NUM];
    nand_pba_t      old_pba  = NAND_PBA_INVALID;
    nand_pba_t      old_pba1 = NAND_PBA_INVALID;
    uint8_t         nplanes  = *planes; /* 本子操作实际页数（可能被读路径降级为 1） */
    uint32_t        data_size;
    int             ret;

    ASSERT((nplanes == 1U) || (nplanes == 2U));

    if (g->op == (uint8_t)PENDING_OP_READ)
    {
        /* 查 L2P：两 plane 物理位置**各自独立**（Policy B：SB 两 plane 不再同号）。
         * 未映射 → 本 LBA 按全零**同步完成**（不进 outstanding）。
         * ⚠ 只覆盖**自己**这一个 LBA：相邻 LBA 的映射与它无关（GC/WL 会让相邻两页
         *   落在不同 plane、不同生命周期），余下 LBA 由 drain 的下一轮处理。 */
        pba[0] = l2p_cache_read(&ftl->cache, internal_lba);
        if (pba[0] == FTL_PBA_INVALID)
        {
            nplanes = 1U;
            *planes = 1U;
            memset(buffer, 0, FTL_READ_USER_SIZE);
            return FTL_OK;
        }

        /* 边界不变量（fc.c 的 fc_poll_fill_queue 靠它）：**进 FC 的 PBA 必须合法**。
         * L2P 条目既不是 FTL_PBA_INVALID 又不是合法 PBA ⇒ 映射表已损坏（介质 bit rot、
         * 半编程页被 ECC「纠」过、跨介质格式复用旧页）。**必须在此拦下**：
         * pba_to_ch/ce/lun 会把越界位当路由字段，放行即让 FC 按它直接索引
         * suspended[][][]（栈越界写）；且坏 PBA 还会经 pba_to_paa 掩位后建出一条
         * 读**别的物理页**的 desc 链——静默读到错误数据。
         * 这是**数据**损坏而非编程错误 ⇒ 报错返回（不 ASSERT），由 drain 把本组标
         * IO_ERROR 交给 host，不 brick 设备。 */
        if (pba_is_valid(pba[0]) == 0U)
        {
            LOGE("[FTL] rd lba=%lu: L2P entry 0x%08lx is not a valid PBA\r\n",
                 (unsigned long)internal_lba,
                 (unsigned long)pba[0]);
            return FTL_ERROR;
        }

        if (nplanes == 2U)
        {
            pba[1] = l2p_cache_read(&ftl->cache, (lba_t)(internal_lba + 1U));

            /* 未映射（INVALID）**先判**：它走降级路径（既有语义），不是损坏 */
            if (pba[1] == FTL_PBA_INVALID)
            {
                nplanes = 1U;
                *planes = 1U;
            }
            else if (pba_is_valid(pba[1]) == 0U)
            {
                /* 同 pba[0] 的判据：不是 INVALID 又不是合法 PBA = 映射表损坏 */
                LOGE("[FTL] rd lba=%lu: L2P entry 0x%08lx is not a valid PBA\r\n",
                     (unsigned long)(internal_lba + 1U),
                     (unsigned long)pba[1]);
                return FTL_ERROR;
            }
            /* 降级判定：multi-plane 命令要求两个 LBA 落在**不同** plane —— 但"LBA 相邻"
             * 只说明它们当初是**一起写**的（写路径 allocator_alloc_pair 给 plane 0/1），
             * 不保证现在还在不同 plane：GC/WL 搬移把有效页一律落到 plane 0
             * （allocator_alloc 只返回 pair[0]，见 allocator.c），原本 plane1 的奇数 LBA
             * 因此可能与偶数 LBA 同 plane。此时合成一条 multi-plane 命令会把同一个
             * (plane,cw) 挂两次 = 链构造错误（fc_interface.c 的 ASSERT 就是拦这个）。
             * 降级为单 plane：只读本 LBA，下一个 LBA 留给 drain 的下一轮。
             * 配对是**每次操作的临时判断**，不是持久属性——这正是"访问哪些物理单元由
             * FTL 自己决定"的含义（fc_nand_design §26.2）。 */
            else if (pba_to_plane(pba[0]) == pba_to_plane(pba[1]))
            {
                nplanes = 1U;
                *planes = 1U;
            }
        }
    }
    else
    {
        /* 分配新 PBA（**逐 plane 独立选块**——Policy B）。Free Pool 耗尽 → 同步跑一轮
         * 紧急 GC（回收 1 个 Super Block），归还后再重试一次。真无空间才永久失败。 */
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

        /* 读旧 PBA（journal 记录用）。planes=2 时 plane1 页也要读旧映射。 */
        old_pba  = l2p_cache_read(&ftl->cache, internal_lba);
        old_pba1 = (nplanes == 2U) ? l2p_cache_read(&ftl->cache, (lba_t)(internal_lba + 1U))
                                   : NAND_PBA_INVALID;
        pba[0]   = new_pba[0];
        pba[1]   = new_pba[1];
    }

    /* 本子操作实际字节数（读路径可能已降级为单页） */
    data_size = FTL_READ_USER_SIZE * nplanes;

    /* 分配 FC 命令槽（池空 = 瞬态） */
    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return FTL_AGAIN;
    }

    /* 配置命令头（按 op + planes 选 operation 码） */
    if (g->op == (uint8_t)PENDING_OP_READ)
    {
        if (nplanes == 1U)
        {
            cmd->config.operation   = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
            cmd->config.multi_plane = 0U;
        }
        else
        {
            cmd->config.operation   = OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT;
            cmd->config.multi_plane = 2U;
        }
    }
    else
    {
        if (nplanes == 1U)
        {
            cmd->config.operation   = OP_NORMAL_PROGRAM_PAGE;
            cmd->config.multi_plane = 0U;
        }
        else
        {
            cmd->config.operation   = OP_PROGRAM_PAGE_MULTI_PLANE;
            cmd->config.multi_plane = 2U;
        }
    }
    cmd->config.cw        = 3U;
    cmd->config.crc       = FC_CRC_ENABLE;
    cmd->config.random    = FC_RANDOM_ENABLE;
    cmd->config.ldpc      = FC_ECC_MODE_BCH;
    cmd->config.chain     = FC_CHAIN_END;
    cmd->config.page_type = FC_PAGE_TYPE_LP;
    cmd->module           = FC_COMMAND_MODULE_FTL;
    cmd->meta             = (DWORD)(internal_lba << CW_WIDTH); /* 扇区 LBA（v24） */

    /* 建 desc 链（逐 plane 各自的 PBA）；desc/buffer 池空 → 瞬态背压 */
    ret = ftl_cmd_setup_ppa_pair(cmd, pba);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return (ret == FTL_AGAIN) ? FTL_AGAIN : FTL_ERROR;
    }

    /* 写：把数据灌进 desc 链（读不需要） */
    if (g->op == (uint8_t)PENDING_OP_WRITE)
    {
        ret = ftl_cmd_write_data(cmd, buffer, data_size);
        if (ret != 0)
        {
            (void)fc_command_release(cmd);
            return FTL_ERROR;
        }
    }

    /* 注册 pending 子操作（必须早于 submit，避免竞态）。单页请求 plane1 字段传 INVALID；
     * lba 传**片内 local**（L2P/Journal 均按 local 索引）。 */
    ret = pending_add(ftl, ftl_cmd_id(cmd), gi, internal_lba, buffer, nplanes, old_pba, (g->op == (uint8_t)PENDING_OP_WRITE) ? new_pba[0] : NAND_PBA_INVALID, old_pba1, ((g->op == (uint8_t)PENDING_OP_WRITE) && (nplanes == 2U)) ? new_pba[1] : NAND_PBA_INVALID, 0U, (ftl_pending_op_t)g->op);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_AGAIN; /* pending 表满：瞬态 */
    }

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
 *             一个子操作消费 1 或 2 个 LBA，取决于 FTL 现场判断能否合成 multi-plane 命令
 *             （读路径可能降级，见 ftl_issue_subop）——"怎么拆"完全由 FTL 决定，调用者
 *             不必预先算出子操作数。每次迭代**重取** g（下发可能改变组状态甚至释放本组）；
 *             全部子操作都在本函数外异步完成，故循环末尾统一试触发组完成通知——覆盖
 *             "整段未映射读"这种从不进 outstanding、无人触发完成的组。
 * -------------------------------------------------------------------- */
static void ftl_drain_group(ftl_context_t *ftl, uint16_t gi)
{
    ftl_io_group_t *g = &ftl->groups[gi];

    while ((g->used != 0U) && (g->next < g->total))
    {
        const uint32_t off    = g->next; /* 已下发到的 LBA 偏移（g->next 以 LBA 计，非子操作序号） */
        const uint32_t remain = g->count - off;
        lba_t          glba   = (lba_t)(g->lba + off);
        void          *buffer = (uint8_t *)g->buffer + ((size_t)off * FTL_READ_USER_SIZE);
        /* 首选双 plane（相邻两 LBA 合成一条 multi-plane 命令）。读路径查出两 LBA
         * 落在同一 plane 时会把 planes 降级为 1（见 ftl_issue_subop），故此处按
         * **实际消费的 LBA 数**推进 next。 */
        uint8_t      planes   = (remain >= 2U) ? 2U : 1U;
        ftl_status_t st       = ftl_issue_subop(ftl, gi, lba_to_local(glba), buffer, &planes);

        g = &ftl->groups[gi]; /* 重取：下发可能改变组状态 */

        if (st == FTL_OK)
        {
            ASSERT((planes == 1U) || (planes == 2U));
            g->stalls = 0U;
            g->next += planes;
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

    /* 语义 = "把已持久化映射落盘"：L2P cache 全部写回 + journal 尾页刷出。
     * **非** NVMe 式 barrier（不等待在飞/排队的写；当前 CLI 靠 g_cli_pending.busy 串行化）。 */
    l2p_cache_flush_all(&ftl->cache);
    journal_flush_page(&ftl->journal);
}

/* --------------------------------------------------------------------
 * ftl_trim
 *   功能：逐 LBA 取消映射（TRIM）——清 bitmap/VPC + 写 L2P INVALID + 记 Journal。
 *   参数：ftl —— 实例；lba —— 起始 LBA；count —— LBA 数（0 视为 1，兼容单 LBA 调用）。
 *   返回：无。
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

        /* 查旧映射：若非 INVALID，同步清该页的 bitmap/VPC（否则 VPC 虚高，GC 选 victim 不准） */
        old_pba = l2p_cache_read(&ftl->cache, local_lba);
        if (old_pba != NAND_PBA_INVALID)
        {
            uint16_t gblock = (uint16_t)ftl_pba_gblock(old_pba);
            ftl_block_clr_valid(&ftl->alloc.blocks[gblock],
                                ftl->alloc.valid_map[gblock],
                                (uint8_t)pba_to_plane(old_pba),
                                (uint8_t)pba_to_page(old_pba));
        }

        l2p_cache_write(&ftl->cache, local_lba, FTL_PBA_INVALID);

        /* ⚠ 取消映射也必须进 Journal（new_pba = INVALID）。L2P 的每一次变更都由
         * Journal 承载，掉电恢复靠回放重建——漏记这一条会让**掉电恢复把已 TRIM
         * 的 LBA 复活**：region 虽被 flush 成 INVALID，但回放按序重放更早的写条目
         * （page_seq 更小但 > cp_sequence）会把它改回有效 PBA，且回放行标 DIRTY
         * 后 ftl_boot_apply_region_bitmap 会跳过预载，NAND 上的 INVALID 也不生效。 */
        journal_append(&ftl->journal, local_lba, old_pba, NAND_PBA_INVALID);
    }
}
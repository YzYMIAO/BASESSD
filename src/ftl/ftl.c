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

/*
 * ftl_now_ms - 当前单调毫秒（经平台抽象 time_us 换算）。
 *
 * 平台未注册（time_us 缺失）时返回 0：此时 elapsed = 0 - start < 阈值，
 * T3 定时恒不触发，行为与接线前一致（不误触发、不破坏启动）。
 */
static uint32_t ftl_now_ms(void)
{
    const ssd_platform_ops_t *platform = ssd_platform_get();

    if ((platform == NULL) || (platform->time_us == NULL))
    {
        return 0U;
    }
    return (uint32_t)(platform->time_us() / 1000U);
}

/*
 * ftl_handle_completion - 处理 FC 命令完成回调（默认 handler）
 *
 * 作为 completion_table 的默认 handler 注册，所有未注册精确路由
 * 的 completion 都走此函数。
 *
 * 根据 completion.module 路由：
 *   FTL      → 查 pending 表 → 读数据 / release
 *   METADATA → release（同步操作完成后释放）
 *   GC       → release
 *
 * @param completion  FC completion status
 * @param ctx         ftl_context_t 指针
 */
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
                    uint32_t data_size = FTL_READ_USER_SIZE * ((pending->io.pages > 0U) ? pending->io.pages : 1U);

                    if ((completion->status & FC_CW_STATUS_NAND_FAIL) == 0U)
                    {
                        /* 判别依据是 pending->op（显式操作类型），不是 io.buffer——
                         * 写请求的 buffer 是数据源、恒非空，用 buffer 判别会把写全判成读。 */
                        if (pending->op == PENDING_OP_READ)
                        {
                            /* 读成功：拷贝数据到 caller buffer */
                            (void)ftl_cmd_read_data(cmd, pending->io.buffer, data_size);

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

                            if (pending->io.status_out != NULL)
                            {
                                *pending->io.status_out = SSD_STATUS_OK;
                            }
                        }
                        else if (pending->cache_intermediate == 0U)
                        {
                            /* 写成功（非 cache 中间页）：确认 Journal + L2P（modify-after-confirm） */
                            journal_append(&ftl->journal,
                                           pending->io.lba,
                                           pending->old_pba,
                                           pending->new_pba);
                            l2p_cache_write(&ftl->cache,
                                            pending->io.lba,
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

                            /* pages=2 Multi-Plane：plane1 页（io.lba+1）同样簿记 */
                            if (pending->io.pages == 2U)
                            {
                                const nand_pba_t pba1 = pending->new_pba1;
                                uint16_t         g1   = (uint16_t)ftl_pba_gblock(pba1);
                                ftl_block_t     *nb1  = &ftl->alloc.blocks[g1];

                                ftl_block_set_valid(nb1,
                                                    ftl->alloc.valid_map[g1],
                                                    (uint8_t)pba_to_plane(pba1),
                                                    (uint8_t)pba_to_page(pba1));

                                /* L2P：io.lba+1 → plane1 页 */
                                l2p_cache_write(&ftl->cache,
                                                (lba_t)(pending->io.lba + 1U),
                                                pba1);
                                journal_append(&ftl->journal,
                                               (lba_t)(pending->io.lba + 1U),
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

                            if (pending->io.status_out != NULL)
                            {
                                *pending->io.status_out = SSD_STATUS_OK;
                            }
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
                                                            pending->io.buffer,
                                                            data_size,
                                                            pending->io.lba,
                                                            &retry_count);
                            if (recovered == 0)
                            {
                                /* Read recovery 成功：恢复好的数据已写入 caller buffer */
                                if (pending->io.status_out != NULL)
                                {
                                    *pending->io.status_out = SSD_STATUS_OK;
                                }
                            }
                            else
                            {
                                /* 恢复失败：返回全零 + 标记坏块
                                 *
                                 * Stage 1（Read Retry）+ Stage 2（邻近页辅助）全失败 →
                                 * 页面已损坏提升为块级不可靠，显式标记坏块（RAM only，
                                 * 后台 bbt_flush 持久化），交由预测搬移/GC 接手搬走有效页。
                                 * （RR Stage 3 XOR/RAID 恢复属后续阶段，当前不做。） */
                                memset(pending->io.buffer, 0, data_size);
                                (void)retry_count; /* 恢复失败，重试次数无意义 */
                                LOGE("[FTL] read unrecoverable lba=%lu gblock=%u\r\n",
                                     (unsigned long)pending->io.lba,
                                     (unsigned)ftl_paa_gblock(cmd->paa));
                                (void)bbt_mark_bad(&ftl->alloc, ftl_paa_gblock(cmd->paa));
                                if (pending->io.status_out != NULL)
                                {
                                    *pending->io.status_out = SSD_STATUS_IO_ERROR;
                                }
                            }
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
                                 (unsigned long)pending->io.lba,
                                 (unsigned)ftl_pba_gblock(pending->new_pba));
                            (void)allocator_close_open_sb(&ftl->alloc,
                                                          (uint8_t)FTL_TEMP_HOT);
                            (void)bbt_mark_bad(&ftl->alloc, ftl_pba_gblock(pending->new_pba));
                        }
                        if (pending->io.status_out != NULL)
                        {
                            *pending->io.status_out = SSD_STATUS_IO_ERROR;
                        }
                    }

                    /* 通知等待方 */
                    if (pending->io.complete_flag != NULL)
                    {
                        *pending->io.complete_flag = 1U;
                    }

                    /* Controller 子请求完成回调（聚合计数器减 1） */
                    if (pending->io.on_complete != NULL)
                    {
                        pending->io.on_complete(pending->io.on_complete_ctx);
                    }

                    pending_remove(ftl, cmd_id);
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

void ftl_service(ftl_context_t *ftl)
{
    uint32_t submitted = 0U;
    uint16_t inflight;

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

    /* Phase 3: 三级后台模式（B3：空闲度基于未决命令数，非"本轮有无提交 NAND"代理）
     *
     * 旧实现用 submitted（本轮 fc_service 提交了几条）判定，host 无 IO 时
     * fc_service 也可能返回非 0（后台命令推进）→ 误判 NORMAL/BUSY，GC/WL/CP 全推迟。
     * 现在用 pending（host IO 未决）+ completion_table（同步/后台命令未决）占用
     * 近似系统空闲度：两者都空 = 真空闲 → IDLE 全力后台；仅 pending 占用高 = host
     * IO 繁忙 → BUSY 最少后台；中间 → NORMAL 按需后台。
     *
     * 近似方案（文档标注）：pending/completion 是"占用快照"，不含队列里排队未 submit
     * 的 host 请求（controller 层不回报其深度）。单实例 LUN_NUM=1 下够用。 */
    inflight = (uint16_t)(pending_count(ftl) + completion_table_count());
    if (inflight == 0U)
    {
        ftl_service_idle(ftl);
    }
    else if (pending_count(ftl) > (FTL_PENDING_MAX / 2U))
    {
        ftl_service_busy(ftl);
    }
    else
    {
        ftl_service_normal(ftl);
    }
}

ftl_status_t ftl_submit_read(ftl_context_t *ftl, const ftl_io_request_t *req)
{
    fc_command_t *cmd;
    nand_pba_t    pba[PLANE_NUM];
    uint32_t      data_size;
    int           ret;
    lba_t         internal_lba;

    if ((ftl == NULL) || (req == NULL) || (req->buffer == NULL) ||
        (req->pages == 0U) || (req->pages > 2U))
    {
        return FTL_ERROR;
    }
    data_size = FTL_READ_USER_SIZE * req->pages;

    /* LBA 越界检查（全局 LBA 空间） */
    if (req->lba >= FTL_LBA_MAX)
    {
        LOGE("[FTL] read lba out of range lba=%lu\r\n", (unsigned long)req->lba);
        return FTL_ERROR;
    }
    /* 全局 LBA → 本 FTL（ch/ce/lun）的片内 local LBA（交错路由，见 l2p_cache.h）。
     * 本 FTL 上下文已绑定固定 (ch,ce,lun)；LUN_NUM=1 时折叠为原值。 */
    internal_lba = lba_to_local(req->lba);

    /* ① 查 L2P：pages=2 时两 plane 的物理位置**各自独立**（Policy B：SB 两 plane 不再同号），
     * 必须查两次。任一未映射 → 整请求按全零返回（与单页未映射的既有语义一致）——
     * 部分映射的 multi-plane 读无法只读一半 plane，且混读会把未写页的残值当数据返回。 */
    pba[0] = l2p_cache_read(&ftl->cache, internal_lba);
    pba[1] = (req->pages == 2U) ? l2p_cache_read(&ftl->cache, (lba_t)(internal_lba + 1U))
                                : NAND_PBA_INVALID;
    if ((pba[0] == FTL_PBA_INVALID) ||
        ((req->pages == 2U) && (pba[1] == FTL_PBA_INVALID)))
    {
        memset(req->buffer, 0, data_size);

        /* 通知等待方：虽无 NAND 操作，但仍需发完成信号 */
        if (req->status_out != NULL)
        {
            *req->status_out = SSD_STATUS_OK;
        }
        if (req->complete_flag != NULL)
        {
            *req->complete_flag = 1U;
        }
        if (req->on_complete != NULL)
        {
            req->on_complete(req->on_complete_ctx);
        }

        return FTL_OK;
    }

    /* ② 分配 FC 命令 */
    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        LOGE("[FTL] read: FC command pool exhausted\r\n");
        return FTL_ERROR;
    }

    /* ③ 配置读命令 — 根据 pages 选择单 Plane 或 Multi-Plane */
    if (req->pages == 1U)
    {
        cmd->config.operation   = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
        cmd->config.multi_plane = 0U;
    }
    else
    {
        cmd->config.operation   = OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT;
        cmd->config.multi_plane = 2U;
    }
    cmd->config.cw        = 3U;
    cmd->config.crc       = FC_CRC_ENABLE;
    cmd->config.random    = FC_RANDOM_ENABLE;
    cmd->config.ldpc      = FC_ECC_MODE_BCH;
    cmd->config.chain     = FC_CHAIN_END;
    cmd->config.page_type = FC_PAGE_TYPE_LP;
    cmd->module           = FC_COMMAND_MODULE_FTL;
    cmd->meta             = (DWORD)(internal_lba << CW_WIDTH); /* 扇区 LBA（v24） */

    /* ④ 配置 PBA + desc 链（逐 plane 各自的 PBA） */
    ret = ftl_cmd_setup_ppa_pair(cmd, pba);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    /* ⑤ 注册 pending（必须早于 submit，避免竞态；io 按值嵌入） */
    ret = pending_add(ftl, ftl_cmd_id(cmd), req, NAND_PBA_INVALID, NAND_PBA_INVALID, NAND_PBA_INVALID, NAND_PBA_INVALID, 0U, PENDING_OP_READ);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    /* ⑥ 异步提交 */
    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        pending_remove(ftl, ftl_cmd_id(cmd));
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    /* ⑦ 立即返回，数据在 completion_table_drain 中拷贝到 buffer */
    return FTL_OK;
}

ftl_status_t ftl_submit_write(ftl_context_t *ftl, const ftl_io_request_t *req)
{
    fc_command_t *cmd;
    nand_pba_t    new_pba[PLANE_NUM];
    nand_pba_t    old_pba;
    nand_pba_t    old_pba1 = NAND_PBA_INVALID;
    uint32_t      data_size;
    int           ret;
    lba_t         internal_lba;

    if ((ftl == NULL) || (req == NULL) || (req->buffer == NULL) ||
        (req->pages == 0U) || (req->pages > 2U))
    {
        return FTL_ERROR;
    }
    data_size = FTL_READ_USER_SIZE * req->pages;

    /* LBA 越界检查（全局 LBA 空间） */
    if (req->lba >= FTL_LBA_MAX)
    {
        LOGE("[FTL] write lba out of range lba=%lu\r\n", (unsigned long)req->lba);
        return FTL_ERROR;
    }

    /* 全局 LBA → 本 FTL（ch/ce/lun）的片内 local LBA（交错路由，见 l2p_cache.h）。
     * 本 FTL 上下文已绑定固定 (ch,ce,lun)；LUN_NUM=1 时折叠为原值。 */
    internal_lba = lba_to_local(req->lba);

    /* ① Allocator 分配新 PBA（**逐 plane 独立选块**——Policy B：SB 两 plane 不再同号）。
     * Free Pool 耗尽 → 同步跑一轮紧急 GC（回收 1 个 Super Block = 2 个 Block，
     * 足够一个 2-page 请求），归还后再重试一次分配。真无空间才返回失败。 */
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

    /* ② 读旧 PBA（journal 记录用）。
     * pages=2 时平面1 页也要读旧映射（Multi-Plane 双页簿记，见 completion）。 */
    old_pba  = l2p_cache_read(&ftl->cache, internal_lba);
    old_pba1 = (req->pages == 2U)
                   ? l2p_cache_read(&ftl->cache, (lba_t)(internal_lba + 1U))
                   : NAND_PBA_INVALID;

    /* ③ 配置 FC 写命令 — 根据 pages 选择单 Plane 或 Multi-Plane */
    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        LOGE("[FTL] write: FC command pool exhausted\r\n");
        return FTL_ERROR;
    }

    if (req->pages == 1U)
    {
        cmd->config.operation   = OP_NORMAL_PROGRAM_PAGE;
        cmd->config.multi_plane = 0U;
    }
    else
    {
        cmd->config.operation   = OP_PROGRAM_PAGE_MULTI_PLANE;
        cmd->config.multi_plane = 2U;
    }
    cmd->config.cw        = 3U;
    cmd->config.crc       = FC_CRC_ENABLE;
    cmd->config.random    = FC_RANDOM_ENABLE;
    cmd->config.ldpc      = FC_ECC_MODE_BCH;
    cmd->config.chain     = FC_CHAIN_END;
    cmd->config.page_type = FC_PAGE_TYPE_LP;
    cmd->module           = FC_COMMAND_MODULE_FTL;
    cmd->meta             = (DWORD)(internal_lba << CW_WIDTH); /* 扇区 LBA（v24） */

    /* ④ 配置 PBA + desc 链 + 写入数据（逐 plane 各自的 PBA） */
    ret = ftl_cmd_setup_ppa_pair(cmd, new_pba);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    ret = ftl_cmd_write_data(cmd, req->buffer, data_size);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    /* ⑤ 注册 pending（io 按值嵌入 + old/new PBA；实例即上下文，无需再存 ftl）。
     * 单页请求 plane1 字段传 INVALID。 */
    ret = pending_add(ftl, ftl_cmd_id(cmd), req, old_pba, new_pba[0], old_pba1, (req->pages == 2U) ? new_pba[1] : NAND_PBA_INVALID, 0U, PENDING_OP_WRITE);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    /* ⑥ 异步提交到 NAND */
    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        pending_remove(ftl, ftl_cmd_id(cmd));
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    /* ⑦ Journal + L2P 在 completion handler 中确认 NAND PROGRAM 成功后执行 */
    return FTL_OK;
}

/* ── UART CLI 便利包装（非阻塞，仅用于 pending 表跟踪）── */

void ftl_trim(ftl_context_t *ftl, uint64_t lba)
{
    nand_pba_t old_pba;
    lba_t      local_lba;

    if (ftl == NULL)
    {
        return;
    }
    if (lba >= FTL_LBA_MAX)
    {
        return;
    }

    /* 全局 LBA → 本 FTL 片内 local（与读/写路径一致的交错路由）。
     * controller 无维度知识，转换在本 FTL 内部完成。 */
    local_lba = lba_to_local(lba);

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

    /* 取消 LBA 映射（非阻塞） */
    l2p_cache_write(&ftl->cache, local_lba, FTL_PBA_INVALID);
}
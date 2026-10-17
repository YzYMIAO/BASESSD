/*
 * ftl.c — FTL 顶层（初始化 / 后台调度服务主循环 / 子操作下发 / 对外读写入口）
 *
 * 职责：
 *   FTL 的对外门面与主循环——ftl_init 装配各子模块；ftl_service 是**服务主循环**
 *   （NAND 泵 → 完成泵 → 补发 host 子操作 → 时间预算后台调度）；ftl_submit_read/write
 *   把一条 host 请求拆成多个 FC 子操作下发；ftl_flush / ftl_trim 供 HIL 调用。
 *
 * 与子模块的分工：
 *   本文件             —— 编排：调度、拆分、完成聚合、模式切换；不解析介质格式。
 *   pending.c          —— 子操作登记 + 组完成聚合（一条 host 请求 = 一个 group）。
 *   completion_table.c —— cmd_id → ctx 旁表 + 实例路由 + CQ 排空（派发给 ftl_op.c）。
 *   其余模块（gc / wl / journal / checkpoint / bbt / wear / l2p_cache）各司其职，
 *   见各自文件头。
 *
 * 服务主循环（ftl_service）四阶段：
 *   Phase 1    fc_service × NAND_SUBMIT_BUDGET —— 推进 NAND
 *   Phase 2    completion_table_drain —— 统一完成泵（按命令 op_tag 派发到服务函数数组）
 *   Phase 2.5  逐组补发 host 子操作（host IO 优先于后台任务）
 *   Phase 3    时间预算后台调度（v0.76）：14 项任务表（6 must-run + 8 预算）经
 *              bg_scheduler_run 执行；档位 IDLE/NORMAL/BUSY 按未决命令数选（见下）
 *
 * 后台档位（按 inflight = pending + groups 近似空闲度，ftl_bg_level_select）：
 *   IDLE    inflight == 0                     全预算（含 RR 巡检）
 *   BUSY    pending > MAX/2 或有排队 host 请求 最小预算（仅 must-run + 1 l2p flush）
 *   NORMAL  其余                               按需预算（无 RR 巡检）
 * 档位 → 预算规模/任务掩码见 g_bg_level_cfg；时间预算语义见 bg_scheduler.h 头注。
 *
 * 文件结构：
 *   1. 内部辅助        ftl_now_ms / ftl_op_host_io（host I/O 完成服务函数）
 *   2. 后台调度器接线  ftl_gc_evaluate（static）+ 14 个 bg_a_* 适配器 + g_bg_level_cfg
 *                      + ftl_bg_init / ftl_bg_level_select（接线在 bg_scheduler.c 引擎上）
 *   3. 对外接口        ftl_init / ftl_service / ftl_submit_read / ftl_submit_write
 *   4. 子操作下发      ftl_issue_subop / ftl_drain_group（static）
 *   5. 元数据落盘/TRIM ftl_flush / ftl_trim
 *
 * ⚠ 回收动作（原 ftl_gc_reclaim_one）已随阶段 4 迁入 `ftl/migrate.c`：GC / WL / 慢死预测
 *   共用一台异步搬移 job（mig_begin_* / mig_service），本文件只做**触发求值**与**启动**。
 *
 * ⚠ 多实例（F6）：pending / groups / rr_last_scan_ms 均 per-instance；completion 全局
 *   但按命令 ch/ce/lun 路由。main.c 主循环：先 ftl_service_pump()（全局泵，每轮一次）
 *   抽干 NAND + 完成表，再**遍历全部实例**调 ftl_service() 逐实例推进 Phase 2.5/3——
 *   只跑 [0][0][0] 会让其余实例的 journal / checkpoint / wear / bbt / GC 永不推进（序 4）。
 */

#include "ftl/ftl.h"

#include "common/assertion.h"
#include "common/log.h"
#include "fc/fc.h"
#include "ftl/bbt.h"
#include "ftl/checkpoint.h"
#include "ftl/completion_table.h"
#include "ftl/ftl_fc_interface.h"
#include "ftl/ftl_op.h"  /* ftl_op_submit / FTL_OP_* */
#include "ftl/gc.h"      /* gc_check_triggers / gc_select_victim（GC 触发求值与选块） */
#include "ftl/migrate.h" /* mig_begin_gc / mig_begin_wl / mig_begin_predict / mig_service */
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
static bool         ftl_wbuf_has(const ftl_context_t *ftl, lba_t internal_lba);
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
 *   返回：FTL_OK 已全部应用；FTL_AGAIN 有 region 未就绪（**一条都没应用**，调用者下轮
 *         重来）。**只有这两态**——就绪前置挡住了唯一的失败源，第二遍不可能失败。
 *   实现要点：
 *     · CW 级映射下 1 LBA = 1 CW：一个子操作覆盖 pending->n_cw 个连续 local LBA，
 *       每个 LBA 各自一条 Journal + 一条 L2P 更新（Journal 流量 ×4，已登记）。
 *     · 目标页按 **LBA 的 plane 位**选：plane0 的 LBA 落 pending->new_pba、plane1 的
 *       落 pending->new_pba1。Policy B 下两 plane 独立选块，且「物理 plane 必须等于
 *       LBA 的 plane 位」是页内 META 能还原出正确 LBA 的前提（ftl_config.h §三）。
 *     · **不存 old_pba**：完成时按 LBA 重查 L2P 得"此刻被替换的映射"。同一 LBA 的两次
 *       写在飞时，先完成的先改 L2P，后完成的重查即得前者；提交时快照会让两条 Journal
 *       都指向最初映射，链断。⚠ **异步化（阶段 5）没有削弱这条**：本函数仍是在**完成
 *       时刻**执行（停泊只是延后几轮，顺序仍按完成序），重查语义不变。
 *     · valid_count 单位是 **CW**：新 CW +1、被替换的旧 CW -1。真有效性一律逐 CW
 *       由页内 META + L2P 反查现判（DRAM-less approach ③，无位图）——见 bst.h 文件头。
 *     · ⚠ **两遍式**（阶段 5）：第一遍只做**就绪前置**（region 未驻留则起异步加载并返
 *       FTL_AGAIN），第二遍才动账。绝不能边查边改——l2p_cache_write 对正在加载/写回
 *       的行返 FTL_AGAIN，中途放弃会让同一页里部分 CW 已记账、部分没记，重试时重复
 *       记账（valid_count 虚高、journal 重复条目、total_writes 多计）。
 *       两遍之间无泵（单线程顺序执行）⇒ 就绪判定与第二遍之间状态不可能变。
 * -------------------------------------------------------------------- */
static ftl_status_t ftl_confirm_write(ftl_context_t *ftl, const ftl_pending_req_t *pending)
{
    /* 两个调用点（ftl_op_host_io 的 `if (pending != NULL)` 内 / ftl_confirm_kick 的
     * `&ftl->pending[i]`）都保证非空 ⇒ 不在本函数复检（§十附 / §二十六）。 */

    /* ── 第一遍：就绪前置（只探不动）── */
    for (uint8_t bit = 0U; bit < (uint8_t)FTL_LBA_PER_PAIR; bit++)
    {
        if ((pending->cw_bitmap & (uint8_t)(1U << bit)) == 0U)
        {
            continue;
        }
        if (!l2p_cache_ready(&ftl->cache, (lba_t)(pending->lba + (lba_t)bit)))
        {
            return FTL_AGAIN; /* 已起加载：本轮**一条都不动** */
        }
    }

    /* ── 第二遍：应用（此时不可能再返 FTL_AGAIN）── */
    /* 页打包（§七 方案 A）：pending 描述**一整页**——pending->lba = 窗口基址，
     * cw_bitmap 标记本页哪些 CW 有效，逐**置位**确认。逐 CW 的 LBA = base + bit
     * （bit 与 LBA 低 3 位 [plane][cw] 同构：bit = plane*CW_NUM + cw）。 */
    for (uint8_t bit = 0U; bit < (uint8_t)FTL_LBA_PER_PAIR; bit++)
    {
        if ((pending->cw_bitmap & (uint8_t)(1U << bit)) == 0U)
        {
            continue; /* 本槽未写（页打包后一页可只填部分 CW） */
        }

        const uint8_t    pl   = (uint8_t)(bit / CW_NUM);
        const uint8_t    cw   = (uint8_t)(bit % CW_NUM);
        const lba_t      l    = (lba_t)(pending->lba + (lba_t)bit);
        const nand_pba_t page = (pl == 0U) ? pending->new_pba : pending->new_pba1;

        /* 本页只为本命令覆盖到的 plane 分配了页（未覆盖的 plane 不进本循环体）。
         * 覆盖到的 plane 必有页（allocator_alloc_pair 两 plane 都给）。 */
        ASSERT(page != NAND_PBA_INVALID);
        ASSERT(pba_to_plane(page) == pl);

        nand_pba_t newp = pba_with_cw(page, cw);

        /* 就绪前置已保证驻留 ⇒ 读必命中、写必不返 FTL_AGAIN。非 OK 只剩"LBA 越界"
         * 一种可能（L2P 范围外）——旧实现同样把它当"未映射"处理，保持等价 + 一行日志。 */
        nand_pba_t oldp;
        if (l2p_cache_read(&ftl->cache, l, &oldp) != FTL_OK)
        {
            LOGE("[FTL] confirm: l2p read lba=%lu out of range\r\n", (unsigned long)l);
            oldp = NAND_PBA_INVALID;
        }

        journal_append(&ftl->journal, l, oldp, newp);
        (void)l2p_cache_write(&ftl->cache, l, newp);

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
             * 打印旧块现场是为了**一次归因**（state 一眼可判，名字由 ftl_block_state_name 给）：
             *   BAD     = 坏块，重建按判据跳过（数据本已不可读）
             *   FREE    = 已归还空闲池 ⇒ 块还会被 allocator 复用，**丢数据**
             *   LOG     = 元数据日志块 ⇒ 判据过宽（v0.69 已修 plane 掩码）
             *   NONE    = 重建从未认领（页 0 判空 / 未扫描到）
             * 代价 = 每次覆盖写多一次比较；收益 = 把「重启后写一次就死机」变成一条可读日志。 */
            if (ftl->alloc.blocks[gob].valid_count == 0U)
            {
                (void)LOGE("[FTL] dangling lba=%lu old=%08lx g=%u st=%s sb=%u newg=%u nst=%s\r\n",
                           (unsigned long)l,
                           (unsigned long)oldp,
                           (unsigned)gob,
                           ftl_block_state_name(ftl_block_state(&ftl->alloc.blocks[gob])),
                           (unsigned)ftl->alloc.blocks[gob].sb_id,
                           (unsigned)ftl_pba_gblock(newp),
                           ftl_block_state_name(
                               ftl_block_state(&ftl->alloc.blocks[ftl_pba_gblock(newp)])));
            }

            ftl_block_sub_valid(&ftl->alloc.blocks[gob]);
        }
    }

    checkpoint_journal_watermark(&ftl->journal);
    ftl->total_writes++; /* 计「编程次数」（页打包后一页 = 一次编程），不是 CW 数 */
    return FTL_OK;
}

/* --------------------------------------------------------------------
 * ftl_op_host_io
 *   功能：host 数据 I/O 子操作的完成服务函数（g_op_service[FTL_OP_HOST_IO]）。
 *   参数：ftl —— 命令归属实例（ftl_op_dispatch 按命令自身 ch/ce/lun 解析后传入）；
 *         completion —— FC 完成状态；ctx —— 未用（host I/O 的私有状态在 pending 表，
 *         比任何旁表 ctx 都全，故该站点提交时 ctx 传 NULL）。
 *   返回：无。
 *   实现要点：按 pending->op 分流——读成功拷数据 + 记 ECC/read_count；写成功
 *             ftl_confirm_write；读失败**托管**给 Read Recovery job（异步，本函数
 *             当场 release 并返回）；写失败关 Open SB + 标坏块。
 *             非托管路径收尾 pending_remove + 组聚合，最后 release
 *             （**本服务函数拥有命令**）。
 *
 *             ⚠ 本函数**不得**同步等待（见 ftl_op.h）——它跑在完成泵里，再调
 *             ftl_sync_submit_wait 就是嵌套泵（阶段 4~8 逐步消灭剩余站点）。
 * -------------------------------------------------------------------- */
void ftl_op_host_io(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx)
{
    (void)ctx; /* host I/O 不登记旁表 ctx */

    /* command_id 由 NAND 从本命令回填；公共壳 fc_command_id_to_command 已断言
     * ID 在池内，恒返回非空指针。 */
    uint16_t      cmd_id = (uint16_t)completion->command_id;
    fc_command_t *cmd    = fc_command_id_to_command(cmd_id);

    /* 不该发生：路由表已在 main.c 逐实例 boot 之前设好（见 ssd_ftl_boot_all ①），
     * 完成派发保证 ftl 恒非空。不留 release-and-return 兜底：debug 当场炸，
     * release（NDEBUG 裁空）下若真走到则 NULL 解引用留现场（比静默吞完成可定位）。 */
    ASSERT(ftl != NULL);

    ftl_pending_req_t *pending = pending_lookup(ftl, cmd_id);

    if (pending != NULL)
    {
        ssd_status_t sub_status = SSD_STATUS_OK;

        if ((completion->status & FC_CW_STATUS_NAND_FAIL) == 0U)
        {
            /* 判别依据是 pending->op（显式操作类型），不是 buffer——
             * 写请求的 buffer 是数据源、恒非空，用 buffer 判别会把写全判成读。 */
            if (pending_op_is_read(pending->op))
            {
                /* 读成功：拷贝数据到 caller buffer。
                 * desc 链序 == LBA 升序（plane-major + 段内 CW 升序，与
                 * LBA 的 [plane][cw] 布局同序）⇒ 按链序连续拷出即 LBA 序。 */
                uint32_t data_size = FTL_IU_BYTES * pending->n_cw; /* 子操作字节数 */
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
                /* 写成功（非 cache 中间页）：逐 CW 确认 Journal + L2P + 有效计数。
                 * ⚠ region 未就绪（正在异步加载）⇒ **停泊**本项（阶段 5）：命令脱钩
                 * （command_id 置哨兵，本命令随即 release，id 会被复用）+ 组在飞数
                 * **不**递减（host 写组继续 pending），由 ftl_confirm_kick 每轮推进。
                 * 与阶段 3 的托管同构，区别是推进者在本文件而非 RR 引擎。 */
                if (ftl_confirm_write(ftl, pending) == FTL_AGAIN)
                {
                    pending->op         = (uint8_t)PENDING_OP_WRITE_CONFIRM;
                    pending->command_id = FTL_PENDING_CMD_NONE;

                    (void)fc_command_release(cmd);
                    return;
                }
            }
            /* cache_intermediate 页：跳过 L2P/Journal 更新，等最终 10h 确认 */
        }
        else
        {
            /* 同上：按 pending->op 判别，不用 io.buffer */
            if (pending_op_is_read(pending->op))
            {
                /* 读失败 ⇒ **托管**给 Read Recovery job（阶段 3，见 read_recovery.c）：
                 *   · pending 项**保留**（不 pending_remove）、组在飞数**不**递减
                 *     ⇒ host 读组继续 pending，等 RR 收尾时统一结算；
                 *   · command_id 置哨兵：本命令随即 release，其 id 会被后续
                 *     fc_command_alloc 复用，留旧值会让新命令的完成误撞上本项；
                 *   · 收尾（含一律报 IO_ERROR 的既有语义）在 rr_finish 里，见该处注释。
                 * ⚠ 本分支**不**再当场恢复——原实现会在这里同步跑完三级恢复，
                 *   即在完成泵里再开一个完成泵（嵌套泵），且整条 host 读流水线被
                 *   一个坏页钉住。 */
                pending->op         = (uint8_t)PENDING_OP_READ_RECOVERY;
                pending->command_id = FTL_PENDING_CMD_NONE;

                (void)fc_command_release(cmd);
                rr_kick(ftl);
                return;
            }
            else
            {
                /* 写失败：行业坏块替换流程（ftl_design §3.8）
                 *
                 * ① 关闭当前 Open Super Block（不再写入新数据）→ 进 closed_sb_list
                 *    → GC 优先回收（该 SB 有效 CW 偏低：失败 CW 不计数 + 旧 CW 被覆盖清账）
                 * ② bbt_mark_bad（RAM only，后台 bbt_kick 持久化）——标 plane0 的 block 号。
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
                (void)allocator_close_open_sb(&ftl->alloc, (uint8_t)FTL_TEMP_HOT);
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

/* --------------------------------------------------------------------
 * ftl_confirm_kick
 *   功能：推进**停泊**的写确认（阶段 5）——那些 NAND 已编程成功、但因 L2P region
 *         尚未就绪（异步加载在飞）而挂起的写子操作。
 *   参数：ftl —— 实例。
 *   返回：无。
 *   实现要点：按**下标**升序扫（停泊项的 command_id 已置哨兵 FTL_PENDING_CMD_NONE，
 *             按 id 查不到）；仍不就绪的原地留着下轮再来。确认完成后走与
 *             ftl_op_host_io **完全相同**的收尾（release pending 槽 + 逐组递减），
 *             故 host 写组能正常结算。
 *   ⚠ 必须由 ftl_service 在 **Phase 2.5 开头**（补发 host 子操作之前）调用：
 *     停泊意味着该 LBA 的 L2P 还是旧映射，此时若先补发一条读同一 LBA 的子操作，
 *     就会读到旧物理页（陈旧数据）。先确认再下发把窗口压到最小。
 * -------------------------------------------------------------------- */
static void ftl_confirm_kick(ftl_context_t *ftl)
{
    for (uint8_t i = 0U; i < FTL_PENDING_MAX; i++)
    {
        ftl_pending_req_t *pending = &ftl->pending[i];

        if ((pending->used == 0U) || (!pending_op_is_write_confirm(pending->op)))
        {
            continue;
        }

        if (ftl_confirm_write(ftl, pending) != FTL_OK)
        {
            continue; /* region 仍在加载：保留停泊项，下轮再来 */
        }

        /* 停泊发生在 NAND **编程成功之后** ⇒ 子操作状态恒 OK（没有写失败分支）。 */
        uint32_t owners = pending->owners;
        pending_release_index(ftl, i);
        pending_group_complete_subop_multi(ftl, owners, SSD_STATUS_OK);
    }
}

/* ── 三级后台服务 ── */

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
 *     与 rr_last_scan_ms / wear_kick 的节拍门同族。
 *     代价（如实登记）：T1/T4 的空间压力响应也被门到同一周期——IDLE 补池突发与
 *     NORMAL 的「一轮回收一个 SB」最坏延迟 GC_EVAL_INTERVAL_MS。真空闲下按需兜底
 *     仍在写路径（ftl_issue_subop），且 10ms 远小于搬一个 SB 的 NAND 时间。
 * -------------------------------------------------------------------- */
static uint32_t ftl_gc_evaluate(ftl_context_t *ftl)
{
    uint32_t now_ms = ftl_now_ms();

    /* 节拍门。⚠ gc_eval_last_ms == 0 是「尚未求值」哨兵：平台时钟缺失时
     * ftl_now_ms() 恒 0，此时**每轮都求值**——GC 不能因缺时钟而停摆。 */
    if ((ftl->gc_eval_last_ms != 0U) && ((now_ms - ftl->gc_eval_last_ms) < GC_EVAL_INTERVAL_MS))
    {
        return 0U;
    }
    ftl->gc_eval_last_ms = now_ms;

    uint32_t triggers = gc_check_triggers(&ftl->alloc);

    /* 观测口：位图见 gc.h GC_TRIGGER_*（多位可同时命中）。日志走**自己的节拍**
     * （GC_LOG_INTERVAL_MS，与求值同一套时基），不是逐次求值都打。
     * ⚠ 求值节拍不能兼作日志节拍：前者管「多久算一次」，10ms 拍仍是 100 行/秒。
     * 语义 = 进入触发态立即一行 + 触发态内每 GC_LOG_INTERVAL_MS 一行心跳；
     * 退出触发态即归零 ⇒ 下次进入必然立即打（故无需另存触发掩码）。
     * ⚠ 行内带 `reclaimed` / `novictim` 两个**进展计数**（mig_finish / mig_begin_gc 自增）：
     * 只有它们能区分「触发但在推进」与「触发但卡死」——`trig` 常亮对两者一视同仁，
     * 而 `[GC] victim` 行（gc.c）在卡死时**根本不出现**，其"消失"也与"已收敛"同形。 */
    if (triggers != 0U)
    {
        if ((ftl->gc_log_last_ms == 0U) || ((now_ms - ftl->gc_log_last_ms) >= GC_LOG_INTERVAL_MS))
        {
            ftl->gc_log_last_ms = now_ms;
            LOGI("[GC] trig=0x%02x free=%u op=%u/%u writes=%lu reclaimed=%lu novictim=%lu\r\n",
                 (unsigned)triggers,
                 (unsigned)allocator_free_count(&ftl->alloc),
                 (unsigned)allocator_op_count(&ftl->alloc, 0U),
                 (unsigned)allocator_op_count(&ftl->alloc, 1U),
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

/* ════════════════════════════════════════════════════════════════════════
 * 后台调度器接线（v0.76）
 *
 * v0.75 的三级 static 服务函数（ftl_service_idle/normal/busy）→ 14 项任务表 +
 * 档位→预算映射。引擎在 bg_scheduler.c（通用、零 kick 知识）；本文件只做三件事：
 *   ① 14 个 static 适配器：把异构 kick（签名/参数各异）包成 void(ftl_context_t*)；
 *   ② g_bg_level_cfg[3]：档位 → 每轮总预算 + l2p flush 条数上界 + 预算任务掩码；
 *   ③ ftl_bg_init 填任务表 / ftl_bg_level_select 负载→档位（纯函数）。
 *
 * 正确性关键集（must_run）无条件每轮跑、不 gate、不占预算——删了系统会挂：
 *   checkpoint_kick / rr / mig_service / metadata_dir / journal need_erase / 紧急 CP。
 * 全表每项的 interval / must_run / budget 依据见 bg_scheduler.h 头注与下方各适配器注释。
 *
 * ⚠ 掩码位下标 = 任务表下标（ftl_bg_init 顺序填充，见计划「勿错位」警示）。
 * ⚠ 预算时钟 = 平台 time_us（SysTick 1ms 量化）；确定性前台保护由「档位→条数/掩码」
 *   承担（见 g_bg_level_cfg 与 bg_scheduler.h「关键语义」）。
 * ════════════════════════════════════════════════════════════════════════ */

/* 档位 → 预算规模（IDLE/NORMAL/BUSY）。锚定 onfi_1_0.c 时序（NAND_TIME_TPROG_US=700）。
 *  IDLE/NORMAL 2000µs ≈ 2~3 页写 + GC 全表求值；BUSY 700µs ≈ 恰 1 页写。
 * 掩码：IDLE 位 6..13 全 budgeted（含 RR 巡检）；NORMAL 位 6..12（无 RR 巡检）；
 *       BUSY 仅位 8（l2p flush）—— 恰「仅 must-run + 1 flush」，与旧 BUSY 档逐字等价。 */
static const bg_level_cfg_t g_bg_level_cfg[BG_LEVEL_BUSY + 1U] = {
  /* IDLE   */ {2000U,        (uint8_t)L2P_CACHE_SIZE,    0x3FC0U},
 /* NORMAL */
    {2000U, (uint8_t)BG_NORMAL_FLUSH_COUNT,    0x1FC0U},
 /* BUSY   */
    { 700U,   (uint8_t)BG_BUSY_FLUSH_COUNT, (1U << 8U)},
};

/* ── 14 个适配器（包异构 kick → void(ftl_context_t*)）── */

/* 位 0 · must_run（最先）—— 推进**已在飞**的 Checkpoint job（阶段 6）。
 * ⚠ 必须排在最前、且不占预算：CP 可能在 BUSY 档被紧急水位起（journal≥95%），后续阶段
 *   要跨多轮跨档位走完——排在预算 pass 里会被低优先级/预算耗尽饿死（旧实现也是先于
 *   三档统一推进，见 ftl_service 的阶段 6 注释）。 */
static void bg_a_checkpoint(ftl_context_t *ftl)
{
    checkpoint_kick(ftl);
}

/* 位 1 · must_run —— 推进 Read Recovery job（阶段 3）。
 * host 读在等结果（托管的子操作占着组在飞数），不推进即死等；任何档位都要跑。 */
static void bg_a_rr(ftl_context_t *ftl)
{
    rr_kick(ftl);
}

/* 位 2 · must_run —— 推进搬移 job（阶段 4，GC/WL/预测共用，已在飞则续跑下一步）。
 * 写路径的紧急 GC 在 BUSY 档起（ftl_wbuf_flush 分配不到块 ⇒ FTL_AGAIN），
 * 不推进即死锁；任何档位都要跑。 */
static void bg_a_mig_service(ftl_context_t *ftl)
{
    mig_service(ftl);
}

/* 位 3 · must_run —— 元数据目录整理 job（阶段 8）。
 * compact_wanted 是 append_slot 在**前台写路径**上置的（池吃紧时换块），
 * 不推进就是写路径永久 FTL_AGAIN；任何档位都要跑。 */
static void bg_a_metadata_dir(ftl_context_t *ftl)
{
    metadata_dir_kick(&ftl->metadata_dir, &ftl->alloc);
}

/* 位 4 · must_run —— 补「未起的换块擦除」（位 9 的子集；journal_kick 单飞行，
 * IDLE/NORMAL 双跑安全）。新环形块没擦过就不能往里写 ⇒ 条件为真即起。 */
static void bg_a_journal_erase(ftl_context_t *ftl)
{
    if ((ftl->journal.job.phase == JRN_IDLE) && (ftl->journal.job.need_erase != 0U))
    {
        journal_kick(&ftl->journal);
    }
}

/* 位 5 · must_run —— 紧急 Checkpoint（Journal ≥ 95%）。
 * 原只在 BUSY 档检查；移入 must-run 后每轮检查。checkpoint_begin 只装配 job、单飞行
 * （cp_job.active → FTL_AGAIN）幂等 ⇒ 这是**刻意的行为放宽**：紧急水位必须随时响应。 */
static void bg_a_emergency_cp(ftl_context_t *ftl)
{
    if (checkpoint_is_emergency(ftl))
    {
        (void)checkpoint_begin(ftl);
    }
}

/* 位 6 · 预算 — GC 统一触发判定（与三档旧实现同一函数；内部 GC_EVAL_INTERVAL_MS 门兜底，
 * interval=GC_EVAL_INTERVAL_MS 是调度器级节拍）。命中即**起一个** GC 搬移 job（单飞行）。 */
static void bg_a_gc_eval(ftl_context_t *ftl)
{
    if (ftl_gc_evaluate(ftl) != 0U)
    {
        (void)mig_begin_gc(ftl);
    }
}

/* 位 7 · 预算 — 磨损均衡（触发判定在 mig_begin_wl 内部；interval=0 交内部门） */
static void bg_a_wl(ftl_context_t *ftl)
{
    (void)mig_begin_wl(ftl);
}

/* 位 8 · 预算 — 后台 flush 脏 L2P region，条数上界 = 档位 l2p_flush_max
 * （IDLE=L2P_CACHE_SIZE / NORMAL=BG_NORMAL_FLUSH_COUNT / BUSY=BG_BUSY_FLUSH_COUNT，
 * 经 ftl->bg.level 反查档位）。BUSY 仅此一项 ⇒ 「must-run + 1 flush」与旧档逐字等价。
 * ⚠ thrashing 降级（P1-4）：主动 flush 大概率白写且挤前台带宽 ⇒ 停主动 flush。 */
static void bg_a_l2p_flush(ftl_context_t *ftl)
{
    if (l2p_cache_is_thrashing(&ftl->cache))
    {
        return;
    }
    l2p_flush_kick(&ftl->cache, (uint8_t)g_bg_level_cfg[ftl->bg.level].l2p_flush_max);
}

/* 位 9 · 预算 — 推进 Journal 刷页 job（阶段 2；job 已在飞则空操作）。
 * BUSY 档被掩掉（host 饱和档不主动起页写；页满时 journal_append 自己会起）。 */
static void bg_a_journal_flush(ftl_context_t *ftl)
{
    journal_kick(&ftl->journal);
}

/* 位 10 · 预算 — BBT 持久化（阶段 7：攒批写 NAND BBT；job 起步/续跑，提交即返回） */
static void bg_a_bbt(ftl_context_t *ftl)
{
    bbt_kick(&ftl->alloc);
}

/* 位 11 · 预算 — Wear 持久化（阶段 7）。
 * ⚠ interval 保持 0：内部 OR 双门（WEAR_FLUSH_INTERVAL_MS 120s / 写增量≥1024，wear.c）
 *   为准——写增量触发是掉电窗口上界，不能被调度器 interval 拖死（唯一诚实例外）。 */
static void bg_a_wear(ftl_context_t *ftl)
{
    wear_kick(&ftl->alloc, ftl->total_writes);
}

/* 位 12 · 预算 — Checkpoint 触发检查（T1 Journal 水位 / T2 写入量 / T3 定时 elapsed）。
 * 语义同旧三档：定时基准无条件重置（触发即重置，不看成败）；job 装配由
 * checkpoint_begin（提交即返回），成败由 [CP] 心跳判。 */
static void bg_a_cp_trigger(ftl_context_t *ftl)
{
    uint32_t now_ms  = ftl_now_ms();
    uint32_t elapsed = now_ms - ftl->cp_timer_start;

    if (checkpoint_check_trigger(ftl, elapsed))
    {
        (void)checkpoint_begin(ftl);
        ftl->cp_timer_start = now_ms;
    }
}

/* 位 13 · 预算 — 慢死预测巡检（节流 1s；一次只搬一个 SB，防长时间阻塞）。仅 IDLE 档。
 * 内部门（rr_last_scan_ms / RR_SCAN_INTERVAL_MS）与调度器 interval=RR_SCAN_INTERVAL_MS
 * 双保险：时钟缺失降级下调度器 interval gate 失效，内部门兜底（与 v0.75 退化形态同构）。 */
static void bg_a_rr_predict(ftl_context_t *ftl)
{
    uint32_t now_ms = ftl_now_ms();

    if ((now_ms - ftl->rr_last_scan_ms) >= RR_SCAN_INTERVAL_MS)
    {
        ftl->rr_last_scan_ms = now_ms;
        (void)mig_begin_predict(ftl);
    }
}

/* ── 任务表 + 档位选择 ── */

/* ftl_bg_init — 填 14 项任务表（表序 = 优先级升序；引擎线性扫 ⇒ 掩码位下标 = 表下标）。
 * 由 ftl_init 在实例装配末尾调用一次。 */
void ftl_bg_init(ftl_context_t *ftl)
{
    bg_scheduler_t *bg = &ftl->bg;

    bg_scheduler_init(bg);

    /* must-run 正确性关键集（位 0..5：无条件、不 gate、不占预算） */
    uint8_t i             = 0U;
    bg->tasks[i].func     = bg_a_checkpoint;
    bg->tasks[i].must_run = 1U;
    i++;
    bg->tasks[i].func     = bg_a_rr;
    bg->tasks[i].must_run = 1U;
    i++;
    bg->tasks[i].func     = bg_a_mig_service;
    bg->tasks[i].must_run = 1U;
    i++;
    bg->tasks[i].func     = bg_a_metadata_dir;
    bg->tasks[i].must_run = 1U;
    i++;
    bg->tasks[i].func     = bg_a_journal_erase;
    bg->tasks[i].must_run = 1U;
    i++;
    bg->tasks[i].func     = bg_a_emergency_cp;
    bg->tasks[i].must_run = 1U;
    i++;

    /* 预算任务（位 6..13；interval / 优先级 / 单次钳制；掩码位 6..13 = 0x3FC0） */
    bg->tasks[i].func        = bg_a_gc_eval;
    bg->tasks[i].priority    = 0U;
    bg->tasks[i].interval_ms = GC_EVAL_INTERVAL_MS;
    bg->tasks[i].budget_us   = 400U;
    i++;
    bg->tasks[i].func      = bg_a_wl;
    bg->tasks[i].priority  = 1U;
    bg->tasks[i].budget_us = 300U;
    i++;
    bg->tasks[i].func      = bg_a_l2p_flush;
    bg->tasks[i].priority  = 2U;
    bg->tasks[i].budget_us = 1500U;
    i++;
    bg->tasks[i].func      = bg_a_journal_flush;
    bg->tasks[i].priority  = 3U;
    bg->tasks[i].budget_us = 700U;
    i++;
    bg->tasks[i].func      = bg_a_bbt;
    bg->tasks[i].priority  = 4U;
    bg->tasks[i].budget_us = 700U;
    i++;
    bg->tasks[i].func      = bg_a_wear;
    bg->tasks[i].priority  = 5U;
    bg->tasks[i].budget_us = 700U;
    i++;
    bg->tasks[i].func      = bg_a_cp_trigger;
    bg->tasks[i].priority  = 6U;
    bg->tasks[i].budget_us = 100U;
    i++;
    bg->tasks[i].func        = bg_a_rr_predict;
    bg->tasks[i].priority    = 7U;
    bg->tasks[i].interval_ms = RR_SCAN_INTERVAL_MS;
    bg->tasks[i].budget_us   = 300U;
    i++;

    bg->task_count = i;
}

/* ftl_bg_level_select — 负载 → 档位（纯函数，无副作用）。
 * 与 v0.75 三级分档**逐字等价**（见 ftl_service 注释）：inflight 全空 = IDLE；
 * pending > 半池 或 有未决组 = BUSY；中间 = NORMAL。 */
uint8_t ftl_bg_level_select(const ftl_context_t *ftl)
{
    uint16_t queued   = pending_group_count(ftl);
    uint16_t inflight = (uint16_t)(pending_count(ftl) + queued);

    if (inflight == 0U)
    {
        return BG_LEVEL_IDLE;
    }
    if ((pending_count(ftl) > (FTL_PENDING_MAX / 2U)) || (queued != 0U))
    {
        return BG_LEVEL_BUSY;
    }
    return BG_LEVEL_NORMAL;
}

/* ── 对外接口 ── */

/* --------------------------------------------------------------------
 * ftl_init
 *   功能：装配一个 FTL 实例——按依赖序初始化拓扑 / pending / BST / cache / allocator
 *         / journal / metadata_dir / bbt / wear / 各计时器。
 *   参数：ftl —— 实例（非 NULL，含自持表存储）；topo —— 绑定的 NAND 芯片路由。
 *   返回：FTL_OK / FTL_ERROR（初始化失败）。
 *   实现要点：表自持（P2-5，`ftl->tables`）；BST 的 memset 必须早于
 *             wear_init，否则计数回填被清零。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_init(ftl_context_t *ftl, ftl_topology_t topo)
{
    /* 前置条件（判据 2）：唯一调用者 main.c 传 &g_ftl[ch][ce][lun]。 */
    ASSERT(ftl != NULL);

    /* ├─ PBA 字段视图自检（一次）：位域布局 implementation-defined，无法用 _Static_assert
     *    证明（见 ftl_config.h §三.2）。若本工具链的布局与 PBA_*_SHIFT 不一致，
     *    强转（*(nand_pba_t *)&fields）会静默算错地址 ⇒ 在此硬失败。 */
    ASSERT(pba_fields_layout_matches());

    ftl->topo = topo;

    /* ⚠ 这里**不**调 completion_table_init()：旁表与实例路由表都是**全局**资源
     *    （completion_table.c 的 file-static），而本函数是**逐实例**调用的 ——
     *    在此清表会抹掉其它实例在飞的 ctx 登记，也会抹掉 main.c 已设好的路由表。
     *    该函数由 main.c 的 ssd_ftl_boot_all 在**所有实例 boot 之前**调用一次。
     *    （完成派发不需要在此注册 handler：命令自己带 op_tag，g_op_service[] 是
     *     编译期常量数组，见 ftl_op.c；旁表只回答"这条命令的私有状态在哪"。） */

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
        const ftl_status_t md_ret = metadata_dir_load(&ftl->metadata_dir);

        if (md_ret != FTL_OK)
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
     * 游标在此归零 ⇒ 首次 Checkpoint job 会先扫池定位（cursor_valid==0，见 CP_HDR_PREP）。 */
    memset(&ftl->cp, 0, sizeof(ftl->cp));
    ftl->cp_timer_start = 0U;

    /* ├─ 统计 ── */
    ftl->total_writes = 0U;

    /* ├─ 后台调度器任务表（v0.76：14 项；must-run + interval/budget/优先级）──
     * 必须在所有在飞的字段就绪后填（适配器只依赖实例字段与 kick，无额外依赖）。 */
    ftl_bg_init(ftl);

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
 *   功能：服务**单个** FTL 实例（Phase 2.5 补发子操作 + Phase 3 后台调度）。
 *   参数：ftl —— FTL 实例（**恒非空**：调用方须传合法实例地址，NULL 属编程错误）。
 *   返回：无。
 *   实现要点：调用方须**先**调 ftl_service_pump()（全局泵，每轮一次），再对**每个**
 *             实例调本函数（序 4：多实例下所有实例都要推进，不能只跑 [0][0][0]）。
 *             Phase 3 用 inflight（pending + groups）选档（bg_suspend 置位时整体跳过，
 *             `bbt --erase` 后保护），档位 → bg_scheduler_run 执行 14 项任务表。
 * -------------------------------------------------------------------- */
void ftl_service(ftl_context_t *ftl)
{
    /* 唯一调用点 main.c 传 &g_ftl[ch][ce][lun]（静态数组元素，结构性恒非空）；
     * NULL 属「不该发生」⇒ ASSERT（release 裁空后 NULL 解引用=HardFault 留现场，
     * 优于静默返回⇒该实例 journal/GC 永不推进的无声损坏）。 */
    ASSERT(ftl != NULL);

    /* Phase 2.5 首：推进**停泊**的写确认（阶段 5）。必须早于补发——停泊意味着那个 LBA
     * 的 L2P 还是旧映射，先补发一条读同一 LBA 的子操作就会读到旧物理页（陈旧数据）。
     * 顺序与 Phase 2.5 的既有约定一致：先处理**已完成**的，再下发新的。 */
    ftl_confirm_kick(ftl);

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

    /* Phase 3: 时间预算后台调度（v0.76）
     *
     * v0.75 三级 static 服务函数 → bg_scheduler 引擎 + 14 项任务表 + 档位→预算映射
     * （接线/引擎见本文件上部「后台调度器接线」与 bg_scheduler.h）。空闲度近似
     * （B3：基于未决命令数，非"本轮有无提交 NAND"代理）**逐字沿用**旧实现：
     *   旧实现用 submitted（本轮 fc_service 提交了几条）判定，host 无 IO 时 fc_service
     *   也可能返回非 0（后台命令推进）→ 误判 NORMAL/BUSY，GC/WL/CP 全推迟。现在用
     *   pending（host IO 未决）+ groups（未完成的 host 请求）近似系统空闲度：
     *   全空 = 真空闲 → IDLE 全力后台；有未完成 host 请求 = host 饿着 → BUSY 最少后台
     *   （让路，否则抢走 drain 要用的 FC buffer）；中间 → NORMAL 按需后台。
     *
     * ⚠ **阶段 4 起不再计入 completion_table**：后台 job（journal / RR / 搬移）的命令
     *   也登记在 completion_table 里，计进来会让「真空闲」永不成立——一个后台 job 在飞
     *   就把模式从 IDLE 顶到 NORMAL，后台反而被自己的命令饿死。后台 job 的活跃度由各自
     *   的「单飞行门」（journal.job.phase / rr.stage / mig.active）单独管，与 host 侧解耦。
     *
     * ⚠ bg_suspend 必须在此**最先** return（早于 must-run pass）：`bbt --erase` 之后
     *   介质已空而 RAM 状态陈旧，任何后台写（含 must-run 里的 journal/CP/元数据目录）
     *   都是往干净芯片上倒垃圾。Phase 1/2/2.5 已跑完（NAND 泵 + host IO 正常）。 */
    if (ftl->bg_suspend != 0U)
    {
        return;
    }

    /* 档位选择在 ftl_bg_level_select 内算（纯函数，inflight = pending + groups）；
     * checkpoint_kick 由 must-run 位 0 承接（引擎 Pass 0 最先执行），此处不再显式调。
     * ftl->bg.level 供 l2p flush 适配器反查条数上界（位 8）。 */
    uint8_t level = ftl_bg_level_select(ftl);
    ftl->bg.level = level;
    bg_scheduler_run(&ftl->bg, ftl, &g_bg_level_cfg[level]);
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
static bool ftl_wbuf_has(const ftl_context_t *ftl, lba_t internal_lba)
{
    if (ftl->wbuf.open == 0U)
    {
        return false;
    }
    if (ftl_lba_pair_base(internal_lba) != ftl->wbuf.base)
    {
        return false;
    }

    const uint8_t slot = (uint8_t)(ftl_lba_plane(internal_lba) * CW_NUM + ftl_lba_cw(internal_lba));
    return (ftl->wbuf.occupied & (uint8_t)(1U << slot)) != 0U;
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
    const lba_t window = ftl_lba_pair_base(internal_lba);

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
 * 返回 FTL_OK 已提交 / FTL_AGAIN 背压（缓冲保留，下次重试）/ FTL_ERROR 建链或搬数据失败
 * （阶段 4 起**不再**有"无空间"的 FTL_ERROR——取不到块改走 FTL_AGAIN + 起 GC job）。
 * ⚠ 缓冲只在**提交成功后**清空——失败时保留供重试；outstanding 已在 append 记过，此处不动。 */
static ftl_status_t ftl_wbuf_flush(ftl_context_t *ftl)
{
    if ((ftl->wbuf.open == 0U) || (ftl->wbuf.occupied == 0U))
    {
        return FTL_OK; /* 空：nothing to do */
    }

    /* 资源次序（IO-013）：**先查 pending 表余量，再动别的资源**。本函数末尾要用一个
     * pending 槽登记本次编程；若表已满却先把物理块和 desc 链配出来，那条链只能整条回滚
     * ——白配一场。单线程 superloop ⇒ 查到这里登记之间没有别的消费者。 */
    if (pending_count(ftl) >= (uint16_t)FTL_PENDING_MAX)
    {
        return FTL_AGAIN;
    }

    /* 分配新页（**逐 plane 独立选块**——Policy B）。Free Pool 耗尽 → **背压**：
     * 起一个 GC 搬移 job（若未在跑）后返回 FTL_AGAIN，缓冲原样保留，由 ftl_drain_group
     * 下轮重试。
     *
     * ⚠ **阶段 4 的行为变化**（原为"同步 GC 重试一次"）：搬移是异步 job，在完成回调/
     *   写路径里同步等它会把 superloop 卡死（一次搬移 = 64 页 × 2 plane 读+写 + 2 次擦）。
     *   改为背压后与既有 FTL_AGAIN 机制完全一致，也是主流做法（写路径不就地回收，
     *   取不到块就挂起主机命令）。**代价**：池满时写延迟升高（等一个 job 走完）。
     *   **死锁边界**：GC 自身搬移需要 free block ⇒ 靠 **OP 预留池**兜底（v0.74 已把 OP
     *   池改成按 plane 预留）；GC job 因池空而中止时（migrate.c 的 alloc 失败分支）
     *   不擦 victim、数据无损，下轮重试。
     *   ⚠ 这里**不再**返回 FTL_ERROR：无 victim 可回收只是"暂时没空间"，下轮可能就有
     *   （另一条写完成归还了块），报 IO_ERROR 会让 host 写永久失败。 */
    nand_pba_t new_pba[PLANE_NUM];
    if (allocator_alloc_pair(&ftl->alloc, FTL_TEMP_HOT, new_pba) != FTL_OK)
    {
        (void)mig_begin_gc(ftl);
        return FTL_AGAIN;
    }

    /* 本页覆盖的 plane 数（决定 operation 码与 plane 段）与实际 CW 数。
     * nplanes 的值域断言在 ftl_plane_write_op 内（界 = PLANE_NUM，与
     * fc_command_plane_span 同源）——不再在这里复检。 */
    uint8_t nplanes = ftl_cw_bitmap_plane_count((uint32_t)ftl->wbuf.occupied);
    uint8_t n_cw    = ftl_cw_bitmap_popcount((uint32_t)ftl->wbuf.occupied);
    ASSERT(n_cw > 0U);

    /* 分配 FC 命令槽（池空 = 瞬态） */
    fc_command_t *cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return FTL_AGAIN;
    }

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
    cmd->meta               = (DWORD)ftl->wbuf.base;

    /* 建 desc 链（位图 = 本页实际覆盖的 CW；desc/buffer 池空 → 瞬态背压） */
    ftl_status_t ret = ftl_cmd_setup_ppa_pair_cw(cmd, new_pba, (uint32_t)ftl->wbuf.occupied);
    if (ret != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return (ftl_status_t)ret; /* FTL_AGAIN（池空，瞬态）/ FTL_ERROR 原样上抛 */
    }

    /* 按页内 CW 偏移从整页镜像**散射**取数（稀疏页必须散射——顺序打包会把槽 0 的数据
     * 塞进第一个链槽而非它自己的 CW 偏移）。 */
    ret = ftl_cmd_write_data_cw(cmd, ftl->wbuf.data, (uint32_t)ftl->wbuf.occupied);
    if (ret != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    /* 注册 pending（owners = 贡献本页的组位掩码；lba = 窗口基址；buffer 传 NULL）。
     * 认领槽位后就地逐字段填——字段名即文档（见 ftl_pending_req_t 注释）。 */
    ftl_pending_req_t *pending = pending_alloc(ftl);

    if (pending == NULL)
    {
        (void)fc_command_release(cmd);
        return FTL_AGAIN; /* pending 表满：瞬态 */
    }

    pending->command_id         = ftl_cmd_id(cmd);
    pending->owners             = ftl->wbuf.owners;
    pending->op                 = (uint8_t)PENDING_OP_WRITE;
    pending->n_cw               = n_cw;
    pending->cw_bitmap          = ftl->wbuf.occupied;
    pending->lba                = ftl->wbuf.base;
    pending->buffer             = NULL;
    pending->new_pba            = new_pba[0];
    pending->new_pba1           = new_pba[1];
    pending->cache_intermediate = 0U;
    pending_validate(pending);

    /* 提交（tag = HOST_IO ⇒ 完成时由 g_op_service[FTL_OP_HOST_IO] 收尾）。
     * ctx 传 NULL：host I/O 的私有状态在 pending 表里（比旁表 ctx 更全）。 */
    if (ftl_op_submit(cmd, FTL_OP_HOST_IO, NULL) != FTL_OK)
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

    /* 夹到 pair 窗口：一个子操作不跨窗口（窗口内最多两个 plane 段、每段一个页）。
     * ⚠ L2P region 512 对齐且 lba_to_local 保留低 9 位 ⇒ 窗口绝不跨实例/region。 */
    uint8_t n;
    {
        const lba_t   window_end = ftl_lba_pair_base(internal_lba) + (lba_t)FTL_LBA_PER_PAIR;
        const uint8_t avail      = (uint8_t)(window_end - internal_lba);

        n = (*n_cw < avail) ? *n_cw : avail;
    }
    ASSERT(n > 0U);

    /* 写：攒进页打包缓冲（窗口满 / 切换窗口时内部触发编程），本函数对写**到此为止**。
     * 数据在 ftl_wbuf_flush 里一次编程（§七 方案 A）——分配/建链/提交/确认全在那里。 */
    if (!pending_op_is_read(g->op))
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

    nand_pba_t pba[PLANE_NUM];
    for (uint8_t i = 0U; i < PLANE_NUM; i++)
    {
        pba[i] = NAND_PBA_INVALID; /* 未覆盖的 plane 保持 INVALID（见 ftl_pending_req_t.new_pba 约定） */
    }

    uint32_t cw_bitmap = 0U; /* flat：bit = plane*CW_NUM + cw */
    {
        /* 逐 LBA 查 L2P 并按 (物理 plane, 页) 分组。收缩条件：
         *   · 该 LBA 仍在写缓冲里（未编程）—— 本 LBA 不进本子操作（下一轮从缓冲取）
         *   · 未映射（INVALID）—— 本 LBA 不进本子操作（下一轮单独处理）
         *   · region 尚未就绪（异步加载在飞）—— 本子操作到此为止，下轮再来
         *   · 同 plane 但换了页 —— 一条命令每 plane 只能一个页，装不下
         *   · 非法 PBA / 物理 plane ≠ LBA 的 plane 位 —— 映射表损坏 → FTL_ERROR */
        uint8_t k;

        for (k = 0U; k < n; k++)
        {
            const lba_t l = (lba_t)(internal_lba + (lba_t)k);
            nand_pba_t  p = NAND_PBA_INVALID; /* 越界（FTL_ERROR）保持此值 = 未映射 */

            if (ftl_wbuf_has(ftl, l))
            {
                break; /* 还在写缓冲（未编程）：收缩到此为止，下一轮同步从缓冲取 */
            }

            /* ⚠ 三态（阶段 5）：FTL_AGAIN = region 未驻留（已起异步加载）。**必须**与
             * "未映射"（FTL_OK + INVALID）分清——把前者当后者会让 host 读到假数据。
             * FTL_ERROR 仅越界，保持 p = INVALID（旧实现返回 INVALID，行为等价）。 */
            if (l2p_cache_read(&ftl->cache, l, &p) == FTL_AGAIN)
            {
                ftl->l2p_defer = 1U; /* 告知 drain：这是加载在飞，不是卡死 */
                break;               /* 本子操作到此为止；已凑到的 k 个 LBA 照常下发 */
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
            if (!pba_is_valid(p))
            {
                LOGE("[FTL] rd lba=%lu: L2P entry 0x%08lx is not a valid PBA\r\n",
                     (unsigned long)l,
                     (unsigned long)p);
                return FTL_ERROR;
            }

            uint8_t    pl = (uint8_t)pba_to_plane(p);
            nand_pba_t pg = pba_page(p);

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

            if ((pba[pl] != NAND_PBA_INVALID) && (!pba_same_page(pba[pl], p)))
            {
                break; /* 同 plane 跨页：本子操作到此为止，余下交给下一轮 */
            }

            pba[pl] = pg;
            cw_bitmap |= (uint32_t)1U << (((uint32_t)pl * CW_NUM) + (uint32_t)ftl_lba_cw(l));
        }

        if (k == 0U)
        {
            /* 首 LBA 未就绪（region 在加载）⇒ 本轮无进展，返背压等加载完成。
             * ⚠ 必须**先**判这一条：把"还不知道"当成"未映射"会给 host 填一页零。 */
            if (ftl->l2p_defer != 0U)
            {
                return FTL_AGAIN;
            }

            /* 首 LBA 未映射：同步返回全零，消费 1 个 LBA（不进 outstanding）。
             * ⚠ 只覆盖**自己**这一个 LBA：相邻 LBA 的映射与它无关，由 drain 下一轮处理。 */
            memset(buffer, 0, FTL_IU_BYTES);
            *n_cw = 1U;
            return FTL_OK;
        }
        n = k;
    }

    /* 本子操作覆盖的 plane 数（决定 operation 码与 plane 段）。
     * 值域断言在 ftl_plane_read_op 内（界 = PLANE_NUM）。 */
    uint8_t nplanes = ftl_cw_bitmap_plane_count(cw_bitmap);

    /* 资源次序（IO-013）：**先查 pending 表余量，再配 desc 链**。末尾要用一个 pending 槽
     * 登记本子操作；表满却先建好链，只能整条回滚——白配一场。
     * ⚠ 必须在 wbuf 命中分支**之后**：那条路径同步完成、不占 pending 槽，不该被表满挡住。 */
    if (pending_count(ftl) >= (uint16_t)FTL_PENDING_MAX)
    {
        return FTL_AGAIN;
    }

    /* 分配 FC 命令槽（池空 = 瞬态） */
    fc_command_t *cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return FTL_AGAIN;
    }

    /* 配置命令头（multi_plane 决定 plane 段，必须早于 ftl_cmd_setup_ppa_pair_cw——
     * 后者按段校验位图） */
    cmd->config.operation   = ftl_plane_read_op(nplanes);
    cmd->config.multi_plane = ftl_plane_multi_field(nplanes);
    cmd->config.cw          = CW_NUM - 1U;
    cmd->config.crc         = FC_CRC_ENABLE;
    cmd->config.random      = FC_RANDOM_ENABLE;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    /* 本命令的**窗口基址**（扇区 LBA，低 3 位清零）：FC 侧 desc->meta =
     * (meta & ~PLANE_CW_MASK) | (plane<<2) | cw ⇒ 逐 CW 铸出该 CW 的扇区 LBA。
     * 传窗口基址（而非 internal_lba）语义更直白，两者在 FC 掩掉低 3 位后等价。 */
    cmd->meta               = (DWORD)ftl_lba_pair_base(internal_lba);

    /* 建 desc 链（逐 plane 各自的 PBA，位图 = 本子操作实际覆盖的 CW）；
     * desc/buffer 池空 → 瞬态背压 */
    ftl_status_t ret = ftl_cmd_setup_ppa_pair_cw(cmd, pba, cw_bitmap);
    if (ret != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return (ftl_status_t)ret; /* FTL_AGAIN（池空，瞬态）/ FTL_ERROR 原样上抛 */
    }

    /* 注册 pending 子操作（必须早于 submit，避免竞态）。读：owners = 单组位、写专用字段传
     * INVALID、cw_bitmap 传 0；lba 传**片内 local**（L2P/Journal 均按 local 索引）。 */
    ftl_pending_req_t *pending = pending_alloc(ftl);

    if (pending == NULL)
    {
        (void)fc_command_release(cmd);
        return FTL_AGAIN; /* pending 表满：瞬态 */
    }

    pending->command_id         = ftl_cmd_id(cmd);
    pending->owners             = (uint32_t)(1U << gi);
    pending->op                 = (uint8_t)PENDING_OP_READ;
    pending->n_cw               = n;
    pending->cw_bitmap          = 0U;
    pending->lba                = internal_lba;
    pending->buffer             = buffer;
    pending->new_pba            = NAND_PBA_INVALID;
    pending->new_pba1           = NAND_PBA_INVALID;
    pending->cache_intermediate = 0U;
    pending_validate(pending);

    *n_cw = n; /* 回填实际消费的 LBA 数（调用者据此推进） */

    /* 记账（**必须早于 submit**——submit 之后 completion 可能立即被 drain 处理） */
    g->outstanding++;

    /* 提交（tag = HOST_IO；ctx 传 NULL —— 私有状态在 pending 表） */
    if (ftl_op_submit(cmd, FTL_OP_HOST_IO, NULL) != FTL_OK)
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
        uint8_t n_cw          = (remain >= (uint32_t)FTL_LBA_PER_PAIR) ? (uint8_t)FTL_LBA_PER_PAIR
                                                                       : (uint8_t)remain;

        ftl->l2p_defer        = 0U; /* 每次下发前清零：本轮的背压原因由本次下发重新判定 */
        const ftl_status_t st = ftl_issue_subop(ftl, gi, lba_to_local(glba), buffer, &n_cw);

        g = &ftl->groups[gi]; /* 重取：下发可能改变组状态 */

        if (st == FTL_OK)
        {
            g->stalls = 0U;
            g->next += n_cw;
            continue;
        }

        if (st == FTL_AGAIN)
        {
            /* 瞬态背压：本轮到此为止，下轮 ftl_service Phase 2.5 再补。
             * 连续无进展超预算 → 判真卡死，弃未下发者并报错（已下发的照常完成）。
             * ⚠ **L2P region 加载在飞不算无进展**（阶段 5）：那是有确定完成时刻的
             *   forward progress（NAND 层 wait/jump 超时保证命令必有完成），计进来会让
             *   一次正常的 region 加载吃掉 8 轮预算里的一轮 ⇒ 随机读满盘时把好组误判
             *   成卡死报 IO_ERROR。真卡死（命令丢失）由 NAND 层超时兜底。 */
            if (ftl->l2p_defer == 0U)
            {
                if (++g->stalls > FTL_DRAIN_STALL_MAX)
                {
                    LOGE("[FTL] drain stalled gi=%u next=%lu/%lu\r\n",
                         (unsigned)gi,
                         (unsigned long)g->next,
                         (unsigned long)g->total);
                    g->agg_status = SSD_STATUS_IO_ERROR;
                    g->next       = g->total;
                }
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
    if ((ftl == NULL) || (req == NULL) || (req->buffer == NULL))
    {
        return FTL_ERROR;
    }

    /* 长度 = 连续 LBA 数（上层 block_count）；FTL 自行决定单/双 plane 拆分 */
    uint32_t count = req->count;
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
    int gi = pending_group_alloc(ftl);
    if (gi < 0)
    {
        LOGE("[FTL] read: no free group slot\r\n");
        return FTL_ERROR;
    }
    ftl_io_group_t *g  = &ftl->groups[gi];
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
    if ((ftl == NULL) || (req == NULL) || (req->buffer == NULL))
    {
        return FTL_ERROR;
    }

    /* 长度 = 连续 LBA 数（上层 block_count）；FTL 自行决定单/双 plane 拆分 */
    uint32_t count = req->count;
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
    int gi = pending_group_alloc(ftl);
    if (gi < 0)
    {
        LOGE("[FTL] write: no free group slot\r\n");
        return FTL_ERROR;
    }
    ftl_io_group_t *g  = &ftl->groups[gi];
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
 *   参数：ftl —— 实例（**前置条件**，非法即 ASSERT）。
 *   返回：无。
 *   实现要点：**非** NVMe 式 barrier——不等待在飞 / 排队的写（CLI 靠 g_cli_pending.busy
 *             串行化）。
 * -------------------------------------------------------------------- */
void ftl_flush(ftl_context_t *ftl)
{
    /* 前置条件（判据 2 / C5）：唯一调用者 controller.c 在调用前已 ASSERT 路由结果非空。 */
    ASSERT(ftl != NULL);

    /* 语义 = "把已持久化映射落盘"：先把页打包缓冲里尚未编程的写编程掉（否则它们还没
     * 进 L2P/journal，"落盘"不成立），再 L2P cache 全部写回 + journal 尾页刷出。
     * **非** NVMe 式 barrier（不等待在飞/排队的写；当前 CLI 靠 g_cli_pending.busy 串行化）。
     * ⚠ wbuf flush 遇瞬态背压会保留缓冲、下轮 ftl_service 重试；此处尽力而为。 */
    (void)ftl_wbuf_flush(ftl);

    /* ⚠ 阶段 5 用**同步**逃生口（自旋泵）而非 kick：`fl` 是显式的"把映射落盘"请求，
     * 返回时脏 region 必须真的已持久（否则"落盘"这个词不成立——而它本就在 CLI 上下文，
     * 自旋不是嵌套泵）。后台攒批走 l2p_flush_kick（ftl_service 各档）。 */
    (void)l2p_cache_flush_all_sync(&ftl->cache);
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
    /* 前置条件（判据 2 / C5）：唯一调用者 controller.c 在调用前已 ASSERT 路由结果非空。 */
    ASSERT(ftl != NULL);

    if (count == 0U)
    {
        count = 1U; /* 兼容"单 LBA"调用 */
    }
    if ((lba + count) > FTL_LBA_MAX)
    {
        return;
    }

    /* 逐 LBA 取消映射（非阻塞、同步完成）。范围上界/分块后续再定。 */
    for (uint32_t i = 0U; i < count; i++)
    {
        nand_pba_t old_pba   = NAND_PBA_INVALID; /* 越界（FTL_ERROR）由 API 回填，此处再兜一层 */
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
        /* ⚠ 阶段 5：TRIM 走**同步**逃生口（自旋泵）——它由 CLI 调用，不在完成派发里，
         * 且契约是"取消映射同步完成"。读得 INVALID 与"未映射"同义，两种都是"无旧映射"。 */
        (void)l2p_cache_read_sync(&ftl->cache, local_lba, &old_pba);
        if (old_pba != NAND_PBA_INVALID)
        {
            uint16_t gblock = (uint16_t)ftl_pba_gblock(old_pba);
            ftl_block_sub_valid(&ftl->alloc.blocks[gblock]);
        }

        (void)l2p_cache_write_sync(&ftl->cache, local_lba, NAND_PBA_INVALID);

        /* ⚠ 取消映射也必须进 Journal（new_pba = INVALID）。L2P 的每一次变更都由
         * Journal 承载，掉电恢复靠回放重建——漏记这一条会让**掉电恢复把已 TRIM
         * 的 LBA 复活**：region 虽被 flush 成 INVALID，但回放按序重放更早的写条目
         * （page_seq 更小但 > cp_sequence）会把它改回有效 PBA，且回放行标 DIRTY
         * 后 ftl_boot_apply_region_bitmap 会跳过预载，NAND 上的 INVALID 也不生效。 */
        journal_append(&ftl->journal, local_lba, old_pba, NAND_PBA_INVALID);
    }
}
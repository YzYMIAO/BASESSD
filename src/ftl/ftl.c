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
 *   其余模块（gc / wl / journal / checkpoint / bbt / l2p_cache）各司其职，
 *   见各自文件头。
 *
 * 服务主循环（ftl_service）四阶段：
 *   Phase 1    fc_service × NAND_SUBMIT_BUDGET —— 推进 NAND
 *   Phase 2    completion_table_drain —— 统一完成泵（按命令 op_tag 派发到服务函数数组）
 *   Phase 2.5  逐组补发 host 子操作（host IO 优先于后台任务）
 *   Phase 3    ftl_bg_service —— 时间预算后台调度（本文件只有一行调用）
 *
 * 文件结构（与下方分区注释一一对应）：
 *   1. 内部辅助        ftl_confirm_write / ftl_host_io_callback（+ 4 个 ftl_host_io_* 完成
 *                      分支）/ pending_confirm_service
 *   2. 对外接口        ftl_init / ftl_service_pump / ftl_service
 *   3. 子操作下发      ftl_issue_subop / ftl_drain_group（static）+ ftl_submit_read / write
 *   4. 元数据落盘/TRIM ftl_flush / ftl_trim
 *
 * ⚠ 后台调度在 `ftl/bg_wiring.c`：10 项任务表 / 档位 IDLE·NORMAL·BUSY → 预算规模 /
 *   mig_gc_evaluate（GC 统一触发求值）/ RR 巡检 / Phase 3 入口 ftl_bg_service。
 *   通用引擎（零 kick 知识）在 `ftl/bg_scheduler.c`。本文件不持有任何后台策略。
 *
 * ⚠ 页打包写缓冲在 `ftl/wbuf.c`（wbuf.h 定义 wbuf_handle_t），本文件只调用其接口。
 *
 * ⚠ 回收动作在 `ftl/migrate.c`：GC / WL / 慢死预测共用一台异步搬移 job
 *   （mig_begin_* / migrate_handle）。触发求值在 `ftl/bg_wiring.c`，写路径的按需 GC 在
 *   ftl_issue_subop；本文件不直接触发搬移。
 *
 * ⚠ 多实例：pending / groups / rr_last_scan_ms 均 per-instance；completion 全局
 *   但按命令 ch/ce/lun 路由。main.c 主循环：先 ftl_service_pump()（全局泵，每轮一次）
 *   抽干 NAND + 完成表，再**遍历全部实例**调 ftl_service() 逐实例推进 Phase 2.5/3——
 *   只跑 [0][0][0] 会让其余实例的 journal / checkpoint / bbt / GC 永不推进。
 */

#include "ftl.h"

#include "assertion.h"
#include "bbt.h"
#include "checkpoint.h"
#include "completion_table.h"
#include "fc.h"
#include "ftl_fc_interface.h"
#include "ftl_op.h" /* ftl_op_submit / FTL_OP_* */
#include "log.h"
#include "pending.h"
#include "wbuf.h"

#include <string.h>

/* ── 内部辅助 ── */

/* 组下发机制（定义在文件后部；ftl_service Phase 2.5 先用到） */
static void ftl_drain_group(ftl_context_t *ftl, WORD gi);

/* --------------------------------------------------------------------
 * ftl_confirm_write
 *   功能：写子操作成功后的**逐 CW** 簿记——Journal + L2P + BST 有效计数/「写过」位。
 *   参数：ftl —— 实例；pending —— 已完成的写子操作记录。
 *   返回：FTL_OK 已全部应用；FTL_AGAIN 有 region 未就绪（**一条都没应用**，调用者下轮
 *         重来）。**只有这两态**——就绪前置挡住了唯一的失败源，第二遍不可能失败。
 *   实现要点：
 *     · CW 级映射下 1 LBA = 1 CW：一个子操作覆盖 pending->n_cw 个连续**全局** LBA，
 *       每个 LBA 各自一条 Journal + 一条 L2P 更新（Journal 流量 ×4，已登记）。
 *     · 目标页按 **LBA 的 plane 位**选：plane0 的 LBA 落 pending->new_pba、plane1 的
 *       落 pending->new_pba1。Policy B 下两 plane 独立选块，且「物理 plane 必须等于
 *       LBA 的 plane 位」是页内 META 能还原出正确 LBA 的前提（ftl_config.h §三）。
 *     · **不存 old_pba**：完成时按 LBA 重查 L2P 得"此刻被替换的映射"。同一 LBA 的两次
 *       写在飞时，先完成的先改 L2P，后完成的重查即得前者；提交时快照会让两条 Journal
 *       都指向最初映射，链断。⚠ **停泊没有削弱这条**：本函数仍是在**完成时刻**执行
 *       （停泊只是延后几轮，顺序仍按完成序），重查语义不变。
 *     · valid_count 单位是 **CW**：新 CW +1、被替换的旧 CW -1。真有效性一律逐 CW
 *       由页内 META + L2P 反查现判（DRAM-less approach ③，无位图）——见 bst.h 文件头。
 *     · ⚠ **两遍式**：第一遍只做**就绪前置**（region 未驻留则起异步加载并返
 *       FTL_AGAIN），第二遍才动账。绝不能边查边改——l2p_cache_write 对正在加载/写回
 *       的行返 FTL_AGAIN，中途放弃会让同一页里部分 CW 已记账、部分没记，重试时重复
 *       记账（valid_count 虚高、journal 重复条目、total_writes 多计）。
 *       两遍之间无泵（单线程顺序执行）⇒ 就绪判定与第二遍之间状态不可能变。
 * -------------------------------------------------------------------- */
static ftl_status_t ftl_confirm_write(ftl_context_t *ftl, const ftl_pending_req_t *pending)
{
    /* 两个调用点（ftl_host_io_callback 的 `if (pending != NULL)` 内 / pending_confirm_service 的
     * `&ftl->pending[i]`）都保证非空 ⇒ 不在本函数复检（§十附 / §二十六）。
     * 同理 pending->lba 恒为**窗口基址**（唯一写点是 wbuf_handle(FLUSH) 的 wbuf.base）——
     * 下面用 ftl_lba_plane/cw(l) 取代"从位下标反解坐标"就依赖这条。 */

    /* ── 第一遍：就绪前置（只探不动）── */
    for (BYTE bit = 0U; bit < FTL_LBA_PER_PAIR; bit++)
    {
        if ((pending->cw_bitmap & (1U << bit)) == 0U)
        {
            continue;
        }

        const DWORD lba = (pending->lba + bit);

        if (!l2p_cache_ready(ftl->cache, lba))
        {
            return FTL_AGAIN; /* 已起加载：本轮**一条都不动** */
        }
    }

    /* ── 第二遍：应用（此时不可能再返 FTL_AGAIN）── */
    /* 页打包：pending 描述**一整页**——pending->lba = 窗口基址，cw_bitmap 标记本页哪些
     * CW 有效，逐**置位**确认。逐 CW 的 LBA = base + bit（bit 与 LBA 低 3 位 [plane][cw]
     * 同构：bit = plane*CW_NUM + cw，即 ftl_lba_pair_slot）。plane/cw 由 **LBA** 取
     * （ftl_lba_plane/cw），不从位下标反解——坐标只有一个来源（见本函数头的 ASSERT）。 */
    for (BYTE bit = 0U; bit < FTL_LBA_PER_PAIR; bit++)
    {
        if ((pending->cw_bitmap & (1U << bit)) == 0U)
        {
            continue; /* 本槽未写（页打包后一页可只填部分 CW） */
        }

        const DWORD l    = (pending->lba + bit);
        const BYTE  pl   = ftl_lba_plane(l);
        const BYTE  cw   = ftl_lba_cw(l);
        const DWORD page = (pl == 0U) ? pending->new_pba : pending->new_pba1;

        /* 本页只为本命令覆盖到的 plane 分配了页（未覆盖的 plane 不进本循环体）。
         * 覆盖到的 plane 必有页（allocator_alloc_pair 两 plane 都给）。 */

        DWORD newp = pba_with_cw(page, cw);

        /* 就绪前置已保证驻留 ⇒ 读必命中、写必不返 FTL_AGAIN。非 OK 只剩"LBA 越界"
         * 一种可能（L2P 范围外）⇒ 按"未映射"处理并留一行日志。 */
        DWORD oldp;
        if (l2p_cache_read(ftl->cache, l, &oldp) != FTL_OK)
        {
            LOGE("[FTL] confirm: l2p read lba=%lu out of range\r\n", l);
            oldp = NAND_PBA_INVALID;
        }

        (void)journal_append(ftl->journal, l, oldp, newp);
        (void)l2p_cache_write(ftl->cache, l, newp);

        const WORD newg = ftl_pba_gblock(newp);

        ftl_block_add_valid(&ftl->alloc.blocks[newg]);

        /* 覆盖写：旧 CW 失效。旧页若正是本页（同一 CW 重写）则不动计数——
         * newp == oldp 只在"重写同一物理 CW"时出现，语义上是同一条映射。 */
        if ((oldp != NAND_PBA_INVALID) && (oldp != newp))
        {
            const WORD gob = ftl_pba_gblock(oldp);

            /* ⚠ **常驻守卫**（非临时探针，勿删）：L2P 指的旧块在 BST 里 0 有效 ⇒ 下一行
             * 必然撞 `bst.h` 的 `valid_count > 0U` 断言。这是「full metadata rebuild 漏认」类缺陷的
             * **唯一故障点**：重建只要静默跳过一个仍被 L2P 指向的块（坏块 / 元数据块判据
             * 过宽 / 页 0 判空），该块就落在所有池之外、valid_count 恒 0，且此后**每次**
             * 覆盖写该 LBA 都会死机——而重启本身看不出任何异常。
             * 打印旧块现场是为了**一次归因**（state 一眼可判，名字由 ftl_block_state_name 给）：
             *   BAD     = 坏块，重建按判据跳过（数据本已不可读）
             *   FREE    = 已归还空闲池 ⇒ 块还会被 allocator 复用，**丢数据**
             *   LOG     = 元数据日志块 ⇒ 重建判据过宽（元数据块的判定必须带 plane 掩码，
             *             只比 per-plane 块号会把跨 plane 的块误认成日志块）
             *   NONE    = 重建从未认领（页 0 判空 / 未扫描到）
             * 代价 = 每次覆盖写多一次比较；收益 = 把「重启后写一次就死机」变成一条可读日志。 */
            if (ftl->alloc.blocks[gob].valid_count == 0U)
            {
                const char *old_state = ftl_block_state_name(ftl_block_state(&ftl->alloc.blocks[gob]));
                const char *new_state = ftl_block_state_name(ftl_block_state(&ftl->alloc.blocks[newg]));

                (void)LOGE("[FTL] dangling lba=%lu old=%08lx g=%u st=%s sb=%u newg=%u nst=%s\r\n",
                           l,
                           oldp,
                           (unsigned)gob,
                           old_state,
                           (unsigned)ftl->alloc.blocks[gob].sb_id,
                           (unsigned)newg,
                           new_state);
            }

            /* 坏块 / count 已为 0 时跳过：bbt_mark_bad 清过账，再减会断言/回绕。
             * 上面的 dangling 日志已把“L2P 仍指向该块”这件事留痕。 */
            ftl_block_sub_valid_if_live(&ftl->alloc.blocks[gob]);
        }
    }

    journal_update_watermark(ftl->journal);
    ftl->total_writes++; /* 计「编程次数」（页打包后一页 = 一次编程），不是 CW 数 */
    return FTL_OK;
}

/* ── host I/O 完成的四个分支（ftl_host_io_callback 按 pending->op 与成败分派）── */

/* 读成功：按链序把数据拷给 caller。
 * desc 链序 == LBA 升序（plane-major + 段内 CW 升序，与 LBA 的 [plane][cw] 布局同序）
 * ⇒ 按链序连续拷出即 LBA 序。
 * ⚠ 返回值必须查：desc 链比请求短时尾部不写（ftl_cmd_read_data 只在这一种情况报错），
 *   不查就等于把未初始化缓冲当数据交给 host。失败 ⇒ 本次读的结果不可信，跳过随后的
 *   ECC / read-disturb 记账（记账无意义）。 */
static ssd_status_t ftl_host_io_read_ok(ftl_context_t     *ftl,
                                        fc_command_t      *cmd,
                                        ftl_pending_req_t *pending,
                                        WORD               cmd_id)
{
    DWORD data_size = FTL_IU_BYTES * pending->n_cw; /* 子操作字节数 */

    if (ftl_cmd_read_data(cmd, pending->buffer, data_size) != FTL_OK)
    {
        LOGE("[FTL] read data short lba=%lu cmd=%u\r\n", pending->lba, (unsigned)cmd_id);
        return SSD_STATUS_IO_ERROR;
    }

    /* Read Recovery：逐 plane 记录 ECC 与 read disturb，用于慢死预测。
     * ⚠ 不能只记 `ftl_cmd_pba()` 的首个 desc：一个 multi-plane 子操作在 Policy B 下
     *   覆盖**两个独立块**，第二块的趋势会漏记（RISK-HOST-02）。
     * ⚠ 须在 fc_command_release(cmd) 之前调用——desc 释放后失效。 */
    ftl_cmd_block_ecc_t blocks[PLANE_NUM];
    const BYTE          nblocks = ftl_cmd_block_ecc_list(cmd, blocks, PLANE_NUM);

    for (BYTE i = 0U; i < nblocks; i++)
    {
        rr_record_ecc(&ftl->alloc.blocks[blocks[i].gblock].rr, blocks[i].ecc_max);

        /* read disturb 计数：读成功累加（周期归零由上层维护）；
         * 随下一次 Checkpoint 的磨损快照持久化。 */
        ftl->alloc.blocks[blocks[i].gblock].read_count++;
    }

    return SSD_STATUS_OK;
}

/* 写成功（非 cache 中间页）：逐 CW 确认 Journal + L2P + 有效计数。
 * ⚠ region 未就绪（正在异步加载）⇒ **停泊**本项：命令脱钩（command_id 置哨兵，本命令
 *   随即 release，id 会被复用）+ 组在飞数**不**递减（host 写组继续 pending），由
 *   pending_confirm_service 每轮推进。与读失败的托管同构（命令脱钩、组不结算），区别是推进者
 *   在本文件而非 RR 引擎。
 * 返回 true = 已停泊 ⇒ 调用者**直接 release 并 return**（勿再动组簿记）。 */
static bool ftl_host_io_write_ok(ftl_context_t *ftl, ftl_pending_req_t *pending)
{
    if (ftl_confirm_write(ftl, pending) != FTL_AGAIN)
    {
        return false;
    }

    /* 停泊：命令脱钩、组在飞数不递减；停泊列表与超时归 pending 模块
     * （docs §5「Confirm → pending 服务」）。 */
    const WORD pending_idx = (pending - ftl->pending);

    pending_confirm_park(ftl, pending_idx, pending->owners);
    return true;
}

/* 读失败 ⇒ **托管**给 Read Recovery job（见 read_recovery.c）：
 *   · pending 项**保留**（不 pending_remove）、组在飞数**不**递减 ⇒ host 读组继续
 *     pending，等 RR 收尾时统一结算；
 *   · command_id 置哨兵：本命令随即 release，其 id 会被后续 fc_command_alloc 复用，
 *     留旧值会让新命令的完成误撞上本项；
 *   · 收尾（含一律报 IO_ERROR 的语义）在 rr_finish 里，见该处注释。
 * ⚠ 恢复**必须**异步：本函数跑在完成泵里，当场同步跑三级恢复就是泵里再开泵（嵌套泵），
 *   且整条 host 读流水线被一个坏页钉住。 */
static void ftl_host_io_read_fail(ftl_context_t *ftl, ftl_pending_req_t *pending)
{
    pending->op         = PENDING_OP_READ_RECOVERY;
    pending->command_id = FTL_PENDING_CMD_NONE;

    /* typed 触发：把 owner 交给 RR handle；RR busy 时该项仍留在 pending 表，
     * 当前 job 收尾后由 pending 端口的 take 领走（行为与旧扫描一致）。 */
    const WORD pending_idx = (pending - ftl->pending);
    rr_owner_t owner;

    owner.owners      = pending->owners;
    owner.pending_idx = pending_idx;
    owner.upper_buf   = (BYTE *)pending->buffer;
    owner.lba         = pending->lba;
    owner.n_cw        = pending->n_cw;

    rr_trigger(&ftl->read_recovery_handle, &owner);
}

/* 写失败：行业坏块替换流程
 *
 * ① bbt_mark_bad（RAM only，后台 bbt_handle 持久化）——先标坏，让 ② 关闭 SB 时把该
 *    plane 从 Closed 条目剔除。
 * ② 关闭当前 Open Super Block → 进 closed_sb_list；好 plane 随条目由 GC 回收。
 *    Policy B 下两 plane 是独立块，坏 plane 不再被 GC 读/擦。
 * ③ L2P 不动 → 自动回滚到旧映射，旧数据完好。
 *
 * 局限：本层拿不到逐 CW 失败状态，只标 plane0 的块；若失败的是 plane1，
 *       其块会在 GC 擦除失败时再标坏。 */
static void ftl_host_io_write_fail(ftl_context_t *ftl, ftl_pending_req_t *pending)
{
    const WORD gblock = ftl_pba_gblock(pending->new_pba);

    LOGE("[FTL] program failed lba=%lu gblock=%u\r\n", pending->lba, (unsigned)gblock);
    if (pending->new_pba != NAND_PBA_INVALID)
    {
        (void)bbt_mark_bad(&ftl->alloc, gblock);
    }
    (void)allocator_close_open_sb(&ftl->alloc, FTL_TEMP_HOT);
}

/* --------------------------------------------------------------------
 * ftl_host_io_callback
 *   功能：host 数据 I/O 子操作的完成服务函数（g_op_callbacks[FTL_OP_HOST_IO]）。
 *   参数：ftl —— 命令归属实例（ftl_op_dispatch 按命令自身 ch/ce/lun 解析后传入）；
 *         completion —— FC 完成状态；ctx —— 未用（host I/O 的私有状态在 pending 表，
 *         比任何旁表 ctx 都全，故该站点提交时 ctx 传 NULL）。
 *   返回：无。
 *   实现要点：按 {读,写}×{成功,失败} 分派到 4 个 ftl_host_io_* 分支（见各自注释）；
 *             其中读失败与写成功（region 未就绪）是**托管/停泊**路径——命令当场
 *             release 并 return，组在飞数不递减，由 RR 引擎 / pending_confirm_service 收尾。
 *             其余路径收尾 pending_remove + 组聚合，最后 release
 *             （**本服务函数拥有命令**）。
 *
 *             ⚠ 本函数**不得**同步等待（见 ftl_op.h）——它跑在完成泵里，再调
 *             ftl_sync_submit_wait 就是嵌套泵（泵里再开泵，完成顺序与栈深都失控）。
 * -------------------------------------------------------------------- */
void ftl_host_io_callback(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx)
{
    (void)ctx; /* host I/O 不登记旁表 ctx */

    /* command_id 由 NAND 从本命令回填；公共壳 fc_command_id_to_command 已断言
     * ID 在池内，恒返回非空指针。 */
    WORD          cmd_id = completion->command_id;
    fc_command_t *cmd    = fc_command_id_to_command(cmd_id);

    /* ftl 恒非空（路由表早于逐实例 boot 设好，见 ftl_op.h）；NULL = 编程错误 ⇒
     * ASSERT 而非 if-guard：release 下解引用留现场，比静默吞完成可定位。 */

    ftl_pending_req_t *pending = pending_lookup(ftl, cmd_id);

    if (pending != NULL)
    {
        ssd_status_t sub_status = SSD_STATUS_OK;

        /* 成败判据 = **全部**错误位（UNC / CRC / MATA_ERROR / NAND_FAIL / TIMEOUT / RAID），
         * 不是单看 NAND_FAIL：ECC 不可纠只置 UNC（fc_data_path.c 的 ECC 分支），
         * 页内 META 不符只置 MATA_ERROR——两者的 NAND_FAIL 都恒清，单看它会把
         * 「内容不可信」当成功。且 UNC 时 FC 跳过后面的解扰/修正 pass，缓冲里
         * 是**加扰垃圾**，搬给 host 就是静默数据损坏。判据见 fc_cw_status_has_error()。
         *
         * 分流依据是 pending->op（显式操作类型），不是 buffer——写请求的 buffer
         * 是数据源、恒非空，用 buffer 判别会把写全判成读。 */
        const bool is_read = pending_op_is_read(pending->op);

        if (!fc_cw_status_has_error(completion->status))
        {
            if (is_read)
            {
                sub_status = ftl_host_io_read_ok(ftl, cmd, pending, cmd_id);
            }
            else if (pending->cache_intermediate == 0U)
            {
                /* cache_intermediate 页跳过 L2P/Journal 更新，等最终 10h 确认。 */
                if (ftl_host_io_write_ok(ftl, pending))
                {
                    (void)fc_command_release(cmd);
                    return;
                }
            }
        }
        else if (is_read)
        {
            ftl_host_io_read_fail(ftl, pending);
            (void)fc_command_release(cmd);
            return;
        }
        else
        {
            ftl_host_io_write_fail(ftl, pending);
            sub_status = SSD_STATUS_IO_ERROR;
        }

        /* 子操作完成 → 组聚合：通知字段（complete_flag/status_out/
         * on_complete）只在组全部子操作完成后触发一次。
         * ⚠ 页打包下一页可能由**多个组**拼成 ⇒ 按 owners 位掩码逐组递减。
         * ⚠ owners 必须在 pending_remove **之前**取走：组完成通知会调 on_complete，
         *    回调可能重入 ftl_submit_* 并复用本槽 ⇒ 释放后再读就是读别人的字段。
         *    （另外两处结算点 read_recovery.c / pending_confirm_service 同此顺序。） */
        DWORD owners = pending->owners;

        pending_remove(ftl, cmd_id);
        pending_group_complete_subop_multi(ftl, owners, sub_status);
    }

    (void)fc_command_release(cmd);
}

/* pending 模块的 confirm handler：数据面事务仍在 FTL（Journal→L2P→BST 唯一顺序）。 */
static ftl_status_t ftl_confirm_handler(void *ctx, ftl_pending_req_t *pending)
{
    return ftl_confirm_write((ftl_context_t *)ctx, pending);
}

/* ── Journal 坏块端口（装配层适配：journal 不直接摸 allocator/BST） ── */

static bool ftl_journal_block_is_bad(void *ctx, WORD gblock)
{
    const ftl_allocator_t *alloc = (const ftl_allocator_t *)ctx;

    return ftl_block_is_bad(&alloc->blocks[gblock]);
}

static void ftl_journal_mark_bad(void *ctx, WORD gblock)
{
    ftl_allocator_t *alloc = (ftl_allocator_t *)ctx;

    (void)bbt_mark_bad(alloc, gblock);
}

/* ── Read Recovery 的 pending 端口（RR 不直接摸 pending 表） ── */

static bool ftl_rr_pending_take(void *ctx, rr_owner_t *out)
{
    ftl_context_t *ftl = (ftl_context_t *)ctx;
    BYTE           idx = 0U;

    if (!pending_find_op(ftl, PENDING_OP_READ_RECOVERY, &idx))
    {
        return false;
    }

    const ftl_pending_req_t *pending = &ftl->pending[idx];

    out->owners      = pending->owners;
    out->pending_idx = idx;
    out->upper_buf   = (BYTE *)pending->buffer;
    out->lba         = pending->lba;
    out->n_cw        = pending->n_cw;
    return true;
}

static void ftl_rr_pending_complete(void *ctx, WORD pending_idx, DWORD owners, ssd_status_t status)
{
    ftl_context_t *ftl = (ftl_context_t *)ctx;

    /* owners 已由 RR 在释放**之前**取走并按值传入（组完成通知可能重入复用本槽）。 */
    pending_release_index(ftl, pending_idx);
    pending_group_complete_subop_multi(ftl, owners, status);
}

/* ── Wbuf 端口（wbuf 不持有 ftl_context / pending 表） ── */

static WORD ftl_wbuf_pending_count(void *ctx)
{
    return pending_count((const ftl_context_t *)ctx);
}

static ftl_pending_req_t *ftl_wbuf_pending_alloc(void *ctx)
{
    return pending_alloc((ftl_context_t *)ctx);
}

static void ftl_wbuf_pending_remove(void *ctx, WORD command_id)
{
    pending_remove((ftl_context_t *)ctx, command_id);
}

static void ftl_wbuf_pending_validate(ftl_pending_req_t *pending)
{
    pending_validate(pending);
}

static void ftl_wbuf_group_add_subop(void *ctx, WORD gi)
{
    pending_group_add_subop((ftl_context_t *)ctx, gi);
}

static void ftl_wbuf_group_complete_multi(void *ctx, DWORD owners, ssd_status_t status)
{
    pending_group_complete_subop_multi((ftl_context_t *)ctx, owners, status);
}

/* 是否还有写组的下一个 LBA 落在本窗口（空闲 flush 的判据）。 */
static bool ftl_wbuf_group_window_pending(void *ctx, DWORD window)
{
    ftl_context_t *ftl = (ftl_context_t *)ctx;

    for (BYTE i = 0U; i < FTL_GROUP_MAX; i++)
    {
        const ftl_io_group_t *g = &ftl->groups[i];

        if ((g->used == 0U) || (!pending_op_is_write(g->op)) || (g->next >= g->total))
        {
            continue;
        }
        if (ftl_lba_pair_base((g->lba + g->next)) == window)
        {
            return true;
        }
    }

    return false;
}

static void ftl_wbuf_gc_urgent(void *ctx)
{
    ftl_context_t *ftl = (ftl_context_t *)ctx;

    migrate_trigger(ftl->migrate_handle, MIGRATE_REASON_GC_URGENT);
}

/* 统一的 wbuf 事件投递：适配器只构造 trigger，状态上报丢弃。 */
static ftl_status_t ftl_wbuf_event(ftl_context_t *ftl, const wbuf_trigger_t *trigger)
{
    wbuf_status_t status;

    return wbuf_handle(ftl->wbuf, trigger, NULL, &status);
}

/* ── 模块端口装配（每个 helper 只做一件事：建端口 + 绑定） ── */

static void ftl_bind_journal_port(ftl_context_t *ftl)
{
    journal_bad_block_port_t port;
    memset(&port, 0, sizeof(port));
    port.ctx          = &ftl->alloc;
    port.block_is_bad = ftl_journal_block_is_bad;
    port.mark_bad     = ftl_journal_mark_bad;

    journal_bind_bad_block_port(ftl->journal, &port);
}

static void ftl_bind_metadata_port(ftl_context_t *ftl)
{
    metadata_port_t port;
    memset(&port, 0, sizeof(port));
    port.alloc        = &ftl->alloc;
    port.total_writes = &ftl->total_writes;

    metadata_dir_bind_port(ftl->metadata_dir, &port);
}

static void ftl_bind_migrate_port(ftl_context_t *ftl)
{
    migrate_port_t port;
    memset(&port, 0, sizeof(port));
    port.alloc        = &ftl->alloc;
    port.journal      = ftl->journal;
    port.cache        = ftl->cache;
    port.total_writes = &ftl->total_writes;
    port.topo         = ftl->topo;

    migrate_init(ftl->migrate_handle, &port);
}

static void ftl_bind_rr_port(ftl_context_t *ftl)
{
    rr_pending_port_t pending;
    memset(&pending, 0, sizeof(pending));
    pending.ctx      = ftl;
    pending.take     = ftl_rr_pending_take;
    pending.complete = ftl_rr_pending_complete;

    rr_port_t port;
    memset(&port, 0, sizeof(port));
    port.cache        = ftl->cache;
    port.alloc        = &ftl->alloc;
    port.topo         = ftl->topo;
    port.page_scratch = ftl->rr_page_scratch;
    port.pending      = pending;

    rr_init(&ftl->read_recovery_handle, &port);
}

static void ftl_bind_wbuf_port(ftl_context_t *ftl)
{
    wbuf_port_t port;
    memset(&port, 0, sizeof(port));
    port.alloc                = &ftl->alloc;
    port.pending_ctx          = ftl;
    port.pending_count        = ftl_wbuf_pending_count;
    port.pending_alloc        = ftl_wbuf_pending_alloc;
    port.pending_remove       = ftl_wbuf_pending_remove;
    port.pending_validate     = ftl_wbuf_pending_validate;
    port.group_add_subop      = ftl_wbuf_group_add_subop;
    port.group_complete_multi = ftl_wbuf_group_complete_multi;
    port.group_window_pending = ftl_wbuf_group_window_pending;
    port.migrate_ctx          = ftl;
    port.gc_urgent            = ftl_wbuf_gc_urgent;

    wbuf_init(ftl->wbuf, &port);
}

static void ftl_bind_checkpoint_port(ftl_context_t *ftl)
{
    checkpoint_port_t port;
    memset(&port, 0, sizeof(port));
    port.cache        = ftl->cache;
    port.journal      = ftl->journal;
    port.metadata_dir = ftl->metadata_dir;
    port.alloc        = &ftl->alloc;
    port.total_writes = &ftl->total_writes;
    port.topo         = ftl->topo;

    ftl->checkpoint_handle->port = port;
}

/* ── 对外接口 ── */

/* --------------------------------------------------------------------
 * ftl_init
 *   功能：装配一个 FTL 实例——按依赖序初始化拓扑 / pending / BST / allocator
 *         / journal / metadata_dir / bbt / 各计时器。
 *   参数：ftl —— 实例（非 NULL，含自持表存储）；topo —— 绑定的 NAND 芯片路由；
 *         cache —— 本实例所属 (ch,ce) 的**共享** L2P cache（设备级已初始化，
 *                  本函数只绑定指针，不 init、不 set_md_dir）。
 *   返回：FTL_OK / FTL_ERROR（初始化失败）。
 *   实现要点：本实例只持有指针，storage 数组/挂载块由 HIL 提供，
 *             位于 NOLOAD 段 ⇒ 本函数必须显式 memset。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_init(ftl_context_t *ftl, ftl_topology_t topo, l2p_cache_t *cache, const ftl_storage_t *storage)
{
    /* 前置条件（判据 2）：唯一调用者 main.c 传 &g_ftl[ch][ce][lun]、共享 cache、storage。 */
    ASSERT(ftl != NULL);
    ASSERT(cache != NULL);
    ASSERT(storage != NULL);
    ASSERT(storage->tables != NULL);
    ASSERT(storage->metadata_dir != NULL);
    ASSERT(storage->journal != NULL);
    ASSERT(storage->checkpoint_handle != NULL);
    ASSERT(storage->migrate != NULL);
    ASSERT(storage->wbuf != NULL);
    ASSERT(storage->rr_page_scratch != NULL);

    /* ├─ PBA 字段视图自检（一次）：位域布局 implementation-defined，无法用 _Static_assert
     *    证明（见 ftl_config.h §三.2）。若本工具链的布局与 PBA_*_SHIFT 不一致，
     *    强转（*(DWORD *)&fields）会静默算错地址 ⇒ 在此硬失败。 */
    ASSERT(pba_fields_layout_matches());

    ftl->topo = topo;

    /* ├─ 绑定外部存储（storage 数组/挂载块由 HIL 按域提供）──
     * ⚠ .sram*_data 是 NOLOAD，启动代码不清零 ⇒ 下面必须整表 memset。 */
    ftl->tables            = storage->tables;
    ftl->metadata_dir      = storage->metadata_dir;
    ftl->journal           = storage->journal;
    ftl->checkpoint_handle = storage->checkpoint_handle;
    ftl->migrate_handle    = storage->migrate;
    ftl->wbuf              = storage->wbuf;
    ftl->rr_page_scratch   = storage->rr_page_scratch;

    /* ⚠ 这里**不**调 completion_table_init()：旁表与实例路由表都是**全局**资源
     *    （completion_table.c 的 file-static），而本函数是**逐实例**调用的 ——
     *    在此清表会抹掉其它实例在飞的 ctx 登记，也会抹掉 main.c 已设好的路由表。
     *    该函数由 main.c 的 ssd_ftl_boot_all 在**所有实例 boot 之前**调用一次。
     *    （完成派发不需要在此注册 handler：命令自己带 op_tag，g_op_callbacks[] 是
     *     编译期常量数组，见 ftl_op.c；旁表只回答"这条命令的私有状态在哪"。） */

    /* ├─ Pending Request 表（per-instance） ──
     * confirm 停泊的推进归 pending 模块；事务 handler 由 FTL 注入一次。 */
    pending_init(ftl);
    pending_confirm_set_handler(ftl_confirm_handler);

    /* ├─ storage 数组显式清零（启动不碰 .sram*_data；Phase 1.5 挂载块同）──
     * tables：allocator_init 逐块读 ftl_block_is_bad() ⇒ blocks 必须先清零；
     *         free/op/spare/closed 的读者按 count 有界，整表清零让 NOLOAD 语义确定。
     * metadata_dir：load/init 不覆盖全部字段 ⇒ 先整表清零。
     * journal：init 只重置部分 job 字段 ⇒ 先整表清零。
     * checkpoint_handle：active 在 cp_begin 自己 memset 之前就会被读 ⇒ 必须先清零。
     * migrate / wbuf / rr_page_scratch：Phase 1.5 挂载块，与其余数组一致清零。
     * ⚠ 全部 memset 必须早于 metadata_dir_load。 */
    memset(ftl->tables, 0, sizeof(*ftl->tables));
    memset(ftl->metadata_dir, 0, sizeof(*ftl->metadata_dir));
    memset(ftl->journal, 0, sizeof(*ftl->journal));
    memset(ftl->checkpoint_handle, 0, sizeof(*ftl->checkpoint_handle));
    /* Phase 1.5 挂载块：与其余 storage 数组一致显式清零（未来若落 NOLOAD 段，
     * 启动代码不清零）。 */
    memset(ftl->migrate_handle, 0, sizeof(*ftl->migrate_handle));
    memset(ftl->wbuf, 0, sizeof(*ftl->wbuf));
    memset(ftl->rr_page_scratch, 0, FTL_READ_USER_SIZE);

    /* ├─ 绑定共享 L2P Cache（设备级已初始化一次；metadata 依赖由 owner 解析）── */
    ftl->cache = cache;

    /* ├─ Allocator（各指针绑到 tables） ── */
    allocator_init(&ftl->alloc, ftl->topo, ftl->tables);

    /* ├─ Journal ── */
    journal_init(ftl->journal, ftl->topo);
    /* 持续擦除失败的环形块要标进 BBT（RISK-JN-01）；journal 通过端口访问坏块账本，
     * 不直接持有 allocator（docs/design/ftl_module_contracts.md §4.2）。 */
    ftl_bind_journal_port(ftl);

    /* ├─ Metadata Directory ── */
    ftl->metadata_dir->topo = ftl->topo;
    {
        const ftl_status_t md_ret = metadata_dir_load(ftl->metadata_dir);

        /* 失败时仍建空 Directory（空片首启 / 格式化路径需要），但把失败状态留给
         * ftl_boot_recovery：介质若已有有效 Checkpoint，空 Directory = 映射整体丢失
         * ⇒ 必须拒绝启动（RISK-BOOT-09）。 */
        ftl->md_load_failed = (md_ret != FTL_OK) ? 1U : 0U;

        if (md_ret != FTL_OK)
        {
            metadata_dir_init(ftl->metadata_dir);
        }
    }

    /* 目录整理 job 的端口：allocator + total_writes（docs §4.3）。 */
    ftl_bind_metadata_port(ftl);

    /* ├─ 搬移引擎（GC / WL / 慢死预测）端口 + 自持节拍 ── */
    ftl_bind_migrate_port(ftl);

    /* ├─ Read Recovery 端口（L2P / allocator / 整页暂存 / pending 端口）── */
    ftl_bind_rr_port(ftl);

    /* ├─ Wbuf（alloc / pending / migrate 端口）── */
    ftl_bind_wbuf_port(ftl);

    /* ├─ BBT 初始化（从 NAND 加载或出厂扫描）── */
    bbt_init(&ftl->alloc);

    /* ├─ Checkpoint 端口（l2p / journal / metadata / allocator / total_writes / topo）──
     * handle 已在上面整体清零 ⇒ 池游标 cursor_valid==0，首次 job 会先扫池定位。
     * ⚠ 端口必须在 memset(handle) **之后**绑定。 */
    ftl_bind_checkpoint_port(ftl);

    /* ├─ 统计 ── */
    ftl->total_writes = 0U;

    /* ├─ 后台调度器任务表（10 项；must-run + interval/budget）──
     * 必须在所有在飞的字段就绪后装载（适配器只依赖实例字段与 kick，无额外依赖）。 */
    ftl_bg_init(ftl);

    return FTL_OK;
}

/* ── 初始化契约自检（debug-only）──
 *
 * 只在 ftl_init + 设备级 owner 登记完成后跑一次，证明“初始化后不可变”的绑定。
 * 覆盖：句柄、storage 数组、端口回调、topology 副本、PBA 布局、bg 任务表、
 *       L2P owner / completion route 身份、各 job 的初始 idle 状态。
 *
 * ⚠ 边界（不要据此删运行期断言）：
 *   · 不覆盖运行期可变状态（phase 重启、计数、池成员、valid_count）；
 *   · 不覆盖每次调用的入参前置条件（out 指针、enum/index、cmd/desc）；
 *   · release（NDEBUG）下本组函数整体不存在，不能替代 release 拒绝（判据 9）。
 *   · 下方 helper 全部包在 #if !defined(NDEBUG) 内，避免 release 的 unused 告警。
 *   · 审计通过后，下游对同一绑定的逐次复检已删除——这些绑定视为生命周期内不可变。
 */
#if !defined(NDEBUG)

/* 每个 ASSERT 按模块聚合：一条断言覆盖同一生命周期的一组字段。
 * 失败时 file:line 定位到模块组；需要逐字段定位时临时拆分。 */

static void contract_check_handles(const ftl_context_t *ftl)
{
    ASSERT((ftl->cache != NULL) && (ftl->tables != NULL) && (ftl->metadata_dir != NULL) &&
           (ftl->journal != NULL) && (ftl->checkpoint_handle != NULL) &&
           (ftl->migrate_handle != NULL) && (ftl->wbuf != NULL) && (ftl->rr_page_scratch != NULL));

    ASSERT((ftl->alloc.blocks == ftl->tables->blocks) &&
           (ftl->alloc.free_blocks == ftl->tables->free_blocks) &&
           (ftl->alloc.op_blocks == ftl->tables->op_blocks) &&
           (ftl->alloc.spare_blocks == ftl->tables->spare_blocks) &&
           (ftl->alloc.closed_sb_list == ftl->tables->closed_sb_list));
}

static void contract_check_topology(const ftl_context_t *ftl)
{
    const ftl_topology_t *topo = &ftl->topo;
    const DWORD           dim  = ftl_topo_dim(*topo);

    ASSERT((topo->ch < CH_NUM) && (topo->ce < CE_NUM) && (topo->lun < LUN_NUM) &&
           (dim < FTL_DIM_TOTAL) && (ftl_dim_ch(dim) == topo->ch) &&
           (ftl_dim_ce(dim) == topo->ce) && (ftl_dim_lun(dim) == topo->lun));

    ASSERT((ftl->alloc.ch == topo->ch) && (ftl->alloc.ce == topo->ce) &&
           (ftl->alloc.lun == topo->lun) && (ftl->cache->ch == topo->ch) &&
           (ftl->cache->ce == topo->ce));

    /* 各模块在 init 时拷贝 topology；副本必须与实例路由一致。 */
    ASSERT((ftl->metadata_dir->topo.ch == topo->ch) &&
           (ftl->metadata_dir->topo.ce == topo->ce) &&
           (ftl->metadata_dir->topo.lun == topo->lun) &&
           (ftl->journal->topo.ch == topo->ch) && (ftl->journal->topo.ce == topo->ce) &&
           (ftl->journal->topo.lun == topo->lun));

    ASSERT((ftl->checkpoint_handle->port.topo.ch == topo->ch) &&
           (ftl->checkpoint_handle->port.topo.ce == topo->ce) &&
           (ftl->checkpoint_handle->port.topo.lun == topo->lun) &&
           (ftl->migrate_handle->port.topo.ch == topo->ch) &&
           (ftl->migrate_handle->port.topo.ce == topo->ce) &&
           (ftl->migrate_handle->port.topo.lun == topo->lun) &&
           (ftl->read_recovery_handle.port.topo.ch == topo->ch) &&
           (ftl->read_recovery_handle.port.topo.ce == topo->ce) &&
           (ftl->read_recovery_handle.port.topo.lun == topo->lun));
}

static void contract_check_ports(const ftl_context_t *ftl)
{
    const metadata_port_t *md = &ftl->metadata_dir->port;

    ASSERT((md->alloc == &ftl->alloc) && (md->total_writes == &ftl->total_writes));

    const journal_bad_block_port_t *jbb = &ftl->journal->bad_block_port;

    ASSERT((jbb->ctx == &ftl->alloc) && (jbb->block_is_bad != NULL) && (jbb->mark_bad != NULL));

    const checkpoint_port_t *cp = &ftl->checkpoint_handle->port;

    ASSERT((cp->cache == ftl->cache) && (cp->journal == ftl->journal) &&
           (cp->metadata_dir == ftl->metadata_dir) && (cp->alloc == &ftl->alloc) &&
           (cp->total_writes == &ftl->total_writes));

    const migrate_port_t *mg = &ftl->migrate_handle->port;

    ASSERT((mg->alloc == &ftl->alloc) && (mg->journal == ftl->journal) &&
           (mg->cache == ftl->cache) && (mg->total_writes == &ftl->total_writes));

    const rr_port_t *rr = &ftl->read_recovery_handle.port;

    ASSERT((rr->cache == ftl->cache) && (rr->alloc == &ftl->alloc) &&
           (rr->page_scratch == ftl->rr_page_scratch) && (rr->pending.ctx == ftl) &&
           (rr->pending.take != NULL) && (rr->pending.complete != NULL));

    const wbuf_port_t *wb = &ftl->wbuf->port;

    ASSERT((wb->alloc == &ftl->alloc) && (wb->pending_ctx == ftl) &&
           (wb->pending_count != NULL) && (wb->pending_alloc != NULL) &&
           (wb->pending_remove != NULL) && (wb->pending_validate != NULL) &&
           (wb->group_add_subop != NULL) && (wb->group_complete_multi != NULL) &&
           (wb->group_window_pending != NULL) && (wb->migrate_ctx == ftl) &&
           (wb->gc_urgent != NULL));
}

static void contract_check_identity(const ftl_context_t *ftl)
{
    const ftl_topology_t topo = ftl->topo;
    const DWORD          dim  = ftl_topo_dim(topo);

    ASSERT(ftl->cache->owner_of_region != NULL);
    ASSERT(completion_table_route(topo.ch, topo.ce, topo.lun) == ftl);

    /* 只查本实例拥有的 region：其他 LUN 的 owner 可能尚未 boot。 */
    for (DWORD local = 0U; local < L2P_REGIONS_PER_INSTANCE; local++)
    {
        const DWORD        global = (local * FTL_DIM_TOTAL) + dim;
        const l2p_owner_t *owner =
            ftl->cache->owner_of_region(ftl->cache->owner_ctx, global);
        const QWORD lba = (QWORD)global * L2P_ENTRIES_PER_REGION;

        ASSERT((owner != NULL) && (owner->dim == dim) &&
               (owner->metadata_dir == ftl->metadata_dir) && (owner->alloc == &ftl->alloc) &&
               (owner->topo.ch == ftl->cache->ch) && (owner->topo.ce == ftl->cache->ce) &&
               (owner->topo.lun == ftl->topo.lun) && (ftl_topo_dim(owner->topo) == dim));

        ASSERT((completion_table_route_region(global) == ftl) &&
               (lba_route_ch(lba) == ftl->cache->ch) &&
               (lba_route_ce(lba) == ftl->cache->ce));
    }
}

static void contract_check_bg(const ftl_context_t *ftl)
{
    ASSERT((ftl->bg.task_count > 0U) && (ftl->bg.task_count <= BG_TASK_COUNT) &&
           (ftl->bg.cfg == NULL) && (ftl->bg.level == BG_LEVEL_IDLE));

    for (BYTE i = 0U; i < ftl->bg.task_count; i++)
    {
        ASSERT(ftl->bg.tasks[i].func != NULL);
    }
}

static void contract_check_jobs_idle(const ftl_context_t *ftl)
{
    const journal_handle_t *j = &ftl->journal->handle;

    ASSERT((j->phase == JRN_IDLE) && (j->current_block < JOURNAL_BLOCK_COUNT) &&
           (j->current_page < PAGES_PER_BLOCK) && (j->current_entry == 0U) &&
           (j->sequence_counter == 1U) && (j->merge_floor == 0U) && (j->need_erase == 0U) &&
           (j->checkpoint_needed == 0U) && (j->emergency == 0U) &&
           (j->write_fail_count == 0U) && (j->write_fail_pending == 0U) &&
           (j->pre_erased_block == JOURNAL_BLOCK_NONE) && (j->bad_mask == 0U) &&
           (j->erase_fail_count == 0U) && (j->ring_full == 0U));

    const checkpoint_handle_t *cp = ftl->checkpoint_handle;

    ASSERT((cp->active == 0U) && (cp->waiting == 0U) && (cp->failed == 0U) &&
           (cp->phase == CP_IDLE) && (cp->cp_state.advance_next == 0U) &&
           (cp->cp_state.cursor_valid == 0U));

    ASSERT((!metadata_dir_busy(ftl->metadata_dir)) &&
           (ftl->metadata_dir->count <= METADATA_DIR_ENTRIES) &&
           ((!ftl->metadata_dir->cur_block_valid) ||
            ((ftl->metadata_dir->cur_metadata_block < FTL_BLOCK_TOTAL) &&
             (ftl->metadata_dir->cur_metadata_page <= PAGES_PER_BLOCK))));

    ASSERT((!ftl->migrate_handle->active) && (!ftl->migrate_handle->waiting) &&
           (ftl->migrate_handle->phase == MIG_IDLE) &&
           migrate_fail_state_clean(ftl->migrate_handle));

    ASSERT((ftl->read_recovery_handle.stage == RR_STAGE_IDLE) &&
           (ftl->read_recovery_handle.waiting == 0U) &&
           (ftl->read_recovery_handle.has_owner == 0U));

    ASSERT((!ftl->wbuf->open) && (ftl->wbuf->occupied == 0U) && (ftl->wbuf->error == FTL_OK));
}

static void contract_check_boot_zero(const ftl_context_t *ftl)
{
    ASSERT((pending_count(ftl) == 0U) && (pending_group_count(ftl) == 0U) &&
           (ftl->total_writes == 0U) && (ftl->cache->load_inflight == 0U) &&
           (ftl->cache->flush_inflight == 0U) && (ftl->bg_suspend == 0U) &&
           (ftl->l2p_defer == 0U));
}

#endif /* !NDEBUG */

void ftl_contract_audit(const ftl_context_t *ftl)
{
#if defined(NDEBUG)
    (void)ftl; /* release：审计不存在，行为与今天一致 */
#else
    ASSERT(ftl != NULL);
    ASSERT(pba_fields_layout_matches());

    contract_check_handles(ftl);
    contract_check_topology(ftl);
    contract_check_ports(ftl);
    contract_check_identity(ftl);
    contract_check_bg(ftl);
    contract_check_jobs_idle(ftl);
    contract_check_boot_zero(ftl);

    LOGI("[FTL ] contract ok ch=%u ce=%u lun=%u regions=%u tasks=%u\r\n",
         (unsigned)ftl->topo.ch,
         (unsigned)ftl->topo.ce,
         (unsigned)ftl->topo.lun,
         (unsigned)L2P_REGIONS_PER_INSTANCE,
         (unsigned)ftl->bg.task_count);
#endif
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
    for (BYTE i = 0U; i < NAND_SUBMIT_BUDGET; i++)
    {
        if (fc_service() != FC_STATUS_OK)
        {
            break;
        }
    }

    /* Phase 2: 统一完成泵（按命令 op_tag 派发到服务函数数组，见 completion_table.c） */
    completion_table_drain();
}

/* --------------------------------------------------------------------
 * ftl_service
 *   功能：服务**单个** FTL 实例（Phase 2.5 补发子操作 + Phase 3 后台调度）。
 *   参数：ftl —— FTL 实例（**恒非空**：调用方须传合法实例地址，NULL 属编程错误）。
 *   返回：无。
 *   实现要点：调用方须**先**调 ftl_service_pump()（全局泵，每轮一次），再对**每个**
 *             实例调本函数（多实例下所有实例都要推进，不能只跑 [0][0][0]）。
 *             Phase 3 收缩为一次 ftl_bg_service——档位选择 / 任务表 / 预算规模全在
 *             `ftl/bg_wiring.c`（含 bg_suspend 判定，见该处头注）。
 * -------------------------------------------------------------------- */
void ftl_service(ftl_context_t *ftl)
{
    /* 唯一调用点 main.c 传 &g_ftl[ch][ce][lun]（静态数组元素，结构性恒非空）；
     * NULL 属「不该发生」⇒ ASSERT（release 裁空后 NULL 解引用=HardFault 留现场，
     * 优于静默返回⇒该实例 journal/GC 永不推进的无声损坏）。 */

    /* Phase 2.5 首：推进**停泊**的写确认。必须早于补发——停泊意味着那个 LBA
     * 的 L2P 还是旧映射，先补发一条读同一 LBA 的子操作就会读到旧物理页（陈旧数据）。
     * 顺序与 Phase 2.5 的既有约定一致：先处理**已完成**的，再下发新的。 */
    pending_confirm_service(ftl, NULL);

    /* Phase 2.5: 补发 host 子操作。完成先腾出 FC 资源（命令/desc/buffer 池、SQ 空位），
     * 再把各组的待下发子操作补上；顺序保证 host IO 优先于 Phase 3 的后台任务。
     * 只遍历**本实例**的组（每实例一份 groups 表）。 */
    for (BYTE i = 0U; i < FTL_GROUP_MAX; i++)
    {
        if (ftl->groups[i].used != 0U)
        {
            ftl_drain_group(ftl, i);
        }
    }

    /* Phase 2.5 尾：页打包写缓冲的**确定性空闲 flush**——补发循环跑完后，若再无写组会
     * 向当前窗口 append，就把缓冲编程掉（否则这些 host 写永不完成）。放在 bg_suspend
     * 判定之前：它是 host IO 的收尾，不是后台任务。 */
    {
        wbuf_trigger_t trigger;
        memset(&trigger, 0, sizeof(trigger));
        trigger.kind = WBUF_TRIGGER_MAYBE_FLUSH_IDLE;
        (void)ftl_wbuf_event(ftl, &trigger);
    }

    /* Phase 3: 时间预算后台调度 —— 档位选择 / 10 项任务表 / 预算规模 / bg_suspend
     * 判定全在 ftl/bg_wiring.c（本文件不持有后台策略）。 */
    ftl_bg_service(ftl);
}

/* ── 子操作下发（一条 host 请求 → 多个 FC 子操作）──
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
 *   参数：ftl —— 实例；gi —— 组下标；internal_lba —— **全局** host LBA（扇区号，
 *         数据通路全程用全局 LBA，不预转 local）；
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
static ftl_status_t ftl_issue_subop(ftl_context_t *ftl, WORD gi, DWORD internal_lba, void *buffer, BYTE *n_cw)
{
    ftl_io_group_t *g = &ftl->groups[gi];

    /* 夹到 pair 窗口：一个子操作不跨窗口（窗口内最多两个 plane 段、每段一个页）。
     * ⚠ L2P region 512 对齐且 pair 窗口只有 8 个 LBA ⇒ 窗口绝不跨 region。 */
    BYTE n;
    {
        const DWORD window_end = ftl_lba_pair_base(internal_lba) + FTL_LBA_PER_PAIR;
        const BYTE  avail      = (window_end - internal_lba);

        n = (*n_cw < avail) ? *n_cw : avail;
    }

    /* 写：攒进页打包缓冲（窗口满 / 切换窗口时内部触发编程），本函数对写**到此为止**。
     * 数据在 wbuf_handle(FLUSH) 里一次编程——分配/建链/提交/确认全在那里。 */
    if (!pending_op_is_read(g->op))
    {
        /* 回填窗口夹住后的实际消费数——调用者（ftl_drain_group）据此推进 g->next。
         * ⚠ 不可漏：内部 n 可能被窗口边界夹小（internal_lba 未 8 对齐时 avail<8），
         *   漏回填会让 g->next 越过窗口、跳过未下发的 LBA。 */
        *n_cw = n;

        wbuf_trigger_t trigger;
        memset(&trigger, 0, sizeof(trigger));
        trigger.kind = WBUF_TRIGGER_APPEND;
        trigger.lba  = internal_lba;
        trigger.data = buffer;
        trigger.n    = n;
        trigger.gi   = gi;

        return ftl_wbuf_event(ftl, &trigger);
    }

    /* ── 以下为读路径 ── */

    /* 命中写缓冲：该 LBA 尚未编程，从缓冲取（否则会读到陈旧 L2P 映射）。同步完成、
     * 消费 1 个 LBA（不进 outstanding）。 */
    if (ftl_wbuf_read(ftl->wbuf, internal_lba, buffer))
    {
        *n_cw = 1U;
        return FTL_OK;
    }

    DWORD pba[PLANE_NUM];
    for (BYTE i = 0U; i < PLANE_NUM; i++)
    {
        pba[i] = NAND_PBA_INVALID; /* 未覆盖的 plane 保持 INVALID（见 ftl_pending_req_t.new_pba 约定） */
    }

    DWORD cw_bitmap = 0U; /* flat：bit = plane*CW_NUM + cw */
    {
        /* 逐 LBA 查 L2P 并按 (物理 plane, 页) 分组。收缩条件：
         *   · 该 LBA 仍在写缓冲里（未编程）—— 本 LBA 不进本子操作（下一轮从缓冲取）
         *   · 未映射（INVALID）—— 本 LBA 不进本子操作（下一轮单独处理）
         *   · region 尚未就绪（异步加载在飞）—— 本子操作到此为止，下轮再来
         *   · 同 plane 但换了页 —— 一条命令每 plane 只能一个页，装不下
         *   · 非法 PBA / 物理 plane ≠ LBA 的 plane 位 —— 映射表损坏 → FTL_ERROR */
        BYTE k;

        for (k = 0U; k < n; k++)
        {
            const DWORD l = (internal_lba + k);
            DWORD       p = NAND_PBA_INVALID; /* 越界（FTL_ERROR）保持此值 = 未映射 */

            if (ftl_wbuf_has(ftl->wbuf, l))
            {
                break; /* 还在写缓冲（未编程）：收缩到此为止，下一轮同步从缓冲取 */
            }

            /* ⚠ 三态：FTL_AGAIN = region 未驻留（已起异步加载）。**必须**与
             * "未映射"（FTL_OK + INVALID）分清——把前者当后者会让 host 读到假数据。
             * FTL_ERROR 仅越界，此时 p 保持 INVALID。 */
            if (l2p_cache_read(ftl->cache, l, &p) == FTL_AGAIN)
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
                LOGE("[FTL] rd lba=%lu: L2P entry 0x%08lx is not a valid PBA\r\n", l, p);
                return FTL_ERROR;
            }

            BYTE  pl = pba_to_plane(p);
            DWORD pg = pba_page(p);

            /* CW 级映射的结构不变量：**物理 plane == LBA 的 plane 位**。
             * 违反 ⇒ FC 侧 desc->meta（由**物理** plane/cw 铸出）还原出的扇区 LBA 不是
             * 这一条，读校验必挂 MATA_ERROR；且位图段（按 plane 位算）与 paa 的 plane 位
             * 会分叉 ⇒ 空链。写路径由构造保证（新页从 pair 取），读路径在此拦下。
             * 属映射表损坏（旧介质格式 / bit rot），不是编程错误。 */
            if (pl != ftl_lba_plane(l))
            {
                LOGE("[FTL] rd lba=%lu: plane mismatch pba=0x%08lx\r\n", l, p);
                return FTL_ERROR;
            }

            if ((pba[pl] != NAND_PBA_INVALID) && (!pba_same_page(pba[pl], p)))
            {
                break; /* 同 plane 跨页：本子操作到此为止，余下交给下一轮 */
            }

            const BYTE cw = ftl_lba_cw(l);

            pba[pl] = pg;
            cw_bitmap |= ftl_cw_bit(pl, cw);
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
    BYTE nplanes = ftl_cw_bitmap_plane_count(cw_bitmap);

    /* 资源次序：**先查 pending 表余量，再配 desc 链**。末尾要用一个 pending 槽
     * 登记本子操作；表满却先建好链，只能整条回滚——白配一场。
     * ⚠ 必须在 wbuf 命中分支**之后**：那条路径同步完成、不占 pending 槽，不该被表满挡住。 */
    if (pending_count(ftl) >= FTL_PENDING_MAX)
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
    cmd->meta               = ftl_lba_pair_base(internal_lba);

    /* 建 desc 链（逐 plane 各自的 PBA，位图 = 本子操作实际覆盖的 CW）；
     * desc/buffer 池空 → 瞬态背压 */
    ftl_status_t ret = ftl_cmd_setup_ppa_pair_cw(cmd, pba, cw_bitmap);
    if (ret != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return ret; /* FTL_AGAIN（池空，瞬态）/ FTL_ERROR 原样上抛 */
    }

    /* 注册 pending 子操作（必须早于 submit，避免竞态）。读：owners = 单组位、写专用字段传
     * INVALID、cw_bitmap 传 0；lba 传**全局** LBA（L2P/Journal/META 均按全局索引）。 */
    ftl_pending_req_t *pending = pending_alloc(ftl);

    if (pending == NULL)
    {
        (void)fc_command_release(cmd);
        return FTL_AGAIN; /* pending 表满：瞬态 */
    }

    pending->command_id         = ftl_cmd_id(cmd);
    pending->owners             = ftl_group_bit(gi);
    pending->op                 = PENDING_OP_READ;
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
    pending_group_add_subop(ftl, gi);

    /* 提交（tag = HOST_IO；ctx 传 NULL —— 私有状态在 pending 表） */
    if (ftl_op_submit(cmd, FTL_OP_HOST_IO, NULL) != FTL_OK)
    {
        pending_remove(ftl, ftl_cmd_id(cmd));
        (void)fc_command_release(cmd);
        pending_group_drop_subop(ftl, gi);
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
static void ftl_drain_group(ftl_context_t *ftl, WORD gi)
{
    ftl_io_group_t *g = &ftl->groups[gi];

    while ((g->used != 0U) && (g->next < g->total))
    {
        const DWORD off    = g->next; /* 已下发到的 LBA 偏移（g->next 以 LBA 计，非子操作序号） */
        const DWORD remain = g->count - off;
        DWORD       glba   = (g->lba + off);
        void       *buffer = (BYTE *)g->buffer + (off * FTL_IU_BYTES);
        /* 本窗口内可提供的最大 LBA 数（≤ 一个 pair 窗口）。实际消费几个由
         * ftl_issue_subop 回填——读路径按 L2P 分组收缩，写路径按窗口边界收缩。 */
        BYTE n_cw          = (remain >= FTL_LBA_PER_PAIR) ? FTL_LBA_PER_PAIR
                                                          : remain;

        ftl->l2p_defer        = 0U; /* 每次下发前清零：本轮的背压原因由本次下发重新判定 */
        const ftl_status_t st = ftl_issue_subop(ftl, gi, glba, buffer, &n_cw);

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
             * ⚠ **L2P region 加载在飞不算无进展**：那是有确定完成时刻的
             *   forward progress（NAND 层 wait/jump 超时保证命令必有完成），计进来会让
             *   一次正常的 region 加载吃掉 8 轮预算里的一轮 ⇒ 随机读满盘时把好组误判
             *   成卡死报 IO_ERROR。真卡死（命令丢失）由 NAND 层超时兜底。 */
            if (ftl->l2p_defer == 0U)
            {
                if (++g->stalls > FTL_DRAIN_STALL_MAX)
                {
                    LOGE("[FTL] drain stalled gi=%u next=%lu/%lu\r\n", (unsigned)gi, g->next, g->total);
                    g->agg_status = SSD_STATUS_IO_ERROR;
                    g->next       = g->total;
                }
            }
            break;
        }

        /* FTL_ERROR：永久失败 → 弃未下发者，组最终报错（已下发的照常完成） */
        LOGE("[FTL] subop failed gi=%u next=%lu/%lu\r\n", (unsigned)gi, g->next, g->total);
        g->agg_status = SSD_STATUS_IO_ERROR;
        g->next       = g->total;
        break;
    }

    pending_group_maybe_complete(ftl, gi);
}

/* --------------------------------------------------------------------
 * ftl_submit_common
 *   功能：读/写共用的提交骨架——入参校验 + 建组 + 快路径下发。
 *   参数：ftl —— 实例；req —— 请求（count = 连续 LBA 数，buffer 非空）；
 *         op —— PENDING_OP_READ / PENDING_OP_WRITE（决定组类型与日志标签）。
 *   返回：FTL_OK（已建组，可能仍有余量待 Phase 2.5 补发）/ FTL_ERROR（参数非法 /
 *         越界 / 组槽满）。
 *   实现要点：两个公开入口的流程**逐字相同**（差异只有 op 与日志文案）⇒ 收敛到本函数
 *             一份，避免两处各自漂移（H6：注释与代码不一致即缺陷，重复实现更甚）。
 * -------------------------------------------------------------------- */
static ftl_status_t ftl_submit_common(ftl_context_t *ftl, const ftl_io_request_t *req, ftl_pending_op_t op)
{
    const char *label = (op == PENDING_OP_READ) ? "read" : "write";

    /* host 入口：入参来自 HIL，属**不可信输入**（H29）⇒ 保留返回码，不转 ASSERT。 */
    if ((ftl == NULL) || (req == NULL) || (req->buffer == NULL))
    {
        return FTL_ERROR;
    }

    /* 长度 = 连续 LBA 数（上层 block_count）；FTL 自行决定单/双 plane 拆分 */
    DWORD count = req->count;
    if (count == 0U)
    {
        return FTL_ERROR;
    }

    /* LBA 范围检查（含末 LBA）。req->lba 是 QWORD ⇒ 加法按 64 位进行，不回绕。 */
    if ((req->lba + count) > FTL_LBA_MAX)
    {
        LOGE("[FTL] %s lba out of range lba=%lu count=%lu\r\n", label, (unsigned long)req->lba, count);
        return FTL_ERROR;
    }

    /* 建完成组（一条 host 请求 = 一个组）。通知字段必须**早于任何下发**填入：
     * 下发可能当场完成整组，届时通知就要读它们。 */
    int gi = pending_group_alloc(ftl);
    if (gi < 0)
    {
        LOGE("[FTL] %s: no free group slot\r\n", label);
        return FTL_ERROR;
    }
    ftl_io_group_t *g  = &ftl->groups[gi];
    g->op              = op;
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
     * 数据在 completion_table_drain 中拷贝回 buffer（读），Journal + L2P 在完成
     * handler 中确认 NAND PROGRAM 成功后执行（写）。 */
    ftl_drain_group(ftl, gi);

    return FTL_OK;
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
    return ftl_submit_common(ftl, req, PENDING_OP_READ);
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
    return ftl_submit_common(ftl, req, PENDING_OP_WRITE);
}

/* ── 元数据落盘 / TRIM（供 HIL 调用）── */

/* --------------------------------------------------------------------
 * ftl_flush
 *   功能：把已持久化映射落盘——L2P cache 全部写回 + journal 尾页刷出（供 HIL 调用）。
 *   参数：ftl —— 实例（**前置条件**；实例绑定由 ftl_contract_audit 在 boot 期证明）。
 *   返回：无。
 *   实现要点：**非** NVMe 式 barrier——不等待在飞 / 排队的写（CLI 靠 g_cli_pending.busy
 *             串行化）。L2P 与 journal 走**同步**逃生口（自旋泵）而非 kick：`fl` 的契约
 *             就是"返回时脏数据真的已持久"，而它本就在 CLI 上下文，自旋不是嵌套泵。
 *             后台攒批走 l2p_flush_kick（ftl_service 各档）。
 * -------------------------------------------------------------------- */
void ftl_flush(ftl_context_t *ftl)
{
    /* 前置条件（判据 2 / C5）：唯一调用者 controller.c 在调用前已 ASSERT 路由结果非空。 */

    /* 先把页打包缓冲里尚未编程的写编程掉：它们还没进 L2P/journal，"落盘"不成立。
     * ⚠ 遇瞬态背压会保留缓冲、下轮 ftl_service 重试；此处尽力而为。 */
    {
        wbuf_trigger_t trigger;
        memset(&trigger, 0, sizeof(trigger));
        trigger.kind = WBUF_TRIGGER_FLUSH;
        (void)ftl_wbuf_event(ftl, &trigger);
    }

    const DWORD dim = ftl_topo_dim(ftl->topo);

    (void)l2p_cache_flush_owner_sync(ftl->cache, dim);

    if (journal_flush_page(ftl->journal) != FTL_OK)
    {
        LOGW("[FTL] flush: journal barrier not reached\r\n");
    }
}

/* --------------------------------------------------------------------
 * ftl_trim
 *   功能：逐 LBA 取消映射（TRIM）——减 CW 有效计数 + 写 L2P INVALID + 记 Journal。
 *   参数：ftl —— 实例；lba —— 起始 LBA；count —— LBA 数（0 视为 1，兼容单 LBA 调用）。
 *   返回：无。
 *   实现要点：CW 级映射下 1 个 LBA = 1 个 CW ⇒ 逐 LBA 减 1 个有效 CW 计数
 *             （valid_count 是**精确**的 CW 计数；无常驻位图，有效性逐 CW
 *             由页内 META + L2P 反查判定——见 bst.h 文件头）。
 *             ⚠ **Journal 先于 L2P 写**（同 host 写路径的顺序）：两步之间掉电时，
 *             回放能重放这条 INVALID 条目，而不会因 L2P 已改、Journal 未记而把
 *             已 TRIM 的 LBA 复活。
 * -------------------------------------------------------------------- */
void ftl_trim(ftl_context_t *ftl, QWORD lba, DWORD count)
{
    /* 前置条件（判据 2 / C5）：唯一调用者 controller.c 在调用前已 ASSERT 路由结果非空。 */

    if (count == 0U)
    {
        count = 1U; /* 兼容"单 LBA"调用 */
    }
    if ((lba + count) > FTL_LBA_MAX)
    {
        return;
    }

    /* 逐 LBA 取消映射（非阻塞、同步完成）。范围上界/分块后续再定。 */
    for (DWORD i = 0U; i < count; i++)
    {
        DWORD       old_pba = NAND_PBA_INVALID; /* 越界（FTL_ERROR）由 API 回填，此处再兜一层 */
        const DWORD l       = (lba + i);        /* 全局 LBA（数据通路统一坐标） */

        /* 该 LBA 还在页打包写缓冲里（未编程）：只清缓冲槽，不碰 L2P/journal——它从未
         * 落盘，无旧映射可失效、也无映射变更需记。缓冲的内部不变量（清空 ⇒ 结算 owners）
         * 由 wbuf_handle(DROP) 收口，本函数不手工维护。 */
        if (ftl_wbuf_has(ftl->wbuf, l))
        {
            wbuf_trigger_t trigger;
            memset(&trigger, 0, sizeof(trigger));
            trigger.kind = WBUF_TRIGGER_DROP;
            trigger.lba  = l;
            (void)ftl_wbuf_event(ftl, &trigger);
            continue;
        }

        /* 查旧映射：TRIM 走**同步**逃生口（自旋泵）——它由 CLI 调用，不在完成派发里，
         * 且契约是"取消映射同步完成"。读得 INVALID 与"未映射"同义，两种都是"无旧映射"。 */
        (void)l2p_cache_read_sync(ftl->cache, l, &old_pba);

        /* ⚠ **Journal 先于 L2P 写**（顺序不可换，同 host 写路径 ftl_confirm_write）。
         * 取消映射也必须进 Journal（new_pba = INVALID）：L2P 的每一次变更都由 Journal
         * 承载，掉电恢复靠回放重建——漏记这一条会让**掉电恢复把已 TRIM 的 LBA 复活**
         * （region 虽被 flush 成 INVALID，但回放按序重放更早的写条目——page_seq 更小
         * 但 > cp_sequence——会把它改回有效 PBA，且回放行标 DIRTY 后
         * ftl_boot_apply_region_bitmap 会跳过预载，NAND 上的 INVALID 也不生效）。
         * 反过来若先写 L2P 再记 Journal，掉电就落在这个窗口：L2P 已 INVALID 而 Journal
         * 无条目 ⇒ 回放不覆盖 ⇒ 该 region 若随后被淘汰重载，映射复活。 */
        (void)journal_append(ftl->journal, l, old_pba, NAND_PBA_INVALID);

        (void)l2p_cache_write_sync(ftl->cache, l, NAND_PBA_INVALID);

        /* 有效计数在两步之后减：它只是 RAM 统计（valid_count 虚高会让 GC 选 victim 不准），
         * 不进 Journal，位置不影响掉电正确性。 */
        if (old_pba != NAND_PBA_INVALID)
        {
            WORD gblock = ftl_pba_gblock(old_pba);

            /* 同写确认路径：BAD / count 已清时跳过，避免断言或 `WORD` 回绕。 */
            ftl_block_sub_valid_if_live(&ftl->alloc.blocks[gblock]);
        }
    }
}

/*
 * ftl.c — FTL 顶层（初始化 / 后台调度服务主循环 / 子操作下发 / 对外读写入口）
 *
 * 职责：
 *   FTL 的对外门面与主循环——ftl_init 装配各子模块；ftl_service 是**服务主循环**
 *   （NAND 泵 → 完成泵 → 补发 host 子操作 → 时间预算后台调度）；ftl_submit_read/write
 *   把一条 host 请求拆成多个 FC 子操作下发；ftl_flush_begin/step / ftl_trim 供 HIL 调用。
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
 *   Phase 2    ct_drain —— 统一完成泵（按命令 op_tag 派发到服务函数数组）
 *   Phase 2.5  逐组补发 host 子操作（host IO 优先于后台任务）
 *   Phase 3    ftl_bg_service —— 时间预算后台调度（本文件只有一行调用）
 *
 * 文件结构（与下方分区注释一一对应）：
 *   1. 内部辅助        ftl_confirm_write / ftl_host_io_callback（+ 4 个 ftl_host_io_* 完成
 *                      分支）/ pend_confirm_service
 *   2. 对外接口        ftl_init / ftl_service_pump / ftl_service
 *   3. 子操作下发      ftl_issue_subop / ftl_drain_group（static）+ ftl_submit_read / write
 *   4. 元数据落盘/TRIM ftl_flush_begin/step / ftl_trim
 *
 * ⚠ 后台调度在 `ftl/bg_wiring.c`：10 项任务表 / 档位 IDLE·NORMAL·BUSY → 预算规模 /
 *   mig_gc_evaluate（GC 统一触发求值）/ RR 巡检 / Phase 3 入口 ftl_bg_service。
 *   通用引擎（零 kick 知识）在 `ftl/bg_scheduler.c`。本文件不持有任何后台策略。
 *
 * ⚠ 页打包写缓冲在 `ftl/wbuf.c`（wbuf.h 定义 wb_handle_t），本文件只调用其接口。
 *
 * ⚠ 回收动作在 `ftl/migrate.c`：GC / WL / 慢死预测共用一台异步搬移 job
 *   （mig_begin_* / mig_handle）。触发求值在 `ftl/bg_wiring.c`，写路径的按需 GC 在
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
#include "ftl_job.h" /* ftl_job_now_ms（flush 触发 CP 的定时） */
#include "ftl_op.h"  /* ftl_op_submit / FTL_OP_* */
#include "log.h"
#include "pending.h"
#include "wbuf.h"

#include <string.h>

/* dangling 守卫日志：
 *   · `st=BAD` 是**预期降级**（旧块已退役，数据本不可读）⇒ 每个旧 gblock 只打一行，
 *     其余 LBA 只累计到该槽位的 lbas（一个块 256 个 LBA，逐条打必刷屏）。
 *   · `st=FREE/LOG/NONE` 是**非预期缺陷** ⇒ 仍用限流 ERROR，保留完整现场。 */
#define FTL_DANGLING_SLOTS (8U)

typedef struct
{
    WORD  g;         /* 旧 gblock；lbas==0 表示空槽 */
    DWORD first_lba; /* 该块第一个命中的 LBA */
    DWORD lbas;      /* 该块累计命中的 LBA 数 */
} ftl_dangling_ent_t;

static ftl_dangling_ent_t s_dangling[FTL_DANGLING_SLOTS];
static BYTE               s_dangling_next;

/* 非预期 dangling 的限流（1 s 一行 + 被抑制条数）。 */
static DWORD s_dangling_log_ms;
static DWORD s_dangling_suppressed;

/* 日志限流（RISK-USB-06）：同一类异常在短时间内重复出现时，5 s 打一行并给出被
 * 抑制条数；平台时钟缺失时退化为 64 次一行。各类别用独立实例（互不抑制）。 */
typedef struct
{
    DWORD ms;         /* 上次打印时刻（0 = 尚未打印） */
    DWORD suppressed; /* 自上次打印以来被抑制的条数 */
} ftl_log_throttle_t;

static bool ftl_log_throttle_due(ftl_log_throttle_t *t, DWORD *out_suppressed)
{
    const DWORD now_ms = ftl_job_now_ms();

    bool due = (t->suppressed >= 64U);

    if ((!due) && (now_ms != 0U))
    {
        due = (t->ms == 0U) || ((now_ms - t->ms) >= FTL_JOB_LOG_SLOW_INTERVAL_MS);
    }

    if (!due)
    {
        t->suppressed++;
        return false;
    }

    *out_suppressed = t->suppressed;
    t->suppressed   = 0U;
    t->ms           = now_ms;
    return true;
}

/* flush 日志限流（RISK-USB-06；host 频繁 fsync 时避免刷屏）：
 *   s_flush_log_ms —— `[FTL] flush took`（正常路径）；
 *   s_flush_err    —— 失败 / 超时（异常路径）。 */
static DWORD              s_flush_log_ms;
static ftl_log_throttle_t s_flush_err_log;

/* 降级 region 的读错误日志（RISK-BOOT-18）：一次 host 校验 / 文件系统扫描会连续
 * 读同一 region 的数百个 LBA，逐条 LOGE 会写满 UART TX 环并拖慢设备。 */
static ftl_log_throttle_t s_lost_log;

/* no-space 归因日志：无空间是终止态，host 会持续重试 ⇒ 逐组打两行会刷屏。 */
static ftl_log_throttle_t s_nospace_log;

/* subop 永久失败日志：host 重试同一个坏 LBA / 降级 region 会反复触发 ⇒ 同样限流。 */
static ftl_log_throttle_t s_subop_fail_log;

/* ── 内部辅助 ── */

/* 组下发机制（定义在文件后部；ftl_service Phase 2.5 先用到） */
static void ftl_drain_group(ftl_ctx_t *ftl, WORD gi);

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
 *       FTL_AGAIN），第二遍才动账。绝不能边查边改——l2p_write 对正在加载/写回
 *       的行返 FTL_AGAIN，中途放弃会让同一页里部分 CW 已记账、部分没记，重试时重复
 *       记账（valid_count 虚高、journal 重复条目、total_writes 多计）。
 *       两遍之间无泵（单线程顺序执行）⇒ 就绪判定与第二遍之间状态不可能变。
 * -------------------------------------------------------------------- */
static ftl_status_e ftl_confirm_write(ftl_ctx_t *ftl, const pend_req_t *pending)
{
    /* 两个调用点（ftl_host_io_callback 的 `if (pending != NULL)` 内 / pend_confirm_service 的
     * `&ftl->pending[i]`）都保证非空 ⇒ 不在本函数复检（§十附 / §二十六）。
     * 同理 pending->lba 恒为**窗口基址**（唯一写点是 wb_handle(FLUSH) 的 wbuf.base）——
     * 下面用 ftl_lba_plane/cw(l) 取代"从位下标反解坐标"就依赖这条。 */

    /* ── 第一遍：就绪前置（只探不动）── */
    for (BYTE bit = 0U; bit < FTL_LBA_PER_PAIR; bit++)
    {
        if ((pending->cw_bitmap & (1U << bit)) == 0U)
        {
            continue;
        }

        const DWORD lba = (pending->lba + bit);

        if (!l2p_is_ready(ftl->cache, lba))
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
         * 覆盖到的 plane 必有页（alc_alloc_pair 两 plane 都给）。 */

        DWORD newp = pba_with_cw(page, cw);

        /* 就绪前置已保证驻留 ⇒ 读必命中、写必不返 FTL_AGAIN。非 OK 只剩"LBA 越界"
         * 一种可能（L2P 范围外）⇒ 按"未映射"处理并留一行日志。 */
        DWORD oldp;
        if (l2p_read(ftl->cache, l, &oldp) != FTL_OK)
        {
            LOGE("[FTL] confirm: l2p read lba=%lu out of range\r\n", l);
            oldp = NAND_PBA_INVALID;
        }

        (void)jrn_append(ftl->journal, l, oldp, newp);
        (void)l2p_write(ftl->cache, l, newp);

        /* 飞行记录器：记录成功确认的 host 写（kind=0）。 */
        alc_trace_record(&ftl->alloc, l, newp, 0U);

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
                const bst_state_e old_st = ftl_block_state(&ftl->alloc.blocks[gob]);

                ftl->dangling_total++;

                if (old_st == FTL_BLOCK_STATE_BAD)
                {
                    /* 预期降级：旧块已退役。每个旧 gblock 只打一行，其余 LBA 只累计。 */
                    ftl_dangling_ent_t *slot = NULL;

                    for (BYTE i = 0U; i < FTL_DANGLING_SLOTS; i++)
                    {
                        if ((s_dangling[i].lbas != 0U) && (s_dangling[i].g == gob))
                        {
                            slot = &s_dangling[i];
                            break;
                        }
                    }

                    if (slot == NULL)
                    {
                        slot = &s_dangling[s_dangling_next % FTL_DANGLING_SLOTS];
                        s_dangling_next++;
                        slot->g         = gob;
                        slot->first_lba = l;
                        slot->lbas      = 0U;

                        LOGW("[FTL] dangling g=%u first_lba=%lu st=BAD newg=%u (retired; later LBAs aggregated)\r\n",
                             (unsigned)gob,
                             l,
                             (unsigned)newg);
                    }

                    slot->lbas++;
                }
                else
                {
                    /* 非预期：FREE/LOG/NONE 都可能意味着重建/分配器/分类器缺陷。 */
                    const DWORD now_ms = ftl_job_now_ms();

                    if ((now_ms == 0U) || (s_dangling_log_ms == 0U) ||
                        ((now_ms - s_dangling_log_ms) >= FTL_JOB_LOG_INTERVAL_MS))
                    {
                        const char *old_state = ftl_block_state_name(old_st);
                        const char *new_state = ftl_block_state_name(ftl_block_state(&ftl->alloc.blocks[newg]));

                        (void)LOGE("[FTL] dangling lba=%lu old=%08lx g=%u st=%s sb=%u newg=%u nst=%s supp=%lu\r\n",
                                   l,
                                   oldp,
                                   (unsigned)gob,
                                   old_state,
                                   (unsigned)ftl->alloc.blocks[gob].sb_id,
                                   (unsigned)newg,
                                   new_state,
                                   s_dangling_suppressed);
                        s_dangling_log_ms     = now_ms;
                        s_dangling_suppressed = 0U;
                    }
                    else
                    {
                        s_dangling_suppressed++;
                    }
                }
            }

            /* 坏块 / count 已为 0 时跳过：bbt_mark_bad 清过账，再减会断言/回绕。
             * 上面的 dangling 日志已把“L2P 仍指向该块”这件事留痕。 */
            ftl_block_sub_valid_if_live(&ftl->alloc.blocks[gob]);
        }
    }

    jrn_update_watermark(ftl->journal);
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
static ssd_status_e ftl_host_io_read_ok(ftl_ctx_t    *ftl,
                                        fc_command_t *cmd,
                                        pend_req_t   *pending,
                                        WORD          cmd_id)
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
 *   pend_confirm_service 每轮推进。与读失败的托管同构（命令脱钩、组不结算），区别是推进者
 *   在本文件而非 RR 引擎。
 * 返回 true = 已停泊 ⇒ 调用者**直接 release 并 return**（勿再动组簿记）。 */
static bool ftl_host_io_write_ok(ftl_ctx_t *ftl, pend_req_t *pending)
{
    if (ftl_confirm_write(ftl, pending) != FTL_AGAIN)
    {
        return false;
    }

    /* 停泊：命令脱钩、组在飞数不递减；停泊列表与超时归 pending 模块
     * （docs §5「Confirm → pending 服务」）。 */
    const WORD pending_idx = (pending - ftl->pending.slots);

    pend_confirm_park(&ftl->pending, pending_idx, pending->owners);
    return true;
}

/* 读失败 ⇒ **托管**给 Read Recovery job（见 read_recovery.c）：
 *   · pending 项**保留**（不 pend_remove）、组在飞数**不**递减 ⇒ host 读组继续
 *     pending，等 RR 收尾时统一结算；
 *   · command_id 置哨兵：本命令随即 release，其 id 会被后续 fc_command_alloc 复用，
 *     留旧值会让新命令的完成误撞上本项；
 *   · 收尾（含一律报 IO_ERROR 的语义）在 rr_finish 里，见该处注释。
 * ⚠ 恢复**必须**异步：本函数跑在完成泵里，当场同步跑三级恢复就是泵里再开泵（嵌套泵），
 *   且整条 host 读流水线被一个坏页钉住。 */
static void ftl_host_io_read_fail(ftl_ctx_t *ftl, pend_req_t *pending)
{
    pending->op         = PENDING_OP_READ_RECOVERY;
    pending->command_id = FTL_PENDING_CMD_NONE;

    /* typed 触发：把 owner 交给 RR handle；RR busy 时该项仍留在 pending 表，
     * 当前 job 收尾后由 pending 端口的 take 领走（行为与旧扫描一致）。 */
    const WORD pending_idx = (pending - ftl->pending.slots);
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
 * 失败 plane 定位：遍历该命令的 desc 链，逐 CW 取 status；失败 CW 的 desc->paa
 * 给出 plane ⇒ 两个 plane 各自的块都能当场标坏（不再依赖 GC 擦除时才发现 plane1）。 */
static void ftl_host_io_write_fail(ftl_ctx_t *ftl, pend_req_t *pending, const fc_command_status_t *completion)
{
    /* 失败 plane 掩码：逐 desc 取 CW 状态 + desc->paa 的 plane。
     * 旧实现只标 new_pba（plane0），plane1 的失败要等 GC 擦除时才暴露（RISK-MIG-07）。 */
    DWORD bad_plane_mask = 0U;

    fc_command_t *cmd = fc_command_id_to_command(completion->command_id);
    WORD          did = cmd->desc_id;

    while (did != FC_DESCRIPTOR_ID_INVALID)
    {
        const fc_descriptor_t *d = fc_descriptor_id_to_descriptor(did);

        if (d == NULL)
        {
            break;
        }
        if (fc_cw_status_has_error(d->status))
        {
            bad_plane_mask |= (1U << paa_to_plane(d->paa));
        }
        did = d->next_id;
    }

    if (bad_plane_mask == 0U)
    {
        /* 没有逐 CW 失败位（命令级失败）：退回旧行为，按 plane0 表项标坏。 */
        bad_plane_mask = (1U << pba_to_plane(pending->new_pba));
    }

    char stbuf[48];

    (void)fc_cw_status_str(completion->status, stbuf, sizeof(stbuf));
    LOGW("[FTL] program-fail lba=%lu pl=0x%lx st=%s\r\n", pending->lba, bad_plane_mask, stbuf);

    if (((bad_plane_mask & 0x1U) != 0U) && (pending->new_pba != NAND_PBA_INVALID))
    {
        alc_trace_record(&ftl->alloc, pending->lba, pending->new_pba, 1U);
        alc_trace_dump(&ftl->alloc, pending->lba, pending->new_pba);
        (void)bbt_mark_bad(&ftl->alloc, ftl_pba_gblock(pending->new_pba));
    }
    if (((bad_plane_mask & 0x2U) != 0U) && (pending->new_pba1 != NAND_PBA_INVALID))
    {
        alc_trace_record(&ftl->alloc, pending->lba, pending->new_pba1, 1U);
        alc_trace_dump(&ftl->alloc, pending->lba, pending->new_pba1);
        (void)bbt_mark_bad(&ftl->alloc, ftl_pba_gblock(pending->new_pba1));
    }

    (void)alc_close_open_sb(&ftl->alloc, FTL_TEMP_HOT);
}

/* --------------------------------------------------------------------
 * ftl_write_retry - 写失败后把同一页重发到新块（透明重试，主流 program-fail 恢复）
 *
 * 数据来源：旧命令的 desc 链在 `fc_command_release` 之前仍持有本页数据
 * （`ftl_cmd_read_data` 的既有契约，见 ftl_fc_interface.h）。因此不需要常驻
 * 页缓冲；只在失败时拷一次。
 *
 * 顺序：读回数据 → 分配新页对 → 建新命令（**链序**填数）→ 提交 →
 * 成功后才更新 pending 的 command_id/new_pba（提交失败时保持旧 id，让调用者
 * 仍能按本次 completion 的 cmd_id 收尾）。
 *
 * @return FTL_OK = 已提交（同一 pending 继续在飞，组在飞数**不**递减）；
 *         其他 = 无法重试，调用者按原 IO_ERROR 收尾。
 * -------------------------------------------------------------------- */
/* 透明重试日志节流：第 1 次立即打印，此后每 N 次一条（program-fail 风暴不刷屏）。 */
#define FTL_WRITE_RETRY_LOG_EVERY (64U)
static DWORD s_write_retry_logged;

static ftl_status_e ftl_write_retry(ftl_ctx_t *ftl, pend_req_t *pending, fc_command_t *old_cmd)
{
    const DWORD len = (DWORD)pending->n_cw * FTL_IU_BYTES;

    if (ftl_cmd_read_data(old_cmd, ftl->write_retry_buf, len) != FTL_OK)
    {
        if ((s_write_retry_logged++ % FTL_WRITE_RETRY_LOG_EVERY) == 0U)
        {
            LOGE("[FTL] retry: page readback failed lba=%lu count=%lu\r\n", pending->lba, s_write_retry_logged);
        }
        return FTL_ERROR;
    }

    DWORD pba[PLANE_NUM];

    if (alc_alloc_pair(&ftl->alloc, FTL_TEMP_HOT, pba) != FTL_OK)
    {
        return FTL_AGAIN; /* Free/OP 耗尽：无法替换 */
    }

    fc_command_t *cmd = fc_command_alloc();

    if (cmd == NULL)
    {
        alc_rewind_pair(&ftl->alloc, FTL_TEMP_HOT);
        return FTL_AGAIN;
    }

    cmd->config = old_cmd->config;
    cmd->meta   = pending->lba;

    ftl_status_e st = ftl_cmd_setup_ppa_pair_cw(cmd, pba, pending->cw_bitmap);

    if (st == FTL_OK)
    {
        st = ftl_cmd_write_data(cmd, ftl->write_retry_buf, len);
    }
    if (st != FTL_OK)
    {
        (void)fc_command_release(cmd);
        alc_rewind_pair(&ftl->alloc, FTL_TEMP_HOT);
        return st;
    }

    if (ftl_op_submit(cmd, FTL_OP_HOST_IO, NULL) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        alc_rewind_pair(&ftl->alloc, FTL_TEMP_HOT);
        return FTL_AGAIN;
    }

    /* 提交成功后才改 pending：失败路径仍按旧 command_id 收尾（见函数头）。 */
    pending->command_id = ftl_cmd_id(cmd);
    pending->new_pba    = pba[0];
    pending->new_pba1   = pba[1];
    pending->retry_count++;

    if ((s_write_retry_logged++ % FTL_WRITE_RETRY_LOG_EVERY) == 0U)
    {
        LOGW("[FTL] write retry lba=%lu cw=0x%02x retry=%u pba=0x%08lx/0x%08lx count=%lu\r\n",
             pending->lba,
             (unsigned)pending->cw_bitmap,
             (unsigned)pending->retry_count,
             pba[0],
             pba[1],
             s_write_retry_logged);
    }

    return FTL_OK;
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
 *             release 并 return，组在飞数不递减，由 RR 引擎 / pend_confirm_service 收尾。
 *             其余路径收尾 pend_remove + 组聚合，最后 release
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

    pend_req_t *pending = pend_find(&ftl->pending, cmd_id);

    if (pending != NULL)
    {
        ssd_status_e sub_status = SSD_STATUS_OK;

        /* 成败判据 = **全部**错误位（UNC / CRC / MATA_ERROR / NAND_FAIL / TIMEOUT / RAID），
         * 不是单看 NAND_FAIL：ECC 不可纠只置 UNC（fc_data_path.c 的 ECC 分支），
         * 页内 META 不符只置 MATA_ERROR——两者的 NAND_FAIL 都恒清，单看它会把
         * 「内容不可信」当成功。且 UNC 时 FC 跳过后面的解扰/修正 pass，缓冲里
         * 是**加扰垃圾**，搬给 host 就是静默数据损坏。判据见 fc_cw_status_has_error()。
         *
         * 分流依据是 pending->op（显式操作类型），不是 buffer——写请求的 buffer
         * 是数据源、恒非空，用 buffer 判别会把写全判成读。 */
        const bool is_read = pend_op_is_read(pending->op);

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
            ftl_host_io_write_fail(ftl, pending, completion);

            /* 透明重试：同一页在新块上重发一次。成功 ⇒ 组保持 pending（**不**递减
             * 在飞数），由新命令的完成继续推进；失败 ⇒ 走原 IO_ERROR 收尾。 */
            if ((pending->retry_count < FTL_WRITE_RETRY_MAX) &&
                (ftl_write_retry(ftl, pending, cmd) == FTL_OK))
            {
                (void)fc_command_release(cmd);
                return;
            }

            sub_status = SSD_STATUS_IO_ERROR;
        }

        /* 子操作完成 → 组聚合：通知字段（complete_flag/status_out/
         * on_complete）只在组全部子操作完成后触发一次。
         * ⚠ 页打包下一页可能由**多个组**拼成 ⇒ 按 owners 位掩码逐组递减。
         * ⚠ owners 必须在 pend_remove **之前**取走：组完成通知会调 on_complete，
         *    回调可能重入 ftl_submit_* 并复用本槽 ⇒ 释放后再读就是读别人的字段。
         *    （另外两处结算点 read_recovery.c / pend_confirm_service 同此顺序。） */
        DWORD owners = pending->owners;

        pend_remove(&ftl->pending, cmd_id);
        pend_group_complete_subop_multi(&ftl->pending, owners, sub_status);
    }

    (void)fc_command_release(cmd);
}

/* pending 模块的 confirm handler：数据面事务仍在 FTL（Journal→L2P→BST 唯一顺序）。 */
static ftl_status_e ftl_confirm_handler(void *ctx, pend_req_t *pending)
{
    return ftl_confirm_write((ftl_ctx_t *)ctx, pending);
}

/* ── 模块端口装配（每个 helper 只做一件事：建端口 + 绑定） ── */

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
ftl_status_e ftl_init(ftl_ctx_t *ftl, ftl_topo_t topo, l2p_cache_t *cache, const ftl_storage_t *storage)
{
    /* 前置条件（判据 2）：唯一调用者 main.c 传 &g_ftl[ch][ce][lun]、共享 cache、storage。 */
    ASSERT(ftl != NULL);
    ASSERT(cache != NULL);
    ASSERT(storage != NULL);
    ASSERT(storage->table_refs != NULL);
    ASSERT(storage->metadata_dir != NULL);
    ASSERT(storage->journal != NULL);
    ASSERT(storage->cp_handle != NULL);
    ASSERT(storage->migrate != NULL);
    ASSERT(storage->wbuf != NULL);
    ASSERT(storage->rr_page_scratch != NULL);

    /* ├─ PBA 字段视图自检（一次）：位域布局 implementation-defined，无法用 _Static_assert
     *    证明（见 ftl_config.h §三.2）。若本工具链的布局与 PBA_*_SHIFT 不一致，
     *    强转（*(DWORD *)&fields）会静默算错地址 ⇒ 在此硬失败。 */
    ASSERT(pba_fields_layout_matches());

    ftl->topo = topo;

    /* ├─ 绑定外部存储（storage 数组/挂载块由 HIL 按域提供）──
     * ⚠ .sram*_data 是 NOLOAD，启动代码不清零 ⇒ 下面必须逐表 memset。 */
    ftl->table_refs      = *storage->table_refs;
    ftl->metadata_dir    = storage->metadata_dir;
    ftl->journal         = storage->journal;
    ftl->cp_handle       = storage->cp_handle;
    ftl->mig_handle      = storage->migrate;
    ftl->wbuf            = storage->wbuf;
    ftl->rr_page_scratch = storage->rr_page_scratch;

    /* ⚠ 这里**不**调 ct_init()：旁表与实例路由表都是**全局**资源
     *    （completion_table.c 的 file-static），而本函数是**逐实例**调用的 ——
     *    在此清表会抹掉其它实例在飞的 ctx 登记，也会抹掉 main.c 已设好的路由表。
     *    该函数由 main.c 的 ssd_ftl_boot_all 在**所有实例 boot 之前**调用一次。
     *    （完成派发不需要在此注册 handler：命令自己带 op_tag，g_op_callbacks[] 是
     *     编译期常量数组，见 ftl_op.c；旁表只回答"这条命令的私有状态在哪"。） */

    /* ├─ Pending Request 表（per-instance） ──
     * confirm 停泊的推进归 pending 模块；事务 handler 由 FTL 注入一次。 */
    pend_init(&ftl->pending);
    pend_confirm_set_handler(ftl_confirm_handler);

    /* ├─ storage 数组显式清零（启动不碰 .sram*_data；Phase 1.5 挂载块同）──
     * 五张表：alc_init 逐块读 ftl_block_is_bad() ⇒ blocks 必须先清零；
     *         free/op/spare/closed 的读者按 count 有界，逐表清零让 NOLOAD 语义确定。
     * metadata_dir：load/init 不覆盖全部字段 ⇒ 先整表清零。
     * journal：init 只重置部分 job 字段 ⇒ 先整表清零。
     * cp_handle：active 在 cp_begin 自己 memset 之前就会被读 ⇒ 必须先清零。
     * migrate / wbuf / rr_page_scratch：Phase 1.5 挂载块，与其余数组一致清零。
     * ⚠ 全部 memset 必须早于 md_load。 */
    memset(ftl->table_refs.blocks, 0, FTL_BLOCK_TOTAL * sizeof(*ftl->table_refs.blocks));
    memset(ftl->table_refs.free_blocks, 0, FTL_BLOCK_TOTAL * sizeof(*ftl->table_refs.free_blocks));
    memset(ftl->table_refs.op_blocks, 0, PLANE_NUM * sizeof(*ftl->table_refs.op_blocks));
    memset(ftl->table_refs.spare_blocks, 0, PLANE_NUM * sizeof(*ftl->table_refs.spare_blocks));
    memset(ftl->table_refs.closed_sb_list, 0, SUPER_BLOCK_LIST_MAX * sizeof(*ftl->table_refs.closed_sb_list));
    memset(ftl->metadata_dir, 0, sizeof(*ftl->metadata_dir));
    memset(ftl->journal, 0, sizeof(*ftl->journal));
    memset(ftl->cp_handle, 0, sizeof(*ftl->cp_handle));
    /* Phase 1.5 挂载块：与其余 storage 数组一致显式清零（未来若落 NOLOAD 段，
     * 启动代码不清零）。 */
    memset(ftl->mig_handle, 0, sizeof(*ftl->mig_handle));
    memset(ftl->wbuf, 0, sizeof(*ftl->wbuf));
    memset(ftl->rr_page_scratch, 0, FTL_READ_USER_SIZE);

    /* ├─ 绑定共享 L2P Cache（设备级已初始化一次；metadata 依赖由 owner 解析）── */
    ftl->cache = cache;

    /* ├─ Allocator（各指针绑到 table_refs） ── */
    alc_init(&ftl->alloc, ftl->topo, &ftl->table_refs);
    ftl->alloc.md_dir = ftl->metadata_dir; /* bbt_mark_bad 的借用日志块判定 */

    /* ├─ 候选池存储绑定（metadata_dir 自持 md_pools_t；allocator 只持有指针）──
     * 必须在 bbt_init 之前：BBT 自举扫描/出厂扫描后要按 BAD 集建正式候选池。 */
    ftl->metadata_dir->topo  = ftl->topo;
    ftl->metadata_dir->alloc = &ftl->alloc;
    ftl->alloc.pools         = &ftl->metadata_dir->pools;

    /* ├─ BBT：先自举扫描/出厂扫描，再用 BAD 集建元数据候选池 ──
     * 顺序是承重的：Anchor/CP/Journal 的候选池都依赖 BBT 的 BAD 集；
     * 而 BBT 自身靠 record 的 magic/sequence 在「名义块 + shared spare」上自举。 */
    bbt_init(&ftl->alloc);

    if (!md_pool_build_all(&ftl->alloc))
    {
        LOGE("[FTL ] metadata candidate pool build failed — refuse boot\r\n");
        return FTL_ERROR;
    }

    /* ├─ Journal ── */
    /* 持续擦除失败的环形块要标进 BBT（RISK-JN-01）；journal 只持有 allocator 指针，
     * 通过 allocator/bbt 的 owner API 查询与标坏，不直接改其内部字段。 */
    jrn_init(ftl->journal, ftl->topo, &ftl->alloc);

    /* ├─ Metadata Directory（Anchor 候选池已就绪）── */
    ftl->total_writes   = 0U; /* 元数据 job 的 total_writes 指针在建目录前就有效 */
    ftl->dangling_total = 0U;
    {
        const ftl_status_e md_ret = md_load(ftl->metadata_dir, &ftl->alloc, &ftl->total_writes);

        /* 失败时仍建空 Directory（空片首启 / 格式化路径需要），但把失败状态留给
         * ftl_boot_recovery：介质若已有有效 Checkpoint，空 Directory = 映射整体丢失
         * ⇒ 必须拒绝启动（RISK-BOOT-09）。 */
        ftl->md_load_failed = (md_ret != FTL_OK) ? 1U : 0U;

        if (md_ret != FTL_OK)
        {
            md_init(ftl->metadata_dir, &ftl->alloc, &ftl->total_writes);
        }
    }

    /* ├─ 搬移引擎（GC / WL / 慢死预测）：窄上下文 + 自持节拍 ── */
    mig_init(ftl->mig_handle, &ftl->alloc, ftl->journal, ftl->cache, &ftl->total_writes, ftl->topo);

    /* ├─ Read Recovery：窄上下文（L2P / allocator / 整页暂存 / pending 表）── */
    rr_init(&ftl->read_recovery_handle, ftl->cache, &ftl->alloc, ftl->topo, ftl->rr_page_scratch, &ftl->pending);

    /* ├─ Wbuf：窄上下文（allocator / pending 表 / migrate handle）── */
    wb_init(ftl->wbuf, &ftl->alloc, &ftl->pending, ftl->mig_handle);

    /* ├─ Checkpoint：窄上下文（l2p / journal / metadata / allocator / topo）──
     * cp_init 内部整体 memset ⇒ 池游标 cursor_valid==0，首次 job 先扫池定位。 */
    cp_init(ftl->cp_handle, ftl->cache, ftl->journal, ftl->metadata_dir, &ftl->alloc, ftl->topo);

    /* ├─ 统计 ── */
    /* ├─ 后台调度器任务表（10 项；must-run + interval/budget）──
     * 必须在所有在飞的字段就绪后装载（适配器只依赖实例字段与 kick，无额外依赖）。 */
    ftl_bg_init(ftl);

    return FTL_OK;
}

/* ── 初始化契约自检（debug-only）──
 *
 * 只在 ftl_init + 设备级 owner 登记完成后跑一次，证明“初始化后不可变”的绑定。
 * 覆盖：句柄、storage 数组、模块窄上下文、topology 副本、PBA 布局、bg 任务表、
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

static void contract_check_handles(const ftl_ctx_t *ftl)
{
    ASSERT((ftl->cache != NULL) && (ftl->metadata_dir != NULL) &&
           (ftl->journal != NULL) && (ftl->cp_handle != NULL) &&
           (ftl->mig_handle != NULL) && (ftl->wbuf != NULL) && (ftl->rr_page_scratch != NULL));

    ASSERT((ftl->table_refs.blocks != NULL) && (ftl->table_refs.free_blocks != NULL) &&
           (ftl->table_refs.op_blocks != NULL) && (ftl->table_refs.spare_blocks != NULL) &&
           (ftl->table_refs.closed_sb_list != NULL));

    ASSERT((ftl->alloc.blocks == ftl->table_refs.blocks) &&
           (ftl->alloc.free_blocks == ftl->table_refs.free_blocks) &&
           (ftl->alloc.op_blocks == ftl->table_refs.op_blocks) &&
           (ftl->alloc.spare_blocks == ftl->table_refs.spare_blocks) &&
           (ftl->alloc.closed_sb_list == ftl->table_refs.closed_sb_list));
}

static void contract_check_topology(const ftl_ctx_t *ftl)
{
    const ftl_topo_t *topo = &ftl->topo;
    const DWORD       dim  = ftl_topo_dim(*topo);

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

    ASSERT((ftl->cp_handle->topo.ch == topo->ch) &&
           (ftl->cp_handle->topo.ce == topo->ce) &&
           (ftl->cp_handle->topo.lun == topo->lun) &&
           (ftl->mig_handle->topo.ch == topo->ch) &&
           (ftl->mig_handle->topo.ce == topo->ce) &&
           (ftl->mig_handle->topo.lun == topo->lun) &&
           (ftl->read_recovery_handle.topo.ch == topo->ch) &&
           (ftl->read_recovery_handle.topo.ce == topo->ce) &&
           (ftl->read_recovery_handle.topo.lun == topo->lun));
}

static void contract_check_module_contexts(const ftl_ctx_t *ftl)
{
    const md_dir_cache_t *md = ftl->metadata_dir;

    ASSERT((md->alloc == &ftl->alloc) && (md->total_writes == &ftl->total_writes));

    ASSERT(ftl->journal->alloc == &ftl->alloc);

    const cp_handle_t *cp = ftl->cp_handle;

    ASSERT((cp->cache == ftl->cache) && (cp->journal == ftl->journal) &&
           (cp->metadata_dir == ftl->metadata_dir) && (cp->alloc == &ftl->alloc));

    const mig_handle_t *mg = ftl->mig_handle;

    ASSERT((mg->alloc == &ftl->alloc) && (mg->journal == ftl->journal) &&
           (mg->cache == ftl->cache) && (mg->total_writes == &ftl->total_writes));

    const rr_handle_t *rr = &ftl->read_recovery_handle;

    ASSERT((rr->cache == ftl->cache) && (rr->alloc == &ftl->alloc) &&
           (rr->page_scratch == ftl->rr_page_scratch) && (rr->pending == &ftl->pending));

    const wb_handle_t *wb = ftl->wbuf;

    ASSERT((wb->alloc == &ftl->alloc) && (wb->pending == &ftl->pending) &&
           (wb->migrate == ftl->mig_handle));
}

static void contract_check_identity(const ftl_ctx_t *ftl)
{
    const ftl_topo_t topo = ftl->topo;
    const DWORD      dim  = ftl_topo_dim(topo);

    ASSERT(ftl->cache->owner_of_region != NULL);
    ASSERT(ct_get_route(topo.ch, topo.ce, topo.lun) == ftl);

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

        ASSERT((ct_get_route_region(global) == ftl) &&
               (lba_route_ch(lba) == ftl->cache->ch) &&
               (lba_route_ce(lba) == ftl->cache->ce));
    }
}

static void contract_check_bg(const ftl_ctx_t *ftl)
{
    ASSERT((ftl->bg.task_count > 0U) && (ftl->bg.task_count <= BG_TASK_COUNT) &&
           (ftl->bg.cfg == NULL) && (ftl->bg.level == BG_LEVEL_IDLE));

    for (BYTE i = 0U; i < ftl->bg.task_count; i++)
    {
        ASSERT(ftl->bg.tasks[i].func != NULL);
    }
}

static void contract_check_jobs_idle(const ftl_ctx_t *ftl)
{
    const jrn_handle_t *j = &ftl->journal->handle;

    ASSERT((j->phase == JRN_IDLE) && (j->current_block < JOURNAL_BLOCK_COUNT) &&
           (j->current_page < PAGES_PER_BLOCK) && (j->current_entry == 0U) &&
           (j->sequence_counter == 1U) && (j->merge_floor == 0U) && (j->need_erase == 0U) &&
           (j->checkpoint_needed == 0U) && (j->emergency == 0U) &&
           (j->write_fail_count == 0U) && (j->write_fail_pending == 0U) &&
           (j->pre_erased_block == JOURNAL_BLOCK_NONE) && (j->bad_mask == 0U) &&
           (j->erase_fail_count == 0U) && (j->ring_full == 0U));

    const cp_handle_t *cp = ftl->cp_handle;

    ASSERT((cp->active == 0U) && (cp->waiting == 0U) && (cp->failed == 0U) &&
           (cp->phase == CP_IDLE) && (cp->cp_state.advance_next == 0U) &&
           (cp->cp_state.cursor_valid == 0U));

    ASSERT((!md_is_busy(ftl->metadata_dir)) &&
           (ftl->metadata_dir->count <= METADATA_DIR_ENTRIES) &&
           ((!ftl->metadata_dir->cur_block_valid) ||
            ((ftl->metadata_dir->cur_metadata_block < FTL_BLOCK_TOTAL) &&
             (ftl->metadata_dir->cur_metadata_page <= PAGES_PER_BLOCK))));

    ASSERT((!ftl->mig_handle->active) && (!ftl->mig_handle->waiting) &&
           (ftl->mig_handle->phase == MIG_IDLE) &&
           mig_is_fail_state_clean(ftl->mig_handle));

    ASSERT((ftl->read_recovery_handle.stage == RR_STAGE_IDLE) &&
           (ftl->read_recovery_handle.waiting == 0U) &&
           (ftl->read_recovery_handle.has_owner == 0U));

    ASSERT((!ftl->wbuf->open) && (ftl->wbuf->occupied == 0U) && (ftl->wbuf->error == FTL_OK));
}

static void contract_check_boot_zero(const ftl_ctx_t *ftl)
{
    ASSERT((pend_get_count(&ftl->pending) == 0U) && (pend_get_group_count(&ftl->pending) == 0U) &&
           (ftl->total_writes == 0U) && (ftl->cache->load_inflight == 0U) &&
           (ftl->cache->flush_inflight == 0U) && (ftl->bg_suspend == 0U) &&
           (ftl->l2p_defer == 0U));
}

#endif /* !NDEBUG */

void ftl_contract_audit(const ftl_ctx_t *ftl)
{
#if defined(NDEBUG)
    (void)ftl; /* release：审计不存在，行为与今天一致 */
#else
    ASSERT(ftl != NULL);
    ASSERT(pba_fields_layout_matches());

    contract_check_handles(ftl);
    contract_check_topology(ftl);
    contract_check_module_contexts(ftl);
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
    ct_drain();
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
void ftl_service(ftl_ctx_t *ftl)
{
    /* 唯一调用点 main.c 传 &g_ftl[ch][ce][lun]（静态数组元素，结构性恒非空）；
     * NULL 属「不该发生」⇒ ASSERT（release 裁空后 NULL 解引用=HardFault 留现场，
     * 优于静默返回⇒该实例 journal/GC 永不推进的无声损坏）。 */

    /* Phase 2.4：运行期元数据 BAD 后的候选池重算（纯 RAM；等元数据 job 空闲才做）。
     * 必须早于本轮新 I/O：游标/候选表先一致，再允许分配与提交。 */
    md_pool_service(ftl);

    /* Phase 2.5 首：推进**停泊**的写确认。必须早于补发——停泊意味着那个 LBA
     * 的 L2P 还是旧映射，先补发一条读同一 LBA 的子操作就会读到旧物理页（陈旧数据）。
     * 顺序与 Phase 2.5 的既有约定一致：先处理**已完成**的，再下发新的。 */
    pend_confirm_service(&ftl->pending, ftl, NULL);

    /* Phase 2.5: 补发 host 子操作。完成先腾出 FC 资源（命令/desc/buffer 池、SQ 空位），
     * 再把各组的待下发子操作补上；顺序保证 host IO 优先于 Phase 3 的后台任务。
     * 只遍历**本实例**的组（每实例一份 groups 表）。 */
    for (BYTE i = 0U; i < FTL_GROUP_MAX; i++)
    {
        if (ftl->pending.groups[i].used != 0U)
        {
            ftl_drain_group(ftl, i);
        }
    }

    /* Phase 2.5 尾：页打包写缓冲的**确定性空闲 flush**——补发循环跑完后，若再无写组会
     * 向当前窗口 append，就把缓冲编程掉（否则这些 host 写永不完成）。放在 bg_suspend
     * 判定之前：它是 host IO 的收尾，不是后台任务。 */
    {
        wb_trigger_t trigger;
        memset(&trigger, 0, sizeof(trigger));
        trigger.kind = WBUF_TRIGGER_MAYBE_FLUSH_IDLE;
        (void)wb_handle(ftl->wbuf, &trigger, NULL, NULL);
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
 *               CW），一次 alc_alloc_pair 拿两个 plane 的页。
 * -------------------------------------------------------------------- */
static ftl_status_e ftl_issue_subop(ftl_ctx_t *ftl, WORD gi, DWORD internal_lba, void *buffer, BYTE *n_cw)
{
    pend_group_t *g = &ftl->pending.groups[gi];

    /* 夹到 pair 窗口：一个子操作不跨窗口（窗口内最多两个 plane 段、每段一个页）。
     * ⚠ L2P region 512 对齐且 pair 窗口只有 8 个 LBA ⇒ 窗口绝不跨 region。 */
    BYTE n;
    {
        const DWORD window_end = ftl_lba_pair_base(internal_lba) + FTL_LBA_PER_PAIR;
        const BYTE  avail      = (window_end - internal_lba);

        n = (*n_cw < avail) ? *n_cw : avail;
    }

    /* 写：攒进页打包缓冲（窗口满 / 切换窗口时内部触发编程），本函数对写**到此为止**。
     * 数据在 wb_handle(FLUSH) 里一次编程——分配/建链/提交/确认全在那里。 */
    if (!pend_op_is_read(g->op))
    {
        /* 回填窗口夹住后的实际消费数——调用者（ftl_drain_group）据此推进 g->next。
         * ⚠ 不可漏：内部 n 可能被窗口边界夹小（internal_lba 未 8 对齐时 avail<8），
         *   漏回填会让 g->next 越过窗口、跳过未下发的 LBA。 */
        *n_cw = n;

        wb_trigger_t trigger;
        memset(&trigger, 0, sizeof(trigger));
        trigger.kind = WBUF_TRIGGER_APPEND;
        trigger.lba  = internal_lba;
        trigger.data = buffer;
        trigger.n    = n;
        trigger.gi   = gi;

        return wb_handle(ftl->wbuf, &trigger, NULL, NULL);
    }

    /* ── 以下为读路径 ── */

    /* 命中写缓冲：该 LBA 尚未编程，从缓冲取（否则会读到陈旧 L2P 映射）。同步完成、
     * 消费 1 个 LBA（不进 outstanding）。 */
    if (wb_read(ftl->wbuf, internal_lba, buffer))
    {
        *n_cw = 1U;
        return FTL_OK;
    }

    DWORD pba[PLANE_NUM];
    for (BYTE i = 0U; i < PLANE_NUM; i++)
    {
        pba[i] = NAND_PBA_INVALID; /* 未覆盖的 plane 保持 INVALID（见 pend_req_t.new_pba 约定） */
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

            if (wb_has(ftl->wbuf, l))
            {
                break; /* 还在写缓冲（未编程）：收缩到此为止，下一轮同步从缓冲取 */
            }

            /* ⚠ 三态：FTL_AGAIN = region 未驻留（已起异步加载）。**必须**与
             * "未映射"（FTL_OK + INVALID）分清——把前者当后者会让 host 读到假数据。
             * FTL_ERROR 仅越界，此时 p 保持 INVALID。 */
            if (l2p_read(ftl->cache, l, &p) == FTL_AGAIN)
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

            /* 降级 L2P region 的未映射不是「合法未映射」：该范围的旧映射已经丢失，
             * 必须报 IO_ERROR，不能零填（静默丢数据，RISK-BOOT-18）。
             * 回放已恢复的 LBA 有有效 PBA，不会走到这里。 */
            if (l2p_is_region_degraded(ftl->cache, lba_to_global_region(internal_lba)))
            {
                DWORD suppressed = 0U;

                if (ftl_log_throttle_due(&s_lost_log, &suppressed))
                {
                    LOGE("[FTL] rd lba=%lu: mapping lost (degraded L2P region) — IO_ERROR (suppressed=%lu)\r\n",
                         internal_lba,
                         (unsigned long)suppressed);
                }
                return FTL_ERROR;
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
    if (pend_get_count(&ftl->pending) >= FTL_PENDING_MAX)
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
    ftl_status_e ret = ftl_cmd_setup_ppa_pair_cw(cmd, pba, cw_bitmap);
    if (ret != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return ret; /* FTL_AGAIN（池空，瞬态）/ FTL_ERROR 原样上抛 */
    }

    /* 注册 pending 子操作（必须早于 submit，避免竞态）。读：owners = 单组位、写专用字段传
     * INVALID、cw_bitmap 传 0；lba 传**全局** LBA（L2P/Journal/META 均按全局索引）。 */
    pend_req_t *pending = pend_alloc(&ftl->pending);

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
    pend_validate(pending);

    *n_cw = n; /* 回填实际消费的 LBA 数（调用者据此推进） */

    /* 记账（**必须早于 submit**——submit 之后 completion 可能立即被 drain 处理） */
    pend_group_add_subop(&ftl->pending, gi);

    /* 提交（tag = HOST_IO；ctx 传 NULL —— 私有状态在 pending 表） */
    if (ftl_op_submit(cmd, FTL_OP_HOST_IO, NULL) != FTL_OK)
    {
        pend_remove(&ftl->pending, ftl_cmd_id(cmd));
        (void)fc_command_release(cmd);
        pend_group_drop_subop(&ftl->pending, gi);
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
static void ftl_drain_group(ftl_ctx_t *ftl, WORD gi)
{
    pend_group_t *g = &ftl->pending.groups[gi];

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
        const ftl_status_e st = ftl_issue_subop(ftl, gi, glba, buffer, &n_cw);

        g = &ftl->pending.groups[gi]; /* 重取：下发可能改变组状态 */

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
             * ⚠ **有确定完成时刻的后台进展不算无进展**：
             *   · L2P region 加载在飞（l2p_defer）：NAND 层 wait/jump 超时保证命令必有完成；
             *   · 搬移 job 在飞（GC / WL / slow-death）：它自己带 ms deadline，且 wbuf 在
             *     Free Pool 空时正是靠它补池。一次回收要数百 ms，8 轮预算远不够，
             *     计进来会把正常等待判成 IO_ERROR（RISK-GC-09）。
             *   真卡死（命令丢失 / 搬移超时不退）由各自 deadline 与 NAND 超时兜底。 */
            const bool bg_progress = (ftl->l2p_defer != 0U) || ftl->mig_handle->active;

            if (!bg_progress)
            {
                if (++g->stalls > FTL_DRAIN_STALL_MAX)
                {
                    /* 归因：GC 已判定「无收益 victim」时，这不是普通超时，是真正的无空间
                     * （free 见底且搬不动）。打印区分行，避免与瞬态背压混淆（RISK-GC-14）。
                     * 无空间是终止态、host 会持续重试 ⇒ 用限流器，避免逐请求刷屏。 */
                    DWORD suppressed = 0U;

                    if (ftl_log_throttle_due(&s_nospace_log, &suppressed))
                    {
                        if (ftl->mig_handle->gc_no_gain)
                        {
                            LOGE("[FTL] no space: GC no-gain (free=%u) — IO_ERROR (suppressed=%lu)\r\n",
                                 (unsigned)ftl->alloc.free_count,
                                 suppressed);
                        }

                        LOGE("[FTL] drain stalled gi=%u next=%lu/%lu (suppressed=%lu)\r\n",
                             (unsigned)gi,
                             g->next,
                             g->total,
                             suppressed);
                    }
                    g->agg_status = SSD_STATUS_IO_ERROR;
                    g->next       = g->total;
                }
            }
            break;
        }

        /* FTL_ERROR：永久失败 → 弃未下发者，组最终报错（已下发的照常完成）。
         * host 会重试 ⇒ 限流打印（保留首个失败位置与抑制计数）。 */
        DWORD suppressed = 0U;

        if (ftl_log_throttle_due(&s_subop_fail_log, &suppressed))
        {
            LOGE("[FTL] subop failed gi=%u next=%lu/%lu (suppressed=%lu)\r\n",
                 (unsigned)gi,
                 g->next,
                 g->total,
                 suppressed);
        }
        g->agg_status = SSD_STATUS_IO_ERROR;
        g->next       = g->total;
        break;
    }

    pend_group_maybe_complete(&ftl->pending, gi);
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
static ftl_status_e ftl_submit_common(ftl_ctx_t *ftl, const ftl_io_req_t *req, pend_op_e op)
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
    int gi = pend_group_alloc(&ftl->pending);
    if (gi < 0)
    {
        LOGE("[FTL] %s: no free group slot\r\n", label);
        return FTL_ERROR;
    }
    pend_group_t *g    = &ftl->pending.groups[gi];
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
     * 数据在 ct_drain 中拷贝回 buffer（读），Journal + L2P 在完成
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
ftl_status_e ftl_submit_read(ftl_ctx_t *ftl, const ftl_io_req_t *req)
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
ftl_status_e ftl_submit_write(ftl_ctx_t *ftl, const ftl_io_req_t *req)
{
    return ftl_submit_common(ftl, req, PENDING_OP_WRITE);
}

/* ── 元数据落盘 / TRIM（供 HIL 调用）── */

/* --------------------------------------------------------------------
 * ftl_flush_begin / ftl_flush_step
 *   功能：异步 FLUSH job——把已持久化的映射落盘（L2P 脏行写回 + journal 尾页刷出）。
 *   参数：ftl —— 实例（**前置条件**；实例绑定由 ftl_contract_audit 在 boot 期证明）。
 *   返回：begin 无返回值（幂等）；step 返回 true = 本次 flush 已完成。
 *   实现要点：
 *     · 主流形态是**排队命令**：提交即返回，完成走 HIL 的完成通道
 *       （complete_flag / status_out，见 docs/archive/ftl_host_interface.md A.1）。
 *       旧实现在 USB SCSI 回调里同步自旋，窗口内主循环无法抽 USB 事件
 *       ⇒ 事件 FIFO 溢出（RISK-USB-06）。
 *     · 语义仍是 best-effort：不等待 host 在飞的写（BOT 串行 + CLI gate 已保证
 *       提交时没有同类请求）；journal 持久性失败直接结束并打 WRN。
 *     · wbuf / L2P / journal 三者都异步推进：每轮 step 各踢一次，由主循环持续运行。
 * -------------------------------------------------------------------- */

/* host flush 也是"该落一次 checkpoint"的信号：没有 CP 时，掉电后只能拿初始 CP
 * （无块表）做全量重建。异步启动，限流 5s（半个 T3），避免 host 频繁 flush 写放大。 */
static void ftl_flush_kick_cp(ftl_ctx_t *ftl)
{
    const DWORD now_ms = ftl_job_now_ms();

    if ((now_ms == 0U) || ((now_ms - ftl->cp_handle->last_ms) < (CP_TIME_INTERVAL_MS / 2U)))
    {
        return;
    }

    cp_trigger_t trigger;

    memset(&trigger, 0, sizeof(trigger));
    trigger.now_ms            = now_ms;
    trigger.total_writes      = ftl->total_writes;
    trigger.journal_seq       = jrn_get_sequence(ftl->journal);
    trigger.jrn_get_watermark = jrn_get_watermark(ftl->journal);
    trigger.emergency         = jrn_is_emergency(ftl->journal);
    trigger.force             = true;
    trigger.reason            = CHECKPOINT_REASON_EXPLICIT;

    cp_status_t status;
    cp_handle(ftl->cp_handle, &trigger, &status);
}

void ftl_flush_cancel(ftl_ctx_t *ftl)
{
    /* 只摘 job 标志：wbuf / L2P / journal 的在飞操作有各自的服务路径，
     * 继续完成不会破坏不变量。下一次 begin 以新的起点重新开始。 */
    ftl->flush_active = false;
}

void ftl_flush_begin(ftl_ctx_t *ftl)
{
    if (ftl->flush_active)
    {
        return; /* 幂等：已有在飞的 flush */
    }

    ftl->flush_active   = true;
    ftl->flush_start_ms = ftl_job_now_ms();

    /* 先把页打包缓冲里尚未编程的写编程掉：它们还没进 L2P/journal，"落盘"不成立。
     * 遇瞬态背压会保留缓冲，后续 step 的 MAYBE_FLUSH_IDLE 会重试。 */
    wb_trigger_t trigger;

    memset(&trigger, 0, sizeof(trigger));
    trigger.kind = WBUF_TRIGGER_FLUSH;
    (void)wb_handle(ftl->wbuf, &trigger, NULL, NULL);
}

bool ftl_flush_step(ftl_ctx_t *ftl)
{
    if (!ftl->flush_active)
    {
        return true; /* 没有在飞的 flush：对调用者而言就是"已完成" */
    }

    const DWORD dim = ftl_topo_dim(ftl->topo);

    /* ① wbuf：把打开的窗口推下去，并取当前状态。 */
    wb_trigger_t wb_trigger;

    memset(&wb_trigger, 0, sizeof(wb_trigger));
    wb_trigger.kind = WBUF_TRIGGER_MAYBE_FLUSH_IDLE;

    wb_status_t wb_status;
    (void)wb_handle(ftl->wbuf, &wb_trigger, NULL, &wb_status);

    /* ② L2P：继续 kick 本 owner 的脏行（受共享 credit 池门控）。 */
    l2p_flush_kick_owner(ftl->cache, dim, L2P_CACHE_SIZE);

    /* ③ Journal：推进异步刷页 job。 */
    const jrn_trigger_e jrn_trigger = JOURNAL_TRIGGER_FLUSH;
    jrn_status_t        jrn_status;
    jrn_handle(ftl->journal, &jrn_trigger, NULL, &jrn_status);

    /* ④ 完成判据：四类在飞数据全部落定。停泊中的写确认也必须先完成——它对应的
     *    L2P 脏行还没出现，否则 flush 会「提前完成」。 */
    const bool confirm_pending = pend_find_op(&ftl->pending, PENDING_OP_WRITE_CONFIRM, NULL);
    const bool wb_done         = (wb_status.state == WBUF_STATE_IDLE) && (wb_status.occupied == 0U);
    const bool l2p_done        = l2p_flush_owner_idle(ftl->cache, dim);
    const bool jrn_done        = jrn_is_idle(ftl->journal) && !jrn_has_pending_entries(ftl->journal);
    const bool jrn_fail        = jrn_has_write_failure(ftl->journal);
    const bool wb_fail         = (wb_status.state == WBUF_STATE_FAILED);
    const bool fail            = jrn_fail || wb_fail;

    /* 墙钟上限：元数据侧真卡死时 host FLUSH 不能永不完成（见 FTL_FLUSH_TIMEOUT_MS）。 */
    const DWORD flush_ms = ftl_job_now_ms() - ftl->flush_start_ms;
    const bool  timeout  = (ftl->flush_start_ms != 0U) && (flush_ms >= FTL_FLUSH_TIMEOUT_MS);

    if (!((wb_done && l2p_done && jrn_done && !confirm_pending) || fail || timeout))
    {
        return false;
    }

    ftl->flush_active = false;

    DWORD suppressed = 0U;

    if (fail)
    {
        /* best-effort：journal 页写/擦或 wbuf 编程持续失败。CP 成功后
         * jrn_clear_write_failure 清 journal 标志；wbuf FAILED 由下一次成功操作清。 */
        if (ftl_log_throttle_due(&s_flush_err_log, &suppressed))
        {
            LOGW("[FTL] flush: write failure pending (jrn=%u wb=%u) — complete best-effort (suppressed=%lu)\r\n",
                 (unsigned)jrn_fail,
                 (unsigned)wb_fail,
                 suppressed);
        }
    }
    else if (timeout)
    {
        if (ftl_log_throttle_due(&s_flush_err_log, &suppressed))
        {
            LOGW("[FTL] flush timeout %lums (wb=%u l2p=%u jrn=%u) — best-effort complete (suppressed=%lu)\r\n",
                 flush_ms,
                 (unsigned)wb_done,
                 (unsigned)l2p_done,
                 (unsigned)jrn_done,
                 suppressed);
        }
    }

    ftl_flush_kick_cp(ftl);

    /* 耗时日志限流 5 s（单线程 superloop；多实例共享一个时间戳即可）。host 若
     * 频繁 fsync，逐次打印会刷屏。 */
    const DWORD now_log = ftl_job_now_ms();

    if ((ftl->flush_start_ms != 0U) && (flush_ms >= FTL_FLUSH_SLOW_LOG_MS) && !timeout &&
        ((s_flush_log_ms == 0U) || ((now_log - s_flush_log_ms) >= FTL_JOB_LOG_SLOW_INTERVAL_MS)))
    {
        s_flush_log_ms = now_log;
        LOGW("[FTL] flush took %lums (async job)\r\n", flush_ms);
    }

    return true;
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
void ftl_trim(ftl_ctx_t *ftl, QWORD lba, DWORD count)
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
         * 由 wb_handle(DROP) 收口，本函数不手工维护。 */
        if (wb_has(ftl->wbuf, l))
        {
            wb_trigger_t trigger;
            memset(&trigger, 0, sizeof(trigger));
            trigger.kind = WBUF_TRIGGER_DROP;
            trigger.lba  = l;
            (void)wb_handle(ftl->wbuf, &trigger, NULL, NULL);
            continue;
        }

        /* 查旧映射：TRIM 走**同步**逃生口（自旋泵）——它由 CLI 调用，不在完成派发里，
         * 且契约是"取消映射同步完成"。读得 INVALID 与"未映射"同义，两种都是"无旧映射"。 */
        (void)l2p_read_sync(ftl->cache, l, &old_pba);

        /* ⚠ **Journal 先于 L2P 写**（顺序不可换，同 host 写路径 ftl_confirm_write）。
         * 取消映射也必须进 Journal（new_pba = INVALID）：L2P 的每一次变更都由 Journal
         * 承载，掉电恢复靠回放重建——漏记这一条会让**掉电恢复把已 TRIM 的 LBA 复活**
         * （region 虽被 flush 成 INVALID，但回放按序重放更早的写条目——page_seq 更小
         * 但 > cp_sequence——会把它改回有效 PBA，且回放行标 DIRTY 后
         * ftl_boot_apply_region_bitmap 会跳过预载，NAND 上的 INVALID 也不生效）。
         * 反过来若先写 L2P 再记 Journal，掉电就落在这个窗口：L2P 已 INVALID 而 Journal
         * 无条目 ⇒ 回放不覆盖 ⇒ 该 region 若随后被淘汰重载，映射复活。 */
        (void)jrn_append(ftl->journal, l, old_pba, NAND_PBA_INVALID);

        (void)l2p_write_sync(ftl->cache, l, NAND_PBA_INVALID);

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

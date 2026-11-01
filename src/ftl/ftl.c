/*
 * ftl.c — FTL 顶层（初始化 / 后台调度服务主循环 / 子操作下发 / 对外读写入口）
 *
 * 职责：
 *   FTL 的对外门面与主循环——ftl_init 装配各子模块；ftl_service 是**服务主循环**
 *   （NAND 泵 → 完成泵 → 补发 host 子操作 → 时间预算后台调度）；ftl_submit_read/write
 *   把一条 host 请求拆成多个 FC 子操作下发；ftl_flush_begin/step / ftl_trim_begin/step
 *   供 HIL 调用（均为异步推进）。
 *
 * 与子模块的分工：
 *   本文件             —— 编排：调度、拆分、完成聚合、模式切换；不解析介质格式。
 *   pending.c          —— 子操作登记 + 组完成聚合（一条 host 请求 = 一个 group）。
 *   ftl_op.c           —— ctx 旁表 + 实例路由 + 提交 + CQ 排空/派发（Phase 2）。
 *   其余模块（gc / wl / journal / checkpoint / bbt / l2p_cache）各司其职，
 *   见各自文件头。
 *
 * 服务主循环（ftl_service）四阶段：
 *   Phase 1    fc_service × NAND_SUBMIT_BUDGET —— 推进 NAND
 *   Phase 2    ftl_op_drain —— 统一完成泵（按命令 op_tag 派发到服务函数数组）
 *   Phase 2.5  逐组补发 host 子操作（host IO 优先于后台任务）
 *   Phase 3    bg_scheduler_service —— 时间预算后台调度（本文件只有一行调用）
 *
 * 文件结构（与下方分区注释一一对应）：
 *   1. 内部辅助        ftl_confirm_write / ftl_host_io_callback（+ 4 个 ftl_host_io_* 完成
 *                      分支）/ pend_confirm_service
 *   2. 对外接口        ftl_init / ftl_service_pump / ftl_service
 *   3. 子操作下发      ftl_issue_subop / ftl_drain_group（static）+ ftl_submit_read / write
 *   4. 元数据落盘/TRIM ftl_flush_begin/step / ftl_trim_begin/step
 *
 * ⚠ 后台调度在 `ftl/bg_scheduler.c`：10 项任务表 / 档位 IDLE·NORMAL·BUSY → 预算规模 /
 *   mig_gc_evaluate（GC 统一触发求值）/ RR 巡检 / Phase 3 入口 bg_scheduler_service；
 *   通用引擎也在同一文件。本文件不持有任何后台策略。
 *
 * ⚠ 页打包写缓冲在 `ftl/wbuf.c`（wbuf.h 定义 wb_handle_t），本文件只调用其接口。
 *
 * ⚠ 回收动作在 `ftl/migrate.c`：GC / WL / 慢死预测共用一台异步搬移 job
 *   （mig_gc_begin / mig_wl_begin / mig_predict_begin / mig_handle）。触发求值在
 *   `ftl/bg_scheduler.c`，写路径的按需 GC 在
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
#include "cache_batch.h"
#include "checkpoint.h"
#include "fault_inject.h" /* 测试镜像专属注入点（ISS-FTL-16，FC_NAND_TEST_ENABLE 下编入） */
#include "fc.h"
#include "ftl_fc_interface.h"
#include "ftl_job.h" /* ftl_job_now_ms（flush 触发 CP 的定时） */
#include "ftl_op.h"  /* ftl_op_submit / FTL_OP_* */
#include "log.h"
#include "pending.h"
#include "ssd_compiler.h"
#include "wbuf.h"

#include <string.h>

// FTL 每 I/O 热数据。链接脚本按整个 ftl.o 把 .bss 放进 DTCM。
// 这些对象都是 CPU-only：NAND/FC 的 DMA buffer 不在这里，DMA1/2 也访问不到 DTCM。
// `.dtcm_data` 是 NOLOAD ⇒ ftl_init 对每个实例显式 memset。
ftl_ctx_t   g_ftl[CH_NUM][CE_NUM][LUN_NUM];
wb_handle_t g_ftl_wbuf[FTL_DIM_TOTAL];
l2p_jrn_t   g_ftl_journal[FTL_DIM_TOTAL];

// dangling 守卫日志：
// · `st=BAD` 是**预期降级**（旧块已退役，数据本不可读）⇒ 每个旧 gblock 只打一行，
// 其余 LBA 只累计到该槽位的 lbas（一个块 256 个 LBA，逐条打必刷屏）。
// · `st=FREE/LOG/NONE` 是**非预期缺陷** ⇒ 仍用限流 ERROR，保留完整现场。
#define FTL_DANGLING_SLOTS (8U)

typedef struct
{
    WORD  g;         /* 旧 gblock；lbas==0 表示空槽 */
    DWORD first_lba; /* 该块第一个命中的 LBA */
    DWORD lbas;      /* 该块累计命中的 LBA 数 */
} ftl_dangling_ent_t;

static ftl_dangling_ent_t s_dangling[FTL_DANGLING_SLOTS];

/* 定义在下方「确认路径」之后；三个减法入口（覆盖写 / 填洞未映射 / TRIM）共用。 */
static void ftl_dangling_check(ftl_ctx_t *ftl, DWORD l, DWORD old_pba, WORD new_g);
static BYTE s_dangling_next;

// 非预期 dangling 的限流（1 s 一行 + 被抑制条数）。
static DWORD s_dangling_log_ms;
/* cache 读命令失败（ECC/CRC/EMPTY 等）的限流时间戳：window 级错误通常成片出现，
 * 1 s 一条足够定位「错在哪类页、哪个 PBA」。 */
static DWORD s_cache_rd_err_log_ms;
static DWORD s_dangling_suppressed;

// 日志限流（RISK-USB-06）：同一类异常在短时间内重复出现时，5 s 打一行并给出被
// 抑制条数；平台时钟缺失时退化为 64 次一行。各类别用独立实例（互不抑制）。
typedef struct
{
    DWORD ms;         /* 上次打印时刻（0 = 尚未打印） */
    DWORD suppressed; /* 自上次打印以来被抑制的条数 */
} ftl_log_throttle_t;

/**
 * @brief 日志限流判定：到期则放行并清零抑制计数，未到期则累计后拒绝。
 *
 * @param t 该日志类别的限流状态（`ms` / `suppressed` 由本函数更新）
 * @param out_suppressed 放行时写入自上次打印以来被抑制的条数（只在返回 true 时写）
 * @return true  本次应打印
 *         false 继续抑制
 *
 * @note 到期判据：抑制满 64 条，或平台时钟可用且距上次打印已达
 * FTL_JOB_LOG_SLOW_INTERVAL_MS；时钟缺失（now_ms == 0）时退化为 64 条一行。
 */
static bool ftl_log_throttle_due(ftl_log_throttle_t *t, DWORD *out_suppressed)
{
    const DWORD now_ms = ftl_job_now_ms();

    // 有时间源时**按时间**限流；条数只作无时钟时的兜底——否则高频事件
    // （如门控期间的背压）一秒撞满 64 条，日志仍会成串。
    bool due;

    if (now_ms != 0U)
    {
        due = (t->ms == 0U) || ((now_ms - t->ms) >= FTL_JOB_LOG_SLOW_INTERVAL_MS);
    }
    else
    {
        due = (t->suppressed >= 64U);
    }

    if (due == false)
    {
        t->suppressed++;
        return false;
    }

    *out_suppressed = t->suppressed;
    t->suppressed   = 0U;
    t->ms           = now_ms;
    return true;
}

// flush 日志限流（RISK-USB-06；host 频繁 fsync 时避免刷屏）：
// s_flush_log_ms —— `[FTL] flush took`（正常路径）；
// s_flush_err    —— 失败 / 超时（异常路径）。
static DWORD              s_flush_log_ms;
static ftl_log_throttle_t s_flush_err_log;

// 降级 region 的读错误日志（RISK-BOOT-18）：一次 host 校验 / 文件系统扫描会连续
// 读同一 region 的数百个 LBA，逐条 LOGE 会写满 UART TX 环并拖慢设备。
static ftl_log_throttle_t s_lost_log;

// no-space 归因日志：无空间是终止态，host 会持续重试 ⇒ 逐组打两行会刷屏。
static ftl_log_throttle_t s_nospace_log;

// subop 永久失败日志：host 重试同一个坏 LBA / 降级 region 会反复触发 ⇒ 同样限流。
static ftl_log_throttle_t s_subop_fail_log;

// host cache program 批次失败：host 反复重试同一坏页时每个批次都会失败 ⇒ 限流。
static ftl_log_throttle_t s_cache_wb_fail_log;

/* ── 内部辅助 ── */

static bool ftl_cache_wr_has_unproven(const ftl_ctx_t *ftl);
static void ftl_cache_wr_abort(ftl_ctx_t *ftl);

// 组下发机制（定义在文件后部；ftl_service Phase 2.5 先用到）。
/**
 * @brief 只清 RAM：关闭 cache 序列状态（不发任何设备命令）。
 *
 * @param ftl 实例。
 *
 * @note 设备侧未收 LAST 的缓存序列由随后的新命令终止（RISK-IF-09，待上板确认）。
 *       本函数与 idle 守卫不在每 I/O 热路径上，放内部 Flash，不占 ITCM。
 */
void SSD_CODE_FAST ftl_cache_seq_release(ftl_ctx_t *ftl)
{
    if ((ftl->cache_seq.kind == FTL_CACHE_SEQ_WRITE) && (ftl_cache_wr_has_unproven(ftl) == true))
    {
        ftl_cache_wr_abort(ftl); /* 等不到结论的页：标记失败 + 关 SB + 清状态 */
        return;
    }

    // 序列被放弃时要分清「命令还在飞」与「已经排空」：前者必须继续关着门控，
    // 否则别的组会在 31h/3Fh 管线半途插命令（RISK-CACHE-14）；后者才真正放开。
    const WORD gi        = ftl->cache_seq.owner_gi;
    const bool in_flight = (ftl->cache_seq.kind == FTL_CACHE_SEQ_READ) && (gi < FTL_GROUP_MAX) && (ftl->pending.groups[gi].used == true) && (ftl->pending.groups[gi].outstanding != 0U);

    ftl->cache_seq.kind            = FTL_CACHE_SEQ_NONE;
    ftl->cache_seq.active          = false;
    ftl->cache_seq.draining        = in_flight;
    ftl->cache_seq.break_requested = false;
    ftl->cache_seq.idle_ms         = 0U;
    ftl->cache_seq.run_pairs       = 0U;
    ftl->cache_seq.closing         = false;
    ftl->cache_seq.close_requested = false;
    ftl->cache_seq.own_page0       = false;
}

/**
 * @brief 让出设备序列：写序列请求 padding 收尾，读序列直接放弃（不发 LAST）。
 *
 * @param ftl 实例
 *
 * @note 需要写 Journal/L2P 的前台控制路径（flush / trim）在入口调用：序列不退出，
 *       这些元数据提交会被门控，flush/trim 只能等空闲守卫。写序列不能 abort
 *       （提前回执页未确认），所以走 padding END。
 */
static void SSD_CODE_FAST ftl_cache_seq_yield(ftl_ctx_t *ftl)
{
    if (ftl->cache_seq.active == false)
    {
        return;
    }

    if (ftl->cache_seq.kind == FTL_CACHE_SEQ_WRITE)
    {
        ftl->cache_seq.close_requested = true;
    }
    else
    {
        ftl_cache_seq_release(ftl);
    }
}

/**
 * @brief 组被弃/失败时关闭它持有的 cache 序列。
 *
 * @param ftl 实例。
 * @param gi  组下标。
 *
 * @note 不关闭会让序列门控永久挡住该 LUN 的所有提交（门控只认 owner_submit）。
 */
static void SSD_CODE_FAST ftl_cache_seq_close(ftl_ctx_t *ftl, WORD gi)
{
    if ((ftl->cache_seq.active == true) && (ftl->cache_seq.owner_gi == gi))
    {
        ftl_cache_seq_release(ftl);
    }
}

/**
 * @brief 序列守卫：两件事 —— ① `draining` 的排空判定；② chunk 边界序列等下一 chunk 的
 *        空闲超时。
 *
 * @param ftl 实例。
 *
 * @note ① `draining`：所有者组的在飞子操作归零 ⇒ 清 `draining` 放开 LUN；超过
 *       FTL_CACHE_SEQ_IDLE_MS 也强制清（组异常消失时不能把 LUN 永久锁死）。
 *       ② `active`：序列跨 host 传输段 / 跨 host 命令保持打开，等同一 LBA 流的下一段；
 *       所有者组已释放且超时无人接手 ⇒ 写序列置 `close_requested`（service 用 padding
 *       END 收尾，不丢提前回执页）；读序列直接释放（不发 LAST）。
 *       两者时钟缺失（now_ms == 0）时都不计时。
 */
static void SSD_CODE_FAST ftl_cache_seq_idle_guard(ftl_ctx_t *ftl)
{
    // 排空守卫：窗口已提交、命令仍在飞 ⇒ 门控继续关着，直到所有者组的在飞子操作
    // 归零（或 1 s 兜底：组异常消失时不能把 LUN 永久锁死）。
    if (ftl->cache_seq.draining == true)
    {
        const WORD gi        = ftl->cache_seq.owner_gi;
        const bool in_flight = (gi < FTL_GROUP_MAX) && (ftl->pending.groups[gi].used == true) && (ftl->pending.groups[gi].outstanding != 0U);

        if (in_flight == false)
        {
            ftl->cache_seq.draining = false;
            ftl->cache_seq.idle_ms  = 0U;
            return;
        }

        const DWORD now_ms = ftl_job_now_ms();

        if ((now_ms == 0U) || (ftl->cache_seq.idle_ms == 0U))
        {
            if (now_ms != 0U)
            {
                ftl->cache_seq.idle_ms = now_ms;
            }
            return;
        }

        if ((now_ms - ftl->cache_seq.idle_ms) > FTL_CACHE_SEQ_IDLE_MS)
        {
            ftl->cache_seq.draining = false;
            ftl->cache_seq.idle_ms  = 0U;
        }
        return;
    }

    if (ftl->cache_seq.active == false)
    {
        return;
    }

    // 所有者组仍在**下发**本序列的页 ⇒ 有进展，不计空闲。
    // 组已释放（读）或已把本 chunk 全部下发、在等下一 chunk 确认尾页（写）⇒ 开始计时。
    const bool owner_valid    = (ftl->cache_seq.owner_gi < FTL_GROUP_MAX) && (ftl->pending.groups[ftl->cache_seq.owner_gi].used == true);
    const bool owner_draining = (owner_valid == true) && (ftl->pending.groups[ftl->cache_seq.owner_gi].next < ftl->pending.groups[ftl->cache_seq.owner_gi].total);

    if (owner_draining == true)
    {
        ftl->cache_seq.idle_ms = 0U; /* 所有者仍在推进：不计空闲 */
        return;
    }

    const DWORD now_ms = ftl_job_now_ms();

    if ((now_ms == 0U) || (ftl->cache_seq.idle_ms == 0U))
    {
        if (now_ms != 0U)
        {
            ftl->cache_seq.idle_ms = now_ms;
        }
        return;
    }

    if ((now_ms - ftl->cache_seq.idle_ms) > FTL_CACHE_SEQ_IDLE_MS)
    {
        if (ftl->cache_seq.kind == FTL_CACHE_SEQ_WRITE)
        {
            // 写序列：没有新数据页可发 END ⇒ 请 service 用 padding END 有序收尾
            // （提前回执后最后一页未确认，不能 abort）。
            ftl->cache_seq.close_requested = true;
        }
        else
        {
            ftl_cache_seq_release(ftl); /* 读序列放弃不发 LAST（RISK-IF-09） */
        }
    }
}

static void ftl_drain_group(ftl_ctx_t *ftl, WORD gi);

/**
 * @brief 写子操作成功后的**逐 CW** 簿记——Journal + L2P + BST 有效计数/「写过」位。
 *
 * @param ftl 实例
 * @param pending 已完成的写子操作记录。
 *
 * @return FTL_OK    已全部应用
 *         FTL_AGAIN 有 region 未就绪（**一条都没应用**，调用者下轮重来）；只有这两态——
 *                   就绪前置挡住了唯一的失败源，第二遍不可能失败。
 *
 * @note 实现要点：· CW 级映射下 1 LBA = 1 CW：一个子操作覆盖 pending->n_cw 个连续**全局** LBA，
 * 每个 LBA 各自一条 Journal + 一条 L2P 更新（Journal 流量 ×4，已登记）。
 * · 目标页按 **LBA 的 plane 位**选：plane0 的 LBA 落 pending->new_pba、plane1 的
 * 落 pending->new_pba1。Policy B 下两 plane 独立选块，且「物理 plane 必须等于
 * LBA 的 plane 位」是页内 META 能还原出正确 LBA 的前提（ftl_config.h §三）。
 * · **不存 old_pba**：完成时按 LBA 重查 L2P 得"此刻被替换的映射"。同一 LBA 的两次
 * 写在飞时，先完成的先改 L2P，后完成的重查即得前者；提交时快照会让两条 Journal
 * 都指向最初映射，链断。⚠ **停泊没有削弱这条**：本函数仍是在**完成时刻**执行
 * （停泊只是延后几轮，顺序仍按完成序），重查语义不变。
 * · valid_count 单位是 **CW**：新 CW +1、被替换的旧 CW -1。真有效性一律逐 CW
 * 由页内 META + L2P 反查现判（DRAM-less approach ③，无位图）——见 bst.h 文件头。
 * · ⚠ **两遍式**：第一遍只做**就绪前置**（region 未驻留则起异步加载并返
 * FTL_AGAIN）与 Journal 容量预留，第二遍才动账。绝不能边查边改——l2p_write 对
 * 正在加载/写回的行返 FTL_AGAIN，中途放弃会让同一页里部分 CW 已记账、部分没记，
 * 重试时重复记账（valid_count 虚高、journal 重复条目、total_writes 多计）。
 * **两遍之间无泵**：本函数不同步刷 Journal、不调用 ftl_sync_submit_wait；
 * Journal 容量不足时 jrn_reserve 只起异步刷页并返 FTL_AGAIN，确认阶段不碰
 * NAND、不重入完成派发 ⇒ 就绪判定与第二遍之间状态不可能变。
 */
/**
 * @brief 停泊的依赖是否**确实有待推进的工作**（RISK-HOST-05 支路 (a) 的判据）。
 *
 * @param ftl 实例。
 * @return true = 至少一项在飞或已排队：L2P region 加载 / region 写回 / Journal 页写 /
 *                Journal 换块擦除（`need_erase`）/ Journal 待刷条目。
 *
 * @note 有 ⇒ 这次等待是**有限**的（各自都有完成点），应延长停泊预算而不是把后台拥塞
 *       结算成 host 写失败；全都没有 ⇒ 结构性卡死，仍让 `FTL_GROUP_DRAIN_TIMEOUT_MS`
 *       到期暴露。陷阱：Journal 换块擦除提交失败期间 `phase` 仍是 IDLE，只看
 *       `jrn_is_idle()` 会漏掉这一类（表现为 `confirm park timeout`）。
 */
static bool ftl_confirm_progress_pending(const ftl_ctx_t *ftl)
{
    // 还有**别的**主机组在飞 ⇒ 前台仍在推进：此刻 FC 池/SQ 被前台占满是预期，
    // L2P 写回与 region 加载只是暂时拿不到资源（`no evictable line` ⇒ 加载起不动），
    // 资源随前台腾出就会释放 ⇒ 延长预算。
    // 只剩停泊项自己这一组 ⇒ 没有任何别的事可等，结构性卡死 ⇒ 让 100 ms 到期结算。
    WORD used_groups = 0U;

    for (BYTE i = 0U; i < FTL_GROUP_MAX; i++)
    {
        if (ftl->pending.groups[i].used == true)
        {
            used_groups++;
        }
    }

    return (ftl->cache->load_inflight > 0U) || (ftl->cache->flush_inflight > 0U) || (jrn_is_idle(ftl->journal) == false) || (jrn_has_erase_pending(ftl->journal) == true) || (jrn_has_pending_entries(ftl->journal) == true) || (used_groups > 1U);
}

/* 一页的「确认描述」：wbuf 路径从 pend_req_t 填、cache 路径从已编程页表填——
 * 单一定义，避免两套口径（C9）。 */
typedef struct
{
    DWORD lba;      /* 页基址（pair 对齐） */
    DWORD new_pba;  /* plane0 页 PBA */
    DWORD new_pba1; /* plane1 页 PBA */
    BYTE  cw_bitmap;
    bool  seq_page; /* 顺序页（cache program）⇒ Journal 走 jrn_append_seq */
} ftl_page_desc_t;

static ftl_status_e ftl_confirm_write(ftl_ctx_t *ftl, const ftl_page_desc_t *d)
{
    // 两个调用点（ftl_host_io_callback 的 `if (pending != NULL)` 内 / pend_confirm_service 的
    // `&ftl->pending[i]`）都保证非空 ⇒ 不在本函数复检（§十附 / §二十六）。
    // 同理 d->lba 恒为**窗口基址**（唯一写点是 wb_handle(FLUSH) 的 wbuf.base）——
    // 下面用 ftl_lba_plane/cw(l) 取代"从位下标反解坐标"就依赖这条。

    // 页对基址：8 个 LBA 属于同一 region ⇒ 就绪只需探一次（原来逐 CW 8 次）。
    ASSERT(ftl_lba_is_pair_aligned(d->lba));

    const DWORD mask = d->cw_bitmap & ((1U << FTL_LBA_PER_PAIR) - 1U);

    if ((mask != 0U) && (l2p_is_ready(ftl->cache, d->lba) == false))
    {
        // 停泊预算按**进展**延长（RISK-HOST-05 支路 (a)）：确有 region 加载在飞 ⇒ 这次
        // 等待是有限的（加载要么完成、要么三次失败后降级），不该把后台拥塞（CP 提交被
        // 门控 / Journal 换块擦在飞）变成 host 写失败。加载**起不动**（`load_inflight==0`）
        // 时不延长 ⇒ 仍保留 100 ms 的有界失败。
        return FTL_AGAIN; /* 已起加载：本轮**一条都不动** */
    }

    // Journal 容量/坐标在动账前判定：不足或页型不符时只起异步刷页并返回 false，
    // 本函数直接停泊重试，绝不在这里同步刷 Journal（那会重入完成派发）。
    // 按最坏情况（mask 置位数）预留并留 1 个空槽 ⇒ 随后的 append 不会触发页满分支。
    // 两种页型走各自的入口：顺序写确认 = jrn_seq_ready + jrn_append_seq（4B 隐式
    // LBA），其余（wbuf / GC / page0）= jrn_reserve + jrn_append（16B 显式 LBA）。
    const BYTE n_entries = ftl_cw_bitmap_popcount(mask);
    // cache program 页（cache_role != NONE）属于顺序写流 ⇒ Journal 走顺序映射页
    // （4B/条、LBA 由页头基准 + 下标隐式给出）；其余（wbuf / GC / page0）走通用页。
    const bool seq_page  = d->seq_page;

    if (n_entries != 0U)
    {
        const bool reserved = (seq_page == true) ? jrn_seq_ready(ftl->journal, d->lba, n_entries)
                                                 : jrn_reserve(ftl->journal, n_entries);

        if (reserved == false)
        {
            // 同上：Journal 页写 / 换块擦在飞 ⇒ 背压有限，延长预算；Journal 完全空闲却
            // 取不到容量（环满且擦不动）⇒ 不延长，保留超时把问题暴露成写失败。
            return FTL_AGAIN;
        }
    }

    DWORD oldp[FTL_LBA_PER_PAIR];
    DWORD newp[FTL_LBA_PER_PAIR];

    // 页内批量读：l2p_line_find 一次；只读取 mask 置位项（逐项记 hit，口径不变）。
    // 就绪前置已保证命中；非 OK 仅越界（不可能）⇒ 保守重试，尚未动账。
    if (l2p_entries_read(ftl->cache, d->lba, mask, oldp) != FTL_OK)
    {
        // 第三处 AGAIN：行在就绪检查之后变成 FLUSHING（写回在飞）⇒ 批量读被拒。
        // 与另两处同口径延长停泊预算（`flush_inflight > 0` 会命中），否则 100 ms
        // 到期把一次**有限**的写回等待结算成 host 写失败（RISK-HOST-05）。
        return FTL_AGAIN;
    }

    /* ── 先 Journal（掉电回放顺序），再一次性更新 L2P 行 ── */
    for (BYTE bit = 0U; bit < FTL_LBA_PER_PAIR; bit++)
    {
        if ((mask & (1U << bit)) == 0U)
        {
            continue;
        }

        const DWORD l    = (d->lba + bit);
        const BYTE  pl   = ftl_lba_plane(l);
        const BYTE  cw   = ftl_lba_cw(l);
        const DWORD page = (pl == 0U) ? d->new_pba : d->new_pba1;

        newp[bit] = pba_with_cw(page, cw);

        if (seq_page == true)
        {
            // 顺序页只记 new_pba；坐标/容量已由 jrn_seq_ready 全批判过 ⇒ 这里的
            // 失败是结构性簿记错误。丢弃条目会让已确认的写在掉电后丢映射，
            // 故 debug 留现场（C7：传错 lba 即可构造失败），release 也必须留下日志
            // （H14：ASSERT 与运行期留痕不是二选一）。
            const bool appended = jrn_append_seq(ftl->journal, l, newp[bit]);

            if (appended == false)
            {
                LOGE(LOG_TAG_FTL "seq journal append failed lba=%lu — mapping not persisted\r\n", l);
            }
            ASSERT(appended);
        }
        else
        {
            (void)jrn_append(ftl->journal, l, oldp[bit], newp[bit]);
        }
    }

    // 整页一次写 L2P（state=DIRTY + region 脏位只置一次）。就绪前置 + 两遍之间
    // 无泵 ⇒ 行始终保持 CLEAN/DIRTY，写不可能返 FTL_AGAIN；断言仅作结构破坏的
    // debug 探针（release 由前述不变量保证，不靠断言兜底）。
    const ftl_status_e wst = l2p_entries_write(ftl->cache, d->lba, mask, newp);

    ASSERT(wst == FTL_OK);
    (void)wst;

    for (BYTE bit = 0U; bit < FTL_LBA_PER_PAIR; bit++)
    {
        if ((mask & (1U << bit)) == 0U)
        {
            continue;
        }

        const DWORD l     = (d->lba + bit);
        const DWORD new_p = newp[bit];
        const WORD  newg  = ftl_pba_gblock(new_p);

        // 飞行记录器：记录成功确认的 host 写（kind=0）。
        alc_trace_record(&ftl->alloc, l, new_p, ALC_TRACE_WRITE_OK);

        ftl_block_add_valid(&ftl->alloc.blocks[newg]);

        // 覆盖写：旧 CW 失效。旧页若正是本页（同一 CW 重写）则不动计数。
        if ((oldp[bit] != NAND_PBA_INVALID) && (oldp[bit] != new_p))
        {
            const WORD gob = ftl_pba_gblock(oldp[bit]);

            ftl_dangling_check(ftl, l, oldp[bit], newg);

            // 坏块 / count 已为 0 时跳过：bbt_mark_bad 清过账，再减会断言/回绕。
            ftl_block_sub_valid_if_live(&ftl->alloc.blocks[gob]);
        }
    }

    jrn_watermark_update(ftl->journal);
    ftl->total_writes++; /* 计「编程次数」（页打包后一页 = 一次编程），不是 CW 数 */
    return FTL_OK;
}

/**
 * @brief 覆盖/解除映射前的**常驻守卫**：L2P 指向的旧块在 BST 里 0 有效时留现场。
 *
 * 触发条件：`valid_count == 0` 却仍被 L2P 引用。此后每次覆盖写该 LBA 都会撞
 * `bst.h` 的 `valid_count > 0U` 断言；运行期被 `sub_valid_if_live` 静默夹住，
 * 但 journal 条目照样落盘 ⇒ 下次开机回放 `pfr_valid_sub` 判**下溢** ⇒ `ctx_ok=0`
 * ⇒ 整份块元数据快照作废（full metadata rebuild）。这是「重建漏认 / 分配器复用
 * 活块」类缺陷的**唯一故障点**，三个减法入口（覆盖写 / 填洞未映射 / TRIM）都必须过它。
 *
 * @param ftl     FTL 实例
 * @param l       LBA（日志定位）
 * @param old_pba L2P 里记录的旧物理地址
 * @param new_g   本次写入的新 gblock（仅用于日志对照）
 *
 * @note 状态一眼可判：BAD=坏块（预期降级）；FREE=已归还空闲池（会被复用，**丢数据**）；
 *       LOG=元数据日志块（分类判据过宽）；NONE=重建从未认领（页 0 判空/未扫描到）。
 */
static void ftl_dangling_check(ftl_ctx_t *ftl, DWORD l, DWORD old_pba, WORD new_g)
{
    const WORD gob = ftl_pba_gblock(old_pba);

    if (ftl->alloc.blocks[gob].valid_count != 0U)
    {
        return;
    }

    const bst_state_e old_st = ftl_block_state(&ftl->alloc.blocks[gob]);

    ftl->dangling_total++;

    if (old_st == FTL_BLOCK_STATE_BAD)
    {
        // 预期降级：旧块已退役。每个旧 gblock 只打一行，其余 LBA 只累计。
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

            LOGW(LOG_TAG_FTL "dangling g=%hu first_lba=%lu st=BAD newg=%hu (retired; later LBAs aggregated)\r\n", gob, l, new_g);
        }

        slot->lbas++;
        return;
    }

    // 非预期：FREE/LOG/NONE 都可能意味着重建/分配器/分类器缺陷。
    const DWORD now_ms = ftl_job_now_ms();

    if ((now_ms == 0U) || (s_dangling_log_ms == 0U) || ((now_ms - s_dangling_log_ms) >= FTL_JOB_LOG_INTERVAL_MS))
    {
        const char *old_state = ftl_block_state_name(old_st);
        const char *new_state = ftl_block_state_name(ftl_block_state(&ftl->alloc.blocks[new_g]));

        (void)LOGE(LOG_TAG_FTL "dangling lba=%lu old=%08lx g=%hu st=%s sb=%hu newg=%hu nst=%s supp=%lu\r\n", l, old_pba, gob, old_state, ftl->alloc.blocks[gob].sb_id, new_g, new_state, s_dangling_suppressed);
        s_dangling_log_ms     = now_ms;
        s_dangling_suppressed = 0U;
    }
    else
    {
        s_dangling_suppressed++;
    }
}

/* ── host I/O 完成的四个分支（ftl_host_io_callback 按 pending->op 与成败分派）── */

/**
 * @brief 读成功：按链序把数据拷给 caller。
 * desc 链序 == LBA 升序（plane-major + 段内 CW 升序，与 LBA 的 [plane][cw] 布局同序）
 * ⇒ 按链序连续拷出即 LBA 序。
 * ⚠ 返回值必须查：desc 链比请求短时尾部不写（ftl_cmd_read_data 只在这一种情况报错），
 * 不查就等于把未初始化缓冲当数据交给 host。失败 ⇒ 本次读的结果不可信，跳过随后的
 * ECC / read-disturb 记账（记账无意义）。
 *
 * @param ftl 实例（读 disturb / RR 趋势记在本实例的 `alloc.blocks[]`）
 * @param cmd 已完成的读命令（desc 链在此处仍有效，调用者随后才 release）
 * @param pending 对应的子操作记录（提供 buffer / n_cw / lba）
 * @param cmd_id 命令 id（仅用于错误日志定位）
 * @return SSD_STATUS_OK       数据已完整拷出
 *         SSD_STATUS_IO_ERROR 数据不足，结果不可信
 */
static ssd_status_e ftl_host_io_read_ok(ftl_ctx_t *ftl, fc_command_t *cmd, pend_req_t *pending, WORD cmd_id)
{
    DWORD data_size = FTL_IU_BYTES * pending->n_cw; /* 子操作字节数 */

    if (ftl_cmd_read_data(cmd, pending->buffer, data_size) != FTL_OK)
    {
        LOGE(LOG_TAG_FTL "read data short lba=%lu cmd=%hu\r\n", pending->lba, cmd_id);
        return SSD_STATUS_IO_ERROR;
    }

    // Read Recovery：逐 plane 记录 ECC 与 read disturb，用于慢死预测。
    // ⚠ 不能只记 `ftl_cmd_pba()` 的首个 desc：一个 multi-plane 子操作在 Policy B 下
    // 覆盖**两个独立块**，第二块的趋势会漏记（RISK-HOST-02）。
    // ⚠ 须在 fc_command_release(cmd) 之前调用——desc 释放后失效。
    ftl_cmd_block_ecc_t blocks[PLANE_NUM];
    const BYTE          nblocks = ftl_cmd_block_ecc_list(cmd, blocks, PLANE_NUM);

    for (BYTE i = 0U; i < nblocks; i++)
    {
        rr_record_ecc(&ftl->alloc.blocks[blocks[i].gblock].rr, blocks[i].ecc_max);

        // read disturb 计数：读成功累加（周期归零由上层维护）；
        // 随下一次 Checkpoint 的磨损快照持久化。
        ftl->alloc.blocks[blocks[i].gblock].read_count++;
    }

    return SSD_STATUS_OK;
}

/**
 * @brief 写成功（非 cache 中间页）：逐 CW 确认 Journal + L2P + 有效计数。
 * ⚠ region 未就绪（正在异步加载）⇒ **停泊**本项：命令脱钩（command_id 置哨兵，本命令
 * 随即 release，id 会被复用）+ 组在飞数**不**递减（host 写组继续 pending），由
 * pend_confirm_service 每轮推进。与读失败的托管同构（命令脱钩、组不结算），区别是推进者
 * 在本文件而非 RR 引擎。
 * 返回 true = 已停泊 ⇒ 调用者**直接 release 并 return**（勿再动组簿记）。
 *
 * @param ftl 实例
 * @param pending 已完成的写子操作记录（其命令由调用点随后 release）
 * @return true  因 region 未就绪已停泊
 *         false 已确认成功，按正常子操作完成继续
 */
static bool ftl_host_io_write_ok(ftl_ctx_t *ftl, pend_req_t *pending)
{
    ftl_page_desc_t d;

    d.lba       = pending->lba;
    d.new_pba   = pending->new_pba;
    d.new_pba1  = pending->new_pba1;
    d.cw_bitmap = pending->cw_bitmap;
    d.seq_page  = (pending->cache_role != PEND_CACHE_ROLE_NONE);

    if (ftl_confirm_write(ftl, &d) != FTL_AGAIN)
    {
        return false;
    }

    // 停泊预算按**进展**延长（RISK-HOST-05 支路 (a)，判据见 ftl_confirm_progress_pending）。
    if (ftl_confirm_progress_pending(ftl) == true)
    {
        pending->park_ms = ftl_job_now_ms();
    }

    // 停泊：命令脱钩、组在飞数不递减；停泊列表与超时归 pending 模块
    // （docs §5「Confirm → pending 服务」）。
    const WORD pending_idx = (pending - ftl->pending.slots);

    pend_confirm_park(&ftl->pending, pending_idx, pending->owners);
    return true;
}

/**
 * @brief 读失败 ⇒ **托管**给 Read Recovery job（见 read_recovery.c）：
 * · pending 项**保留**（不 pend_remove）、组在飞数**不**递减 ⇒ host 读组继续
 * pending，等 RR 收尾时统一结算；
 * · command_id 置哨兵：本命令随即 release，其 id 会被后续 fc_command_alloc 复用，
 * 留旧值会让新命令的完成误撞上本项；
 * · 收尾（含一律报 IO_ERROR 的语义）在 rr_job_finish 里，见该处注释。
 * ⚠ 恢复**必须**异步：本函数跑在完成泵里，当场同步跑三级恢复就是泵里再开泵（嵌套泵），
 * 且整条 host 读流水线被一个坏页钉住。
 *
 * @param ftl 实例（RR 触发入口在本实例的 `read_recovery_handle`）
 * @param pending 失败的读子操作记录（原地改成 READ_RECOVERY 托管态）
 */
static void ftl_host_io_read_fail(ftl_ctx_t *ftl, pend_req_t *pending)
{
    pending->op         = PENDING_OP_READ_RECOVERY;
    pending->command_id = FTL_PENDING_CMD_NONE;

    // typed 触发：把 owner 交给 RR handle；RR busy 时该项仍留在 pending 表，
    // 当前 job 收尾后由 pending 端口的 take 领走（行为与旧扫描一致）。
    const WORD pending_idx = (pending - ftl->pending.slots);
    rr_owner_t owner;

    owner.owners      = pending->owners;
    owner.pending_idx = pending_idx;
    owner.upper_buf   = (BYTE *)pending->buffer;
    owner.lba         = pending->lba;
    owner.n_cw        = pending->n_cw;

    rr_trigger(&ftl->read_recovery_handle, &owner);
}

/**
 * @brief 写失败：行业坏块替换流程
 *
 * ① bbt_mark_bad（RAM only，后台 bbt_handle 持久化）——先标坏，让 ② 关闭 SB 时把该
 * plane 从 Closed 条目剔除。
 * ② 关闭当前 Open Super Block → 进 closed_sb_list；好 plane 随条目由 GC 回收。
 * Policy B 下两 plane 是独立块，坏 plane 不再被 GC 读/擦。
 * ③ L2P 不动 → 自动回滚到旧映射，旧数据完好。
 *
 * 失败 plane 定位：遍历该命令的 desc 链，逐 CW 取 status；失败 CW 的 desc->paa
 * 给出 plane ⇒ 两个 plane 各自的块都能当场标坏（不再依赖 GC 擦除时才发现 plane1）。
 *
 * @param ftl 实例
 * @param pending 失败的写子操作记录（提供本次分配的 new_pba / new_pba1）
 * @param completion 命令完成状态（提供 command_id，用于遍历 desc 链取逐 CW 状态）
 */
static void ftl_host_io_write_fail(ftl_ctx_t *ftl, pend_req_t *pending, const fc_command_status_t *completion)
{
    // 失败 plane 掩码：逐 desc 取 CW 状态 + desc->paa 的 plane。
    // FC 把 NAND per-CW FAIL 并进 desc（program 与 read 都并），plane1 的失败不再
    // 等到 GC 擦除才暴露（RISK-MIG-07）。
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
        // 没有逐 CW 失败位（如无 desc 的命令）：按 plane0 表项标坏。
        // 带 desc 的 program 现在由 FC 把 NAND per-CW FAIL 并进 desc ⇒ 正常走上面的掩码。
        bad_plane_mask = (1U << pba_to_plane(pending->new_pba));
    }

    char stbuf[48];

    (void)fc_cmd_status_str(completion->status, stbuf, sizeof(stbuf));
    LOGW(LOG_TAG_FTL "program-fail lba=%lu pl=0x%lx st=%s\r\n", pending->lba, bad_plane_mask, stbuf);

    if (((bad_plane_mask & 0x1U) != 0U) && (pending->new_pba != NAND_PBA_INVALID))
    {
        alc_trace_record(&ftl->alloc, pending->lba, pending->new_pba, ALC_TRACE_PROGRAM_FAIL);
        alc_trace_dump(&ftl->alloc, pending->lba, pending->new_pba);
        (void)bbt_mark_bad(&ftl->alloc, ftl_pba_gblock(pending->new_pba));
    }
    if (((bad_plane_mask & 0x2U) != 0U) && (pending->new_pba1 != NAND_PBA_INVALID))
    {
        alc_trace_record(&ftl->alloc, pending->lba, pending->new_pba1, ALC_TRACE_PROGRAM_FAIL);
        alc_trace_dump(&ftl->alloc, pending->lba, pending->new_pba1);
        (void)bbt_mark_bad(&ftl->alloc, ftl_pba_gblock(pending->new_pba1));
    }

    (void)alc_super_close(&ftl->alloc, FTL_TEMP_HOT);
}

// 透明重试日志节流：第 1 次立即打印，此后每 N 次一条（program-fail 风暴不刷屏）。
#define FTL_WRITE_RETRY_LOG_EVERY (64U)
static DWORD s_write_retry_logged;

/**
 * @brief 写失败后把同一页重发到新块（透明重试，主流 program-fail 恢复）
 *
 * 数据来源：旧命令的 desc 链在 `fc_command_release` 之前仍持有本页数据
 * （`ftl_cmd_read_data` 的既有契约，见 ftl_fc_interface.h）。因此不需要常驻
 * 页缓冲；只在失败时拷一次。
 *
 * 顺序：读回数据 → 分配新页对 → 建新命令（**链序**填数）→ 提交 →
 * 成功后才更新 pending 的 command_id/new_pba（提交失败时保持旧 id，让调用者
 * 仍能按本次 completion 的 cmd_id 收尾）。
 *
 * 其他 = 无法重试，调用者按原 IO_ERROR 收尾。
 *
 * @param ftl 实例（数据拷出到 `write_retry_buf`、新页对从本实例 allocator 分配）
 * @param pending 当前写子操作记录（提交成功后原地更新 command_id / new_pba / retry_count）
 * @param old_cmd 失败的旧命令（desc 链仍持有本页数据，调用者随后才 release）
 * @return FTL_OK    已提交（同一 pending 继续在飞，组在飞数**不**递减）
 *         FTL_AGAIN 瞬态资源不足（Free/OP 耗尽、命令槽满、提交背压），本次未提交
 *         FTL_ERROR 读回源数据失败或建链失败，无法重试
 */
static ftl_status_e ftl_write_retry(ftl_ctx_t *ftl, pend_req_t *pending, fc_command_t *old_cmd)
{
    const DWORD len = (DWORD)pending->n_cw * FTL_IU_BYTES;

    if (ftl_cmd_read_data(old_cmd, ftl->write_retry_buf, len) != FTL_OK)
    {
        if ((s_write_retry_logged++ % FTL_WRITE_RETRY_LOG_EVERY) == 0U)
        {
            LOGE(LOG_TAG_FTL "retry: page readback failed lba=%lu count=%lu\r\n", pending->lba, s_write_retry_logged);
        }
        return FTL_ERROR;
    }

    DWORD pba[PLANE_NUM];

    if (alc_super_open_alloc_pair(&ftl->alloc, ALC_SUPER_ALLOC_HOT, pba) != FTL_OK)
    {
        return FTL_AGAIN; /* Free/OP 耗尽：无法替换 */
    }

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        if (bit_mask_any(pending->cw_bitmap, ftl_cw_plane_mask(p)) && (pba_to_page(pba[p]) > 0U) && (alc_super_open_get_pba_state(&ftl->alloc, FTL_TEMP_HOT, pba[p]) != ALC_SUPER_PBA_OPEN_PAGE0_DONE))
        {
            alc_super_open_rewind_pair(&ftl->alloc, FTL_TEMP_HOT);
            return FTL_AGAIN;
        }
    }

    fc_command_t *cmd = fc_command_alloc();

    if (cmd == NULL)
    {
        alc_super_open_rewind_pair(&ftl->alloc, FTL_TEMP_HOT);
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
        alc_super_open_rewind_pair(&ftl->alloc, FTL_TEMP_HOT);
        return st;
    }

    if (ftl_op_submit(cmd, FTL_OP_HOST_IO, NULL) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        alc_super_open_rewind_pair(&ftl->alloc, FTL_TEMP_HOT);
        return FTL_AGAIN;
    }

    // 提交成功后才改 pending：失败路径仍按旧 command_id 收尾（见函数头）。
    pending->command_id = ftl_cmd_id(cmd);
    pending->new_pba    = pba[0];
    pending->new_pba1   = pba[1];
    pending->retry_count++;

    if ((s_write_retry_logged++ % FTL_WRITE_RETRY_LOG_EVERY) == 0U)
    {
        LOGW(LOG_TAG_FTL "write retry lba=%lu cw=0x%02hhx retry=%hhu pba=0x%08lx/0x%08lx count=%lu\r\n", pending->lba, pending->cw_bitmap, pending->retry_count, pba[0], pba[1], s_write_retry_logged);
    }

    return FTL_OK;
}

/* ── host cache program 序列（PENDING_OP_WRITE_CACHE）──
 *
 * 缓存程序是流水线：第 i 页的最终结果由**第 i+1 条命令**的 FAIL_CACHE 给出，
 * 最后一页由 END（10h）的 FAIL 给出。所以已提交页不能在完成时立刻确认映射，
 * 必须按提交顺序进未确认 FIFO（ftl->cache_seq.wr），等后继命令 / END 结算。
 *
 * 任一错误位（FAILC / NAND_FAIL / TIMEOUT / ...）⇒ 序列中止：未确认页一律不确认
 * 映射（按 IO_ERROR 结算）、关闭 hot SB、清序列状态。与旧的整批失败语义一致。
 */

/* ── host cache program 序列（PENDING_OP_WRITE_CACHE）──
 *
 * 早回执（RISK-CACHE-27）：页在**自身命令完成**时就算「已编程」——
 *   ① 立刻结算它的子操作（组完成 ⇒ host 看到完成），不再等下一命令的 FAILC；
 *   ② 页面进入「已编程页表」（`cache_seq.wr`），等后继 FAILC 证明；
 *   ③ 证明之前读该 LBA 走页表覆盖（指到刚编程的 PBA，见 ftl_cache_wr_lookup）；
 *   ④ 证明失败 ⇒ 撤销映射 + 标记 region degraded + 粘性错误（RISK-CACHE-28）。
 *
 * 确认（Journal→L2P→BST）与组完成解耦：由 ftl_cache_wr_confirm_step() 每轮按序推进。
 */

/**
 * @brief 把一页登记进「已编程页表」，并把结论游标指向它。
 *
 * @param ftl 实例。
 * @param pg  页描述（pba / bitmap / 是否已由自身 STATUS 证明）。
 *
 * @return true = 入表；false = 环满（结构性错误，调用者断言）。
 */
static bool ftl_cache_wr_ring_push(ftl_ctx_t *ftl, const ftl_cache_page_t *pg)
{
    ftl_cache_wr_t *wr = &ftl->cache_seq.wr;

    if (wr->count >= FTL_CACHE_SEQ_RING)
    {
        return false;
    }

    const BYTE idx = (BYTE)((wr->head + wr->count) % FTL_CACHE_SEQ_RING);

    wr->page[idx] = *pg;
    wr->count++;

    // 本页成为「下一条命令的结论落点」；已证明的页（END 由自身 STATUS 证明）没有
    // 后继结论，清游标——否则下一条命令的 FAIL_CACHE（可能是上一序列的残留位）
    // 会打到它身上。
    wr->pend_idx   = idx;
    wr->pend_valid = (pg->proven == false);
    return true;
}

/**
 * @brief 取走「下一条命令结论的落点」页，并清游标。
 *
 * @param ftl 实例。
 *
 * @return 待证页；NULL = 本命令没有上一页（新序列首条 / 上一页是 END 页）。
 */
static ftl_cache_page_t *ftl_cache_wr_take_verdict(ftl_ctx_t *ftl)
{
    ftl_cache_wr_t *wr = &ftl->cache_seq.wr;

    if (wr->pend_valid == false)
    {
        return NULL;
    }

    wr->pend_valid = false;
    return &wr->page[wr->pend_idx];
}

/**
 * @brief 弹出队头（已证明且已确认 / 已撤销）。
 *
 * @param ftl 实例。
 *
 * @note 只动 head/count，不搬元素——页表里的绝对下标（含结论游标）因此不会因
 *       弹出而漂移。
 */
static void ftl_cache_wr_ring_pop(ftl_ctx_t *ftl)
{
    ftl_cache_wr_t *wr = &ftl->cache_seq.wr;

    if (wr->count == 0U)
    {
        return;
    }

    wr->head = (BYTE)((wr->head + 1U) % FTL_CACHE_SEQ_RING);
    wr->count--;
}

/**
 * @brief 页表里是否还有「等不到结论」的页（未证明且未判失败）。
 *
 * @param ftl 实例。
 *
 * @return true = 存在未证明页（序列释放时必须 abort）。
 */
static bool ftl_cache_wr_has_unproven(const ftl_ctx_t *ftl)
{
    const ftl_cache_wr_t *wr = &ftl->cache_seq.wr;

    for (BYTE k = 0U; k < wr->count; k++)
    {
        const ftl_cache_page_t *pg = &wr->page[(BYTE)((wr->head + k) % FTL_CACHE_SEQ_RING)];

        if ((pg->proven == false) && (pg->state != FTL_CACHE_PAGE_FAILED))
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 在已编程页表里查**覆盖该 LBA 的最新**一页。
 *
 * @param ftl 实例。
 * @param lba 全局 host LBA。
 *
 * @return 命中的页；NULL = 页表里没有覆盖该 LBA 的页。
 *
 * @note 读覆盖层与失败拦截共用这一条扫描规则（C2）：从**最新**往回查，第一个覆盖
 *       该 LBA 的环项就定论 —— 后写必须盖住先写，绝不回退到更早的项（否则会把
 *       旧数据复活）。
 */
static const ftl_cache_page_t *ftl_cache_wr_find(const ftl_ctx_t *ftl, DWORD lba)
{
    const ftl_cache_wr_t *wr   = &ftl->cache_seq.wr;
    const DWORD           flat = ftl_cw_bit(ftl_lba_plane(lba), ftl_lba_cw(lba));

    // 同一条扫描规则（读到它的两处：读覆盖层 / 失败拦截）：从**最新**往回查，
    // 第一个覆盖该 LBA 的环项就定论（后写必须盖住先写，绝不回退到更早的项）。
    for (BYTE k = wr->count; k > 0U; k--)
    {
        const ftl_cache_page_t *pg = &wr->page[(BYTE)((wr->head + k - 1U) % FTL_CACHE_SEQ_RING)];

        if ((lba < pg->lba) || (lba >= (pg->lba + FTL_LBA_PER_PAIR)))
        {
            continue;
        }
        if (bit_mask_any(pg->cw_bitmap, flat) == false)
        {
            continue;
        }

        return pg;
    }

    return NULL;
}

/**
 * @brief 读覆盖层：该 LBA 是否有「已编程、尚未进 L2P」的页。
 *
 * @param ftl     实例。
 * @param lba     全局 host LBA。
 * @param out_pba 命中时写入该 LBA 所在 plane 的**页基址 PBA**（调用者再按 cw 取）。
 *
 * @return true = 命中（读必须走 out_pba，不能读 L2P）。
 *
 * @note PROGRAMMED 用它；CONFIRMED（L2P 才是真值）或 FAILED（映射已作废）都返回
 *       false —— 都不回退到更早的环项。
 */
bool ftl_cache_wr_lookup(const ftl_ctx_t *ftl, DWORD lba, DWORD *out_pba)
{
    const ftl_cache_page_t *pg = ftl_cache_wr_find(ftl, lba);

    if ((pg == NULL) || (pg->state != FTL_CACHE_PAGE_PROGRAMMED))
    {
        return false; /* CONFIRMED：L2P 真值；FAILED：映射已作废 —— 都不回退 */
    }

    const BYTE  plane    = ftl_lba_plane(lba);
    const DWORD page_pba = (plane == 0U) ? pg->pba0 : pg->pba1;

    *out_pba = pba_with_cw(page_pba, ftl_lba_cw(lba));
    return true;
}

/**
 * @brief 该 LBA 是否落在**已判失败、尚未撤销**的 cache 页上。
 *
 * @param ftl 实例。
 * @param lba 全局 host LBA。
 *
 * @return true = 读该 LBA 必须报 IO_ERROR（不能读 L2P 里的可疑 PBA）。
 *
 * @note 用 ftl_cache_wr_find 的同一条扫描规则（从新到旧、第一个覆盖项定论）。
 *       失败页在撤销完成前仍留在环里，而它的 L2P 映射可能还指向编程失败的页
 *       ⇒ 这一层必须在 l2p_read 之前拦下（RISK-CACHE-28）。
 */
bool ftl_cache_wr_failed_covers(const ftl_ctx_t *ftl, DWORD lba)
{
    const ftl_cache_page_t *pg = ftl_cache_wr_find(ftl, lba);

    return (pg != NULL) && (pg->state == FTL_CACHE_PAGE_FAILED);
}

/**
 * @brief 该页所属的 hot SB 若仍打开则关闭它。
 *
 * @param ftl  实例。
 * @param pba0 plane0 页 PBA。
 * @param pba1 plane1 页 PBA。
 *
 * @note 先查再关：失败页可能属于一个早已关闭/换掉的块，无条件 close 会误关无关块。
 */
static void ftl_cache_wr_close_sb_if_open(ftl_ctx_t *ftl, DWORD pba0, DWORD pba1)
{
    const bool in_sb = ((alc_super_open_get_pba_state(&ftl->alloc, FTL_TEMP_HOT, pba0) != ALC_SUPER_PBA_NOT_OPEN) ||
                        (alc_super_open_get_pba_state(&ftl->alloc, FTL_TEMP_HOT, pba1) != ALC_SUPER_PBA_NOT_OPEN));

    if (in_sb == true)
    {
        (void)alc_super_close(&ftl->alloc, FTL_TEMP_HOT);
    }
}

/**
 * @brief 把一页标记为失败：关 hot SB、region 立即降级、置粘性 deferred error。
 *
 * @param ftl 实例。
 * @param pg  失败页。
 * @param why 失败来源（日志用：FAIL_CACHE / abort / pad …）。
 *
 * @note 撤销（L2P 作废）交给 confirm_step：它可能要等 Journal 容量，不能在这里同步做。
 *       region 降级**立即**置位，让撤销完成前的读也报 IO_ERROR（RISK-CACHE-28）。
 *       `cache_wb_aborts` 按**命令级中止**计一次（member_done 的错误分支），这里不计，
 *       否则一条 FAILC 会同时经「结论落点失败」与「本命令中止」记两笔。
 */
static void ftl_cache_wr_mark_failed(ftl_ctx_t *ftl, ftl_cache_page_t *pg, const char *why)
{
    DWORD suppressed = 0U;

    pg->state  = FTL_CACHE_PAGE_FAILED;
    pg->proven = false;

    ftl_cache_wr_close_sb_if_open(ftl, pg->pba0, pg->pba1);
    ftl->deferred_error = true;

    // region 降级**立即**置位（不等撤销）：FAILC 之后该页的 L2P 映射还在，读会指到
    // 编程失败的页；降级位让「撤销完成前」的读也报 IO_ERROR，而不是返回可疑页内容。
    l2p_note_region_lost(ftl->cache, lba_to_global_region(pg->lba));

    if (ftl_log_throttle_due(&s_cache_wb_fail_log, &suppressed))
    {
        LOGE(LOG_TAG_FTL "cache-wr failed lba=%lu (%s) — close hot SB (suppressed=%lu)\r\n", pg->lba, why, suppressed);
    }
}

/**
 * @brief 撤销一页失败写：把该页覆盖的 LBA 从 L2P 里作废（Journal 先行，与 TRIM 同源）。
 *
 * @param ftl 实例。
 * @param pg  已判失败的页。
 *
 * @return FTL_OK    已作废
 *         FTL_AGAIN region 加载 / Journal 容量未就绪（下轮重试）
 *         FTL_ERROR L2P 读失败（LBA 越界 ⇒ 编程错误，按丢弃处理）
 *
 * @note region 的 degraded 标记在判失败时就置好了（mark_failed），这里只作废映射。
 */
static ftl_status_e ftl_cache_wr_undo_page(ftl_ctx_t *ftl, const ftl_cache_page_t *pg)
{
    for (BYTE bit = 0U; bit < FTL_LBA_PER_PAIR; bit++)
    {
        const DWORD l = (pg->lba + bit);

        // flat 位图：bit = plane*CW_NUM + cw（页对覆盖 2 plane × 4 CW）。走 helper 而不是
        // 手写 `1U << bit`——同一个坐标在页表里只有这一套（C9/H15）。
        if (bit_mask_any(pg->cw_bitmap, ftl_cw_bit(ftl_lba_plane(l), ftl_lba_cw(l))) == false)
        {
            continue;
        }

        if (l2p_is_ready(ftl->cache, l) == false)
        {
            return FTL_AGAIN; /* 已起异步加载：下轮再来 */
        }
        if (jrn_reserve(ftl->journal, 1U) == false)
        {
            return FTL_AGAIN; /* 已起异步刷页 */
        }

        DWORD old = NAND_PBA_INVALID;

        const ftl_status_e rst = l2p_read(ftl->cache, l, &old);

        if (rst == FTL_AGAIN)
        {
            return FTL_AGAIN;
        }
        if (rst != FTL_OK)
        {
            return FTL_ERROR;
        }
        if (old == NAND_PBA_INVALID)
        {
            continue; /* 本来就未映射 */
        }

        (void)jrn_append(ftl->journal, l, old, NAND_PBA_INVALID);

        const ftl_status_e wst = l2p_write(ftl->cache, l, NAND_PBA_INVALID);

        ASSERT(wst == FTL_OK);
        (void)wst;

        // 解除映射也要过守卫：count 已为 0 时运行期静默夹住，但 journal 已记账
        // ⇒ 下次开机回放 `pfr_valid_sub` 判下溢、整份块元数据快照作废（RISK-MD-06）。
        ftl_dangling_check(ftl, l, old, FTL_GBLOCK_INVALID);
        ftl_block_sub_valid_if_live(&ftl->alloc.blocks[ftl_pba_gblock(old)]);
    }

    return FTL_OK;
}

/**
 * @brief 推进「已编程页表」：按序确认 / 撤销 / 弹出已证明的页。
 *
 * @param ftl 实例。
 *
 * @note 每轮 ftl_service 调用一次。顺序 = 提交顺序（Journal 章节也因此有序）。
 *       遇到确认/撤销的瞬态背压（region 加载、Journal 容量）就地停下，下轮继续。
 */
static void ftl_cache_wr_confirm_step(ftl_ctx_t *ftl)
{
    ftl_cache_wr_t *wr = &ftl->cache_seq.wr;

    while (wr->count > 0U)
    {
        ftl_cache_page_t *pg = &wr->page[wr->head];

        if (pg->state == FTL_CACHE_PAGE_FAILED)
        {
            const ftl_status_e st = ftl_cache_wr_undo_page(ftl, pg);

            if (st == FTL_AGAIN)
            {
                return;
            }
            if (st != FTL_OK)
            {
                // 硬错误只可能是 L2P 读的 LBA 越界（编程错误）：本页已判失败、region 已
                // 降级，丢弃该项避免永久卡住队头（flush 屏障会一直等它）。
                LOGE(LOG_TAG_FTL "cache-wr undo failed lba=%lu — drop entry\r\n", pg->lba);
            }
            ftl_cache_wr_ring_pop(ftl);
            continue;
        }

        if (pg->state == FTL_CACHE_PAGE_PROGRAMMED)
        {
            ftl_page_desc_t d;

            d.lba       = pg->lba;
            d.new_pba   = pg->pba0;
            d.new_pba1  = pg->pba1;
            d.cw_bitmap = pg->cw_bitmap;
            d.seq_page  = true;

            if (ftl_confirm_write(ftl, &d) != FTL_OK)
            {
                return; /* FTL_AGAIN：背压，下轮再来 */
            }
            pg->state = FTL_CACHE_PAGE_CONFIRMED;
        }

        if (pg->proven == false)
        {
            return; /* 等后继命令的 FAIL 结论；确认已完成，读走 L2P */
        }

        ftl_cache_wr_ring_pop(ftl);
    }
}

/**
 * @brief 中止写序列：未证明页一律按 IO_ERROR 结算、关闭 hot SB、清序列状态。
 *
 * @param ftl 实例。
 *
 * @note 只处理仍在本序列页表里的页：**器件已证明**的页保留（数据有效，交给确认路径），
 *       已判失败的页不重复记账。在飞成员（尚未进页表的命令）由完成入口的序列代号
 *       守卫丢弃（RISK-CACHE-19：失败序列其后页一律不确认）。代号在这里递增，
 *       是本函数与迟到完成的唯一分界。
 */
static void ftl_cache_wr_abort(ftl_ctx_t *ftl)
{
    ftl_cache_wr_t *wr = &ftl->cache_seq.wr;

    for (BYTE k = 0U; k < wr->count; k++)
    {
        ftl_cache_page_t *pg = &wr->page[(BYTE)((wr->head + k) % FTL_CACHE_SEQ_RING)];

        if (pg->proven == true)
        {
            continue; /* 器件已证明该页编程成功：数据有效，交给 confirm_step 正常确认并弹出
                       * （早回执下它可能还没进 L2P，但不是「失败」） */
        }
        if (pg->state == FTL_CACHE_PAGE_FAILED)
        {
            continue; /* 已判失败（如 FAILC 落点）：等 confirm_step 撤销，不重复记账 */
        }

        ftl_cache_wr_mark_failed(ftl, pg, "abort");
    }

    wr->pend_valid                 = false;
    ftl->cache_seq.kind            = FTL_CACHE_SEQ_NONE;
    ftl->cache_seq.gen             = (BYTE)(ftl->cache_seq.gen + 1U); /* 迟到完成 ⇒ 代号不符 */
    ftl->cache_seq.active          = false;
    ftl->cache_seq.break_requested = false;
    ftl->cache_seq.idle_ms         = 0U;
    ftl->cache_seq.run_pairs       = 0U;
    ftl->cache_seq.closing         = false;
    ftl->cache_seq.close_requested = false;
    ftl->cache_seq.own_page0       = false;
}

/**
 * @brief cache program 成员的完成入口（早回执，RISK-CACHE-27）。
 *
 * 命令 i 完成时：
 *  1. 代号不符（序列已中止 / 已被新序列取代）⇒ 迟到完成：只按 IO_ERROR 结算组，不碰页表；
 *  2. 状态带 FAIL_CACHE ⇒ **结论游标页**（= page i-1）判失败；否则该页被证明成功；
 *  3. 状态带 NAND 级错误 **或** FAIL_CACHE ⇒ 序列中止：本页不入表、按 IO_ERROR 结算，
 *     未证明页一律不确认（RISK-CACHE-19），器件已证明的页保留；
 *  4. 否则本页已编程 ⇒ 入表 + **立即结算子操作**（组完成 ⇒ host 早回执）。
 *
 * @note 找不到 pending 的迟到完成由调用者兜底（本函数只处理仍登记在序列里的页）。
 */
static void ftl_cache_wr_member_done(ftl_ctx_t *ftl, pend_req_t *pending, const fc_command_status_t *completion)
{
    const BYTE  idx      = (BYTE)(pending - ftl->pending.slots);
    const DWORD page_lba = pending->lba;
    const DWORD owners   = pending->owners;

    // 在飞成员数：flush 屏障与「开新序列」都等它归零（页在完成时才进页表，
    // 只看 wr.count 会漏掉仍在编程的页）。所有出口都必须走到这里一次。
    ASSERT(ftl->cache_seq.wr_inflight > 0U);
    ftl->cache_seq.wr_inflight--;

    // 测试镜像专属注入（ISS-FTL-16，FC_NAND_TEST_ENABLE 下编入）：在本条 cache program 命令的完成上伪造 FAIL_CACHE。
    // 器件语义 = 第 N 条命令的 70h 状态报**上一页**的结论（op44/op48 映射，op49 首页
    // 不映射）⇒ 注入到某页的完成 = 判它的**前一页**失败。release 下 fi_* 是空实现，
    // 产品固件里它是空实现，这里的拷贝被优化掉（H14：不付运行期开销）。
    fc_command_status_t completion_used = *completion;

    if (fi_cache_fail_take(page_lba))
    {
        completion_used.status |= FC_CMD_STATUS_FAIL_CACHE; /* 常量落在 8 位字段内（H48 ②） */
    }
    completion = &completion_used;

    // 迟到完成：所属写序列已中止 / 已被新序列取代（代号不符）⇒ 本页不确认映射
    // （RISK-CACHE-19：失败序列其后页一律不确认，否则 rebuild 的 page0 捷径会被击穿）。
    if (pending->cache_gen != ftl->cache_seq.gen)
    {
        DWORD suppressed = 0U;

        if (ftl_log_throttle_due(&s_cache_wb_fail_log, &suppressed))
        {
            LOGW(LOG_TAG_FTL "cache-wr late completion lba=%lu gen=%hhu stale (suppressed=%lu) — drop page\r\n", page_lba, pending->cache_gen, suppressed);
        }

        pend_release_index(&ftl->pending, idx);
        pend_group_complete_subop_multi(&ftl->pending, owners, SSD_STATUS_IO_ERROR);
        return;
    }

    const DWORD status     = completion->status;
    const bool  is_end     = (pending->cache_role == PEND_CACHE_ROLE_END);
    const bool  fail_cache = ((status & FC_CMD_STATUS_FAIL_CACHE) != 0U);
    const bool  own_fail   = ((status & (FC_CMD_STATUS_NAND_FAIL | FC_CMD_STATUS_TIMEOUT | FC_CMD_STATUS_DATA_ERROR)) != 0U);

    // 1) 本命令给**上一页**的结论（FAILC 位）；没有上一页（首条命令的残留 FAILC）时忽略。
    ftl_cache_page_t *prev = ftl_cache_wr_take_verdict(ftl);

    if (prev != NULL)
    {
        if (fail_cache == true)
        {
            ftl_cache_wr_mark_failed(ftl, prev, "FAIL_CACHE");
        }
        else
        {
            prev->proven = true;
        }
    }

    // 2) 序列中止：① 本命令自身编程失败；② 本命令的 FAILC 判了前一页失败。
    //    为什么 FAILC 也要中止：器件级 FAIL_CACHE 表示**缓存程序管线**里上一页没编程
    //    成功，后续页还在同一条可疑管线上；RISK-CACHE-19 依赖「任一错误 ⇒ 其后页一律
    //    不确认」（rebuild 的「page0 空白 ⇒ 整块空白」捷径），继续写下去会击穿它。
    //    本页不入表、按 IO_ERROR 结算组；控制器已提交即回执，失败由 deferred error
    //    在下一条命令上报。
    //    `prev == NULL` 说明本命令是第一页（first，前无本序列的页）：器件对 op49 不映射
    //    FAILC（RISK-IF-11），此时该位只能是上一序列的残留 ⇒ 与步骤 1 同口径忽略。
    if ((own_fail == true) || ((fail_cache == true) && (prev != NULL)))
    {
        char  stbuf[48];
        DWORD suppressed = 0U;

        (void)fc_cmd_status_str(status, stbuf, sizeof(stbuf));
        ftl->cache_wb_aborts++;

        if (ftl_log_throttle_due(&s_cache_wb_fail_log, &suppressed))
        {
            LOGE(LOG_TAG_FTL "cache-wr cmd failed lba=%lu st=%s (suppressed=%lu)\r\n", page_lba, stbuf, suppressed);
        }

        ftl_cache_wr_close_sb_if_open(ftl, pending->new_pba, pending->new_pba1);
        ftl->deferred_error = true;

        // 顺序：先中止序列，再结算组。组完成会回调 `on_complete`，回调可以重入
        // `ftl_submit_*`（`pend_group_maybe_complete` 的既有契约）；若结算在前，
        // 回调可能已经开出一条新序列，随后的 abort 会把**新序列**一起中止。
        ftl_cache_wr_abort(ftl); /* 其余页：已回执的撤销、未证明的丢弃 */

        pend_release_index(&ftl->pending, idx);
        pend_group_complete_subop_multi(&ftl->pending, owners, SSD_STATUS_IO_ERROR);
        return;
    }

    // 3) 本页已编程 ⇒ 入表 + **早回执**。
    //    page0 屏障记账（cache 路径自己打开的块）：成功编程即记，与确认解耦。
    if (pba_to_page(pending->new_pba) == 0U)
    {
        alc_super_open_note_page_programmed(&ftl->alloc, FTL_TEMP_HOT, pending->new_pba);
        alc_super_open_note_page_programmed(&ftl->alloc, FTL_TEMP_HOT, pending->new_pba1);
    }

    {
        ftl_cache_page_t pg;

        pg.lba       = page_lba;
        pg.pba0      = pending->new_pba;
        pg.pba1      = pending->new_pba1;
        pg.cw_bitmap = pending->cw_bitmap;
        pg.state     = FTL_CACHE_PAGE_PROGRAMMED;
        pg.proven    = is_end; /* END 的 FAIL 位就是它自己的结论，不会有后继命令 */

        if (ftl_cache_wr_ring_push(ftl, &pg) == false)
        {
            // 环满属结构性错误：环深 16 ≫ 在飞窗口（≤4 页/窗口 + 前窗尾页）。
            ASSERT(false);
        }
    }

    pend_release_index(&ftl->pending, idx);
    pend_group_complete_subop_multi(&ftl->pending, owners, SSD_STATUS_OK);
}

/* ── 写序列的有序收尾（padding END）── */

// padding 页镜像：整页零。页不被任何 L2P 引用（META = UNKNOWN），只用于给
// 已经提前回执的最后一页提供 FAIL_CACHE 结算点。文件级 .bss，启动清零；只读使用。
static BYTE s_cache_pad_page[FTL_LBA_PER_PAIR * FTL_IU_BYTES];

/**
 * @brief 为打开中的写序列提交一条 padding END（48h）。
 *
 * @param ftl 实例
 * @return FTL_OK    已提交（seq->closing = true，门控继续关到完成）
 *         FTL_AGAIN 页对 / 命令资源不足（已登记紧急 GC，下轮重试）
 *         FTL_ERROR 建链失败
 *
 * @note 为什么需要它：设备序列没有"无数据 END"。写序列在 host 命令边界保持打开
 *       （提前回执模型），当下一段 LBA 不连续 / host 空闲 / flush 到来时，必须
 *       用一个**数据页**把最后已提交页的 FAIL_CACHE 结算出来。本仓用一个不映射的
 *       零页承担这个角色（每段最多一次，≈0.4% 额外页写）。
 */
static SSD_CODE_FAST ftl_status_e ftl_cache_pad_submit(ftl_ctx_t *ftl)
{
    ftl_cache_seq_t *seq = &ftl->cache_seq;

    if ((seq->active != true) || (seq->kind != FTL_CACHE_SEQ_WRITE) || (seq->closing == true))
    {
        return FTL_OK;
    }

    DWORD pba[PLANE_NUM];

    if (alc_super_open_alloc_pair(&ftl->alloc, ALC_SUPER_ALLOC_HOT, pba) != FTL_OK)
    {
        mig_gc_request_urgent(ftl->mig_handle);

        // ⚠ 无空闲页 ⇒ 收尾页也提交不了。若继续等（序列门控 + GC 需要 Journal 容量），
        // 会与 GC 互相等待（livelock）：GC 的 jrn_reserve 被序列门控拒绝，而序列又
        // 需要 GC 腾出的页才能收尾。**放弃本序列**换活性：未确认页按 IO_ERROR 结算
        // （host 已提前回执 ⇒ deferred error），门控放开后 GC 可以刷 Journal 页并回收。
        ftl_cache_wr_abort(ftl);
        return FTL_ERROR;
    }

    fc_command_t      *cmd = NULL;
    const ftl_status_e st  = ftl_async_write_pair_cmd(&cmd, pba, s_cache_pad_page, FTL_CACHE_FULL_CW, OP_TWO_PLANE_PROGRAM_CACHE_END, FTL_PAGE_CFG_DATA_ANY());

    if (st != FTL_OK)
    {
        alc_super_open_rewind_pair(&ftl->alloc, FTL_TEMP_HOT);
        return st;
    }

    cmd->config.chain = FC_CHAIN_END;

    // 序列所有者通道：本命令就是该 LUN 序列的收尾，门控必须旁路。
    const bool owner_prev     = seq->owner_submit;
    seq->owner_submit         = true;
    const ftl_status_e sub_st = ftl_op_submit(cmd, FTL_OP_CACHE_PAD, NULL);
    seq->owner_submit         = owner_prev;

    if (sub_st != FTL_OK)
    {
        (void)fc_command_release(cmd);
        alc_super_open_rewind_pair(&ftl->alloc, FTL_TEMP_HOT);
        return sub_st;
    }

    // 记账用**真实**两 plane 地址：Policy B 下它们可能是不同的物理块，不能用
    // pba_with_plane() 从 plane0 推导（否则 plane1 的 page0 屏障会漏记，见回调）。
    seq->pad_pba[0] = pba[0];
    seq->pad_pba[1] = pba[1];

    seq->closing         = true;
    seq->close_requested = false;
    seq->break_requested = false;
    return FTL_OK;
}

void SSD_CODE_FAST ftl_cache_pad_callback(ftl_ctx_t *ftl, const fc_command_status_t *completion, void *ctx)
{
    // ctx 恒 NULL（见 ftl_op.h 契约）；序列状态在 ftl->cache_seq。
    (void)ctx;

    ftl_cache_seq_t *seq = &ftl->cache_seq;

    // 收尾页在飞期间序列被中止（盘满 abort / 失败 abort）⇒ 这条回调属于已死的序列：
    // 不能再动 allocator 的 page0/SB 记账（那会误关新块的 SB）。
    if ((seq->kind != FTL_CACHE_SEQ_WRITE) || (seq->closing == false))
    {
        return;
    }

    // 收尾页自身的结果（NAND_FAIL）不映射任何数据 ⇒ 只取 FAIL_CACHE 判定**上一页**；
    // 超时意味着上一页结论未知，按失败处理（保守）。
    const bool prev_ok = ((completion->status & (FC_CMD_STATUS_FAIL_CACHE | FC_CMD_STATUS_TIMEOUT)) == 0U);

    // padding 页落在新块 page0 时承担 page0 异步写屏障：成功必须记账，否则
    // allocator 的 `alc_super_open_get_room` 永远返回 0（后续窗口全被拒）。
    // 失败（含上一页 FAILC）⇒ 关 hot SB：与 cache 成员失败同口径，绝不继续写可疑块。
    fc_command_t *cmd = fc_command_id_to_command(completion->command_id);

    if ((cmd != NULL) && (fc_cmd_status_is_success(completion->status) == true))
    {
        if (pba_to_page(seq->pad_pba[0]) == 0U)
        {
            // ⚠ 逐 plane 用 submit 时记下的**真实**地址：Policy B 下两 plane 的块可能
            // 不同，用 pba_with_plane(pad_pba[0], 1) 推导会把 plane1 的 page0 屏障
            // 漏记（RISK-CACHE-24 实测：`p0=01` ⇒ wbuf 永久 AGAIN）。
            for (BYTE p = 0U; p < PLANE_NUM; p++)
            {
                alc_super_open_note_page_programmed(&ftl->alloc, FTL_TEMP_HOT, seq->pad_pba[p]);
            }
        }
    }
    else
    {
        (void)alc_super_close(&ftl->alloc, FTL_TEMP_HOT);
    }

    // 收尾页的 FAIL_CACHE 给出**队头页**（= 最后一页数据页）的结论；超时按失败处理。
    {
        ftl_cache_page_t *prev = ftl_cache_wr_take_verdict(ftl);

        if (prev != NULL)
        {
            if (prev_ok == true)
            {
                prev->proven = true;
            }
            else
            {
                ftl_cache_wr_mark_failed(ftl, prev, "pad FAIL_CACHE");
            }
        }
    }

    if (seq->closing == true)
    {
        // 防御：收尾页之后不该再有**等不到结论**的数据页；若还有（不该发生）判失败，
        // 免得它们永久卡住队头。
        // ⚠ **只判未证明页**（与 ftl_cache_wr_abort 同口径）：页表里的页要先确认
        // （Journal→L2P→BST）才弹出，确认背压时收尾页完成的那一刻，表里完全可能有
        // 已编程成功、只是在等确认的页。把它们判失败会撤销有效映射并触发
        // deferred error（现场：`cache-wr failed … (pad: stray page)`）。
        for (BYTE k = 1U; k < seq->wr.count; k++)
        {
            ftl_cache_page_t *pg = &seq->wr.page[(BYTE)((seq->wr.head + k) % FTL_CACHE_SEQ_RING)];

            if ((pg->proven == false) && (pg->state != FTL_CACHE_PAGE_FAILED))
            {
                ftl_cache_wr_mark_failed(ftl, pg, "pad: stray page");
            }
        }

        seq->kind            = FTL_CACHE_SEQ_NONE;
        seq->active          = false;
        seq->closing         = false;
        seq->close_requested = false;
        seq->break_requested = false;
        seq->own_page0       = false;
        seq->draining        = false;
        seq->idle_ms         = 0U;

        ftl->cache_wb_pad_closes++;
    }

    // 本服务函数拥有命令。
    if (cmd != NULL)
    {
        (void)fc_command_release(cmd);
    }
}

/**
 * @brief 序列有序收尾入口（Phase 2.5 每轮调用）。
 *
 * 读序列：直接放弃（不发 LAST）；写序列：提交 padding END（不能 abort——未确认页
 * 已经对 host 提前回执）。资源不足时保持关闭请求，下轮重试。
 *
 * @param ftl 实例
 */
static SSD_CODE_FAST void ftl_cache_seq_close_step(ftl_ctx_t *ftl)
{
    ftl_cache_seq_t *seq = &ftl->cache_seq;

    if ((seq->active != true) || (seq->owner_submit == true) || (seq->closing == true))
    {
        return;
    }
    if ((seq->close_requested == false) && (seq->break_requested == false))
    {
        return;
    }

    if (seq->kind == FTL_CACHE_SEQ_READ)
    {
        ftl_cache_seq_release(ftl);
        return;
    }

    (void)ftl_cache_pad_submit(ftl);
}

// 实现要点：按 {读,写}×{成功,失败} 分派到 4 个 ftl_host_io_* 分支（见各自注释）；
// 其中读失败与写成功（region 未就绪）是**托管/停泊**路径——命令当场
// release 并 return，组在飞数不递减，由 RR 引擎 / pend_confirm_service 收尾。
// 其余路径收尾 pend_remove + 组聚合，最后 release
// （**本服务函数拥有命令**）。
//
// ⚠ 本函数**不得**同步等待（见 ftl_op.h）——它跑在完成泵里，再调
// ftl_sync_submit_wait 就是嵌套泵（泵里再开泵，完成顺序与栈深都失控）。
void ftl_host_io_callback(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx)
{
    (void)ctx; /* host I/O 不登记旁表 ctx */

    // command_id 由 NAND 从本命令回填；公共壳 fc_command_id_to_command 已断言
    // ID 在池内，恒返回非空指针。
    WORD          cmd_id = completion->command_id;
    fc_command_t *cmd    = fc_command_id_to_command(cmd_id);

    // ftl 恒非空（路由表早于逐实例 boot 设好，见 ftl_op.h）；NULL = 编程错误 ⇒
    // ASSERT 而非 if-guard：release 下解引用留现场，比静默吞完成可定位。

    pend_req_t *pending = pend_find(&ftl->pending, cmd_id);

    if (pending != NULL)
    {
        ssd_status_e sub_status = SSD_STATUS_OK;

        // 成败判据 = **全部**错误位（UNC / CRC / MATA_ERROR / NAND_FAIL / TIMEOUT / RAID），
        // 不是单看 NAND_FAIL：ECC 不可纠只置 UNC（fc_data_path.c 的 ECC 分支），
        // 页内 META 不符只置 MATA_ERROR——两者的 NAND_FAIL 都恒清，单看它会把
        // 「内容不可信」当成功。且 UNC 时 FC 跳过后面的解扰/修正 pass，缓冲里
        // 是**加扰垃圾**，搬给 host 就是静默数据损坏。判据见 fc_cmd_status_has_error()。
        //
        // 分流依据是 pending->op（显式操作类型），不是 buffer——写请求的 buffer
        // 是数据源、恒非空，用 buffer 判别会把写全判成读。
        const bool is_read = pend_op_is_read(pending->op);

        if (pend_op_is_write_cache(pending->op))
        {
            // cache program 成员：中间页/末页/失败均由 batch 上下文结算；
            // 不走透明重试（op44 不能回读重发），也不走普通写失败分支（不按 plane 标坏）。
            ftl_cache_wr_member_done(ftl, pending, completion);
            (void)fc_command_release(cmd);
            return;
        }

        if (fc_cmd_status_is_success(completion->status))
        {
            if (is_read == false)
            {
                // page0 异步写屏障：只记**本命令实际覆盖的 plane**。PAGE0 补全写
                // 覆盖两 plane（缺失 plane 写零），两个 page0 都已编程。
                if (pending->op == PENDING_OP_WRITE_PAGE0)
                {
                    alc_super_open_note_page_programmed(&ftl->alloc, FTL_TEMP_HOT, pending->new_pba);
                    alc_super_open_note_page_programmed(&ftl->alloc, FTL_TEMP_HOT, pending->new_pba1);
                }
                else
                {
                    for (BYTE p = 0U; p < PLANE_NUM; p++)
                    {
                        if (bit_mask_any(pending->cw_bitmap, ftl_cw_plane_mask(p)))
                        {
                            const DWORD pba = (p == 0U) ? pending->new_pba : pending->new_pba1;

                            alc_super_open_note_page_programmed(&ftl->alloc, FTL_TEMP_HOT, pba);
                        }
                    }
                }
            }

            if (is_read)
            {
                sub_status = ftl_host_io_read_ok(ftl, cmd, pending, cmd_id);
            }
            else
            {
                // 普通写 / page0 写：完成后确认映射（cache program 成员已在上面分流）。
                if (ftl_host_io_write_ok(ftl, pending))
                {
                    (void)fc_command_release(cmd);
                    return;
                }
            }
        }
        else if (is_read)
        {
            if (pending->op == PENDING_OP_READ_CACHE)
            {
                // cache sequence 仍在同 LUN 队列里：这里启动 RR 会向同 LUN 插命令，
                // 破坏 31h/3Fh 管线。直接报组错误；后续成员照常完成。
                // 失败原因（NAND_FAIL/UNC/CRC/MATA/EMPTY…）只在这条 completion 上有，
                // 限流打出来：定位「缓存读指到了什么页」不靠猜。
                const DWORD now_ms = ftl_job_now_ms();

                if ((now_ms == 0U) || (s_cache_rd_err_log_ms == 0U) || ((now_ms - s_cache_rd_err_log_ms) >= 1000U))
                {
                    s_cache_rd_err_log_ms = now_ms;
                    LOGE(LOG_TAG_FTL "cache read cmd failed lba=%lu cmd=%hu st=0x%02lX pba=%08lX\r\n", pending->lba, cmd_id, (DWORD)completion->status, ftl_cmd_pba(cmd));
                }
                sub_status = SSD_STATUS_IO_ERROR;
            }
            else
            {
                ftl_host_io_read_fail(ftl, pending);
                (void)fc_command_release(cmd);
                return;
            }
        }
        else
        {
            ftl_host_io_write_fail(ftl, pending, completion);

            // 透明重试：同一页在新块上重发一次。成功 ⇒ 组保持 pending（**不**递减
            // 在飞数），由新命令的完成继续推进；失败 ⇒ 走原 IO_ERROR 收尾。
            if ((pending->op != PENDING_OP_WRITE_PAGE0) && (pending->retry_count < FTL_WRITE_RETRY_MAX) && (ftl_write_retry(ftl, pending, cmd) == FTL_OK))
            {
                (void)fc_command_release(cmd);
                return;
            }

            sub_status = SSD_STATUS_IO_ERROR;
        }

        // 子操作完成 → 组聚合：通知字段（complete_flag/status_out/
        // on_complete）只在组全部子操作完成后触发一次。
        // ⚠ 页打包下一页可能由**多个组**拼成 ⇒ 按 owners 位掩码逐组递减。
        // ⚠ owners 必须在 pend_remove **之前**取走：组完成通知会调 on_complete，
        // 回调可能重入 ftl_submit_* 并复用本槽 ⇒ 释放后再读就是读别人的字段。
        // （另外两处结算点 read_recovery.c / pend_confirm_service 同此顺序。）
        DWORD owners = pending->owners;

        pend_remove(&ftl->pending, cmd_id);
        pend_group_complete_subop_multi(&ftl->pending, owners, sub_status);
    }

    (void)fc_command_release(cmd);
}

/**
 * @brief pending 模块的 confirm handler：数据面事务仍在 FTL（Journal→L2P→BST 唯一顺序）。
 *
 * @param ctx 注入的 FTL 实例（pend_confirm_set_handler 绑定的 `ftl_ctx_t *`）
 * @param pending 待确认的写子操作记录
 * @return FTL_OK    已确认
 *         FTL_AGAIN 仍未就绪（由 pending 模块继续停泊）
 */
static ftl_status_e ftl_confirm_handler(void *ctx, pend_req_t *pending)
{
    ftl_page_desc_t d;

    d.lba       = pending->lba;
    d.new_pba   = pending->new_pba;
    d.new_pba1  = pending->new_pba1;
    d.cw_bitmap = pending->cw_bitmap;
    d.seq_page  = (pending->cache_role != PEND_CACHE_ROLE_NONE);

    return ftl_confirm_write((ftl_ctx_t *)ctx, &d);
}

/* ── 模块端口装配（每个 helper 只做一件事：建端口 + 绑定） ── */

/* ── 对外接口 ── */

void ftl_global_init(void)
{
    // ftl.o 的 .bss 随整文件规则落 DTCM（NOLOAD），文件级日志/守卫静态不在
    // `ftl` 结构体内，必须显式复位。`s_dangling_next` 虽然取模后不会越界，
    // 但 `s_dangling[].lbas` 的随机残留会污染 dangling 日志聚合。
    memset(s_dangling, 0, sizeof(s_dangling));
    s_dangling_next       = 0U;
    s_dangling_log_ms     = 0U;
    s_cache_rd_err_log_ms = 0U;
    s_dangling_suppressed = 0U;
    s_flush_log_ms        = 0U;
    s_write_retry_logged  = 0U;
    memset(&s_flush_err_log, 0, sizeof(s_flush_err_log));
    memset(&s_lost_log, 0, sizeof(s_lost_log));
    memset(&s_nospace_log, 0, sizeof(s_nospace_log));
    memset(&s_subop_fail_log, 0, sizeof(s_subop_fail_log));
    memset(&s_cache_wb_fail_log, 0, sizeof(s_cache_wb_fail_log));
}

// 实现要点：本实例只持有指针，storage 数组/挂载块由 HIL 提供，
// 位于 NOLOAD 段 ⇒ 本函数必须显式 memset。
SSD_CODE_FAST ftl_status_e ftl_init(ftl_ctx_t *ftl, ftl_topo_t topo, l2p_cache_t *cache, const ftl_storage_t *storage)
{
    // 前置条件（判据 2）：唯一调用者 main.c 传 &g_ftl[ch][ce][lun]、共享 cache、storage。
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

    // .dtcm_data 是 NOLOAD，启动不清零；描述符本身没有逐字段覆盖所有状态，
    // 必须在绑定前整表清零（与 storage 数组的 memset 同一纪律）。
    memset(ftl, 0, sizeof(*ftl));

    // ├─ PBA 字段视图自检（一次）：位域布局 implementation-defined，无法用 _Static_assert
    // 证明（见 ftl_config.h §三.2）。若本工具链的布局与 PBA_*_SHIFT 不一致，
    // 强转（*(DWORD *)&fields）会静默算错地址 ⇒ 在此硬失败。
    ASSERT(pba_fields_layout_matches());

    ftl->topo = topo;

    // 异步 TRIM job 初态（NOLOAD 段不清零，显式复位）。
    ftl->trim_active    = false;
    ftl->trim_lba       = 0U;
    ftl->trim_end       = 0U;
    ftl->trim_start_lba = 0U;
    ftl->trim_start_ms  = 0U;
    ftl->trim_wait_ms   = 0U;

    // ├─ 绑定外部存储（storage 数组/挂载块由 HIL 按域提供）──
    // ⚠ .sram*_data 是 NOLOAD，启动代码不清零 ⇒ 下面必须逐表 memset。
    ftl->table_refs      = *storage->table_refs;
    ftl->metadata_dir    = storage->metadata_dir;
    ftl->journal         = storage->journal;
    ftl->cp_handle       = storage->cp_handle;
    ftl->mig_handle      = storage->migrate;
    ftl->wbuf            = storage->wbuf;
    ftl->rr_page_scratch = storage->rr_page_scratch;

    // ⚠ 这里**不**调 ftl_op_init()：旁表与实例路由表都是**全局**资源
    // （ftl_op.c 的 file-static），而本函数是**逐实例**调用的 ——
    // 在此清表会抹掉其它实例在飞的 ctx 登记，也会抹掉 main.c 已设好的路由表。
    // 该函数由 main.c 的 ssd_ftl_boot_all 在**所有实例 boot 之前**调用一次。
    // （完成派发不需要在此注册 handler：命令自己带 op_tag，g_op_callbacks[] 是
    // 编译期常量数组，见 ftl_op.c；旁表只回答"这条命令的私有状态在哪"。）

    // ├─ Pending Request 表（per-instance） ──
    // confirm 停泊的推进归 pending 模块；事务 handler 由 FTL 注入一次。
    pend_init(&ftl->pending);
    pend_confirm_set_handler(ftl_confirm_handler);

    // ├─ storage 数组显式清零（启动不碰 .sram*_data；Phase 1.5 挂载块同）──
    // 五张表：alc_init 逐块读 ftl_block_is_bad() ⇒ blocks 必须先清零；
    // free/op/spare/closed 的读者按 count 有界，逐表清零让 NOLOAD 语义确定。
    // metadata_dir：load/init 不覆盖全部字段 ⇒ 先整表清零。
    // journal：init 只重置部分 job 字段 ⇒ 先整表清零。
    // cp_handle：active 在 cp_job_begin 自己 memset 之前就会被读 ⇒ 必须先清零。
    // migrate / wbuf / rr_page_scratch：Phase 1.5 挂载块，与其余数组一致清零。
    // ⚠ 全部 memset 必须早于 md_load。
    memset(ftl->table_refs.blocks, 0, FTL_BLOCK_TOTAL * sizeof(*ftl->table_refs.blocks));
    memset(ftl->table_refs.free_blocks, 0, FTL_BLOCK_TOTAL * sizeof(*ftl->table_refs.free_blocks));
    memset(ftl->table_refs.op_blocks, 0, PLANE_NUM * sizeof(*ftl->table_refs.op_blocks));
    memset(ftl->table_refs.spare_blocks, 0, PLANE_NUM * sizeof(*ftl->table_refs.spare_blocks));
    memset(ftl->table_refs.closed_sb_list, 0, SUPER_BLOCK_LIST_MAX * sizeof(*ftl->table_refs.closed_sb_list));
    memset(ftl->metadata_dir, 0, sizeof(*ftl->metadata_dir));
    memset(ftl->journal, 0, sizeof(*ftl->journal));
    memset(ftl->cp_handle, 0, sizeof(*ftl->cp_handle));
    // Phase 1.5 挂载块：与其余 storage 数组一致显式清零（未来若落 NOLOAD 段，
    // 启动代码不清零）。
    memset(ftl->mig_handle, 0, sizeof(*ftl->mig_handle));
    memset(ftl->wbuf, 0, sizeof(*ftl->wbuf));
    memset(ftl->rr_page_scratch, 0, FTL_READ_USER_SIZE);

    // ├─ 绑定共享 L2P Cache（设备级已初始化一次；metadata 依赖由 owner 解析）──
    ftl->cache = cache;

    // ├─ Allocator（各指针绑到 table_refs） ──
    alc_init(&ftl->alloc, ftl->topo, &ftl->table_refs);
    ftl->alloc.md_dir = ftl->metadata_dir; /* bbt_mark_bad 的借用日志块判定 */

    // ├─ 候选池存储绑定（metadata_dir 自持 md_pools_t；allocator 只持有指针）──
    // 必须在 bbt_init 之前：BBT 自举扫描/出厂扫描后要按 BAD 集建正式候选池。
    ftl->metadata_dir->topo  = ftl->topo;
    ftl->metadata_dir->alloc = &ftl->alloc;
    ftl->alloc.pools         = &ftl->metadata_dir->pools;

    // ├─ BBT：先自举扫描/出厂扫描，再用 BAD 集建元数据候选池 ──
    // 顺序是承重的：Anchor/CP/Journal 的候选池都依赖 BBT 的 BAD 集；
    // 而 BBT 自身靠 record 的 magic/sequence 在「名义块 + shared spare」上自举。
    bbt_init(&ftl->alloc);

    if (md_pool_build_all(&ftl->alloc) == false)
    {
        LOGE("[FTL ] metadata candidate pool build failed — refuse boot\r\n");
        return FTL_ERROR;
    }

    // ├─ Journal ──
    // 持续擦除失败的环形块要标进 BBT（RISK-JN-01）；journal 只持有 allocator 指针，
    // 通过 allocator/bbt 的 owner API 查询与标坏，不直接改其内部字段。
    jrn_init(ftl->journal, ftl->topo, &ftl->alloc);

    // ├─ Metadata Directory（Anchor 候选池已就绪）──
    ftl->total_writes   = 0U; /* 元数据 job 的 total_writes 指针在建目录前就有效 */
    ftl->dangling_total = 0U;

    // 顺序读检测 / cache read 观测（NOLOAD 段，必须显式清零）。
    ftl->seq_last_end_lba     = 0U;
    ftl->seq_run              = 0U;
    ftl->seq_run_max          = 0U;
    ftl->seq_generation       = 0U;
    ftl->seq_prefetch_target  = 0U;
    ftl->seq_prefetch_valid   = false;
    ftl->cache_read_batches   = 0U;
    ftl->cache_read_pairs     = 0U;
    ftl->cache_read_starts    = 0U;
    ftl->cache_read_declines  = 0U;
    ftl->cache_read_fallbacks = 0U;
    ftl->cache_seq_breaks     = 0U;
    ftl->cache_wb_batches     = 0U;
    ftl->cache_wb_pages       = 0U;
    ftl->cache_wb_declines    = 0U;
    ftl->cache_wb_aborts      = 0U;
    ftl->cache_wb_starts      = 0U;
    ftl->cache_wb_page_ends   = 0U;
    ftl->cache_wb_pad_closes  = 0U;
    ftl->stream_rd.last_end   = FTL_LBA_INVALID;
    ftl->stream_rd.run        = 0U;
    ftl->stream_rd.active     = false;
    ftl->stream_wr.last_end   = FTL_LBA_INVALID;
    ftl->stream_wr.run        = 0U;
    ftl->stream_wr.active     = false;
    {
        const ftl_status_e md_ret = md_load(ftl->metadata_dir, &ftl->alloc, &ftl->total_writes);

        // 失败时仍建空 Directory（空片首启 / 格式化路径需要），但把失败状态留给
        // pfr_boot_recovery：介质若已有有效 Checkpoint，空 Directory = 映射整体丢失
        // ⇒ 必须拒绝启动（RISK-BOOT-09）。
        ftl->md_load_failed = (md_ret != FTL_OK) ? 1U : 0U;

        if (md_ret != FTL_OK)
        {
            md_init(ftl->metadata_dir, &ftl->alloc, &ftl->total_writes);
        }
    }

    // ├─ 搬移引擎（GC / WL / 慢死预测）：窄上下文 + 自持节拍 ──
    mig_init(ftl->mig_handle, &ftl->alloc, ftl->journal, ftl->cache, &ftl->total_writes);

    // ├─ Read Recovery：窄上下文（L2P / allocator / 整页暂存 / pending 表）──
    rr_init(&ftl->read_recovery_handle, ftl->cache, &ftl->alloc, ftl->rr_page_scratch, &ftl->pending);

    // ├─ Wbuf：窄上下文（allocator / pending 表 / migrate handle）──
    wb_init(ftl->wbuf, &ftl->alloc, &ftl->pending, ftl->mig_handle);

    // ├─ Checkpoint：窄上下文（l2p / journal / metadata / allocator / topo）──
    // cp_init 内部整体 memset ⇒ 池游标 cursor_valid==0，首次 job 先扫池定位。
    cp_init(ftl->cp_handle, ftl->cache, ftl->journal, ftl->metadata_dir, &ftl->alloc);

    // ├─ 统计 ──
    // ├─ 后台调度器任务表（10 项；must-run + interval/budget）──
    // 必须在所有在飞的字段就绪后装载（适配器只依赖实例字段与 kick，无额外依赖）。
    bg_scheduler_init(&ftl->bg);

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
 *   · 审计通过后，下游对同一绑定**不逐次复检**——这些绑定在生命周期内不可变。
 */
#if !defined(NDEBUG)

/**
 * @brief 每个 ASSERT 按模块聚合：一条断言覆盖同一生命周期的一组字段。
 * 失败时 file:line 定位到模块组；需要逐字段定位时临时拆分。
 *
 * @param ftl 已初始化的实例（只读）
 */
static SSD_CODE_FAST void contract_check_handles(const ftl_ctx_t *ftl)
{
    ASSERT((ftl->cache != NULL) && (ftl->metadata_dir != NULL) && (ftl->journal != NULL) && (ftl->cp_handle != NULL) && (ftl->mig_handle != NULL) && (ftl->wbuf != NULL) && (ftl->rr_page_scratch != NULL));

    ASSERT((ftl->table_refs.blocks != NULL) && (ftl->table_refs.free_blocks != NULL) && (ftl->table_refs.op_blocks != NULL) && (ftl->table_refs.spare_blocks != NULL) && (ftl->table_refs.closed_sb_list != NULL));

    ASSERT((ftl->alloc.blocks == ftl->table_refs.blocks) && (ftl->alloc.free_blocks == ftl->table_refs.free_blocks) && (ftl->alloc.op_blocks == ftl->table_refs.op_blocks) && (ftl->alloc.spare_blocks == ftl->table_refs.spare_blocks) && (ftl->alloc.closed_sb_list == ftl->table_refs.closed_sb_list));
}

/**
 * @brief 校验 topology 副本与实例路由一致（各模块 init 时都拷了一份）。
 *
 * @param ftl 已初始化的实例（只读）
 */
static SSD_CODE_FAST void contract_check_topology(const ftl_ctx_t *ftl)
{
    const ftl_topo_t *topo = &ftl->topo;
    const DWORD       dim  = ftl_topo_dim(*topo);

    ASSERT((topo->ch < CH_NUM) && (topo->ce < CE_NUM) && (topo->lun < LUN_NUM) && (dim < FTL_DIM_TOTAL) && (ftl_dim_ch(dim) == topo->ch) && (ftl_dim_ce(dim) == topo->ce) && (ftl_dim_lun(dim) == topo->lun));

    ASSERT((ftl->alloc.ch == topo->ch) && (ftl->alloc.ce == topo->ce) && (ftl->alloc.lun == topo->lun) && (ftl->cache->ch == topo->ch) && (ftl->cache->ce == topo->ce));

    // 各模块在 init 时拷贝 topology；副本必须与实例路由一致。
    ASSERT((ftl->metadata_dir->topo.ch == topo->ch) && (ftl->metadata_dir->topo.ce == topo->ce) && (ftl->metadata_dir->topo.lun == topo->lun) && (ftl->journal->topo.ch == topo->ch) && (ftl->journal->topo.ce == topo->ce) && (ftl->journal->topo.lun == topo->lun));

    ASSERT((ftl->cp_handle->topo.ch == topo->ch) && (ftl->cp_handle->topo.ce == topo->ce) && (ftl->cp_handle->topo.lun == topo->lun) && (ftl->mig_handle->topo.ch == topo->ch) && (ftl->mig_handle->topo.ce == topo->ce) && (ftl->mig_handle->topo.lun == topo->lun) && (ftl->read_recovery_handle.topo.ch == topo->ch) && (ftl->read_recovery_handle.topo.ce == topo->ce) && (ftl->read_recovery_handle.topo.lun == topo->lun));
}

/**
 * @brief 校验各模块窄上下文绑定的指针与本实例账本一致。
 *
 * @param ftl 已初始化的实例（只读）
 */
static SSD_CODE_FAST void contract_check_module_contexts(const ftl_ctx_t *ftl)
{
    const md_dir_cache_t *md = ftl->metadata_dir;

    ASSERT((md->alloc == &ftl->alloc) && (md->total_writes == &ftl->total_writes));

    ASSERT(ftl->journal->alloc == &ftl->alloc);

    const cp_handle_t *cp = ftl->cp_handle;

    ASSERT((cp->cache == ftl->cache) && (cp->journal == ftl->journal) && (cp->metadata_dir == ftl->metadata_dir) && (cp->alloc == &ftl->alloc));

    const mig_handle_t *mg = ftl->mig_handle;

    ASSERT((mg->alloc == &ftl->alloc) && (mg->journal == ftl->journal) && (mg->cache == ftl->cache) && (mg->total_writes == &ftl->total_writes));

    const rr_handle_t *rr = &ftl->read_recovery_handle;

    ASSERT((rr->cache == ftl->cache) && (rr->alloc == &ftl->alloc) && (rr->page_scratch == ftl->rr_page_scratch) && (rr->pending == &ftl->pending));

    const wb_handle_t *wb = ftl->wbuf;

    ASSERT((wb->alloc == &ftl->alloc) && (wb->pending == &ftl->pending) && (wb->migrate == ftl->mig_handle));
}

/**
 * @brief 校验本实例的 owner / route 身份（只查本实例拥有的 region）。
 *
 * @param ftl 已初始化的实例（只读）
 */
static SSD_CODE_FAST void contract_check_identity(const ftl_ctx_t *ftl)
{
    const ftl_topo_t topo = ftl->topo;
    const DWORD      dim  = ftl_topo_dim(topo);

    ASSERT(ftl->cache->owner_of_region != NULL);
    ASSERT(ftl_op_route_get(topo.ch, topo.ce, topo.lun) == ftl);

    // 只查本实例拥有的 region：其他 LUN 的 owner 可能尚未 boot。
    for (DWORD local = 0U; local < L2P_REGIONS_PER_INSTANCE; local++)
    {
        const DWORD        global = (local * FTL_DIM_TOTAL) + dim;
        const l2p_owner_t *owner  = ftl->cache->owner_of_region(ftl->cache->owner_ctx, global);
        const QWORD        lba    = (QWORD)global * L2P_ENTRIES_PER_REGION;

        ASSERT((owner != NULL) && (owner->dim == dim) && (owner->metadata_dir == ftl->metadata_dir) && (owner->alloc == &ftl->alloc) && (owner->topo.ch == ftl->cache->ch) && (owner->topo.ce == ftl->cache->ce) && (owner->topo.lun == ftl->topo.lun) && (ftl_topo_dim(owner->topo) == dim));

        ASSERT((ftl_op_route_get_region(global) == ftl) && (lba_route_ch(lba) == ftl->cache->ch) && (lba_route_ce(lba) == ftl->cache->ce));
    }
}

/**
 * @brief 校验后台调度器任务表已装载且处于初始 IDLE 档。
 *
 * @param ftl 已初始化的实例（只读）
 */
static SSD_CODE_FAST void contract_check_bg(const ftl_ctx_t *ftl)
{
    ASSERT((ftl->bg.task_count > 0U) && (ftl->bg.task_count <= BG_TASK_COUNT) && (ftl->bg.cfg == NULL) && (ftl->bg.level == BG_LEVEL_IDLE));

    for (BYTE i = 0U; i < ftl->bg.task_count; i++)
    {
        ASSERT(ftl->bg.tasks[i].func != NULL);
    }
}

/**
 * @brief 校验各异步 job（journal / checkpoint / metadata / migrate / RR / wbuf）初始 idle。
 *
 * @param ftl 已初始化的实例（只读）
 */
static SSD_CODE_FAST void contract_check_jobs_idle(const ftl_ctx_t *ftl)
{
    const jrn_handle_t *j = &ftl->journal->handle;

    ASSERT((j->phase == JRN_IDLE) && (j->current_block < JOURNAL_BLOCK_COUNT) && (j->current_page < PAGES_PER_BLOCK) && (j->current_entry == 0U) && (j->sequence_counter == 1U) && (j->merge_floor == 0U) && (j->need_erase == false) && (j->checkpoint_needed == false) && (j->emergency == false) && (j->write_fail_count == 0U) && (j->write_fail_pending == false) && (j->pre_erased_block == JRN_BLOCK_NONE) && (j->bad_mask == 0U) && (j->erase_fail_count == 0U) && (j->ring_full == false));

    const cp_handle_t *cp = ftl->cp_handle;

    ASSERT((cp->active == false) && (cp->waiting == false) && (cp->failed == false) && (cp->phase == CP_IDLE) && (cp->cp_state.advance_next == false) && (cp->cp_state.cursor_valid == false));

    ASSERT((md_is_busy(ftl->metadata_dir) == false) && (ftl->metadata_dir->count <= METADATA_DIR_ENTRIES) && md_cursor_bounds_ok(ftl->metadata_dir));

    ASSERT((ftl->mig_handle->active == false) && (ftl->mig_handle->waiting == false) && (ftl->mig_handle->phase == MIG_IDLE) && mig_fail_state_is_clean(ftl->mig_handle));

    ASSERT((ftl->read_recovery_handle.stage == RR_STAGE_IDLE) && (ftl->read_recovery_handle.waiting == false) && (ftl->read_recovery_handle.has_owner == false));

    ASSERT((ftl->wbuf->open == false) && (ftl->wbuf->occupied == 0U) && (ftl->wbuf->error == FTL_OK));
}

/**
 * @brief 校验开机零态：pending 空、计数为 0、无在飞 loading / flush。
 *
 * @param ftl 已初始化的实例（只读）
 */
static SSD_CODE_FAST void contract_check_boot_zero(const ftl_ctx_t *ftl)
{
    ASSERT((pend_slot_get_count(&ftl->pending) == 0U) && (pend_group_get_count(&ftl->pending) == 0U) && (ftl->total_writes == 0U) && (ftl->cache->load_inflight == 0U) && (ftl->cache->flush_inflight == 0U) && (ftl->bg_suspend == false) && (ftl->l2p_defer == false));
}

#endif /* !NDEBUG */

SSD_CODE_FAST void ftl_contract_audit(const ftl_ctx_t *ftl)
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

    LOGI("[FTL ] contract ok ch=%hhu ce=%hhu lun=%hhu regions=%u tasks=%hhu\r\n", ftl->topo.ch, ftl->topo.ce, ftl->topo.lun, L2P_REGIONS_PER_INSTANCE, ftl->bg.task_count);
#endif
}

// 实现要点：这两段不依赖任何实例：NAND 引擎（fc_service）是全局的，完成表
// （ftl_op ctx 旁表/路由）也是全局的、按命令的 ch/ce/lun 路由回实例。
// 故 superloop 每轮**只调一次**，而不是每个实例调一次
// （per-instance 调用会把 fc_service 预算重复消耗 N 遍）。
void ftl_service_pump(void)
{
    // Phase 1: fc_service 推进 NAND。
    // 第 1 次调用即把本轮所有完成抽干（nand_dispatch_poll 全遍历 + sink 逐条消费），
    // 之后几次走填充 pass；预算因此封顶的是"本轮跑几次 fc_service"，不是完成条数。
    for (BYTE i = 0U; i < NAND_SUBMIT_BUDGET; i++)
    {
        if (fc_service() != FC_STATUS_OK)
        {
            break;
        }
    }

    // Phase 2: 统一完成泵（按命令 op_tag 派发到服务函数数组，见 ftl_op.c）
    ftl_op_drain();
}

// 实现要点：调用方须**先**调 ftl_service_pump()（全局泵，每轮一次），再对**每个**
// 实例调本函数（多实例下所有实例都要推进，不能只跑 [0][0][0]）。
// Phase 3 收缩为一次 bg_scheduler_service——档位选择 / 任务表 / 预算规模全在
// `ftl/bg_scheduler.c`（含 bg_suspend 判定，见该处头注）。
void ftl_service(ftl_ctx_t *ftl)
{
    // 唯一调用点 main.c 传 &g_ftl[ch][ce][lun]（静态数组元素，结构性恒非空）；
    // NULL 属「不该发生」⇒ ASSERT（release 裁空后 NULL 解引用=HardFault 留现场，
    // 优于静默返回⇒该实例 journal/GC 永不推进的无声损坏）。

    // Phase 2.4：运行期元数据 BAD 后的候选池重算（纯 RAM；等元数据 job 空闲才做）。
    // 必须早于本轮新 I/O：游标/候选表先一致，再允许分配与提交。
    md_pool_service(ftl);

    // Phase 2.5 首：推进**停泊**的写确认。必须早于补发——停泊意味着那个 LBA
    // 的 L2P 还是旧映射，先补发一条读同一 LBA 的子操作就会读到旧物理页（陈旧数据）。
    // 顺序与 Phase 2.5 的既有约定一致：先处理**已完成**的，再下发新的。
    pend_confirm_service(&ftl->pending, ftl, NULL);

    // Phase 2.5 次：推进 cache 写序列的「已编程页表」——按序确认（Journal→L2P→BST）、
    // 撤销失败页、弹出已证明页（RISK-CACHE-27/28）。组早已完成，这里只做元数据记账。
    ftl_cache_wr_confirm_step(ftl);

    // Phase 2.5: 补发 host 子操作。完成先腾出 FC 资源（命令/desc/buffer 池、SQ 空位），
    // 再把各组的待下发子操作补上；顺序保证 host IO 优先于 Phase 3 的后台任务。
    // 只遍历**本实例**的组（每实例一份 groups 表）。
    for (BYTE i = 0U; i < FTL_GROUP_MAX; i++)
    {
        if (ftl->pending.groups[i].used == true)
        {
            ftl_drain_group(ftl, i);
        }
    }

    // 安全网：序列跨 host 段/命令保持打开等同一 LBA 流的下一段；孤儿序列超时关闭。
    ftl_cache_seq_idle_guard(ftl);

    // 有序收尾：写序列用 padding END（给提前回执的最后一页补 FAIL_CACHE 结算），
    // 读序列直接放弃。放在补发之后：本轮的窗口先发出去，再决定是否收尾。
    ftl_cache_seq_close_step(ftl);

    // Phase 2.5 尾：页打包写缓冲的**确定性空闲 flush**——补发循环跑完后，若再无写组会
    // 向当前窗口 append，就把缓冲编程掉（否则这些 host 写永不完成）。放在 bg_suspend
    // 判定之前：它是 host IO 的收尾，不是后台任务。
    {
        wb_trigger_t trigger;
        memset(&trigger, 0, sizeof(trigger));
        trigger.kind = WBUF_TRIGGER_MAYBE_FLUSH_IDLE;
        (void)wb_handle(ftl->wbuf, &trigger, NULL, NULL);
    }

    // Phase 3: 时间预算后台调度 —— 档位选择 / 10 项任务表 / 预算规模 / bg_suspend
    // 判定全在 ftl/bg_scheduler.c（本文件不持有后台策略）。
    bg_scheduler_service(ftl);
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

/**
 * @brief 下发**一个**子操作（pair 窗口内的连续 LBA 段）——查/分配 PBA、建 FC 命令、注册 pending、submit。
 *
 * @param ftl 实例
 * @param gi 组下标
 * @param internal_lba **全局** host LBA（扇区号，数据通路全程用全局 LBA，不预转 local）
 * @param buffer 数据缓冲
 * @param n_cw 入参兼出参（最大可提供 / 实际消费的 LBA 数）。
 *
 * @return FTL_OK    已下发（或如未映射读般**同步完成**，不进 outstanding）
 *         FTL_AGAIN 瞬态背压（命令/desc/buffer 池满、SQ 满、pending 表满）→ 排队重试
 *         FTL_ERROR 永久失败（无可用块 / 映射表损坏）
 *
 * @note 实现要点：⚠ 三态分类是全局枢纽（见上方分区头）。
 * · 读：逐 LBA 查 L2P，按 (物理 plane, 页) 分组——未映射、或同 plane 换了页
 * 即收缩；首个 LBA 就未映射则同步零填 1 个 LBA 并消费它。
 * · 写：按窗口内 LBA 段算 flat 位图（每个 LBA 落自己 plane 位的页 + 自己的
 * CW），一次 alc_super_open_alloc_pair 拿两个 plane 的页。
 */
static ftl_status_e ftl_issue_subop(ftl_ctx_t *ftl, WORD gi, DWORD internal_lba, void *buffer, BYTE *n_cw)
{
    pend_group_t *g = &ftl->pending.groups[gi];

    // 夹到 pair 窗口：一个子操作不跨窗口（窗口内最多两个 plane 段、每段一个页）。
    // ⚠ L2P region 512 对齐且 pair 窗口只有 8 个 LBA ⇒ 窗口绝不跨 region。
    BYTE n;
    {
        const DWORD window_end = ftl_lba_pair_base(internal_lba) + FTL_LBA_PER_PAIR;
        const BYTE  avail      = (window_end - internal_lba);

        n = (*n_cw < avail) ? *n_cw : avail;
    }

    // 写：攒进页打包缓冲（窗口满 / 切换窗口时内部触发编程），本函数对写**到此为止**。
    // 数据在 wb_handle(FLUSH) 里一次编程——分配/建链/提交/确认全在那里。
    if (pend_op_is_read(g->op) == false)
    {
        // 回填窗口夹住后的实际消费数——调用者（ftl_drain_group）据此推进 g->next。
        // ⚠ 不可漏：内部 n 可能被窗口边界夹小（internal_lba 未 8 对齐时 avail<8），
        // 漏回填会让 g->next 越过窗口、跳过未下发的 LBA。
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

    // 命中写缓冲：该 LBA 尚未编程，从缓冲取（否则会读到陈旧 L2P 映射）。同步完成、
    // 消费 1 个 LBA（不进 outstanding）。
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
        // 逐 LBA 查 L2P 并按 (物理 plane, 页) 分组。收缩条件：
        // · 该 LBA 仍在写缓冲里（未编程）—— 本 LBA 不进本子操作（下一轮从缓冲取）
        // · 未映射（INVALID）—— 本 LBA 不进本子操作（下一轮单独处理）
        // · region 尚未就绪（异步加载在飞）—— 本子操作到此为止，下轮再来
        // · 同 plane 但换了页 —— 一条命令每 plane 只能一个页，装不下
        // · 非法 PBA / 物理 plane ≠ LBA 的 plane 位 —— 映射表损坏 → FTL_ERROR
        BYTE k;

        for (k = 0U; k < n; k++)
        {
            const DWORD l = (internal_lba + k);
            DWORD       p = NAND_PBA_INVALID; /* 越界（FTL_ERROR）保持此值 = 未映射 */

            if (wb_has(ftl->wbuf, l))
            {
                break; /* 还在写缓冲（未编程）：收缩到此为止，下一轮同步从缓冲取 */
            }

            // cache 写序列里**已编程、尚未确认**的页：L2P 仍是旧映射，必须读刚编程的
            // 页（RISK-CACHE-27）。命中即用它的 PBA，跳过 L2P。
            if (ftl_cache_wr_lookup(ftl, l, &p) == false)
            {
                // 已判失败、撤销还没走完的页：L2P 可能仍指着编程失败的 PBA
                // ⇒ 必须报错，不能把它的内容当数据返回（RISK-CACHE-28）。
                if (ftl_cache_wr_failed_covers(ftl, l) == true)
                {
                    DWORD suppressed = 0U;

                    if (ftl_log_throttle_due(&s_lost_log, &suppressed))
                    {
                        LOGE(LOG_TAG_FTL "rd lba=%lu: cache page failed (FAIL_CACHE) — IO_ERROR (suppressed=%lu)\r\n", l, suppressed);
                    }
                    return FTL_ERROR;
                }

                // ⚠ 三态：FTL_AGAIN = region 未驻留（已起异步加载）。**必须**与
                // "未映射"（FTL_OK + INVALID）分清——把前者当后者会让 host 读到假数据。
                // FTL_ERROR 仅越界，此时 p 保持 INVALID。
                if (l2p_read(ftl->cache, l, &p) == FTL_AGAIN)
                {
                    ftl->l2p_defer = true; /* 告知 drain：这是加载在飞，不是卡死 */
                    break;                 /* 本子操作到此为止；已凑到的 k 个 LBA 照常下发 */
                }
            }

            if (p == NAND_PBA_INVALID)
            {
                break; /* 未映射：收缩到此为止 */
            }

            // 边界不变量（fc.c 的 fc_poll_fill_queue 靠它）：**进 FC 的 PBA 必须合法**。
            // L2P 条目既不是 NAND_PBA_INVALID 又不是合法 PBA ⇒ 映射表已损坏（介质 bit rot、
            // 半编程页被 ECC「纠」过、跨介质格式复用旧页）。**必须在此拦下**：
            // pba_to_ch/ce/lun 会把越界位当路由字段，放行即让 FC 按它直接索引
            // suspended[][][]（栈越界写）；且坏 PBA 还会经 pba_to_paa 掩位后建出一条
            // 读**别的物理页**的 desc 链——静默读到错误数据。
            // 这是**数据**损坏而非编程错误 ⇒ 报错返回（不 ASSERT），由 drain 把本组标
            // IO_ERROR 交给 host，不 brick 设备。
            if (pba_is_valid(p) == false)
            {
                LOGE(LOG_TAG_FTL "rd lba=%lu: L2P entry 0x%08lx is not a valid PBA\r\n", l, p);
                return FTL_ERROR;
            }

            BYTE  pl = pba_to_plane(p);
            DWORD pg = pba_page(p);

            // CW 级映射的结构不变量：**物理 plane == LBA 的 plane 位**。
            // 违反 ⇒ FC 侧 desc->meta（由**物理** plane/cw 铸出）还原出的扇区 LBA 不是
            // 这一条，读校验必挂 MATA_ERROR；且位图段（按 plane 位算）与 paa 的 plane 位
            // 会分叉 ⇒ 空链。写路径由构造保证（新页从 pair 取），读路径在此拦下。
            // 属映射表损坏（旧介质格式 / bit rot），不是编程错误。
            if (pl != ftl_lba_plane(l))
            {
                LOGE(LOG_TAG_FTL "rd lba=%lu: plane mismatch pba=0x%08lx\r\n", l, p);
                return FTL_ERROR;
            }

            if ((pba[pl] != NAND_PBA_INVALID) && (pba_same_page(pba[pl], p) == false))
            {
                break; /* 同 plane 跨页：本子操作到此为止，余下交给下一轮 */
            }

            const BYTE cw = ftl_lba_cw(l);

            pba[pl] = pg;
            cw_bitmap |= ftl_cw_bit(pl, cw);
        }

        if (k == 0U)
        {
            // 首 LBA 未就绪（region 在加载）⇒ 本轮无进展，返背压等加载完成。
            // ⚠ 必须**先**判这一条：把"还不知道"当成"未映射"会给 host 填一页零。
            if (ftl->l2p_defer == true)
            {
                return FTL_AGAIN;
            }

            // 降级 L2P region 的未映射不是「合法未映射」：该范围的旧映射已经丢失，
            // 必须报 IO_ERROR，不能零填（静默丢数据，RISK-BOOT-18）。
            // 回放已恢复的 LBA 有有效 PBA，不会走到这里。
            if (l2p_is_region_degraded(ftl->cache, lba_to_global_region(internal_lba)))
            {
                DWORD suppressed = 0U;

                if (ftl_log_throttle_due(&s_lost_log, &suppressed))
                {
                    LOGE(LOG_TAG_FTL "rd lba=%lu: mapping lost (degraded L2P region) — IO_ERROR (suppressed=%lu)\r\n", internal_lba, suppressed);
                }
                return FTL_ERROR;
            }

            // 首 LBA 未映射：同步返回全零，消费 1 个 LBA（不进 outstanding）。
            // ⚠ 只覆盖**自己**这一个 LBA：相邻 LBA 的映射与它无关，由 drain 下一轮处理。
            memset(buffer, 0, FTL_IU_BYTES);
            *n_cw = 1U;
            return FTL_OK;
        }
        n = k;
    }

    // 本子操作覆盖的 plane 数（决定 operation 码与 plane 段）。
    // cw_bitmap 非空由 ftl_issue_subop 的拆分子操作保证（至少 1 个 CW）。
    BYTE nplanes = ftl_cw_bitmap_plane_count(cw_bitmap);

    // 资源次序：**先查 pending 表余量，再配 desc 链**。末尾要用一个 pending 槽
    // 登记本子操作；表满却先建好链，只能整条回滚——白配一场。
    // ⚠ 必须在 wbuf 命中分支**之后**：那条路径同步完成、不占 pending 槽，不该被表满挡住。
    if (pend_slot_get_count(&ftl->pending) >= FTL_PENDING_MAX)
    {
        return FTL_AGAIN;
    }

    // 分配 FC 命令槽（池空 = 瞬态）
    fc_command_t *cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return FTL_AGAIN;
    }

    // 配置命令头（multi_plane 决定 plane 段，必须早于 ftl_cmd_setup_ppa_pair_cw——
    // 后者按段校验位图）
    cmd->config.operation   = ftl_plane_read_op(nplanes);
    cmd->config.multi_plane = ftl_plane_multi_field(nplanes);
    cmd->config.cw          = CW_NUM - 1U;
    cmd->config.crc         = FC_CRC_ENABLE;
    cmd->config.random      = FC_RANDOM_ENABLE;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    // 本命令的**窗口基址**（扇区 LBA，低 3 位清零）：FC 侧 desc->meta =
    // (meta & ~PLANE_CW_MASK) | (plane<<2) | cw ⇒ 逐 CW 铸出该 CW 的扇区 LBA。
    // 传窗口基址（而非 internal_lba）语义更直白，两者在 FC 掩掉低 3 位后等价。
    cmd->meta               = ftl_lba_pair_base(internal_lba);

    // 建 desc 链（逐 plane 各自的 PBA，位图 = 本子操作实际覆盖的 CW）；
    // desc/buffer 池空 → 瞬态背压
    ftl_status_e ret = ftl_cmd_setup_ppa_pair_cw(cmd, pba, cw_bitmap);
    if (ret != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return ret; /* FTL_AGAIN（池空，瞬态）/ FTL_ERROR 原样上抛 */
    }

    // 注册 pending 子操作（必须早于 submit，避免竞态）。读：owners = 单组位、写专用字段传
    // INVALID、cw_bitmap 传 0；lba 传**全局** LBA（L2P/Journal/META 均按全局索引）。
    pend_req_t *pending = pend_alloc(&ftl->pending);

    if (pending == NULL)
    {
        (void)fc_command_release(cmd);
        return FTL_AGAIN; /* pending 表满：瞬态 */
    }

    pending->command_id = ftl_cmd_id(cmd);
    pending->owners     = ftl_group_bit(gi);
    pending->op         = PENDING_OP_READ;
    pending->n_cw       = n;
    pending->cw_bitmap  = 0U;
    pending->lba        = internal_lba;
    pending->buffer     = buffer;
    pending->new_pba    = NAND_PBA_INVALID;
    pending->new_pba1   = NAND_PBA_INVALID;
    pending->cache_role = PEND_CACHE_ROLE_NONE;
    pend_validate(pending);

    *n_cw = n; /* 回填实际消费的 LBA 数（调用者据此推进） */

    // 记账（**必须早于 submit**——submit 之后 completion 可能立即被 drain 处理）
    pend_group_add_subop(&ftl->pending, gi);

    // 提交（tag = HOST_IO；ctx 传 NULL —— 私有状态在 pending 表）
    if (ftl_op_submit(cmd, FTL_OP_HOST_IO, NULL) != FTL_OK)
    {
        pend_remove(&ftl->pending, ftl_cmd_id(cmd));
        (void)fc_command_release(cmd);
        pend_group_drop_subop(&ftl->pending, gi);
        return FTL_AGAIN; /* SQ 满：瞬态 */
    }

    return FTL_OK;
}

/**
 * @brief drain 判卡死时的现场转储（调用点已限流）。
 *
 * why：「no progress」只是症状，真正没回来的资源可能是序列状态 / Journal 页写或擦 /
 * wbuf 忙 / pending 池 / 分配器 Open SB / FC 资源池中的任意一个。常规 `info` 只有
 * 计数，看不到哪一项卡着；把这几项一起打出来，下一次复现就能直接归因。
 *
 * @param ftl 实例
 * @param gi  被判定卡死的组
 *
 * @note 只读状态，不改任何记录。拆三行保证单行不超日志缓冲。
 */
static void ftl_drain_stall_dump(const ftl_ctx_t *ftl, WORD gi)
{
    const ftl_cache_seq_t *seq = &ftl->cache_seq;
    const jrn_handle_t    *jrn = &ftl->journal->handle;

    (void)LOGE(LOG_TAG_FTL "stall: seq kind=%d act=%d drain=%d close=%d brk=%d wr=%hu next=%lu\r\n", (int)seq->kind, (int)seq->active, (int)seq->draining, (int)seq->close_requested, (int)seq->break_requested, seq->wr.count, seq->next_lba);

    (void)LOGE(LOG_TAG_FTL "stall: jrn phase=%d erase=%d full=%d kind=%d ent=%hu seq=%lu jf=%lu\r\n", (int)jrn->phase, (int)jrn->need_erase, (int)jrn->ring_full, (int)jrn->page_kind, jrn->current_entry, jrn->sequence_counter, jrn_page_writes(ftl->journal));

    {
        fc_resource_stats_t rs;
        WORD                fpmin = 0U;

        memset(&rs, 0, sizeof(rs));
        (void)fc_get_resource_stats(&rs);

        // per-plane 最小空闲块数（RISK-ALLOC-09：成对分配看的是最饿的那个 plane）。
        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            const WORD c = alc_free_get_count(&ftl->alloc, p);

            if ((p == 0U) || (c < fpmin))
            {
                fpmin = c;
            }
        }

        (void)LOGE(LOG_TAG_FTL "stall: gi=%hu next=%lu/%lu wb_busy=%d pend=%hu free=%hu fpmin=%hu sb_open=%d np=%hhu p0=%02hhx fc cmd=%hu/%hu desc=%hu/%hu buf=%hu/%hu sq=%hu/%hu cq=%hu/%hu\r\n", gi, ftl->pending.groups[gi].next, ftl->pending.groups[gi].total, (int)(wb_is_idle(ftl->wbuf) == false), pend_slot_get_count(&ftl->pending), ftl->alloc.free_count, fpmin, (int)ftl->alloc.hot_sb.is_open, ftl->alloc.hot_sb.next_page, ftl->alloc.hot_sb.page0_programmed, rs.command_used, (WORD)FC_COMMAND_POOL_DEPTH, rs.descriptor_used, (WORD)FC_DESCRIPTOR_POOL_DEPTH, rs.buffer_used, (WORD)FC_BUFFER_POOL_DEPTH, rs.sq_used, (WORD)FC_SQ_DEPTH, rs.cq_used, (WORD)FC_CQ_DEPTH);
    }
}

/**
 * @brief 连续下发一个组的待下发子操作，直到全部下发完 / 遇瞬态背压（FTL_AGAIN）/遇永久失败（FTL_ERROR）。
 *
 * @param ftl 实例
 * @param gi 组下标。
 *
 * @note 实现要点：**进度单位是 LBA 偏移**（g->next / g->total 都以 LBA 计），不是子操作序号：
 * 一个子操作消费 1..FTL_LBA_PER_PAIR（8）个 LBA，取决于 FTL 现场判断
 * （读路径按 L2P 分组收缩、写路径按 pair 窗口夹住，见 ftl_issue_subop）——
 * "怎么拆"完全由 FTL 决定，调用者不必预先算出子操作数。每次迭代**重取** g
 * （下发可能改变组状态甚至释放本组）；全部子操作都在本函数外异步完成，
 * 故循环末尾统一试触发组完成通知——覆盖"整段未映射读"这种从不进 outstanding、
 * 无人触发完成的组。
 */
static void ftl_drain_group(ftl_ctx_t *ftl, WORD gi)
{
    pend_group_t *g = &ftl->pending.groups[gi];

    while ((g->used == true) && (g->next < g->total))
    {
        const DWORD off    = g->next; /* 已下发到的 LBA 偏移（g->next 以 LBA 计，非子操作序号） */
        const DWORD remain = g->total - off;
        DWORD       glba   = (g->lba + off);
        void       *buffer = (BYTE *)g->buffer + (off * FTL_IU_BYTES);
        // 本窗口内可提供的最大 LBA 数（≤ 一个 pair 窗口）。实际消费几个由
        // ftl_issue_subop 回填——读路径按 L2P 分组收缩，写路径按窗口边界收缩。
        BYTE n_cw          = (remain >= FTL_LBA_PER_PAIR) ? FTL_LBA_PER_PAIR : remain;

        ftl->l2p_defer = false; /* 每次下发前清零：本轮的背压原因由本次下发重新判定 */

        ftl_status_e st         = FTL_OK;
        bool         use_normal = true;

        // 连续 pair 的 two-plane cache read：是否进入序列由 FTL 顺序流检测器判
        // （stream_rd），序列所有者必须每轮尝试续窗口（窗口消费量不一定落在固定
        // 节拍上）。随机读只在检测器判为顺序流后才尝试，避免每次先付一轮 L2P 查找。
        const bool seq_owner = (ftl->cache_seq.active == true) && (ftl->cache_seq.owner_gi == gi);

        if (pend_op_is_read(g->op) && (seq_owner || (ftl->stream_rd.active == true)))
        {
            DWORD                   used = 0U;
            ftl_cache_read_result_e cr   = ftl_cache_read_try(ftl, gi, glba, buffer, remain, &used);

            switch (cr)
            {
                case FTL_CACHE_READ_OK:
                    g->stalls = 0U;
                    g->next += used;
                    continue;

                case FTL_CACHE_READ_DECLINED:
                    break; /* 条件不满足：显式回退普通读 */

                case FTL_CACHE_READ_AGAIN:
                    st         = FTL_AGAIN;
                    use_normal = false;
                    break;

                case FTL_CACHE_READ_ERROR:
                    st         = FTL_ERROR;
                    use_normal = false;
                    break;

                default:
                    ASSERT(false); /* H7/C7：新增枚举值必须在此显式处理 */
                    break;
            }
        }

        // host 16 KiB cache program：最多 4 个 pair 页；条件不满足回退 wbuf。
        if (use_normal && (g->op == PENDING_OP_WRITE))
        {
            DWORD                          used = 0U;
            const ftl_cache_write_result_e cw   = ftl_cache_write_try(ftl, gi, glba, buffer, remain, &used);

            switch (cw)
            {
                case FTL_CACHE_WRITE_OK:
                    g->stalls = 0U;
                    g->next += used;
                    continue;

                case FTL_CACHE_WRITE_DECLINED:
                    break; /* 条件不满足：显式回退 wbuf 普通写 */

                case FTL_CACHE_WRITE_AGAIN:
                    st         = FTL_AGAIN;
                    use_normal = false;
                    break;

                case FTL_CACHE_WRITE_ERROR:
                    st         = FTL_ERROR;
                    use_normal = false;
                    break;

                default:
                    ASSERT(false); /* H7/C7：新增枚举值必须在此显式处理 */
                    break;
            }
        }

        if (use_normal)
        {
            st = ftl_issue_subop(ftl, gi, glba, buffer, &n_cw);
        }

        g = &ftl->pending.groups[gi]; /* 重取：下发可能改变组状态 */

        if (st == FTL_OK)
        {
            g->stalls = 0U;
            g->next += n_cw;
            continue;
        }

        if (st == FTL_AGAIN)
        {
            // 瞬态背压：本轮到此为止，下轮 ftl_service Phase 2.5 再补。
            // 连续无进展超预算 → 判真卡死，弃未下发者并报错（已下发的照常完成）。
            // ⚠ **有确定完成时刻的后台进展不算无进展**：
            // · L2P region 加载在飞（l2p_defer）：NAND 层 wait/jump 超时保证命令必有完成；
            // · 搬移 job 在飞（GC / WL / slow-death）：它自己带 ms deadline，且 wbuf 在
            // Free Pool 空时正是靠它补池。一次回收要数百 ms，8 轮预算远不够，
            // 计进来会把正常等待判成 IO_ERROR（RISK-GC-09）。
            // 真卡死（命令丢失 / 搬移超时不退）由各自 deadline 与 NAND 超时兜底。
            // · 别的组持有 cache 设备序列：本组被序列门控挡回是**确定性等待**
            //   （所有者每轮都在推进），不能累计成「卡死」。
            const bool other_seq       = (ftl->cache_seq.active == true) && (ftl->cache_seq.owner_gi != gi);
            // 写序列所有者被确认 FIFO / 资源暂时挡回时，队列里的页命令一定有完成
            // （NAND 每命令有超时），属**有确定完成时刻的进展**，不计 stall。
            // 注意与 cache_seq.wr_inflight（在飞成员计数）区分：本项只看「本组的页还在页表里」。
            const bool wr_ring_pending = (ftl->cache_seq.kind == FTL_CACHE_SEQ_WRITE) && (ftl->cache_seq.owner_gi == gi) &&
                                         ((ftl->cache_seq.wr.count > 0U) || (ftl->cache_seq.wr_inflight > 0U));
            // 读序列所有者的在飞子操作同理：NAND 每命令有超时 ⇒ 必有完成。
            // 没有它，读窗口一次占满 FC 池时后续窗口的预算等待会被误判成卡死。
            const bool rd_inflight    = (ftl->cache_seq.kind == FTL_CACHE_SEQ_READ) && (ftl->cache_seq.owner_gi == gi) && (g->outstanding > 0U);
            // 序列正在有序收尾（padding END 已提交或在途）：这是确定性进展，等它完成。
            const bool seq_closing    = (ftl->cache_seq.closing == true) || (ftl->cache_seq.close_requested == true);
            // Journal 页写 / 换块擦在飞：NAND 命令有超时 ⇒ 必有完成，属确定性等待。
            // 少这一条，序列边界触发的页刷若跨过 8 轮预算，正常的写会被误判成卡死。
            const bool jrn_inflight   = (jrn_is_idle(ftl->journal) == false) || (ftl->journal->handle.need_erase == true);
            // 停泊的写确认：要么 region 就绪后结算，要么 100 ms 后超时释放（pending.c）
            // ⇒ 确定性等待。停泊本身不占 drain 的进展，不能算卡死。
            const bool confirm_parked = pend_find_op(&ftl->pending, PENDING_OP_WRITE_CONFIRM, NULL);
            const bool bg_progress    = (ftl->l2p_defer == true) || ftl->mig_handle->active || other_seq || wr_ring_pending || rd_inflight || seq_closing || jrn_inflight || confirm_parked;

            if (bg_progress == false)
            {
                if (++g->stalls > FTL_DRAIN_STALL_MAX)
                {
                    // 归因：GC 已判定「无收益 victim」时，这不是普通超时，是真正的无空间
                    // （free 见底且搬不动）。打印区分行，避免与瞬态背压混淆（RISK-GC-14）。
                    // 无空间是终止态、host 会持续重试 ⇒ 用限流器，避免逐请求刷屏。
                    DWORD suppressed = 0U;

                    if (ftl_log_throttle_due(&s_nospace_log, &suppressed))
                    {
                        if (ftl->mig_handle->gc_nogain)
                        {
                            LOGE(LOG_TAG_FTL "no space: GC no-gain (free=%hu) — IO_ERROR (suppressed=%lu)\r\n", ftl->alloc.free_count, suppressed);
                        }

                        LOGE(LOG_TAG_FTL "drain stalled gi=%hhu next=%lu/%lu (suppressed=%lu)\r\n", gi, g->next, g->total, suppressed);
                        ftl_drain_stall_dump(ftl, gi);
                    }
                    g->agg_status = SSD_STATUS_IO_ERROR;
                    g->next       = g->total;
                    ftl_cache_seq_close(ftl, gi);
                }
            }
            break;
        }

        // FTL_ERROR：永久失败 → 弃未下发者，组最终报错（已下发的照常完成）。
        // host 会重试 ⇒ 限流打印（保留首个失败位置与抑制计数）。
        DWORD suppressed = 0U;

        if (ftl_log_throttle_due(&s_subop_fail_log, &suppressed))
        {
            LOGE(LOG_TAG_FTL "subop failed gi=%hhu next=%lu/%lu (suppressed=%lu)\r\n", gi, g->next, g->total, suppressed);
        }
        g->agg_status = SSD_STATUS_IO_ERROR;
        g->next       = g->total;
        ftl_cache_seq_close(ftl, gi);
        break;
    }

    pend_group_maybe_complete(&ftl->pending, gi);
}

/**
 * @brief 复位顺序读检测状态，并使已算出的预取计划失效。
 *
 * @param ftl 实例（seq_* 字段与 seq_generation 由本函数更新）
 *
 * @note 写 / flush / trim 入口显式调用：它们会打断顺序读流。
 */
static void ftl_seq_reset(ftl_ctx_t *ftl)
{
    ftl->seq_last_end_lba    = 0U;
    ftl->seq_run             = 0U;
    ftl->seq_prefetch_valid  = false;
    ftl->seq_prefetch_target = 0U;
    ftl->seq_generation++;
}

/**
 * @brief 复位一个方向的顺序流检测器。
 *
 * @param det 检测器（stream_rd / stream_wr）
 */
static void ftl_stream_reset(ftl_stream_det_t *det)
{
    det->last_end = FTL_LBA_INVALID;
    det->run      = 0U;
    det->active   = false;
}

/**
 * @brief 按本请求更新顺序流检测器：只有与上一请求**首尾相接**才累计。
 *
 * @param det   检测器
 * @param lba   本请求起始 LBA
 * @param count 本请求 LBA 数
 *
 * @note run 在达到阈值后不再累加（只判 ≥ 阈值，避免长会话回绕）。
 */
static void ftl_stream_note(ftl_stream_det_t *det, DWORD lba, DWORD count)
{
    if (det->last_end == lba)
    {
        if (det->run < FTL_STREAM_MIN_LBAS)
        {
            det->run += count;
        }
    }
    else
    {
        det->run = count;
    }

    det->last_end = lba + count;
    det->active   = (det->run >= FTL_STREAM_MIN_LBAS);
}

/**
 * @brief 只更新检测状态并算出预取目标，不在这里起加载（host 读应先拿资源）。
 *
 * @param ftl 实例（检测字段与 seq_prefetch_target 由本函数更新）
 * @param req 刚受理的读请求（只读 lba / count）
 */
static void ftl_seq_note_read(ftl_ctx_t *ftl, const ftl_io_req_t *req)
{
    ftl->seq_generation++;
    ftl->seq_prefetch_valid = false;

    if (ftl->flush_active || ftl->trim_active || ftl->bg_suspend)
    {
        ftl_seq_reset(ftl);
        return;
    }

    const DWORD lba   = (DWORD)req->lba;
    const DWORD count = req->count;

    if ((ftl->seq_run != 0U) && (lba == ftl->seq_last_end_lba))
    {
        ftl->seq_run++;
    }
    else
    {
        ftl->seq_run = 1U;
    }

    ftl->seq_last_end_lba = lba + count;

    if (ftl->seq_run > ftl->seq_run_max)
    {
        ftl->seq_run_max = ftl->seq_run;
    }

    if (ftl->seq_run < FTL_SEQ_PREFETCH_MIN_RUN)
    {
        return;
    }

    const DWORD last         = lba + count - 1U;
    const DWORD last_region  = lba_to_global_region(last);
    const DWORD first_region = lba_to_global_region(lba);
    const DWORD used         = (DWORD)lba_to_entry(last) + 1U;
    const bool  cross_region = (last_region > first_region);
    const bool  near_end     = (used + FTL_SEQ_PREFETCH_AHEAD_LBAS) >= L2P_ENTRIES_PER_REGION;

    if ((cross_region == false) && (near_end == false))
    {
        return;
    }

    // 本实例的下一个 region（与 cache 序列跨 region 预加载共用同一坐标推导）。
    const DWORD target_lba = lba_next_region_base(last);

    if ((target_lba == FTL_LBA_INVALID) || ((QWORD)target_lba >= FTL_LBA_MAX) || (lba_routes_to(target_lba, ftl->topo) == false))
    {
        return; /* 只预取本实例会访问、且主机可见的 region */
    }

    ftl->seq_prefetch_target = lba_to_global_region(target_lba);
    ftl->seq_prefetch_valid  = true;
}

/**
 * @brief drain 之后执行预取；若 on_complete 重入更新过检测器，generation 变化即放弃。
 *
 * @param ftl 实例
 * @param generation 调用者记录的检测代次（与 ftl->seq_generation 不等即放弃）
 */
static void ftl_seq_prefetch_pending(ftl_ctx_t *ftl, DWORD generation)
{
    if ((generation != ftl->seq_generation) || (ftl->seq_prefetch_valid == false))
    {
        return;
    }

    const DWORD target      = ftl->seq_prefetch_target;
    ftl->seq_prefetch_valid = false;

    if (ftl->flush_active || ftl->trim_active || ftl->bg_suspend)
    {
        return;
    }

    (void)l2p_region_prefetch(ftl->cache, target);
}

/**
 * @brief 读/写共用的提交骨架——入参校验 + 建组 + 快路径下发。
 *
 * @param ftl 实例
 * @param req 请求（count = 连续 LBA 数，buffer 非空）
 * @param op PENDING_OP_READ / PENDING_OP_WRITE（决定组类型与日志标签）。
 *
 * @return FTL_OK    已建组（可能仍有余量待 Phase 2.5 补发）
 *         FTL_ERROR 参数非法 / 越界 / 组槽满
 *
 * @note 实现要点：两个公开入口的流程**逐字相同**（差异只有 op 与日志文案）⇒ 收敛到本函数
 * 一份，避免两处各自漂移（H6：注释与代码不一致即缺陷，重复实现更甚）。
 */
static ftl_status_e ftl_submit_common(ftl_ctx_t *ftl, const ftl_io_req_t *req, pend_op_e op)
{
    const char *label = (op == PENDING_OP_READ) ? "read" : "write";

    // host 入口：入参来自 HIL，属**不可信输入**（H29）⇒ 保留返回码，不转 ASSERT。
    if ((ftl == NULL) || (req == NULL) || (req->buffer == NULL))
    {
        return FTL_ERROR;
    }

    // volatile write cache 的**延后错误**（RISK-CACHE-27）：上一次已回执的写后来被判
    // 失败 ⇒ 用**下一条**命令承载这个结论（主机看到该命令失败一次），与控制器层
    // `usb_msc_take_deferred_error` 同口径（RISK-USBC-01）。
    if (ftl->deferred_error == true)
    {
        ftl->deferred_error = false;
        LOGE(LOG_TAG_FTL "%s: deferred write error from an earlier acked segment — fail this command once\r\n", label);
        return FTL_ERROR;
    }

    // 长度 = 连续 LBA 数（上层 block_count）；FTL 自行决定单/双 plane 拆分
    DWORD count = req->count;
    if (count == 0U)
    {
        return FTL_ERROR;
    }

    // LBA 范围检查（含末 LBA）。req->lba 是 QWORD ⇒ 加法按 64 位进行，不回绕。
    if ((req->lba + count) > FTL_LBA_MAX)
    {
        LOGE(LOG_TAG_FTL "%s lba out of range lba=%llu count=%lu\r\n", label, req->lba, count);
        return FTL_ERROR;
    }

    // 写/flush/trim 会打断顺序读流；写在入口处重置检测器。
    if (op != PENDING_OP_READ)
    {
        ftl_seq_reset(ftl);
    }

    // 顺序流检测（FTL 自判，不是 host 开关）：读 / 写各自累计连续 LBA；换向即复位另一侧。
    // 它决定 cache 序列**能否开启**；序列打开后的延续由 cache_seq.next_lba 判定
    // （host 传输段边界不构成断点，见 docs/design/ftl_design.md §3.16）。
    if (op == PENDING_OP_READ)
    {
        ftl_stream_note(&ftl->stream_rd, (DWORD)req->lba, count);
        ftl_stream_reset(&ftl->stream_wr);
    }
    else
    {
        ftl_stream_note(&ftl->stream_wr, (DWORD)req->lba, count);
        ftl_stream_reset(&ftl->stream_rd);
    }

    // 建完成组（一条 host 请求 = 一个组）。通知字段必须**早于任何下发**填入：
    // 下发可能当场完成整组，届时通知就要读它们。
    BYTE gi = 0U;

    if (pend_group_alloc(&ftl->pending, &gi) == false)
    {
        LOGE(LOG_TAG_FTL "%s: no free group slot\r\n", label);
        return FTL_ERROR;
    }
    pend_group_t *g  = &ftl->pending.groups[gi];
    g->op            = op;
    g->total         = count; /* 进度以 **LBA** 计（子操作数由 drain 现场决定，可降级） */
    g->next          = 0U;
    g->outstanding   = 0U;
    g->stalls        = 0U;
    g->lba           = req->lba;
    g->buffer        = req->buffer;
    g->agg_status    = SSD_STATUS_OK;
    g->cmd_remaining = req->cmd_remaining;

    // 序列延续只看 **LBA 连续性**（与 host 传输段边界 / 命令身份无关）：方向不同或
    // 接续点不一致 ⇒ 上一序列不可能再由本请求续。
    //   读序列：直接放弃（不发 LAST，RISK-IF-09）；
    //   写序列：设备侧没有数据页可 END ⇒ 由 service 用 **padding END** 有序收尾
    //          （不能 abort：未确认页已经对 host 提前回执，abort 会丢已确认数据）。
    if (ftl->cache_seq.active == true)
    {
        const ftl_cache_seq_kind_e want      = (op == PENDING_OP_READ) ? FTL_CACHE_SEQ_READ : FTL_CACHE_SEQ_WRITE;
        const bool                 continues = (ftl->cache_seq.kind == want) && (ftl->cache_seq.next_lba == (DWORD)req->lba);

        if (continues == false)
        {
            if (ftl->cache_seq.kind == FTL_CACHE_SEQ_READ)
            {
                ftl_cache_seq_release(ftl);
            }
            else
            {
                ftl->cache_seq.close_requested = true;
            }
        }
    }

    g->complete_flag   = req->complete_flag;
    g->status_out      = req->status_out;
    g->on_complete     = req->on_complete;
    g->on_complete_ctx = req->on_complete_ctx;

    // 先更新顺序检测（纯 RAM，无 I/O），drain 后才真正起预取，给 host 读让资源。
    // 记 generation：drain 内 on_complete 可能重入 submit 并再次更新检测器。
    DWORD seq_gen = 0U;

    if (op == PENDING_OP_READ)
    {
        ftl_seq_note_read(ftl, req);
        seq_gen = ftl->seq_generation;
    }

    // 快路径：小请求当场下发完；遇背压则余下留待 ftl_service Phase 2.5 补发。
    // 数据在 ftl_op_drain 中拷贝回 buffer（读），Journal + L2P 在完成
    // handler 中确认 NAND PROGRAM 成功后执行（写）。
    ftl_drain_group(ftl, gi);

    if (op == PENDING_OP_READ)
    {
        const pend_group_t *g_done = &ftl->pending.groups[gi];

        // 本组还有未下发的子操作（背压）⇒ 不给预取抢资源；等本组下发完再预取。
        if (g_done->next >= g_done->total)
        {
            ftl_seq_prefetch_pending(ftl, seq_gen);
        }
    }

    return FTL_OK;
}

bool ftl_take_deferred_error(ftl_ctx_t *ftl)
{
    if (ftl == NULL)
    {
        return false;
    }

    const bool has = ftl->deferred_error;

    ftl->deferred_error = false;
    return has;
}

ftl_status_e ftl_submit_read(ftl_ctx_t *ftl, const ftl_io_req_t *req)
{
    return ftl_submit_common(ftl, req, PENDING_OP_READ);
}

ftl_status_e ftl_submit_write(ftl_ctx_t *ftl, const ftl_io_req_t *req)
{
    return ftl_submit_common(ftl, req, PENDING_OP_WRITE);
}

/* ── 元数据落盘 / TRIM（供 HIL 调用）── */

/**
 * @brief host flush 也是"该落一次 checkpoint"的信号：没有 CP 时，掉电后只能拿初始 CP
 * （无块表）做全量重建。异步启动，限流 5s（半个 T3），避免 host 频繁 flush 写放大。
 *
 * @param ftl 实例（触发参数取自本实例的 CP / journal 状态）
 */
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
    trigger.journal_seq       = jrn_sequence_get(ftl->journal);
    trigger.journal_watermark = jrn_watermark_get(ftl->journal);
    trigger.emergency         = jrn_is_emergency(ftl->journal);
    trigger.force             = true;
    trigger.reason            = CP_REASON_EXPLICIT;

    cp_status_t status;
    cp_handle(ftl->cp_handle, &trigger, &status);
}

void ftl_flush_cancel(ftl_ctx_t *ftl)
{
    // 只摘 job 标志：wbuf / L2P / journal 的在飞操作有各自的服务路径，
    // 继续完成不会破坏不变量。下一次 begin 以新的起点重新开始。
    ftl->flush_active = false;
}

// 实现要点：· 主流形态是**排队命令**：提交即返回，完成走 HIL 的完成通道
// （complete_flag / status_out，见 docs/archive/ftl_host_interface.md A.1）。
// 旧实现在 USB SCSI 回调里同步自旋，窗口内主循环无法抽 USB 事件
// ⇒ 事件 FIFO 溢出（RISK-USB-06）。
// · 语义仍是 best-effort：不等待 host 在飞的写（BOT 串行 + CLI gate 已保证
// 提交时没有同类请求）；journal 持久性失败直接结束并打 WRN。
// · wbuf / L2P / journal 三者都异步推进：每轮 step 各踢一次，由主循环持续运行。
void ftl_flush_begin(ftl_ctx_t *ftl)
{
    if (ftl->flush_active)
    {
        return; /* 幂等：已有在飞的 flush */
    }

    ftl->flush_active   = true;
    ftl->flush_start_ms = ftl_job_now_ms();
    ftl_seq_reset(ftl);

    // 序列必须先退出，flush 的 L2P/journal 写才不会被门控。
    ftl_cache_seq_yield(ftl);

    // 先把页打包缓冲里尚未编程的写编程掉：它们还没进 L2P/journal，"落盘"不成立。
    // 遇瞬态背压会保留缓冲，后续 step 的 MAYBE_FLUSH_IDLE 会重试。
    wb_trigger_t trigger;

    memset(&trigger, 0, sizeof(trigger));
    trigger.kind = WBUF_TRIGGER_FLUSH;
    (void)wb_handle(ftl->wbuf, &trigger, NULL, NULL);
}

bool ftl_flush_step(ftl_ctx_t *ftl)
{
    if (ftl->flush_active == false)
    {
        return true; /* 没有在飞的 flush：对调用者而言就是"已完成" */
    }

    const DWORD dim = ftl_topo_dim(ftl->topo);

    // ① wbuf：把打开的窗口推下去，并取当前状态。
    wb_trigger_t wb_trigger;

    memset(&wb_trigger, 0, sizeof(wb_trigger));
    wb_trigger.kind = WBUF_TRIGGER_MAYBE_FLUSH_IDLE;

    wb_status_t wb_status;
    (void)wb_handle(ftl->wbuf, &wb_trigger, NULL, &wb_status);

    // ② L2P：继续 kick 本 owner 的脏行（受共享 credit 池门控）。
    l2p_owner_flush_kick(ftl->cache, dim, L2P_CACHE_SIZE);

    // ③ Journal：推进异步刷页 job。
    const jrn_trigger_e jrn_trigger = JRN_TRIGGER_FLUSH;
    jrn_status_t        jrn_status;
    jrn_handle(ftl->journal, &jrn_trigger, NULL, &jrn_status);

    // ④ 完成判据：四类在飞数据全部落定。停泊中的写确认也必须先完成——它对应的
    // L2P 脏行还没出现，否则 flush 会「提前完成」。
    const bool confirm_pending  = pend_find_op(&ftl->pending, PENDING_OP_WRITE_CONFIRM, NULL);
    // 早回执后 cache 页的等待对象变了：组早已完成，flush 要等**已编程页表**排空
    // （每页都已被后继/收尾证明、且已确认进 L2P），否则 SYNCHRONIZE CACHE 会在
    // 「未证明」窗口里提前回执（RISK-CACHE-18 的屏障语义）。
    // 页在**完成时**才进页表 ⇒ 屏障必须同时看「在飞成员」：控制器可能已对写段
    // 提前回执（BOT 串行下 SYNCHRONIZE CACHE 紧接着到来），此时命令还在 NAND 里编程。
    const bool cache_wb_pending = (ftl->cache_seq.wr.count != 0U) || (ftl->cache_seq.wr_inflight != 0U);
    const bool cache_wr_active  = (ftl->cache_seq.active == true) && (ftl->cache_seq.kind == FTL_CACHE_SEQ_WRITE);
    const bool wb_done          = (wb_status.state == WBUF_STATE_IDLE) && (wb_status.occupied == 0U);
    const bool l2p_done         = l2p_flush_owner_idle(ftl->cache, dim);
    const bool jrn_done         = jrn_is_idle(ftl->journal) && (jrn_has_pending_entries(ftl->journal) == false);
    const bool jrn_fail         = jrn_has_write_failure(ftl->journal);
    const bool wb_fail          = (wb_status.state == WBUF_STATE_FAILED);
    const bool fail             = jrn_fail || wb_fail;

    // 墙钟上限：元数据侧真卡死时 host FLUSH 不能永不完成（见 FTL_FLUSH_TIMEOUT_MS）。
    const DWORD flush_ms = ftl_job_now_ms() - ftl->flush_start_ms;
    const bool  timeout  = (ftl->flush_start_ms != 0U) && (flush_ms >= FTL_FLUSH_TIMEOUT_MS);

    if (((wb_done && l2p_done && jrn_done && (confirm_pending == false) && (cache_wb_pending == false) && (cache_wr_active == false)) || fail || timeout) == false)
    {
        return false;
    }

    ftl->flush_active = false;

    DWORD suppressed = 0U;

    if (fail)
    {
        // best-effort：journal 页写/擦或 wbuf 编程持续失败。CP 成功后
        // jrn_write_failure_clear 清 journal 标志；wbuf FAILED 由下一次成功操作清。
        if (ftl_log_throttle_due(&s_flush_err_log, &suppressed))
        {
            LOGW(LOG_TAG_FTL "flush: write failure pending (jrn=%d wb=%d) — complete best-effort (suppressed=%lu)\r\n", jrn_fail, wb_fail, suppressed);
        }
    }
    else if (timeout)
    {
        if (ftl_log_throttle_due(&s_flush_err_log, &suppressed))
        {
            LOGW(LOG_TAG_FTL "flush timeout %lums (wb=%d l2p=%d jrn=%d) — best-effort complete (suppressed=%lu)\r\n", flush_ms, wb_done, l2p_done, jrn_done, suppressed);
        }
    }

    ftl_flush_kick_cp(ftl);

    // 耗时日志限流 5 s（单线程 superloop；多实例共享一个时间戳即可）。host 若
    // 频繁 fsync，逐次打印会刷屏。
    const DWORD now_log = ftl_job_now_ms();

    if ((ftl->flush_start_ms != 0U) && (flush_ms >= FTL_FLUSH_SLOW_LOG_MS) && (timeout == false) && ((s_flush_log_ms == 0U) || ((now_log - s_flush_log_ms) >= FTL_JOB_LOG_SLOW_INTERVAL_MS)))
    {
        s_flush_log_ms = now_log;
        LOGW(LOG_TAG_FTL "flush took %lums (async job)\r\n", flush_ms);
    }

    return true;
}

#define FTL_TRIM_STEP_LBAS  (8U)
#define FTL_TRIM_TIMEOUT_MS (1000U) /* 无进展上限：Journal ring_full / L2P 加载卡死时让 TRIM 失败 */

/**
 * @brief 本轮没有进展时的等待记账：超过上限 ⇒ 中止 TRIM（避免 CLI/USB 永久 BUSY）。
 * 有进展（processed > 0）就清等待起点；平台时钟缺失时退化为无超时（同 FLUSH）。
 *
 * @param ftl 实例（trim_wait_ms 由本函数更新）
 * @param processed 本轮已处理的 LBA 数（0 = 无进展）
 * @return FTL_AGAIN 继续等待
 *         FTL_ERROR 等待超时，已中止 TRIM
 */
static ftl_status_e ftl_trim_defer(ftl_ctx_t *ftl, BYTE processed)
{
    if (processed != 0U)
    {
        ftl->trim_wait_ms = 0U;
        jrn_watermark_update(ftl->journal);
        return FTL_AGAIN;
    }

    const DWORD now_ms = ftl_job_now_ms();

    if (now_ms != 0U)
    {
        if (ftl->trim_wait_ms == 0U)
        {
            ftl->trim_wait_ms = now_ms;
        }
        else if ((now_ms - ftl->trim_wait_ms) >= FTL_TRIM_TIMEOUT_MS)
        {
            LOGE(LOG_TAG_FTL "trim: wait timeout lba=%lu\r\n", ftl->trim_lba);
            ftl->trim_active  = false;
            ftl->trim_wait_ms = 0U;
            return FTL_ERROR;
        }
    }

    return FTL_AGAIN;
}

// 实现要点：· 每次 step 最多推进 FTL_TRIM_STEP_LBAS 条，避免长 TRIM 占满主循环；
// · L2P region 未就绪 / Journal 容量不足 ⇒ 只起异步加载/刷页并返 FTL_AGAIN，
// 游标不前进，下一轮重试（不产生半状态）；
// · Journal 先于 L2P，与 host 写确认同序；`jrn_reserve(1)` 保证 append 不触发
// 页满分支；
// · 由 CLI 控制面驱动（controller 的 TRIM 请求），数据面不调用。
ftl_status_e ftl_trim_begin(ftl_ctx_t *ftl, DWORD lba, DWORD count)
{
    ASSERT(ftl != NULL);

    if (ftl->trim_active)
    {
        return FTL_AGAIN; /* 单飞行：已有 TRIM job */
    }
    if (count == 0U)
    {
        return FTL_ERROR;
    }
    if (((QWORD)lba + count) > (QWORD)FTL_LBA_MAX)
    {
        return FTL_ERROR;
    }

    ftl->trim_lba       = lba;
    ftl->trim_end       = lba + count;
    ftl->trim_start_lba = lba;
    ftl->trim_start_ms  = ftl_job_now_ms();
    ftl->trim_active    = true;
    ftl->trim_wait_ms   = 0U;
    ftl_seq_reset(ftl);

    // 与 flush 同口径：TRIM 要写 Journal/L2P，序列必须先退出。
    ftl_cache_seq_yield(ftl);

    LOGI(LOG_TAG_TRIM "start lba=%lu n=%lu\r\n", lba, count);
    return FTL_OK;
}

ftl_status_e ftl_trim_step(ftl_ctx_t *ftl)
{
    ASSERT(ftl != NULL);

    if (ftl->trim_active == false)
    {
        return FTL_OK;
    }

    BYTE processed = 0U;

    while ((ftl->trim_lba < ftl->trim_end) && (processed < FTL_TRIM_STEP_LBAS))
    {
        const DWORD l = ftl->trim_lba;

        // 该 LBA 还在页打包写缓冲里（未编程）：只清缓冲槽，不碰 L2P/journal——它从未
        // 落盘，无映射变更需记。缓冲的内部不变量（清空 ⇒ 结算 owners）由 wb_handle(DROP)
        // 收口。
        // ⚠ 这里不清旧 L2P 映射：当前依赖 Controller 串行化（TRIM 时该 LBA 没有未完成
        // 写）。若将来并发 TRIM，drop 后旧映射仍可读 ⇒ 见 RISK-HOST-09。
        if (wb_has(ftl->wbuf, l))
        {
            wb_trigger_t trigger;
            memset(&trigger, 0, sizeof(trigger));
            trigger.kind = WBUF_TRIGGER_DROP;
            trigger.lba  = l;
            (void)wb_handle(ftl->wbuf, &trigger, NULL, NULL);

            ftl->trim_lba++;
            processed++;
            continue;
        }

        if (l2p_is_ready(ftl->cache, l) == false)
        {
            return ftl_trim_defer(ftl, processed); /* 已起异步加载：游标不动 */
        }
        if (jrn_reserve(ftl->journal, 1U) == false)
        {
            return ftl_trim_defer(ftl, processed); /* 已起异步刷页：游标不动 */
        }

        DWORD old_pba;

        const ftl_status_e rst = l2p_read(ftl->cache, l, &old_pba);
        if (rst == FTL_AGAIN)
        {
            return ftl_trim_defer(ftl, processed);
        }
        if (rst != FTL_OK)
        {
            LOGE(LOG_TAG_FTL "trim: l2p read lba=%lu failed\r\n", l);
            ftl->trim_active = false;
            return FTL_ERROR;
        }

        // Journal 先于 L2P（同 host 写确认）；预留已保证 append 不触发页满分支。
        (void)jrn_append(ftl->journal, l, old_pba, NAND_PBA_INVALID);

        const ftl_status_e wst = l2p_write(ftl->cache, l, NAND_PBA_INVALID);

        ASSERT(wst == FTL_OK);
        (void)wst;

        // 有效计数在两步之后减：它只是 RAM 统计，不进 Journal。
        if (old_pba != NAND_PBA_INVALID)
        {
            const WORD gblock = ftl_pba_gblock(old_pba);

            // 同写确认路径：BAD / count 已清时跳过，避免断言或 `WORD` 回绕。
            ftl_dangling_check(ftl, l, old_pba, FTL_GBLOCK_INVALID);
            ftl_block_sub_valid_if_live(&ftl->alloc.blocks[gblock]);
        }

        ftl->trim_lba++;
        processed++;
    }

    if (processed != 0U)
    {
        ftl->trim_wait_ms = 0U;
        jrn_watermark_update(ftl->journal);
    }

    if (ftl->trim_lba >= ftl->trim_end)
    {
        const DWORD n       = ftl->trim_end - ftl->trim_start_lba;
        const DWORD elapsed = (ftl->trim_start_ms != 0U) ? ftl_job_age_ms(&ftl->trim_start_ms) : 0U;

        LOGI(LOG_TAG_TRIM "done n=%lu elapsed=%lums\r\n", n, elapsed);
        ftl->trim_active    = false;
        ftl->trim_start_lba = 0U;
        ftl->trim_start_ms  = 0U;
        ftl->trim_wait_ms   = 0U;
        return FTL_OK;
    }

    return FTL_AGAIN;
}

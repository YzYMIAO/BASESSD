#ifndef FTL_CACHE_H
#define FTL_CACHE_H

#include "fc_interface.h"
#include "ftl_internal.h"
#include "pending.h"

/* cache 写序列的未确认页 FIFO 深度。
 *
 * 提交侧按 `wr.count + wr_inflight` clamp 到空闲槽（cache_wr_try 的 ring_free），
 * 正常运行下 push 不会溢出；环满仍按结构性错误处理（member_done 断言 + abort，
 * ）。 */
#define FTL_CACHE_SEQ_RING (16U)

/* cache 流式序列状态（per instance；读 / 写各自一套，kind 区分）。
 * start/middle/end 由 FTL 的方向 + 连续性判断决定，不依赖 FC 的额外标志。 */
typedef enum FTL_CACHE_SEQ_KIND_E
{
    FTL_CACHE_SEQ_NONE = 0,
    FTL_CACHE_SEQ_READ,
    FTL_CACHE_SEQ_WRITE,
} ftl_cache_seq_kind_e;

/* 写序列的「已编程页表」（读序列不用）。
 *
 * 缓存程序是流水线：第 i 页的编程结论由**第 i+1 条命令**的 FAIL_CACHE 给出，
 * END（10h）的 FAIL 给出最后一页的结论。早回执把**组完成**从
 * 「等到 FAILC」提前到「本页命令完成」，因此本表要在窗口内记住每页的落点：
 *   · 读路径靠它把 LBA 指到刚编程的页（此时 L2P 可能还没确认）；
 *   · 后续 FAILC 判失败时靠它撤销。
 * 环按**提交顺序**；head 是最老、尚未被弹出（= 尚未「已证明且已确认」）的页。
 * 结论落点**按 LBA 绑定**，不按入表顺序（见 ftl_cache_wr_s.verdict）。 */
typedef enum FTL_CACHE_PAGE_STATE_E
{
    FTL_CACHE_PAGE_PROGRAMMED = 0, /* 已编程；待确认（Journal→L2P→BST） */
    FTL_CACHE_PAGE_CONFIRMED,      /* 已进 L2P；等后继证明 */
    FTL_CACHE_PAGE_FAILED,         /* 后继 FAILC 判失败；等撤销（L2P 失效 + region 降级） */
} ftl_cache_page_state_e;

typedef struct FTL_CACHE_PAGE_S
{
    DWORD lba;       /* 页基址 */
    DWORD pba0;      /* plane0 的真实 PBA（Policy B 下两 plane 块号不同，不可用
                      * pba_with_plane 从 plane0 推导） */
    DWORD pba1;      /* plane1 的真实 PBA */
    BYTE  cw_bitmap; /* 本页覆盖的 CW（通常 FTL_CACHE_FULL_CW） */
    BYTE  state;     /* ftl_cache_page_state_e */
    BOOL  proven;    /* 后继命令的 FAIL 结论已到、且成功（END 由自身 STATUS 证明） */
} ftl_cache_page_s;

/* cache 写序列的「待落结论」：FAIL_CACHE 按**页 LBA** 绑定，不依赖完成顺序。
 *
 * 每条 cache program 命令的 FAIL_CACHE 指的是**提交序前一页**（B 页命令判 A 页）。
 * 建链时把 A 的 LBA 记进 B 的 pending 槽；完成时按 LBA 找页落结论。若 A 还没入表
 * （完成顺序反转），先存进本表，A 入表时消费——因此完成顺序不再影响正确性。 */
typedef struct FTL_CACHE_VERDICT_S
{
    DWORD lba;   /* 结论指向的页 LBA */
    BOOL  ok;    /* true = 编程成功；false = FAIL_CACHE */
    BOOL  valid; /* false = 空槽 */
} ftl_cache_verdict_s;

typedef struct FTL_CACHE_WR_S
{
    ftl_cache_page_s    page[FTL_CACHE_SEQ_RING];    /* 已编程页（提交顺序） */
    BYTE                head;                        /* 队头环形下标（确认/弹出的起点） */
    BYTE                count;                       /* 已编程未证明的页数 */
    BYTE                verdict_next;                /* 待落结论表的轮转下标 */
    ftl_cache_verdict_s verdict[FTL_CACHE_SEQ_RING]; /* 页尚未入表时的暂存结论 */
} ftl_cache_wr_s;

typedef struct FTL_CACHE_SEQ_S
{
    ftl_cache_seq_kind_e kind;
    BYTE                 gen; /* 写序列代号：**中止时**递增。cache 成员的完成带自己的
                               * 代号，与当前代号不一致 ⇒ 迟到完成（序列已死），只结算
                               * 组、不碰页表。正常开新序列不递增：迟到的 END 页
                               * （proven）与新页表关系明确，递增反而误判合法完成。 */
    BYTE wr_inflight;         /* 已下发、尚未完成的 cache 写成员数（提交时 +，完成时 -）。
                               * 页在**完成时**才进页表，只看 wr.count 会漏掉仍在编程
                               * 的页：flush 屏障与「开新序列」都必须等它归零
                               * 。 */

    /* ⚠ 偏离 H32（P5 留痕）：active / draining / closing 是**同一设备管线**的
     * 互斥状态，这里沿用既有 BOOL 组而不是互斥枚举（改动面大且需同步全部判定点）；
     * 互斥性由 ftl_cache_seq_release / close_step / pad 回调三处维护。
     * 后续重构时收敛成一个状态枚举。 */

    BOOL active;                      /* true = 设备侧序列打开（START 已发、END 未发） */
    BOOL draining;                    /* true = 读序列已放弃但命令仍在飞：门控继续关着，
                                       * 直到所有者组的在飞子操作归零。 */
    BOOL owner_submit;                /* 序列所有者正在提交窗口（门控旁路） */
    BOOL break_requested;             /* 非所有者提交被门控拒绝 ⇒ 请 service 收尾序列 */
    BOOL yield_pending;               /* 后台（L2P 加载等）被门控拒绝 ⇒ host 不得立刻开新
                                       * 序列，保证后台在本轮拿到一次提交机会；由 L2P
                                       * 提交成功或 `yield_ms` 超时清除。 */
    DWORD yield_ms;                   /* yield_pending 置位时刻（超时兜底，防饿死 host） */
    BOOL  close_requested;            /* 写序列需要**有序收尾**（下一段 LBA 不连续 / 空闲 /
                                       * flush / 前台等待）：service 用 padding END 落下
                                       * 一个收尾页，把最后一页的 FAIL_CACHE 结算掉。 */
    BOOL closing;                     /* padding END 已在飞：门控继续关，续写窗口被拒（等 pad 完成） */
    BOOL own_page0;                   /* 当前 hot SB 的 page0 由**本序列**写入：允许它在确认前
                                       * 继续写 page>0（有序管线保证 page0 失败会中止其后所有页，
                                       * 见 allocator.h 的 alc_super_open_get_room_seq）。
                                       * 序列开始即清、分配 page0 时置位；跨 block 续写时由新块
                                       * 的分配重新置位（新 SB 的 page0 同样由本序列写）。 */
    WORD  owner_gi;                   /* 最近持有序列的组（窗口记账 / 门控归因） */
    DWORD next_lba;                   /* 续序列的判据：下一个 host 数据段必须从这里开始 */
    DWORD idle_ms;                    /* 所有者组已释放/已发完后等待下一段的起点 */
    DWORD run_pairs;                  /* 读序列本段已提交的 pair 数（跨 block 续接的让步上限，
                                       * 见 cache_batch.h 的 FTL_CACHE_READ_MAX_RUN_PAIRS） */
    DWORD pad_pba[PLANE_NUM];         /* padding END 页的逐 plane PBA。Policy B 下两 plane 的块号
                                       * 不同 ⇒ 回调记账必须用真实地址，不能用 pba_with_plane()
                                       * 从 plane0 推导（那样会漏记 plane1 的 page0 屏障）。 */
    DWORD          pad_verdict_lba;   /* padding END 的 FAIL_CACHE 指向的页 LBA（提交时确定） */
    BOOL           pad_verdict_valid; /* false = pad 前面没有数据页（无结论可落） */
    ftl_cache_wr_s wr;                /* 写序列的逐页确认 FIFO */
} ftl_cache_seq_s;

/* 顺序流检测器（per instance × per direction）——FTL 自己判断顺序读写的状态。
 *
 * 判据只有一条：同一方向相邻 host 请求**首尾相接**（本请求起始 LBA == 上一请求的
 * 结束 LBA）。连续长度累计达到 FTL_STREAM_MIN_LBAS 即进入顺序流；LBA 空档或换向
 * 即复位（flush / trim 不打断检测——它们不改变后续写的 LBA 连续性，新起点由判据
 * 自然处理）。它只决定 cache 序列**能否开启**；序列一旦打开，延续由
 * cache_stream.seq.next_lba 判定（与 host 传输段边界无关）。 */
typedef struct FTL_STREAM_DETECT_S
{
    DWORD last_end; /* 上一请求的结束 LBA（exclusive）；FTL_LBA_INVALID = 无上一请求 */
    DWORD run;      /* 连续 LBA 累计（封顶在阈值附近，只用于判 ≥ FTL_STREAM_MIN_LBAS） */
    BOOL  active;   /* true = 连续长度已达阈值（顺序流） */
} ftl_stream_det_s;

/* cache 流式模块状态（per instance）——顺序检测 / 读预取 / 观测计数 / 设备侧序列。
 *
 * 两个检测器**坐标不同**，不要合并：
 *   · rd / wr（ftl_stream_det_s）：按 **LBA 数**累计，flush / trim 不打断；
 *     判据是 cache 序列**能否开启**（FTL_STREAM_MIN_LBAS）。
 *   · prefetch_*：按 **请求次数**累计，flush / trim 时复位；
 *     只用于 L2P region 预取（FTL_SEQ_PREFETCH_MIN_RUN）。 */
typedef enum FTL_L2P_DEMAND_WINDOW_STATE_E
{
    FTL_L2P_DEMAND_WINDOW_IDLE = 0,
    FTL_L2P_DEMAND_WINDOW_OPEN,
    FTL_L2P_DEMAND_WINDOW_EXPIRED,
} ftl_l2p_demand_window_state_e;

/* 写确认依赖的 demand L2P 加载被 cache 序列门控拒绝时的独立提交窗口。
 * 只挡**新** cache 序列，不打断已有序列；窗口有界，超时后恢复 host。 */
typedef struct FTL_L2P_DEMAND_WINDOW_S
{
    ftl_l2p_demand_window_state_e state;
    timer_deadline_s              deadline;
} ftl_l2p_demand_window_s;

typedef struct FTL_CACHE_STREAM_S
{
    /* 设备侧 cache 读写序列（单 LUN 同时一条；跨窗口 / 跨 chunk 保持）。 */
    ftl_cache_seq_s seq;

    /* confirm 依赖的 demand L2P 独立提交窗口（调度状态归 cache stream）。 */
    ftl_l2p_demand_window_s l2p_demand;

    /* 顺序流检测：cache 序列的开启判据。 */
    ftl_stream_det_s rd; /* 读方向 */
    ftl_stream_det_s wr; /* 写方向 */

    /* 顺序读检测：只用于 L2P region 预取。 */
    DWORD prefetch_last_end;   /* 上一读请求末 LBA（exclusive） */
    DWORD prefetch_run;        /* 连续读请求计数 */
    DWORD prefetch_run_max;    /* 本次开机最大连续读长度（观测） */
    DWORD prefetch_generation; /* 每次检测更新/重置递增，防 on_complete 重入覆盖 */
    DWORD prefetch_target;     /* 本次检测算出的目标 region */
    BOOL  prefetch_valid;      /* target 有效且尚未执行 */

    /* 观测计数（不参与决策；用于性能归因）。 */
    DWORD read_batches;   /* 成功提交的读窗口数 */
    DWORD read_pairs;     /* 累计提交的 pair 数（pairs / batches = 平均窗口深度） */
    DWORD read_starts;    /* 打开的读序列段数（START 计数；batches / starts = 平均段长） */
    DWORD read_declines;  /* 收集不足最小 pair 数而放弃尝试的次数（不连续 / wbuf 命中 / L2P 加载中） */
    DWORD seq_breaks;     /* D4：门控拒绝导致的序列提前收尾次数（读+写） */
    DWORD read_fallbacks; /* 已决定走 cache read 但资源不足或提交失败而回退的次数 */

    DWORD wb_batches;    /* 成功提交的写窗口数 */
    DWORD wb_pages;      /* 累计提交的页数（pages / batches = 平均窗口深度） */
    DWORD wb_declines;   /* 条件不满足而回退 wbuf 的次数 */
    DWORD wb_aborts;     /* 批次首次成员失败而中止的次数（同一批只计一次） */
    DWORD wb_starts;     /* 打开的写序列段数（START 计数） */
    DWORD wb_page_ends;  /* 写满一张 Journal 顺序页而主动 END 的次数（≈每 252 KiB 一次） */
    DWORD wb_pad_closes; /* padding END 有序收尾的次数（LBA 不连续 / 空闲 / flush） */

    DWORD demand_window_open;    /* 为 confirm 依赖的 demand load 开独立窗口次数 */
    DWORD demand_window_blocked; /* 因该窗口拒绝开新 cache 序列的次数 */
    DWORD demand_window_expired; /* 窗口超时、恢复 host 的次数 */
} ftl_cache_stream_s;

/* ── 查询 / 谓词 ── */

/**
 * @brief 读覆盖层：该 LBA 是否有「已编程、尚未进 L2P」的页。
 *
 * @param ftl     实例。
 * @param lba     全局 host LBA。
 * @param out_pba 命中时写入该 LBA 所在 plane 的页基址 PBA（调用者再按 cw 取）。
 *
 * @return true = 命中，读必须走 out_pba。
 */
BOOL ftl_cache_wr_lookup(const ftl_ctx_s *ftl, DWORD lba, DWORD *out_pba);

/**
 * @brief 该 LBA 是否落在**已判失败、尚未撤销**的 cache 页上。
 *
 * @param ftl 实例。
 * @param lba 全局 host LBA。
 *
 * @return true = 读该 LBA 必须报 IO_ERROR（撤销完成前 L2P 可能仍指着失败页）。
 */
BOOL ftl_cache_wr_failed_covers(const ftl_ctx_s *ftl, DWORD lba);

/* ── 序列控制 ── */

/**
 * @brief 只清 RAM：关闭 cache 序列状态（不发任何设备命令）。
 *
 * @param ftl 实例。
 *
 * @note 设备侧未收 LAST 的缓存序列由随后的新命令终止（待上板确认）。
 *       若调用时仍有该序列的命令在飞，置 `draining` 让门控继续关到排空——否则别的组会在
 *       31h/3Fh 管线半途插命令。
 */
void ftl_cache_seq_release(ftl_ctx_s *ftl);

/**
 * @brief 破坏性控制路径（format）丢弃 cache 序列并强制打开门控。
 *
 * @param ftl 实例。
 *
 * @note 与 `ftl_cache_seq_release` 的差别：release 对读序列在飞会保留
 *       `draining`（管线保护）；格式化不需要这条保护，且
 *       `active/draining` 会让 `ftl_op_seq_gate` 拒绝 SYNC_WAIT 的擦写提交，
 *       使整片格式化退化为每次 20 ms 超时。未证明页按失败结算，不产生静默映射。
 */
void ftl_cache_seq_discard(ftl_ctx_s *ftl);

/** @brief 写序列有序收尾（padding END）完成后关闭序列状态。 */
void ftl_cache_seq_close(ftl_ctx_s *ftl, WORD gi);

/** @brief 让出设备序列：写序列走 padding END，读序列直接放弃。 */
void ftl_cache_seq_yield(ftl_ctx_s *ftl);

/* ── 写序列步进 ── */

/** @brief cache program 成员的完成入口（早回执 + FAILC 结算）。 */
void ftl_cache_wr_member_done(ftl_ctx_s *ftl, pend_req_s *pending, const fc_cmd_status_s *completion);

/** @brief 推进一页的确认（Journal→L2P→BST），与组完成解耦。 */
void ftl_cache_wr_confirm_step(ftl_ctx_s *ftl);

/** @brief 序列收尾步进：等 padding END 完成并关闭门控。 */
void ftl_cache_seq_close_step(ftl_ctx_s *ftl);

/** @brief 复位本模块文件级状态（NOLOAD 段不清零）。 */
void ftl_cache_wr_init(void);

#if defined(FC_NAND_TEST_ENABLE)
/**
 * @brief 测试专用：驱动 padding END 提交（CACHE-15 盘满例外）。
 */
ftl_status_e ftl_cache_test_pad_submit(ftl_ctx_s *ftl);
#endif

#endif /* FTL_CACHE_H */

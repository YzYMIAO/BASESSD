#ifndef FTL_SERVICE_H
#define FTL_SERVICE_H

#include "ftl_capacity.h"
#include "ftl_interface.h"

/*
 * ftl_service — 主循环调度器
 *
 * 每轮 superloop 两段（**泵与实例分开**）：
 *   ① ftl_service_pump()   —— **实例无关**：[pump] 推进 NAND + [pump] 完成泵。
 *                              全局的 NAND 引擎与完成表，每轮**只调一次**。
 *   ② ftl_service(ftl)     —— **单实例**：元数据候选池重算 → 写确认结算 →
 *                              cache 序列确认 → scheduler host 补发 → 序列收尾 →
 *                              wbuf 空闲 flush → 后台调度（逐步清单见 ftl_service 契约）。
 *                              对**每一个**实例都要调（多 LUN 下不能只跑 [0][0][0]）。
 *
 * 三级模式切换依据未决命令数（pending + 组），非"本轮提交了几条 NAND"。
 */

#include "allocator.h"   /* ftl_table_refs_s, super_block_s */
#include "bg_policy.h"   /* 工作量配额后台调度器（ftl_ctx_s 含调度器） */
#include "cache_batch.h" /* cache read/write 批次的常量与对 FTL 的尝试入口 */
#include "checkpoint.h"
#include "ftl_sched.h" /* FTL request scheduler */
#include "ftl_type.h"  /* ftl_io_req_s */
#include "gc.h"
#include "journal.h"
#include "l2p_cache.h"
#include "metadata.h"
#include "migrate.h"
#include "pending.h"
#include "read_recovery.h"
#include "ssd_types.h"
#include "stats.h" /* SSD_STATS_ENABLE（统计专用字段按档裁剪） */
#include "timer.h"
#include "wbuf.h"

#include <stdbool.h>
#include <stdint.h>

/* ── 宏 ── */

#define NAND_SUBMIT_BUDGET  (4U)     /* 每轮 fc_service 最多跑 4 次（首轮抽干完成，余为填充轮次） */
#define CP_WRITE_INTERVAL   (1024U)  /* 写入量触发间隔 */
#define CP_TIME_INTERVAL_MS (10000U) /* 定时触发间隔（ms）*/

#define FTL_READ_USER_SIZE (2048U) /* 一次读返回的用户数据大小 = 4 CW × 512B */

/* 异步 FLUSH job 的耗时告警阈值：从 `ftl_flush_begin` 到完成
 * 超过该值即打一行 WRN。用于观察主机看到的 flush 延迟；job 本身不阻塞主循环。 */
#define FTL_FLUSH_SLOW_LOG_MS (10U)

/* 异步 FLUSH job 的**无进展**上限：只要 wbuf / L2P / journal / 写确认仍在推进，
 * 就按进展续期；连续 1 s 没有任何推进才判超时。超时按 best-effort 完成并打 WRN；
 * 若有未确认写页，同时置 deferred_error（不能对 host 宣称成功，H14/）。
 * 1 s 远小于主机 SCSI/BOT 超时，又能覆盖正常的满盘 flush。 */
#define FTL_FLUSH_TIMEOUT_MS (1000U)

/* 一个组的子操作连续遇 FTL_AGAIN（瞬态背压）而**无任何进展**的轮数上限。
 * 超过即判真卡死：弃未下发子操作、组报 IO_ERROR（已下发的照常完成）。
 * 预算取 8——正常背压在下轮 ftl_service 抽干完成后即恢复，连续 8 轮无进展属异常。
 * ⚠ 「有确定完成时刻的后台进展」不计入：L2P region 加载在飞、搬移 job（GC/WL）
 *   在飞时清零/不累加——否则一次数百 ms 的正常回收会被 8 轮预算判成 IO_ERROR
 *   。 */
#define FTL_DRAIN_STALL_MAX (8U)

/* cache 序列跨 chunk 等待下一 chunk 的上限：超过即关闭序列状态，避免门控永久
 * 挡住该 LUN。16 KiB chunk 的 USB 传输在 ms 级，1s 是宽松上界（时钟缺失时不计时）。 */
#define FTL_CACHE_SEQ_IDLE_MS (1000U)

/* 窗口已提交、命令却迟迟不排空的兜底：超过即强制放开 LUN 门控（异常路径，留 WRN）。
 * 与 IDLE_MS 理由不同——那是「等 host 下一段」，这是「等器件命令完成」，故分开命名。 */
#define FTL_CACHE_SEQ_DRAIN_STALL_MS (1000U)

/* 后台让行的最长 host 暂停：后台被门控拒绝后置 yield_pending，host 暂不开新序列，
 * 让后台在本轮提交。后台若因别的原因一直提交不出去，超过本值即忽略让行、恢复 host，
 * 防止后台故障把 host 饿死。取 20 ms：一个 16 KiB 窗口在 940 KB/s 下约 17 ms。 */
#define FTL_CACHE_SEQ_YIELD_MAX_MS (20U)

/* 顺序流检测阈值（本仓自定，FTL 自判）：同一方向连续 LBA 累计达到 64 条
 * （32 KiB = 两个 16 KiB host 传输段）才认为进入顺序流，允许开 cache 序列。
 * 依据：本阈值是工程取值（两个 16 KiB 传输段），没有对应的论文 / 规范参数；
 * 形态上对齐 DRAM-less 分支"顺序访问按整页 / 整段落盘"的处理（DFTL，Gupta et al.,
 * ASPLOS'09；见 docs/design/storage_controller_design.md 的经典论文清单）。
 * 阈值只用于过滤单个随机请求，不是 QoS 参数。
 * ⚠ host 传输段（USB 16 KiB）只是 LBA 流的一次交付，不是本阈值的作用域——
 *   跨段连续由 seq->next_lba 延续，见 docs/design/ftl_design.md §3.16。 */
#define FTL_STREAM_MIN_LBAS (64U)

/* 顺序写序列的主动断点 = Journal 顺序页剩余空间（`jrn_seq_room()`）：页写满
 * （508 条，按整 pair 用到 504 条 = 63 pair ≈252 KiB）即 END，让这张页落盘。
 * 顺序序列占用每个 plane 的 cache 寄存器期间不能插元数据写，所以断点由页容量
 * 而非 host 命令边界决定（页可以跨多条 host 命令续用）。器件约束：END（48h）
 * 必须落在数据页上，故窗口始终按 pair 整数倍提交。 */

/* 写失败透明重试次数上限（H4：只重试一次就够覆盖「遇到一个新坏块」的情形；
 * 第二次仍失败说明介质/通道有问题，如实返回 IO_ERROR，不掩盖故障）。 */
#define FTL_WRITE_RETRY_MAX (1U)

/* 顺序读 L2P region 预取（本仓自定）：连续 2 个 host 读请求才算顺序；
 * 距 region 末尾 64 LBA（32 KiB，约两个 16 KiB 请求）时预取下一个 region。
 * 每次最多预取 1 个 region，在飞上限见 L2P_PREFETCH_MAX_INFLIGHT。 */
#define FTL_SEQ_PREFETCH_MIN_RUN    (2U)
#define FTL_SEQ_PREFETCH_AHEAD_LBAS (64U)

/*
 * 主机可见的最大 LBA = 实际可用于用户数据的块 × 每块 CW 数。
 *
 * ⚠ `FTL_IU_TOTAL`（= 全部物理 CW）是**映射表寻址空间**，不是主机容量：
 *   它把固定元数据、OP 预留、Spare、L2P 日志区全算成了可写空间。直接拿它当容量
 *   会超卖，写满后 Free/OP 池耗尽、GC 没有搬移目标，前台写入失败。
 *
 * 每 plane 的预算：
 *   FTL_METADATA_AREA_END（98 = 固定元数据 46 + **L2P 日志区 48** +
 *                          **shared metadata spare 4**）
 * + FTL_OP_BLOCKS_PER_PLANE（204，10% GC headroom）
 * + FTL_SPARE_POOL_PER_PLANE（50，坏块替换）
 * = 352 blocks/plane；数据块 = 2048 − 352 = 1696/plane，全设备 3392。
 * 当前几何：3392 × 64 × 4 = 868,352 LBA = 424 MiB（不是 512/437 MiB）。
 *
 * ⚠ L2P 日志区必须计入：它是**固定预留**的元数据区（4 MiB 表本体 × 2 份副本
 *   = 8 MiB，再留 1× 工作余量 = 12 MiB = 96 gblocks，的 L2）。
 *   若从 Free Pool 动态分配且不扣容量，日志碎片会吃掉用户容量（实测可达 1076 块 /
 *   134 MiB），导致容量未满就写失败。
 * ⚠ shared metadata spare 也必须计入：它是固定元数据分区的坏块替换池
 *   （BBT + sparing 的主流形态，见 metadata.h）。
 */

/* 全局 LBA 上限 = 每实例数据块 × 每块 CW × 设备维度数
 * （每 (ch,ce,lun) 是独立 FTL 实例；单 die 时 3392×256 = 868,352 = 424 MiB）。
 * ⚠ 这是**L2P 映射空间上限**，不是主机最终可见容量：开机按实际坏块扣减后，
 *   对外一律以 `ssd_ctl_s.host_lba_max` 为准。 */

/* 主机范围必须落在共享 cache 的键空间内（C9：同一语义只允许一套上限）。 */
_Static_assert(FTL_LBA_MAX_AT_MAX <= L2P_GLOBAL_LBA_MAX_AT_MAX, "FTL_LBA_MAX 超出共享 cache 的全局 LBA 上限");

/* ── FTL 主上下文 ── */

/* cache 流式序列状态（per instance；读 / 写各自一套，kind 区分）。
 * start/middle/end 由 FTL 的方向 + 连续性判断决定，不依赖 FC 的额外标志。 */
typedef enum FTL_CACHE_SEQ_KIND_E
{
    FTL_CACHE_SEQ_NONE = 0,
    FTL_CACHE_SEQ_READ,
    FTL_CACHE_SEQ_WRITE,
} ftl_cache_seq_kind_e;

/* cache 写序列的未确认页 FIFO 深度。
 *
 * 提交侧按 `wr.count + wr_inflight` clamp 到空闲槽（cache_wr_try 的 ring_free），
 * 正常运行下 push 不会溢出；环满仍按结构性错误处理（member_done 断言 + abort，
 *）。 */
#define FTL_CACHE_SEQ_RING (16U)

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
#if defined(FC_NAND_TEST_ENABLE)
/**
 * @brief 测试专用：驱动 padding END 提交（CACHE-15 盘满例外）。
 */
ftl_status_e ftl_cache_test_pad_submit(ftl_ctx_s *ftl);
#endif

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

/* ── cache 写序列（FTL 内部；cache_batch.c 与 ftl.c 共用）──
 *
 * 早回执：页在**自身命令完成**时入表（提交时不压 pending 下标），
 * 组的完成也不等后继 FAILC。表的用途见 ftl_cache_page_s 的注释。
 */
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

typedef struct FTL_FLAGS_S
{
    BOOL deferred_error; /* 已回执的 cache 页后来失败，下一命令报 ERROR 一次 */
    BOOL bg_suspend;     /* bbt --erase 后整体跳过 Phase 3（无清除路径，只能复位） */
    BOOL md_load_failed; /* Directory 加载失败：有 CP 时拒绝启动 */
    BOOL l2p_defer;      /* L2P region 未就绪的背压信号：drain 不计 stall */
} ftl_flags_s;

typedef struct FTL_STATISTICS_S
{
    DWORD total_writes;   /* 累计 host 编程次数（功能量：mig 判 host 写流） */
    DWORD dangling_total; /* L2P 指向 valid_count==0 的累计覆盖写次数 */
#if SSD_STATS_ENABLE
    DWORD deferred_taken; /* host 取走 deferred error 的次数（非 FUA 写迟到失败） */
#endif
} ftl_stats_s;

typedef struct FTL_FLUSH_JOB_S
{
    BOOL  active;
    DWORD start_ms;         /* 总耗时观测（FTL_FLUSH_SLOW_LOG_MS） */
    DWORD last_progress_ms; /* 最近一次观察到元数据推进的时刻（按进展续期） */
    DWORD progress;         /* 上一次的推进签名；变化即刷新 last_progress_ms */
} ftl_flush_job_s;

typedef struct FTL_TRIM_JOB_S
{
    BOOL             active;
    DWORD            lba;
    DWORD            end;
    DWORD            start_lba; /* 完成时 trim_lba 已到 end，不能反推 */
    timer_ms_t       start_ms;  /* 观测 elapsed */
    timer_deadline_s deadline;  /* 无进展等待 deadline */
} ftl_trim_job_s;

typedef struct FTL_CONTEXT_S
{
    ftl_topo_s   topo;    /* 本 FTL 绑定的 NAND 芯片路由 */
    l2p_cache_s *cache;   /* 共享：按 (ch,ce) 由 l2p_cache_workspace_get() 取 */
    pend_ctx_s   pending; /* pending / group 两张表 */
    wb_handle_s *wbuf;    /* 页打包写缓冲（装配挂载） */

    ftl_table_refs_s table_refs; /* 五张表引用（按值，可审计） */
    alc_s            alloc;      /* allocator（指针绑到 table_refs） */

    l2p_jrn_s      *journal;      /* Journal（装配挂载） */
    md_dir_cache_s *metadata_dir; /* Directory（装配挂载） */
    cp_handle_s    *cp_handle;    /* Checkpoint job（装配挂载） */

    ftl_flags_s flags; /* deferred_error / bg_suspend / md_load_failed / l2p_defer */
    ftl_stats_s stats; /* total_writes / dangling_total */

    ftl_cache_stream_s cache_stream; /* 顺序检测 / 预取 / 观测 / 序列 */

    ftl_flush_job_s flush; /* host SYNCHRONIZE CACHE / CLI fl */
    ftl_trim_job_s  trim;  /* CLI tr */

    bg_policy_s bg;
    ftl_sched_s sched;         /* request scheduler（L2P/host/后台 job + host 写 admission） */
    BYTE        l2p_flush_due; /* timer 置位，bg_policy 消费 */

    mig_handle_s *mig_handle;           /* GC / WL / predict（装配挂载） */
    rr_handle_s   read_recovery_handle; /* 异步恢复 job */
    BYTE         *rr_page_scratch;      /* 恢复整页暂存（装配挂载） */
    BYTE          write_retry_buf[FTL_LBA_PER_PAIR * FTL_IU_BYTES];
} ftl_ctx_s;

/**
 * @brief FTL 实例的 workspace 引用（内部装配结构）
 *
 * 各块由**拥有它的模块**定义并选定内存域（allocator→SRAM12、cp→SRAM3、
 * md→SRAM4、journal/wbuf→DTCM、migrate/rr→AXI）；本结构只收引用（指针）。
 * 由 ftl_instance_init 按 die 装配，ftl_init 只绑定指针；
 * 生命周期覆盖全程（完成表的 ctx 会保存这些地址）。
 */
typedef struct FTL_STORAGE_S
{
    const ftl_table_refs_s *table_refs;
    md_dir_cache_s         *metadata_dir;
    l2p_jrn_s              *journal;
    cp_handle_s            *cp_handle;
    mig_handle_s           *migrate;         /* 装配挂载：指针（不减少总 RAM） */
    wb_handle_s            *wbuf;            /* 装配挂载 */
    BYTE                   *rr_page_scratch; /* 装配挂载：FTL_READ_USER_SIZE */
} ftl_storage_s;

/* ── 接口 ──
 * 公开的 boot/topology 入口（ftl_global_init / ftl_topology_build）声明在
 * ftl_interface.h；本头只列 FTL 内部装配接口。 */

/**
 * @brief 初始化 FTL 子系统
 *
 * 描述符只持有指针；storage 引用的 workspace 由各模块自持，
 * 装配入口 ftl_instance_init 按 die 取引用后传入。
 * L2P cache 是**设备级共享对象**（同一 (ch,ce) 一份），由 l2p_device_init
 * 初始化一次后传入；本函数只绑定指针，不 init、不 set_md_dir。
 *
 * ⚠ storage 指向的数组可能在 NOLOAD 段（`.sram*_data`）：启动代码不清零，
 *   本函数会显式 memset 四个数组。
 *
 * @param ftl     FTL 描述符
 * @param topo    本实例绑定的 NAND 芯片路由
 * @param cache   本实例所属 (ch,ce) 的共享 L2P cache（非空，设备级已初始化）
 * @param storage storage 数组/挂载块的存储（非空，所有指针都非空）
 * @return FTL_OK    成功
 *         FTL_ERROR 元数据候选池构建失败（拒绝启动）
 */
ftl_status_e ftl_init(ftl_ctx_s *ftl, ftl_topo_s topo, l2p_cache_s *cache, const ftl_storage_s *storage);

/**
 * @brief debug-only 初始化契约自检（release 下空实现）
 *
 * 在 ftl_init 与设备级 owner 登记完成后调用一次。只证明初始化后**不可变**的绑定：
 * 句柄、storage 数组、端口回调、topology 副本、PBA 布局、bg 任务表、owner/route 身份。
 * 不检查运行期可变状态（phase / 计数 / 池成员），也不替代每次调用的入参断言。
 *
 * @param ftl FTL 上下文（非 NULL；由 ssd_ftl_boot 在 ftl_init 成功后调用）
 */
void ftl_contract_audit(const ftl_ctx_s *ftl);

/**
 * @brief 打印 FTL 各模块的关键现场（allocator / pending / cache seq / CP / MIG /
 *        metadata / Journal / wbuf）。
 *
 * @param module NULL / "all" = 全部；否则只打印指定模块：
 *               alloc / pend / seq / cache / jrn / cp / mig / md / wb。
 * @return true = 参数合法并已输出；false = 未知模块。
 *
 * @note 只读、标量快照，供 CLI `ftl_ctx` 与断言终端路径使用；不调用会断言的池扫描。
 *       断言路径由 `ftl_assert_context()` 弱钩子调用（见 common/assertion.c）。
 */
BOOL ftl_debug_dump(const char *module);

/**
 * @brief 断言终端的 FTL 现场钩子（强符号覆盖 assertion.c 的弱默认）。
 */
void ftl_assert_context(void);

/* ── 内部辅助（FTL 层内共用，不属于 HIL 接口）── */

/**
 * @brief 推进一个 host 完成组的待下发子操作
 *
 * @param ftl FTL 实例
 * @param gi  pending 组下标
 */
void ftl_host_group_drain(ftl_ctx_s *ftl, WORD gi);

/**
 * @brief 按当前实例状态构造一次 Checkpoint 触发快照。
 *
 * @param ftl    实例（读 total_writes 与 journal 的 seq / 水位 / 紧急标志）
 * @param reason 触发原因（原样写入 trigger.reason）
 * @param force  强制请求标志（显式 CP 传 true；后台适配器传 false）
 *
 * @return 触发快照（now_ms 取调用时刻）
 *
 * @note 后台适配器（bg_policy）与显式 CP（ftl_flush_kick_cp）共用——同一组字段
 *       只在单处装配，`cp_trigger_s` 加字段时不会漏改某一处。
 */
cp_trigger_s ftl_cp_trigger_build(const ftl_ctx_s *ftl, cp_reason_e reason, BOOL force);

#endif /* FTL_SERVICE_H */
#ifndef FTL_SERVICE_H
#define FTL_SERVICE_H

/*
 * ftl_service — 主循环调度器
 *
 * 每轮 superloop 两段（**泵与实例分开**）：
 *   ① ftl_service_pump()   —— **实例无关**：[pump] 推进 NAND + [pump] 完成泵。
 *                              全局的 NAND 引擎与完成表，每轮**只调一次**。
 *   ② ftl_service(ftl)     —— **单实例**：元数据候选池重算 → 写确认结算 →
 *                              cache 序列确认 → host 补发 → 序列收尾 →
 *                              wbuf 空闲 flush → 后台调度（逐步清单见 ftl_service 契约）。
 *                              对**每一个**实例都要调（多 LUN 下不能只跑 [0][0][0]）。
 *
 * 三级模式切换依据未决命令数（pending + 组），非"本轮提交了几条 NAND"。
 */

#include "allocator.h"    /* ftl_table_refs_t, super_block_t */
#include "bg_scheduler.h" /* 工作量配额后台调度器（ftl_ctx_t 含调度器） */
#include "cache_batch.h"  /* cache read/write 批次的常量与对 FTL 的尝试入口 */
#include "checkpoint.h"
#include "ftl_type.h" /* ftl_io_req_t */
#include "gc.h"
#include "journal.h"
#include "l2p_cache.h"
#include "metadata.h"
#include "migrate.h"
#include "pending.h"
#include "read_recovery.h"
#include "ssd_types.h"
#include "timer.h"
#include "wbuf.h"

#include <stdbool.h>
#include <stdint.h>

/* ── 宏 ── */

#define NAND_SUBMIT_BUDGET  (4U)     /* 每轮 fc_service 最多跑 4 次（首轮抽干完成，余为填充轮次） */
#define CP_WRITE_INTERVAL   (1024U)  /* 写入量触发间隔 */
#define CP_TIME_INTERVAL_MS (10000U) /* 定时触发间隔（ms）*/

#define FTL_READ_USER_SIZE (2048U) /* 一次读返回的用户数据大小 = 4 CW × 512B */

/* 异步 FLUSH job 的耗时告警阈值（RISK-USB-06）：从 `ftl_flush_begin` 到完成
 * 超过该值即打一行 WRN。用于观察主机看到的 flush 延迟；job 本身不阻塞主循环。 */
#define FTL_FLUSH_SLOW_LOG_MS (10U)

/* 异步 FLUSH job 的墙钟上限。超时按 best-effort 完成（状态仍报 OK）并打 WRN。
 * 旧同步路径由 `L2P_SYNC_GUARD_MAX` 隐式限定循环次数；改成异步后必须显式给上限，
 * 否则元数据侧真卡死时 host FLUSH 永不完成（SCSI 超时 → BOT reset recovery）。
 * 1 s 远小于主机 SCSI/BOT 超时，又能覆盖正常的满盘 flush。 */
#define FTL_FLUSH_TIMEOUT_MS (1000U)

/* 一个组的子操作连续遇 FTL_AGAIN（瞬态背压）而**无任何进展**的轮数上限。
 * 超过即判真卡死：弃未下发子操作、组报 IO_ERROR（已下发的照常完成）。
 * 预算取 8——正常背压在下轮 ftl_service 抽干完成后即恢复，连续 8 轮无进展属异常。
 * ⚠ 「有确定完成时刻的后台进展」不计入：L2P region 加载在飞、搬移 job（GC/WL）
 *   在飞时清零/不累加——否则一次数百 ms 的正常回收会被 8 轮预算判成 IO_ERROR
 *   （RISK-GC-09）。 */
#define FTL_DRAIN_STALL_MAX (8U)

/* cache 序列跨 chunk 等待下一 chunk 的上限：超过即关闭序列状态，避免门控永久
 * 挡住该 LUN。16 KiB chunk 的 USB 传输在 ms 级，1s 是宽松上界（时钟缺失时不计时）。 */
#define FTL_CACHE_SEQ_IDLE_MS (1000U)

/* 窗口已提交、命令却迟迟不排空的兜底：超过即强制放开 LUN 门控（异常路径，留 WRN）。
 * 与 IDLE_MS 理由不同——那是「等 host 下一段」，这是「等器件命令完成」，故分开命名。 */
#define FTL_CACHE_SEQ_DRAIN_STALL_MS (1000U)

/* 顺序流检测阈值（本仓自定，FTL 自判）：同一方向连续 LBA 累计达到 64 条
 * （32 KiB = 两个 16 KiB host 传输段）才认为进入顺序流，允许开 cache 序列。
 * 依据：本阈值是工程取值（两个 16 KiB 传输段），没有对应的论文 / 规范参数；
 * 形态上对齐 DRAM-less 分支"顺序访问按整页 / 整段落盘"的处理（DFTL，Gupta et al.,
 * ASPLOS'09；见 docs/design/storage_controller_learning.md 的经典论文清单）。
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
 *   = 8 MiB，再留 1× 工作余量 = 12 MiB = 96 gblocks，RISK-BOOT-18 的 L2）。
 *   若从 Free Pool 动态分配且不扣容量，日志碎片会吃掉用户容量（实测可达 1076 块 /
 *   134 MiB），导致容量未满就写失败（RISK-MD-07 / RISK-CAP-02）。
 * ⚠ shared metadata spare 也必须计入：它是固定元数据分区的坏块替换池
 *   （BBT + sparing 的主流形态，见 metadata.h）。
 */
#define FTL_HOST_DATA_BLOCKS_PER_INSTANCE (FTL_BLOCK_TOTAL - ((FTL_METADATA_AREA_END + FTL_OP_BLOCKS_PER_PLANE + FTL_SPARE_POOL_PER_PLANE) * PLANE_NUM))

/* 全局 LBA 上限 = 每实例数据块 × 每块 CW × 设备维度数
 * （每 (ch,ce,lun) 是独立 FTL 实例；单 die 时 3392×256 = 868,352 = 424 MiB）。
 * ⚠ 这是**L2P 映射空间上限**，不是主机最终可见容量：开机按实际坏块扣减后，
 *   对外一律以 `ssd_controller_t.host_lba_max` 为准（RISK-CAP-03）。 */
#define FTL_LBA_MAX ((QWORD)FTL_HOST_DATA_BLOCKS_PER_INSTANCE * PAGES_PER_BLOCK * CW_NUM * DIE_MAX)

/* 静态上限版本：只用于编译期断言（运行期用 FTL_LBA_MAX）。 */
#define FTL_LBA_MAX_AT_MAX ((QWORD)FTL_HOST_DATA_BLOCKS_PER_INSTANCE * PAGES_PER_BLOCK * CW_NUM * FTL_DIE_MAX)

/* 主机可见容量（字节）：给日志/文档用，和 USB MSC 的 READ CAPACITY 同源。 */
#define FTL_HOST_CAPACITY_BYTES ((QWORD)FTL_LBA_MAX * FTL_IU_BYTES)

/* 主机范围必须落在共享 cache 的键空间内（C9：同一语义只允许一套上限）。 */
_Static_assert(FTL_LBA_MAX_AT_MAX <= L2P_GLOBAL_LBA_MAX_AT_MAX, "FTL_LBA_MAX 超出共享 cache 的全局 LBA 上限");

/* ── FTL 主上下文 ── */

/* cache 流式序列状态（per instance；读 / 写各自一套，kind 区分）。
 * start/middle/end 由 FTL 的方向 + 连续性判断决定，不依赖 FC 的额外标志。 */
typedef enum
{
    FTL_CACHE_SEQ_NONE = 0,
    FTL_CACHE_SEQ_READ,
    FTL_CACHE_SEQ_WRITE,
} ftl_cache_seq_kind_e;

/* cache 写序列的未确认页 FIFO 深度：上界 = 两个在飞 chunk（2×4 页）+ 前一 chunk
 * 的尾页 + 余量。环满属结构性错误（在飞页数被窗口/credit 夹住，达不到 16）⇒
 * ftl_cache_wr_member_done 断言（RISK-CACHE-32 记录了 release 下的残余风险）。 */
#define FTL_CACHE_SEQ_RING (16U)

/* 写序列的「已编程页表」（读序列不用）。
 *
 * 缓存程序是流水线：第 i 页的编程结论由**第 i+1 条命令**的 FAIL_CACHE 给出，
 * END（10h）的 FAIL 给出最后一页的结论。早回执（RISK-CACHE-27）把**组完成**从
 * 「等到 FAILC」提前到「本页命令完成」，因此本表要在窗口内记住每页的落点：
 *   · 读路径靠它把 LBA 指到刚编程的页（此时 L2P 可能还没确认）；
 *   · 后续 FAILC 判失败时靠它撤销（RISK-CACHE-28）。
 * 环按**提交顺序**；head 是最老、尚未被弹出（= 尚未「已证明且已确认」）的页。
 * 结论的落点不是 head，而是独立的结论游标——确认可能背压滞后，head 会停在
 * 「已证明、等确认」的页上（见 ftl_cache_wr_t.pend_idx）。 */
typedef enum
{
    FTL_CACHE_PAGE_PROGRAMMED = 0, /* 已编程；待确认（Journal→L2P→BST） */
    FTL_CACHE_PAGE_CONFIRMED,      /* 已进 L2P；等后继证明 */
    FTL_CACHE_PAGE_FAILED,         /* 后继 FAILC 判失败；等撤销（L2P 失效 + region 降级） */
} ftl_cache_page_state_e;

typedef struct
{
    DWORD lba;       /* 页基址 */
    DWORD pba0;      /* plane0 的真实 PBA（Policy B 下两 plane 块号不同，不可用
                      * pba_with_plane 从 plane0 推导） */
    DWORD pba1;      /* plane1 的真实 PBA */
    BYTE  cw_bitmap; /* 本页覆盖的 CW（通常 FTL_CACHE_FULL_CW） */
    BYTE  state;     /* ftl_cache_page_state_e */
    bool  proven;    /* 后继命令的 FAIL 结论已到、且成功（END 由自身 STATUS 证明） */
} ftl_cache_page_t;

typedef struct
{
    ftl_cache_page_t page[FTL_CACHE_SEQ_RING]; /* 已编程页（提交顺序） */
    BYTE             head;                     /* 队头环形下标（确认/弹出的起点） */
    BYTE             count;                    /* 已编程未证明的页数 */

    /* 结论游标：**下一条 cache 命令的 FAIL_CACHE 结论落在哪一页**。
     *
     * 不能用 head 代替：结论按页序到达，而确认（Journal→L2P）可能背压滞后，
     * head 会停在「已证明、等确认」的页上；此时再用 head 承接新结论，会让该页
     * 被重复证明、而真正待证的页永远等不到结论（环排不空 ⇒ flush 超时）。
     * 环形下标是绝对下标（pop 只动 head/count，不搬元素），所以不会因弹出而漂移；
     * 该页未证明前不可能被弹出。 */
    BYTE pend_idx;   /* 待结论页的 page[] 绝对下标 */
    bool pend_valid; /* false = 没有待结论页（新序列首条 / END 页自身已证明） */
} ftl_cache_wr_t;

typedef struct
{
    ftl_cache_seq_kind_e kind;
    BYTE                 gen; /* 写序列代号：**中止时**递增。cache 成员的完成带自己的
                               * 代号，与当前代号不一致 ⇒ 迟到完成（序列已死），只结算
                               * 组、不碰页表。正常开新序列不递增：迟到的 END 页
                               * （proven）与新页表关系明确，递增反而误判合法完成。 */
    BYTE wr_inflight;         /* 已下发、尚未完成的 cache 写成员数（提交时 +，完成时 -）。
                               * 页在**完成时**才进页表，只看 wr.count 会漏掉仍在编程
                               * 的页：flush 屏障与「开新序列」都必须等它归零
                               * （RISK-CACHE-29）。 */

    /* ⚠ 偏离 H32（P5 留痕）：active / draining / closing 是**同一设备管线**的
     * 互斥状态，这里沿用既有 bool 组而不是互斥枚举（改动面大且需同步全部判定点）；
     * 互斥性由 ftl_cache_seq_release / close_step / pad 回调三处维护。
     * 后续重构时收敛成一个状态枚举。 */

    bool active;              /* true = 设备侧序列打开（START 已发、END 未发） */
    bool draining;            /* true = 读序列已放弃但命令仍在飞：门控继续关着，
                               * 直到所有者组的在飞子操作归零（RISK-CACHE-14）。 */
    bool owner_submit;        /* 序列所有者正在提交窗口（门控旁路） */
    bool break_requested;     /* 非所有者提交被门控拒绝 ⇒ 请 service 收尾序列 */
    bool close_requested;     /* 写序列需要**有序收尾**（下一段 LBA 不连续 / 空闲 /
                               * flush / 前台等待）：service 用 padding END 落下
                               * 一个收尾页，把最后一页的 FAIL_CACHE 结算掉。 */
    bool closing;             /* padding END 已在飞：门控继续关，新的窗口等待 */
    bool own_page0;           /* 当前 hot SB 的 page0 由**本序列**写入：允许它在确认前
                               * 继续写 page>0（有序管线保证 page0 失败会中止其后所有页，
                               * 见 allocator.h 的 alc_super_open_get_room_seq）。
                               * 序列开始即清、分配 page0 时置位；跨 block 续写时由新块
                               * 的分配重新置位（新 SB 的 page0 同样由本序列写）。 */
    WORD  owner_gi;           /* 最近持有序列的组（窗口记账 / 门控归因） */
    DWORD next_lba;           /* 续序列的判据：下一个 host 数据段必须从这里开始 */
    DWORD idle_ms;            /* 所有者组已释放/已发完后等待下一段的起点 */
    DWORD run_pairs;          /* 读序列本段已提交的 pair 数（跨 block 续接的让步上限，
                               * 见 cache_batch.h 的 FTL_CACHE_READ_MAX_RUN_PAIRS） */
    DWORD pad_pba[PLANE_NUM]; /* padding END 页的逐 plane PBA。Policy B 下两 plane 的块号
                               * 不同 ⇒ 回调记账必须用真实地址，不能用 pba_with_plane()
                               * 从 plane0 推导（那样会漏记 plane1 的 page0 屏障）。 */
    ftl_cache_wr_t wr;        /* 写序列的逐页确认 FIFO */
} ftl_cache_seq_t;

/* 顺序流检测器（per instance × per direction）——FTL 自己判断顺序读写的状态。
 *
 * 判据只有一条：同一方向相邻 host 请求**首尾相接**（本请求起始 LBA == 上一请求的
 * 结束 LBA）。连续长度累计达到 FTL_STREAM_MIN_LBAS 即进入顺序流；LBA 空档或换向
 * 即复位（flush / trim 不打断检测——它们不改变后续写的 LBA 连续性，新起点由判据
 * 自然处理）。它只决定 cache 序列**能否开启**；序列一旦打开，延续由
 * cache_stream.seq.next_lba 判定（与 host 传输段边界无关）。 */
typedef struct
{
    DWORD last_end; /* 上一请求的结束 LBA（exclusive）；FTL_LBA_INVALID = 无上一请求 */
    DWORD run;      /* 连续 LBA 累计（封顶在阈值附近，只用于判 ≥ FTL_STREAM_MIN_LBAS） */
    bool  active;   /* true = 连续长度已达阈值（顺序流） */
} ftl_stream_det_t;

/* cache 流式模块状态（per instance）——顺序检测 / 读预取 / 观测计数 / 设备侧序列。
 *
 * 两个检测器**坐标不同**，不要合并：
 *   · rd / wr（ftl_stream_det_t）：按 **LBA 数**累计，flush / trim 不打断；
 *     判据是 cache 序列**能否开启**（FTL_STREAM_MIN_LBAS）。
 *   · prefetch_*：按 **请求次数**累计，flush / trim 时复位；
 *     只用于 L2P region 预取（FTL_SEQ_PREFETCH_MIN_RUN）。 */
typedef struct
{
    /* 设备侧 cache 读写序列（单 LUN 同时一条；跨窗口 / 跨 chunk 保持）。 */
    ftl_cache_seq_t seq;

    /* 顺序流检测：cache 序列的开启判据。 */
    ftl_stream_det_t rd; /* 读方向 */
    ftl_stream_det_t wr; /* 写方向 */

    /* 顺序读检测：只用于 L2P region 预取。 */
    DWORD prefetch_last_end;   /* 上一读请求末 LBA（exclusive） */
    DWORD prefetch_run;        /* 连续读请求计数 */
    DWORD prefetch_run_max;    /* 本次开机最大连续读长度（观测） */
    DWORD prefetch_generation; /* 每次检测更新/重置递增，防 on_complete 重入覆盖 */
    DWORD prefetch_target;     /* 本次检测算出的目标 region */
    bool  prefetch_valid;      /* target 有效且尚未执行 */

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
} ftl_cache_stream_t;

/**
 * @brief 只清 RAM：关闭 cache 序列状态（不发任何设备命令）。
 *
 * @param ftl 实例。
 *
 * @note 设备侧未收 LAST 的缓存序列由随后的新命令终止（RISK-IF-09，待上板确认）。
 *       若调用时仍有该序列的命令在飞，置 `draining` 让门控继续关到排空——否则别的组会在
 *       31h/3Fh 管线半途插命令（RISK-CACHE-14）。
 */
void ftl_cache_seq_release(ftl_ctx_t *ftl);

/* ── cache 写序列（FTL 内部；cache_batch.c 与 ftl.c 共用）──
 *
 * 早回执（RISK-CACHE-27）：页在**自身命令完成**时入表（提交时不压 pending 下标），
 * 组的完成也不等后继 FAILC。表的用途见 ftl_cache_page_t 的注释。
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
bool ftl_cache_wr_lookup(const ftl_ctx_t *ftl, DWORD lba, DWORD *out_pba);

/**
 * @brief 该 LBA 是否落在**已判失败、尚未撤销**的 cache 页上。
 *
 * @param ftl 实例。
 * @param lba 全局 host LBA。
 *
 * @return true = 读该 LBA 必须报 IO_ERROR（撤销完成前 L2P 可能仍指着失败页）。
 */
bool ftl_cache_wr_failed_covers(const ftl_ctx_t *ftl, DWORD lba);

typedef struct
{
    bool deferred_error; /* RISK-CACHE-27：已回执的 cache 页后来失败，下一命令报 ERROR 一次 */
    bool bg_suspend;     /* bbt --erase 后整体跳过 Phase 3（无清除路径，只能复位） */
    bool md_load_failed; /* Directory 加载失败：有 CP 时拒绝启动（RISK-BOOT-09） */
    bool l2p_defer;      /* L2P region 未就绪的背压信号：drain 不计 stall */
} ftl_flags_t;

typedef struct
{
    DWORD total_writes;   /* 累计 host 编程次数 */
    DWORD dangling_total; /* L2P 指向 valid_count==0 的累计覆盖写次数 */
} ftl_stats_t;

typedef struct
{
    bool  active;
    DWORD start_ms; /* 耗时观测（FTL_FLUSH_SLOW_LOG_MS） */
} ftl_flush_job_t;

typedef struct
{
    bool             active;
    DWORD            lba;
    DWORD            end;
    DWORD            start_lba; /* 完成时 trim_lba 已到 end，不能反推 */
    timer_time_t     start_ms;  /* 观测 elapsed */
    timer_deadline_t deadline;  /* 无进展等待 deadline */
} ftl_trim_job_t;

struct ftl_context
{
    ftl_topo_t    topo;    /* 本 FTL 绑定的 NAND 芯片路由 */
    l2p_cache_t  *cache;   /* 共享：g_l2p_cache[ch][ce] */
    pending_ctx_t pending; /* pending / group 两张表 */
    wb_handle_t  *wbuf;    /* 页打包写缓冲（装配挂载） */

    ftl_table_refs_t table_refs; /* 五张表引用（按值，可审计） */
    alc_t            alloc;      /* allocator（指针绑到 table_refs） */

    l2p_jrn_t      *journal;      /* Journal（装配挂载） */
    md_dir_cache_t *metadata_dir; /* Directory（装配挂载） */
    cp_handle_t    *cp_handle;    /* Checkpoint job（装配挂载） */

    ftl_flags_t flags; /* deferred_error / bg_suspend / md_load_failed / l2p_defer */
    ftl_stats_t stats; /* total_writes / dangling_total */

    ftl_cache_stream_t cache_stream; /* 顺序检测 / 预取 / 观测 / 序列 */

    ftl_flush_job_t flush; /* host SYNCHRONIZE CACHE / CLI fl */
    ftl_trim_job_t  trim;  /* CLI tr */

    bg_scheduler_t bg;
    BYTE           l2p_flush_due; /* timer 置位，bg_scheduler 消费 */

    mig_handle_t *mig_handle;           /* GC / WL / predict（装配挂载） */
    rr_handle_t   read_recovery_handle; /* 异步恢复 job */
    BYTE         *rr_page_scratch;      /* 恢复整页暂存（装配挂载） */
    BYTE          write_retry_buf[FTL_LBA_PER_PAIR * FTL_IU_BYTES];
};

/**
 * @brief 描述符的外部存储
 *
 * storage 数组由 HIL 提供并决定落哪个域（main.c：五张表与 md→各自 section、
 * journal/cp_handle→SRAM3；migrate/wbuf/rr_page_scratch→AXI .bss）。
 * 五张表各自独立定义，本结构只收它们的引用（指针），不约束表本体的放置。
 * ftl_init 只绑定指针；数组定义在 main.c，
 * 生命周期覆盖全程（完成表的 ctx 会保存这些地址）。
 */
typedef struct
{
    const ftl_table_refs_t *table_refs;
    md_dir_cache_t         *metadata_dir;
    l2p_jrn_t              *journal;
    cp_handle_t            *cp_handle;
    mig_handle_t           *migrate;         /* 装配挂载：指针（不减少总 RAM） */
    wb_handle_t            *wbuf;            /* 装配挂载 */
    BYTE                   *rr_page_scratch; /* 装配挂载：FTL_READ_USER_SIZE */
} ftl_storage_t;

/* ── 接口 ── */

/**
 * @brief 复位 FTL 文件级全局静态（只调用一次）
 *
 * `ftl.o` 的 `.bss` 整文件落 DTCM（NOLOAD），`s_dangling[]`、日志节流等
 * 文件级静态不被启动代码清零；必须在**所有实例 ftl_init 之前**复位一次。
 * 不能放在逐实例 ftl_init 里：多实例/运行期 re-init 会清掉已运行实例的共享状态。
 */
void ftl_global_init(void);

/**
 * @brief 由上电扫描结果构建紧凑 die 表（纯 RAM，不碰硬件）
 *
 * targets 由 nand_scan_targets() 在 armed 之前填好
 * （present / lun_count / lun_present[]）。
 * 只给实际存在且 78h 探测有响应的 LUN 分配紧凑 die 号；
 * g_chcelun_table 与 g_die_table 互为逆映射。
 *
 * @param targets 板级物理拓扑（CH_MAX × CE_MAX）
 * @return true = 至少一个 die 且往返自检通过；false = 无 die / 越界 / 自检失败
 */
bool ftl_topology_build(const nand_target_info_t targets[CH_MAX][CE_MAX]);

/**
 * @brief 初始化 FTL 子系统
 *
 * 描述符只持有指针；storage 数组/挂载块由调用者提供。
 * L2P cache 是**设备级共享对象**（同一 (ch,ce) 一份），由调用者初始化一次后
 * 传入；本函数只绑定指针，不 init、不 set_md_dir。
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
ftl_status_e ftl_init(ftl_ctx_t *ftl, ftl_topo_t topo, l2p_cache_t *cache, const ftl_storage_t *storage);

/**
 * @brief debug-only 初始化契约自检（release 下空实现）
 *
 * 在 ftl_init 与设备级 owner 登记完成后调用一次。只证明初始化后**不可变**的绑定：
 * 句柄、storage 数组、端口回调、topology 副本、PBA 布局、bg 任务表、owner/route 身份。
 * 不检查运行期可变状态（phase / 计数 / 池成员），也不替代每次调用的入参断言。
 *
 * @param ftl FTL 上下文（非 NULL；由 ssd_ftl_boot 在 ftl_init 成功后调用）
 */
void ftl_contract_audit(const ftl_ctx_t *ftl);

/**
 * @brief 实例无关的每轮泵（superloop 每轮调一次）
 *
 * 步骤（按执行序）：
 *   ① `[pump] 推进 NAND` —— fc_service() ≤ NAND_SUBMIT_BUDGET 次，首轮即抽干完成
 *   ② `[pump] 完成泵`    —— ftl_op_drain()：从 CQ 取完成，按命令 ch/ce/lun 路由回实例
 *
 * ⚠ 必须在所有 ftl_service() 之前调，且**每轮只调一次**——NAND 引擎与完成表都是
 *   全局单份，per-instance 调用会把 fc_service 预算重复消耗 N 遍。
 */
void ftl_service_pump(void);

/**
 * @brief 单个 FTL 实例的服务（对**每个**实例都要调）
 *
 * **步骤清单（唯一定义；其它模块一律用这些名字引用，不用编号）**，按执行序：
 *
 * | 步骤 | 做什么 | 实现 |
 * |---|---|---|
 * | 元数据候选池重算 | 元数据 BAD 后重算候选池（纯 RAM；等 metadata job 空闲） | `md_pool_service` |
 * | 写确认结算 | 推进待入账的写确认（**必须先于 host 补发**：待入账意味着该 LBA 还是旧映射，先补发会读到旧物理页） | `ftl_confirm_service` |
 * | cache 序列确认 | 按序确认已编程页（Journal→L2P→BST）、撤销失败页、弹出已证明页 | `ftl_cache_wr_confirm_step` |
 * | host 补发 | 逐组下发待补发的子操作（host IO 优先于后台） | `ftl_drain_group` |
 * | 序列收尾 | 孤儿序列超时关闭（读序列放弃）+ 写序列 padding END 有序收尾 | `ftl_cache_seq_idle_guard` / `_close_step` |
 * | wbuf 空闲 flush | 确认再无写组会 append 时，把页打包缓冲编程掉（否则这些写永不完成） | `wb_handle(MAYBE_FLUSH_IDLE)` |
 * | 后台调度 | 选档 → 任务表 → 预算（`bg_scheduler_service`） | `bg_scheduler_service` |
 *
 * ⚠ 调用方须先调 ftl_service_pump()。多实例（多 LUN）下漏调某实例 ⇒ 它的
 *   journal / checkpoint / bbt / GC 永不推进。
 *
 * @param ftl  FTL 上下文（**非 NULL**；实例绑定由 ftl_contract_audit 在 boot 期证明）
 */
void ftl_service(ftl_ctx_t *ftl);

/**
 * @brief 非阻塞提交读请求（长度任意）
 *
 * 请求带**长度**（req->count 个连续 LBA），FTL 自行决定单/双 plane 拆分：
 *   ① 建完成组（一条 host 请求 = 一个组）
 *   ② 逐子操作下发（每个子操作 2 页 Multi-Plane，末条奇数页时单 plane）
 *   ③ 未映射子操作同步返回全零；余下子操作遇资源背压留待 ftl_service 的 host 补发步
 *   ④ 数据在 ftl_op_drain 中路由回 buffer
 *
 * ⚠ 一次调用的完成通知（complete_flag/status_out/on_complete）**只在全部子操作
 *   完成后触发一次**。返回 FTL_OK 仅表示已受理（含部分子操作排队），不代表已完成。
 *
 * @param ftl  FTL 上下文
 * @param req  IO 请求描述（lba/buffer/count/complete_flag/status_out/on_complete/ctx）
 * @return FTL_OK    已受理
 *         FTL_ERROR 立即失败（参数非法 / LBA 越界 / 组槽满）
 */
ftl_status_e ftl_submit_read(ftl_ctx_t *ftl, const ftl_io_req_t *req);

/**
 * @brief 非阻塞提交写请求（长度任意）
 *
 * 与 ftl_submit_read 对称：FTL 按 count 拆子操作，每子操作分配新 PBA（**逐 plane
 * 独立选块**——Policy B：SB 两 plane 的块号各自独立）+ 建 desc 链 + 异步写 NAND。
 * Journal + L2P 在 completion handler 中确认 NAND PROGRAM 成功后执行。
 *
 * @param ftl  FTL 上下文
 * @param req  IO 请求描述（同 ftl_submit_read）
 * @return FTL_OK    已受理
 *         FTL_ERROR 立即失败
 */
ftl_status_e ftl_submit_write(ftl_ctx_t *ftl, const ftl_io_req_t *req);

/**
 * @brief 取走并清除「延后错误」标志（volatile write cache 语义）。
 *
 * @param ftl 实例。
 * @return true = 上一次已回执的写后来被判失败；调用者应在**下一条**能承载状态的
 *         host 命令上上报一次错误（控制器 `usb_msc_take_deferred_error` 消费）。
 *
 * @note 早回执后组在页编程完成时就报 OK，FAILC 要晚一条命令才到；这条通道就是把
 *       那个迟到的结论交回给控制器的唯一出口（RISK-USBC-01 的 FTL 侧对应物）。
 */
bool ftl_take_deferred_error(ftl_ctx_t *ftl);

/**
 * @brief 异步 FLUSH job（host SYNCHRONIZE CACHE / CLI `fl`）
 *
 * 主流形态：FLUSH 是**排队命令**，提交即返回，完成由 HIL 的完成通道
 * （本仓 `complete_flag` / `status_out`，见 docs/archive/ftl_host_interface.md A.1）
 * 通知；不允许在 host 回调里自旋——那段时间主循环无法服务 USB 事件（RISK-USB-06）。
 *
 * 用法：HIL 提交 FLUSH 时调 begin（幂等）；此后每轮主循环调 step，返回 true = 完成。
 * 完成 = wbuf 无未编程数据 + 本 owner 的 L2P 无 DIRTY/FLUSHING/LOADING +
 *        Journal 无在飞 job 且无未落盘条目 + 无待入账中的 WRITE_CONFIRM。
 * Journal 持久性失败（best-effort）直接结束并打 WRN——与旧的同步语义一致。
 *
 * @param ftl  FTL 上下文（**前置条件**；实例绑定由 ftl_contract_audit 在 boot 期证明）
 */
void ftl_flush_begin(ftl_ctx_t *ftl);

/**
 * @brief 推进 FLUSH job（每轮主循环调用一次）
 *
 * @param ftl FTL 上下文（**前置条件**；实例绑定由 ftl_contract_audit 在 boot 期证明）
 * @return true  完成（含无在飞 job 的幂等完成）
 *         false 继续下一轮
 */
bool ftl_flush_step(ftl_ctx_t *ftl);

/**
 * @brief 放弃当前 FLUSH job（HIL 撤销回执时调用，如 host 复位）
 *
 * 只清本实例的 job 标志：在飞的数据操作由各自模块的异步路径自行收敛，无需回滚。
 * 下一次 begin 会以新的起点重新开始。
 *
 * @param ftl FTL 上下文
 */
void ftl_flush_cancel(ftl_ctx_t *ftl);

/**
 * @brief 异步取消 [lba, lba+count) 的 LBA 映射
 *
 * begin 只登记范围；step 每次异步推进一小段（L2P 未就绪 / Journal 容量不足时
 * 返 FTL_AGAIN，不产生半状态）。完成后 step 返 FTL_OK 并清 active。
 * 数据面 host 写确认不走这里；TRIM 只由 CLI 发起（控制面）。
 *
 * @param ftl   FTL 上下文（**前置条件**；实例绑定由 ftl_contract_audit 在 boot 期证明）
 * @param lba   起始逻辑地址
 * @param count 连续 LBA 数（>0；越界返 FTL_ERROR）
 * @return FTL_OK    已登记范围
 *         FTL_AGAIN 已有 TRIM 在飞
 *         FTL_ERROR 参数非法（count == 0 或越界）
 */
ftl_status_e ftl_trim_begin(ftl_ctx_t *ftl, DWORD lba, DWORD count);

/**
 * @brief 推进 TRIM job（每次最多处理 FTL_TRIM_STEP_LBAS 条 LBA）
 *
 * @param ftl FTL 上下文（**前置条件**；实例绑定由 ftl_contract_audit 在 boot 期证明）
 * @return FTL_OK    已完成（或本就没有在飞 TRIM）
 *         FTL_AGAIN 仍有剩余，下一轮继续
 *         FTL_ERROR 等待超时或 L2P 读失败，已中止 TRIM
 */
ftl_status_e ftl_trim_step(ftl_ctx_t *ftl);

/* ── 内部辅助（FTL 层内共用，不属于 HIL 接口）── */

/**
 * @brief 按当前实例状态构造一次 Checkpoint 触发快照。
 *
 * @param ftl    实例（读 total_writes 与 journal 的 seq / 水位 / 紧急标志）
 * @param reason 触发原因（原样写入 trigger.reason）
 * @param force  强制请求标志（显式 CP 传 true；后台适配器传 false）
 *
 * @return 触发快照（now_ms 取调用时刻）
 *
 * @note 后台适配器（bg_scheduler）与显式 CP（ftl_flush_kick_cp）共用——同一组字段
 *       只在单处装配，`cp_trigger_t` 加字段时不会漏改某一处。
 */
cp_trigger_t ftl_cp_trigger_build(const ftl_ctx_t *ftl, cp_reason_e reason, bool force);

#endif /* FTL_SERVICE_H */
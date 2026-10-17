#ifndef FTL_MIGRATE_H
#define FTL_MIGRATE_H

/*
 * migrate — Super Block 搬移引擎（GC / 静态 WL / 慢死预测**共用一台**异步 job）
 *
 * 阶段 4：把「读 victim 的有效 CW → 写进冷块 → 更新 L2P/Journal → 擦除归还」
 * 从**同步阻塞**改成**异步 job 状态机**。原实现是 gc.c 的 gc_migrate_sb +
 * gc_erase_and_free 与 wear_leveling.c 的 wl_relocate + wl_execute +
 * wl_predictive_relocate —— 三者的搬移主体逐字同构，差别只在
 *   · 谁当 victim（gc_select_victim / wl_select_victim / rr_predictive_scan）；
 *   · 擦完怎么处置（GC：归还 Free Pool；WL：归还 + 退役 RESERVED）。
 * 故收敛成**一个** mig_job_t：`kind` 选 victim，`post` 决定收尾。
 *
 * ── 为什么必须异步 ──
 *
 * 单次搬移 = 最多 64 页 × 2 plane 的读 + 写 + 2 次擦，同步执行可阻塞数十毫秒；
 * 而它有两个**在完成回调里**的入口（写路径紧急 GC，见 ftl_wbuf_flush），
 * 于是 host 写完成的处理会长时间占住完成泵。job 化后每轮只推进**一步**
 * （一条 NAND 命令），host I/O 与后台搬移在 superloop 上真正重叠。
 *
 * ── 状态机 ──
 *
 *   SELECT（在 mig_begin_* 里做，不是 job 阶段）→ READ → READ_SETTLE → WRITE → WRITE_SETTLE
 *   → FLUSH_JRN → ERASE → DONE
 *
 *   · READ  逐页逐 plane：异步读 → 完成时解 META 得 cw_lba、记 rr_record_ecc / read_count
 *           （慢死预测闭环）。读失败 / 全页空洞 ⇒ 该 plane 无有效 CW。
 *   · READ_SETTLE 纯计算：逐 CW 反查 L2P 得 cw_bitmap。**先就绪前置**（阶段 5）——
 *           region 未驻留会起异步加载并**原地等待**，全部就绪才动 bitmap；否则半途而废
 *           会让 bitmap 一半有一半没有（重试时又从头 `|=`，语义静默错乱）。
 *   · WRITE 本页两 plane 都读完且有有效 CW ⇒ allocator_alloc_pair 拿目标页，逐 plane
 *           提交 CW 子集写。写失败的 plane 不动 L2P（源数据仍有效，下轮再搬）。
 *   · WRITE_SETTLE 纯计算：逐 CW l2p_cache_write + journal_append + 有效计数迁移。
 *           同样**先就绪前置**再动账——半途而废会重复记账（valid_count 虚高、journal
 *           重复条目）。
 *   · FLUSH_JRN 等 Journal job 空闲（**F10 契约**：搬移条目先落盘再擦 victim）。
 *   · ERASE 逐 plane 异步擦：成功 erase_count++ / wear_mark_dirty / 清 valid / 归还 Free Pool；
 *           失败 bbt_mark_bad。WL/预测额外 rr_retire_block。
 *   · DONE  allocator_remove_closed_sb + 进展计数（gc_reclaimed_count）/ 预测命中日志。
 *
 * ── 与同步版的**行为差异**（刻意，登记在案）──
 *
 * 1. **分配不到目标页**：原 gc_migrate_sb 在 allocator_alloc_pair 失败时 `break` 出页循环，
 *    而其调用者 ftl_gc_reclaim_one **无条件**接着 gc_erase_and_free ⇒ **擦掉尚未搬移的
 *    有效页 = 静默丢数据**。本引擎改为：分配不到目标页 ⇒ **中止 job 且不擦 victim**
 *    （源数据仍在、L2P 未动、条目留在 closed_sb_list），下轮 GC 再来。自愈，且不丢数据。
 *
 * 2. **擦除失败时不再退役**：原 wl_predictive_relocate 在 wl_relocate 返回后**无条件**
 *    rr_retire_block（即使 gc_erase_and_free 已把该块标坏）。本引擎只在**擦除成功**时
 *    退役（`post == MIG_POST_RETIRE`）——擦不掉的块归 bbt_mark_bad 管，把它同时标成
 *    "RESERVED、仍可读"是自相矛盾的。擦除失败本身是极端分支（NAND 拒擦）。
 *
 * 3. **提交侧卡死计数**：原同步版没有这个概念（阻塞调用要么成功要么当场报错）。异步后
 *    加 MIG_SUBMIT_STALL_MAX 轮上限，超限中止 job（同 1：不擦 victim，数据无损）。
 *
 * ── 单飞行 ──
 *
 * 每实例同时只允许一个 mig job（`active`）。GC 与 WL 共用引擎 ⇒ WL 让 GC 先跑
 * （符合「GC 是 WL 的擦除出口」）。
 *
 * ⚠ 本文件**不被** gc.h / wear_leveling.h include —— migrate.c 单向依赖它们的选块策略，
 *   反向包含会成环。ftl.h include 本文件以放置 mig_job_t。
 */

#include "fc/fc_command.h"    /* fc_command_status_t（服务函数签名） */
#include "ftl/allocator.h"    /* super_block_t */
#include "ftl/ftl_config.h"   /* nand_pba_t, CW_NUM, PAGE_MAIN_SIZE */
#include "ftl/ftl_type.h"     /* lba_t */
#include "nand/nand_config.h" /* PLANE_NUM, PAGES_PER_BLOCK */

#include <stdint.h>

/* 前向声明（ftl_context_t 在 ftl.h 中定义；本文件被 ftl.h include，用 struct 标签打破环） */
struct ftl_context;
typedef struct ftl_context ftl_context_t;

/* ── 连续「提交不出去」的轮数上限 ──
 * 只兜**提交侧**（命令池被在飞的 host I/O 占满 ⇒ ftl_async_* 返 FTL_AGAIN）。
 * 命令一旦在飞，NAND 层的 wait/jump 超时保证它**必有**完成（超时置
 * FC_CW_STATUS_TIMEOUT），故不需要（也不该）在飞时强拆 job。
 * 超过即判卡死 ⇒ 中止 job（不擦 victim，数据无损，下轮重试）。 */
#define MIG_SUBMIT_STALL_MAX (256U)

/* ── 状态机的 phase ──
 * 约定同 rr_stage_t：纯计算 phase 只从 mig_step 进入、跑完立刻转下一个（不提交命令）；
 * 提交 phase 由 mig_step 提交命令后**原地停留**，等完成服务函数改写。
 * 判「有没有命令在飞」用 waiting 而不是 phase：提交可能因背压失败（phase 不变但无命令）。 */
typedef enum
{
    MIG_IDLE = 0,     /* 空闲：无 job */
    MIG_READ,         /* 提交阶段：读 victim 当前页的当前 plane */
    MIG_READ_SETTLE,  /* 纯计算：反查 L2P 得 cw_bitmap（region 未就绪则原地等，阶段 5） */
    MIG_WRITE,        /* 提交阶段：写目标页的当前 plane */
    MIG_WRITE_SETTLE, /* 纯计算：改 L2P/Journal + 迁移有效计数（同上，阶段 5） */
    MIG_FLUSH_JRN,    /* 纯计算：等 Journal job 空闲（F10 屏障） */
    MIG_ERASE,        /* 提交阶段：擦 victim 的当前 plane */
    MIG_DONE          /* 纯计算：收尾（移除 closed 条目 + 计数 + 复位 job） */
} mig_phase_t;

/* 收尾方式（决定擦除后怎么处置 victim 的块）。 */
typedef enum
{
    MIG_POST_FREE = 0, /* GC：擦完归还 Free Pool */
    MIG_POST_RETIRE    /* WL / 慢死预测：归还后**退役**（RESERVED，不再分配、不标坏、仍可读） */
} mig_post_t;

/* victim 来源（只用于日志与收尾差异）。 */
typedef enum
{
    MIG_KIND_GC = 0, /* 空间回收（gc_select_victim） */
    MIG_KIND_WL,     /* 静态磨损均衡（wl_select_victim） */
    MIG_KIND_PREDICT /* 慢死预测旁路（rr_predictive_scan） */
} mig_kind_t;

/* ── 搬移 job（per-instance，放 ftl->mig）──
 *
 * ⚠ page_buf 是 4KB 的页镜像，**今天在 gc_migrate_sb / wl_relocate 的栈上**，迁到本
 *   结构体是真实新增的 `.bss` 占用（见 ftl_optimization_plan 内存预算）。
 *   放上下文而非栈：异步 job 的栈帧早就在提交时返回了，数据必须活到完成。
 */
typedef struct
{
    uint8_t  active;       /* 1 = job 在跑（单飞行门） */
    uint8_t  phase;        /* mig_phase_t */
    uint8_t  kind;         /* mig_kind_t */
    uint8_t  post;         /* mig_post_t */
    uint8_t  waiting;      /* 1 = 有一条搬移命令在飞（等 g_op_service[FTL_OP_GC]/[FTL_OP_WL]） */
    uint8_t  page;         /* 当前页索引 0..PAGES_PER_BLOCK；== PAGES_PER_BLOCK ⇒ 读阶段结束 */
    uint8_t  plane;        /* 当前 plane（读/写/擦各自迭代） */
    uint8_t  aborted;      /* 1 = 中止（不擦 victim、不移除 closed 条目、不计数） */
    uint8_t  decode_ok;    /* 1 = 本 plane 的 READ 解 META 成功（决定 SETTLE 要不要反查 L2P） */
    uint8_t  write_ok;     /* 1 = 本 plane 的 WRITE 成功（决定 SETTLE 要不要改 L2P/Journal） */
    uint16_t stall_rounds; /* 连续提交不出去的轮数（见 MIG_SUBMIT_STALL_MAX） */

    super_block_t victim; /* victim 的**副本**（closed_sb_list 会被 swap-remove 挪动，不持指针） */

    uint32_t   cw_bitmap[PLANE_NUM];      /* 本页逐 CW 有效位（bit = plane*CW_NUM+cw） */
    lba_t      base[PLANE_NUM];           /* 本页各 plane 的页基址（CW0 的扇区 LBA） */
    lba_t      cw_lba[PLANE_NUM][CW_NUM]; /* 本页逐 CW 的扇区 LBA（未写过 = FTL_LBA_INVALID） */
    nand_pba_t src[PLANE_NUM];            /* 本页各 plane 的源 PBA */
    nand_pba_t dst[PLANE_NUM];            /* 本页各 plane 的目标 PBA（allocator_alloc_pair 出参） */

    /* ⚠ 源块的全局块号（BST 索引）**不缓存**在 job 里：真值是 victim.plane_blocks[plane]，
     *   快路径（valid_total == 0 ⇒ 直接 ERASE）不经过 READ 阶段，缓存的字段在快路径下
     *   是 0 ⇒ 擦除收尾会改块 0 的元数据。见 migrate.c 的 mig_src_gblock。 */

    uint32_t migrated; /* 本 job 累计搬移的有效 CW 数（>0 才需要 F10 屏障） */

    /* 心跳基准（见 ftl_job.h）：在飞期间每满 1s 打一行 kind/phase/page/age */
    uint32_t start_ms;
    uint32_t log_ms;

    uint8_t page_buf[PLANE_NUM][PAGE_MAIN_SIZE]; /* 页镜像（读落此、写从此取） */
} mig_job_t;

/* ── 接口 ── */

/*
 * mig_begin_gc - 选 victim 并启动一个 GC 搬移 job
 *
 * 选不出 victim ⇒ gc_novictim_count++ 并返回 0（"触发但选不出 victim"是卡死嫌疑，
 * 该计数是区分「在推进」与「卡死」的唯一判据，见 ftl.h）。
 *
 * @param ftl FTL 实例（**前置条件**，非法即 ASSERT）
 * @return true = 已启动 job；false = 未启动（job 忙 / 无 victim）
 */
bool mig_begin_gc(ftl_context_t *ftl);

/*
 * mig_begin_wl - 触发判定 → 选 victim → 启动一个静态 WL 搬移 job
 *
 * @param ftl FTL 实例（**前置条件**，非法即 ASSERT）
 * @return true = 已启动 job；false = 未启动（job 忙 / 未触发 / 无 victim）
 */
bool mig_begin_wl(ftl_context_t *ftl);

/*
 * mig_begin_predict - 慢死预测巡检 → 启动一个"搬空并退役"的搬移 job
 *
 * @param ftl FTL 实例（**前置条件**，非法即 ASSERT）
 * @return true = 已启动 job；false = 未启动（job 忙 / 未命中）
 */
bool mig_begin_predict(ftl_context_t *ftl);

/*
 * mig_service - 推进搬移 job（每轮 ftl_service 的 IDLE / NORMAL / BUSY 三档都要调）
 *
 * 在飞 ⇒ 空操作（等完成服务函数）；不在飞且 job 未结束 ⇒ 续跑下一步。
 * 幂等且廉价：无 job 时只判一个 active。
 *
 * ⚠ 本函数**不**同步等待；提交走 ftl_async_* + ftl_op_submit。
 *
 * @param ftl FTL 实例（**前置条件**，非法即 ASSERT）
 */
void mig_service(ftl_context_t *ftl);

/* 完成服务函数 ftl_op_migrate 的声明在 ftl_op.h —— 那里是 g_op_service 各槽的唯一权威列表。 */

#endif /* FTL_MIGRATE_H */

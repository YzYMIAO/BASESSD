#ifndef FTL_INTERNAL_H
#define FTL_INTERNAL_H

#include "ftl_config.h"
#include "ssd_types.h"
#include "timer.h"

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

/* FTL 上下文前向声明：模块头只依赖指针，避免把完整 ftl_ctx_s 拉进每个文件。 */
typedef struct FTL_CONTEXT_S ftl_ctx_s;

/* 主机范围必须落在共享 cache 的键空间内（C9：同一语义只允许一套上限）。 */
_Static_assert(FTL_LBA_MAX_AT_MAX <= L2P_GLOBAL_LBA_MAX_AT_MAX, "FTL_LBA_MAX 超出共享 cache 的全局 LBA 上限");

#endif /* FTL_INTERNAL_H */

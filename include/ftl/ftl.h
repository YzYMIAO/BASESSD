#ifndef FTL_SERVICE_H
#define FTL_SERVICE_H

/*
 * ftl_service — 主循环调度器
 *
 * 每轮 superloop 两段（**泵与实例分开**）：
 *   ① ftl_service_pump()   —— **实例无关**：Phase 1 推进 NAND + Phase 2 抽干完成表。
 *                              全局的 NAND 引擎与完成表，每轮**只调一次**。
 *   ② ftl_service(ftl)     —— **单实例**：Phase 2.5 补发子操作 + Phase 3 三级后台。
 *                              对**每一个**实例都要调（多 LUN 下不能只跑 [0][0][0]）。
 *
 * 三级模式切换依据未决命令数（pending + 组），非"本轮提交了几条 NAND"。
 */

#include "allocator.h"    /* ftl_tables_t, super_block_t */
#include "bg_scheduler.h" /* 时间预算后台调度器（ftl_context_t 含调度器） */
#include "checkpoint.h"
#include "ftl_type.h" /* ftl_io_request_t */
#include "gc.h"
#include "journal.h"
#include "l2p_cache.h"
#include "metadata.h"
#include "migrate.h"
#include "pending.h"
#include "read_recovery.h"
#include "ssd_types.h"
#include "wbuf.h"

#include <stdint.h>

/* ── 宏 ── */

#define NAND_SUBMIT_BUDGET    (4U)     /* 每轮 fc_service 最多跑 4 次（首轮抽干完成，余为填充 pass） */
#define BG_NORMAL_FLUSH_COUNT (4U)     /* NORMAL 模式下最多 flush 4 line */
#define BG_BUSY_FLUSH_COUNT   (1U)     /* BUSY 模式下最多 flush 1 line */
#define CP_WRITE_INTERVAL     (1024U)  /* 写入量触发间隔 */
#define CP_TIME_INTERVAL_MS   (10000U) /* 定时触发间隔（ms）*/

/* GC 触发判定的求值节拍（见 ftl_gc_evaluate）。
 * 判据本身是**事件**（空间压力/磨损），但求值含一次**全表扫描**，不能按主循环 pass 跑。 */
#define GC_EVAL_INTERVAL_MS (10U)

/* GC 触发日志的节拍（与求值同一套时基）。
 * ⚠ 求值节拍**不能**兼作日志节拍：求值节拍管「多久算一次」，10ms 拍仍会打出 100 行/秒。
 * 日志要的是「同一状态别重复报」。 */
#define GC_LOG_INTERVAL_MS (1000U)

#define FTL_READ_USER_SIZE (2048U) /* 一次读返回的用户数据大小 = 4 CW × 512B */

/* 一个组的子操作连续遇 FTL_AGAIN（瞬态背压）而**无任何进展**的轮数上限。
 * 超过即判真卡死：弃未下发子操作、组报 IO_ERROR（已下发的照常完成）。
 * 预算取 8——正常背压在下轮 ftl_service 抽干完成后即恢复，连续 8 轮无进展属异常。 */
#define FTL_DRAIN_STALL_MAX (8U)

/* 最大有效 LBA（全局）= 映射单元总数（FTL_IU_TOTAL，见 ftl/ftl_config.h §一）。
 * 当前几何下 = 2048×512 = 1,048,576，与用户容量 512 MiB 逐条对应。 */
#define FTL_LBA_MAX (FTL_IU_TOTAL)

/* 全局 LBA 上限必须与共享 cache 的键空间同源（C9：同一语义只允许一套范围）。 */
_Static_assert((uint64_t)FTL_LBA_MAX == L2P_GLOBAL_LBA_MAX, "FTL_LBA_MAX 与共享 cache 的全局 LBA 上限不一致");

/* ── FTL 主上下文 ── */

typedef struct ftl_context
{
    /* 拓扑信息（本 FTL 绑定的 NAND 芯片路由） */
    ftl_topology_t topo; /* 本 FTL 绑定的 NAND 芯片路由 */

    /* L2P Cache（共享：指向 g_l2p_cache[ch][ce]，同一 (ch,ce) 下的所有 LUN 共用） */
    l2p_cache_t *cache;

    /* Pending Request 表（per-instance，本实例异步请求追踪）
     * 由 pending.c 操作：每个子操作一个槽（command_id → 组 + 子操作字段）。 */
    ftl_pending_req_t pending[FTL_PENDING_MAX];

    /* 完成组表（一条 host 请求 = 一个组，聚合其全部子操作的完成通知）
     * 由 pending.c 操作（pending_group_*）。 */
    ftl_io_group_t groups[FTL_GROUP_MAX];

    /* 页打包写缓冲——同一 pair 窗口的多次写攒成一页再编程，
     * 见 wbuf.h 的 ftl_wbuf_t。由 wbuf.c 的 ftl_wbuf_* 族操作。 */
    ftl_wbuf_t wbuf;

    /* FTL 表指针（由 HIL 的 ftl_storage_t 提供并决定落哪个域）。
     * BST blocks(80KB) / free+op(16KB) / closed(32KB) / spare(200B)，见 ftl_tables_t。
     * allocator_init 会把 alloc 内部的指针直接绑到本表，故下游大多数表访问走
     * `ftl->alloc->...`，不经过本指针。 */
    ftl_tables_t *tables;

    /* Allocator（各指针绑到上面的 tables，见 allocator_init） */
    ftl_allocator_t alloc;

    /* Journal（指针，由 storage 提供；落 SRAM3） */
    l2p_journal_t *journal;

    /* Metadata Directory（指针，由 storage 提供；落 SRAM4） */
    metadata_dir_cache_t *metadata_dir;

    /* Checkpoint 池游标/序号（per-instance）。
     * 开机由 checkpoint_find_latest 恢复，Checkpoint job 据此选池内下一槽续写。
     * ⚠ 与 journal.sequence_counter 无关——本池自己的单调序号（见 l2p_cp_state_t），
     *   boot 日志里的 cp_seq 另来自 ftl_boot_result_t。 */
    l2p_cp_state_t cp;

    /* Checkpoint 异步 job（脏区落盘 → Anchor → Header → 截断 journal 的
     * 有序流水线，见 checkpoint.h 的 cp_job_t / 掉电安全契约）。
     * ⚠ 含 256B 的 region 脏位图**快照**——必须在 job 起点取定，不能沿用实时位图
     *   （flush 成功会清实时位，见 checkpoint.c 的"快照点前移"）。
     * 指针，由 storage 提供；落 SRAM3。 */
    cp_job_t *cp_job;

    /* Checkpoint 定时触发基准（T3：elapsed_ms ≥ CP_TIME_INTERVAL_MS） */
    uint32_t cp_timer_start; /* 上次 Checkpoint 时间戳（ms） */

    /* 统计 */
    uint32_t total_writes;

    /* 后台写闸门（`bbt --erase` 置位）。
     * 全片擦除后介质已空，而本实例 RAM 里的 BST/L2P/Journal 仍描述着**已不存在**的
     * 布局 ⇒ 唯一正确的后续动作是复位。置位后 ftl_bg_service 整体跳过（含 must-run
     * 集：checkpoint / journal / 元数据目录 / bbt / GC 全在那里），杜绝把旧 RAM 状态
     * 写回擦净的芯片；Phase 1/2/2.5 照常（NAND 泵与 host IO 不受影响）。
     * ⚠ 只由诊断命令置位，且**无清除路径**——清它只能靠复位。 */
    uint8_t bg_suspend;

    /* L2P 加载背压信号（per-instance）。
     * ftl_issue_subop 因 **L2P region 未就绪**（异步加载在飞，l2p_cache_read 返
     * FTL_AGAIN）而返回 FTL_AGAIN 时置 1；ftl_drain_group 据此**不计** `stalls`——
     * 加载在飞 = 有进展，不是"下发不动"。否则一次正常的 region 加载会吃掉 8 轮预算
     * （FTL_DRAIN_STALL_MAX）里的一轮，随机读满盘时把组误判成卡死报 IO_ERROR。
     * 真卡死（命令丢失）由 NAND 层 wait/jump 超时兜底，不靠这个计数。 */
    uint8_t l2p_defer;

    /* 慢死预测巡检节流（per-instance） */
    uint32_t rr_last_scan_ms;

    /* GC 触发求值的节拍基准（per-instance）。
     * ⚠ 为什么需要：求值点 ftl_gc_evaluate 里的 T5 要算**全表**磨损极值
     * （gc_table_extremes 遍历 FTL_BLOCK_TOTAL = 2048×PLANE_NUM 个块，≈80KB），
     * 而它挂在 Phase 3 的 IDLE/NORMAL 两条路径上、主循环 `while(1)` 又无节拍
     * ⇒ 必须用本字段限频，否则每轮扫一遍全表（选不出 victim 的卡死态下即 100% CPU）。
     * 与 rr_last_scan_ms（1s 巡检）/ cp_timer_start 的节拍门同族。
     * ⚠ 0 作「尚未求值」哨兵：平台时钟缺失时 ftl_now_ms() 恒返回 0，此时每轮都求值
     * ——GC 不能因为缺时钟而停摆（与 T3 定时「恒不触发」的取舍方向相反）。 */
    uint32_t gc_eval_last_ms;

    /* GC 触发日志的节拍基准（per-instance，与 gc_eval_last_ms 同一套时基）。
     * 语义 = 「进入触发态立即打一行 + 触发态内每 GC_LOG_INTERVAL_MS 一行心跳」：
     * 退出触发态时**归零**，故下次进入必然立即打（不需要另存触发掩码）。
     * ⚠ T1（free_count < FREE_POOL_TARGET）是 GC 的**常态工作区间**而非异常，
     * 逐轮打会把 UART 环形队列占满、挤掉其它日志（`log_get_truncated_count` 上涨）；
     * 而卡死态又需要心跳——「一行之后再无消息」会被误读成已恢复。
     * ⚠ 同 gc_eval_last_ms：0 兼作「本触发态内尚未打过」哨兵。 */
    uint32_t gc_log_last_ms;

    /* 时间预算后台调度器（per-instance）。
     * 任务表（13 项：6 must-run + 7 预算）由 ftl_bg_init 从 `ftl/bg_wiring.c` 的静态表
     * 装载；每轮 Phase 3 由 ftl_bg_service 选档后跑 bg_scheduler_run。 */
    bg_scheduler_t bg;

    /* GC 进展计数（per-instance，只增不减，纯观测）。
     * ⚠ 为什么需要：`trig=` 常亮**不能区分**「在推进」与「卡死」——两种情况打的是同一行。
     * 卡死链路：gc_select_victim 选不出 victim ⇒ mig_begin_gc 返 0 ⇒ 空闲池只出不进
     * ⇒ free_count 掉到 0 后永不回升、写最终永久失败。Closed 条目对 GC/WL 不可见
     * （`allocator_close_sb` 的 is_open 顺序写错）就是这个形态。
     * 判据：两次心跳之间 gc_reclaimed_count 不动、gc_novictim_count 在涨 = 卡死。
     * 自增点：reclaimed 在 mig_finish 的 MIG_DONE（仅擦除成功且 closed 条目已移除），
     * novictim 在 mig_begin_gc ⇒ IDLE 补池与 NORMAL 单发两条路径都覆盖。 */
    uint32_t gc_reclaimed_count; /* 累计成功回收的 SB 数 */
    uint32_t gc_novictim_count;  /* 累计「触发但选不出 victim」次数 */

    /* 搬移 job（GC / 静态 WL / 慢死预测共用一台异步引擎，见 migrate.h）。
     * ⚠ 含逐 plane 的页镜像（page_buf，当前共 4KB），必须常驻：异步 job 的栈帧在
     *   提交命令时就已返回，数据要活到完成回调。 */
    mig_job_t mig;

    /* Read Recovery：per-Block ECC 趋势追踪在 alloc.blocks[].rr（BST 单数据源）；
     * 本结构 = 异步恢复 job 状态机本体（见 read_recovery.h） */
    ftl_read_recovery_t rr;

    /* Read Recovery 整页暂存（CW 级映射：一个子操作可能只覆盖页内部分 CW，
     * 而恢复读内部是**整页读**、从 CW0 起连续拷出。故恢复结果先落本页
     * 镜像，再按各 CW 的真实偏移搬回 caller 缓冲（见 rr_settle_plane）。
     * 放上下文而非栈：2048B 的栈局部在 DTCM 栈上风险高，且 per-instance 无别名问题。 */
    uint8_t rr_page_scratch[FTL_READ_USER_SIZE];
} ftl_context_t;

/*
 * ftl_storage_t - 描述符的外部存储
 *
 * 四个大数组由 HIL 提供并决定落哪个域（main.c：tables→SRAM12、md→SRAM4、
 * journal/cp_job→SRAM3）。ftl_init 只绑定指针；数组定义在 main.c，
 * 生命周期覆盖全程（完成表的 ctx 会保存这些地址）。
 */
typedef struct
{
    ftl_tables_t         *tables;
    metadata_dir_cache_t *metadata_dir;
    l2p_journal_t        *journal;
    cp_job_t             *cp_job;
} ftl_storage_t;

/* ── 接口 ── */

/*
 * ftl_init - 初始化 FTL 子系统
 *
 * 描述符只持有指针；四个大数组由调用者通过 storage 提供。
 * L2P cache 是**设备级共享对象**（同一 (ch,ce) 一份），由调用者初始化一次后
 * 传入；本函数只绑定指针，不 init、不 set_md_dir。
 *
 * ⚠ storage 指向的数组可能在 NOLOAD 段（`.sram*_data`）：启动代码不清零，
 *   本函数会显式 memset 四个数组。
 *
 * @param ftl     FTL 描述符
 * @param topo    本实例绑定的 NAND 芯片路由
 * @param cache   本实例所属 (ch,ce) 的共享 L2P cache（非空，设备级已初始化）
 * @param storage 四个大数组的存储（非空，四个指针都非空）
 * @return FTL_OK 成功
 */
ftl_status_t ftl_init(ftl_context_t *ftl, ftl_topology_t topo, l2p_cache_t *cache, const ftl_storage_t *storage);

/*
 * ftl_service_pump - 实例无关的每轮泵（superloop 每轮调一次）
 *
 * Phase 1: fc_service() 推进 NAND（≤ NAND_SUBMIT_BUDGET 次，首轮即抽干完成）
 * Phase 2: completion_table_drain() 处理完成回调（按命令 ch/ce/lun 路由回实例）
 *
 * ⚠ 必须在所有 ftl_service() 之前调，且**每轮只调一次**——NAND 引擎与完成表都是
 *   全局单份，per-instance 调用会把 fc_service 预算重复消耗 N 遍。
 */
void ftl_service_pump(void);

/*
 * ftl_service - 单个 FTL 实例的服务（对**每个**实例都要调）
 *
 * Phase 2.5: 补发本实例各组的待下发子操作（host IO 优先于后台）
 * Phase 3:   本实例的时间预算后台调度（ftl_bg_service：选档 → 任务表 → 预算）
 *
 * ⚠ 调用方须先调 ftl_service_pump()。多实例（多 LUN）下漏调某实例 ⇒ 它的
 *   journal / checkpoint / bbt / GC 永不推进。
 *
 * @param ftl  FTL 上下文（**非 NULL**；NULL 属编程错误，ASSERT 兜底）
 */
void ftl_service(ftl_context_t *ftl);

/*
 * ftl_bg_init - 装配实例的时间预算后台调度器
 *
 * 把 `ftl/bg_wiring.c` 的静态任务表（13 项：6 must-run 正确性关键集 + 7 预算任务，
 * 含 bg_a_* 适配器）装载进 ftl->bg。由 ftl_init 末尾调用一次。
 *
 * ⚠ 为什么不做成 ftl.c 直接调 bg_scheduler_init：任务表是 bg_wiring.c 的 static 数据，
 *   外部模块（ftl.c）不该看到它——本函数是这张表的**唯一装配口**（H13 三问：隐藏数组 /
 *   边界检查 / 单点读写，三者皆是）。
 */
void ftl_bg_init(ftl_context_t *ftl);

/*
 * ftl_bg_service - Phase 3 入口：bg_suspend 判定 → 选档 → 档位配置 → 引擎跑一轮
 *
 * ⚠ bg_suspend 判定在函数内**最先**做（早于 must-run pass）：`bbt --erase` 之后介质已空
 *   而 RAM 状态陈旧，任何后台写都是往干净芯片上倒垃圾。调用方不要在外层重复判。
 *
 * @param ftl  FTL 上下文（**非 NULL**；唯一调用点 ftl_service 已保证）
 */
void ftl_bg_service(ftl_context_t *ftl);

/*
 * ftl_bg_level_select - 负载 → 后台档位（纯函数）
 *
 * 判据：
 *   inflight = pending_count + pending_group_count
 *   inflight==0 ⇒ IDLE；pending > FTL_PENDING_MAX/2 或 queued!=0 ⇒ BUSY；否则 NORMAL。
 * 返回 BG_LEVEL_IDLE / BG_LEVEL_NORMAL / BG_LEVEL_BUSY（bg_scheduler.h）。
 */
uint8_t ftl_bg_level_select(const ftl_context_t *ftl);

/*
 * ftl_submit_read - 非阻塞提交读请求（长度任意）
 *
 * 请求带**长度**（req->count 个连续 LBA），FTL 自行决定单/双 plane 拆分：
 *   ① 建完成组（一条 host 请求 = 一个组）
 *   ② 逐子操作下发（每个子操作 2 页 Multi-Plane，末条奇数页时单 plane）
 *   ③ 未映射子操作同步返回全零；余下子操作遇资源背压留待 ftl_service Phase 2.5
 *   ④ 数据在 completion_table_drain 中路由回 buffer
 *
 * ⚠ 一次调用的完成通知（complete_flag/status_out/on_complete）**只在全部子操作
 *   完成后触发一次**。返回 FTL_OK 仅表示已受理（含部分子操作排队），不代表已完成。
 *
 * @param ftl  FTL 上下文
 * @param req  IO 请求描述（lba/buffer/count/complete_flag/status_out/on_complete/ctx）
 * @return FTL_OK 已受理，FTL_ERROR 立即失败（参数非法 / LBA 越界 / 组槽满）
 */
ftl_status_t ftl_submit_read(ftl_context_t *ftl, const ftl_io_request_t *req);

/*
 * ftl_submit_write - 非阻塞提交写请求（长度任意）
 *
 * 与 ftl_submit_read 对称：FTL 按 count 拆子操作，每子操作分配新 PBA（**逐 plane
 * 独立选块**——Policy B：SB 两 plane 的块号各自独立）+ 建 desc 链 + 异步写 NAND。
 * Journal + L2P 在 completion handler 中确认 NAND PROGRAM 成功后执行。
 *
 * @param ftl  FTL 上下文
 * @param req  IO 请求描述（同 ftl_submit_read）
 * @return FTL_OK 已受理，FTL_ERROR 立即失败
 */
ftl_status_t ftl_submit_write(ftl_context_t *ftl, const ftl_io_request_t *req);

/*
 * ftl_flush - 落盘已持久化映射（L2P cache 写回 + journal 尾页刷出）
 *
 * 语义 = "把 dirty 元数据落到 NAND"，**非** NVMe 式 barrier（不等待在飞/排队的写）。
 *
 * @param ftl  FTL 上下文（**前置条件**，非法即 ASSERT）
 */
void ftl_flush(ftl_context_t *ftl);

/*
 * ftl_trim - 取消 [lba, lba+count) 的 LBA 映射（非阻塞、同步完成）
 *
 * @param ftl   FTL 上下文（**前置条件**，非法即 ASSERT）
 * @param lba   起始逻辑地址（uint64_t）
 * @param count 连续 LBA 数（0 视作 1，兼容单 LBA 调用）
 */
void ftl_trim(ftl_context_t *ftl, uint64_t lba, uint32_t count);

#endif /* FTL_SERVICE_H */
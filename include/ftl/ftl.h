#ifndef FTL_SERVICE_H
#define FTL_SERVICE_H

/*
 * ftl_service — 主循环调度器
 *
 * 每轮 superloop 两段（序 4：多实例化后**泵与实例分开**）：
 *   ① ftl_service_pump()   —— **实例无关**：Phase 1 推进 NAND + Phase 2 抽干完成表。
 *                              全局的 NAND 引擎与完成表，每轮**只调一次**。
 *   ② ftl_service(ftl)     —— **单实例**：Phase 2.5 补发子操作 + Phase 3 三级后台。
 *                              对**每一个**实例都要调（多 LUN 下不能只跑 [0][0][0]）。
 *
 * 三级模式切换依据未决命令数（pending + 组 + 完成表占用），非"本轮提交了几条 NAND"。
 */

#include "common/ssd_types.h"
#include "ftl/allocator.h" /* ftl_tables_t, super_block_t */
#include "ftl/checkpoint.h"
#include "ftl/ftl_type.h" /* ftl_io_request_t */
#include "ftl/gc.h"
#include "ftl/journal.h"
#include "ftl/l2p_cache.h"
#include "ftl/metadata.h"
#include "ftl/pending.h"
#include "ftl/read_recovery.h"

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

/* ── 页打包写缓冲（§七 方案 A）──
 *
 * CW 级映射下子操作常只覆盖半个 pair 窗口（如一次 512B 写只落 plane0 的 CW0），
 * 而器件**按页编程**（参数页 Byte86-89 部分页单位 = 1 CW；Byte110 每 ECC user area
 * 只能编一次）。逐子操作分配页会让每次写都吃掉一整对页 ⇒ 利用率 12.5%~50%。
 *
 * 本缓冲把**同一 pair 窗口**内的多次 host 写攒成一页，窗口填满 / 切到别的窗口 /
 * 显式 flush / 空闲时**一次编程**（两 plane 都有数据即一条 multi-plane）⇒ 写指针
 * 按页推进，页内利用率回到 100%。
 *
 * data[slot] 的 slot = plane*CW_NUM + cw（与 LBA 低 3 位同构），occupied 逐位标记有效
 * 槽。owners 记录「哪些组向本页贡献过」——一页可能由多个组拼成，完成时必须**逐组**
 * 递减在飞数（见 pending_group_complete_subop_multi）。 */
typedef struct
{
    uint8_t  data[FTL_LBA_PER_PAIR * FTL_IU_BYTES]; /* 8 CW × 512B = 4096B 页镜像 */
    uint8_t  occupied;                              /* bit i = 槽 i 有数据（i = plane*CW_NUM+cw） */
    uint8_t  open;                                  /* 1 = 正持有某窗口 */
    lba_t    base;                                  /* 持有页的 pair 窗口基址（低 3 位清零） */
    uint32_t owners;                                /* bit gi = 组 gi 贡献过本页（FTL_GROUP_MAX=32） */
} ftl_wbuf_t;

/* ── FTL 主上下文 ── */

typedef struct ftl_context
{
    /* 拓扑信息（本 FTL 绑定的 NAND 芯片路由） */
    ftl_topology_t topo; /* 本 FTL 绑定的 NAND 芯片路由 */

    /* L2P Cache */
    l2p_cache_t cache;

    /* Pending Request 表（F6：per-instance，本实例异步请求追踪）
     * 由 pending.c 操作：每个子操作一个槽（command_id → 组 + 子操作字段）。 */
    ftl_pending_req_t pending[FTL_PENDING_MAX];

    /* 完成组表（S1：一条 host 请求 = 一个组，聚合其全部子操作的完成通知）
     * 由 pending.c 操作（pending_group_*）。 */
    ftl_io_group_t groups[FTL_GROUP_MAX];

    /* 页打包写缓冲（§七 方案 A）——同一 pair 窗口的多次写攒成一页再编程，
     * 见 ftl_wbuf_t。由 ftl.c 的 ftl_wbuf_* 族操作。 */
    ftl_wbuf_t wbuf;

    /* FTL 自持表（P2-5）——大数组归实例所有，HIL 不再传指针。
     * BST blocks(80KB) / free+op(16KB) / closed(16KB) / spare(100B)，见 ftl_tables_t。
     * 是 ftl_context_t 的**大头**，故内存预算只看本结构体尺寸（main.c 的 _Static_assert）。 */
    ftl_tables_t tables;

    /* Allocator（各指针绑到上面的自持表，见 allocator_init） */
    ftl_allocator_t alloc;

    /* Journal */
    l2p_journal_t journal;

    /* Metadata Directory */
    metadata_dir_cache_t metadata_dir;

    /* Checkpoint 池游标/序号（序 4：per-instance，原 checkpoint.c 的 file-scope static）。
     * 开机由 checkpoint_find_latest 恢复，checkpoint_create 据此选池内下一槽续写。
     * ⚠ 与 journal.sequence_counter 无关——本池自己的单调序号（见 l2p_cp_state_t）。
     * 原 `uint32_t cp_sequence` 字段已删：只写不读，且与本字段语义重复（真正的
     * checkpoint 序号是 cp.sequence；boot 日志里的 cp_seq 来自 ftl_boot_result_t）。 */
    l2p_cp_state_t cp;

    /* Checkpoint 定时触发基准（T3：elapsed_ms ≥ CP_TIME_INTERVAL_MS） */
    uint32_t cp_timer_start; /* 上次 Checkpoint 时间戳（ms） */

    /* 统计 */
    uint32_t total_writes;

    /* 后台写闸门（`bbt --erase` 置位）。
     * 全片擦除后介质已空，而本实例 RAM 里的 BST/L2P/Journal 仍描述着**已不存在**的
     * 布局 ⇒ 唯一正确的后续动作是复位。置位后 ftl_service 的 Phase 3（IDLE/NORMAL/BUSY
     * 三级后台：checkpoint / bbt / wear / journal / GC 全在这里）整体跳过，杜绝把旧
     * RAM 状态写回擦净的芯片；Phase 1/2/2.5 照常（NAND 泵与 host IO 不受影响）。
     * ⚠ 只由诊断命令置位，且**无清除路径**——清它只能靠复位。 */
    uint8_t bg_suspend;

    /* 慢死预测巡检节流（F6：per-instance，原全局 g_rr_last_scan_ms） */
    uint32_t rr_last_scan_ms;

    /* GC 触发求值的节拍基准（per-instance）。
     * ⚠ 为什么需要：求值点 ftl_gc_evaluate 里的 T5 要算**全表**磨损极值
     * （gc_table_extremes 遍历 FTL_BLOCK_TOTAL = 2048×PLANE_NUM 个块，≈80KB），
     * 而它挂在 Phase 3 的 IDLE/NORMAL 两条路径上、主循环 `while(1)` 又无节拍
     * ⇒ 改造前是**每轮扫一遍全表**（GC 选不出 victim 的卡死态下即 100% CPU）。
     * 与 rr_last_scan_ms（1s 巡检）/ wear_flush 的节拍门同族。
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

    /* GC 进展计数（per-instance，只增不减，纯观测）。
     * ⚠ 为什么需要：`trig=` 常亮**不能区分**「在推进」与「卡死」——两种情况打的是同一行。
     * 卡死链路：gc_select_victim 选不出 victim ⇒ ftl_gc_reclaim_one 返 0 ⇒ 空闲池只出不进
     * （`allocator_close_sb` 的 valid 顺序缺陷 cf604ba 就是这个形态：条目对 GC/WL 永久不可见
     * ⇒ free_count 掉到 0 后永不回升、写最终永久失败）。
     * 判据：两次心跳之间 gc_reclaimed_count 不动、gc_novictim_count 在涨 = 卡死。
     * 自增点在 ftl_gc_reclaim_one 内 ⇒ IDLE 补池突发与 NORMAL 单发两条路径都覆盖。 */
    uint32_t gc_reclaimed_count; /* 累计成功回收的 SB 数 */
    uint32_t gc_novictim_count;  /* 累计「触发但选不出 victim」次数 */

    /* Read Recovery：per-Block ECC 趋势追踪已并入 alloc.blocks[].rr（BST 单数据源） */
    ftl_read_recovery_t rr; /* Read Recovery 上下文（仅重试计数） */

    /* Read Recovery 整页暂存（CW 级映射：一个子操作可能只覆盖页内部分 CW，
     * 而 rr_recover_read 内部是**整页读**、从 CW0 起连续拷出。故恢复结果先落本页
     * 镜像，再按各 CW 的真实偏移搬回 caller 缓冲（见 ftl_op_host_io）。
     * 放上下文而非栈：2048B 的栈局部在 DTCM 栈上风险高，且 per-instance 无别名问题。 */
    uint8_t rr_page_scratch[FTL_READ_USER_SIZE];
} ftl_context_t;

/* ── 接口 ── */

/*
 * ftl_init - 初始化 FTL 子系统
 *
 * 表由实例**自持**（P2-5）：大数组在 `ftl->tables` 内，调用者只给拓扑。
 *
 * @param ftl   FTL 上下文（含自持表存储）
 * @param topo  本实例绑定的 NAND 芯片路由
 * @return FTL_OK 成功
 */
ftl_status_t ftl_init(ftl_context_t *ftl, ftl_topology_t topo);

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
 * Phase 3:   本实例的三级后台任务（IDLE / NORMAL / BUSY）
 *
 * ⚠ 调用方须先调 ftl_service_pump()。多实例（多 LUN）下漏调某实例 ⇒ 它的
 *   journal / checkpoint / wear / bbt / GC 永不推进。
 *
 * @param ftl  FTL 上下文（NULL 直接返回）
 */
void ftl_service(ftl_context_t *ftl);

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
 * 独立选块**——Policy B：SB 两 plane 不再同号）+ 建 desc 链 + 异步写 NAND。
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
 * @param ftl  FTL 上下文
 */
void ftl_flush(ftl_context_t *ftl);

/*
 * ftl_trim - 取消 [lba, lba+count) 的 LBA 映射（非阻塞、同步完成）
 *
 * @param ftl   FTL 上下文
 * @param lba   起始逻辑地址（uint64_t）
 * @param count 连续 LBA 数（0 视作 1，兼容单 LBA 调用）
 */
void ftl_trim(ftl_context_t *ftl, uint64_t lba, uint32_t count);

#endif /* FTL_SERVICE_H */
/*
 * nand_dispatch_internal.h
 *
 * 内部数据结构，不对外暴露。
 * 由 nand_dispatch.c / nand_dispatch_tables.c 共享。
 */

#ifndef NAND_DISPATCH_INTERNAL_H
#define NAND_DISPATCH_INTERNAL_H

#include "fc_command.h"
#include "fc_types.h"
#include "nand_config.h"
#include "nand_dispatch.h"
#include "nand_fc_interface.h"
#include "nand_fmc.h"
#include "onfi_1_0.h"

#include <stdint.h>

/* ── CW 掩码遍历 ──
 * flat CW 掩码（bit n = plane*CW_NUM + cw）是"哪些 CW 要访问"的唯一依据，
 * 也是推进顺序的唯一依据。两个小工具被 nand_dispatch.c 与 nand_dispatch_tables.c 共用，
 * 故放头文件里 inline（无外部链接、无未使用告警）。 */

/* mask 中 ≥ n 的第一个置位的绝对编号；没有则返回 CW_TOTAL_MAX（哨兵，越界即"到头"）。
 * 用法：`for (k = nand_dispatch_cw_next(mask, 0U); k < CW_TOTAL_MAX; k = nand_dispatch_cw_next(mask, k + 1U))`。 */
static inline uint8_t nand_dispatch_cw_next(uint32_t cw_bitmap, uint8_t n)
{
    while (n < CW_TOTAL_MAX)
    {
        if ((cw_bitmap & (1UL << n)) != 0UL)
        {
            return n;
        }
        n++;
    }
    return (uint8_t)CW_TOTAL_MAX;
}

/* mask 最低位的绝对编号；mask 为空（退化命令）时返回 0 —— 只用于"取一个不越界的下标"，
 * 需要区分"空掩码"时用 nand_dispatch_cw_next() 的哨兵。 */
static inline uint8_t nand_dispatch_cw_first(uint32_t cw_bitmap)
{
    uint8_t n = nand_dispatch_cw_next(cw_bitmap, 0U);

    return (n < CW_TOTAL_MAX) ? n : 0U;
}

/* ── dispatch task：一次预解析后的命令实例 ──
 * 由 nand_dispatch_task_load() 从 nand_dispatch_request_t 填充，
 * poll 依此驱动 nand_fc_start/step。 */
typedef struct
{
    uint16_t command_id;
    uint8_t  operation;
    uint8_t  ch;            /* Channel — needed for page_buffer access. */
    uint8_t  ce;            /* Chip-enable — needed for routing. */
    uint8_t  lun;           /* LUN — needed for routing. */
    uint8_t  page_type : 2; /* Copy from dispatch request for LDPC engine. */
    uint8_t  reserved  : 6;
    uint32_t cw_bitmap;    /* 选中的 CW 集合：bit n = plane*CW_NUM + cw（flat 绝对编号）。
                            * 与 request->cw_bitmap 同一值；是 cw_buffers / cw_index 的唯一依据
                            * （"本任务有几个 CW"= popcount(mask)，"第 k 个"= 第 k 个置位）。 */
    uint8_t cw_index;      /* 当前 CW 的 **flat 绝对编号 n**（∈ cw_bitmap），不是密排槽位。
                            * 生产：task_load 置为掩码最低位；cw_chain 每取完一个推进到下一个置位。
                            * 消费：completion_per_cw 用它当下标写 cw_status[n]，
                            *   并把它作为 failed_descriptor_id 上报。 */
    uint8_t plane_count;   /* 0 = 单 plane（FC 只对 multi-plane 分支赋值），N = multi-plane。
                            * 见 nand_dispatch.h 同名字段的完整说明。 */
    uint8_t plane;         /* 本命令目标物理 plane 索引。
                            * 单 plane：= request->plane，所有单 plane req_* handler / cw_chain /
                            *   scatter 一律读 plane_paa[plane]。
                            * multi-plane：恒 0；multi-plane handler 不读本字段，仍写死 [0]/[1]。
                            * ⚠ 必须无条件赋值：task slot 会被链内后续命令复用（task_load 不做整体
                            *   memset），漏赋值会读到上一条命令的残值。 */
    uint8_t  started;      /* 0=未启动；1=已 nand_fc_start（含 start 内同步完成） */
    uint8_t  feature_addr; /* SET/GET_FEATURES 的寄存器地址（column）。内联数据走 cw_buffers，不走本层。 */
    uint16_t column;       /* **显式列地址**（仅 OP_READ_COLUMN 用，见 nand_dispatch.h 同名字段）。
                            * 其余 op 恒 0；req_read_column 直接把它当列地址，不再从 paa 的 cw 推。 */

    /* Pre-resolved per-CW data（定容数组，容量 = 硬件上限 CW_TOTAL_MAX = PLANE_NUM×CW_NUM）。
     * ⚠ 下标是 **flat 绝对 CW 号 n = plane*CW_NUM + cw**，与 cw_bitmap 的 bit 号、
     *   completion.cw_status[] 的下标三者同一空间——"哪些格有效"完全由 cw_bitmap 决定，
     *   不在掩码里的格是确定性零值（request 整体 {0} 初始化后只填掩码内的格）。
     *   物理 plane 由 n / CW_NUM 得出，CW 号由 n % CW_NUM 得出，不再各存一份。 */
    nand_paa_t plane_paa[PLANE_NUM];     /* 各 plane 的行地址承载 PAA（CW 位被 paa_to_nand_row 丢弃）。 */
    uint8_t   *cw_buffers[CW_TOTAL_MAX]; /* [n]，仅 n ∈ cw_bitmap 有效。 */

    /* ── 引擎上下文四件套（Phase V）──
     * 全部随 task slot 常驻，跨多次 poll 调用存活；生产/消费阶段见各字段。 */

    /* FMC 后端（handler 表 + context，单 bank 全 LUN 共享）。
     * 生产：nand_dispatch_task_load() 经 nand_fmc_backend_get() 绑定（唯一写点）。
     * 消费：同一函数下一行转交 transaction.engine.backend —— task 自身路径此后不再读它；
     *       真正的消费者是引擎（nand_fc_start 判空回退默认后端，nand_engine_start 经
     *       engine->backend 取 handler）。 */
    const nand_engine_backend_t *backend;

    /* 本次 ONFI 事务的请求参数（column/row/data/data_length）。
     * 生产：nand_dispatch_task_start() → nand_dispatch_task_build_request() 经
     *       g_nand_dispatch_req_table[operation] 分派，各 req_* handler 在
     *       nand_dispatch_tables.c 里整块赋值；CW 链续读时由 nand_dispatch_task_cw_chain()
     *       覆写（改用 OP_RANDOM_DATA_READ + CW 偏移列地址）。
     * 消费：nand_fc_start() —— 拷入 transaction->request 后交给引擎。 */
    nand_fc_request_t request;

    /* 引擎侧事务状态（UOP 游标 + 三指针 + 引擎完成项）。
     * 生产：nand_dispatch_task_load() 一次性绑定 engine 三指针（地址随事务固定，只绑一次）；
     *       nand_fc_start() 重置 transaction->request/completion 并 nand_engine_start()；
     *       此后由 nand_engine_step() 逐 uop 推进。
     * 消费：nand_fc_step() 推进；engine.active 判"start 内同步完成"；
     *       transaction.completion.nand_status 逐处抄进 ctx->nand_completion。
     * 这是 ISSUE 发完即返回后、tR/tPROG/tBERS 期间"记住发到哪了"的唯一载体。 */
    nand_fc_transaction_t transaction;

    /* 调度层完成项（对 FC 上报的最终结果）。
     * 生产：nand_dispatch_task_load() 初始化；nand_dispatch_task_complete() 写
     *       execution_status/nand_status 后按 operation 分派 completion handler 填 CW 位。
     * 消费：nand_dispatch_lun_step() 在每条命令收尾时 *completion = task->completion
     *       并出队（每槽 = 一条命令 = 一份 completion）。
     * 注意：与 transaction.completion（引擎层 nand_command_completion_t）是两个不同对象，
     *       仅名字撞车——本字段才是给上层的。 */
    nand_dispatch_completion_t completion;
} nand_dispatch_task_t;

/*
 * LUN 相位枚举 (Phase V multi-LUN ISSUE/WAIT/FETCH phase machine)
 *   IDLE  → ISSUE → ISSUED → FETCH → IDLE
 *   ISSUE ：发完命令立即返回，LUN 进入 tR/tPROG/tBERS
 *   ISSUED：DWT 计时最短忙期，到点后启动配对 WAIT/FETCH
 *   FETCH ：执行 WAIT_READY + 数据 + 状态检查
 */
#define LUN_PHASE_IDLE   (0U) /* LUN 空闲 */
#define LUN_PHASE_ISSUE  (1U) /* 正在执行 ISSUE uop */
#define LUN_PHASE_ISSUED (2U) /* ISSUE 完成，LUN 内部操作中（tR/tPROG/tBERS） */
#define LUN_PHASE_FETCH  (3U) /* 正在执行 WAIT/FETCH uop */

/*
 * 相位写入点 / 读取点（均在 src/nand/nand_dispatch.c）——读相位机代码先看这张表：
 *   写入：IDLE   ← submit / submit_chain（新槽） / lun_step（一条命令出队后复位）
 *         ISSUE  ← 仅 nand_dispatch_task_fc_start（ISSUE 型 OP 启动时）
 *         ISSUED ← lun_drive 的 ISSUE→ISSUED 转场
 *         FETCH  ← nand_dispatch_task_fc_start（非 ISSUE 型 OP 启动时）
 *   读取：ISSUE  → lun_drive，判"只是发完了，别报完成"
 *         ISSUED → lun_step，判"走 issued_advance 等最短忙期"
 *         FETCH  → 无显式读；落到驱动循环继续 step 即是 FETCH 语义
 *
 * 注意：ISSUE 只有一个写入点，且藏在 task_fc_start 内部。读 lun_drive
 *   里那句 `if (queue->phase == LUN_PHASE_ISSUE)` 时，该值是同一调用链
 *   上方 task_fc_start 刚写进去的，不是别处遗留的状态。
 */

/*
 * Per-LUN 待处理 FIFO（深 NAND_DISPATCH_CHAIN_DEPTH）。
 *
 * **每槽 = 一条命令**，没有链标记：一条链 = 连续 N 槽，由 FC 侧
 * nand_dispatch_submit_chain() 原子批量追加保证「连续 + 不可插入」。
 *
 *   head  —— 出队游标，恒指向当前执行/待执行槽
 *   count —— 占用槽数（0 = LUN 空闲）；tail = (head + count) % DEPTH
 *   phase —— 当前执行槽的 LUN_PHASE_*
 *
 * 与旧「执行中的链」模型的区别：count 不再兼作链深/忙标志；NAND 侧看不到「链」，
 * 逐条完成、逐条出队。poll 遍历每 LUN 每轮只推进一次，完成一条即回一份 completion。
 *
 * 注意：count 仍是"LUN 忙"的唯一判据（poll 只靠 count!=0 判断该 LUN 有在途任务）；
 *   但没有"命令已离开队列、尚未完成"的中间态了——命令从入队到完成始终留在
 *   本结构里，不存在旧模型"通道队列空 + count>0"的悬空窗口。
 */
typedef struct
{
    nand_dispatch_task_t tasks[NAND_DISPATCH_CHAIN_DEPTH];
    uint8_t              head;              /* 出队游标（当前执行/待执行槽） */
    uint8_t              count;             /* 占用槽数 */
    uint8_t              phase;             /* 当前执行槽的 LUN_PHASE_* */
    uint32_t             wait_start_cycles; /* Phase V: ISSUE 完成时 DWT 时间戳 */
} nand_dispatch_lun_queue_t;

/* ======================================================================
 * 函数指针表类型定义
 * ====================================================================== */

/* 异步 request 构建 handler：用 task 的预解析数据组装 nand_fc_request_t。
 * 无返回码——task 的字段在 nand_dispatch_task_load() 时已全部就绪且不变量已断言，
 * 构建不存在失败路径；调用点因此可以无条件接续 nand_fc_start。 */
typedef void (*nand_dispatch_req_handler_t)(nand_dispatch_task_t *task);

/* Completion 策略 handler：根据不同操作类型设置 cw_status 和 failed_descriptor_id。 */
typedef void (*nand_dispatch_completion_handler_t)(
    nand_dispatch_task_t       *task,
    uint8_t                     result,
    const nand_fc_completion_t *nand_completion);

/* OP_COUNT 项，OP_* 直接作为下标。 */
extern const nand_dispatch_req_handler_t        g_nand_dispatch_req_table[OP_COUNT];
extern const nand_dispatch_completion_handler_t g_nand_dispatch_completion_table[OP_COUNT];

#endif /* NAND_DISPATCH_INTERNAL_H */
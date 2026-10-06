/*
 * nand_dispatch_internal.h
 *
 * 内部数据结构，不对外暴露。
 * 由 nand_dispatch.c / nand_dispatch_tables.c 共享。
 */

#ifndef NAND_DISPATCH_INTERNAL_H
#define NAND_DISPATCH_INTERNAL_H

#include "fc/fc_command.h"
#include "fc/fc_types.h"
#include "nand_config.h"
#include "nand_dispatch.h"
#include "nand_fc_interface.h"
#include "nand_fmc.h"
#include "onfi_1_0.h"

#include <stdint.h>

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
    uint8_t  cw_index;            /* 链槽位游标（0-based，跨 plane 的 plane-major 展开位），不是 CW 号。 */
    uint8_t  cw_count[PLANE_NUM]; /* 每 plane 的 CW 数（0 .. CW_NUM）。flat 掩码下可不对称、可为 0。
                                   * 槽位 → (plane, ordinal) 由 nand_dispatch_slot_to_plane_cw() 在
                                   * 本数组上走减得出（不再用除法/取模）。 */
    uint8_t plane_count;          /* 0 = 单 plane（FC 只对 multi-plane 分支赋值），N = multi-plane。
                                   * 见 nand_dispatch.h 同名字段的完整说明。 */
    uint8_t plane;                /* 本命令目标物理 plane 索引（cw_paa/cw_buffers 第一维的语义）。
                                   * 单 plane：= request->plane，所有单 plane req_* handler / cw_chain /
                                   *   scatter 一律读 cw_paa[plane][*] / cw_buffers[plane][*]。
                                   * multi-plane：恒 0；multi-plane handler 不读本字段，仍写死 [0]/[1]。
                                   * ⚠ 必须无条件赋值：task slot 会被链内后续命令复用（task_load 不做整体
                                   *   memset），漏赋值会读到上一条命令的残值。 */
    uint8_t started;              /* 0=未启动；1=已 nand_fc_start（含 start 内同步完成） */
    uint8_t feature_addr;         /* SET/GET_FEATURES 的寄存器地址（column）。内联数据走 cw_buffers，不走本层。 */

    /* Pre-resolved per-CW data. 第一维 = 物理 plane 索引（不是"第几组槽位"）。
     * 定容数组：容量 = PLANE_NUM×CW_NUM = 2×4，实际有效条数由 cw_count / plane_count
     * 给出——本项目无动态内存，一律按硬件上限预留。
     * 单 plane 命令只填 cw_paa[plane][*] / cw_buffers[plane][*]；其余行是确定性零值：
     * FC 侧 fc_build_dispatch_request() 先 *request = {0} 再按 plane 落位，而 task_load
     * 无条件照拷全部 PLANE_NUM 行，故非目标行的 cw_paa == 0 / cw_buffers == NULL，
     * 不是未初始化内存。multi-plane 命令两行都有效（plane 恒 0，uop 硬编码 row[0]/row[1]）。
     * ⚠ 不要与 nand_dispatch_completion_t.cw_status[] 的索引混同：后者是**desc 链序号**
     *   （plane-major 链走位，flat 掩码下每 plane 的 CW 数可不同），与这里的物理 plane 索引
     *   是两套独立坐标。单 plane 打 plane 1 时二者概念分叉。 */
    nand_paa_t cw_paa[PLANE_NUM][CW_NUM];
    uint8_t   *cw_buffers[PLANE_NUM][CW_NUM];

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
/*
 * fc_nand_dispatch.c — NAND 命令分发器
 *
 * 职责：接收 FC 侧预解析的多命令请求（fc_nand_dispatch_req_s），
 *       送入目标 LUN 的待处理 FIFO，由 poll 驱动 NAND 引擎执行：
 *         FC 预解析 → submit/submit_chain 入队 → poll 出队 + 执行 + 回 completion
 *
 * 层次关系：
 *   上层 FC（fc_interface.h）→ 本文件（dispatch）
 *     → fc_nand_start/step（fc_nand_engine.c）→ fc_uop_engine → NAND PHY（nand_fmc.c）
 *
 * 执行模型：
 *   - 单线程 poll 推进模型，无锁；依赖外部串行调用 poll。
 *   - submit/submit_chain 仅"登记"命令（按 room 追加 + 预加载 task），
 *     实际执行发生在后续 poll。
 *   - **每 LUN 一个待处理 FIFO（深 NAND_DISPATCH_CHAIN_DEPTH）**，每槽 = 一条命令；
 *     NAND 侧看不到"链"——一条链 = 连续 N 槽（FC 原子批量追加）。
 *     poll 遍历每 LUN FIFO 先进先出，每轮每 LUN 推进一条，完成一条即回一份 completion。
 *   - 相位机（多 LUN 相位机）：IDLE → ISSUE → ISSUED → FETCH → IDLE（per-LUN，作用于队头槽）。
 *
 * 对外接口：fc_nand_dispatch_init / submit / submit_chain / poll / lun_free。
 * 异步放行闩锁的置位/查询在 fc_nand_engine.h（引擎执行模式）。
 */

#include "fc_nand_dispatch.h"

#include "assertion.h"
#include "delay.h"
#include "fc_interface.h"
#include "fc_nand_engine.h"
#include "nand_interface.h"
#if defined(FC_NAND_TEST_ENABLE)
#    include "fault_inject.h"
#endif
#include "log.h"
#include "nand_debug.h"
#include "ssd_compiler.h"

#include <stddef.h>
#include <string.h>

/*
 * LUN 相位枚举 (多 LUN 相位机 multi-LUN ISSUE/WAIT/FETCH phase machine)
 *   IDLE  → ISSUE → ISSUED → FETCH → IDLE
 *   ISSUE ：发完命令立即返回，LUN 进入 tR/tPROG/tBERS
 *   ISSUED：SysTick 计时最短忙期，到点后启动配对 WAIT/FETCH
 *   FETCH ：执行 WAIT_READY + 数据 + 状态检查
 */
#define LUN_PHASE_IDLE (0U) /* LUN 空闲 */

#define LUN_PHASE_ISSUE (1U) /* 正在执行 ISSUE uop */

#define LUN_PHASE_ISSUED (2U) /* ISSUE 完成，LUN 内部操作中（tR/tPROG/tBERS） */

#define LUN_PHASE_FETCH (3U) /* 正在执行 WAIT/FETCH uop */

/* ── dispatch task：一次预解析后的命令实例 ──
 * 由 fc_nand_dispatch_task_load() 从 fc_nand_dispatch_req_s 填充，
 * poll 依此驱动 fc_nand_start/step。 */
typedef struct FC_NAND_DISPATCH_TASK_S
{
    WORD  cmd_id;
    BYTE  operation;
    BYTE  ch;            /* Channel — needed for page_buffer access. */
    BYTE  ce;            /* Chip-enable — needed for routing. */
    BYTE  lun;           /* LUN — needed for routing. */
    BYTE  page_type : 2; /* Copy from dispatch request; 当前无读者，LDPC decode threshold 预留。 */
    BYTE  reserved  : 6;
    DWORD cw_bitmap;      /* 选中的 CW 集合：bit n = plane*CW_NUM + cw（flat 绝对编号）。
                           * 与 request->cw_bitmap 同一值；是 cw_buffers / cw_index 的唯一依据
                           * （"本任务有几个 CW"= popcount(mask)，"第 k 个"= 第 k 个置位）。 */
    BYTE cw_index;        /* 当前 CW 的 **flat 绝对编号 n**（∈ cw_bitmap），不是密排槽位。
                           * 生产：task_load 置为掩码最低位；cw_chain 每取完一个推进到下一个置位。
                           * 消费：completion_per_cw 用它当下标写 cw_status[n]。 */
    BYTE plane_count;     /* 0 = 单 plane（FC 只对 multi-plane 分支赋值），N = multi-plane。
                           * 见 fc_nand_dispatch.h 同名字段的完整说明。 */
    BYTE plane;           /* 本命令目标物理 plane 索引。
                           * 单 plane：= request->plane，所有单 plane req_* handler / cw_chain /
                           *   scatter 一律读 plane_paa[plane]。
                           * multi-plane：恒 0；multi-plane handler 不读本字段，仍写死 [0]/[1]。
                           * ⚠ 必须无条件赋值：task slot 会被链内后续命令复用（task_load 不做整体
                           *   memset），漏赋值会读到上一条命令的残值。 */
    BYTE started;         /* 0=未启动；1=已 fc_nand_start（含 start 内同步完成） */
    BYTE target_acquired; /* 1 = 本任务持有 target 所属 bank 的 ownership */
    BYTE feature_addr;    /* SET/GET_FEATURES 的寄存器地址（column）。内联数据走 cw_buffers，不走本层。 */
    WORD column;          /* **显式列地址**（仅 OP_READ_COLUMN 用，见 fc_nand_dispatch.h 同名字段）。
                           * 其余 op 恒 0；req_read_column 直接把它当列地址，不从 paa 的 cw 推。 */

    /* Pre-resolved per-CW data（定容数组，容量 = 硬件上限 CW_TOTAL_MAX = PLANE_NUM×CW_NUM）。
     * ⚠ 下标是 **flat 绝对 CW 号 n = plane*CW_NUM + cw**，与 cw_bitmap 的 bit 号、
     *   completion.cw_status[] 的下标三者同一空间——"哪些格有效"完全由 cw_bitmap 决定，
     *   不在掩码里的格是确定性零值（request 整体 {0} 初始化后只填掩码内的格）。
     *   物理 plane 由 n / CW_NUM 得出，CW 号由 n % CW_NUM 得出，只存一份。 */
    DWORD plane_paa[PLANE_NUM];     /* 各 plane 的行地址承载 PAA（CW 位被 paa_to_nand_row 丢弃）。 */
    BYTE *cw_buffers[CW_TOTAL_MAX]; /* [n]，仅 n ∈ cw_bitmap 有效。 */

    /* ── 引擎上下文四件套（多 LUN 相位机）──
     * 全部随 task slot 常驻，跨多次 poll 调用存活；生产/消费阶段见各字段。 */

    /* 本次 ONFI 事务的请求参数（column/row/data/data_length）。
     * 生产：fc_nand_dispatch_task_start() → fc_nand_dispatch_task_build_request() 经
     *       g_fc_nand_dispatch_req_table[operation] 分派，各 req_* handler 在
     *       fc_nand_dispatch.c 里整块赋值；CW 链续读时由 fc_nand_dispatch_task_cw_chain()
     *       覆写（改用 OP_RANDOM_DATA_READ + CW 偏移列地址）。
     * 消费：fc_nand_start() —— 拷入 transaction->request 后交给引擎。 */
    nand_cmd_req_s request;

    /* 引擎侧事务状态（UOP 游标 + 三指针 + 引擎完成项）。
     * 生产：fc_nand_dispatch_task_load() 一次性绑定 engine 三指针（地址随事务固定，只绑一次）；
     *       fc_nand_start() 重置 transaction->request/completion 并 fc_uop_engine_start()；
     *       此后由 fc_uop_engine_step() 逐 uop 推进。
     * 消费：fc_nand_step() 推进；engine.active 判"start 内同步完成"；
     *       transaction.completion.nand_status 逐处抄进 ctx->nand_completion。
     * 这是 ISSUE 发完即返回后、tR/tPROG/tBERS 期间"记住发到哪了"的唯一载体。 */
    fc_nand_txn_s transaction;

    /* 调度层完成项（对 FC 上报的最终结果）。
     * 生产：fc_nand_dispatch_task_load() 初始化；fc_nand_dispatch_task_complete() 写
     *       execution_status/nand_status 后按 operation 分派 completion handler 填 CW 位。
     * 消费：fc_nand_dispatch_lun_step() 在每条命令收尾时 *completion = task->completion
     *       并出队（每槽 = 一条命令 = 一份 completion）。
     * 注意：与 transaction.completion（引擎层 nand_cmd_comp_s）是两个不同对象，
     *       仅名字撞车——本字段才是给上层的。 */
    fc_nand_dispatch_comp_s completion;
} fc_nand_dispatch_task_s;

/*
 * 相位写入点 / 读取点（均在 src/fc/fc_nand_dispatch.c）——读相位机代码先看这张表：
 *   写入：IDLE   ← submit / submit_chain（新槽） / lun_step（一条命令出队后复位）
 *         ISSUE  ← 仅 fc_nand_dispatch_task_fc_start（ISSUE 型 OP 启动时）
 *         ISSUED ← lun_drive 的 ISSUE→ISSUED 转场
 *         FETCH  ← fc_nand_dispatch_task_fc_start（非 ISSUE 型 OP 启动时）
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
 * fc_nand_dispatch_submit_chain() 原子批量追加保证「连续 + 不可插入」。
 *
 *   head  —— 出队游标，恒指向当前执行/待执行槽
 *   count —— 占用槽数（0 = LUN 空闲）；tail = (head + count) % DEPTH
 *   phase —— 当前执行槽的 LUN_PHASE_*
 *
 * count 只表示本任务 CW 计数，不兼作链深/忙标志；NAND 侧看不到「链」，
 * 逐条完成、逐条出队。poll 遍历每 LUN 每轮只推进一次，完成一条即回一份 completion。
 *
 * 注意：count 是“LUN 忙”的唯一判据（poll 只靠 count != 0 判断该 LUN 有在途任务）。
 * 命令从入队到完成始终留在本结构里；不存在“命令已离开队列、尚未完成”的中间态，
 * 也不存在“队列空 + count > 0”的悬空窗口。
 */
typedef struct FC_NAND_DISPATCH_LUN_QUEUE_S
{
    fc_nand_dispatch_task_s tasks[NAND_DISPATCH_CHAIN_DEPTH];
    BYTE                    head;          /* 出队游标（当前执行/待执行槽） */
    BYTE                    count;         /* 占用槽数 */
    BYTE                    phase;         /* 当前执行槽的 LUN_PHASE_* */
    DWORD                   wait_start_us; /* 多 LUN 相位机: ISSUE 完成时 SysTick µs 时间戳 */
} fc_nand_dispatch_lun_queue_s;

/* ======================================================================
 * 函数指针表类型定义
 * ====================================================================== */

/* 异步 request 构建 handler：用 task 的预解析数据组装 nand_cmd_req_s。
 * 无返回码——task 的字段在 fc_nand_dispatch_task_load() 时已全部就绪且不变量已断言，
 * 构建不存在失败路径；调用点因此可以无条件接续 fc_nand_start。 */
typedef void (*fc_nand_dispatch_req_handler_fn_t)(fc_nand_dispatch_task_s *task);

/* Completion 策略 handler：根据不同操作类型设置 cw_status。 */
typedef void (*fc_nand_dispatch_completion_handler_fn_t)(fc_nand_dispatch_task_s *task, BYTE result);

/* ======================================================================
 * poll — 推进各 LUN FIFO 队头事务
 *
 * 遍历模型（每 LUN 一个 FIFO，先进先出）：
 *   - 外层 round-robin 遍历通道（从上次返回完成的通道的下一个续走）。
 *   - 每 LUN 每轮只推进一次（fc_nand_dispatch_lun_step）：未启动则启动，已启动则 step；
 *     完成一条命令即回一份 completion 并出队（head++/count--）。
 *   - 队列空（count==0）的 LUN 直接跳过。
 *
 * 相位机（多 LUN 相位机，作用于队头槽）：IDLE → ISSUE → ISSUED → FETCH → IDLE
 *   ISSUE：发完命令即返回，LUN 进入 tR/tPROG/tBERS
 *   ISSUED：SysTick 计时最短忙期，到点后启动配对 WAIT/FETCH
 *   FETCH：执行 WAIT_READY + 数据 + 状态检查
 *
 * 完整状态机图见 docs/design/fc_nand_design.md §23（图 20）。
 *
 * 本段函数按职责组织：
 *   Command issue  —— task_fc_start / task_start
 *   Per-LUN FSM    —— issued_advance / lun_drive / lun_step
 *   Data path      —— task_cw_chain / scatter_page_buffer
 * ====================================================================== */

/* ── 单 LUN 单步推进结果 ── */
typedef enum FC_NAND_DISPATCH_LUN_OUTCOME_E
{
    NAND_DISPATCH_LUN_WAIT = 0, /* 本轮无进展，调用方看下一 LUN */
    NAND_DISPATCH_LUN_DONE      /* 队头命令完成，*completion 已回填并出队 */
} fc_nand_dispatch_lun_outcome_e;

/* ── 命令发起结果：req_table 构建无失败路径（见 fc_nand_dispatch_req_handler_fn_t），
 *    故只剩"异步待下轮 step"与"同步完成/带错完成"两种。注意 fc_nand_start 返回的
 *    INVALID_ARGUMENT/TIMEOUT/OPERATION_FAILED 不是失败出口，而是"带错误状态的完成"，
 *    必须继续走完成处理链。 ── */
typedef enum FC_NAND_DISPATCH_START_OUTCOME_E
{
    NAND_DISPATCH_START_ASYNC = 0, /* fc_nand_start 返回 NOT_READY，待下轮 step */
    NAND_DISPATCH_START_SYNC,      /* 同步完成或返回错误码，*fc_result 有效 */
    NAND_DISPATCH_START_WAIT       /* target/bank 被占用，本任务尚未启动 */
} fc_nand_dispatch_start_outcome_e;

/* ── CW 链式续读结果（无 ERROR 出口：链内 start 错误仍走完成路径） ── */
typedef enum FC_NAND_DISPATCH_CW_OUTCOME_E
{
    NAND_DISPATCH_CW_ASYNC = 0, /* 下一 CW 异步启动，本轮到此为止 */
    NAND_DISPATCH_CW_DONE       /* 链式续读结束 */
} fc_nand_dispatch_cw_outcome_e;

/* ── 单 LUN 上下文。不缓存 task 指针——出队后旧指针失效，
 *    一律由 &queue->tasks[queue->head] 现取（head 在单步内不变）。
 *    也不缓存 ch/ce/lun：底层 task 自带这三项（见 fc_nand_dispatch_task_s），
 *    需要时经 task 取即可，本层不重复。 ── */
typedef struct FC_NAND_DISPATCH_LUN_CONTEXT_S
{
    fc_nand_dispatch_comp_s      *completion; /* 完成输出目标 */
    fc_nand_dispatch_lun_queue_s *queue;
    nand_cmd_comp_s               nand_completion; /* 读前必写（active==0 分支依赖同轮先写） */
    BYTE                          result;
} fc_nand_dispatch_lun_ctx_s;

/* OP_COUNT 项，OP_* 直接作为下标。 */
extern const fc_nand_dispatch_req_handler_fn_t        g_fc_nand_dispatch_req_table[OP_COUNT];
extern const fc_nand_dispatch_completion_handler_fn_t g_fc_nand_dispatch_completion_table[OP_COUNT];

/* ======================================================================
 * 全局状态
 * ====================================================================== */

/* 每 LUN 待处理 FIFO：submit 按 room 追加，poll 逐条排空。
 * 每槽 = 一条命令；链 = 连续槽（由 submit_chain 原子批量追加保证）。 */
static fc_nand_dispatch_lun_queue_s g_fc_nand_dispatch_queues[CH_NUM][CE_NUM][LUN_NUM];

/* 异步放行闩锁由 fc_nand_engine.c 持有（它表达引擎的执行模式）。
 * 本层只查询，不定义——dispatch → engine 单向依赖。 */
/* 上次有完成产出的通道的下一个通道；下一轮从这里开始扫描，形成通道 round-robin。 */
static BYTE s_poll_next_channel;

/* 不可继续的调度层错误（当前：µs 时间源不可用，无法执行最短忙期等待）。
 * fc.c 每轮 poll 后查询，转成控制器 fatal。 */
static BOOL s_dispatch_fatal;

/* 页缓冲固定在 AXI_NOCACHE（与基线一致）。
 * 板端实验：放到 DTCM 时 multi-plane program 的 handler 收到了正确的
 * bitmap/data，但 NAND 读回全 FF；回到 AXI_NOCACHE 后 format 正常。
 * DMA=1 时同样要求 noncacheable（DMA1/2 不能访问 DTCM）。
 * 按 (ch, ce, lun) 各持一块——同通道各 LUN 由 poll 轮流驱动，且 request.buffer
 * 指针会跨多次 poll 存活（如 program ISSUE 卡在 70h 忙轮询），共享一块会被后驱动的
 * LUN 覆盖（详见 fc_nand_dispatch.c 顶部注释）。
 * 多 plane 时拼接双页（plane stride = PAGE_SIZE），scatter 拆页依赖它。 */
BYTE g_nand_channel_page_buffer[CH_NUM][CE_NUM][LUN_NUM][PAGE_BUFFER_SIZE] NONCACHEABLE_BUFFER;

/* ======================================================================
 * 函数指针表定义
 * ====================================================================== */

static fc_nand_dispatch_start_outcome_e fc_nand_dispatch_task_fc_start(fc_nand_dispatch_lun_queue_s *queue, fc_nand_dispatch_task_s *task, BYTE *fc_result);

static inline BYTE fc_nand_dispatch_cw_next(DWORD cw_bitmap, BYTE n);

/* mask 最低位的绝对编号；mask 为空（退化命令）时返回 0 —— 只用于"取一个不越界的下标"，
 * 需要区分"空掩码"时用 fc_nand_dispatch_cw_next() 的哨兵。 */
static inline BYTE fc_nand_dispatch_cw_first_or_zero(DWORD cw_bitmap)
{
    BYTE n = fc_nand_dispatch_cw_next(cw_bitmap, 0U);

    return (n < CW_TOTAL_MAX) ? n : 0U;
}

/* 单 plane 命令的完整页位图：该 plane 的全部 CW。 */
static BOOL fc_nand_dispatch_bitmap_is_full_single(DWORD cw_bitmap, BYTE plane)
{
    const DWORD full = ((1UL << CW_NUM) - 1UL) << (plane * CW_NUM);

    return (cw_bitmap == full);
}

/* 多 plane 命令的完整页位图：全部 plane × 全部 CW。 */
static BOOL fc_nand_dispatch_bitmap_is_full_multi(DWORD cw_bitmap)
{
    return (cw_bitmap == ((1UL << (PLANE_NUM * CW_NUM)) - 1UL));
}

/* ======================================================================
 * 辅助函数
 * ====================================================================== */

_Static_assert(FC_CW_STATUS_UNUSED == 0U, "completion_init relies on the unused status being zero");

/**
 * @brief 初始化一个 completion 结构（全清零 + 命令关联键）
 *
 * `FC_CW_STATUS_UNUSED` 当前为 0，memset 已把它写入全部 CW 槽位；不另做
 * 逐槽赋值，避免“清零后又写一遍 0”的伪工作。
 *
 * @param completion 目标
 * @param cmd_id FC 关联键
 */
static void fc_nand_dispatch_completion_init(fc_nand_dispatch_comp_s *completion, WORD cmd_id)
{
    memset(completion, 0, sizeof(*completion));
    completion->cmd_id = cmd_id;
}

/* 释放本任务持有的 target/bank ownership；幂等。 */
static void fc_nand_dispatch_target_release(fc_nand_dispatch_task_s *task)
{
    if (task->target_acquired != 0U)
    {
        nand_board_release_target(task->ch, task->ce);
        task->target_acquired = 0U;
    }
}

/**
 * @brief 构建 request 并发起事务（命令启动唯一入口，含日志点）
 *
 * @param queue     所属 LUN FIFO
 * @param task      目标任务
 * @param fc_result 输出：fc_nand_start 返回码
 * @return ASYNC / SYNC
 */
static fc_nand_dispatch_start_outcome_e fc_nand_dispatch_task_start(fc_nand_dispatch_lun_queue_s *queue, fc_nand_dispatch_task_s *task, BYTE *fc_result)
{
    g_fc_nand_dispatch_req_table[task->operation](task);

    DISP_LOGD("FC dispatch op=%u buffer=%p length=%u column=0x%04X row=0x%06lX\r\n", task->operation, (void *)task->request.buffer, task->request.data_length, task->request.column, task->request.row[0]);

    return fc_nand_dispatch_task_fc_start(queue, task, fc_result);
}

/**
 * @brief 是否为「页已加载进页缓冲」的 op
 *
 * @param operation OP_*
 * @return 1 是；0 否
 */
static BYTE fc_nand_dispatch_is_load_complete_op(BYTE operation)
{
    return ((operation == OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT) || (operation == OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT) || (operation == OP_READ_PAGE_WAIT)) ? 1U : 0U;
}

/**
 * @brief 是否为「从页缓冲逐 CW 取数」的 op
 *
 * @param operation OP_*
 * @return 1 是；0 否
 */
static BYTE fc_nand_dispatch_is_fetch_op(BYTE operation)
{
    return ((operation == OP_RANDOM_DATA_READ) || (operation == OP_RANDOM_DATA_READ_MULTI_PLANE)) ? 1U : 0U;
}

/* ======================================================================
 * Completion 策略 handler
 * ====================================================================== */

/* --------------------------------------------------------------------
 * 全局变量 g_nand_channel_page_buffer
 *   页缓冲，物理位置在 noncacheable_buffer 段（DMA 直接访问，
 *   DCache 关闭）。FC 侧 data path 与 NAND 引擎共用。
 *   按 (ch, ce, lun) 各持一块：同一通道的多个 LUN 由 Phase 1 轮流驱动
 *   （fc_nand_dispatch_poll 的 ce/lun 双层循环），而 request.buffer 一旦指向本缓冲
 *   就跨多次 poll 存活（如 program ISSUE 先被 70h 忙轮询挡下，数据已 memcpy
 *   进缓冲却要等下一轮才 DATA_IN）。若全通道共用一块，后驱动的 LUN 会把先
 *   驱动的 LUN 尚未搬走的数据覆盖掉——故必须带 ce/lun 维度。
 *   多 plane 操作时保存双页数据（PAGE_BUFFER_SIZE = 2112×2），
 *   由 req_* 拼接写入 / poll 拆回各 CW buffer。
 *   本文件定义并在 req_* / poll 中引用。
 * -------------------------------------------------------------------- */

/**
 * @brief 清零 request 并填非行地址字段
 *
 * row[] 由各 handler 按 plane 填写；未用到的 plane 保持 0。
 *
 * @param task        任务
 * @param column      列地址
 * @param data        数据缓冲
 * @param data_length 数据长度
 */
static void req_init(fc_nand_dispatch_task_s *task, WORD column, BYTE *data, WORD data_length)
{
    memset(&task->request, 0, sizeof(task->request));
    task->request.column      = column;
    task->request.buffer      = data;
    task->request.data_length = data_length;
}

/**
 * @brief 仅标记当前处理的 CW（逐 CW 取数链）
 *
 * @param task           正在收尾的任务
 * @param result         NAND_RESULT_* 执行结果
 */
static void completion_per_cw(fc_nand_dispatch_task_s *task, BYTE result)
{
    BYTE status = (result == NAND_RESULT_OK) ? FC_CW_STATUS_UNUSED : FC_CW_STATUS_FAIL;

    task->completion.cw_status[task->cw_index] = status;
}

/**
 * @brief 构建单 plane CACHE 读请求
 *
 * 整页读入 page_buffer（data_length = PAGE_SIZE），随后由 poll 拆回各 CW。
 *
 * @param task 预解析完成的任务
 */
static void req_cache_read(fc_nand_dispatch_task_s *task)
{
    BYTE *page_buffer = g_nand_channel_page_buffer[task->ch][task->ce][task->lun];

    memset(page_buffer, 0x00, PAGE_SIZE);
    req_init(task, 0U, page_buffer, PAGE_SIZE);
    task->request.row[0] = paa_to_nand_row(task->plane_paa[task->plane]);
}

/**
 * @brief 构建单 plane 块擦除请求（无数据传输）
 *
 * @param task 预解析完成的任务
 */
static void req_erase(fc_nand_dispatch_task_s *task)
{
    req_init(task, 0U, NULL, 0U);
    task->request.row[0] = paa_to_nand_row(task->plane_paa[task->plane]);
}

/**
 * @brief 构建 SET/GET_FEATURES 请求
 *
 * column = feature_addr，data_length 固定 4 字节；Feature 是 die 级，固定用本
 * plane 的 CW0 那一格。
 *
 * @param task 预解析完成的任务
 */
static void req_feature(fc_nand_dispatch_task_s *task)
{
    req_init(task, task->feature_addr, task->cw_buffers[fc_cw_flat(task->plane, 0U)], 4U);
}

/**
 * @brief 构建 META 只读回读请求（OP_READ_META）
 *
 * 列地址 = paa_to_cw(plane_paa[plane]) * CW_SIZE；4 字节落本 plane 的 CW0 格。
 *
 * @param task 预解析完成的任务
 */
static void req_meta_read(fc_nand_dispatch_task_s *task)
{
    const BYTE plane = task->plane;

    req_init(task, (paa_to_cw(task->plane_paa[plane]) * CW_SIZE), task->cw_buffers[fc_cw_flat(plane, 0U)], 4U);
    task->request.row[0] = paa_to_nand_row(task->plane_paa[plane]);
}

/**
 * @brief 构建按列读请求（OP_READ_COLUMN）
 *
 * 列地址来自 task->column（可够到 spare 区），行地址取 plane_paa。
 *
 * @param task 预解析完成的任务
 */
static void req_read_column(fc_nand_dispatch_task_s *task)
{
    const BYTE plane = task->plane;

    req_init(task, task->column, task->cw_buffers[fc_cw_flat(plane, 0U)], 4U);
    task->request.row[0] = paa_to_nand_row(task->plane_paa[plane]);
}

/**
 * @brief 构建 ONFI 签名读请求（90h + 地址 20h）
 *
 * 列地址恒 0x20；4 字节落本 plane 的 CW0 格。
 *
 * @param task 预解析完成的任务
 */
static void req_onfi_signature(fc_nand_dispatch_task_s *task)
{
    req_init(task, NAND_READ_ID_ADDR_ONFI_SIGNATURE, task->cw_buffers[fc_cw_flat(task->plane, 0U)], 4U);
}

/**
 * @brief 构建固定长度数据传输请求（READ_ID / PARAMETER_PAGE / RANDOM 等）
 *
 * 数据长度取自 g_nand_operation_map；缓冲取掩码最低位那一格。
 *
 * @param task 预解析完成的任务
 */
static void req_single(fc_nand_dispatch_task_s *task)
{
    WORD data_length = g_nand_operation_map[task->operation].data_length;

    req_init(task, 0U, task->cw_buffers[fc_nand_dispatch_cw_first_or_zero(task->cw_bitmap)], data_length);
    task->request.row[0] = paa_to_nand_row(task->plane_paa[task->plane]);
}

/**
 * @brief 构建页缓冲随机读请求（05h-E0h / 06h-E0h）
 *
 * 列地址 = (n % CW_NUM) * CW_SIZE；行地址取 plane_paa[n / CW_NUM]。
 *
 * @param task 预解析完成的任务
 */
static void req_random_read(fc_nand_dispatch_task_s *task)
{
    BYTE n = fc_nand_dispatch_cw_first_or_zero(task->cw_bitmap);

    req_init(task, ((n % CW_NUM) * CW_SIZE), task->cw_buffers[n], CW_SIZE);
    task->request.row[0] = paa_to_nand_row(task->plane_paa[n / CW_NUM]);
}

/**
 * @brief 构建无数据传输请求（RESET / 块锁 / 块解锁等）
 *
 * @param task 预解析完成的任务
 */
static void req_no_data(fc_nand_dispatch_task_s *task)
{
    req_init(task, 0U, NULL, 0U);
    task->request.row[0] = paa_to_nand_row(task->plane_paa[task->plane]);
}

/**
 * @brief 构建多 plane 块擦除请求（无数据传输，双行地址）
 *
 * @param task 预解析完成的任务
 */
static void req_multi_plane_erase(fc_nand_dispatch_task_s *task)
{
    BYTE plane;

    req_init(task, 0U, NULL, 0U);

    /* ⚠ 上界写死 PLANE_NUM，不看 task->plane_count：本 handler 只由 multi-plane OP
     *   选中，那条路径上 plane_count 恒 = PLANE_NUM。改 plane 语义时两处要一起动。
     * row[PLANE_NUM..NAND_PLANE_MAX-1] 由 req_init 的 memset 零填充。 */
    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        task->request.row[plane] = paa_to_nand_row(task->plane_paa[plane]);
    }
}

/* ── CW 掩码遍历 ──
 * flat CW 掩码（bit n = plane*CW_NUM + cw）是"哪些 CW 要访问"的唯一依据，
 * 也是推进顺序的唯一依据。两个小工具只在本文件内使用，故放本文件 `static inline`。 */

/* mask 中 ≥ n 的第一个置位的绝对编号；没有则返回 CW_TOTAL_MAX（哨兵，越界即"到头"）。
 * 用法：`for (k = fc_nand_dispatch_cw_next(mask, 0U); k < CW_TOTAL_MAX; k = fc_nand_dispatch_cw_next(mask, k + 1U))`。 */
static inline BYTE fc_nand_dispatch_cw_next(DWORD cw_bitmap, BYTE n)
{
    while (n < CW_TOTAL_MAX)
    {
        if ((cw_bitmap & (1UL << n)) != 0UL)
        {
            return n;
        }
        n++;
    }
    return CW_TOTAL_MAX;
}

/* ── 按位图选择读路径 ──
 * 非稀疏（完整页）且单 LUN：保留 WITH_DATA_OUT，30h 后直接整页 Dout。
 * 稀疏，或多 LUN：改成加载型，数据由后续 05h-E0h / 06h-E0h 逐 CW 取。
 * UOP 表本身不拆；这里只决定用哪个 op 进入引擎。 */
static BYTE fc_nand_dispatch_select_read_op(BYTE operation, DWORD cw_bitmap, BYTE plane, BYTE plane_count)
{
    switch (operation)
    {
        case OP_NORMAL_READ_PAGE_WITH_DATA_OUT:
            if ((LUN_NUM == 1U) && (fc_nand_dispatch_bitmap_is_full_single(cw_bitmap, plane) == true))
            {
                return operation;
            }
            return (LUN_NUM > 1U) ? OP_READ_PAGE_LOAD : OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT;

        case OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT:
            if ((LUN_NUM == 1U) && (plane_count > 0U) && (fc_nand_dispatch_bitmap_is_full_multi(cw_bitmap) == true))
            {
                return operation;
            }
            return (LUN_NUM > 1U) ? OP_READ_PAGE_MULTI_PLANE_LOAD : OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT;

        case OP_OTP_DATA_READ:
            if ((LUN_NUM == 1U) && (fc_nand_dispatch_bitmap_is_full_single(cw_bitmap, plane) == true))
            {
                return operation;
            }
            return (LUN_NUM > 1U) ? OP_READ_PAGE_LOAD : OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT;

        default:
            return operation;
    }
}

/**
 * @brief 统一收尾：写状态后按 operation 分派到 completion 策略 handler
 *
 * @param task           完成任务
 * @param result         执行结果
 * @param nand_completion NAND 完成
 */
static void fc_nand_dispatch_task_complete(fc_nand_dispatch_task_s *task, BYTE result, const nand_cmd_comp_s *nand_completion)
{
    BYTE effective = result;

#if defined(FC_NAND_TEST_ENABLE)
    /* D2 共享 seam：在 completion 策略之前把本次 NAND 执行结果改为失败。
     * 只按 opcode 键；completion 表会据此把选中 CW 全部标 FAIL（引擎级，不是 CQ 出口）。 */
    DWORD result_arg = 0U;

    if (fi_take_ex(FI_FC_ENGINE_FAIL, task->operation, &result_arg) == true)
    {
        effective = (result_arg != 0U) ? (BYTE)result_arg : (BYTE)NAND_RESULT_OPERATION_FAILED;
    }
#endif

    task->completion.execution_status = effective;
    task->completion.nand_status      = nand_completion->nand_status;
    task->completion.pre_status       = nand_completion->pre_status;

    g_fc_nand_dispatch_completion_table[task->operation](task, effective);

#if defined(FC_NAND_TEST_ENABLE)
    /* D2 seam：按 plane 覆盖完成状态。用于 host 写失败“只标 plane0”一类负例：
     * 命令级 result 仍为成功，但指定 plane 的 cw_status 置失败位。 */
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        DWORD arg = 0U;

        if (fi_take_ex(FI_PLANE_STATUS, p, &arg) == true)
        {
            const BYTE st = (arg != 0U) ? (BYTE)arg : (BYTE)FC_CW_STATUS_FAIL;

            for (BYTE n = 0U; n < CW_TOTAL_MAX; n++)
            {
                if (((task->cw_bitmap & (1UL << n)) != 0U) && ((n / CW_NUM) == p))
                {
                    task->completion.cw_status[n] = st;
                }
            }
        }
    }
#endif
}

/**
 * @brief 把 FC 预解析 request 搬到 LUN FIFO 的 task slot
 *
 * 先把所有 cw_buffers 置 NULL，再搬掩码内的项；掩码外由消费点保证不读。
 *
 * @param task    目标 slot
 * @param request 预解析请求
 */
static void fc_nand_dispatch_task_load(fc_nand_dispatch_task_s *task, const fc_nand_dispatch_req_s *request)
{
    BYTE n;

    fc_nand_dispatch_completion_init(&task->completion, request->cmd_id);
    /* 事务状态整体清零：task slot 会被后续命令复用，target 校验失败时
     * fc_nand_start 不会被调用，不能让上一条命令的 completion/active 残留下来。 */
    memset(&task->transaction, 0, sizeof(task->transaction));
    task->cmd_id                        = request->cmd_id;
    task->ch                            = request->ch;
    task->ce                            = request->ce;
    task->lun                           = request->lun;
    /* 非稀疏单 LUN 整页读保留 WITH_DATA_OUT；稀疏/多 LUN 读改成加载型，
     * 选中 CW 由后续 05h/06h 随机读取回。 */
    task->operation                     = fc_nand_dispatch_select_read_op(request->operation,
                                                      request->cw_bitmap,
                                                      request->plane,
                                                      request->plane_count);
    task->page_type                     = request->page_type;
    task->cw_bitmap                     = request->cw_bitmap;
    task->plane_count                   = request->plane_count;
    task->plane                         = request->plane;
    task->started                       = 0U;
    task->target_acquired               = 0U;
    task->feature_addr                  = request->feature_addr;
    task->column                        = request->column;
    /* 引擎上下文三指针：request/completion 指向本 slot 内持久存储；backend 在启动时
     * 由 task_fc_start 经 nand_target_get 一次取得（通路校验 + backend 不可分）。 */
    task->transaction.engine.backend    = NULL;
    task->transaction.engine.request    = &task->transaction.request;
    task->transaction.engine.completion = &task->transaction.completion;
    /* 当前 CW = 掩码最低位：直接下发的取数 op（OP_RANDOM_DATA_READ）靠它定位自己那一位，
     * 加载型 op 靠它定首次取数的起点。空掩码（退化命令）退化为 0，保证
     * completion_per_cw 的下标不越界。 */
    task->cw_index                      = fc_nand_dispatch_cw_first_or_zero(request->cw_bitmap);
    /* 先把所有指针置 NULL，再搬掩码内的 CW buffer。掩码外的格是确定性 NULL，
     * 不会沿用到上一条命令的指针；空掩码（擦除/无数据 op）因此留下全 NULL。 */
    memset(task->cw_buffers, 0, sizeof(task->cw_buffers));
    for (n = fc_nand_dispatch_cw_next(request->cw_bitmap, 0U); n < CW_TOTAL_MAX; n = fc_nand_dispatch_cw_next(request->cw_bitmap, (n + 1U)))
    {
        task->cw_buffers[n] = request->cw_buffers[n];
    }
    memcpy(task->plane_paa, request->plane_paa, sizeof(task->plane_paa));
    /* 单 plane 有效行的归属由生产者保证，此处不做逐项复核：掩码位 n 由 desc 的 PAA 本身
     * 算出（见 fc_build_request_data_planes），"在 n 位写了 buffer"与
     * "该 desc 的 plane/cw 位是 n"是同一条表达式。真正可能错位的情形（desc plane 位
     * 与 cmd->paa 不一致）在造链时就被 fc_command_configure_descriptor 的 plane 段
     * 校验（越段 → ASSERT）拦下，不会走到这里。
     * 擦除位图为空 ⇒ cw_buffers[] 全为 NULL；擦除路径不读它（req_erase 只用 plane_paa）。 */
}

/* ======================================================================
 * submit — 入队（单命令 / 整链）
 *
 * 语义：按 room 追加到目标 LUN FIFO 的尾部。
 *   - 单命令 = count 1 的批。
 *   - 整链 = 原子批量追加：剩余深度不足整链则整体拒收，无副作用。
 *   - 追加后链占据连续槽位、按 FIFO 顺序执行，中间不可被后续命令插入。
 * ====================================================================== */

/**
 * @brief 把一批命令按 room 原子追加到 LUN FIFO
 *
 * 放行闩锁在此检查；剩余深度不足整批则整体拒收、无副作用。
 *
 * @param requests 命令数组（同 ch/ce/lun）
 * @param count    批大小
 * @return OK / INVALID_ARGUMENT 未放行 / NOT_READY 深度不足
 */
static nand_result_e fc_nand_dispatch_enqueue(const fc_nand_dispatch_req_s *requests, BYTE count)
{
    fc_nand_dispatch_lun_queue_s *queue;
    BYTE                          i;

    /* 异步路径放行闩锁：单命令与整链两个入口共用同一道门，结构上不会再分叉。
     * 未放行是运行时状态（非调用者编程错误）→ 保留返回码，不转 ASSERT。 */
    if (fc_nand_engine_is_armed() == 0U)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    /* NAND dispatch 是请求的唯一入口边界：debug 停机 + release 返回 INVALID_ARGUMENT。
     * 整批逐条校验（不只 [0]）：队列按 requests[0] 的 LUN 选取，链成员必须同 LUN；
     * 掩码上界是 completion_* 写 cw_status[n] 的前置。 */
    ASSERT_REJECT(requests != NULL, NAND_RESULT_INVALID_ARGUMENT, "dispatch: requests NULL");
    ASSERT_REJECT((count != 0U) && (count <= NAND_DISPATCH_CHAIN_DEPTH),
                  NAND_RESULT_INVALID_ARGUMENT,
                  "dispatch: count=%hhu out of range",
                  count);
    for (i = 0U; i < count; i++)
    {
        ASSERT_REJECT(requests[i].ch < CH_NUM, NAND_RESULT_INVALID_ARGUMENT, "dispatch ch=%hhu", requests[i].ch);
        ASSERT_REJECT(requests[i].ce < CE_NUM, NAND_RESULT_INVALID_ARGUMENT, "dispatch ce=%hhu", requests[i].ce);
        ASSERT_REJECT(requests[i].lun < LUN_NUM, NAND_RESULT_INVALID_ARGUMENT, "dispatch lun=%hhu", requests[i].lun);
        ASSERT_REJECT((requests[i].ch == requests[0U].ch) && (requests[i].ce == requests[0U].ce) && (requests[i].lun == requests[0U].lun),
                      NAND_RESULT_INVALID_ARGUMENT,
                      "dispatch: chain member not same target");
        ASSERT_REJECT(requests[i].operation < OP_COUNT, NAND_RESULT_INVALID_ARGUMENT, "dispatch op=%hhu out of range", requests[i].operation);
        ASSERT_REJECT(g_fc_nand_dispatch_req_table[requests[i].operation] != NULL,
                      NAND_RESULT_INVALID_ARGUMENT,
                      "dispatch op=%hhu has no request handler",
                      requests[i].operation);
        ASSERT_REJECT(g_fc_nand_dispatch_completion_table[requests[i].operation] != NULL,
                      NAND_RESULT_INVALID_ARGUMENT,
                      "dispatch op=%hhu has no completion handler",
                      requests[i].operation);
        ASSERT_REJECT((requests[i].cw_bitmap & ~(((1UL << CW_TOTAL_MAX) - 1UL))) == 0UL,
                      NAND_RESULT_INVALID_ARGUMENT,
                      "dispatch: cw_bitmap=0x%08lX out of range",
                      requests[i].cw_bitmap);
    }

    queue = &g_fc_nand_dispatch_queues[requests[0U].ch][requests[0U].ce][requests[0U].lun];

    /* 原子性检查：剩余深度必须容纳整批 */
    if ((queue->count + count) > NAND_DISPATCH_CHAIN_DEPTH)
    {
        return NAND_RESULT_NOT_READY;
    }

    for (i = 0U; i < count; i++)
    {
        BYTE slot = ((queue->head + queue->count + i) % NAND_DISPATCH_CHAIN_DEPTH);
        fc_nand_dispatch_task_load(&queue->tasks[slot], &requests[i]);
    }
    /* 全部预加载后才提交 count：poll 不会看到半挂载的批 */
    queue->count = (queue->count + count);
    return NAND_RESULT_OK;
}

/**
 * @brief 发起异步 ONFI 事务并置初始相位
 *
 * @param queue     所属 LUN FIFO
 * @param task      目标任务
 * @param fc_result 输出：fc_nand_start 返回码
 * @return ASYNC / SYNC / WAIT
 */
static fc_nand_dispatch_start_outcome_e fc_nand_dispatch_task_fc_start(fc_nand_dispatch_lun_queue_s *queue, fc_nand_dispatch_task_s *task, BYTE *fc_result)
{
    const nand_phy_backend_s *backend = NULL;
    nand_result_e             acquire_result;
    nand_result_e             target_result;
    BYTE                      result;
    BOOL                      target_ok;

    /* 首次启动先取得 target/bank ownership；配对 WAIT/FETCH 复用已持有的 ownership。 */
    if (task->target_acquired == 0U)
    {
        acquire_result = nand_board_acquire_target(task->ch, task->ce);

        if (acquire_result == NAND_RESULT_NOT_READY)
        {
            *fc_result = (BYTE)NAND_RESULT_NOT_READY;
            return NAND_DISPATCH_START_WAIT;
        }
        if (acquire_result != NAND_RESULT_OK)
        {
            task->transaction.engine.backend = NULL;
            result                           = (BYTE)acquire_result;
            task->started                    = 1U;
            queue->phase                     = (nand_operation_is_issue(task->operation) != 0U) ? LUN_PHASE_ISSUE : LUN_PHASE_FETCH;
            *fc_result                       = result;
            return NAND_DISPATCH_START_SYNC;
        }
        task->target_acquired = 1U;
    }

    target_result = nand_target_get(task->ch, task->ce, &backend);
    result        = (BYTE)target_result;
    target_ok     = (target_result == NAND_RESULT_OK);

    // 共享总线板卡：任务开始前一次取得「通路 + backend」。
    // 通路失败不是“引擎异步忙”：必须走 SYNC 完成路径，否则下一轮会把
    // engine.active==0 误判成成功完成。
    task->transaction.engine.backend = (target_ok == true) ? backend : NULL;
    if (target_ok == true)
    {
        result = fc_nand_start(&task->transaction, task->operation, &task->request);
    }
    task->started = 1U;
    if (nand_operation_is_issue(task->operation) != 0U)
    {
        queue->phase = LUN_PHASE_ISSUE;
    }
    else
    {
        queue->phase = LUN_PHASE_FETCH;
    }
    *fc_result = result;

    /* ownership 统一由 lun_step 在命令完成/失败时释放；sync start 之后仍可能
     * 进入 cw_chain 续读（例如加载型 op 同步完成后继续 05/06），不能提前放锁。 */
    if (target_ok == false)
    {
        return NAND_DISPATCH_START_SYNC;
    }
    return (result == NAND_RESULT_NOT_READY) ? NAND_DISPATCH_START_ASYNC : NAND_DISPATCH_START_SYNC;
}

/**
 * @brief ISSUED 相位推进：等最短忙期到点，启动配对的 WAIT/FETCH
 *
 * @param ctx 单 LUN 上下文
 * @return WAIT / DONE（nand_completion 有效）
 */
static fc_nand_dispatch_lun_outcome_e fc_nand_dispatch_issued_advance(fc_nand_dispatch_lun_ctx_s *ctx)
{
    fc_nand_dispatch_task_s *task   = &ctx->queue->tasks[ctx->queue->head];
    WORD                     min_us = g_nand_operation_map[task->operation].min_busy_us;
    BYTE                     pair_op;
    BYTE                     result;

    /* ISSUE 段已完成但失败（如起始 70h 状态 FAIL）⇒ 别再发 WAIT/取数。
     * 否则后续逐 CW 取数可能把已标失败的状态覆盖成成功。 */
    if (task->completion.execution_status != NAND_RESULT_OK)
    {
        ctx->nand_completion.execution_status = task->completion.execution_status;
        ctx->nand_completion.nand_status      = task->completion.nand_status;
        ctx->nand_completion.pre_status       = task->completion.pre_status;
        ctx->result                           = task->completion.execution_status;
        return NAND_DISPATCH_LUN_DONE;
    }

    if ((min_us != 0U) && !delay_time_elapsed_us(ctx->queue->wait_start_us, min_us))
    {
        return NAND_DISPATCH_LUN_WAIT; /* 最短等待未到，下一轮再检查 */
    }

    pair_op = nand_operation_get_pair(task->operation);
    if ((pair_op != 0U) && (pair_op != task->operation))
    {
        task->operation = pair_op;
    }

    if (fc_nand_dispatch_task_start(ctx->queue, task, &result) != NAND_DISPATCH_START_SYNC)
    {
        return NAND_DISPATCH_LUN_WAIT;
    }

    ctx->nand_completion.execution_status = result;
    ctx->nand_completion.nand_status      = task->transaction.completion.nand_status;
    ctx->nand_completion.pre_status       = task->transaction.completion.pre_status;
    ctx->result                           = result;
    return NAND_DISPATCH_LUN_DONE;
}

/**
 * @brief Per-LUN FSM 单步：启动或推进队头事务
 *
 * @param ctx 单 LUN 上下文（result / nand_completion 就地更新）
 * @return WAIT / DONE
 */
static fc_nand_dispatch_lun_outcome_e fc_nand_dispatch_lun_drive(fc_nand_dispatch_lun_ctx_s *ctx)
{
    fc_nand_dispatch_task_s *task = &ctx->queue->tasks[ctx->queue->head];
    BYTE                     result;

    if (task->started == 0U)
    {
        if (fc_nand_dispatch_task_start(ctx->queue, task, &result) != NAND_DISPATCH_START_SYNC)
        {
            return NAND_DISPATCH_LUN_WAIT;
        }
        ctx->nand_completion.execution_status = result;
        ctx->nand_completion.nand_status      = task->transaction.completion.nand_status;
        ctx->nand_completion.pre_status       = task->transaction.completion.pre_status;
    }
    else if (task->transaction.engine.active != 0U)
    {
        result = fc_nand_step(&task->transaction, &ctx->nand_completion);
        if (result == NAND_RESULT_NOT_READY)
        {
            return NAND_DISPATCH_LUN_WAIT;
        }
    }
    else
    {
        result = ctx->nand_completion.execution_status;
    }

    ctx->result = result;

    /* ── 相位 ISSUE → ISSUED ──
     * 相位由上方 task_fc_start 置入（ISSUE 型 OP → ISSUE，其余 → FETCH）。
     * 命中即"命令只发完了前半段"：task_complete 已调用、task->completion 已填好，
     * 但 LUN 还在 tR/tPROG/tBERS，绝不能往下走 DONE 把结果当命令完成报出去。 */
    if (ctx->queue->phase == LUN_PHASE_ISSUE)
    {
        fc_nand_dispatch_task_complete(task, result, &ctx->nand_completion);
        ctx->queue->phase = LUN_PHASE_ISSUED;
        /* 记录 tR/tPROG/tBERS 起点，供 ISSUED 块最短等待用。
         * 时间源不可用时不能留下陈旧时间戳（ISSUED 会永等），转 dispatch fatal。 */
        if (delay_time_now_us(&ctx->queue->wait_start_us) == false)
        {
            s_dispatch_fatal = true;
            LOGE("fc: time source unavailable; dispatch fatal\r\n");
        }
        return NAND_DISPATCH_LUN_WAIT; /* 不报完成，下一轮由 ISSUED 块接管 */
    }
    return NAND_DISPATCH_LUN_DONE;
}

/**
 * @brief CW 级链式推进：加载进页缓冲后逐 CW 取数
 *
 * @param ctx 单 LUN 上下文
 * @return CW_ASYNC / CW_DONE
 */
static fc_nand_dispatch_cw_outcome_e fc_nand_dispatch_task_cw_chain(fc_nand_dispatch_lun_ctx_s *ctx)
{
    fc_nand_dispatch_task_s *task   = &ctx->queue->tasks[ctx->queue->head];
    BYTE                     result = ctx->result;

    for (;;)
    {
        BYTE n;

        fc_nand_dispatch_task_complete(task, result, &ctx->nand_completion);
        if (result != NAND_RESULT_OK)
        {
            break;
        }

        if (fc_nand_dispatch_is_load_complete_op(task->operation) != 0U)
        {
            /* 加载完成：数据已在页缓冲，从掩码最低位起取 */
            n = fc_nand_dispatch_cw_next(task->cw_bitmap, 0U);
            if (n == CW_TOTAL_MAX)
            {
                break;
            }
            task->cw_index = n;
        }
        else if (fc_nand_dispatch_is_fetch_op(task->operation) != 0U)
        {
            /* 取数：推进到下一个置位，链尾则结束 */
            n = fc_nand_dispatch_cw_next(task->cw_bitmap, (task->cw_index + 1U));
            if (n == CW_TOTAL_MAX)
            {
                break;
            }
            task->cw_index = n;
        }
        else
        {
            break;
        }

        task->operation = (task->plane_count > 0U) ? OP_RANDOM_DATA_READ_MULTI_PLANE : OP_RANDOM_DATA_READ;
        task->started   = 1U; /* 复用 start 路径但标记已启动 */
        {
            BYTE plane     = (task->cw_index / CW_NUM);
            BYTE cw        = (task->cw_index % CW_NUM);
            WORD cw_column = (cw * CW_SIZE);

            memset(&task->request, 0, sizeof(task->request));
            task->request.column      = cw_column;
            task->request.row[0]      = paa_to_nand_row(task->plane_paa[plane]);
            task->request.buffer      = task->cw_buffers[task->cw_index];
            task->request.data_length = CW_SIZE;
        }
        result = fc_nand_start(&task->transaction, task->operation, &task->request);
        if (result == NAND_RESULT_NOT_READY)
        {
            break;
        }
        ctx->nand_completion.execution_status = result;
        ctx->nand_completion.nand_status      = task->transaction.completion.nand_status;
        ctx->nand_completion.pre_status       = task->transaction.completion.pre_status;
    }

    ctx->result = result;
    return (result == NAND_RESULT_NOT_READY) ? NAND_DISPATCH_CW_ASYNC : NAND_DISPATCH_CW_DONE;
}

/**
 * @brief 把页缓冲整页数据拆回各 CW buffer
 *
 * 只遍历掩码内的置位；单 plane / 双 plane 的 offset 规则不同。
 *
 * @param task 已完成任务
 */
static void fc_nand_dispatch_scatter_page_buffer(const fc_nand_dispatch_task_s *task)
{
    BYTE *page_buffer = g_nand_channel_page_buffer[task->ch][task->ce][task->lun];
    BYTE  n;

    if ((task->operation == OP_NORMAL_READ_PAGE_WITH_DATA_OUT) || (task->operation == OP_OTP_DATA_READ) || (task->operation == OP_READ_PAGE_CACHE_START_SEQ) || (task->operation == OP_READ_PAGE_CACHE_SEQ) || (task->operation == OP_READ_PAGE_CACHE_RANDOM) || (task->operation == OP_READ_PAGE_CACHE_LAST))
    {
        for (n = fc_nand_dispatch_cw_next(task->cw_bitmap, 0U); n < CW_TOTAL_MAX; n = fc_nand_dispatch_cw_next(task->cw_bitmap, (n + 1U)))
        {
            BYTE cw_num = (n % CW_NUM);
            memcpy(task->cw_buffers[n], &page_buffer[cw_num * CW_SIZE], CW_SIZE);
        }
    }

    if ((task->operation == OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT) || (task->operation == OP_READ_PAGE_CACHE_START_SEQ_MP) || (task->operation == OP_READ_PAGE_CACHE_SEQ_MP) || (task->operation == OP_READ_PAGE_CACHE_LAST_MP))
    {
        for (n = fc_nand_dispatch_cw_next(task->cw_bitmap, 0U); n < CW_TOTAL_MAX; n = fc_nand_dispatch_cw_next(task->cw_bitmap, (n + 1U)))
        {
            BYTE  plane  = (n / CW_NUM);
            BYTE  cw_num = (n % CW_NUM);
            BYTE *src    = &page_buffer[plane * PAGE_SIZE + cw_num * CW_SIZE];
            memcpy(task->cw_buffers[n], src, CW_SIZE);
        }
    }
}

/**
 * @brief Per-LUN FSM 单步：推进队头命令，完成即回填并出队
 *
 * @param ctx 单 LUN 上下文
 * @return WAIT / DONE
 */
static fc_nand_dispatch_lun_outcome_e fc_nand_dispatch_lun_step(fc_nand_dispatch_lun_ctx_s *ctx)
{
    fc_nand_dispatch_task_s       *task;
    fc_nand_dispatch_lun_outcome_e out;

    /* ── 相位 ISSUED 预检（只判一次）── */
    if (ctx->queue->phase == LUN_PHASE_ISSUED)
    {
        out = fc_nand_dispatch_issued_advance(ctx);
        if (out != NAND_DISPATCH_LUN_DONE)
        {
            return out; /* WAIT（DONE 落到下方收尾） */
        }
        /* DONE：同步完成，落到下方驱动路径收尾 */
    }

    task = &ctx->queue->tasks[ctx->queue->head];
    out  = fc_nand_dispatch_lun_drive(ctx);
    if (out != NAND_DISPATCH_LUN_DONE)
    {
        return out; /* WAIT（DONE 落到下方收尾） */
    }

    if (fc_nand_dispatch_task_cw_chain(ctx) == NAND_DISPATCH_CW_ASYNC)
    {
        return NAND_DISPATCH_LUN_WAIT;
    }

    /* 失败时 page buffer 可能是半页/未初始化数据，不往 CW buffer 搬；
     * 完成状态已经由 task_complete 标成失败。 */
    if (ctx->result == NAND_RESULT_OK)
    {
        fc_nand_dispatch_scatter_page_buffer(task);
    }

    /* 队头命令的所有 NAND 事务已结束：释放 target/bank ownership。 */
    fc_nand_dispatch_target_release(task);

    /* ── 队头命令完成：回填 completion、出队、复位相位 ── */
    *ctx->completion = task->completion;
    ctx->queue->head = ((ctx->queue->head + 1U) % NAND_DISPATCH_CHAIN_DEPTH);
    ctx->queue->count--;
    ctx->queue->phase = LUN_PHASE_IDLE;
    return NAND_DISPATCH_LUN_DONE;
}

/**
 * @brief 把统一状态写入任务的全部选中 CW（整页读/编程/擦除/ISSUE/WAIT 等）
 *
 * 掩码为空时写 cw_status[0]（无 CW 概念或 ERASE 的合法空集）。
 *
 * @param task           正在收尾的任务
 * @param result         NAND_RESULT_* 执行结果
 */
static void completion_all_cws(fc_nand_dispatch_task_s *task, BYTE result)
{
    BYTE status = (result == NAND_RESULT_OK) ? FC_CW_STATUS_UNUSED : FC_CW_STATUS_FAIL;
    BYTE n;

    if (task->cw_bitmap == 0U)
    {
        task->completion.cw_status[0] = status;
        return;
    }
    for (n = fc_nand_dispatch_cw_next(task->cw_bitmap, 0U); n < CW_TOTAL_MAX; n = fc_nand_dispatch_cw_next(task->cw_bitmap, (n + 1U)))
    {
        task->completion.cw_status[n] = status;
    }
}

/**
 * @brief 加载型 op 的完成策略
 *
 * 成功时不写 cw_status（由后续逐 CW 取数填）；失败时刷满整张表。
 *
 * @param task           正在收尾的任务
 * @param result         NAND_RESULT_* 执行结果
 */
static void completion_load(fc_nand_dispatch_task_s *task, BYTE result)
{
    BYTE status;
    BYTE i;

    if (result == NAND_RESULT_OK)
    {
        return;
    }
    status = FC_CW_STATUS_FAIL;
    for (i = 0U; i < CW_TOTAL_MAX; i++)
    {
        task->completion.cw_status[i] = status;
    }
}

/* ======================================================================
 * Request 构建 handler
 * ====================================================================== */

/**
 * @brief 把各 CW buffer 拼进页缓冲，构建单 plane 页写请求
 *
 * 拼接偏移 = cw * CW_SIZE；未填充 CW 保持 0xFF 擦除态。
 *
 * @param task 预解析完成的任务
 */
static void req_program_page(fc_nand_dispatch_task_s *task)
{
    BYTE  n;
    BYTE *page_buffer = g_nand_channel_page_buffer[task->ch][task->ce][task->lun];

    memset(page_buffer, 0xFF, PAGE_SIZE);
    for (n = fc_nand_dispatch_cw_next(task->cw_bitmap, 0U); n < CW_TOTAL_MAX; n = fc_nand_dispatch_cw_next(task->cw_bitmap, (n + 1U)))
    {
        /* 页缓冲内的偏移 = CW 号 × CW_SIZE；CW 号就是 n % CW_NUM
         * （未写到的 CW 保持上面的 0xFF 擦除态）。 */
        memcpy(&page_buffer[(n % CW_NUM) * CW_SIZE], task->cw_buffers[n], CW_SIZE);
    }
    req_init(task, 0U, page_buffer, PAGE_SIZE);
    task->request.row[0] = paa_to_nand_row(task->plane_paa[task->plane]);
}

/**
 * @brief 构建整页加载请求
 *
 * 同时服务 WITHOUT_DATA_OUT（纯加载）与 WITH_DATA_OUT/OTP（标准语义）；
 * 空掩码 = 无 desc 的纯加载，data 置 NULL/0。
 *
 * @param task 预解析完成的任务
 */
static void req_read_page(fc_nand_dispatch_task_s *task)
{
    BYTE *cw_buffer;

    /* 非稀疏单 LUN 的整页 direct：整页 Dout 到 page buffer，完成后再 scatter。 */
    if ((task->operation == OP_NORMAL_READ_PAGE_WITH_DATA_OUT) || (task->operation == OP_OTP_DATA_READ))
    {
        BYTE *page_buffer = g_nand_channel_page_buffer[task->ch][task->ce][task->lun];

        req_init(task, 0U, page_buffer, PAGE_SIZE);
        task->request.row[0] = paa_to_nand_row(task->plane_paa[task->plane]);
        return;
    }

    /* 空掩码 = 无 desc 的「纯加载」op（如 op 33 直接下发）：uop 无 DATA_OUT，data 不被读；
     * task_load 已把 cw_buffers[] 全清为 NULL，这里仍显式置 NULL/0，保持请求自描述。 */
    if (task->cw_bitmap == 0U)
    {
        req_init(task, 0U, NULL, 0U);
        task->request.row[0] = paa_to_nand_row(task->plane_paa[task->plane]);
        return;
    }

    /* 掩码最低位 = 本 plane 第一个要访问的 CW（其缓冲由 FC 数据路径填满，恒非空） */
    cw_buffer = task->cw_buffers[fc_nand_dispatch_cw_first_or_zero(task->cw_bitmap)];

    req_init(task, 0U, cw_buffer, CW_SIZE);
    task->request.row[0] = paa_to_nand_row(task->plane_paa[task->plane]);
}

/**
 * @brief 构建多 plane 整页加载请求
 *
 * row[] 按 plane 填 PLANE_NUM 个行地址；数据落各自 plane 页缓冲。
 *
 * @param task 预解析完成的任务
 */
static void req_multi_plane_read(fc_nand_dispatch_task_s *task)
{
    BYTE  plane;
    BYTE *page_buffer = g_nand_channel_page_buffer[task->ch][task->ce][task->lun];

    memset(page_buffer, 0x00, PAGE_BUFFER_SIZE);

    /* 每次 DATA_OUT 只读 1 个 Plane（PAGE_SIZE）；第二个 Plane 由 06h-E0h +
     * DATA_OUT operand=PAGE_SIZE 分两次读取。 */
    req_init(task, 0U, page_buffer, PAGE_SIZE);

    /* ⚠ 上界写死 PLANE_NUM，不看 task->plane_count：本 handler 只由 multi-plane OP
     *   选中，那条路径上 plane_count 恒 = PLANE_NUM。改 plane 语义时两处要一起动。
     * row[PLANE_NUM..NAND_PLANE_MAX-1] 由 req_init 的 memset 零填充。 */
    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        task->request.row[plane] = paa_to_nand_row(task->plane_paa[plane]);
    }
}

/**
 * @brief 构建多 plane 写请求
 *
 * 拼接偏移 = (n / CW_NUM) * PAGE_SIZE + (n % CW_NUM) * CW_SIZE；
 * 未填充 CW 保持 0xFF；行地址按 plane 填 PLANE_NUM 个。
 *
 * @param task 预解析完成的任务
 */
static void req_multi_plane_program(fc_nand_dispatch_task_s *task)
{
    BYTE  n, plane;
    BYTE *page_buffer = g_nand_channel_page_buffer[task->ch][task->ce][task->lun];

    memset(page_buffer, 0xFF, PAGE_BUFFER_SIZE);
    for (n = fc_nand_dispatch_cw_next(task->cw_bitmap, 0U); n < CW_TOTAL_MAX; n = fc_nand_dispatch_cw_next(task->cw_bitmap, (n + 1U)))
    {
        memcpy(&page_buffer[(n / CW_NUM) * PAGE_SIZE + (n % CW_NUM) * CW_SIZE], task->cw_buffers[n], CW_SIZE);
    }

    /* 每次 DATA_IN 只发 1 个 Plane（PAGE_SIZE），第二个 Plane 由 DATA_IN
     * operand=PAGE_SIZE 偏移读取。 */
    req_init(task, 0U, page_buffer, PAGE_SIZE);

    /* ⚠ 上界写死 PLANE_NUM，理由同上。row[PLANE_NUM..] 由 req_init 的 memset 零填充。 */
    for (plane = 0U; plane < PLANE_NUM; plane++)
    {
        task->request.row[plane] = paa_to_nand_row(task->plane_paa[plane]);
    }
}

/* ======================================================================
 * 初始化 / 就绪
 * ====================================================================== */

/**
 * @brief 清零软件调度状态（不碰 FMC 硬件）
 *
 * @note 纯软件重置：只做 memset + disarm，没有失败路径。
 */
/* --------------------------------------------------------------------
 * completion 表 g_fc_nand_dispatch_completion_table
 *   每个 OP 的完成策略选择依据（与 onfi_1_0.h 的 OP 语义对应）：
 *     - completion_all_cws   ：所有选中 CW 同状态；空掩码写 cw_status[0]。
 *                              用于无 CW 概念（RESET/ID/STATUS/FEATURES/块锁/META 回读等）、
 *                              编程/擦除、整页 cache 读、ISSUE/WAIT 段。
 *     - completion_per_cw    ：逐 CW 独立状态（普通读 / OTP 读的随机取数链）。
 *     - completion_load      ：加载型 op 成功不写状态，失败时刷满
 *                              （WITHOUT_DATA_OUT 两种 + 多 LUN 相位机的 LOAD/WAIT）。
 *   下标 OP_* 不连续（含多 LUN 相位机扩展 35..43），用设计初始化器定位。
 * -------------------------------------------------------------------- */
const fc_nand_dispatch_completion_handler_fn_t g_fc_nand_dispatch_completion_table[OP_COUNT] = {
    [OP_RESET]                                  = completion_all_cws,
    [OP_READ_ID]                                = completion_all_cws,
    [OP_READ_PARAMETER_PAGE]                    = completion_all_cws,
    [OP_READ_UNIQUE_ID]                         = completion_all_cws,
    [OP_GET_FEATURES]                           = completion_all_cws,
    [OP_SET_FEATURES]                           = completion_all_cws,
    [OP_READ_STATUS]                            = completion_all_cws,
    [OP_READ_STATUS_ENHANCED]                   = completion_all_cws,
    [OP_RANDOM_DATA_READ]                       = completion_per_cw,
    [OP_RANDOM_DATA_READ_MULTI_PLANE]           = completion_per_cw,
    [OP_READ_ONFI_SIGNATURE]                    = completion_all_cws,
    [OP_PROGRAM_FOR_INTERNAL_DATA_MOVE]         = completion_all_cws,
    [OP_READ_META]                              = completion_all_cws,
    [OP_NORMAL_READ_PAGE_WITH_DATA_OUT]         = completion_all_cws,
    [OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT]      = completion_load,
    /* 单 plane cache 读同样整页读入 page_buffer 后 scatter 到所有选中 CW；
     * 完成状态必须覆盖整组 CW，不能只标最低位那一个。 */
    [OP_READ_PAGE_CACHE_SEQ]                    = completion_all_cws,
    [OP_READ_PAGE_CACHE_RANDOM]                 = completion_all_cws,
    [OP_READ_PAGE_CACHE_LAST]                   = completion_all_cws,
    [OP_READ_PAGE_CACHE_START_SEQ]              = completion_all_cws,
    [OP_TWO_PLANE_PROGRAM_CACHE]                = completion_all_cws,
    [OP_TWO_PLANE_PROGRAM_CACHE_END]            = completion_all_cws,
    [OP_TWO_PLANE_PROGRAM_CACHE_FIRST]          = completion_all_cws,
    [OP_READ_PAGE_CACHE_START_SEQ_MP]           = completion_all_cws,
    [OP_READ_PAGE_CACHE_SEQ_MP]                 = completion_all_cws,
    [OP_READ_PAGE_CACHE_LAST_MP]                = completion_all_cws,
    [OP_NORMAL_PROGRAM_PAGE]                    = completion_all_cws,
    [OP_PROGRAM_PAGE_CACHE]                     = completion_all_cws,
    [OP_PROGRAM_PAGE_CACHE_FIRST]               = completion_all_cws,
    [OP_ERASE_BLOCK]                            = completion_all_cws,
    [OP_READ_FOR_INTERNAL_DATA_MOVE]            = completion_all_cws,
    [OP_BLOCK_UNLOCK_LOW]                       = completion_all_cws,
    [OP_BLOCK_UNLOCK_HIGH]                      = completion_all_cws,
    [OP_BLOCK_LOCK]                             = completion_all_cws,
    [OP_BLOCK_LOCK_TIGHT]                       = completion_all_cws,
    [OP_BLOCK_LOCK_STATUS]                      = completion_all_cws,
    [OP_OTP_DATA_LOCK]                          = completion_all_cws,
    [OP_OTP_DATA_PROGRAM]                       = completion_all_cws,
    [OP_OTP_DATA_READ]                          = completion_all_cws,
    [OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT] = completion_load,
    [OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT]    = completion_all_cws,
    [OP_PROGRAM_PAGE_MULTI_PLANE]               = completion_all_cws,
    [OP_ERASE_BLOCK_MULTI_PLANE]                = completion_all_cws,
    /* 多 LUN 相位机: ISSUE/WAIT read completion —— 读的 CW 状态统一由逐 CW 取数写，
     * 所以 LOAD/WAIT 都用 completion_load（成功不写、失败刷满）。 */
    [OP_READ_PAGE_LOAD]                         = completion_load,
    [OP_READ_PAGE_WAIT]                         = completion_load,
    [OP_READ_PAGE_MULTI_PLANE_LOAD]             = completion_load,
    [OP_PROGRAM_PAGE_ISSUE]                     = completion_all_cws,
    [OP_PROGRAM_PAGE_WAIT]                      = completion_all_cws,
    [OP_ERASE_BLOCK_ISSUE]                      = completion_all_cws,
    [OP_ERASE_BLOCK_WAIT]                       = completion_all_cws,
    [OP_READ_COLUMN]                            = completion_all_cws,
};

/* --------------------------------------------------------------------
 * req 表 g_fc_nand_dispatch_req_table
 *   每个 OP 的 request 构建策略选择依据：
 *     - req_no_data        ：纯地址操作，无数据传输
 *     - req_single         ：固定长度数据（长度查 g_nand_operation_map）
 *     - req_random_read    ：页缓冲随机读单 CW（列地址 = paa_to_cw(PAA)×CW_SIZE）
 *     - req_read_page      ：读 CW0 到 cw_buffer（后续 CW 走 RANDOM_DATA_READ chaining）
 *     - req_cache_read     ：START_SEQ/SEQ/RANDOM/LAST 单 plane 全页读入 page_buffer
 *     - req_program_page   ：单 plane 多 CW 拼接后一次写
 *     - req_multi_plane_read/program/erase：双 plane 行地址 / 双 plane 拼接写
 *     - req_feature        ：SET/GET_FEATURES（column=feature_addr, 4B）
 *     - req_meta_read      ：META 只读回读（column = CW 号×CW_SIZE, 4B, 无 desc）
 *     - req_onfi_signature ：ONFI 签名读（90h + 地址 20h, 4B, 无 desc）
 *     - req_read_column    ：按显式列地址读（列来自 task->column, 4B, 无 desc）
 *     - req_erase          ：擦除（无数据）
 *   下标 OP_* 不连续（含 多 LUN 相位机 扩展 35..43），用设计初始化器定位。
 * -------------------------------------------------------------------- */
const fc_nand_dispatch_req_handler_fn_t g_fc_nand_dispatch_req_table[OP_COUNT] = {
    [OP_RESET]                                  = req_no_data,
    [OP_READ_ID]                                = req_single,
    [OP_READ_PARAMETER_PAGE]                    = req_single,
    [OP_READ_UNIQUE_ID]                         = req_single,
    [OP_GET_FEATURES]                           = req_feature,
    [OP_SET_FEATURES]                           = req_feature,
    [OP_READ_STATUS]                            = req_single,
    [OP_READ_STATUS_ENHANCED]                   = req_single,
    [OP_RANDOM_DATA_READ]                       = req_random_read,
    [OP_RANDOM_DATA_READ_MULTI_PLANE]           = req_random_read,
    [OP_READ_ONFI_SIGNATURE]                    = req_onfi_signature,
    [OP_PROGRAM_FOR_INTERNAL_DATA_MOVE]         = req_single,
    [OP_READ_META]                              = req_meta_read,
    [OP_NORMAL_READ_PAGE_WITH_DATA_OUT]         = req_read_page,
    [OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT]      = req_read_page,
    [OP_READ_PAGE_CACHE_SEQ]                    = req_cache_read,
    [OP_READ_PAGE_CACHE_RANDOM]                 = req_cache_read,
    [OP_READ_PAGE_CACHE_LAST]                   = req_cache_read,
    [OP_READ_PAGE_CACHE_START_SEQ]              = req_cache_read,
    [OP_TWO_PLANE_PROGRAM_CACHE]                = req_multi_plane_program,
    [OP_TWO_PLANE_PROGRAM_CACHE_END]            = req_multi_plane_program,
    [OP_TWO_PLANE_PROGRAM_CACHE_FIRST]          = req_multi_plane_program,
    [OP_READ_PAGE_CACHE_START_SEQ_MP]           = req_multi_plane_read,
    [OP_READ_PAGE_CACHE_SEQ_MP]                 = req_multi_plane_read,
    [OP_READ_PAGE_CACHE_LAST_MP]                = req_multi_plane_read,
    [OP_NORMAL_PROGRAM_PAGE]                    = req_program_page,
    [OP_PROGRAM_PAGE_CACHE]                     = req_program_page,
    [OP_PROGRAM_PAGE_CACHE_FIRST]               = req_program_page,
    [OP_ERASE_BLOCK]                            = req_erase,
    [OP_READ_FOR_INTERNAL_DATA_MOVE]            = req_no_data,
    [OP_BLOCK_UNLOCK_LOW]                       = req_no_data,
    [OP_BLOCK_UNLOCK_HIGH]                      = req_no_data,
    [OP_BLOCK_LOCK]                             = req_no_data,
    [OP_BLOCK_LOCK_TIGHT]                       = req_no_data,
    [OP_BLOCK_LOCK_STATUS]                      = req_no_data,
    [OP_OTP_DATA_LOCK]                          = req_no_data,
    [OP_OTP_DATA_PROGRAM]                       = req_program_page,
    [OP_OTP_DATA_READ]                          = req_read_page,
    [OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT] = req_multi_plane_read,
    [OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT]    = req_multi_plane_read,
    [OP_PROGRAM_PAGE_MULTI_PLANE]               = req_multi_plane_program,
    [OP_ERASE_BLOCK_MULTI_PLANE]                = req_multi_plane_erase,
    /* 多 LUN 相位机: ISSUE/WAIT read request */
    [OP_READ_PAGE_LOAD]                         = req_read_page,
    [OP_READ_PAGE_WAIT]                         = req_no_data,
    [OP_READ_PAGE_MULTI_PLANE_LOAD]             = req_multi_plane_read,
    [OP_PROGRAM_PAGE_ISSUE]                     = req_program_page,
    [OP_PROGRAM_PAGE_WAIT]                      = req_no_data,
    [OP_ERASE_BLOCK_ISSUE]                      = req_erase,
    [OP_ERASE_BLOCK_WAIT]                       = req_no_data,
    [OP_READ_COLUMN]                            = req_read_column,
};

void fc_nand_dispatch_init(void)
{
    memset(g_fc_nand_dispatch_queues, 0, sizeof(g_fc_nand_dispatch_queues));
    /* DTCM/非缓存段都不由启动代码清零；各 builder 虽会先 memset，这里仍显式清零，
     * 避免任何未来路径读到上电残留。 */
    memset(g_nand_channel_page_buffer, 0, sizeof(g_nand_channel_page_buffer));
    nand_board_ownership_reset();
    fc_nand_engine_set_armed(0U);
    s_poll_next_channel = 0U;
    s_dispatch_fatal    = false;
}

/**
 * @brief 调度层是否出现不可继续的 fatal（当前仅 µs 时间源不可用）。
 *
 * @return true = fatal，fc.c 应把控制器 fatal 置位并终止服务。
 */
BOOL fc_nand_dispatch_fatal(void)
{
    return s_dispatch_fatal;
}

/**
 * @brief 目标 LUN FIFO 的剩余槽数
 *
 * @param ch  目标 ch
 * @param ce  目标 ce
 * @param lun 目标 lun
 * @return 剩余槽数
 */
BYTE fc_nand_dispatch_lun_free(BYTE ch, BYTE ce, BYTE lun)
{
    // route 合法性由唯一生产调用者 fc_poll_fill_queue 在入口断言。
    return (NAND_DISPATCH_CHAIN_DEPTH - g_fc_nand_dispatch_queues[ch][ce][lun].count);
}

/**
 * @brief 目标 LUN FIFO 的占用槽数
 *
 * @param ch  目标 ch
 * @param ce  目标 ce
 * @param lun 目标 lun
 * @return 占用槽数
 */
BYTE fc_nand_dispatch_lun_used(BYTE ch, BYTE ce, BYTE lun)
{
    // 诊断查询；调用点使用固定 (0,0,0) 或已验证 route。
    return (BYTE)g_fc_nand_dispatch_queues[ch][ce][lun].count;
}

/**
 * @brief 目标 LUN 的相位机当前相位
 *
 * @param ch  目标 ch
 * @param ce  目标 ce
 * @param lun 目标 lun
 * @return 相位（LUN_PHASE_*）
 */
BYTE fc_nand_dispatch_lun_phase(BYTE ch, BYTE ce, BYTE lun)
{
    // 诊断查询；调用点使用固定 (0,0,0) 或已验证 route。
    return g_fc_nand_dispatch_queues[ch][ce][lun].phase;
}

/**
 * @brief 单命令入队（非阻塞登记）
 *
 * @param request 预解析请求
 * @return OK / INVALID_ARGUMENT / NOT_READY
 */
nand_result_e fc_nand_dispatch_submit(const fc_nand_dispatch_req_s *request)
{
    return fc_nand_dispatch_enqueue(request, 1U);
}

/**
 * @brief 整链原子入队
 *
 * @param requests 命令数组
 * @param count    链长
 * @return OK / INVALID_ARGUMENT / NOT_READY
 */
nand_result_e fc_nand_dispatch_submit_chain(const fc_nand_dispatch_req_s *requests, BYTE count)
{
    return fc_nand_dispatch_enqueue(requests, count);
}

/**
 * @brief 遍历全部 LUN，每 LUN 推进一条；完成经 sink 逐条交出
 *
 * 全遍历不早退，保证公平；返回本轮产出条数。
 *
 * @param sink 完成回调
 * @param ctx  透传给 sink 的上下文
 * @return 本轮完成条数
 */
DWORD fc_nand_dispatch_poll(fc_nand_dispatch_sink_fn_t sink, void *ctx)
{
    fc_nand_dispatch_comp_s completion;
    DWORD                   delivered     = 0U;
    const BYTE              start_channel = s_poll_next_channel;

    /* 回调前置条件由调用者保证（fc.c 轮询路径恒传有效 sink） */
    ASSERT(sink != NULL);

    /* 通道 round-robin：从上次有完成的通道的下一个开始，避免 CH0 永久优先。 */
    for (BYTE ch_offset = 0U; ch_offset < CH_NUM; ch_offset++)
    {
        const BYTE channel = (BYTE)((start_channel + ch_offset) % CH_NUM);

        for (BYTE ce = 0U; ce < CE_NUM; ce++)
        {
            for (BYTE lun_idx = 0U; lun_idx < LUN_NUM; lun_idx++)
            {
                fc_nand_dispatch_lun_queue_s  *queue = &g_fc_nand_dispatch_queues[channel][ce][lun_idx];
                fc_nand_dispatch_lun_ctx_s     lun_ctx;
                fc_nand_dispatch_lun_outcome_e out;

                if (queue->count == 0U)
                {
                    continue; /* 该 LUN 无在途任务，跳过 */
                }

                /* completion 只在 lun_step 返回 DONE 时被写入，此处复用同一槽 */
                lun_ctx.completion = &completion;
                lun_ctx.queue      = queue;
                memset(&lun_ctx.nand_completion, 0, sizeof(lun_ctx.nand_completion));
                lun_ctx.result = NAND_RESULT_OK;

                out = fc_nand_dispatch_lun_step(&lun_ctx);
                if (out == NAND_DISPATCH_LUN_DONE)
                {
                    sink(&completion, ctx);
                    delivered++;
                    s_poll_next_channel = (BYTE)((channel + 1U) % CH_NUM);
                }
                if (s_dispatch_fatal == true)
                {
                    return delivered; /* 时间源缺失：不再推进其他 LUN */
                }
            }
        }
    }
    return delivered;
}

#if defined(FC_NAND_TEST_ENABLE)
void fc_nand_dispatch_test_completion(BYTE operation, DWORD cw_bitmap, BYTE cw_status[CW_TOTAL_MAX])
{
    fc_nand_dispatch_task_s task;

    memset(&task, 0, sizeof(task));
    task.operation = operation;
    task.cw_bitmap = cw_bitmap;

    for (BYTE i = 0U; i < CW_TOTAL_MAX; i++)
    {
        cw_status[i] = FC_CW_STATUS_UNUSED;
    }

    g_fc_nand_dispatch_completion_table[operation](&task, (BYTE)NAND_RESULT_OPERATION_FAILED);

    memcpy(cw_status, task.completion.cw_status, sizeof(task.completion.cw_status));
}
#endif

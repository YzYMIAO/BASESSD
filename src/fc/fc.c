/*
 * fc.c — FC 核心实现（命令池 / 描述符池 / 缓冲池 / 调度 / 服务主循环）
 *
 * 资源模型（三池，全部静态固定，无动态内存）：
 *   - Command Pool（FC_COMMAND_POOL_DEPTH）：fc_cmd_s，单条命令
 *   - Descriptor Pool（FC_DESCRIPTOR_POOL_DEPTH）：fc_desc_s，一个 CW 一个描述符，next_id 串联
 *   - Buffer Pool（FC_BUFFER_POOL_DEPTH）：528B CW 缓冲，总线直访（noncacheable 段）
 *
 * 生命周期（对 FTL）：
 *   fc_command_alloc() → 填 config/参数 → 逐条 fc_command_configure_descriptor() 组装 desc 链
 *   → fc_submit() → fc_service()（后台推进）
 *   → fc_poll_completion() 取完成 → fc_command_release() 回收。
 *   （掩码建链与数据搬运在 FTL 侧：ftl_cmd_configure_descriptors / ftl_cmd_write_data）
 *
 * 文件结构（按职责分区，函数定义顺序即依赖顺序，无前向声明）：
 *    0. 内部类型        控制器/调度/命令链状态（原 fc_internal.h）
 *    1. 静态资源        三池 + 链表数组 + g_fc_controller
 *    2. 查询/转换小工具 is_page_read/write、getter、ID→指针、buffer 工具
 *    3. 初始化          fc_init
 *    4. 资源分配/回滚   command/descriptor/buffer alloc、rollback、release
 *    5. desc 链原语     fc_pool_chain_append
 *    6. 提交            fc_sched_submit
 *    7. 完成            fc_sched_complete、fc_complete_from_nand
 *    8. Stripe RAID     fc_stripe_release_parity、fc_stripe_submit
 *    9. 调度/轮询       fc_poll_* 辅助 + fc_service
 *   10. CQ 出口         fc_poll_completion、fc_poll_completions
 *   11. 对外 API        公开函数实现（原 fc_interface.c；签名不变）
 *
 * 服务主循环（fc_service）：
 *   ① fc_nand_dispatch_poll() 推进 NAND 层全部 LUN，每完成经 sink 当场 fc_complete_from_nand()
 *   ② 无完成 → fc_poll_fill() 一次扫描 HSQ/SQ，按 LUN 保序把能放下的命令（含链）
 *      提交进 NAND per-LUN FIFO（分派 fc_poll_dispatch_single() /
 *      fc_poll_dispatch_chain()；后者内部构建 request → submit/submit_chain，
 *      raid=ENCODE 走 fc_stripe_submit）
 *
 * 状态机：FREE → ALLOCATED → QUEUED → ACTIVE → COMPLETED。
 */

#include "fc.h"

#include "assertion.h"
#include "fc_crc_engine.h"
#include "fc_data_path.h"
#include "fc_ecc_engine.h"
#include "fc_nand_dispatch.h"
#include "fc_queue.h"
#include "fc_raid_engine.h"
#include "fc_random_engine.h"
#include "log.h"
#include "ssd_compiler.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════
 * 内部类型（原 fc_internal.h；控制器/调度/命令链状态只在本文件可见）
 * ══════════════════════════════════════════════════════════ */

/* ── ① 调度器状态 / 命令链描述 ──
 * 选择逻辑在 fc.c 的 fc_poll_fill；本处只放类型与状态。 */
#define FC_SCHEDULER_CHAIN_MAX NAND_DISPATCH_CHAIN_DEPTH

typedef struct FC_SCHEDULER_CHAIN_S
{
    fc_cmd_id_t ids[FC_SCHEDULER_CHAIN_MAX];
    BYTE        count; /* 0=空, 1=单命令, 2+=链 */
} fc_sched_chain_s;

/* 链归属队列（per-LUN 链锁）。粒度是「哪条提交队列能喂这个 LUN」，
 * 不是「LUN 不能用」；跨填充轮次持久。详见 fc_poll_fill_queue。 */
typedef enum FC_CHAIN_OWNER_E
{
    FC_CHAIN_OWNER_NONE = 0U, /* 该 LUN 上没有未结束的链 */
    FC_CHAIN_OWNER_SQ,        /* 未结束的链在普通队列里 */
    FC_CHAIN_OWNER_HSQ        /* 未结束的链在高优先队列里 */
} fc_chain_owner_e;

typedef struct FC_SCHEDULER_S
{
    fc_sub_queue_s *sq;
    fc_sub_queue_s *hsq;
    /* 链锁：链一旦开始，该 LUN 的填充权归链所在的队列独占，直到链的 END 下发。
     * 取值 fc_chain_owner_e。跨填充轮次持久——fc_poll_fill 的 suspended[] 每趟
     * memset 清零，做不到跨趟保留，所以这份状态必须挂在控制器上。 */
    fc_chain_owner_e chain_owner[CH_NUM][CE_NUM][LUN_NUM];
} fc_sched_s;

/* ── ② 命令 / buffer 状态枚举（本文件独占） ── */
typedef enum FC_COMMAND_STATE_E
{
    FC_COMMAND_STATE_FREE = 0U, /* 空闲 */
    FC_COMMAND_STATE_ALLOCATED, /* 已分配（可配置 desc/buffer） */
    FC_COMMAND_STATE_QUEUED,    /* 已提交到 SQ/HSQ */
    FC_COMMAND_STATE_ACTIVE,    /* 已出队，NAND 执行中 */
    FC_COMMAND_STATE_COMPLETED  /* 完成项已入 CQ（可释放） */
} fc_cmd_state_e;

typedef enum FC_BUFFER_STATE_E
{
    FC_BUFFER_STATE_FREE     = 0U, /* 未占用 */
    FC_BUFFER_STATE_OCCUPIED = 1U, /* 已分配给某条命令 */
} fc_buf_state_e;

#if CONFIG_FC_RAID
/* ── RAID 预留类型（条件编译）──
 *
 * 当前 PLANE_NUM=2，FC_STRIPE_DATA_MAX=1，单命令即满 stripe，
 * fc_stripe_submit() 在 fc_service() 中直接处理，不经过 accumulator。
 *
 * fc_stripe_accum_s 和 stripe_accum[] 为 PLANE_NUM>=4 预留扩展：
 * 当需要跨多次 fc_service() 调用积累多条数据命令后统一提交 parity 时，
 * 由 fc_poll_stripe_accumulate()（待实现）管理。 */

/* 单组 stripe 的最大数据命令数 = PLANE_NUM - 1 */
#    define FC_STRIPE_DATA_MAX ((PLANE_NUM - 1U))

/* 每个 LUN 的 stripe 积累状态（PLANE_NUM>=4 扩展预留） */
typedef struct FC_STRIPE_ACCUM_S
{
    BOOL        active;                       /* true = 积累中 */
    BYTE        ch, ce, lun;                  /* 目标 LUN */
    BYTE        count;                        /* 已积累的数据命令数 */
    fc_cmd_id_t data_ids[FC_STRIPE_DATA_MAX]; /* 已积累的数据命令 ID */
    fc_cmd_id_t parity_id;                    /* 已分配的 parity 命令 ID */
} fc_stripe_accum_s;
#endif /* CONFIG_FC_RAID */

/* ── ③ FC 控制器：命令状态机 + 三池引用 + 队列/调度器 ──
 * 仅内部实现可见；对外通过 fc_interface.h 的 API 访问。
 * 队列头尾/计数直接读 sq/hsq/cq 字段（没有独立的寄存器影子结构）。 */
typedef struct FC_CONTROLLER_S
{
    fc_sub_queue_s  sq;
    fc_sub_queue_s  hsq;
    fc_comp_queue_s cq;
    fc_sched_s      scheduler;
    fc_cmd_s       *command_pool;
    fc_desc_s      *descriptor_pool;
    BYTE(*buffer_pool)
    [FC_BUFFER_POOL_CW_SIZE];
    fc_cmd_id_t    *descriptor_owner;
    fc_cmd_state_e *command_state;
    fc_cmd_id_t    *command_free_next;
    WORD           *descriptor_free_next;
    WORD           *buffer_free_next;
    fc_buf_state_e *buffer_state;
    /* buffer 归属：OCCUPIED 时必须等于持有它的命令 ID；上层预申请但尚未
     * 挂到命令时为 FC_CMD_ID_INVALID。释放闸门用它拒绝双释放/在飞释放。 */
    fc_cmd_id_t *buffer_owner;
    fc_cmd_id_t  command_free_head;
    WORD         descriptor_free_head;
    WORD         buffer_free_head;

    /* O(1) 资源计数：只在分配/释放闸门增减；高水位用于诊断与容量调参。 */
    WORD command_used;
    WORD descriptor_used;
    WORD buffer_used;
    WORD command_high;
    WORD descriptor_high;
    WORD buffer_high;

    /* ── Stripe RAID 积累状态 ── */
#if CONFIG_FC_RAID
    fc_stripe_accum_s stripe_accum[LUN_NUM];                         /* 每 LUN stripe 积累 */
    fc_cmd_id_t       stripe_parity_for_data[FC_COMMAND_POOL_DEPTH]; /* 数据命令 ID → parity 命令 ID 映射 */
#endif                                                               /* CONFIG_FC_RAID */
} fc_ctl_s;

/* ══════════════════════════════════════════════════════════
 * 1. 静态资源
 *
 * 静态资源区：FC 控制面固定资源，当前位于默认 .bss（AXI SRAM）。
 * 不使用动态内存。后续根据 Cache 和 Buffer Pool 设计，
 * 可将各数组分别放入指定链接段。
 *
 *   池布局：
 *     command_pool / descriptor_pool / buffer_pool —— 三池本体
 *     descriptor_owner[desc] —— desc 归属的命令 ID（O(1) 反查）
 *     command_state / command_free_next —— 命令状态 + 空闲链表
 *     descriptor_free_next —— desc 空闲链表
 *     buffer_free_next / buffer_state —— buffer 空闲链表 + 占用状态
 *
 *   ⚠ 「一条命令拥有哪些 desc」**只有一份真值**：命令的 desc 链
 *     （command->desc_id + descriptor_pool[].next_id）。不设另一条
 *     「所有权链」（descriptor_owned_next[] + command_descriptor_head[]）为
 *     「desc 已分配但尚未挂进数据链」的中间态兜底，（省 640B）。
 *
 *   ⚠ **desc 出池即挂链**：fc_command_configure_descriptor 分配 desc 后
 *     先把字段填好、立刻 append，再分配 buffer。于是「已分配 desc 全部在链上」
 *     恒成立，失败时沿链释放即完备——不需要任何「链外半成品」的回滚逻辑
 *     （中间态本身不存在，不依赖就地回滚）。代价：链上允许存在
 *     buffer_id == FC_BUF_ID_INVALID 的 desc（FC_BUFFER_POOL_DEPTH < FC_DESCRIPTOR_POOL_DEPTH，
 *     这是最现实的失败点），所有走链的读者都要容忍它。
 * ══════════════════════════════════════════════════════════ */

static fc_cmd_s  g_fc_command_pool[FC_COMMAND_POOL_DEPTH];
static fc_desc_s g_fc_descriptor_pool[FC_DESCRIPTOR_POOL_DEPTH];
// 数据缓冲固定在 noncacheable 段：FMC 数据面按字节读写，避免 CPU D-Cache
// 与总线视图不一致；也保证地址落在 DMA 可达区（DMA1/2 访问不到 DTCM）。
static BYTE            g_fc_buffer_pool[FC_BUFFER_POOL_DEPTH][FC_BUFFER_POOL_CW_SIZE] __attribute__((section(".noncacheable_buffer")));
static fc_cmd_id_t     g_fc_sq_storage[FC_SQ_DEPTH];
static fc_cmd_id_t     g_fc_hsq_storage[FC_HSQ_DEPTH];
static fc_cmd_status_s g_fc_cq_storage[FC_CQ_DEPTH];
static fc_cmd_id_t     g_fc_descriptor_owner[FC_DESCRIPTOR_POOL_DEPTH];
static fc_cmd_state_e  g_fc_command_state[FC_COMMAND_POOL_DEPTH];
static fc_cmd_id_t     g_fc_command_free_next[FC_COMMAND_POOL_DEPTH];
static WORD            g_fc_descriptor_free_next[FC_DESCRIPTOR_POOL_DEPTH];
static WORD            g_fc_buffer_free_next[FC_BUFFER_POOL_DEPTH];
static fc_buf_state_e  g_fc_buffer_state[FC_BUFFER_POOL_DEPTH];
static fc_cmd_id_t     g_fc_buffer_owner[FC_BUFFER_POOL_DEPTH];

// GET/SET_FEATURES 的 4 字节内联数据暂存，按 command_id（= 命令池槽位号）索引。
// 为什么需要：SQ 命令提交后不可写（见 fc_command.h「SQ 不可变」），而引擎需要一个
// 稳定的 4 字节落点写回寄存器值；返回值唯一的归宿是 CQ 的 param 联合体，
// 故这里只做提交→完成之间的中转。索引用的 command_id 是「指针 − command_pool 基址」
// 派生出来的池内下标，恒 < FC_COMMAND_POOL_DEPTH。
// 与 buffer pool 同放 noncacheable 段：READ_META / GET_FEATURES 的 4 字节
// 返回段由引擎经 FMC 写回，避免 CPU 缓存与总线视图不一致。
static BYTE g_fc_return_staging[FC_COMMAND_POOL_DEPTH][4] __attribute__((section(".noncacheable_buffer")));

/* 填充轮次计数：排障用（fc_debug_state_get 读取）。 */
static DWORD s_fill_calls;
static DWORD s_fill_pops;

// 全局单例控制器：仅本文件可见（公开 API 实现同在 fc.c），FTL 不可见
static fc_ctl_s g_fc_controller;

/* ══════════════════════════════════════════════════════════
 * 2. 查询 / 转换小工具
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 判断 operation 是否为带 ECC 编码数据的 Page Read。
 *
 * 这类命令读取的 NAND 页数据经过 ECC 编码，需要 FC 内部在 CQ push 前自动做
 * ECC 解码 + 条件解扰。READ_ID / READ_STATUS / GET_FEATURES 等虽然也有数据返回，
 * 但它们不是 ECC 编码的 Page Data，不应走 Data Path 处理。
 *
 * @param operation NAND 操作码
 *
 * @return true = 是 Page Read；false = 不是
 */
static BOOL fc_is_page_read(BYTE operation)
{
    switch (operation)
    {
        case OP_NORMAL_READ_PAGE_WITH_DATA_OUT:
        case OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT:
        case OP_READ_PAGE_CACHE_START_SEQ:
        case OP_READ_PAGE_CACHE_SEQ:
        case OP_READ_PAGE_CACHE_RANDOM:
        case OP_READ_PAGE_CACHE_LAST:
        case OP_READ_PAGE_CACHE_START_SEQ_MP:
        case OP_READ_PAGE_CACHE_SEQ_MP:
        case OP_READ_PAGE_CACHE_LAST_MP:
        case OP_RANDOM_DATA_READ:
        case OP_RANDOM_DATA_READ_MULTI_PLANE: /* 06h-E0h 多 plane 逐 CW 取数（与 05h 对称） */
            return true;
        default:
            return false;
    }
}

/**
 * @brief 判断 operation 是否为带 ECC 编码数据的 Page Write。
 *
 * 写入 NAND 之前需要对数据做 Random 加扰 + ECC 编码；stripe parity 另由 RAID 路径生成。
 * SET_FEATURES 虽然也写数据，但不是 ECC 编码的 Page Data，不应走 Data Path。
 *
 * @param operation NAND 操作码
 *
 * @return true = 是 Page Write；false = 不是
 */
static BOOL fc_is_page_write(BYTE operation)
{
    switch (operation)
    {
        case OP_NORMAL_PROGRAM_PAGE:
        case OP_PROGRAM_PAGE_CACHE:
        case OP_OTP_DATA_PROGRAM:
        case OP_TWO_PLANE_PROGRAM_CACHE:
        case OP_TWO_PLANE_PROGRAM_CACHE_END:
        case OP_TWO_PLANE_PROGRAM_CACHE_FIRST:
        case OP_PROGRAM_PAGE_MULTI_PLANE:
        case OP_PROGRAM_PAGE_ISSUE: /* Phase V */
            return true;
        default:
            return false;
    }
}

/**
 * @brief 读取填充轮次计数（排障用）
 *
 * @param calls 输出：填充调用次数
 * @param pops  输出：出队次数
 */
static void fc_debug_fill_counters(DWORD *calls, DWORD *pops)
{
    ASSERT(calls != NULL);
    ASSERT(pops != NULL);

    *calls = s_fill_calls;
    *pops  = s_fill_pops;
}

/* ── 资源统计读取器（fc_get_resource_stats 用；本文件内调用） ── */
/**
 * @brief 读命令状态
 *
 * ID 越界即不变量破坏 ⇒ ASSERT。
 *
 * @param controller FC 控制器
 * @param command_id 池内命令 ID
 * @return 命令状态
 */
static fc_cmd_state_e fc_command_state_get(const fc_ctl_s *controller, fc_cmd_id_t command_id)
{
    ASSERT(command_id < FC_COMMAND_POOL_DEPTH);
    return controller->command_state[command_id];
}
/**
 * @brief 读 desc 的归属命令 ID
 *
 * @param controller    FC 控制器
 * @param descriptor_id 池内 desc ID
 * @return 归属命令 ID；FC_CMD_ID_INVALID = 空闲
 *
 * @note 唯一消费者是 fc_get_resource_stats 的 debug 全池校验（#ifndef NDEBUG）；
 *       release 档没有等价读者，故标 unused，避免 -Wunused-function。
 */
static fc_cmd_id_t __attribute__((unused)) fc_descriptor_owner_get(const fc_ctl_s *controller, WORD descriptor_id)
{
    ASSERT(descriptor_id < FC_DESCRIPTOR_POOL_DEPTH);
    return controller->descriptor_owner[descriptor_id];
}
/**
 * @brief 读 buffer 占用状态
 *
 * @param controller FC 控制器
 * @param buffer_id  池内 buffer ID
 * @return buffer 状态
 */
static fc_buf_state_e fc_buffer_state_get(const fc_ctl_s *controller, fc_buf_id_t buffer_id)
{
    ASSERT(buffer_id < FC_BUFFER_POOL_DEPTH);
    return controller->buffer_state[buffer_id];
}

/* ── 命令/描述符 ID ↔ 指针（范围由公开入口校验；本文件内调用） ── */
/**
 * @brief 命令 ID → 命令指针
 *
 * @param controller FC 控制器
 * @param command_id 池内命令 ID
 * @return 命令指针
 */
static fc_cmd_s *fc_pool_command_by_id(fc_ctl_s *controller, WORD command_id)
{
    // 公共壳已断言过 ID 范围；内部 helper 直接取池项。
    return &controller->command_pool[command_id];
}

/**
 * @brief 命令指针 → 池索引（指针 − 池基址）
 *
 * 前置：command 属于本控制器命令池。
 *
 * @param controller FC 控制器
 * @param command    命令指针
 * @return 池索引
 */
static fc_cmd_id_t fc_pool_command_id(const fc_ctl_s *controller, const fc_cmd_s *command)
{
    ASSERT(controller != NULL);
    ASSERT(command != NULL);

    return (fc_cmd_id_t)(command - controller->command_pool);
}

/**
 * @brief 描述符 ID → 描述符指针
 *
 * @param controller    FC 控制器
 * @param descriptor_id 池内 desc ID
 * @return 描述符指针
 */
static fc_desc_s *fc_pool_descriptor_by_id(fc_ctl_s *controller, WORD descriptor_id)
{
    // 公共壳已断言过 ID 范围；内部 helper 直接取池项。
    return &controller->descriptor_pool[descriptor_id];
}

/**
 * @brief buffer ID → CW 基址（528B，不做 USER 偏移）
 *
 * @param controller FC 控制器
 * @param buffer_id  池内 buffer ID
 * @return CW 基址
 */
static BYTE *fc_pool_buffer_cw_base(fc_ctl_s *controller, fc_buf_id_t buffer_id)
{
    // 公共壳已断言过 ID 范围；内部 helper 直接取池项。
    return controller->buffer_pool[buffer_id];
}

/**
 * @brief 命令状态唯一 setter：只允许状态机定义的转移。
 *
 * @param controller FC 控制器
 * @param command_id 命令池槽位号
 * @param next       目标状态
 *
 * @note 非法转移在 debug 下 ASSERT；release 下仍写入目标值（调用链已保证前置，
 *       这里只收口状态字段的唯一写点，避免多处直接赋值漂移）。
 */
static void fc_pool_command_state_set(fc_ctl_s *controller, fc_cmd_id_t command_id, fc_cmd_state_e next)
{
    const fc_cmd_state_e current = controller->command_state[command_id];
    BOOL                 allowed = false;

    switch (next)
    {
        case FC_COMMAND_STATE_ALLOCATED:
            allowed = (current == FC_COMMAND_STATE_FREE);
            break;
        case FC_COMMAND_STATE_QUEUED:
            allowed = (current == FC_COMMAND_STATE_ALLOCATED);
            break;
        case FC_COMMAND_STATE_ACTIVE:
            allowed = (current == FC_COMMAND_STATE_QUEUED);
            break;
        case FC_COMMAND_STATE_COMPLETED:
            allowed = (current == FC_COMMAND_STATE_ACTIVE);
            break;
        case FC_COMMAND_STATE_FREE:
            allowed = (current == FC_COMMAND_STATE_ALLOCATED) || (current == FC_COMMAND_STATE_COMPLETED);
            break;
        default:
            allowed = false;
            break;
    }
    ASSERT(allowed);
    (void)allowed; /* NDEBUG 下 ASSERT 裁空，显式标记仅用于断言 */
    controller->command_state[command_id] = next;
}

/* ══════════════════════════════════════════════════════════
 * 3. 初始化
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 初始化 FC 控制器：清三池/队列/状态位图并挂指针
 *
 * 三池本体是文件作用域 static，地址在固件生命周期内固定；可重复调用以重置。
 *
 * @return FC_STATUS_OK        初始化成功。
 *         FC_STATUS_IO_ERROR  NAND 派发层初始化失败。
 */
SSD_CODE_FAST fc_status_e fc_init(void)
{
    fc_ctl_s *controller = &g_fc_controller;
    WORD      index;

    /* ── ① 清底：三池本体 + 三条队列的存储 + 命令状态位图全部归零。
     *    这些数组都是文件作用域 static（见 §1 静态资源），此处只清内容，
     *    数组地址不变——总线看到的物理地址在整个固件生命周期内固定。 ── */
    memset(g_fc_command_pool, 0, sizeof(g_fc_command_pool));
    memset(g_fc_descriptor_pool, 0, sizeof(g_fc_descriptor_pool));
    memset(g_fc_buffer_pool, 0, sizeof(g_fc_buffer_pool));
    memset(g_fc_sq_storage, 0, sizeof(g_fc_sq_storage));
    memset(g_fc_hsq_storage, 0, sizeof(g_fc_hsq_storage));
    memset(g_fc_cq_storage, 0, sizeof(g_fc_cq_storage));
    memset(g_fc_command_state, 0, sizeof(g_fc_command_state));
    memset(g_fc_return_staging, 0, sizeof(g_fc_return_staging));

    /* ── ② 把池地址挂进 controller。三池本体与各簿记数组是 static，
     *    fc_ctl_s 只存指针——这样 fc_ctl_s 可以整体赋值/传递而不搬运大数组。 ── */
    memset(controller, 0, sizeof(*controller));
    controller->command_pool         = g_fc_command_pool;
    controller->descriptor_pool      = g_fc_descriptor_pool;
    controller->buffer_pool          = g_fc_buffer_pool;
    controller->descriptor_owner     = g_fc_descriptor_owner;
    controller->command_state        = g_fc_command_state;
    controller->command_free_next    = g_fc_command_free_next;
    controller->descriptor_free_next = g_fc_descriptor_free_next;
    controller->buffer_free_next     = g_fc_buffer_free_next;
    controller->buffer_state         = g_fc_buffer_state;
    controller->buffer_owner         = g_fc_buffer_owner;

    /* ── ③ 三条环形队列：SQ（普通命令）/ HSQ（高优先，如 read retry）/
     *    CQ（完成项）。init 只登记存储与深度，head/tail/count 归零。 ── */
    fc_submission_queue_init(&controller->sq, g_fc_sq_storage, FC_SQ_DEPTH);
    fc_submission_queue_init(&controller->hsq, g_fc_hsq_storage, FC_HSQ_DEPTH);
    fc_completion_queue_init(&controller->cq, g_fc_cq_storage, FC_CQ_DEPTH);
    /* ── ④ 调度器绑定 SQ/HSQ 指针（"忙"查 NAND 侧 fc_nand_dispatch_lun_free，无 busy 表） ── */
    controller->scheduler.sq  = &controller->sq;
    controller->scheduler.hsq = &controller->hsq;

    /* ── ⑤ 本函数里唯一的 NAND 依赖：只清 NAND 侧的通道队列与 per-LUN 链状态
     *    （纯 memset，不碰 FMC 硬件）。硬件那半由 fc_nand_init() 负责，且在 main()
     *    里排在 fc_init() 之后——正因为这里不碰硬件，FC 才能先初始化。 ── */
    if (fc_nand_dispatch_init() != NAND_RESULT_OK)
    {
        return FC_STATUS_IO_ERROR;
    }

    // 初始化 RAID 引擎上下文（每个 LUN 独立）
#if CONFIG_FC_RAID
    for (BYTE lun = 0U; lun < LUN_NUM; lun++)
    {
        fc_raid_init_ctx(lun);
    }
#endif /* CONFIG_FC_RAID */

    /* ── ⑥ CRC 硬件（STM32H743 CRC 外设）：写路径编码时给每个 CW 算 CRC ── */
    fc_crc_init();

    // 初始化 Stripe RAID 积累状态
#if CONFIG_FC_RAID
    for (BYTE lun = 0U; lun < LUN_NUM; lun++)
    {
        memset(&controller->stripe_accum[lun], 0, sizeof(controller->stripe_accum[lun]));
    }
    for (index = 0U; index < FC_COMMAND_POOL_DEPTH; index++)
    {
        controller->stripe_parity_for_data[index] = FC_CMD_ID_INVALID;
    }
#endif /* CONFIG_FC_RAID */

    /* ── ⑦ 串空闲链表。三张链都是"下标即 ID"的静态链，分配 = 取 head 并把
     *    head 推到 next，释放 = 头插（见 §4 资源分配 / 回滚 / 释放）：
     *      command_free_next    : 0→1→…→FC_COMMAND_POOL_DEPTH-1 → FC_CMD_ID_INVALID
     *      descriptor_free_next : 0→1→…→FC_DESCRIPTOR_POOL_DEPTH-1 → FC_DESC_ID_INVALID
     *      buffer_free_next     : 0→1→…→FC_BUFFER_POOL_DEPTH-1 → FC_BUF_ID_INVALID ── */
    for (index = 0U; index < FC_COMMAND_POOL_DEPTH; index++)
    {
        controller->command_free_next[index] = ((index + 1U) < FC_COMMAND_POOL_DEPTH) ? (fc_cmd_id_t)(index + 1U) : FC_CMD_ID_INVALID;
    }
    // 串接描述符空闲链表 + 清归属表
    for (index = 0U; index < FC_DESCRIPTOR_POOL_DEPTH; index++)
    {
        controller->descriptor_owner[index]     = FC_CMD_ID_INVALID;
        controller->descriptor_free_next[index] = ((index + 1U) < FC_DESCRIPTOR_POOL_DEPTH) ? (index + 1U) : FC_DESC_ID_INVALID;
    }
    // 串接 buffer 空闲链表 + 清占用态/归属
    for (index = 0U; index < FC_BUFFER_POOL_DEPTH; index++)
    {
        controller->buffer_free_next[index] = ((index + 1U) < FC_BUFFER_POOL_DEPTH) ? (index + 1U) : FC_BUF_ID_INVALID;
        controller->buffer_state[index]     = FC_BUFFER_STATE_FREE;
        controller->buffer_owner[index]     = FC_CMD_ID_INVALID;
    }
    controller->command_free_head    = 0U;
    controller->descriptor_free_head = 0U;
    controller->buffer_free_head     = 0U;

    return FC_STATUS_OK;
}

/* ══════════════════════════════════════════════════════════
 * 4. 资源分配 / 回滚 / 释放
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 分配命令槽位（清零 + desc_id=INVALID）
 *
 * 不清 command_id——结构体无此字段，ID 由指针派生。
 *
 * @return 命令指针；NULL = 命令池空
 */
fc_cmd_s *fc_command_alloc(void)
{
    fc_ctl_s   *controller = &g_fc_controller;
    fc_cmd_id_t command_id;

    command_id = controller->command_free_head;
    if (command_id == FC_CMD_ID_INVALID)
    {
        return NULL;
    }
    ASSERT_REJECT(controller->command_state[command_id] == FC_COMMAND_STATE_FREE, NULL, "command alloc: cmd=%hu state=%d", command_id, controller->command_state[command_id]);
    controller->command_free_head = controller->command_free_next[command_id];
    memset(&controller->command_pool[command_id], 0, sizeof(controller->command_pool[command_id]));
    controller->command_pool[command_id].desc_id = FC_DESC_ID_INVALID;
    fc_pool_command_state_set(controller, command_id, FC_COMMAND_STATE_ALLOCATED);
    controller->command_used++;
    if (controller->command_used > controller->command_high)
    {
        controller->command_high = controller->command_used;
    }
    return &controller->command_pool[command_id];
}

/**
 * @brief 分配 desc 并登记归属（不挂链）
 *
 * 出池即自洽：next_id/buffer_id 显式置哨兵。
 *
 * @param controller FC 控制器
 * @param command    归属命令
 * @return desc 指针；NULL = desc 池空
 */
static fc_desc_s *fc_pool_descriptor_alloc(fc_ctl_s *controller, fc_cmd_s *command)
{
    WORD        descriptor_index;
    fc_cmd_id_t command_id;

    command_id       = fc_pool_command_id(controller, command);
    descriptor_index = controller->descriptor_free_head;
    if (descriptor_index == FC_DESC_ID_INVALID)
    {
        return NULL;
    }
    ASSERT_REJECT(controller->descriptor_owner[descriptor_index] == FC_CMD_ID_INVALID, NULL, "descriptor alloc: desc=%hu owner=%hu", descriptor_index, controller->descriptor_owner[descriptor_index]);
    controller->descriptor_free_head = controller->descriptor_free_next[descriptor_index];
    memset(&controller->descriptor_pool[descriptor_index], 0, sizeof(controller->descriptor_pool[descriptor_index]));
    controller->descriptor_pool[descriptor_index].next_id   = FC_DESC_ID_INVALID;
    controller->descriptor_pool[descriptor_index].buffer_id = FC_BUF_ID_INVALID;
    controller->descriptor_owner[descriptor_index]          = command_id;
    controller->descriptor_used++;
    if (controller->descriptor_used > controller->descriptor_high)
    {
        controller->descriptor_high = controller->descriptor_used;
    }
    return &controller->descriptor_pool[descriptor_index];
}

/**
 * @brief 把描述符还回空闲链（清归属、清本体、头插空闲链）。
 *
 * @param controller FC 控制器
 * @param descriptor_index 池内下标。
 *
 * @note **不碰数据链、不碰 buffer**——只把 desc 本体还池。调用者负责
 * 先把该 desc 从链上摘掉（见 fc_pool_chain_release_head）。
 */
static void fc_pool_descriptor_release(fc_ctl_s *controller, WORD descriptor_index)
{
    ASSERT_REJECT_VOID(controller->descriptor_owner[descriptor_index] != FC_CMD_ID_INVALID, "descriptor release: desc=%hu already free", descriptor_index);
    ASSERT(controller->descriptor_used > 0U);
    memset(&controller->descriptor_pool[descriptor_index], 0, sizeof(controller->descriptor_pool[descriptor_index]));
    controller->descriptor_owner[descriptor_index]     = FC_CMD_ID_INVALID;
    controller->descriptor_free_next[descriptor_index] = controller->descriptor_free_head;
    controller->descriptor_free_head                   = descriptor_index;
    controller->descriptor_used--;
}

/**
 * @brief 分配一个 buffer
 *
 * @return buffer ID；FC_BUF_ID_INVALID = 池空
 */
fc_buf_id_t fc_buffer_alloc(void)
{
    fc_ctl_s *controller = &g_fc_controller;
    WORD      buffer_index;

    buffer_index = controller->buffer_free_head;
    if (buffer_index == FC_BUF_ID_INVALID)
    {
        return FC_BUF_ID_INVALID;
    }
    ASSERT_REJECT(controller->buffer_state[buffer_index] == FC_BUFFER_STATE_FREE, FC_BUF_ID_INVALID, "buffer alloc: id=%hu state=%d", buffer_index, controller->buffer_state[buffer_index]);
    controller->buffer_free_head           = controller->buffer_free_next[buffer_index];
    controller->buffer_state[buffer_index] = FC_BUFFER_STATE_OCCUPIED;
    controller->buffer_owner[buffer_index] = FC_CMD_ID_INVALID;
    controller->buffer_used++;
    if (controller->buffer_used > controller->buffer_high)
    {
        controller->buffer_high = controller->buffer_used;
    }
    return buffer_index;
}

/**
 * @brief 释放 buffer（校验归属 → 状态清零 → 挂回空闲链表头）
 *
 * @param controller FC 控制器
 * @param buffer_id  池内 buffer ID
 * @param owner      期望归属：命令 ID；上层预申请未挂链时为 FC_CMD_ID_INVALID
 *
 * @return true = 已释放；false = 未占用或归属不符（release 档拒绝，空闲链保持完整）。
 */
static BOOL fc_pool_buffer_release(fc_ctl_s *controller, fc_buf_id_t buffer_id, fc_cmd_id_t owner)
{
    // release 档也必须先判下标，再访问数组（ASSERT 在 NDEBUG 下不存在）。
    ASSERT_REJECT(buffer_id < FC_BUFFER_POOL_DEPTH, false, "buffer release: id=%hu out of range", buffer_id);
    // 判据 9：归属不符/未占用时拒绝，避免同一节点二次入链造成静默介质破坏。
    ASSERT_REJECT((controller->buffer_state[buffer_id] == FC_BUFFER_STATE_OCCUPIED) && (controller->buffer_owner[buffer_id] == owner), false, "buffer release: id=%hu state=%d owner=%hu expected=%hu", buffer_id, controller->buffer_state[buffer_id], controller->buffer_owner[buffer_id], owner);
    ASSERT(controller->buffer_used > 0U);

    controller->buffer_state[buffer_id]     = FC_BUFFER_STATE_FREE;
    controller->buffer_owner[buffer_id]     = FC_CMD_ID_INVALID;
    controller->buffer_free_next[buffer_id] = controller->buffer_free_head;
    controller->buffer_free_head            = buffer_id;
    controller->buffer_used--;
    return true;
}

/**
 * @brief 释放链头一个 desc（含其 buffer），头指针推进到 next
 *
 * 先读 next_id 再释放（释放会整体清零）；无 buffer 的 desc 跳过 buffer 释放。
 *
 * @param controller FC 控制器
 * @param head       链头（进出参）
 */
static void fc_pool_chain_release_head(fc_ctl_s *controller, fc_desc_id_t *head)
{
    fc_desc_id_t descriptor_id;
    fc_buf_id_t  buffer_id;

    if (*head >= FC_DESCRIPTOR_POOL_DEPTH)
    {
        return;
    }
    descriptor_id           = *head;
    const fc_cmd_id_t owner = controller->descriptor_owner[descriptor_id];

    // 成环时链头会回到已经释放的 desc：owner 已清 INVALID。先断链再返回，
    // 避免把已清零的 buffer_id/next_id 当成有效数据继续释放。
    if (owner == FC_CMD_ID_INVALID)
    {
        *head = FC_DESC_ID_INVALID;
        ASSERT_MSG(owner != FC_CMD_ID_INVALID, "descriptor chain cycle at desc=%hu", descriptor_id);
        return;
    }

    buffer_id = (fc_buf_id_t)controller->descriptor_pool[descriptor_id].buffer_id;
    *head     = (fc_desc_id_t)controller->descriptor_pool[descriptor_id].next_id;

    if (buffer_id != FC_BUF_ID_INVALID)
    {
        const BOOL released = fc_pool_buffer_release(controller, buffer_id, owner);

        ASSERT(released);
        (void)released;
    }
    fc_pool_descriptor_release(controller, descriptor_id);
}

/**
 * @brief 沿链释放全部 desc + buffer，头指针置 INVALID
 *
 * @param controller FC 控制器
 * @param head       链头（进出参）
 */
static void fc_pool_chain_release_all(fc_ctl_s *controller, fc_desc_id_t *head)
{
    // 链在上界内仍可能成环；释放路径必须有界，不能靠调用者先校验。
    for (WORD steps = 0U; (*head < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH); steps++)
    {
        fc_pool_chain_release_head(controller, head);
    }
    ASSERT(*head == FC_DESC_ID_INVALID);
    *head = FC_DESC_ID_INVALID;
}

/**
 * @brief 沿命令的 desc 链归还**全部 buffer**，并把各 desc 的 buffer_id 抹成 FC_BUF_ID_INVALID（**幂等**：已 INVALID 的跳过）。
 *
 * @param controller FC 控制器
 * @param command 命令。
 *
 * @note 只动 buffer，**不动 desc 链、不动命令槽位**——它们仍由
 * fc_command_release 回收。唯一调用者：fc_complete_from_nand 在
 * 「buf_own 的释放位 = FC 释放（01/11）」时当场归还（此时完成项已组装完毕，
 * 之后没有任何读者碰 buffer）。抹 INVALID 是为了让随后的链释放自然跳过，
 * 从而不双重归还；命令若没走到完成就释放，buffer 仍有效，链释放照常归还
 * ⇒ 不泄漏。归属语义见 fc_buf_own_e。
 */
static void fc_pool_command_release_buffers(fc_ctl_s *controller, fc_cmd_s *command)
{
    const fc_cmd_id_t command_id = fc_pool_command_id(controller, command);
    fc_desc_id_t      desc_id    = (fc_desc_id_t)command->desc_id;

    for (WORD steps = 0U; (desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH); steps++)
    {
        fc_desc_s  *descriptor = &controller->descriptor_pool[desc_id];
        fc_buf_id_t buffer_id  = (fc_buf_id_t)descriptor->buffer_id;

        if (buffer_id != FC_BUF_ID_INVALID)
        {
            const BOOL released = fc_pool_buffer_release(controller, buffer_id, command_id);

            ASSERT(released);
            if (released == true)
            {
                descriptor->buffer_id = FC_BUF_ID_INVALID;
            }
        }
        desc_id = (fc_desc_id_t)descriptor->next_id;
    }
    ASSERT(desc_id == FC_DESC_ID_INVALID);
}

/**
 * @brief 提交前的一次性 desc 链校验（归属 + 唯一性 + 坐标 + buffer 占用）。
 *
 * 这是建链/提交路径的**唯一**完整校验漏斗：逐 desc 的建链入口只保留逐项坐标断言，
 * 整链在提交前扫一次；不在每次 append 时做 O(n) 查重。
 *
 * @param controller FC 控制器
 * @param command_id 命令 ID
 * @param command    命令
 * @return true 链完整、归属一致、CW 不重复、buffer（若非 INVALID）已占用；false 否则
 *
 * @note 调用点只有 fc_submit / fc_submit_batch 的 ASSERT（release 档无消费者），
 *       故标 unused；释放档的完整性由 FTL 调用链保证（RISK-POOL-02）。
 */
static BOOL __attribute__((unused)) fc_pool_chain_validate(const fc_ctl_s *controller, fc_cmd_id_t command_id, const fc_cmd_s *command)
{
    fc_desc_id_t descriptor_id = (fc_desc_id_t)command->desc_id;
    DWORD        seen          = 0U;
    WORD         count         = 0U;
    BYTE         plane_first;
    BYTE         plane_last;

    (void)fc_command_plane_span(command, &plane_first, &plane_last);

    while (descriptor_id != FC_DESC_ID_INVALID)
    {
        if (descriptor_id >= FC_DESCRIPTOR_POOL_DEPTH)
        {
            return false;
        }
        if (controller->descriptor_owner[descriptor_id] != command_id)
        {
            return false;
        }
        /* 链长上界同时防环：超过 desc 池容量必然有重复节点。 */
        if (count >= FC_DESCRIPTOR_POOL_DEPTH)
        {
            return false;
        }

        const fc_desc_s *descriptor = &controller->descriptor_pool[descriptor_id];
        const BYTE       plane      = paa_to_plane(descriptor->paa);
        const BYTE       cw         = paa_to_cw(descriptor->paa);
        const DWORD      n          = (DWORD)plane * CW_NUM + cw;

        if ((plane < plane_first) || (plane >= plane_last) || (cw >= CW_NUM) || (n >= CW_TOTAL_MAX))
        {
            return false;
        }
        if ((seen & (1UL << n)) != 0UL)
        {
            return false;
        }
        seen |= (1UL << n);

        if (descriptor->buffer_id != FC_BUF_ID_INVALID)
        {
            if ((descriptor->buffer_id >= FC_BUFFER_POOL_DEPTH) || (controller->buffer_state[descriptor->buffer_id] == FC_BUFFER_STATE_FREE) || (controller->buffer_owner[descriptor->buffer_id] != command_id))
            {
                return false;
            }
        }

        count++;
        descriptor_id = (fc_desc_id_t)descriptor->next_id;
    }
    return true;
}

// desc 链释放只有 fc_pool_chain_release_all 一份实现（唯一漏斗）；
// 不设只放 desc、不放命令槽的中间态回滚——调用点后面紧跟
// fc_pool_command_release()，它把「desc 链 + buffer + 命令槽位」一并做完。

/**
 * @brief 释放命令：沿链释放 desc+buffer，命令槽位回 FREE（FREE 幂等）
 *
 * 调用链保证只在「未提交」或「完成项已取走」后调用；在飞释放是调用者 bug，
 * debug 下 ASSERT。release 下由 FTL 的 submitted/drain 契约保证。
 *
 * @param controller FC 控制器
 * @param command    命令
 */
static void fc_pool_command_release(fc_ctl_s *controller, fc_cmd_s *command)
{
    fc_cmd_id_t    command_id;
    fc_desc_id_t   head;
    fc_cmd_state_e state;

    command_id = fc_pool_command_id(controller, command);
    state      = controller->command_state[command_id];

    if (state == FC_COMMAND_STATE_FREE)
    {
        return;
    }
    ASSERT((state == FC_COMMAND_STATE_ALLOCATED) || (state == FC_COMMAND_STATE_COMPLETED));

    // 沿链释放 desc + buffer（buffer 随链一并归还）。
    // desc_id 是位域取不了地址，故用局部头变量中转。
    head = (fc_desc_id_t)command->desc_id;
    fc_pool_chain_release_all(controller, &head);

    ASSERT(controller->command_used > 0U);
    memset(&controller->command_pool[command_id], 0, sizeof(controller->command_pool[command_id]));
    fc_pool_command_state_set(controller, command_id, FC_COMMAND_STATE_FREE);
    controller->command_free_next[command_id] = controller->command_free_head;
    controller->command_free_head             = command_id;
    controller->command_used--;
}

/* ══════════════════════════════════════════════════════════
 * 5. desc 链原语
 *   建链入口（校验 + 出池 desc/buffer + 填字段 + 挂链的编排）收敛到
 *   fc_command_configure_descriptor（§11）；本节只留池/链原语。
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 把已填好的 desc 追加到命令链尾
 *
 * 空链即链头；前驱 chain 置 MORE，末条保持 END。调用者保证 desc 自洽。
 *
 * @param controller FC 控制器
 * @param command    命令
 * @param descriptor 待挂 desc
 */
static void fc_pool_chain_append(fc_ctl_s *controller, fc_cmd_s *command, fc_desc_s *descriptor)
{
    const fc_desc_id_t new_id = (fc_desc_id_t)(descriptor - controller->descriptor_pool);
    fc_desc_id_t       tail   = (fc_desc_id_t)command->desc_id;

    if (tail == FC_DESC_ID_INVALID)
    {
        command->desc_id = new_id; /* 空链：本条即链头 */
        return;
    }
    ASSERT_REJECT_VOID(tail < FC_DESCRIPTOR_POOL_DEPTH, "chain append: head=%hu out of range", tail);

    WORD steps = 0U;
    while ((controller->descriptor_pool[tail].next_id != FC_DESC_ID_INVALID) && (steps < FC_DESCRIPTOR_POOL_DEPTH))
    {
        tail = (fc_desc_id_t)controller->descriptor_pool[tail].next_id;
        steps++;
    }
    ASSERT(controller->descriptor_pool[tail].next_id == FC_DESC_ID_INVALID);
    if (controller->descriptor_pool[tail].next_id != FC_DESC_ID_INVALID)
    {
        return;
    }
    controller->descriptor_pool[tail].config.chain = 1U;
    controller->descriptor_pool[tail].next_id      = new_id;
}

/* ══════════════════════════════════════════════════════════
 * 6. 提交
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 命令入队（按 high_priority 选 HSQ/SQ），状态置 QUEUED
 *
 * @param controller FC 控制器
 * @param command    命令
 * @return FC_STATUS_OK；FC_STATUS_QUEUE_FULL
 */
static fc_status_e fc_sched_submit(fc_ctl_s *controller, fc_cmd_s *command)
{
    fc_sub_queue_s   *queue      = (command->high_priority != 0U) ? &controller->hsq : &controller->sq;
    const fc_cmd_id_t command_id = fc_pool_command_id(controller, command);

    // 重复提交会让同一命令在 SQ/HSQ 中出现两次；调用链保证 ALLOCATED。
    ASSERT(controller->command_state[command_id] == FC_COMMAND_STATE_ALLOCATED);

    if (fc_submission_queue_push(queue, command_id) != FC_STATUS_OK)
    {
        return FC_STATUS_QUEUE_FULL;
    }

    fc_pool_command_state_set(controller, command_id, FC_COMMAND_STATE_QUEUED);
    return FC_STATUS_OK;
}

/* ══════════════════════════════════════════════════════════
 * 7. 完成
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 完成项入 CQ，命令状态置 COMPLETED（供 fc_complete_from_nand 使用）。
 *
 * @param controller FC 控制器
 * @param completion 待入队的完成项（command_id 须为池内 ID）
 *
 * @return FC_STATUS_OK = 入队成功且状态已置 COMPLETED
 *         FC_STATUS_QUEUE_FULL = CQ 满，完成项未入队
 */
static fc_status_e fc_sched_complete(fc_ctl_s *controller, fc_cmd_status_s *completion)
{
    fc_status_e result;
    fc_cmd_id_t command_id;

    command_id = completion->command_id;
    ASSERT(command_id < FC_COMMAND_POOL_DEPTH);
    // 只有执行中的命令能产生完成项；调用链保证 ACTIVE。
    ASSERT(controller->command_state[command_id] == FC_COMMAND_STATE_ACTIVE);

    result = fc_completion_queue_push(&controller->cq, completion);
    if (result == FC_STATUS_OK)
    {
        fc_pool_command_state_set(controller, command_id, FC_COMMAND_STATE_COMPLETED);
    }
    return result;
}

/**
 * @brief NAND 完成 → 回填 descriptor 状态/ECC → 数据路径 → 组装 CQ 完成项。
 *
 * @param controller FC 控制器
 * @param nand_completion NAND 层完成。
 *
 * @return FC_STATUS_OK / IO_ERROR（CQ 满强制 COMPLETED 防泄漏）。
 *
 * @note Step 1：把 nand_completion 的 per-CW status/ecc_bits 写回 desc。
 * - Step 2：Page Read 命令做 data path（ECC 解码 + 条件解扰）。
 * - Step 3：组装完成项——page read 从 desc 读 ECC 解码后状态
 * （聚合 status + 前 4 CW ECC + 首个失败 CW 的 failed_desc_id，见 FC_CW_STATUS_ERROR_MASK），
 * 非 page read 直接透传 NAND 状态。
 * - CQ 满 → 强制 COMPLETED（数据路径已跑，CQ entry 丢失是 fatal）。
 */
static fc_status_e fc_complete_from_nand(fc_ctl_s *controller, const fc_nand_dispatch_comp_s *nand_completion)
{
    fc_cmd_status_s completion;
    fc_cmd_s       *command;
    BOOL            data_path_failed = false;

    // 完成项由 nand_dispatch 侧产出，command_id 必在池范围内
    ASSERT(nand_completion != NULL);
    ASSERT(nand_completion->command_id < FC_COMMAND_POOL_DEPTH);

    command = &controller->command_pool[nand_completion->command_id];
    memset(&completion, 0, sizeof(completion));

    ASSERT(controller->command_state[nand_completion->command_id] == FC_COMMAND_STATE_ACTIVE);

    /* ── Step 1: Write back per-CW status and ECC count from NAND completion ──
     * cw_status[] 的下标是 **flat 绝对 CW 号 n = plane*CW_NUM + cw**（与请求掩码同空间），
     * 故这里按 desc 自己的 PAA 算出 n 直接取。 */
    if (command->desc_id != FC_DESC_ID_INVALID)
    {
        fc_desc_id_t desc_id = (fc_desc_id_t)command->desc_id;

        for (WORD steps = 0U; (desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH); steps++)
        {
            fc_desc_s *descriptor = &controller->descriptor_pool[desc_id];
            BYTE       n          = (paa_to_plane(descriptor->paa) * CW_NUM + paa_to_cw(descriptor->paa));

            ASSERT(n < CW_TOTAL_MAX);
            descriptor->status   = nand_completion->cw_status[n];
            descriptor->ecc_bits = nand_completion->ecc_bit_count[n];
            desc_id              = (fc_desc_id_t)descriptor->next_id;
        }
        ASSERT(desc_id == FC_DESC_ID_INVALID);
    }

    /* ── Step 2: Data path processing (ECC decode + conditional descramble) ──
     * 仅对 Page Read 命令执行。READ_ID / GET_FEATURES 等命令
     * 返回的数据不是 ECC 编码的 NAND 页数据，不应进入 Data Path。 */
    // 合成失败（execution_status == NOT_READY）没有可解码的数据，且 buffer 可能未分配；
    // 不要进 ECC 解码路径，直接按失败 completion 上报。
    if ((fc_is_page_read(command->config.operation) == true) && (nand_completion->execution_status != NAND_RESULT_NOT_READY))
    {
        const fc_status_e data_result = fc_data_path_process_read(command);

        // process_read 失败（UNC / ECC 引擎不可用 / buffer 缺失 / 链损坏）都必须
        // 反映到命令级状态，不能只看 desc->status 是否被填过。
        data_path_failed = (data_result != FC_STATUS_OK);
    }

    /* ── Step 3: Populate completion entry ──
     * CQ 只带命令级聚合：status + 返回段 param。per-CW ECC 只留在 desc 链，
     * 聚合 ECC 走 fc_get_cmd_ecc_max()（须在 fc_command_release() 前调用）。 */
    completion.command_id     = nand_completion->command_id;
    completion.failed_cw      = nand_completion->failed_cw;
    completion.failed_desc_id = FC_DESC_ID_INVALID;
    completion.status         = 0U;
    /* param 已由上面的整体 memset 清零；仅 GET_FEATURES / OP_READ_META /
     * OP_READ_ONFI_SIGNATURE / OP_READ_COLUMN / BLOCK_LOCK_STATUS 会再填。 */

    /* 命令级状态 = NAND 前/后两次读状态的位 + 数据路径聚合。
     * 前一次（pre_status）：RDY/ARDY/WP —— 命令下发时的器件状态；
     * 后一次（nand_status）：FAIL/FAILC —— 本次操作结果。
     * 命令成功 = 错误位全 0，**不是 RDY**。 */
    {
        const BYTE pre  = nand_completion->pre_status;
        const BYTE post = nand_completion->nand_status;
        const BYTE exec = nand_completion->execution_status;
        DWORD      cmd  = 0U;

        // post 不一定是 70h 状态寄存器：BLOCK_LOCK_STATUS 的 7Ah 字节也经
        // DATA_OUT_STATUS 进 nand_status（bit0 是 LT，不是 FAIL），不能按状态位映射。
        const BOOL post_is_70h = (command->config.operation != OP_BLOCK_LOCK_STATUS);

        if ((post_is_70h == true) && ((post & NAND_STATUS_FAIL) != 0U))
        {
            cmd |= FC_CMD_STATUS_NAND_FAIL;
        }
        // FAILC 只在 cache program 的**中间页/末页**有意义；首页(49h)与非 cache op
        // 的状态字节里 bit1 可能是上一批残留，不能映射（与 CHECK_FAIL_CACHE 的
        // 分 op 判据一致：FIRST 只查 FAIL）。
        const BOOL fail_cache_valid = (command->config.operation == OP_TWO_PLANE_PROGRAM_CACHE) || (command->config.operation == OP_TWO_PLANE_PROGRAM_CACHE_END);

        if ((post_is_70h == true) && (fail_cache_valid == true) && ((post & NAND_STATUS_FAILC) != 0U))
        {
            cmd |= FC_CMD_STATUS_FAIL_CACHE;
        }
        if ((pre & NAND_STATUS_READY) != 0U)
        {
            cmd |= FC_CMD_STATUS_RDY;
        }
        if ((pre & NAND_STATUS_ARDY) != 0U)
        {
            cmd |= FC_CMD_STATUS_AREADY;
        }
        if ((pre & NAND_STATUS_WP) != 0U)
        {
            cmd |= FC_CMD_STATUS_WP;
        }

        if (exec == NAND_RESULT_TIMEOUT)
        {
            cmd |= FC_CMD_STATUS_TIMEOUT;
        }
        else if ((exec != NAND_RESULT_OK) && ((cmd & (FC_CMD_STATUS_NAND_FAIL | FC_CMD_STATUS_FAIL_CACHE)) == 0U))
        {
            // 构建/提交失败注入（exec = NOT_READY）没有状态字节：按 NAND_FAIL 上报。
            cmd |= FC_CMD_STATUS_NAND_FAIL;
        }

        if (command->desc_id != FC_DESC_ID_INVALID)
        {
            // per-CW：先把 NAND 的 per-CW 结果并进 desc，再把 desc 聚合成命令级。
            // **不只 page read**：program 的 desc 也要拿到 FAIL，否则 FTL 的程序失败
            // 平面归因只能回退 plane0（ftl.c 的 bad_plane_mask）。
            // 失败类别（UNC/CRC/META/RAID）只留在 desc；命令级只保留 DATA_ERROR/META。
            fc_desc_id_t did        = (fc_desc_id_t)command->desc_id;
            BYTE         found_fail = 0U;
            BYTE         slot       = 0U;

            while ((did < FC_DESCRIPTOR_POOL_DEPTH) && (slot < CW_TOTAL_MAX))
            {
                fc_desc_s *d    = &controller->descriptor_pool[did];
                // cw_status[] 是 flat 绝对编号（plane*CW_NUM+cw）；desc 链可能只覆盖子集
                // （flat 掩码读），不能拿链序 slot 直接当索引。
                const BYTE flat = (BYTE)((paa_to_plane(d->paa) * CW_NUM) + paa_to_cw(d->paa));

                d->status |= nand_completion->cw_status[flat];

                if ((d->status & FC_CW_STATUS_FAIL) != 0U)
                {
                    cmd |= FC_CMD_STATUS_NAND_FAIL;
                }
                if ((d->status & FC_CW_STATUS_META_ERROR) != 0U)
                {
                    cmd |= FC_CMD_STATUS_META_ERROR;
                }
                if ((d->status & (FC_CW_STATUS_UNC | FC_CW_STATUS_CRC_ERROR | FC_CW_STATUS_RAID_ERROR)) != 0U)
                {
                    cmd |= FC_CMD_STATUS_DATA_ERROR;
                }
                if ((found_fail == 0U) && fc_cw_status_has_error(d->status))
                {
                    completion.failed_desc_id = did;
                    found_fail                = 1U;
                }
                did = (fc_desc_id_t)d->next_id;
                slot++;
            }
        }

        if (data_path_failed == true)
        {
            cmd |= FC_CMD_STATUS_DATA_ERROR;
        }

        // 非 page read：命令级状态已由上面的 NAND 位给出；下面只处理特殊返回段。
        // OP_BLOCK_LOCK_STATUS：7Ah 的状态字节由引擎直读 DQ 总线送进
        // completion->nand_status（NAND_UOP_DATA_OUT_STATUS），不经数据缓冲 ⇒ 只能从这里取。
        if (command->config.operation == OP_BLOCK_LOCK_STATUS)
        {
            completion.param.bytes[0] = nand_completion->nand_status;
        }
        // GET_FEATURES / OP_READ_META / OP_READ_ONFI_SIGNATURE / OP_READ_COLUMN：引擎把
        // 寄存器值 / 页内 META / 器件 ONFI 签名 / 按列读的 4 字节写进控制器侧暂存，拷进 CQ
        // 返回段（都用 4 字节暂存 g_fc_return_staging）。
        else if ((command->config.operation == OP_GET_FEATURES) || (command->config.operation == OP_READ_META) || (command->config.operation == OP_READ_ONFI_SIGNATURE) || (command->config.operation == OP_READ_COLUMN))
        {
            BYTE i;

            for (i = 0U; i < 4U; i++)
            {
                completion.param.bytes[i] = g_fc_return_staging[nand_completion->command_id][i];
            }
        }

        completion.status = cmd;
    }

    /* ── Step 4: buffer 归属 = FC 释放（buf_own bit0 = 1，即 01/11）时当场归还 ──
     * 完成项已组装完毕（Step 3 之后没有任何读者碰 buffer），此处归还并把各 desc 的
     * buffer_id 抹成 INVALID ⇒ 随后 fc_command_release 沿链释放时自然跳过，不双重归还。
     * 00/10（上层释放）不在这里动。放在 fc_sched_complete 之前，
     * 顺带覆盖 CQ 满强制 COMPLETED 的 fatal 分支。 */
    if ((command->config.buf_own & FC_BUF_OWN_FREE_BY_FC) != 0U)
    {
        fc_pool_command_release_buffers(controller, command);
    }

    {
        const fc_status_e complete_status = fc_sched_complete(controller, &completion);

        if (complete_status == FC_STATUS_QUEUE_FULL)
        {
            // CQ 满：完成项没有进 CQ，FTL 永远不会收到这条命令；这不是“防泄漏”，
            // 而是 fatal 的完成丢失。保留 COMPLETED 只是避免 FC 侧命令槽永久 ACTIVE，
            // 上层必须把本轮 IO_ERROR 当系统故障处理。
            LOGE(LOG_TAG_RES "CQ full: completion lost for cmd=%hu\r\n", nand_completion->command_id);
            fc_pool_command_state_set(controller, nand_completion->command_id, FC_COMMAND_STATE_COMPLETED);
            return FC_STATUS_IO_ERROR;
        }
        return complete_status;
    }
}

/* ══════════════════════════════════════════════════════════
 * 8. Stripe RAID 辅助函数
 *
 * fc_stripe_release_parity()  — 回收 parity 命令的 desc+buffer+command 资源
 * fc_stripe_submit()         — 分配 parity 命令、配置、XOR、构建 MP request、submit
 *
 * 编译开关：CONFIG_FC_RAID (include/fc/fc.h)
 * ══════════════════════════════════════════════════════════ */

#if CONFIG_FC_RAID

/**
 * @brief 回收 parity 命令占用的所有资源（desc 链 + buffer + command 槽位）。
 *
 * @param controller FC 控制器
 * @param parity_id parity 命令 ID。
 *
 * @note 在 NAND completion 路径中调用（parity 数据已完成使命），
 * 把 desc/buffer 挂回空闲链，command 槽位置 FREE 回池。
 */
static void fc_stripe_release_parity(fc_ctl_s *controller, fc_cmd_id_t parity_id)
{
    // 层内 helper：唯一调用者是完成路径的 stripe 清理，parity_id 取自
    // stripe_parity_for_data[]（注册时即 fc_command_alloc() 铸出的池内 ID）。
    ASSERT(parity_id < FC_COMMAND_POOL_DEPTH);

    // 与 fc_pool_command_release 完全同义（desc 链 + buffer + 命令槽位），
    // 直接复用，不另写拷贝。parity 未单独提交，状态应为 ALLOCATED。
    fc_pool_command_release(controller, &controller->command_pool[parity_id]);
}

/**
 * @brief 处理 raid=ENCODE 命令的 Stripe 提交路径：
 * ① 分配 parity 命令（command + desc 链 + buffer）
 * ② 配置 parity 命令的 OP 码和 PAA（plane = PLANE_NUM-1）
 * ③ fc_data_path_generate_stripe_parity() — XOR + ECC 编码 parity
 * ④ 构建 Multi-Plane dispatch request（plane0=data, plane1=parity）
 * ⑤ fc_nand_dispatch_submit()
 *
 * 成功时 parity 命令由 completion 路径自动释放。
 * 失败时已分配资源全部回滚 + 数据命令以 error completion 返回。
 *
 * @param controller FC 控制器
 * @param data_cmd 触发 stripe 的数据命令
 * @param data_cmd_id 数据命令的池内 ID
 *
 * @return FC_STATUS_OK = parity 命令已提交
 *         FC_STATUS_IO_ERROR = 任一步失败，已回滚并给数据命令注 error completion
 *
 * @note 仅处理 PLANE_NUM=2 场景（单数据命令 + 单 parity 命令）。
 */
static fc_status_e fc_stripe_submit(fc_ctl_s *controller, fc_cmd_s *data_cmd, fc_cmd_id_t data_cmd_id)
{
    fc_cmd_s              *parity_cmd;
    fc_cmd_id_t            parity_id;
    fc_nand_dispatch_req_s mp_req;
    BYTE                   cw_count;
    BYTE                   cw;
    fc_desc_id_t           desc_id;
    const fc_cmd_s        *data_ptrs[FC_STRIPE_DATA_MAX];

    cw_count = (data_cmd->config.cw_count + 1U);

    // RAID/stripe 的 XOR 按 CW 配对：两个 plane 必须访问**同一组 CW**，
    // flat 掩码（plane 不对称）与 stripe 语义不兼容，直接断言拒绝。
    // 掩码由 **desc 链重建**（链是 CW 集合的唯一真值，不回读 FTL 的输入字段）。
    ASSERT(fc_cw_bitmap_is_plane_symmetric(fc_get_cmd_cw_bitmap(data_cmd)));

    // ① 分配 parity command
    parity_cmd = fc_command_alloc();
    if (parity_cmd == NULL)
    {
        goto stripe_error;
    }
    parity_id = fc_pool_command_id(controller, parity_cmd);

    // ② 配置 parity command
    parity_cmd->config           = data_cmd->config;
    parity_cmd->config.raid      = FC_RAID_MODE_NORMAL;         /* parity 命令不需要 RAID */
    parity_cmd->config.operation = OP_PROGRAM_PAGE_MULTI_PLANE; /* 提交时走 Multi-Plane */
    // parity 是 FC 内部命令（buffer 由 FC 申请、FC 释放），不继承 data_cmd 的 buf_own——
    // 否则 data_cmd 若是「上层申请」档，建链入口会因拿不到上层 buffer_id 而断言。
    parity_cmd->config.buf_own   = FC_BUF_OWN_FC_ALLOC_FC_FREE;
    parity_cmd->high_priority    = data_cmd->high_priority;
    parity_cmd->meta             = data_cmd->meta;

    // 调整 PAA：same page/block/lun/cw，plane = PLANE_NUM-1。
    // ch/ce 不在 PAA 里——parity 命令的路由取自它自己的 config（上面已从 data_cmd 复制）。
    parity_cmd->paa = paa_with_plane(data_cmd->paa, PLANE_NUM - 1U);

    // ③ 分配 parity desc + buffer
    // parity 命令是**单 plane**（plane = PLANE_NUM-1），CW 集合 = 连续 0..config.cw_count。
    // 「按掩码建链」在 FTL，FC 内没有掩码枚举；RAID 是 FC 内部机制，
    // 建链只有公共入口 fc_command_configure_descriptor，这里逐条调它挂链
    // （parity 恒为连续 0..config.cw_count，等价于 FTL 侧整段位图 fc_command_plane_span_cw_bitmap）。
    // ⚠ 该入口固定使用单例 g_fc_controller；RAID 暂未启用（CONFIG_FC_RAID=0），
    // 将来启用多实例前需先把这条路径改回 controller 参数化入口。
    // 行地址：同一 plane 内所有 CW 共用 parity_cmd->paa（page/block/lun）。
    {
        const BYTE  parity_plane = paa_to_plane(parity_cmd->paa);
        const DWORD parity_paa   = parity_cmd->paa;
        BYTE        pcw;

        for (pcw = 0U; pcw <= parity_cmd->config.cw_count; pcw++)
        {
            if (fc_command_configure_descriptor(parity_cmd, parity_plane, pcw, parity_paa) != FC_STATUS_OK)
            {
                // 回滚半成品：fc_pool_command_release 一次做完 desc 链 + buffer + 命令槽位。
                fc_pool_command_release(controller, parity_cmd);
                goto stripe_error;
            }
        }
    }

    // 写入 parity descriptors 的 meta（与数据命令对应 CW 一致）
    {
        fc_desc_id_t data_desc_id   = (fc_desc_id_t)data_cmd->desc_id;
        fc_desc_id_t parity_desc_id = (fc_desc_id_t)parity_cmd->desc_id;

        for (cw = 0U; cw < cw_count; cw++)
        {
            fc_desc_s *pdesc = fc_descriptor_id_to_descriptor(parity_desc_id);
            fc_desc_s *ddesc = fc_descriptor_id_to_descriptor(data_desc_id);
            if ((pdesc != NULL) && (ddesc != NULL))
            {
                pdesc->meta = ddesc->meta;
            }
            parity_desc_id = (pdesc != NULL) ? (fc_desc_id_t)pdesc->next_id : FC_DESC_ID_INVALID;
            data_desc_id   = (ddesc != NULL) ? (fc_desc_id_t)ddesc->next_id : FC_DESC_ID_INVALID;
        }
    }

    // ④ fc_data_path_generate_stripe_parity() — XOR + ECC 编码 parity
    data_ptrs[0U] = data_cmd;
    if (fc_data_path_generate_stripe_parity(data_ptrs, 1U, parity_cmd) != FC_STATUS_OK)
    {
        fc_stripe_release_parity(controller, parity_id);
        goto stripe_error;
    }

    // ⑤ 构建 Multi-Plane dispatch request
    memset(&mp_req, 0, sizeof(mp_req));
    mp_req.command_id  = data_cmd_id; /* 完成路由回数据命令 */
    mp_req.operation   = OP_PROGRAM_PAGE_MULTI_PLANE;
    mp_req.ch          = data_cmd->route.ch;
    mp_req.ce          = data_cmd->route.ce;
    mp_req.lun         = data_cmd->route.lun;
    mp_req.plane_count = PLANE_NUM;
    mp_req.plane       = 0U; /* multi-plane：起始 plane 恒 0（uop 硬编码 row[0]/row[1]） */
    mp_req.page_type   = data_cmd->config.page_type;
    // 两 plane 的行地址 + 掩码：stripe 命令 plane 对称（见上方 ASSERT），
    // 两 plane 访问同一组 CW → 掩码 = 两组相同的低位。
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        mp_req.plane_paa[p] = paa_with_plane(data_cmd->paa, p);
        for (cw = 0U; cw < cw_count; cw++)
        {
            mp_req.cw_bitmap |= (1UL << (p * CW_NUM + cw));
        }
    }

    // 填充 plane 0（= 数据命令）的 CW buffer
    desc_id = (fc_desc_id_t)data_cmd->desc_id;
    for (cw = 0U; cw < cw_count; cw++)
    {
        // desc_id 由链遍历产生，恒在池内——越界即链损坏 = 调用者 bug，转 ASSERT
        ASSERT(desc_id < FC_DESCRIPTOR_POOL_DEPTH);
        const fc_desc_s *desc = &controller->descriptor_pool[desc_id];

        mp_req.cw_buffers[cw] = (desc->buffer_id != FC_BUF_ID_INVALID) ? fc_buffer_get_base(desc->buffer_id) : NULL;
        desc_id               = (fc_desc_id_t)desc->next_id;
    }

    // 填充 plane 1（= parity 命令）的 CW buffer
    desc_id = (fc_desc_id_t)parity_cmd->desc_id;
    for (cw = 0U; cw < cw_count; cw++)
    {
        ASSERT(desc_id < FC_DESCRIPTOR_POOL_DEPTH);
        const fc_desc_s *desc = &controller->descriptor_pool[desc_id];

        mp_req.cw_buffers[CW_NUM + cw] = (desc->buffer_id != FC_BUF_ID_INVALID) ? fc_buffer_get_base(desc->buffer_id) : NULL;
        desc_id                        = (fc_desc_id_t)desc->next_id;
    }

    // 注册 parity 命令到数据命令的清理映射表
    controller->stripe_parity_for_data[data_cmd_id] = parity_id;

    // ⑥ fc_nand_dispatch_submit()
    if (fc_nand_dispatch_submit(&mp_req) != NAND_RESULT_OK)
    {
        controller->stripe_parity_for_data[data_cmd_id] = FC_CMD_ID_INVALID;
        fc_stripe_release_parity(controller, parity_id);
        goto stripe_error;
    }

    return FC_STATUS_OK;

stripe_error:
    // 数据命令注 error completion，让 FTL 感知失败
    {
        fc_nand_dispatch_comp_s fail_comp;

        memset(&fail_comp, 0, sizeof(fail_comp));
        fail_comp.command_id       = data_cmd_id;
        fail_comp.execution_status = NAND_RESULT_NOT_READY;
        fail_comp.cw_status[0]     = FC_CW_STATUS_FAIL;
        (void)fc_complete_from_nand(controller, &fail_comp);
    }
    return FC_STATUS_IO_ERROR;
}

#endif /* CONFIG_FC_RAID */

/* ══════════════════════════════════════════════════════════
 * 9. 调度 / 轮询
 *
 * 服务主循环 fc_service 的两条提交路径拆成独立 helper：
 *   fc_poll_dispatch_single — 单命令（count==1）
 *   fc_poll_dispatch_chain  — 链式（count>=2）
 * 两者共用 fc_build_dispatch_request 构建 request；
 * 失败兜底统一走 fc_poll_fail_command。
 *
 * 行为不对称（既有，勿统一）：
 *   - 链式路径构建失败时从循环体内直接返回，放弃整条链。
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 以 NAND_FAIL 合成完成项，把命令注入完成队列（poll 内失败兜底）。
 *
 * @param controller FC 控制器
 * @param command_id 失败命令。
 *
 * @note request 构建失败 / submit 失败共用；execution_status 用
 * NOT_READY 表示"未能提交"，cw_status[0] 置 FC_CW_STATUS_FAIL。
 */
static void fc_poll_fail_command(fc_ctl_s *controller, fc_cmd_id_t command_id)
{
    fc_nand_dispatch_comp_s fail_comp;

    memset(&fail_comp, 0, sizeof(fail_comp));
    fail_comp.command_id       = command_id;
    fail_comp.execution_status = NAND_RESULT_NOT_READY;
    fail_comp.failed_cw        = NAND_CW_INVALID;
    fail_comp.cw_status[0]     = FC_CW_STATUS_FAIL;
    (void)fc_complete_from_nand(controller, &fail_comp);
}

/**
 * @brief 沿 desc 链把 CW 落位到 request 的 cw_bitmap / cw_buffers[]（带数据命令专用）。
 *
 * @param controller FC 控制器
 * @param command 命令
 * @param request 输出（header 已填、全零）。
 *
 * @return FC_STATUS_OK / FC_STATUS_INVALID_ARGUMENT（multi-plane 窄位图，见下）。
 *
 * @note 每个 desc 落位到 **flat 绝对 CW 号 n = plane*CW_NUM + cw** 那一格：
 * 置 request->cw_bitmap 的 bit n、写 request->cw_buffers[n]。落位顺序无关紧要
 * （掩码是集合），故不再有"槽位计数器"，也不再有链序 == plane-major 的不变量。
 * - **链长度即 CW 数**（不由 config.cw_count 推导）：popcount(cw_bitmap)。
 * - 行地址不在这里逐 CW 存：它是 per-plane 的，由下面按 plane 集合填 plane_paa[]。
 * 每 plane 的行地址**取自链上该 plane 的第一条 desc**（链即真值，与 cw_bitmap 同源），
 * 而不是从 command->paa 复制——Policy B 下两个 plane 的 block/page 可以不同。
 * - **唯一的失败出口**：multi-plane 命令的 desc 链没覆盖 plane 段里的每一个 plane
 * （见分支内注释）。debug 档 ASSERT 停机，release 档返回 INVALID_ARGUMENT 拒绝。
 * - 只由 fc_build_dispatch_request 的"带数据命令"分支调用。
 */
static fc_status_e fc_build_request_data_planes(const fc_ctl_s *controller, const fc_cmd_s *command, fc_nand_dispatch_req_s *request)
{
    // 带数据命令必须有 desc 链；无链的 has_data 命令（READ_STATUS 等）由引擎内部
    // 直接发 70h/78h，不应走 FC 提交路径。
    if (command->desc_id == FC_DESC_ID_INVALID)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    fc_desc_id_t desc_id          = (fc_desc_id_t)command->desc_id;
    BYTE         planes_with_desc = 0U; /* bit p = plane p 链上有 desc */
    DWORD        plane_row[PLANE_NUM];  /* 各 plane 的行地址（链上第一条 desc） */
    WORD         steps = 0U;

    _Static_assert(PLANE_NUM <= 8U, "planes_with_desc is a BYTE bitmap");

    // 链遍历必须有界：成环时不能把 dispatch 线程挂死。
    while ((desc_id != FC_DESC_ID_INVALID) && (desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH))
    {
        const fc_desc_s *descriptor = &controller->descriptor_pool[desc_id];
        BYTE             plane      = paa_to_plane(descriptor->paa);
        BYTE             n          = (plane * CW_NUM + paa_to_cw(descriptor->paa));

        if ((plane >= PLANE_NUM) || (n >= CW_TOTAL_MAX))
        {
            return FC_STATUS_INVALID_ARGUMENT;
        }
        // 同一个 CW 被两条 desc 覆盖 = desc 链构造错误（掩码是集合，重复置位会丢数据）
        if ((request->cw_bitmap & (1UL << n)) != 0UL)
        {
            return FC_STATUS_INVALID_ARGUMENT;
        }

        // 带数据命令的每个 desc 都必须有 buffer；INVALID 是建链失败后的半成品，
        // 不能把 NULL 指针交给 NAND 层（release 会 memcpy(NULL)）。
        if (descriptor->buffer_id == FC_BUF_ID_INVALID)
        {
            return FC_STATUS_INVALID_ARGUMENT;
        }

        if ((planes_with_desc & (1U << plane)) == 0U)
        {
            // 该 plane 的行地址取链上第一条 desc（行地址只含 page/block/lun+plane，CW 位丢掉）：
            // paa_page_base 清 CW 位、paa_with_plane 换 plane —— 与逐字段重排逐位等价。
            plane_row[plane] = paa_with_plane(paa_page_base(descriptor->paa), plane);
            planes_with_desc |= (1U << plane);
        }
        else
        {
            // 同一 plane 的所有 desc 必须指向同一物理位置——一个 plane 一次只访问一个块/页
            ASSERT(paa_to_block(descriptor->paa) == paa_to_block(plane_row[plane]));
            ASSERT(paa_to_page(descriptor->paa) == paa_to_page(plane_row[plane]));
            ASSERT(paa_to_lun(descriptor->paa) == paa_to_lun(plane_row[plane]));
        }

        request->cw_bitmap |= (1UL << n);
        // Page read/write 需要 CW 基址（ECC 在完整 528B CW 上处理）；
        // 非 page 命令数据直接写入 USER 区域，test/FTL 通过
        // fc_buffer_get_user_data 直接读取。
        request->cw_buffers[n] = (fc_is_page_read(command->config.operation) == true) || (fc_is_page_write(command->config.operation) == true) ? fc_buffer_get_base(descriptor->buffer_id) : fc_buffer_get_user_data(descriptor->buffer_id);
        desc_id                = (fc_desc_id_t)descriptor->next_id;
        steps++;
    }
    if (desc_id != FC_DESC_ID_INVALID)
    {
        return FC_STATUS_IO_ERROR;
    }

    if (command->config.multi_plane > 0U)
    {
        const BYTE plane_count = command->config.multi_plane;

        if ((plane_count == 0U) || (plane_count > PLANE_NUM))
        {
            return FC_STATUS_INVALID_ARGUMENT;
        }
        const BYTE all_planes = ((1U << plane_count) - 1U);
        BYTE       plane;

        request->plane_count = plane_count;
        request->plane       = 0U; /* multi-plane：起始 plane 恒 0（uop 硬编码 row[0]/row[1]） */

        // 不变式：multi-plane 命令的 desc 链必须覆盖 plane 段 [0, plane_count) 里的
        // **每一个** plane。违反 = 调用者的编程错误，按 §十三 判据一律 ASSERT（不当返回码
        // 替代断言）；但 ASSERT 在 NDEBUG 下被裁空，故下面**再补一道 release 拒绝**。
        //
        // 为什么非炸不可：NAND 侧的多 plane handler **无条件遍历 PLANE_NUM** 下行地址，
        // 既不看掩码也不看 plane_count（fc_nand_dispatch.c 的 req_multi_plane_{program,
        // read,erase}），器件侧多 plane program 会把点名过的 plane **全部编程**。未覆盖的
        // plane 既无数据（cw_buffers[n] 恒 NULL）也无地址，只能凭空造一个——写路径于是
        // **静默**给该 plane 编一页全 0xFF（页缓冲 memset 0xFF、只填掩码内的 CW），
        // 调用者完全不知道。
        //
        // 这条不变式此前只在测试夹具里守着（fc_test_common.c 的 test_cmd_build），生产侧
        // 无守卫——ftl_cmd_resolve_cw_bitmap 只查**越段**，部分位图对整段而言是"合法"的。
        // 生产路径不可达：FTL 的多 plane 入口自己保证位图覆盖段内每一个 plane
        // （ftl_issue_subop / wb_handle(FLUSH) 由 cw_bitmap 构造保证）。踩进来 = 调用者直接调
        // ftl_cmd_configure_descriptors(cmd, 部分位图) 配 multi_plane > 0。
        // 想只访问某个 plane ⇒ 用**单 plane op**（multi_plane = 0），别用多 plane op 配窄位图。
        ASSERT((planes_with_desc & all_planes) == all_planes);
        // release 侧**同步拒绝**（ASSERT 在 NDEBUG 下整条裁空）：宁可不发这条命令，也
        // 不能让未覆盖 plane 的回退地址被凭空造出来、静默写进介质。走
        // fc_poll_fail_command 注一条 NAND_FAIL 完成项，FTL 的 pending 照常释放
        // （否则命令永不完成，表现为"挂死"，比报错更难查）。
        if ((planes_with_desc & all_planes) != all_planes)
        {
            return FC_STATUS_INVALID_ARGUMENT;
        }

        // 行地址按 plane 逐个填（行地址只含 page/block/lun + plane 位，CW 位被
        // paa_to_nand_row() 丢掉）：命令 plane 集合里每个 plane 都要填——空 plane 不填
        // 会退化成 block0/page0，静默误操作。上面那条 ASSERT 已保证「每个 plane 都有
        // desc」，故三元恒取 plane_row[plane]；else 支只在 NDEBUG 下可达，是**最后兜底**
        // （回退到 command->paa 的同页地址），生产不可达。
        for (plane = 0U; plane < plane_count; plane++)
        {
            request->plane_paa[plane] = ((planes_with_desc & (1U << plane)) != 0U) ? plane_row[plane] : paa_with_plane(command->paa, plane);
        }
    }
    else
    {
        // Single-plane：地址取自 command->paa（pba_to_cmd 单 plane 路径直接写它，
        // GC/WL 的单 plane 写也走这条），desc 链只负责 CW 落位。
        // 不变式：本命令所有 desc 的 plane 位相同、行地址与 command->paa 一致——下面这条
        // ASSERT 把它显式化（有 desc 时链上地址 == command->paa）。
        // 「无重复 n」（掩码位数 == 链长）由 fc_pool_chain_validate 在提交前扫一次；
        // 本函数另有 request->cw_bitmap 重复位检查（重复即 INVALID_ARGUMENT）。
        request->plane                     = paa_to_plane(command->paa);
        request->plane_paa[request->plane] = command->paa;
        ASSERT(((planes_with_desc & (1U << request->plane)) == 0U) || ((paa_to_block(plane_row[request->plane]) == paa_to_block(command->paa)) && (paa_to_page(plane_row[request->plane]) == paa_to_page(command->paa))));
    }

    return FC_STATUS_OK;
}

/**
 * @brief 下发前对 request->operation 做两处改写（读语义 + Phase V 拆分）。
 *
 * @param request 已构建完成的 dispatch 请求。
 *
 * @note 读改写先把 WITH_DATA_OUT 换成「加载型」op；写/擦再按 splittable 拆成
 * ISSUE（仅 LUN_NUM>1）。两处互不重叠，顺序不敏感，但读改写必须在这之后
 * 不被 splittable 判定命中——LOAD/WAIT 本身已是 ISSUE/WAIT 型。
 * - 由 fc_build_dispatch_request 末尾调用；单命令与链式路径共用。
 */
static void fc_dispatch_rewrite_operation(fc_nand_dispatch_req_s *request)
{
    /* ── 读命令统一为「加载 + 逐 CW 取数」──
     * FC 层命令保持 WITH_DATA_OUT 语义（desc 建链、fc_is_page_read、数据路径、
     * 完成项判定都用它），但下发给 NAND 的是对应的「只加载、不输出」op：
     * 整页先搬进 NAND 页缓冲，数据随后由 fc_nand_dispatch_task_cw_chain 用
     * 05h（单 plane）/ 06h（多 plane）按 mask 逐 CW 取回（见 fc_nand_dispatch.c）。
     *
     * LUN_NUM==1：原子 WITHOUT_DATA_OUT（发 30h 后等 tR，再逐 CW 取数）。
     * LUN_NUM>1 ：LOAD（ISSUE，发完 30h 立即返回）→ WAIT（等 tR + 状态）→
     *             逐 CW 取数；tR 期间别的 LUN 可以使用总线。 */
    if (request->operation == OP_NORMAL_READ_PAGE_WITH_DATA_OUT)
    {
        request->operation = (LUN_NUM > 1U) ? OP_READ_PAGE_LOAD : OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT;
    }
    else if (request->operation == OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT)
    {
        request->operation = (LUN_NUM > 1U) ? OP_READ_PAGE_MULTI_PLANE_LOAD : OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT;
    }

    // Phase V: splittable 原子命令自动拆为 ISSUE（仅 LUN_NUM>1 时生效；
    // 拆分后由 dispatch Phase 机异步接续 WAIT/FETCH）。
    if ((LUN_NUM > 1U) && nand_operation_is_splittable(request->operation))
    {
        request->operation = nand_operation_to_issue(request->operation);
    }
}

/**
 * @brief 由一条已 ACTIVE 的命令构建自包含的 NAND dispatch 请求。
 *
 * @param controller FC 控制器
 * @param command 命令
 * @param command_id 命令 ID
 * @param request 输出请求。
 *
 * @return FC_STATUS_OK / FC_STATUS_INVALID_ARGUMENT（multi-plane 窄位图 / buffer 缺失 /
 *         链重复或成环，由 fc_build_request_data_planes 透传）。
 *
 * @note 填 ch/ce/lun/page_type/feature_addr，再按操作类型填 cw_bitmap / cw_buffers[] / plane_paa[]：
 * * ERASE_BLOCK：只填 plane_paa[本 plane]，**不置掩码**（擦除无逐 CW 数据，
 * 请求位图是合法空集，completion 的空集分支会写 cw_status[0]），无 buffer；
 * * ERASE_BLOCK_MULTI_PLANE：填 plane_paa[0..1]，同样不置掩码；
 * * SET/GET_FEATURES：cw_buffers[plane*CW_NUM] 指向控制器侧暂存 g_fc_return_staging
 * （plane 恒 0，feature 寄存器是 die 级、无 plane 语义）；
 * * OP_READ_META：无 desc 链，plane_paa[本 plane] 取自 command->paa（保留 cw 位 →
 * NAND 侧列地址），cw_buffers[plane*CW_NUM] 指向同一暂存（回读的 4 字节 META）；
 * * OP_READ_ONFI_SIGNATURE：无 desc 链、**不填 plane_paa**——列地址恒 0x20、
 * 行地址恒 0，与介质坐标无关；cw_buffers[plane*CW_NUM] 指向同一暂存（4 字节签名）；
 * * OP_READ_COLUMN：无 desc 链，plane_paa[本 plane] 取自 command->paa（只提供
 * 行地址，cw 位被丢掉）；**列地址来自命令级 param 字节 0/1（小端）**，落进
 * request->column；cw_buffers[plane*CW_NUM] 指向同一暂存（4 字节）；
 * * 块锁族（BLOCK_UNLOCK_LOW/HIGH、BLOCK_LOCK(_TIGHT)、BLOCK_LOCK_STATUS）：
 * 无 desc 链、无 buffer，只填 plane_paa[本 plane]（23h/24h/7Ah 带 3 个地址周期，
 * 行地址必须来自 command->paa；2Ah/2Ch 无地址周期，填了不下发）；
 * * 其余带数据命令：沿 desc 链落位（见 fc_build_request_data_planes）。
 * - **索引空间唯一**：掩码 bit n = plane*CW_NUM + cw（flat 绝对编号），cw_buffers[] 同下标；
 * NAND 层不再有槽位/链序等第二套坐标。未写的格由上面的 {0} 初始化保证为确定性零值。
 * - **plane_paa[] 是 per-plane 的行地址**，与掩码无关：命令 plane 集合内每个 plane 都填
 * （哪怕它没有选中任何 CW）。行地址始终由 paa_to_nand_row() 从 PAA 值算出。
 * - page read/write 用 CW 基址（ECC 处理完整 528B），非 page 用 USER 区。
 * - 末尾调 fc_dispatch_rewrite_operation()：读语义改写 + Phase V splittable → ISSUE。
 *
 * @note 注意：本函数不做 META 预填——buffer[0..3] 的 META 由
 * fc_data_path_encode 的 Stage 0 写（两条派发路径都经 encode）。
 */
static fc_status_e fc_build_dispatch_request(fc_ctl_s *controller, fc_cmd_s *command, fc_cmd_id_t command_id, fc_nand_dispatch_req_s *request)
{
    // Pre-resolve descriptor-chain data into a self-contained dispatch request.
    memset(request, 0, sizeof(*request));
    request->command_id   = command_id;
    request->operation    = command->config.operation;
    request->ch           = command->route.ch;
    request->ce           = command->route.ce;
    request->lun          = command->route.lun;
    request->page_type    = command->config.page_type;
    request->feature_addr = command->feature_addr;

    if (command->config.operation == OP_ERASE_BLOCK)
    {
        // 单 plane 擦除：目标 plane 取自 command->paa（本分支无 desc 可用）。
        // **不置 cw_bitmap**——擦除无逐 CW 数据，请求位图是合法空集；
        // completion_all_cws 对空集写 cw_status[0]，CQ 聚合照样读得到擦除成败。
        request->plane                     = paa_to_plane(command->paa);
        request->plane_paa[request->plane] = command->paa;
    }
    else if (command->config.operation == OP_ERASE_BLOCK_MULTI_PLANE)
    {
        BYTE plane;

        request->plane_count = PLANE_NUM;
        request->plane       = 0U; /* multi-plane：起始 plane 恒 0（uop 硬编码 row[0]/row[1]） */
        for (plane = 0U; plane < PLANE_NUM; plane++)
        {
            request->plane_paa[plane] = paa_with_plane(command->paa, plane);
        }
        // 不置 cw_bitmap（同单 plane 擦除）——多 plane 擦除的 plane 集合由
        // plane_count/plane_paa[] 表达，与 CW 位图无关。
    }
    else if ((command->config.operation == OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT) || (command->config.operation == OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT))
    {
        // 无 desc 的「纯加载」op：只把目标页搬进器件页缓冲，**不建 CW 位图、不占 buffer**
        // —— op 表里 has_data = 0（host 侧无数据搬运），故 ftl_cmd_needs_desc_chain 不要求链。
        // 数据随后由**独立的** op 8（05h-E0h）/ op 41（06h-E0h）命令带 desc 链取回。
        // ⚠ 生产读路径**不走这里**：FC 命令恒为 op 12/30（has_data=1，走下面的 data_planes 分支
        // 建好链与位图），op 33/29 只作为 fc_dispatch_rewrite_operation 的**改写目标**出现。
        // 本分支只服务「命令本身就是 33/29」的直接下发（测试用）。
        // 行地址取自 command->paa（无 desc 可用）：单 plane 取本 plane；多 plane 逐 plane 补 plane 位。
        if (command->config.operation == OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT)
        {
            BYTE plane;

            request->plane_count = PLANE_NUM;
            request->plane       = 0U; /* multi-plane：起始 plane 恒 0（uop 硬编码 row[0]/row[1]） */
            for (plane = 0U; plane < PLANE_NUM; plane++)
            {
                request->plane_paa[plane] = paa_with_plane(command->paa, plane);
            }
        }
        else
        {
            request->plane                     = paa_to_plane(command->paa);
            request->plane_paa[request->plane] = command->paa;
        }
        // 不置 cw_bitmap（空集 = 无 CW 可访问）：加载 op 的 uop 无 DATA_OUT，位图/缓冲皆不用。
    }
    else if ((command->config.operation == OP_SET_FEATURES) || (command->config.operation == OP_GET_FEATURES))
    {
        // Feature 寄存器是 die 级、无 plane 语义，command->paa 从不设置（fc_command_alloc
        // 做 (fc_cmd_s){0}），故 plane 恒 0。仍按 plane 落位，以保住"数据恒在
        // cw_buffers[plane*CW_NUM]"这条无例外的规则。
        // 内联数据落**控制器侧暂存**——SQ 命令体提交后不可写；
        // SET 先把命令里的字节拷进暂存，GET 由引擎写回，完成时再拷进 CQ 的 param。
        // 掩码只置该 plane 的 CW0 位：Feature 恒为 4 字节单次传输，与 config.cw_count 无关
        // （只用槽位 0，其余槽位不承载数据）。
        request->plane                               = paa_to_plane(command->paa);
        request->cw_bitmap                           = (1UL << (request->plane * CW_NUM));
        request->cw_buffers[request->plane * CW_NUM] = g_fc_return_staging[command_id];
        if (command->config.operation == OP_SET_FEATURES)
        {
            (void)memcpy(g_fc_return_staging[command_id], command->param.bytes, 4U);
        }
        else
        {
            (void)memset(g_fc_return_staging[command_id], 0, 4U);
        }
    }
    else if (command->config.operation == OP_READ_META)
    {
        // META 只读回读：无 desc 链（has_data = 0），回读指定页、指定 CW 的前 4 字节
        // META（CW 布局 [META:4][CRC:4][USER:512][PARITY:8]，META 在加扰区前、明文）。
        // CW 由 command->paa 的 cw 字段选（PAA 原生含 cw），NAND 侧据
        // paa_to_cw(plane_paa[plane]) * CW_SIZE 算列地址；行地址取自同一 PAA。
        // 4 字节落**控制器侧暂存**（同 GET_FEATURES，SQ 提交后不可写），完成时拷进 CQ 的 param。
        // 掩码只置该 plane 的 CW0 位（暂存落点恒在 cw_buffers[plane*CW_NUM]）。
        request->plane                               = paa_to_plane(command->paa);
        request->plane_paa[request->plane]           = command->paa;
        request->cw_bitmap                           = (1UL << (request->plane * CW_NUM));
        request->cw_buffers[request->plane * CW_NUM] = g_fc_return_staging[command_id];
        (void)memset(g_fc_return_staging[command_id], 0, 4U);
    }
    else if (command->config.operation == OP_READ_ONFI_SIGNATURE)
    {
        // ONFI 签名读（90h + 地址 20h → "ONFI"）：器件级命令，无 desc 链（has_data = 0）。
        // 与 OP_READ_META 同型，但**不填 plane_paa**——列地址恒 0x20、行地址恒 0（uop 序列
        // 无 ROW 段），与介质坐标无关，故 command->paa 只用来定 plane（掩码位 = plane*CW_NUM）。
        // 4 字节落**控制器侧暂存**（同 GET_FEATURES / READ_META），完成时拷进 CQ 的 param。
        // ⚠ 必须建这一支：否则落进末尾 else，位图保持空集 ⇒ fc_nand_dispatch_task_load 一格
        // cw_buffers[] 都不搬 ⇒ NAND 侧拿到残留指针，DATA_OUT 写向野地址。
        request->plane                               = paa_to_plane(command->paa);
        request->cw_bitmap                           = (1UL << (request->plane * CW_NUM));
        request->cw_buffers[request->plane * CW_NUM] = g_fc_return_staging[command_id];
        (void)memset(g_fc_return_staging[command_id], 0, 4U);
    }
    else if (command->config.operation == OP_READ_COLUMN)
    {
        // 按显式列读：无 desc 链（has_data = 0）。与 OP_READ_META 同型，唯一区别是
        // **列地址不走 paa 的 cw 字段，而由命令级 param 的字节 0/1（小端）传入**——cw 只有
        // 2 位（列上限 1584），够不到 spare 区（出厂坏块标记在列 2048）。行地址仍取自
        // command->paa。4 字节落**控制器侧暂存**（同 GET_FEATURES / READ_META），完成时拷进
        // CQ 的 param。
        // ⚠ 必须建这一支：否则落进末尾 else，位图保持空集 ⇒ fc_nand_dispatch_task_load 一格
        // cw_buffers[] 都不搬 ⇒ NAND 侧拿到残留指针，DATA_OUT 写向野地址。
        request->plane                               = paa_to_plane(command->paa);
        request->plane_paa[request->plane]           = command->paa;
        request->cw_bitmap                           = (1UL << (request->plane * CW_NUM));
        request->cw_buffers[request->plane * CW_NUM] = g_fc_return_staging[command_id];
        request->column                              = (command->param.bytes[0] | (command->param.bytes[1] << 8U));
        (void)memset(g_fc_return_staging[command_id], 0, 4U);
    }
    else if ((command->config.operation == OP_BLOCK_UNLOCK_LOW) || (command->config.operation == OP_BLOCK_UNLOCK_HIGH) || (command->config.operation == OP_BLOCK_LOCK) || (command->config.operation == OP_BLOCK_LOCK_TIGHT) || (command->config.operation == OP_BLOCK_LOCK_STATUS))
    {
        // 块锁族：**无 host 数据搬运**（op 表 has_data 全为 0），但 23h/24h/7Ah
        // 都带 3 个地址周期（R1/R2/R3）⇒ 行地址**必须取自 command->paa**，否则退化为
        // block 0（本分支之前这些命令落到末尾 else，plane_paa[] 保持 {0} 整体清零值，
        // 而 ROW_0 的 uop 正是 *address = request->row[operand]，见 nand_fmc.c）。
        // 2Ah/2Ch（BLOCK_LOCK / BLOCK_LOCK_TIGHT）无地址周期，纳入本分支只为把「块锁族」
        // 记在一处——对它们只是多填一个不会被下发的行地址。
        // 不置 cw_bitmap：7Ah 的状态字节走 DQ 总线直读（NAND_UOP_DATA_OUT_STATUS →
        // completion->nand_status，不写 request->buffer），不经任何数据缓冲。
        request->plane                     = paa_to_plane(command->paa);
        request->plane_paa[request->plane] = command->paa;
    }
    else if (nand_operation_has_data(command->config.operation))
    {
        const fc_status_e build_result = fc_build_request_data_planes(controller, command, request);
        if (build_result != FC_STATUS_OK)
        {
            // 唯一失败出口（multi-plane 窄位图）。request 只被写了一半，但调用者拿到
            // 非 OK 就不会提交它——宁可不发，也不能让凭空造的回退地址写进介质。
            return build_result;
        }
    }
    else
    {
        // 其余命令（RESET / BLOCK_LOCK / BLOCK_UNLOCK / OTP_DATA_LOCK 等 has_data == 0
        // 且无 desc 链的）：位图保持 0 = **合法空集**（无 CW 可访问）。擦除也走这条口径
        // （上面两个 ERASE 分支只填 plane_paa[]，CW 位保持空集）。
        // ⚠ 别把 READ_STATUS / READ_STATUS_ENHANCED 列进来——两者的 has_data 都是 1
        // （onfi_1_0.c:820/821），实际走上面的 fc_build_request_data_planes 分支；
        // 它们只是从不由 FC 提交（引擎 WAIT_READY 内部直接用 70h/78h uop）。
    }

    // 读语义改写 + Phase V 拆分（顺序敏感，见 helper 头注释）
    fc_dispatch_rewrite_operation(request);

    return FC_STATUS_OK;
}

/**
 * @brief 提交调度器选出的单条命令（Data Path encode → submit）。
 *
 * @param controller FC 控制器
 * @param chain 调度器链（count==1）。
 *
 * @return FC_STATUS_OK / IO_ERROR / fc_stripe_submit 结果。
 *
 * @note page write 先 fc_data_path_encode（内含 Stage 0 写 META）。
 * - raid=ENCODE 的 page write 走 fc_stripe_submit（Multi-Plane）。
 * - 其余构建 request → fc_nand_dispatch_submit；**encode 失败、构建失败与 submit 失败同路兜底**
 * （注 NAND_FAIL 完成并返回 IO_ERROR）。构建失败包括 multi-plane 窄位图、buffer 缺失
 * 和链损坏；debug 档会在对应 ASSERT 处停机，release 档由这里了结命令。
 * - 只负责占用目标 LUN；完成由后续 fc_service() 推进。
 * - 单命令路径；FTL 的 cache run 走 fc_poll_dispatch_chain。
 */
static fc_status_e fc_poll_dispatch_single(fc_ctl_s *controller, const fc_sched_chain_s *chain)
{
    fc_cmd_id_t            command_id = chain->ids[0U];
    fc_cmd_s              *command;
    fc_nand_dispatch_req_s request;

    // 命令刚从 SQ/HSQ 弹出：id 恒为池内铸出值，状态恒 QUEUED（fc_sched_submit 置位）。
    // 出队到置 ACTIVE 之间不得 return 失败：命令已出队且未回推会永久泄漏。
    ASSERT(command_id < FC_COMMAND_POOL_DEPTH);
    ASSERT(controller->command_state[command_id] == FC_COMMAND_STATE_QUEUED);
    fc_pool_command_state_set(controller, command_id, FC_COMMAND_STATE_ACTIVE);
    command = &controller->command_pool[command_id];

    /* ── Data Path encode: Random 加扰 → ECC 编码（RAID XOR 已移至 stripe 路径）──
     * 在 NAND 编程前对 buffer 数据做编码，对 FTL 完全透明。 */
    if (fc_is_page_write(command->config.operation) == true)
    {
        const fc_status_e encode_result = fc_data_path_encode(command);

        if (encode_result != FC_STATUS_OK)
        {
            // 编码失败（ECC 引擎不可用 / buffer 缺失）绝不能把裸数据发给 NAND。
            LOGW("fc: data path encode failed for op=%hhu cmd_id=%hu status=%d\r\n", command->config.operation, command_id, encode_result);
            fc_poll_fail_command(controller, command_id);
            return FC_STATUS_IO_ERROR;
        }
    }

    /* ── Stripe RAID 分支：raid=ENCODE 的 page write 走 Multi-Plane 提交 ── */
#if CONFIG_FC_RAID
    if ((fc_is_page_write(command->config.operation) == true) && (command->config.raid == FC_RAID_MODE_ENCODE))
    {
        fc_status_e stripe_result = fc_stripe_submit(controller, command, command_id);
        return stripe_result; /* 成功/失败均已在 fc_stripe_submit 中处理 */
    }
#endif /* CONFIG_FC_RAID */

    const fc_status_e build_result = fc_build_dispatch_request(controller, command, command_id, &request);
    if (build_result != FC_STATUS_OK)
    {
        // 构建失败（multi-plane 窄位图）：debug 档上一条 ASSERT 已停机，这里只在 release
        // 档可达——命令注 NAND_FAIL 完成项后了结，**绝不**把凭空造的回退地址提交给 NAND。
        LOGW("fc: build request failed for op=%hhu cmd_id=%hu status=%d\r\n", command->config.operation, command_id, build_result);
        fc_poll_fail_command(controller, command_id);
        return FC_STATUS_IO_ERROR;
    }

    const BYTE submit_result = fc_nand_dispatch_submit(&request);

    // 填充轮次（fc_poll_fill）已按 fc_nand_dispatch_lun_free() 预留槽位，
    // 此处不应再遇到 NOT_READY；其余失败（设备未就绪等）才是真错误。
    ASSERT(submit_result != NAND_RESULT_NOT_READY);
    if (submit_result != NAND_RESULT_OK)
    {
        LOGW("fc: fc_nand_dispatch_submit failed for op=%hhu cmd_id=%hu cw_bitmap=0x%02lX plane_count=%hhu\r\n", command->config.operation, command_id, request.cw_bitmap, request.plane_count);
        fc_poll_fail_command(controller, command_id);
        return FC_STATUS_IO_ERROR;
    }

    // Submission only occupies the target LUN. Completion is advanced by a later fc_service() call.
    return FC_STATUS_OK;
}

/**
 * @brief 提交调度器选出的链式命令（逐条 encode + 构建 → submit_chain）。
 *
 * @param controller FC 控制器
 * @param chain 调度器链（count>=2）。
 *
 * @return FC_STATUS_OK / IO_ERROR。
 *
 * @note 整链先置 ACTIVE，再逐条 page write encode（不做 META/PAA 预填）并构建 requests[i]。
 * - 全部构建完成后一次 fc_nand_dispatch_submit_chain。
 * - 任一步失败 → 对应/整链命令注 NAND_FAIL 完成并返回 IO_ERROR。
 * - 使用者：FTL 的 cache read / cache program run（同一 (ch,ce,lun) 的连续命令）。
 */
static fc_status_e fc_poll_dispatch_chain(fc_ctl_s *controller, const fc_sched_chain_s *chain)
{
    BYTE                   chain_count = chain->count;
    fc_nand_dispatch_req_s reqs[FC_SCHEDULER_CHAIN_MAX];
    BYTE                   i;
    BYTE                   j;

    // 整链先全部进入 ACTIVE：后续任一步失败时，整链都能用同一个完成兜底了结，
    // 不会出现「前缀 ACTIVE、后缀 QUEUED」的半状态。
    for (i = 0U; i < chain_count; i++)
    {
        const fc_cmd_id_t command_id = chain->ids[i];

        ASSERT(command_id < FC_COMMAND_POOL_DEPTH);
        ASSERT(controller->command_state[command_id] == FC_COMMAND_STATE_QUEUED);
        fc_pool_command_state_set(controller, command_id, FC_COMMAND_STATE_ACTIVE);
    }

    for (i = 0U; i < chain_count; i++)
    {
        const fc_cmd_id_t command_id = chain->ids[i];
        fc_cmd_s         *command    = &controller->command_pool[command_id];

        /* ── Data Path encode: Random 加扰 → ECC 编码 ──
         * 在 NAND 编程前对 buffer 数据做编码，对 FTL 完全透明。 */
        if (fc_is_page_write(command->config.operation) == true)
        {
            const fc_status_e encode_result = fc_data_path_encode(command);

            if (encode_result != FC_STATUS_OK)
            {
                LOGW("fc: data path encode failed for cmd_id=%hu (chain %hhu/%hhu) status=%d\r\n", command_id, i, chain_count, encode_result);
                for (j = 0U; j < chain_count; j++)
                {
                    fc_poll_fail_command(controller, chain->ids[j]);
                }
                return FC_STATUS_IO_ERROR;
            }
        }

        const fc_status_e build_result = fc_build_dispatch_request(controller, command, command_id, &reqs[i]);
        if (build_result != FC_STATUS_OK)
        {
            // 构建失败（multi-plane 窄位图 / buffer 缺失 / 链损坏）→ 整链注 NAND_FAIL。
            LOGW("fc: build request failed for cmd_id=%hu (chain %hhu/%hhu) status=%d\r\n", command_id, i, chain_count, build_result);
            for (j = 0U; j < chain_count; j++)
            {
                fc_poll_fail_command(controller, chain->ids[j]);
            }
            return FC_STATUS_IO_ERROR;
        }
    }

    const BYTE submit_result = fc_nand_dispatch_submit_chain(reqs, chain_count);

    // 填充轮次已按 fc_nand_dispatch_lun_free() 预留整链槽位，此处不应再遇到 NOT_READY。
    ASSERT(submit_result != NAND_RESULT_NOT_READY);
    if (submit_result != NAND_RESULT_OK)
    {
        for (i = 0U; i < chain_count; i++)
        {
            fc_poll_fail_command(controller, chain->ids[i]);
        }
        return FC_STATUS_IO_ERROR;
    }

    // Submission only occupies the target LUN. Completion is advanced by a later fc_service() call.
    return FC_STATUS_OK;
}

/**
 * @brief 计算队头命令所属链的长度（链头已弹出，peek 从队头看后续）。
 *
 * @param queue 提交队列，HSQ/SQ 通用（队头已是链头之后的下一条）
 * @param head 已弹出的链头命令。
 *
 * @return 链长（1..FC_SCHEDULER_CHAIN_MAX）。
 *
 * @note head->config.chain == FC_CHAIN_END → 单命令，返回 1。
 * - 否则 peek 后续，收集同一 (ch,ce,lun) 的连续命令，直到 chain==END 或链满。
 * - 只 peek 不消耗——真正弹出在 fc_poll_fill_queue 里，链放不下时可原样留在队列。
 */
static BYTE fc_poll_chain_length(const fc_sub_queue_s *queue, const fc_cmd_s *head)
{
    BYTE need = 1U;

    ASSERT(queue != NULL);
    ASSERT(head != NULL);

    if (head->config.chain == FC_CHAIN_END)
    {
        return 1U;
    }

    while (need < FC_SCHEDULER_CHAIN_MAX)
    {
        fc_cmd_id_t     next_id = fc_submission_queue_peek(queue, (need - 1U));
        const fc_cmd_s *next;

        if (next_id == FC_CMD_ID_INVALID)
        {
            break; /* 队列尾 */
        }
        next = fc_command_id_to_command(next_id);

        // 链成员必须同一 target（FTL 保证）。debug 下停机；release 下**不要把
        // 不同 LUN 的成员算进本链**——否则 fill 会按链头 LUN 选队列，把错目标
        // 的命令塞进同一条 per-LUN FIFO。截断在链头/已确认前缀处，剩余命令
        // 留在队列，由下一轮独立处理。
        const BOOL same_target = (next->route.ch == head->route.ch) && (next->route.ce == head->route.ce) && (next->route.lun == head->route.lun);

        ASSERT_MSG(same_target, "chain target mismatch: head ch/ce/lun=%hhu/%hhu/%hhu next=%hhu/%hhu/%hhu", head->route.ch, head->route.ce, head->route.lun, next->route.ch, next->route.ce, next->route.lun);
        if (same_target == false)
        {
            break;
        }
        need++;
        if (next->config.chain == FC_CHAIN_END)
        {
            break; /* chain=0：链结束 */
        }
    }
    return need;
}

/**
 * @brief 扫描单个提交队列一趟，把能放下的命令（含链）提交进 NAND per-LUN FIFO。
 *
 * @param controller FC 控制器
 * @param queue 待扫描队列（HSQ 或 SQ）
 * @param owner 本队列的链归属标签（FC_CHAIN_OWNER_SQ / _HSQ）；链锁准入用：某 LUN 上有未结束的链时，只放行链所在队列
 * @param suspended 本轮已挂起 LUN 表（HSQ/SQ 共享，见 fc_poll_fill）
 * @param dispatched 累计已提交条数（进出）。
 *
 * @return FC_STATUS_OK / IO_ERROR（提交失败，剩余命令留在队列）。
 *
 * @note **按 LUN 保序**：命令 need = 链长；fc_nand_dispatch_lun_free(lun) >= need 才取，
 * 否则该 LUN 本轮挂起（suspended），后续同 LUN 命令一律跳过。
 * - **链锁准入**：该 LUN 上有未结束的链（chain_owner != NONE）且链不归本队列 →
 * 本命令回推跳过。**只回推、不置 suspended**——置了会把链自己的续批一起锁死。
 * - **链锁更新**：一批下发后看批尾的 chain 位——MORE 继续占着该 LUN（转本队列），
 * END 解锁；下发失败（整链注 NAND_FAIL）也清锁，不留残锁。
 * - **一次扫描**：按进入时的队列快照数量扫描，扫完即止。
 * - **顺序保持**：取不走的命令 pop 后回推队尾——扫完整轮等价于一次旋转，
 * 未取出命令的相对顺序不变（同 LUN 的命令因 LUN 挂起必然一起回推，顺序不破）。
 * 链成员连续且逐个回推，链头永远排在成员之前，链序不破。
 * - 提交失败即中止本轮，剩余命令仍在队列里（未被取出），下轮重试。
 */
/* 填充拒绝下发的原因（限流 8 条）：`S`=本趟已挂起该 LUN、`L`=链锁归另一队列、
 * `R`=FIFO 剩余槽放不下整条链。只用于定位「命令在 SQ 却一直不发」。 */
/**
 * @brief 单队列填充轮次：按 LUN 保序、链锁准入、room 判定
 *
 * 取不走的命令 pop 后回推队尾（等价一次旋转）；提交失败即中止本轮。
 *
 * @param controller FC 控制器
 * @param queue      目标队列
 * @param owner      队列所有者（SQ/HSQ）
 * @param suspended  本轮已挂起 LUN 表
 * @param dispatched 累计已提交条数（进出）
 * @return FC_STATUS_OK；FC_STATUS_IO_ERROR 提交失败
 */
static fc_status_e fc_poll_fill_queue(fc_ctl_s *controller, fc_sub_queue_s *queue, fc_chain_owner_e owner, BOOL suspended[CH_NUM][CE_NUM][LUN_NUM], BYTE *dispatched)
{
    WORD total;
    WORD k;

    ASSERT(queue != NULL);
    ASSERT(dispatched != NULL);

    total = queue->count;

    for (k = 0U; k < total; k++)
    {
        fc_cmd_id_t      id;
        fc_cmd_s        *cmd;
        fc_sched_chain_s chain;
        fc_status_e      dispatch_status;
        BYTE             ch;
        BYTE             ce;
        BYTE             lun;
        BYTE             need;
        BYTE             j;

        (void)fc_submission_queue_pop(queue, &id);
        s_fill_pops++;
        cmd = fc_command_id_to_command(id);

        ch  = cmd->route.ch;
        ce  = cmd->route.ce;
        lun = cmd->route.lun;
        // 路由字段越界是编程错误：值由 FTL 从 PBA 经 pba_to_ch/ce/lun 灌入，
        // PBA 已过 pba_is_valid() 校验。此处必须断言而非防御——suspended[] 按
        // [ch][ce][lun] 直接索引，放行即栈越界写；且越界命令会因
        // fc_nand_dispatch_lun_free()==0 被永久回推（静默活锁），比当场停机更难定位。
        ASSERT((ch < CH_NUM) && (ce < CE_NUM) && (lun < LUN_NUM));
        if (suspended[ch][ce][lun] == true)
        {
            (void)fc_submission_queue_push(queue, id);
            continue;
        }
        // 链锁准入：该 LUN 的填充权归另一条队列的未结束链 → 本命令回推跳过。
        // 为什么「只让拥有者队列喂」就够：链成员在队列里连续（见 fc_poll_fill_queue 的
        // 前置契约），续批恒是该 LUN 在队列里最老的那几条，按序扫描必然先取到它们；
        // 后入队的同 LUN 命令都排在链后面，只会在链结束（chain_owner 清）之后才被取。
        // ⚠ **只回推、不置 suspended**——置了会把链自己的续批也一并锁死（该 LUN 从此
        // 既不接受另一队列、也不接受本链，活锁）。
        // chain_owner 为 NONE（无未结束的链）时恒假；FTL cache run 打开后只放行链所在队列。
        if ((controller->scheduler.chain_owner[ch][ce][lun] != FC_CHAIN_OWNER_NONE) && (controller->scheduler.chain_owner[ch][ce][lun] != owner))
        {
            (void)fc_submission_queue_push(queue, id);
            continue;
        }

        need = fc_poll_chain_length(queue, cmd);
        if (fc_nand_dispatch_lun_free(ch, ce, lun) < need)
        {
            // 剩余槽放不下整条链：本轮挂起该 LUN，链头回推（成员后续轮次一并回推）
            suspended[ch][ce][lun] = true;
            (void)fc_submission_queue_push(queue, id);
            continue;
        }

        chain.count   = need;
        chain.ids[0U] = id;
        for (j = 1U; j < need; j++)
        {
            (void)fc_submission_queue_pop(queue, &chain.ids[j]);
        }
        k = (k + need - 1U); /* 跳过已取出的链成员 */

        // 单命令走 single，链走 chain——按 need 分流（不写成条件里的三目，
        // 两个分支都是带副作用的提交，塞进 if 里读不出"做了什么事"）
        if (need == 1U)
        {
            dispatch_status = fc_poll_dispatch_single(controller, &chain);
        }
        else
        {
            dispatch_status = fc_poll_dispatch_chain(controller, &chain);
        }

        if (dispatch_status != FC_STATUS_OK)
        {
            // 下发失败 = fc_poll_dispatch_chain 已把整链注 NAND_FAIL（构建失败或 submit
            // 失败都如此），这条链已经死了、不会再续批 ⇒ 必须清锁。不清的话该 LUN 会
            // 一直只认本队列，而那条链又不存在了，等于把 LUN 永久锁死。
            controller->scheduler.chain_owner[ch][ce][lun] = FC_CHAIN_OWNER_NONE;
            return FC_STATUS_IO_ERROR;
        }
        // 链锁更新：看**批尾**（chain.ids[need-1]，本批最后一条）的 chain 位——
        // MORE ⇒ 这条链还没完，该 LUN 的填充权继续归本队列（下一批仍从这条队列取）；
        // END  ⇒ 链在此收尾，解锁，另一条队列可以喂这个 LUN 了。
        // 单命令（need == 1）批尾恒 END ⇒ 恒写 NONE，即生产路径只是多一次无副作用的写。
        controller->scheduler.chain_owner[ch][ce][lun] = (controller->command_pool[chain.ids[need - 1U]].config.chain == FC_CHAIN_MORE) ? owner : FC_CHAIN_OWNER_NONE;
        (*dispatched)++;
    }

    return FC_STATUS_OK;
}

/**
 * @brief 一次填充轮次——扫描 HSQ/SQ，把可下发的命令（含链）提交进 NAND per-LUN FIFO。
 *
 * @param controller FC 控制器。
 *
 * @return FC_STATUS_OK（本轮至少提交了一条）
 *         IO_ERROR（提交失败，剩余命令留在队列）/ QUEUE_EMPTY
 *         NOT_READY（无可提交，供 ftl_service 结束本轮 fc_service 循环）。
 *
 * @note **HSQ 优先**：先扫 HSQ，再扫 SQ；两个队列走同一套扫描逻辑（含链）。
 * - **共享 suspended**：HSQ 因 room 不足挂起某 LUN 后，SQ 本轮也跳过该 LUN——
 * 否则同 LUN 的 SQ 命令会插到 HSQ 命令之前下发，优先级倒挂。
 * - **链锁独占（跨轮次持久，与 suspended 正交）**：链一旦开始，该 LUN 的填充权
 * 归链所在队列独占，直到链的 END 下发——另一条队列的命令一律回推。
 * 与上一条的区别：suspended 管"本趟 room 不足"、每趟清零；chain_owner 管"链没完"、
 * 跨趟保留。两者互不影响（链锁准入只回推，不置 suspended）。
 * - 扫描细节见 fc_poll_fill_queue()。
 */
static fc_status_e fc_poll_fill(fc_ctl_s *controller)
{
    fc_sched_s *scheduler = &controller->scheduler;
    BOOL        suspended[CH_NUM][CE_NUM][LUN_NUM];
    BYTE        dispatched = 0U;

    memset(suspended, 0, sizeof(suspended));
    s_fill_calls++;

    /* ── HSQ 优先：先扫 HSQ，再扫 SQ；同一套扫描逻辑，共享同一张 suspended 表 ── */
    if (fc_poll_fill_queue(controller, scheduler->hsq, FC_CHAIN_OWNER_HSQ, suspended, &dispatched) != FC_STATUS_OK)
    {
        return FC_STATUS_IO_ERROR;
    }
    if (fc_poll_fill_queue(controller, scheduler->sq, FC_CHAIN_OWNER_SQ, suspended, &dispatched) != FC_STATUS_OK)
    {
        return FC_STATUS_IO_ERROR;
    }

    if (dispatched != 0U)
    {
        return FC_STATUS_OK;
    }
    return ((scheduler->sq->count == 0U) && (scheduler->hsq->count == 0U)) ? FC_STATUS_QUEUE_EMPTY : FC_STATUS_NOT_READY;
}

/* ── NAND 完成回调上下文 + sink ──
 * fc_nand_dispatch_poll 每产出一条完成就调一次 fc_nand_completion_sink；每条当场消费
 * （写 desc/ECC、跑数据路径、组 CQ 项、清 stripe parity），不跨条缓存。 */
typedef struct FC_NAND_SINK_CONTEXT_S
{
    fc_ctl_s *controller;
    BOOL      failed; /* fc_complete_from_nand 返回非 OK（CQ 满等 fatal）时置位 */
} fc_nand_sink_ctx_s;

/**
 * @brief NAND 完成回调：把一条 NAND 完成当场转成 FC 完成项（含 stripe parity 清理）。
 *
 * @param nand_completion NAND 层完成项
 * @param ctx 回调上下文，实际类型为 fc_nand_sink_ctx_s *
 *
 * @note 每条完成当场消费（写 desc/ECC、跑数据路径、组 CQ 项、清 stripe parity），
 * 不跨条缓存；fc_complete_from_nand 返回非 OK 时在上下文中置 failed。
 */
static void fc_nand_completion_sink(const fc_nand_dispatch_comp_s *nand_completion, void *ctx)
{
    fc_nand_sink_ctx_s *sink_ctx = (fc_nand_sink_ctx_s *)ctx;

    if (fc_complete_from_nand(sink_ctx->controller, nand_completion) != FC_STATUS_OK)
    {
        sink_ctx->failed = true;
    }

    /* ── Stripe parity 清理 ──
     * 如果完成的命令是 stripe 数据命令，释放内部 parity 命令资源。 */
#if CONFIG_FC_RAID
    {
        fc_cmd_id_t completed_id = nand_completion->command_id;
        // command_id 是 FC 投递给 NAND 的池内 ID，fc_complete_from_nand 已断言过范围
        ASSERT(completed_id < FC_COMMAND_POOL_DEPTH);
        if (sink_ctx->controller->stripe_parity_for_data[completed_id] != FC_CMD_ID_INVALID)
        {
            fc_cmd_id_t parity_id                                      = sink_ctx->controller->stripe_parity_for_data[completed_id];
            sink_ctx->controller->stripe_parity_for_data[completed_id] = FC_CMD_ID_INVALID;
            fc_stripe_release_parity(sink_ctx->controller, parity_id);
        }
    }
#endif /* CONFIG_FC_RAID */
}

/**
 * @brief FC 服务主入口：先取 NAND 完成，无完成则填充下发
 *
 * @return FC_STATUS_OK；FC_STATUS_IO_ERROR CQ 满 / 提交失败
 */
fc_status_e fc_service(void)
{
    fc_ctl_s          *controller = &g_fc_controller;
    fc_status_e        result;
    fc_nand_sink_ctx_s sink_ctx;

    sink_ctx.controller = controller;
    sink_ctx.failed     = false;

    const BYTE delivered = fc_nand_dispatch_poll(fc_nand_completion_sink, &sink_ctx);

    if (delivered != 0U)
    {
        // 完成已全部消费。只有 CQ 满（FC 侧故障）才报 IO_ERROR；
        // NAND 命令自身的失败不是故障——状态已随 completion 进了 CQ entry。
        if (sink_ctx.failed == true)
        {
            return FC_STATUS_IO_ERROR;
        }

        // 完成和提交放同一轮：否则持续有完成时，提交会被一直让位。
        // 填充失败（IO_ERROR）上升为本轮错误；QUEUE_EMPTY/NOT_READY 不改变
        // “本轮已有完成”的成功语义。
        result = fc_poll_fill(controller);
        return (result == FC_STATUS_IO_ERROR) ? FC_STATUS_IO_ERROR : FC_STATUS_OK;
    }

    // 无完成 → 填充轮次：一次扫描把可下发命令（含链）提交进 NAND per-LUN FIFO。
    result = fc_poll_fill(controller);
    return result;
}

/* ══════════════════════════════════════════════════════════
 * 10. CQ 出口
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 从 CQ 取一条完成项
 *
 * @param completion 输出：完成项
 * @return FC_STATUS_OK；FC_STATUS_QUEUE_EMPTY
 */
fc_status_e fc_poll_completion(fc_cmd_status_s *completion)
{
    fc_status_e result;
    fc_ctl_s   *controller = &g_fc_controller;

    ASSERT(completion != NULL);

    result = fc_completion_queue_pop(&controller->cq, completion);
    return result;
}

/**
 * @brief 从 CQ 批量取完成项
 *
 * @param completions 输出数组
 * @param max_count   最多取几条
 * @param count       输出：实际条数
 * @return FC_STATUS_OK；FC_STATUS_QUEUE_EMPTY
 */
fc_status_e fc_poll_completions(fc_cmd_status_s *completions, WORD max_count, WORD *count)
{
    WORD      n          = 0U;
    fc_ctl_s *controller = &g_fc_controller;

    ASSERT(completions != NULL);
    ASSERT(count != NULL);

    while ((n < max_count) && (fc_completion_queue_pop(&controller->cq, &completions[n]) == FC_STATUS_OK))
    {
        n++;
    }
    *count = n;

    return (n != 0U) ? FC_STATUS_OK : FC_STATUS_QUEUE_EMPTY;
}

/* ══════════════════════════════════════════════════════════
 * 断言终端路径的 FC 现场补充
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 断言终端路径的 FC 现场补充：把各资源池占用与队列长度追加到日志。
 *
 * @note 覆盖 assertion.c 中的同名弱钩子（强符号），由 assertion_terminate 在
 * flush 之前调用；资源统计只读，不干扰停机路径。
 */
void assertion_platform_context(void)
{
    fc_resource_stats_s stats;

    fc_get_resource_stats(&stats);

    LOGE(LOG_TAG_RES "cmd=%hu/%u desc=%hu/%u buf=%hu/%u sq=%hu hsq=%hu cq=%hu\r\n", stats.command_used, FC_COMMAND_POOL_DEPTH, stats.descriptor_used, FC_DESCRIPTOR_POOL_DEPTH, stats.buffer_used, FC_BUFFER_POOL_DEPTH, stats.sq_used, stats.hsq_used, stats.cq_used);
}

/* ══════════════════════════════════════════════════════════
 * 11. 对外 API（原 fc_interface.c；公开签名不变，实现直接落在本文件）
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 建链入口：分配 desc、填 PAA/config/meta、挂链、按需分配 buffer
 *
 * 失败不回滚：已分配的 desc 都在链上，调用方沿链释放即完整回收。唯一合法
 * 失败是资源池空（RESOURCE_EMPTY）。
 *
 * @param command   命令（ALLOCATED 且 plane 段/掩码已设好）
 * @param plane     plane 号
 * @param cw        CW 号
 * @param row_paa   该 plane 的行地址（PAA）
 * @param buffer_id 上层申请的 buffer（buf_own 默认路径传 INVALID）
 * @return FC_STATUS_OK；FC_STATUS_RESOURCE_EMPTY desc/buffer 池空
 */
fc_status_e fc_command_configure_descriptor_with_buffer(fc_cmd_s *command, BYTE plane, BYTE cw, DWORD row_paa, fc_buf_id_t buffer_id)
{
    BYTE        plane_first;
    BYTE        plane_last;
    fc_desc_s  *descriptor;
    fc_buf_id_t alloc_id;
    fc_cmd_id_t command_id = FC_CMD_ID_INVALID;
    DWORD       plane_cw;

    /* ── 校验段：不得触碰任何池 ──
     * 资源申请不到（desc / buffer 池空）→ RESOURCE_EMPTY，是模仿硬件的预期失败。
     * 其余配置错误（句柄 / 生命周期 / 坐标 / buffer 归属）都是调用者 bug ⇒ ASSERT。 */
    ASSERT(command != NULL);
    ASSERT(fc_command_to_id(command, &command_id) == true);
    ASSERT(fc_command_state_get(&g_fc_controller, command_id) == FC_COMMAND_STATE_ALLOCATED);

    (void)fc_command_plane_span(command, &plane_first, &plane_last);
    ASSERT((cw < CW_NUM) && (plane >= plane_first) && (plane < plane_last));
    ASSERT(paa_to_plane(row_paa) == plane);

    if ((command->config.buf_own & FC_BUF_OWN_ALLOC_BY_UPPER) != 0U)
    {
        ASSERT((buffer_id != FC_BUF_ID_INVALID) && (buffer_id < FC_BUFFER_POOL_DEPTH));
        ASSERT(fc_buffer_state_get(&g_fc_controller, buffer_id) == FC_BUFFER_STATE_OCCUPIED);
        ASSERT(g_fc_controller.buffer_owner[buffer_id] == FC_CMD_ID_INVALID);
    }
    else
    {
        ASSERT(buffer_id == FC_BUF_ID_INVALID);
    }
    (void)command_id; /* release 下 ASSERT 裁空；所有者从 descriptor_owner 取 */

    /* ── 机制段 ── */
    descriptor = fc_pool_descriptor_alloc(&g_fc_controller, command);
    if (descriptor == NULL)
    {
        return FC_STATUS_RESOURCE_EMPTY;
    }

    const fc_desc_id_t descriptor_id = (fc_desc_id_t)(descriptor - g_fc_controller.descriptor_pool);
    const fc_cmd_id_t  owner         = g_fc_controller.descriptor_owner[descriptor_id];

    // 先把除 buffer_id 外的字段全部填好，再挂链：desc 一进链就能被任何
    // 走链的读者正确解释（paa/config/meta 均已有效，next_id 已终止）。

    // 两条公式：PAA 与 LBA 的 plane/cw 位置不同，不能再用同一条 plane_cw 公式。
    // desc->paa  —— PAA 空间：plane 与 cw 不相邻，用 paa_with_plane/paa_with_cw 逐字段换；
    // 行地址取自实参 row_paa（每 plane 独立，Policy B 两 plane 可不同块）。
    // desc->meta —— LBA 空间：plane/cw 低 3 位相邻（PLANE_CW_MASK 有效），
    // 高位取自 command->meta（本命令扇区 LBA）。
    plane_cw                    = (plane << PLANE_CW_SHIFT) | cw;
    descriptor->paa             = paa_with_cw(paa_with_plane(row_paa, plane), cw);
    descriptor->config          = command->config;
    descriptor->config.cw_count = 0U; /* 命令级每-plane 数量，desc 内无意义 */
    descriptor->cw_index        = cw;
    descriptor->next_id         = FC_DESC_ID_INVALID;
    descriptor->meta            = (command->meta & ~PLANE_CW_MASK) | plane_cw;

    fc_pool_chain_append(&g_fc_controller, command, descriptor); /* 出池即挂链 */

    // 上层申请（buf_own 10/11）：buffer 由调用者 fc_buffer_alloc 好并传进来，无失败路径。
    if ((command->config.buf_own & FC_BUF_OWN_ALLOC_BY_UPPER) != 0U)
    {
        descriptor->buffer_id                   = buffer_id;
        g_fc_controller.buffer_owner[buffer_id] = owner;
        return FC_STATUS_OK;
    }

    // FC 申请（默认 00/01）：buffer 池比 desc 池小，这里是最现实的失败点。失败**不回滚**：
    // desc 已在链上、buffer_id 保持 INVALID，调用方沿链释放时自然跳过 buffer、回收 desc。
    alloc_id = fc_buffer_alloc();
    if (alloc_id == FC_BUF_ID_INVALID)
    {
        return FC_STATUS_RESOURCE_EMPTY;
    }
    descriptor->buffer_id                  = alloc_id;
    g_fc_controller.buffer_owner[alloc_id] = owner;
    return FC_STATUS_OK;
}

/**
 * @brief 建链入口（FC 申请 buffer 的默认路径）
 *
 * @param command 命令
 * @param plane   plane 号
 * @param cw      CW 号
 * @param row_paa 该 plane 的行地址
 * @return FC_STATUS_OK；FC_STATUS_RESOURCE_EMPTY 池空
 */
fc_status_e fc_command_configure_descriptor(fc_cmd_s *command, BYTE plane, BYTE cw, DWORD row_paa)
{
    return fc_command_configure_descriptor_with_buffer(command, plane, cw, row_paa, FC_BUF_ID_INVALID);
}

/**
 * @brief 释放链头一个 desc（含其 buffer），返回新链头
 *
 * @param head 当前链头
 * @return 新链头；FC_DESC_ID_INVALID = 链已空
 */
fc_desc_id_t fc_descriptor_chain_release_head(fc_desc_id_t head)
{
    fc_pool_chain_release_head(&g_fc_controller, &head);
    return head;
}

/**
 * @brief 沿链释放全部 desc + buffer
 *
 * 建链中途失败的回滚入口。
 *
 * @param head 链头
 * @return FC_DESC_ID_INVALID
 */
fc_desc_id_t fc_descriptor_chain_release_all(fc_desc_id_t head)
{
    fc_pool_chain_release_all(&g_fc_controller, &head);
    return head;
}

/**
 * @brief 提交一条命令
 *
 * 配置错误一律 ASSERT；唯一合法失败是队列满。
 *
 * @param command 命令（ALLOCATED）
 * @return FC_STATUS_OK；FC_STATUS_QUEUE_FULL
 */
fc_status_e fc_submit(fc_cmd_s *command)
{
    fc_cmd_id_t command_id = FC_CMD_ID_INVALID;

    /* ── 校验段：全是调用者自铸值 ⇒ ASSERT；唯一合法失败是队列满（资源）。 ── */
    ASSERT(command != NULL);
    ASSERT(fc_command_to_id(command, &command_id) == true);
    ASSERT(fc_command_state_get(&g_fc_controller, command_id) == FC_COMMAND_STATE_ALLOCATED);
    fc_command_validate(command);
    ASSERT(fc_pool_chain_validate(&g_fc_controller, command_id, command) != 0U);
    (void)command_id; /* release 下 ASSERT 裁空 */

    return fc_sched_submit(&g_fc_controller, command);
}

/**
 * @brief 批量提交（全进或全不进）
 *
 * cache program 批次依赖此语义；链契约要求全批同优先级。
 *
 * @param cmds 命令数组
 * @param n    条数
 * @return FC_STATUS_OK；FC_STATUS_QUEUE_FULL；FC_STATUS_INVALID_ARGUMENT 链契约违反
 */
fc_status_e fc_submit_batch(fc_cmd_s *const cmds[], DWORD n)
{
    ASSERT(cmds != NULL);
    ASSERT(n > 0U);

    WORD  sq_need        = 0U;
    WORD  hsq_need       = 0U;
    BOOL  has_chain      = false;
    BOOL  mixed_queue    = false;
    DWORD first_priority = 0U;

    for (DWORD i = 0U; i < n; i++)
    {
        fc_cmd_s   *command    = cmds[i];
        fc_cmd_id_t command_id = FC_CMD_ID_INVALID;

        ASSERT(command != NULL);
        ASSERT(fc_command_to_id(command, &command_id) == true);
        ASSERT(fc_command_state_get(&g_fc_controller, command_id) == FC_COMMAND_STATE_ALLOCATED);
        fc_command_validate(command);
        ASSERT(fc_pool_chain_validate(&g_fc_controller, command_id, command) != 0U);
        (void)command_id; /* release 下 ASSERT 裁空 */

        if (i == 0U)
        {
            first_priority = command->high_priority;
        }
        else if (command->high_priority != first_priority)
        {
            mixed_queue = true;
        }

        has_chain = (has_chain == true) || (command->config.chain == FC_CHAIN_MORE);

        if (command->high_priority != 0U)
        {
            hsq_need++;
        }
        else
        {
            sq_need++;
        }
    }

    // 链契约：一批里的 chain 必须全批同优先级；**末条允许 MORE**（流式窗口）。
    // · 混优先级 ⇒ 链被拆成 SQ/HSQ 两段，fc_poll_chain_length 只沿本队列 peek，
    //   跳过的成员被当成不存在 ⇒ 序列静默错乱（cache read 输出错页 / 写错序列）；
    // · 末条 MORE 是合法中间窗口：FTL 的序列状态保证在 run_end 发 END；FC 的
    //   chain_owner 跨窗口保留，下一窗口继续。
    // 负例：cmds[0].chain=MORE 且 cmds[1].high_priority=1 ⇒ release 拒绝。
    ASSERT_REJECT(!((has_chain == true) && (mixed_queue == true)), FC_STATUS_INVALID_ARGUMENT, "mixed-priority chain batch");

    if (((WORD)(g_fc_controller.sq.count + sq_need) > FC_SQ_DEPTH) || ((WORD)(g_fc_controller.hsq.count + hsq_need) > FC_HSQ_DEPTH))
    {
        return FC_STATUS_QUEUE_FULL;
    }

    for (DWORD i = 0U; i < n; i++)
    {
        const fc_status_e st = fc_sched_submit(&g_fc_controller, cmds[i]);

        // 预检通过后入队不可能失败；失败即队列不变量被破坏。
        ASSERT(st == FC_STATUS_OK);
        (void)st;
    }

    return FC_STATUS_OK;
}

/**
 * @brief 释放命令槽位（FREE 幂等）
 *
 * 指针身份是配置错误 ⇒ ASSERT；幂等早退在 fc_pool_command_release。
 *
 * @param command 命令；指针须落在命令池内。
 */
void fc_command_release(fc_cmd_s *command)
{
    fc_cmd_id_t command_id = FC_CMD_ID_INVALID;

    ASSERT(command != NULL);
    ASSERT(fc_command_to_id(command, &command_id) == true);
    (void)command_id; /* release（NDEBUG）下 command_id 仅用于 ASSERT */

    fc_pool_command_release(&g_fc_controller, command);
}

/**
 * @brief 是否占用（供 fc_buffer_release / get_user_data / get_base 的前置 ASSERT 用）。
 *
 * @param buffer_id 待检查的 buffer ID。
 *
 * @return true = 池内且已占用；false = 越界或未占用。
 *
 * @note 仅用于 ASSERT，故标 __attribute__((unused))（NDEBUG 下 ASSERT 被裁空，
 * 否则触发 -Wunused-function）。
 */
static BOOL __attribute__((unused)) fc_buffer_is_occupied(fc_buf_id_t buffer_id)
{
    return (buffer_id < FC_BUFFER_POOL_DEPTH) && (fc_buffer_state_get(&g_fc_controller, buffer_id) != FC_BUFFER_STATE_FREE);
}

/**
 * @brief 释放上层预申请的 buffer（尚未挂到命令上）
 *
 * 未占用 / 仍归属某条命令是配置错误 ⇒ ASSERT；release 下由调用链保证。
 *
 * @param buffer_id buffer id
 */
void fc_buffer_release(fc_buf_id_t buffer_id)
{
    ASSERT(buffer_id < FC_BUFFER_POOL_DEPTH);
    ASSERT(fc_buffer_state_get(&g_fc_controller, buffer_id) == FC_BUFFER_STATE_OCCUPIED);
    ASSERT(g_fc_controller.buffer_owner[buffer_id] == FC_CMD_ID_INVALID);

    const BOOL released = fc_pool_buffer_release(&g_fc_controller, buffer_id, FC_CMD_ID_INVALID);

    ASSERT(released);
    (void)released;
}

/**
 * @brief buffer id → USER 数据区指针（跳过 META+CRC）
 *
 * 前置：buffer_id 须已占用；成功时恒非空。
 *
 * @param buffer_id buffer id
 * @return USER 数据区指针
 */
BYTE *fc_buffer_get_user_data(fc_buf_id_t buffer_id)
{
    BYTE *base;

    ASSERT(fc_buffer_is_occupied(buffer_id) == true);
    // 占用即保证池内指针非空（fc_pool_buffer_cw_base 只做数组下标），无需再判 NULL
    base = fc_pool_buffer_cw_base(&g_fc_controller, buffer_id);

    // 对外只暴露 USER 数据区，跳过 META(4) + CRC(4)
    return base + FC_BUFFER_USER_OFFSET;
}

/**
 * @brief buffer id → CW 基址（528B，含 META/CRC/PARITY）
 *
 * 前置：buffer_id 须已占用；成功时恒非空。
 *
 * @param buffer_id buffer id
 * @return CW 基址
 */
BYTE *fc_buffer_get_base(fc_buf_id_t buffer_id)
{
    ASSERT(fc_buffer_is_occupied(buffer_id) == true);
    return fc_pool_buffer_cw_base(&g_fc_controller, buffer_id);
}

/**
 * @brief 返回命令中 CW 号 == cw_index 的 CW 的数据指针
 *
 * 按 paa_to_cw 匹配，不是链走位（flat CW 掩码下两者会分叉）。
 *
 * @param command  命令
 * @param cw_index CW 号
 * @return 数据指针；未找到 / 无 buffer 返回 NULL
 */
BYTE *fc_get_cmd_data_ptr(fc_cmd_s *command, BYTE cw_index)
{
    fc_desc_id_t desc_id;

    // command/desc 链非空是前置条件；cw_index 不在链上仍按查询语义返回 NULL
    ASSERT(command != NULL);
    ASSERT(command->desc_id != FC_DESC_ID_INVALID);

    desc_id = (fc_desc_id_t)command->desc_id;
    for (WORD steps = 0U; (desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH); steps++)
    {
        fc_desc_s *descriptor = fc_descriptor_id_to_descriptor(desc_id);

        if (paa_to_cw(descriptor->paa) == cw_index)
        {
            if (descriptor->buffer_id == FC_BUF_ID_INVALID)
            {
                return NULL;
            }
            return fc_buffer_get_user_data(descriptor->buffer_id);
        }
        desc_id = (fc_desc_id_t)descriptor->next_id;
    }

    return NULL;
}

/**
 * @brief 返回命令中 CW 号 == cw_index 的 CW 的 META 指针
 *
 * 按 paa_to_cw(desc->paa) 匹配，不是链走位。
 *
 * @param command  命令
 * @param cw_index CW 号
 * @return META 指针；未找到 / 无 buffer 返回 NULL
 */
BYTE *fc_buffer_get_meta(fc_cmd_s *command, BYTE cw_index)
{
    fc_desc_id_t did;

    // command 非空是前置条件（gc.c 传入有效命令）；未找到 CW 仍按契约返回 NULL
    ASSERT(command != NULL);

    did = (fc_desc_id_t)command->desc_id;
    for (WORD steps = 0U; (did < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH); steps++)
    {
        fc_desc_s *d = fc_descriptor_id_to_descriptor(did);

        if (paa_to_cw(d->paa) == cw_index)
        {
            return (d->buffer_id != FC_BUF_ID_INVALID) ? fc_pool_buffer_cw_base(&g_fc_controller, d->buffer_id) : NULL;
        }
        did = (fc_desc_id_t)d->next_id;
    }
    return NULL;
}

/**
 * @brief 按 desc 链重建命令的 flat CW 位图
 *
 * 链是 CW 集合的唯一真值；须在 fc_command_release 之前调用。
 *
 * @param command 命令
 * @return flat 位图（bit = plane*CW_NUM + cw）
 */
DWORD fc_get_cmd_cw_bitmap(const fc_cmd_s *command)
{
    fc_desc_id_t desc_id;
    DWORD        mask = 0U;

    ASSERT(command != NULL);

    desc_id = (fc_desc_id_t)command->desc_id;
    for (WORD steps = 0U; (desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH); steps++)
    {
        const fc_desc_s *descriptor = fc_descriptor_id_to_descriptor(desc_id);
        const BYTE       plane      = paa_to_plane(descriptor->paa);
        const BYTE       n          = (plane * CW_NUM + paa_to_cw(descriptor->paa));

        // 与 fc_build_request_data_planes 的同源断言一致：链上的 CW 编号必须在 flat 空间内
        ASSERT(n < CW_TOTAL_MAX);
        mask |= (1U << n);
        desc_id = (fc_desc_id_t)descriptor->next_id;
    }
    return mask;
}

/**
 * @brief 命令读到的所有 CW 是否都是擦除页
 *
 * 空链按「非空」处理（没有 CW 就没有证据）；只对读命令 + 已完成命令有意义。
 *
 * @param command 命令
 * @return true 全部 CW 都是 EMPTY；false 否则
 */
BOOL fc_get_cmd_all_cw_empty(const fc_cmd_s *command)
{
    fc_desc_id_t desc_id;
    WORD         n = 0U;

    ASSERT(command != NULL);

    desc_id = (fc_desc_id_t)command->desc_id;
    while ((desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (n < CW_TOTAL_MAX))
    {
        const fc_desc_s *descriptor = fc_descriptor_id_to_descriptor(desc_id);

        if (fc_cw_status_is_empty(descriptor->status) == false)
        {
            return false;
        }
        n++;
        desc_id = (fc_desc_id_t)descriptor->next_id;
    }

    return (n > 0U);
}

/**
 * @brief 命令 desc 链上最大的 ECC bit 数
 *
 * 须在 fc_command_release 之前调用。
 *
 * @param command 命令
 * @return 最大 ECC bit 数；空链为 0
 */
BYTE fc_get_cmd_ecc_max(const fc_cmd_s *command)
{
    fc_desc_id_t desc_id;
    BYTE         ecc_max = 0U;

    ASSERT(command != NULL);

    desc_id = (fc_desc_id_t)command->desc_id;
    for (WORD steps = 0U; (desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH); steps++)
    {
        const fc_desc_s *descriptor = fc_descriptor_id_to_descriptor(desc_id);

        if (descriptor->ecc_bits > ecc_max)
        {
            ecc_max = descriptor->ecc_bits;
        }
        desc_id = (fc_desc_id_t)descriptor->next_id;
    }
    return ecc_max;
}

/**
 * @brief 命令指针 → 池索引（O(1)）
 *
 * @param command    命令指针
 * @param command_id 输出：池索引
 * @return true 在池内；false 不在命令池区间（如栈上命令）
 */
BOOL fc_command_to_id(const fc_cmd_s *command, fc_cmd_id_t *command_id)
{
    // 层内 helper：调用者（fc_command_configure_descriptor / fc_command_release）已判过
    // command 非空，command_id 是本文件局部地址——传空是调用者编程错误。
    ASSERT(command != NULL);
    ASSERT(command_id != NULL);

    const uintptr_t base = (uintptr_t)g_fc_controller.command_pool;
    const uintptr_t addr = (uintptr_t)command;
    const uintptr_t end  = base + sizeof(g_fc_controller.command_pool);

    if ((addr < base) || (addr >= end) ||
        (((addr - base) % sizeof(fc_cmd_s)) != 0U))
    {
        return false;
    }
    *command_id = (fc_cmd_id_t)((addr - base) / sizeof(fc_cmd_s));
    return true;
}

/**
 * @brief 命令 ID → 命令指针
 *
 * @param command_id 池内命令 ID
 * @return 命令指针
 */
fc_cmd_s *fc_command_id_to_command(WORD command_id)
{
    // command_id 只来自 fc_command_alloc() 铸出的 ID（队列里只存分配产出的 ID），
    // 任何调用者都不可能传入越界值——越界即不变量破坏，留现场。
    ASSERT(command_id < FC_COMMAND_POOL_DEPTH);
    return fc_pool_command_by_id(&g_fc_controller, command_id);
}

/**
 * @brief 描述符 ID → 描述符指针
 *
 * @param descriptor_id 池内描述符 ID
 * @return 描述符指针
 */
fc_desc_s *fc_descriptor_id_to_descriptor(WORD descriptor_id)
{
    // descriptor_id 只来自命令 desc 链（configure_descriptors 铸出），恒在池内。
    ASSERT(descriptor_id < FC_DESCRIPTOR_POOL_DEPTH);
    return fc_pool_descriptor_by_id(&g_fc_controller, descriptor_id);
}

/**
 * @brief 汇总队列/三池/调度器调试状态
 *
 * @param out 输出
 */
void fc_debug_state_get(fc_debug_state_s *out)
{
    ASSERT(out != NULL);

    memset(out, 0, sizeof(*out));
    out->sq_used     = g_fc_controller.sq.count;
    out->hsq_used    = g_fc_controller.hsq.count;
    /* 诊断采样：只报告 (ch,ce,lun)=(0,0,0) 的 LUN，不是全维遍历。 */
    out->chain_owner = (BYTE)g_fc_controller.scheduler.chain_owner[0][0][0];
    out->lun_used    = fc_nand_dispatch_lun_used(0U, 0U, 0U);
    out->lun_phase   = fc_nand_dispatch_lun_phase(0U, 0U, 0U);
    fc_debug_fill_counters(&out->fill_calls, &out->fill_pops);
    out->sched_sq_count = (g_fc_controller.scheduler.sq != NULL) ? g_fc_controller.scheduler.sq->count : 0U;
    out->sched_sq_bound = ((g_fc_controller.scheduler.sq == &g_fc_controller.sq) && (g_fc_controller.scheduler.hsq == &g_fc_controller.hsq)) ? 1U : 0U;

    const fc_cmd_id_t sq_id = fc_submission_queue_peek(&g_fc_controller.sq, 0U);

    if (sq_id != FC_CMD_ID_INVALID)
    {
        const fc_cmd_s *cmd = fc_command_id_to_command(sq_id);

        out->sq_head_id    = sq_id;
        out->sq_head_state = (BYTE)fc_command_state_get(&g_fc_controller, sq_id);
        out->sq_head_chain = (WORD)cmd->config.chain;
        out->sq_head_op    = (BYTE)cmd->config.operation;
    }

    const fc_cmd_id_t hsq_id = fc_submission_queue_peek(&g_fc_controller.hsq, 0U);

    if (hsq_id != FC_CMD_ID_INVALID)
    {
        const fc_cmd_s *cmd = fc_command_id_to_command(hsq_id);

        out->hsq_head_id    = hsq_id;
        out->hsq_head_chain = (WORD)cmd->config.chain;
        out->hsq_head_op    = (BYTE)cmd->config.operation;
    }
}

/**
 * @brief 汇总队列与三池占用
 *
 * @param stats 输出
 */
void fc_get_resource_stats(fc_resource_stats_s *stats)
{
    // stats 为出参前置条件（调用者恒传有效指针）
    ASSERT(stats != NULL);

    stats->sq_used         = g_fc_controller.sq.count;
    stats->hsq_used        = g_fc_controller.hsq.count;
    stats->cq_used         = g_fc_controller.cq.count;
    stats->command_used    = g_fc_controller.command_used;
    stats->descriptor_used = g_fc_controller.descriptor_used;
    stats->buffer_used     = g_fc_controller.buffer_used;
    stats->command_high    = g_fc_controller.command_high;
    stats->descriptor_high = g_fc_controller.descriptor_high;
    stats->buffer_high     = g_fc_controller.buffer_high;

#ifndef NDEBUG
    /* debug 档做一次全池扫描，证明 O(1) 计数器与真值一致（C7 负例：
     * 任一闸门漏增减，这里立刻炸）。release 档只读计数器。 */
    WORD command_scan    = 0U;
    WORD descriptor_scan = 0U;
    WORD buffer_scan     = 0U;

    for (WORD index = 0U; index < FC_COMMAND_POOL_DEPTH; index++)
    {
        if (fc_command_state_get(&g_fc_controller, index) != FC_COMMAND_STATE_FREE)
        {
            command_scan++;
        }
    }
    for (WORD index = 0U; index < FC_DESCRIPTOR_POOL_DEPTH; index++)
    {
        if (fc_descriptor_owner_get(&g_fc_controller, index) != FC_CMD_ID_INVALID)
        {
            descriptor_scan++;
        }
    }
    for (WORD index = 0U; index < FC_BUFFER_POOL_DEPTH; index++)
    {
        if (fc_buffer_state_get(&g_fc_controller, index) != FC_BUFFER_STATE_FREE)
        {
            buffer_scan++;
        }
    }
    ASSERT(command_scan == stats->command_used);
    ASSERT(descriptor_scan == stats->descriptor_used);
    ASSERT(buffer_scan == stats->buffer_used);
#endif
}

/**
 * @brief 打印 FC 容量与拓扑（开机一次）
 *
 * 与断言路径的 [RES] 现场行互补：此处打容量，那里打占用。
 */
void fc_boot_report(void)
{
    const char *data_path = "PIO"; /* FMC NAND 无 DMA 请求线，数据面固定 PIO */

    LOGI("[FC  ] cmd=%u desc=%u buf=%u sq=%u hsq=%u cq=%u\r\n", FC_COMMAND_POOL_DEPTH, FC_DESCRIPTOR_POOL_DEPTH, FC_BUFFER_POOL_DEPTH, FC_SQ_DEPTH, FC_HSQ_DEPTH, FC_CQ_DEPTH);
    LOGI("[FC  ] topology ch=%d ce=%d lun=%d plane=%d raid=%s\r\n", CH_NUM, CE_NUM, LUN_NUM, PLANE_NUM, (CONFIG_FC_RAID != 0U) ? "on" : "off");
    LOGI("[FC  ] data path=%s ecc=%s random=%s\r\n", data_path, fc_ecc_algo_name(), fc_random_algo_name());
}

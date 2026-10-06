/*
 * fc.c — FC 核心实现（命令池 / 描述符池 / 缓冲池 / 调度 / 服务主循环）
 *
 * 资源模型（三池，全部静态固定，无动态内存）：
 *   - Command Pool（64）：fc_command_t，单条命令
 *   - Descriptor Pool（256）：fc_descriptor_t，一个 CW 一个描述符，next_id 串联
 *   - Buffer Pool（128）：528B CW 缓冲，DMA 直访（noncacheable 段）
 *
 * 生命周期（对 FTL）：
 *   fc_command_alloc() → 填 config/参数 → fc_command_configure_descriptors()
 *   → fc_write_cmd_data() → fc_submit() → fc_service()（后台推进）
 *   → fc_poll_completion() 取完成 → fc_command_release() 回收。
 *
 * 文件结构（按职责分区，函数定义顺序即依赖顺序，无前向声明）：
 *    1. 静态资源        三池 + 链表数组 + g_fc_controller
 *    2. 查询/转换小工具 is_page_read/write、getter、ID→指针、buffer 工具
 *    3. 初始化          fc_register_queue_state、fc_init_internal
 *    4. 资源分配/回滚   command/descriptor/buffer alloc、rollback、release
 *    5. 命令配置        fc_command_configure_descriptor(s)_internal
 *    6. 提交            fc_submit_to_queue、fc_submit_internal
 *    7. 完成            fc_complete_internal、fc_complete_from_nand
 *    8. Stripe RAID     fc_stripe_release_parity、fc_stripe_submit
 *    9. 调度/轮询       fc_poll_* 辅助 + fc_poll_internal
 *   10. CQ 出口         fc_poll_completion_internal
 *
 * 服务主循环（fc_poll_internal）：
 *   ① nand_dispatch_poll() 推进 NAND 层，有完成 → fc_complete_from_nand()
 *   ② 无完成 → fc_poll_fill() 一次扫描 HSQ/SQ，按 LUN 保序把能放下的命令（含链）
 *      提交进 NAND per-LUN FIFO（分派 fc_poll_dispatch_single() /
 *      fc_poll_dispatch_chain()；后者内部构建 request → submit/submit_chain，
 *      raid=ENCODE 走 fc_stripe_submit）
 *
 * 状态机：FREE → ALLOCATED → QUEUED → ACTIVE → COMPLETED。
 */

#include "fc.h"

#include "common/assertion.h"
#include "common/log.h"
#include "fc_crc_engine.h"
#include "fc_data_path.h"
#include "fc_internal.h"
#include "fc_queue.h"
#include "fc_registers.h"
#include "fc_scheduler.h"
#include "nand/nand_dispatch.h"

#include <stddef.h>
#include <string.h>

#if CONFIG_FC_RAID
#    include "fc_raid_engine.h"
#endif

/* ══════════════════════════════════════════════════════════
 * 1. 静态资源
 *
 * 静态资源区：FC 控制面固定资源，当前位于默认 .bss（AXI SRAM）。
 * 不使用动态内存。后续根据 DMA/Cache 和 Buffer Pool 设计，
 * 可将各数组分别放入指定链接段。
 *
 *   池布局：
 *     command_pool / descriptor_pool / buffer_pool —— 三池本体
 *     descriptor_owner[desc] —— desc 归属的命令 ID
 *     command_state / command_free_next —— 命令状态 + 空闲链表
 *     descriptor_free_next / descriptor_owned_next —— desc 空闲/归属链表
 *     buffer_free_next / buffer_state —— buffer 空闲链表 + 占用状态
 *     command_descriptor_head[cmd] —— 命令的 desc 链头（回滚/释放用）
 * ══════════════════════════════════════════════════════════ */

static fc_command_t        g_fc_command_pool[FC_COMMAND_POOL_DEPTH];
static fc_descriptor_t     g_fc_descriptor_pool[FC_DESCRIPTOR_POOL_DEPTH];
static uint8_t             g_fc_buffer_pool[FC_BUFFER_POOL_DEPTH][FC_BUFFER_POOL_CW_SIZE] __attribute__((section(".noncacheable_buffer")));
static fc_command_id_t     g_fc_sq_storage[FC_SQ_DEPTH];
static fc_command_id_t     g_fc_hsq_storage[FC_HSQ_DEPTH];
static fc_command_status_t g_fc_cq_storage[FC_CQ_DEPTH];
static fc_command_id_t     g_fc_descriptor_owner[FC_DESCRIPTOR_POOL_DEPTH];
static uint8_t             g_fc_command_state[FC_COMMAND_POOL_DEPTH];
static fc_command_id_t     g_fc_command_free_next[FC_COMMAND_POOL_DEPTH];
static uint16_t            g_fc_descriptor_free_next[FC_DESCRIPTOR_POOL_DEPTH];
static uint16_t            g_fc_descriptor_owned_next[FC_DESCRIPTOR_POOL_DEPTH];
static uint16_t            g_fc_buffer_free_next[FC_BUFFER_POOL_DEPTH];
static uint8_t             g_fc_buffer_state[FC_BUFFER_POOL_DEPTH];
static uint16_t            g_fc_command_descriptor_head[FC_COMMAND_POOL_DEPTH];

/* GET/SET_FEATURES 的 4 字节内联数据暂存，按 command_id（= 命令池槽位号）索引。
 * 为什么需要：SQ 命令提交后不可写（见 fc_command.h「SQ 不可变」），而引擎需要一个
 * 稳定的 4 字节落点写回寄存器值；返回值唯一的归宿是 CQ 的 param 联合体，
 * 故这里只做提交→完成之间的中转。command_id 恒 < FC_COMMAND_POOL_DEPTH（fc.c 里
 * command_pool[index].command_id = index），索引安全。 */
static uint8_t g_fc_return_staging[FC_COMMAND_POOL_DEPTH][4];

/* 全局单例控制器：对 API 薄壳（fc_interface.c）可见，FTL 不可见 */
fc_t g_fc_controller;

/* ══════════════════════════════════════════════════════════
 * 2. 查询 / 转换小工具
 * ══════════════════════════════════════════════════════════ */

/*
 * 判断 operation 是否为带 ECC 编码数据的 Page Read。
 * 这类命令读取的 NAND 页数据经过 ECC 编码，需要 FC 内部
 * 在 CQ push 前自动做 ECC 解码 + 条件解扰。
 *
 * READ_ID / READ_STATUS / GET_FEATURES 等虽然也有数据返回，
 * 但它们不是 ECC 编码的 Page Data，不应走 Data Path 处理。
 */
static uint8_t fc_is_page_read(uint8_t operation)
{
    switch (operation)
    {
        case OP_NORMAL_READ_PAGE_WITH_DATA_OUT:
        case OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT:
        case OP_READ_PAGE_CACHE_SEQ:
        case OP_READ_PAGE_CACHE_RANDOM:
        case OP_READ_PAGE_CACHE_LAST:
        case OP_RANDOM_DATA_READ:
        case OP_READ_PAGE_FETCH: /* Phase V */
            return 1U;
        default:
            return 0U;
    }
}

/*
 * 判断 operation 是否为带 ECC 编码数据的 Page Write。
 * 写入 NAND 之前需要对数据做 Random 加扰 + ECC 编码 + RAID XOR。
 *
 * SET_FEATURES 虽然也写数据，但不是 ECC 编码的 Page Data，不应走 Data Path。
 */
static uint8_t fc_is_page_write(uint8_t operation)
{
    switch (operation)
    {
        case OP_NORMAL_PROGRAM_PAGE:
        case OP_PROGRAM_PAGE_CACHE:
        case OP_OTP_DATA_PROGRAM:
        case OP_PROGRAM_PAGE_MULTI_PLANE:
        case OP_PROGRAM_PAGE_ISSUE: /* Phase V */
            return 1U;
        default:
            return 0U;
    }
}

/* ── 资源统计读取器（fc_get_resource_stats 用，fc_interface.c 调用） ── */
uint16_t fc_get_sq_count(const fc_t *controller)
{
    return controller->sq.count;
}
uint16_t fc_get_hsq_count(const fc_t *controller)
{
    return controller->hsq.count;
}
uint16_t fc_get_cq_count(const fc_t *controller)
{
    return controller->cq.count;
}
uint8_t fc_command_state_get(const fc_t *controller, fc_command_id_t command_id)
{
    return (command_id < FC_COMMAND_POOL_DEPTH) ? controller->command_state[command_id] : FC_COMMAND_STATE_FREE;
}
fc_command_id_t fc_descriptor_owner_get(const fc_t *controller, uint16_t descriptor_id)
{
    return (descriptor_id < FC_DESCRIPTOR_POOL_DEPTH) ? controller->descriptor_owner[descriptor_id] : FC_COMMAND_ID_INVALID;
}
uint8_t fc_buffer_state_get(const fc_t *controller, fc_buffer_id_t buffer_id)
{
    return (buffer_id < FC_BUFFER_POOL_DEPTH) ? controller->buffer_state[buffer_id] : 0U;
}

/* ── 命令/描述符 ID → 指针（范围由公共壳 fc_interface.c 校验） ── */
fc_command_t *fc_command_id_to_command_internal(fc_t *controller, uint16_t command_id)
{
    /* 公共壳 fc_command_id_to_command() 已断言过 command_id < FC_COMMAND_POOL_DEPTH */
    ASSERT(command_id < FC_COMMAND_POOL_DEPTH);
    return &controller->command_pool[command_id];
}
fc_descriptor_t *fc_descriptor_id_to_descriptor_internal(fc_t *controller, uint16_t descriptor_id)
{
    /* 公共壳 fc_descriptor_id_to_descriptor() 已断言过 descriptor_id < FC_DESCRIPTOR_POOL_DEPTH */
    ASSERT(descriptor_id < FC_DESCRIPTOR_POOL_DEPTH);
    return &controller->descriptor_pool[descriptor_id];
}

/* 命令 ID 有效性（< 池深度） */
static uint8_t fc_command_id_is_valid(fc_command_id_t command_id)
{
    return (uint8_t)(command_id < FC_COMMAND_POOL_DEPTH);
}

/* 内部：直接返回 CW 基址（528B，含 META/CRC/PARITY），不做 USER 偏移 */
uint8_t *fc_buffer_id_to_real_buffer_internal(fc_t *controller, fc_buffer_id_t buffer_id)
{
    return controller->buffer_pool[buffer_id];
}

/* 内部：释放 buffer（状态清零 + 挂回空闲链表头） */
fc_status_t fc_buffer_release_internal(fc_t *controller, fc_buffer_id_t buffer_id)
{
    controller->buffer_state[buffer_id]     = 0U;
    controller->buffer_free_next[buffer_id] = controller->buffer_free_head;
    controller->buffer_free_head            = buffer_id;
    return FC_STATUS_OK;
}

/* 命令 desc 链的长度（走链计数，0xFFFF 终止；无 desc 时 0）。
 * ⚠ **链长度是唯一真值**：flat CW 掩码下 config.cw 已不再等于 CW 数
 *   （见 fc_command.h「链长度即真值」），任何长度/每-plane 计数一律走链。
 * 上界 CW_TOTAL_MAX 只是防损坏链死循环的安全网（合法链长恒 ≤ 它）。 */
static uint8_t fc_command_descriptor_count(const fc_t *controller, const fc_command_t *command)
{
    fc_descriptor_id_t descriptor_id = (fc_descriptor_id_t)command->desc_id;
    uint8_t            count         = 0U;

    while ((descriptor_id < FC_DESCRIPTOR_POOL_DEPTH) && (count < CW_TOTAL_MAX))
    {
        count++;
        descriptor_id = (fc_descriptor_id_t)controller->descriptor_pool[descriptor_id].next_id;
    }
    return count;
}

/* 校验命令的 desc 链是否全部归该命令所有（所有权一致性） */
uint8_t fc_command_descriptors_owned_internal(const fc_t *controller, fc_command_id_t command_id, const fc_command_t *command)
{
    fc_descriptor_id_t descriptor_id = (fc_descriptor_id_t)command->desc_id;
    uint8_t            remaining     = fc_command_descriptor_count(controller, command);

    while (remaining > 0U)
    {
        if (descriptor_id >= FC_DESCRIPTOR_POOL_DEPTH)
        {
            return 0U;
        }
        if (controller->descriptor_owner[descriptor_id] != command_id)
        {
            return 0U;
        }
        descriptor_id = (fc_descriptor_id_t)controller->descriptor_pool[descriptor_id].next_id;
        remaining--;
    }
    return 1U;
}

/* ══════════════════════════════════════════════════════════
 * 3. 初始化
 * ══════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * fc_register_queue_state
 *   功能：把 SQ/HSQ/CQ 计数 + ACTIVE 命令数汇总进 fc_registers 的状态位（对外可见）。
 *   参数：controller —— FC 控制器。
 *   返回：无。
 *   实现要点：纯状态快照（由当前 state 重算，无副作用），可重复调用。
 * -------------------------------------------------------------------- */
static void fc_register_queue_state(fc_t *controller)
{
    uint32_t status = controller->registers.status & ~(FC_STATUS_READY | FC_STATUS_BUSY | FC_STATUS_SQ_NOT_EMPTY | FC_STATUS_HSQ_NOT_EMPTY | FC_STATUS_CQ_NOT_EMPTY);
    uint16_t index;
    uint8_t  active_found = 0U;

    for (index = 0U; index < FC_COMMAND_POOL_DEPTH; index++)
    {
        if (controller->command_state[index] == FC_COMMAND_STATE_ACTIVE)
        {
            active_found = 1U;
            break;
        }
    }
    if (active_found != 0U)
    {
        status |= FC_STATUS_BUSY;
    }
    else
    {
        status |= FC_STATUS_READY;
    }
    if (controller->sq.count != 0U)
    {
        status |= FC_STATUS_SQ_NOT_EMPTY;
    }
    if (controller->hsq.count != 0U)
    {
        status |= FC_STATUS_HSQ_NOT_EMPTY;
    }
    if (controller->cq.count != 0U)
    {
        status |= FC_STATUS_CQ_NOT_EMPTY;
    }

    controller->registers.status   = status;
    controller->registers.sq_head  = controller->sq.head;
    controller->registers.sq_tail  = controller->sq.tail;
    controller->registers.hsq_head = controller->hsq.head;
    controller->registers.hsq_tail = controller->hsq.tail;
    controller->registers.cq_head  = controller->cq.head;
    controller->registers.cq_tail  = controller->cq.tail;
}

/* --------------------------------------------------------------------
 * fc_init_internal
 *   功能：初始化 FC 控制面（三池清零 + 空闲链表建立 + 队列/调度器/NAND）。
 *   参数：controller —— FC 控制器。
 *   返回：FC_STATUS_OK / 子初始化错误。
 *   实现要点（步骤编号与函数体内注释一一对应）：
 *     ① 三池本体 + 队列存储 + 命令状态位图 memset 归零（数组地址不变）。
 *     ② controller 清零后挂上各静态数组指针。
 *     ③ SQ/HSQ/CQ 三条环形队列 init。
 *     ④ 调度器绑定 SQ/HSQ + 清 busy/饥饿计数。
 *     ⑤ nand_dispatch_init()——本函数唯一的 NAND 依赖，纯 memset 不碰硬件，
 *        因此 fc_init 可以先于 nand_init 执行（见 main.c 的级联顺序）。
 *     ⑥ CRC 硬件、⑦ stripe 积累状态（CONFIG_FC_RAID 时）。
 *     ⑧ 串三张空闲链表 + 置 free head；末尾刷新软件寄存器影子。
 *   幂等：可重复调用以重置（不释放任何硬件资源）。
 *   流程图见 docs/design/fc_nand_design.md §22（图 15）。
 * -------------------------------------------------------------------- */
fc_status_t fc_init_internal(fc_t *controller)
{
    fc_status_t result;
    uint16_t    index;

    /* ── ① 清底：三池本体 + 三条队列的存储 + 命令状态位图全部归零。
     *    这些数组都是文件作用域 static（见 §1 静态资源），此处只清内容，
     *    数组地址不变——DMA 看到的物理地址在整个固件生命周期内固定。 ── */
    memset(g_fc_command_pool, 0, sizeof(g_fc_command_pool));
    memset(g_fc_descriptor_pool, 0, sizeof(g_fc_descriptor_pool));
    memset(g_fc_buffer_pool, 0, sizeof(g_fc_buffer_pool));
    memset(g_fc_sq_storage, 0, sizeof(g_fc_sq_storage));
    memset(g_fc_hsq_storage, 0, sizeof(g_fc_hsq_storage));
    memset(g_fc_cq_storage, 0, sizeof(g_fc_cq_storage));
    memset(g_fc_command_state, 0, sizeof(g_fc_command_state));
    memset(g_fc_return_staging, 0, sizeof(g_fc_return_staging));

    /* ── ② 把池地址挂进 controller。三池本体与各簿记数组是 static，
     *    fc_t 只存指针——这样 fc_t 可以整体赋值/传递而不搬运大数组。 ── */
    *controller                         = (fc_t){0};
    controller->command_pool            = g_fc_command_pool;
    controller->descriptor_pool         = g_fc_descriptor_pool;
    controller->buffer_pool             = g_fc_buffer_pool;
    controller->descriptor_owner        = g_fc_descriptor_owner;
    controller->command_state           = g_fc_command_state;
    controller->command_free_next       = g_fc_command_free_next;
    controller->descriptor_free_next    = g_fc_descriptor_free_next;
    controller->descriptor_owned_next   = g_fc_descriptor_owned_next;
    controller->buffer_free_next        = g_fc_buffer_free_next;
    controller->buffer_state            = g_fc_buffer_state;
    controller->command_descriptor_head = g_fc_command_descriptor_head;
    /* 软件寄存器影子复位（status = READY）；之后由 fc_register_queue_state() 持续刷新 */
    fc_registers_reset(&controller->registers);

    /* ── ③ 三条环形队列：SQ 64（普通命令）/ HSQ 16（高优先，如 read retry）/
     *    CQ 64（完成项）。init 只登记存储与深度，head/tail/count 归零。 ── */
    result = fc_submission_queue_init(&controller->sq, g_fc_sq_storage, FC_SQ_DEPTH);
    if (result != FC_STATUS_OK)
    {
        return result;
    }
    result = fc_submission_queue_init(&controller->hsq, g_fc_hsq_storage, FC_HSQ_DEPTH);
    if (result != FC_STATUS_OK)
    {
        return result;
    }
    result = fc_completion_queue_init(&controller->cq, g_fc_cq_storage, FC_CQ_DEPTH);
    if (result != FC_STATUS_OK)
    {
        return result;
    }
    /* ── ④ 调度器绑定 SQ/HSQ 指针（"忙"查 NAND 侧 nand_dispatch_lun_free，无 busy 表） ── */
    result = fc_scheduler_init(&controller->scheduler, &controller->sq, &controller->hsq);
    if (result != FC_STATUS_OK)
    {
        return result;
    }

    /* ── ⑤ 本函数里唯一的 NAND 依赖：只清 NAND 侧的通道队列与 per-LUN 链状态
     *    （纯 memset，不碰 FMC 硬件）。硬件那半由 nand_init() 负责，且在 main()
     *    里排在 fc_init() 之后——正因为这里不碰硬件，FC 才能先初始化。 ── */
    if (nand_dispatch_init() != NAND_RESULT_OK)
    {
        return FC_STATUS_IO_ERROR;
    }

    /* 初始化 RAID 引擎上下文（每个 LUN 独立） */
#if CONFIG_FC_RAID
    for (uint8_t lun = 0U; lun < LUN_NUM; lun++)
    {
        fc_raid_init_ctx(lun);
    }
#endif /* CONFIG_FC_RAID */

    /* ── ⑥ CRC 硬件（STM32H743 CRC 外设）：写路径编码时给每个 CW 算 CRC ── */
    fc_crc_init();

    /* 初始化 Stripe RAID 积累状态 */
#if CONFIG_FC_RAID
    for (uint8_t lun = 0U; lun < LUN_NUM; lun++)
    {
        controller->stripe_accum[lun] = (fc_stripe_accum_t){0};
    }
    for (index = 0U; index < FC_COMMAND_POOL_DEPTH; index++)
    {
        controller->stripe_parity_for_data[index] = FC_COMMAND_ID_INVALID;
    }
#endif /* CONFIG_FC_RAID */

    /* ── ⑦ 串空闲链表。三张链都是"下标即 ID"的静态链，分配 = 取 head 并把
     *    head 推到 next，释放 = 头插（见 §4 资源分配 / 回滚 / 释放）：
     *      command_free_next    : 0→1→…→63 →INVALID   （head=0）
     *      descriptor_free_next : 0→1→…→255→UINT16_MAX（head=0）
     *      buffer_free_next     : 0→1→…→127→UINT16_MAX（head=0）
     *    同时初始化 desc 链头 command_descriptor_head[cmd]（每命令一条 desc 链）。 ── */
    for (index = 0U; index < FC_COMMAND_POOL_DEPTH; index++)
    {
        controller->command_free_next[index]       = ((index + 1U) < FC_COMMAND_POOL_DEPTH) ? (fc_command_id_t)(index + 1U) : FC_COMMAND_ID_INVALID;
        controller->command_descriptor_head[index] = UINT16_MAX;
        controller->command_pool[index].command_id = index;
    }
    /* 串接描述符空闲/归属链表 */
    for (index = 0U; index < FC_DESCRIPTOR_POOL_DEPTH; index++)
    {
        controller->descriptor_owner[index]      = FC_COMMAND_ID_INVALID;
        controller->descriptor_free_next[index]  = ((index + 1U) < FC_DESCRIPTOR_POOL_DEPTH) ? (uint16_t)(index + 1U) : UINT16_MAX;
        controller->descriptor_owned_next[index] = UINT16_MAX;
    }
    /* 串接 buffer 空闲链表 + 清占用态 */
    for (index = 0U; index < FC_BUFFER_POOL_DEPTH; index++)
    {
        controller->buffer_free_next[index] = ((index + 1U) < FC_BUFFER_POOL_DEPTH) ? (uint16_t)(index + 1U) : UINT16_MAX;
        controller->buffer_state[index]     = 0U;
    }
    controller->command_free_head    = 0U;
    controller->descriptor_free_head = 0U;
    controller->buffer_free_head     = 0U;
    controller->active_handle        = FC_COMMAND_ID_INVALID;

    /* 首次刷新软件寄存器影子：此刻三池全空 → status 只剩 READY 位 */
    fc_register_queue_state(controller);
    return FC_STATUS_OK;
}

/* ══════════════════════════════════════════════════════════
 * 4. 资源分配 / 回滚 / 释放
 * ══════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * fc_command_alloc_internal
 *   功能：从空闲链表分配一个命令槽位并置 ALLOCATED。
 *   参数：controller —— FC 控制器。
 *   返回：命令指针；池空返回 NULL。
 *   实现要点：清零 + command_id 置入 + desc_id=INVALID（未配置描述符）。
 * -------------------------------------------------------------------- */
fc_command_t *fc_command_alloc_internal(fc_t *controller)
{
    fc_command_id_t command_id;

    command_id = controller->command_free_head;
    if (command_id == FC_COMMAND_ID_INVALID)
    {
        return NULL;
    }
    controller->command_free_head                   = controller->command_free_next[command_id];
    controller->command_pool[command_id]            = (fc_command_t){0};
    controller->command_pool[command_id].command_id = command_id;
    controller->command_pool[command_id].desc_id    = FC_DESCRIPTOR_ID_INVALID;
    controller->command_state[command_id]           = FC_COMMAND_STATE_ALLOCATED;
    return &controller->command_pool[command_id];
}

/* --------------------------------------------------------------------
 * fc_descriptor_alloc_internal
 *   功能：从空闲链表分配一个描述符并挂到命令的 desc 链头。
 *   参数：controller —— FC 控制器；command —— 归属命令。
 *   返回：描述符指针；池空返回 NULL。
 *   实现要点：descriptor_owner[desc]=command_id；
 *         owned_next 把新 desc 插到 command_descriptor_head[cmd] 链头。
 * -------------------------------------------------------------------- */
static fc_descriptor_t *fc_descriptor_alloc_internal(fc_t *controller, fc_command_t *command)
{
    uint16_t        descriptor_index;
    fc_command_id_t command_id;

    /* Derive command_id from pointer arithmetic (already validated at public API). */
    command_id       = (fc_command_id_t)(command - controller->command_pool);
    descriptor_index = controller->descriptor_free_head;
    if (descriptor_index == UINT16_MAX)
    {
        return NULL;
    }
    controller->descriptor_free_head                    = controller->descriptor_free_next[descriptor_index];
    controller->descriptor_pool[descriptor_index]       = (fc_descriptor_t){0};
    controller->descriptor_owner[descriptor_index]      = command_id;
    controller->descriptor_owned_next[descriptor_index] = controller->command_descriptor_head[command_id];
    controller->command_descriptor_head[command_id]     = descriptor_index;
    return &controller->descriptor_pool[descriptor_index];
}

/* --------------------------------------------------------------------
 * fc_buffer_alloc_internal
 *   功能：从空闲链表分配一个 buffer 槽位并置占用。
 *   参数：controller —— FC 控制器。
 *   返回：buffer ID；池空返回 FC_BUFFER_ID_INVALID。
 * -------------------------------------------------------------------- */
fc_buffer_id_t fc_buffer_alloc_internal(fc_t *controller)
{
    uint16_t buffer_index;

    buffer_index = controller->buffer_free_head;
    if (buffer_index == UINT16_MAX)
    {
        return FC_BUFFER_ID_INVALID;
    }
    controller->buffer_free_head           = controller->buffer_free_next[buffer_index];
    controller->buffer_state[buffer_index] = 1U;
    return (fc_buffer_id_t)buffer_index;
}

/* --------------------------------------------------------------------
 * fc_command_resources_rollback
 *   功能：回滚命令已分配的全部描述符 + buffer（配置失败时调用）。
 *   参数：controller —— FC 控制器；command_id —— 命令。
 *   返回：无。
 *   实现要点：沿 command_descriptor_head[cmd] 链逐个释放 buffer、
 *         描述符挂回空闲链，最后清链头。
 * -------------------------------------------------------------------- */
static void fc_command_resources_rollback(fc_t *controller, fc_command_id_t command_id)
{
    uint16_t descriptor_index = controller->command_descriptor_head[command_id];

    while (descriptor_index != UINT16_MAX)
    {
        const uint16_t       next_descriptor_index = controller->descriptor_owned_next[descriptor_index];
        const fc_buffer_id_t buffer_id             = controller->descriptor_pool[descriptor_index].buffer_id;

        if (buffer_id != FC_BUFFER_ID_INVALID)
        {
            (void)fc_buffer_release_internal(controller, buffer_id);
        }
        controller->descriptor_pool[descriptor_index]       = (fc_descriptor_t){0};
        controller->descriptor_owner[descriptor_index]      = FC_COMMAND_ID_INVALID;
        controller->descriptor_owned_next[descriptor_index] = UINT16_MAX;
        controller->descriptor_free_next[descriptor_index]  = controller->descriptor_free_head;
        controller->descriptor_free_head                    = descriptor_index;
        descriptor_index                                    = next_descriptor_index;
    }
    controller->command_descriptor_head[command_id] = UINT16_MAX;
}

/* --------------------------------------------------------------------
 * fc_command_release_internal_api
 *   功能：释放命令全部资源（desc 链 + buffer + command 槽位）。
 *   参数：controller —— FC 控制器；command —— 命令。
 *   返回：恒 FC_STATUS_OK。
 *   实现要点：沿 command_descriptor_head[cmd] 链释放 desc+buffer，
 *         命令槽位置 FREE 回池。调用方（公共接口）已保证状态合法、
 *         命令不在 CQ 中。
 * -------------------------------------------------------------------- */
fc_status_t fc_command_release_internal_api(fc_t *controller, fc_command_t *command)
{
    uint16_t        descriptor_index;
    uint16_t        next_descriptor_index;
    fc_command_id_t command_id;

    command_id       = (fc_command_id_t)(command - controller->command_pool);
    descriptor_index = controller->command_descriptor_head[command_id];
    while (descriptor_index != UINT16_MAX)
    {
        next_descriptor_index                               = controller->descriptor_owned_next[descriptor_index];
        controller->descriptor_pool[descriptor_index]       = (fc_descriptor_t){0};
        controller->descriptor_owner[descriptor_index]      = FC_COMMAND_ID_INVALID;
        controller->descriptor_owned_next[descriptor_index] = UINT16_MAX;
        controller->descriptor_free_next[descriptor_index]  = controller->descriptor_free_head;
        controller->descriptor_free_head                    = descriptor_index;
        descriptor_index                                    = next_descriptor_index;
    }
    controller->command_descriptor_head[command_id] = UINT16_MAX;
    controller->command_pool[command_id]            = (fc_command_t){0};
    controller->command_state[command_id]           = FC_COMMAND_STATE_FREE;
    controller->command_free_next[command_id]       = controller->command_free_head;
    controller->command_free_head                   = command_id;
    return FC_STATUS_OK;
}

/* ══════════════════════════════════════════════════════════
 * 5. 命令配置
 * ══════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * fc_cw_mask_popcount
 *   功能：统计 flat CW 掩码的置位数（bit n = plane*CW_NUM + cw）。
 *   参数：mask —— flat CW 集合掩码（uint32_t，覆盖 CW_TOTAL_MAX = 8 位）。
 *   返回：置位数（0..CW_TOTAL_MAX）。
 *   实现要点：CW_TOTAL_MAX=8，逐位循环即最优，不引 __builtin_popcount
 *         （避免 libgcc __popcountsi2 依赖）。
 * -------------------------------------------------------------------- */
static uint8_t __attribute__((unused)) fc_cw_mask_popcount(uint32_t mask)
{
    uint8_t count = 0U;
    uint8_t bit;

    for (bit = 0U; bit < CW_TOTAL_MAX; bit++)
    {
        if ((mask & (uint32_t)(1U << bit)) != 0U)
        {
            count++;
        }
    }
    return count;
}

/* --------------------------------------------------------------------
 * fc_command_configure_descriptor_internal
 *   功能：为命令配置一个 (plane, cw) 描述符（分配 desc+buffer，填 PAA/config/meta）。
 *   参数：controller —— FC 控制器；command —— 命令；plane —— 目标 plane；
 *         cw —— 目标 CW 号；configured_descriptor —— 输出。
 *   返回：FC_STATUS_OK / RESOURCE_EMPTY（池空）/ INVALID_ARGUMENT。
 *   实现要点：
 *     - multi_plane>0：PAA 用 command->paa 的 block/page/lun + 传入的 plane/cw 组装。
 *     - 单 plane：保留 command->paa 的 plane/block/page/lun，只改写 CW 位（plane 参数不用）。
 *     - CW 号由调用方给出（来自 mask 枚举），不再由 desc 序号推导——见
 *       fc_command_configure_descriptors_internal 的"链走位 vs CW 号解耦"说明。
 *     - meta = lba_to_cw_meta(command->meta, cw)。
 * -------------------------------------------------------------------- */
static fc_status_t fc_command_configure_descriptor_internal(fc_t *controller, fc_command_t *command, uint8_t plane, uint8_t cw, fc_descriptor_t **configured_descriptor)
{
    fc_descriptor_t *descriptor;
    fc_buffer_id_t   buffer_id;

    /* 唯一调用者 fc_command_configure_descriptors_internal 传的是栈上 &descriptor */
    ASSERT(configured_descriptor != NULL);
    /* 单 plane 命令的 CW 数不超过一页；多 plane 由 mask 保证 */
    ASSERT(cw < CW_NUM);
    descriptor = fc_descriptor_alloc_internal(controller, command);
    buffer_id  = fc_buffer_alloc_internal(controller);
    if ((descriptor == NULL) || (buffer_id == FC_BUFFER_ID_INVALID))
    {
        if (buffer_id != FC_BUFFER_ID_INVALID)
        {
            (void)fc_buffer_release_internal(controller, buffer_id);
        }
        return FC_STATUS_RESOURCE_EMPTY;
    }

    if (command->config.multi_plane > 0U)
    {
        uint32_t blk    = paa_to_block(command->paa);
        uint32_t pg     = paa_to_page(command->paa);
        /* 保留 command->paa 的 LUN 字段（多 LUN 并发必需：desc PAA/META/随机种子/ECC 都依赖它） */
        descriptor->paa = paa_make(pg, plane, blk, paa_to_lun(command->paa), cw);
    }
    else
    {
        descriptor->paa = (command->paa & ~NAND_FIELD_MASK(CW_WIDTH)) | cw;
    }
    descriptor->config     = command->config;
    descriptor->config.cw  = cw;
    descriptor->buffer_id  = buffer_id;
    descriptor->next_id    = FC_DESCRIPTOR_ID_INVALID;
    descriptor->meta       = lba_to_cw_meta(command->meta, cw);
    *configured_descriptor = descriptor;
    return FC_STATUS_OK;
}

/* --------------------------------------------------------------------
 * fc_command_configure_descriptors_internal
 *   功能：为命令配置整条 desc 链（每个待访问 CW 一个描述符）。
 *   参数：controller —— FC 控制器；command —— 命令。
 *   返回：FC_STATUS_OK / RESOURCE_EMPTY（池空，已回滚）。
 *   实现要点：
 *     - NO_DATA 操作（RESET/LOCK 等）与 ERASE 无数据，
 *       不需要描述符；SET/GET_FEATURES 用内联 param.bytes 代替。
 *     - CW 集合由 command->param.cw_mask 决定，掩码是 **flat 绝对编号**
 *       （bit n = plane*CW_NUM + cw，覆盖 CW_TOTAL_MAX = 8 位）；mask 为 0 时
 *       退化为"对命令的每个 plane 取连续 0..config.cw"（现有调用点的行为，零回归）。
 *     - **config.cw 不回写**：它只是 mask==0 时的输入提示，configure 之后没有任何
 *       下游读者（长度一律走 desc 链）。见 fc_command.h「链长度即真值」。
 *     - 链序：plane-major（先 plane 0 的全部置位 CW，再 plane 1）。
 *       **链走位（desc 序号）与 CW 号自此解耦**：所有按链走位索引的地方不动，
 *       CW 号一律从 paa_to_cw(desc->paa) 取。
 *     - 前驱 desc 的 next_id 串联；首 desc 记入 command->desc_id。
 *     - 任一 desc 配置失败 → fc_command_resources_rollback 整体回滚。
 * -------------------------------------------------------------------- */
fc_status_t fc_command_configure_descriptors_internal(fc_t *controller, fc_command_t *command)
{
    fc_command_id_t  command_id;
    fc_descriptor_t *previous = NULL;
    uint32_t         mask;
    uint32_t         allowed;
    uint8_t          plane_count;
    uint8_t          plane_first;
    uint8_t          plane_last;
    uint8_t          plane;
    uint8_t          cw;

    /* NO_DATA operations (RESET, BLOCK_LOCK, etc.) and ERASE have no
     * data payload — they do not need descriptors or buffers.
     * SET/GET_FEATURES use inline param.bytes + feature_addr instead. */
    if ((nand_operation_has_data((uint8_t)command->config.operation) == 0U) ||
        (command->config.operation == OP_SET_FEATURES) ||
        (command->config.operation == OP_GET_FEATURES))
    {
        command->desc_id = FC_DESCRIPTOR_ID_INVALID;
        return FC_STATUS_OK;
    }

    plane_count = (command->config.multi_plane > 0U) ? (uint8_t)command->config.multi_plane : 1U;
    /* ⚠ 上界写死 PLANE_NUM：desc PAA 的 plane 位与 cws[PLANE_NUM][CW_NUM] 都按 PLANE_NUM 定尺。
     *   改 plane 语义时这里与 nand_dispatch_tables.c 的 multi-plane handler 要一起动。 */
    ASSERT(plane_count <= PLANE_NUM);
    if (plane_count > PLANE_NUM)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    /* 命令能触及的平面位段 [plane_first, plane_last)：
     *   multi-plane：从 plane 0 起，覆盖 plane_count 个 plane；
     *   单 plane：**只覆盖 paa_to_plane(command->paa) 那一个**——掩码是绝对编号，
     *     plane 1 的单 plane 命令掩码是 0xF0 而不是 0x0F。 */
    plane_first = (plane_count == 1U) ? (uint8_t)paa_to_plane(command->paa) : 0U;
    plane_last  = (uint8_t)(plane_first + plane_count);
    allowed     = ((uint32_t)((1U << (plane_count * CW_NUM)) - 1U)) << (plane_first * CW_NUM);

    mask = command->param.cw_mask;
    if (mask == 0U)
    {
        /* 未指定 → 对命令的**每个** plane 取连续 0..config.cw。
         * ⚠ 必须逐 plane 平移到该 plane 的绝对位段：照抄单 plane 的
         *   (1 << (config.cw+1)) - 1 只会点亮 plane 0，多 plane 命令会静默退化。 */
        uint32_t per_plane = (uint32_t)((1U << (command->config.cw + 1U)) - 1U);

        for (plane = plane_first; plane < plane_last; plane++)
        {
            mask |= per_plane << (plane * CW_NUM);
        }
    }
    else if ((mask & ~allowed) != 0U)
    {
        /* 掩码越出本命令的 plane 位段（单 plane：不得点亮别的 plane 的位） */
        return FC_STATUS_INVALID_ARGUMENT;
    }
    ASSERT(fc_cw_mask_popcount(mask) > 0U);

    command_id = (fc_command_id_t)(command - controller->command_pool);
    for (plane = plane_first; plane < plane_last; plane++)
    {
        for (cw = 0U; cw < CW_NUM; cw++)
        {
            fc_descriptor_t *descriptor = NULL;
            fc_status_t      result;

            if ((mask & (uint32_t)(1U << (plane * CW_NUM + cw))) == 0U)
            {
                continue; /* 该 CW 不在本次访问集合内 */
            }
            result = fc_command_configure_descriptor_internal(controller, command, plane, cw, &descriptor);
            if (result != FC_STATUS_OK)
            {
                fc_command_resources_rollback(controller, command_id);
                command->desc_id = FC_DESCRIPTOR_ID_INVALID;
                return result;
            }
            if (previous == NULL)
            {
                command->desc_id = (fc_descriptor_id_t)(descriptor - controller->descriptor_pool);
            }
            else
            {
                previous->config.chain = 1U;
                previous->next_id      = (fc_descriptor_id_t)(descriptor - controller->descriptor_pool);
            }
            previous = descriptor;
        }
    }
    return FC_STATUS_OK;
}

/* ══════════════════════════════════════════════════════════
 * 6. 提交
 * ══════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * fc_submit_to_queue
 *   功能：命令 ID 入队（SQ 或 HSQ），状态置 QUEUED。
 *   参数：controller —— FC 控制器；queue —— 目标队列；command —— 命令。
 *   返回：FC_STATUS_OK / QUEUE_FULL。
 * -------------------------------------------------------------------- */
static fc_status_t fc_submit_to_queue(fc_t *controller, fc_submission_queue_t *queue, fc_command_t *command)
{
    fc_command_id_t command_id;

    command_id = (fc_command_id_t)(command - controller->command_pool);
    if (fc_submission_queue_push(queue, command_id) == FC_STATUS_OK)
    {
        controller->command_state[command_id] = FC_COMMAND_STATE_QUEUED;
        return FC_STATUS_OK;
    }
    return FC_STATUS_QUEUE_FULL;
}

/* --------------------------------------------------------------------
 * fc_submit_internal
 *   功能：按优先级把命令投递到 HSQ（high_priority）或 SQ。
 *   参数：controller —— FC 控制器；command —— 命令。
 *   返回：FC_STATUS_OK / QUEUE_FULL。
 *   实现要点：投递后刷新寄存器状态位。
 * -------------------------------------------------------------------- */
fc_status_t fc_submit_internal(fc_t *controller, fc_command_t *command)
{
    fc_status_t result;

    result = fc_submit_to_queue(controller, (command->high_priority != 0U) ? &controller->hsq : &controller->sq, command);
    fc_register_queue_state(controller);
    return result;
}

/* ══════════════════════════════════════════════════════════
 * 7. 完成
 * ══════════════════════════════════════════════════════════ */

/* 完成项入 CQ，命令状态置 COMPLETED（供 fc_complete_from_nand 使用） */
fc_status_t fc_complete_internal(fc_t *controller, fc_command_status_t *completion)
{
    fc_status_t     result;
    fc_command_id_t command_id;

    command_id = completion->command_id;
    result     = fc_completion_queue_push(&controller->cq, completion);
    if (result == FC_STATUS_OK)
    {
        controller->command_state[command_id] = FC_COMMAND_STATE_COMPLETED;
    }
    fc_register_queue_state(controller);
    return result;
}

/* --------------------------------------------------------------------
 * fc_complete_from_nand
 *   功能：NAND 完成 → 回填 descriptor 状态/ECC → 数据路径 → 组装 CQ 完成项。
 *   参数：controller —— FC 控制器；nand_completion —— NAND 层完成。
 *   返回：FC_STATUS_OK / IO_ERROR（CQ 满强制 COMPLETED 防泄漏）。
 *   实现要点：
 *     - Step 1：把 nand_completion 的 per-CW status/ecc_bits 写回 desc。
 *     - Step 2：Page Read 命令做 data path（ECC 解码 + 条件解扰）。
 *     - Step 3：组装完成项——page read 从 desc 读 ECC 解码后状态
 *       （聚合 status + 前 4 CW ECC + 首个 UNC 的 desc_id），
 *       非 page read 直接透传 NAND 状态。
 *     - CQ 满 → 强制 COMPLETED（数据路径已跑，CQ entry 丢失是 fatal）。
 * -------------------------------------------------------------------- */
static fc_status_t fc_complete_from_nand(fc_t *controller, const nand_dispatch_completion_t *nand_completion)
{
    fc_command_status_t completion;
    fc_command_t       *command;

    /* 完成项由 nand_dispatch 侧产出，command_id 必在池范围内 */
    ASSERT(nand_completion != NULL);
    ASSERT(nand_completion->command_id < FC_COMMAND_POOL_DEPTH);

    command = &controller->command_pool[nand_completion->command_id];

    /* ── Step 1: Write back per-CW status and ECC count from NAND completion ──
     * 链长度即 CW 数：cw_status[] 的下标是**链走位**（plane-major），与链一一对应。 */
    if (command->desc_id != FC_DESCRIPTOR_ID_INVALID)
    {
        fc_descriptor_id_t desc_id = (fc_descriptor_id_t)command->desc_id;
        uint8_t            slot    = 0U;

        while ((desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (slot < CW_TOTAL_MAX))
        {
            fc_descriptor_t *descriptor = &controller->descriptor_pool[desc_id];

            descriptor->status   = nand_completion->cw_status[slot];
            descriptor->ecc_bits = nand_completion->ecc_bit_count[slot];
            desc_id              = (fc_descriptor_id_t)descriptor->next_id;
            slot++;
        }
    }

    /* ── Step 2: Data path processing (ECC decode + conditional descramble) ──
     * 仅对 Page Read 命令执行。READ_ID / GET_FEATURES 等命令
     * 返回的数据不是 ECC 编码的 NAND 页数据，不应进入 Data Path。 */
    if (fc_is_page_read((uint8_t)command->config.operation) != 0U)
    {
        (void)fc_data_path_process_read(command);
        /* 即使 process_read 返回 IO_ERROR（有 CW 不可纠），
         * 已完成 ECC 解码的 desc->status 已更新。
         * 后续 completion entry 将从 descriptor 读取最终 status。 */
    }

    /* ── Step 3: Populate completion entry ──
     * CQ 只带命令级聚合：status + 返回段 param。per-CW ECC 只留在 desc 链，
     * 聚合 ECC 走 fc_get_cmd_ecc_max()（须在 fc_command_release() 前调用）。 */
    completion.command_id    = nand_completion->command_id;
    completion.desc_id       = nand_completion->failed_descriptor_id;
    completion.module        = command->module;
    completion.status        = 0U;
    completion.param.cw_mask = 0U; /* 联合体整体清零（4 字节） */

    if ((command->desc_id != FC_DESCRIPTOR_ID_INVALID) &&
        (fc_is_page_read((uint8_t)command->config.operation) != 0U))
    {
        /* 走 desc 链聚合：status 全链 OR；失败掩码按 **flat 绝对编号** 置位 */
        fc_descriptor_id_t did       = (fc_descriptor_id_t)command->desc_id;
        uint8_t            found_unc = 0U;

        while (did < FC_DESCRIPTOR_POOL_DEPTH)
        {
            const fc_descriptor_t *d = &controller->descriptor_pool[did];

            completion.status |= d->status;
            /* 更新 desc_id 指向 ECC 解码后第一个 UNC 的 desc */
            if ((found_unc == 0U) && (d->status == FC_CW_STATUS_UNC))
            {
                completion.desc_id = did;
                found_unc          = 1U;
            }
            /* 失败 CW 掩码：bit = plane*CW_NUM + cw（与请求掩码同编号空间，
             * 可直接做"请求集 & 失败集"求最小重读集） */
            if ((d->status & (FC_CW_STATUS_UNC | FC_CW_STATUS_CRC_ERROR)) != 0U)
            {
                completion.param.cw_mask |=
                    (uint32_t)(1U << ((uint8_t)paa_to_plane(d->paa) * CW_NUM + (uint8_t)paa_to_cw(d->paa)));
            }
            did = (fc_descriptor_id_t)d->next_id;
        }
    }
    else
    {
        /* 非 page read：直接透传 NAND completion 状态（无 per-CW 解码，掩码恒 0）。
         * 未用到的槽位由 nand_dispatch_completion_init() 预置 FC_CW_STATUS_UNUSED(=0)。 */
        uint8_t slot;

        for (slot = 0U; slot < CW_TOTAL_MAX; slot++)
        {
            completion.status |= nand_completion->cw_status[slot];
        }

        /* GET_FEATURES：引擎把寄存器值写进控制器侧暂存，拷进 CQ 返回段。
         * ⚠ SQ 命令体提交后不再被写（见 fc_command.h「SQ 不可变」）。 */
        if (command->config.operation == OP_GET_FEATURES)
        {
            uint8_t i;

            for (i = 0U; i < 4U; i++)
            {
                completion.param.bytes[i] = g_fc_return_staging[nand_completion->command_id][i];
            }
        }
    }

    if (fc_complete_internal(controller, &completion) != FC_STATUS_OK)
    {
        /* CQ 满：强制标记 COMPLETED 防止命令泄漏。
         * 数据路径处理（ECC 解码等）和 descriptor 状态已更新，
         * 但 CQ entry 丢失，FTL 无法通过 fc_poll_completion 感知。
         * 这是 fatal 级系统背压错误，需要上层干预。 */
        controller->command_state[nand_completion->command_id] = FC_COMMAND_STATE_COMPLETED;
        return FC_STATUS_IO_ERROR;
    }
    return FC_STATUS_OK;
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

/* --------------------------------------------------------------------
 * fc_stripe_release_parity
 *   功能：回收 parity 命令占用的所有资源（desc 链 + buffer + command 槽位）。
 *   参数：controller —— FC 控制器；parity_id —— parity 命令 ID。
 *   返回：无。
 *   实现要点：在 NAND completion 路径中调用（parity 数据已完成使命），
 *         把 desc/buffer 挂回空闲链，command 槽位置 FREE 回池。
 * -------------------------------------------------------------------- */
static void fc_stripe_release_parity(fc_t *controller, fc_command_id_t parity_id)
{
    uint16_t descriptor_index;
    uint16_t next_descriptor_index;

    /* 层内 helper：唯一调用者是完成路径的 stripe 清理，parity_id 取自
     * stripe_parity_for_data[]（注册时即 fc_command_alloc() 铸出的池内 ID）。 */
    ASSERT(parity_id < FC_COMMAND_POOL_DEPTH);

    /* 释放 parity 命令的 desc 链 + buffer */
    descriptor_index = controller->command_descriptor_head[parity_id];
    while (descriptor_index != UINT16_MAX)
    {
        next_descriptor_index = controller->descriptor_owned_next[descriptor_index];

        /* 释放 buffer */
        fc_buffer_id_t buf_id = controller->descriptor_pool[descriptor_index].buffer_id;
        if (buf_id != FC_BUFFER_ID_INVALID)
        {
            (void)fc_buffer_release_internal(controller, buf_id);
        }

        /* 释放 descriptor */
        controller->descriptor_pool[descriptor_index]       = (fc_descriptor_t){0};
        controller->descriptor_owner[descriptor_index]      = FC_COMMAND_ID_INVALID;
        controller->descriptor_owned_next[descriptor_index] = UINT16_MAX;
        controller->descriptor_free_next[descriptor_index]  = controller->descriptor_free_head;
        controller->descriptor_free_head                    = descriptor_index;

        descriptor_index = next_descriptor_index;
    }
    controller->command_descriptor_head[parity_id] = UINT16_MAX;

    /* 返回 command slot 到空闲池 */
    controller->command_pool[parity_id]      = (fc_command_t){0};
    controller->command_state[parity_id]     = FC_COMMAND_STATE_FREE;
    controller->command_free_next[parity_id] = controller->command_free_head;
    controller->command_free_head            = parity_id;
}

/*
 * 处理 raid=ENCODE 命令的 Stripe 提交路径：
 *  ① 分配 parity 命令（command + desc 链 + buffer）
 *  ② 配置 parity 命令的 OP 码和 PAA（plane = PLANE_NUM-1）
 *  ③ fc_data_path_generate_stripe_parity() — XOR + ECC 编码 parity
 *  ④ 构建 Multi-Plane dispatch request（cws[0]=data, cws[1]=parity）
 *  ⑤ nand_dispatch_submit()
 *
 * 成功时 parity 命令由 completion 路径自动释放。
 * 失败时已分配资源全部回滚 + 数据命令以 error completion 返回。
 *
 * 仅处理 PLANE_NUM=2 场景（单数据命令 + 单 parity 命令）。
 */
static fc_status_t fc_stripe_submit(fc_t *controller, fc_command_t *data_cmd, fc_command_id_t data_cmd_id)
{
    fc_command_t           *parity_cmd;
    fc_command_id_t         parity_id;
    nand_dispatch_request_t mp_req;
    uint8_t                 cw_count;
    uint8_t                 cw;
    fc_descriptor_id_t      desc_id;
    const fc_command_t     *data_ptrs[FC_STRIPE_DATA_MAX];

    cw_count = (uint8_t)(data_cmd->config.cw + 1U);

    /* RAID/stripe 的 XOR 按 CW 配对：两个 plane 必须访问**同一组 CW**，
     * flat 掩码（plane 不对称）与 stripe 语义不兼容，直接断言拒绝。
     * mask == 0（未指定）视为对称。 */
    ASSERT(fc_cw_mask_is_plane_symmetric(data_cmd->param.cw_mask));

    /* ① 分配 parity command */
    parity_cmd = fc_command_alloc_internal(controller);
    if (parity_cmd == NULL)
    {
        goto stripe_error;
    }
    parity_id = parity_cmd->command_id;

    /* ② 配置 parity command */
    parity_cmd->config           = data_cmd->config;
    parity_cmd->config.raid      = FC_RAID_MODE_NORMAL;         /* parity 命令不需要 RAID */
    parity_cmd->config.operation = OP_PROGRAM_PAGE_MULTI_PLANE; /* 提交时走 Multi-Plane */
    parity_cmd->high_priority    = data_cmd->high_priority;
    parity_cmd->module           = data_cmd->module;
    parity_cmd->meta             = data_cmd->meta;

    /* 调整 PAA：same page/block/lun/cw，plane = PLANE_NUM-1 */
    {
        uint32_t page   = paa_to_page(data_cmd->paa);
        uint32_t block  = paa_to_block(data_cmd->paa);
        uint32_t plun   = paa_to_lun(data_cmd->paa);
        parity_cmd->paa = paa_make(page, (uint32_t)(PLANE_NUM - 1U), block, plun, 0U);
    }

    /* ③ 分配 parity desc + buffer */
    if (fc_command_configure_descriptors_internal(controller, parity_cmd) != FC_STATUS_OK)
    {
        /* release parity command slot (no desc/buffer allocated yet, but alloc_internal consumed slot) */
        controller->command_pool[parity_id]      = (fc_command_t){0};
        controller->command_state[parity_id]     = FC_COMMAND_STATE_FREE;
        controller->command_free_next[parity_id] = controller->command_free_head;
        controller->command_free_head            = parity_id;
        goto stripe_error;
    }

    /* 写入 parity descriptors 的 meta（与数据命令对应 CW 一致） */
    {
        fc_descriptor_id_t data_desc_id   = (fc_descriptor_id_t)data_cmd->desc_id;
        fc_descriptor_id_t parity_desc_id = (fc_descriptor_id_t)parity_cmd->desc_id;

        for (cw = 0U; cw < cw_count; cw++)
        {
            fc_descriptor_t *pdesc = fc_descriptor_id_to_descriptor(parity_desc_id);
            fc_descriptor_t *ddesc = fc_descriptor_id_to_descriptor(data_desc_id);
            if ((pdesc != NULL) && (ddesc != NULL))
            {
                pdesc->meta = ddesc->meta;
            }
            parity_desc_id = (pdesc != NULL) ? (fc_descriptor_id_t)pdesc->next_id : FC_DESCRIPTOR_ID_INVALID;
            data_desc_id   = (ddesc != NULL) ? (fc_descriptor_id_t)ddesc->next_id : FC_DESCRIPTOR_ID_INVALID;
        }
    }

    /* ④ fc_data_path_generate_stripe_parity() — XOR + ECC 编码 parity */
    data_ptrs[0U] = data_cmd;
    if (fc_data_path_generate_stripe_parity(data_ptrs, 1U, parity_cmd) != FC_STATUS_OK)
    {
        fc_stripe_release_parity(controller, parity_id);
        goto stripe_error;
    }

    /* ⑤ 构建 Multi-Plane dispatch request */
    mp_req             = (nand_dispatch_request_t){0};
    mp_req.command_id  = (uint16_t)data_cmd_id; /* 完成路由回数据命令 */
    mp_req.operation   = OP_PROGRAM_PAGE_MULTI_PLANE;
    mp_req.ch          = (uint8_t)data_cmd->config.ch;
    mp_req.ce          = (uint8_t)data_cmd->config.ce;
    mp_req.lun         = (uint8_t)data_cmd->config.lun;
    mp_req.plane_count = PLANE_NUM;
    mp_req.plane       = 0U; /* multi-plane：起始 plane 恒 0（uop 硬编码 row[0]/row[1]） */
    mp_req.page_type   = (uint8_t)data_cmd->config.page_type;
    /* 两 plane 各 cw_count 个 CW（stripe 命令 plane 对称，见上方 ASSERT） */
    for (uint8_t p = 0U; p < PLANE_NUM; p++)
    {
        mp_req.cw_count[p] = cw_count;
    }

    /* 填充 cws[0] = 数据命令 CW PAA + buffer */
    desc_id = (fc_descriptor_id_t)data_cmd->desc_id;
    for (cw = 0U; cw < cw_count; cw++)
    {
        const fc_descriptor_t *desc = (desc_id < FC_DESCRIPTOR_POOL_DEPTH)
                                          ? &controller->descriptor_pool[desc_id]
                                          : NULL;

        if (desc == NULL)
        {
            fc_stripe_release_parity(controller, parity_id);
            goto stripe_error;
        }
        mp_req.cws[0U][cw].paa    = desc->paa;
        mp_req.cws[0U][cw].buffer = (desc->buffer_id != FC_BUFFER_ID_INVALID)
                                        ? fc_buffer_get_base(desc->buffer_id)
                                        : NULL;
        desc_id                   = (fc_descriptor_id_t)desc->next_id;
    }

    /* 填充 cws[1] = parity 命令 CW PAA + buffer */
    desc_id = (fc_descriptor_id_t)parity_cmd->desc_id;
    for (cw = 0U; cw < cw_count; cw++)
    {
        const fc_descriptor_t *desc = (desc_id < FC_DESCRIPTOR_POOL_DEPTH)
                                          ? &controller->descriptor_pool[desc_id]
                                          : NULL;

        if (desc == NULL)
        {
            fc_stripe_release_parity(controller, parity_id);
            goto stripe_error;
        }
        mp_req.cws[1U][cw].paa    = desc->paa;
        mp_req.cws[1U][cw].buffer = (desc->buffer_id != FC_BUFFER_ID_INVALID)
                                        ? fc_buffer_get_base(desc->buffer_id)
                                        : NULL;
        desc_id                   = (fc_descriptor_id_t)desc->next_id;
    }

    /* 注册 parity 命令到数据命令的清理映射表 */
    controller->stripe_parity_for_data[data_cmd_id] = parity_id;

    /* ⑥ nand_dispatch_submit() */
    if (nand_dispatch_submit(&mp_req) != NAND_RESULT_OK)
    {
        controller->stripe_parity_for_data[data_cmd_id] = FC_COMMAND_ID_INVALID;
        fc_stripe_release_parity(controller, parity_id);
        goto stripe_error;
    }

    return FC_STATUS_OK;

stripe_error:
    /* 数据命令注 error completion，让 FTL 感知失败 */
    {
        nand_dispatch_completion_t fail_comp = (nand_dispatch_completion_t){0};
        fail_comp.command_id                 = (uint16_t)data_cmd_id;
        fail_comp.execution_status           = NAND_RESULT_NOT_READY;
        fail_comp.cw_status[0]               = FC_CW_STATUS_NAND_FAIL;
        (void)fc_complete_from_nand(controller, &fail_comp);
    }
    return FC_STATUS_IO_ERROR;
}

#endif /* CONFIG_FC_RAID */

/* ══════════════════════════════════════════════════════════
 * 9. 调度 / 轮询
 *
 * 服务主循环 fc_poll_internal 的两条提交路径拆成独立 helper：
 *   fc_poll_dispatch_single — 单命令（count==1）
 *   fc_poll_dispatch_chain  — 链式（count>=2）
 * 两者共用 fc_build_dispatch_request 构建 request；
 * 失败兜底统一走 fc_poll_fail_command。
 *
 * 行为不对称（既有，勿统一）：
 *   - META/PAA 预填（fc_fill_cw_meta_paa）只在单命令路径。
 *   - 链式路径 desc 链断裂时从循环体内直接返回，放弃整条链。
 * ══════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * fc_poll_fail_command
 *   功能：以 NAND_FAIL 合成完成项，把命令注入完成队列（poll 内失败兜底）。
 *   参数：controller —— FC 控制器；command_id —— 失败命令。
 *   返回：无。
 *   实现要点：request 构建失败 / submit 失败共用；execution_status 用
 *         NOT_READY 表示"未能提交"，cw_status[0] 置 FC_CW_STATUS_NAND_FAIL。
 * -------------------------------------------------------------------- */
static void fc_poll_fail_command(fc_t *controller, fc_command_id_t command_id)
{
    nand_dispatch_completion_t fail_comp = (nand_dispatch_completion_t){0};

    fail_comp.command_id       = (uint16_t)command_id;
    fail_comp.execution_status = NAND_RESULT_NOT_READY;
    fail_comp.cw_status[0]     = FC_CW_STATUS_NAND_FAIL;
    (void)fc_complete_from_nand(controller, &fail_comp);
}

/* --------------------------------------------------------------------
 * fc_fill_cw_meta_paa
 *   功能：encode 前把每个 CW 的 PAA 写入其 buffer 的 META 字段 [0..3]。
 *   参数：controller —— FC 控制器；command —— page write 命令。
 *   返回：无。
 *   实现要点：沿 desc 链遍历全部 CW（链长度是 CW 数的唯一真值）；仅单命令路径
 *         调用（链式路径不做此预填，为既有行为差异，勿统一）。
 * -------------------------------------------------------------------- */
static void fc_fill_cw_meta_paa(fc_t *controller, fc_command_t *command)
{
    fc_descriptor_id_t mid = (fc_descriptor_id_t)command->desc_id;

    while (mid < FC_DESCRIPTOR_POOL_DEPTH)
    {
        fc_descriptor_t *md = &controller->descriptor_pool[mid];

        if (md->buffer_id != FC_BUFFER_ID_INVALID)
        {
            uint8_t   *cw_base  = fc_buffer_id_to_real_buffer_internal(controller, md->buffer_id);
            nand_paa_t meta_paa = md->paa;
            cw_base[0]          = (uint8_t)(meta_paa >> 0U);
            cw_base[1]          = (uint8_t)(meta_paa >> 8U);
            cw_base[2]          = (uint8_t)(meta_paa >> 16U);
            cw_base[3]          = (uint8_t)(meta_paa >> 24U);
        }
        mid = (fc_descriptor_id_t)md->next_id;
    }
}

/* --------------------------------------------------------------------
 * fc_build_dispatch_request
 *   功能：由一条已 ACTIVE 的命令构建自包含的 NAND dispatch 请求。
 *   参数：controller —— FC 控制器；command —— 命令；
 *         command_id —— 命令 ID；request —— 输出请求。
 *   返回：FC_STATUS_OK；desc 链断裂时该命令注 NAND_FAIL 完成并返回
 *         FC_STATUS_IO_ERROR。
 *   实现要点：
 *     - 填 ch/ce/lun/page_type/feature_addr，再按操作类型填 cw_count[plane] 与 cws[][]：
 *         * ERASE_BLOCK：单 cws[plane][0]（plane = paa_to_plane(command->paa)），无 buffer；
 *         * ERASE_BLOCK_MULTI_PLANE：两 plane 各一个 cws，无 buffer；
 *         * SET/GET_FEATURES：cws[plane][0].buffer 指向控制器侧暂存 g_fc_return_staging
 *           （plane 恒 0，feature 寄存器是 die 级、无 plane 语义）；
 *         * 其余带数据命令：沿 desc 链落位 cws[plane][slot]，slot 按 plane 内递增。
 *     - **cws 第一维恒为物理 plane 索引**（不是"第几组槽位"）：单 plane 命令只填
 *       cws[request->plane][*]，multi-plane 填 cws[0][*]/cws[1][*]（request->plane 恒 0）。
 *       未写的行由上面的 {0} 初始化保证为确定性零值。行地址始终由 paa_to_nand_row()
 *       从 PAA 值算出，与数组下标无关——故本次落位规则是行为中性的。
 *     - **cw_count 是每-plane 数组**：flat CW 掩码下两 plane 的 CW 数可以不同、甚至为 0
 *       （如只选 plane 1）。数据命令由链落位循环按 plane 计数得出，不由 config.cw 推导。
 *     - page read/write 用 CW 基址（ECC 处理完整 528B），非 page 用 USER 区。
 *     - 末尾做 Phase V splittable → ISSUE 转换（仅 LUN_NUM>1）。
 *   注意：本函数不做 META/PAA 预填——那是单命令路径独有的步骤，
 *         见 fc_fill_cw_meta_paa。
 * -------------------------------------------------------------------- */
static fc_status_t fc_build_dispatch_request(fc_t *controller, fc_command_t *command, fc_command_id_t command_id, nand_dispatch_request_t *request)
{
    fc_descriptor_id_t desc_id;

    /* Pre-resolve descriptor-chain data into a self-contained dispatch request. */
    *request              = (nand_dispatch_request_t){0};
    request->command_id   = (uint16_t)command_id;
    request->operation    = (uint8_t)command->config.operation;
    request->ch           = (uint8_t)command->config.ch;
    request->ce           = (uint8_t)command->config.ce;
    request->lun          = (uint8_t)command->config.lun;
    request->page_type    = (uint8_t)command->config.page_type;
    request->feature_addr = (uint8_t)command->feature_addr;

    if (command->config.operation == OP_ERASE_BLOCK)
    {
        /* 单 plane 擦除：目标 plane 取自 command->paa（本分支无 desc 可用）。
         * 行地址由 paa_to_nand_row() 从 PAA 值算出，与数组下标无关。 */
        request->plane                          = (uint8_t)paa_to_plane(command->paa);
        request->cw_count[request->plane]       = 1U;
        request->cws[request->plane][0U].paa    = command->paa;
        request->cws[request->plane][0U].buffer = NULL;
    }
    else if (command->config.operation == OP_ERASE_BLOCK_MULTI_PLANE)
    {
        uint8_t plane;

        request->plane_count = PLANE_NUM;
        request->plane       = 0U; /* multi-plane：起始 plane 恒 0（uop 硬编码 row[0]/row[1]） */
        for (plane = 0U; plane < PLANE_NUM; plane++)
        {
            request->cw_count[plane]       = 1U;
            request->cws[plane][0U].paa    = paa_make(paa_to_page(command->paa), plane, paa_to_block(command->paa), paa_to_lun(command->paa), 0U);
            request->cws[plane][0U].buffer = NULL;
        }
    }
    else if ((command->config.operation == OP_SET_FEATURES) ||
             (command->config.operation == OP_GET_FEATURES))
    {
        /* Feature 寄存器是 die 级、无 plane 语义，command->paa 从不设置（fc_command_alloc_internal
         * 做 (fc_command_t){0}），故 plane 恒 0。仍按 plane 落位，以保住"单 plane 数据恒在
         * cws[request->plane][*]"这条无例外的规则。
         * 内联数据落**控制器侧暂存**（不再指向 command->param）——SQ 命令体提交后不可写；
         * SET 先把命令里的字节拷进暂存，GET 由引擎写回，完成时再拷进 CQ 的 param。
         * 槽位数沿用旧口径（has_data 时 config.cw+1）：只有槽位 0 承载数据，其余槽位
         * 由 task_load 的"槽位 0 无条件拷"护栏 + 全零值兜住。 */
        request->plane                          = (uint8_t)paa_to_plane(command->paa);
        request->cw_count[request->plane]       = nand_operation_has_data((uint8_t)command->config.operation)
                                                      ? (uint8_t)(command->config.cw + 1U)
                                                      : 0U;
        request->cws[request->plane][0U].buffer = g_fc_return_staging[command_id];
        if (command->config.operation == OP_SET_FEATURES)
        {
            (void)memcpy(g_fc_return_staging[command_id], command->param.bytes, 4U);
        }
        else
        {
            (void)memset(g_fc_return_staging[command_id], 0, 4U);
        }
    }
    else if (nand_operation_has_data((uint8_t)command->config.operation))
    {
        /* 沿 desc 链落位：cws[plane][slot] 的 slot = 该 plane 内已落位的 CW 数，
         * 同时充当 cw_count[plane] 的计数器。**链长度即 CW 数**（不由 config.cw 推导）。
         * ⚠ slot 是**链槽位**（该 plane 内第几个待访问 CW），不是 CW 号——mask 不满时
         *   两者不同（如 mask=0b1011 的 plane 内槽位 0/1/2 对应 CW 0/1/3）。
         *   CW 号由 PAA 携带（paa_to_cw），下游 req_* 与 cw_chain 一律从 PAA 取。 */
        desc_id = (fc_descriptor_id_t)command->desc_id;
        while (desc_id < FC_DESCRIPTOR_POOL_DEPTH)
        {
            const fc_descriptor_t *descriptor = &controller->descriptor_pool[desc_id];
            uint8_t                plane      = (uint8_t)paa_to_plane(descriptor->paa);
            uint8_t                slot;

            ASSERT(plane < PLANE_NUM);
            slot = request->cw_count[plane];
            ASSERT(slot < CW_NUM);

            request->cws[plane][slot].paa = descriptor->paa;
            if (descriptor->buffer_id != FC_BUFFER_ID_INVALID)
            {
                /* Page read/write 需要 CW 基址（ECC 在完整 528B CW 上处理）；
                 * 非 page 命令数据直接写入 USER 区域，test/FTL 通过
                 * fc_buffer_id_to_real_buffer 直接读取。 */
                request->cws[plane][slot].buffer =
                    (fc_is_page_read((uint8_t)command->config.operation) != 0U) ||
                            (fc_is_page_write((uint8_t)command->config.operation) != 0U)
                        ? fc_buffer_get_base(descriptor->buffer_id)
                        : fc_buffer_id_to_real_buffer(descriptor->buffer_id);
            }
            else
            {
                request->cws[plane][slot].buffer = NULL;
            }
            request->cw_count[plane] = (uint8_t)(slot + 1U);
            desc_id                  = (fc_descriptor_id_t)descriptor->next_id;
        }

        if (command->config.multi_plane > 0U)
        {
            uint8_t plane;

            request->plane_count = (uint8_t)command->config.multi_plane;
            request->plane       = 0U; /* multi-plane：起始 plane 恒 0（uop 硬编码 row[0]/row[1]） */
            /* ⚠ flat 掩码下某 plane 可以是 **0 个 CW**（如只读 plane 1 = 0xF0），但多 plane
             *   加载/编程的行地址取自 cw_paa[plane][0]（uop 硬编码 row[0]/row[1]）——空 plane
             *   不显式填 PAA 会退化成 block0/page0，静默误操作。故对命令 plane 集合里的
             *   **每个** plane 都补一条 CW=0 的 PAA（行地址只用 page/block/lun，CW 位被
             *   paa_to_nand_row() 丢掉，故 CW=0 不影响取数）。 */
            for (plane = 0U; plane < request->plane_count; plane++)
            {
                if (request->cw_count[plane] == 0U)
                {
                    request->cws[plane][0U].paa    = paa_make(paa_to_page(command->paa), plane, paa_to_block(command->paa), paa_to_lun(command->paa), 0U);
                    request->cws[plane][0U].buffer = NULL;
                }
            }
        }
        else
        {
            /* Single-plane：目标 plane 取自 command->paa，数据落位 cws[plane][*]（不再恒为 [0]）。
             * 不变式：本命令所有 desc 的 plane 位相同——fc_command_configure_descriptor_internal()
             * 单 plane 分支只改 CW 位（descriptor->paa = (command->paa & ~CW_MASK) | cw），保留
             * plane 位，故一次取值即对全部 desc 成立；下面的 ASSERT 把该不变式显式化。 */
            request->plane = (uint8_t)paa_to_plane(command->paa);
            ASSERT(request->cw_count[request->plane] == fc_command_descriptor_count(controller, command));
        }
    }
    else
    {
        /* 其余无数据命令（RESET / BLOCK_LOCK / READ_STATUS 等）：cw_count 保持全 0 */
    }

    /* ── 读命令统一为「加载 + 逐 CW 取数」──
     * FC 层命令保持 WITH_DATA_OUT 语义（desc 建链、fc_is_page_read、数据路径、
     * 完成项判定都用它），但下发给 NAND 的是对应的「只加载、不输出」op：
     * 整页先搬进 NAND 页缓冲，数据随后由 nand_dispatch_task_cw_chain 用
     * 05h（单 plane）/ 06h（多 plane）按 mask 逐 CW 取回（见 nand_dispatch.c）。
     * 这样任意 CW 子集（含不含 CW0）走同一条路径。
     * ⚠ LUN_NUM>1 时本转换会让这两个 op 不再命中下面的 Phase V splittable 拆分
     *   （ISSUE/WAIT 多 LUN 重叠随之失效）；启用多 LUN 前需要把 LOAD/FETCH 拆分
     *   与逐 CW 取数模型重新对齐。当前 LUN_NUM=1，该拆分本就不生效。 */
    if (request->operation == OP_NORMAL_READ_PAGE_WITH_DATA_OUT)
    {
        request->operation = OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT;
    }
    else if (request->operation == OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT)
    {
        request->operation = OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT;
    }

    /* Phase V: splittable 原子命令自动拆为 ISSUE（仅 LUN_NUM>1 时生效；
     * 拆分后由 dispatch Phase 机异步接续 WAIT/FETCH）。单命令与链式路径共用。 */
    if ((LUN_NUM > 1U) && nand_operation_is_splittable(request->operation))
    {
        request->operation = nand_operation_to_issue(request->operation);
    }
    return FC_STATUS_OK;
}

/* --------------------------------------------------------------------
 * fc_poll_dispatch_single
 *   功能：提交调度器选出的单条命令（Data Path encode → submit）。
 *   参数：controller —— FC 控制器；chain —— 调度器链（count==1）。
 *   返回：FC_STATUS_OK / IO_ERROR / fc_stripe_submit 结果。
 *   实现要点：
 *     - page write 先 fc_fill_cw_meta_paa + fc_data_path_encode。
 *     - raid=ENCODE 的 page write 走 fc_stripe_submit（Multi-Plane）。
 *     - 其余构建 request → nand_dispatch_submit；失败注 NAND_FAIL 完成。
 *     - 只负责占用目标 LUN；完成由后续 fc_service() 推进。
 *     - **运行时唯一活提交路径**（FTL 恒 chain=END，count 恒 1）。
 * -------------------------------------------------------------------- */
static fc_status_t fc_poll_dispatch_single(fc_t *controller, const fc_scheduler_chain_t *chain)
{
    fc_command_id_t         command_id = chain->ids[0U];
    fc_command_t           *command;
    nand_dispatch_request_t request;

    if ((fc_command_id_is_valid(command_id) == 0U) ||
        (controller->command_state[command_id] != FC_COMMAND_STATE_QUEUED))
    {
        /* 与拆分前一致：此校验失败早退，不刷新寄存器 */
        return FC_STATUS_IO_ERROR;
    }
    controller->command_state[command_id] = FC_COMMAND_STATE_ACTIVE;
    command                               = &controller->command_pool[command_id];

    /* ── Data Path encode: Random 加扰 → ECC 编码（RAID XOR 已移至 stripe 路径）──
     * 在 NAND 编程前对 buffer 数据做编码，对 FTL 完全透明。 */
    if (fc_is_page_write((uint8_t)command->config.operation) != 0U)
    {
        fc_fill_cw_meta_paa(controller, command);
        (void)fc_data_path_encode(command);
    }

    /* ── Stripe RAID 分支：raid=ENCODE 的 page write 走 Multi-Plane 提交 ── */
#if CONFIG_FC_RAID
    if ((fc_is_page_write((uint8_t)command->config.operation) != 0U) &&
        (command->config.raid == FC_RAID_MODE_ENCODE))
    {
        fc_status_t stripe_result = fc_stripe_submit(controller, command, command_id);
        fc_register_queue_state(controller);
        return stripe_result; /* 成功/失败均已在 fc_stripe_submit 中处理 */
    }
#endif /* CONFIG_FC_RAID */

    if (fc_build_dispatch_request(controller, command, command_id, &request) != FC_STATUS_OK)
    {
        fc_register_queue_state(controller);
        return FC_STATUS_IO_ERROR;
    }

    const uint8_t submit_result = nand_dispatch_submit(&request);

    /* 填充 pass（fc_poll_fill）已按 nand_dispatch_lun_free() 预留槽位，
     * 此处不应再遇到 NOT_READY；其余失败（设备未就绪等）才是真错误。 */
    ASSERT(submit_result != NAND_RESULT_NOT_READY);
    if (submit_result != NAND_RESULT_OK)
    {
        LOGW("fc: nand_dispatch_submit failed for op=%u cmd_id=%u cw_count=%u plane_count=%u\r\n",
             (unsigned)command->config.operation,
             (unsigned)command_id,
             (unsigned)request.cw_count,
             (unsigned)request.plane_count);
        fc_poll_fail_command(controller, command_id);
        fc_register_queue_state(controller);
        return FC_STATUS_IO_ERROR;
    }

    /* Submission only occupies the target LUN. Completion is advanced by a later fc_service() call. */
    fc_register_queue_state(controller);
    return FC_STATUS_OK;
}

/* --------------------------------------------------------------------
 * fc_poll_dispatch_chain
 *   功能：提交调度器选出的链式命令（逐条 encode + 构建 → submit_chain）。
 *   参数：controller —— FC 控制器；chain —— 调度器链（count>=2）。
 *   返回：FC_STATUS_OK / IO_ERROR。
 *   实现要点：
 *     - 逐条：置 ACTIVE → page write encode（不做 META/PAA 预填，
 *       与单命令路径的既有差异，勿统一）→ 构建 requests[i]。
 *     - 全部构建完成后一次 nand_dispatch_submit_chain。
 *     - 任一步失败 → 对应/整链命令注 NAND_FAIL 完成并返回 IO_ERROR。
 *     - 运行时不会命中：FTL 恒 chain=END（count==1），本路径仅单测覆盖。
 * -------------------------------------------------------------------- */
static fc_status_t fc_poll_dispatch_chain(fc_t *controller, const fc_scheduler_chain_t *chain)
{
    uint8_t                 chain_count = chain->count;
    nand_dispatch_request_t reqs[FC_SCHEDULER_CHAIN_MAX];
    uint8_t                 i;

    for (i = 0U; i < chain_count; i++)
    {
        fc_command_id_t command_id = chain->ids[i];
        fc_command_t   *command;

        if ((fc_command_id_is_valid(command_id) == 0U) ||
            (controller->command_state[command_id] != FC_COMMAND_STATE_QUEUED))
        {
            /* 与拆分前一致：此校验失败早退，不刷新寄存器 */
            return FC_STATUS_IO_ERROR;
        }
        controller->command_state[command_id] = FC_COMMAND_STATE_ACTIVE;
        command                               = &controller->command_pool[command_id];

        /* ── Data Path encode: Random 加扰 → ECC 编码 → RAID XOR ──
         * 在 NAND 编程前对 buffer 数据做编码，对 FTL 完全透明。 */
        if (fc_is_page_write((uint8_t)command->config.operation) != 0U)
        {
            (void)fc_data_path_encode(command);
        }

        if (fc_build_dispatch_request(controller, command, command_id, &reqs[i]) != FC_STATUS_OK)
        {
            fc_register_queue_state(controller);
            return FC_STATUS_IO_ERROR;
        }
    }

    const uint8_t submit_result = nand_dispatch_submit_chain(reqs, chain_count);

    /* 填充 pass 已按 nand_dispatch_lun_free() 预留整链槽位，此处不应再遇到 NOT_READY。 */
    ASSERT(submit_result != NAND_RESULT_NOT_READY);
    if (submit_result != NAND_RESULT_OK)
    {
        for (i = 0U; i < chain_count; i++)
        {
            fc_poll_fail_command(controller, chain->ids[i]);
        }
        fc_register_queue_state(controller);
        return FC_STATUS_IO_ERROR;
    }

    /* Submission only occupies the target LUN. Completion is advanced by a later fc_service() call. */
    fc_register_queue_state(controller);
    return FC_STATUS_OK;
}

/* --------------------------------------------------------------------
 * fc_poll_chain_length
 *   功能：计算 SQ 队头命令所属链的长度（链头已弹出，peek 从队头看后续）。
 *   参数：sq —— 普通提交队列（队头已是链头之后的下一条）；head —— 已弹出的链头命令。
 *   返回：链长（1..FC_SCHEDULER_CHAIN_MAX）。
 *   实现要点：
 *     - head->config.chain == FC_CHAIN_END → 单命令，返回 1。
 *     - 否则 peek 后续，收集同一 (ch,ce,lun) 的连续命令，直到 chain==END 或链满。
 *     - 只 peek 不消耗——真正弹出在 fc_poll_fill 里，链放不下时可原样留在队列。
 * -------------------------------------------------------------------- */
static uint8_t fc_poll_chain_length(const fc_submission_queue_t *sq, const fc_command_t *head)
{
    uint8_t need = 1U;

    ASSERT(sq != NULL);
    ASSERT(head != NULL);

    if (head->config.chain == FC_CHAIN_END)
    {
        return 1U;
    }

    while (need < FC_SCHEDULER_CHAIN_MAX)
    {
        fc_command_id_t     next_id = fc_submission_queue_peek(sq, (uint16_t)(need - 1U));
        const fc_command_t *next;

        if (next_id == FC_COMMAND_ID_INVALID)
        {
            break; /* 队列尾 */
        }
        next = fc_command_id_to_command(next_id);
        ASSERT(next != NULL);

        /* 连续性校验：必须同一 target（链成员同 LUN，由 FTL 保证） */
        if ((next->config.ch != head->config.ch) || (next->config.ce != head->config.ce) || (next->config.lun != head->config.lun))
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

/* --------------------------------------------------------------------
 * fc_poll_fill
 *   功能：一次填充 pass——扫描 HSQ/SQ，把可下发的命令（含链）提交进 NAND per-LUN FIFO。
 *   参数：controller —— FC 控制器。
 *   返回：FC_STATUS_OK（本轮至少提交了一条）/ IO_ERROR（提交失败，剩余命令留在队列）/
 *         QUEUE_EMPTY / NOT_READY（无可提交，供 ftl_service 结束本轮 fc_service 循环）。
 *   实现要点：
 *     - **HSQ 优先**：先扫 HSQ（HSQ 命令不参与链，逐条），再扫 SQ。
 *     - **按 LUN 保序**：命令 need = 链长；nand_dispatch_lun_free(lun) >= need 才取，
 *       否则该 LUN 本轮挂起（suspended），后续同 LUN 命令一律跳过。
 *     - **一次扫描**：按本轮快照数量扫描（HSQ/SQ 各一份），扫完即止。
 *     - **顺序保持**：取不走的命令 pop 后回推队尾——扫完整轮等价于一次旋转，
 *       未取出命令的相对顺序不变（同 LUN 的命令因 LUN 挂起必然一起回推，顺序不破）。
 *       链成员连续且逐个回推，链头永远排在成员之前，链序不破。
 *     - 提交失败即中止本轮，剩余命令仍在队列里（未被取出），下轮重试。
 * -------------------------------------------------------------------- */
static fc_status_t fc_poll_fill(fc_t *controller)
{
    fc_scheduler_t *scheduler = &controller->scheduler;
    uint8_t         suspended[CH_NUM][CE_NUM][LUN_NUM];
    uint8_t         dispatched = 0U;

    memset(suspended, 0, sizeof(suspended));

    /* ── HSQ 优先：HSQ 命令不参与链，逐条下发 ── */
    {
        uint16_t total = scheduler->hsq->count;
        uint16_t k;

        for (k = 0U; k < total; k++)
        {
            fc_command_id_t      id;
            fc_command_t        *cmd;
            fc_scheduler_chain_t chain;
            uint8_t              ch;
            uint8_t              ce;
            uint8_t              lun;

            (void)fc_submission_queue_pop(scheduler->hsq, &id);
            cmd = fc_command_id_to_command(id);
            ASSERT(cmd != NULL);

            ch  = (uint8_t)cmd->config.ch;
            ce  = (uint8_t)cmd->config.ce;
            lun = (uint8_t)cmd->config.lun;
            /* 路由字段越界是编程错误：值由 FTL 从 PBA 经 pba_to_ch/ce/lun 灌入，
             * PBA 已过 pba_is_valid() 校验。此处必须断言而非防御——suspended[] 按
             * [ch][ce][lun] 直接索引，放行即栈越界写；且越界命令会因
             * nand_dispatch_lun_free()==0 被永久回推（静默活锁），比当场停机更难定位。 */
            ASSERT((ch < CH_NUM) && (ce < CE_NUM) && (lun < LUN_NUM));
            if ((suspended[ch][ce][lun] != 0U) || (nand_dispatch_lun_free(ch, ce, lun) < 1U))
            {
                /* 该 LUN 本轮已挂起或 FIFO 满：挂起并原样回推 */
                suspended[ch][ce][lun] = 1U;
                (void)fc_submission_queue_push(scheduler->hsq, id);
                continue;
            }

            chain.count   = 1U;
            chain.ids[0U] = id;
            if (fc_poll_dispatch_single(controller, &chain) != FC_STATUS_OK)
            {
                return FC_STATUS_IO_ERROR;
            }
            dispatched++;
        }
    }

    /* ── SQ：按 LUN 保序，链整条取出 ── */
    {
        uint16_t total = scheduler->sq->count;
        uint16_t k;

        for (k = 0U; k < total; k++)
        {
            fc_command_id_t      id;
            fc_command_t        *cmd;
            fc_scheduler_chain_t chain;
            uint8_t              ch;
            uint8_t              ce;
            uint8_t              lun;
            uint8_t              need;
            uint8_t              j;

            (void)fc_submission_queue_pop(scheduler->sq, &id);
            cmd = fc_command_id_to_command(id);
            ASSERT(cmd != NULL);

            ch  = (uint8_t)cmd->config.ch;
            ce  = (uint8_t)cmd->config.ce;
            lun = (uint8_t)cmd->config.lun;
            /* 同 HSQ 分支：路由字段越界是编程错误，断言而非防御（栈越界写 / 静默活锁） */
            ASSERT((ch < CH_NUM) && (ce < CE_NUM) && (lun < LUN_NUM));
            if (suspended[ch][ce][lun] != 0U)
            {
                (void)fc_submission_queue_push(scheduler->sq, id);
                continue;
            }

            need = fc_poll_chain_length(scheduler->sq, cmd);
            if (nand_dispatch_lun_free(ch, ce, lun) < need)
            {
                /* 剩余槽放不下整条链：本轮挂起该 LUN，链头回推（成员后续轮次一并回推） */
                suspended[ch][ce][lun] = 1U;
                (void)fc_submission_queue_push(scheduler->sq, id);
                continue;
            }

            chain.count   = need;
            chain.ids[0U] = id;
            for (j = 1U; j < need; j++)
            {
                (void)fc_submission_queue_pop(scheduler->sq, &chain.ids[j]);
            }
            k = (uint16_t)(k + need - 1U); /* 跳过已取出的链成员 */

            if (((need == 1U) ? fc_poll_dispatch_single(controller, &chain) : fc_poll_dispatch_chain(controller, &chain)) != FC_STATUS_OK)
            {
                return FC_STATUS_IO_ERROR;
            }
            dispatched++;
        }
    }

    if (dispatched != 0U)
    {
        return FC_STATUS_OK;
    }
    return ((scheduler->sq->count == 0U) && (scheduler->hsq->count == 0U)) ? FC_STATUS_QUEUE_EMPTY : FC_STATUS_NOT_READY;
}

/* --------------------------------------------------------------------
 * fc_poll_internal
 *   功能：FC 服务主循环单步——推进 NAND + 取命令提交。
 *   参数：controller —— FC 控制器。
 *   返回：FC_STATUS_OK / IO_ERROR / QUEUE_EMPTY / NOT_READY。
 *   实现要点：
 *     - 先 nand_dispatch_poll() 推进 NAND；有完成 → fc_complete_from_nand
 *       （并清理 stripe parity），OK 直接返回（一次调用处理一条完成）。
 *     - 无完成 → fc_poll_fill()：一次扫描把能放下的命令（含链）提交进 NAND
 *       per-LUN FIFO（按 LUN 保序、HSQ 优先、room 判定）。
 *     - 队列状态寄存器由两个 dispatch helper **逐点刷新**（fc_register_queue_state
 *       幂等，但早退路径不刷，见 helpers 内注释），非在本函数统一刷新。
 *   运行时事实（勿被 §16/图 11 误导）：FTL 所有命令 config.chain=FC_CHAIN_END，
 *   运行时 count 恒为 1 → **只有 fc_poll_dispatch_single 是活路径**；
 *   fc_poll_dispatch_chain 仅单元测试覆盖。详见设计文档 §23.8。
 *   调用方：ftl_service()（ftl.c）/ ftl_internal_sync_wait() / main.c 的裸 fc_service。
 * -------------------------------------------------------------------- */
fc_status_t fc_poll_internal(fc_t *controller)
{
    fc_status_t                result;
    nand_dispatch_completion_t nand_completion;

    const uint8_t nand_result = nand_dispatch_poll(&nand_completion);

    if (nand_result != NAND_RESULT_NOT_READY)
    {
        if (fc_complete_from_nand(controller, &nand_completion) != FC_STATUS_OK)
        {
            return FC_STATUS_IO_ERROR;
        }

        /* ── Stripe parity 清理 ──
         * 如果完成的命令是 stripe 数据命令，释放内部 parity 命令资源。 */
#if CONFIG_FC_RAID
        {
            fc_command_id_t completed_id = (fc_command_id_t)nand_completion.command_id;
            /* command_id 是 FC 投递给 NAND 的池内 ID，fc_complete_from_nand 已断言过范围 */
            ASSERT(completed_id < FC_COMMAND_POOL_DEPTH);
            if (controller->stripe_parity_for_data[completed_id] != FC_COMMAND_ID_INVALID)
            {
                fc_command_id_t parity_id                        = controller->stripe_parity_for_data[completed_id];
                controller->stripe_parity_for_data[completed_id] = FC_COMMAND_ID_INVALID;
                fc_stripe_release_parity(controller, parity_id);
            }
        }
#endif /* CONFIG_FC_RAID */

        fc_register_queue_state(controller);
        return (nand_result == NAND_RESULT_OK) ? FC_STATUS_OK : FC_STATUS_IO_ERROR;
    }

    /* 无完成 → 填充 pass：一次扫描把可下发命令（含链）提交进 NAND per-LUN FIFO。 */
    result = fc_poll_fill(controller);
    fc_register_queue_state(controller);
    return result;
}

/* ══════════════════════════════════════════════════════════
 * 10. CQ 出口
 * ══════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * fc_poll_completion_internal
 *   功能：从 CQ 取一条完成项（FTL 侧消费）。
 *   参数：controller —— FC 控制器；completion —— 输出。
 *   返回：FC_STATUS_OK / QUEUE_EMPTY。
 * -------------------------------------------------------------------- */
fc_status_t fc_poll_completion_internal(fc_t *controller, fc_command_status_t *completion)
{
    fc_status_t result;

    result = fc_completion_queue_pop(&controller->cq, completion);
    fc_register_queue_state(controller);
    return result;
}

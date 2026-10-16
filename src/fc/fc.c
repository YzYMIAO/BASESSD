/*
 * fc.c — FC 核心实现（命令池 / 描述符池 / 缓冲池 / 调度 / 服务主循环）
 *
 * 资源模型（三池，全部静态固定，无动态内存）：
 *   - Command Pool（64）：fc_command_t，单条命令
 *   - Descriptor Pool（256）：fc_descriptor_t，一个 CW 一个描述符，next_id 串联
 *   - Buffer Pool（128）：528B CW 缓冲，DMA 直访（noncacheable 段）
 *
 * 生命周期（对 FTL）：
 *   fc_command_alloc() → 填 config/参数 → 逐条 fc_command_configure_descriptor() 组装 desc 链
 *   → fc_submit() → fc_service()（后台推进）
 *   → fc_poll_completion() 取完成 → fc_command_release() 回收。
 *   （掩码建链与数据搬运在 FTL 侧：ftl_cmd_configure_descriptors / ftl_cmd_write_data）
 *
 * 文件结构（按职责分区，函数定义顺序即依赖顺序，无前向声明）：
 *    1. 静态资源        三池 + 链表数组 + g_fc_controller
 *    2. 查询/转换小工具 is_page_read/write、getter、ID→指针、buffer 工具
 *    3. 初始化          fc_init_internal
 *    4. 资源分配/回滚   command/descriptor/buffer alloc、rollback、release
 *                       （descriptor_alloc 经 fc_internal.h 供建链入口调用）
 *    5. desc 链原语     fc_command_chain_append（供 fc_interface.c 的建链入口调用）
 *    6. 提交            fc_submit_to_queue、fc_submit_internal
 *    7. 完成            fc_complete_internal、fc_complete_from_nand
 *    8. Stripe RAID     fc_stripe_release_parity、fc_stripe_submit
 *    9. 调度/轮询       fc_poll_* 辅助 + fc_poll_internal
 *   10. CQ 出口         fc_poll_completion_internal、fc_poll_completions_internal
 *
 * 服务主循环（fc_poll_internal）：
 *   ① nand_dispatch_poll() 推进 NAND 层全部 LUN，每完成经 sink 当场 fc_complete_from_nand()
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
 *     descriptor_owner[desc] —— desc 归属的命令 ID（O(1) 反查）
 *     command_state / command_free_next —— 命令状态 + 空闲链表
 *     descriptor_free_next —— desc 空闲链表
 *     buffer_free_next / buffer_state —— buffer 空闲链表 + 占用状态
 *
 *   ⚠ 「一条命令拥有哪些 desc」**只有一份真值**：命令的 desc 链
 *     （command->desc_id + descriptor_pool[].next_id）。v15 之前另有一条
 *     「所有权链」（descriptor_owned_next[] + command_descriptor_head[]）为
 *     「desc 已分配但尚未挂进数据链」的中间态兜底，已整条删除（640B）。
 *
 *   ⚠ **desc 出池即挂链**：fc_command_configure_descriptor（fc_interface.c）分配 desc 后
 *     先把字段填好、立刻 append，再分配 buffer。于是「已分配 desc 全部在链上」
 *     恒成立，失败时沿链释放即完备——不需要任何「链外半成品」的回滚逻辑
 *     （v15 曾用就地回滚打补丁，v16 改为消除中间态本身）。代价：链上允许存在
 *     buffer_id == FC_BUFFER_ID_INVALID 的 desc（buffer 池 128 < desc 池 256，
 *     这是最现实的失败点），所有走链的读者都要容忍它。
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
static uint16_t            g_fc_buffer_free_next[FC_BUFFER_POOL_DEPTH];
static uint8_t             g_fc_buffer_state[FC_BUFFER_POOL_DEPTH];

/* GET/SET_FEATURES 的 4 字节内联数据暂存，按 command_id（= 命令池槽位号）索引。
 * 为什么需要：SQ 命令提交后不可写（见 fc_command.h「SQ 不可变」），而引擎需要一个
 * 稳定的 4 字节落点写回寄存器值；返回值唯一的归宿是 CQ 的 param 联合体，
 * 故这里只做提交→完成之间的中转。索引用的 command_id 是「指针 − command_pool 基址」
 * 派生出来的池内下标，恒 < FC_COMMAND_POOL_DEPTH。 */
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
        case OP_RANDOM_DATA_READ_MULTI_PLANE: /* 06h-E0h 多 plane 逐 CW 取数（与 05h 对称） */
        case OP_READ_PAGE_FETCH:              /* Phase V */
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
/* 三个状态读取器：ID 恒由池派生（fc_command_to_id）或循环上界保证在范围内，
 * 越界即不变量破坏——原先静默回退默认值（v21 改断言），尤其 FC_COMMAND_STATE_FREE
 * 会把"没有这个槽位"说成"槽位空闲可用"。与同文件 fc_command_id_to_command_internal 同法。 */
uint8_t fc_command_state_get(const fc_t *controller, fc_command_id_t command_id)
{
    ASSERT(command_id < FC_COMMAND_POOL_DEPTH);
    return controller->command_state[command_id];
}
fc_command_id_t fc_descriptor_owner_get(const fc_t *controller, uint16_t descriptor_id)
{
    ASSERT(descriptor_id < FC_DESCRIPTOR_POOL_DEPTH);
    return controller->descriptor_owner[descriptor_id];
}
uint8_t fc_buffer_state_get(const fc_t *controller, fc_buffer_id_t buffer_id)
{
    ASSERT(buffer_id < FC_BUFFER_POOL_DEPTH);
    return controller->buffer_state[buffer_id];
}

/* ── 命令/描述符 ID ↔ 指针（范围由公共壳 fc_interface.c 校验） ── */
fc_command_t *fc_command_id_to_command_internal(fc_t *controller, uint16_t command_id)
{
    /* 公共壳 fc_command_id_to_command() 已断言过 command_id < FC_COMMAND_POOL_DEPTH */
    ASSERT(command_id < FC_COMMAND_POOL_DEPTH);
    return &controller->command_pool[command_id];
}

/* 指针 → 池索引。**全仓唯一一份实现**：此前该减法在 fc_descriptor_alloc_internal /
 * fc_command_release_internal_api / fc_submit_to_queue 三处各内联一遍，外加公共壳
 * 里一份 O(256) 线性扫描。
 * 前置条件：command 属于本控制器的 command_pool（公共壳已校验区间）。 */
fc_command_id_t fc_command_to_id_internal(const fc_t *controller, const fc_command_t *command)
{
    ASSERT(controller != NULL);
    ASSERT(command != NULL);
    ASSERT((command >= controller->command_pool) && (command < &controller->command_pool[FC_COMMAND_POOL_DEPTH]));

    return (fc_command_id_t)(command - controller->command_pool);
}

fc_descriptor_t *fc_descriptor_id_to_descriptor_internal(fc_t *controller, uint16_t descriptor_id)
{
    /* 公共壳 fc_descriptor_id_to_descriptor() 已断言过 descriptor_id < FC_DESCRIPTOR_POOL_DEPTH */
    ASSERT(descriptor_id < FC_DESCRIPTOR_POOL_DEPTH);
    return &controller->descriptor_pool[descriptor_id];
}

/* 内部：直接返回 CW 基址（528B，含 META/CRC/PARITY），不做 USER 偏移。
 * buffer_id 恒由池派生、且调用者已先校验占用（公共壳 ASSERT 占用 / 链释放前判
 * != FC_BUFFER_ID_INVALID）——越界即不变量破坏，v21 补断言（同 fc_command_id_to_command_internal）。 */
uint8_t *fc_buffer_id_to_real_buffer_internal(fc_t *controller, fc_buffer_id_t buffer_id)
{
    ASSERT(buffer_id < FC_BUFFER_POOL_DEPTH);
    return controller->buffer_pool[buffer_id];
}

/* 内部：释放 buffer（状态清零 + 挂回空闲链表头） */
fc_status_t fc_buffer_release_internal(fc_t *controller, fc_buffer_id_t buffer_id)
{
    ASSERT(buffer_id < FC_BUFFER_POOL_DEPTH);
    controller->buffer_state[buffer_id]     = 0U;
    controller->buffer_free_next[buffer_id] = controller->buffer_free_head;
    controller->buffer_free_head            = buffer_id;
    return FC_STATUS_OK;
}

/* 命令 desc 链的长度（走链计数，链尾 next_id == FC_DESCRIPTOR_ID_INVALID 终止；无 desc 时 0）。
 * ⚠ **链长度是唯一真值**：flat CW 掩码下 config.cw 已不再等于 CW 数
 *   （见 fc_command.h「链长度即真值」），任何长度/每-plane 计数一律走链。
 * 结束条件是链自己的哨兵（`descriptor_id < FC_DESCRIPTOR_POOL_DEPTH` 即 next_id != INVALID），
 * 不另设计数上界——「链长 ≤ CW_TOTAL_MAX」是结构不变量（掩码位宽决定的），
 * 走链时无需重复它；与 fc_complete_from_nand 等其余走链循环同一写法。 */
static uint8_t fc_command_descriptor_count(const fc_t *controller, const fc_command_t *command)
{
    fc_descriptor_id_t descriptor_id = (fc_descriptor_id_t)command->desc_id;
    uint8_t            count         = 0U;

    while (descriptor_id < FC_DESCRIPTOR_POOL_DEPTH)
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
    *controller                      = (fc_t){0};
    controller->command_pool         = g_fc_command_pool;
    controller->descriptor_pool      = g_fc_descriptor_pool;
    controller->buffer_pool          = g_fc_buffer_pool;
    controller->descriptor_owner     = g_fc_descriptor_owner;
    controller->command_state        = g_fc_command_state;
    controller->command_free_next    = g_fc_command_free_next;
    controller->descriptor_free_next = g_fc_descriptor_free_next;
    controller->buffer_free_next     = g_fc_buffer_free_next;
    controller->buffer_state         = g_fc_buffer_state;

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
     *      buffer_free_next     : 0→1→…→127→UINT16_MAX（head=0） ── */
    for (index = 0U; index < FC_COMMAND_POOL_DEPTH; index++)
    {
        controller->command_free_next[index] = ((index + 1U) < FC_COMMAND_POOL_DEPTH) ? (fc_command_id_t)(index + 1U) : FC_COMMAND_ID_INVALID;
    }
    /* 串接描述符空闲链表 + 清归属表 */
    for (index = 0U; index < FC_DESCRIPTOR_POOL_DEPTH; index++)
    {
        controller->descriptor_owner[index]     = FC_COMMAND_ID_INVALID;
        controller->descriptor_free_next[index] = ((index + 1U) < FC_DESCRIPTOR_POOL_DEPTH) ? (uint16_t)(index + 1U) : UINT16_MAX;
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
 *   实现要点：清零 + desc_id=INVALID（未配置描述符）。**不清 command_id**——
 *         结构体里根本没有这个字段，ID 由指针派生。
 * -------------------------------------------------------------------- */
fc_command_t *fc_command_alloc_internal(fc_t *controller)
{
    fc_command_id_t command_id;

    command_id = controller->command_free_head;
    if (command_id == FC_COMMAND_ID_INVALID)
    {
        return NULL;
    }
    controller->command_free_head                = controller->command_free_next[command_id];
    controller->command_pool[command_id]         = (fc_command_t){0};
    controller->command_pool[command_id].desc_id = FC_DESCRIPTOR_ID_INVALID;
    controller->command_state[command_id]        = FC_COMMAND_STATE_ALLOCATED;
    return &controller->command_pool[command_id];
}

/* --------------------------------------------------------------------
 * fc_descriptor_alloc_internal
 *   功能：从空闲链表分配一个描述符（出池即自洽，但不挂链）。
 *   参数：controller —— FC 控制器；command —— 归属命令。
 *   返回：描述符指针；池空返回 NULL。
 *   实现要点：descriptor_owner[desc]=command_id（O(1) 反查表）。
 *         **不挂任何链**——desc 何时进入命令的数据链由调用者
 *         （fc_command_configure_descriptor，fc_interface.c）决定，本函数只负责出池。
 *         跨文件契约见 fc_internal.h：调用者必须立刻 fc_command_chain_append。
 *         但**出池的 desc 必须自洽**：整体清零会把 next_id/buffer_id 都变成 0
 *         （两个 0 都是合法号——链会走进 desc 0，释放会误放别人的 buffer 0），
 *         故显式置哨兵。
 * -------------------------------------------------------------------- */
fc_descriptor_t *fc_descriptor_alloc_internal(fc_t *controller, fc_command_t *command)
{
    uint16_t        descriptor_index;
    fc_command_id_t command_id;

    command_id       = fc_command_to_id_internal(controller, command);
    descriptor_index = controller->descriptor_free_head;
    if (descriptor_index == UINT16_MAX)
    {
        return NULL;
    }
    controller->descriptor_free_head                        = controller->descriptor_free_next[descriptor_index];
    controller->descriptor_pool[descriptor_index]           = (fc_descriptor_t){0};
    controller->descriptor_pool[descriptor_index].next_id   = FC_DESCRIPTOR_ID_INVALID;
    controller->descriptor_pool[descriptor_index].buffer_id = FC_BUFFER_ID_INVALID;
    controller->descriptor_owner[descriptor_index]          = command_id;
    return &controller->descriptor_pool[descriptor_index];
}

/* --------------------------------------------------------------------
 * fc_descriptor_release_internal
 *   功能：把描述符还回空闲链（清归属、清本体、头插空闲链）。
 *   参数：controller —— FC 控制器；descriptor_index —— 池内下标。
 *   返回：无。
 *   实现要点：**不碰数据链、不碰 buffer**——只把 desc 本体还池。调用者负责
 *         先把该 desc 从链上摘掉（见 fc_descriptor_chain_release_head_internal）。
 * -------------------------------------------------------------------- */
static void fc_descriptor_release_internal(fc_t *controller, uint16_t descriptor_index)
{
    controller->descriptor_pool[descriptor_index]      = (fc_descriptor_t){0};
    controller->descriptor_owner[descriptor_index]     = FC_COMMAND_ID_INVALID;
    controller->descriptor_free_next[descriptor_index] = controller->descriptor_free_head;
    controller->descriptor_free_head                   = descriptor_index;
}

/* --------------------------------------------------------------------
 * fc_descriptor_chain_release_head_internal
 *   功能：释放链头这一个 desc（含其 buffer），*head 前移到后继。
 *   参数：controller —— FC 控制器；head —— 链头 ID 的地址（就地更新）。
 *   返回：无。*head 越界（空链）直接返回。
 *   实现要点：**先读 next_id 再释放**——释放会把 desc 整体清零。
 *         buffer_id == FC_BUFFER_ID_INVALID 是合法态（buffer 池比 desc 池小，
 *         分配失败时 desc 已挂链但无 buffer），跳过 buffer 释放。
 * -------------------------------------------------------------------- */
void fc_descriptor_chain_release_head_internal(fc_t *controller, fc_descriptor_id_t *head)
{
    fc_descriptor_id_t descriptor_id;
    fc_buffer_id_t     buffer_id;

    if (*head >= FC_DESCRIPTOR_POOL_DEPTH)
    {
        return;
    }
    descriptor_id = *head;
    buffer_id     = (fc_buffer_id_t)controller->descriptor_pool[descriptor_id].buffer_id;
    *head         = (fc_descriptor_id_t)controller->descriptor_pool[descriptor_id].next_id;

    if (buffer_id != FC_BUFFER_ID_INVALID)
    {
        (void)fc_buffer_release_internal(controller, buffer_id);
    }
    fc_descriptor_release_internal(controller, descriptor_id);
}

/* --------------------------------------------------------------------
 * fc_descriptor_chain_release_all_internal
 *   功能：沿链释放**这条命令**的全部 desc + buffer，*head 置 FC_DESCRIPTOR_ID_INVALID。
 *         "all" = 本命令的 desc 链（该命令已申请的全部资源），**不是**清空池子——
 *         链即该命令的资源集合，别的命令的 desc 不在链上、不受影响。
 *   参数：controller —— FC 控制器；head —— 链头 ID 的地址（就地清空）。
 *   返回：无。
 *   实现要点：结束条件是链自己的哨兵——每轮 release_head 把 *head 推进到 next_id，
 *         走完链尾后 *head == FC_DESCRIPTOR_ID_INVALID（0xFFFF ≥ 池深），循环自然结束。
 *         与全仓其余走链循环（fc_complete_from_nand / 完成项折叠 / 填 META /
 *         fc_build_request_data_planes）同一写法，**不另设计数上界**（链长 ≤ CW_TOTAL_MAX
 *         是掩码位宽决定的结构不变量，走链时无需重复）。
 *         末尾的 *head 赋值是显式后置条件（保证返回恒为 INVALID），不是循环判据。
 * -------------------------------------------------------------------- */
void fc_descriptor_chain_release_all_internal(fc_t *controller, fc_descriptor_id_t *head)
{
    while (*head < FC_DESCRIPTOR_POOL_DEPTH)
    {
        fc_descriptor_chain_release_head_internal(controller, head);
    }
    *head = FC_DESCRIPTOR_ID_INVALID;
}

/* --------------------------------------------------------------------
 * fc_command_release_buffers_internal
 *   功能：沿命令的 desc 链归还**全部 buffer**，并把各 desc 的 buffer_id 抹成
 *         FC_BUFFER_ID_INVALID（**幂等**：已 INVALID 的跳过）。
 *   参数：controller —— FC 控制器；command —— 命令。
 *   返回：无。
 *   实现要点：只动 buffer，**不动 desc 链、不动命令槽位**——它们仍由
 *         fc_command_release 回收。唯一调用者：fc_complete_from_nand 在
 *         「buf_own 的释放位 = FC 释放（01/11）」时当场归还（此时完成项已组装完毕，
 *         之后没有任何读者碰 buffer）。抹 INVALID 是为了让随后的链释放自然跳过，
 *         从而不双重归还；命令若没走到完成就释放，buffer 仍有效，链释放照常归还
 *         ⇒ 不泄漏。归属语义见 fc_buf_own_t。
 * -------------------------------------------------------------------- */
static void fc_command_release_buffers_internal(fc_t *controller, fc_command_t *command)
{
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)command->desc_id;

    while (desc_id < FC_DESCRIPTOR_POOL_DEPTH)
    {
        fc_descriptor_t *descriptor = &controller->descriptor_pool[desc_id];
        fc_buffer_id_t   buffer_id  = (fc_buffer_id_t)descriptor->buffer_id;

        if (buffer_id != FC_BUFFER_ID_INVALID)
        {
            (void)fc_buffer_release_internal(controller, buffer_id);
            descriptor->buffer_id = FC_BUFFER_ID_INVALID;
        }
        desc_id = (fc_descriptor_id_t)descriptor->next_id;
    }
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

/* fc_command_resources_rollback（只放 desc、不放命令槽的中间态回滚）已删除：
 * 它唯一调用者（fc_stripe_submit 的 parity 建链失败路径）后面紧跟一次
 * fc_command_release_internal_api()，而后者把「desc 链 + buffer + 命令槽位」
 * 一并做完，前者纯属冗余。至此 desc 链释放只有 fc_descriptor_chain_release_all_internal
 * 一份实现（此前共三份：release_internal_api 自己 / resources_rollback /
 * fc_stripe_release_parity）。 */

/* --------------------------------------------------------------------
 * fc_command_release_internal_api
 *   功能：释放命令全部资源（desc 链 + buffer + command 槽位）。
 *   参数：controller —— FC 控制器；command —— 命令。
 *   返回：恒 FC_STATUS_OK。
 *   实现要点：沿命令的 desc 链（command->desc_id + next_id）释放 desc+buffer，
 *         命令槽位置 FREE 回池。**幂等**：已 FREE 的命令直接返回 OK——这一步
 *         必须挡在最前面，因为释放会把命令结构体整体清零，desc_id 归 0
 *         （是个合法 desc 号），重复释放会顺着别人的链把别人的 desc 释放掉。
 *         调用方（公共接口）已保证指针在池内、命令不在 CQ 中。
 * -------------------------------------------------------------------- */
fc_status_t fc_command_release_internal_api(fc_t *controller, fc_command_t *command)
{
    fc_command_id_t    command_id;
    fc_descriptor_id_t head;

    command_id = fc_command_to_id_internal(controller, command);

    if (controller->command_state[command_id] == FC_COMMAND_STATE_FREE)
    {
        return FC_STATUS_OK;
    }

    /* 沿链释放 desc + buffer（buffer 的归还此前一直漏着，v16 补上）。
     * desc_id 是位域取不了地址，故用局部头变量中转——清空后本就不再需要。 */
    head = (fc_descriptor_id_t)command->desc_id;
    fc_descriptor_chain_release_all_internal(controller, &head);

    controller->command_pool[command_id]      = (fc_command_t){0};
    controller->command_state[command_id]     = FC_COMMAND_STATE_FREE;
    controller->command_free_next[command_id] = controller->command_free_head;
    controller->command_free_head             = command_id;
    return FC_STATUS_OK;
}

/* ══════════════════════════════════════════════════════════
 * 5. desc 链原语
 *   建链入口（校验 + 出池 desc/buffer + 填字段 + 挂链的编排）自 v17 起收敛到
 *   fc_interface.c 的 fc_command_configure_descriptor；本节只留池/链原语。
 * ══════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * fc_command_chain_append
 *   功能：把已填好的 descriptor 追加到命令 desc 链的尾部。
 *   参数：controller —— FC 控制器；command —— 命令；descriptor —— 待挂 desc（属本命令）。
 *   返回：无。
 *   实现要点：
 *     - 走 next_id 找链尾（链长 ≤ CW_TOTAL_MAX，O(N) 可接受，且不必让调用方维护 previous）。
 *     - 空链时本条即链头（记入 command->desc_id）；否则前驱 next_id 串上并置 config.chain = MORE。
 *     - 末条保持 FC_CHAIN_END（desc 分配时已整体清零），无需回改。
 *     - **调用者契约（v17 跨文件）**：desc 必须已填好 paa/config/meta/next_id——调用者是
 *       fc_interface.c 的建链入口 fc_command_configure_descriptor，它保证「出池即挂链」，
 *       故链上不存在半成品 desc。
 * -------------------------------------------------------------------- */
void fc_command_chain_append(fc_t *controller, fc_command_t *command, fc_descriptor_t *descriptor)
{
    const fc_descriptor_id_t new_id = (fc_descriptor_id_t)(descriptor - controller->descriptor_pool);
    fc_descriptor_id_t       tail   = (fc_descriptor_id_t)command->desc_id;

    if (tail == FC_DESCRIPTOR_ID_INVALID)
    {
        command->desc_id = new_id; /* 空链：本条即链头 */
        return;
    }
    while (controller->descriptor_pool[tail].next_id != FC_DESCRIPTOR_ID_INVALID)
    {
        tail = (fc_descriptor_id_t)controller->descriptor_pool[tail].next_id;
    }
    controller->descriptor_pool[tail].config.chain = 1U;
    controller->descriptor_pool[tail].next_id      = new_id;
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

    command_id = fc_command_to_id_internal(controller, command);
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
 *       （聚合 status + 前 4 CW ECC + 首个失败 CW 的 desc_id，见 FC_CW_STATUS_ERROR_MASK），
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
     * cw_status[] 的下标是 **flat 绝对 CW 号 n = plane*CW_NUM + cw**（与请求掩码同空间），
     * 故这里按 desc 自己的 PAA 算出 n 直接取，不再沿链数序号。 */
    if (command->desc_id != FC_DESCRIPTOR_ID_INVALID)
    {
        fc_descriptor_id_t desc_id = (fc_descriptor_id_t)command->desc_id;

        while (desc_id < FC_DESCRIPTOR_POOL_DEPTH)
        {
            fc_descriptor_t *descriptor = &controller->descriptor_pool[desc_id];
            uint8_t          n          = (uint8_t)((uint8_t)paa_to_plane(descriptor->paa) * CW_NUM +
                                  (uint8_t)paa_to_cw(descriptor->paa));

            ASSERT(n < CW_TOTAL_MAX);
            descriptor->status   = nand_completion->cw_status[n];
            descriptor->ecc_bits = nand_completion->ecc_bit_count[n];
            desc_id              = (fc_descriptor_id_t)descriptor->next_id;
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
    completion.command_id = nand_completion->command_id;
    completion.desc_id    = nand_completion->failed_descriptor_id;
    completion.op_tag     = command->op_tag;
    completion.status     = 0U;
    completion.param      = (fc_param_t){0}; /* 联合体整体清零（4 字节）；仅 GET_FEATURES / OP_READ_META / OP_READ_ONFI_SIGNATURE / OP_READ_COLUMN / BLOCK_LOCK_STATUS 会再填 */

    if ((command->desc_id != FC_DESCRIPTOR_ID_INVALID) &&
        (fc_is_page_read((uint8_t)command->config.operation) != 0U))
    {
        /* 走 desc 链聚合：status 全链 OR；首个失败 CW 的 desc_id。
         * v28：**不再**聚合失败 CW 位图——上层要定位失败 CW，遍历 desc 链 OR 各 desc->status
         * 即可（desc 链本来就完整留在池里直到 fc_command_release），FC 不必代为派生。 */
        fc_descriptor_id_t did        = (fc_descriptor_id_t)command->desc_id;
        uint8_t            found_fail = 0U;

        while (did < FC_DESCRIPTOR_POOL_DEPTH)
        {
            const fc_descriptor_t *d = &controller->descriptor_pool[did];

            completion.status |= d->status;
            /* 更新 desc_id 指向**首个失败 CW** 的 desc——契约见 fc_command_status_t.desc_id
             * （"first-failing desc on error"）。失败位取 FC_CW_STATUS_ERROR_MASK 全集，
             * 不只 UNC：ECC 纠错成功但 CRC/META 校验失败的 CW 同样是失败 CW（其 status 为
             * READY|CRC_ERROR / READY|MATA_ERROR），此前只认 UNC 会让这类命令的 desc_id
             * 停留在 NAND 层透传值（读成功时即 INVALID），与契约不符。 */
            if ((found_fail == 0U) && ((d->status & FC_CW_STATUS_ERROR_MASK) != 0U))
            {
                completion.desc_id = did;
                found_fail         = 1U;
            }
            did = (fc_descriptor_id_t)d->next_id;
        }
    }
    else
    {
        /* 非 page read：直接透传 NAND completion 状态（无 per-CW 解码）。
         * 未用到的槽位由 nand_dispatch_completion_init() 预置 FC_CW_STATUS_UNUSED(=0)。 */
        uint8_t slot;

        for (slot = 0U; slot < CW_TOTAL_MAX; slot++)
        {
            completion.status |= nand_completion->cw_status[slot];
        }

        /* OP_BLOCK_LOCK_STATUS（v49）：7Ah 的状态字节由引擎**直读 DQ 总线**送进
         * completion->nand_status（NAND_UOP_DATA_OUT_STATUS，见 nand_fmc.c 的
         * nand_fmc_exec_data_out_status），**不经任何数据缓冲** ⇒ 只能从这里取。
         * 字节位定义（MT29F4G08 手册）：I/O2 = Lock#（0 = 锁定，1 = 未锁）、
         * I/O1 = LT#、I/O0 = LT（1 = 紧锁）。 */
        if (command->config.operation == OP_BLOCK_LOCK_STATUS)
        {
            completion.param.bytes[0] = nand_completion->nand_status;
        }
        /* GET_FEATURES / OP_READ_META / OP_READ_ONFI_SIGNATURE / OP_READ_COLUMN：引擎把
         * 寄存器值 / 页内 META / 器件 ONFI 签名 / 按列读的 4 字节写进控制器侧暂存，拷进 CQ
         * 返回段（都用 4 字节暂存 g_fc_return_staging，见 fc_build_dispatch_request）。
         * ⚠ SQ 命令体提交后不再被写（见 fc_command.h「SQ 不可变」）。 */
        else if ((command->config.operation == OP_GET_FEATURES) ||
                 (command->config.operation == OP_READ_META) ||
                 (command->config.operation == OP_READ_ONFI_SIGNATURE) ||
                 (command->config.operation == OP_READ_COLUMN))
        {
            uint8_t i;

            for (i = 0U; i < 4U; i++)
            {
                completion.param.bytes[i] = g_fc_return_staging[nand_completion->command_id][i];
            }
        }
    }

    /* ── Step 4: buffer 归属 = FC 释放（buf_own bit0 = 1，即 01/11）时当场归还 ──
     * 完成项已组装完毕（Step 3 之后没有任何读者碰 buffer），此处归还并把各 desc 的
     * buffer_id 抹成 INVALID ⇒ 随后 fc_command_release 沿链释放时自然跳过，不双重归还。
     * 00/10（上层释放）不在这里动，行为与今天逐字相同。放在 fc_complete_internal 之前，
     * 顺带覆盖 CQ 满强制 COMPLETED 的 fatal 分支。 */
    if ((command->config.buf_own & FC_BUF_OWN_FREE_BY_FC) != 0U)
    {
        fc_command_release_buffers_internal(controller, command);
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
    /* 层内 helper：唯一调用者是完成路径的 stripe 清理，parity_id 取自
     * stripe_parity_for_data[]（注册时即 fc_command_alloc() 铸出的池内 ID）。 */
    ASSERT(parity_id < FC_COMMAND_POOL_DEPTH);

    /* 与 fc_command_release_internal_api 完全同义（desc 链 + buffer + 命令槽位），
     * 原先这里有一份逐行拷贝。 */
    (void)fc_command_release_internal_api(controller, &controller->command_pool[parity_id]);
}

/*
 * 处理 raid=ENCODE 命令的 Stripe 提交路径：
 *  ① 分配 parity 命令（command + desc 链 + buffer）
 *  ② 配置 parity 命令的 OP 码和 PAA（plane = PLANE_NUM-1）
 *  ③ fc_data_path_generate_stripe_parity() — XOR + ECC 编码 parity
 *  ④ 构建 Multi-Plane dispatch request（plane0=data, plane1=parity）
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
     * 掩码由 **desc 链重建**（v13：链是 CW 集合的唯一真值，不回读 FTL 的输入字段）。 */
    ASSERT(fc_cw_bitmap_is_plane_symmetric(fc_get_cmd_cw_bitmap(data_cmd)));

    /* ① 分配 parity command */
    parity_cmd = fc_command_alloc_internal(controller);
    if (parity_cmd == NULL)
    {
        goto stripe_error;
    }
    parity_id = fc_command_to_id_internal(controller, parity_cmd);

    /* ② 配置 parity command */
    parity_cmd->config           = data_cmd->config;
    parity_cmd->config.raid      = FC_RAID_MODE_NORMAL;         /* parity 命令不需要 RAID */
    parity_cmd->config.operation = OP_PROGRAM_PAGE_MULTI_PLANE; /* 提交时走 Multi-Plane */
    /* parity 是 FC 内部命令（buffer 由 FC 申请、FC 释放），不继承 data_cmd 的 buf_own——
     * 否则 data_cmd 若是「上层申请」档，建链入口会因拿不到上层 buffer_id 而断言。 */
    parity_cmd->config.buf_own   = FC_BUF_OWN_FC_ALLOC_FC_FREE;
    parity_cmd->high_priority    = data_cmd->high_priority;
    parity_cmd->op_tag           = data_cmd->op_tag;
    parity_cmd->meta             = data_cmd->meta;

    /* 调整 PAA：same page/block/lun/cw，plane = PLANE_NUM-1。
     * ch/ce 不在 PAA 里——parity 命令的路由取自它自己的 config（上面已从 data_cmd 复制）。 */
    parity_cmd->paa = paa_with_plane(data_cmd->paa, PLANE_NUM - 1U);

    /* ③ 分配 parity desc + buffer
     *    parity 命令是**单 plane**（plane = PLANE_NUM-1），CW 集合 = 连续 0..config.cw。
     *    v13 起「按掩码建链」已上移 FTL，FC 内不再有掩码枚举；RAID 是 FC 内部机制，
     *    v17 起建链只剩公共入口 fc_command_configure_descriptor，这里逐条调它挂链
     *    （parity 恒为连续 0..config.cw，等价于 FTL 侧整段位图 fc_command_plane_span_cw_bitmap）。
     *    ⚠ 该入口固定使用单例 g_fc_controller；RAID 暂未启用（CONFIG_FC_RAID=0），
     *      将来启用多实例前需先把这条路径改回 controller 参数化入口。
     *    行地址：同一 plane 内所有 CW 共用 parity_cmd->paa（page/block/lun）。 */
    {
        const uint8_t    parity_plane = (uint8_t)paa_to_plane(parity_cmd->paa);
        const nand_paa_t parity_paa   = parity_cmd->paa;
        uint8_t          pcw;

        for (pcw = 0U; pcw <= (uint8_t)parity_cmd->config.cw; pcw++)
        {
            if (fc_command_configure_descriptor(parity_cmd, parity_plane, pcw, parity_paa) != FC_STATUS_OK)
            {
                /* 回滚半成品：release_internal_api 一次做完 desc 链 + buffer + 命令槽位
                 * （此前这里是 resources_rollback + 手写槽位回收两条，纯冗余）。 */
                (void)fc_command_release_internal_api(controller, parity_cmd);
                goto stripe_error;
            }
        }
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
    /* 两 plane 的行地址 + 掩码：stripe 命令 plane 对称（见上方 ASSERT），
     * 两 plane 访问同一组 CW → 掩码 = 两组相同的低位。 */
    for (uint8_t p = 0U; p < PLANE_NUM; p++)
    {
        mp_req.plane_paa[p] = paa_with_plane(data_cmd->paa, p);
        for (cw = 0U; cw < cw_count; cw++)
        {
            mp_req.cw_bitmap |= (uint32_t)(1UL << (p * CW_NUM + cw));
        }
    }

    /* 填充 plane 0（= 数据命令）的 CW buffer */
    desc_id = (fc_descriptor_id_t)data_cmd->desc_id;
    for (cw = 0U; cw < cw_count; cw++)
    {
        /* desc_id 由链遍历产生，恒在池内——越界即链损坏 = 调用者 bug，转 ASSERT（v20） */
        ASSERT(desc_id < FC_DESCRIPTOR_POOL_DEPTH);
        const fc_descriptor_t *desc = &controller->descriptor_pool[desc_id];

        mp_req.cw_buffers[cw] = (desc->buffer_id != FC_BUFFER_ID_INVALID)
                                    ? fc_buffer_get_base(desc->buffer_id)
                                    : NULL;
        desc_id               = (fc_descriptor_id_t)desc->next_id;
    }

    /* 填充 plane 1（= parity 命令）的 CW buffer */
    desc_id = (fc_descriptor_id_t)parity_cmd->desc_id;
    for (cw = 0U; cw < cw_count; cw++)
    {
        ASSERT(desc_id < FC_DESCRIPTOR_POOL_DEPTH);
        const fc_descriptor_t *desc = &controller->descriptor_pool[desc_id];

        mp_req.cw_buffers[CW_NUM + cw] = (desc->buffer_id != FC_BUFFER_ID_INVALID)
                                             ? fc_buffer_get_base(desc->buffer_id)
                                             : NULL;
        desc_id                        = (fc_descriptor_id_t)desc->next_id;
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
 *   - 链式路径构建失败时从循环体内直接返回，放弃整条链。
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
 * fc_build_request_data_planes
 *   功能：沿 desc 链把 CW 落位到 request 的 cw_bitmap / cw_buffers[]（带数据命令专用）。
 *   参数：controller —— FC 控制器；command —— 命令；
 *         request —— 输出（header 已填、全零）。
 *   返回：FC_STATUS_OK / FC_STATUS_INVALID_ARGUMENT（multi-plane 窄位图，见下）。
 *   实现要点：
 *     - 每个 desc 落位到 **flat 绝对 CW 号 n = plane*CW_NUM + cw** 那一格：
 *       置 request->cw_bitmap 的 bit n、写 request->cw_buffers[n]。落位顺序无关紧要
 *       （掩码是集合），故不再有"槽位计数器"，也不再有链序 == plane-major 的不变量。
 *     - **链长度即 CW 数**（不由 config.cw 推导）：popcount(cw_bitmap)。
 *     - 行地址不在这里逐 CW 存：它是 per-plane 的，由下面按 plane 集合填 plane_paa[]。
 *       每 plane 的行地址**取自链上该 plane 的第一条 desc**（链即真值，与 cw_bitmap 同源），
 *       而不是从 command->paa 复制——Policy B 下两个 plane 的 block/page 可以不同。
 *     - **唯一的失败出口**：multi-plane 命令的 desc 链没覆盖 plane 段里的每一个 plane
 *       （见分支内注释）。debug 档 ASSERT 停机，release 档返回 INVALID_ARGUMENT 拒绝。
 *     - 只由 fc_build_dispatch_request 的"带数据命令"分支调用。
 * -------------------------------------------------------------------- */
static fc_status_t fc_build_request_data_planes(const fc_t *controller, const fc_command_t *command, nand_dispatch_request_t *request)
{
    fc_descriptor_id_t desc_id          = (fc_descriptor_id_t)command->desc_id;
    uint8_t            planes_with_desc = 0U; /* bit p = plane p 链上有 desc */
    nand_paa_t         plane_row[PLANE_NUM];  /* 各 plane 的行地址（链上第一条 desc） */

    while (desc_id < FC_DESCRIPTOR_POOL_DEPTH)
    {
        const fc_descriptor_t *descriptor = &controller->descriptor_pool[desc_id];
        uint8_t                plane      = (uint8_t)paa_to_plane(descriptor->paa);
        uint8_t                n          = (uint8_t)(plane * CW_NUM + paa_to_cw(descriptor->paa));

        ASSERT(plane < PLANE_NUM);
        ASSERT(n < CW_TOTAL_MAX);
        /* 同一个 CW 被两条 desc 覆盖 = desc 链构造错误（掩码是集合，重复置位会丢数据） */
        ASSERT((request->cw_bitmap & (uint32_t)(1UL << n)) == 0UL);

        if ((planes_with_desc & (uint8_t)(1U << plane)) == 0U)
        {
            /* 该 plane 的行地址取链上第一条 desc（行地址只含 page/block/lun+plane，CW 位丢掉）：
             * paa_page_base 清 CW 位、paa_with_plane 换 plane —— 与逐字段重排逐位等价。 */
            plane_row[plane] = paa_with_plane(paa_page_base(descriptor->paa), plane);
            planes_with_desc |= (uint8_t)(1U << plane);
        }
        else
        {
            /* 同一 plane 的所有 desc 必须指向同一物理位置——一个 plane 一次只访问一个块/页 */
            ASSERT(paa_to_block(descriptor->paa) == paa_to_block(plane_row[plane]));
            ASSERT(paa_to_page(descriptor->paa) == paa_to_page(plane_row[plane]));
            ASSERT(paa_to_lun(descriptor->paa) == paa_to_lun(plane_row[plane]));
        }

        request->cw_bitmap |= (uint32_t)(1UL << n);
        if (descriptor->buffer_id != FC_BUFFER_ID_INVALID)
        {
            /* Page read/write 需要 CW 基址（ECC 在完整 528B CW 上处理）；
             * 非 page 命令数据直接写入 USER 区域，test/FTL 通过
             * fc_buffer_id_to_real_buffer 直接读取。 */
            request->cw_buffers[n] =
                (fc_is_page_read((uint8_t)command->config.operation) != 0U) ||
                        (fc_is_page_write((uint8_t)command->config.operation) != 0U)
                    ? fc_buffer_get_base(descriptor->buffer_id)
                    : fc_buffer_id_to_real_buffer(descriptor->buffer_id);
        }
        else
        {
            request->cw_buffers[n] = NULL;
        }
        desc_id = (fc_descriptor_id_t)descriptor->next_id;
    }

    if (command->config.multi_plane > 0U)
    {
        const uint8_t plane_count = (uint8_t)command->config.multi_plane;
        const uint8_t all_planes  = (uint8_t)((1U << plane_count) - 1U);
        uint8_t       plane;

        request->plane_count = plane_count;
        request->plane       = 0U; /* multi-plane：起始 plane 恒 0（uop 硬编码 row[0]/row[1]） */

        /* 不变式：multi-plane 命令的 desc 链必须覆盖 plane 段 [0, plane_count) 里的
         * **每一个** plane。违反 = 调用者的编程错误，按 §十三 判据一律 ASSERT（不当返回码
         * 替代断言）；但 ASSERT 在 NDEBUG 下被裁空，故下面**再补一道 release 拒绝**。
         *
         * 为什么非炸不可：NAND 侧的多 plane handler **无条件遍历 PLANE_NUM** 下行地址，
         * 既不看掩码也不看 plane_count（nand_dispatch_tables.c 的 req_multi_plane_{program,
         * read,erase}），器件侧多 plane program 会把点名过的 plane **全部编程**。未覆盖的
         * plane 既无数据（cw_buffers[n] 恒 NULL）也无地址，只能凭空造一个——写路径于是
         * **静默**给该 plane 编一页全 0xFF（页缓冲 memset 0xFF、只填掩码内的 CW），
         * 调用者完全不知道。
         *
         * 这条不变式此前只在测试夹具里守着（fc_test_common.c 的 test_cmd_build），生产侧
         * 无守卫——ftl_cmd_resolve_cw_bitmap 只查**越段**，部分位图对整段而言是"合法"的。
         * 生产路径不可达：FTL 的多 plane 入口自己保证位图覆盖段内每一个 plane
         * （ftl_issue_subop / ftl_wbuf_flush 由 cw_bitmap 构造保证）。踩进来 = 调用者直接调
         * ftl_cmd_configure_descriptors(cmd, 部分位图) 配 multi_plane > 0。
         * 想只访问某个 plane ⇒ 用**单 plane op**（multi_plane = 0），别用多 plane op 配窄位图。 */
        ASSERT((planes_with_desc & all_planes) == all_planes);
        /* release 侧**同步拒绝**（ASSERT 在 NDEBUG 下整条裁空）：宁可不发这条命令，也
         * 不能让未覆盖 plane 的回退地址被凭空造出来、静默写进介质。走
         * fc_poll_fail_command 注一条 NAND_FAIL 完成项，FTL 的 pending 照常释放
         * （否则命令永不完成，表现为"挂死"，比报错更难查）。 */
        if ((planes_with_desc & all_planes) != all_planes)
        {
            return FC_STATUS_INVALID_ARGUMENT;
        }

        /* 行地址按 plane 逐个填（行地址只含 page/block/lun + plane 位，CW 位被
         * paa_to_nand_row() 丢掉）：命令 plane 集合里每个 plane 都要填——空 plane 不填
         * 会退化成 block0/page0，静默误操作。上面那条 ASSERT 已保证「每个 plane 都有
         * desc」，故三元恒取 plane_row[plane]；else 支只在 NDEBUG 下可达，是**最后兜底**
         * （回退到 command->paa 的同页地址 = v11 及以前的行为），生产不可达。 */
        for (plane = 0U; plane < plane_count; plane++)
        {
            request->plane_paa[plane] = ((planes_with_desc & (uint8_t)(1U << plane)) != 0U)
                                            ? plane_row[plane]
                                            : paa_with_plane(command->paa, plane);
        }
    }
    else
    {
        /* Single-plane：地址取自 command->paa（ftl_cmd_setup_ppa 单 plane 路径直接写它，
         * GC/WL 的单 plane 写也走这条），desc 链只负责 CW 落位。
         * 不变式：本命令所有 desc 的 plane 位相同、行地址与 command->paa 一致——下面这条
         * ASSERT 把它显式化（有 desc 时链上地址 == command->paa）。
         * 「无重复 n」（掩码位数 == 链长）由 fc_command_configure_descriptor 的 attach 侧
         * ASSERT(fc_command_cw_attached(...)) 在污染点拦下，此处不再复核。 */
        request->plane                     = (uint8_t)paa_to_plane(command->paa);
        request->plane_paa[request->plane] = command->paa;
        ASSERT(((planes_with_desc & (uint8_t)(1U << request->plane)) == 0U) ||
               ((paa_to_block(plane_row[request->plane]) == paa_to_block(command->paa)) &&
                (paa_to_page(plane_row[request->plane]) == paa_to_page(command->paa))));
    }

    return FC_STATUS_OK;
}

/* --------------------------------------------------------------------
 * fc_dispatch_rewrite_operation
 *   功能：下发前对 request->operation 做两处改写（读语义 + Phase V 拆分）。
 *   参数：request —— 已构建完成的 dispatch 请求。
 *   实现要点：
 *     - ⚠ **两处改写有顺序依赖**：读改写必须排在 splittable 判定之前。读改写把
 *       WITH_DATA_OUT 换成 WITHOUT_DATA_OUT，而后者在 g_nand_operation_map 里
 *       op_type=ATOMIC / pair_op=0，于是不再 splittable。当前 LUN_NUM=1 该拆分本就不
 *       生效，但启用多 LUN 前必须把 LOAD/FETCH 拆分与逐 CW 取数模型重新对齐。
 *     - 由 fc_build_dispatch_request 末尾调用；单命令与链式路径共用。
 * -------------------------------------------------------------------- */
static void fc_dispatch_rewrite_operation(nand_dispatch_request_t *request)
{
    /* ── 读命令统一为「加载 + 逐 CW 取数」──
     * FC 层命令保持 WITH_DATA_OUT 语义（desc 建链、fc_is_page_read、数据路径、
     * 完成项判定都用它），但下发给 NAND 的是对应的「只加载、不输出」op：
     * 整页先搬进 NAND 页缓冲，数据随后由 nand_dispatch_task_cw_chain 用
     * 05h（单 plane）/ 06h（多 plane）按 mask 逐 CW 取回（见 nand_dispatch.c）。
     * 这样任意 CW 子集（含不含 CW0）走同一条路径。 */
    if (request->operation == OP_NORMAL_READ_PAGE_WITH_DATA_OUT)
    {
        request->operation = OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT;
    }
    else if (request->operation == OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT)
    {
        request->operation = OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT;
    }

    /* Phase V: splittable 原子命令自动拆为 ISSUE（仅 LUN_NUM>1 时生效；
     * 拆分后由 dispatch Phase 机异步接续 WAIT/FETCH）。 */
    if ((LUN_NUM > 1U) && nand_operation_is_splittable(request->operation))
    {
        request->operation = nand_operation_to_issue(request->operation);
    }
}

/* --------------------------------------------------------------------
 * fc_build_dispatch_request
 *   功能：由一条已 ACTIVE 的命令构建自包含的 NAND dispatch 请求。
 *   参数：controller —— FC 控制器；command —— 命令；
 *         command_id —— 命令 ID；request —— 输出请求。
 *   返回：FC_STATUS_OK / FC_STATUS_INVALID_ARGUMENT（multi-plane 窄位图——**唯一**的
 *         失败出口，由 fc_build_request_data_planes 透传）。其余路径不可失败
 *         （desc 链遍历按池深截断，不产生错误路径）。
 *   实现要点：
 *     - 填 ch/ce/lun/page_type/feature_addr，再按操作类型填 cw_bitmap / cw_buffers[] / plane_paa[]：
 *         * ERASE_BLOCK：只填 plane_paa[本 plane]，**不置掩码**（v28：擦除无逐 CW 数据，
 *           请求位图是合法空集，completion 的空集分支会写 cw_status[0]），无 buffer；
 *         * ERASE_BLOCK_MULTI_PLANE：填 plane_paa[0..1]，同样不置掩码（v28）；
 *         * SET/GET_FEATURES：cw_buffers[plane*CW_NUM] 指向控制器侧暂存 g_fc_return_staging
 *           （plane 恒 0，feature 寄存器是 die 级、无 plane 语义）；
 *         * OP_READ_META：无 desc 链，plane_paa[本 plane] 取自 command->paa（保留 cw 位 →
 *           NAND 侧列地址），cw_buffers[plane*CW_NUM] 指向同一暂存（回读的 4 字节 META）；
 *         * OP_READ_ONFI_SIGNATURE（v59）：无 desc 链、**不填 plane_paa**——列地址恒 0x20、
 *           行地址恒 0，与介质坐标无关；cw_buffers[plane*CW_NUM] 指向同一暂存（4 字节签名）；
 *         * OP_READ_COLUMN（v63）：无 desc 链，plane_paa[本 plane] 取自 command->paa（只提供
 *           行地址，cw 位被丢掉）；**列地址来自命令级 param 字节 0/1（小端）**，落进
 *           request->column；cw_buffers[plane*CW_NUM] 指向同一暂存（4 字节）；
 *         * 块锁族（BLOCK_UNLOCK_LOW/HIGH、BLOCK_LOCK(_TIGHT)、BLOCK_LOCK_STATUS，v49）：
 *           无 desc 链、无 buffer，只填 plane_paa[本 plane]（23h/24h/7Ah 带 3 个地址周期，
 *           行地址必须来自 command->paa；2Ah/2Ch 无地址周期，填了不下发）；
 *         * 其余带数据命令：沿 desc 链落位（见 fc_build_request_data_planes）。
 *     - **索引空间唯一**：掩码 bit n = plane*CW_NUM + cw（flat 绝对编号），cw_buffers[] 同下标；
 *       NAND 层不再有槽位/链序等第二套坐标。未写的格由上面的 {0} 初始化保证为确定性零值。
 *     - **plane_paa[] 是 per-plane 的行地址**，与掩码无关：命令 plane 集合内每个 plane 都填
 *       （哪怕它没有选中任何 CW）。行地址始终由 paa_to_nand_row() 从 PAA 值算出。
 *     - page read/write 用 CW 基址（ECC 处理完整 528B），非 page 用 USER 区。
 *     - 末尾调 fc_dispatch_rewrite_operation()：读语义改写 + Phase V splittable → ISSUE。
 *   注意：本函数不做 META 预填——buffer[0..3] 的 META 由
 *         fc_data_path_encode 的 Stage 0 写（两条派发路径都经 encode）。
 * -------------------------------------------------------------------- */
static fc_status_t fc_build_dispatch_request(fc_t *controller, fc_command_t *command, fc_command_id_t command_id, nand_dispatch_request_t *request)
{
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
         * v28：**不置 cw_bitmap**——擦除无逐 CW 数据，请求位图是合法空集；
         * completion_all_cws 对空集写 cw_status[0]，CQ 聚合照样读得到擦除成败。 */
        request->plane                     = (uint8_t)paa_to_plane(command->paa);
        request->plane_paa[request->plane] = command->paa;
    }
    else if (command->config.operation == OP_ERASE_BLOCK_MULTI_PLANE)
    {
        uint8_t plane;

        request->plane_count = PLANE_NUM;
        request->plane       = 0U; /* multi-plane：起始 plane 恒 0（uop 硬编码 row[0]/row[1]） */
        for (plane = 0U; plane < PLANE_NUM; plane++)
        {
            request->plane_paa[plane] = paa_with_plane(command->paa, plane);
        }
        /* v28：不置 cw_bitmap（同单 plane 擦除）——多 plane 擦除的 plane 集合由
         * plane_count/plane_paa[] 表达，与 CW 位图无关。 */
    }
    else if ((command->config.operation == OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT) ||
             (command->config.operation == OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT))
    {
        /* 无 desc 的「纯加载」op（v45）：只把目标页搬进器件页缓冲，**不建 CW 位图、不占 buffer**
         * —— op 表里 has_data = 0（host 侧无数据搬运），故 ftl_cmd_needs_desc_chain 不要求链。
         * 数据随后由**独立的** op 8（05h-E0h）/ op 41（06h-E0h）命令带 desc 链取回。
         * ⚠ 生产读路径**不走这里**：FC 命令恒为 op 12/30（has_data=1，走下面的 data_planes 分支
         *   建好链与位图），op 33/29 只作为 fc_dispatch_rewrite_operation 的**改写目标**出现。
         *   本分支只服务「命令本身就是 33/29」的直接下发（测试用）。
         * 行地址取自 command->paa（无 desc 可用）：单 plane 取本 plane；多 plane 逐 plane 补 plane 位。 */
        if (command->config.operation == OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT)
        {
            uint8_t plane;

            request->plane_count = PLANE_NUM;
            request->plane       = 0U; /* multi-plane：起始 plane 恒 0（uop 硬编码 row[0]/row[1]） */
            for (plane = 0U; plane < PLANE_NUM; plane++)
            {
                request->plane_paa[plane] = paa_with_plane(command->paa, plane);
            }
        }
        else
        {
            request->plane                     = (uint8_t)paa_to_plane(command->paa);
            request->plane_paa[request->plane] = command->paa;
        }
        /* 不置 cw_bitmap（空集 = 无 CW 可访问）：加载 op 的 uop 无 DATA_OUT，位图/缓冲皆不用。 */
    }
    else if ((command->config.operation == OP_SET_FEATURES) ||
             (command->config.operation == OP_GET_FEATURES))
    {
        /* Feature 寄存器是 die 级、无 plane 语义，command->paa 从不设置（fc_command_alloc_internal
         * 做 (fc_command_t){0}），故 plane 恒 0。仍按 plane 落位，以保住"数据恒在
         * cw_buffers[plane*CW_NUM]"这条无例外的规则。
         * 内联数据落**控制器侧暂存**（不再指向 command->param）——SQ 命令体提交后不可写；
         * SET 先把命令里的字节拷进暂存，GET 由引擎写回，完成时再拷进 CQ 的 param。
         * 掩码只置该 plane 的 CW0 位：Feature 恒为 4 字节单次传输，与 config.cw 无关
         * （旧实现用 config.cw+1 个槽位，但只有槽位 0 承载数据，其余是零值摆设）。 */
        request->plane                               = (uint8_t)paa_to_plane(command->paa);
        request->cw_bitmap                           = (uint32_t)(1UL << (request->plane * CW_NUM));
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
        /* META 只读回读（v46）：无 desc 链（has_data = 0），回读指定页、指定 CW 的前 4 字节
         * META（CW 布局 [META:4][CRC:4][USER:512][PARITY:8]，META 在加扰区前、明文）。
         * CW 由 command->paa 的 cw 字段选（PAA 原生含 cw），NAND 侧据
         * paa_to_cw(plane_paa[plane]) * CW_SIZE 算列地址；行地址取自同一 PAA。
         * 4 字节落**控制器侧暂存**（同 GET_FEATURES，SQ 提交后不可写），完成时拷进 CQ 的 param。
         * 掩码只置该 plane 的 CW0 位（暂存落点恒在 cw_buffers[plane*CW_NUM]）。 */
        request->plane                               = (uint8_t)paa_to_plane(command->paa);
        request->plane_paa[request->plane]           = command->paa;
        request->cw_bitmap                           = (uint32_t)(1UL << (request->plane * CW_NUM));
        request->cw_buffers[request->plane * CW_NUM] = g_fc_return_staging[command_id];
        (void)memset(g_fc_return_staging[command_id], 0, 4U);
    }
    else if (command->config.operation == OP_READ_ONFI_SIGNATURE)
    {
        /* ONFI 签名读（v59，90h + 地址 20h → "ONFI"）：器件级命令，无 desc 链（has_data = 0）。
         * 与 OP_READ_META 同型，但**不填 plane_paa**——列地址恒 0x20、行地址恒 0（uop 序列
         * 无 ROW 段），与介质坐标无关，故 command->paa 只用来定 plane（掩码位 = plane*CW_NUM）。
         * 4 字节落**控制器侧暂存**（同 GET_FEATURES / READ_META），完成时拷进 CQ 的 param。
         * ⚠ 必须建这一支：否则落进末尾 else，位图保持空集 ⇒ nand_dispatch_task_load 一格
         *   cw_buffers[] 都不搬 ⇒ NAND 侧拿到残留指针，DATA_OUT 写向野地址。 */
        request->plane                               = (uint8_t)paa_to_plane(command->paa);
        request->cw_bitmap                           = (uint32_t)(1UL << (request->plane * CW_NUM));
        request->cw_buffers[request->plane * CW_NUM] = g_fc_return_staging[command_id];
        (void)memset(g_fc_return_staging[command_id], 0, 4U);
    }
    else if (command->config.operation == OP_READ_COLUMN)
    {
        /* 按显式列读（v63）：无 desc 链（has_data = 0）。与 OP_READ_META 同型，唯一区别是
         * **列地址不走 paa 的 cw 字段，而由命令级 param 的字节 0/1（小端）传入**——cw 只有
         * 2 位（列上限 1584），够不到 spare 区（出厂坏块标记在列 2048）。行地址仍取自
         * command->paa。4 字节落**控制器侧暂存**（同 GET_FEATURES / READ_META），完成时拷进
         * CQ 的 param。
         * ⚠ 必须建这一支：否则落进末尾 else，位图保持空集 ⇒ nand_dispatch_task_load 一格
         *   cw_buffers[] 都不搬 ⇒ NAND 侧拿到残留指针，DATA_OUT 写向野地址。 */
        request->plane                               = (uint8_t)paa_to_plane(command->paa);
        request->plane_paa[request->plane]           = command->paa;
        request->cw_bitmap                           = (uint32_t)(1UL << (request->plane * CW_NUM));
        request->cw_buffers[request->plane * CW_NUM] = g_fc_return_staging[command_id];
        request->column                              = (uint16_t)((uint16_t)command->param.bytes[0] | ((uint16_t)command->param.bytes[1] << 8U));
        (void)memset(g_fc_return_staging[command_id], 0, 4U);
    }
    else if ((command->config.operation == OP_BLOCK_UNLOCK_LOW) ||
             (command->config.operation == OP_BLOCK_UNLOCK_HIGH) ||
             (command->config.operation == OP_BLOCK_LOCK) ||
             (command->config.operation == OP_BLOCK_LOCK_TIGHT) ||
             (command->config.operation == OP_BLOCK_LOCK_STATUS))
    {
        /* 块锁族（v49）：**无 host 数据搬运**（op 表 has_data 全为 0），但 23h/24h/7Ah
         * 都带 3 个地址周期（R1/R2/R3）⇒ 行地址**必须取自 command->paa**，否则退化为
         * block 0（本分支之前这些命令落到末尾 else，plane_paa[] 保持 {0} 整体清零值，
         * 而 ROW_0 的 uop 正是 *address = request->row[operand]，见 nand_fmc.c）。
         * 2Ah/2Ch（BLOCK_LOCK / BLOCK_LOCK_TIGHT）无地址周期，纳入本分支只为把「块锁族」
         * 记在一处——对它们只是多填一个不会被下发的行地址。
         * 不置 cw_bitmap：7Ah 的状态字节走 DQ 总线直读（NAND_UOP_DATA_OUT_STATUS →
         * completion->nand_status，不写 request->data），不经任何数据缓冲。 */
        request->plane                     = (uint8_t)paa_to_plane(command->paa);
        request->plane_paa[request->plane] = command->paa;
    }
    else if (nand_operation_has_data((uint8_t)command->config.operation))
    {
        const fc_status_t build_result = fc_build_request_data_planes(controller, command, request);
        if (build_result != FC_STATUS_OK)
        {
            /* 唯一失败出口（multi-plane 窄位图）。request 只被写了一半，但调用者拿到
             * 非 OK 就不会提交它——宁可不发，也不能让凭空造的回退地址写进介质。 */
            return build_result;
        }
    }
    else
    {
        /* 其余命令（RESET / BLOCK_LOCK / BLOCK_UNLOCK / OTP_DATA_LOCK 等 has_data == 0
         * 且无 desc 链的）：位图保持 0 = **合法空集**（无 CW 可访问）。擦除也走这条口径
         * （上面两个 ERASE 分支只填 plane_paa[]，不再伪造 CW 位）。
         * ⚠ 别把 READ_STATUS / READ_STATUS_ENHANCED 列进来——两者的 has_data 都是 1
         *   （onfi_1_0.c:820/821），实际走上面的 fc_build_request_data_planes 分支；
         *   它们只是从不由 FC 提交（引擎 WAIT_READY 内部直接用 70h/78h uop）。 */
    }

    /* 读语义改写 + Phase V 拆分（顺序敏感，见 helper 头注释） */
    fc_dispatch_rewrite_operation(request);

    return FC_STATUS_OK;
}

/* --------------------------------------------------------------------
 * fc_poll_dispatch_single
 *   功能：提交调度器选出的单条命令（Data Path encode → submit）。
 *   参数：controller —— FC 控制器；chain —— 调度器链（count==1）。
 *   返回：FC_STATUS_OK / IO_ERROR / fc_stripe_submit 结果。
 *   实现要点：
 *     - page write 先 fc_data_path_encode（内含 Stage 0 写 META）。
 *     - raid=ENCODE 的 page write 走 fc_stripe_submit（Multi-Plane）。
 *     - 其余构建 request → nand_dispatch_submit；**构建失败与 submit 失败同路兜底**
 *       （注 NAND_FAIL 完成并返回 IO_ERROR）。构建失败只有 multi-plane 窄位图一种，
 *       debug 档已在 fc_build_request_data_planes 的 ASSERT 处停机，此处只在 release 档可达。
 *     - 只负责占用目标 LUN；完成由后续 fc_service() 推进。
 *     - **运行时唯一活提交路径**（FTL 恒 chain=END，count 恒 1）。
 * -------------------------------------------------------------------- */
static fc_status_t fc_poll_dispatch_single(fc_t *controller, const fc_scheduler_chain_t *chain)
{
    fc_command_id_t         command_id = chain->ids[0U];
    fc_command_t           *command;
    nand_dispatch_request_t request;

    /* 命令刚从 SQ/HSQ 弹出：id 恒为池内铸出值，状态恒 QUEUED（fc_submit_to_queue 置位）。
     * 旧写法 return IO_ERROR 时命令已出队且未置 ACTIVE/未回推 → 永久泄漏，比停机更难查。 */
    ASSERT(command_id < FC_COMMAND_POOL_DEPTH);
    ASSERT(controller->command_state[command_id] == FC_COMMAND_STATE_QUEUED);
    controller->command_state[command_id] = FC_COMMAND_STATE_ACTIVE;
    command                               = &controller->command_pool[command_id];

    /* ── Data Path encode: Random 加扰 → ECC 编码（RAID XOR 已移至 stripe 路径）──
     * 在 NAND 编程前对 buffer 数据做编码，对 FTL 完全透明。 */
    if (fc_is_page_write((uint8_t)command->config.operation) != 0U)
    {
        (void)fc_data_path_encode(command);
    }

    /* ── Stripe RAID 分支：raid=ENCODE 的 page write 走 Multi-Plane 提交 ── */
#if CONFIG_FC_RAID
    if ((fc_is_page_write((uint8_t)command->config.operation) != 0U) &&
        (command->config.raid == FC_RAID_MODE_ENCODE))
    {
        fc_status_t stripe_result = fc_stripe_submit(controller, command, command_id);
        return stripe_result; /* 成功/失败均已在 fc_stripe_submit 中处理 */
    }
#endif /* CONFIG_FC_RAID */

    const fc_status_t build_result = fc_build_dispatch_request(controller, command, command_id, &request);
    if (build_result != FC_STATUS_OK)
    {
        /* 构建失败（multi-plane 窄位图）：debug 档上一条 ASSERT 已停机，这里只在 release
         * 档可达——命令注 NAND_FAIL 完成项后了结，**绝不**把凭空造的回退地址提交给 NAND。 */
        LOGW("fc: build request failed for op=%u cmd_id=%u status=%d\r\n",
             (unsigned)command->config.operation,
             (unsigned)command_id,
             (int)build_result);
        fc_poll_fail_command(controller, command_id);
        return FC_STATUS_IO_ERROR;
    }

    const uint8_t submit_result = nand_dispatch_submit(&request);

    /* 填充 pass（fc_poll_fill）已按 nand_dispatch_lun_free() 预留槽位，
     * 此处不应再遇到 NOT_READY；其余失败（设备未就绪等）才是真错误。 */
    ASSERT(submit_result != NAND_RESULT_NOT_READY);
    if (submit_result != NAND_RESULT_OK)
    {
        LOGW("fc: nand_dispatch_submit failed for op=%u cmd_id=%u cw_bitmap=0x%02X plane_count=%u\r\n",
             (unsigned)command->config.operation,
             (unsigned)command_id,
             (unsigned)request.cw_bitmap,
             (unsigned)request.plane_count);
        fc_poll_fail_command(controller, command_id);
        return FC_STATUS_IO_ERROR;
    }

    /* Submission only occupies the target LUN. Completion is advanced by a later fc_service() call. */
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
    uint8_t                 j;

    for (i = 0U; i < chain_count; i++)
    {
        fc_command_id_t command_id = chain->ids[i];
        fc_command_t   *command;

        /* 同 fc_poll_dispatch_single：id 恒在池内、状态恒 QUEUED（队列成员不变量） */
        ASSERT(command_id < FC_COMMAND_POOL_DEPTH);
        ASSERT(controller->command_state[command_id] == FC_COMMAND_STATE_QUEUED);
        controller->command_state[command_id] = FC_COMMAND_STATE_ACTIVE;
        command                               = &controller->command_pool[command_id];

        /* ── Data Path encode: Random 加扰 → ECC 编码 → RAID XOR ──
         * 在 NAND 编程前对 buffer 数据做编码，对 FTL 完全透明。 */
        if (fc_is_page_write((uint8_t)command->config.operation) != 0U)
        {
            (void)fc_data_path_encode(command);
        }

        const fc_status_t build_result = fc_build_dispatch_request(controller, command, command_id, &reqs[i]);
        if (build_result != FC_STATUS_OK)
        {
            /* 构建失败（multi-plane 窄位图）→ 整链注 NAND_FAIL，与下面 submit 失败的兜底
             * 口径一致：已置 ACTIVE 的前缀命令一并了结，不留半截链。 */
            LOGW("fc: build request failed for cmd_id=%u (chain %u/%u) status=%d\r\n",
                 (unsigned)command_id,
                 (unsigned)i,
                 (unsigned)chain_count,
                 (int)build_result);
            for (j = 0U; j < chain_count; j++)
            {
                fc_poll_fail_command(controller, chain->ids[j]);
            }
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
        return FC_STATUS_IO_ERROR;
    }

    /* Submission only occupies the target LUN. Completion is advanced by a later fc_service() call. */
    return FC_STATUS_OK;
}

/* --------------------------------------------------------------------
 * fc_poll_chain_length
 *   功能：计算队头命令所属链的长度（链头已弹出，peek 从队头看后续）。
 *   参数：queue —— 提交队列，HSQ/SQ 通用（队头已是链头之后的下一条）；
 *         head  —— 已弹出的链头命令。
 *   返回：链长（1..FC_SCHEDULER_CHAIN_MAX）。
 *   实现要点：
 *     - head->config.chain == FC_CHAIN_END → 单命令，返回 1。
 *     - 否则 peek 后续，收集同一 (ch,ce,lun) 的连续命令，直到 chain==END 或链满。
 *     - 只 peek 不消耗——真正弹出在 fc_poll_fill_queue 里，链放不下时可原样留在队列。
 * -------------------------------------------------------------------- */
static uint8_t fc_poll_chain_length(const fc_submission_queue_t *queue, const fc_command_t *head)
{
    uint8_t need = 1U;

    ASSERT(queue != NULL);
    ASSERT(head != NULL);

    if (head->config.chain == FC_CHAIN_END)
    {
        return 1U;
    }

    while (need < FC_SCHEDULER_CHAIN_MAX)
    {
        fc_command_id_t     next_id = fc_submission_queue_peek(queue, (uint16_t)(need - 1U));
        const fc_command_t *next;

        if (next_id == FC_COMMAND_ID_INVALID)
        {
            break; /* 队列尾 */
        }
        next = fc_command_id_to_command(next_id);
        ASSERT(next != NULL);

        /* 链成员必须同一 target（FTL 保证）；不满足即链构造错误，
         * 此处 break 会静默把一条链截成两条、丢失链语义，故断言。 */
        ASSERT((next->config.ch == head->config.ch) && (next->config.ce == head->config.ce) &&
               (next->config.lun == head->config.lun));
        need++;
        if (next->config.chain == FC_CHAIN_END)
        {
            break; /* chain=0：链结束 */
        }
    }
    return need;
}

/* --------------------------------------------------------------------
 * fc_poll_fill_queue
 *   功能：扫描单个提交队列一趟，把能放下的命令（含链）提交进 NAND per-LUN FIFO。
 *   参数：controller —— FC 控制器；
 *         queue      —— 待扫描队列（HSQ 或 SQ）；
 *         owner      —— 本队列的链归属标签（FC_CHAIN_OWNER_SQ / _HSQ）；
 *                       链锁准入用：某 LUN 上有未结束的链时，只放行链所在队列；
 *         suspended  —— 本轮已挂起 LUN 表（HSQ/SQ 共享，见 fc_poll_fill）；
 *         dispatched —— 累计已提交条数（进出）。
 *   返回：FC_STATUS_OK / IO_ERROR（提交失败，剩余命令留在队列）。
 *   实现要点：
 *     - **按 LUN 保序**：命令 need = 链长；nand_dispatch_lun_free(lun) >= need 才取，
 *       否则该 LUN 本轮挂起（suspended），后续同 LUN 命令一律跳过。
 *     - **链锁准入**：该 LUN 上有未结束的链（chain_owner != NONE）且链不归本队列 →
 *       本命令回推跳过。**只回推、不置 suspended**——置了会把链自己的续批一起锁死。
 *     - **链锁更新**：一批下发后看批尾的 chain 位——MORE 继续占着该 LUN（转本队列），
 *       END 解锁；下发失败（整链注 NAND_FAIL）也清锁，不留残锁。
 *     - **一次扫描**：按进入时的队列快照数量扫描，扫完即止。
 *     - **顺序保持**：取不走的命令 pop 后回推队尾——扫完整轮等价于一次旋转，
 *       未取出命令的相对顺序不变（同 LUN 的命令因 LUN 挂起必然一起回推，顺序不破）。
 *       链成员连续且逐个回推，链头永远排在成员之前，链序不破。
 *     - 提交失败即中止本轮，剩余命令仍在队列里（未被取出），下轮重试。
 * -------------------------------------------------------------------- */
static fc_status_t fc_poll_fill_queue(fc_t *controller, fc_submission_queue_t *queue, uint8_t owner, uint8_t suspended[CH_NUM][CE_NUM][LUN_NUM], uint8_t *dispatched)
{
    uint16_t total;
    uint16_t k;

    ASSERT(queue != NULL);
    ASSERT(dispatched != NULL);

    total = queue->count;

    for (k = 0U; k < total; k++)
    {
        fc_command_id_t      id;
        fc_command_t        *cmd;
        fc_scheduler_chain_t chain;
        fc_status_t          dispatch_status;
        uint8_t              ch;
        uint8_t              ce;
        uint8_t              lun;
        uint8_t              need;
        uint8_t              j;

        (void)fc_submission_queue_pop(queue, &id);
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
        if (suspended[ch][ce][lun] != 0U)
        {
            (void)fc_submission_queue_push(queue, id);
            continue;
        }
        /* 链锁准入：该 LUN 的填充权归另一条队列的未结束链 → 本命令回推跳过。
         * 为什么「只让拥有者队列喂」就够：链成员在队列里连续（见 fc_scheduler.h 的
         * 前置契约），续批恒是该 LUN 在队列里最老的那几条，按序扫描必然先取到它们；
         * 后入队的同 LUN 命令都排在链后面，只会在链结束（chain_owner 清）之后才被取。
         * ⚠ **只回推、不置 suspended**——置了会把链自己的续批也一并锁死（该 LUN 从此
         *   既不接受另一队列、也不接受本链，活锁）。
         * 生产路径恒假：FTL 恒 FC_CHAIN_END ⇒ chain_owner 恒 NONE。 */
        if ((controller->scheduler.chain_owner[ch][ce][lun] != (uint8_t)FC_CHAIN_OWNER_NONE) &&
            (controller->scheduler.chain_owner[ch][ce][lun] != owner))
        {
            (void)fc_submission_queue_push(queue, id);
            continue;
        }

        need = fc_poll_chain_length(queue, cmd);
        if (nand_dispatch_lun_free(ch, ce, lun) < need)
        {
            /* 剩余槽放不下整条链：本轮挂起该 LUN，链头回推（成员后续轮次一并回推） */
            suspended[ch][ce][lun] = 1U;
            (void)fc_submission_queue_push(queue, id);
            continue;
        }

        chain.count   = need;
        chain.ids[0U] = id;
        for (j = 1U; j < need; j++)
        {
            (void)fc_submission_queue_pop(queue, &chain.ids[j]);
        }
        k = (uint16_t)(k + need - 1U); /* 跳过已取出的链成员 */

        /* 单命令走 single，链走 chain——按 need 分流（不写成条件里的三目，
         * 两个分支都是带副作用的提交，塞进 if 里读不出"做了什么事"） */
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
            /* 下发失败 = fc_poll_dispatch_chain 已把整链注 NAND_FAIL（构建失败或 submit
             * 失败都如此），这条链已经死了、不会再续批 ⇒ 必须清锁。不清的话该 LUN 会
             * 一直只认本队列，而那条链又不存在了，等于把 LUN 永久锁死。 */
            controller->scheduler.chain_owner[ch][ce][lun] = (uint8_t)FC_CHAIN_OWNER_NONE;
            return FC_STATUS_IO_ERROR;
        }
        /* 链锁更新：看**批尾**（chain.ids[need-1]，本批最后一条）的 chain 位——
         *   MORE ⇒ 这条链还没完，该 LUN 的填充权继续归本队列（下一批仍从这条队列取）；
         *   END  ⇒ 链在此收尾，解锁，另一条队列可以喂这个 LUN 了。
         * 单命令（need == 1）批尾恒 END ⇒ 恒写 NONE，即生产路径只是多一次无副作用的写。 */
        controller->scheduler.chain_owner[ch][ce][lun] =
            (controller->command_pool[chain.ids[need - 1U]].config.chain == FC_CHAIN_MORE) ? owner : (uint8_t)FC_CHAIN_OWNER_NONE;
        (*dispatched)++;
    }

    return FC_STATUS_OK;
}

/* --------------------------------------------------------------------
 * fc_poll_fill
 *   功能：一次填充 pass——扫描 HSQ/SQ，把可下发的命令（含链）提交进 NAND per-LUN FIFO。
 *   参数：controller —— FC 控制器。
 *   返回：FC_STATUS_OK（本轮至少提交了一条）/ IO_ERROR（提交失败，剩余命令留在队列）/
 *         QUEUE_EMPTY / NOT_READY（无可提交，供 ftl_service 结束本轮 fc_service 循环）。
 *   实现要点：
 *     - **HSQ 优先**：先扫 HSQ，再扫 SQ；两个队列走同一套扫描逻辑（含链）。
 *     - **共享 suspended**：HSQ 因 room 不足挂起某 LUN 后，SQ 本轮也跳过该 LUN——
 *       否则同 LUN 的 SQ 命令会插到 HSQ 命令之前下发，优先级倒挂。
 *     - **链锁独占（跨 pass 持久，与 suspended 正交）**：链一旦开始，该 LUN 的填充权
 *       归链所在队列独占，直到链的 END 下发——另一条队列的命令一律回推。
 *       与上一条的区别：suspended 管"本趟 room 不足"、每趟清零；chain_owner 管"链没完"、
 *       跨趟保留。两者互不影响（链锁准入只回推，不置 suspended）。
 *     - 扫描细节见 fc_poll_fill_queue()。
 * -------------------------------------------------------------------- */
static fc_status_t fc_poll_fill(fc_t *controller)
{
    fc_scheduler_t *scheduler = &controller->scheduler;
    uint8_t         suspended[CH_NUM][CE_NUM][LUN_NUM];
    uint8_t         dispatched = 0U;

    memset(suspended, 0, sizeof(suspended));

    /* ── HSQ 优先：先扫 HSQ，再扫 SQ；同一套扫描逻辑，共享同一张 suspended 表 ── */
    if (fc_poll_fill_queue(controller, scheduler->hsq, (uint8_t)FC_CHAIN_OWNER_HSQ, suspended, &dispatched) != FC_STATUS_OK)
    {
        return FC_STATUS_IO_ERROR;
    }
    if (fc_poll_fill_queue(controller, scheduler->sq, (uint8_t)FC_CHAIN_OWNER_SQ, suspended, &dispatched) != FC_STATUS_OK)
    {
        return FC_STATUS_IO_ERROR;
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
 *     - 先 nand_dispatch_poll() 推进 NAND 全部 LUN；每条完成经 sink 回调**当场消费**
 *       （fc_complete_from_nand + 清 stripe parity），故一个 sink 上下文即可，无需缓存批量。
 *       一次调用把本轮所有完成抽干（NAND 命令自身的失败不算 FC 故障——状态已在 CQ entry 里）。
 *     - 无完成 → fc_poll_fill()：一次扫描把能放下的命令（含链）提交进 NAND
 *       per-LUN FIFO（按 LUN 保序、HSQ 优先、room 判定）。
 *   运行时事实（勿被 §16/图 11 误导）：FTL 所有命令 config.chain=FC_CHAIN_END，
 *   运行时 count 恒为 1 → **只有 fc_poll_dispatch_single 是活路径**；
 *   fc_poll_dispatch_chain 仅单元测试覆盖。详见设计文档 §23.8。
 *   调用方：ftl_service()（ftl.c）/ ftl_sync_submit_wait() / main.c 的裸 fc_service。
 * -------------------------------------------------------------------- */

/* ── NAND 完成回调上下文 + sink ──
 * nand_dispatch_poll 每产出一条完成就调一次 fc_nand_completion_sink；每条当场消费
 * （写 desc/ECC、跑数据路径、组 CQ 项、清 stripe parity），不跨条缓存。 */
typedef struct
{
    fc_t   *controller;
    uint8_t failed; /* fc_complete_from_nand 返回非 OK（CQ 满强制 COMPLETED 防泄漏）时置位 */
} fc_nand_sink_ctx_t;

static void fc_nand_completion_sink(const nand_dispatch_completion_t *nand_completion, void *ctx)
{
    fc_nand_sink_ctx_t *sink_ctx = (fc_nand_sink_ctx_t *)ctx;

    if (fc_complete_from_nand(sink_ctx->controller, nand_completion) != FC_STATUS_OK)
    {
        sink_ctx->failed = 1U;
    }

    /* ── Stripe parity 清理 ──
     * 如果完成的命令是 stripe 数据命令，释放内部 parity 命令资源。 */
#if CONFIG_FC_RAID
    {
        fc_command_id_t completed_id = (fc_command_id_t)nand_completion->command_id;
        /* command_id 是 FC 投递给 NAND 的池内 ID，fc_complete_from_nand 已断言过范围 */
        ASSERT(completed_id < FC_COMMAND_POOL_DEPTH);
        if (sink_ctx->controller->stripe_parity_for_data[completed_id] != FC_COMMAND_ID_INVALID)
        {
            fc_command_id_t parity_id                                  = sink_ctx->controller->stripe_parity_for_data[completed_id];
            sink_ctx->controller->stripe_parity_for_data[completed_id] = FC_COMMAND_ID_INVALID;
            fc_stripe_release_parity(sink_ctx->controller, parity_id);
        }
    }
#endif /* CONFIG_FC_RAID */
}

fc_status_t fc_poll_internal(fc_t *controller)
{
    fc_status_t        result;
    fc_nand_sink_ctx_t sink_ctx;

    sink_ctx.controller = controller;
    sink_ctx.failed     = 0U;

    if (nand_dispatch_poll(fc_nand_completion_sink, &sink_ctx) != 0U)
    {
        /* 完成已全部消费。只有 CQ 满（FC 侧故障）才报 IO_ERROR；
         * NAND 命令自身的失败不是故障——状态已随 completion 进了 CQ entry。 */
        if (sink_ctx.failed != 0U)
        {
            return FC_STATUS_IO_ERROR;
        }
        return FC_STATUS_OK;
    }

    /* 无完成 → 填充 pass：一次扫描把可下发命令（含链）提交进 NAND per-LUN FIFO。 */
    result = fc_poll_fill(controller);
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
    return result;
}

/* --------------------------------------------------------------------
 * fc_poll_completions_internal
 *   功能：从 CQ 批量取完成项——弹到 max_count 条或队列空为止。
 *   参数：controller —— FC 控制器；completions —— 输出数组（容量 ≥ max_count）；
 *         max_count —— 本次上限（**调用方给**，FC 不写死）；count —— 输出实际条数。
 *   返回：取到 ≥1 条 = OK；一条没取到 = QUEUE_EMPTY（含 max_count==0）。
 *   要点：循环被 CQ 占用数（≤ FC_CQ_DEPTH）天然封顶，max_count 给多大都不会越界；
 *         语义与单条版一致（有取到 = OK / 一条没有 = QUEUE_EMPTY）。
 * -------------------------------------------------------------------- */
fc_status_t fc_poll_completions_internal(fc_t *controller, fc_command_status_t *completions, uint16_t max_count, uint16_t *count)
{
    uint16_t n = 0U;

    while ((n < max_count) && (fc_completion_queue_pop(&controller->cq, &completions[n]) == FC_STATUS_OK))
    {
        n++;
    }
    *count = n;

    return (n != 0U) ? FC_STATUS_OK : FC_STATUS_QUEUE_EMPTY;
}

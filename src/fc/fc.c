#include "fc.h"

#include "common/ssd_log.h"
#include "fc_crc_engine.h"
#include "fc_data_path.h"
#include "fc_queue.h"
#include "fc_registers.h"
#include "fc_scheduler.h"
#include "nand/nand_dispatch.h"

#if CONFIG_FC_RAID
#    include "fc_raid_engine.h"
#endif

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
        case OP_READ_PAGE_FETCH:             /* Phase V: FETCH 也有数据输出 */
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
        case OP_PROGRAM_PAGE_ISSUE:          /* Phase V: ISSUE 段也有数据写入 */
            return 1U;
        default:
            return 0U;
    }
}

#include <stddef.h>
#include <string.h>

/* FC 硬件模型仅属于实现层；FTL 不能取得或修改该控制器上下文。 */
typedef enum
{
    FC_COMMAND_STATE_FREE = 0U,
    FC_COMMAND_STATE_ALLOCATED,
    FC_COMMAND_STATE_QUEUED,
    FC_COMMAND_STATE_ACTIVE,
    FC_COMMAND_STATE_COMPLETED
} fc_command_state_t;

#if CONFIG_FC_RAID
/* ── 跨 Plane Stripe RAID 积累状态 ──
 *
 * 当前 PLANE_NUM=2，FC_STRIPE_DATA_MAX=1，单命令即满 stripe，
 * fc_stripe_submit() 在 fc_poll_internal() 中直接处理，不经过 accumulator。
 *
 * fc_stripe_accum_t 和 stripe_accum[] 为 PLANE_NUM>=4 预留扩展：
 * 当需要跨多次 fc_service() 调用积累多条数据命令后统一提交 parity 时，
 * 由 fc_poll_stripe_accumulate()（待实现）管理。 */

/* 单组 stripe 的最大数据命令数 = PLANE_NUM - 1 */
#    define FC_STRIPE_DATA_MAX ((uint8_t)(PLANE_NUM - 1U))

/* 每个 LUN 的 stripe 积累状态（PLANE_NUM>=4 扩展预留） */
typedef struct
{
    uint8_t         active;                       /* 0=空闲, 1=积累中 */
    uint8_t         ch, ce, lun;                  /* 目标 LUN */
    uint8_t         count;                        /* 已积累的数据命令数 */
    fc_command_id_t data_ids[FC_STRIPE_DATA_MAX]; /* 已积累的数据命令 ID */
    fc_command_id_t parity_id;                    /* 已分配的 parity 命令 ID */
} fc_stripe_accum_t;
#endif /* CONFIG_FC_RAID */

typedef struct
{
    fc_registers_t        registers;
    fc_submission_queue_t sq;
    fc_submission_queue_t hsq;
    fc_completion_queue_t cq;
    fc_scheduler_t        scheduler;
    fc_command_t         *command_pool;
    fc_descriptor_t      *descriptor_pool;
    uint8_t (*buffer_pool)[FC_BUFFER_POOL_CW_SIZE];
    fc_command_id_t *descriptor_owner;
    uint8_t         *command_state;
    fc_command_id_t *command_free_next;
    uint16_t        *descriptor_free_next;
    uint16_t        *descriptor_owned_next;
    uint16_t        *buffer_free_next;
    uint8_t         *buffer_state;
    uint16_t        *command_descriptor_head;
    fc_command_id_t  command_free_head;
    uint16_t         descriptor_free_head;
    uint16_t         buffer_free_head;
    fc_command_id_t  active_handle;
    uint8_t          active_valid;

    /* ── Stripe RAID 积累状态 ── */
#if CONFIG_FC_RAID
    fc_stripe_accum_t stripe_accum[LUN_NUM];                         /* 每 LUN stripe 积累 */
    fc_command_id_t   stripe_parity_for_data[FC_COMMAND_POOL_DEPTH]; /* 数据命令 ID → parity 命令 ID 映射 */
#endif                                                               /* CONFIG_FC_RAID */
} fc_t;

/* fc_complete_from_nand() is defined before the internal completion implementation. */
static fc_status_t fc_complete_internal(fc_t *controller, fc_command_status_t *completion);

/*
 * FC 控制面固定资源区。当前位于默认 .bss（AXI SRAM）；不使用动态内存。
 * 后续根据 DMA/Cache 和 Buffer Pool 设计，可将各数组分别放入指定链接段。
 */
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
static fc_t                g_fc_controller;

static uint8_t fc_command_id_is_valid(fc_command_id_t command_id)
{
    return (uint8_t)(command_id < FC_COMMAND_POOL_DEPTH);
}

static uint8_t fc_buffer_id_is_valid(const fc_t *controller, fc_buffer_id_t buffer_id)
{
    return (uint8_t)((buffer_id < FC_BUFFER_POOL_DEPTH) &&
                     (controller->buffer_state[buffer_id] != 0U));
}

static uint8_t *fc_buffer_id_to_real_buffer_internal(fc_t *controller, fc_buffer_id_t buffer_id)
{
    return controller->buffer_pool[buffer_id];
}

static fc_buffer_id_t fc_real_buffer_to_buffer_id_internal(fc_t *controller, const uint8_t *buffer)
{
    uint16_t index;

    for (index = 0U; index < FC_BUFFER_POOL_DEPTH; index++)
    {
        if (buffer == controller->buffer_pool[index])
        {
            return (fc_buffer_id_t)index;
        }
    }
    return FC_BUFFER_ID_INVALID;
}

static fc_status_t fc_buffer_release_internal(fc_t *controller, fc_buffer_id_t buffer_id)
{
    controller->buffer_state[buffer_id]     = 0U;
    controller->buffer_free_next[buffer_id] = controller->buffer_free_head;
    controller->buffer_free_head            = buffer_id;
    return FC_STATUS_OK;
}

static uint8_t fc_command_to_id(const fc_t *controller, const fc_command_t *command, fc_command_id_t *command_id)
{
    uint16_t index;

    if ((controller == NULL) || (command == NULL) || (command_id == NULL))
    {
        return 0U;
    }
    for (index = 0U; index < FC_COMMAND_POOL_DEPTH; index++)
    {
        if (command == &controller->command_pool[index])
        {
            *command_id = (fc_command_id_t)index;
            return 1U;
        }
    }
    return 0U;
}

static uint8_t fc_command_descriptor_count(const fc_command_t *command)
{
    return (command->desc_id != FC_DESCRIPTOR_ID_INVALID) ? (uint8_t)(command->config.cw + 1U) : 0U;
}

static uint8_t fc_command_descriptors_owned(const fc_t *controller, fc_command_id_t command_id, const fc_command_t *command)
{
    fc_descriptor_id_t     descriptor_id = (fc_descriptor_id_t)command->desc_id;
    const fc_descriptor_t *descriptor;
    uint8_t                descriptor_count;
    uint16_t               pool_index;
    uint8_t                found;

    for (descriptor_count = 0U; descriptor_count < fc_command_descriptor_count(command); descriptor_count++)
    {
        found      = 0U;
        pool_index = descriptor_id;
        if (pool_index >= FC_DESCRIPTOR_POOL_DEPTH)
        {
            return 0U;
        }
        descriptor = &controller->descriptor_pool[pool_index];
        if (controller->descriptor_owner[pool_index] == command_id)
        {
            found = 1U;
        }
        if (found == 0U)
        {
            return 0U;
        }
        descriptor_id = (fc_descriptor_id_t)descriptor->next_id;
    }
    return 1U;
}

static uint8_t fc_cq_contains_command(const fc_t *controller, const fc_command_t *command)
{
    uint16_t index = controller->cq.head;
    uint16_t count;

    for (count = 0U; count < controller->cq.count; count++)
    {
        if ((command != NULL) && (controller->cq.entries[index].command_id == command->command_id))
        {
            return 1U;
        }
        index = (uint16_t)((index + 1U) % controller->cq.depth);
    }
    return 0U;
}

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

static fc_status_t fc_init_internal(fc_t *controller)
{
    fc_status_t result;
    uint16_t    index;

    memset(g_fc_command_pool, 0, sizeof(g_fc_command_pool));
    memset(g_fc_descriptor_pool, 0, sizeof(g_fc_descriptor_pool));
    memset(g_fc_buffer_pool, 0, sizeof(g_fc_buffer_pool));
    memset(g_fc_sq_storage, 0, sizeof(g_fc_sq_storage));
    memset(g_fc_hsq_storage, 0, sizeof(g_fc_hsq_storage));
    memset(g_fc_cq_storage, 0, sizeof(g_fc_cq_storage));
    memset(g_fc_command_state, 0, sizeof(g_fc_command_state));
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
    fc_registers_reset(&controller->registers);
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
    result = fc_scheduler_init(&controller->scheduler, &controller->sq, &controller->hsq);
    if (result != FC_STATUS_OK)
    {
        return result;
    }
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

    /* 初始化 CRC 引擎 */
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

    for (index = 0U; index < FC_COMMAND_POOL_DEPTH; index++)
    {
        controller->command_free_next[index]       = ((index + 1U) < FC_COMMAND_POOL_DEPTH) ? (fc_command_id_t)(index + 1U) : FC_COMMAND_ID_INVALID;
        controller->command_descriptor_head[index] = UINT16_MAX;
        controller->command_pool[index].command_id = index;
    }
    for (index = 0U; index < FC_DESCRIPTOR_POOL_DEPTH; index++)
    {
        controller->descriptor_owner[index]      = FC_COMMAND_ID_INVALID;
        controller->descriptor_free_next[index]  = ((index + 1U) < FC_DESCRIPTOR_POOL_DEPTH) ? (uint16_t)(index + 1U) : UINT16_MAX;
        controller->descriptor_owned_next[index] = UINT16_MAX;
    }
    for (index = 0U; index < FC_BUFFER_POOL_DEPTH; index++)
    {
        controller->buffer_free_next[index] = ((index + 1U) < FC_BUFFER_POOL_DEPTH) ? (uint16_t)(index + 1U) : UINT16_MAX;
        controller->buffer_state[index]     = 0U;
    }
    controller->command_free_head    = 0U;
    controller->descriptor_free_head = 0U;
    controller->buffer_free_head     = 0U;
    controller->active_handle        = FC_COMMAND_ID_INVALID;
    fc_register_queue_state(controller);
    return FC_STATUS_OK;
}

static fc_command_t *fc_command_alloc_internal(fc_t *controller)
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

static fc_buffer_id_t fc_buffer_alloc_internal(fc_t *controller)
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

static fc_status_t fc_buffer_release_internal_api(fc_t *controller, fc_buffer_id_t buffer_id)
{
    return fc_buffer_release_internal(controller, buffer_id);
}

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

static fc_status_t fc_command_configure_descriptor_internal(fc_t *controller, fc_command_t *command, uint8_t index, fc_descriptor_t **configured_descriptor)
{
    fc_descriptor_t *descriptor;
    fc_buffer_id_t   buffer_id;
    uint8_t          cw;

    if (configured_descriptor == NULL)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
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
        uint8_t  plane        = index / CW_NUM;
        uint8_t  cw_idx       = index % CW_NUM;
        uint32_t blk          = paa_to_block(command->paa);
        uint32_t pg           = paa_to_page(command->paa);
        descriptor->paa       = paa_make(pg, plane, blk, 0U, cw_idx);
        descriptor->config    = command->config;
        descriptor->config.cw = cw_idx;
    }
    else
    {
        cw = (uint8_t)(paa_to_cw(command->paa) + index);
        if (cw >= CW_NUM)
        {
            (void)fc_buffer_release_internal(controller, buffer_id);
            return FC_STATUS_INVALID_ARGUMENT;
        }
        descriptor->paa       = (command->paa & ~NAND_FIELD_MASK(CW_WIDTH)) | cw;
        descriptor->config    = command->config;
        descriptor->config.cw = cw;
    }
    descriptor->buffer_id  = buffer_id;
    descriptor->next_id    = FC_DESCRIPTOR_ID_INVALID;
    descriptor->faa        = (uint32_t)(command->faa + index);
    *configured_descriptor = descriptor;
    return FC_STATUS_OK;
}

static fc_status_t fc_command_configure_descriptors_internal(fc_t *controller, fc_command_t *command)
{
    fc_command_id_t  command_id;
    fc_descriptor_t *previous = NULL;
    uint8_t          index;

    /* NO_DATA operations (RESET, BLOCK_LOCK, READ_MODE, etc.) and ERASE have no
     * data payload — they do not need descriptors or buffers.
     * SET/GET_FEATURES use inline parameter[0..3] + feature_addr instead. */
    if ((nand_operation_has_data((uint8_t)command->config.operation) == 0U) ||
        (command->config.operation == OP_SET_FEATURES) ||
        (command->config.operation == OP_GET_FEATURES))
    {
        command->desc_id = FC_DESCRIPTOR_ID_INVALID;
        return FC_STATUS_OK;
    }

    uint8_t total_desc = (command->config.multi_plane > 0U)
                             ? (uint8_t)(command->config.multi_plane * (uint8_t)(command->config.cw + 1U))
                             : (uint8_t)(command->config.cw + 1U);

    command_id = (fc_command_id_t)(command - controller->command_pool);
    for (index = 0U; index < total_desc; index++)
    {
        fc_descriptor_t *descriptor = NULL;
        fc_status_t      result     = fc_command_configure_descriptor_internal(controller, command, index, &descriptor);

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
    return FC_STATUS_OK;
}

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

static fc_status_t fc_submit_internal(fc_t *controller, fc_command_t *command)
{
    fc_status_t result;

    result = fc_submit_to_queue(controller, (command->high_priority != 0U) ? &controller->hsq : &controller->sq, command);
    fc_register_queue_state(controller);
    return result;
}

static fc_status_t fc_complete_from_nand(fc_t *controller, const nand_dispatch_completion_t *nand_completion)
{
    fc_command_status_t completion;
    fc_command_t       *command;

    if ((nand_completion == NULL) || (nand_completion->command_id >= FC_COMMAND_POOL_DEPTH))
    {
        return FC_STATUS_IO_ERROR;
    }

    command = &controller->command_pool[nand_completion->command_id];

    /* ── Step 1: Write back per-CW status and ECC count from NAND completion ── */
    if (command->desc_id != FC_DESCRIPTOR_ID_INVALID)
    {
        fc_descriptor_id_t desc_id = (fc_descriptor_id_t)command->desc_id;
        uint8_t            cw;
        uint8_t            total_cw = (command->config.multi_plane > 0U)
                                          ? (uint8_t)(command->config.multi_plane * (uint8_t)(command->config.cw + 1U))
                                          : (uint8_t)(command->config.cw + 1U);

        for (cw = 0U; cw < total_cw; cw++)
        {
            fc_descriptor_t *descriptor;

            if (desc_id >= FC_DESCRIPTOR_POOL_DEPTH)
            {
                break;
            }
            descriptor           = &controller->descriptor_pool[desc_id];
            descriptor->status   = nand_completion->cw_status[cw];
            descriptor->ecc_bits = nand_completion->ecc_bit_count[cw];
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

    /* ── Step 3: Populate completion entry ── */
    completion.command_id = nand_completion->command_id;
    completion.desc_id    = nand_completion->failed_descriptor_id;
    completion.module     = command->module;

    if ((command->desc_id != FC_DESCRIPTOR_ID_INVALID) &&
        (fc_is_page_read((uint8_t)command->config.operation) != 0U))
    {
        /* 从 descriptor 读取 ECC 解码后的 per-CW status/ecc_bits */
        fc_descriptor_id_t did;
        uint8_t            i;
        uint8_t            status_slot[4] = {0U, 0U, 0U, 0U};
        uint8_t            ecc_slot[4]    = {0U, 0U, 0U, 0U};
        uint8_t            found_unc      = 0U;
        uint8_t            total_cw       = (command->config.multi_plane > 0U)
                                                ? (uint8_t)(command->config.multi_plane * (uint8_t)(command->config.cw + 1U))
                                                : (uint8_t)(command->config.cw + 1U);

        did = (fc_descriptor_id_t)command->desc_id;
        for (i = 0U; i < total_cw; i++)
        {
            if ((did < FC_DESCRIPTOR_POOL_DEPTH) &&
                (i < 4U || command->config.multi_plane > 0U))
            {
                const fc_descriptor_t *d = &controller->descriptor_pool[did];
                /* 将前 4 个 CW 的 ECC 和 status 填入 completion slot；
                 * 后续 CW（多 plane）仅累加 status。 */
                if (i < 4U)
                {
                    status_slot[i] = d->status;
                    ecc_slot[i]    = d->ecc_bits;
                }
                else
                {
                    status_slot[i - 4U] |= d->status;
                }
                /* 更新 failed_descriptor_id 指向 ECC 解码后第一个 UNC 的 desc */
                if ((found_unc == 0U) && (d->status == FC_CW_STATUS_UNC))
                {
                    completion.desc_id = did;
                    found_unc          = 1U;
                }
                did = (fc_descriptor_id_t)d->next_id;
            }
            else
            {
                if (i < 4U)
                {
                    status_slot[i] = FC_CW_STATUS_UNUSED;
                    ecc_slot[i]    = 0U;
                }
                break;
            }
        }
        completion.status   = (uint8_t)(status_slot[0] | status_slot[1] | status_slot[2] | status_slot[3]);
        completion.ecc_bit0 = ecc_slot[0];
        completion.ecc_bit1 = ecc_slot[1];
        completion.ecc_bit2 = ecc_slot[2];
        completion.ecc_bit3 = ecc_slot[3];
    }
    else
    {
        /* 非 page read：直接透传 NAND completion 状态 */
        completion.ecc_bit0 = nand_completion->ecc_bit_count[0];
        completion.ecc_bit1 = nand_completion->ecc_bit_count[1];
        completion.ecc_bit2 = nand_completion->ecc_bit_count[2];
        completion.ecc_bit3 = nand_completion->ecc_bit_count[3];
        completion.status   = (uint8_t)(nand_completion->cw_status[0] |
                                      nand_completion->cw_status[1] |
                                      nand_completion->cw_status[2] |
                                      nand_completion->cw_status[3]);
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
 * Stripe RAID 辅助函数
 *
 * fc_stripe_release_parity()  — 回收 parity 命令的 desc+buffer+command 资源
 * fc_stripe_submit()         — 分配 parity 命令、配置、XOR、构建 MP request、submit
 *
 * 编译开关：CONFIG_FC_RAID (include/fc/fc.h)
 * ══════════════════════════════════════════════════════════ */

#if CONFIG_FC_RAID

/*
 * 回收 parity 命令占用的所有资源，返回 command slot 到空闲池。
 * 在 NAND completion 路径中调用，parity 数据已完成使命。
 */
static void fc_stripe_release_parity(fc_t *controller, fc_command_id_t parity_id)
{
    uint16_t descriptor_index;
    uint16_t next_descriptor_index;

    if (parity_id >= FC_COMMAND_POOL_DEPTH)
    {
        return;
    }

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
    parity_cmd->faa              = data_cmd->faa;

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

    /* 写入 parity descriptors 的 faa（与数据命令对应 CW 一致） */
    {
        fc_descriptor_id_t data_desc_id   = (fc_descriptor_id_t)data_cmd->desc_id;
        fc_descriptor_id_t parity_desc_id = (fc_descriptor_id_t)parity_cmd->desc_id;

        for (cw = 0U; cw < cw_count; cw++)
        {
            fc_descriptor_t *pdesc = fc_descriptor_id_to_descriptor(parity_desc_id);
            fc_descriptor_t *ddesc = fc_descriptor_id_to_descriptor(data_desc_id);
            if ((pdesc != NULL) && (ddesc != NULL))
            {
                pdesc->faa = ddesc->faa;
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
    mp_req.cw_count    = cw_count;
    mp_req.page_type   = (uint8_t)data_cmd->config.page_type;

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

static fc_status_t fc_poll_internal(fc_t *controller)
{
    fc_status_t                result;
    fc_command_id_t            command_id;
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
            if ((completed_id < FC_COMMAND_POOL_DEPTH) &&
                (controller->stripe_parity_for_data[completed_id] != FC_COMMAND_ID_INVALID))
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

    /* 从调度器获取可执行命令（可能为单命令或链）。 */
    fc_scheduler_chain_t sched_chain;
    result = fc_scheduler_select_chain(&controller->scheduler, &sched_chain);
    if (result == FC_STATUS_OK)
    {
        fc_command_t *command;
        uint8_t       i;

        if (sched_chain.count == 0U)
        {
            return FC_STATUS_IO_ERROR;
        }

        /* 单命令：现有路径 */
        if (sched_chain.count == 1U)
        {
            nand_dispatch_request_t request;
            uint8_t                 cw;
            fc_descriptor_id_t      desc_id;

            command_id = sched_chain.ids[0U];
            if ((fc_command_id_is_valid(command_id) == 0U) ||
                (controller->command_state[command_id] != FC_COMMAND_STATE_QUEUED))
            {
                return FC_STATUS_IO_ERROR;
            }
            controller->command_state[command_id] = FC_COMMAND_STATE_ACTIVE;
            command                               = &controller->command_pool[command_id];

            /* ── Data Path encode: Random 加扰 → ECC 编码（RAID XOR 已移至 stripe 路径）──
             * 在 NAND 编程前对 buffer 数据做编码，对 FTL 完全透明。 */
            if (fc_is_page_write((uint8_t)command->config.operation) != 0U)
            {
                /* 在 encode 前将 PAA 写入 CW META 字段 [0..3] */
                {
                    fc_descriptor_id_t mid = (fc_descriptor_id_t)command->desc_id;
                    uint8_t            mw;
                    uint8_t            meta_total = (command->config.multi_plane > 0U)
                                                        ? (uint8_t)(command->config.multi_plane * (uint8_t)(command->config.cw + 1U))
                                                        : (uint8_t)(command->config.cw + 1U);
                    for (mw = 0U; mw < meta_total; mw++)
                    {
                        fc_descriptor_t *md = (mid < FC_DESCRIPTOR_POOL_DEPTH)
                                                  ? &controller->descriptor_pool[mid]
                                                  : NULL;
                        if ((md != NULL) && (md->buffer_id != FC_BUFFER_ID_INVALID))
                        {
                            uint8_t   *cw_base  = fc_buffer_id_to_real_buffer_internal(controller, md->buffer_id);
                            nand_paa_t meta_paa = md->paa;
                            cw_base[0]          = (uint8_t)(meta_paa >> 0U);
                            cw_base[1]          = (uint8_t)(meta_paa >> 8U);
                            cw_base[2]          = (uint8_t)(meta_paa >> 16U);
                            cw_base[3]          = (uint8_t)(meta_paa >> 24U);
                        }
                        mid = (mid < FC_DESCRIPTOR_POOL_DEPTH) ? (fc_descriptor_id_t)controller->descriptor_pool[mid].next_id : FC_DESCRIPTOR_ID_INVALID;
                    }
                }
                (void)fc_data_path_encode(command);
            }

            /* ── Stripe RAID 分支：raid=ENCODE 的 page write 走 Multi-Plane 提交 ── */
#if CONFIG_FC_RAID
            if ((fc_is_page_write((uint8_t)command->config.operation) != 0U) &&
                (command->config.raid == FC_RAID_MODE_ENCODE))
            {
                result = fc_stripe_submit(controller, command, command_id);
                fc_register_queue_state(controller);
                return result; /* 成功/失败均已在 fc_stripe_submit 中处理 */
            }
#endif /* CONFIG_FC_RAID */

            /* Pre-resolve descriptor-chain data into a self-contained dispatch request. */
            request              = (nand_dispatch_request_t){0};
            request.command_id   = (uint16_t)command_id;
            request.operation    = (uint8_t)command->config.operation;
            request.ch           = (uint8_t)command->config.ch;
            request.ce           = (uint8_t)command->config.ce;
            request.lun          = (uint8_t)command->config.lun;
            request.page_type    = (uint8_t)command->config.page_type;
            request.parameter[0] = (uint8_t)command->parameter0;
            request.parameter[1] = (uint8_t)command->parameter1;
            request.parameter[2] = (uint8_t)command->parameter2;
            request.parameter[3] = (uint8_t)command->parameter3;
            request.feature_addr = (uint8_t)command->feature_addr;

            if (nand_operation_has_data((uint8_t)command->config.operation))
            {
                request.cw_count = (uint8_t)(command->config.cw + 1U);
            }
            else
            {
                request.cw_count = ((command->config.operation == OP_ERASE_BLOCK) ||
                                    (command->config.operation == OP_ERASE_BLOCK_MULTI_PLANE))
                                       ? 1U
                                       : 0U;
            }

            if (command->config.operation == OP_ERASE_BLOCK)
            {
                request.cws[0U][0U].paa    = command->paa;
                request.cws[0U][0U].buffer = NULL;
            }
            else if (command->config.operation == OP_ERASE_BLOCK_MULTI_PLANE)
            {
                request.plane_count        = PLANE_NUM;
                request.cw_count           = 1U;
                request.cws[0U][0U].paa    = paa_make(paa_to_page(command->paa), 0U, paa_to_block(command->paa), 0U, 0U);
                request.cws[1U][0U].paa    = paa_make(paa_to_page(command->paa), 1U, paa_to_block(command->paa), 0U, 0U);
                request.cws[0U][0U].buffer = NULL;
                request.cws[1U][0U].buffer = NULL;
            }
            else if ((command->config.operation == OP_SET_FEATURES) ||
                     (command->config.operation == OP_GET_FEATURES))
            {
                request.cws[0U][0U].buffer = (uint8_t *)&command->parameter0;
            }
            else if (request.cw_count > 0U)
            {
                desc_id = (fc_descriptor_id_t)command->desc_id;
                if (command->config.multi_plane > 0U)
                {
                    /* Multi-plane: walk full desc chain, sort into cws[plane][cw] */
                    uint8_t total_cw = (uint8_t)(command->config.multi_plane * request.cw_count);
                    uint8_t idx;
                    for (idx = 0U; idx < total_cw; idx++)
                    {
                        const fc_descriptor_t *descriptor =
                            (desc_id < FC_DESCRIPTOR_POOL_DEPTH) ? &controller->descriptor_pool[desc_id] : NULL;
                        if (descriptor == NULL)
                        {
                            nand_completion                  = (nand_dispatch_completion_t){0};
                            nand_completion.command_id       = command_id;
                            nand_completion.execution_status = NAND_RESULT_NOT_READY;
                            nand_completion.cw_status[0]     = FC_CW_STATUS_NAND_FAIL;
                            (void)fc_complete_from_nand(controller, &nand_completion);
                            fc_register_queue_state(controller);
                            return FC_STATUS_IO_ERROR;
                        }
                        uint8_t plane                = (uint8_t)paa_to_plane(descriptor->paa);
                        uint8_t cw_s                 = (uint8_t)paa_to_cw(descriptor->paa);
                        request.cws[plane][cw_s].paa = descriptor->paa;
                        if (descriptor->buffer_id != FC_BUFFER_ID_INVALID)
                        {
                            /* Page read/write 需要 CW 基址（ECC 在完整 528B CW 上处理）；
                             * 非 page 命令数据直接写入 USER 区域，test/FTL 通过
                             * fc_buffer_id_to_real_buffer 直接读取。 */
                            request.cws[plane][cw_s].buffer =
                                (fc_is_page_read((uint8_t)command->config.operation) != 0U) ||
                                        (fc_is_page_write((uint8_t)command->config.operation) != 0U)
                                    ? fc_buffer_get_base(descriptor->buffer_id)
                                    : fc_buffer_id_to_real_buffer(descriptor->buffer_id);
                        }
                        else
                        {
                            request.cws[plane][cw_s].buffer = NULL;
                        }
                        desc_id = (fc_descriptor_id_t)descriptor->next_id;
                    }
                    request.plane_count = (uint8_t)command->config.multi_plane;
                }
                else
                {
                    /* Single-plane: existing path */
                    for (cw = 0U; cw < request.cw_count; cw++)
                    {
                        const fc_descriptor_t *descriptor =
                            (desc_id < FC_DESCRIPTOR_POOL_DEPTH) ? &controller->descriptor_pool[desc_id] : NULL;

                        if (descriptor == NULL)
                        {
                            nand_completion                  = (nand_dispatch_completion_t){0};
                            nand_completion.command_id       = command_id;
                            nand_completion.execution_status = NAND_RESULT_NOT_READY;
                            nand_completion.cw_status[0]     = FC_CW_STATUS_NAND_FAIL;
                            (void)fc_complete_from_nand(controller, &nand_completion);
                            fc_register_queue_state(controller);
                            return FC_STATUS_IO_ERROR;
                        }
                        request.cws[0][cw].paa = descriptor->paa;
                        if (descriptor->buffer_id != FC_BUFFER_ID_INVALID)
                        {
                            request.cws[0][cw].buffer =
                                (fc_is_page_read((uint8_t)command->config.operation) != 0U) ||
                                        (fc_is_page_write((uint8_t)command->config.operation) != 0U)
                                    ? fc_buffer_get_base(descriptor->buffer_id)
                                    : fc_buffer_id_to_real_buffer(descriptor->buffer_id);
                        }
                        else
                        {
                            request.cws[0][cw].buffer = NULL;
                        }
                        desc_id = (fc_descriptor_id_t)descriptor->next_id;
                    }
                }
            }

            if (nand_dispatch_submit(&request) != NAND_RESULT_OK)
            {
                SSD_LOGW("fc: nand_dispatch_submit failed for op=%u cmd_id=%u cw_count=%u plane_count=%u\r\n",
                         (unsigned)command->config.operation,
                         (unsigned)command_id,
                         (unsigned)request.cw_count,
                         (unsigned)request.plane_count);
                nand_completion                  = (nand_dispatch_completion_t){0};
                nand_completion.command_id       = command_id;
                nand_completion.execution_status = NAND_RESULT_NOT_READY;
                nand_completion.cw_status[0]     = FC_CW_STATUS_NAND_FAIL;
                (void)fc_complete_from_nand(controller, &nand_completion);
                fc_register_queue_state(controller);
                return FC_STATUS_IO_ERROR;
            }
        }
        else
        {
            /* 链式命令（count >= 2）：构建 requests[] → submit_chain */
            uint8_t                 chain_count = sched_chain.count;
            nand_dispatch_request_t reqs[FC_SCHEDULER_CHAIN_MAX];

            for (i = 0U; i < chain_count; i++)
            {
                uint8_t            cw;
                fc_descriptor_id_t desc_id;

                command_id = sched_chain.ids[i];
                if ((fc_command_id_is_valid(command_id) == 0U) ||
                    (controller->command_state[command_id] != FC_COMMAND_STATE_QUEUED))
                {
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

                reqs[i]              = (nand_dispatch_request_t){0};
                reqs[i].command_id   = (uint16_t)command_id;
                reqs[i].operation    = (uint8_t)command->config.operation;
                reqs[i].ch           = (uint8_t)command->config.ch;
                reqs[i].ce           = (uint8_t)command->config.ce;
                reqs[i].lun          = (uint8_t)command->config.lun;
                reqs[i].page_type    = (uint8_t)command->config.page_type;
                reqs[i].parameter[0] = (uint8_t)command->parameter0;
                reqs[i].parameter[1] = (uint8_t)command->parameter1;
                reqs[i].parameter[2] = (uint8_t)command->parameter2;
                reqs[i].parameter[3] = (uint8_t)command->parameter3;
                reqs[i].feature_addr = (uint8_t)command->feature_addr;

                if (nand_operation_has_data((uint8_t)command->config.operation))
                {
                    reqs[i].cw_count = (uint8_t)(command->config.cw + 1U);
                }
                else
                {
                    reqs[i].cw_count = ((command->config.operation == OP_ERASE_BLOCK) ||
                                        (command->config.operation == OP_ERASE_BLOCK_MULTI_PLANE))
                                           ? 1U
                                           : 0U;
                }

                if (command->config.operation == OP_ERASE_BLOCK)
                {
                    reqs[i].cws[0U][0U].paa    = command->paa;
                    reqs[i].cws[0U][0U].buffer = NULL;
                }
                else if (command->config.operation == OP_ERASE_BLOCK_MULTI_PLANE)
                {
                    reqs[i].plane_count        = PLANE_NUM;
                    reqs[i].cw_count           = 1U;
                    reqs[i].cws[0U][0U].paa    = paa_make(paa_to_page(command->paa), 0U, paa_to_block(command->paa), 0U, 0U);
                    reqs[i].cws[1U][0U].paa    = paa_make(paa_to_page(command->paa), 1U, paa_to_block(command->paa), 0U, 0U);
                    reqs[i].cws[0U][0U].buffer = NULL;
                    reqs[i].cws[1U][0U].buffer = NULL;
                }
                else if ((command->config.operation == OP_SET_FEATURES) ||
                         (command->config.operation == OP_GET_FEATURES))
                {
                    reqs[i].cws[0U][0U].buffer = (uint8_t *)&command->parameter0;
                }
                else if (reqs[i].cw_count > 0U)
                {
                    desc_id = (fc_descriptor_id_t)command->desc_id;
                    if (command->config.multi_plane > 0U)
                    {
                        uint8_t total_cw_req = (uint8_t)(command->config.multi_plane * reqs[i].cw_count);
                        uint8_t idx;
                        for (idx = 0U; idx < total_cw_req; idx++)
                        {
                            const fc_descriptor_t *descriptor =
                                (desc_id < FC_DESCRIPTOR_POOL_DEPTH) ? &controller->descriptor_pool[desc_id] : NULL;
                            if (descriptor == NULL)
                            {
                                nand_completion                  = (nand_dispatch_completion_t){0};
                                nand_completion.command_id       = command_id;
                                nand_completion.execution_status = NAND_RESULT_NOT_READY;
                                nand_completion.cw_status[0]     = FC_CW_STATUS_NAND_FAIL;
                                (void)fc_complete_from_nand(controller, &nand_completion);
                                fc_register_queue_state(controller);
                                return FC_STATUS_IO_ERROR;
                            }
                            uint8_t plane                = (uint8_t)paa_to_plane(descriptor->paa);
                            uint8_t cw_s                 = (uint8_t)paa_to_cw(descriptor->paa);
                            reqs[i].cws[plane][cw_s].paa = descriptor->paa;
                            if (descriptor->buffer_id != FC_BUFFER_ID_INVALID)
                            {
                                reqs[i].cws[plane][cw_s].buffer =
                                    (fc_is_page_read((uint8_t)command->config.operation) != 0U) ||
                                            (fc_is_page_write((uint8_t)command->config.operation) != 0U)
                                        ? fc_buffer_get_base(descriptor->buffer_id)
                                        : fc_buffer_id_to_real_buffer(descriptor->buffer_id);
                            }
                            else
                            {
                                reqs[i].cws[plane][cw_s].buffer = NULL;
                            }
                            desc_id = (fc_descriptor_id_t)descriptor->next_id;
                        }
                        reqs[i].plane_count = (uint8_t)command->config.multi_plane;
                    }
                    else
                    {
                        for (cw = 0U; cw < reqs[i].cw_count; cw++)
                        {
                            const fc_descriptor_t *descriptor =
                                (desc_id < FC_DESCRIPTOR_POOL_DEPTH) ? &controller->descriptor_pool[desc_id] : NULL;

                            if (descriptor == NULL)
                            {
                                nand_completion                  = (nand_dispatch_completion_t){0};
                                nand_completion.command_id       = command_id;
                                nand_completion.execution_status = NAND_RESULT_NOT_READY;
                                nand_completion.cw_status[0]     = FC_CW_STATUS_NAND_FAIL;
                                (void)fc_complete_from_nand(controller, &nand_completion);
                                fc_register_queue_state(controller);
                                return FC_STATUS_IO_ERROR;
                            }
                            reqs[i].cws[0][cw].paa = descriptor->paa;
                            if (descriptor->buffer_id != FC_BUFFER_ID_INVALID)
                            {
                                reqs[i].cws[0][cw].buffer =
                                    (fc_is_page_read((uint8_t)command->config.operation) != 0U) ||
                                            (fc_is_page_write((uint8_t)command->config.operation) != 0U)
                                        ? fc_buffer_get_base(descriptor->buffer_id)
                                        : fc_buffer_id_to_real_buffer(descriptor->buffer_id);
                            }
                            else
                            {
                                reqs[i].cws[0][cw].buffer = NULL;
                            }
                            desc_id = (fc_descriptor_id_t)descriptor->next_id;
                        }
                    }
                }
            }

            if (nand_dispatch_submit_chain(reqs, chain_count) != NAND_RESULT_OK)
            {
                for (i = 0U; i < chain_count; i++)
                {
                    nand_completion                  = (nand_dispatch_completion_t){0};
                    nand_completion.command_id       = sched_chain.ids[i];
                    nand_completion.execution_status = NAND_RESULT_NOT_READY;
                    nand_completion.cw_status[0]     = FC_CW_STATUS_NAND_FAIL;
                    (void)fc_complete_from_nand(controller, &nand_completion);
                }
                fc_register_queue_state(controller);
                return FC_STATUS_IO_ERROR;
            }
        }

        /* Submission only occupies the target LUN. Completion is advanced by a later fc_service() call. */
        result = FC_STATUS_OK;
    }
    fc_register_queue_state(controller);
    return result;
}

static fc_status_t fc_complete_internal(fc_t *controller, fc_command_status_t *completion)
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

static fc_status_t fc_poll_completion_internal(fc_t *controller, fc_command_status_t *completion)
{
    fc_status_t result;

    result = fc_completion_queue_pop(&controller->cq, completion);
    fc_register_queue_state(controller);
    return result;
}

static fc_status_t fc_command_release_internal_api(fc_t *controller, fc_command_t *command)
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

fc_status_t fc_init(void)
{
    return fc_init_internal(&g_fc_controller);
}

fc_command_t *fc_command_alloc(void)
{
    return fc_command_alloc_internal(&g_fc_controller);
}

fc_command_t *fc_command_id_to_command(uint16_t command_id)
{
    if (command_id >= FC_COMMAND_POOL_DEPTH)
    {
        return NULL;
    }
    return &g_fc_controller.command_pool[command_id];
}

fc_descriptor_t *fc_descriptor_id_to_descriptor(uint16_t descriptor_id)
{
    if (descriptor_id >= FC_DESCRIPTOR_POOL_DEPTH)
    {
        return NULL;
    }
    return &g_fc_controller.descriptor_pool[descriptor_id];
}

fc_status_t fc_command_configure_descriptors(fc_command_t *command)
{
    fc_command_id_t command_id;

    if ((command == NULL) || (fc_command_to_id(&g_fc_controller, command, &command_id) == 0U) ||
        (g_fc_controller.command_state[command_id] != FC_COMMAND_STATE_ALLOCATED) ||
        (command->desc_id != FC_DESCRIPTOR_ID_INVALID))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    return fc_command_configure_descriptors_internal(&g_fc_controller, command);
}

fc_buffer_id_t fc_buffer_alloc(void)
{
    return fc_buffer_alloc_internal(&g_fc_controller);
}

fc_status_t fc_buffer_release(fc_buffer_id_t buffer_id)
{
    if (fc_buffer_id_is_valid(&g_fc_controller, buffer_id) == 0U)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    return fc_buffer_release_internal_api(&g_fc_controller, buffer_id);
}

uint8_t *fc_buffer_id_to_real_buffer(fc_buffer_id_t buffer_id)
{
    uint8_t *base;

    if (fc_buffer_id_is_valid(&g_fc_controller, buffer_id) == 0U)
    {
        return NULL;
    }
    base = fc_buffer_id_to_real_buffer_internal(&g_fc_controller, buffer_id);
    if (base == NULL)
    {
        return NULL;
    }
    /* 对外只暴露 USER 数据区，跳过 META(4) + CRC(4) */
    return base + FC_BUFFER_USER_OFFSET;
}

uint8_t *fc_buffer_get_base(fc_buffer_id_t buffer_id)
{
    if (fc_buffer_id_is_valid(&g_fc_controller, buffer_id) == 0U)
    {
        return NULL;
    }
    return fc_buffer_id_to_real_buffer_internal(&g_fc_controller, buffer_id);
}

uint8_t *fc_buffer_get_meta(fc_command_t *command, uint8_t cw_index)
{
    if (command == NULL)
    {
        return NULL;
    }
    fc_descriptor_id_t did = (fc_descriptor_id_t)command->desc_id;
    uint8_t            idx = 0U;
    while (did != FC_DESCRIPTOR_ID_INVALID)
    {
        if (idx == cw_index)
        {
            fc_descriptor_t *d = (did < FC_DESCRIPTOR_POOL_DEPTH) ? &g_fc_controller.descriptor_pool[did] : NULL;
            if (d == NULL)
            {
                return NULL;
            }
            return (d->buffer_id != FC_BUFFER_ID_INVALID)
                       ? fc_buffer_id_to_real_buffer_internal(&g_fc_controller, d->buffer_id)
                       : NULL;
        }
        did = (did < FC_DESCRIPTOR_POOL_DEPTH) ? (fc_descriptor_id_t)g_fc_controller.descriptor_pool[did].next_id : FC_DESCRIPTOR_ID_INVALID;
        idx++;
    }
    return NULL;
}

fc_buffer_id_t fc_real_buffer_to_buffer_id(const uint8_t *buffer)
{
    if (buffer == NULL)
    {
        return FC_BUFFER_ID_INVALID;
    }
    /* 外部拿到的是 USER 区域指针，回退 8 字节到 CW 基址再比对 */
    return fc_real_buffer_to_buffer_id_internal(&g_fc_controller, buffer - FC_BUFFER_USER_OFFSET);
}

fc_status_t fc_write_cmd_data(fc_command_t *command, const void *data, uint32_t length)
{
    fc_command_id_t    command_id;
    fc_descriptor_id_t desc_id;
    uint8_t            cw_count;
    uint8_t            i;
    const uint8_t     *src;

    if ((command == NULL) || (data == NULL) ||
        (fc_command_to_id(&g_fc_controller, command, &command_id) == 0U) ||
        (g_fc_controller.command_state[command_id] != FC_COMMAND_STATE_ALLOCATED) ||
        (command->desc_id == FC_DESCRIPTOR_ID_INVALID))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    cw_count = (uint8_t)(command->config.cw + 1U);
    src      = (const uint8_t *)data;
    desc_id  = (fc_descriptor_id_t)command->desc_id;

    for (i = 0U; i < cw_count; i++)
    {
        fc_descriptor_t *descriptor;
        uint8_t         *buf;
        uint32_t         copy_len;

        if (desc_id >= FC_DESCRIPTOR_POOL_DEPTH)
        {
            return FC_STATUS_INVALID_ARGUMENT;
        }
        descriptor = &g_fc_controller.descriptor_pool[desc_id];
        if (descriptor->buffer_id == FC_BUFFER_ID_INVALID)
        {
            return FC_STATUS_INVALID_ARGUMENT;
        }
        buf = fc_buffer_id_to_real_buffer(descriptor->buffer_id);

        if (length > 0U)
        {
            copy_len = (length >= FC_BUFFER_USER_SIZE) ? FC_BUFFER_USER_SIZE : length;
            (void)memcpy(buf, src, copy_len);
            src += copy_len;
            length -= copy_len;

            /* 当前 buffer 未填满部分用 0xFF dummy 填充 */
            if (copy_len < FC_BUFFER_USER_SIZE)
            {
                (void)memset(buf + copy_len, 0xFF, (size_t)(FC_BUFFER_USER_SIZE - copy_len));
            }
        }
        else
        {
            /* 数据已耗尽，整 buffer 填充 0xFF dummy */
            (void)memset(buf, 0xFF, FC_BUFFER_POOL_CW_SIZE);
        }

        desc_id = (fc_descriptor_id_t)descriptor->next_id;
    }

    return FC_STATUS_OK;
}

uint8_t *fc_get_cmd_data_ptr(fc_command_t *command, uint8_t cw_index)
{
    fc_command_id_t    command_id;
    fc_descriptor_id_t desc_id;
    uint8_t            i;

    if ((command == NULL) ||
        (fc_command_to_id(&g_fc_controller, command, &command_id) == 0U) ||
        (command->desc_id == FC_DESCRIPTOR_ID_INVALID) ||
        (cw_index > command->config.cw))
    {
        return NULL;
    }

    desc_id = (fc_descriptor_id_t)command->desc_id;
    for (i = 0U; i <= cw_index; i++)
    {
        fc_descriptor_t *descriptor;

        if (desc_id >= FC_DESCRIPTOR_POOL_DEPTH)
        {
            return NULL;
        }
        descriptor = &g_fc_controller.descriptor_pool[desc_id];
        if (i == cw_index)
        {
            if (descriptor->buffer_id == FC_BUFFER_ID_INVALID)
            {
                return NULL;
            }
            return fc_buffer_id_to_real_buffer(descriptor->buffer_id);
        }
        desc_id = (fc_descriptor_id_t)descriptor->next_id;
    }

    return NULL;
}

fc_status_t fc_submit(fc_command_t *command)
{
    fc_command_id_t command_id;

    if ((command == NULL) || (fc_command_to_id(&g_fc_controller, command, &command_id) == 0U) ||
        (g_fc_controller.command_state[command_id] != FC_COMMAND_STATE_ALLOCATED) ||
        (fc_command_validate(command) != FC_STATUS_OK) ||
        (fc_command_descriptors_owned(&g_fc_controller, command_id, command) == 0U))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    return fc_submit_internal(&g_fc_controller, command);
}

fc_status_t fc_service(void)
{
    return fc_poll_internal(&g_fc_controller);
}

fc_status_t fc_poll_completion(fc_command_status_t *completion)
{
    if (completion == NULL)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    return fc_poll_completion_internal(&g_fc_controller, completion);
}

fc_status_t fc_command_release(fc_command_t *command)
{
    fc_command_id_t command_id;

    if (command == NULL)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    command_id = command->command_id;
    if ((command_id >= FC_COMMAND_POOL_DEPTH) ||
        (&g_fc_controller.command_pool[command_id] != command))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    if ((g_fc_controller.command_state[command_id] != FC_COMMAND_STATE_ALLOCATED) &&
        (g_fc_controller.command_state[command_id] != FC_COMMAND_STATE_COMPLETED))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    if (fc_cq_contains_command(&g_fc_controller, command) != 0U)
    {
        return FC_STATUS_NOT_READY;
    }
    return fc_command_release_internal_api(&g_fc_controller, command);
}

fc_status_t fc_get_resource_stats(fc_resource_stats_t *stats)
{
    uint16_t index;

    if (stats == NULL)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    stats->sq_used         = g_fc_controller.sq.count;
    stats->hsq_used        = g_fc_controller.hsq.count;
    stats->cq_used         = g_fc_controller.cq.count;
    stats->command_used    = 0U;
    stats->descriptor_used = 0U;
    stats->buffer_used     = 0U;
    for (index = 0U; index < FC_COMMAND_POOL_DEPTH; index++)
    {
        if (g_fc_controller.command_state[index] != FC_COMMAND_STATE_FREE)
        {
            stats->command_used++;
        }
    }
    for (index = 0U; index < FC_DESCRIPTOR_POOL_DEPTH; index++)
    {
        if (g_fc_controller.descriptor_owner[index] != FC_COMMAND_ID_INVALID)
        {
            stats->descriptor_used++;
        }
    }
    for (index = 0U; index < FC_BUFFER_POOL_DEPTH; index++)
    {
        if (g_fc_controller.buffer_state[index] != 0U)
        {
            stats->buffer_used++;
        }
    }
    return FC_STATUS_OK;
}

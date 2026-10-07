/*
 * fc_interface.c — FC 对外接口实现（单例 g_fc_controller 的薄壳）
 *
 * 职责：
 *   只做参数校验 + 转发到 fc.c 的 fc_*_internal()（参数化控制器实现）。
 *   不含任何池/调度/数据路径逻辑——那些留在 fc.c（核心）中。
 *
 * 与 fc.c 的分工：
 *   fc.c            ：核心实现（池、生命周期状态机、服务主循环 fc_poll_internal、
 *                     完成回写 fc_complete_from_nand、Stripe RAID）——全部 internal。
 *   fc_interface.c  ：对外 API 薄壳，固定使用单例 g_fc_controller。
 *   fc_internal.h   ：两个文件共享的 internal 签名（FTL 不得 include）。
 *
 * 设计说明（为什么薄壳单独成文件）：
 *   - fc.c 专注核心路径，对外入口一目了然（本文件）。
 *   - 对外 API 全部是无状态转发 + 参数校验，无逻辑，可读性集中于一处。
 *   - 新增对外接口 = 改 fc.h（或 fc_interface.h）+ 本文件，不影响核心。
 */

#include "fc_interface.h"

#include "common/assertion.h"
#include "fc_command.h"
#include "fc_internal.h"

#include <stddef.h>

/* 命令指针 → 池索引（指针算术，前提：指针属于 command_pool） */
static uint8_t fc_command_to_id(const fc_command_t *command, fc_command_id_t *command_id)
{
    uint16_t index;

    /* 层内 helper：调用者（fc_command_configure_descriptors / fc_submit）已判过
     * command 非空，command_id 是本文件局部地址——传空是调用者编程错误。 */
    ASSERT(command != NULL);
    ASSERT(command_id != NULL);

    for (index = 0U; index < FC_COMMAND_POOL_DEPTH; index++)
    {
        if (command == fc_command_id_to_command(index))
        {
            *command_id = (fc_command_id_t)index;
            return 1U;
        }
    }
    return 0U;
}

/* buffer ID 有效性（< 池深度且已占用） */
static uint8_t fc_buffer_id_is_valid(fc_buffer_id_t buffer_id)
{
    return (uint8_t)(buffer_id < FC_BUFFER_POOL_DEPTH);
}

/* 是否占用（供 fc_buffer_release / id_to_real_buffer 校验） */
static uint8_t fc_buffer_is_occupied(fc_buffer_id_t buffer_id)
{
    return (fc_buffer_id_is_valid(buffer_id) != 0U) &&
           (fc_buffer_state_get(&g_fc_controller, buffer_id) != 0U);
}

/* ══════════════════════════════════════════════════════════
 * ① 初始化
 * ══════════════════════════════════════════════════════════ */

/* 初始化全局控制器（幂等，可重复调用以重置） */
fc_status_t fc_init(void)
{
    return fc_init_internal(&g_fc_controller);
}

/* ══════════════════════════════════════════════════════════
 * ② 命令生命周期
 * ══════════════════════════════════════════════════════════ */

/* 分配命令槽位 */
fc_command_t *fc_command_alloc(void)
{
    return fc_command_alloc_internal(&g_fc_controller);
}

/* 配置命令 desc 链（仅 ALLOCATED 且尚未配置时合法） */
fc_status_t fc_command_configure_descriptors(fc_command_t *command)
{
    fc_command_id_t command_id;

    /* command 非空是前置条件；指针身份 + 生命周期状态是文档化入口契约，保留返回码 */
    ASSERT(command != NULL);
    if ((fc_command_to_id(command, &command_id) == 0U) ||
        (fc_command_state_get(&g_fc_controller, command_id) != FC_COMMAND_STATE_ALLOCATED) ||
        (fc_command_id_to_command(command_id)->desc_id != FC_DESCRIPTOR_ID_INVALID))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    return fc_command_configure_descriptors_internal(&g_fc_controller, command);
}

/* 逐条追加一个 (plane, cw) 到命令 desc 链尾（仅 ALLOCATED 时合法）。
 * 与 fc_command_configure_descriptors 并列：后者按掩码一次性建链（策略），
 * 本函数把"机制"（分配 desc+buffer + 挂链）交给上层自行组装。
 * row_paa 由调用者按 plane 给出，故 multi-plane 命令两个 plane 可指向不同 block。 */
fc_status_t fc_command_attach_cw(fc_command_t *command, uint8_t plane, uint8_t cw, nand_paa_t row_paa)
{
    fc_command_id_t command_id;

    /* command 非空是前置条件；指针身份 + 生命周期状态是文档化入口契约，保留返回码 */
    ASSERT(command != NULL);
    if ((fc_command_to_id(command, &command_id) == 0U) ||
        (fc_command_state_get(&g_fc_controller, command_id) != FC_COMMAND_STATE_ALLOCATED))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    return fc_command_attach_cw_internal(&g_fc_controller, command, plane, cw, row_paa);
}

/* --------------------------------------------------------------------
 * fc_submit
 *   功能：命令投递到 SQ/HSQ（公共接口）。
 *   参数：command —— 已配置好的命令。
 *   返回：FC_STATUS_OK / INVALID_ARGUMENT / QUEUE_FULL。
 *   实现要点：状态须 ALLOCATED、validate 通过、desc 链所有权完整。
 * -------------------------------------------------------------------- */
fc_status_t fc_submit(fc_command_t *command)
{
    fc_command_id_t command_id;

    /* command 非空是前置条件；指针身份 + 生命周期 + 校验结果是文档化入口契约，保留返回码 */
    ASSERT(command != NULL);
    if ((fc_command_to_id(command, &command_id) == 0U) ||
        (fc_command_state_get(&g_fc_controller, command_id) != FC_COMMAND_STATE_ALLOCATED) ||
        (fc_command_validate(command) != FC_STATUS_OK) ||
        (fc_command_descriptors_owned_internal(&g_fc_controller, command_id, command) == 0U))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    return fc_submit_internal(&g_fc_controller, command);
}

/* --------------------------------------------------------------------
 * fc_service
 *   功能：服务主循环单步（公共入口）。
 *   返回：FC_STATUS_OK / IO_ERROR / NOT_READY（无命令可推进）。
 * -------------------------------------------------------------------- */
fc_status_t fc_service(void)
{
    return fc_poll_internal(&g_fc_controller);
}

/* 从 CQ 取完成项 */
fc_status_t fc_poll_completion(fc_command_status_t *completion)
{
    /* completion 为出参前置条件 */
    ASSERT(completion != NULL);
    return fc_poll_completion_internal(&g_fc_controller, completion);
}

/* --------------------------------------------------------------------
 * fc_command_release
 *   功能：释放命令全部资源（公共接口）。
 *   参数：command —— 命令。
 *   返回：FC_STATUS_OK / INVALID_ARGUMENT / NOT_READY（仍在 CQ 中）。
 *   实现要点：仅 ALLOCATED 或 COMPLETED 可释放；命令须不在 CQ 中
 *         （FTL 应已 poll_completion 取走完成项）。
 * -------------------------------------------------------------------- */
fc_status_t fc_command_release(fc_command_t *command)
{
    fc_command_id_t command_id;

    /* command 非空是前置条件；指针身份校验是文档化入口契约（辨析 D），保留返回码 */
    ASSERT(command != NULL);

    command_id = command->command_id;
    if ((command_id >= FC_COMMAND_POOL_DEPTH) ||
        (fc_command_id_to_command(command_id) != command))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    return fc_command_release_internal_api(&g_fc_controller, command);
}

/* ══════════════════════════════════════════════════════════
 * ③ buffer 访问
 * ══════════════════════════════════════════════════════════ */

/* 分配 buffer */
fc_buffer_id_t fc_buffer_alloc(void)
{
    return fc_buffer_alloc_internal(&g_fc_controller);
}

/* 释放 buffer（须已占用） */
fc_status_t fc_buffer_release(fc_buffer_id_t buffer_id)
{
    if (fc_buffer_is_occupied(buffer_id) == 0U)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    return fc_buffer_release_internal(&g_fc_controller, buffer_id);
}

/* 对外：返回 USER 数据区指针（跳过 META+CRC 8 字节） */
uint8_t *fc_buffer_id_to_real_buffer(fc_buffer_id_t buffer_id)
{
    uint8_t *base;

    if (fc_buffer_is_occupied(buffer_id) == 0U)
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

/* FC 内部：返回 CW 基址（528B，含 META/CRC/PARITY，数据路径用） */
uint8_t *fc_buffer_get_base(fc_buffer_id_t buffer_id)
{
    if (fc_buffer_is_occupied(buffer_id) == 0U)
    {
        return NULL;
    }
    return fc_buffer_id_to_real_buffer_internal(&g_fc_controller, buffer_id);
}

/* 返回命令中 **CW 号 == cw_index** 的 CW 的 META 指针（校验物理地址用）。
 * 按 paa_to_cw(desc->paa) 匹配，不是链走位——flat CW 掩码下两者会分叉。 */
uint8_t *fc_buffer_get_meta(fc_command_t *command, uint8_t cw_index)
{
    fc_descriptor_id_t did;

    /* command 非空是前置条件（gc.c 传入有效命令）；未找到 CW 仍按契约返回 NULL */
    ASSERT(command != NULL);

    did = (fc_descriptor_id_t)command->desc_id;
    while (did < FC_DESCRIPTOR_POOL_DEPTH)
    {
        fc_descriptor_t *d = fc_descriptor_id_to_descriptor(did);

        if ((uint8_t)paa_to_cw(d->paa) == cw_index)
        {
            return (d->buffer_id != FC_BUFFER_ID_INVALID)
                       ? fc_buffer_id_to_real_buffer_internal(&g_fc_controller, d->buffer_id)
                       : NULL;
        }
        did = (fc_descriptor_id_t)d->next_id;
    }
    return NULL;
}

/* ══════════════════════════════════════════════════════════
 * ④ 数据便利
 * ══════════════════════════════════════════════════════════ */

/* 便利：把外部数据按 CW 顺序写入命令 buffer（不足填 0xFF dummy） */
fc_status_t fc_write_cmd_data(fc_command_t *command, const void *data, uint32_t length)
{
    fc_descriptor_id_t desc_id;
    const uint8_t     *src;

    /* 入参前置条件由调用者保证（FTL 传入已配置 desc 链的命令） */
    ASSERT(command != NULL);
    ASSERT(data != NULL);
    ASSERT(command->desc_id != FC_DESCRIPTOR_ID_INVALID);

    /* 遍历界走 desc 链（链长度是 CW 数的唯一真值，见 fc_command.h） */
    src     = (const uint8_t *)data;
    desc_id = (fc_descriptor_id_t)command->desc_id;

    while (desc_id < FC_DESCRIPTOR_POOL_DEPTH)
    {
        fc_descriptor_t *descriptor;
        uint8_t         *buf;
        uint32_t         copy_len;

        /* desc 链由 fc_command_configure_descriptors 构建，ID 必在池内 */
        ASSERT(desc_id < FC_DESCRIPTOR_POOL_DEPTH);
        descriptor = fc_descriptor_id_to_descriptor(desc_id);
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

/* --------------------------------------------------------------------
 * fc_read_cmd_data
 *   功能：便利——从命令 buffer 按 CW 顺序读取用户数据到外部缓冲。
 *   参数：command —— 命令；data —— 输出；length —— 字节数。
 *   返回：FC_STATUS_OK / INVALID_ARGUMENT / IO_ERROR。
 * -------------------------------------------------------------------- */
fc_status_t fc_read_cmd_data(fc_command_t *command, void *data, uint32_t length)
{
    fc_descriptor_id_t desc_id;
    uint8_t           *dst;
    uint32_t           remaining;

    /* 入参前置条件由调用者保证（FTL 传入已配置 desc 链的命令） */
    ASSERT(command != NULL);
    ASSERT(data != NULL);
    ASSERT(command->desc_id != FC_DESCRIPTOR_ID_INVALID);

    /* 遍历界走 desc 链（链长度是 CW 数的唯一真值，见 fc_command.h） */
    dst       = (uint8_t *)data;
    remaining = length;
    desc_id   = (fc_descriptor_id_t)command->desc_id;

    while ((desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (remaining > 0U))
    {
        fc_descriptor_t *descriptor;
        uint8_t         *buf;

        /* desc 链由 fc_command_configure_descriptors 构建，ID 必在池内 */
        ASSERT(desc_id < FC_DESCRIPTOR_POOL_DEPTH);
        descriptor = fc_descriptor_id_to_descriptor(desc_id);
        if (descriptor->buffer_id == FC_BUFFER_ID_INVALID)
        {
            return FC_STATUS_IO_ERROR;
        }

        buf = fc_buffer_id_to_real_buffer(descriptor->buffer_id);
        if (buf == NULL)
        {
            return FC_STATUS_IO_ERROR;
        }

        {
            uint32_t copy_len = (remaining >= FC_BUFFER_USER_SIZE) ? FC_BUFFER_USER_SIZE : remaining;
            (void)memcpy(dst, buf, copy_len);
            dst += copy_len;
            remaining -= copy_len;
        }

        desc_id = (fc_descriptor_id_t)descriptor->next_id;
    }

    return FC_STATUS_OK;
}

/* --------------------------------------------------------------------
 * fc_get_cmd_data_ptr
 *   功能：获取命令中 **CW 号 == cw_index** 的那个 CW 的 USER 数据指针。
 *   参数：command —— 命令；cw_index —— CW 号（0..CW_NUM-1）。
 *   返回：USER 指针 / NULL（该 CW 不在链上或 buffer 无效）。
 *   实现要点：按 `paa_to_cw(desc->paa)` 匹配，**不是**链走位——flat CW 掩码下
 *         两者会分叉（见 fc_command.h 不变量 2）。uniform 命令下等价。
 * -------------------------------------------------------------------- */
uint8_t *fc_get_cmd_data_ptr(fc_command_t *command, uint8_t cw_index)
{
    fc_descriptor_id_t desc_id;

    /* command/desc 链非空是前置条件；cw_index 不在链上仍按查询语义返回 NULL */
    ASSERT(command != NULL);
    ASSERT(command->desc_id != FC_DESCRIPTOR_ID_INVALID);

    desc_id = (fc_descriptor_id_t)command->desc_id;
    while (desc_id < FC_DESCRIPTOR_POOL_DEPTH)
    {
        fc_descriptor_t *descriptor = fc_descriptor_id_to_descriptor(desc_id);

        if ((uint8_t)paa_to_cw(descriptor->paa) == cw_index)
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

/* --------------------------------------------------------------------
 * fc_get_cmd_ecc_max
 *   功能：命令级聚合 ECC——desc 链上所有 CW 的 ecc_bits 最大值。
 *   参数：command —— 已完成的命令（须仍持有 desc 链）。
 *   返回：最大纠正 bit 数；无 desc 时 0。
 *   实现要点：**取全链**（含两个 plane）——fc_complete_from_nand 的 per-CW ECC
 *         只对 plane 0 折叠，flat 掩码下"只取 plane 0"会恒为 0；
 *         desc->ecc_bits 由该函数对链上**每个** desc 填写，全链取值无损。
 *         调用者须在 fc_command_release() 之前调用（desc 释放后失效）。
 * -------------------------------------------------------------------- */
uint8_t fc_get_cmd_ecc_max(const fc_command_t *command)
{
    fc_descriptor_id_t desc_id;
    uint8_t            ecc_max = 0U;

    ASSERT(command != NULL);

    desc_id = (fc_descriptor_id_t)command->desc_id;
    while (desc_id < FC_DESCRIPTOR_POOL_DEPTH)
    {
        const fc_descriptor_t *descriptor = fc_descriptor_id_to_descriptor(desc_id);

        if (descriptor->ecc_bits > ecc_max)
        {
            ecc_max = (uint8_t)descriptor->ecc_bits;
        }
        desc_id = (fc_descriptor_id_t)descriptor->next_id;
    }
    return ecc_max;
}

/* ══════════════════════════════════════════════════════════
 * ⑤ 查询工具
 * ══════════════════════════════════════════════════════════ */

/* 命令 ID → 命令指针（ID 恒为池内 ID，越界是调用者编程错误） */
fc_command_t *fc_command_id_to_command(uint16_t command_id)
{
    /* command_id 只来自 fc_command_alloc() 铸出的 ID（队列里只存分配产出的 ID），
     * 任何调用者都不可能传入越界值——越界即不变量破坏，留现场。 */
    ASSERT(command_id < FC_COMMAND_POOL_DEPTH);
    return fc_command_id_to_command_internal(&g_fc_controller, command_id);
}

/* 描述符 ID → 描述符指针（ID 恒为池内 ID，越界是调用者编程错误） */
fc_descriptor_t *fc_descriptor_id_to_descriptor(uint16_t descriptor_id)
{
    /* descriptor_id 只来自命令 desc 链（configure_descriptors 铸出），恒在池内。 */
    ASSERT(descriptor_id < FC_DESCRIPTOR_POOL_DEPTH);
    return fc_descriptor_id_to_descriptor_internal(&g_fc_controller, descriptor_id);
}

/* --------------------------------------------------------------------
 * fc_get_resource_stats
 *   功能：统计当前各资源池使用量（监控/测试）。
 *   参数：stats —— 输出。
 *   返回：FC_STATUS_OK / INVALID_ARGUMENT。
 * -------------------------------------------------------------------- */
fc_status_t fc_get_resource_stats(fc_resource_stats_t *stats)
{
    uint16_t index;

    /* stats 为出参前置条件（调用者恒传有效指针） */
    ASSERT(stats != NULL);

    stats->sq_used         = fc_get_sq_count(&g_fc_controller);
    stats->hsq_used        = fc_get_hsq_count(&g_fc_controller);
    stats->cq_used         = fc_get_cq_count(&g_fc_controller);
    stats->command_used    = 0U;
    stats->descriptor_used = 0U;
    stats->buffer_used     = 0U;
    for (index = 0U; index < FC_COMMAND_POOL_DEPTH; index++)
    {
        if (fc_command_state_get(&g_fc_controller, index) != FC_COMMAND_STATE_FREE)
        {
            stats->command_used++;
        }
    }
    for (index = 0U; index < FC_DESCRIPTOR_POOL_DEPTH; index++)
    {
        if (fc_descriptor_owner_get(&g_fc_controller, index) != FC_COMMAND_ID_INVALID)
        {
            stats->descriptor_used++;
        }
    }
    for (index = 0U; index < FC_BUFFER_POOL_DEPTH; index++)
    {
        if (fc_buffer_state_get(&g_fc_controller, index) != 0U)
        {
            stats->buffer_used++;
        }
    }
    return FC_STATUS_OK;
}

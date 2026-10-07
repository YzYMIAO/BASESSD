#include "ftl/ftl_fc_interface.h"

#include "common/assertion.h"
#include "fc/fc.h"
#include "ftl/completion_table.h"
#include "ftl/ftl_type.h"
#include "nand/onfi_1_0.h"

#include <string.h>

/* --------------------------------------------------------------------
 * ftl_cmd_configure_descriptors
 *   功能：按 cmd->param.cw_mask 组装整条 desc 链（v13 由 FC 策略层上移）。
 *   参数：cmd —— 命令（须已设 config.operation / multi_plane / cw / paa）。
 *   返回：FTL_OK / FTL_ERROR（掩码越出本命令 plane 段、池空或坐标非法）。
 *   实现要点：
 *     - 无数据操作（ERASE / RESET 等）与 SET/GET_FEATURES 不建链，desc_id 置 INVALID。
 *     - cw_mask = 0 → 本命令每个 plane 取连续 0..config.cw；非 0 → 必须落在本命令
 *       plane 段内（越段 = 编程错误）。离散读 plane1 的 CW0/CW2 即
 *       cmd->param.cw_mask = (1U << 4) | (1U << 6)。
 *     - 行地址所有 plane 共用 cmd->paa（同页语义）；两 plane 不同块（Policy B）
 *       走 ftl_cmd_setup_ppa_pair()，本函数不适用。
 *     - 半成品链由调用者 fc_command_release 统一回收。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_cmd_configure_descriptors(fc_command_t *cmd)
{
    uint32_t mask;
    uint32_t allowed;
    uint8_t  plane_count;
    uint8_t  plane_first;
    uint8_t  plane_last;
    uint8_t  plane;
    uint8_t  cw;

    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    if (ftl_cmd_needs_desc_chain(cmd) == 0U)
    {
        cmd->desc_id = FC_DESCRIPTOR_ID_INVALID;
        return FTL_OK;
    }

    plane_count = ftl_cmd_plane_span(cmd, &plane_first, &plane_last);
    allowed     = ((uint32_t)((1U << (plane_count * CW_NUM)) - 1U)) << ((uint32_t)plane_first * CW_NUM);

    mask = cmd->param.cw_mask;
    if (mask == 0U)
    {
        const uint32_t per_plane = (uint32_t)((1U << (cmd->config.cw + 1U)) - 1U);

        for (plane = plane_first; plane < plane_last; plane++)
        {
            mask |= per_plane << ((uint32_t)plane * CW_NUM);
        }
    }
    else if ((mask & ~allowed) != 0U)
    {
        return FTL_ERROR;
    }

    for (plane = plane_first; plane < plane_last; plane++)
    {
        const nand_paa_t row_paa = paa_make(paa_to_page(cmd->paa), plane, paa_to_block(cmd->paa), paa_to_lun(cmd->paa), 0U);

        for (cw = 0U; cw < CW_NUM; cw++)
        {
            if ((mask & ((uint32_t)1U << ((uint32_t)plane * CW_NUM + cw))) == 0U)
            {
                continue;
            }
            if (fc_command_attach_cw(cmd, plane, cw, row_paa) != FC_STATUS_OK)
            {
                return FTL_ERROR;
            }
        }
    }

    return FTL_OK;
}

ftl_status_t ftl_cmd_setup_ppa(fc_command_t *cmd, nand_pba_t pba)
{
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    cmd->config.ch  = (DWORD)pba_to_ch(pba);
    cmd->config.ce  = (DWORD)pba_to_ce(pba);
    cmd->config.lun = (DWORD)pba_to_lun(pba);
    cmd->paa        = pba_to_paa(pba);

    return ftl_cmd_configure_descriptors(cmd);
}

ftl_status_t ftl_cmd_setup_ppa_pair(fc_command_t *cmd, const nand_pba_t pba[PLANE_NUM])
{
    uint8_t plane_count;
    uint8_t i;

    if ((cmd == NULL) || (pba == NULL))
    {
        return FTL_ERROR;
    }

    /* 拓扑取 pba[0]（同一命令两 plane 必属同一 ch/ce/lun）；command->paa 也写 pba[0]——
     * 它是 ftl_cmd_plane_span 定 plane 段的输入，各 plane 的实际行地址逐条由 row 决定。 */
    cmd->config.ch  = (DWORD)pba_to_ch(pba[0]);
    cmd->config.ce  = (DWORD)pba_to_ce(pba[0]);
    cmd->config.lun = (DWORD)pba_to_lun(pba[0]);
    cmd->paa        = pba_to_paa(pba[0]);

    if (ftl_cmd_needs_desc_chain(cmd) == 0U)
    {
        cmd->desc_id = FC_DESCRIPTOR_ID_INVALID;
        return FTL_OK;
    }

    plane_count = (cmd->config.multi_plane > 0U) ? (uint8_t)cmd->config.multi_plane : 1U;

    for (i = 0U; i < plane_count; i++)
    {
        /* ⚠ plane 号取自 PBA 自己的 plane 位，不是下标 i：读路径的 pba[] 是按 LBA 查出来的，
         *   plane 取决于当初怎么写进去（`wr 0 2` 之后 lba 1 映射到 plane 1），下标与 plane 号
         *   并不总相等。链序仍按 pba[] 顺序 → ftl_cmd_read_data 拷出的布局与调用者期望一致。 */
        const uint8_t    plane = (uint8_t)pba_to_plane(pba[i]);
        const nand_paa_t row   = pba_to_paa(pba[i]);
        uint8_t          cw;

        for (cw = 0U; cw <= (uint8_t)cmd->config.cw; cw++)
        {
            if (fc_command_attach_cw(cmd, plane, cw, row) != FC_STATUS_OK)
            {
                /* 半成品链由调用者 fc_command_release 统一回收 */
                return FTL_ERROR;
            }
        }
    }

    return FTL_OK;
}

/* --------------------------------------------------------------------
 * ftl_cmd_write_data
 *   功能：把外部数据按 CW 顺序写入命令 desc 链上各 CW 的 USER 区。
 *   参数：cmd —— 已建链的命令；data —— 源数据；length —— 字节数。
 *   返回：FTL_OK / FTL_ERROR（链上 desc 无 buffer）。
 *   实现要点（v13 由 FC 上移：硬件 FCC 不提供「按 CW 拷贝」寄存器，数据由固件填）：
 *     - 遍历界走 desc 链（链长度是 CW 数的唯一真值），链序即数据布局顺序。
 *     - 数据耗尽后剩余 CW 的 USER 区填 0xFF dummy（不把上一轮的残值写进 NAND）。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_cmd_write_data(fc_command_t *cmd, const void *data, uint32_t length)
{
    fc_descriptor_id_t desc_id;
    const uint8_t     *src;

    ASSERT(cmd != NULL);
    ASSERT(data != NULL);
    ASSERT(cmd->desc_id != FC_DESCRIPTOR_ID_INVALID);

    src     = (const uint8_t *)data;
    desc_id = (fc_descriptor_id_t)cmd->desc_id;

    while (desc_id < FC_DESCRIPTOR_POOL_DEPTH)
    {
        fc_descriptor_t *descriptor = fc_descriptor_id_to_descriptor(desc_id);
        uint8_t         *buf;

        if (descriptor->buffer_id == FC_BUFFER_ID_INVALID)
        {
            return FTL_ERROR;
        }
        buf = fc_buffer_id_to_real_buffer(descriptor->buffer_id);

        if (length > 0U)
        {
            uint32_t copy_len = (length >= FC_BUFFER_USER_SIZE) ? FC_BUFFER_USER_SIZE : length;

            (void)memcpy(buf, src, copy_len);
            src += copy_len;
            length -= copy_len;

            /* 本 CW 未填满部分用 0xFF dummy 补齐 */
            if (copy_len < FC_BUFFER_USER_SIZE)
            {
                (void)memset(buf + copy_len, 0xFF, (size_t)(FC_BUFFER_USER_SIZE - copy_len));
            }
        }
        else
        {
            /* 数据已耗尽：本 CW 的 USER 区整体填 0xFF dummy。
             * ⚠ 长度必须是 USER 区大小——buf 是 base+FC_BUFFER_USER_OFFSET，
             *   照旧代码用 FC_BUFFER_POOL_CW_SIZE 会越界 8B 踩到下一 CW 的 META/CRC。 */
            (void)memset(buf, 0xFF, (size_t)FC_BUFFER_USER_SIZE);
        }

        desc_id = (fc_descriptor_id_t)descriptor->next_id;
    }

    return FTL_OK;
}

/* --------------------------------------------------------------------
 * ftl_cmd_read_data
 *   功能：把命令 desc 链上各 CW 的 USER 区按链序拷出到外部缓冲。
 *   参数：cmd —— 已完成的命令；buffer —— 输出；length —— 字节数。
 *   返回：FTL_OK / FTL_ERROR（链上 desc 无 buffer）。
 *   实现要点（v13 由 FC 上移，与 ftl_cmd_write_data 对称）：
 *     - 遍历界走 desc 链；数据够即停（remaining 归零）。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_cmd_read_data(fc_command_t *cmd, void *buffer, uint32_t length)
{
    fc_descriptor_id_t desc_id;
    uint8_t           *dst;
    uint32_t           remaining;

    ASSERT(cmd != NULL);
    ASSERT(buffer != NULL);
    ASSERT(cmd->desc_id != FC_DESCRIPTOR_ID_INVALID);

    dst       = (uint8_t *)buffer;
    remaining = length;
    desc_id   = (fc_descriptor_id_t)cmd->desc_id;

    while ((desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (remaining > 0U))
    {
        fc_descriptor_t *descriptor = fc_descriptor_id_to_descriptor(desc_id);
        uint8_t         *buf;
        uint32_t         copy_len;

        if (descriptor->buffer_id == FC_BUFFER_ID_INVALID)
        {
            return FTL_ERROR;
        }
        buf = fc_buffer_id_to_real_buffer(descriptor->buffer_id);
        if (buf == NULL)
        {
            return FTL_ERROR;
        }

        copy_len = (remaining >= FC_BUFFER_USER_SIZE) ? FC_BUFFER_USER_SIZE : remaining;
        (void)memcpy(dst, buf, copy_len);
        dst += copy_len;
        remaining -= copy_len;

        desc_id = (fc_descriptor_id_t)descriptor->next_id;
    }

    return FTL_OK;
}

/* ── 内部同步 NAND 操作 ── */

/*
 * 同步等待指定 command_id 完成。
 *
 * 使用 completion_table 统一路由：
 *   ① 注册 ftl_sync_waiter_handler 到 completion_table
 *   ② 循环 fc_service() + completion_table_drain() 直到 done
 *   ③ 无需处理 "other" completion — completion_table_drain 会自动路由
 *     非目标 completion 到 default handler（ftl_handle_completion）
 *
 * @param cmd_id      要等待的 command_id
 * @param status_out  输出 completion status
 * @param timeout_us  超时（0 = 使用默认 10ms）
 * @return 0 完成, -1 超时
 */
ftl_status_t ftl_internal_sync_wait(uint16_t cmd_id, fc_command_status_t *status_out, uint32_t timeout_us)
{
    ftl_sync_waiter_t waiter;
    uint32_t          elapsed  = 0U;
    uint32_t          max_wait = (timeout_us > 0U) ? timeout_us : 10000U;

    waiter.cmd_id = cmd_id;
    waiter.done   = 0U;

    if (completion_table_add(cmd_id, ftl_sync_waiter_handler, &waiter) != 0)
    {
        return FTL_ERROR; /* 路由表满 */
    }

    for (;;)
    {
        (void)fc_service();
        completion_table_drain();

        if (waiter.done != 0U)
        {
            break;
        }

        /* 简易超时：elapsed 递增（精确度取决于 fc_service 调用频率） */
        elapsed++;
        if (elapsed > max_wait)
        {
            completion_table_remove(cmd_id);
            return FTL_ERROR;
        }
    }

    if (status_out != NULL)
    {
        *status_out = waiter.completion;
    }

    return FTL_OK;
}
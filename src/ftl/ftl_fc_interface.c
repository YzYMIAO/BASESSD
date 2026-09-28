#include "ftl/ftl_fc_interface.h"

#include "fc/fc.h"
#include "ftl/ftl_type.h"
#include "nand/onfi_1_0.h"

#include <string.h>

int ftl_cmd_setup_ppa(fc_command_t *cmd, nand_pba_t pba)
{
    if (cmd == NULL)
    {
        return -1;
    }

    cmd->config.ch  = (DWORD)pba_to_ch(pba);
    cmd->config.ce  = (DWORD)pba_to_ce(pba);
    cmd->config.lun = (DWORD)pba_to_lun(pba);
    cmd->paa        = pba_to_paa(pba);

    return (int)fc_command_configure_descriptors(cmd);
}

int ftl_cmd_write_data(fc_command_t *cmd, const void *data, uint32_t length)
{
    return (int)fc_write_cmd_data(cmd, data, length);
}

int ftl_cmd_read_data(fc_command_t *cmd, void *buffer, uint32_t length)
{
    return (int)fc_read_cmd_data(cmd, buffer, length);
}

/* ── 内部同步 NAND 操作 ── */

/*
 * 同步等待指定 command_id 完成。
 *
 * 循环调用 fc_service() + fc_poll_completion()。
 * 如果 fc_poll_completion 返回的 command_id 与目标不匹配，
 * 说明有并发 I/O 完成了——释放该命令后继续等待。
 *
 * @param cmd_id      要等待的 command_id
 * @param status_out  输出 completion status
 * @param timeout_us  超时（0 = 使用默认 10ms）
 * @return 0 完成, -1 超时
 */
int ftl_internal_sync_wait(uint16_t cmd_id, fc_command_status_t *status_out, uint32_t timeout_us)
{
    uint32_t elapsed  = 0U;
    uint32_t max_wait = (timeout_us > 0U) ? timeout_us : 10000U; /* 默认 10ms */
    (void)elapsed;
    (void)max_wait;

    /*
     * 注意：简化实现——当前单 LUN 无并发 I/O 场景，直接循环。
     * 同步操作期间不会有其他 I/O 完成，所以不处理 "other" completion。
     * 多 LUN 并发时需接入完成路由机制。
     */
    for (;;)
    {
        fc_command_status_t completion;
        fc_status_t         ret;

        /* 驱动 FC 引擎 */
        (void)fc_service();

        /* 轮询 CQ */
        ret = fc_poll_completion(&completion);
        while (ret == FC_STATUS_OK)
        {
            if (completion.command_id == cmd_id)
            {
                if (status_out != NULL)
                {
                    *status_out = completion;
                }
                return 0; /* 找到目标完成 */
            }

            /*
             * 非本命令的 completion（多 LUN 并发时可能出现）：
             * 跳过——由 ftl_service Phase 2 的 ftl_handle_completion 处理，
             * 包含 pending 路由、读数据拷贝、命令释放等完整逻辑。
             * 此处不做任何操作保持 CQ entry 不被消耗。
             *
             * 但在当前实现中 fc_poll_completion 消耗了 CQ entry，
             * 不会再次出现。多 LUN 接入时需改造此循环。
             */
            ret = fc_poll_completion(&completion);
        }
    }

    /* 超时 */
    return -1;
}

int ftl_internal_read_page(nand_pba_t pba, void *buffer, uint32_t length, uint8_t crc, uint8_t random, uint8_t module, lba_t lba)
{
    fc_command_t       *cmd;
    fc_command_status_t completion;
    int                 ret;

    /* 1. 分配命令 */
    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return -1;
    }

    /* 2. 配置 */
    cmd->config.operation   = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
    cmd->config.cw          = 3U; /* 4 CW */
    cmd->config.crc         = (DWORD)crc;
    cmd->config.random      = (DWORD)random;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.multi_plane = 0U;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    cmd->module             = (DWORD)module;
    cmd->meta               = (DWORD)lba;

    /* 3. 配置 PBA + desc 链 */
    if (ftl_cmd_setup_ppa(cmd, pba) != 0)
    {
        (void)fc_command_release(cmd);
        return -1;
    }

    /* 4. 提交（非阻塞） */
    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        (void)fc_command_release(cmd);
        return -1;
    }

    /* 5. 同步等待完成 */
    ret = ftl_internal_sync_wait(cmd->command_id, &completion, 0U);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return -1;
    }

    /* 6. 检查 NAND 状态 */
    if (completion.status & FC_CW_STATUS_NAND_FAIL)
    {
        (void)fc_command_release(cmd);
        return -1;
    }

    /* 7. 读取数据 */
    if ((buffer != NULL) && (length > 0U))
    {
        (void)ftl_cmd_read_data(cmd, buffer, length);
    }

    /* 8. 释放命令 */
    (void)fc_command_release(cmd);

    return 0;
}

int ftl_internal_write_page(nand_pba_t pba, const void *data, uint32_t length, uint8_t crc, uint8_t random, uint8_t module, lba_t lba)
{
    fc_command_t       *cmd;
    fc_command_status_t completion;
    int                 ret;

    /* 1. 分配命令 */
    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return -1;
    }

    /* 2. 配置 */
    cmd->config.operation   = OP_NORMAL_PROGRAM_PAGE;
    cmd->config.cw          = 3U; /* 4 CW */
    cmd->config.crc         = (DWORD)crc;
    cmd->config.random      = (DWORD)random;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.multi_plane = 0U;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    cmd->module             = (DWORD)module;
    cmd->meta               = (DWORD)lba;

    /* 3. 配置 PBA + desc 链 */
    if (ftl_cmd_setup_ppa(cmd, pba) != 0)
    {
        (void)fc_command_release(cmd);
        return -1;
    }

    /* 4. 写入数据 */
    if ((data != NULL) && (length > 0U))
    {
        (void)ftl_cmd_write_data(cmd, data, length);
    }

    /* 5. 提交（非阻塞） */
    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        (void)fc_command_release(cmd);
        return -1;
    }

    /* 6. 同步等待完成 */
    ret = ftl_internal_sync_wait(cmd->command_id, &completion, 0U);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return -1;
    }

    /* 7. 检查 NAND 状态 */
    if (completion.status & FC_CW_STATUS_NAND_FAIL)
    {
        (void)fc_command_release(cmd);
        return -1;
    }

    /* 8. 释放命令 */
    (void)fc_command_release(cmd);

    return 0;
}

int ftl_internal_erase_block(nand_pba_t block_pba)
{
    fc_command_t       *cmd;
    fc_command_status_t completion;
    int                 ret;

    /* 1. 分配命令 */
    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return -1;
    }

    /* 2. 配置 */
    cmd->config.operation   = OP_ERASE_BLOCK;
    cmd->config.cw          = 0U; /* 擦除不涉及 CW */
    cmd->config.crc         = FC_CRC_DISABLE;
    cmd->config.random      = FC_RANDOM_DISABLE;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.multi_plane = 0U;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    cmd->module             = FC_COMMAND_MODULE_METADATA;
    cmd->meta               = 0U;

    /* 3. 配置 PBA + desc 链 */
    if (ftl_cmd_setup_ppa(cmd, block_pba) != 0)
    {
        (void)fc_command_release(cmd);
        return -1;
    }

    /* 4. 提交 */
    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        (void)fc_command_release(cmd);
        return -1;
    }

    /* 5. 同步等待完成（擦除最长 ~10ms）*/
    ret = ftl_internal_sync_wait(cmd->command_id, &completion, 20000U);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return -1;
    }

    /* 6. 检查 NAND 状态 */
    if (completion.status & FC_CW_STATUS_NAND_FAIL)
    {
        (void)fc_command_release(cmd);
        return -1;
    }

    /* 7. 释放命令 */
    (void)fc_command_release(cmd);

    return 0;
}

int ftl_internal_read_page_raw(nand_pba_t pba, void *buffer)
{
    fc_command_t       *cmd;
    fc_command_status_t completion;
    int                 ret;
    uint8_t             cw;

    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return -1;
    }

    /* 以 LDPC_RAW 模式读全页（跳过 ECC 解码） */
    cmd->config.operation   = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
    cmd->config.cw          = 3U;
    cmd->config.crc         = FC_CRC_DISABLE;
    cmd->config.random      = FC_RANDOM_DISABLE;
    cmd->config.ldpc        = FC_ECC_MODE_LDPC_RAW;
    cmd->config.multi_plane = 0U;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    cmd->module             = FC_COMMAND_MODULE_BBT;
    cmd->meta               = 0U;

    if (ftl_cmd_setup_ppa(cmd, pba) != 0)
    {
        (void)fc_command_release(cmd);
        return -1;
    }

    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        (void)fc_command_release(cmd);
        return -1;
    }

    ret = ftl_internal_sync_wait(cmd->command_id, &completion, 0U);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return -1;
    }

    /* 即使 NAND_FAIL/UNC 也尝试读数据——raw 模式返回所有 CW 数据 */
    for (cw = 0U; cw < CW_NUM; cw++)
    {
        uint8_t *src = fc_get_cmd_data_ptr(cmd, cw);
        if (src != NULL)
        {
            uint32_t cw_user_size = FC_BUFFER_USER_SIZE; /* 512B */
            memcpy((uint8_t *)buffer + (uint32_t)cw * FC_BUFFER_POOL_CW_SIZE,
                   src,
                   cw_user_size);
        }
    }

    (void)fc_command_release(cmd);
    return 0;
}
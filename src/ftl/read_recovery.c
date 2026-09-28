#include "ftl/read_recovery.h"
#include "ftl/ftl_fc_interface.h"
#include "fc/fc.h"
#include "fc/fc_command.h"
#include "nand/onfi_1_0.h"

#include <string.h>

void rr_init(ftl_read_recovery_t *rr,
              rr_ecc_track_t *block_track,
              uint16_t max_blocks)
{
    if (rr == NULL) return;

    rr->block_track  = block_track;
    rr->max_blocks   = max_blocks;
    rr->retry_count  = 0U;

    if (block_track != NULL)
    {
        memset(block_track, 0, sizeof(rr_ecc_track_t) * max_blocks);
    }
}

void rr_record_ecc(ftl_read_recovery_t *rr,
                    uint16_t block, uint8_t ecc_bits,
                    uint32_t total_writes)
{
    rr_ecc_track_t *track;

    if ((rr == NULL) || (rr->block_track == NULL) || (block >= rr->max_blocks))
    {
        return;
    }

    track = &rr->block_track[block];

    /*
     * 环形 buffer 写入当前 ECC bit 数。
     * 比较新值与上一次值判断趋势。
     */
    {
        uint8_t prev = track->ecc_history[track->history_idx];
        uint8_t idx;

        track->ecc_history[track->history_idx] = ecc_bits;
        idx = (uint8_t)((track->history_idx + 1U) % RR_ECC_HISTORY_DEPTH);
        track->history_idx = idx;
        track->last_update = total_writes;

        /* 趋势：连续上升计数 */
        if (ecc_bits > prev)
        {
            track->rising_count++;
        }
        else
        {
            track->rising_count = 0U;  /* 不上升就清空连续计数 */
        }

        /* 健康状态升级 */
        if (track->rising_count >= RR_ECC_RISING_THRESHOLD)
        {
            track->health = RR_BLOCK_WARNING;
        }
        if (ecc_bits >= RR_ECC_HIGH_WATER)
        {
            track->health = RR_BLOCK_CRITICAL;
        }
        if (ecc_bits >= RR_ECC_MAX)
        {
            track->health = RR_BLOCK_DYING;
        }
    }
}

/*
 * 内部：通过 FC 执行一次 Read Retry（切换读电压模式）
 *
 * 使用 SET_FEATURES 写 Read Retry mode reg（0x89），
 * 写完后用新模式重试一次读。
 *
 * @return 0 读取成功，-1 仍需恢复
 */
static int rr_exec_retry(uint8_t ch, uint8_t ce,
                          nand_pba_t pba, void *buffer, uint32_t length,
                          lba_t lba, uint8_t retry_mode)
{
    fc_command_t          *cmd;

    (void)ch;
    (void)ce;
    fc_command_status_t    completion;
    int                    ret;

    /*
     * Step 1: SET_FEATURES 写 Read Retry mode
     * feature_addr = 0x89, parameter[0] = retry_mode
     */
    cmd = fc_command_alloc();
    if (cmd == NULL) return -1;

    cmd->config.operation   = OP_SET_FEATURES;
    cmd->config.cw          = 0U;
    cmd->config.crc         = FC_CRC_DISABLE;
    cmd->config.random      = FC_RANDOM_DISABLE;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.multi_plane = 0U;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->module             = FC_COMMAND_MODULE_READ_RECOVERY;
    cmd->meta               = 0U;
    cmd->feature_addr       = RR_FEATURE_READ_RETRY;
    cmd->parameter0         = retry_mode;
    cmd->parameter1         = 0U;
    cmd->parameter2         = 0U;
    cmd->parameter3         = 0U;

    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        (void)fc_command_release(cmd);
        return -1;
    }

    ret = ftl_internal_sync_wait(cmd->command_id, &completion, 0U);
    (void)fc_command_release(cmd);
    if (ret != 0) return -1;
    if (completion.status & FC_CW_STATUS_NAND_FAIL) return -1;

    /*
     * Step 2: 用新模式重试读
     */
    ret = ftl_internal_read_page(pba, buffer, length,
                                  FC_CRC_ENABLE, FC_RANDOM_ENABLE,
                                  FC_COMMAND_MODULE_READ_RECOVERY, lba);
    if (ret == 0)
    {
        return 0;  /* 重试成功 */
    }

    /*
     * Step 3: 恢复默认读模式
     */
    cmd = fc_command_alloc();
    if (cmd == NULL) return -1;

    cmd->config.operation   = OP_SET_FEATURES;
    cmd->config.cw          = 0U;
    cmd->config.crc         = FC_CRC_DISABLE;
    cmd->config.random      = FC_RANDOM_DISABLE;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.multi_plane = 0U;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->module             = FC_COMMAND_MODULE_READ_RECOVERY;
    cmd->meta               = 0U;
    cmd->feature_addr       = RR_FEATURE_READ_RETRY;
    cmd->parameter0         = 0U;  /* Mode 0 = normal */
    cmd->parameter1         = 0U;
    cmd->parameter2         = 0U;
    cmd->parameter3         = 0U;

    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        (void)fc_command_release(cmd);
        return -1;
    }

    ret = ftl_internal_sync_wait(cmd->command_id, &completion, 0U);
    (void)fc_command_release(cmd);
    if (ret != 0) return -1;

    return -1;  /* 重试后仍然失败 */
}

/*
 * 内部：邻近页辅助读取
 *
 * 目标页读出错的常见原因是邻近页写入时的单元间干扰。
 * 读取相邻页（page ± 1）的 META 信息来辅助数据恢复。
 *
 * @return 0 恢复成功，-1 失败
 */
static int rr_neighbor_assist(uint8_t ch, uint8_t ce,
                               nand_pba_t pba, void *buffer, uint32_t length,
                               lba_t lba)
{
    uint8_t  target_page;
    uint8_t  plane;
    uint16_t block;
    uint8_t  neighbors[2];
    int      i;

    target_page = (uint8_t)pba_to_page(pba);
    plane       = (uint8_t)pba_to_plane(pba);
    block       = (uint16_t)pba_to_block(pba);

    /*
     * 邻居选择：读 target_page +1 和 -1 的 USER 数据。
     *
     * 在 SLC NAND 上邻近页干扰较弱，但读邻居有助于：
     *   ① 测距：如果邻居也失败，说明 Block 级问题而非单页问题
     *   ② 跨页 ECC：未来扩展（当前跳过）
     */
    neighbors[0] = (uint8_t)((target_page > 0U) ?
                             (target_page - 1U) : PAGES_PER_BLOCK - 1U);
    neighbors[1] = (uint8_t)((target_page < PAGES_PER_BLOCK - 1U) ?
                             (target_page + 1U) : 0U);

    for (i = 0; i < (int)RR_NEIGHBOR_ATTEMPTS; i++)
    {
        nand_pba_t neighbor_pba;
        uint8_t    neighbor_buf[FC_BUFFER_USER_SIZE * CW_NUM];
        int        ret;

        neighbor_pba = pba_make(ch, ce, neighbors[i], plane, block, 0, 0);

        memset(neighbor_buf, 0, sizeof(neighbor_buf));
        ret = ftl_internal_read_page(neighbor_pba, neighbor_buf, sizeof(neighbor_buf),
                                      FC_CRC_ENABLE, FC_RANDOM_ENABLE,
                                      FC_COMMAND_MODULE_READ_RECOVERY, 0U);
        if (ret == 0)
        {
            /*
             * 邻居读成功 → 说明 Block 本身没问题，
             * 可能是目标页有局部 bit 错。用标准 ECC 再试一次。
             */
            ret = ftl_internal_read_page(pba, buffer, length,
                                          FC_CRC_ENABLE, FC_RANDOM_ENABLE,
                                          FC_COMMAND_MODULE_READ_RECOVERY, lba);
            if (ret == 0)
            {
                return 0;  /* 邻近页辅助后恢复成功 */
            }
        }
        /* 邻居也失败 → Block 级问题，等后续处理 */
    }

    return -1;
}

int rr_recover_read(ftl_read_recovery_t *rr,
                     uint8_t ch, uint8_t ce,
                     nand_pba_t pba, void *buffer, uint32_t length,
                     lba_t lba, uint8_t *retry_count)
{
    uint8_t attempt;
    int     ret;

    if ((rr == NULL) || (buffer == NULL))
    {
        return -1;
    }

    rr->retry_count = 0U;

    /* Stage 1: Standard Read Retry（尝试不同的读电压模式） */
    for (attempt = 1U; attempt <= RR_MAX_RETRY; attempt++)
    {
        ret = rr_exec_retry(ch, ce, pba, buffer, length, lba, attempt);
        rr->retry_count = attempt;

        if (ret == 0)
        {
            if (retry_count != NULL)
            {
                *retry_count = attempt;
            }
            return 0;  /* Stage 1 恢复成功 */
        }
    }

    /* Stage 2: 邻近页辅助 */
    ret = rr_neighbor_assist(ch, ce, pba, buffer, length, lba);
    if (ret == 0)
    {
        if (retry_count != NULL)
        {
            *retry_count = rr->retry_count;
        }
        return 0;  /* Stage 2 恢复成功 */
    }

    /* Stage 3+ : 未来扩展 XOR/RAID 恢复 */
    /* 当前未实现，返回失败 */

    return -1;
}

int rr_check_predictive(ftl_read_recovery_t *rr, uint16_t *block)
{
    uint16_t i;

    if ((rr == NULL) || (rr->block_track == NULL) || (block == NULL))
    {
        return 0;
    }

    for (i = 0U; i < rr->max_blocks; i++)
    {
        rr_ecc_track_t *track = &rr->block_track[i];

        if (track->health >= RR_BLOCK_CRITICAL)
        {
            *block = i;
            return 1;
        }
    }

    return 0;
}

rr_block_health_t rr_get_health(const ftl_read_recovery_t *rr, uint16_t block)
{
    if ((rr == NULL) || (rr->block_track == NULL) || (block >= rr->max_blocks))
    {
        return RR_BLOCK_HEALTHY;
    }

    return (rr_block_health_t)rr->block_track[block].health;
}
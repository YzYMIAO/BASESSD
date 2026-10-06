#include "ftl/read_recovery.h"

#include "common/assertion.h"
#include "common/log.h"
#include "fc/fc.h"
#include "fc/fc_command.h"
#include "ftl/ftl_fc_interface.h"
#include "nand/onfi_1_0.h"

#include <string.h>

/* ── 内部同步读页（原 ftl_internal_read_page 内联化，CRC/Random 使能 + LBA META）── */
static ftl_status_t sync_read_page(nand_addr_t addr, void *buffer, uint32_t length, uint8_t crc, uint8_t random, uint8_t module, lba_t lba)
{
    fc_command_t       *cmd;
    fc_command_status_t completion;
    int                 ret;

    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    cmd->config.operation   = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
    cmd->config.cw          = 3U;
    cmd->config.crc         = (DWORD)crc;
    cmd->config.random      = (DWORD)random;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.multi_plane = 0U;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    cmd->module             = (DWORD)module;
    cmd->meta               = (DWORD)lba;

    if (addr_to_cmd(cmd, &addr) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    ret = ftl_internal_sync_wait(cmd->command_id, &completion, 0U);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    if (completion.status & FC_CW_STATUS_NAND_FAIL)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    if ((buffer != NULL) && (length > 0U))
    {
        (void)ftl_cmd_read_data(cmd, buffer, length);
    }

    (void)fc_command_release(cmd);
    return FTL_OK;
}

void rr_record_ecc(rr_ecc_track_t *track, uint8_t ecc_bits)
{
    /* 层内接口：调用者一律传 &ftl_block_t.rr（BST 常驻，恒非 NULL） */
    ASSERT(track != NULL);

    /* 趋势：与上一次读数比较（last_ecc 即上次值），连续上升计数 */
    if (ecc_bits > track->last_ecc)
    {
        track->rising_count++;
    }
    else
    {
        track->rising_count = 0U; /* 不上升就清空连续计数 */
    }
    track->last_ecc = ecc_bits;

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

/*
 * 内部：通过 FC 执行一次 Read Retry（切换读电压模式）
 *
 * 使用 SET_FEATURES 写 Read Retry mode reg（0x89），
 * 写完后用新模式重试一次读。
 *
 * @return 0 读取成功，-1 仍需恢复
 */
static int rr_exec_retry(uint8_t ch, uint8_t ce, nand_pba_t pba, void *buffer, uint32_t length, lba_t lba, uint8_t retry_mode)
{
    fc_command_t *cmd;

    (void)ch;
    (void)ce;
    fc_command_status_t completion;
    int                 ret;

    /*
     * Step 1: SET_FEATURES 写 Read Retry mode
     * feature_addr = 0x89, parameter[0] = retry_mode
     */
    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return -1;
    }

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
    cmd->param.parameter0   = retry_mode;
    cmd->param.parameter1   = 0U;
    cmd->param.parameter2   = 0U;
    cmd->param.parameter3   = 0U;

    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        (void)fc_command_release(cmd);
        return -1;
    }

    ret = ftl_internal_sync_wait(cmd->command_id, &completion, 0U);
    (void)fc_command_release(cmd);
    if (ret != 0)
    {
        return -1;
    }
    if (completion.status & FC_CW_STATUS_NAND_FAIL)
    {
        return -1;
    }

    /*
     * Step 2: 用新模式重试读
     */
    {
        nand_addr_t raddr = addr_from_pba(pba);
        ret               = sync_read_page(raddr, buffer, length, FC_CRC_ENABLE, FC_RANDOM_ENABLE, FC_COMMAND_MODULE_READ_RECOVERY, lba);
    }
    if (ret == 0)
    {
        return 0; /* 重试成功 */
    }

    /*
     * Step 3: 恢复默认读模式
     */
    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return -1;
    }

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
    cmd->param.parameter0   = 0U; /* Mode 0 = normal */
    cmd->param.parameter1   = 0U;
    cmd->param.parameter2   = 0U;
    cmd->param.parameter3   = 0U;

    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        (void)fc_command_release(cmd);
        return -1;
    }

    ret = ftl_internal_sync_wait(cmd->command_id, &completion, 0U);
    (void)fc_command_release(cmd);
    if (ret != 0)
    {
        return -1;
    }

    return -1; /* 重试后仍然失败 */
}

/*
 * 内部：邻近页辅助读取
 *
 * 目标页读出错的常见原因是邻近页写入时的单元间干扰。
 * 读取相邻页（page ± 1）的 META 信息来辅助数据恢复。
 *
 * @return 0 恢复成功，-1 失败
 */
static int rr_neighbor_assist(uint8_t ch, uint8_t ce, nand_pba_t pba, void *buffer, uint32_t length, lba_t lba)
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
    neighbors[0] = (uint8_t)((target_page > 0U) ? (target_page - 1U) : PAGES_PER_BLOCK - 1U);
    neighbors[1] = (uint8_t)((target_page < PAGES_PER_BLOCK - 1U) ? (target_page + 1U) : 0U);

    for (i = 0; i < (int)RR_NEIGHBOR_ATTEMPTS; i++)
    {
        nand_addr_t neighbor_addr;
        uint8_t     neighbor_buf[FC_BUFFER_USER_SIZE * CW_NUM];
        int         ret;

        neighbor_addr = addr_from_paa(ch, ce, paa_make(neighbors[i], plane, block, 0U, 0U));

        memset(neighbor_buf, 0, sizeof(neighbor_buf));
        ret = sync_read_page(neighbor_addr, neighbor_buf, sizeof(neighbor_buf), FC_CRC_ENABLE, FC_RANDOM_ENABLE, FC_COMMAND_MODULE_READ_RECOVERY, 0U);
        if (ret == 0)
        {
            /*
             * 邻居读成功 → 说明 Block 本身没问题，
             * 可能是目标页有局部 bit 错。用标准 ECC 再试一次。
             */
            {
                nand_addr_t raddr = addr_from_pba(pba);
                ret               = sync_read_page(raddr, buffer, length, FC_CRC_ENABLE, FC_RANDOM_ENABLE, FC_COMMAND_MODULE_READ_RECOVERY, lba);
            }
            if (ret == 0)
            {
                return 0; /* 邻近页辅助后恢复成功 */
            }
        }
        /* 邻居也失败 → Block 级问题，等后续处理 */
    }

    return -1;
}

int rr_recover_read(ftl_read_recovery_t *rr,
                    uint8_t              ch,
                    uint8_t              ce,
                    nand_pba_t           pba,
                    void                *buffer,
                    uint32_t             length,
                    lba_t                lba,
                    uint8_t             *retry_count)
{
    uint8_t attempt;
    int     ret;

    /* 层内接口：唯一调用者 ftl_handle_completion 传 &ftl->rr 与已判非空的 io.buffer */
    ASSERT(rr != NULL);
    ASSERT(buffer != NULL);

    rr->retry_count = 0U;

    /* Stage 1: Standard Read Retry（尝试不同的读电压模式） */
    for (attempt = 1U; attempt <= RR_MAX_RETRY; attempt++)
    {
        ret             = rr_exec_retry(ch, ce, pba, buffer, length, lba, attempt);
        rr->retry_count = attempt;

        if (ret == 0)
        {
            if (retry_count != NULL)
            {
                *retry_count = attempt;
            }
            return 0; /* Stage 1 恢复成功 */
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
        return 0; /* Stage 2 恢复成功 */
    }

    /* Stage 3: 恢复失败 → 显式兜底
     *
     * Stage 1（Read Retry）+ Stage 2（邻近页辅助）全失败后，
     * 页面已损坏提升为块级不可靠。调用方（读完成路径）在此处
     * 标记坏块（FTL_BLOCK_BAD_READ）+ 搬移，由预测搬移/GC 接手。
     * XOR/RAID 恢复（原规划 Stage 3）属后续阶段，当前不实现。 */

    return -1;
}

int rr_check_predictive(const ftl_block_t *blocks, uint16_t max_blocks, uint16_t *gblock)
{
    uint16_t i;

    if ((blocks == NULL) || (gblock == NULL))
    {
        return 0;
    }

    for (i = 0U; i < max_blocks; i++)
    {
        const rr_ecc_track_t *track = &blocks[i].rr;

        if (track->health >= RR_BLOCK_CRITICAL)
        {
            *gblock = i;
            return 1;
        }
    }

    return 0;
}

rr_block_health_t rr_get_health(const rr_ecc_track_t *track)
{
    if (track == NULL)
    {
        return RR_BLOCK_HEALTHY;
    }

    return (rr_block_health_t)track->health;
}

int rr_predictive_scan(const ftl_allocator_t *alloc, uint16_t *gblock)
{
    uint16_t i;

    /* 层内接口：唯一调用者 wl_predictive_relocate 传 &ftl->alloc 与局部 gblock */
    ASSERT(alloc != NULL);
    ASSERT(alloc->blocks != NULL);
    ASSERT(gblock != NULL);

    for (i = 0U; i < FTL_BLOCK_TOTAL; i++)
    {
        const ftl_block_t *b = &alloc->blocks[i];

        /* 慢死预测只针对 Closed 块：Open 还在写、Free 没数据、Bad/Reserved 已退役 */
        if (!ftl_block_state_is(b, FTL_BLOCK_STATE_CLOSED))
        {
            continue;
        }

        /* B5：ECC 趋势（连续上升 ≥RISING / 单次 ≥HIGH_WATER）或 read disturb
         * 高水位（read_count 超阈值，慢死第一信号）都触发搬移。 */
        if ((b->rr.health >= RR_BLOCK_CRITICAL) ||
            (b->read_count >= RR_READ_DISTURB_THRESHOLD))
        {
            *gblock = i;
            return 1;
        }
    }

    return 0;
}

void rr_retire_block(ftl_block_t *b)
{
    /* 层内接口：唯一调用者 wl_predictive_relocate 传 &ftl->alloc.blocks[gblock] */
    ASSERT(b != NULL);

    /* 退役：不再分配新数据（RESERVED = Spare Pool 语义），不标坏（BBT 不删、仍可读） */
    ftl_block_state_clr(b, FTL_BLOCK_STATE_OPEN | FTL_BLOCK_STATE_CLOSED | FTL_BLOCK_STATE_FREE | FTL_BLOCK_TEMP_MASK);
    ftl_block_state_set(b, FTL_BLOCK_STATE_RESERVED);
    ftl_block_set_sb(b, FTL_BLOCK_NO_SB);

    /* 健康复位：块仍活着，ECC 趋势重计（read_count 归零，read disturb 重计） */
    b->rr.health       = (uint8_t)RR_BLOCK_HEALTHY;
    b->rr.rising_count = 0U;
    b->rr.last_ecc     = 0U;
    b->read_count      = 0U;
}
#include "ftl/checkpoint.h"

#include "common/boot_policy.h"
#include "fc/fc.h"
#include "ftl/ftl.h"
#include "ftl/ftl_fc_interface.h"
#include "ftl/metadata.h"
#include "nand/onfi_1_0.h"
#include "platform/platform.h"

#include <string.h>

/* ── 内部辅助 ── */

/*
 * checkpoint_now_ms - 当前单调毫秒（经平台抽象 time_us 换算）。
 * 平台未注册时返回 0，checkpoint 头部时间戳退化为 0（行为同接线前）。
 */
static uint32_t checkpoint_now_ms(const ftl_context_t *ftl)
{
    const ssd_platform_ops_t *platform = ssd_platform_get();

    (void)ftl;
    if ((platform == NULL) || (platform->time_us == NULL))
    {
        return 0U;
    }
    return (uint32_t)(platform->time_us() / 1000U);
}

/* Checkpoint CRC 覆盖的字节数（从开头到 crc 字段） */
#define CHECKPOINT_CRC_SIZE   (sizeof(uint32_t) * 6)                        /* magic+seq+ts+rc+bitmap[64]+js = 6 fields before crc */
#define CHECKPOINT_CRC_OFFSET (CHECKPOINT_CRC_SIZE + sizeof(uint32_t) * 64) /* + region_bitmap[64] */
/* = 24 + 256 = 280 bytes covered by CRC */

/* ── 内部同步写页（原 ftl_internal_write_page 内联化，固定地址直填）── */
static ftl_status_t sync_write_page(nand_addr_t addr, const void *data, uint32_t length,
                                    uint8_t module)
{
    fc_command_t       *cmd;
    fc_command_status_t completion;
    int                 ret;

    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    cmd->config.operation   = OP_NORMAL_PROGRAM_PAGE;
    cmd->config.cw          = 3U;
    cmd->config.crc         = FC_CRC_DISABLE;
    cmd->config.random      = FC_RANDOM_DISABLE;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.multi_plane = 0U;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    cmd->module             = (DWORD)module;
    cmd->meta               = 0U;

    if (addr_to_cmd(cmd, &addr) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    if ((data != NULL) && (length > 0U))
    {
        (void)ftl_cmd_write_data(cmd, data, length);
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

    (void)fc_command_release(cmd);
    return FTL_OK;
}

/* ── 内部同步读页（原 ftl_internal_read_page 内联化）── */
static ftl_status_t sync_read_page(nand_addr_t addr, void *buffer, uint32_t length,
                                   uint8_t module)
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
    cmd->config.crc         = FC_CRC_DISABLE;
    cmd->config.random      = FC_RANDOM_DISABLE;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.multi_plane = 0U;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    cmd->module             = (DWORD)module;
    cmd->meta               = 0U;

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

/* ── Checkpoint 固定位置页地址（Block0/Block1 各 3 页）── */
static nand_addr_t checkpoint_page_addr(uint8_t ch, uint8_t ce, uint8_t lun, uint8_t i)
{
    nand_addr_t addr;

    addr.ch     = ch;
    addr.ce     = ce;
    addr.lun    = lun;
    addr.plane  = (uint8_t)((i & 1U) & 1U); /* 页号奇偶 → P1/P0（双 Plane 交错） */
    addr.page   = (uint8_t)((i < 3U) ? i : (i - 3U));
    addr.block  = (uint8_t)((i < 3U) ? CHECKPOINT_BLOCK0 : CHECKPOINT_BLOCK1);
    addr.cw     = 0U;

    return addr;
}

ftl_status_t checkpoint_create(ftl_context_t *ftl)
{
    l2p_checkpoint_header_t cp;
    uint8_t                 i;
    int                     ret_total = 0;

    if (ftl == NULL)
    {
        return FTL_ERROR;
    }

    /* ① flush 所有 dirty cache → NAND L2P Table */
    l2p_cache_flush_all(&ftl->cache);

    /* ② 构造 Header */
    memset(&cp, 0, sizeof(cp));
    cp.magic            = CHECKPOINT_MAGIC;
    cp.sequence         = ftl->journal.sequence_counter; /* 用 journal 最新 sequence */
    cp.timestamp        = checkpoint_now_ms(ftl);         /* 平台 time_us 换算 ms 快照 */
    cp.region_count     = L2P_REGIONS_TOTAL;             /* 全量 checkpoint */
    cp.journal_sequence = ftl->journal.sequence_counter; /* 截断到此 seq */

    /* region_bitmap: 全 1（全量） */
    for (i = 0U; i < 64U; i++)
    {
        cp.region_bitmap[i] = 0xFFFFFFFFU;
    }

    /* CRC */
    cp.crc = 0U;
    cp.crc = boot_crc32(&cp, sizeof(cp) - sizeof(cp.padding) - 4); /* 不含 padding 和 crc 自身 */

    /* ③ 写 6 页冗余 */
    for (i = 0U; i < CHECKPOINT_TOTAL_PAGES; i++)
    {
        nand_addr_t pba;
        int         ret;

        pba = checkpoint_page_addr(ftl->topo.ch, ftl->topo.ce, ftl->topo.lun, i);

        ret = sync_write_page(pba, &cp, sizeof(cp), FC_COMMAND_MODULE_CHECKPOINT);
        if (ret != 0)
        {
            ret_total = -1; /* 单份失败不影响整体，有冗余 */
        }
    }

    /* ④ flush Metadata Directory（保证 Directory 最新） */
    metadata_dir_flush_to_anchor(&ftl->metadata_dir);

    /* ⑤ 截断 Journal */
    journal_truncate(&ftl->journal, cp.journal_sequence);

    return ret_total;
}

ftl_status_t checkpoint_find_latest(l2p_checkpoint_header_t *cp, ftl_topology_t topo)
{
    l2p_checkpoint_header_t  candidates[6];
    l2p_checkpoint_header_t *best = NULL;
    uint8_t                  i;
    int                      found = 0;

    if (cp == NULL)
    {
        return FTL_ERROR;
    }

    memset(candidates, 0, sizeof(candidates));

    for (i = 0U; i < CHECKPOINT_TOTAL_PAGES; i++)
    {
        nand_addr_t pba;
        int         ret;

        pba = checkpoint_page_addr(topo.ch, topo.ce, topo.lun, i);

        ret = sync_read_page(pba, &candidates[i], sizeof(candidates[i]), FC_COMMAND_MODULE_CHECKPOINT);
        if (ret != 0)
        {
            continue;
        }

        /* 验证 Magic */
        if (candidates[i].magic != CHECKPOINT_MAGIC)
        {
            continue;
        }

        /* 验证 CRC */
        {
            uint32_t saved_crc = candidates[i].crc;
            candidates[i].crc  = 0U;
            if (boot_crc32(&candidates[i], sizeof(candidates[i]) - sizeof(candidates[i].padding) - 4) != saved_crc)
            {
                candidates[i].crc = saved_crc;
                continue;
            }
            candidates[i].crc = saved_crc;
        }

        /* 选 sequence 最大的 */
        if ((best == NULL) || (candidates[i].sequence > best->sequence))
        {
            best  = &candidates[i];
            found = 1;
        }
    }

    if (!found)
    {
        return FTL_ERROR;
    }

    *cp = *best;
    return FTL_OK;
}

int checkpoint_check_trigger(const ftl_context_t *ftl, uint32_t elapsed_ms)
{
    (void)elapsed_ms;

    if (ftl == NULL)
    {
        return FTL_OK;
    }

    /* T1: Journal 水位线 */
    if (ftl->journal.checkpoint_needed)
    {
        return 1;
    }

    /* T2: 写入量触发 */
    if ((ftl->total_writes > 0U) &&
        ((ftl->total_writes % CP_WRITE_INTERVAL) == 0U))
    {
        return 1;
    }

    /* T3: 定时触发（由调用者检查 elapsed_ms）*/
    if (elapsed_ms >= CP_TIME_INTERVAL_MS)
    {
        return 1;
    }

    return FTL_OK;
}

int checkpoint_is_emergency(const ftl_context_t *ftl)
{
    if (ftl == NULL)
    {
        return FTL_OK;
    }

    return (int)ftl->journal.emergency;
}

void checkpoint_journal_watermark(l2p_journal_t *journal)
{
    uint32_t used;

    if (journal == NULL)
    {
        return;
    }

    /*
     * 计算 Journal 使用率：
     *   used% = (current_page × 100 + current_entry × 100 / JOURNAL_ENTRIES_PER_PAGE)
     *           / PAGES_PER_BLOCK
     */
    used = ((uint32_t)journal->current_page * 100U +
            (uint32_t)journal->current_entry * 100U / JOURNAL_ENTRIES_PER_PAGE) /
           PAGES_PER_BLOCK;

    if (used >= JOURNAL_CRITICAL)
    {
        journal->emergency = 1U; /* ≥95% 紧急 */
    }
    else if (used >= JOURNAL_FULL_THRESHOLD)
    {
        journal->checkpoint_needed = 1U; /* ≥85% 需 Checkpoint */
    }
}
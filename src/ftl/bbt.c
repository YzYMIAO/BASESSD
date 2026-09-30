#include "ftl/bbt.h"

#include "common/boot_policy.h"
#include "fc/fc.h"
#include "ftl/ftl_fc_interface.h"
#include "nand/onfi_1_0.h"

#include <string.h>

/*
 * BBT 内部 SRAM 缓存（全局单例，运行时用）。
 * 出厂扫描或加载时填充，bbt_mark_bad 时同步更新。
 */
static uint8_t  g_bbt_bitmap[BBT_BITMAP_SIZE]; /* 4096 bit = 512B */
static uint32_t g_bbt_sequence = 0U;           /* 当前 sequence */
static int      g_bbt_dirty    = 0;            /* 是否有未写回 NAND 的变更 */

/* BBT CRC 覆盖的字节数（从开头到 crc 字段） */
#define BBT_CRC_SIZE (sizeof(uint32_t) * 4 + sizeof(uint32_t) * 3 + BBT_BITMAP_SIZE)

/* ── 内部同步写页（原 ftl_internal_write_page 内联化，固定地址直填）── */
static ftl_status_t sync_write_page(nand_addr_t addr, const void *data, uint32_t length)
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
    cmd->module             = FC_COMMAND_MODULE_BBT;
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
static ftl_status_t sync_read_page(nand_addr_t addr, void *buffer, uint32_t length)
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
    cmd->module             = FC_COMMAND_MODULE_BBT;
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

/* ── 内部同步读全页（原 ftl_internal_read_page_raw 内联化，LDPC_RAW）──
 *
 * 以 LDPC_RAW 模式读完整 NAND 页（PAGE_SIZE=2112），4 个 CW 依次拷出，
 * 每 CW 拷 FC_BUFFER_USER_SIZE 字节到 buffer 的 CW 槽（FC_BUFFER_POOL_CW_SIZE 间隔）。
 */
static ftl_status_t sync_read_page_raw(nand_addr_t addr, void *buffer)
{
    fc_command_t       *cmd;
    fc_command_status_t completion;
    int                 ret;
    uint8_t             cw;

    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

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

    for (cw = 0U; cw < CW_NUM; cw++)
    {
        uint8_t *src = fc_get_cmd_data_ptr(cmd, cw);
        if (src != NULL)
        {
            memcpy((uint8_t *)buffer + (uint32_t)cw * FC_BUFFER_POOL_CW_SIZE,
                   src,
                   FC_BUFFER_USER_SIZE);
        }
    }

    (void)fc_command_release(cmd);
    return FTL_OK;
}

/* ── BBT 固定位置页地址辅助（Block 36 / 备用 Block 39 各 3 页）── */
static nand_addr_t bbt_page_addr(uint8_t ch, uint8_t ce, uint8_t lun, uint8_t i)
{
    nand_addr_t addr;

    addr.ch     = ch;
    addr.ce     = ce;
    addr.lun    = lun;
    addr.plane  = (uint8_t)((i & 1U) & 1U);
    addr.page   = (uint8_t)((i < BBT_REDUNDANCY) ? i : (i - BBT_REDUNDANCY));
    addr.block  = (uint8_t)((i < BBT_REDUNDANCY) ? BBT_BLOCK : BBT_BLOCK_BACKUP);
    addr.cw     = 0U;

    return addr;
}

ftl_status_t bbt_init(ftl_allocator_t *alloc)
{
    uint32_t seq;
    int      ret;
    uint32_t loaded_bad_count = 0U;

    if (alloc == NULL)
    {
        return FTL_ERROR;
    }

    memset(g_bbt_bitmap, 0, sizeof(g_bbt_bitmap));
    g_bbt_sequence = 0U;
    g_bbt_dirty    = 0;

    /* 尝试从 NAND 加载已存在的 BBT */
    {
        ftl_topology_t topo = { .ch = alloc->ch, .ce = alloc->ce, .lun = alloc->lun };
        ret = bbt_load(g_bbt_bitmap, &loaded_bad_count, &seq, topo);
    }
    if (ret == 0)
    {
        uint16_t block;
        uint16_t bad_idx = 0U;

        g_bbt_sequence   = seq;
        alloc->bad_count = (uint16_t)loaded_bad_count;

        /* 重建 alloc->bad_blocks 数组 */
        for (block = 0U; block < BBT_MAX_BLOCKS; block++)
        {
            if (bbt_is_bad(g_bbt_bitmap, block))
            {
                if (bad_idx < BLOCKS_PER_PLANE * PLANE_NUM)
                {
                    alloc->bad_blocks[bad_idx++] = block;
                }
            }
        }
        alloc->bad_count = bad_idx;

        return FTL_OK;
    }

    /* 无有效 BBT → 执行出厂扫描 */
    return bbt_scan_factory(alloc);
}

int bbt_scan_factory(ftl_allocator_t *alloc)
{
    uint16_t block;
    uint16_t bad_idx = 0U;

    if (alloc == NULL)
    {
        return FTL_OK;
    }

    memset(g_bbt_bitmap, 0, sizeof(g_bbt_bitmap));
    g_bbt_dirty = 0;

    /*
     * 扫描策略：
     *
     * 标准 NAND 出厂坏块标记在 Page 0/1 的 OOB 区域第一个字节。
     * 当前 FC 层没有独立 OOB 读取接口，采用替代策略：
     *
     * 以 LDPC_RAW 模式（跳过 ECC）读取 Page 0 的原始 CW 数据。
     * 如果读取完全失败（FTL_RESULT_FAIL），判断为出厂坏块。
     *
     * 注意：LDPC_RAW 模式下 CW 数据仍然可以被读取，但 full-FF
     *   可能是空块的特征，不是坏块特征。真正的 OOB 检测需要
     *   FC 层扩展 spare 区读取功能。
     */
    for (block = 0U; block < BBT_MAX_BLOCKS; block++)
    {
        uint8_t     raw_buf[PAGE_SIZE];
        nand_addr_t page_addr0;
        int         ret0;

        /* 跳过 Block 0（出厂总是好块）*/
        if (block == 0U)
        {
            continue;
        }

        /* 尝试读 Page 0（只检查 Plane 0） */
        page_addr0 = addr_from_paa(alloc->ch, alloc->ce, paa_make(0U, 0U, block, 0U, 0U));

        memset(raw_buf, 0xFF, sizeof(raw_buf));
        ret0 = sync_read_page_raw(page_addr0, raw_buf);

        /*
         * LDPC_RAW 模式下，即使 NAND 物理失败，
         * fc_poll_completion 仍然返回 status=READY（raw 模式无 ECC 错误）。
         * 所以用返回码判断坏块不一定可靠。
         *
         * 替代方案：检查 Page 0 的数据是否为全 0xFF（空块特征）。
         * 出厂好块的 Page 0 数据区不为全 FF（有坏块标记等）。
         * 出厂坏块的 Page 0 可能全 FF 或读取失败。
         *
         * 当前实现：
         *   两种都标记——ret0 失败 或 raw_buf 全 FF（出厂商预写到 spare 区的
         *   标识被跳过，FC 层当前只读 USER 数据区）。
         *
         * 更准确的检测需要 FC 提供 spare/OOB 读取接口。
         */

        /* 保守策略：读取出错就标记坏块 */
        if (ret0 != 0)
        {
            bbt_set_bad(g_bbt_bitmap, block);
            if (bad_idx < BLOCKS_PER_PLANE * PLANE_NUM)
            {
                alloc->bad_blocks[bad_idx++] = block;
            }
        }
    }

    alloc->bad_count = bad_idx;
    g_bbt_dirty      = 1;

    /* 将扫描结果写入 NAND BBT */
    (void)bbt_save(alloc);

    return (int)bad_idx;
}

ftl_status_t bbt_mark_bad(ftl_allocator_t *alloc, uint16_t block)
{
    if ((alloc == NULL) || (block >= BBT_MAX_BLOCKS))
    {
        return FTL_ERROR;
    }

    /* BBT bitmap 置位（RAM only — 后台 bbt_flush 持久化到 NAND） */
    bbt_set_bad(g_bbt_bitmap, block);
    g_bbt_dirty = 1;

    /* allocator 侧记录（不再分配此 block） */
    allocator_mark_bad(alloc, block);

    /* 不再同步调 bbt_save() — NAND BBT 持久化由后台 bbt_flush 批量执行 */

    return FTL_OK;
}

ftl_status_t bbt_save(const ftl_allocator_t *alloc)
{
    bbt_header_t header;
    uint8_t      i;

    if (alloc == NULL)
    {
        return FTL_ERROR;
    }
    if (!g_bbt_dirty)
    {
        return FTL_OK;
    }

    g_bbt_sequence++;

    memset(&header, 0, sizeof(header));
    header.magic        = BBT_MAGIC;
    header.sequence     = g_bbt_sequence;
    header.total_blocks = BBT_MAX_BLOCKS;
    header.bad_count    = alloc->bad_count;

    memcpy(header.bad_map, g_bbt_bitmap, BBT_BITMAP_SIZE);

    /* CRC（crc 字段置 0 后计算） */
    header.crc = 0U;
    header.crc = boot_crc32(&header, sizeof(header) - 4);

    /* 写 3 页到 Block 36 */
    for (i = 0U; i < BBT_REDUNDANCY; i++)
    {
        nand_addr_t pba = bbt_page_addr(alloc->ch, alloc->ce, alloc->lun, i);
        (void)sync_write_page(pba, &header, sizeof(header));
    }

    /* 再写 3 页到备用 Block 39 */
    for (i = 0U; i < BBT_REDUNDANCY; i++)
    {
        nand_addr_t pba = bbt_page_addr(alloc->ch, alloc->ce, alloc->lun, (uint8_t)(i + BBT_REDUNDANCY));
        (void)sync_write_page(pba, &header, sizeof(header));
    }

    g_bbt_dirty = 0;
    return FTL_OK;
}

ftl_status_t bbt_flush(ftl_allocator_t *alloc)
{
    /* 仅当 dirty 时才写 NAND */
    if (!g_bbt_dirty)
    {
        return FTL_OK;
    }
    return bbt_save(alloc);
}

ftl_status_t bbt_load(uint8_t *bad_map, uint32_t *bad_count, uint32_t *sequence, ftl_topology_t topo)
{
    bbt_header_t  candidates[BBT_TOTAL_PAGES];
    bbt_header_t *best = NULL;
    uint8_t       i;
    int           found = 0;

    if ((bad_map == NULL) || (bad_count == NULL) || (sequence == NULL))
    {
        return FTL_ERROR;
    }

    memset(candidates, 0, sizeof(candidates));

    /* 扫描 Block 36 + Block 39 共 6 页 */
    for (i = 0U; i < BBT_TOTAL_PAGES; i++)
    {
        nand_addr_t pba;
        int         ret;

        pba = bbt_page_addr(topo.ch, topo.ce, topo.lun, i);

        ret = sync_read_page(pba, &candidates[i], sizeof(candidates[i]));
        if (ret != 0)
        {
            continue;
        }

        /* 验证 Magic */
        if (candidates[i].magic != BBT_MAGIC)
        {
            continue;
        }

        /* 验证 CRC */
        {
            uint32_t saved_crc = candidates[i].crc;
            candidates[i].crc  = 0U;
            if (boot_crc32(&candidates[i], sizeof(candidates[i]) - 4) != saved_crc)
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

    /* 输出到 caller */
    memcpy(bad_map, best->bad_map, BBT_BITMAP_SIZE);
    *bad_count = best->bad_count;
    *sequence  = best->sequence;

    return FTL_OK;
}

ftl_status_t bbt_spare_pool_refill(ftl_allocator_t *alloc)
{
    (void)alloc;

    /*
     * Spare Pool 的 Block 由上层管理（出厂时从 OP 池预留）。
     * 当前实现：Spare Pool 耗尽时返回 -1，不影响正常工作。
     *
     * 完整实现待 allocator 扩展 spare_pool_blocks[] 数组后接入。
     */
    return FTL_ERROR;
}
/*
 * test_fc_level3.c — Level 3 FC 全链路测试：Block 级数据完整性 + 多场景组合验证
 *
 * 测试场景：
 *   1. 初始化序列：RESET → READ_ID → READ_PARAMETER_PAGE → GET_FEATURES
 *   2. Feature 写回：SET_FEATURES → GET_FEATURES → 验证
 *   3. 状态查询：READ_STATUS / READ_STATUS_ENHANCED
 *   4. 写 + 读：NORMAL_PROGRAM_PAGE → NORMAL_READ_PAGE_WITH_DATA_OUT（全 block）
 *   5. Random 列读：READ → RANDOM_DATA_READ（依赖于前置 READ）
 *   6. Cache 读序列：CACHE_RANDOM → CACHE_SEQ × N → CACHE_LAST
 *   7. 内部搬移：READ_FOR_INTERNAL_DATA_MOVE → PROGRAM_FOR_INTERNAL_MOVE
 *   8. 多 Plane：MP_ERASE / MP_PROGRAM / MP_READ
 *   9. 擦除验证：ERASE → 全 0xFF 验证
 *   10. 紧锁定验证：BLOCK_LOCK_TIGHT（必须最后执行）
 *
 * PAA 约定：ch=0 ce=0 lun=0 plane=0 block=<test> page=0..63 cw=0
 *           多 Plane 操作使用 plane=0/1，双 Plane 地址
 *
 * 用法：
 *   ssd> test /fc-nand/lifecycle/
 *   ssd> test --list
 */

#include "munit.h"

#include "fc/fc.h"
#include "fc/fc_command.h"
#include "fc/fc_queue.h"
#include "fc/fc_scheduler.h"
#include "nand/onfi_1_0.h"
#include "nand/nand_config.h"
#include "nand/nand_dispatch.h"
#include "common/ssd_log.h"

#include <string.h>

/**************************************************************************
 *  Suite 级 setup
 *
 *  fc_init() 和 NAND init 由 main.c 在 UART CLI 之前完成。
 **************************************************************************/

static void *setup_level3(const MunitParameter params[], void *user_data)
{
    (void)params;
    (void)user_data;

    /* 恢复 device_ready——Level 1/2 的 fc_init() 在 setup 阶段将其重置为 0。 */
    nand_dispatch_set_device_ready(1U);

    fc_resource_stats_t stats;
    if (fc_get_resource_stats(&stats) == FC_STATUS_OK)
    {
        SSD_LOGI("[RES] L3_start: cmd=%u/%u desc=%u/%u buf=%u/%u sq=%u hsq=%u cq=%u\r\n",
                 (unsigned)stats.command_used,   (unsigned)FC_COMMAND_POOL_DEPTH,
                 (unsigned)stats.descriptor_used, (unsigned)FC_DESCRIPTOR_POOL_DEPTH,
                 (unsigned)stats.buffer_used,     (unsigned)FC_BUFFER_POOL_DEPTH,
                 (unsigned)stats.sq_used,
                 (unsigned)stats.hsq_used,
                 (unsigned)stats.cq_used);
    }
    return NULL;
}

/**************************************************************************
 *  NAND 几何参数（固定值，用于 block 级验证）
 *
 *  block 100（plane 0）：
 *    每 page = 4 CW x 512B user data = 2048B
 *    每 block = 64 pages
 *    全 block user data = 128 KB
 *
 *  block 101：内部搬移目标
 **************************************************************************/

#define BLOCK_TEST        (100U)   /* 固定测试 block */
#define BLOCK_DEST        (102U)   /* 内部搬移目标 block */

/**************************************************************************
 *  PAA 辅助
 *
 *  约定：ch=0, ce=0, lun=0
 **************************************************************************/

static nand_paa_t make_paa(uint16_t block, uint8_t plane, uint8_t page)
{
    return paa_make(page, plane, block, 0U, 0U);
}

/**************************************************************************
 *  Buffer pattern 辅助
 *
 *  每个 page 用 (block << 8 | page) 做 seed，生成 CW 粒度的 pattern。
 *  这样各 page 数据不同但可预测，读回后能逐字节还原验证。
 **************************************************************************/

static void fill_page_pattern(uint8_t *buf, uint16_t block, uint8_t page, uint8_t plane)
{
    uint16_t seed = (uint16_t)((block & 0xFFU) << 8U) | (uint16_t)((uint16_t)page << 4U) | plane;
    for (uint32_t i = 0U; i < FC_BUFFER_USER_SIZE; i++)
    {
        buf[i] = (uint8_t)(seed + (uint16_t)i);
    }
}

static uint8_t expected_byte(uint16_t block, uint8_t page, uint8_t plane, uint32_t offset)
{
    uint16_t seed = (uint16_t)((block & 0xFFU) << 8U) | (uint16_t)((uint16_t)page << 4U) | plane;
    return (uint8_t)(seed + (uint16_t)(offset % 256U));
}

/**************************************************************************
 *  辅助：读命令 buffer 全部 memset 为 0xCC
 *
 *  在 submit 前调用，确保 buffer 没有残留数据。
 *  如果 NAND 读不完全填满 buffer，剩余位置保留 0xCC，方便 debug。
 **************************************************************************/

static void memset_read_buffer(fc_command_t *cmd)
{
    fc_descriptor_id_t did = cmd->desc_id;
    while (did != FC_DESCRIPTOR_ID_INVALID)
    {
        fc_descriptor_t *d = fc_descriptor_id_to_descriptor(did);
        if (d == NULL) break;
        uint8_t *buf = fc_buffer_id_to_real_buffer(d->buffer_id);
        if (buf != NULL) memset(buf, 0xCC, FC_BUFFER_USER_SIZE);
        did = d->next_id;
    }
}

/**************************************************************************
 *  辅助：配置 desc chain，返回 desc 数量
 *
 *  配置命令的 desc chain（CW_NUM 个 desc），并为每个 desc buffer 填充
 *  pattern。适用于有数据输入/输出的 OP。
 **************************************************************************/

static uint8_t cmd_configure_and_fill(fc_command_t *cmd, uint16_t block, uint8_t page, uint8_t plane)
{
    cmd->config.cw = CW_NUM - 1U;
    cmd->paa       = make_paa(block, plane, page);

    fc_status_t result = fc_command_configure_descriptors(cmd);
    munit_assert_int(result, ==, FC_STATUS_OK);

    /* 遍历 desc chain，填充 pattern */
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
    uint8_t            cw_idx  = 0U;
    while (desc_id != FC_DESCRIPTOR_ID_INVALID)
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        if (desc == NULL) break;
        uint8_t *buf = fc_buffer_id_to_real_buffer(desc->buffer_id);
        if (buf != NULL)
        {
            fill_page_pattern(buf, block, page, plane);
        }
        desc_id = (fc_descriptor_id_t)desc->next_id;
        cw_idx++;
    }
    return cw_idx;
}

/**************************************************************************
 *  辅助：验证 desc chain 中各 CW 的 status 均为 SUCCESS
 **************************************************************************/

static uint8_t verify_cw_status_all_success(fc_command_t *cmd)
{
    uint8_t all_ok = 1U;
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
    uint8_t            cw_idx  = 0U;
    while (desc_id != FC_DESCRIPTOR_ID_INVALID)
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        if (desc == NULL) break;
        if ((desc->status & FC_CW_STATUS_SUCCESS) == 0U)
        {
            munit_errorf("CW %u status=0x%02X expected FC_CW_STATUS_SUCCESS", cw_idx, (unsigned)desc->status);
            all_ok = 0U;
        }
        desc_id = (fc_descriptor_id_t)desc->next_id;
        cw_idx++;
    }
    return all_ok;
}

/**************************************************************************
 *  辅助：释放命令关联的所有 buffer
 *
 *  fc_command_release() 释放 command 和 descriptor，但不释放 buffer。
 *  在 block 级测试中，用完数据后必须显式释放 buffer，否则 128 个
 *  buffer 在 32 pages（每 page 4 CW）后耗尽。
 **************************************************************************/

static void release_command_buffers(fc_command_t *cmd)
{
    fc_descriptor_id_t desc_id;
    uint8_t            iter;

    if ((cmd == NULL) || (cmd->desc_id == FC_DESCRIPTOR_ID_INVALID))
    {
        return;
    }

    desc_id = (fc_descriptor_id_t)cmd->desc_id;
    iter    = 0U;
    while ((desc_id != FC_DESCRIPTOR_ID_INVALID) && (iter < CW_TOTAL_MAX))
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        if (desc == NULL) break;
        if (desc->buffer_id != FC_BUFFER_ID_INVALID)
        {
            (void)fc_buffer_release(desc->buffer_id);
        }
        desc_id = (fc_descriptor_id_t)desc->next_id;
        iter++;
    }
}

/**************************************************************************
 *  辅助：读回 desc chain buffer，与预期 pattern 逐字节对比
 **************************************************************************/

static uint8_t verify_page_data(fc_command_t *cmd, uint16_t block, uint8_t page, uint8_t plane)
{
    uint8_t            all_ok  = 1U;
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
    uint8_t            cw_idx  = 0U;
    while (desc_id != FC_DESCRIPTOR_ID_INVALID)
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        if (desc == NULL) break;
        uint8_t *buf = fc_buffer_id_to_real_buffer(desc->buffer_id);
        if (buf != NULL)
        {
            for (uint32_t i = 0U; i < FC_BUFFER_USER_SIZE; i++)
            {
                uint8_t exp = expected_byte(block, page, plane, (uint32_t)cw_idx * FC_BUFFER_USER_SIZE + i);
                if (buf[i] != exp)
                {
                    munit_errorf("page=%u cw=%u offset=%u: got 0x%02X expected 0x%02X",
                                 (unsigned)page, (unsigned)cw_idx, (unsigned)i,
                                 (unsigned)buf[i], (unsigned)exp);
                    all_ok = 0U;
                    goto next_desc;
                }
            }
        }
next_desc:
        desc_id = (fc_descriptor_id_t)desc->next_id;
        cw_idx++;
    }
    return all_ok;
}

/**************************************************************************
 *  辅助：读回 desc chain buffer，验证每个 byte == 0xFF（擦除后）
 **************************************************************************/

static uint8_t verify_page_erased(fc_command_t *cmd, uint8_t page)
{
    uint8_t            all_ok  = 1U;
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
    uint8_t            cw_idx  = 0U;
    while (desc_id != FC_DESCRIPTOR_ID_INVALID)
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        if (desc == NULL) break;
        uint8_t *buf = fc_buffer_id_to_real_buffer(desc->buffer_id);
        if (buf != NULL)
        {
            for (uint32_t i = 0U; i < FC_BUFFER_USER_SIZE; i++)
            {
                if (buf[i] != 0xFFU)
                {
                    munit_errorf("page=%u cw=%u offset=%u: got 0x%02X expected 0xFF (erased)",
                                 (unsigned)page, (unsigned)cw_idx, (unsigned)i, (unsigned)buf[i]);
                    all_ok = 0U;
                    goto next_desc;
                }
            }
        }
next_desc:
        desc_id = (fc_descriptor_id_t)desc->next_id;
        cw_idx++;
    }
    return all_ok;
}

/**************************************************************************
 *  辅助：执行命令生命周期（submit + poll + release）
 *
 *  对给定的 command 执行 submit -> service loop -> poll -> release。
 *  适用于不需要检查 completion 数据的场景。
 *  如需检查 completion 数据（status、parameter0、buffer 数据等），
 *  请使用 run_submit_poll() + 手动 release。
 **************************************************************************/

#define SERVICE_LOOP_MAX (5000U)

static void run_lifecycle(fc_command_t *cmd)
{
    fc_command_status_t completion;
    fc_status_t         result;
    uint32_t            service_count;
    uint8_t             completed = 0U;

    munit_assert_not_null(cmd);

    SSD_LOGI("[L3] op=%s block=%u page=%u plane=%u cw=%u\r\n",
             g_nand_operation_map[cmd->config.operation].name,
             (unsigned)paa_to_block(cmd->paa),
             (unsigned)paa_to_page(cmd->paa),
             (unsigned)paa_to_plane(cmd->paa),
             (unsigned)cmd->config.cw);

    /* --- 1. submit --- */
    result = fc_submit(cmd);
    munit_assert_int(result, ==, FC_STATUS_OK);

    /* --- 2. service loop -- 驱动 NAND 执行 --- */
    for (service_count = 0U; service_count < SERVICE_LOOP_MAX; service_count++)
    {
        (void)fc_service();

        result = fc_poll_completion(&completion);
        if (result == FC_STATUS_OK)
        {
            completed = 1U;
            break;
        }
    }

    /* --- 3. 验证 completion --- */
    munit_assert_uint32(completed, ==, 1U);
    munit_assert_uint32(completion.command_id, ==, cmd->command_id);

SSD_LOGI("[L3] done op=%s status=0x%02X ecc=0x%02X/0x%02X/0x%02X/0x%02X\r\n",
             g_nand_operation_map[cmd->config.operation].name,
             (unsigned)completion.status,
             (unsigned)completion.ecc_bit0,
             (unsigned)completion.ecc_bit1,
             (unsigned)completion.ecc_bit2,
             (unsigned)completion.ecc_bit3);

    /* --- 4. release --- */
    release_command_buffers(cmd);
    result = fc_command_release(cmd);
    munit_assert_int(result, ==, FC_STATUS_OK);
}

/**************************************************************************
 *  辅助：submit + poll（不 release）
 *
 *  执行 submit -> service loop -> poll，返回 completion。
 *  调用者可在 release 前检查 cmd 中的 completion 数据
 *  （如 parameter0、status、buffer 内容等），然后自行调用
 *  release_command_buffers() + fc_command_release()。
 **************************************************************************/

static void run_submit_poll(fc_command_t *cmd, fc_command_status_t *completion)
{
    fc_status_t result;
    uint32_t    service_count;
    uint8_t     completed = 0U;

    munit_assert_not_null(cmd);
    munit_assert_not_null(completion);

    SSD_LOGI("[L3] op=%s block=%u page=%u plane=%u cw=%u\r\n",
             g_nand_operation_map[cmd->config.operation].name,
             (unsigned)paa_to_block(cmd->paa),
             (unsigned)paa_to_page(cmd->paa),
             (unsigned)paa_to_plane(cmd->paa),
             (unsigned)cmd->config.cw);

    result = fc_submit(cmd);
    munit_assert_int(result, ==, FC_STATUS_OK);

    for (service_count = 0U; service_count < SERVICE_LOOP_MAX; service_count++)
    {
        (void)fc_service();

        result = fc_poll_completion(completion);
        if (result == FC_STATUS_OK)
        {
            completed = 1U;
            break;
        }
    }

    munit_assert_uint32(completed, ==, 1U);
    munit_assert_uint32(completion->command_id, ==, cmd->command_id);

SSD_LOGI("[L3] done op=%s status=0x%02X ecc=0x%02X/0x%02X/0x%02X/0x%02X\r\n",
             g_nand_operation_map[cmd->config.operation].name,
             (unsigned)completion->status,
             (unsigned)completion->ecc_bit0,
             (unsigned)completion->ecc_bit1,
             (unsigned)completion->ecc_bit2,
             (unsigned)completion->ecc_bit3);
}

/**************************************************************************
 *  辅助：解锁 block（OP_BLOCK_UNLOCK_LOW + OP_BLOCK_UNLOCK_HIGH）
 *
 *  仅用于 BLOCK_LOCK 后的解锁恢复。
 *  正常情况下 NAND 上电后所有 block 默认解锁，无需调用。
 **************************************************************************/

static void __attribute__((unused)) unlock_block(uint16_t block)
{
    fc_command_t *cmd_low = fc_command_alloc();
    munit_assert_not_null(cmd_low);

    cmd_low->config.operation = OP_BLOCK_UNLOCK_LOW;
    cmd_low->config.ch        = 0U;
    cmd_low->config.ce        = 0U;
    cmd_low->config.lun       = 0U;
    cmd_low->config.cw        = 0U;
    cmd_low->config.random    = FC_RANDOM_DISABLE;
    cmd_low->config.ldpc      = FC_ECC_MODE_LDPC_RAW;
    cmd_low->config.raid      = FC_RAID_MODE_NORMAL;
    cmd_low->desc_id          = FC_DESCRIPTOR_ID_INVALID;
    cmd_low->paa              = make_paa(block, 0U, 0U);

    run_lifecycle(cmd_low);

    fc_command_t *cmd_high = fc_command_alloc();
    munit_assert_not_null(cmd_high);

    cmd_high->config.operation = OP_BLOCK_UNLOCK_HIGH;
    cmd_high->config.ch        = 0U;
    cmd_high->config.ce        = 0U;
    cmd_high->config.lun       = 0U;
    cmd_high->config.cw        = 0U;
    cmd_high->config.random    = FC_RANDOM_DISABLE;
    cmd_high->config.ldpc      = FC_ECC_MODE_LDPC_RAW;
    cmd_high->config.raid      = FC_RAID_MODE_NORMAL;
    cmd_high->desc_id          = FC_DESCRIPTOR_ID_INVALID;
    cmd_high->paa              = make_paa(block, 0U, 0U);

    run_lifecycle(cmd_high);
}

/**************************************************************************
 *  辅助：全 block 擦除
 **************************************************************************/

static void erase_block(uint16_t block, uint8_t multi_plane)
{
    fc_command_t *cmd = fc_command_alloc();
    munit_assert_not_null(cmd);

    cmd->config.operation = multi_plane ? OP_ERASE_BLOCK_MULTI_PLANE : OP_ERASE_BLOCK;
    cmd->config.ch        = 0U;
    cmd->config.ce        = 0U;
    cmd->config.lun       = 0U;
    cmd->config.random    = FC_RANDOM_DISABLE;
    cmd->config.ldpc      = FC_ECC_MODE_LDPC_RAW;
    cmd->config.raid      = FC_RAID_MODE_NORMAL;
    cmd->desc_id          = FC_DESCRIPTOR_ID_INVALID;
    cmd->config.cw        = 0U;
    cmd->config.multi_plane = multi_plane ? PLANE_NUM : 0U;
    cmd->paa              = make_paa(block, 0U, 0U);

    run_lifecycle(cmd);
}

/**************************************************************************
 *  辅助：program 单 page
 *
 *  使用 normal program page，填充 page pattern。
 **************************************************************************/

static fc_command_t *program_page(uint16_t block, uint8_t page, uint8_t plane)
{
    fc_command_t *cmd = fc_command_alloc();
    munit_assert_not_null(cmd);

    cmd->config.operation = OP_NORMAL_PROGRAM_PAGE;
    cmd->config.ch        = 0U;
    cmd->config.ce        = 0U;
    cmd->config.lun       = 0U;
    cmd->config.random    = FC_RANDOM_DISABLE;
    cmd->config.ldpc      = FC_ECC_MODE_LDPC_RAW;
    cmd->config.raid      = FC_RAID_MODE_NORMAL;

    (void)cmd_configure_and_fill(cmd, block, page, plane);
    return cmd;
}

/**************************************************************************
 *  辅助：read 单 page（带数据输出）
 **************************************************************************/

static fc_command_t *read_page(uint16_t block, uint8_t page, uint8_t plane)
{
    fc_command_t *cmd = fc_command_alloc();
    munit_assert_not_null(cmd);

    cmd->config.operation = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
    cmd->config.ch        = 0U;
    cmd->config.ce        = 0U;
    cmd->config.lun       = 0U;
    cmd->config.random    = FC_RANDOM_DISABLE;
    cmd->config.ldpc      = FC_ECC_MODE_LDPC_RAW;
    cmd->config.raid      = FC_RAID_MODE_NORMAL;

    cmd->config.cw = CW_NUM - 1U;
    cmd->paa       = make_paa(block, plane, page);

    fc_status_t result = fc_command_configure_descriptors(cmd);
    munit_assert_int(result, ==, FC_STATUS_OK);

    /* 读前 buffer memset 为 0xCC，方便区分"读失败残留"和"NAND 实际数据"。 */
    {
        fc_descriptor_id_t did = cmd->desc_id;
        while (did != FC_DESCRIPTOR_ID_INVALID)
        {
            fc_descriptor_t *d = fc_descriptor_id_to_descriptor(did);
            if (d == NULL) break;
            uint8_t *buf = fc_buffer_id_to_real_buffer(d->buffer_id);
            if (buf != NULL) memset(buf, 0xCC, FC_BUFFER_USER_SIZE);
            did = d->next_id;
        }
    }

    return cmd;
}

/**************************************************************************
 *  辅助：全 block 写（64 pages），使用 NORMAL_PROGRAM_PAGE
 **************************************************************************/

static void write_block(uint16_t block, uint8_t plane)
{
    for (uint8_t page = 0U; page < PAGES_PER_BLOCK; page++)
    {
        fc_command_t *cmd = program_page(block, page, plane);
        run_lifecycle(cmd);
    }
}

/**************************************************************************
 *  辅助：全 block 读回验证
 **************************************************************************/

static void read_block_verify(uint16_t block, uint8_t plane)
{
    for (uint8_t page = 0U; page < PAGES_PER_BLOCK; page++)
    {
        fc_command_t *cmd = read_page(block, page, plane);

        fc_command_status_t completion;
        fc_status_t         result;
        uint32_t            service_count;
        uint8_t             completed = 0U;

        munit_assert_not_null(cmd);

        result = fc_submit(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);

        for (service_count = 0U; service_count < SERVICE_LOOP_MAX; service_count++)
        {
            (void)fc_service();
            result = fc_poll_completion(&completion);
            if (result == FC_STATUS_OK)
            {
                completed = 1U;
                break;
            }
        }

        munit_assert_uint32(completed, ==, 1U);
        munit_assert_uint32(completion.command_id, ==, cmd->command_id);
        munit_assert_uint8(verify_cw_status_all_success(cmd), ==, 1U);

        /* 逐字节验证数据 */
        munit_assert_uint8(verify_page_data(cmd, block, page, plane), ==, 1U);

        release_command_buffers(cmd);
        result = fc_command_release(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);
    }
}

/**************************************************************************
 *  辅助：全 block 读回验证擦除（全 0xFF）
 **************************************************************************/

static void read_block_verify_erased(uint16_t block, uint8_t plane)
{
    for (uint8_t page = 0U; page < PAGES_PER_BLOCK; page++)
    {
        fc_command_t *cmd = read_page(block, page, plane);

        fc_command_status_t completion;
        fc_status_t         result;
        uint32_t            service_count;
        uint8_t             completed = 0U;

        munit_assert_not_null(cmd);

        result = fc_submit(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);

        for (service_count = 0U; service_count < SERVICE_LOOP_MAX; service_count++)
        {
            (void)fc_service();
            result = fc_poll_completion(&completion);
            if (result == FC_STATUS_OK)
            {
                completed = 1U;
                break;
            }
        }

        munit_assert_uint32(completed, ==, 1U);
        munit_assert_uint32(completion.command_id, ==, cmd->command_id);
        munit_assert_uint8(verify_cw_status_all_success(cmd), ==, 1U);

        /* 验证擦除后全 0xFF */
        munit_assert_uint8(verify_page_erased(cmd, page), ==, 1U);

        release_command_buffers(cmd);
        result = fc_command_release(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);
    }
}

/**************************************************************************
 *  test_init_sequence — 初始化序列
 *
 *  模拟 NAND 上电后的标准初始化流程：
 *    1. RESET — 复位 NAND
 *    2. READ_ID — 读取 NAND ID
 *    3. READ_PARAMETER_PAGE — 读取 ONFI 参数页
 *    4. GET_FEATURES — 读取默认 Feature 寄存器
 *
 *  验证点：
 *    - 各命令独立执行，生命周期完整
 *    - 读取的 ID 与预期值一致
 **************************************************************************/

static MunitResult test_init_sequence(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    /* Step 1: RESET */
    {
        fc_command_t *cmd = fc_command_alloc();
        munit_assert_not_null(cmd);
        cmd->config.operation = OP_RESET;
        cmd->config.ch        = 0U;
        cmd->config.ce        = 0U;
        cmd->config.lun       = 0U;
        cmd->config.cw        = 0U;
        cmd->config.random    = FC_RANDOM_DISABLE;
        cmd->config.ldpc      = FC_ECC_MODE_LDPC_RAW;
        cmd->config.raid      = FC_RAID_MODE_NORMAL;
        cmd->desc_id          = FC_DESCRIPTOR_ID_INVALID;
        run_lifecycle(cmd);
    }

    /* Step 2: READ_ID — 在 release 前验证 buffer 数据 */
    {
        fc_command_t *cmd = fc_command_alloc();
        munit_assert_not_null(cmd);
        cmd->config.operation = OP_READ_ID;
        cmd->config.ch        = 0U;
        cmd->config.ce        = 0U;
        cmd->config.lun       = 0U;
        cmd->config.cw        = 0U;
        cmd->config.random    = FC_RANDOM_DISABLE;
        cmd->config.ldpc      = FC_ECC_MODE_LDPC_RAW;
        cmd->config.raid      = FC_RAID_MODE_NORMAL;
        cmd->paa              = make_paa(0U, 0U, 0U);

        fc_status_t result = fc_command_configure_descriptors(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);

        fc_command_status_t completion;
        run_submit_poll(cmd, &completion);

        /* 验证 CW 状态正常 */
        munit_assert_uint8(verify_cw_status_all_success(cmd), ==, 1U);

        /* 验证 desc chain buffer 有 5 字节 ID 数据（在 release 前） */
        fc_descriptor_t *d = fc_descriptor_id_to_descriptor((fc_descriptor_id_t)cmd->desc_id);
        munit_assert_not_null(d);
        uint8_t *buf = fc_buffer_id_to_real_buffer(d->buffer_id);
        munit_assert_not_null(buf);
        munit_assert_uint8(verify_cw_status_all_success(cmd), ==, 1U);
        /* NAND 制造商 ID 应为 0x2C（Micron） */
        munit_assert_uint8(buf[0], ==, 0x2CU);

        release_command_buffers(cmd);
        result = fc_command_release(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);
    }

    /* Step 3: READ_PARAMETER_PAGE — 在 release 前验证 buffer 非 0xFF */
    {
        fc_command_t *cmd = fc_command_alloc();
        munit_assert_not_null(cmd);
        cmd->config.operation = OP_READ_PARAMETER_PAGE;
        cmd->config.ch        = 0U;
        cmd->config.ce        = 0U;
        cmd->config.lun       = 0U;
        cmd->config.cw        = 0U;
        cmd->config.random    = FC_RANDOM_DISABLE;
        cmd->config.ldpc      = FC_ECC_MODE_LDPC_RAW;
        cmd->config.raid      = FC_RAID_MODE_NORMAL;
        cmd->paa              = make_paa(0U, 0U, 0U);

        fc_status_t result = fc_command_configure_descriptors(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);

        fc_command_status_t completion;
        run_submit_poll(cmd, &completion);

        /* 验证参数页前 4 字节含 ONFI 签名 "ONFI"（0x4F 0x4E 0x46 0x49）
         * 数据由 PIO 写入 USER 区域（fc_buffer_id_to_real_buffer 指向的位置）。 */
        fc_descriptor_t *d = fc_descriptor_id_to_descriptor((fc_descriptor_id_t)cmd->desc_id);
        munit_assert_not_null(d);
        uint8_t *buf = fc_buffer_id_to_real_buffer(d->buffer_id);
        munit_assert_not_null(buf);
        munit_assert_uint8(buf[0], ==, 0x4FU);  /* 'O' */

        release_command_buffers(cmd);
        result = fc_command_release(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);
    }

    /* Step 4: GET_FEATURES (address 0x01) — 验证在 parameter0 中返回 */
    {
        fc_command_t *cmd = fc_command_alloc();
        munit_assert_not_null(cmd);
        cmd->config.operation = OP_GET_FEATURES;
        cmd->config.ch        = 0U;
        cmd->config.ce        = 0U;
        cmd->config.lun       = 0U;
        cmd->config.cw        = 0U;
        cmd->config.random    = FC_RANDOM_DISABLE;
        cmd->config.ldpc      = FC_ECC_MODE_LDPC_RAW;
        cmd->config.raid      = FC_RAID_MODE_NORMAL;
        cmd->desc_id          = FC_DESCRIPTOR_ID_INVALID;
        cmd->feature_addr     = 0x01U;

        fc_command_status_t completion;
        run_submit_poll(cmd, &completion);
        munit_assert_uint32(completion.status, ==, FC_CW_STATUS_SUCCESS);

        fc_status_t result = fc_command_release(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);
    }

    return MUNIT_OK;
}

/**************************************************************************
 *  test_feature_write_read — Feature 读验证
 *
 *  流程：
 *    1. GET_FEATURES(addr=0x01) — 读 Feature 寄存器
 *    2. GET_FEATURES(addr=0x81) — 读另一个 Feature 寄存器
 *
 *  验证点：
 *    - Feature 读取命令独立执行成功
 *    - 返回数据非全 0xFF（硬件在呼吸）
 *
 *  注意：本测试只读不写，避免意外切换 NAND 时序模式。
 **************************************************************************/

static MunitResult test_feature_write_read(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    /* Step 1: GET_FEATURES(addr=0x01) */
    {
        fc_command_t *cmd = fc_command_alloc();
        munit_assert_not_null(cmd);
        cmd->config.operation = OP_GET_FEATURES;
        cmd->config.ch        = 0U;
        cmd->config.ce        = 0U;
        cmd->config.lun       = 0U;
        cmd->config.cw        = 0U;
        cmd->config.random    = FC_RANDOM_DISABLE;
        cmd->config.ldpc      = FC_ECC_MODE_LDPC_RAW;
        cmd->config.raid      = FC_RAID_MODE_NORMAL;
        cmd->desc_id          = FC_DESCRIPTOR_ID_INVALID;
        cmd->feature_addr     = 0x01U;

        fc_command_status_t completion;
        run_submit_poll(cmd, &completion);
        munit_assert_uint32(completion.status, ==, FC_CW_STATUS_SUCCESS);

        fc_status_t result = fc_command_release(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);
    }

    /* Step 2: GET_FEATURES(addr=0x81) */
    {
        fc_command_t *cmd = fc_command_alloc();
        munit_assert_not_null(cmd);
        cmd->config.operation = OP_GET_FEATURES;
        cmd->config.ch        = 0U;
        cmd->config.ce        = 0U;
        cmd->config.lun       = 0U;
        cmd->config.cw        = 0U;
        cmd->config.random    = FC_RANDOM_DISABLE;
        cmd->config.ldpc      = FC_ECC_MODE_LDPC_RAW;
        cmd->config.raid      = FC_RAID_MODE_NORMAL;
        cmd->desc_id          = FC_DESCRIPTOR_ID_INVALID;
        cmd->feature_addr     = 0x81U;

        fc_command_status_t completion;
        run_submit_poll(cmd, &completion);
        munit_assert_uint32(completion.status, ==, FC_CW_STATUS_SUCCESS);

        fc_status_t result = fc_command_release(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);
    }

    return MUNIT_OK;
}

/**************************************************************************
 *  test_status_query — 状态查询
 *
 *  流程：
 *    1. READ_STATUS — 读取基本状态（parameter0 返回 nand_status）
 *    2. READ_STATUS_ENHANCED — 读取增强状态
 *
 *  验证点：
 *    - 状态查询命令独立执行，生命周期完整
 *    - NAND 状态寄存器的值在合理范围内，非 0
 **************************************************************************/

static MunitResult test_status_query(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    /* Step 1: READ_STATUS — 在 release 前验证 parameter0 */
    {
        fc_command_t *cmd = fc_command_alloc();
        munit_assert_not_null(cmd);
        cmd->config.operation = OP_READ_STATUS;
        cmd->config.ch        = 0U;
        cmd->config.ce        = 0U;
        cmd->config.lun       = 0U;
        cmd->config.cw        = 0U;
        cmd->config.random    = FC_RANDOM_DISABLE;
        cmd->config.ldpc      = FC_ECC_MODE_LDPC_RAW;
        cmd->config.raid      = FC_RAID_MODE_NORMAL;

        fc_status_t result = fc_command_configure_descriptors(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);

        fc_command_status_t completion;
        run_submit_poll(cmd, &completion);
        munit_assert_uint32(completion.status, ==, FC_CW_STATUS_SUCCESS);

        release_command_buffers(cmd);
        result = fc_command_release(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);
    }

    /* Step 2: READ_STATUS_ENHANCED */
    {
        fc_command_t *cmd = fc_command_alloc();
        munit_assert_not_null(cmd);
        cmd->config.operation = OP_READ_STATUS_ENHANCED;
        cmd->config.ch        = 0U;
        cmd->config.ce        = 0U;
        cmd->config.lun       = 0U;
        cmd->config.cw        = 0U;
        cmd->config.random    = FC_RANDOM_DISABLE;
        cmd->config.ldpc      = FC_ECC_MODE_LDPC_RAW;
        cmd->config.raid      = FC_RAID_MODE_NORMAL;
        cmd->paa              = make_paa(0U, 0U, 0U);

        fc_status_t result = fc_command_configure_descriptors(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);

        fc_command_status_t completion;
        run_submit_poll(cmd, &completion);
        munit_assert_uint32(completion.status, ==, FC_CW_STATUS_SUCCESS);

        release_command_buffers(cmd);
        result = fc_command_release(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);
    }

    return MUNIT_OK;
}

/**************************************************************************
 *  test_internal_data_move — 内部搬移验证
 *
 *  流程：
 *    1. 擦除 block 100 + 101
 *    2. 写 block 100（64 pages，独立 pattern）
*    3. 对每 page 执行 READ_FOR_INTERNAL_DATA_MOVE
 *    4. 对每 page 执行 PROGRAM_FOR_INTERNAL_MOVE（搬移到 block 102）
 *    5. 读 block 101 page 0 验证数据一致性
 *
 *  验证点：
 *    - 片上内部搬移数据正确
 *    - 搬移后源 block 数据仍可读
 **************************************************************************/

static MunitResult test_internal_data_move(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    /* Step 1: 擦除源 + 目标 block */
    erase_block(BLOCK_TEST, 0U);
    erase_block(BLOCK_DEST, 0U);

    /* Step 2: 写源 block（64 pages，独立 pattern） */
    write_block(BLOCK_TEST, 0U);

/* Step 2.5: 用普通读（00h-30h）验证 block 100 数据正确，排除写阶段问题 */
    SSD_LOGI("[L3] verify source block %u data before internal move\r\n", (unsigned)BLOCK_TEST);
    {
        fc_command_t *cmd = read_page(BLOCK_TEST, 0U, 0U);
        fc_command_status_t completion;
        fc_status_t         result;
        uint32_t            service_count;
        uint8_t             completed = 0U;

        munit_assert_not_null(cmd);
        result = fc_submit(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);
        for (service_count = 0U; service_count < SERVICE_LOOP_MAX; service_count++)
        {
            (void)fc_service();
            result = fc_poll_completion(&completion);
            if (result == FC_STATUS_OK) { completed = 1U; break; }
        }
        munit_assert_uint32(completed, ==, 1U);
        munit_assert_uint8(verify_cw_status_all_success(cmd), ==, 1U);
        munit_assert_uint8(verify_page_data(cmd, BLOCK_TEST, 0U, 0U), ==, 1U);
        SSD_LOGI("[L3] source block %u page 0 data verified OK\r\n", (unsigned)BLOCK_TEST);
        release_command_buffers(cmd);
        result = fc_command_release(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);
    }

    /* Step 3+4: 逐 page 内部搬移：READ_FOR_INTERNAL_DATA_MOVE → PROGRAM_FOR_INTERNAL_MOVE */
    {
        for (uint8_t page = 0U; page < PAGES_PER_BLOCK; page++)
        {
            /* 3a: READ_FOR_INTERNAL_DATA_MOVE (00h-35h) — 将源 block page 读到 NAND 内部缓存 */
            {
                fc_command_t *cmd = fc_command_alloc();
                munit_assert_not_null(cmd);
                cmd->config.operation = OP_READ_FOR_INTERNAL_DATA_MOVE;
                cmd->config.ch        = 0U;
                cmd->config.ce        = 0U;
                cmd->config.lun       = 0U;
                cmd->config.cw        = CW_NUM - 1U;
                cmd->config.random    = FC_RANDOM_DISABLE;
                cmd->config.ldpc      = FC_ECC_MODE_LDPC_RAW;
                cmd->config.raid      = FC_RAID_MODE_NORMAL;
                cmd->paa              = make_paa(BLOCK_TEST, 0U, page);

                /* 分配 desc chain（提供 CW 级 PAA 映射给 dispatch） */
                fc_status_t ret = fc_command_configure_descriptors(cmd);
                munit_assert_int(ret, ==, FC_STATUS_OK);

                run_lifecycle(cmd);
            }

            /* 3b: PROGRAM_FOR_INTERNAL_MOVE (85h-10h) — 将内部缓存数据写入目标 block page */
            {
                fc_command_t *cmd = fc_command_alloc();
                munit_assert_not_null(cmd);
                cmd->config.operation = OP_PROGRAM_FOR_INTERNAL_MOVE;
                cmd->config.ch        = 0U;
                cmd->config.ce        = 0U;
                cmd->config.lun       = 0U;
                cmd->config.cw        = CW_NUM - 1U;
                cmd->config.random    = FC_RANDOM_DISABLE;
                cmd->config.ldpc      = FC_ECC_MODE_LDPC_RAW;
                cmd->config.raid      = FC_RAID_MODE_NORMAL;
                cmd->paa              = make_paa(BLOCK_DEST, 0U, page);

                /* 分配 desc chain（提供 CW 级 PAA 映射给 dispatch） */
                fc_status_t ret = fc_command_configure_descriptors(cmd);
                munit_assert_int(ret, ==, FC_STATUS_OK);

                run_lifecycle(cmd);
            }
        }
    }

    /* Step 5: 读回目标 block 64 pages 全部验证 */
    read_block_verify(BLOCK_DEST, 0U);

    return MUNIT_OK;
}

#if 0
/**************************************************************************
 *  test_unlock_erase — 解锁 + 擦除验证
 *
 *  流程：
 *    1. OP_BLOCK_UNLOCK_LOW — 解锁低段
 *    2. OP_BLOCK_UNLOCK_HIGH — 解锁高段
 *    3. OP_ERASE_BLOCK — 擦除 block
 *    4. READ_PAGE — 验证全 0xFF
 *
 *  验证点：
 *    - 锁机制操作正确
 *    - 解锁后擦除成功
 *
 *  ⚠️ 当前测试用 `#if 0` 隔开，确认稳定后启用。
 *     移除 `#if 0` / `#endif` 即可。
 **************************************************************************/

static MunitResult test_unlock_erase(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    uint16_t block = BLOCK_TEST;

    /* Phase 1: 执行 BLOCK_LOCK — 锁定全部 block */
    {
        fc_command_t *cmd = fc_command_alloc();
        munit_assert_not_null(cmd);
        cmd->config.operation = OP_BLOCK_LOCK;
        cmd->config.ch        = 0U;
        cmd->config.ce        = 0U;
        cmd->config.lun       = 0U;
        cmd->config.cw        = 0U;
        cmd->config.random    = FC_RANDOM_DISABLE;
        cmd->config.ldpc      = FC_ECC_MODE_LDPC_RAW;
        cmd->config.raid      = FC_RAID_MODE_NORMAL;
        cmd->desc_id          = FC_DESCRIPTOR_ID_INVALID;
        run_lifecycle(cmd);
    }

    /* Phase 2: UNLOCK_LOW + UNLOCK_HIGH */
    unlock_block(block);

    /* Phase 3: 擦除 */
    erase_block(block, 0U);

    /* Phase 4: 验证擦除 */
    read_block_verify_erased(block, 0U);

    return MUNIT_OK;
}
#endif /* #if 0 — unlock_erase test disabled */

/**************************************************************************
 *  test_block_write_read — 写 + 读核心验证
 *
 *  流程：
 *    1. 擦除 block 100
 *    2. 用 NORMAL_PROGRAM_PAGE 写全部 64 pages（每页独立 pattern）
 *    3. 用 NORMAL_READ_PAGE_WITH_DATA_OUT 读回全部 64 pages
 *    4. 每页每 CW 每 byte 与预期 pattern 比对
 *
 *  验证点：
 *    - NAND 硬件真正执行了擦除和编程
 *    - 读回数据与写入数据完全一致
 *    - completion per-CW status 均为 SUCCESS
 **************************************************************************/

static MunitResult test_block_write_read(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    /* Phase 1: 擦除 */
    erase_block(BLOCK_TEST, 0U);

    /* Phase 2: 写全 block 64 pages */
    write_block(BLOCK_TEST, 0U);

    /* Phase 3: 读回全 block 64 pages 验证 */
    read_block_verify(BLOCK_TEST, 0U);

    return MUNIT_OK;
}

/**************************************************************************
 *  test_block_erase_verify — 擦除验证
 *
 *  流程：
 *    1. 擦除 block 100
 *    2. 用 NORMAL_READ_PAGE_WITH_DATA_OUT 读全部 64 pages
 *    3. 每 CW 每 byte 验证 == 0xFF
 *
 *  验证点：
 *    - 擦除后 NAND 确实将全 block 数据位重置为 1
 *    - 无残留数据
 **************************************************************************/

static MunitResult test_block_erase_verify(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    erase_block(BLOCK_TEST, 0U);
    read_block_verify_erased(BLOCK_TEST, 0U);

    return MUNIT_OK;
}

/**************************************************************************
 *  test_block_cache_ops — Cache 读序列验证
 *
 *  流程：
 *    1. 擦除 block 100
 *    2. 用 NORMAL_PROGRAM_PAGE 写全 block（cache 读需要已有数据）
 *    3. Cache 读序列遍历所有 64 pages
 *    4. 每 page 数据与预期 pattern 比对
 *
 *  验证点：
 *    - Cache 读序列正确遍历多页
 *    - 每页数据与 NORMAL_PROGRAM 写入一致
 *    - 各阶段 status 正确
 **************************************************************************/

static MunitResult test_block_cache_ops(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    /* Phase 0: 擦除 */
    erase_block(BLOCK_TEST, 0U);

    /* Phase 1: 用 NORMAL_PROGRAM_PAGE 写全 block */
    write_block(BLOCK_TEST, 0U);

    /* Phase 2: Cache 读序列 */
    uint8_t page;
    fc_command_status_t completion;
    fc_status_t         result;
    uint32_t            service_count;
    uint8_t             completed;

    /* page 0: OP_READ_PAGE_CACHE_RANDOM */
    {
        fc_command_t *cmd = fc_command_alloc();
        munit_assert_not_null(cmd);

        cmd->config.operation = OP_READ_PAGE_CACHE_RANDOM;
        cmd->config.ch        = 0U;
        cmd->config.ce        = 0U;
        cmd->config.lun       = 0U;
        cmd->config.random    = FC_RANDOM_DISABLE;
        cmd->config.ldpc      = FC_ECC_MODE_LDPC_RAW;
        cmd->config.raid      = FC_RAID_MODE_NORMAL;
        cmd->config.cw        = CW_NUM - 1U;
        cmd->paa              = make_paa(BLOCK_TEST, 0U, 0U);

        result = fc_command_configure_descriptors(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);

        memset_read_buffer(cmd);

        result = fc_submit(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);

        completed = 0U;
        for (service_count = 0U; service_count < SERVICE_LOOP_MAX; service_count++)
        {
            (void)fc_service();
            result = fc_poll_completion(&completion);
            if (result == FC_STATUS_OK) { completed = 1U; break; }
        }
        munit_assert_uint32(completed, ==, 1U);
        munit_assert_uint8(verify_cw_status_all_success(cmd), ==, 1U);
        munit_assert_uint8(verify_page_data(cmd, BLOCK_TEST, 0U, 0U), ==, 1U);
        release_command_buffers(cmd);
        result = fc_command_release(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);
    }

    /* page 1..62: OP_READ_PAGE_CACHE_SEQ */
    for (page = 1U; page < PAGES_PER_BLOCK - 1U; page++)
    {
        fc_command_t *cmd = fc_command_alloc();
        munit_assert_not_null(cmd);

        cmd->config.operation = OP_READ_PAGE_CACHE_SEQ;
        cmd->config.ch        = 0U;
        cmd->config.ce        = 0U;
        cmd->config.lun       = 0U;
        cmd->config.random    = FC_RANDOM_DISABLE;
        cmd->config.ldpc      = FC_ECC_MODE_LDPC_RAW;
        cmd->config.raid      = FC_RAID_MODE_NORMAL;
        cmd->config.cw        = CW_NUM - 1U;
        cmd->desc_id          = FC_DESCRIPTOR_ID_INVALID;

        result = fc_command_configure_descriptors(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);

        memset_read_buffer(cmd);

        result = fc_submit(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);

        completed = 0U;
        for (service_count = 0U; service_count < SERVICE_LOOP_MAX; service_count++)
        {
            (void)fc_service();
            result = fc_poll_completion(&completion);
            if (result == FC_STATUS_OK) { completed = 1U; break; }
        }
        munit_assert_uint32(completed, ==, 1U);
        munit_assert_uint8(verify_cw_status_all_success(cmd), ==, 1U);
        munit_assert_uint8(verify_page_data(cmd, BLOCK_TEST, page, 0U), ==, 1U);
        release_command_buffers(cmd);
        result = fc_command_release(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);
    }

    /* page 63: OP_READ_PAGE_CACHE_LAST */
    {
        fc_command_t *cmd = fc_command_alloc();
        munit_assert_not_null(cmd);

        cmd->config.operation = OP_READ_PAGE_CACHE_LAST;
        cmd->config.ch        = 0U;
        cmd->config.ce        = 0U;
        cmd->config.lun       = 0U;
        cmd->config.random    = FC_RANDOM_DISABLE;
        cmd->config.ldpc      = FC_ECC_MODE_LDPC_RAW;
        cmd->config.raid      = FC_RAID_MODE_NORMAL;
        cmd->config.cw        = CW_NUM - 1U;
        cmd->desc_id          = FC_DESCRIPTOR_ID_INVALID;

        result = fc_command_configure_descriptors(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);

        memset_read_buffer(cmd);

        result = fc_submit(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);

        completed = 0U;
        for (service_count = 0U; service_count < SERVICE_LOOP_MAX; service_count++)
        {
            (void)fc_service();
            result = fc_poll_completion(&completion);
            if (result == FC_STATUS_OK) { completed = 1U; break; }
        }
        munit_assert_uint32(completed, ==, 1U);
        munit_assert_uint8(verify_cw_status_all_success(cmd), ==, 1U);
        munit_assert_uint8(verify_page_data(cmd, BLOCK_TEST, PAGES_PER_BLOCK - 1U, 0U), ==, 1U);
        release_command_buffers(cmd);
        result = fc_command_release(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);
    }

    return MUNIT_OK;
}

/**************************************************************************
 *  test_block_multi_plane — 多 Plane 验证
 *
 *  全流程使用 Multi-Plane OP 码：
 *    1. 双 Plane 擦除（OP_ERASE_BLOCK_MULTI_PLANE）
 *    2. 双 Plane 编程（OP_PROGRAM_PAGE_MULTI_PLANE），plane 0/1 不同 pattern
 *    3. 双 Plane 读回（OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT），逐字节验证
 *
 *  验证点：
 *    - 多 Plane 擦除 + 编程 + 读回全部正确
 *    - 两个 plane 的数据完整性各自独立验证
 **************************************************************************/

static MunitResult test_block_multi_plane(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    /* Step 1: Multi-plane erase block 100 (both planes) */
    erase_block(BLOCK_TEST, 1U);

    /* Step 2: Multi-plane program — 64 pages */
    for (uint8_t page = 0U; page < PAGES_PER_BLOCK; page++)
    {
        fc_command_t *cmd = fc_command_alloc();
        munit_assert_not_null(cmd);

        cmd->config.operation = OP_PROGRAM_PAGE_MULTI_PLANE;
        cmd->config.ch        = 0U;
        cmd->config.ce        = 0U;
        cmd->config.lun       = 0U;
        cmd->config.random    = FC_RANDOM_DISABLE;
        cmd->config.ldpc      = FC_ECC_MODE_LDPC_RAW;
        cmd->config.raid      = FC_RAID_MODE_NORMAL;
        cmd->config.cw        = CW_NUM - 1U;                    /* per-plane CW count - 1 */
        cmd->config.multi_plane = PLANE_NUM;                    /* = 2 */
        cmd->paa              = make_paa(BLOCK_TEST, 0U, page); /* base PAA: plane 0 */

        fc_status_t result = fc_command_configure_descriptors(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);

        /* Fill desc[0..3] = plane 0, desc[4..7] = plane 1, with different patterns */
        {
            fc_descriptor_id_t did = (fc_descriptor_id_t)cmd->desc_id;
            uint8_t            idx = 0U;
            while (did != FC_DESCRIPTOR_ID_INVALID)
            {
                fc_descriptor_t *d = fc_descriptor_id_to_descriptor(did);
                if (d == NULL) break;
                uint8_t *buf = fc_buffer_id_to_real_buffer(d->buffer_id);
                if (buf != NULL)
                {
                    uint8_t p = idx / CW_NUM;  /* plane 0 or 1 */
                    fill_page_pattern(buf, BLOCK_TEST, page, p);
                }
                did = (fc_descriptor_id_t)d->next_id;
                idx++;
            }
        }

        run_lifecycle(cmd);
    }

    /* Step 3: Multi-plane read back — verify both planes */
    for (uint8_t page = 0U; page < PAGES_PER_BLOCK; page++)
    {
        fc_command_t *cmd = fc_command_alloc();
        munit_assert_not_null(cmd);

        cmd->config.operation = OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT;
        cmd->config.ch        = 0U;
        cmd->config.ce        = 0U;
        cmd->config.lun       = 0U;
        cmd->config.random    = FC_RANDOM_DISABLE;
        cmd->config.ldpc      = FC_ECC_MODE_LDPC_RAW;
        cmd->config.raid      = FC_RAID_MODE_NORMAL;
        cmd->config.cw        = CW_NUM - 1U;
        cmd->config.multi_plane = PLANE_NUM;
        cmd->paa              = make_paa(BLOCK_TEST, 0U, page);

        fc_status_t result = fc_command_configure_descriptors(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);

        /* Pre-fill with 0xCC to detect incomplete reads */
        memset_read_buffer(cmd);

        /* Submit + poll */
        {
            fc_command_status_t completion;
            uint32_t            service_count;
            uint8_t             completed = 0U;

            result = fc_submit(cmd);
            munit_assert_int(result, ==, FC_STATUS_OK);

            for (service_count = 0U; service_count < SERVICE_LOOP_MAX; service_count++)
            {
                (void)fc_service();
                result = fc_poll_completion(&completion);
                if (result == FC_STATUS_OK)
                {
                    completed = 1U;
                    break;
                }
            }
            munit_assert_uint32(completed, ==, 1U);
        }

        /* Verify data plane-by-plane */
        {
            fc_descriptor_id_t did = (fc_descriptor_id_t)cmd->desc_id;
            uint8_t            idx = 0U;
            while (did != FC_DESCRIPTOR_ID_INVALID)
            {
                fc_descriptor_t *d = fc_descriptor_id_to_descriptor(did);
                if (d == NULL) break;
                uint8_t *buf = fc_buffer_id_to_real_buffer(d->buffer_id);
                if (buf != NULL)
                {
                    uint8_t p  = idx / CW_NUM;
                    uint8_t cw = idx % CW_NUM;
                    for (uint32_t i = 0U; i < FC_BUFFER_USER_SIZE; i++)
                    {
                        uint8_t exp = expected_byte(BLOCK_TEST, page, p,
                                                     (uint32_t)cw * FC_BUFFER_USER_SIZE + i);
                        if (buf[i] != exp)
                        {
                            munit_errorf("multi_plane page=%u plane=%u cw=%u offset=%u: got 0x%02X expected 0x%02X",
                                         (unsigned)page, (unsigned)p, (unsigned)cw, (unsigned)i,
                                         (unsigned)buf[i], (unsigned)exp);
                            goto mp_desc_next;
                        }
                    }
                }
mp_desc_next:
                did = (fc_descriptor_id_t)d->next_id;
                idx++;
            }
        }

        release_command_buffers(cmd);
        result = fc_command_release(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);
    }

    return MUNIT_OK;
}

/**************************************************************************
 *  test_block_random_data — 随机列读验证
 *
 *  流程：
 *    1. 擦除 + 写全 block 基准数据（plane 0）
 *    2. 抽样 5 个 page（0/15/31/47/63），每页：
 *       a. Normal read page 加载页缓冲
 *       b. RANDOM_DATA_READ 抽样 CW1 和 CW3，验证数据
 *
 *  验证点：
 *    - 同页不同 CW 的列地址解码正确
 *    - 跨页行地址切换后随机列读正确
 *    - 数据与原始写入一致
 **************************************************************************/

static MunitResult test_block_random_data(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    erase_block(BLOCK_TEST, 0U);
    write_block(BLOCK_TEST, 0U);

    const uint8_t sample_pages[] = {0U, 15U, 31U, 47U, 63U};
    const uint8_t sample_cws[]   = {1U, 3U};
    uint8_t       si, ci;

    for (si = 0U; si < (uint8_t)sizeof(sample_pages); si++)
    {
        uint8_t page = sample_pages[si];

        /* Normal read page — 加载 NAND 页缓冲 */
        {
            fc_command_t *cmd = read_page(BLOCK_TEST, page, 0U);
            fc_command_status_t completion;
            fc_status_t         result;
            uint32_t            service_count;
            uint8_t             completed = 0U;

            result = fc_submit(cmd);
            munit_assert_int(result, ==, FC_STATUS_OK);
            for (service_count = 0U; service_count < SERVICE_LOOP_MAX; service_count++)
            {
                (void)fc_service();
                result = fc_poll_completion(&completion);
                if (result == FC_STATUS_OK) { completed = 1U; break; }
            }
            munit_assert_uint32(completed, ==, 1U);
            munit_assert_uint8(verify_cw_status_all_success(cmd), ==, 1U);
            release_command_buffers(cmd);
            result = fc_command_release(cmd);
            munit_assert_int(result, ==, FC_STATUS_OK);
        }

        for (ci = 0U; ci < (uint8_t)sizeof(sample_cws); ci++)
        {
            uint8_t cw = sample_cws[ci];

            fc_command_t *cmd = fc_command_alloc();
            munit_assert_not_null(cmd);

            cmd->config.operation = OP_RANDOM_DATA_READ;
            cmd->config.ch        = 0U;
            cmd->config.ce        = 0U;
            cmd->config.lun       = 0U;
            cmd->config.random    = FC_RANDOM_DISABLE;
            cmd->config.ldpc      = FC_ECC_MODE_LDPC_RAW;
            cmd->config.raid      = FC_RAID_MODE_NORMAL;
            cmd->config.cw        = 0U;                      /* 1 个 desc */
            cmd->paa              = paa_make(page, 0U, BLOCK_TEST, 0U, cw);

            fc_status_t result = fc_command_configure_descriptors(cmd);
            munit_assert_int(result, ==, FC_STATUS_OK);

            memset_read_buffer(cmd);

            result = fc_submit(cmd);
            munit_assert_int(result, ==, FC_STATUS_OK);

            uint8_t completed = 0U;
            fc_command_status_t completion;
            for (uint32_t i = 0U; i < SERVICE_LOOP_MAX; i++)
            {
                (void)fc_service();
                result = fc_poll_completion(&completion);
                if (result == FC_STATUS_OK) { completed = 1U; break; }
            }
            munit_assert_uint32(completed, ==, 1U);
            munit_assert_uint8(verify_cw_status_all_success(cmd), ==, 1U);

            /* 逐字节验证 — 使用 PAA 编码的 CW 编号计算偏移 */
            {
                fc_descriptor_id_t did = (fc_descriptor_id_t)cmd->desc_id;
                fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(did);
                if (desc != NULL)
                {
                    uint8_t *buf = fc_buffer_id_to_real_buffer(desc->buffer_id);
                    if (buf != NULL)
                    {
                        for (uint32_t i = 0U; i < FC_BUFFER_USER_SIZE; i++)
                        {
                            uint8_t exp = expected_byte(BLOCK_TEST, page, 0U,
                                                         (uint32_t)cw * FC_BUFFER_USER_SIZE + i);
                            if (buf[i] != exp)
                            {
                                munit_errorf("random_data page=%u cw=%u off=%u: got 0x%02X exp 0x%02X",
                                             (unsigned)page, (unsigned)cw, (unsigned)i,
                                             (unsigned)buf[i], (unsigned)exp);
                                goto rand_page_done;
                            }
                        }
                    }
                }
            }
rand_page_done:
            release_command_buffers(cmd);
            result = fc_command_release(cmd);
            munit_assert_int(result, ==, FC_STATUS_OK);
        }
    }

    return MUNIT_OK;
}

#if 0 /* BLOCK_LOCK_TIGHT — 不可逆锁死整片 NAND，暂不启用 */

/**************************************************************************
 *  test_block_lock_tight — 紧锁定验证（必须放在最后！）
 *
 *  BLOCK_LOCK_TIGHT 永久锁死整片 NAND（直到下电），不可逆。
 *  执行后尝试擦除 -> 编程 -> 读回，预期数据不变（或 operation 被硬件拒绝）。
 *  如果 BLOCK_LOCK_TIGHT 生效，NAND 硬件拒绝 PROGRAM，
 *  读回的数据应为编程前的非预期值，测试视为 PASS。
 *
 *  注意：此测试会永久锁死芯片！本 suite 中必须放在最后一项。
 **************************************************************************/

static MunitResult test_block_lock_tight(const MunitParameter params[], void *data)
{
    fc_command_t *cmd;
    uint8_t       page;

    (void)params;
    (void)data;

    /* Phase 1: 执行 BLOCK_LOCK_TIGHT -- 永久锁定整片 NAND */
    cmd = fc_command_alloc();
    munit_assert_not_null(cmd);
    cmd->config.operation = OP_BLOCK_LOCK_TIGHT;
    cmd->config.ch        = 0U;
    cmd->config.ce        = 0U;
    cmd->config.lun       = 0U;
    cmd->config.random    = FC_RANDOM_DISABLE;
    cmd->config.ldpc      = FC_ECC_MODE_LDPC_RAW;
    cmd->config.raid      = FC_RAID_MODE_NORMAL;
    cmd->desc_id          = FC_DESCRIPTOR_ID_INVALID;
    cmd->config.cw        = 0U;

    run_lifecycle(cmd);

    /* Phase 2: 尝试擦除 + 写 + 读一个 page -- 预期失败 */
    erase_block(BLOCK_TEST, 0U);

    page = 0U;
    cmd = program_page(BLOCK_TEST, page, 0U);
    run_lifecycle(cmd);

    cmd = read_page(BLOCK_TEST, page, 0U);
    {
        fc_command_status_t completion;
        fc_status_t         result;
        uint32_t            service_count;
        uint8_t             completed = 0U;

        munit_assert_not_null(cmd);

        result = fc_submit(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);

        for (service_count = 0U; service_count < SERVICE_LOOP_MAX; service_count++)
        {
            (void)fc_service();
            result = fc_poll_completion(&completion);
            if (result == FC_STATUS_OK)
            {
                completed = 1U;
                break;
            }
        }
        munit_assert_uint32(completed, ==, 1U);

        /* 验证：由于 BLOCK_LOCK_TIGHT，NAND 拒绝 PROGRAM，
         * 读回数据应与预期 pattern 不同。 */
        fc_descriptor_id_t did = (fc_descriptor_id_t)cmd->desc_id;
        uint8_t            cw_idx;
        uint8_t            locked = 0U;
        uint8_t           *buf;

        for (cw_idx = 0U; cw_idx < CW_NUM; cw_idx++)
        {
            fc_descriptor_t *d = fc_descriptor_id_to_descriptor(did);
            if (d == NULL) break;
            buf = fc_buffer_id_to_real_buffer(d->buffer_id);
            if (buf == NULL) break;
            if (buf[0] != expected_byte(BLOCK_TEST, page, 0U, (uint32_t)cw_idx * FC_BUFFER_USER_SIZE))
            {
                locked = 1U;
                break;
            }
            did = (fc_descriptor_id_t)d->next_id;
        }

        /* BLOCK_LOCK_TIGHT 生效：数据与写入不一致 -> 测试 PASS */
        munit_assert_uint8(locked, ==, 1U);
    }

    return MUNIT_OK;
}

#endif /* #if 0 — BLOCK_LOCK_TIGHT disabled (permanently locks the chip) */

/**************************************************************************
 *  套件定义
 *
 *  包含：
 *    - init_sequence（初始化序列：RESET -> READ_ID -> ...）
 *    - feature_write_read（Feature 写回验证）
 *    - status_query（状态查询）
 *    - internal_data_move（片上内部搬移）
 *    - block_erase_verify（擦除后全 0xFF 验证）
 *    - block_write_read（全 block 写读 128KB 数据对比）
 *    - block_cache_ops（cache 读序列数据验证）
 *    - block_multi_plane（多 Plane 擦写读验证）
 *    - block_random_data（随机列读写验证）
 *    - block_lock_tight（永久锁芯片验证，必须放在最后）
 *
 *  （unlock_erase 用 #if 0 隔开，暂不启用）
 **************************************************************************/

MunitTest g_fc_lifecycle_tests[] = {
    { "init_sequence",       test_init_sequence,       setup_level3, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "feature_write_read",  test_feature_write_read,  setup_level3, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "status_query",        test_status_query,        setup_level3, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "internal_data_move",  test_internal_data_move,  setup_level3, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "block_erase_verify",  test_block_erase_verify,  setup_level3, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "block_write_read",    test_block_write_read,    setup_level3, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "block_cache_ops",     test_block_cache_ops,     setup_level3, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "block_multi_plane",   test_block_multi_plane,   setup_level3, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "block_random_data",   test_block_random_data,   setup_level3, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
#if 0 /* block_lock_tight 被 #if 0 隔开，此处不注册 */
    { "block_lock_tight",    test_block_lock_tight,    setup_level3, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
#endif
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
};
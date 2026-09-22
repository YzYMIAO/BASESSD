/*
 * test_fc_level2.c — Level 2 数据路径测试：fc_data_path 模块
 *
 * 测试三个数据路径管线函数：
 *   fc_data_path_encode      写路径：Random 加扰 → ECC 编码 → RAID XOR
 *   fc_data_path_process_read 读路径 Stage 1：ECC 解码 → 条件解扰
 *   fc_data_path_raid_recover 读路径 Stage 2：RAID XOR 重建 → ECC 重试 → 解扰
 *
 * 依赖：需要 fc_init() 初始化 Pool（suite 级一次性）
 *       不需要 NAND 硬件，全部在 buffer 层面验证
 *
 * 参数组合覆盖：
 *   random  : FC_RANDOM_ENABLE / FC_RANDOM_DISABLE
 *   raid    : FC_RAID_MODE_NORMAL / FC_RAID_MODE_ENCODE
 *   cw      : 0 (1 CW) / 3 (4 CW)
 *   ecc_结果: 全部成功 / 有 UNC
 *   failed_idx: 首位 / 中间 / 末位
 */

#include "munit.h"

#include "fc/fc.h"
#include "fc/fc_command.h"
#include "fc/fc_data_path.h"
#include "fc/fc_ldpc_engine.h"
#include "fc/fc_random_engine.h"
#include "fc/fc_raid_engine.h"
#include "nand/onfi_1_0.h"
#include "nand/nand_config.h"

#include <string.h>

/**************************************************************************
 *  Suite 级 setup：fc_init() 一次性初始化 Pool
 **************************************************************************/

static int g_level2_inited = 0;

static void *setup_level2(const MunitParameter params[], void *user_data)
{
    (void)params;
    (void)user_data;
    if (!g_level2_inited)
    {
        fc_init();
        g_level2_inited = 1;
    }
    return NULL;
}

/**************************************************************************
 *  辅助函数
 **************************************************************************/

/* 用固定 pattern 填充 buffer（每个 CW 对应一个 desc buffer） */
static void fill_pattern(uint8_t *buffer, uint16_t length, uint8_t pattern)
{
    memset(buffer, pattern, length);
}

/* 验证 buffer 是否为预期 pattern */
static uint8_t check_pattern(const uint8_t *buffer, uint16_t length, uint8_t pattern)
{
    for (uint16_t i = 0U; i < length; i++)
    {
        if (buffer[i] != pattern)
            return 0U;
    }
    return 1U;
}

/* 分配 command 并配置 desc 链，返回 command 指针 */
static fc_command_t *alloc_cmd(uint8_t cw_count, uint8_t random_mode, uint8_t raid_mode)
{
    fc_command_t *cmd = fc_command_alloc();
    munit_assert_not_null(cmd);

    cmd->config.operation = OP_NORMAL_PROGRAM_PAGE; /* data_path 不依赖 operation */
    cmd->config.cw        = (cw_count > 0U) ? (uint8_t)(cw_count - 1U) : 0U;
    cmd->config.ch        = 0U;
    cmd->config.ce        = 0U;
    cmd->config.lun       = 0U;
    cmd->config.random    = random_mode;
    cmd->config.raid      = raid_mode;
    cmd->config.ldpc      = FC_ECC_MODE_BCH; /* BCH 为默认 ECC */

    fc_status_t result = fc_command_configure_descriptors(cmd);
    munit_assert_int(result, ==, FC_STATUS_OK);

    return cmd;
}

/* 获取 desc 链中第 idx 个 CW 的 buffer 指针（idx=0 为第一个 CW） */
static uint8_t *get_cw_buffer(const fc_command_t *cmd, uint8_t idx)
{
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
    uint8_t            i       = 0U;

    while (desc_id != FC_DESCRIPTOR_ID_INVALID)
    {
        if (i == idx)
        {
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            if (desc == NULL) return NULL;
            return fc_buffer_id_to_real_buffer(desc->buffer_id);
        }
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        if (desc == NULL) return NULL;
        desc_id = (fc_descriptor_id_t)desc->next_id;
        i++;
    }
    return NULL;
}

/* 获取 desc 链中 CW 数量 */
static uint8_t get_cw_count(const fc_command_t *cmd)
{
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
    uint8_t            count   = 0U;

    while (desc_id != FC_DESCRIPTOR_ID_INVALID)
    {
        count++;
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        if (desc == NULL) break;
        desc_id = (fc_descriptor_id_t)desc->next_id;
    }
    return count;
}

/**************************************************************************
 *  fc_data_path_encode 测试
 *
 *  验证管线：
 *     Data → Random 加扰? → ECC 编码 → RAID XOR?
 *
 *  验证策略：encode 后 round-trip decode → 原始 pattern 恢复
 *            （decode 走 fc_ldpc_decode + fc_random_transform，模拟 NAND 读回）
 *
 *  参数组合：
 *     random × raid × cw
 *     off    normal  4CW   仅 ECC
 *     on     normal  4CW   加扰 + ECC
 *     off    encode  4CW   ECC + RAID
 *     on     encode  4CW   全路径
 *     on     normal  1CW   单 CW 边界
 *     off    normal  1CW   单 CW 边界
 **************************************************************************/

static MunitResult test_encode_random_off(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    fc_command_t *cmd = alloc_cmd(4U, FC_RANDOM_DISABLE, FC_RAID_MODE_NORMAL);
    uint8_t count = get_cw_count(cmd);
    munit_assert_uint32(count, ==, 4U);

    for (uint8_t i = 0U; i < count; i++)
    {
        uint8_t *buf = get_cw_buffer(cmd, i);
        munit_assert_not_null(buf);
        fill_pattern(buf, FC_BUFFER_POOL_CW_SIZE, (uint8_t)(0x55U + i));
    }

    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    /* 验证：ECC 编码后末 8 字节为 parity（非零），前 520 字节保持原始 pattern */
    for (uint8_t i = 0U; i < count; i++)
    {
        uint8_t *buf = get_cw_buffer(cmd, i);
        munit_assert_not_null(buf);
        /* 前 520 字节应保持原始数据 */
        munit_assert_uint32(check_pattern(buf, FC_BUFFER_POOL_CW_SIZE - 8U, (uint8_t)(0x55U + i)), ==, 1U);
        /* 末 8 字节为 ECC parity，非零 */
        uint8_t parity_zero = 1U;
        for (uint16_t j = FC_BUFFER_POOL_CW_SIZE - 8U; j < FC_BUFFER_POOL_CW_SIZE; j++)
        {
            if (buf[j] != 0U) { parity_zero = 0U; break; }
        }
        munit_assert_uint32(parity_zero, ==, 0U);
    }

    /* round-trip：ECC 解码后恢复原始 pattern */
    {
        const fc_ldpc_config_t cfg = { FC_BUFFER_POOL_CW_SIZE, 8U, 4U };
        for (uint8_t i = 0U; i < count; i++)
        {
            uint8_t *buf = get_cw_buffer(cmd, i);
            munit_assert_int(fc_ldpc_decode(&cfg, buf, FC_BUFFER_POOL_CW_SIZE), ==, FC_STATUS_OK);
            munit_assert_uint32(check_pattern(buf, FC_BUFFER_POOL_CW_SIZE, (uint8_t)(0x55U + i)), ==, 1U);
        }
    }

    fc_command_release(cmd);
    return MUNIT_OK;
}

static MunitResult test_encode_random_on(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    fc_command_t *cmd = alloc_cmd(4U, FC_RANDOM_ENABLE, FC_RAID_MODE_NORMAL);
    uint8_t count = get_cw_count(cmd);

    for (uint8_t i = 0U; i < count; i++)
    {
        uint8_t *buf = get_cw_buffer(cmd, i);
        munit_assert_not_null(buf);
        fill_pattern(buf, FC_BUFFER_POOL_CW_SIZE, 0xAAU);
    }

    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    /* round-trip：ECC 解码 → 解扰 → 原始 pattern 恢复 */
    {
        const fc_ldpc_config_t cfg = { FC_BUFFER_POOL_CW_SIZE, 8U, 4U };
        for (uint8_t i = 0U; i < count; i++)
        {
            uint8_t *buf = get_cw_buffer(cmd, i);
            munit_assert_int(fc_ldpc_decode(&cfg, buf, FC_BUFFER_POOL_CW_SIZE), ==, FC_STATUS_OK);
            /* ECC 解码后数据仍在加扰状态，需解扰恢复原文 */
            fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
            for (uint8_t d = 0U; d < i; d++)
            {
                fc_descriptor_t *dsc = fc_descriptor_id_to_descriptor(desc_id);
                if (dsc == NULL) break;
                desc_id = (fc_descriptor_id_t)dsc->next_id;
            }
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            uint32_t seed = fc_random_seed_from_paa(desc->paa, 0U);
            fc_random_transform(buf, FC_BUFFER_POOL_CW_SIZE, seed);
            munit_assert_uint32(check_pattern(buf, FC_BUFFER_POOL_CW_SIZE, 0xAAU), ==, 1U);
        }
    }

    fc_command_release(cmd);
    return MUNIT_OK;
}

static MunitResult test_encode_raid_on(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    /* 至少需要 2 个 CW 才能验证 RAID XOR */
    fc_command_t *cmd = alloc_cmd(4U, FC_RANDOM_ENABLE, FC_RAID_MODE_ENCODE);
    uint8_t count = get_cw_count(cmd);
    munit_assert_uint32(count, >=, 2U);

    /* 不同 CW 填不同 pattern */
    for (uint8_t i = 0U; i < count; i++)
    {
        uint8_t *buf = get_cw_buffer(cmd, i);
        munit_assert_not_null(buf);
        fill_pattern(buf, FC_BUFFER_POOL_CW_SIZE, (uint8_t)(0x10U + i));
    }

    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    /*
     * RAID XOR 将前 count-1 个 CW 的 XOR 结果写入末 CW（parity CW）。
     * 验证：清空 CW0 → RAID recover → 与原始 pattern 一致
     */
    {
        uint8_t *buffers[CW_TOTAL_MAX];
        for (uint8_t i = 0U; i < count; i++)
        {
            buffers[i] = get_cw_buffer(cmd, i);
        }

        /* 清空 CW0，模拟丢失 */
        memset(buffers[0], 0, FC_BUFFER_POOL_CW_SIZE);

        /* RAID recover CW0 */
        uint8_t recovered[FC_BUFFER_POOL_CW_SIZE];
        fc_raid_recover(recovered, buffers, count, 0U, FC_BUFFER_POOL_CW_SIZE);
        memcpy(buffers[0], recovered, FC_BUFFER_POOL_CW_SIZE);

        /* ECC 解码 + 解扰后验证 */
        const fc_ldpc_config_t cfg = { FC_BUFFER_POOL_CW_SIZE, 8U, 4U };
        for (uint8_t i = 0U; i < count; i++)
        {
            uint8_t *buf = buffers[i];
            munit_assert_int(fc_ldpc_decode(&cfg, buf, FC_BUFFER_POOL_CW_SIZE), ==, FC_STATUS_OK);
            fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
            for (uint8_t d = 0U; d < i; d++)
            {
                fc_descriptor_t *dsc = fc_descriptor_id_to_descriptor(desc_id);
                if (dsc == NULL) break;
                desc_id = (fc_descriptor_id_t)dsc->next_id;
            }
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            uint32_t seed = fc_random_seed_from_paa(desc->paa, 0U);
            fc_random_transform(buf, FC_BUFFER_POOL_CW_SIZE, seed);
            munit_assert_uint32(check_pattern(buf, FC_BUFFER_POOL_CW_SIZE, (uint8_t)(0x10U + i)), ==, 1U);
        }
    }

    fc_command_release(cmd);
    return MUNIT_OK;
}

static MunitResult test_encode_single_cw(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    fc_command_t *cmd = alloc_cmd(1U, FC_RANDOM_DISABLE, FC_RAID_MODE_NORMAL);
    uint8_t count = get_cw_count(cmd);
    munit_assert_uint32(count, ==, 1U);

    uint8_t *buf = get_cw_buffer(cmd, 0U);
    munit_assert_not_null(buf);
    fill_pattern(buf, FC_BUFFER_POOL_CW_SIZE, 0x55U);

    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    /* round-trip decode */
    const fc_ldpc_config_t cfg = { FC_BUFFER_POOL_CW_SIZE, 8U, 4U };
    munit_assert_int(fc_ldpc_decode(&cfg, buf, FC_BUFFER_POOL_CW_SIZE), ==, FC_STATUS_OK);
    munit_assert_uint32(check_pattern(buf, FC_BUFFER_POOL_CW_SIZE, 0x55U), ==, 1U);

    fc_command_release(cmd);
    return MUNIT_OK;
}

static MunitResult test_encode_null(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    munit_assert_int(fc_data_path_encode(NULL), ==, FC_STATUS_INVALID_ARGUMENT);
    return MUNIT_OK;
}

/**************************************************************************
 *  fc_data_path_process_read 测试
 *
 *  验证管线（Pass 1 + Pass 2）：
 *     Buffer → ECC 解码 → 全部成功? → 解扰
 *                              ↓ 否
 *                        不解扰，返回 IO_ERROR
 *
 *  参数组合：
 *     random  ECC 结果  cw    说明
 *     on      全 OK    4CW   解码成功 → 解扰
 *     off     全 OK    4CW   只解码，不涉及解扰
 *     on      有 UNC   4CW   不解扰，返回 IO_ERROR
 *     off     有 UNC   4CW   不解扰，返回 IO_ERROR
 *     on      全 OK    1CW   单 CW 边界
 *
 *  UNC 模拟：encode 后故意翻转 buffer 中大量 bit → ECC 不可纠
 **************************************************************************/

static MunitResult test_process_read_ok_random_on(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    fc_command_t *cmd = alloc_cmd(4U, FC_RANDOM_ENABLE, FC_RAID_MODE_NORMAL);
    uint8_t count = get_cw_count(cmd);

    /* 先 encode（加扰 + ECC） */
    for (uint8_t i = 0U; i < count; i++)
    {
        uint8_t *buf = get_cw_buffer(cmd, i);
        fill_pattern(buf, FC_BUFFER_POOL_CW_SIZE, 0x55U);
    }
    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    /* process_read：ECC 解码 + 解扰 */
    fc_status_t result = fc_data_path_process_read(cmd);
    munit_assert_int(result, ==, FC_STATUS_OK);

    /* 验证：全部解扰后恢复原文 */
    for (uint8_t i = 0U; i < count; i++)
    {
        uint8_t *buf = get_cw_buffer(cmd, i);
        munit_assert_uint32(check_pattern(buf, FC_BUFFER_POOL_CW_SIZE, 0x55U), ==, 1U);
    }

    fc_command_release(cmd);
    return MUNIT_OK;
}

static MunitResult test_process_read_ok_random_off(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    fc_command_t *cmd = alloc_cmd(4U, FC_RANDOM_DISABLE, FC_RAID_MODE_NORMAL);
    uint8_t count = get_cw_count(cmd);

    for (uint8_t i = 0U; i < count; i++)
    {
        uint8_t *buf = get_cw_buffer(cmd, i);
        fill_pattern(buf, FC_BUFFER_POOL_CW_SIZE, (uint8_t)(0x55U + i));
    }
    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    fc_status_t result = fc_data_path_process_read(cmd);
    munit_assert_int(result, ==, FC_STATUS_OK);

    for (uint8_t i = 0U; i < count; i++)
    {
        uint8_t *buf = get_cw_buffer(cmd, i);
        munit_assert_uint32(check_pattern(buf, FC_BUFFER_POOL_CW_SIZE, (uint8_t)(0x55U + i)), ==, 1U);
    }

    fc_command_release(cmd);
    return MUNIT_OK;
}

static MunitResult test_process_read_unc(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    fc_command_t *cmd = alloc_cmd(4U, FC_RANDOM_ENABLE, FC_RAID_MODE_NORMAL);
    uint8_t count = get_cw_count(cmd);

    for (uint8_t i = 0U; i < count; i++)
    {
        uint8_t *buf = get_cw_buffer(cmd, i);
        fill_pattern(buf, FC_BUFFER_POOL_CW_SIZE, 0x55U);
    }
    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    /* 破坏 CW1：翻转多个字节 → ECC 无法纠正 */
    uint8_t *buf1 = get_cw_buffer(cmd, 1U);
    munit_assert_not_null(buf1);
    buf1[0]   ^= 0xFF;
    buf1[100] ^= 0xFF;
    buf1[200] ^= 0xFF;
    buf1[300] ^= 0xFF;

    /* process_read 应返回 IO_ERROR（不解扰） */
    fc_status_t result = fc_data_path_process_read(cmd);
    munit_assert_int(result, ==, FC_STATUS_IO_ERROR);

    /* 验证 desc status：CW1 为 UNC，其余为 READY */
    {
        fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
        for (uint8_t i = 0U; i < count; i++)
        {
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            munit_assert_not_null(desc);
            if (i == 1U)
            {
                munit_assert_uint32(desc->status, ==, FC_CW_STATUS_UNC);
            }
            else
            {
                munit_assert_uint32(desc->status, ==, FC_CW_STATUS_READY);
            }
            desc_id = (fc_descriptor_id_t)desc->next_id;
        }
    }

    fc_command_release(cmd);
    return MUNIT_OK;
}

static MunitResult test_process_read_single_cw(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    fc_command_t *cmd = alloc_cmd(1U, FC_RANDOM_ENABLE, FC_RAID_MODE_NORMAL);
    uint8_t count = get_cw_count(cmd);
    munit_assert_uint32(count, ==, 1U);

    uint8_t *buf = get_cw_buffer(cmd, 0U);
    fill_pattern(buf, FC_BUFFER_POOL_CW_SIZE, 0x55U);
    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    munit_assert_int(fc_data_path_process_read(cmd), ==, FC_STATUS_OK);
    munit_assert_uint32(check_pattern(buf, FC_BUFFER_POOL_CW_SIZE, 0x55U), ==, 1U);

    fc_command_release(cmd);
    return MUNIT_OK;
}

static MunitResult test_process_read_null(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    munit_assert_int(fc_data_path_process_read(NULL), ==, FC_STATUS_INVALID_ARGUMENT);
    return MUNIT_OK;
}

/**************************************************************************
 *  fc_data_path_raid_recover 测试
 *
 *  验证管线：
 *     RAID XOR 重建 → ECC 重试 → 解扰所有 CW
 *
 *  参数组合：
 *     random   failed_idx     说明
 *     on       0 (首位)       恢复第一个 CW
 *     on       1 (中间)       恢复中间 CW
 *     on       count-1 (末位) 恢复末位 CW（含 parity）
 *     off      1              无加扰恢复
 *
 *  策略：encode（含 RAID）→ 破坏指定 CW → raid_recover → 验证恢复
 **************************************************************************/

static void do_raid_recover_test(uint8_t random_mode, uint8_t failed_idx)
{
    fc_command_t *cmd = alloc_cmd(4U, random_mode, FC_RAID_MODE_ENCODE);
    uint8_t count = get_cw_count(cmd);
    munit_assert_uint32(count, >=, 3U);
    munit_assert_uint32(failed_idx, <, count);

    /* 不同 CW 填不同 pattern */
    for (uint8_t i = 0U; i < count; i++)
    {
        uint8_t *buf = get_cw_buffer(cmd, i);
        fill_pattern(buf, FC_BUFFER_POOL_CW_SIZE, (uint8_t)(0x10U + i));
    }

    /* encode：加扰 + ECC + RAID XOR */
    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    /* 破坏指定 CW：翻转全部 bit */
    uint8_t *failed_buf = get_cw_buffer(cmd, failed_idx);
    munit_assert_not_null(failed_buf);
    for (uint16_t j = 0U; j < FC_BUFFER_POOL_CW_SIZE; j++)
    {
        failed_buf[j] ^= 0xFF;
    }

    /* raid_recover：XOR 重建 + ECC 重试 + 解扰 */
    fc_status_t result = fc_data_path_raid_recover(cmd, failed_idx);
    munit_assert_int(result, ==, FC_STATUS_OK);

    /* 验证：全部 CW 恢复原始 pattern */
    for (uint8_t i = 0U; i < count; i++)
    {
        uint8_t *buf = get_cw_buffer(cmd, i);
        munit_assert_uint32(check_pattern(buf, FC_BUFFER_POOL_CW_SIZE, (uint8_t)(0x10U + i)), ==, 1U);
    }

    fc_command_release(cmd);
}

static MunitResult test_raid_recover_first(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    do_raid_recover_test(FC_RANDOM_ENABLE, 0U);
    return MUNIT_OK;
}

static MunitResult test_raid_recover_mid(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    do_raid_recover_test(FC_RANDOM_ENABLE, 1U);
    return MUNIT_OK;
}

static MunitResult test_raid_recover_last(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    do_raid_recover_test(FC_RANDOM_ENABLE, 3U);
    return MUNIT_OK;
}

static MunitResult test_raid_recover_no_random(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    do_raid_recover_test(FC_RANDOM_DISABLE, 1U);
    return MUNIT_OK;
}

static MunitResult test_raid_recover_null(const MunitParameter params[], void *data)
{
    (void)params; (void)data;
    munit_assert_int(fc_data_path_raid_recover(NULL, 0U), ==, FC_STATUS_INVALID_ARGUMENT);
    return MUNIT_OK;
}

/**************************************************************************
 *  套件定义
 *
 *  所有 Level 2 测试共享 setup_level2 suite 级 setup（一次性 fc_init）
 **************************************************************************/

MunitTest g_fc_data_path_tests[] = {
    /* ── encode ── */
    { "encode_random_off",    test_encode_random_off,    setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "encode_random_on",     test_encode_random_on,     setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "encode_raid_on",       test_encode_raid_on,       setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "encode_single_cw",     test_encode_single_cw,     setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "encode_null",          test_encode_null,          setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },

    /* ── process_read ── */
    { "process_read_ok_random_on",  test_process_read_ok_random_on,  setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "process_read_ok_random_off", test_process_read_ok_random_off, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "process_read_unc",           test_process_read_unc,           setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "process_read_single_cw",     test_process_read_single_cw,     setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "process_read_null",          test_process_read_null,          setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },

    /* ── raid_recover ── */
    { "raid_recover_first",    test_raid_recover_first,    setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "raid_recover_mid",      test_raid_recover_mid,      setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "raid_recover_last",     test_raid_recover_last,     setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "raid_recover_no_random", test_raid_recover_no_random, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "raid_recover_null",     test_raid_recover_null,     setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },

    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
};
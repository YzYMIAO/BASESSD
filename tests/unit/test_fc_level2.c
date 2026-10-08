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
 *       建链走 FTL 的 ftl_cmd_configure_descriptors()——「按掩码建整条链」的
 *       策略层自 v13 起属 FTL（FC 只剩单数逐条入口 fc_command_configure_descriptor）
 *
 * 参数组合覆盖：
 *   random  : FC_RANDOM_ENABLE / FC_RANDOM_DISABLE
 *   raid    : FC_RAID_MODE_NORMAL / FC_RAID_MODE_ENCODE
 *   cw      : 0 (1 CW) / 3 (4 CW)
 *   ecc_结果: 全部成功 / 有 UNC
 *   failed_idx: 首位 / 中间 / 末位
 *
 * 释放：一律 fc_command_release() 收尾——v16 起它沿 desc 链把 desc + buffer
 *       一起还池；不要再手动 fc_buffer_release（会二次释放、空闲链成环）。
 */

#include "fc/fc.h"
#include "fc/fc_command.h"
#include "fc/fc_data_path.h"
#include "fc/fc_ldpc_engine.h"
#include "fc/fc_random_engine.h"
#include "ftl/ftl_fc_interface.h"
#include "munit.h"
#include "nand/nand_config.h"
#include "nand/onfi_1_0.h"

#if CONFIG_FC_RAID
#    include "fc/fc_raid_engine.h"
#endif

#include <string.h>

/**************************************************************************
 *  Suite 级 setup：fc_init() 一次性初始化 Pool
 **************************************************************************/

static int g_level2_inited = 0;

/* 资源统计打印已注释（套件 setup 每个用例都打一次，刷屏）。
 * 函数体保留备查——需要时取消注释，并恢复 setup_level2 里的调用。
static void print_resource_stats(const char *tag)
{
    fc_resource_stats_t stats;
    if (fc_get_resource_stats(&stats) == FC_STATUS_OK)
    {
        fprintf(stderr, "[RES] %s: cmd=%u/%u desc=%u/%u buf=%u/%u sq=%u hsq=%u cq=%u\n", tag, (unsigned)stats.command_used, (unsigned)FC_COMMAND_POOL_DEPTH, (unsigned)stats.descriptor_used, (unsigned)FC_DESCRIPTOR_POOL_DEPTH, (unsigned)stats.buffer_used, (unsigned)FC_BUFFER_POOL_DEPTH, (unsigned)stats.sq_used, (unsigned)stats.hsq_used, (unsigned)stats.cq_used);
    }
}
*/

static void *setup_level2(const MunitParameter params[], void *user_data)
{
    (void)params;
    (void)user_data;
    if (!g_level2_inited)
    {
        fc_init();
        g_level2_inited = 1;
    }
    /* print_resource_stats("L2"); */ /* 已注释：每用例打一次刷屏 */
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
        {
            return 0U;
        }
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

    ftl_status_t result = ftl_cmd_configure_descriptors(cmd);
    munit_assert_int(result, ==, FTL_OK);

    return cmd;
}

/* 获取 desc 链中第 idx 个 CW 的 buffer 指针（idx=0 为第一个 CW）
 * 返回 USER 数据区首地址（FTL 视角） */
static uint8_t *get_cw_buffer(const fc_command_t *cmd, uint8_t idx)
{
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
    uint8_t            i       = 0U;

    while (desc_id != FC_DESCRIPTOR_ID_INVALID)
    {
        if (i == idx)
        {
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            if (desc == NULL)
            {
                return NULL;
            }
            return fc_buffer_id_to_real_buffer(desc->buffer_id);
        }
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        if (desc == NULL)
        {
            return NULL;
        }
        desc_id = (fc_descriptor_id_t)desc->next_id;
        i++;
    }
    return NULL;
}

/* 获取 desc 链中第 idx 个 CW 的基址（META 开头，FC 内部使用） */
static uint8_t *get_cw_base(const fc_command_t *cmd, uint8_t idx)
{
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
    uint8_t            i       = 0U;

    while (desc_id != FC_DESCRIPTOR_ID_INVALID)
    {
        if (i == idx)
        {
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            if (desc == NULL)
            {
                return NULL;
            }
            return fc_buffer_get_base(desc->buffer_id);
        }
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        if (desc == NULL)
        {
            return NULL;
        }
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
        if (desc == NULL)
        {
            break;
        }
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
    (void)params;
    (void)data;
    fc_command_t *cmd   = alloc_cmd(4U, FC_RANDOM_DISABLE, FC_RAID_MODE_NORMAL);
    uint8_t       count = get_cw_count(cmd);
    munit_assert_uint32(count, ==, 4U);

    for (uint8_t i = 0U; i < count; i++)
    {
        uint8_t *buf = get_cw_buffer(cmd, i);
        munit_assert_not_null(buf);
        fill_pattern(buf, FC_BUFFER_USER_SIZE, (uint8_t)(0x55U + i));
    }

    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    /* 验证：ECC 编码后末 8 字节为 parity（非零），前 520 字节保持原始 pattern */
    for (uint8_t i = 0U; i < count; i++)
    {
        uint8_t *buf  = get_cw_buffer(cmd, i);
        uint8_t *base = get_cw_base(cmd, i);
        munit_assert_not_null(buf);
        munit_assert_not_null(base);
        /* USER 数据区（512 字节）应保持原始数据 */
        munit_assert_uint32(check_pattern(buf, FC_BUFFER_USER_SIZE, (uint8_t)(0x55U + i)), ==, 1U);
        /* 末 8 字节为 ECC parity，非零 */
        uint8_t parity_zero = 1U;
        for (uint16_t j = FC_BUFFER_DATA_SIZE; j < FC_BUFFER_POOL_CW_SIZE; j++)
        {
            if (base[j] != 0U)
            {
                parity_zero = 0U;
                break;
            }
        }
        munit_assert_uint32(parity_zero, ==, 0U);
    }

    /* round-trip：ECC 解码后恢复原始 pattern */
    {
        const fc_ldpc_config_t cfg = {FC_BUFFER_DATA_SIZE, 8U, 4U};
        for (uint8_t i = 0U; i < count; i++)
        {
            uint8_t *buf  = get_cw_buffer(cmd, i);
            uint8_t *base = get_cw_base(cmd, i);
            munit_assert_int(fc_ldpc_decode(&cfg, base, FC_BUFFER_DATA_SIZE, NULL), ==, FC_STATUS_OK);
            munit_assert_uint32(check_pattern(buf, FC_BUFFER_USER_SIZE, (uint8_t)(0x55U + i)), ==, 1U);
        }
    }

    fc_command_release(cmd);
    return MUNIT_OK;
}

static MunitResult test_encode_random_on(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    fc_command_t *cmd   = alloc_cmd(4U, FC_RANDOM_ENABLE, FC_RAID_MODE_NORMAL);
    uint8_t       count = get_cw_count(cmd);

    for (uint8_t i = 0U; i < count; i++)
    {
        uint8_t *buf = get_cw_buffer(cmd, i);
        munit_assert_not_null(buf);
        fill_pattern(buf, FC_BUFFER_USER_SIZE, 0xAAU);
    }

    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    /* round-trip：ECC 解码 → 解扰 → 原始 pattern 恢复 */
    {
        const fc_ldpc_config_t cfg = {FC_BUFFER_DATA_SIZE, 8U, 4U};
        for (uint8_t i = 0U; i < count; i++)
        {
            uint8_t *buf  = get_cw_buffer(cmd, i);
            uint8_t *base = get_cw_base(cmd, i);
            munit_assert_int(fc_ldpc_decode(&cfg, base, FC_BUFFER_DATA_SIZE, NULL), ==, FC_STATUS_OK);
            /* ECC 解码后数据仍在加扰状态，需解扰恢复原文 */
            fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
            for (uint8_t d = 0U; d < i; d++)
            {
                fc_descriptor_t *dsc = fc_descriptor_id_to_descriptor(desc_id);
                if (dsc == NULL)
                {
                    break;
                }
                desc_id = (fc_descriptor_id_t)dsc->next_id;
            }
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            uint32_t         seed = fc_random_seed_from_paa(desc->paa, 0U);
            fc_random_transform(base, FC_BUFFER_DATA_SIZE, seed);
            munit_assert_uint32(check_pattern(buf, FC_BUFFER_USER_SIZE, 0xAAU), ==, 1U);
        }
    }

    fc_command_release(cmd);
    return MUNIT_OK;
}

#if CONFIG_FC_RAID

static MunitResult test_encode_raid_on(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    /* 至少需要 2 个 CW 才能验证 RAID XOR */
    fc_command_t *cmd   = alloc_cmd(4U, FC_RANDOM_ENABLE, FC_RAID_MODE_ENCODE);
    uint8_t       count = get_cw_count(cmd);
    munit_assert_uint32(count, >=, 2U);

    /* 不同 CW 填不同 pattern */
    for (uint8_t i = 0U; i < count; i++)
    {
        uint8_t *buf = get_cw_buffer(cmd, i);
        munit_assert_not_null(buf);
        fill_pattern(buf, FC_BUFFER_USER_SIZE, (uint8_t)(0x10U + i));
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
            buffers[i] = get_cw_base(cmd, i);
        }

        /* 清空 CW0，模拟丢失 */
        memset(buffers[0], 0, FC_BUFFER_POOL_CW_SIZE);

        /* RAID recover CW0 */
        uint8_t recovered[FC_BUFFER_POOL_CW_SIZE];
        fc_raid_recover(recovered, buffers, count, 0U, FC_BUFFER_POOL_CW_SIZE);
        memcpy(buffers[0], recovered, FC_BUFFER_POOL_CW_SIZE);

        /* ECC 解码 + 解扰后验证（不包含 parity CW，它存储的是 XOR 结果） */
        const fc_ldpc_config_t cfg = {FC_BUFFER_DATA_SIZE, 8U, 4U};
        for (uint8_t i = 0U; i < count - 1U; i++)
        {
            uint8_t *base = buffers[i];
            uint8_t *buf  = get_cw_buffer(cmd, i);
            munit_assert_int(fc_ldpc_decode(&cfg, base, FC_BUFFER_DATA_SIZE, NULL), ==, FC_STATUS_OK);
            fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
            for (uint8_t d = 0U; d < i; d++)
            {
                fc_descriptor_t *dsc = fc_descriptor_id_to_descriptor(desc_id);
                if (dsc == NULL)
                {
                    break;
                }
                desc_id = (fc_descriptor_id_t)dsc->next_id;
            }
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            uint32_t         seed = fc_random_seed_from_paa(desc->paa, 0U);
            fc_random_transform(base, FC_BUFFER_DATA_SIZE, seed);
            munit_assert_uint32(check_pattern(buf, FC_BUFFER_USER_SIZE, (uint8_t)(0x10U + i)), ==, 1U);
        }
    }

    fc_command_release(cmd);
    return MUNIT_OK;
}

#endif /* CONFIG_FC_RAID */

static MunitResult test_encode_single_cw(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    fc_command_t *cmd   = alloc_cmd(1U, FC_RANDOM_DISABLE, FC_RAID_MODE_NORMAL);
    uint8_t       count = get_cw_count(cmd);
    munit_assert_uint32(count, ==, 1U);

    uint8_t *buf  = get_cw_buffer(cmd, 0U);
    uint8_t *base = get_cw_base(cmd, 0U);
    munit_assert_not_null(buf);
    fill_pattern(buf, FC_BUFFER_USER_SIZE, 0x55U);

    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    /* round-trip decode */
    const fc_ldpc_config_t cfg = {FC_BUFFER_DATA_SIZE, 8U, 4U};
    munit_assert_int(fc_ldpc_decode(&cfg, base, FC_BUFFER_DATA_SIZE, NULL), ==, FC_STATUS_OK);
    munit_assert_uint32(check_pattern(buf, FC_BUFFER_USER_SIZE, 0x55U), ==, 1U);

    fc_command_release(cmd);
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
    (void)params;
    (void)data;
    fc_command_t *cmd   = alloc_cmd(4U, FC_RANDOM_ENABLE, FC_RAID_MODE_NORMAL);
    uint8_t       count = get_cw_count(cmd);

    /* 先 encode（加扰 + ECC） */
    for (uint8_t i = 0U; i < count; i++)
    {
        uint8_t *buf = get_cw_buffer(cmd, i);
        fill_pattern(buf, FC_BUFFER_USER_SIZE, 0x55U);
    }
    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    /* process_read：ECC 解码 + 解扰 + CRC 校验 */
    fc_status_t result = fc_data_path_process_read(cmd);
    munit_assert_int(result, ==, FC_STATUS_OK);

    /* 验证：全部解扰后恢复原文 */
    for (uint8_t i = 0U; i < count; i++)
    {
        uint8_t *buf = get_cw_buffer(cmd, i);
        munit_assert_uint32(check_pattern(buf, FC_BUFFER_USER_SIZE, 0x55U), ==, 1U);
    }

    fc_command_release(cmd);
    return MUNIT_OK;
}

static MunitResult test_process_read_ok_random_off(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    fc_command_t *cmd   = alloc_cmd(4U, FC_RANDOM_DISABLE, FC_RAID_MODE_NORMAL);
    uint8_t       count = get_cw_count(cmd);

    for (uint8_t i = 0U; i < count; i++)
    {
        uint8_t *buf = get_cw_buffer(cmd, i);
        fill_pattern(buf, FC_BUFFER_USER_SIZE, (uint8_t)(0x55U + i));
    }
    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    fc_status_t result = fc_data_path_process_read(cmd);
    munit_assert_int(result, ==, FC_STATUS_OK);

    for (uint8_t i = 0U; i < count; i++)
    {
        uint8_t *buf = get_cw_buffer(cmd, i);
        munit_assert_uint32(check_pattern(buf, FC_BUFFER_USER_SIZE, (uint8_t)(0x55U + i)), ==, 1U);
    }

    fc_command_release(cmd);
    return MUNIT_OK;
}

static MunitResult test_process_read_unc(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    fc_command_t *cmd   = alloc_cmd(4U, FC_RANDOM_ENABLE, FC_RAID_MODE_NORMAL);
    uint8_t       count = get_cw_count(cmd);

    for (uint8_t i = 0U; i < count; i++)
    {
        uint8_t *buf = get_cw_buffer(cmd, i);
        fill_pattern(buf, FC_BUFFER_USER_SIZE, 0x55U);
    }
    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    /* 破坏 CW1：翻转多个字节 → ECC 无法纠正 */
    uint8_t *buf1_base = get_cw_base(cmd, 1U);
    munit_assert_not_null(buf1_base);
    buf1_base[0] ^= 0xFF;
    buf1_base[100] ^= 0xFF;
    buf1_base[200] ^= 0xFF;
    buf1_base[300] ^= 0xFF;

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
    (void)params;
    (void)data;
    fc_command_t *cmd   = alloc_cmd(1U, FC_RANDOM_ENABLE, FC_RAID_MODE_NORMAL);
    uint8_t       count = get_cw_count(cmd);
    munit_assert_uint32(count, ==, 1U);

    uint8_t *buf = get_cw_buffer(cmd, 0U);
    fill_pattern(buf, FC_BUFFER_USER_SIZE, 0x55U);
    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    munit_assert_int(fc_data_path_process_read(cmd), ==, FC_STATUS_OK);
    munit_assert_uint32(check_pattern(buf, FC_BUFFER_USER_SIZE, 0x55U), ==, 1U);

    fc_command_release(cmd);
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
 *
 *  编译开关：CONFIG_FC_RAID
 **************************************************************************/

#if CONFIG_FC_RAID

static void do_raid_recover_test(uint8_t random_mode, uint8_t failed_idx)
{
    fc_command_t *cmd   = alloc_cmd(4U, random_mode, FC_RAID_MODE_ENCODE);
    uint8_t       count = get_cw_count(cmd);
    munit_assert_uint32(count, >=, 3U);
    munit_assert_uint32(failed_idx, <, count);

    /* 不同 CW 填不同 pattern（USER 数据区 512B） */
    for (uint8_t i = 0U; i < count; i++)
    {
        uint8_t *buf = get_cw_buffer(cmd, i);
        fill_pattern(buf, FC_BUFFER_USER_SIZE, (uint8_t)(0x10U + i));
    }

    /* encode：加扰 + ECC + RAID XOR */
    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    /* 破坏指定 CW（全 CW 528B 翻转，使 ECC 不可纠） */
    uint8_t *failed_base = get_cw_base(cmd, failed_idx);
    munit_assert_not_null(failed_base);
    for (uint16_t j = 0U; j < FC_BUFFER_POOL_CW_SIZE; j++)
    {
        failed_base[j] ^= 0xFF;
    }

    /* raid_recover：XOR 重建 + ECC 重试 + 解扰 */
    fc_status_t result = fc_data_path_raid_recover(cmd, failed_idx);
    munit_assert_int(result, ==, FC_STATUS_OK);

    /* 验证：全部 CW USER 数据区恢复原始 pattern */
    for (uint8_t i = 0U; i < count; i++)
    {
        uint8_t *buf = get_cw_buffer(cmd, i);
        munit_assert_uint32(check_pattern(buf, FC_BUFFER_USER_SIZE, (uint8_t)(0x10U + i)), ==, 1U);
    }

    fc_command_release(cmd);
}

static MunitResult test_raid_recover_first(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    do_raid_recover_test(FC_RANDOM_ENABLE, 0U);
    return MUNIT_OK;
}

static MunitResult test_raid_recover_mid(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    do_raid_recover_test(FC_RANDOM_ENABLE, 1U);
    return MUNIT_OK;
}

static MunitResult test_raid_recover_last(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    do_raid_recover_test(FC_RANDOM_ENABLE, 3U);
    return MUNIT_OK;
}

static MunitResult test_raid_recover_no_random(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    do_raid_recover_test(FC_RANDOM_DISABLE, 1U);
    return MUNIT_OK;
}

/**************************************************************************
 *  stripe parity 测试 — fc_data_path_generate_stripe_parity()
 *
 *  验证跨命令 stripe XOR parity 的正确性，不依赖 NAND 硬件。
 *  使用两条命令（数据 + parity），各 4 CW，数据命令先走 encode() 加扰/ECC，
 *  然后 generate_stripe_parity() XOR → parity ECC 编码。
 *
 *  验证方式：清空数据命令某 CW → 用 fc_raid_recover 从 parity 重建 →
 *  与原始 pattern 一致。
 **************************************************************************/

static MunitResult test_generate_stripe_parity(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    fc_command_t *data_cmd;
    fc_command_t *parity_cmd;
    uint8_t       cw_count;

    /* 分配数据命令（4 CW, random=ON, raid=NORMAL — 不再在 encode 里做 XOR） */
    data_cmd = alloc_cmd(4U, FC_RANDOM_ENABLE, FC_RAID_MODE_NORMAL);
    cw_count = get_cw_count(data_cmd);
    munit_assert_uint32(cw_count, ==, 4U);

    /* 不同 CW 填不同 pattern */
    for (uint8_t i = 0U; i < cw_count; i++)
    {
        uint8_t *buf = get_cw_buffer(data_cmd, i);
        munit_assert_not_null(buf);
        fill_pattern(buf, FC_BUFFER_USER_SIZE, (uint8_t)(0x20U + i));
    }

    /* encode 数据命令：META → CRC → Random → ECC */
    munit_assert_int(fc_data_path_encode(data_cmd), ==, FC_STATUS_OK);

    /* 分配 parity 命令（同 CW 数，raid=NORMAL） */
    parity_cmd = alloc_cmd(cw_count, FC_RANDOM_DISABLE, FC_RAID_MODE_NORMAL);
    munit_assert_uint32(get_cw_count(parity_cmd), ==, cw_count);

    /* 同步 parity desc 的 FAA（模拟 fc_stripe_submit 中的行为） */
    {
        fc_descriptor_id_t d_desc_id = (fc_descriptor_id_t)data_cmd->desc_id;
        fc_descriptor_id_t p_desc_id = (fc_descriptor_id_t)parity_cmd->desc_id;
        for (uint8_t i = 0U; i < cw_count; i++)
        {
            fc_descriptor_t *ddesc = fc_descriptor_id_to_descriptor(d_desc_id);
            fc_descriptor_t *pdesc = fc_descriptor_id_to_descriptor(p_desc_id);
            munit_assert_not_null(ddesc);
            munit_assert_not_null(pdesc);
            pdesc->meta = ddesc->meta;
            d_desc_id   = (fc_descriptor_id_t)ddesc->next_id;
            p_desc_id   = (fc_descriptor_id_t)pdesc->next_id;
        }
    }

    /* 调用 generate_stripe_parity — XOR 数据命令 CW 到 parity buffer + ECC 编码 parity */
    {
        const fc_command_t *data_ptrs[1U]; /* PLANE_NUM=2 → 1 条数据命令 */
        data_ptrs[0U] = data_cmd;
        munit_assert_int(fc_data_path_generate_stripe_parity(data_ptrs, 1U, parity_cmd), ==, FC_STATUS_OK);
    }

    /*
     * 验证：清空数据命令的 CW0 → 用 fc_raid_recover() 从 parity + 其他 CW 重建 →
     * ECC 解码 + 解扰 → 与原始 pattern 一致。
     *
     * buffers[] 数组：[CW0, CW1, CW2, CW3, parity_CW0, parity_CW1, parity_CW2, parity_CW3]
     * XOR 重建时传入所有 CW 和 parity CW，fc_raid_recover 会跳过 failed_idx 并 XOR 其余。
     */
    {
        uint8_t *buffers[CW_TOTAL_MAX];
        uint8_t  total_count;

        /* 收集数据命令的 CW 基址 */
        for (uint8_t i = 0U; i < cw_count; i++)
        {
            buffers[i] = get_cw_base(data_cmd, i);
            munit_assert_not_null(buffers[i]);
        }
        /* 收集 parity 命令的 CW 基址 */
        for (uint8_t i = 0U; i < cw_count; i++)
        {
            buffers[cw_count + i] = get_cw_base(parity_cmd, i);
            munit_assert_not_null(buffers[cw_count + i]);
        }
        total_count = (uint8_t)(cw_count + cw_count); /* 8 */

        /* 清空数据命令 CW0，模拟丢失 */
        memset(buffers[0U], 0, FC_BUFFER_POOL_CW_SIZE);

        /* RAID XOR 重建 CW0：传入所有 CW（含 parity） */
        {
            uint8_t recovered[FC_BUFFER_POOL_CW_SIZE];
            fc_raid_recover(recovered, buffers, total_count, 0U, FC_BUFFER_POOL_CW_SIZE);
            memcpy(buffers[0U], recovered, FC_BUFFER_POOL_CW_SIZE);
        }

        /* ECC 解码 + 解扰后验证数据 CW0~CW3（parity CW 不验证，它们存储 XOR 结果） */
        const fc_ldpc_config_t cfg = {FC_BUFFER_DATA_SIZE, 8U, 4U};
        for (uint8_t i = 0U; i < cw_count; i++)
        {
            uint8_t *base = buffers[i];
            uint8_t *buf  = get_cw_buffer(data_cmd, i);
            munit_assert_not_null(base);
            munit_assert_not_null(buf);

            munit_assert_int(fc_ldpc_decode(&cfg, base, FC_BUFFER_DATA_SIZE, NULL), ==, FC_STATUS_OK);

            /* 解扰 */
            fc_descriptor_id_t desc_id = (fc_descriptor_id_t)data_cmd->desc_id;
            for (uint8_t d = 0U; d < i; d++)
            {
                fc_descriptor_t *dsc = fc_descriptor_id_to_descriptor(desc_id);
                if (dsc == NULL)
                {
                    break;
                }
                desc_id = (fc_descriptor_id_t)dsc->next_id;
            }
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            munit_assert_not_null(desc);
            uint32_t seed = fc_random_seed_from_paa(desc->paa, 0U);
            fc_random_transform(base, FC_BUFFER_DATA_SIZE, seed);

            munit_assert_uint32(check_pattern(buf, FC_BUFFER_USER_SIZE, (uint8_t)(0x20U + i)), ==, 1U);
        }
    }

    fc_command_release(data_cmd);
    fc_command_release(parity_cmd);
    return MUNIT_OK;
}

#endif /* CONFIG_FC_RAID */

/**************************************************************************
 *  flat CW 掩码建链（v7）
 *
 *  建链走 FTL 策略层（ftl_cmd_configure_descriptors），不碰 NAND：
 *    - 掩码按 **flat 绝对编号**（bit n = plane*CW_NUM + cw）逐位建 desc
 *    - 链序 plane-major
 *    - mask == 0 时对**每个** plane 展开连续 0..config.cw
 *
 *  「按掩码建整条链」是策略，v13 起属 FTL（FC 只剩单数逐条入口
 *  fc_command_configure_descriptor）。这三条用例仍留在 Level 2，
 *  因为验证的是 desc 链的几何（plane/cw 展开），而非 NAND 行为。
 **************************************************************************/

/* 走 desc 链，返回条数；plane_out / cw_out 非空时逐条回填 (plane, cw) */
static uint8_t collect_chain(const fc_command_t *cmd, uint8_t *plane_out, uint8_t *cw_out)
{
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
    uint8_t            count   = 0U;

    while ((desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (count < CW_TOTAL_MAX))
    {
        fc_descriptor_t *d = fc_descriptor_id_to_descriptor(desc_id);
        munit_assert_not_null(d);
        if (plane_out != NULL)
        {
            plane_out[count] = (uint8_t)paa_to_plane(d->paa);
        }
        if (cw_out != NULL)
        {
            cw_out[count] = (uint8_t)paa_to_cw(d->paa);
        }
        desc_id = (fc_descriptor_id_t)d->next_id;
        count++;
    }
    return count;
}

/* 掩码 0x29：plane0 取 {CW0,CW3} + plane1 取 {CW1} → 3 条 desc（plane-major） */
static MunitResult test_cw_mask_flat_mixed(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_t *cmd = fc_command_alloc();
    munit_assert_not_null(cmd);

    cmd->config.operation   = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
    cmd->config.multi_plane = 2U;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->paa                = paa_make(0U, 0U, 0U, 0U, 0U);
    cmd->param.cw_mask      = 0x29U; /* 0b0010_1001 */

    munit_assert_int(ftl_cmd_configure_descriptors(cmd), ==, FTL_OK);

    uint8_t plane_out[CW_TOTAL_MAX];
    uint8_t cw_out[CW_TOTAL_MAX];
    uint8_t n = collect_chain(cmd, plane_out, cw_out);

    munit_assert_uint32(n, ==, 3U);
    munit_assert_uint32(plane_out[0], ==, 0U);
    munit_assert_uint32(cw_out[0], ==, 0U);
    munit_assert_uint32(plane_out[1], ==, 0U);
    munit_assert_uint32(cw_out[1], ==, 3U);
    munit_assert_uint32(plane_out[2], ==, 1U);
    munit_assert_uint32(cw_out[2], ==, 1U);

    /* config.cw 不回写（纯输入提示，configure 后无下游读者） */
    munit_assert_uint32(cmd->config.cw, ==, 0U);

    fc_command_release(cmd);
    return MUNIT_OK;
}

/* 掩码 0xF0：只选 plane 1 的 4 个 CW（绝对编号，不是 0x0F）→ 4 条 desc 全在 plane 1 */
static MunitResult test_cw_mask_plane1_only(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_t *cmd = fc_command_alloc();
    munit_assert_not_null(cmd);

    cmd->config.operation   = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
    cmd->config.multi_plane = 2U;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->paa                = paa_make(0U, 0U, 0U, 0U, 0U);
    cmd->param.cw_mask      = 0xF0U;

    munit_assert_int(ftl_cmd_configure_descriptors(cmd), ==, FTL_OK);

    uint8_t plane_out[CW_TOTAL_MAX];
    uint8_t cw_out[CW_TOTAL_MAX];
    uint8_t n = collect_chain(cmd, plane_out, cw_out);

    munit_assert_uint32(n, ==, 4U);
    for (uint8_t i = 0U; i < n; i++)
    {
        munit_assert_uint32(plane_out[i], ==, 1U);
        munit_assert_uint32(cw_out[i], ==, i);
    }

    fc_command_release(cmd);
    return MUNIT_OK;
}

/* mask == 0 回归：multi_plane=2 + config.cw=3 必须建出 8 条 desc（不是 4 条）
 * —— 钉住"mask==0 展开必须逐 plane 平移"这条不变量。 */
static MunitResult test_cw_mask_zero_multi_plane(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_t *cmd = fc_command_alloc();
    munit_assert_not_null(cmd);

    cmd->config.operation   = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
    cmd->config.multi_plane = 2U;
    cmd->config.cw          = 3U; /* 每 plane 4 CW */
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->paa                = paa_make(0U, 0U, 0U, 0U, 0U);
    cmd->param.cw_mask      = 0U; /* 未指定 */

    munit_assert_int(ftl_cmd_configure_descriptors(cmd), ==, FTL_OK);

    uint8_t plane_out[CW_TOTAL_MAX];
    uint8_t cw_out[CW_TOTAL_MAX];
    uint8_t n = collect_chain(cmd, plane_out, cw_out);

    munit_assert_uint32(n, ==, 8U);
    for (uint8_t i = 0U; i < n; i++)
    {
        munit_assert_uint32(plane_out[i], ==, (i < CW_NUM) ? 0U : 1U);
        munit_assert_uint32(cw_out[i], ==, (uint8_t)(i % CW_NUM));
    }

    fc_command_release(cmd);
    return MUNIT_OK;
}

/**************************************************************************
 *  套件定义
 *
 *  所有 Level 2 测试共享 setup_level2 suite 级 setup（一次性 fc_init）
 **************************************************************************/

MunitTest g_fc_data_path_tests[] = {
  /* ── encode ── */
    {         "encode_random_off",          test_encode_random_off, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          "encode_random_on",           test_encode_random_on, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
#if CONFIG_FC_RAID
    {            "encode_raid_on",             test_encode_raid_on, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
#endif
    {          "encode_single_cw",           test_encode_single_cw, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},

 /* ── process_read ── */
    { "process_read_ok_random_on",  test_process_read_ok_random_on, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {"process_read_ok_random_off", test_process_read_ok_random_off, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          "process_read_unc",           test_process_read_unc, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {    "process_read_single_cw",     test_process_read_single_cw, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},

 /* ── raid_recover ── */
#if CONFIG_FC_RAID
    {        "raid_recover_first",         test_raid_recover_first, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          "raid_recover_mid",           test_raid_recover_mid, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {         "raid_recover_last",          test_raid_recover_last, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {    "raid_recover_no_random",     test_raid_recover_no_random, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},

 /* ── stripe parity ── */
    {    "generate_stripe_parity",     test_generate_stripe_parity, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
#endif  /* CONFIG_FC_RAID */

  /* ── flat CW 掩码建链（v7）── */
    {        "cw_mask_flat_mixed",         test_cw_mask_flat_mixed, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {       "cw_mask_plane1_only",        test_cw_mask_plane1_only, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {  "cw_mask_zero_multi_plane",   test_cw_mask_zero_multi_plane, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},

    {                        NULL,                            NULL,         NULL, NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};
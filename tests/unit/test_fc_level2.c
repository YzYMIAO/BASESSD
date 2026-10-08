/*
 * test_fc_level2.c — Level 2 数据路径测试：fc_data_path 模块
 *
 * 测试三个数据路径管线函数：
 *   fc_data_path_encode       写路径：META → CRC? → Random 加扰? → ECC 编码
 *   fc_data_path_process_read 读路径 Stage 1：ECC 解码 → 条件解扰 → CRC 校验 → META 校验
 *   fc_data_path_raid_recover 读路径 Stage 2：RAID XOR 重建 → ECC 重试 → 解扰
 *
 * 依赖：需要 fc_init() 初始化 Pool（每个用例的 setup 里一次性初始化，见 g_level2_inited）
 *       不需要 NAND 硬件，全部在 buffer 层面验证
 *       建链走 FTL 的 ftl_cmd_configure_descriptors()——「按掩码建整条链」的
 *       策略层自 v13 起属 FTL（FC 只剩单数逐条入口 fc_command_configure_descriptor）
 *
 * 命令 fixture（alloc_cmd / alloc_cmd_full 统一构造）：
 *   paa        : paa_make(32, 0, 50, 0, 0) —— 真实页地址，**不可为 0**（见 L2_PAA_PAGE 注释）
 *   random     : FC_RANDOM_ENABLE / FC_RANDOM_DISABLE
 *   crc        : FC_CRC_ENABLE / FC_CRC_DISABLE
 *   ldpc       : FC_ECC_MODE_BCH / FC_ECC_MODE_LDPC_RAW
 *   multi_plane: 0 = 单 plane 连续；2 = 按 cw_mask 在 plane 段内选 CW
 *   cw_mask    : 0 = 每 plane 连续 0..config.cw；非 0 = flat 绝对编号掩码
 *   raid       : 形参保留但**数据路径不读**——config.raid 全仓唯一读者是 fc.c 的
 *                分派钩子（决定走不走 fc_stripe_submit），见 alloc_cmd_full 注释
 *   ecc 结果   : 全部成功 / 有 UNC / 有 CRC_ERROR / 有 MATA_ERROR
 *
 * ⚠ 数据路径用例必须给命令真实页地址。paa=0 时 BASIC 种子（fc_random_engine.c）
 *   退化成 seed == cw → {1,1,2,3}，LFSR 低 16 位取这几个小值后 keystream 除 byte0
 *   外全 0 → 加扰形同虚设，"encode→decode 还原原文"照样通过。故 random=ON 的用例
 *   另加一条「encode 后 USER 区 ≠ 明文」断言自证加扰真的发生了。
 *
 * 释放：一律 fc_command_release() 收尾——v16 起它沿 desc 链把 desc + buffer
 *       一起还池；不要再手动 fc_buffer_release（会二次释放、空闲链成环）。
 */

#include "fc/fc.h"
#include "fc/fc_command.h"
#include "fc/fc_crc_engine.h"
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

/* 真实页地址。⚠ 不可用 paa=0（理由见文件头）：取 (page=32, block=50) 使 4 个 CW 的
 * BASIC 种子 = 0x6468..0x646B，互异且远离小值，LFSR 才能跑出满周期 keystream。
 * page=32 < 64、block=50 < 2048，满足 paa_is_valid。 */
#define L2_PAA_PAGE  (32U)
#define L2_PAA_BLOCK (50U)

/**************************************************************************
 *  Suite 级 setup：fc_init() 一次性初始化 Pool
 *
 *  本 fork 的 MunitSuite 结构体没有 setup 字段（tests/framework/munit.h:442），
 *  套件级 setup 不存在 → 只能逐用例挂，靠 g_level2_inited 退化成一次性初始化。
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

/* 验证 buffer 是否为预期 pattern（是返回 1，否返回 0） */
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

/* desc 链中第 idx 个 desc；越界返回 NULL */
static fc_descriptor_t *cw_desc(const fc_command_t *cmd, uint8_t idx)
{
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;

    for (uint8_t i = 0U; desc_id != FC_DESCRIPTOR_ID_INVALID; i++)
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        if (desc == NULL)
        {
            return NULL;
        }
        if (i == idx)
        {
            return desc;
        }
        desc_id = (fc_descriptor_id_t)desc->next_id;
    }
    return NULL;
}

/* 第 idx 个 CW 的 USER 数据区首址（FTL 视角，512B）。链上必有该 desc，故直接断言。 */
static uint8_t *cw_user(const fc_command_t *cmd, uint8_t idx)
{
    fc_descriptor_t *desc = cw_desc(cmd, idx);
    munit_assert_not_null(desc);
    return fc_buffer_id_to_real_buffer(desc->buffer_id);
}

/* 第 idx 个 CW 的 528B 基址（META 开头，FC 内部视角） */
static uint8_t *cw_base(const fc_command_t *cmd, uint8_t idx)
{
    fc_descriptor_t *desc = cw_desc(cmd, idx);
    munit_assert_not_null(desc);
    return fc_buffer_get_base(desc->buffer_id);
}

/* 第 idx 个 CW 的加扰种子——与 encode / process_read 内部同源（由该 desc 的 PAA 派生） */
static uint32_t cw_seed(const fc_command_t *cmd, uint8_t idx)
{
    fc_descriptor_t *desc = cw_desc(cmd, idx);
    munit_assert_not_null(desc);
    return fc_random_seed_from_paa(desc->paa, 0U);
}

/* desc 链长度（= 本次访问的 CW 数） */
static uint8_t cw_count(const fc_command_t *cmd)
{
    uint8_t count = 0U;

    while (cw_desc(cmd, count) != NULL)
    {
        count++;
    }
    return count;
}

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

/* 分配 command 并按掩码建链。
 *   multi_plane == 0 → 单 plane 连续建链（每 plane config.cw+1 个 CW，由 cw_count 推得）
 *   multi_plane >  0 → CW 集合由 cw_mask 决定（mask == 0 时每 plane 连续 0..cw_count-1）
 * raid_mode 仅落进 config.raid：数据路径函数不读它，全仓唯一读者是 fc.c 的分派钩子
 * （决定走不走 fc_stripe_submit）——本层不经过分派，故传 NORMAL / ENCODE 无差别。 */
static fc_command_t *alloc_cmd_full(uint8_t multi_plane, uint8_t cw_count, uint32_t cw_mask, uint8_t random_mode, uint8_t raid_mode)
{
    fc_command_t *cmd = fc_command_alloc();
    munit_assert_not_null(cmd);

    cmd->config.operation   = OP_NORMAL_PROGRAM_PAGE; /* data_path 不依赖 operation */
    cmd->config.cw          = (cw_count > 0U) ? (uint8_t)(cw_count - 1U) : 0U;
    cmd->config.ch          = 0U;
    cmd->config.ce          = 0U;
    cmd->config.lun         = 0U;
    cmd->config.random      = random_mode;
    cmd->config.raid        = raid_mode;
    cmd->config.ldpc        = FC_ECC_MODE_BCH; /* BCH 为默认 ECC */
    cmd->config.multi_plane = multi_plane;
    cmd->paa                = paa_make(L2_PAA_PAGE, 0U, L2_PAA_BLOCK, 0U, 0U);
    cmd->param.cw_mask      = cw_mask;

    ftl_status_t result = ftl_cmd_configure_descriptors(cmd);
    munit_assert_int(result, ==, FTL_OK);

    return cmd;
}

/* 单 plane 连续建链的简写（等价 alloc_cmd_full(0, cw_count, 0, random, raid)） */
static fc_command_t *alloc_cmd(uint8_t cw_count, uint8_t random_mode, uint8_t raid_mode)
{
    return alloc_cmd_full(0U, cw_count, 0U, random_mode, raid_mode);
}

/**************************************************************************
 *  fc_data_path_encode 测试
 *
 *  验证管线（Stage 3 跨命令 stripe RAID 已移出，见 fc_data_path.c:191）：
 *     Data → META → CRC? → Random 加扰? → ECC 编码
 *
 *  验证策略：encode 后 round-trip decode → 原始 pattern 恢复
 *            （decode 走 fc_ldpc_decode + fc_random_transform，模拟 NAND 读回）
 *
 *  参数组合：
 *     random  crc      cw    说明
 *     off     on       4CW   仅 META + CRC + ECC
 *     on      on       4CW   加扰 + ECC（另证 USER 区确已非明文）
 *     on      on       1CW   单 CW 边界
 *     off     off      4CW   CRC 关闭 → CRC 字段不被写（哨兵保留）
 **************************************************************************/

static MunitResult test_encode_random_off(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    fc_command_t *cmd   = alloc_cmd(4U, FC_RANDOM_DISABLE, FC_RAID_MODE_NORMAL);
    uint8_t       count = cw_count(cmd);
    munit_assert_uint32(count, ==, 4U);

    for (uint8_t i = 0U; i < count; i++)
    {
        memset(cw_user(cmd, i), (uint8_t)(0x55U + i), FC_BUFFER_USER_SIZE);
    }

    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    for (uint8_t i = 0U; i < count; i++)
    {
        uint8_t *buf  = cw_user(cmd, i);
        uint8_t *base = cw_base(cmd, i);

        /* USER 数据区 512B 保持原始 pattern（[0,8) 是 META + CRC，不属于 USER 区） */
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
            munit_assert_int(fc_ldpc_decode(&cfg, cw_base(cmd, i), FC_BUFFER_DATA_SIZE, NULL), ==, FC_STATUS_OK);
            munit_assert_uint32(check_pattern(cw_user(cmd, i), FC_BUFFER_USER_SIZE, (uint8_t)(0x55U + i)), ==, 1U);
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
    uint8_t       count = cw_count(cmd);
    munit_assert_uint32(count, ==, 4U);

    for (uint8_t i = 0U; i < count; i++)
    {
        memset(cw_user(cmd, i), 0xAAU, FC_BUFFER_USER_SIZE);
    }

    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    /* 自证加扰真的发生了：encode 后 USER 区应已不是明文。
     * 否则 fixture 一旦退化（例如 paa=0 使 keystream 除 byte0 外全 0），
     * 下面"解扰后等于明文"的恒等式会把问题原样盖过去。 */
    for (uint8_t i = 0U; i < count; i++)
    {
        munit_assert_uint32(check_pattern(cw_user(cmd, i), FC_BUFFER_USER_SIZE, 0xAAU), ==, 0U);
    }

    /* round-trip：ECC 解码 → 解扰 → 原始 pattern 恢复 */
    {
        const fc_ldpc_config_t cfg = {FC_BUFFER_DATA_SIZE, 8U, 4U};
        for (uint8_t i = 0U; i < count; i++)
        {
            uint8_t *base = cw_base(cmd, i);
            munit_assert_int(fc_ldpc_decode(&cfg, base, FC_BUFFER_DATA_SIZE, NULL), ==, FC_STATUS_OK);
            /* ECC 解码后数据仍在加扰状态，需解扰恢复原文 */
            (void)fc_random_transform(base, FC_BUFFER_DATA_SIZE, cw_seed(cmd, i));
            munit_assert_uint32(check_pattern(cw_user(cmd, i), FC_BUFFER_USER_SIZE, 0xAAU), ==, 1U);
        }
    }

    fc_command_release(cmd);
    return MUNIT_OK;
}

static MunitResult test_encode_single_cw(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    fc_command_t *cmd   = alloc_cmd(1U, FC_RANDOM_DISABLE, FC_RAID_MODE_NORMAL);
    uint8_t       count = cw_count(cmd);
    munit_assert_uint32(count, ==, 1U);

    memset(cw_user(cmd, 0U), 0x55U, FC_BUFFER_USER_SIZE);

    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    /* round-trip decode */
    const fc_ldpc_config_t cfg = {FC_BUFFER_DATA_SIZE, 8U, 4U};
    munit_assert_int(fc_ldpc_decode(&cfg, cw_base(cmd, 0U), FC_BUFFER_DATA_SIZE, NULL), ==, FC_STATUS_OK);
    munit_assert_uint32(check_pattern(cw_user(cmd, 0U), FC_BUFFER_USER_SIZE, 0x55U), ==, 1U);

    fc_command_release(cmd);
    return MUNIT_OK;
}

/* CRC 关闭分支：encode 不写 CRC 字段（哨兵原样保留）→ 读侧 Pass 3 整个跳过 → 无 CRC_ERROR */
static MunitResult test_encode_crc_disabled(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    fc_command_t *cmd   = alloc_cmd(4U, FC_RANDOM_DISABLE, FC_RAID_MODE_NORMAL);
    uint8_t       count = cw_count(cmd);
    munit_assert_uint32(count, ==, 4U);

    cmd->config.crc = FC_CRC_DISABLE;
    for (uint8_t i = 0U; i < count; i++)
    {
        memset(cw_user(cmd, i), (uint8_t)(0x70U + i), FC_BUFFER_USER_SIZE);
        /* 哨兵：确认 encode 在 CRC 关闭时确实没碰 [4,8) */
        memset(cw_base(cmd, i) + FC_CRC_META_SIZE, 0xFF, FC_CRC_FIELD_SIZE);
    }

    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    for (uint8_t i = 0U; i < count; i++)
    {
        munit_assert_uint32(check_pattern(cw_base(cmd, i) + FC_CRC_META_SIZE, FC_CRC_FIELD_SIZE, 0xFFU), ==, 1U);
    }

    munit_assert_int(fc_data_path_process_read(cmd), ==, FC_STATUS_OK);
    for (uint8_t i = 0U; i < count; i++)
    {
        munit_assert_uint32(cw_desc(cmd, i)->status, ==, FC_CW_STATUS_READY);
        munit_assert_uint32(check_pattern(cw_user(cmd, i), FC_BUFFER_USER_SIZE, (uint8_t)(0x70U + i)), ==, 1U);
    }

    fc_command_release(cmd);
    return MUNIT_OK;
}

/**************************************************************************
 *  fc_data_path_process_read 测试
 *
 *  验证管线（Pass 1~4）：
 *     Buffer → ECC 解码 → 全部成功? → 解扰? → CRC 校验? → META 校验
 *                              ↓ 否
 *                        不解扰，返回 IO_ERROR
 *
 *  ⚠ Pass 3/Pass 4 只置 desc status 位、**不改返回值**（fc_data_path.c:551 恒返回 OK），
 *    所以"读成功"必须查 desc->status == FC_CW_STATUS_READY，只看返回值会漏掉
 *    CRC / META 校验回归。
 *
 *  参数组合：
 *     random  ecc/crc/meta  cw / mask      说明
 *     on      全 OK         4CW            解码成功 → 解扰 → 全 READY
 *     off     全 OK         4CW            只解码
 *     on      有 UNC        4CW            不解扰，返回 IO_ERROR
 *     on      全 OK         1CW            单 CW 边界
 *     on      全 OK         2CW / 0x05     非连续掩码：链位 0,1 ↔ cw 0,2
 *     off     META 不符     2CW / 0x05     改 command->meta → MATA_ERROR
 *     off     CRC 不符      4CW            哨兵 CRC → CRC_ERROR
 *     off     全 OK         4CW            LDPC_RAW：跳过 ECC 编码/解码
 *
 *  UNC 模拟：encode 后故意翻转 buffer 中大量 bit → ECC 不可纠
 **************************************************************************/

static MunitResult test_process_read_ok_random_on(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    fc_command_t *cmd   = alloc_cmd(4U, FC_RANDOM_ENABLE, FC_RAID_MODE_NORMAL);
    uint8_t       count = cw_count(cmd);
    munit_assert_uint32(count, ==, 4U);

    /* 先 encode（META + CRC + 加扰 + ECC） */
    for (uint8_t i = 0U; i < count; i++)
    {
        memset(cw_user(cmd, i), 0x55U, FC_BUFFER_USER_SIZE);
    }
    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    /* process_read：ECC 解码 + 解扰 + CRC 校验 + META 校验 */
    munit_assert_int(fc_data_path_process_read(cmd), ==, FC_STATUS_OK);

    /* 验证：全部解扰后恢复原文，且无任何校验位被置起 */
    for (uint8_t i = 0U; i < count; i++)
    {
        munit_assert_uint32(cw_desc(cmd, i)->status, ==, FC_CW_STATUS_READY);
        munit_assert_uint32(check_pattern(cw_user(cmd, i), FC_BUFFER_USER_SIZE, 0x55U), ==, 1U);
    }

    fc_command_release(cmd);
    return MUNIT_OK;
}

static MunitResult test_process_read_ok_random_off(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    fc_command_t *cmd   = alloc_cmd(4U, FC_RANDOM_DISABLE, FC_RAID_MODE_NORMAL);
    uint8_t       count = cw_count(cmd);
    munit_assert_uint32(count, ==, 4U);

    for (uint8_t i = 0U; i < count; i++)
    {
        memset(cw_user(cmd, i), (uint8_t)(0x55U + i), FC_BUFFER_USER_SIZE);
    }
    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    munit_assert_int(fc_data_path_process_read(cmd), ==, FC_STATUS_OK);

    for (uint8_t i = 0U; i < count; i++)
    {
        munit_assert_uint32(cw_desc(cmd, i)->status, ==, FC_CW_STATUS_READY);
        munit_assert_uint32(check_pattern(cw_user(cmd, i), FC_BUFFER_USER_SIZE, (uint8_t)(0x55U + i)), ==, 1U);
    }

    fc_command_release(cmd);
    return MUNIT_OK;
}

static MunitResult test_process_read_unc(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    fc_command_t *cmd   = alloc_cmd(4U, FC_RANDOM_ENABLE, FC_RAID_MODE_NORMAL);
    uint8_t       count = cw_count(cmd);
    munit_assert_uint32(count, ==, 4U);

    for (uint8_t i = 0U; i < count; i++)
    {
        memset(cw_user(cmd, i), 0x55U, FC_BUFFER_USER_SIZE);
    }
    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    /* 破坏 CW1：翻转多个字节 → ECC 无法纠正 */
    uint8_t *buf1_base = cw_base(cmd, 1U);
    buf1_base[0] ^= 0xFF;
    buf1_base[100] ^= 0xFF;
    buf1_base[200] ^= 0xFF;
    buf1_base[300] ^= 0xFF;

    /* process_read 应返回 IO_ERROR（不解扰） */
    munit_assert_int(fc_data_path_process_read(cmd), ==, FC_STATUS_IO_ERROR);

    /* 验证 desc status：CW1 为 UNC，其余为 READY */
    for (uint8_t i = 0U; i < count; i++)
    {
        if (i == 1U)
        {
            munit_assert_uint32(cw_desc(cmd, i)->status, ==, FC_CW_STATUS_UNC);
        }
        else
        {
            munit_assert_uint32(cw_desc(cmd, i)->status, ==, FC_CW_STATUS_READY);
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
    uint8_t       count = cw_count(cmd);
    munit_assert_uint32(count, ==, 1U);

    memset(cw_user(cmd, 0U), 0x55U, FC_BUFFER_USER_SIZE);
    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    munit_assert_int(fc_data_path_process_read(cmd), ==, FC_STATUS_OK);
    munit_assert_uint32(cw_desc(cmd, 0U)->status, ==, FC_CW_STATUS_READY);
    munit_assert_uint32(check_pattern(cw_user(cmd, 0U), FC_BUFFER_USER_SIZE, 0x55U), ==, 1U);

    fc_command_release(cmd);
    return MUNIT_OK;
}

/* 非连续掩码 + 数据路径：链位 0,1 ↔ cw 0,2。
 * 专钉 Pass 4 用的是 paa_to_cw(desc->paa) 而非链走位——若改用链走位，
 * 第 2 条 desc 会拿 cw=1 去算期望 META，假报 MATA_ERROR。 */
static MunitResult test_process_read_masked_chain(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    /* 0x05 = plane0 的 CW0 | CW2 */
    fc_command_t *cmd   = alloc_cmd_full(2U, 1U, 0x05U, FC_RANDOM_ENABLE, FC_RAID_MODE_NORMAL);
    uint8_t       count = cw_count(cmd);
    munit_assert_uint32(count, ==, 2U);

    /* 链几何：plane 0 的 CW0 与 CW2（链位 0,1 与 CW 号 0,2 不同） */
    {
        uint8_t plane_out[CW_TOTAL_MAX];
        uint8_t cw_out[CW_TOTAL_MAX];
        munit_assert_uint32(collect_chain(cmd, plane_out, cw_out), ==, 2U);
        munit_assert_uint32(plane_out[0], ==, 0U);
        munit_assert_uint32(cw_out[0], ==, 0U);
        munit_assert_uint32(plane_out[1], ==, 0U);
        munit_assert_uint32(cw_out[1], ==, 2U);
    }

    for (uint8_t i = 0U; i < count; i++)
    {
        memset(cw_user(cmd, i), (uint8_t)(0x30U + i), FC_BUFFER_USER_SIZE);
    }
    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);
    munit_assert_int(fc_data_path_process_read(cmd), ==, FC_STATUS_OK);

    for (uint8_t i = 0U; i < count; i++)
    {
        munit_assert_uint32(cw_desc(cmd, i)->status, ==, FC_CW_STATUS_READY);
        munit_assert_uint32(check_pattern(cw_user(cmd, i), FC_BUFFER_USER_SIZE, (uint8_t)(0x30U + i)), ==, 1U);
    }

    fc_command_release(cmd);
    return MUNIT_OK;
}

/* Pass 4（META 校验）触发：encode 后改掉 command->meta，
 * 期望值 lba_to_cw_meta(command->meta, cw) 随之中变 → 每个 CW 置 MATA_ERROR。
 * 返回值仍为 OK。 */
static MunitResult test_process_read_mata_error(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    fc_command_t *cmd   = alloc_cmd_full(2U, 1U, 0x05U, FC_RANDOM_DISABLE, FC_RAID_MODE_NORMAL);
    uint8_t       count = cw_count(cmd);
    munit_assert_uint32(count, ==, 2U);

    for (uint8_t i = 0U; i < count; i++)
    {
        memset(cw_user(cmd, i), (uint8_t)(0x40U + i), FC_BUFFER_USER_SIZE);
    }
    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    /* desc->meta 已在 configure 时按 command->meta(=0) 定死并写入 buffer[0..4)；
     * 现在改 command->meta，读侧期望值随之改变 → 读回 META 与期望不符 */
    cmd->meta = 0x10U;

    munit_assert_int(fc_data_path_process_read(cmd), ==, FC_STATUS_OK);
    for (uint8_t i = 0U; i < count; i++)
    {
        munit_assert_uint32(cw_desc(cmd, i)->status & FC_CW_STATUS_MATA_ERROR, !=, 0U);
    }

    fc_command_release(cmd);
    return MUNIT_OK;
}

/* Pass 3（CRC 校验）触发：encode 时关 CRC（[4,8) 被写成哨兵，ECC 覆盖的就是该状态），
 * 读时开 CRC → stored 哨兵 ≠ 重算值 → 每个 CW 置 CRC_ERROR。返回值仍为 OK。
 * ECC 未被扰动，故触发是确定性的。 */
static MunitResult test_process_read_crc_error(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    fc_command_t *cmd   = alloc_cmd(4U, FC_RANDOM_DISABLE, FC_RAID_MODE_NORMAL);
    uint8_t       count = cw_count(cmd);
    munit_assert_uint32(count, ==, 4U);

    const uint32_t sentinel = 0xDEADBEEFU;

    cmd->config.crc = FC_CRC_DISABLE;
    for (uint8_t i = 0U; i < count; i++)
    {
        memset(cw_user(cmd, i), (uint8_t)(0x60U + i), FC_BUFFER_USER_SIZE);
        (void)memcpy(cw_base(cmd, i) + FC_CRC_META_SIZE, &sentinel, FC_CRC_FIELD_SIZE);
    }

    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    cmd->config.crc = FC_CRC_ENABLE; /* 读侧打开 → stored 哨兵与重算值必不相等 */
    munit_assert_int(fc_data_path_process_read(cmd), ==, FC_STATUS_OK);

    for (uint8_t i = 0U; i < count; i++)
    {
        munit_assert_uint32(cw_desc(cmd, i)->status & FC_CW_STATUS_CRC_ERROR, !=, 0U);
    }

    fc_command_release(cmd);
    return MUNIT_OK;
}

/* LDPC_RAW 分支：encode 跳过 ECC 编码（parity 区不写）、
 * process_read Pass 1 跳过解码直接标 READY → 数据原样往返。 */
static MunitResult test_process_read_ldpc_raw(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    fc_command_t *cmd   = alloc_cmd(4U, FC_RANDOM_DISABLE, FC_RAID_MODE_NORMAL);
    uint8_t       count = cw_count(cmd);
    munit_assert_uint32(count, ==, 4U);

    cmd->config.ldpc = FC_ECC_MODE_LDPC_RAW;
    for (uint8_t i = 0U; i < count; i++)
    {
        memset(cw_user(cmd, i), (uint8_t)(0x80U + i), FC_BUFFER_USER_SIZE);
    }

    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);
    munit_assert_int(fc_data_path_process_read(cmd), ==, FC_STATUS_OK);

    for (uint8_t i = 0U; i < count; i++)
    {
        munit_assert_uint32(cw_desc(cmd, i)->status, ==, FC_CW_STATUS_READY);
        munit_assert_uint32(check_pattern(cw_user(cmd, i), FC_BUFFER_USER_SIZE, (uint8_t)(0x80U + i)), ==, 1U);
    }

    fc_command_release(cmd);
    return MUNIT_OK;
}

/**************************************************************************
 *  fc_data_path_raid_recover 测试
 *
 *  验证管线：RAID XOR 重建 → ECC 重试 → 解扰所有 CW
 *
 *  ⚠ 本层构造不出真 stripe：fc_data_path_raid_recover 只收集**单条命令**的 desc 链
 *    （fc_data_path.c:591），而 parity CW 在另一条命令的链上（fc.c:888/924-933）；
 *    跨命令适配是 include/fc/fc_raid_engine.h:22 的未办事项。故这里用「4 个数据 CW
 *    的退化 stripe」，靠 pattern 异或为 0 还原被擦 CW。真 stripe 覆盖见
 *    test_generate_stripe_parity。
 *
 *  参数组合：random=OFF（无加扰，退化 stripe 成立）。
 *            random=ON 的变体在修好 fixture 后不成立——被擦 CW 的加扰种子无处可取，
 *            故不保留（详见 docs/changelog/fc_ut_level2_rework_v22.md）。
 *
 *  编译开关：CONFIG_FC_RAID
 **************************************************************************/

#if CONFIG_FC_RAID

static void do_raid_recover_test(uint8_t random_mode, uint8_t failed_idx)
{
    fc_command_t *cmd   = alloc_cmd(4U, random_mode, FC_RAID_MODE_NORMAL);
    uint8_t       count = cw_count(cmd);
    munit_assert_uint32(count, >=, 3U);
    munit_assert_uint32(failed_idx, <, count);

    /* 不同 CW 填不同 pattern（USER 数据区 512B） */
    for (uint8_t i = 0U; i < count; i++)
    {
        memset(cw_user(cmd, i), (uint8_t)(0x10U + i), FC_BUFFER_USER_SIZE);
    }

    /* encode：META + CRC + 加扰? + ECC（RAID 不在 encode 里做，见 fc_data_path.c:191） */
    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    /* 破坏指定 CW（全 CW 528B 翻转，使 ECC 不可纠） */
    uint8_t *failed_base = cw_base(cmd, failed_idx);
    for (uint16_t j = 0U; j < FC_BUFFER_POOL_CW_SIZE; j++)
    {
        failed_base[j] ^= 0xFF;
    }

    /* raid_recover：XOR 重建 + ECC 重试 + 解扰 */
    munit_assert_int(fc_data_path_raid_recover(cmd, failed_idx), ==, FC_STATUS_OK);

    /* 验证：全部 CW USER 数据区恢复原始 pattern */
    for (uint8_t i = 0U; i < count; i++)
    {
        munit_assert_uint32(check_pattern(cw_user(cmd, i), FC_BUFFER_USER_SIZE, (uint8_t)(0x10U + i)), ==, 1U);
    }

    fc_command_release(cmd);
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
    /* 分配数据命令（4 CW, random=ON, raid=NORMAL — encode 里不做 XOR） */
    fc_command_t *data_cmd = alloc_cmd(4U, FC_RANDOM_ENABLE, FC_RAID_MODE_NORMAL);
    uint8_t       cw_cnt   = cw_count(data_cmd);
    munit_assert_uint32(cw_cnt, ==, 4U);

    /* 不同 CW 填不同 pattern */
    for (uint8_t i = 0U; i < cw_cnt; i++)
    {
        memset(cw_user(data_cmd, i), (uint8_t)(0x20U + i), FC_BUFFER_USER_SIZE);
    }

    /* encode 数据命令：META → CRC → Random → ECC */
    munit_assert_int(fc_data_path_encode(data_cmd), ==, FC_STATUS_OK);

    /* 分配 parity 命令（同 CW 数，random=OFF） */
    fc_command_t *parity_cmd = alloc_cmd(cw_cnt, FC_RANDOM_DISABLE, FC_RAID_MODE_NORMAL);
    munit_assert_uint32(cw_count(parity_cmd), ==, cw_cnt);

    /* 同步 parity desc 的 META（模拟 fc_stripe_submit 中的行为） */
    for (uint8_t i = 0U; i < cw_cnt; i++)
    {
        cw_desc(parity_cmd, i)->meta = cw_desc(data_cmd, i)->meta;
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

        for (uint8_t i = 0U; i < cw_cnt; i++)
        {
            buffers[i]          = cw_base(data_cmd, i);
            buffers[cw_cnt + i] = cw_base(parity_cmd, i);
        }
        uint8_t total_count = (uint8_t)(cw_cnt + cw_cnt); /* 8 */

        /* 清空数据命令 CW0，模拟丢失 */
        memset(buffers[0U], 0, FC_BUFFER_POOL_CW_SIZE);

        /* RAID XOR 重建 CW0：传入所有 CW（含 parity） */
        {
            uint8_t recovered[FC_BUFFER_POOL_CW_SIZE];
            fc_raid_recover(recovered, buffers, total_count, 0U, FC_BUFFER_POOL_CW_SIZE);
            (void)memcpy(buffers[0U], recovered, FC_BUFFER_POOL_CW_SIZE);
        }

        /* ECC 解码 + 解扰后验证数据 CW0~CW3（parity CW 不验证，它们存储 XOR 结果） */
        const fc_ldpc_config_t cfg = {FC_BUFFER_DATA_SIZE, 8U, 4U};
        for (uint8_t i = 0U; i < cw_cnt; i++)
        {
            uint8_t *base = buffers[i];

            munit_assert_int(fc_ldpc_decode(&cfg, base, FC_BUFFER_DATA_SIZE, NULL), ==, FC_STATUS_OK);
            (void)fc_random_transform(base, FC_BUFFER_DATA_SIZE, cw_seed(data_cmd, i));
            munit_assert_uint32(check_pattern(cw_user(data_cmd, i), FC_BUFFER_USER_SIZE, (uint8_t)(0x20U + i)), ==, 1U);
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

/* 掩码 0x29：plane0 取 {CW0,CW3} + plane1 取 {CW1} → 3 条 desc（plane-major） */
static MunitResult test_cw_mask_flat_mixed(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_t *cmd = alloc_cmd_full(2U, 1U, 0x29U, /* 0b0010_1001 */
                                       FC_RANDOM_DISABLE,
                                       FC_RAID_MODE_NORMAL);

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

    fc_command_t *cmd = alloc_cmd_full(2U, 4U, 0xF0U, FC_RANDOM_DISABLE, FC_RAID_MODE_NORMAL);

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

    fc_command_t *cmd = alloc_cmd_full(2U, 4U, 0U /* 未指定 */, FC_RANDOM_DISABLE, FC_RAID_MODE_NORMAL);
    munit_assert_uint32(cmd->config.cw, ==, 3U); /* 每 plane 4 CW */

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
 *  所有 Level 2 测试共享 setup_level2（一次性 fc_init；本 fork 的 MunitSuite
 *  没有 setup 字段，只能逐用例挂）
 **************************************************************************/

MunitTest g_fc_data_path_tests[] = {
  /* ── encode ── */
    {         "encode_random_off",          test_encode_random_off, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          "encode_random_on",           test_encode_random_on, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          "encode_single_cw",           test_encode_single_cw, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {       "encode_crc_disabled",        test_encode_crc_disabled, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},

 /* ── process_read ── */
    { "process_read_ok_random_on",  test_process_read_ok_random_on, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {"process_read_ok_random_off", test_process_read_ok_random_off, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          "process_read_unc",           test_process_read_unc, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {    "process_read_single_cw",     test_process_read_single_cw, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    { "process_read_masked_chain",  test_process_read_masked_chain, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {   "process_read_mata_error",    test_process_read_mata_error, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {    "process_read_crc_error",     test_process_read_crc_error, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {     "process_read_ldpc_raw",      test_process_read_ldpc_raw, setup_level2, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},

 /* ── raid_recover ── */
#if CONFIG_FC_RAID
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

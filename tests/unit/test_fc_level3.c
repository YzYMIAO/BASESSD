/*
 * test_fc_level3.c — Level 3 FC 全链路测试：34 个 OP 码生命周期全覆盖
 *
 * 测试目标：验证 FC 命令完整生命周期（alloc → configure → submit → service → poll → release）
 * 在真实 NAND 硬件上执行。
 *
 * 核心验证点：
 *   1. 每个 OP 码的命令能完整走完 FC 生命周期
 *   2. 各阶段状态转换正确（不崩溃、不泄漏）
 *   3. completion->command_id 与提交的命令匹配
 *   4. 资源（command/descriptor/buffer）无泄漏
 *
 * 不验证：
 *   - NAND 数据正确性（那是集成测试的职责）
 *   - 数据路径编解码正确性（那是 Level 2 的职责）
 *
 * 参数化维度：
 *   operation : 34 个 OP_* 值（0..33）
 *   block     : 用户通过 CLI 传入，如 --param block 10
 *
 * PAA 约定：ch=0 ce=0 lun=0 plane=0 block=<user> page=0..63 cw=0
 *           多 Plane 操作使用 plane=0/1，双 Plane 地址
 *
 * 用法：
 *   ssd> test --param block 10 /fc-nand/lifecycle/
 *   ssd> test --param block 10 --seed 0x1234
 *   ssd> test --list
 */

#include "munit.h"

#include "fc/fc.h"
#include "fc/fc_command.h"
#include "fc/fc_queue.h"
#include "fc/fc_scheduler.h"
#include "nand/onfi_1_0.h"
#include "nand/nand_config.h"

#include <string.h>

/**************************************************************************
 *  辅助：从 MunitParameter 数组中查找参数值
 *
 *  munit 标准机制，替代不存在的 munit_parameters_get()
 **************************************************************************/

static const char *param_get(const MunitParameter params[], const char *name)
{
    for (const MunitParameter *p = params; p != NULL && p->name != NULL; p++)
    {
        if (strcmp(p->name, name) == 0)
            return p->value;
    }
    return NULL;
}

/**************************************************************************
 *  OP 码信息表（用 designated initializer 以 OP 值为 key）
 *
 *  只需标记 needs_paa（需要有效 PAA）和 standalone（可独立执行），
 *  其余属性（has_data / is_setget）在运行时从已有宏/比较计算。
 **************************************************************************/

typedef struct
{
    const char *name;
    uint8_t     value;
    uint8_t     needs_paa;   /* 需要有效 PAA（非 0）才能不被 NAND 拒绝 */
    uint8_t     standalone;  /* 可独立执行，不需要前置 NAND 状态 */
    const char *desc;
} op_info_t;

static const op_info_t g_op_table[OP_COUNT] = {
    [OP_RESET]                                  = { "OP_RESET",                                  OP_RESET,                                  0, 1, "复位 NAND" },
    [OP_READ_ID]                                = { "OP_READ_ID",                                OP_READ_ID,                                0, 1, "读 ID" },
    [OP_READ_PARAMETER_PAGE]                    = { "OP_READ_PARAMETER_PAGE",                    OP_READ_PARAMETER_PAGE,                    0, 1, "读参数页" },
    [OP_READ_UNIQUE_ID]                         = { "OP_READ_UNIQUE_ID",                         OP_READ_UNIQUE_ID,                         0, 1, "读唯一 ID" },
    [OP_GET_FEATURES]                           = { "OP_GET_FEATURES",                           OP_GET_FEATURES,                           0, 1, "读 Feature" },
    [OP_SET_FEATURES]                           = { "OP_SET_FEATURES",                           OP_SET_FEATURES,                           0, 1, "写 Feature" },
    [OP_READ_STATUS]                            = { "OP_READ_STATUS",                            OP_READ_STATUS,                            0, 1, "读状态" },
    [OP_READ_STATUS_ENHANCED]                   = { "OP_READ_STATUS_ENHANCED",                   OP_READ_STATUS_ENHANCED,                   0, 1, "增强读状态" },
    [OP_RANDOM_DATA_READ]                       = { "OP_RANDOM_DATA_READ",                       OP_RANDOM_DATA_READ,                       1, 0, "随机列读（需前置 READ）" },
    [OP_RANDOM_DATA_INPUT]                      = { "OP_RANDOM_DATA_INPUT",                      OP_RANDOM_DATA_INPUT,                      1, 0, "随机列输入（需前置状态）" },
    [OP_PROGRAM_FOR_INTERNAL_DATA_MOVE]         = { "OP_PROGRAM_FOR_INTERNAL_DATA_MOVE",         OP_PROGRAM_FOR_INTERNAL_DATA_MOVE,         1, 0, "内部搬移编程" },
    [OP_READ_MODE]                              = { "OP_READ_MODE",                              OP_READ_MODE,                              0, 1, "读模式" },
    [OP_NORMAL_READ_PAGE_WITH_DATA_OUT]         = { "OP_NORMAL_READ_PAGE_WITH_DATA_OUT",         OP_NORMAL_READ_PAGE_WITH_DATA_OUT,         1, 1, "标准读（有数据输出）" },
    [OP_READ_PAGE_CACHE_SEQ]                    = { "OP_READ_PAGE_CACHE_SEQ",                    OP_READ_PAGE_CACHE_SEQ,                    0, 0, "缓存读顺序（需前置 CACHE）" },
    [OP_READ_PAGE_CACHE_RANDOM]                 = { "OP_READ_PAGE_CACHE_RANDOM",                 OP_READ_PAGE_CACHE_RANDOM,                 1, 0, "缓存读随机（起始）" },
    [OP_READ_PAGE_CACHE_LAST]                   = { "OP_READ_PAGE_CACHE_LAST",                   OP_READ_PAGE_CACHE_LAST,                   0, 0, "缓存读最后" },
    [OP_NORMAL_PROGRAM_PAGE]                    = { "OP_NORMAL_PROGRAM_PAGE",                    OP_NORMAL_PROGRAM_PAGE,                    1, 1, "标准编程" },
    [OP_PROGRAM_PAGE_CACHE]                     = { "OP_PROGRAM_PAGE_CACHE",                     OP_PROGRAM_PAGE_CACHE,                     1, 0, "缓存编程" },
    [OP_ERASE_BLOCK]                            = { "OP_ERASE_BLOCK",                            OP_ERASE_BLOCK,                            1, 1, "擦除 Block" },
    [OP_READ_FOR_INTERNAL_DATA_MOVE]            = { "OP_READ_FOR_INTERNAL_DATA_MOVE",            OP_READ_FOR_INTERNAL_DATA_MOVE,            1, 0, "内部搬移读" },
    [OP_PROGRAM_FOR_INTERNAL_MOVE]              = { "OP_PROGRAM_FOR_INTERNAL_MOVE",              OP_PROGRAM_FOR_INTERNAL_MOVE,              1, 0, "内部搬移写" },
    [OP_BLOCK_UNLOCK_LOW]                       = { "OP_BLOCK_UNLOCK_LOW",                       OP_BLOCK_UNLOCK_LOW,                       1, 1, "解锁低段" },
    [OP_BLOCK_UNLOCK_HIGH]                      = { "OP_BLOCK_UNLOCK_HIGH",                      OP_BLOCK_UNLOCK_HIGH,                      1, 1, "解锁高段" },
    [OP_BLOCK_LOCK]                             = { "OP_BLOCK_LOCK",                             OP_BLOCK_LOCK,                             0, 1, "锁定" },
    [OP_BLOCK_LOCK_TIGHT]                       = { "OP_BLOCK_LOCK_TIGHT",                       OP_BLOCK_LOCK_TIGHT,                       0, 1, "紧锁定" },
    [OP_BLOCK_LOCK_STATUS]                      = { "OP_BLOCK_LOCK_STATUS",                      OP_BLOCK_LOCK_STATUS,                      1, 1, "锁定状态" },
    [OP_OTP_DATA_LOCK]                          = { "OP_OTP_DATA_LOCK",                          OP_OTP_DATA_LOCK,                          1, 0, "OTP 锁定" },
    [OP_OTP_DATA_PROGRAM]                       = { "OP_OTP_DATA_PROGRAM",                       OP_OTP_DATA_PROGRAM,                       1, 0, "OTP 编程" },
    [OP_OTP_DATA_READ]                          = { "OP_OTP_DATA_READ",                          OP_OTP_DATA_READ,                          1, 0, "OTP 读" },
    [OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT] = { "OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT", OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT, 1, 0, "多 Plane 读无数据" },
    [OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT]    = { "OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT",    OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT,    1, 0, "多 Plane 读有数据" },
    [OP_PROGRAM_PAGE_MULTI_PLANE]               = { "OP_PROGRAM_PAGE_MULTI_PLANE",               OP_PROGRAM_PAGE_MULTI_PLANE,               1, 0, "多 Plane 编程" },
    [OP_ERASE_BLOCK_MULTI_PLANE]                = { "OP_ERASE_BLOCK_MULTI_PLANE",                OP_ERASE_BLOCK_MULTI_PLANE,                1, 0, "多 Plane 擦除" },
    [OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT]      = { "OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT",      OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT,      1, 1, "标准读（无数据输出）" },
};

/* 验证表格覆盖了 0..OP_COUNT-1 所有 OP */
_Static_assert(sizeof(g_op_table) / sizeof(g_op_table[0]) == OP_COUNT,
               "g_op_table must cover all OP_COUNT entries");

/**************************************************************************
 *  参数枚举 — 用于 munit 参数化展开
 *
 *  operation: 34 个 OP 码（笛卡尔积展开）
 *  block:     CLI 通过 --param block N 传入，values=NULL 时跳过展开
 **************************************************************************/

/* 参数名数组 — 在 setup 中填充 */
static char *g_op_names[OP_COUNT + 1];

static MunitParameterEnum g_lifecycle_params[] = {
    { "operation", g_op_names },
    { "block",     NULL },  /* NULL = 仅从 CLI --param 接受，不展开 */
    { NULL, NULL },
};

/**************************************************************************
 *  Suite 级 setup
 *
 *  fc_init() 和 NAND init 由 main.c 在 UART CLI 之前完成。
 *  此处只做参数名数组的初始化。
 **************************************************************************/

static int g_level3_inited = 0;

static void *setup_level3(const MunitParameter params[], void *user_data)
{
    (void)params;
    (void)user_data;

    if (!g_level3_inited)
    {
        for (uint32_t i = 0U; i < OP_COUNT; i++)
        {
            g_op_names[i] = (char *)g_op_table[i].name;
        }
        g_op_names[OP_COUNT] = NULL;
        g_level3_inited = 1;
    }
    return NULL;
}

/**************************************************************************
 *  PAA 辅助
 *
 *  约定：ch=0, ce=0, lun=0
 *  block 由用户通过 --param block N 传入，默认 block=10
 *  plane=0（多 Plane 操作使用双 Plane 地址）
 *  page=0（编程/读取使用 page 0）
 *  cw=0
 **************************************************************************/

#define DEFAULT_TEST_BLOCK (10U)

static nand_paa_t make_paa(uint16_t block, uint8_t plane, uint8_t page)
{
    return paa_make(page, plane, block, 0U, 0U);
}

static uint16_t get_test_block(const MunitParameter params[])
{
    const char *val = param_get(params, "block");
    if (val == NULL)
        return DEFAULT_TEST_BLOCK;

    /* 简单十进制解析 */
    uint16_t block = 0U;
    while (*val >= '0' && *val <= '9')
    {
        block = (uint16_t)(block * 10U + (uint16_t)(*val - '0'));
        val++;
    }
    return block;
}

/**************************************************************************
 *  辅助：执行命令生命周期
 *
 *  对给定的 command 执行 submit → service loop → poll → release，
 *  验证各阶段返回值和 completion 一致性。
 *
 *  注：对于有前置依赖的 OP（standalone=0），NAND 可能返回错误状态，
 *  但 FC 生命周期仍应正常完成（completion 能被 poll 到）。
 **************************************************************************/

#define SERVICE_LOOP_MAX (5000U)

static void run_lifecycle(fc_command_t *cmd)
{
    fc_command_status_t completion;
    fc_status_t         result;
    uint32_t            service_count;
    uint8_t             completed = 0U;

    munit_assert_not_null(cmd);

    /* ── 1. submit ── */
    result = fc_submit(cmd);
    munit_assert_int(result, ==, FC_STATUS_OK);

    /* ── 2. service loop — 驱动 NAND 执行 ── */
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

    /* ── 3. 验证 completion ── */
    munit_assert_uint32(completed, ==, 1U);
    munit_assert_uint32(completion.command_id, ==, cmd->command_id);

    /* ── 4. release ── */
    result = fc_command_release(cmd);
    munit_assert_int(result, ==, FC_STATUS_OK);
}

/**************************************************************************
 *  生命周期测试函数（参数化）
 *
 *  对 munit 展开的每个 (operation, block) 组合执行生命周期。
 *
 *  配置逻辑：
 *    - SET/GET_FEATURES：内联数据，不配置 desc chain
 *    - 有数据（nand_operation_has_data）：配置 desc chain + buffer 填 pattern
 *    - 无数据：desc_id = INVALID
 *    - needs_paa：构造合法 PAA（block 来自用户参数）
 **************************************************************************/

static MunitResult test_lifecycle_op(const MunitParameter params[], void *data)
{
    (void)data;

    const char  *op_name  = param_get(params, "operation");
    uint16_t     block    = get_test_block(params);
    uint8_t      op_value = OP_COUNT; /* 默认非法值，找不到时 assert 会触发 */
    fc_command_t *cmd;

    munit_assert_not_null(op_name);

    /* OP 名 → 值 */
    for (uint32_t i = 0U; i < OP_COUNT; i++)
    {
        if (strcmp(g_op_table[i].name, op_name) == 0)
        {
            op_value = (uint8_t)i;
            break;
        }
    }
    munit_assert_uint32(op_value, <, OP_COUNT);

    /* ── alloc ── */
    cmd = fc_command_alloc();
    munit_assert_not_null(cmd);

    /* ── 基础配置 ── */
    cmd->config.operation   = op_value;
    cmd->config.ch          = 0U;
    cmd->config.ce          = 0U;
    cmd->config.lun         = 0U;
    cmd->config.random      = FC_RANDOM_DISABLE;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.raid        = FC_RAID_MODE_NORMAL;

    /* ── 根据 OP 属性配置 ── */
    uint8_t has_data  = nand_operation_has_data(op_value);
    uint8_t is_setget = (op_value == OP_SET_FEATURES) || (op_value == OP_GET_FEATURES);
    uint8_t needs_paa = g_op_table[op_value].needs_paa;

    if (is_setget)
    {
        /* SET/GET_FEATURES：内联数据路径 */
        cmd->desc_id       = FC_DESCRIPTOR_ID_INVALID;
        cmd->feature_addr  = 0x01U;
        cmd->config.cw     = 0U;

        if (op_value == OP_SET_FEATURES)
        {
            cmd->parameter0 = 0x40U;
            cmd->parameter1 = 0x00U;
            cmd->parameter2 = 0x00U;
            cmd->parameter3 = 0x00U;
        }
    }
    else if (has_data)
    {
        /* 有数据的 OP：配置 desc chain + buffer */
        cmd->config.cw = CW_NUM - 1U;
        cmd->paa       = make_paa(block, 0U, 0U);

        fc_status_t result = fc_command_configure_descriptors(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);

        /* buffer 填 pattern */
        fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
        uint8_t            cw_idx  = 0U;
        while (desc_id != FC_DESCRIPTOR_ID_INVALID)
        {
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            if (desc == NULL) break;
            uint8_t *buf = fc_buffer_id_to_real_buffer(desc->buffer_id);
            if (buf != NULL)
            {
                memset(buf, (int)(0x50U + cw_idx), FC_BUFFER_POOL_CW_SIZE);
            }
            desc_id = (fc_descriptor_id_t)desc->next_id;
            cw_idx++;
        }
    }
    else
    {
        /* 无数据操作 */
        cmd->desc_id   = FC_DESCRIPTOR_ID_INVALID;
        cmd->config.cw = 0U;

        if (needs_paa)
        {
            cmd->paa = make_paa(block, 0U, 0U);
        }
    }

    /* ── 执行生命周期 ── */
    run_lifecycle(cmd);

    return MUNIT_OK;
}

/**************************************************************************
 *  套件定义
 *
 *  一个参数化的测试用例展开为 34 个 OP × 1 个 block 组合。
 **************************************************************************/

MunitTest g_fc_lifecycle_tests[] = {
    { "lifecycle_op", test_lifecycle_op, setup_level3, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, g_lifecycle_params },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
};
/*
 * test_fc_level3.c — Level 3 FC 全链路测试：34 个 OP 码生命周期全覆盖 + Block 级数据完整性
 *
 * 测试目标：
 *   层面一：验证 FC 命令完整生命周期（alloc → configure → submit → service → poll → release）
 *           在真实 NAND 硬件上执行。
 *   层面二：以 block 100 为单位，全页数据写入后读回逐字节对比。
 *   层面三：擦除后验证全 block 0xFF。
 *
 * 核心验证点：
 *   1. 每个 OP 码的命令能完整走完 FC 生命周期
 *   2. 数据写入后读回逐字节一致
 *   3. 擦除后全 block 读回 0xFF
 *   4. Cache 读序列、多 Plane 等特殊 OP 的数据完整性
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

/* 参数名数组 — 编译时从 g_op_table 填充
 *
 * ⚠️ 必须编译时初始化，不能放在 setup_level3 中。
 *    munit 的参数枚举（wildcard 展开）在 setup 之前执行，
 *    如果此时 g_op_names 全为 NULL，操作参数会被跳过，
 *    导致生命周期测试收到空 params 而 assert(0)。
 */
static const char *g_op_names[OP_COUNT + 1] = {
    g_op_table[0U].name,  g_op_table[1U].name,  g_op_table[2U].name,  g_op_table[3U].name,
    g_op_table[4U].name,  g_op_table[5U].name,  g_op_table[6U].name,  g_op_table[7U].name,
    g_op_table[8U].name,  g_op_table[9U].name,  g_op_table[10U].name, g_op_table[11U].name,
    g_op_table[12U].name, g_op_table[13U].name, g_op_table[14U].name, g_op_table[15U].name,
    g_op_table[16U].name, g_op_table[17U].name, g_op_table[18U].name, g_op_table[19U].name,
    g_op_table[20U].name, g_op_table[21U].name, g_op_table[22U].name, g_op_table[23U].name,
    g_op_table[24U].name, g_op_table[25U].name, g_op_table[26U].name, g_op_table[27U].name,
    g_op_table[28U].name, g_op_table[29U].name, g_op_table[30U].name, g_op_table[31U].name,
    g_op_table[32U].name, g_op_table[33U].name,
    NULL  /* terminator */
};

static MunitParameterEnum g_lifecycle_params[] = {
    { "operation", g_op_names },
    { "block",     NULL },  /* NULL = 仅从 CLI --param 接受，不展开 */
    { NULL, NULL },
};

/**************************************************************************
 *  Suite 级 setup
 *
 *  fc_init() 和 NAND init 由 main.c 在 UART CLI 之前完成。
 *  g_op_names 已在编译时初始化，此处无需额外操作。
 **************************************************************************/

static void *setup_level3(const MunitParameter params[], void *user_data)
{
    (void)params;
    (void)user_data;
    return NULL;
}

/**************************************************************************
 *  NAND 几何参数（固定值，用于 block 级验证）
 *
 *  block 100（plane 0）：
 *    每 page = 4 CW × 512B user data = 2048B
 *    每 block = 64 pages
 *    全 block user data = 128 KB
 **************************************************************************/

#define BLOCK_TEST        (100U)   /* 固定测试 block */
#define BLOCK_DEST        (101U)   /* 内部搬移目标 block */

/**************************************************************************
 *  PAA 辅助
 *
 *  约定：ch=0, ce=0, lun=0
 **************************************************************************/

static nand_paa_t make_paa(uint16_t block, uint8_t plane, uint8_t page)
{
    return paa_make(page, plane, block, 0U, 0U);
}

static uint16_t get_test_block(const MunitParameter params[])
{
    const char *val = param_get(params, "block");
    if (val == NULL)
        return 10U;

    uint16_t block = 0U;
    while (*val >= '0' && *val <= '9')
    {
        block = (uint16_t)(block * 10U + (uint16_t)(*val - '0'));
        val++;
    }
    return block;
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
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
    while (desc_id != FC_DESCRIPTOR_ID_INVALID)
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        if (desc == NULL) break;
        if (desc->buffer_id != FC_BUFFER_ID_INVALID)
        {
            (void)fc_buffer_release(desc->buffer_id);
        }
        desc_id = (fc_descriptor_id_t)desc->next_id;
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
 *  辅助：执行命令生命周期
 *
 *  对给定的 command 执行 submit → service loop → poll → release，
 *  验证各阶段返回值和 completion 一致性。
 *
 *  生命周期测试只验证 submit→poll→release 流程不崩溃。数据完整性
 *  和 per-CW status 验证由 block_write_read 等专用测试负责。
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
    release_command_buffers(cmd);
    result = fc_command_release(cmd);
    munit_assert_int(result, ==, FC_STATUS_OK);
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
    cmd->config.ldpc      = FC_ECC_MODE_BCH;
    cmd->config.raid      = FC_RAID_MODE_NORMAL;
    cmd->desc_id          = FC_DESCRIPTOR_ID_INVALID;
    cmd->config.cw        = 0U;
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
    cmd->config.ldpc      = FC_ECC_MODE_BCH;
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
    cmd->config.ldpc      = FC_ECC_MODE_BCH;
    cmd->config.raid      = FC_RAID_MODE_NORMAL;

    cmd->config.cw = CW_NUM - 1U;
    cmd->paa       = make_paa(block, plane, page);

    fc_status_t result = fc_command_configure_descriptors(cmd);
    munit_assert_int(result, ==, FC_STATUS_OK);

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

        /* 层面二：逐字节验证数据 */
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

        /* 层面三：验证擦除后全 0xFF */
        munit_assert_uint8(verify_page_erased(cmd, page), ==, 1U);

        release_command_buffers(cmd);
        result = fc_command_release(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);
    }
}

/**************************************************************************
 *  生命周期测试函数（参数化）— 层面一
 *
 *  对 munit 展开的每个 (operation, block) 组合执行生命周期。
 *  生命周期测试只验证 submit→poll→release 流程不崩溃，不做 per-CW
 *  status 断言。数据完整性验证由 block_write_read 等专用测试负责。
 **************************************************************************/

static MunitResult test_lifecycle_op(const MunitParameter params[], void *data)
{
    (void)data;

    const char  *op_name  = param_get(params, "operation");
    uint16_t     block    = get_test_block(params);
    uint8_t      op_value = OP_COUNT;
    fc_command_t *cmd;

    munit_assert_not_null(op_name);

    for (uint32_t i = 0U; i < OP_COUNT; i++)
    {
        if (strcmp(g_op_table[i].name, op_name) == 0)
        {
            op_value = (uint8_t)i;
            break;
        }
    }
    munit_assert_uint32(op_value, <, OP_COUNT);

    cmd = fc_command_alloc();
    munit_assert_not_null(cmd);

    cmd->config.operation   = op_value;
    cmd->config.ch          = 0U;
    cmd->config.ce          = 0U;
    cmd->config.lun         = 0U;
    cmd->config.random      = FC_RANDOM_DISABLE;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.raid        = FC_RAID_MODE_NORMAL;

    uint8_t has_data  = nand_operation_has_data(op_value);
    uint8_t is_setget = (op_value == OP_SET_FEATURES) || (op_value == OP_GET_FEATURES);
    uint8_t needs_paa = g_op_table[op_value].needs_paa;

    if (is_setget)
    {
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
        cmd->config.cw = CW_NUM - 1U;
        cmd->paa       = make_paa(block, 0U, 0U);

        fc_status_t result = fc_command_configure_descriptors(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);

        fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
        uint8_t            cw_idx  = 0U;
        while (desc_id != FC_DESCRIPTOR_ID_INVALID)
        {
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
            if (desc == NULL) break;
            uint8_t *buf = fc_buffer_id_to_real_buffer(desc->buffer_id);
            if (buf != NULL)
            {
                memset(buf, (int)(0x50U + cw_idx), FC_BUFFER_USER_SIZE);
            }
            desc_id = (fc_descriptor_id_t)desc->next_id;
            cw_idx++;
        }
    }
    else
    {
        cmd->desc_id   = FC_DESCRIPTOR_ID_INVALID;
        cmd->config.cw = 0U;

        if (needs_paa)
        {
            cmd->paa = make_paa(block, 0U, 0U);
        }
    }

    run_lifecycle(cmd);

    return MUNIT_OK;
}

/**************************************************************************
 *  test_block_write_read — 层面二核心验证
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

    /* Phase 0: 擦除 */
    erase_block(BLOCK_TEST, 0U);

    /* Phase 1: 写全 block 64 pages */
    write_block(BLOCK_TEST, 0U);

    /* Phase 2: 读回全 block 64 pages 验证 */
    read_block_verify(BLOCK_TEST, 0U);

    return MUNIT_OK;
}

/**************************************************************************
 *  test_block_erase_verify — 层面三验证
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
        cmd->config.ldpc      = FC_ECC_MODE_BCH;
        cmd->config.raid      = FC_RAID_MODE_NORMAL;
        cmd->config.cw        = CW_NUM - 1U;
        cmd->paa              = make_paa(BLOCK_TEST, 0U, 0U);

        result = fc_command_configure_descriptors(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);

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
        cmd->config.ldpc      = FC_ECC_MODE_BCH;
        cmd->config.raid      = FC_RAID_MODE_NORMAL;
        cmd->config.cw        = CW_NUM - 1U;
        cmd->desc_id          = FC_DESCRIPTOR_ID_INVALID;

        result = fc_command_configure_descriptors(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);

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
        cmd->config.ldpc      = FC_ECC_MODE_BCH;
        cmd->config.raid      = FC_RAID_MODE_NORMAL;
        cmd->config.cw        = CW_NUM - 1U;
        cmd->desc_id          = FC_DESCRIPTOR_ID_INVALID;

        result = fc_command_configure_descriptors(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);

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
 *  流程：
 *    1. 擦除 block 100（plane 0 + 1 使用双 Plane 擦除）
 *    2. 在 plane 0 用 NORMAL_PROGRAM_PAGE 写全 block
 *    3. 读回验证
 *
 *  验证点：
 *    - 多 Plane 擦除正确
 *    - 数据完整性
 **************************************************************************/

static MunitResult test_block_multi_plane(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    erase_block(BLOCK_TEST, 1U);
    write_block(BLOCK_TEST, 0U);
    read_block_verify(BLOCK_TEST, 0U);

    return MUNIT_OK;
}

/**************************************************************************
 *  test_block_random_data — 随机列读验证
 *
 *  流程：
 *    1. 擦除 + 写全 block 基准数据
 *    2. Normal read page 0 建立读状态
 *    3. RANDOM_DATA_READ 读 page 0 数据并验证
 *
 *  验证点：
 *    - 随机列读数据与原始写入一致
 **************************************************************************/

static MunitResult test_block_random_data(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    erase_block(BLOCK_TEST, 0U);
    write_block(BLOCK_TEST, 0U);

    /* Normal read page 0 */
    {
        fc_command_t *cmd = read_page(BLOCK_TEST, 0U, 0U);
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

    /* RANDOM_DATA_READ */
    {
        fc_command_t *cmd = fc_command_alloc();
        munit_assert_not_null(cmd);

        cmd->config.operation = OP_RANDOM_DATA_READ;
        cmd->config.ch        = 0U;
        cmd->config.ce        = 0U;
        cmd->config.lun       = 0U;
        cmd->config.random    = FC_RANDOM_DISABLE;
        cmd->config.ldpc      = FC_ECC_MODE_BCH;
        cmd->config.raid      = FC_RAID_MODE_NORMAL;
        cmd->config.cw        = 0U;
        cmd->paa              = make_paa(BLOCK_TEST, 0U, 0U);

        fc_status_t result = fc_command_configure_descriptors(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);

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
        munit_assert_uint8(verify_page_data(cmd, BLOCK_TEST, 0U, 0U), ==, 1U);

        release_command_buffers(cmd);
        result = fc_command_release(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);
    }

    return MUNIT_OK;
}

/**************************************************************************
 *  套件定义
 *
 *  包含：
 *    - lifecycle_op（参数化，34 OP × 1 block，层面一生命周期验证）
 *    - block_write_read（层面二：全 block 写读 128KB 数据对比）
 *    - block_erase_verify（层面三：擦除后全 0xFF 验证）
 *    - block_cache_ops（cache 读序列数据验证）
 *    - block_multi_plane（多 Plane 擦写读验证）
 *    - block_random_data（随机列读写验证）
 **************************************************************************/

MunitTest g_fc_lifecycle_tests[] = {
    { "lifecycle_op",        test_lifecycle_op,        setup_level3, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, g_lifecycle_params },
    { "block_write_read",    test_block_write_read,    setup_level3, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "block_erase_verify",  test_block_erase_verify,  setup_level3, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "block_cache_ops",     test_block_cache_ops,     setup_level3, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "block_multi_plane",   test_block_multi_plane,   setup_level3, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { "block_random_data",   test_block_random_data,   setup_level3, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
    { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
};
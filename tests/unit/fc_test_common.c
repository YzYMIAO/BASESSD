/*
 * fc_test_common.c — FC-NAND 单元测试共享夹具（实现）
 *
 * 接口与契约见 fc_test_common.h。本文件只放「测试脚手架」，不含被测逻辑。
 */

#include "fc_test_common.h"

#include "fc/fc_data_path.h"
#include "fc/fc_random_engine.h"
#include "nand/nand_dispatch.h"

#include <stdlib.h>
#include <string.h>

/* 源数据槽图案：槽号 (plane*CW_NUM + cw) 占高 3 位，槽内位置占低 5 位。
 * slot 0..7 → 0x00/0x20/…/0xE0，槽内 0..31 ⇒ 值域恰好铺满 [0x00, 0xFF]，
 * 且相邻槽的最小差 0x20 —— 读错 CW / 读错 plane 立刻显形。 */
#define TEST_SOURCE_SLOT_STRIDE (0x20U)
#define TEST_SOURCE_SLOT_MASK   (0x1FU)

uint8_t *g_source_data = NULL;

/**************************************************************************
 *  setup / teardown
 **************************************************************************/

void *test_setup(const MunitParameter params[], void *user_data)
{
    /* fc_init() 只在第一次调用时执行：本 fork 的 MunitSuite 没有 setup 字段，
     * 逐用例挂 setup 就退化成「每用例一次」，而 fc_init 会重置全部资源池——
     * 每用例重初始化会把「上一个用例漏还的命令/desc/buffer」洗掉，掩盖泄漏。 */
    static uint8_t inited = 0U;

    (void)params;
    (void)user_data;

    if (inited == 0U)
    {
        (void)fc_init();
        inited = 1U;
    }

    /* 异步 dispatch 放行闩锁（main.c 在 NAND 初始化后已置 1，此处幂等重申）。
     * 本套件走 fc_service() → nand_fc_start/step 异步路径，闩锁必须为 1。 */
    nand_dispatch_set_armed(1U);

    if (g_source_data == NULL)
    {
        const uint32_t slots = (uint32_t)CW_NUM * (uint32_t)PLANE_NUM;

        g_source_data = (uint8_t *)malloc((size_t)FC_BUFFER_USER_SIZE * slots);
        munit_assert_not_null(g_source_data);

        for (uint8_t plane = 0U; plane < PLANE_NUM; plane++)
        {
            for (uint8_t cw = 0U; cw < CW_NUM; cw++)
            {
                uint8_t *slot = test_source_slot(plane, cw);
                uint8_t  base = (uint8_t)(((uint32_t)plane * CW_NUM + (uint32_t)cw) * TEST_SOURCE_SLOT_STRIDE);

                for (uint16_t i = 0U; i < FC_BUFFER_USER_SIZE; i++)
                {
                    slot[i] = (uint8_t)(base + (uint8_t)(i & TEST_SOURCE_SLOT_MASK));
                }
            }
        }
    }

    return NULL;
}

void test_tear_down(void *fixture)
{
    (void)fixture;

    if (g_source_data != NULL)
    {
        free(g_source_data);
        g_source_data = NULL;
    }
}

/**************************************************************************
 *  命令构造
 **************************************************************************/

/* plane_bitmap 中最低置位的 plane 号（调用方已断言非 0） */
static uint8_t test_first_plane(uint8_t plane_bitmap)
{
    for (uint8_t p = 0U; p < PLANE_NUM; p++)
    {
        if ((plane_bitmap & TEST_PLANE_BM(p)) != 0U)
        {
            return p;
        }
    }
    return 0U; /* 不可达 */
}

static uint8_t test_plane_count(uint8_t plane_bitmap)
{
    uint8_t count = 0U;

    for (uint8_t p = 0U; p < PLANE_NUM; p++)
    {
        if ((plane_bitmap & TEST_PLANE_BM(p)) != 0U)
        {
            count++;
        }
    }
    return count;
}

/* 按 multi_flag 选 op 变体 */
static uint8_t test_resolve_operation(test_page_op_t operation, uint8_t multi_flag)
{
    if (multi_flag != 0U)
    {
        switch (operation)
        {
            case TEST_OP_PROGRAM:
                return (uint8_t)OP_PROGRAM_PAGE_MULTI_PLANE;
            case TEST_OP_READ:
                return (uint8_t)OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT;
            case TEST_OP_ERASE:
                return (uint8_t)OP_ERASE_BLOCK_MULTI_PLANE;
            default:
                break;
        }
    }
    else
    {
        switch (operation)
        {
            case TEST_OP_PROGRAM:
                return (uint8_t)OP_NORMAL_PROGRAM_PAGE;
            case TEST_OP_READ:
                return (uint8_t)OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
            case TEST_OP_ERASE:
                return (uint8_t)OP_ERASE_BLOCK;
            default:
                break;
        }
    }

    /* 未知操作种类 = 调用者编程错误 */
    ASSERT(0);
    return (uint8_t)OP_NORMAL_PROGRAM_PAGE;
}

fc_command_t *test_page_cmd(uint16_t block_index, uint8_t page, uint8_t plane_bitmap, uint8_t multi_flag, test_page_op_t operation)
{
    /* meta 取页 LBA（page << CW_WIDTH）：写读同页自然一致，且页号进了 META，
     * 「读错页」会被数据路径 Pass 4 ② 抓到（同页不同 CW 的 LBA 只差 1，也各自可分）。 */
    return test_page_cmd_ex(block_index, page, plane_bitmap, multi_flag, test_resolve_operation(operation, multi_flag), 0U, (DWORD)page << CW_WIDTH);
}

fc_command_t *test_page_cmd_ex(uint16_t block_index, uint8_t page, uint8_t plane_bitmap, uint8_t multi_flag, uint8_t operation, uint32_t cw_mask, uint32_t meta)
{
    fc_command_t *cmd;
    uint8_t       plane;

    /* ── 契约校验（配置错误一律 ASSERT，不发到硬件）── */
    ASSERT(plane_bitmap != 0U);
    ASSERT((plane_bitmap & (uint8_t)~TEST_PLANE_BM_ALL) == 0U); /* 不含段外 plane */
    ASSERT((block_index < BLOCKS_PER_PLANE) && (page < PAGES_PER_BLOCK));

    if (multi_flag != 0U)
    {
        /* multi 操作必须覆盖全部 plane——否则未覆盖的 plane 会走
         * fc_build_request_data_planes 的同行回退地址被静默写入。 */
        ASSERT(plane_bitmap == TEST_PLANE_BM_ALL);
    }
    else
    {
        ASSERT(test_plane_count(plane_bitmap) == 1U);
    }

    plane = test_first_plane(plane_bitmap);

    cmd = fc_command_alloc();
    munit_assert_not_null(cmd);

    /* 默认值 = Level 3 的「裸页」形态：LDPC_RAW、不加扰、不 RAID。
     * crc 取 FC_CRC_ENABLE（= 池清零后的默认，与既有 program_page / read_page 一致）；
     * 需要 CRC_DISABLE 的用例（如 cache 路径）在建链后自行覆盖。 */
    cmd->config.operation   = (DWORD)operation;
    cmd->config.ch          = 0U;
    cmd->config.ce          = 0U;
    cmd->config.lun         = 0U;
    cmd->config.cw          = 0U;
    cmd->config.crc         = FC_CRC_ENABLE;
    cmd->config.random      = FC_RANDOM_DISABLE;
    cmd->config.ldpc        = FC_ECC_MODE_LDPC_RAW;
    cmd->config.raid        = FC_RAID_MODE_NORMAL;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    cmd->config.multi_plane = (multi_flag != 0U) ? (DWORD)PLANE_NUM : 0U;
    cmd->module             = FC_COMMAND_MODULE_HOST_IO;

    cmd->pba = pba_from_addr(&(nand_addr_t){.ch    = 0U,
                                            .ce    = 0U,
                                            .lun   = 0U,
                                            .block = block_index,
                                            .plane = plane,
                                            .page  = page});

    /* meta 是本命令的**扇区** LBA，原样使用（见头文件「meta 的语义」）。
     * test_page_cmd() 传 page << CW_WIDTH；本入口不派生——调用者要什么就是什么。 */
    cmd->meta = meta;

    cmd->param.cw_mask = cw_mask;

    munit_assert_int(ftl_cmd_configure_descriptors(cmd), ==, FTL_OK);

    return cmd;
}

/**************************************************************************
 *  提交 / 一步到位
 **************************************************************************/

fc_command_status_t test_submit_wait(fc_command_t *cmd)
{
    fc_command_status_t completion = {0};
    fc_command_id_t     expect_id  = FC_COMMAND_ID_INVALID;
    uint8_t             completed  = 0U;

    munit_assert_not_null(cmd);
    munit_assert_int(fc_submit(cmd), ==, FC_STATUS_OK);

    for (uint32_t i = 0U; i < TEST_SERVICE_LOOP_MAX; i++)
    {
        (void)fc_service();

        if (fc_poll_completion(&completion) == FC_STATUS_OK)
        {
            completed = 1U;
            break;
        }
    }

    munit_assert_uint32(completed, ==, 1U);

    /* cmd 无缓存 id（v15 起由指针派生）；completion 里的是建链时算好的快照。 */
    munit_assert_uint32(fc_command_to_id(cmd, &expect_id), ==, 1U);
    munit_assert_uint32(completion.command_id, ==, expect_id);

    return completion;
}

void test_run_lifecycle(fc_command_t *cmd)
{
    test_submit_wait(cmd);
    munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
}

fc_command_t *test_read_page(uint16_t block_index, uint8_t page, uint8_t plane_bitmap, uint8_t multi_flag)
{
    fc_command_t *cmd = test_page_cmd(block_index, page, plane_bitmap, multi_flag, TEST_OP_READ);

    /* 读前 USER 区预填 0xCC：NAND 没填满时残留可见，便于区分「读失败」与「读到真数据」。 */
    test_memset_read_buffer(cmd);
    test_submit_wait(cmd);

    return cmd;
}

void test_erase_block(uint16_t block_index, uint8_t plane_bitmap, uint8_t multi_flag)
{
    fc_command_t *cmd = test_page_cmd(block_index, 0U, plane_bitmap, multi_flag, TEST_OP_ERASE);

    test_submit_wait(cmd);
    munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
}

/**************************************************************************
 *  数据搬运 / 校验
 **************************************************************************/

void test_fill_user_from_source(fc_command_t *cmd)
{
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;

    while (desc_id != FC_DESCRIPTOR_ID_INVALID)
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        munit_assert_not_null(desc);

        uint8_t *buf = fc_buffer_id_to_real_buffer(desc->buffer_id);
        munit_assert_not_null(buf);

        (void)memcpy(buf,
                     test_source_slot((uint8_t)paa_to_plane(desc->paa), (uint8_t)paa_to_cw(desc->paa)),
                     FC_BUFFER_USER_SIZE);

        desc_id = (fc_descriptor_id_t)desc->next_id;
    }
}

void test_verify_against_source(fc_command_t *cmd)
{
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
    uint8_t            idx     = 0U;

    while (desc_id != FC_DESCRIPTOR_ID_INVALID)
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        munit_assert_not_null(desc);

        const uint8_t plane = (uint8_t)paa_to_plane(desc->paa);
        const uint8_t cw    = (uint8_t)paa_to_cw(desc->paa);
        uint8_t      *buf   = fc_buffer_id_to_real_buffer(desc->buffer_id);
        munit_assert_not_null(buf);

        const uint8_t *expected = test_source_slot(plane, cw);

        for (uint16_t i = 0U; i < FC_BUFFER_USER_SIZE; i++)
        {
            if (buf[i] != expected[i])
            {
                munit_errorf("chain[%u] plane=%u cw=%u offset=%u: got 0x%02X expected 0x%02X",
                             (unsigned)idx,
                             (unsigned)plane,
                             (unsigned)cw,
                             (unsigned)i,
                             (unsigned)buf[i],
                             (unsigned)expected[i]);
                return;
            }
        }

        desc_id = (fc_descriptor_id_t)desc->next_id;
        idx++;
    }
}

void test_verify_erased(fc_command_t *cmd)
{
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
    uint8_t            idx     = 0U;

    while (desc_id != FC_DESCRIPTOR_ID_INVALID)
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        munit_assert_not_null(desc);

        uint8_t *buf = fc_buffer_id_to_real_buffer(desc->buffer_id);
        munit_assert_not_null(buf);

        for (uint16_t i = 0U; i < FC_BUFFER_USER_SIZE; i++)
        {
            if (buf[i] != 0xFFU)
            {
                munit_errorf("chain[%u] plane=%u cw=%u offset=%u: got 0x%02X expected 0xFF (erased)",
                             (unsigned)idx,
                             (unsigned)paa_to_plane(desc->paa),
                             (unsigned)paa_to_cw(desc->paa),
                             (unsigned)i,
                             (unsigned)buf[i]);
                return;
            }
        }

        desc_id = (fc_descriptor_id_t)desc->next_id;
        idx++;
    }
}

void test_memset_read_buffer(fc_command_t *cmd)
{
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;

    while (desc_id != FC_DESCRIPTOR_ID_INVALID)
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        munit_assert_not_null(desc);

        uint8_t *buf = fc_buffer_id_to_real_buffer(desc->buffer_id);
        if (buf != NULL)
        {
            (void)memset(buf, 0xCC, FC_BUFFER_USER_SIZE);
        }

        desc_id = (fc_descriptor_id_t)desc->next_id;
    }
}

uint8_t test_verify_cw_status_all_success(fc_command_t *cmd)
{
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
    uint8_t            idx     = 0U;
    uint8_t            all_ok  = 1U;

    while (desc_id != FC_DESCRIPTOR_ID_INVALID)
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        munit_assert_not_null(desc);

        if ((desc->status & FC_CW_STATUS_SUCCESS) == 0U)
        {
            munit_errorf("chain[%u] plane=%u cw=%u status=0x%02X expected FC_CW_STATUS_SUCCESS",
                         (unsigned)idx,
                         (unsigned)paa_to_plane(desc->paa),
                         (unsigned)paa_to_cw(desc->paa),
                         (unsigned)desc->status);
            all_ok = 0U;
        }

        desc_id = (fc_descriptor_id_t)desc->next_id;
        idx++;
    }

    return all_ok;
}

/**************************************************************************
 *  desc 链小工具
 **************************************************************************/

fc_descriptor_t *test_cw_desc(const fc_command_t *cmd, uint8_t idx)
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

uint8_t *test_cw_user(const fc_command_t *cmd, uint8_t idx)
{
    fc_descriptor_t *desc = test_cw_desc(cmd, idx);

    munit_assert_not_null(desc);
    return fc_buffer_id_to_real_buffer(desc->buffer_id);
}

uint8_t *test_cw_base(const fc_command_t *cmd, uint8_t idx)
{
    fc_descriptor_t *desc = test_cw_desc(cmd, idx);

    munit_assert_not_null(desc);
    return fc_buffer_get_base(desc->buffer_id);
}

uint8_t test_cw_count(const fc_command_t *cmd)
{
    uint8_t count = 0U;

    while (test_cw_desc(cmd, count) != NULL)
    {
        count++;
    }
    return count;
}

uint8_t test_collect_chain(const fc_command_t *cmd, uint8_t *plane_out, uint8_t *cw_out)
{
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
    uint8_t            count   = 0U;

    while ((desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (count < CW_TOTAL_MAX))
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);

        munit_assert_not_null(desc);

        if (plane_out != NULL)
        {
            plane_out[count] = (uint8_t)paa_to_plane(desc->paa);
        }
        if (cw_out != NULL)
        {
            cw_out[count] = (uint8_t)paa_to_cw(desc->paa);
        }

        desc_id = (fc_descriptor_id_t)desc->next_id;
        count++;
    }

    return count;
}

uint32_t test_cw_seed(const fc_command_t *cmd, uint8_t idx)
{
    fc_descriptor_t *desc = test_cw_desc(cmd, idx);

    munit_assert_not_null(desc);
    return fc_random_seed_from_meta(desc->meta);
}

uint8_t test_check_pattern(const uint8_t *buffer, uint16_t length, uint8_t pattern)
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

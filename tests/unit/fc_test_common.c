/*
 * fc_test_common.c — FC-NAND 单元测试共享夹具（实现）
 *
 * 接口与契约见 fc_test_common.h。本文件只放「测试脚手架」，不含被测逻辑。
 */

#include "fc_test_common.h"

#include "fc/fc_random_engine.h"
#include "nand/nand_dispatch.h"

#include <stdlib.h>
#include <string.h>

/* test_fill_paa / test_check_paa 按 **dword** 步进（`FC_BUFFER_USER_SIZE / 4U` 次 ×
 * `sizeof(nand_paa_t)` 字节）：只有 PAA 正好 4 字节、且 USER 区是 4 的整数倍，
 * 「128 个 dword 填满 512B 槽」才成立。两者任一漂移都会**静默**写穿 / 漏填槽。 */
_Static_assert(sizeof(nand_paa_t) == sizeof(uint32_t), "nand_paa_t 非 4 字节：dword 步进填槽失效");
_Static_assert((FC_BUFFER_USER_SIZE % sizeof(uint32_t)) == 0U, "USER 区非 4 的整数倍");

uint8_t *g_source_data = NULL;

/* 步骤级用例的 CW 暂存区（见 fc_test_common.h「test_cw_scratch」）。
 * 与 g_source_data 同处分配 / 释放；**不放栈上**（4KB+ 会撑爆用例栈帧）。 */
static uint8_t *g_cw_scratch = NULL;

/**************************************************************************
 *  内部小工具
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

/* plane_bitmap → flat CW 位图：选中 plane 段内的全部 CW（plane1 单 plane ⇒ 0xF0）。
 * 刻意**不复用** fc_command_plane_span_cw_bitmap(cmd)：那份是从 cmd->pba 的 plane 位与
 * config.multi_plane **反推**段，这份是从调用者给的 plane_bitmap **正推**段。两条独立
 * 推导必须一致——不一致时 ftl_cmd_resolve_cw_bitmap 的越段断言当场炸，而不是静默访问
 * 错的 plane。（v27 起 cw_bitmap 是字面位图，0 = 空集，故这里必须显式算出来。） */
static uint32_t test_plane_cw_bitmap(uint8_t plane_bitmap)
{
    uint32_t mask = 0U;

    for (uint8_t p = 0U; p < PLANE_NUM; p++)
    {
        if ((plane_bitmap & TEST_PLANE_BM(p)) != 0U)
        {
            mask |= ((uint32_t)((1U << CW_NUM) - 1U)) << ((uint32_t)p * CW_NUM);
        }
    }
    return mask;
}

/* 该 op 是否是多 plane 变体——决定 config.multi_plane（= 命令 plane 段宽度）。
 * 全仓没有这份分类（FTL 是内联 `pages == 1 ? single : multi` 挑的），故此处自带一份。 */
static uint8_t test_is_multi_op(uint8_t operation)
{
    switch (operation)
    {
        case (uint8_t)OP_PROGRAM_PAGE_MULTI_PLANE:
        case (uint8_t)OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT:
        case (uint8_t)OP_ERASE_BLOCK_MULTI_PLANE:
            return 1U;
        default:
            return 0U;
    }
}

/* submit + fc_service 循环 + poll + 校验 command_id。**不 release**。
 * 5 个 helper 内部收尾用；不对外导出（用例的提交原语见头文件说明）。 */
static fc_command_status_t test_submit_wait(fc_command_t *cmd)
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

/* 读路径收尾：FC 已在 fc_complete_from_nand 里跑过 process_read（原地解码），
 * 此刻 desc 的 USER 区是明文——逐 desc 拷回 g_source_data 槽，供调用者校验。 */
static void test_capture_user_paa(const fc_command_t *cmd)
{
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
    uint8_t            idx     = 0U;

    while ((desc_id != FC_DESCRIPTOR_ID_INVALID) && (idx < CW_TOTAL_MAX))
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        munit_assert_not_null(desc);

        uint8_t *slot = test_source_slot((uint8_t)paa_to_plane(desc->paa), (uint8_t)paa_to_cw(desc->paa));
        uint8_t *user = fc_buffer_id_to_real_buffer(desc->buffer_id);
        munit_assert_not_null(user);

        (void)memcpy(slot, user, FC_BUFFER_USER_SIZE);

        desc_id = (fc_descriptor_id_t)desc->next_id;
        idx++;
    }
}

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

        /* 内容**不在这里填**：槽内容 = 各 CW 自己的 PAA，只有建链之后才知道。
         * 写路径（test_fill_user_paa）负责填。此处只清零，让「没写过」可辨认。 */
        (void)memset(g_source_data, 0x00, (size_t)FC_BUFFER_USER_SIZE * slots);
    }

    if (g_cw_scratch == NULL)
    {
        g_cw_scratch = (uint8_t *)malloc((size_t)CW_TOTAL_MAX * FC_BUFFER_POOL_CW_SIZE);
        munit_assert_not_null(g_cw_scratch);

        /* 同样不预填：步骤级用例自己决定写什么（未写处应可见残留） */
        (void)memset(g_cw_scratch, 0x00, (size_t)CW_TOTAL_MAX * FC_BUFFER_POOL_CW_SIZE);
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

    if (g_cw_scratch != NULL)
    {
        free(g_cw_scratch);
        g_cw_scratch = NULL;
    }
}

/* 接口与契约见 fc_test_common.h。 */
uint8_t *test_cw_scratch(uint8_t idx)
{
    ASSERT(g_cw_scratch != NULL);
    ASSERT(idx < CW_TOTAL_MAX);

    return &g_cw_scratch[(uint32_t)idx * FC_BUFFER_POOL_CW_SIZE];
}

/**************************************************************************
 *  只建链、不提交的原语
 **************************************************************************/

/* 接口与契约见 fc_test_common.h「只建链、不提交的原语」。 */
fc_command_t *test_cmd_build(const nand_addr_t *addr, uint32_t cw_bitmap, uint32_t meta, const fc_cmd_conf_t *config)
{
    fc_command_t *cmd;
    uint8_t       is_multi;

    ASSERT(addr != NULL);
    ASSERT(config != NULL);
    ASSERT(addr->block < BLOCKS_PER_PLANE);
    ASSERT(addr->plane < PLANE_NUM);
    ASSERT(addr->page < PAGES_PER_BLOCK);
    /* 链上每条 desc 的 CW 由 cw_bitmap 逐位铸出；pba 的 CW 位在建链时被
     * `cmd->pba & ~PLANE_CW_MASK` 掩掉（ftl_fc_interface.c:86）⇒ 地址里带 cw 只会
     * 让调用者以为它有效。断言 0 把这个误解挡在建链之前。 */
    ASSERT(addr->cw == 0U);

    is_multi = test_is_multi_op((uint8_t)config->operation);

    /* multi-plane 命令的位图必须**每个 plane 都被覆盖到**：未覆盖的 plane 会被
     * fc_build_request_data_planes 补一条同行回退地址，写路径静默给该 plane 编一页
     * 全 0xFF（见 fc_test_common.h「三」）。cw_bitmap == 0 是无链命令（ERASE 等），跳过。 */
    if ((is_multi != 0U) && (cw_bitmap != 0U))
    {
        for (uint8_t p = 0U; p < PLANE_NUM; p++)
        {
            ASSERT((cw_bitmap & (((uint32_t)((1U << CW_NUM) - 1U)) << ((uint32_t)p * CW_NUM))) != 0U);
        }
    }

    cmd = fc_command_alloc();
    munit_assert_not_null(cmd);

    /* 照 ftl_cmd_setup_ppa 的口径：地址信息既进 pba，也镜像进 config.ch/ce/lun。 */
    cmd->config             = *config;
    cmd->config.ch          = (DWORD)addr->ch;
    cmd->config.ce          = (DWORD)addr->ce;
    cmd->config.lun         = (DWORD)addr->lun;
    cmd->config.multi_plane = (is_multi != 0U) ? (DWORD)PLANE_NUM : 0U;
    cmd->module             = FC_COMMAND_MODULE_HOST_IO;
    cmd->pba                = pba_from_addr(addr);
    cmd->meta               = meta;

    /* 无数据 op 走到 ftl_cmd_needs_desc_chain 的短路分支（cw_bitmap 不看），
     * desc_id 保持 fc_command_alloc 置的 INVALID。 */
    munit_assert_int(ftl_cmd_configure_descriptors(cmd, cw_bitmap), ==, FTL_OK);

    return cmd;
}

/**************************************************************************
 *  5 个页 helper（建立在 test_cmd_build 之上）
 **************************************************************************/

fc_command_status_t test_erase_block(uint16_t block_index, uint8_t plane_bitmap, const fc_cmd_conf_t *config)
{
    fc_command_t       *cmd;
    fc_command_status_t completion;
    uint8_t             is_multi;

    ASSERT(config != NULL);
    ASSERT(plane_bitmap != 0U);
    ASSERT((plane_bitmap & (uint8_t)~TEST_PLANE_BM_ALL) == 0U); /* 不含段外 plane */
    ASSERT(block_index < BLOCKS_PER_PLANE);
    ASSERT((config->operation == (DWORD)OP_ERASE_BLOCK) || (config->operation == (DWORD)OP_ERASE_BLOCK_MULTI_PLANE));

    is_multi = test_is_multi_op((uint8_t)config->operation);
    if (is_multi != 0U)
    {
        /* multi-plane 擦除按定义就擦两个 plane（fc_build_dispatch_request 的
         * OP_ERASE_BLOCK_MULTI_PLANE 分支无条件填 plane_paa[0]/[1]），半掩码
         * 表达不出别的意思 ⇒ 自洽性检查。 */
        ASSERT(plane_bitmap == TEST_PLANE_BM_ALL);
    }
    else
    {
        ASSERT(test_plane_count(plane_bitmap) == 1U);
    }

    /* 擦除**无数据传输** ⇒ cw_bitmap = 0：test_cmd_build 走到 ftl_cmd_needs_desc_chain
     * 的短路分支，desc_id 保持 INVALID，不建链、不申请 buffer。meta 同理不参与
     * （无数据路径 ⇒ 无 encode / 无 Pass 4）。
     * 请求位图在 NAND 侧是**合法空集**（v28）：FC 的 ERASE 分支只填 plane_paa[]、
     * 不再伪造 CW0 位，completion_all_cws 的空集分支写 cw_status[0] 兜住成败。
     * multi_plane 由 test_cmd_build 按 operation 派生——它是「本命令跨几个 plane」的
     * 真实属性，填它不为任何读者，只为让命令的声明几何不撒谎。 */
    cmd = test_cmd_build(&(nand_addr_t){.block = block_index, .plane = test_first_plane(plane_bitmap)}, 0U, 0U, config);

    completion = test_submit_wait(cmd);

    munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
    return completion;
}

fc_command_status_t test_prog_page(uint16_t block_index, uint8_t plane_bitmap, uint16_t page_index, const fc_cmd_conf_t *config, uint8_t **data)
{
    fc_command_t       *cmd;
    fc_command_status_t completion;
    uint8_t             is_multi;

    ASSERT(config != NULL);
    ASSERT(plane_bitmap != 0U);
    ASSERT((plane_bitmap & (uint8_t)~TEST_PLANE_BM_ALL) == 0U);
    ASSERT((block_index < BLOCKS_PER_PLANE) && (page_index < PAGES_PER_BLOCK));
    ASSERT((config->operation == (DWORD)OP_NORMAL_PROGRAM_PAGE) || (config->operation == (DWORD)OP_PROGRAM_PAGE_MULTI_PLANE));

    is_multi = test_is_multi_op((uint8_t)config->operation);
    if (is_multi != 0U)
    {
        ASSERT(plane_bitmap == TEST_PLANE_BM_ALL);
    }
    else
    {
        ASSERT(test_plane_count(plane_bitmap) == 1U);
    }

    /* meta = 页 LBA：写读同页自然一致，且页号进了 META ⇒「读错页」被 Pass 4 ② 抓到。
     * cw_bitmap 由 plane_bitmap 正推（test_plane_cw_bitmap），与 test_cmd_build 内部
     * 按 cmd->pba/config.multi_plane 的段推导互为交叉检查。 */
    cmd = test_cmd_build(&(nand_addr_t){.block = block_index, .plane = test_first_plane(plane_bitmap), .page = page_index},
                         test_plane_cw_bitmap(plane_bitmap),
                         (DWORD)page_index << CW_WIDTH,
                         config);

    /* 明文 PAA 图案：填 g_source_data 槽，同时拷进各 desc 的 USER 区。
     * 提交后 FC 会原地编码，desc buffer 不再可读作明文 ⇒ `*data` 只能指向槽。 */
    test_fill_user_paa(cmd);

    completion = test_submit_wait(cmd);

    if (data != NULL)
    {
        *data = g_source_data;
    }

    munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
    return completion;
}

fc_command_status_t test_read_page(uint16_t block_index, uint8_t plane_bitmap, uint16_t page_index, const fc_cmd_conf_t *config, uint8_t **data)
{
    fc_command_t       *cmd;
    fc_command_status_t completion;
    uint8_t             is_multi;

    ASSERT(config != NULL);
    ASSERT(plane_bitmap != 0U);
    ASSERT((plane_bitmap & (uint8_t)~TEST_PLANE_BM_ALL) == 0U);
    ASSERT((block_index < BLOCKS_PER_PLANE) && (page_index < PAGES_PER_BLOCK));
    ASSERT((config->operation == (DWORD)OP_NORMAL_READ_PAGE_WITH_DATA_OUT) ||
           (config->operation == (DWORD)OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT));

    is_multi = test_is_multi_op((uint8_t)config->operation);
    if (is_multi != 0U)
    {
        ASSERT(plane_bitmap == TEST_PLANE_BM_ALL);
    }
    else
    {
        ASSERT(test_plane_count(plane_bitmap) == 1U);
    }

    /* meta / cw_bitmap 与 test_prog_page 同口径——写读同页必须一致，否则 Pass 4 ② 报错。 */
    cmd = test_cmd_build(&(nand_addr_t){.block = block_index, .plane = test_first_plane(plane_bitmap), .page = page_index},
                         test_plane_cw_bitmap(plane_bitmap),
                         (DWORD)page_index << CW_WIDTH,
                         config);

    /* 读前 USER 区预填 0xCC：NAND 没填满时残留可见，便于区分「读失败」与「读到真数据」。 */
    test_memset_read_buffer(cmd);

    completion = test_submit_wait(cmd);

    /* FC 已跑过 process_read（原地解码），此刻 desc 的 USER 区是明文。 */
    test_capture_user_paa(cmd);

    if (data != NULL)
    {
        *data = g_source_data;
    }

    munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
    return completion;
}

void test_prog_block(uint16_t block_index, uint8_t plane_bitmap, const fc_cmd_conf_t *config, uint8_t **data)
{
    /* 逐页写，页间自然轮询（页 helper 内部即 submit + service 轮询 + release）。 */
    for (uint16_t page = 0U; page < PAGES_PER_BLOCK; page++)
    {
        fc_command_status_t completion = test_prog_page(block_index, plane_bitmap, page, config, NULL);

        munit_assert_uint32(completion.status & FC_CW_STATUS_ERROR_MASK, ==, 0U);
    }

    if (data != NULL)
    {
        *data = g_source_data; /* 最后一页的明文 */
    }
}

void test_read_block(uint16_t block_index, uint8_t plane_bitmap, const fc_cmd_conf_t *config, uint8_t **data)
{
    /* 逐页读，页间自然轮询。
     * **必须在页内校验**：g_source_data 每页都被覆写，攒到最后只剩最后一页可比。 */
    for (uint16_t page = 0U; page < PAGES_PER_BLOCK; page++)
    {
        fc_command_status_t completion = test_read_page(block_index, plane_bitmap, page, config, data);

        munit_assert_uint32(completion.status & FC_CW_STATUS_ERROR_MASK, ==, 0U);
        test_verify_page_paa(g_source_data, block_index, page, plane_bitmap);
    }

    if (data != NULL)
    {
        *data = g_source_data; /* 最后一页的明文 */
    }
}

/**************************************************************************
 *  数据搬运 / 校验
 **************************************************************************/

void test_fill_paa(uint8_t *slot, nand_paa_t paa)
{
    ASSERT(slot != NULL);

    /* 用 memcpy 而非 uint32_t* 强转：避开对齐与严格别名问题（槽在 malloc 块内，
     * 但没必要依赖这一点）。 */
    for (uint32_t i = 0U; i < (FC_BUFFER_USER_SIZE / 4U); i++)
    {
        (void)memcpy(&slot[4U * i], &paa, sizeof(paa));
    }
}

uint8_t test_check_paa(const uint8_t *slot, nand_paa_t paa)
{
    uint32_t word;

    ASSERT(slot != NULL);

    for (uint32_t i = 0U; i < (FC_BUFFER_USER_SIZE / 4U); i++)
    {
        (void)memcpy(&word, &slot[4U * i], sizeof(word));

        if (word != paa)
        {
            return 0U;
        }
    }
    return 1U;
}

void test_fill_user_paa(fc_command_t *cmd)
{
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
    uint8_t            idx     = 0U;

    while ((desc_id != FC_DESCRIPTOR_ID_INVALID) && (idx < CW_TOTAL_MAX))
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        munit_assert_not_null(desc);

        uint8_t *slot = test_source_slot((uint8_t)paa_to_plane(desc->paa), (uint8_t)paa_to_cw(desc->paa));
        uint8_t *user = fc_buffer_id_to_real_buffer(desc->buffer_id);
        munit_assert_not_null(user);

        test_fill_paa(slot, desc->paa);
        (void)memcpy(user, slot, FC_BUFFER_USER_SIZE);

        desc_id = (fc_descriptor_id_t)desc->next_id;
        idx++;
    }
}

void test_memset_read_buffer(fc_command_t *cmd)
{
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
    uint8_t            idx     = 0U;

    while ((desc_id != FC_DESCRIPTOR_ID_INVALID) && (idx < CW_TOTAL_MAX))
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);
        munit_assert_not_null(desc);

        uint8_t *user = fc_buffer_id_to_real_buffer(desc->buffer_id);
        munit_assert_not_null(user);

        (void)memset(user, 0xCC, FC_BUFFER_USER_SIZE);

        desc_id = (fc_descriptor_id_t)desc->next_id;
        idx++;
    }
}

void test_verify_page_paa(const uint8_t *data, uint16_t block_index, uint16_t page_index, uint8_t plane_bitmap)
{
    munit_assert_not_null(data);
    ASSERT(plane_bitmap != 0U);
    ASSERT((plane_bitmap & (uint8_t)~TEST_PLANE_BM_ALL) == 0U);

    for (uint8_t plane = 0U; plane < PLANE_NUM; plane++)
    {
        if ((plane_bitmap & TEST_PLANE_BM(plane)) == 0U)
        {
            continue;
        }

        for (uint8_t cw = 0U; cw < CW_NUM; cw++)
        {
            const uint8_t *slot = &data[((uint32_t)plane * CW_NUM + (uint32_t)cw) * FC_BUFFER_USER_SIZE];
            nand_paa_t     expected =
                paa_from_addr(&(nand_addr_t){.block = block_index, .plane = plane, .page = page_index, .cw = cw});

            if (test_check_paa(slot, expected) == 0U)
            {
                munit_errorf("PAA mismatch: block=%u page=%u plane=%u cw=%u (expected paa=0x%08lX)",
                             (unsigned)block_index,
                             (unsigned)page_index,
                             (unsigned)plane,
                             (unsigned)cw,
                             (unsigned long)expected);
                return;
            }
        }
    }
}

void test_verify_erased(const fc_command_t *cmd)
{
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
    uint8_t            idx     = 0U;

    while ((desc_id != FC_DESCRIPTOR_ID_INVALID) && (idx < CW_TOTAL_MAX))
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

uint8_t test_verify_cw_status_all_success(const fc_command_t *cmd)
{
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
    uint8_t            idx     = 0U;
    uint8_t            all_ok  = 1U;

    while ((desc_id != FC_DESCRIPTOR_ID_INVALID) && (idx < CW_TOTAL_MAX))
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

/* 链遍历一律带 `CW_TOTAL_MAX` 上界：链是池内 id 串起来的，正常不超过 PLANE_NUM×CW_NUM，
 * 但**成环**（next_id 指回）时哨兵永远不出现——上界把挂死换成安静的截断。 */
fc_descriptor_t *test_cw_desc(const fc_command_t *cmd, uint8_t idx)
{
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;

    for (uint8_t i = 0U; (desc_id != FC_DESCRIPTOR_ID_INVALID) && (i < CW_TOTAL_MAX); i++)
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

    while ((desc_id != FC_DESCRIPTOR_ID_INVALID) && (count < CW_TOTAL_MAX))
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

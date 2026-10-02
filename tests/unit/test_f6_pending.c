/* F6 pending/completion 多实例化纯 RAM 单元测试
 *
 * 覆盖：
 *   - pending 表 per-instance：两个 ftl_context_t 各 add 同 cmd_id → 各自
 *     lookup 只命中自己；init 只清本实例
 *   - pending 满 32 → FTL_ERROR（per-instance 背压上限，互不挤占）
 *   - completion_table 实例路由：set_route 后按 (ch,ce,lun) 取到对应实例，
 *     越界返回 NULL
 *
 * 纯 RAM：只链接 pending.c + completion_table.c + munit（无 FC 桩——
 * 本测试不触 fc_submit / fc_poll_completion；completion_table_drain 不测）。
 */

#include "munit.h"

#include "ftl/completion_table.h"
#include "ftl/ftl.h"
#include "ftl/pending.h"
#include "nand/nand_config.h"

#include <string.h>

/**************************************************************************
 *  fixture：两个 ftl_context_t + main.c 同款三维 route 表
 **************************************************************************/

typedef struct
{
    ftl_context_t         a;
    ftl_context_t         b;
    ftl_context_t        *route[CH_NUM][CE_NUM][LUN_NUM];
} f6_fixture_t;

static void fixture_init(f6_fixture_t *fx)
{
    memset(fx, 0, sizeof(*fx));

    pending_init(&fx->a);
    pending_init(&fx->b);

    /* main.c 同款 route 表：a → (0,0,0)，b → (0,0,1)（若 LUN_NUM>1） */
    fx->route[0][0][0] = &fx->a;
#if LUN_NUM > 1
    fx->route[0][0][1] = &fx->b;
#endif
    completion_table_set_route(fx->route);
}

/**************************************************************************
 *  test_pending_instance_isolation_*
 **************************************************************************/

static MunitResult test_pending_isolation(const MunitParameter params[], void *data)
{
    (void)params;
    f6_fixture_t *fx = (f6_fixture_t *)data;
    ftl_io_request_t io_a;
    ftl_io_request_t io_b;

    memset(&io_a, 0, sizeof(io_a));
    memset(&io_b, 0, sizeof(io_b));
    io_a.lba = 0x10U;
    io_b.lba = 0x20U;

    /* 同一 cmd_id 分别 add 到两个实例 */
    munit_assert_int(pending_add(&fx->a, 0x1111U, &io_a,
                                 NAND_PBA_INVALID, NAND_PBA_INVALID,
                                 NAND_PBA_INVALID, 0U), ==, FTL_OK);
    munit_assert_int(pending_add(&fx->b, 0x1111U, &io_b,
                                 NAND_PBA_INVALID, NAND_PBA_INVALID,
                                 NAND_PBA_INVALID, 0U), ==, FTL_OK);

    /* 各自 lookup 只命中自己（cmd_id 相同也不串） */
    {
        ftl_pending_req_t *pa = pending_lookup(&fx->a, 0x1111U);
        ftl_pending_req_t *pb = pending_lookup(&fx->b, 0x1111U);
        munit_assert_not_null(pa);
        munit_assert_not_null(pb);
        munit_assert_int((int)pa->io.lba, ==, 0x10);
        munit_assert_int((int)pb->io.lba, ==, 0x20);
    }

    /* a 的 remove 只清 a，b 不受影响 */
    pending_remove(&fx->a, 0x1111U);
    munit_assert_null(pending_lookup(&fx->a, 0x1111U));
    munit_assert_not_null(pending_lookup(&fx->b, 0x1111U));

    return MUNIT_OK;
}

static MunitResult test_pending_init_only_own(const MunitParameter params[], void *data)
{
    (void)params;
    f6_fixture_t *fx = (f6_fixture_t *)data;
    ftl_io_request_t io;

    memset(&io, 0, sizeof(io));
    io.lba = 0x55U;
    munit_assert_int(pending_add(&fx->a, 0x2222U, &io,
                                 NAND_PBA_INVALID, NAND_PBA_INVALID,
                                 NAND_PBA_INVALID, 0U), ==, FTL_OK);

    /* 只 reinit b → a 的 pending 保留（F6 核心：第二个实例 init 不再冲掉第一个） */
    pending_init(&fx->b);
    munit_assert_not_null(pending_lookup(&fx->a, 0x2222U));
    munit_assert_null(pending_lookup(&fx->b, 0x2222U));

    return MUNIT_OK;
}

static MunitResult test_pending_full_backpressure(const MunitParameter params[], void *data)
{
    (void)params;
    f6_fixture_t *fx = (f6_fixture_t *)data;
    ftl_io_request_t io;
    uint8_t          i;

    memset(&io, 0, sizeof(io));

    /* 填满实例 a 的 32 槽 */
    for (i = 0U; i < FTL_PENDING_MAX; i++)
    {
        munit_assert_int(pending_add(&fx->a, (uint16_t)(0x3000U + i), &io,
                                     NAND_PBA_INVALID, NAND_PBA_INVALID,
                                     NAND_PBA_INVALID, 0U), ==, FTL_OK);
    }

    /* 第 33 个 → 表满（per-instance 背压上限） */
    munit_assert_int(pending_add(&fx->a, 0x3FFFU, &io,
                                 NAND_PBA_INVALID, NAND_PBA_INVALID,
                                 NAND_PBA_INVALID, 0U), ==, FTL_ERROR);

    /* 但 b 仍有独立 32 槽可用（互不挤占） */
    munit_assert_int(pending_add(&fx->b, 0x4000U, &io,
                                 NAND_PBA_INVALID, NAND_PBA_INVALID,
                                 NAND_PBA_INVALID, 0U), ==, FTL_OK);

    return MUNIT_OK;
}

/**************************************************************************
 *  test_completion_table_route_*
 **************************************************************************/

static MunitResult test_completion_route(const MunitParameter params[], void *data)
{
    (void)params;
    f6_fixture_t *fx = (f6_fixture_t *)data;

    munit_assert_ptr_equal(completion_table_route(0U, 0U, 0U), &fx->a);
#if LUN_NUM > 1
    munit_assert_ptr_equal(completion_table_route(0U, 0U, 1U), &fx->b);
#endif

    /* 越界 → NULL */
    munit_assert_null(completion_table_route((uint8_t)CH_NUM, 0U, 0U));
    munit_assert_null(completion_table_route(0U, (uint8_t)CE_NUM, 0U));
    munit_assert_null(completion_table_route(0U, 0U, (uint8_t)LUN_NUM));

    return MUNIT_OK;
}

/**************************************************************************
 *  套件定义 + host 入口
 **************************************************************************/

static void *f6_setup(const MunitParameter params[], void *user_data)
{
    (void)params;
    (void)user_data;
    f6_fixture_t *fx = (f6_fixture_t *)malloc(sizeof(*fx));
    if (fx == NULL)
    {
        return NULL;
    }
    fixture_init(fx);
    return fx;
}

static void f6_teardown(void *fixture)
{
    free(fixture);
}

MunitTest g_f6_pending_tests[] = {
    {"pending_isolation",            test_pending_isolation,            f6_setup, f6_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {"pending_init_only_own",        test_pending_init_only_own,        f6_setup, f6_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {"pending_full_backpressure",    test_pending_full_backpressure,    f6_setup, f6_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {"completion_route",             test_completion_route,             f6_setup, f6_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                                NULL,                     NULL,        NULL,        NULL, MUNIT_TEST_OPTION_NONE, NULL},
};

#if defined(MUNIT_HOST_STANDALONE)
static MunitSuite g_f6_suite =
    {
        (char *)"/f6/",
        g_f6_pending_tests,
        NULL,
        1,
        MUNIT_SUITE_OPTION_NONE,
    };

int main(int argc, char *argv[argc + 1])
{
    return munit_suite_main(&g_f6_suite, NULL, argc, argv);
}
#endif

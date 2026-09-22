/*
 * ut_main.c — 裸机统一测试入口
 *
 * 将 Level 1 / Level 2 / Level 3 所有子套件汇总到根套件，
 * 通过 munit_suite_main 入口执行。
 *
 * 供 UART CLI 调用：ut_run_all(argc, argv)
 */

#include "munit.h"

/* Level 1: 纯逻辑测试 */
extern MunitTest g_fc_command_tests[];
extern MunitTest g_fc_queue_tests[];
extern MunitTest g_fc_scheduler_tests[];

/* Level 2: 数据路径测试 */
extern MunitTest g_fc_data_path_tests[];

/* Level 3: 全链路测试 */
extern MunitTest g_fc_lifecycle_tests[];

/* Level 2: 数据路径测试 */
/* Level 3: 全链路测试 */

static MunitSuite g_fc_nand_suite =
{
    (char *)"/fc-nand/",
    NULL,
    (MunitSuite[]){
        /* Level 1 */
        { (char *)"command",   g_fc_command_tests,   NULL, 1, MUNIT_SUITE_OPTION_NONE },
        { (char *)"queue",     g_fc_queue_tests,     NULL, 1, MUNIT_SUITE_OPTION_NONE },
        { (char *)"scheduler", g_fc_scheduler_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },

        /* Level 2 */
        { (char *)"data_path", g_fc_data_path_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },

        /* Level 3 */
        { (char *)"lifecycle", g_fc_lifecycle_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE },

        /* Level 2 */
        /* TODO: data_path 子套件 */

        /* Level 3 */
        /* TODO: lifecycle 子套件 */

        { NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE },
    },
    1,
    MUNIT_SUITE_OPTION_NONE,
};

int ut_run_all(int argc, char *argv[])
{
    return munit_suite_main(&g_fc_nand_suite, NULL, argc, argv);
}
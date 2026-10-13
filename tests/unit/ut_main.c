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
extern MunitTest g_fc_logic_tests[];

/* Level 2: 数据路径测试 */
extern MunitTest g_fc_data_path_tests[];

/* Level 3: 全链路测试 */
extern MunitTest g_fc_lifecycle_tests[];

/* ⚠ 子套件前缀**必须以 '/' 结尾**：munit 只把前缀和用例名**首尾相接**
 * （munit.c:1285 下探子套件、:1103 拼用例名），**不补分隔符**。根前缀已带尾斜杠；
 * 子套件漏了就会打印成 "/fc-nand/lifecycleblock_multi_plane"，且过滤串
 * "/fc-nand/lifecycle/…" 一条都匹配不上（用例名不带前导斜杠）。 */
static MunitSuite g_fc_nand_suite =
    {
        (char *)"/fc-nand/",
        NULL,
        (MunitSuite[]){
                       /* Level 1 */
                       {(char *)"logic/", g_fc_logic_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE},

                       /* Level 2 */
                       {(char *)"data_path/", g_fc_data_path_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE},

                       /* Level 3 */
                       {(char *)"lifecycle/", g_fc_lifecycle_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE},

                       {NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE},
                       },
        1,
        MUNIT_SUITE_OPTION_NONE,
};

int ut_run_all(int argc, char *argv[])
{
    return munit_suite_main(&g_fc_nand_suite, NULL, argc, argv);
}
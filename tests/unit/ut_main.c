/*
 * ut_main.c — 裸机统一测试入口
 *
 * 汇总 FC-NAND 与 FTL 两个根套件的全部子套件，通过 munit_suite_main 执行。
 *
 * 供 UART CLI 调用：ut_run_all(argc, argv)
 *
 * 编译范围由 `make tests UT=…` 选择（见顶层 makefile §1.5）：
 *   UT=（空）  两个根都在（-DUT_FC_ENABLE -DUT_FTL_ENABLE）
 *   UT=FC      只留 /fc-nand/（-DUT_FC_ENABLE）
 *   UT=FTL     只留 /ftl/（-DUT_FTL_ENABLE）
 * 未编入的根套件其 extern 声明与套件项一并被预处理掉 —— 否则会引用未链接的
 * 套件数组（`g_ftl_*_tests` 等）而链接失败。本文件是两层测试的**唯一耦合点**。
 */

#include "munit.h"

#if defined(UT_FC_ENABLE)

/* ── FC-NAND（HIL/FC 层，src/controller + src/fc） ── */

/* Level 1: 纯逻辑测试 */
extern MunitTest g_fc_logic_tests[];

/* Level 2: 数据路径测试 */
extern MunitTest g_fc_data_path_tests[];

/* Level 3: 全链路测试 */
extern MunitTest g_fc_lifecycle_tests[];

#endif /* UT_FC_ENABLE */

#if defined(UT_FTL_ENABLE)

/* ── FTL（src/ftl，纯 RAM L0 测试；见 docs/design/ftl_ut_munit.md） ── */

extern MunitTest g_ftl_allocator_tests[];
extern MunitTest g_ftl_watermark_tests[];
extern MunitTest g_ftl_gc_wl_tests[];
extern MunitTest g_ftl_rr_tests[];
extern MunitTest g_ftl_bbt_tests[];
extern MunitTest g_ftl_metadata_tests[];
extern MunitTest g_ftl_pending_ct_tests[];
extern MunitTest g_ftl_group_tests[];

#endif /* UT_FTL_ENABLE */

/* ⚠ 子套件前缀**必须以 '/' 结尾**：munit 只把前缀和用例名**首尾相接**
 * （munit.c:1285 下探子套件、:1103 拼用例名），**不补分隔符**。根前缀已带尾斜杠；
 * 子套件漏了就会打印成 "/fc-nand/lifecycleblock_multi_plane"，且过滤串
 * "/fc-nand/lifecycle/…" 一条都匹配不上（用例名不带前导斜杠）。 */
static MunitSuite g_ut_root_suite =
    {
        (char *)"",
        NULL,
        (MunitSuite[]){
#if defined(UT_FC_ENABLE)
                       /* ══ FC-NAND 根 ══ */
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
                       },
#endif /* UT_FC_ENABLE */

#if defined(UT_FTL_ENABLE)
                       /* ══ FTL 根 ══ */
                       {
                       (char *)"/ftl/",
                       NULL,
                       (MunitSuite[]){
                       {(char *)"allocator/", g_ftl_allocator_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE},
                       {(char *)"watermark/", g_ftl_watermark_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE},
                       {(char *)"gc_wl/", g_ftl_gc_wl_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE},
                       {(char *)"rr/", g_ftl_rr_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE},
                       {(char *)"bbt/", g_ftl_bbt_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE},
                       {(char *)"metadata/", g_ftl_metadata_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE},
                       {(char *)"pending_ct/", g_ftl_pending_ct_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE},
                       {(char *)"group/", g_ftl_group_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE},
                       {NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE},
                       },
                       1,
                       MUNIT_SUITE_OPTION_NONE,
                       },
#endif /* UT_FTL_ENABLE */

                       {NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE},
                       },
        1,
        MUNIT_SUITE_OPTION_NONE,
};

int ut_run_all(int argc, char *argv[])
{
    return munit_suite_main(&g_ut_root_suite, NULL, argc, argv);
}

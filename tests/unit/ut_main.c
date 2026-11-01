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

/* ── FTL（src/ftl；见 docs/design/ftl_ut_design.md） ── */

extern MunitTest g_ftl_pure_mem_tests[];
extern MunitTest g_ftl_pure_md_tests[];
extern MunitTest g_ftl_pure_host_tests[];
extern MunitTest g_ftl_pure_io_tests[];

/* test_ftl_media.c：每次 `test` 命令开始时复位「介质已格式化」状态（RISK-UT-08） */
extern void      ftl_ut_media_session_reset(void);
extern MunitTest g_ftl_media_e2e_tests[];

#endif /* UT_FTL_ENABLE */

#if defined(UT_FC_ENABLE) || defined(UT_FTL_ENABLE)
/* 速度测试（test_speed.c）：/speed/fc_nand_rw 与 /speed/ftl_rw 由 UT 范围决定。 */
extern MunitTest g_speed_tests[];
#endif

/* ⚠ 子套件前缀**必须以 '/' 结尾**：munit 只把前缀和用例名**首尾相接**
 * （munit.c:1285 下探子套件、:1103 拼用例名），**不补分隔符**。根前缀已带尾斜杠；
 * 子套件漏了就会打印成 "/fc-nand/lifecycleblock_multi_plane"，且过滤串
 * "/fc-nand/lifecycle/…" 一条都匹配不上（用例名不带前导斜杠）。 */
static MunitSuite g_ut_root_suite = {
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
                   {
                   (char *)"pure/",
                   NULL,
                   (MunitSuite[]){
                   {(char *)"mem/", g_ftl_pure_mem_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE},
                   {(char *)"md/", g_ftl_pure_md_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE},
                   {(char *)"host/", g_ftl_pure_host_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE},
                   {(char *)"io/", g_ftl_pure_io_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE},
                   {NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE},
                   },
                   1,
                   MUNIT_SUITE_OPTION_NONE,
                   },
                   {
                   (char *)"media/",
                   NULL,
                   (MunitSuite[]){
                   {(char *)"e2e/", g_ftl_media_e2e_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE},
                   {NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE},
                   },
                   1,
                   MUNIT_SUITE_OPTION_NONE,
                   },
                   {NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE},
                   },
                   1,
                   MUNIT_SUITE_OPTION_NONE,
                   },
#endif /* UT_FTL_ENABLE */

#if defined(UT_FC_ENABLE) || defined(UT_FTL_ENABLE)
                   /* 板端速度测试（破坏性；两条用例按 UT 范围出现） */
                   {(char *)"/speed/", g_speed_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE},
#endif
                   {NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE},
                   },
    1,
    MUNIT_SUITE_OPTION_NONE,
};

/* ── 套件路径展开 ─────────────────────────────────────────────────────────────
 * munit 的过滤是「全路径前缀」匹配（munit.c:1297-1303）：先要求套件累积前缀 pre 是过滤串
 * 的前缀，再要求过滤串余部是**用例名**的前缀。用例挂在**叶子**套件下，其 pre 比用户输入的
 * 套件路径长 ⇒ 第一条 strncmp 在路径末尾即失配。⇒ `test /ftl/` 恒打印 "NO test run"，
 * 只有 `test /ftl/allocator/`（叶子套件全路径）或 `test /ftl/allocator/<用例名>` 能命中。
 *
 * 这里把「以 '/' 结尾」的实参展开为其下**全部叶子套件路径**，让 `test /ftl/`、
 * `test /fc-nand/` 按直觉工作。展开幂等（叶子路径展开成自身），命中不到任何叶子的串原样
 * 透传（行为与展开前一致）。
 * ────────────────────────────────────────────────────────────────────────── */
#define UT_PATH_MAX   64U
#define UT_PATH_COUNT 32U
#define UT_ARGV_MAX   48U

static char    g_ut_leaf_paths[UT_PATH_COUNT][UT_PATH_MAX];
static uint8_t g_ut_leaf_count;
static char   *g_ut_argv[UT_ARGV_MAX];

/* 深度优先收集「持有用例的套件」的全路径（拼接方式与 munit 一致：前缀首尾相接）。 */
static void ut_collect_leaf_paths(const MunitSuite *suite, const char *prefix)
{
    char              path[UT_PATH_MAX];
    const MunitSuite *child;
    size_t            base = strlen(prefix);
    size_t            len  = base + strlen(suite->prefix);

    if (len >= UT_PATH_MAX)
    {
        return; /* 套件树是静态的，实际不会发生 */
    }
    memcpy(path, prefix, base);
    memcpy(path + base, suite->prefix, len - base + 1U); /* 含结尾 '\0' */

    if ((suite->tests != NULL) && (g_ut_leaf_count < UT_PATH_COUNT))
    {
        memcpy(g_ut_leaf_paths[g_ut_leaf_count], path, len + 1U);
        g_ut_leaf_count++;
    }

    for (child = suite->suites; (child != NULL) && (child->prefix != NULL); child++)
    {
        ut_collect_leaf_paths(child, path);
    }
}

int ut_run_all(int argc, char *argv[])
{
    uint8_t n = 0U;
    int     i;

#if defined(UT_FTL_ENABLE)
    /* 介质状态只在本条 `test` 命令内有效：FC 原始块测试 / 上一条命令都会改变它，
     * 媒体套件必须重新格式化一次（RISK-UT-08）。 */
    ftl_ut_media_session_reset();
#endif

    g_ut_leaf_count = 0U;
    ut_collect_leaf_paths(&g_ut_root_suite, "");

    if (argc > 0)
    {
        g_ut_argv[n++] = argv[0];
    }

    for (i = 1; i < argc; i++)
    {
        size_t  len      = strlen(argv[i]);
        uint8_t expanded = 0U;
        uint8_t k;

        /* 以 '/' 结尾 ⇒ 视作套件路径，展开为其下所有叶子套件路径 */
        if ((len > 0U) && (argv[i][len - 1U] == '/'))
        {
            for (k = 0U; k < g_ut_leaf_count; k++)
            {
                if ((n < (UT_ARGV_MAX - 1U)) && (strncmp(g_ut_leaf_paths[k], argv[i], len) == 0))
                {
                    g_ut_argv[n++] = g_ut_leaf_paths[k];
                    expanded       = 1U;
                }
            }
        }

        if ((expanded == 0U) && (n < (UT_ARGV_MAX - 1U)))
        {
            g_ut_argv[n++] = argv[i];
        }
    }
    g_ut_argv[n] = NULL;

    return munit_suite_main(&g_ut_root_suite, NULL, (int)n, g_ut_argv);
}

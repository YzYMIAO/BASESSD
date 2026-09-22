#ifndef UT_MAIN_H
#define UT_MAIN_H

/*
 * ut_main.h — 裸机测试入口声明
 *
 * 在 UART CLI 中注册 test 命令后，通过 ut_run_all(argc, argv) 调用 munit 测试套件。
 * argc 中包含 test 命令自身（argv[0]），剩余参数传给 munit_suite_main 做过滤。
 */

int ut_run_all(int argc, char *argv[]);

#endif /* UT_MAIN_H */
/* munit host 测试的 FC 桩声明（供测试 TU 访问失败注入开关） */
#ifndef MUNIT_STUB_FC_H
#define MUNIT_STUB_FC_H

#ifdef __cplusplus
extern "C" {
#endif

/* 置 1 后下一次 fc_submit 返回 IO_ERROR（模拟 NAND 写失败）；
 * 由 munit_stub_fc.c 定义，host 测试仅在需要时使用。 */
extern int fc_stub_fail_next;

#ifdef __cplusplus
}
#endif

#endif /* MUNIT_STUB_FC_H */

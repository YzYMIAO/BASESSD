/* 上板 F4 测试的失败注入开关（host 独立构建由 munit_stub_fc.c 提供同名符号；
 * 上板固件无 host 桩，这里定义供 test_f4_region_bitmap 链接）。
 *
 * 上板环境不注入 NAND 失败（真实硬件异常由 NAND FAIL 路径覆盖），
 * 本符号恒 0，仅满足链接。 */
int fc_stub_fail_next = 0;

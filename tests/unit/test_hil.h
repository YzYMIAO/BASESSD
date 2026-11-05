#ifndef TEST_HIL_H
#define TEST_HIL_H

#include "munit.h"

/*
 * HIL 板端 UT（真 NAND/FTL，无 mock）。
 *
 * 套件路径：/hil/。破坏性用例会 format + 写 NAND；运行前拔掉 USB。
 * 覆盖 HIL 可被固件直接驱动的部分：controller 状态/初始化/范围校验、
 * 经 controller 的真实读写/flush/trim。
 *
 * ⚠ 单 die 当前无法构造多 run 拆分 / 跨实例部分失败；USB 槽状态机由 PC
 *   主机侧矩阵覆盖（见 docs/design/known_risks.md RISK-USB-09..13）。
 */

extern MunitTest g_hil_tests[];

#endif /* TEST_HIL_H */

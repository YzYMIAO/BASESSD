#ifndef NAND_INTERFACE_H
#define NAND_INTERFACE_H

/*
 * nand_interface.h — FC 面向 NAND 的唯一公共入口
 *
 * 只聚合 FC 需要的公共契约：
 *   - 几何/地址布局（nand_config.h）
 *   - UOP / 请求完成 / 结果码（nand_protocol.h）
 *   - 器件 backend 入口（nand_device.h）
 *   - bank ownership（nand_board.h）
 *   - ONFI 命令/时序常量（onfi_1_0.h）
 *
 * 私有实现头在 src/nand/（FMC、target PHY 初始化），FC 不得 include。
 * FTL 仍可单独 include nand_config.h / onfi_1_0.h 取几何/常量。
 */

#include "nand_board.h"
#include "nand_config.h"
#include "nand_device.h"
#include "nand_protocol.h"
#include "onfi_1_0.h"

#endif /* NAND_INTERFACE_H */

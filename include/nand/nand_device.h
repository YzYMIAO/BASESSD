#ifndef NAND_DEVICE_H
#define NAND_DEVICE_H

#include "nand_config.h"
#include "nand_protocol.h"

#include <stdint.h>

/*
 * nand_device.h — NAND 器件/PHY 层对 FC 暴露的接口
 *
 * 本头只含裸类型，不依赖 HAL、不依赖 FC 头。FC 通过这里的
 * nand_device_desc_s 取得器件事实，通过 nand_select_target 选择物理通路；
 * NAND 不反向 include FC。
 *
 * 器件事实（几何/时序/ID/参数页）由 devices/<family>.c 导出；FMC 总线后端
 * 在 nand_fmc.c 实现（见 nand_fmc.h，FC 不得 include）。
 */

/*
 * 板级 target 选择：把后续 NAND 命令发往 (ch,ce)。
 *
 * 当前板只有 FMC Bank3 + 单 NCE，(0,0) 有物理通路；其他坐标返回
 * NAND_RESULT_NOT_READY。多 target 板卡在此切换 FMC bank / CE# GPIO。
 */
nand_result_e nand_select_target(BYTE ch, BYTE ce);

/*
 * 器件描述符：FC 初始化/时序切换所需的器件事实与硬件生命周期入口。
 *
 * hw_init     —— HAL/FMC/GPIO 初始化 + FMC 后端实例配置（只在启动时调用）；
 * set_timing  —— SET_FEATURES 成功后按 mode/fmc_hz 重配 FMC 时序与后端；
 */
typedef struct NAND_DEVICE_DESCRIPTOR_S
{
    const char *name;
    DWORD       expected_id; /* READ ID 的 id[1..4]（大端拼成 32 位） */

    nand_result_e (*hw_init)(void);
    nand_result_e (*set_timing)(BYTE ch, BYTE ce, BYTE mode, DWORD fmc_hz); /* 按 target 切 ONFI mode / FMC 时序 */
} nand_device_desc_s;

/* 当前选中的器件（由 NAND_DEVICE 编译期选择）。 */
const nand_device_desc_s *nand_device_get(void);

/* NAND PHY 后端（FMC handler 表 + context），供 FC 的 UOP 引擎绑定。 */
const nand_phy_backend_s *nand_phy_backend_get(BYTE ch, BYTE ce);

#endif /* NAND_DEVICE_H */

#ifndef NAND_DEVICE_H
#define NAND_DEVICE_H

#include "nand_config.h"
#include "nand_protocol.h"

#include <stdint.h>

/*
 * nand_device.h — NAND 器件/PHY 层对 FC 暴露的接口
 *
 * 本头只含裸类型，不依赖 HAL、不依赖 FC 头。FC 通过这里的
 * nand_device_desc_s 取得器件事实，通过 nand_target_get 一次取得「通路 + backend」；
 * NAND 不反向 include FC。
 *
 * 器件事实（几何/时序/ID/参数页）由 devices/<family>.c 导出；FMC 总线后端
 * 在 nand_fmc.c 实现（见 nand_fmc.h，FC 不得 include）。
 */

/*
 * 板级 target 获取：校验 (ch,ce) 有物理通路，并把该 target 的 PHY backend 交给 FC。
 *
 * 这是 FC 取得 NAND 执行入口的**唯一入口**——通路选择与 backend 获取必须成对发生。
 * 拆成两个函数会让每个调用点手工维护配对，多 target 下漏一次就会把命令打到当前
 * bank（静默写错 target）。当前板只有 FMC Bank3 + 单 NCE，只有 (0,0) 有通路。
 *
 * @param ch      通道号。
 * @param ce      片选号。
 * @param backend 出参：通路存在时写入该 target 的 PHY backend（恒非空）。
 *
 * @return NAND_RESULT_OK               通路存在，backend 已写入。
 *         NAND_RESULT_INVALID_ARGUMENT 坐标越界。
 *         NAND_RESULT_NOT_READY        板级无此物理通路。
 */
nand_result_e nand_target_get(BYTE ch, BYTE ce, const nand_phy_backend_s **backend);

/* 器件生命周期/查询函数原型（H50：函数指针统一 _fn_t）。 */
typedef nand_result_e (*nand_hw_init_fn_t)(void);
typedef nand_result_e (*nand_set_timing_fn_t)(BYTE ch, BYTE ce, BYTE mode, DWORD fmc_hz);
typedef nand_result_e (*nand_timing_mode_fn_t)(BYTE ch, BYTE ce, BYTE *mode);

/*
 * 器件描述符：FC 初始化/时序切换所需的器件事实与硬件生命周期入口。
 *
 * hw_init         —— HAL/FMC/GPIO 初始化 + FMC 后端实例配置（只在启动时调用）；
 * set_timing      —— 按 mode/fmc_hz 重配 FMC 时序与后端（mode 0 = 低速）；
 * get_timing_mode —— 指定 target 当前生效的 ONFI timing mode（status + 出参）。
 */
typedef struct NAND_DEVICE_DESCRIPTOR_S
{
    const char *name;
    DWORD       expected_id; /* READ ID 的 id[1..4]（大端拼成 32 位） */

    nand_hw_init_fn_t     hw_init;
    nand_set_timing_fn_t  set_timing;
    nand_timing_mode_fn_t get_timing_mode;
} nand_device_desc_s;

/* 当前选中的器件（由 NAND_DEVICE 编译期选择）。 */
const nand_device_desc_s *nand_device_get(void);

/**
 * @brief 通用器件查询：指定 target 当前生效的 ONFI timing mode
 *
 * @param ch   通道号
 * @param ce   片选号
 * @param mode 输出：timing mode
 * @return OK / INVALID_ARGUMENT / NOT_READY
 */
nand_result_e nand_get_timing_mode(BYTE ch, BYTE ce, BYTE *mode);

#endif /* NAND_DEVICE_H */

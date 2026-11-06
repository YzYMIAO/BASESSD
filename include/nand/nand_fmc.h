#ifndef PLATFORM_STM32H743_NAND_FMC_H
#define PLATFORM_STM32H743_NAND_FMC_H

#include "nand_protocol.h"
#include "sys_port.h"

/*
 * nand_fmc.h — STM32H743 FMC NAND 后端接口
 *
 * 依赖：nand_protocol.h（backend/handler 类型）、nand_config.h（CH_NUM 等拓扑宏）。
 *
 * 本文件为 FC 的 UOP 引擎提供 FMC 后端：将 UOP 微指令映射为 FMC NAND
 * Bank3 内存窗口访问（命令/地址/数据窗口别名，见器件头的 NAND_FMC_* 宏）。
 */

/*
 * R/B# 接线（板级）：FMC NAND Bank3 的 ready/busy 接 GPIOD.6。
 *
 * 放在平台侧而不是器件头：① 它是本板接线，不是芯片属性；② GPIOD / GPIO_PIN_6
 * 是 HAL 类型，器件头必须保持平台纯净（它经 nand_config.h 进入 FTL include 闭包）。
 */
#define NAND_RB_GPIO_PORT GPIOD
#define NAND_RB_GPIO_PIN  GPIO_PIN_6

/*
 * FMC 后端配置：
 *   data/command/address 是 volatile 指针，指向 FMC NAND Bank3 内存
 *   窗口的三个别名地址（base / base|A16 / base|A17），写入即产生
 *   对应的命令/地址/数据周期。
 */
typedef struct NAND_FMC_CONFIG_S
{
    volatile BYTE *data;
    volatile BYTE *command;
    volatile BYTE *address;
    GPIO_TypeDef  *rb_port; /* 本 target 的 R/B# GPIO 端口 */
    WORD           rb_pin;  /* 本 target 的 R/B# 引脚 */
} nand_fmc_cfg_s;

/* ── 后端访问器（同步路径 + dispatch 按 (ch,ce) 取） ── */
/* ── FMC 后端实例 ──
 * 单 bank：数据/命令/地址窗口对同 bank 各 LUN 一致；FMC PCC 时序寄存器是
 * bank 级。故无需按 (CH,CE,LUN) 分化，一个实例即够。 */
typedef struct NAND_FMC_INSTANCE_S
{
    nand_fmc_cfg_s     config;
    nand_phy_backend_s backend;
} nand_fmc_instance_s;

/* ── 后端初始化：按 (ch,ce) 绑定一个 FMC 实例（当前板只初始化 (0,0)） ── */
nand_result_e nand_fmc_init_target(BYTE ch, BYTE ce, const nand_fmc_cfg_s *config);
/* 兼容入口：等价于 nand_fmc_init_target(0,0,...)。 */
nand_result_e nand_fmc_init(const nand_fmc_cfg_s *config);

#endif /* PLATFORM_STM32H743_NAND_FMC_H */

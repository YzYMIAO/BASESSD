#ifndef PLATFORM_STM32H743_NAND_FMC_H
#define PLATFORM_STM32H743_NAND_FMC_H

#include "nand_engine.h"
#include "sys_port.h"

/*
 * nand_fmc.h — STM32H743 FMC NAND 后端接口
 *
 * 依赖：nand_engine.h（backend 类型）、nand_config.h（CH_NUM 等拓扑宏，
 * 经 nand_engine.h 间接包含）。
 *
 * 本文件为 nand_engine 提供 FMC 后端：将 UOP 微指令映射为 FMC NAND
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
 * FMC NAND 时序参数（29 个 WORD，单位 ns）。
 *
 * ⚠ 顺序耦合：字段顺序必须与 src/nand/devices/mt29f___g08a.c 中两张时序表的
 * 29 个初始化值一一对应（slow / fast），错一位全错。
 * 数值来源：MT29F4G08ABADAWP 数据手册时序参数表。
 */
typedef struct
{
    WORD t_adl_ns;     /* 地址→数据加载延时 */
    WORD t_alh_ns;     /* ALE 保持 */
    WORD t_als_ns;     /* ALE 建立 */
    WORD t_ch_ns;      /* CE 保持 */
    WORD t_clh_ns;     /* CLE 保持 */
    WORD t_cls_ns;     /* CLE 建立 */
    WORD t_cs_ns;      /* CE 建立 */
    WORD t_dh_ns;      /* 数据保持 */
    WORD t_ds_ns;      /* 数据建立 */
    WORD t_wc_ns;      /* 写周期 */
    WORD t_wh_ns;      /* WE 高电平宽度 */
    WORD t_wp_ns;      /* WE 低电平宽度 */
    WORD t_ar_ns;      /* ALE→RE 延时 */
    WORD t_cea_max_ns; /* CE→输出有效（max） */
    WORD t_chz_max_ns; /* CE→输出高阻（max） */
    WORD t_clr_ns;     /* CLE→RE 延时 */
    WORD t_coh_ns;     /* CE 高→输出保持 */
    WORD t_ir_ns;      /* 命令→数据输出间隔 */
    WORD t_rc_ns;      /* 读周期 */
    WORD t_rea_max_ns; /* RE→输出有效（max） */
    WORD t_reh_ns;     /* RE 高电平宽度 */
    WORD t_rhoh_ns;    /* RE 高→输出保持 */
    WORD t_rhw_ns;     /* RE→WE 延时 */
    WORD t_rhz_max_ns; /* RE→输出高阻（max） */
    WORD t_rloh_ns;    /* RE 低→输出保持 */
    WORD t_rp_ns;      /* RE 低电平宽度 */
    WORD t_rr_ns;      /* 读命令→读命令间隔 */
    WORD t_wb_max_ns;  /* WE 高→busy（max） */
    WORD t_whr_ns;     /* 状态→RE 高延时 */
} nand_fmc_timing_t;

/*
 * FMC 后端配置：
 *   data/command/address 是 volatile 指针，指向 FMC NAND Bank3 内存
 *   窗口的三个别名地址（base / base|A16 / base|A17），写入即产生
 *   对应的命令/地址/数据周期。
 *   timing 内嵌 29 字段。
 */
typedef struct
{
    volatile BYTE    *data;
    volatile BYTE    *command;
    volatile BYTE    *address;
    nand_fmc_timing_t timing;
} nand_fmc_config_t;

/* ── 后端访问器（同步路径 + 多 LUN 共享，单 bank 下与坐标无关） ── */
const nand_engine_backend_t *nand_fmc_backend_get(void);

/* ── FMC 后端实例 ──
 * 单 bank：数据/命令/地址窗口与时序对全部 LUN 一致（FMC 时序寄存器是
 * bank 级，同 bank 各 LUN 共享窗口），故无需按 (CH,CE,LUN) 分化，一个即够。 */
typedef struct
{
    nand_fmc_config_t     config;
    nand_engine_backend_t backend;
} nand_fmc_instance_t;

/* ── 后端初始化：填唯一实例的 config + handler 表（nand_init 调用） ── */
nand_result_e nand_fmc_init(const nand_fmc_config_t *config);

#endif /* PLATFORM_STM32H743_NAND_FMC_H */

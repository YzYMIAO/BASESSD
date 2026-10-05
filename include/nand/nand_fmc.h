#ifndef PLATFORM_STM32H743_NAND_FMC_H
#define PLATFORM_STM32H743_NAND_FMC_H

#include "nand_engine.h"

/*
 * nand_fmc.h — STM32H743 FMC NAND 后端接口
 *
 * 依赖：nand_engine.h（backend 类型）、nand_config.h（CH_NUM 等拓扑宏，
 * 经 nand_engine.h 间接包含）。
 *
 * 本文件为 nand_engine 提供 FMC 后端：将 UOP 微指令映射为 FMC NAND
 * Bank3 内存窗口访问（命令/地址/数据窗口别名，见 nand_mt29f4g08abadawp.h）。
 */

/*
 * FMC NAND 时序参数（29 个 uint16_t，单位 ns）。
 *
 * ⚠ 顺序耦合：字段顺序必须与 nand_mt29f4g08abadawp.c 中两张时序表的
 * 29 个初始化值一一对应（slow / fast），错一位全错。
 * 数值来源：MT29F4G08ABADAWP 数据手册时序参数表。
 */
typedef struct
{
    uint16_t t_adl_ns;     /* 地址→数据加载延时 */
    uint16_t t_alh_ns;     /* ALE 保持 */
    uint16_t t_als_ns;     /* ALE 建立 */
    uint16_t t_ch_ns;      /* CE 保持 */
    uint16_t t_clh_ns;     /* CLE 保持 */
    uint16_t t_cls_ns;     /* CLE 建立 */
    uint16_t t_cs_ns;      /* CE 建立 */
    uint16_t t_dh_ns;      /* 数据保持 */
    uint16_t t_ds_ns;      /* 数据建立 */
    uint16_t t_wc_ns;      /* 写周期 */
    uint16_t t_wh_ns;      /* WE 高电平宽度 */
    uint16_t t_wp_ns;      /* WE 低电平宽度 */
    uint16_t t_ar_ns;      /* ALE→RE 延时 */
    uint16_t t_cea_max_ns; /* CE→输出有效（max） */
    uint16_t t_chz_max_ns; /* CE→输出高阻（max） */
    uint16_t t_clr_ns;     /* CLE→RE 延时 */
    uint16_t t_coh_ns;     /* CE 高→输出保持 */
    uint16_t t_ir_ns;      /* 命令→数据输出间隔 */
    uint16_t t_rc_ns;      /* 读周期 */
    uint16_t t_rea_max_ns; /* RE→输出有效（max） */
    uint16_t t_reh_ns;     /* RE 高电平宽度 */
    uint16_t t_rhoh_ns;    /* RE 高→输出保持 */
    uint16_t t_rhw_ns;     /* RE→WE 延时 */
    uint16_t t_rhz_max_ns; /* RE→输出高阻（max） */
    uint16_t t_rloh_ns;    /* RE 低→输出保持 */
    uint16_t t_rp_ns;      /* RE 低电平宽度 */
    uint16_t t_rr_ns;      /* 读命令→读命令间隔 */
    uint16_t t_wb_max_ns;  /* WE 高→busy（max） */
    uint16_t t_whr_ns;     /* 状态→RE 高延时 */
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
    volatile uint8_t *data;
    volatile uint8_t *command;
    volatile uint8_t *address;
    nand_fmc_timing_t timing;
} nand_fmc_config_t;

/* ── 默认 (0,0,0) 后端访问器（同步路径 + NULL 兜底用） ── */
const nand_engine_backend_t *nand_fmc_backend_get(void);

/* ── Phase V: per-(CH,CE,LUN) 后端实例 ── */
typedef struct
{
    nand_fmc_config_t     config;
    nand_engine_backend_t backend;
} nand_fmc_instance_t;

/* ── Phase V 接口（多 LUN 并发用） ── */
nand_result_t                nand_fmc_init_instance(uint8_t ch, uint8_t ce, uint8_t lun, const nand_fmc_config_t *config);
const nand_engine_backend_t *nand_fmc_backend_get_ext(uint8_t ch, uint8_t ce, uint8_t lun);

extern nand_fmc_instance_t g_nand_fmc_instances[CH_NUM][CE_NUM][LUN_NUM];

#endif /* PLATFORM_STM32H743_NAND_FMC_H */

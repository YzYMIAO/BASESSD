#ifndef PLATFORM_STM32H743_NAND_FMC_H
#define PLATFORM_STM32H743_NAND_FMC_H

#include "nand_engine.h"

typedef uint8_t (*nand_fmc_wait_ready_t)(void *context, uint16_t timeout_us);

typedef struct
{
    uint16_t t_adl_ns;
    uint16_t t_alh_ns;
    uint16_t t_als_ns;
    uint16_t t_ch_ns;
    uint16_t t_clh_ns;
    uint16_t t_cls_ns;
    uint16_t t_cs_ns;
    uint16_t t_dh_ns;
    uint16_t t_ds_ns;
    uint16_t t_wc_ns;
    uint16_t t_wh_ns;
    uint16_t t_wp_ns;
    uint16_t t_ar_ns;
    uint16_t t_cea_max_ns;
    uint16_t t_chz_max_ns;
    uint16_t t_clr_ns;
    uint16_t t_coh_ns;
    uint16_t t_ir_ns;
    uint16_t t_rc_ns;
    uint16_t t_rea_max_ns;
    uint16_t t_reh_ns;
    uint16_t t_rhoh_ns;
    uint16_t t_rhw_ns;
    uint16_t t_rhz_max_ns;
    uint16_t t_rloh_ns;
    uint16_t t_rp_ns;
    uint16_t t_rr_ns;
    uint16_t t_wb_max_ns;
    uint16_t t_whr_ns;
} nand_fmc_timing_t;

typedef struct
{
    volatile uint8_t     *data;
    volatile uint8_t     *command;
    volatile uint8_t     *address;
    nand_fmc_timing_t     timing;
    nand_fmc_wait_ready_t wait_ready;
    void                 *context;
} nand_fmc_config_t;

nand_result_t                nand_fmc_configure(const nand_fmc_config_t *config);
const nand_engine_backend_t *nand_fmc_backend_get(void);

/* ── Phase V: per-(CH,CE,LUN) 后端实例 ── */

typedef struct
{
    nand_fmc_config_t       config;
    nand_engine_backend_t   backend;
} nand_fmc_instance_t;

/* 初始化指定 (ch,ce,lun) 的 FMC 后端实例。config 内容拷贝到实例。 */
nand_result_t nand_fmc_init_instance(uint8_t ch, uint8_t ce, uint8_t lun,
                                     const nand_fmc_config_t *config);

/* 获取指定 (ch,ce,lun) 的 engine backend 指针。越界返回 NULL。 */
const nand_engine_backend_t *nand_fmc_backend_get_ext(uint8_t ch, uint8_t ce, uint8_t lun);

/* 全局实例数组（供 nand_dispatch 直接访问） */
extern nand_fmc_instance_t g_nand_fmc_instances[CH_NUM][CE_NUM][LUN_NUM];

#endif /* PLATFORM_STM32H743_NAND_FMC_H */

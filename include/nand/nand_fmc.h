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

#endif /* PLATFORM_STM32H743_NAND_FMC_H */

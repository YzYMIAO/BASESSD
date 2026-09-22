#include "nand_mt29f4g08abadawp.h"

#include "common/ssd_log.h"
#include "delay.h"
#include "nand_config.h"
#include "nand_fmc.h"
#include "platform/stm32h743/nand_dma.h"
#include "platform/stm32h743/nand_rb_interrupt.h"

#include <stddef.h>

static NAND_HandleTypeDef g_nand_handle;

/* 低速时序（NAND 默认 Mode 0，tRC=50ns） */
static const nand_fmc_timing_t g_nand_timing_slow = {
    70U, 5U, 10U, 5U, 5U, 10U, 15U, 5U, 7U, 20U, 7U, 10U, 10U, 25U, 50U,
    10U, 15U, 0U, 20U, 16U, 7U, 15U, 100U, 100U, 5U, 10U, 20U, 100U, 60U,
};

/* 高速时序（ONFI Mode 4，tRC=20ns） */
static const nand_fmc_timing_t g_nand_timing_fast = {
    20U, 5U, 5U, 5U, 5U, 5U, 10U, 5U, 5U, 10U, 5U, 5U, 10U, 20U, 30U,
    10U, 10U, 0U, 10U, 12U, 5U, 5U, 50U, 50U, 5U, 5U, 20U, 50U, 30U,
};

/*
 * 根据 ONFI 时序参数 nand_fmc_timing_t 和 FMC 时钟频率，
 * 计算 FMC PCC 硬件寄存器值。
 *
 * FMC kernel clock = 220 MHz → tHCLK ≈ 4.55 ns。
 * 所有 ONFI 时序量纲为 ns，向上取整到整周期数。
 *
 * 当前实现基于 MT29F4G08ABADAWP 异步时序协商规则：
 *   SetupTime     = max(tCLS, tALS, tCS)           / tHCLK
 *   WaitSetupTime = max(tRP, tWP)                  / tHCLK
 *   HoldSetupTime = max(tCH, tALH, tCLH, tDH)      / tHCLK
 *   HiZSetupTime  = tRHZ / tHCLK
 */
static void nand_fmc_timing_to_pcc(const nand_fmc_timing_t *onfi,
                                    FMC_NAND_PCC_TimingTypeDef *pcc)
{
    /* FMC kernel clock period in ns, fixed at compile time for STM32H743 */
    const uint32_t hclk_ns = 5U;  /* 220 MHz → 4.55 ns, 取整到 5 ns */

    #define CEIL_DIV(a, b) (((a) + (b) - 1U) / (b))

    /* max(tCLS, tALS, tCS) → SetupTime */
    {
        uint32_t max_ns = onfi->t_cls_ns;
        if (onfi->t_als_ns > max_ns) max_ns = onfi->t_als_ns;
        if (onfi->t_cs_ns  > max_ns) max_ns = onfi->t_cs_ns;
        pcc->SetupTime = (uint8_t)CEIL_DIV(max_ns, hclk_ns);
    }

    /* max(tRP, tWP) → WaitSetupTime */
    {
        uint32_t max_ns = onfi->t_rp_ns;
        if (onfi->t_wp_ns > max_ns) max_ns = onfi->t_wp_ns;
        pcc->WaitSetupTime = (uint8_t)CEIL_DIV(max_ns, hclk_ns);
    }

    /* max(tCH, tALH, tCLH, tDH) → HoldSetupTime */
    {
        uint32_t max_ns = onfi->t_ch_ns;
        if (onfi->t_alh_ns > max_ns) max_ns = onfi->t_alh_ns;
        if (onfi->t_clh_ns > max_ns) max_ns = onfi->t_clh_ns;
        if (onfi->t_dh_ns  > max_ns) max_ns = onfi->t_dh_ns;
        pcc->HoldSetupTime = (uint8_t)CEIL_DIV(max_ns, hclk_ns);
    }

    pcc->HiZSetupTime = (uint8_t)CEIL_DIV(onfi->t_rhz_max_ns, hclk_ns);

    #undef CEIL_DIV
}
static uint8_t nand_fmc_wait_ready(void *context, uint16_t timeout_us)
{
    uint16_t elapsed_us;

    (void)context;
    for (elapsed_us = 0U; elapsed_us < timeout_us; elapsed_us++)
    {
        if (HAL_GPIO_ReadPin(MT29F4G08ABADAWP_RB_GPIO_PORT, MT29F4G08ABADAWP_RB_GPIO_PIN) == GPIO_PIN_SET)
        {
            return 0U;
        }
        delay_us(1U);
    }
    return 1U;
}

nand_result_t nand_fc_execute(uint8_t operation, const nand_fc_request_t *request, nand_fc_completion_t *completion)
{
    const nand_operation_map_t *map;
    nand_command_request_t      command_request;
    nand_command_completion_t   command_completion;
    uint8_t                     status;

    if ((operation >= OP_COUNT) || (request == NULL) || (completion == NULL))
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    map = &g_nand_operation_map[operation];
    if (map->uops == NULL)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    command_request              = (nand_command_request_t){operation, request->column, {request->row[0]}, request->data, request->data_length};
    status                       = nand_engine_execute(nand_fmc_backend_get(), map->uops, &map->timing, &command_request, &command_completion);
    completion->execution_status = status;
    completion->nand_status      = command_completion.nand_status;
    return status;
}

nand_result_t nand_fc_start(nand_fc_transaction_t *transaction, uint8_t operation, const nand_fc_request_t *request)
{
    const nand_operation_map_t *map;
    uint8_t                     result;

    if ((transaction == NULL) || (operation >= OP_COUNT) || (request == NULL))
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    map = &g_nand_operation_map[operation];
    if (map->uops == NULL)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    transaction->request    = (nand_command_request_t){operation, request->column, {request->row[0]}, request->data, request->data_length};
    transaction->completion = (nand_command_completion_t){0};
    transaction->active     = 1U;
    result                  = nand_engine_start(&transaction->engine, nand_fmc_backend_get(), map->uops, &map->timing, &transaction->request, &transaction->completion);
    if ((result != NAND_RESULT_OK) && (result != NAND_RESULT_NOT_READY))
    {
        SSD_LOGE("nand: start failed, op=%u result=0x%02X status=0x%02X\r\n", operation, result, transaction->completion.nand_status);
        transaction->active = 0U;
    }
    else if (result == NAND_RESULT_OK)
    {
        /* Commands without a ready/busy phase complete during nand_engine_start(). */
        transaction->active = 0U;
    }
    return result;
}

nand_result_t nand_fc_step(nand_fc_transaction_t *transaction, nand_fc_completion_t *completion)
{
    uint8_t result;

    if ((transaction == NULL) || (completion == NULL) || (transaction->active == 0U))
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    result = nand_engine_step(&transaction->engine);
    if (result != NAND_RESULT_NOT_READY)
    {
        completion->execution_status = result;
        completion->nand_status      = transaction->completion.nand_status;
        SSD_LOGD("nand: complete, op=%u result=0x%02X status=0x%02X\r\n", transaction->request.operation, result, completion->nand_status);
        transaction->active = 0U;
    }
    return result;
}

nand_result_t nand_fc_reset(nand_fc_completion_t *completion)
{
    const nand_fc_request_t request = {.column = 0U, .row = {0U}, .data = NULL, .data_length = 0U};

    return nand_fc_execute(OP_RESET, &request, completion);
}

nand_result_t nand_fc_read_id(uint8_t id[5], nand_fc_completion_t *completion)
{
    const nand_fc_request_t request = {.column = 0U, .row = {0U}, .data = id, .data_length = 5U};

    if (id == NULL)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    return nand_fc_execute(OP_READ_ID, &request, completion);
}

nand_result_t nand_fc_set_features(uint8_t feature_address, const uint8_t feature_data[4], nand_fc_completion_t *completion)
{
    const nand_fc_request_t request = {.column = feature_address, .row = {0U}, .data = (uint8_t *)feature_data, .data_length = 4U};

    if (feature_data == NULL)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    return nand_fc_execute(OP_SET_FEATURES, &request, completion);
}

nand_result_t nand_fc_read_page(const nand_fc_request_t *request, nand_fc_completion_t *completion)
{
    return nand_fc_execute(OP_NORMAL_READ_PAGE_WITH_DATA_OUT, request, completion);
}

nand_result_t nand_fc_program_page(const nand_fc_request_t *request, nand_fc_completion_t *completion)
{
    return nand_fc_execute(OP_NORMAL_PROGRAM_PAGE, request, completion);
}

nand_result_t nand_fc_erase_block(const nand_fc_request_t *request, nand_fc_completion_t *completion)
{
    return nand_fc_execute(OP_ERASE_BLOCK, request, completion);
}

uint8_t nand_init(void)
{
    FMC_NAND_PCC_TimingTypeDef common_timing    = {0};
    FMC_NAND_PCC_TimingTypeDef attribute_timing = {0};
    nand_fc_completion_t       completion;
    uint8_t                    id[5];
    uint8_t                    result;

    g_nand_handle.Instance             = FMC_NAND_DEVICE;
    g_nand_handle.Init.NandBank        = FMC_NAND_BANK3;
    g_nand_handle.Init.Waitfeature     = FMC_NAND_PCC_WAIT_FEATURE_DISABLE;
    g_nand_handle.Init.MemoryDataWidth = FMC_NAND_PCC_MEM_BUS_WIDTH_8;
    g_nand_handle.Init.EccComputation  = FMC_NAND_ECC_DISABLE;
    g_nand_handle.Init.ECCPageSize     = FMC_NAND_ECC_PAGE_SIZE_512BYTE;
    /* Start with the board-vendor's conservative timings before switching NAND to Mode 4. */
    g_nand_handle.Init.TCLRSetupTime = 8U;
    g_nand_handle.Init.TARSetupTime  = 8U;

    /* FMC kernel clock=220 MHz. These values are valid at NAND power-on default timing. */
    common_timing.SetupTime     = 4U;
    common_timing.WaitSetupTime = 6U;
    common_timing.HoldSetupTime = 2U;
    common_timing.HiZSetupTime  = 3U;
    attribute_timing            = common_timing;

    if (HAL_NAND_Init(&g_nand_handle, &common_timing, &attribute_timing) != HAL_OK)
    {
        SSD_LOGE("nand: HAL_NAND_Init failed\r\n");
        return NAND_RESULT_OPERATION_FAILED;
    }
    SSD_LOGI("nand: FMC ready, R/B#=%u\r\n",
             (unsigned int)HAL_GPIO_ReadPin(MT29F4G08ABADAWP_RB_GPIO_PORT,
                                            MT29F4G08ABADAWP_RB_GPIO_PIN));

    /* 初始化 R/B# EXTI6 中断（在首次 NAND 命令前完成） */
    nand_rb_interrupt_init();

    {
        const nand_fmc_config_t fmc_config = {
            .data       = (volatile uint8_t *)MT29F4G08ABADAWP_FMC_BASE_ADDR,
            .command    = (volatile uint8_t *)(MT29F4G08ABADAWP_FMC_BASE_ADDR | MT29F4G08ABADAWP_FMC_CMD_OFFSET),
            .address    = (volatile uint8_t *)(MT29F4G08ABADAWP_FMC_BASE_ADDR | MT29F4G08ABADAWP_FMC_ADDR_OFFSET),
            .timing     = g_nand_timing_slow,
            .wait_ready = nand_fmc_wait_ready,
            .context    = NULL,
        };

        if (nand_fmc_configure(&fmc_config) != NAND_RESULT_OK)
        {
            SSD_LOGE("nand: FMC backend configure failed\r\n");
            return NAND_RESULT_OPERATION_FAILED;
        }

        if (nand_dma_init() != 0U)
        {
            SSD_LOGE("nand: DMA init failed, falling back to PIO\r\n");
            /* DMA 失败不是致命错误，回退到 CPU 逐字节搬运 */
        }
    }

    /* Datasheet power-on rule: allow R/B# to become valid before the first FFh command. */
    delay_ms(1U);
    result = nand_fc_reset(&completion);
    if (result != NAND_RESULT_OK)
    {
        SSD_LOGE("nand: reset failed, result=0x%02X status=0x%02X R/B#=%u\r\n",
                 (unsigned int)result,
                 (unsigned int)completion.nand_status,
                 (unsigned int)HAL_GPIO_ReadPin(MT29F4G08ABADAWP_RB_GPIO_PORT,
                                                MT29F4G08ABADAWP_RB_GPIO_PIN));
        return result;
    }
    SSD_LOGI("nand: reset complete, status=0x%02X\r\n", (unsigned int)completion.nand_status);
    result = nand_fc_read_id(id, &completion);
    if (result != NAND_RESULT_OK)
    {
        SSD_LOGE("nand: read ID failed, result=0x%02X status=0x%02X\r\n",
                 (unsigned int)result,
                 (unsigned int)completion.nand_status);
        return result;
    }
    SSD_LOGI("nand: ID=%02X %02X %02X %02X %02X\r\n", (unsigned int)id[0], (unsigned int)id[1], (unsigned int)id[2], (unsigned int)id[3], (unsigned int)id[4]);
    if ((((uint32_t)id[1] << 24U) | ((uint32_t)id[2] << 16U) | ((uint32_t)id[3] << 8U) | id[4]) != MT29F4G08ABADAWP_ID)
    {
        SSD_LOGE("nand: unexpected ID\r\n");
        return NAND_RESULT_OPERATION_FAILED;
    }

#if CONFIG_NAND_TIMING_MODE_4
    /* ── 高速模式：切 NAND 到 Mode 4 + 更新 FMC 硬件时序 ── */
    {
        const uint8_t mode_4[4] = {4U, 0U, 0U, 0U};
        FMC_NAND_PCC_TimingTypeDef fast_pcc = {0};

        result = nand_fc_set_features(0x01U, mode_4, &completion);
        if (result != NAND_RESULT_OK)
        {
            SSD_LOGE("nand: set feature 01h (Mode 4) failed, result=0x%02X status=0x%02X\r\n",
                     (unsigned int)result,
                     (unsigned int)completion.nand_status);
            return result;
        }
        SSD_LOGI("nand: switched NAND to ONFI Mode 4\r\n");

        /* 根据 Mode 4 时序重新计算 FMC PCC 硬件参数 */
        nand_fmc_timing_to_pcc(&g_nand_timing_fast, &fast_pcc);
        g_nand_handle.Init.TCLRSetupTime = 4U;
        g_nand_handle.Init.TARSetupTime  = 4U;
        if (HAL_NAND_Init(&g_nand_handle, &fast_pcc, &fast_pcc) != HAL_OK)
        {
            SSD_LOGE("nand: HAL_NAND_Init (Mode 4) failed\r\n");
            return NAND_RESULT_OPERATION_FAILED;
        }
        SSD_LOGI("nand: FMC timing updated for Mode 4\r\n");

        /* 更新 FMC 后端时序，供 UOP handler 使用 */
        {
            nand_fmc_config_t fast_fmc = {
                .data       = (volatile uint8_t *)MT29F4G08ABADAWP_FMC_BASE_ADDR,
                .command    = (volatile uint8_t *)(MT29F4G08ABADAWP_FMC_BASE_ADDR | MT29F4G08ABADAWP_FMC_CMD_OFFSET),
                .address    = (volatile uint8_t *)(MT29F4G08ABADAWP_FMC_BASE_ADDR | MT29F4G08ABADAWP_FMC_ADDR_OFFSET),
                .timing     = g_nand_timing_fast,
                .wait_ready = nand_fmc_wait_ready,
                .context    = NULL,
            };
            if (nand_fmc_configure(&fast_fmc) != NAND_RESULT_OK)
            {
                SSD_LOGE("nand: FMC backend configure (Mode 4) failed\r\n");
                return NAND_RESULT_OPERATION_FAILED;
            }
        }
    }
#else
    /* ── 低速模式：跳过 Mode 4 切换，NAND 和 FMC 都运行在默认慢速时序 ── */
    SSD_LOGI("nand: keeping NAND at default timing (CONFIG_NAND_TIMING_MODE_4=0)\r\n");
#endif /* CONFIG_NAND_TIMING_MODE_4 */

    SSD_LOGI("nand: init complete\r\n");
    return NAND_RESULT_OK;
}

void HAL_NAND_MspInit(NAND_HandleTypeDef *hnand)
{
    GPIO_InitTypeDef gpio = {0};

    (void)hnand;
    __HAL_RCC_FMC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();

    gpio.Pin   = MT29F4G08ABADAWP_RB_GPIO_PIN;
    gpio.Mode  = GPIO_MODE_INPUT;
    gpio.Pull  = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_HIGH;
    HAL_GPIO_Init(MT29F4G08ABADAWP_RB_GPIO_PORT, &gpio);

    gpio.Pin       = GPIO_PIN_9;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_HIGH;
    gpio.Alternate = GPIO_AF12_FMC;
    HAL_GPIO_Init(GPIOG, &gpio);

    gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_14 | GPIO_PIN_15;
    HAL_GPIO_Init(GPIOD, &gpio);

    gpio.Pin = GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10;
    HAL_GPIO_Init(GPIOE, &gpio);
}

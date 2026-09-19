#include "nand_mt29f4g08abadawp.h"

#include "common/ssd_log.h"
#include "delay.h"
#include "nand_fmc.h"

#include <stddef.h>

static NAND_HandleTypeDef g_nand_handle;

/* R/B# polling is backend transport behavior. Command/status sequencing remains in ONFI UOPs. */
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

uint8_t nand_fc_execute(uint8_t operation, const nand_fc_request_t *request, nand_fc_completion_t *completion)
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

    command_request              = (nand_command_request_t){operation, request->column, request->row, request->data, request->data_length};
    status                       = nand_engine_execute(nand_fmc_backend_get(), map->uops, &map->timing, &command_request, &command_completion);
    completion->execution_status = status;
    completion->nand_status      = command_completion.nand_status;
    return status;
}

uint8_t nand_fc_start(nand_fc_transaction_t *transaction, uint8_t operation, const nand_fc_request_t *request)
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
    transaction->request    = (nand_command_request_t){operation, request->column, request->row, request->data, request->data_length};
    transaction->completion = (nand_command_completion_t){0};
    transaction->active     = 1U;
    result                  = nand_engine_start(&transaction->engine, nand_fmc_backend_get(), map->uops, &map->timing, &transaction->request, &transaction->completion);
    if ((result != NAND_RESULT_OK) && (result != NAND_RESULT_NOT_READY))
    {
        SSD_LOGE("nand: start failed, op=%u result=0x%02X status=0x%02X\r\n", operation, result,
                 transaction->completion.nand_status);
        transaction->active = 0U;
    }
    else if (result == NAND_RESULT_OK)
    {
        /* Commands without a ready/busy phase complete during nand_engine_start(). */
        transaction->active = 0U;
    }
    return result;
}

uint8_t nand_fc_step(nand_fc_transaction_t *transaction, nand_fc_completion_t *completion)
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
        SSD_LOGD("nand: complete, op=%u result=0x%02X status=0x%02X\r\n", transaction->request.operation, result,
                 completion->nand_status);
        transaction->active          = 0U;
    }
    return result;
}

uint8_t nand_fc_reset(nand_fc_completion_t *completion)
{
    const nand_fc_request_t request = {0U, 0U, NULL, 0U};

    return nand_fc_execute(OP_RESET, &request, completion);
}

uint8_t nand_fc_read_id(uint8_t id[5], nand_fc_completion_t *completion)
{
    const nand_fc_request_t request = {0U, 0U, id, 5U};

    if (id == NULL)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    return nand_fc_execute(OP_READ_ID, &request, completion);
}

uint8_t nand_fc_set_features(uint8_t feature_address, const uint8_t feature_data[4], nand_fc_completion_t *completion)
{
    const nand_fc_request_t request = {feature_address, 0U, (uint8_t *)feature_data, 4U};

    if (feature_data == NULL)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    return nand_fc_execute(OP_SET_FEATURES, &request, completion);
}

uint8_t nand_fc_read_page(const nand_fc_request_t *request, nand_fc_completion_t *completion)
{
    return nand_fc_execute(OP_NORMAL_READ_PAGE, request, completion);
}

uint8_t nand_fc_program_page(const nand_fc_request_t *request, nand_fc_completion_t *completion)
{
    return nand_fc_execute(OP_NORMAL_PROGRAM_PAGE, request, completion);
}

uint8_t nand_fc_erase_block(const nand_fc_request_t *request, nand_fc_completion_t *completion)
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
    const uint8_t              mode_4[4] = {4U, 0U, 0U, 0U};

    g_nand_handle.Instance             = FMC_NAND_DEVICE;
    g_nand_handle.Init.NandBank        = FMC_NAND_BANK3;
    g_nand_handle.Init.Waitfeature     = FMC_NAND_PCC_WAIT_FEATURE_DISABLE;
    g_nand_handle.Init.MemoryDataWidth = FMC_NAND_PCC_MEM_BUS_WIDTH_8;
    g_nand_handle.Init.EccComputation  = FMC_NAND_ECC_DISABLE;
    g_nand_handle.Init.ECCPageSize     = FMC_NAND_ECC_PAGE_SIZE_512BYTE;
    /* Start with the board-vendor's conservative timings before switching NAND to Mode 4. */
    g_nand_handle.Init.TCLRSetupTime   = 8U;
    g_nand_handle.Init.TARSetupTime    = 8U;

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

    {
        const nand_fmc_config_t fmc_config = {
            .data       = (volatile uint8_t *)MT29F4G08ABADAWP_FMC_BASE_ADDR,
            .command    = (volatile uint8_t *)(MT29F4G08ABADAWP_FMC_BASE_ADDR | MT29F4G08ABADAWP_FMC_CMD_OFFSET),
            .address    = (volatile uint8_t *)(MT29F4G08ABADAWP_FMC_BASE_ADDR | MT29F4G08ABADAWP_FMC_ADDR_OFFSET),
            .timing     = {70U, 5U, 10U, 5U, 5U, 10U, 15U, 5U, 7U, 20U, 7U, 10U, 10U, 25U, 50U, 10U, 15U, 0U, 20U, 16U, 7U, 15U, 100U, 100U, 5U, 10U, 20U, 100U, 60U},
            .wait_ready = nand_fmc_wait_ready,
            .context    = NULL,
        };

        if (nand_fmc_configure(&fmc_config) != NAND_RESULT_OK)
        {
            SSD_LOGE("nand: FMC backend configure failed\r\n");
            return NAND_RESULT_OPERATION_FAILED;
        }
    }

    /* Datasheet power-on rule: allow R/B# to become valid before the first FFh command. */
    delay_ms(1U);
    result = nand_fc_reset(&completion);
    if (result != NAND_RESULT_OK)
    {
        SSD_LOGE("nand: reset failed, result=0x%02X status=0x%02X R/B#=%u\r\n",
                 (unsigned int)result, (unsigned int)completion.nand_status,
                 (unsigned int)HAL_GPIO_ReadPin(MT29F4G08ABADAWP_RB_GPIO_PORT,
                                                 MT29F4G08ABADAWP_RB_GPIO_PIN));
        return result;
    }
    SSD_LOGI("nand: reset complete, status=0x%02X\r\n", (unsigned int)completion.nand_status);
    result = nand_fc_read_id(id, &completion);
    if (result != NAND_RESULT_OK)
    {
        SSD_LOGE("nand: read ID failed, result=0x%02X status=0x%02X\r\n",
                 (unsigned int)result, (unsigned int)completion.nand_status);
        return result;
    }
    SSD_LOGI("nand: ID=%02X %02X %02X %02X %02X\r\n", (unsigned int)id[0],
             (unsigned int)id[1], (unsigned int)id[2], (unsigned int)id[3],
             (unsigned int)id[4]);
    if ((((uint32_t)id[1] << 24U) | ((uint32_t)id[2] << 16U) | ((uint32_t)id[3] << 8U) | id[4]) != MT29F4G08ABADAWP_ID)
    {
        SSD_LOGE("nand: unexpected ID\r\n");
        return NAND_RESULT_OPERATION_FAILED;
    }
    result = nand_fc_set_features(0x01U, mode_4, &completion);
    if (result != NAND_RESULT_OK)
    {
        SSD_LOGE("nand: set feature 01h failed, result=0x%02X status=0x%02X\r\n",
                 (unsigned int)result, (unsigned int)completion.nand_status);
        return result;
    }
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

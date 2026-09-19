#ifndef PLATFORM_STM32H743_NAND_MT29F4G08ABADAWP_H
#define PLATFORM_STM32H743_NAND_MT29F4G08ABADAWP_H

#include "nand/onfi_1_0.h"
#include "sys.h"

/* MT29F4G08ABADAWP physical geometry. FC/FTL logical geometry is in nand_config.h. */
#define MT29F4G08ABADAWP_PAGE_MAIN_SIZE   2048U
#define MT29F4G08ABADAWP_PAGE_SPARE_SIZE  64U
#define MT29F4G08ABADAWP_PAGE_TOTAL_SIZE  2112U
#define MT29F4G08ABADAWP_PAGES_PER_BLOCK  64U
#define MT29F4G08ABADAWP_BLOCKS_PER_PLANE 2048U
#define MT29F4G08ABADAWP_TOTAL_BLOCKS     4096U
#define MT29F4G08ABADAWP_SIZE_BYTES       (512UL * 1024UL * 1024UL)

/* Read ID returns 2C DC 90 95 56; this value excludes the Micron manufacturer byte. */
#define MT29F4G08ABADAWP_ID 0xDC909556UL

/* FMC NAND Bank3 memory windows. Only nand_fmc.c writes command/address/data cycles. */
#define MT29F4G08ABADAWP_FMC_BASE_ADDR   0x80000000UL
#define MT29F4G08ABADAWP_FMC_CMD_OFFSET  (1UL << 16)
#define MT29F4G08ABADAWP_FMC_ADDR_OFFSET (1UL << 17)

/* R/B# is a backend-ready indication, not a NAND command interface. */
#define MT29F4G08ABADAWP_RB_GPIO_PORT GPIOD
#define MT29F4G08ABADAWP_RB_GPIO_PIN  GPIO_PIN_6

typedef struct
{
    uint16_t column;
    uint32_t row;
    uint8_t *data;
    uint16_t data_length;
} nand_fc_request_t;

typedef struct
{
    uint8_t execution_status; /* NAND_RESULT_* */
    uint8_t nand_status;      /* Raw 70h status value after command completion. */
} nand_fc_completion_t;

typedef struct
{
    nand_engine_state_t       engine;
    nand_command_request_t    request;
    nand_command_completion_t completion;
    uint8_t                   active;
} nand_fc_transaction_t;

/*
 * Initializes FMC hardware and verifies the NAND through OP_RESET/OP_READ_ID/OP_SET_FEATURES.
 * @return NAND_RESULT_OK on success; otherwise a NAND_RESULT_* failure code.
 */
uint8_t nand_init(void);

/* Generic ONFI execution entry: OP_* -> g_nand_operation_map -> UOP -> FMC backend. */
uint8_t nand_fc_execute(uint8_t operation, const nand_fc_request_t *request, nand_fc_completion_t *completion);
uint8_t nand_fc_start(nand_fc_transaction_t *transaction, uint8_t operation, const nand_fc_request_t *request);
uint8_t nand_fc_step(nand_fc_transaction_t *transaction, nand_fc_completion_t *completion);

/* Typed ONFI operation helpers. No helper writes FMC command/address/data windows directly. */
uint8_t nand_fc_reset(nand_fc_completion_t *completion);
uint8_t nand_fc_read_id(uint8_t id[5], nand_fc_completion_t *completion);
uint8_t nand_fc_set_features(uint8_t feature_address, const uint8_t feature_data[4], nand_fc_completion_t *completion);
uint8_t nand_fc_read_page(const nand_fc_request_t *request, nand_fc_completion_t *completion);
uint8_t nand_fc_program_page(const nand_fc_request_t *request, nand_fc_completion_t *completion);
uint8_t nand_fc_erase_block(const nand_fc_request_t *request, nand_fc_completion_t *completion);

#endif /* PLATFORM_STM32H743_NAND_MT29F4G08ABADAWP_H */

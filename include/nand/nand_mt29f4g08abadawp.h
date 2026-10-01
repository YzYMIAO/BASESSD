#ifndef PLATFORM_STM32H743_NAND_MT29F4G08ABADAWP_H
#define PLATFORM_STM32H743_NAND_MT29F4G08ABADAWP_H

#include "nand/nand_fc_interface.h"
#include "sys.h"

/*
 * nand_mt29f4g08abadawp.h — Micron MT29F4G08ABADAWP 芯片定义
 *
 * 芯片：512MB SLC NAND（2 plane × 2048 block × 64 page × 2112B）。
 *
 * 注意两套几何并存：
 *   - 本头文件 = 物理几何（芯片数据手册，含 spare 区）。
 *   - nand_config.h = FC/FTL 逻辑几何（地址编码 PAA/PBA 视角）。
 */

/* MT29F4G08ABADAWP physical geometry. FC/FTL logical geometry is in nand_config.h. */
#define MT29F4G08ABADAWP_PAGE_MAIN_SIZE   2048U /* 主区 */
#define MT29F4G08ABADAWP_PAGE_SPARE_SIZE  64U   /* 备区 */
#define MT29F4G08ABADAWP_PAGE_TOTAL_SIZE  2112U /* 主+备 */
#define MT29F4G08ABADAWP_PAGES_PER_BLOCK  64U
#define MT29F4G08ABADAWP_BLOCKS_PER_PLANE 2048U
#define MT29F4G08ABADAWP_TOTAL_BLOCKS     4096U /* 2 plane × 2048 */
#define MT29F4G08ABADAWP_SIZE_BYTES       (512UL * 1024UL * 1024UL)

/* Read ID returns 2C DC 90 95 56; this value excludes the Micron manufacturer byte.
 * 校验时跳过 id[0]=0x2C，用 id[1..4] 拼 4 字节比较（见 nand_init）。 */
#define MT29F4G08ABADAWP_ID 0xDC909556UL

/*
 * FMC NAND Bank3 memory windows. Only nand_fmc.c writes command/address/data cycles.
 *
 * 窗口别名机制：对 FMC Bank 内存映射地址写，A16=1 选命令口、A17=1 选地址口、
 * A16/A17 均 0 为数据口。BASE + CMD_OFFSET / ADDR_OFFSET 即三个窗口。
 */
#define MT29F4G08ABADAWP_FMC_BASE_ADDR   0x80000000UL /* FMC Bank3 起始 */
#define MT29F4G08ABADAWP_FMC_CMD_OFFSET  (1UL << 16)  /* A16=1 → 命令窗口 */
#define MT29F4G08ABADAWP_FMC_ADDR_OFFSET (1UL << 17)  /* A17=1 → 地址窗口 */

/* R/B# is a backend-ready indication, not a NAND command interface. */
#define MT29F4G08ABADAWP_RB_GPIO_PORT GPIOD
#define MT29F4G08ABADAWP_RB_GPIO_PIN  GPIO_PIN_6

/*
 * nand_init
 *   功能：初始化 FMC 硬件并通过 OP_RESET/OP_READ_ID/OP_SET_FEATURES
 *         验证 NAND（含 R/B# 中断、DMA、可选 Mode 4 切换）。
 *   参数：无。
 * @return NAND_RESULT_OK on success; otherwise a NAND_RESULT_* failure code.
 */
uint8_t nand_init(void);

/* 泛型执行入口：OP_* -> g_nand_operation_map -> UOP -> FMC backend。
 * nand_fc_execute = 同步；nand_fc_start/step = 异步（start 发起 + step 推进）。 */
nand_result_t nand_fc_execute(uint8_t operation, const nand_fc_request_t *request, nand_fc_completion_t *completion);
nand_result_t nand_fc_start(nand_fc_transaction_t *transaction, uint8_t operation, const nand_fc_request_t *request);
nand_result_t nand_fc_step(nand_fc_transaction_t *transaction, nand_fc_completion_t *completion);

/* Typed ONFI operation helpers. No helper writes FMC command/address/data windows directly. */
nand_result_t nand_fc_reset(nand_fc_completion_t *completion);
nand_result_t nand_fc_read_id(uint8_t id[5], nand_fc_completion_t *completion);
nand_result_t nand_fc_set_features(uint8_t feature_address, const uint8_t feature_data[4], nand_fc_completion_t *completion);
nand_result_t nand_fc_read_page(const nand_fc_request_t *request, nand_fc_completion_t *completion);
nand_result_t nand_fc_program_page(const nand_fc_request_t *request, nand_fc_completion_t *completion);
nand_result_t nand_fc_erase_block(const nand_fc_request_t *request, nand_fc_completion_t *completion);

#endif /* PLATFORM_STM32H743_NAND_MT29F4G08ABADAWP_H */

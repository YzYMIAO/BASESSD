/*
 * nand_board.c — 板级 NAND 事实：target 通路表 + FMC/GPIO 板级接线
 *
 * 与器件模型分开：
 *   devices/<family>.c  器件几何 / 时序 / ID / 参数页事实；
 *   本文件              这块板上有哪些 (ch,ce)、R/B# 与 NCE/数据总线的 GPIO。
 *
 * 当前板：FMC Bank3 + 单个 NCE，只有 (0,0) 有物理通路。多 target 板卡在这里
 * 扩展 target 表，并在 nand_select_target() 中实现 FMC bank / CE# GPIO 切换。
 */

#include "assertion.h"
#include "log.h"
#include "nand_config.h"
#include "nand_device.h"
#include "nand_fmc.h"

#include <stddef.h>
#include <string.h>

/* 上电拓扑扫描结果：板级/物理事实，不是器件属性。 */
nand_target_info_s g_nand_targets[CH_NUM][CE_NUM];

/* 板级物理通路表：每一项表示 (ch,ce) 有一条可寻址的 NAND target。 */
typedef struct NAND_BOARD_TARGET_S
{
    BYTE ch;
    BYTE ce;
} nand_board_target_s;

static const nand_board_target_s s_board_targets[] = {
    {0U, 0U},
};

/**
 * @brief 板级 target 选择（校验 (ch,ce) 是否在物理通路表内）
 *
 * @param ch 目标 ch
 * @param ce 目标 ce
 *
 * @return NAND_RESULT_OK           通路存在
 *         NAND_RESULT_INVALID_ARGUMENT 越界
 *         NAND_RESULT_NOT_READY   无物理通路
 *
 * @note 多 target 板卡需要在这里切换 FMC bank / CE# GPIO；当前单 bank 单 NCE，
 *       本函数只做通路校验。
 */
static nand_result_e nand_select_target(BYTE ch, BYTE ce)
{
    if ((ch >= CH_NUM) || (ce >= CE_NUM))
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    for (size_t i = 0U; i < (sizeof(s_board_targets) / sizeof(s_board_targets[0])); i++)
    {
        if ((s_board_targets[i].ch == ch) && (s_board_targets[i].ce == ce))
        {
            return NAND_RESULT_OK;
        }
    }

    return NAND_RESULT_NOT_READY;
}

/**
 * @brief 取指定 target 的 NAND 执行入口：通路校验 + PHY backend 一次完成
 *
 * @param ch      通道号。
 * @param ce      片选号。
 * @param backend 出参：通路存在时写入 PHY backend。
 *
 * @return NAND_RESULT_OK / INVALID_ARGUMENT / NOT_READY。
 *
 * @note 通路选择与 backend 获取必须成对发生，所以收成一个入口；调用点不再
 *       分别调用 nand_select_target / nand_phy_backend_get。
 */
nand_result_e nand_target_get(BYTE ch, BYTE ce, const nand_phy_backend_s **backend)
{
    ASSERT(backend != NULL);

    const nand_result_e result = nand_select_target(ch, ce);

    if (result != NAND_RESULT_OK)
    {
        return result;
    }

    *backend = nand_phy_backend_get(ch, ce);
    ASSERT(*backend != NULL);
    return NAND_RESULT_OK;
}

/**
 * @brief 通用器件查询：指定 target 当前生效的 ONFI timing mode
 *
 * @param ch 通道号。
 * @param ce 片选号。
 *
 * @return 模式号（0 = 低速）。
 */
BYTE nand_get_timing_mode(BYTE ch, BYTE ce)
{
    const nand_device_desc_s *dev = nand_device_get();

    ASSERT(dev != NULL);
    ASSERT(dev->get_timing_mode != NULL);
    return dev->get_timing_mode(ch, ce);
}

/**
 * @brief HAL 平台 MSP 回调：时钟 + GPIO 引脚复用
 *
 * 由 HAL_NAND_Init 内部调用；R/B# 上拉输入，NCE/数据总线 AF12。
 *
 * @param hnand HAL 句柄（未使用）
 */
void HAL_NAND_MspInit(NAND_HandleTypeDef *hnand)
{
    GPIO_InitTypeDef gpio;

    memset(&gpio, 0, sizeof(gpio));
    (void)hnand;
    __HAL_RCC_FMC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();

    /* R/B#：上拉输入（busy 时为低） */
    gpio.Pin   = NAND_RB_GPIO_PIN;
    gpio.Mode  = GPIO_MODE_INPUT;
    gpio.Pull  = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_HIGH;
    HAL_GPIO_Init(NAND_RB_GPIO_PORT, &gpio);

    /* GPIOG PIN9：FMC NCE（片选，AF12） */
    gpio.Pin       = GPIO_PIN_9;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_HIGH;
    gpio.Alternate = GPIO_AF12_FMC;
    HAL_GPIO_Init(GPIOG, &gpio);

    /* GPIOD：数据总线低 8 位相关脚（AF12，由上一段 gpio 配置延续） */
    gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_14 | GPIO_PIN_15;
    HAL_GPIO_Init(GPIOD, &gpio);

    /* GPIOE：数据总线高 8 位相关脚（AF12） */
    gpio.Pin = GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10;
    HAL_GPIO_Init(GPIOE, &gpio);

    LOGI(LOG_TAG_NAND "FMC ready, R/B#=%u\r\n", (unsigned int)HAL_GPIO_ReadPin(NAND_RB_GPIO_PORT, NAND_RB_GPIO_PIN));
}

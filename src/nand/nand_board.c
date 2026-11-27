/*
 * nand_board.c — 板级 NAND 事实：target 通路表 + FMC/GPIO 板级接线
 *
 * 与器件模型分开：
 *   devices/<family>.c  器件几何 / 时序 / ID / 参数页事实；
 *   本文件              这块板上有哪些 (ch,ce)、R/B# 与 NCE/数据总线的 GPIO。
 *
 * 当前板：FMC Bank3 + 单个 NCE，只有 (0,0) 有物理通路。多 target 板卡在这里
 * 扩展 target 表，并在 nand_board_select_target() 中实现 FMC bank / CE# GPIO 切换。
 */

#include "nand_board.h"

#include "assertion.h"
#include "log.h"
#include "nand_board_internal.h"
#include "nand_config.h"
#include "nand_device.h"
#include "nand_fmc.h"

#include <stddef.h>
#include <string.h>

/* 上电拓扑扫描结果：板级/物理事实，不是器件属性。 */
nand_target_info_s g_nand_targets[CH_NUM][CE_NUM];

/* 板级物理通路表：每一项表示 (ch,ce) 有一条可寻址的 NAND target。 */
static const nand_board_target_s s_board_targets[] = {
    {0U, 0U, 0U},
};
/* NAND_BOARD_TARGET_MAX 是板级上界；表项数不得超过它（稀疏板卡用它收紧常驻对象）。 */
_Static_assert((sizeof(s_board_targets) / sizeof(s_board_targets[0])) <= NAND_BOARD_TARGET_MAX, "板级 target 表项数超出 NAND_BOARD_TARGET_MAX");

/* 当前板共享 FMC Bank3；同 bank target 不能并发。 */
#define NAND_BOARD_BANK_MAX (16U)

/* 0 = 空闲；非 0 = target 索引 + 1。 */
static WORD s_bank_owner[NAND_BOARD_BANK_MAX];

static size_t nand_board_target_count(void)
{
    return sizeof(s_board_targets) / sizeof(s_board_targets[0]);
}

static const nand_board_target_s *nand_board_target_get(size_t index)
{
    if (index >= nand_board_target_count())
    {
        return NULL;
    }
    return &s_board_targets[index];
}

/* 按 (ch,ce) 查 board target；找到时可选返回数组下标。 */
static const nand_board_target_s *nand_board_find_target(BYTE ch, BYTE ce, size_t *index)
{
    for (size_t i = 0U; i < nand_board_target_count(); i++)
    {
        if ((s_board_targets[i].ch == ch) && (s_board_targets[i].ce == ce))
        {
            if (index != NULL)
            {
                *index = i;
            }
            return &s_board_targets[i];
        }
    }
    return NULL;
}

/* 当前板的 FMC Bank3 三窗口 + R/B# 接线；多 target 板在此按 target 取值。 */
static void nand_board_build_fmc_config(nand_fmc_cfg_s *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->data    = (volatile BYTE *)NAND_FMC_BASE_ADDR;
    cfg->command = (volatile BYTE *)(NAND_FMC_BASE_ADDR | NAND_FMC_CMD_OFFSET);
    cfg->address = (volatile BYTE *)(NAND_FMC_BASE_ADDR | NAND_FMC_ADDR_OFFSET);
    cfg->rb_port = NAND_RB_GPIO_PORT;
    cfg->rb_pin  = NAND_RB_GPIO_PIN;
}

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
static nand_result_e nand_board_select_target(BYTE ch, BYTE ce)
{
    if ((ch >= CH_NUM) || (ce >= CE_NUM))
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    for (size_t i = 0U; i < nand_board_target_count(); i++)
    {
        if ((s_board_targets[i].ch == ch) && (s_board_targets[i].ce == ce))
        {
            /* 当前单 bank 单 NCE 无需切硬件；多 target 板在此切 NCE/FMC bank。 */
            return NAND_RESULT_OK;
        }
    }

    return NAND_RESULT_NOT_READY;
}

nand_result_e nand_board_acquire_target(BYTE ch, BYTE ce)
{
    size_t                     index  = 0U;
    const nand_board_target_s *target = nand_board_find_target(ch, ce, &index);

    if (target == NULL)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }
    if (target->bank >= NAND_BOARD_BANK_MAX)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    const WORD owner = (WORD)(index + 1U);

    if ((s_bank_owner[target->bank] == 0U) || (s_bank_owner[target->bank] == owner))
    {
        s_bank_owner[target->bank] = owner;
        return NAND_RESULT_OK;
    }
    return NAND_RESULT_NOT_READY;
}

void nand_board_release_target(BYTE ch, BYTE ce)
{
    size_t                     index  = 0U;
    const nand_board_target_s *target = nand_board_find_target(ch, ce, &index);

    if ((target == NULL) || (target->bank >= NAND_BOARD_BANK_MAX))
    {
        return;
    }

    const WORD owner = (WORD)(index + 1U);

    if (s_bank_owner[target->bank] == owner)
    {
        s_bank_owner[target->bank] = 0U;
    }
}

void nand_board_ownership_reset(void)
{
    memset(s_bank_owner, 0, sizeof(s_bank_owner));
}

/**
 * @brief 按 (ch,ce) 装配并初始化该 target 的 PHY 后端
 *
 * @param ch 目标通道
 * @param ce 目标片选
 * @return OK / NOT_READY（target 不在板上）/ 错误码
 */
nand_result_e nand_board_init_target(BYTE ch, BYTE ce)
{
    const nand_result_e select_result = nand_board_select_target(ch, ce);

    if (select_result != NAND_RESULT_OK)
    {
        return select_result;
    }

    nand_fmc_cfg_s cfg;

    nand_board_build_fmc_config(&cfg);
    return nand_fmc_init_target(ch, ce, &cfg);
}

/**
 * @brief 初始化板上全部 target 的 PHY 后端
 *
 * @return OK / NOT_READY（无 target）/ 错误码
 */
nand_result_e nand_board_init_all_targets(void)
{
    memset(s_bank_owner, 0, sizeof(s_bank_owner));

    if (nand_board_target_count() == 0U)
    {
        return NAND_RESULT_NOT_READY;
    }

    for (size_t i = 0U; i < nand_board_target_count(); i++)
    {
        const nand_board_target_s *target = nand_board_target_get(i);

        if (target == NULL)
        {
            return NAND_RESULT_INVALID_ARGUMENT;
        }

        const nand_result_e result = nand_board_init_target(target->ch, target->ce);

        if (result != NAND_RESULT_OK)
        {
            return result;
        }
    }
    return NAND_RESULT_OK;
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
 *       分别调用 nand_board_select_target / nand_phy_backend_get。
 */
nand_result_e nand_target_get(BYTE ch, BYTE ce, const nand_phy_backend_s **backend)
{
    ASSERT_REJECT(backend != NULL, NAND_RESULT_INVALID_ARGUMENT, "target get: backend NULL");

    const nand_result_e result = nand_board_select_target(ch, ce);

    if (result != NAND_RESULT_OK)
    {
        return result;
    }

    *backend = nand_phy_backend_get(ch, ce);
    ASSERT_REJECT(*backend != NULL, NAND_RESULT_INVALID_ARGUMENT, "target get: backend missing");
    ASSERT_REJECT((*backend)->handlers != NULL, NAND_RESULT_NOT_READY, "target get: target not initialized");
    return NAND_RESULT_OK;
}

/**
 * @brief 通用器件查询：指定 target 当前生效的 ONFI timing mode
 *
 * @param ch   通道号。
 * @param ce   片选号。
 * @param mode 输出：timing mode（0 = 低速）。
 * @return OK / INVALID_ARGUMENT / NOT_READY。
 */
nand_result_e nand_timing_mode_get(BYTE ch, BYTE ce, BYTE *mode)
{
    ASSERT_REJECT(mode != NULL, NAND_RESULT_INVALID_ARGUMENT, "timing mode: mode NULL");

    const nand_result_e select_result = nand_board_select_target(ch, ce);

    if (select_result != NAND_RESULT_OK)
    {
        return select_result;
    }

    const nand_device_desc_s *dev = nand_device_get();

    ASSERT_REJECT(dev != NULL, NAND_RESULT_INVALID_ARGUMENT, "timing mode: device NULL");
    ASSERT_REJECT(dev->get_timing_mode != NULL, NAND_RESULT_INVALID_ARGUMENT, "timing mode: callback NULL");
    return dev->get_timing_mode(ch, ce, mode);
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

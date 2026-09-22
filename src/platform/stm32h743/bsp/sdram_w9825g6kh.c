#include "common/common.h"

#include "sdram_w9825g6kh.h"

#include "delay.h"
#include "sys.h"

SDRAM_HandleTypeDef g_sdram_handle; /* SRAM句柄 */

/**
 * @brief       初始化SDRAM
 * @param       无
 * @retval      无
 */
uint8_t sdram_init(void)
{
    FMC_SDRAM_TimingTypeDef sdram_timing;

    g_sdram_handle.Instance                = FMC_SDRAM_DEVICE;
    g_sdram_handle.Init.SDBank             = FMC_SDRAM_BANK1;                    /* 第一个SDRAM BANK */
    g_sdram_handle.Init.ColumnBitsNumber   = FMC_SDRAM_COLUMN_BITS_NUM_9;        /* 列数量 */
    g_sdram_handle.Init.RowBitsNumber      = FMC_SDRAM_ROW_BITS_NUM_13;          /* 行数量 */
    g_sdram_handle.Init.MemoryDataWidth    = FMC_SDRAM_MEM_BUS_WIDTH_16;         /* 数据宽度为16位 */
    g_sdram_handle.Init.InternalBankNumber = FMC_SDRAM_INTERN_BANKS_NUM_4;       /* 一共4个BANK */
    g_sdram_handle.Init.CASLatency         = FMC_SDRAM_CAS_LATENCY_2;            /* CAS为2 */
    g_sdram_handle.Init.WriteProtection    = FMC_SDRAM_WRITE_PROTECTION_DISABLE; /* 失能写保护 */
    g_sdram_handle.Init.SDClockPeriod      = FMC_SDRAM_CLOCK_PERIOD_2;           /* SDCLK=220/2=110 MHz */
    g_sdram_handle.Init.ReadBurst          = FMC_SDRAM_RBURST_ENABLE;            /* 使能突发 */
    g_sdram_handle.Init.ReadPipeDelay      = FMC_SDRAM_RPIPE_DELAY_1;            /* 读通道延时 */

    sdram_timing.LoadToActiveDelay    = SDRAM_TMR_LOAD_TO_ACTIVE;
    sdram_timing.ExitSelfRefreshDelay = SDRAM_TMR_EXIT_SELF_REFRESH;
    sdram_timing.SelfRefreshTime      = SDRAM_TMR_SELF_REFRESH;
    sdram_timing.RowCycleDelay        = SDRAM_TMR_ROW_CYCLE;
    sdram_timing.WriteRecoveryTime    = SDRAM_TMR_WRITE_RECOVERY;
    sdram_timing.RPDelay              = SDRAM_TMR_ROW_PRECHARGE;
    sdram_timing.RCDDelay             = SDRAM_TMR_ROW_TO_COLUMN;
    if (HAL_SDRAM_Init(&g_sdram_handle, &sdram_timing) != HAL_OK)
    {
        return FUNC_STATUS_ERROR;
    }

    if (sdram_initialization_sequence() != 0U)
    {
        return 2U;
    }

    /**
     * 刷新频率计数器(以SDCLK频率计数),计算方法:
     * COUNT=SDRAM刷新周期/行数-20=SDRAM刷新周期(us)*SDCLK频率(Mhz)/行数
     * 我们使用的SDRAM刷新周期为64ms,SDCLK=220/2=110Mhz,行数为8192(2^13).
     * 所以,COUNT=64*1000*110/8192-20=839
     */
    if (HAL_SDRAM_ProgramRefreshRate(&g_sdram_handle, SDRAM_REFRESH_RATE) != HAL_OK)
    {
        return 3U;
    }
    return FUNC_STATUS_SUCCESS;
}

/**
 * @brief       发送SDRAM初始化序列
 * @param       无
 * @retval      无
 */
uint8_t sdram_initialization_sequence(void)
{
    uint32_t temp = 0;

    /* SDRAM控制器初始化完成以后还需要按照如下顺序初始化SDRAM */
    if (sdram_send_cmd(0U, FMC_SDRAM_CMD_CLK_ENABLE, 1U, 0U) != 0U)
    {
        return FUNC_STATUS_ERROR;
    }
    delay_us(500); /* 至少延时500us */
    if (sdram_send_cmd(0U, FMC_SDRAM_CMD_PALL, 1U, 0U) != 0U)
    {
        return 2U;
    }
    if (sdram_send_cmd(0U, FMC_SDRAM_CMD_AUTOREFRESH_MODE, SDRAM_AUTO_REFRESH_COMMANDS, 0U) != 0U)
    {
        return 3U;
    }

    /* 配置模式寄存器,SDRAM的bit0~bit2为指定突发访问的长度，
     * bit3为指定突发访问的类型，bit4~bit6为CAS值，bit7和bit8为运行模式
     * bit9为指定的写突发模式，bit10和bit11位保留位 */
    temp = (uint32_t)SDRAM_MODEREG_BURST_LENGTH_1 | /* 设置突发长度:1(可以是1/2/4/8) */
           SDRAM_MODEREG_BURST_TYPE_SEQUENTIAL |    /* 设置突发类型:连续(可以是连续/交错) */
           SDRAM_MODEREG_CAS_LATENCY_2 |            /* 设置CAS值:2(可以是2/3) */
           SDRAM_MODEREG_OPERATING_MODE_STANDARD |  /* 设置操作模式:0,标准模式 */
           SDRAM_MODEREG_WRITEBURST_MODE_SINGLE;    /* 设置突发写模式:1,单点访问 */
    if (sdram_send_cmd(0U, FMC_SDRAM_CMD_LOAD_MODE, 1U, (uint16_t)temp) != 0U)
    {
        return 4U;
    }
    return FUNC_STATUS_SUCCESS;
}

/**
 * @brief       SDRAM底层驱动，引脚配置，时钟使能
 * @note        此函数会被HAL_SDRAM_Init()调用
 * @param       hsdram   : SDRAM句柄
 * @retval      无
 */
void HAL_SDRAM_MspInit(SDRAM_HandleTypeDef *hsdram)
{
    GPIO_InitTypeDef gpio_init_struct;

    (void)hsdram;

    __HAL_RCC_FMC_CLK_ENABLE();   /* 使能FMC时钟 */
    __HAL_RCC_GPIOC_CLK_ENABLE(); /* 使能GPIOC时钟 */
    __HAL_RCC_GPIOD_CLK_ENABLE(); /* 使能GPIOD时钟 */
    __HAL_RCC_GPIOE_CLK_ENABLE(); /* 使能GPIOE时钟 */
    __HAL_RCC_GPIOF_CLK_ENABLE(); /* 使能GPIOF时钟 */
    __HAL_RCC_GPIOG_CLK_ENABLE(); /* 使能GPIOG时钟 */

    gpio_init_struct.Pin       = GPIO_PIN_0 | GPIO_PIN_2 | GPIO_PIN_3;
    gpio_init_struct.Mode      = GPIO_MODE_AF_PP; /* 推挽复用 */
    gpio_init_struct.Pull      = GPIO_PULLUP;     /* 上拉 */
    gpio_init_struct.Speed     = GPIO_SPEED_HIGH; /* 高速 */
    gpio_init_struct.Alternate = GPIO_AF12_FMC;   /* 复用为FMC */
    HAL_GPIO_Init(GPIOC, &gpio_init_struct);      /* 初始化PC0,2,3 */

    gpio_init_struct.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_14 | GPIO_PIN_15;
    HAL_GPIO_Init(GPIOD, &gpio_init_struct); /* 初始化PD0,1,8,9,10,14,15 */

    gpio_init_struct.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
    HAL_GPIO_Init(GPIOE, &gpio_init_struct); /* 初始化PE0,1,7,8,9,10,11,12,13,14,15 */

    gpio_init_struct.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
    HAL_GPIO_Init(GPIOF, &gpio_init_struct); /* 初始化PF0,1,2,3,4,5,11,12,13,14,15 */

    gpio_init_struct.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_8 | GPIO_PIN_15;
    HAL_GPIO_Init(GPIOG, &gpio_init_struct); /* 初始化PG0,1,2,4,5,8,15 */
}

/**
 * @brief       向SDRAM发送命令
 * @param       bankx   : 0,向BANK5上面的SDRAM发送指令
 *   @arg                 1,向BANK6上面的SDRAM发送指令
 * @param       cmd     : 指令(0,正常模式/1,时钟配置使能/2,预充电所有存储区/3,自动刷新/4,加载模式寄存器/5,自刷新/6,掉电)
 * @param       refresh : 自刷新次数
 * @retval      返回值:0,正常;1,失败.
 * @retval      模式寄存器的定义
 */
uint8_t sdram_send_cmd(uint8_t bankx, uint8_t cmd, uint8_t refresh, uint16_t regval)
{
    uint32_t                 target_bank = 0;
    FMC_SDRAM_CommandTypeDef command;

    if (bankx == 0)
    {
        target_bank = FMC_SDRAM_CMD_TARGET_BANK1;
    }
    else if (bankx == 1)
    {
        target_bank = FMC_SDRAM_CMD_TARGET_BANK2;
    }

    command.CommandMode            = cmd;         /* 命令 */
    command.CommandTarget          = target_bank; /* 目标SDRAM存储区域 */
    command.AutoRefreshNumber      = refresh;     /* 自刷新次数 */
    command.ModeRegisterDefinition = regval;      /* 要写入模式寄存器的值 */

    if (HAL_SDRAM_SendCommand(&g_sdram_handle, &command, 0X1000) == HAL_OK) /* 向SDRAM发送命令 */
    {
        return FUNC_STATUS_SUCCESS;
    }
    else
    {
        return FUNC_STATUS_ERROR;
    }
}

/**
 * @brief       在 SDRAM_BASE_ADDR + writeaddr 开始连续写入 n 个字节
 * @param       pbuf      : 字节指针
 * @param       writeaddr : 要写入的地址
 * @param       n         : 要写入的字节数
 * @retval      无
 */
void fmc_sdram_write_buffer(uint8_t *pbuf, uint32_t writeaddr, uint32_t n)
{
    for (; n != 0; n--)
    {
        *(volatile uint8_t *)(SDRAM_BASE_ADDR + writeaddr) = *pbuf;
        writeaddr++;
        pbuf++;
    }
}

/**
 * @brief       从 SDRAM_BASE_ADDR + readaddr 开始连续读出 n 个字节
 * @param       pbuf     : 字节指针
 * @param       readaddr : 要读出的起始地址
 * @param       n        : 要写入的字节数
 * @retval      无
 */
void fmc_sdram_read_buffer(uint8_t *pbuf, uint32_t readaddr, uint32_t n)
{
    for (; n != 0; n--)
    {
        *pbuf++ = *(volatile uint8_t *)(SDRAM_BASE_ADDR + readaddr);
        readaddr++;
    }
}

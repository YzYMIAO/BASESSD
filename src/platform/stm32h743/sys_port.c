/*
 * sys_port.c — stm32h743 系统服务适配层
 *
 * 只做硬件相关的事：MPU 区域表、Cache/VTOR/WFI/PRIMASK/MSP 的内核内建、
 * RCC/PLL 时钟树、HAL 初始化，以及 USE_FULL_ASSERT 钩子。启动编排与 sys_*
 * 转发都在共享 src/platform/sys.c；本文件只实现 sys_port_* 接口。
 */

#include "sys_port.h"

#include "common/common.h"

static const MPU_Region_InitTypeDef g_sys_mpu_regions[] = {
  /* Flash: executable read-only firmware. */
    {.Enable = MPU_REGION_ENABLE,  .Number = 0U, .BaseAddress = 0x08000000U,   .Size = MPU_REGION_SIZE_2MB, .SubRegionDisable = 0U, .TypeExtField = MPU_TEX_LEVEL0, .AccessPermission = MPU_REGION_PRIV_RO_URO,  .DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE, .IsShareable = MPU_ACCESS_NOT_SHAREABLE,     .IsCacheable = MPU_ACCESS_CACHEABLE,     .IsBufferable = MPU_ACCESS_BUFFERABLE},
 /* The two TCM blocks connect directly to the Cortex-M7. */
    {.Enable = MPU_REGION_ENABLE,  .Number = 1U, .BaseAddress = 0x00000000U,  .Size = MPU_REGION_SIZE_64KB, .SubRegionDisable = 0U, .TypeExtField = MPU_TEX_LEVEL0, .AccessPermission = MPU_REGION_FULL_ACCESS,  .DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE, .IsShareable = MPU_ACCESS_NOT_SHAREABLE,     .IsCacheable = MPU_ACCESS_CACHEABLE,     .IsBufferable = MPU_ACCESS_BUFFERABLE},
    {.Enable = MPU_REGION_ENABLE,  .Number = 2U, .BaseAddress = 0x20000000U, .Size = MPU_REGION_SIZE_128KB, .SubRegionDisable = 0U, .TypeExtField = MPU_TEX_LEVEL0, .AccessPermission = MPU_REGION_FULL_ACCESS, .DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE, .IsShareable = MPU_ACCESS_NOT_SHAREABLE,     .IsCacheable = MPU_ACCESS_CACHEABLE,     .IsBufferable = MPU_ACCESS_BUFFERABLE},
 /* AXI SRAM includes normal .data/.bss placement. */
    {.Enable = MPU_REGION_ENABLE,  .Number = 3U, .BaseAddress = 0x24000000U, .Size = MPU_REGION_SIZE_512KB, .SubRegionDisable = 0U, .TypeExtField = MPU_TEX_LEVEL0, .AccessPermission = MPU_REGION_FULL_ACCESS, .DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE, .IsShareable = MPU_ACCESS_NOT_SHAREABLE,     .IsCacheable = MPU_ACCESS_CACHEABLE,     .IsBufferable = MPU_ACCESS_BUFFERABLE},
 /* D2 domain SRAM1, SRAM2 and SRAM3. */
    {.Enable = MPU_REGION_ENABLE,  .Number = 4U, .BaseAddress = 0x30000000U, .Size = MPU_REGION_SIZE_128KB, .SubRegionDisable = 0U, .TypeExtField = MPU_TEX_LEVEL0, .AccessPermission = MPU_REGION_FULL_ACCESS, .DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE, .IsShareable = MPU_ACCESS_NOT_SHAREABLE,     .IsCacheable = MPU_ACCESS_CACHEABLE,     .IsBufferable = MPU_ACCESS_BUFFERABLE},
    {.Enable = MPU_REGION_ENABLE,  .Number = 5U, .BaseAddress = 0x30020000U, .Size = MPU_REGION_SIZE_128KB, .SubRegionDisable = 0U, .TypeExtField = MPU_TEX_LEVEL0, .AccessPermission = MPU_REGION_FULL_ACCESS, .DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE, .IsShareable = MPU_ACCESS_NOT_SHAREABLE,     .IsCacheable = MPU_ACCESS_CACHEABLE,     .IsBufferable = MPU_ACCESS_BUFFERABLE},
    {.Enable = MPU_REGION_ENABLE,  .Number = 6U, .BaseAddress = 0x30040000U,  .Size = MPU_REGION_SIZE_32KB, .SubRegionDisable = 0U, .TypeExtField = MPU_TEX_LEVEL0, .AccessPermission = MPU_REGION_FULL_ACCESS, .DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE, .IsShareable = MPU_ACCESS_NOT_SHAREABLE,     .IsCacheable = MPU_ACCESS_CACHEABLE,     .IsBufferable = MPU_ACCESS_BUFFERABLE},
 /* D3 SRAM4. Backup SRAM uses the privileged default mapping. */
    {.Enable = MPU_REGION_ENABLE,  .Number = 7U, .BaseAddress = 0x38000000U,  .Size = MPU_REGION_SIZE_64KB, .SubRegionDisable = 0U, .TypeExtField = MPU_TEX_LEVEL0, .AccessPermission = MPU_REGION_FULL_ACCESS, .DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE, .IsShareable = MPU_ACCESS_NOT_SHAREABLE,     .IsCacheable = MPU_ACCESS_CACHEABLE,     .IsBufferable = MPU_ACCESS_BUFFERABLE},
 /* Peripheral window: non-cacheable, ordered, and never executable. */
    {.Enable = MPU_REGION_ENABLE,  .Number = 8U, .BaseAddress = 0x40000000U, .Size = MPU_REGION_SIZE_512MB, .SubRegionDisable = 0U, .TypeExtField = MPU_TEX_LEVEL0, .AccessPermission = MPU_REGION_FULL_ACCESS, .DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE,     .IsShareable = MPU_ACCESS_SHAREABLE, .IsCacheable = MPU_ACCESS_NOT_CACHEABLE,     .IsBufferable = MPU_ACCESS_BUFFERABLE},
 /* FMC aperture stays Device/non-cacheable until the external bus is initialized. */
    {.Enable = MPU_REGION_ENABLE,  .Number = 9U, .BaseAddress = 0x60000000U,  .Size = MPU_REGION_SIZE_64MB, .SubRegionDisable = 0U, .TypeExtField = MPU_TEX_LEVEL0, .AccessPermission = MPU_REGION_FULL_ACCESS, .DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE,     .IsShareable = MPU_ACCESS_SHAREABLE, .IsCacheable = MPU_ACCESS_NOT_CACHEABLE, .IsBufferable = MPU_ACCESS_NOT_BUFFERABLE},
 /* FMC NAND Bank3: command, address and data windows are at 0x80000000. Never cache bus cycles. */
    {.Enable = MPU_REGION_ENABLE, .Number = 10U, .BaseAddress = 0x80000000U,  .Size = MPU_REGION_SIZE_64MB, .SubRegionDisable = 0U, .TypeExtField = MPU_TEX_LEVEL0, .AccessPermission = MPU_REGION_FULL_ACCESS, .DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE,     .IsShareable = MPU_ACCESS_SHAREABLE, .IsCacheable = MPU_ACCESS_NOT_CACHEABLE, .IsBufferable = MPU_ACCESS_NOT_BUFFERABLE},
 /* Higher-numbered overlay for linker AXI_NOCACHE: 0x24060000..0x2407FFFF. */
    {.Enable = MPU_REGION_ENABLE, .Number = 11U, .BaseAddress = 0x24060000U, .Size = MPU_REGION_SIZE_128KB, .SubRegionDisable = 0U, .TypeExtField = MPU_TEX_LEVEL1, .AccessPermission = MPU_REGION_FULL_ACCESS, .DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE,     .IsShareable = MPU_ACCESS_SHAREABLE, .IsCacheable = MPU_ACCESS_NOT_CACHEABLE, .IsBufferable = MPU_ACCESS_NOT_BUFFERABLE},
};

void sys_port_mpu_config(void)
{
    uint32_t index;
    HAL_MPU_Disable();
    for (index = 0U; index < (sizeof(g_sys_mpu_regions) / sizeof(g_sys_mpu_regions[0])); ++index)
    {
        HAL_MPU_ConfigRegion(&g_sys_mpu_regions[index]);
    }
    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

void sys_port_cache_enable(void)
{
    SCB_EnableICache();
    SCB_EnableDCache();
}

uint8_t sys_port_icache_enabled(void)
{
    return (uint8_t)((SCB->CCR >> 17U) & 1U);
}

uint8_t sys_port_dcache_enabled(void)
{
    return (uint8_t)((SCB->CCR >> 16U) & 1U);
}

uint32_t sys_port_mpu_region_count(void)
{
    return (uint32_t)(sizeof(g_sys_mpu_regions) / sizeof(g_sys_mpu_regions[0]));
}

/* per_ck：FMC / QSPI 的 CLKP 备选源（HSI/CSI/HSE）。 */
static uint32_t sys_port_per_ck_hz(void)
{
    switch (__HAL_RCC_GET_CLKP_SOURCE())
    {
        case RCC_CLKPSOURCE_HSI:
            return HSI_VALUE;
        case RCC_CLKPSOURCE_CSI:
            return CSI_VALUE;
        case RCC_CLKPSOURCE_HSE:
            return HSE_VALUE;
        default:
            return 0U;
    }
}

/*
 * 时钟树回读：sysclk/hclk/pclk1..4 由 HAL 直接给出，读的是「芯片实际处于什么
 * 状态」而非本文件的配置常量。
 *
 * FMC / QSPI / USART1 三项 HAL 不给：本 HAL 的 HAL_RCCEx_GetPeriphCLKFreq 只覆盖
 * ADC/FDCAN/SAI/SDMMC/SPI，对其余外设（含这三个）一律返回 0。故这里读各自的
 * mux 位、再用 HAL 的 PLL getter 换算 —— 只做 mux 译码，不重算 PLL 参数。
 */
void sys_port_clock_tree_get(sys_clock_tree_t *out)
{
    PLL1_ClocksTypeDef pll1 = {0};
    PLL2_ClocksTypeDef pll2 = {0};
    PLL3_ClocksTypeDef pll3 = {0};
    uint32_t           hclk;
    uint32_t           per_ck;

    HAL_RCCEx_GetPLL1ClockFreq(&pll1);
    HAL_RCCEx_GetPLL2ClockFreq(&pll2);
    HAL_RCCEx_GetPLL3ClockFreq(&pll3);
    hclk   = HAL_RCC_GetHCLKFreq();
    per_ck = sys_port_per_ck_hz();

    out->sysclk_hz = HAL_RCC_GetSysClockFreq();
    out->hclk_hz   = hclk;
    out->pclk1_hz  = HAL_RCC_GetPCLK1Freq();
    out->pclk2_hz  = HAL_RCC_GetPCLK2Freq();
    out->pclk3_hz  = HAL_RCCEx_GetD1PCLK1Freq();
    out->pclk4_hz  = HAL_RCCEx_GetD3PCLK1Freq();

    switch (__HAL_RCC_GET_FMC_SOURCE())
    {
        case RCC_FMCCLKSOURCE_D1HCLK:
            out->fmc_hz = hclk;
            break;
        case RCC_FMCCLKSOURCE_PLL:
            out->fmc_hz = pll1.PLL1_Q_Frequency;
            break;
        case RCC_FMCCLKSOURCE_PLL2:
            out->fmc_hz = pll2.PLL2_R_Frequency;
            break;
        case RCC_FMCCLKSOURCE_CLKP:
            out->fmc_hz = per_ck;
            break;
        default:
            out->fmc_hz = 0U;
            break;
    }

    switch (__HAL_RCC_GET_QSPI_SOURCE())
    {
        case RCC_QSPICLKSOURCE_D1HCLK:
            out->qspi_hz = hclk;
            break;
        case RCC_QSPICLKSOURCE_PLL:
            out->qspi_hz = pll1.PLL1_Q_Frequency;
            break;
        case RCC_QSPICLKSOURCE_PLL2:
            out->qspi_hz = pll2.PLL2_R_Frequency;
            break;
        case RCC_QSPICLKSOURCE_CLKP:
            out->qspi_hz = per_ck;
            break;
        default:
            out->qspi_hz = 0U;
            break;
    }

    switch (__HAL_RCC_GET_USART1_SOURCE())
    {
        case RCC_USART1CLKSOURCE_D2PCLK2:
            out->uart_hz = out->pclk2_hz;
            break;
        case RCC_USART1CLKSOURCE_PLL2:
            out->uart_hz = pll2.PLL2_Q_Frequency;
            break;
        case RCC_USART1CLKSOURCE_PLL3:
            out->uart_hz = pll3.PLL3_Q_Frequency;
            break;
        case RCC_USART1CLKSOURCE_HSI:
            out->uart_hz = HSI_VALUE;
            break;
        case RCC_USART1CLKSOURCE_CSI:
            out->uart_hz = CSI_VALUE;
            break;
        case RCC_USART1CLKSOURCE_LSE:
            out->uart_hz = LSE_VALUE;
            break;
        default:
            out->uart_hz = 0U;
            break;
    }
}

void sys_port_set_vector_table(uint32_t base_address, uint32_t offset)
{
    SCB->VTOR = base_address | (offset & 0xFFFFFE00UL);
}

void sys_port_wfi(void)
{
    __WFI();
}

void sys_port_irq_disable(void)
{
    __disable_irq();
}

void sys_port_irq_enable(void)
{
    __enable_irq();
}

void sys_port_set_msp(uint32_t address)
{
    __set_MSP(address);
}

void sys_port_hal_init(void)
{
    HAL_Init();
}

/*
 * Apollo V2 时钟总览（HSE=25 MHz）：
 *   PLL1 VCO = 25 / PLL1M(5) * PLL1N(160) = 800 MHz
 *   PLL1P    = 2 -> SYSCLK/CPU = 400 MHz
 *   AHB 分频 = 2 -> HCLK/D1HCLK/D2HCLK = 200 MHz
 *   APB1/2/3/4 分频 = 2 -> PCLK1/2/3/4 = 100 MHz
 *   APB 分频不为 1 时，挂在 APB1/2 上的定时器内核时钟为 2*PCLK = 200 MHz
 *   PLL1Q = 4 -> PLL1_Q = 200 MHz（QSPI、RNG 等外设时钟源）
 *   PLL2R = 2 -> PLL2_R = 220 MHz（FMC 时钟源；SDRAM SDCLK=220/2=110 MHz）
 */

uint8_t sys_port_clock_init(uint32_t pll_prediv, uint32_t pll_mul, uint32_t sysclk_div, uint32_t peripheral_div)
{
    RCC_OscInitTypeDef       oscillator        = {0};
    RCC_ClkInitTypeDef       clocks            = {0};
    RCC_PeriphCLKInitTypeDef peripheral_clocks = {0};

    /* 电源：VOS1，允许 CPU 运行到 400 MHz。 */
    MODIFY_REG(PWR->CR3, PWR_CR3_SCUEN, 0U);
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
    while ((PWR->D3CR & PWR_D3CR_VOSRDY) != PWR_D3CR_VOSRDY)
    {
    }

    /* HSE=25 MHz 作为 PLL1/PLL2 输入；HSI48=48 MHz 供 USB 等外设使用。 */
    oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSE | RCC_OSCILLATORTYPE_HSI48;
    oscillator.HSEState       = RCC_HSE_ON;
    oscillator.HSIState       = RCC_HSI_OFF;
    oscillator.CSIState       = RCC_CSI_OFF;
    oscillator.HSI48State     = RCC_HSI48_ON;
    /* RCC_OscInitTypeDef.PLL 对应硬件 PLL1。以下参数得到 PLL1P=400 MHz。 */
    oscillator.PLL.PLLState   = RCC_PLL_ON;
    oscillator.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    oscillator.PLL.PLLM       = pll_prediv;     /* 预分频：25/5 = 5 MHz，PLL 输入频率 */
    oscillator.PLL.PLLN       = pll_mul;        /* 倍频：5*160 = 800 MHz，PLL1 VCO */
    oscillator.PLL.PLLP       = sysclk_div;     /* SYSCLK 分频：800/2 = 400 MHz，CPU 时钟 */
    oscillator.PLL.PLLQ       = peripheral_div; /* 外设分频：800/4 = 200 MHz，PLL1_Q */
    oscillator.PLL.PLLR       = 2U;
    oscillator.PLL.PLLRGE     = RCC_PLL1VCIRANGE_2;
    oscillator.PLL.PLLVCOSEL  = RCC_PLL1VCOWIDE;
    oscillator.PLL.PLLFRACN   = 0U;
    if (HAL_RCC_OscConfig(&oscillator) != HAL_OK)
    {
        return FUNC_STATUS_ERROR;
    }

    clocks.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 |
                       RCC_CLOCKTYPE_PCLK2 | RCC_CLOCKTYPE_D1PCLK1 | RCC_CLOCKTYPE_D3PCLK1;
    clocks.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK; /* SYSCLK=400 MHz */
    clocks.SYSCLKDivider  = RCC_SYSCLK_DIV1;         /* CPU/CM7=400 MHz */
    clocks.AHBCLKDivider  = RCC_HCLK_DIV2;           /* HCLK=200 MHz */
    clocks.APB1CLKDivider = RCC_APB1_DIV2;           /* PCLK1=100 MHz；TIM2/3/4/5/6/7 内核=200 MHz */
    clocks.APB2CLKDivider = RCC_APB2_DIV2;           /* PCLK2=100 MHz；TIM1/8/15/16/17 内核=200 MHz */
    clocks.APB3CLKDivider = RCC_APB3_DIV2;           /* PCLK3=100 MHz */
    clocks.APB4CLKDivider = RCC_APB4_DIV2;           /* PCLK4=100 MHz */
    if (HAL_RCC_ClockConfig(&clocks, FLASH_LATENCY_2) != HAL_OK)
    {
        return FUNC_STATUS_ERROR;
    }

    /*
     * PLL2 专门给 FMC：25/25*440=440 MHz VCO，PLL2_R=2，
     * 所以 FMC kernel clock=220 MHz；SDRAM 配置 SDCLK=2，最终为 110 MHz。
     */
    peripheral_clocks.PeriphClockSelection = RCC_PERIPHCLK_FMC;
    peripheral_clocks.FmcClockSelection    = RCC_FMCCLKSOURCE_PLL2; /* FMC=220 MHz */
    peripheral_clocks.PLL2.PLL2M           = 25U;
    peripheral_clocks.PLL2.PLL2N           = 440U;
    peripheral_clocks.PLL2.PLL2P           = 2U;
    peripheral_clocks.PLL2.PLL2Q           = 2U;
    peripheral_clocks.PLL2.PLL2R           = 2U;
    peripheral_clocks.PLL2.PLL2RGE         = RCC_PLL2VCIRANGE_0;
    peripheral_clocks.PLL2.PLL2VCOSEL      = RCC_PLL2VCOWIDE;
    peripheral_clocks.PLL2.PLL2FRACN       = 0U;
    if (HAL_RCCEx_PeriphCLKConfig(&peripheral_clocks) != HAL_OK)
    {
        return FUNC_STATUS_ERROR;
    }

    (void)HAL_PWREx_EnableUSBVoltageDetector();
    /*
     * Crash Records 通过 BKPSRAM（备份 SRAM）在软复位后保留。
     * 要使 VDD 掉电后数据不丢，需要 VBAT 引脚供电并打开备份稳压器。
     * 当前核心板仅通过 SWD 供电（VBAT 悬空），备份稳压器永远无法就绪，
     * 使能它会导致卡死。调试阶段跳过。
     *
     *   __HAL_RCC_BKPRAM_CLK_ENABLE();
     *   if (HAL_PWREx_EnableBkUpReg() != HAL_OK) { return FUNC_STATUS_ERROR; }
     */
    __HAL_RCC_CSI_ENABLE();
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    HAL_EnableCompensationCell();
    return FUNC_STATUS_SUCCESS;
}

/* ─────────────────────────── 向量表入口 ─────────────────────────── */

/*
 * SysTick 中断入口（向量表名由 startup_stm32h743xx.s 固定）。
 * HAL_Init() 把 SysTick 配成 1 ms 节拍，本文件即该向量的归属地。
 */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
    while (1)
    {
    }
}
#endif

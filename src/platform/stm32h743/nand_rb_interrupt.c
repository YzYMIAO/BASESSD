#include "nand_rb_interrupt.h"

#include "stm32h7xx_hal.h"
#include "stm32h743xx.h"

#include <stddef.h>

/*
 * NAND R/B# 中断驱动实现。
 *
 * PD6 连接 NAND R/B# 引脚，配置为 EXTI6 上升沿触发。
 * EXTI6 属于共享中断线组 EXTI9_5，中断入口为 EXTI9_5_IRQn。
 *
 * 工作流程：
 *   1. nand_rb_interrupt_arm(timeout_us): 配置 EXTI6 上升沿，清 pending，启中断。
 *      如果此时 RB 引脚已为高电平（NAND 已空闲），直接标记 triggered=1，
 *      避免上升沿永不发生导致的超时等待。
 *   2. nand_engine_step() 遇到 NAND_UOP_WAIT_READY 时调用 nand_fmc_exec_wait_ready()，
 *      handler 通过本模块判断三种状态：
 *      - triggered == 1  → NAND 就绪
 *      - timed_out == 1  → 超时，回退 GPIO 轮询最后一次确认
 *      - otherwise       → 继续等 step 轮询
 *   3. 超时后 handler 通过 GPIO 轮询回退做最后一次确认，避免中断丢失导致的误超时。
 *
 * 当前实现使用 DWT 计数器做超时检测。
 */

static volatile struct {
    uint8_t  armed;           /* 已 arm，等待触发或超时 */
    uint8_t  triggered;       /* EXTI6 上升沿已触发 */
    uint32_t arm_cycles;      /* arm 时的 DWT 计数值 */
    uint32_t timeout_us;      /* 超时微秒数 */
} g_rb_int;

/* DWT 时钟周期计数（在 delay.h 中已声明） */
extern uint32_t g_dwt_cyccnt_per_us;

void nand_rb_interrupt_init(void)
{
    g_rb_int.armed = 0U;
    g_rb_int.triggered = 0U;
    g_rb_int.arm_cycles = 0U;
    g_rb_int.timeout_us = 0U;

    /* PD6 已由 HAL_NAND_MspInit 配置为 GPIO 输入（上拉）。
     * 这里将 PD6 连接到 EXTI6。 */

    /* 启用 SYSCFG 时钟（如果尚未启用） */
    __HAL_RCC_SYSCFG_CLK_ENABLE();

    /* 将 EXTI6 连接到 PD6 (SYSCFG_EXTICR2_EXTI6_PD) */
    SYSCFG->EXTICR[1] = (SYSCFG->EXTICR[1] & ~SYSCFG_EXTICR2_EXTI6_Msk)
                        | SYSCFG_EXTICR2_EXTI6_PD;

    /* 配置 EXTI6 上升沿触发 */
    EXTI->RTSR1 |= EXTI_RTSR1_TR6;

    /* 清除 EXTI6 pending 标志 */
    EXTI->PR1 = EXTI_PR1_PR6;

    /* 设置中断优先级（较低数字 = 较高优先级，与 UART/DMA 一致） */
    HAL_NVIC_SetPriority(EXTI9_5_IRQn, 4U, 0U);
    /* 注意：不在这里使能 NVIC——在 arm() 中使能，避免误触发 */
}

void nand_rb_interrupt_arm(uint32_t timeout_us)
{
    g_rb_int.armed      = 1U;
    g_rb_int.triggered  = 0U;
    g_rb_int.timeout_us = timeout_us;
    g_rb_int.arm_cycles = 0U;

    /* 记录 arm 时的 DWT 计数值（使用 delay_dwt_cycles_now 类似方法） */
    if (g_dwt_cyccnt_per_us != 0U)
    {
        g_rb_int.arm_cycles = *((volatile uint32_t *)0xE0001004UL);  /* DWT_CYCCNT */
    }

    /* 清除可能残留的 pending 标志 */
    EXTI->PR1 = EXTI_PR1_PR6;

    /* 使能 EXTI9_5 中断组 */
    EXTI->IMR1 |= EXTI_IMR1_IM6;
    HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);

    /*
     * ★ tWB 等待 + GPIO 确认
     *
     * 刚发完 busy-making 命令（D0h/10h/30h 等）后，NAND 需要最长 1us（tWB）
     * 才能将 R/B# 拉低。如果不等待直接读引脚，NAND 尚未拉低，引脚读回 HIGH，
     * 短路条件成立 → 立即标记 triggered=1 → WAIT_READY 立即返回 → 实际并未等待。
     *
     * 这里用 DWT 自旋等待 1us（tWB max），确保 NAND 有足够时间拉低 R/B#，
     * 然后再检查 GPIO。如果仍然 HIGH，说明 NAND 确实空闲，可以短路。
     */
    if (g_dwt_cyccnt_per_us != 0U)
    {
        uint32_t start       = *((volatile uint32_t *)0xE0001004UL);
        uint32_t delay_cycle = (uint32_t)g_dwt_cyccnt_per_us * 1U;  /* 1 us */
        while ((*((volatile uint32_t *)0xE0001004UL) - start) < delay_cycle) {}
    }

    /* ★ RB 已经高电平 → 上升沿永远不会来，直接标记已触发 */
    if (HAL_GPIO_ReadPin(GPIOD, GPIO_PIN_6) == GPIO_PIN_SET)
    {
        g_rb_int.triggered = 1U;
    }
}

void nand_rb_interrupt_disarm(void)
{
    /* 禁用 EXTI9_5 中断 */
    HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);
    EXTI->IMR1 &= ~EXTI_IMR1_IM6;

    /* 清除 pending */
    EXTI->PR1 = EXTI_PR1_PR6;

    g_rb_int.armed     = 0U;
    g_rb_int.triggered = 0U;
}

uint8_t nand_rb_interrupt_armed(void)
{
    return g_rb_int.armed;
}

uint8_t nand_rb_interrupt_triggered(void)
{
    return g_rb_int.triggered;
}

uint8_t nand_rb_interrupt_timed_out(void)
{
    uint32_t elapsed_us;

    if ((g_rb_int.armed == 0U) || (g_rb_int.timeout_us == 0U) || (g_dwt_cyccnt_per_us == 0U))
    {
        return 0U;
    }

    elapsed_us = (*((volatile uint32_t *)0xE0001004UL) - g_rb_int.arm_cycles) / g_dwt_cyccnt_per_us;
    return (elapsed_us >= g_rb_int.timeout_us) ? 1U : 0U;
}

void nand_rb_interrupt_handler(void)
{
    /* 检查 EXTI6 是否触发 */
    if ((EXTI->PR1 & EXTI_PR1_PR6) != 0U)
    {
        /* 清除 pending 标志 */
        EXTI->PR1 = EXTI_PR1_PR6;

        /* 标记触发 */
        g_rb_int.triggered = 1U;
    }
}
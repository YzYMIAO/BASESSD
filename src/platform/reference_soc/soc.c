#include "platform/soc.h"

static volatile uint64_t g_reference_time_us;

void soc_early_init(void)
{
    /* Clock, TCM and watchdog setup belongs to a future real SoC BSP. */
}

void soc_memory_init(void)
{
    /* A debugger or simulator is assumed to have initialized executable RAM. */
}

void soc_interrupt_init(void)
{
    /* The reference platform has no physical interrupt controller. */
}

void soc_timer_init(void)
{
    g_reference_time_us = 0U;
}

void soc_uart_init(void)
{
}

void soc_uart_write(const char *message)
{
    (void)message;
}

uint64_t soc_time_us(void)
{
    return g_reference_time_us;
}

#include "platform/interrupt.h"

#include <stddef.h>

#define REFERENCE_SOC_IRQ_COUNT 64U

typedef struct
{
    ssd_irq_handler_t handler;
    void             *context;
    int               enabled;
} reference_irq_entry_t;

static reference_irq_entry_t g_irq_table[REFERENCE_SOC_IRQ_COUNT];

int ssd_irq_register(uint32_t irq, ssd_irq_handler_t handler, void *context)
{
    if ((irq >= REFERENCE_SOC_IRQ_COUNT) || (handler == NULL))
    {
        return -1;
    }

    g_irq_table[irq].handler = handler;
    g_irq_table[irq].context = context;
    return 0;
}

void ssd_irq_enable(uint32_t irq)
{
    if (irq < REFERENCE_SOC_IRQ_COUNT)
    {
        g_irq_table[irq].enabled = 1;
    }
}

void ssd_irq_disable(uint32_t irq)
{
    if (irq < REFERENCE_SOC_IRQ_COUNT)
    {
        g_irq_table[irq].enabled = 0;
    }
}

void ssd_irq_dispatch(void)
{
    /* A real BSP reads the active IRQ ID and invokes the matching entry. */
}

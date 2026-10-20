#ifndef SSD_PLATFORM_INTERRUPT_H
#define SSD_PLATFORM_INTERRUPT_H

#include <stdint.h>

typedef void (*ssd_irq_handler_t)(void *context);

int  ssd_irq_register(DWORD irq, ssd_irq_handler_t handler, void *context);
void ssd_irq_enable(DWORD irq);
void ssd_irq_disable(DWORD irq);
void ssd_irq_dispatch(void);

#endif /* SSD_PLATFORM_INTERRUPT_H */

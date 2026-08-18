#ifndef SSD_PLATFORM_SOC_H
#define SSD_PLATFORM_SOC_H

#include <stdint.h>

/* Hooks implemented by the selected SoC BSP. */
void soc_early_init(void);
void soc_memory_init(void);
void soc_interrupt_init(void);
void soc_timer_init(void);
void soc_uart_init(void);
void soc_uart_write(const char *message);
uint64_t soc_time_us(void);

#endif /* SSD_PLATFORM_SOC_H */

#ifndef SSD_PLATFORM_CPU_ARMV7R_H
#define SSD_PLATFORM_CPU_ARMV7R_H

#include <stdint.h>

void armv7r_set_vector_base(uint32_t address);
void armv7r_data_sync_barrier(void);
void armv7r_instruction_sync_barrier(void);
void armv7r_wait_for_interrupt(void);

#endif /* SSD_PLATFORM_CPU_ARMV7R_H */

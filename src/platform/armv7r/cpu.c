#include "platform/cpu_armv7r.h"

void armv7r_set_vector_base(uint32_t address)
{
    __asm volatile("mcr p15, 0, %0, c12, c0, 0" : : "r"(address) : "memory");
    armv7r_instruction_sync_barrier();
}

void armv7r_data_sync_barrier(void)
{
    __asm volatile("dsb" ::: "memory");
}

void armv7r_instruction_sync_barrier(void)
{
    __asm volatile("isb" ::: "memory");
}

void armv7r_wait_for_interrupt(void)
{
    __asm volatile("wfi");
}

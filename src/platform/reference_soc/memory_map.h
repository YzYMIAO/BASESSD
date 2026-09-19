#ifndef SSD_REFERENCE_SOC_MEMORY_MAP_H
#define SSD_REFERENCE_SOC_MEMORY_MAP_H

/* Repository-owned virtual memory contract, not addresses of a real chip. */
#define REF_SOC_BOOT_MEMORY_BASE 0x00000000U
#define REF_SOC_BOOT_MEMORY_SIZE 0x00200000U
#define REF_SOC_DTCM_BASE        0x20000000U
#define REF_SOC_DTCM_SIZE        0x00010000U
#define REF_SOC_SRAM_BASE        0x21000000U
#define REF_SOC_SRAM_SIZE        0x00080000U
#define REF_SOC_DRAM_BASE        0x40000000U
#define REF_SOC_DRAM_SIZE        0x10000000U

#endif /* SSD_REFERENCE_SOC_MEMORY_MAP_H */

/*
 * STM32H743xI bare-metal C linker script.
 *
 * This layout targets the 2 MiB Flash H743xI variants.  For an H743xG
 * (1 MiB Flash), change __FLASH_SIZE to 0x00100000.
 */

ENTRY(Reset_Handler)

__FLASH_BEGIN = 0x08000000;
__FLASH_SIZE = 0x00200000;

/* Keep the final 128 KiB of AXI SRAM for DMA/NAND buffers marked non-cacheable. */
__AXI_RAM_BEGIN = 0x24000000;
__AXI_RAM_SIZE = 0x00060000;
__AXI_NOCACHE_BEGIN = 0x24060000;
__AXI_NOCACHE_SIZE = 0x00020000;

_Min_Heap_Size = 0x200;
_Min_Stack_Size = 0x800;

MEMORY
{
  FLASH       (rx)  : ORIGIN = __FLASH_BEGIN,   LENGTH = __FLASH_SIZE
  ITCM        (xrw) : ORIGIN = 0x00000000,      LENGTH = 0x00010000
  DTCM        (xrw) : ORIGIN = 0x20000000,      LENGTH = 0x00020000
  AXI_RAM     (xrw) : ORIGIN = __AXI_RAM_BEGIN, LENGTH = __AXI_RAM_SIZE
  AXI_NOCACHE (xrw) : ORIGIN = __AXI_NOCACHE_BEGIN, LENGTH = __AXI_NOCACHE_SIZE
  /* SRAM1+SRAM2 连续映射（0x30000000..0x3003FFFF）：FTL tables 跨两 bank 使用。 */
  SRAM12      (xrw) : ORIGIN = 0x30000000,      LENGTH = 0x00040000
  SRAM3       (xrw) : ORIGIN = 0x30040000,      LENGTH = 0x00008000
  SRAM4       (xrw) : ORIGIN = 0x38000000,      LENGTH = 0x00010000
  BKPSRAM     (xrw) : ORIGIN = 0x38800000,      LENGTH = 0x00001000
}

_estack = ORIGIN(DTCM) + LENGTH(DTCM);

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } > FLASH

  /* Startup code must copy this range from Flash before calling it. */
  .ssd_code_high :
  {
    . = ALIGN(4);
    __ssd_code_high_start = .;
    KEEP(*(.ssd_code_high))
    KEEP(*(.ssd_code_high.*))
    . = ALIGN(4);
    __ssd_code_high_end = .;
  } > ITCM AT > FLASH
  __ssd_code_high_load_start = LOADADDR(.ssd_code_high);

  .ssd_code_fast :
  {
    . = ALIGN(4);
    KEEP(*(.ssd_code_fast))
    KEEP(*(.ssd_code_fast.*))
    . = ALIGN(4);
  } > FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    . = ALIGN(4);
    _etext = .;
  } > FLASH

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } > FLASH

  _sidata = LOADADDR(.data);
  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    *(.RamFunc)
    *(.RamFunc*)
    . = ALIGN(4);
    _edata = .;
  } > AXI_RAM AT > FLASH

  /* Non-cacheable DMA/NAND buffers in AXI_NOCACHE (MPU-disabled DCache). */
  .noncacheable_buffer (NOLOAD) :
  {
    . = ALIGN(32);
    __NONCACHEABLEBUFFER_BEGIN = .;
    KEEP(*(noncacheable_buffer))
    KEEP(*(noncacheable_buffer.*))
    . = ALIGN(32);
    __NONCACHEABLEBUFFER_END = .;
  } > AXI_NOCACHE

  .bss (NOLOAD) :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = .;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = .;
  } > AXI_RAM

  /* Put DMA buffers declared with section("noncacheable_buffer") here. */
  .noncacheable_buffer (NOLOAD) :
  {
    . = ALIGN(32);
    __NONCACHEABLEBUFFER_BEGIN = .;
    KEEP(*(noncacheable_buffer))
    KEEP(*(noncacheable_buffer.*))
    . = ALIGN(32);
    __NONCACHEABLEBUFFER_END = .;
  } > AXI_NOCACHE

  /* Optional explicit placement sections; NOLOAD data is not initialized. */
  .dtcm_data (NOLOAD) : { . = ALIGN(8); KEEP(*(.dtcm_data)) KEEP(*(.dtcm_data.*)) } > DTCM
  .sram12_data (NOLOAD) : { . = ALIGN(32); KEEP(*(.sram12_data)) KEEP(*(.sram12_data.*)) } > SRAM12
  .sram3_data (NOLOAD) : { . = ALIGN(32); KEEP(*(.sram3_data)) KEEP(*(.sram3_data.*)) } > SRAM3
  .sram4_data (NOLOAD) : { . = ALIGN(32); KEEP(*(.sram4_data)) KEEP(*(.sram4_data.*)) } > SRAM4
  .bkpsram_data (NOLOAD) : { . = ALIGN(4); *(.bkpsram_data) *(.bkpsram_data.*) } > BKPSRAM

  /* NOLOAD 且不被启动代码清零 → 跨软复位（NVIC_SystemReset）保留。
   * 崩溃记录落在这里，见 docs/design/fault_capture.md。 */
  .noinit (NOLOAD) : { . = ALIGN(8); KEEP(*(.noinit)) KEEP(*(.noinit.*)) } > DTCM

  ._user_heap_stack (NOLOAD) :
  {
    . = ALIGN(8);
    PROVIDE(end = .);
    PROVIDE(_end = .);
    . += _Min_Heap_Size;
    . += _Min_Stack_Size;
    . = ALIGN(8);
  } > DTCM

  /DISCARD/ :
  {
    /* This firmware is C-only: reject C++ unwinding and constructor metadata. */
    *(.eh_frame*)
    *(.ARM.extab*)
    *(.ARM.exidx*)
    *(.preinit_array*)
    *(.init_array*)
    *(.fini_array*)
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}

ASSERT(__NONCACHEABLEBUFFER_END <= ORIGIN(AXI_NOCACHE) + LENGTH(AXI_NOCACHE),
       "AXI non-cacheable DMA buffer region overflow")
ASSERT(SIZEOF(.sram12_data) <= LENGTH(SRAM12), "SRAM12 (SRAM1+SRAM2) FTL tables overflow")
ASSERT(SIZEOF(.sram3_data) <= LENGTH(SRAM3), "SRAM3 FTL cp_job/journal overflow")
ASSERT(SIZEOF(.sram4_data) <= LENGTH(SRAM4), "SRAM4 FTL metadata dir overflow")

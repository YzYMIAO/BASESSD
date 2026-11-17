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
    /* Whole-module hot code: no per-function annotations. Selection is the host
     * data path (FTL submit/complete, L2P, wbuf/pending, cache read/write),
     * FC pump/BCH, and NAND dispatch/FMC.
     *
     * The common set below must fit the -O0 debug build (64 KB ITCM).  Release
     * adds the modules listed after it: their object paths contain
     * `release/obj/`, which the debug link does not, so the same script yields
     * a larger ITCM image only in release.  This is build-type selection by
     * input path, not a C preprocessor switch. */
    KEEP(*ftl/ftl.o(.text .text.*))
    KEEP(*ftl/l2p_cache.o(.text .text.*))
    KEEP(*ftl/wbuf.o(.text .text.*))
    KEEP(*ftl/pending.o(.text .text.*))
    KEEP(*ftl/cache_batch.o(.text .text.*))
    KEEP(*fc/fc.o(.text .text.*))
    KEEP(*fc/fc_data_path.o(.text .text.*))
    KEEP(*fc/fc_bch_engine.o(.text .text.*))
    KEEP(*nand/nand_dispatch.o(.text .text.*))
    KEEP(*nand/nand_fmc.o(.text .text.*))
    /* Release-only additions: write/GC/background path.  Keep this block
     * below ~62 KB total (release .ssd_code_high measured after every change);
     * debug is unaffected because its object directory is `debug/obj/`. */
    KEEP(*release/obj/ftl/allocator.o(.text .text.*))
    KEEP(*release/obj/ftl/journal.o(.text .text.*))
    KEEP(*release/obj/ftl/metadata.o(.text .text.*))
    KEEP(*release/obj/ftl/metadata_job.o(.text .text.*))
    KEEP(*release/obj/ftl/migrate.o(.text .text.*))
    KEEP(*release/obj/ftl/gc.o(.text .text.*))
    /* wear_leveling / bg_scheduler 是纯后台路径，放 Flash 运行；写回 CMT 让
     * l2p_cache.o 增大后，ITCM 优先留给 host 数据面。 */
    KEEP(*release/obj/ftl/ftl_op.o(.text .text.*))
    KEEP(*release/obj/fc/fc_interface.o(.text .text.*))
    KEEP(*release/obj/fc/fc_queue.o(.text .text.*))
    KEEP(*release/obj/fc/fc_command.o(.text .text.*))
    KEEP(*release/obj/nand/nand_engine.o(.text .text.*))
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
    KEEP(*(.noncacheable_buffer))
    KEEP(*(.noncacheable_buffer.*))
    KEEP(*(noncacheable_buffer))
    KEEP(*(noncacheable_buffer.*))
    . = ALIGN(32);
    __NONCACHEABLEBUFFER_END = .;
  } > AXI_NOCACHE

  /* DTCM hot data: whole-object placement (no per-variable annotations).
   * Must be before the generic .bss pattern, which would otherwise consume
   * these .bss.* input sections first. NOLOAD: init functions memset them. */
  .dtcm_data (NOLOAD) :
  {
    . = ALIGN(8);
    KEEP(*(.dtcm_data))
    KEEP(*(.dtcm_data.*))
    KEEP(*ftl/ftl.o(.bss .bss.*))
    /* metadata.o（g_ftl_md）与 BCH 交换域：g_ftl_md 回到基线 SRAM4，
     * BCH 只读查表放 DTCM。 */
    KEEP(*fc/fc.o(.bss .bss.*))
    KEEP(*nand/nand_dispatch.o(.bss .bss.*))
    KEEP(*fc/fc_bch_engine.o(.bss .bss.*))
    . = ALIGN(8);
  } > DTCM

  .sram4_data (NOLOAD) :
  {
    . = ALIGN(32);
    KEEP(*(.sram4_data))
    KEEP(*(.sram4_data.*))
    /* g_ftl_md 回到基线域（TCM 之前本就在 SRAM4，843 KB/s 已验证）。
     * 必须排在通用 `.bss` 之前，否则 metadata.o 的 `.bss` 会先被
     * `.bss` 的 `*(.bss*)` 吞掉。 */
    KEEP(*ftl/metadata.o(.bss .bss.*))
    . = ALIGN(32);
  } > SRAM4

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

  .sram12_data (NOLOAD) : { . = ALIGN(32); KEEP(*(.sram12_data)) KEEP(*(.sram12_data.*)) } > SRAM12
  .sram3_data (NOLOAD) : { . = ALIGN(32); KEEP(*(.sram3_data)) KEEP(*(.sram3_data.*)) } > SRAM3
  .bkpsram_data (NOLOAD) : { . = ALIGN(4); *(.bkpsram_data) *(.bkpsram_data.*) } > BKPSRAM

  /* NOLOAD 且不被启动代码清零 → 跨软复位（NVIC_SystemReset）保留。
   * 崩溃记录落在这里，见 docs/design/common/fault_capture.md。 */
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
ASSERT(SIZEOF(.dtcm_data) + SIZEOF(.noinit) + 0x5000 <= LENGTH(DTCM), "DTCM hot data + 20KB stack reserve overflow")
ASSERT(SIZEOF(.ssd_code_high) <= LENGTH(ITCM), "ITCM hot code overflow")
ASSERT((SIZEOF(.ssd_code_high) & 3) == 0, "ITCM copy loop requires 4-byte multiple size")
ASSERT(SIZEOF(.sram12_data) <= LENGTH(SRAM12), "SRAM12 (SRAM1+SRAM2) FTL tables overflow")
ASSERT(SIZEOF(.sram3_data) <= LENGTH(SRAM3), "SRAM3 FTL cp_job/journal overflow")
ASSERT(SIZEOF(.sram4_data) <= LENGTH(SRAM4), "SRAM4 FTL metadata dir overflow")

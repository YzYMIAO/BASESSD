/*
 * STM32H743xI bare-metal C linker script —— UT 专用变体。
 *
 * 与 stm32h743xx_flash.ld 的唯一区别是**数据放置**：全量 UT（FC-NAND + FTL +
 * HIL + speed）把测试自有的大缓冲也编进同一个镜像，原布局会溢出
 * AXI_RAM（debug +3.7 KB / release +5.5 KB）和 AXI_NOCACHE（+832 B）。
 *
 * 放置原则（与生产脚本一致，只是更显式）：
 *   ITCM / DTCM     最快（400 MHz、无 cache）：热代码、热全局、栈。
 *   SRAM12/3/4      片内 SRAM（D2/D3 域）：FTL 表、checkpoint、metadata、冷数据。
 *   AXI_RAM         512 KB AXI SRAM 的可缓存部分：默认 .data/.bss（L2P cache 等）。
 *   AXI_NOCACHE     不可缓存：只放 DMA / FMC 数据面缓冲。
 *   FLASH           冷代码与只读数据。
 *
 * UT 新增段（测试自有、CPU-only，无 DMA 访问）：
 *   .ut_test_slow    tests/unit/test_ftl_media.o 的 32 KB 读写缓冲
 *                    → SRAM123 尾部（D2 SRAM，比 AXI 慢，但测试不敏感）。
 *   .ut_test_nocache tests/unit/test_hil.o 的假 ftl_ctx（约 9.6 KB）
 *                    → SRAM4 尾部，源文件里已 memset 后使用，缓存属性无影响。
 * 两者都从 AXI/NOCACHE 移出，给生产的 L2P cache 与 FC/NAND 缓冲留余量。
 *
 * 这两段是 NOLOAD：启动代码只清 _sbss.._ebss。所选对象要么在被使用前
 * 显式 memset，要么先写后读，不依赖静态零初始化。
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
  /* D2 域 SRAM1+SRAM2+SRAM3 连续映射（0x30000000..0x30047FFF）。
   * UT 侧合成一个 region：FTL 表之后、cp_handle 之前留出一段连续冷数据区。 */
  SRAM123     (xrw) : ORIGIN = 0x30000000,      LENGTH = 0x00048000
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

  /* 热代码段（.ssd_code_high / .ssd_code_fast）由共享片段提供：
   * 产品与 UT 不再各自维护一份 KEEP 清单（见 RISK-BUILD-04）。
   * INCLUDE 的搜索路径由链接参数 -L$(PROJECT_ROOT)/linker 提供。 */
  INCLUDE stm32h743xx_hot.ld

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    . = ALIGN(4);
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

  /* FTL 热表放 SRAM123 前部（生产布局不变）。 */
  .sram12_data (NOLOAD) :
  {
    . = ALIGN(32);
    KEEP(*(.sram12_data))
    KEEP(*(.sram12_data.*))
    . = ALIGN(32);
  } > SRAM123

  /* UT cold data: keep the 32 KB media I/O buffer out of AXI_RAM. Must
   * precede the generic `.bss` rule, which would otherwise consume the
   * `.bss.s_io_buf` input section. It is filled before use, not zero-init. */
  .ut_test_slow (NOLOAD) :
  {
    . = ALIGN(32);
    KEEP(*tests/unit/test_ftl_media.o(.bss.s_io_buf))
    . = ALIGN(32);
  } > SRAM123

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

  /* UT cold fixture: fake ftl_ctx (~9.6 KB, explicitly memset before use).
   * Must precede `.noncacheable_buffer` below, which matches the same input
   * section and would otherwise place it in the full AXI_NOCACHE region. */
  .ut_test_nocache (NOLOAD) :
  {
    . = ALIGN(32);
    KEEP(*tests/unit/test_hil.o(.noncacheable_buffer))
    . = ALIGN(32);
  } > SRAM4

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

  /* cp_handle 放在 UT 冷数据段之后，SRAM123 的剩余空间由三段连续分配。 */
  .sram3_data (NOLOAD) :
  {
    . = ALIGN(32);
    KEEP(*(.sram3_data))
    KEEP(*(.sram3_data.*))
    . = ALIGN(32);
  } > SRAM123

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
ASSERT(SIZEOF(.sram12_data) + SIZEOF(.ut_test_slow) + SIZEOF(.sram3_data) <= LENGTH(SRAM123), "SRAM123 (SRAM1+SRAM2+SRAM3) FTL tables + UT slow data overflow")
ASSERT(SIZEOF(.sram4_data) + SIZEOF(.ut_test_nocache) <= LENGTH(SRAM4), "SRAM4 FTL metadata dir + UT fixture overflow")

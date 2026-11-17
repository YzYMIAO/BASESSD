# v61：中断服务函数分散化 —— 删掉总装 `stm32h7rsxx_it.c`，每个 ISR 归入其 port 文件

## 〇、起因

用户提出：**「对于中断服务函数，不要这个总装 `stm32h7rsxx_it.c`，建议分散到各个函数中去。」**

现状：`src/platform/stm32h743/stm32h7rsxx_it.c`（220 行）把所有 ISR 集中在一处，每个入口
都只是**一行转发**到真正处理它的模块函数。这带来两个问题：

1. **所有权割裂**：模块把 HAL 句柄设成 `static`、只暴露一个转发入口，真正的 ISR 名字却住在
   另一个文件里 —— 改一个外设的中断要先跨文件找总装表。
2. **模式只落实了一半**：`fault_port.c` 早已把 5 个 Cortex-M 异常入口（naked asm）收进自己
   文件，其余外设 ISR 却还留在总装里。

**关键事实（迁移可行性的根据）**：ISR 名字由 `third_party/stm32h743/startup_stm32h743xx.s` 的
向量表固定，且每个名字在启动文件里都是 `.weak … .thumb_set X, Default_Handler`。任何 TU 定义
同名强符号都会在链接期覆盖弱符号 ⇒ **ISR 可自由迁移到任何文件，无需改启动文件**。

**范围**：只做 `stm32h743`（项目规则：只跑 h743）。`stm32h7r3` 保持冻结不动。

## 一、ISR → 新归属

| ISR（向量表名） | 原内容 | 新归属文件 |
|---|---|---|
| `USART1_IRQHandler` | `usart_port_irq_handler()` | `src/platform/stm32h743/usart_port.c` |
| `DMA1_Stream0_IRQHandler` | `uart_tx_dma_irq_handler()` | `src/platform/stm32h743/dma_port.c` |
| `TIM6_DAC_IRQHandler` | `btim_port_irq_handler()` | `src/platform/stm32h743/btim_port.c` |
| `WWDG_IRQHandler` | `storage_wdg_wwdg_irq_handler()` | `src/platform/stm32h743/watchdog_port.c` |
| `DMA2_Stream0_IRQHandler` | `nand_dma_irq_handler_data_out()` | `src/platform/stm32h743/nand_dma.c`（`#if CONFIG_NAND_DMA` 内） |
| `DMA2_Stream1_IRQHandler` | `nand_dma_irq_handler_data_in()` | `src/platform/stm32h743/nand_dma.c`（`#if CONFIG_NAND_DMA` 内） |
| `SysTick_Handler` | `HAL_IncTick()` | `src/platform/stm32h743/sys_port.c` |
| `SVC_Handler` / `DebugMon_Handler` / `PendSV_Handler` | 空壳 | `src/platform/stm32h743/fault_port.c`（与既有 5 个异常入口同处） |
| `EXTI0/2/3/15_10_IRQHandler` | `HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_x)` | **删除** |

`NMI/HardFault/MemManage/BusFault/UsageFault` 已在 `fault_port.c`，**未动**。

### 用户拍板的两处

1. **EXTI 4 个入口直接删除**：它们在 h743 上是**死代码** —— 全仓没有 `HAL_GPIO_EXTI_Callback`
   实现、没有按键初始化、没有对 EXTI 的 NVIC 使能，是从板级模板带过来的残留。删除后这 4 个
   向量回落到 startup 的弱 `Default_Handler`（死循环）。
2. **内核异常安置**：`SysTick_Handler` 归 `sys_port.c`（HAL 时基）；`SVC/DebugMon/PendSV` 三个
   空壳留在 `fault_port.c`（`SYS_SUPPORT_OS == 0U`，永不触发），**行为逐字不变**。

## 二、`src/` 改动

| # | 文件 | 改动 |
|---|---|---|
| 1 | `usart_port.c` | 新增 `USART1_IRQHandler`（原体逐字搬入，含 `#if SYS_SUPPORT_OS` 的 `OSIntEnter/OSExit` 包裹；`SYS_SUPPORT_OS==0U` ⇒ 行为不变） |
| 2 | `dma_port.c` | 新增 `DMA1_Stream0_IRQHandler` |
| 3 | `btim_port.c` | 新增 `TIM6_DAC_IRQHandler`；改一处指向总装的注释 |
| 4 | `watchdog_port.c` | 新增 `WWDG_IRQHandler` |
| 5 | `nand_dma.c` | 新增 `DMA2_Stream0/1_IRQHandler`（放进文件既有的 `#if CONFIG_NAND_DMA` 内）；改两处指向总装的注释 |
| 6 | `sys_port.c` | 新增 `SysTick_Handler`（`HAL_IncTick()`） |
| 7 | `fault_port.c` | 新增 `SVC/DebugMon/PendSV` 三个空入口 |
| 8 | **删除** `src/platform/stm32h743/stm32h7rsxx_it.c` + `include/platform/stm32h743/stm32h7rsxx_it.h` | 后者全仓**只有前者 include**（h7r3 有它自己的一份），向量表在 `.s` 里按名字引用 ⇒ 可整删；`stm32h743/makefile.mk` 是 `*.c` glob ⇒ **无需改构建** |

新增/搬移的函数体与 `stm32h7rsxx_it.c` 中的原文**逐字一致**，只换了所在文件。

## 三、验证

```bash
make format && make format-check    # ✅ 通过
make clean && make tests            # ✅ 0 warning / 0 error（248160 B）
make clean && make all              # ✅ 0 warning / 0 error（170044 B，production debug）
```

**符号归属核对**（本版核心判据）——`arm-none-eabi-nm --defined-only` 逐对象文件确认：

```
btim_port.o     T TIM6_DAC_IRQHandler
dma_port.o      T DMA1_Stream0_IRQHandler
fault_port.o    T DebugMon_Handler / PendSV_Handler / SVC_Handler
sys_port.o      T SysTick_Handler
usart_port.o    T USART1_IRQHandler
watchdog_port.o T WWDG_IRQHandler
```

`stm32h7rsxx_it.o` **已不存在**；`DMA2_Stream0/1_IRQHandler` 与 4 个 EXTI 入口**均未定义**
（前者因 `CONFIG_NAND_DMA=0`，后者已删除）⇒ 相应向量回落 `Default_Handler`。
`.isr_vector` 尺寸 **664 B**，与改前**一致** ⇒ 向量表未变。

### 上板已验（2026-10-11，seed `0x5a4f21e4`）：**58 of 58 (100%)**、0 skipped、870.60 ms

开机日志逐字不变 ⇒ 迁移正确：

- `soft_ssd console ready` / `type 'help'` 与整段 CLI 可用 ⇒ **`USART1_IRQHandler` 迁移正确**
  （RX 中断仍进 `usart_port_irq_handler()`）。
- `btim task table ready` + 全套 870 ms 计时正常 ⇒ **`TIM6_DAC_IRQHandler` 迁移正确**
  （调度器毫秒心跳仍在）。
- `[FAULT] reset reason: PIN SFT (rsr=0x01460000)` 正常打印 ⇒ `SysTick_Handler`（`HAL_IncTick`）
  与 `fault_port.c` 的复位原因解码路径均未受影响。

## 四、风险与已知影响（如实登记）

1. **`CONFIG_NAND_DMA` 默认 = 0**（`include/platform/dma.h:36`，全仓无覆盖）⇒ 当前固件里
   `nand_dma.c` **整体不参与编译**，`DMA2_Stream0/1_IRQHandler` 原本靠 `dma.h` 的
   `static inline` no-op 空实现存在（map 中每个 12 B）。搬进 `nand_dma.c` 的 `#if` 内后，
   **关 DMA 时这两个向量不再定义 ⇒ 回落 `Default_Handler`**。行为安全：该配置下 DMA2 从不
   使能、不会有中断到来。
2. **`.bin` 不作为判据**：本版把定义在 TU 之间搬移、删了文件，`.bin` 尺寸必然变化
   （tests 248160 B，v60 是 248352 B）——
   动符号归属时 `.bin` 会漂。改用 **`nm --print-size` 逐符号 + `.isr_vector` 尺寸**核对。
3. **上板已验（2026-10-11）**：完整 `test` ⇒ **58 of 58 (100%)**、0 skipped、870.60 ms；
   开机日志逐字不变（见 §三）。

## 不做

- **不动 h7r3**（保持冻结；它有自己的一份 `stm32h7rsxx_it.c`）。
- 不动 `src/ftl/**`、`tests/**`、`ut_main.c`；不翻 `CONFIG_FC_RAID`。
- 不新增按键/EXTI 功能（只删死代码）。

## 出处

- 设计文档：`docs/design/platform_abstraction.md` §2.2 / §四 / §五 Stage 2.7 / §5.5。
- 启动文件：`third_party/stm32h743/startup_stm32h743xx.s`（向量表 + 弱符号绑定）。

# 企业级故障捕获机制（Cortex-M7 HardFault 全链路诊断）

> **落地日期**: 2026-10-04
> **涉及模块**: `src/platform/stm32h743/fault.c`（新增）、`include/platform/stm32h743/fault.h`（新增）、`src/common/crash.c` + `include/common/crash.h`（重写）、`src/common/assertion.c` + `include/common/assertion.h`、`src/fc/fc_assert_context.c`、`linker/stm32h743xx_flash.ld`、`src/platform/stm32h743/sys.c`、`src/platform/stm32h743/stm32h7rsxx_it.c`、`src/main.c`
> **芯片**: STM32H743II + MT29F4G08ABADAWP NAND
> **现状**: 编译验证通过（`make tests` / `make debug` 零告警零错误、`make format-check` 通过、向量表与蹦床反汇编已核对）；**上板部分验证**：复位原因打印正确、FC-NAND 三套件 59/59 全通过无 HardFault；人工触发故障/断言/跨复位三项 dump 未做

---

## 1. 问题：故障无声无息

跑板端 UT（UART CLI `test`）时经常"进系统级错误、断函数"，但串口上**什么都没打印**。原因有两层：

1. **异常处理器是空的**。`src/platform/stm32h743/stm32h7rsxx_it.c` 里 5 个 CPU 异常处理器全是裸 `while(1) {}` —— 无任何诊断输出，只能接 SWD 看寄存器。
2. **断言失败信息会丢**。断言路径打印后**不 flush 就停机**；日志走的是异步 UART TX 队列，由 DMA/中断排出，停机后队列再也不会被排空 —— 失败信息就卡在队列里，表现同样是"什么都没打印就断了"。修复后这一顺序收敛到断言设施 `assertion_terminate()` 里（`munit_errorf_ex` 经 `abort()` 重定向进入同一路径，见 `docs/design/munit_adapter.md` v1.2）。

## 2. 目标与范围

任何 CPU 故障或断言失败，都要在 UART 上打出**可读原因 + 出错地址 + 寄存器现场**，并写一份**跨软复位保留**的崩溃记录。

| 项 | 范围 |
|----|------|
| CPU 异常 | NMI / HardFault / MemManage / BusFault / UsageFault |
| 断言 | `ASSERT` / `ASSUME`（产品代码）+ munit 断言失败（测试代码，经 `abort()` 重定向） |
| 输出 | 轮询 UART（绕开 DMA/IRQ），不依赖 libc 格式化 |
| 持久化 | `.noinit` 崩溃记录，软复位后仍可读 |
| 停机语义 | dump 后 `while(1)` **保留现场**交给 SWD；IWDG 若开启则超时复位 |

**本次不做**（明确划界）：

- 不接 ITM/SWO/RTT（全工程无该通路）
- 不开 `CCR.DIV_0_TRP / UNALIGN_TRP`（会把除零/非对齐访问变成 UsageFault，可能影响既有代码）
- 不接线 SDRAM（`sdram_init()` 零调用且未上板验证）
- 不自动复位（保留现场优先；要看复位后记录就靠 `.noinit` + 手动 `NVIC_SystemReset`）

## 3. 架构

```
CPU 异常 (NMI/Hard/Mem/Bus/Usage)
      │  naked 蹦床：按 EXC_RETURN 选 MSP/PSP + 取 EXC_RETURN
      ▼
fault_handler_c(frame, exc_return)          [src/platform/stm32h743/fault.c]
      ├─ 读 SCB->CFSR/HFSR/MMFAR/BFAR/AFSR/SHCSR
      ├─ 解码为可读原因 + 打印堆栈帧 R0-R3/R12/LR/PC/xPSR
      ├─ 有界栈扫描 → 候选返回地址（poor-man's backtrace）
      ├─ 轮询 UART 同步输出（中断/DMA 已死也能出）
      ├─ 写崩溃记录（kind=FAULT）→ .noinit
      └─ weak fault_hook() → while(1) 停机

ASSERT/ASSUME 失败 (assertion_fail) ─┐
munit 断言失败 → abort() 重定向 ──────┴─→ 同一 crash_record_t + 同一 dump 格式
                                          （先 flush 异步日志，再轮询 dump）

开机：fault_report_boot() → RCC->RSR 复位原因 + 上次崩溃记录（走异步日志）
```

## 4. 模块清单

| 文件 | 角色 |
|------|------|
| `include/platform/stm32h743/fault.h` | `fault_kind_t`、`fault_ctx_t`、`fault_init/fault_report_boot/fault_hook` 声明 |
| `src/platform/stm32h743/fault.c` | 5 个 naked 入口 + 蹦床 + 主处理 + 轮询控制台 + 解码 + 开机报告 |
| `include/common/crash.h` | `crash_kind_t{NONE,ASSERT,FAULT,ABORT}`、`crash_fault_record_t`、`crash_record_t`、`crash_platform_report()` |
| `src/common/crash.c` | `.noinit` 记录存储 + 校验和 + 弱默认 `crash_platform_report()`（空实现，host 安全） |
| `include/common/assertion.h` | 生产断言 `ASSERT`/`ASSUME` + 富断言集 `ASSERT_*` + 失败入口声明 |
| `src/common/assertion.c` | 写记录 → 异步日志 → `assertion_terminate`（flush → 平台上下文 → `crash_platform_report` → 停机）；固件 `abort()` 重定向 |
| `src/fc/fc_assert_context.c` | 覆盖弱钩子 `assertion_platform_context()`，停机前追加 `[RES]` FC 资源快照 |
| `linker/stm32h743xx_flash.ld` | 新增 `.noinit (NOLOAD)` 段，落 DTCM |
| `src/platform/stm32h743/sys.c` | `sys_init()` 末尾调 `fault_init()` |
| `src/platform/stm32h743/stm32h7rsxx_it.c` | 删除 5 个裸 `while(1)` 处理器（改由 fault.c 提供） |
| `src/main.c` | `sys_init()` + sink 绑定之后调 `fault_report_boot()` |

## 5. 关键设计

### 5.1 naked 蹦床：必须在 C 序言之前确定栈

异常入栈是硬件做的，但**入哪个栈**由 `EXC_RETURN`（进入异常时的 LR）决定。任何 C 函数的序言（压寄存器、调整 SP）都会覆盖寄存器，所以读栈指针这一步必须在 naked 函数里、用汇编完成：

```c
static void fault_trampoline(void) __attribute__((naked, used));
static void fault_trampoline(void)
{
    __asm volatile("tst   lr, #4          \n"   /* EXC_RETURN bit2：0=MSP 1=PSP */
                   "ite   eq              \n"
                   "mrseq r0, msp         \n"
                   "mrsne r0, psp         \n"
                   "mov   r1, lr          \n"   /* r1 = EXC_RETURN，供上层判 FPU 帧 */
                   "b     fault_handler_c \n");
}
```

5 个入口各自 `b fault_trampoline`（宏 `FAULT_ENTRY` 生成）。反汇编核对：

```
080182ec <fault_trampoline>:
 80182ec:  f01e 0f04   tst.w  lr, #4
 80182f0:  bf0c        ite    eq
 80182f2:  f3ef 8008   mrseq  r0, MSP
 80182f6:  f3ef 8009   mrsne  r0, PSP
 80182fa:  4671        mov    r1, lr
 80182fc:  f000 b810   b.w    8018320 <fault_handler_c>
```

**EXC_RETURN 相关位**：

| 位 | 含义 | 本工程取值 |
|----|------|-----------|
| bit2 | 0=MSP，1=PSP | 线程模式用 MSP（无 RTOS） |
| bit3 | 0=返回 Handler 模式，1=返回 Thread 模式 | — |
| bit4 | 0=叠加 FPU 扩展帧，1=无 FPU 帧 | **硬浮点（`-mfpu=fpv5-d16 -mfloat-abi=hard`），常态为 0** |

FPU 扩展帧（S0-S15/FPSCR/保留字）**排在**基本帧（R0-R3,R12,LR,PC,xPSR）**之后**，所以 `frame[0..7]` 的取法在有/无 FPU 帧时都成立；`fp_frame` 只作提示。

> **出处（2026-10-14 核实）**：Zephyr `include/zephyr/arch/arm/cortex_m/exception.h` 的 `struct arch_esf` 把
> `struct __basic_sf {r0,r1,r2,r3,r12,lr,pc,xpsr}` 放在 `struct __fpu_sf {s[16],fpscr,undefined}` **之前**，
> 且故障路径直接用 `esf->basic.pc` 取出错 PC ⇒ **基本帧在低地址、SP 指向 R0**，FPU 帧在高地址。
> 这条结论是「PC/LR 可作证据」的前提：若排布反过来，`frame[6]` 读到的是 S6 而不是 PC，整个故障现场就是垃圾。

### 5.2 轮询控制台：故障时中断通路已不可信

正常日志走 `uart_log_write()` → 软件 TX 环 → DMA/USART 中断。故障现场可能就死在中断/DMA 里，这条通路必须绕开：

1. `fault_console_init()`：清 `USART1->CR1.TXEIE`、清 `USART1->CR3.DMAT`、abort `DMA1_Stream0`（有界自旋等 `EN` 清零），再按 `USART1->CR1.UE` 判断控制台是否可用。
2. `fault_putc()`：有界自旋等 `USART1->ISR.TXE` 后写 `USART1->TDR`。
3. `fault_console_drain()`：有界自旋等 `USART1->ISR.TC`，确保最后一位离开引脚再停机。

全部自包含，**不用 libc 格式化**（`fault_hex32`/`fault_dec`/`fault_puts` 自己实现），因此不依赖堆、不依赖 newlib。

> 若故障发生在 `usart_init()` 之前（`UE=0`），控制台不可用 —— 此时仍然照常写崩溃记录，只是不打印。

### 5.3 寄存器解码表

**HFSR**（`0xE000ED2C`）：

| 位 | 名称 | 含义 |
|----|------|------|
| 31 | FORCED | 由可配置故障升级而来 → **必须再看 CFSR 定位真因** |
| 1 | VECTTBL | 取向量时总线故障 |
| 0 | DEBUGEVT | 调试事件 |

**CFSR**（`0xE000ED28`）分三段：

| 段 | 位 | 名称 | 含义 |
|----|----|------|------|
| MMFSR (bit7:0) | 0 | IACCVIOL | 取指违反 MPU/XN |
| | 1 | DACCVIOL | 数据访问违反 MPU |
| | 3 | MUNSTKERR | 异常返回时出栈失败 |
| | 4 | MSTKERR | 异常入栈失败 |
| | 7 | MMARVALID | MMFAR 有效 |
| BFSR (bit15:8) | 0 | IBUSERR | 取指总线错误 |
| | 1 | PRECISERR | 精确数据总线错误（**PC 就是出错指令**） |
| | 2 | IMPRECISERR | 不精确（写缓冲，PC 可能已跑远） |
| | 3 | UNSTKERR | 出栈总线错误 |
| | 4 | STKERR | 入栈总线错误 |
| | 7 | BFARVALID | BFAR 有效 |
| UFSR (bit31:16) | 0 | UNDEFINSTR | 未定义指令 |
| | 1 | INVSTATE | 非法状态（如 EPSR.T=0） |
| | 2 | INVPC | 非法 PC 装载 |
| | 3 | NOCP | 无协处理器（未使能 FPU 却用 FP） |
| | 8 | UNALIGNED | 非对齐访问 |
| | 9 | DIVBYZERO | 除零 |

`fault_decode_cfsr()` 会把非零位打印成名字，`BFARVALID/MMARVALID` 置位时额外打印 `BFAR`/`MMFAR`。

> ⚠ **PC 落在 `0x5354414A` / `0x5354414B` 是栈哨兵 pattern**（`STACK_GUARD_PATTERN`
> = `0x5354414B`，"STAK" 的 ASCII），不是代码地址：说明某个保存的 LR / 函数指针
> 被栈填充值覆盖。已知案例：`stack_guard_init` 用入口 SP 作 `paint_hi`，release `-O2`
> 下会覆盖自己 `push` 的保存 LR（RISK-PLAT-04；PC 报 `0x5354414A` 是 Thumb 位清零）。

### 5.4 有界栈扫描

从出错 SP 向上扫 32 个字，落在 Flash 区间 `0x08000000..0x08200000` 的值当作候选返回地址打印（穷人的 backtrace）。

**安全约束**：SP 不在 DTCM（`0x20000000.._estack`）内就整个跳过 —— 避免用一个可疑的 SP 去读内存，触发二次故障进 Lockup。

### 5.5 持久化：`.noinit` 而不是 BKPSRAM

链接脚本新增：

```ld
.noinit (NOLOAD) : { . = ALIGN(8); KEEP(*(.noinit)) KEEP(*(.noinit.*)) } > DTCM
```

- `NOLOAD` → 不占 Flash、启动代码不清零（启动只清 `_sbss.._ebss`，在 AXI_RAM）
- 落 DTCM → 普通 SRAM，**软复位（`NVIC_SystemReset`）后保留**，硬复位/掉电不保留

为什么不用 BKPSRAM：`sys.c` 里已记录，本板 VBAT 未供电，备份稳压器永远无法就绪，使能它会卡死。

记录带 `magic + version + checksum`，`crash_record_get()` 三者都过才返回，否则视为脏数据。`version=2` 起 `checksum` 覆盖 `version/kind/line + 两段文本 + fault 负载全部字段`。

> 构建产物确认：`.noinit 248 0x20000000`（248 B = 5×4 + 96 + 96 + 9×4）。

### 5.6 停机语义

dump 完 `for(;;)`，**不主动复位**：

- 保留现场 → SWD 可继续看寄存器/内存
- IWDG 若开启 → 超时自然复位，复位后 `fault_report_boot()` 会打印 `RCC->RSR` 的 `IWDG` 标志 + `.noinit` 里的上次记录

### 5.7 开机报告

`fault_report_boot()` 在 `main()` 里 sink 绑定之后调用（**必须**，因为它走异步日志）：

- 打印 `RCC->RSR` 解码后的复位原因：`POR / PIN / BOR / SFT / IWDG / WWDG / LPWR`
- 有上次记录时打印摘要（FAULT：类型 + PC/LR/CFSR/HFSR；ASSERT：表达式 + file:line）
- 最后 `RCC->RSR |= RCC_RSR_RMVF` 清标志，让**下次**复位的原因不被这次污染

> 开机报告**不用**轮询控制台：`fault_console_init()` 会关掉 TX DMA/中断，在开机阶段这么干会把正常异步日志打断。

### 5.8 扩展点

```c
void fault_hook(const fault_ctx_t *ctx);   /* 弱符号，默认空实现 */
```

捕获到故障、落完记录、停机之前调用。留给上层接：复位前落 Flash、点亮故障灯、喂最后的看门狗等。

## 6. 上板验证

1. **复位原因**：烧录后看串口，应有 `[INF] [FAULT] reset reason: POR ...`（首次上电）。**已上板验证**：实测 `[INF] [FAULT] reset reason: POR PIN BOR SFT (rsr=0x01fe0000)`（标志位累积，含调试器复位与上次 SFT）。
2. **触发 BusFault**：临时加一行 `*(volatile uint32_t *)0x0 = 1U;`（或 SWD 把 PC 指到非法地址），预期串口打出：
   ```
   === CPU FAULT ===
   kind   : BusFault (IPSR=5)
   stack  : MSP
   PC     : 0x0800xxxx
   ...
   BFSR   : PRECISERR BFARVALID
   BFAR   : 0x00000000
   stack  : candidate return addresses
     0x2001fxxx -> 0x0800xxxx
   ```
3. **触发断言**：构造一次 munit 断言失败，确认失败消息**完整打出**后再停机（验证 flush 修复），并出现 `=== CRASH RECORD === kind : ASSERT`。
4. **跨复位**：触发一次故障后软复位（`NVIC_SystemReset()` 或调试器 reset），开机应打印 `[ERR] [FAULT] previous crash: ...`。
5. **回归**：`make tests` 三套 FC-NAND 套件（command/queue/scheduler/data_path/lifecycle）正常跑完 —— rebuild 套件已删除（见下）。**已上板验证**：59/59 全通过，无 HardFault。

## 7. 同期改动

### 7.1 FTL rebuild 套件从构建摘除

`test_rebuild.c` 的 `rebuild_fixture_t` ≈ 245 KB，而堆 = DTCM = 128 KB，物理上装不下，是"跑 UT 就 HardFault"的直接来源（见 `docs/archive/debug_sbrk_heap_stack_20261004.md`）。当前调试重心在 FC-NAND 层，先把该套件摘出构建，让故障捕获机制先落地可观测：

- `src/fc/makefile.mk`：注释掉 `test_rebuild.c`
- `tests/unit/ut_main.c`：删掉 `g_rebuild_tests` 的 extern 与子套件条目

`test_rebuild.c` 文件本身未删未改。夹具落地区域另议（AXI_RAM 实测只剩约 71 KB，不够；SDRAM 未接线）。

> **2026-10-13 后续**：该文件**已删除**——其两条仍有效的用例（`add_closed_sb_full` 满列表边界、
> `is_metadata_block_dynamic` 动态 region）并入 `../archive/ftl_ut_munit.md §4.1/§4.6` 的 FTL 套件设计；
> 其余用例与 §4 既有条目重合，且 `is_metadata_block` 断言的是 P0-2 前的**旧单块布局**（已失效）。
> 夹具超容的根因由 FTL 套件的「复用活实例」方案（`../archive/ftl_ut_munit.md §2.2`）化解，不再需要独立大夹具。

### 7.2 CLI 的 `%llu` → `%lu` 修复（newlib-nano 不支持 long long）

上板验证故障捕获机制时，串口打出 `[FTL] controller ready, lba_max=lu` —— 6 处 CLI 输出（`wr`/`rd`/`tr`/`info` 的 lba、n）全部受影响。根因：`-specs=nano.specs` 的 newlib-nano 未开 `_WANT_IO_LONG_LONG`，`%llu` 把首个 `l` 当长度修饰符吃掉、剩下 `l`/`u` 当字面量原样输出；GCC 的 `-Wformat` 按标准 C 语义检查，编译期**零告警**，只有上板看串口才发现。

- `src/platform/stm32h743/usart.c`：5 处调用点 `%llu`→`%lu` + `(unsigned long)` 强转；并加 `_Static_assert(FTL_LBA_MAX <= 0xFFFFFFFFUL, ...)` 钉住值域前提
- 验证方法：qemu-arm 跑同工具链二进制比上板快（`snprintf("%llu", 1234567890123ULL)` 返回 2，内容 `"lu"`）

后续固件里格式化 64 位值一律 `(unsigned long)` + `%lu`；确需 64 位输出时须重建带 `_WANT_IO_LONG_LONG` 的 newlib 或自写 u64→str。

## 8. 已知限制

- **栈溢出型故障可能打不出来**：处理器本身跑在 MSP 上。若故障根因就是栈已经压穿 DTCM（`STKERR/MSTKERR` 或 SP 越界），入异常时压帧会再次失败 → 双重故障 → Lockup，连 dump 的机会都没有。这类只能靠 SWD 看 `CFSR`。`_sbrk` 堆守卫（`syscalls.c`）挡的是堆撞栈，不是栈自己溢出。
- **故障发生在 `usart_init()` 之前**：`UE=0`，控制台不可用，只写记录不打印。
- **`.noinit` 只跨软复位**：硬复位/掉电后记录丢失（VBAT 未供电，BKPSRAM 用不了）。
- **烧录 / 软复位都不清 `.noinit`**：记录只在**断电**时丢失。由此有两个陷阱：
  ① 同一份 dump 在多次复位后**原样重复打印**（连 SP/寄存器都相同）——这**不构成**
     「又崩了一次」；§3.3 的「又看到同一行」判据即指此。
  ② 重新烧录后看到的 dump 可能属于**上一份镜像**：用当前 ELF 解 `PC`/`LR` 会落在
     错的位置（症状：LR 落在毫无间接调用的函数中间）。
  复现步骤必须是：**烧录 → 拔电（不是按复位）→ 上电确认 `crash` 无记录 → 再跑**。
  ⚠ 横幅的 `__DATE__/__TIME__` 只在 **main.c 重编**时更新，增量构建下会过期，
  **不能**用它证明当前镜像（待引入真正的 build id）。
- **IMPRECISERR**：写缓冲导致的不精确总线错误，栈里的 PC 可能已经跑远，`PC` 不一定是出错指令。
- **轮询输出期间中断是关的**：dump 期间若还有别的故障源在跑，会直接锁死。这是"停机保留现场"语义的必然代价。

## 9. 验证清单

- [x] `make tests` 零告警零错误
- [x] `make debug`（产品固件，无 `FC_NAND_TEST_ENABLE`）零告警零错误
- [x] `make format-check` 通过（clang-format-15）
- [x] `.isr_vector` 偏移 `0x08/0x0C/0x10/0x14/0x18` 分别指向 `NMI/HardFault/MemManage/BusFault/UsageFault_Handler`（fault.c 版本）
- [x] `fault_trampoline` 反汇编为 `tst lr,#4 / ite eq / mrseq r0,msp / mrsne r0,psp / mov r1,lr / b fault_handler_c`
- [x] map 里 `.noinit` 存在且落 DTCM（`0x20000000`，248 B），`._user_heap_stack` 相应上移
- [x] 上板：复位原因打印正确（`POR PIN BOR SFT (rsr=0x01fe0000)`）
- [x] 上板：FC-NAND 三套件 59/59 全通过，无 HardFault
- [ ] 上板：人工触发故障，dump 完整
- [ ] 上板：断言失败信息完整打出
- [ ] 上板：软复位后能读到上次崩溃记录

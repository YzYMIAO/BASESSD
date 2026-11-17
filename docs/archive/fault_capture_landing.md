# 故障捕获落地与验证记录（历史）

> 从 `docs/design/common/fault_capture.md` 迁入（2026-11-17）；正文不改写。
> 当前设计见 [`../design/common/fault_capture.md`](../design/common/fault_capture.md)。


> **落地日期**: 2026-10-04
> **涉及模块**: `src/platform/stm32h743/fault.c`（新增）、`include/platform/stm32h743/fault.h`（新增）、`src/common/crash.c` + `include/common/crash.h`（重写）、`src/common/assertion.c` + `include/common/assertion.h`、`src/fc/fc_assert_context.c`、`linker/stm32h743xx_flash.ld`、`src/platform/stm32h743/sys.c`、`src/platform/stm32h743/stm32h7rsxx_it.c`、`src/main.c`
> **芯片**: STM32H743II + MT29F4G08ABADAWP NAND
> **现状**: 编译验证通过（`make tests` / `make debug` 零告警零错误、`make format-check` 通过、向量表与蹦床反汇编已核对）；**上板部分验证**：复位原因打印正确、FC-NAND 三套件 59/59 全通过无 HardFault；人工触发故障/断言/跨复位三项 dump 未做

---


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
3. **触发断言**：构造一次 munit 断言失败，确认失败消息**完整打出**，随后出现 `[ASSERT] console active; FTL/FC background frozen` 与正常 CLI 提示符；在 CLI 里敲 `crash` 应能读到 `kind: ASSERT` 的 `.noinit` 记录。断言路径不再打印 `=== CRASH RECORD ===`（那是 CPU 异常路径的轮询 dump）。
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

---



---

> 以下日志落地状态表从主文档迁入（2026-11-17）。

### 7.5 落地状态（2026-10-23）

| 项 | 状态 |
|---|---|
| `[STAT]` 10 s slim 摘要（main.c） | 已改、未上板 |
| GC 实际搬移事件 `[MIG ] move`（migrate.c） | 已改、未上板 |
| 空块/无搬移 GC 不刷 `[GC  ] start`；`[GC  ] progress/end` 仅 `moved>0` | 已改、未上板 |
| `diag` 增加累计 `mv=`（搬移 CW）与 `nop=`（空块回收） | 已改、未上板 |
| CP start / JRN rotate 高频元数据事件不逐次打（看 `info` 的 `cp=`/`jrn=`） | 已改、未上板 |
| 模块标签 4 字符对齐（log.h + src 批量替换） | 已改、未上板 |
| `map` 增加 `paa=`；新增 `addr pba/paa/g`（cli.c） | 已改、未上板 |
| `FREE_POOL_TARGET` = 32（2026-10-23 回退）；BUSY 档 `free<32` 仍做水位 GC | 已上板（H2testw 后 free=64，未触发 T1） |
| DEBUG 收敛：GC trig 5 s、删逐页/逐 CW MIG/JRN/RR 流水 | 已改、未上板 |
| dangling 按旧 gblock 聚合（ftl.c） | 已改、未上板 |
| RR：已退役块每 gblock 一次、好块首故障全字段 + `st=`（read_recovery.c） | 已改、未上板 |
| `fc_cw_status_str()` / `fc_cmd_status_str()` 解码（fc_command.c） | 已改、未上板 |
| 写轨迹飞行记录器（allocator.c / ftl.c / read_recovery.c） | 已改、未上板 |
| CP done / GC trig 降 DEBUG；MIG 单块降 WARN | 已改、未上板 |
| GC 系统性读失败熔断（`MIG_SYSTEMIC_READ_FAIL_MAX=8`） | 已改、未上板（RISK-GC-10） |
| `info` 增加 `diag[...] dng/rrab/mig/streak` | 已改、未上板 |
| 只对**真正搬了数据**的 victim 打 `[MIG ] move ...`（`migrated==0` 不打印；aborted 不打） | 已改、未上板 |
| 统一 `log_ratelimit_s` 抽象 | 未做（现有各类节流已覆盖；后续收敛；见 RISK-LOG-02） |


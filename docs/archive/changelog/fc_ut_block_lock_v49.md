# v49：块锁族（除永久锁）合并为一条用例 `block_lock_ops`——顺带补两处 src 缺陷

- **日期**：2026-10-11
- **范围**：**测试 + src**。`tests/unit/fc_test_common.{c,h}`、`tests/unit/test_fc_level3.c`、
  `docs/design/fc_nand_ut.md` + 本文档；**`src/` 四文件六处**——`src/fc/fc.c`（两处）、
  `src/nand/onfi_1_0.c`（op 25 表项）、`src/nand/nand_dispatch_tables.c`（req 表一行）、
  `src/nand/nand_fmc.c`（两处 `LOGI`→`LOGD`，上板后追加，见「三·A5」）、
  `include/nand/onfi_1_0.h`（**仅 op 25 注释**）。**`src/ftl/**` 一行不动**。
- **介质格式**：无变更。
- **测试**：**用例数 53 → 54**（Level 3 **11 → 12**）。`make format-check` 过；
  `make clean && make tests` **0 warning 0 error**、链接 **229492 B**。
- ✅ **上板已验**（2026-10-11 seed `0x5a4f21e4`）：`/fc-nand/lifecycle/block_lock_ops` **[ OK ]**、
  完整 `test` **54 of 54 (100%)**、0 skipped。⇒ 本版对 `src/` 的四处改动**无回归**（见「四」）。

## 〇、起因

用户要求：**把除永久锁（`OP_BLOCK_LOCK_TIGHT`，24 / 2Ch）以外的块锁相关 op 合并成一条测试用例。**

| op | 命令 | 地址周期 | 改动前现状 |
|---|---|---|---|
| 21 `OP_BLOCK_UNLOCK_LOW` | 23h | 3 | op 表 `has_data=0`，handler = `req_no_data`，**零用例** |
| 22 `OP_BLOCK_UNLOCK_HIGH` | 24h | 3 | 同上 |
| 23 `OP_BLOCK_LOCK` | 2Ah | 0 | 同上 |
| 25 `OP_BLOCK_LOCK_STATUS` | 7Ah | 3 | `has_data=1U`（**语义错**），handler = `req_single`，**返回值读不出来** |

盘点结论：

- 这 4 个 op **没有任何生产调用者**（`grep OP_BLOCK_*` 只命中 op 表、req/completion 表、
  `onfi_1_0.h` 定义、以及 `test_fc_level3.c` 里两个 `#if 0` 死块）。
- `test_fc_level3.c` 里两个 `#if 0` 死块（`test_unlock_erase`、`test_block_lock_tight`）的函数体
  都调用了 v32 已删除的 helper（`test_run_lifecycle` / `erase_block` / `program_page` / `read_page` /
  `expected_byte`）⇒ 即使放开 `#if 0` 也编不过。
- `unlock_block()`（标 `__attribute__((unused))`）是唯一还活着的块锁辅助，但它**丢弃 completion**、
  不设 `ch/ce/lun`。

## 一、先补两处 src 缺陷（已定位）

### 缺陷 1：`plane_paa[]` 没填 ⇒ 23h/24h/7Ah 恒寻址 block 0

`fc_build_dispatch_request`（`src/fc/fc.c`）的分支链里块锁族落进末尾 `else`，**不写 `plane_paa[]`**；
而 `request` 在函数开头被 `(nand_dispatch_request_t){0}` 整体清零 ⇒ `plane_paa[plane]=0` ⇒
`req_no_data`/`req_single` 算出的 `row[0] = paa_to_nand_row(0)`。`ROW_0` 的 uop 就是
`*config->address = (uint8_t)request->row[uop->operand]`（`src/nand/nand_fmc.c`）⇒
三条带地址的命令**永远指向 block 0**，`cmd->paa` 白设。

**修法**（`src/fc/fc.c:1369-1385`）：新增「块锁族」分支，插在 `OP_READ_META` 分支之后、
`nand_operation_has_data(...)` 分支之前：

```c
request->plane                     = (uint8_t)paa_to_plane(command->paa);
request->plane_paa[request->plane] = command->paa;
```

**不置 `cw_bitmap`**——这 4 个 op 无 host 数据搬运（op 表 `has_data` 全为 0）。

### 缺陷 2：7Ah 的状态字节到不了 CQ

`nand_fmc_exec_data_out_status`（`src/nand/nand_fmc.c:399-412`）把字节写进
`completion->nand_status`（**不写 `request->data`**，line 406 显式 `(void)request`）；
该值经 `nand_fc_completion_t` → `nand_dispatch_completion_t.nand_status`（`nand_dispatch.h:70`）
一路传到 FC，但 `fc_complete_from_nand`（`src/fc/fc.c`）**从不读它** ⇒ 字节在 FC 边界被丢弃。

**修法**（`src/fc/fc.c:798-806`）：非 page-read 分支新增一支：

```c
if (command->config.operation == OP_BLOCK_LOCK_STATUS)
{
    completion.param.bytes[0] = nand_completion->nand_status;
}
```

## 二、决策（已与用户确认）

| 问题 | 结论 |
|---|---|
| 7Ah 的状态字节 | **改 src 让它进 CQ `param`**（新增完成分支，见缺陷 2） |
| 断言强度 | **只硬断言「可逆」**——每个 op 无 `FC_CW_STATUS_ERROR_MASK` 位，且 LOCK→UNLOCK 之后擦/写/读**全部成功**；「LOCK 是否真挡住擦除」「状态字节前后怎么变」一律**只打印不断言** |

断言强度的理由：手册 MT29F4G08 的 **LOCK 引脚**——「上电时 LOCK 为低则所有 BLOCK LOCK 命令被禁用；
为高则默认锁死全部 block」。本板现有用例一直能擦除 ⇒ 块未被保护 ⇒ LOCK 引脚几乎肯定是**低** ⇒
2Ah 大概率**空转**。硬断言「LOCK 挡住擦除」在本板会当场红，而红的原因不是代码错。

## 三、改动

### `src/nand/onfi_1_0.c`（A1）

op 25 表项（`:840`）：`.has_data` `1U`→`0U`、`.data_length` `1U`→`0U`。

**理由**：`has_data` 的契约是「是否涉及**用户数据传输**（需 descriptor+buffer）」
（`include/nand/onfi_1_0.h:446`）。7Ah 的字节**走 DQ 总线直读、不经任何数据缓冲**（缺陷 2 已证实），
标 `has_data=1` 是语义谎言，还逼调用方为一条无数据命令建 desc 链（`ftl_cmd_needs_desc_chain(25)==1`）。
**先例 = v45** 为同样理由把 op 33/29 的 `has_data` 从 1 翻成 0。

### `src/nand/nand_dispatch_tables.c`（A2）

req 表 `:592`：`[OP_BLOCK_LOCK_STATUS] = req_single` → `req_no_data`。

**理由**：`req_single` 在空掩码下取 `cw_buffers[cw_first(0)] = cw_buffers[0]`——那是**上一次占用该 slot
的残留指针**（契约见 `src/nand/nand_dispatch.c:164-166`）。`DATA_OUT_STATUS` 不解引用它，故**当前无害**，
但违反「空掩码 = 无 buffer 可读」。**先例 = v45 改动 D**（`req_read_page` 空掩码显式置 `data=NULL`）。
改走 `req_no_data` 后 `data=NULL, data_length=0`，与其余四个块锁 op 口径一致。

### `src/fc/fc.c`（A3、A4）

见「一」的两处修法。

### `include/nand/onfi_1_0.h`（注释）

op 25 注释补一句：状态字节经 `completion->nand_status` → CQ `param.bytes[0]`，故 `has_data = 0`；
附位定义（I/O2 = Lock#、I/O1 = LT#、I/O0 = LT）。

### `tests/unit/fc_test_common.{c,h}`（B1）

新增两个 helper（仿 `test_read_meta` 的无链范式）：

- `void test_block_lock_cmd(uint16_t block_index, uint8_t plane, const fc_cmd_conf_t *config)`
  覆盖 21/22/23；`test_cmd_build(&addr{.block,.plane}, 0U, 0U, config)`（**无 desc 链**）→
  `test_submit_wait` → 断言无 `FC_CW_STATUS_ERROR_MASK` 位 → `fc_command_release`。
- `uint8_t test_block_lock_status(uint16_t block_index, uint8_t plane, const fc_cmd_conf_t *config)`
  op 25；同上，但**在 `fc_command_release` 之前**取 `completion.param.bytes[0]` 返回。

两者都 `ASSERT(block_index < BLOCKS_PER_PLANE)`、`ASSERT(plane < PLANE_NUM)`。

### `tests/unit/test_fc_level3.c`（B2、B3）

新用例 `block_lock_ops`（`MUNIT_TEST_OPTION_NONE`，地址由 `l3_plane_draw`/`l3_block_draw` 抽样，
`page = PAGES_PER_BLOCK/2`），八阶段：

| Phase | 动作 | 断言 |
|---|---|---|
| 0 | 基线：erase + prog(page) + read(page) + 校验 | **硬断言**（块可用、内容已知） |
| 1 | `7Ah` 取锁定前状态 | 无（证据打印，解码 Lock#/LT#/LT） |
| 2 | `2Ah` BLOCK_LOCK（无地址周期 ⇒ 锁全片） | 硬断言无错误位 |
| 3 | `7Ah` 取锁定后状态 | 无（证据打印） |
| 4 | 再擦一次，看是否被拒 | **不断言**（有错误位 = LOCK 生效；无 = 本板 LOCK 引脚为低、2Ah 空转） |
| 5 | `23h` UNLOCK_LOW + `24h` UNLOCK_HIGH | 硬断言无错误位 |
| 6 | `7Ah` 取解锁后状态 | 无（证据打印） |
| 7 | 再 erase + prog(page) + read(page) + 校验 | **硬断言「可逆」——本用例唯一强判据** |

`23h@block` 覆盖 0..block、`24h@block` 覆盖 block..last ⇒ 合起来解锁全器件 = 恢复默认。
Phase 4 用现成的 `test_erase_block`——它**只返回 completion、内部不断言成败**，正好当「证据」。

**清理**：删 `unlock_block()`（被 `test_block_lock_cmd` 取代）与 `#if 0` 死块 `test_unlock_erase`
（被本用例取代且调用了已删 helper）；`test_block_lock_tight` 的 `#if 0` **保留**（记录「永久锁为什么不做」）。

### `docs/design/fc_nand_ut.md`

用例表加 `block_lock_ops` 行；op 覆盖表补 v1.31 说明（21/22/23/25 被点名、op 25 `has_data` 1→0）；
§7 修订历史加 **v1.31** 行。

### `src/nand/nand_fmc.c`（A5，上板后追加）

READ_ID 数据转储的两处 `LOGI` → **`LOGD`**（DMA 路径 + PIO 路径，共两处）。

**起因**：上板跑 `test` 时，`/fc-nand/lifecycle/init_sequence` 那行被这条 `[INF]` 从中间劈开：

```
/fc-nand/lifecycle/init_sequence:                      [INF] nand: READ_ID data=2C DC 90 95 56 buffer=0x24060e78
[ OK     ] [ 237.14 us ]
```

**根因**：munit 先打不带换行的用例名前缀（补齐到 55 列），跑完用例才打 `[ OK ]`；用例体内的
写者（这里是 nand 驱动的日志）插在中间。`tests/unit/fc_test_common.c` 的 `test_trace_begin()`
**只对证据档**（`--log-visible info/debug`）断行，默认档一个字节都不输出 ⇒ 默认档挡不住。

**为什么降到 `LOGD` 而不是删掉**：① 同一次上电 `src/nand/nand_mt29f4g08abadawp.c:508` 已有
一条正式的 `[INF] nand: ID=...`，**字节完全相同**，这条是冗余的低层 dump（还带 `buffer=%p`）；
② `nand_fmc.c` 里其余 3 条诊断本来就是 `LOGD`，这两条是**唯一的例外** ⇒ 降级即归位；
③ 默认档是 `log level info`（`src/main.c:312`）⇒ 降级后默认不打印，`log level debug` 仍可看；
④ release 构建 `DEBUG` 未定义（`config.mk:143-147`）⇒ `LOGD` 编译成 `((void)0)`，**零成本**。

⚠ 证据档（`--log-visible debug`）下这条 `LOGD` 仍会打印，而 `test_init_sequence` **没调**
`test_trace_begin()` ⇒ 那个档位下仍会粘行。本轮未处理（只影响 debug 档，默认档已干净）。

## 四、验证

```bash
make format-check                  # ✅ Format check passed.（须走 clang-format-15）
make clean && make tests           # ✅ 0 warning 0 error；用例数 54（53 → 54）
                                   #    text 227736 + data 1712 = 229448 B（.bin 229492 B）
make clean && make                 # ✅ 0 warning 0 error；release 固件 .bin 166796 B
                                   #    （A5 的 LOGD 在 release 下编译为 ((void)0)）
```

**上板（用户跑，本版唯一强判据）**：

```
test --log-visible debug /fc-nand/lifecycle/block_lock_ops
test                      # 完整套，确认没有后续用例被块锁影响
```

验收点：`block_lock_ops` **1 of 1**；日志里四条证据（LOCK 前/后、UNLOCK 后的状态字节 +
Phase 4 擦除是否被拒）齐全；全套 **54 of 54**。

### 实测（seed `0x5a4f21e4`）—— ✅ 绿

```
/fc-nand/lifecycle/block_lock_ops:                     [ OK     ] [ 4.04 ms ]
…
54 of 54 (100%) tests successful, 0 (0%) test skipped. Total wall-clock: 699.32 ms
```

- `block_lock_ops` 通过 ⇒ **Phase 7 的硬判据「可逆」成立**：2Ah 锁定 + 23h/24h 解锁之后，
  该 block 仍能 erase / program / read 并逐 CW 校验通过。
- **完整套 54 of 54** ⇒ 本版对 `src/` 的四处改动（`fc_build_dispatch_request` 新分支、
  `fc_complete_from_nand` 新分支、op 25 `has_data` 1→0、req 表 `req_single`→`req_no_data`）
  **无回归**——「没有遗漏的调用点」这条只有完整套能判（先例 = v47）。
- ⚠ 本次上板用的是默认日志档（未加 `--log-visible debug`）⇒ **Phase 1/3/4/6 的四条证据行
  未落到串口**。用例本身已通过（Phase 4 的擦除尝试无论成败都不影响判据），故**不重跑**；
  若要留存证据字节，可另跑一次 `test --log-visible debug /fc-nand/lifecycle/block_lock_ops`。

## 五、风险 / 需上板确认（如实记录）

- **若本板 LOCK 引脚为高（块锁启用）**，2Ah 会**锁死全器件**：Phase 4 会看到擦除被拒（正是要观察的
  现象），Phase 5 的 23h/24h 合起来解锁全器件，Phase 7 应恢复通过。**若 Phase 7 红 ⇒「锁不可逆」是真
  结论，不绕过。** 旁证：本板 `block_erase_verify` 等用例一直能擦 ⇒ 块未被保护 ⇒ 引脚为低、2Ah 空转，
  风险低。
- **`OP_BLOCK_LOCK_TIGHT`（24 / 2Ch）不做**：锁到断电，资源不可恢复（同 OTP 的处理口径）。
- **A1/A2/A3/A4 影响面**：已核实这 5 个 op 无生产调用者；A3 分支对其它 op 不可达；
  A1 让 `ftl_cmd_needs_desc_chain(25)` 从 1 变 0——FTL 侧无人用 op 25，无行为变化。
- ~~**本版动了 `src/`**（A1~A4），只有真实器件能判~~ ⇒ **已由完整 `test` 54 of 54 坐实**（见「四」）。
  三处改动都只作用于「命令本身就是块锁族」的路径，其余 op 不可达 —— 这一点已由上板无回归验证。
- ⚠ **Phase 4 的现象（LOCK 是否真挡住擦除）本轮未从日志观测到**（跑的是默认日志档）。按
  「断言强度」的决策，这一项**本就不断言**；若将来需要该现象的实证，重跑 debug 档即可。

## 不做

- 不做 `OP_BLOCK_LOCK_TIGHT`（24）；不做 OTP（26/27/28）——资源不可恢复。
- 不改 `OP_COUNT`、不改 `fc_is_page_read`、不改 `fc_dispatch_rewrite_operation`。
- `src/ftl/**` 一行不动；不翻 `CONFIG_FC_RAID`；不动 `tests/integration/**`、`tests/framework/**`、
  `ut_main.c`。

## 出处

- 手册语义（MT29F4G08ABADAWP）：`BLOCK LOCK (2Ah)` **无地址周期、锁全部 block**；
  `BLOCK UNLOCK LOW (23h)` / `HIGH (24h)` 带 3 个地址周期；`BLOCK LOCK READ STATUS (7Ah)` 带 3 个
  地址周期、返回 1 字节（I/O2 = Lock#：0 = 锁定、1 = 未锁；I/O1 = LT#；I/O0 = LT：1 = 紧锁）；
  **LOCK 引脚**上电为低则禁用全部 BLOCK LOCK 命令、为高则默认锁死全部 block。
- 地址空间与换算：`include/nand/nand_config.h`（PAA/PBA/`pba_to_paa`）。
- op 表 / uop：`src/nand/onfi_1_0.c:443-515`（uop 序列）、`:836-840`（op 表）；
  `src/nand/onfi_1_0.h:303-311`（op 25 注释）。
- 派发表：`src/nand/nand_dispatch_tables.c:393-400`（`req_no_data`）、`:592`（req 表）。
- 状态字节通路：`src/nand/nand_fmc.c:399-412`（`nand_fmc_exec_data_out_status`）、
  `src/nand/nand_dispatch.h:70`、`src/fc/fc.c:798-806`。
- 建链分支：`src/fc/fc.c:1369-1385`；`request` 清零点 `src/fc/fc.c:1275`。
- 同构的已跑通模型：`tests/unit/fc_test_common.c` 的 `test_read_meta`（无 desc 链、直接读 CQ `param`）。

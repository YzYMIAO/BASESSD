# v36：单测证据打印（复用 munit `--log-visible`）+ `feature_write_read` 改读-改-写回环

- **日期**：2026-10-10
- **范围**：`tests/framework/munit.{c,h}`、`tests/unit/fc_test_common.{c,h}`、
  `tests/unit/test_fc_level1.c`、`tests/unit/test_fc_level2.c`、`tests/unit/test_fc_level3.c`、
  `src/platform/stm32h743/usart.c`（**仅 `test` 的帮助文本**）、
  `docs/design/{fc_nand_ut.md,munit_adapter.md,wsl_debug_environment.md}`
- **生产代码**：`src/**` 的**逻辑零改动**——唯一的 `src/` 变更是 `usart.c` 里 `test` 命令的
  usage 串补 `--log-visible`（与 v32 加 `--single` 时同一做法）。
- **介质格式**：无变更。
- **测试**：`make format-check` 过；`make clean && make tests` **0 warning 0 error**、
  链接 **219012 B**；**上板已验**：默认 / info / debug 三档各 **77 of 77**。

## 背景：用户不信「一片 `[ OK ]`」

原话：「我目前从打印来看只是成功，不知道是不是真的成功」。
这个不信任**有根据**——本会话前脚刚清掉两类「绿灯不可信」：

- **v34** 修掉一条**恒真断言**（`process_read_mata_error` 的篡改落在 `PLANE_CW_MASK` 的 plane 位，
  被 `lba_to_meta()` 整段擦除，断言永不可能成立，却一直显示 OK）；
- **v35** 修掉两处 `cw_bitmap` 建链缺陷（测试不是静默通过，而是**直接崩**——同样躲过了编译期）。

两者都说明：`[ OK ]` 只证明「没炸」，不证明「真的验了」。

## 机制：munit 的 `--log-visible`，不用项目自己的 `log.h`

**开关本来就在**，缺的是测试侧的证据。munit fork 已有日志等级
`DEBUG(0)/INFO(1)/WARNING(2)/ERROR(3)`，CLI 有 `--log-visible <level>`（`munit.c:1569-1606`），
**默认可见级 `ERROR`**（`munit.c:136`），且**过滤发生在任何输出之前**
（`munit.c:140-145` 的 `if (level < visible) return;`）——这正是「默认档报告格式逐字不变」的根据。

不用 `include/common/log.h` 的理由：`LOGI` **默认就开着**会立刻刷屏，`LOGD` 被**编译期**闸住
（运行时打不开），且 `g_log_level` 是**全局固件状态**，调完会一直影响 FTL/NAND 的生产日志。

框架侧只加 **3 行**：`munit_log_level_visible` 是 `munit.c` 的文件级 static，框架外读不到，
故新增只读访问器 `bool munit_log_visible(MunitLogLevel level)`（见 `munit_adapter.md` v1.4）。

**但不走 `munit_logf`**：它给每一行加 `Info:<文件>:<行号>` 前缀（`munit.c:147-178`），
逐页明细会被撑到 ~110 字符、UART 阻塞时间翻倍。证据行**直接写 `MUNIT_OUTPUT_FILE`**。

## 输出格式：先解决「trace 插进结果行中间」

munit 的参数前缀行（`    block=100, plane=0`）**不带换行**就开跑用例（`munit.c:900-926`），
补齐到 `MUNIT_TEST_NAME_LEN`(55)，最后才打 `[ OK ]\n`。所以用例体内的任何输出都会插进结果行中间。

**解法**：`test_trace_begin()` 在可见时**急切**补一个 `'\n'`（不等第一条证据行）。
这顺带修掉一个既有缺陷——`init_sequence` 里 nand 驱动打的 `[INF] nand: READ_ID ...`
此前会粘在结果行上（默认档仍在，因为可见级恒假、一个字节都不能发）。

## 改动

| 文件 | 改动 |
|---|---|
| `tests/framework/munit.{h,c}` | +3 行只读访问器 `munit_log_visible()`（唯一的框架改动） |
| `tests/unit/fc_test_common.{h,c}` | 新增 `test_trace_begin()` / `test_trace_summary(INFO)` / `test_trace_detail(DEBUG)`；夹具 `test_prog_page` / `test_read_page` / `test_read_block_cache` / `verify_chain_paa` 加 DEBUG 明细 |
| `tests/unit/test_fc_level3.c` | 9 条用例各加摘要；`feature_write_read` 改回环（见下） |
| `tests/unit/test_fc_level2.c` | 迁移既有 6 处裸 `munit_logf`（原先正是粘在结果行后的）到 `test_trace_summary`；给 4 个自证函数加摘要 |
| `tests/unit/test_fc_level1.c` | 11 条纯 RAM 用例各加 1 条摘要 |
| `src/platform/stm32h743/usart.c` | `test` 的 usage 串补 `--log-visible`（**仅帮助文本**） |

**不动任何断言、不增删用例**：仍是 **77** 条。

## 顺带：`feature_write_read` 从「只 GET」改成读-改-写回环

设计文档 `docs/design/fc_nand_ut.md:269` 早就写着 `SET_FEATURES → GET_FEATURES`，
但实现只 GET——**只查 status 的版本证明不了「写进去的值读得回来」**（器件把寄存器恒读成 0 也照样过）。

### ⚠ 第一版写 `0x89`，上板直接红

第一版照抄 `include/ftl/read_recovery.h:56` 的 `RR_FEATURE_READ_RETRY (0x89U)`。
**`0x89` 在本器件手册（MT29F4G08ABADAWP Table 13）里落在 `82h–FFh` 保留段**，读回值未定义。
上板结果：`readback[0] == probe[0] (0 == 1)`。

改后按手册来（用户 2026-10-10：「89 不支持，这个测试中地址不可以随机必须按手册来」）：

- **Phase 1**：手册 Table 13 的四个可寻址寄存器（`01h`/`80h`/`81h`/`90h`）各 GET 一次，**只读**。
- **Phase 2**：在 **`01h`（Timing mode）** 上做读-改-写回环，探针取 Table 15 给死的 `01h`。

**为什么回环选 `01h` 而不是 `90h`（唯一另一个有手册取值编码的）**——看**还原失败的代价**。
手册说 feature 断电才复位、连 `RESET(FFh)` 都清不掉，所以还原失败必然把器件留在非默认态：

- 停在**模式 1 是良性的**：模式是「器件能接收的**上限**速度」，而主机 FMC 时序按模式 0 配、
  比器件上限**慢**，照常工作；
- 停在 `90h` 的 `08h` 则**内部 ECC 一直开着**，后面所有裸页读写**静默出错**到断电。

`80h`/`81h` 一律**只读**（改驱动强度影响信号完整性；改 R/B# 下拉写重了 WAIT_READY 会读到超时）。
**还原的 SET 排在断言之前**，断言 abort 时也不会把器件留在非默认态。

### ✅ 上板结果

```
[EV] GET_FEATURES 0x01 (Timing mode) -> 00 00 00 00
[EV] GET_FEATURES 0x80 (Drive strength) -> 00 00 00 00
[EV] GET_FEATURES 0x81 (RB# pull-down) -> 00 00 00 00
[EV] GET_FEATURES 0x90 (Array op mode) -> 00 00 00 00
[EV] 0x01 回环: 原值 00 -> SET 01 -> GET 01 -> 还原 00
[ OK     ]
```

这是本板**第一次**真正跑通 `SET_FEATURES`/`GET_FEATURES` 的 4 字节双向——
此前唯一的 SET 调用者随 `CONFIG_NAND_TIMING_MODE_4=0` 被编译掉，这条路径从未被摸过。

### 顺带证伪了自己写的一条判据

Phase 1 注释原本断言「`0x80`（驱动强度）的默认值不该是 0；**若四个地址全读回 `00 00 00 00`，
说明 4 字节根本没搬回来**」。板上四个地址**确实全是 `00`**，但回环能写进去读回来 ⇒ 判据被证伪：

`GET` 的暂存是**先 `memset(0)` 再交给引擎**的（`fc.c:1287`），读回非 0 **只可能来自器件**。
所以全 0 是器件在上电默认态下的**真实读数**，不是搬运失败。
那句「`0x80` 默认不该是 0」是**没查手册的猜测**（违反 `no-unverified-protocol-claims`），注释已改掉。

## 上板实测

| 档 | 结果 | 证据行 |
|---|---|---|
| 默认（`--log-visible error`） | 77/77，输出**逐字不变** | **0** |
| `--log-visible info` | 77/77 | **131** |
| `--log-visible debug` | 77/77 | **4936**（131 摘要 + 4805 明细） |

`[ERR]` 仅 1 条 = 开机时 `.noinit` 里的**陈旧 crash 记录**（上一镜像留下的），不是本次。

**非空转抽查**（这轮的核心价值）：`64/64 pages`、`4 CW/page`、每 plane `131072 B`、
`block_random_data` 的 plane 1 CW0 打出 `0x10`（v35 踩过的 plane 盲位图）、
debug 档逐页 `PBA` 按 `flat_page<<3 | plane<<2 | cw` 递增且 `status=0x40`（SUCCESS）。

## 教训

1. **`--log-fatal` 是陷阱**：它与 `--log-visible` 共用取值解析，`level >= fatal` 会 `abort()`
   ⇒ `--log-fatal info` 让每条证据行 abort。默认 `fatal = ERROR` 安全。
2. **可见级跨 `test` 调用残留**：`munit_log_level_visible` 是文件级 static、`munit_suite_main` 不重置
   ⇒ 一旦开了 info，之后光打 `test` 也仍出证据，直到重启或 `--log-visible error`。
3. **UART TX 是阻塞的**（环 4096 B / 115200 波特 ≈ 11.5 KB/s）⇒ debug 档只对**单条**用例跑；
   且证据打印在用例体内，`make tests` 的 `-DMUNIT_ENABLE_TIMING` 会把 UART 阻塞算进 `[ N ms ]`
   ⇒ **开证据后不可再当性能基线**。
4. **Feature 地址不能跨器件照抄**：`0x89` 是从 FTL 层抄来的，在本器件上是保留地址。
   用例里的地址一律取自**本器件手册**的 Table 13。
5. **`src/ftl/**` 本轮不动**：`read_recovery.c:78/:130` 同样往保留地址 `0x89` 写读重试档
   （Stage 1 的「8 次读重试」大概率自始空转），但用户 2026-10-10 明确
   「不去处理 FTL 的东西，错了就错了，保证 FC 测试通过就行」，故**仅记录不修**。

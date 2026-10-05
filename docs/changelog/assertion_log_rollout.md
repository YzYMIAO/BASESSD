# 非 FC 层断言落地 + 统一日志设施

**日期**: 2026-10-05
**分支**: main
**涉及模块**: `src/nand/`、`src/ftl/`、`src/controller/`、`src/platform/stm32h743/usart.c` + `include/platform/stm32h743/usart.h`、`docs/design/assertion_guidelines.md`（新增）
**设施前置**: `common/assertion.h` / `common/log.h` 已由 `fd2a0d9` 建成，FC 层已先行落地（`docs/design/fc_nand_design.md` §24）
**现状**: 编译验证通过（`make debug` / `make tests` / `make release` 零告警零错误、`make format-check` 通过）；**上板验证未做**（断言在 tests 构建里是活的，必须上板跑套件）

---

## 背景

`fd2a0d9` 把断言/日志设施从 FC 层独立成 `src/common/` 模块，FC 层按四条判据落地了 41 处 `ASSERT`。本轮把同一套设施推广到**全部非 FC 层**，并把 platform 里并行的第二套日志等级设施并入 `common/log.h`。

判据的完整表述独立成文于 `docs/design/assertion_guidelines.md`（四条判据 + 三条辨析 + 落地注意），FC 层 §24 改为指向它。

## 一、断言落地

模式：`if (<前置条件不成立>) { return <错误码>; }` → `ASSERT(<前置条件>);`，删掉恢复/降级分支，并在注释里保留原判据语义。

### Tier 1（计划内确定项）

| 层 | 文件 | 转换点 |
|---|---|---|
| `nand/` | `nand_engine.c` | UOP 流非法（`uop->type >= NAND_UOP_COUNT`） |
| | `nand_dispatch_tables.c` | 5 个 `req_*` 的 `cw_buffers == NULL`（CW 缓冲由 FC 数据路径填满） |
| | `nand_fmc.c` | DATA_IN / DATA_OUT handler 的 `request->buffer` / `data_length` |
| | `nand_dispatch.c` | `default:` 分支 → `ASSERT_UNREACHABLE()` |
| `ftl/` | `allocator.c` | closed-SB 列表满（重建阶段不应发生） |
| | `power_failure_recovery.c` | `allocator_add_closed_sb` 返回的 `sb_idx` |
| | `wear_leveling.c` | `closed_idx < closed_sb_count` |
| `controller/` | `controller.c` | `route` 的 ch/ce/lun 越界、route 后 `ftl == NULL`、FLUSH 循环 `fftl == NULL` |
| | `request.c` | route 失败降级、读/写循环里的 `ftl == NULL` |

### Tier 2（逐点核对调用链后转）

| 文件 | 转换点 | 依据 |
|---|---|---|
| `nand_engine.c` | `nand_engine_start` / `nand_engine_step` 全部入参前置条件 | 头文件已文档化前置条件；两个调用者（`nand_fc_execute`/`nand_fc_start`/`nand_fc_step`）均可证 |
| `pending.c` | 6 处 `ftl == NULL` | 层内接口，唯一调用者 `ftl.c` 已判过 |
| `read_recovery.c` | `rr_record_ecc` / `rr_predictive_scan` / `rr_retire_block` / `rr_recover_read` | 调用者一律传 `&ftl->alloc.blocks[i].rr` 或已判非空 |

**全工程 0 处 `ASSUME`**（无 LTO，`__builtin_unreachable()` 换不来代码生成收益；`ASSERT` 现场更可定位）。

### 刻意不转断言

| 位置 | 理由 |
|---|---|
| `journal.c` 整个文件 | 回放时读的是 **NAND 上不可信的持久化数据**，损坏是掉电的预期情形 → 返回错误码走恢复流程，属判据 3。计划原列的 5 处经核对不是守卫（4 处是页/块翻转控制流，1 处是介质数据校验） |
| `crash.c` 的 `crash_copy(source == NULL)` | 故障路径禁止断言（同 `fault.c` / `assertion.c` / `syscalls.c`），健壮性 > 不变量 |
| 所有 `fc_command_alloc()` 后的 `cmd == NULL` | 资源类（判据 3） |
| `status_out` / `optional-out` 判空 | 语义分支（辨析 B） |

## 二、补日志（FTL / controller）

这两层原先**一条日志都没有**。策略：只在**层边界与阶段跃迁**处加 `LOGE/LOGW`，不逐分支加（FTL 有 40+ 个 `return FTL_ERROR`，全加会淹没串口）。

- **FTL**：`ftl_init` 失败、`ftl_submit_read/write` 校验失败、FC 命令池耗尽、GC 后无空闲块、`ftl_handle_completion` 读不可恢复 + 写失败标坏、`wl_predictive_relocate` 慢死退役。
- **controller**：`ssd_controller_init` 失败（槽位为空 / topo 不匹配）、`ssd_controller_submit` 未就绪拒绝、状态跃迁到 `FAILED`。

`ssd_controller_init` 里的 `LOGE` 安全：`main.c` 在 `ssd_ftl_boot_all` 之前已 `log_set_sink`。

## 三、`uart_log_*` 双栈并入 `common/log.h`

`src/platform/stm32h743/usart.c` 里曾并存两套等级日志设施：`log.h` 栈（`log_printf` → `g_log_level` → sink `stm32_uart_log_sink` → `uart_log_write`）与 `usart.c` 自己的栈（`uart_log_printf` → `g_uart_log_level`）。本轮合并：

- 13 处 `uart_log_printf(UART_LOG_*, fmt, ...)` → `LOGE/LOGW/LOGI(fmt, ...)`（**保留格式串末尾 `\r\n`**：sink 路径不经过 `_write`，没有 CRLF 归一化）。
- `uart_cli_log` 的 4 个 `uart_log_set_level(UART_LOG_*)` → `log_set_level(LOG_ERROR/WARNING/INFO/DEBUG)`。
- 删除 `uart_log_printf` / `uart_log_set_level` / `uart_log_get_level` / `uart_log_level_name` / `g_uart_log_level`，以及 header 的 `uart_log_level_t` 枚举与三个声明。
- **保留** `UART_LOG_FLUSH_FOREVER`、sink 用的 `uart_log_write` / `uart_log_flush`，以及裸 `uart_log_write` 的**终端 I/O**（prompt / 回显 / 退格 / `rd PASS` / `error: ...` / `version` / `log level updated` / `tr|fl submit ok`）——这些不是带等级的日志，加 `[ERR] ` 前缀反而改变 CLI 观感。

> 遗留不一致（**原代码就有，刻意保留**）：`tr submit ok` / `fl submit ok` 是裸写，而 `wr submit ok` / `rd submit ok` 走 `LOGI`。两者对 `log level` 的响应不同。若要统一，应把 tr/fl 也改为 `LOGI`。

`src/platform/stm32h7r3/` 未动（该板不参与编译）。

## 四、FC 层自铸句柄补扫（辨析 D）

非 FC 层落地后回扫 FC 层，发现 `fc_interface.c` 里仍有一类"不可能"守卫没转：**对 FC 自铸句柄的越界检查**。判据 1 原先把 `fc_interface.h` 全部入口整体划为外部边界，属过度概括——按句柄来源细分后（见 `assertion_guidelines.md` §七）：

- `command_id` 只来自 `fc_command_alloc()`，`descriptor_id` 只来自命令 desc 链，调用者**不可能**传越界值 → 越界即不变量破坏 → `ASSERT`。

转换点：

| 文件 | 转换点 |
|---|---|
| `fc_interface.c` | `fc_command_to_id` 两处 NULL（静态 helper，两个调用者都已判过 `command`）、`fc_write_cmd_data` / `fc_read_cmd_data` / `fc_get_cmd_data_ptr` 的 `desc_id >= FC_DESCRIPTOR_POOL_DEPTH`、`fc_command_id_to_command` / `fc_descriptor_id_to_descriptor` 的越界 → NULL |
| `fc.c` | `fc_stripe_release_parity` 的 `parity_id` 越界、完成路径 `completed_id` 的冗余范围检查（`fc_complete_from_nand` 已断言过） |

**保留返回码**：`fc_command_configure_descriptors` / `fc_submit` / `fc_command_release` 三个顶层生命周期入口的句柄校验（指针身份 + 生命周期状态，有 `FC_STATUS_INVALID_ARGUMENT` 文档契约）。`fc_command_release` 的 `command_id >= FC_COMMAND_POOL_DEPTH` 保留，因它在调用 `fc_command_id_to_command()` 之前先挡一道。

头文件 `fc_interface.h` 的两条"越界返回 NULL"注释同步改为前置条件描述。

## 四之二、FC + NAND "全部入口守卫"补扫

辨析 D 只处理了 FC 自铸句柄的越界。用户随后指出 `fc_command_status_set_cw` / `fc_scheduler_select_chain` / `nand_dispatch_poll` 等**入参 NULL/值域**守卫也还是返回码，要求把 FC + NAND 两层的入口守卫一次性扫干净。

**判据 1 再次收窄**（`assertion_guidelines.md` §一）：边界判定标准从"函数声明在哪个头文件"改为"**值从哪来**"——调用者自己铸出/自己算出/自己持有的值 → 判据 2 转 `ASSERT`，即使该函数声明在 `nand_dispatch.h` / `fc_interface.h` 这类边界头里。唯一例外是 host / CLI / 介质读回的**外部输入**，以及"指针身份 + 生命周期状态"这类文档化入口契约。

转换点（FC）：

| 文件 | 转换点 |
|---|---|
| `fc_command.c` | `fc_command_status_init` 的 `status`、`fc_command_status_set_cw` 的 `status`/`cw` |
| `fc_scheduler.c` | `fc_scheduler_init`（scheduler/sq/hsq）、`fc_scheduler_select`（scheduler/command_id）、`fc_scheduler_select_chain`（scheduler/chain） |
| `fc_registers.c` | `fc_registers_reset` 的 `registers` |
| `fc_raid_engine.c` | `fc_raid_init_ctx` 的 `lun`；`fc_raid_start/accumulate/end` 的 `lun`/`cw_buffer`/`parity_buffer`/`total`；`fc_raid_recover` 的 `recovered`/`buffers`/`count`/`failed_idx`/`length` |
| `fc_random_engine.c` | 三种 `fc_random_transform_impl` 的 `data`；XORSHIFT 的 `seed == 0U`（种子生成器保证非零） |
| `fc_crc_engine.c` | `fc_crc_compute`/`fc_crc_verify` 的 `user_data`（保留 `g_crc_ready`） |
| `fc_ldpc_engine.c` | `fc_ldpc_encode/decode` 的 `data`/`data_length`/`data_length < 8U`（保留 `err_bits` 语义分支、`g_bch/g_qc` 惰性初始化） |
| `fc_ldpc_qc_engine.c` | `fc_ldpc_qc_decode` 的 `qc`/`data`（保留 `g_initialized`） |
| `fc_data_path.c` | `fc_data_path_encode`/`process_read`/`raid_recover` 的 `command`/`cmd`/`desc_id`/`failed_cw_idx` |
| `fc_interface.c` | 三个顶层生命周期入口的 `command == NULL`（保留身份/状态返回码）、`fc_poll_completion` 的 `completion`、`fc_buffer_get_meta` 的 `command`、`fc_write_cmd_data`/`fc_read_cmd_data` 的 `command`/`data`/`desc_id`、`fc_get_cmd_data_ptr` 的 `command`/`desc_id`、`fc_get_resource_stats` 的 `stats` |

转换点（NAND）：

| 文件 | 转换点 |
|---|---|
| `nand_dispatch.c` | `nand_dispatch_submit` 的 `request`/`ch`/`ce`/`lun`/`cw_count`（保留 `g_nand_device_ready` 闸门）、`nand_dispatch_submit_chain` 的 `requests`/`count`/`ch`/`ce`/`lun`、`nand_dispatch_poll` 的 `completion` |
| `nand_fmc.c` | `nand_fmc_init_instance` 的 `config`/`ch`/`ce`/`lun`、`nand_fmc_backend_get_ext` 的 `ch`/`ce`/`lun` |
| `nand_mt29f4g08abadawp.c` | `nand_fc_execute`/`nand_fc_start`/`nand_fc_step` 的 `transaction`/`operation`/`request`/`completion`/`active`（保留 `map->uops == NULL` 表缺项）、`nand_fc_read_id` 的 `id`、`nand_fc_set_features` 的 `feature_data` |

**保留返回码**：`fc_command_validate`（校验器）、`fc_queue.c` 的哨兵值域、`fc_command_alloc_internal`/`fc_descriptor_alloc_internal` 的池空、`fc_buffer_is_occupied`/`fc_buffer_release` 的生命周期、`g_nand_device_ready`/`g_crc_ready`/`g_initialized`/`ctx->active` 运行时状态、`map->uops`/`handler` 表完整性、`err_bits`/`command`/`pop_n(ids)` 的 NULL 语义分支、desc 链遍历带 `break`/`return` 的降级、`fc_bch_init` 的 `m`/`t` 值域。

**删用例**：`test_fc_level1.c` 删 `status_init_null`/`status_set_cw_overflow`/`status_set_cw_null`/`scheduler init_null`/`scheduler select_null`/`submit_null`/`poll_completion_null`/`configure_descriptors_null`；`test_fc_level2.c` 删 `encode_null`/`process_read_null`/`raid_recover_null`。

## 五、验证

| 项 | 结果 |
|---|---|
| `make debug` | 零告警零错误（149156 B） |
| `make tests` | 零告警零错误（203084 B） |
| `make release` | 零告警零错误（87284 B） |
| `make format-check` | 通过 |
| `grep -rn 'ASSUME(' src/` | 0（全工程） |
| 上板三套 munit + CLI 冒烟 | **待做** |

> ⚠️ `build/firmware/debug` 被 `make debug` 与 `make tests` 共用（`config.mk:44` 的 `BUILD_DIR` 不含 `FC_NAND_TEST_ENABLE`）。先跑 debug 再跑 tests 会复用陈旧对象，**必须 `make clean`**。

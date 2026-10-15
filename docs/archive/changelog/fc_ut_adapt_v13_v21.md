# FC 三级 UT 适配 v13~v21 接口/策略收敛

- **日期**：2026-10-08
- **范围**：`tests/unit/test_fc_level1.c` / `test_fc_level2.c` / `test_fc_level3.c`
- **背景**：v13~v21 一连串接口与策略收敛（策略层上移 FTL、命令身份去缓存、desc 链所有权、入口守卫全面转 ASSERT）之后，三级 UT 自 v15 起就没再编译过——`make -k tests` 报 **29 error / 7 warning**，全部落在 `tests/`。

## 结果

| 指标 | 适配前 | 适配后 |
|---|---|---|
| `make tests` | 29 error / 7 warning（无法链接） | **0 error / 0 warning，链接通过**（197468 B） |
| RAID=1 变体（`-DCONFIG_FC_RAID=1`） | — | 0 error / 0 warning，链接通过（206524 B） |
| `make format-check` | — | passed |
| Level 1 用例 | 27 | **16** |
| Level 2 用例 | 12（+RAID 5） | **10**（+RAID 6） |
| Level 3 用例 | 9 | **9** |

> 上板未做（项目硬规则：`make tests` 上板由用户执行）。

## Level 1：删 11 / 重写 4

| 用例 | 处置 | 原因 |
|---|---|---|
| `validate_null` / `validate_desc_needed` / `validate_cw_mask` | 删 | `fc_command_validate` v20 整函数转 ASSERT，配置错误不再返回码，无返回码可测 |
| `validate_op_range` | 重写 | 只保留「0..OP_COUNT-1 合法 → 恒 OK」 |
| `scheduler_set_clear_busy` / `select_empty` / `select_hsq_first` / `select_sq_order` / `select_busy_skip` / `starvation` / `select_chain_single` / `select_chain_multi` | 删（8 条） | v8 起 SQ/HSQ 选择逻辑搬进 `fc.c` 的 `static fc_poll_fill`，只能经 `fc_service()` 驱动真 NAND 才可达；Level 1 不再能覆盖 |
| `status_init_cw1` / `status_init_cw4` | 重写为 `status_init_pool_cmd` / `status_init_stack_cmd` | v15 删 `fc_command_t.command_id`；改为「池内命令 → `fc_command_to_id(cmd,&id)` 反查 + 断言 `cq_entry.command_id == 派生 id`」/「栈上命令 → `FC_COMMAND_ID_INVALID`」 |
| `scheduler_init` | 重写 | 只断言 `sq` / `hsq` 指针绑定 |

新增 `setup_fc_suite`（`fc_init()` 一次性）——`status_init` 两例需命令池。删掉 `g_scheduler_inited` / `setup_scheduler_suite` 与 `#include "common/log.h"`。

## Level 2 / Level 3：三处同源改动

### 1. 建链入口改名（v13）

`fc_command_configure_descriptors`（复数）已删，策略上移 FTL。全部改为 `ftl_cmd_configure_descriptors()`（返回 `ftl_status_t`，成功为 `FTL_OK`），补 `#include "ftl/ftl_fc_interface.h"`。Level 2 五处、Level 3 十四处。

语义等价：掩码为 0 时按 `config.cw` 对每个 plane 展开连续 CW；非 0 时按 flat 绝对编号逐位建链。

**三条 `cw_mask_*` 用例留在 Level 2**（用户拍板）——它们验证的是 desc 链几何（plane / CW 展开与链序），不是 NAND 行为。

### 2. 删掉双释放 helper（真 bug）

Level 2 的 `release_cmd_buffers()` 与 Level 3 的 `release_command_buffers()` 沿 desc 链手动调 `fc_buffer_release()`；而紧随其后的 `fc_command_release()` **自 v16 起也沿链把 desc + buffer 一起还池**。于是每个 buffer 被**二次释放**——`fc_buffer_release_internal` 无占用检查，第二次释放把 `buffer_free_next[id] = id` 写成自环，污染空闲链。

删 Level 2 helper + 14 个调用点、Level 3 helper + 13 个调用点，只留 `fc_command_release()`。自 v15 起潜伏（那之后 level2/3 从未编译过），上板必炸。

### 3. `command_id` 引用（v15）

Level 3 四处 `cmd->command_id` 改为：
```c
fc_command_id_t expect_id = FC_COMMAND_ID_INVALID;
munit_assert_uint32(fc_command_to_id(cmd, &expect_id), ==, 1U);
munit_assert_uint32(completion.command_id, ==, expect_id);
```

## Level 3 另两处

- `nand_dispatch_set_device_ready(1U)` → `nand_dispatch_set_armed(1U)`：`device_ready` 闩锁全仓已整体改名为 armed（`nand_dispatch_set_armed` / `nand_dispatch_is_armed`）。本套件走异步路径，闩锁必须为 1。
- `test_generate_stripe_parity` 删死局部 `uint8_t cw;`（RAID=1 专属，`-Wunused-variable`）。

## 编译陷阱：`fc_status_t` ≠ `ftl_status_t`

两者是**不同 enum**，互相赋值触发 `-Wenum-conversion`（`-Wextra` 下是活的）。因此凡 `result` 变量后续还要接 `fc_submit()` / `fc_command_release()`（返回 `fc_status_t`）的用例，不能把 `result` 的类型改成 `ftl_status_t`，只能：

- **内联断言**：`munit_assert_int(ftl_cmd_configure_descriptors(cmd), ==, FTL_OK);`，或
- 另立 `ftl_status_t` 变量。

另：`expect_id` 必须声明为 `fc_command_id_t` 而非 `int`——`munit_assert_uint32` 走 `_Generic`，类型不符直接编译错。

## 未跟进

`tests/integration/test_fc_nand.c` 仍引用已删的复数建链入口（5 处）与 `command->command_id`（3 处），但它**不在 `tests` 目标里**——`src/fc/makefile.mk` 只编 `tests/unit` 的 level1~3 + `ut_main` + `munit`。属遗留未编文件，本轮未动。

## 验证

```bash
make clean && make -k tests                      # 0 error / 0 warning，链接通过
make -k BUILD_TARGET=firmware FC_NAND_TEST_ENABLE=1 \
     CFLAGS_EXTRA="-DMUNIT_ENABLE_TIMING -DCONFIG_FC_RAID=1"   # 同上
make format-check                                # passed
```

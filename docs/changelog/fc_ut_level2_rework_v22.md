# v22：Level 2 UT 重构——fixture 修正 + RAID 退化用例清理 + 覆盖补齐

- **日期**：2026-10-08
- **范围**：`tests/unit/test_fc_level2.c`（`g_fc_data_path_tests`）+ 文档
- **前置**：v1.8~v1.10（`fc_ut_adapt_v13_v21.md` / `fc_ut_level1_logic_merge_v21.md`）——三级 UT 已恢复 0/0 编译，Level 1 刚做完一轮「死代码用例」清理。本轮把同样的审视推到 Level 2。

## 背景

走读 Level 2（953 行 / 默认 10 条 + RAID 6 条）发现三类问题，用户确认全部处理：

1. **fixture 退化，用例名不副实**：`alloc_cmd` 从不设 `cmd->paa`（`fc_command_alloc` 用 `(fc_command_t){0}` 清零 → `paa = 0`），单 plane 建链分支（`fc_interface.c:189`）把 `desc->paa` 落成 `cw` 本身。BASIC 种子（`fc_random_engine.c`）随之退化成 `seed == cw` → `{1,1,2,3}`，LFSR 低 16 位取这几个小值后 **keystream 除 byte0 外全 0**（主机侧复算验证：`K0..K3` 仅 byte0 非零）。于是**所有 `random=ON` 用例的加扰都是空操作**——把 `fc_random_transform` 从 encode 里整段删掉，它们照样通过。
2. **RAID 组 5 条靠三重巧合通过**：`fc_data_path_raid_recover` 全仓**零生产调用者**（唯一调用者是测试），且结构上只收集单条命令的 desc 链（`fc_data_path.c:591`），而 parity CW 在**另一条**命令的链上（`fc.c:888/924-933`）——跨命令适配是 `fc_raid_engine.h:22` 的未办事项。`encode_raid_on` 与 `raid_recover_first/mid/last` 只能靠「无 parity + pattern 异或恰为 0（`0x10^0x11^0x12^0x13`）+ keystream 退化」通过；**修 fixture 后 random=ON 的几条会直接挂**。
3. **覆盖缺口**：数据路径用例一律不设 `cmd->param.cw_mask`（保持 0），三条 `cw_mask_*` 只查链几何不跑数据路径 → Pass 3（CRC_ERROR）、Pass 4（MATA_ERROR）、非连续掩码 + 数据路径、`CRC_DISABLE`、`LDPC_RAW` 全零覆盖。其中 Pass 4 的 `paa_to_cw(desc->paa)`（`fc_data_path.c:541`，代码注释专门警告"不能用链走位"）正落在「掩码测试与 process_read 互不相交」的空白里。

## 结果

| 指标 | 改前 | 改后 |
|---|---|---|
| Level 2 用例（默认） | 10 | **15** |
| Level 2 用例（`CONFIG_FC_RAID=1`） | 16 | **17** |
| `make -k tests` | 0 error / 0 warning，193856 B | 0 error / 0 warning，**197344 B** |
| RAID=1 档 | — | 0 error / 0 warning，**204328 B** |
| `make format-check` | passed | passed |

> 上板未做（项目硬规则：`make tests` 上板由用户执行）。

## 1. fixture：给命令真实页地址

新增 `#define L2_PAA_PAGE (32U)` / `L2_PAA_BLOCK (50U)` 与统一建链入口：

```c
static fc_command_t *alloc_cmd_full(uint8_t multi_plane, uint8_t cw_count, uint32_t cw_mask,
                                    uint8_t random_mode, uint8_t raid_mode);
static fc_command_t *alloc_cmd(uint8_t cw_count, uint8_t random_mode, uint8_t raid_mode);
/* ↑ 退化为 alloc_cmd_full(0U, cw_count, 0U, random_mode, raid_mode) */
```

`alloc_cmd_full` 设 `paa = paa_make(32, 0, 50, 0, 0)`、`operation = OP_NORMAL_PROGRAM_PAGE`、`param.cw_mask`、`config.multi_plane`。取 (page=32, block=50) 使 4 个 CW 的种子 = `0x6468..0x646B`，互异且远离小值，LFSR 才跑得出满周期 keystream（`page=32 < 64`、`block=50 < 2048`，满足 `paa_is_valid`）。

现有 11 处 `alloc_cmd(...)` 调用点签名不变；3 条 `cw_mask_*` 从各自的 `fc_command_alloc()` 手填改走 `alloc_cmd_full`。

**并补自证断言**：`random=ON` 的用例在 encode 之后、解扰之前加一条「USER 区 ≠ 明文」——否则 fixture 下次再退化，"解扰后等于明文"这个恒等式仍然会把问题原样盖过去。

## 2. RAID 组：删 4 留 2

| 用例 | 处置 | 原因 |
|---|---|---|
| `encode_raid_on` | 删 | 名不副实：encode 自 Stage 3 移除后不做 RAID；内容与 `do_raid_recover_test` + `generate_stripe_parity` 重复 |
| `raid_recover_first` / `_mid` / `_last` | 删 | 构造不出真 stripe（无 parity），只靠三重巧合通过；修 fixture 后 random=ON 必挂 |
| `raid_recover_no_random` | **留** | random=OFF → 无加扰，退化 stripe 成立 |
| `generate_stripe_parity` | **留** | parity 命令真实参与 XOR（真 stripe） |

`do_raid_recover_test` 随之只剩一个调用者，保留但重写注释，如实写明单链局限并指向 `fc_raid_engine.h:22`。判据固化为 `assertion_guidelines.md` **§十七·判据 6**（用例构造不出被测函数的真实前置条件时，退化的用例应删）。

## 3. 覆盖补齐：新增 5 条

| 用例 | 构造 | 断言 |
|---|---|---|
| `encode_crc_disabled` | `config.crc = FC_CRC_DISABLE`，[4,8) 预置 `0xFF` 哨兵 | 哨兵原样保留（encode 确未写 CRC）；读侧 Pass 3 跳过 → 全 `READY` |
| `process_read_masked_chain` | `alloc_cmd_full(2, 1, 0x05, ...)` → 链位 0,1 ↔ CW 0,2 | encode/process_read 均 OK、全 `READY`、pattern 还原。**专钉 Pass 4 用 `paa_to_cw(desc->paa)`**——若改用链走位，第 2 条 desc 会拿 `cw=1` 算期望 META 而假报 `MATA_ERROR` |
| `process_read_mata_error` | 同上掩码，encode 后改 `cmd->meta = 0x10` | 返回值仍 OK，但每个 CW 置 `MATA_ERROR`（Pass 4 此前零覆盖） |
| `process_read_crc_error` | `crc` 写关读开，encode 前把 [4,8) 置 `0xDEADBEEF` | 返回值仍 OK，但每个 CW 置 `CRC_ERROR`（Pass 3 此前零覆盖）；ECC 未被扰动，触发确定 |
| `process_read_ldpc_raw` | `config.ldpc = FC_ECC_MODE_LDPC_RAW` | encode 跳过 ECC、Pass 1 跳过解码，数据原样往返、全 `READY` |

## 4. 断言加强

`process_read_ok_random_on` / `_off` / `_single_cw` 补 `desc->status == FC_CW_STATUS_READY`。理由：`fc_data_path_process_read` 在 Pass 3 / Pass 4 标脏后**仍返回 `FC_STATUS_OK`**（`fc_data_path.c:551`），只查返回值会漏掉 CRC / META 校验回归——新加的 `process_read_mata_error` / `crc_error` 正是靠这一条才立得住。

## 5. 去冗余

- 删 `fill_pattern`（纯 `memset` 包装）与 `print_resource_stats` 注释块及其调用点。
- 新增 `cw_desc(cmd, idx)`，把 `get_cw_buffer` / `get_cw_base` / `get_cw_count` 收敛成「一次走链 + 三个取值器」，并消掉 3 处逐字重复的「走链 i 次取第 i 个 desc」（原 `:285-300`、`:357-372`、`:758-773`）；再加 `cw_seed(cmd, idx)` 收掉解扰种子的重复推导。
- 修注释：`test_encode_random_off` 的「前 520 字节」→「USER 区 512B（[0,8) 是 META+CRC）」；文件头两张参数组合表按实际重写；`alloc_cmd` 的 `raid_mode` 形参加注「数据路径不读，唯一读者是 `fc.c` 的分派钩子」。
- `test_raid_recover_last` 的 `3U` 魔数随用例一并消失。

## 6. 一处计划外偏差：套件级 setup 不存在

原计划把 `setup_level2` 从每个用例条目挪到 `MunitSuite.setup`。实施前查证发现**本 fork 的 `MunitSuite` 结构体根本没有 `setup` 字段**（`tests/framework/munit.h:442`，字段只有 `prefix` / `tests` / `suites` / `iterations` / `options`），`munit.c` 里唯一被调用的是 `test->setup`（`munit.c:704`）。故**保持逐用例挂 `setup_level2`**，靠 `g_level2_inited` 幂等守卫退化成一次性 `fc_init()`；已在文件里写明这一约束，避免后来者再"顺手挪上去"。

## 未跟进

1. **RAID 单链局限本身**：`fc_data_path_raid_recover` 仍只能收单命令链，跨命令适配（`fc_raid_engine.h:22`）未做。本轮只把"用例不再假装覆盖它"落地，代码缺口原样保留。
2. **doc/code 表述分歧（stripe XOR 宽度）**：`fc_nand_design.md:163/166/1070/1178` 统一称 RAID XOR 覆盖**整 528B**；代码里两个方向并不对称——`fc_data_path_generate_stripe_parity()` 只 XOR **520B**（`FC_BUFFER_DATA_SIZE`，`fc_data_path.c:304/312`）再重算 parity，而 `fc_raid_recover()` 按调用者给的宽度 XOR（本层测试传 528）。两者靠 **ECC 线性**（`ECC(D1)^ECC(D2) == ECC(D1^D2)`）等价，都正确，但文档统一写 528 不够精确。本轮不修。
3. **`tests/integration/test_fc_nand.c` 已失效**：该文件 5 处调用 `fc_command_configure_descriptors`（**复数**）——该符号 v13 已删、`src/` 与 `include/` 中不存在，文件本身也不在 `TEST_C_SRCS` 里（`src/fc/makefile.mk` 只收 `tests/framework` + `tests/unit`）。属用户维护的上板测试，本轮不动。

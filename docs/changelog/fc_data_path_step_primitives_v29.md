# v29：数据路径步骤函数化——7 个单 CW 原语 + 步骤级用例参数化 + munit 移植修复

- **日期**：2026-10-09
- **范围**：`include/fc/fc_data_path.h`、`src/fc/fc_data_path.c`、`tests/framework/munit.{c,h}`、
  `tests/unit/test_fc_level2.c`、文档
- **前置**：v28（CW 集合退出命令结构体）、v26（Level 2/3 共享夹具 `fc_test_common`）
- **介质格式**：**无变更**——纯提取，产物逐字节不变（见「影响面核查」）。
- **上板未验**：本项目硬规则。
- **测试**：本轮**未编译**（用户要求「重构测试代码期间不编译」）。`make format-check` 通过（101 文件）；
  `-fsyntax-only`（套 makefile 全量告警集）对 `test_fc_level2.c` 与 `munit.c` 均 0 warning 0 error。
  ⚠ `make tests` **仍编不过**——`test_fc_level3.c` 还在引用夹具里已删的旧符号（v27/v28 遗留的中间态，
  见「未跟进」）。

## 背景

用户提出的改法：把 `fc_data_path_encode` / `fc_data_path_process_read` 里**各算法步骤拆成单 CW 原语**，
Level 2 直接拿一块 buffer 调这些原语。于是**「调了哪些步骤」就是「开了哪些功能」**——
不需要命令、desc、链，也不需要任何 config 位。

原状态下的代价（以 CRC 用例为例）：要验「只开 CRC」，得先建链，再把 `cmd->config.ldpc` 设成
`FC_ECC_MODE_LDPC_RAW` 才能让 ECC 别把注入的 bit 纠回去——**「只开 CRC」这个意图被翻译成两三个
字段的取值**，读用例的人得反推。

附带收益：`fc_ldpc_config_t` 原先在 `fc_data_path.c` 里**写了多遍**字面量 `{520, 8, 4}`。

## 改动

### A. 新增 7 个单 CW 原语（`fc_data_path.h` / `fc_data_path.c`）

一律只操作一块 528B 裸 CW buffer（`[META:4][CRC:4][USER:512][ECC:8]`），不碰 command / desc / 链。

| 原语 | 提取自 | 行为 |
|---|---|---|
| `void fc_data_path_meta_write(uint8_t *cw, uint32_t meta)` | Stage 0 | `memcpy(cw, &meta, 4)` |
| `void fc_data_path_crc_write(uint8_t *cw)` | Stage 1a | `fc_crc_compute(cw + 8)` → 写 `cw[4..8)` |
| `void fc_data_path_scramble(uint8_t *cw, uint32_t seed)` | Stage 1b | `fc_random_transform(cw + 8, 512, seed)` |
| `fc_status_t fc_data_path_ecc_encode(uint8_t *cw)` | Stage 2 | `fc_ldpc_encode(&CFG, cw, 520)`；`NOT_SUPPORTED` 吞成 OK |
| `fc_status_t fc_data_path_ecc_decode(uint8_t *cw, uint8_t *ecc_bits)` | Pass 1 | `fc_ldpc_decode(&CFG, cw, 520, ecc_bits)`；**不吞** `NOT_SUPPORTED` |
| `uint8_t fc_data_path_crc_check(const uint8_t *cw)` | Pass 3 | 1 = 重算值 == `cw[4..8)` |
| `uint32_t fc_data_path_meta_check(const uint8_t *cw, uint32_t command_meta, nand_paa_t paa)` | Pass 4 | 返回 `FC_CW_STATUS_MATA_ERROR` 或 0 |

**Pass 2 不单独出函数**：加扰与解扰是同一个 XOR 变换（自反），两个名字 = 两处实现 = 漂移风险。
读侧就是再调一次 `fc_data_path_scramble`，种子由管线从页内明文 META 自举。

`meta_check` 取 `nand_paa_t paa`（而非 plane/cw 两个数）：自洽校验要的 `(plane << PBA_PLANE_SHIFT) | cw`
与 LBA 校验要的 `lba_to_meta(command_meta, plane, cw)` 都从 `paa_to_plane` / `paa_to_cw(paa)` 取——
与迁移前逐字一致，且**必须**用 `paa_to_*` 而非链走位（`cw_bitmap` 不满时两者不同，v22 已确立）。

### B. 两个管线函数退化为编排

`fc_data_path_encode` / `fc_data_path_process_read` 每个 stage 的循环体从「取 buffer + 内联算法」
变成「取 buffer + 调原语」。**行为逐字不变**——这是纯提取。

**留在管线里**（步骤级用例永远验不到，后续另行覆盖）：gate 三个 `if` / Pass 2 的 `if (all_ok)` /
链遍历 / `desc->meta` 写回 / `desc->status` 赋值与 `|=` / `return FC_STATUS_IO_ERROR` / ECC 错误透传。

### C. `fc_ldpc_config_t` 收敛为一处

```c
static const fc_ldpc_config_t FC_DATA_PATH_LDPC_CFG = {
    .codeword_bytes = FC_BUFFER_DATA_SIZE,
    .parity_bytes   = 8U,
    .max_iterations = 4U,
};
```

`ecc_encode` / `ecc_decode` 共用。原先 `{520, 8, 4}` 在 encode / process_read / stripe_parity /
raid_recover 各写了一遍字面量。

### D. 步骤级用例（`tests/unit/test_fc_level2.c`）

三条步骤级用例（不建链、不需要 config 位），全部 `MUNIT_TEST_OPTION_NONE`：

| 用例 | 形态 | 覆盖 |
|---|---|---|
| `crc_step_detect` | **参数化** 4 case | 钉 CRC 的**覆盖边界**（见下） |
| `ecc_step` | **参数化** 11 case | BCH(m=13,t=4)：1..4 bit 必精确纠正；≥5 bit 走 BOUND（不钉结果，只钉两条不变量） |
| `scramble_whiten` | 单函数内 `kind=0..2` 循环 | 全 0x00 / 全 0xFF / PAA 重复三种输入的白化度 + 自反性 |

`crc_step_detect` 的 4 个 case 是本轮的实质收获：

| case | 注入区间（CW 内偏移） | 期望 `crc_check` |
|---|---|---|
| `user-1` | `[8, 520)` | `0` 必检出 |
| `crc-field-1` | `[4, 8)`（存的值本身） | `0` 必检出 |
| `meta-1` | `[0, 4)` | `1` **必不检出** |
| `parity-1` | `[520, 528)` | `1` **必不检出** |

依据：`fc_data_path_crc_check` 存的 CRC 读自 `cw + FC_CRC_META_SIZE`，算的 CRC 吃
`cw + FC_CRC_USER_OFFSET` 的 `FC_CRC_USER_SIZE = 512` 字节（`fc_crc_engine.c:125` 的循环上界）。
META 与 parity 两头**结构性**在覆盖之外。**后两条是负例**——没有它们，一个「把整块 528B 都拿去算
CRC」的错误实现照样能过。`crc-field-1` 的机制也不同：坏的是**存的值**，不是被校验的数据。

### E. munit 移植修复（3 处，对照上游 nemequ/munit）

| 位置 | 修复前 | 修复后 |
|---|---|---|
| `munit.c` | 无 `munit_parameters_get()` | 补回（上游逐字同名同实现） |
| `munit.c` `run_test_runner_run_test_wild` | 递归传 `p`（同一参数上无限递归） | 传 `next` |
| `munit.c` `munit_test_runner_exec` | `iterations = runner->iterations;`（自赋值，套件级 `iterations` 字段失效） | `iterations = runner->suite->iterations;` |

**只有第一条会挡住写参数化用例**——runner 早已把 `params[]` 填好，缺的只是按名取值的函数；
补之前用例得自己手写那 8 行循环。第二条是潜伏 bug：只在**≥2 个参数枚举**时触发，单枚举照常工作；
全仓此前零个用例带参数（`tests/unit` 有 48 处 `(void)params;`、零个消费者），所以没人踩到。
第三条**当前观测不到差别**——所有套件的 `iterations` 都是 1，而 `iterations == 0` 时 `do-while`
也正好只跑一遍；修的是「套件级字段终于生效」。

`--param name value` 这条 CLI 链在补 helper **之前就是通的**（`munit.c:1442` 解析 → `:1105` 比对
枚举名 → 填 `params[]`）。⚠ 值**不做合法性校验**，原样塞进 `params[]` ⇒ 打错场景名上游会照跑；
两条步骤级用例各自 `munit_assert_true(c != NULL)` 把它变成显式失败。

## 影响面核查

- **生产行为中性**：纯提取。7 个原语的函数体逐字来自原内联代码；`FC_DATA_PATH_LDPC_CFG` 的值
  与原四处字面量逐字段相同。**产物逐字节不变**。
- **`fc_data_path.h` 新增 7 个非 static 函数** ⇒ 它们进入符号表，但**无生产调用者**
  （唯一调用者是单测）——这是 §十七 判据 5 的**显式例外**：它们不是「失去调用者的死代码」，
  而是**为可测性刻意拆出的接口**，与管线共用（管线也调它们）。
- **铁律不变**：提交出去的命令**绝不能再手工调**这些原语——FC 自己会跑整条管线，重复编码 = 数据损坏。
  已写进 `fc_data_path.h` 的头注与 `fc_test_common.h`「五」。
- **步骤级用例不建链** ⇒ 不占 desc/buffer 池，只借 `test_cw_scratch()` 给的 528B 暂存区。

## 验证状态

| 项 | 结果 |
|---|---|
| `clang-format-15 -i` | 已跑 |
| `make format-check` | **passed（101 文件）** |
| `-fsyntax-only`（makefile 全量告警集 + `-DFC_NAND_TEST_ENABLE=1 -DMUNIT_ENABLE_TIMING`） | `test_fc_level2.c` / `munit.c` 均 **0 warning 0 error** |
| `make tests` / `make debug` / `make release` | **未跑**（用户要求重构期不编译） |
| 上板 | **未验** |

## 未跟进

1. **`test_fc_level3.c` 仍在旧符号上**（`test_page_cmd_ex` / `test_run_lifecycle` /
   `test_fill_user_from_source` / `test_verify_against_source` / `test_submit_wait`）⇒ `make tests`
   编不过。夹具侧的替代品已就位（`test_cmd_build` + 5 个页 helper），Level 3 迁移是下一轮。
   `test_fc_level3.c:280` 的 `CMD_FIELD_U(param.cw_bitmap)`（在 `#if 0` 块里）迁移时须一并删。
2. **random / meta 的步骤级用例**未加：`scramble_whiten` 验了白化度与自反性，但没验「种子取自
   META 低 3 位」这条自举契约；`meta_check` 的两项校验只有管线级（`process_read_mata_error`）覆盖。
3. **管线级用例**（gate 三个 `if` / Pass 2 条件 / 返回码）仍需建链，未动。
4. `test_process_read_crc_error`（哨兵版）与步骤级 `crc_step_detect` 覆盖重叠，去留待定。

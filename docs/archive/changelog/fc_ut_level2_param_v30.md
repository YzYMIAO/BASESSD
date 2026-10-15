# v30：Level 2 三组用例参数化——encode / process_read_ok / cw_bitmap（判据 7 的追加应用）

- **日期**：2026-10-09
- **范围**：`tests/unit/test_fc_level2.c`、文档（`fc_nand_ut.md`、`assertion_guidelines.md`）
- **前置**：v29（数据路径步骤函数化 + 步骤级用例参数化 + munit 移植修复）
- **生产代码**：**零改动**——纯测试重构。
- **介质格式**：无变更。
- **上板未验**：本项目硬规则。
- **测试**：**未编译**（用户要求重构期不编译）。`make format-check` 通过（101 文件）；
  `-fsyntax-only`（套 makefile 全量告警集）对 `test_fc_level2.c` **0 warning 0 error**。
  ⚠ `make tests` **仍编不过**——`test_fc_level3.c` 在旧符号上（v27~v29 遗留中间态，本轮未动）。

## 背景

v29 确立了判据 7（**不同分支/期望 → 参数化；同一断言、不同输入 → 迭代**）并把它用在三条步骤级
用例上。本轮的起点是用户指出：encode 组（4 条）与 process_read 组（8 条）里也有可以合并的。

判据不是「能合就合」——要按**断言结构**判，不能按名字像不像判。把 12 条的函数体摊开读：

| 组 | 用例 | 断言结构 | 结论 |
|---|---|---|---|
| encode | `random_off` / `random_on` / `single_cw` | 同一套（`cw_count` / META / CRC / parity / USER 区），**只有两列期望不同**（cw 数、USER 区是否仍是明文） | **参数化** |
| encode | `crc_disabled` | **另一套**（查 `[4,8)` 哨兵未被写，无 parity / USER 断言） | 独立 |
| process_read | `ok_random_on` / `ok_random_off` / `single_cw` | 三条**各只有一行** `l2_buffer_round_trip(bitmap, random, crc)`，断言全在 helper 内 | **参数化** |
| process_read | `unc` / `masked_chain` / `mata_error` / `crc_error` / `ldpc_raw` | 各自断言**不同的可观测量**（返回码 / 不同的 `status` 位 / 不同的哨兵区） | 独立 |
| cw_bitmap | `flat_mixed` / `plane1_only` / `span_multi_plane` | 三条共用「建链 → 收链 → 断言 desc 数 → 逐条断言 (plane, cw)」骨架，差异全在**段宽 / `addr.plane` / 位图** | **参数化** |

关键一条：`l2_buffer_round_trip` 的**三个形参恰好就是那三条用例唯一的差异**——helper 早已把这组
拆好了，剩下三个包装器是空的。

## 改动

### A. `encode`：3 条 → 1 条参数化（`test_fc_level2.c`）

```c
typedef struct
{
    const char *name;
    uint32_t    cw_bitmap;
    uint8_t     random_mode;
    uint8_t     crc_mode;
    uint8_t     n_cw;         /* 期望链上 desc 数 */
    uint8_t     expect_plain; /* 1 = encode 后 USER 区仍等于明文 */
} l2_enc_case_t;

static const l2_enc_case_t l2_enc_cases[] = {
    { "random-off-4cw", L2_BM_PLANE0_ALL, FC_RANDOM_DISABLE, FC_CRC_ENABLE, CW_NUM, 1U},
    {  "random-on-4cw", L2_BM_PLANE0_ALL,  FC_RANDOM_ENABLE, FC_CRC_ENABLE, CW_NUM, 0U},
    {      "single-cw",             0x01U, FC_RANDOM_DISABLE, FC_CRC_ENABLE,     1U, 1U},
};
```

`expect_plain` 把原 `random_on` 的**自证断言**（「encode 后 USER 区 ≠ 明文」）变成表里的一列，
语义不丢。`crc_disabled` 保持独立（断言集不同）。

### B. `process_read_ok`：3 条 → 1 条参数化

```c
static const l2_rt_case_t l2_rt_cases[] = {
    { "random-on-4cw", L2_BM_PLANE0_ALL,  FC_RANDOM_ENABLE, FC_CRC_ENABLE},
    {"random-off-4cw", L2_BM_PLANE0_ALL, FC_RANDOM_DISABLE, FC_CRC_ENABLE},
    {      "single-cw",             0x01U,  FC_RANDOM_ENABLE, FC_CRC_ENABLE},
};

static MunitResult test_process_read_ok(const MunitParameter params[], void *data)
{
    const l2_rt_case_t *c = l2_rt_case_find(munit_parameters_get(params, "case"));

    (void)data;
    munit_assert_true(c != NULL); /* --param case 传了表外的名字 */
    l2_buffer_round_trip(c->cw_bitmap, c->random_mode, c->crc_mode);

    return MUNIT_OK;
}
```

判据 7 在这里有一处值得写明的**选择**：三条断言相同、只有输入不同，字面属「迭代」。仍取**参数化**
的理由是 munit 的两条机制收益——① 失败时把 `case=…` 带进报错（内部循环只能靠 `munit_logf` 一行
日志）；② `--param case single-cw` 能单跑一条（这正是 v29 补 `munit_parameters_get` 换来的能力）。

### C. `cw_bitmap`：3 条 → 1 条参数化

```c
typedef struct
{
    const char          *name;
    uint8_t              addr_plane;
    uint32_t             bitmap;
    const fc_cmd_conf_t *conf;
    uint8_t              n;         /* 期望链上 desc 数 */
    const uint8_t       *plane_exp; /* 期望链：逐 desc 的 plane */
    const uint8_t       *cw_exp;
} l2_bm_case_t;

static const l2_bm_case_t l2_bm_cases[] = {
    {       "flat-mixed", 0U, 0x29U, &L2_PROG_MULTI,  3U, l2_bm_mixed_plane, l2_bm_mixed_cw},
    {      "plane1-only", 1U, 0xF0U, &L2_PROG_SINGLE, 4U,    l2_bm_p1_plane,    l2_bm_p1_cw},
    {"span-multi-plane", 0U, 0xFFU, &L2_PROG_MULTI,  8U,   l2_bm_span_plane,  l2_bm_span_cw},
};
```

**代价与取舍**：原 `plane1-only` / `span-multi-plane` 把期望写成**规则**
（`plane[i]==1 && cw[i]==i`、`plane[i]==(i<CW_NUM)?0:1 && cw[i]==i%CW_NUM`），规则本身是
「plane-major」「cw 循环」两条不变量的代码表达。改成显式表后，**不变量只剩注释在扛**。
仍合的理由：① 三行的 (段宽, `addr.plane`, 位图) 组合摆在一张表里，覆盖了哪几种、缺了哪种一眼可见；
② 逐 case 命名 + `--param case plane1-only` 单跑；③ 显式列表对失败诊断不差。

顺带**加强**一条：`cmd->config.cw == 0`（原只有 `flat_mixed` / `span_multi_plane` 有断言）对三条
场景**都成立**，统一加进参数化体。

### D. 三条参数化用例都不设 `SINGLE_ITERATION`

与 v29 的步骤级用例一致 ⇒ `--iterations N` 生效（默认仍 1）。

### E. 刻意不并的 5 条

`unc` / `masked_chain` / `mata_error` / `crc_error` / `ldpc_raw` 各自断言不同的可观测量。
`mata_error` 与 `crc_error` 的**断言形状**相同（逐 CW 查一个 `status` 位），但 setup 机制不同
（改 `cmd->meta` vs 翻 `config.crc` + 预置 4B 哨兵）——合表要给每行挂 setup 回调，两行 + 两个
回调不比两个函数清楚。

## 影响面

- **用例数与覆盖不变**：行数 18 → **12**（RAID 20 → **14**），但参数化展开后**实际执行的用例数
  仍是 31**（`crc_step_detect` 4 + `scramble_whiten` 1 + `ecc_step` 11 + `encode` 3 +
  `encode_crc_disabled` 1 + `process_read_ok` 3 + 其余 5 + `cw_bitmap` 3 = 31）。
- 参数化用例的每个 case 由 runner 各跑一次独立 `setup` / `tear_down` ⇒ 池状态互不串扰。
- **铁律不变**：本层一律 buffer 级、不提交；`l2_buffer_round_trip` 内部只调
  `fc_data_path_encode` / `process_read`，绝不手工调单 CW 原语（FC 自己会跑整条管线）。

## 顺带修正

同表两条**陈旧参数**（v26 起就与代码不符，本轮重写该表时一并订正）：

- `process_read_masked_chain`：表中写 `multi_plane=2`，实为**单 plane 段**（掩码只覆盖 plane 0 时，
  multi-plane 写路径会给未覆盖的 plane 补一条同行回退地址、静默编一页全 0xFF——v26 已改为单 plane）。
- `process_read_mata_error`：表中写 `mask=0x05`，实为 4CW（`L2_BM_PLANE0_ALL`）。

另修 `fc_nand_ut.md` 两处 `§二十二` → `§二十三` 的引用（判据 7/8 实际在 §二十三）。

## 验证状态

| 项 | 结果 |
|---|---|
| `clang-format-15 -i` | 已跑 |
| `make format-check` | **passed（101 文件）** |
| `-fsyntax-only`（makefile 全量告警集 + `-DFC_NAND_TEST_ENABLE=1 -DMUNIT_ENABLE_TIMING`） | `test_fc_level2.c` **0 warning 0 error** |
| `make tests` / `make debug` / `make release` | **未跑**（用户要求重构期不编译） |
| 上板 | **未验** |

## 未跟进

1. **`test_fc_level3.c` 仍在旧符号上** ⇒ `make tests` 编不过（v27~v29 共同的遗留中间态，见
   `fc_data_path_step_primitives_v29.md`「未跟进」1）。Level 3 迁移是下一轮。
2. `process_read_crc_error`（哨兵版）与步骤级 `crc_step_detect` 覆盖重叠——但前者钉的是
   **管线 Pass 3 的 gate**（`if (config.crc == ENABLE)`），步骤级验不到，暂留。
3. 管线级 gate 用例（encode 的两个 `if` / Pass 2 的 `if (all_ok)` / `return IO_ERROR`）仍需建链，
   未动。

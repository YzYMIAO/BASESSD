# v54：两条补测并入既有用例 —— `OP_READ_UNIQUE_ID`(op 3) + `fc_buffer_get_meta`

- **日期**：2026-10-11
- **范围**：**纯测试 + 文档**。`tests/unit/test_fc_level3.c`、`docs/design/fc_nand_ut.md`、本文档。
  **`src/**` 一行不动**（两条都是「已有 src、只缺用例」）。
- **介质格式**：无变更。
- **测试**：**用例数不变（仍 57）**——两条都**并入**既有用例，不新增注册行、不碰 `ut_main.c`。
  `make format-check` 过；`make clean && make tests` **0 warning 0 error**、
  `.bin` **240436 B**（v53 是 238980 B）。
- ✅ **上板已验**（2026-10-11，seed `0x5a4f21e4`）：完整 `test` **57 of 57 (100%)**、0 skipped——
  `init_sequence` **[ OK ] [ 328.22 us ]**（含 Step 5 `READ_UNIQUE_ID`，**op 3 首次上板被器件接受**）、
  `block_read_meta` **[ OK ] [ 40.16 ms ]**（含 `fc_buffer_get_meta` 链内复核）、
  `buf_own_release` 2.00 ms、`mixed_pipeline` 165.19 ms（其余 55 条逐字不变）。

## 〇、起因

在 FC 公开 API / OP 宏的覆盖体检里，列了三条「性价比最高的补测」，用户点掉了
`fc_get_cmd_ecc_max`（见 §三），留下这两条：

> 「性价比最高的三条补测：OP_READ_UNIQUE_ID（只读、零风险）fc_buffer_get_meta（gc 搬移用）
> 这两个看看能不能并入现有测试，不能并入的加测试用例」

两条**都能并入**既有用例 ⇒ 用例数不变。选它们而非别的，正因为：一条**零风险只读**
（op 3 与已覆盖的 op 2/4 共用同一 UOP 模板，却从未在板上跑过），一条是**生产真调用**
（`fc_buffer_get_meta` 的唯一调用者是 `gc.c`）。

## 一、`OP_READ_UNIQUE_ID`（op 3，`EDh`）→ 并入 `init_sequence`

**现状**：`grep OP_READ_UNIQUE_ID tests/` **无命中**——op 表里它与 `OP_READ_PARAMETER_PAGE`(2)、
`OP_GET_FEATURES`(4) **共用同一 UOP 模板**（`src/nand/onfi_1_0.c:185` 注释：
「共用相同的 UOP 序列，但命令字节不同」），`has_data = 1`、`data_length = 256`、1 地址周期，
却**从未作为命令下发过**。

**并入点**：`test_init_sequence` 是「逐条初始化 op 冒烟」的用例，已有 RESET / READ_ID /
READ_PARAMETER_PAGE / GET_FEATURES 四步 ⇒ 新增 **Step 5**，与 Step 3（参数页）**同型**：

```c
fc_command_t *cmd = test_cmd_build(&(nand_addr_t){.block = 0U, .plane = 0U, .page = 0U}, 0x01U, 0U,
                                   &L3_CONF(OP_READ_UNIQUE_ID, FC_CRC_ENABLE));
(void)test_submit_wait(cmd);
munit_assert_uint8(test_verify_cw_status_all_success(cmd), ==, 1U);   /* 硬判据 */
/* 16 字节 → uid_bytes[]，走证据摘要打出 */
```

**为什么不断言内容**：唯一 ID 是**出厂烧录的 16 字节**，本器件手册（MT29F4G08）未给死取值
⇒ 硬断言具体字节就是凭记忆说「协议要求」（见判据「协议断言必须先查证」）。
硬判据只到「命令 + CW 状态全 `SUCCESS`」；16 字节走新增证据行 `UNIQUE_ID(16B)=…`（INFO，默认档可见）
打出，**全 `0xFF` 即视为器件未实现该 ID**，由人工比对。

## 二、`fc_buffer_get_meta` → 并入 `block_read_meta`

**现状**：`uint8_t *fc_buffer_get_meta(fc_command_t *command, uint8_t cw_index)`
（`src/fc/fc_interface.c:384`）返回命令中 **CW 号 == `cw_index`** 的 CW 基址（offset 0 = META 4 字节），
按 `paa_to_cw(desc->paa)` 匹配**而非链走位**（flat CW 掩码下两者会分叉）。
**唯一生产调用者 = `src/ftl/gc.c:96`**：读页完成后 `fc_buffer_get_meta(cmd, 0U)` 取 CW0 的 META，
`>> CW_WIDTH` 得**页 LBA**（gc 搬移靠它定位源页）。

**并入点**：`test_block_read_meta` 本就是「META 回读」用例（op 11 无链只读回读 CW0/CW2），
块已擦写就绪 ⇒ 在既有回读循环之后，加一段**普通读**（`test_cmd_build` + 完整 plane 位图，**走 desc 链**），
读回后对 CW0/CW2 调 `fc_buffer_get_meta`，比对**同一个** expected：

```c
expected = (base & ~PLANE_CW_MASK) | ((uint32_t)plane << PBA_PLANE_SHIFT) | (uint32_t)cw;
meta_ptr = fc_buffer_get_meta(cmd, cw);          /* CW 基址，offset 0 = META */
got_meta = meta_ptr[0] | (meta_ptr[1] << 8) | (meta_ptr[2] << 16) | (meta_ptr[3] << 24);
munit_assert_uint32(got_meta, ==, expected);
```

- **互为交叉检查**：op 11 走「无链只读回读」，本段走「链内 buffer」——同一页、同一公式、两条独立路径。
- **为什么普通读后 buffer 内仍有 META**：读路径 **Pass 4** 会把 buffer[0..4] 读作 `read_meta`
  并做自洽 + LBA 比对（`src/fc/fc_data_path.c:618-619`）⇒ offset 0 一直是页内**明文** META
  （加扰区从 offset 8 起）。
- **附加契约断言**：`munit_assert_null(fc_buffer_get_meta(cmd, (uint8_t)CW_NUM))`——cw 不在链上
  按契约返回 NULL。

## 三、为什么不做 `fc_get_cmd_ecc_max`（用户已划掉，附核实结论）

用户划掉它是对的。核实链：`fc_get_cmd_ecc_max` 读的是 desc 的 `ecc_bits`，该值经
`fc_complete_from_nand` **Step 2**（`fc_data_path_process_read`，BCH 模式下写回真实纠正位数）
→ FTL `rr_record_ecc`（`ftl.c:106`）→ `track.health`。但：

- 单次高水位那条路**死的**——`RR_ECC_HIGH_WATER = 6`（`include/ftl/read_recovery.h:44`），
  而 BCH `t = 4`（`src/fc/fc_bch_engine.c:31`）最多纠 4 bit ⇒ `ecc_bits ≤ 4` **恒 < 6**，
  永远触发不了；
- 消费者链的两个出口 `rr_get_health` / `rr_check_predictive` **零调用者**（死 API）。

⇒ 它顶多算一条**半活的弱链**，不值得补测。（`src/ftl/**` 按用户口径不动，本版只做核实记录。）

## 四、验证

```bash
make format-check              # clang-format-15 → Format check passed.
make clean && make tests       # 0 warning 0 error；用例数不变（仍 57）、.bin 240436 B
```

上板（**用户跑**，唯一硬判据）：

```
test /fc-nand/lifecycle/init_sequence    # [ OK ] [ 328.22 us ]（--log-visible info 下打 UNIQUE_ID(16B)=…）
test /fc-nand/lifecycle/block_read_meta  # [ OK ] [ 40.16 ms ]（--log-visible info 下多两行 buffer_get_meta）
test                                     # 57 of 57 (100%)、0 skipped
```

## 五、风险 / 不做

- **纯测试**：`src/**` 一行不动，生产路径逐字不变。
- **不动** `src/ftl/**`、`tests/integration/**`、`tests/framework/**`、`ut_main.c`；**不翻** `CONFIG_FC_RAID`。
- ⚠ **op 3 首次上板**：`EDh` 的 UOP 与 `ECh`（参数页）同模板，器件是否接受需上板确认；
  若红，正是要暴露的结论，不绕过。
- `fc_buffer_get_meta` 的断言只覆盖「单 plane、连续位图」——**CW 号 ≠ 链走位**的分叉形状
  （非连续位图，如 CW{0,2}）本版**未**单独构造（gc 只用 `get_meta(cmd, 0)`，链位 0 即 CW0，
  两者不分叉）；如需覆盖该分叉，需另建非连续链，留待后续。

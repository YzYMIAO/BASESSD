# v59：新增 `OP_READ_ONFI_SIGNATURE`（槽位 9）—— 90h + 地址 20h 的 4 字节 ONFI 签名，写入 CQ 的 `param`

## 〇、起因：手册 Figure 27 那条 90h 没实现

用户对照 MT29F4G08 手册提问「这个支持了吗」，贴的是相邻两张图：

| 手册图 | 时序 | 现状 |
|---|---|---|
| **Figure 26** | `90h` + 地址 **00h** → 5 字节（厂商/器件 ID） | **已支持** —— `OP_READ_ID (1U)`，开机日志 `nand: ID=2C DC 90 95 56` |
| **Figure 27** | `90h` + 地址 **20h** → 4 字节 `4Fh 4Eh 46h 49h` = `"ONFI"` | **不支持** —— 全仓没有任何一处把 `0x20` 送进 90h 的地址段 |

用户拍板：「加吧，只返回4个参数，是不是可以考虑放在返回的命令状态参数里」——
**4 字节走 CQ 的 `fc_command_status_t.param.bytes[]`**，复用既有的控制器侧暂存
`g_fc_return_staging` 机制（`OP_GET_FEATURES` / `OP_READ_META` / `OP_BLOCK_LOCK_STATUS` 同款），
**不新增出参 API**。

范围（用户明确选择）：
1. **只做 FC 路径的一等 op**（同 `OP_GET_FEATURES`），**不加启动期自检**——启动走同步
   `nand_fc_execute`，其 completion 无 `param` 通道，与本设计冲突。
2. **补单测**：并入既有 `tests/unit/test_fc_level3.c` 的 `init_sequence`，**不新建测试文件**。

> ⚠ 与 `OP_READ_PARAMETER_PAGE`（`ECh`）的 "ONFI" **不是同一条路径**：EC 是独立命令、走
> desc 链、字节落 USER 缓冲；本 op 是 90h 命令、**无 desc 链**、字节经暂存落 CQ `param`。
> `init_sequence` 里两步并排，正好把这两条路径的差别摆在报告里。

## 一、设计定案

| 决策 | 结论 |
|---|---|
| OP 槽位 | **复用已删除槽位 9**（原 `OP_RANDOM_DATA_INPUT`，功能早已由 op 10 覆盖）⇒ **`OP_COUNT` 保持 42 不变**。槽位 20 是墓碑（「已删除…不要复用」），不动 |
| uop 序列 | **复用 `g_nand_read_id_uops`**（`WAIT_READY → 90h → COLUMN_0 → DATA_OUT → END`）——`NAND_UOP_COLUMN_0` 的 handler 写的是 **`request->column` 低字节**（`uop` 被显式忽略，`nand_fmc.c:149`），故同一数组同时服务 90h+00h 与 90h+20h |
| 地址字节 | **恒 `0x20`**，由 NAND 侧 `req_onfi_signature` 硬编码（与介质坐标、PAA、desc 链全无关） |
| 行地址 | 恒 0 —— 该 uop 序列无 ROW 段，填了也不下发（同 `req_feature`） |
| 数据长度 | 4 字节，落本 plane 的 CW0 那一格（掩码位 = `plane*CW_NUM`） |
| 改动范围 | src + Level 3 单测 + 文档/changelog |

## 二、`src/` 改动

| # | 文件 | 改动 | 理由 |
|---|---|---|---|
| A | `include/nand/onfi_1_0.h` | `OP_READ_ONFI_SIGNATURE (9U)` 定义（§1，紧跟 `OP_READ_ID`）+ 手册 Figure 27 时序注释；§2 的 op-9 墓碑注释改为「已复用」指引 | 复用槽位，`OP_COUNT` 不变 |
| B | `include/nand/onfi_1_0.h` | 命令宏区新增 `NAND_READ_ID_ADDR_ONFI_SIGNATURE (0x20U)` | 90h 的地址段取值（00h / 20h）在命令宏旁一处给死 |
| C | `src/nand/onfi_1_0.c` | op 表加 `[OP_READ_ONFI_SIGNATURE]`（`:827`） | 字段照 `[OP_READ_ID]`，但 `.name = "READ_ONFI_SIGNATURE"`、**`.has_data = 0U`**（同 `OP_READ_META` ⇒ `ftl_cmd_needs_desc_chain` / `fc_command_validate` 都不要求链）、`.data_length = 4U`、`.uops = g_nand_read_id_uops`。**不新增 uop 数组** |
| D | `src/nand/onfi_1_0.c` | `g_nand_read_id_uops` 的头注释补一句「地址字节来自 `request->column`」 | 说明该数组为何能被两个 op 共用（数组本体一字不动） |
| E | `src/nand/nand_dispatch_tables.c` | 新 handler `req_onfi_signature()`（`:344` 附近，照 `req_meta_read` 写法）+ 注册进 req 表（`:600`）与 completion 表（`:538`） | `.column = 0x20`、`.row = {0U}`、`.data = cw_buffers[plane*CW_NUM]`、`.data_length = 4U`；completion 用 `completion_single`（无 CW 概念） |
| F | `src/fc/fc.c` | `fc_build_dispatch_request` 新增 `OP_READ_ONFI_SIGNATURE` 分支（`:1417`，插在 `OP_READ_META` 与块锁族之间） | 位图只置本 plane 的 CW0；`cw_buffers[plane*CW_NUM]` 挂 `g_fc_return_staging[command_id]`；**不填 `plane_paa[]`**（无地址段需要）。**必须建这一支**——否则落进末尾 `else`，位图保持空集 ⇒ `nand_dispatch_task_load` 一格 `cw_buffers[]` 都不搬 ⇒ NAND 侧拿到残留指针，`DATA_OUT` 写向野地址 |
| G | `src/fc/fc.c` | `fc_complete_from_nand` 的 staging→`param.bytes` 拷贝条件加本 op（`:841-844`）；同步 `:789` / `:838` 注释 | 4 字节从控制器侧暂存拷进 CQ `param` |

### ⚠ 两张 `[OP_COUNT]` 表都必须填

`nand_dispatch.c:420` 是**裸函数指针调用**（`g_nand_dispatch_req_table[task->operation](task);`），
漏填 = NULL 调用 = HardFault。completion 表同理由 `:104` 直接分派。

### ⚠ 生产路径零影响（已核实）

- `fc_is_page_read()` 不含本 op ⇒ 走非 page-read 分支 ⇒ 不进 ECC / 数据路径。
- `fc_dispatch_rewrite_operation()` 只动 op 12/30/41 ⇒ 本 op 不被改写。
- 本 op **零生产调用者**（同 `OP_GET_FEATURES` 现状），只有 `init_sequence` 点名。
- `src/ftl/**` **一行不动**。

## 三、`tests/` 改动

**并入既有用例 `init_sequence`（`tests/unit/test_fc_level3.c`），新增 Step 4**——原 Step 4/5 顺延为
Step 5/6：

```c
fc_command_t *cmd = test_cmd_build(&(nand_addr_t){.block = 0U, .plane = 0U, .page = 0U}, 0U, 0U,
                                   &L3_CONF(OP_READ_ONFI_SIGNATURE, FC_CRC_ENABLE));
fc_command_status_t completion = test_submit_wait(cmd);
munit_assert_uint32(completion.status & FC_CW_STATUS_ERROR_MASK, ==, 0U);
munit_assert_uint8(completion.param.bytes[0], ==, 0x4FU); /* 'O' */
munit_assert_uint8(completion.param.bytes[1], ==, 0x4EU); /* 'N' */
munit_assert_uint8(completion.param.bytes[2], ==, 0x46U); /* 'F' */
munit_assert_uint8(completion.param.bytes[3], ==, 0x49U); /* 'I' */
```

- `cw_bitmap = 0`（无 desc 链）；**夹具不用改**——`test_cmd_try_build` 的
  `ASSERT((addr->cw == 0U) || (operation == OP_READ_META))` 走前半支。
- 证据摘要扩一条：`SIG(90h+20h)=4F 4E 46 49 (ONFI)`，与 `PARAM_PAGE=… (ONFI)` 并排。
- **用例数不变（仍 57）**，不新增注册行、不碰 `ut_main.c`。

## 四、验证

```bash
make format && make format-check    # ✅ 通过
make clean && make tests            # ✅ 0 warning / 0 error
```

链接 **242776 B**（v58 是 241584 B）。

**上板（用户跑，2026-10-11）**：完整 `test` ⇒ **57 of 57 (100%)**，总耗时 **870.83 ms**；
`init_sequence` `[ OK ]`。该用例的 Step 4 是**硬断言**（`param.bytes[0..3] == 4F 4E 46 49`），
它绿即证明器件真在 `90h + 地址 20h` 之后返回了 `"ONFI"`。**上板已验。**

## 五、风险（上板后回填）

1. **`0x20` 只在手册 Figure 27 一处出现** —— ✅ **已闭合**：器件实测返回 `"ONFI"`，
   断言未红。（若当初凭记忆断言，此案正是「协议断言必须先查证」的反例。）
2. **4 字节输出在 `90h` 命令之后**（不是 `00h`-`30h` 之后）—— ✅ **已闭合**：同上。
3. **`g_fc_return_staging` 落 `.bss` → AXI_RAM（Write-Back）**：SWD 读内存会看到陈旧值——
   但这是 `GET_FEATURES` / `READ_META` **既有**的同一块暂存，非本版引入。
4. `fc_param_t` 是 union，`bytes[]` 按 `uint8_t[4]` 逐字节索引（同 `OP_BLOCK_LOCK_STATUS`
   写 `bytes[0]` 的口径）⇒ 无端序歧义。

## 不做

- **不加启动期自检**（用户已选「只做 FC 路径的 op」）。
- **不新增出参 API**（4 字节走 CQ `param`）。
- 不改 `nand_fc_read_id`（`src/nand/nand_mt29f4g08abadawp.c:349`，同步 API，5 字节 00h 路径）。
- 不动 `src/ftl/**`、不动 h7r3、不翻 `CONFIG_FC_RAID`。

## 出处

- 手册：MT29F4G08 Figure 26 / Figure 27（`90h` 的 `00h` 与 `20h` 两个地址段）。
- 同型先例：`docs/archive/changelog/fc_ut_read_meta_v46.md`（无 desc 链 + 4 字节 + 经暂存进 CQ `param`）。

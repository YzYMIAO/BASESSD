# v23：FTL 建链收敛为「单一掩码旋钮」——`cw_mask == 0` = 本命令 plane 段内全部 CW

- **日期**：2026-10-08
- **范围**：`src/ftl/ftl_fc_interface.c`、`src/ftl/read_recovery.c`、`include/ftl/ftl_fc_interface.h`、`include/fc/fc_command.h`、`src/fc/fc.c`（仅注释）、`tests/unit/test_fc_level2.c`、`tests/unit/test_fc_level3.c`、文档
- **前置**：v13（策略层上移 FTL）、v14（同步原语收敛）、v18~v21（FC 层入口守卫收口）
- **不碰**：NAND 层（`request->cw_mask` 的 0 = 空集契约原样保留）

## 背景

起因是用户的一个提问：「`request->cw_mask`（NAND 层）和 `cmd->param.cw_mask`（FC 命令层）只是共用同一套 flat 编号，值并不相等」，以及随后的提议：「把 `cw_mask = 0` 不设置默认为都有数据如何」。

走查后确认**两层 0 方向相反是结构性的，不可能靠改 0 消除**：

| | `cmd->param.cw_mask`（SQ 输入） | `request->cw_mask`（NAND 请求） |
|---|---|---|
| 角色 | **意图**（"我想要哪些 CW"） | **已解析结果**（"实际访问哪些 CW"） |
| 0 的含义 | 未指定 → 展开 | **空集**，合法 |
| 读者 | FTL（唯一） | NAND 层（唯一） |

`request->cw_mask` 的 0 = 空集是**承重契约**：`cw_buffers[]` 由掩码自身决定填不填
（`nand_dispatch.c`），消费点对置位格一律 `ASSERT` 非 NULL 或直接解引用
（`nand_dispatch_tables.c` 五处、`nand_dispatch.c` 三处连检查都没有）。把 0 解释成全集
而不在 `task_load` 这个唯一入口同步展开 buffer，就是 NULL 解引用；收益仅"擦除少置一位"。
故**不采用**该提议。

**但走查暴露了一个真缺陷**：FTL 建链有**两个重叠的旋钮**，两个入口各认一个——

| 入口 | 调用者 | v22 前认哪个旋钮 |
|---|---|---|
| `ftl_cmd_setup_ppa_pair` | **host 读写**（`ftl.c`） | 只认 `config.cw`，**完全不读 `param.cw_mask`** |
| `ftl_cmd_configure_descriptors` | GC / 同步原语 / 测试 | 认 `param.cw_mask`，0 时退回 `config.cw` |

即 `param.cw_mask` 这条通道**主数据路径根本不看它**。今天没出事只因为生产路径从不设它
（`src/` 内该字段的语义写入者为 **0**），而 `config.cw` 在生产恒为 3
（`ftl.c:628/737`、`gc.c:44`、`sync_page_prepare`；`read_recovery.c` 的两处 0 是
`OP_SET_FEATURES`，`needs_desc_chain == 0` 短路）。

## 目标

**把 0 定义成「本命令 plane 段内的全部 CW」，让两个入口共用同一套展开规则，两个旋钮塌缩成一个（掩码）。**
与 v13/v14 一路"收敛冗余旋钮"同线。

**不是**目标：让两层 mask 的值相等（做不到，见上）。v23 消除的只是"0 在 FC 侧到底是不是通配符"这处歧义。

## 1. 唯一的展开规则：`ftl_cmd_resolve_cw_mask()`

`src/ftl/ftl_fc_interface.c` 新增 static 解析函数，是「0 = 全部」的**唯一定义处**：

```c
static uint32_t ftl_cmd_resolve_cw_mask(const fc_command_t *cmd, uint8_t plane_first, uint8_t plane_count)
{
    const uint32_t allowed = ((uint32_t)((1U << ((uint32_t)plane_count * CW_NUM)) - 1U)) << ((uint32_t)plane_first * CW_NUM);
    const uint32_t mask    = cmd->param.cw_mask;

    if (mask == 0U)
    {
        return allowed;                     /* 0 = 本命令 plane 段内的全部 CW */
    }
    ASSERT((mask & ~allowed) == 0U);        /* 越段 = 调用者编程错误 */
    return mask;
}
```

**越段检查为什么必须留在这里**：机制层 `fc_command_configure_descriptor` 的越段 `ASSERT`
只在**逐条调用时**生效；FTL 的枚举循环只走 `plane_first..plane_last` × `cw 0..CW_NUM-1`，
段外的高位 bit **根本不会被枚举到**——不检查就是**静默忽略**。这是唯一的检测点。

判据来源：`assertion_guidelines.md` **§十三**（建链入口的"配置错误 → ASSERT"），v18 只收了
FC 侧入口，FTL 侧这一处是同判据的漏网 → 本轮补上，并记录为 **§十八**。

## 2. 两个入口改读同一个旋钮

- **`ftl_cmd_configure_descriptors`**：删掉 `mask == 0` 的
  `per_plane = (1U << (config.cw + 1U)) - 1U` 逐 plane 平移段，以及
  `else if ((mask & ~allowed) != 0U) return FTL_ERROR;`（转 `ASSERT`）。
  返回码说明同步：越段不再返回 `FTL_ERROR`，只剩"池空"。
- **`ftl_cmd_setup_ppa_pair`**：从"忽略掩码"变成"读掩码"——plane 段改由
  `fc_command_plane_span` 定（不再 `multi_plane ? multi_plane : 1`），内层循环改为
  **按掩码位枚举**。回滚路径与返回码不变。
  **未加**"plane 落在段内"的断言：`PLANE_NUM = 2` 时该断言恒真，属 §十七 判据 5 要清的死代码。
- **`sync_page_prepare`**：删 `cw` 死参数及其 `cmd->config.cw = (DWORD)cw;`（唯一用途就是喂建链），
  四个调用点去实参。

## 3. 清掉惯性写法

`src/ftl/read_recovery.c` 两处 `cmd->config.cw = 0U;` 删除——两条都是 `OP_SET_FEATURES`，
`needs_desc_chain == 0` 不建链，该赋值对建链无意义。
（⚠ 注意该函数的 `param.parameter0..3` 与 `cw_mask` 是**联合体别名**，但 SET_FEATURES 从不建链，
故不会被当成掩码。）

## 4. 测试夹具同步（7 个用例，几何逐字不变）

判据：**受影响 = `param.cw_mask == 0` 且 `config.cw < 3`**（`config.cw == 3` 时新旧都是 4 个 CW）。

| 文件 | 改动 |
|---|---|
| `tests/unit/test_fc_level2.c` | `alloc_cmd()` 改为 `alloc_cmd_full(0U, cw_count, (cw_count >= CW_NUM) ? 0U : ((1U << cw_count) - 1U), …)`——整页仍传 0（走真实的生产形态），子集显式给掩码。影响 `test_encode_single_cw` / `test_process_read_single_cw`（仍 1 条 desc） |
| 同上 | `test_cw_mask_zero_multi_plane` **加强**：把 `config.cw` 由 4U 改成 **0U**，断言由 `== 3U` 改成 `== 0U`，链仍须 8 条——v23 前这条会建成 2 条（`per_plane = 1<<(0+1) - 1 = 1`），故它成为"0 的展开与 `config.cw` 无关"的**真回归钉** |
| `tests/unit/test_fc_level3.c` | 5 处各加一行 `cmd->param.cw_mask = 0x01U;`（READ_ID / READ_PARAMETER_PAGE / READ_STATUS / READ_STATUS_ENHANCED / RANDOM_DATA_READ）——这 5 条此前用 `config.cw = 0` 表达"1 条 desc"，新语义下会变成 4 条。同时删掉 `RANDOM_DATA_READ` 处已失效的 `/* 1 个 desc */` 注释 |

**无需改**（已核）：`test_cw_mask_flat_mixed` / `test_cw_mask_plane1_only`（显式掩码）；
所有 `config.cw == CW_NUM-1` 的用例（4→4、8→8）；
`test_fc_level1.c` / `ut_main.c`（不建链）；
`test_fc_level3.c` 里其余 `config.cw = 0U` 全部落在无链操作上
（RESET / GET_FEATURES / BLOCK_LOCK / BLOCK_UNLOCK / BLOCK_LOCK_TIGHT / ERASE 辅助函数）。

## 5. 注释与文档

- `include/fc/fc_command.h`：`config.cw` 注释改为"v23 起 FTL 建链不再读它"；`fc_param_t`
  的 SQ 侧 `0 = 未指定` → `0 = 本命令 plane 段内的全部 CW`，并新增一段**"别与 NAND 层
  `request->cw_mask` 混淆"**的警示（两层 0 方向相反）。
- `include/ftl/ftl_fc_interface.h`：文件头与两个入口的文档注释统一到新语义。
- `docs/design/fc_nand_design.md`：顶部版本列表新增 v23；§2.10 生命周期、§13.3.1 字段表、
  §25.2 决策 1 注、§25.4 展开规则代码块 + 新增 **v23 小节**、§25.8 风险 5 重写。
- `docs/design/assertion_guidelines.md`：新增 **§十八**。
- `src/fc/fc.c`：**顺带修正** `fc_build_dispatch_request` 末尾 else 分支的注释——它把
  `READ_STATUS` 列为"无数据命令"，但 `OP_READ_STATUS` 的 `has_data = 1`
  （`onfi_1_0.c:820`），实际走 `fc_build_request_data_planes`；也删掉误列的 `ERASE`
  （ERASE 在上面两个分支已各自置位）。因 `READ_STATUS` 从不由 FC 提交
  （引擎 WAIT_READY 内部直接用 70h/78h uop），行为无影响，只是注释不实。

## 验证

| 档 | 结果 | 二进制 |
|---|---|---|
| `make debug` | 0 warning / 0 error | 150300 B |
| `make release` | 0 warning / 0 error | 84800 B |
| `make -k tests` | 0 error / 0 warning | **197388 B**（基线 197344 B，+44 B） |
| RAID=1 tests（子 make + `-DCONFIG_FC_RAID=1`） | 0 error / 0 warning | **204372 B**（基线 204328 B） |
| `make format-check` | passed | — |

- **`nm` 抽查**：`ftl_cmd_resolve_cw_mask` 是 **local（`t`）**——未引入新的外部链接符号；
  RAID 档 `fc_raid_recover` / `fc_data_path_raid_recover` 在。
- **大小不作硬判据**：展开逻辑本身变了，且测试夹具改动也会改变 tests 档二进制。只记录前后大小。
- **真正的行为判据是推理 + 板测**：本工程测试只在板上跑，本地只能验证"编译干净"与
  "链几何不变"的推理。链几何不变的依据是：生产 `config.cw` 恒为 3 → `0x0F` / `0xFF`，
  与新语义"段内全部 CW"**逐位相同**；`ftl_cmd_setup_ppa_pair` 的唯一生产调用者
  （`ftl.c`）不设 `param.cw_mask`（恒 0）；7 个受影响用例已按 §4 显式化，几何逐字不变。
- **上板未验**（项目硬规则：`make tests` 上板由用户执行）。

## 未跟进

1. **`request->cw_mask` 的 0 = 空集不动**——两层 0 方向相反是结构性的，只能靠文档说清。
2. **RAID stripe 仍读 `config.cw`**（`fc.c` 的 `fc_stripe_submit` / parity 建链、
   `fc_data_path.c` 的 `cw_count`，默认不编译）。要让 `config.cw` 彻底退出生产读者，
   需连 RAID 一起改成"全部 CW"。
3. **`config.cw` 的写入者变成死写**：`gc.c:44`（`3U`）在默认构建下已无读者
   （GC 走 `setup_ppa` → `configure_descriptors`）。保留（RAID 档可能仍读），不删。
4. **`ftl_cmd_setup_ppa_pair` 零测试覆盖**——本轮恰恰把它的语义从"忽略掩码"改成"读掩码"，
   这是整次改动里唯一**没有回归保护**的点（`tests/` 内无任何 `setup_ppa_pair` 调用）。
   建议补一条 `test_setup_ppa_pair_mask`（`multi_plane=2`、`mask=0x05`、两个 PBA 行地址不同
   → 断言链为 2 条且两条 desc 的 `paa` 分别落在各自 plane）。按项目规则测试源文件归用户，
   **待用户点头再做**。
5. **`docs/design/fc_nand_design.md` §11.3 / §12.4.2 是更早版本的历史段落**，仍在教
   `fc_command_configure_descriptors()`（**复数**，v13 已删）与 `while (… cw <= cmd->config.cw)`
   的建链写法。本轮不动（已在 v13 起就失效），留给后续文档清理。
6. **`tests/integration/test_fc_nand.c` 已失效**（5 处调用 v13 删除的复数符号，且不在
   `TEST_C_SRCS` 里）——用户维护的上板测试，不动。

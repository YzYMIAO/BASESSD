# v28：CW 集合退出命令结构体——SQ 请求集改作建链形参，CQ 失败集删除

- **日期**：2026-10-09
- **范围**：`include/fc/{fc_command.h,fc_interface.h}`、`include/ftl/ftl_fc_interface.h`、
  `include/nand/nand_dispatch.h`、`src/fc/{fc.c,fc_command.c,fc_interface.c}`、
  `src/ftl/ftl_fc_interface.c`、`src/nand/{nand_dispatch.c,nand_dispatch_tables.c}`、
  `tests/unit/{fc_test_common.c,fc_test_common.h}`、文档
- **前置**：v27（`cw_mask` → `cw_bitmap`，取消「0 = 段内全部」重载）
- **介质格式**：**无变更**——位图只影响固件侧建链与完成项组装，不下沉到 NAND 页格式。
- **上板未验**：本项目硬规则。
- **测试**：本轮**未编译**（用户要求「重构测试代码期间不编译」）。`make format-check` 通过。
  构建内的 Level 2/3 仍在迁移到公共夹具（`test_fc_common`），其调用点本轮未改完，`make tests` 必然失败；
  `make debug` / `make release` 不编 `tests/`，不受影响。

## 背景

用户提问：「能不能把 `cw_bitmap` 干掉，底层的能不能一起干掉？」给出的三条理由：

1. 擦除命令不需要它；
2. 写命令是页粒度的（flash 特性），由上层保证；
3. 读命令通过遍历 desc 就能拿到对应信息——底层拿到一个 desc 就发一条 `06h-E0h`（多 plane）/
   `05h-E0h`（单 plane）；**CQ 也不用去组合哪些 CW 错误，由上层遍历链 OR desc 状态**。

随后用户澄清模型：「底层是通过读命令所带 desc 所对应的位置发 `06h-E0h`/`05h-E0h` 读数据的；
**上层的 bitmap 最多是给 desc 配置函数说明哪个 CW 需要申请 desc**」。

**走查结论**：NAND 层的 `request->cw_bitmap` **保留**——它是 `cw_buffers[]` 这个稀疏数组的
成员判据（task slot 复用且不清零，掩码外是残留指针），散掉它需要每个消费点自己判空，收益为负。
但 **FC 命令层与 CQ 层的那两个 `cw_bitmap` 视图确实该删**（见下）。

**范围决定（用户拍板）**：**CQ `param` 字段保留**——`tests/integration/test_fc_nand.c` 的
GET_FEATURES 回读要读 `completion.param.parameter0..3`（起初我 grep 只扫了 `src/`+`include/`，
漏了 `tests/`，误报"无读者"，已更正）。故只删 `param` 里的 `cw_bitmap` **视图**，不删 `param`。

## 病根：住在结构体里的派生量

`cmd->param.cw_bitmap`（v27 形态）在 FC 内**既无写入者也无读者**——唯一读者
`ftl_cmd_resolve_cw_bitmap()` 就在建链那一刻把它取出来用掉，之后再无回读。它是**建链的输入**，
不是命令的属性。住在共享结构体里的代价：

1. 多一处可被误读的"真值"（`fc_command_validate` 该不该校验它？RAID 断言读字段还是读链？——
   这两个问题都因此产生过）；
2. 命令结构体对"访问哪些 CW"撒了两个谎：字段值（意图）与 desc 链（事实）可以不一致，
   而事实由链决定。

CQ 侧的失败位图同理：它是「desc 链上 `status & (UNC|CRC_ERROR)` 那些 desc 的 `n`」的**纯派生量**，
上层遍历链 OR 一次就有（链保留到 `fc_command_release()`）。FC 代为聚合只省了上层一个循环，
却多维护一份可能与链不一致的快照。

## 改动

### A. SQ 请求集 → 建链形参

```c
/* include/ftl/ftl_fc_interface.h */
ftl_status_t ftl_cmd_configure_descriptors(fc_command_t *cmd, uint32_t cw_bitmap);
```

`ftl_cmd_resolve_cw_bitmap()` 的首参由 `const fc_command_t *cmd` 改为 `uint32_t bitmap`——
它现在校验的是**调用者给的值**，与命令结构体无关。两条 ASSERT 语义不变（§二十一）。

三个「整段访问」入口改为**算出位图当实参传**（不再是"填字段再调"）：

| 入口 | v27 | v28 |
|---|---|---|
| `addr_to_cmd`（static inline） | `cmd->param.cw_bitmap = fc_command_plane_span_cw_bitmap(cmd);` 再调建链 | `ftl_cmd_configure_descriptors(cmd, fc_command_plane_span_cw_bitmap(cmd))` |
| `ftl_cmd_setup_ppa` | 同上 | 同上 |
| `ftl_cmd_setup_ppa_pair` | 同上 | 位图作 `ftl_cmd_resolve_cw_bitmap()` 的实参 |

`fc_command_plane_span_cw_bitmap(cmd)` 本身**不动**（仍是"整段访问"的唯一写法），
只是产出物从"字段值"变成"实参"。

### B. CQ 失败集 → 删除聚合

`src/fc/fc.c` 的 `fc_complete_from_nand` 第 3 步：

- 删除按 desc 链 OR 失败位图的那段（原 `completion.param.cw_bitmap |= 1U << (...)`）；
- 联合体清零由 `completion.param.cw_bitmap = 0U;` 改为 `completion.param = (fc_param_t){0};`
  （`cw_bitmap` 视图已删，不能再借它清零）；
- **保留** GET_FEATURES 的 `completion.param.bytes[i] = g_fc_return_staging[...][i]`；
- `completion.desc_id` = 首个失败 CW 的 desc id **照旧**（失败判据仍是 `FC_CW_STATUS_ERROR_MASK` 全集）。

### C. 擦除的「假 CW 位」→ 移到 NAND 侧

v7~v27，FC 的 ERASE 分支给请求位图硬置一个 CW0 位，唯一目的是让 `completion_all_cws` 的
`cw_next(bitmap, 0)` 循环至少跑一次、把 `cw_status[]` 写上——否则 FC 的非 page read 聚合分支
（全表 OR）读不到擦除成败。这是**为 completion 服务的假数据**。

v28 删除该位（单 plane 与 multi-plane 擦除分支都删，只留 `plane_paa[]` 的填充），
`src/nand/nand_dispatch_tables.c` 的 `completion_all_cws` 补一条空集分支：

```c
if (task->cw_bitmap == 0U)
{
    task->completion.cw_status[0] = status;   /* 与 completion_single 同口径 */
    ...
    return;
}
```

⇒ 擦除请求位图自此是**合法空集**，位图只表达"访问哪些 CW"这一个意思。

### D. 结构体与文档

| 位置 | 改动 |
|---|---|
| `fc_param_t` | 删 `uint32_t cw_bitmap` 成员（只剩 `parameter0..3` / `bytes[4]`，即 FEATURES 内联数据） |
| `fc_command_t.param` 注释 | 改为"仅 FEATURES 命令使用" |
| `fc_command_status_t.param` | **保留字段**（GET_FEATURES 回读）；注释去掉失败位图 |
| `fc_command_status_t` 头注 | "失败 CW 定位应走 `param.cw_bitmap`" → "遍历 desc 链 OR `desc->status`" |
| `fc_command_plane_span_cw_bitmap` 头注 | "填 `cmd->param.cw_bitmap`" → "产出建链实参" |
| `nand_dispatch.c` 的 `task_load` 头注 | 更正"其余格保持零值"（slot 复用不清零，掩码外是残留值） |

### E. 测试夹具

`tests/unit/fc_test_common.c` 的 `test_prog_page` / `test_read_page`：
`cmd->param.cw_bitmap = test_plane_cw_bitmap(plane_bitmap); ftl_cmd_configure_descriptors(cmd);`
→ `ftl_cmd_configure_descriptors(cmd, test_plane_cw_bitmap(plane_bitmap));`。
`test_erase_block` 的注释同步（不再有"不填位图"这回事——位图压根不在命令里）。

顺手加固（不影响接口）：

- 两条 `_Static_assert` 钉死「`nand_paa_t` 正好 4 字节 + USER 区是 4 的整数倍」——
  `test_fill_paa` / `test_check_paa` 按 dword 步进填槽，任一漂移都会静默写穿 512B 槽。
- 链遍历**一律带 `CW_TOTAL_MAX` 上界**：链成环时哨兵永不出现，上界把挂死换成安静截断。
  `test_collect_chain` 的终止条件从 v17 前的 `desc_id < FC_DESCRIPTOR_POOL_DEPTH`
  改为哨兵（与全仓其余走链统一）。
- `test_verify_erased` / `test_verify_cw_status_all_success` 收 const；
  `test_memset_read_buffer` 对 buffer 改为断言非 NULL（与 `test_fill_user_paa` 同口径）。

## 影响面核查

- **生产行为中性**：三个整段入口算出的位图与 v27 **逐位相同**（只是从"填字段"改"传实参"）；
  唯一行为变化是**擦除**——请求位图由 `1U<<plane*CW_NUM` 变为 `0`，而 completion 侧
  同步补了空集分支写 `cw_status[0]`，FC 聚合读到的擦除成败**不变**。
- **`request->cw_bitmap == 0` 的消费面扩大**：此前只有 RESET/STATUS/块锁等，现在加上擦除。
  `nand_dispatch_task_load` 对空掩码一格都不搬 `cw_buffers[]`（slot 复用 ⇒ 残留旧指针），
  而 `req_erase` / `req_multi_plane_erase` **只用 `plane_paa[]` 不读 `cw_buffers[]`** ⇒ 安全。
  **这条契约现在依赖消费点真的不读**，已写进 `task_load` 注释（新增无数据 op 时须复核）。
- **RAID 不受影响**：`fc_get_cmd_cw_bitmap()` 早在 v13 就从 desc 链重建位图，从不读
  `command->param.cw_bitmap`；`fc_stripe_submit` 自己构造 `mp_req.cw_bitmap`（NAND 请求位图，保留）。
- **release 风险不变**：两条 ASSERT 在 NDEBUG 下被裁掉 ⇒ 忘给位图 = 静默空操作（v27 已知代价，
  本轮不新增）。

## 验证状态

| 项 | 结果 |
|---|---|
| `clang-format-15 -i` | 已跑（`fc_test_common.c`） |
| `make format-check` | **passed（101 文件）** |
| `make tests` / `make debug` / `make release` | **未跑**（用户要求重构期不编译） |
| 上板 | **未验** |

## 未跟进

1. **`request->cw_bitmap`（NAND 层）保留**——它是 `cw_buffers[]` 稀疏数组的成员判据，
   散掉它需要每个消费点自判空，收益为负（用户已确认"底层不动"）。
2. **`tests/unit/test_fc_level2.c` / `test_fc_level3.c`** 的调用点迁移：
   - **Level 2 已迁完**（同一工作区内）：整体退化为**纯 buffer 级**——建链后不提交，
     自行调 `fc_data_path_*`；`test_cmd_build()` 是新的公共建链原语。
   - **Level 3 待迁**：`test_fc_level3.c:280` 的 `CMD_FIELD_U(param.cw_bitmap)`
     是一行结构体字段计数宏（在 `#if 0` 块里），迁移时须删除（字段已不存在）。
3. **失败 CW 的定位读者尚未接线**：FTL 侧（`read_recovery.c`）目前不读 CQ 失败位图，
   v28 后要定位失败 CW 须遍历 desc 链——与 v27 的"不在本次范围"同一状态。

# v27：`cw_mask` → `cw_bitmap`——取消「0 = 段内全部」重载，两层统一为字面位图

- **日期**：2026-10-09
- **范围**：`include/fc/{fc_command.h,fc_interface.h}`、`include/ftl/ftl_fc_interface.h`、
  `include/nand/nand_dispatch.h`、`src/fc/{fc.c,fc_command.c,fc_data_path.c,fc_interface.c}`、
  `src/ftl/ftl_fc_interface.c`、`src/nand/{nand_dispatch.c,nand_dispatch_internal.h,nand_dispatch_tables.c}`、
  `tests/unit/{fc_test_common.c,fc_test_common.h,test_fc_level1.c,test_fc_level2.c,test_fc_level3.c}`、文档
- **前置**：v23（FTL 建链收敛为「单一掩码旋钮」）、v24（地址布局统一）、v25（地址构造收敛）
- **介质格式**：**无变更**——位图只影响固件侧建链，不下沉到 NAND 页格式。
- **上板未验**：本项目硬规则。
- **测试**：本轮**未编译**（用户要求「重构测试代码期间不编译」）。`make format-check` 通过（101 文件）。
  构建内的 Level 2/3 正在迁移到新的公共夹具，其调用点本轮未改完，`make tests` 必然失败；
  `make debug` / `make release` 不编 `tests/`，不受影响。

## 背景

用户提问：「对应 `cmd->param->cw_mask`，我想把它改成 `cw_bitmap`。哪个位置置为 1 则哪个位置就需要
读写数据。如何？」随后明确两件事：

1. **范围**：FC 侧 `fc_command_t.param` 与 NAND 侧 `request->cw_mask` **一起改**。
2. **语义**：「两层都是字面位图，**0 = 一个都不访问**」。

这条语义决定与 v23 直接冲突——v23 刚把 SQ 侧的 `0` 重定义为「本命令 plane 段内的全部 CW」。
本轮把这条重载**取消**。

## 病根：一个字段两种读法

v7~v26 期间 `param.cw_mask` 同时承载两种读法：

| 取值 | v7~v22 | v23~v26 |
|---|---|---|
| 非 0 | 字面集合 | 字面集合 |
| **0** | 「每个 plane 取连续 `0..config.cw`」（**借 `config.cw` 的语义**） | 「本命令 plane 段内的全部 CW」（**借 plane 段的语义**） |

重载带来两个代价：

1. **两层 0 方向相反**。SQ 侧 `param.cw_mask`（0 = 全部）与 NAND 侧 `request->cw_mask`
   （0 = **合法空集**，`cw_buffers[]` 由位图自身决定填不填）**方向相反**。这不是笔误，
   是重载的直接产物——只能靠文档解释，任何"顺手统一一下"的改动都会静默改变语义。
   v23 的 changelog 已把这条记为「结构性的，不在本次消除范围」。
2. **名字与语义不符**。字段叫 `mask` 时"0 = 全部"勉强说得通；但它逐位表达的是**集合**，
   与 NAND 层的同名字段口径不一致。

**核查结论**：`command->param.cw_mask` 在 `src/` 里**从无生产写入者**——FTL 只设
`config.cw = 3U`（v23 起建链已不读它）。也就是说生产路径一直是隐式「整段访问」，
而这条隐式语义是"碰巧"生效的。

## 改动

### A. 字段改名（两层同改）

| 旧名 | 新名 | 位置 |
|---|---|---|
| `fc_param_t.cw_mask` | `cw_bitmap` | `include/fc/fc_command.h`（SQ 请求集与 CQ 失败集共用同一联合体） |
| `nand_dispatch_request_t.cw_mask` | `cw_bitmap` | `include/nand/nand_dispatch.h` |
| `nand_dispatch_task_t.cw_mask` | `cw_bitmap` | `src/nand/nand_dispatch_internal.h` |

### B. 语义：字面位图，`0` = 空集

`ftl_cmd_resolve_cw_mask()` → **`ftl_cmd_resolve_cw_bitmap()`**（`src/ftl/ftl_fc_interface.c`，static）：

```c
static uint32_t ftl_cmd_resolve_cw_bitmap(const fc_command_t *cmd, uint8_t plane_first, uint8_t plane_count)
{
    const uint32_t allowed = ((uint32_t)((1U << ((uint32_t)plane_count * CW_NUM)) - 1U)) << ((uint32_t)plane_first * CW_NUM);
    const uint32_t bitmap  = cmd->param.cw_bitmap;

    ASSERT(bitmap != 0U);              /* 字面位图：0 = 空集 = 生产者忘了填（v27 起无隐式展开） */
    ASSERT((bitmap & ~allowed) == 0U); /* 越出本命令 plane 段 = 编程错误 */
    return bitmap;
}
```

`0` 的展开分支整段删除。判据与失效面见 `docs/design/assertion_guidelines.md` **§二十一**。

### C. 新增 `fc_command_plane_span_cw_bitmap()`——「整段访问」的显式写法

`include/fc/fc_command.h` 的 static inline，紧邻既有的 `fc_command_plane_span()`：

```c
static inline uint32_t fc_command_plane_span_cw_bitmap(const fc_command_t *command)
{
    uint8_t plane_first;
    uint8_t plane_last;

    (void)fc_command_plane_span(command, &plane_first, &plane_last);
    return (uint32_t)((1U << ((uint32_t)(plane_last - plane_first) * CW_NUM)) - 1U) << ((uint32_t)plane_first * CW_NUM);
}
```

段内每个 CW 一位置 1（plane1 单 plane ⇒ `0xF0`，双 plane ⇒ `0xFF`）。
⚠ 读的是 `cmd->pba` 与 `config.multi_plane`，**必须在两者都写好之后**调用。

### D. 三个「整段访问」入口各自填位图

| 入口 | 调用者 | v27 起 |
|---|---|---|
| `addr_to_cmd`（`ftl_fc_interface.h`，static inline） | `sync_page_prepare` → 四个 `ftl_sync_*` 页原语 | 填 `plane_span_cw_bitmap` |
| `ftl_cmd_setup_ppa` | `gc.c` | 填 `plane_span_cw_bitmap`（在 `cmd->pba` 写好之后） |
| `ftl_cmd_setup_ppa_pair` | **host 读写**（`ftl.c`） | 填 `plane_span_cw_bitmap`；**调用者先前设的位图会被丢弃** |

`ftl_cmd_configure_descriptors()` 是**唯一**接受调用者给的部分位图的入口。
（ERASE / RESET / SET_FEATURES / GET_FEATURES 走 `ftl_cmd_needs_desc_chain()` 短路，
在 `ftl_cmd_resolve_cw_bitmap` 之前返回，故新断言对它们不生效。）

### E. 连带改名

| 旧名 | 新名 |
|---|---|
| `fc_get_cmd_cw_mask()` | `fc_get_cmd_cw_bitmap()` |
| `fc_cw_mask_is_plane_symmetric()` | `fc_cw_bitmap_is_plane_symmetric()` |
| `nand_dispatch_cw_next(uint32_t cw_mask, …)` / `_cw_first(uint32_t cw_mask)` | 形参改 `cw_bitmap` |
| 夹具 `test_plane_cw_mask(plane_bitmap)` | `test_plane_cw_bitmap(plane_bitmap)` |
| 用例 `test_cw_mask_zero_multi_plane` | `test_cw_bitmap_span_multi_plane`（体由 `0U` 改 `0xFFU`） |

**刻意不动**：`include/nand/nand_config.h:216-217` 的 `const uint32_t cw_mask = NAND_FIELD_MASK(CW_WIDTH);`
——它是 **CW 字段的位掩码**（`cw` 字段占 2 位的位宽掩码），与"CW 集合位图"是两回事，同名不同物。

## 影响面核查

- **三个 0 口径统一**：SQ 请求集（原 0 = 全部）、CQ 失败集（0 = 无失败）、NAND 请求集
  （0 = 合法空集）——现在**三处同口径**（0 = 空集），v23 那条「两层 0 方向相反是结构性的」作废。
- **行为中性**：三个整段入口填出的位图与 v23~v26 的隐式展开**逐位相同**
  （单 plane 生产路径 `config.cw = 3` ⇒ 该 plane 段全 4 CW = `0x0F`/`0xF0`；双 plane ⇒ `0xFF`）。
- **NAND 层契约不变**：`request->cw_bitmap == 0` 仍是**合法空集**（无 desc 的退化命令），
  `cw_buffers[]` 由位图自身决定填不填——只是现在与 FC 侧同向。
- **release 风险（已记档）**：`ASSERT(bitmap != 0U)` 在 NDEBUG 下被裁掉 ⇒ 忘填位图 = 静默空操作。
  这是取消重载的已知代价，备选方案（`config.reserved` 加 flag / 造空集哨兵）均被判为"引入第二个旋钮"。

## 验证状态

| 项 | 结果 |
|---|---|
| `clang-format-15 -i`（17 个 .c/.h） | 已跑 |
| `make format-check` | **passed（101 文件）** |
| `make tests` / `make debug` / `make release` | **未跑**（用户要求重构期不编译） |
| 上板 | **未验** |

## 未跟进

- `tests/unit/test_fc_level2.c` / `test_fc_level3.c` 的调用点迁移（迁到 5 个页 helper）留到下一轮；
  本轮它们引用已删除的 `test_page_cmd_ex`，**处于不可编译状态**，属「先定型接口、再迁移调用点」的中间态。
- `tests/integration/*`、`tests/unit/test_rebuild.c`（构建外）未动。

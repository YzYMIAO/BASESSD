# v13.1：建链收敛——plane 段规则合并 + 删冗余断言

**日期**: 2026-10-07
**相关设计**: `docs/design/fc_nand_design.md` §25.4（建链，新增「v13.1」小节）、`docs/design/assertion_guidelines.md` §十一附
**前置**: `fc_policy_layer_uplift.md`（v13 策略层上移 FTL，本文件收拾它留下的两处冗余）

---

## 概述

v13 把 FC 的策略层上移 FTL 后，留下了两处可收敛的冗余。本文件记录收敛过程——**纯重构，行为中性**：
产出给 NAND 的 `nand_dispatch_request_t`（`cw_mask` / `plane_paa[]` / `cw_buffers[]`）逐字节不变。

1. **plane 段规则合并为一份定义**（v13 的已知风险 #1）。
2. **同源查重断言二选一**（v13 未察觉——两层各查一次同一不变量）。

---

## 一、plane 段规则合并

### 问题

v13 后 `fc_command_plane_span` 有两份同语义实现：

| 位置 | 形态 | 使用者 |
|---|---|---|
| `src/fc/fc.c` | `static` | `fc_command_attach_cw_internal` 的越段校验 |
| `include/ftl/ftl_fc_interface.h` | `static inline`（`ftl_cmd_plane_span`） | `ftl_cmd_configure_descriptors` 的掩码展开 |

改 plane 语义（`PLANE_NUM` / BA6 含义 / 单 plane 起点）必须同时改两处——已在两处注释里互相标注，
但注释挡不住漏改。

### 解法

`fc_command_plane_span` 上移 `include/fc/fc_command.h` 作 `static inline`，两边共用一份。

**为什么放这里合法**：它是**纯函数**——只读 `command->config.multi_plane` 和 `command->paa`，
不碰 `fc_t`、不碰三池。因此不受"FTL 看不到 `fc_t`"这条边界的约束（`fc_internal.h` 是 FC 私有头）。
它是 `fc_command_t` 的**结构属性**，本来就该跟结构体定义放一起。

`include` 链已通、无需补：`fc_command.h` → `fc_types.h` → `nand/nand_config.h`
（`PLANE_NUM` / `paa_to_plane` / `CW_NUM` 均可见）。新增 `#include "common/assertion.h"`
（头文件版保留了 `ASSERT(plane_count <= PLANE_NUM)` 守卫）。

### ⚠ FC 侧的越段校验保留，不是重复实现

`fc_command_attach_cw_internal` 里

```c
if ((cw >= CW_NUM) || (plane < plane_first) || (plane >= plane_last)) {
    return FC_STATUS_INVALID_ARGUMENT;
}
```

**必须保留**：它挡的是"desc 落到 `plane_paa[]` 覆盖范围之外"这个**静默腐坏**——
`fc_build_request_data_planes` 只为命令 plane 集合内的 `plane_count` 个 plane 填 `plane_paa[]`，
一个越段 desc 会让 NAND 侧看到 `cw_mask` 里一个不在 `plane_count` 范围内的位。
合并的只是**规则的定义**，不是这道校验。

---

## 二、同源查重断言二选一

### 问题

"没有两个 desc 落在同一个 `n`"这个不变量，v13 后有两处断言：

| 位置 | 机制 | 覆盖 |
|---|---|---|
| `fc_command_attach_cw_internal`（`fc.c`） | `ASSERT(fc_command_cw_attached(controller, command, plane, cw) == 0U)` —— O(N) 走链查重 | **全部** attach（单/多 plane） |
| `fc_build_request_data_planes` 单 plane 分支（`fc.c`） | `ASSERT(fc_cw_mask_popcount(request->cw_mask) == fc_command_descriptor_count(controller, command))` | **仅单 plane** |

两条查的是同一个不变量，而且第二条覆盖面反而是残缺的那一半。

### 解法

保留 attach 侧那条，删掉 `fc_build_request_data_planes` 里的 popcount 断言。

**判据（`assertion_guidelines.md` §十附判据 1：同源 → 删）**：`request->cw_mask` 正是从同一条
desc 链按 `plane*CW_NUM + cw` OR 出来的——与它要防的对象**同源**，等于同一条表达式算两遍。
而 attach 侧的断言在**污染点**就炸，定位更准，且覆盖完整。

连带删除 `fc_cw_mask_popcount`（唯一调用者就是那条断言）。`fc_command_descriptor_count` **不受影响**
——`fc_command_release` 等仍在用（`fc.c` 的 `remaining` 计数）。

---

## 三、改动清单

| 文件 | 改动 |
|---|---|
| `include/fc/fc_command.h` | 新增 `#include "common/assertion.h"`；新增 `static inline fc_command_plane_span()`（唯一一份定义，含 `PLANE_NUM` 守卫） |
| `src/fc/fc.c` | 删 `static fc_command_plane_span`（含注释块）；删 `fc_cw_mask_popcount`（含注释块）；删 `fc_build_request_data_planes` 单 plane 分支的 popcount 断言（注释同步）；两处注释里对 `fc_cw_mask_popcount` 的引用改掉 |
| `include/ftl/ftl_fc_interface.h` | 删 `static inline ftl_cmd_plane_span`；小节注释改指向 `fc_command.h` |
| `src/ftl/ftl_fc_interface.c` | 调用点 `ftl_cmd_plane_span` → `fc_command_plane_span`（2 处，含 1 处注释） |
| `src/nand/nand_dispatch.c` | 注释同步：原引用 `ASSERT(popcount(cw_mask) == fc_command_descriptor_count(...))`，改指 `fc_command_attach_cw_internal` 的越段校验 |
| `docs/design/fc_nand_design.md` | §25.4 表格行 + 新增「v13.1」小节；§25.4 历史段落里 `fc_cw_mask_popcount` 加删除注 |
| `docs/design/assertion_guidelines.md` | §十一 表格里 `fc_cw_mask_popcount` 引用改掉；新增 §十一附（本次删除的判据记录） |
| `docs/changelog/fc_policy_layer_uplift.md` | 已知风险 #1 标注 v13.1 已解决 |

---

## 四、行为中性核对

- 段 1：`fc_command_plane_span` 逐字节同语义（唯一差异是头文件版带 `ASSERT(plane_count <= PLANE_NUM)`，
  原 `fc.c` 版也有同一条，只是位置不同）。
- 段 2：删的是 **ASSERT**（`NDEBUG` 下本就编译为空），release 行为天然不变；debug 下少一次
  O(N²) 的链查重（N ≤ `CW_TOTAL_MAX` = 8，性能无关紧要）。

---

## 五、验证

| 项 | 结果 |
|---|---|
| `make format` | 通过 |
| `make debug` | 零警告零错误，二进制 153344 → **153316** 字节 |
| `make release` | 零警告零错误，86616 字节 |
| `make format-check` | 通过 |
| `make debug CFLAGS_EXTRA=-DCONFIG_FC_RAID=1` | 零警告零错误，157984 字节（RAID 分支确认被编译） |
| 代码残留引用 | `ftl_cmd_plane_span` / `fc_cw_mask_popcount` 全仓 `src/`+`include/` 已无 |

**上板验证未做**（依赖用户）：常规回归 `info → wr 0 2 → rd 0 2 c → tr 0 → fl → info`。

---

## 六、未做 / 遗留

- `fc_command_configure_descriptor_internal` 与 `fc_command_chain_append` 的拆分**保留**——
  全仓只有一处调用且总是连着调，但拆开把两种失败语义分清楚了（前者可能 `RESOURCE_EMPTY` 需回滚，
  后者 `void` 不可能失败）。合并收益小、可读性下降，本次不动。
- `docs/design/fc_nand_design.md` §25.9 文件变更表**未补 v13 / v13.1 行**（v13 当时也没补）——留作后续。
- 测试迁移仍归用户（v13 遗留）：`fc_command_configure_descriptors` 在测试里 24 处，
  改为循环调 `fc_command_attach_cw`。见 `fc_policy_layer_uplift.md`（平台规则：测试源文件由用户自建/迁移）。

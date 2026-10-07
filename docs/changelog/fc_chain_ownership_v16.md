# v16：desc 链所有权收敛——出池即挂链 + 建链/搬数据分离 + 释放漏 buffer 修复

**日期**: 2026-10-07
**相关设计**: `docs/design/fc_nand_design.md` §2.1（池布局）、§2.9（接口表）、§2.10（生命周期）、§26.3
**相关文档**: `docs/design/fc_api_usage.md`
**前置**: `fc_command_identity_convergence.md`（v15 命令身份去缓存化）、`fc_policy_layer_uplift.md`（v13 策略层上移 FTL）

---

## 概述

三件事，一件是**真 bug 修复**，两件是**结构收敛**：

| # | 项 | 改动前 | 改动后 |
|---|---|---|---|
| 1 | **buffer 泄漏**（真 bug） | `fc_command_release()` 只释放 desc，**从不还 buffer** | `fc_descriptor_chain_release_all_internal()` 沿链 desc+buffer 一起还池 |
| 2 | **半成品 desc 窗口** | desc 出池后先填字段、**再挂链**，中间失败要靠就地回滚（v15 的 patch） | **出池 → 填字段 → 立即挂链**，窗口不存在 → 回滚逻辑整体删除 |
| 3 | **建链与搬数据耦合** | 建链函数里顺手 memcpy 数据 | 建链**只建链**；搬数据是上层（FTL）在链建好之后、**仅对写操作**做 |

外加用户点名的**释放原语拆两个**：`fc_descriptor_chain_release_head()`（放一个）+
`fc_descriptor_chain_release_all()`（放全部），都传**链头 id**。

---

## 1. buffer 泄漏（真 bug，自 v7 起潜伏）

**症状**：`fc_command_release()` 沿链释放 desc 并还池，但**从不调用 `fc_buffer_release_internal()`**。
buffer 池深 128，一条 2-page multi-plane 写命令占 8 个 buffer → **16 条命令就耗尽**，
之后所有 `fc_buffer_alloc_internal()` 恒返回 `FC_BUFFER_ID_INVALID`，建链全线失败。

**核查过程**（穷举 grep + git 追溯）：

- 全仓 `fc_buffer_release_internal` 只有**两个**调用者：v15 的就地回滚（`fc.c:580`）
  和测试用的公共薄壳。**主释放路径上没有。**
- 逐版本追溯（v7 → v13.1）：`fc_command_release_internal_api` 从来没有 buffer 释放。
- **v15 让 RAID 路径也中招**：v15 把 `fc_stripe_release_parity` 的「显式 desc+buffer 循环」
  替换成委托给 `fc_command_release_internal_api` —— 而后者不放 buffer，于是 parity 命令
  的 buffer 也泄漏了。（v15 那一步本身是「删逐行拷贝」的正确收敛，只是暴露了下层漏 buffer。）

**修复**：新释放原语在摘 desc 的同时读出 `descriptor->buffer_id` 并还池。所有走
`fc_command_release()` 的路径（普通命令、同步页原语、RAID parity）**一处修复、全部受益**。

---

## 2. 「出池即挂链」——消除半成品窗口

### 改动前的窗口

v15 的 `fc_command_configure_descriptor_internal` 顺序是：

```
分配 desc → 填字段 → 分配 buffer → 挂链
```

若 buffer 分配失败，desc 已出池但**不在链上** → 沿链释放回收不到它 → 需要一个
**就地回滚**把这条游离 desc 还池（v15 加的 patch，`fc.c:580` 一带）。

### 改动后

```
分配 desc → 填字段 → 挂链 → 分配 buffer
```

把挂链提到 buffer 分配**之前**。于是不变量变成：

> **凡已分配的 desc，必在命令的数据链上。**

任何一步失败，调用方沿链释放即**完整**回收，不需要任何回滚分支。v15 的就地回滚代码
**整段删除**（不是修，是让窗口不存在）。

**代价**（已确认可接受）：链上会短暂/长期存在 `buffer_id == FC_BUFFER_ID_INVALID` 的 desc
（buffer 池 128 < desc 池 256，buffer 分配失败是最现实的路径）。凡走链的读者都要容忍：

| 读者 | 处理 |
|---|---|
| `fc_descriptor_chain_release_head_internal` | `buffer_id == INVALID` 跳过 buffer 释放 |
| `fc_command_descriptor_count` | 按 `next_id` 计数，不看 buffer |
| `fc_get_cmd_cw_mask` | 按 `paa` 重建掩码，不看 buffer |
| `ftl_cmd_write_data` / `ftl_cmd_read_data` | 遇 `INVALID` 返回 `FTL_ERROR`（数据不完整，本就该失败） |

`fc_descriptor_alloc_internal` 也补了自洽：整体 `{0}` 会把 `next_id`/`buffer_id` 都变成 **0**
（两个 0 都是合法号——链会走进 desc 0，释放会误放别人的 buffer 0），故出池时**显式置哨兵**
`FC_DESCRIPTOR_ID_INVALID` / `FC_BUFFER_ID_INVALID`。

---

## 3. 建链只建链，搬数据归上层

**用户判据**：搬数据是「按是否为写操作」才做的事，且是**上层**的事。
若放进建链函数里，则「建链中途失败 → 整链释放」会把刚拷贝的数据**白拷一遍**。

**改动**：

- `fc_command_configure_descriptor_internal` 只做 分配 desc + 填字段 + 挂链 + 分配 buffer。
  **不碰数据。**
- 数据由 `ftl_cmd_write_data()` 在链建好之后、**仅写操作**调用（`ftl_sync_read_page_raw`
  等读路径不调）。
- 判据与 v13 一致：硬件 FCC 不提供「按 CW 拷贝」寄存器，数据是固件填的 → 属 FTL 侧策略。

---

## 4. 释放原语拆两个（传链头 id）

用户点名要两种粒度。原计划的形态是 `void f(fc_descriptor_id_t *head)` 传指针就地更新，
**但 `fc_command_t.desc_id` 是位域**（`DWORD desc_id : 16`），`&command->desc_id` **取不了地址**，
该形态无法编译。故实现为**传值 + 取回返回值**：

```c
/* 放链头那一个（含其 buffer），返回新链头；head 越界（空链）原样返回 */
fc_descriptor_id_t fc_descriptor_chain_release_head(fc_descriptor_id_t head);

/* 沿链放全部 desc + buffer，返回 FC_DESCRIPTOR_ID_INVALID */
fc_descriptor_id_t fc_descriptor_chain_release_all(fc_descriptor_id_t head);
```

用法（`cmd->desc_id` 是位域，不能取地址，故整体赋值取回）：

```c
cmd->desc_id = fc_descriptor_chain_release_all(cmd->desc_id);
```

内部实现一份 `fc_descriptor_chain_release_head_internal(fc_t *, fc_descriptor_id_t *head)`
（就地更新，供 `_all` 循环调用）+ 一层薄封装。`_all` 的循环上界 `CW_TOTAL_MAX` 是
防损坏链成环的安全网——释放路径死循环比早停更糟，故超界后**强制**把 `*head` 清成 INVALID。

**FTL 侧回滚点**（建链中途失败）现在统一为：

```c
cmd->desc_id = fc_descriptor_chain_release_all(cmd->desc_id);
return FTL_ERROR;
```

（`ftl_cmd_configure_descriptors` 与 `ftl_cmd_setup_ppa_pair` 各一处。）

---

## 5. 顺带删除：`fc_command_resources_rollback`

它唯一调用者（`fc_stripe_submit` 的 parity 建链失败路径）后面**紧跟**一次
`fc_command_release_internal_api()`，后者把「desc 链 + buffer + 命令槽位」一并做完，
前者纯属冗余。删除后 desc 链释放只剩**一份**实现
（此前三份：`release_internal_api` 自己 / `resources_rollback` / `fc_stripe_release_parity`）。

---

## 改动文件

| 文件 | 改动 |
|---|---|
| `src/fc/fc.c` | `fc_descriptor_alloc_internal` 补哨兵；新增 `fc_descriptor_chain_release_head_internal` / `_all_internal`；`fc_command_release_internal_api` 改用新原语（**修 buffer 泄漏**）；`fc_command_configure_descriptor_internal` 改为「出池即挂链」+ 去数据拷贝 + 删回滚；`fc_command_attach_cw_internal` 瘦身为 guard + 转发；删 `fc_command_resources_rollback` |
| `include/fc/fc_internal.h` | 新增两个 `_internal` 声明 |
| `include/fc/fc_interface.h` | 新增 `fc_descriptor_chain_release_head` / `_all` 声明；更新 `fc_command_attach_cw` 失败语义；`fc_command_release` 注释加「+ 各 CW buffer」 |
| `src/fc/fc_interface.c` | 新增两个公共薄壳 |
| `src/ftl/ftl_fc_interface.c` | 两处建链失败回滚改用 `fc_descriptor_chain_release_all`；doc 注明「只建链，不搬数据」 |
| `docs/design/fc_nand_design.md` | §2.1 池布局 v16 段；§2.9 接口数 19→21 + 两个新签名；v16 失败语义；§2.10 生命周期；图 1；§26.3 |
| `docs/design/fc_api_usage.md` | v16 横幅；接口表 +2 行；验证表更新；写/读流程图补失败路径与「搬数据只在写」 |
| `docs/changelog/fc_chain_ownership_v16.md` | 本文件（新增） |

---

## 验证

| 配置 | 命令 | 结果 |
|---|---|---|
| debug | `make clean && make debug` | ✅ 148636 B，零警告 |
| release | `make clean && make release` | ✅ 85352 B，零警告 |
| RAID | `make clean && make debug CFLAGS_EXTRA=-DCONFIG_FC_RAID=1` | ✅ 152304 B，零警告 |
| 格式 | `make format-check` | ✅ 通过 |
| 单元测试 | `make -k tests` | 29 个既有错误（24 × `command_id`、4 × `fc_scheduler_t.busy`、1 × `consecutive_write_count`），**与本次改动无关，数量未增** |

**上板待验**（用户执行）：

1. **buffer 池不再泄漏**：连续提交 >16 条 2-page multi-plane 写命令，`info` 的 buffer 使用量
   应在释放后回落到基线（修复前会单调爬升到 128 封顶）。
2. **建链失败回滚完整**：制造 buffer 池耗尽，`ftl_cmd_configure_descriptors()` 返回失败后
   desc 池应完整还池（不残留游离 desc）。
3. **常规回归**：`info → wr 0 2 → rd 0 2 c → tr 0 → fl → info`。

---

## 未决 / 有意留下

- **`fc_command_cw_attached` 断言保留**。v15 曾讨论它是否冗余：掩码路径确实产不出重复，
  但 `ftl_cmd_setup_ppa_pair` 的 plane 取自 `pba_to_plane(pba[i])`（L2P 查表结果），
  两个 LBA 可能解析到同一 plane → 重复 `(plane, cw)` **真实可达**。设计文档原先给的理由
  （「掩码迭代可能重复」）是错的，已在 §25.4 更正（`assertion_guidelines.md` §十二附同步）。
  **release 构建下无任何守卫**这条数据驱动的违规——留待后续（可加返回码而非仅 ASSERT）。
- **`fc_descriptor_chain_release_head` 目前无生产调用者**（`_all` 已覆盖现有路径）。
  按用户要求提供，作为「按需回收链头」的原语备用。

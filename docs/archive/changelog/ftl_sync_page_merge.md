# v14：页级同步原语收敛——18 份内联 → 4 份公共

**日期**: 2026-10-07
**相关设计**: `docs/design/fc_nand_design.md` §26（分层判据）、`ftl_fc_interface.h`（⑧ 页级同步原语）
**前置**: `fc_policy_layer_uplift.md`（v13 策略层上移 FTL）

---

## 概述

9 个 FTL 模块各自内联了一份 `static sync_write_page` / `sync_read_page` /
`sync_read_page_raw` / `sync_erase_block`，共 **18 份、逐字节相同**，只差四个字段
（`crc` / `random` / `module` / `lba`）。本次收敛为 **4 个公共函数 + 一份配置结构**
（`ftl_page_cfg_t` + `FTL_PAGE_CFG_META/DATA` 复合字面量宏），落在 `ftl_fc_interface.c`
（该层已是「FTL 对 FC 的包装」，9 个模块均已 include 其头文件）。

**纯重构，行为中性**：每个变体逐字段比对旧内联实现，语义完全一致（见 §四）。

---

## 一、收敛后的接口

```c
typedef struct { uint8_t crc, random, ldpc, module; lba_t lba; } ftl_page_cfg_t;

ftl_status_t ftl_sync_write_page(nand_addr_t addr, const void *data, uint32_t length, const ftl_page_cfg_t *cfg);
ftl_status_t ftl_sync_read_page (nand_addr_t addr, void *buffer,      uint32_t length, const ftl_page_cfg_t *cfg);
ftl_status_t ftl_sync_read_page_raw(nand_addr_t addr, void *buffer, uint8_t module);
ftl_status_t ftl_sync_erase_block(nand_addr_t addr);
```

调用形态：

| 用途 | 调用 |
|---|---|
| 元数据页（journal / L2P / metadata / checkpoint / BBT / 擦除） | `FTL_PAGE_CFG_META(FC_COMMAND_MODULE_XXX)` |
| 用户数据页（GC / WL 搬移、Read Recovery 重读） | `FTL_PAGE_CFG_DATA(FC_COMMAND_MODULE_XXX, lba)` |
| 裸页（LDPC_RAW，忽略 NAND 成败） | `ftl_sync_read_page_raw(addr, buf, FC_COMMAND_MODULE_XXX)` |

四个函数共用两个 static helper：`sync_page_prepare`（alloc + 填配置 + `addr_to_cmd` 建链）
与 `sync_page_submit_wait`（submit + `ftl_internal_sync_wait`，**不判 NAND 成败**——留给调用者，
因为 RAW 读刻意忽略它）。

---

## 二、迁移清单

| 文件 | 删除 static | 迁移调用点 |
|---|---|---|
| `gc.c` | 2（写 / 擦） | 2 |
| `wear_leveling.c` | 1（写） | 1 |
| `journal.c` | 3（写 / 读 / 擦） | 6 |
| `checkpoint.c` | 2（写 / 读） | 2 |
| `l2p_cache.c` | 2（读 / 写） | 2 |
| `metadata.c` | 2（写 / 读） | 3 |
| `bbt.c` | 3（写 / 读 / 裸读） | 3 |
| `read_recovery.c` | 1（读） | 3 |
| `power_failure_recovery.c` | 2（读 / 裸读） | 4 |
| **合计** | **18** | **26** |

（18 是**定义**数；**调用点**是 26 处。）

---

## 三、⚠ `module` 是同步路径的死字段（本次核查副产品）

`power_failure_recovery.c` 的 `sync_read_page_raw` 用 `FC_COMMAND_MODULE_BBT`，而同文件的
`sync_read_page`（非 raw）用 `FC_COMMAND_MODULE_METADATA`——**是笔误**，但**可观测行为为零**：

1. `module` 全仓只有一个读者：`ftl.c:69` 的 `switch (completion->module)`，位于
   `ftl_handle_completion`（`completion_table` 的**默认 handler**）。
2. `completion_table_drain`（`completion_table.c:147`）**先查表**——命中表项走
   `ftl_sync_waiter_handler`，未命中才走默认 handler。
3. 18 个 `sync_*` 全走 `ftl_internal_sync_wait` → `completion_table_add` 注册；注册发生在
   `fc_submit` 之后、首次 `fc_service` 之前，而 `fc_submit` 只入队不推进 NAND（`fc.c:1471`）
   ——**完成项永远在注册之后才产生**。
4. 即便落到默认 handler，`switch` 里 `METADATA / GC / CHECKPOINT / BBT` 与 `default`
   **行为相同**（都是 release）。

**处理**：迁移时**保留原值 `BBT`**，以严格行为中性。要修是将来单独一次一行改动。
结论同时记入 `ftl_fc_interface.h` 的 `ftl_page_cfg_t.module` 字段注释。

---

## 四、行为中性核对

逐字段比对旧内联实现与新合并实现：

| 变体 | 差异点 | 结论 |
|---|---|---|
| 写 | `prepare`(operation/cw/crc/random/ldpc/multi_plane/chain/page_type/module/meta) 与旧版逐字段相同；`write_data` 在 submit 前 | ✅ 一致 |
| 读 | `submit_wait` → 判 FAIL → **`read_data`** → release（顺序同旧版） | ✅ 一致 |
| 裸读 | **不判 NAND_FAIL**，4×CW 按 `FC_BUFFER_POOL_CW_SIZE` 间隔拷 `FC_BUFFER_USER_SIZE` | ✅ 一致 |
| 擦除 | cw=0、超时 **20000U**、module=METADATA | ✅ 一致 |

唯一结构差异：submit 失败时，旧版在函数内 `fc_command_release`，新版由
`sync_page_submit_wait` 返回 -1、调用者 release——**净效果相同**。

---

## 五、验证

| 项 | 结果 |
|---|---|
| `make format` | 通过 |
| `make debug` | 零警告零错误，**148236** 字节 |
| `make release` | 零警告零错误 |
| `make format-check` | 通过 |
| `make debug CFLAGS_EXTRA=-DCONFIG_FC_RAID=1` | 零警告零错误，152904 字节 |
| 残留裸 `sync_*` 引用（`src/ftl`） | 仅 `ftl_fc_interface.c` 的 2 个新 static helper，无遗留 |
| 代码净变化 | `src/` 约 −1000 行（9 个模块） + 190 行（`ftl_fc_interface.c`） |

**上板验证未做**（依赖用户）：常规回归 `info → wr 0 2 → rd 0 2 c → tr 0 → fl → info`。

---

## 六、未做 / 遗留

- **测试不受影响**：26 处调用点全在 `src/ftl/*.c`，非测试代码；`tests/` 无 `sync_*` 引用。
- `power_failure_recovery.c` 的 `BBT` 笔误**保留**（§三）。
- 未动 `ftl_internal_sync_wait` 本身（它是这 4 个函数共同的下游，形态见 §七）。

---

## 七、后续可议（不在本次范围）

这四个函数暴露的 `ftl_internal_sync_wait` 阻塞形态，是原型与量产主控之间最大的分界线：
行业主控的「泵」是**调度器唯一所有者 + 事件源分离 + 工作项状态机**，而本项目是
**可重入的万能推进器 + 调用栈上的阻塞**。真正往行业靠需要把「等」从调用栈移进状态机，
代价是线性流程被拆成回调。本项目约束（单核、浅队列、无 QoS、无真 host）下收益为零，
故本次只做「18→4」的结构收敛，不动阻塞模型。

# v15：命令身份去缓存化 + 资源簿记去重复

**日期**: 2026-10-07
**相关设计**: `docs/design/fc_nand_design.md` §2.1（资源池）、§2.2（`fc_command_t`）、§2.9（对外接口）、
`docs/design/fc_api_usage.md`（命令 ID 由指针派生）
**前置**: `fc_policy_layer_uplift.md`（v13 策略层上移 FTL）、`ftl_sync_page_merge.md`（v14）

---

## 概述

两件独立但同源的事，都是「同一件事存了两份真值」：

1. **命令池 ID 是缓存字段**——`fc_command_t.command_id` 只是 `command - command_pool` 的副本，
   FC 内部**三处已经不信它**、自己算。删除字段，ID 一律由指针派生。
2. **「一条命令拥有哪些 desc」有两条链**——数据链（`desc_id` + `next_id`）之外还有一条
   「所有权链」（`descriptor_owned_next[]` + `command_descriptor_head[]`，640B），
   为「desc 已出池但未挂进数据链」的中间态兜底。把那个中间态就地回滚掉，整条链删除。

两件事合起来：`fc_command_t` **24 → 20 字节**，`.bss` **净 −896 字节**（−640 链 −256 字段）。

---

## 一、命令池 ID 由指针派生

### 删除的字段

```c
struct fc_command_t {
-   DWORD command_id : 16;   /* Command Pool ID; 0xFFFF means invalid. */
-   DWORD reserved0  : 16;   /* Reserved for Command Pool metadata. */
    DWORD desc_id    : 16;
    ...
};
```

`reserved0` 的注释是「Reserved for Command Pool metadata」，是 `command_id` 的配对填充，
一并删除（位域仍恰好占满一个 DWORD：16+1+4+8+3 = 32）。

### 唯一的派生实现

| 函数 | 位置 | 检查 |
|---|---|---|
| `fc_command_to_id_internal(controller, command)` | `fc.c` | 仅 `ASSERT`（前置：指针属本池），**无条件做减法** |
| `fc_command_to_id(command, &id)` | `fc_interface.c` | 真运行时区间检查，返回 0 = 指针不在池内 |

⚠ **减法不能放进 `ASSERT`**：`NDEBUG` 会把赋值一起裁掉，留下未初始化变量
（`-Wmaybe-uninitialized` / UB）。内部版无条件算，只在 `ASSERT` 里查区间；
真正的运行时检查放在公共壳。

### 迁移清单

| 侧 | 位置 | 改动 |
|---|---|---|
| FC 写者 | `fc.c` `fc_init_internal` 建链循环 | 删 `command_pool[index].command_id = index;` |
| FC 写者 | `fc.c` `fc_command_alloc_internal` | 删 `command_pool[command_id].command_id = command_id;` |
| FC 读者 | `fc.c` `fc_descriptor_alloc_internal` / `fc_submit_to_queue` / `fc_command_release_internal_api` | 三处内联减法 → `fc_command_to_id_internal()` |
| FC 读者 | `fc_command.c` `fc_command_status_init` | 读字段 → `fc_command_to_id()`（NULL / 池外指针 → `FC_COMMAND_ID_INVALID`） |
| FTL 读者 | `ftl.c` ×4、`gc.c`、`read_recovery.c` ×2、`ftl_fc_interface.c` | `cmd->command_id` → `ftl_cmd_id(cmd)` |

FTL 侧新增 `ftl_cmd_id()` 静态内联（`include/ftl/ftl_fc_interface.h`，四个调用文件都已 include 它），
避免在 FTL 里铺 10 份 `fc_command_to_id` 两行样板。

**保留**：`fc_command_status_t.command_id`（CQ 位域）——它是跨模块关联键
（`nand_dispatch_completion_t` / `pending` / `completion_table`），不是缓存。

---

## 二、删除「所有权链」（640B）

### 原来为什么有两条链

| | 数据链 | 所有权链 |
|---|---|---|
| 链指针 | `fc_descriptor_t.next_id` | `descriptor_owned_next[]` |
| 链头 | `command->desc_id` | `command_descriptor_head[cmd]` |
| 顺序 | 追加序（plane-major） | 逆分配序（头插） |
| 读者 | NAND 投递 / FTL 拷贝 / 掩码重建…十几处 | **只有 release 一处** |
| 何时挂 | `fc_command_chain_append`（分配**之后**） | `fc_descriptor_alloc_internal`（分配**当时**） |

`fc_command_configure_descriptor_internal` 是「分配 desc → 分配 buffer → 填字段」三步，
任一步失败就 `RESOURCE_EMPTY`。旧实现下 buffer 失败时 **desc 已出池、已登记归属，但还没进数据链**
（`command->desc_id` 里没有它）——只沿数据链回收会漏。所有权链就是为这个窗口兜底的。

### 消除窗口

```c
    if ((descriptor == NULL) || (buffer_id == FC_BUFFER_ID_INVALID))
    {
+       if (descriptor != NULL)
+       {
+           fc_descriptor_release_internal(controller, (uint16_t)(descriptor - controller->descriptor_pool));
+       }
        if (buffer_id != FC_BUFFER_ID_INVALID) { (void)fc_buffer_release_internal(controller, buffer_id); }
        return FC_STATUS_RESOURCE_EMPTY;
    }
```

（buffer 池 128 < desc 池 256，所以「desc 成功而 buffer 失败」是这里最现实的路径。）

**新不变量**：**desc 出池 ⟺ 立刻挂进数据链，或立刻回滚。**
`fc_command_configure_descriptor_internal` 成功返回到 `fc_command_chain_append` 之间没有失败点
（后者返回 void），所以「命令拥有哪些 desc」只有数据链一份真值。

### 删除与新增

| 动作 | 对象 |
|---|---|
| 删 | `g_fc_descriptor_owned_next[256]`（512B）、`g_fc_command_descriptor_head[64]`（128B）、`fc_t` 两个指针字段 |
| 新增 | `fc_descriptor_release_internal()`（static，出池的逆操作：清本体 + 清 owner + 头插空闲链） |
| 改 | `fc_command_release_internal_api()` 沿 `command->desc_id` / `next_id` 走，循环上界 `CW_TOTAL_MAX`（防损坏链成环；与 `fc_command_descriptor_count` 同一判据） |
| 留 | `descriptor_owner[256]`——**O(1) 反查表**，`fc_command_descriptors_owned_internal`（submit 时校验链所有权）与 `fc_get_resource_stats` 在用，链提供不了这个 |

### 为什么不是「一条链 + 遍历函数」

「desc 归哪条命令」若只能从链头走，就从 **O(1) 变 O(N)**：`fc_submit` 的所有权校验要
O(命令数 × 链长)，`fc_get_resource_stats` 同理。方向反了，故 `descriptor_owner[]` 保留。

### 为什么不是数组（`fc_command_t.cw_desc[8]` + count）

算过账：`cw_desc[8]` × 2B = 16B/命令 × 64 = **1KB**，比省下的 640B 还多，**净 +384B**；
若保留 `desc_id` 再额外加数组，位域从 32 位溢出到 64 位，`fc_command_t` 变 40B，**净 +640B**。
更根本的是**数组天生超配一倍**：8 槽 × 64 命令 = 512 个索引槽，desc 池只有 256 个 desc。
链是 256 项，正好等于池容量。故不采纳。

---

## 三、顺带修掉的三个既有毛病（v14 阶段一）

1. **`fc_command_to_id` 的 O(256) 线性扫描**（`fc_interface.c`）：注释写「指针算术」，
   函数体却是线性扫描，且只在 FC 内部三处内联版之外存在。现统一为 O(1) 减法 + 区间检查。
2. **`fc_command_release` 没有状态检查**：重复释放会把同一槽位两次压入空闲链（链表成环）。
   现**幂等**（已在 FREE 直接返回 OK）。`ftl.c` 里那个恒真的守卫
   `if (cmd->command_id != FC_COMMAND_ID_INVALID)`（释放把结构体清零 → 归 0 不是 0xFFFF）
   随之删除，METADATA/GC/CHECKPOINT/BBT 与 `default` 合并成一个分支。
   幂等性收在 `fc_command_release_internal_api()` 一处（RAID 路径直接调它）。
3. **RAID 路径三份 desc 链释放循环**：`fc_command_resources_rollback` 唯一调用者后面紧跟
   `fc_command_release_internal_api()`，纯冗余 → 整函数删除；`fc_stripe_release_parity`
   的逐行拷贝 → 改为委托。至此 desc 链释放只有一份实现。

---

## 四、验证

| 项 | 结果 |
|---|---|
| `make format` / `make format-check` | 通过 |
| `make debug` | 零警告，**148556** 字节 |
| `make release` | 零警告，**85224** 字节 |
| `make debug CFLAGS_EXTRA=-DCONFIG_FC_RAID=1` | 零警告，**152224** 字节 |
| `arm-none-eabi-nm` | `g_fc_command_pool` = 0x500（**1280B = 64 × 20**，原 24B/条）；`g_fc_descriptor_owned_next` / `g_fc_command_descriptor_head` 已消失 |
| `make -k tests` | 29 错误，**全部**为测试侧迁移 + 既有 v8/v13 漂移（见 §五），本次改动未新增 |

---

## 五、测试影响（阶段三，用户域）

`make tests` 编译集只含 `tests/unit/test_fc_level{1,2,3}.c` + `ut_main.c`
（`src/fc/makefile.mk` 的 `FC_NAND_TEST_ENABLE` 段）。

**本次字段删除直接打断 24 处**：

| 文件 | 行 | 说明 |
|---|---|---|
| `test_fc_level1.c` | 147 / 175 | **栈上** `cmd.command_id = 0x01U`——需改成池命令（`fc_command_alloc()`），否则 `fc_command_status_init` 派生不出 ID |
| `test_fc_level1.c` | 644 / 650 / 654 / 657 / 692 / 693 / 696 / 699 / 734 / 735 / 744 / 782 / 805 / 815 / 857 / 861 / 893 / 900 | `cmd->command_id` 传给 `fc_submission_queue_push` → 改 `fc_command_to_id()` |
| `test_fc_level3.c` | 477 / 522 / 736 / 780 | 同上 |
| `test_fc_nand.c` | 127 / 268 | **不在 `make tests` 编译集内**（集成测试，用户自建），不报错但迟早要改 |

`test_fc_level3.c:402/403` 的 `CMD_FIELD_U(command_id)` / `CMD_FIELD_U(reserved0)` 位于
`#if 0`（362–447，命令改写排查插桩）内，不参与编译；重开时需删这两行。

**既有的 v8/v13 漂移**（与本次无关，仍在）：`fc_scheduler_t.busy` / `consecutive_write_count` /
`fc_scheduler_set_busy|clear_busy|select|select_chain`、`fc_command_configure_descriptors`、
`nand_dispatch_set_device_ready`。

---

## 六、上板待验（用户域）

1. **重复释放**：同一命令连调两次 `fc_command_release` → 第二次返回 `OK` 且空闲链不变。
2. **半成品回滚**：把 buffer 池耗到只剩几个再发多 CW 命令 → `RESOURCE_EMPTY` 后
   `info` 的 desc/buffer 用量应回落到提交前的值（旧实现下 desc 会漏）。
3. 常规回归：`info → wr 0 2 → rd 0 2 c → tr 0 → fl → info`。

---

## 七、后续可议（不在本次范围）

- **方案 C（数组化）**：把 `desc_id` + `next_id` 换成 `fc_command_t` 内的
  `fc_descriptor_id_t cw_desc[CW_TOTAL_MAX]` + `count`，追加从 O(N) 尾走变 O(1)。
  空间是净增的（见 §二末），且牵动 `fc_data_path.c` 二十来处走 `next_id` 的循环，
  值得单独立项。
- `fc_command_status_init` 在 `src/` 中**没有生产调用者**（只有测试用）。要么接线，
  要么连同测试一起清理。

# FTL 设计变更历史

> 本文档记录 FTL 模块设计过程中的关键讨论、决策反转、方案对比和选型理由。
> 本文档与 `fc_nand_design.md` 顶部的变更历史（Phase A~N）风格一致，用于追溯 FTL 设计的演进过程。

---

## v0.1 — 初始实现（Phase Unknown）

**日期**：2026-09-21 前

**内容**：
- 页级 L2P 映射（页面级 `ssd_ppa_t`，每 LBA 4096B）
- 顺序物理页分配（`next_free_ppa` 单调递增）
- 直接调用 `nand_read_page()` / `nand_program_page()` 绕过 FC
- Controller 路由：同步阻塞式 `ftl_read/write` → NAND

**问题**：
1. ❌ **绕过 FC 层**：没有 CRC/ECC/Random/RAID 数据保护
2. ❌ **地址编码不一致**：`ssd_ppa_t`（uint64_t 线性页号）与 `nand_paa_t`（uint32_t bitfield）不兼容
3. ❌ **LBA 不对齐**：LBA_SIZE=4096B vs NAND page=2048B，写 4KB 跨 2 页
4. ❌ **9 个桩模块**：allocator、GC、wear_leveling、BBT、metadata 等都是 `#warning` 占位
5. ❌ **同步阻塞模型**：未利用 FC 的异步非阻塞能力

**状态**：标记为需重写的初始实现。

---

## v0.2 — 完整 DRAM-less 设计

**日期**：2026-09-23

### 讨论内容

基于 FC 层的完整设计文档 `fc_nand_design.md` 和项目现有代码结构，以 FTL 层为蓝本进行完整设计。

### 关键决策

| 序号 | 决策 | 选择 | 拒绝方案 | 理由 |
|------|------|------|---------|------|
| **D1** | 映射策略 | **纯 CW 级（512B）页映射** | 块映射、混合映射（BAST/FAST） | 写放大小、行业标准、FC 对齐 |
| **D2** | L2P 存储 | **NAND（DRAM-less）** | SDRAM 全表（8MB）、SRAM 全表 | 学习嵌入式/UFS 主流，有 1MB SRAM 可做 cache |
| **D3** | 替换策略 | **CLOCK** | LRU（链表复杂）、FIFO（命中率低） | 接近 LRU 命中率、无动态内存、O(1) |
| **D4** | 写回策略 | **Write-back + Journal** | Write-through（WA 高）、纯 Write-back（掉电风险） | 性能与掉电安全折中 |
| **D5** | 映射粒度 | **512B（1 CW）** | 2048B（页面级）、4096B | 与 FC CW USER 区完全对齐 |
| **D6** | FTL → NAND | **通过 FC 接口** | 直接 nand_* 调用（现状） | 完整数据保护 |
| **D7** | RAID | **不实现** | CONFIG_FC_RAID=1 开启 | 复杂度高，SLC 暂不需要 |
| **D8** | Super Block | **跨双 Plane Super Block** | 单 Block | 学习企业级跨 Die 条带化逻辑，当前双 Plane 做跨 Plane 配对 |
| **D9** | 温度分区 | **Hot / Cold 双 Super Block** | 单温区 | 提升 GC 效率，冷热数据分离 |
| **D10** | OP | **10% 预留（可配置 7~28%）** | 0% | 支持 RMA 坏块替换 + 性能恢复，消费级典型值 10% |
| **D11** | Free Pool | **三级结构**（Pre-conditioned / Normal / Emergency） | 单池 | 学习 Background GC 和前台 IO 隔离 |
| **D12** | SLC Cache | **不实现** | — | 本身就是 SLC NAND，无需模拟 SLC |

### 地址编码决策

- FTL 使用 FC 的 `nand_paa_t`（uint32_t）格式
- 放弃原有 `ssd_ppa_t`（uint64_t 线性页号）
- PAA 布局：`[CW:2][PAGE:6][PLANE:1][BLOCK:11]` = 20 bit 有效
- `cmd->paa` 填 CW=0 基址，FC 内部自动生成 desc 链

### 关于 RAID 的讨论

RAID 功能因实现复杂度过高，在当前 SLC 阶段不实现。具体不确定点：
- Stripe RAID 与 GC 的交互（parity block 如何参与 GC）
- 跨 Plane 错误的报告语义（数据命令成功、parity 失败时如何通知 FTL）
- parity block 的分配管理（allocator 能否分配 parity plane 的 block）

**结论**：RAID 延后，已有 ECC（BCH t=4）在 SLC 上可提供足够保护。

---

## 行业对标讨论摘要

### 映射策略的行业主流

| 时期 | 主流策略 | 代表产品 | 原因 |
|------|---------|---------|------|
| ~2007 | 块映射 | 早期 U 盘、低端控制器 | SRAM 极度有限（几 KB~几十 KB）|
| 2007~2012 | 混合映射（BAST/FAST） | 中低端 SSD、U 盘主控 | SRAM 增大（64~256KB），但 DRAM 仍罕见 |
| 2015~至今 | 纯页映射 | **所有现代 SSD**（企业/消费/UFS） | SRAM/DRAM 降价，硬件条件满足 |

纯页映射是绝对主流。块映射和混合映射已被淘汰，核心原因：
- QLC/TLC 的有限 P/E 寿命无法承受 BAST/FAST 的 merge 开销
- DRAM/HMB 普及，表空间不再是限制因素

### DRAM-less vs 有 DRAM

| 类型 | 做法 | 经典产品 |
|------|------|---------|
| 有 DRAM 企业级 | 全表常驻 DRAM | Samsung PM9A3、Solidigm D7 |
| 有 DRAM 消费级 | 全表常驻 DRAM | Samsung 990 PRO、WD SN850X |
| DRAM-less 消费级 | HMB 借用主机内存 | WD SN580、Phison E21T |
| DRAM-less 嵌入式 | NAND + SRAM cache（本方案）| UFS 4.0、eMMC 5.1 |

**本方案定位**：DRAM-less 嵌入式，与 UFS 旗舰主控架构一致。

---

## v0.18 — FTL 全部模块实现 + 代码审视修复

**日期**：2026-09-29

### 内容
FTL 层剩余 5 个模块全部实现，21 个源文件 0 个 `#warning` 核心桩。代码审视修复 4 个逻辑断裂。

### 新增模块

| 模块 | 文件 | 内容 |
|------|------|------|
| **Checkpoint** | `checkpoint.h/c` | 全量 L2P 快照 + Journal 截断协同 + 三级触发（水位线/写入量/定时）+ 紧急 95% 强制 + Block 37-38 各 3 页冗余 |
| **BBT** | `bbt.h/c` | 出厂扫描（LDPC_RAW 读 Page 0）+ NAND 持久化（Block 36+39 各 3 页冗余，CRC 保护）+ 运行时标记 + Spare Pool 预留接口 |
| **Read Recovery** | `read_recovery.h/c` | Stage 1 SET_FEATURES Read Retry（8 次重试）+ Stage 2 邻近页辅助 + ECC 趋势预测（环形 buffer，连续上升告警）+ 4 级 Block 健康状态 |
| **Power Failure Recovery** | `power_failure_recovery.h/c` | 三类启动判断（fresh/clean/PF）+ 写指针 NAND 扫描 + ftl_boot_recovery 主入口 |
| **Wear Leveling** | `wear_leveling.h/c` | 动态（alloc 选低擦除）+ 静态三星 Daemon（三条件触发：差值 5000 / 比值 3x / 低区占比 5%）+ wl_relocate 搬移 + wl_execute 集成 ftl_service |

### 代码审视修复（4 个逻辑断裂）

| # | 问题 | 影响 | 修复 |
|---|------|------|------|
| 1 | `l2p_cache_set_md_dir()` 从未调用 | `cache->md_dir` 永远 NULL，所有 Region 查询回退固定位置，Metadata Directory 完全无效 | `ftl_init()` 中加调用 |
| 2 | `bbt_init()` 从未调用 | 出厂坏块不加载，所有 4096 Block 视为好块 | `ftl_init()` 中加调用 |
| 3 | `ftl_internal_sync_wait()` 中 release "other" completion | 用户读的 pending 表项泄漏，8 次后所有读请求失败 | 删除 release 代码，注释说明单 LUN 无并发 |
| 4 | `ftl_boot_recovery()` 调 `journal_replay` 做判断 | 双重 replay 浪费 2× Journal 扫描时间 + 修改 journal 内部状态 | 改为直接读 header `page_seq` |

### 编译状态
**零错误零警告**。FTL 层 21 个源文件全部通过。Binary size: 58368 bytes。

### 剩余 #warning（非 FTL 层）
- Controller 层：`scheduler.c` `state_machine.c` — 主机请求调度/状态机（`request.c` 已在 v0.23/v0.36 实现，非桩）→ ✅ **v0.47 已落地**（最小状态机 + 调度钩子，见 v0.47 条目）
- NVMe 层：`nvme_admin.c` `nvme_io.c` `submission_queue.c` `completion_queue.c` — 当前用 UART CLI，5 个桩文件（含 nvme_controller.c 返回 NOT_SUPPORTED）→ ✅ **v0.47 已删除**（C2 砍掉，UART CLI 驱动 FTL，D10）
- Platform BSP：`timer.c` `uart.c` `cache.c` `dma.c` `pcie.c` `interrupt.c` — 已有 soc.h + DWT 实现

---

## v0.19 — Completion Table 统一完成路由 + L2P Region 动态分配

**日期**：2026-09-29

### 背景

代码审视发现三个结构性隐患：
1. **两泵冲突**：`ftl_service` Phase 2 和 `ftl_internal_sync_wait` 各自调用 `fc_poll_completion` 消费 CQ entry。同步等待循环中的 `fc_poll_completion` 会"吞掉"用户 I/O 的完成事件——数据不拷贝到 caller buffer、pending 不释放、命令不 release。
2. **`ftl_write` 完成判定依赖全局 SQ/CQ 空**：在并发内部操作（GC、Region flush、Checkpoint）同时运行时可能提前返回或无限等待。
3. **L2P Region 首次写入回退固定位置**：`l2p_cache_flush_one` 中的 TODO 显示 Region 动态分配未接入，始终回退 Block 0~31。

### 解决方案

新增 `Completion Table` 模块（`completion_table.h/c`）：

```
统一完成路由设计（企业级 Unified Completion Pump）：

每个 I/O 提交时可注册 completion handler：
  completion_table_add(cmd_id, handler, ctx);

fc_poll_completion() 消费一个 CQ entry 后：
  ① cmd_id → 查 completion_table → 找到 handler
  ② handler(cmd, ctx) — 可能触发 pending 回拷或设置 waiter->done
  ③ 无注册 → 走 default handler（ftl_handle_completion）

所有完成事件统一经过 completion_table_drain() 分发：
  ftl_service Phase 2:  completion_table_drain()
  ftl_internal_sync_wait:  completion_table_add → completion_table_drain()
  ftl_read/ftl_write 同步循环:  completion_table_drain()
```

### 改动清单

| 文件 | 改动 | 说明 |
|------|------|------|
| `include/ftl/completion_table.h` | **新增** | 16 项路由表【v0.31 扩为 32】+ `ftl_sync_waiter_t` + 统一泵声明 |
| `src/ftl/completion_table.c` | **新增** | 路由表实现 + `ftl_sync_waiter_handler`（带/不带 release）|
| `src/ftl/ftl.c` | 重写 | `ftl_handle_completion` 改为 default handler 注册；Phase 2 改为 `completion_table_drain()`；同步循环统一泵 |
| `src/ftl/ftl_fc_interface.c` | 重写 | `ftl_internal_sync_wait` 改为 `completion_table_add` + `completion_table_drain()` 循环，不再独占 `fc_poll_completion` |
| `include/ftl/l2p_cache.h` | 修改 | 新增 `alloc` 指针；`l2p_cache_set_md_dir` 增加 `alloc` 参数 |
| `src/ftl/l2p_cache.c` | 修改 | `l2p_cache_flush_one` 中 TODO 消除：首次写 Region 调用 `metadata_dir_alloc_region` 动态分配 |
| `src/ftl/ftl.c` → `ftl_init()` | 修改 | 两次 `l2p_cache_set_md_dir`：md_dir 未就绪时设 NULL（首次），加载后重设（含 alloc）|

### 三个隐患修复状态

| 隐患 | 状态 | 机制 |
|------|------|------|
| 同步等待吞 completion | ✅ **已修复** | `completion_table_drain` 自动路由非目标 completion 到 default handler |
| ftl_write 全局空判定 | ⚠️ **改善** | 同步循环已用统一泵，后续可改为 per-cmd sync_waiter |
| Region 回退固定位置 | ✅ **已修复** | 首次写通过 `metadata_dir_alloc_region` 从 Free Pool 分配 |

### 多 LUN 并发前置条件

现在同步等待和用户 I/O 共用同一个泵，完成事件不被吞。
后续还需：
1. CQ 深度确认 — 确保 FC 层 CQ 能容纳多 LUN 完成事件
2. `FTL_PENDING_MAX` 扩展 — 从 8 增至 32 ✅ **（v0.31 已完成）**
3. `ftl_write` 同步等待改为 per-cmd sync_waiter

### 编译状态

**零错误零警告**。新增 `completion_table.o` 398B text + 200B bss（32 项路由表，v0.31 扩）。FTL 层 22 个源文件。
Binary size: 58368 bytes。

---

## v0.20 — 返回值体系统一 + 历史死代码清理 + WL/GC 重复消除

**日期**：2026-09-29

### 背景

FTL 层 ~30 个操作函数的返回值不统一：`int 0/-1`、`SSD_STATUS_OK/IO_ERROR`、`(int)fc_status_t` 混用，
调用者需要记住每个函数的成功/失败约定。同时项目积累了一批已退役的历史遗留文件。

### 返回值统一方案（企业级风格）

参考存储主控行业做法，FTL 内部按职责分三类：

| 类别 | 返回值 | 函数数 | 说明 |
|------|--------|--------|------|
| **操作函数** | `ftl_status_t` | ~32 | `FTL_OK=0` / `FTL_ERROR=-1`，两值简洁枚举 |
| **谓词函数** | `int` | ~8 | 保持 `1=是` / `0=否`（check_trigger/needs_gc 等） |
| **计数函数** | `int` | ~3 | 保持非负=数量（gc_migrate_sb/wl_relocate 等） |
| **对外 API** | `ssd_status_t` | 3 | 保持 `SSD_STATUS_*`（ftl_read/write/trim） |

### 新增类型

`include/ftl/ftl_type.h`：

```c
typedef enum {
    FTL_OK    = 0,   /* 成功 */
    FTL_ERROR = -1,  /* 通用失败 */
} ftl_status_t;
```

与 `ssd_status_t` 的边界：FTL 对外 API 依然返回 `ssd_status_t`，内部模块间调用返回 `ftl_status_t`。
在 `ftl_read/write` 包装中：`FTL_ERROR` → `SSD_STATUS_IO_ERROR`。

### 改动清单

#### 头文件签名变更（13 个头文件）

| 模块 | 函数 | 旧 | 新 |
|------|------|----|----|
| `allocator.h` | `allocator_init` | `int` | `ftl_status_t` |
| `bbt.h` | `bbt_init/mark_bad/save/load/spare_pool_refill` | `int` | `ftl_status_t` |
| `checkpoint.h` | `checkpoint_create/find_latest` | `int` | `ftl_status_t` |
| `gc.h` | `gc_read_page_with_lba` | `int` | `ftl_status_t` |
| `journal.h` | `journal_replay` | `int` | `ftl_status_t` |
| `l2p_cache.h` | `l2p_cache_flush_one/flush_all` | `int` | `ftl_status_t` |
| `metadata.h` | `metadata_dir_flush_to_anchor/load` | `int` | `ftl_status_t` |
| `completion_table.h` | `completion_table_add` | `int` | `ftl_status_t` |
| `ftl_fc_interface.h` | 全部 7 个函数 | `int` | `ftl_status_t` |
| `power_failure_recovery.h` | 全部 4 个函数 | `int` | `ftl_status_t` |
| `wear_leveling.h` | `wl_init()` | 删除 | — |
| `ftl.h` | `ftl_init/submit_read/submit_write` | `int` | `ftl_status_t` |
| `ssd_types.h` | `ssd_ppa_t` | 删除 | — |

#### 审视修复（返回值切换中发现的 bug）

| # | 问题 | 文件 | 修法 |
|---|------|------|------|
| 1 | `checkpoint_find_latest` 改为 `ftl_status_t` 后调用处 `if (!cp_found)` 逻辑反转 | `power_failure_recovery.c` | `if (checkpoint_find_latest(&cp) != FTL_OK)` |
| 2 | `ftl_cmd_setup_ppa` 等 3 函数返回 `(int)fc_*()` 强转 | `ftl_fc_interface.c` | `(fc_*() == FC_STATUS_OK) ? FTL_OK : FTL_ERROR` |

#### 历史死代码删除

| 文件 | 理由 |
|------|------|
| `include/nand/nand.h` + `src/nand/nand.c` | 旧 NAND 后端抽象，0 调用，已被 FC 层取代 |
| `include/nand/nand_geometry.h` + `src/nand/nand_geometry.c` | 唯一用户是 nand.h（已死），几何参数已在 nand_config.h |
| `config/nand_geometry.h` | 设计文档已标废弃 |
| `config/ftl_config.h` | SSD_FTL_LBA_SIZE=4096 0 引用 |
| `include/ftl/ftl_type.h` → `typedef uint32_t pba_t` | 无人用，全用 nand_pba_t |
| `include/ftl/wear_leveling.h` → `wl_init()` | 5 行空实现 0 处调用 |
| `include/ftl/allocator.h` + `src/ftl/allocator.c` → `allocator_refill_pre_cond` | dead code（free_count 已在 allocator_free_block 中自同步）|
| `common/ssd_types.h` → `typedef uint64_t ssd_ppa_t` | 已被 nand_paa_t 替代 |
| `__patch_01.txt` + `__patch__.txt` | 根目录临时残留 |

#### WL/GC 重复代码消除

| 改动 | 效果 |
|------|------|
| `wl_relocate` 内联 FC 命令序列（~45 行）→ `gc_read_page_with_lba()` | `gc_read_page_with_lba` 从 `static` 升为 `gc.h` 公开接口 |
| `wl_relocate` 擦除循环（~15 行）→ `gc_erase_and_free()` | 构造临时 `super_block_t` 后复用 GC 擦除逻辑 |
| `gc.h` 的 `PRE_COND_LOW_WATER` → 删除（与 allocator.h 的 `FREE_POOL_TARGET` 重复）| 调用处改为 `FREE_POOL_TARGET` |

### 编译状态

**零错误零 FTL 警告**。源文件数 105→103。Binary size: 58368 bytes（不变，死代码不链接）。

---

## v0.21 — FTL 拓扑信息重构（ftl_topology_t）

**日期**：2026-09-29

### 背景

`ftl_context_t` 中散落 `ch/ce/lun` 三个字段，`ftl_init` 需要 8 个参数，
Controller 层被迫知道底层 NAND 拓扑。行业做法是 FTL 对外只暴露 LBA，
拓扑信息在初始化时一次性传入后由各子模块内部持有。

### 改动

将分散在各结构体中的 `ch/ce/lun` 字段统一为 `ftl_topology_t`：

| 结构体 | 原字段 | 改为 |
|--------|--------|------|
| `ftl_context_t` | `ch / ce / lun` | `ftl_topology_t topo` |
| `ftl_allocator_t` | `ch / ce / lun`（曾被删除又恢复） | `ch / ce`（保留，从 topo 初始化） |
| `l2p_journal_t` | `ch / ce / lun` | `ftl_topology_t topo` |
| `metadata_dir_cache_t` | （无） | `ftl_topology_t topo` |

### 新增类型

`ftl_type.h`：

```c
typedef struct {
    uint8_t ch;
    uint8_t ce;
    uint8_t lun;
} ftl_topology_t;
```

### 接口变化

```c
// 旧
ftl_init(ftl, ch, ce, lun, free_blocks, free_count, bad_blocks, erase_counts, closed_sb_list);

// 新
ftl_init(ftl, topo, free_blocks, free_count, bad_blocks, erase_counts, closed_sb_list);
```

所有元数据路径（Journal/Checkpoint/BBT/Anchor）使用 `topo.ch/ce` 构造 PBA。
用户数据路径（allocator_alloc）使用 `alloc->ch/ce` 构造 PBA。

### 波及的文件

| 文件 | 改动 |
|------|------|
| `include/ftl/ftl_type.h` | **新增** `ftl_topology_t` |
| `include/ftl/ftl.h` | `ftl_context_t` 增加 `topo` 字段；`ftl_init` 增加 `ftl_topology_t` 参数 |
| `include/ftl/allocator.h` | 恢复 `ch/ce` 字段；`allocator_init` 恢复 `ch/ce` 参数 |
| `include/ftl/journal.h` | `l2p_journal_t` 增加 `topo`；`journal_init` 增加 `ftl_topology_t` 参数 |
| `include/ftl/metadata.h` | `metadata_dir_cache_t` 增加 `topo` 字段 |
| `include/ftl/checkpoint.h` | `checkpoint_find_latest` 增加 `ftl_topology_t` 参数 |
| `include/ftl/bbt.h` | `bbt_load` 增加 `ftl_topology_t` 参数 |
| `src/ftl/ftl.c` | `ftl_init` 签名更新，子模块初始化传拓扑 |
| `src/ftl/journal.c` | 全部 `pba_make(0,0,...)` → `pba_make(topo.ch, topo.ce, ...)` |
| `src/ftl/allocator.c` | `allocator_init` 保存 `ch/ce`；`allocator_alloc` 用 `alloc->ch/ce` |
| `src/ftl/gc.c` | `pba_make(0,0,...)` → `pba_make(alloc->ch, alloc->ce, ...)` |
| `src/ftl/checkpoint.c` | `pba_make(0,0,...)` → `pba_make(topo.ch/ce)` 或 `ftl->topo.ch/ce` |
| `src/ftl/bbt.c` | `pba_make(0,0,...)` → `pba_make(alloc->ch/ce)` 或 `pba_make(topo.ch/ce)` |
| `src/ftl/metadata.c` | `pba_make(0,0,...)` → `pba_make(dir->topo.ch/ce)` |
| `src/ftl/wear_leveling.c` | `pba_make(0,0,...)` → `pba_make(ftl->topo.ch/ce)` |
| `src/ftl/power_failure_recovery.c` | `pba_make(0,0,...)` → `pba_make(ftl->topo.ch/ce)`；`checkpoint_find_latest` 传 `ftl->topo` |

### 编译状态

**零错误零 FTL 警告**。Binary size: 58368 bytes。

## CLOCK 替换策略升级方向

当前选择 CLOCK 算法作为替换策略。以下为已识别的优化方向，标记为**未来升级**，当前不实现：

| 升级方向 | 说明 | 触发条件 | 预期收益 |
|---------|------|---------|---------|
| **CLOCK-pro** | access_count 从固定位宽改为动态阈值，热数据更易留在 cache 中 | Phase 3 性能优化 | 命中率 +3~5% |
| **自适应模式切换** | 顺序访问时 prefetch 预加载后续 region，随机访问回到 CLOCK | Sequential 检测 | 顺序读场景命中率接近 100% |
| **Thrashing 降级** | 命中率低于 10% 时切换 write-through + 停 background flush | miss_rate > 90% | 避免性能断崖 |

---

## 各版本对比

| 维度 | v0.1（初始实现） | v0.2（当前设计） |
|------|-----------------|-----------------|
| 映射粒度 | 页面级（2048B） | CW 级（512B） |
| 地址编码 | `ssd_ppa_t`（uint64_t） | `nand_paa_t`（uint32_t） |
| 表存储 | SDRAM | **NAND（DRAM-less）** |
| L2P 缓存 | 无（全表在 SDRAM） | SRAM region cache（64~128 reg） |
| 替换策略 | 无 | CLOCK |
| FTL → NAND | 直接 nand_* | **通过 FC 接口** |
| 数据保护 | 无 | CRC + Random + ECC |
| 掉电恢复 | 无 | Journal + Checkpoint |
| GC | 桩（#warning） | 贪心 + 同步搬移 |
| Allocator | `next_free_ppa++` | Free list + 磨损均衡 |
| Block 类型 | 未区分 | 元数据/数据/空闲/坏块 |
| Modules | 9 个桩 | 完整接口定义 |
| RAID | 无（CONFIG=0） | 无（CONFIG=0） |

---

## v0.23 — D4 多 LBA 拆分（Controller 层方案 A）

**日期**：2026-09-29

### 背景

`block_count != 1` 的多 LBA 请求一直被 `count != 1U` 守卫拒绝。FTL 对外接口的 `count` 参数语义不清晰——它存在但从未真正被消费。

### 决策：方案 A — Controller 层拆分

存储主控行业标准做法：Controller 层负责主机协议翻译（多 LBA → 单页子请求），FTL 层只处理单页原语。

### 改动清单

#### FTL 接口净化

| 接口 | 旧签名 | 新签名 |
|------|--------|--------|
| `ftl_submit_read` | `(ftl, lba, buf, count)` | `(ftl, lba, buf, complete_flag, on_complete, ctx)` |
| `ftl_submit_write` | `(ftl, lba, data, count, complete_flag)` | `(ftl, lba, data, complete_flag, on_complete, ctx)` |

- 删除 `count` 参数（FTL 只支持单页）
- 新增 `pending_cb_t on_complete` + `void *on_complete_ctx` 回调（pending 完成通知）
- 读请求新增 `complete_flag` 参数（此前读没有完成信号）

#### Pending 表扩展

`ftl_pending_req_t` 变化：
- 删除 `count` 字段（未使用）
- 新增 `on_complete` 回调指针 + `on_complete_ctx` 上下文
- `pending_add` 签名对应更新

#### Completion 路径

`ftl_handle_completion` 中新增回调调用：`complete_flag=1` 后 → `on_complete(ctx)`（pending 释放前）。

读请求 PBA 无效路径也发完成信号（memset 零后 → `complete_flag=1` → `on_complete(ctx)`），确保所有提交路径的完成语义一致。

#### Controller 拆分

`controller.c`：
- `block_count == 1`：直通 FTL，传 `complete_flag`
- `block_count > 1`：路由到 `ssd_request_split()`

`request.c`（核心拆分实现）：

```c
static void on_sub_complete(void *ctx) {
    ssd_request_t *req = (ssd_request_t *)ctx;
    if (req->sub_remaining > 0U) req->sub_remaining--;
    if (req->sub_remaining == 0U) req->complete_flag = 1U;
}
```

`ssd_request_t` 新增 `sub_remaining` 计数器。

### 波及的文件

| 文件 | 改动 |
|------|------|
| `include/ftl/ftl.h` | `ftl_submit_read/write` 签名：删 count + 加 complete_flag(读) + 加 on_complete/ctx |
| `include/ftl/pending.h` | 删 count + 加 on_complete/ctx；新增 `pending_cb_t` 类型 |
| `src/ftl/pending.c` | `pending_add` 签名更新 |
| `src/ftl/ftl.c` | `ftl_submit_read/write` 实现对应调整；`ftl_handle_completion` 加 on_complete 调用；PBA 无效发完成信号 |
| `include/controller/ssd_request.h` | 新增 `sub_remaining` 字段 |
| `src/controller/controller.c` | block_count 拆分路由（1→直通，>1→ssd_request_split） |
| `src/controller/request.c` | 核心拆分实现 + on_sub_complete 回调聚合 |
| `src/controller/completion.c` | 注释更新 |

### 编译状态

**零错误零警告**。Binary size: 58368 bytes（不变）。

---

## v0.24 — D5 写失败回滚（modify-after-confirm + BBT 两阶段 + 自动回滚）

**日期**：2026-09-29

### 背景

`ftl_submit_write` 在 `fc_submit` 后立即执行 `journal_append` + `l2p_cache_write`，此时 NAND PROGRAM 尚未完成。如果 PROGRAM 失败：

1. **Journal 包含不可逆的映射** — 掉电后 replay 将 LBA 指向未写完成的新块 → 数据静默损坏
2. **L2P 指向垃圾数据** — 映射已更新但物理页无效
3. **坏块未标记** — 失败的 block 没有标记为坏，后续可能被重新分配

同时 `bbt_mark_bad` 在当前实现中同步写 NAND（`bbt_save` → 6 页 PROGRAM），如果在 completion handler 中调用会阻塞 completion 泵数毫秒。

### 决策：modify-after-confirm + BBT 两阶段 + 自然回滚

#### 顺序修正

`journal_append` / `l2p_cache_write` / `checkpoint_journal_watermark` / `total_writes++` 从 `ftl_submit_write` 移入 `ftl_handle_completion` 的写成功确认路径，NAND PROGRAM 确认成功后执行（存储主控 industry standard）。

#### BBT 两阶段化

| 阶段 | 位置 | 操作 | NAND 访问 |
|------|------|------|-----------|
| 快速路径 | `ftl_handle_completion` | BBT bitmap 置位 + allocator 列表标记 | ❌ 无 |
| 后台持久化 | `ftl_service_idle/normal` → `bbt_flush()` | 攒批写 BBT payload 到 Block 36+39（6 页） | ✅ |

#### 回滚

写失败后 L2P 不动（自动指向 `old_pba`），无额外回滚逻辑。Super Block 空洞由 GC 扫描时读 NAND 失败自然跳过。

### 改动清单

#### Pending 表扩展

`include/ftl/pending.h`：
- `ftl_pending_req_t` 新增 `old_pba` / `new_pba` / `ftl` 三个字段
- `pending_add` 签名新增 3 参数

`src/ftl/pending.c`：`pending_add` 实现对应更新

#### ftl_submit_write 顺序修正

`src/ftl/ftl.c` — `ftl_submit_write`：
- 删除 `journal_append()` / `l2p_cache_write()` / `checkpoint_journal_watermark()` / `total_writes++`
- `pending_add` 传 `old_pba`, `new_pba`, `ftl`
- 注释改为 `/* Journal + L2P 在 completion handler 中确认 NAND PROGRAM 成功后执行 */`

#### ftl_handle_completion 确认/回滚

`src/ftl/ftl.c` — `ftl_handle_completion` FC_COMMAND_MODULE_FTL 路径：

| 条件 | 动作 |
|------|------|
| NAND_SUCCESS + buffer != NULL（读成功） | `ftl_cmd_read_data`（不变） |
| NAND_SUCCESS + buffer == NULL（写成功） | `journal_append` + `l2p_cache_write` + `checkpoint_journal_watermark` + `total_writes++` |
| NAND_FAIL + buffer != NULL（读失败） | `memset` 零（不变） |
| NAND_FAIL + buffer == NULL（写失败） | `pba_to_block` → `bbt_mark_bad`（RAM only） |

#### BBT 两阶段化

`src/ftl/bbt.c` — `bbt_mark_bad`：删除 `bbt_save()` 调用
`src/ftl/bbt.c` — 新增 `bbt_flush()` 公开函数
`include/ftl/bbt.h` — 声明 `bbt_flush()`
`src/ftl/ftl.c` — `ftl_service_idle` + `ftl_service_normal` 中调用 `bbt_flush()`

### 波及的文件

| 文件 | 改动 |
|------|------|
| `include/ftl/pending.h` | pending 加 old_pba/new_pba/ftl 字段 + pending_add 签名扩展；恢复 FTL_PENDING_MAX 宏 |
| `src/ftl/pending.c` | pending_add 实现对应更新 |
| `src/ftl/ftl.c` — `ftl_submit_write` | 移出 journal/l2p/watermark/total_writes；pending_add 传 old_pba/new_pba/ftl |
| `src/ftl/ftl.c` — `ftl_handle_completion` | 写成功→确认 journal+l2p；写失败→坏块标记 |
| `src/ftl/ftl.c` — `ftl_submit_read` | pending_add 适配新签名 |
| `src/ftl/bbt.c` — `bbt_mark_bad` | 删除同步 bbt_save() |
| `src/ftl/bbt.c` | 新增 bbt_flush() |
| `include/ftl/bbt.h` | 声明 bbt_flush() |
| `src/ftl/ftl.c` — `ftl_service` | IDLE/NORMAL 模式加 bbt_flush() |

### 编译状态

**零错误零警告**。Binary size: 58368 bytes（不变）。

---

## v0.25 — D6 队列深度/背压方案确认（pending 表满即隐式背压）
**日期**：2026-09-29

### 背景

D4 多 LBA 拆分后，单次 host 请求（`block_count=N`）产生 N 个 FC 命令同时入 SQ。
SQ 深度 64、Command Pool 64、Pending 表 32 各层资源构成多层漏斗。需要确认背压策略。

### 决策：方案① pending 表满即隐式背压

四种方案的原理、实现和行业使用情况在 `ftl_design.md §17` 中详细分析。

**选择理由（定量分析）**：

| 资源 | 深度 | 实际占用峰值 | 结论 |
|------|------|-------------|------|
| SQ | 64 | budget=4 + pending ≤32 + HSQ ≤6 ≈ 42 | << 64 ✅ |
| Command Pool | 64 | 同 SQ ≈ 42 | << 64 ✅ |
| Pending 表 | 32 | D4 拆分 block_count=8 时占用 8/32 ⚠️ | 并发上限（v0.31 扩） |

核心逻辑：
- SQ/池满几乎不会发生：实际并发 << 深度
- pending 表满时 `pending_add()` → `FTL_ERROR` → fc_submit 之前就拒绝
- SRAM 是 DRAM-less 最贵的资源，不设 retry queue（方案②）
- NVMe 实现后自然过渡到方案④（主机级流控）→ **v0.47 删 NVMe（学习主控 UART CLI 驱动）后不适用**；UART CLI 两段式天然免背压

**不采用**：
- 方案②（内部排队重试）：需额外 SRAM，与 DRAM-less 设计相悖
- 方案③（动态 budget）：当前资源深度足够，预算固定即可
- 方案④（主机级流控）：NVMe 层还没实现 → v0.47 后不适用（Host = UART CLI）

### 波及的文件

| 文件 | 改动 |
|------|------|
| `docs/design/ftl_design.md` | 新增 §17 D6 详细分析（四种方案原理 + 对比表 + 定量推算 + 选择理由） |

**无代码改动**。

---

## v0.26 — D7 多命令流水线（Multi-Plane + Cache Program 安全中间页）

**日期**：2026-09-29

### 背景

D4 的"FTL 只做单页原语"过于绝对——Multi-Plane 操作中 FTL 必须知道页数才能配置正确的 OP 码和 multi_plane 字段。同时 Super Block 设计（`plane_blocks[2]`）天生为双 Plane 配对，但 `allocator_alloc` 只返回单页 PBA。

### 决策：修正 D4 + Multi-Plane + Cache Program 安全

#### 修正 D4：加回 pages 参数

```c
ftl_submit_read(ftl, lba, buf, pages, complete_flag, status_out, on_complete, ctx)
ftl_submit_write(ftl, lba, data, pages, complete_flag, status_out, on_complete, ctx)
```

| pages | 含义 | OP 码 | multi_plane | 数据量 |
|-------|------|-------|------------|--------|
| 1 | 单 Plane | `OP_NORMAL_*` | 0 | 2048 |
| 2 | 双 Plane Multi-Plane | `OP_*_MULTI_PLANE` | 2 | 4096 |

#### Multi-Plane

写总是 pages=2（Super Block 双 Plane 同步写入），读由 Controller 层两两连续 LBA 配对。

#### Cache Program 安全

`pending_req_t` 加 `cache_intermediate:1` 标志。中间页（15h）completion 时不更新 Journal/L2P，等最终页（10h）确认后一起更新。

### 波及的文件

| 文件 | 改动 |
|------|------|
| `include/ftl/ftl.h` | `ftl_submit_read/write` 签名加 `pages` 参数 |
| `include/ftl/pending.h` | 结构体加 `pages` + `cache_intermediate`；`pending_add` 签名加 2 参数 |
| `src/ftl/ftl.c` | `submit_read/write` pages 分支 + Multi-Plane OP 码；`handle_completion` cache 中间页跳过确认 |
| `src/ftl/pending.c` | `pending_add` 实现加 2 参数 |
| `src/controller/controller.c` | 单块直通传 pages=1 |
| `src/controller/request.c` | 配对拆分 pages=2，奇数 pages=1；`sub_remaining` 修正为 `ceil(N/2)` |

### 编译状态

**零错误零警告**。Binary size: 58368 bytes（不变）。

---

## v0.27 — D8 错误通知粒度（status_out 消费级完成状态）

**日期**：2026-09-29

### 背景

`complete_flag` 只能通知"完成"，不能区分"成功完成"和"失败完成"。消费级 DRAM-less 不需要企业级的 ECC 计数、failed descriptor ID、Error Log Entry——只需要知道"成功了还是失败了"。

### 决策：status_out 指针

pending 表加 `volatile ssd_status_t *status_out` 指针（同 complete_flag 模式），completion handler 在置 `complete_flag` 的同时写入 `SSD_STATUS_OK` / `SSD_STATUS_IO_ERROR`。

```
ftl_handle_completion:
  NAND_SUCCESS + 读 → ftl_cmd_read_data → *status_out = SSD_STATUS_OK
  NAND_SUCCESS + 写(非cache中间页) → journal+l2p → *status_out = SSD_STATUS_OK
  NAND_FAIL + 读 → memset 零 → *status_out = SSD_STATUS_IO_ERROR
  NAND_FAIL + 写 → bbt_mark_bad → *status_out = SSD_STATUS_IO_ERROR
  cache_intermediate → 不设 status_out（由最终页决定）
```

### 波及的文件

| 文件 | 改动 |
|------|------|
| `include/ftl/pending.h` | pending 加 `status_out`；`pending_add` 签名加 1 参数；加 `#include "common/ssd_types.h"` |
| `src/ftl/pending.c` | `pending_add` 实现加 `status_out` |
| `include/ftl/ftl.h` | `ftl_submit_read/write` 签名加 `volatile ssd_status_t *status_out` |
| `src/ftl/ftl.c` — `ftl_handle_completion` | 各路径写 `*status_out = OK/IO_ERROR` |
| `src/ftl/ftl.c` — `ftl_submit_read/write` | `pending_add` 传 `status_out` |
| `src/controller/controller.c` | 单块传 `&request->completion_status` |
| `src/controller/request.c` | 拆分传 `&request->completion_status` |

### 编译状态

**零错误零警告**。Binary size: 58368 bytes（不变）。

## v0.28 — 地址体系双层化 + 形参优化 Step 1/2（nand_addr_t + addr_to_cmd + 同步操作内联化）

**日期**：2026-09-30

### 背景

- 形参数过多：`ftl_internal_read/write_page(7)`、`pending_add(12)`、`ftl_submit_read/write(8)`、`ftl_init(7)`
- 遍历路径无意义打包拆包：`pba_make` 打包 → `ftl_cmd_setup_ppa` 拆包（GC 搬移、元数据扫描、Checkpoint）
- `super_block_t` 不含拓扑，调用点从外部传 ch/ce

### 决策：地址双层化

存储主控内部地址分两层：

| 层 | 类型 | 用途 | 布局 |
|----|------|------|------|
| **展开** | `nand_addr_t` | 遍历/迭代路径直填 FC 命令 | 独立字段（ch/ce/lun/block/plane/page/cw），无需 shift |
| **紧凑** | `nand_pba_t` (packed) | L2P 表存储接口 | 32-bit 位域（[CH][CE][PAA]） |

遍历路径不再打包→拆包，直接用展开地址：

```c
/* nand_config.h — 双向转换 */
nand_pba_t pba_from_addr(const nand_addr_t *addr);   // addr → PBA
nand_addr_t addr_from_pba(nand_pba_t pba);           // PBA → addr（随机路径）
nand_addr_t addr_from_paa(uint8_t ch, uint8_t ce, nand_paa_t paa);  // PAA+CH/CE → addr（元数据）

/* ftl_fc_interface.h — 地址直填 */
ftl_status_t addr_to_cmd(fc_command_t *cmd, const nand_addr_t *addr);
```

`super_block_t` 加 `ftl_topology_t topo` 字段，新增 `super_block_page_addr(sb, plane, page)`
（ch/ce/lun 取自 sb->topo）。旧 `super_block_page_pba(ch, ce, sb, ...)` 保留为兼容委托。

### Step 1 内容（地址类型 + 结构体拓扑）

| 文件 | 改动 |
|------|------|
| `include/nand/nand_config.h` | `nand_addr_t` + `pba_from_addr` + `addr_from_pba` + `addr_from_paa`；修复头文件守卫破坏 |
| `include/ftl/allocator.h` | `super_block_t` 加 `topo`；新增 `super_block_page_addr`；旧 PBA helper 保留委托 |
| `include/ftl/ftl_fc_interface.h` | 新增 `addr_to_cmd` inline |
| `include/common/ftl_types.h` | 新建：`ftl_io_request_t` / `ftl_init_params_t`（Step 3/4 使用） |

### Step 2 内容（删除 4 个同步包装，内联化）

`ftl_internal_read_page / write_page / erase_block / read_page_raw` 四个包装函数从
`ftl_fc_interface.c` 删除（-232 行），各模块内联为 `static` 同步助手
（`alloc → addr_to_cmd → submit → sync_wait → release`）：

| 模块 | 内联助手 | 地址来源 |
|------|---------|---------|
| `l2p_cache.c` | `sync_read_page` / `sync_write_page` | `addr_from_paa` / `addr_from_pba` |
| `journal.c` | `sync_read/write_page`、`sync_erase_block` | `journal_page_addr` / `journal_block_addr` |
| `checkpoint.c` | `sync_read/write_page` | `checkpoint_page_addr` |
| `metadata.c` | `sync_read/write_page` | `anchor_page_addr` |
| `bbt.c` | `sync_read/write_page`、`sync_read_page_raw` | `bbt_page_addr` |
| `gc.c` | `sync_write_page`、`sync_erase_block` | `addr_from_pba` / `super_block_page_addr` |
| `wear_leveling.c` | `sync_write_page` | `addr_from_pba` |
| `read_recovery.c` | `sync_read_page` | `addr_from_pba` / `addr_from_paa` |
| `power_failure_recovery.c` | `sync_read_page`、`sync_read_page_raw` | `journal_page_addr` |

仅保留 `ftl_internal_sync_wait`（completion_table 统一路由）。

### 保留原则

- **随机 IO 路径**（`ftl.c` 从 L2P 拿 PBA 后 `ftl_cmd_setup_ppa`）**保留不动** —— PBA 已在手，无需展开
- `gc_read_page_with_lba`（GC 读页 + 提取 LBA）保留不动
- 元数据操作不走 Super Block，保持 `multi_plane=0`

### 编译状态

**零错误零警告**。13 文件，+1379/-463（含 Step 1 未提交部分）。`ftl_fc_interface.c` 净 -227。

### 下一步

- Step 3：`ftl_submit_read/write` 改 `(ftl, req)` 结构体传参；`pending_req_t` 嵌入 `io_request`
- Step 4：`ftl_init` / `allocator_init` 改指针传参（`ftl_init_params_t`）

## v0.29 — 形参优化 Step 3（ftl_submit_read/write → (ftl, req) + pending 嵌入 io）

**日期**：2026-09-30

### 背景

`ftl_submit_read/write` 有 8 个形参（lba/buffer/pages/complete_flag/status_out/on_complete/on_complete_ctx），
`pending_add` 有 12 个形参，pending 表与 submit 签名重复描述同一请求。

### 决策：ftl_io_request_t 结构体传参 + pending 按值嵌入

1. **`ftl_submit_read/write(ftl_context_t *ftl, const ftl_io_request_t *req)`** — 8 形参 → 2
2. **`ftl_pending_req_t` 嵌入 `ftl_io_request_t io`**（`g_pending[i].io = *req` 一次性拷贝 7 字段）
3. **`pending_add(cmd_id, io, old_pba, new_pba, ftl, cache_intermediate)`** — 12 形参 → 6
4. **位域 → 普通字段**：`used:1/command_id:15/pages:2/cache_intermediate:1` 展开（设计原则"非数组结构体不用位域"）

### pending_cb_t 迁移（打破循环 include）

- `pending.h` embed `ftl_io_request_t` 需 include `ftl_types.h`
- `ftl_types.h` 原本 include `pending.h` 取 `pending_cb_t` → 循环
- **方案**：`pending_cb_t` 迁至 `ftl_types.h`，`pending.h` 单向 include `ftl_types.h`
- 顺带修复遗留隐患：`ftl_init_params_t` 用 `super_block_t` 但未 include `allocator.h`（此前未编译进任何 .c）

### 波及的文件

| 文件 | 改动 |
|------|------|
| `include/common/ftl_types.h` | `pending_cb_t` 迁入；补 `#include "ftl/allocator.h"` |
| `include/ftl/pending.h` | 单向 include ftl_types.h；`ftl_pending_req_t` 嵌 `io` + 位域展开；`pending_add` 6 形参 |
| `src/ftl/pending.c` | `pending_add` 实现 `g_pending[i].io = *io`（结构体拷贝）；`lookup_by_lba` 改 `io.lba` |
| `include/ftl/ftl.h` | 两个 submit 签名改 `(ftl, req)`；补 include ftl_types.h |
| `src/ftl/ftl.c` | submit 改读 `req->` 字段；`ftl_handle_completion` 改 `pending->io.` 前缀；old/new_pba/ftl/cache_intermediate 不变 |
| `include/controller/ssd_request.h` | 加 include ftl_types.h；结构体加 `ftl_io_request_t io` |
| `src/controller/controller.c` | 单块：填 `request->io`（lba/buffer/pages + 通知指针）后 `ftl_submit_*(ftl, &request->io)` |
| `src/controller/request.c` | 拆分：公共字段（on_complete/status_out）循环前设一次，循环内只改 lba/buffer/pages |

### Controller 层调用形态

```c
/* 单块直通（controller.c） */
request->io.lba           = request->start_lba;
request->io.buffer        = request->buffer;
request->io.pages         = 1U;
request->io.complete_flag = &request->complete_flag;
request->io.status_out    = &request->completion_status;
ftl_submit_read(controller->ftl, &request->io);

/* 拆分（request.c）— 循环内复用 io 模板 */
request->io.complete_flag   = NULL;               /* 子请求用回调聚合 */
request->io.status_out      = &request->completion_status;
request->io.on_complete     = on_sub_complete;
request->io.on_complete_ctx = request;
for (i = 0; i < block_count; ) {
    request->io.lba    = start_lba + i;
    request->io.buffer = buf + i * FTL_READ_USER_SIZE;
    request->io.pages  = (i + 1 < block_count) ? 2U : 1U;
    ftl_submit_read(controller->ftl, &request->io);
    i += (request->io.pages == 2U) ? 2U : 1U;
}
```

> io 只需在 submit 调用期间有效（pending 表立即按值拷贝），循环内复用安全。

### 语义不变项（回归防护）

- `ftl_handle_completion` 分支逻辑（读成功/写确认/读失败 memset/写失败标坏块/cache_intermediate 跳过）零改动，仅字段前缀
- `data_size = FTL_READ_USER_SIZE × req->pages` 与 0/1/2 校验不变
- PBA 无效快速路径（memset + 通知）行为不变
- `pending_lookup / lookup_by_lba / remove` 不变
- `ftl_init_params_t`（Step 4 用）不动

### 编译状态

**零错误零警告**。23 文件，+1666/-697（含 Step 1/2 与文档）。`ftl.c` 净 -100、`pending.c` 净 -24、`request.c` 净 -48、`controller.c` 净 -26（均为形参聚合受益）。

### 下一步

- Step 4：`ftl_init` / `allocator_init` 改指针传参（`ftl_init_params_t`）

## v0.30 — 形参优化 Step 4（ftl_init / allocator_init → ftl_init_params_t 指针传参）

**日期**：2026-09-30

### 背景

`ftl_init` 7 个形参（topo/free_blocks/free_count/bad_blocks/erase_counts/closed_sb_list），
`allocator_init` 8 个形参（ch/ce + 同 6 个）。Step 1 已建 `ftl_init_params_t` 但未落地。

### 决策：结构体指针传参

```c
ftl_status_t ftl_init(ftl_context_t *ftl, const ftl_init_params_t *params);          /* 7 → 2 */
ftl_status_t allocator_init(ftl_allocator_t *alloc, const ftl_init_params_t *params); /* 8 → 2 */
```

`ftl_init` 内部将 `params` 原样透传给 `allocator_init`（拓扑经 `ftl->topo` 保持一致）。

### ftl_init_params_t 定义位置（打破循环 include）

- 初版放 `common/ftl_types.h`，但该结构体依赖 `super_block_t`（allocator.h 定义），
  而 allocator.h 需要 include ftl_types.h（拿 ftl_init_params_t）→ **循环 include 报错**
- **方案**：`ftl_init_params_t` 定义于 `include/ftl/allocator.h`（super_block_t 之后），
  `common/ftl_types.h` 收敛为 `pending_cb_t` + `ftl_io_request_t`（无 allocator 依赖）

```
common/ftl_types.h  ── pending_cb_t / ftl_io_request_t（无 allocator 依赖）
ftl/allocator.h     ── super_block_t → ftl_init_params_t（依赖闭合方向正确）
ftl.h               ── include common/ftl_types.h + ftl/allocator.h
```

### 波及的文件

| 文件 | 改动 |
|------|------|
| `include/common/ftl_types.h` | 删除 `ftl_init_params_t`（移往 allocator.h）；去掉 `#include "ftl/allocator.h"` |
| `include/ftl/allocator.h` | 新增 `ftl_init_params_t`（super_block_t 之后）；`allocator_init` 改 `(alloc, const params*)` |
| `src/ftl/allocator.c` | `allocator_init` 读 `params->topo/free_blocks/free_count/bad_blocks/erase_counts/closed_sb_list` |
| `include/ftl/ftl.h` | `ftl_init` 改 `(ftl, const params*)`；include 调整为 common/ftl_types.h + ftl/allocator.h |
| `src/ftl/ftl.c` | `ftl_init` 接收 params → `ftl->topo = params->topo` → `allocator_init(&ftl->alloc, params)` |

### 启动示例（host 层）

```c
ftl_init_params_t params = {
    .topo           = {0, 0, 0},
    .free_blocks    = g_free_blocks,
    .free_count     = BLOCKS_PER_PLANE * PLANE_NUM,
    .bad_blocks     = g_bad_blocks,
    .erase_counts   = g_erase_counts,
    .closed_sb_list = g_closed_sb_list,
};
ftl_init(&ftl, &params);
```

### 编译状态

**零错误零警告**。24 文件，+1848/-806（含 Step 1~3 与文档）。`allocator.c` 净 -14、`ftl.c` 净 -60（init 路径）。

### 总结：形参优化 4 步全部完成

| 步骤 | 内容 | 形参变化 | 状态 |
|------|------|---------|------|
| Step 1 | nand_addr_t + addr_to_cmd + super_block_t topo + ftl_types.h | — | ✅ v0.28 |
| Step 2 | 删除 4 个 ftl_internal_* 包装，内联 sync 助手 | 7 → 0 | ✅ v0.28 |
| Step 3 | ftl_submit_read/write (ftl, req) + pending 嵌 io | 8 → 2 / 12 → 6 | ✅ v0.29 |
| Step 4 | ftl_init / allocator_init (params 指针) | 7→2 / 8→2 | ✅ v0.30 |

### 下一步

- P-A：pending 表扩展 8 → 16/32（✅ v0.31 已做）
- P-B：scheduler / state_machine / NVMe 层（`#warning` 桩，6 文件）→ ✅ v0.47 落地/删除（最小状态机 + 调度钩子 + NVMe 删除，见 v0.47 条目）
- P-C.5：反向映射 bitmap（✅ v0.32 已做——`ftl_block_t.valid_map`）
- PFR closed_sb_list 掉电重建（v0.32 二期项）
- CP/GC 定时触发接线（`checkpoint_check_trigger(ftl, 0U)` / `gc_check_trigger(..., 500U)` 硬编码 elapsed；`cp_timestamp` 恒 0——需平台 `time_us` 钩子）

---

## v0.31 — P-A 多 LUN 并发前置 + read recovery 接入

**日期**：2026-09-30

**内容**：
- Pending 表 + Completion Table 扩容（8→32），多 LUN 并发软件前置
- 元数据 lun 泛化（bbt/checkpoint/metadata 地址构造器加 lun 形参）
- ECC 返回链路打通：`fc_ldpc_decode` 加 `err_bits` out 参数
- read recovery 接入：`ftl_context` 加 `rr` + `rr_block_track`（**未分配，空转**）

**遗留**：rr_block_track 无存储（空转）→ v0.32 BST 供电解决。

---

## v0.32 — Block State Table（BST）统一 per-block 结构体数组

**日期**：2026-09-30

### 背景

v0.31 read recovery 接入后，per-block 元数据分散四张表（erase_counts / bad_blocks /
g_bbt_bitmap / rr_block_track），无统一承载，且存 3 处全局/per-plane 块号碰撞 bug。

### 关键决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 统一单数组 vs 分散表 | **A：统一 `ftl_block_t[4096]`** | 单一数据源，GC/WL/read recovery/BBT 共享；符合行业 BIT |
| 粒度 | 物理 Block 全局号 | 擦除/坏块/ECC 是物理级事实；SB 是逻辑层 |
| 存储位置 | 调用者经 `ftl_init_params_t.blocks` | 与 free_blocks/closed_sb_list 指针契约一致；多 LUN 每 LUN 一张 |
| VPC + bitmap | **全真值**（valid_count + valid_map） | P-C.5 落地；GC/WL 迁移扫位图免 NAND 读 |
| 全字段 | **56B/224KB 全常驻 SRAM** | 功能优先，剩 ~537KB；档三裁剪见 ftl_design §19 |
| state 表示 | **bitmask 宏 + 访问器**（非位域/联合体） | 嵌入式可移植 + 可位运算 |
| bad_reason | 独立 `uint8_t` | 可读性（用户拍板），6 种原因枚举 |

### 实现

- 新建 `include/ftl/bst.h`：`ftl_block_t`（56B）+ 状态位 + 访问器 + 坏块原因枚举
- 修复 3 处全局/per-plane 块号碰撞：`ftl_gblock(plane, block)` 单点转换
- read recovery 由空转变为供电：ECC 历史入 `ftl_block_t.rr`
- VPC/bitmap 七点闭环：写成功（含覆盖清旧页）/ 读成功 / GC·WL 迁移 / 擦除 / trim / 坏块 / 掉电
- GC/WL 迁移改扫 `valid_map` 置位页（`__builtin_ctzll`），免无效页 NAND 读
- `bbt_mark_bad` 签名扩展带 bad_reason（3 处调用点）
- PFR open SB 扫描重建 bitmap + 置 OPEN
- **Closed 列表回收闭环**（审视补全）：新增 `allocator_remove_closed_sb`（swap-remove O(1)），
  GC 擦除后从 `closed_sb_list` 移除 victim——修 `closed_sb_count` 只增不减导致的列表爆炸 +
  已回收 SB 被再次选为 victim 的无限 GC 空转；`sb_id` 语义统一（OPEN 占位 NO_SB，关闭写 closed 索引，
  swap-remove 时同步修正）；`gc_erase_and_free` 加 `now` 参数传 `ftl->total_writes` 修 last_pe_ts
  非单调时间戳

### 编译状态

**零错误零警告**（`make` + `make tests`）。16 文件，+520/-355。

### 二期项

- **PFR closed_sb_list 掉电重建**：本期只做 open SB 状态机；closed 重建需扫全 NAND
  判有效页分组，涉及既有 PFR 缺口，独立立项
- **档三（SRAM 核心 + NAND 扩展）**：全常驻 224KB 的产品化裁剪路径，已写入 ftl_design §19，不实现

## v0.33 — FTL 内部边缘缺口闭环（write 路径紧急 GC + RR Stage 3 兜底）

**日期**：2026-10-01

### 关键决策

| 项 | 决策 | 理由 |
|----|------|------|
| Free Pool 耗尽 → write 路径 | **同步紧急 GC 兜底**（gc_select_victim + migrate + erase_and_free 1 个 SB 后重试一次） | 写请求在 Free Pool 枯竭瞬间不再丢弃；真无空间才返回 FTL_ERROR |
| RR Stage 3 | **显式坏块标记**（`bbt_mark_bad(FTL_BLOCK_BAD_READ)`） | 不做 XOR/RAID 恢复（FC raid 引擎孤儿代码）；页面损坏提升块级不可靠，预测搬移/GC 接手 |
| allocator.h #warning | 移除，改正式文档 | write 路径已闭环 |

### 实现

- `ftl_submit_write` 第①步：`allocator_alloc` 失败 → 同步 GC → 重试（`src/ftl/ftl.c`）
- 读失败恢复全失败 → `bbt_mark_bad(READ)`（`src/ftl/ftl.c`）
- `read_recovery.c` Stage 3 注释改显式兜底语义；`allocator.h`/`read_recovery.h` 文档同步

### 编译状态

零错误零新增警告（debug + tests）。commit `e727d29`。

## v0.34 — P-A 多 LUN 命令层机制启用（ISSUE/WAIT/FETCH + auto-split 活代码）

**日期**：2026-10-01

### 背景

Phase V 已建好 ISSUE/WAIT/FETCH 三阶段骨架，但 `LUN_NUM=1/LUN_ADDR_WIDTH=0` 使机制全为死代码。

### 关键决策

| 项 | 决策 | 理由 |
|----|------|------|
| LUN_NUM | 临时改 2（后续 v0.35 回滚至真实芯片 1） | 让门控 `(LUN_NUM>1U)` 变活、机制可验证 |
| 原子 OP pair_op | 3 个原子 OP 补 pair_op（READ→LOAD/PROGRAM→ISSUE/ERASE→ISSUE） | `nand_operation_is_splittable` 依赖 op_type==ATOMIC && pair_op!=0 |
| desc PAA lun | 修复 `paa_make(...,0U,...)` → 保留 `paa_to_lun(command->paa)` | 多 LUN desc META/随机种子/ECC 依赖 |
| 链式路径 | 补 auto-split（与单命令路径一致） | 单命令已有 fc.c:1341，链式缺失 |
| sb->topo | allocator_alloc 分配时绑定真实 ch/ce/lun | 此前恒零，擦除/GC 地址全零 |

### 编译状态

零错误零新增警告。commit `4e14c2a`。**注意：LUN_NUM=2 为临时验证值，v0.35 已回滚至 1。**

## v0.35 — 数据平面多维度化（L2P 分片 + ch/ce/lun 泛化 + 路由哈希）

**日期**：2026-10-01

### 背景

v0.34 把 LUN_NUM 改成 2 与 MT29F4G08 真实结构（单 die，DIE_NUM=1）不符。用户决策：**配置回滚真实芯片（LUN_NUM=1），代码按多维度写，用 CH_NUM/CE_NUM/LUN_NUM 宏定大小**——换芯片只改宏。

### 关键决策

| 项 | 决策 | 理由 |
|----|------|------|
| 配置 | LUN_NUM 2→1、LUN_ADDR_WIDTH 1→0 | MT29F4G08 单 die；P-A pair_op/auto-split 修复保留（换宏即活） |
| L2P 分片 | `L2P_REGIONS_PER_LUN = 2048/LUN_NUM`、`L2P_BLOCKS_PER_LUN`、`l2p_region_pba(ch,ce,lun,...)` | 行业标准：每 LUN 独立 region 表，总容量不变 |
| 路由 | `lba_route_ch/ce/lun` 哈希 + `lba_to_local` 交错 | 行业主控 LBA 哈希到三维；顺序写摊多 die |
| META 语义 | **存片内 local_lba** | GC/WL/journal replay/读恢复反查随 local 自洽（全链决策基准） |
| cache 拓扑 | `l2p_cache_t` 加 ch/ce/lun；`l2p_cache_init(cache,ch,ce,lun)` | 消固定位置寻址写死 0 |
| metadata_dir | 预填/alloc_region/anchor 按 `dir->topo.lun` 错开（ANCHOR_BLOCK_STRIDE + L2P_BLOCKS_PER_LUN） | 每 LUN 独立 anchor 区 |
| trim | 收全局 LBA → lba_to_local | controller 无维度知识，转换 ftl 内部完成 |

### 实现

- `nand_config.h`：LUN_NUM/LUN_ADDR_WIDTH 回滚
- `l2p_cache.h/.c`：分片宏 + 路由 + local 转换 + cache 绑定 topo + 固定位置回退按 lun
- `metadata.h/.c`：anchor stride + 预填/alloc/load 按 topo.lun
- `ftl.c`：read/write/trim 全局→local；`cmd->meta=local`；l2p_cache_init 绑 topo
- `pfr.c`：3 处 l2p_cache_init 同步

### 编译状态

零错误零新增警告（debug + tests）。commit `5f886e1`。LUN_NUM=1 折叠单 LUN，行为与现状一致。

### 遗留（v0.36/v0.37 已闭环）

- **controller 三维 ftl 指针 + 路由**：✅ 已实现（v0.36，见下）
- **main.c FTL 上线**：✅ 已实现（v0.37 eeb2ace，见下）——host 大数组 + ftl_init 循环 + free pool 回填 + boot 恢复 + controller 接入，主循环切 ftl_service
- SRAM：每 LUN 352KB（BST 224KB + L2P 128KB）；v0.37 实测 AXI_RAM≈355.5KB + SRAM2≈24KB 不溢出；LUN_NUM=2 需跨区

## v0.36 — Controller FTL 实例三维路由接入（ftl[CH][CE][LUN] + 按 LBA 路由）

**日期**：2026-10-01

### 背景

v0.35 数据平面多维化停在模块层（每 FTL 实例已绑 (ch,ce,lun)，但 controller 仍是单 `ftl_context_t *ftl` 指针，全仓无路由）。本版本把 controller 升级为三维指针数组 + 按 LBA 路由，把多维化推到系统层闭环。

### 关键决策

| 项 | 决策 | 理由 |
|----|------|------|
| init 接口 | `ssd_controller_init(controller, ftl_context_t *ftl[CH_NUM][CE_NUM][LUN_NUM])` | 收三维数组，逐槽校验非空 + 槽位 topo 一致 |
| 路由函数 | `ssd_controller_route`（controller.c 内部函数，request.c 拆分循环共用，不暴露进公共头） | 按 `lba_route_ch/ce/lun`（region 粒度取模）选实例；后续换哈希策略只改此处 |
| Multi-Plane 配对 | `ssd_split_use_multi_plane`：连续 LBA 同实例才 pages=2，跨 region 边界切实例时降级单 Plane | FC Multi-Plane desc 从单个 `cmd->paa` 展开 plane0/1（fc.c:597-605），一次操作永远落在单个 (ch,ce,lun)；跨实例配 pages=2 会写错芯片导致静默数据丢失 |
| 配对判断基准 | 用「route 相等」而非「region 相等」 | route 相等才是同实例的充要条件；region 相等过严漏掉大量可配对场景；换哈希自动跟随 |
| 子请求计数 | `ssd_split_count` 预统计（替代硬公式 `ceil(block_count/2)`） | 跨实例降级使实际子请求数 > 公式值，欠计导致父请求 `complete_flag` 永不置位（host 悬挂）；且未映射读会同步完成，必须先计数后提交 |
| route helper 位置 | controller.c 内部函数 + request.c 前向声明 | controller.h 不依赖 l2p_cache.h（保持接口头纯净，业界做法） |

### 实现

- `include/controller/controller.h`：`ssd_controller_t.ftl` 单指针 → `ftl_context_t *ftl[CH_NUM][CE_NUM][LUN_NUM]`；init 签名收三维数组
- `src/controller/controller.c`：新增 `ssd_controller_route`（内部函数）；init 三重循环逐槽校验（非空 + topo 一致）；submit 单块分支 3 处 `controller->ftl` → 局部 `ftl`（route 后）
- `src/controller/request.c`：新增 `ssd_split_use_multi_plane` + `ssd_split_count`；拆分循环内每轮按 `io.lba` 路由 + 同实例配对判断；4 处 `controller->ftl` → 局部 `ftl`

### 正确性要点

- 跨实例 Multi-Plane 必须降级：FC desc 从单 `cmd->paa` 展开，一次操作永远落在单 (ch,ce,lun)。跨实例配 pages=2 会把 512 数据写进 A 芯片 plane1，读路由到 B → B 的 L2P 查不到 → 静默读零（数据丢失）。
- 子请求计数必须预统计：跨实例降级使实际数 > `ceil(block_count/2)`，硬公式欠计 → host 悬挂；未映射读同步完成（ftl.c:472-487）须先计数后提交。
- LUN_NUM=1 折叠：`lba_route_*` 恒 0 → 配对恒真、计数 = ceil(block_count/2)，行为与现状完全一致（零回归）。

### 编译状态

零告警（gcc -pedantic）+ 固件 stm32h743 debug/release/tests 通过。commit `86552ce`（已推送）。

### 边界行为（用户确认）

拆分写失败保持 **partial write 语义**（已成功子请求不回滚，host 决定重试）——业界 NVMe 主流做法；批量回滚不在本次范围。

## v0.37 — main.c FTL 上线（✅ 已实现）

**日期**：2026-10-01

### 背景

v0.36 把 controller 路由接入，但 **FTL 仍无生产调用入口**——`src/main.c` 只做 `fc_init` + `nand_init` + UART + watchdog，从不调 `ftl_init`/`ftl_service`。本版本把 main.c 接入 FTL 全链路，让多维化真正可运行、可上板。

### 关键决策（已与用户确认）

| 项 | 决策 | 理由 |
|----|------|------|
| SRAM 布局（方案 B 修正） | BST `g_blocks[4096]`（224KB）**连续落 AXI_RAM**；`g_free_blocks` + `g_closed_sb_list` 落 **SRAM2**；l2p cache 随 ftl_context_t 在 AXI_RAM | SRAM1 仅 128KB（< BST 224KB），BST 不能落 SRAM1；拆两段会破坏 `params.blocks` 线性索引契约；AXI_RAM 空余 367KB，g_ftl(131.5)+g_blocks(224)=355.5KB 刚好放下 |
| BST 存储分层 | **档三分层记为后续独立任务，本次不动 BST 结构** | 大工程（读写路径全部改造 + NAND 布局 + 掉电一致性），塞进本次会爆炸 |
| free pool 回填 | 调用者（main.c）在 `ftl_init` 后遍历 BST 非坏块 `allocator_free_block` 逐个回填 | allocator_init 不填 free pool（free_count 硬清零）、bbt_scan_factory 只标坏块不产 free_blocks——**当前最大缺口，不回填则首次写失败** |
| boot 恢复 | `ftl_boot_recovery(&ftl, &result)` 由 main.c 调用（ftl_init 后、ftl_service 前） | ftl_init 内部不调用 boot；第一次上电 = BOOT_FRESH_START |
| 主循环 | `ftl_service(&g_ftl[0][0][0])` 替换裸 `fc_service()`（加 ftl_ready 守卫，失败降级 fc_service） | ftl_service Phase1 内部已含 fc_service（≤4 次） |
| 掉电重建 closed_sb_list | 本次不处理（v0.32 二期遗留） | power_failure 只重建 open SB；closed 列表为空时 GC 不回收已满 SB（空间泄漏但不丢数据） |
| controller 传参 | **显式 `g_ftl_ptrs` 三维指针表**（`ftl_context_t *[CH][CE][LUN]`） | C 数组退化规则：static 数组名退化为「指向 [CE][LUN] 子数组的指针」，与形参「指向指针的 [CE][LUN] 数组的指针」类型不匹配，GCC 10.3 起恒报 `-Wincompatible-pointer-types`（gcc/arm 双编译器复现）。指针表在 boot 后填 `&g_ftl[...]`，类型精确匹配 |

### SRAM 核算（已验证，debug 实测）

| 对象 | 尺寸 | 落点 | 区域空余 |
|------|------|------|---------|
| `g_ftl`（含 l2p cache 128.5KB + journal/md ~3KB） | 131.5KB | AXI_RAM | 384-16.7=367KB |
| `g_blocks` BST | 224KB | AXI_RAM（连续） | 367-131.5-224=11.5KB 剩 |
| `g_free_blocks[4096]` uint16 | 8KB | SRAM2 | 128KB |
| `g_closed_sb_list[1024]` super_block_t | 16KB | SRAM2 | 128-24=104KB 剩 |
| 合计 | ~379.5KB | AXI 355.5 + SRAM2 24 | ✓ 不溢出 |

**链接实测**（`arm-none-eabi-nm` debug.elf）：`.sram2_data` = 20480B（= 8KB free + 16KB closed，0x30020000 起）；`.bss` = 383348B（AXI_RAM，g_ftl + g_blocks + 既有 16.7KB，末址 0x2405d9e8 < 0x24060000）✓ 不溢出。

### 实现（src/main.c，唯一改动文件）

1. **include 区**新增：controller.h / ftl.h / allocator.h / bst.h / power_failure_recovery.h。
2. **声明区**：`g_ftl[CH][CE][LUN]`（AXI_RAM）、`g_blocks[CH][CE][LUN][FTL_BLOCK_TOTAL]`（AXI_RAM 连续）、`g_free_blocks[...]` + `g_closed_sb_list[...]`（`.sram2_data` NOLOAD）、`g_ctrl`、`g_ftl_ready`（主循环守卫）、`g_ftl_ptrs`（三维指针路由表）。
3. **`ssd_ftl_boot(ch,ce,lun)`**：构造 `ftl_init_params_t`（topo={ch,ce,lun}、blocks/free_blocks/closed_sb_list 指向多维子数组、free_count=0）→ `ftl_init`（内部含 bbt_init 标坏块）→ **遍历 BST 非坏块 `allocator_free_block` 回填 free pool** → `ftl_boot_recovery` → 日志 `[FTL] instance ... free blocks=N` + `[FTL] boot type=...`。
4. **`ssd_ftl_boot_all()`**：全实例三重循环 boot → 填 `g_ftl_ptrs` → `ssd_controller_init(&g_ctrl, g_ftl_ptrs)` → 成功置 `g_ftl_ready` + 日志 `[FTL] controller ready, lba_max=...`；任一失败降级（不置 ready，主循环退回裸 fc_service）。
5. **main()**：nand_init 成功分支末尾调 `ssd_ftl_boot_all()`；主循环 `if (g_ftl_ready) ftl_service(&g_ftl[0][0][0]); else fc_service();`。

### 验证

- make debug / release / tests 均**零新警告**（tests 仅剩余既有 controller/host `#warning` 占位注释；release `-s` 剥离符号故 nm 汇总显示 0，debug 实测 `.sram2_data`=20KB、AXI .bss 末 0x2405d9e8 不溢出）
- 上板 UART 预期：`[FTL] instance ch=0 ce=0 lun=0 free blocks=N`（N≈4096−坏块）、`[FTL] boot type=0`（FRESH）、`[FTL] controller ready, lba_max=1048576`、主循环跑 ftl_service 不挂、首次写 allocator_alloc 成功

### 待确认/遗留

- 掉电重建 closed_sb_list——v0.32 二期遗留

## v0.38 — F1/F3/F5 闭环（time_us 钩子 + predictive 搬移 + Multi-Plane 双页簿记）

**日期**：2026-10-02

### 背景

FTL 层三个已知缺口在本版本闭环（对应「FTL 层未实现清单」F1/F3/F5）：
- **F1**：T3 定时触发从未生效（`checkpoint_check_trigger(ftl,0U)` 恒传 0、`gc_check_trigger(...,500U)` 硬编码、`cp.timestamp` 恒 0）
- **F3**：`rr_check_predictive` 无调用方 → CRITICAL/DYING 块不主动搬移
- **F5**：Multi-Plane 写（pages=2）只簿记 plane0，plane1 是"幽灵页"，且探索确认根因更深——**allocator 独立选异号块**导致 plane1 实际烧录到错误物理块

### 关键决策

| 项 | 决策 | 理由 |
|----|------|------|
| F1 时间源 | **平台抽象 `ssd_platform_ops_t.time_us`**（新增 `stm32_platform_time_us` = HAL_GetTick×1000，main.c boot 前注册） | FTL 核心不直接依赖 H743 平台实现（架构边界）；register 强制 time_us 非 NULL；us→ms `/1000` 换算防 10s 阈值变 10ms |
| F1 计时字段 | `ftl_now_ms()` helper（未注册返 0 → T3 恒不触发，行为等同接线前）；`cp_timer_start`/`gc_timer_start` 触发后回写 | 复用已有 trigger 签名（elapsed_ms 参数本就在），只填真实值 |
| F3 挂载 | **ftl_service_idle 巡检 + 1s 节流**（`RR_SCAN_INTERVAL_MS`），一次只搬一个 SB | 行业后台周期性巡检；read disturb 是慢变量，避免频繁全扫 4096 块 |
| F3 搬移粒度 | **整 Super Block**（`wl_relocate` 复用）+ 双 plane `rr_retire_block` 退役 | 行业 Predictive Failures：有效页整体搬走 + 慢死块退役（RESERVED，不再分配新数据），**不标坏**（BBT 不删、仍可读） |
| F3 归属 | `wl_predictive_relocate` **并入 wear_leveling.c**（删独立 predictive.c） | 搬移执行本来就归 WL，predictive 只是新触发源；RR 模块只留 scan/retire（健康判定） |
| F5 核心 | **allocator 同号双 plane**：`allocator_select_block` 按 block 号配对（双 plane 必须 FREE 且非坏，寿命取平均 EC），`allocator_alloc` 从 pool 移除两 gblock | 行业 super block = 每 plane 取同编号 block（NAND 行地址 plane 只差 BA6 位，block 必须同号）；修复 plane1 烧错块根因 |
| F5 簿记 | completion 对 plane1 页对称簿记（set_valid/program_count/L2P `io.lba+1`/Journal/覆盖清旧页）；pending 加 `old_pba1` | plane1 不再无映射幽灵页 → GC/WL 搬移不丢数据，运行/掉电恢复路径一致 |
| F5 语义 | pages=2 = `io.lba` 与 `io.lba+1` 两个连续 LBA 分别映射 plane0/plane1（**与奇偶无关**） | 放宽旧"必须偶数"假设；controller 拆分已产生连续 LBA 对，无需改 |

### 实现

1. **F1**：`src/platform/stm32h743/platform_ops.c/h`（新增）+ `src/main.c`（注册）+ `src/ftl/ftl.c`（`ftl_now_ms` + 三处 trigger elapsed + 回写）+ `src/ftl/checkpoint.c`（`cp.timestamp` 填真值）。
2. **F3**：`src/ftl/read_recovery.c/h`（`rr_predictive_scan` 只扫 Closed health≥CRITICAL + `rr_retire_block` 置 RESERVED/健康复位）+ `src/ftl/wear_leveling.c/h`（`wl_predictive_relocate`）+ `src/ftl/ftl.c`（idle 巡检 1s 节流）。
3. **F5**：`src/ftl/allocator.c`（`allocator_pool_remove` 新增 + `allocator_select_block` 同号配对 + `allocator_alloc` 双 gblock 移除 + `plane_blocks[0]=plane_blocks[1]`）+ `src/ftl/pending.c/h`（`old_pba1`）+ `src/ftl/ftl.c`（submit 读 `old_pba1` + completion 双页簿记）+ `include/ftl/ftl.h`（pages=2 语义注释）。

### 验证

- make debug / release / tests 均**零错误零新增警告**（仅剩余既有 usart.c:321 死代码警告 + controller/host `#warning` 桩）
- F5 关键正确性：allocator 同号配对（坏块单 plane 整对跳过）、completion 双页簿记对称性、读路径无需改（同号后 desc 派生自动正确）
- 上板验证（F1/F8/F9 前置，待 FTL 完工统一做）：T3 定时 checkpoint 10s 触发、GC 定时 500ms、pages=2 写读回一致、GC 搬移后 plane1 数据存活、predictive 巡检触发

## v0.39 — F7 写失败 Super Block 关闭（行业坏块替换流程）

**日期**：2026-10-02

### 背景

F7（ftl_revision_history F 清单）：写失败路径原只 `bbt_mark_bad` 标坏块，但 **Super Block 仍保持 Open 继续写**——坏块还挂在 OPEN SB 里，与 allocator 状态矛盾（bbt_mark_bad 清 valid + 摘块，而 SB next_page 还推着写）。设计文档 §3.8「运行时坏块替换流程」已定义行业做法但未实现。

### 关键决策

| 项 | 决策 | 理由 |
|----|------|------|
| 写失败处理 | **① 关闭当前 Open SB（不再写入）→ 进 closed_sb_list → GC 优先回收（VPC 低）；② bbt_mark_bad 标 block 号；③ L2P 不动自动回滚 old_pba** | 行业流程 §3.8：坏 SB 立即退役不再写，GC 回收时好 plane 擦除回 free pool、坏 plane 擦失败再标坏 |
| 标坏粒度 | 保留 `bbt_mark_bad`（标 block 号） | 同号双 plane 下两 plane 一起退役（同号语义固有粒度：一个 block 号横跨两 plane，物理上常同坏） |
| 空洞页 | 不置 valid（现状保留），GC 搬移跳过 | L2P 已回滚，读不会读空洞页 |

### 实现

1. **`include/ftl/allocator.h` + `src/ftl/allocator.c`**：新增 `allocator_close_open_sb(alloc, temperature)` —— 运行时关闭指定温度的 Open SB（复用内部 `allocator_close_sb`：置 CLOSED + 进 closed_sb_list + valid=0）。
2. **`src/ftl/ftl.c`**：写失败分支（completion handler）—— 调 `allocator_close_open_sb(FTL_TEMP_HOT)` 关闭坏 SB，然后 `bbt_mark_bad` 标坏，L2P 不动自动回滚。

### 验证

- make debug / release / tests 均**零错误零新增警告**（仅剩余既有 usart.c:321 + controller/host `#warning` 桩）
- 逻辑闭环：写失败 → 坏 SB 关闭进 closed → GC 选 victim（VPC 低）→ gc_erase_and_free 擦双 plane → 好 plane 回 free pool、坏 plane 擦失败标坏
- 上板验证（待 FTL 完工统一做）：注入 PROGRAM 失败 → 观察 SB 关闭 + GC 回收 + 好 plane 复用

- pending/completion 全局单例多实例化——LUN_NUM>1 启用时必修
- controller 传参的 `g_ftl_ptrs` 显式指针表（避免 -Wincompatible-pointer-types）；若后续 controller 头改收数组退化形式（`ftl_context_t (*ftl)[CE][LUN]`）可去掉
- write 路径 pending 完成依赖 completion_table_drain 泵（非同步释放）——pending 槽位只在完成回调释放，submit 失败路径已配对 remove；无显式泄漏（UART 层尚未接 pending 通知，上板前关注）

## v0.40 — F2 掉电重建 closed_sb_list（全 NAND 扫描）

**日期**：2026-10-02

### 背景

F2（ftl_revision_history F 清单）：`closed_sb_list` 是纯 RAM 态（SRAM2 NOLOAD），掉电即失；`allocator_init` 只清 `closed_sb_count`，不从 NAND 重建。掉电恢复 `ftl_boot_power_failure` 只扫 2 个 Open SB 写指针（且被 `hot_sb.valid!=0` 门控恒假 → 实际没扫），closed 列表完全没碰；BST 里已满块 state 停在 `allocator_init` 统一置的 FREE。结果：掉电重启后已满 SB 全部消失 → GC/WL/rr_predictive_scan 看不到 → 容量泄漏（不丢数据但衰减）；且这些块还被 free pool 回填，可能复用未擦除半满块。

### 关键决策

| 项 | 决策 | 理由 |
|----|------|------|
| 重建时机 | **开机 recovery 阶段同步全扫**（Clean/Power Failure 两分支都重建） | fresh 全 NAND 未写白扫；clean 掉电前 closed 块同样在 free pool，同一根因；同步扫逻辑集中（用户确认） |
| 判定写过 | `sync_read_page_raw`（LDPC_RAW）+ 全 0xFF 检测（逐 plane） | FC 层无独立 OOB 接口，全库统一判据 |
| 判定有效 | `gc_read_page_with_lba` 取 LBA（META CW0）→ `l2p_cache_read` 反查 PBA==本页才置 valid | 覆盖/torn 页不置位 → GC victim 判据不虚高，迁移不搬幽灵页 |
| 部分写块 | 前 2 对恢复 Open SB（Hot/Cold），多余强制 Closed（GC 迁移自动清 torn） | 替代原被 valid!=0 门控恒假的 scan_write_pointer；torn 页靠 GC L2P 反查丢弃（不丢已确认写） |
| 固定元数据块排除 | `ftl_is_metadata_block` 统一谓词（固定区间 + md_dir 动态 region），rebuild 与 free pool 回填共用 | Journal/L2P 可写满 64 页，逐页判空会误判为满 SB；main.c 回填误纳元数据块问题同链修复（用户确认） |
| erase_count | F2 范围外不修（无持久化表） | WL 开机初期 max_ec-min_ec=0 退化为全平局，GC VPC 判据不受影响，写进文档 |

### 实现

1. **`include/ftl/metadata.h` + `src/ftl/metadata.c`**：新增 `ftl_is_metadata_block(md_dir, block)` —— 固定块区间（L2P 0..L2P_BLOCKS_PER_LUN-1 / Anchor 33/34+stride / BBT 36/39 / Checkpoint 37/38 / Journal 40/41）+ md_dir 动态 L2P region 块（扫 entries）。
2. **`include/ftl/allocator.h` + `src/ftl/allocator.c`**：新增 `allocator_add_closed_sb(alloc, plane_blocks, temperature)`（仿 close_sb 后半段：置 CLOSED/清 FREE/温度/sb_id + 追加列表，valid=0）与 `allocator_remove_from_free(alloc, gblock)`（对 static allocator_pool_remove 的公共薄包装）。
3. **`src/ftl/power_failure_recovery.c`**：新增 `ftl_rebuild_closed_sb_list(ftl, stats)` + 3 个 static helper（`page_is_blank` / `scan_block_write_pointer` / `recover_open_sb` / `close_full_block`）；`ftl_boot_clean_start` 与 `ftl_boot_power_failure` 各加调用；顺带修 `ftl_boot_scan_write_pointer` 遗留的 buf[16] 传给 2048B 拷贝的栈溢出（新代码统一 `uint8_t raw_buf[PAGE_SIZE]`）。
4. **`src/main.c`**：free pool 回填加 `ftl_is_metadata_block` 谓词排除（时序安全：回填在前、boot_recovery 重建在后，中间无 allocator_alloc）。
5. **`docs/design/ftl_revision_history.md`**：v0.40 条目 + F 清单 F2 标"✅ v0.40 已实现"。

### 验证

- make debug / release / tests 均**零错误零新增警告**（仅剩余既有 usart.c:321 死代码 + controller/host `#warning` 桩）
- 上板验证（待 FTL 完工统一做）：写若干 SB（满/部分/覆盖留空洞）→ 断电重启 → 断言 closed_sb_count/valid_count、`gc_select_victim` 可选中满 SB、`gc_migrate_sb` 后 L2P 命中页全迁移、满块经 `gc_erase_and_free` 后 free pool 归还

- **已知代价**：Clean Start 重建的 L2P 反查触发按需 region 加载（cache 空，从 NAND 读 L2P 表），开机比 Power Failure 分支慢；另 erase_count 无持久化表，开机后 WL 短暂退化为全平局。

## v0.41 — F4 checkpoint 增量（region_bitmap）

**日期**：2026-10-02

### 背景

F4（ftl_revision_history F 清单）：checkpoint 目前全量写 6 页 header（`region_bitmap` 恒全 1、`region_count` 恒全量），恢复侧完全不消费 bitmap。目标是把 checkpoint 改造成行业标准语义：**按 region 脏标记填 bitmap + 恢复侧按 bitmap 局部加载**。

关键事实（设计确认）：checkpoint 的「6 页」是 header 冗余（写 2048B header 到 Block 37/38 各 3 页），**不是 L2P 全表**；单 die 拓扑（LUN_NUM=1）下增量**没有 NAND 页数收益**（脏区数据本来就都要写，每区最多一页；evict 兜底保证任何脏区最终落 NAND）。用户确认**仍完整实现**——这是行业标准格式语义 + 未来多 LUN 的前置（多 die 时 checkpoint 不再覆盖全部芯片，bitmap 才有真正的增量收益）。

### 关键决策

| 项 | 决策 | 理由 |
|----|------|------|
| 脏区追踪 | `l2p_cache_t` 加 `region_dirty_bitmap[L2P_REGIONS_PER_LUN/32]`（256B SRAM），`l2p_cache_write` 置位、`l2p_cache_flush_one` 成功清零 | 与 line 级 state 互补：evict 兜底 flush 后行 CLEAN/失效，但"本次周期内被改过"由 bitmap 记住；bit=1 ⟺ 该区 NAND 页可能有未落盘数据 |
| 快照先于 flush | `checkpoint_create` ①先 `memcpy` 快照 bitmap → ②按 bitmap 只 flush 脏区 → ③填 header → ④写 6 页 → ⑤anchor → ⑥truncate | flush 内 `flush_one` 会清 bit，先 flush 后快照则 bitmap 恒空、header 声称无区当前（功能为空）；truncate 必须最后（其擦对方块依赖 ≤cp_seq 已落表） |
| flush 失败中止 | `checkpoint_flush_dirty_regions` 任一失败 → `checkpoint_create` 立即 `return FTL_ERROR`，不写 header/不 flush anchor/不截断 journal | 失败区 NAND 页仍旧、bit 保持置位；若继续写 header 声称已落盘 → boot 按 bitmap 预载旧页 = 数据一致性破坏。不再忽略 flush 返回值（原 182 行 `l2p_cache_flush_all` 返回值被 `(void)` 吞掉，改后必须消费） |
| evict 失败保留行 | `l2p_cache_evict_one`：DIRTY 行 flush 失败 → **不复用该行**（视同 FLUSHING 跳过继续找；找不到返回 FTL_ERROR） | 原实现 flush 失败仍置 INVALID 复用 → RAM 脏数据丢弃 + NAND 页仍旧 + **bit 置位但无 resident DIRTY 行（孤儿 bit）** → checkpoint 按 bitmap 假声明 → boot 必坏读。F4 前置修复 |
| 恢复按 bitmap 局部加载 | `ftl_boot_power_failure` 签名追加 `const l2p_checkpoint_header_t *cp`；`journal_replay` 后新增 `ftl_boot_apply_region_bitmap`：DIRTY（回放）行跳过；bitmap 区显式读页置 CLEAN；非 bitmap 区置 INVALID（懒加载 CP 时刻快照） | 回放行若 CLEAN 声明"cache==NAND"是假（NAND 是 CP 时刻旧值）、INVALID 则直接丢回放数据；INVALID 行直接置 CLEAN 的 entries 仍是 init 全 0xFF，read 对有效映射返回假 INVALID。强制顺序必须在 replay 之后（init 后所有行 region_id=0，先强制误匹配全部 64 行）。`ftl_boot_clean_start` 不变（天然安全：全 INVALID + 懒加载） |
| 诚实标注 | 单 die 下选择性 flush 与 `flush_all` **覆盖完全相同**（都是所有 resident DIRTY 行）；`ftl_service_idle` 每 tick 的 `l2p_cache_flush_all`（ftl.c:303）使 IDLE 模式 CP 触发时 bitmap 近空 | **本改造不减少写页数**；真实收益只在多 die/multi-LUN 时 CP 不再覆盖全部芯片才出现。格式/语义前移是主要目的 |

### 实现

1. **`include/ftl/l2p_cache.h` + `src/ftl/l2p_cache.c`**：`l2p_cache_t` 加 `region_dirty_bitmap[L2P_REGIONS_PER_LUN/32]`；`l2p_cache_init` memset 0；`l2p_cache_write` 置位；`l2p_cache_flush_one` 成功路径清位（失败保持）；`l2p_cache_evict_one` 失败保留行（见决策）。
2. **`src/ftl/checkpoint.c`**：新增 static `checkpoint_flush_dirty_regions`（只 flush state==DIRTY && bit 置位的行）；`checkpoint_create` 改「快照 bitmap → 选择性 flush（失败中止）→ header（region_count=置位 word 数，无消费方仅信息量）→ 6 页 → anchor → truncate」；`region_bitmap` 从 cache 快照填入。
3. **`src/ftl/power_failure_recovery.c`**：`ftl_boot_power_failure` 签名 `(ftl, cp, result)`（调用点 236 行同步改）；新增 static `ftl_boot_apply_region_bitmap`（跳过 DIRTY 回放行 / bitmap 区显式读页 CLEAN / 非 bitmap 区 INVALID）；`cp_sequence` 改取 `cp->journal_sequence`。
4. **`include/ftl/power_failure_recovery.h`**：签名 + include checkpoint.h。
5. **测试**：`tests/unit/test_f4_region_bitmap.c`（5 例，纯 RAM + FC 桩：bit 生命周期、evict 失败保留行、CP 中止）+ `tests/framework/munit_stub_fc.c/.h`（PC 本机 FC 桩 + `fc_stub_fail_next` 失败注入）+ `tests/unit/f4_stub_fail.c`（上板同名符号）+ `tests/Makefile` PC 本机 target + `src/fc/makefile.mk`/`ut_main.c` 上板注册。（v0.44 起：PC 桩/本机 target 与 F4 测试一并移除，测试统一走 `make tests` 上板。）
6. **`docs/design/ftl_revision_history.md`**：v0.41 条目 + F 清单 F4 标"✅ v0.41 已实现"。

### 验证

- make debug / release / tests 均**零错误零新增警告**（仅剩余既有 usart.c:321 死代码 + controller/host `#warning` 桩）
- PC 本机纯 RAM 验证 `make -C tests f4`：5/5 全过（v0.44 起移除）
- 上板验证（待 FTL 完工统一做）：连续写 → checkpoint → 断电重启 → 断言恢复读正确（bitmap 区预载 CLEAN 与懒加载无差别）、flush 失败注入路径（临时改 fc 桩）CP 中止不丢数据

## v0.42 — F6 pending/completion 多实例化（per-instance pending + 全局 completion 路由）

**日期**：2026-10-02

### 背景

F6（ftl_revision_history F 清单）：`g_pending[32]`（src/ftl/pending.c）与 `g_table[32]`（src/ftl/completion_table.c）是全局单例，`ftl_init` 每次实例化都 `completion_table_init()` + `pending_init()` 把整表清零——**LUN_NUM>1 时第二个实例的 init 会把第一个实例的 pending 全部冲掉**。P-A 多 LUN 并发（LUN_NUM=2 已启用）的软件前置。

关键架构事实（v0.42 设计确认）：
- FC 层全局单一：`fc_service()` / `fc_submit()` / `fc_poll_completion()` / `fc_command_id_to_command()` 都操作全局 `g_fc_controller`；command_id 全局唯一（FC command pool 槽号）。
- **单线程顺序主循环下"实例 A 的 drain 遇到实例 B 的 completion"是必然事件**（A#cmd1 提交后 B#cmd2 异步在途，下一轮 A drain 时两者都在 CQ）。

### 关键决策

| 项 | 决策 | 理由 |
|----|------|------|
| pending 表 | **per-instance**：`g_pending[32]` 移入 `ftl_context_t`（`ftl->pending[32]`），API 改 `(ftl, ...)` 指针风格（仿 l2p_cache_t） | 实例独立 32 槽背压；`ftl_pending_req_t.ftl` 字段删除（实例即上下文）；与既有 cache/alloc/journal 指针模式一致 |
| completion 表 | **保持全局**（不迁入 ftl_context_t） | FC completion 全局单一 CQ + cmd_id 全局唯一；per-instance 表会让 B 实例的 completion 走 A 的 default handler → `pending_lookup(A, cmd2)=NULL` → 读不回填 buffer、写不确认 Journal/L2P → **数据丢失**。路由必须依赖 completion 自身内容（cmd_id → command → ch/ce/lun → 实例）而非"谁在 drain" |
| 实例路由 | 新增 `g_route[CH_NUM][CE_NUM][LUN_NUM]`（ftl_context_t* 表）：default handler 里 `cmd_id → fc_command_id_to_command → cmd->config.ch/ce/lun → g_route → 实例 → pending_lookup(ftl, cmd_id)`；`completion_table_set_route`（main.c boot 后调用一次）+ `completion_table_route`（越界 NULL） | 两段无缝衔接：全局表 cmd_id 精确命中（sync waiter）→ 未命中走全局 default → route 到正确实例的 pending |
| `ftl_internal_sync_wait` | **签名不变，21 处调用点零改动** | waiter 注册在全局表；等待期间他实例的 completion 由全局 default handler 正确分发到各自实例，不互吞 |
| `g_rr_last_scan_ms` | 移入 `ftl_context_t`（`ftl->rr_last_scan_ms`） | 与 F6 同源（全局单例），一并收干净；慢死巡检节流变 per-instance |
| main.c | 一处：`ssd_ftl_boot_all` 填完 `g_ftl_ptrs` 后调 `completion_table_set_route(g_ftl_ptrs)` | `g_ftl[][][]` 已是实例数组；route 表全局，boot 后一次设置覆盖全部 |

**调用约定（正确性前提）**：提交实例 = 命令 ch/ce/lun 来源实例 = route 解析实例。由 ftl_submit_read/write 用本实例 topo 构造命令（cmd->config.ch/ce/lun 来自 allocator 分配/本实例 topo）保证。

### 实现

1. **`include/ftl/pending.h` + `src/ftl/pending.c`**：全部 API 加 `ftl_context_t *ftl` 首参（`pending_init/add/lookup/lookup_by_lba/remove`）；删全局 `g_pending`，操作 `ftl->pending`；`ftl_pending_req_t` 删 `ftl` 字段、`pending_add` 删尾参。
2. **`include/ftl/completion_table.h` + `src/ftl/completion_table.c`**：`g_table/g_default_handler/g_default_ctx` 保持全局；新增 `completion_table_set_route(ftl_context_t *route[][CE_NUM][LUN_NUM])` + `completion_table_route(ch, ce, lun)`；`completion_table_init` 一并清 route。
3. **`include/ftl/ftl.h`**：`ftl_context_t` 加 `pending[FTL_PENDING_MAX]` + `rr_last_scan_ms`（不 include completion_table.h，无环）。
4. **`src/ftl/ftl.c`**：删全局 `g_rr_last_scan_ms`（22 行）；`ftl_handle_completion` FTL case 内 `completion_table_route(cmd->config.ch/ce/lun)`（位域显式 (uint8_t) 转换），NULL → release+break；全部 `pending->ftl->X` → `ftl->X`；`ftl_init` 改 `pending_init(ftl)` + `ftl->rr_last_scan_ms=0` + set_default 注释注明 ctx 仅 fallback；submit_read/write 的 `pending_add(ftl,...)/pending_remove(ftl,...)`。
5. **`src/main.c`**：include completion_table.h；`ssd_ftl_boot_all` 填完 `g_ftl_ptrs` 后 `completion_table_set_route(g_ftl_ptrs)`（boot 后、controller init 前）。
6. **测试**：`tests/unit/test_f6_pending.c`（4 例纯 RAM：实例隔离 / init 只清本实例 / 满 32 背压互不挤占 / route 越界 NULL）+ PC 本机 target `make -C tests f6`（链 pending.c + completion_table.c + munit + munit_stub_fc，桩补 `fc_poll_completion`=QUEUE_EMPTY + `fc_command_id_to_command`=NULL）+ 上板 `src/fc/makefile.mk`/`ut_main.c` 注册 `/f6/` 套件。（v0.44 起：PC 桩/本机 target 与 F6 测试一并移除，测试统一走 `make tests` 上板。）
7. **`docs/design/ftl_revision_history.md`**：v0.42 条目 + F 清单 F6 标"✅ v0.42 已实现"。

### 验证

- make debug / release / tests 均**零错误零新增警告**（仅剩余既有 usart.c:321 死代码 + controller/host `#warning` 桩）
- PC 本机纯 RAM 验证 `make -C tests f4`（5/5）+ `f6`（4/4）全过（v0.44 起移除）
- grep 复核：`pending_lookup/add/remove/init` 调用点全部带 ftl 首参；`g_pending`/`g_rr_last_scan_ms`/`pending->ftl` 归零；`ftl_internal_sync_wait` 21 处调用点无改动
- 上板验证（待 FTL 完工统一做）：LUN_NUM=2 双实例提交 → 各自 completion 正确路由（读回填各自 buffer、写确认各自 Journal）

### FTL 层未实现清单（后续实现参考）

> 本清单汇总「FTL 层边界内」的真实缺口（与上文遗留对应，按优先级排序）。
> 非 FTL 层（NVMe/controller 调度/UART CLI/平台 BSP 计时）见 ftl_design §16「未实现」。

| # | 项 | 现状 | 影响 | 工作量 | 入口/依赖 |
|---|----|------|------|--------|-----------|
| F1 | **CP/GC 定时触发接线 + 平台 time_us 钩子** | ✅ **v0.38 已实现** — `stm32_platform_time_us`（HAL_GetTick×1000）注册 `ssd_platform_ops_t`；ftl.c `ftl_now_ms()` 计算真实 elapsed；`cp_timer_start`/`gc_timer_start` 触发后回写；`cp.timestamp` 填真实时间戳 | T3 定时 checkpoint（10s）/GC 定时（500ms）首次真正生效 | 小（半天内） | 见 v0.38 条目 |
| F2 | **掉电重建 closed_sb_list** | ✅ **v0.40 已实现** — `ftl_rebuild_closed_sb_list`（power_failure_recovery）：全 NAND 扫描，逐 plane LDPC_RAW 判空 + `gc_read_page_with_lba` 取 LBA + `l2p_cache_read` 反查回填 valid_map；满块进 closed_sb_list（`allocator_add_closed_sb`）、前 2 部分写对恢复 Open、其余强制 Closed；free pool 拉出写过块；clean/PF 两分支都接线 | 掉电重启后已满 SB 正确重建 → GC 回收 + 不丢数据；顺带修复 free pool 回填误纳元数据块 | 大（独立立项） | 见 v0.40 条目 |
| F3 | **predictive 搬移（read disturb / 慢死）** | ✅ **v0.38 已实现** — `wl_predictive_relocate`（并入 wear_leveling）：`rr_predictive_scan` 扫 Closed 块 health≥CRITICAL → sb_id 反查 SB → `wl_relocate` 整 SB 搬移 → 双 plane `rr_retire_block` 退役（RESERVED，不标坏）；ftl_service_idle 带 1s 节流接入 | CRITICAL/DYING 块主动搬移 + 退役，不再靠读到坏兜底 | 中 | 见 v0.38 条目 |
| F4 | **checkpoint 增量（region_bitmap）** | ✅ **v0.41 已实现** — 脏区追踪（`region_dirty_bitmap` 置位/成功清零）+ `checkpoint_create` 快照先于 flush + 选择性 flush 失败中止 + 恢复侧 `ftl_boot_apply_region_bitmap` 按 bitmap 局部加载（跳过回放行）；顺带修 evict flush 失败丢脏数据（孤儿 bit）。**诚实标注：单 die 拓扑下与全量写页数相同，真实收益在 multi-LUN** | 行业标准 bitmap 语义 + 多 LUN 前置（CP 不再覆盖全部芯片时才有增量页数收益） | 大 | 见 v0.41 条目 |
| F5 | **写 Multi-Plane 真双页（VPC/空洞侧）** | ✅ **v0.38 已实现** — allocator 同号双 plane（industry super block）+ completion 对 plane1 页对称簿记（set_valid/L2P io.lba+1/Journal/清旧页）；pending 加 `old_pba1` | 跨 plane 写 VPC/映射/GC 搬移完整闭环，修复"plane1 烧错块"根因 | 中 | 见 v0.38 条目 |
| F6 | **pending/completion 单例多实例化** | ✅ **v0.42 已实现** — pending 表 per-instance（`ftl->pending[32]`，API 加 ftl 首参，删 pending 项内 ftl 字段）；completion 表保持全局（FC CQ 全局 + cmd_id 唯一，per-instance 会让异实例 completion 走错 default handler → 数据丢失），新增 `g_route[CH][CE][LUN]` 按 cmd->config.ch/ce/lun 路由到实例；`g_rr_last_scan_ms` 一并移入 ftl_context；`ftl_internal_sync_wait` 签名不变（21 处调用点零改动） | LUN_NUM>1 多 FTL 实例安全共存（第二个实例 init 不再冲掉第一个的 pending）；多 LUN 并发前置 | 中 | 见 v0.42 条目 |
| F7 | **写失败 Super Block 空洞的显式回滚** | ✅ **v0.39 已实现** — `allocator_close_open_sb` 运行时关闭坏 SB（行业流程 §3.8 ①）+ `bbt_mark_bad` 标坏 block 号 + L2P 自动回滚；坏 SB 进 closed → GC 优先回收 | 修复"坏块还挂在 OPEN SB 里继续写"的状态矛盾；好 plane 经 GC 擦除回 free pool | 低 | 见 v0.39 条目 |
| F8 | **journal 空间不足的紧急 checkpoint 回切校验** | `journal_truncate` 简化实现（擦除非当前 block）；`checkpoint_is_emergency` 有但触发面未验证 | Journal 满时依赖 checkpoint 水位线，简化截断在极端写模式下的正确性未测 | 中（测试为主） | 上板高压写验证；`journal_flush_page` 满页回切逻辑已实现 |
| F10 | **journal_replay 只回放最新一页（掉电 ≥2 页条目丢失）** | ✅ **v0.43 已实现** — `journal_replay` 签名加 `cp_sequence`，两遍扫描：收集全部 `page_seq > cp_sequence` 页 → 按 `(page_seq, entry_index)` 递增序应用（per-LBA last-write-wins，同 LBA 旧 GC/WL 搬移条目被新写覆盖）；**顺带闭合 GC/WL 擦除先于 journal flush 的掉电损坏窗口**（Explore 调研发现：journal write-back 未 flush 就擦 victim，掉电回放指已擦除块 → `gc_migrate_sb`/`wl_relocate` 擦除前强制 `journal_flush_page`）；PC 本机单测 `/f10/` 5 例（多页回放/CP 过滤/last-write-wins/排序防御/GC flush 契约，内存 NAND 桩，v0.44 起随 PC 桩一并移除） | 掉电恢复丢写（post-CP 条目超过 128 条时）+ 回放指向已擦除块 | 中 | 见 v0.43 条目 |
| F9 | **CQ/SQ 深度与多实例并发实测** | FC 池深度（64/256/128/64/16/64）按单实例预算 | 多 LUN/多实例并发 + 后台（GC/CP/BBT）同时争池未实测 | 中（测试） | 上板多实例后观察；文档 §17 背压已分析 |

**上板验证（v0.37）是 F1/F8/F9 的前置**：FTL 首次真跑，UART 日志 + 高压写会暴露 allocator/pending/GC 的真实行为。

## v0.43 — F10 journal_replay 全量回放 + GC/WL 擦除前强制 flush

**日期**：2026-10-02

### 背景

F10（F 清单，v0.41 F4 review 发现）：`journal_replay` 只回放 `page_seq` 最大那一页的 128 条 entry。而 checkpoint 窗口内 journal 条目量可达 **~13,900 条 ≈ 109 页**（CP 触发：每 1024 次写 / journal 85% 水位 ≈ 13,900 条 / 10s 定时，checkpoint.c:345-374）。掉电时 post-CP 条目跨 ≥2 页 → 只恢复最新 128 条，其余**静默丢失**。

Explore 调研另发现一个超出 F10 原描述的**真数据损坏窗口**：journal 是 write-back（RAM `page_buf` 攒 128 条才 `journal_flush_page`）；GC/WL 搬移 append 条目到 RAM 缓冲后**同步擦除 victim 块**，两者间无任何 flush。掉电落在窗口内 → NAND 上只有旧条目 `X→A`（指向已被擦除块），新条目 `X→B` 从未落盘 → 恢复回放后 L2P 指向已擦除块 = 数据损坏。此窗口 F10 全量回放不修复（新条目根本不在 NAND 上），必须一并闭合。

### 关键决策

| 项 | 决策 | 理由 |
|----|------|------|
| `journal_replay` 签名 | 加 `cp_sequence`（`cp->journal_sequence`），由 `ftl_boot_power_failure` 传入 | v0.41 已把 `cp` 传进恢复侧，前置条件已具备；过滤 `page_seq > cp_sequence` 的页（≤ 的条目已在 checkpoint 时 flush 进 NAND L2P 表） |
| 回放顺序 | 两遍扫描：收集全部需回放页 → 按 `(page_seq, entry_index)` 插入排序后逐页应用 | per-LBA last-write-wins——同 LBA 的旧 GC/WL 搬移条目被新写覆盖，不指向已擦除块；双块 page_seq 区间不交叠（truncate 擦整非当前块），显式排序防御未来 truncate 变化 |
| GC/WL 擦除前 flush | `gc_migrate_sb` / `wl_relocate` 在 `gc_erase_and_free` 前调 `journal_flush_page` | 闭合"搬移条目未落盘 + victim 已擦除"掉电窗口；`journal_flush_page` 幂等（空缓冲 no-op），后台 idle flush 不受影响 |
| 栈上页表 | `replay_pages[128]`（2 块×64 页），每项 16B → 512B | 现有栈上已有 `page_data[2048]`；不缓存 entry（逐页重读），SRAM 余量充足 |

### 实现

1. **`include/ftl/journal.h`**：`journal_replay` 声明加 `uint32_t cp_sequence` 第三参。
2. **`src/ftl/journal.c`**：`journal_replay` 重写中段——第一遍扫描收集 `page_seq > cp_sequence` 页（沿用 magic/count/上限校验），第二遍按 `(page_seq, entry_index)` 插入排序后逐页应用 entry；保留尾部写指针推进（`current_block/page/entry` + `sequence_counter`，`ftl_boot_power_failure` 依赖其估算 entry_count）。
3. **`src/ftl/power_failure_recovery.c`**：`ftl_boot_power_failure` 把 `cp_sequence` 传给 `journal_replay`。
4. **`src/ftl/gc.c` + `src/ftl/wear_leveling.c`**：搬移循环结束、擦除前 `journal_flush_page`。
5. **测试**：`tests/unit/test_f10_replay.c`（5 例，内存 NAND 桩）——多页回放（3 页全回放，旧实现只回放最大页）/ CP 过滤（`page_seq ≤ cp_sequence` 不回放）/ last-write-wins（同 LBA 旧 GC 条目被新写覆盖）/ 排序防御（跨块条目按 page_seq 序结果一致）/ GC flush 契约（擦除前 flush 后条目必落盘）+ `tests/Makefile` target `f10` + 上板 `src/fc/makefile.mk`/`ut_main.c` 注册 `/f10/` 套件。
6. **`tests/framework/munit_stub_fc.c`**：`ftl_cmd_write/read_data` 从 no-op 升级为内存 NAND 桩（静态数组按 `cmd->paa` 地址读写 + erase 清 0xFF），`journal_truncate` 改弱符号（F10 链真实 journal.c 强符号自动覆盖）。
7. **`docs/design/ftl_revision_history.md`**：v0.43 条目 + F 清单 F10 标"✅ v0.43 已实现"。

### 验证

- `make -C tests test`：f4（5/5）+ f6（4/4）+ **f10（5/5）** 全过，编译零警告（-Werror）
- `make` 默认固件零错误零新增警告
- `make FC_NAND_TEST_ENABLE=1` 上板测试固件零错误编译
- 上板验证（待 FTL 完工统一做）：高压写中随机掉电 + 重启，`/f10/` 套件回放多页/覆盖场景数据一致

### FTL 层剩余清单

> 本清单汇总「FTL 层边界内」的真实缺口（与上文遗留对应，按优先级排序）。
> 非 FTL 层（NVMe/controller 调度/UART CLI/平台 BSP 计时）见 ftl_design §16「未实现」。

| # | 项 | 现状 | 影响 | 工作量 | 入口/依赖 |
|---|----|------|------|--------|-----------|

---

## v0.44 — FTL 未落地项体检 + A 档落地（OP 10% + Spare Pool + 三级 Free Pool 状态位）

**日期**：2026-10-03

### 背景

对 FTL 层做全量体检（对照设计文档/变更历史/记忆点逐项 grep 验证），发现设计承诺但代码零消费的缺口：
- **A 档**：`FTL_OP_PERCENT(10)`/`FTL_SPARE_POOL_SIZE(50)` 只定义不消费；`spare_pool_count` 仅 init 赋 0；`bbt_spare_pool_refill` 是 `return FTL_ERROR` 桩；`FTL_BLOCK_STATE_PRE_COND/EMERGENCY` 全库从未 set/clr → 容量语义实际 = 全好块进 free pool（OP 0%）。
- **B 档**（下阶段）：journal 水位分母错、checkpoint/emergency 标志永不复位、ftl_service 模式代理、GC/WL 读不记录 ECC、read_count 只写不读。
- **C 档**（下阶段）：controller state_machine/scheduler + NVMe host 全是编译进来的 `#warning` 桩；`SSD_REQUEST_FLUSH` 返回 NOT_SUPPORTED。

### 关键决策

| 项 | 决策 | 理由 |
|----|------|------|
| OP/Spare 容量语义 | **容量不动**（512MiB 不变）：OP 10% + Spare 50 只做 GC 缓冲池 + 坏块替换池，不进用户分配路径 | 用户已确认；L2P region 数/metadata 布局/FTL_LBA_MAX 全部不动，广告容量不变 |
| Spare 池分配 | **前 50 好块入 Spare 池**（坏块替换池，只出不进，耗尽后容量自然下降） | ADR-11 Block Sparing：坏块替换专用 |
| OP 池分配 | **前 10% 好块入 OP 池**（GC 缓冲池），free pool 耗尽时回退 OP 取双 plane 块 | ADR-8：GC 缓冲 + 性能保速 |
| PRE_COND 状态位 | `allocator_free_block` 置（归还即预擦除态）、`allocator_alloc` 清（取走占用） | free pool 唯一归还入口，全块预擦除态，`pre_cond_count` 从 free_count 别名升为真实预擦除计数 |
| EMERGENCY 状态位 | 保留枚举定义，不 set（无物理 Emergency 池） | `FREE_POOL_EMERGENCY` 阈值由 `allocator_needs_gc` 的 `free_count==0` 兜底 |

### 实现

1. **`include/ftl/allocator.h`**：`ftl_init_params_t` + `ftl_allocator_t` 增 `op_blocks/spare_blocks/op_count/spare_count`；删 `spare_pool_count` 别名。
2. **`src/main.c`**：`g_op_blocks`/`g_spare_blocks` 落 SRAM2；boot 回填三分流（前 50 → Spare → 前 10% → OP → 其余 → free pool）；日志 `free/op/spare`。
3. **`src/ftl/allocator.c`**：`allocator_init` 读新 params；新增 `allocator_pull_from_op`；`allocator_alloc` free 耗尽回退 OP 取块；`allocator_free_block` 置 `PRE_COND`、`allocator_alloc` 清 `PRE_COND`。
4. **`src/ftl/bbt.c`**：`bbt_spare_pool_refill` 桩改真实现（`spare_blocks` 取块补 free pool）；`bbt_mark_bad` 后调用。

### 验证

- `make debug / release / FC_NAND_TEST_ENABLE=1` 全零错误零新增警告
- SRAM：AXI 374.7KB/384KB、SRAM2 28.1KB/128KB（新增 op+spare 数组，不溢出）
- 上板预期：`[FTL] instance ... free=N op=~400 spare=50`；PRE_COND 状态位随 alloc/free set/clr
- 上板高压写：OP 池耗尽回退分配、坏块触发 Spare 补池

### 待办（B/C 档，见 docs/design/ftl_full_landing_plan_20261003.md）

- **B 档**：B1 journal 水位分母（`PAGES_PER_BLOCK`→`JOURNAL_BLOCK_COUNT×PAGES_PER_BLOCK`）、B2 checkpoint/emergency 标志复位、B3 ftl_service 模式基于 pending 占用、B4 GC/WL 搬移读记录 ECC + read_count++、B5 read_count 消费（predictive scan/WL victim）。
- **C 档**：C1 controller state_machine 完整状态机、C1b scheduler 优先级调度、C2 NVMe host（nvme_io/admin/SQ/CQ/nvme_controller 全量）、C3 `SSD_REQUEST_FLUSH` 全实例 flush。（**已在 v0.47 落地**：C2 NVMe 按学习主控定位砍掉，改 UART CLI 驱动 FTL，见 v0.47 条目）
- **F8/F9**：依赖上板（紧急 CP 回切校验 / CQ·SQ 深度实测）。

---

## v0.45 — B 档落地（journal 水位修复 + 标志复位 + ftl_service 空闲度 + GC/WL 读记 ECC + read_count 消费）

**日期**：2026-10-03

### 背景

v0.44 体检确认的 B 档 5 项语义缺口（FTL 层正确性）全部落地：

| # | 缺口 | 修复 |
|----|------|------|
| B1 | `checkpoint_journal_watermark` 分母用 `PAGES_PER_BLOCK(64)`（单块），容量实为双块 128 页 → 85%/95% 在 page≈55/61 触发，紧急水位离写满只剩 3 页≈384 条（设计预算 768 条） | 分母改 `JOURNAL_BLOCK_COUNT×PAGES_PER_BLOCK(128)`：85%→page≈108（留 19 页）、95%→page≈122（留 6 页≈768 条），与 §7.5.2 对齐 |
| B2 | `journal.checkpoint_needed/emergency` 只有 init 置 0、append 置 1，永不复位 → T1 永久命中（每圈必 CP）+ emergency 置位后 BUSY 每圈强制 CP | `checkpoint_create` 成功末尾 + `journal_truncate` 双处 `=0` 复位（设计 §7.5.4） |
| B3 | `ftl_service` 三级模式 = `submitted==0→IDLE/≤2→NORMAL/else→BUSY`（"本轮有无提交 NAND"代理），host 无 IO 时 fc_service 也可能非 0 → 误判 NORMAL/BUSY 推迟后台 | 新增 `pending_count()`/`completion_table_count()`，模式改 `inflight==0→IDLE / pending>16→BUSY / 其余→NORMAL`（基于未决命令占用近似空闲度） |
| B4 | GC/WL 搬移读 `gc_read_page_with_lba` 不记录 ECC/read_count（`rr_record_ecc` 只在 host 读 completion）→ 搬移读 read disturb 盲区 | `gc_read_page_with_lba` 加 `ecc_out`/`read_count_out`；`gc_migrate_sb`/`wl_relocate` 成功读 → `rr_record_ecc` + `read_count++` |
| B5 | `read_count/program_count` 只写不读 | `rr_predictive_scan` 加 `read_count≥RR_READ_DISTURB_THRESHOLD(1000)` 判据；`wl_victim_t` 加 `read_count` 字段，同有效页数下 read 多冷块优先 |

### 实现

1. **`src/ftl/checkpoint.c`**（B1/B2）：`checkpoint_journal_watermark` 分母改 `(uint32_t)JOURNAL_BLOCK_COUNT * PAGES_PER_BLOCK`；`checkpoint_create` 末尾复位 `emergency/checkpoint_needed`。
2. **`src/ftl/journal.c`**（B2）：`journal_truncate` 擦除对方块后复位两标志（双保险）。
3. **`include/ftl/pending.h` + `src/ftl/pending.c`**（B3）：新增 `pending_count(ftl)`。
4. **`include/ftl/completion_table.h` + `src/ftl/completion_table.c`**（B3）：新增 `completion_table_count()`。
5. **`src/ftl/ftl.c`**（B3）：`ftl_service` Phase 3 改 `inflight==0→IDLE / pending>16→BUSY / 其余→NORMAL`。
6. **`include/ftl/gc.h` + `src/ftl/gc.c`**（B4）：`gc_read_page_with_lba` 签名加 `ecc_out`/`read_count_out`（4 CW 最大 ECC，与 host 读 completion 同口径）；`gc_migrate_sb` 搬移读后 `rr_record_ecc` + `read_count++`。
7. **`src/ftl/wear_leveling.c`**（B4/B5）：`wl_relocate` 搬移读后 `rr_record_ecc` + `read_count++`；`wl_select_victim` 加 read_count 权重（`wl_victim_t` 加字段）。
8. **`include/ftl/read_recovery.h` + `src/ftl/read_recovery.c`**（B5）：`RR_READ_DISTURB_THRESHOLD(1000)`；`rr_predictive_scan` 加 read_count 判据。
9. **`src/ftl/power_failure_recovery.c`**（B4 签名波及）：掉电重建反查传 `NULL,NULL`（重建非搬移读，不记录）。

### 验证

- `make debug / release / FC_NAND_TEST_ENABLE=1` 全零错误零新增警告
- SRAM 无变化（纯逻辑改动）
- 上板预期：journal 85%/95% 水位按双块 128 页正确触发；T1 不永久命中；emergency 复位后 BUSY 不再每圈强制 CP；高压写/搬移后 read_count 与 ECC 历史真实增长，predictive scan 能命中 read disturb 慢死块

### 待办（C 档，见 docs/design/ftl_full_landing_plan_20261003.md）→ ✅ v0.47 已落地

- C1 controller state_machine 完整状态机、C1b scheduler 优先级调度、C2 NVMe host（nvme_io/admin/SQ/CQ/nvme_controller 全量）、C3 `SSD_REQUEST_FLUSH` 全实例 flush。（落地：C1 最小状态机 / C1b 调度钩子 / C3 FLUSH / C2 砍掉改 UART CLI，见 v0.47 条目）
- **F8/F9**：依赖上板（紧急 CP 回切校验 / CQ·SQ 深度实测）。

---

## v0.46 — B 档复查修复（B1 水位分子口径 + B4 死变量清理）

**日期**：2026-10-03

### 背景

v0.45 B 档落地后逐 diff 复查，发现 2 处问题（编译验证盲区——逻辑错误 make 检不出）：

1. **B1 真 bug（分子口径错）**：v0.45 只把 `checkpoint_journal_watermark` 分母 `PAGES_PER_BLOCK(64)` 改成 `JOURNAL_BLOCK_COUNT×PAGES_PER_BLOCK(128)`，但**分子仍是 `current_page`（0..63）**。分母 128 + 分子最大 63 → `used` 最大 ≈ 49%，**85%/95% 永远不触发**（比旧实现更糟，水位线整体失效）。
2. **B4 死变量**：`gc_migrate_sb` 加了 `now_writes = 0U` 恒 0（本函数无 total_writes 入参），`rr_record_ecc` 传 0 使 `last_update` 无意义。

### 修复

1. **`src/ftl/checkpoint.c`**：`checkpoint_journal_watermark` 分子分母都按双块 128 页全局口径：
   ```
   used = (current_block×64 + current_page)×100 / (2×64) + entry 项
   ```
   分子含 `current_block×PAGES_PER_BLOCK`（双块环形全局页偏移）。85% 在全局页≈108（留 19 页）、95% 在全局页≈122（留 6 页≈768 条），与 §7.5.2 对齐。
2. **`src/ftl/gc.c`**：删 `now_writes` 死变量，`rr_record_ecc(&src->rr, ecc, 0U)` + 注释"不追踪时间维度，rising_count/health 只用 ecc_bits 本身"。

### 验证

- `make debug / release / FC_NAND_TEST_ENABLE=1` 全零错误零新增警告
- **上板验证（必要）**：`checkpoint_journal_watermark` 只在 host 读 completion 调用，编译无法验证 85%/95% 触发时序——上板高压写观察 journal 水位 / emergency 复位。这是编译验证盲区的直接教训。

### 教训

`make` 只保证编译通过，不保证逻辑正确。纯逻辑改动（水位公式/标志复位）必须**上板验证触发时序**，不能以"编译零警告"作为完成标准。

---

## v0.47 — C 档落地（最小状态机 + host IO 优先调度 + FLUSH + UART CLI 驱动 FTL；NVMe 删除）

**日期**：2026-10-03

### 背景

C 档按学习主控定位（stm32h743 + MT29F4G08，非真存储主控）重新规划（设计文档 §13.4 **D10：Host 命令来源 = UART Shell CLI**，§16 NVMe 标未实现）。NVMe 是 PCIe 协议壳、无硬件对手件，纯桩无验证价值 → **C2 砍掉**，UART CLI 作为 host 层完整驱动 FTL。

### 落地

**C1 最小状态机**（`src/controller/state_machine.c` + `include/controller/controller.h`）
- `ssd_controller_state_transition`：RESET→INITIALIZING（boot 入口）→READY/FAILED（槽位校验结果），READY→FAILED 预留，FAILED 终态；非法转换忽略
- `ssd_controller_init`（controller.c）改走状态机：任一槽位缺失/拓扑不符 → `transition(FAILED)` + 返回 `INVALID_ARGUMENT`；全过 → `transition(READY)`

**C1b host IO 优先调度**（`src/controller/scheduler.c` + `controller.h`）
- `ssd_controller_schedule(controller, host_io_active)` 轻量钩子：`ssd_controller_submit` 每次提交调一次（host_io_active=1）返回 0=后台让路；空闲返回 1=全力后台
- 真正后台预算由 `ftl_service` B3 空闲度（pending+completion_table 占用 → IDLE/NORMAL/BUSY）承担，scheduler 不重复实现队列（学习主控单 LUN，全量优先级调度收益有限）

**C3 FLUSH**（`src/controller/controller.c`）
- `SSD_REQUEST_FLUSH`：对全部 `controller->ftl[ch][ce][lun]` 同步 `l2p_cache_flush_all(&ftl->cache)` + `journal_flush_page(&ftl->journal)`（两个原语内部 `ftl_internal_sync_wait` 阻塞泵完）→ `completion_status=OK` + `complete_flag=1`
- 多块 FLUSH（block_count≥2）仍走 `ssd_request_split` default → `NOT_SUPPORTED`（FLUSH 是全局操作，不按 LBA 路由，命令表 `fl` 用 block_count=1）

**UART CLI 驱动 FTL**（`src/platform/stm32h743/usart.c`，D10 核心，+367 行）
- `g_wr_buf[FTL_READ_USER_SIZE×CW_NUM×PLANE_NUM]=16KB` 落 SRAM2（`.sram2_data` NOLOAD → `usart_init` 主动预填）：plane0 区（前 8KB）=`0x55555555`、plane1 区（后 8KB）=`0xAAAAAAAA`
  - Multi-Plane 写（pages=2）时前 2048B 进 plane0（LBA n）、后 2048B 进 plane1（LBA n+1）——写后读回一眼可确认双页簿记正确
- 命令表 5 条（`wr <lba> <n>` / `rd <lba> <n> [c]` / `tr <lba>` / `fl` / `info`）：
  - `rd` 无 `c` → hex dump 前 64B；有 `c` → 与 pattern 按 4B 粒度比对，报 `PASS` / `FAIL at byte <off>: got.. expect..`
  - `tr`/`fl` 单块提交；`info` 打印 controller 状态机名 + 每 (ch,ce,lun) `free/op/spare/pre_cond/bad/closed`
- **两段式**（D11 非阻塞 pending 语义）：handler 只 `ssd_controller_submit` 立即返回（打印 `submit ok, waiting...`），`uart_service_poll` 每轮末尾 `uart_cli_pending_poll()` 检查 `ssd_controller_is_complete` 打印结果——主循环 `ftl_service` 异步完成，CLI 不阻塞主循环
- `g_ctrl` 去 static（main.c）+ `usart.h` `extern ssd_controller_t g_ctrl;`

**NVMe 删除**（C2 砍掉落地）
- 删 `src/host/nvme/*.c`（5 桩）、`include/host/nvme/nvme.h`、`src/host/makefile.mk`；`config.mk` MODULES 移除 `host`
- 零引用零编译（wildcard 收集，无文件 include nvme.h）；`make debug/release/tests` 全零警告零错误（4 个 NVMe `#warning` 彻底消失）

### 验证

- `make debug / release / tests(FC_NAND_TEST_ENABLE=1)` 全零错误零警告
- SRAM2 44.1KB/128KB（+16KB g_wr_buf，不溢出）；AXI_RAM 377KB/384KB
- 上板序列（真 NAND）：`info`（state=READY + free/op/spare 计数）→ `wr 0 2` → `rd 0 2 c`（PASS 确认双 plane pattern）→ `tr 0` → `fl` → `info`（free 回补）
- **F8/F9 仍依赖上板**（紧急 CP 回切校验 / CQ·SQ 深度实测），本轮不改

### 备注

- `FC_COMMAND_MODULE_HOST_IO` 枚举保留零使用（F8/F9 上板后视需要再接线）
- stm32h7r3 平台 usart.c 未动

---

## v0.48 — Block State Table 瘦身（ftl_block_t 56B → 40B）

**日期**：2026-10-04

### 背景

AXI_RAM 使用率已达 98.2%（377.1KB/384KB），内存压力真实出现。逐字段审计 `ftl_block_t` 12 个字段后确认：**问题不是"该搬到 NAND"，而是"胖 + 字段顺序浪费对齐"**。行业对标（设计文档 §4.9.3）显示本方案 56B/项 是消费级 DRAM-less 基准（4~16B）的 3~14 倍。

### 落地

**① `rr` ECC 健康 16B → 3B**（`include/ftl/bst.h`、`src/ftl/read_recovery.c`）
- 删 `ecc_history[8]` + `history_idx` + `last_update`，改单字节 `last_ecc`
- 8 深度环形缓冲的**唯一用途**是供给趋势比较的 `prev` 一个值 → 单字节足够
- **顺带修复 bug**：原 `prev = ecc_history[history_idx]` 取的是环形缓冲里**将被覆盖的最老格**（8 次读之前的值），与注释「上一次值」不符；`last_ecc` 语义即真正的上一次读数
- 顺带清掉 `last_update`（4B，`gc.c:401` 写点有、读点零，且 `gc.c:407` 调用时已在传 `0U` 假值）
- `rr_record_ecc` 签名去掉 `total_writes` 形参，3 个调用点同步（`ftl.c:115` / `gc.c:407` / `wear_leveling.c:254`）

**② 字段重排**（`include/ftl/bst.h`）
- `rr` 从 `valid_map` **之后**移到**之前**
- 原因：`valid_map` 是 `uint64_t[PLANE_NUM]` 需 8 字节对齐；`rr` 只有 3B，留在数组之后会被尾部对齐从 40B 撑到 **48B**（白丢 8B/块 = 32KB）
- 新增 `_Static_assert(sizeof(ftl_block_t) == 40, ...)` 编译期守卫（此前无任何尺寸断言）

### 审计发现（本次保留，未处理）

| 类别 | 明细 | 规模 |
|---|---|---|
| 死字段（写了无人读） | ~~`last_pe_ts`~~（**v0.51 已消费**，见下）、~~`program_count`~~、~~`bad_reason`~~、~~`gc_generation`~~（三者 **v0.56 已删**） | ~~8B/块 = 32KB~~（**订正：这 4B 落在 `valid_map` 对齐 padding 里，删之省 0 字节**，见 `ftl_design.md §4.9.6`） |
| 只写不读的状态位 | `OPEN`、`RESERVED`、`TEMP_HOT`、`TEMP_COLD`、~~`PRE_COND`~~；~~`EMERGENCY`~~ 全库零引用（两者 **v0.52 已删**） | — |
| 跨重启不持久化 | `erase_count` / `read_count` / `rr` 每次 `ftl_init` 被 `memset` 归零（`ftl.c:471`）；`METADATA_TYPE_ERASE_COUNT` 定义了但零引用 | — |

死字段成因：v0.32 建 BST 时按「行业 Block Info Table 标准形态」一次性把字段铺全（`c74f1ca`），标注「为后续预留」，但消费方一直未写。

**持久化缺口是独立的功能问题**——磨损均衡（`erase_count`）与 read disturb（`read_count`）、慢死预测（`rr`）跨重启全部失效，需单独排期，不属于结构体重构范畴。

### 验证

- `make tests` 零告警零错误；`_Static_assert` 通过
- `.bss.g_blocks`：`0x38000`(224KB) → **`0x28000`(160KB)**，精确命中预期
- AXI_RAM：98.2% → **81.5%**（377.1KB → 313.1KB，-64KB）；总 SRAM 55.8% → 48.6%
- **上板验证未做**（按项目规则由用户执行）

### 备注

- **SB 索引粒度已评估，本次不做**：`allocator.c:299` 的 `block1 = block0` 使 SB 永远配对同号物理块，两 plane 的 `erase_count`/`state`/`sb_id`/`valid_count` 天然相同，per-block 存是重复的 → 改按 SB 索引可再省 96KB（160KB → 64KB）。但 `ftl_gblock()` 是所有索引的单一入口，改动覆盖整个 FTL，且丢 per-plane 分辨率（`read_count`/`rr`）、与 BBT 的 per-physical-block 粒度不一致。设计文档 §4.9.5 选项 C 早已评估并否决，理由一致。⚠ **v0.57 结案**：本条前提 `block1 = block0` 已被 **v0.12 Policy B** 删除（逐 plane 独立选块）⇒ 两 plane 元数据互不相同、**无可去重**；且主流是**两级**结构（band 级 + per-block BIT）。见 **v0.57 条目**。
- **关联问题**：`block1 = block0` 本身是简化而非行业做法（真实主控允许 SB 内每 plane 用不同物理块号）。当前同号约束导致 plane0 的坏块连坐浪费 plane1 的同号好块。与索引粒度强相关，应合并评估。
- **rebuild 套件夹具超容**：`tests/unit/test_rebuild.c` 的 245KB 夹具超 128KB DTCM 堆，本次结构体缩小不解决该独立缺陷。→ **2026-10-04 后续处理**：已把该套件从板端构建摘除（`src/fc/makefile.mk` + `tests/unit/ut_main.c`），夹具落地区域另议，见 `fault_capture.md` §7 与 `debug_sbrk_heap_stack_20261004.md` §5。

---

## v0.49 — P0-1 计数持久化（Wear 表 load/save）

### 背景

`ftl_optimization_plan.md` §P0-1（最高优先）。`ftl_init` 的
`memset(params->blocks, 0, sizeof(ftl_block_t) * FTL_BLOCK_TOTAL)` 每次开机把整张 BST 清零
⇒ 三个计数跨重启归零，依赖它们的三条机制**全部失效**：

| 消费方 | 位置 | 失效表现 |
|---|---|---|
| 动态 WL（选擦除最少块） | `allocator_select_plane_block` | 永远选到第一个 free 块 |
| 静态 WL（`max_ec - min_ec ≥ 5000`） | `wl_should_trigger` | 判定恒假 |
| read-disturb 预测（`read_count ≥ 1000`） | `rr_predictive_scan` | 判定恒假 |

### 关键决策

**① 介质形态：块内**页式追加日志**，不是"每次整表重写"。**

块 `WEAR_BLOCK`（26）只有 64 页，全量快照 25 页 ⇒ 整表重写会让块 26 在 ~2.6 次落盘后
被擦一次，磨损远超数据块（器件寿命 ≈ 4096 块 × 10⁵ P/E ≈ 4.1×10⁸ 次块擦除；整表重写方案
块 26 寿命内约 12 万次自擦，超器件规格；追加日志 1–4 页/次 ⇒ 约 2–6 万次，在规格内）。

形态与 Anchor / Journal 同一套日志惯例：每页自包含 `magic + sequence + entry_count + CRC`，
**页序 == sequence 序**（块内只追加；块满则擦块 + 从头写全量快照）。
语义 = **绝对值 + 最新胜**（不是增量累加）⇒ 同一块多次出现只有最后一页生效，
重试重复写同一块是幂等的。

**② 持久化哪三个字段。**

只落**有消费方或为档三预留**的：`erase_count`（静态/动态 WL）、`read_count`
（read-disturb 阈值 + `wl_select_victim` 平手判定）、`program_count`（健康模型，
**当前无消费方**，为 `ftl_design.md` §19 档三预留）。

不落 `last_pe_ts` / `gc_generation`（死字段，且 `last_pe_ts` 是时间戳、跨重启无意义）。
**`rr`（3B）不落** —— 它驱动 `rr_predictive_scan` 的 CRITICAL 分支，但把它加进来会把条目
从 12B 撑到 16B、快照 25→33 页、压缩 delta 余量；read-disturb 的主触发
（`read_count ≥ 1000`）已被覆盖。登记为后续扩展。

**③ 未复用 `METADATA_TYPE_ERASE_COUNT`（`metadata.h:36`，仍零引用）。**

Wear 块位置固定，不需要走 Metadata Directory 登记——与 BBT 同理（BBT 也是固定块，
不走 Directory）。`METADATA_TYPE_*` 是给"位置会变"的元数据用的。

**④ 状态放 file-scope static，不放 `ftl_context_t`。**

与最近的模板 `bbt.c`（`g_bbt_sequence` / `g_bbt_dirty`）一致。
⚠ 登记局限：**单实例**，与 `bbt.c` / `completion_table.c` 同类，是已登记的提维度阻断点之一，
等 dim ≥ 2 落地时一并搬进 `ftl_context_t`（per-instance 惯例见 `checkpoint.c`）。

**⑤ 节拍门限量用 `ftl->total_writes`（主机写成功数）作为"擦除活跃度"的代理。**

没有独立的擦除计数器可用，而主机写入是 GC 擦除的唯一驱动。
`WEAR_FLUSH_INTERVAL_MS`（120 s）**或** `WEAR_FLUSH_ERASE_DELTA`（1024 次写入）任一满足即落盘。
纯读负载下 `total_writes` 不增长 ⇒ 只有定时门限生效（这正是想要的：读负载只需保 `read_count`）。
代价：两次落盘之间掉电丢 ≤ 1024 次擦除 / ≤ 120 s 读计数。Wear 是**软状态**
（丢了只退化 WL 精度，不丢数据），静态 WL 阈值 5000 ⇒ 1024 的窗口 = 20%，可接受。

### 改动清单

| 文件 | 改动 |
|---|---|
| `include/ftl/wear.h`（新增） | `WEAR_MAGIC` / `wear_entry_t`(12B) / `wear_page_t`(2048B) / 节拍常量 / 3 个原型 + 3 条 `_Static_assert` |
| `src/ftl/wear.c`（新增） | `wear_page_addr` / `wear_init` / `wear_flush` / `wear_mark_dirty` + 脏位图 |
| `include/ftl/ftl_config.h` | 新增 `FTL_WEAR_ENTRY_BYTES (12U)`；`FTL_WEAR_TABLE_BYTES` 由 `× sizeof(uint32_t)` 改 `× FTL_WEAR_ENTRY_BYTES`（16 KB → 48 KB，**仅逻辑表字节，不影响介质寻址**） |
| `src/ftl/ftl.c` | `ftl_init` 在 `bbt_init` 后加 `wear_init`；`ftl_service_idle`/`_normal` 在 `bbt_flush` 后加 `wear_flush`；3 处 `wear_mark_dirty` |
| `src/ftl/gc.c` | 2 处 `wear_mark_dirty`（`gc_erase_and_free` 的 `erase_count++`、`gc_migrate_sb` 的 `read_count++`） |
| `src/ftl/wear_leveling.c` | 2 处 `wear_mark_dirty`（`wl_relocate` 的 `read_count++`、`rr_retire_block` 调用点） |

**7 个脏标记点**（`erase_count`/`read_count`/`program_count` 的每一个变更点）：

| # | 位置 | 变更的计数 |
|---|---|---|
| 1 | `gc.c` `gc_erase_and_free` | `erase_count++` |
| 2 | `ftl.c` 主机读完成 | `read_count++` |
| 3 | `gc.c` `gc_migrate_sb` 迁移读 | `read_count++` |
| 4 | `wear_leveling.c` `wl_relocate` 迁移读 | `read_count++` |
| 5 | `ftl.c` plane0 新页 | `program_count++` |
| 6 | `ftl.c` plane1 新页（multi-plane） | `program_count++` |
| 7 | `wear_leveling.c` `rr_retire_block` 调用点 | `read_count = 0`（`rr_retire_block` 只拿到 `ftl_block_t*`，故标记放在其唯一调用者） |

### 介质格式

**不新增格式变更**：块 26 早已在分区表 `g_ftl_partitions[]` 里预留（`"Wear"` 项，
`ftl_config.c`）且被 `ftl_is_metadata_block` 排除 ⇒ 本次只是**第一次往里写内容**。
块号/分区一个没动。上板前仍须擦除芯片——那是因为**上一次布局维度无关化**
（`c5cb8a0`：Anchor 改分页自包含 + L2P 块位置 + Journal 2→4 块），不是本次。

### 开机可观测

`wear_init` 末尾一行横幅式 `LOGI`（这是判据「重启后计数一致」的观测口）：

```
[INF] [WEAR] seq=… pages=… max_ec=… min_ec=…
```

`pages` = 回放的有效页数（0 = 冷启动）；`max_ec`/`min_ec` = 非坏块的擦除计数极值。

### 验证

- `make all` / `make tests` 零告警零错误；`build/*/obj/ftl/wear.o` 由 `wildcard` 自动纳入
  （**不需要改 Makefile**）
- `make format-check` 通过（`FORMAT_FILES` 用 `find`，新文件自动纳入）
- `_Static_assert`：`sizeof(wear_entry_t) == 12`、`sizeof(wear_page_t) == 2048`、
  `WEAR_SNAPSHOT_PAGES(25) <= PAGES_PER_BLOCK(64)`
- **本次不新增任何测试源**（平台规则：测试源用户自建）
- **上板验证未做**（按项目规则由用户执行）

### 登记不做

- **`program_count` 溢出**：u16 从不归零（`gc_erase_and_free` 只清 `erase_count`/`last_pe_ts`/valid）
  → **v0.56 已删该字段**，溢出问题随之消失
- **`last_pe_ts` / `gc_generation`** 死字段 → `last_pe_ts` v0.51 已消费；`gc_generation` **v0.56 已删**
- **`rr` 持久化**（见决策 ②）
- **多实例**（见决策 ④）
- **read 侧量化**：若窗口内 4096 块全脏（全盘随机读），日志退化成整表 25 页 ⇒ 与整表重写同价。
  要收紧就给 read 侧加"每 N 次读才标脏"的量化，或调大 `WEAR_FLUSH_ERASE_DELTA`。

---

## v0.50 — P0-2 元数据写模型统一（追加日志 + 块池轮转）

**日期**：2026-10-12

**状态**：✅ 代码已落地、**未上板验证**（`make clean && make all` / `make tests` 零告警零错误）

### 背景

`ftl_optimization_plan.md` 里 P0-2 的原文是「`METADATA_DIR_ENTRIES=128` 但 region 有 2048」——
**这一条已过时**（常量早已改成 `= L2P_REGIONS_PER_INSTANCE`）。但顺着它往下查，挖出的是
比「容量不够」严重得多的两个**已生效**缺陷：

1. **Anchor 一次都没写过**：`metadata_dir_fill_fixed` 把 2048 个 region **全部预登记**成固定位置
   ⇒ `metadata_dir_lookup` 永远命中 ⇒ `dir->dirty` 恒 0 ⇒ `metadata_dir_flush_to_anchor` 早退
   ⇒ `metadata_dir_load` 永远失败 ⇒ 永远走 `metadata_dir_init`。整条动态路径死透。
2. **Checkpoint 第二次写即损坏**：`checkpoint_create` 每次重写**同 6 页**，NAND 只能 1→0 ⇒
   第二次起头 CRC 即失效 ⇒ `checkpoint_find_latest` 失败 ⇒ `ftl_boot_fresh_start` ⇒ **映射全丢**。
   T3 定时触发是「elapsed ≥ 10 s 且 IDLE/NORMAL」，`ftl_service_idle` 在 `inflight==0` 时必被调
   ⇒ **板子空闲跑过 10 s 就必中**——不是潜伏，只是从未被断言。

**真正的根缺陷在最下面一层**：`l2p_cache_flush_one` 把每个 L2P region 写回**固定地址，且从不擦除**。
NAND 只能 1→0，对已编程页二次编程 = 新数据 **AND** 旧数据 ⇒ **静默的映射损坏**。
同病遍布全部元数据：全仓 `ftl_sync_erase_block` 的调用者原先只有 `journal.c` / `gc.c` / `wear.c`
—— **没有任何 L2P / Anchor / BBT / Checkpoint 块被擦过**。
潜伏原因：顺序冒烟测试每个 region 只 flush 一次（64 行 × 512 项 = 32768 LBA，测试写不到）。

### 关键决策

① **方向 = B 方案（追加日志 + Directory），按存储主控方向走**（用户定）。
   不是「加容量」，是**换写模型**——扩容只治了症状，且那次扩容反而把机制从半工作翻成全死。

② **修复范围 = 全部元数据统一**（用户定）：L2P / Anchor / BBT / Checkpoint 一套写模型。
   **Journal 已经是对的，不动**（4 块环 + 擦除 + 两遍重放是全仓唯一本来就对的元数据；
   本次只挪块号 40-41 → 58-61）。

③ **验收判据 = 两者都要**（用户定）：① 同一 region 连续两次 flush 后读回正确；
   ② `region_id > 128` 的 LBA 走动态分配。**追加 ③**：复位后数据仍在（因为当前必然失败）。

④ **Anchor 类元数据落多块池 N=32**（用户定）。理由不是磨损（本仓磨损不是约束），
   是**主控形态**：固定根 + 专用元数据块池。

### 落地

**统一写模型（4 条规则 + 1 条校验）**：

| 规则 | 说明 |
|---|---|
| 只追加 | 元数据页只写**擦除态**页；对已编程页的二次编程 = 损坏，一律禁止 |
| 带 sequence | 每条 record 带单调递增 sequence；页序 == sequence 序 |
| 池内轮转 | 写满一块 → 擦**池内下一块** → 从页 0 续写 |
| 最新有效胜 | 读取 = 扫池内所有 record，取「**整条校验通过**」中 sequence 最大者 |
| 整条校验 | 一条 record 的多页必须**全部有效且 sequence 一致**，否则整条丢弃（不取半条）|

**共用原语**（`src/ftl/metadata.c`，Anchor / Checkpoint / BBT 三方复用）：
`metadata_ring_addr` / `metadata_ring_locate` / `metadata_ring_prepare_write`。

`metadata_ring_locate` 是本次最费心的算法（O(池块数) 快路径 + 仅损坏时全池慢路径）：
① 逐块廉价探针**该块末条** record（末条有效 = 该块写满）；
② 第一个末条无效的块 = **游标块**；若全部有效 ⇒ 池转满一圈 ⇒ 游标 = 最新满块之后，且该块整体擦掉重写；
③ 游标块内降序找最后一条有效 record；④ 写点 = 其 +1（块内最后一条则跨到下一块槽 0）；
⑤ 候选 = {全局最新满块末条, 游标块末条}，去重、按 seq 降序、**整条校验**；
⑥ 慢路径（仅损坏时）：全池探针取 max seq，整条校验，失败则排除该 seq 再找次新。

**新块布局（per-plane 块号）**：

| 区 | 块号 | 块数 | 说明 |
|---|---|---|---|
| L2P 基础层 | 0..15 | 16 | **哨兵层：永不写入**，页保持擦除态 ⇒ 读回 0xFF = 未映射 |
| Anchor 池 | 16..47 | 32 | 一条 8 页，256 槽 |
| Checkpoint 池 | 48..55 | 8 | 一条 1 页，512 槽 |
| BBT 池 | 56..57 | 2 | 一条 1 页，128 槽 |
| Journal | 58..61 | 4 | **只挪号，逻辑不动** |
| Wear | 62 | 1 | 只挪号 |
| `FTL_METADATA_BLOCK_END` | **63** | | 原 27 |

物理 126 / 4096 = **3.1%** 系统区。

**Checkpoint 写序重排**：旧序 ② flush 脏 region → ③ 写 header → ④ flush Anchor → ⑤ truncate
⇒ header 早于 Anchor ⇒ 两者之间掉电会留下「header 新 / Directory 旧」⇒ 已声称落盘的 region
在 Directory 里找不到 ⇒ **丢更新**。新序 **① 快照 bitmap → ② flush 脏 region → ③ flush Anchor
→ ④ 写 header → ⑤ truncate journal**：掉电在 ④ 之前 ⇒ 本次 CP 未记录，boot 用上一个 CP
并回放 journal（⑤ 还没跑）⇒ 一致。

**L2P 日志块**：`l2p_cache_flush_one` 每次 flush **无条件追加**到日志块（一块 64 槽，一个 region
= 一页）。块满 ⇒ 从 Free Pool 取新整块（`allocator_alloc_reserved_block`，**不是**与 GC 共享的
「冷 SB 的一页」）。游标**不跨重启持久化**（`metadata_dir_load` 置 `cur_block_valid = 0`），
首次追加时才分配 ⇒ 绝不复用可能已编程的旧页。旧块活 region 归零即归还
（`metadata_dir_commit_region` 里急切回收，把「已无引用却仍带旧内容」的窗口压到最短）。

### 介质格式

**再次变更**（叠加在 `c5cb8a0` 布局维度无关化之上）⇒ 上板前**必须先擦除芯片**。

### 开机可观测

`metadata_dir_load` 末尾一行（判据②「走动态分配」/ 判据③「Anchor 真的落盘」的唯一观测口）：

```
[INF] [META] anchor_seq=… dyn=… cur_blk=… live_blks=…
```

### 验证

- `make clean && make all` / `make clean && make tests` 零告警零错误
  （⚠ `make all` 后直接 `make tests` 是空操作：共用 obj 目录、不跟踪 CFLAGS ⇒ **必须先 `make clean`**）
- 新增 `_Static_assert`：各池槽位数 ≤ `UINT16_MAX`；`PAGES_PER_BLOCK % ANCHOR_PAGE_COUNT == 0`
- **本次不新增任何测试源**（平台规则：测试源用户自建）
- **上板验证未做**（按项目规则由用户执行）

### 顺带修复（超出计划字面范围，登记）

- **`l2p_checkpoint_header_t` 尺寸 bug（2296 → 2048 B）**：`padding[2048 - 7*4 - 4]` 算式把
  `region_bitmap[64]`（256 B）算成 7 字 ⇒ 结构体 2296 B > 一页。旧写入被
  `ftl_cmd_write_data`（4 CW × 512 B）**静默截到 2048**，所以从未真正越界——但「一条 record
  = 一页」名不副实。现改为 `padding[2048 - 70*4]`（1768 B）+ `_Static_assert`，
  并把 `CHECKPOINT_CRC_LEN` 改写成 `offsetof(..., crc)`（算式写错时覆盖范围会跟着错）。
  （⚠ **v0.62 起该宏随记录内 CRC 一并删除**——元数据页完整性改由下层 FC per-CW CRC + ECC 承担。）
- **`ftl_boot_fresh_start` 补 `ftl_rebuild_closed_sb_list`**：`main.c` 的注释断言
  「用户数据 written 块仍回填（时序安全：boot_recovery 的 F2 重建会把它们拉出）」，
  但 fresh_start 路径此前跳过了它 ⇒ Free Pool 会信任未擦除的块。已闭合。

### 登记不做

- **阶段 3 L2P 日志块压缩**（抗碎片）⇒ 已登记为 **`ftl_optimization_plan.md` 的 P0-3**
  （含完整设计：触发条件 / victim 选择 / 搬移顺序 / 重入切断 / 崩溃安全 / 判据 / 依赖）。
  一句话：若某日志块挂着「永不再写」的冷 region，它永远不归零 ⇒ 永久占块。
  **依赖本 ADR 的阶段 1+2 先上板验证通过**，**长时压测前必须补**。
- **多实例**：`checkpoint.c` / `bbt.c` / `wear.c` 的 file-scope static 游标/序号在多 LUN 下
  所有实例共享一份（局限登记在案）。
- **不做 SPDK FTL 式的 P2L 重建**（每 band 尾部存 P2L、开机顺序扫描重建 L2P）——那是替换整层
  持久化机制的大改，登记为后续方向；本仓 `ftl_rebuild_closed_sb_list` 已有雏形。
- **不顺手修**：~~`ftl_is_metadata_block` 的 per-plane 粒度~~（**✅ v0.69 已修**，见该版 ADR）、~~`program_count` 无消费方~~（**v0.56 已删该字段**）、`rr` 不持久化（**✅ v0.65 已落地**）。

## v0.51 — P1 算法档（GC victim 加权 + GC 触发补齐 + 读写判别修复）

**日期**：2026-10-12

**状态**：✅ 代码已落地、**未上板验证**（`make clean && make all` 182480 B / `make clean && make tests`
260588 B，均零告警零错误；`make format-check` 通过）

### 背景

`ftl_optimization_plan.md` 的 P1 档。开工前逐条核对源码，四条前提与现状不符（登记在下），
另挖出一个**既有真缺陷**。

### ① P1-1 GC victim 多维加权评分

`gc_select_victim` 原为**单维贪心**（取 `valid_count` 之和最小的 SB），设计 §4.3 指出的
「磨损均衡缺失」「温度维度缺失」两条缺陷确实成立。

**本仓定案**（`cost` **取小** = 越优先回收，四项均为 0..1000 千分比整数，无浮点）：

```
cost = 500·valid_ratio + 200·erase_ratio + 200·cold_penalty + 100·youth_penalty
```

| 项 | 定义 | 方向 |
|---|---|---|
| `valid_ratio` | `valid*1000 / (present_planes*PAGES_PER_BLOCK)` | 有效页少 ⇒ 优先 |
| `erase_ratio` | `max_erase_of_sb*1000 / max_erase_all` | 磨损高 ⇒ 降权（避开临近寿命块） |
| `cold_penalty` | `HOT ? 0 : 1000` | **HOT 优先**当 victim（COLD 有效页几乎不变，下次还得搬） |
| `youth_penalty` | `1000 − (max_pe_ts − pe_min)*1000/max_pe_ts` | 越久没被擦 ⇒ 越优先 |

**与设计 §4.3 逐字公式的三处偏离**（文档公式符号自相矛盾：按「取大」读会奖励有效页**多**的块，
与同节「选有效页最少」相反；`temp_factor` 未定义；擦除项字面 `(1-erase_ratio)` 在取小约定下
会**偏向**磨损重的块）——三处均已写进 `gc.h` 的 `GC_WEIGHT_*` 块注释。

**`last_pe_ts` 不持久化**（P0-1 刻意未落盘）：上电后全表为 0 ⇒ 时间项对所有条目同值
（常量偏移，不影响排序）；开机后被 GC 擦过的块才有非零时间戳 ⇒ 「本上电周期内还没被回收过」
的块优先——正是公平性语义本身。

新增 `gc_table_extremes()`（一次全表扫描取 min/max 擦除 + max `last_pe_ts`），P1-1 的归一化分母
与 P1-2 的 T5 共用这一次扫描。

### ② P1-2 GC 触发补齐（T4/T5 落地，**T6 废弃**）

原 `gc_check_trigger` 只有三条（水位 / 写入量 / 定时）。新增 `gc_check_triggers()` 返回**位掩码**
`GC_TRIGGER_*`，`gc_check_trigger()` 退化为 `!= 0` 薄封装（旧契约不变，调用点 `ftl.c` 改用位掩码
并打观测行 `[GC] trig=0x%02x pre=… free=… writes=…`）。

- **T4 强制**：`free_count <= FREE_POOL_EMERGENCY (8)`——终于消费掉 P1-3 清单里的死常量。
  与 T1（`free_count < FREE_POOL_TARGET 32`）**同源不同阈值**：T1 主动补池，T4 是池见底的最后手段。
  （v0.52 更正：原文写作 `pre_cond_count < 32`，该字段已随 P1-3 删除，见下。）
- **T5 磨损比**：`max_erase >= GC_WEAR_LEVEL_MIN_ERASE (64) && min_erase > 0 && max/min > 10`。
  用**除法**不用乘法（既避免 `10*min` 溢出，也逐字对应「比 > 10」）。两个 guard 的理由：
  `min_erase > 0` —— 对 0 求比值无意义，且「有块从未被擦过」是**静态 WL**（`wl_execute` 搬冷数据）
  的职责，T5 只管「所有块都在轮转、但轮得不均」；`max_erase` 地板 —— 防早期（max 才几次）
  满足比例后反复空跑。

**T6「碎片率」废弃**（原定义「平均有效页 > 70% 触发」，`GC_FRAGMENT_THRESHOLD 0.7`）：

1. **方向与标签相反**：碎片率高应对应有效页**少**；且 `gc_select_victim` 挑的是有效页**最少**的
   victim ⇒ 把它移出 `closed_sb_list` 后剩余平均值**上升** ⇒ T6 更易命中 ⇒ **自激正反馈**。
2. **收益为负**：搬一个 70% 有效的 SB 要写 ~90 页，净空间仅 `1 − 0.7 = 0.3` 个 SB，写放大 ~45×。
   行业 victim 选择定式（贪心取有效页最少；LFS cost-benefit 最大化 `(1−u)·age/(1+u)`）**一致偏向 u 小**。
3. **语义为空**：closed 块平均 70% 有效 ⇒ 盘近满 ⇒ 空闲池必然小 ⇒ **T1/T4 已命中**。
4. **主流处置方向相反**：占用率高 ⇒ 写放大 ≈ `1/(1−u)` 高 ⇒ 行业杠杆是 **OP（预留空间压低 u）**
   与 **GC 限速**（§4.6 的 `GC_BANDWIDTH_*`），不是「因为块太满所以多 GC」。

⇒ `GC_FRAGMENT_THRESHOLD` 不定义；`GC_TRIGGER_*` 位图**刻意留空 0x20**。完整论证见
`ftl_design.md` §4.4 表下注；`gc.h` 对应位置有交叉引用。

### ③ 顺带修复：`ftl_handle_completion` 读写判别缺陷（既有，非本批引入）

`ftl_handle_completion`（`ftl.c`）用 `pending->io.buffer != NULL` 判别读/写，但 `ftl_submit_write`
**拒绝 NULL buffer**（`ftl_io_request_t.buffer` 对写是「数据源」）⇒ 该条件对写**恒真** ⇒
写成功分支（`journal_append` / `l2p_cache_write` / `checkpoint_journal_watermark` / `total_writes++` /
VPC 簿记 / `wear_mark_dirty`）与写失败分支**都是死代码**，每次写完成反而走读分支
（`ftl_cmd_read_data` 把 NAND 数据读回源 buffer）。

**现象**：`wr` 报 `done status=0`，但 L2P/Journal 从不更新 ⇒ `rd` 未映射 ⇒ 返回全 0。
**爆炸半径**：`total_writes` 恒 0 ⇒ 连带静默关掉 checkpoint T2、`wear_flush` 的写入量节拍门、
`wl_should_trigger` 的 `WL_MIN_INTERVAL`、**P1-2 的 T2**、以及 **P1-1 的时间维度**（`last_pe_ts`
取 `total_writes` ⇒ 恒 0 ⇒ 时间项永久中性）。

**修法**：`ftl_pending_req_t` 新增显式 `op`（`ftl_pending_op_t`：`PENDING_OP_READ/WRITE`），
`pending_add` 加尾参，两处判别改按 `pending->op`；订正 `pending.h` 的文件头注释
（原文「写操作不需要 buffer」与代码相反）。

### 前提核对：三条计划书描述已过期（登记）

- **P1-5（SB 独立块号）已完成**：`allocator_open_sb` 已逐 plane 独立选块
  （`allocator_select_plane_block`），`plane_blocks[PLANE_NUM]` 存各自块号。
  计划书「现状 block1 = block0」是过期描述 ⇒ **跳过**。
- **P1-6 前提部分过期**：Journal 已是 **4 块**（`JOURNAL_BLOCKS = CW_NUM`，块 58-61），
  非文档 §14.1 的「2 块」。剩余工作 = region 更新合并 + `journal_truncate` 尊重 `up_to_sequence`。
- **P1-3 前提含糊**：`pre_cond_count ≡ free_count` 的根因是 `gc_erase_and_free` **先擦后归还**
  ⇒ 每个空闲块本就已擦除，文档的三级池在本仓**物理上是同一批**。待定案。

### 验证

- `make clean && make all` 182480 B（P1-1+P1-2 前 182240 B）；`make clean && make tests` 260588 B
- 零告警零错误；`make format-check` 通过
- **本次不新增任何测试源**（平台规则：测试源用户自建）
- **上板验证未做**（按项目规则由用户执行）

### 登记不做

- **T6 碎片率触发** —— 废弃，论证见上。
- **P1-3 三级池真三级化** —— 待定案（三池物理同批，需先定语义）。
- **P1-4 L2P cache thrashing 检测 + 降级**、**P1-6 journal 合并 + `truncate` 语义** —— 后续批次。
- **P1 各效果类判据本轮拿不到** —— 依赖 ③ 先上板验证（`total_writes` 才真正开始递增）。

## v0.52 — P1-3 Free Pool「三级水位」（原题「真三级化」的定案更正）

**日期**：2026-10-12

**状态**：✅ 代码已落地、**未上板验证**（`make clean && make all` 182336 B / `make clean && make tests`
260452 B，均零告警零错误；`make format-check` 通过）

### 结论：计划书的前提是错的，改的是**文档**不是**实现**

`ftl_optimization_plan.md` 的 P1-3 标题为「Pre-conditioned Pool 真三级化」，判据是
「`pre_cond_count` 与 `free_count` 解耦，前台 IO 不再等 GC」。逐条核对源码后判定**该判据不可达、
且不该达**：

1. **`pre_cond_count ≡ free_count` 不是缺陷，是正确推论**。`gc_erase_and_free`（`gc.c`）的语义是
   **先擦除、后归还**（`allocator_free_block` 的文件注释即写「本入口的契约是『调用者已擦除』」）⇒
   池内**每个**块都已擦除 ⇒ 不存在「已预擦除块」与「未擦除块」的物理区分 ⇒ **没有东西可以分区**。
   `pre_cond_count` 与 `free_count` 本就是同一个量，所谓「解耦」等于要求实现制造一个假的第二池。
2. **文档 §3.7 把「三级水位」画成了「三级池」**。设计原意（BG GC 预擦除池）在本仓的实现里
   体现为**一个已擦除池 + 三级软/硬水位**，不是三个池。
3. **主流主控正是这么做的**：一个已擦除 free pool + 软/硬水位（low watermark 触发后台 GC、
   critical watermark 阻塞/紧急 GC），另配**语义独立的保留池**（本仓 = OP 池 性能缓冲 / Spare 池
   坏块替换）。没有主流控制器在 free 池内部再切「预擦除 / 未擦除」两级——擦除落在关键路径上
   本身就是 OP 与后台擦除要消灭的东西。
4. **原判据的正确替代**：「前台 IO 不再等 GC」由**水位①**保证（池里常驻 ≥32 个已擦除块，
   `FREE_POOL_TARGET`），不由第二个池保证。

### 定案：一个池 + 三级水位（代码侧只删不加）

| 水位 | 条件 | 语义 |
|---|---|---|
| ① 正常 | `free_count ≥ FREE_POOL_TARGET (32)` | 不动作 |
| ② 补池 | `FREE_POOL_EMERGENCY (8) < free_count < 32` | 后台 GC 主动补池（`ftl_service_idle` 循环补到水位①） |
| ③ 紧急 | `free_count ≤ FREE_POOL_EMERGENCY (8)` | 写路径同步兜底 + `GC_TRIGGER_EMERGENCY` |

**删除清单**（全部为死代码或冗余派生量，删除前逐个确认零读者）：

| 符号 | 位置 | 删除理由 |
|---|---|---|
| `pre_cond_count`（字段） | `ftl_allocator_t` | `≡ free_count`，唯一写入点是 `power_failure_recovery.c` 的直接赋值 |
| `emergency_count`（字段） | `ftl_allocator_t` | 无任何读写点 |
| `FTL_BLOCK_STATE_PRE_COND` | `bst.h` 状态位 | 只写不读（无 `ftl_block_state_is` 读者） |
| `FTL_BLOCK_STATE_EMERGENCY` | `bst.h` 状态位 | 无置位点、无读者 |
| `allocator_needs_gc()` | `allocator.c` | 零调用者，且重复了 `gc_check_triggers` 的 T1/T4 |

连带：`FTL_BLOCK_STATE_MASK` 由 `0x01FF` 收窄为 `0x007F`（bit7/bit8 释放）；
`cli.c` 状态行去掉 `pre_cond=%u`；`bst.h` 的结构体注释改为「BAD/OPEN/CLOSED/FREE/RESERVED」。

**GC 触发的真源收敛到 `ftl/gc.h`**：`allocator.h` 中原 `allocator_needs_gc` 的声明位置留一条
指向 `gc.h` 的删除注记（水位判定只此一份）。

### 文档同步（本次改动的主体）

`ftl_design.md` §3.7 整节重写为「Free Pool 三级水位（v0.52 更正）」，含水位图 + 三行水位表 +
「水位之外的两级保留池」表；ADR-9 由「三级池」改为「三级**水位**」；另修正约 15 处连带表述
（对比表 / 树 / 流程图 / 状态位清单 / 接口清单 / 伪码 / 文件清单）。
`ftl_ut_munit.md` 的 allocator 套件行与不变量行、`ftl_api_usage.md` 的三处接口描述同步。

### 验证

- `make clean && make all` 182336 B（v0.51 的 182480 B − 144 B）；`make clean && make tests` 260452 B
- 零告警零错误；`make format-check` 通过
- **不新增任何测试源**（平台规则：测试源用户自建）
- **上板验证未做**（按项目规则由用户执行）；⚠ 该批改动**无介质格式变更**，无需重新擦除芯片

### 登记不做

- **P1-4 L2P cache thrashing 检测 + 降级**、**P1-6 journal region 合并 + `journal_truncate` 尊重
  `up_to_sequence`** —— 后续批次。
- **P1 各效果类判据**仍依赖 v0.51 ③ 的读写判别修复先上板验证。

## v0.53 — P1-4 L2P cache thrashing 检测 + 降级（初稿三条降级只保留一条）

**日期**：2026-10-12

**状态**：✅ 代码已落地、**未上板验证**（`make clean && make all` 182872 B / `make clean && make tests`
260988 B，均零告警零错误；`make format-check` 通过）

### 前提核对

设计 §6.5 给了三条降级策略（write-through / 增大 batch 预取 / 停 background flush）。
`hit_count`/`miss_count` 确如计划书所述「只写不读」（无任何降级逻辑）——缺口成立。但三条策略
逐条对照**本仓实现**后**只有一条成立**。

### 检测（新增）

累计 `hit_count`/`miss_count` 随开机时长渐近，反映不了**当前**行为 ⇒ 降级判定必须看**窗口**：

- `l2p_cache_t` 新增 `window_access` / `window_miss` / `miss_pct` / `thrashing`。
- `l2p_cache_note_access(cache, is_miss)`（static）在 `l2p_cache_find_or_load` 的 hit/miss 分支各调一次；
  每 `L2P_THRASH_WINDOW (256)` 次访问结算 `miss_pct`，带**滞回**：进入 `≥ L2P_THRASH_ENTER_PCT (90)`、
  退出 `< L2P_THRASH_EXIT_PCT (70)`，中间保持——避免阈值附近反复翻转。
- 公开只读访问器 `l2p_cache_is_thrashing()`（static inline）。
- 观测口：**状态翻转**时打一行 `[L2P] thrash=… miss=…% hit=… miss_total=…`（抖动不刷屏）；
  CLI 状态行 `ftl[...]` 追加 `thrash=%u miss=%u%%`。

### 降级（只保留一条）

`ftl_service_normal` 每轮主动 flush 最多 4 条脏行的循环，**在 thrashing 时整体跳过**——脏行只由
**驱逐路径按需**写回。理由：thrashing 下这些主动 flush 大概率是白写（被 flush 的行往往在下次访问前
就被驱逐，届时还要再 flush 一次），且挤占前台 IO 的 NAND 带宽。

**持久性不受影响**（关键核对）：L2P 更新在写完成路径里**先 `journal_append` 再 `l2p_cache_write`**
⇒ 掉电可回放；`checkpoint_create` 也无条件 flush 全部脏 region（`checkpoint_flush_dirty_regions`）。
故「脏行滞留 RAM」不比原设计多任何丢失窗口。

### 刻意不实现（初稿策略 1 / 2）

| 策略 | 不实现的原因 |
|---|---|
| **write-through** | 本仓 L2P 写模型是**整 region 页追加**（改 1 个 entry 也要写回整页 2KB）⇒ 无论 write-back 还是 write-through 都要先把 region **读进来**（RMW）再写一次，write-through **省不掉那次 load**，却放弃 write-back 的**合并收益**（同 region 多次写合并为一次 flush）⇒ 净亏。 |
| **增大 batch / 预取连续 region** | 预取依赖**空间局部性**，而 thrashing 的成因恰恰是**随机**访问（无局部性）⇒ 预取只多读一页。预取属于**顺序**场景优化，与 thrashing 正交。 |

### 判据核对

原判据「随机写压测下不出现『evict→flush→load』活锁」：该活锁由**写路径紧急 GC**（v0.33）结构性
保证（分配不到块时同步 GC 兜底）；本轮降级是**降本**（少白写、让带宽）而非防活锁 ⇒ 该判据属上板
压力观测，**非纯 RAM UT 可判**。

### 登记不做

- **读路径查 Journal overlay**（DRAM-less 行业解法之一：Journal 即 L2P 增量日志，thrashing 下少
  load region）——本仓 `l2p_cache_read` 不查 Journal，属独立改造。
- **L0 覆盖 thrashing 窗口**：结算+滞回是纯逻辑，但当前是 `l2p_cache.c` 的 `static` 内部函数；
  L0 要测需先把结算步导出成公开原语——属测试接线决策（实施时再定）。

### 验证

- `make clean && make all` 182872 B（v0.52 的 182336 B + 536 B）；`make clean && make tests` 260988 B
  （v0.52 的 260452 B + 536 B）；零告警零错误；`make format-check` 通过
- **不新增任何测试源**（平台规则：测试源用户自建）
- **上板验证未做**（按项目规则由用户执行）；无介质格式变更 ⇒ 无需重新擦除芯片

## v0.54 — P1-6 Journal 寿命瓶颈（环形块双倍擦除修复 + 条目合并）

**日期**：2026-10-12

**状态**：✅ 代码已落地、**未上板验证**（`make clean && make all` 182992 B / `make clean && make tests`
261108 B，均零告警零错误；`make format-check` 通过）

### 前提核对（两条过期，一条成立）

计划书 P1-6 的两条方向，逐条对照**本仓实现**后**两条都已不成立**：

| 计划书方向 | 核对结果 |
|---|---|
| **增 `JOURNAL_BLOCKS`（2→4/8）** | ❌ **过期**——实现早已是 4 块（`JOURNAL_BLOCKS = CW_NUM = 4`，块 58-61），设计自荐的「2→4」本就落地 ⇒ 寿命 ~1.6 TB。文档 §14.1 的「2 块 ≈ 800GB」为初稿值，本版一并更正。 |
| **region 更新合并**（同 region 多次写合并为一次 flush）| ❌ **本就固有**——region cache 把同一 region 的多次写合并进**一条脏行**，只 flush 一次；无需改动。 |

⇒ 真缺陷另在**环形块擦除路径**（见下）。

### 真缺陷：环形块双倍擦除（P1-6 实修）

`journal_flush_page` 在写满一块回切下一块时**擦除新块**；而 `journal_truncate`
（由 `checkpoint_create` 调用）此前也擦除「环形下一块」。两者**擦的是同一个块**：

```
一圈内同一块被擦两次：
  truncate   擦 block N+1  ──┐
  flush_page 回切 block N+1 ──┴─ 又擦一次 ⇒ 双倍磨损
```

直击 §14.1 认定的 **Journal 寿命瓶颈**——Journal 磨损凭空 **+25%**（每圈多擦一块）。

**修法**：块的物理擦除**收敛到唯一一点**——`journal_flush_page` 回切进入新块时擦。

- `journal_truncate` 只做**预擦除 + 记账**：擦环形下一块，置 `pre_erased_block = next`。
- `journal_flush_page` 回切时，若 `pre_erased_block == current_block` ⇒ **跳过擦除**并清记账
  （`JOURNAL_BLOCK_NONE`），否则按原路径擦。
- 重复调用（两次 checkpoint 之间无写入）时 `next == pre_erased_block` ⇒ 跳过重擦。
- 新增哨兵 `JOURNAL_BLOCK_NONE (0xFFU)`；`l2p_journal_t` 新增字段 `uint8_t pre_erased_block`。

### `up_to_sequence` 生效（不再是死形参）

旧 `journal_truncate` 忽略 `up_to_sequence`。新实现加**水位校验**：若
`sequence_counter == 0` 或 `(up_to_sequence + 1) < sequence_counter`（即水位**不足以**覆盖全部已写
条目），**直接返回不截断**——最旧块可能仍持有回放需要的条目，擦掉即丢数据。调用者
`checkpoint_create` 传的正是 `sequence_counter`（下一待分配序号），正常路径恒满足。

### 条目合并（`journal_append`）

页缓冲里若已存在**同一 LBA** 的条目 ⇒ **就地更新** `new_pba`，不新增条目、**不消耗 sequence**、
**不推进 `current_entry`**。理由：回放按页序 + 页内下标应用（per-LBA last-write-wins），页内同 LBA
只保留最新 `new_pba` 即正确（`old_pba` 仅首条保留，回放不读它）。⚠ 必须**同时**不推进
`sequence_counter`——页头 `hdr_page_seq = sequence_counter − current_entry` 由二者反推，只推一个会
导致页序错乱、回放顺序错误。

### 判据核对

- 原判据「寿命估算翻倍」：已由「4 块」满足（初稿 2 块 ⇒ 800 GB，4 块 ⇒ ~1.6 TB）。
- 原判据「写压测下 journal 水位不长期贴顶」：属**上板压力观测**，非纯 RAM UT 可判。
- ⚠ **无介质格式变更**（Journal 块位置/块数/页头/条目格式**均未变**）⇒ **无需重新擦除芯片**。

### 验证

- `make clean && make all` 182992 B（v0.53 的 182872 B + 120 B）；`make clean && make tests`
  261108 B（v0.53 的 260988 B + 120 B）；零告警零错误；`make format-check` 通过
- **不新增任何测试源**（平台规则：测试源用户自建）
- **上板验证未做**（按项目规则由用户执行）

---

## v0.55 — 逐页有效位图搬出 BST 条目（位图与条目的分离）

**日期**：2026-10-12

**状态**：✅ 代码已落地、**未上板验证**（`make clean && make all` 183368 B / `make clean && make tests`
261476 B，均零告警零错误）

### 判据换向（本版的出发点）

用户明确：**判据换成「有出处的主流存储主控 FTL 设计」，不是本仓当前方案**——本仓
`ftl_design.md §19（档三）/§4.9.3（厂商对照表）` 是**待改对象**。故本版先核主流 BIT 设计，
再据其改本仓。已核到的**一手出处**：KIISE《Study of Effective Valid Page Tracking Methods
in Mobile Flash Storage without DRAM》(`jok.kiise.or.kr/pub-reader/1847`)，逐字：

- VPC = **每块 4 字节**，且「블록 당 4B만 사용되어 … 충분히 SRAM에서 관리할 수 있다」（小到能常驻）。
- 有效页位图 = **1 bit/4KB 页**；DRAM-less 下**留 SRAM 装不下** ⇒ **退到 flash 里管**
  （「유효 페이지 비트맵을 플래시 메모리에서 관리할 수 있지만」，代价是频繁 flash I/O）。

⇒ 主流的分界**不是「u64 还是 u8」**，而是**「位图是不是 per-block 条目里常驻的一个字段」**：
主流把**块级聚合（VPC）留常驻**、把**逐页位图当独立、可落盘、可 GC 重建的弹性项**。

⚠ **撤回**：本版讨论中一度引用的「主流把擦除次数/无效页/状态打包进 32-bit bitfield」**查不到一手出处**
（OFTL 的 20+10+2 位域布局全网不可检索），按项目规矩**不作依据**。KIISE 的一手值恰恰是
VPC = 4B（整 u32），**不是**打包字段。

### 变更：`valid_map` 从 `ftl_block_t` 搬为独立表

| 项 | v0.48~v0.54 | v0.55 |
|---|---|---|
| BIT 条目 | `ftl_block_t` **40B**（含 `valid_map[PLANE_NUM]` 16B + `_pad` 1B） | `ftl_block_t` **24B**（无位图） |
| 位图 | 条目内字段 | **独立表** `alloc->valid_map[gblock]`（每块 `PLANE_NUM` 个 u64 = 16B） |
| 访问器 | `ftl_block_set_valid(b, plane, page)` | `ftl_block_set_valid(b, valid_map, plane, page)`（显式收本块切片） |
| 承载 | `g_blocks[4096]` 160KB | `g_blocks[4096]` 96KB + `g_valid_map[4096][2]` 64KB = 160KB |

**内存不变**（160KB）——本变更是**结构性**的：条目不再携带位图，两者可独立驻留/搬移；
`valid_count`（块级聚合）仍是条目里的**真值**，位图与它由访问器单点同步。

**顺带揭示的隐藏前提**：v0.48 记的「删死字段省 0 字节」（`ftl_design.md §4.9.6`）**只对
`valid_map = uint64_t[PLANE_NUM]` 成立**——它的 8 字节对齐把省下的 4B 又吃回 padding。
位图搬出后对齐约束消失，条目自然落到 24B（96KB）。⇒ **「对齐要求」才是那个隐藏前提**。
（死字段 `program_count`/`bad_reason`/`gc_generation` **本次未删**，仍按 v0.48 的「暂缓」保留；
**v0.56 已删**，见下一条。）

### 改动文件

- `include/ftl/bst.h`：结构体去 `valid_map`/`_pad`；`_Static_assert` 40→**24**；四个访问器加
  位图切片形参；文件头加「位图与条目的分离」说明。
- `include/ftl/allocator.h`：`ftl_init_params_t` 与 `ftl_allocator_t` 各加
  `uint64_t (*valid_map)[PLANE_NUM]`。
- `src/main.c`：新增 `g_valid_map[CH][CE][LUN][FTL_BLOCK_TOTAL][PLANE_NUM]`（64KB → AXI_RAM）；
  AXI 预算 `_Static_assert` 计入它。
- `src/ftl/{ftl,allocator,gc,wear_leveling,bbt,power_failure_recovery}.c`：15 个访问器调用点
  + 2 个位图扫描循环（`gc.c`/`wear_leveling.c` 的 `__builtin_ctzll` 扫块）改取独立表；
  `ftl_init` 加位图表 memset + NULL 校验。
- `tests/unit/test_rebuild.c`：夹具加 `valid_map` 数组与 `params.valid_map`（结构体变更的**编译必需**
  适配，非新增测试）。
- 文档：`ftl_design.md §4.9`（§4.9.2 结构、新增 §4.9.7、§19 标注）、`ftl_optimization_plan.md`。

### 验证

- `make clean && make all` 183368 B（v0.54 的 182992 B + 376 B）；`make clean && make tests`
  261476 B（v0.54 的 261108 B + 368 B）；零告警零错误
- AXI_RAM 332.4 KB / 384 KB（86.6%）
- ⚠ **无介质格式变更**（位图从不落盘，掉电由写指针扫描重建）⇒ **无需重新擦除芯片**
- **不新增任何测试源**（平台规则：测试源用户自建）
- **上板验证未做**（按项目规则由用户执行）

---

## v0.56 — 删 BST 死字段（条目 24B → 20B；Wear 条目 12B → 8B）

**日期**：2026-10-12
**性质**：结构体瘦身 + 接口收敛（**无 RAM 之外的介质格式变更**）

### 动机

v0.55 把逐页位图搬出条目后，揭示了 v0.48 那条「删死字段省 0 字节」的**隐藏前提**——
省下的 4B 原本被 `valid_map = uint64_t[PLANE_NUM]` 的 8 字节对齐吃回 padding。
对齐约束消失后，删除**真的省字节**。用户据此拍板「删掉吧」。

### 删除清单

| 字段 | 大小 | 依据 |
|---|---|---|
| `program_count` | 2B | 写成功++，全仓**零读者**（v0.45 曾设想做健康模型，从未落地） |
| `bad_reason` | 1B | 仅 `bbt_mark_bad` 写入，零读者；BBT 持久化本就是 bitmap，原因跨重启必丢 |
| `gc_generation` | 1B | GC 迁移成功++，零读者 |

### 连带撤除

| 项 | 变化 |
|---|---|
| `FTL_BLOCK_BAD_*` 枚举 | 整删（6 常量，唯一用途是喂 `bad_reason`） |
| `bbt_mark_bad()` | `(alloc, gblock, reason)` → `(alloc, gblock)`；4 处调用点（`gc.c` ×1、`ftl.c` ×2、内部 ×3） |
| `ftl_block_bad_reason()` / `ftl_block_set_bad_reason()` | 整删 |
| Wear 条目 `wear_entry_t` | 去 `program_count` + `_reserved`（对齐填充随之不再需要）⇒ **12B → 8B**；`FTL_WEAR_ENTRY_BYTES` 12→8；全量快照 **25 → 17 页** |
| `wear_mark_dirty` 脏标记点 | **7 → 5**（删 `ftl.c` 写完成的两点，它们只为 `program_count` 服务） |

### 保留未动

`OPEN` / `RESERVED` / `TEMP_HOT` / `TEMP_COLD` 状态位——它们是**状态机语义**的一部分
（非独立字段），且 `tests/unit/test_rebuild.c` 直接断言，另案处理。

### 改动文件

- `include/ftl/bst.h`：结构体去 3 字段、`_Static_assert` 24→**20**、删 `FTL_BLOCK_BAD_*` 枚举
  与两个 `bad_reason` 访问器、文件头维护点注释同步
- `include/ftl/bbt.h` / `src/ftl/bbt.c`：`bbt_mark_bad` 去 `reason` 形参（3 处内部调用点同步）
- `src/ftl/{gc,ftl}.c`：`bbt_mark_bad` 调用点去实参；删 `gc_generation++`、
  `program_count++` ×2 及其 `wear_mark_dirty` ×2
- `include/ftl/wear.h` / `src/ftl/wear.c` / `include/ftl/ftl_config.h`：条目去两槽、常量 12→8
- 文档：`ftl_design.md`（§4.9.2/§4.9.3/§4.9.4/§4.9.6、新增 **§4.9.8**、§19 全节）、
  `ftl_optimization_plan.md`、`cw_level_mapping_design.md`、`ftl_api_usage.md`、`ftl_ut_munit.md`

### 验证

- `make clean && make all` **182992 B**（v0.55 的 183368 B − 376）；`make clean && make tests`
  **261100 B**（v0.55 的 261476 B − 376）；零告警零错误；`make format-check` 通过
- AXI_RAM **316.4 KB** / 384 KB（82.4%；v0.55 为 332.4 KB）——**条目 96KB → 80KB（−16KB）**，
  位图表 64KB 不变
- ⚠ **Wear 表条目格式变更**（12B → 8B）。P0-1 尚未上板 ⇒ 芯片上无旧格式数据；
  且上板前本就须擦除芯片（P0-2 布局维度无关化）⇒ **无需额外处理**
- **不新增任何测试源**（平台规则：测试源用户自建）
- **上板验证未做**（按项目规则由用户执行）

---

## v0.57 — P2-2 结案（SB 索引粒度：前提被 Policy B 作废）

**性质**：**纯文档决策**，无代码改动（结论是「不实现」）。

### 动机

`ftl_optimization_plan.md` §P2-2 登记着「按 SB 索引可再省 96KB」。复核发现该推导的
**前提已被删除**，且删除后**没有任何可省的空间**。

### 依据

**① 前提已死（Policy B）。** 原推导的前提是 `allocator.c` 的 `block1 = block0`：SB 两 plane
永远配对**同号物理块** ⇒ 两 plane 的 `erase_count`/`state`/`sb_id`/`valid_count` 天然相同 ⇒
per-block 存是重复的。**v0.12 Policy B**（`cc09580`+`1927762`）已把前提删掉——`allocator_open_sb`
（`allocator.c:159-189`）**逐 plane 独立**调 `allocator_select_plane_block(alloc, plane)`。于是
`erase_count`/`last_pe_ts`/`read_count`/`valid_count`/`state`/`sb_id`/`rr` **各自属于不同物理块，
无一重复** ⇒ 没有东西可去重。

**② 数字双重过期。** 备注写「再省 96KB」；当前条目 20B（v0.56）× 4096 = **80KB**，且按 SB 索引
（2048 条）只是 80KB → 40KB——省下的是**分辨率**而非重复。

**③ 主流方向相反（两级结构）。** SPDK FTL（`docs/reference/pages/spdk_ftl.html`，本仓一手出处）
是**两级**：band 级记 sequence ID / validity / write pointer / OPEN-FREE-CLOSE，底层仍是
**per-block BIT**（"Valid map - bitmask of all the valid physical addresses"）。本仓
`super_block_t`（SB 级）+ `ftl_block_t`（block 级）**逐项对应**，已对齐。改成按 SB 索引
= **删掉 block 级 BIT**，是往主流反方向走。

**④ 「删 `sb_id` 省 8KB」一并撤回。** 删掉 2B 的 `sb_id` 后 `ftl_block_t` **仍是 20B**
（4+4+2+2+2+3 = 17B → u32 对齐 20B）⇒ **省 0 字节**。这与 v0.48「删死字段省 0 字节」是**同一个
对齐陷阱**（设计 §4.9.6）。且主流 DRAM 控制器**有** block→band 反查表。要真省字节只能砍
**活**字段（`rr` 3B / `last_pe_ts` 4B），两者都有消费者（慢死预测 / GC youth penalty）⇒ 不砍。

### 连带

- 新增登记 **P2-4**（`ftl_optimization_plan.md`）：band 级 **sequence ID + validity 聚合** 是本仓
  对 SPDK 的**唯一剩余偏离**。**不实现**的理由：SPDK 的 sequence ID 唯一消费者是「按 band 序号
  重放重建 L2P」，而本仓恢复走 **Journal/Checkpoint 的 `page_seq` 排序**
  （`power_failure_recovery.c`，粒度是日志页、比 band 级更精确）⇒ 消费者被取代，加了即死字段
  （v0.56 教训）。GC victim 侧 SPDK 用 **validity ratio**（"The lower the ratio, the higher the
  chance the band will be chosen"），本仓 `GC_WEIGHT_VALID` 已对齐。
- `ftl_design.md` §4.9.5 选项 C 的否决理由中，「两 plane 同号 ⇒ 可去重」一条已失效，剩下
  「擦除/坏块/ECC 是物理块级事实」——**更硬**且独立成立。

### 改动文件

- `docs/design/ftl_optimization_plan.md`：§P2-2 重写为结案 + 新增 §P2-4
- `docs/design/ftl_revision_history.md`：v0.48 备注加结案指针 + 本条目
- `docs/design/ftl_design.md`：§4.9.5 选项 C 加订正注

### 验证

- **无代码改动** ⇒ 无 `.bin` 变化、无介质变更、不新增测试源
- 结论以文档内可核对的事实为准（`allocator.c:159-189`、`bst.h:121-138` 的 `_Static_assert`）

---

## v0.58 — FTL 对外接口对齐主流（plane 策略下沉 + 组完成语义）

**日期**：2026-10-13

**性质**：**接口重构 + 文档**（策略下沉，行为变更：大请求从「失败」变「成功」）。设计依据见
新增文档 `docs/design/ftl_host_interface.md`（含 D1–D6 差异表与接口级 UT 设计）。

### 动机

学**主流存储主控的 FTL 对外接口设计**（本仓北极星：`fc_nand_design.md` §26 行业对照，非 Linux）。
问题：「FTL 对外接口是什么、外部传什么给 FTL、当前架构有何差异、如何设计 UT」。

### 依据（判据 = 本仓 §26.1/§26.2）

**① 「访问哪些物理单元」是 FTL 的事。** §26.1 原文：设备侧固件拿到的是一条**已解析的请求**
（目标 LBA/地址 + 数据缓冲指针 + 操作类型），**访问哪些物理单元、数据怎么搬都是固件的事**。
**multi-plane 配对正是「访问哪些物理单元」** ⇒ 属 FTL，不属 HIL。而 v0.58 前，配对逻辑在
`controller/request.c` 的 `ssd_split_use_multi_plane`（HIL 侧）——**位置错**。

**② 主流传的是「长度」不是「plane 数」。** NVMe 的 NLB、eMMC CMDQ 的块数都是**长度**；FTL 自己
决定怎么拆。本仓旧 `ftl_io_request_t.pages`（1/2）把 FTL 的内部策略泄漏给了上层。

### 关键决策

| # | 项 | 主流 | v0.58 前（本仓） | v0.58 处置 |
|---|---|---|---|---|
| **D1** | plane 配对 | FTL 内部策略 | HIL 的 `ssd_split_use_multi_plane` 决定 `pages=2` | **下沉到 FTL**（`ftl_issue_subop` 按 `count` 自拆） |
| **D2** | FLUSH | FTL 的一条 op | HIL 直接调 FTL 内部 `l2p_cache_flush_all`/`journal_flush_page` | **新增 `ftl_flush(ftl)`** |
| **D3** | TRIM 粒度 | 带长度的 deallocate | `ftl_trim(ftl,lba)` 单 LBA（忽略 `block_count`） | **改 `ftl_trim(ftl,lba,count)`** |
| **D4** | 请求 vs 通知 | 请求纯输入；完成走 CQ | 通知字段与请求混在一起 | **不改**（本仓无 CQ，轮询+回调是既定异步契约） |
| **D5** | Host 队列+doorbell+CQ | HIL 有 | HIL 无队列；队列机制只在 FC 内部 | **不改**（§26.2 已定位在 FC；HIL 侧属更大重构） |
| **D6** | LBA→实例路由 | 单一 FTL 拥有全 LBA | Controller 按 LBA 路由到 `ftl[ch][ce][lun]` 分片 | **保留**（分片是刻意模型；路由=后端选择，非 plane 策略） |

**③ 组完成语义（一次 submit = 一个组）。** 长度任意后，一次 `ftl_submit_*` 内部含多个 FC 子操作，
通知字段（`complete_flag`/`status_out`/`on_complete`）必须**只在全部子操作完成后触发一次**。
新增 `ftl_io_group_t`（`ftl->groups[FTL_GROUP_MAX]`，`FTL_GROUP_MAX = FTL_PENDING_MAX = 32`）承载
聚合；`ftl_pending_req_t` **不再内嵌 `ftl_io_request_t io`**，改显式子操作字段 + `group`/`planes`
（否则 `count`（host run 长）与 `planes`（子操作 plane 数）会共用一个字段、产生静默二义）。

**④ 三态返回是枢纽。** `ftl_issue_subop` 返回 `FTL_OK`（已下发/同步完成）/ `FTL_AGAIN`（瞬态背压：
命令/desc/buffer 池满、SQ 满、pending 表满）/ `FTL_ERROR`（永久）。旧实现把三类**全当致命**，
正是「大请求在第 N 个子操作崩」的根因。背压排队由 `ftl_service` **Phase 2.5** 补发
（完成先腾出 FC 资源 → 再补 host 子操作 → 后台吃剩下的）；连续无进展超
`FTL_DRAIN_STALL_MAX`（8 轮）才判卡死。

### 顺带修掉的两个真缺陷

- **① 大请求提前完成**：旧 `ssd_request_split` 同步循环提交，`ftl_submit_*` 中途失败时置
  `complete_flag=1`，而 `sub_remaining` 已被预置为全量 ⇒ **父请求提前完成、余下计数永不归零**。
  真实容量瓶颈是 **FC buffer 池**（128 buffer ÷ 每多 plane 命令 8 buffer = **16** 并发子操作），
  `wr 0 40` 旧实现必失败。v0.58 后 FTL 自拆分 + 按需 drain，瞬态背压排队重试。
- **② CLI 谎报缓冲容量**：`cli.c` 把 `buffer_size` 设为 `n × FTL_READ_USER_SIZE`，而 `g_wr_buf`
  实为 16 KB ⇒ `wr 0 40` 会**写爆 16 KB 数组**（内存损坏）。改为上报真实 `sizeof(g_wr_buf)`，
  由 Controller 的容量校验拒绝（`block_count × FTL_IU_BYTES(512) > buffer_size` ⇒ `INVALID_ARGUMENT`）。
  （⚠ 此处原写 `× 2048`：CW 级迁移（v0.59）后步进单位由页 2048B 改为 IU 512B。）

### 连带（登记，本轮不做）

- **D4/D5**：HIL 侧 host 队列 + doorbell + CQ（更大重构）。
- **D6**：LBA→实例路由上移到单一 FTL 前端（与本仓分片模型冲突，需单独立项）。
- `main.c` 目前只对 `g_ftl[0][0][0]` 调 `ftl_service` ⇒ Phase 2.5 只 drain 实例 0。当前几何
  （CH=CE=LUN=1）正确；维度扩展时须遍历全部实例（**既有结构缺口**，本步使其显形）。
- `ftl_trim` 大范围同步阻塞（超循环长停）——本轮只加 API，范围上界/分块后续定。
- `ftl_flush` 语义 = 「落已持久化映射」，**非** NVMe 式 barrier（不等待在飞/排队写）。
- 跨组 LBA 重叠时组间写序未定义（当前 HIL 串行，不成问题）。
- **保留未改**：`ftl_handle_completion` 读失败分支**无条件**报 `IO_ERROR`——即使 read recovery
  实际救回数据（S1 前既有行为，按「行为等价」原则原样保留 + ⚠ 注释，待单独立项）。

### 改动文件

- `include/common/ftl_types.h`：`pages` → `count`
- `include/ftl/ftl_type.h`：新增 `FTL_AGAIN`
- `include/ftl/pending.h` / `src/ftl/pending.c`：`ftl_io_group_t` + 组管理；pending 改显式子操作字段
  + `group`/`planes`；删 `pending_lookup_by_lba`（零调用者死代码）
- `include/ftl/ftl.h`：`ftl_flush` / `ftl_trim(count)` / `FTL_DRAIN_STALL_MAX` / `groups[]` 进 context
- `src/ftl/ftl.c`：`ftl_issue_subop`（三态）/ `ftl_drain_group` / Phase 2.5 / 组完成；IDLE 判据改用未决数
- `src/ftl/ftl_fc_interface.c`：`ftl_cmd_setup_ppa_pair` desc 配置失败区分 `FTL_AGAIN`/`FTL_ERROR`
- `src/controller/request.c`：删 plane 配对，改按实例边界切 run 提交
- `src/controller/controller.c`：统一提交路径；FLUSH 走 `ftl_flush`；去 l2p/journal 直依赖；buffer_size 校验
- `src/controller/cli.c`：`buffer_size` 上报真实容量
- `docs/design/ftl_host_interface.md`（新增）/ `ftl_api_usage.md` / `ftl_ut_munit.md`（§8.6 L1 接口套件）

### 验证

- `make clean && make all` / `make clean && make tests`：**零错误零新增警告**（`184304 B` / `262420 B`）
- **等价性判据**：单实例、`count ≤ 2` 路径下提交的 FC 命令序列应与 v0.57 逐命令一致
- 上板（UART CLI）：`wr 0 5 / rd 0 5 c`（走 2+2+1 自拆）、`wr 0 2 / rd 0 2 c`、`tr 0 / fl`
- ⚠ CLI 缓冲仅 16 KB ⇒ `n ≤ 32`（CW 级迁移后步进 512B，`UART_CLI_MAX_SECTORS = 16KB/512`）；
  验证更大请求需先扩 `g_wr_buf`。（v0.58 当时为页级 2048B ⇒ `n ≤ 8`。）

---

## v0.59 — CW 级映射迁移（IU = 512 B；`ftl_optimization_plan.md §九` 波 2 序 2）

**日期**：2026-10-13

**性质**：**数据路径由页级改为 IU 级**——映射粒度从「页 2048 B」改到「CW 512 B」
（`FTL_IU_BYTES = FC_BUFFER_USER_SIZE = 512`，1 LBA = 1 CW = page/4）。**介质格式变更**。

> 本文档此前 v0.58 → v0.60 断档，本条目补齐（v0.60/v0.61 多处以此迁移为前提）。
> 完整设计见 `cw_level_mapping_design.md`（§八 有效位粒度、§十一 落地状态表）。

### 动机

判据 = **有出处的主流存储主控 FTL 设计**（本仓北极星）。主流主控的 **indirection unit（IU）**
普遍小于物理页（SPDK FTL 的 IU 可配、Cosmos+ 亦然），IU = 映射粒度 = 有效位粒度。本仓此前
「页级映射 + 页级 2048 B/LBA」与主流不同构，且 **L2P 表声明空间（`FTL_LBA_MAX`）超容量 4×**
（伪缺陷的根因是步进仍按页）。

### 改动（代码）

| 层 | 改动 | 文件 |
|---|---|---|
| 地址辅助 | `ftl_lba_cw/plane/page/pair_base`、`pba_page`、`ftl_cw_span_mask` | `include/ftl/ftl_config.h` §八 |
| 请求层 | 缓冲步进 = `FTL_IU_BYTES`（512 B/扇区）；`io.count` = 扇区数 | `src/controller/request.c`、`controller.c` |
| 写路径 | 1 子操作 → 逐 CW 一条 L2P entry + 一条 journal；`allocator_alloc_pair` 成对取页 | `src/ftl/ftl.c` |
| 读路径 | 逐 LBA 查 L2P，按物理 plane 分组；pair 窗口内至多两 plane 段 | `src/ftl/ftl.c` |
| GC / WL | 逐页索引成对分配目标页；逐 CW L2P 反查判有效；`ftl_sync_write_page_cw` 只编有效 CW | `src/ftl/gc.c`、`src/ftl/wear_leveling.c` |
| BST | `valid_count` 改 per-CW（0..512） | `include/ftl/bst.h` |
| 掉电重建 | `close_full_block` 逐 CW L2P 反查回填；`valid_pages`→`valid_cw` | `src/ftl/power_failure_recovery.c` |
| 页基址反推 | `gc_read_page_with_lba` 由**首个写过的 CW** 反推页基址（容忍页内空洞） | `src/ftl/gc.c` |
| CLI | `g_wr_buf` 16 KB ⇒ `wr/rd n ≤ 32` 扇区（旧页级上限 8） | `src/controller/cli.c` |

### 关键决策

- **不设页缓冲**（`cw_level_mapping_design.md §七` 方案 A）：FC 层有 CW 位图能力（FC v27），
  512 B 写**直接编 CW 子集**，**页内允许空洞**（CW0 可能未写）。
- **不做 per-CW 位图**：`valid_count` 改 per-CW 但内存未增（v0.60 索性切 approach ③，见下）。
- **GC/WL 目标页必须成对同 plane**；**修真 bug**：页基址由首个写过 CW 反推（CW0 可能是空洞）。

### 代价 / 登记

- **单 plane 子操作浪费另一 plane 同页** ⇒ 登记 `ftl_optimization_plan.md §8.1` **P2-6**
  （修法 = per-plane 独立写指针）。
- **上板前须擦芯片**（介质格式变更）——后被 **v0.61 的开机自动格式化**取代。

### 验证

`make clean && make all` 0 warning；**未上板**。声明地址空间回到 `1,048,576 × 512 B = 512 MiB`。

---

## v0.60 — 有效性切 DRAM-less approach ③（删常驻位图）

**日期**：2026-10-13

**性质**：**结构删除 + 判据换源**（无介质格式变更；行为等价，仅少一次「跳过未写页」的优化）。
前置 = v0.59/v0.58 的 CW 级映射迁移（波 2 序 2）已落地。

### 动机

用户决策（2026-10-13）：**有效位（validity）按 DRAM-less approach ③ = flash reverse mapping 实现**
——**读每个 CW 的 META 取逻辑地址**（该 META 即本仓的「OOB」），再**反查 L2P** 判该 CW 是否仍有效。

判据 = **有出处的主流存储主控 FTL 设计**（本仓北极星，见 `cw_level_mapping_design.md §八`）：

- **DFTL** — OOB 存 LBA；GC 读 OOB → 查 L2P。
- **arXiv 1504.01666**（*Garbage Collection for Flash-Resident Page-Mapping FTLs*）— 活页识别三法：
  ① 页映射扫描 ② PVB 逐页位图 ③ **flash reverse-mapping**。DRAM-less 选 **③**（② 的 RAM ∝ 页数：
  Micron P420m 要 **16 MB**）。
- **KIISE**（*Mobile Flash Storage without DRAM*）— 逐页位图放 flash 后，失效时清旧物理页的位
  会引发大量 flash I/O（旧页散落在任意块里）⇒ 主流**不做**常驻逐页位图。
- **LeaFTL / VPFTL** — P2L 存 OOB/spare，GC 读回并与 L2P **交叉验证**（匹配 = 活）。
- **SPDK FTL** — `valid_map` 按**物理地址**索引 = **有 DRAM** 的形态（1 bit / 映射单元）；
  其索引粒度佐证「有效位粒度 = 映射粒度」，但**驻留**前提是有 DRAM，本仓不具备。

### 现状核查（改动前）

本仓有效性**已经**是 approach ③：`gc_read_page_with_lba` 读每个 CW 的 META（校验低位 plane/cw
自洽 + `0xFFFFFFFF` 哨兵）；`gc_migrate_sb` / `wl_relocate` / `close_full_block` **逐 CW 反查 L2P**
（`l2p_cache_read(l) == pba_with_cw(src, cw)`）判有效。META 就是本仓的 OOB 反向映射
（`[META:4][CRC:4][USER:512][PARITY:8]`，`include/fc/fc.h`）。

**唯一非 DRAM-less 的残留 = 常驻 64 KB 页级「写过」位图** `g_valid_map`：语义只是「本页被编程过」
（**单调、非真值**），**唯一用途** = 在 `gc_migrate_sb` 与 `wl_relocate` 跳过从未写过的页的那次 NAND 读。

### 关键决策

| # | 项 | 决策 |
|---|---|---|
| **A** | 常驻位图 | **删**（`g_valid_map` 64KB、`ftl_init_params_t.valid_map`、`ftl_allocator_t.valid_map`） |
| **B** | 「写过」访问器 | **删** `ftl_block_mark_written` / `ftl_block_is_written`（含 5 处写调用点 + 2 处读调用点） |
| **C** | `ftl_block_clear_valid` | 去位图形参 ⇒ 单参（`gc.c` 擦除、`bbt.c` 标坏块两处同步） |
| **D** | GC/WL 跳过未写页 | 改为**快路径**：`gc_migrate_sb` 入口判「双 plane `valid_count` 之和 == 0」⇒ 直接返回（调用方仍会 `gc_erase_and_free`，语义不变）。这正是位图原先覆盖的主场景——**P2-6 整块空洞 plane**（2048B 写只落 plane0） |
| **E** | 逐 CW LBA 来源 | `gc_read_page_with_lba` 签名改为**输出逐 CW 的扇区 LBA**（`lba_t cw_lba[CW_NUM]`，未写过 = `FTL_LBA_INVALID`）⇒ GC/WL/掉电重建**直接用每个 CW 自己的 META**，不再做 `base + cw` 推断（消除对「同页 CW 的 META 恒为 B+c」不变量的隐式依赖） |

### 代价（诚实登记）

- **散点空洞**（同块内既满页写又 512B 写）的页仍会多读一次 NAND；**整块空洞**由 D 的快路径覆盖。
  登记于 `ftl_optimization_plan.md §8.1 P2-6`。
- 收益：AXI_RAM **319 → 255.0 KB**（余 ~129 KB，为波 2 序 3/4 表所有权 / 多实例化腾空间）；
  同时消除「位图提示 vs `valid_count` 计数」两者漂移的隐患（TRIM 只减计数不清位、裸 NAND 扫描只置位不计数）。

### 改动文件

- `include/ftl/bst.h`（删两访问器 + `clear_valid` 去参 + 文件头/结构注释改口径）
- `include/ftl/allocator.h` / `src/ftl/allocator.c`（删 `valid_map` 字段与赋值）
- `src/main.c`（删 `g_valid_map` + `_Static_assert` 项 + `params.valid_map`）
- `src/ftl/ftl.c`（删 `mark_written` 调用、`valid_map` NULL 检查与 `memset`）
- `src/ftl/gc.c` / `include/ftl/gc.h`（签名改逐 CW 输出 + 快路径 + 去 is_written 守卫）
- `src/ftl/wear_leveling.c`（同上）
- `src/ftl/bbt.c`（`clear_valid` 单参）
- `src/ftl/power_failure_recovery.c` / `include/ftl/power_failure_recovery.h`（去 `mark_written`，逐 CW LBA 回填）
- `docs/design/cw_level_mapping_design.md §八/§十一/§十二.2`、`ftl_optimization_plan.md §8.1/§8.2/§九 序 2.5`、`ftl_design.md §2.3/§4.9.*/§19`

### 验证

- `make clean && make all`：**零告警**；AXI_RAM **255.0 / 384 KB**（66.4%）
- 静态：`grep -rn "valid_map\|mark_written\|is_written" src/ include/` 零命中
- ⚠ `tests/unit/test_rebuild.c` 绑定了 `params.valid_map` ⇒ **删字段后该文件不再编译**
  （测试源文件由用户自建，本变更不代改，需用户更新后 `make tests` 方可运行）
- 上板：**先擦除芯片**（工作树含 CW 迁移的介质格式变更）→ 烧 `debug.bin` → UART `test` 全绿

---

## v0.61 — 介质布局版本标记 + 开机自动格式化（顺带修 Anchor 页 CRC 覆盖面缺陷）

**日期**：2026-10-13

**性质**：**新增持久化字段（介质格式变更）+ 真缺陷修复**。本改动本身即改介质格式 ⇒
首次上板必然触发一次自动格式化（符合预期，见下）。

### 动机

CW 级映射迁移（波 2 序 2）与 v0.50~v0.60 的一连串元数据改造，**多次改变了 NAND 上的持久化
格式**（L2P 块位置、Journal 块数、Wear 块、Anchor 分页、CW 级映射）。固件**没有任何机制识别
"介质是旧格式"**：直接烧板会在旧数据上启动，而 **Free Pool 里全是未擦的旧块**——allocator
**不做擦除**（"free pool 保证已擦除" 是隐式前提）⇒ 写下去 = 新数据 AND 旧数据 = **静默损坏**。
用户每次上板都得先手动 `bbt --erase` 再断电重启。本版把这个手动步骤变成固件行为。

**用户决策（2026-10-13）**：
1. 做法 = **加版本标记 + 自动格式化**；
2. 版本标记放 **Checkpoint 头 + Anchor 页都加**（两处独立校验）；
3. 顺带查出的 **Anchor 页 CRC 覆盖面缺陷** = **本次一并修**；
4. 自动格式化**保留出厂坏块标记**。

### 顺带修复的真缺陷：Anchor 页 CRC 覆盖面

`src/ftl/metadata.c` 的 `ANCHOR_PAGE_CRC_LEN` 原为
`sizeof(metadata_anchor_page_t) - sizeof(uint32_t)` = **2044**，但 `crc` 字段实际在
**offset 1808**（其后还有 236 B padding）⇒ CRC 覆盖面把 `crc` **自己**也算进去 ⇒
写入时（crc=0）算出的值与读回时（crc≠0）算出的值**必然不等** ⇒ **`anchor_page_valid`
恒返回 0**。

**证据**（Python ctypes 复现该结构体布局 + `zlib.crc32`）：
`sizeof = 2048`、`crc.offset = 1808`；`crc32(buf[:2044])` 写入后自校验 **False**。

⇒ `metadata_dir_load` 永远失败 ⇒ 每次开机都走 `metadata_dir_init`（全哨兵 Directory）⇒
**L2P 映射跨重启全丢**。这正是 `ftl_optimization_plan.md §8.2 P0-2 判据③`「复位后数据仍在
（`[META] anchor_seq` 非 0、`dyn` 非 0）——**当前必然失败**」与 `ftl_ut_munit.md`「`anchor_seq=0`…
根因未闭合」的根因。缺陷在 `c5cb8a0` 引入：旧实现用显式 `offsetof` 常量
`METADATA_DIR_CRC_OFFSET`（=908，对的），重写时改成了 `sizeof - 4` 而字段顺序已变。

**修法**：改为 `((size_t)offsetof(metadata_anchor_page_t, crc))`（=1812），与
`checkpoint.h` 的 `CHECKPOINT_CRC_LEN` 同一写法（padding 不参与校验）。
（⚠ 该修法在 **v0.62 被取代**：记录内 CRC 连同 `CHECKPOINT_CRC_LEN` / `ANCHOR_PAGE_CRC_LEN`
一并删除——`anchor_page_valid` 改由下层 FC 的 per-CW CRC + ECC 保证，见 v0.62 ADR。）

### 关键决策

| # | 项 | 决策 |
|---|---|---|
| **A** | 版本常量 | 新增 `FTL_MEDIA_LAYOUT_VERSION`（`ftl_config.h`）；**任何改介质格式的改动都必须 +1** |
| **B** | 标记位置 | `l2p_checkpoint_header_t` 插 `layout_version`（padding `2048-70*4`→`2048-71*4`，`CHECKPOINT_CRC_LEN` 276→280 自动）；`metadata_anchor_page_t` 插 `layout_version`（padding 236→232）。两者 `sizeof` 仍 2048 |
| **C** | 写侧 | `checkpoint_create` / `metadata_dir_flush_to_anchor` 各写当前版本（**在算 CRC 之前**） |
| **D** | 校验侧 | `checkpoint_header_valid` / `anchor_page_valid` 各加 `layout_version != 当前 ⇒ 无效`（旧格式记录字段处是旧 padding/旧 entries 字节，恒 ≠ 1） |
| **E** | 格式化触发判据 | **唯一**：本实例 `checkpoint_find_latest(&cp, topo) != FTL_OK` ⇒ 需格式化。因版本校验已含在 `checkpoint_header_valid` 内 ⇒ 该判据自动涵盖「有当前版本（否）/ 版本不符（是）/ 完全没有（是）」三种情况，且**自终止**（格式化后写入当前版本标记 ⇒ 下次开机判据 ① 命中） |
| **F** | 钩子位置 | `main.c ssd_ftl_boot()` 内、**`ftl_init()` 之后、free-pool 回填之前**。放 `ftl_init` 之后的理由：`ftl_init → bbt_init` 已**先把出厂坏块标记读进 BST**（`bbt_scan_factory` 读每块第 0 页列 2048）⇒ 之后再擦全片**不丢坏块信息**（满足用户「保留坏块标记」） |
| **G** | 擦除原语 | 新增 `bbt_erase_all_blocks(alloc)`：`plane × BLOCKS_PER_PLANE` 双循环，复用 `bbt_probe_block_bad(..., 1)`（擦前读出厂标记、擦除失败判坏）。**不走** `bbt_print_bad_map`（那是 CLI 打印用，会刷屏） |
| **H** | 标记写手 | 新增 `checkpoint_write_initial(topo)`：空 header（sequence=1、当前版本、journal_sequence=0）经 `metadata_ring_prepare_write` 取槽（自动擦目标块）后写入 |

**开机日志**（单行 `LOGW`）：`[FTL ] media layout mismatch -> format (erase all blocks)`

### 代价 / 取舍（诚实登记）

- **出厂空片首启会多擦一遍全片**（约 4096 块 × ~3ms ≈ 十余秒，一次性）。换来的是判据无需
  "介质是否为空" 这种启发式判断。格式化后写标记 ⇒ 只发生一次。
- **BST 的 `erase_count` 仍是旧介质读来的值**（`wear_init` 在格式化前已加载）——格式化后不重零。
  仅影响磨损均衡起点，无害；本次不修（重零会连带清掉 `bbt_init` 标好的坏位，得不偿失）。
- **格式化后 Directory 复位为哨兵层**：旧 L2P 位置全丢——这正是 "格式化" 的语义。
- **保留** `bbt --erase` CLI 作为手动兜底；**不**引入 "介质是否为空" 启发式。

### 改动文件

- `include/ftl/ftl_config.h`（`FTL_MEDIA_LAYOUT_VERSION` + 文件头「须先擦除」改「已自动兜底」）
- `include/ftl/checkpoint.h` / `src/ftl/checkpoint.c`（字段 + 写侧 + 校验侧 + `checkpoint_write_initial`）
- `include/ftl/metadata.h` / `src/ftl/metadata.c`（字段 + 写侧 + 校验侧 + **CRC 覆盖面修复**）
- `include/ftl/bbt.h` / `src/ftl/bbt.c`（`bbt_erase_all_blocks`）
- `include/ftl/power_failure_recovery.h` / `src/ftl/power_failure_recovery.c`（`ftl_media_needs_format` / `ftl_media_format`）
- `src/main.c`（`ssd_ftl_boot` 内钩子）
- `docs/design/ftl_design.md §3.3`、`ftl_optimization_plan.md §8.2`

### 验证

- `make clean && make all`：**零告警**；AXI_RAM **255.0 / 384 KB**（66.4%，本改动不增 RAM）
- `make clean && make tests`：**零告警**
- 静态：`grep -n "layout_version" include/ src/` 覆盖两结构体 + 两写侧 + 两校验侧
- 上板（测试一律上板）：
  - 烧 `debug.bin` 后**首次**开机打印 `media layout mismatch -> format`，随后正常 boot；
    断电重启 → **不再**打印格式化
  - **不再需要**手动 `bbt --erase`（本改动目的）
  - 写数据 → 断电重启 → 数据仍在（同时验证 **Anchor CRC 修复**：`[META] anchor_seq` 非 0、`dyn` 非 0）
  - `wr`/`rd`/`tr`/`fl`/`info` 冒烟；一轮 GC 后无 `IO_ERROR`

---

## v0.62 — 元数据校验和下沉：删 FTL 记录内 CRC，改由下层 FC per-CW CRC + ECC 承担

**日期**：2026-10-13

### 背景

长期遗留的一处不一致：FTL 给自己的每一条元数据 record（Checkpoint / Anchor / BBT / Wear）
自算整记录 CRC，但**同时又**把元数据页的 FC 层 CRC **关闭**（`FTL_PAGE_CFG_META` = `FC_CRC_DISABLE`），
只剩 BCH ECC。于是「FTL 自算 CRC」与「下层已有 CRC 通道却被关掉」两件事并存——既重复又矛盾。

要判定该保留哪一种，需要**出处**（项目北极星：判据 = 有出处的主流存储主控设计，非 Linux）。

### 出处调研（2026-10-13，逐源码核对）

| 出处 | 类型 | 元数据校验做法 |
|---|---|---|
| SPDK FTL（`lib/ftl/`） | **DRAM 型** | **自算 CRC32C**：superblock `crc` 字段；`ftl_band_md.p2l_map_checksum` / `ftl_nv_cache_chunk_md.p2l_map_checksum`（注释 "CRC32 checksum of the associated P2L map"）；`ftl_p2l.c` 用 `spdk_crc32c_update` 写校验、恢复时比对 |
| Cosmos+ OpenSSD GreedyFTL | **DRAM 型** | **只靠 ECC**：9 个 `.c` + `ftl_config.h` 全查零 crc/checksum（唯一 `Crc` = 硬件 ECC 引擎的 `V2FCrcValid`） |
| **dhara**（`dlbeer/dhara`，低内存嵌入式 NAND FTL） | **DRAM-less** | **校验和下沉到 NAND 层**：`journal.c`/`map.c` **零** crc/checksum；接口契约 `prog: program a NAND page, including ECC and **checksums**`、`nand.h`: `ECC must be handled by the NAND implementation`。dhara 明确：`Uncorrectable errors **must** be detected` / `may require the use of a checksum **in addition to ECC**` / `ensure that the checksum bytes are **also protected by ECC**!` |

**结论**：「FTL 有无自己的 CRC」不是行业定论，而是**按 DRAM 型 / DRAM-less 分岔**：

- DRAM 型（SPDK / Cosmos+）元数据是独立页、不假设底层兜底 ⇒ 自加 CRC（SPDK）或干脆只靠 ECC（Cosmos+）。
- **DRAM-less（本仓所属）** 元数据与用户数据走同一条 NAND 通道、下层就是自带 CRC+ECC 的 FC
  ⇒ 主流做法是**校验和下沉到下层**，FTL 只留结构判据（dhara 模型）。

本仓是 DRAM-less（`ftl_design.md` 标题即 "设计 DRAM-less L2P/CLOCK/Journal/Allocator"），
故判据取 **dhara 模型**。

### 三个候选与裁定

| 方案 | 内容 | 裁定 |
|---|---|---|
| **A** | 保留 FTL 整记录 CRC | SPDK 模型（DRAM 型）——与本仓 DRAM-less 定位不符 ❌ |
| **B** | 删 FTL CRC，只靠 ECC | **dhara 明确反对**（不可纠错误必须被检出，单靠 ECC 不够）❌ |
| **C** | 删 FTL 记录 CRC + 下层 FC 出 per-CW CRC + ECC | **dhara 模型 ✅ = 本版采纳** |

⚠ 关键事实核对：**本仓 v0.62 之前其实落在 B 不是 C**——`FTL_PAGE_CFG_META` = `FC_CRC_DISABLE`
⇒ 元数据页连 FC CRC 都没有、只剩 ECC。走 C 必须把该开关改成 `FC_CRC_ENABLE`。

### 技术前提核对（均已读码确认）

- FC 的 ECC 码字 = `FC_BUFFER_DATA_SIZE`（520B = META 4 + CRC 4 + USER 512）
  ⇒ **CRC 字段本身在 ECC 覆盖内**（满足 dhara 要求的 "checksum bytes also protected by ECC"）。
- `ftl_sync_read_page` 的成败判据 = `(completion.status & FC_CW_STATUS_ERROR_MASK) != 0`，
  该掩码**已含 `FC_CW_STATUS_CRC_ERROR`** ⇒ 翻开关后读路径**零改动**即生效。
- `ftl_cmd_write_data` 对不满页部分 `memset 0xFF` 补齐、并写满 4 个 CW ⇒ CRC 覆盖整页。
- 环池探针（`checkpoint_record_probe` / `anchor_record_probe` / `bbt_record_probe` / wear replay）
  读失败即视为「该 record 无效」⇒ 与「CRC 不符」同一处置。

### 关键决策

| # | 项 | 决策 |
|---|---|---|
| **A** | 元数据页 FC CRC | `FTL_PAGE_CFG_META`：`FC_CRC_DISABLE` → **`FC_CRC_ENABLE`**（单点开关；Checkpoint/Anchor/BBT/Wear/Journal/L2P-Region 六条路径共用它，一处即全部生效） |
| **B** | 删 FTL 记录 CRC | 四结构体删 `crc` 字段 + 全部算/校验点：`l2p_checkpoint_header_t`（padding `2048-71*4`→`2048-70*4`；删 `CHECKPOINT_CRC_LEN`）、`metadata_anchor_page_t`（padding 去掉 `-4U`；删 `ANCHOR_PAGE_CRC_LEN`）、`bbt_header_t`（packed，544→**540B**）、`wear_page_t`（`WEAR_ENTRY_MAX` 去掉 `-4U`：252→**253**，padding 12→8；`WEAR_SNAPSHOT_PAGES` 仍 17） |
| **C** | 布局版本 | `FTL_MEDIA_LAYOUT_VERSION` 1U → **2U**（四结构体尺寸/格式变化）⇒ 首次上板由 v0.61 自动格式化兜底 |
| **D** | 不动 | `boot_crc32` 保留（仍被 `boot_policy.c` 的开机镜像 CRC 使用）；`ftl_sync_read_page_raw` 家族（LDPC_RAW / 读列 / 读 META）保持 `FC_CRC_DISABLE` |

### 代价 / 取舍（诚实登记）

- **每条元数据页多 4B/CW 的 CRC 字段与计算**：CPU 开销可忽略（STM32H743 硬件 CRC 外设），
  介质开销 = 每 CW 4B（已在既有 528B CW 布局内，不额外占用户区）。
- **读路径不变**：CRC 错误与既有 NAND/TIMEOUT/UNC 走同一 `FC_CW_STATUS_ERROR_MASK` 判据。
- **介质格式变更**：四结构体尺寸/内容变化 ⇒ 首次上板触发一次自动格式化（预期内）。

### 改动文件

- `include/ftl/ftl_fc_interface.h`（`FTL_PAGE_CFG_META`：CRC 开关翻转）
- `include/ftl/checkpoint.h` / `src/ftl/checkpoint.c`（删字段 + 删 `CHECKPOINT_CRC_LEN` + 删写/校验）
- `include/ftl/metadata.h` / `src/ftl/metadata.c`（删字段 + 删 `ANCHOR_PAGE_CRC_LEN` + 删写/校验）
- `include/ftl/bbt.h` / `src/ftl/bbt.c`（删字段 544→540B + 删写/校验）
- `include/ftl/wear.h` / `src/ftl/wear.c`（删字段 + `WEAR_ENTRY_MAX` 252→253 + 删写/校验）
- `include/ftl/ftl_config.h`（`FTL_MEDIA_LAYOUT_VERSION` 1U→2U）
- 注释同步：`l2p_cache.c` / `journal.c` / `wear.c` / `metadata.h` / `bbt.h` / `ftl_fc_interface.h`
- `docs/design/ftl_design.md`（§7.6 四层保护重写、§3.8 BBT、§8.2 Checkpoint、§13.1 约束表、§Anchor、§Journal 接口）

### 验证

- `make clean && make all`：**零告警**；AXI_RAM **255.0 / 384 KB**（66.4%，本改动不增 RAM）
- `make clean && make tests`：**零告警**（`_Static_assert(sizeof == PAGE_MAIN_SIZE)` 全部通过
  ⇒ 四结构体尺寸回填正确）
- 静态：`grep -rni crc include/ftl src/ftl` 仅剩 FC 层数据路径引用，无 FTL 记录级 CRC 残留
- 上板（测试一律上板）：烧 `debug.bin` → 首次开机自动格式化一次 → 写数据 → 断电重启 → 数据仍在；
  人为翻转介质上一字节 → 该页读取报 `CRC_ERROR` 被丢弃（验证 CRC 真正生效）

---

## v0.63 — GC 触发对齐 DRAM-less 主流：删无出处触发（T2/T3）+ 统一求值点 + 修 IDLE 补池下溢

**日期**：2026-10-13

### 背景（问题从哪来）

波 0 上板后跑了几百次 `wr 0 32`，串口**一行 `[GC]` 都没有**。排查结论不是「GC 坏了」，
而是**触发模型与求值点双错**，外加一个顺带查出的真缺陷。

### 讨论内容

**1. 求值点不可达（结构性）**

`gc_check_triggers` 全仓**只有一个调用者**：`ftl_service_normal`（`ftl.c:481`）。
而 host 写全程 `queued != 0`（`pending.c` 的组 `used` 要到全部子操作完成才清）⇒ 模式三选一
走 `ftl_service_busy`（不 GC）；真空闲走 `ftl_service_idle`，它有**自己的**内联补池循环、
**不调**该函数。⇒ 五路触发里只有 T2（写入量整除 1024）能在实测中到达，且要 ≈256 次
`wr 0 32` 恰好命中整除点。

**2. T2/T3 在参考层查无出处（判据问题）**

逐字核过 `docs/reference/`（7 论文 + 9 网页）与 `storage_controller_learning.md`：
**T2（`total_writes % GC_INTERVAL_WRITES`）与 T3（`GC_TIMER_INTERVAL_MS`）零引用**。

| 出处 | 说的 GC 触发是什么 |
|------|-------------------|
| `reference/pages/spdk_ftl.html:180` | **按需**——「all of the bands contain some valid data and no band can be freed, so no writes can be executed anymore」 |
| 同上 `:160` | nvcache 压缩的 **free-chunk 阈值**（水位） |
| `reference/README.md:174`（dhara，DRAM-less） | 「没有复杂 GC，只有磨损均衡 + 坏块 + 日志」 |
| `cw_level_mapping_design.md:445-465`（DFTL/LeaFTL/VPFTL） | DRAM-less 侧只讨论**有效性判定**（OOB 反向映射），触发同样只认空间压力 |

对照：Journal 的 `CP_WRITE_INTERVAL` 在 `ftl_design.md` 有完整推导；T2/T3 只有
「分散 GC 压力」「空闲时有空就做一点」两句断言，参数表/代码注释/变更史全无推导。
**T3 的注释还自称「host 不写也不擦时，让空闲期把池补回去」（`gc.c:678`），
实现却在只在 host IO 在场时才跑的 NORMAL 路径——注释与实现自相矛盾。**

**3. IDLE 补池循环 `uint32_t` 下溢（真缺陷，顺带修）**

`ftl.c` 的 IDLE 补池：

```c
uint32_t target = FREE_POOL_TARGET - ftl->alloc.free_count;   /* ∈ [1, 32] */
while (target > 0U) { …GC 一个 SB…; target -= 2U; }           /* 每轮减 2 */
```

`free_count` 按 **2** 递减（`allocator_alloc_pair` 一次一对），本板实测 `free=3713`
（**奇数**）⇒ 一旦跌破 32，`target` 也是奇数 ⇒ 最后一次迭代后 `target == 1`，
`1 - 2U` 回绕成 **`0xFFFFFFFF`** ⇒ 循环只在 `gc_select_victim` 返回 0 时退出，
即**把 closed 链上的 SB 全搬全擦一遍**（长时间阻塞 + 无谓写放大）。

### 决策

| 序号 | 决策 | 选择 | 拒绝方案 | 理由 |
|------|------|------|---------|------|
| **D1** | 触发集合 | **删 T2/T3，留 T1/T4/T5** | ① 保留 T2/T3 只修求值点（T2 改有状态）② 连 T5 一起删 | 判据 = 有出处的主流；T2/T3 零引用且求值点不可达。T5（磨损比）是本仓 GC 的均衡维度，与独立静态 WL 分工不同，保留 |
| **D2** | 求值点 | **IDLE + NORMAL 共用 `ftl_gc_evaluate`** | ① 搬进 BUSY（三档全判）② 只留 IDLE | BUSY 是「host 饿着、后台让路」档（QoS 契约）；「无块可写」已由 `ftl_issue_subop` 的紧急 GC 兜底 = SPDK 的 demand 触发 |
| **D3** | 触发位值 | **不重排**（0x01/0x08/0x10；0x02/0x04 留空） | 重排成连续位 | 保持 `[GC] trig=` 日志与历史 ADR 里的位值可比；与 T6 的 0x20 留空同例 |
| **D4** | IDLE 突发规模 | **按「需回收的 SB 数」计数**（`⌈deficit/PLANE_NUM⌉`） | 保留 `target -= 2U` | 修下溢；deficit ≤ 0（仅 T5 命中）时取 1，即只回收一个 SB |
| **D5** | 顺带删 | `gc_check_trigger`（布尔版，零调用者）、`ftl_context_t.gc_last_run_writes`（零读者）、`gc_timer_start`（只服务 T3）、`BG_NORMAL_GC_PAGES` / `BG_IDLE_GC_TARGET`（死常量，注释描述已删模型） | — | 项目规矩：没有用处直接删掉 |
| **D6** | 不动 | `gc_select_victim` 四维评分（P1-1）、`gc_migrate_sb` 的 approach ③ 逐 CW 有效性判定、BUSY 档行为 | — | 均已有出处且已上板验证 |

### 代价 / 取舍（诚实登记）

- **空闲板（`free ≈ 3713 ≫ 32`）按设计就不该跑 GC** —— 这不是缺陷，是水位驱动的正确行为。
  代价是 §7 的**手工验证需要一个入口**：把 `FREE_POOL_TARGET` 临时改成 `free + 2`
  （触发 1 个 SB 的搬移+擦除），验完改回 32。**未引入 CLI `gc` 命令**（用户已选改常量方案）。
- **一次 GC 迭代「取 2 块目标页 + 还 2 块 victim」⇒ `free` 净变化 ≈ 0**，所以 §7 的判据
  是 `[GC]` 两行 + `rd PASS` + 无 `IO_ERROR`，**不是**「`free` 上升」。
- **删 T2 后不再有「按写入量摊平 GC 压力」的机制**：GC 变成纯水位驱动 ⇒ 压力集中在水位线附近。
  这是主流形态（SPDK 即如此），代价由 **OP（预留空间压低 u）** 与后台档位承担。

### 改动文件

- `include/ftl/gc.h`（删 `GC_INTERVAL_WRITES` / `GC_TIMER_INTERVAL_MS` / `GC_TRIGGER_WRITES` /
  `GC_TRIGGER_TIMER` / `gc_check_trigger`；`gc_check_triggers` 签名去两参；位注释改写）
- `src/ftl/gc.c`（删 T2/T3 分支与布尔包装；文件头 + 函数注释「五路」→「三路」）
- `include/ftl/ftl.h`（删 `gc_last_run_writes` / `gc_timer_start` / `BG_NORMAL_GC_PAGES` /
  `BG_IDLE_GC_TARGET`）
- `src/ftl/ftl.c`（新增 static `ftl_gc_reclaim_one` / `ftl_gc_evaluate`；`ftl_service_idle`
  与 `ftl_service_normal` 改用统一求值点；删两字段 init；修补池下溢）
- `docs/design/ftl_design.md`（§4.4 触发表重写 + T2/T3 删除论证、§4.6 带宽/后台模型重写并删
  无出处的「三星 GC 策略」句、§4.7 参数表、§12 服务模式伪码与实现原型、§4.9 对照表）
- `docs/design/ftl_api_usage.md`（上下文结构列举、Phase 3 流程叙述）
- `docs/design/ftl_wave0_board_verify.md`（§7 配方重写 + §4 前提行）

### 验证

- 静态：`grep -rn "GC_INTERVAL_WRITES\|GC_TIMER_INTERVAL_MS\|GC_TRIGGER_WRITES\|GC_TRIGGER_TIMER\|
  gc_timer_start\|gc_last_run_writes\|gc_check_trigger\b" src/ include/ tests/` ⇒ **零命中**
  （仅 `gc.h` 位注释里作为「已退役」的说明性引用）
- `make clean && make all`：**零告警**；AXI_RAM **255.0 / 384 KB**（66.4%，本改动只减字段）；
  `Binary size: 189656 bytes`
- `make clean && make tests`：**零告警**；`Binary size: 267772 bytes`
- 上板（测试一律上板，见 `ftl_wave0_board_verify.md` §7 新配方）：
  临时 `FREE_POOL_TARGET = free + 2` ⇒ 开机后第一个 IDLE tick 应打
  `[GC] trig=0x01 free=… op=… writes=…` + `[GC] victim p0=… p1=… vr=… er=… cp=… yp=… cost=…`
  **各一行**（下溢修复的判据就是行数：旧代码会把 closed 链全搬全擦、连打几十行）；
  `rd 0 8 c` ⇒ `rd PASS`；断电重启 ⇒ `[WEAR] max_ec ≥ 1`

---

## v0.64 — 修「写→掉电→重启丢数据」：干净关机路径从不恢复 Journal RAM 写态

**日期**：2026-10-13

### 背景（问题从哪来）

波 0 清单 §6（掉电重启数据仍在）**上板失败**：

```
[INF] [FTL ] boot type=2 cp_seq=624 journal_entries=3811
...
rd FAIL at byte 0: got 00000000 expect 55555555
```

`journal_entries=3811` 这个数字本身就是证据：3811 = 30×127 + 1 ⇒ 回放扫到的条目
落在**环形第 0 块第 30 页**（`JOURNAL_ENTRIES_PER_PAGE=127`、`PAGES_PER_BLOCK=64`）。
但一个刚格式化的环，写到第 30 页意味着 3811 条 L2P 更新——远超本轮测试实际写过的量。
⇒ 这是**之前几百次 `wr 0 32` 留在环里的旧页**被重新当成「本轮数据」回放/漏回放。

### 讨论内容

**1. 根因：`journal_init` 无条件把 RAM 写态复位，而 NAND 上的环并没被擦**

`journal_init` 每次开机都把 `current_block/page/entry = 0`、`sequence_counter = 1`。
`ftl_boot_clean_start` 的注释据此写着「④ Journal 已由 ftl_init 初始化（空）」——**这个前提是错的**：

- `journal_truncate`（Checkpoint 的截断动作）**只预擦环形下一块**（P1-6 的单块单擦设计），
  它**不擦**环里其余已编程的页；
- 于是「Journal 是空的」只对**刚格式化过的介质**成立。正常运行的板子上，
  环里躺着一圈已编程的旧页。

两个后果，任一都足以毁掉 §6：

- **① 序号回退**。序号从 1 重来 ⇒ 本轮新条目的 `page_seq` **排在环内旧页之后**。
  下次开机 `journal_replay` 的过滤（`page_seq > cp_sequence`）把新页当「已进 L2P 表」整页丢弃，
  而旧页（序号大）反而通过过滤被应用 ⇒ L2P 被**旧映射覆盖**。
  这解释了 FAIL 那次为什么读到全 0：LBA 0 的映射被指回了从未写过数据的物理位置。
- **② 写指针回到 block0/page0** ⇒ 对**已编程**的页二次编程。NAND 编程只能 1→0，
  二次编程 = 按位 AND ⇒ 页头 `entry_count`/`magic` 被静默改坏，环从此不可解析。

**2. 顺带查出的第二个缺陷：`journal_replay` 的写指针落点**

旧实现的指针恢复是「最新页 + 页内下标 + 1」——即**继续填最新那一页**。
若最新页是部分页（掉电撕裂），续填会在页满时对该页**再 flush 一次**，同样二次编程页头。
正确落点是**最新页之后一页**（最新页已编程，不可再动）。

**3. 为什么「干净关机」也会中招**

干净关机（`type=1`）看似不经回放，但 §6 的失败恰恰在 `type=2`。两条路径共用一个错误前提：
**都假设 `journal_init` 之后 RAM 写态与 NAND 一致**。所以修复必须覆盖全部三条启动路径。

> 旁证：随后的「干净环重测」打印 `type=1 journal_entries=0` 并 `rd PASS`。
> 那次 PASS **没有覆盖回放路径**（数据是靠 Checkpoint 活下来的，`cp_seq` 1→4），
> 所以它**不能**用来判定缺陷已清 —— 两个日志是同一个机制的两面。

### 决策

**Journal 层新增两个 API，把「扫环」与「接写态」从启动路径里收进 Journal 模块**：

- `journal_scan_latest(const l2p_journal_t *, journal_page_ref_t *out)` —— **只读**扫环取最新页
  （最大 `page_seq`），出参含 `{page_seq, block, page, entry_count}`。环空返回 0。
  与 `journal_replay` 共用同一份扫描内核 `journal_scan_ring`（旧实现是启动路径内联扫一遍，
  两份逻辑各写各的）。
- `journal_resume(l2p_journal_t *, const journal_page_ref_t *latest, uint32_t floor_sequence)` ——
  按 NAND 实际内容恢复写指针与序号，**不回放任何条目**：
  - 落点 = 最新页**之后一页**；跨块时先擦下一块（那里还留着上一圈的旧页）；
  - 序号 = `max(最新页 page_seq + 最新页 entry_count, floor_sequence)`，`floor_sequence` 由调用者传
    Checkpoint 的 `journal_sequence`（兜住「环空」与「环内最新页反而落后」两种情形）；
    **必须是 `+ entry_count` 而不是 `+1`** —— 一页占了 `entry_count` 个序号
    （`journal_append` 逐条 `sequence_counter++`，页头 `hdr_page_seq = sequence_counter −
    current_entry` 反推），只 +1 会让序号退回本页内部，下一页的 `page_seq` 落进本页区间，
    回放过滤 `page_seq + entry_count > cp_sequence` 便**重复回放已进 L2P 的页**
    （上板现象 = `journal_entries` 虚高：286、389 = 3×127+8，都带整页形状）。
  - 清空 `page_buf` 为全 FF、`current_entry = 0`。

**三条启动路径全部接上**：

| 路径 | 调用 |
|---|---|
| `ftl_boot_recovery` | 先 `journal_scan_latest(&ftl->journal, &ring)` 取快照（只读，供判型），再分派 |
| `ftl_boot_fresh_start` | 新增 ④ `journal_resume(&ftl->journal, ring, 1U)` |
| `ftl_boot_clean_start` | 新增 ② `journal_resume(&ftl->journal, ring, cp_journal_sequence)`（这是本缺陷的正面修复） |
| `ftl_boot_power_failure` | 走 `journal_replay`，其末尾已改为 `journal_resume` |

**回放过滤判据由「页首」改为「整页」**：`page_seq > cp_sequence`
⇒ `page_seq + entry_count > cp_sequence`。旧判据会把**跨水位的那一页整页跳过**，
丢掉它尾部那几条本应回放的新条目（水位落在页中间时必现）。

**删死代码**：`ftl_boot_recovery` 里的内联扫描被 `journal_scan_latest` 取代后，
`power_failure_recovery.c` 的 static `journal_page_addr` 变成孤儿 ⇒ 删。
扫描用的 2 KB `page_data` 缓冲随之从 `ftl_boot_recovery` 的栈帧移到 `journal_scan_ring`，
**栈占用不增**（`_Min_Stack_Size = 0x800`）。

### 代价 / 取舍（诚实登记）

- **每次开机多一次只读扫环**（`JOURNAL_BLOCK_COUNT` 块 × 64 页的页头读，只读第一页头即可
  命中即跳过整块，但当前实现是逐页扫）。代价在开机时间，不在稳态路径。
  与「干净关机必须扫」的收益（否则写指针就是猜的）相比，这个代价是必需的。
- **`pre_erased_block` 是纯 RAM 记账，掉电即失**。重启后 `journal_resume` 跨块时会**再擦一次**
  下一块 ⇒ 相对「不丢记账」的情形多一次擦除。**这不是新引入的**（`journal_init` 本来就丢），
  但修复让这个路径真正可达了 ⇒ 登记为已知代价。P1-6 的「每圈只擦一次」在**单次开机内**成立。
- **`ftl_boot_power_failure` 仍走 `journal_replay` 的全量回放**，与 `resume` 的分工是
  「先回放（应用条目）→ 再 resume（接写态）」，没有合并成一次扫描的优化（可读性优先）。

### 改动文件

- `include/ftl/journal.h`（新增 `journal_page_ref_t` + 两个 API 声明；`journal_replay` 的
  判据注释改为整页口径）
- `src/ftl/journal.c`（§4 重写：static `journal_scan_ring` 单一扫描内核 +
  `journal_scan_latest` + `journal_resume`；`journal_replay` 改用共用内核，
  过滤改整页口径，末尾接 `journal_resume`；文件头职责行同步）
- `include/ftl/power_failure_recovery.h`（两个 boot 函数签名加 `const journal_page_ref_t *ring`；
  文档注释补 `journal_resume` 步骤与「环形块并没被擦」的关键说明）
- `src/ftl/power_failure_recovery.c`（`ftl_boot_recovery` 改为「先取快照 → 判型 → 分派」；
  两条 boot 路径各加一步 `journal_resume`；删 static `journal_page_addr`）

### 验证

- `make clean && make all`：**零告警**；AXI_RAM **255.0 / 384 KB**（66.4%）；
  `Binary size: 189336 bytes`（改前 189656，**净减 320B** —— 内联扫描 + 孤儿函数删掉，
  扫描缓冲只留一份）
- `make clean && make tests`：**零告警**；`Binary size: 267460 bytes`（改前 267772）
- `grep -n journal_page_addr src/ftl/power_failure_recovery.c` ⇒ **零命中**
- 上板（待验，判据见 `ftl_wave0_board_verify.md` §6）：
  ① 复现配方 = 先把环弄脏（若干次 `wr 0 32`）→ 干净关机（`fl` 后断电）→ 再 `wr 0 8`
  → 直接断电 → 重启 `rd 0 8 c` 应 `rd PASS`；
  ② 修复前的签名是 `rd FAIL at byte 0: got 00000000`，修复后 `boot type` 仍按关机方式为 1 或 2，
  但 `journal_entries` 应回到**本轮实际写量**量级（不再是 30×127+1 这种整圈数）。
- **上板结果（2026-10-13）**：§6 主判据过 —— 脏环配方全程无 `rd FAIL`，最后一次
  `type=2 cp_seq=122 journal_entries=389` 下 `rd 1000 8 c` ⇒ `rd PASS`（该 LBA 本轮只写过 8 条，
  映射只能来自回放）。**但 `journal_entries` 虚高未消**（286、389 = 3×127+8，都带整页形状），
  据此反查 `journal_resume` 的序号落点，发现上述 `+1` / `+ entry_count` 缺陷并修正
  （`Binary size: 189344 bytes`，零告警）。**修正后需重跑 §6 复核 `journal_entries`。**
- **不变量（比具体数字更重要）**：`journal_resume` 保证 `sequence_counter > 环内任何 page_seq`
  ⇒ 新条目序号恒高于环内所有旧页 ⇒ 回放按 `page_seq` 升序 + per-LBA last-write-wins
  ⇒ **旧页即使被回放也必然先应用、被新条目覆盖**。这是 v0.64 真正提供的数据安全保证，
  「`journal_entries` 偏大」本身不构成数据风险，只浪费开机时间。

#### v0.64 追加修正 ③：判型判据的 off-by-one（**本批第三个、也是最要命的缺陷**）

**上板证据（擦净后的全新介质，2026-10-13）**：

```
（bbt --erase 后首次开机自动格式化）
[INF] [F2] rebuild blocks=0 partial=0 valid_cw=0
[INF] [FTL ] boot type=1 cp_seq=2 journal_entries=0
wr 1000 8                      ← status=0，写成功
（断电）
[INF] [F2] rebuild blocks=2 partial=2 valid_cw=0
[INF] [FTL ] boot type=1 cp_seq=3 journal_entries=0
rd 1000 8 c  ⇒  [ERR] rd FAIL at byte 0: got 00000000 expect 55555555
```

**数据页在 NAND 上（`blocks=2`），映射没了（`valid_cw=0`）⇒ 映射从未被恢复。**

**根因**：`ftl_boot_recovery` 的判型只看**页首**：

```c
if (ring.page_seq <= cp.journal_sequence)   /* ✗ 判成「干净关机」 */
```

而 `cp.journal_sequence` 的语义是「**下一个待分配**序号」（`checkpoint_create` 直接取
`sequence_counter`；`journal_truncate` 的注释亦如此声明：全部已写条目 `seq < up_to_sequence`）。
⇒ 被 CP 覆盖的条目是 `seq < cp.journal_sequence`。一页占
`[page_seq, page_seq+entry_count-1]`，**全被覆盖 ⟺ `page_seq + entry_count ≤ cp.journal_sequence`**。

**Checkpoint 之后写入的第一页，其 `page_seq` 恰好 == `cp.journal_sequence`**（因为页头由
`hdr_page_seq = sequence_counter − current_entry` 反推，CP 时刻 counter 就是那个值）。
于是：写 → 掉电 → 判 `type=1`（干净）→ **不回放** → L2P 从 NAND 表加载（没有这批映射）
→ 静默丢数据。

**修法**：判型改整页口径，与 `journal_replay` 的过滤同口径：

```c
if ((ring.page_seq + (uint32_t)ring.entry_count) <= cp.journal_sequence)   /* ✓ */
```

**为什么这个缺陷特别隐蔽**：① 它只在「CP 之后写入、且该页是环内最新页」时命中，而
`ftl_service_idle` 每轮都会 `journal_flush_page`，所以页确实落了盘、看起来一切正常；
② CLI 的 pattern 与 LBA 无关（前 8KB 全 `0x55555555`、后 8KB 全 `0xAAAAAAAA`，见
`cli.c:103-151`）⇒ 只要读到**任何**含该 pattern 的页就报 `rd PASS`，**旧介质上的
「PASS」因此可能是假绿**；③ 它同时解释了本批日志里 `type=1 journal_entries=0` 反复出现——
判型几乎总是偏一页地判成「干净」。

**验证**：`make clean && make all` 零告警，`Binary size: 189488 bytes`。
上板判据：擦净介质 → `wr 1000 8` → 断电（**不需要抢时间**：journal 页由 IDLE 每轮刷出）
→ 重启应 `type=2` + `journal_entries=8` + `rd 1000 8 c` ⇒ `rd PASS`。

**已改正的判据口径（三处必须一致）**：
| 位置 | 判据 |
|---|---|
| `ftl_boot_recovery` 判型（干净） | `page_seq + entry_count <= cp_sequence` |
| `journal_scan_ring` 回放过滤 | `page_seq + entry_count > cp_sequence` |
| `journal_resume` 序号落点 | `page_seq + entry_count` |

---

## v0.65 — 波 2 剩余 + 波 3：表自持 / 多实例 / OOB 出厂坏块 / 分片模型定案 / L2P 日志块压缩 / rr 持久化

**日期**：2026-10-13

### 背景

按 `ftl_optimization_plan.md` §九「优化顺序」推进。波 0（v0.59~v0.64）收口后，
波 2 剩四项（序 3/4/5/6，均为**潜伏债**，`dims=1` 时不咬）、波 3 两项（序 7/8，
有效性缺口）、波 5 的最后一个死代码项，本批一并落地。

**本批全部未上板。**

---

### 序 3 + 序 4 — FTL 表自持 + 多实例化（同一改动的两面）

**问题**：`checkpoint.c` / `bbt.c` / `wear.c` 各有一组 **file-scope static** 游标与序号。
单实例下无害；多实例（多 LUN，`g_ftl[ch][ce][lun]`）下**所有实例共享一份** ⇒ 各自往
同一个池槽写、`sequence` 互相踩、脏位图互相清。这与「每个 `(ch,ce,lun)` 是一个完整
独立的 FTL 实例」的分片模型（`cw_level_mapping_design.md` §3.1）直接冲突。

**做法**（把状态搬到「它天然属于谁」的地方，而不是一律塞进 `ftl_context_t`）：

| 原 static | 新家 | 为什么 |
|---|---|---|
| `bbt.c` 的 sequence / write_idx / cursor_valid / dirty | `ftl_allocator_t.bbt`（`bbt_state_t`） | `bbt.h` 的**全部**接口都以 `ftl_allocator_t *` 为首参；且 `bbt.h` 已 include `allocator.h`，反向包含成环 |
| `wear.c` 的 sequence / next_page / 节拍基准 / 脏位图 | `ftl_allocator_t.wear`（`wear_state_t`） | 同上 |
| `checkpoint.c` 的 sequence / write_idx / cursor_valid | `ftl_context_t.cp`（`l2p_cp_state_t`） | `checkpoint.h` 已前向声明 `struct ftl_context`，无环 |

`allocator_init` 把 `alloc->bbt` / `alloc->wear` **显式 memset 归零**——这两个结构里的
`cursor_valid` / `dirty[]` 决定首次落盘是「扫池定位」还是「从池首开始」，残留值会让
新实例写到别人的槽上。

**顺带删掉的死字段**：`ftl_context_t.cp_sequence`——4 个写点、**0 个读点**。boot 日志里
的 `cp_seq` 来自 `ftl_boot_result_t.cp_sequence`，真正的池序号是 `cp.sequence`，三者
语义各不相同，旧字段纯属混淆源。同理 `ftl_boot_clean_start` 的 `cp_sequence` 形参
（`checkpoint_find_latest` 已把序号写进 `ftl->cp.sequence`，形参成了第二真值）。

**泵/实例分离**（多实例的正确调度形态）：

```c
void ftl_service_pump(void);          /* 实例无关：Phase 1 推进 NAND + Phase 2 抽干完成表 */
void ftl_service(ftl_context_t *ftl); /* 单实例：Phase 2.5 补发子操作 + Phase 3 三级后台 */
```

`main.c` 每轮：`ftl_service_pump()` 一次 + 对**每一个** `g_ftl[ch][ce][lun]` 调
`ftl_service()`。Phase 1/2 的 NAND 引擎与完成表是**全局单份**，per-instance 调用会把
`fc_service` 预算重复消耗 N 遍；Phase 2.5/3 则必须逐实例，否则漏调的实例
journal / checkpoint / wear / bbt / GC **永不推进**。

**内存**：`g_ftl[1][1][1]` 单实例，AXI_RAM 283.1/384 KB（73.7%）。当前
`CH_NUM=CE_NUM=LUN_NUM=1` ⇒ 内存预算恰好只装一个实例；维度扩展须先重规划内存
（`main.c` 的 `_Static_assert` 会在编译期拦下）。

---

### 序 5 — 出厂坏块检测改走 OOB 标记（P2-3）

**问题**：`bbt_scan_factory` 原来靠「裸读返回码 + 全 0xFF」猜坏块。两个判据都不可靠：
raw 模式对物理失败也返回 READY；全 0xFF 既可能是空块也可能是坏块，**分不开**。

**做法**：改读器件手册规定的**出厂坏块标记**——每块首页 spare 区首字节（列 2048），
经 `OP_READ_COLUMN`（`ftl_sync_read_column`，FC 槽位 42，**此前已实现**）够到：

- 标记非 `0xFF` ⇒ 出厂坏块；标记 == `0xFF` ⇒ 好块；**列读失败 ⇒ 坏块（保守）**。

判据逐字来自 Micron AN1819：*"the bad block information is erasable and cannot be
recovered once erased"* + *"System software should check the first spare area location on
the first page of each block prior to performing any PROGRAM or ERASE operations"*。
⇒ 扫描阶段**只读不擦**（`bbt_probe_block_bad(..., do_erase=0)`）——擦除会永久毁掉标记。

`bbt_load` 顺带去掉 `topo` 形参（从 `alloc` 派生，与其余 BBT 接口一致）。

---

### 序 6 — D6「LBA→实例路由上移到单一 FTL 前端」：**登记理由，保留分片模型**

**动作是文档，不是代码**，理由是查证结论：

- 该主张的出处是**本仓自己的 D6 对照表**（`ftl_host_interface.md:65`、
  `ftl_revision_history.md:2380`、`ftl_optimization_plan.md:597`），三处均标「主流」
  但**无任何外部引用**。
- 它唯一援引的 `fc_nand_design.md §26`（本项目锁定的判据）**通篇不谈** L2P 在
  通道/die 间如何分布——只定 HIL/FTL/FCC 分层边界。
- 仓内还存在**反方向**的无出处断言（`ftl_revision_history.md:1032`「行业标准：每 LUN
  独立 region 表」）⇒ 两个方向都被断言过，都无证据。
- 唯一可能支持「单一 FTL 拥有全 LBA 空间」的**已备份**厂商文档（SPDK FTL，
  `docs/reference/pages/spdk_ftl.html`）从未被引用于此。

**裁决**（按 `ftl_optimization_plan.md:597` 明文允许的「**或**明确保留分片并登记理由」）：
**保留分片模型**，理由如下，且**不**把它记为「与主流偏离」——

1. 分片是**本仓的刻意模型**（`cw_level_mapping_design.md` §3.1/§3.2 有完整对比：
   模型 A 分片 vs 模型 B「LUN 作为地址维度」，后者已删）。
2. 判据要求「有出处」。「主流是单一前端」无出处 ⇒ 按无出处处理，**不得**据此改代码
   （否则就是以另一个无出处断言替换现有的刻意设计）。
3. 平台现状 `CH_NUM=CE_NUM=LUN_NUM=1` ⇒ 今天恰好只有一个 FTL 实例，
   「单一前端」性质**平凡成立**；真正的分歧只在维度扩展时才显现，属未排期。

⇒ `ftl_host_interface.md` 的 D6 行与 `ftl_optimization_plan.md` §九 序 6 据此订正
（「主流」列改为「无出处（本仓断言）」+ 保留理由）。

---

### 序 7 — P0-3 L2P 日志块压缩（抗碎片）

**问题（活跃容量泄漏）**：`metadata_dir_reclaim` **只**回收 `l2p_live[g] == 0` 的日志块。
某个块只要还挂着一个**永不再写的冷 region**（如 63 冷 + 1 热），活计数就永不归零 ⇒
**永久占块**。稳态下日志块数单调增长，最坏吃掉近半个 die（`FTL_BLOCK_TOTAL/2` 块）。

**做法**：新增 `metadata_dir_compact(dir, alloc)`——`reclaim` 的补集。

- **victim** = `l2p_live` 最小（但 > 0）的**非当前**日志块，且必须是 `RESERVED`
  （与 `reclaim_block` 同一身份判据，避免误伤恰好活计数非零的用户块）。平手取块号最小者
  （结果确定，跨复位可复现）。
- **搬移** = 裸页拷贝：读 victim 上的 region 页（2KB = `L2P_REGION_SIZE`，region 页无头，
  就是 entries 数组）→ `metadata_dir_append_slot` 取当前块新槽 → 写 → `commit_region` 登记。
  **不经过 `l2p_cache`** ⇒ 不触发「写→evict→flush→又要块」的递归。
- **掉电安全顺序**（不可重排）：① 搬移 → ② 登记 → ③ `metadata_dir_flush_to_anchor`
  → ④ 擦 victim → ⑤ 归还 Free Pool。**③ 必须早于 ④**——否则掉电后 Anchor 仍指向
  已擦块，那些 region 的映射**永久丢失**（读回 0xFF = 未映射）。
- **触发点**：`metadata_dir_append_slot` **刚取到新日志块**之后，且
  `alloc->free_count <= FREE_POOL_TARGET`。此刻块内有整 `PAGES_PER_BLOCK` 个空槽
  ——这是唯一能「一次搬空任意 victim」的时机（一块最多 `PAGES_PER_BLOCK` 个 region）。
  低水位门槛保证池子宽裕时不做无谓 NAND 写；稳态下每 `PAGES_PER_BLOCK` 次 region 写
  最多压缩一块。

**新增 `dir->compacting` 标志，两个作用缺一不可**：

1. **抑制逐次立即回收**：搬移 victim 最后一个 region 时活计数归零，
   `commit_region` 的「归零即擦」分支会在 ③ 之前把 victim 擦掉（见上）；
2. **防重入**：搬移走 `append_slot`，其换块分支会再触发一次压缩 ⇒ `append_slot` 在
   `compacting != 0` 时直接返回 `NAND_PBA_INVALID` 让路（压缩方收到即中止，victim 不擦，
   已搬 region 的新位置已登记 ⇒ **无信息丢失**，下次重来）。

`compacting` 在 `metadata_dir_init` / `metadata_dir_load` 都显式归零（不跨重启）。

---

### 序 8 — rr（慢死预测）持久化

**问题**：`ftl_block_t.rr`（`last_ecc` / `rising_count` / `health`，3B）是纯 RAM 态，
`ftl_init` 的 memset 跨重启清零 ⇒ `rr_predictive_scan` 的 ECC 趋势分支**跨重启归零**，
要重新攒 `RR_ECC_RISING_THRESHOLD` 次连续上升才再触发，期间那块一直在劣化。
（read-disturb 分支不受影响——`read_count` 已由 P0-1 的 Wear 表持久化。）

**做法**：把三字段并进 **Wear 条目**（该表已有完整的「页式追加日志 + 脏位图 + 全量
快照」机制，语义上也同属「per-Block 账本」）⇒ `wear_entry_t` 8 B → 12 B。
三字段**都必要**：

| 字段 | 丢了会怎样 |
|---|---|
| `last_ecc` | 「本次是否上升」的比较基准丢失 ⇒ 重启后首次读**必判上升**（虚高） |
| `rising_count` | 连续上升的进度丢失 ⇒ 趋势从头数 |
| `health` | 已判定的 WARNING/CRITICAL 白丢 |

`wear_init` 回放时对 `health` 做**范围钳制**（越界值 → `RR_BLOCK_HEALTHY`）：枚举只有
0..3，坏页读出的越界值会让 `rr_predictive_scan` 的 `>= RR_BLOCK_CRITICAL` 比较行为
未定义；钳到 HEALTHY 等于「这块重头数」，安全侧。

**介质格式变了 ⇒ `FTL_MEDIA_LAYOUT_VERSION` 2 → 3**。老格式 Wear 页的 `magic` **仍然
匹配**、条目布局却不同 ⇒ 不靠版本拦下就会把计数读成垃圾（这正是 v0.61 版本机制要防的）。

---

### 波 5 — 删死代码 `ftl_boot_scan_write_pointer`

旧单-SB 写指针扫描路径，**零调用者**（写指针恢复由 F2 全扫 NAND 完成）。且它把
`uint8_t buf[16]` 传给按 `PAGE_MAIN_SIZE`（2048B）拷贝的 `ftl_sync_read_page_raw`
——栈溢出。按用户「无用直接删」原则删除（函数 + 头声明 + 文件结构注释）。
`ftl_sync_read_page_raw` 本身保留（F2 的 `page_is_blank` 仍在用）。

---

### 验证

```bash
make clean && make all      # 0 warning；AXI_RAM 283.1/384 KB（73.7%）；Binary size: 190952 B
make clean && make tests    # 0 warning
```

**本批全部未上板。** 上板判据（并入 `ftl_wave0_board_verify.md`）：
多实例化与内存断言由编译期保证；OOB 出厂扫描的判据是「擦净介质后开机 `bbt` 读数与
器件标记一致」；P0-3 压缩的判据是「长写后 `free` 不单调下降 + `rd PASS`」；
rr 持久化的判据是「`[WEAR]` 行在重启后 `max_ec` 与关机前一致」。

### 不做（本批）

`CONFIG_FC_RAID` 保持 0；`read_recovery.c` 保留地址 0x89 不碰；`journal.c`/`crash.c`
断言策略不变；不动 `include/nand/nand_config.h`；**P2-6（per-plane 独立写指针）**与
**波 4（GC Bitmap Layer 2 / Prefetch / 多流 hint）**保持登记未排期——前者是写指针模型的
结构性改动（multi-plane 要求两 plane 同页号，独立指针会拆掉 multi-plane 的前提），
后者的正确性窗口已由 v0.43「擦除前 flush」闭合（Layer 2 只是掉电后**断点续搬**的优化）。

---

## v0.66 — P2-6 定案（器件/标准约束）+ 页打包写缓冲（§七 方案 A）

**日期**：2026-10-13

**范围**：① P2-6 从「未排期（结构性改动）」改判为 **「不实现（器件/标准约束）」**（纯文档收口）；
② 实现 `cw_level_mapping_design.md §七 方案 A` 的**页打包写缓冲**——同一 pair 窗口内的多次
host 写攒成一页、窗口填满 / 切换 / flush / 空闲时**一次编程**。

### 问题（P2-6）

`allocator_alloc_pair`（`src/ftl/allocator.c`）**每个写子操作**都给出一对 plane 页并推进写指针。
CW 级映射下子操作常只覆盖半个 pair 窗口 ⇒ 一次 512 B 写（1 CW）消耗一整对 4096 B 页
= **12.5 % 利用率**；`wr 0 4`（半窗口）= 50 %。器件**按页编程**：

- Micron `MT29F4G08ABADAWP` 参数页：Byte 86–89 数据字节/部分页 = `0x0200` = **512 B = 1 CW**；
  Byte 110 programs per page = **4**；时序表 Note 1：*"Four total partial-page programs to the
  same page. If ECC is enabled, then the device is limited to **one partial-page program per ECC
  user area**, not exceeding four partial-page programs per page."*（本仓每 CW 一个 ECC user area
  ⇒ 每 CW 只能编一次）。
- ONFI 3.2 §5.7.1.25 Byte 110 同义。

### 关键决策：P2-6 的登记修法不可实现

登记修法 = **per-plane 独立写指针**。**否决**——两处一手判据都要求两 plane **同页地址**：

| 出处 | 逐字引文 |
|---|---|
| Micron `MT29F4G08ABADAWP` §Two-Plane Addressing | *"**The page address bits, PA[5:0], must be identical** for each issued address."*（`BA[6]` 必须不同；LUN 位必须相同） |
| ONFI 3.2 §3.1.1 Multi-plane Addressing | *"The plane address bit(s) shall be distinct… **The page address shall be the same** as any other multi-plane operations in the multi-plane command sequence."* |
| ONFI 3.2 §3.1.1.1 | *"**No restriction**: All block address bits may be different between two plane addresses."* ⇒ **Policy B（两 plane 不同块号）有标准级出处** |

独立写指针会让两 plane 的页号分叉 ⇒ 只能退化成两次单 plane 编程 ⇒ **拆掉 multi-plane 前提**。
⇒ 唯一能消除浪费又保留 multi-plane 的路 = **§七 方案 A 页打包写缓冲**（也正是主流做法）。

### ONFI 版本时间线（对 ONFI 全集二分查证）

| ONFI 版本 | 全文 `plane` 出现次数 | 备注 |
|---|---|---|
| 1.0 / 2.0 / 2.1 / 2.2 | **0** | 1.0 的 `11h` 是 **Interleaved（多 LUN）**，不是 multi-plane |
| 2.3a | 首现 | `plane` 一词首次出现 |
| **3.2** | **首成标准** | §3.1.1 / §6.3 / §6.5 / §6.6 定义 multi-plane，并**强制同页地址** |

### 方案 A 设计（`ftl_wbuf_t`）

`ftl_context_t` 新增 `ftl_wbuf_t wbuf`（4 KB 页镜像）：

```c
typedef struct {
    uint8_t  data[FTL_LBA_PER_PAIR * FTL_IU_BYTES]; /* 8 CW × 512B = 4096B 页镜像 */
    uint8_t  occupied;   /* bit i = 槽 i 有数据；i = plane*CW_NUM + cw（与 LBA 低 3 位同构） */
    uint8_t  open;       /* 1 = 正持有某窗口 */
    lba_t    base;       /* 持有页的 pair 窗口基址（低 3 位清零） */
    uint32_t owners;     /* bit gi = 组 gi 向本页贡献过 ≥1 槽（FTL_GROUP_MAX=32） */
} ftl_wbuf_t;
```

**写准入**（`ftl_issue_subop` 写分支 → `ftl_wbuf_append`）：窗口变化先 flush；把 n 个 CW 从 host
切片拷进 `wbuf.data[slot]`、置 `occupied` 位、`owners |= 1u<<gi`；满（`occupied==0xFF`）则尝试 flush。

**flush**（`ftl_wbuf_flush`）：`allocator_alloc_pair`（含 emergency GC）→ `fc_command_alloc` →
op select（两 plane 有数据 = `OP_PROGRAM_PAGE_MULTI_PLANE`，否则单 plane）→
`ftl_cmd_setup_ppa_pair_cw(cmd, new_pba, occupied)` → **`ftl_cmd_write_data_cw(cmd, wbuf.data,
occupied)`**（**按 CW 偏移从整页镜像散射**）→ `pending_add`（`lba = wbuf.base`，`cw_bitmap =
occupied`，`owners = wbuf.owners`，`buffer = NULL`）→ `fc_submit`。

> ⚠ **不能用 `ftl_cmd_write_data`（顺序打包）**：方案 A 合并多组后 `occupied` 可能**非连续**
> （组 A 写槽 {0,1}、组 B 写槽 {5,6} ⇒ `0x63`），顺序打包会把槽 0/1 的数据塞进 CW0/CW5。
> 稀疏位图是**固有需求**（`wr 0 1` 今天就是 `0x01` 的 512 B 部分页编程），此前位图恒是**连续段**
> 才恰好用顺序打包对；方案 A 起必须换散射。

**触发**：① 窗口变化 ② 满 ③ `ftl_flush` ④ **确定性 idle flush**（`ftl_service` Phase 2.5 之后
`ftl_wbuf_maybe_flush_idle`：若无任何 used 组的下一 LBA 落在 open 窗口内 ⇒ flush）。**不引入超时**。

### 多组完成（`owners` 位掩码）

一页可能由**多个组**拼成（一个 FC 命令 = 一个 pending 槽）⇒ 完成时须对**每个**贡献组各递减一次
在飞数。`ftl_pending_req_t` 的 `uint16_t group` 换成 `uint32_t owners`；新增
`pending_group_complete_subop_multi(ftl, owners, sub_status)` 逐位置位调 `pending_group_complete_subop`
（`owners` 按值传入 = 局部副本，`on_complete` 重入复用槽不影响本循环）。

> ⚠ **`owner[8]` 逐槽写法是错的**：同一窗口内并发组覆盖同槽时旧组 `outstanding` 永不减 ⇒ 该 host
> 请求永久挂死。用 `owners` 位掩码（「哪些组向本页贡献过」）。

### 读 / TRIM / 掉电语义

- **读命中**：`ftl_wbuf_has(ftl, lba)` ⇒ 直接从 `wbuf.data[slot]` 拷回 caller，不进 outstanding；
  L2P 循环内遇 buffered LBA 即 `break` 收缩。
- **TRIM**：命中 ⇒ 清该槽 `occupied` 位、跳过 L2P/journal；窗口变空 ⇒ 退掉 append（逐组完成 OK）。
- **掉电语义（无电容）**：**不做数据日志，改「推迟完成」**——buffered CW 只在 SRAM；
  `ftl_confirm_write`（journal + L2P + BST）**只**在 flush 编程成功的完成处理里调用（与既有顺序一致）。
  ⇒ 不变量「L2P/journal 只引用**已编程**页」由构造保证。掉电时这些 host 写**从未收到完成**，丢失
  即正确契约；重启回放旧映射，被覆盖的旧映射完好。

### 顺带删死码

`allocator_alloc` **零外部调用者**（GC/WL v0.59 已全改 `allocator_alloc_pair`）⇒ 删函数体 + 声明 +
文档块 + 注释引用（`allocator.c` / `allocator.h`）。`grep -rn "allocator_alloc\b"` 只剩 `_pair`。

### 验证

```bash
make clean && make all      # 0 warning；AXI_RAM 291.2/384 KB（75.8%）；Binary size: 193516 B
make clean && make tests    # 0 warning（不改测试源）
```

**本批未上板。** 板上回归（须全绿）：`wr 0 8`→`rd 0 8 c`；`wr 0 4`→`rd 0 4 c`；`wr 4 8`（跨窗口）；
`wr`→`fl`→断电→重启→`rd`；`tr`→`rd`。**合并可观测性**：新增 CLI 诊断命令 **`wp <lba> <n> <k>`**
（一次提交 k 组同窗口小写，绕过 busy 闸门）⇒ 判据 = `info` 的 `total_writes` 只 +1（对比逐组的 +k）、
`ftl_wbuf_flush` 打一行、`rd` 回读各段字节 = 0xA0+组号。

### 局限（诚实登记）

收益需**多个组同时在飞**（pipelined host）。串行 host（CLI 现状）下「完成 = 持久」⇒ 两次写之间必然
flush ⇒ **不合并**；单个孤立的 4-LBA 写仍与今天相同（分配整对、编一个 plane）。本方案是**架构对齐
主流**（控制器形状）+ 为将来的流水线 HIL 铺路，**不是对串行 CLI 的吞吐改进**。

### 不做（本批）

`CONFIG_FC_RAID` 保持 0；`read_recovery.c` 保留地址 0x89 不碰；`journal.c`/`crash.c` 断言策略不变；
不动 `include/nand/nand_config.h`；不引入 DRAM 常驻 L2P；不重排 `GC_TRIGGER_*` 位值；
不把 `ftl_trim` 改成异步（只登记在飞 flush 的既有竞争）；不把 CLI 改造成队列（用最小诊断命令替代）。

---

## v0.67 — 修 `allocator_close_sb` 的 valid 顺序缺陷：运行期关闭的 SB 对 GC/WL 永久不可见

**日期**：2026-10-14

**范围**：① 生产缺陷修复（`src/ftl/allocator.c`，一行换序）；② 测试基建修复
（`tests/unit/ut_main.c` 套件路径展开）。**上板跑 FTL L0 UT 顶出来的**。

### 问题

板端 `test` 跑到第 4 例失败：

```
Error:test_ftl.c:345 assertion failed: a->closed_sb_list[0].valid == 0U (1 == 0)
```

`include/ftl/allocator.h:79` 的字段语义是**字段级**的：`valid` = `1=正在使用（Open），0=已关闭`。
GC（`gc.c:387`）、WL（`wear_leveling.c:129`）、`gc_erase_and_free`（`gc.c:220`）三处遍历都以
`sb->valid != 0U → 跳过` 为闸门，注释写的是「跳过仍为 Open 的 SB」；重建路径
`allocator_add_closed_sb` 也显式 `valid = 0U` 并在文档里注明「**`gc_select_victim` 依赖**」。

但 `allocator_close_sb` **先拷贝、后清 valid**，清的是**源**（`hot_sb`/`cold_sb`）而非**副本**：

```c
alloc->closed_sb_list[alloc->closed_sb_count++] = *sb;   /* 拷进去的仍是 Open 态 valid=1 */
sb->valid = 0U;                                          /* 只失效了 Open 槽 */
```

⇒ **运行期关闭的每个 SB 都带 `valid == 1` 进列表 ⇒ GC/WL 一律跳过**；只有开机重建路径的条目可见。
`allocator_close_open_sb` 的文档注释本来就写着「进 closed_sb_list + **valid=0**」——
**意图正确，实现写错了对象**。

### 影响（潜伏）

`free_count` 只出不进：水位跌破 `FREE_POOL_TARGET(32)` → `gc_check_triggers` 触发 T1 →
`gc_select_victim` 全表跳过 → 无 victim → 紧急 GC 无块可回收 → `allocator_alloc_pair` 返回
`NAND_PBA_INVALID` → **写永久失败**。WL 同理（一条候选都看不到，磨损均衡停摆）。

此前未暴露的原因：SB 需写满 64 页才关闭，GC 需 `free < 32` 才触发；板子开机后 `free=3710`
（`F2 rebuild blocks=2`），短测 `wr`/`rd` 触不到。

### 修法

`allocator_close_sb` 把 `sb->valid = 0U;` 提到拷贝**之前**，与重建路径收敛到同一约定。
两处调用者（`allocator_alloc_pair` 写满关闭 / `allocator_close_open_sb` 显式关闭）语义不变。

### 顺带：`test /ftl/` 恒「NO test run」

munit 的过滤（`tests/framework/munit.c:1297-1303`）要求过滤串 = **套件累积前缀 `pre` + 用例名前缀**。
用例挂在**叶子**套件下，其 `pre`（如 `/ftl/allocator/`）比 `/ftl/` 长 ⇒ 第一条 `strncmp` 即失配。
⇒ 只有叶子全路径（`/ftl/allocator/`）或全路径 + 用例名前缀能命中。

`ut_run_all`（`tests/unit/ut_main.c`）新增深度优先收集「持有用例的套件」全路径，把「以 `/` 结尾」
的实参展开为其下全部叶子套件路径。展开幂等，命中不到叶子的串原样透传。⇒ `test /ftl/` 按直觉工作。

### 判据

`make tests UT=FTL` / `make tests` 均 0 warning；上板 `test /ftl/` → 53/53 PASS
（`close_open_sb` 此前是必 FAIL 的一例）。

### 附：为什么 UT 值得先跑

本缺陷由 `test_alloc_close_open_sb` 一例直接顶出来；同一套件的 `test_gc_victim_skips_open`
（`test_ftl.c:817/831` 手工置 `valid = 1U` 模拟 Open 条目）**早已把「`valid != 0` = 跳过」的
正确约定写进了断言**。两例一个描述约定、一个验证生产者，合起来夹住这条潜伏缺陷——
v0.66 那轮 53 例纯静态审查没能发现它，因为缺陷在**生产代码**里，用例本身看不出矛盾。

---

## v0.68 — GC 触发求值加节拍门（+ 触发日志走自己的节拍 + 进展计数）

**日期**：2026-10-14

**范围**：`include/ftl/ftl.h`（4 字段 + 2 常量）、`src/ftl/ftl.c`（`ftl_gc_evaluate`
节拍门 + 日志节拍、`ftl_gc_reclaim_one` 进展计数、`ftl_init` 初始化）。
**纯后台调度口径与观测，不改任何判据语义**。

### 问题（板端顶出来的）

跑完 FTL L0 UT 后，板子开始刷屏：

```
[INF] [GC] trig=0x09 free=0 op=0 writes=0
```

`trig=0x09` = `GC_TRIGGER_WATERMARK | GC_TRIGGER_EMERGENCY`，`free=0` 与之一致。
`free=0 / op=0 / writes=0` 是**破坏性夹具的残留**（`ftl_fixture_reset` 调 `allocator_init`
不回填 Free Pool、并清零 `total_writes`，见 `test_ftl.c:71-101` 的文件头声明）——
这一半是既定代价，复位即消。

**但刷屏本身是生产缺陷**：`ftl_gc_evaluate`（`ftl.c`）挂在 Phase 3 的 IDLE/NORMAL 两条
路径上，而主循环 `while(1)`（`src/main.c:458`）**没有任何节拍** ⇒ 求值**每轮一次**。而
`trig != 0` 不是异常——`free_count < FREE_POOL_TARGET(32)` 正是 GC 的**正常工作区间**
（T1 的定义就是「低于水位」）。⇒ 任何把空闲池压在 32 以下的**持续写负载**都会让 NORMAL
逐轮打一行；GC 选不出 victim 的卡死态下则是每轮一行 + 每轮一次全表扫描。

### 根因：求值点没有节拍

`gc_check_triggers` → `gc_table_extremes`（`gc.c:313`）是**全表扫描**：

```c
for (uint16_t b = 0U; b < FTL_BLOCK_TOTAL; b++)   /* 2048 × PLANE_NUM(2) = 4096 块 ≈ 80KB */
```

T5 判据要全表磨损极值，所以这次扫描无法避免；但**它的频率不该等于主循环频率**。

⚠ **「主流按节拍求值」是错的**——本 ADR 初稿如此断言（"主流主控的后台 GC 跑在有节拍的
调度单元上，没有哪家每个 pass 无条件求值"），**已订正**：SPDK FTL 的 `ftl_core_poller`
注册周期就是 `0`（`lib/ftl/mngt/ftl_mngt_misc.c:208`：`SPDK_POLLER_REGISTER(ftl_core_poller,
dev, 0)`）＝连续轮询、**每轮求值**。它能这么做是因为判据是 **O(1)**：`ftl_needs_reloc()`
（`lib/ftl/ftl_core.c:291`）只做一次 `dev->num_free <= ftl_get_limit(SPDK_FTL_LIMIT_START)`
比较（默认 5 个自由 band，`lib/ftl/utils/ftl_conf.c:20`）。本仓 T5 是 O(4096) ⇒
**门控是成本补偿，不是向主流的节拍对齐**。

本仓既有同族做法：`rr_last_scan_ms`（1 s 巡检）、`wear_flush` 的节拍门、`cp_timer_start`（10 s）。

### 修法

① **节拍门**：`ftl_gc_evaluate` 入口按 `GC_EVAL_INTERVAL_MS(10 ms)` 门控，per-instance
记 `ftl->gc_eval_last_ms`。`0` 作「尚未求值」哨兵——平台时钟缺失时 `ftl_now_ms()` 恒 0，
此时每轮都求值（**GC 不能因缺时钟而停摆**；与 T3 定时「恒不触发」的取舍方向相反）。

② **日志走自己的节拍**：`[GC] trig=` 按 `GC_LOG_INTERVAL_MS`（1 s，与求值同一套时基，
`ftl->gc_log_last_ms`）打。**求值节拍不能兼作日志节拍**——前者管「多久算一次」，10 ms 拍
仍是 100 行/秒；日志要的是「同一状态别重复报」。语义 = **进入触发态立即一行 + 触发态内
每 1 s 一行心跳**：退出触发态时归零该字段，故下次进入必然立即打（**不需要另存触发掩码**，
早期草案的 `gc_trig_last` 已废弃）。心跳是刻意保留的——稳态静默是对的（T1 是常态），
但「一行之后再无消息」会被误读成已恢复。

③ **心跳行带进展计数**：`ftl_gc_reclaim_one` 内自增 `gc_reclaimed_count`（成功回收一个 SB）
/ `gc_novictim_count`（触发但选不出 victim），两计数进 `[GC] trig=` 行。
**为什么**：`trig=` 常亮对「在推进」与「卡死」一视同仁，唯一的进展证据 `[GC] victim`
（`gc.c:449`）只在选到 victim 时才打——卡死时**根本不出现**，其"消失"又与"已收敛"**同形**
⇒ 只有计数能一行判读。⚠ 这不是假想：`allocator_close_sb` 的 valid 顺序缺陷（**cf604ba**）
正是"触发常亮且永不收敛"的**生产形态**——运行期关闭的 SB 对 GC/WL 永久不可见 ⇒
`gc_select_victim` 恒选不出 victim ⇒ 空闲池只出不进 ⇒ `free_count` 掉到 0 后永不回升、
**写最终永久失败**。

### 代价（如实登记）

T1/T4 的空间压力响应也被门到同一周期 ⇒ IDLE 补池突发与 NORMAL 的「一轮回收一个 SB」
最坏延迟 10 ms。真卡死仍由写路径的按需 GC 兜底（`ftl_issue_subop`），且 10 ms 远小于
搬一个 SB（128 页）的 NAND 时间，实际不构成瓶颈。若日后要更快的空间响应，正确做法是
**把 T1/T4（O(1)）与 T5（O(N)）拆开**：便宜的每轮求值、贵的上节拍——本版未做。

### 判据

`make all` / `make tests` 均 0 warning。⚠ 体积按 **v0.68 提交本身**实测：product
**192976 B** / 全套 **322980 B**（早先记的 193804 / 323968 含随后那轮排查临时加进树的
诊断探针——`[F2] judge/blk/skip`、`[L2P] r=`——那些探针已在 v0.69 删除，不属于本改动）。
上板判据：① 跑完 FTL L0 UT 后**不再刷屏**（卡死态是 1 行/秒心跳，改前是全速刷）；
② 正常写负载下进入水位立刻一行、之后每 1 s 一行、离开水位后静默；
③ **卡死可辨**——卡死态 `reclaimed` 不动而 `novictim` 持续上涨；正常回收则 `reclaimed` 每行都涨。

---

## v0.69 — 修 `ftl_is_metadata_block` 的 plane 掩码：被掩的块落在所有池之外，L2P 悬空撞断言

**日期**：2026-10-14

**范围**：`include/ftl/metadata.h` + `src/ftl/metadata.c`（判据加 `plane`）、`include/ftl/bst.h`
（`ftl_gblock_plane/block` 单点解码）、`src/main.c` / `tests/unit/test_ftl.c`（调用点）、
`src/ftl/power_failure_recovery.c`（`page_is_blank` 读失败判非空）、`include/ftl/journal.h` +
`src/ftl/journal.c`（`journal_replay` 回填实际回放条数）、`src/controller/cli.c`（usage 只在参数个数非法时打）。

### 问题（板端顶出来的）

板端**重启后写一次**就撞断言：

```
[ERR] [FAULT] previous assert: b->valid_count > 0U (include/ftl/bst.h:261)
```

`ftl_block_sub_valid` 的断言 ⇒ 覆盖写时对**旧映射所在块**减有效计数，而该块的有效计数是 0。
即：**L2P 指向了一个 BST 认为「零有效」的块**。

### 根因：元数据块判据只比 per-plane 块号

`ftl_is_metadata_block` 三分支里，① 固定分区**与 plane 无关**（分片模型下各 die 用同一份块号），
但 ② 当前日志块 / ③ 已登记日志块是**从用户区动态取的**，旧实现一律只比 per-plane 块号：

```c
/* 旧：cur_metadata_block 是 gblock，却只比了它的低 11 位 */
if ((md_dir->cur_block_valid != 0U) && (block == (uint16_t)(md_dir->cur_metadata_block % BLOCKS_PER_PLANE)))
...
if ((entries[i].type == METADATA_TYPE_L2P_REGION) && (paa_to_block(entries[i].paa) == block))
```

⇒ **一个 plane 的日志块会连带把另一 plane 的同号块一起排除**。被掩的块：

- 进不了 Free Pool（`main.c` 回填用同一判据）；
- 被 F2 重建跳过（`ftl_rebuild_closed_sb_list` 用同一判据）⇒ `valid_count` **恒 0**；
- 而日志块会**迁移**（`metadata_dir_reclaim` / `metadata_dir_compact`），掩码块号随之漂移
  ⇒ 历史上只要有一刻它没被掩住、被 allocator 当用户块发出去写过数据，L2P 就指向它。

⇒ 此后**每次**覆盖写该 LBA 都会撞断言，而重启本身看不出任何异常——这正是
「只在重启后、且必须写一次才炸」的形态。

⚠ **原判「偏保守、安全，不修」是错的**（`ftl_optimization_plan.md §七.7` / §8.3 已订正）：
被掩的块**落在所有池之外**（既不在 Free Pool、也不在 Closed），GC 永远不会碰它 ⇒ 数据确实
不会被擦掉；但 `valid_count` 恒 0 这件事**本身就是断言条件** ⇒ 不是保守，是定时炸弹。

### 修法

① **判据加 `plane`**：`ftl_is_metadata_block(md_dir, plane, block)` —— ① 固定分区分支**不看**
plane，②③ 两条动态分支**必须**带 plane 比较。调用点三处：`main.c` free pool 回填、
`power_failure_recovery.c` F2 重建、`tests/unit/test_ftl.c` 夹具。
顺带在 `bst.h` 加 `ftl_gblock_plane/block`（`ftl_gblock` 的逆）做**单点解码**，
`metadata.c` 原有的 `gblock / BLOCKS_PER_PLANE` 内联算式改为调用它。

② **`page_is_blank` 读失败判「非空」**（同一类「F2 静默漏块」的另一支）：
`ftl_sync_read_page_raw` 刻意不判定 NAND 失败，返回 `FTL_ERROR` 只代表**命令**没走通
（提交失败 / 超时 / 无 buffer），此时契约规定 buffer 内容**未定义**。旧实现 memset 成 0xFF
后忽略返回码 ⇒ 读失败页被判空白 ⇒ `scan_block_write_pointer` 对该块返回 0 ⇒ 块被当成
"没写过"**留在 Free Pool** ⇒ 既可能被 allocator 拿去覆盖活数据，又让 L2P 指向 valid_count
恒 0 的块。判「非空」的代价只是真为空的块被拉出 Free Pool 多擦一次——不丢数据。

③ **`journal_replay` 回填实际回放条数**（`out_entries`）：旧实现用 journal 的 RAM 写指针
反推（`current_entry > 0` → `current_page × ENTRIES_PER_PAGE + current_entry`）——**恒假**：
`journal_replay` 末尾必调 `journal_resume`，后者把落点放到最新页之后并清 `current_entry`。
于是三条启动路径的 `journal_entries` 全是 0，该观测口不具判别力。改为在**应用循环**里逐页
累加 `entry_count`（读失败的页不计）。

④ **CLI 只在参数个数非法时打 usage**（`cli.c`）：旧写法把 `handler(...) != 0` 并进同一个
`||`，于是 handler 返回「执行失败」也被当成"参数错" ⇒ `wr 0 32` 在 busy 时会紧跟一条
`usage: wr <lba> <n>`，看着像语法错误、实际参数完全合法（wr/rd/tr/fl 四个命令的 busy 分支全中）。

### 上板证据（定位用的临时探针，结论已入档，探针已删）

| 探针 | 结论 |
|---|---|
| `[F2] judge page_seq=169 entry_count=32 sum=201 cp_jseq=169` | `sum(201) > cp_jseq(169)` ⇒ 判型**正确**走掉电路径，不是 v0.64 那个 off-by-one 假绿 |
| `[L2P] r=0 dir=1 b=322 pl=0 pg=8 … e=00027a00..03` | 条目解出来是 plane0 block317 page0 cw0..3，与 `[F2] blk b=317 ok=4` 严丝合缝 ⇒ **L2P 不是垃圾** |
| 5 条 `[F2] skip … bad=0 meta=1`（块 16/48/49/58/62 + 322） | 全落在固定分区 / 活日志块 ⇒ **判据无假阳性**（且 5 条全是 `bad=0`，坏块支也干净） |
| `[F2] rebuild blocks=8 partial=8 valid_cw=39` | 4(b317)+16(b321)+3(p1 b63)+16(p1 b2043) = **39**，与各块 ok 计数一分不差 ⇒ 重建**完整自洽** |

**旁证**：`free=3715` 反证 plane 修复已在运行镜像里——4096 − 126（固定 63 块 × 2 plane）−
1（活日志块）= 3969 = 3715 + 204 + 50；若判据还是旧的只比块号，活日志块 322 会连 plane1
的同号块一起掩掉 ⇒ 3968 ⇒ free 会是 3714。

### 保留的守卫（非临时探针，勿删）

`ftl_confirm_write` 里保留一条**常驻守卫**：覆盖写时若旧块 `valid_count == 0`，打一行
`[FTL] dangling lba=… old=… g=… st=… sb=… newg=… nst=…` 再进断言。它是这一类缺陷的
**唯一故障点**，且带旧块 `state` ⇒ 一次归因：

| `st` | 含义 |
|---|---|
| `0x01` BAD | 坏块，重建按判据跳过（数据本已不可读） |
| `0x08` FREE | 已归还空闲池 ⇒ 块还会被 allocator 复用，**丢数据** |
| RESERVED | 元数据日志块 ⇒ 判据过宽（v0.69 已修 plane 掩码） |
| `0x0000` | 重建从未认领（页 0 判空 / 未扫描到） |

代价 = 每次覆盖写多一次比较；收益 = 把「重启后写一次就死机」变成一条可读日志。

### 判据

`make all` / `make tests` 均 0 warning（product 194196 B / 全套 324608 B）。
上板：① 重启后跑写负载，不再出现 `[FTL] dangling` 与 `bst.h` 断言；② `free` 比修复前**多 1**
（每个活日志块不再白掩一个同号用户块）；③ `boot type=N journal_entries=N` 的条数不再恒 0。

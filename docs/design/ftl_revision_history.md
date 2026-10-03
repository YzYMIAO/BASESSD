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
- Controller 层：`scheduler.c` `state_machine.c` — 主机请求调度/状态机（`request.c` 已在 v0.23/v0.36 实现，非桩）
- NVMe 层：`nvme_admin.c` `nvme_io.c` `submission_queue.c` `completion_queue.c` — 当前用 UART CLI，5 个桩文件（含 nvme_controller.c 返回 NOT_SUPPORTED）
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
- NVMe 实现后自然过渡到方案④（主机级流控）

**不采用**：
- 方案②（内部排队重试）：需额外 SRAM，与 DRAM-less 设计相悖
- 方案③（动态 budget）：当前资源深度足够，预算固定即可
- 方案④（主机级流控）：NVMe 层还没实现

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
- P-B：scheduler / state_machine / NVMe 层（`#warning` 桩，6 文件）
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
- **C 档**：C1 controller state_machine 完整状态机、C1b scheduler 优先级调度、C2 NVMe host（nvme_io/admin/SQ/CQ/nvme_controller 全量）、C3 `SSD_REQUEST_FLUSH` 全实例 flush。
- **F8/F9**：依赖上板（紧急 CP 回切校验 / CQ·SQ 深度实测）。

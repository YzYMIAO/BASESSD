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
- Controller 层：`request.c` `scheduler.c` `state_machine.c` — 主机请求拆分/调度/状态机
- NVMe 层：`nvme_admin.c` `nvme_io.c` `submission_queue.c` `completion_queue.c` — 当前用 UART CLI
- Platform BSP：`timer.c` `uart.c` `cache.c` `dma.c` `pcie.c` `interrupt.c` — 已有 soc.h + DWT 实现
- Host Sim：5 个文件 — PC 模拟环境，当前跑在真实硬件

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

- P-A：pending 表扩展 8 → 16/32
- P-B：scheduler / state_machine / NVMe 层（`#warning` 桩）
- P-D：GC 加速等优化

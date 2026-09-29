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
| `include/ftl/completion_table.h` | **新增** | 16 项路由表 + `ftl_sync_waiter_t` + 统一泵声明 |
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
2. `FTL_PENDING_MAX` 扩展 — 从 8 增至 16/32
3. `ftl_write` 同步等待改为 per-cmd sync_waiter

### 编译状态

**零错误零警告**。新增 `completion_table.o` 398B text + 200B bss（16 项路由表）。FTL 层 22 个源文件。
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
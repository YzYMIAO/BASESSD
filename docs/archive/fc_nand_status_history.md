# FC-NAND 状态/待完善项历史归档

> 从 `docs/design/fc_nand_design.md` §8/§9 移出；正文不改写。

## 8. 已知待完善项

### 8.1 空壳/占位模块

| 模块 | 状态 | 说明 |
|------|------|------|
| `src/flash_controller/` | ✅ 已删除 | 死代码模块，已被 FC + nand_dispatch 取代 |
| `src/nand/bad_block.c` | ✅ 已删除 | 桩文件 |
| `src/nand/ecc.c` | ✅ 已删除 | 桩文件 |
| `src/nand/nand_command.c` | ✅ 已删除 | 桩文件，所有 ONFI 命令编码已在 `onfi_1_0.c` 实现 |
| `src/nand/nand_timing.c` | ✅ 已删除 | 桩文件，时序参数已在 `nand_op_map_s.timing` 中 |
| `fc_ecc_engine` | ✅ 已完成 | BCH(m=13, t=4) 编码/解码；QC-LDPC 硬判决编码/解码（可选） |
| `fc_raid_engine` | ✅ 已完成 | per-LUN START/ACCUMULATE/END 序列；fc_raid_recover 静态恢复 |
| `fc_data_path` | ✅ 已完成 | 写路径：Random→ECC→RAID；读路径：ECC→条件解扰（FC 内部）/ RAID 恢复（FTL 触发） |

### 8.2 接口待改进项

| 问题 | 说明 | 状态 |
|------|------|------|
| `nand_fc_*` 类型位置 | 已提取到 `nand_fc_interface.h`（通用接口头） | ✅ 已解决 |
| `nand_config.h` 命名歧义 | `PAGE_NUM→PAGES_PER_BLOCK`，`BLOCK_NUM→BLOCKS_PER_PLANE` | ✅ 已解决 |
| 缺少 Main/Spare 分离 | 已添加 `PAGE_MAIN_SIZE` / `PAGE_SPARE_SIZE` 宏 | ✅ 已解决 |
| `nand_device_t` 遗留 | 与 FTL 模拟路径共存，未来 FTL 迁移到 FC 接口后可移除 | 📝 已文档化 |
| 双向头文件依赖 | `nand_dispatch.h` **不再 include 任何 `fc/` 头**（2026-10-14 清掉一处零使用的 `fc/fc_types.h` 残留）；FC 在 submit 时预解析 Descriptor 链，nand_dispatch 不再回调 FC | ✅ 已解决 |
| `fc_command_handle` 编译 bug | `fc.c` 内部类型已统一为 `fc_cmd_id_t` | ✅ 已解决 |
| 死代码 `flash_controller/` | 模块已删除 | ✅ 已解决 |
| 桩文件清理 | `bad_block.c`, `ecc.c`, `nand_command.c`, `nand_timing.c` 已删除 | ✅ 已解决 |
| 双完成结构转换 | `fc_complete_from_nand()` 简化为直接字段拷贝 | ✅ 已解决 |
| **FTL 只判 `NAND_FAIL`** | FC 把 `UNC` / `CRC_ERROR` / `MATA_ERROR` 都 OR 进 `completion.status`（`fc.c:772` 全链聚合），但 FTL 五处判据只查 bit0 ⇒ **已检出的错误当成功交给主机**（`*status_out = SSD_STATUS_OK`）。本工程无 read retry / RAID 兜底（`CONFIG_FC_RAID=0`）⇒ 等效 UBER ≈ `P(误纠)` 而非 `P(误纠) × 2⁻³²`（理论见 §21.3.3）。**在 FTL 层，本次不改 `src/ftl/**`** | 📝 已文档化 |
| **GC / 掉电重建路径 META 缺显式校验** | `command->meta == FC_META_UNKNOWN` 时 步骤 4 交叉校验被跳过，META 只剩 3 位自洽 + **种子耦合**（隐式，且依赖加扰开启）。行业在此放显式校验（独立 CRC / 序号 / 冗余）。见 §5.3 | 📝 已文档化 |

### 8.3 功能扩展规划

| 阶段 | 内容 | 状态 |
|------|------|------|
| Phase 1 | 多 LUN 并发：nand_engine 异步步进 + 每 LUN 独立状态机 | ✅ |
| Phase 2 | DMA 数据传输：FMC 数据窗口改为 DMA 驱动，中断完成通知 | ❌ STM32H7 限制 |
| Phase 3 | 中断驱动：R/B# EXTI 替代轮询 WAIT_READY | ⚠️ 已回退：单 EXTI6 无法扩展到多 LUN，模块已移除，WAIT_READY 改回纯 GPIO 轮询 |
| Phase 4 | LDPC 硬解码实现 + BCH ECC 移植 | ✅ |
| Phase 5 | RAID 跨 Plane XOR 恢复 + 读路径管线拆分 | ✅ |

---

## 9. 测试

FC→NAND 测试以 **munit 单元测试**形式落 `tests/unit/`，在 STM32H743 核心板上运行
（源码与套件注册见 `docs/design/fc_nand_ut.md`）。此前独立的
`tests/integration/test_fc_nand.c` 集成测试**已删除**。

### 9.1 三级单元测试套件

| 套件前缀 | 文件 | 覆盖 |
|---|---|---|
| `/fc-nand/logic/` | `tests/unit/test_fc_level1.c` | 纯逻辑（掩码 / 地址 / 位图，不碰 NAND） |
| `/fc-nand/data_path/` | `tests/unit/test_fc_level2.c` | 数据路径（建链 / 提交 / 收发） |
| `/fc-nand/lifecycle/` | `tests/unit/test_fc_level3.c` | 全链路（擦写读 / multi-plane / 迁移） |

> ⚠ 套件前缀**必须带尾斜杠**（munit 只把前缀与用例名首尾相接，不补分隔符）。

### 9.2 串口控制台命令

```
help                               → 命令列表
status / version                   → UART 计数器 / 固件版本
log level <error|warn|info|debug>  → 运行期日志等级
wr <lba> <n>                       → 写 n 个块（固定模式）
rd <lba> <n> [c]                   → 读 n 个块（c=比对模式）
tr <lba>                           → trim 一个 LBA
fl                                 → 刷脏 L2P + journal 到 NAND
info                               → 控制器 + FTL 统计
bbt [--erase]                      → 打印出厂坏块图（--erase 逐块擦）
test [选项] [用例名…]              → 跑 munit 测试套件（仅 FC_NAND_TEST_ENABLE 构建）
```

`test` 选项：`--seed N` / `--iterations N` / `--single` / `--list` / `--list-params` /
`--log-visible info|debug`。跑全套**不带过滤串**；按套件过滤用带尾斜杠的前缀。

### 9.3 编译

```sh
make clean && make tests
# 烧 build/firmware/debug/bin/debug.bin
```

> ⚠ `make all` 之后直接 `make tests` 是**空操作**（目标未按测试 flag 重编）⇒ 必须先 `make clean`。

---

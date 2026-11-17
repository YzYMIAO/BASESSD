# soft_ssd — STM32H743 软 SSD 固件

STM32H743 + MT29F4G08（4 Gb SLC NAND）上的 **DRAM-less 软 SSD 固件原型**：
主机经 USB MSC 把它当作 U 盘访问，固件内部按 HIL / FTL / FC-NAND 分层实现。

> 这是**学习存储主控设计**的原型项目，不是产品固件。验证范围以本文「状态」表为准。

## 架构

| 层 | 目录 | 职责 |
|---|---|---|
| HIL | `src/hil/` | 主机接口（USB MSC、UART CLI）、请求拆分与排队 |
| FTL | `src/ftl/` | L2P 映射、GC、WL、Journal、Checkpoint、掉电恢复 |
| FC-NAND | `src/fc/` + `src/nand/` | 命令/描述符/缓冲池、UOP 引擎、ECC/CRC、FMC 时序 |
| DRAM | 无独立层 | DRAM-less：元数据常驻 AXI_RAM / SRAM2 |

分层说明见 `docs/architecture.md`；行业对照与分层判据见 `docs/design/fc_nand_design.md` §26。

## 硬件假设

- MCU：STM32H743（Cortex-M7），FMC bank 3 接 NAND
- NAND：Micron MT29F4G08（ONFI 1.0，4 Gb，x8），单颗、单 LUN
- 主机接口：`USB2_OTG_FS` device mode，全速 12 Mbps，MSC/BOT，单 LUN，512 B 块，主机可见容量约 424 MiB
- 串口：USART1，115200 8N1（CLI 与日志）
- 调试：J-Link（SWD）

引脚、时钟与内存布局见 `docs/hardware_contract.md`。⚠ 板卡为 Apollo V2，USB 引脚映射未经原理图确认。

## 构建

依赖：`arm-none-eabi-gcc`、`make`、`clang-format`（`make all` 前置格式检查）、`python3`（`make types-check`）。

```sh
make all            # debug 构建（含 format + types-check）
make release        # release 构建
make tests UT=FTL   # UT 编译/链接；UT=FC|FTL|HIL，留空 = 全部
```

产物在 `build/<debug|release>/`。`make tests` **只证明编译与链接**；UT 必须在目标板上运行，
本项目不做 host 模拟。CI（`.github/workflows/build.yml`）只跑构建与链接，不上板。

## 烧写与上板

```sh
./sh/debug_env.sh    # WSL 下启动 JLink 链路
# 之后在 VS Code 用对应 launch 配置烧写 / 调试
```

完整调试链（WSL ↔ JLink ↔ 串口）见 `docs/guides/wsl_debug_environment.md`。
串口 CLI 命令见 `src/hil/cli.c`（`format`、`test`、统计类命令等）。

## 状态

| 项目 | 状态 | 证据 / 说明 |
|---|---|---|
| debug / release 构建 | 已验证 | 2026-11-17，本机 arm-none-eabi-gcc，零新告警 |
| UT 编译（FC/FTL/HIL） | 已验证 | 2026-11-17，`make tests UT=FTL` 链接通过；仅证链接 |
| 板端 UT（release 全量） | 已验证 | 2026-11-16，306/306 通过、3 SKIP、636 s（`test --seed 0x5ee5cfbc`） |
| 板端 H2testw | 已验证 | 2026-11-16，415/423 MB、0 错误；写 950 / 读 885 KB/s |
| `CONFIG_FC_RAID` | 关闭，未验证 | `include/fc/fc_interface.h`；功能代码完成但未上板 |
| IWDG 看门狗 | 关闭，待上板决策 | `src/main.c`（`SSD_WATCHDOG_ENABLE=0`） |
| read recovery `0x89` | 保留地址，空转 | `src/ftl/read_recovery.c`；本器件无读电压重试表，不处理 |

> 「已验证」仅覆盖表中证据对应的范围；未列出的分支与配置均为未验证。

## 已知限制

- **DRAM-less**：L2P 等元数据常驻片上 SRAM，容量与性能受 AXI_RAM / SRAM2 预算约束。
- 主机接口是 **USB MSC BOT**（单命令）+ UART CLI；没有 NVMe SQ/CQ/doorbell，也没有原生多队列。
- RAID 关闭；看门狗关闭。
- NAND 层尚未完全走平台抽象：`src/nand/nand_fmc.c` 直接访问 FMC 窗口。
- 只支持 STM32H743 + MT29F4G08 这一目标；没有 host 侧模拟。
- 板端为单颗 NAND（单 LUN）；多 LUN 路径未上板验证。

## 文档地图

| 入口 | 内容 |
|---|---|
| `docs/README.md` | 按任务索引的文档地图 |
| `docs/architecture.md` | 分层与平台抽象 |
| `docs/hardware_contract.md` | 硬件契约、内存布局、USB 配置 |
| `docs/design/ftl_design.md` | FTL 主设计 |
| `docs/design/fc_nand_design.md` | FC-NAND 主设计（§26 行业对照） |
| `docs/design/hil_design.md` | 主机接口层 |
| `docs/guides/wsl_debug_environment.md` | WSL / JLink / 串口调试链 |
| `docs/design/known_open_items.md` | 已知风险与残余项（维护者账本） |
| `CLAUDE.md` | 工程规则与评审判据（不是用户手册） |

## License

项目代码采用 Apache-2.0，见 `LICENSE`。第三方组件（TinyUSB、STM32H743 HAL/CMSIS、μnit）
遵循各自上游条款，见 `THIRD_PARTY_NOTICES.md`。ST 组件的许可原文见
`third_party/stm32h743/LICENSE`。

## 问题反馈

仓库公开后，问题与建议请开 GitHub issue。安全敏感问题请先在 issue 中只说明「希望私下沟通」，
不要在公开区贴利用细节。

# 设计文档索引（design/）

本目录是活跃权威设计文档（主设计 12 篇，含 request scheduler 设计稿 + [`common/`](common/) 公共设施 6 篇 + [`revision_history/`](revision_history/) 决策史 2 篇）。先读哪个
取决于任务 —— 导航表在 [`../README.md`](../README.md)。这里只给**阅读顺序**与
**哪些是内部账本**，不复制设计内容。

## 阅读顺序

| 层 / 主题 | 主设计（先读） | 决策史 | 测试设计 |
|---|---|---|---|
| 系统总览 | [`../architecture.md`](../architecture.md) → [`../hardware_contract.md`](../hardware_contract.md) | — | — |
| 启动流程 / Boot policy | [`../boot_flow.md`](../boot_flow.md)、`../src/boot/`（当前未接入 main） | — | — |
| FTL | `ftl_design.md`（含 §3.4.10 request scheduler；ADR/主流对照见 `controller_target_features.md` §8/§9） | `revision_history/ftl_revision_history.md` | `ftl_ut_design.md` |
| FC-NAND | `fc_nand_design.md`（§6.2 行业对照 = 判据） | `revision_history/fc_nand_revision_history.md` | `fc_nand_ut.md` |
| HIL / USB | `hil_design.md` → `usb_msc.md` | — | `hil_ut_design.md` |
| 平台 / 移植 | `platform_design.md` | — | — |
| 测试通用方法 | `../guides/testing_guide.md`（§12 = 负例与注入框架） | — | — |
| 公共设施 / 横切规则 | [`common/README.md`](common/README.md)（assert / fault / logging / cli / timer / munit） | — | `common/munit_adapter.md` |
| 设计原则 / 学习 | `storage_controller_design.md`（设计要点手册） | — | — |
| **存储主控 SoC 介绍** | `controller_target_features.md`（硬件功能块 + FTL/固件功能块 + 消费/企业差异 + 当前平台案例 + 候选平台） | — | — |

## FTL 文档内部分工

`ftl_design.md` 只写设计；以下内容按归属放在别处：

| 想知道 | 读 |
|---|---|
| FTL 对外接口（初始化 / 提交 / 调用约束） | 公共 FTL 接口头 + `ftl_design.md` §5.1（模块契约） |
| 逐版本决策史 / 上板记录 | `docs/README.md` 的文档地图 |
| 多 LUN / 共享 cache | `ftl_design.md` §5.4 |
| CW 级映射与介质布局 | `ftl_design.md` §2.1 / §2.2 / §2.3 |
| FC / NAND 命令、ECC/CRC、地址层 | [`fc_nand_design.md`](fc_nand_design.md) |
| 存储主控设计要点 | [`storage_controller_design.md`](storage_controller_design.md) |
| 断言判据 | [`common/assertion_guidelines.md`](common/assertion_guidelines.md) |

⚠ **签名以头文件为准**。`ftl_design.md` 给结构形状与字段语义，不复制完整函数原型——接口改动频繁，
文档复制签名必然漂移。当前 host / 启动公共入口见 `ftl_interface.h`（API v1）；CLI 诊断仍是已文档化的内部例外。模块内部头已移入 FTL 模块目录。

## FTL 模块阅读顺序

以下 `§x.y` 均指 `ftl_design.md` 的章节。先读 **§1.4 总图 + §1.5 路径表**，建立“一次 host
读写从哪进、经过谁、从哪出”的地图；然后按下面的顺序读，**不要从 §3.6 metadata 开始**：

| 顺序 | 模块 | 为什么先看它 |
|---:|---|---|
| 1 | **§3.1 Host 提交 / group / pending** | 整个 FTL 的入口。先理解“一条 host 请求如何拆成子操作、如何用一个组聚合完成”，后面所有异步 job 都是同一套骨架 |
| 2 | **§3.2 完成派发面（ftl_op）** | 与 §3.1 配对：提交与完成解耦，op tag 决定谁收尾。不懂这条链路，后面每个 job 的 `waiting/phase` 都会显得莫名其妙 |
| 3 | **§3.3 写缓冲 wbuf** | 一次 512 B 写如何攒成一页、双 plane 如何合并。写路径最直观的一层 |
| 4 | **§3.5 L2P Cache** | 读命中/未命中、异步加载、脏行回写。**这是 metadata 的前置** |
| 5 | **§3.6 元数据目录** | 现在再看：region 为什么不在固定位置、append/commit 为什么分离、live 位图 干什么、目录检查点为什么存在 |
| 6 | **§3.7 Journal + §3.8 Checkpoint** | 写回之后的掉电一致性：增量日志 + 全量快照 + 截断顺序 |
| 7 | **§3.9 分配器 / BST / Super Block** | 页从哪来、块状态怎么变、Free/OP/Spare/Closed 四类池的计数。GC/WL 的前提 |
| 8 | **§3.10 BBT + §3.11 WL + §3.12 GC / 迁移** | 后台如何回收空间、均衡磨损、处理坏块 |
| 9 | **§3.4 service / 后台调度** | 回头看这些 job 如何在单线程 superloop 里被调度、为什么有 must-run |
| 10 | **§3.13 读恢复 + §3.14 启动恢复** | 异常路径：读失败怎么救、掉电后怎么重建 |

最后用 **§4.5 主机写**、**§4.6 L2P 回写**、**§4.7 Checkpoint** 三个场景把前面串一遍；
状态机记不住时查 §5.2。

### 如果只想先攻克 metadata

按这个最小闭环读：

1. §2.2 地址模型（LBA / region / PBA）
2. §3.5.4 读路径、§3.5.5 写路径与写回
3. §3.6.5 ring / locate / prepare
4. §3.6.6 lookup 与启动加载
5. §3.6.7 append → 写页 → commit
6. §3.6.8 live 位图 / 回收 / 压缩
7. §3.6.9 目录检查点

原因：`metadata.c` 里的 `md_slot_pair_append` / `md_commit_region` / live 位图 不是凭空出现的——
它们是 **L2P cache 脏行回写**的落盘端。先看 §3.5 的 `l2p_flush_kick` 怎么调用它们，
再回头看 §3.6，顺序就顺了。

### 其它三条快速路径

| 路径 | 顺序 |
|---|---|
| **按上电顺序**（想知道盘怎么活过来） | §1.1~1.3 → §2 → §3.14 引导恢复 → §3.9 分配器 → §3.10 BBT → §3.11 磨损 → §3.6 元数据目录 |
| **按写路径**（想知道一次 host 写经过谁） | §1.4 总图 → §3.1 提交/组 → §3.3 wbuf → §3.9 分配 → §3.2 完成派发面 → §3.5 L2P cache → §3.7 Journal → §3.6 元数据目录 → §4.5 |
| **按后台任务**（想知道空闲时在做什么） | §3.4 调度器 → §3.12 GC/迁移 → §3.11 WL → §3.8 Checkpoint → §3.13 读恢复 |

## FC-NAND 模块阅读顺序

以下 `§x.y` 均指 [`fc_nand_design.md`](fc_nand_design.md) 的章节。先读 **§0 摘要 + §1.4 模块
全景与数据流**，建立「一条命令从 FTL 提交到 NAND 完成经过谁」的地图；然后按下面顺序读：

| 顺序 | 章节 | 为什么先看它 |
|---:|---|---|
| 1 | **§1 / §2** | 分层坐标、模块表、三套地址与 CW 布局。地址搞混，后面每个模块都看不懂 |
| 2 | **§3.1~§3.5** | FC 前端：公共接口、三池与 buffer 归属、命令 / 描述符、队列、调度器。提交侧全貌 |
| 3 | **§3.6~§3.7** | 数据路径（编解码）与 dispatch 边界。看懂「哪些字段跨入硬件、哪些留在固件」 |
| 4 | **§3.8~§3.14** | NAND 侧：UOP 引擎、硬件后端、命令表、器件模型、multi-plane、并发相位机、时序 |
| 5 | **§4** | 六个端到端场景。初始化 → 命令生命周期 → 按 CW 子集读 → cache run → Feature 内联读 → multi-plane 批次 |
| 6 | **§5 → §7** | 字段归属、不变量、断言判据，然后看未验证项与已知限制 |
| 7 | **§6** | 分层对照与「硬件 FCC 寄存器表」接口判据——加接口前回来对照 |

只想先看懂「一条读命令怎么走完」：读 §4.2 的走读表，再回头按 §3.5 → §3.7 → §3.8 补细节。

## 内部账本（维护者用，公开阅读可跳过）

- `known_open_items.md`：未闭合风险（`RISK-*`）与未实现/未验证缺口（`ISS-*`）台账；开头是未闭合看板，条目按模块 1~16 分节。**复查模块前先读。**
- `known_closed_items.md`：已闭合 / 不实现条目的索引与详情；从开放式台账迁入后只追加，不改写历史。
- 各层 UT 设计文档的 **§7.2 负例判定台账**：M（红过且上板）/ N（缝已确认、差上板）/
  K（不可构造 + 理由）与状态词，按层分册，不集中在一处。
- `revision_history/*_revision_history.md`：逐版本决策史与被否方案（回答「为什么不是另一种做法」）。

# 已知未实现 / 未验证项（known_iss）

> **边界**（与 `known_risks.md` 的分工，R11）：
> - 本文件 = **设计 / 计划 / 测试中尚未实现或尚未上板验证**的条目（feature & verification backlog）。
> - `known_risks.md` = **缺陷、静默失效、残余风险**（含已修未上板的实现风险）。
> - 同一件事只在一处登记；跨类引用写 ID，不复制内容。
>
> 来源文档多为一次性计划/设计稿，已按 2026-10-22 文档重组移入
> `docs/archive/`；条目保留来源文件名，便于追溯全文。
>
> 状态词：`未实现` · `部分实现` · `未上板` · `待决策` · `待硬件` · `待测试源`。

## 1. UT / 系统集成

| ID | 状态 | 条目 | 来源 | 下一步 / 判据 |
|---|---|---|---|---|
| ISS-UT-01 | 已实现（2026-10-24 上板） | FTL UT 按 `docs/design/ftl_ut_design.md` 重建：**63 个用例函数 / 参数化 85 次运行**，`test /ftl/` 全过（61.05 s，含 format） | `docs/design/ftl_ut_design.md`；`tests/unit/ftl_ut_common.*`、`test_ftl_pure.c`、`test_ftl_media.c` | 未落地项只剩需故障注入/空白片/白盒构造的部分：M-11 真介质注入、CP 版本不符、真空白片、NAND 写擦失败注入；见设计文档 §4.5/§7 |
| ISS-HIL-01 | 未实现 | HIL L0 纯 RAM UT 9 例（state_machine / controller / scheduler / request）；`tests/unit/test_hil.c` 与 `g_hil_*_tests[]` 均不存在 | `../archive/hil_ut_design.md` Part A / 覆盖矩阵 | 先裁决 `ssd_controller_route` / `run_end` 是否导出到 `controller.h`；再按 `munit_adapter.md` 接线 |
| ISS-HIL-02 | 部分实现 | HIL L2 验收 24 例：基础读写 9、拆分/聚合/背压 4、跨重启 8、后台/水位观测 3。v0.58~v0.66 期间可能用 CLI 人工跑过，但**无逐条上板记录** | `../archive/hil_ut_design.md` Part B | 按 `usb_msc_verification.md` 的矩阵补记录；跨重启 8 例优先（`reboot_persist_*` 已修，需确认） |
| ISS-HIL-03 | 待测试源 | CH/CE/LUN>1 后的 HIL 扩维用例（route / run_end 在 1 维下过于平凡） | `../archive/hil_ut_design.md` §3 | 等真实多实例硬件或 N=2 影子构建；勿在 1 维伪造覆盖 |

## 2. NAND OP 启用（方案已评审，未改代码）

| ID | 状态 | 条目 | 来源 | 下一步 / 判据 |
|---|---|---|---|---|
| ISS-OP-01 | 未实现 | **P1：GC/WL 用现有 multi-plane 写（OP 31）**；当前 `mig_write_submit` 仍逐 plane 提交（与 RISK-PERF-02 同一问题） | `../archive/nand_op_enablement_plan.md` §3.1 | 先完成 ISS-OP-04 的 Phase 0；P1 的判据是写放大下降、host 写速不降 |
| ISS-OP-02 | 未实现 | **P2：Copy-Back 复合 op 45**；仅限整页全有效、同 plane、非 metadata、源 ECC 干净四条件 | 同上 §3.2 | 依赖 P1 与 Phase 0 结论；方案 A/B 选型待定 |
| ISS-OP-03 | 部分实现 / 部分上板 | **P3：Two-plane Cache Program（op 44）**：host 16 KiB 正常路径已上板（H2testw 415/423 MB，843/889 KB/s，`cw` 24906/99624/0 abort）；GC/WL batch 已接 FTL 但本次 `mb=0` 未触发；FAILC 负例、失败注入、掉电注入、静态 WL high-wear 批次未做 | 同上 §3.3；`docs/design/ftl_design.md` ADR-15；`ftl_revision_history.md` v0.94/v0.96 | 补 FAILC 负例/掉电注入；GC batch 需要构造可批的 victim 后再验 |
| ISS-OP-04 | 未实现 | **Phase 0 测量**：5 项假设埋点（host 写分块 / NAND program 次数 / GC 占比等）+ Stop/Go 判据 | 同上 §5 | 只加计数器不改行为；Stop 条件见 §5.4 |
| ISS-OP-05 | 待决策 | D1~D5（是否先做 P1、copy-back 选项 A/B、cache program 是否进入路线、op 表扩展方式等） | 同上 §8 | 需用户裁决后才能排期 |
| ISS-OP-06 | 未实现（边界） | Cache Read、ISSUE/WAIT/FETCH、Block Lock/OTP/on-die ECC：**当前明确不做**，不属于待办 | 同上 §3.4/§3.5 | 换器件或多 LUN 后重估；不要按“op 表剩余码全开”实现 |

## 3. FTL 优化计划遗留

| ID | 状态 | 条目 | 来源 | 下一步 / 判据 |
|---|---|---|---|---|
| ISS-FTL-01 | 部分实现 | P2-3 OOB/spare 通路：出厂坏块检测已改为 Page0 META 抽样，**OOB 标记通路未启用** | `../archive/ftl_optimization_plan.md` §8.1 | 二手片出厂标记已丢（AN1819），当前不追；换新片/需要出厂坏块图时重估 |
| ISS-FTL-02 | 待决策 | P2-4 band 级 sequence/validity 聚合：消费者已被 journal 恢复取代，判定为死字段 | 同上 §8.1 | 确认关闭并删除残留代码/文档；不要重新引入第二套坐标 |
| ISS-FTL-03 | 未实现 | GC Bitmap（Layer 2）断点续搬；正确性窗口已由“擦除前 flush”闭合，仅剩性能收益 | 同上 §8.2 | 需要 GC 掉电续搬性能数据后再排期 |
| ISS-FTL-04 | 未实现 | P3-1 Prefetch 顺序读 | 同上 §8.2 | 前沿项，未排期 |
| ISS-FTL-05 | 未实现 | P3-2 多流 host stream hint（当前仅双温区） | 同上 §8.2 | 企业级方向，未排期 |
| ISS-FTL-06 | 未实现 | P3-3 学习型 L2P 压缩（LeaFTL 方向） | 同上 §8.2 | 工程量大，未排期 |
| ISS-FTL-07 | 待定位 | 擦除 op 18→17：固件侧实测零改写，原因未闭合 | 同上 §8.5；`RISK-...` 见 known_risks 相关条 | 插桩停在 `test_fc_level3.c`；未出结论前不改 op 映射 |
| ISS-FTL-08 | 未上板 | F8 / F9（依赖上板的跨层项） | 同上 §8.5 | 按原条目补板端证据后再定 |
| ISS-FTL-09 | 待修 | `HAL_RCCEx_GetPeriphCLKFreq` 对 FMC/QSPI/USART1 恒返 0（HAL 无这三个 case） | 同上 §8.5；平台层 | 平台层自建频率查询或固定表；不要在 FTL 侧兜 |
| ISS-FTL-10 | 未上板 | FMC HiZ 是写方向：写压力测试未做 | 同上 §8.5 | 长时写 + 拔电回归 |
| ISS-FTL-11 | 未实现 | D4/D5：HIL 队列 + 门铃 + CQ（更大重构，主控学习方向） | 同上 §8.5 | 单独设计；不要与 USB BOT 单命令模型混用 |
| ISS-FTL-12 | 未实现 | 跨 LUN superblock 条带 / 跨 LUN GC/WL（当前严格 per-LUN） | `../archive/multi_lun_generalization.md`；`RISK-ARCH-02` | 需要设备级 relocation 引擎；先在真实多 LUN 上做基线 |

## 4. 多 LUN 泛化

| ID | 状态 | 条目 | 来源 | 下一步 / 判据 |
|---|---|---|---|---|
| ISS-ML-01 | 未上板 | 多 LUN / 共享 L2P cache Phase A/B/C：仅影子 N=2 链接，无实机 | `../archive/multi_lun_generalization.md` | 需要真实多 die/多 LUN 硬件 |
| ISS-ML-02 | 未实现 | Phase D：审计、文档、验证清单未开始 | 同上 Phase D | 依赖 ISS-ML-01 |
| ISS-ML-03 | 待决策 | 目标 LUN 数（2 / 4）与是否需要物理并行通道 | 同上 §9 | 用户裁决；与 cw 文档 §3.7 的 `LUN_ADDR_WIDTH` 结论对齐 |
| ISS-ML-04 | 未实现 | 跨 LUN GC/WL / 条带（同 ISS-FTL-12） | 同上 | 单独设计，不在当前 per-LUN 引擎上打补丁 |

## 5. USB / 验证手册待测项

| ID | 状态 | 条目 | 来源 | 下一步 / 判据 |
|---|---|---|---|---|
| ISS-USB-01 | 未验证 | 写过程中掉电恢复（USB-V08） | `usb_msc_verification.md` §8 | 写 32–64 MB 中拔电，重启后卷可挂载、哈希一致 |
| ISS-USB-02 | 未验证 | 吞吐 / `dd` 基线（USB-V09） | 同上 | 记录顺序写/读、4K 随机；与 RISK-NAND-04 的瓶颈定位合并 |
| ISS-USB-03 | 未验证 | WRITE(10) 的 **FUA** 位语义（只发 FUA 不发 SYNCHRONIZE CACHE） | 同上；`RISK-USB-05` | 抓包确认主机是否用 FUA；若用，写完成路径补 flush |
| ISS-USB-04 | 未验证 | 多分区 / 第二分区（>128 MB）写满 + 校验 | 同上；历史 rev33 第二分区失败见 RISK-MIG-07 | v12 介质上重跑；与全盘 H2testw 分开记录 |

## 6. 平台 / 日志 / 故障捕获

| ID | 状态 | 条目 | 来源 | 下一步 / 判据 |
|---|---|---|---|---|
| ISS-PLAT-01 | 未实现 | 平台抽象 Stage 3：NAND 层裸 MMIO / `HAL_NAND_*` 收敛 | `platform_abstraction.md` §五 | 单独一轮；改后地址/时序类量必须上板 |
| ISS-PLAT-02 | 待决策 | `src/platform/stm32h7r3/` 是继续维护还是废弃 | 同上 | 用户裁决；未构建平台不要顺手改 |
| ISS-PLAT-03 | 未修 | linker `.noncacheable_buffer` 输入段漏前导点，落点随启发式漂移 | `RISK-PLAT-01` | 单独一轮：`KEEP(*(.noncacheable_buffer))` + 上板回归 DMA |
| ISS-LOG-01 | 未上板 | `logging_design.md` §5 的 9 项落地状态：`[STAT]` / `[MIG] done` 已在 2026-10-22 日志出现，其余（飞行记录器、打印预算逐项）未逐条确认 | `logging_design.md` §5/§6 | 按 §6 判据逐项打勾；未验的不要写成已验 |
| ISS-FAULT-01 | 未验证 | `fault_capture.md` 三项手工验证：人工 BusFault、断言失败 dump、跨复位读取 `.noinit` 记录 | `fault_capture.md` §6/§9 | 烧录→拔电→上电确认无记录；再逐个注入 |

## 7. 架构 / 契约残余

| ID | 状态 | 条目 | 来源 | 下一步 / 判据 |
|---|---|---|---|---|
| ISS-ARCH-01 | 已上板（2026-10-25） | 模块 handle 契约迁移（Phase 0~8）：正常读写/GC/CP/journal/BBT 已由 `/ftl/` 85/85 与 H2testw 415 MB 覆盖；**掉电/格式化中断/故障注入仍未单独验**（见 RISK-BOOT-22/BBT-18 等） | `../archive/ftl_module_contracts.md` §12；`RISK-ARCH-06`（已闭合，archive §2026-10-25） | 剩余只跟随 BOOT-22/BBT-18 等专项条目推进，不再单列契约回归 |
| ISS-ARCH-02 | 待决策 | 契约正文（目标设计）与已落地签名存在偏离（`jrn_handle` trigger、`wbuf_handle` 返回码、`pending_confirm_*` 入参） | `../archive/ftl_module_contracts.md` §12；`RISK-ARCH-10` | 定“权威契约以哪份为准”；定稿前不要按文档强行改签名 |
| ISS-CW-01 | 待决策 | `LUN_ADDR_WIDTH=0` 与多维地址构造的根因结论是否仍成立（cw 文档 §3.7 与 v0.73/v0.90 多维改动需对齐） | `../archive/cw_level_mapping_design.md` §3.7 | 与 ISS-ML-03 一起裁决 |
| ISS-CACHE-01 | 部分上板（2026-10-30 已改；host 4 页链 H2testw 415/423 MB 无错；跨 pass/失败页未验） | **cache read/program 序列依赖「全部 FTL 命令走 SQ 且连续入队」**：若 RR 等改走 HSQ，`fc_poll_fill` 先扫 HSQ，命令会插入 31h/3Fh（或 15h/10h）序列，破坏器件 cache 管线。FC 已有 `config.chain` + `chain_owner` 链锁可解 | `docs/design/ftl_design.md` §3.9.5 / ADR-17；`include/fc/fc_scheduler.h:28-49`；`src/fc/fc.c` 的 `fc_poll_fill_queue` / `fc_poll_chain_length`；`src/ftl/cache_batch.c`（read/write 链标记）；`src/ftl/migrate.c`（GC/WL batch 链标记） | **方案 A 已落地**：cache read/program、GC/WL batch 都标成一条 FC chain（末条 `FC_CHAIN_END`，其余 `FC_CHAIN_MORE`），经 `ftl_op_submit_batch` 原子入队；FC 零功能改动。判据：① 上板跑 FC UT `test_chain_priority_lock`（8 条跨 pass 链 + HSQ 回推）；② host 顺序读（4 条链上 45h/46h/47h 不交错，H2testw 校验通过）；③ GC batch `mb>0`。⚠ chain 写路径不处理 RAID stripe encode（`known_risks.md` RISK-IF-06） |

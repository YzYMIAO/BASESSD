# 风险负例测试清单（可执行）

> 目的：把 `docs/design/known_risks.md` 里「待上板」的负例判据整理成可逐条执行的清单。
> 每条都给出：风险 ID、触发方式、期望、证据。**未上板前不得勾选**（R1）。
>
> 当前硬件：无程控电源 / USB 继电器 ⇒ 掉电与拔线只能手动，无法自动化与大批量。
> 已有能力：测试镜像的固件注入钩子（`FC_NAND_TEST_ENABLE`）+ 串口 CLI + H2testw/`/speed`/UT。

---

## 0. 前置条件

| 项 | 要求 |
|---|---|
| 测试镜像 | `make tests UT=FTL`（或 `UT=FC` / `UT=HIL`）——只有 `make tests` 定义 `FC_NAND_TEST_ENABLE` |
| 注入钩子 | `fault_inject.c/h` 与 `bbt fault` **只在测试镜像存在**；产品 debug/release 均不编入 |
| 串口 | USART1 115200，`log level debug`（按需） |
| 主机 | H2testw（全盘写读）、`/speed`（FTL/FC 压测） |
| 判据取证 | 串口原始日志 + `info` / `ftl_ctx` 输出 + H2testw 报告 |

> ⚠ 注入类用例必须在**测试镜像**上跑；产品镜像跑不出这些路径（钩子不存在）。
> ⚠ `bbt fault` 会永久标坏块（写 BBT），跑完建议 `format --verify` 恢复介质。

---

## 1. NAND 写失败 / program fail

- [ ] **RISK-HOST-06** 写重试
  - 触发：`bbt fault <plane> <block>` 标坏即将写入的 data 块，再 `wr` 同一区域。
  - 期望：串口出现 `[FTL] write retry`，host 写仍成功；第二次失败如实 `IO_ERROR`。
  - 证据：串口行 + `info` 的 `bad` 增长。
- [ ] **RISK-CACHE-30/31/32** cache program 失败/撤销
  - 触发：`fi_cache_fail_arm <lba>`（测试镜像），写该 LBA 后读回。
  - 期望：失败页及其后页不确认映射、之前页已确认；无 `pad: stray page`；`cache_failc_abort` 类 UT 同步通过。
  - 证据：串口 + `/ftl/media/` 结果。
- [ ] **RISK-IF-08** NAND 读超时/FAIL 归因
  - 触发：注入单页读失败（`fi_*` 或 `bbt fault` 后读）。
  - 期望：命令状态带 `TIMEOUT`/`FAIL_CACHE`，FTL 不进成功路径。

## 2. Journal / Checkpoint 失败

- [ ] **RISK-JN-01/04/07/08/10** Journal 擦/写失败与撕裂页
  - 触发：`fi_journal_fail_arm <pages>`，写满一个环形块后复跑。
  - 期望：`[JRN] erase kick failed` 有界；`resume: skip torn page`；不二次编程；回放不完整时回退 full rebuild。
  - 证据：串口 + 重启后 `boot type=` / `journal_entries=`。
- [ ] **RISK-CP-04/05/07** CP 提交饥饿 / abort 幂等 / flush 屏障
  - 触发：持续 H2testw 写 + 观察 `[CP] submit stalled`；`fl` 后立即断电（见 §3）。
  - 期望：`abort at phase=1`（不是 12）、1s 限流；断电重启 `find latest` + restore，非 full rebuild。
- [ ] **RISK-HOST-05/16** confirm park timeout
  - 触发：满盘 + GC 压力（H2testw 写至 free 低位），开 `--log-project info`。
  - 期望：无 `confirm park timeout`；若有，`parked=`/`fc=` 能定位。

## 3. 掉电（手动，当前无硬件）

> 步骤：H2testw 写到 `free` 低位（或指定 MB）→ **直接断板子供电**（不是拔 USB 线）→ 上电 → H2testw verify / `rd` 读回。

- [ ] **RISK-MD-06** 恢复路径丢映射（最高优先）
  - 步骤：`format` → 全量 `test`（制造多代写）→ `/speed/ftl_rw` 写 25 MiB → 第 4 次重启前断电。
  - 期望：无 `region … degraded` / `mapping lost` / `fallback`；`info` 的 `closed` 对账一致。
  - 证据：开机完整日志 + `info`。
- [ ] **RISK-BOOT-15/18/22/23** 启动恢复/格式化中断
  - 步骤：格式化过程中（BBT save 后 / erase 中 / initial CP 前）断电；再上电。
  - 期望：能继续格式化或 `restored`；不出现 `refuse to format`。
- [ ] **RISK-CP-07** SYNCHRONIZE CACHE 屏障
  - 步骤：H2testw 写完后安全弹出（触发 fsync）→ 断电 → 上电 verify。
  - 期望：0 错误、无尺寸缺口。
- [ ] **RISK-HOST-14** 拔线/中止后的门控环形等待
  - 步骤：写到接近满盘 → 拔 USB（不断电）→ 重插 → 读回/校验。
  - 期望：`drain stalled` 快速触发并自愈；无 `dirty=64` 持续刷屏。

## 4. 拔线 / 总线复位（手动或 `devcon`）

- [ ] **RISK-USBC-01/03、RISK-MSC-02** USB 中止/异步完成
  - 触发：读写过程中手动拔线；或 Windows `devcon disable/enable "USB\VID_0483&PID_5720*"`（逻辑断开）。
  - 期望：无死命令、无残留 CSW；重插后正常挂载；`pending_io` 不永久为真。
- [ ] **RISK-USB-10/13** 同 LBA 连写 / 两写槽时读
  - 触发：连续两次写同一 LBA（间隔 < FTL 完成）；两 chunk 在飞时立刻读。
  - 期望：两条命令都完成；读不报 I/O error。

## 5. 坏块 / 读失败 / 超时

- [ ] **RISK-BBT-01/11/15/17/19/20** BBT 幂等/持久化/擦失败
  - 触发：`bbt fault` + 重启；`format --verify`；写飞行中再标坏。
  - 期望：`bad_count`/`spare_count`/BBT 位一致；`old_bad/recovered/new_bad` 合理；无 livelock。
- [ ] **RISK-GC-01/07/10、RISK-MIG-02/07** victim 读失败与熔断
  - 触发：`bbt fault` 标坏 victim plane 后触发 GC；H2testw 写到 2/3。
  - 期望：3 次失败后换候选；连续 8 个 victim 失败熔断；不成批标坏、不活锁。
- [ ] **RISK-RR-02/04** RR 恢复/预测
  - 触发：坏页读（`fi_*` 或 `bbt fault`）+ 可恢复/不可恢复两种。
  - 期望：可恢复返回成功；单 plane 不可恢复整体 IO_ERROR；含 BAD plane 的 SB 被跳过。

## 6. 压力 / 背压 / 超时

- [ ] **RISK-GC-06/14、RISK-WL-07、RISK-CP-04** 满盘 GC/WL/CP
  - 触发：H2testw 写满 + `/speed`；观察 `[GC]`/`[WL]`/`[CP]`。
  - 期望：写速稳定、无 `submit stalled` 刷屏、无 `confirm park timeout`；WL 不在 host 写期间占引擎。
- [ ] **RISK-IF-09/10/12/13** 流式序列/门控
  - 触发：大顺序读写 + 中途中止；观察 `d4=`/`mig=`/`rrab=`。
  - 期望：放弃序列不饿死后台；链尾不占 `chain_owner`。

---

## 7. 目前缺硬件导致的覆盖缺口

| 场景 | 现状 | 自动化方案（后续） |
|---|---|---|
| 真实掉电 | 手动断板子供电，次数有限 | USB 继电器 / YKUSH / 程控电源 + 脚本 |
| 真实拔线 | 手动拔插 | 同上（切 VBUS） |
| 大批量掉电组合 | 无法批量 | 继电器 + H2testw/`/speed` 脚本循环 |
| 总线复位 | `devcon` 逻辑复位可部分替代 | USB 协议分析仪（Beagle/LUNA） |

> 主机侧 `dm-flakey` / `dm-dust` / `scsi_debug` 只测**主机块层**，不是本设备的 FTL，
> 不能作为上板证据（R4）。

---

## 8. 记录模板

每条执行后回填：

```text
RISK-XXX-NN | 镜像横幅（__DATE__ __TIME__）| 触发步骤 | 期望/实际 | 结论（通过/失败/未复现）| 证据（日志行）
```

结果写回 `known_risks.md` 对应条目的「历史/验证」行；通过的按 §0.4 判定是否 `已闭合`。

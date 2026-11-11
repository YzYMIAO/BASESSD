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
| ISS-UT-01 | 已实现（2026-10-24 上板；**计数已过时**） | FTL UT 按 `docs/design/ftl_ut_design.md` 重建：**63 个用例函数 / 参数化 85 次运行**，`test /ftl/` 全过（61.05 s，含 format）。⚠ 2026-11-11 复核实际注册 **76 个函数 / 100 次运行**（pure 53/61 + media 23/39）；63/85 是 v1.10 前的旧数，`ftl_ut_design.md` 待定稿时同步。2026-11-11 21:22:39 上板：**`test /ftl/` 100/100（pure 61 + media 39）** | `docs/design/ftl_ut_design.md`；`tests/unit/ftl_ut_common.*`、`test_ftl_pure.c`、`test_ftl_media.c` | 未落地项只剩需故障注入/空白片/白盒构造的部分：M-11 真介质注入、CP 版本不符、真空白片、NAND 写擦失败注入；见设计文档 §4.5/§7 |
| ISS-HIL-01 | **已实现并上板 15/15（2026-11-11 22:32:57）；剩余覆盖见 RISK-UT-34** | HIL 板端 UT：`tests/unit/test_hil.c/.h` + `g_hil_tests[]`（`make tests UT=HIL`）。覆盖：controller 状态机 / init（缺槽、topo 不符）/ `lba_range_ok` / submit 边界（buffer 不足、0 长度、越界、NULL、未知 opcode）/ `is_complete` / host queue tag/owner/used / 经 controller 的真实 READ/WRITE/FLUSH/TRIM / `write→flush→read` / FLUSH/TRIM 单飞行背压。**不 format**，读写固定用容量尾部 8-LBA 窗口；不 mock，复用主 boot 的 FTL/NAND。**未覆盖**：buffer NULL、TRIM 越界、跨重启、多 run 拆分/跨实例部分失败（单 die；需多 die 或 submit 注入）、USB 槽状态机（需 PC 主机侧矩阵）。`docs/README.md` 仍写「HIL UT 未实现」，口径待同步。 | `tests/unit/test_hil.c/.h`；`tests/unit/ut_main.c`（`/hil/`）；`src/hil/makefile.mk` / `src/fc/makefile.mk` / `makefile`（`UT=HIL`） | 上板记录：2026-11-11 22:14:53 `test /hil/` **12/12，0 skip**；22:25:23 镜像 15 例跑到 `write_flush_readback` 时用例体通过、teardown `timed_out`（只泵 `ftl_service`，未主动推进 CP/Journal）。22:32:57 镜像 `test /hil/ --log-visible debug`：**15/15，0 skip，8.99 ms**；`host_queue` 证据行已修正为 `owner=2`，`write_flush_readback` teardown 不再超时。剩余缺口（buffer NULL / TRIM 越界 / 跨重启 / 多 run / USB 槽）见 RISK-UT-34。USB 侧另跑主机矩阵（write→fsync 0x35/0x91、两写 lookahead→读、相同 LBA 连写、总线复位/拔出）。 |
| ISS-HIL-02 | 部分实现 | HIL L2 验收 24 例：基础读写 9、拆分/聚合/背压 4、跨重启 8、后台/水位观测 3。v0.58~v0.66 期间可能用 CLI 人工跑过，但**无逐条上板记录** | `../archive/hil_ut_design.md` Part B | 按 `usb_msc_verification.md` 的矩阵补记录；跨重启 8 例优先（`reboot_persist_*` 已修，需确认） |
| ISS-UT-02 | **已同步（2026-11-10，未上板）** | FC/NAND 测试源已按 R4 同步：`make tests UT=FC` / `UT=HIL` 通过（只证链接），新增 3 条用例（`fail_cw_attribution` / `read_column` / `process_read_ecc_max`，见 RISK-DP-29）。本条目原有报错（`test_speed` / `fc_test_common` / level1 / level3 / 数据路径头拆分 / 返回值与字段改名）已全部消除。FTL 测试源仍冻结，见 ISS-UT-04 | `tests/unit/` FC/NAND 测试源 | 上板跑 `/fc-nand/` 全套与 `/hil/`；测试源按 R4 由用户维护 |
| ISS-UT-04 | **已同步并上板（2026-11-11 21:22:39 `test /ftl/` 100/100）** | FTL 测试源已按当前生产 API 全量同步并重写语义漂移项；`make tests UT=FTL` 0 error / 0 warning。板端结果：`test /ftl/` **100/100**（2026-11-11 21:22:39 镜像，pure 61 + media 39，235.70 s，0 skip）。本轮收口覆盖 teardown 强约束、临时诊断清理、强转/格式清理、`cache_wr_lookup` 与 GC/WL flush。原 103 个编译错误（`test_ftl_pure.c` 78 / `test_ftl_media.c` 25）已消除；字段映射见 `known_risks.md` §2Y RISK-UT-15~21 | `tests/unit/test_ftl_pure.c`、`tests/unit/test_ftl_media.c`、`tests/unit/ftl_ut_common.c` | 后续只在改动 FTL API / allocator 语义 / 后台驱动方式时重跑；覆盖缺口见 ISS-UT-05 |
| ISS-UT-05 | **已闭合（2026-11-11 21:44:15 上板 95/95，0 skip）** | **FTL UT 覆盖缺口**：① v1.10 早回执 3 用例已补：`cache_early_ack_read_visible` / `cache_failc_undo_read_error` / `cache_early_ack_flush_barrier`；② CMT 基础用例 `l2p_cmt_write_readback`（dirty 安装 → 读命中 → flush 合并 → FAILC undo 清 dirty）；③ CMT 专项已补 4 条纯 RAM 用例：`l2p_cmt_capacity_evict` / `l2p_cmt_multi_region_merge` / `l2p_cmt_entries_fallback` / `l2p_cmt_line_invalidate`，另加媒体用例 `cache_cmt_writeback_retire`；④ `host_io_matrix` 改为 count 参数化、起点按种子随机；⑤ 现有 `md_dir_ckpt_write_*`、`cp_timer_reset` 已由重建用例覆盖 | `tests/unit/test_ftl_media.c`、`tests/unit/test_ftl_pure.c`；`docs/design/ftl_ut_design.md` §4.5/§7 | 上板记录：镜像横幅 `built Nov 11 2026 21:44:15`，`test /ftl/ --log-visible debug` → `95 of 95 (100%) tests successful, 0 skipped, 235.63 s`（seed `0x5a4f21e4`）。新增 CMT / 参数化矩阵用例均通过，本条闭合 |
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
| ISS-FTL-13 | 待决策 | **顺序流感知识别下的物理相邻选块**：写序列跨 block 已允许（`RISK-CACHE-20/21`），但读侧实测段长只到 **41 pair/start**（`cr=27435/105840/2583`）——分配器 `alc_free_find_block()` 按**动态 WL（最小擦除次数）** 选块，顺序写下去的下一个 block 大多不是物理 `block+1`，于是读 cache 序列在 block 边界就断（预取 100% 命中，128 pair 上限不是限制）。若要让读段长接近 128，需在**顺序流打开时**优先选物理相邻的空闲块；与 WL（最小 EC）直接冲突，需先定权衡（顺序块相邻 vs 磨损均衡），并评估 GC/OP 影响 | 2026-10-31 板端 `cr`/`pf`；`src/ftl/allocator.c` `alc_free_find_block`；`docs/design/ftl_revision_history.md` v1.06；`RISK-CACHE-20` | **先裁决**是否把物理相邻列为顺序流的选块偏好；实现前先测「现有块序里 `block+1` 的命中率」（可在 `alc_super_open` 记录），命中率低才值得改选块策略 |
| ISS-FTL-14 | 待决策 | **CP 让步策略**：持续 host 写期间 CP 只能在序列让出 die 的窗口提交（单 die，序列占用 cache 寄存器时 `ARDY=0`，程序命令发不进去）。可选：A. must-run 后台被拒超过 N ms 时请求一次 `break_requested`（下一窗口 END）；B. 序列结束后空出让位一轮。条件与后果（`phase=8` 长时间心跳、`cp` 推进慢但非死锁）见 `RISK-CACHE-23`，本条只记实现项 | `RISK-CACHE-23`；`src/ftl/ftl_op.c` `ftl_op_seq_gate`；2026-10-31 板端 CP 心跳 | **需用户裁决**：阈值（如每 1 s 最多让一次）、是否接受 `jf` 上升与写吞吐略降；上板判据 = 写期间 `cp` 序列持续前进且写速下降可接受 |
| ISS-FTL-15 | 未实现 | **消除 cache 写路径的 wbuf 回退（≈1.2 页/块）**：`jrn_seq_begin()` 在「顺序页页刷/擦在飞」时返回 false，写路径直接 `DECLINED` 回退 wbuf；该等待是瞬时的，等页刷完成后本可以 1 页窗口开序列并跨 block 续写。**前置取证**：给 cache 写路径的 decline 加**原因计数器**（现在只有总数 2,020），确认多数 decline 来自这一条再改语义 | `RISK-CACHE-21` 的 `wr−cw` 残差；`src/ftl/cache_batch.c` `ftl_cache_write_try`；`src/ftl/journal.c` `jrn_seq_begin` | ① 加 decline 归因计数（零行为变化）→ 上板一次取分布；② `ring_full` ⇒ 仍回退 wbuf，页刷/擦在飞 ⇒ 返回 `AGAIN` 等待（`bg_progress` 已计 Journal 在飞）；③ 上板判据：`wr−cw` 降到 ~0.1 页/块、`jf` 不上升 |
| ISS-FTL-18 | 部分实现 / 已首次上板执行 | **GC「搬移有效页」路径（`mv>0`）缺少自然触发场景**：所有上板运行 `mv=0`、`nop` 持续增长——victim 全是**空块**（删文件/覆盖写使旧页失效）。`mig_job_*` 的 READ/WRITE/Journal 搬移分支只在 victim 带有效 CW 时才走 ⇒ 该分支从未被上板压过 | 2026-10-31 板端 `info`/`[STAT]`（`mv=0 nop=1290`）；`RISK-GC-09`/`RISK-GC-14`；`src/ftl/migrate.c` | **2026-10-31 首次自然触发**：`[GC ] start reason=periodic trig=0x01 mode=off free=30` → `end jobs=11 moved=66 rec=11 noop=6 abort=0 elapsed=10804ms`，`free` 22→32；搬移路径本身完整执行、无 abort。⚠ 同一次运行随后出现数据丢失（RISK-ALLOC-10/L2P-09），搬移的**内容正确性**仍未单独验证。判据 = `mv>0`、搬移后 verify 0 错误、`dng=0` |
| ISS-FTL-17 | 未实现（需介质格式升级） | **持久化 block→SB 归属，让超级块成为分配/回收的唯一单位**（主流形态）：CP 块快照 word 只有 16 bit（`cp_block_word_pack(valid, st, temp, pool)`，`src/ftl/checkpoint.c:1350`），不含 `sb_id` ⇒ 恢复后 closed 条目只能是**单 plane**（`pfr_closed_sb_add`），GC 逐条回收会让 free 池 plane 失衡（RISK-ALLOC-09）。彻底修法 = 把 sb_id 持久化（扩快照 word 或在块 OOB/META 记录组号），恢复时成对重建；**代价**：介质布局版本 +1、首启格式化、整条掉电恢复路径重验（`RISK-CACHE-17` 等）。用户 2026-10-31 决定：本轮不做打折版（恢复期合成配对），等做完整版 | `src/ftl/checkpoint.c` `cp_block_word_pack`；`src/ftl/power_failure_recovery.c` `pfr_closed_sb_add`；`RISK-ALLOC-09`；`docs/design/ftl_design.md` §3.13.4 | **判据**：格式升级后恢复的 `closed` 条目全部成对（除非原始数据本身单 plane），GC 回收后 `fpmin` 与 `free` 同步增长；升级同时重跑两轮掉电回放 |
| ISS-FTL-16 | 部分实现（FTL 注入点 + UT **2026-10-31 已上板验证**：`cache_failc_abort` arm=1/2/last、`journal_write_fail_wal_stall` 全过）；CLI 钩子与 deferred-error 端到端未做 | **注入类验证的工程化**：① FTL 层注入 `include/common/fault_inject.h`（**测试镜像专属**：开关 `FC_NAND_TEST_ENABLE`，产品固件零代码；2026-11-01 起不再是 `NDEBUG` 门控，否则 release UT 的注入类用例失去判据）：`fi_cache_fail_arm`（cache program 完成上伪造 FAIL_CACHE，一次性、按页 LBA）、`fi_journal_fail_arm`（Journal 页写按失败结算）；UT 侧已落地 `cache_failc_abort` / `journal_write_fail_wal_stall`。② **未做**：`ftl_sync_*`（`RISK-BBT-02` / `RISK-MD-16`；⚠ 注入同步页读失败前要先裁决 `bbt_medium_has_ftl_data` 的判据 —— 它用普通同步读探「介质是否写过」，读不通会被当成空片 ⇒ 出厂扫描可能把写过的块全判坏）、命令池精确饥饿（`RISK-L2P-05` / `RISK-CP-04`）、CLI 注入钩子、deferred error 的**控制器/USB 端到端**（`RISK-USBC-01`；2026-11-01 已接线：`usb_msc_take_deferred_error()` 先问 FTL，UT 用例规格见 `ftl_ut_design.md` §4.5，**上板未做**）；③ 掉电回放 JSEG（`RISK-CACHE-17`）需物理断电/复位 | `RISK-CACHE-19` / `RISK-JN-03` / `RISK-L2P-09` / `RISK-USBC-01`；`docs/design/ftl_ut_design.md §4.5/§5` | 注入只在 debug 档编入；判据 = page0 失败时其后页均不确认、Journal 失败时区域写回被 WAL 闸门挡住、清注入后能恢复；测试 setup 必须 `fi_reset()`（状态是进程级） |

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
| ISS-USB-03 | **不实现（用户裁决 2026-11-01）** | WRITE(10) 的 **FUA** 位语义（只发 FUA 不发 SYNCHRONIZE CACHE） | `RISK-USB-05` | 不做（保持简单）：主机 fsync 走 SYNCHRONIZE CACHE；若抓包发现只发 FUA 的主机，再按 RISK-USB-05 的要点补（末 chunk 等一次 flush） |
| ISS-USB-04 | 未验证 | 多分区 / 第二分区（>128 MB）写满 + 校验 | 同上；历史 rev33 第二分区失败见 RISK-MIG-07 | v12 介质上重跑；与全盘 H2testw 分开记录 |

## 6. 平台 / 日志 / 故障捕获

| ID | 状态 | 条目 | 来源 | 下一步 / 判据 |
|---|---|---|---|---|
| ISS-PLAT-01 | 未实现 | 平台抽象 Stage 3：NAND 层裸 MMIO / `HAL_NAND_*` 收敛 | `platform_abstraction.md` §五 | 单独一轮；改后地址/时序类量必须上板 |
| ISS-PLAT-02 | 已关闭（2026-11-07） | 用户裁决删除 `stm32h7r3` 平台：`src/platform/stm32h7r3/`、`include/platform/stm32h7r3/`、`third_party/stm32h7r3/`、`third_party/stm32h7r3_vendor.mk`、两个 `stm32h7r3xx` 链接脚本全部移除；构建配置只保留 `stm32h743` | 同上 | 无；历史变更记录保留在 `docs/archive/` 与 `ftl_revision_history.md`，不回写 |
| ISS-PLAT-03 | 未修 | linker `.noncacheable_buffer` 输入段漏前导点，落点随启发式漂移 | `RISK-PLAT-01` | 单独一轮：`KEEP(*(.noncacheable_buffer))` + 上板回归 DMA |
| ISS-PLAT-04 | 已实现（Phase 0~3，2026-11-03，未上板） | **统一定时调度器 + 时间源纠正**：`btim`→`timer` 改名；一张表 `{fn, ctx, first_delay, period}`；watchdog / `[STAT]` / FTL 周期评估迁入；拆 `mig_handle` 的 advance/eval；`bg_scheduler` 降为预算子引擎。**系统时基 = ARM 核心 SysTick（ms + µs）；timer 调度器心跳 = TIM6；DWT 从产品路径删除** | `docs/design/scheduler_design.md` | Phase 0~3 已落地（改名 + 容量 64 + `add_at/set_period/task_count` + `[STAT]` 迁移 + `mig_eval` 注册）；delay/usart/watchdog/TinyUSB/FTL `time_ms` 统一到 SysTick；NAND/USB 的 DWT µs 计时改为 SysTick `LOAD/VAL`；TIM6 只驱动 timer 任务表；上板验证待做 |
| ISS-BG-01 | 已实现（2026-11-03，未上板） | **后台节拍 flag + token bucket**：CP T3 / L2P flush / BBT 起新 job 由 10 ms timer 任务置 flag；`bg_scheduler` 预算从“每轮配额”改为“每 10 ms 补 `grant_units` credit（IDLE/NORMAL 4、BUSY 1，上限 4 个 interval）”；must-run 与在飞推进不变 | `RISK-BG-10`；`scheduler_design.md` Phase 5 | 上板测 GC/WL 吞吐与 host 写延迟；按结果调 `grant_units` / `BG_TOKEN_INTERVAL_MS`；验证 flag 消费、T3 不延迟、BBT 仍能落盘 |
| ISS-TOP-01 | 已实现（Phase 1/2a/2b，2026-11-03，未上板） | **NAND 拓扑扫描 + FTL die 表**：板级 `nand_scan_targets` 在 armed 前经 `nand_select_target` 选通路、探测 (ch,ce) target，并用 78h 逐 LUN 探测，结果写 `g_nand_targets`；FTL `ftl_topology_build` 给实际存在的 LUN 分配紧凑 die 号，维护 `g_chcelun_table` / `g_die_table`。**Phase 2a：实例遍历改走 `for die < DIE_MAX` + `g_die_table`**。**Phase 2b：L2P 路由（`lba_2_*` / `l2p_global_*` / `ftl_op_route_get_region`）、容量（`FTL_LBA_MAX` / `FTL_IU_TOTAL`）、介质布局版本（`ftl_media_layout_version()`）、die 槽位（controller 只校验 present die）全部按运行时 `DIE_MAX`；数组按静态上限 `FTL_DIE_MAX`** | `RISK-TOP-01`；`nand_config.h` / `ftl_config.h` / `l2p_cache.h` / `metadata.h` / `ftl.c` / `main.c` / `controller.c` | 上板：单 die 介质不触发重格式化（版本仍 BASE）；H2testw 读写/坏块/吞吐；多 target 需扩展板级 `nand_select_target` 通路表；多 LUN 的逐 LUN 78h probe 需换多 LUN 器件再验 |
| ISS-LOG-01 | 未上板 | `logging_design.md` §5 的 9 项落地状态：`[STAT]` / `[MIG] done` 已在 2026-10-22 日志出现，其余（飞行记录器、打印预算逐项）未逐条确认 | `logging_design.md` §5/§6 | 按 §6 判据逐项打勾；未验的不要写成已验 |
| ISS-FAULT-01 | 未验证 | `fault_capture.md` 三项手工验证：人工 BusFault、断言失败 dump、跨复位读取 `.noinit` 记录 | `fault_capture.md` §6/§9 | 烧录→拔电→上电确认无记录；再逐个注入 |
| ISS-FAULT-02 | **已实现（未上板，2026-11-05）** | **PC 采样器（死循环 / 跑飞定位）**：SysTick naked 蹦床取被中断上下文的 `frame[6]`（PC），写入 `.noinit` 256 项活跃环；`pc_sample_init()` 在启动最早把上一轮活跃环**按时间顺序重排**后快照到 prev。CLI：`pc` 默认打上一轮**原始序列**（oldest→newest，4 个/行，最近 64 个；一行行重复即循环）、`pc all` 打全部 256、`pc top` 打命中次数排行（含 share）、`pc live` 看本轮、`pc clear` 归零、`pc rate <n>` 设每 n 个 tick 采一次（窗口 256×n ms，持久化到 `.noinit`，复位后沿用）。**边界**：原始序列是**采样顺序不是指令顺序**；紧凑循环（< rate ms 且整除）仍混叠，调大 rate 只会更差；中断关闭 / 更高优先级 ISR / SysTick 自身死循环采不到；`.noinit` 只跨软复位/看门狗复位，不跨 POR | `src/common/pc_sample.c` / `include/common/pc_sample.h`；`src/platform/stm32h743/sys_port.c` `SysTick_Handler` / `sys_tick_handler_c`；`src/main.c` `pc_sample_init` / boot hint；`src/hil/cli.c` `uart_cli_pc`；`src/platform/fault.c` / `fault_port.c`（异常现场 + 96 词栈扫描，与本模块互补） | **上板**：① 人为 `while(1)` → NRST → `pc` 原始序列命中该函数；`pc top`/`pc all` 一致；② `pc rate 4` 后窗口/span 与 run 行为符合预期；③ 回归 HAL tick/系统时间/串口（naked SysTick 零回归）；④ 掉电后 `pc` 无快照。反查必须用同一镜像的 `debug.map`/`debug.elf`（CLAUDE.md §3.3） |

## 7. 架构 / 契约残余

| ID | 状态 | 条目 | 来源 | 下一步 / 判据 |
|---|---|---|---|---|
| ISS-ARCH-01 | 已上板（2026-10-25） | 模块 handle 契约迁移（Phase 0~8）：正常读写/GC/CP/journal/BBT 已由 `/ftl/` 85/85 与 H2testw 415 MB 覆盖；**掉电/格式化中断/故障注入仍未单独验**（见 RISK-BOOT-22/BBT-18 等） | `../archive/ftl_module_contracts.md` §12；`RISK-ARCH-06`（已闭合，archive §2026-10-25） | 剩余只跟随 BOOT-22/BBT-18 等专项条目推进，不再单列契约回归 |
| ISS-ARCH-02 | 待决策 | 契约正文（目标设计）与已落地签名存在偏离（`jrn_handle` trigger、`wbuf_handle` 返回码、`pending_confirm_*` 入参） | `../archive/ftl_module_contracts.md` §12；`RISK-ARCH-10` | 定“权威契约以哪份为准”；定稿前不要按文档强行改签名 |
| ISS-CW-01 | 待决策 | `LUN_ADDR_WIDTH=0` 与多维地址构造的根因结论是否仍成立（cw 文档 §3.7 与 v0.73/v0.90 多维改动需对齐） | `../archive/cw_level_mapping_design.md` §3.7 | 与 ISS-ML-03 一起裁决 |
| ISS-CACHE-01 | 部分上板（2026-10-30 已改；host 4 页链 H2testw 415/423 MB 无错；跨轮次/失败页未验） | **cache read/program 序列依赖「全部 FTL 命令走 SQ 且连续入队」**：若 RR 等改走 HSQ，`fc_poll_fill` 先扫 HSQ，命令会插入 31h/3Fh（或 15h/10h）序列，破坏器件 cache 管线。FC 已有 `config.chain` + `chain_owner` 链锁可解 | `docs/design/ftl_design.md` §3.9.5 / ADR-17；`src/fc/internal/fc_internal.h` 的 `fc_sched_s`；`src/fc/fc.c` 的 `fc_poll_fill_queue` / `fc_poll_chain_length`；`src/ftl/cache_batch.c`（read/write 链标记）；`src/ftl/migrate.c`（GC/WL batch 链标记） | **方案 A 已落地**：cache read/program、GC/WL batch 都标成一条 FC chain（末条 `FC_CHAIN_END`，其余 `FC_CHAIN_MORE`），经 `ftl_op_submit_batch` 原子入队；FC 零功能改动。判据：① 上板跑 FC UT `test_chain_priority_lock`（8 条跨轮次链 + HSQ 回推）；② host 顺序读（4 条链上 45h/46h/47h 不交错，H2testw 校验通过）；③ GC batch `mb>0`。⚠ chain 写路径不处理 RAID stripe encode（`known_risks.md` RISK-IF-06） |
| ISS-FC-01 | 待接线 | `fc_desc_s.config` 保留为命令泛化/错误恢复上下文；当前无读者。只保留 config 并不能单独恢复整条命令（route/paa/meta/param/cmd_id 仍在 `fc_cmd_s`） | `include/fc/fc_command.h`；`src/fc/fc_pool.c` 建链写入 | 先定「错误恢复时 `fc_cmd_s` 是否仍存活」：存活则登记为校验副本；不存活则补齐完整恢复上下文。无消费者前不得删除或改名（H5/R11） |

## 8. 控制器观测 / 调试命令 backlog（2026-11-05 评估）

> E0 = `q/cp/jrn/stat` 四条只读命令**已实现、未上板**，其上板核对登记在 `known_risks.md` RISK-CLI-06，不在此重复。
> 本节只登记尚未实现的 P1/P2 观测项与评估结论。

| ID | 状态 | 条目 | 来源 | 下一步 / 判据 |
|---|---|---|---|---|
| ISS-OBS-01 | **部分实现（E1 已改、未上板 2026-11-05；E2/E3 未实现）** | **ECC 观测分三级**（对「ecc 需要 FC 跨层」的修正）：**E1 纯 CLI（已落地 `ecc`）** —— 扫 `bst_block_s.rr`（`last_ecc/rising_count/health`）+ `read_count`，出 last_ecc 直方图 / health 计数 / top-N / `read_blocks` 覆盖率；零新增状态，数据已由 host 读（`ftl_host_io_read_ok` → `rr_record_ecc`）与 GC 读（`migrate.c` → `fc_get_cmd_ecc_max`）维护。**E2 FTL 级累计（未做）** —— `ftl_stats_s` 加读次数、纠正 bit 总和/最大、UNC/CRC/MATA/RAID 事件计数；增量点是 `ftl_host_io_read_ok`（有 `cmd` + `ftl_cmd_block_ecc_list`）与读失败分支（有 `cmd` + `completion->status`），**不需要 FC 改动**，POR 清零。**E3 FC 级 per-CW（未做）** —— `fc_ecc_stats_t` + 完成路径增量 + `fc_get_ecc_stats()`，覆盖 CP/journal/BBT 同步读的「全 NAND 流量」口径。⚠ 不统计「read retry」：`read_recovery` 的 read-retry feature 地址 0x89 在本器件是保留值、自始空转（用户明确不处理，见 CLAUDE.md §四） | `src/hil/cli.c` `uart_cli_ecc`；`src/ftl/ftl.c` `ftl_host_io_read_ok` / `ftl_host_io_callback`；`src/ftl/migrate.c` `mig_read` 完成；`include/ftl/bst.h` `rr_ecc_track_s`；`src/ftl/read_recovery.c` `rr_record_ecc` / `rr_get_health`；`include/fc/fc_interface.h` `fc_get_cmd_ecc_max` | E1 上板核对并入 `RISK-CLI-06`；E2 等板端需要累计值再排；E3 不做。判据：E1 的 health 计数与 `rr_predictive_scan` 触发一致；E2 的 UNC/CRC 事件数与 `[FTL] ... cmd failed` 日志条数一致 |
| ISS-OBS-02 | **已实现（未上板，2026-11-05）** | **`wear` 磨损 / read-disturb 视图（已落地）**：扫 `erase_count` / `read_count` / `rr.{last_ecc,health}`，出 EC 直方图 + min/max/avg + top-N 冷热块 + `read_count` 分布 + WL 触发差值。纯 CLI，O(`FTL_BLOCK_TOTAL`)/die。**WL 判据同一漏斗**：新增 `wl_ec_spread_get()`（取 `max_free_ec` / `min_used_ec`），`wl_should_trigger` 改为调用它；CLI 不重算判据（C9）。⚠ `writes < WL_MIN_WRITES(1000)` 时启动门未过，`valid=0` 是预期 | `src/hil/cli.c` `uart_cli_wear`；`include/ftl/wear_leveling.h` `wl_ec_spread_get`；`src/ftl/wear_leveling.c`；`include/ftl/bst.h`；`include/ftl/allocator.h` `alc_free_get_max_erase` | 上板核对并入 `RISK-CLI-06`；判据：静态 WL 触发前后 `max_free−min_used` 与 `[WL ]` 日志一致 |
| ISS-OBS-03 | **已实现（未上板，2026-11-05）** | **`id` 配置 / 身份视图（已落地）**：编译期几何（CH/CE/LUN/PLANE/BLOCKS/PAGE/CW/IU）、容量（LBA / IU 总数 / 主机 MiB）、FTL 配置（pair / 块总数 / group / op / spare / CMT）、介质布局版本、timing mode、控制器状态。纯 CLI、无 NAND 访问。ONFI 参数页按计划不做（boot 已打印，`id --onfi` 需 idle 重读，未实现） | `src/hil/cli.c` `uart_cli_id`；`include/ftl/ftl_config.h`；`ftl_media_layout_version()`；`nand_get_timing_mode()` | 上板核对与 boot 横幅 / `info` 一致（并入 `RISK-CLI-06`） |
| ISS-OBS-04 | 待决策 | **`errlog` RAM 错误事件环**：`{t, layer, op, lba, pba, status}` × 32 ≈ 512 B；单漏斗 `errlog_record()`，挂 FTL 完成 / 失败路径；POR 丢失，**不是** NVMe Error Log 的实现（本仓自拟）。多层级直接写环会变成「不变量靠外部模块维护」（C3/H10），必须一个 API + 主循环 / 完成上下文（H21） | `RISK-USBC-01`；`ftl_host_io_callback`；`power_failure_recovery` | 等板端出现**间歇性失败**再排；先定记录点清单与是否持久化 |
| ISS-OBS-05 | 待决策 | **控制类命令**：`gc now/stop`、`reset`、`scan`。风险：与 `bg_scheduler` / `mig_handle` 单飞行冲突、host 在飞、长操作喂狗；`reset` 需「无在飞 IO」门。`scan` 可先用 `test` 的速度/读扫描替代 | `src/ftl/gc.c` / `bg_scheduler.c`；`RISK-CLI-01` | 等板端排障需要再排；任何控制命令都 debug-only + host-idle 门 |

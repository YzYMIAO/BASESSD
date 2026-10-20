# 已知残余风险登记

> **用途**：把「暂不修 / 待上板 / 待测试源同步 / 待决策」的风险固定下来。
> **复查 `metadata` / GC / migrate / journal / BBT / 掉电恢复 / L2P cache /
> Checkpoint / 磨损计数与 WL / Read Recovery / FTL↔FC 接口 / 完成路由与 op dispatch /
> 页打包写缓冲 wbuf / FTL service 与后台调度器 / 类型规则 / USB MSC 前先读本表**，避免重复分析、重复消耗。
>
> ⚠ **登记是硬约束（`CLAUDE.md` R11）**：缺陷与判定结论一经得出，当场登记在对应分节，
> 只在对话里说 = 未完成。说不出失败场景/后果的归 §七 S，不进本表。
>
> 状态词：`待决策`（需要用户裁决）· `待后续`（已登记，独立任务）·
> `待上板`（代码已改，板端未验）· `待测试源`（冻结 UT 待所有者同步）· `已修未上板` ·
> `已议`（分析后决定不改，理由见行内）· `信息`（只记录，无动作）。
>
> ⚠ 本表只登记**当前仍成立**的风险；闭合后移到文末「已闭合」并注明提交。

---

## 1. metadata 模块（提交 `992985c`）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-MD-01 | 已修未上板 | 日志块擦除持续失败（坏块）时，回收位保留并无限重试；轮转游标只保证不饿死其它块，没有坏块策略 | `md_reclaim_take` 轮转游标 | 若要收敛：连续 N 次擦失败后 `bbt_mark_bad` + 清位；需确认 N 与账本 | `src/ftl/metadata_job.c`（`ftl_metadata_callback` MD_JOB_RECLAIM 失败分支） |
| RISK-MD-02 | 已修未上板 | 压缩预算按 `md_compact_begin` 时的 `l2p_live[victim]` 计算；job 启动前已在飞的前台 region 写可能在扫描期间落回 victim ⇒ 超预算 ⇒ `slot_now` 返回 INVALID ⇒ 中止（**良性**：victim 不擦） | 中止路径安全；注释已写明 | 若要消除误报：扫描结束后重算预算，或预留 1 个槽位 | `src/ftl/metadata_job.c` `md_compact_begin` / `md_compact_write` |
| RISK-MD-03 | 待测试源 | `tests/unit/test_ftl.c` 仍引用已删字段/旧签名：`change_count`、`ring.plane`、`have_dynamic`、旧 `metadata_dir_commit_region` 签名、`super_block_t.valid/topo` 等；`make tests UT=FTL` 失败 | 未改测试源（R4） | 测试所有者更新后重新跑 `make tests UT=FTL` | `tests/unit/test_ftl.c` |
| RISK-MD-04 | 已修未上板 | H15 跨模块欠账已基本收敛：`src/ftl` 下再无手写 `FC_CW_STATUS_ERROR_MASK`（本轮收掉 read_recovery.c 3 处、bbt.c 1 处）；**剩 `src/fc/fc.c:809`** 一处，属 FC 层内部（取首个失败 desc 供日志归因，非成败判定） | 其余消费方一律走 `fc_cw_status_has_error()` | 若要彻底归零：给 FC 侧补一个「首个失败 desc」helper | `include/fc/fc_command.h`（helper）；`grep -rn FC_CW_STATUS_ERROR_MASK src` |
| RISK-MD-05 | 待上板 | 介质布局 v9（P0+P1 镜像 / Journal multi-plane 擦 / Wear 并入 CP）+ 本轮 metadata 重构均未上板 | 已有独立 memory 记录 | 见 memory `ftl-media-layout-v9-unverified.md` 的板端清单 | `docs/design/ftl_design.md §7.1` |
| RISK-MD-06 | 已修未上板（**未被本次冷启动覆盖**） | **回收旧日志块不等 Anchor 落盘**：普通 L2P flush 在 `commit_region` 里就登记回收候选，而新位置只在 RAM Directory；`MD_JOB_RECLAIM` 原来只检查 live/current/LOG 就擦。掉电后 on-media Anchor 仍指向**已擦块** ⇒ 重启 L2P region 读失败（修前上板 `region 0 load failed 3 times`）。 | 修法：① `MD_JOB_RECLAIM` 擦前要求 `dir->dirty == false`；脏则先走 `CP_ANCHOR_*` 写 Anchor（`anchor_for_reclaim`），成功后回 RECLAIM。② `dirty_gen/anchor_gen` 防止 Anchor 写入期间的新 commit 被误标为已覆盖。③ Anchor 落盘成功后才 `md_reclaim_mark_all`。 | 上板复现必须**释放一个元数据日志块**：只对 LBA 0 写两次 + `fl` 只是往同一个 64 页块追加 2 页，`anchor_seq` 不变（已实测 rev25：`anchor_seq=1 cp_seq=1`，**未触发**本路径）。需写满 64 页（64 个不同 region，LBA = r×512）再整体改写使其轮转；或加 debug 测试钩子。验证判据：`anchor_seq > cp_seq`、冷启动无 `[L2P] load failed` | `src/ftl/metadata_job.c`（IDLE/RECLAIM/CP_ANCHOR）；`src/ftl/metadata.c`（dirty_gen/anchor_gen）；rev25 实测：format→POR→`wr 0 1`+`fl`×2→POR，`boot type=2 journal_entries=2`、无 L2P 错误但 `anchor_seq` 未增 |
| RISK-MD-07 | 已修未上板（2026-10-20，方案 A） | **L2P 日志块碎片把物理空间吃光，容量未满即写失败**：2048 个 region × 2KB = **4 MiB 理想**；旧实现 append-only 日志从 Free Pool 动态取块 + 随机 region flush ⇒ 块高度稀疏，实测 `live_blks=1076` × 128 KB ≈ **134 MiB（33× 理想值）**。压缩只在**换新日志块且 `free_count ≤ 32`** 时请求，且一次只搬一个 victim ⇒ 大部分运行期不压缩，碎片持续增长。物理账（343 MiB 时）：数据 ≈2744 gblock + 日志 ≈1000 + OP/Spare/固定 ≈600 > 4096 ⇒ 无空闲页对，host 写最终 IO_ERROR（RISK-GC-09）。**之前 2/3 容量失败、GC 活锁都是同一空间问题的表现** | **已实现**：① 固定预留 L2P 日志区 64 gblock（8 MiB，容量已扣，见 RISK-CAP-02）；② 压缩触发改为**元数据水位**：`live_log_blocks > L2P_LOG_COMPACT_TRIGGER`（理想 32 + 25% = 40）；③ `md_compact_next` 在同一个新生块里连续搬**多个**稀疏 victim（直到空槽不够），一次退多块；④ `metadata_log_alloc/release` 管理固定区，擦除后置 NONE 不回 Free Pool；⑤ `info` 暴露 `live=` | **上板判据**：format → H2testw 全量写到 429 MiB 分区末端通过；`info` 的 `live=` 稳定在 ≤40 附近而非上千；reboot 后日志区无丢失 region（`rd`/`map` 抽查） | rev32 H2testw 343 MiB Code 34；rev30 `live_blks=1076` 且 `op=166/167 free=0`；`src/ftl/metadata.c` `metadata_log_*` / `metadata_dir_rotate_log_block`；`src/ftl/metadata_job.c` `md_compact_next` / `md_note_block_erased`；`include/ftl/metadata.h` L2P_LOG_* |

---

## 2. GC / migrate / journal（提交 `fdc7c7f`）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-GC-01 | 已修未上板 | 永久不可读 / META 页基址损坏的 victim 页会让 GC 反复 abort：数据安全，但容量不再增长 | 读错误 / `GC_DECODE_CORRUPT` ⇒ 中止，victim 不擦 | 重试上限 + 升级 RR / `bbt_mark_bad` / retire；需策略 | `src/ftl/migrate.c` `mig_complete_read`（`[待上板]` 分支） |
| RISK-GC-02 | 已修未上板 | 目的页写失败不标目的块 BAD：目的冷 SB 可能已有成功迁移页，整块标坏也错 | 写失败只中止 job，源数据仍有效 | 上层坏块策略：按连续失败页数/块判定，或隔离目的块剩余页 | `src/ftl/migrate.c` `mig_complete_write` |
| RISK-GC-03 | 已修未上板 | ~~TRIM 仍是 L2P-first、Journal-second，两步之间存在掉电窗口~~ | 改为 Journal-first（同 host 写路径）；见 RISK-HOST-04 的板端取证 | 上板：TRIM 后立即断电，重启确认该 LBA 不复活 | `src/ftl/ftl.c` `ftl_trim` |
| RISK-GC-04 | 已修未上板 | 部分 plane 已擦除后 job 中止，SB 仍留在 `closed_sb_list`；若被释放的 plane 又被重新分配并 Closed，旧条目理论上可能误认 | `mig_next_present_plane` 只接受 BST `CLOSED`，已降低概率 | 擦完一个 plane 后同步把该 plane 从 closed 条目摘除（需要 allocator 接口） | `src/ftl/migrate.c` `mig_next_present_plane` / `mig_finish` |
| RISK-MIG-01 | 已修未上板 | READ_SETTLE / WRITE_SETTLE 等 L2P region、FLUSH_JRN 等 Journal 时把 stall 计时清零 ⇒ 原为无上限等待 | `mig_wait` 用独立 `FTL_WAIT_TIMEOUT_MS`（50ms）deadline，超时 abort（victim 不擦） | 上板注入 region 加载永久卡住：`timed_out`/`timeout_phase` 上报且 job 中止 | `src/ftl/migrate.c` `mig_wait` |
| RISK-JN-01 | 已修未上板 | Journal 环形块擦除失败时重新挂起 `need_erase` 并无限重试；Journal 层没有 allocator，无法标 BAD | 不再写未擦块（拒绝继续） | Journal 块坏块策略：失败 N 次后由上层标记/换块 | `src/ftl/journal.c`（`ftl_journal_callback` JRN_ERASE 失败分支） |
| RISK-JN-02 | 已修未上板 | ~~`journal_flush_page` / `journal_flush_now` 返回 `void`：同步刷页失败会置 `write_fail_pending`，但调用者无法知道“屏障未达成”~~ | 改为 `ftl_status_t` / `bool`；drain 超时、擦失败、写失败均返回 `FTL_ERROR`，`ftl_flush` 记 `LOGW` | 上板：注入同步写/擦失败，`fl` 能报错且 pending 阻止 victim 擦 | `src/ftl/journal.c` `journal_flush_page` |
| RISK-PERF-01 | 待后续 | `gc_check_triggers` 与 `gc_select_victim` 各扫一次全表；`wl_should_trigger` 与 `wl_select_victim` 也各自扫描 | GC 10 ms / WL 5 s 节拍门兜成本 | 需要时把 `allocator_erase_stats_t` 从求值点透传（注意别把策略耦合进 mig API） | `src/ftl/gc.c` / `src/ftl/wear_leveling.c` |
| RISK-PERF-02 | 待后续 | GC/WL 目的页仍逐 plane 单条写，没有 multi-plane 命令 | 正确性无影响 | 命令形式优化，需 FC 侧支持 | `docs/design/ftl_design.md §7.2` |
| RISK-GC-05 | 待后续 | GC Bitmap（Layer 2）未实现：掉电后 GC 从块头重来 | 正确性由 F10 + 重试覆盖 | 性能优化项 | `docs/design/ftl_design.md §7.2` |
| RISK-RR-01 | 待后续 | `read_recovery.c` 的 `rr_step` 超 H11 NBNC（状态机 switch 较大）。~~`ftl_read_recovery_callback`~~ 已完成按 stage 拆 `rr_complete_*`（2026-10-19，见 §2H）。~~`RR_STAGE_ABORT` 死枚举~~ 已删除 | 与 GC/migrate 解耦；本轮只提取邻居页计算 | 独立重构任务（`rr_step` 按 stage 拆 handler）；范围已收窄为 `rr_step` 本体 | `src/ftl/read_recovery.c`；`include/ftl/read_recovery.h` |
| RISK-RR-02 | 已修未上板 | ~~恢复成功后仍以 `SSD_STATUS_IO_ERROR` 完成托管子操作；且 `recovered` 只是最后一个 plane 的结果~~ | 新增 `all_ok` 聚合位：job 起点置 1，任一 plane 失败或 abort 清零；`rr_finish` 按它报 OK/IO_ERROR | 上板：可恢复失败读应返回成功；单 plane 不可恢复整体报错 | `src/ftl/read_recovery.c` `rr_finish` / `rr_settle_plane` |
| RISK-RR-03 | 已修未上板 | ~~`rr_record_ecc` 健康状态可降级：CRITICAL/DYING 块在低 ECC 连续上升时被打回 WARNING~~ | 改为单调 max 升级 | 上板/UT 负例：CRITICAL 后低 ECC 上升不得回落 | `src/ftl/read_recovery.c` `rr_record_ecc` |
| RISK-RR-04 | 已修未上板 | ~~预测巡检返回的 CLOSED 块所在 SB 可能含 BAD plane ⇒ migrate 反复 abort 卡死~~ | `rr_predictive_scan` 命中候选后校验 SB，含 BAD plane 则跳过 | 上板：运行期标坏后预测路径仍能推进 | `src/ftl/read_recovery.c` `rr_predictive_scan` |
| RISK-GC-06 | 待上板 | 本轮行为变化未板验：T5 交叉相乘、WL 5 s 节拍、CLOSED plane 门控、`valid_count` 单块上界、`gc_decode_cw_lba` 三态 | 编译零告警 | 按 §2 板端清单回归（GC 并发覆盖写/TRIM、断电、WL、fault injection） | 提交 `fdc7c7f` |
| RISK-GC-07 | 已修未上板（2026-10-20） | **约 2/3 容量即触发 GC/CP 活锁**（用户确认非满盘）：H2testw 写到约 2/3 后 `free=0`、`op=166/167`；GC 选中 victim block 305（`vr=183`），page1 读失败 → `MIG abort kind=0 ... victim read failed`；随后 `[CP] submit stalled 55/59ms — abort phase=1` 与 GC 重选反复刷屏，`reclaimed=0`，无前进。**根因**：`mig_complete_read` 在**任一页读成功**时清零同一 gblock 的 `fail_count`（`migrate.c` 原 1245-1249）；坏页在第 1 页，每次重试第 0 页都先成功读一遍 ⇒ 计数永远是 1，`MIG_VICTIM_READ_FAIL_MAX=3` 不可达 ⇒ 坏块永不隔离、同一 victim 永久重选、空间永不回收。另：两个 plane 的块号不同却共用一个计数槽，交替失败也会互相清零 | 失败计数改为**按 plane 的源块**分别记（`fail_gblock[PLANE_NUM]` / `fail_count[PLANE_NUM]`）；**单页读成功不再清零**，只在该块整块迁移成功（`mig_finish`）或该 plane 标 BAD 时清；达 3 次仍走 `bbt_mark_bad` + `allocator_closed_drop_plane` | 上板复跑 H2testw（至少写满 2/3）：确认出现 `[MIG] victim read failed blk=... x3 — mark BAD`，随后 `reclaimed` 单调增长、`free` 回升、GC/CP 刷屏停止；`info`/`bbt` 中 `bad_count` 增加且重启后仍保留 | `src/ftl/migrate.c` `mig_complete_read` / `mig_start` / `mig_finish` / `migrate_init`；`include/ftl/migrate.h` `migrate_handle_t.fail_gblock[]`；`src/ftl/ftl.c` `migrate_fail_state_clean`；串口 rev30 日志（block 305 / page1 / `reclaimed=0`） |
| RISK-JN-03 | 待上板 | `write_fail_pending` + CP 清账逻辑未做故障注入：页写失败 / 擦失败 / CP 早于失败点三种时序未实测 | 静态路径已审查 | 注入 Journal 页写失败，确认 victim 未擦、CP 后恢复 | `src/ftl/journal.c` / `src/ftl/checkpoint.c` |
| RISK-JN-04 | 已修未上板 | ~~drain 超时后 `journal_flush_page` 仍同步刷页；在飞写命令仍拥有当前页地址 ⇒ 双重编程~~ | drain 失败 ⇒ `LOGE` + 置 pending + `FTL_ERROR`，**不再**同步刷；`journal_flush_now` 入口加 `job.phase == JRN_IDLE` 守卫 | 上板：注入命令丢失/超时（drain 超时），确认不发生同页二次编程、后续可恢复 | `src/ftl/journal.c` `journal_flush_page` / `journal_flush_now` |
| RISK-MIG-02 | 已修未上板（与 RISK-GC-07 同一修法，2026-10-20） | **predictive 迁移活锁**：`kind=2`（RR 慢死预测）反复选中同一 SB（plane0=453 / plane1=1964），page 读失败 → `mig_abort` → 立刻重选，约 1 Hz 刷屏；`MIG_VICTIM_READ_FAIL_MAX=3` 的标坏路径有时触发、有时不触发。**根因已确认**：`fail_count` 在**同一 gblock 的其它页读成功**时被清零（原 `migrate.c:1245-1249`），坏页之前的页每轮先成功一遍 ⇒ 计数到不了 3；两个 plane 的块号不同却共用同一计数槽，交替失败也会互相清零。后果：NAND/CPU 被空转风暴占满，host 格式化超时失败（Windows 报“无法完成格式化”） | 同 RISK-GC-07：计数改 per-plane 源块；仅在整块迁移成功或标 BAD 时清零 | 上板：predict 路径复现慢死块，确认 3 次后 `bbt_mark_bad` + `allocator_closed_drop_plane` 执行、刷屏停止；`bbt` 重启后 BAD 仍保留 | `src/ftl/migrate.c` `mig_complete_read` / `mig_abort`；`include/ftl/migrate.h` `migrate_handle_t.fail_gblock[]`；上板日志 rev25~rev28：`victim read failed blk=453/1964/300/307` 反复出现 |
| RISK-MIG-03 | 已修；dangling 路径已上板观测（rev32），TRIM/release 回绕未单独验（**原二级崩溃**） | `bbt_mark_bad()` 清 `valid_count` 后，L2P 仍可能指向该块（predictive 读失败标坏 / full metadata rebuild 漏认）。写确认与 TRIM 直接调 `ftl_block_sub_valid()` ⇒ debug 撞 `valid_count > 0U` 断言（上板 rev27：`dangling lba=9 old=00025811 g=300 st=BAD ...` 后 `assertion failed bst.h:285`），release 则 uint16 回绕成 65535，虚高计数污染 GC/WL | 修法：`bst.h` 新增 `ftl_block_sub_valid_if_live()`（BAD 或 count==0 跳过）；写确认、TRIM、migrate 源块减计数统一用它 | ✅ rev32 上板已观测：308/2493 被 predict 标 BAD 后，host 覆盖写 LBA 440–447 只打 `[FTL] dangling lba=… g=308/2493 st=BAD … newg=313/4089 nst=OPEN`，**无断言、无回绕**，新映射正常落到 OPEN 块。待补：TRIM 同一 LBA；release 档回绕负例 | `include/ftl/bst.h` `ftl_block_sub_valid_if_live`；`src/ftl/ftl.c`（写确认 / `ftl_trim`）；`src/ftl/migrate.c`；rev27 崩溃日志 + rev32 dangling 日志 |
| RISK-MIG-04 | 信息（2026-10-20 上板观测） | **format 后不久出现 `kind=2` predictive abort 是设计内行为**：`allocator_note_erase` 刻意保留 `rr.health`（慢死趋势跨擦除周期观察），`ftl_media_format` 不清 rr；carve 又把每 plane 前 50（Spare）+204（OP）个块先占走，首批 Free 块从 per-plane 约 300 起 ⇒ 格式化前的高 ECC 块正好落在这一段，predict 会最先读到并验证它们。读失败按 3 次标 BAD（本次 plane1=445→g2493、plane0=308），**不是新的活锁**（计数按 plane 源块累计后已收敛） | RISK-GC-07 / RISK-MIG-02 的 per-plane 计数修复；`bad=` 增长并隔离 | 观察 `bad=` 增长速率：只是 format 后清出旧坏块则正常；若持续快速增长则评估换芯片。若希望 format 后从零 rr 开始，可在 `ftl_media_format` 显式清 rr（当前保留是为趋势观察） | 板端 rev32 日志：`MIG abort kind=2 ... plane1 block=445` ×3 → `blk=2493 x3 — mark BAD`；随后 `plane0 block=308` ×3 → `blk=308 x3 — mark BAD`；`src/ftl/allocator.c` `allocator_note_erase`；`src/ftl/read_recovery.c` `rr_predictive_scan`；`src/ftl/power_failure_recovery.c` `ftl_boot_carve_pools` |
| RISK-MIG-05 | 待修（可观测性 / 吞吐） | **GC 读失败路径串口刷屏，且日志是阻塞式**：每次 abort 打 3 行（abort + victim plane0/1），同一坏块 3 次 = 9 行；每次重选还打 2 行 `[GC] victim ...`；115200 下 UART ≈11.5 KB/s（`uart_log_write` 阻塞）⇒ 单坏块约 0.1–0.3 s 纯串口开销，used 介质坏块多时既刷屏又拖慢 GC/USB。上板 rev32 实测：`kind=0` GC 在 779/2827/1220 等 victim 上反复 abort，日志把有效进展淹没，`reclaimed` 仍为 0 不易判断 | 运行期 `log level error` 可立即抑制 INFO/WARN，仅保留 `mark BAD` 与 `dangling`；无需重烧 | 下一版：① `mig_abort` 合并为**一行**（把 victim 各 plane 块号并入同一行）；② `[GC] victim ...` 降为 `LOGD`（debug 档才可见）；③ 可选：同一 (gblock, reason) 的 abort 每秒最多一条。上板确认日志量下降且诊断信息不丢 | 板端 rev32 串口；`src/ftl/migrate.c` `mig_abort` / `mig_gc_evaluate`；`src/common/log.c`（串口阻塞） |
| RISK-GC-09 | 待修（错误放大，2026-10-20 上板） | **近满盘时正常 GC 等待被过早判成 IO_ERROR**：`ftl_drain_group` 的无进展预算 `FTL_DRAIN_STALL_MAX=8` 单位是**主循环轮次**（不是 ms）；`wbuf_flush` 在无空闲页对时返 `FTL_AGAIN` 并登记紧急 GC，而 GC 完成一次回收要数百 ms（高 valid 比 + 坏块更久）⇒ 8 轮远远不够，组被置 `SSD_STATUS_IO_ERROR`，USB 回 CSW failed（H2testw：343 MiB 时 “Code 34”）。真正的根因是物理空间耗尽（RISK-MD-07）；本条是**错误放大**：本应表现为背压/延迟而不是立即 IO 错误 | `l2p_defer` 不计 stall（只覆盖 region 加载，不覆盖 GC 回收） | ① 预算改 ms deadline（与 MIG/CP 的 10/50ms 同族），或 `free < FREE_POOL_EMERGENCY` 且 GC 在飞时不累计 stall；② 更根本：host 写入口按 free 水位做**背压**（返回可重试状态让 HIL 等），而不是受理组后再判死；③ 上板：满盘连续写压测不出现 CSW failed | `src/ftl/ftl.c` `ftl_drain_group`；`include/ftl/ftl.h:43`；`src/ftl/wbuf.c` `wbuf_flush`；rev32 H2testw 日志（343 MiB / 188 KB/s） |

---

## 2A. BBT（Bad Block Table）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-BBT-01 | 待上板 | `bbt_mark_bad` 改为幂等 + `dirty_gen` 后，运行时标坏 / 重启持久化 / Spare 只减一次未实测 | 代码审查 + 编译零告警 | 上板：注入擦失败/RR 标坏，重启后 `bad_count`/`spare_count`/BBT 位一致；BBT 写飞行期间再标坏，下一次 save 必须包含新块 | `src/ftl/bbt.c` `bbt_mark_bad` / `bbt_submit_write` / `ftl_bbt_callback` |
| RISK-BBT-02 | 待上板 | `ftl_sync_read_column` 现在对 NAND 错误返回 `FTL_ERROR`；工厂扫描对“读失败页”的判坏行为未实测 | BBT 侧 `!= FTL_OK ⇒ 坏块` 保守路径 | 注入列读失败，确认判坏而不是误判好块；注意空片出厂标记与 NAND 失败的区别 | `src/ftl/ftl_fc_interface.c` `ftl_sync_read_column` |
| RISK-BBT-03 | 已修未上板 | ~~格式化先擦掉工厂标记再 save BBT~~ | 已改为两阶段 `bbt_format_erase`：① 置 `FORMAT_IN_PROGRESS` 并 save 失败即中止（不擦用户块）；② 擦除 BBT 池**之外**的块；③ 再 best-effort save 并入新 BAD。BBT 池旧 record 保留，新 record 以更高 sequence 胜出 | 上板：格式化过程中掉电注入（save 后 / erase 中 / CP marker 前），重启后 BBT 仍含工厂坏块图 | `src/ftl/bbt.c` `bbt_format_erase`；`src/ftl/power_failure_recovery.c` `ftl_media_format` |
| RISK-BBT-04 | 待后续 | BBT header **无布局版本字段**；只靠 `total_blocks` 做代理。块数相同但 bitmap 语义变化的格式演进无法识别 | `bbt_header_valid` 校验 `total_blocks == FTL_BLOCK_TOTAL` | 下次 BBT 格式变更时用 `reserved[]` 加版本（属于介质格式变更，需 bump `FTL_MEDIA_LAYOUT_VERSION`） | `include/ftl/bbt.h` `bbt_header_t` |
| RISK-BBT-05 | 已修未上板 | `bbt_medium_has_ftl_data` 只查固定元数据区；元数据被擦、用户区仍有数据时会被误判为空片并重扫（出厂标记已被覆盖 ⇒ 坏块图丢失） | **格式化决策已不再依赖它**：`checkpoint_probe_media_status` 先 raw 读 CP 池页区分空片/读死；本谓词只剩 `bbt_init` 内部决定“是否做出厂扫描” | 若要闭合：增加用户区抽样 / 独立介质标记点 | `src/ftl/bbt.c` `bbt_medium_has_ftl_data`；`src/ftl/checkpoint.c` `checkpoint_probe_media_status` |
| RISK-BBT-06 | 待后续 | `ftl_bbt_callback` 没有保存 in-flight `command_id`；擦完成与写完成若被重复投递，phase 可能误配 | `waiting==0` 的孤儿守卫只能挡 job 结束后的迟到完成 | **2026-10-19 核实**：FC 侧 `fc_complete_internal`（`fc.c:712-724`）**没有**「已 COMPLETED 就不重复入队」的守卫 ⇒ 是否重复完成完全取决于 NAND 层是否重复上报。当前 NAND 是单次轮询完成模型，无重复上报源 ⇒ 暂不修；换 NAND 引擎 / 加中断完成时**必须先补该守卫** | `src/ftl/bbt.c` `ftl_bbt_callback`；`src/fc/fc.c` `fc_complete_internal` |
| RISK-BBT-07 | 已修未上板 | ~~boot 期 CP 快照并集的 BAD 不补 Spare~~ | 新增 `allocator_spare_refill_from_free(alloc, plane)`：摘 Free → state 置 NONE → push Spare，直到 `FTL_SPARE_POOL_PER_PLANE` 或无同 plane 空闲块。三条启动路径（clean 快照 / clean full metadata rebuild / PF full metadata rebuild）建池后逐 plane 补池并打印补后计数 | 上板：制造 CP 快照含 BAD、Spare 不足的介质，确认补回 50/plane；Free 不足时自然下降 | `src/ftl/allocator.c`；`src/ftl/power_failure_recovery.c` `recovery_refill_spare_pools` |
| RISK-BBT-08 | 信息 | `bbt_scan_factory` 跳过 gblock 0，依赖“Block 0 出厂总是好块”的器件假设 | 器件手册（Micron AN1819）支持 | 换器件时复核 | `src/ftl/bbt.c` `bbt_scan_factory` |
| RISK-BBT-09 | 信息 | CLI `bbt --erase` / `bbt_print_bad_map(do_erase)` 会擦全片，且运行时 BST 不更新 | CLI 先 suspend 后台并提示断电 | 只作为专用体检工具；不要放进开机路径 | `src/ftl/bbt.c` `bbt_print_bad_map`；`src/controller/cli.c` |
| RISK-BBT-10 | 待测试源 | BBT UT 只覆盖 `mark_bad` / `spare_pool_refill` / inline bitmap；`bbt_load/save/kick/scan_factory/format_erase` 无用例；`test_ftl.c` 的“已知偏差 ④”注释在幂等修复后过期 | 未改测试源（R4） | 测试所有者补 NAND seam / 更新注释；新增 `dirty_gen`、`FORMAT_IN_PROGRESS` 中断恢复用例 | `tests/unit/test_ftl.c`；`docs/design/ftl_ut_munit.md §4.5` |

---

## 2B. 掉电恢复 / 启动路径（本轮审查，未提交）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-BOOT-01 | 待上板 | ~~F4 按 INVALID 行的 `region_id` 反查 CP bitmap ⇒ 全部别名到 region 0~~ | 新增 `l2p_cache_preload_region(cache, global_region, pba)`：由 CP bitmap **逐 bit** 找 INVALID 行、设 `region_id`、同步读、置 CLEAN/INVALID；F4 只对 resident CLEAN 行做 bitmap 失效 | 上板：region 0 与非 0 region 各预载一次，确认 `l2p_cache_find` 不返回别名、post-CP 映射可读 | `src/ftl/l2p_cache.c`；`src/ftl/power_failure_recovery.c` `ftl_boot_apply_region_bitmap` |
| RISK-BOOT-02 | 待上板 | ~~carve 先于 full metadata rebuild ⇒ 写过块同时是池成员与 CLOSED 条目 ⇒ 活数据可被复用~~ | 三条路径统一 **full metadata rebuild / 快照 → carve**；`ftl_boot_carve_pools` 只把 `state == NONE` 的块入池，其余只计入 `total_blocks` | 上板：预置数据在旧 carve 会选中的块上，走 full metadata rebuild fallback 后验证不在池 | `src/ftl/power_failure_recovery.c` |
| RISK-BOOT-03 | 待上板 | ~~CP 读失败被当作“需要格式化” ⇒ 一次瞬时读错整片擦除~~ | `checkpoint_probe_media_status` 区分 `CURRENT / NO_RECORD / VERSION_MISMATCH / IO_ERROR`；`ftl_media_check_format` 对 IO_ERROR 返回 `FTL_ERROR`，`main` 拒绝格式化并停止启动 | 上板：CP 读失败注入不擦片；版本不符正常格式化 | `src/ftl/checkpoint.c`；`src/main.c` |
| RISK-BOOT-04 | 待上板 | 两阶段格式化中途掉电后，重启不能判断“格式化未完成” | `FORMAT_IN_PROGRESS` 写入 `bbt_header_t.reserved[0]`，`bbt_load` 恢复；`checkpoint_probe_media_status` 见该标志返回 NO_RECORD ⇒ 继续格式化 | 上板：save 后 / erase 中 / CP marker 前各断电一次 | `src/ftl/bbt.c`；`src/ftl/checkpoint.c` |
| RISK-BOOT-05 | 待上板 | 快照/恢复路径的校验与计数改动未板验：restore Pass 0 全量校验（不再部分应用 BAD）、`recovery_materialize_pba` 校验 ch/ce/lun、`recovery_add_closed` 传播列表满、`valid_cw` 改 `uint32_t` | 编译零告警；失败路径回退 full metadata rebuild | 上板：快照中途注入非法 word（不留下部分 BAD）；journal 注入越界 PBA（回退 full metadata rebuild）；满盘重建确认 `valid_cw` 不截断 | `src/ftl/power_failure_recovery.c` |
| RISK-BOOT-06 | 已修未上板 | full metadata rebuild 把“第一个空白页”当写指针：中间若因编程失败留下 hole，hole 之后的页不会被 recount（valid_count 少算） | 写路径失败通常关闭 SB，hole+后续写概率低 | 若要闭合：full metadata rebuild 扫全块 page 0..N；代价是启动时间随块数线性增长 | `src/ftl/power_failure_recovery.c` `ftl_recovery_scan_write_pointer` |
| RISK-BOOT-07 | 已修未上板 | `ftl_recovery_page_is_blank` 读的是 raw USER 区（DATA 页已加扰），不是 META/CRC；依赖当前页面配置 | 当前 user 页走 DATA 加扰 ⇒ 空页全 FF 成立 | 若引入 `RANDOM_DISABLE` 且用户页前缀全 FF，判空会失效 ⇒ 改为读 META/CRC | `src/ftl/power_failure_recovery.c` `ftl_recovery_page_is_blank` |
| RISK-BOOT-08 | 已修未上板 | `journal_scan_latest` / `journal_replay` 的读失败不向上传播：坏页被当作“无更多条目”，可能少回放 | **读失败现在有 `LOGW`**（scan 汇总一条 / replay 逐页）；回放按 `(page_seq, entry_idx)` 单调过滤 | 统一到 journal 的读错误策略（重试/隔离），属跨模块改动 | `src/ftl/journal.c` |
| RISK-BOOT-09 | 已修未上板 | `metadata_dir_load` 失败时静默回退到新建 Directory：旧 region 映射可能整体丢失，但启动继续 | 失败后有 LOGW（无返回码） | 决定是否把“Directory 读失败”升级为启动失败 / 重扫 | `src/ftl/metadata.c` `metadata_dir_load` |
| RISK-BOOT-10 | 已修未上板 | ~~`journal_resume` 恢复写态时忽略跨块擦除结果：擦失败后首次写会落在未擦页~~ | 擦失败 ⇒ `LOGE` + 重新挂起 `job.need_erase`（在 job 复位之后写入）；`journal_handle` / `flush_now` 先补擦，`journal_freeze` 在 `need_erase != 0` 时拒绝写 | 上板：注入 resume 跨块擦失败，首次写前必须补擦成功 | `src/ftl/journal.c` `journal_resume` |
| RISK-BOOT-11 | 已修未上板 | 快照 probe 的 `l2p_cache_read_sync` 失败只记 LOGW 并计入 `anomalies`，`blocks_restored` 仍为 1 ⇒ valid_count 少算 | 读失败日志可见；不触发 full metadata rebuild | 若读失败频繁，升级为 full metadata rebuild 或按块隔离 | `src/ftl/power_failure_recovery.c` `ftl_recovery_probe_blocks` |
| RISK-BOOT-13 | 待定位（归属未确认；POR 后未复现） | `format` 完成后未按提示 `POWER-CYCLE NOW` 就下发 `wr`：曾出现 CLI 请求永久 `busy`；同一会话的 `.noinit` 记录显示 **HardFault** `PC=0x2400038c LR=0x24000b09 CFSR=IACCVIOL HFSR=FORCED`（按 rev25 解析落在 CLI 16 KB `g_wr_buf` 内，属执行流跳进非可执行 RAM）。**但一次 POR 后 `previous crash` 行消失，且 format→POR→wr/fl 正常** ⇒ 该记录至少可能是 rev24/旧会话的陈旧记录，不能归因于本轮 FTL 改动。另：format 后未重启的 `wr` 本次返回 `status=-3`，未再挂死 | `format` 已打印 `POWER-CYCLE NOW`；未做运行期拒绝 | ① 继续在 rev25 上做正常读写/断电回归；② 若 `previous crash` 再次出现：立即 `crash` + 记录命令序列，按**当次镜像**符号表定位；③ 考虑让 format 后置 FTL 为不可提交状态直到重启 | rev25 日志 2026-10-20：首次 `.noinit` 有 HardFault；**POR 后无该行**；`arm-none-eabi-nm` `g_wr_buf=0x240002dc` |
| RISK-BOOT-14 | 已改未上板（**性能优化**） | full metadata rebuild 的空闲块扫描原来逐页扫满 64 页（4096 块 × 64 ≈ 26 万次页读），是 rebuild 慢的主因。改为 **page 0 的 4 个 CW META 全 FF ⇒ 整块空白**，空闲块从 64 次读降到 1 次（本板约省 22 万次）。**假设**：写模型顺序追加，page 0 编程失败会关闭 SB、不会继续写后续页；若将来开放 Open SB 续写/失败后续写，该假设失效，hole 可能被漏掉 | 代码注释 + `ftl_design.md §3.1.5` 写明假设；当前写路径失败即关闭 SB | 上板验证：clean/PF/fresh 三条路径；部分写块 `partial`/`valid_cw` 与优化前一致；故意构造 page 0 空白但后续页有数据的异常块（如注入）应能暴露。若引入续写，恢复全块扫描或补读 page 63 | `src/ftl/power_failure_recovery.c` `ftl_recovery_scan_write_pointer`；`docs/design/ftl_design.md §3.1.5` 第 6 条 |
| RISK-BOOT-15 | 已修未上板（2026-10-20，L0） | **full rebuild → 下一次 CP 快照失效 → 每次开机都 rebuild 的循环**：`ftl_rebuild_closed_sb_list` 把**动态元数据 LOG 块**（`ftl_is_metadata_block(dir,...)` 为真）直接 `continue`，`ftl_boot_carve_pools` 也跳过它们 ⇒ 这些块的 BST state 停在 **NONE 且不属于任何 allocator 池**（归 metadata_dir 所有）。`checkpoint_block_snapshot` 只看 BST + OP/Spare 池，把它们写成 `NONE + no pool tag`；下次开机 `ftl_recovery_restore_blocks` Pass 0 见到该 word 即拒绝整份快照（`[BLK] g=303 NONE with no pool tag`）⇒ 又走 full rebuild，再次留下同样状态。后果：每次冷启动都全盘扫描（空片 ~0.5s；312 MiB 介质实测 **2–4 分钟**），NAND 读放大与启动时间持续劣化；**数据不丢**（fallback 从 META/journal 重建），USB 在 rebuild 完成前不会枚举 | **L0 已实现（rev32）**：① `ftl_rebuild_closed_sb_list` 对非固定、`ftl_is_metadata_block(dir,...)` 为真的块显式置 `LOG`（清 valid/temp/sb）；② `checkpoint_block_snapshot(alloc, md_dir, words)` 以 Directory 为准：`metadata_live_count>0` 且非 BAD 的块强制编码为 `LOG/no-pool`（生产者兜底，`md_dir==NULL` 时保持旧行为）；③ `restore_blocks` Pass 0 对 `metadata_live_count(dir,g)>0` 的 `NONE/no-pool` 放行，Pass 2 按 Directory 修正为 LOG（**自愈已污染的旧快照**，不再为一处旧 word 全盘 rebuild）；④ Pass 2 只对“非 LOG 且非 NONE”的真冲突打 LOGW，避免自愈时上百条刷屏。未做 L1（按块粒度局部重建）——用户决定先用镜像兜底 | **上板判据**：① 现有已污染介质上电应直接出现 `[BLK] restore closed=… log=…`（不再 `fallback to full metadata rebuild`）；② rebuild 一次 → 写数据 → 触发 CP → 断电重上电，第二次启动必须走快照恢复；③ 断电恢复（PF）路径同样不再退化为全盘 rebuild。若仍出现 `NONE with no pool tag`，说明该块不是 live LOG 块，需按 L1 处理 | 板端 rev31 日志（g=303）；`src/ftl/power_failure_recovery.c` `ftl_rebuild_closed_sb_list`（LOG 落账）、`ftl_recovery_restore_blocks` Pass 0/Pass 2（放行+修正）、`ftl_boot_carve_pools`（LOG 块跳过）；`src/ftl/checkpoint.c` `checkpoint_block_snapshot`（Directory 权威编码）；`docs/design/ftl_design.md §3.1.5` 步骤 4/6 与启动时间表 |

---

## 2C. L2P cache（本轮审查）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-L2P-01 | 已修未上板 | region 页加载失败时行被置 CLEAN + entries 全 `NAND_PBA_INVALID`（= 该 region 全部未映射），host 读到零填充；映射在行被驱逐前**静默不可用**，且不升级到 read recovery | 避免「坏 region 每次访问都重读 + host 永久停在 FTL_AGAIN」的活锁；行为与「从未写过」一致 | 接入 RR 后：失败改为 RR / 标块坏，或按 region 限次重试后再降级 | `src/ftl/l2p_cache.c` `ftl_l2p_callback`（LOADING 失败分支） |
| RISK-L2P-02 | 已修未上板 | ~~先取槽后建链，命令池空重试浪费 L2P 日志槽~~ | `flush_kick` 在 `append_slot` 前用 `fc_get_resource_stats()` 预检命令/描述符/buffer/SQ/HSQ；不足即返 `FTL_AGAIN`，不消耗槽位 | 上板：命令池高压下长时写，日志块换块频率不异常 | `src/ftl/l2p_cache.c` `l2p_flush_resources_available` |
| RISK-L2P-03 | 待上板 | 本轮行为变化未板验：`flush_kick` 背压改报 `FTL_AGAIN`、写回前资源预检、CLOCK 引用位置 1、加载失败降级为全未映射 | 编译零告警 | 上板：命令池压力下 `fl`/Checkpoint 不中止且不烧日志槽；热点 region 不被长期钉住；坏 region 读不活锁 | `src/ftl/l2p_cache.c` |
| RISK-L2P-04 | 已修未上板 | 上板复位后启动出现 `[L2P] region 0 load failed 3 times — degrade to unmapped`。降级路径把行置 CLEAN + entries 全 INVALID 并让 `l2p_cache_read_sync` 返回 **OK** ⇒ probe 的 `l2p_read_errors` 保持 0，启动仍报 `restored`。**后果**：若该 region 有 journal 未覆盖的旧映射，会静默丢失且不触发 full metadata rebuild。| 修法：① `l2p_cache_line_t` 增 `degraded` 标志（与 `fresh/load_fail_count` 共用对齐填充，行尺寸不变）；② 新增 `l2p_cache_region_degraded()`；③ boot probe 读成功后检查该标志，计入 `l2p_read_errors` → 回退 full metadata rebuild；④ journal replay 在降级基线上应用条目时同样置 `block_ctx->ok=0`。 | 上板验证：正常启动不得误报（`restored` 仍正常）；故障注入（人工破坏一个 L2P region 页）后启动应出现 `L2P region N degraded — snapshot incomplete` 并回退 full metadata rebuild，而不是 `restored` | `src/ftl/l2p_cache.c` / `include/ftl/l2p_cache.h`；`src/ftl/power_failure_recovery.c` `ftl_recovery_probe_blocks`；`src/ftl/journal.c` `journal_replay` |

---

## 2D. Checkpoint（本轮审查）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-CP-01 | 已修未上板 | ~~journal 预擦失败仍记 `pre_erased_block` ⇒ journal 回切时跳过擦除、写进未擦块~~ | 失败改 `journal_truncate_commit(JOURNAL_BLOCK_NONE)`：只复位水位、不记账；回切时由 journal 的 `need_erase` 重试，下一次 CP 的 plan 也会再报 NEEDS_ERASE | 上板：擦除失败注入，确认不跳过擦除、CP 不因 T1 反复触发 | `src/ftl/checkpoint.c` `ftl_checkpoint_callback`（CP_TRUNC_ERASE） |
| RISK-CP-02 | 已修未上板 | ~~`ftl_checkpoint_callback` 无 `waiting` 守卫：重复/迟到完成会按当前 phase 再走一遍~~ | 入口 `ASSERT(waiting)` + release 拒绝（LOGE + release + return） | FC 重复完成注入 / 上板回归 | `src/ftl/checkpoint.c` `ftl_checkpoint_callback` |
| RISK-CP-03 | 待上板 | 本轮状态机改动未板验：提交状态传播（FTL_ERROR 立即中止）、`find_latest` 失败复位游标、水位公式精确到条目 | 编译零告警 | 正常 CP、各 phase 掉电、journal 回切、85%/95% 触发点 | `src/ftl/checkpoint.c` |
| RISK-CP-04 | 待定位（上板观测 2026-10-20） | **持续 host 写流下 CP 提交饥饿**：H2testw 期间反复出现 `[CP] submit stalled 55/59ms — abort phase=1`。CP 用 `ftl_op_submit` 申请 FC 命令，host 写可占满 `FC_COMMAND_POOL_DEPTH=64`（`FTL_PENDING_MAX=32` 个写各占 2 条命令）⇒ CP 连续 50 ms 申请不到槽位即中止重试。后果：checkpoint 长期不推进、journal 环增长；环满后 host 写被 Backpressure 拒绝，表现为写停顿（无静默损坏）。当前无法区分它是 GC 活锁的**症状**（GC 空转占满命令池）还是独立缺陷 | 中止后可重试；journal 环满是安全背压，不会越界写 | ① 先修 RISK-GC-07 后复跑 H2testw，观察 CP 是否仍持续 abort；② 若仍复现：给元数据/CP 预留命令槽，或让 CP 的 submit deadline 长于 host 写突发；③ 上板确认 journal 水位与 CP 成功率 | `src/ftl/checkpoint.c` `cp_stall` / `cp_submit_result`；`src/ftl/ftl_op.c` `ftl_op_submit`；`include/fc/fc.h` `FC_COMMAND_POOL_DEPTH`；串口 rev30 日志 |

---

## 2E. 磨损计数 / 静态 WL（本轮审查）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-WL-01 | 已修未上板 | ~~WL 擦完 victim 用 `MIG_POST_RETIRE`：RETIRED 仍计入 erase 统计的 min/max ⇒ 低擦除块永久压低 min + 每次 WL 退役一块 ⇒ 容量持续流失~~ | 改 `MIG_POST_FREE`：擦完归还 Free Pool，动态 WL 优先选中它写入，擦除计数回升 | 上板：触发 WL 后 Free Pool/容量不下降，低擦除块被重新分配 | `src/ftl/migrate.c` `mig_begin_wl` |
| RISK-WL-02 | 已修未上板 | ~~`allocator_note_erase` 不重置 `read_count`：read disturb 跨擦除周期累计 ⇒ 正常块越过 1000 被预测路径退役~~ | 擦除成功清 `read_count`；`rr.health / last_ecc / rising_count` 保留（慢死趋势跨周期） | 长时 soak：正常块不被 `rr_predictive_scan` 误退役 | `src/ftl/allocator.c` `allocator_note_erase` |
| RISK-WL-03 | 已修未上板 | ~~Closed SB 的某个 plane 在标 BAD 后仍被 WL 选中：`plane_blocks` 含 BAD 块 ⇒ migrate 读失败 abort，且每轮重选 ⇒ WL 永久卡死~~ | `wl_select_victim` 预检：任一 present plane 的块 BAD ⇒ 跳过该 SB | 上板：运行期标坏后 WL 仍能前进 | `src/ftl/wear_leveling.c` `wl_select_victim` |
| RISK-WL-04 | 待上板 | 本轮行为变化未板验：WL 收尾 FREE、read_count 复位、BAD plane 过滤、磨损表 `record_pages` 校验 | 编译零告警 | 正常 WL/GC、断电、长时 soak | `src/ftl/migrate.c` / `allocator.c` / `wear_leveling.c` |
| RISK-WL-05 | 待后续 | `wl_should_trigger` 与 `wl_select_victim` 各做一次全表扫描（RISK-PERF-01 的一部分）；RETIRED 块仍计入 erase stats | 5 s 节拍门 | 需要时把 `allocator_erase_stats_t` 从触发点透传 | `src/ftl/wear_leveling.c` |
| RISK-WL-06 | 信息 | 磨损快照**逐页应用**：中途失败时前面的页已回填，其余保持默认（软状态） | Header 字段校验 + 每页 CRC + 日志 | 若要求整条原子恢复：32 KB 暂存或两遍读 | `src/ftl/checkpoint.c` `checkpoint_load_wear_table` |

---

## 2F. FTL↔FC 接口（本轮审查）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-IF-01 | 已修未上板 | ~~`ftl_cmd_write_data_cw` 用 `1U << cw` 判位（plane 盲）：flat 位图里 plane 1 的 bits 4..7 在 bits 0..3 全 0 ⇒ GC/WL 迁移 plane 1 的每个 CW 被填 0xFF~~ | 从 `descriptor->paa` 解 plane，判位改用 `ftl_cw_bit(plane, cw)` | 上板：迁移 plane 1 数据后逐 CW 读回校验 | `src/ftl/ftl_fc_interface.c` `ftl_cmd_write_data_cw` |
| RISK-IF-02 | 已修未上板 | ~~`ftl_sync_submit_wait` 超时摘表后命令迟到完成 ⇒ dispatch `take` 得 NULL ctx，`ftl_sync_wait_callback` 解引用 NULL + 命令槽位泄漏~~ | `ftl_sync_wait_callback` 对 `ctx == NULL`：LOGE + `fc_command_release` | 上板/故障注入：超时后迟到完成不 HardFault、池不泄漏 | `src/ftl/ftl_op.c` |
| RISK-IF-03 | 已修未上板 | ~~同步原语忽略数据搬运返回值（写失败仍编程、读失败仍 OK）；raw 读缺 CW 静默跳过；短链读返回 OK 且尾部未初始化~~ | 三处均检查返回值并返回 `FTL_ERROR` | 上板/故障注入 | `src/ftl/ftl_fc_interface.c` |
| RISK-IF-04 | 待上板 | 本轮行为变化未板验：plane 1 迁移数据、wbuf 稀疏页、越段位图 release 拒绝、迟到完成路径 | 编译零告警（release 两条 unused 已消失） | 按 §5 板端清单回归 | `src/ftl/ftl_fc_interface.c` / `ftl_op.c` |
| RISK-IF-05 | 待上板 | 旁表容量 `COMPLETION_TABLE_SIZE` 32 → `FC_COMMAND_POOL_DEPTH`（64）：**修复**原容量小于真实上界（表按 `command_id` 键，上界 = 在飞命令数 ≤ 池深）⇒ 原「满则 `FTL_ERROR`」路径实为可达，且 `bbt.c` 会把它当永久失败吞掉 BBT save | `src/ftl/completion_table.c` 加 `_Static_assert(COMPLETION_TABLE_SIZE >= FC_COMMAND_POOL_DEPTH)`；RAM 代价 +256 B（8 B/项 × 32 项） | 上板：观察 `[FTL]` 心跳无异常；`completion_table_count` 上界变 64 | `include/ftl/completion_table.h` / `src/ftl/completion_table.c` |

---

## 2G. Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`，本轮审查，未提交）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-HOST-01 | 已修未上板 | ~~host 读成败判据只看 `FC_CW_STATUS_NAND_FAIL`：ECC 不可纠只置 `UNC`、页内 META 不符只置 `MATA_ERROR`，两者的 `NAND_FAIL` 恒清 ⇒ 加扰垃圾按 `SSD_STATUS_OK` 交给 host~~ | 改为 `fc_cw_status_has_error(completion->status)`（全集）；失败分支本就走「托管给 RR」，无需新增逻辑 | **上板取证**：造一次 ECC 不可纠 / MATA 读（写某 LBA → 擦掉其映射所指块 → 读该 LBA），确认 host 得 `IO_ERROR` 且 `[RR]` 心跳可见 | `src/ftl/ftl.c` `ftl_host_io_callback`；判据 `include/fc/fc_command.h` `fc_cw_status_has_error` |
| RISK-HOST-02 | 已修未上板 | host 读成功路径的 `read_count++` / `rr_record_ecc` 只记 `ftl_cmd_pba(cmd)` 的**首个 desc** 所在 gblock；一个子操作可覆盖 2 个 plane，Policy B 下两 plane 是**独立块** ⇒ 第二块的 read-disturb / ECC 趋势漏记 | 无数据损失，只影响慢死预测准确度（搬移阈值要 2× 读数才到） | 需 FTL 侧逐 plane PBA 查询（现只有 `ftl_cmd_pba`；逐 plane 要摸 FC desc 链，属 H13 跨层）⇒ 待后续 | `src/ftl/ftl.c` `ftl_host_io_callback` |
| RISK-HOST-03 | 已修未上板 | ~~`ftl_cmd_read_data` 返回值被丢弃：desc 链比请求短时尾部不写，未初始化缓冲被当数据交给 host~~ | 检查返回值：失败 ⇒ `LOGE` + `SSD_STATUS_IO_ERROR`，并跳过该次的 ECC/read_count 记账 | 上板/故障注入：短链读必须报错 | `src/ftl/ftl.c` `ftl_host_io_callback` |
| RISK-HOST-04 | 已修未上板 | ~~`ftl_trim` 是 L2P-first / Journal-second，两步之间掉电 ⇒ 回放重放更早的写条目把已 TRIM 的 LBA 复活~~ | 改为 Journal-first（`journal_append` 早于 `l2p_cache_write_sync`），与 host 写路径 `ftl_confirm_write` 同序 | 上板：TRIM 后立即断电，重启确认该 LBA 不复活 | `src/ftl/ftl.c` `ftl_trim` |

---

## 2H. 完成路由与 op dispatch（`completion_table.c` + `ftl_op.c`，本轮审查）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-CR-01 | 已修未上板 | ~~`completion_table_add` 只找首个空槽、**不按 `command_id` 去重** ⇒ 旁表可存在两条同 id 项；而 `completion_table_take` 取**首个**匹配 ⇒ 泄漏之后 id 被复用，服务函数拿到**别人的 ctx**（可能是早已失效的 `checkpoint_handle_t *` / 栈上 `ftl_sync_waiter_t *`），本命令自己的表项永久泄漏~~。可达路径：`fc.c` CQ 满强制置 COMPLETED（注释自陈「CQ entry 丢失，FTL 无法感知」）⇒ 表项泄漏 → `fc_command_alloc` 复用 id → 再次 add | `completion_table_add` 改两遍扫：先全表查 `cmd_id`，命中则**原地覆盖 ctx** + `LOGW`；未命中才取空槽 ⇒ 「同一 cmd_id 至多一条表项」收进**唯一闸门**（H10） | **不可直接构造**（需 CQ 满 / 丢完成）⇒ 静态收口 + 新增 `LOGW` 使一旦发生即可见；随常规回归上板，**不宣称已取证** | `src/ftl/completion_table.c` `completion_table_add` |
| RISK-CR-02 | 待上板 | 坐标类改动未板验：`completion_table_route_region` 的 dim→(ch,ce,lun) 分解改走 `ftl_dim_ch/ce/lun`（与 `lba_route_*` 收敛为同一套公式）。坐标属「编译期无声的三类量」 | 分解式与原手写式逐字同值；新增三个 `static inline` 纯函数（H42） | **必须上板**：跑一遍读写 + `map <lba>` | `include/ftl/l2p_cache.h` / `src/ftl/completion_table.c` |
| RISK-CR-03 | 待上板 | 9 个服务函数按 phase 拆 handler（纯代码搬移）+ 注释收敛（H37）；`ftl_checkpoint_callback` 额外把 `release + waiting=0` 提到 switch 之前（8 个 case 同构，`cp_abort` 不碰 `waiting`） | 判据 / 顺序 / 日志文案逐字不变；`switch` 的 `default` 副作用留在 dispatcher（§四） | 随常规回归上板（行为应逐字等价） | `src/ftl/{ftl,journal,bbt,read_recovery,checkpoint,metadata_job}.c` |

---

## 2I. 页打包写缓冲 wbuf（`wbuf.c` + `wbuf.h`，本轮审查）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-WB-01 | 已修未上板 | ~~`wbuf_handle(FLUSH)` 取到页对后 4 条失败返回（命令池满 / 建链 / pending 满 / SQ 满）**丢弃**该页对，而 allocator 无归还 API ⇒ 缓冲保留、每轮重试**白烧 2 页**，表现为容量无声流失（CLAUDE.md §3.1）~~ | 资源次序改为**按有无归还路径排序**（命令槽先于页对）+ 新增 `allocator_rewind_pair` + 统一错误出口归还页对 | **必须上板**：`next_page` 属地址/容量类量（§3.2 编译期无声）。背压下命令池满不易人为构造 ⇒ 静态收口 + 可达路径结构性收窄，**不宣称已取证** | `src/ftl/wbuf.c` `wbuf_handle(FLUSH)`；`src/ftl/allocator.c` `allocator_rewind_pair` |
| RISK-WB-02 | 已修未上板 | ~~TRIM 清空窗口的**内部不变量**（清 `occupied` + 退 `owners` 在飞数 + 复位）由 `ftl_trim`（外部模块）手工维护；`cli.c` 的 `bbt --erase` 直接 `memset(&ftl->wbuf,…)` 丢弃 `owners` ⇒ 那些写组在飞数永不归零、host 写挂死~~ | 不变量收进 wbuf 模块：新增 `wbuf_handle(DROP)`（TRIM）/ `wbuf_handle(RESET)`（介质擦净，按写失败收尾）；调用点改为调接口（§3.1 / C3 / H13） | 上板：`trim` 走新路径不回归；`bbt --erase` 后不再有写组挂死 | `src/ftl/wbuf.c`；`src/ftl/ftl.c` `ftl_trim`；`src/controller/cli.c` |
| RISK-WB-03 | 待上板 | 模块抽出（`ftl.c` §4 → `wbuf.c`/`wbuf.h`）+ 位运算收敛到 `ftl_cw_bit` + hot-path 日志降 `LOGD`：纯搬移与等价改写，但 wbuf 是写路径核心 | 逐段比对搬出前后函数体一致；`make all` / `make release` 零新告警 | 随常规回归上板（`wr`/`rd`/`fl`/`trim` + `wp <lba> 1 8` 合并仍为 1 次编程） | `src/ftl/wbuf.c` / `include/ftl/wbuf.h` |
| RISK-WB-04 | 已修未上板 | `ftl->groups[gi].outstanding++` 是**跨模块字段写**（§3.1 形态）：写点 `wbuf.c`（append）与 `ftl.c`（`ftl_confirm_write`），递减点在 `ftl.c` 与 `pending.c` —— 组在飞数这一不变量由多个模块各自手工维护，任一处漏掉即 host 写组挂死 | 目前两处写点都紧邻 `pending_group_*` 调用，人工可核；本轮**未**加闸门（不在批准范围） | 若再出现第三个写点 ⇒ 加 `pending_group_add_subop()` 唯一闸门，与 `pending_group_complete_subop*` 配对（H10 / §3.1） | `src/ftl/wbuf.c` `wbuf_handle(APPEND)`；`src/ftl/ftl.c` `ftl_confirm_write`；`src/ftl/pending.c` |
| RISK-WB-05 | 信息 | `ftl_init` 的 `memset(&ftl->wbuf, 0, sizeof(ftl->wbuf))` **有意保留**，不换成 `wbuf_handle(RESET)`：init 期构造，`g_ftl` 在 `.bss` 已零；换 `wbuf_handle(RESET)` 会在 `pending_init` 已清组表后带着陈旧 `owners` 调 `pending_group_complete_subop_multi` ⇒ 风险大于收益 | 有意不改（P5 留痕） | 若 `ftl_init` 的顺序变化（`pending_init` 之前有 wbuf 写入）需重估 | `src/ftl/ftl.c` `ftl_init` |
| RISK-WB-06 | **已修并上板（基础数据面）** | **wbuf 误用单页版填装函数**：`wbuf_program_page` 的源是 plane-major pair 镜像（`slot = plane*CW_NUM + cw`），却调用 `ftl_cmd_write_data_cw`（契约是**单页镜像**，只按 `cw*512` 取数）⇒ plane1 的 4 个 CW 全部复制 plane0 的数据。表现：host 大文件每 4KB 的后 2KB 重复前 2KB（rev26 `notepad.exe` 360448B：2KB 块 1=src0、块 3=src2 …）。CLI `wr 0 32 c` 的 pattern 每 2KB 相同，**完全看不见** | 新增 `ftl_cmd_write_data_pair_cw`（槽位 plane*CW_NUM+cw），wbuf 改用它；`ftl_cmd_write_data_cw` 保持单页语义给 mirror/GC/WL | **rev27 已上板验证**：Windows FAT 格式化成功；写 `notepad.exe` + 16KB 非均匀 pattern → `Write-VolumeCache` → 拔插 USB → `SRC==DST`、`PAT` 一致、2KB 块 0..7 内容分别为 0..7（`all_match=True`）。剩余：大文件/GC/掉电回归 | `src/ftl/wbuf.c` `wbuf_program_page`；`src/ftl/ftl_fc_interface.c` `cmd_write_data_cw_core`；上板 rev27 哈希/块指纹证据 |

---

## 2J. FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h` + `bg_wiring.c`，本轮审查）

> 本轮把接线从 `ftl.c` 抽成 `src/ftl/bg_wiring.c`（任务表 / 13 个 `bg_a_*` 适配器 /
> 档位预算 / Phase 3 入口 `ftl_bg_service`），`ftl.c` 只留一行调用。
> 根因：`40f2bee`（磨损并入 Checkpoint）把任务表 14 → 13 项，只改了代码没改注释与结构。

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-BG-01 | 已修未上板 | ~~`bg_a_rr_predict` 的内部门 `(now_ms - rr_last_scan_ms) >= RR_SCAN_INTERVAL_MS` 在时钟缺失（`ftl_now_ms()` 恒 0）下恒为假 ⇒ 慢死预测巡检**永久停摆**；与同族 `mig_gc_evaluate` 的哨兵语义（退化为「每轮求值」）方向相反，也与注释宣称的「内部门兜底」相反~~ | 补 `\|\| (rr_last_scan_ms == 0U)` 哨兵，与 GC 同构 | 上板：确认 1 s 节流仍在（巡检不逐轮跑） | `src/ftl/bg_wiring.c` `bg_a_rr_predict` |
| RISK-BG-02 | 已议 | `mig_gc_evaluate` 的 `gc_eval_last_ms` 节拍门与调度器 `interval_ms = GC_EVAL_INTERVAL_MS` **同源同值双门**（v0.76「节拍统一进任务表」的残留）。删任一门都不改变当前行为 | 保留双门 | 保留的理由是「调度器 interval 被误置 0 时仍不逐轮全表扫描」（本函数成本 = O(全表)）。若出现第二调用点或 interval 语义变更 ⇒ 重估是否合并 | `src/ftl/bg_wiring.c` `mig_gc_evaluate` / `bg_a_gc_eval` |
| RISK-BG-03 | 信息 | 时钟缺失降级分支（`clock_ok == 0`）在本板**不可达**：`ssd_platform_register` 拒绝 `time_us == NULL` ⇒ 整条降级路径从未被验证（RISK-BG-01 的缺陷就长在这条不可达路径上，靠读码而非运行发现） | 分支保留（引擎通用性 + 防御） | 换平台 / 改注册路径时重估；不要在这条路径上堆策略 | `src/platform/platform.c` `ssd_platform_register`；`src/ftl/bg_scheduler.c` |
| RISK-BG-04 | 已修未上板 | ~~任务表装载**无边界检查**：加第 14 项会 `tasks[13]` 越界写踩掉 `task_count`（§3.2 静默失效，编译期无声）；且适配器注释里的位号（12 / 13）与真实下标（11 / 12）不符（H6）~~ | 改表驱动 `s_bg_tasks` + `_Static_assert` 容量门 + `bg_scheduler_init` 入口 `ASSERT(count <= BG_TASK_COUNT)`（**在拷贝循环之前**）+ 删写死位号（改为表序描述） | 上板：IDLE / NORMAL / BUSY 各跑一轮，确认 13 项照常执行 | `src/ftl/bg_wiring.c` `s_bg_tasks`；`src/ftl/bg_scheduler.c` `bg_scheduler_init` |
| RISK-BG-05 | 已修未上板 | ~~`bg_scheduler_t` 死字段 `priority` / `round_total_us` / `round_used_us` / `round_seq` / `level` / `clock_ok` 无生产消费者（H5）；`priority` 与表序是**同一语义的两套坐标**且已漂移（取值跳过 5，缺口正是被删的 Wear 项）~~ | 删除；`clock_ok` 降为 `bg_scheduler_run` 局部变量；表序即优先级（C9） | 上板：确认无行为变化（表序未变） | `include/ftl/bg_scheduler.h`；`src/ftl/bg_scheduler.c` |
| RISK-BG-06 | 已修未上板 | ~~`bg_a_l2p_flush` 经 `ftl->bg.level` **反查** `g_bg_level_cfg` 取 `l2p_flush_max` = 第二套配置读取路径，与引擎实际收到的 `cfg` 可能不同步（引擎按形参跑、适配器按字段读）~~ | 引擎在 `bg_scheduler_run` 起点发布 `sched->cfg = cfg`，适配器读 `ftl->bg.cfg->l2p_flush_max`（单写点） | 上板：BUSY 档 flush 条数应为 1（`BG_BUSY_FLUSH_COUNT`） | `src/ftl/bg_wiring.c` `bg_a_l2p_flush`；`src/ftl/bg_scheduler.c` |
| RISK-BG-07 | 已修未上板 | ~~`bg_a_journal_erase` 手工读 journal 内部状态 `job.phase` / `job.need_erase`（§3.1 形态：判据属别的模块）。journal 一旦改内部表示（如换 phase 枚举），该守卫**静默恒假** ⇒ 换块擦除永不启动 ⇒ 新环形块未擦就写~~ | 判据收回 owner 模块：`journal_handle(ERASE_ONLY)()`（`journal.h` 声明 / `journal.c` 实现），行为与原守卫逐字等价（`phase == JRN_IDLE && need_erase != 0` 才调 `journal_handle`） | 上板：跑 journal 换块路径（写满一个环形块），确认擦除照常补起 | `src/ftl/journal.c` `journal_handle(ERASE_ONLY)` |
| RISK-BG-08 | 信息（有意设计） | `bg_suspend` 由诊断命令直接写 FTL 上下文字段（跨模块字段写，§3.1 形态） | 字段契约已写明「只由诊断命令置位、无清除路径」；判定已收进 `ftl_bg_service` 首行 | 诊断命令增多 / 出现第二个写点时考虑加 `ftl_bg_suspend()` 闸门 | `src/controller/cli.c`（`bbt --erase`）；`include/ftl/ftl.h` `bg_suspend` |
| RISK-BG-09 | 待测试源 | 本轮 API 变更后 `make tests UT=FTL` 失败：`test_ftl_bg.c` 仍用旧 `bg_scheduler_init(sched)` 单参签名、`bg_task_t.priority`、`bg_scheduler_t.round_total_us/round_used_us/round_seq`；`ftl_test_common.c` 另有既存旧字段（`F->metadata_dir` 取值、旧 `l2p_cache_init` 签名、`allocator.wear`） | 未改测试源（R4）；`make all` / `make release` 均通过 | 测试所有者按新 API 更新后重跑 `make tests UT=FTL` | `tests/unit/test_ftl_bg.c`、`tests/unit/ftl_test_common.c` |

---

## 2K. 2026-10-19 缺陷收口（代码已完成，未上板）

> 本轮按「先改代码、上板以后」执行：处理了 4 条待决策与可无板完成的正确性/健壮性
> 待后续项。**全部未上板**（R1），上板回归见本节各行与 §5。
> 纯性能项（PERF-01/02、GC-05、Prefetch/多流）与需介质格式变更的 BBT-04、
> 依赖换 NAND 引擎的 BBT-06 未处理。
>
> ⚠ 原表各条的「当前缓解 / 重访触发」是**审查当时**的描述；本轮落实情况以本节的
> 「改动」列为准（status 已同步为 `已修未上板` / `已议`）。

| ID | 改动 | 证据 | 上板验证 / 下一步 |
|---|---|---|---|
| MD-01 | 元数据回收块连续擦除失败 3 次 ⇒ `bbt_mark_bad` + 清回收位，不再无限重试 | `MD_RECLAIM_FAIL_MAX`；`src/ftl/metadata_job.c` `md_complete_reclaim` | 注入擦除失败：第 3 次后块进 BBT、回收位清零、后台不再重试 |
| MD-02 | 压缩空槽预算预留 1 槽（`MD_COMPACT_RESERVED_SLOTS`），消除在飞 region 写导致的良性中止 | `src/ftl/metadata_job.c` `md_compact_begin` | 压缩期间并发写：不再出现 “无空槽中止” |
| MD-04 / TYPE-02 | `fc.c` 最后一处手写 ERROR_MASK 改走 `fc_cw_status_has_error()`（H15 归零） | `src/fc/fc.c` `fc_complete_from_nand` | 随读错误注入回归（首个失败 desc 仍正确） |
| GC-01 | 同一 victim 的**每个 plane 源块**连续读失败 3 次 ⇒ 标 BAD + 从 Closed 条目摘除该 plane，GC 换候选。计数跨 job 保留（`mig_start` 保存/恢复）；**单页读成功不清零**，只在整块迁移成功（`mig_finish`）或标 BAD 时清，否则 3 次阈值永不生效 | `MIG_VICTIM_READ_FAIL_MAX`；`migrate_handle_t.fail_gblock[PLANE_NUM]` / `fail_count[PLANE_NUM]`；`src/ftl/migrate.c` `mig_complete_read` / `mig_start` / `mig_finish` | 造不可纠读：第 3 次后容量继续增长，GC 不再空转（RISK-GC-07 的根因） |
| GC-07 | 单页读成功不再清零读失败计数；计数按 plane 源块分开 ⇒ 坏页可稳定累计到 3 次并隔离，满盘/近满盘 GC 不再永久重选同一 victim | `src/ftl/migrate.c` `mig_complete_read`；`include/ftl/migrate.h`；`src/ftl/ftl.c` `migrate_fail_state_clean` | 上板 H2testw 写到 ≥2/3：`mark BAD` 后 `reclaimed` 增长、`free` 回升 |
| GC-02 | 目的页写失败 ⇒ **立即** `allocator_close_open_sb(COLD)`，**不**标 BAD（已有成功迁移页）；失败页可能半编程，不能 rewind，关闭后失败页留在 SB 尾部，下一次 GC 换新目的块 | `src/ftl/migrate.c` `mig_dst_fail_cleanup` / `mig_complete_write` | 注入目的页写失败：关闭 Cold SB、已迁移页仍可读、下一页不再写同一 SB |
| GC-03 | 目的页写提交失败 / stall 超时：本页对无任何 plane 编程 ⇒ `allocator_rewind_pair(COLD)`；前序 plane 已编程 ⇒ 关闭 Cold SB。不得在同一 Open SB 上继续写下一页（否则留下 hole） | `src/ftl/migrate.c` `mig_dst_fail_cleanup` / `mig_step_write`；`allocator.c` `allocator_rewind_pair` | 注入提交超时：plane0 场景页对回退、plane1 场景关 Cold SB |
| GC-04 | 每擦完一个 plane 即从 Closed 条目摘除（`allocator_closed_drop_plane`），防止部分擦除后旧条目误认 | `src/ftl/allocator.c`；`mig_complete_erase` | 两 plane victim 中途断电/中止：重启后旧条目不含已释放 plane |
| GC-05 | `waiting` 期间也 arm `FTL_WAIT_TIMEOUT_MS` deadline；完成丢失时 abort，不再永久 `active=true`；写阶段超时同时关闭 Cold SB（防 hole）。迟到的完成由回调的 `(!active || !waiting)` 分支 release | `src/ftl/migrate.c` `cmd_wait_ms` / `mig_step`；`fc.c` CQ-full 丢完成 | 注入完成丢失：GC/WL 超时中止并恢复 |
| GC-06 | `ftl_migrate_callback` 增加 `ctx == NULL` 早退（重复/迟到完成只 release 命令）；对齐 RR 同族守卫 | `src/ftl/migrate.c` `ftl_migrate_callback`；`read_recovery.c` 同族写法 | 注入重复完成：release 命令，不解引用 NULL |
| MIG-01 | 等 L2P 加载 / Journal 刷页用独立 ms deadline（`FTL_WAIT_TIMEOUT_MS`），超时 abort（victim 不擦） | `src/ftl/migrate.c` `mig_wait` | 注入 region 加载永久卡住：job 超时中止而非永久挂起 |
| JN-01 | 环形块连续擦除失败 3 次 ⇒ 标坏（RAM + BBT 两个 plane）+ 跳到下一可用块；全坏则 `ring_full` 停止写入 | `JRN_ERASE_FAIL_MAX`；`journal_ring_block_bad` / `journal_erase_failed` | 注入环形块擦失败：journal 用剩余块继续；重启后 BBT 仍挡住坏块 |
| BOOT-06 | full metadata rebuild 写指针检测 hole：空白页后仍有写过页 ⇒ 返回全块，逐页反查 valid_count | `ftl_recovery_scan_write_pointer` | 造 hole（page 0 写失败、后续页成功）：valid_count 不少算 |
| BOOT-07 | 判空改看每 CW 的 META（4B，明文），不再依赖 USER 区加扰 | `ftl_recovery_page_is_blank` | 空片/写满块判空仍正确；RANDOM_DISABLE 不受影响 |
| BOOT-08 | Journal 扫描读失败向上传播：判型走掉电路径，回放读失败返回 `FTL_ERROR` ⇒ full metadata rebuild | `journal_scan_latest(..., out_read_error)` / `journal_replay`；`ftl_boot_recovery` | 注入 journal 页读失败：不复用“无更多条目”，启动回退 full metadata rebuild |
| BOOT-09 | Directory 加载失败不再静默建新表：介质有有效 CP 时拒绝启动（空片/格式化后才放行） | `ftl->md_load_failed`；`ftl_boot_recovery` | Anchor 损坏 + 有效 CP：启动停止并 LOGE，不丢映射 |
| BOOT-11 | probe 的 L2P 反查失败计数向上传播，非 0 ⇒ 快照结果作废、回退 full metadata rebuild | `ftl_rebuild_stats_t.l2p_read_errors`；`ftl_recovery_probe_blocks` | 注入 L2P 读失败：不声称 `blocks_restored=1` |
| BOOT-12 | **上板通过（debug，2026-10-20）**：initial CP 由“空 Header”升级为完整 21 页快照（4 页块元数据 + 16 页磨损 + Header 最后 commit）；format 后首次掉电走“快照 + journal”，不再 full metadata rebuild。旧空 initial CP 保留 clean-start 快速路径 | `checkpoint_write_initial`；`ftl_media_format`（reset → carve → snapshot → reset → write）；`ftl_boot_power_failure` | 板上实测：format→`boot type=1 bit=restored`；`wr 0 32` 不 `fl` 断电→`boot type=2 journal_entries=32 bit=restored`、无 fallback、`rd 0 32 c` PASS；覆盖写后 `fl`→CP seq=2→`boot type=1 bit=restored`。首次 PF 的 `probe anomalies=1 valid_cw=0` 是 probe 对未引用池块的统计口径（journal 记账不在该 stat 内），覆盖写/读均 PASS，后续 CP 后 `anomalies=0` |
| L2P-01 | region 加载失败先重试 3 次（置 INVALID），仍失败才降级为全未映射 CLEAN 行 | `L2P_LOAD_RETRY_MAX`；`ftl_l2p_callback` LOADING 分支 | 造瞬态读错：映射不丢；持续坏 region：不活锁 |
| HOST-02 | 逐 plane 反查命令访问的全局块与 ECC（`ftl_cmd_block_ecc_list`），读记账覆盖两个 plane | `src/ftl/ftl_fc_interface.c`；`ftl_host_io_read_ok` | multi-plane 读：两个块的 read_count / ECC 都增长 |
| WB-04 | 组在飞数收敛为 `pending_group_add_subop` / `pending_group_drop_subop` 唯一闸门 | `src/ftl/pending.c`；`wbuf.c` / `ftl.c` 调用点 | 写路径回归：组完成/回滚均在飞数归零 |
| BBT-05 | 固定元数据区为空时抽样用户区 Page 0 的 META（8 点/plane），避免误判空片后重扫丢坏块图 | `bbt_medium_has_ftl_data`；`BBT_USER_SAMPLE_COUNT` | 擦掉元数据区但保留用户数据：`bbt_init` 拒绝出厂扫描 |
| TYPE-03 | 删同类型 `(DWORD)` 强转（`ftl.c` / `wbuf.c`） | H43 | 编译期即可 |
| BG-02 | **已议**：保留 GC 双节拍门（内部门是 interval 误置 0 时 O(全表) 扫描的唯一兜底），不改代码 | `mig_gc_evaluate` / `bg_a_gc_eval` | 无 |

**本轮实现自带的残留（均已登记，不隐藏）：**

- **JN-01**：`erase_fail_count` / `bad_mask` 是 RAM 态。若掉电发生在 `bbt_mark_bad`
  持久化之前，重启后该块会被重新尝试 3 次——属可接受的重发现成本，不丢数据。
- **L2P-01**：`load_fail_count` 随 cache 行保存；行被驱逐后重试预算重置。持续访问的
  坏 region 通常复用同一 INVALID 行，仍能达到上限，但没有全局 per-region 计数表。
- **BOOT-09**：Anchor 损坏时选择**拒绝启动**；当前没有「重扫 L2P region 重建
  Directory」的路径，需要人工决定是否格式化。
- **BBT-05**：用户区是**抽样**（8 点/plane）；数据恰好全部落在抽样点之外仍会误判空片。
- **GC-02**：关闭 Cold SB 依赖 `allocator_close_open_sb`；若 Cold SB 已被关闭，
  该调用返回 `FTL_ERROR`（no-op），下一次 GC 仍可能选到同一目的块。

---

## 2L. FTL 模块 handle 契约（2026-10-19 设计，未改代码）

> 设计文档：`docs/design/ftl_module_contracts.md`。本节只登记架构决策与后续项，
> 避免下一轮把「有意设计」当缺陷重查（R11）。

| ID | 状态 | 风险 / 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-ARCH-01 | 信息（有意设计） | 共享数据面账本（L2P / BST / allocator / journal 水位 / metadata / pending / completion）保持共享 device context，模块只封装执行状态；不做每模块私有副本 | `ftl_module_contracts.md` 的契约与字段所有权表；typed handle + 端口 | 若有人提出「模块私有账本/去掉共享 context」，先读契约文档；多实现/多 device 需求才重估 | `docs/design/ftl_module_contracts.md` §1/§8 |
| RISK-ARCH-02 | 待后续（性能） | 跨 LUN superblock 条带 / 跨 LUN GC/WL **未实现**；当前 migrate 严格 per-LUN（源/目的/账本都在本实例） | per-LUN `migrate_handle`；`lba_routes_to` 守卫 | 上板基线稳定、需要多 die 均衡/条带性能时重估；需要设备级 relocation 引擎 + 跨实例事务，不是改 handle 入参 | `multi_lun_generalization.md:26,358-359`；`migrate.c:553-557,524`；`allocator.c:26-34` |
| RISK-ARCH-03 | 信息（有意设计） | 无模块级 liveness supervisor / 无 per-engine watchdog；IWDG 只兜系统级卡死 | 各 handle ms deadline + 主循环轮询 + IWDG | 若出现主循环卡死/漏 deadline 的实际案例，加主循环 progress monitor；**不要**在 ISR 里跑状态机或喂狗 | `known_risks.md` §6 设计；`src/main.c` IWDG |
| RISK-ARCH-04 | 已议（本轮保留 compact） | RR 的 desc 保留方案（本仓自拟、非主流）**未实现**；本轮按 §〇 主流判据保留 **compact 重下发**：失败读释放命令，RR 按 LBA/PBA 重新下发新命令 + 新 buffer。代价：需 FC descriptor 公开原语 + 所有权契约拆分；当前机制已验证可用 | `rr_handle` 只持有 `rr_owner_t`（owners/pending_idx/upper_buf/lba/n_cw）；`desc_count` 恒 0 | 若未来实测 desc 保留有收益（减少一次重读命令构建）再评估；否则维持 compact | `src/ftl/read_recovery.c`；`include/ftl/read_recovery.h` |
| RISK-ARCH-05 | 已改未上板 | 独立 `confirm_handle` **未采用**；write-confirm park 已收进 `pending_confirm_service()`（`pending_confirm_park/park_ms/超时`）；TRIM 仍留在 `ftl.c` | 停泊列表 = pending 表的 `PENDING_OP_WRITE_CONFIRM` 项；超时 `FTL_GROUP_DRAIN_TIMEOUT_MS` 后 complete group IO_ERROR | Journal→L2P→BST 顺序有两处实现（confirm/TRIM），**任何顺序变更必须同步两处**（C2/R9） | `src/ftl/pending.c` `pending_confirm_*`；`src/ftl/ftl.c` `ftl_confirm_handler` |
| RISK-ARCH-06 | 待后续 | 模块 handle 契约迁移**未上板**：只证 `make all` / `make release` 零新告警；typed 入口/端口/字段归属改动量大，行为保持需板端回归 | 每个 Phase 保持行为；新增状态上报只读；`ftl_status_t` 增 `FTL_TIMEOUT`（未消费） | 上板回归：正常读写/GC/WL/RR/CP/journal/BBT/格式化/掉电重启；重点看完成路由与组结算 | `docs/design/ftl_module_contracts.md`；本轮各模块 `*_handle` |
| RISK-ARCH-07 | 已改未上板 | §8.1 Phase 1.5：`migrate_handle` / `wbuf` / `rr_page_scratch` 已从 `ftl_context_t` 改为 `ftl_storage_t` 挂载指针；数组落 AXI `.bss`（`g_ftl_migrate` / `g_ftl_wbuf` / `g_ftl_rr_page_scratch`） | `ftl_init` 绑定 + 显式 memset；AXI 预算 `_Static_assert` 已把三块计入 | 上板确认 RAM 实测与 cache/局部性；若需要域优化再调整放置 | `src/main.c`；`include/ftl/ftl.h` `ftl_storage_t` |
| RISK-ARCH-08 | 已改未上板 | migrate / RR / CP 的提交/等待计数已由**轮数**改为绝对 ms deadline：`FTL_SUBMIT_TIMEOUT_MS = 10` / `FTL_WAIT_TIMEOUT_MS = 50`；confirm park 用 `FTL_GROUP_DRAIN_TIMEOUT_MS = 100` | 时钟缺失（now_ms == 0）时 deadline 不生效（退化为无超时；本板 time_us 恒非空 ⇒ 不可达） | 上板标定这三个值（NAND 页操作 ~700µs + 主循环轮速）；观察 `timed_out`/`timeout_phase` 上报 | `include/ftl/ftl_module.h`；`migrate.c` / `read_recovery.c` / `checkpoint.c` / `pending.c` |
| RISK-ARCH-09 | 已改未上板 | §5 PFR 收为 boot orchestrator：fresh/clean/PF 子流程与 full metadata rebuild 原语（restore/probe/build_pools/reset/page_is_blank/scan_write_pointer/close_block/carve/rebuild）全部改为 `static`；公开头只留 `ftl_media_check_format` / `ftl_media_format` / `ftl_boot_recovery` | `checkpoint_block_words()` / `metadata_live_count()` / `l2p_cache_invalidate_line()` 三个穿透已收口 | 上板回归 fresh/clean/PF/full metadata rebuild 四条启动路径 | `src/ftl/power_failure_recovery.c`；`include/ftl/power_failure_recovery.h` |
| RISK-ARCH-10 | 信息（有意偏离） | 为适配本仓 **per-instance** 架构，typed 入口有几处签名偏离设计文档原文：`journal_handle` 增加 `journal_trigger_t`（区分 FLUSH / ERASE_ONLY）；`wbuf_handle` 返回 `ftl_status_t`（调用者必须分支 FTL_AGAIN/ERROR）；`pending_confirm_*` 带 `ftl_context_t *`；`rr_handle` 的 `rr_trigger` 仍登记 owner 而非 desc 链 | 各偏差已在头文件与 P2/P5 注明 | 若未来引入设备级 pending/wbuf 上下文，再评估收回这些实参 | `include/ftl/journal.h` / `wbuf.h` / `pending.h`；`docs/design/ftl_module_contracts.md` §3.3 |

---

## 2M. USB MSC / 平台（2026-10-20，代码已落地）

> 设计文档：`docs/design/usb_msc_udisk.md`。本节只登记「未上板 / 待确认」项。

| ID | 状态 | 风险 / 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-USB-01 | 已上板（基础数据面，rev30）；容量口径 rev33 改为 **429 MiB**（RISK-CAP-02）；大文件/掉电待测 | **已通过（rev27，旧容量参数）**：枚举（`[USB] mounted`）、Windows 新建 MBR 分区 + FAT 格式化、`notepad.exe` 与 16KB 非均匀 pattern 写入 → `Write-VolumeCache` → 拔插后 SHA256/块指纹一致。rev30 已确认旧容量 **437 MiB（458,227,712B / 894,976 sectors）**；rev33 起改为 **429 MiB（449,839,104B / 878,592 sectors）**（固定 L2P 日志区）。**未测**：大文件/GC 压力、`dd` 吞吐、写过程中掉电恢复 | `make all` / `make release` / `UT=FC` 零告警；`RISK-WB-06` 已修并上板 | rev33 上板：首启自动格式化（介质 v10）→ `Get-Disk Size=449839104` → 重建 MBR/分区/FAT → H2testw 全量；见 §2N/§2O | `src/controller/usb_msc.c`；`src/platform/stm32h743/usb_port.c`；rev27 哈希证据 |
| RISK-USB-02 | 待上板 | MSC 异步完成路径未实测：`tud_msc_async_io_done()` 的 deferred 回调、主机复位期间的 `discard` 收尾都只在静态路径成立 | `discard` 标志 + `tud_ready()` 守卫；FTL 只持有静态 bounce buffer | 上板：读写过程中拔线/总线复位/重新枚举，确认无死命令、无残留 CSW 错误；确认不会出现 `pending_io` 永久为真 | `src/controller/usb_msc.c` `usb_msc_task` / `tud_umount_cb` |
| RISK-USB-03 | 已确认（上板枚举通过） | PA11/PA12 = OTG_FS 的接线曾未确认（Apollo V2 也可能引到 PB14/PB15）；2026-10-20 上板 `[USB ] mounted` 证明当前板走 PA11/PA12 AF10 可用 | 代码按 PA11/PA12 AF10 实现；`CFG_TUD_VBUS_DETECT_HW=0` 避开 PA9 冲突 | 换板/换核心板时重新确认；若某板无枚举，再查 PB14/PB15（OTG_HS 内嵌 FS PHY，AF12） | `docs/hardware_contract.md` USB 节；上板日志 `[USB ] mounted` |
| RISK-USB-04 | 信息（有意设计） | USB MSC/BOT 是**单命令在飞**；UASP 需要 USB 3.0 bulk streams，本板不可用 ⇒ 不能作为「多命令在飞」的学习载体 | 设计文档 §5 已写明；FTL 内部仍可把一条命令拆多个子操作并行 | 若目标是学主控队列/门铃，另做 NVMe 风格 SQ/CQ 软件模型，不要改 USB 路径 | `docs/design/usb_msc_udisk.md` §5 |
| RISK-USB-05 | 待上板 | WRITE(10) 的 **FUA** 位未处理：主机若只发 FUA 而不发 SYNCHRONIZE CACHE，命令完成时数据仍在 FTL 缓冲/日志，不保证已编程到 NAND | 主机 fsync 路径通常随后发 SYNCHRONIZE CACHE；`ftl_flush` 会先刷 wbuf 再刷 L2P/journal | 上板抓包确认主机是否使用 FUA；若使用，需要在写完成路径补 flush（或在 MSC 回调里记录 FUA 并要求 `usb_msc_task` 完成后 flush） | `src/controller/usb_msc.c` `usb_msc_transfer`；`src/ftl/ftl.c` `ftl_flush` |
| RISK-PLAT-01 | 待决策 | linker 的 `.noncacheable_buffer` 输入段匹配漏了前导点（写成 `*(noncacheable_buffer)`），这些段被 ld 当**孤儿段**按启发式放置：加入 USB 描述符前 `g_nand_channel_page_buffer` 实测在可缓存 AXI RAM `0x24000204`，加入后 `.data` 变大又变成非缓存区 `0x24070800`。落点随链接器启发式漂移，与源文件注释「D-Cache 关闭」不构成保证 | **本轮未改**（USB 补丁不依赖该段）。NAND DMA 路径在 `nand_fmc.c` 有显式 `nand_dma_cache_invalidate`，当前不是活 bug | 单独一轮修：`KEEP(*(.noncacheable_buffer))` + 删除重复输出段；改后地址固定会变，属 R2 的「地址」类改动，**必须上板**回归 NAND 读写/GC/掉电。不要与 USB 补丁混在一起 | `linker/stm32h743xx_flash.ld`；`arm-none-eabi-nm -n build/firmware/debug/bin/debug.elf \| grep -E 'g_nand_channel_page_buffer\|g_fc_buffer_pool'` |

---

## 2N. 容量 / 几何（2026-10-20）

| ID | 状态 | 风险 / 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-CAP-01 | 已修并上板（rev30）；容量口径被 RISK-CAP-02/v10 取代（429 MiB） | `FTL_LBA_MAX` 直接取 `FTL_IU_TOTAL`（全部物理 CW），把固定元数据（92 块）、OP（408 块）、Spare（100 块）也算进主机容量：对外 1,048,576 LBA = **512 MiB**，实际可写数据块只有 **3496 块 = 437 MiB**，**超卖 75 MiB（约 17%）**。主机写满后 Free/OP 耗尽，GC 无块可搬，写入失败/抖动；H2testw 512MB 必在后段失败 | 修法：`FTL_HOST_DATA_BLOCKS_PER_INSTANCE = FTL_BLOCK_TOTAL − (FTL_METADATA_BLOCK_END + FTL_OP_BLOCKS_PER_PLANE + FTL_SPARE_POOL_PER_PLANE) × PLANE_NUM` = 3496；`FTL_LBA_MAX` = **894,976（437 MiB）**；`FTL_USER_CAPACITY_BYTES` 更名 `FTL_MAPPING_CAPACITY_BYTES`（映射空间），新增 `FTL_HOST_CAPACITY_BYTES`。L2P 表/Anchor/Directory 布局不变 | rev30 已确认：`lba_max=894976 (437MiB)`；Windows `Get-Disk Size=458,227,712`；FAT 格式化；`usb_verify write/check` 拔插后 `RESULT=PASS`。**待测**：H2testw/大文件写到 437 MiB 边界不应 GC 抖动。旧介质上 LBA>894,975 的映射需 CLI `format` 清掉（不抬介质版本）。**DIM>1 待核对**：`FTL_LBA_MAX` 按每实例容量 × 维度数计算，但 host region 是路由到实例的；需确认各实例实际写入不超过 1748 region（当前只验证 DIM=1） | `include/ftl/ftl.h` `FTL_HOST_DATA_BLOCKS_PER_INSTANCE`；`include/ftl/ftl_config.h`；`docs/design/ftl_design.md` §2.1 / ADR-8；上板 `info free=3496` |
| RISK-CAP-02 | 已修未上板（方案 A，2026-10-20） | **主机容量未扣“动态元数据（L2P 日志块）”占用**：旧实现 `FTL_HOST_DATA_BLOCKS_PER_INSTANCE` 只扣固定元数据/OP/Spare；动态 LOG 块由 `allocator_alloc_log_block()` 从 **Free Pool（用户数据块）** 取 ⇒ 用户实际可写 = 437 MiB − live_log 占用。理想 L2P 表 = 2048 region × 2KB = 4 MiB（32 块）；现状碎片化到 1076 块 ≈134 MiB（RISK-MD-07）。rev32 H2testw 在 343 MiB 处因物理空间耗尽失败 | **方案 A 已实现**：① 固定预留 L2P 日志区 `L2P_LOG_BLOCK0=46 .. FTL_METADATA_AREA_END=78`（32/plane，总 64 gblock = 8 MiB），由 metadata 模块独占，Free Pool 不含它；② 容量公式改用 `FTL_METADATA_AREA_END`：数据块 = 4096 − (78+204+50)×2 = **3432 = 429 MiB**，`FTL_LBA_MAX = 878,592`，`FTL_HOST_CAPACITY_BYTES = 449,839,104`；③ 介质版本 **9 → 10**（块布局变更），首启自动格式化是预期；④ `info` 新增 `live=`（活日志块数） | **上板判据**：① 首启自动格式化后 `lba_max=878592 (429MiB)`、Windows `Get-Disk Size=449,839,104`；② H2testw 全量写满分区末端不出现写失败；③ `info` 的 `live=` 保持在 `L2P_LOG_COMPACT_TRIGGER`（40）附近而非上千；④ 重启后快照恢复且日志区状态正确（活块 LOG、空闲块 NONE） | `include/ftl/metadata.h`（L2P_LOG_* / AREA_END / 分区表）；`include/ftl/ftl.h` `FTL_HOST_DATA_BLOCKS_PER_INSTANCE`；`src/ftl/metadata.c` `metadata_log_alloc` / `metadata_log_area_restore` / `metadata_dir_live_block_count`；`src/ftl/metadata_job.c` `md_note_block_erased` / `md_compact_next`；主流判据 `docs/reference/pages/spdk_ftl.html`（nvcache / band tail / surfaced blocks） |

---

## 2O. NAND / FMC 时序与吞吐（2026-10-20 分析，未改代码）

| ID | 状态 | 风险 / 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-NAND-01 | 待决策 | **NAND 当前跑默认异步 Mode 0**：`CONFIG_NAND_TIMING_MODE_4` 默认 0（Mode 4 整段编掉），FMC PCC 用板厂保守值 4/6/2/4；`CONFIG_NAND_DMA` 默认 0 ⇒ 数据走 PIO（写路径每字节一次 `__DMB()`）。器件参数页 `timingmodes=0x003F` 表示支持异步 Mode 0–5。后果：数据相裸带宽约 18 MB/s（见 NAND-04 的算术），但写吞吐由 tPROG 主导，Mode 4 对写的收益视 tPROG 而定（典型约 +50%、上限约 +20%），对读约 2×。**不是 200 KB/s 的原因** | Mode 4 与 DMA 都保持默认关闭；功能路径已验证 | 若要提升读吞吐再评估开 Mode 4（必须先修 NAND-03）；DMA 只省 CPU，不缩短 FMC 总线时间 | `include/nand/devices/mt29f___g08a.h:75-77`；`src/nand/devices/mt29f___g08a.c:511-514, 627-675`；`include/platform/dma.h:35-36`；`src/platform/stm32h743/sys_port.c:216-217, 269-272`；`docs/archive/changelog/boot_banner_v62.md:33` |
| RISK-NAND-02 | 待修（口径分叉） | `tRC` 在三处不一致：文件头/设计文档/头文件注释写 Mode 0=50 ns、Mode 4=20 ns；`g_nand_timing_slow` 表写 **20 ns**、`g_nand_timing_fast` 表写 **10 ns**。另 `docs/design/fc_nand_design.md §19.2` 说宏在 `nand_config.h`，实际在 `mt29f___g08a.h`。后果：当前 `t_rc_ns` 不参与 PCC 换算（惰性，无运行时影响）；但开启 Mode 4 或按表核对时序时会把错误值当依据 | Mode 4 编掉 ⇒ 无运行时影响 | 开 Mode 4 前先按器件手册统一三处 `tRC` 与宏位置（文档改动属待办，未改） | `src/nand/devices/mt29f___g08a.c:11, 64, 103`；`include/nand/devices/mt29f___g08a.h:71-77`；`docs/design/fc_nand_design.md:3728-3734` |
| RISK-NAND-03 | 待修（Mode 4 前置） | 若按 ST 精确式（`(SET+1)·tHCLK ≥ …`、`HOLD·tHCLK ≥ …`、`HiZ·tHCLK ≥ …`）核对，`nand_fmc_timing_to_pcc()` 用 5 ns 向上取整得到的 fast PCC（SET=2 / WAIT=1 / HOLD=1 / HiZ=2）在 **HOLD**（1×4.55=4.55 ns < max(tCH,tCLH,tALH)=5 ns）与 **HiZ**（2×4.55=9.09 ns < max(tCS,tCLS,tALS)+(tWP−tDS)=10 ns）各差约 1 个周期。后果：直接打开 Mode 4 可能让器件在超规格时序下工作 ⇒ **静默数据错误**；当前 Mode 4 编掉，无现网影响 | Mode 4 默认关闭 | 启用前按 ST 精确式重算（宁多 1 周期），并按 R2「地址类」上板做写压力 + 回读校验；不要只翻宏 | `src/nand/devices/mt29f___g08a.c:130-202`；`docs/design/fc_nand_design.md:3751-3766`；`third_party/stm32h743/hal/src/stm32h7xx_ll_fmc.c:560-566` |
| RISK-NAND-04 | 待定位（性能） | **USB 写速度约 200 KB/s 的瓶颈未定位**。算术上界：Mode 0 字节访问 4+6+2=12 个 FMC 周期 ≈54.5 ns ⇒ 数据相 ~18 MB/s；4 KB pair 写 ≈ 数据搬入 230 µs + tPROG（典型 200 µs / 最大 600–700 µs）⇒ **4.6–8.4 MB/s**；多 plane 程序 UOP 内含 `WAIT_READY`（`min_busy_us=0`），不会死等 700 µs；只有单 plane ISSUE 走 700 µs 最小等待。USB FS 12 Mbps 理论上限 1.5 MB/s、BOT 实测约 0.8–1 MB/s。200 KB/s ≈ 每 4 KB 20 ms，比 USB 上限还低 4–5 倍 ⇒ 瓶颈不在 NAND 模式。候选：主机 WRITE(10) 的实际分块/往返（TinyUSB 按 `CFG_TUD_MSC_EP_BUFSIZE=4096` 分块，若主机按 512 B 下发则 BOT 往返占主导）、FTL 同步等待、journal/L2P/CP/GC 争用（测量时 GC 正处活锁） | rev31 已修 GC 活锁（RISK-GC-07），待复测 | ① rev31 复跑 H2testw，记录 `[GC]/[CP]` 频率与速度变化；② 需要时临时加 `WRITE(10) length` 直方图或 DWT 分段计时（改代码前先确认）；③ 用 `test /fc-nand/...` 的用例 µs 或 CLI 大块写读作 NAND 侧对照 | `src/controller/usb_msc.c` `usb_msc_transfer`；`third_party/tinyusb/src/class/msc/msc_device.c:888-905`；`include/tusb_config.h` `CFG_TUD_MSC_EP_BUFSIZE`；`src/nand/nand_fmc.c:289-294, 371-375`；`src/nand/onfi_1_0.c:881, 886` |
| RISK-NAND-05 | 待定位（上板观测 2026-10-20） | **used 介质持续出现不可纠读页**：rev32 启动后 host 读 LBA 129 反复报 `[ERR] [FTL] read unrecoverable lba=129 gblock=1064 retry=8`。RR 的 Stage 1（读重试）用 Feature 地址 **0x89**——本器件该地址是**保留段**（见 §四），实际没有电压/软判决重试；ECC 不可纠 ⇒ 直接 `bbt_mark_bad(1064)` + host `IO_ERROR`。L2P 仍指向该坏块（悬空），同一 LBA 的每次 host 读都会重试 8 次并重复打日志；若该 LBA 属文件系统元数据，Windows 可能把卷判为损坏/RAW。已登记坏块：305（GC 活锁）、1064（本次）。**非本仓 FTL 逻辑产生**，是芯片物理页失效（或早期写坏）；数据在该 LBA 上本就不可读 | 坏块由 `bbt_mark_bad` 隔离（RAM + 后台 BBT 持久化）；格式化后该 LBA 会重映射到新页，日志停止 | ① 现场处理：Windows 快速格式化（或 CLI `format` + POR）重建文件系统后再跑 H2testw；② 观察 1064 是否在重启后仍为 BAD（BBT 持久化）；③ 若坏块持续增长，评估换芯片 / 真实 read-retry（需器件支持，0x89 不可用）| 板端 rev32 串口：`[ERR] [FTL] read unrecoverable lba=129 gblock=1064 retry=8`（两次）；`src/ftl/read_recovery.c:284-293`；`docs/design/known_risks.md` §四「0x89 保留地址」 |

---

## 3. 类型 / 显式强转（H44，提交 `6050703`）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-TYPE-01 | 已修未上板 | ~~release 下 `ftl_fc_interface.c` 的 `allowed` / `plane_bits` 只在 `ASSERT` 里使用 ⇒ 检查被裁空、两条 unused 告警；注释声称的 FC 侧 release 拒绝已不存在（FC 建链校验全 ASSERT）~~ | `ftl_cmd_validate_cw_bitmap` 改为返回 `ftl_status_t`：ASSERT + release 运行期拒绝（空集 / 越段 / multi-plane 未覆盖每个 plane）；两个建链入口统一处理 | 上板：release 下越段位图应被拒绝而不是发空链 | `src/ftl/ftl_fc_interface.c` |
| RISK-TYPE-02 | 已修未上板 | 与 RISK-MD-04 同一项 H15 欠账，不重复登记 | — | 见 RISK-MD-04 | — |
| RISK-TYPE-03 | 已修未上板 | `cmd->meta = (DWORD)<DWORD>` 是**同类型强转**（`DWORD` ≡ `uint32_t` ≡ `DWORD`，见 `ssd_types.h`）⇒ 按 H43 属可删；但这是**全仓既有习语**（`ftl.c`、`ftl_fc_interface.c`、`wbuf.c` 三处同款），只改一处反而不一致（S1） | 逐处删等于制造风格分裂，暂不动 | 待一次全局收敛（连同 `cmd->meta` 的字段类型一起看，可能应由 H44 从生产者侧收窄） | `grep -rn 'meta\s*=\s*(DWORD)' src/ftl` |

---

## 4. 跨模块 / 历史欠账（已在 memory，不重复展开）

| 主题 | 记录位置 | 要点 |
|---|---|---|
| 介质布局 v9 未上板 | memory `ftl-media-layout-v9-unverified.md` | Anchor/CP P0+P1、Journal multi-plane 擦、Wear 并入 CP；老介质首启格式化 |
| allocator 不变量 / 列表容量 | memory `allocator-invariant-api-v0p77.md` | `allocator_close_sb` 列表满静默丢弃 vs `add_closed_sb` ASSERT 不对称；阶段 3「让 FREE 只说真话」未做、须上板 |
| FTL 入口守卫欠账 | memory `ftl-entry-guard-debt-census.md` | 77 处未覆盖；不是活 bug，但未来重构可达即静默降级 |
| UT 冻结 / 异步套件计划 | memory `ftl-refactor-ut-frozen.md`、`ftl-ut-async-plan-v0p75.md` | 不改测试源；异步套件实现待 FTL 落定 |
| MemManage / 栈指针被写坏 | memory `hardfault-open-v0p71.md` | 已按用户裁决降级关闭、只记录；`-fstack-protector-strong` 未做 |
| 擦除 op 18→17 未决 | memory `cmd-operation-18-to-17-open.md` | 固件侧实测零改写，原因未闭合 |
| IO 路线建议点 | `.claude/io_review_backlog.md` | 只登记不分析；编号 `IO-NNN` |
| `read_recovery` 0x89 保留地址 | memory `nand-feature-addr-table-mt29f4g08.md` | 用户明确不处理 |

---

## 5. 本轮已修但未上板（板端回归清单）

- metadata 重构（`992985c`）：ring 几何、plane 轮转、O(1) 动态块判据、reclaim 轮转、Anchor cheap probe。
- PBA getter 自然宽度 / H44（`6050703`）：纯类型改动，编译期验证；无介质/行为变化。
- GC/migrate/Journal（`fdc7c7f`）：见 RISK-GC-06 / RISK-JN-03。
- BBT（本次审查）：`bbt_mark_bad` 幂等、`dirty_gen`、列读失败、两阶段 `bbt_format_erase`、
  sync 写/擦错误掩码；见 RISK-BBT-01 / RISK-BBT-02 / RISK-BBT-03。
- 掉电恢复（本次审查）：F4 逐 region 预载、full metadata rebuild→carve、CP 介质状态三态、格式化中断恢复、
  Spare 补池、restore Pass 0、materialize PBA 校验；见 RISK-BOOT-01~05。
- L2P cache（本次审查）：`flush_kick` 背压改报 `FTL_AGAIN`、CLOCK 引用位置 1、
  位图/命中记账收敛、删除零调用 helper；见 RISK-L2P-03。
- Journal（本次审查）：擦除失败判据改用全集、drain 超时拒绝同步刷、resume 擦失败
  重新挂起、同步屏障返回状态、回放写失败可见；见 RISK-JN-02/04、RISK-BOOT-10。
- Checkpoint（本次审查）：预擦失败不再记 `pre_erased_block`、完成服务 waiting 守卫、
  提交状态传播、find_latest 失败复位、水位公式精确到条目；见 RISK-CP-01~03。
- 磨损计数 / 静态 WL（本次审查）：WL 收尾改 FREE、擦除清零 read_count、跳过含 BAD
  plane 的候选、磨损表一致性校验；见 RISK-WL-01~04。
- Read Recovery（本次审查）：恢复成功按聚合位报 OK、health 单调升级、提交硬失败
  立即 abort、预测巡检跳过含 BAD plane 的 SB；见 RISK-RR-02~04。
- FTL↔FC 接口（本次审查）：CW 填装位图 plane 盲修复、sync_wait 迟到完成回收、
  位图 release 拒绝、同步原语检查搬运结果；见 RISK-IF-01~04、RISK-TYPE-01。
- Host 提交 / 完成组 / pending（本次审查）：读成败判据改全集、短链读报错、TRIM
  Journal-first、owners 结算顺序统一、两个 submit 收敛为公共骨架；见
  RISK-HOST-01/03/04、RISK-GC-03。**RISK-HOST-01 必须单独取证**（造 ECC 不可纠读）。
  ⚠ 本轮删了 `pending_add` / `pending_group_free`（零生产调用），UT 重构时改用
  `pending_alloc` + 就地填槽 + `pending_validate` 的生产写法。
- 完成路由与 op dispatch（本次审查）：旁表按 `command_id` 去重、容量 32→64、
  `completion_table_route` 注释与代码对齐、dim 分解收敛、9 个服务函数按 phase 拆 handler；
  见 RISK-CR-01/02/03、RISK-IF-05。
  ⚠ **RISK-CR-02 必须单独上板**（坐标 = 编译期无声的三类量之一），跑读写 + `map <lba>`。
  ⚠ RISK-CR-01 的泄漏路径不可直接构造，只做静态收口 + `LOGW` 可见化。
- 页打包写缓冲 wbuf（本次审查）：`ftl.c` §4 抽成 `wbuf.c`/`wbuf.h`、取页后失败归还页对
  （`allocator_rewind_pair`）、TRIM/复位的不变量收进 `wbuf_handle(DROP)`/`wbuf_handle(RESET)`、
  位运算收敛到 `ftl_cw_bit`、flush 日志降 `LOGD`；见 RISK-WB-01~03。
  ⚠ **RISK-WB-01 必须上板**（`next_page` = 编译期无声的地址/容量类量）；背压路径不易构造，
  只做静态收口，**不宣称已取证**。`trim` 走新 `wbuf_handle(DROP)` 路径，**必测**。
- FTL service 与后台调度器（本次审查）：接线从 `ftl.c` 抽成 `bg_wiring.c`、任务表改
  表驱动 + `_Static_assert` 容量门、`bg_scheduler_t` 删死字段、档位配置由引擎发布、
  `journal_handle(ERASE_ONLY)` 收回判据、RR 巡检节拍门补哨兵；见 RISK-BG-01/04/05/06/07。
  ⚠ 接线搬移属**地址/表序类量**，上板须确认 IDLE/NORMAL/BUSY 三档任务执行序与条数不变
  （BUSY 档 l2p flush 条数 = 1）；`bg_scheduler` 的时钟缺失分支在本板不可达（RISK-BG-03）。
- 缺陷收口批次（2026-10-19）：坏块/持续失败策略（MD-01、GC-01/02、JN-01）、
  等待上限（MIG-01）、启动恢复（BOOT-06/07/08/09/11）、L2P 降级（L2P-01）、
  逐 plane 记账（HOST-02）、组在飞闸门（WB-04）、BBT 用户区抽样（BBT-05）；
  详见 §2K。**全部未上板**。
- 命名重构（2026-10-19）：异步 job 类型/字段统一为 `<module>_handle_t` /
  `handle`，完成服务函数统一为 `ftl_*_callback`，`g_op_service` → `g_op_callbacks`。
  纯改名、无行为变化；测试源未同步（R4），`make tests UT=FTL` 的旧符号错误增加
  （见 RISK-MD-03 / RISK-BG-09）。
- 板端最小回归：格式化启动 → 顺序写满多个 SB → GC → 并发覆盖写/TRIM → 断电重启 → WL →
  Journal/BBT 故障注入 → 格式化中途断电 → 长时 soak 后全量读回。

---

## 已闭合

（暂无。闭合时在此记录 ID、提交、验证证据。）

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
> `待上板`（代码已改，板端未验）· `待测试源`（冻结 UT 待所有者同步）·
> `待定位` / `观察中`（已复现或待归因）· `部分已上板`（正常路径已验、残余未验）。
>
> ⚠ **本表只保留仍需动作的风险**：代码已修、已验证、信息/已议判定、已闭合条目全部移到
> `docs/archive/known_risks_batches.md`（2026-10-24 首轮收敛，后续批次见 §2026-10-30 / §2026-10-31），
> 并在文末「归档索引」保留 ID + 状态。`已修未上板` 类条目中的注入负例仍在归档行内，
> 重访时按行内「重访触发」执行。
> （首轮收敛提交：`aee360f`，2026-10-24；2026-10-31 批次见 archive 同节。）

---

## 1. metadata 模块（提交 `992985c`）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-MD-06 | 已修未上板（**未被本次冷启动覆盖**） | **回收旧日志块不等 Anchor 落盘**：普通 L2P flush 在 `commit_region` 里就登记回收候选，而新位置只在 RAM Directory；`MD_JOB_RECLAIM` 原来只检查 live/current/LOG 就擦。掉电后 on-media Anchor 仍指向**已擦块** ⇒ 重启 L2P region 读失败（修前上板 `region 0 load failed 3 times`）。 | 修法：① `MD_JOB_RECLAIM` 擦前要求 `dir->dirty == false`；脏则先走 `CP_ANCHOR_*` 写 Anchor（`anchor_for_reclaim`），成功后回 RECLAIM。② `dirty_gen/anchor_gen` 防止 Anchor 写入期间的新 commit 被误标为已覆盖。③ Anchor 落盘成功后才 `md_reclaim_mark_all`。 | 上板复现必须**释放一个元数据日志块**：只对 LBA 0 写两次 + `fl` 只是往同一个 64 页块追加 2 页，`anchor_seq` 不变（已实测 rev25：`anchor_seq=1 cp_seq=1`，**未触发**本路径）。需写满 64 页（64 个不同 region，LBA = r×512）再整体改写使其轮转；或加 debug 测试钩子。验证判据：`anchor_seq > cp_seq`、冷启动无 `[L2P] load failed` | `src/ftl/metadata_job.c`（IDLE/RECLAIM/CP_ANCHOR）；`src/ftl/metadata.c`（dirty_gen/anchor_gen）；rev25 实测：format→POR→`wr 0 1`+`fl`×2→POR，`boot type=2 journal_entries=2`、无 L2P 错误但 `anchor_seq` 未增 |

---

## 2. GC / migrate / journal（提交 `fdc7c7f`）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-PERF-01 | 待后续 | `gc_should_trigger` 与 `gc_select_victim` 各扫一次全表；`wl_should_trigger` 与 `wl_select_victim` 也各自扫描 | GC 10 ms / WL 5 s 节拍门兜成本 | 需要时把 `alc_bst_erase_stats_t` 从求值点透传（注意别把策略耦合进 mig API） | `src/ftl/gc.c` / `src/ftl/wear_leveling.c` |
| RISK-PERF-03 | 信息（2026-10-25 板端实测） | **cache read 生效后读速没有提升**：`/speed/ftl_rw` cache 档 `rd=1363 KB/s` vs normal 档 `1356 KB/s`（几乎相同），尽管 `rdc batches=1600`、`pairs>0` 表明 4-pair 序列确实在跑。说明当前瓶颈不在命令/启动开销，而在数据面（PIO 逐字节搬运 + 校验）或单 LUN 串行，cache 序列省下的命令开销被掩盖 | 功能正确性不受影响；cache 读仍是有效路径（掉电/兼容性无变化） | 要拿到收益需先定位数据面：① 统计单 pair 的 `tR` 与 PIO 拷贝耗时占比；② 评估 DMA/更大 FIFO 或提高 FC 批处理深度。与 RISK-PERF-02 的命令形式优化一并评估 | 板端 2026-10-25 debug：cache `rd_ms=18776` vs normal `18873`；release -O2（00:35 镜像）：cache `rd_ms=7598` vs normal `7598`（`rd_normal=7561`）——提速后两者仍相同；H2testw（2026-10-25）`cr=25931/102794/681/0` 证明 host 顺序读已走 cache read，但吞吐仍受 CPU 数据面限制；`src/platform/stm32h743/nand_dma.c`（PIO 路径） |
| RISK-PERF-04 | 信息（2026-10-25 release -O2 实测） | **内部数据面是 CPU-bound，NAND 阵列利用率极低**：2-plane 页 4096 B，按 datasheet（tR=25µs / tprog=200µs）阵列上限 ≈163.8 MB/s 读、20.5 MB/s 写。**接口上限也远未到**：x16 @3.3V Mode 5 的 tWC/tRC=20 ns ⇒ 2 B/20 ns = **100 MB/s** 总线（1.8V 25 ns ⇒ 80 MB/s）。按 datasheet 组合：2-plane pair（4096 B 主区）非流水读 25+42=67 µs ⇒ 61 MB/s，cache 流水读 ≈97 MB/s，cache program 稳态 4096 B/200 µs ⇒ 20.5 MB/s。release 实测：FC 读 3533–3591 KB/s（≈3.6 MB/s，阵列 6%）、FC 写 3670–4319 KB/s（≈4.0 MB/s，约 20%）、FTL host 读 3369 KB/s（5.7%）、FTL host 写 2499–2687 KB/s（12–13%）。debug→release 全线提速 2.2–2.5× ⇒ 瓶颈在 CPU 侧（PIO 拷贝 + randomizer/CRC + BCH），不是 NAND，也不是总线 | 无（功能正确性不受影响） | 按收益排序：① PIO → DMA（FC data path）；② BCH/randomizer 硬件化或向量化；③ 减少逐字节 copy。先用单 pair 拆 `tR` 与 CPU 段耗时验证 | 板端 2026-10-25 release 00:35 镜像 `/speed/` 4/4；H2testw 415/423 MB（**cache program + cache read 均开**）：写 843 KB/s、读 889 KB/s（USB FS 12 Mbit/s 的 56%/59%，FTL 内部的 31%/26%）；无 cache program 基线 765 KB/s（写 +10%），cache read 对读速无增益（884→889，RISK-PERF-03）；`src/platform/stm32h743/nand_dma.c`；MT29F4G08 datasheet tR/tprog/tWC/tRC。**2026-10-31 v1.06 复测**（读写序列跨 block、padding 修复后）：写 **894 KB/s**、读 **888 KB/s**（USB FS 12 Mbit/s 的 59%/59%）——序列优化不改变带宽（收益在 START 数与设备侧延迟），吞吐上限仍由 USB FS 决定，要提高需 USB HS/UASP 或主机侧调度 |
| RISK-PERF-02 | 待后续 | GC/WL 目的页仍逐 plane 单条写，没有 multi-plane 命令 | 正确性无影响 | 命令形式优化，需 FC 侧支持 | `docs/design/ftl_design.md §7.2` |
| RISK-GC-05 | 待后续 | GC Bitmap（Layer 2）未实现：掉电后 GC 从块头重来 | 正确性由 F10 + 重试覆盖 | 性能优化项 | `docs/design/ftl_design.md §7.2` |
| RISK-GC-06 | 部分已上板（2026-10-25：gc_trigger/select/decode/progress 已过；WL 5 s 节拍未直接断言） | 本轮行为变化未板验：WL 5 s 节拍、CLOSED plane 门控、`valid_count` 单块上界、`gc_decode_cw_lba` 三态（T5 已于 2026-10-23 移出 GC） | 编译零告警 | 按 §2 板端清单回归（GC 并发覆盖写/TRIM、断电、WL、fault injection） | 提交 `fdc7c7f` |
| RISK-JN-03 | 待上板 | `write_fail_pending` + CP 清账逻辑未做故障注入：页写失败 / 擦失败 / CP 早于失败点三种时序未实测 | 静态路径已审查 | 注入 Journal 页写失败，确认 victim 未擦、CP 后恢复 | `src/ftl/journal.c` / `src/ftl/checkpoint.c` |
| RISK-MIG-03 | 已修；dangling 路径已上板观测（rev32），TRIM/release 回绕未单独验（**原二级崩溃**） | `bbt_mark_bad()` 清 `valid_count` 后，L2P 仍可能指向该块（predictive 读失败标坏 / full metadata rebuild 漏认）。写确认与 TRIM 直接调 `ftl_block_sub_valid()` ⇒ debug 撞 `valid_count > 0U` 断言（上板 rev27：`dangling lba=9 old=00025811 g=300 st=BAD ...` 后 `assertion failed bst.h:285`），release 则 uint16 回绕成 65535，虚高计数污染 GC/WL | 修法：`bst.h` 新增 `ftl_block_sub_valid_if_live()`（BAD 或 count==0 跳过）；写确认、TRIM、migrate 源块减计数统一用它 | ✅ rev32 上板已观测：308/2493 被 predict 标 BAD 后，host 覆盖写 LBA 440–447 只打 `[FTL] dangling lba=… g=308/2493 st=BAD … newg=313/4089 nst=OPEN`，**无断言、无回绕**，新映射正常落到 OPEN 块。待补：TRIM 同一 LBA；release 档回绕负例 | `include/ftl/bst.h` `ftl_block_sub_valid_if_live`；`src/ftl/ftl.c`（写确认 / `ftl_trim`）；`src/ftl/migrate.c`；rev27 崩溃日志 + rev32 dangling 日志 |
| RISK-MIG-07 | 已复现（2026-10-21 夜 rev33），根因待定位 | **大批块在写阶段被 GC 读失败退役 ⇒ L2P 悬空到 BAD 块 ⇒ 校验阶段 host 读永久失败**：H2testw 写入 428 MB 期间 GC 累计 `mark BAD` 127 个 gblock（`fc_err`，page 8..61 均有）；`[FTL] dangling lba=208 ... st=BAD` 证明 L2P 仍指向已退役块。校验到 107 MB 时文件系统元数据 LBA 3 落在 gblock 356（已 BAD）⇒ RR 8 次重试失败 ⇒ 每次 host 重读都失败 ⇒ Code 21「设备未就绪」。**该 LBA 数据已不可恢复**（介质/读路径问题，不是映射崩溃）；写阶段本身全容量通过 | ① RR 对**已 BAD 块**快速失败（不再 8 次重试、不再重复 `bbt_mark_bad`）⇒ 校验速度与日志同时改善；② `read unrecoverable` 5 s 限流 + suppressed；③ MIG abort 日志增加原始 `fc=0x%08x` 状态位，用于区分 ECC 不可纠 / NAND 失败 / META 不符；④ 写阶段 GC 读失败率高仍是**开放问题** | 上板：format 后复跑，观察 `mark BAD` 速率是否仍 >100/次；看 `fc=` 位判断 UNC 占比；若以 UNC 为主 ⇒ 介质磨损，换片；若以 NAND/META 为主 ⇒ 查 GC 读地址/描述符链 | `src/ftl/read_recovery.c` `rr_settle_plane` / `rr_job_step`；`src/ftl/migrate.c` `mig_read_complete` / `mig_job_abort`；板端 `tmpl.log` 2026-10-21（2887 行）；**2026-10-21 夜 rev33 复现**（407 MiB 盘、第二个 279 MB 分区）：写入到 `free=1178→32` 后 GC 启动，156→**420** BAD（+264，全部 `fc_err`=UNC）、`dng=55`、`op=204/204→11/0`、`spare=50/50→0/29`，`wr` 在 53120 处冻结、USB 反复重挂载；悬空 LBA 从 262346 起（第二分区起点） |
| RISK-MIG-08 | 待定位（2026-10-23） | **H2testw 全盘通过但 `[FTL] dangling` 指向 `st=CLOSED` 且 `valid_count==0` 的块**：日志出现大量 `dangling lba=... g=1777 st=CLOSED sb=1463 newg=... nst=OPEN/CLOSED supp=...`；L2P 仍指向该块，但块计数为 0 ⇒ GC 把它当空块回收，`free` 起伏但没有 `[MIG ] move`，`mig=0/0`。H2testw 只校验**本次新写的数据**，不证明旧映射/GC 迁移一致；若该块仍有 live CW，后续擦除会造成旧数据/映射静默丢失 | 新增 `diag mv/nop` 区分真实搬移与空块回收；`addr g <g>` / `map <lba>` 可现场核对 `valid`；当前未修，先保留证据 | 下次启动后若 `dng>0` 或 `free` 在无 `[MIG]` 时跳升：① `map` 一个 dangling LBA，确认 `valid==0`；② 检查 CP 快照的 block valid 与 L2P region flush 是否同一 cut；③ format 后只做一次 H2testw 复现并比较 `[BLK] restore`/`diag dng`；④ 若 `dng==0` 但 `mv==0`，再查 GC 选块/有效计数 | 原 `RISK-MD-10` 重开；`src/ftl/ftl.c` `ftl_confirm_write` 的 dangling 守卫；`src/ftl/power_failure_recovery.c` `pfr_restore_blocks` / `pfr_probe_blocks`；`src/ftl/checkpoint.c` `cp_job_begin` / `cp_block_snapshot`；板端 2026-10-23 23:49:55 H2testw 日志 |
| RISK-GC-14 | 待上板（2026-10-23 阈值重基线） | **收益判据边界**：no-gain 结论依赖 `total_writes/free` 快照；host 写流用 2 s 窗口，极慢写会误判 idle；写路径 NO_SPACE 最终表现为 IO_ERROR | 阈值/退避已按 `FREE_POOL_TARGET=32` 基线；no-gain 只在真实搬移/回收后清；任何 `total_writes` 变化会重扫 | 上板复测满盘只读/覆盖写/满盘写；若误报 NO_SPACE，再调阈值/混合策略 | `src/ftl/gc.c` `gc_select_victim`；`src/ftl/migrate.c` `mig_gc_mode_select` / `mig_gc_scan_is_allowed` / `mig_gc_nogain_note` |

---

## 2A. BBT（Bad Block Table）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-BBT-01 | 部分已上板（2026-10-25：bbt_mark_bad/bbt_persist clean-pool 已过；写飞行中再标坏未验） | `bbt_mark_bad` 改为幂等 + `dirty_gen` 后，运行时标坏 / 重启持久化 / Spare 只减一次未实测 | 代码审查 + 编译零告警 | 上板：注入擦失败/RR 标坏，重启后 `bad_count`/`spare_count`/BBT 位一致；BBT 写飞行期间再标坏，下一次 save 必须包含新块 | `src/ftl/bbt.c` `bbt_mark_bad` / `bbt_write_submit` / `ftl_bbt_callback` |
| RISK-BBT-02 | 待上板 | `ftl_sync_read_column` 现在对 NAND 错误返回 `FTL_ERROR`；工厂扫描对“读失败页”的判坏行为未实测 | BBT 侧 `!= FTL_OK ⇒ 坏块` 保守路径 | 注入列读失败，确认判坏而不是误判好块；注意空片出厂标记与 NAND 失败的区别 | `src/ftl/ftl_fc_interface.c` `ftl_sync_read_column` |
| RISK-BBT-04 | 待后续 | BBT header **无布局版本字段**；只靠 `total_blocks` 做代理。块数相同但 bitmap 语义变化的格式演进无法识别 | `bbt_header_valid` 校验 `total_blocks == FTL_BLOCK_TOTAL` | 下次 BBT 格式变更时用 `reserved[]` 加版本（属于介质格式变更，需 bump `FTL_MEDIA_LAYOUT_VERSION`） | `include/ftl/bbt.h` `bbt_header_t` |
| RISK-BBT-06 | 待后续 | `ftl_bbt_callback` 没有保存 in-flight `command_id`；擦完成与写完成若被重复投递，phase 可能误配 | `waiting==0` 的孤儿守卫只能挡 job 结束后的迟到完成 | **2026-10-19 核实**：FC 侧 `fc_complete_internal`（`fc.c:712-724`）**没有**「已 COMPLETED 就不重复入队」的守卫 ⇒ 是否重复完成完全取决于 NAND 层是否重复上报。当前 NAND 是单次轮询完成模型，无重复上报源 ⇒ 暂不修；换 NAND 引擎 / 加中断完成时**必须先补该守卫** | `src/ftl/bbt.c` `ftl_bbt_callback`；`src/fc/fc.c` `fc_complete_internal` |
| RISK-BBT-18 | 待定位（2026-10-24 板端 UT 观测） | **多圈 BBT ring 上新写 record 后 reload 选到上一条**：介质历史 seq=1383、write_idx=103；`bbt_save` 写 seq=1384 到 idx=103 且返回 OK、dirty 清 0；随后 `bbt_load`/`bbt_init` 返回 **seq=1383、count=0**，新标坏块不在 bitmap。干净 BBT 池下 save→load 正常（seq=1），故持久化写入本身 OK；问题在 `md_ring_locate` 对“ring 已写满多圈 + 当前块半写”的定位（游标块末尾可能来自上一圈，“降序找最后一条有效”会选中旧 record）。后果：重启可能加载旧 BBT，最近运行期标坏的块丢失，坏块可能再次被分配 | clean-pool UT 覆盖基本持久化；运行期每次标坏仍会再写新 record；未观察到数据错误。**2026-10-24 21:29 第二次独立复现**：`format_in_progress_resume` 置 FORMAT_IN_PROGRESS 后 `bbt_save`，重启后标志读不回来（新 record 被旧 record 盖过）；用“更大 sequence 的新 record”可胜出 ⇒ 与“选旧 record”一致 | 构造 >2 圈 BBT 写入后 save/reload 复现；检查 `md_ring_locate` 的块末探针与游标块扫描，改为按 sequence 最大值而非“最后一个有效”定位；修后 clean/多圈两条 UT 都要过 | 板端 2026-10-24 `test /ftl/media/e2e/bbt_persist` 日志（seq1383→1384，reload 1383）；**同日 21:29** `format_in_progress_resume` 重启后 `alc_bbt_is_format_in_progress` 为 false（save 的 flag record 未被 reload 选中）；`src/ftl/metadata.c` `md_ring_locate`；`src/ftl/bbt.c` `bbt_save`/`bbt_load`；`tests/unit/test_ftl_media.c` `test_format_in_progress_resume`（sequence 抬升法） |
| RISK-BBT-10 | 部分覆盖（2026-10-25：`FORMAT_IN_PROGRESS` 恢复已由 UT 覆盖；factory-scan/format_erase 未做） | BBT 的 `mark_bad` 幂等/Spare 一次、元数据块丢失标记、clean-pool save→load 已由 `pure/mem/bbt_*` 与 `media/e2e/bbt_persist` 覆盖；`kick/scan_factory/format_erase` 仍未单测；`FORMAT_IN_PROGRESS` 恢复已由 `format_in_progress_resume` 覆盖（2026-10-25） | 用例已入库；多圈 ring 定位问题见 RISK-BBT-18 | 补 factory-scan / format-in-progress 注入用例 | `tests/unit/test_ftl_pure.c`、`tests/unit/test_ftl_media.c`；`docs/design/ftl_ut_design.md` §4.1 |

---

## 2B. 掉电恢复 / 启动路径

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-BOOT-02 | UT-P 已上板（2026-10-25：`md_abort_keeps_victim` RAM 语义）；物理注入未做 | ~~carve 先于 full metadata rebuild ⇒ 写过块同时是池成员与 CLOSED 条目 ⇒ 活数据可被复用~~ | 三条路径统一 **full metadata rebuild / 快照 → carve**；`pfr_boot_carve_pools` 只把 `state == NONE` 的块入池，其余只计入 `total_blocks` | 上板：预置数据在旧 carve 会选中的块上，走 full metadata rebuild fallback 后验证不在池 | `src/ftl/power_failure_recovery.c` |
| RISK-BOOT-03 | 部分已上板（2026-10-24：IO_ERROR 分支已由 UT 覆盖） | ~~CP 读失败被当作“需要格式化” ⇒ 一次瞬时读错整片擦除~~ | `cp_media_status_probe` 区分 `CURRENT / NO_RECORD / VERSION_MISMATCH / IO_ERROR`；`pfr_media_check_format` 对 IO_ERROR 返回 `FTL_ERROR`，`main` 拒绝格式化并停止启动。UT `cp_corrupt_refuse_format` 已上板：擦 CP 池 + 写非 FF 页 ⇒ IO_ERROR、拒绝格式化 | 版本不符（VERSION_MISMATCH）分支仍未单测；真读失败注入仍无 seam | `src/ftl/checkpoint.c`；`src/main.c`；`tests/unit/test_ftl_media.c` `test_cp_corrupt_refuse_format` |
| RISK-BOOT-04 | 部分上板（2026-10-25：`format_in_progress_resume` 覆盖“save 后”断电点；erase 中/CP marker 前未做） | 两阶段格式化中途掉电后，重启不能判断“格式化未完成” | `FORMAT_IN_PROGRESS` 写入 `bbt_header_t.reserved[0]`，`bbt_load` 恢复；`cp_media_status_probe` 见该标志返回 NO_RECORD ⇒ 继续格式化 | 上板：save 后 / erase 中 / CP marker 前各断电一次 | `src/ftl/bbt.c`；`src/ftl/checkpoint.c` |
| RISK-BOOT-05 | 待上板 | 快照/恢复路径的校验与计数改动未板验：restore Pass 0 全量校验（不再部分应用 BAD）、`recovery_materialize_pba` 校验 ch/ce/lun、`recovery_add_closed` 传播列表满、`valid_cw` 改 `uint32_t` | 编译零告警；失败路径回退 full metadata rebuild | 上板：快照中途注入非法 word（不留下部分 BAD）；journal 注入越界 PBA（回退 full metadata rebuild）；满盘重建确认 `valid_cw` 不截断 | `src/ftl/power_failure_recovery.c` |
| RISK-BOOT-13 | 观察中（2026-10-24：POR 后无记录，判定为旧镜像残留；原始 format 后写请求挂死仍未定位） | `format` 完成后未按提示 `POWER-CYCLE NOW` 就下发 `wr`：曾出现 CLI 请求永久 `busy`；同一会话的 `.noinit` 记录显示 **HardFault** `PC=0x2400038c LR=0x24000b09 CFSR=IACCVIOL HFSR=FORCED`（按 rev25 解析落在 CLI 16 KB `g_wr_buf` 内，属执行流跳进非可执行 RAM）。**但一次 POR 后 `previous crash` 行消失，且 format→POR→wr/fl 正常** ⇒ 该记录至少可能是 rev24/旧会话的陈旧记录，不能归因于本轮 FTL 改动。另：format 后未重启的 `wr` 本次返回 `status=-3`，未再挂死 | `format` 已打印 `POWER-CYCLE NOW`；未做运行期拒绝 | ① 继续在 rev25 上做正常读写/断电回归；② 若 `previous crash` 再次出现：立即 `crash` + 记录命令序列，按**当次镜像**符号表定位；③ 考虑让 format 后置 FTL 为不可提交状态直到重启 | rev25 日志 2026-10-20：首次 `.noinit` 有 HardFault；**POR 后无该行**；`arm-none-eabi-nm` `g_wr_buf=0x240002dc`。**2026-10-24 追加**：21:09/21:14 镜像多次软复位后仍报同签名 MemManage（PC=0x2400038c LR=0x24000b09 CFSR=0x00000001），但 **21:14 镜像真实断电（`reset reason: POR PIN BOR`）后启动无 `previous crash` 行** ⇒ 该记录是更早会话/镜像的 `.noinit` 残留，不是当前镜像的活缺陷。只有 **POR 后仍出现** 才按新崩溃重开 |
| RISK-BOOT-23 | 已解释（观察中，2026-10-30 两次上板对照） | **非正常复位后最新 CP record 双 plane 读失败 ⇒ 全量 metadata rebuild**：`[CP] find latest seq=17 idx=2` → `[BLK] read failed plane=0/1 page=0 rec=2` → `unavailable (rec=2 seq=17)` → `fallback to full metadata rebuild (power failure, blocks_valid=0 applied=0 ctx_ok=0 replay_ok=1)`；重建成功（`blocks=60 valid_cw=13283`），`bad areas cp=0`（该 CP 块未标坏）。若**每次** dirty reset 都复现，说明最新 CP 记录的坏页退避不足（下次 boot 最新 CP 仍指向同一坏页），代价只是启动慢，无数据丢失。**2026-10-30 对照**：dirty reset（前一次写/GC 后）⇒ 重建一次；随后 format 后的 PIN reset ⇒ `boot type=1 restored`、无重建 ⇒ 前次重建由 dirty metadata 触发，符合设计预期；受控掉电注入仍未做。 | Journal 回放成功（`journal_entries=41 replay_ok=1`）；全量重建成功；同次 H2testw 417 MB 0 错误。 | **重访**：下次干净断电上电看是否仍有 `[BLK] read failed ... rec=` / `fallback`；只在 dirty reset 后出现 ⇒ 预期接受并记录；干净上电也复现 ⇒ 查 boot 读 CP 失败时是否应标坏该块并切换 CP 块 | `src/ftl/checkpoint.c` `find latest`；`src/ftl/power_failure_recovery.c:572/1012`；板端 2026-10-30 启动日志 |
| RISK-MD-16 | 待定位（2026-10-22 板端） | **victim 上的 region 页读失败 ⇒ 压缩每轮在同一页中止，victim 永不回收**：板端 GC 期间反复出现 `[META] md job abort phase=2 victim=46 moved=10 (victim 不擦)`；`phase=2 = MD_JOB_CP_READ`，且**没有**伴随 `submit stalled` ⇒ 是读命令完成状态带错误位（若为提交超时则属瞬态）。每次 `victim=46/moved=10` 完全相同 ⇒ 确定性坏页，不是瞬时背压。后果：该元数据日志块无法搬空、live 水位偏高、压缩反复白做；**无数据丢失**（abort 不擦 victim，未搬 region 仍指向它，已搬的指向新页） | abort 语义本身安全（两边都有效）；日志已区分瞬态超时（debug）与真异常（限流 LOGE） | ① 先在 abort 行打出完成状态位（`fc=`）以区分 UNC/CRC/META；② L2 双副本下若 victim 上那份不可读而另一份有效 ⇒ 压缩**跳过该副本**（保留 Directory 的另一份），不要中止整轮；③ 两份都不可读 ⇒ 按 RISK-BOOT-18 降级该 region | `src/ftl/metadata_job.c` `md_handle_abort` / `md_cp_read_complete`；`src/ftl/metadata.c` `md_compact_read`；板端 2026-10-22 GC 日志 |
| RISK-MD-17 | 待决策（2026-10-24 审查发现） | **status.error 看不到整理 job 的最近一次中止**：`md_handle_abort` 把 `handle.error` 置为 `FTL_ERROR`，但 `md_handle_step` 的 IDLE 分支在**每次**进入 IDLE 时先写 `FTL_OK`；`md_handle` 是先 step 后 `md_status_fill`，abort（无论来自完成回调还是 step 内）后的下一次采样前 error 已被清回 `FTL_OK`。后果：host/诊断只看到 `error=0`，无法归因最近一次 abort；无数据损坏（abort 本身不擦 victim） | abort 仍会打 `[META] md job abort phase=...` 日志；`md_status_t` 的 `phase/victim/moved` 仍可观测 | 若 `status.error` 要作为对外归因：把清除点从「进入 IDLE」移到「真正启动新 job」（reclaim/anchor/compact 将要设置 phase 处），并补一条 UT/板端负例（构造 abort 后读 status）；否则在文档中明确该字段只反映“当前 job” | `src/ftl/metadata_job.c:708`（IDLE 清 error）/ `:282`（abort 置 error）/ `:992`（status_fill 读取） |
| RISK-BOOT-22 | 已复现（2026-10-24 板端；未修） | **格式化中断后可能拒绝格式化/启动**：两阶段格式化在「BBT 已落盘（带 FORMAT_IN_PROGRESS）、initial CP 尚未写」时掉电；重启时若 `bbt_init` 因 RISK-BBT-18 选中了**旧 BBT record**（不带标志），`cp_media_status_probe` 会把「有 BBT、无 CP」判成 `CP_MEDIA_IO_ERROR` ⇒ `pfr_media_check_format` 拒绝格式化，`boot failed, fallback to fc_service`。设备需人工 CLI `format` 恢复。后果是可用性问题，不是数据破坏（此时尚无用户数据承诺） | 无产品侧缓解。UT `format_in_progress_resume` 用“更大的 BBT sequence”让新 record 在 bootstrap 扫描中胜出，从而隔离该变量（不擦 BBT 载体） | 优先修 RISK-BBT-18（locate 按 sequence 最大值选）；修好后本用例可去掉 bootstrap 清池。重访：每次改 BBT/CP 池几何或 locate 都要在“中断格式化”介质上重启验证 | 板端 2026-10-24 21:32:27 启动：`[ERR] [BOOT] CP media unreadable — refuse to format` + `[ERR] [FTL] boot failed, fallback to fc_service`；`src/ftl/checkpoint.c` `cp_media_status_probe`；`src/ftl/bbt.c` `bbt_load` / `bbt_save`（sequence 只取候选池 max）；`tests/unit/test_ftl_media.c` `test_format_in_progress_resume` |
| RISK-BOOT-24 | 信息（有意设计，2026-10-31 上板示例） | **掉电恢复里的 `anomalies=N` 不是缺陷**：`pfr_probe_blocks` 按 CP 快照探测「池块（Free/OP/Spare）却有数据」——发生在 CP 快照之后才分配、且其 Journal 条目落在丢失尾部的块上。处理方式：把这些块转 CLOSED、按 L2P 全块重建 valid_count（`pfr_close_block`），并计入 `stats.anomalies`。因此 `[BLK] restore closed=X` 与随后 `info` 的 `closed=` 相差 N 是**正常对账**（X + anomalies = info closed） | 安全网本身；`dng=0`/`rrab=0` 是数据完整性的旁证 | **判据**：`anomalies` 可在任意掉电点非零；只要 `info` 的 `closed = restore closed + anomalies`、`dng=0`、无 `mapping lost` 即视为预期。若 `closed` 对不上或出现 dangling，按缺陷处理 | 2026-10-31 第二轮板端：`restore closed=3086 … anomalies=2` → `info closed=3088`；`src/ftl/power_failure_recovery.c` `pfr_probe_blocks` / `pfr_close_block`；`include/ftl/power_failure_recovery.h` `anomalies` 注释 |
| RISK-BOOT-17 | 信息 / 待决策（2026-10-21，v0.86 阅读时发现） | **空片首启可能拒绝自动格式化**：`bbt_init` 在空片上执行出厂扫描后 `bbt_factory_scan` 会 `bbt_save` 写一条 BBT record；随后 `pfr_media_check_format → cp_media_status_probe` 在「CP 池全 FF」分支调用 `bbt_medium_has_ftl_data()`，该函数探到这条合法 BBT 页 ⇒ 返回 true ⇒ 判成 `CP_MEDIA_IO_ERROR` 并拒绝格式化。用过的介质不受影响（本来就会拒绝，需 CLI `format`）。**未在真空白片上实测**，结论来自代码路径阅读 | 无。空片可先 CLI `format`（绕过 `pfr_media_check_format`）。⚠ 不要据此把 `bbt_factory_scan` 的 `bbt_save` 当成 bug 直接删：两阶段格式化的前置 BAD 图依赖它（或由 `bbt_format_erase` 补落） | 下一步（需真空白片或擦净介质）：确认是否真会拒绝；若是，方案 A = 出厂扫描只置 dirty 不 save，交给 `bbt_format_erase` 首次 save；方案 B = `cp_media_status_probe` 区分「本次 boot 刚扫出的 BBT」。在此之前不修改 | `src/ftl/bbt.c` `bbt_factory_scan` / `bbt_medium_has_ftl_data`；`src/ftl/checkpoint.c` `cp_media_status_probe` |

---

## 2C. L2P cache

> 本节条目已归档（见 `docs/archive/known_risks_batches.md` §2026-10-30）：RISK-L2P-08（已闭合 2026-10-30，ITCM 野写的根因与写者均定位）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-L2P-03 | 待上板 | 本轮行为变化未板验：`flush_kick` 背压改报 `FTL_AGAIN`、写回前资源预检、CLOCK 引用位置 1、加载失败降级为全未映射 | 编译零告警 | 上板：命令池压力下 `fl`/Checkpoint 不中止且不烧日志槽；热点 region 不被长期钉住；坏 region 读不活锁 | `src/ftl/l2p_cache.c` |
| RISK-L2P-05 | 待上板（v0.90 双副本的时序残余） | **备副本在完成回调里提交，此时刚完成的主命令尚未 `fc_command_release`**：命令池恰好吃紧时 `ftl_async_write_page` 失败 ⇒ 整次 flush 判失败（`flush_fail`）、已取的两槽作废、行回 DIRTY 重试。后果：高压力下 flush 假失败 + 日志槽浪费；**无数据破坏**（旧 pair 仍是 Directory 真值） | 失败不提交、行保持 DIRTY、下轮重试；槽位作废是既有语义（与单副本相同）。`l2p_flush_resources_available()` 只在 kick 时预检一次（够一条命令） | 上板观察：`L2P flush` 失败日志 / 日志块换块率异常升高时，改为①在回调里先 release 主命令再提交备份，或②kick 时一次性建好两条命令；二者都需核对命令生命周期（`src/ftl/l2p_cache.c` `ftl_l2p_callback` FLUSHING 分支） | `src/ftl/l2p_cache.c` `l2p_flush_kick` / `ftl_l2p_callback`；`include/ftl/l2p_cache.h` `L2P_FLUSH_MAX_INFLIGHT` |
| RISK-L2P-06 | 部分已改未上板（2026-10-22：miss 语义已对齐；越界分支待后续） | **批量读 API 的 miss 语义已修**：`l2p_entries_read` 原先在行未驻留时只返 `FTL_AGAIN`，既不 `miss_count++` 也不 `l2p_load_start` ⇒ 未来调用点若按头文件「语义与逐条一致」直接调用会永久停泊。**残余**：越界分支返 `FTL_ERROR` 后被 `ftl_confirm_write` 压成 `FTL_AGAIN`，同样是永久重试（当前 `FTL_LBA_MAX` 是 8 的整数倍，越界不可达） | `l2p_entries_read` 的 miss 已与 `l2p_read` 对齐（记 miss + 起加载）；唯一调用点仍有 `l2p_is_ready` 前置；页内 8 LBA 同 region 由编译期 `_Static_assert` 保证；越界当前不可达 | 新增调用点 / 改 ready 语义时复核越界分支：要么改成显式错误传播并让 host 报 IO_ERROR，要么在 pending 入口 `ASSERT` 坐标合法 | `src/ftl/l2p_cache.c` `l2p_entries_read`；`src/ftl/ftl.c` `ftl_confirm_write` |

---

## 2D. Checkpoint

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-CP-03 | 待上板 | 本轮状态机改动未板验：提交状态传播（FTL_ERROR 立即中止）、`find_latest` 失败复位游标、水位公式精确到条目 | 编译零告警 | 正常 CP、各 phase 掉电、journal 回切、85%/95% 触发点 | `src/ftl/checkpoint.c` |
| RISK-CP-04 | 待定位；2026-10-30 H2testw 未复现（cp=1724 持续递增、无 `[CP] submit stalled` 行） | **持续 host 写流下 CP 提交饥饿**：H2testw 期间反复出现 `[CP] submit stalled 25ms — abort phase=1`。CP 用 `ftl_op_submit` 申请 FC 命令，host 写可占满 `FC_COMMAND_POOL_DEPTH=64`（`FTL_PENDING_MAX=32` 个写各占 2 条命令）⇒ CP 连续 10ms（`FTL_SUBMIT_TIMEOUT_MS`）申请不到槽位即中止重试。后果：checkpoint 长期不推进、journal 环增长；环满后 host 写被 Backpressure 拒绝，表现为写停顿（无静默损坏）。当前无法区分它是 GC 活锁的**症状**还是独立缺陷。2026-10-21 复现时日志刷屏，已加 abort 日志限流（见 RISK-CP-05），但饥饿本身未修 | 中止后可重试；journal 环满是安全背压，不会越界写；abort 日志 1s/实例。**2026-10-30 增补**：cache 序列打开期间 CP 的提交 deadline 由 `ftl_op_cache_seq_active()` 挂起（RISK-IF-13），避免门控等待触发本 abort | ① 先修 RISK-GC-07 后复跑 H2testw，观察 CP 是否仍持续 abort；② 若仍复现：给元数据/CP 预留命令槽，或让 CP 的 submit deadline 长于 host 写突发；③ 上板确认 journal 水位与 CP 成功率 | `src/ftl/checkpoint.c` `cp_job_stall` / `cp_command_result_handle`；`src/ftl/ftl_op.c` `ftl_op_submit`；`include/fc/fc.h` `FC_COMMAND_POOL_DEPTH`；串口 rev30/2026-10-21 日志 |

---

## 2E. 磨损计数 / 静态 WL

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-WL-04 | 待上板 | 本轮行为变化未板验：WL 收尾 FREE、read_count 复位、BAD plane 过滤、磨损表 `record_pages` 校验 | 编译零告警 | 正常 WL/GC、断电、长时 soak | `src/ftl/migrate.c` / `allocator.c` / `wear_leveling.c` |
| RISK-WL-05 | 待后续 | `wl_should_trigger` 与 `wl_select_victim` 各做一次全表扫描（RISK-PERF-01 的一部分）；RETIRED 块仍计入 erase stats | 5 s 节拍门 | 需要时把 `alc_bst_erase_stats_t` 从触发点透传 | `src/ftl/wear_leveling.c` |

---

## 2F. FTL↔FC 接口

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-IF-04 | 部分上板（2026-10-30：全盘写/校验 0 错误；GC 搬迁 41.4 万页 ⇒ plane1 迁移与背压回退已走量）；wbuf 稀疏页 / 越段位图 release / 迟到完成负例未注入 | 本轮行为变化未板验：plane 1 迁移数据、wbuf 稀疏页、越段位图 release 拒绝、迟到完成路径 | 编译零告警（release 两条 unused 已消失） | 按 §5 板端清单回归 | `src/ftl/ftl_fc_interface.c` / `ftl_op.c` |
| RISK-IF-06 | 待决策（2026-10-30 读码判定） | **FC chain 派发路径不处理 RAID stripe encode**：`fc_poll_dispatch_chain` 没有单命令路径的 `#if CONFIG_FC_RAID` 分支。若 FTL 把写命令标成 `FC_CHAIN_MORE` 且 `raid=ENCODE`，整链写入会**静默不写 parity**（`CONFIG_FC_RAID=0` 时 ENCODE 分支不编译，不构成实际失效） | RAID 关闭（R5）⇒ `raid` 字段固定 NORMAL、ENCODE 分支不编译；FTL 当前不设置 `raid=ENCODE` | 启用 RAID，或 FTL 写路径开始设置 `raid=ENCODE` **之前**：在 chain 路径补 stripe 分支，或 `ASSERT(raid != FC_RAID_MODE_ENCODE)`；加 UT 负例 | `src/fc/fc.c:1491-1499`（single 的 RAID 分支）vs `src/fc/fc.c:1540-1593`（chain 无该分支）；`include/fc/fc.h:38-52` |
| RISK-IF-07 | 正常路径已上板（2026-10-30：5.2 万窗口、全盘 0 错误；跨 chunk 链正常）；混优先级 / wild-chain 负例未注入（代码守卫保留） | **一批 chain 违反契约会静默错乱或锁死 LUN**：① 混优先级 ⇒ `fc_submit_batch` 按 `high_priority` 把链拆到 SQ/HSQ 两段，`fc_poll_chain_length` 只沿单队列 peek，跳过的成员被当成不存在 ⇒ cache read 输出错页 / cache program 写错序列；② 末条 `MORE` 而没有后续 END ⇒ `chain_owner` 永不解锁，该 LUN 的另一队列被永久饿死 | `fc_submit_batch` 仍拒绝混优先级（debug `ASSERT` + release `INVALID_ARGUMENT`）；**末条 MORE 改为合法**（流式中间窗口），由 FTL 的 cache 序列状态机保证最终 END（`end_seq` = run 到头 / 物理断 / D4 / 尾部不足一对）、`ftl_op_submit*` 门控保证窗口间无插入；孤儿序列由 `ftl_cache_seq_idle_guard()`（1s）关闭。**写序列**用同一机制（首 49 / 中间 44 / 断点 48，窗口末条 MORE 跨 chunk 续链） | 上板：读/写流式窗口跨 chunk 的 END、D4 断点、wild-chain 负例（FTL 断点算错） | `src/fc/fc_interface.c` `fc_submit_batch`；`src/ftl/ftl_op.c` 门控；`src/ftl/cache_batch.c` `cache_read_submit`；`include/ftl/ftl.h` `ftl_cache_seq_t` |
| RISK-IF-08 | 已改、未上板（2026-10-30 状态字重构） | **page read 的完成聚合忽略 NAND 结果位**：`fc_complete_from_nand` 的 page-read 分支只 OR `desc->status`（数据路径），不 OR `nand_completion->cw_status`（NAND FAIL/TIMEOUT）。① NAND 读 FAIL 不以 `NAND_FAIL` 上报（通常数据路径也会置 UNC，错误可见但归因含糊）；② NAND 读 TIMEOUT 若 buffer 仍是上一次的有效数据且 `meta=FC_META_UNKNOWN`（GC/raw 读），ECC/CRC/META 都能过 ⇒ 命令被报成 READY，**静默返回陈旧数据**（§3.2） | 状态字重构已修：page-read 分支按 flat CW 把 `cw_status[]` 并进 desc（`FC_CW_STATUS_FAIL`）；命令级从 `pre_status`/`nand_status`/`execution_status` 映射 `NAND_FAIL`/`FAIL_CACHE`/`RDY`/`AREADY`/`WP`/`TIMEOUT`。TIMEOUT 现在一定进命令状态，FTL 的 `fc_cmd_status_has_error` 会拒绝陈旧数据 | 上板负例：注入 NAND 读超时并保留旧 buffer（GC/raw 读），确认命令状态带 `TIMEOUT`、FTL 不进成功路径 | `src/fc/fc.c` `fc_complete_from_nand` Step 3；`src/nand/nand_engine.c` `pre_jump_index`；`src/nand/nand_dispatch_tables.c` `completion_per_cw`；`include/fc/fc_command.h` `FC_CMD_STATUS_*` |
| RISK-IF-09 | 待上板（2026-10-30，P1 读流式） | **放弃序列只清 RAM，不发设备 LAST**：跨 chunk 的序列在「命令上下文不同 / 续接点不一致 / 续窗口首 pair 收不到 / 组被弃或失败 / 空闲超时」时由 `ftl_cache_seq_release()` 释放状态，但 NAND 侧仍停在 cache read 模式（预取页未消费）。假设：后续命令（普通读 `00h/30h` 或新 `START 45h`）会终止残序列，且不会有副作用。若器件要求显式 `47h LAST` 或 `FFh RESET`，则残序列会让后续读出错/挂起 | ① 只有上述异常入口才发生（正常路径必在 run 头 `START`、run 尾 `LAST`）；② 放弃后立刻允许普通读，并由 `ftl_cache_seq_idle_guard()` 限时 1s；③ 读数据错误按 D7 逐页上报，不会静默成功 | **上板负例**：读中途拔线/主机中止 → 再读同一/相邻 LBA，逐字节比对；若出现错页/超时，改为「放弃时下发 `FFh RESET`（LUN reset）收尾」并复核 datasheet 原文（R12） | `src/ftl/cache_batch.c` `ftl_cache_read_try`；`src/ftl/ftl.c` `ftl_cache_seq_release` / `ftl_cache_seq_idle_guard`；`include/ftl/ftl.h` `ftl_cache_seq_t` |
| RISK-IF-10 | 待上板（2026-10-30，P1 读流式） | **被放弃的 MORE 链尾会占住 FC `chain_owner`**：流式窗口末条 `MORE` 后若下一窗口（同一 SCSI 命令的下一 chunk）不再到来，链在 FC 侧没有 `END`；`chain_owner` 只由「末条 END」或「派发失败」清。期间同 LUN 的 **HSQ 后台命令（CP/journal/GC）被永久饿死**，直到下一次主机提交（任意命令作为新链尾 END） | FTL 门控 + `ftl_cache_seq_idle_guard()` 只释放 FTL 状态，**不碰** FC 链；正常路径下一 chunk 在 ms 级到达并续链；USB BOT 串行 + CLI gate ⇒ 同一时刻只有一个主机命令 | **上板**：大顺序读中途中止/拔线，观察之后 CP/journal 是否恢复；若饿死，需要 FC 侧「强制结束链」或放弃时下发 LUN reset（优先走 reset，顺带覆盖 RISK-IF-09）。跟踪触发：任何时候发现「主机空闲但 journal 水位不降」 | `src/fc/fc.c` `fc_poll_fill_queue`（chain_owner 更新）；`src/ftl/cache_batch.c` `cache_read_build`（末条 chain 位）；`src/ftl/ftl.c` `ftl_cache_seq_idle_guard` |
| RISK-IF-11 | 正常路径已上板（2026-10-30：10.1 万页确认、cw abort=0、全盘 0 错误）；失败注入未做 | **逐页确认依赖「完成按提交顺序」与 FAILC 语义**：缓存程序第 i 页的最终结果由第 i+1 条命令的 `FAIL_CACHE` 给出，确认 FIFO 按提交顺序 pop（头==自己的页才跳过）。若 FC 的完成顺序被破坏，或器件 FAILC 的「上一页」归属与实现不符，会把成功页标失败/把失败页当成功（**静默坏数据**）。 | ① FC 每 LUN FIFO 严格先进先出，完成按序；② FAILC 只在 op44/op48 映射（op49 首页不映射，避免残留）；③ 任一错误位（FAILC/NAND_FAIL/TIMEOUT/…）⇒ 整个序列中止、未确认页一律不确认映射、关闭 hot SB。 | **上板负例**：注入单 plane/双 plane 中间页 program 失败，确认失败页及其后页不确认、之前页已确认；命令 trace 复核完成顺序；若器件 FAILC 归属与假设不符，改为按 desc 逐 CW 状态归因 | `src/ftl/ftl.c` `ftl_cache_wr_member_done`；`src/fc/fc.c:778`（`fail_cache_valid`）；`src/nand/nand_fmc.c` `nand_fmc_exec_check_fail_cache` |
| RISK-IF-12 | 满盘压测已上板（2026-10-30：free=2 + 持续 GC，未观测 1s 停顿/IO_ERROR）；注入式验证未做 | **确认停泊 × 序列门控可能退化到 1 s idle 守卫**：序列打开时 L2P 加载 / journal 刷页的提交被 `ftl_op_seq_gate` 挡住；若确认因此停泊、且最老的组被占住（两个槽都用满），下一 chunk 无空闲槽 ⇒ 序列看不到下一窗口、停泊无法推进，直到 idle 守卫（1 s）中止序列（尾页按错误结算，host 重试）。 | ① 开序列前预加载窗口 L2P region；② 每窗口 `jrn_reserve` 预留本窗口条目，失败即本窗口 END（序列结束，刷页/加载能跑）；③ 下一窗口 L2P 驻留 lookahead；④ `ftl_cache_seq_idle_guard()` 兜底。 | **上板**：大顺序写 + free pool 低位（journal/GC 压力）测吞吐、IO_ERROR 计数与最长写停顿；若出现 >100 ms 停顿或非注入性 IO_ERROR，优先增加第三槽或把「预留失败立即 END」扩展到 ring 级 | `src/ftl/cache_batch.c` `ftl_cache_write_try`；`src/ftl/ftl.c` `ftl_cache_seq_idle_guard`；`src/controller/usb_msc.c` 槽状态机 |
| RISK-IF-13 | 审计结论 + deadline 挂起 + 限流均上板验证（2026-10-30 新镜像 17:33：满盘写+校验全程无 submit-stalled 刷屏，JRN/L2P 单行带 suppressed、GC/CP/journal 均推进） | **门控覆盖与后台/RR 交互（判定结论，非缺陷）**：`ftl_op_submit*` 是唯一提交漏斗（`src/ftl` 下 `grep fc_submit` 为 0），门控覆盖 host / GC / WL / CP / journal / metadata / L2P / RR。各后台路径都把门控拒绝当 `FTL_AGAIN` 重试：迁移 `mig_command_submit` 释放命令并回退 waiting、RR `rr_read_submit` 释放命令且 stage 不变、journal/checkpoint/metadata/L2P 同型。被拒提交置 `break_requested`，序列在下一窗口收尾；BOT 串行下 RR（host 读触发）与写序列不共存。 | **上板发现并已修**：后台提交 deadline（`FTL_SUBMIT_TIMEOUT_MS=10ms`）短于 USB 一个 chunk 的间隔（16 KiB @ ~0.84 MB/s ≈ 19 ms）⇒ 写序列打开期间 META/CP/MIG/RR 每 10 ms abort 一次并刷屏。修复：四类 job 的 stall 检查先看 `ftl_op_cache_seq_active()`，序列打开时**挂起 deadline**（等待有界：D4 下一窗口 / 空闲守卫 1 s）；META 的 stall 日志另加 5 s 限流。`info` 的 `d4=` 计数证明 D4 生效。 | **上板**：大顺序读写 + 后台 GC 压测，观察 `d4=`、`mig=` 与 `rrab=`；若后台长时间无进展，检查是否为门控未收尾（序列卡在等 chunk） | `src/ftl/ftl_op.c` 门控；`src/ftl/migrate.c:265`；`src/ftl/read_recovery.c:146`；`src/ftl/journal.c:451` |

---

## 2G. Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-HOST-12 | 待决策（2026-10-24 读码判定） | **组槽耗尽时 host 入口返回 `FTL_ERROR` 而非 `FTL_AGAIN`**：`ftl_submit_common` 在 `pend_group_alloc` 失败时直接 `return FTL_ERROR`（`src/ftl/ftl.c:1897-1901`）。组槽是**瞬态资源**（完成回调会释放），按 H38「资源类空/满一律 `FTL_AGAIN`」这里应可重试；当前口径会让 Controller/USB 把一次瞬态耗尽当成永久失败（host 看到 IO_ERROR），虽然极难触发（需 32 个组同时在飞）| 无。组槽 32 个、正常负载远达不到；未观察到现场 | 若确认要按 H38 收口：改为 `FTL_AGAIN` 并补一条 UT（先把 32 个组占满，再 `ftl_submit_write` 断言 `FTL_AGAIN`，最后释放）；同时回扫 Controller 对 `FTL_AGAIN` 的重试处理。改语义前先裁决 | `src/ftl/ftl.c` `ftl_submit_common`；`include/ftl/pending.h` `pend_group_alloc`；`CLAUDE.md` H38 |
| RISK-HOST-05 | 待定位（2026-10-21；诊断已补、已上板命中；**支路 (a) 板端确认**） | **`[FTL] confirm park timeout` 不是告警，是 host 写失败**：停泊超过 `FTL_GROUP_DRAIN_TIMEOUT_MS`（**100 ms 墙钟，固定预算**）后 `pend_confirm_service` 直接释放 pending 槽并按 `SSD_STATUS_IO_ERROR` 完成组 —— 数据已编程进 NAND，但 Journal+L2P 均未记账 ⇒ 该次写按失败返回、物理页作废（不产生静默错值）。100 ms 与满盘时后台 GC/CP 数百 ms 的争用不匹配（与 RISK-GC-09「预算单位/量级错」同型）。上游根因分三支：**(a) 静默**——`l2p_load_start` 的三种失败（`l2p_line_evict` 无候选 / `ftl_async_read_page` 命令池空 / `ftl_op_submit` 背压）全部**只返 false 不打日志**，`l2p_is_ready` 永远 false；最可能的形态是 **64 行全 DIRTY + Free Pool 空**（`l2p_flush_kick` 在资源预检或 `md_append_slot` 处背压）⇒ 无行可驱逐 ⇒ 加载永不启动，GC 补不上池就在 host 侧表现为 confirm 超时；**(b) 显性**——同一 region 加载 3 次失败后降级，打 `[L2P] region ... degrade`（LOGE，`log level error` 下仍可见）；**(c) 行卡在 FLUSHING**——该行 FC 命令永不完成（未被观测到）。⚠ 日志按 5 s 限流且打印**扫描到的第一条**停泊项 ⇒ 反复出现 `idx=0` / 同一 LBA **不代表只有它在停泊**，不能据此认定是单一 region 卡住 | 超时按失败结算（不会把未记账的写报成成功）；停泊日志 5 s 限流；`pend_confirm_park` 只改 op/owners，不新增列表（C9）。**2026-10-21 已补诊断**：`l2p_load_start` 三因限流日志（`no_evictable` 现由 `l2p_evict_fail_log` 打现场：`region=/inv=/clean=/dirty=/loading=/flushing=/suppressed=`，SLOW 档 5 s 或 64 次一行；`no_command` / `submit_backpressure` 仍为 1 s 或 64 次一行。同一次失败只打一行，不再重复）+ park 日志加 `parked=`（本轮停泊总数，不再只报首条 LBA） | 复现时取**判据**而非再猜：① 停泊前一行起 `log level debug` 的 `[L2P] load copy failed` / `[L2P]` 心跳；② `info` 的 `free/op/bad` 与 `FC` 资源计数（free 贴底 ⇒ 支路 (a)）；③ 有无 `[L2P] region ... degrade`（有 ⇒ 支路 (b)，查该 region 所在块是否已被 MIG 标 BAD）。修法候选：`l2p_load_start` 失败原因分级限流打印；停泊超时日志改成「停泊条数 + 首个超时 LBA」；超时预算按后台进展动态判定（先与 RISK-GC-09 一并裁决） | `src/ftl/pending.c` `pend_confirm_service`；`src/ftl/ftl.c` `ftl_confirm_write`；`src/ftl/l2p_cache.c` `l2p_line_evict` / `l2p_load_start` / `l2p_flush_kick` / `l2p_is_ready`；`include/ftl/ftl_module.h` `FTL_GROUP_DRAIN_TIMEOUT_MS`；板端 2026-10-21 日志 `confirm park timeout lba=292864 idx=0` / `lba=338352`；**rev33 上板（2026-10-21）新增诊断命中**：`[L2P] load start failed region=63/126/189/252/315 cause=no_evictable suppressed=0`，间隔 ≈63~64 个 region；同窗口**无** `confirm park timeout`、无 `region ... degrade`、无 `submit stalled`，`free=3362`。与「CLOCK 扫一轮只递减引用位、下一轮重试即可驱逐」的**瞬态**一致（`l2p_line_evict` 单次返回 NULL 是设计内行为），但尚不能排除「64 行全 DIRTY + flush 全背压」。下一步：在 `l2p_line_evict` 返回 NULL 处按状态计数（INVALID/CLEAN/DIRTY/LOADING/FLUSHING）限流打印一次，一次上板即可区分；**2026-10-21 H2testw 板端已确认支路 (a)**：`region=568 cause=no_evictable suppressed=3` 与 `confirm park timeout lba=290816 idx=0 parked=1` 同窗口出现，且 CP 卡在 `phase=1`（见 RISK-MD-09）⇒ 不是单次 CLOCK 空扫，是「日志槽取不到 ⇒ 脏行 kick 恒 AGAIN ⇒ 无行可驱逐 ⇒ 新 region 加载不了」的稳态 |
| RISK-HOST-08 | 部分已上板（2026-10-22：TRIM 写/擦/**断电回放**已验；全盘 H2testw/GC 已过；CLI 诊断/CP 冷启动 locate 保留；极端 Journal 页满未构造） | **TRIM 已修**：原 `ftl_trim` 用 `l2p_read_sync`/`l2p_write_sync`，Journal 容量不足时显式 `jrn_flush_page`。**残余非数据面同步点**：①CLI 诊断 `map`/`rdpaa` 用 `l2p_read_sync`/`ftl_sync_read_page`；②CP 冷启动首次 `md_ring_locate` 同步扫池（内含 `ftl_sync_read_page`）。后果：诊断/冷启动首次 CP 会阻塞主循环一次；**不造成静默数据破坏** | TRIM 已改 `ftl_trim_begin/step` + Controller 每轮按 run 推进；CLI 诊断是交互式控制面；CP locate 只在游标未建立时发生一次（有有效 CP 时零代价） | 上板验证 TRIM 异步：`tr <lba>` 后 `map <lba>` 为 INVALID、立即断电重启不复活；长 TRIM 期间 USB/后台 job 不停摆；Journal 水位随 trim 上升。若要求字面“除上电初始化外全异步”：①`md_ring_locate` 拆 phase（每次只读一页，跨 service 轮）或移到 boot 一次完成；②CLI 诊断保留同步（交互式命令）或也走命令队列。先裁决哪些算“控制面例外” | `src/ftl/ftl.c` `ftl_trim_begin/step`；`src/controller/controller.c` `ssd_controller_trim_service`；`src/controller/cli.c` `map` / `rdpaa`；`src/ftl/checkpoint.c` `CP_ANCHOR_PREP` / `md_ring_locate` |
| RISK-HOST-11 | 待定位（2026-10-24 板端 UT 观测） | **Journal 压力下 TRIM 持久化可能丢失**：media 套件在 trim 之前新增 matrix/random 大量写映射后，`trim_no_resurrection` 的**重启前**读回为 0，**模拟重启后**又读到旧数据（line 273 断言失败）。说明 trim 的 Journal 失效条目在回放时未生效（或 flush 屏障未覆盖该页），旧 L2P 快照胜出。把重写用例挪到 trim 之后可规避，但未修生产逻辑 | UT 顺序规避；TRIM 当前仍依赖 host FLUSH | 复现：trim 前用 matrix 写入同 LBA 区并 flush，再 trim+flush+模拟重启；检查 journal page_seq/truncate 与 CP 快照顺序，确认 flush 是否等到 trim 条目落盘 | 板端 2026-10-24 `test /ftl/media/` 日志（trim 后重启旧数据复活）；`src/ftl/ftl.c` `ftl_trim_step` / `ftl_flush_step`；`src/ftl/journal.c` `jrn_append`/`jrn_replay` |
| RISK-HOST-09 | 待后续（2026-10-22 读码判定；当前串行化下不可达） | **TRIM 命中 wbuf 中未编程 LBA 时只 drop 缓冲槽，不失效旧 L2P 映射**：`ftl_trim_step` 的 `wb_has(l)` 分支认为「它从未落盘，无旧映射可失效」，但覆盖写场景下该 LBA 的**旧映射仍指向旧 PBA**。若 TRIM 与未完成 host 写并发，drop 后旧映射继续可读（TRIM 语义被破坏），valid_count 也不减。当前 CLI TRIM 只在 `g_cli_pending.busy==0` 且 `usb_msc_host_busy()==0` 时提交，Controller 也单飞行，所以该并发不可达 | Controller 串行化（CLI/USB gate + TRIM 单飞行）；`wb_handle(DROP)` 只清未编程槽并结算 owners | 接入 USB UNMAP / 并发 TRIM 前必须裁决：①TRIM 前先等该 LBA 的写完成/落盘，或②drop 时同步失效旧 L2P（Journal + valid_count）。上板：`wr` 一串后立即 `tr` 同 LBA，`map` 必须 INVALID | `src/ftl/ftl.c` `ftl_trim_step` 的 `wb_has` 分支；`src/controller/controller.c` TRIM 单飞行；`src/controller/cli.c` gate |

---

## 2H. 完成派发面（`ftl_op.c/h`）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-CR-02 | 待上板 | 坐标类改动未板验：`ftl_op_route_get_region` 的 dim→(ch,ce,lun) 分解改走 `ftl_dim_ch/ce/lun`（与 `lba_route_*` 收敛为同一套公式）。坐标属「编译期无声的三类量」 | 分解式与原手写式逐字同值；新增三个 `static inline` 纯函数（H42） | **必须上板**：跑一遍读写 + `map <lba>` | `include/ftl/l2p_cache.h` / `src/ftl/ftl_op.c` |

---

| RISK-FC-05 | **复现条件已消除，底层原因未证实**（2026-10-30） | `/speed/ftl_rw` 连跑 4 组合时，`cache rd=4` 结束后 FTL 回不到 idle：一条带 ctx 的**后台双 plane 页写**（`op=31`、`chain=END`）留在 SQ 头，`lun_used=0/lun_phase=IDLE/owner=NONE`，填充 pass 在跑（`fill_delta=7496/3748`）却没把这条命令发出去，teardown 断言 `ftl_op_ctx_count()==0` 失败。单独跑该组合不复现 ⇒ 依赖前序组合留下的状态。 | **测试侧隔离**：`/speed/ftl_rw` 的非首个组合开始时先 `ftl_ut_reboot_from_media()`（quiesce + `fc_init` + 重新武装 + PFR），只清 RAM 运行态、保留介质内容。全套 `test` 已 **148/148 通过**（2026-10-30 22:43:58 镜像）。FC 侧只读诊断 `fc_debug_state_get()`（队列头 / 链锁 / LUN / 填充计数）保留。 | **重访触发**：① 若在 H2testw 或长时间 running 下出现「host I/O 变慢/停住」；② 若其它测试再出现 teardown ctx 残留，`ftl teardown leaked …` 行会带 `sq_head/st/bound/sched_sq/fill`，按该行定位。⚠ **这不是修复**：泄漏可能在,只是复现条件被隔离掉；不要把它当成已闭合。 | 串口 2026-10-30（`SPEED idle timeout:` 行）；`src/fc/fc.c` `fc_poll_fill_queue` / `fc_debug_fill_counters`；`src/fc/fc_interface.c` `fc_debug_state_get`；`tests/unit/test_speed.c`（组合隔离）；`tests/unit/ftl_ut_common.c`（teardown 泄漏消息） |

## 2I. 页打包写缓冲 wbuf（`wbuf.c` + `wbuf.h`，本轮审查）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-WB-03 | 待上板 | 模块抽出（`ftl.c` §4 → `wbuf.c`/`wbuf.h`）+ 位运算收敛到 `ftl_cw_bit` + hot-path 日志降 `LOGD`：纯搬移与等价改写，但 wbuf 是写路径核心 | 逐段比对搬出前后函数体一致；`make all` / `make release` 零新告警 | 随常规回归上板（`wr`/`rd`/`fl`/`trim` + `wp <lba> 1 8` 合并仍为 1 次编程） | `src/ftl/wbuf.c` / `include/ftl/wbuf.h` |

---

## 2J. FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`，本轮审查）

> 接线与引擎现同处 `src/ftl/bg_scheduler.c`（任务表当前 10 项 / `bg_task_*` 适配器 /
> 档位预算 / Phase 3 入口 `bg_scheduler_service`），`ftl.c` 只留一行调用。
> 历史根因：`40f2bee`（磨损并入 Checkpoint）把任务表 14 → 13 项，只改了代码没改注释与结构。

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-LOG-02 | 待上板（2026-10-23） | GC 事件在极端 victim 速率下仍可能占用 UART；没有通用 `log_ratelimit_t` | 默认 INFO 只打 `[GC] start`（每会话一次）+ `[GC] progress`（5 s）+ `[GC] end`；逐 victim `[MIG] move` 仅 DEBUG 且只带变化字段；空块只进 `noop`；`[STAT]` 10 s | 上板 30 s 抓包：INFO >5 行/s 或 UART 阻塞导致吞吐下降 ⇒ 引入统一限流 helper（优先提升 `ftl.c` 的 `ftl_log_throttle_t`） | `src/ftl/migrate.c` `mig_gc_session_progress` / `mig_job_finish`；`include/common/log.h` |

---

## 2K. 历史批次（2026-10-19 缺陷收口）

2026-10-19 的逐条收口批次已移到 `docs/archive/known_risks_batches.md`。
当前状态以其对应的 `RISK-*` 活跃行为准；批次里的 `MD-xx`/`GC-xx` 只是当时的别名。

## 2L. FTL 模块 handle 契约（2026-10-19 设计，未改代码）

> 设计文档：`docs/design/ftl_design.md` §5.1（全文 `docs/archive/ftl_module_contracts.md`）。本节只登记架构决策与后续项，
> 避免下一轮把「有意设计」当缺陷重查（R11）。

| ID | 状态 | 风险 / 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-ARCH-02 | 待后续（性能） | 跨 LUN superblock 条带 / 跨 LUN GC/WL **未实现**；当前 migrate 严格 per-LUN（源/目的/账本都在本实例） | per-LUN `mig_handle`；`lba_routes_to` 守卫 | 上板基线稳定、需要多 die 均衡/条带性能时重估；需要设备级 relocation 引擎 + 跨实例事务，不是改 handle 入参 | `docs/archive/multi_lun_generalization.md:26,358-359`；`migrate.c:553-557,524`；`allocator.c:26-34` |

---

## 2M. USB MSC / 平台（2026-10-20，代码已落地）

> 设计文档：`docs/design/usb_msc_udisk.md`。本节只登记「未上板 / 待确认」项。

| ID | 状态 | 风险 / 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-USB-02 | 待上板 | MSC 异步完成路径未实测：`tud_msc_async_io_done()` 的 deferred 回调、主机复位期间的 `discard` 收尾都只在静态路径成立 | `discard` 标志 + `tud_ready()` 守卫；FTL 只持有静态 bounce buffer | 上板：读写过程中拔线/总线复位/重新枚举，确认无死命令、无残留 CSW 错误；确认不会出现 `pending_io` 永久为真 | `src/controller/usb_msc.c` `usb_msc_task` / `tud_umount_cb` |
| RISK-USB-05 | 待上板 | WRITE(10) 的 **FUA** 位未处理：主机若只发 FUA 而不发 SYNCHRONIZE CACHE，命令完成时数据仍在 FTL 缓冲/日志，不保证已编程到 NAND | 主机 fsync 路径通常随后发 SYNCHRONIZE CACHE；`ftl_flush` 会先刷 wbuf 再刷 L2P/journal | 上板抓包确认主机是否使用 FUA；若使用，需要在写完成路径补 flush（或在 MSC 回调里记录 FUA 并要求 `usb_msc_task` 完成后 flush） | `src/controller/usb_msc.c` `usb_msc_transfer`；`src/ftl/ftl.c` `ftl_flush` |
| RISK-PLAT-07 | 信息 / 待决策（2026-10-24） | **所有构建配置共用同一个 `debug.elf`**（`BIN_DIR`/`TARGET` 只按 debug/release 分，不按 `UT` 分）：`make tests UT=FTL` 后再 `make tests UT=FC`，后者的镜像**只含 FC 套件**并覆盖前者。板端现象是 `test /ftl/` 打印 `NO test run,0(100%) skipped.`（不是套件坏，是镜像里没有 FTL 用例）。2026-10-24 已真实踩到一次 | 无代码改动。构建顺序纪律：**最后一个构建决定烧录镜像**；烧 FTL UT 前必须让 `make tests UT=FTL` 是最后一次链接。已把 FTL UT 镜像重新链接回来 | 待决策：是否按配置给 ELF 命名（如 `debug_ut_FTL.elf`）并把 `launch.json`/烧录路径跟着分；在此之前，怀疑「用例凭空消失」时先看横幅时间戳对应的**最后一次**构建命令 | `config.mk:76-81`（`BIN_DIR`/`TARGET` 共用）；`docs/design/ftl_ut_design.md` 运行流程；板端 2026-10-24 20:05:13 镜像 `test /ftl/` → `NO test run` |
| RISK-USB-07 | USB 路径已上板（2026-10-30：2.7 万写 chunk + 全盘 0 错误）；CLI static 请求继承路径未专项验证 | **`request->io` 模板的 per-run 字段跨命令残留**：`ssd_request_split()` 把本 run 的 `cmd_remaining`（同一 host 命令在本 run 之后还剩的 LBA）写回 `request->io`（跨命令复用的模板）。USB 路径每次 `usb_msc_slot_submit()` 都 `memset` 并重设，**CLI 路径**是 `static g_cli_pending.req`：残留的 `cmd_remaining` 会让下一条 CLI 命令误以为自己还有后续数据（序列过晚 END / 门控挂到 1 s 空闲守卫） | 已改：split 入口把 chunk 级 `cmd_remaining` 取成本地常量，循环内只写 per-run 值，**所有出口还原模板** | 上板：连续两条不同长度的 CLI `rd`，确认第二条仍走 cache 读且 `45h/47h` 成对；若后续把 `ssd_request_t` 改为流水提交，必须去掉「还原模板」改为按请求保存 | `src/controller/request.c` `ssd_request_split` |
| RISK-USB-08 | 正常路径已上板（2026-10-30：满盘写流下 lookahead/BUSY 走量，CSW 全成功）；失败 CSW 未注入 | **写 lookahead 的提前回执 / BUSY 背压未上板**：非末写 chunk 在 FTL 确认前就回 `tud_msc_async_io_done`（纯流控），若中途失败，最终由末 chunk 的 CSW 报错；主机对「提前回执后 CSW 失败」的行为未验证。另一风险：两槽都忙时返回 `TUD_MSC_RET_BUSY`，依赖 `tud_task` 单次最多 16 个事件（`CFG_TUD_TASK_EVENTS_PER_RUN`）才不饿死 FTL。 | 末 chunk 回执前用 `s_wr_error` 汇总前序 chunk 的失败；FTL 失败不确认映射，host 重试走新 SB。 | **上板**：写大文件 + 程序失败注入；观察 CSW 是否失败、主循环是否被 BUSY 重投拖慢；若 BUSY 重投开销显著，改为「第三槽 + 更低概率背压」或给 TinyUSB 事件上限调小 | `src/controller/usb_msc.c` `usb_msc_task` / `usb_msc_slot_complete`；`third_party/tinyusb/src/tusb_option.h:651`（事件上限） |
| RISK-PLAT-08 | 信息 / 待定位（2026-10-25 启动记录） | **`.noinit` 故障记录出现一次新的 HardFault**：`PC=0x0801665e LR=0xffffffff CFSR=0x00000400 HFSR=0x40000000`（IMPRECISERR + FORCED）。用**当前** debug 镜像符号解，PC 落在 newlib `_svfiprintf_r`（`vfprintf.c:1584`）；故障发生的镜像无法确定——记录跨 PIN 复位保留、只有断电才清（§3.3），不能据此说“又崩了一次”，也不能排除是之前 release/H2testw 会话留下的。本次 01:09:23 镜像跑 `/ftl/` 85/85 正常，运行期间无新故障 | 无 | 先断电清空 `.noinit`，再用同一镜像复现；若再现，用**当场镜像**的 `.sym`/`.elf` 解 PC，检查 printf 调用点与栈哨兵/帧指针 | 板端 2026-10-25 01:09 启动 `previous crash:` 行；`docs/design/fault_capture.md`；`src/platform/stm32h743/fault_port.c` |

---

## 2N. 容量 / 几何（2026-10-20）

> 本节 4 条（`RISK-CAP-01` ~ `RISK-CAP-04`）已于 **2026-10-22** 全部闭合
> （v12 首启格式化 + 全盘 H2testw 415/423 MB 通过）。详见文末「归档索引」与 archive。

---

## 2O. NAND / FMC 时序与吞吐（2026-10-20 分析，未改代码）

| ID | 状态 | 风险 / 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-NAND-05 | 待定位（上板观测 2026-10-20） | **used 介质持续出现不可纠读页**：rev32 启动后 host 读 LBA 129 反复报 `[ERR] [FTL] read unrecoverable lba=129 gblock=1064 retry=8`。RR 的 Stage 1（读重试）用 Feature 地址 **0x89**——本器件该地址是**保留段**（见 §四），实际没有电压/软判决重试；ECC 不可纠 ⇒ 直接 `bbt_mark_bad(1064)` + host `IO_ERROR`。L2P 仍指向该坏块（悬空），同一 LBA 的每次 host 读都会重试 8 次并重复打日志；若该 LBA 属文件系统元数据，Windows 可能把卷判为损坏/RAW。已登记坏块：305（GC 活锁）、1064（本次）。**非本仓 FTL 逻辑产生**，是芯片物理页失效（或早期写坏）；数据在该 LBA 上本就不可读 | 坏块由 `bbt_mark_bad` 隔离（RAM + 后台 BBT 持久化）；格式化后该 LBA 会重映射到新页，日志停止 | ① 现场处理：Windows 快速格式化（或 CLI `format` + POR）重建文件系统后再跑 H2testw；② 观察 1064 是否在重启后仍为 BAD（BBT 持久化）；③ 若坏块持续增长，评估换芯片 / 真实 read-retry（需器件支持，0x89 不可用）| 板端 rev32 串口：`[ERR] [FTL] read unrecoverable lba=129 gblock=1064 retry=8`（两次）；`src/ftl/read_recovery.c:284-293`；`docs/design/known_risks.md` §四「0x89 保留地址」 |
| RISK-NAND-06 | 待定位（2026-10-21，v0.86 板端证据） | **BAD 标记来源与 NAND 时序/FTL 写路径强相关，且旧 verify 不测已知 BAD**：此前 CLI `format --verify` 日志为 `tested=3810 bad=0`——它**跳过**了 BBT 里已有的 282 个 BAD，所以这 282 个是**运行期**累积的标记（host 写 program fail / RR 不可纠读 / 擦除失败），不是那次 verify 发现的。2026-10-21 开卡镜像的全片 re-scan（**低速**，在 `[NAND] switched to ONFI Mode 5` 之前）得到 `old_bad=282 recovered=282 new_bad=0`、`tested=4092 bad=0` ⇒ 这 282 个块**不是硬坏块**（低速擦/写/读全过）。三个待区分的候选：① **Mode 5 时序边缘**导致运行期程序/读失败；② **FTL 写路径 bug（如同一页二次编程）**导致读错后被 RR/写失败标坏；③ 真正边缘块（低速偶然通过）。⚠ 另外，`pfr_format_verify_blocks` 是**单 plane** 写，运行期 pair 写走 **multi-plane**；多 plane 程序/时序问题不会被这次体检捕获。⚠ 图样的块分量是 `(BYTE)block`（只覆盖低 8 位），且每块测完立即擦除 ⇒ **块号高位错误 / 两块地址别名**不会被检出（同 §3.2 的 `expected_byte` 截断教训）。 | 当前 0 BAD、428 MiB；运行期有 write retry + Spare/OP，但读侧仍可能丢数据 | ① 正常镜像 0 BAD 跑 H2testw；② 失败时看标记来源：`[FTL] program failed` ⇒ 写/时序；`[RR] read unrecoverable` ⇒ 读/二次编程/时序；`[JRN]/[META] ... erase failed` ⇒ 擦；③ 用 `map <lba>` + `rdpaa <paa> --raw` 判断是映射错还是物理页错；④ 再跑一次 **Mode 5 下**的 CLI `format --verify`：若那批块在直测中仍失败 ⇒ 时序/物理；若直测通过但 FTL 运行期又标坏 ⇒ 回到 FTL 写路径（二次编程）方向 | 板端 2026-10-21 日志 `full re-scan old_bad=282 recovered=282 new_bad=0`；此前 `format --verify: done tested=3810 bad=0`；`src/main.c` `ssd_ftl_boot`（format 在 Mode 5 切换前）；`src/nand/devices/mt29f___g08a.c` `nand_enable_high_speed`；RISK-NAND-01/03；**2026-10-21 夜 rev33 追加**：+264 BAD 全部 `fc_err`(UNC)；低速 re-scan `old_bad=282 recovered=282 new_bad=0`，**用户报告高速 re-scan 亦通过**（待补命令形态/日志）⇒ 候选 ①（单纯时序）进一步削弱，主嫌疑转向 **② FTL 写路径（同页二次编程 / pair 写）** 与 **GC 读范围（读到未编程页）**。⚠ 现有 re-scan/`format --verify` 是**单 plane、顺序写满再读**，既不复现 pair 写，也不读“半写块的空洞页”，故不能排除这两项 |

---

## 2P. allocator（2026-10-21 读码判定，未改代码）

| ID | 状态 | 风险 / 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-ALLOC-09 | 已上板验证（生路）/ 结构性部分转 ISS-FTL-17 | **低 free 时成对分配失败 ⇒ 写路径永久 stall**：2026-10-31 GC 压力测试（`free` 降到 ~50 后 GC 开始回收空块）在 `free=264` 时 `drain stalled`。dump：`seq act=0`、`pend=0`、`wb_busy=0`、FC 池全空、`sb_open=0 np=64 p0=03`、`free=264` ⇒ 唯一阻塞是**打不开新 SB（每 plane 各需一块）**。机制（高置信度）：**恢复路径把 closed 列表建成单 plane 条目**（`pfr_closed_sb_add` 每物理块一条，所以 `closed+free=3385` 是 block 口径），GC 逐条回收**空块**（`nop` 54→671、`mv=0`），free 池逐块增加但**两 plane 不平衡**；某 plane 先耗尽后每次分配都从 OP 池补（204/196），OP 耗尽 ⇒ 分配永久失败。次生问题：`cache_wr_build` 把 `alc_super_open_alloc_pair` 的 `FTL_ERROR` 也映射成 `FTL_AGAIN`（H38：永久失败被当瞬态），于是 drain 重试 8 轮判 stall → 该 chunk deferred error → 主机 FS 脏（H2testw Code 1392） | **已改（2026-10-31，未上板）**：① allocator 新增 `free_plane_count[PLANE_NUM]`（唯一闸门 push/remove 维护）+ `alc_free_get_count` / `alc_free_min_plane_count`；② GC 触发 T1 增加「任一 plane < `FREE_POOL_PLANE_LOW`(4)」；③ victim 选择加 plane 平衡偏好（成对条目 + 饥饿 plane 单条目优先，偏好组空才退回）；④ `cache_wr_build` / `wb_flush` 把 alloc 的 `FTL_ERROR` 原样上报（不再伪装 `FTL_AGAIN`）；⑤ stall dump 与 `info` 的 diag 行加 `fpmin`。**结构性根治（持久化 block→SB 归属）转 `known_iss.md` ISS-FTL-17** | **板端（2026-10-31 20:13 镜像）**：同一场景下 `free` 跌破 32（稳态 30~32）后 H2testw **写不中断**（239 MB 时仍 894 KB/s）、`fpmin=10`（没有 plane 贴 0）、`nop` 持续增长（空块回收）；对比上一版 `free=264` 即 stall ⇒ 1+4 的生路验收通过。但观察到 `op=204/0`（plane1 的 OP 耗尽、free 10/20 仍不平衡——偏好阈值 4 才生效）与 **`dng=2`**（见 RISK-ALLOC-10，优先级更高）；`mv` 仍为 0（victim 全是空块，见 ISS-FTL-18）。断电视角另测 | 板端 2026-10-31：`[ERR] [FTL] drain stalled gi=0 next=0/32` + `stall: … free=264 sb_open=0 np=64 p0=03`；`nop=54→671`、`mv=0`；H2testw `offset 0x13000000` Code 1392。`src/ftl/allocator.c` `alc_super_open` / `alc_free_find_block`；`src/ftl/power_failure_recovery.c` `pfr_closed_sb_add`；`src/ftl/migrate.c` `mig_job_finish`；`src/ftl/cache_batch.c` `cache_wr_build` |
| RISK-ALLOC-06 | 待后续（构造点无入参契约；**消费点 `gblock` 断言已按用户裁决删除**，2026-10-21，未上板） | `ftl_gblock(plane, block)`（`bst.h:147-150`）没有入参断言，返回 `WORD`：`plane * BLOCKS_PER_PLANE + block` 在 `int` 里算完再**隐式截断到 16 位**。`FTL_BLOCK_NO_PLANE = 65535` 是 `super_block_t.plane_blocks[]` 的合法哨兵：`ftl_gblock(0, 65535) = 65535` ⇒ 访问器的 `gblock < FTL_BLOCK_TOTAL` 能抓到；但 `ftl_gblock(1, 65535) = 2048 + 65535 = 67583` ⇒ 截断成 **2047**，落在合法区间内、实际指向 **plane 0 的 block 2047**，断言**抓不到**。这不是算式写错，是输入契约（`plane < PLANE_NUM`、`block < BLOCKS_PER_PLANE`、且不得为哨兵）没有被编码；编译期完全无声（无 `-Wconversion`），普通 UT 也不会走到，除非故意构造带哨兵 plane 的 SB 并走一条漏了 `super_block_has_plane` 的路径。⚠ **分层边界**：gblock 是 FTL 的 **BST 数组下标**，不进入 FC。FC 数据路径的守卫分两种：① `pba_is_valid` / `paa_is_valid_*` 只校验 PAA/PBA **格式**；② 读路径的 `fc_data_path_meta_check` 会把页内 META 与 `command->meta`（期望 LBA）比对，能检测「读到了别的 LBA 的页」。但 ② **只在读且 LBA 已知时生效**（GC/RR 用 `FTL_PAGE_CFG_DATA_ANY` ⇒ `FC_META_UNKNOWN`，主动跳过），**写路径没有语义校验**，账本访问器（valid_count / state）是纯 RAM、CRC/ECC 完全不参与；且检测发生在数据已丢之后 ⇒ 不能替代本层断言。更一般地：**CRC/ECC 保证的是 content integrity（读回位 == 写入位），不保证 placement / mapping 语义**——「把正确 LBA + 正确数据写到错误的块」对 CRC/ECC/META 全部自洽通过，因为错的不是内容而是意图；意图只存在于 allocator 账本里，算法无法审计 | **当前所有展开 `plane_blocks[]` 的调用点都有 `super_block_has_plane` 守卫**（抽查 `migrate.c:64/94`、`allocator.c:413/459/545`、`power_failure_recovery.c`、`read_recovery.c:881`、`wear_leveling.c:133`），故不是活 bug。⚠ **访问器上的 `gblock` 断言已按用户裁决删除**（2026-10-21）：此后的边界保护只剩「各调用点的 `has_plane` 守卫 + 构造漏斗（尚未加断言）」；CRC/ECC 只保证内容完整性，不覆盖账本/placement。`alc_free_push` 的四条断言（含 `gblock`）**未动**——那里同时有 release 拒绝 | 在 `ftl_gblock`（单一构造漏斗，C9）内加 `ASSERT(plane < PLANE_NUM); ASSERT(block < BLOCKS_PER_PLANE);`——哨兵在**构造处**即被拒，不依赖每个调用点记得 `has_plane`；访问器的 `gblock < FTL_BLOCK_TOTAL` 已按用户裁决删除（不经构造器的 CP 回放 / `fail_gblock` / 循环下标路径现在只剩调用点守卫）。触发：新增 `ftl_gblock` 调用点 / 有人提出删访问器断言 | `include/ftl/bst.h:145-150`；几何：`include/nand/devices/mt29f___g08a.h:29-30`（BLOCKS_PER_PLANE=2048 / PLANE_NUM=2）、`include/common/ssd_types.h:31`（WORD=uint16_t）；哨兵 `include/ftl/allocator.h` `FTL_BLOCK_NO_PLANE`；`super_block_has_plane`（`allocator.h:151-154`）；FC 侧格式校验 `pba_is_valid`（`ftl_config.h:505-514`，调用点仅在 FTL→FC 漏斗的 ASSERT）、`paa_is_valid_paa`（`nand_config.h:224`，当前 **0 调用点**） |

---

## 2Q. FTL 类型 / 命名 / 对齐收口（2026-10-21，未上板）

> 本节条目已归档（见 `docs/archive/known_risks_batches.md` §2026-10-24）：RISK-ARCH-12（已改未上板）


## 2R. 位运算公共宏（2026-10-21 从 CLAUDE.md 迁入，未改代码）

| ID | 状态 | 风险 / 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-BIT-01 | 待后续 | 全仓唯一的**通用**（按位宽生成）掩码构造器 `NAND_FIELD_MASK(width)` 挂在 NAND 层，而 FTL / FC 同样需要按位宽生成掩码 ⇒ 跨层取用（H13 分层归属）。实际后果不是编译错误，而是**各自就地另写一套掩码宏**：NAND 位宽一改，FTL 侧的字面量宏不跟着变，位判断静默错位（C2「第 3 次必须提炼」的失败形态） | 现 FTL / FC 未直接使用它，各自掩码均为本地字面量宏；H15 已要求新增前先找它 | 下次出现「按位宽生成掩码」需求时下沉到 `include/common/`（NAND 层保留 `PAA_*` 别名），同步改 `CLAUDE.md` H15 的落点描述。触发：FTL/FC 出现第 2 个同类宏 | `include/nand/nand_config.h:100-104`（定义 + `PAA_*` 消费者）、`:176/:199/:207/:213/:279`（NAND 内部使用） |

---

## 2S. FTL cache read（2026-10-23；2026-10-31 上板验证）

> 本节条目已归档（见 `docs/archive/known_risks_batches.md` §2026-10-24）：RISK-CACHE-01（已改未上板）
> 本节条目已归档（见 `docs/archive/known_risks_batches.md` §2026-10-30）：RISK-CACHE-14（已闭合 2026-10-30，并发 cache 读门控早开）


## 2T. FTL cache program（2026-10-23；2026-10-31 跨 block / padding 上板验证）

> 本节条目已归档（见 `docs/archive/known_risks_batches.md` §2026-10-24）：RISK-CACHE-02（已改、host 正常路径上板；FAILC 负例未验） · RISK-CACHE-03（已改、正常路径上板；失败注入未做） · RISK-CACHE-04（已改、正常路径上板；HSQ/chain 未引入） · RISK-CACHE-05（已改、上板） · RISK-CACHE-06（已改、正常路径上板；掉电注入未做） · RISK-CACHE-12（已改、上板） · RISK-CACHE-13（已改、上板） · RISK-CACHE-07（已改、正常路径上板；失败归因未注入） · RISK-CACHE-08（已改、正常路径上板；失败注入未做） · RISK-CACHE-09（已改、正常路径上板；瞬态背压未注入） · RISK-CACHE-10（已改、正常路径上板；丢完成未注入） · RISK-CACHE-11（已改、上板）

## 2U. ITCM / DTCM 热路径（2026-10-23，正常路径已上板；压力/掉电未覆盖）

> 本节条目已归档（见 `docs/archive/known_risks_batches.md` §2026-10-24）：RISK-TCM-01（已上板验证） · RISK-TCM-02（已上板验证） · RISK-TCM-03（已上板验证） · RISK-TCM-04（已上板验证） · RISK-TCM-09（已上板验证） · RISK-TCM-05（已上板验证） · RISK-TCM-06（已上板验证） · RISK-TCM-08（已上板验证） · RISK-TCM-07（已判定）
> 本节条目已归档（见 `docs/archive/known_risks_batches.md` §2026-10-30）：RISK-TCM-10（已闭合 2026-10-30，ITCM 运行期只读）

| ID | 状态 | 风险 / 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|

## 2V. 板端读完整性事件（2026-10-23，观察中）

| ID | 状态 | 风险 / 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-RD-01 | 观察中，未定位 | **单次不可纠读，状态 `CRC\|MATA`**：`lba=220 pba=0x000404cc g=2562 pl=1 bk=514 pg=25 cw=0`。`MATA` 表示页内 META 的 LBA 与请求不一致，不是单纯单元磨损；RR 按设计退役该块（`bad=1`、`free+1`），LBA 220 数据丢失，容量 424→423 MiB（`lba_max=867840`）。可能是旧介质页/旧映射，也可能指向 cache program / GC 批处理或 L2P 恢复的映射问题。 | RR 退役块并持久化 BBT；断电重启后 `bad areas data=1`、无新 `unc`，系统正常。 | **跑 H2testw 415/423 MB**；若再出现任意 `st` 含 `MATA` 的 `unc`（尤其不同 LBA），停下并抓完整日志 + 前后 STAT，按映射问题查 `cache_write` / `migrate` / L2P journal。 | 板端串口日志 2026-10-23；`src/ftl/read_recovery.c` `rr_read_complete`；`src/fc/fc_data_path.c` MATA 判据 |

---

## 2W. pending 表 / 完成组（2026-10-24，未上板）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|

---

## 2X. 日志格式化 — 编译器格式检查收口（2026-10-24，未上板）

> 触发：`log_printf` 补上 `__attribute__((format(printf, 2, 3)))`，格式串与实参不匹配从
> 「板端才看得出乱码」变成编译期告警。同时链接改为完整 newlib（`config.mk` 去掉
> `-specs=nano.specs`），`%llu` / `%zu` / 浮点格式化均可用。

> 本节条目已归档（见 `docs/archive/known_risks_batches.md` §2026-10-24）：RISK-LOG-05（信息）

---

## 2Y. UT 框架输出（2026-10-24）

> 本节条目已归档（见 `docs/archive/known_risks_batches.md` §2026-10-30）：RISK-UT-07（已闭合 2026-10-30，FC 擦除页判据）

| ID | 状态 | 缺陷 / 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-UT-04 | 信息 / 已上板（2026-10-24 23:13） | **FC 速度测试直接擦写 live FTL 拥有的 per-plane 块 1024..**：`/speed/fc_nand_rw` 用 FC 公共接口对原始块做 multi-plane 擦写，FTL 的 RAM 映射/日志区可能指向这些块；测试期间 FTL 主循环被 CLI 阻塞、`bg_suspend=1`，不会并发分配，但**测试返回后 FTL 状态可能已失效**。若用户跑完 FC-only 直接交给 host/USB，可能读到旧映射或写失败 | 测试 summary 明确打印“raw blocks destroyed — run format + POR”；文档要求 FC-only 后 `format` + POR；完整 `/` 运行由 `/speed/ftl_rw` 的 format 修复 | 任何改变 FC 速度测试块范围/是否擦写的改动都要重新评估；不要把它并入默认的 `test /ftl/` 流程 | `tests/unit/test_speed.c` `test_fc_speed_mp`；`docs/design/fc_nand_ut.md` §4.1.1 |
| RISK-UT-01 | 已改未上板（SKIP 分支本轮未复现） | **munit 只给 `[ OK ]` 闭合方括号并换行**：`" ] [ 耗时 ]\n"` 写在 `report.successful > 0` 分支内，`[ SKIP ]` / `[ FAIL ]` / `[ ERROR ]` / `[ TODO ]` 只打出左边 `"[ SKIP "` 就返回，下一条用例名接在同一行（板端实测 `[ SKIP  /ftl/pure/io/wl_victim_threshold: ...`）。后果：跳过/失败行与下一条混排，容易把 SKIP 读成通过 | 把「闭合 + 耗时 + 换行」移到 if/else 链之后，对所有结果标签统一执行；耗时平均仍按 `report.successful` 分支保护（0/1 时取单次） | **复现条件**：`l2p_miss_unmapped_evict` 在 cache 行有 DIRTY/FLUSHING/LOADING 时按设计 `MUNIT_SKIP`（`tests/unit/test_ftl_pure.c:1768-1776`）。同一上电先跑一个写介质的 media 用例（如 `cross_region_roundtrip`）再跑 `test /ftl/`，即可验证 SKIP 行格式；FAIL 行可临时改一条断言造负例（C7）。2026-10-24 20:08 镜像跑出 77/77、0 skipped ⇒ 本轮走的是 OK 分支，修复未获证据（R1） | `tests/framework/munit.c` `munit_test_runner_run_test_with_params`（结果打印尾部）；板端 2026-10-24 76/77 输出的 `[ SKIP  ...` 行；`tests/unit/test_ftl_pure.c` 的 SKIP 条件 |
| RISK-UT-05 | 信息（2026-10-25 板端实测） | **munit 的 `[ OK ] [ 耗时 ]` 与 Total wall-clock 在长用例上按 10.737 s 取模**：DWT CYCCNT 32-bit @400 MHz ⇒ 回绕 2^32/400e6 = 10.737 s，munit 直接相减、不做回绕补偿。证据：FC normal 真实 6.975+7.244+0.041=14.26 s，打印 3.52 s（=14.26−10.74）；FTL cache 真实 ≈43.3 s，打印 0.368 s（=43.3−4×10.74）；Total 真实 ≈98.8 s，打印 12.95 s（=98.8−8×10.74）。**用例内部的 `wr_ms`/`rd_ms`（`ftl_job_now_ms`）不受影响** | 速度结论只看行内 `*_ms` 字段，不看 munit 计时 | 若要准确框架计时：把计时源改成 64-bit ms 或累计回绕次数，不要在 CYCCNT 上直接相减 | 板端 2026-10-25 release 日志 `SPEED ... wr_ms=6975 rd_ms=7244` vs `[ OK ] [ 3.52 s ]`；`tests/framework/munit.c` 计时逻辑 |
| RISK-UT-06 | 信息（2026-10-25 板端实测） | **每次完整跑 `/ftl/media/` 会永久标坏 1 个用户块**：`bbt_persist` 从 Free Pool 取一个块做 `bbt_mark_bad`，验证 save→reload；该 BAD 经 BBT 持久化，后续 `format` 不清除坏块图。板端证据：启动 `bad=2` → 全量 85/85 跑完后 `bad=3`，本次消耗 1 块（256 KiB）。后果：反复跑全量按 256 KiB/次 缩减容量（423 MiB 盘约 0.06%/次）；不影响正确性 | 无（用例必须真实标坏才能验证持久化） | 若要长期反复跑：① 每次跑前记 `bad`，累计到阈值后接受容量下降或换片；② 或给测试提供“清除运行期 BAD”的生产路径（当前无 unmark API，不能在用例里手改终态）；③ 至少在测试计划中写明每次消耗 1 块 | 板端 2026-10-25 debug 01:09:23 镜像：boot `[BLK] bad areas ... data=2 total=2` → 跑完 `[STAT] bad=3`；随后 `[CAP] lba_max` 由 867,328 降到 **866,816**（恰为 1 gblock = 512 LBA = 256 KiB）；`tests/unit/test_ftl_media.c` `test_bbt_persist`；`src/ftl/bbt.c` `bbt_mark_bad` |

---

## 2Z. cache 长流 + Journal 顺序映射页（2026-10-31，设计 A；2026-10-31 上板验证）

> 起因：v1.00 的 cache 序列实际按 host 传输段（16 KiB）截断（H2testw `cr≈3.93 pair/batch`）。
> 设计 A：FTL 顺序流检测器开启序列、LBA 连续性跨段延续、到 Journal 顺序页配额（63 pair 页 ≈ 252 KiB）
> 主动 END 落元数据。设计文档见 `docs/design/ftl_design.md` §3.16。

| ID | 状态 | 缺陷 / 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-CACHE-15 | 已上板验证（正常路径；盘满例外未验） | **写序列的"无数据 END"问题**：器件没有不带数据的 END。旧方案在空闲 1 s 后 abort（未确认页按 IO_ERROR 结算、丢已提前回执的数据）。现改为 **padding END（本仓自拟）**：提交一个不映射的零页（META=UNKNOWN）作为 48h，用其 FAIL_CACHE 结算最后一页。**盘满例外**：padding 页也分配不到时，为避免「序列门控 ↔ GC 等 Journal 容量」死锁，放弃序列（未确认页 IO_ERROR + deferred error），让 GC 回收。剩余风险：padding 页落在新块 page0 时若失败，会关闭 hot SB（丢弃该块剩余空间）；padding 页被 GC 视为 invalid（无害） | 有序收尾路径不再 abort（盘满例外见左）；padding 页成功且为 page0 时记 `page0_programmed`，失败关 SB | **2026-10-31 上板**：H2testw 415 MB 全程 `pc=51`、无 `drain stalled`（padding 页落 page0 的 plane1 屏障问题已由 RISK-CACHE-24 修复并验证）；盘满例外（无页可分配 → abort）未验。观察 `pc=`；若失败率异常，查 padding 页的 META/ECC 与 page0 记账 | `src/ftl/ftl.c` `ftl_cache_pad_submit` / `ftl_cache_pad_callback`；`src/ftl/cache_batch.c` |
| RISK-CACHE-16 | 已上板验证（正常路径；deferred 注入未做） | **跨命令序列 + volatile write cache（正常路径已验证）**：① 所有写段（含末段）提交即回执，设备序列跨 host 命令延续；② 顺序页跨命令续用到 63 pair 再断（`sw` ≈ 1/63 pair）；③ padding END 收尾；④ deferred error 由下一条命令上报。编译四关通过、零新告警；UT 只证链接 | 构建通过；`info` 新增 `sw=starts/page_ends pc=pad_closes jf=page_writes` | **2026-10-31 上板**：H2testw **415/415 MB 0 错误**；`sw=1663/1610`、`pages/starts`=62.8、`aborts=0`、`pc=51`；掉电回放见 RISK-CACHE-17（已归档）。**deferred error 注入仍未做**（ISS-FTL-16）；失败运行中该路径被动触发过一次（见 RISK-USBC-01） | `docs/design/ftl_design.md` §3.16.10；`src/ftl/cache_batch.c` / `ftl.c` / `journal.c` |
| RISK-CACHE-19 | 已上板验证（正常路径）；page0 失败注入未做 | **cache 序列自开 block page0（`own_page0`）依赖"有序管线自证"**：允许序列在 page0 尚未确认时继续写 page>0，前提是 page0 的 FAIL_CACHE 失败会中止整个序列、其后页一律不确认（保住 rebuild 的「page0 空白 ⇒ 整块空白」捷径）。若改动 `ftl_cache_wr_member_done` / settle 顺序、或出现跨路径确认（例如旧页重试）使 page>0 先被确认，捷径会被击穿：rebuild 跳过该块 ⇒ 丢失仍被 L2P 引用的数据 | `get_room_seq` 只在 `own_page0`（本序列写的 page0）时绕开 page0 闸门；settle 成功才 `note_page_programmed`（内部按 plane+block 校验）；失败路径统一 abort 并结算未确认页；wbuf/GC 独立程序者仍走 `get_pba_state` 屏障 | **2026-10-31 正常路径已验证**（415 MB 运行中含 page0 自开 + padding 页落 page0）；**page0 编程失败注入仍未做**：确认该序列其后页都不确认、deferred error 上报、重启 rebuild 不引用该块；任何改 settle/abort 顺序的改动都必须复核本项 | `src/ftl/allocator.c` `alc_super_open_get_room_seq`；`src/ftl/ftl.c` `ftl_cache_wr_settle_idx`；`src/ftl/cache_batch.c` `own_page0` |
| RISK-USBC-01 | 部分验证（被动触发一次）/ 近似（非标准形态） | **deferred error 的近似**：已提前回执的写段后来失败时，本仓让**下一条** READ/WRITE/FLUSH 命令直接失败一次（TinyUSB 无自定义 sense）。与公开 SCSI 语义（CHECK CONDITION + deferred sense）不同：主机拿不到失败 LBA/异常类别（本仓出处层无 SPC/SBC 原文，具体 ASC 不作断言），FS 只能整条命令重试；若下一条是读，读被失败一次 | 错误不吞：下一条命令必失败一次并清标志；日志 LOGE 记录失败 lba/len | **2026-10-31 被动触发**：首轮失败运行出现 `write chunk failed after ack … — deferred` + 下一条命令失败一次，主机随即判卷被改并重挂——路径可见性满足，但**主动注入仍未做**（ISS-FTL-16）；主机侧后果（重挂/只读）符合预期，未改回末段等待 | `src/controller/usb_msc.c` `usb_msc_take_deferred_error` / `usb_msc_slot_complete` |
| RISK-CACHE-23 | 待议 / 设计（实测为慢，非死锁） | **持续 host 写期间后台 CP 被序列门控饿死**：板端日志显示 CP 从 t=80 s 起长期停在 `phase=8`（WEAR_WRITE），心跳 `age=1000~2003ms` 持续 460 s，`cp=100` 不再推进。机制：写序列 `active` 期间门控拒绝非所有者提交，后台不置 `break_requested`；序列在 Journal 配额处结束时 drain 立刻接手同一 host 命令剩余 LBA，门控空窗只有 µs 级（仅当配额恰落在大块边界时有 ~18 ms）。后果：Checkpoint 推进很慢（2026-10-31 通过运行：`phase=8` 心跳持续，但 `cp` 序列 61→99、上一失败运行的 `closed=1896`/`anchor seq=458` 在下次开机被正常恢复 ⇒ **不是死锁**，是后台让步窗口过小）。元数据收效变慢，掉电恢复点较旧 | CP 自带 1 s 心跳可见；序列门控不改 | **重访触发**：RISK-CACHE-22 归因后若指向 Journal/元数据回收，本项优先；可选方案：must-run 后台在连续被拒 N ms 后允许请求一次断点（`ftl_op_seq_gate_breaks` 加年龄条件），或序列结束后空出让位一轮（实现项与判据见 `known_iss.md` **ISS-FTL-14**） | `src/ftl/ftl_op.c` `ftl_op_seq_gate`；`src/ftl/checkpoint.c` `cp_job_step`；板端 `[CP ] job phase=8` 日志 |
| RISK-CACHE-18 | 已验证（flush 屏障）/ 设备 VWC 窗口未量化 | **提前回执扩大了掉电暴露窗口**：host 已 ack 但尚未确认的写（最后 1 页 + Journal 顺序页 ≤252 KiB 的映射）在掉电后可能回滚到旧映射。`SYNCHRONIZE CACHE` 在正常路径是持久性屏障（flush 先 padding END 再刷 journal/L2P）；超时仍按既有 best-effort 语义完成 | volatile write cache 的固有取舍；flush 语义完整 | **上板（2026-10-31）**：写 130 MB 断电重启，存活 117 MB verify 0 错误、`anomalies=0` ⇒ 窗口外无损坏这一半成立；117→130 MB 的尾差无法归因（拔 USB 即器件消失，Windows 写回缓存与文件大小元数据都会丢）——要单独量设备窗口需**主机先 flush/关闭写缓存**再断电。**flush 屏障已验证（2026-10-31）**：H2testw 写 168 MB → 关闭 H2testw → 安全弹出（主机 SYNCHRONIZE CACHE）→ 断设备电 → 上电 `boot type=1`（干净启动）、`journal_entries=0`、`closed=1342 free=2043 anomalies=0`（3385−1342=2043 ✓）、`dng=0`；挂载无需 chkdsk，H2testw verify **167 MB 0 错误**、**无尺寸缺口**（对比未 flush 的两轮 130→117 / 152→137 MB 缺口 ⇒ 缺口全部来自主机写回/FS 元数据，不是设备丢数据）。剩余：**设备 VWC 窗口量化 = 可选 / 暂缓（用户 2026-10-31 决定不做）**——依据：flush 后零丢失；未 flush 运行里主机发送 159.8 MB、设备实际占用 1222 块 × 128 KiB = 160.2 MB（数据页全部落盘），尺寸缺口是主机 FS 元数据；只剩 ≤1 页 JSEG 映射（≤252 KiB）未量，属设计上限。若将来要做，方法见 `testing_guide.md` 的可选条目（预分配 + WriteThrough + pattern 比对）。**判读坑（2026-10-31）**：不 flush 拔电后主机 FS 可能脏，H2testw 报 `Error reading file … Code 1392`——这是主机 FS 元数据不一致（不是设备数据错，数据错是 `Data error`）；先 `chkdsk /f` 再判，设备侧要查 `dng`/`rrab` 与 `mapping lost` 日志。**实测闭环（2026-10-31）**：`chkdsk /f` 报「1.h2w 项的大小无效」并修正（138→127 MB）后，H2testw verify **127 MB 0 错误**、`dng=0`/`rrab=0` ⇒ 已落盘数据零损坏；丢失的 11 MB 尾 = 主机写回缓存 + VWC 窗口 + FS 元数据未落盘（未分离） | `src/ftl/ftl.c` `ftl_flush_begin/step`；§3.16.5 |

---

## 3. 类型 / 显式强转（H44，提交 `6050703`）

> 本节条目已归档（见 `docs/archive/known_risks_batches.md` §2026-10-24）：RISK-TYPE-01（已修未上板）

## 4. 跨模块 / 历史欠账（已在 memory，不重复展开）

| 主题 | 记录位置 | 要点 |
|---|---|---|
| 介质布局 v9 未上板 | memory `ftl-media-layout-v9-unverified.md` | Anchor/CP P0+P1、Journal multi-plane 擦、Wear 并入 CP；老介质首启格式化 |
| allocator 不变量 / 列表容量 | memory `allocator-invariant-api-v0p77.md` | `alc_super_close_live` 列表满静默丢弃 vs `add_closed_sb` ASSERT 不对称；阶段 3「让 FREE 只说真话」未做、须上板 |
| FTL 入口守卫欠账 | memory `ftl-entry-guard-debt-census.md` | 77 处未覆盖；不是活 bug，但未来重构可达即静默降级 |
| FTL UT / 异步套件计划 | `docs/design/ftl_ut_design.md`、memory `ftl-ut-async-plan-v0p75.md` | UT 已重建并上板（85 次运行）；异步套件（`/ftl/async/`、`/ftl-hil/`）仍只有计划、未实现 |
| MemManage / 栈指针被写坏 | memory `hardfault-open-v0p71.md` | 已按用户裁决降级关闭、只记录；`-fstack-protector-strong` 未做 |
| 擦除 op 18→17 未决 | memory `cmd-operation-18-to-17-open.md` | 固件侧实测零改写，原因未闭合 |
| IO 路线建议点 | `.claude/io_review_backlog.md` | 只登记不分析；编号 `IO-NNN` |
| `read_recovery` 0x89 保留地址 | memory `nand-feature-addr-table-mt29f4g08.md` | 用户明确不处理 |

---

## 5. 板端回归入口

历史“本轮已修未上板”批次与 2026-10-24 收敛归档均在 `docs/archive/known_risks_batches.md`。
当前板端回归入口以 `docs/design/testing_guide.md` 为准；本表只保留仍成立的风险。

---

## 归档索引（2026-10-24 起，含 2026-10-30 / 2026-10-31 批次）

> 完整五要素见 `docs/archive/known_risks_batches.md`：首轮 §2026-10-24，后续 §2026-10-30 / §2026-10-31。
> 标「已修 / 已改」的条目正常路径已经 rev33 全盘读写验证；注入类触发条件仍按归档行执行。

| 模块 | 已归档条目（状态） |
|---|---|
| 1. metadata 模块 | RISK-MD-01（已修未上板） · RISK-MD-02（已修未上板） |
| 2. GC / migrate / journal | RISK-GC-01（已修未上板） · RISK-GC-02（已修未上板） · RISK-GC-04（已修未上板） · RISK-MIG-01（已修未上板） · RISK-JN-01（已修未上板） · RISK-JN-02（已修未上板） · RISK-RR-01（已闭合） · RISK-RR-02（已修未上板） · RISK-RR-03（已修未上板） · RISK-RR-04（已修未上板） · RISK-GC-07（已修未上板） · RISK-JN-04（已修未上板） · RISK-JN-05（已改未上板） · RISK-MIG-02（已修未上板） · RISK-MIG-05（已改未上板） · RISK-MIG-06（已改未上板） · RISK-RR-05（已改未上板） · RISK-BBT-11（已改未上板） · RISK-BBT-12（已改未上板） · RISK-BBT-13（已改未上板） · RISK-BBT-14（已改未上板） · RISK-BBT-15（已改未上板） · RISK-BBT-16（已改未上板） · RISK-MIG-09（已改未上板） · RISK-GC-10（已改未上板） · RISK-GC-15（已改、上板） · RISK-GC-16（已改、上板） |
| 2A. BBT | RISK-BBT-03（已修未上板） · RISK-BBT-05（已修未上板） · RISK-BBT-07（已修未上板） · RISK-BBT-17（已改未上板） |
| 2B. 掉电恢复 / 启动路径 | RISK-BOOT-06（已修未上板） · RISK-BOOT-07（已修未上板） · RISK-BOOT-08（已修未上板） · RISK-BOOT-09（已修未上板） · RISK-BOOT-10（已修未上板） · RISK-BOOT-11（已修未上板） · RISK-BOOT-14（已改未上板） · RISK-BOOT-15（已修未上板） · RISK-BOOT-16（已改未上板） · RISK-BOOT-18（已改未上板） · RISK-BOOT-21（已修未上板） · RISK-MD-14（已改未上板） · RISK-MD-15（已改未上板） |
| 2C. L2P cache | RISK-L2P-09（2026-10-31 上板验证：区域写回按 WAL 次序等 Journal） · RISK-L2P-01（已修未上板） · RISK-L2P-02（已修未上板） · RISK-L2P-04（已改未上板） · RISK-L2P-07（已改未上板） |
| 2D. Checkpoint | RISK-CP-01（已修未上板） · RISK-CP-02（已修未上板） · RISK-CP-05（已修未上板） · RISK-CP-07（已改未上板） |
| 2E. 磨损计数 / 静态 WL | RISK-WL-01（已修未上板） · RISK-WL-02（已修未上板） · RISK-WL-03（已修未上板） · RISK-WL-07（已改未上板） |
| 2F. FTL↔FC 接口 | RISK-IF-01（已修未上板） · RISK-IF-02（已修未上板） · RISK-IF-03（已修未上板） |
| 2G. Host 提交 / 完成组 / pending | RISK-HOST-01（已修未上板） · RISK-HOST-02（已修未上板） · RISK-HOST-03（已修未上板） · RISK-HOST-06（已改未上板） · RISK-HOST-07（已改未上板） |
| 2H. 完成派发面 | RISK-CR-01（已修未上板） |
| 2I. 页打包写缓冲 wbuf | RISK-WB-01（已修未上板） · RISK-WB-02（已修未上板） · RISK-WB-04（已修未上板） |
| 2J. FTL service 与后台调度器 | RISK-BG-01（已修未上板） · RISK-BG-04（已修未上板） · RISK-BG-05（已修未上板） · RISK-BG-06（已修未上板） · RISK-BG-07（已修未上板） · RISK-LOG-03（已改未上板） |
| 2L. FTL 模块 handle 契约 | RISK-ARCH-05（已改未上板） · RISK-ARCH-07（已改未上板） · RISK-ARCH-08（已改未上板） · RISK-ARCH-09（已改未上板） · RISK-ARCH-11（已改未上板） |
| 2M. USB MSC / 平台 | RISK-PLAT-01（已上板） |
| 2O. NAND / FMC 时序与吞吐 | RISK-NAND-07（已上板） |
| 2P. allocator | RISK-ALLOC-03（已改未上板） · RISK-ALLOC-07（已改未上板） · RISK-ALLOC-09（已改、板端生路通过：per-plane 水位 + victim 平衡 + 错误语义） · RISK-ALLOC-10（2026-10-31 闭合：dangling 归因 WAL） · RISK-ALLOC-08（已改未上板） |
| 2Q. FTL 类型 / 命名 / 对齐收口 | RISK-ARCH-12（已改未上板） |
| 2S. FTL cache read | RISK-CACHE-01（已改未上板） · RISK-CACHE-20（2026-10-31 上板验证：31h 跨 block；段长受分配布局限制，见 ISS-FTL-13） |
| 2T. FTL cache program | RISK-CACHE-21（2026-10-31 上板验证：写序列跨 block） · RISK-CACHE-22（2026-10-31 归因 → 24） · RISK-CACHE-24（2026-10-31 上板验证：padding page0 plane1 屏障） · RISK-CACHE-02（已改、host 正常路径上板；FAILC 负例未验） · RISK-CACHE-03（已改、正常路径上板；失败注入未做） · RISK-CACHE-04（已改、正常路径上板；HSQ/chain 未引入） · RISK-CACHE-05（已改、上板） · RISK-CACHE-06（已改、正常路径上板；掉电注入未做） · RISK-CACHE-12（已改、上板） · RISK-CACHE-13（已改、上板） · RISK-CACHE-07（已改、正常路径上板；失败归因未注入） · RISK-CACHE-08（已改、正常路径上板；失败注入未做） · RISK-CACHE-09（已改、正常路径上板；瞬态背压未注入） · RISK-CACHE-10（已改、正常路径上板；丢完成未注入） · RISK-CACHE-11（已改、上板） |
| 2U. ITCM / DTCM 热路径 | RISK-TCM-01（已上板验证） · RISK-TCM-02（已上板验证） · RISK-TCM-03（已上板验证） · RISK-TCM-04（已上板验证） · RISK-TCM-09（已上板验证） · RISK-TCM-05（已上板验证） · RISK-TCM-06（已上板验证） · RISK-TCM-08（已上板验证） · RISK-TCM-07（已判定） |
| 2V. 板端读完整性事件 | RISK-RD-02（已判定） · RISK-RD-03（已判定） |
| 2X. 日志格式化 — 编译器格式检查收口 | RISK-LOG-05（信息） |
| 2Y. UT 框架输出 | RISK-UT-02（已上板） · RISK-UT-03（已上板） |
| 2Z. cache 长流 + Journal 顺序页 | RISK-CACHE-17（2026-10-31 掉电回放验证：`boot type=2`/8,704 条/两次 `closed+free=3385`） |
| 3. 类型 / 显式强转 | RISK-TYPE-01（已修未上板） |

## 判定归档（信息 / 已议）

> 非活跃风险，保留判定与重开触发，避免下轮重复分析；不是已闭合缺陷。

> 本节条目已归档（见 `docs/archive/known_risks_batches.md` §2026-10-24）：RISK-MIG-04（信息） · RISK-BBT-08（信息） · RISK-BBT-09（信息） · RISK-BOOT-19（信息） · RISK-BOOT-20（信息） · RISK-WL-06（信息） · RISK-WL-09（信息） · RISK-WB-05（信息） · RISK-BG-02（已议） · RISK-BG-03（信息） · RISK-BG-08（信息） · RISK-ARCH-01（信息） · RISK-ARCH-03（信息） · RISK-ARCH-04（已议） · RISK-ARCH-10（信息） · RISK-USB-04（信息） · RISK-ALLOC-01（信息） · RISK-MD-04（信息） · RISK-PLAT-06（信息，2026-10-24）

- **RISK-TCM-11（信息，2026-10-31）**：release 的 ITCM 集（`.ssd_code_high`）**63,856 B / 65,536 B，余量 1,680 B**（drain stall 转储 +320 B、padding 屏障 +32 B、per-plane 水位/victim 平衡 +152 B、L2P WAL 闸门 +192 B）（v1.05 把 padding/cache-close 与 Journal 开机路径（scan/resume/replay）改放 `.ssd_code_fast`（Flash）后收窄；v1.06 读写跨 block 判据 + `alc_super_can_open` 再增 ~464 B。v1.04 曾到 64,352 B / 余量 1,184 B）。后果：余量再被吃掉时**链接期** `ASSERT(SIZEOF(.ssd_code_high) <= LENGTH(ITCM))` 会直接失败（不是静默），但排障时会阻塞构建。下一步：下次把 release-only 模块（journal / allocator / metadata / migrate 等）移出 ITCM 集之前，先复核本余量；不要靠删热路径函数换空间。证据：`arm-none-eabi-size -A build/firmware/release/bin/release.elf`；`linker/stm32h743xx_flash.ld` 的 ITCM 段与 ASSERT。

- **RISK-DOC-02（已改，2026-10-31，原为注释表述不准）**：`src/ftl/cache_batch.c:243-244` 写「同 block 是本仓的保守选择（不跨块），跨块直接回退普通读——不是器件强制」。**前半正确**：datasheet 原文「The READ PAGE CACHE SEQUENTIAL (31h) command **can be used to cross block boundaries** ... Do not issue 31h to cross die (LUN) boundaries」（本地摘录 `docs/reference/datasheet_mt29f_cache_ops.md` §2）⇒ 同 block 确实是本仓保守选择，不是器件强制。**后半不准**：断点后 drain 循环 `continue`（`src/ftl/ftl.c:2151-2154`），剩余 LBA 会**重新 START 一条 cache read**，不是回退普通读（只有下一次 START 条件不满足时才回退）。**失败场景**：只在维护者按注释误解"跨 block 必须回退普通读"时把行为描述错，不影响数据正确性（判据本身保守且安全）。已改（2026-10-31）：这两行注释已改为「46h 无地址、由器件自增；31h 可跨 block（同 die，禁止跨 die/LUN）」并把断点后行为写清楚；跨 block 续读的实现登记为 RISK-CACHE-20。证据：`src/ftl/cache_batch.c:283-284`、`src/nand/onfi_1_0.c:1028-1030`、`src/ftl/ftl.c:2151-2154`、`docs/reference/datasheet_mt29f_cache_ops.md`。

- **RISK-DOC-01（信息，2026-10-31，**既有债**，非本次引入）**：`docs/design/ftl_api_usage.md` 的非 Journal 分节仍用旧 API 名（`l2p_cache_read` / `l2p_cache_write` / `journal_init` / `journal_append` 等，现名见 `include/ftl/*.h`）。后果：读文档写代码会对不上签名（编译期即可发现，不是静默）。缓解：本次已把 **Journal 分节**改为"权威定义在 journal.h + 页格式表"，并修正 `journal.c` 同步调用计数行；其余分节按模块逐个收敛（H37 的落地节奏）。重访触发：任何一次改到这些分节的接口文档。证据：`docs/design/ftl_api_usage.md` §2.3 / §3；`include/ftl/ftl.h` / `journal.h`。

## 已闭合

> 本节条目已归档（见 `docs/archive/known_risks_batches.md` §2026-10-31）：RISK-L2P-09（2026-10-31 WAL 闸门上板验证） · RISK-ALLOC-10（2026-10-31 闭合：dangling 归因 WAL） · RISK-CACHE-17（2026-10-31 掉电回放） · RISK-CACHE-20（31h 跨 block） · RISK-CACHE-21（写序列跨 block） · RISK-CACHE-22（归因 → 24） · RISK-CACHE-24（padding page0 plane1 屏障）

> 本节条目已归档（见 `docs/archive/known_risks_batches.md` §2026-10-30）：RISK-IF-14（2026-10-30）
> 本节条目已归档（见 `docs/archive/known_risks_batches.md` §2026-10-30 闭合：cache 序列并发门控 / ITCM 野写 / MPU 只读 / erase 判据）：RISK-CACHE-14（2026-10-30） · RISK-L2P-08（2026-10-30） · RISK-TCM-10（2026-10-30） · RISK-UT-07（2026-10-30）
>
> 本节条目已归档（见 `docs/archive/known_risks_batches.md` §2026-10-25）：RISK-HOST-13（2026-10-25） · RISK-NAND-04（2026-10-25） · RISK-PEND-02（2026-10-25） · RISK-IF-05（2026-10-25） · RISK-BOOT-01（2026-10-25） · RISK-CR-03（2026-10-25） · RISK-ARCH-06（2026-10-25）

> 闭合 = 缺陷已消除且有证据；明细已移入 archive §2026-10-24，本处保留 ID + 日期索引。
> 若「重开触发」出现，按新条目重新登记。

> 本节条目已归档（见 `docs/archive/known_risks_batches.md` §2026-10-24）：RISK-LOG-04（2026-10-24） · RISK-REFACTOR-01（2026-10-24） · RISK-CAP-01（2026-10-22） · RISK-CAP-02（2026-10-22） · RISK-CAP-03（2026-10-22） · RISK-CAP-04（2026-10-22） · RISK-MD-05（2026-10-22） · RISK-MD-07（2026-10-22） · RISK-MD-08（2026-10-22） · RISK-MD-09（2026-10-22） · RISK-MD-11（2026-10-22） · RISK-MD-12（2026-10-22） · RISK-MD-13（2026-10-22） · RISK-GC-09（2026-10-22） · RISK-GC-11（2026-10-22） · RISK-GC-12（2026-10-22） · RISK-GC-13（2026-10-22） · RISK-WB-06（2026-10-22） · RISK-USB-01（2026-10-22） · RISK-USB-03（2026-10-22） · RISK-USB-06（2026-10-22） · RISK-WL-08（2026-10-22） · RISK-CP-06（2026-10-22） · RISK-CP-08（2026-10-22） · RISK-CP-09（2026-10-22） · RISK-NAND-01（2026-10-22） · RISK-NAND-02（2026-10-22） · RISK-NAND-03（2026-10-22） · RISK-HOST-10（2026-10-23） · RISK-PLAT-04（2026-10-23） · RISK-HOST-04（2026-10-23） · RISK-GC-03（2026-10-23） · RISK-LOG-01（2026-10-23） · RISK-ALLOC-02（2026-10-23） · RISK-ALLOC-04（2026-10-23） · RISK-ALLOC-05（2026-10-23） · RISK-TYPE-03（2026-10-23） · RISK-BOOT-12（2026-10-23） · RISK-PEND-01（2026-10-24） · RISK-MD-03（2026-10-24） · RISK-BG-09（2026-10-24） · RISK-PLAT-05（2026-10-24）

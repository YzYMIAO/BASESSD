# 已知残余风险登记

> **用途**：把「暂不修 / 待上板 / 待测试源同步 / 待决策」的风险固定下来。
> **复查 `metadata` / GC / migrate / journal / BBT / 掉电恢复 / L2P cache /
> Checkpoint / 磨损计数与 WL / Read Recovery / FTL↔FC 接口 / 类型规则前先读本表**，
> 避免重复分析、重复消耗。
>
> 状态词：`待决策`（需要用户裁决）· `待后续`（已登记，独立任务）·
> `待上板`（代码已改，板端未验）· `待测试源`（冻结 UT 待所有者同步）· `已修未上板`。
>
> ⚠ 本表只登记**当前仍成立**的风险；闭合后移到文末「已闭合」并注明提交。

---

## 1. metadata 模块（提交 `992985c`）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-MD-01 | 待决策 | 日志块擦除持续失败（坏块）时，回收位保留并无限重试；轮转游标只保证不饿死其它块，没有坏块策略 | `md_reclaim_take` 轮转游标 | 若要收敛：连续 N 次擦失败后 `bbt_mark_bad` + 清位；需确认 N 与账本 | `src/ftl/metadata_job.c`（`ftl_op_metadata` MD_JOB_RECLAIM 失败分支） |
| RISK-MD-02 | 待后续 | 压缩预算按 `md_compact_begin` 时的 `l2p_live[victim]` 计算；job 启动前已在飞的前台 region 写可能在扫描期间落回 victim ⇒ 超预算 ⇒ `slot_now` 返回 INVALID ⇒ 中止（**良性**：victim 不擦） | 中止路径安全；注释已写明 | 若要消除误报：扫描结束后重算预算，或预留 1 个槽位 | `src/ftl/metadata_job.c` `md_compact_begin` / `md_compact_write` |
| RISK-MD-03 | 待测试源 | `tests/unit/test_ftl.c` 仍引用已删字段/旧签名：`change_count`、`ring.plane`、`have_dynamic`、旧 `metadata_dir_commit_region` 签名、`super_block_t.valid/topo` 等；`make tests UT=FTL` 失败 | 未改测试源（R4） | 测试所有者更新后重新跑 `make tests UT=FTL` | `tests/unit/test_ftl.c` |
| RISK-MD-04 | 待后续 | H15 跨模块欠账已基本收敛：`src/ftl` 下再无手写 `FC_CW_STATUS_ERROR_MASK`（本轮收掉 read_recovery.c 3 处、bbt.c 1 处）；**剩 `src/fc/fc.c:809`** 一处，属 FC 层内部（取首个失败 desc 供日志归因，非成败判定） | 其余消费方一律走 `fc_cw_status_has_error()` | 若要彻底归零：给 FC 侧补一个「首个失败 desc」helper | `include/fc/fc_command.h`（helper）；`grep -rn FC_CW_STATUS_ERROR_MASK src` |
| RISK-MD-05 | 待上板 | 介质布局 v9（P0+P1 镜像 / Journal multi-plane 擦 / Wear 并入 CP）+ 本轮 metadata 重构均未上板 | 已有独立 memory 记录 | 见 memory `ftl-media-layout-v9-unverified.md` 的板端清单 | `docs/design/ftl_design.md §7.1` |

---

## 2. GC / migrate / journal（提交 `fdc7c7f`）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-GC-01 | 待决策 | 永久不可读 / META 页基址损坏的 victim 页会让 GC 反复 abort：数据安全，但容量不再增长 | 读错误 / `GC_DECODE_CORRUPT` ⇒ 中止，victim 不擦 | 重试上限 + 升级 RR / `bbt_mark_bad` / retire；需策略 | `src/ftl/migrate.c` `mig_complete_read`（`[待上板]` 分支） |
| RISK-GC-02 | 待后续 | 目的页写失败不标目的块 BAD：目的冷 SB 可能已有成功迁移页，整块标坏也错 | 写失败只中止 job，源数据仍有效 | 上层坏块策略：按连续失败页数/块判定，或隔离目的块剩余页 | `src/ftl/migrate.c` `mig_complete_write` |
| RISK-GC-03 | 已修未上板 | ~~TRIM 仍是 L2P-first、Journal-second，两步之间存在掉电窗口~~ | 改为 Journal-first（同 host 写路径）；见 RISK-HOST-04 的板端取证 | 上板：TRIM 后立即断电，重启确认该 LBA 不复活 | `src/ftl/ftl.c` `ftl_trim` |
| RISK-GC-04 | 待后续 | 部分 plane 已擦除后 job 中止，SB 仍留在 `closed_sb_list`；若被释放的 plane 又被重新分配并 Closed，旧条目理论上可能误认 | `mig_next_present_plane` 只接受 BST `CLOSED`，已降低概率 | 擦完一个 plane 后同步把该 plane 从 closed 条目摘除（需要 allocator 接口） | `src/ftl/migrate.c` `mig_next_present_plane` / `mig_finish` |
| RISK-MIG-01 | 待后续 | READ_SETTLE / WRITE_SETTLE 等 L2P region、FLUSH_JRN 等 Journal 时把 `stall_rounds` 清零 ⇒ 无上限等待；依赖心跳可见 | 心跳 `[MIG]` 日志 | 增加独立的 wait 计数/超时后中止（victim 不擦） | `src/ftl/migrate.c` `mig_step_read_settle` / `mig_step_write_settle` / `mig_step_flush_jrn` |
| RISK-JN-01 | 待后续 | Journal 环形块擦除失败时重新挂起 `need_erase` 并无限重试；Journal 层没有 allocator，无法标 BAD | 不再写未擦块（拒绝继续） | Journal 块坏块策略：失败 N 次后由上层标记/换块 | `src/ftl/journal.c`（`ftl_op_journal` JRN_ERASE 失败分支） |
| RISK-JN-02 | 已修未上板 | ~~`journal_flush_page` / `journal_flush_now` 返回 `void`：同步刷页失败会置 `write_fail_pending`，但调用者无法知道“屏障未达成”~~ | 改为 `ftl_status_t` / `bool`；drain 超时、擦失败、写失败均返回 `FTL_ERROR`，`ftl_flush` 记 `LOGW` | 上板：注入同步写/擦失败，`fl` 能报错且 pending 阻止 victim 擦 | `src/ftl/journal.c` `journal_flush_page` |
| RISK-PERF-01 | 待后续 | `gc_check_triggers` 与 `gc_select_victim` 各扫一次全表；`wl_should_trigger` 与 `wl_select_victim` 也各自扫描 | GC 10 ms / WL 5 s 节拍门兜成本 | 需要时把 `allocator_erase_stats_t` 从求值点透传（注意别把策略耦合进 mig API） | `src/ftl/gc.c` / `src/ftl/wear_leveling.c` |
| RISK-PERF-02 | 待后续 | GC/WL 目的页仍逐 plane 单条写，没有 multi-plane 命令 | 正确性无影响 | 命令形式优化，需 FC 侧支持 | `docs/design/ftl_design.md §7.2` |
| RISK-GC-05 | 待后续 | GC Bitmap（Layer 2）未实现：掉电后 GC 从块头重来 | 正确性由 F10 + 重试覆盖 | 性能优化项 | `docs/design/ftl_design.md §7.2` |
| RISK-RR-01 | 待后续 | `read_recovery.c` 的 `rr_step` 超 H11 NBNC（状态机 switch 较大）。~~`ftl_op_read_recovery`~~ 已完成按 stage 拆 `rr_complete_*`（2026-10-19，见 §2H）。~~`RR_STAGE_ABORT` 死枚举~~ 已删除 | 与 GC/migrate 解耦；本轮只提取邻居页计算 | 独立重构任务（`rr_step` 按 stage 拆 handler）；范围已收窄为 `rr_step` 本体 | `src/ftl/read_recovery.c`；`include/ftl/read_recovery.h` |
| RISK-RR-02 | 已修未上板 | ~~恢复成功后仍以 `SSD_STATUS_IO_ERROR` 完成托管子操作；且 `recovered` 只是最后一个 plane 的结果~~ | 新增 `all_ok` 聚合位：job 起点置 1，任一 plane 失败或 abort 清零；`rr_finish` 按它报 OK/IO_ERROR | 上板：可恢复失败读应返回成功；单 plane 不可恢复整体报错 | `src/ftl/read_recovery.c` `rr_finish` / `rr_settle_plane` |
| RISK-RR-03 | 已修未上板 | ~~`rr_record_ecc` 健康状态可降级：CRITICAL/DYING 块在低 ECC 连续上升时被打回 WARNING~~ | 改为单调 max 升级 | 上板/UT 负例：CRITICAL 后低 ECC 上升不得回落 | `src/ftl/read_recovery.c` `rr_record_ecc` |
| RISK-RR-04 | 已修未上板 | ~~预测巡检返回的 CLOSED 块所在 SB 可能含 BAD plane ⇒ migrate 反复 abort 卡死~~ | `rr_predictive_scan` 命中候选后校验 SB，含 BAD plane 则跳过 | 上板：运行期标坏后预测路径仍能推进 | `src/ftl/read_recovery.c` `rr_predictive_scan` |
| RISK-GC-06 | 待上板 | 本轮行为变化未板验：T5 交叉相乘、WL 5 s 节拍、CLOSED plane 门控、`valid_count` 单块上界、`gc_decode_cw_lba` 三态 | 编译零告警 | 按 §2 板端清单回归（GC 并发覆盖写/TRIM、断电、WL、fault injection） | 提交 `fdc7c7f` |
| RISK-JN-03 | 待上板 | `write_fail_pending` + CP 清账逻辑未做故障注入：页写失败 / 擦失败 / CP 早于失败点三种时序未实测 | 静态路径已审查 | 注入 Journal 页写失败，确认 victim 未擦、CP 后恢复 | `src/ftl/journal.c` / `src/ftl/checkpoint.c` |
| RISK-JN-04 | 已修未上板 | ~~drain 超时后 `journal_flush_page` 仍同步刷页；在飞写命令仍拥有当前页地址 ⇒ 双重编程~~ | drain 失败 ⇒ `LOGE` + 置 pending + `FTL_ERROR`，**不再**同步刷；`journal_flush_now` 入口加 `job.phase == JRN_IDLE` 守卫 | 上板：注入命令丢失/超时（drain 超时），确认不发生同页二次编程、后续可恢复 | `src/ftl/journal.c` `journal_flush_page` / `journal_flush_now` |

---

## 2A. BBT（Bad Block Table）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-BBT-01 | 待上板 | `bbt_mark_bad` 改为幂等 + `dirty_gen` 后，运行时标坏 / 重启持久化 / Spare 只减一次未实测 | 代码审查 + 编译零告警 | 上板：注入擦失败/RR 标坏，重启后 `bad_count`/`spare_count`/BBT 位一致；BBT 写飞行期间再标坏，下一次 save 必须包含新块 | `src/ftl/bbt.c` `bbt_mark_bad` / `bbt_submit_write` / `ftl_op_bbt` |
| RISK-BBT-02 | 待上板 | `ftl_sync_read_column` 现在对 NAND 错误返回 `FTL_ERROR`；工厂扫描对“读失败页”的判坏行为未实测 | BBT 侧 `!= FTL_OK ⇒ 坏块` 保守路径 | 注入列读失败，确认判坏而不是误判好块；注意空片出厂标记与 NAND 失败的区别 | `src/ftl/ftl_fc_interface.c` `ftl_sync_read_column` |
| RISK-BBT-03 | 已修未上板 | ~~格式化先擦掉工厂标记再 save BBT~~ | 已改为两阶段 `bbt_format_erase`：① 置 `FORMAT_IN_PROGRESS` 并 save 失败即中止（不擦用户块）；② 擦除 BBT 池**之外**的块；③ 再 best-effort save 并入新 BAD。BBT 池旧 record 保留，新 record 以更高 sequence 胜出 | 上板：格式化过程中掉电注入（save 后 / erase 中 / CP marker 前），重启后 BBT 仍含工厂坏块图 | `src/ftl/bbt.c` `bbt_format_erase`；`src/ftl/power_failure_recovery.c` `ftl_media_format` |
| RISK-BBT-04 | 待后续 | BBT header **无布局版本字段**；只靠 `total_blocks` 做代理。块数相同但 bitmap 语义变化的格式演进无法识别 | `bbt_header_valid` 校验 `total_blocks == FTL_BLOCK_TOTAL` | 下次 BBT 格式变更时用 `reserved[]` 加版本（属于介质格式变更，需 bump `FTL_MEDIA_LAYOUT_VERSION`） | `include/ftl/bbt.h` `bbt_header_t` |
| RISK-BBT-05 | 待后续 | `bbt_medium_has_ftl_data` 只查固定元数据区；元数据被擦、用户区仍有数据时会被误判为空片并重扫（出厂标记已被覆盖 ⇒ 坏块图丢失） | **格式化决策已不再依赖它**：`checkpoint_probe_media_status` 先 raw 读 CP 池页区分空片/读死；本谓词只剩 `bbt_init` 内部决定“是否做出厂扫描” | 若要闭合：增加用户区抽样 / 独立介质标记点 | `src/ftl/bbt.c` `bbt_medium_has_ftl_data`；`src/ftl/checkpoint.c` `checkpoint_probe_media_status` |
| RISK-BBT-06 | 待后续 | `ftl_op_bbt` 没有保存 in-flight `command_id`；擦完成与写完成若被重复投递，phase 可能误配 | `waiting==0` 的孤儿守卫只能挡 job 结束后的迟到完成 | **2026-10-19 核实**：FC 侧 `fc_complete_internal`（`fc.c:712-724`）**没有**「已 COMPLETED 就不重复入队」的守卫 ⇒ 是否重复完成完全取决于 NAND 层是否重复上报。当前 NAND 是单次轮询完成模型，无重复上报源 ⇒ 暂不修；换 NAND 引擎 / 加中断完成时**必须先补该守卫** | `src/ftl/bbt.c` `ftl_op_bbt`；`src/fc/fc.c` `fc_complete_internal` |
| RISK-BBT-07 | 已修未上板 | ~~boot 期 CP 快照并集的 BAD 不补 Spare~~ | 新增 `allocator_spare_refill_from_free(alloc, plane)`：摘 Free → state 置 NONE → push Spare，直到 `FTL_SPARE_POOL_PER_PLANE` 或无同 plane 空闲块。三条启动路径（clean 快照 / clean F2 / PF F2）建池后逐 plane 补池并打印补后计数 | 上板：制造 CP 快照含 BAD、Spare 不足的介质，确认补回 50/plane；Free 不足时自然下降 | `src/ftl/allocator.c`；`src/ftl/power_failure_recovery.c` `recovery_refill_spare_pools` |
| RISK-BBT-08 | 信息 | `bbt_scan_factory` 跳过 gblock 0，依赖“Block 0 出厂总是好块”的器件假设 | 器件手册（Micron AN1819）支持 | 换器件时复核 | `src/ftl/bbt.c` `bbt_scan_factory` |
| RISK-BBT-09 | 信息 | CLI `bbt --erase` / `bbt_print_bad_map(do_erase)` 会擦全片，且运行时 BST 不更新 | CLI 先 suspend 后台并提示断电 | 只作为专用体检工具；不要放进开机路径 | `src/ftl/bbt.c` `bbt_print_bad_map`；`src/controller/cli.c` |
| RISK-BBT-10 | 待测试源 | BBT UT 只覆盖 `mark_bad` / `spare_pool_refill` / inline bitmap；`bbt_load/save/kick/scan_factory/format_erase` 无用例；`test_ftl.c` 的“已知偏差 ④”注释在幂等修复后过期 | 未改测试源（R4） | 测试所有者补 NAND seam / 更新注释；新增 `dirty_gen`、`FORMAT_IN_PROGRESS` 中断恢复用例 | `tests/unit/test_ftl.c`；`docs/design/ftl_ut_munit.md §4.5` |

---

## 2B. 掉电恢复 / 启动路径（本轮审查，未提交）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-BOOT-01 | 待上板 | ~~F4 按 INVALID 行的 `region_id` 反查 CP bitmap ⇒ 全部别名到 region 0~~ | 新增 `l2p_cache_preload_region(cache, global_region, pba)`：由 CP bitmap **逐 bit** 找 INVALID 行、设 `region_id`、同步读、置 CLEAN/INVALID；F4 只对 resident CLEAN 行做 bitmap 失效 | 上板：region 0 与非 0 region 各预载一次，确认 `l2p_cache_find` 不返回别名、post-CP 映射可读 | `src/ftl/l2p_cache.c`；`src/ftl/power_failure_recovery.c` `ftl_boot_apply_region_bitmap` |
| RISK-BOOT-02 | 待上板 | ~~carve 先于 F2 ⇒ 写过块同时是池成员与 CLOSED 条目 ⇒ 活数据可被复用~~ | 三条路径统一 **F2/快照 → carve**；`ftl_boot_carve_pools` 只把 `state == NONE` 的块入池，其余只计入 `total_blocks` | 上板：预置数据在旧 carve 会选中的块上，走 F2 fallback 后验证不在池 | `src/ftl/power_failure_recovery.c` |
| RISK-BOOT-03 | 待上板 | ~~CP 读失败被当作“需要格式化” ⇒ 一次瞬时读错整片擦除~~ | `checkpoint_probe_media_status` 区分 `CURRENT / NO_RECORD / VERSION_MISMATCH / IO_ERROR`；`ftl_media_check_format` 对 IO_ERROR 返回 `FTL_ERROR`，`main` 拒绝格式化并停止启动 | 上板：CP 读失败注入不擦片；版本不符正常格式化 | `src/ftl/checkpoint.c`；`src/main.c` |
| RISK-BOOT-04 | 待上板 | 两阶段格式化中途掉电后，重启不能判断“格式化未完成” | `FORMAT_IN_PROGRESS` 写入 `bbt_header_t.reserved[0]`，`bbt_load` 恢复；`checkpoint_probe_media_status` 见该标志返回 NO_RECORD ⇒ 继续格式化 | 上板：save 后 / erase 中 / CP marker 前各断电一次 | `src/ftl/bbt.c`；`src/ftl/checkpoint.c` |
| RISK-BOOT-05 | 待上板 | 快照/恢复路径的校验与计数改动未板验：restore Pass 0 全量校验（不再部分应用 BAD）、`recovery_materialize_pba` 校验 ch/ce/lun、`recovery_add_closed` 传播列表满、`valid_cw` 改 `uint32_t` | 编译零告警；失败路径回退 F2 | 上板：快照中途注入非法 word（不留下部分 BAD）；journal 注入越界 PBA（回退 F2）；满盘重建确认 `valid_cw` 不截断 | `src/ftl/power_failure_recovery.c` |
| RISK-BOOT-06 | 待后续 | F2 把“第一个空白页”当写指针：中间若因编程失败留下 hole，hole 之后的页不会被 recount（valid_count 少算） | 写路径失败通常关闭 SB，hole+后续写概率低 | 若要闭合：F2 扫全块 page 0..N；代价是启动时间随块数线性增长 | `src/ftl/power_failure_recovery.c` `ftl_recovery_scan_write_pointer` |
| RISK-BOOT-07 | 待后续 | `ftl_recovery_page_is_blank` 读的是 raw USER 区（DATA 页已加扰），不是 META/CRC；依赖当前页面配置 | 当前 user 页走 DATA 加扰 ⇒ 空页全 FF 成立 | 若引入 `RANDOM_DISABLE` 且用户页前缀全 FF，判空会失效 ⇒ 改为读 META/CRC | `src/ftl/power_failure_recovery.c` `ftl_recovery_page_is_blank` |
| RISK-BOOT-08 | 待后续 | `journal_scan_latest` / `journal_replay` 的读失败不向上传播：坏页被当作“无更多条目”，可能少回放 | **读失败现在有 `LOGW`**（scan 汇总一条 / replay 逐页）；回放按 `(page_seq, entry_idx)` 单调过滤 | 统一到 journal 的读错误策略（重试/隔离），属跨模块改动 | `src/ftl/journal.c` |
| RISK-BOOT-09 | 待决策 | `metadata_dir_load` 失败时静默回退到新建 Directory：旧 region 映射可能整体丢失，但启动继续 | 失败后有 LOGW（无返回码） | 决定是否把“Directory 读失败”升级为启动失败 / 重扫 | `src/ftl/metadata.c` `metadata_dir_load` |
| RISK-BOOT-10 | 已修未上板 | ~~`journal_resume` 恢复写态时忽略跨块擦除结果：擦失败后首次写会落在未擦页~~ | 擦失败 ⇒ `LOGE` + 重新挂起 `job.need_erase`（在 job 复位之后写入）；`journal_kick` / `flush_now` 先补擦，`journal_freeze` 在 `need_erase != 0` 时拒绝写 | 上板：注入 resume 跨块擦失败，首次写前必须补擦成功 | `src/ftl/journal.c` `journal_resume` |
| RISK-BOOT-11 | 待后续 | 快照 probe 的 `l2p_cache_read_sync` 失败只记 LOGW 并计入 `anomalies`，`blocks_restored` 仍为 1 ⇒ valid_count 少算 | 读失败日志可见；不触发 F2 | 若读失败频繁，升级为 F2 或按块隔离 | `src/ftl/power_failure_recovery.c` `ftl_recovery_probe_blocks` |

---

## 2C. L2P cache（本轮审查）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-L2P-01 | 待后续 | region 页加载失败时行被置 CLEAN + entries 全 `NAND_PBA_INVALID`（= 该 region 全部未映射），host 读到零填充；映射在行被驱逐前**静默不可用**，且不升级到 read recovery | 避免「坏 region 每次访问都重读 + host 永久停在 FTL_AGAIN」的活锁；行为与「从未写过」一致 | 接入 RR 后：失败改为 RR / 标块坏，或按 region 限次重试后再降级 | `src/ftl/l2p_cache.c` `ftl_op_l2p`（LOADING 失败分支） |
| RISK-L2P-02 | 已修未上板 | ~~先取槽后建链，命令池空重试浪费 L2P 日志槽~~ | `flush_kick` 在 `append_slot` 前用 `fc_get_resource_stats()` 预检命令/描述符/buffer/SQ/HSQ；不足即返 `FTL_AGAIN`，不消耗槽位 | 上板：命令池高压下长时写，日志块换块频率不异常 | `src/ftl/l2p_cache.c` `l2p_flush_resources_available` |
| RISK-L2P-03 | 待上板 | 本轮行为变化未板验：`flush_kick` 背压改报 `FTL_AGAIN`、写回前资源预检、CLOCK 引用位置 1、加载失败降级为全未映射 | 编译零告警 | 上板：命令池压力下 `fl`/Checkpoint 不中止且不烧日志槽；热点 region 不被长期钉住；坏 region 读不活锁 | `src/ftl/l2p_cache.c` |

---

## 2D. Checkpoint（本轮审查）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-CP-01 | 已修未上板 | ~~journal 预擦失败仍记 `pre_erased_block` ⇒ journal 回切时跳过擦除、写进未擦块~~ | 失败改 `journal_truncate_commit(JOURNAL_BLOCK_NONE)`：只复位水位、不记账；回切时由 journal 的 `need_erase` 重试，下一次 CP 的 plan 也会再报 NEEDS_ERASE | 上板：擦除失败注入，确认不跳过擦除、CP 不因 T1 反复触发 | `src/ftl/checkpoint.c` `ftl_op_checkpoint`（CP_TRUNC_ERASE） |
| RISK-CP-02 | 已修未上板 | ~~`ftl_op_checkpoint` 无 `waiting` 守卫：重复/迟到完成会按当前 phase 再走一遍~~ | 入口 `ASSERT(waiting)` + release 拒绝（LOGE + release + return） | FC 重复完成注入 / 上板回归 | `src/ftl/checkpoint.c` `ftl_op_checkpoint` |
| RISK-CP-03 | 待上板 | 本轮状态机改动未板验：提交状态传播（FTL_ERROR 立即中止）、`find_latest` 失败复位游标、水位公式精确到条目 | 编译零告警 | 正常 CP、各 phase 掉电、journal 回切、85%/95% 触发点 | `src/ftl/checkpoint.c` |

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
| RISK-IF-02 | 已修未上板 | ~~`ftl_sync_submit_wait` 超时摘表后命令迟到完成 ⇒ dispatch `take` 得 NULL ctx，`ftl_op_sync_wait` 解引用 NULL + 命令槽位泄漏~~ | `ftl_op_sync_wait` 对 `ctx == NULL`：LOGE + `fc_command_release` | 上板/故障注入：超时后迟到完成不 HardFault、池不泄漏 | `src/ftl/ftl_op.c` |
| RISK-IF-03 | 已修未上板 | ~~同步原语忽略数据搬运返回值（写失败仍编程、读失败仍 OK）；raw 读缺 CW 静默跳过；短链读返回 OK 且尾部未初始化~~ | 三处均检查返回值并返回 `FTL_ERROR` | 上板/故障注入 | `src/ftl/ftl_fc_interface.c` |
| RISK-IF-04 | 待上板 | 本轮行为变化未板验：plane 1 迁移数据、wbuf 稀疏页、越段位图 release 拒绝、迟到完成路径 | 编译零告警（release 两条 unused 已消失） | 按 §5 板端清单回归 | `src/ftl/ftl_fc_interface.c` / `ftl_op.c` |
| RISK-IF-05 | 待上板 | 旁表容量 `COMPLETION_TABLE_SIZE` 32 → `FC_COMMAND_POOL_DEPTH`（64）：**修复**原容量小于真实上界（表按 `command_id` 键，上界 = 在飞命令数 ≤ 池深）⇒ 原「满则 `FTL_ERROR`」路径实为可达，且 `bbt.c` 会把它当永久失败吞掉 BBT save | `src/ftl/completion_table.c` 加 `_Static_assert(COMPLETION_TABLE_SIZE >= FC_COMMAND_POOL_DEPTH)`；RAM 代价 +256 B（8 B/项 × 32 项） | 上板：观察 `[FTL]` 心跳无异常；`completion_table_count` 上界变 64 | `include/ftl/completion_table.h` / `src/ftl/completion_table.c` |

---

## 2G. Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`，本轮审查，未提交）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-HOST-01 | 已修未上板 | ~~host 读成败判据只看 `FC_CW_STATUS_NAND_FAIL`：ECC 不可纠只置 `UNC`、页内 META 不符只置 `MATA_ERROR`，两者的 `NAND_FAIL` 恒清 ⇒ 加扰垃圾按 `SSD_STATUS_OK` 交给 host~~ | 改为 `fc_cw_status_has_error(completion->status)`（全集）；失败分支本就走「托管给 RR」，无需新增逻辑 | **上板取证**：造一次 ECC 不可纠 / MATA 读（写某 LBA → 擦掉其映射所指块 → 读该 LBA），确认 host 得 `IO_ERROR` 且 `[RR]` 心跳可见 | `src/ftl/ftl.c` `ftl_op_host_io`；判据 `include/fc/fc_command.h` `fc_cw_status_has_error` |
| RISK-HOST-02 | 待后续 | host 读成功路径的 `read_count++` / `rr_record_ecc` 只记 `ftl_cmd_pba(cmd)` 的**首个 desc** 所在 gblock；一个子操作可覆盖 2 个 plane，Policy B 下两 plane 是**独立块** ⇒ 第二块的 read-disturb / ECC 趋势漏记 | 无数据损失，只影响慢死预测准确度（搬移阈值要 2× 读数才到） | 需 FTL 侧逐 plane PBA 查询（现只有 `ftl_cmd_pba`；逐 plane 要摸 FC desc 链，属 H13 跨层）⇒ 待后续 | `src/ftl/ftl.c` `ftl_op_host_io` |
| RISK-HOST-03 | 已修未上板 | ~~`ftl_cmd_read_data` 返回值被丢弃：desc 链比请求短时尾部不写，未初始化缓冲被当数据交给 host~~ | 检查返回值：失败 ⇒ `LOGE` + `SSD_STATUS_IO_ERROR`，并跳过该次的 ECC/read_count 记账 | 上板/故障注入：短链读必须报错 | `src/ftl/ftl.c` `ftl_op_host_io` |
| RISK-HOST-04 | 已修未上板 | ~~`ftl_trim` 是 L2P-first / Journal-second，两步之间掉电 ⇒ 回放重放更早的写条目把已 TRIM 的 LBA 复活~~ | 改为 Journal-first（`journal_append` 早于 `l2p_cache_write_sync`），与 host 写路径 `ftl_confirm_write` 同序 | 上板：TRIM 后立即断电，重启确认该 LBA 不复活 | `src/ftl/ftl.c` `ftl_trim` |

---

## 2H. 完成路由与 op dispatch（`completion_table.c` + `ftl_op.c`，本轮审查）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-CR-01 | 已修未上板 | ~~`completion_table_add` 只找首个空槽、**不按 `command_id` 去重** ⇒ 旁表可存在两条同 id 项；而 `completion_table_take` 取**首个**匹配 ⇒ 泄漏之后 id 被复用，服务函数拿到**别人的 ctx**（可能是早已失效的 `cp_job_t *` / 栈上 `ftl_sync_waiter_t *`），本命令自己的表项永久泄漏~~。可达路径：`fc.c` CQ 满强制置 COMPLETED（注释自陈「CQ entry 丢失，FTL 无法感知」）⇒ 表项泄漏 → `fc_command_alloc` 复用 id → 再次 add | `completion_table_add` 改两遍扫：先全表查 `cmd_id`，命中则**原地覆盖 ctx** + `LOGW`；未命中才取空槽 ⇒ 「同一 cmd_id 至多一条表项」收进**唯一闸门**（H10） | **不可直接构造**（需 CQ 满 / 丢完成）⇒ 静态收口 + 新增 `LOGW` 使一旦发生即可见；随常规回归上板，**不宣称已取证** | `src/ftl/completion_table.c` `completion_table_add` |
| RISK-CR-02 | 待上板 | 坐标类改动未板验：`completion_table_route_region` 的 dim→(ch,ce,lun) 分解改走 `ftl_dim_ch/ce/lun`（与 `lba_route_*` 收敛为同一套公式）。坐标属「编译期无声的三类量」 | 分解式与原手写式逐字同值；新增三个 `static inline` 纯函数（H42） | **必须上板**：跑一遍读写 + `map <lba>` | `include/ftl/l2p_cache.h` / `src/ftl/completion_table.c` |
| RISK-CR-03 | 待上板 | 9 个服务函数按 phase 拆 handler（纯代码搬移）+ 注释收敛（H37）；`ftl_op_checkpoint` 额外把 `release + waiting=0` 提到 switch 之前（8 个 case 同构，`cp_abort` 不碰 `waiting`） | 判据 / 顺序 / 日志文案逐字不变；`switch` 的 `default` 副作用留在 dispatcher（§四） | 随常规回归上板（行为应逐字等价） | `src/ftl/{ftl,journal,bbt,read_recovery,checkpoint,metadata_job}.c` |

---

## 2I. 页打包写缓冲 wbuf（`wbuf.c` + `wbuf.h`，本轮审查）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-WB-01 | 已修未上板 | ~~`ftl_wbuf_flush` 取到页对后 4 条失败返回（命令池满 / 建链 / pending 满 / SQ 满）**丢弃**该页对，而 allocator 无归还 API ⇒ 缓冲保留、每轮重试**白烧 2 页**，表现为容量无声流失（CLAUDE.md §3.1）~~ | 资源次序改为**按有无归还路径排序**（命令槽先于页对）+ 新增 `allocator_rewind_pair` + 统一错误出口归还页对 | **必须上板**：`next_page` 属地址/容量类量（§3.2 编译期无声）。背压下命令池满不易人为构造 ⇒ 静态收口 + 可达路径结构性收窄，**不宣称已取证** | `src/ftl/wbuf.c` `ftl_wbuf_flush`；`src/ftl/allocator.c` `allocator_rewind_pair` |
| RISK-WB-02 | 已修未上板 | ~~TRIM 清空窗口的**内部不变量**（清 `occupied` + 退 `owners` 在飞数 + 复位）由 `ftl_trim`（外部模块）手工维护；`cli.c` 的 `bbt --erase` 直接 `memset(&ftl->wbuf,…)` 丢弃 `owners` ⇒ 那些写组在飞数永不归零、host 写挂死~~ | 不变量收进 wbuf 模块：新增 `ftl_wbuf_drop`（TRIM）/ `ftl_wbuf_reset`（介质擦净，按写失败收尾）；调用点改为调接口（§3.1 / C3 / H13） | 上板：`trim` 走新路径不回归；`bbt --erase` 后不再有写组挂死 | `src/ftl/wbuf.c`；`src/ftl/ftl.c` `ftl_trim`；`src/controller/cli.c` |
| RISK-WB-03 | 待上板 | 模块抽出（`ftl.c` §4 → `wbuf.c`/`wbuf.h`）+ 位运算收敛到 `ftl_cw_bit` + hot-path 日志降 `LOGD`：纯搬移与等价改写，但 wbuf 是写路径核心 | 逐段比对搬出前后函数体一致；`make all` / `make release` 零新告警 | 随常规回归上板（`wr`/`rd`/`fl`/`trim` + `wp <lba> 1 8` 合并仍为 1 次编程） | `src/ftl/wbuf.c` / `include/ftl/wbuf.h` |
| RISK-WB-04 | 待后续 | `ftl->groups[gi].outstanding++` 是**跨模块字段写**（§3.1 形态）：写点 `wbuf.c`（append）与 `ftl.c`（`ftl_confirm_write`），递减点在 `ftl.c` 与 `pending.c` —— 组在飞数这一不变量由多个模块各自手工维护，任一处漏掉即 host 写组挂死 | 目前两处写点都紧邻 `pending_group_*` 调用，人工可核；本轮**未**加闸门（不在批准范围） | 若再出现第三个写点 ⇒ 加 `pending_group_add_subop()` 唯一闸门，与 `pending_group_complete_subop*` 配对（H10 / §3.1） | `src/ftl/wbuf.c` `ftl_wbuf_append`；`src/ftl/ftl.c` `ftl_confirm_write`；`src/ftl/pending.c` |
| RISK-WB-05 | 信息 | `ftl_init` 的 `memset(&ftl->wbuf, 0, sizeof(ftl->wbuf))` **有意保留**，不换成 `ftl_wbuf_reset`：init 期构造，`g_ftl` 在 `.bss` 已零；换 `ftl_wbuf_reset` 会在 `pending_init` 已清组表后带着陈旧 `owners` 调 `pending_group_complete_subop_multi` ⇒ 风险大于收益 | 有意不改（P5 留痕） | 若 `ftl_init` 的顺序变化（`pending_init` 之前有 wbuf 写入）需重估 | `src/ftl/ftl.c` `ftl_init` |

---

## 3. 类型 / 显式强转（H44，提交 `6050703`）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-TYPE-01 | 已修未上板 | ~~release 下 `ftl_fc_interface.c` 的 `allowed` / `plane_bits` 只在 `ASSERT` 里使用 ⇒ 检查被裁空、两条 unused 告警；注释声称的 FC 侧 release 拒绝已不存在（FC 建链校验全 ASSERT）~~ | `ftl_cmd_validate_cw_bitmap` 改为返回 `ftl_status_t`：ASSERT + release 运行期拒绝（空集 / 越段 / multi-plane 未覆盖每个 plane）；两个建链入口统一处理 | 上板：release 下越段位图应被拒绝而不是发空链 | `src/ftl/ftl_fc_interface.c` |
| RISK-TYPE-02 | 待后续 | 与 RISK-MD-04 同一项 H15 欠账，不重复登记 | — | 见 RISK-MD-04 | — |
| RISK-TYPE-03 | 待后续 | `cmd->meta = (DWORD)<lba_t>` 是**同类型强转**（`DWORD` ≡ `uint32` ≡ `uint32_t` ≡ `lba_t`，见 `ssd_types.h`）⇒ 按 H43 属可删；但这是**全仓既有习语**（`ftl.c`、`ftl_fc_interface.c`、`wbuf.c` 三处同款），只改一处反而不一致（S1） | 逐处删等于制造风格分裂，暂不动 | 待一次全局收敛（连同 `cmd->meta` 的字段类型一起看，可能应由 H44 从生产者侧收窄） | `grep -rn 'meta\s*=\s*(DWORD)' src/ftl` |

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
- 掉电恢复（本次审查）：F4 逐 region 预载、F2→carve、CP 介质状态三态、格式化中断恢复、
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
  （`allocator_rewind_pair`）、TRIM/复位的不变量收进 `ftl_wbuf_drop`/`ftl_wbuf_reset`、
  位运算收敛到 `ftl_cw_bit`、flush 日志降 `LOGD`；见 RISK-WB-01~03。
  ⚠ **RISK-WB-01 必须上板**（`next_page` = 编译期无声的地址/容量类量）；背压路径不易构造，
  只做静态收口，**不宣称已取证**。`trim` 走新 `ftl_wbuf_drop` 路径，**必测**。
- 板端最小回归：格式化启动 → 顺序写满多个 SB → GC → 并发覆盖写/TRIM → 断电重启 → WL →
  Journal/BBT 故障注入 → 格式化中途断电 → 长时 soak 后全量读回。

---

## 已闭合

（暂无。闭合时在此记录 ID、提交、验证证据。）

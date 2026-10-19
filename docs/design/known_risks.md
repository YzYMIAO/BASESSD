# 已知残余风险登记

> **用途**：把「暂不修 / 待上板 / 待测试源同步 / 待决策」的风险固定下来。
> **复查 `metadata` / GC / migrate / journal / BBT / 掉电恢复 / L2P cache /
> Checkpoint / 类型规则前先读本表**，避免重复分析、重复消耗。
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
| RISK-MD-04 | 待后续 | H15 跨模块欠账：`FC_CW_STATUS_ERROR_MASK` 手写仍有 ~13 处；本仓已有 `fc_cw_status_has_error()` | metadata_job.c 已收敛到 helper | 全局机械替换为 `fc_cw_status_has_error()` | `include/fc/fc_command.h`（helper）；`grep -rn FC_CW_STATUS_ERROR_MASK src/ftl` |
| RISK-MD-05 | 待上板 | 介质布局 v9（P0+P1 镜像 / Journal multi-plane 擦 / Wear 并入 CP）+ 本轮 metadata 重构均未上板 | 已有独立 memory 记录 | 见 memory `ftl-media-layout-v9-unverified.md` 的板端清单 | `docs/design/ftl_design.md §7.1` |

---

## 2. GC / migrate / journal（提交 `fdc7c7f`）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-GC-01 | 待决策 | 永久不可读 / META 页基址损坏的 victim 页会让 GC 反复 abort：数据安全，但容量不再增长 | 读错误 / `GC_DECODE_CORRUPT` ⇒ 中止，victim 不擦 | 重试上限 + 升级 RR / `bbt_mark_bad` / retire；需策略 | `src/ftl/migrate.c` `mig_complete_read`（`[待上板]` 分支） |
| RISK-GC-02 | 待后续 | 目的页写失败不标目的块 BAD：目的冷 SB 可能已有成功迁移页，整块标坏也错 | 写失败只中止 job，源数据仍有效 | 上层坏块策略：按连续失败页数/块判定，或隔离目的块剩余页 | `src/ftl/migrate.c` `mig_complete_write` |
| RISK-GC-03 | 待后续 | TRIM 仍是 L2P-first、Journal-second，两步之间存在掉电窗口 | 既有行为；非本轮范围 | 统一为 Journal-first（同 host 写路径） | `src/ftl/ftl.c` `ftl_trim` |
| RISK-GC-04 | 待后续 | 部分 plane 已擦除后 job 中止，SB 仍留在 `closed_sb_list`；若被释放的 plane 又被重新分配并 Closed，旧条目理论上可能误认 | `mig_next_present_plane` 只接受 BST `CLOSED`，已降低概率 | 擦完一个 plane 后同步把该 plane 从 closed 条目摘除（需要 allocator 接口） | `src/ftl/migrate.c` `mig_next_present_plane` / `mig_finish` |
| RISK-MIG-01 | 待后续 | READ_SETTLE / WRITE_SETTLE 等 L2P region、FLUSH_JRN 等 Journal 时把 `stall_rounds` 清零 ⇒ 无上限等待；依赖心跳可见 | 心跳 `[MIG]` 日志 | 增加独立的 wait 计数/超时后中止（victim 不擦） | `src/ftl/migrate.c` `mig_step_read_settle` / `mig_step_write_settle` / `mig_step_flush_jrn` |
| RISK-JN-01 | 待后续 | Journal 环形块擦除失败时重新挂起 `need_erase` 并无限重试；Journal 层没有 allocator，无法标 BAD | 不再写未擦块（拒绝继续） | Journal 块坏块策略：失败 N 次后由上层标记/换块 | `src/ftl/journal.c`（`ftl_op_journal` JRN_ERASE 失败分支） |
| RISK-JN-02 | 已修未上板 | ~~`journal_flush_page` / `journal_flush_now` 返回 `void`：同步刷页失败会置 `write_fail_pending`，但调用者无法知道“屏障未达成”~~ | 改为 `ftl_status_t` / `bool`；drain 超时、擦失败、写失败均返回 `FTL_ERROR`，`ftl_flush` 记 `LOGW` | 上板：注入同步写/擦失败，`fl` 能报错且 pending 阻止 victim 擦 | `src/ftl/journal.c` `journal_flush_page` |
| RISK-PERF-01 | 待后续 | `gc_check_triggers` 与 `gc_select_victim` 各扫一次全表；`wl_should_trigger` 与 `wl_select_victim` 也各自扫描 | GC 10 ms / WL 5 s 节拍门兜成本 | 需要时把 `allocator_erase_stats_t` 从求值点透传（注意别把策略耦合进 mig API） | `src/ftl/gc.c` / `src/ftl/wear_leveling.c` |
| RISK-PERF-02 | 待后续 | GC/WL 目的页仍逐 plane 单条写，没有 multi-plane 命令 | 正确性无影响 | 命令形式优化，需 FC 侧支持 | `docs/design/ftl_design.md §7.2` |
| RISK-GC-05 | 待后续 | GC Bitmap（Layer 2）未实现：掉电后 GC 从块头重来 | 正确性由 F10 + 重试覆盖 | 性能优化项 | `docs/design/ftl_design.md §7.2` |
| RISK-RR-01 | 待后续 | `read_recovery.c` 的 `rr_step` / `ftl_op_read_recovery` 超 H11 NBNC；`RR_STAGE_ABORT` 是死枚举（`rr_abort` 设 FINISH） | 与 GC/migrate 解耦 | 独立重构任务 | `src/ftl/read_recovery.c`；`include/ftl/read_recovery.h` |
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
| RISK-BBT-06 | 待后续 | `ftl_op_bbt` 没有保存 in-flight `command_id`；擦完成与写完成若被重复投递，phase 可能误配 | `waiting==0` 的孤儿守卫只能挡 job 结束后的迟到完成 | 若 FC 层可能出现重复完成，保存 command_id 并比对 | `src/ftl/bbt.c` `ftl_op_bbt` |
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

## 3. 类型 / 显式强转（H44，提交 `6050703`）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-TYPE-01 | 待决策 | release 下 `ftl_fc_interface.c` 的 `allowed` / `plane_bits` 只在 `ASSERT` 里使用 ⇒ 两条检查被裁空；多 plane 位图覆盖 / 越段是否完全由 FC 侧 release 拒绝兜住，需确认 | FC 侧另有 `INVALID_ARGUMENT` 释放拒绝（注释声明） | 按 P3/判据 10：确认后决定是否补 release 拒绝，或把变量改成 `(void)` | `src/ftl/ftl_fc_interface.c`（release 既有两个 unused-variable 告警） |
| RISK-TYPE-02 | 待后续 | 与 RISK-MD-04 同一项 H15 欠账，不重复登记 | — | 见 RISK-MD-04 | — |

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
- 板端最小回归：格式化启动 → 顺序写满多个 SB → GC → 并发覆盖写/TRIM → 断电重启 → WL →
  Journal/BBT 故障注入 → 格式化中途断电 → 长时 soak 后全量读回。

---

## 已闭合

（暂无。闭合时在此记录 ID、提交、验证证据。）

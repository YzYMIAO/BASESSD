# 已知残余风险登记

> **用途**：把「暂不修 / 待上板 / 待测试源同步 / 待决策」的风险固定下来。
> **复查 `metadata` / GC / migrate / journal / 类型规则前先读本表**，避免重复分析、重复消耗。
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
| RISK-JN-02 | 待后续 | `journal_flush_page` / `journal_flush_now` 返回 `void`：同步刷页失败会置 `write_fail_pending`，但调用者无法知道“屏障未达成” | pending 阻止 GC；页缓冲保留 | 让同步屏障返回状态，HIL `fl` / 开机路径可观测 | `src/ftl/journal.c` `journal_flush_now` |
| RISK-PERF-01 | 待后续 | `gc_check_triggers` 与 `gc_select_victim` 各扫一次全表；`wl_should_trigger` 与 `wl_select_victim` 也各自扫描 | GC 10 ms / WL 5 s 节拍门兜成本 | 需要时把 `allocator_erase_stats_t` 从求值点透传（注意别把策略耦合进 mig API） | `src/ftl/gc.c` / `src/ftl/wear_leveling.c` |
| RISK-PERF-02 | 待后续 | GC/WL 目的页仍逐 plane 单条写，没有 multi-plane 命令 | 正确性无影响 | 命令形式优化，需 FC 侧支持 | `docs/design/ftl_design.md §7.2` |
| RISK-GC-05 | 待后续 | GC Bitmap（Layer 2）未实现：掉电后 GC 从块头重来 | 正确性由 F10 + 重试覆盖 | 性能优化项 | `docs/design/ftl_design.md §7.2` |
| RISK-RR-01 | 待后续 | `read_recovery.c` 的 `rr_step` / `ftl_op_read_recovery` 超 H11 NBNC；`RR_STAGE_ABORT` 是死枚举（`rr_abort` 设 FINISH） | 与 GC/migrate 解耦 | 独立重构任务 | `src/ftl/read_recovery.c`；`include/ftl/read_recovery.h` |
| RISK-GC-06 | 待上板 | 本轮行为变化未板验：T5 交叉相乘、WL 5 s 节拍、CLOSED plane 门控、`valid_count` 单块上界、`gc_decode_cw_lba` 三态 | 编译零告警 | 按 §2 板端清单回归（GC 并发覆盖写/TRIM、断电、WL、fault injection） | 提交 `fdc7c7f` |
| RISK-JN-03 | 待上板 | `write_fail_pending` + CP 清账逻辑未做故障注入：页写失败 / 擦失败 / CP 早于失败点三种时序未实测 | 静态路径已审查 | 注入 Journal 页写失败，确认 victim 未擦、CP 后恢复 | `src/ftl/journal.c` / `src/ftl/checkpoint.c` |

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
- 板端最小回归：格式化启动 → 顺序写满多个 SB → GC → 并发覆盖写/TRIM → 断电重启 → WL → Journal 故障注入 → 长时 soak 后全量读回。

---

## 已闭合

（暂无。闭合时在此记录 ID、提交、验证证据。）

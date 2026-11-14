# known_risks 历史批次归档

> **历史批次冻结（2026-11-12）**：风险详情已按模块合并回 `docs/design/known_risks.md`
> （索引 + 详情块 + 各模块已闭合表）。本文件的历史批次**不再重写**；
> 新闭合条目的详情只追加到文末「新增已闭合」分节（只追加，不改写）。


> 从 `docs/design/known_risks.md` 移出的历史批次，保留追溯，不再作为当前风险表使用。

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
| GC-01 | 同一 victim 的**每个 plane 源块**连续读失败 3 次 ⇒ 标 BAD + 从 Closed 条目摘除该 plane，GC 换候选。计数跨 job 保留（`mig_start` 保存/恢复）；**单页读成功不清零**，只在整块迁移成功（`mig_finish`）或标 BAD 时清，否则 3 次阈值永不生效 | `MIG_VICTIM_READ_FAIL_MAX`；`mig_handle_t.fail_gblock[PLANE_NUM]` / `fail_count[PLANE_NUM]`；`src/ftl/migrate.c` `mig_complete_read` / `mig_start` / `mig_finish` | 造不可纠读：第 3 次后容量继续增长，GC 不再空转（RISK-GC-07 的根因） |
| GC-07 | 单页读成功不再清零读失败计数；计数按 plane 源块分开 ⇒ 坏页可稳定累计到 3 次并隔离，满盘/近满盘 GC 不再永久重选同一 victim | `src/ftl/migrate.c` `mig_complete_read`；`include/ftl/migrate.h`；`src/ftl/ftl.c` `mig_is_fail_state_clean` | 上板 H2testw 写到 ≥2/3：`mark BAD` 后 `reclaimed` 增长、`free` 回升 |
| GC-02 | 目的页写失败 ⇒ **立即** `alc_close_open_sb(COLD)`，**不**标 BAD（已有成功迁移页）；失败页可能半编程，不能 rewind，关闭后失败页留在 SB 尾部，下一次 GC 换新目的块 | `src/ftl/migrate.c` `mig_dst_fail_cleanup` / `mig_complete_write` | 注入目的页写失败：关闭 Cold SB、已迁移页仍可读、下一页不再写同一 SB |
| GC-03 | 目的页写提交失败 / stall 超时：本页对无任何 plane 编程 ⇒ `alc_rewind_pair(COLD)`；前序 plane 已编程 ⇒ 关闭 Cold SB。不得在同一 Open SB 上继续写下一页（否则留下 hole） | `src/ftl/migrate.c` `mig_dst_fail_cleanup` / `mig_step_write`；`allocator.c` `alc_rewind_pair` | 注入提交超时：plane0 场景页对回退、plane1 场景关 Cold SB |
| GC-04 | 每擦完一个 plane 即从 Closed 条目摘除（`alc_closed_drop_plane`），防止部分擦除后旧条目误认 | `src/ftl/allocator.c`；`mig_complete_erase` | 两 plane victim 中途断电/中止：重启后旧条目不含已释放 plane |
| GC-05 | `waiting` 期间也 arm `FTL_WAIT_TIMEOUT_MS` deadline；完成丢失时 abort，不再永久 `active=true`；写阶段超时同时关闭 Cold SB（防 hole）。迟到的完成由回调的 `(!active || !waiting)` 分支 release | `src/ftl/migrate.c` `cmd_wait_ms` / `mig_step`；`fc.c` CQ-full 丢完成 | 注入完成丢失：GC/WL 超时中止并恢复 |
| GC-06 | `ftl_migrate_callback` 增加 `ctx == NULL` 早退（重复/迟到完成只 release 命令）；对齐 RR 同族守卫 | `src/ftl/migrate.c` `ftl_migrate_callback`；`read_recovery.c` 同族写法 | 注入重复完成：release 命令，不解引用 NULL |
| MIG-01 | 等 L2P 加载 / Journal 刷页用独立 ms deadline（`FTL_WAIT_TIMEOUT_MS`），超时 abort（victim 不擦） | `src/ftl/migrate.c` `mig_wait` | 注入 region 加载永久卡住：job 超时中止而非永久挂起 |
| JN-01 | 环形块连续擦除失败 3 次 ⇒ 标坏（RAM + BBT 两个 plane）+ 跳到下一可用块；全坏则 `ring_full` 停止写入 | `JRN_ERASE_FAIL_MAX`；`journal_ring_block_bad` / `journal_erase_failed` | 注入环形块擦失败：journal 用剩余块继续；重启后 BBT 仍挡住坏块 |
| BOOT-06 | full metadata rebuild 写指针检测 hole：空白页后仍有写过页 ⇒ 返回全块，逐页反查 valid_count | `ftl_recovery_scan_write_pointer` | 造 hole（page 0 写失败、后续页成功）：valid_count 不少算 |
| BOOT-07 | 判空改看每 CW 的 META（4B，明文），不再依赖 USER 区加扰 | `ftl_recovery_page_is_blank` | 空片/写满块判空仍正确；RANDOM_DISABLE 不受影响 |
| BOOT-08 | Journal 扫描读失败向上传播：判型走掉电路径，回放读失败返回 `FTL_ERROR` ⇒ full metadata rebuild | `jrn_scan_latest(..., out_read_error)` / `jrn_replay`；`ftl_boot_recovery` | 注入 journal 页读失败：不复用“无更多条目”，启动回退 full metadata rebuild |
| BOOT-09 | Directory 加载失败不再静默建新表：介质有有效 CP 时拒绝启动（空片/格式化后才放行） | `ftl->md_load_failed`；`ftl_boot_recovery` | Anchor 损坏 + 有效 CP：启动停止并 LOGE，不丢映射 |
| BOOT-11 | probe 的 L2P 反查失败计数向上传播，非 0 ⇒ 快照结果作废、回退 full metadata rebuild | `pfr_rebuild_stats_t.l2p_read_errors`；`ftl_recovery_probe_blocks` | 注入 L2P 读失败：不声称 `blocks_restored=1` |
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
- **GC-02**：关闭 Cold SB 依赖 `alc_close_open_sb`；若 Cold SB 已被关闭，
  该调用返回 `FTL_ERROR`（no-op），下一次 GC 仍可能选到同一目的块。

---


---

## 5. 本轮已修但未上板（板端回归清单）

> ⚠ **2026-10-22 全盘 H2testw（415/423 MB 写+校验，`bad=0`）已覆盖下列多数项**；
> 已满足证据的条目已移到文末 §已闭合。本节保留的是尚未被该回归覆盖的部分。

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
  （`alc_rewind_pair`）、TRIM/复位的不变量收进 `wbuf_handle(DROP)`/`wbuf_handle(RESET)`、
  位运算收敛到 `ftl_cw_bit`、flush 日志降 `LOGD`；见 RISK-WB-01~03。
  ⚠ **RISK-WB-01 必须上板**（`next_page` = 编译期无声的地址/容量类量）；背压路径不易构造，
  只做静态收口，**不宣称已取证**。`trim` 走新 `wbuf_handle(DROP)` 路径，**必测**。
- FTL service 与后台调度器（本次审查）：接线从 `ftl.c` 抽成 `bg_wiring.c`、任务表改
  表驱动 + `_Static_assert` 容量门、`bg_sched_t` 删死字段、档位配置由引擎发布、
  `jrn_handle(ERASE_ONLY)` 收回判据、RR 巡检节拍门补哨兵；见 RISK-BG-01/04/05/06/07。
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

---

## 2026-10-24 收敛归档（已修 / 已验证 / 信息 / 已议 / 已闭合）

> 本批从活跃表移出，行内容与状态保持原样，五要素完整。
> rev33 上板回归（format→POR→H2testw 415/423 MB 无错误）覆盖了其中正常路径；
> 状态为「已修未上板 / 已改未上板」的，注入类触发条件仍未单独验证，重访时按行内「重访触发」执行。

### 归档自 1. metadata 模块（提交 `992985c`）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-MD-01 | 已修未上板 | 日志块擦除持续失败时原会无限重试，无坏块策略 | 连续 3 次擦失败 ⇒ `bbt_mark_bad` + 清回收位，不再重试 | 上板注入擦除失败：第 3 次后进 BBT、回收位清零、后台停止重试 | `src/ftl/metadata_job.c` `md_reclaim_complete`；批次细节见 `docs/archive/known_risks_batches.md` |
| RISK-MD-02 | 已修未上板 | 压缩预算按启动时 `l2p_live` 计算，在飞 region 写可能使其超预算 | `MD_COMPACT_RESERVED_SLOTS` 预留空槽；超预算只良性中止、不擦 victim | 压缩期间并发写：不再出现“无空槽中止” | `src/ftl/metadata_job.c` `md_compact_begin` / `md_compact_write`；批次细节见 archive |

### 归档自 2. GC / migrate / journal（提交 `fdc7c7f`）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-GC-01 | 已修未上板 | 永久不可读 / META 损坏的 victim 会反复 abort，容量不再增长 | 每个 plane 源块连续 3 次读失败 ⇒ `bbt_mark_bad` + 从 Closed 条目摘除该 plane；计数跨 job 保留 | 造不可纠读：第 3 次后 GC 换候选、容量继续增长 | `src/ftl/migrate.c` `mig_read_complete` / `mig_job_start` / `mig_job_finish`；批次细节见 archive |
| RISK-GC-02 | 已修未上板 | 目的页写失败不能把已有成功迁移页的 Cold SB 整块标坏 | 写失败立即 `alc_super_close(COLD)`，不标 BAD；提交侧无 plane 编程时 `alc_super_open_rewind_pair`，有 plane 编程则关闭 Cold SB | 注入目的页写失败：关闭 Cold SB、已迁移页可读、下一页换新 SB | `src/ftl/migrate.c` `mig_dst_fail_cleanup` / `mig_write_complete` / `mig_write_step`；批次细节见 archive |
| RISK-GC-04 | 已修未上板 | 部分 plane 擦除后中止，旧 Closed 条目可能误认已释放 plane | 每擦完一个 plane 即 `alc_super_closed_drop_plane`；最后一个 plane 摘除时移除条目 | 两 plane victim 中途中止/断电：重启后旧条目不含已释放 plane | `src/ftl/migrate.c` `mig_erase_complete`；`src/ftl/allocator.c` `alc_super_closed_drop_plane`；批次细节见 archive |
| RISK-MIG-01 | 已修未上板 | READ_SETTLE / FLUSH_JRN 等阶段原会把 stall 计时清零，等待无上限；等待中的命令丢失也可能永久 active | `mig_job_wait` 使用独立 `FTL_WAIT_TIMEOUT_MS`；`waiting` 期间也 arm `FTL_WAIT_TIMEOUT_MS`，超时 abort 且写阶段关闭 Cold SB；迟到完成只 release | 注入 region 加载永久卡住/完成丢失：job 超时中止、不永久挂起 | `src/ftl/migrate.c` `mig_job_wait` / `cmd_wait_ms` / `mig_job_step`；批次细节见 archive；另合并 §2K GC-05：waiting 阶段完成丢失超时中止 |
| RISK-JN-01 | 已修未上板 | Journal 环形块擦除失败原会无限重挂 `need_erase` | 连续擦失败 3 次 ⇒ 标坏（RAM + BBT 两 plane）并跳到下一可用块；全坏则 `ring_full` 停止写入 | 注入环形块擦失败：Journal 用剩余块继续，重启后 BBT 仍挡住坏块 | `src/ftl/journal.c` `JRN_ERASE_FAIL_MAX` / `journal_erase_failed`；批次细节见 archive |
| RISK-JN-02 | 已修未上板 | ~~`jrn_flush_page` / `jrn_flush_now` 返回 `void`：同步刷页失败会置 `write_fail_pending`，但调用者无法知道“屏障未达成”~~ | 改为 `ftl_status_e` / `bool`；drain 超时、擦失败、写失败均返回 `FTL_ERROR`，`ftl_flush` 记 `LOGW` | 上板：注入同步写/擦失败，`fl` 能报错且 pending 阻止 victim 擦 | `src/ftl/journal.c` `jrn_flush_page` |
| RISK-RR-01 | 已闭合（H11 行数上限移除，2026-10-23） | 原 `read_recovery.c` 的 `rr_job_step` 超 H11 NBNC（状态机 switch 较大）；H11 现已取消 50 行上限、只保留嵌套 ≤4 层，故不再作为独立重构任务。~~`ftl_read_recovery_callback`~~ 已完成按 stage 拆 `rr_complete_*`（2026-10-19，见 §2H）；~~`RR_STAGE_ABORT` 死枚举~~ 已删除 | 与 GC/migrate 解耦；本轮只提取邻居页计算 | 若 `rr_job_step` 出现实际可读性问题，按语义边界拆，不以行数为判据 | `src/ftl/read_recovery.c`；`include/ftl/read_recovery.h`；`CLAUDE.md` H11 |
| RISK-RR-02 | 已修未上板 | ~~恢复成功后仍以 `SSD_STATUS_IO_ERROR` 完成托管子操作；且 `recovered` 只是最后一个 plane 的结果~~ | 新增 `all_ok` 聚合位：job 起点置 true，任一 plane 失败或 abort 清 false；`rr_job_finish` 按它报 OK/IO_ERROR | 上板：可恢复失败读应返回成功；单 plane 不可恢复整体报错 | `src/ftl/read_recovery.c` `rr_job_finish` / `rr_settle_plane` |
| RISK-RR-03 | 已修未上板 | ~~`rr_record_ecc` 健康状态可降级：CRITICAL/DYING 块在低 ECC 连续上升时被打回 WARNING~~ | 改为单调 max 升级 | 上板/UT 负例：CRITICAL 后低 ECC 上升不得回落 | `src/ftl/read_recovery.c` `rr_record_ecc` |
| RISK-RR-04 | 已修未上板 | ~~预测巡检返回的 CLOSED 块所在 SB 可能含 BAD plane ⇒ migrate 反复 abort 卡死~~ | `rr_predictive_scan` 命中候选后校验 SB，含 BAD plane 则跳过 | 上板：运行期标坏后预测路径仍能推进 | `src/ftl/read_recovery.c` `rr_predictive_scan` |
| RISK-GC-07 | 已修未上板（2026-10-20） | **约 2/3 容量即触发 GC/CP 活锁**（用户确认非满盘）：H2testw 写到约 2/3 后 `free=0`、`op=166/167`；GC 选中 victim block 305（`vr=183`），page1 读失败 → `MIG abort kind=0 ... victim read failed`；随后 `[CP] submit stalled 55/59ms — abort phase=1` 与 GC 重选反复刷屏，`reclaimed=0`，无前进。**根因**：`mig_read_complete` 在**任一页读成功**时清零同一 gblock 的 `fail_count`（`migrate.c` 原 1245-1249）；坏页在第 1 页，每次重试第 0 页都先成功读一遍 ⇒ 计数永远是 1，`MIG_VICTIM_READ_FAIL_MAX=3` 不可达 ⇒ 坏块永不隔离、同一 victim 永久重选、空间永不回收。另：两个 plane 的块号不同却共用一个计数槽，交替失败也会互相清零 | 失败计数改为**按 plane 的源块**分别记（`fail_gblock[PLANE_NUM]` / `fail_count[PLANE_NUM]`）；**单页读成功不再清零**，只在该块整块迁移成功（`mig_job_finish`）或该 plane 标 BAD 时清；达 3 次仍走 `bbt_mark_bad` + `alc_super_closed_drop_plane` | 上板复跑 H2testw（至少写满 2/3）：确认出现 `[MIG] victim read failed blk=... x3 — mark BAD`，随后 `reclaimed` 单调增长、`free` 回升、GC/CP 刷屏停止；`info`/`bbt` 中 `bad_count` 增加且重启后仍保留 | `src/ftl/migrate.c` `mig_read_complete` / `mig_job_start` / `mig_job_finish` / `mig_init`；`include/ftl/migrate.h` `mig_handle_t.fail_gblock[]`；`src/ftl/ftl.c` `mig_fail_state_is_clean`；串口 rev30 日志（block 305 / page1 / `reclaimed=0`） |
| RISK-JN-04 | 已修未上板 | ~~drain 超时后 `jrn_flush_page` 仍同步刷页；在飞写命令仍拥有当前页地址 ⇒ 双重编程~~ | drain 失败 ⇒ `LOGE` + 置 pending + `FTL_ERROR`，**不再**同步刷；`jrn_flush_now` 入口加 `job.phase == JRN_IDLE` 守卫 | 上板：注入命令丢失/超时（drain 超时），确认不发生同页二次编程、后续可恢复 | `src/ftl/journal.c` `jrn_flush_page` / `jrn_flush_now` |
| RISK-JN-05 | 已改未上板（2026-10-21） | **回放失败/降级日志逐条打印 ⇒ 刷屏并拖慢开机**：`jrn_page_replay_apply` 对**每条**落在降级 region 的条目打一行 `[JRN] replay: L2P region N degraded — force full metadata rebuild`，对每条 L2P 写失败再打一行 LOGE。一次掉电回放 1240 条、热点 region 可命中上千次 ⇒ 日志写满 4 KB TX 环后 `uart_log_write` 会等待（环满是阻塞的），开机被日志拖慢 | 改为文件内汇总计数（`s_replay_degraded_entries/_region`、`s_replay_write_fail_entries/_lba`）；`jrn_replay` 起点清零，结束时每类只打**一行**（首个 region / 首条 LBA + 条数） | 上板：掉电回放出现降级时只应看到一行汇总；开机时间不随日志条数增长 | `src/ftl/journal.c` `jrn_page_replay_apply` / `jrn_replay`；板端 2026-10-21 日志（region 51 降级连续刷屏） |
| RISK-MIG-02 | 已修未上板（与 RISK-GC-07 同一修法，2026-10-20） | **predictive 迁移活锁**：`kind=2`（RR 慢死预测）反复选中同一 SB（plane0=453 / plane1=1964），page 读失败 → `mig_job_abort` → 立刻重选，约 1 Hz 刷屏；`MIG_VICTIM_READ_FAIL_MAX=3` 的标坏路径有时触发、有时不触发。**根因已确认**：`fail_count` 在**同一 gblock 的其它页读成功**时被清零（原 `migrate.c:1245-1249`），坏页之前的页每轮先成功一遍 ⇒ 计数到不了 3；两个 plane 的块号不同却共用同一计数槽，交替失败也会互相清零。后果：NAND/CPU 被空转风暴占满，host 格式化超时失败（Windows 报“无法完成格式化”） | 同 RISK-GC-07：计数改 per-plane 源块；仅在整块迁移成功或标 BAD 时清零 | 上板：predict 路径复现慢死块，确认 3 次后 `bbt_mark_bad` + `alc_super_closed_drop_plane` 执行、刷屏停止；`bbt` 重启后 BAD 仍保留 | `src/ftl/migrate.c` `mig_read_complete` / `mig_job_abort`；`include/ftl/migrate.h` `mig_handle_t.fail_gblock[]`；上板日志 rev25~rev28：`victim read failed blk=453/1964/300/307` 反复出现 |
| RISK-MIG-05 | 已改未上板（2026-10-23） | **GC 读失败 abort 日志刷屏且阻塞 UART**：旧版每个 abort 打 3 行（abort + 两个 plane），同一坏块反复重试时淹没进展 | `mig_job_abort` 已合并为一行含 `g0/g1`；逐 plane `[GC] victim` DEBUG 已删；abort 按时间 1 行/5 s（有 count 兜底） | 上板坏块/满盘场景：abort 日志 ≤1 行/5 s，仍带 gblock/cause | `src/ftl/migrate.c` `mig_job_abort` / `mig_abort_log_is_due`；板端 rev32 旧日志 |
| RISK-MIG-06 | 已改未上板（2026-10-23） | **`mark BAD` 日志单位/原因不区分**：旧版 `blk=` 与 per-plane block 混用，且 `read_ok=false` 合并 FC 状态错/短链/META 三种原因 | `victim read failed` 已带 `gblk=` 与 `cause=`（`fc_err` / `short_chain` / `decode_corrupt`）；`mig_job_abort` 行带同一 cause | 上板坏页：同一 gblk 的 cause 可直接分流到 FC/数据面/META 排查 | `src/ftl/migrate.c` `mig_read_complete` / `mig_job_abort`；板端 2026-10-21 `blk=342 x3` 旧日志 |
| RISK-RR-05 | 已改未上板（2026-10-21） | **SLC 无 read-retry table 却做 8 次电压重试 + 邻居页辅助**：`SET_FEATURES(0x89)` 在 MT29F4G08 是保留地址（空操作），Stage 1 只是把同一页读 8 遍；Stage 2 读邻居页也无法恢复目标页。后果：坏页读失败要白跑 10 次 NAND 读，校验速度被拖到写速度以下（实测 verify 195 KB/s vs write 290 KB/s） | 已删除 SET_FEATURES 循环、邻居辅助与 4 个相关 stage；改为**一次正常读 + 仅 CRC_ERROR 允许一次重读**；UNC/NAND/MATA/TIMEOUT 直接失败。已 BAD 块直接结算（RISK-MIG-07）。失败日志带 `pba=/paa=`；新增 CLI `rdpaa <paa> [--raw]`（默认 ECC/CRC/解扰，`--raw` 取原始位）用于现场定位 | 上板：坏页读失败应在 1~2 次读内返回；校验速度不再被重试拖慢；日志只保留 `read unrecoverable`（5 s 限流） | `src/ftl/read_recovery.c` `rr_job_step` / `rr_read_complete`；`include/ftl/read_recovery.h` `rr_stage_e` |
| RISK-BBT-11 | 已改未上板（2026-10-21） | **二手片出厂坏块标记已丢失，没有可信 BBT**：AN1819 明确标记擦除即不可恢复；现网只有运行时退役（擦/写/读失败标坏），坏块会在首次写到时才被发现，数据可能已丢 | 新增 `format --verify`：在 `bbt_format_erase` 之后**清空非 BBT 池的旧 BAD 集**，对**所有**非 BBT 池物理块做「写图样 → 读回比对 → 再擦」，失败块 `bbt_mark_bad` 并 `bbt_save`（BBT 池保留本轮 record）。这就是「从头识别坏块」：会重新测试原先标记 BAD 的块，并打印 `old_bad/recovered/new_bad`。耗时数分钟，仅显式命令启用；自动格式化仍 `verify=false` | 上板：二手介质先跑 `format --verify`，结束时打印 `tested/bad` 并落盘；之后 `format` 正常启动、坏块不再参与分配。⚠ 若 bad 数量巨大 ⇒ 换片 | `src/ftl/power_failure_recovery.c` `pfr_format_verify_blocks` / `pfr_media_format`；`src/controller/cli.c` `uart_cli_format` |
| RISK-BBT-12 | 已改未上板（2026-10-21 v0.86） | **固定元数据池没有 BAD 跳过/替换机制**：`md_ring_addr_plane` 按 record_idx 直接算块号，不检查 BST BAD；Anchor / Checkpoint / BBT 三个池都是如此。固定池内某块变 BAD 时，只能靠 P0/P1 镜像的另一份；若**两个 plane 的同号块**都坏，该 record 槽永久不可用，而 ring 仍会反复选中它（CP/Anchor 写失败→abort/重试）。对比：L2P 日志区（`md_log_alloc` 跳过 BAD）、Journal（`jrn_block_next` 跳过）、OP/Spare（分配时跳过）都有处理 | **v0.86 已实现**：`md_ring_t` 改为候选块表（`blocks[]`），`md_pool_build_all()` 从名义区间挑 plane_mask 所有 plane 非 BAD 的块，缺额由 shared metadata spare（78..81）补；`md_ring_addr_plane`/Journal 地址查候选表。`format`/`format --verify` 仍保留 BAD，但坏块不再被固定池直接选中 | 上板：`pool` 打印候选池；`bbt fault <plane> <block>` 把某个名义元数据块标 BAD，重启后该分区候选数不变、出现 `remapped -> spare`，且记录仍可定位。当前元数据区实测 BAD=0（`[BLK] bad areas ... data=282`），负例用注入 | `src/ftl/metadata.c` `md_ring_addr_plane`；`src/ftl/checkpoint.c` `cp_job_step`；`src/ftl/bbt.c` `bbt_save` / `bbt_handle` |
| RISK-BBT-13 | 已改未上板（2026-10-21 v0.86） | **固定元数据分区无法按 BBT 重映射**：Anchor / Checkpoint / BBT / Journal / L2P 日志区的物理位置是编译期几何；BAD 落在这些分区时只能靠镜像（Anchor/CP 的 P0+P1）或跳过（Journal / L2P 日志有跳过；Anchor / CP / BBT 没有，见 RISK-BBT-12）。要真正做到「分区也按好块分配」，需要一个**分区重映射表**：在开机最早可读的位置（block 0；AN1819 保证出厂好块，本仓 factory scan 也跳过 gblock 0）存 logical partition block → physical block 映射 + CRC + 双副本，boot 先读它再定位各分区 | **v0.86 按主流实现，不做 remap 表**：固定元数据分区 = 候选块池（名义区间跳过 BAD + shared spare 补缺），record 靠 sequence 扫描定位；布局 v11 在 78..81/plane 预留 4 块 shared spare。判据见 `ftl_design.md §3.3.8`（BBT+sparing 而非 block remap 表） | 上板：① v11 首启 `[BLK] bad areas` 仍含 282 data BAD；② format 后 `pool` 候选池正常；③ `bbt fault` 注入元数据 BAD 后候选被 spare 顶替、重启仍能定位；④ Journal/L2P 注入后 `REBUILD_REQUIRED` 生效并进入 full rebuild | `src/ftl/power_failure_recovery.c` `pfr_boot_log_bad_areas`；`include/ftl/metadata.h` 分区表；`src/ftl/metadata.c` `md_ring_addr_plane`；`docs/design/ftl_design.md` §3.3 / §3.8 |
| RISK-BBT-14 | 已改未上板（2026-10-21） | **`FORMAT_IN_PROGRESS` 提交点过早**：`bbt_format_erase` 擦完就清掉标志，而 `format --verify` 的全片擦除体检和 initial Anchor/CP 写入都在其后。中途复位/烧录 ⇒ 介质停在「有 BBT、无 CP」，`cp_media_status_probe` 返回 IO_ERROR ⇒ 开机拒绝格式化，只能靠 CLI `format` 恢复。2026-10-21 实机复现：`[BLK] bad areas` 元数据区全 0、`anchor load newest=65535`，排除 RISK-BBT-12 | 标志保持到 initial CP **读回成功**（`pfr_format_end_bbt`）；此后掉电由已有的 `FORMAT_IN_PROGRESS` 分支自动续做格式化。显式失败路径也保持置位，下次开机重试 | 上板：`format --verify` 中途复位，重启应打印 `format required` 并自动格式化，不得出现 `CP media unreadable` | `src/ftl/bbt.c` `bbt_format_erase`；`src/ftl/power_failure_recovery.c` `pfr_format_end_bbt` / `pfr_media_format` |
| RISK-BBT-15 | 已改未上板（2026-10-21 v0.86） | **运行期元数据块变坏时，候选池/写游标仍指向坏块**：`bbt_mark_bad` 只置 BAD，Anchor/CP/BBT/Journal 的候选表与游标不会自动更新；继续写会打到坏块 | `bbt_mark_bad` 对元数据块只置 `pools->dirty`（不弹用户 Spare）；`md_pool_service` 在 CP/Journal/BBT 空闲时重建候选池、作废各游标；Journal 候选变化时 `jrn_page_scan_latest + jrn_resume` 接回实际写点；Journal/L2P 失块置 `REBUILD_REQUIRED`（BBT `reserved[1]`），新 CP 落盘后清除。⚠ **全量重建只恢复块状态/池，不保证重建已丢失的 L2P 条目**（依赖数据页 META 的旧机制本身如此）；丢失映射的 LBA 可能一直读不到，属于诚实降级 | 上板：`bbt fault` 注入后观察 `[POOL] rebuilt`、`pool` 候选变化、重启后记录可定位；Journal/L2P 注入确认下次开机 full rebuild | `src/ftl/bbt.c` `bbt_mark_bad`；`src/ftl/metadata.c` `md_pool_service` / `md_pool_build_all`；`src/ftl/checkpoint.c` `cp_hdr_write_complete`；`src/ftl/power_failure_recovery.c` `pfr_boot_recovery` |
| RISK-BBT-16 | 已改未上板（v0.88：判据改为**擦除成功**） | **全片体检可能「恢复」一个边缘坏块**：`format --verify` 从头扫描时清空非 BBT 池的旧 BAD 集，原先标记 BAD 的块**只做一遍擦除**就判好；擦得动但编不进去/读不出的边缘块会被当做好块重新分配。之后运行期若再次失败，由写失败重试 + Spare/OP 兜底，但读取侧仍可能丢数据（RR 只重试 CRC 一次） | 单遍擦除筛片；编程/读失败由运行期写重试 + `ftl_host_io_write_fail` / RR 标坏兜底；`format --verify` 打印 `old_bad/recovered/new_bad`，可据此判断恢复量 | 上板：`old_bad/recovered/new_bad` 合理；H2testw 全盘通过；若 `recovered` 很大或随后 H2testw 出错，改为「写/读体检」或「旧 BAD 一律保留、只新增不恢复」 | `src/ftl/power_failure_recovery.c` `pfr_format_reset_bad_map` / `pfr_format_verify_blocks` / `pfr_media_format` |
| RISK-MIG-09 | 已改未上板（2026-10-21） | **host 写失败只标 plane0 的块**（`ftl_host_io_write_fail` 旧实现），plane1 的编程失败要等 GC 擦除时才暴露，期间坏 plane 仍可能被分配 | 遍历失败命令的 desc 链，按逐 CW `status` + `desc->paa` 的 plane 得到失败 plane 掩码，分别 `bbt_mark_bad` 两个 plane 的块；没有逐 CW 失败位时退回旧行为 | 上板：注入单 plane 写失败（或看 `program failed ... planes=0x?`），确认对应 plane 的块当轮标坏、`info bad` 增长、后续不再分配 | `src/ftl/ftl.c` `ftl_host_io_write_fail`；`include/fc/fc_command.h` `fc_command_status_t` |
| RISK-GC-10 | 已改未上板（v0.87；v0.86 上板未通过） | **系统性 victim 读失败没有熔断，GC 会把好块成批标 BAD**：Mode 5 运行期读回 UNC 时，`mig_read_complete` 对每个 victim 连续 3 次失败就 `bbt_mark_bad` + 摘除 Closed plane，然后 GC 立刻选下一个 victim，同样失败 ⇒ `free` 从 31 掉到 18、BAD 图被成批写坏；host 读 FAT/MBR 的 LBA 1/4 命中已 BAD 块 ⇒ 反复 `read unrecoverable ... already-BAD` ⇒ Windows Code 21「设备未就绪」，H2testw 在 108 MB 中止。日志同时刷屏 `[MIG] victim read failed ... mark BAD` | 新增熔断：连续 **8 个不同 victim 块**被标 BAD（`MIG_SYSTEMIC_READ_FAIL_MAX`）⇒ `mig_handle.paused=true`，停止 GC/WL/predict 直到重启；整块迁移成功（擦除成功）才清零计数。RAM 态，重启后重新评估。⚠ 这是**安全网**，根因见 RISK-NAND-06（Mode 5 读/写时序） | 上板：注入系统性读失败时，应只看到 ≤8 行 `mark BAD`，随后一行 `systemic victim read failures — suspend GC until reboot`，之后不再有 `[GC]` 触发；正常盘上一次成功迁移即清零，不影响常规 GC。**2026-10-21 夜 rev33 实测未通过**：264 个不同 gblock 被标 BAD，全程无 `systemic` 行。根因有两个：① `mig_job_start` 的 `memset(m, 0, ...)` 只恢复 `fail_gblock[]` / `fail_count[]` 等字段，**不恢复 `read_fail_streak`** ⇒ 每个 job 都从 0 起，单 job 最多 +2，阈值 8 **结构性不可达**；② 即使保住，`mig_erase_complete` 在**单个 plane** 擦除成功时就清零（`migrate.c:1405`），而 victim 通常一侧 plane 健康（可读→擦成功）另一侧读失败 ⇒ 每个 job 都会再归零一次。判据=一次写流程中 `mark BAD` 行数 > 阈值且无 `suspend GC` 行。**v0.87 已修**：① `mig_job_start` 保存/恢复 `read_fail_streak`（连同 `paused`）；② 删除 `mig_erase_complete` 的逐 plane 清零，改为只在 `mig_job_finish` 的「整块迁移成功」分支清零。**未上板** | `src/ftl/migrate.c` `mig_read_complete`（`read_fail_streak++`）/ `mig_erase_complete`（`read_fail_streak = 0`）/ `mig_handle`；`include/ftl/migrate.h` `MIG_SYSTEMIC_READ_FAIL_MAX`；板端 2026-10-21 H2testw 日志（`fc_err`/`fc=0x48`、`free=18`、Code 21）；**2026-10-21 夜 rev33 `tmpl.log`（847 行，t=700..1510s）：bad 156→420、dng 55、op 11/0、spare 0/29** |
| RISK-GC-15 | 已改、上板（free=64 未触发 T1，符合 target=32） | **BUSY 档原会挡掉 urgent GC**：host 持续写时 `mig_handle` 先 return，`free` 掉到 1 才急救，且已无目的页可分配 | urgent 处理已移到 BUSY 早退前；`FREE_POOL_TARGET` = 32；BUSY 档 `free<32` 也执行水位 GC；`diag` 增加 `mv/nop` | 已上板（free=64 未触发 T1，符合 target=32）；若继续写至 free<32 需观察水位 GC | `src/ftl/migrate.c` `mig_handle`；`include/ftl/allocator.h` `FREE_POOL_TARGET`；`src/controller/cli.c` `diag` |
| RISK-GC-16 | 已改、上板（H2testw 无 T5/无 GC，843/889 KB/s） | **T5 磨损 GC 用“全表 max/min”作比值，min 可能来自从未使用的空闲块 ⇒ 永不收敛**：板端 H2testw 期间 GC 持续运行，`free` 从 3388 被搬到 64 后仍不停止，读写速度下降。 | T5 已从 `gc_should_trigger` 移除；磨损维护改由 `wear_leveling` 的 UBI 判据（`max_free_ec − min_used_ec ≥ WL_THRESHOLD`）在 host 空闲时独立启动；`mig_wl_begin` 增加 `[WL] start` 行。 | 已上板复跑 H2testw（无 T5/无 GC，843/889 KB/s）；空闲态 `[WL] start` 尚未观测。 | `src/ftl/gc.c` `gc_should_trigger`；`src/ftl/migrate.c` `mig_wl_begin`；`include/ftl/gc.h` |

### 归档自 2A. BBT（Bad Block Table）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-BBT-03 | 已修未上板 | ~~格式化先擦掉工厂标记再 save BBT~~ | 已改为两阶段 `bbt_format_erase`：① 置 `FORMAT_IN_PROGRESS` 并 save 失败即中止（不擦用户块）；② 擦除 BBT 池**之外**的块；③ 再 best-effort save 并入新 BAD。BBT 池旧 record 保留，新 record 以更高 sequence 胜出 | 上板：格式化过程中掉电注入（save 后 / erase 中 / CP marker 前），重启后 BBT 仍含工厂坏块图 | `src/ftl/bbt.c` `bbt_format_erase`；`src/ftl/power_failure_recovery.c` `pfr_media_format` |
| RISK-BBT-05 | 已修未上板 | 固定元数据区为空时原会把有用户数据的介质误判空片并重扫 | 抽样用户区 Page 0 的 META（8 点/plane）判断是否为空片 | 擦掉元数据区但保留用户数据：`bbt_init` 拒绝出厂扫描 | `src/ftl/bbt.c` `bbt_medium_has_ftl_data` / `BBT_USER_SAMPLE_COUNT`；批次细节见 archive |
| RISK-BBT-07 | 已修未上板 | ~~boot 期 CP 快照并集的 BAD 不补 Spare~~ | 新增 `alc_spare_refill_from_free(alloc, plane)`：摘 Free → state 置 NONE → push Spare，直到 `FTL_SPARE_POOL_PER_PLANE` 或无同 plane 空闲块。三条启动路径（clean 快照 / clean full metadata rebuild / PF full metadata rebuild）建池后逐 plane 补池并打印补后计数 | 上板：制造 CP 快照含 BAD、Spare 不足的介质，确认补回 50/plane；Free 不足时自然下降 | `src/ftl/allocator.c`；`src/ftl/power_failure_recovery.c` `recovery_refill_spare_pools` |
| RISK-BBT-17 | 已改未上板（2026-10-23 可读性收口） | **BBT 候选池判断重复 3 份**：`bbt.c:502-504`、`power_failure_recovery.c:242-244`、`:302-305` 各自手写同一段 `ring != NULL && block_count >= 2 ? md_ring_has_block(...) : (block in BBT_POOL)`。三处原为手写重复（不是活 bug）；已统一为 helper，避免未来只改一处（阈值/ring 语义）导致误擦/误留 BBT 候选池、BBT record 丢失或启动恢复失败。C2/R9 + 原则 2.2。 | 已抽 `bbt_block_is_candidate()`（`bbt.h` 纯 inline），三处统一；`borrowed_log` 拆为 `is_l2p_live` / `is_current_log`。 | 上板 `format --verify` + 重启 BBT 定位；若后续 BBT ring 语义变化，只改 helper | `src/ftl/bbt.c:502-504`；`src/ftl/power_failure_recovery.c:242-244,302-305` |

### 归档自 2B. 掉电恢复 / 启动路径（本轮审查，未提交）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-BOOT-06 | 已修未上板 | full metadata rebuild 原按第一个空白页当写指针，hole 之后的页漏计 valid_count | `pfr_scan_write_pointer` 检测“空白页后仍有写过页”并返回全块 | 造 page 0 空洞、后续页有数据：valid_count 不少算 | `src/ftl/power_failure_recovery.c` `pfr_scan_write_pointer`；批次细节见 archive |
| RISK-BOOT-07 | 已修未上板 | 原判空依赖 raw USER 区，DATA 页加扰后不可靠 | 改看每 CW 的 META（4B 明文）；`RANDOM_DISABLE` 不影响 | 空片/写满块判空仍正确 | `src/ftl/ftl_fc_interface.c` `ftl_page_is_blank_raw`；批次细节见 archive |
| RISK-BOOT-08 | 已修未上板 | Journal 扫描/回放读失败原被当作“无更多条目”，可能少回放 | `jrn_page_scan_latest(..., out_read_error)` / `jrn_replay` 返回 `FTL_ERROR`，启动回退 full metadata rebuild | 注入 journal 页读失败：不复用“无更多条目” | `src/ftl/journal.c` `jrn_page_scan_latest` / `jrn_replay`；批次细节见 archive |
| RISK-BOOT-09 | 已修未上板 | `md_load` 失败原静默新建 Directory，旧 region 映射可能整体丢失 | Directory 加载失败不再静默建新表：介质有有效 CP 时置 `ftl->md_load_failed` 并拒绝启动 | Anchor 损坏 + 有效 CP：启动停止并 LOGE | `src/ftl/metadata.c` `md_load`；`src/ftl/power_failure_recovery.c` `pfr_boot_recovery`；批次细节见 archive |
| RISK-BOOT-10 | 已修未上板 | ~~`jrn_resume` 恢复写态时忽略跨块擦除结果：擦失败后首次写会落在未擦页~~ | 擦失败 ⇒ `LOGE` + 重新挂起 `job.need_erase`（在 job 复位之后写入）；`jrn_handle` / `flush_now` 先补擦，`jrn_freeze` 在 `need_erase != 0` 时拒绝写 | 上板：注入 resume 跨块擦失败，首次写前必须补擦成功 | `src/ftl/journal.c` `jrn_resume` |
| RISK-BOOT-11 | 已修未上板 | 快照 probe 的 L2P 反查失败只 LOGW，valid_count 少算仍声称 `restored` | 读失败计数向上传播；非 0 ⇒ 快照结果作废、回退 full metadata rebuild | 注入 L2P 读失败：不声称 `blocks_restored=1` | `src/ftl/power_failure_recovery.c` `pfr_probe_blocks`；批次细节见 archive |
| RISK-BOOT-14 | 已改未上板（**性能优化**） | full metadata rebuild 的空闲块扫描原来逐页扫满 64 页（4096 块 × 64 ≈ 26 万次页读），是 rebuild 慢的主因。改为 **page 0 的 4 个 CW META 全 FF ⇒ 整块空白**，空闲块从 64 次读降到 1 次（本板约省 22 万次）。**假设**：写模型顺序追加，page 0 编程失败会关闭 SB、不会继续写后续页；若将来开放 Open SB 续写/失败后续写，该假设失效，hole 可能被漏掉 | 代码注释 + `ftl_design.md §3.1.5` 写明假设；当前写路径失败即关闭 SB | 上板验证：clean/PF/fresh 三条路径；部分写块 `partial`/`valid_cw` 与优化前一致；故意构造 page 0 空白但后续页有数据的异常块（如注入）应能暴露。若引入续写，恢复全块扫描或补读 page 63 | `src/ftl/power_failure_recovery.c` `pfr_scan_write_pointer`；`docs/design/ftl_design.md §3.1.5` 第 6 条 |
| RISK-BOOT-15 | 已修未上板（2026-10-20，L0） | **full rebuild → 下一次 CP 快照失效 → 每次开机都 rebuild 的循环**：`pfr_rebuild_closed_sb_list` 把**动态元数据 LOG 块**（`md_block_is_metadata(dir,...)` 为真）直接 `continue`，`pfr_boot_carve_pools` 也跳过它们 ⇒ 这些块的 BST state 停在 **NONE 且不属于任何 allocator 池**（归 metadata_dir 所有）。`cp_block_snapshot` 只看 BST + OP/Spare 池，把它们写成 `NONE + no pool tag`；下次开机 `pfr_restore_blocks` Pass 0 见到该 word 即拒绝整份快照（`[BLK] g=303 NONE with no pool tag`）⇒ 又走 full rebuild，再次留下同样状态。后果：每次冷启动都全盘扫描（空片 ~0.5s；312 MiB 介质实测 **2–4 分钟**），NAND 读放大与启动时间持续劣化；**数据不丢**（fallback 从 META/journal 重建），USB 在 rebuild 完成前不会枚举 | **L0 已实现（rev32）**：① `pfr_rebuild_closed_sb_list` 对非固定、`md_block_is_metadata(dir,...)` 为真的块显式置 `LOG`（清 valid/temp/sb）；② `cp_block_snapshot(alloc, md_dir, words)` 以 Directory 为准：`metadata_live_count>0` 且非 BAD 的块强制编码为 `LOG/no-pool`（生产者兜底，`md_dir==NULL` 时保持旧行为）；③ `restore_blocks` Pass 0 对 `metadata_live_count(dir,g)>0` 的 `NONE/no-pool` 放行，Pass 2 按 Directory 修正为 LOG（**自愈已污染的旧快照**，不再为一处旧 word 全盘 rebuild）；④ Pass 2 只对“非 LOG 且非 NONE”的真冲突打 LOGW，避免自愈时上百条刷屏。未做 L1（按块粒度局部重建）——用户决定先用镜像兜底 | **上板判据**：① 现有已污染介质上电应直接出现 `[BLK] restore closed=… log=…`（不再 `fallback to full metadata rebuild`）；② rebuild 一次 → 写数据 → 触发 CP → 断电重上电，第二次启动必须走快照恢复；③ 断电恢复（PF）路径同样不再退化为全盘 rebuild。若仍出现 `NONE with no pool tag`，说明该块不是 live LOG 块，需按 L1 处理 | 板端 rev31 日志（g=303）；`src/ftl/power_failure_recovery.c` `pfr_rebuild_closed_sb_list`（LOG 落账）、`pfr_restore_blocks` Pass 0/Pass 2（放行+修正）、`pfr_boot_carve_pools`（LOG 块跳过）；`src/ftl/checkpoint.c` `cp_block_snapshot`（Directory 权威编码）；`docs/design/ftl_design.md §3.1.5` 步骤 4/6 与启动时间表 |
| RISK-BOOT-16 | 已改未上板（2026-10-21） | **full metadata rebuild 逐 CW / 逐 region 日志刷屏**：`pfr_close_block` 对每个落在降级 region 的 CW 打一行 `[BLK] L2P region N degraded — snapshot incomplete`、对每个读失败 CW 打一行 `[BLK] L2P read failed lba=...`；降级 LOGE 也对每个 region 打一行。介质多处损坏时一次重建可产生数千行，写满 4 KB TX 环后 `uart_log_write` **会等待** ⇒ 重建被日志拖住；且 console 在 FTL boot 之后才起来，重建完成前 CLI 不可用（实测 region 0/51/52/53 同时降级，无法输入 `format`） | 扫描级汇总：`s_rebuild_read_fail_cw / s_rebuild_degraded_cw / s_rebuild_first_region` 在两条扫描入口清零、结束时每类一行；L2P 降级 LOGE 改 5 s 限流 + suppressed 计数（`l2p_degrade_log`） | 上板：多处 region 降级时重建日志 ≤3 行；不再出现逐 CW 行；重建结束后 CLI 立即可用 | `src/ftl/power_failure_recovery.c` `pfr_close_block` / `pfr_rebuild_closed_sb_list` / `pfr_probe_blocks`；`src/ftl/l2p_cache.c` `l2p_degrade_log`；板端 2026-10-21 日志 |
| RISK-BOOT-18 | 已改未上板（v0.90：L3 范围化 + L2 region 副本） | **单个 L2P region 不可读 ⇒ 全局回退全盘重建**：`[L2P] region 1200 load failed 3 times — degrade to unmapped` ⇒ Journal 回放命中降级基线 ⇒ `block_ctx.ok=0`（一个**全局**布尔）⇒ 整份块元数据快照被丢弃，`full metadata rebuild` 扫描全片（312 MiB 介质实测 2–4 分钟；本仓 `RISK-BOOT-15` 也提到按块粒度局部重建 L1 未做）。**这是本仓自拟的保守策略，不是主流形态**：valid_count 一旦带病会让 GC 漏账/误擦（静默介质破坏），所以选择「宁可全量重建」。主流的分层恢复手段本仓缺三项：① 器件侧 read-retry（MT29F4G08 的 0x89 是保留地址，无电压重试表；FC 只有 BCH、RAID 关闭）⇒ 单页不可纠无法就地救回；② 映射表冗余/parity；③ 范围化恢复——SPDK FTL 从 nvcache 的 L2P 页 + closed band 的 P2L 重建，**不需要读全盘**（`docs/reference/pages/spdk_ftl.html`） | **v0.90 已实现**：**L3 范围化**——降级 region 不再清 `block_ctx.ok`（Journal 增量仍正确、只丢该 region 的 pre-CP 旧映射），probe 的降级 CW 单独计数、不触发全量重建；host 读丢失映射返回 `IO_ERROR`（cache 级 `region_degraded_bitmap` 保证行驱逐后仍识别）。**L2 副本**——每个 region 写主/备两份（同一次 flush 串行写、两份都成功才原子提交），`md_entry_t` 主 PBA + 平行 `backup_pba[]`（Anchor 记录 8→16 页），读失败自动回退副本并交换主备；L2P 日志区 32→48 块/plane（容量 428→**424 MiB**，布局 v12）。⚠ 残余：两份都坏时 pre-CP 旧映射仍丢（boot-scoped 的 IO_ERROR）；相关块 valid_count 偏高、由 GC 逐 CW 反查自愈 | **上板判据**：① 当前 v11 介质（region 1200 仍坏）烧入后，启动应出现 `bit=scoped`、**不再** `fallback to full metadata rebuild`，启动秒级；② host 读 region 1200 覆盖的 LBA 返回 `IO_ERROR`，其余数据正常；③ v12 首启自动格式化 → `[CAP]` **424 MiB**；④ 注入某 region 主副本读失败 ⇒ 从备副本加载并交换、不降级；⑤ 两份都失败 ⇒ `bit=scoped`、不重建整盘。后续可加**诊断**（L2P 加载失败打印 PBA + `fc_cw_status_str()`，区分半编程页/地址错/介质失效） | `src/ftl/power_failure_recovery.c` `pfr_boot_power_failure`；`src/ftl/journal.c` `jrn_page_replay_apply`（`block_ctx->ok=0`）；`src/ftl/l2p_cache.c` `l2p_degrade_log` / `region_degraded_bitmap`；`src/ftl/metadata.c` `md_commit_region(MD_COMMIT_PAIR)` / `md_slot_pair_append` / `md_backup_find` / `md_live_copy_add`；`src/ftl/metadata_job.c` 副本感知压缩；`include/ftl/metadata.h` `L2P_REGION_COPIES` / `ANCHOR_DIR_PRIMARY_PAGES`；板端 2026-10-22 日志 `region 1200 load failed 3 times` + `fallback ... ctx_ok=0` |
| RISK-BOOT-21 | 已修未上板（v0.90 自查） | **降级位图被误当成「禁止加载」开关**：`l2p_load_start` 命中 `region_degraded_bitmap` 时直接合成全 INVALID 行、不读 Directory。但降级只表示「部分旧映射丢失」；region 被 journal 回放 / host 写入恢复并 flush 到新页后，行被驱逐再重载会再次合成 INVALID ⇒ 已恢复的映射读不到（host 误报 IO_ERROR），GC 迁移反查也会把这些 CW 当无效 ⇒ 可能漏搬、随后擦除有效数据 | 已改：删除短路，加载一律走 Directory（主→备）；位图只用于「未映射的 LBA 报 IO_ERROR」判定 | 上板：制造降级 region → 写入并触发 flush → cache 压力驱逐该行 → 重读应命中且 `map`/`rd` 不报错；`[L2P] load` 不应反复失败 | `src/ftl/l2p_cache.c` `l2p_load_start` / `l2p_is_region_degraded`；`include/ftl/l2p_cache.h` `region_degraded_bitmap` |
| RISK-MD-14 | 已改未上板（v0.90） | **双副本的账本/Anchor 原子性**：① 两份 PBA 都在 `l2p_live` 里计数（回收判据 `==0`）；② `md_live_rebuild`/`md_block_is_metadata` 的 debug 差分必须同时扫主/备；③ Anchor 记录 8→16 页，**主半必须完整同 sequence 才算有效**，备份半 best-effort（缺页只让对应 region 暂无双副本，不废整条记录）；④ 两份都写成功才 `md_commit_region(MD_COMMIT_PAIR)`，否则旧 pair 仍是真值 | 代码已按此实现；Anchor 记录主/备分半校验 | 上板：满盘写 + CP/压缩 + 反复重启；注入单个备份页损坏应仍能加载最新 Directory | `src/ftl/metadata.c` `md_anchor_page` / `md_load_assemble` / `md_anchor_record_probe_full` / `md_commit_region(MD_COMMIT_PAIR)`；`src/ftl/metadata_job.c` |
| RISK-MD-15 | 已改未上板（v0.90） | **副本感知压缩**：victim 上可能只有主、只有备、或两份都在；压缩必须逐副本搬移，且**两份都离开 victim 前不得推进 region 游标**；前台 flush 双副本占两个槽 ⇒ 预留槽位 1→2 | 已实现 `j->cur_copy` + 两副本扫描 + `md_commit_region(MD_COMMIT_BACKUP/PRIMARY)`；`MD_COMPACT_RESERVED_SLOTS=L2P_REGION_COPIES` | 上板：满盘触发压缩，`live=` 稳定、无 `no directory entry`/`clear stale count`；重启后数据完整 | `src/ftl/metadata_job.c` `md_compact_read` / `md_cp_write_complete` / `md_compact_begin` |

### 归档自 2C. L2P cache（本轮审查）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-L2P-01 | 已修未上板 | region 加载失败原立即降级为全未映射，映射静默不可用 | 先重试 3 次（`L2P_LOAD_RETRY_MAX`），仍失败才置 CLEAN + 全 INVALID；不活锁 | 造瞬态读错：映射不丢；持续坏 region：不活锁 | `src/ftl/l2p_cache.c` `ftl_l2p_callback`；批次细节见 archive |
| RISK-L2P-02 | 已修未上板 | ~~先取槽后建链，命令池空重试浪费 L2P 日志槽~~ | `flush_kick` 在 `append_slot` 前用 `fc_get_resource_stats()` 预检命令/描述符/buffer/SQ/HSQ；不足即返 `FTL_AGAIN`，不消耗槽位 | 上板：命令池高压下长时写，日志块换块频率不异常 | `src/ftl/l2p_cache.c` `l2p_flush_resources_available` |
| RISK-L2P-04 | 已改未上板（v0.90：主备副本 + 范围化，不再全量重建） | 上板复位后启动出现 `[L2P] region 0 load failed 3 times — degrade to unmapped`。降级路径把行置 CLEAN + entries 全 INVALID 并让 `l2p_read_sync` 返回 **OK** ⇒ probe 的 `l2p_read_errors` 保持 0，启动仍报 `restored`。**后果**：若该 region 有 journal 未覆盖的旧映射，会静默丢失且不触发 full metadata rebuild。| 修法：① `l2p_cache_line_t` 增 `degraded` 标志（与 `fresh/load_fail_count` 共用对齐填充，行尺寸不变）；② 新增 `l2p_cache_region_degraded()`；③ boot probe 读成功后检查该标志，计入 `l2p_read_errors` → 回退 full metadata rebuild；④ journal replay 在降级基线上应用条目时同样置 `block_ctx->ok=0`。**v0.90 起改为范围化**：cache 级 `region_degraded_bitmap`（行驱逐后仍识别）+ host 读该范围返回 `IO_ERROR` + probe 降级 CW 单独计数；只有**两份副本都读失败**才降级，且不再据此回退 full metadata rebuild（`RISK-BOOT-18`）。| 上板验证：正常启动不得误报（`restored` 仍正常）；故障注入（人工破坏一个 L2P region 页）后启动应出现 `L2P region N degraded — snapshot incomplete` 并回退 full metadata rebuild，而不是 `restored` | `src/ftl/l2p_cache.c` / `include/ftl/l2p_cache.h`；`src/ftl/power_failure_recovery.c` `pfr_probe_blocks`；`src/ftl/journal.c` `jrn_replay` |
| RISK-L2P-07 | 已改未上板（2026-10-23：顺序读预取） | **预取可能白占 cache 行/资源**：顺序检测误判、目标 region 未映射、或预取 load 在飞延迟 `l2p_flush_owner_idle`（LOADING 算非 idle），使 host FLUSH 等一个 region load；`load_inflight` 没有硬上限（只靠 prefetch 自己的 2 上限）。 | 只对顺序读、距 region 末尾 64 LBA 内触发；thrashing/flush/trim/资源不足时停；最多 2 个在飞；遇 DIRTY 不 kick 写回；不计 miss/hit。 | 上板看 `info` 的 `pf skipped/started/hits` 与 `miss_pct`；若 FLUSH 延迟或 miss 变差，先降 `AHEAD`/`MAX_INFLIGHT` 或停用。 | `src/ftl/ftl.c` `ftl_seq_note_read`；`src/ftl/l2p_cache.c` `l2p_region_prefetch` / `l2p_load_begin` |

### 归档自 2D. Checkpoint（本轮审查）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-CP-01 | 已修未上板 | ~~journal 预擦失败仍记 `pre_erased_block` ⇒ journal 回切时跳过擦除、写进未擦块~~ | 失败改 `jrn_truncate_commit(JRN_BLOCK_NONE)`：只复位水位、不记账；回切时由 journal 的 `need_erase` 重试，下一次 CP 的 plan 也会再报 NEEDS_ERASE | 上板：擦除失败注入，确认不跳过擦除、CP 不因 T1 反复触发 | `src/ftl/checkpoint.c` `ftl_checkpoint_callback`（CP_TRUNC_ERASE） |
| RISK-CP-02 | 已修未上板 | ~~`ftl_checkpoint_callback` 无 `waiting` 守卫：重复/迟到完成会按当前 phase 再走一遍~~ | 入口 `ASSERT(waiting)` + release 拒绝（LOGE + release + return） | FC 重复完成注入 / 上板回归 | `src/ftl/checkpoint.c` `ftl_checkpoint_callback` |
| RISK-CP-05 | 已修未上板（2026-10-21） | **abort 被调用两次，`fail_phase` 被覆盖成 CP_DONE(12) + 日志刷屏**：`cp_regions_flush` 的 `cp_job_stall` 内部已 `cp_job_abort`（记 fail_phase=1、phase=CP_DONE），返回 `CP_FLUSH_FAILED` 后 `cp_job_step` 又调一次 `cp_job_abort`，第二次看到 phase 已是 CP_DONE ⇒ `fail_phase=12`。日志因此出现 `abort at phase=12`（真实相位 1 丢失），且每次重试打两条 WRN，每 10~25ms 一次 | `cp_job_abort` 幂等：只在 `!failed` 时记首次 `fail_phase`；abort 日志按实例 1s 限流（`log_abort_ms`，`cp_job_begin` 的 memset 前后保存/恢复） | 上板：持续写流下 CP 日志应 1s 最多一行，且 `abort at phase=` 显示真实相位（1，而不是 12）。若真实相位仍为 12，说明另有路径在 CP_DONE 调 abort | `src/ftl/checkpoint.c` `cp_job_abort` / `cp_job_stall` / CP_DONE 分支；`include/ftl/checkpoint.h` `log_abort_ms` |
| RISK-CP-07 | 已改未上板（2026-10-23） | **SYNCHRONIZE CACHE 不产生 Checkpoint**：`ftl_flush` 只刷 L2P/journal，CP 仍靠 10 s/水位；host 写完即断电时可能只有初始 CP | `ftl_flush` 距上次 CP ≥5 s 时 force 一次异步 CP；CP start 不逐次打、done 仍 DEBUG（当前策略，**不是 INFO**）；稳定性看 `info cp=` / boot `[CP] find latest` / `[BLK] restore` | 上板：H2testw/SYNCHRONIZE CACHE 后拔电，重启应 `find latest` + restore，非 full rebuild | `src/ftl/ftl.c` `ftl_flush`；`src/ftl/checkpoint.c` `cp_pool_find_latest` / CP_DONE；`docs/design/logging_design.md` |

### 归档自 2E. 磨损计数 / 静态 WL（本轮审查）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-WL-01 | 已修未上板 | ~~WL 擦完 victim 用 `MIG_POST_RETIRE`：RETIRED 仍计入 erase 统计的 min/max ⇒ 低擦除块永久压低 min + 每次 WL 退役一块 ⇒ 容量持续流失~~ | 改 `MIG_POST_FREE`：擦完归还 Free Pool，动态 WL 优先选中它写入，擦除计数回升 | 上板：触发 WL 后 Free Pool/容量不下降，低擦除块被重新分配 | `src/ftl/migrate.c` `mig_wl_begin` |
| RISK-WL-02 | 已修未上板 | ~~`alc_bst_note_erase` 不重置 `read_count`：read disturb 跨擦除周期累计 ⇒ 正常块越过 1000 被预测路径退役~~ | 擦除成功清 `read_count`；`rr.health / last_ecc / rising_count` 保留（慢死趋势跨周期） | 长时 soak：正常块不被 `rr_predictive_scan` 误退役 | `src/ftl/allocator.c` `alc_bst_note_erase` |
| RISK-WL-03 | 已修未上板 | ~~Closed SB 的某个 plane 在标 BAD 后仍被 WL 选中：`plane_blocks` 含 BAD 块 ⇒ migrate 读失败 abort，且每轮重选 ⇒ WL 永久卡死~~ | `wl_select_victim` 预检：任一 present plane 的块 BAD ⇒ 跳过该 SB | 上板：运行期标坏后 WL 仍能前进 | `src/ftl/wear_leveling.c` `wl_select_victim` |
| RISK-WL-07 | 已改未上板（2026-10-21） | **WL 在 host 持续写盘时仍启动，占住单飞行 migrate 引擎**：USB BOT 串行 ⇒ 两条命令之间 `pending` 掉到 0，`bg_scheduler_level_select` 判成 IDLE/NORMAL，`mig_handle` 只在 BUSY 让路 ⇒ WL（日志 `[MIG] kind=1`）照常启动。实测单个 WL job age 4~7 s（3 pages/s），期间 GC 起不来、命令槽被分走，CP 持续 `submit stalled phase=1`，H2testw 写速降到 78 KB/s | WL 启动三重门：① 两次 5 s 求值之间 `*total_writes` **未增长**（host 静默窗口）；② `load_level == BG_LEVEL_IDLE`；③ `free_count > FREE_POOL_TARGET`（容量紧张把引擎让给 GC）。已启动的 WL job 不强行 abort（中止会留下已写未记 L2P 的页） | 上板复跑 H2testw：确认 `[MIG] kind=1` 心跳消失、写速回升、`info` 的 free 不再长期贴底；若仍 78 KB/s 且无 WL，瓶颈转 USB BOT/flush，另立 RISK-PERF | `src/ftl/bg_scheduler.c` `bg_scheduler_level_select`；`src/ftl/migrate.c` `mig_handle` / `mig_wl_begin`；串口日志 `[MIG] job kind=1 ... age=4221ms` |

### 归档自 2F. FTL↔FC 接口（本轮审查）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-IF-01 | 已修未上板 | ~~`ftl_cmd_write_data_cw` 用 `1U << cw` 判位（plane 盲）：flat 位图里 plane 1 的 bits 4..7 在 bits 0..3 全 0 ⇒ GC/WL 迁移 plane 1 的每个 CW 被填 0xFF~~ | 从 `descriptor->paa` 解 plane，判位改用 `ftl_cw_bit(plane, cw)` | 上板：迁移 plane 1 数据后逐 CW 读回校验 | `src/ftl/ftl_fc_interface.c` `ftl_cmd_write_data_cw` |
| RISK-IF-02 | 已修未上板 | ~~`ftl_sync_submit_wait` 超时摘表后命令迟到完成 ⇒ dispatch `take` 得 NULL ctx，`ftl_sync_wait_callback` 解引用 NULL + 命令槽位泄漏~~ | `ftl_sync_wait_callback` 对 `ctx == NULL`：LOGE + `fc_command_release` | 上板/故障注入：超时后迟到完成不 HardFault、池不泄漏 | `src/ftl/ftl_op.c`；另合并 §2K GC-06：`ctx==NULL` 迟到完成早退 |
| RISK-IF-03 | 已修未上板 | ~~同步原语忽略数据搬运返回值（写失败仍编程、读失败仍 OK）；raw 读缺 CW 静默跳过；短链读返回 OK 且尾部未初始化~~ | 三处均检查返回值并返回 `FTL_ERROR` | 上板/故障注入 | `src/ftl/ftl_fc_interface.c` |

### 归档自 2G. Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`，本轮审查，未提交）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-HOST-01 | 已修未上板 | ~~host 读成败判据只看 `FC_CW_STATUS_NAND_FAIL`：ECC 不可纠只置 `UNC`、页内 META 不符只置 `MATA_ERROR`，两者的 `NAND_FAIL` 恒清 ⇒ 加扰垃圾按 `SSD_STATUS_OK` 交给 host~~ | 改为 `fc_cw_status_has_error(completion->status)`（全集）；失败分支本就走「托管给 RR」，无需新增逻辑 | **上板取证**：造一次 ECC 不可纠 / MATA 读（写某 LBA → 擦掉其映射所指块 → 读该 LBA），确认 host 得 `IO_ERROR` 且 `[RR]` 心跳可见 | `src/ftl/ftl.c` `ftl_host_io_callback`；判据 `include/fc/fc_command.h` `fc_cw_status_has_error` |
| RISK-HOST-02 | 已修未上板 | host 读成功路径只记首个 desc 的 pba，multi-plane 第二块漏记 read_count/ECC | `ftl_cmd_block_ecc_list` 逐 plane 反查命令访问的全局块与 ECC | multi-plane 读：两个块的 read_count / ECC 都增长 | `src/ftl/ftl_fc_interface.c`；`src/ftl/ftl.c` `ftl_host_io_read_ok`；批次细节见 archive |
| RISK-HOST-03 | 已修未上板 | ~~`ftl_cmd_read_data` 返回值被丢弃：desc 链比请求短时尾部不写，未初始化缓冲被当数据交给 host~~ | 检查返回值：失败 ⇒ `LOGE` + `SSD_STATUS_IO_ERROR`，并跳过该次的 ECC/read_count 记账 | 上板/故障注入：短链读必须报错 | `src/ftl/ftl.c` `ftl_host_io_callback` |
| RISK-HOST-06 | 已改未上板（2026-10-21 v0.86） | **写 program fail 直接返回 host IO_ERROR**：坏块被标 BAD 后本页数据未重发，H2testw 等 host 会看到一次写失败 | 从失败命令的 desc 链读回本页（`ftl_cmd_read_data`），分配新页对、重建命令并重发一次（`FTL_WRITE_RETRY_MAX=1`）；同一 pending 保持，组在飞数不递减 | 上板：debug `bbt fault` 人为标坏即将写入的 data 块，`[FTL] write retry` 出现且 host 写仍成功；第二次失败仍如实 IO_ERROR | `src/ftl/ftl.c` `ftl_write_retry` / `ftl_host_io_callback`；`include/ftl/pending.h` `retry_count`；`include/ftl/ftl.h` `FTL_WRITE_RETRY_MAX` |
| RISK-HOST-07 | 已改未上板（2026-10-22；方向：全异步/背压） | **`ftl_confirm_write` 的「两遍之间无泵」对停泊确认不成立**：正常完成路径在 `ftl_op_dispatch` 内（`ftl_op_is_in_dispatch()==true`），`jrn_append` 页满且 `jrn_freeze` 失败时只丢弃不泵；但 `pend_confirm_service` 在主循环（dispatch 深度 0）调它，同一条件下 `jrn_append` 会走 `jrn_flush_page` → `jrn_drain` / `ftl_sync_write_page` → `ftl_op_drain` **重入 `ftl_host_io_callback`/`ftl_confirm_write`**。后果：①批量化在 Journal 循环前一次读完 `oldp[]`，重入确认若改同一页的 LBA ⇒ `oldp` 陈旧（Journal 的 old_pba 链断、`valid_count` 重复扣减）；②重入的 `l2p_is_ready` → `l2p_load_start` → `l2p_line_evict` 可把本行从 DIRTY kick 成 FLUSHING ⇒ `l2p_entries_write` 返 `FTL_AGAIN`，release 下 `(void)wst` 静默跳过 L2P 更新（Journal 已追加）⇒ 运行期映射分叉，掉电回放才恢复。逐 CW 旧实现只在“正在 append 的那一 CW”有窗口，批量化扩大到整页。⚠ `ftl_op_is_in_dispatch()` 只挡“已在派发中”，**挡不住 `pend_confirm_service` 这种非派发服务函数内部再开泵** | `ftl_confirm_write` 已在动账前调用 `jrn_reserve`（非阻塞、只起异步刷页、留 1 空槽）：不足即返 `FTL_AGAIN` 停泊；`ring_full` 时预留失败、停泊超时按 IO_ERROR 结算（不再静默成功）；GC/WL 的 `MIG_WRITE_SETTLE` 同样改为 `jrn_reserve` + `mig_job_wait`；`jrn_append` 已在**所有上下文**移除同步回退，`jrn_flush_page` 同步出口已删除（TRIM 也改 `jrn_reserve`）；Journal→L2P→BST 顺序未变；`ASSERT(wst == FTL_OK)` 仍仅 debug 可见 | 已实现（2026-10-22，未上板）：**不采用同步刷 Journal**，按主流存储主控全异步/背压做。`jrn_reserve(n)` 在 `ftl_confirm_write` 读 `oldp`/追加任何条目之前预检本页剩余槽位；不足时只调用 `jrn_freeze()` 起异步刷页并返 `FTL_AGAIN` 让 pending 继续停泊，confirm 内不调用 `jrn_flush_page` / `ftl_sync_submit_wait` / `ftl_op_drain` ⇒ 无重入、不丢条目；冻结/擦除由 `jrn_handle` 后续轮推进。若需要“Journal 已持久化”才完成 host 写，用**异步依赖**（pending 等 journal job 完成回调再进 APPLY），不是阻塞等待。上板验证：页满 + 后台 Journal 在飞 + 同页并发写时无 `ASSERT(wst)`、无 confirm park timeout、断电后映射一致；难构造，先随常规回归上板 | `src/ftl/ftl.c` `ftl_confirm_write` / `ftl_trim_step`；`src/ftl/journal.c` `jrn_reserve` / `jrn_append` / `jrn_freeze`；`src/ftl/migrate.c` `mig_write_settle_step`；`src/ftl/pending.c` `pend_confirm_service`；`src/ftl/l2p_cache.c` `l2p_entries_write` |

### 归档自 2H. 完成派发面（`ftl_op.c/h`，本轮审查）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-CR-01 | 已修未上板 | ~~`ftl_op_ctx_add` 只找首个空槽、**不按 `command_id` 去重** ⇒ 旁表可存在两条同 id 项；而 `ftl_op_ctx_take` 取**首个**匹配 ⇒ 泄漏之后 id 被复用，服务函数拿到**别人的 ctx**（可能是早已失效的 `cp_handle_t *` / 栈上 `ftl_sync_waiter_t *`），本命令自己的表项永久泄漏~~。可达路径：`fc.c` CQ 满强制置 COMPLETED（注释自陈「CQ entry 丢失，FTL 无法感知」）⇒ 表项泄漏 → `fc_command_alloc` 复用 id → 再次 add | `ftl_op_ctx_add` 改两遍扫：先全表查 `cmd_id`，命中则**原地覆盖 ctx** + `LOGW`；未命中才取空槽 ⇒ 「同一 cmd_id 至多一条表项」收进**唯一闸门**（H10） | **不可直接构造**（需 CQ 满 / 丢完成）⇒ 静态收口 + 新增 `LOGW` 使一旦发生即可见；随常规回归上板，**不宣称已取证** | `src/ftl/ftl_op.c` `ftl_op_ctx_add` |

### 归档自 2I. 页打包写缓冲 wbuf（`wbuf.c` + `wbuf.h`，本轮审查）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-WB-01 | 已修未上板 | ~~`wbuf_handle(FLUSH)` 取到页对后 4 条失败返回（命令池满 / 建链 / pending 满 / SQ 满）**丢弃**该页对，而 allocator 无归还 API ⇒ 缓冲保留、每轮重试**白烧 2 页**，表现为容量无声流失（CLAUDE.md §3.1）~~ | 资源次序改为**按有无归还路径排序**（命令槽先于页对）+ 新增 `alc_super_open_rewind_pair` + 统一错误出口归还页对 | **必须上板**：`next_page` 属地址/容量类量（§3.2 编译期无声）。背压下命令池满不易人为构造 ⇒ 静态收口 + 可达路径结构性收窄，**不宣称已取证** | `src/ftl/wbuf.c` `wbuf_handle(FLUSH)`；`src/ftl/allocator.c` `alc_super_open_rewind_pair` |
| RISK-WB-02 | 已修未上板 | ~~TRIM 清空窗口的**内部不变量**（清 `occupied` + 退 `owners` 在飞数 + 复位）由 `ftl_trim`（外部模块）手工维护；`cli.c` 的 `bbt --erase` 直接 `memset(&ftl->wbuf,…)` 丢弃 `owners` ⇒ 那些写组在飞数永不归零、host 写挂死~~ | 不变量收进 wbuf 模块：新增 `wbuf_handle(DROP)`（TRIM）/ `wbuf_handle(RESET)`（介质擦净，按写失败收尾）；调用点改为调接口（§3.1 / C3 / H13） | 上板：`trim` 走新路径不回归；`bbt --erase` 后不再有写组挂死 | `src/ftl/wbuf.c`；`src/ftl/ftl.c` `ftl_trim`；`src/controller/cli.c` |
| RISK-WB-04 | 已修未上板 | 组在飞数原由 wbuf/ftl/pending 多处手工维护，漏减会挂死 host 写组 | 收敛为 `pend_group_add_subop` / `pend_group_drop_subop` 唯一闸门 | 写路径回归：组完成/回滚均在飞数归零 | `src/ftl/pending.c`；`src/ftl/wbuf.c` / `ftl.c` 调用点；批次细节见 archive |

### 归档自 2J. FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`，本轮审查）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-BG-01 | 已修未上板 | ~~`bg_task_rr` 的内部门 `(now_ms - rr_last_scan_ms) >= RR_SCAN_INTERVAL_MS` 在时钟缺失（`ftl_now_ms()` 恒 0）下恒为假 ⇒ 慢死预测巡检**永久停摆**；与同族 `mig_gc_evaluate` 的哨兵语义（退化为「每轮求值」）方向相反，也与注释宣称的「内部门兜底」相反~~ | 补 `\|\| (rr_last_scan_ms == 0U)` 哨兵，与 GC 同构 | 上板：确认 1 s 节流仍在（巡检不逐轮跑） | `src/ftl/bg_scheduler.c` `bg_task_rr` |
| RISK-BG-04 | 已修未上板 | ~~任务表装载**无边界检查**：加第 14 项会 `tasks[13]` 越界写踩掉 `task_count`（§3.2 静默失效，编译期无声）；且适配器注释里的位号（12 / 13）与真实下标（11 / 12）不符（H6）~~ | 改表驱动 `s_bg_tasks` + `_Static_assert` 容量门 + `bg_scheduler_init` 直接取编译期表长装载（无运行期 count 参数、无 ASSERT）+ 任务位改由表项 `.mask` 显式声明（不再由表下标推导） | 上板：IDLE / NORMAL / BUSY 各跑一轮，确认 10 项照常执行 | `src/ftl/bg_scheduler.c` `s_bg_tasks` / `bg_scheduler_init` |
| RISK-BG-05 | 已修未上板 | ~~`bg_scheduler_t` 死字段 `priority` / `round_total_us` / `round_used_us` / `round_seq` / `level` / `clock_ok` 无生产消费者（H5）；`priority` 与表序是**同一语义的两套坐标**且已漂移（取值跳过 5，缺口正是被删的 Wear 项）~~ | 删除；`clock_ok` 降为 `bg_scheduler_run` 局部变量；表序即优先级（C9） | 上板：确认无行为变化（表序未变） | `include/ftl/bg_scheduler.h`；`src/ftl/bg_scheduler.c` |
| RISK-BG-06 | 已修未上板 | ~~`bg_task_l2p_flush` 经 `ftl->bg.level` **反查** `s_bg_level_cfg` 取 `l2p_flush_max` = 第二套配置读取路径，与引擎实际收到的 `cfg` 可能不同步（引擎按形参跑、适配器按字段读）~~ | 引擎在 `bg_scheduler_run` 起点发布 `sched->cfg = cfg`，适配器读 `ftl->bg.cfg->l2p_flush_max`（单写点） | 上板：BUSY 档 flush 条数应为 1（`BG_BUSY_FLUSH_COUNT`） | `src/ftl/bg_scheduler.c` `bg_task_l2p_flush` |
| RISK-BG-07 | 已修未上板 | ~~`bg_task_journal_erase` 手工读 journal 内部状态 `job.phase` / `job.need_erase`（§3.1 形态：判据属别的模块）。journal 一旦改内部表示（如换 phase 枚举），该守卫**静默恒假** ⇒ 换块擦除永不启动 ⇒ 新环形块未擦就写~~ | 判据收回 owner 模块：`jrn_handle(ERASE_ONLY)()`（`journal.h` 声明 / `journal.c` 实现），行为与原守卫逐字等价（`phase == JRN_IDLE && need_erase != 0` 才调 `jrn_handle`） | 上板：跑 journal 换块路径（写满一个环形块），确认擦除照常补起 | `src/ftl/journal.c` `jrn_handle(ERASE_ONLY)` |
| RISK-LOG-03 | 已改未上板（2026-10-23） | **`[GC  ] no-gain` 刷屏**：`mig_gc_begin` 在选块前/启动后过早清 `gc_nogain`；选块成功但目的页分配失败（free=1）时，每轮「起 READ → dest 分配失败 → abort」都会重新把边沿日志当成首次，日志连续刷屏 | 只在 `mig_job_finish` 且**实际 `migrated>0` 或成功回收**后清 `gc_nogain`；仅启动 job/中止不清；no-gain 降 DEBUG，INFO 档不可见 | 上板 `free=1` 持续写：INFO 不应再出现 no-gain；DEBUG 下每个 no-gain 状态最多一行；若仍刷，查 `gc_nogain` 的清零点 | `src/ftl/migrate.c` `mig_job_finish` / `mig_gc_nogain_note`；板端 2026-10-23 `free=1` 刷屏日志 |

### 归档自 2L. FTL 模块 handle 契约（2026-10-19 设计，未改代码）

| ID | 状态 | 风险 / 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-ARCH-05 | 已改未上板 | 独立 `confirm_handle` **未采用**；write-confirm park 已收进 `pend_confirm_service()`（`pend_confirm_park/park_ms/超时`）；TRIM 仍留在 `ftl.c` | 停泊列表 = pending 表的 `PENDING_OP_WRITE_CONFIRM` 项；超时 `FTL_GROUP_DRAIN_TIMEOUT_MS` 后 complete group IO_ERROR | Journal→L2P→BST 顺序有两处实现（confirm/TRIM），**任何顺序变更必须同步两处**（C2/R9） | `src/ftl/pending.c` `pend_confirm_*`；`src/ftl/ftl.c` `ftl_confirm_handler` |
| RISK-ARCH-07 | 已改未上板 | §8.1 Phase 1.5：`mig_handle` / `wbuf` / `rr_page_scratch` 已从 `ftl_ctx_t` 改为 `ftl_storage_t` 挂载指针；数组落 AXI `.bss`（`g_ftl_migrate` / `g_ftl_wbuf` / `g_ftl_rr_page_scratch`） | `ftl_init` 绑定 + 显式 memset；AXI 预算 `_Static_assert` 已把三块计入 | 上板确认 RAM 实测与 cache/局部性；若需要域优化再调整放置 | `src/main.c`；`include/ftl/ftl.h` `ftl_storage_t` |
| RISK-ARCH-08 | 已改未上板 | migrate / RR / CP 的提交/等待计数已由**轮数**改为绝对 ms deadline：`FTL_SUBMIT_TIMEOUT_MS = 10` / `FTL_WAIT_TIMEOUT_MS = 50`；confirm park 用 `FTL_GROUP_DRAIN_TIMEOUT_MS = 100` | 时钟缺失（now_ms == 0）时 deadline 不生效（退化为无超时；本板 time_us 恒非空 ⇒ 不可达） | 上板标定这三个值（NAND 页操作 ~700µs + 主循环轮速）；观察 `timed_out`/`timeout_phase` 上报 | `include/ftl/ftl_module.h`；`migrate.c` / `read_recovery.c` / `checkpoint.c` / `pending.c` |
| RISK-ARCH-09 | 已改未上板 | §5 PFR 收为 boot orchestrator：fresh/clean/PF 子流程与 full metadata rebuild 原语（restore/probe/build_pools/reset/page_is_blank/scan_write_pointer/close_block/carve/rebuild）全部改为 `static`；公开头只留 `pfr_media_check_format` / `pfr_media_format` / `pfr_boot_recovery` | `checkpoint_block_words()` / `metadata_live_count()` / `l2p_cache_invalidate_line()` 三个穿透已收口 | 上板回归 fresh/clean/PF/full metadata rebuild 四条启动路径 | `src/ftl/power_failure_recovery.c`；`include/ftl/power_failure_recovery.h` |
| RISK-ARCH-11 | 已改未上板 | 端口删除后，模块直接持有 owner 结构指针（`alloc` / `journal` / `cache` / `metadata_dir` / `pending`），C 类型仍可见；「不直接改对方字段」从类型约束降为 owner API + 单写者约定 | 跨模块写只走 owner API；新增 `alc_block_*` / `rr_record_block_ecc` / `rr_retire_block_at` / `pend_view_op` / `pend_group_has_window_pending` 收敛访问；`contract_check_module_contexts` 证明绑定 | 若出现模块直接写对方字段的 review 案例，或需要多后端/测试替身，再评估 opaque 类型/更强的接口；当前不做，避免过度封装 | `src/ftl/ftl.c` `contract_check_module_contexts`；`include/ftl/allocator.h`；`include/ftl/pending.h`；`docs/archive/ftl_module_contracts.md` §3.3/§8 |

### 归档自 2M. USB MSC / 平台（2026-10-20，代码已落地）

| ID | 状态 | 风险 / 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-PLAT-01 | 已上板（PIO 正常路径）；`CONFIG_NAND_DMA=1` 未验 | linker 的 `.noncacheable_buffer` 输入段匹配原只有无前导点形式（`*(noncacheable_buffer)`），`nand_dispatch.o` 的页缓冲被当**孤儿段**启发式放置：实测落进可缓存 AXI RAM `0x24000204`，与源注释「D-Cache 关闭」不符。`CONFIG_NAND_DMA=1` 时是潜在静默数据破坏。 | linker 补齐 `.noncacheable_buffer` / `.noncacheable_buffer.*`；页缓冲、FC buffer pool / staging 全部固定 noncacheable。**页缓冲不再条件放 DTCM**（见 RISK-TCM-08）。 | `nm` 核对三块页缓冲/池在 `AXI_NOCACHE`；H2testw 415/423 MB 0 错误已覆盖 PIO 正常路径；DMA=1 仍需单独上板。 | `linker/stm32h743xx_flash.ld`；`src/nand/nand_dispatch.c`；`src/fc/fc.c`；板端 H2testw 415/423 MB |

### 归档自 2O. NAND / FMC 时序与吞吐（2026-10-20 分析，未改代码）

| ID | 状态 | 风险 / 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-NAND-07 | 已上板（2026-10-23，two-plane cache 读写 + UT；功能通过） | **双 plane cache 读/写路径首次接入**：中间页 `80h-11h-80h-15h`、末页 `80h-11h-80h-10h`；读侧 `00h-00h-30h + 31h/3Fh` 后必须用 `06h-E0h` 两次分别选 P0/P1。错一处会读错 plane 或把最后一页留在 data register。当前只做 NAND/FC 层 + `block_cache_multi_plane` UT，**未接 FTL**；**功能已上板验证，失败注入未验** | 新 op44~47 + `completion_all_cws` + 双 plane scatter；UT 覆盖双 plane 写/读与 06h 选择；单 plane `START` 改为 `START_SEQ` 并重写旧 UT | 上板跑 `test /fc-level3/block_cache_ops`、`block_cache_multi_plane`；若数据/状态错，先看 `06h` 选择与 scatter offset（`plane*PAGE_SIZE + cw*CW_SIZE`），再看 completion 是否覆盖全部 CW；之后再评估 FTL batch | `src/nand/onfi_1_0.c` op44~47；`src/nand/nand_dispatch.c` scatter；`tests/unit/fc_test_common.c` / `test_fc_level3.c`；**2026-10-23 板端** `test /fc-nand/` **56/56**（3 skipped，928.61 ms），`block_cache_ops` 62.16 ms、`block_cache_multi_plane` 114.91 ms 均 OK |

### 归档自 2P. allocator（2026-10-21 读码判定，未改代码）

| ID | 状态 | 风险 / 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-ALLOC-03 | 已改未上板（2026-10-21） | 原判定「访问器是有意设计、勿展开」的依据是它们携带边界断言。`gblock` / `alloc` 断言按用户裁决删除后，除 `op_at` / `spare_at` 外都成了**纯一行转发**（`return ftl_block_xxx(&alloc->blocks[g])`）。而 `blocks[]` 本来就是公开指针字段：`allocator.c` 之外的模块已有 **37 处**直接写 `&alloc->blocks[g]` + `ftl_block_*`（metadata / bbt / read_recovery / wear_leveling…）⇒ 同一语义已经存在**两套访问坐标**（C9），封装并未真正生效。**建议**：删 7 个纯转发（`is_bad` / `state` / `sb_id` / `valid_count` / `temp` / `add_valid` / `sub_valid_if_live`），统一到 `bst.h` 的 `ftl_block_*`（`valid_count ±1` 的单一入口随之改为 `ftl_block_add_valid` / `sub_valid_if_live`，实现本就唯一）；保留 `op_at` / `spare_at`（`index < count` 边界 + 池数组无其它直接访问）、`wear_get` / `restore_wear`（多字段快照/恢复）、`bbt_format_in_progress`（跨模块读 bbt 子状态）、`count_blocks_below`（整表扫描） | 已删 7 个纯转发，统一到 `bst.h` 的 `ftl_block_*`；保留 `op_at` / `spare_at` / `wear_get` / `restore_wear` / `bbt_format_in_progress` / `count_blocks_below`。测试源不引用这 7 个访问器（R4 未受影响） | 已改（2026-10-21）。表拆分见 RISK-ALLOC-07。⚠ **不通过改表挂载来消除封装**：那是内存域布局，嵌入 `g_ftl`（AXI）需要重排预算，且一个访问器也减不掉 | `include/ftl/allocator.h:390-469`；`src/ftl/allocator.c:719-843`；直接访问 37 行（`grep -rn "alloc->blocks\[" src/ftl` 去掉 `allocator.c`）；内存域 `src/main.c:184-239` |
| RISK-ALLOC-07 | 已改未上板（2026-10-21） | `ftl_tables_t`（内含 5 个数组，123,896 B）拆成 `main.c` 的 5 个**独立对象**：`g_ftl_blocks` / `g_ftl_free_blocks` / `g_ftl_op_blocks` / `g_ftl_spare_blocks` / `g_ftl_closed_sb_list`，各自带 `SRAM12_DATA`；FTL 改收 `ftl_table_refs_t`（5 指针），`ftl_ctx_t.table_refs` 按值保存。每个数组之后可**单独换内存域**（布局自由度），当前仍全部留 SRAM12，容量/行为不变。⚠ 这是**地址/布局类**改动，编译期无法证明放置正确 | 五表分别 memset（NOLOAD）；`_Static_assert` 改为五者 `sizeof` 之和 ≤ SRAM12；`contract_check_handles` 比 `alloc->* == table_refs.*`；refs 按值拷进 context（不持有 `main.c` 栈上局部指针） | **上板判据**：启动/格式化恢复、`info`（429 MiB/free）、`wr/rd ... c`、GC、重启恢复、`map`。`make tests UT=FTL` 失败原因在冻结夹具（`F->tables` 旧形状），非生产代码（R2/R4） | `src/main.c`（五数组 + refs 装配 + 断言）；`include/ftl/allocator.h` `ftl_table_refs_t`；`include/ftl/ftl.h` `table_refs`；`src/ftl/ftl.c`（逐表 memset / alc_init / audit）；`src/ftl/{migrate,journal,checkpoint}.c` 调用点 |
| RISK-ALLOC-08 | 已改未上板（2026-10-23 可读性收口） | **原缺陷：`alc_super_close_live` 吞掉 append 失败码**。若 Closed 列表满，live SB 已关闭但未进 `closed_sb_list` ⇒ GC/容量账看不到该块，永久漏账。H30/C5。现已把 append + live 标志合并为唯一 `alc_super_close_live`：先检查列表容量，满时保持 live 状态不变并返回 `FTL_BLOCK_NO_SB`；调用者按失败码处理。 | `RISK-ALLOC-01` 的唯一 live setter 保留；`alc_super_open_alloc_pair` 与 `alc_super_close` 上抛失败；`alc_super_prepare_wl_destination` 的 void 边界显式丢弃。 | 上板 `format` + H2testw；若新增关闭路径，必须复用同一函数并检查返回值 | `src/ftl/allocator.c` `alc_super_close_live`；`include/ftl/allocator.h`；`RISK-ALLOC-01` |

### 归档自 2Q. FTL 类型 / 命名 / 对齐收口（2026-10-21，未上板）

| ID | 状态 | 判定 / 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-ARCH-12 | 已改未上板（2026-10-21） | ① `config.mk` 显式加 `-fshort-enums`（本工具链默认已是 1 字节，属钉住默认值）；② 上下文/句柄里的布尔语义字段改 `bool`、多状态字段改具名枚举（`bst_block_t.state`、`cp_phase_e`、`md_handle_phase_e`、`rr_stage_e`、`bg_level_e` 等）；③ 公共 API 统一「模块缩写 + 动作」前缀（`alc_` / `jrn_` / `cp_` / `md_` / `mig_` / `rr_` / `wb_` / `l2p_` / `pend_` / `ct_`），并补 `get_`/`is_`/`has_`/`find_`；④ 类型别名也按模块缩写：结构体 `<模块缩写>_*_t`（`ftl_block_t`→`bst_block_t`、`ftl_ctx_t`→`ftl_ctx_t`、`metadata_*`→`md_*`、`ftl_pending_*`→`pend_*` 等），枚举一律 `<模块缩写>_*_e`（项目内全部枚举，third_party/stm32h7r3 除外）；⑤ 删除 6 个 static 纯转发与 6 个零调用公共 API；⑥ `pend_req_t` 36→32 B、`pend_group_t` 56→48 B（`ftl_ctx_t` 3,360→2,976 B）。 ⚠ 批量改名漏改会编译报错可发现；**语义改错不会**，必须 review + 上板。 ⚠ 若工具链默认枚举变 4 字节，`bst_block_t` 会 20→24 B（+16 KB，N=2 超 SRAM12） | 静态断言钉住：`sizeof(bst_state_e)==1`、`sizeof(bst_block_t)==20`、`sizeof(pend_req_t)==32`、`sizeof(pend_group_t)==48`；`make -B all/release` 零新告警 | 新增枚举字段 / 新增公共 API 时按同一约定；换工具链时先看 `-fshort-enums` 与四条静态断言。**上板判据**：启动/恢复、`info`、`wr/rd ... c`、GC、重启、`map` | `config.mk`（`FIRMWARE_CFLAGS`）；`include/ftl/bst.h`（枚举断言）；`include/ftl/pending.h`（重排 + 断言）；各 `*_handle_t` 头；`docs/design/ftl_design.md §8` |

### 归档自 2S. FTL cache read（2026-10-23，未上板）

| ID | 状态 | 风险 / 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-CACHE-01 | 已改未上板（2026-10-23） | **cache read sequence 的连续性依赖当前 FTL 全 SQ / 无 HSQ**：4 条命令连续提交后，后续命令只在 SQ 尾部排队；若未来任何模块用 HSQ（`high_priority=1`）或链式提交插入同 LUN，31h/3Fh 管线会被打断，读到错页。另一风险：成员失败不启动 RR，组报 IO_ERROR，后续已入队成员仍执行；不做单成员回滚。 | 批最多 4 个 pair（NAND FIFO 深度）；一次连续提交、中间不调 `fc_service`；`PENDING_OP_READ_CACHE` 失败分支跳过 RR；资源不足/物理不连续/未映射回退普通读。 | 上板看 `info` 的 `cr batches/pairs/declines/fallbacks` 与读回校验；若以后引入 HSQ/chain，先重审本风险；失败注入验证后续成员不会误完成 host 组。 | `src/ftl/cache_batch.c`；`src/ftl/ftl.c` `ftl_drain_group` / `ftl_host_io_callback`；`include/ftl/pending.h` `PENDING_OP_READ_CACHE` |

### 归档自 2T. FTL cache program（2026-10-23，host 正常路径已上板）

| ID | 状态 | 风险 / 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-CACHE-02 | 已改、host 正常路径上板；FAILC 负例未验 | **15h 中间页失败可能不可见**：状态寄存器 bit1=FAILC 表示**上一页** cache program 结果；旧 `CHECK_FAIL` 只查 bit0，会把中间页失败静默当成成功，末页再确认映射 ⇒ 静默坏数据。 | op44 与末页 op48 使用 `CHECK_FAIL_CACHE`（FAIL\|FAILC）；op48 开始前再查一次上一页 FAILC；首页 op49 只查 FAIL，避免残留 FAILC 误判。 | 上板做重复编程/坏块负例，确认 FAILC 能报错；未通过前不得声称 cache program 全部已验证。 | `include/nand/onfi_1_0.h` `NAND_STATUS_FAILC` / op48；`src/nand/onfi_1_0.c`；`src/nand/nand_fmc.c` |
| RISK-CACHE-03 | 已改、正常路径上板；失败注入未做 | **批次失败后仍有命令在飞**：若立即释放全部 pending/槽位，迟到完成会误撞新批次；host 组 outstanding 也会与真实在飞数不符。 | host：失败只置 abort + 关闭 hot SB，已完成的成员立即 IO_ERROR 结算，在飞成员由各自完成回调结算；GC：失败关闭 cold SB 后 abort，迟到完成走孤儿守卫 release。 | 故障注入：让中间页失败，确认其余成员不会误确认映射、组只报一次错。 | `src/ftl/ftl.c` `ftl_cache_wb_*`；`src/ftl/migrate.c` `mig_write_batch_complete` |
| RISK-CACHE-04 | 已改、正常路径上板；HSQ/chain 未引入 | **批次连续性依赖全 SQ / 无 HSQ**：4 条命令经 `ftl_op_submit_batch()` 一次入 SQ；若未来引入 HSQ 或 FC chain 插队，cache sequence 会被打断。 | `fc_submit_batch()` 先精确预检 SQ/HSQ 余量，全进或全不进；FTL 当前全部 `high_priority=0`。 | 引入 HSQ/chain 前重审本风险；上板验证失败注入下 batch 不会半提交。 | `src/fc/fc_interface.c` `fc_submit_batch`；`src/ftl/ftl_op.c` `ftl_op_submit_batch` |
| RISK-CACHE-05 | 已改、上板（正常路径） | **GC batch 增加 16 KiB `.bss`**：`mig_handle_t.batch_buf[4][2][2048]`；N=4 多实例构建可能更紧。 | 只在 `DIM=1` 生产构建启用；release/debug 体积已实测。 | 多实例/低 RAM 配置时改小 `MIG_BATCH_PAGES` 或改放外部 RAM。 | `include/ftl/migrate.h`；`make release` 体积 |
| RISK-CACHE-06 | 已改、正常路径上板；掉电注入未做 | **page0 空白恢复捷径**：page0 写失败、page1..3 已编程时，full rebuild 会把块当空白复用（RISK-BOOT-14）。 | `page0_programmed` 异步屏障 + **page0 补全**：wbuf 的 page0 命令按 full pair 写（缺失 plane 补零），GC 在某 plane page0 无数据时先写一页零补齐；恢复扫描 page0 空白时补探 page1..3。 | 上板做掉电注入；若 batch 上限 >4，必须同步扩大恢复探测范围。 | `src/ftl/allocator.c` `alc_super_open_note_page_programmed`；`src/ftl/wbuf.c`；`src/ftl/migrate.c` `mig_write_submit`；`src/ftl/power_failure_recovery.c` |
| RISK-CACHE-12 | 已改、上板（连续 `dst failure` 消失） | **page0 屏障曾对“page0 本来无数据”的 plane 永久返回 FTL_AGAIN**：GC 每 2 s stall 超时 → `dst failure — rewind pair` 逐次刷屏，且永远搬不动该 victim。 | wbuf page0 补全写 + GC dummy page0 写，让 page0 屏障最终可满足；`mig_dst_fail_cleanup` 的两条逐次日志降为 DEBUG（`mig_job_abort` 仍保留限流 WARN）。 | 已上板复跑（无连续 `dst failure`）；掉电/失败注入仍待做。 | `src/ftl/migrate.c` `mig_write_submit` / `mig_write_complete`；`src/ftl/wbuf.c`；`src/ftl/ftl.c` `PENDING_OP_WRITE_PAGE0` |
| RISK-CACHE-13 | 已改、上板（无重复 base mismatch） | **`batch plane base mismatch` 每个 GC job 重复**：`mig_job_start()` 的 `memset` 把 `batch_disabled` 清零，同一 victim 每个 job 都重新尝试 cache batch、再次 abort；板端表现为同一页反复 `[MIG] abort kind=gc ...`。 | `batch_disabled` 在 `mig_job_start` 的 memset 前后显式保留（对整个 boot 生效）；遇到任一页两 plane 基址不一致即关闭 batch，后续 job 走普通逐 plane 路径。 | 已上板复跑（无重复 mismatch）；失败注入仍待做。 | `src/ftl/migrate.c` `mig_job_start` / `mig_read_step` |
| RISK-CACHE-07 | 已改、正常路径上板；失败归因未注入 | **batch 失败无法可靠归因 plane**：`completion_all_cws` 给整条命令同一状态，不能区分 plane；按旧写失败路径会把两个 plane 块都标 BAD。 | cache batch 失败只关闭目的 SB，不调用逐 plane `bbt_mark_bad`；需要归因时用 `78h` 逐 plane 查状态。 | 故障注入确认只关闭 SB、不误标坏；若需精确标坏，补 `78h` 状态读取。 | `src/ftl/ftl.c` `ftl_cache_wb_fail`；`src/ftl/migrate.c` `mig_write_batch_complete` |
| RISK-CACHE-08 | 已改、正常路径上板；失败注入未做 | **host batch 失败曾无条件关闭“当前 hot SB”**：batch 提交后 drain 可能已写满旧块并开了新 SB，迟到失败会误关新 SB。 | `alc_super_open_get_pba_state() != ALC_SUPER_PBA_NOT_OPEN` 按 plane+block 比对；`ftl_cache_wb_fail` 只在 batch 页确实属于当前 hot SB 时关闭。 | 上板做 batch 中途失败 + 后续写入，确认只关目标 SB。 | `src/ftl/ftl.c` `ftl_cache_wb_fail`；`src/ftl/allocator.c` `alc_super_open_get_pba_state` |
| RISK-CACHE-09 | 已改、正常路径上板；瞬态背压未注入 | **GC batch 曾把瞬态 FTL_AGAIN 当永久失败**：host 忙时命令池/SQ 背压会让 GC 反复 abort。 | `mig_write_batch_step` 对 `FTL_AGAIN` 释放/归还后保留 batch 缓冲，用 `mig_job_stall` 重试；只有 `FTL_ERROR` 才 abort。 | 上板在 host 写压力下观察 `mb aborts` 不因瞬态背压增长。 | `src/ftl/migrate.c` `mig_write_batch_step` |
| RISK-CACHE-10 | 已改、正常路径上板；丢完成未注入 | **GC batch 失败等待曾无超时**：完成丢失时 job 可能永久 active。 | `mig_job_step` 对 batch_failed 等待加 `cmd_wait_ms` 硬 deadline；超时置 `paused` 并 abort，禁止新 job 启动，迟到完成由孤儿守卫 release。 | 上板/故障注入：丢完成场景下 GC 暂停而非卡死；重启恢复。 | `src/ftl/migrate.c` `mig_job_step` |
| RISK-CACHE-11 | 已改、上板（`info` 两行完整） | **`info` 单行曾超过 `log_vprintf` 192 B**，尾部计数会被截断。 | 拆成两行：`seq/pf/cr` 与 `cw/mb` 各一行。 | 已上板看 `info` 两行完整、无截断。 | `src/controller/cli.c`；`src/common/log.c` |

### 归档自 2U. ITCM / DTCM 热路径（2026-10-23，正常路径已上板；压力/掉电未覆盖）

| ID | 状态 | 风险 / 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-TCM-01 | 已上板验证（正常路径） | **DTCM 是 NOLOAD**：`g_ftl` 原先依赖 `.bss` 启动清零；移入 DTCM 后若不全量 memset，`flush_active/bg_suspend/l2p_defer/...` 会带随机值。 | `ftl_init` 入口 `memset(ftl,0,sizeof(*ftl))`；`ftl_global_init` 一次性复位文件级静态；`metadata.o` 的 `s_pool_rebuilt_logged` 在 `md_init` 复位；linker ASSERT 预留 20 KB stack。 | 上板启动、format、H2testw；若出现随机状态先查 memset 顺序。 | `src/ftl/ftl.c`；`src/ftl/metadata.c`；`linker/stm32h743xx_flash.ld` |
| RISK-TCM-02 | 已上板验证（正常路径） | **ITCM 代码未初始化**：H743 startup 原先不复制 `.ssd_code_high`，直接执行会跑飞。 | 在 H743 `Reset_Handler` 的 `.data` 复制之后、`.bss` 清零之前补 `__ssd_code_high_*` 复制循环，并加 `DSB/ISB`；ITCM 大小 release 61,632 B / debug 62,080 B < 64 KB（release 余量 3,904 B）。 | 上板启动必须越过 banner/USB；若 first fault 在 0x0 附近，先查复制符号/LMA。 | `third_party/stm32h743/startup_stm32h743xx.s`；`linker/stm32h743xx_flash.ld` |
| RISK-TCM-03 | 已上板验证（正常路径） | **DTCM 被 DMA 引用会静默错**：DMA1/2 不可访问 TCM；误移 FC buffer/NAND page buffer/staging 会让 NAND/USB 数据错。**`.noncacheable_buffer` 整体 72,064 B**（`g_fc_buffer_pool` 67,584 + `g_fc_return_staging` 256 + `g_nand_channel_page_buffer` 4,224），大于 DTCM 当前空闲 62,760 B ⇒ 整体搬 DTCM 容量也不成立；单搬 page buffer 虽放得下，但板端已证 multi-plane program 失败（RISK-TCM-08）。 | 只移 CPU-only 对象；`g_fc_buffer_pool`、`g_fc_return_staging`、`g_nand_channel_page_buffer` 全部固定 `.noncacheable_buffer`。 | `nm` 复查；上板 NAND 读写/H2testw；若再有人提议搬 DTCM，先看本条容量 + RISK-TCM-08 实测。 | `src/fc/fc.c`；`src/nand/nand_dispatch.c`；`src/ftl/ftl.c`；`linker/stm32h743xx_flash.ld` |
| RISK-TCM-04 | **已上板验证（正常路径）** | **DTCM 预算口径**：BCH 曾放 DTCM 使 `.dtcm_data` 到 105,808 B、只剩约 25 KB 堆/栈，出现 CP 不可读；页缓冲移出后 58,952 B 版本已 H2testw 通过（peak 2,964 B、heap 0）。当前交换版：BCH 42,656 B→DTCM、`g_ftl_md` 33,752 B→SRAM4，`.dtcm_data` 67,832 B，free≈63 KB；2026-10-23 启动 + H2testw 415/423 MB 通过。 | linker ASSERT `data + noinit + 20 KB <= DTCM`；页缓冲 AXI_NOCACHE；页级 scratch 移出栈。 | 压力/掉电场景仍按 RISK-TCM-07 复测 `stack`；若 headroom <20 KB 或异常，换回。 | `src/main.c`；`src/platform/syscalls.c`；`linker/stm32h743xx_flash.ld`；板端 H2testw 415/423 MB |
| RISK-TCM-09 | **已上板验证（正常路径）** | **BCH↔g_ftl_md 交换**：BCH 只读查表移 DTCM、`g_ftl_md` 回 SRAM4；预算 67,832 B + 248 + 20 KB = 88,560 ≤128 KB，free≈63 KB。风险：BCH 表紧贴 `end`（栈溢出会先踩表）、SRAM4 上的 md 访问延迟。 | 单变量交换，未改 C；页缓冲仍 AXI_NOCACHE。 | 2026-10-23 上板：`BCH ready`、boot 恢复、H2testw 415/423 MB 0 错误（843/887 KB/s）通过。压力/掉电仍按 RISK-TCM-04 复测；无收益或异常则换回原域。 | `linker/stm32h743xx_flash.ld`；`src/ftl/metadata.c`；`src/fc/fc_bch_engine.c`；板端 H2testw 415/423 MB |
| RISK-TCM-05 | **已上板验证（正常路径）；收益未证实** | **ITCM/FLASH 跨 128 MB 调用**：依赖 linker veneer；目标文件集合过大或热点跨段调用会引入 stub/relocation 失败。release 额外集按对象路径 `*release/obj/...` 匹配，debug 对象在 `debug/obj/` 不匹配 —— 不是 C 预处理器条件编译。 | 公共集：host I/O + L2P/wbuf/pending/cache read/write + FC pump/BCH + NAND dispatch/FMC（debug 62,080 B，94.7%）。release 额外集：`allocator/journal/metadata/metadata_job/migrate/gc/wear_leveling/bg_scheduler/ftl_op/fc_interface/fc_queue/fc_command/nand_engine`，release 61,632 B（余量 3,904 B）。DTCM 布局不变（67,832 B）。2026-10-23 H2testw 843/887 KB/s、415/423 MB 0 错误；相对 840/885（release 29,728 B）差 +3/+2 KB/s，属噪声；本次 `mig=0/0`、`gcm=0`，未触发 GC/迁移，额外集收益未覆盖。 | `make all` / `make release` 出现 ITCM overflow 即按目标文件裁 release 额外集；若改 `OBJ_DIR` 命名，先查 release map 的 `.ssd_code_high` 是否仍含额外模块（路径不匹配会静默回落到公共集）；**H2testw 是 USB FS + NAND 时序约束下的顺序负载，不是 ITCM 收益判据**——需用 BCH/metadata/GC CPU-bound 微基准或填盘后随机覆盖 A/B；GC/后台压力仍无可测收益则删额外集。 | `build/firmware/release/bin/release.map` / `build/firmware/debug/bin/debug.map` 的 `.ssd_code_high`；`linker/stm32h743xx_flash.ld`；板端 H2testw 415/423 MB |
| RISK-TCM-06 | **已上板验证** | **TCM 版 H2testw 写速曾 843→620 KB/s**。format A/B 定位到 **NAND 页缓冲放 DTCM**：multi-plane handler 收到正确 bitmap/data，但 NAND 读回全 FF；放回 AXI_NOCACHE 后 `format` 成功。修复后 H2testw **840/885 KB/s、415/423 MB 无错误**，`info avg=4209us`、`bad=0`。 | 页缓冲固定 AXI_NOCACHE；BCH 去堆并放 SRAM4；页级 scratch 移出栈。 | 若后续再出现写速/CP 回归，先查页缓冲是否又落回 DTCM。 | 板端 2026-10-23：baseline 843/889、页面缓冲 AXI 840/885、DTCM 下 format 全失败；`src/nand/nand_dispatch.c` |
| RISK-TCM-08 | **已上板验证** | **NAND 页缓冲放 DTCM 会静默破坏 multi-plane program**：handler 日志显示 `bitmap=0xff`、源 buffer 有效、page buffer 已填，但同地址读回全 FF；单 plane Anchor 写正常；8 个 CP 候选块全失败。 | 页缓冲固定 `.noncacheable_buffer`（AXI_NOCACHE）；不再按 `CONFIG_NAND_DMA` 条件放 DTCM。 | 不得再把页缓冲移到 DTCM；若要追求 PIO 性能，需先解释并证明 multi-plane 写仍成立。 | 板端 2026-10-23 A/B（DTCM format 失败 / AXI format+H2testw 通过）；`src/nand/nand_dispatch.c` |
| RISK-TCM-07 | 已判定（口径错误） | **“栈峰值 55 KB”没有实测依据**：唯一出处是 `docs/archive/ftl_ut_munit.md:50` 的 UT fixture 估计；唯一有串口证据的实测是 v0.75 `peak=5448 B`、`headroom=125224 B`、`alarm=0`。该数字被抄进 `syscalls.c`/`main.c`/`known_risks` 并写成“实测”。 | `main.c`/`syscalls.c`/文档改为“历史实测 5,448 B / 当前待复测”；DTCM 预留从 64 KB 降到 20 KB；`stack` 增加 `heap_used`。 | 当前版本上板 `stack` 复测；若 peak 明显高于 5.4 KB，按 RISK-TCM-04 调整。 | `docs/archive/ftl_v075_board_verify.md:205`；`src/main.c`；`src/platform/syscalls.c` |

### 归档自 2V. 板端读完整性事件（2026-10-23，观察中）

| ID | 状态 | 风险 / 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-RD-02 | 已判定（陈旧记录） | **`previous crash` 跨复位残留**：PIN 复位后仍打印上次 HardFault；本次 PC=0x08004d4a 落在当前镜像 `.ssd_code_high` 的 FLASH 装载副本，不是运行时地址（ITCM 运行时 0x0，`.text` 从 0x0800f580 起）。`.noinit` 只有断电才清。 | 断电重启后该行消失，确认是旧镜像记录；不得据它判定当前镜像崩溃。 | 若断电重启后仍出现新的 crash 行，按**当前 ELF** 的符号表解 PC/CFSR。 | `docs/design/fault_capture.md`；`linker/stm32h743xx_flash.ld`；板端日志 2026-10-23 |
| RISK-RD-03 | 已判定（陈旧记录，待观察） | 20:21 镜像首次 PIN 复位后打印 `MemManage PC=0x20006108 LR=0x08024923 CFSR=0x1`：PC 在 DTCM 数据区、`CFSR=1` 为 IACCVIOL（跳到数据地址）。随后 **POR 断电重启该行消失**，H2testw 415/423 MB 全过、无新崩溃。不能据此判定当前镜像有活缺陷。 | 记录保留；后续若断电重启后**再次**出现新 MemManage，按当前 ELF 解 LR/栈回溯；否则视为旧镜像 `.noinit` 残留。 | 新 crash 行出现时，抓 `crash`/`stack` 与当前 ELF 符号。 | 板端日志 2026-10-23；`src/platform/fault*.c`；`docs/design/fault_capture.md` |

### 归档自 2X. 日志格式化 — 编译器格式检查收口（2026-10-24，未上板）

| ID | 状态 | 风险 / 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-LOG-05 | 信息（2026-10-24，已上板） | **完整 newlib 的体积/依赖后果**：release `.text` +10080 B、`.data` +2012 B、AXI_RAM +2 KB；`malloc` 由 `mallocr.o`（dlmalloc）提供，浮点格式化经 `_dtoa_r` 依赖它。`--gc-sections` 仍丢弃未用的公共算法/数据结构代码。本板 2 MiB Flash，余量充足。 | `config.mk` 注释写明体积代价与选项；`syscalls.c` 注释同步为完整 `libc.a`。 | 若 Flash 余量收紧，重新评估 nano + 定点打印；换工具链时复核 `_vfprintf_r` 是否仍被链接。 | `config.mk`；`build/firmware/release/*.map`；`src/platform/syscalls.c` |

### 归档自 3. 类型 / 显式强转（H44，提交 `6050703`）

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-TYPE-01 | 已修未上板 | ~~release 下 `ftl_fc_interface.c` 的 `allowed` / `plane_bits` 只在 `ASSERT` 里使用 ⇒ 检查被裁空、两条 unused 告警；注释声称的 FC 侧 release 拒绝已不存在（FC 建链校验全 ASSERT）~~ | `ftl_cmd_validate_cw_bitmap` 改为返回 `ftl_status_e`：ASSERT + release 运行期拒绝（空集 / 越段 / multi-plane 未覆盖每个 plane）；两个建链入口统一处理 | 上板：release 下越段位图应被拒绝而不是发空链 | `src/ftl/ftl_fc_interface.c` |

### 归档自 判定归档（信息 / 已议）

| ID | 状态 | 风险 / 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-MIG-04 | 信息（2026-10-20 上板观测） | **format 后不久出现 `kind=2` predictive abort 是设计内行为**：`alc_bst_note_erase` 刻意保留 `rr.health`（慢死趋势跨擦除周期观察），`pfr_media_format` 不清 rr；carve 又把每 plane 前 50（Spare）+204（OP）个块先占走，首批 Free 块从 per-plane 约 300 起 ⇒ 格式化前的高 ECC 块正好落在这一段，predict 会最先读到并验证它们。读失败按 3 次标 BAD（本次 plane1=445→g2493、plane0=308），**不是新的活锁**（计数按 plane 源块累计后已收敛） | RISK-GC-07 / RISK-MIG-02 的 per-plane 计数修复；`bad=` 增长并隔离 | 观察 `bad=` 增长速率：只是 format 后清出旧坏块则正常；若持续快速增长则评估换芯片。若希望 format 后从零 rr 开始，可在 `pfr_media_format` 显式清 rr（当前保留是为趋势观察） | 板端 rev32 日志：`MIG abort kind=2 ... plane1 block=445` ×3 → `blk=2493 x3 — mark BAD`；随后 `plane0 block=308` ×3 → `blk=308 x3 — mark BAD`；`src/ftl/allocator.c` `alc_bst_note_erase`；`src/ftl/read_recovery.c` `rr_predictive_scan`；`src/ftl/power_failure_recovery.c` `pfr_boot_carve_pools` |
| RISK-BBT-08 | 信息 | `bbt_factory_scan` 跳过 gblock 0，依赖“Block 0 出厂总是好块”的器件假设 | 器件手册（Micron AN1819）支持 | 换器件时复核 | `src/ftl/bbt.c` `bbt_factory_scan` |
| RISK-BBT-09 | 信息 | CLI `bbt --erase` / `bbt_print_bad_map(do_erase)` 会擦全片，且运行时 BST 不更新 | CLI 先 suspend 后台并提示断电 | 只作为专用体检工具；不要放进开机路径 | `src/ftl/bbt.c` `bbt_print_bad_map`；`src/controller/cli.c` |
| RISK-BOOT-19 | 信息（v0.90 残余） | **范围化恢复不解算丢失 region 的 pre-CP 映射所在块的 valid_count**：这些 CW 物理仍在、L2P 已丢，块计数保持快照值（偏高）⇒ GC 选 victim 时略晚回收；无数据破坏。无法在无 P2L 索引下定位受影响块，全盘扫描又违背 L3 目的 | 接受：GC 迁移时逐 CW 反查 L2P，空洞不会被搬移；块被回收后自动归零 | 若观测到 `bad`/容量异常或 GC 长期不回收某些块，再评估按 region 反查 | `src/ftl/power_failure_recovery.c` `pfr_boot_power_failure`；`RISK-BOOT-18` |
| RISK-BOOT-20 | 信息（v0.90 残余） | **「丢失映射读返回 IO_ERROR」只在本次启动内可靠**：CP 把降级 region 重写成新页后，未回放的 LBA 变成“合法未映射”，下次启动读它们回到零填充。Phase B 的主备副本消除了**单副本**失效；两份都坏时仍无跨启动的 lost 标记（Directory/Anchor 无该位） | 接受：单副本失效已由 L2 覆盖；两份都坏属双故障，boot-scoped 报错 + 后续 CP 重建 | 若需跨启动保留，给 Anchor 增加 per-region `lost` 位（同一 v12 格式内扩展） | `src/ftl/l2p_cache.c` `region_degraded_bitmap`；`src/ftl/metadata.c` Anchor 编码 |
| RISK-WL-06 | 信息 | 磨损快照**逐页应用**：中途失败时前面的页已回填，其余保持默认（软状态） | Header 字段校验 + 每页 CRC + 日志 | 若要求整条原子恢复：32 KB 暂存或两遍读 | `src/ftl/checkpoint.c` `checkpoint_load_wear_table` |
| RISK-WL-09 | 信息（v0.91 残余） | **静态 WL 用擦除次数判定「冷数据」，未用块年龄 / 热数据分类**：动态 WL 会把刚写的热数据分配到低擦除块，WL 随后可能把这块当冷数据搬到高擦除块；热数据很快失效，搬移收益降低（额外写放大），但**不破坏数据**。公开资料也提示「低擦除 ≠ 冷」（US20220300184A1 / Rejuvenator MSST'11） | 选块同时要求有效 CW ≥ 70%，并优先 read_count 高者；搬移只在 `max_free_ec − min_used_ec ≥ 200` 时发生（不会常态搬迁） | 上板若观察到 `kind=wl` 频繁且 `free`/有效数据收益低：把块年龄（`last_pe_ts`）或 hot/cold 分类加入选块（先改策略，不改介质） | `src/ftl/wear_leveling.c` `wl_select_victim`；`docs/design/storage_controller_learning.md` §2.5（dhara 分支对照） |
| RISK-WB-05 | 信息 | `ftl_init` 的 `memset(&ftl->wbuf, 0, sizeof(ftl->wbuf))` **有意保留**，不换成 `wbuf_handle(RESET)`：init 期构造，`g_ftl` 在 `.bss` 已零；换 `wbuf_handle(RESET)` 会在 `pend_init` 已清组表后带着陈旧 `owners` 调 `pend_group_complete_subop_multi` ⇒ 风险大于收益 | 有意不改（P5 留痕） | 若 `ftl_init` 的顺序变化（`pend_init` 之前有 wbuf 写入）需重估 | `src/ftl/ftl.c` `ftl_init` |
| RISK-BG-02 | 已议 | `mig_gc_evaluate` 的 `gc_eval_last_ms` 节拍门与调度器 `interval_ms = GC_EVAL_INTERVAL_MS` **同源同值双门**（v0.76「节拍统一进任务表」的残留）。删任一门都不改变当前行为 | 保留双门 | 保留的理由是「调度器 interval 被误置 0 时仍不逐轮全表扫描」（本函数成本 = O(全表)）。若出现第二调用点或 interval 语义变更 ⇒ 重估是否合并 | `src/ftl/bg_scheduler.c` `mig_gc_evaluate` / `bg_a_gc_eval` |
| RISK-BG-03 | 信息 | 时钟缺失降级分支（`clock_ok == 0`）在本板**不可达**：`ssd_platform_register` 拒绝 `time_us == NULL` ⇒ 整条降级路径从未被验证（RISK-BG-01 的缺陷就长在这条不可达路径上，靠读码而非运行发现） | 分支保留（引擎通用性 + 防御） | 换平台 / 改注册路径时重估；不要在这条路径上堆策略 | `src/platform/platform.c` `ssd_platform_register`；`src/ftl/bg_scheduler.c` |
| RISK-BG-08 | 信息（有意设计） | `bg_suspend` 由诊断命令直接写 FTL 上下文字段（跨模块字段写，§3.1 形态） | 字段契约已写明「只由诊断命令置位、无清除路径」；判定已收进 `bg_scheduler_service` 首行 | 诊断命令增多 / 出现第二个写点时考虑加 `ftl_bg_suspend()` 闸门 | `src/controller/cli.c`（`bbt --erase`）；`include/ftl/ftl.h` `bg_suspend` |
| RISK-ARCH-01 | 信息（有意设计，2026-10-20 修订） | 共享数据面账本（L2P / BST / allocator / journal 水位 / metadata / pending / completion）保持共享 device context，模块只封装执行状态；**不再用 `*_port_t` 结构体**，模块 handle 只存实际需要的窄上下文指针，跨模块操作走 owner API | `../archive/ftl_module_contracts.md` 的契约与字段所有权表；typed handle + 窄上下文 + owner API | 若有人提出「模块私有账本/去掉共享 context」，先读契约文档；多实现/多 device 需求才重估 | `docs/archive/ftl_module_contracts.md` §1/§3.3/§8；`src/ftl/ftl.c` `contract_check_module_contexts` |
| RISK-ARCH-03 | 信息（有意设计） | 无模块级 liveness supervisor / 无 per-engine watchdog；IWDG 只兜系统级卡死 | 各 handle ms deadline + 主循环轮询 + IWDG | 若出现主循环卡死/漏 deadline 的实际案例，加主循环 progress monitor；**不要**在 ISR 里跑状态机或喂狗 | `known_risks.md` §6 设计；`src/main.c` IWDG |
| RISK-ARCH-04 | 已议（本轮保留 compact） | RR 的 desc 保留方案（本仓自拟、非主流）**未实现**；本轮按 §〇 主流判据保留 **compact 重下发**：失败读释放命令，RR 按 LBA/PBA 重新下发新命令 + 新 buffer。代价：需 FC descriptor 公开原语 + 所有权契约拆分；当前机制已验证可用 | `rr_handle` 只持有 `rr_owner_t`（owners/pending_idx/upper_buf/lba/n_cw）；compact 机制不保留失败命令的 desc | 若未来实测 desc 保留有收益（减少一次重读命令构建）再评估；否则维持 compact | `src/ftl/read_recovery.c`；`include/ftl/read_recovery.h` |
| RISK-ARCH-10 | 信息（有意偏离，2026-10-20 更新） | 为适配本仓 **per-instance** 架构，typed 入口有几处签名偏离设计文档原文：`jrn_handle` 增加 `jrn_trigger_e`（区分 FLUSH / ERASE_ONLY）；`wbuf_handle` 返回 `ftl_status_e`（调用者必须分支 FTL_AGAIN/ERROR）；`pend_confirm_*` 现收 `pend_table_t *` + `confirm_ctx`（不再收整个 `ftl_ctx_t *`）；`rr_handle` 的 `rr_trigger` 仍登记 owner 而非 desc 链 | 各偏差已在头文件与 P2/P5 注明 | 若未来引入设备级 pending/wbuf 上下文，再评估收回这些实参 | `include/ftl/journal.h` / `wbuf.h` / `pending.h`；`docs/archive/ftl_module_contracts.md` §3.3 |
| RISK-USB-04 | 信息（有意设计） | USB MSC/BOT 是**单命令在飞**；UASP 需要 USB 3.0 bulk streams，本板不可用 ⇒ 不能作为「多命令在飞」的学习载体 | 设计文档 §5 已写明；FTL 内部仍可把一条命令拆多个子操作并行 | 若目标是学主控队列/门铃，另做 NVMe 风格 SQ/CQ 软件模型，不要改 USB 路径 | `docs/design/usb_msc_udisk.md` §5 |
| RISK-ALLOC-01 | 信息（判定：有意设计，**勿内联**） | 合并前的 `alc_super_close_live` 只有两条语句（`sb->is_open = false` + commit），形态上像「薄包装」，但它是**唯一把 live Open SB 置为 Closed 的地方**：合并前的 `alc_closed_sb_append` 形参是 `const super_block_t *`，只改 `closed_sb_list` 里的**副本**（`entry.is_open = false`），不碰 `hot_sb/cold_sb`。若把这步展开到调用点或漏写 live 标志：① `alc_super_open_alloc_pair` 检测到 Open SB 含 BAD plane 时关闭后 `sb->is_open` 仍为 true ⇒ 不换 SB、继续从已关闭且含坏 plane 的 SB 取页；② `alc_super_close` 返回 FTL_OK 但该 SB 仍可继续写；③ 同一 SB 被重复关闭 ⇒ `closed_sb_list` 出现重复条目，GC 按陈旧的 valid_count 迁移 | 2026-10-23 已把 append + live 标志合并为唯一 `alc_super_close_live`（返回索引/`FTL_BLOCK_NO_SB`）；仍禁止展开到调用点（H32）。 | 新增关闭路径 / 把 `super_block_t` 改 opaque / 有人提议「删薄包装」时，先读本条 | `src/ftl/allocator.c` `alc_super_close_live`（唯一 live setter + 列表追加）；消费侧 `src/ftl/gc.c:273`、`src/ftl/wear_leveling.c:117` |
| RISK-MD-04 | 信息（静态口径） | **H15 跨模块欠账的权威登记点**（`CLAUDE.md` H15 只留指针，不记处数）。`src/` 下**手写 `status & FC_CW_STATUS_ERROR_MASK` 已归零**：原 `src/fc/fc.c` 的「首个失败 desc」聚合点也已改走 `fc_cw_status_has_error()`，剩余命中全是注释。⚠ 该项目曾按「**14 处 / 跨 10 个文件**」写进 `CLAUDE.md`，2026-10-21 复核实测为 **0** —— 计数型基线会随代码收敛漂移成**错误事实**，故此处只记 grep 口径 | 全仓统一 `fc_cw_status_has_error()` | 新增判 `completion->status` 成败的代码点直接用它；若再发现手写掩码，按 C2 收敛到该 helper，**不要**把处数写回文档 | `include/fc/fc_command.h`（helper）；复核 `grep -rnE 'status *& *FC_CW_STATUS_ERROR_MASK' src` |

### 归档自 已闭合

| ID | 闭合于 | 依据 / 证据 | 重开触发 |
|---|---|---|---|
| RISK-LOG-04 | 2026-10-24 | rev33 上板：`[SYS] clk sys=400 hclk=200 pclk=100 fmc=200 qspi=200 uart=100` 六项对齐；format→POR→H2testw 415/423 MB 无错误 | 时钟日志实参再增减 |
| RISK-REFACTOR-01 | 2026-10-24 | FTL 模块规范优化（migrate/pending/掉电恢复/读恢复/wbuf/WL/FTL↔FC 接口）+ 完整 newlib + 打印修饰符收口：format→PIN 复位→POR→H2testw 415/423 MB 写/校验无错误（844/888 KB/s），boot/STAT/诊断日志数值正确 | 再改日志修饰符或 FTL 重构后 |
| RISK-CAP-01 | 2026-10-22 | rev30 已上板；v12 全盘 H2testw 415/423 MB 通过 | 容量口径重新定义 |
| RISK-CAP-02 | 2026-10-22 | 固定日志区方案已被 v12（96 gblock，容量已扣）取代；实测 `total=48/plane` | 日志区布局再变 |
| RISK-CAP-03 | 2026-10-22 | boot `[CAP]` 动态容量实测（坏块 6 时 423 MiB）；H2testw 写满无超卖 | DIM>1 / 多实例容量核对 |
| RISK-CAP-04 | 2026-10-22 | v12 首启自动格式化 + H2testw 415 MB 无错误、`bad=0` | 介质布局版本再变 |
| RISK-MD-05 | 2026-10-22 | v9 介质已被 v10/v11/v12 取代；v12 全盘回归通过 | 回退旧介质 |
| RISK-MD-07 | 2026-10-22 | v12 固定日志区 96 块；近满盘（free=56）写+校验无写失败 | `live` 长期上千 / 写失败复现 |
| RISK-MD-08 | 2026-10-22 | 近满盘无 `submit stalled`；CP 递增到 1277 | 再出现 CP stall |
| RISK-MD-09 | 2026-10-22 | OP 借用 + 开机对账后，近满盘写+校验无死锁、`op=203/204` | 满盘 + 掉电时 borrow 计数异常 |
| RISK-MD-11 | 2026-10-22 | 压缩互锁修复后，近满盘无 `no_evictable` / `confirm park timeout` | 再出现压缩拿不到目标块 |
| RISK-MD-12 | 2026-10-22 | O(1) 稠密快路径后，跨 region 全盘写无 USB 掉线 | 主循环再出现百 ms 级停顿 |
| RISK-MD-13 | 2026-10-22 | 幽灵 victim 修复后，全盘写无 USB 掉线、无 `clear stale count` | 再出现 `clear stale count` |
| RISK-GC-09 | 2026-10-22 | 近满盘写+校验无 CSW failed；`free` 稳定 | GC 等待再被误判 IO_ERROR |
| RISK-GC-11 | 2026-10-22 | BLANK 检测 + `next_page` 截断后全盘写 `bad=0`（修复前曾 +264） | 重建路径仍按 64 页假设（见 RISK-NAND-06） |
| RISK-GC-12 | 2026-10-22 | 稀疏页 MATA 修复；全盘 H2testw 通过、`bad=0` | 映射指到未编程 CW 的残余 |
| RISK-GC-13 | 2026-10-22 | 满盘校验读速 297 KB/s（修复前 192）；`kind=gc` 不再持续 | 校验期 GC 空转复现 |
| RISK-WB-06 | 2026-10-22 | rev27 起上板；本轮全盘 H2testw 数据完整性通过 | wbuf 填装路径改动 |
| RISK-USB-01 | 2026-10-22 | 全盘 H2testw 415/423 MB 通过（写+校验）；枚举/拔插历史证据 | 掉电测试另见 RISK-USB-05 / RISK-BOOT-18 |
| RISK-USB-03 | 2026-10-22 | PA11/PA12 枚举实测（`[USB] mounted`） | 换板 / 换核心板 |
| RISK-USB-06 | 2026-10-22 | FLUSH 改异步后，H2testw 结束正常、无 `queue_event` 断言 | 再出现 USB 事件丢失 |
| RISK-WL-08 | 2026-10-22 | idle t=1510–1530 s 无 `kind=wl`、`mig=0/0` | 高磨损搬移另见 RISK-WL-04 / WL-09 |
| RISK-CP-06 | 2026-10-22 | host 资源预算后，持续写 + 校验期间 CP 递增到 1277 | 再出现 CP stall |
| RISK-CP-08 | 2026-10-22 | seq 拒绝 0/0xFFFFFFFF；实测 CP 持续递增、无 seq=0 stall | seq 接近 UINT32_MAX 回绕 |
| RISK-CP-09 | 2026-10-22 | Anchor 后转 `CP_HDR_PREP`；CP 递增、boot `seq=1` 正常 | 再出现 `done with invalid seq=0` |
| RISK-NAND-01 | 2026-10-22 | FCUT 55/55（Mode 5，2026-10-21） | 换器件 / 时序表 |
| RISK-NAND-02 | 2026-10-22 | 单一 3.3V 时序表；FCUT 55/55 覆盖 | 换器件 / 换电压域 |
| RISK-NAND-03 | 2026-10-22 | Mode 5 时序收紧后 FCUT 55/55（828.54 ms） | 再压 Setup/Wait |
| RISK-HOST-10 | 2026-10-23 | 已上板验证（2026-10-22 18:43:34）：`wr/tr/map` 打印正确；QWORD+`%lu` 修复 | 新增 QWORD + `%lu` 打印点 |
| RISK-PLAT-04 | 2026-10-23 | 已修并上板（2026-10-22 release）：release 启动无 MemManage，H2testw 通过 | 换编译器/优化档导致帧 >64 B |
| RISK-HOST-04 | 2026-10-23 | TRIM Journal-first；`RISK-HOST-08` 已板端验证写/擦/断电回放不复活 | TRIM 顺序或同步点变更 |
| RISK-GC-03 | 2026-10-23 | 与 `RISK-HOST-04` 同一缺陷；TRIM Journal-first 已由 `RISK-HOST-08` 板端验证 | TRIM 顺序或同步点变更 |
| RISK-LOG-01 | 2026-10-23 | 日志改为只有 `migrated>0` 才打一行 `[MIG] move`；`done` 不再存在（`de757e8`） | 重新引入逐 victim `done` |
| RISK-ALLOC-02 | 2026-10-23 | 静态收口：仅注释纠正，代码赋值正确 | `alc_super_close_live` 语义变化 |
| RISK-ALLOC-04 | 2026-10-23 | 静态收口：仅 rename，行为不变 | `alc_closed_sb_append` 语义变化 |
| RISK-ALLOC-05 | 2026-10-23 | 静态收口：删除 12 条冗余 debug 断言，release 行为不变 | 访问器重新引入运行期 NULL 断言 |
| RISK-TYPE-03 | 2026-10-23 | 静态收口：同类型 `(DWORD)` 强转已删除；`grep` 复核 | 重新引入同型强转 |
| RISK-BOOT-12 | 2026-10-23 | initial CP 完整快照；format 后首次 PF 走快照+journal（debug 2026-10-20 上板通过） | initial CP 格式变更 |
| RISK-PEND-01 | 2026-10-24 | 静态收口：`pend_group_t.count` 与 `total` 同值同义（C9 两套坐标）；两套 LBA 计数一旦漂移，`ftl_drain_group` 的 `remain` 会算错。已删除 `count`、统一用 `total`；`sizeof(pend_group_t)==48` 不变，`make all`/`make release` 零新告警 | 组进度再引入第二个 LBA 计数 |

---

### 归档自 2M. USB MSC / 平台

| ID | 状态 | 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-PLAT-06 | 信息 / 已确认（2026-10-24） | **「munit 名字行与 `[ OK ]` 之间多一个换行」不是固件输出**，只在长操作（6 s media 用例、开机 NAND 扫描、10 s 一次的 `[STAT]`）后的静默期出现。固件侧已排除：`_write` 缓冲区标记显示名字行 55 B（首尾均非 `\\n`）、结果行 22 B（首字节非 `\\n`）；`uart_log_write` 短写标记（1/2 B）在两者之间零命中；强制关闭 TX DMA（只走 TXE 中断）症状不变。若为真输出，唯一入口 `uart_log_write` 必然留下标记 ⇒ 换行由串口工具的「接收设置 → 自动换行」（值为 200）在静默后补入（用户截图确认）。影响：仅日志观感；曾按固件缺陷排查多轮 | 无代码改动（诊断标记已移除）；串口工具侧取消「自动换行」 | 若再出现：先看串口工具「接收设置 → 自动换行」；需要硬证据时用原始字节捕获（`xxd`）确认 0x0A 是否真在线上。若 xxd 证实在线，才回到 `uart_log_write` / 硬件方向 | 板端 2026-10-24：`<w37se>…<w16sE>` 序列（`src/platform/usart.c` `_write` 临时标记）；TX DMA 关闭对照；`src/platform/usart.c` `uart_log_write` 为唯一 TX 入口；串口工具截图「接收设置：自动换行 ☑ 200」 |

---

## 2026-10-24 FTL UT 上板闭合

| ID | 闭合于 | 依据 / 证据 | 重开触发 |
|---|---|---|---|
| RISK-MD-03 | 2026-10-24 | 旧冻结测试源已删除；新套件按 `docs/design/ftl_ut_design.md` 重建。板端 `test /ftl/pure/` 23/23、`test /ftl/media/` 5/5 全绿（media 22.5 s，含 format 10.4 s）；`make tests UT=FTL` 零告警 | 新套件再次与生产 API 漂移 |
| RISK-BG-09 | 2026-10-24 | `bg_engine` 用例随 `/ftl/pure/host/` 上板通过 | bg 任务表/档位接口再变 |
| RISK-PLAT-05 | 2026-10-24 | `LIB_DIR` 按配置分置后，UT 镜像上板 `test --list` 正常列出 28 条；普通镜像无 `test` 命令 | 构建目录命名再变 |
| RISK-UT-02 | 2026-10-24 | CP 完成断言改为「job IDLE + on-media Header = 用例起点→落盘序号」；板端 `journal barrier cp 1->2`、`trim cp 2->3` 通过（21:14:42） | CP 池/sequence 语义或 `cp_pool_find_latest` 回写行为变化 |
| RISK-UT-03 | 2026-10-24 | 测试隔离 BBT-18 改为「抬 sequence」而非擦 BBT 载体；`format_in_progress_resume` 在 21:44:05 镜像通过（8.10 s），不再有丢坏块图的失败路径 | 新用例需要隔离 BBT-18 |

---

## 2026-10-25 闭合：cache read 从未生效（RISK-HOST-13）

| ID | 状态 | 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-HOST-13 | 已闭合（2026-10-25 上板验证；修复见 v1.00） | **cache read 自实现以来从未生效**：`cache_read_collect` 用 `pba_page()` 做**跨 plane** 比较（`src/ftl/cache_batch.c:139`）。`pba_page()` 只清 CW 位、保留 plane/block 位（`include/ftl/ftl_config.h:447`），两 plane 的同一页号必然不等（实测 plane0 `0x00045000` vs plane1 `0x000CDC04`）⇒ `complete` 恒 false ⇒ `n_pairs=0` ⇒ 每个 ≥16 LBA 的读在组首 decline 一次后整组回退普通读。相邻 pair 的连续性判据（`:152`）同样错用 `pba_page()+1`：那是给 CW 位加 1，不是页号 +1。后果：读吞吐达不到设计值、`cache_read_batches` 恒 0；**无数据破坏**（回退普通读路径正确） | 已改 `pba_to_page()`（跨 plane 页号比较 + 页号 +1 连续性）。**板端 2026-10-25 验证**：`PROBE n=4`、`rd_path=cache`、`rdc(b/d/f)=1600/0/0`、`err=0`，2/2 用例通过 | 修正已上板；后续可在 H2testw 前后各 `info` 记录 `cr=` 确认 host 顺序读走 cache：① `:139` → `pba_to_page(pair_pba[p]) == pba_to_page(pair_pba[0])`；② `:152` → `pba_to_page(pair_pba[p]) == (pba_to_page(pba[j-1][p]) + 1U)` 且块号相等。上板须看到 `rd_path=cache`、`rdc(b/...)` 的 batches>0、`err=0`。**同步改 UT 的 `spd_ftl_probe_pairs`（同一判据的复刻）**。另：H2testw 前后各 `info` 记录 `cr=batches/pairs/declines/fallbacks`，确认 host 大文件顺序读修复后也走 cache | 板端 2026-10-25：`PROBE ftl pairs n=0 fail_j=0 reason=8` / `PROBE ftl map pba=00045000..,000CDC04..`（两 plane 页号均 0）/ `SPEED ... rdc(b/d/f)=0/1600/0`；`src/ftl/cache_batch.c` `cache_read_collect`；`include/ftl/ftl_config.h` `pba_page` |
| RISK-NAND-04 | 已闭合（2026-10-25，release -O2 实测） | **USB 写速度约 200 KB/s 的瓶颈未定位**。算术上界：Mode 0 字节访问 4+6+2=12 个 FMC 周期 ≈54.5 ns ⇒ 数据相 ~18 MB/s；4 KB pair 写 ≈ 数据搬入 230 µs + tPROG（典型 200 µs / 最大 600–700 µs）⇒ **4.6–8.4 MB/s**；多 plane 程序 UOP 内含 `WAIT_READY`（`min_busy_us=0`），不会死等 700 µs；只有单 plane ISSUE 走 700 µs 最小等待。USB FS 12 Mbps 理论上限 1.5 MB/s、BOT 实测约 0.8–1 MB/s。200 KB/s ≈ 每 4 KB 20 ms，比 USB 上限还低 4–5 倍 ⇒ 瓶颈不在 NAND 模式。候选：主机 WRITE(10) 的实际分块/往返（TinyUSB 按 `CFG_TUD_MSC_EP_BUFSIZE=4096` 分块，若主机按 512 B 下发则 BOT 往返占主导）、FTL 同步等待、journal/L2P/CP/GC 争用（测量时 GC 正处活锁） | rev31 已修 GC 活锁（RISK-GC-07），待复测 | ① rev31 复跑 H2testw，记录 `[GC]/[CP]` 频率与速度变化；② **已加诊断（2026-10-21，未上板）**：`usb_msc_write10_cb` 按数据相长度做桶计数（<=512B / <=4K / >4K），`info` 打印 — `[USB ] wr chunk calls=... <=512=... <=4K=...`；500ms 级 DWT 分段计时仍未做（需先看分块结论）；③ 用 `test /fc-nand/...` 的用例 µs 或 CLI 大块写读作 NAND 侧对照 | `src/controller/usb_msc.c` `usb_msc_transfer`；`third_party/tinyusb/src/class/msc/msc_device.c:888-905`；`include/tusb_config.h` `CFG_TUD_MSC_EP_BUFSIZE`；`src/nand/nand_fmc.c:289-294, 371-375`；`src/nand/onfi_1_0.c:881, 886`；**2026-10-22 板端**：debug 16 KB EP 322 KB/s、`[USB] latency avg=27.5 ms`；release 同配置 **写 673 KB/s / 读 758 KB/s**、avg=6.6 ms（全盘 H2testw 415/423 MB 无错误）⇒ `-O0` 是主因；剩余约 USB FS 总线 13.5 ms/16 KB + 设备侧 6.6 ms。下一步：EP 32 KB A/B 与 FC/NAND 子段计时；**2026-10-22 18:51:42** release + 1/4/5 批处理、近满盘 GC：写 **765 KB/s** / 读 **884 KB/s**、415/423 MB 无错误；断电重启后 `boot restored`、`bad=0`（当时 `dng` 为 0），`map 100/100000` 有映射、`rd` 无 IO_ERROR（写较空盘 673 +13.7%；读可能含 Windows 缓存；本次写阶段后已断电，`[USB] latency` **未取**，严格归因需同条件 A/B）；**2026-10-25 收口**：release -O2 H2testw 写 **843 KB/s** / 读 **889 KB/s**，200 KB/s 问题已消除；剩余上限归 USB FS（1.5 MB/s）与 CPU 数据面（RISK-PERF-04） |

---

## 2026-10-25 UT 全量后的收口（RISK-PEND-02 / IF-05 / BOOT-01 / CR-03 / ARCH-06）

| ID | 状态 | 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-BOOT-01 | 已闭合（2026-10-25：`preload_region_zero_and_high` 上板 85/85） | ~~F4 按 INVALID 行的 `region_id` 反查 CP bitmap ⇒ 全部别名到 region 0~~ | 新增 `l2p_cache_preload_region(cache, global_region, pba)`：由 CP bitmap **逐 bit** 找 INVALID 行、设 `region_id`、同步读、置 CLEAN/INVALID；F4 只对 resident CLEAN 行做 bitmap 失效 | 上板：region 0 与非 0 region 各预载一次，确认 `l2p_cache_find` 不返回别名、post-CP 映射可读 | `src/ftl/l2p_cache.c`；`src/ftl/power_failure_recovery.c` `pfr_boot_apply_region_bitmap` |
| RISK-IF-05 | 已闭合（2026-10-25：`op_ctx_capacity` 上板 85/85） | 旁表容量 `FTL_OP_CTX_TABLE_SIZE` 32 → `FC_COMMAND_POOL_DEPTH`（64）：**修复**原容量小于真实上界（表按 `command_id` 键，上界 = 在飞命令数 ≤ 池深）⇒ 原「满则 `FTL_ERROR`」路径实为可达，且 `bbt.c` 会把它当永久失败吞掉 BBT save | `src/ftl/ftl_op.c` 加 `_Static_assert(FTL_OP_CTX_TABLE_SIZE >= FC_COMMAND_POOL_DEPTH)`；RAM 代价 +256 B（8 B/项 × 32 项） | 上板：观察 `[FTL]` 心跳无异常；`ftl_op_ctx_count` 上界变 64 | `include/ftl/ftl_op.h` / `src/ftl/ftl_op.c` |
| RISK-CR-03 | 已闭合（2026-10-25：`/ftl/` 85/85 行为回归） | 9 个服务函数按 phase 拆 handler（纯代码搬移）+ 注释收敛（H37）；`ftl_checkpoint_callback` 额外把 `release + waiting=0` 提到 switch 之前（8 个 case 同构，`cp_job_abort` 不碰 `waiting`） | 判据 / 顺序 / 日志文案逐字不变；`switch` 的 `default` 副作用留在 dispatcher（§四） | 随常规回归上板（行为应逐字等价） | `src/ftl/{ftl,journal,bbt,read_recovery,checkpoint,metadata_job}.c` |
| RISK-ARCH-06 | 已闭合（2026-10-25：`/ftl/` 85/85 + H2testw 415 MB 回归） | 模块 handle 契约迁移**未上板**：只证 `make all` / `make release` 零新告警；2026-10-20 又删掉 6 个端口、提取 `pend_table_t`，行为保持更需板端回归 | 每个 Phase 保持行为；新增状态上报只读；`ftl_status_e` 增 `FTL_TIMEOUT`（未消费） | 上板回归：正常读写/GC/WL/RR/CP/journal/BBT/格式化/掉电重启；重点看完成路由、组结算与 `pending` 表提取后的索引 | `docs/archive/ftl_module_contracts.md`；本轮各模块 `*_handle` |
| RISK-PEND-02 | 已闭合（2026-10-25：`group_exhaustion` 上板 85/85） | **组槽满的错误路径失效**：`pend_group_alloc` 返回 `int`，失败时返回 `FTL_ERROR`（=1，非负），调用点判 `gi < 0` 恒假 ⇒ 把**组 1** 当成新组，覆盖在飞请求的 `total/next/on_complete`，完成通知错投/丢失。组可以没有 pending 槽（整段未映射读组从不进 outstanding），故 `ftl_cache_budget_ok` 的 pending 预检挡不住组槽耗尽 | 改为 `bool pend_group_alloc(pend_table_t *, BYTE *out_gi)`：状态与数据分离（H38），调用点判返回值；`gi < 0` 判据删除 | 组槽耗尽注入（放宽/绕过 pending 预检）：返回 false、打印 `no free group slot`、不触碰任何组；常规上板回归 | `src/ftl/pending.c` `pend_group_alloc`；`src/ftl/ftl.c` 的建组点 |

---

## 2026-10-30 闭合：P1 读流式跨窗口的 L2P 漂移判定（RISK-IF-14）

| ID | 状态 | 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-IF-14 | 已闭合（2026-10-30 上板） | **跨窗口 L2P 漂移（读流式）判定安全**：器件 `31h` 输出的是上一条命令地址页的下一页（预取页），序列隐含物理连续；窗口之间 GC/RR 改 L2P 时命令地址与预取页漂移。判定安全：读命令期间只有内容保持的重映射（GC 复制同内容、RR 恢复同内容；host 写由 BOT 串行），`erase` 提交在序列期间被门控、旧页在序列结束前不会被擦 ⇒ 预取页内容 = 当前映射内容；窗口内 `collect+submit` 无泵插入。TRIM 解映射 ⇒ 收集端 INVALID 直接放弃序列。 | 残余：ECC / read_disturb 按命令地址（新块）记账，预取实际读旧块——一次性统计偏差，不影响数据。 | 换器件 / 改 cache 读 op 序列 / 引入 host 并发写 / 放开 gate 时重开 | H2testw 417/423 MB **0 错误**（满盘 + 持续 GC，`mig=1` 贯穿校验；`cr=26509/104149/216/0`、`cw=25676/101452/7671/0`、`d4=24820`）；`include/nand/onfi_1_0.h:256-282`；`src/ftl/cache_batch.c` `cache_read_collect` |

## 2026-10-30 闭合：cache 序列并发门控 / ITCM 野写 / MPU 只读 / erase 判据

| ID | 状态 | 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-CACHE-14 | 已闭合（2026-10-30 上板/全套 UT 验证） | **并发下 cache 读窗口的 LUN 门控提前放开**：`cache_batch.c` 在窗口**提交成功**的瞬间就把 `cache_seq.active = false`，而 `ftl_op_seq_gate()` 只认 `active` ⇒ 上一窗口的 31h/3Fh 管线还没走完，别的组/后台的提交就能进同一 LUN。rd=1（单流）看不到；rd=4（4 组）200/1600 窗口读到 **plane 0 的擦除页**（`cst=0x40 UNC`、`badp=4/0`、`bm=0xFFFFFFFF`）。 | ① `ftl_cache_seq_t.draining`：窗口提交完置位，门控 `(active \|\| draining)` 继续关闭，直到所有者组在飞子操作归零（1 s 兜底）；② `ftl_cache_seq_release()` 按「是否还有在飞命令」决定是否继续关门控；③ 新增永久回归 `/fc-nand/lifecycle/block_cache_multi_plane_cross_block`（跨块 MP cache 读写，两 plane 各一个 block）。 | **下一步**：全套 `/ftl/` + H2testw 复核；若 H2testw 读速度下降明显，再评估能否在不破坏管线的前提下放开多窗口。 | 板端 2026-10-30：rd=1 0 错 / rd=4 修前 200 错、修后 0 错（`rdc(b/d/f)=1600/100/0`）；`src/ftl/cache_batch.c`（end_seq）、`src/ftl/ftl_op.c`（gate）、`src/ftl/ftl.c`（`ftl_cache_seq_release` / `ftl_cache_seq_idle_guard`）；全套 `test` 148/148（debug 22:43 镜像）；H2testw release 23:14 镜像 415/422 MB 0 错误、`cr=26475/104138/1793/0`（fallback=0）/`cw=25538/100912/7983/0`（abort=0）/`d4=18020` |
| RISK-L2P-08 | 已闭合（2026-10-30 上板/全套 UT 验证） | **不是 L2P 的缺陷**：`/ftl/pure/io/test_wbuf_density` 用 `wb_init(&h, …, NULL)` 构造无迁移器的 wbuf；该用例让 allocator 取不到页对 ⇒ `wb_flush` 调 `mig_gc_request_urgent(h->migrate=NULL)` ⇒ `m->urgent_gc = true` 写到 `0 + offsetof(mig_handle_t, urgent_gc) = 0x00004220`，**恰好落在 ITCM 的 `l2p_load_begin` 指令上**（`0x68BB ldr r3,[r7,#8]` → `0x6801 ldr r1,[r0]`，值 0x01）。之后任意执行到该指令的路径（`l2p_miss_unmapped_evict`）因 `r0=0xFFFFFFFF` 做非对齐字读 ⇒ UsageFault UNALIGNED。触发集：wbuf 用例在 `/ftl/pure/host/` 套件，`mem/+io/`、`md/+io/` 不含 host/ 故通过；完整 pure 跑到 host/ 才写下这一字节。证据链：18:55 `code:` 显示字节 `BB→01`；ITCM 改只读后 18:59 镜像当场 MemManage（IPSR=4、`MMFAR=0x00004220`、`PC=0x080336E0` = `mig_gc_request_urgent` 的 `strb r2,[r3,#0x220]`、`R3=0x4000`、`R2=1`）。 | ① `wb_flush` 对 `h->migrate` 判空（可选端口语义，`wbuf.h` 契约已写明）；② `mig_gc_request_urgent` 入口 `ASSERT(m != NULL)`；③ ITCM 保持 MPU 只读（RISK-TCM-10）。 | 重跑完整 UT（`test` / `test /ftl/pure/`）确认 wbuf_density 与 l2p 用例均过；上板 H2testw 复核正常写路径（判空只影响取页对失败分支）。 | 串口 dump 2026-10-30 18:59（MemManage/MMFAR/PC）；`src/ftl/wbuf.c` `wb_flush`；`src/ftl/migrate.c` `mig_gc_request_urgent`；`tests/unit/test_ftl_pure.c:1633` |
| RISK-TCM-10 | 已闭合（2026-10-30 上板/全套 UT 验证） | **ITCM（0x0..0xFFFF）在运行期是 FULL_ACCESS 可写**：野写落到这里会静默改掉指令，要等执行到那条指令才以「与写入点无关」的故障爆发（本例：`0x4220` 一个字节 `BB→01`，把 `ldr r3,[r7,#8]` 变成 `ldr r1,[r0]`，`r0=0xFFFFFFFF` ⇒ UNALIGNED；见 RISK-L2P-08）。这类破坏**编译期无声、断言无效**，且现场指向的 PC 具有误导性。 | MPU region 1 改为 `MPU_REGION_PRIV_RO_URO`（`src/platform/stm32h743/sys_port.c`）；ITCM 搬运在 `Reset_Handler`、MPU 使能在 `main` 之后，顺序安全。 | 跑完整 UT 与 H2testw：若出现 MemManage/写保护故障即说明仍有运行期写 ITCM 的路径；上板确认无正常路径依赖写 ITCM | `src/platform/stm32h743/sys_port.c` `g_sys_mpu_regions[]`；`third_party/stm32h743/startup_stm32h743xx.s` `CopyHighCode` |
| RISK-UT-07 | 已闭合（2026-10-30 上板/全套 UT 验证） | **`/fc-nand/lifecycle/block_erase_verify` 判据没随状态字重构改完**：擦除页裸读（`LDPC RAW` 跳 ECC、CRC 开）的 CRC/META 天然失败，状态为 `CRC|MATA`；新模型「成功 = 无错误位」下 `test_verify_cw_status_all_success` 用在擦除页上必然红（旧模型有独立 SUCCESS 位与错误位共存）。`FC_CW_STATUS_EMPTY` 只在 ECC 解码失败后识别，RAW 模式不识别。 | 测试侧修：`test_fc_level3.c` 新增 `verify_cw_status_erased`（允许 CRC|MATA|EMPTY，禁 FAIL/UNC/RAID），`read_page_verify` 按 `erased` 分流；`fc_data_path.h` 的 EMPTY 注释补 RAW 例外。生产代码未改。 | 重跑 `/fc-nand/lifecycle/block_erase_verify`；仍红再查生产侧 | `tests/unit/test_fc_level3.c`；`src/fc/fc_data_path.c` `fc_cw_is_empty`；板端 2026-10-30 `status=CRC|MATA expected UNUSED` |

---

## 2026-10-31 闭合：读/写序列跨 block + padding page0 屏障 + JSEG 掉电回放

> 上板镜像 2026-10-31 17:55 / 20:38（debug）。H2testw **415/415 MB 0 错误**（写 894 / 读 888 KB/s）；掉电回放多轮（空盘顺序写 130 MB；覆盖写 245 MB→删文件→140 MB；GC 压力 + 拔 USB），均 `boot type=2` + Journal 回放。本节含两批：cache 序列跨 block / padding 屏障 / JSEG 回放（CACHE-17/20/21/22/24），以及 v1.07/v1.08 的分配器 plane 平衡（ALLOC-10 归因）与 L2P 区域写回 WAL 次序（L2P-09）。

| ID | 状态 | 缺陷 / 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-CACHE-17 | 已上板验证（2026-10-31 掉电回放） | **JSEG 条目不存 `old_pba`，回放时由 L2P 基线现读**。正确性依赖「确认顺序 == 回放顺序」且 CP 快照水位与 Journal `page_seq` 一致；若 CP 水位判据错误，valid_count 增量会用错旧值 | 回放严格按 `(page_seq, 页内下标)` 排序；`entry_seq ≥ cp_sequence` 才记账 | **上板已验证（2026-10-31）**：format → H2testw 写 130 MB 时断设备电 → 上电 `boot type=2`（BOOT_POWER_FAILURE）、`journal_entries=8704`、`[CP] find latest seq=33 jrn_seq=259352`、`restore closed=1050 free=2335 anomalies=0`（block 账 exact：3385−1050=2335）；挂载后 H2testw verify **117 MB 0 错误**（存活数据零损坏；117→130 MB 的尾差主要是主机写回缓存 + 文件大小元数据未落盘，设备侧窗口无法用此方法分离，见 RISK-CACHE-18）。**第二轮（覆盖写 + 回放，2026-10-31）**：写 245 MB 掉电 → `boot type=2`、`journal_entries=3584`、`closed=1962 free=1423 anomalies=0`；删文件后再覆盖写 ~140 MB 掉电 → `journal_entries=7688`、`closed=3086（+2 anomalies→info 3088）free=297`，两次 `closed+free=3385` 均精确对账，`map 1000` VERDICT ok（覆盖写安全）、`dng=0`。**内容校验**（H2testw verify / `rd` 抽查未覆盖的旧区域）本轮未做。剩余：TRIM 后掉电、GC 压力下掉电未做。改 CP 水位/回放过滤时必须同时复核 | `src/ftl/journal.c` `jrn_page_replay_apply` / `jrn_ring_scan` |
| RISK-CACHE-20 | 已上板验证（能力生效；段长受分配布局限制） | **读序列跨 block 续读（31h 自增）**：datasheet 明确 31h 可跨 block（同 die；禁止跨 die/LUN），原判据要求「同 block 且 page+1」，比器件语义窄。现改为两种合法形态：同 block 的 page+1，或**两 plane 都 block+1 且 page == 0**（`cache_read_pair_contiguous`，收集与窗口末 lookahead 共用）。**单段上限 `FTL_CACHE_READ_MAX_RUN_PAIRS` = 128 pair**：读序列期间该 die 的 cache 寄存器被占用（ARDY = 0），程序命令发不进去，断点是后台元数据在本实例上的让步点（与写侧 Journal 配额同性质，不是器件约束）。风险：两 plane 判据写错会**静默读错页**（ECC 照过） | 判据按 datasheet 要求两 plane 同时成立；收集与 lookahead 共用同一 helper；吃到上限的窗口强制 LAST | **板端（2026-10-31，H2testw 通过运行）**：`cr=27435/105840/2583/430/0` ⇒ `pairs/starts`=**41.0**（改前 ~32）；`pf started=2891 hits=2891`（预取 100% 命中）⇒ region 边界已不是瓶颈；段长未到 128，限制在**物理不连续**——分配器按动态 WL 选块，写下的 block 大多不是物理 `block+1`。结论：跨 block 续读生效，41 ≈ 当前分配布局下的实际上界；要让段长继续上升需让分配器偏向物理相邻（另一设计决定）。**上板**：H2testw 后 `cr` 的 `pairs/starts` 应从 ~32 升至 65~128（受 128 上限与下一 region 驻留限制）；长顺序读期间确认 GC/CP 仍有进展（`mig=` / `cp=`）；若 GC 被饿死则调小上限 | `src/ftl/cache_batch.c` `cache_read_pair_contiguous` / `ftl_cache_read_try`；`include/ftl/cache_batch.h` `FTL_CACHE_READ_MAX_RUN_PAIRS`；`docs/reference/datasheet_mt29f_cache_ops.md` §2 |
| RISK-CACHE-21 | 已上板验证（2026-10-31） | **写序列跨 block 续写**（器件本无物理连续性要求：44/48h 每条自带地址）。改动：① block 到头不再置 `end_seq`——窗口仍 clamp 到 Open SB 剩余（一次构建不跨 SB，保 `alc_super_open_rewind_pair` 回滚前提），下一窗口由 `alloc_pair` 关旧块、开新块后继续；② 允许 1 页窗口开新序列（块尾 1..3 页不再回退 wbuf），`jrn_seq_begin` 预留量改为 min(4, room)；③ 收尾块时 `alc_super_can_open` 为 false ⇒ 仍在该窗口 END（否则序列停在「设备序列打开、无页可提交」：门控挡 GC、padding END 也无页可分配，RISK-CACHE-15 盘满例会中止已回执序列） | 上述 ①②③；page0 归属由 `cache_wr_build` 分配 page0 时置 `own_page0`（跨块后由新块分配重新置位）；逐页 FAILC 确认与 block 无关 | **板端（2026-10-31 通过运行）：写序列 1663 次 START = 1610 page_end + 51 padding + D4 断点，`aborts=0`、`pages/starts`=62.8，`free` 降到 55 写才由主机停止 ⇒ 跨 block 续写与块尾 1 页窗口正常；`wr−cw pages`≈2,019（≈1.2 页/块，Journal 页尾残差；消除项见 `known_iss.md` **ISS-FTL-15**）。早前失败运行：1663 次 START = 1575 page_end + 45 padding + 2 D4，`cw aborts=0`、`pages/starts`=62.9 ⇒ 序列/断点/收尾逻辑本身正常；该次运行在 405.6 MiB 处遇到 drain stall（与本次改动无直接证据关联，见 RISK-CACHE-22），未跑到读阶段。**上板**：`wr − cw pages`（wbuf 页数）应接近 0；`jf` 应接近 pairs/63（块尾 wbuf 页走普通 Journal 条目 `JRN_PAGE_LBA`，会先把当前 JSEG 页冻结——块尾页进序列后 JSEG 页才能填满 63 pair）；长顺序写期间确认 GC 正常；做一次新开 SB 的 page0 失败注入验 abort 收尾；任何改窗口 clamp / 回滚路径的改动都要复核「一次构建不跨 SB」 | `src/ftl/cache_batch.c` `ftl_cache_write_try`；`src/ftl/allocator.c` `alc_super_can_open` / `alc_super_open_rewind_pair`；`docs/reference/datasheet_mt29f_cache_ops.md` §5/§6 |
| RISK-CACHE-22 | 已闭合（归因 RISK-CACHE-24，2026-10-31 上板验证） | **根因 = RISK-CACHE-24**（padding 页 page0 的 plane1 屏障漏记）。本行保留诊断资产：第二跑 dump `sb_open=1 np=1 p0=01 wb_busy=1 pend=0 free=1489 fc 全空` 直接定位 | **满盘附近（~96%）写路径 drain stall 丢失全部写能力**：2026-10-31 板端 H2testw 写到 405.6 MiB（`free=135` 块）时 `[ERR] drain stalled gi=0 next=0/32` → 该 chunk 提前回执后 deferred error → 主机判「卷被外部更改」并反复重挂；此后 free 冻结在 135，小写也全失败。已排除：空间耗尽（还有 135 块）、序列状态残留（1622 次 START 全部有序收尾：1575 page_end + 45 pad + 2 D4）、停泊确认超时（无 `confirm park timeout` 日志）、Journal 写失败（无 `jrn` 失败日志）。**未归因**：`wb_flush` 的四个 AGAIN 来源（host 预算 / pending 池 / 分配 / page0 屏障）都没有日志 | 新增 `ftl_drain_stall_dump()`：卡死时打印 seq（kind/active/close/wr.count/next_lba）、jrn（phase/need_erase/ring_full/page_kind/entry）、wbuf 忙、pending 用量、free、Open SB（is_open/next_page/page0_programmed）、FC 池用量；`bg_progress` 补 `jrn_is_idle==false` 与「有停泊确认」两条确定性等待（防止正常页刷被 8 轮预算误判） | **下一轮上板复现即读 dump 三行归因**；若 dump 显示 `sb_open=1 np>0 p0=00` ⇒ page0 屏障卡死；`pend` 满 ⇒ 停泊泄漏；`fc` 满 ⇒ 资源泄漏；三者皆否 ⇒ 查分配器/序列门控 | `src/ftl/ftl.c` `ftl_drain_stall_dump` / `ftl_drain_group` 的 `bg_progress`；串口日志 2026-10-31 18:XX（`drain stalled` 段） |
| RISK-CACHE-24 | 已上板验证（2026-10-31） | **padding END 页落在新 SB 的 page0 时，plane1 的 page0 屏障漏记**：`ftl_cache_pad_callback` 用 `pba_with_plane(pad_pba0, 1U)` 从 plane0 推导 plane1 地址，而 Policy B 下两 plane 的物理块不同 ⇒ `alc_super_open_note_page_programmed` 的 plane+block 校验不匹配、直接丢弃 ⇒ `page0_programmed` 只置 bit0。后果：`wb_flush` 的 page0 屏障永远拒绝写该 SB 的 page>0（plane1）⇒ 所有 host 写永久 `FTL_AGAIN` → drain stall → deferred error → 主机判卷被改并重挂；只有断电/重刷才恢复（且因 CP 未推进，上次数据不可见，见 RISK-CACHE-23） | `ftl_cache_seq_t.pad_pba[PLANE_NUM]` 在 submit 时记下**真实**两 plane PBA，回调逐 plane 记账；不再从 plane0 推导 | **上板已验证**：H2testw 415/415 MB 0 错误；`pc=51`（padding 走 51 次）**无** `drain stalled`/`p0=01`，写一直持续到 `free=55` 由主机判卷满；`sw=1663/1610`、`aborts=0`、`pages/starts`=62.8。可归档。任何从单 plane 推导另一 plane 地址的 写法都要按 Policy B 复核（`pba_with_plane` 只适用于固定元数据镜像 = 两 plane 同块） | `src/ftl/ftl.c` `ftl_cache_pad_submit` / `ftl_cache_pad_callback`；板端 2026-10-31 第二跑 dump（`p0=01`）；`include/ftl/ftl.h` `pad_pba` |
| RISK-L2P-09 | 已上板验证（2026-10-31，GC 压力 + 掉电） | **L2P region 写回不遵守 WAL 次序**：确认路径是「Journal 追加到 RAM 页缓冲 → L2P 行原地更新」，两者的**落盘**相互独立；`l2p_flush_kick` 只做资源预检与槽位申请，**不等**覆盖这些映射的 Journal 页落盘。掉电后可能出现「区域页已落盘、Journal 页未落盘」⇒ 恢复时区域里的新映射生效，而 `valid_count` 只有 CP 快照（+存活的回放）的数字 ⇒ 计数偏低甚至为 0 ⇒ `dangling`（RISK-ALLOC-10）、极端情况下 GC 按 `valid_cw==0` 回收仍在被 L2P 引用的块 ⇒ **静默丢数据**。另：回放写入的计数与区域快照的新旧顺序也可能交织（RISK-CACHE-17）。**F4 的假设也被击穿**：`pfr_boot_apply_region_bitmap` 注释写「未置位区的 NAND 页为更旧值/从未写」，但 CP 之后被驱逐写回的 region 就是**更新**的，恢复会经目录懒加载到它（WAL 条目若丢失则计数缺） | **已改（2026-10-31，未上板）**：① `jrn_handle_t` 增加 WAL 水位 `persisted_seq`（页写成功时按「在飞页序号区间」推进，`jrn_freeze` 记录区间）；② L2P 行增加 `dirty_seq`（`l2p_write` / `l2p_entries_write` 在 Journal 追加之后记录当时的 `sequence_counter`）；③ `l2p_flush_kick` 在 `persisted_seq < dirty_seq` 时 `wal_defer++` 并返回 `FTL_AGAIN`（CP / 后台 flush 都把 AGAIN 当背压重试）；④ 行加载/失效时清零 `dirty_seq`；⑤ `info` 的 `cw=` 行加 `wd=` 观测。F10 仍只覆盖 GC 迁移 | **先静态复核，不必先复现**：① 确认恢复路径有没有校验「区域版本 vs Journal 水位」（若无 ⇒ 结构性漏洞成立）；② 修法：L2P 行记录最后修改的 Journal sequence，`l2p_flush_kick` 等到 Journal 持久化水位 ≥ 该序列（或先强制刷当前 Journal 页）再写回；③ **上板验证（2026-10-31，20:38 镜像）**：format 后基线全盘写+校验（`dng=0`/`rrab=0`、`wd>0`）；再删文件重写压到 `free<32`（GC 启动）时拔 USB 掉电 → 上电 `boot type=2`、`journal_entries=4608`、`closed=3354 free=28 anomalies=0`、**`dng=0`、`rrab=0`**；chkdsk 修正主机 FS（Code 1392）后 verify **127 MB 0 错误**。对比修复前同场景（`dng=10`/`rrab=1215`/LBA 0 MATA 错页/块退役）⇒ **闭合**。**2026-10-31 第二次上板证据**：`dng=10`、`rrab=1215`、LBA 0 读到 MATA 错页并被 RR 退役（data lost）、H2testw Code 34 ⇒ 「映射在、计数缺 → 漂移 → 错映射 → 数据丢失」链条已被板上观测到 | `src/ftl/l2p_cache.c` `l2p_flush_kick`（无 journal 依赖）；`src/ftl/ftl.c` `ftl_confirm_write`（RAM 追加先于区域写回）；`src/ftl/power_failure_recovery.c` `pfr_boot_apply_region_bitmap`（F4 假设）与恢复计数来源；板端 2026-10-31 `dangling lba=43627 st=CLOSED sb=284` + `dng=2` |
| RISK-ALLOC-10 | 已闭合（归因 RISK-L2P-09；2026-10-31 修复后同场景 `dng=0`） | **L2P 指向「CLOSED 且 valid_count=0」的块（dangling 守卫命中）**：覆盖写 LBA 43627 时打 `dangling lba=43627 old=0008a7fb g=1107 st=CLOSED sb=284 newg=941 nst=OPEN`，`info dng=2`。守卫按设计把「覆盖写撞 `valid_count>0` 断言」降级为一条日志（不崩溃，且本次覆盖写会把该 LBA 修好）。**后果**：若该块先被 GC 判为 valid=0 回收擦除，L2P 引用的旧数据会被静默擦掉。本次 `mv=0`（GC 没有搬移/动账）⇒ 漂移来自更早（上一运行写入或上一次 CP 的 RAM 状态），不是本轮 GC 造成。**2026-10-31 第二次上板（同一镜像）进一步升级**：`dng` 10、`rrab=1215`（1215 次读到已坏块）、t=932s 读 LBA 0 命中 `MATA|ARDY|RDY|WP`（页 META 与期望 LBA 不符）→ RR 退役块 1023「data lost」、`bad 7→8`；H2testw 校验在 offset 0x16d00000 收到设备读错（Code 34）⇒ **漂移已造成真实数据丢失**，不再只是计数不一致  | 守卫 + 覆盖写自愈；`dng` 计数在 `info`/`diag` | **根因 = RISK-L2P-09**（区域写回未按 WAL 次序等 Journal 页落盘）。**2026-10-31 修复后同场景复测**：GC 压力 + 掉电 → `dng=0`、`rrab=0`、verify 0 错误 ⇒ 闭合。其余排查方向（`ftl_confirm_write` 原子性、GC 空块判据、CP cut 一致性）未发现独立问题，保留为后续复核项。**原排查方向**：① `map <lba>` 看 PBA/块状态；② 复核 `ftl_confirm_write` 的 add/sub 与 L2P 更新在「部分锁存」下是否可能只做了一半（当前所有 AGAIN 都在动账前，但不是事务）；③ 复核 GC 空块回收判据（`valid_cw==0`）是否可能漏掉仍被 L2P 引用的块；④ 复核 CP 快照与 L2P region 刷写的 cut 一致性 | 板端 2026-10-31 20:13 镜像日志；`src/ftl/ftl.c` `ftl_confirm_write` 的 dangling 守卫与 add/sub；`src/ftl/gc.c` / `migrate.c` 空块回收路径 |
| RISK-MIG-08 | 已闭合（归因 RISK-L2P-09，2026-10-31） | **H2testw 全盘通过但 `[FTL] dangling` 指向 `st=CLOSED` 且 `valid_count==0` 的块**：日志出现大量 `dangling lba=... g=1777 st=CLOSED sb=1463 newg=... nst=OPEN/CLOSED supp=...`；L2P 仍指向该块，但块计数为 0 ⇒ GC 把它当空块回收，`free` 起伏但没有 `[MIG ] move`，`mig=0/0`。H2testw 只校验**本次新写的数据**，不证明旧映射/GC 迁移一致；若该块仍有 live CW，后续擦除会造成旧数据/映射静默丢失 | 新增 `diag mv/nop` 区分真实搬移与空块回收；`addr g <g>` / `map <lba>` 可现场核对 `valid`；当前未修，先保留证据 | 下次启动后若 `dng>0` 或 `free` 在无 `[MIG]` 时跳升：① `map` 一个 dangling LBA，确认 `valid==0`；② 检查 CP 快照的 block valid 与 L2P region flush 是否同一 cut；③ format 后只做一次 H2testw 复现并比较 `[BLK] restore`/`diag dng`；④ 若 `dng==0` 但 `mv==0`，再查 GC 选块/有效计数 | 原 `RISK-MD-10` 重开；`src/ftl/ftl.c` `ftl_confirm_write` 的 dangling 守卫；`src/ftl/power_failure_recovery.c` `pfr_restore_blocks` / `pfr_probe_blocks`；`src/ftl/checkpoint.c` `cp_job_begin` / `cp_block_snapshot`；板端 2026-10-23 23:49:55 H2testw 日志 ；**2026-10-31 归因**：与 RISK-ALLOC-10 同一签名（`st=CLOSED` + `valid_count==0` + `mig=0/0`）⇒ 根因 = RISK-L2P-09（L2P 区域写回未等 Journal 落盘）；v1.08 修复后同场景复测 `dng=0` |

---

## 2026-11-01 闭合：早回执模型（VWC）验收 + UT 介质会话状态

> 板端：release -O2 全量 UT 镜像 `built 04:31:58`，`test` **156/156**（3 skipped，237.9 s）。
> `/speed/ftl_rw` chunk=8/16/32 写 **3329/3179/3208 KB/s**、读 3379/3330/3386 KB/s（`rd_path=cache`、`err=0`）；
> FC 上下限同镜像：写 3639 normal / 4281 cache、读 3500/3557。debug -O0 同用例 chunk=32 写 1222 KB/s（旧 142）。

| ID | 状态 | 缺陷 / 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-CACHE-25 | **已上板验证（2026-11-01）**：release -O2 全量 UT 里 `/speed/ftl_rw` chunk=8/16/32 写 **3329/3179/3208 KB/s**、读 3379/3330/3386 KB/s（`rd_path=cache`、`err=0`）；同用例 debug -O0 chunk=32 = 1222 KB/s（旧 142 KB/s、`wr_ms=180001`）。FC 上下限同镜像：写 3639 normal / 4281 cache、读 3500/3557 | **同步主机的单命令写会等 1 s 空闲守卫**：写序列跨 host 命令保持打开（段长才能到 JSEG 页容量 ≈63 页），尾页要等**下一段**的 FAIL_CACHE 结算；同步主机（`ftl_submit_write` + 等 `complete_flag`，且同一时刻只有一条命令在飞，如 `/speed/ftl_rw` 原 cache 档 depth=1）在组完成前不会发下一段 ⇒ 只能由 `FTL_CACHE_SEQ_IDLE_MS`（1 s）触发 padding END。板端证据：cache 档 `wr_ms=180001`（phase 180 s 超时）、`wb=185` 批 ⇒ **0.97 s/命令**；normal 档不触发序列，1328/1351 KB/s 正常。读序列同理会让随后的写等最多 1 s | **目标主机（USB BOT）不受影响**：写提前回执 + 控制器双槽，下一 chunk 约 19 ms 到达，序列正常续接（H2testw 415 MB 0 错误）。UT 侧已按真实主机建模：`/speed/ftl_rw` cache 档改为 **16 LBA × 深度 2**（arena 16 KiB 上限），不再用 depth=1 | **2026-11-01 已改**：FTL 早回执（RISK-CACHE-27）后，组在**页编程完成**时就完成 ⇒ 同步主机的 `complete_flag` 不再等后继 FAILC，1 s/命令消失；尾页的**证明/确认**仍由下一段或 padding END 推进（不影响 host 完成，只影响 flush 屏障等待）。**上板判据**：`/speed/ftl_rw chunk=32/depth=1` 回到 ~1200 KB/s 量级、`wr_ms` 不再 180 s。⚠ 尾页若长期无后继且未 flush，ring 会留在内存里直到 idle 守卫 padding 收尾（正常路径 ≤1 s，不阻塞 host） | `src/ftl/cache_batch.c` `cache_batch_cmd_end` / `end_seq`；`src/ftl/ftl.c` `ftl_cache_seq_idle_guard`；`include/ftl/ftl.h` `FTL_CACHE_SEQ_IDLE_MS`；板端 2026-10-31 23:15 `/speed/ftl_rw` 日志；`tests/unit/test_speed.c` `wchunk/wdepth` |
| RISK-CACHE-19 | **v1.10 形态已上板**（2026-11-01：隔离 media 36/36 + release -O2 全量 UT 里 arm=1/2/last 全过）；旧形态 2026-10-31 已上板 | **cache 序列自开 block page0（`own_page0`）依赖"有序管线自证"**：允许序列在 page0 尚未确认时继续写 page>0，前提是 page0 的 FAIL_CACHE 失败会中止整个序列、其后页一律不确认（保住 rebuild 的「page0 空白 ⇒ 整块空白」捷径）。若改动 `ftl_cache_wr_member_done` / settle 顺序、或出现跨路径确认（例如旧页重试）使 page>0 先被确认，捷径会被击穿：rebuild 跳过该块 ⇒ 丢失仍被 L2P 引用的数据 | `get_room_seq` 只在 `own_page0`（本序列写的 page0）时绕开 page0 闸门；settle 成功才 `note_page_programmed`（内部按 plane+block 校验）；失败路径统一 abort 并结算未确认页；wbuf/GC 独立程序者仍走 `get_pba_state` 屏障 | **2026-10-31 正常路径已验证**（415 MB 运行中含 page0 自开 + padding 页落 page0）。失败侧：`fi_cache_fail_arm` + UT `cache_failc_abort`（arm=1/2/last，命令 K 结算上一页）断言「其后页不确认、已确认前缀保留、重启后可见性不变、恢复写可落盘」，**2026-10-31 板端全过**。**2026-11-01 v1.10 重写**：页改为完成时入表（早回执），FAILC 也中止序列；abort 只标记**未证明**页（器件已证明的页保留），并新增**序列代号守卫**丢弃中止后迟到完成（否则迟到页会被确认、击穿本项捷径）——本项判据在 v1.10 形态下**需重新上板**（`cache_failc_abort` 三档）。deferred error 的控制器/USB 端到端仍归 BOARD（`RISK-USBC-01`） | `src/ftl/allocator.c` `alc_super_open_get_room_seq`；`src/ftl/ftl.c` `ftl_cache_wr_confirm_step`（旧 `ftl_cache_wr_settle_idx` 已随 v1.10 删除）；`src/ftl/cache_batch.c` `own_page0` |
| RISK-UT-08 | **已改、已上板验证（2026-11-01 release -O2 全量 `test`）** | **同一上电周期内连跑多条 `test` 命令时，media 套件不重新格式化**：`s_media_ready` 是 `tests/unit/test_ftl_media.c` 的**进程级 static**，只在首次 `media_prepare` 置位；而全量 `test` 会先跑 `/fc-nand/lifecycle/` 的原始块测试（直接用 FC 擦写任意块），**改写 live FTL 的元数据区**。于是第二条 `test`（或全量 `test`）里的 media 套件在一份被改写、又跳过格式化的介质上运行。**板端证据（2026-11-01 镜像）**：先 `test /ftl/media/`（格式化、36/36 过）→ `/speed/ftl_rw`（自己格式化、1222 KB/s）→ 全量 `test`：`format_and_roundtrip` 只花 **14.62 ms**（隔离跑是 21.14 s ⇒ 确实没格式化），随后 `cache_failc_abort arm=last` 在「重启后读失败页」断言（`test_ftl_media.c:1096`）失败；**同一镜像上隔离跑 media 36/36 全过** | `ut_run_all()` 每次进入先调 `ftl_ut_media_session_reset()` ⇒ 复位 `s_media_ready`：每条 `test` 命令内 media 首次必格式化一次（与文档「media 内部先 format」一致）。与 RISK-UT-04 同族（FC 原始块会毁 FTL 状态）。**2026-11-01 上板复跑**：全量 `test` 的 `format_and_roundtrip` 耗时 10.28 s（release -O2 的格式化量级）⇒ 每条命令确实重新格式化了；156/156 全过 | **上板复跑**：① 上电复位后直接全量 `test`（media 会先格式化）⇒ 期望全绿；② 上电复位后 `test /ftl/media/` ⇒ 期望 36/36。⚠ 若①在**已先格式化**后仍复现 `arm=last` 的「重启后读零」失败 ⇒ 不是本项：转 `RISK-CACHE-28` 排查撤销持久化，用 `map 864304` 分流（失败页 = 864304..864311）：INVALID ⇒ region 加载失败/降级；有 PBA ⇒ undo 条目丢了 | `tests/unit/test_ftl_media.c` `s_media_ready` / `media_prepare` / `ftl_ut_media_session_reset`；`tests/unit/ut_main.c` `ut_run_all`；板端 2026-11-01 全量 `test` 日志 |

---

## 2026-11-12 结构化重写：已闭合详情搬迁

> 主文件已改为「索引 + 详情块」；以下条目已闭合，详情保留在此（原文照录）。

| ID | 状态 | 风险 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|
| RISK-MD-17 | **已关闭（2026-11-02：status 上报层删除）** | 原风险：`status.error` 看不到整理 job 的最近一次中止——`md_handle_abort` 置 `FTL_ERROR`，但 `md_handle_step` 的 IDLE 分支每次进入 IDLE 先写 `FTL_OK`，采样前已被清。**现状**：`md_status_t` 与 `*_handle` 的 out 已删除，不再有对外 error 字段；`handle.error` 仍在进入 IDLE 时清（模块内部行为） | abort 仍打 `[META] md job abort phase=...` 日志；UT `test_md_status_error` 改为直接断言 `d->handle.error == FTL_OK`（原语义保留） | 若将来重新引入对外 error 上报：把清除点移到「真正启动新 job」处，并补负例 | `src/ftl/metadata_job.c`：`md_handle_step`（IDLE 清 error）/ `md_handle_abort`；`tests/unit/test_ftl_pure.c` `test_md_status_error` |
| RISK-BOOT-27 | **已关闭（2026-11-04）** | 原风险：initial CP 判据（`block_word_count==0`）把 CP 格式知识复制进 PFR，格式演进后会静默退化。**查证结论**：该判据只在 `d2e4e1e` 之前的“空 initial CP”介质上可达；`d2e4e1e` 起 `cp_header_fill()` 恒写 `block_word_count=FTL_BLOCK_TOTAL`，随后 `4f68ede` 把版本推到 v12（当前 14），更老介质会被版本门自动格式化 ⇒ **当前固件下没有可达介质**。重构时整段删除（含 `force_rebuild` 参数），不再保留兼容分支 | 无：fallback 在空白介质上走 page0 快路径（~400–550 ms），不需要捷径 | 若将来要支持 v14 之前未经格式化的介质（不支持），才需要恢复兼容判定 | `src/ftl/power_failure_recovery.c`（已删分支）；`src/ftl/checkpoint.c` `cp_header_fill`；`git show d2e4e1e` / `4f68ede` |
| RISK-BOOT-29 | **已关闭（2026-11-04）** | 原风险：`pfr_boot_recovery` 注释描述已删除的只读取证，读者误以为运行期仍在采集。**现状**：入口重写为 scan → plan → execute，该注释随旧函数删除 | 无 | 无 | `src/ftl/power_failure_recovery.c` `pfr_boot_recovery` |
| RISK-CP-12 | **已关闭（2026-11-04）** | **注释与当前 initial CP 格式不符**（原判定）：`cp_block_table_load` / `cp_wear_table_load` 写「`block_page_count==0` / `wear_page_count==0` = initial CP 无快照」；但 `d2e4e1e` 起 initial CP 写的是完整 21 页，只有 v12 前旧介质才会为 0。**现状**：注释改为「旧记录 / v12 前空 initial CP」，并注明当前 initial CP 含完整块元数据 + 磨损快照 | 仅注释，不影响行为 | 无（格式再变时同步） | `src/ftl/checkpoint.c` `cp_block_table_load` / `cp_wear_table_load`；`include/ftl/checkpoint.h` 契约；`git show d2e4e1e` |
| RISK-HOST-18 | **已闭合 / 上板 H2testw 通过（2026-11-12）** | **H2testw 全盘写+校验在近满盘触发确认超时与已回执写丢失**：405.2 MB OK / **756 KB DATA LOST（1512 sectors）**，首错 offset `0x000834b000`（读回全 0）。固件时间线：free 3307→23；`predictive relocate retired g=388/396`；GC start free=31；`[WRN] [CP] submit stalled 10ms — abort phase=1 free=3207 cmd=1 buf=4`（池几乎空 ⇒ 不是池耗尽）；`[ERR] confirm park timeout lba=302240 idx=0 parked=6`；`[ERR] [USB] write chunk failed after ack ... deferred`；USB 中途重新 mount；`[WRN] [MIG] abort kind=gc ... — l2p load timeout fc=0x000000e0(ARDY|RDY|WP)`（WP=1=未保护，非写保护）；`[WRN] [L2P] load start failed region=... cause=submit_backpressure` 持续；结束时 free=23、mv=5710、nop=65。**机制**：`ftl_op_seq_gate`（`ftl_op.c:376-390`）在 host cache 序列 active/draining 且非 `owner_submit` 时拒绝**所有** op；confirm 写回依赖的 L2P region 加载（FTL_OP_L2P）被拒且**不置 `break_requested`** ⇒ 加载永不提交 ⇒ `confirm park timeout`（100 ms）⇒ host 写按 IO_ERROR 结算；早回执的写随后编程失败 ⇒ 已确认数据丢失。RISK-HOST-16 的「无进展」预算救不了：L2P 加载根本没提交，没有进展源。**2026-11-12 第二次（NTFS partitionless，13:21:34 镜像）**：H2testw 在 file offset `0x15700000`（343 MiB）报 **Code 1006（卷被外部更改）**，写入 943 KB/s；串口 `[CP] submit stalled 10ms — abort phase=1 free=3233 **cmd=0 buf=0**`、`[META] md job submit stalled 10ms — abort`、`confirm park timeout lba=829952 idx=0 parked=8`、两条 `write chunk failed after ack ... deferred`。**cmd=0/buf=0 证明 FC 命令池/缓冲池被 host 写全部占用**，不只是序列 gate；后台 CP/L2P 连资源都申请不到 ⇒ 确认超时 ⇒ 已回执写丢失。 | **已改（2026-11-12，未上板）**：① `cache_wr_build` 入口复用 `ftl_fc_host_budget_ok_n(n_pages)`（此前只有 wbuf 路径调，cache 写序列漏检）；预算从 2 pair 提到 **4 pair**（`FTL_FC_HOST_RESERVED_*`：8 cmd / 32 desc / 32 buf / 8 SQ）——host 写达到阈值即 `FTL_AGAIN`，后台始终有提交配额；② `ftl_op_seq_gate` 对 **`FTL_OP_L2P`** 也置 `break_requested`（confirm 依赖的加载会请求段边界收尾）。**13:50:08 镜像复测仍失败**（`cmd=0 buf=0`、free=487、confirm park timeout）⇒ host **读**路径未走预算；18:04:36 补：③ `cache_read_build` 与 `ftl_issue_subop` 读分支都加 `ftl_fc_host_budget_ok*`；④ CP stall 日志新增 `held ops` 分布（host/l2p/cp/md/gc/wl/jrn/pad/bbt/rr/sync），下次失败可直接看持有者。早回执写失败仍以 deferred error 报给 host（非静默错值） | **上板结果（18:04:36 镜像，2026-11-12）**：H2testw 1.4 `Test finished without errors`（406/420 MB，写 **940** / 读 **885** KB/s，`DATA LOST=0`）；结束时 `free=23`、`pend slot/group/req=0`、`cmd/buf` 未被打空。判据全部满足：① 无 `cmd=0 buf=0` / `confirm park timeout` / `write chunk failed after ack` / Code 1006；② `DATA LOST=0`。UT 待补：被拒 L2P 加载请求断点、预算阈值回压 | H2testw 1.4 输出（Writing 928 / Reading 861 KB/s，406/420 MB tested）；板端 2026-11-12 串口日志；`src/ftl/ftl_op.c:376` `ftl_op_seq_gate`；`src/ftl/l2p_cache.c` `l2p_load_submit`；`src/ftl/ftl.c:680-720` `ftl_confirm_service`；同表 RISK-HOST-05 / RISK-HOST-16 / RISK-CP-04 |
| RISK-UT-01 | 已闭合（2026-11-11 板端 SKIP 行格式正确） | **munit 只给 `[ OK ]` 闭合方括号并换行**：`" ] [ 耗时 ]\n"` 写在 `report.successful > 0` 分支内，`[ SKIP ]` / `[ FAIL ]` / `[ ERROR ]` / `[ TODO ]` 只打出左边 `"[ SKIP "` 就返回，下一条用例名接在同一行（板端实测 `[ SKIP  /ftl/pure/io/wl_victim_threshold: ...`）。后果：跳过/失败行与下一条混排，容易把 SKIP 读成通过 | 把「闭合 + 耗时 + 换行」移到 if/else 链之后，对所有结果标签统一执行；耗时平均仍按 `report.successful` 分支保护（0/1 时取单次） | **复现条件**：`l2p_miss_unmapped_evict` 在 cache 行有 DIRTY/FLUSHING/LOADING 时按设计 `MUNIT_SKIP`（`tests/unit/test_ftl_pure.c:1768-1776`）。同一上电先跑一个写介质的 media 用例（如 `cross_region_roundtrip`）再跑 `test /ftl/`，即可验证 SKIP 行格式；FAIL 行可临时改一条断言造负例（C7）。2026-10-24 20:08 镜像跑出 77/77、0 skipped ⇒ 本轮走的是 OK 分支，修复未获证据（R1） | `tests/framework/munit.c` `munit_test_runner_run_test_with_params`（结果打印尾部）；板端 2026-10-24 76/77 输出的 `[ SKIP  ...` 行；`tests/unit/test_ftl_pure.c` 的 SKIP 条件 |
| RISK-UT-05 | **已闭合（2026-11-03：DWT 删除，munit 计时改用平台 `time_us`）** | **munit 的 `[ OK ] [ 耗时 ]` 与 Total wall-clock 在长用例上按 10.737 s 取模**：DWT CYCCNT 32-bit @400 MHz ⇒ 回绕 2^32/400e6 = 10.737 s，munit 直接相减、不做回绕补偿。证据：FC normal 真实 6.975+7.244+0.041=14.26 s，打印 3.52 s（=14.26−10.74）；FTL cache 真实 ≈43.3 s，打印 0.368 s（=43.3−4×10.74）；Total 真实 ≈98.8 s，打印 12.95 s（=98.8−8×10.74）。**用例内部的 `wr_ms`/`rd_ms`（`platform_now_ms`）不受影响** | **已改（2026-10-31；2026-11-03 DWT 删除后彻底闭合）**：`munit_timing_stop` 只用平台 `time_us`（产品侧为 SysTick 64 位 µs），不再读 DWT CYCCNT，因此 32 位周期回绕问题不存在。早期方案（毫秒判断回绕 + DWT 补亚毫秒）已删除。`ftl_ut_clock_install` 目前无调用者（若将来有测试替换平台时钟，该用例的框架计时会失真） | **上板**：跑含 `format`/`boot_restore` 的长用例，核对 `[ OK ] [ 耗时 ]` 与真实墙钟一致（format ≈ 21 s 不再显示 10.6 s）、`Total wall-clock` 与整轮实测吻合 | 板端 2026-10-25 release 日志 `SPEED ... wr_ms=6975 rd_ms=7244` vs `[ OK ] [ 3.52 s ]`；`tests/framework/munit.c` 计时逻辑 |
| RISK-UT-09 | 已闭合（2026-11-01 板端全量 156/156） | **`media_ledger_total` 漏计「用户区借用日志块」⇒ 守恒判据误报**：账本原五项（free/op/spare/closed/open）不含 `state==LOG` 的用户区块——L2P 日志从 OP 池借出的块既不在池里也不在 SB 里。运行期该类块不计账；重启后写游标不持久化，`pfr_probe_blocks` 判「陈旧 LOG + `l2p_live==0` + 有数据」⇒ 关成单 plane Closed ⇒ 账本 +1，`boot_restore_reconcile` 断言失败（实测 3871/3870、3868/3867 各一次）。**不是介质或记账缺陷**：故障时 free/op/spare 三项前后**逐项相同**，证明没有块丢失或重复登记。 | 测试口径补全：新增 `media_user_log_planes()`，`media_ledger_total()` 增加第六项；诊断打印走 `ftl_ut_trace_detail`（DEBUG 档，由日志等级控制）。 | 真丢块（`state==NONE` 但不在池）仍计不进去 ⇒ 判据未被稀释；后续新增块状态时须同步账本口径 | `tests/unit/test_ftl_media.c` `media_ledger_total` / `media_user_log_planes`；板端 2026-11-01 10:15 镜像全量 156/156 |
| RISK-UT-11 | 已闭合（2026-11-11 板端） | **`block_erase_verify` 判据无区分力（C7/C8）**：原流程只有 erase → 读回全 0xFF，**没有先写 pattern 再擦**；若擦除未真正清掉旧数据（或器件擦除失败仍回成功），用例照样绿。代码注释自认「不证明擦除真的清掉了旧数据」 | 已改为 prog → erase → 读回，打印逐 CW 字节数。板端 rev=33（2026-11-11 18:22:53，59/59）：`block_write_read` `[ OK ] [ 55.19 ms ]` 证明**写入态**同路径读回是 PAA 图案（非 0xFF），`block_erase_verify` `[ OK ] [ 56.96 ms ]` 证明 prog+erase 后整块 0xFF ⇒ 同一读路径对「未擦净」与「已擦净」给出相反结果，判据有区分力 | C7 的硬件故障注入负例不构造：本器件 22h LOCK 在默认配置下空转（`block_lock_ops` 已实测），无可用注入通道；区分力由上述跨用例实测覆盖 | `tests/unit/test_fc_level3.c:360-372` `read_block_verify_erased`；`:1163-1191` `test_block_erase_verify` |
| RISK-UT-15 | 已修复并上板（2026-11-11 19:32:02；pure 59/59 + media 36/36） | **CMT 写回改造后，L2P 纯 RAM 用例直接改 `l2p_cache_line_s` 内部状态，且只回滚 line / `region_dirty_bitmap`，不回滚 `tags[]` / CMT**：① `l2p_write` 在 `cmt_enabled != false` 时把 CLEAN 行的写装进 dirty CMT，不再置 `line->state=DIRTY` ⇒ `test_l2p_resident_states` 的状态断言必然失败；② `test_l2p_miss_unmapped_evict` 的写安装 dirty CMT，清理只 memset line，CMT 条目跨用例残留；③ `test_l2p_wal_gate` 只恢复 line，`tags[0].state/dirty_seq` 留在 DIRTY ⇒ 后续用例看到假脏行。后果：即使字段名机械改完，纯 RAM 套件仍会红/污染后续用例 | 无（测试源待重建） | 重建 L2P 用例时：读状态走 `l2p_line_get`；需要行路径语义时显式 `c->cmt_enabled=false` 并在 teardown 恢复；实验前保存/恢复 `tags[idx]` + `cmt_*` + 两个 dirty bitmap；新增 CMT 命中/合并/驱逐写回用例 | `tests/unit/test_ftl_pure.c:2613-2699` `test_l2p_resident_states`、`:2717-2783` `test_l2p_miss_unmapped_evict`、`:2794-2860` `test_l2p_wal_gate`；`src/ftl/l2p_cache.c:1666-1737` `l2p_write`、`:2316-2370` `l2p_flush_kick`；`include/ftl/l2p_cache.h:306-331` |
| RISK-UT-16 | 已修复并上板（2026-11-11 19:32:02；pure 59/59 + media 36/36） | **两条用例的语义已随生产改变，不能只改字段名**：① `test_cp_init_timer` 断言 `cp_timer_reset` 把 `last_ms` 清 0；现 `cp_timer_reset` 只重装 `cp_state.next_t3_ms = now + CP_TIME_INTERVAL_MS`（`checkpoint.c:1834-1837`）⇒ 改名后仍必红，且设计文档的“清 0”口径也过时；② `test_dir_ckpt_three_step` 使用已删除的 `md_dir_ckpt_prepare/page` + 4 参 `md_dir_ckpt_commit`；现为 `md_dir_ckpt_write_begin/advance/commit(dir, md_dir_ckpt_write_s*)` 的 write-session 形态 | 无 | 按当前 API 重写这两条；CP timer 用伪时钟断言 `next_t3_ms == now + CP_TIME_INTERVAL_MS`；目录检查点按 begin→advance→commit 重写并覆盖 abort/跳过分支 | `tests/unit/test_ftl_pure.c:1004-1034`、`:904-952`；`include/ftl/metadata.h:779-805`、`:1180-1265`；`src/ftl/checkpoint.c:1834-1837` |
| RISK-UT-17 | 已修复并上板（2026-11-11 19:32:02；pure 59/59 + media 36/36） | **`bbt_mark_bad` 的幂等断言写死 plane 0**：用例按 `plane` 参数（0/1）把 Spare 压入对应 plane，但重标后断言 `alc_spare_get_count(a, 0U)==0`；当 `plane=1` 时该断言恒真，无法发现“重标把 plane1 Spare 再弹一次/弹错 plane”的缺陷 | 无（改测试） | 改为 `alc_spare_get_count(a, plane)==0`，并加负例：重复 `bbt_mark_bad` 后 free_count 不再增长 | `tests/unit/test_ftl_pure.c:430-473`（断言在 `:466`） |
| RISK-UT-18 | 已修复并上板（2026-11-11 19:32:02；pure 59/59 + media 36/36） | **证据行打印硬编码值 / 打印的不是判据所用的那次调用**：`test_confirm_is_parked` 的 `[EV]` 行把 `a_parked/b_parked` 写成常量 `0,0`；`test_journal_write_fail_clear` 把第一次 `pending` 写成常量 `1`；`test_gc_select_victim` 在末尾用 fallback 阶段的 `out` 打印 `best_valid_pct/scanned/eligible`，与阶段①②的判据不对应。后果：通过时无法从日志复核判据，失败时日志误导排查 | 无 | 证据行改为读实际变量/在每次判据处就地打印；与断言一一对应 | `tests/unit/test_ftl_pure.c:1910-1915`、`:1098`、`:2311-2312` |
| RISK-UT-19 | 已修复并上板（2026-11-11 19:32:02；pure 59/59 + media 36/36） | **声称的覆盖没有判据**：`wl_trigger_and_victim` 注释称“victim 取擦除次数最高的空闲块”，实际只断言 `wl_select_victim()` 返回 true，未断言 victim 的 EC/plane/valid；`gc_select_victim` 注释称回报 `best_valid_pct/scanned/eligible`，但未断言这三个值。失败场景：victim 选错块/统计回报恒 0 时用例仍绿 | 无 | 补断言：WL victim 的 `erase_count` 为最高、`valid_cw` 达门槛；GC 的 `best_valid_pct/scanned/eligible` 与构造值匹配 | `tests/unit/test_ftl_pure.c:2323-2383`、`:2203-2315` |
| RISK-UT-20 | 已修复并上板（2026-11-11 19:32:02；pure 59/59 + media 36/36） | **WAL 次序的兜底判据只扫 line DIRTY，漏掉 dirty CMT**：`journal_barrier_cp_progress` 的“驻留 dirty 行 `dirty_seq <= persisted`”循环只看 `lines[i].state==DIRTY`；CMT 成为写缓存主单元后，dirty CMT 条目（line 仍 CLEAN）不会被检查。失败场景：CMT 的 dirty_seq 高于 persisted 而 line 判据全绿 ⇒ WAL 次序回归漏检 | 无 | 判据改为同时遍历 dirty CMT（或直接断言 `l2p_flush_owner_idle`/`cmt_dirty==0` 与 `persisted_seq` 的关系）；补一条 CMT 先于 Journal 的负例 | `tests/unit/test_ftl_media.c:466-546`（循环在 `:499-507`）；`src/ftl/l2p_cache.c` `l2p_cmt_merge_region` / `l2p_flush_kick` |
| RISK-UT-21 | 已修复并上板（2026-11-11 20:22:43：media 39/39；pure 60/60 前序镜像） | **板端首轮暴露的三处测试假设 / 驱动缺失**：① `alc_free_remove` 当前语义是「出池即置 `NONE`」（`src/ftl/allocator.c:202-204`），旧用例仍断言 `FREE`（`free_pool_invariants` 红：0==1）；② `md_ring_locate` 的写点在 record 处于块中段时会**保守跳过一槽**，旧用例按 `newest+1` 断言（6==5 红）；③ 显式 CP / Journal 失败等待只泵 `ftl_service()`，没有跑 `timer_scheduler_poll()` ⇒ bg_scheduler 的 credit 耗尽后 CP 触发与 Journal 刷页永不运行（`trim` cp 2>2、`journal_write_fail` fail_seen=false） | 已改：free 池断言改 `NONE`；ring 用例补「中段跳一槽 + 块末跨块」两态；`trim_no_resurrection` / `journal_barrier_cp_progress` / `journal_write_fail_wal_stall` 的 4 个等待循环补 `timer_scheduler_poll()`，与生产主循环一致。**追加**：`ftl_ut_io_wait` / `ftl_ut_flush` / `ftl_ut_trim` 三个通用等待循环也补上（长写序列在 credit 耗尽后停摆，2026-11-11 板端 `cache_seq_cross_block` 写失败） | 改动 allocator 出池语义、`md_ring_locate` 写点规则或后台预算驱动方式时，重跑 pure 59 + media 36 | `tests/unit/test_ftl_pure.c` `test_free_pool_invariants` / `test_ring_locate`；`tests/unit/test_ftl_media.c` 的 trim / journal barrier / journal fail 等待循环；板端 2026-11-11 19:32:02 镜像日志 |
| RISK-UT-22 | 已修复并上板（2026-11-11 20:22:43） | **media 套件的尾窗按编译期 `FTL_LBA_MAX` 计算，运行期 `host_lba_max` 因坏块更小**：`FTL_UT_LBA_BASE = (FTL_LBA_MAX - 8192) & ~7`，而实测 `g_ctrl.host_lba_max=867840`；`host_io_random` 曾写到 `lba=868251`（> host_lba_max）。用例仍过，因为 `ftl_submit_common` 的上界检查用静态 `FTL_LBA_MAX`（`ftl.c:3069`），不是主机可见容量。后果：尾窗用例并不覆盖「主机可见容量的尾部」；若将来 FTL 入口改用运行期 `host_lba_max`，这些用例会直接失败 | 已改为运行期 `ftl_ut_lba_base()`（取 `g_ctrl.host_lba_max`，保留 8192 LBA 尾窗）；需上板重跑纯/媒体套件确认 | 下次改容量/尾窗逻辑时复核；上板判据：随机 LBA ≤ 运行期 `host_lba_max` | `tests/unit/test_ftl_common.h` `FTL_UT_LBA_BASE`；`tests/unit/test_ftl_media.c` `host_io_random`；板端 2026-11-11 19:32:02 日志 `lba=868251` vs `lba_max=867840` |
| RISK-UT-23 | 已修复并上板（2026-11-11 20:22:43） | **早回执用例把内部读路径钉死**：`cache_early_ack_read_visible` 原断言 `ftl_cache_wr_lookup()` 必须命中，但该函数只对 `FTL_CACHE_PAGE_PROGRAMMED` 返回 true；页面可能已 `CONFIRMED` 但仍在等后继 FAILC 结论（`proven==false`），此时读走 L2P 而非覆盖层。板端 `pending>0` 成立、`covered=false` ⇒ 断言过强。产品判据应是「不 flush 即可见新数据」，不是内部走哪条路 | 已删 `covered` 断言，保留 `wr.count>0` 与读回逐字节比对；用例注释说明两条内部路径都必须给出新数据 | 上板重跑 media；若 `pending==0` 再评估早回执时序 | `tests/unit/test_ftl_media.c` `test_cache_early_ack_read_visible`；`src/ftl/ftl.c:1081-1095` `ftl_cache_wr_lookup`、`:1238-1288` `ftl_cache_wr_confirm_step` |
| RISK-UT-24 | 已修复并上板（2026-11-11 20:22:43） | **`gc_progress` 第一次 urgent GC 可能只回收 valid=0 的 padding/空 SB**：写满一个 SB 再 flush/关闭时，cache 序列的 padding END 可能落在新 SB 的 page0，形成一个 `valid_cw=0` 的 closed 条目；`gc_select_victim` 取最低有效率的 victim ⇒ 第一轮 `reclaimed+1, moved=0`（板端实测 `closed=2, victim valid=0`）。原用例只 request 一次 urgent，随后断言 `moved>0` 必然红 | 用例循环内每轮重新 `mig_gc_request_urgent`，直到有有效 victim 被搬移；同时断言 closed 列表里至少存在一个 `valid_cw>0` 的 SB。诊断打 `pick/victim/valid/job` | 上板重跑 media；若连续多轮仍 moved=0，看 `job(aborted/phase)` 与 `gc_select_victim` 的 `best/eligible` | `tests/unit/test_ftl_media.c` `test_gc_progress`；`src/ftl/gc.c` `gc_select_victim`；`src/ftl/migrate.c` `mig_gc_begin` |
| RISK-UT-26 | 已修复并上板（2026-11-11 21:22:39：`test /ftl/` 100/100） | **测试里的临时强制诊断与预期失败打印违反证据分级**：`ftl_ut_trace_io_fail` 用 `ftl_ut_result()`（force）打印预期注入失败，且注释写「默认只在 debug 档可见」与实现不符（H6）；`gc_progress` 的 3 条 `ftl_ut_result` 也是排障期加的，成功路径仍打印。设计文档 §1.5 约定默认档只打用例名，`ftl_ut_result` 只用于 `/speed` 结果。后果：99/99 通过时仍出现大量 `host write FAIL`，掩盖真正的异常输出 | 无（临时诊断） | 把预期注入失败的打印降为 `trace_detail` 或加 expected 标记；`gc_progress` 诊断降为 debug 或仅失败时打印；同步注释 | `tests/unit/ftl_ut_common.c:371-391`；`tests/unit/test_ftl_media.c:650-715`；`docs/design/ftl_ut_design.md` §1.5 |
| RISK-UT-27 | 已修复并上板（2026-11-11 21:22:39：`test /ftl/` 100/100） | **结束不变量不完整**：`ftl_ut_assert_clean` 不检查 `cache_stream.seq`（`wr.count`/`wr_inflight`/`active`/`closing`）与 `cache->stats.cmt_dirty`/region dirty 位图；`media_teardown` 只 quiesce，不调用 `assert_clean`。失败场景：某用例留下 cache 页表/序列或 dirty CMT，teardown 仍绿，下一用例读到旧页/被门控拒绝（本轮 `cache_seq_cross_block` 的 credit/状态问题即这类） | 无 | media teardown 在 quiesce 后调用 `assert_clean`；`assert_clean` 增 cache_stream/CMT 断言（media 允许恢复后 CMT 为空作为前置） | `tests/unit/ftl_ut_common.c:542-611`；`tests/unit/test_ftl_media.c:103-116`；`docs/design/ftl_ut_design.md` §3.4 |
| RISK-UT-28 | 已修复并上板（2026-11-11 21:22:39：`test /ftl/` 100/100） | **测试注释混有变更史/日期**：`test_ftl_media.c:531`（v1.08 上板复现并修复）、`:602`（2026-11-11 板端复现）、`test_ftl_pure.c:2394`（v0.91 修复的形态）、`:2904`（v1.08 已在板上复现并修复）。H37 要求注释只描述当前设计，变更史归 git/changelog | 无 | 改成当前约束/失败后果，例如「区域页先于 Journal 落盘会导致 valid_count 漂移」「100% 有效 SB 在 emergency 下无收益」，删版本/日期 | `tests/unit/test_ftl_media.c:531,602`；`tests/unit/test_ftl_pure.c:2394,2904` |
| RISK-UT-29 | 已修复并上板（2026-11-11 21:22:39：`test /ftl/` 100/100） | **CMT 用例证据行硬编码**：`test_l2p_cmt_write_readback` 的 `[EV]` 打印 `cmt_dirty=1 line_clean=1 entry=0x11111` 为字面量，不读实际变量；若安装/合并逻辑变化，日志仍显示期望值，失去区分力 | 无 | 捕获实际 `cmt_dirty`/tag state/line entry 后打印；与断言一一对应 | `tests/unit/test_ftl_pure.c:3075` |
| RISK-UT-30 | 已修复并上板（2026-11-11 21:22:39：`test /ftl/` 100/100） | **printf/断言里存在系统性冗余强转与格式不匹配**：`(int)BOOL/enum` 配 `%d`、`(unsigned)U 后缀宏` 配 `%u`、`BYTE` 配 `%02X` 再套 `(unsigned)`（应 `%02hhX` 并删 cast）；例：`ftl_ut_common.c:279,385-386,569-580`，`test_ftl_pure.c:263,384,586,834`，`test_ftl_media.c:659,709-715,837`。当前零告警是因为未开 `-Wformat` 严格档/强转掩盖，不是规范正确 | 无 | 按 H44 统一格式修饰符（BYTE `%hhu/%hhx`、WORD `%hu/%hx`、DWORD `%lu/%lx`、bool/enum `%d`），删同型/默认提升可省的 `(int)/(unsigned)/(DWORD)`；逐文件批量替换后重编 | 上述行；H43/H44/H48 |
| RISK-USBC-02 | **已闭合（2026-11-12 用户结案：非代码问题；项目基线 MBR+FAT32 已验证通过）** | **固件 format 后 Windows 侧「无法格式化」**：CLI `format` 后介质是 **partitionless**（raw 读 LBA0 全零），Windows 把无分区表的可移动盘表示成 `offset=0` 的合成分区（超级软盘）。**2026-11-12 Explorer 矩阵：FAT（最小 8K）/ FAT32（4K）/ exFAT（4K）全部在 0% 报「Windows 无法完成格式化」；NTFS（4K）成功**（LBA0 变成 `EB 52 90 "NTFS"` 引导扇区，卷健康可用）。FAT32/exFAT 在标准 MBR 分区上也失败（此前 diskpart 实测 `ERROR_NOT_SUPPORTED` / `ERROR_INSUFFICIENT_BUFFER`）⇒ 不是 partitionless、也不是簇大小，而是本机 **Windows 10 Home China 25H2 build 26200.9974** 的非 NTFS 格式化路径。**时间线**：历史 FAT 格式化 + H2testw 成功在 2026-10-22/25；本机 2026-11-11 安装 **KB5132688 / KB5132061**；2026-11-12 FAT/FAT32/exFAT 全部失败、NTFS 正常 ⇒ 高度指向该累积更新引入的非 NTFS 格式化回归（未回滚验证）。**结论：不是 NAND/FTL/USB**；设备侧 UNIT ATTENTION（0x29、0x3A→0x28）不改变该结果，已回退。⚠ Explorer NTFS 成功得到的是 **partitionless NTFS（超级软盘）**，不是历史 MBR+FAT32 布局。 | **标准布局契约**：`usb_verify.ps1 -Step format`（`diskpart clean` → `create partition primary` → `format fs=NTFS quick`；失败自动 raw 清零前 1MB 重试）⇒ MBR 分区（offset 64KB）。**临时可用**：Explorer 选 NTFS+4096 可在 partitionless 介质上直接格式化。FAT/FAT32/exFAT 在本机不可用（矩阵全失败）；需要 FAT 基线时只能换主机/工具，本机未解决。 | 重访触发：换 Windows 版本 / 需要 FAT 基线 / 需要标准 MBR 布局。上板判据：固件 format → 断电 → Explorer NTFS+4096 能格式化；工具 `-Step format` 能恢复标准 MBR 分区。 | 板端 2026-11-12 串口（13:21:34 镜像）、raw LBA0（全零 → NTFS 引导扇区）、`Get-Volume G: NTFS/4096`、`Get-Partition offset=0`；`Get-ComputerInfo`（25H2/26200.9974）与 `Get-HotFix`（KB5132688/KB5132061 2026-11-11）；`tools/usb_verify/usb_verify.ps1`；同表 RISK-USBC-01；2026-11-12 20:11 `DISM /Remove-Package Package_for_RollupFix~...26100.9974.1.23` 返回 **3010（成功，需重启 PC）**。**2026-11-12 21:06 重启后核实：回滚已生效且未被覆盖**——UBR **26200.9711**（原 26200.9974）；CBS 包 `Package_for_RollupFix~31bf3856ad364e35~amd64~~26100.9974.1.23` 已消失，现存 `26100.9711.1.30` CurrentState=0x70（Installed）；WU/CBS 均无 RebootPending。当日 Windows Update 只装了 Store 应用（BingSearch 成功 / OpenAI.Codex 0x80073D02）与 Defender 定义 KB2267602，**无累积更新重装**（`Get-HotFix` 仍列 KB5132688 属历史记录残留，与 UBR 不符）。**2026-11-12 21:23 FAT32 复测通过**：`diskpart` `clean → create partition primary → format fs=fat32 quick` 成功，卷 `F: SOFTSSD / FAT32 / 435,879,936 B / Healthy`（分区 offset=65,536 size=440,074,240，恰好铺满盘尾）；64 KiB 写→读→逐字节比对一致。⇒ 回滚**修复了 diskpart 路径**。⚠ 首次 `clean` 仍报「设备中没有介质」，清零前 1 MB 后成功 —— 间歇无介质未消失，见 RISK-USBC-03。**但 2026-11-12 21:5x 用户手动复测：固件 `format` 后介质为 raw（无分区表），资源管理器 FAT32 格式化**仍报「Windows 无法完成格式化」**，串口 SCSI trace 与回滚前一致（INQUIRY → READ FORMAT CAPACITIES → READ CAPACITY → MODE SENSE 0x1C/0x08 → READ CAPACITY → TUR×2 → PREVENT ALLOW lock→unlock，**无任何 WRITE/FORMAT UNIT**）；同一时刻 raw 读写 512B 均成功 ⇒ 不是设备未就绪。待判定： Explorer 路径是否只在 raw 介质上失败（下一步：先建 MBR 分区再让 Explorer 格式化），还是 Explorer 的 FAT 路径本身未被回滚修复。**补充（同日）：磁盘管理在 partitionless（`Get-Partition` offset=0 超级软盘）上格式化 FAT16 **成功**（卷 FAT/439,902,208 B/Healthy），随后在同一介质上选 FAT32 **失败**、卷退化为 `Size=0 / 无文件系统` ⇒ `FAT32` 与 `partitionless` 两个变量尚未分离：已知「MBR 分区 + diskpart FAT32」成功、「partitionless + FAT32」失败。判定实验改为：`diskpart clean → create partition primary`（真实 MBR，offset=64KiB）后，用 Explorer/磁盘管理格式化 FAT32。**2026-11-12 21:38 判定完成**：真实 MBR 分区（offset=65,536 / size=440,074,240）上 `Format-Volume -FileSystem FAT32`（FormatEx 路径，与资源管理器同一引擎）**成功** —— `F: SOFTSSD / FAT32 / 435,879,936 B / Healthy / OK`，128 KiB 写→读逐字节一致；盘符已设回 F:。⇒ 回滚确实修复了**项目基线路径（MBR + FAT32）**。唯一遗留：`partitionless`（`Get-Partition` offset=0 超级软盘）+ FAT32 仍失败（Explorer 与磁盘管理均失败，同介质 FAT16 成功）；该组合是否与同一回归相关未判定，但历史基线（2026-10-22/25）是 MBR+FAT32，不影响项目路径。**结案判定（2026-11-12，用户确认）**：设备与固件无问题；用户侧失败的根因是**对 partitionless 介质直接格式化**（Windows 对可移动盘把无分区表合成为 `offset=0` 的 RAW「主分区」，磁盘管理因此既不给「新建简单卷」、又会直接格式化这个合成对象）。**U 盘常规使用 FAT32 没有问题**；正规流程 = 固件 `format` → `diskpart clean/create partition primary`（真实 MBR，offset=64KiB）→ 在分区上格式化 FAT32。**纯 GUI 路径不可用（2026-11-12）**：磁盘管理对这块盘的合成 RAW 卷执行「删除卷」返回「**不支持这个请求**」⇒ 无法通过 GUI 把介质变回「未分配」再「新建简单卷」；与既有记录「异常 MBR 下 `Clear-Disk`/`New-Partition` 报 `Not enough available capacity`」同类。普通用户可用路径只剩：① 一键脚本（包装 `diskpart`）；② 第三方 GUI 工具；③ 产品侧让固件 `format` 后写一个默认 MBR 分区表（设计决定，未实施）。**端到端验收通过（2026-11-12，用户上板）**：FAT（FAT16）卷上 H2testw v1.4 —— **371/419 MB tested、0 errors、写 925 KB/s、读 886 KB/s**（约 48 MB 未覆盖 = H2testw 写满即停，卷内已无更多可用空间；非错误）。设备侧结论完整 |
| RISK-POOL-10 | **已关闭（2026-11-07）** | `fc_command_to_id` 的池上界取 `sizeof(指针字段)`，slot > 0 的命令会被误判为池外；该指针→ID 接口及其辅助函数已随 `cmd_id` 字段化整体删除，风险不再存在 | 命令 ID 由 `fc_cmd_s.cmd_id` 携带；不再有运行时指针区间反查 | 无；`grep fc_command_to_id` 应只剩历史文档 | 已删除接口；历史记录保留在 `docs/archive/` |
| RISK-DP-30 | **已修复、已上板验证（2026-11-11：`block_multi_plane` 1/1 + 两个 cache 用例 2/2 全绿）** | **板端 `/fc-nand/lifecycle/block_cache_multi_plane` 的 START_SEQ_MP 首个 pair 返回 16 = `FC_CMD_STATUS_META_ERROR`**（`tests/unit/fc_test_common.c:787`，`test_cache_start_seq(page 0)`）。本用例配置 `L3_CONF` = RAW ECC + CRC off + 加扰 off ⇒ 读路径**唯一能产生**的错误位就是 META；且 RAW 分支不跑 `fc_cw_is_blank`（`fc_data_path_process_read` 的 `ldpc != RAW` 才判空页）⇒ 未编程页读回 0xFF（低 3 位 = 7）同样报 META。**症状不区分**「读错页 / 读串 plane」「页根本没写进去」「写进去但 META 错」。另：`test_prog_block_cache_multi_plane` 只断言命令状态、从不读回数据，编程内容在本 UT 里从未被验证。**2026-11-11 三轮（`block_multi_plane` 单跑 debug）**：64 条 plain MP program 全部 `status=0xE0`、PBA 逐页 +8（块 663 两 plane）⇒ 写命令自报成功；随后**没有任何一条 `read pXX` 行**（`read_page_verify` 只在通过后打印）⇒ 页 0 首个 desc（plane0 cw0）即 META，不是"写到一半漂"。结构事实：`a166f38` 的 `fc_nand_dispatch_rewrite_operation` 对 MP 读**恒改写**为 WITHOUT_DATA_OUT ⇒ `g_nand_multi_plane_read_with_data_out_uops` 在本轮之前**从未执行过**；同块 cache 用例在板上是绿的 ⇒「06h-E0h + 双 2112B DATA_OUT」形状硬件可行，两表差异只有 cache 表在 30h 与 06h 之间多 `31h + TWB + WAIT_READY`。**2026-11-11 四轮（读路径排除）**：TEMP-BISECT 强制 MP 读走加载型（= `a166f38` 行为）后重跑 `block_multi_plane` 仍红，失败点同为 `chain[0] plane=0 cw=0`（横幅 12:07:51 证新镜像）⇒ **整页 WITH_DATA_OUT 读表不是根因**，逐 CW 06h 取数路径与基线一致且可用 ⇒ 剩余变量在写侧。**2026-11-11 五轮（写侧二分，横幅 12:17:39）**：TEMP-BISECT 暂删 plane 1 分支（只编 plane 0）后重跑 `block_multi_plane`，失败点移到 `chain[4] plane=1 cw=0` ⇒ plane 0 的 desc / 页缓冲组装 / META 编码 / PBA→PAA / 行地址 / `80h→TADL→DATA_IN→10h` **全部正确**；根因就在 `11h` 切换 + 第二组 `80h`/地址 + 合体 `10h` 这段。**结构事实**：`RISK-NANDP-16`（2026-11-07）按 Table 29 Note 2 把 `TADL` 从 `11h` 之后移到每组地址之后，并**删掉 `11h` 后的 1µs 延时**（涉及 6 张 UOP 表，全部未上板）；`a166f38`（`/fc-nand/` 56/56 通过）的对应表相反——**地址后无延时、`11h` 后有 `{DELAY_US, 1U}`**。五轮同时证明「地址后 TADL」本身无害（plane 0-only 序列带着它照样写对）。**2026-11-11 六轮（修复验证）**：补回 1µs 后 `block_multi_plane` **1/1 通过**（2.84 s）⇒ 根因确认 = 删除 `11h` 后延时；区分力成立（同一表删延时必红、加回必绿，负例见五轮）。**七轮（cache 回归，横幅 12:33:39）**：`block_cache_multi_plane` 与 `block_cache_multi_plane_cross_block` 全绿（64 pair = 63×`15h`+1×`10h`，两 plane PAA 校验；cross-block 为 plane0 blk 725 / plane1 blk 1333）⇒ 写侧修复对 cache 双 plane 同样有效，**本风险关闭**。剩余未回归项：整页 `WITH_DATA_OUT` 读表（见下一步）与 H2testw 写校验 | **2026-11-11 二轮（单跑证据）**：① 已答——单跑仍红 ⇒ 非前序用例残留，确定性复现。64 条 cache program 全部 `status=0xE0`（bit0..4 全 0；0xE0 = AREADY\|RDY\|WP，即 NAND 自报编程成功），页 0 读回 `status=0xF0` = 0xE0\|META_ERROR ⇒ 只有 META 比对失败，无 NAND_FAIL/FAIL_CACHE/TIMEOUT/DATA_ERROR；页 1..62 用 op 44（`failc_valid` 覆盖）⇒ 若页 0 编程失败会在下一 pair 报 FAIL_CACHE，实际没有。本轮新增静态排除：`fc_nand_dispatch_select_read_op` 对 MP cache 读 op 原样返回（不改写）；MP 读写 op 的 `.flags` 正确置 PAGE_READ/PAGE_WRITE ⇒ `need_cw_base` 成立（`cw_buffers` 是 CW 基址，非 USER 区）；`.pair_op = 0` ⇒ 无 ISSUE/WAIT 拆分；`fc_nand_dispatch_task_cw_chain` 对这两类 op 立即 break（无逐 CW 重读）。余下最可能：写进去的 META 区内容不对（页 0 实际未编程 = 0xFF 低 3 位 7；或写错页/错 plane；或页缓冲组装错），以及两个盲点：`test_prog_block_cache_multi_plane` 从不读回数据、两个 cache 用例都不断言 erase 返回值。唯一已做的是静态比对：MP 读 / 写 / 擦三条 UOP 序列与 `a166f38`（`/fc-nand/` 56/56 通过基线）逐条一致（差异只有新增 `DELAY_TIMING TWB` 与 `TWH→TWHR` 改名）；desc→request 落位（`fc_build_request_data_planes`）、`plane_paa[]`、`paa_to_nand_row`、06h P0/P1 行地址、`DATA_OUT 0/PAGE_SIZE`、scatter 的 `plane*PAGE_SIZE + cw*CW_SIZE`、`encode`/`meta_check` 公式均已比对一致 ⇒ **静态无法定位**。**第二个 TEMP-BISECT（已上板，横幅 12:17:39）**：`g_nand_multi_plane_program_uops` 暂删 plane 1 分支，只编 plane 0——判据命中：失败点移到 `chain[4] plane=1`（结论见第 2 列五轮）⇒ desc / 页缓冲 / meta / 寻址全部正确，根因是双 plane 写序列。第 2 步已落 TEMP-BISECT（`fc_nand_dispatch_select_read_op`：MP `WITH_DATA_OUT` 完整页也恒走加载型 = 基线行为），debug 已编译（零新告警），**未提交** | **2026-11-11 收尾（debug_ut 横幅 12:56:46，R2 三关零告警，未提交）**：写侧修复保留并改为 `11h` 后 `TWB` 延时 + `WAIT_READY`（R/B# 引脚轮询）；读侧 TEMP-BISECT 已回退（`fc_nand_dispatch_select_read_op` 恢复 `return operation;`）。**上板 `/fc-nand/` 59/59 全绿（3 skip）** ⇒ 整页 `g_nand_multi_plane_read_with_data_out_uops` 首次上板执行通过，本风险关闭。参数建模：`11h` 后的等待已用 `TWB` + `WAIT_READY` 表达，不再需要独立 timing 字段；RISK-NANDP-08 仅余 `tADL` 定点归位。以下为原计划：上板隔离（①已答：单跑仍红 ⇒ 从 ② 起；每次前先 POR，`.noinit` 故障记录只有断电才清）：① 单独跑 `test --log-visible debug /fc-nand/lifecycle/block_cache_multi_plane`——套件内红而单跑绿 ⇒ 前序用例残留（block 复用 / page buffer / FAILC）；② `/fc-nand/lifecycle/block_multi_plane`（普通 MP 擦/编/读）——同红 ⇒ 与 cache 序列无关，是 MP 平面寻址；绿 ⇒ 定位到 cache 序列；③ `block_cache_multi_plane_cross_block`（plane0/plane1 不同块，专测 06h plane 选择）；④ 用 `--log-visible debug` 取 `cache start p00 ... status=0x%hX` 与逐页程序状态。**改判据前先能构造出必然失败的负例（C7）** | `tests/unit/fc_test_common.c:787`；`src/fc/fc_data_path.c:625`（`fc_data_path_meta_check(buffer, cmd->meta, desc->paa)`）、`:218-250`（低 3 位自洽 + 全量 LBA 比对）、RAW 分支跳过判空页；`src/nand/onfi_1_0.c` `g_nand_cache_start_seq_mp_uops` / `g_nand_two_plane_program_cache_uops` / `g_nand_multi_plane_erase_uops`；`src/fc/fc_nand_dispatch.c` `fc_nand_dispatch_scatter_page_buffer` / `req_multi_plane_read` / `req_multi_plane_program`；`src/fc/fc_nand_dispatch.c:90-120`（`fc_nand_dispatch_select_read_op`，读 TEMP-BISECT 已回退）；`src/nand/onfi_1_0.c:773-822`（`g_nand_multi_plane_read_with_data_out_uops`）；`src/nand/onfi_1_0.c` `g_nand_multi_plane_program_uops`（1µs 在 `11h` 后，注释说明其非 tADL 语义）、`g_nand_two_plane_program_cache_uops` / `g_nand_two_plane_program_cache_end_uops`（1µs 已补，`src/nand/onfi_1_0.c:537-539` / `:576-578`）；`docs/design/known_risks.md` RISK-NANDP-16 |
| RISK-BUILD-03 | **已闭合（2026-11-11 注释已修）** | **`makefile` 顶部把 `UT=FTL` 写成「仅 FTL L0（纯 RAM、不碰 NAND）」，但实际编入 `test_ftl_media.c`（格式化 + 真 NAND）和 `test_speed.c` 的 `/speed/ftl_rw`（先 format 再覆写 LBA 0..51199）**：操作者按注释以为 `UT=FTL` 不碰介质，可能直接跑 speed/ftl_rw 造成数据丢失；`UT=FC` 也包含破坏性的 `/speed/fc_nand_rw`。注释与构建条件不一致（H6） | 无（仅文档误导；实际行为由 `src/ftl/makefile.mk` / `src/fc/makefile.mk` 决定） | 修正 `makefile` §1.5 注释：`UT=FTL` = FTL 纯 RAM + media + `/speed/ftl_rw`（破坏性）；`UT=FC` = FC 三级 + `/speed/fc_nand_rw`（破坏性）；`UT=HIL` 不含 speed；若要只跑 speed，再评估 `UT=SPEED` | `makefile` §1.5；`src/ftl/makefile.mk:11-13`；`src/fc/makefile.mk:19-30`；`tests/unit/test_speed.c:1-30` |
| RISK-UT-31 | **已闭合 / 上板 15/15（2026-11-11 22:32:57）** | **用例注释缺失且头文件口径矛盾**：12 个用例中 10 个没有用例级注释（`init_ready` / `init_missing_slot` / `init_topo_mismatch` / `host_queue` / `lba_range_ok` / `submit_not_ready` / `submit_edges` / `flush_job` / `flush_busy` / `trim_job`）；`state_machine` 与 `write_read_roundtrip` 前只有分节标题，不含 `覆盖`/`判据`。同时 `test_hil.h:9` 写「破坏性用例会 format + 写 NAND」，`test_hil.c:8` 写「本套件不 format」——H6 矛盾，操作者会误判是否需要预留格式化/数据区。后果：维护者无法从用例处确认覆盖与失败判据，头文件误导操作 | 已改：15 个用例均有紧邻的 `覆盖`/`判据` 注释；`.h` 改为「不 format，固定用容量尾部 8-LBA 窗口」 | 上板跑 `test /hil/` 复核；后续新增用例必须带用例级注释 | `tests/unit/test_hil.c` 各 `test_hil_*` 上方；`tests/unit/test_hil.h:8-10` |
| RISK-UT-32 | **已闭合 / 上板 15/15（2026-11-11 22:32:57）** | **HIL 等待循环没有 `timer_scheduler_poll()`**：`hil_wait` 与 `hil_quiesce` 只泵 `ftl_service_pump` / `ftl_service` / `ssd_controller_service`；生产主循环每轮还调 `timer_scheduler_poll()`，bg_scheduler 的 credit、CP/Journal 触发、WL 评估都靠它。FTL UT 已因同类问题在 `ftl_ut_io_wait` / `ftl_ut_flush` / `ftl_ut_trim` 补过（RISK-UT-21）。后果：长写/flush/后台 CP 等不到 credit，HIL 请求 5 s 超时，或 quiesce 永远等不到池归零；短 4-LBA 用例可能暂时不触发 | 已改：`hil_wait` 与 `hil_quiesce` 每轮在 `ssd_controller_service` 后调用 `timer_scheduler_poll()`，与 `src/main.c` 主循环一致 | 上板跑长写（≥64 LBA）+ FLUSH/TRIM 观察是否超时；若仍超时，查 bg_scheduler credit 与 CP 触发日志 | `tests/unit/test_hil.c` `hil_wait` / `hil_quiesce`；`src/main.c:845-885`；`tests/unit/ftl_ut_common.c:369-388`；`make tests UT=HIL` 0 error / 0 warning；板端 2026-11-11 22:14:53 `test /hil/` 12/12 |
| RISK-UT-33 | **已闭合 / 上板 15/15（2026-11-11 22:32:57）** | **teardown 超时会留下 `bg_suspend=1` 和 FTL 后台在飞**：`hil_quiesce` 在循环内直接 `munit_assert_uint32(..., <, HIL_IO_TIMEOUT_MS)`；断言触发 longjmp 后跳过 `ftl->flags.bg_suspend = saved`。同时它只等 FC 池/`ftl_op_req_count` 归零，不等 CP/MIG/Journal/wbuf 空闲，也不做 `ftl_ut_assert_clean` 一类终态断言；失败的用例若留下在飞 `flush_req`/`trim_req`，后续用例会看到 BUSY 或假空闲。后果：首因被后续级联失败掩盖。**22:25 首轮 15 例**：`write_flush_readback` 用例体通过，teardown 在 `timed_out` 断言超时——`bg_suspend=1` 时只泵 `ftl_service` 不会主动推进 CP/Journal，`hil_ftl_idle` 永远不为真 | 已改：超时只置 `timed_out` 并退出循环，先恢复 `bg_suspend` 再 `munit_assert_false`；`hil_ftl_idle` 把 CP/MIG/metadata/Journal/wbuf/cache 序列/cache inflight 一起纳入排空判据；按 `ftl_ut_quiesce` 补 `cp_handle` / `mig_handle` / `md_handle` / `jrn_handle` 主动驱动；超时打印各分量 | 用 22:32 镜像重跑 `test /hil/`；若仍超时看 `quiesce timeout:` 行哪一项非零 | `tests/unit/test_hil.c` `hil_ftl_idle` / `hil_quiesce` / `hil_teardown`；`tests/unit/ftl_ut_common.c:542-633`；板端 2026-11-11 22:25:23 日志 `test_hil.c:182 timed_out` |
| RISK-UT-35 | **已闭合 / 上板 15/15（2026-11-11 22:32:57）** | **HIL 用例只有 munit `[ OK ]`，没有 `/ftl/`、`/fc-nand/` 那样的 `[EV]` 证据行**：`--log-visible debug` 只影响项目日志与 munit 结果，HIL 用例体内没有任何摘要/明细输出，通过时无法复核 LBA、状态、队列计数、flush/trim 实际结果。失败时只能靠断言值，排障信息不足 | 已改：`test_hil.c` 增加本地 `hil_trace_begin/summary/detail`，复用 `munit_log_visible` 与 `[EV]` 前缀；12 例均补 summary（info）+ 关键明细（debug），默认 `error` 档零输出、报告格式不变 | 上板跑 `test /hil/ --log-visible debug` 核对每例都有证据行；后续若抽出公共 `test_trace.c`，把 FC/FTL/HIL 三份实现收敛 | `tests/unit/test_hil.c` `hil_trace_*` 与各用例尾部；`make tests UT=HIL` 0 error / 0 warning；板端 2026-11-11 22:14:53 `test /hil/ --log-visible debug` 12/12 且每例均有 `[EV]` |
| RISK-UT-36 | **已闭合 / 上板 3/3 err=0（2026-11-12 00:40）** | **`/speed/ftl_rw` 长写在部分 LBA 上写失败，chunk=32 单组 32 LBA 不完成**：23:08 镜像 FTL 先跑、已加 `timer_scheduler_poll()`，`chunk=8` 仍有 7 次写 `STATUS=3`、L2P INVALID；`chunk=16` 1 次；`chunk=32` 单组写 30 s/180 s 不完成。23:18 已把 phase 超时从 180 s 降到 30 s。失败 LBA 每次不同（22:37 为 2576/34840/40992/43560，23:08 为 2576/13848/21536/29224/36912/37944/44608），更像 NAND 介质/写路径失败，而不是固定坏块或顺序污染。`fill_delta` 有值、`fill_pops=0` 说明 FC/NAND 命令未完成。 | 已改：① `/speed/` 顺序 FTL→FC；② speed 等待循环补 `timer_scheduler_poll()`；③ phase 超时 30 s，并在超时时打印 free/bad/closed/wb/seq/cache/pending/cp/mig；④ idle 超时打印同口径状态；⑤ FTL speed 的固定 LBA 0..51199 改为 munit seed 选取的 8 对齐随机起点并打印 base（23:22 实测错误随 base 转移，排除“固定逻辑地址”）；⑥ phase/wait_idle 在 CP/MIG active 时显式推进 `cp_handle` / `mig_handle`（23:22 诊断 `cp=1/0`、pending group=1、seq=16、fill_pops=0）。**根因未闭合**。23:28 随机 base=329520 仍复现：chunk=8 7 次写 STATUS=3；chunk=32 **首条 wr32 本身即 STATUS=3**，随后 `pend(g)=1 seq=16/0 cp=0/0 mig=0/0 fill_pops=0`。说明不是固定 LBA、不是 format 脏、也不是 CP 占用；问题在 FTL 写组/写缓存完成路径。**23:28 同一镜像 `/ftl/media/` 30/30 全过**（含 count=64 的 host_io_matrix、cache_seq_cross_block），说明普通 8/64-LBA 顺序写与 media 夹具正常，失败只在 speed 的持续/多组写压力下出现 | 23:44 构建确认 `jrn=107113/93/0/1`：journal 未满、未 emergency、idle；CP/MIG 均 0 ⇒ 排除 journal/CP。23:52 构建把 chunk 顺序改为 **32→16→8**，32 先跑（depth=1、单组顺序写、干净 format）仍复现：无 STATUS=3，约 9568 LBA 后 `next=9568 inflight=1`、`pend group=1`、`seq=16/0`、`cp=0 mig=0`、journal 低 `9425/14/0/1`、`fill_pops=0`。⇒ 不是固定 LBA、不是 journal/CP、也不是 8/16 多组并发；是**跑过数百组后某一组的完成/序列收尾死锁**。23:58 已在超时路径加 `spd_ftl_dump`，一次打印 pending 组 `op/lba/next/total/outstanding/stalls/agg`、cache 序列 `kind/active/close/next_lba/wr`、wbuf `open/base/occupied/owners`。**根因已定位**：`FTL_CACHE_SEQ_RING=16`，`seq.wr` 达到 16/16 时 `ftl_cache_write_try` 把 `n_pages` clamp 到 0 后只返回 `FTL_CACHE_WRITE_AGAIN`；组 `next<total`、`outstanding=0`，`bg_progress` 的 `wr_ring_pending` 又不计 stall，idle guard 因组仍 issuing 不请求 close ⇒ **ring 等新命令证明旧页、组等 ring 空**的环形等待。修复：`starting==false && n_pages==0` 时置 `seq->close_requested=true`，由 padding END 证明旧页后释放 ring；`ftl_cache_write_try` 再续。`make tests UT=` 0 error / 0 warning。**上板结果**：00:06 镜像 `test /speed/ftl_rw` 3/3 完成、无 phase timeout（ring 死锁已修复），但仍有 `err(wr)=7/4/3`：chunk=8 有 7 次写 `STATUS=3` 且对应读 L2P INVALID；chunk=16/32 也有写错误但读未报错。**测试仍打印 `[OK]` 是因为判据只判「phase 是否排空」，写 IO_ERROR 只计数不判失败（测试口径弱点）**；这不是正常结果。 **根因已定位（00:34）**：`--log-project warning` 现场为 `drain stalled gi=0 next=0/8`，`stall: ... wb_busy=1 ... fc cmd=2/64 desc=8/256 buf=8/128`；`wb_busy=1` 表示 wbuf 页正在编程，但 `ftl_drain_group` 的 `bg_progress` 没有把它算作确定性进展。speed 的 drain 轮次远快于 tPROG=600 µs，8 轮内把“wbuf 正在编程”误判为 IO_ERROR。修复：`bg_progress` 增加 `wb_busy = (wb_is_idle(ftl->wbuf) == false)`。00:34 上板：chunk=8/32 的写错误消失（err=0），但 chunk=16 暴露 **读序列** 问题：`drain stalled gi=0 next=8/16`、`seq kind=1 act=0 drain=1 brk=1`、`wb_busy=0`，`err(wr=0 rd=91)`；旧测试只判 `err_wr` 所以仍显示 OK。已再修：`bg_progress` 把 `seq.draining/break_requested` 也算确定性进展；speed 判据改为 `err_wr || err_rd != 0 ⇒ FAIL`。00:40 上板 `test /speed/ftl_rw --log-visible info --log-project warning`：**3/3 通过，err(wr/rd)=0**；`drain stalled` 消失。仍有 WRN/ERR 级压力日志：一次 `[JRN] erase kick failed blk=2`（下轮重试成功）、`[L2P] load start failed cause=submit_backpressure` 两次、`flush took 111ms`；均未造成数据错误，归属现有 cache 写压力/背压场景（RISK-HOST-05/14），不是本项未闭合。 | `tests/unit/test_speed.c` 23:28 诊断；板端 23:28 日志 `SPEED err#1 wr32 lba=329520 STATUS=3`、`SPEED phase timeout: ... pend(s/g)=0/1 seq=16/0 cp=0/0`；`ftl_ut_common.c:573` | `tests/unit/test_speed.c` `spd_ftl_phase` / `spd_ftl_wait_idle`；板端 22:37/22:59/23:08 日志；`make tests UT=` 0 error / 0 warning（2026-11-12 00:06） |

---
## 2026-11-12 证据复核闭合

| ID | 状态 | 详情 |
|---|---|---|
| RISK-GC-07 | 已闭合（2026-11-12 上板验证） | #### RISK-GC-07 2/3 容量触发 GC/CP 活锁  - **后果**：H2testw 写到约 2/3 后 `free=0`、`op=166/167`；GC 选中 victim 305，page1 读失败 → abort；随后 CP stalled 与 GC 重选反复刷屏、`reclaimed=0`。根因：`mig_read_complete` 在任一页读成功时清零同一 gblock 的 `fail_count`，坏页在第 1 页 ⇒ 计数永远到不了 3 ⇒ 坏块永不隔离、同一 victim 永久重选。 - **缓解**：失败计数按 plane 源块分开、单页读成功不再清零；满盘/近满盘 GC 不再永久重选同一 victim。 - **下一步**：format 后复跑 H2testw，确认 2/3 容量不再活锁、`mark BAD` 速率正常。 - **证据**：`src/ |
| RISK-BBT-11 | 已闭合（2026-11-01 上板验证） | #### RISK-BBT-11 二手片出厂坏块标记丢失  - **后果**：二手片出厂坏块标记已丢失，没有可信 BBT：现网只有运行时退役（擦/写/读失败标坏），坏块会在首次写到时才发现，数据可能已丢。 - **缓解**：新增 `format --verify`：`bbt_format_erase` 之后清空非 BBT 池的旧 BAD 集，对所有非 BBT 池物理块做「写图样 → 读回比对 → 再擦」，失败块标坏并 save；打印 `old_bad/recovered/new_bad`。耗时数分钟，仅显式命令启用。 - **下一步**：上板跑 `format --verify`，确认 `old_bad/recovered/new_bad` 合理、重启后 BBT 位一致。 - **证据**：`src/ftl/power_failure_recovery.c` `format --verif |
| RISK-CP-07 | 已闭合（2026-10-31 上板验证） | #### RISK-CP-07 SYNCHRONIZE CACHE 不产生 Checkpoint  - **后果**：`ftl_flush` 只刷 L2P/journal，CP 仍靠 10s/水位；host 写完即断电时可能只有初始 CP。 - **缓解**：`ftl_flush` 距上次 CP ≥5s 时 force 一次异步 CP；CP start 不逐次打、done 仍 DEBUG；稳定性看 `info cp=` / boot `[CP] find latest` / `[BLK] restore`。 - **下一步**：H2testw/SYNCHRONIZE CACHE 后拔电，重启应 `find latest` + restore，非 full rebuild。 - **证据**：`src/ftl/ftl.c:ftl_flush`；`src/ftl/checkpoint.c:cp |
| RISK-WL-07 | 已闭合（2026-11-12 上板验证） | #### RISK-WL-07 host 写时 WL 占住 migrate 引擎  - **后果**：USB BOT 串行 ⇒ 两条命令之间 `pending` 掉到 0，`bg_scheduler_level_select` 判成 IDLE/NORMAL，`mig_handle` 只在 BUSY 让路 ⇒ WL 照常启动。实测单个 WL job age 4~7s（3 pages/s），期间 GC 起不来、命令槽被分走，CP 持续 `submit stalled phase=1`，H2testw 写速降到 78 KB/s。 - **缓解**：WL 启动三重门：① 两次 5s 求值之间 `*total_writes` 未增长；② `load_level == BG_LEVEL_IDLE`；③ `free_count > FREE_POOL_TARGET`。已启动的 WL job 不强行 ab |
| RISK-CR-02 | 已闭合（2026-11-12 上板验证） | #### RISK-CR-02 die→(ch,ce,lun) 分解未板验  - **后果**：`ftl_op_route_get_region` 的 die→(ch,ce,lun) 分解改走 `die_2_ch/ce/lun`，与 `lba_2_*` 收敛为同一套公式；坐标属「编译期无声的三类量」。 - **缓解**：分解式与原手写式逐字同值；新增三个 `static inline` 纯函数（H42）。 - **下一步**：**必须上板**：跑一遍读写 + `map <lba>`。 - **证据**：`include/ftl/l2p_cache.h` / `src/ftl/ftl_op.c`。 - **历史/验证**：   - （日期未记录）坐标公式统一 → 未上板 |
| RISK-NANDP-09 | 已闭合（2026-11-11 上板验证） | #### RISK-NANDP-09 FMC 命令/地址窗口写缺少 __DMB  - **后果**：连续 5 个地址周期是对同一 `address` 窗口的连续 store；STM32H743 AXI write buffer 可能合并连续 STRB ⇒ 命令或地址字节被合并/重排 ⇒ NAND 收到错误地址或缺周期，可能读写错页/块（静默介质破坏）。`DATA_IN` 已每字节加 `__DMB`。 - **缓解**：唯一入口 `nand_fmc_write_window()`（写后 `__DMB()`）；`COMMAND`、`COLUMN_0/1`、`ROW_0/1/2` 全部走它；`DATA_OUT` 读方向不加。 - **下一步**：上板用非零 block/page、plane1、multi-plane/cache 全量写读回归，确认地址/命令序列。 - **证据**：`src/na |

---
## 2026-11-13 闭合：cache 写序列结论丢失（RISK-CACHE-33 / 34 / 35 / 37）

#### RISK-CACHE-33 padding END 早于数据成员排空

- **后果**：写序列收尾时 `ftl_cache_seq_close_step` 未等 `wr_inflight` 归零就提交 padding END：pad 先完成并把序列置为非活动，随后迟到的数据成员完成把页推进页表——该页没有后继 `FAIL_CACHE` 可证明，永久卡住队头 ⇒ flush 1 s 超时、teardown 断言。触发条件：`/speed/ftl_rw` chunk=16（depth=2）在数据页仍编程时进入 flush。
- **缓解**：① `ftl_cache_seq_close_step` 在 `seq->wr_inflight != 0` 时直接返回，等所有数据成员完成并进页表后再提交 pad；② flush 超时改为**按进展续期**（wbuf/L2P/journal/确认任一推进就刷新 deadline，连续 1 s 无进展才算 stalled）；③ 真超时且仍有未确认写页时置 `deferred_error`（host 下一条命令如实报 IO_ERROR），不静默成功；日志限流，一行。
- **下一步**：上板跑 `test /speed/ftl_rw --log-project warning`，确认无 `cache-wr push into inactive seq` / `flush timeout`、`wr.count` 归零；再跑全量 `test` 回归。
- **证据**：`src/ftl/ftl.c:ftl_cache_seq_close_step` / `ftl_cache_wr_ring_push` / `ftl_flush_step` 超时日志。
- **历史/验证**：
  - 2026-11-13 板端定位：16 条 `cache-wr push into inactive seq` + `flush timeout 1000ms (wb=1 l2p=1 jrn=1) seq(act=0 wr=1/0)` → 修复 → 编译通过，未上板
  - 2026-11-13 二修：flush 改按进展续期 + 超时置 deferred_error；删除误报的 push 日志 → 编译通过，未上板
  - 2026-11-13 上板闭合：`test` 全量 **98/98**、`/speed/ftl_rw` 3/3（`err(wr=0 rd=0)`，chunk=32 `WR=3130 / RD=3195 KB/s`）；同批确认根因为 RISK-CACHE-37（padding END 在飞时续写窗口）

#### RISK-CACHE-34 pad 收尾漏掉未证明的 head

- **后果**：`ftl_cache_pad_callback` 的 stray 循环从 `k=1` 开始，默认 head 已被 pad 的 FAIL_CACHE 证明；但 `take_verdict()` 证明的是**结论游标页**（最近入表页），不一定是 head。若 head 因完成/入表顺序错位没拿到自己的 FAILC，它既不被证明也不被判失败 ⇒ 永久卡在队头（`st=PROGRAMMED proven=0`）⇒ confirm_step 不前进 ⇒ flush 连续 1 s 零进展后超时，teardown 断言。
- **缓解**：① stray 循环覆盖 `k=0..count-1`，只跳过本次被 pad 证明的 `verdict_page`；② `ftl_cache_wr_confirm_step` 增加安全网：写序列已关闭、无在飞、无收尾请求时，`proven==false` 的页**不可能再等到 FAILC**，直接 `mark_failed` 走撤销并弹出（否则页表永久钉住、flush 无进展超时）。 ③ **结构性修法**：FAIL_CACHE 结论改为**按页 LBA 绑定**——建链时把「提交序前一页」的 LBA 记进 pending 槽（`verdict_lba/verdict_valid`），完成时按 LBA 落结论；页尚未入表则暂存进页表 `verdict[]`，入表时消费。删除全局 `pend_idx/pend_valid`，完成顺序反转不再丢结论。
- **下一步**：上板跑全量 `test`（同 seed 才能复现同一 LBA 起点），断言后看故障 shell 自动 dump 的 `DBG … wr-fail lba=… why=… st=… cascade=…`：`why` 直接指认 5 条判失败路径之一。判据：无 `flush stalled`、chunk=8/16/32 全 `[ OK ]`、`wr.count` 归零。
- **证据**：`src/ftl/ftl.c:ftl_cache_pad_callback`（stray 循环）/ `ftl_cache_wr_take_verdict`。
- **历史/验证**：
  - 2026-11-13 板端：`flush stalled 1003ms (no progress 1000ms) with 1 unconfirmed write page(s)` + `seq head lba=802840 st=1 proven=0` → 修复 → 编译通过，未上板
  - 2026-11-13 二修（安全网）：`seq kind=0 act=0 drain=0 wr=1/0` 下 pad 不会被提交，stray 循环不执行；改为 confirm_step 对「无未来结论」的页直接判失败 → 编译通过，未上板
  - 2026-11-13 三修（结构性）：结论按 LBA 绑定 + 暂存表；删除全局游标；test_speed 诊断行随字段移除同步 → 编译通过，未上板
  - 2026-11-13 全量回归复现（同类未闭合）：`test` 全量跑完 media 后 `/speed/ftl_rw chunk=32` 在写阶段约 231 ms 中止（`WR=110822 KB/s RD=0 KB/s` —— 110 MB/s 物理不可能，写阶段未跑完），teardown `cmt_dirty=1632`。同 seed 单跑该套件通过（`WR=3063 KB/s`）；两次运行 **LBA 起点不同**（`test_ftl_media.c` 也消耗共享的 munit RNG 流）⇒ 不是回退，是同一类「页丢结论」在不同起点/边界被触发。判别结论：01:35 构建与 01:17 构建的唯一功能差异是 `ftl_cache_seq_discard`（只走 `pfr_media_format`，其后紧跟 `ftl_init` 整块清零）⇒ 到不了 chunk=32。取证加固：`ftl_cache_fail_s.last_fail` 粘滞记录第一次写页失败（lba/why/status/ms/cascade），`ftl_debug_dump` 打印，`fault_shell_run` 进 shell 自动全量 dump；`/speed` 结果行默认带 `base=` 与 `err(wr/rd)` → 编译通过，未上板
  - 2026-11-13 上板闭合：`test` 全量 **98/98**、`/speed/ftl_rw` 3/3（`err(wr=0 rd=0)`，chunk=32 `WR=3130 / RD=3195 KB/s`）；同批确认根因为 RISK-CACHE-37（padding END 在飞时续写窗口）

#### RISK-CACHE-35 残留 cache 序列门控拒绝格式化同步擦写

- **后果**：`pfr_media_format` 的逐块同步擦除走 `ftl_op_submit`（`SYNC_WAIT`）；若 cache 序列仍 `active/draining`，`ftl_op_seq_gate` 拒绝所有非 owner 提交 ⇒ 每次擦除只能等约 20 ms 同步超时。4096 块 ≈ 82 s，表现为格式化长时间无输出（`test` 占用 CLI 主循环，`ftl_ctx` 也无法执行）。
- **缓解**：`pfr_media_format` 入口调用 `ftl_cache_seq_discard(ftl)`：丢弃残留序列、强制打开门控；未证明页按失败结算，不产生静默映射。
- **下一步**：上板跑 `test /ftl/media/e2e/format_and_roundtrip`，应在 ~21 s 内完成；再跑全量 `test`。
- **证据**：`src/ftl/power_failure_recovery.c:pfr_media_format`；`src/ftl/ftl.c:ftl_cache_seq_discard`；`src/ftl/ftl_op.c:ftl_op_seq_gate`。
- **历史/验证**：
  - 2026-11-13 板端：speed 3/3 后直接跑全量 `test`，`format_and_roundtrip` >60 s 无输出（quiesce 5 s 上限未触发）→ 加 `ftl_cache_seq_discard` → 编译通过，未上板
  - 2026-11-13 上板闭合：`test` 全量 **98/98**、`/speed/ftl_rw` 3/3（`err(wr=0 rd=0)`，chunk=32 `WR=3130 / RD=3195 KB/s`）；同批确认根因为 RISK-CACHE-37（padding END 在飞时续写窗口）

#### RISK-CACHE-37 padding END 在飞时续写窗口

- **后果**：`ftl_cache_seq_close_step` 提交 padding END 后 `closing = true`，但 `active` 仍为真；`ftl_cache_write_try` 的续序列判定只看 `next_lba`，于是**新数据窗口排在 pad 之后**提交。pad 完成即把序列置为 `NONE`（且不升 `gen`），新窗口的最后一页再没有后继 `FAIL_CACHE` 可证明 ⇒ 确认步判 `no verdict`，把已编程成功的页当失败撤销 ⇒ `deferred_error` ⇒ host 下一条命令失败、写阶段中止、flush 不执行（teardown `cmt_dirty != 0`）。触发条件：pad 在飞期间恰好有同一 LBA 流的下一条 host 请求（持续顺序写）。
- **缓解**：`ftl_cache_write_try` 在 `seq->closing == true` 时返回 `FTL_CACHE_WRITE_AGAIN`（等 pad 完成、序列真正关闭后再以新序列续写）；`seq_closing` 已计入 `bg_progress`，等待不计 stall。
- **下一步**：上板回归 `/speed/ftl_rw` 三档 + 全量 `test`；判据：无 `no verdict` / `deferred write error`，三档 `err(wr=0 rd=0)`，teardown `cmt_dirty == 0`。若仍失败，看 `DBG wr-fail … why=` 与 `DBG seq-close by=`。
- **证据**：`src/ftl/cache_batch.c:ftl_cache_write_try`；`src/ftl/ftl.c:ftl_cache_seq_close_step` / `ftl_cache_pad_callback` / `ftl_cache_wr_confirm_step`。
- **历史/验证**：
  - 2026-11-13 板端（02:09 镜像，`/speed/ftl_rw chunk=32`，`base=495392`）：`seq-close by=pad kind=2 act=1 wr=1/4 next=497664`，2 ms 后 `wr-fail lba=497656 why=no verdict`（pad 之后提交的窗口末页）→ 加 `closing` 门控 → 编译通过，未上板
  - 2026-11-13 上板闭合：`test` 全量 **98/98**、`/speed/ftl_rw` 3/3（`err(wr=0 rd=0)`，chunk=32 `WR=3130 / RD=3195 KB/s`）；同批确认根因为 RISK-CACHE-37（padding END 在飞时续写窗口）

## 2026-11-13 新增已闭合

### RISK-DOC-01 ftl_api_usage.md 旧 API 名（已闭合 2026-11-13）

- **原状态**：信息（既有债，2026-10-31）
- **原文**：`docs/design/ftl_api_usage.md` 的非 Journal 分节仍用旧 API 名（`l2p_cache_read` / `l2p_cache_write` / `journal_init` / `journal_append` 等，现名见 `include/ftl/*.h`）。后果：读文档写代码会对不上签名（编译期即可发现，不是静默）。缓解：本次已把 Journal 分节改为「权威定义在 journal.h + 页格式表」，并修正 `journal.c` 同步调用计数行；其余分节按模块逐个收敛（H37 的落地节奏）。重访触发：任何一次改到这些分节的接口文档。证据：`docs/design/ftl_api_usage.md` §2.3 / §3；`include/ftl/ftl.h` / `journal.h`。
- **闭合原因**：用户 2026-11-13 决定删除 `docs/design/ftl_api_usage.md`，不再维护；API 以 `include/ftl/*.h` 的函数契约与 `docs/design/ftl_design.md` §5.1 的模块契约为准。

### RISK-DP-29 新增 FC-NAND UT 覆盖

- **原状态**：待上板（三条新增覆盖已落地，未板端执行）
- **原文**：`fail_cw_attribution` 关放行闩锁合成 dispatch 下发失败，断言 flat 位图与全部选中 desc == `FC_CW_STATUS_FAIL`；`process_read_ecc_max` 断言干净/能力内/UNC 三段 `desc->ecc_bits` 与 `fc_get_cmd_ecc_max` 聚合；`read_column` 直接下发 `OP_READ_COLUMN`，覆盖列地址跨 CW 切分、4 字节偏移与 spare 区读。
- **上板记录**：2026-11-13 13:37:20 镜像 `test` → **61 OK + 3 SKIP**（`/fc-nand/` 59 OK + 3 SKIP），三条用例均 OK；总 27.95 s，seed `0x5a4f21e4`。
- **归档原因**：三条覆盖已上板通过；后续随 FC API/测试源变化做常规回归。

## 2026-11-13 UT 收口新增已闭合

### RISK-GC-16 T5 磨损 GC 永不收敛

- **原状态**：待上板
- **原文**：T5 用「全表 max/min」作比值，min 可能来自从未使用的空闲块 ⇒ 永不收敛；H2testw 期间 GC 持续运行，`free` 从 3388 被搬到 64 后仍不停。
- **闭合原因**：T5 已从 `gc_should_trigger` 移除；磨损维护由 `wear_leveling` 的 UBI 判据接管。T5 失效模式本身不再存在；WL 行为由 `RISK-WL-04` 跟踪。
- **证据**：`src/ftl/gc.c:gc_should_trigger`；`src/ftl/migrate.c:mig_wl_begin`。

### RISK-POOL-11 链校验误拒无 desc 的 multi-plane 擦除

- **原状态**：待上板（2026-11-11 板端复现 `submit: chain invalid`）
- **原文**：`fc_pool_chain_validate` 对 multi-plane 无条件要求 `planes_with_desc == all_planes`，而 multi-plane 擦除/纯加载按契约不建 desc 链。
- **闭合原因**：覆盖校验改为只对 `nand_operation_has_data(op)` 的 multi-plane 数据命令生效；2026-11-13 `/fc-nand/` 中 `fail_cw_attribution`（无 desc 擦除）与 `block_multi_plane` 均通过。
- **证据**：`src/fc/fc_pool.c:fc_pool_chain_validate`；`src/fc/fc.c:fc_build_dispatch_request`；`tests/unit/test_fc_level3.c:test_fail_cw_attribution`。

### RISK-SCH-13 合成失败完成项固定写 cw_status[0]

- **原状态**：待上板
- **原文**：`fc_poll_fail_command` 只标 flat CW 0；命令访问 plane1 或 CW>0 时 desc 级失败归因丢失。
- **闭合原因**：改为按 `fc_get_cmd_cw_bitmap(cmd_id)` 的实际 flat CW 集合布 FAIL；2026-11-13 `/fc-nand/` 中 `fail_cw_attribution` 用 bm=0x0FF 断言全部选中 desc FAIL，上板通过。
- **证据**：`src/fc/fc.c:fc_poll_fail_command / fc_complete_from_nand`；`tests/unit/test_fc_level3.c:test_fail_cw_attribution`。

### RISK-CTRL-02 HIL 入口缺 LBA 范围校验

- **原状态**：待上板
- **原文**：`ssd_controller_submit` 对 READ/WRITE 只校验 `buffer_size`，范围校验依赖 USB/CLI 前端；直接调用 controller 的测试或新前端可把运行时越界 LBA 交给 FTL。
- **闭合原因**：submit 已调 `ssd_controller_lba_range_ok` 并在越界时返回 `INVALID_ARGUMENT` + `complete_flag`；2026-11-13 `/hil/` 15/15 上板通过，`lba_range_ok` 与 `submit_edges` 均覆盖越界。
- **证据**：`src/hil/controller.c:ssd_controller_submit`；`tests/unit/test_hil.c:test_hil_lba_range_ok / test_hil_submit_edges`。

## 2026-11-13 Mode 0 UT 收口新增已闭合

### RISK-UT-12 chain_priority_lock 断言早于 feature 还原

- **原状态**：待后续（Mode 5 下 SKIP，修复未执行）
- **原文**：Step 3 断言排在还原 01h 原值之前；断言失败经 longjmp 跳过还原，器件停在非默认 timing mode。
- **闭合原因**：restore 已排在断言之前；2026-11-13 Mode 0 构建（`CONFIG_NAND_TIMING_MODE=0`，横幅 18:36:56）跑 `/fc-nand/` **62/62，0 SKIP**，`chain_priority_lock` OK。
- **证据**：`tests/unit/test_fc_level3.c:test_chain_priority_lock`；Mode 0 串口日志。

### RISK-NANDP-16 tADL 位置错误

- **原状态**：待上板
- **原文**：`tADL` 原放在 `11h` 后；删掉 `11h` 后 1µs 会使双 plane 编程整页丢失。
- **闭合原因**：`TADL` 移到地址→数据之间、`11h` 后改 TWB+WAIT_READY；2026-11-13 Mode 0 `/fc-nand/` **62/62，0 SKIP**（含 `init_sequence`/`feature_write_read`/`block_multi_plane`/cache 双 plane），2026-11-12 H2testw 371/419 MB 0 错误。
- **证据**：`src/nand/onfi_1_0.c` 相关 UOP 数组；`tests/unit/test_fc_level3.c`；Mode 0 串口日志。

## 2026-11-14 Batch 2A 收口新增已闭合

### RISK-GC-20 满盘启动 idle GC 抽干 free/OP 后每 job 都 abort

- **原状态**：待上板（第一版修复在 `free+OP` 判据上失效）
- **原文**：满盘重启后 `[GC] start reason=periodic mode=idle free=29`；GC 持续搬 valid 数据（`moved=212060 rec=846`），`free` 29→1→0；随后 `free=1 op=24/0`，每个新 job 拿不到成对目标块，`no free block` abort 循环（该原因不打日志），设备只剩读能力。
- **闭合原因**：
  1. GC 低水位**按块计净收益**：`free <= FREE_POOL_TARGET` 时，victim 必须满足 `plane_count > ceil(valid_cw / FTL_CW_PER_BLOCK)`（净增 ≥1 块）才搬；空 victim 始终允许；无压力时保留原收益模式语义。
  2. 工作区预留 `GC_RESERVE_PAIRS = 2` 按**最饿 plane 的 free** 判定（不含 OP）。
  3. OP 仍是所有分配路径共用的余量（曾试过只允许 GC 用 OP，导致 `RISK-HOST-19` 的 wbuf 卡死，已回退）；OP 消耗由净收益闸门约束。
- **板端证据**：
  - 2026-11-13 21:26:30 镜像复位（旧状态 `free=1 op=24/0`）：`GC start trig=0x09 free=4 → end jobs=30 moved=88 rec=30 noop=28 abort=0`，`free 1→32`；
  - 2026-11-13 22:49:55 镜像写满 H2testw：**949/887 KB/s 0 错误**，`GC start free=30 → end jobs=1 moved=3 rec=1 abort=0`；
  - 2026-11-14 满盘掉电重启（`free=8 op=402`）：**无 GC churn**，`free` 稳定，Verify 通过。
- **残余**：WL（`mig_wl_begin`）不受该预留约束，触发条件 `erase spread ≥200`，短测难达到；已在 `RISK-GC-20` 关闭前记录于主表历史。
- **证据**：`src/ftl/gc.c:gc_select_victim / GC_RESERVE_PAIRS / under_pressure`；`src/ftl/allocator.c:alc_super_open`；`docs/reference/README.md` A4（SPDK CSAL GC 缓冲）。

### RISK-HOST-19 低 free 满盘写时 wbuf 拿不到成对块，host 写永不完成

- **原状态**：待上板（根因已定位并回退）
- **原文**：H2testw 写到 413 MiB（`free=14`）时 host 写命令永不完成，USB BOT 超时 → `Code 433`。现场：`wb open=1 base=584 occ=0xFF own=0x3`、两个写组 `next=8/8 out=1`、`free=14 op=204/203 hot=0`、`fc cmd/desc/buf` 全空。根因：当时把「只允许 GC 用 OP」当成结构改动，host 路径无法在 plane 不均时补块，`alc_super_open_alloc_pair(HOT)` 永久失败，wbuf 永远 flush 不出去。（当时误判为「固件挂死」；实际主循环在跑，CLI 因 host 请求 pending 报 busy。）
- **闭合原因**：回退为 `allow_op = (mode != ALC_SUPER_ALLOC_COLD_HIGH_WEAR)`，除静态 WL 外所有分配路径都可在 free 不足时从 OP 补；plane 不均时 host 写窗口可正常完成。
- **板端证据**：2026-11-13 22:49:55 镜像写满 H2testw **949/887 KB/s，`Test finished without errors`**；`free 3350→8`，GC 仅 1 个收益 job，host 写不再卡死。
- **残余**：若 `free + OP` 真正耗尽，host 写组仍会无限重试（`wb_busy` 计为进展、无 deadline）；按容量上界（854,016 LBA ÷ 256 CW/block = 3336 blocks，free+OP 远大于此）本测试不可达，后续可补「无进展超时 → 有界 IO_ERROR」。
- **证据**：`src/ftl/allocator.c:alc_super_open / alc_super_can_open`；`src/ftl/wbuf.c` 分配失败路径；板端 `ftl_ctx`/`q` 现场与 H2testw 输出。

## 2026-11-14 负例框架收口新增已闭合

### RISK-L2P-24 L2P 加载启动失败被当作未映射零填

- **原状态**：待决策 → 待上板
- **原文**：`l2p_read` 在 region 未驻留时起加载并返 `FTL_AGAIN`；`ftl_issue_subop` 用
  `l2p_defer = l2p_cache_any_inflight()` 标记「是否有确定完成」。加载**启动失败**（无空闲行 /
  提交被拒）时没有在飞命令 ⇒ `l2p_defer == false`，同一 `k == 0` 分支把它当成「合法未映射」
  ⇒ 零填 1 个 LBA 并返回 `FTL_OK`。host 读把已映射数据看成 0，属静默数据损坏；drain 也不会
  再重试该 LBA。
- **修复**：`k == 0` 分支用局部 `l2p_again` 区分 break 原因 —— `l2p_read` 返 `FTL_AGAIN`
  （含加载启动失败）一律返 `FTL_AGAIN`，由 drain 的无进展预算结算成 IO_ERROR；只有
  `l2p_read` 返 `FTL_OK` 且 `p == NAND_PBA_INVALID`（合法未映射）才零填。`l2p_defer` 仍只
  表示「有在飞命令」，供 drain 的进展判定使用。
- **闭合原因**：负例 `/ftl/media/negative/l2p_load_fail_bounded` 在板端通过 —— 注入 META 读
  失败时 host 读**有界报错**（不再零填），放开后读回原数据；同一轮其余 8 例也通过，
  正常读写路径（含 wbuf/L2P/Journal/CP/BBT）无回归。缺陷由该负例首次暴露，修复与回归
  判据是同一个用例。
- **残余**：无（该失败模式的实现与用例同批）。
- **证据**：`src/ftl/ftl.c:ftl_issue_subop`（`l2p_again` / `k == 0`）；
  `src/ftl/l2p_cache.c:l2p_read` / `l2p_load_start`；
  `tests/unit/test_negative.c:test_l2p_load_fail_bounded`；
  2026-11-14 11:32:21 镜像板端 9/9。

## 2026-11-14 负例框架收口（第二批）

> 下列风险由 `/ftl/media/negative/` 与 `/fc-nand/negative/` 的板端负例验证后闭合；
> 原文整体保留、不改写。板端证据：debug UT=FTL 36/36（200.10 s）、
> release UT=FTL 39/39（186.77 s）、release UT=FC 9/9（129 µs）。

#### RISK-MD-21 目录检查点 abort 不消费槽位

- **后果**：16 页 record 写到一半超时 abort 后，下一轮复用同一非块首槽、无擦除 ⇒ 对已编程页二次编程；若 abort 发生在块首擦除失败后，同块后续槽也不可用。
- **缓解**：`md_dir_ckpt_s.erase_required` 记录本次是否块首槽；`md_dir_ckpt_abort` 消费当前槽：块中槽 `write_idx++`，块首/擦失败槽跳到下一块槽 0；commit/abort 清标志。
- **下一步**：注入 submit stall / 擦失败；确认下次写点不落在半编程槽；观察 `dir_ckpt` seq/idx 推进。
- **证据**：`src/ftl/metadata.c:md_dir_ckpt_prepare/commit/abort`；`include/ftl/metadata.h:erase_required`。
- **历史/验证**：
  - 2026-11-04 修复 → 编译通过，未上板


> 闭合（2026-11-14）：负例 `cp_dir_ckpt_abort` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-GC-01 永久不可读 victim 反复 abort

- **后果**：永久不可读 / META 损坏的 victim 会反复 abort，容量不再增长。
- **缓解**：每个 plane 源块连续 3 次读失败 ⇒ `bbt_mark_bad` + 从 Closed 条目摘除该 plane；计数跨 job 保留。
- **下一步**：造不可纠读：第 3 次后 GC 换候选、容量继续增长。
- **证据**：`src/ftl/migrate.c:mig_read_complete` / `mig_job_start` / `mig_job_finish`。
- **历史/验证**：
  - （日期未记录）修复：按 plane 连续失败标坏 → 未上板
  - 2026-11-14 debug 板端：用例首版靠「连续 6 次 GC」等同一 victim 被重选，`marked` 未置位。
    定位：victim 按成本排序，而 `fail_gblock[]` **每个 plane 只有一个槽**；失败本身不改变评分，
    但列表中的空条目/同分条目会让槽在换 victim 时被重置，连续 3 次同一 gsrc 不保证可达。
    用例改为「先回收空 victim → 选有数据的候选 → 预置 2 次失败 → 注入 1 次」验证第 3 次标坏
    路径（不再依赖重选）。
  - **残余（待判定）**：若生产中多个不可读 victim 交替被选，同一 plane 的单槽会互相重置，
    永远到不了 `MIG_VICTIM_READ_FAIL_MAX`；需评估是否改成「每 plane N 个 gsrc 计数」。
    触发条件未在真 NAND 上观察到，暂不判定为缺陷。


> 闭合（2026-11-14）：负例 `gc_read_unc_marks_bad` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-GC-17 cache-program batch 页基址误判

- **后果**：`mig_page_base_has_mismatch` 直接比较 `job.base[plane]`，但 `ftl_lba_page_base()` 刻意保留 plane 位 ⇒ 同一 pair 的两 plane 基址恒差 CW_NUM，两 plane 都有数据的页一出现就把 `state.batch_disabled` 永久置位 ⇒ 本次开机 GC/WL 的成对 cache 写全部失效（回退逐页单 plane 写，数据无损、掉吞吐）。
- **缓解**：比较键改为 `ftl_lba_pair_base()`；顺带补齐 batch 提交后的 external/command deadline、batch 攒页复用 `mig_page_next()` 清 per-plane 状态。
- **下一步**：看 `batch_jobs` / `batch_pages` 是否增长、`batch_disabled` 不再置位；GC 吞吐对比。
- **证据**：`src/ftl/migrate.c:mig_page_base_has_mismatch` / `mig_batch_submit`；`include/ftl/ftl_config.h:172`。
- **历史/验证**：
  - 2026-11-04 修复 → 编译通过，未上板


> 闭合（2026-11-14）：负例 `gc_batch_program_fail` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-MIG-01 等待命令丢失/卡住

- **后果**：READ_SETTLE / FLUSH_JRN 等阶段原会把 stall 计时清零，等待无上限；等待中的命令丢失也可能永久 active。
- **缓解**：`mig_job_wait` 使用独立 `FTL_WAIT_TIMEOUT_MS`；`waiting` 期间也 arm 超时，超时 abort 且写阶段关闭 Cold SB；迟到完成只 release。
- **下一步**：注入 region 加载永久卡住/完成丢失：job 超时中止、不永久挂起。
- **证据**：`src/ftl/migrate.c:mig_job_wait` / `cmd_wait_ms` / `mig_job_step`。
- **历史/验证**：
  - （日期未记录）修复（合并 §2K GC-05）→ 未上板


> 闭合（2026-11-14）：负例 `gc_l2p_load_wait_timeout` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-JN-02 同步刷页返回 void

- **后果**：`jrn_flush_page` / `jrn_flush_now` 返回 void：同步刷页失败会置 `write_fail_pending`，但调用者无法知道「屏障未达成」。
- **缓解**：改为 `ftl_status_e` / `bool`；drain 超时、擦失败、写失败均返回 `FTL_ERROR`，`ftl_flush` 记 `LOGW`。
- **下一步**：注入同步写/擦失败，`fl` 能报错且 pending 阻止 victim 擦。
- **证据**：`src/ftl/journal.c:jrn_flush_page`。
- **历史/验证**：
  - （日期未记录）修复 → 未上板


> 闭合（2026-11-14）：负例 `flush_reports_journal_error` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-JN-09 32-bit Journal 序号无回绕处理

- **后果**：`sequence_counter` / `page_seq` / CP `journal_sequence` 全是 DWORD，跨重启单调；按每 LBA 一条、512B 条目折算，累计写 ≈2 TiB 后回绕。回绕后 `jrn_ring_scan` 的 `page_seq + entry_count > cp_sequence` 把新页判成已 checkpoint 而跳过回放；`out_latest` 取 max 会选中回绕前旧页 ⇒ 写指针退回旧位置。
- **缓解**：B 案（串行号比较）：新增 `jrn_seq_after` / `jrn_seq_ge`（RFC 1982 语义）；ring scan latest/回放过滤、resume/truncate/append、L2P WAL 闸门、CP T3 增量、PFR 判型与 gap、metadata 环空哨兵全部改走串行比较；活值窗口上界 ≈13 万 ≪ 2^31。A 案（64-bit）会动 `dirty_seq`/CMT 宽度与介质格式，不采纳。
- **下一步**：回绕无法在板端跑到；需 UT 用合成序号覆盖（测试源由用户维护）。
- **证据**：`src/ftl/journal.c` / `include/ftl/journal.h` / `l2p_cache.c` / `checkpoint.c` / `power_failure_recovery.c` / `metadata.c`。
- **历史/验证**：
  - 2026-11-05 改串行比较 → 编译通过，未上板


> 闭合（2026-11-14）：负例 `journal_seq_serial_compare` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-RR-02 恢复成功仍报 IO_ERROR

- **后果**：恢复成功后仍以 `SSD_STATUS_IO_ERROR` 完成托管子操作；`recovered` 只是最后一个 plane 的结果。
- **缓解**：新增 `all_ok` 聚合位：job 起点置 true，任一 plane 失败或 abort 清 false；`rr_job_finish` 按它报 OK/IO_ERROR。
- **下一步**：可恢复失败读应返回成功；单 plane 不可恢复整体报错。
- **证据**：`src/ftl/read_recovery.c:rr_job_finish` / `rr_settle_plane`。
- **历史/验证**：
  - 2026-11-14 板端 `fc_status_nand_fail`：读完成注入 1 次 NAND_FAIL → RR 重读两 plane 均成功
    （`planes_recovered=2`），host 读返回 **OK** 且数据逐字节一致 ⇒「恢复成功仍报 IO_ERROR」
    已验证修复；残余：单 plane 恢复 / 另一 plane 不可恢复的**混合**场景未构造，`all_ok`
    聚合的另一半未验
  - （日期未记录）修复 → 未上板


> 闭合（2026-11-14）：负例 `rr_mixed_plane_fail` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-RR-03 ECC 健康状态可降级

- **后果**：`rr_record_ecc` 健康状态可降级：CRITICAL/DYING 块在低 ECC 连续上升时被打回 WARNING。
- **缓解**：改为单调 max 升级。
- **下一步**：CRITICAL 后低 ECC 上升不得回落（UT 负例）。
- **证据**：`src/ftl/read_recovery.c:rr_record_ecc`。
- **历史/验证**：
  - （日期未记录）修复：单调升级 → 未上板


> 闭合（2026-11-14）：负例 `rr_ecc_monotonic` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-BOOT-05 快照恢复校验/计数未板验

- **后果**：restore 第 1 遍全量校验（不再部分应用 BAD）、`recovery_materialize_pba` 校验 ch/ce/lun、`recovery_add_closed` 传播列表满、`valid_cw` 改 `uint32_t`，均未板验。
- **缓解**：编译零告警；失败路径回退 full metadata rebuild。
- **下一步**：快照中途注入非法 word；journal 注入越界 PBA；满盘重建确认 `valid_cw` 不截断。
- **证据**：`src/ftl/power_failure_recovery.c`。
- **历史/验证**：
  - （日期未记录）上述改动 → 未上板


> 闭合（2026-11-14）：负例 `boot_corrupt_cp_restore` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-BOOT-15 每次开机都 full rebuild

- **后果**：`pfr_rebuild_closed_sb_list` 把动态元数据 LOG 块直接 `continue`，`pfr_boot_carve_pools` 也跳过 ⇒ 这些块 BST state 停在 NONE 且不属于任何池；`cp_block_snapshot` 写成 `NONE + no pool tag` ⇒ 下次开机 `pfr_restore_blocks` Pass 0 拒绝整份快照 ⇒ 又走 full rebuild，再次留下同样状态。空片 ~0.5s；312 MiB 介质实测 2–4 分钟。
- **缓解**：动态元数据块归 metadata_dir 所有，CARVE 时需要专门处理（见修复）。
- **下一步**：clean/PF 启动路径验证不再循环 rebuild。
- **证据**：`src/ftl/power_failure_recovery.c:pfr_rebuild_closed_sb_list` / `pfr_boot_carve_pools` / `pfr_restore_blocks`；`src/ftl/checkpoint.c:cp_block_snapshot`。
- **历史/验证**：
  - 2026-10-20 L0 修复 → 未上板


> 闭合（2026-11-14）：负例 `boot_no_rebuild_loop` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-BOOT-21 降级位图被误当成禁止加载

- **后果**：`l2p_load_start` 命中 `region_degraded_bitmap` 时直接合成全 INVALID 行、不读 Directory。region 被 journal 回放 / host 写入恢复并 flush 到新页后，行被驱逐再重载会再次合成 INVALID ⇒ 已恢复的映射读不到；GC 迁移反查也会把这些 CW 当无效 ⇒ 可能漏搬、随后擦除有效数据。
- **缓解**：删除短路，加载一律走 Directory（主→备）；位图只用于「未映射的 LBA 报 IO_ERROR」判定。
- **下一步**：制造降级 region → 写入并 flush → cache 压力驱逐 → 重读应命中且 `map`/`rd` 不报错。
- **证据**：`src/ftl/l2p_cache.c:l2p_load_start` / `l2p_is_region_degraded`。
- **历史/验证**：
  - v0.90 修复 → 未上板


> 闭合（2026-11-14）：负例 `boot_degraded_reload` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-L2P-01 region 加载失败降级

- **后果**：region 加载失败时立即降级为全未映射，映射静默不可用。
- **缓解**：先重试 3 次（`L2P_LOAD_RETRY_MAX`），仍失败才置 CLEAN + 全 INVALID；不活锁。
- **下一步**：造瞬态读错验证映射不丢；持续坏 region 时不活锁。
- **证据**：`src/ftl/l2p_cache.c:ftl_l2p_callback`。
- **历史/验证**：
  - （日期未记录）加载失败改为重试 3 次后降级 → 未上板
  - （日期未记录）原归档条目回迁（`docs/archive/known_risks_batches.md §2C`）


> 闭合（2026-11-14）：负例 `l2p_read_error_degrades` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-L2P-03 flush_kick 背压与加载降级

- **后果**：flush_kick 背压返 `FTL_AGAIN`、写回前资源预检、CLOCK 引用位 1、加载失败降级为全未映射；这些路径未板验，有误会导致 flush/CP 中止或热点 region 长期钉住。
- **缓解**：无（仅编译通过）。
- **下一步**：命令池压力下 `fl`/CP 不中止、不烧日志槽；坏 region 读不活锁。
- **证据**：`src/ftl/l2p_cache.c`。
- **历史/验证**：
  - （日期未记录）flush_kick 背压改报 `FTL_AGAIN`、写回前资源预检、CLOCK 引用位改 1、加载失败降级 → 未上板


> 闭合（2026-11-14）：负例 `l2p_flush_backpressure` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-L2P-12 读/取旧值路径绕过 CMT

- **后果**：dirty CMT 可与 CLEAN line 共存；读/oldp 路径漏查 CMT ⇒ 写确认拿到旧 oldp（valid_count 虚高）或读拿到旧 PBA。
- **缓解**：所有读/oldp 路径 CMT 优先；`l2p_entries_read` 已逐 LBA CMT 优先；`l2p_line_get` 仅诊断。
- **下一步**：新增读入口时复核；跑 line CLEAN + dirty CMT 读回负例。
- **证据**：`src/ftl/l2p_cache.c:l2p_read` / `l2p_entries_read` / `l2p_is_ready` / `l2p_entries_ready`。
- **历史/验证**：
  - 2026-11-04 写回 CMT 改造，读/oldp 路径改为 CMT 优先 → 未上板


> 闭合（2026-11-14）：负例 `l2p_cmt_read_priority` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-L2P-18 dirty CMT 低于水位时只在 RAM

- **后果**：host 不 FLUSH 且 CP T3 门未触发时依赖 Journal；Journal 未持久则最近映射丢失（既有写缓存窗口）。
- **缓解**：低于水位只推迟主动写回；IDLE 强制排空、FLUSH 走 force、CP/容量 eviction 也强制；Journal append 先于 dirty 写入。
- **下一步**：低负载写后 idle，看 `cmt` 是否归零；再断电重启读回。
- **证据**：`src/ftl/l2p_cache.c:L2P_CMT_WB_WATERMARK` / `l2p_handle`；`src/ftl/ftl.c:ftl_flush_step`。
- **历史/验证**：
  - 2026-11-04 写回 CMT 水位机制引入 → 未上板


> 闭合（2026-11-14）：负例 `l2p_idle_drain` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-CP-03 状态机改动未板验

- **后果**：提交状态传播（FTL_ERROR 立即中止）、`find_latest` 失败复位游标、水位公式精确到条目，这三处行为改动未经板验。
- **缓解**：编译零告警。
- **下一步**：正常 CP、各 phase 掉电、journal 回切、85%/95% 触发点。
- **证据**：`src/ftl/checkpoint.c`。
- **历史/验证**：
  - 2026-11-14 板端 `cp_abort_phase`：HDR_WRITE 持续提交失败 → 10 ms submit deadline 中止
    （`timed_out=1`、`timeout_phase = fail_phase = CP_HDR_WRITE`），放开后强制 CP 写出新
    Header（sequence 3→4）⇒ 提交状态的持续失败传播与中止后恢复已验；残余：各 phase 定点
    掉电、journal 回切、85%/95% 触发点
  - 2026-11-14 写阶段掉电：`format` + POR 后 H2testw 写到 ~170 MB 拔电；上电 `boot type=2 cp_seq=83 journal_entries=6656 anomalies=0 free=1854`，`power-failure diag gap=6208`、`L2P borrow=1`；随后 Verify **`Test finished without errors`，读 884 KB/s** ⇒ journal 回放 + CP 快照恢复在写中断电场景通过
  - （日期未记录）上述三处改动 → 未上板


> 闭合（2026-11-14）：负例 `cp_abort_phase` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-WL-02 擦除不重置 read_count

- **后果**：`alc_bst_note_erase` 不重置 `read_count`：read disturb 跨擦除周期累计 ⇒ 正常块越过 1000 被预测路径退役。
- **缓解**：擦除成功清 `read_count`；`rr.health / last_ecc / rising_count` 保留（慢死趋势跨周期）。
- **下一步**：长时 soak，正常块不被 `rr_predictive_scan` 误退役。
- **证据**：`src/ftl/allocator.c:alc_bst_note_erase`。
- **历史/验证**：
  - （日期未记录）修复：擦除清 read_count → 未上板


> 闭合（2026-11-14）：负例 `wl_erase_resets_read_count` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-WL-03 Closed SB 含 BAD plane 仍被选中

- **后果**：Closed SB 的某 plane 标 BAD 后仍被 WL 选中：`plane_blocks` 含 BAD 块 ⇒ migrate 读失败 abort，且每轮重选 ⇒ WL 永久卡死。
- **缓解**：`wl_select_victim` 预检：任一 present plane 的块 BAD ⇒ 跳过该 SB。
- **下一步**：运行期标坏后 WL 仍能前进。
- **证据**：`src/ftl/wear_leveling.c:wl_select_victim`。
- **历史/验证**：
  - （日期未记录）修复：victim 预检跳过 BAD plane → 未上板


> 闭合（2026-11-14）：负例 `wl_skip_bad_plane` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-IF-07 批次 chain 违反契约

- **后果**：① 混优先级 ⇒ `fc_submit_batch` 把链拆到 SQ/HSQ 两段，`fc_poll_chain_length` 只沿单队列 peek，跳过的成员被当成不存在 ⇒ cache read 输出错页 / cache program 写错序列；② 末条 `MORE` 无后续 END ⇒ `chain_owner` 永不解锁，该 LUN 的另一队列被永久饿死。
- **缓解**：`fc_submit_batch` 拒绝混优先级；末条 MORE 改为合法（流式中间窗口），由 FTL cache 序列状态机保证最终 END；`ftl_op_submit*` 门控保证窗口间无插入；孤儿序列由 `ftl_cache_seq_idle_guard()`（1s）关闭。
- **下一步**：读/写流式窗口跨 chunk 的 END、D4 断点、wild-chain 负例。
- **证据**：`src/fc/fc_interface.c:fc_submit_batch`；`src/ftl/ftl_op.c` 门控；`src/ftl/cache_batch.c:cache_read_submit`；`include/ftl/ftl.h:ftl_cache_seq_s`。
- **历史/验证**：
  - 2026-11-13 Mode 0 `/fc-nand/` 62/62，`chain_priority_lock` OK → 链锁路径验证；残余：混优先级/wild-chain 负例
  - 2026-10-30 5.2 万窗口、全盘 0 错误；跨 chunk 链正常 → 正常路径上板
  - 混优先级 / wild-chain 负例未注入


> 闭合（2026-11-14）：负例 `batch_arg_guards` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-IF-11 逐页确认依赖完成顺序与 FAILC

- **后果**：缓存程序第 i 页的最终结果由第 i+1 条命令的 `FAIL_CACHE` 给出，确认 FIFO 按提交顺序 pop（头==自己的页才跳过）。若 FC 完成顺序被破坏，或器件 FAILC 的「上一页」归属与实现不符，会把成功页标失败/把失败页当成功（静默坏数据）。
- **缓解**：FC 每 LUN FIFO 严格先进先出；FAILC 只在 op44/op48 映射（op49 首页不映射）；任一错误位 ⇒ 整个序列中止、未确认页一律不确认映射、关闭 hot SB。
- **下一步**：注入单 plane/双 plane 中间页 program 失败，确认失败页及其后页不确认、之前页已确认；若器件 FAILC 归属与假设不符，改为按 desc 逐 CW 状态归因。
- **证据**：`src/ftl/ftl.c:ftl_cache_wr_member_done`；`src/fc/fc.c:778`；`src/nand/nand_fmc.c:nand_fmc_exec_check_fail_cache`。
- **历史/验证**：
  - 2026-10-30 10.1 万页确认、cw abort=0、全盘 0 错误 → 正常路径上板；失败注入未做


> 闭合（2026-11-14）：负例 `` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-IF-15 RAW 读判空把已编程页当空白

- **后果**：FC 只向 FTL 暴露 USER 512B，`ftl_sync_read_page_raw()` 用 LDPC_RAW 刻意跳过 ECC；于是「用户数据恰好全 0xFF」的已编程页被判成未写过。触发：TRIM 掉整个 region ⇒ flush 写出 2048B 全 0xFF 的 region 页（META/CRC/ECC 已编程）⇒ `md_log_block_erased()` 判整块已擦净 ⇒ `md_log_area_restore` 置 NONE 而不擦 ⇒ 下次分配同页二次编程（AND）⇒ CRC/ECC 不符。同一误判还影响 full rebuild 写指针扫描与 BBT/CP 空片探针。
- **缓解**：判据搬回解码路径：① FC `fc_cw_is_blank()` = 整条 528B CW 的 1 比特占比 ≥99%，仅 ECC 解码失败后调用；② 新增 `fc_get_cmd_all_cw_empty()`；③ FTL `ftl_page_blank_check()` / `ftl_page_is_blank()` 普通读一次判空，读不通按非空；④ 块状态路径不再判空（`md_log_area_restore` 全部非活块置 LOG + 登记回收，擦除按水位懒执行）；⑤ 介质探针改 `ftl_page_is_blank`；⑥ 删除 `ftl_page_is_blank_raw()`。
- **下一步**：format → 整 region TRIM → 断电重启，确认日志块被擦除而非 release，再写无 `region degrade`/CRC 错；观测 full rebuild 耗时；99% 阈值为推导值，若空页被判非空下调到 98%。
- **证据**：`src/fc/fc_data_path.c:fc_cw_is_blank`；`src/fc/fc_interface.c:fc_get_cmd_all_cw_empty`；`src/ftl/ftl_fc_interface.c:ftl_page_blank_check` / `ftl_page_is_blank`；`src/ftl/metadata.c:md_log_area_restore`；`src/ftl/power_failure_recovery.c:pfr_scan_write_pointer`；`src/ftl/bbt.c:bbt_medium_has_ftl_data`；`src/ftl/journal.c:jrn_page_raw_blank`。
- **历史/验证**：
  - 2026-11-01 判据搬到解码路径 + 新增 `ftl_page_is_blank` → 正常路径上板
  - 2026-11-01 release 全量 UT 156/156、format --verify + H2testw 415/423 MB 0 错误、拔插后校验 195 MB 0 错误
  - 负例（全 0xFF 用户区的已编程页）未构造


> 闭合（2026-11-14）：负例 `blank_ff_trim_rewrite` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-HOST-02 multi-plane 读只记首个 desc

- **后果**：host 读成功路径只记首个 desc 的 pba，multi-plane 第二块漏记 read_count/ECC。
- **缓解**：`ftl_cmd_block_ecc_list` 逐 plane 反查命令访问的全局块与 ECC。
- **下一步**：multi-plane 读：两个块的 read_count / ECC 都增长。
- **证据**：`src/ftl/ftl_fc_interface.c`；`src/ftl/ftl.c:ftl_host_io_read_ok`。
- **历史/验证**：
  - （日期未记录）修复：逐 plane 反查 → 未上板


> 闭合（2026-11-14）：负例 `host_read_multiplane_stats` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-HOST-17 host queue entry 生命周期

- **后果**：`ssd_host_queue` 的 entry 由 USB slot 提交/unmap start 分配，由 `usb_msc_slot_release` / CLI 完成 poll 释放；若某条失败/复位路径漏 release，8 个 entry 用尽后所有 host 提交都失败（USB I/O error）；重复 release 则 ASSERT 停机或 release 下 `used--` 下溢。
- **缓解**：entry 分配点只有两处，所有释放路径统一走 `usb_msc_slot_release`；CLI 在 submit 失败与完成 poll 两处释放；2026-11-05 复核：`q` 当前未输出 `hostq`（`ssd_host_queue_used()` 仅 UT 使用）⇒ 现场观测路径尚不存在。
- **下一步**：补一行 `hostq` 到 `q`；H2testw + 拔插/复位后 `hostq` 必须回 0；`/hil/host_queue` 覆盖 alloc/free/owner。
- **证据**：`src/hil/host_queue.c/.h`；`src/hil/usb_msc.c:usb_msc_slot_release` / `usb_msc_slot_submit` / `usb_msc_unmap_start`；`src/hil/cli.c:uart_cli_host_submit` / `uart_cli_pending_poll`；`tests/unit/test_hil.c:host_queue`。
- **历史/验证**：
  - 2026-11-05 复核：`q` 未输出 hostq，观测路径待补 → 未上板


> 闭合（2026-11-14）：负例 `` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-CR-04 request 表改用 command_id 直接索引

- **后果**：`g_op_req` 改为按 command_id 直接索引（容量 = `FC_COMMAND_POOL_DEPTH`，元素 `{ctx, op, pending}`），删除线性扫描与 `used / cmd_id:15`；`fc_cmd_status_s` / `fc_cmd_s` 删除 `op_tag`。失败场景：① 非法 command_id 直接索引越界（`ftl_op_req_add` 入口返 `FTL_ERROR`）；② 仍读 `completion.op_tag` / `cmd->op_tag` 的代码会编译失败；③ 超时/迟到完成时表项已摘 ⇒ dispatch 走 missing-request 只 release；④ 要求 ctx 的 op 传 NULL 属编程错误：debug ASSERT，release 无运行期拒绝（回调用 NULL 崩溃，不是静默介质破坏）。
- **缓解**：表容量 = 池深、无表满；直接索引 O(1)；每条在飞命令都有 request。
- **下一步**：上板 H2testw + `[FTL]` 日志；UT `ftl_op_req_count` / request 表行为。
- **证据**：`src/ftl/ftl_op.c:ftl_op_req_add` / `ftl_op_drain`；`include/fc/fc_command.h:fc_cmd_s` / `fc_cmd_status_s`；`include/ftl/ftl_op.h`。
- **历史/验证**：
  - 2026-11-03 完成派发表主流化 → 编译通过，未上板


> 闭合（2026-11-14）：负例 `op_req_table_guard` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-WB-01 FLUSH 失败丢弃页对导致容量流失

- **后果**：`wbuf_handle(FLUSH)` 取到页对后 4 条失败返回（命令池满 / 建链 / pending 满 / SQ 满）丢弃该页对，而 allocator 无归还 API ⇒ 缓冲保留、每轮重试白烧 2 页，容量无声流失。
- **缓解**：资源次序改为按有无归还路径排序（命令槽先于页对）+ 新增 `alc_super_open_rewind_pair` + 统一错误出口归还页对。
- **下一步**：`next_page` 属地址/容量类量，必须上板；背压下命令池满不易构造 ⇒ 静态收口 + 可达路径收窄，不宣称已取证。
- **证据**：`src/ftl/wbuf.c:wbuf_handle(FLUSH)`；`src/ftl/allocator.c:alc_super_open_rewind_pair`。
- **历史/验证**：
  - （日期未记录）修复：资源次序 + 页对归还 → 未上板


> 闭合（2026-11-14）：负例 `data_write_fail_bounded` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-ARCH-08 提交/等待计数改绝对 ms deadline

- **后果**：migrate / RR / CP 的提交/等待计数已由轮数改为绝对 ms deadline：`FTL_SUBMIT_TIMEOUT_MS = 10` / `FTL_WAIT_TIMEOUT_MS = 50`；confirm park 用 `FTL_GROUP_DRAIN_TIMEOUT_MS = 100`。
- **缓解**：时钟缺失（now_ms == 0）时 deadline 不生效（退化为无超时；本板 time_us 恒非空 ⇒ 不可达）。
- **下一步**：上板标定这三个值（NAND 页操作 ~700µs + 主循环轮速）；观察 `timed_out`/`timeout_phase` 上报。
- **证据**：`include/ftl/ftl_module.h`；`src/ftl/migrate.c` / `read_recovery.c` / `checkpoint.c` / `pending.c`。
- **历史/验证**：
  - （日期未记录）deadline 改造 → 未上板


> 闭合（2026-11-14）：负例 `cp_abort_phase` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-ALLOC-13 `alc_super_open_rewind_pair` 无下溢守卫

- **后果**：`next_page` 是 `BYTE`，误调用（未紧邻一次成功 alloc）时 `0--` = 255；PBA 页域只有 6 bit（`PAGES_PER_BLOCK=64`）⇒ 255 被掩码成页 63，静默二次编程最后一页。
- **缓解**：入口 `ASSERT(next_page > 0)` + release 拒绝（LOGE 后 return，不递减）；所有调用点都紧跟一次成功的 `alc_super_open_alloc_pair`。
- **下一步**：若将来允许跨函数 rewind，需重新评估。
- **证据**：`src/ftl/allocator.c` `alc_super_open_rewind_pair`；`include/ftl/ftl_config.h` `PBA_PAGE_MASK`。
- **历史/验证**：
  - 2026-11-04 入口加 `ASSERT(next_page > 0)` + release 拒绝 → 未上板


> 闭合（2026-11-14）：负例 `alloc_rewind_underflow` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-ALLOC-16 `alc_spare_pop` 不跳过 BAD

- **后果**：`alc_free_refill_from_op` 会丢弃 BAD 的 OP 条目，但 `alc_spare_pop` 只按下标弹出。若 Spare 池里的块在运行期被标坏（CLI `bbt --erase` 诊断 / 将来复用路径），`bbt_spare_refill` 弹出它 → `alc_free_push` 拒绝 BAD → 仍返回 `FTL_OK` ⇒ `bbt_mark_bad` 不再走 OP 兜底，本次替换落空，spare 计数静默 -1。
- **缓解**：`alc_spare_pop` 循环弹出并丢弃 BAD，直到取到好块或池空（与 `alc_free_refill_from_op` 同口径）；全坏时返回 false，上层走 OP 兜底。
- **下一步**：上板/注入：把 Spare 池尾块标 BAD 后触发一次坏块替换，确认取到下一个好 spare（或正确回退 OP）。
- **证据**：`src/ftl/allocator.c` `alc_spare_pop`；`src/ftl/bbt.c` `bbt_spare_refill`。
- **历史/验证**：
  - 2026-11-05 `alc_spare_pop` 循环丢弃 BAD → 未上板


> 闭合（2026-11-14）：负例 `alloc_spare_skip_bad` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-CACHE-23 持续 host 写期间后台 CP 被序列门控饿死

- **后果**：持续 host 写期间，写序列 `active` 时门控拒绝非所有者提交，后台不置 `break_requested`；序列在 Journal 配额处结束时 drain 立刻接手同一 host 命令剩余 LBA，门控空窗只有 µs 级（仅当配额恰落在大块边界时约 18 ms）⇒ Checkpoint 推进很慢（后台让步窗口过小），掉电恢复点较旧；不是死锁。元数据收效变慢。
- **缓解**：已实现「让行」机制（2026-11-14，未上板）：门控拒绝 `FTL_OP_L2P` 时除置 `break_requested` 外再置 `seq.yield_pending`（记 `yield_ms`）；host 的写/读路径在**开新序列**前检查 `cache_seq_yielded()`，让行期间返回 `AGAIN/DECLINED`，保证被拒的 L2P 加载在序列收尾后的下一轮拿到一次提交机会；`ftl_op_submit` 在 L2P 提交成功时清标志；超过 `FTL_CACHE_SEQ_YIELD_MAX_MS = 20 ms` 自动放弃让行，防止后台故障饿死 host。drain 的 `bg_progress` 把 `yield_pending` 计为确定性等待，避免误判 stall。
- **下一步**：板端满盘写验证：`[MIG] abort … l2p load timeout` 应消失或显著减少，CP `phase=8/9` 推进加快；同时确认写吞吐无明显回退（让行会暂停 host 一个窗口）。若吞吐损失明显，改为「连续被拒 N 次/超过 N ms 才让行一次」的年龄条件。
- **证据**：`src/ftl/ftl_op.c:ftl_op_seq_gate / ftl_op_seq_yield_clear`；`src/ftl/cache_batch.c:cache_seq_yielded`；`src/ftl/ftl.c:bg_progress`；`src/ftl/checkpoint.c:cp_job_step`。
- **历史/验证**：
  - 2026-11-14 08:30:32 镜像写满 H2testw 板端验证：低水位 `GC start free=30` 后 `[GC] end jobs=3 moved=24 rec=3 noop=2 **abort=0**`，全程**无** `[MIG] abort … l2p load timeout`（同一场景上一版必现）；写满 0 错误，掉电后 Verify 通过。⚠ 同期仍出现 2 次 `confirm park timeout`（见 `RISK-HOST-05`，附近无加载失败日志，属另一支路）
  - 2026-11-14 实现让行机制（`yield_pending` + 20 ms 上限），`make all` / `make release` / `make tests UT=FTL` 零告警 → 上板验证通过（MIG L2P 饥饿路径）
  - 2026-11-13 满盘写实测 `[MIG] abort kind=gc … — l2p load timeout`（GC 的 L2P 加载被写序列饿死，50 ms 等待超时）→ 复现饥饿
  - 2026-10-31 上板观察：t=80 s 起 `phase=8`（WEAR_WRITE）持续 460 s、心跳 `age=1000~2003ms`、`cp=100`；上一失败运行的 `closed=1896`/`dir_ckpt seq=458` 下次开机正常恢复 → 实测为慢，非死锁


> 闭合（2026-11-14）：负例 `` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-CACHE-27 早回执后读不到新数据

- **后果**：控制器层提交即回执（`usb_msc.c` 写槽 `ack_sent`），但 FTL 读路径只有 wbuf 一个覆盖层，已回执但未确认的 cache-program 页不在其中 ⇒ 主机「写完立刻读同一 LBA」会读到旧映射的旧数据。
- **缓解**：`cache_stream.seq.wr` 升级为「已编程页表」（lba/pba0/pba1/cw_bitmap/state），读路径在 `wb_read` 之后、`l2p_read` 之前查该表，命中直接读刚编程页；`cache_read_probe_pair` 对未确认 pair 判不可批量。页在自身命令完成时入表（提交时不压下标）；结论用独立游标 `pend_idx`（不能复用队头：确认背压会让队头停在「已证明等确认」的页，新结论会打错页 ⇒ 环排不空、flush 超时）。
- **下一步**：上板：写完立刻读回（未确认窗口）逐字节比对；H2testw 415 MB 0 错误。
- **证据**：`src/ftl/ftl.c:ftl_issue_subop` / `ftl_cache_wr_member_done` / `ftl_cache_wr_take_verdict`；`src/ftl/cache_batch.c:cache_read_probe_pair`；`include/ftl/ftl.h:ftl_cache_wr_s`。
- **历史/验证**：
  - 2026-11-01 media 36/36 含 `cache_failc_abort` 三档；`/speed` 写后读回 err=0 → FTL/UT 层上板通过；USB 端到端未注入
  - （日期未记录）五关构建零新告警 → 编译通过


> 闭合（2026-11-14）：负例 `` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-CACHE-28 FAILC 撤销的语义边界（本仓自拟取舍）

- **后果**：① 只对已确认（已进 L2P）的失败页做「L2P 作废 + region 降级 ⇒ 读 IO_ERROR」；同批未确认页被中止丢弃时保留旧映射，读仍返回旧数据（失败由 deferred error 上报，符合「一次失败写不影响旧数据可见性」）；② region 降级位是 RAM 级、重启后丢失（`l2p_owner_invalidate` 会清），所以「失败 LBA 读 IO_ERROR」只在本开机周期成立；重启后这些 LBA 是「未映射 ⇒ 零填」。主流对照：NVMe 用命名空间级策略——DLFEAT 规定去分配/未写 LBA 的读值（`001b` = 全零，与本仓重启后一致），另有 DULBE 特性让整个命名空间「读去分配区间」报错；没有 per-LBA 持久丢失标记这种形态。⇒ 裁决不升级（不持久化降级位）：无出处支持、要改介质格式（版本+1、首启格式化）、收益仅跨重启可见，失败本身已经由 deferred error 上报。FAILC 撤销的 region 粒度降级：已早回执的页被器件 FAILC 判失败时，撤销做法是把该页 LBA 在 L2P 中写 `NAND_PBA_INVALID` 并标记整 region degraded（复用 RISK-BOOT-18 机制）⇒ 该 region 内其它未映射 LBA 的 host 读也会报 IO_ERROR（粗于「只影响失败 LBA」）。主流做法是 per-LBA 的丢失标记/deferred sense，本仓因无自定义 sense 且不想新增持久状态而取 region 粒度。
- **缓解**：失败可见（不静默、不回旧数据）；粘性错误由下一条 host 命令上报一次。region degraded 在 `mark_failed` 时立即置位（不等撤销）——否则撤销走完前 L2P 仍指着编程失败的页，读会返回可疑内容；读路径用 `ftl_cache_wr_failed_covers()` 把覆盖失败页的 LBA 直接判 IO_ERROR，cache 读批量对该 pair 判不可批量。
- **下一步**：上板 + 注入：`fi_cache_fail_arm` 注入 FAILC ⇒ 下一条命令 IO_ERROR 一次、失败 LBA 读 IO_ERROR、同 region 未映射 LBA 也 IO_ERROR（把粒度后果记为预期）；若将来需要精确粒度，先裁决「per-LBA 丢失位图 + 掉电语义」。
- **证据**：`src/ftl/ftl.c:ftl_cache_wr_member_done` / `ftl_cache_wr_failed_covers`；`src/ftl/l2p_cache.c:l2p_is_region_degraded` / `l2p_note_region_lost`。
- **历史/验证**：
  - 2026-11-01 `cache_failc_abort` arm=1/2/last 全过（失败页读 IO_ERROR、deferred 只报一次、重启后读零） → FTL/UT 层上板通过；USB 端到端未注入


> 闭合（2026-11-14）：负例 `` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-POOL-01 buffer 归属表缺失

- **后果**：buffer 没有归属表时，双释放 / 在飞释放只能靠调用者契约；同一节点二次头插会让空闲链成环。
- **缓解**：`fc_core_buffer_release` 要求传入期望 owner，owner 不符或未占用即拒绝；`buffer_owner[]` 在 alloc 时置 `FC_COMMAND_ID_INVALID`、建链时改命令 ID；释放闸门同时校验下标 / 状态 / owner；公开 `fc_buffer_release` 恢复 `void`，只允许释放未挂链 buffer；内部返回 bool 作为判据 9 闸门。
- **下一步**：上板阴性用例：双释放、命令在飞时释放、上层释放已挂链 buffer 均被拒；UT 同步。
- **证据**：`src/fc/fc.c` `fc_core_buffer_alloc` / `fc_core_buffer_release`；`src/fc/fc_interface.c` `fc_buffer_release`。
- **历史/验证**：
  - 2026-11-06 增加 `buffer_owner[]` 与带 owner 的释放闸门 → 未上板


> 闭合（2026-11-14）：负例 `in_flight_release_guard` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-POOL-05 资源统计扫描三池

- **后果**：`used` 计数若由扫描派生，扫描窗口内的分配 / 释放会让 `q` 与真值不一致；三池全扫（448 次迭代）的开销也落在统计路径上。
- **缓解**：`fc_get_resource_stats` 改读 O(1) 计数；`command_used` / `descriptor_used` / `buffer_used` 只在分配 / 释放闸门增减，另维护 high-water；debug 档保留一次全池扫描并 `ASSERT` 计数与真值一致（证明不漂移），release 档只读计数器。
- **下一步**：上板跑 `q` 核对 used / hwm；UT 同步新增字段。
- **证据**：`src/fc/internal/fc_internal.h` `fc_ctl_s`；`src/fc/fc.c` 三池闸门 / `fc_get_resource_stats`；`src/hil/cli.c` `q`。
- **历史/验证**：
  - 2026-11-06 统计改 O(1) 计数 + debug 差分断言 → 未上板


> 闭合（2026-11-14）：负例 `stats_consistency` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-DP-10 fc_nand_dispatch_enqueue 入口检查只有 debug ASSERT

- **后果**：`requests != NULL`、`count ∈ [1, DEPTH]`、`ch/ce/lun < NUM`、链成员同 target、`cw_bitmap` 上界只有 debug ASSERT；release 下 NULL/count=0 会解引用 `requests[0]`，越界 route 会按错误 LUN 入队/越界索引，错 target 链会静默进同一 FIFO。
- **缓解**：上述检查全部改 `ASSERT_REJECT(NAND_RESULT_INVALID_ARGUMENT)`；operation 原有 release 拒绝保留。
- **下一步**：上板/负例：NULL、count=0、越界 route、跨 target 链，确认 debug 停机/release 返回 INVALID。
- **证据**：`src/fc/fc_nand_dispatch.c:fc_nand_dispatch_enqueue`；`src/fc/fc.c:fc_submit / fc_poll_fill_queue`；RISK-SCH-04。
- **历史/验证**：
  - 2026-11-07 修复：enqueue 入口检查全部改 `ASSERT_REJECT` → 未上板


> 闭合（2026-11-14）：负例 `dispatch_enqueue_guards` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-DP-23 fc_nand_start 依赖调用者预绑定 engine 指针

- **后果**：`fc_nand_start()` 把 request 拷进 `transaction->request`，随后 `fc_uop_engine_start()` 使用 `state->request/state->completion`；这两个指针只有 `fc_nand_dispatch_task_load()` 和高速切换函数会绑定；新调用者用零初始化 `fc_nand_txn_s` 直接 start 会触发 debug 断言，release 下解引用空指针。
- **缓解**：`fc_nand_start()` 统一绑定 `engine.request = &transaction->request`、`engine.completion = &transaction->completion`；保留 `engine.backend` 由调用者按 target 绑定，入口用 `ASSERT_REJECT` 拒绝空 backend/非法参数；头文件写明契约。
- **下一步**：负例：零初始化 txn（不绑定 request/completion）调 start，确认 debug 停机、release 返回 INVALID；上板回归 dispatch 与高速切换。
- **证据**：`src/fc/fc_nand_engine.c:fc_nand_start`；`include/fc/fc_nand_engine.h:fc_nand_start`；`src/fc/fc_nand_dispatch.c:fc_nand_dispatch_task_load`；`fc_nand_enable_high_speed_target`。
- **历史/验证**：
  - 2026-11-07 修复：start 统一绑定 request/completion → 未上板


> 闭合（2026-11-14）：负例 `nand_start_unbound_txn` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-DP-25 dispatch 不校验 req/completion 表项非空

- **后果**：`fc_nand_dispatch_enqueue()` 只拒绝 `operation >= OP_COUNT`；未使用槽位（表项为 NULL）仍可入队，随后 `fc_nand_dispatch_task_start()` 的 `g_fc_nand_dispatch_req_table[op](task)` 或 `fc_nand_dispatch_task_complete()` 的 completion 表调用会空指针崩溃。
- **缓解**：enqueue 对每个 request 增加 req/completion 表项非空 `ASSERT_REJECT`，debug 停机、release 返回 `INVALID_ARGUMENT`；当前 FC/FTL 已实现 op 均通过检查。
- **下一步**：负例：构造未使用/保留 op 提交，确认 debug 停机、release 拒绝；上板回归正常 op。
- **证据**：`src/fc/fc_nand_dispatch.c:fc_nand_dispatch_enqueue / fc_nand_dispatch_task_start / fc_nand_dispatch_task_complete`；`include/nand/onfi_1_0.h:OP_COUNT`。
- **历史/验证**：
  - 2026-11-07 修复：enqueue 增加 req/completion 表项非空检查 → 未上板


> 闭合（2026-11-14）：负例 `dispatch_enqueue_guards` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-SCH-07 fc_submit_batch 不拒绝批内重复命令指针

- **后果**：校验循环在提交前对每条都查 `ALLOCATED`，同一 `fc_cmd_s*` 出现两次时两次都通过；提交循环 push 两次，第二次 `fc_pool_command_state_set(QUEUED)` 被拒绝但队列里已经多了一条；fill 会把同一命令当两条派发（setter 拒绝后 dispatch 仍继续），可能对同一 desc/buffer 二次下发；FTL `ftl_op_req_add` 也只在 pending 表里覆盖并打 `LOGW`。
- **缓解**：批量校验循环加 O(n²) 重复指针检查，命中即返回 `FC_STATUS_INVALID_ARGUMENT`，且在 `sq_need/hsq_need` 计数之前返回。
- **下一步**：构造批内重复命令负例（debug 停机 / release 拒绝），上板确认队列里不会出现重复 ID。
- **证据**：`src/fc/fc.c:fc_submit_batch / fc_sched_submit / fc_poll_dispatch_single`；`src/ftl/ftl_op.c:ftl_op_req_add`。
- **历史/验证**：
  - 2026-11-06 修复：批内加 O(n²) 重复指针检查 → 未上板


> 闭合（2026-11-14）：负例 `batch_arg_guards` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-SCH-08 fc_get_cmd_data_ptr 的 cw_index 不带 plane

- **后果**：multi-plane 命令的链上同一 CW 号在两个 plane 各有一条 desc，函数返回链序第一个匹配（通常 plane 0）⇒ 若用它们取 plane 1 的数据/META，会静默拿到 plane 0。当前调用者 `ftl_sync_read_page_raw` 与 `gc_decode_cw_lba` 都只用于单 plane 命令，未触发。
- **缓解**：头文件明确「仅单 plane」；实现加 debug ASSERT + release 返回 NULL（不静默交出错 plane）；multi-plane 逐 CW 读取走 `ftl_cmd_read_data` 沿链遍历。
- **下一步**：上板回归单 plane 读/GC；若将来新增 multi-plane 调用者，改加 plane 形参或新 API。
- **证据**：`src/fc/fc_pool.c:fc_get_cmd_data_ptr / fc_buffer_get_meta`；`src/ftl/ftl_fc_interface.c:ftl_sync_read_page_raw`；`src/ftl/gc.c:gc_decode_cw_lba`。
- **历史/验证**：
  - 2026-11-06 修复：头文件限定单 plane，release 返回 NULL → 未上板


> 闭合（2026-11-14）：负例 `data_ptr_single_plane_only` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-SCH-12 fc_submit_batch 的入参/提交不变量只有 debug ASSERT

- **后果**：`cmds != NULL`、`n > 0` 在 release 下分别崩溃/返回 OK；提交循环 `ASSERT(st == OK)` + `(void)st` 忽略预检后的 `fc_sched_submit` 失败，可能半批入队。
- **缓解**：`cmds/n` 改 `ASSERT_REJECT(INVALID_ARGUMENT)`；提交循环失败置 controller fatal 并返回 IO_ERROR。
- **下一步**：上板负例：NULL 数组、n=0、预检后人为破坏队列不变量。
- **证据**：`src/fc/fc.c:fc_submit_batch / fc_sched_submit`；RISK-SCH-07。
- **历史/验证**：
  - 2026-11-07 修复：入口改 `ASSERT_REJECT`，提交失败置 fatal → 未上板



> 闭合（2026-11-14）：负例 `batch_arg_guards` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

#### RISK-NANDP-13 timing mode 查询/设置的 release 边界不完整

- **后果**：`mt29f_set_timing()` 的 `ch/ce/mode` 仅 debug `ASSERT`，release 可越界写 `s_timing_mode` 或接受非法 mode；`mt29f_get_timing_mode()` 只 debug ASSERT ch/ce；`nand_get_timing_mode()` 若返回 `BYTE` 则无错误通道，也不先走 `nand_board_select_target()`。当前 CLI 用合法 die/ch/ce，engine 只传 0/4/5，暂不触发。
- **缓解**：按 H38 采用 status+出参：`nand_result_e nand_get_timing_mode(ch,ce,BYTE *mode)`；descriptor 回调和 `mt29f_get_timing_mode` 同步；`nand_get_timing_mode` 先 `nand_board_select_target()`；`mt29f_set_timing` 对 ch/ce/mode/fmc_hz 用 `ASSERT_REJECT`；CLI 两个调用点失败显示 `?`。
- **下一步**：上板：CLI `info`/`id` 正常显示 mode；负例用非法 ch/ce/absent target，确认 release 返回错误而不是越界；回归高速/低速切换。
- **证据**：`src/nand/devices/mt29f___g08a.c:mt29f_set_timing` / `mt29f_get_timing_mode`；`src/nand/nand_board.c:nand_get_timing_mode`；`include/nand/nand_device.h:nand_timing_mode_fn_t` / `nand_get_timing_mode`；`src/hil/cli.c:uart_cli_info` / `uart_cli_id`。
- **历史/验证**：
  - 2026-11-07 改 status+出参、`ASSERT_REJECT` → 未上板


> 闭合（2026-11-14）：负例 `nand_timing_bounds` 板端通过；覆盖明细见 `docs/design/negative_coverage.md`。

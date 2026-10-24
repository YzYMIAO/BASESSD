# known_risks 历史批次归档

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

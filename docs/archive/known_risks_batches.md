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


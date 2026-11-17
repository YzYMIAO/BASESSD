# FTL 单元测试设计（板端）

> 状态：活跃设计文档。本文是 **test design**，按被测模块（test item）组织：
> 每个模块有哪些设计要点、用什么判据覆盖、缺什么。
> 用例名只作代表，完整清单以 `tests/unit/test_ftl_*.c`、`test_negative_*.c` 的注册表为准；
> 运行结果见 `build/test-results/`，风险状态见 `docs/design/known_open_items.md`。
> 范围：`src/ftl/`（FTL 层）。HIL（`src/hil/`）的 UT 不在本文，见
> `docs/design/known_open_items.md` ISS-HIL-01。
> 关联：`docs/design/ftl_design.md`（被测设计）、
> `docs/guides/testing_guide.md` §12（负例与故障注入框架，跨层）、
> §3（L2 FTL）。

---

## 0. 文档定位与单一事实源

本文回答三个问题：

1. 被测模块各自的设计要点（不变量、契约、错误语义）是什么；
2. 这些要点靠哪些测试层、什么判据覆盖；
3. 哪些点当前没有覆盖，原因是什么。

下面这些信息**不在本文复制**：

| 信息 | 唯一事实源 | 本文怎么引用 |
|---|---|---|
| 用例是否存在、参数、断言 | `tests/unit/test_ftl_pure.c`、`test_ftl_media.c`、`test_negative_pure.c`、`test_negative_media.c`、`test_speed.c`；`test --list` | §2 只写设计点与代表用例，不列全量用例表 |
| 风险状态、历史、是否闭合 | `docs/design/known_open_items.md`（已闭合历史另见 `docs/archive/known_risks_batches.md`） | §7 只写风险 → 模块追溯，**不写状态词** |
| 负例覆盖判定（M/N/K） | 本文 §7.2 台账 | §3.5 只给入口与套件映射 |
| 故障注入框架、seam 语义 | `docs/guides/testing_guide.md` §12 | §4.9 只写本套件怎么用 |
| 未实现 / 未验证的设计项 | `docs/design/known_open_items.md` | §6 只给链接 |
| 每次运行结果与证据 | `build/test-results/` + `docs/guides/testing_guide.md` §8.7 | §5.4 只给目录与四件套 |
| 分层与跨层测试流程 | `docs/guides/testing_guide.md` | 本文沿用其 L0~L6 坐标 |

> 判据：读者要知道「某风险现在什么状态」，去 `known_open_items.md`；要知道「某条用例断言
> 什么」，看测试源。本文两处都不重复。

---

## 1. 测试项与测试分层

### 1.1 测试项清单

顺序按模块依赖：块账本 / 坏块 / 元数据 → 地址映射 → host 请求 → 后台搬移 → 掉电恢复。

| 测试项 | 源文件 | 主要接口 / 状态 | 测试层 |
|---|---|---|---|
| `allocator` + BST | `src/ftl/allocator.c`、`include/ftl/bst.h` | `alc_*`、Free / OP / Spare / Closed、块状态、`valid_count` | pure / negative / media |
| `bbt` | `src/ftl/bbt.c` | `bbt_*`、坏块记录 ring、候选池、Spare 补池 | pure / media / negative |
| `metadata` / `metadata_job` | `src/ftl/metadata.c`、`metadata_job.c` | `md_*`、Directory 双副本、ring、压缩 job | pure / media / negative |
| `checkpoint` | `src/ftl/checkpoint.c` | `cp_*`、CP phase、Header、Directory checkpoint | pure / media / negative |
| `journal` | `src/ftl/journal.c` | `jrn_*`、append / truncate / replay、WAL 水位 | pure / media / negative |
| `L2P` / `CMT` | `src/ftl/l2p_cache.c`（L2P + CMT） | 驻留行状态、miss / load、CMT set 路、WAL 闸门 | pure / media / negative |
| `pending` / group | `src/ftl/pending.c` | `pend_*`、组槽、确认停车、错误聚合 | pure / negative / media |
| `wbuf` | `src/ftl/wbuf.c` | 页打包窗口、owners、逐 CW 读回 | pure / media / negative |
| `ftl_op` | `src/ftl/ftl_op.c` | request 表、route、ctx 旁表 | pure / negative / media |
| `bg_scheduler` | `src/ftl/bg_scheduler.c` | 任务表、等级选择、门控 | pure / negative / media |
| `migrate` / `GC` | `src/ftl/migrate.c`、`gc.c` | `mig_*`、victim 选择、搬移 / 擦除 / 失败封顶 | pure / media / negative |
| `wear_leveling` | `src/ftl/wear_leveling.c` | `wl_*`、UBI 判据、victim、目的块准备 | pure / media / negative |
| `read_recovery` | `src/ftl/read_recovery.c` | `rr_*`、健康等级、预测退役、读失败闸门 | pure / media / negative |
| `cache_batch` / L2P cache | `src/ftl/cache_batch.c`、`src/ftl/l2p_cache.c` | FAILC、早回执、序列读、CMT 写回 | pure / media / negative |
| `ftl.c` / `ftl_fc_interface` | `src/ftl/ftl.c`、`ftl_fc_interface.c` | `ftl_*` 对外接口、提交 / 取完成 / 同步等待 | media / negative |
| `power_failure_recovery` | `src/ftl/power_failure_recovery.c` | `pfr_*`、启动恢复、CP / Journal / BBT 对账 | media / negative |
| `ftl_ut_common` | `tests/unit/ftl_ut_common.c` | 活实例、测试 RAM、伪时钟、模拟重启、清理断言 | 所有套件 |

> 测试项按「模块 + 公共接口」切，不按函数逐个列；内部 `static` helper 由所属模块的
> 公共接口间接覆盖。`tests/` 侧夹具也算测试项，因为它承担「不变量归零」的判据。

### 1.2 测试层与套件

| 层 | 根套件 | 环境 | 能证明什么 | 不能证明什么 |
|---|---|---|---|---|
| **FTL-U / pure** | `/ftl/pure/` | RAM，测试自有对象 | 模块状态机、账本、边界、错误分支 | 持久化、真实 NAND 时序 |
| **FTL-I / media** | `/ftl/media/` | 真 NAND（首次 format，部分用例再 format） | 跨模块契约、持久化、恢复、GC/WL、cache | 真 POR、长时 soak |
| **negative pure** | `/ftl/negative/pure/` | RAM + `fi_*` | 非法状态、守卫、注入降级 | 真实介质失败 |
| **negative media** | `/ftl/negative/media/` | 真 NAND + `fi_*` | 读 / 写 / 擦失败、恢复拒绝、多 plane | 真掉电窗口 |
| **PERF** | `/bench/speed/ftl_rw` | 真 NAND | 能不能跑完 + 错误计数 | 功能正确率（不是通过判据） |
| **MANUAL** | 板端流程 | 真板 | POR、USB、H2testw、坏块注入 | 不替代 UT 判据 |

- `FTL-U` / `FTL-I` 是**本仓自拟**的套件标签，对应主流测试分层的 unit / integration；
  不声称行业标准术语。
- media 侧只保留一个 `e2e/` 子套件：用例共享一次 format 与 `s_media_ready` 状态，拆成
  l2p/md/cp/bbt 多个套件会让「哪条用例负责准备介质」变成跨套件隐式依赖。
- 当前注册表规模：pure 60（mem 11 / md 15 / host 16 / io 18）、media 24、
  negative pure 42、negative media 49（gc 12 / l2p 6 / meta 14 / boot 7 / if 10）、
  `/bench/speed/ftl_rw` 1；合计 **176 条 FTL 用例**。`test_speed.c` 另注册
  `/bench/speed/fc_nand_rw` 1 条，属 FC 基准，不计入 FTL 176。

### 1.3 测试原则

1. **只在目标板跑**（R4）：`make tests UT=FTL` 生成镜像，串口 CLI `test` 执行；不做 host 模拟。
2. **一个用例只钉一个契约**：断言必须能回答「比较的是哪两个值」；没有区分力的断言
   不算覆盖（C7 / C8）。
3. **负例必须能红**：非法状态、越界、池满、注入失败都要有明确判据；注入类还必须给出
   `fi_hits` 增长等「故障确实发生」的证据。
4. **正常路径全绿 ≠ 全覆盖**：不等于持久化 / 掉电窗口 / 多 die 已验证（R1）。
5. **状态如实**：未上板标「未上板」；SKIP 必须打印原因；不复用 H2testw 结果冒充 UT。

---

## 2. 按模块的测试设计

> 每个小节固定三段：**设计要点**（不变量 / 契约 / 错误语义）、**判据与代表用例**、**缺口**。
> 代表用例只用于定位测试意图，不是完整清单；负例列只列该模块的守卫与注入用例。
> `本仓自拟` 的覆盖点 `CP-*` / 不变量 `INV-*` 只在 §3 / §4 定义。

### 2.1 `allocator` + BST — 块账本、池、状态

**设计要点**

- 四本账：Free / OP / Spare / Closed；`state == FREE ⟺ 在 Free Pool`，BAD 不在任何池或
  Closed。任一处漏记就是**永久漏账**，不报错、不崩溃，只表现为容量无声流失。
- Free / OP / Spare 按 plane 独立；gblock 是 plane-major，跨 plane 的块号必须先经
  `ftl_gblock_plane/block` 拆开。
- 开 SB 两 plane 同页号；rewind 不烧块；close 记录 `sb_id`；Closed 按 plane 摘除，
  最后一个 plane 才释放条目。
- `valid_count` 是 GC / WL 的输入，BAD 之后的 sub 必须无效；擦除后 `erase_count + 1`、
  `last_pe_ts` 更新、`valid/read_count` 清零。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| PBA / gblock 编解码 | LBA / PBA / gblock 往返；plane-major 编号不串 plane | `pba_roundtrip` | 跨 LUN 未实现 |
| CW 位图 | CW flat 绝对编号、span / plane 掩码、popcount、`NAND_FIELD_MASK` | `pba_cw_masks` | — |
| Free Pool 不变量 | 每块 `state==FREE` 当且仅当在池；OP refill LIFO；四本账同步 | `alloc_free_pool_invariants`、`media_live` | — |
| SB 开 / 关 / rewind | 两 plane 同页号；rewind 不烧块；close 记录 `sb_id` | `alloc_super_open_close`（mode=hot/cold/high_wear） | — |
| Closed 列表 | 按 plane 摘除；最后一个 plane 才释放条目 | `alloc_closed_list_add_drop` | — |
| `valid_count` | 记账正确；BAD 后继续 sub 无效 | `alloc_valid_count_guard` | — |
| OP / Spare 池 | per-plane 计数独立；Spare LIFO；空池 pop 失败 | `alloc_op_spare_pools`、`alloc_spare_full_refill_none` | — |
| 擦除统计 | `erase_count+1`、`last_pe_ts`、`valid/read_count` 清零、state OPEN | `alloc_erase_stats` | — |

**负例 / 守卫**

`alloc_rewind_underflow`（页游标下溢必须被拒绝）、`alloc_spare_skip_bad`（BAD 块不得进入
Spare）、`alloc_closed_list_full`（Closed 满返回瞬态资源）、`alloc_closed_plane_bad_drop`
（标坏后不得残留 Closed 条目）、`alloc_close_bad_pair_no_entry`（坏 pair 关闭不产生条目）。

**缺口**

- 跨 LUN 条带 / GC / WL 未实现，`RISK-ARCH-02` 只覆盖到 `pba_roundtrip` 的地址编码。
- `INV-ALLOC` 尚未纳入通用 teardown，allocator 用例必须显式断言（见 §4.4）。

### 2.2 `bbt` — 坏块记录、候选池、Spare

**设计要点**

- 坏块记录是 ring；加载必须选**最新** record，不是最早；标坏幂等。
- 元数据块标坏**不弹用户 Spare**，只置 `pools.dirty` / `rebuild_required`。
- 坏块标坏时清 `valid/sb_id`，推进 `bad/dirty_gen`；Spare 从 Free Pool 补池。
- `bbt_persist` 先擦 BBT 池并清 `a->bbt` 环形状态：多圈 ring 的 save→reload 会选旧
  record，clean-pool 路径才能稳定验证持久化。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| 标坏状态 | 置 BAD、清 valid/sb_id、bad/dirty_gen 推进、Spare 补池、幂等 | `alloc_bbt_mark_bad`（plane=0/1）、`bbt_persist` | — |
| 元数据块标坏 | 不弹用户 Spare；置 dirty / rebuild_required | `alloc_bbt_metadata_bad` | — |
| ring 最新记录 | 两条 record 后重启走生产 `bbt_init`，两个坏块都在 | `bbt_ring_reload` | 多圈 ring 的旧 record 选择 `RISK-BBT-18` |
| ring 擦除推进 | 擦失败仍推进，不原地死循环 | `bbt_ring_erase_fail_advances` | — |
| 候选池 / 补池 | Spare 从 Free Pool 补；空池不越界 | `bbt_kick_candidates`、`bbt_spare_refill_from_free` | — |

**负例 / 守卫**

`bbt_metadata_pool_rebuild`（元数据坏块触发 rebuild 而非用户 Spare）、`bbt_write_fail`
（记录写失败有界返回）、`bbt_mark_bad_idempotent`（重复标坏不重复扣账）、
`bbt_ring_reload_latest`（恢复选最新）、`bbt_format_verify_runs`（格式化 verify 路径确实执行）、
`bbt_metadata_empty_user_data_kept`（元数据块为空不得清用户数据）。

**缺口**

- 真空白片需另测；断电发生在 BBT 记录写入中仍靠人工 POR。
- `bbt_persist` / `bbt_ring_reload` 每次全量跑都会永久标坏用户块，`RISK-UT-06` 是已知测试成本。

### 2.3 `metadata` / `metadata_job` — Directory、ring、压缩

**设计要点**

- Directory 是主备双副本；primary 覆盖写要迁移 live 计数；entry / backup 都要可查询。
- Journal 序号推进、pending 置位、未到水位不请求 CP；`write_fail_seq` 只有 CP 水位追上才清除。
- ring locate 要区分「空环」与「找不到最新记录」。
- 压缩写 abort 只回 IDLE，不回收 / 擦除 victim；`l2p_live > 0` 的 victim 不登记回收。
- 连续 3 次擦失败 → `bbt_mark_bad`、清 reclaim、phase 回 IDLE；borrowed 块结算
  `log_borrow_count`。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| idle / 几何 | job 初始态、live block 计数 0；records/block、total、哨兵 | `md_idle`、`md_ring_geometry` | — |
| Directory 提交 | primary 覆盖写 live 计数迁移；PAIR 双副本；entry/backup 查询 | `md_directory_commit` | — |
| Journal append | init/reserve/append 序号推进；pending 置位；水位前不请求 CP | `md_journal_append` | — |
| ring locate | 中段 / 块末 / 空环定位；空环 ≠ 找不到最新 | `md_ring_locate` | — |
| 三步 checkpoint | begin → page/advance → commit；`dirty_gen` 竞态不清脏 | `md_dir_ckpt_three_step` | — |
| 日志块轮转 | 分配 / 释放 / 轮转、页游标归零 | `md_log_alloc_rotate_release` | — |
| 错误状态 | `handle.error` 进 IDLE 被清，采样 `FTL_OK`（特征化） | `md_status_error` | — |
| 写失败清除 | `write_fail_seq` 只有 CP 水位追上才清除 | `md_journal_write_fail_clear` | — |
| abort 保留 victim | 压缩写 abort 不回收 / 擦除 victim | `md_abort_keeps_victim` | 物理不可读 victim 的 fallback 成功路径未注入 |
| 回收擦失败 | 3 次擦失败 → `bbt_mark_bad`；borrowed 结算 | `md_reclaim_erase_fail`（kind=fixed/borrowed） | — |
| CP_READ 失败 | 无备份 → abort；`l2p_live>0` victim 不登记回收 | `md_cp_read_fail_no_reclaim` | — |
| truncate 幂等 | 水位 / 0 保护；环形下一块；重复 plan=ALREADY | `md_journal_truncate_idempotent` | — |
| 编码器 | block word 位段、快照、OP/Spare 标签、磨损页字节布局 | `md_cp_encoders` | — |

**负例 / 守卫**

`md_compact_reserved_budget`（压缩不得吃掉保留预算）、`md_dir_ckpt_dirty_rewrite`
（脏代际不得被旧写覆盖）、`md_compact_superseded_no_commit`（被取代的压缩不提交）、
`md_victim_skips_reserved`（保留块不得当选 victim）、`md_alias_full_rebuild_repro`
（别名满时重建可复现）、`md_backup_copy_corrupt`（备份副本损坏必须拒绝）。

**缺口**

- 物理不可读 victim 的 fallback 成功路径仍未注入（`RISK-MD-16`）。
- metadata 压缩的「save 后、erase 中」掉电窗口靠人工 POR。

### 2.4 `checkpoint` — CP phase、Header、Directory

**设计要点**

- CP 初始 `active/waiting=0`、`phase=IDLE`；T3 是绝对到期时刻重装，不是相对延时。
- begin / page / advance / commit 是四段；重复完成必须幂等，abort 只回 IDLE。
- `cp_media_status_probe` 的四种结论必须区分：CURRENT / NO_RECORD /
  FORMAT_IN_PROGRESS / IO_ERROR；IO_ERROR 不得触发格式化。
- CP Header 带介质布局版本；版本不匹配要拒绝而不是「先擦了再说」。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| 初始化 / 定时器 | `active/waiting=0`、`phase=IDLE`、T3 绝对到期重装 | `md_cp_init_timer` | — |
| 四段推进 | begin → page/advance → commit；`dirty_gen` 竞态不清脏 | `md_dir_ckpt_three_step` | — |
| 介质探针 | `CURRENT`、版本匹配、不误格式化 | `cp_probe_clean_media` | 真空白片 |
| 损坏拒绝 | CP 池非 FF 页 → IO_ERROR、拒绝格式化 | `cp_corrupt_refuse_format` | — |
| 屏障 | flush 后无 pending、CP 序号推进、Header 落盘、重启一致 | `journal_barrier_cp_progress` | — |
| 编码 | Header / 快照 / 磨损页字节布局 | `md_cp_encoders` | — |

**负例 / 守卫**

`cp_duplicate_completion_guard`（重复完成不得二次提交）、`cp_abort_phase`
（abort 只回 IDLE，不回收介质）、`cp_dir_ckpt_abort`（Directory checkpoint 中断安全）。

**缺口**

- 真 POR 在 CP 三个时间点的行为仍是人工流程（`RISK-BOOT-04`）。
- 空白片的 `NO_RECORD` 分支没有全新芯片，只能靠擦 CP 池模拟。

### 2.5 `journal` — append、truncate、replay、WAL

**设计要点**

- Journal 是只追加日志；`init / reserve / append` 的序号推进必须串行，重复序号要拒绝。
- `truncate` 保护水位与 0；环形下一块；重复 plan 返回 `ALREADY`；`commit(NONE)`
  不清预擦块。
- `write_fail_seq` 只有 CP 水位追上才清除；WAL 闸门在 `persisted_seq < dirty_seq` 时
  退让（AGAIN + `wal_defer_count+1`），追上后放行。
- replay 必须绕过 CMT，直接按 Journal 条目重建 L2P；坏条目按不可信输入校验（H29）。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| append 序号 | init/reserve/append 单调；pending 置位；水位前不请求 CP | `md_journal_append` | — |
| truncate 幂等 | 水位 / 0 保护；环形下一块；重复 plan=ALREADY | `md_journal_truncate_idempotent` | — |
| 写失败清除 | `write_fail_seq` 追平才清 | `md_journal_write_fail_clear` | — |
| WAL 闸门 | `persisted_seq < dirty_seq` 退让；追平放行 | `l2p_wal_gate` | — |
| 注入写失败 | 2 次页写失败 → 标记置位、`persisted_seq` 落后；放开后有界收尾、数据可读 | `journal_write_fail_wal_stall` | — |
| 屏障 + 重启 | flush 后 CP 推进、Header 落盘、模拟重启一致 | `journal_barrier_cp_progress`、`flush_reboot_readback` | 真 POR |
| TRIM 持久化 | trim → flush → 强制 CP → 重启不复活；journal-only 段不复活 | `trim_no_resurrection` | 极端时序人工 |

**负例 / 守卫**

`journal_truncate_commit_none`（`commit(NONE)` 不清预擦块）、`jrn_handle_busy_guard`
（句柄忙时拒绝重入）、`journal_seq_serial_compare`（序号比较必须按 `page_seq +
entry_count`，只比页首会 off-by-one）、`jrn_resume_cross_block_erase_fail`
（跨块恢复擦失败有界）、`journal_ring_erase_fail`（环擦失败不原地死循环）、
`flush_reports_journal_error`（Journal 错误必须上报，不得静默成功）。

**缺口**

- 真 POR 在 Journal append 的中间时点仍人工；`JOURNAL_ENTRIES_PER_PAGE` 的坏数据是
 掉电预期情形，刻意保留运行期检查（H29）。

### 2.6 `L2P` / `CMT` — 地址映射、驻留行、写回

**设计要点**

- 同一语义只允许一套坐标：LBA / PBA / gblock 的转换只在单一漏斗发生，现场手写位操作
  是缺陷（C9）。
- 驻留行状态 CLEAN / DIRTY / FLUSHING / LOADING 互斥；批量掩码同源。
- miss → AGAIN → CLEAN + 未映射；该 region 已驻留、无空行或 cache 有脏 / 在飞时 SKIP。
- CMT 同 set 4 路全 DIRTY ⇒ install 失败回退整行；`make_room` 选 LRU dirty victim；
  inflight 饱和只背压不淘汰；set 满时整批回退，不部分插入。
- CMT 写回后 dirty retire 成 CLEAN（若驻留）；TRIM 负条目丢弃。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| 驻留状态 | CLEAN/DIRTY/FLUSHING/LOADING 语义；批量掩码同源 | `l2p_resident_states` | — |
| miss / evict | miss→AGAIN→CLEAN+未映射；写后读回 | `l2p_miss_unmapped_evict` | 已驻留 / 无空行 / dirty 时 SKIP |
| CMT 安装 / 读 | dirty 安装、读命中、flush 前合并、写完 retire | `l2p_cmt_write_readback`、`cache_cmt_writeback_retire` | — |
| CMT 容量 | 4 路全 DIRTY ⇒ 回退；LRU victim；inflight 只背压 | `l2p_cmt_capacity_evict` | — |
| 多 region 合并 | flush 只合并目标 region 的 dirty CMT | `l2p_cmt_multi_region_merge` | — |
| 整批回退 | set 满时整批回退，不部分插入 | `l2p_cmt_entries_fallback` | — |
| 行失效 | 只清 CLEAN，保留 DIRTY | `l2p_cmt_line_invalidate` | — |
| WAL 闸门 | `persisted_seq < dirty_seq` 退让；追平放行 | `l2p_wal_gate`、`journal_write_fail_wal_stall` | — |
| 跨 region 读写 | 起点 504、长度 16 跨 512 LBA 边界 | `cross_region_roundtrip` | — |
| region 0 / 高位 | 两区预载不别名 | `preload_region_zero_and_high` | — |
| TRIM / 重启 | trim → flush → CP → 重启不复活 | `trim_no_resurrection` | — |
| flush 回读 | 写 → flush → 模拟重启 → 数据来自介质 | `flush_reboot_readback` | 真 POR |

**负例 / 守卫**

`l2p_thrash_does_not_gate_flush`（thrashing 不得阻断 flush）、
`l2p_cmt_capacity_precheck`（容量预检失败不得部分安装）、
`l2p_replay_bypass_cmt`（replay 绕 CMT）、`l2p_fail_page_undo_precise`
（失败页只回滚自己的条目）、`l2p_cmt_dirty_double_release`（dirty 不得双重释放）、
`l2p_invalid_dir_pba`（非法 Directory PBA 必须拒绝）、`l2p_orphan_dirty_bit`
（孤儿 dirty 位不得泄漏）、`l2p_load_fail_bounded`（load 失败有界返回）、
`l2p_read_error_degrades`（读错误降级不误映射）、`l2p_cmt_read_priority`
（CMT 读优先于介质读）、`l2p_idle_drain`（idle 排空不得丢脏行）、
`l2p_flush_backpressure`（flush 背压返回 AGAIN）、`l2p_pool_starve_flush`
（池饿死时 flush 有界失败）、`blank_ff_trim_rewrite`（空 FF 页 TRIM 后重写）。

**缺口**

- 跨 LUN 条带映射未实现。
- 真 POR 在 CMT 写回中间时点靠人工。

### 2.7 `pending` / group — 请求槽、组完成、确认停车

**设计要点**

- 槽按 `command_id` 查找 / 移除；满返回 NULL，移除后可再分配。
- 组槽满返回 0，已有组字段不被覆盖；两个子操作 outstanding 2→0 只回调一次并释放槽。
- 子操作 IO_ERROR 后 `agg_status` 粘滞，后续 OK 不清错。
- 待入账判别只看 `WRITE_CONFIRM`；普通 used 写槽为 false。超时 `settle` 释放槽并按
  IO_ERROR 结算一次。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| 初始基线 | pending / group / ctx 全 0；wbuf idle；bg 表长度正确 | `host_idle` | — |
| 槽分配 | 满返回 NULL；按 `command_id` 查找 / 移除；移除后可再分配 | `pend_slots` | — |
| 组槽满 | 返回 0；已有组字段不被覆盖 | `pend_group_exhaustion` | — |
| 组完成一次 | outstanding 2→0，只回调一次并释放组槽 | `pend_group_complete_once` | — |
| 错误聚合 | IO_ERROR 后 `agg_status` 粘滞 | `pend_group_failure_aggregation` | — |
| 确认停车超时 | `pend_confirm_is_expired` 到点；`settle` 释放并按 IO_ERROR 结算一次 | `pend_confirm_park_timeout` | 现场根因需板端日志 |
| 停车判别 | 只看 `WRITE_CONFIRM`；普通 used 写槽 false | `pend_confirm_is_parked` | — |
| 入口参数 | count=0 / buffer=NULL / LBA 越界 / ftl=NULL 在分配池资源前拒绝 | `host_submit_invalid_args` | — |

**负例 / 守卫**

`sync_timeout_late_completion`（超时后迟到完成不得二次结算）、`pend_table_guard`
（旁表越界必须拒绝）、`host_group_exhaustion`（host 组槽耗尽返回 AGAIN 不覆盖）。

**缺口**

- `pend_confirm_park_timeout` 的现场根因仍需板端日志（`RISK-HOST-05`）。

### 2.8 `wbuf` — 页打包窗口

**设计要点**

- 单 CW append / read；同窗口稀疏两位；owners 与在飞计数必须同步。
- DROP 清位，最后一个 CW 按 OK 结算；RESET 按 IO_ERROR 结算。
- `occupied == (1 << n) - 1` 是密度判据；组在飞只 +1；逐 CW 读回一致。
- 窗口是平面位图，CW 编号是 flat 绝对编号（plane 1 的 CW0 是 bit 4）。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| append / read | 单 CW 往返；同窗口稀疏两位；owners / 在飞计数 | `wbuf_append_read` | — |
| DROP / RESET | DROP 清位、最后 CW 按 OK；RESET 按 IO_ERROR | `wbuf_drop_reset_owners` | — |
| 密度 | `occupied==(1<<n)-1`；组在飞只 +1；逐 CW 读回 | `wbuf_density`（n=1/2/4/8） | — |
| 跨 pair 写读 | 8 是 pair 边界、9 是跨 pair 非对齐、7 是奇尾 | `host_io_matrix` | — |

**负例 / 守卫**

`cache_pad_no_free_aborts`、`cache_pad_skips_proven`、`cache_pad_early_exit_releases`、
`cache_release_sees_inflight`（见 §2.14 的 cache 守卫；wbuf 与 cache 共用窗口/在飞语义）。

**缺口**

- 密度参数只到 8 CW；更大窗口没有生产配置。

### 2.9 `ftl_op` — 请求表、路由、ctx

**设计要点**

- request 表 add / remove 计数必须守恒；旁表容量有限，同 `cmd_id` 覆盖不追加。
- `(ch, ce, lun)` 与 region 路由必须取回同一实例；用例结束恢复原表。
- route 是跨模块惰性绑定，不是第二套坐标；地址转换只在单一漏斗发生。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| request 表 | add / remove 计数守恒 | `op_ctx` | — |
| 旁表容量 | 容量边界；同 `cmd_id` 覆盖不追加 | `op_ctx_capacity` | — |
| route | `(ch,ce,lun)` 与 region 路由取回同一实例；结束恢复 | `op_route` | 多 die 组合未覆盖 |
| 入口参数 | 非法 host 输入在分配池资源前拒绝 | `host_submit_invalid_args` | — |

**负例 / 守卫**

`op_req_table_guard`（request 表越界 / 重复删除必须拒绝）、
`iface_pair_cmd_guards`（成对命令参数守卫）、`sync_timeout_late_completion`
（迟完成不得二次挂接）。

**缺口**

- 跨 die 路由组合受单 target 板限制（`RISK-CR-02`）。

### 2.10 `bg_scheduler` — 后台任务与门控

**设计要点**

- 任务表长度 = `BG_TASK_COUNT`；`level_select` 返回合法档位。
- pending 0→IDLE、8→NORMAL、17→BUSY；used group 保守升 BUSY。
- 门控必须单一配置源；host busy 与后台任务不得互相饿死。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| 任务表 / 等级 | 表长 = `BG_TASK_COUNT`；`level_select` 合法 | `bg_engine` | — |
| 等级边界 | pending 0→IDLE、8→NORMAL、17→BUSY；used group 升 BUSY | `bg_level_select` | — |
| 空盘基线 | GC/WL 入口无副作用；空盘不误触发 | `gc_policy_entry` | — |

**负例 / 守卫**

`bg_busy_cfg_single_source`（busy 阈值只能有一个配置源）、`seq_gate_starve`
（序列门控不得饿死前台）。

**缺口**

- 慢路径饿死仍是调度判据盲区，需要带 host 负载的板端观测。

### 2.11 `migrate` / `GC` — victim、搬移、擦除

**设计要点**

- GC 触发：空池 EMERGENCY；TARGET 以上不触发；总水位达标但单 plane 低仍 WATERMARK。
- victim 选择按 benefit 模式选有效数据更少者；饥饿 plane 优先成对 / 单条目。
- 搬移必须逐 CW 确认；读失败 / 写失败 / 擦失败都要有界封顶，不能无限重试。
- 有效 CW 解码有 OK / EMPTY / CORRUPT / 空洞四态；corrupt 不得当有效数据搬走。
- `mig_eval` 读 SysTick 而非 TIM6 快照；伪时钟 12345 下 `write_last_ms=12345`。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| 入口 / 空盘 | GC/WL 入口无副作用；空盘基线 | `gc_policy_entry` | — |
| 伪时钟 | 写计数变化 ⇒ `write_last_ms=12345` | `mig_eval_fake_clock` | — |
| 触发水位 | 空池 EMERGENCY；TARGET 以上不触发；单 plane 低仍 WATERMARK | `gc_trigger` | — |
| victim | 三种 benefit 模式选有效数据更少者；饥饿 plane 优先 | `gc_select_victim`（off/idle/emergency） | NO_SPACE 假报需 host 负载 |
| CW 解码 | OK / EMPTY / CORRUPT / 空洞四态 | `gc_decode_cw_lba` | — |
| 搬移进度 | 构造唯一有效 CW victim → urgent GC → moved 增长、源 LBA 可读 | `gc_progress` | — |
| `valid_count` 联动 | GC 搬移后计数守恒 | `alloc_valid_count_guard` | — |

**负例 / 守卫**

pure：`gc_empty_victim_journal_gate`（空 victim 不占 Journal 闸门）、
`mig_inflight_blocks_new_job`（在飞搬移阻止新 job）、`mig_block_sub_valid_no_wrap`
（块内有效计数不绕回）。
media：`alloc_fail_bounded`、`gc_read_fail_bounded`、`gc_write_fail_bounded`、
`gc_erase_fail_bounded`（四类失败均有界返回）、`gc_read_unc_marks_bad`
（不可纠读标坏）、`mig_read_success_keeps_fail_count`（读成功不清失败计数）、
`host_write_plane_fail`（单 plane 写失败不拖垮另一 plane）、
`gc_batch_program_fail`（批量编程失败封顶）、`gc_l2p_load_wait_timeout`
（L2P load 等待超时）、`gc_partial_plane_erase_abort`（部分 plane 擦除中止）、
`gc_systemic_read_fuse`（系统性读失败熔断）、`gc_write_complete_fail_closes_cold`
（写完成失败关闭 cold 块）。

**缺口**

- 真 NAND 的坏块注入会永久消耗块，跑完必须 format + POR。
- 跨 LUN GC 未实现。

### 2.12 `wear_leveling` — UBI 判据、victim、目的块

**设计要点**

- 触发判据是 UBI 模型：`max_free_ec − min_used_ec ≥ 200`；victim 取最低擦除 used SB。
- 低有效 CW 跳过；有效比例 ≥70% 接受。
- 目的块准备失败不得留下半搬移状态；坏 plane 必须跳过。
- WL 擦除成功要清零 read_count（读计数属于块生命周期，不是永久统计）。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| 触发 / victim | UBI 判据；victim 取最低擦除 used SB | `wl_trigger_and_victim` | — |
| 有效比例 | 低有效 CW 跳过；≥70% 接受 | `wl_victim_threshold` | — |
| 搬移进度 | 构造 UBI 擦除差 → WL 进入 `MIG_KIND_WL`、moved 增长、数据可读 | `wl_progress` | — |

**负例 / 守卫**

`wl_erase_resets_read_count`（擦除后 read_count 清零）、`wl_skip_bad_plane`
（坏 plane 跳过）、`wl_destination_prepare_fail`（目的块准备失败有界）、
`wl_snapshot_entry_trigger`（快照条目触发 WL 但不得越界）。

**缺口**

- UBI 阈值只在边界值附近验证，未做全擦除分布扫描。

### 2.13 `read_recovery` — 健康等级、预测退役

**设计要点**

- 健康等级 HEALTHY → WARNING → CRITICAL → DYING 单调不回落。
- ECC 达 `RR_ECC_MAX` 命中预测退役；退役清健康计数。
- 读失败闸门必须忽略 hosted read，不能把 host 读的重试当成介质恶化。
- 坏 plane 的预测退役必须跳过，不得误退役。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| 健康等级 | 单调不回落 | `rr_health` | — |
| 预测退役 | ECC 达 `RR_ECC_MAX` 命中；退役清健康计数 | `rr_predictive_retire` | — |
| 读失败闸门 | hosted read 不触发退化 | `rr_gate_ignores_hosted_read` | — |
| ECC 单调 | ECC 计数只增不减 | `rr_ecc_monotonic` | — |
| 坏 plane | 预测退役跳过坏 plane | `rr_predictive_skip_bad_plane` | — |
| media 读失败 | 读失败有界；混合 plane 失败隔离 | `rr_read_fail_bounded`、`rr_mixed_plane_fail` | — |

**负例 / 守卫**

`rr_gate_ignores_hosted_read`（hosted read 不触发退化）、`rr_ecc_monotonic`
（ECC 计数不回落）、`rr_predictive_skip_bad_plane`（预测退役跳过坏 plane）、
`rr_read_fail_bounded`（读失败有界）、`rr_mixed_plane_fail`（混合 plane 失败隔离）。

**缺口**

- `RR_FEATURE_READ_RETRY = 0x89` 在本器件是保留地址，路径自始空转；用户明确不处理。

### 2.14 `cache_batch` / cache — FAILC、早回执、序列

**设计要点**

- PROGRAMMED 返回真实 PBA；CONFIRMED / FAILED 不返回；FAILED 由 covers 判 IO_ERROR。
- FAILC 结算上一页：已确认前缀保留、失败页 LBA 读 IO_ERROR、`wb_aborts+1`、
  同批未确认页保留旧映射。
- 早回执：写新数据不 flush 立刻读回新数据；flush + 重启仍一致。
- flush 完成前 `wr.count>0` 阻止完成；完成后 `wr.count==0`、L2P 指向新 PBA。
- 连续写 1024 LBA（2 物理 block）分段读回一致；`read_pairs` 覆盖整段、`fallbacks` 不增长。
- CMT 写回后 dirty retire 成 CLEAN；TRIM 负条目丢弃。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| 写查询 | PROGRAMMED 返回真实 PBA；CONFIRMED/FAILED 不返回 | `cache_wr_lookup` | — |
| FAILC 结算 | 已确认前缀保留；失败页 IO_ERROR；`wb_aborts+1`；未确认页保留旧映射 | `cache_failc_abort`（arm=1/2/last） | — |
| 早回执读可见 | 写后不 flush 立刻读新数据；flush+重启一致 | `cache_early_ack_read_visible` | — |
| FAILC 撤销 | 下一 host 命令报一次 deferred error；失败页/同 region 未映射 LBA 读 IO_ERROR | `cache_failc_undo_read_error` | — |
| flush 屏障 | 完成前 `wr.count>0` 阻止；完成后 =0、L2P 新 PBA、重启一致 | `cache_early_ack_flush_barrier` | — |
| 跨 block 序列 | 1024 LBA 跨 2 物理 block；`read_pairs` 覆盖、`fallbacks` 不增长 | `cache_seq_cross_block` | — |
| CMT 写回 | dirty retire 成 CLEAN；TRIM 负条目丢弃 | `cache_cmt_writeback_retire` | — |

**负例 / 守卫**

`cache_pad_no_free_aborts`（无空闲页时 padding 中止）、`cache_pad_skips_proven`
（已证明页跳过）、`cache_pad_early_exit_releases`（早退必须释放资源）、
`cache_release_sees_inflight`（释放必须看见在飞）、`gc_batch_program_fail`
（批量编程失败封顶，见 §2.11）。

**缺口**

- 序列读的 8 LBA 驻留 lookahead 只覆盖正常路径；放弃序列的重试窗口靠板端日志。
- 真 POR 在 FAILC 结算中间时点仍人工。

### 2.15 `ftl.c` / `ftl_fc_interface` — 对外接口与生命周期

**设计要点**

- host 入口返回码：`FTL_ERROR` 永久失败、`FTL_AGAIN` 瞬态资源；资源空 / 满一律 AGAIN。
- `ftl_submit_*` 在分配池资源前校验 count / buffer / LBA；非法参数不改任何账本。
- 同步等待必须处理迟到完成与超时；不能永久悬挂。
- `format_and_roundtrip` 是端到端最小闭环：format → 8 LBA 写 → flush → 读回逐字节一致。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| host 参数 | count=0 / buffer=NULL / LBA 越界在分配资源前拒绝 | `host_submit_invalid_args`、`iface_sparse_page` | — |
| 端到端写读 | format → 写 → flush → 读回逐字节一致 | `format_and_roundtrip` | — |
| 参数矩阵 | count=1/7/8/9/64；8 是 pair 边界、9 跨 pair、7 奇尾；起点按种子随机 | `host_io_matrix` | — |
| 随机构造 | 固定种子 8 轮随机 LBA/长度写读，每轮 flush 后读回 | `host_io_random` | — |
| 活实例基线 | free/op/spare/bad 账本与池/组/ctx 基线 | `media_live` | — |
| 基准 | 100 gblock（51,200 LBA = 25 MiB）host 读写；只判跑完 + 错误计数 | `ftl_rw`（chunk=8/16/32） | — |

**负例 / 守卫**

`iface_pair_cmd_guards`（成对命令参数守卫）、`read_column_fail_seam`
（列读失败必须上报）、`data_read_fail_bounded` / `data_write_fail_bounded`
（数据面失败有界）、`rr_read_fail_bounded`（RR 读失败有界）、`fc_status_nand_fail`
（NAND 状态失败透传）、`seq_gate_starve`（序列门控不饿死前台）、
`host_read_multiplane_stats`（多 plane 读统计正确）、`host_group_exhaustion`
（host 组槽耗尽返回 AGAIN）。

**缺口**

- USB / HIL 槽状态机、多 run 拆分不在 FTL UT（`ISS-HIL-01` / `ISS-HIL-03`）。
- 跨 LUN 生命周期未实现。

### 2.16 `power_failure_recovery` — 启动恢复与对账

**设计要点**

- 启动先做 CP 探针；`IO_ERROR` 必须拒绝格式化，`NO_RECORD` / `FORMAT_IN_PROGRESS`
  才允许继续或格式化。
- 恢复顺序：CP → Directory → Journal replay → L2P 重建 → BBT 对账；任何一步失败要有
  明确降级，不能静默格式化。
- `boot_page0_hole_counts_full_block`：page 0 空洞必须按整块计数，不能按页跳过。
- `boot_restore_reconcile`：写 + flush → 记录 free/OP/Spare/Closed/open + 借用的用户
  日志块合计 → 重启后守恒、抽样读回一致。

| 设计点 | 判据 | 代表用例 | 缺口 |
|---|---|---|---|
| CP 探针 | `CURRENT` 不误格式化；IO_ERROR 拒绝格式化 | `cp_probe_clean_media`、`cp_corrupt_refuse_format` | 真空白片 |
| 格式化中恢复 | FORMAT_IN_PROGRESS record + 擦 CP 池 → 重启判 NO_RECORD/继续 | `format_in_progress_resume` | erase 中 / marker 前掉电人工 |
| BBT 恢复 | 两条 record 后重启走生产 `bbt_init`，载入最新 | `bbt_ring_reload` | `RISK-BBT-18` |
| 账本守恒 | free/OP/Spare/Closed/open + 借用日志块合计重启后守恒 | `boot_restore_reconcile` | 真掉电人工 |
| 页面空洞 | page 0 空洞按整块计数 | `boot_page0_hole_counts_full_block` | — |

**负例 / 守卫**

`boot_cp_read_fail_refuses_format`（CP 读失败拒绝格式化）、`boot_page0_hole_counts_full_block`
（空洞整块计数）、`boot_md_load_fail_refuses`（metadata load 失败拒绝启动）、
`boot_corrupt_cp_restore`（损坏 CP 走恢复而非静默格式化）、`boot_no_rebuild_loop`
（重建不得死循环）、`boot_degraded_reload`（降级重载可继续）、
`boot_read_fail_policy`（读失败策略有界）。

**缺口**

- 真 POR 的三个掉电时间点（CP before/after、Journal append 中）仍是人工流程
  （`RISK-BOOT-04`）。
- 全新空白片的 `NO_RECORD` 分支没有芯片可测。

### 2.17 `ftl_ut_common` — 夹具、模拟重启、清理

**设计要点**

- `ftl_ut_live()` 返回 `g_ctrl.ftl[0]`（die 0）；`ftl_ctx_s` 按值复制不现实。
- pure 用例不 reset 活实例的 allocator/metadata；使用测试自有对象。
- `ftl_ut_reboot_from_media` 只标「模拟重启」，不标 POR：quiesce → `fc_init` +
  `fc_nand_engine_set_armed(1)` → `ftl_op_init` + route → 保存句柄 → 重建 storage →
  `ftl_init` + `l2p_init` → `pfr_boot_recovery`。
- teardown 的 `INV-PEND` / `INV-FC` / `INV-MD` / `INV-CACHE` 必须归零；`INV-ALLOC`
  尚未纳入通用 teardown，allocator 用例显式断言。

| 设计点 | 判据 | 代表夹具 | 缺口 |
|---|---|---|---|
| 活实例 | `g_ctrl.ftl[0]`；拓扑按 `g_nand_targets` 重建 | `ftl_ut_live()`、`ftl_ut_topology_init()` | — |
| 测试 RAM | 结构体与生产同型；不引入第二套几何；链接溢出即门禁 | `s_ut_blocks`、`s_ut_free`、`s_ut_closed`、`s_ut_md` | — |
| 伪时钟 | 保存 / 恢复平台 ops；真 NAND I/O 用真实时钟 | `ftl_ut_clock_install/restore` | — |
| 结束不变量 | 池 / 组 / ctx / FC / metadata 全 idle | `ftl_ut_assert_clean()` | `INV-ALLOC` 未通用化 |
| 模拟重启 | 五步顺序缺一步即板端断言 / 日志风暴 | `ftl_ut_reboot_from_media()` | 真 POR 仍人工 |
| 介质准备 | 首次 `media_prepare()` 做一次 format；后续共享 | `media_prepare()` | — |

**负例 / 守卫**

夹具没有独立负例；它由 teardown 的 `INV-*` 与 `media_prepare()` 的幂等共用状态守护。

**缺口**

- 实时钟 / 真掉电不可在 munit 内模拟；`INV-ALLOC` 归零仍按用例显式断言。

---

## 3. 跨模块 / 系统级场景

模块内用例只证明单点契约；下面这些场景跨多个模块，必须单独设计。

### 3.1 写读与映射

| 场景 | 涉及模块 | 判据 | 代表用例 |
|---|---|---|---|
| 最小闭环 | `ftl.c` + L2P + wbuf + FC | format → 写 → flush → 读回逐字节一致 | `format_and_roundtrip` |
| 跨 region | L2P + wbuf + FC | 起点 504、长度 16 跨 512 LBA 边界 | `cross_region_roundtrip` |
| pair 边界 | wbuf + L2P + FC | 8 是 pair 边界、9 跨 pair 非对齐、7 奇尾 | `host_io_matrix` |
| 随机 | L2P + wbuf + FC | 固定种子 8 轮随机 LBA / 长度；每轮 flush 后读回 | `host_io_random` |
| 覆盖写 | L2P + CMT + wbuf | 写后读、覆盖写、TRIM 后不复活 | `host_io_matrix`、`trim_no_resurrection` |

### 3.2 持久化与重启

| 场景 | 涉及模块 | 判据 | 代表用例 |
|---|---|---|---|
| flush 回读 | journal + checkpoint + L2P | 写 → flush → 模拟重启 → 数据来自介质 | `flush_reboot_readback` |
| CP 屏障 | journal + checkpoint + L2P | flush 后无 pending、CP 序号推进、Header 落盘、重启一致 | `journal_barrier_cp_progress` |
| TRIM 持久化 | journal + L2P + CP | trim → flush → 强制 CP → 重启不复活；journal-only 段不复活 | `trim_no_resurrection` |
| 账本守恒 | allocator + metadata + PFR | free/OP/Spare/Closed/open + 借用日志块合计重启后守恒 | `boot_restore_reconcile` |
| BBT 持久化 | BBT + PFR | 标坏 → save → 模拟重启 → 坏块仍在 | `bbt_persist`、`bbt_ring_reload` |
| 格式化恢复 | CP + PFR | FORMAT_IN_PROGRESS record + 擦 CP 池 → 重启判 NO_RECORD/继续 | `format_in_progress_resume` |
| CP 损坏 | CP + PFR | CP 池非 FF → 探针 IO_ERROR、拒绝格式化 | `cp_corrupt_refuse_format` |

### 3.3 GC / WL / RR

| 场景 | 涉及模块 | 判据 | 代表用例 |
|---|---|---|---|
| GC victim | allocator + migrate + L2P | 构造唯一有效 CW victim → urgent GC → moved 增长、源 LBA 可读 | `gc_progress` |
| GC 失败封顶 | migrate + L2P + BBT | 读 / 写 / 擦失败均有界；UNC 标坏 | `alloc_fail_bounded`、`gc_read_fail_bounded`、`gc_write_fail_bounded`、`gc_erase_fail_bounded`、`gc_read_unc_marks_bad` |
| WL 搬移 | allocator + WL + L2P | UBI 擦除差 → WL 进入 `MIG_KIND_WL`、moved 增长、数据可读 | `wl_progress` |
| RR 退役 | RR + BBT + L2P | ECC 达阈值命中；退役跳过坏 plane | `rr_predictive_retire`、`rr_predictive_skip_bad_plane` |

### 3.4 cache / host 路径

| 场景 | 涉及模块 | 判据 | 代表用例 |
|---|---|---|---|
| FAILC | cache + wbuf + L2P | 已确认前缀保留、失败页 IO_ERROR、`wb_aborts+1`、未确认页旧映射 | `cache_failc_abort` |
| 早回执 | cache + L2P + journal | 写后不 flush 立刻读新数据；flush+重启一致 | `cache_early_ack_read_visible` |
| 撤销 | cache + pending + L2P | 下一 host 命令报一次 deferred error；失败页/同 region 未映射 LBA 读 IO_ERROR | `cache_failc_undo_read_error` |
| flush 屏障 | cache + wbuf + L2P | 完成前 `wr.count>0` 阻止；完成后 =0、L2P 新 PBA | `cache_early_ack_flush_barrier` |
| 跨 block 序列 | cache + wbuf + FC | 1024 LBA 跨 2 物理 block；`read_pairs` 覆盖、`fallbacks` 不增长 | `cache_seq_cross_block` |
| CMT 写回 | CMT + L2P + cache | dirty retire 成 CLEAN；TRIM 负条目丢弃 | `cache_cmt_writeback_retire` |

### 3.5 负例套件映射与判定

`/ftl/negative/` 是独立测试层，不是「pure 套件的补充」。判定用 M/N/K 三档，完整判定见
§7.2 台账；本节只给本仓套件与模块映射。

| 套件 | 注册数 | 主要模块 | 判定入口 |
|---|---:|---|---|
| `/ftl/negative/pure/` | 42 | allocator / BBT / metadata / journal / L2P / cache / GC / WL / RR / iface | §7.2 |
| `/ftl/negative/media/gc/` | 12 | GC / migrate / allocator | 同上 |
| `/ftl/negative/media/l2p/` | 6 | L2P / CMT / flush | 同上 |
| `/ftl/negative/media/meta/` | 14 | metadata / journal / BBT / checkpoint | 同上 |
| `/ftl/negative/media/boot/` | 7 | PFR / CP / metadata | 同上 |
| `/ftl/negative/media/if/` | 10 | ftl.c / interface / RR / cache | 同上 |

**判定词**：`M` = 有能变红的负例且上板通过；`N` = seam / 触发 / 断言已从源码确认、
只差上板；`K` = 当前不可构造（真 POR / 真空白片 / 多 die）。状态变化只改
`known_open_items.md`，判定变化只改 §7.2。

### 3.6 覆盖模型（CP-*）

| 覆盖点 | 要覆盖什么 | 典型用例 |
|---|---|---|
| `CP-STATE` | allocator / pending / group / cache stream / L2P line / CP phase 的状态与转移 | `alloc_free_pool_invariants`、`l2p_resident_states` |
| `CP-BOUND` | 池满/空、队列满、ring 回绕、region/block/page-pair 边界 | `pend_group_exhaustion`、`wbuf_density` |
| `CP-ERR` | `FTL_ERROR` / `FTL_AGAIN` / deferred error / 注入失败 | `journal_write_fail_wal_stall` |
| `CP-LEDGER` | allocator 四本账、pending/group/ctx、FC 资源、cache/CMT 计数 | `alloc_free_pool_invariants`、teardown |
| `CP-MAP` | 写后读、覆盖写、TRIM、GC 搬移、重启回放 | `host_io_matrix`、`trim_no_resurrection` |
| `CP-DUR` | flush 屏障、WAL 闸门、CP、Journal 回放、TRIM 持久化 | `journal_barrier_cp_progress` |
| `CP-CONC` | 单飞行、队列满、host busy、cache batch 在飞 | `cache_early_ack_flush_barrier`、`cache_failc_abort` |

> `CP-*` 是**本仓自拟**的功能覆盖点，不是代码覆盖率；裸机目标上不以行覆盖作为主指标。

---

## 4. 测试技术、夹具与 seam

### 4.1 技术矩阵

| 技术 | 判据 | 落点 |
|---|---|---|
| **Directed / contract** | 一个不变量或契约一条用例 | `alloc_free_pool_invariants`、`l2p_wal_gate` |
| **Negative / guard** | 非法状态 / 越界 / 错误码必须被拒绝或降级 | `/ftl/negative/`、`host_submit_invalid_args` |
| **Boundary / parameterized** | 边界值成组覆盖，不写死单点；同一断言不同输入迭代 | `host_io_matrix`、`wbuf_density`、`cache_failc_abort` |
| **Seeded random** | 记录 seed；失败可按 seed 复现 | `host_io_random`、`/bench/speed/ftl_rw` |
| **Fault injection** | 注入点 → 预期降级/恢复 → 清除后可恢复；`fi_hits` 可证 | `fi_cache_fail_arm`、`fi_journal_fail_arm` |
| **Reboot / crash point** | 模拟重启或真 POR 后映射 / 账本一致 | `*_reboot_*`、`trim_no_resurrection` |
| **Stress / perf** | 只作基准，不作功能通过率 | `/bench/speed/ftl_rw` |

> 随机用例必须把 seed 写进证据行；没有 seed 的「随机跑过」不算可复现判据。
> 压力/性能不进功能通过率：`/bench/speed/ftl_rw` 只判「跑不跑得完」，写返回 `IO_ERROR`
> 与读数据不符只计入 `err(wr/rd)` 上报。

### 4.2 活实例与测试 RAM

- `ftl_ut_live()` 返回 `g_ctrl.ftl[0]`（die 0；`extern ssd_ctl_s g_ctrl`）。
  `ftl_ctx_s` 是大型静态对象，按值复制不现实。
- 每个套件 setup 调 `ftl_ut_topology_init()`：用 `g_nand_targets` 重建 die 表，UT 不依赖
  主 boot 留下的表顺序；route / reboot 也按 die 索引。
- pure 用例**不 reset 活实例的 allocator/metadata**；使用测试自有对象。
- 需要活实例 L2P 行的用例只借少量行，teardown 原样恢复。

| 对象（`ftl_ut_common.c`） | 放置 | 用法 |
|---|---|---|
| `s_ut_blocks[FTL_BLOCK_TOTAL]` | SRAM12 | 只用前 `FTL_UT_BLOCKS`（8）个 gblock，其余 NONE |
| `s_ut_free[FTL_BLOCK_TOTAL]` | SRAM12 | 同上 |
| `s_ut_closed[SUPER_BLOCK_LIST_MAX]` | SRAM12 | 按用例填少量 closed 条目 |
| `s_ut_op[PLANE_NUM]` / `s_ut_spare[PLANE_NUM]` | SRAM12 | 按用例填少量 per-plane 条目 |
| `s_ut_md`（`md_dir_cache_s`） | AXI_NOCACHE | 按用例填少量 region |
| `s_ut_alloc`（`alc_s`） | 静态 `.bss` | 指向上面各表 |

若链接放不下：退回借用活实例的表 + 小状态，不缩减数组尺寸（结构体必须与生产同型）。
链接期区域溢出即为硬门禁。

### 4.3 伪时钟

- 复用 `ssd_platform_register` / `ssd_platform_ops_s.time_us`；`ftl_ut_clock_install()`
  保存原 ops，`ftl_ut_clock_restore()` 恢复。
- 仅需要固定 now 的用例安装（当前为 `md_cp_init_timer`、`mig_eval_fake_clock`）；用例结束
  显式调用 `ftl_ut_clock_restore()`，不要依赖 teardown。真 NAND I/O 一律用真实时钟。

### 4.4 结束不变量（INV-*）

每个用例 teardown 至少检查（`ftl_ut_assert_clean`）：

- `INV-PEND`：`pend_slot_get_count()==0`、`pend_group_get_count()==0`、
  `ftl_op_req_count()==0`；
- `INV-FC`：FC 资源 `sq/hsq/cq/command/descriptor/buffer` 全 0；
- `INV-MD`：migrate/checkpoint/metadata/journal/wbuf/L2P 均 idle；
- `INV-CACHE`：`cache_stream` 序列计数归零，CMT 无残留 dirty。

`INV-ALLOC`（`state==FREE ⟺ 在 Free Pool`、BAD 不在任何池/Closed、计数不越界）由
allocator 用例显式断言（`alloc_free_pool_invariants` 等），**尚未**纳入通用 teardown；
新增 allocator 用例时不要假设 teardown 已代查。

> `INV-*` 是**本仓自拟**的不变量编号；用于把「结束条件」写成可引用的契约，不声称
> 行业标准术语。新增/修改 teardown 时同步本节。

### 4.5 真 NAND 安全模型

- 破坏性：`media_prepare()` 首次调用做一次完整 format（`pfr_media_format(ftl, 0)`，
  不 verify）；`gc_progress` / `format_in_progress_resume` / `cp_corrupt_refuse_format`
  还会各自 format（清场或恢复介质）。跑完人工 `format` + POR。
- LBA 窗口：默认使用容量尾部 8192 个 LBA，按 8 LBA 对齐；固定偏移用例各用不同区段，
  随机用例可在窗口内重叠（media 用例共享同一介质，不保证全局互不重叠）。
  `preload_region_zero_and_high` 显式写 LBA 0 以测 region 0 预载，不在此窗口内。
- 运行前必须拔掉 USB 并确认没有 host 在飞请求。⚠ `test` 命令**不**替用例检查
  `cli_host_busy()` / `usb_msc_host_busy()`，由操作者保证；写/读期间的 host 请求会
  与 UT 抢同一个 FTL/FC 实例。
- 不恢复 host 数据；结束后打印提示。

### 4.6 模拟重启夹具（`ftl_ut_reboot_from_media`）

顺序是承重的，缺一步就在板端以断言/日志风暴的形式暴露：

1. `ftl_ut_quiesce()`：泵到 FC 池、CQ、`ftl_op_req_count()` 全 0（否则旧 CP 的迟到完成
   撞 `checkpoint.c` 的 `h->waiting != 0`）；
2. `fc_init()` 后必须 `fc_nand_engine_set_armed(1U)`（`fc_init` 会清派发放行闩锁）；
3. `ftl_op_init()` + 恢复实例路由表（`ftl_op_route_set`）；
4. 先保存 cache/metadata/journal/CP/migrate/wbuf/rr/table_refs 句柄（`ftl_init` 会
   memset 整个上下文），重建 `ftl_storage_s`，再 `ftl_init` + `l2p_init`；
5. `pfr_boot_recovery(ftl, &result)`。

只标「模拟重启」，**不标 POR**（真掉电仍是人工流程）。

### 4.7 media 用例顺序与状态依赖

- `media_prepare()` 只在第一次调用时执行 `ftl_ut_media_format()`；后续用例共享已格式化的介质。
- 写介质的重用例（`host_io_matrix` / `host_io_random`）排在 TRIM 用例**之后**：
  Journal 压力下 TRIM 的持久化时序有问题（`RISK-HOST-11`），先跑 TRIM、再压大量重写
  是其当前缓解；不要为了「参数矩阵在前」调换二者顺序。
- `trim_no_resurrection` 必须 `trim → flush → 强制 CP → 模拟重启`：TRIM 只记 Journal，
  不 flush 就重启会「复活」（`RISK-HOST-08` / `RISK-HOST-11`）。等待 CP 时要等
  `phase == CP_IDLE`，CP 基准要在**用例起点**取（写入/trim 之前），并在调用
  `cp_pool_find_latest` **之前**保存目标序号（否则断言恒真）。
- `bbt_persist` 先擦 BBT 池并清 `a->bbt` 环形状态再标坏：多圈 ring 的 save→reload 会选旧
  record（`RISK-BBT-18`），clean-pool 路径才能稳定验证持久化。
- `format_in_progress_resume` 用**更大的 BBT sequence**（`0xFFFFFF00` 起步）写一条带
  FORMAT_IN_PROGRESS 的 record，让它在真实 bootstrap 扫描中胜出（隔离 `RISK-BBT-18`），
  再擦 CP 池制造「格式化未完成」状态；**不擦 BBT 载体**（擦 spare 会在失败路径上丢坏块图）。
  用例末尾跑完整 format 恢复介质；它排在 media 套件末尾（`boot_restore_reconcile` 之前）。

### 4.8 可测性 seam

| seam | 用途 | 产品固件是否编入 |
|---|---|---|
| `time_us` | 伪时钟 / 超时 | 是（平台 ops） |
| `l2p_owner_fn_t` | L2P 归属探测 | 是 |
| `metadata_ring_probe_fn_t` | ring locate 假 probe | 是 |
| `jrn_replay_ctx_s` | Journal 回放注入 | 是 |
| `fault_inject.h` | cache / journal 失败注入 | **否**（仅测试镜像） |

> 新增 seam 前先过 `CLAUDE.md` C2 / H13：能复用现有 seam 就不新增；新增 seam 必须
> 说明「封装了什么不变量/边界」，并在本表登记。

### 4.9 负例框架

负例框架的完整语义见 `docs/guides/testing_guide.md` §12；这里只记本套件的用法。

- **注入 API**：`fi_arm(kind, site, fail_after)` 在测试入口打开注入；
  `fi_arm_at(kind, site, at)` 指定第几次命中；`fi_take()` 由被测代码消费一次故障；
  `fi_hits()` 返回命中次数；`fi_reset()` 清状态。setup / teardown 都必须 `fi_reset()`，
  否则注入状态跨用例静默污染。
- **链接期 seam**：`fi_wrap.c` 用 `-Wl,--wrap` 包装测试符号，生产源码不动；当前只有
  cache FAILC 与 Journal 页写两个静态 hook，新增 hook 必须登记在
  `../guides/testing_guide.md` §12.10。
- **编译门禁**：注入只在 `FC_NAND_TEST_ENABLE` 测试镜像生效；产品镜像不编入。
- **判据**：注入类用例必须同时断言「预期降级」与「故障确实发生」（`fi_hits` 增长），
  只断言后者是假阳性；只断言前者无法证明 seam 生效（C8）。
- **负例模板**：先构造合法前置状态 → `fi_arm` → 触发生产 API → 断言返回码 / 状态 /
  账本 → `fi_reset` → 复跑一次证明可恢复。

---

## 5. 构建、运行与证据

### 5.1 构建

```text
make all
make release
make tests UT=FTL
```

- `make all` → `debug.elf`；`make tests UT=...` → `debug_ut.elf`，产品与 UT 产物分名。
- ⚠ `UT=FTL` / `UT=FC` / `UT=HIL` 共用 `debug_ut.elf`；烧 FTL UT 前必须让
  `make tests UT=FTL` 是最后一次 UT 链接。
- 症状对照：`test` 命令不存在 ⇒ 烧了产品镜像；`test /ftl/` 打 `NO test run` ⇒ 镜像里
  是 FC-only 或 HIL-only 套件。两者都先查产物路径与横幅时间戳（`RISK-PLAT-07`）。
- ⚠ `make tests` 只证链接，不证正确（R2）；碰建链 / 位图 / 地址三类量必须上板。

### 5.2 运行

1. pure：`test --list` → `test /ftl/pure/`（可只跑子套件；安全）。
2. negative pure：`test /ftl/negative/pure/`（RAM + 注入；安全）。
3. media：拔 USB → `test /ftl/media/`（破坏性，内部先 format）。
4. negative media：`test /ftl/negative/media/`（真 NAND + 注入；破坏性）。
5. speed：`test /bench/speed/ftl_rw`（破坏性，单独运行）。
6. 抓完整串口日志；记录套件/用例数、耗时、失败项、跳过的用例名。
7. 收尾：`format` + POR，恢复可用的空盘。

- 套件前缀必须带尾斜杠；`/ftl/` 由 `ut_run_all()` 展开为全部叶子套件。
- `--seed N` 复现；`--iterations N` 只乘 `MUNIT_TEST_OPTION_NONE`。

### 5.3 SKIP 语义

| 条件 | SKIP 用例 | 说明 |
|---|---|---|
| `l2p_miss_unmapped_evict` 前提不满足 | 该用例 | region 已驻留、无空行或 cache 有 DIRTY/FLUSHING/LOADING 行 |
| 依赖固定 UBI 擦除差的 WL 用例 | `wl_progress` 等 | 介质状态不满足时不硬造，打印原因 |
| 依赖固定坏块分布的 BBT 用例 | 部分 | 已有坏块会改变候选池 |
| release UT | 无额外 SKIP | `ASSERT` 裁空，注入类仍执行拒绝语义 |

- SKIP 必须打印原因；把 SKIP 当 PASS 是判据错误（R1）。
- `test /ftl/` 恒 0 条通常是过滤串写成父套件名而非叶子全路径。

### 5.4 日志与证据

三层输出互相独立：

| 开关 | 额外可见内容 |
|---|---|
| 默认（无开关） | 用例名 + `[ OK ]/[ SKIP ]` + 耗时 |
| `--log-visible info` | `[EV]` 简单前后对比（每个用例一条 summary） |
| `--log-visible debug` | `[EV]` 完整对比（inputs / outputs / 中间计数逐项列出） |
| `--log-project info/debug` | 额外打开 FTL/FC 项目日志（默认 `LOG_OFF`） |

- `[EV]` 行由用例自己调用 `ftl_ut_trace_summary/detail` 输出；没有调用就只有
  `[ OK ]`。需要看内部过程时用 `--log-project info/debug` 另开项目日志。
- 项目日志（`LOG_*`）在测试期间默认整档关闭；munit 的 `--log-visible` **每次 `test`
  调用重置为 ERROR**（不跨调用粘滞）；`--log-fatal` 是「达到该级别即中止」阈值，
  不是打印开关。
- 串口工具若开启「接收设置 → 自动换行」，会在静默期后补一个换行，把 munit 的名字行
  与 `[ OK ]` 拆成两行；这是采集端行为，不是固件输出（`RISK-PLAT-06`）。

原始串口日志与运行元数据放 `build/test-results/`（`build/` 已被 `.gitignore` 整目录忽略），
**不放 `tests/`**：

```text
build/test-results/<YYYYMMDD-HHMMSS>_<build>_<target>/
  run.log      # 串口终端工具的 log（原始输出）
  meta.txt     # commit / 镜像横幅时间戳 / 命令 / 结果 / 备注
```

`meta.txt` 的四件套是证据判据（P6）：`commit` = 代码版本，`image` 的横幅时间戳 =
烧的是哪个镜像，`command` / `result` = 跑了什么、结果如何。需要长期留证的结论写
`docs/design/known_open_items.md` 或 `docs/design/known_open_items.md`，原始日志不进版本库。

---

## 6. 不覆盖与判据盲区

### 6.1 不覆盖项

- 真 POR / 掉电：人工断电流程，不进 munit（允许 `ftl_init` + `pfr_boot_recovery` 模拟，
  但只标「模拟重启」）。
- NAND 页 / 擦 / 写失败注入：当前无 `ftl_sync_*` 注入 seam（覆盖回收 / 目录检查点 / abort
  不擦 victim 的注入部分）。
- 真空白片与格式化中断：真空白片需要全新芯片；`FORMAT_IN_PROGRESS` 的「save 后」分支
  已由 `format_in_progress_resume` 覆盖，erase 中 / CP marker 前掉电仍需人工
  （`RISK-BOOT-04` / `RISK-BOOT-17` / `RISK-BBT-02`）。
- USB 主机行为、外部吞吐（H2testw）、长时 soak；内部 FC/FTL 速度见 `/bench/speed/*`。
- release 档特有行为：功能验收以 debug UT 为准；release `-O2` 全量 UT 也用于回归，
  但不能替代 debug 档的断言覆盖——release 下 `ASSERT` 被裁空，行为差异须按 H14
  另补运行期拒绝。
- 跨 LUN 条带 / GC / WL（未实现）。
- HIL/USB 槽状态机、多 run 拆分 / 跨实例部分失败：见 `docs/design/known_open_items.md`
  ISS-HIL-01 / ISS-HIL-03。

> 每条风险「能不能构造负例、为什么暂时不可构造（K）」的逐条理由见 §7.2 台账；
> 本节只归纳整类缺口。

### 6.2 判据盲区

| 盲区 | 具体 | 后果 | 下一步 |
|---|---|---|---|
| 随机抽样不穷举 | `host_io_random` 固定 8 轮；`host_io_matrix` 只抽起点 | 罕见 LBA / 长度组合漏测 | 用 `--iterations` 加量；seed 写证据 |
| 模拟重启 ≠ POR | `ftl_ut_reboot_from_media` 是进程内重建 | 真掉电窗口（CP / Journal / FAILC 中间）漏测 | 人工 POR + 板端日志 |
| 介质状态依赖 | media 用例共享 format 后的芯片 | 前序坏块 / 脏行改变后序结果 | 用 `media_prepare` / 每个破坏性用例自清场 |
| 注入覆盖有限 | 只有 cache FAILC 与 Journal 页写两个静态 hook | NAND 读 / 擦失败的真实时序未注入 | `fi_wrap.c` 增 hook，登记 `../guides/testing_guide.md` §12.10 |
| `RR_FEATURE_READ_RETRY` | `0x89` 是保留地址，路径自始空转 | 读重试策略无真实介质证据 | 用户明确不处理，不纳入 UT 判据 |
| release 断言裁空 | `ASSERT` 在 `NDEBUG` 下为 `((void)0)` | 静默介质破坏路径无守卫 | H14 运行期拒绝 + release UT 回归 |

---

## 7. 风险追溯（副轴）

> 风险是**追溯维度**，不是本文的组织轴。状态、历史、是否闭合见
> `docs/design/known_open_items.md`；负例判定台账（M/N/K）见 §7.2。
> §7.1 只保留「判据非显然 / 跨模块」的风险簇。

### 7.1 风险簇

| RISK 簇 | 涉及测试项 | 判据 | 代表用例 |
|---|---|---|---|
| `RISK-UT-06` | `bbt` / `allocator` | BBT 持久化用例会永久标坏用户块；全量跑必须接受该成本 | `bbt_persist`、`bbt_ring_reload` |
| `RISK-ALLOC-06` / `RISK-ALLOC-09` / `RISK-MIG-03` / `RISK-ARCH-02` | `allocator` / `migrate` / GC | PBA 编解码往返；Free Pool 四本账同步；`valid_count` 搬移守恒；跨 LUN 未实现 | `pba_roundtrip`、`alloc_free_pool_invariants`、`alloc_valid_count_guard`、`gc_progress` |
| `RISK-BIT-01` | `allocator` / wbuf / cache | CW flat 绝对编号；span/plane 掩码；`NAND_FIELD_MASK` | `pba_cw_masks`、`wbuf_density` |
| `RISK-MD-06` / `RISK-MD-16` / `RISK-MD-17` / `RISK-BOOT-02` | `metadata` / `checkpoint` | 压缩写 abort 不回收 / 擦 victim；物理不可读 victim 的 fallback 成功路径未注入；`handle.error` 的清除语义 | `md_abort_keeps_victim`、`md_cp_read_fail_no_reclaim`、`md_status_error` |
| `RISK-JN-03` / `RISK-L2P-10` | `journal` / `L2P` | 写失败标记置位、WAL 退让、追平后放行；有界收尾 | `md_journal_write_fail_clear`、`l2p_wal_gate`、`journal_write_fail_wal_stall` |
| `RISK-BOOT-03` / `RISK-BOOT-04` / `RISK-BOOT-05` / `RISK-BOOT-13` / `RISK-BOOT-17` / `RISK-BOOT-22` / `RISK-BOOT-24` / `RISK-CP-03` / `RISK-BBT-02` | `PFR` / `CP` / `metadata` | CP 探针四态；FORMAT_IN_PROGRESS 恢复；启动账本守恒；CP 编码 / 定时器；真掉电点人工 | `cp_probe_clean_media`、`cp_corrupt_refuse_format`、`format_in_progress_resume`、`boot_restore_reconcile`、`md_cp_encoders`、`md_cp_init_timer`、`format_and_roundtrip` |
| `RISK-GC-06` / `RISK-GC-14` / `RISK-PLAT-10` / `RISK-NAND-06` | `GC` / `migrate` | 触发水位、victim benefit、CW 解码、失败封顶；SysTick / TIM6 时钟源；真 NAND 失败靠板端观测 | `gc_trigger`、`gc_select_victim`、`gc_decode_cw_lba`、`gc_progress`、`mig_eval_fake_clock`、`format_and_roundtrip` |
| `RISK-WL-04` | `wear_leveling` | UBI 判据；victim 最低擦除 used SB；有效比例阈值 | `wl_trigger_and_victim`、`wl_victim_threshold`、`wl_progress` |
| `RISK-L2P-03` / `RISK-L2P-06` | `L2P` / `CMT` | 驻留行状态；miss / evict；跨 region；TRIM 不复活 | `l2p_resident_states`、`l2p_miss_unmapped_evict`、`cross_region_roundtrip`、`trim_no_resurrection` |
| `RISK-CACHE-19` / `RISK-CACHE-20` / `RISK-CACHE-24` / `RISK-CACHE-27` / `RISK-CACHE-28` / `RISK-CACHE-29` | `cache_batch` | FAILC 结算、跨 block 序列、早回执可见性、flush 屏障 | `cache_failc_abort`、`cache_seq_cross_block`、`cache_early_ack_read_visible`、`cache_failc_undo_read_error`、`cache_early_ack_flush_barrier` |
| `RISK-HOST-05` / `RISK-HOST-08` / `RISK-HOST-09` / `RISK-HOST-11` | `pending` / `journal` / `L2P` | 确认停车超时；TRIM 持久化；Journal 压力下的时序 | `pend_confirm_park_timeout`、`pend_confirm_is_parked`、`trim_no_resurrection` |
| `RISK-WB-03` / `RISK-IF-04` / `RISK-CR-02` / `RISK-BBT-01` / `RISK-BBT-06` / `RISK-BBT-10` / `RISK-BBT-18` / `RISK-RD-01` | `wbuf` / `ftl_op` / `BBT` / `RR` | 窗口 owners / 在飞计数；route 同一实例；BBT 最新记录 / 重复完成；RR 健康单调 | `wbuf_append_read`、`op_route`、`bbt_ring_reload`、`rr_health` |

### 7.2 负例判定台账（FTL 层）

> 本表只列**当前未闭合**的 FTL 风险（索引状态取自 `docs/design/known_open_items.md`）。
> **M** = 有能变红的负例且上板通过；**N** = 注入点 / 触发步骤 / 断言已从源码确认、只差上板；
> **K** = 当前不可构造（写明原因）；非负例项保留其状态词（`待定位` / `待议` / `待决策`）。
> **信息**与**已闭合**条目的判定与理由只在 `known_open_items.md` 维护，本表不重复。
> 组名沿用风险登记模块分组；跨层框架（注入 API、宏隔离、验收）见
> `docs/guides/testing_guide.md` §12。

**metadata（2）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-MD-06` | M（待定位） | 无 seam；3 代写后 reboot×4；断言 region 未降级、读回第 3 代；用例 `md_alias_full_rebuild_repro` |
| `RISK-MD-23` | K | 需写目录检查点期间掉电（人工断电） |

**GC / migrate / journal（9）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-GC-02` | N（待定位） | `gc_write_complete_fail_closes_cold`：行为断言通过，但已迁移页读回 0xFF，根因未定 |
| `RISK-GC-05` | K | 性能优化项（GC bitmap），非失败场景 |
| `RISK-GC-06` | K | 需长压测：WL 5s 节拍 / CLOSED 门控需白盒或长压测 |
| `RISK-GC-14` | K | 需满盘 / 慢写窗口，短负例不可控 |
| `RISK-GC-15` | K | 需写到 free<32 的长序列 |
| `RISK-MIG-07` | M（待定位） | 同 `gc_read_unc_marks_bad`（熔断后不再成批标坏）；用例 `gc_systemic_read_fuse` |
| `RISK-JN-10` | K | 需写 journal 期间掉电 |
| `RISK-PERF-01` | K | 性能项（GC/WL 选块重复全表扫描），非失败场景 |
| `RISK-PERF-02` | K | 性能项（目的页逐 plane 单条写） |

**BBT（5）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-BBT-02` | N | `read_column_fail_seam`：列读 PBA 失败 seam（工厂扫描判坏路径待板） |
| `RISK-BBT-03` | K | 需 format 处理中掉电 |
| `RISK-BBT-04` | K | 介质格式演进项（header 无布局版本），非负例 |
| `RISK-BBT-10` | N | kick 路径（`bbt_kick_candidates`）已上板；factory-scan / format_erase 破坏性路径未覆盖 |
| `RISK-BBT-14` | K | 需 format 中途掉电 |

**掉电恢复 / 启动（5）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-BOOT-04` | K | 两阶段格式化中途掉电（人工断电） |
| `RISK-BOOT-17` | K | 需真空白片 |
| `RISK-BOOT-22` | K | 需 format 中途掉电 |
| `RISK-BOOT-25` | K | 需长压测：dangling 归因需日志采集与长序列 |
| `RISK-MD-15` | K | 需满盘压缩与双副本搬移压力 |

**Checkpoint（1）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-CP-04` | N | 扩展 `l2p_pool_starve_flush`：命令池占满时 force CP 有界返回、不无限等槽 |

**磨损计数 / 静态 WL（2）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-WL-04` | K | 需正常 WL/GC + 断电 + soak，属长时压测 |
| `RISK-WL-05` | K | 性能项（`wl_should_trigger` 与 `wl_select_victim` 各做一次全表扫描） |

**FTL↔FC 接口（2）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-IF-09` | K | 需主机中途拔线 / 中止（PC 侧手动） |
| `RISK-IF-12` | K | 需满盘大顺序写 + 序列门控，属长压测 |

**Host 提交 / 完成组 / pending（4）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-HOST-05` | 待定位 | 上游三支未定，需日志现场采集 |
| `RISK-HOST-11` | N | 复用 `trim_no_resurrection`：Journal 压力下 TRIM 持久化 |
| `RISK-HOST-14` | K | 需拔线 + 满盘 GC 窗口，人工触发 |
| `RISK-HOST-22` | K | 需 free+OP 同时耗尽；无注入点 |

**完成派发面（1）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-FC-05` | 待定位 | 依赖前序组合状态，复现条件已隔离，原因未证实 |

**页打包写缓冲（1）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-WB-07` | 待定位 | depth2 半页卡死未定位 |

**service / 后台调度器（2）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-BG-01` | 待定位 | 风险描述与当前实现不符（已无 `rr_last_scan` 门），需重审该条目 |
| `RISK-BG-10` | K | token bucket / 节拍是吞吐行为，需长压测 |

**模块 handle 契约（1）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-ARCH-02` | K | 跨 LUN 条带需多实例硬件 |

**allocator（1）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-ALLOC-09` | K | 需 plane 不平衡的低 free 长压测 |

**L2P cache（1）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-L2P-07` | K | 顺序读预取白占 cache 行 / 资源，可能延迟 FLUSH；需长压测（预取收益与延迟的权衡） |

**cache 长流（3）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-CACHE-18` | K | VWC 窗口需掉电量化 |
| `RISK-CACHE-26` | 待定位 | 1536 LBA 读回全 0 未定位，需压测复现 |
| `RISK-CACHE-29` | K | 长流 + 掉电窗口未量化 |

**平台定时调度器（2）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-PLAT-09` | K | 时基行为需板端长跑；UT 只能测伪时钟 |
| `RISK-PLAT-12` | K | DWT→SysTick 需板端对比 |

**NAND 拓扑扫描（1）**

| ID | 判定 | 用例 / 判定说明 |
|---|---|---|
| `RISK-TOP-03` | N | `onfi_read_fail_seam`：参数页读失败 seam（扫描拒绝启动路径待板） |

> 台账合计 **43** 条：M 2 / N 6 / K 30 / 状态词 5。HIL 主机前端相关的
> `RISK-HOST-20` / `RISK-HOST-21` 见 `docs/design/hil_ut_design.md` §7.2；
> `RISK-UT-43` 见 `docs/design/fc_nand_ut.md` §7.2。

---

## 8. 维护规则

- **新增/删除测试项**：同批更新 §1.1 与对应模块小节；不要新增全局用例表。
- **新增/删除用例**：改测试源；只有当「设计要点或判据」变化时才改本文。
- **风险状态变化**：只改 `docs/design/known_open_items.md`；§7 只在映射关系变化时改。
- **覆盖判定变化**：只改 §7.2 台账。
- **API / 夹具 / 地址 / 位图变化**：同批更新 §2 / §4。
- **运行结果**：写 `build/test-results/` 与 `known_open_items.md` 历史；本文不保留运行快照。
- **禁止**：§7 写状态词；§2 复制实现细节；文档头写「N 函数 / M 次运行」快照。
- **用例注释模板**：

```c
/* <name> — <一句话契约>
 *
 * 覆盖：<状态/边界/错误/账本/映射/持久化/并发>。
 * 判据：<断言比较哪两个值；负例是什么；失败后果>。
 * 备注：<纯 RAM / 真 NAND / 注入 / 依赖顺序>。
 */
```

- **pure 用例**：禁止调用 `ftl_init` / `pfr_*` / 任何 `ftl_sync_*` / NAND 命令。
- **media 用例**：必须走 `media_prepare()`（首次调用做一次 format）；除
  `preload_region_zero_and_high` 这类显式测 region 0 的用例外，使用 LBA 尾窗；
  不得依赖 host 文件系统。
- **每次改 API 后**先跑 `make tests UT=FTL`；测试源与生产同仓维护，不再冻结。
- 新增/修改用例必须保留「覆盖 / 判据 / 备注」注释块（对齐 `test_ftl_pure.c` /
  `test_ftl_media.c` 现有风格）。

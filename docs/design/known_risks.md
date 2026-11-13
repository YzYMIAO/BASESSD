# 已知风险登记

> **用途**：记录「暂不修 / 待上板 / 待测试源 / 待决策」的风险与判定结论，避免重复分析。
> **登记是硬约束（`CLAUDE.md` R11）**：缺陷与判定结论一经得出，当场登记；只在对话里说 = 未完成。
> 说不出失败场景/后果的归 `CLAUDE.md` §七 S，不进本表。
> **负例执行清单**：`docs/design/testing_guide.md` §11（按场景列出注入钩子、步骤与判据）。

## 0. 文档规则

### 0.1 状态词（受控，只有这 8 个）

| 状态 | 含义 | 需要动作 |
|---|---|---|
| `待定位` | 已复现或待归因 | 定位 |
| `待议` | 结论未定，需要讨论 | 讨论 |
| `待决策` | 需要用户裁决 | 用户 |
| `待上板` | 代码已改/已判定，**按「最新验证」分两档**：①`未上板` = 完全未验；②`YYYY-MM-DD 常规回归通过（残余：…）` = 正常路径已验、负例（注入/掉电/坏块/超时/拔线）未验 | 上板 |
| `待测试源` | 冻结 UT 待所有者同步 | 等测试源 |
| `待后续` | 已登记，独立任务 | 排期 |
| `信息` | 判定结论/维护不变量，无动作 | 无（保留可查） |
| `已闭合` | 有证据且无残余动作 | 归档 |

禁止同义写法：`已改未上板` / `部分已上板` / `正常路径已上板` / `已关闭` 一律归一到上表。

### 0.2 索引表

每个模块一张索引表，四列固定：

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-XXX-NN | 待上板 | ≤40 字，只写问题本身 | 见下 |

**最新验证**取值：`未上板` · `YYYY-MM-DD 编译通过` · `YYYY-MM-DD 常规回归通过（残余：…）` · `YYYY-MM-DD 上板通过` · `YYYY-MM-DD 上板失败` · `YYYY-MM-DD 代码核对`。
索引只放未闭合与 `信息` 条目；`已闭合` 进模块末尾的「已闭合」表。

**汇报口径**：`待上板` 必须拆成两档分别计数——`完全未验`（最新验证 = `未上板`）与 `常规已验、残余负例`（最新验证含「常规回归通过」）。不得只报总数，否则会把「正常路径已随 UT/H2testw 跑过」误算成「从未测试」。

### 0.3 详情块

每条风险一个小节，字段固定：

```markdown
#### RISK-XXX-NN 一句话标题

- **后果**：失败场景与后果（当前事实）
- **缓解**：当前已有的防护（当前事实）
- **下一步**：重访触发 / 待做动作
- **证据**：`文件:符号`
- **历史/验证**：（倒序，最新在上；只追加，不改写）
  - YYYY-MM-DD 动作 → 结果
```

- 前四个字段只写**当前事实**：不写「已改 / 原先 / 本轮 / 此前」；变更史全部进历史行。
- 历史行格式 `YYYY-MM-DD 动作 → 结果`；日期未知写 `（日期未记录）`，**不得编造**。
- 一条风险的当前字段超过 4 行 ⇒ 拆分，或把细节移进 archive。

### 0.4 归档规则

- `已闭合`：从详情区移出，只在模块末尾「已闭合」表和 `docs/archive/known_risks_batches.md` 保留；原文整体移动，不改写。
- `信息`：保留在主文件（结论可查），「下一步」写维护规则而不是待做动作。
- 状态变更时同步索引的「状态」与「最新验证」两列（本仓接受这两处的重复维护）。

---

## 1. metadata 模块

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-MD-01 | 待上板 | 日志块擦除持续失败无限重试，无坏块策略 | 未上板 |
| RISK-MD-02 | 待上板 | 压缩预算按启动时 l2p_live，在飞 region 写可能超预算 | 未上板 |
| RISK-MD-06 | 待定位 | L2P 指向已归还/已擦块，恢复回退 full rebuild 丢映射 | 2026-11-01 release 156/156 未复现 |
| RISK-MD-18 | 待上板 | 目录检查点未覆盖最新 Directory 时压缩擦 victim | 2026-11-04 编译通过 |
| RISK-MD-19 | 待上板 | victim 未排除在飞槽位，stale count 被清零 | 2026-11-04 编译通过 |
| RISK-MD-20 | 待上板 | 压缩 read→commit 无 CAS，陈旧映射持久化 | 2026-11-04 编译通过 |
| RISK-MD-21 | 待上板 | 目录检查点 abort 不消费槽位，二次编程 | 2026-11-04 编译通过 |
| RISK-MD-22 | 待上板 | 借来的日志块标 BAD 时漏减 borrow 配额 | 2026-11-04 编译通过 |
| RISK-MD-23 | 待上板 | ring frontier 落在块中时复用半编程槽 | 2026-11-04 编译通过 |
| RISK-MD-24 | 待上板 | initial 目录检查点只写单 plane，回退到旧 record | 2026-11-04 编译通过 |

### 详情

#### RISK-MD-01 日志块擦除持续失败无限重试

- **后果**：日志块擦除持续失败时原会无限重试，无坏块策略。
- **缓解**：连续 3 次擦失败 ⇒ `bbt_mark_bad` + 清回收位，不再重试。
- **下一步**：注入擦除失败：第 3 次后进 BBT、回收位清零、后台停止重试。
- **证据**：`src/ftl/metadata_job.c:md_reclaim_complete`。
- **历史/验证**：
  - （日期未记录）修复：3 次失败标坏 → 未上板

#### RISK-MD-02 压缩预算按启动时 l2p_live

- **后果**：压缩预算按启动时 `l2p_live` 计算，在飞 region 写可能使其超预算。
- **缓解**：`MD_COMPACT_RESERVED_SLOTS` 预留空槽；超预算只良性中止、不擦 victim。
- **下一步**：压缩期间并发写：不再出现「无空槽中止」。
- **证据**：`src/ftl/metadata_job.c:md_compact_begin` / `md_compact_write`。
- **历史/验证**：
  - （日期未记录）修复：预留空槽 → 未上板

#### RISK-MD-06 L2P 指向已归还/已擦块

- **后果**：L2P 映射指向已归还/已擦除的日志块——块 46 的 region 页 CRC 错、目录出现同 PBA 别名（52≡86 / 53≡87 / 54≡88），开机回退 `full metadata rebuild` 并降级 8 个 region ⇒ 映射丢失。只在「format + 3 代写 + 第 4 次重启（`boot type=2`）」的 UT 硬复位序列下确定性复现；H2testw 415/423 MB 与真实断电验证均 0 错误。
- **缓解**：已落地的独立改进：① `pfr_valid_sub` 计数为 0 时按运行期 `ftl_block_sub_valid_if_live` 同口径跳过，不再把一条陈旧减法升级为「整份块元数据快照作废」；② 回放不再对 `old_pba` 调 `materialize`；③ 回放后不变量守卫成为「快照不可信」的唯一判据。目录检查点单飞、slot 预留、写回自校验探针均已尝试，未消除故障（探针已撤）。
- **下一步**：根因未定位。再开条件：真实掉电后出现 `region … degraded` / `mapping lost`，或 `dangling` 与 `mapping lost` 同时出现。判据：`replay: N sub-at-zero kept …` / `fallback … reason/pba/vc_end/live/sub0` / `[FTL] dangling`。
- **证据**：`src/ftl/power_failure_recovery.c`（回放/守卫）；`src/ftl/metadata.c`（`dirty_gen` / `dir_ckpt_gen`）；`src/ftl/metadata_slot.c`；`include/ftl/bst.h:ftl_block_sub_valid_if_live`。
- **历史/验证**：
  - 2026-11-01 13:11 用户裁决：当前介质状态下观测不到，本轮结束；**根因未定位、改动未上板验证**
  - 2026-11-01 13:21 release 全量 `test` 156/156、`/speed` 5/5 `err=0`；无 fallback / dangling
  - 2026-11-01 13:3x 产品镜像 `format --verify` + H2testw 415/423 MB 0 错误
  - 2026-11-01 08:47 写回自校验探针无效且抢占 FC（把 pad 收尾挤成 stray page），已整体撤除
  - 2026-11-01 07:24 `dir dup` + `degrade probe ff=1`（有数据但 CRC 不符）⇒ 同一页被编程两次；根因链 = `ftl_op_dispatch` 不判 NULL ctx（已修）
  - 2026-11-01 06:54 擦除时 `refs=0 resv=0` ⇒ 账本自洽，排除「在飞写被擦」
  - 2026-11-01 二次修复（`slot_resv` + `md_log_block_is_idle`）上板失败，指纹不变
  - 2026-11-01 最小复现：format + 3 代写 + 第 4 次重启（`boot type=2`）

#### RISK-MD-18 目录检查点未覆盖最新 Directory 时擦 victim

- **后果**：`md_dir_ckpt_commit` 后 `dir->ckpt.dirty` 仍为 true（写入期间有 commit / 混代），普通 reclaim 会回 IDLE，compaction 旧逻辑直接 `CP_RECLAIM` 擦块；掉电后旧 record 仍指向 victim ⇒ 映射指向已擦块。
- **缓解**：`MD_JOB_CP_DIR_CKPT_WRITE` commit 后先判 `dirty`：true ⇒ 回 `CP_DIR_CKPT_PREP` 重写稳定记录，不登记候选、不擦 victim。
- **下一步**：GC 压力下注入/等待目录检查点期间的前台 commit；确认无 `dirty` 擦块。
- **证据**：`src/ftl/metadata_job.c:MD_JOB_CP_DIR_CKPT_WRITE` / `CP_RECLAIM`；`src/ftl/metadata.c:md_dir_ckpt_commit`。
- **历史/验证**：
  - 2026-11-04 修复：commit 后判 dirty → 编译通过，未上板

#### RISK-MD-19 victim 未排除在飞槽位

- **后果**：job 启动前已发出的前台写回可能在扫描期间 commit 到 victim；旧代码扫描越过该 region 后把 `l2p_live != 0` 当账本分叉清零，随后擦块。
- **缓解**：`md_compact_victim_find` 跳过 `md_slot_reserved` 的块；扫描结束若仍 `live != 0` 则 `MD_COMPACT_FAILED`（不擦、不清零），下一轮重扫。
- **下一步**：GC 压力 + 在飞写回；若仍见 `live but no directory entry`，保留现场查 slot_resv/Directory，不得改回清零。
- **证据**：`src/ftl/metadata_job.c:md_compact_victim_find` / `md_compact_read`。
- **历史/验证**：
  - 2026-11-04 修复 → 编译通过，未上板

#### RISK-MD-20 压缩 read→commit 无 CAS

- **后果**：读 victim 页到写完成之间，前台写回把同一副本移到新位置；旧逻辑按当前 Directory 的 old 值减账并覆盖，造成陈旧映射持久化/主备分叉。
- **缓解**：`md_handle_s.cur_source` 记录本次读到的源 PBA；写完成 commit 前重查该副本，若已变化则丢弃新槽（死页）并回 `CP_READ`，不覆盖。
- **下一步**：GC + 并发 host 写；若出现 `superseded` 日志，确认没有 Directory 回退。
- **证据**：`src/ftl/metadata_job.c:md_compact_read` / `md_cp_write_complete`；`include/ftl/metadata.h:cur_source`。
- **历史/验证**：
  - 2026-11-04 修复 → 编译通过，未上板

#### RISK-MD-21 目录检查点 abort 不消费槽位

- **后果**：16 页 record 写到一半超时 abort 后，下一轮复用同一非块首槽、无擦除 ⇒ 对已编程页二次编程；若 abort 发生在块首擦除失败后，同块后续槽也不可用。
- **缓解**：`md_dir_ckpt_s.erase_required` 记录本次是否块首槽；`md_dir_ckpt_abort` 消费当前槽：块中槽 `write_idx++`，块首/擦失败槽跳到下一块槽 0；commit/abort 清标志。
- **下一步**：注入 submit stall / 擦失败；确认下次写点不落在半编程槽；观察 `dir_ckpt` seq/idx 推进。
- **证据**：`src/ftl/metadata.c:md_dir_ckpt_prepare/commit/abort`；`include/ftl/metadata.h:erase_required`。
- **历史/验证**：
  - 2026-11-04 修复 → 编译通过，未上板

#### RISK-MD-22 借来的日志块标 BAD 时漏减 borrow 配额

- **后果**：reclaim 连续擦失败走 `bbt_mark_bad`，旧路径不调 `md_log_release`，`log_borrow_count` 永久占一个（上限 8），削弱 OP 借用逃生口。
- **缓解**：BAD 收尾改为 `bbt_mark_bad` → `md_log_release`（BAD 分支只结算 borrow 配额）→ `md_reclaim_clear`。
- **下一步**：注入擦失败，检查 `borrow=` 随 BAD 块下降。
- **证据**：`src/ftl/metadata_job.c:md_reclaim_complete`；`src/ftl/metadata.c:md_log_release`。
- **历史/验证**：
  - 2026-11-04 修复 → 编译通过，未上板

#### RISK-MD-23 ring frontier 落在块中

- **后果**：掉电时 record 只写了一部分（chunk0 无效/部分有效）时，locate 返回的写点可能落在块中；`need_erase` 只对块首为 true ⇒ 下一写复用半编程槽。
- **缓解**：`md_ring_locate` 对块中 frontier 保守跳过一槽（块首仍走擦除）；目录检查点/BBT/CP 共用该 locate。
- **下一步**：写目录检查点期间断电，重启后续写一次再断电；确认无重复页/CRC 错。
- **证据**：`src/ftl/metadata.c:md_ring_locate`。
- **历史/验证**：
  - 2026-11-04 修复 → 编译通过，未上板

#### RISK-MD-24 initial 目录检查点只写单 plane

- **后果**：格式化路径 `pfr_format_write_initial_dir_ckpt` 用 `ftl_sync_erase_block` + `ftl_sync_write_page` 只处理 plane_mask 的第一个 plane，而运行期 CP/metadata job 是 multi-plane 擦 + 镜像写。该页后续读失败时，`md_dir_ckpt_record_probe_cheap` 回退到另一 plane 上未擦除的旧 record ⇒ 开机 load 到格式化前的 Directory，指向已擦除的 L2P 日志块 ⇒ 映射整体错误。
- **缓解**：格式化路径与运行期 CP 同一口径：多 plane 时 `ftl_sync_erase_block_multi_plane` 擦整块、`ftl_sync_write_page_mirror` 写各 plane；写完回读第 0 页校验 magic / seq / layout_version。
- **下一步**：format → 断电 → 重启，确认 `dir_ckpt load` 成功且 seq 正确；再验证单 plane 读失败时不会载入旧 record。
- **证据**：`src/ftl/power_failure_recovery.c:pfr_format_write_initial_dir_ckpt`；对照 `src/ftl/checkpoint.c:cp_erase_submit` / `cp_write_submit`。
- **历史/验证**：
  - 2026-11-04 修复 → 编译通过，未上板

---

## 2. GC / migrate / journal

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-GC-01 | 待上板 | 永久不可读/ META 损坏 victim 反复 abort，容量不增长 | 未上板 |
| RISK-GC-02 | 待上板 | 目的页写失败不能整块标坏已有成功迁移页的 Cold SB | 未上板 |
| RISK-GC-04 | 待上板 | 部分 plane 擦除后中止，旧 Closed 条目误认已释放 plane | 未上板 |
| RISK-GC-05 | 待后续 | GC Bitmap 未实现，掉电后 GC 从块头重来 | — |
| RISK-GC-06 | 待上板 | WL 5s 节拍/CLOSED 门控/valid_count 上界/三态未板验 | 2026-11-12 常规回归通过（/ftl/ 100/100 + H2testw）；残余：WL 5s 节拍直接断言 |
| RISK-GC-10 | 待上板 | 系统性 victim 读失败无熔断，好块成批标 BAD | 未上板 |
| RISK-GC-14 | 待上板 | no-gain 判据依赖 total_writes/free 快照 | 2026-10-23 阈值重基线 |
| RISK-GC-15 | 待上板 | BUSY 档挡掉 urgent GC，free 掉到 1 才急救 | 已上板（free=64 未触发） |
| RISK-GC-16 | 待上板 | T5 磨损 GC 用全表 max/min，永不收敛 | 已上板 |
| RISK-GC-17 | 待上板 | cache-program batch 页基址误判，batch 永久禁用 | 2026-11-04 编译通过 |
| RISK-GC-18 | 待上板 | 空 victim 快路径绕过 Journal 持久性闸门 | 2026-11-04 编译通过 |
| RISK-GC-19 | 待上板 | 解码缺 meta 的 CW 被静默当未写，静默丢数据 | 2026-11-04 编译通过 |
| RISK-MIG-01 | 待上板 | 等待命令丢失/卡住时永久 active | 未上板 |
| RISK-MIG-02 | 待上板 | predictive 迁移活锁（同 GC-07 修法） | 未上板 |
| RISK-MIG-03 | 待上板 | 标 BAD 后 L2P 仍指向该块，valid_count 回绕 | rev32 上板观测 |
| RISK-MIG-05 | 待上板 | GC 读失败 abort 日志刷屏阻塞 UART | 2026-10-23 修复 |
| RISK-MIG-06 | 待上板 | mark BAD 日志单位/原因不区分 | 2026-10-23 修复 |
| RISK-MIG-07 | 待定位 | 大批块被 GC 读失败退役，L2P 悬空到 BAD 块 | 2026-10-21 复现 |
| RISK-MIG-09 | 待上板 | host 写失败只标 plane0 的块 | 2026-10-21 修复 |
| RISK-MIG-10 | 待上板 | 迟到完成可能按新 job 的 phase 派发 | 2026-11-04 编译通过 |
| RISK-JN-01 | 待上板 | Journal 环形块擦除失败无限重挂 need_erase | 未上板 |
| RISK-JN-02 | 待上板 | 同步刷页返回 void，调用者不知屏障未达成 | 未上板 |
| RISK-JN-03 | 待上板 | write_fail_pending + CP 清账未做故障注入 | 2026-10-31 上板 |
| RISK-JN-04 | 待上板 | drain 超时后仍同步刷页，同页二次编程 | 未上板 |
| RISK-JN-05 | 待上板 | 回放失败/降级日志逐条打印，拖慢开机 | 2026-10-21 修复 |
| RISK-JN-06 | 待上板 | 回放工作缓冲从栈改文件级 static | 2026-11-04 编译通过 |
| RISK-JN-07 | 待上板 | jrn_block_next 把 from 自身算作下一块 | 2026-11-05 编译通过 |
| RISK-JN-08 | 待上板 | 回放第二遍读失败不上报，静默跳过条目 | 2026-11-05 编译通过 |
| RISK-JN-09 | 待上板 | 32-bit Journal 序号无回绕处理 | 2026-11-05 编译通过 |
| RISK-JN-10 | 待上板 | 撕裂 frontier 页重复编程 + 每次开机 full rebuild | 2026-11-05 编译通过 |
| RISK-RR-02 | 待上板 | 恢复成功仍报 IO_ERROR / recovered 只看最后 plane | 未上板 |
| RISK-RR-03 | 待上板 | ECC 健康状态可降级（CRITICAL→WARNING） | 未上板 |
| RISK-RR-04 | 待上板 | 预测巡检命中含 BAD plane 的 SB，migrate 卡死 | 未上板 |
| RISK-RR-05 | 待上板 | SLC 无 read-retry table 却做 8 次重试 + 邻居辅助 | 2026-10-21 修复 |
| RISK-WL-10 | 待上板 | 静态 WL 快照恢复后从不触发（分母用整 SB） | 2026-11-04 编译通过 |
| RISK-WL-11 | 待上板 | WL 目的准备失败静默退化为普通分配 | 2026-11-04 编译通过 |
| RISK-ALLOC-14 | 待上板 | 运行期标 BAD 让 Closed 条目永久卡死 | 2026-11-04 编译通过 |
| RISK-PERF-01 | 待后续 | GC/WL 选块重复全表扫描 | — |
| RISK-PERF-02 | 待后续 | GC/WL 目的页逐 plane 单条写，无 multi-plane 命令 | — |
| RISK-PERF-03 | 信息 | cache read 生效后读速未提升，瓶颈在数据面 | 2026-10-25 板端实测 |
| RISK-PERF-04 | 信息 | 内部数据面 CPU-bound，NAND 阵列利用率极低 | 2026-10-25 板端实测 |

### 详情

#### RISK-GC-01 永久不可读 victim 反复 abort

- **后果**：永久不可读 / META 损坏的 victim 会反复 abort，容量不再增长。
- **缓解**：每个 plane 源块连续 3 次读失败 ⇒ `bbt_mark_bad` + 从 Closed 条目摘除该 plane；计数跨 job 保留。
- **下一步**：造不可纠读：第 3 次后 GC 换候选、容量继续增长。
- **证据**：`src/ftl/migrate.c:mig_read_complete` / `mig_job_start` / `mig_job_finish`。
- **历史/验证**：
  - （日期未记录）修复：按 plane 连续失败标坏 → 未上板

#### RISK-GC-02 目的页写失败

- **后果**：目的页写失败不能把已有成功迁移页的 Cold SB 整块标坏。
- **缓解**：写失败立即 `alc_super_close(COLD)`，不标 BAD；提交侧无 plane 编程时 `alc_super_open_rewind_pair`，有 plane 编程则关闭 Cold SB。
- **下一步**：注入目的页写失败：关闭 Cold SB、已迁移页可读、下一页换新 SB。
- **证据**：`src/ftl/migrate.c:mig_dst_fail_cleanup` / `mig_write_complete` / `mig_write_step`。
- **历史/验证**：
  - （日期未记录）修复 → 未上板

#### RISK-GC-04 部分 plane 擦除后中止

- **后果**：部分 plane 擦除后中止，旧 Closed 条目可能误认已释放 plane。
- **缓解**：每擦完一个 plane 即 `alc_super_closed_drop_plane`；最后一个 plane 摘除时移除条目。
- **下一步**：两 plane victim 中途中止/断电：重启后旧条目不含已释放 plane。
- **证据**：`src/ftl/migrate.c:mig_erase_complete`；`src/ftl/allocator.c:alc_super_closed_drop_plane`。
- **历史/验证**：
  - （日期未记录）修复 → 未上板

#### RISK-GC-05 GC Bitmap 未实现

- **后果**：掉电后 GC 从块头重来。
- **缓解**：正确性由 F10 + 重试覆盖。
- **下一步**：性能优化项。
- **证据**：`docs/design/ftl_design.md §7.2`。
- **历史/验证**：
  - （日期未记录）设计判定：性能项 → 待后续

#### RISK-GC-06 GC 行为变化未板验

- **后果**：WL 5s 节拍、CLOSED plane 门控、`valid_count` 单块上界、`gc_decode_cw_lba` 三态未经板验。
- **缓解**：编译零告警。
- **下一步**：按 §2 板端清单回归（GC 并发覆盖写/TRIM、断电、WL、fault injection）。
- **证据**：`src/ftl/gc.c` / `wear_leveling.c`。
- **历史/验证**：
  - 2026-10-25 gc_trigger/select/decode/progress 已过；WL 5s 节拍未直接断言 → 部分上板


#### RISK-GC-10 系统性 victim 读失败无熔断

- **后果**：Mode 5 运行期读回 UNC 时，每个 victim 连续 3 次失败就标 BAD + 摘除，GC 立刻选下一个 victim，同样失败 ⇒ `free` 从 31 掉到 18、BAD 图成批写坏；host 读 LBA 1/4 命中已 BAD 块 ⇒ Windows Code 21，H2testw 108 MB 中止。
- **缓解**：新增熔断：连续 8 个不同 victim 块被标 BAD（`MIG_SYSTEMIC_READ_FAIL_MAX`）⇒ 停止 GC 标坏并上报。
- **下一步**：format 后复跑，确认不再成批标坏；看 `fc=` 位区分 UNC/ NAND/META。
- **证据**：`src/ftl/migrate.c` 熔断；板端 2026-10-21 日志。
- **历史/验证**：
  - v0.87 修复；v0.86 上板未通过 → 待上板

#### RISK-GC-14 no-gain 收益判据边界

- **后果**：no-gain 结论依赖 `total_writes/free` 快照；host 写流用 2s 窗口，极慢写会误判 idle；写路径 NO_SPACE 最终表现为 IO_ERROR。
- **缓解**：阈值/退避按 `FREE_POOL_TARGET=32` 基线；no-gain 只在真实搬移/回收后清；任何 `total_writes` 变化会重扫。
- **下一步**：上板复测满盘只读/覆盖写/满盘写；若误报 NO_SPACE，再调阈值/混合策略。
- **证据**：`src/ftl/gc.c:gc_select_victim`；`src/ftl/migrate.c:mig_gc_mode_select` / `mig_gc_scan_is_allowed` / `mig_gc_nogain_note`。
- **历史/验证**：
  - 2026-10-23 阈值重基线 → 待上板

#### RISK-GC-15 BUSY 档挡掉 urgent GC

- **后果**：host 持续写时 `mig_handle` 先 return，`free` 掉到 1 才急救，且已无目的页可分配。
- **缓解**：urgent 处理移到 BUSY 早退前；`FREE_POOL_TARGET = 32`；BUSY 档 `free<32` 也执行水位 GC；`diag` 增加 `mv/nop`。
- **下一步**：继续写至 free<32 观察水位 GC。
- **证据**：`src/ftl/migrate.c:mig_handle`；`include/ftl/allocator.h:FREE_POOL_TARGET`；`src/hil/cli.c:diag`。
- **历史/验证**：
  - 已上板（free=64 未触发 T1，符合 target=32）→ 待上板（残余场景）

#### RISK-GC-16 T5 磨损 GC 永不收敛

- **后果**：T5 用「全表 max/min」作比值，min 可能来自从未使用的空闲块 ⇒ 永不收敛；H2testw 期间 GC 持续运行，`free` 从 3388 被搬到 64 后仍不停，读写速度下降。
- **缓解**：T5 从 `gc_should_trigger` 移除；磨损维护改由 `wear_leveling` 的 UBI 判据（`max_free_ec − min_used_ec ≥ WL_THRESHOLD`）在 host 空闲时独立启动；`mig_wl_begin` 增加 `[WL] start` 行。
- **下一步**：空闲态观察 `[WL] start`。
- **证据**：`src/ftl/gc.c:gc_should_trigger`；`src/ftl/migrate.c:mig_wl_begin`。
- **历史/验证**：
  - 已上板复跑 H2testw（无 T5/无 GC，843/889 KB/s）；空闲态 `[WL] start` 未观测 → 待上板

#### RISK-GC-17 cache-program batch 页基址误判

- **后果**：`mig_page_base_has_mismatch` 直接比较 `job.base[plane]`，但 `ftl_lba_page_base()` 刻意保留 plane 位 ⇒ 同一 pair 的两 plane 基址恒差 CW_NUM，两 plane 都有数据的页一出现就把 `state.batch_disabled` 永久置位 ⇒ 本次开机 GC/WL 的成对 cache 写全部失效（回退逐页单 plane 写，数据无损、掉吞吐）。
- **缓解**：比较键改为 `ftl_lba_pair_base()`；顺带补齐 batch 提交后的 external/command deadline、batch 攒页复用 `mig_page_next()` 清 per-plane 状态。
- **下一步**：看 `batch_jobs` / `batch_pages` 是否增长、`batch_disabled` 不再置位；GC 吞吐对比。
- **证据**：`src/ftl/migrate.c:mig_page_base_has_mismatch` / `mig_batch_submit`；`include/ftl/ftl_config.h:172`。
- **历史/验证**：
  - 2026-11-04 修复 → 编译通过，未上板

#### RISK-GC-18 空 victim 快路径绕过 Journal 闸门

- **后果**：`valid_cw==0` 直接进 `MIG_ERASE`，且 `MIG_FLUSH_JRN` 以 `migrated==0` 早退；上一 job 已提交映射但 Journal 条目仍在页缓冲时 abort，下一 job 选中同一 victim 会先擦后掉电 ⇒ 回放后 L2P 指向已擦块。
- **缓解**：空 victim 起始相位改 `MIG_FLUSH_JRN`；删除 `migrated==0` 早退，擦除前一律要求 `jrn_is_idle && !has_erase_pending && !has_pending_entries`。
- **下一步**：GC 进行中强制 abort → 立即断电 → 重启查 `dangling` 与映射。
- **证据**：`src/ftl/migrate.c:mig_job_start` / `mig_flush_jrn_step`；`include/ftl/journal.h:459/505/528/537`。
- **历史/验证**：
  - 2026-11-04 修复 → 编译通过，未上板

#### RISK-GC-19 解码缺 meta 的 CW 被静默当未写

- **后果**：`gc_decode_cw_lba` 对 `fc_buffer_get_meta()==NULL` 直接 `continue` ⇒ 该 CW 不搬、victim 照常擦，L2P 仍指向它 = 静默丢数据。FC 契约允许 NULL（链上无该 CW / desc 无 buffer），正常读链不可达。
- **缓解**：返回 `GC_DECODE_CORRUPT`，调用者中止且不擦 victim。
- **下一步**：构造/注入缺 CW 的读链确认走 CORRUPT、victim 未擦。
- **证据**：`src/ftl/gc.c:gc_decode_cw_lba`；`include/fc/fc_interface.h:254`。
- **历史/验证**：
  - 2026-11-04 修复 → 编译通过，未上板

#### RISK-MIG-01 等待命令丢失/卡住

- **后果**：READ_SETTLE / FLUSH_JRN 等阶段原会把 stall 计时清零，等待无上限；等待中的命令丢失也可能永久 active。
- **缓解**：`mig_job_wait` 使用独立 `FTL_WAIT_TIMEOUT_MS`；`waiting` 期间也 arm 超时，超时 abort 且写阶段关闭 Cold SB；迟到完成只 release。
- **下一步**：注入 region 加载永久卡住/完成丢失：job 超时中止、不永久挂起。
- **证据**：`src/ftl/migrate.c:mig_job_wait` / `cmd_wait_ms` / `mig_job_step`。
- **历史/验证**：
  - （日期未记录）修复（合并 §2K GC-05）→ 未上板

#### RISK-MIG-02 predictive 迁移活锁

- **后果**：`kind=2` 反复选中同一 SB（plane0=453 / plane1=1964），page 读失败 → abort → 立刻重选，约 1 Hz 刷屏；`fail_count` 被其它页读成功清零，两个 plane 共用一个计数槽交替清零。
- **缓解**：与 RISK-GC-07 同一修法（按 plane 分计数、成功不清零）。
- **下一步**：format 后复跑，观察 `mark BAD` 速率与预测迁移是否收敛。
- **证据**：`src/ftl/migrate.c:mig_read_complete`；板端 2026-10-20 日志。
- **历史/验证**：
  - 2026-10-20 修复（与 GC-07 同修法）→ 未上板

#### RISK-MIG-03 标 BAD 后 L2P 仍指向该块

- **后果**：`bbt_mark_bad()` 清 `valid_count` 后，L2P 仍可能指向该块；写确认与 TRIM 直接调 `ftl_block_sub_valid()` ⇒ debug 撞 `valid_count > 0` 断言、release 下 uint16 回绕成 65535，虚高计数污染 GC/WL。
- **缓解**：新增 `ftl_block_sub_valid_if_live()`（BAD 或 count==0 跳过）；写确认、TRIM、migrate 源块减计数统一用它。
- **下一步**：TRIM 同一 LBA；release 档回绕负例。
- **证据**：`include/ftl/bst.h:ftl_block_sub_valid_if_live`；`src/ftl/ftl.c`（写确认 / `ftl_trim`）；`src/ftl/migrate.c`。
- **历史/验证**：
  - rev32 上板：308/2493 被 predict 标 BAD 后，覆盖写只打 `[FTL] dangling`，无断言、无回绕，新映射落 OPEN 块 → 已上板观测
  - rev27 崩溃：`dangling lba=9 ... st=BAD` 后断言失败

#### RISK-MIG-05 GC 读失败 abort 日志刷屏

- **后果**：旧版每个 abort 打 3 行，同一坏块反复重试时淹没进展并阻塞 UART。
- **缓解**：`mig_job_abort` 合并为一行含 `g0/g1`；逐 plane `[GC] victim` DEBUG 已删；abort 按时间 1 行/5s（有 count 兜底）。
- **下一步**：坏块/满盘场景：abort 日志 ≤1 行/5s，仍带 gblock/cause。
- **证据**：`src/ftl/migrate.c:mig_job_abort` / `mig_abort_log_is_due`。
- **历史/验证**：
  - 2026-10-23 修复 → 未上板

#### RISK-MIG-06 mark BAD 日志单位/原因不区分

- **后果**：旧版 `blk=` 与 per-plane block 混用，且 `read_ok=false` 合并 FC 状态错/短链/META 三种原因。
- **缓解**：`victim read failed` 带 `gblk=` 与 `cause=`（`fc_err` / `short_chain` / `decode_corrupt`）；`mig_job_abort` 行带同一 cause。
- **下一步**：坏页：同一 gblk 的 cause 可直接分流到 FC/数据面/META。
- **证据**：`src/ftl/migrate.c:mig_read_complete` / `mig_job_abort`。
- **历史/验证**：
  - 2026-10-23 修复 → 未上板

#### RISK-MIG-07 大批块被 GC 读失败退役

- **后果**：H2testw 写入 428 MB 期间 GC 累计 `mark BAD` 127 个 gblock（`fc_err`，page 8..61 均有）；`[FTL] dangling lba=208 ... st=BAD` 证明 L2P 仍指向已退役块。校验到 107 MB 时文件系统元数据 LBA 3 落在已 BAD 块 ⇒ RR 8 次重试失败 ⇒ Code 21。该 LBA 数据不可恢复；写阶段本身全容量通过。
- **缓解**：① RR 对已 BAD 块快速失败（不再 8 次重试/重复标坏）；② `read unrecoverable` 5s 限流 + suppressed；③ MIG abort 日志增加原始 `fc=` 状态位；④ 写阶段 GC 读失败率高仍是开放问题。
- **下一步**：format 后复跑，观察 `mark BAD` 速率是否仍 >100/次；看 `fc=` 位判断 UNC 占比；UNC 为主 ⇒ 换片，NAND/META 为主 ⇒ 查 GC 读地址/描述符链。
- **证据**：`src/ftl/read_recovery.c:rr_settle_plane` / `rr_job_step`；`src/ftl/migrate.c:mig_read_complete` / `mig_job_abort`；板端 2026-10-21 `tmpl.log`。
- **历史/验证**：
  - 2026-10-21 夜 rev33 复现：156→420 BAD（+264，全部 `fc_err`=UNC）、`dng=55`、`spare=50/50→0/29`，写冻结、USB 反复重挂载
  - 2026-10-21 首次复现（428 MB 写，127 BAD）

#### RISK-MIG-09 host 写失败只标 plane0 的块

- **后果**：`ftl_host_io_write_fail` 旧实现只标 plane0，plane1 的编程失败要等 GC 擦除才暴露，期间坏 plane 仍可能被分配。
- **缓解**：遍历失败命令的 desc 链，按逐 CW `status` + `desc->paa` 的 plane 得到失败 plane 掩码，分别 `bbt_mark_bad` 两个 plane 的块；没有逐 CW 失败位时退回旧行为。
- **下一步**：注入单 plane 写失败，确认对应 plane 的块当轮标坏、`info bad` 增长、后续不再分配。
- **证据**：`src/ftl/ftl.c:ftl_host_io_write_fail`；`include/fc/fc_command.h:fc_command_status_t`。
- **历史/验证**：
  - 2026-10-21 修复 → 未上板

#### RISK-MIG-10 迟到完成按新 job 的 phase 派发

- **后果**：完成服务只校验 `job.active && waiting`，不区分命令属于哪个 job。超时 abort 后命令仍在 FC 侧，新 job 已 active+waiting 时旧完成到达 ⇒ 若新 job 在 ERASE，把未擦块按擦成功记账 ⇒ 块被重新分配覆盖有效数据。
- **缓解**：`mig_state_s.inflight` 统计已提交未完成命令数（batch 按 n 计），完成回调统一递减；三个 `*_begin` 在 `inflight != 0` 时拒绝启新 job。FC 真丢完成时引擎停摆（写路径 no-space），不会静默回收错误块；`bg_progress` 已把 `inflight != 0` 计入。
- **下一步**：GC 中制造超时/丢完成，确认无错误回收、`free` 与实际块状态一致。
- **证据**：`src/ftl/migrate.c` 四个提交原语 / `ftl_migrate_callback` / `mig_gc_begin|mig_wl_begin|mig_predict_begin`；`include/ftl/migrate.h:inflight`。
- **历史/验证**：
  - 2026-11-04 修复 → 编译通过，未上板

#### RISK-JN-01 Journal 环形块擦除失败无限重挂

- **后果**：Journal 环形块擦除失败原会无限重挂 `need_erase`。
- **缓解**：连续擦失败 3 次 ⇒ 标坏（RAM + BBT 两 plane）并跳到下一可用块；全坏则 `ring_full` 停止写入。
- **下一步**：注入环形块擦失败：Journal 用剩余块继续，重启后 BBT 仍挡住坏块。
- **证据**：`src/ftl/journal.c:JRN_ERASE_FAIL_MAX` / `journal_erase_failed`。
- **历史/验证**：
  - （日期未记录）修复 → 未上板

#### RISK-JN-02 同步刷页返回 void

- **后果**：`jrn_flush_page` / `jrn_flush_now` 返回 void：同步刷页失败会置 `write_fail_pending`，但调用者无法知道「屏障未达成」。
- **缓解**：改为 `ftl_status_e` / `bool`；drain 超时、擦失败、写失败均返回 `FTL_ERROR`，`ftl_flush` 记 `LOGW`。
- **下一步**：注入同步写/擦失败，`fl` 能报错且 pending 阻止 victim 擦。
- **证据**：`src/ftl/journal.c:jrn_flush_page`。
- **历史/验证**：
  - （日期未记录）修复 → 未上板

#### RISK-JN-03 write_fail_pending + CP 清账未注入

- **后果**：页写失败 / 擦失败 / CP 早于失败点三种时序未实测。
- **缓解**：静态路径已审查；UT `journal_write_fail_wal_stall` 覆盖「失败 ⇒ 区域写回被 WAL 闸门挡住 ⇒ 放开后恢复」。
- **下一步**：注入 Journal 页写失败（`fi_journal_fail_arm`），确认 victim 未擦、CP 后恢复。
- **证据**：`src/ftl/journal.c` / `src/ftl/checkpoint.c`。
- **历史/验证**：
  - 2026-10-31 `journal_write_fail_wal_stall` 上板通过 → 注入路径已验

#### RISK-JN-04 drain 超时后仍同步刷页

- **后果**：drain 超时后 `jrn_flush_page` 仍同步刷页；在飞写命令仍拥有当前页地址 ⇒ 双重编程。
- **缓解**：drain 失败 ⇒ `LOGE` + 置 pending + `FTL_ERROR`，不再同步刷；`jrn_flush_now` 入口加 `job.phase == JRN_IDLE` 守卫。
- **下一步**：注入命令丢失/超时，确认不发生同页二次编程、后续可恢复。
- **证据**：`src/ftl/journal.c:jrn_flush_page` / `jrn_flush_now`。
- **历史/验证**：
  - （日期未记录）修复 → 未上板

#### RISK-JN-05 回放失败日志逐条打印

- **后果**：`jrn_page_replay_apply` 对每条落在降级 region 的条目打一行，对每条 L2P 写失败再打一行 LOGE；一次掉电回放 1240 条、热点 region 命中上千次 ⇒ 日志写满 4KB TX 环后阻塞，开机被拖慢。
- **缓解**：改为文件内汇总计数（`s_replay_degraded_entries/_region` 等）；`jrn_replay` 起点清零，结束时每类只打一行（首个 region / 首条 LBA + 条数）。
- **下一步**：掉电回放后确认每类日志只有一行。
- **证据**：`src/ftl/journal.c:jrn_page_replay_apply` / `jrn_replay`。
- **历史/验证**：
  - 2026-10-21 修复 → 未上板

#### RISK-JN-06 回放工作缓冲从栈改 static

- **后果**：`replay_pages[4×64]` = 4 KiB、`jrn_ring_scan` 2108 B，突破本仓 2 KiB 栈预算；`jrn_replay` 整帧 `sub sp,#6208`。
- **缓解**：`s_replay_pages`（4 KiB）+ `s_scan_page`（2 KiB）改为文件级 static（回放只在开机、单线程、无重入）；实测帧降到 92 B / 60 B，`journal.o` BSS 6168 B。
- **下一步**：format→写→POR 回放无 HardFault / 栈哨兵告警；若将来需要并发放回，static 缓冲改传入上下文。
- **证据**：`src/ftl/journal.c:s_replay_pages` / `s_scan_page` / `jrn_replay` / `jrn_ring_scan`；objdump 帧。
- **历史/验证**：
  - 2026-11-04 改 static → 编译通过，未上板

#### RISK-JN-07 jrn_block_next 把 from 自身算作下一块

- **后果**：循环上界 `i <= JOURNAL_BLOCK_COUNT` 使 `(from+COUNT)%COUNT == from` 成为候选；只剩 1 个可用块时返回自身 ⇒ `jrn_freeze` 换块把 `tblock` 定成当前块并置 `need_erase` ⇒ 擦掉刚写满的块；`jrn_resume` / `jrn_truncate_plan` / `jrn_erase_failed` 同样会擦正在写的块。
- **缓解**：循环上界改 `i < JOURNAL_BLOCK_COUNT`；`jrn_erase_failed` 把 `next == current_block` 与 NONE 同样置 `ring_full`。触发需 3 次连续擦失败，正常路径不可达。
- **下一步**：把 3 个环形块标 BAD 后触发换块，确认 journal 停写（`ring_full`）而不是擦当前块。
- **证据**：`src/ftl/journal.c:jrn_block_next` / `jrn_erase_failed` / `jrn_freeze` / `jrn_resume` / `jrn_truncate_plan`。
- **历史/验证**：
  - 2026-11-05 修复 → 编译通过，未上板

#### RISK-JN-08 回放第二遍读失败不上报

- **后果**：`jrn_page_replay_apply` 页读失败只 LOGW 并返回 0；`jrn_replay` 只信第一遍扫描的 `read_errors`。某页扫描时可读、应用时读失败（ECC 临界/瞬态）⇒ 条目被静默跳过且仍返回 `FTL_OK` ⇒ 调用者把不完整回放当完整结果（映射丢失）。
- **缓解**：新增 `s_replay_page_read_fail`，应用遍页读失败即计数；`jrn_replay` 最终判据 = 扫描遍 `read_errors` 或应用遍计数 >0 ⇒ 返回 `FTL_ERROR`（PFR 回退 full rebuild）。
- **下一步**：回放期间对选中页注入读失败，确认日志与 full rebuild 回退。
- **证据**：`src/ftl/journal.c:jrn_page_replay_apply` / `jrn_replay`。
- **历史/验证**：
  - 2026-11-05 修复 → 编译通过，未上板

#### RISK-JN-09 32-bit Journal 序号无回绕处理

- **后果**：`sequence_counter` / `page_seq` / CP `journal_sequence` 全是 DWORD，跨重启单调；按每 LBA 一条、512B 条目折算，累计写 ≈2 TiB 后回绕。回绕后 `jrn_ring_scan` 的 `page_seq + entry_count > cp_sequence` 把新页判成已 checkpoint 而跳过回放；`out_latest` 取 max 会选中回绕前旧页 ⇒ 写指针退回旧位置。
- **缓解**：B 案（串行号比较）：新增 `jrn_seq_after` / `jrn_seq_ge`（RFC 1982 语义）；ring scan latest/回放过滤、resume/truncate/append、L2P WAL 闸门、CP T3 增量、PFR 判型与 gap、metadata 环空哨兵全部改走串行比较；活值窗口上界 ≈13 万 ≪ 2^31。A 案（64-bit）会动 `dirty_seq`/CMT 宽度与介质格式，不采纳。
- **下一步**：回绕无法在板端跑到；需 UT 用合成序号覆盖（测试源由用户维护）。
- **证据**：`src/ftl/journal.c` / `include/ftl/journal.h` / `l2p_cache.c` / `checkpoint.c` / `power_failure_recovery.c` / `metadata.c`。
- **历史/验证**：
  - 2026-11-05 改串行比较 → 编译通过，未上板

#### RISK-JN-10 撕裂 frontier 页重复编程

- **后果**：掉电正在写的 journal 页可能半编程（非空白、CRC 失败）；开机扫描计入 `read_errors` ⇒ `jrn_replay` 返 `FTL_ERROR` ⇒ PFR 回退 full rebuild。该页留在环里直到其块被擦，期间每次开机重复 full rebuild；且原 `jrn_resume` 把写指针落在 `latest+1` = 该撕裂页上，首次写再编程它一次。
- **缓解**：A 案：`jrn_resume` 选写点前先 `ftl_page_is_blank(latest+1)`；非空白（撕裂页）记 WRN 并跳过一页，绝不二次编程；块尾由换块擦除路径处理。不可读页仍按全量重建（与 SPDK 脏关机基线一致）。B 案（块关闭标记）需改介质格式，列入后续版本选项。
- **下一步**：写 journal 期间断电 → 重启，确认 `resume: skip torn page` 且不再重复编程。
- **证据**：`src/ftl/journal.c:jrn_ring_scan` / `jrn_resume`；`src/ftl/power_failure_recovery.c:pfr_boot_plan`。
- **历史/验证**：
  - 2026-11-05 修复 → 编译通过，未上板

#### RISK-RR-02 恢复成功仍报 IO_ERROR

- **后果**：恢复成功后仍以 `SSD_STATUS_IO_ERROR` 完成托管子操作；`recovered` 只是最后一个 plane 的结果。
- **缓解**：新增 `all_ok` 聚合位：job 起点置 true，任一 plane 失败或 abort 清 false；`rr_job_finish` 按它报 OK/IO_ERROR。
- **下一步**：可恢复失败读应返回成功；单 plane 不可恢复整体报错。
- **证据**：`src/ftl/read_recovery.c:rr_job_finish` / `rr_settle_plane`。
- **历史/验证**：
  - （日期未记录）修复 → 未上板

#### RISK-RR-03 ECC 健康状态可降级

- **后果**：`rr_record_ecc` 健康状态可降级：CRITICAL/DYING 块在低 ECC 连续上升时被打回 WARNING。
- **缓解**：改为单调 max 升级。
- **下一步**：CRITICAL 后低 ECC 上升不得回落（UT 负例）。
- **证据**：`src/ftl/read_recovery.c:rr_record_ecc`。
- **历史/验证**：
  - （日期未记录）修复：单调升级 → 未上板

#### RISK-RR-04 预测巡检命中含 BAD plane 的 SB

- **后果**：预测巡检返回的 CLOSED 块所在 SB 可能含 BAD plane ⇒ migrate 反复 abort 卡死。
- **缓解**：`rr_predictive_scan` 命中候选后校验 SB，含 BAD plane 则跳过。
- **下一步**：运行期标坏后预测路径仍能推进。
- **证据**：`src/ftl/read_recovery.c:rr_predictive_scan`。
- **历史/验证**：
  - （日期未记录）修复 → 未上板

#### RISK-RR-05 对无 retry table 的 SLC 做 8 次重试

- **后果**：`SET_FEATURES(0x89)` 在 MT29F4G08 是保留地址（空操作），Stage 1 只是把同一页读 8 遍；Stage 2 读邻居页也无法恢复目标页。坏页读失败要白跑 10 次 NAND 读，校验速度被拖到写速度以下（实测 verify 195 KB/s vs write 290 KB/s）。
- **缓解**：删除 SET_FEATURES 循环、邻居辅助与 4 个相关 stage；改为一次正常读 + 仅 CRC_ERROR 允许一次重读；UNC/NAND/MATA/TIMEOUT 直接失败。已 BAD 块直接结算（RISK-MIG-07）；失败日志带 `pba=/paa=`；新增 CLI `rdpaa <paa> [--raw]`。
- **下一步**：坏页场景观察 verify 速度与 RR 日志。
- **证据**：`src/ftl/read_recovery.c`；`src/hil/cli.c:rdpaa`。
- **历史/验证**：
  - 2026-10-21 修复（用户明确不处理 0x89 空转）→ 未上板

#### RISK-WL-10 静态 WL 快照恢复后从不触发

- **后果**：70% 门限用整 SB 容量（358），而快照恢复出的条目是单 plane（最多 256 CW）⇒ 全被拒，`min_used` 保持 UINT32_MAX ⇒ 本次开机 WL 完全不动，直到积累出足够多的双 plane SB。GC 侧同一概念用 `st.plane_count * FTL_CW_PER_BLOCK`。
- **缓解**：分母改为该条目自身容量 `plane_count × FTL_CW_PER_BLOCK`。
- **下一步**：format → 写满若干干净 SB → 重启（走快照恢复）→ 观察 `kind=wl` 是否仍出现、`max_free−min_used` 收敛。
- **证据**：`src/ftl/wear_leveling.c:wl_candidate_get`；`src/ftl/allocator.c:1082-1111`。
- **历史/验证**：
  - 2026-11-04 修复 → 编译通过，未上板

#### RISK-WL-11 WL 目的准备失败静默退化

- **后果**：`alc_super_prepare_wl_destination` 返回 void；closed 列表满时关不掉 current cold SB，随后 `open_alloc_pair(HIGH_WEAR)` 走「已打开 SB 继续追加」分支 ⇒ 冷数据写进普通块，UBI 方向不成立，搬移变纯写放大。
- **缓解**：接口改返回 `bool`；`mig_wl_begin` 在返回 false 时放弃本次 WL（`alc_super_close_live` 已 LOGE 归因）。
- **下一步**：制造 closed 列表满（或 UT 注入）确认 WL 不启动且无普通块续写。
- **证据**：`src/ftl/allocator.c:alc_super_prepare_wl_destination`；`src/ftl/migrate.c:mig_wl_begin`；`include/ftl/allocator.h`。
- **历史/验证**：
  - 2026-11-04 修复 → 编译通过，未上板

#### RISK-ALLOC-14 运行期标 BAD 让 Closed 条目永久卡死

- **后果**：`bbt_mark_bad` 只清 BST、不动 `closed_sb_list`。Closed SB 的某 plane 块被标 BAD 后，另一 plane 搬完时 finish 按 victim 全 plane 原值查不到条目（已 drop 的 plane 是 NO_PLANE）⇒ 条目留下、`plane_count==0` ⇒ GC/WL 永久跳过 ⇒ 每来一次泄漏一个槽，累计满后关 SB 全部失败、写入报错。
- **缓解**：`bbt_mark_bad` 同步 `alc_super_closed_drop_plane`；条目在最后一个 plane 摘除时由现有 swap-remove 收尾。
- **下一步**：读写中制造可纠/不可纠错误触发 RR 标坏，观察 `closed_sb_count` 不随事件单调增长、关 SB 不报 `list full`。
- **证据**：`src/ftl/bbt.c:bbt_mark_bad`；`src/ftl/allocator.c:alc_super_closed_drop_plane`；`src/ftl/migrate.c:943-956`。
- **历史/验证**：
  - 2026-11-04 修复 → 编译通过，未上板

#### RISK-PERF-01 GC/WL 选块重复全表扫描

- **后果**：`gc_select_victim` 扫 closed 列表并逐条做 per-plane 统计；`wl_should_trigger` 与 `wl_select_victim` 各扫一次（5s 节拍，后者仅在触发后）。`gc_should_trigger` 本身只读 free 计数。
- **缓解**：GC 10ms / WL 5s 节拍门兜成本。
- **下一步**：需要时把 `alc_bst_erase_stats_s` 从求值点透传（注意别把策略耦合进 mig API）。
- **证据**：`src/ftl/gc.c` / `src/ftl/wear_leveling.c`。
- **历史/验证**：
  - （日期未记录）识别为重复扫描 → 待后续

#### RISK-PERF-02 GC/WL 目的页逐 plane 单条写

- **后果**：GC/WL 目的页仍逐 plane 单条写，没有 multi-plane 命令。
- **缓解**：正确性无影响。
- **下一步**：命令形式优化，需 FC 侧支持。
- **证据**：`docs/design/ftl_design.md §7.2`。
- **历史/验证**：
  - （日期未记录）性能项 → 待后续

#### RISK-PERF-03 cache read 生效后读速未提升

- **后果**：`/speed/ftl_rw` cache 档 `rd=1363 KB/s` vs normal `1356 KB/s`（几乎相同），尽管 `rdc batches=1600`、`pairs>0` 表明 4-pair 序列在跑。瓶颈不在命令/启动开销，而在数据面（PIO 逐字节搬运 + 校验）或单 LUN 串行。
- **缓解**：功能正确性不受影响；cache 读仍是有效路径。
- **下一步**：定位数据面：① 统计单 pair 的 `tR` 与 PIO 拷贝耗时占比；② 评估 DMA/更大 FIFO 或提高 FC 批处理深度。与 RISK-PERF-02 一并评估。
- **证据**：板端 2026-10-25 debug/release 数据；`src/platform/stm32h743/nand_dma.c`。
- **历史/验证**：
  - 2026-10-25 板端实测：debug cache 18776 vs normal 18873 ms；release 7598 vs 7598 ms → 信息

#### RISK-PERF-04 内部数据面 CPU-bound

- **后果**：2-plane 页 4096B，按 datasheet 阵列上限 ≈163.8 MB/s 读、20.5 MB/s 写；接口上限 100 MB/s（Mode 5 x16）。release 实测 FC 读 3.6 MB/s（阵列 6%）、FC 写 4.0 MB/s（约 20%）、FTL host 读 3.4 MB/s、写 2.5–2.7 MB/s；debug→release 全线 2.2–2.5× ⇒ 瓶颈在 CPU 侧（PIO 拷贝 + randomizer/CRC + BCH），不是 NAND，也不是总线。
- **缓解**：无（功能正确性不受影响）。
- **下一步**：按收益排序：① PIO → DMA（FC data path）；② BCH/randomizer 硬件化或向量化；③ 减少逐字节 copy。先用单 pair 拆 `tR` 与 CPU 段耗时验证。
- **证据**：板端 2026-10-25 release 00:35 镜像 `/speed/` 4/4；H2testw 415/423 MB；`src/platform/stm32h743/nand_dma.c`；MT29F4G08 datasheet。
- **历史/验证**：
  - 2026-10-31 v1.06 复测：写 894 KB/s、读 888 KB/s（USB FS 59%/59%），序列优化不改变带宽
  - 2026-10-25 release 实测数据 → 信息


### 已闭合（详情见 archive）

| ID | 日期 | 一句话 | 归档 |
|---|---|---|---|
| RISK-GC-07 | 2026-11-12 | 2/3 容量触发 GC/CP 活锁（读成功清 fail_count） | `docs/archive/known_risks_batches.md` |

---

## 2A. BBT（Bad Block Table）

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-BBT-01 | 待上板 | 运行时标坏的幂等 / 持久化 / Spare 记账未板验 | 2026-10-25 上板通过 |
| RISK-BBT-02 | 待上板 | 工厂扫描对列读失败页的判坏行为未实测 | 未上板 |
| RISK-BBT-03 | 待上板 | 格式化擦除与 BBT save 间掉电丢失工厂坏块图 | 未上板 |
| RISK-BBT-04 | 待后续 | BBT header 无布局版本字段，格式演进无法识别 | 未上板 |
| RISK-BBT-05 | 待上板 | 元数据区为空时可能误判空片并重扫 | 未上板 |
| RISK-BBT-06 | 待后续 | BBT 回调不保存 command_id，重复完成可能错配 phase | 2026-10-19 代码核对 |
| RISK-BBT-07 | 待上板 | CP 快照并集的 BAD 不补 Spare 池 | 未上板 |
| RISK-BBT-10 | 待测试源 | kick / factory-scan / format_erase 缺单测 | 2026-10-25 上板通过 |
| RISK-BBT-12 | 待上板 | 固定元数据池无 BAD 跳过/替换 | 未上板 |
| RISK-BBT-13 | 待上板 | 固定元数据分区无法按 BBT 重映射 | 未上板 |
| RISK-BBT-14 | 待上板 | FORMAT_IN_PROGRESS 提交点过早 | 未上板 |
| RISK-BBT-15 | 待上板 | 运行期元数据块变坏，候选池/游标不更新 | 未上板 |
| RISK-BBT-16 | 待上板 | 全片体检可能恢复边缘坏块 | 未上板 |
| RISK-BBT-17 | 待上板 | BBT 候选池判断散落多处，易误擦 / 误留 | 未上板 |
| RISK-BBT-18 | 待上板 | 多圈 BBT ring 新写 record 后 reload 选到上一条 | 2026-10-31 上板通过 |
| RISK-BBT-19 | 待上板 | BBT ring 块擦除失败导致持久化永久 livelock | 未上板 |
| RISK-BBT-20 | 待上板 | 同步 bbt_save 与异步 job / 并发标坏无协议 | 未上板 |
| RISK-BBT-21 | 待上板 | 出厂扫描跳过 Block 0 的器件依据无出处 | 未上板 |

### 详情

#### RISK-BBT-01 运行时标坏的幂等 / 持久化 / Spare 记账未板验

- **后果**：`bbt_mark_bad` 幂等、`dirty_gen` 驱动的持久化、Spare 只减一次这三处行为未经板验。幂等失效（同一块重复标坏）⇒ Spare 被重复扣减；`dirty_gen` 未覆盖写飞行中新增的坏块 ⇒ 重启后坏块图缺项，仍会写坏块。
- **缓解**：clean-pool 的 `bbt_persist` 路径已由板端 UT 覆盖；`bbt_mark_bad` 按 `dirty_gen` 代际推进，`bbt_save` / `bbt_write_complete` 只在代际未变时清 dirty。
- **下一步**：注入擦失败 / RR 标坏后重启，核对 `bad_count` / `spare_count` / BBT 位一致；BBT 写飞行期间再标坏，下一次 save 必须包含新块。
- **证据**：`src/ftl/bbt.c:bbt_mark_bad` / `bbt_write_submit` / `ftl_bbt_callback`；`src/ftl/ftl_op.c` `ftl_bbt_callback` 派发表。
- **历史/验证**：
  - 2026-10-25 板端 `bbt_mark_bad` / `bbt_persist` clean-pool 通过 → 写飞行中再标坏的路径未覆盖
  - （日期未记录）`bbt_mark_bad` 改为幂等 + 引入 `dirty_gen` → 未上板

#### RISK-BBT-02 工厂扫描对列读失败页的判坏行为未实测

- **后果**：`ftl_sync_read_column` 对 NAND 错误返回 `FTL_ERROR`，工厂扫描据此判坏的路径未经板验：漏判 ⇒ 坏块进入分配；误判 ⇒ 好块被标坏、容量流失。还需区分空片出厂坏块标记与 NAND 读失败。
- **缓解**：`bbt_probe_block_bad` 走保守判据——列读非 0xFF 或读失败一律判坏，与擦除失败同调。
- **下一步**：注入列读失败，确认判坏而不是误判好块；同时核对空片出厂标记与 NAND 失败的区分。
- **证据**：`src/ftl/ftl_fc_interface.c:ftl_sync_read_column`；`src/ftl/bbt.c:bbt_probe_block_bad`。
- **历史/验证**：
  - （日期未记录）`ftl_sync_read_column` 改为对 NAND 错误返回 `FTL_ERROR` → 未上板

#### RISK-BBT-03 格式化擦除与 BBT save 间掉电丢失工厂坏块图

- **后果**：两阶段格式化中擦除与 BBT save 的次序被掉电打断时，工厂坏块图可能丢失 ⇒ 坏块进入分配，写入失败或数据丢失。
- **缓解**：`bbt_format_erase` 两阶段：① 置 `FORMAT_IN_PROGRESS` 并 save，失败即中止（不擦用户块）；② 擦除 BBT 池**之外**的块；③ best-effort save 并入新 BAD。BBT 池旧 record 保留，新 record 以更高 sequence 胜出。
- **下一步**：格式化过程中掉电注入（save 后 / erase 中 / CP marker 前），重启后 BBT 仍含工厂坏块图。
- **证据**：`src/ftl/bbt.c:bbt_format_erase`；`src/ftl/power_failure_recovery.c:pfr_media_format`。
- **历史/验证**：
  - （日期未记录）改为两阶段 `bbt_format_erase`（置标志并 save → 擦池外块 → 并入新 BAD）→ 未上板

#### RISK-BBT-04 BBT header 无布局版本字段

- **后果**：header 只靠 `total_blocks` 作版本代理：块数相同而 bitmap 语义变化的格式演进无法识别 ⇒ 新旧格式互读会把坏块图解释错。
- **缓解**：`bbt_header_valid` 校验 `total_blocks == FTL_BLOCK_TOTAL`。
- **下一步**：下次 BBT 格式变更时用 `reserved[]` 加版本（属介质格式变更，需 bump `FTL_MEDIA_LAYOUT_VERSION`）。
- **证据**：`include/ftl/bbt.h:bbt_header_s`；`src/ftl/bbt.c:bbt_header_valid`。
- **历史/验证**：
  - （日期未记录）登记：header 无版本字段，仅 `total_blocks` 代理 → 待后续

#### RISK-BBT-05 元数据区为空时可能误判空片并重扫

- **后果**：固定元数据区为空时，若只看元数据区会把仍带用户数据的介质判成空片并重跑出厂扫描 ⇒ 原有坏块图被覆盖。
- **缓解**：`bbt_medium_has_ftl_data` 抽样用户区 Page 0 的 META（`BBT_USER_SAMPLE_COUNT = 8` 点/plane）判是否被写过。⚠ 抽样是概率判据：数据全部落在抽样点之外仍会误判空片。
- **下一步**：擦掉元数据区但保留用户数据，`bbt_init` 必须拒绝出厂扫描。
- **证据**：`src/ftl/bbt.c:bbt_medium_has_ftl_data` / `BBT_USER_SAMPLE_COUNT`。
- **历史/验证**：
  - （日期未记录）改为抽样用户区 META 判空（8 点/plane）→ 未上板

#### RISK-BBT-06 BBT 回调不保存 command_id

- **后果**：`ftl_bbt_callback` 不保存 in-flight `command_id`；擦完成与写完成被重复投递时 phase 会错配，用错的完成推进写游标 / 清 dirty。
- **缓解**：`waiting == 0` 的孤儿守卫，只挡 job 结束后的迟到完成。
- **下一步**：FC 侧 `fc_complete_internal` 没有「已 COMPLETED 就不重复入队」守卫，重复完成取决于 NAND 是否重复上报；当前 NAND 是单次轮询完成模型、无重复上报源 ⇒ 暂不修。换 NAND 引擎 / 加中断完成前**必须先补该守卫**。
- **证据**：`src/ftl/bbt.c:ftl_bbt_callback`；`src/fc/fc.c:fc_complete_internal`。
- **历史/验证**：
  - 2026-10-19 代码核对 `fc_complete_internal`（`fc.c:712-724`）→ 无重复入队守卫；当前无重复上报源，暂不修

#### RISK-BBT-07 CP 快照并集的 BAD 不补 Spare 池

- **后果**：boot 期 CP 快照并集得到的 BAD 不会回填 Spare 池 ⇒ 恢复后 `spare_count` 低于每 plane 目标，坏块替换余量不足。
- **缓解**：`alc_spare_refill_from_free(alloc, plane)` 摘 Free → state 置 NONE → push Spare，直到 `FTL_SPARE_POOL_PER_PLANE` 或无同 plane 空闲块；三条启动路径（clean 快照 / clean full metadata rebuild / PF full metadata rebuild）建池后逐 plane 补池并打印补后计数。
- **下一步**：制造 CP 快照含 BAD、Spare 不足的介质，确认补回 50/plane；Free 不足时补后计数自然下降。
- **证据**：`src/ftl/allocator.c:alc_spare_refill_from_free`；`src/ftl/power_failure_recovery.c:pfr_refill_spare_pools`。
- **历史/验证**：
  - （日期未记录）新增补池函数并接入三条启动路径 → 未上板

#### RISK-BBT-10 kick / factory-scan / format_erase 缺单测

- **后果**：`kick` / `scan_factory` / `format_erase` 三条路径没有单测 ⇒ 这些路径的回归没有自动判据。
- **缓解**：`mark_bad` 幂等 / Spare 一次、元数据块丢失标记、clean-pool save→load 已由 `pure/mem/bbt_*` 与 `media/e2e/bbt_persist` 覆盖；`FORMAT_IN_PROGRESS` 恢复由 `format_in_progress_resume` 覆盖；多圈 ring 定位问题见 RISK-BBT-18。
- **下一步**：补 factory-scan / format-in-progress 注入用例（测试源由用户维护，R4）。
- **证据**：`tests/unit/test_ftl_pure.c` / `tests/unit/test_ftl_media.c`；`docs/design/ftl_ut_design.md` §4.1。
- **历史/验证**：
  - 2026-10-25 `format_in_progress_resume` 覆盖 `FORMAT_IN_PROGRESS` 恢复 → 上板通过
  - （日期未记录）BBT 用例入库 → `kick` / `scan_factory` / `format_erase` 仍未覆盖

#### RISK-BBT-17 BBT 候选池判断散落多处

- **后果**：候选池判定散落在多个调用点（`bbt.c` 与 `power_failure_recovery.c` 两处）：只改阈值或 ring 语义的一处，就会误擦 / 误留 BBT 候选池 ⇒ BBT record 丢失或启动恢复失败。
- **缓解**：统一为 `bbt_block_is_candidate()`（`bbt.h` 纯 inline，判据 `ring != NULL && block_count >= 2 ? md_ring_has_block(...) : (block in BBT_POOL)`）；`borrowed_log` 拆为 `is_l2p_live` / `is_current_log`。
- **下一步**：上板 `format --verify` + 重启 BBT 定位；BBT ring 语义变化时只改 helper。
- **证据**：`src/ftl/bbt.c:502-504`；`src/ftl/power_failure_recovery.c:242-244,302-305`；`include/ftl/bbt.h:bbt_block_is_candidate`。
- **历史/验证**：
  - 2026-10-23 抽出 `bbt_block_is_candidate()`、三处收敛并拆分 `borrowed_log` → 未上板

#### RISK-BBT-18 多圈 BBT ring 新写 record 后 reload 选到上一条

- **后果**：多圈 BBT ring 上新写 record 后 `bbt_load` / `bbt_init` 返回上一条（板端 seq=1383 / write_idx=103，save 写 seq=1384 返回 OK 后 reload 仍 1383、count=0；同日 `FORMAT_IN_PROGRESS` record 同样读不回）。根因在共用 `md_ring_scan`：它假设「块满 ⟺ 末槽有效」「**第一个**末槽无效的块 = 写 frontier」。`bbt_load` 在候选池尚未建立时用引导环（名义 BBT 块 + 全部 shared spare，**含 BAD 块**，见 `bbt_ring_init`）；某个 BAD 块末页读失败时它被当成 cursor 块，descending 扫描一无所获 ⇒ `best_full` 退化为某块末槽的旧 record，真正最新记录（在后续块中段）被忽略。随后 save 用旧 sequence / 游标写（可能落非空白槽 ⇒ 二次编程）⇒ 下次 load 仍选旧值；掉电后坏块图回退，运行期坏块可能被写入，`FORMAT_IN_PROGRESS` 丢失形态见 RISK-BOOT-22。
- **缓解**：`md_ring_scan` 把所有「末槽无效」块都作为 cursor 候选，各自 descending 找最后有效 record，取**全局最大 sequence**；写点由**已校验的**胜者推（末槽 record ⇒ 下一块槽 0；中段 record ⇒ idx+1 并保留 skip；池空 ⇒ 第一个空块槽 0）。运行期每次标坏仍会再写新 record；`pfr_restore_blocks` 把 CP 块元数据快照的 BAD 与 BBT 并集，最后一次 CP 晚于坏块标记时可恢复丢失的 BAD 位。残余：部分擦除块的末槽仍可能「有效但属于上一圈」，完全消除需按 first/last seq 关系或全槽扫描。
- **下一步**：上板判据 = 多圈 + 一个读失败 BAD 块后 save→load 必须载入最新；clean / 多圈两条 UT 都要过。
- **证据**：`src/ftl/metadata.c:md_ring_scan` / `md_ring_locate`；`src/ftl/bbt.c:bbt_ring_init` / `bbt_record_probe` / `bbt_load` / `bbt_save`；`tests/unit/test_ftl_media.c`。
- **历史/验证**：
  - 2026-11-05 `md_ring_scan` 改为「末槽无效块各自降序找最后有效 record + 全局最大 sequence」，写点由已校验胜者推 → 逻辑模型 BBT-18 场景返回 seq=1103、多圈重启模型 500 次无回归；未上板
  - 2026-10-31 板端 UT `bbt_ring_reload` 两次 save 场景通过 → 多圈未覆盖
  - 2026-10-24 板端 `test /ftl/media/e2e/bbt_persist`：seq 1383→1384、reload 1383；同日 21:29 `format_in_progress_resume` 也读不回 → 复现
  - （日期未记录）按 `md_ring_scan` 逐字建模复现（非板端）：引导环 [N0(BAD/不可读), N1, S1, S2]、N1 末槽 seq=1063、S1 page38 seq=1103 ⇒ locate 返回 1063

#### RISK-BBT-19 BBT ring 块擦除失败导致持久化 livelock

- **后果**：`bbt_handle` 换块时提交异步擦；`bbt_erase_complete` 擦失败只回 `BBT_JOB_IDLE`，不推进 write_idx、不标坏、不改候选池 ⇒ 下一次 tick 又对同一块 prepare + 擦，永远重试；`dirty` 永不落盘 ⇒ 运行期标坏的块重启后丢失，旧坏块图被使用。同步 `bbt_save` 的 `md_ring_prepare_write` 同样：擦失败返回 ERROR、游标不动。失败场景：BBT ring 块磨损到不可擦（运行期坏块的正常来源）。
- **缓解**：擦失败时 `bbt_ring_block_mark_bad` 按 plane_mask 逐 plane `bbt_mark_bad`（置 `pools->dirty`，由 `md_pool_service` 重建候选池并作废游标），下一次 job 在新池上重新定位；重建凑不齐时置 `rebuild_required` 并 LOGE（可见降级，不是静默 busy loop）。同步路径仍靠后台 job 兜底；每次重试有 `ring block erase failed` LOGE。
- **下一步**：注入一次 BBT ring 擦失败，BBT 必须在有界时间内改写到另一候选块并落盘。
- **证据**：`src/ftl/bbt.c:bbt_handle` / `bbt_erase_complete` / `bbt_save`；`src/ftl/metadata.c:md_ring_prepare_addr` / `md_ring_prepare_write` / `md_pool_build_all`。
- **历史/验证**：
  - 2026-11-05 擦失败改为标坏 ring 块 + 重建候选池（失败置 `rebuild_required`）→ 未上板

#### RISK-BBT-20 同步 bbt_save 与异步 job / 并发标坏无协议

- **后果**：① `bbt_save` 不检查 `alloc->bbt.handle.waiting`；CLI `format` 只置 `bg_suspend`、不停在飞命令 ⇒ `pfr_media_format` 首个 `bbt_save` 可能与在飞 BBT 写**同槽二次编程**（两条写路径共用 `write_idx`），且同步泵抽干在飞完成 ⇒ `bbt_write_complete` 中途推进游标。格式化期间的 BBT record（含 `FORMAT_IN_PROGRESS`）被 AND 损坏 ⇒ 掉电后按 RISK-BOOT-22 形态误判。② `bbt_save` 成功后**无条件**清 `dirty`，而异步路径用 `dirty_gen` 比对；同步写的嵌套泵里若发生 `bbt_mark_bad`，新坏块不在本条 record 却把 dirty 清了 ⇒ 该坏块不落盘（直到下一次标坏或 CP 并集）。
- **缓解**：`bbt_save` 入口 `ASSERT(handle.waiting == false)` + 运行期拒绝（返回 `FTL_ERROR`）；CLI `format` 在 `pfr_media_format` 前、`bbt --erase` 在全片擦除前，用有界 `ftl_service_pump` 等到 BBT idle（1000 次上限，超时才让 format 失败）；`bbt_save` 建 header 前快照 `dirty_gen`，写成功仅当代际未变才清 dirty（与 `bbt_write_complete` 对称）。启动路径（`main.c:390`）没有在飞 job，只有运行时 CLI `format` 暴露 ①（约 1 ms 窗口）。
- **下一步**：上板让 CLI `format` 与在飞 BBT 写重叠，确认无同槽二次编程、dirty 不被误清。
- **证据**：`src/ftl/bbt.c:bbt_save` / `bbt_write_complete`；`src/ftl/power_failure_recovery.c:pfr_media_format` / `bbt_format_erase`；`src/hil/cli.c:uart_cli_format`；同表 RISK-BOOT-22。
- **历史/验证**：
  - 2026-11-05 加 `waiting` 守卫 + 运行期拒绝、CLI 等 BBT idle、`dirty_gen` 快照 → 未上板

#### RISK-BBT-21 出厂扫描跳过 Block 0 的器件依据无出处

- **后果**：`bbt_factory_scan` 跳过 Block 0 依赖「出厂总是好块」的器件断言，`docs/reference` 与头文件注释都没有该条保证的定位（R12）。若 Block 0 实际是出厂坏块，它属于目录检查点池（`DIR_CKPT_BLOCK0 = 0`），会被写入并在运行中损坏。
- **缓解**：去掉跳过——`bbt_factory_scan` 逐块扫描（含 Block 0）；若 Block 0 有出厂坏块标记，会像其它块一样被标坏并由 shared spare 补位。
- **下一步**：空片首启出厂扫描应把 Block 0 一并探测（多付 1 次列读）。
- **证据**：`src/ftl/bbt.c:bbt_factory_scan`；`include/ftl/metadata.h:DIR_CKPT_BLOCK0`；`docs/reference/README.md`。
- **历史/验证**：
  - 2026-11-05 裁决去掉 Block 0 跳过、改逐块扫描 → 未上板


#### RISK-BBT-12 固定元数据池无 BAD 跳过/替换

- **后果**：`md_ring_addr_plane` 按 record_idx 直接算块号，不检查 BST BAD；Anchor / Checkpoint / BBT 三个池都是如此。固定池内某块变 BAD 时只能靠 P0/P1 镜像；若两个 plane 同号块都坏，该 record 槽永久不可用，而 ring 仍会反复选中它。
- **缓解**：v0.86：`md_ring_t` 改为候选块表（`blocks[]`），`md_pool_build_all()` 从名义区间挑好块。
- **下一步**：上板注入固定池块 BAD，确认 ring 跳过并重建候选表。
- **证据**：`src/ftl/metadata.c` `md_ring_t` / `md_pool_build_all`。
- **历史/验证**：
  - 2026-10-21 v0.86 实现 → 未上板

#### RISK-BBT-13 固定元数据分区无法按 BBT 重映射

- **后果**：Anchor / Checkpoint / BBT / Journal / L2P 日志区的物理位置是编译期几何；BAD 落在这些分区时只能靠镜像（Anchor/CP 的 P0+P1）或跳过（Journal / L2P 日志有跳过；Anchor / CP / BBT 没有）。真正按好块分配需要一个分区重映射表。
- **缓解**：v0.86 按主流实现，不做 remap 表；靠候选块表 + 镜像兜底。
- **下一步**：若分区内坏块导致不可用，再评估重映射表（需介质格式变更）。
- **证据**：`src/ftl/metadata.c`；`docs/design/ftl_design.md`。
- **历史/验证**：
  - 2026-10-21 设计判定：不做 remap 表 → 未上板

#### RISK-BBT-14 FORMAT_IN_PROGRESS 提交点过早

- **后果**：`bbt_format_erase` 擦完就清掉标志，而 `format --verify` 的全片擦除体检和 initial Anchor/CP 写入都在其后。中途复位/烧录 ⇒ 介质停在「有 BBT、无 CP」，`cp_media_status_probe` 返回 IO_ERROR ⇒ 开机拒绝格式化，只能 CLI `format` 恢复。
- **缓解**：标志保持到 initial CP 读回成功（`pfr_format_end_bbt`）；显式失败路径也保持置位，下次开机重试。
- **下一步**：上板在 save 后 / erase 中 / CP marker 前各断电一次。
- **证据**：`src/ftl/bbt.c:bbt_format_erase`；`src/ftl/power_failure_recovery.c:pfr_format_end_bbt`。
- **历史/验证**：
  - 2026-10-21 实机复现；修复后标志保持到 initial CP 读回成功 → 未上板

#### RISK-BBT-15 运行期元数据块变坏

- **后果**：`bbt_mark_bad` 只置 BAD，Anchor/CP/BBT/Journal 的候选表与游标不会自动更新；继续写会打到坏块。
- **缓解**：`bbt_mark_bad` 对元数据块只置 `pools->dirty`（不弹用户 Spare）；`md_pool_service` 在 CP/Journal/BBT 空闲时重建候选池、作废各游标；Journal 候选变化时 `jrn_page_scan_latest + jrn_resume` 接回实际写点；Journal/L2P 失块置 `REBUILD_REQUIRED`，新 CP 落盘后清除。⚠ 全量重建只恢复块状态/池，不保证重建已丢失的 L2P 条目。
- **下一步**：上板运行期标坏元数据块，确认候选池重建、写点接回。
- **证据**：`src/ftl/bbt.c:bbt_mark_bad`；`src/ftl/metadata.c:md_pool_service`；`src/ftl/journal.c`。
- **历史/验证**：
  - 2026-10-21 v0.86 实现 → 未上板

#### RISK-BBT-16 全片体检可能恢复边缘坏块

- **后果**：`format --verify` 从头扫描时清空非 BBT 池的旧 BAD 集，原先标记 BAD 的块只做一遍擦除就判好；擦得动但编不进去/读不出的边缘块会被当好块重新分配。
- **缓解**：v0.88 判据改为擦除成功；编程/读失败由运行期写重试 + RR 标坏兜底；`format --verify` 打印 `old_bad/recovered/new_bad` 判断恢复量。
- **下一步**：上板看 `old_bad/recovered/new_bad` 是否合理；若 recovered 异常高，收紧判据。
- **证据**：`src/ftl/power_failure_recovery.c` `format --verify`；`src/ftl/bbt.c`。
- **历史/验证**：
  - v0.88 判据改为擦除成功 → 未上板



### 已闭合（详情见 archive）

| ID | 日期 | 一句话 | 归档 |
|---|---|---|---|
| RISK-BBT-11 | 2026-11-01 | 二手片出厂坏块标记丢失，无可信 BBT | `docs/archive/known_risks_batches.md` |

---
## 2B. 掉电恢复 / 启动路径

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-BOOT-02 | 待上板 | carve 先于 full rebuild，写过块同时是池成员与 CLOSED | 2026-10-25 UT-P 上板；物理注入未做 |
| RISK-BOOT-03 | 待上板 | CP 读失败被当「需要格式化」，一次瞬时读错擦整片 | 部分上板（2026-10-24） |
| RISK-BOOT-04 | 待上板 | 两阶段格式化中途掉电，重启无法判断未完成 | 部分上板（2026-10-25） |
| RISK-BOOT-05 | 待上板 | 快照恢复的校验/计数改动未板验 | 2026-11-12 常规回归通过（boot_restore_reconcile UT）；残余：非法 word/越界 PBA 注入 |
| RISK-BOOT-06 | 待上板 | full rebuild 按第一个空白页当写指针，hole 后漏计 | 未上板 |
| RISK-BOOT-07 | 待上板 | 判空依赖 raw USER 区，加扰后不可靠 | 未上板 |
| RISK-BOOT-08 | 待上板 | Journal 扫描/回放读失败被当无更多条目 | 未上板 |
| RISK-BOOT-09 | 待上板 | md_load 失败静默新建 Directory，旧映射整体丢失 | 未上板 |
| RISK-BOOT-10 | 待上板 | jrn_resume 忽略跨块擦除结果，首次写落未擦页 | 未上板 |
| RISK-BOOT-11 | 待上板 | 快照 probe L2P 反查失败只 LOGW，仍声称 restored | 未上板 |
| RISK-BOOT-13 | 待定位 | format 后未重启就写，CLI 永久 busy（旧镜像残留） | 2026-10-24 POR 后无记录 |
| RISK-BOOT-14 | 待上板 | full rebuild 空闲块扫描从 64 页降到 1 页 | 未上板 |
| RISK-BOOT-15 | 待上板 | full rebuild → 下次 CP 快照失效 → 每次开机 rebuild | 2026-10-20 L0 |
| RISK-BOOT-16 | 待上板 | full rebuild 逐 CW/逐 region 日志刷屏拖慢启动 | 2026-10-21 修复 |
| RISK-BOOT-17 | 待决策 | 空片首启可能拒绝自动格式化 | 代码路径推断，未实测 |
| RISK-BOOT-18 | 待上板 | 单个 L2P region 不可读 ⇒ 全局回退全盘重建 | v0.90 范围化 |
| RISK-BOOT-21 | 待上板 | 降级位图被误当成「禁止加载」开关 | v0.90 |
| RISK-BOOT-22 | 待定位 | 格式化中断后可能拒绝格式化/启动 | 2026-10-24 复现 |
| RISK-BOOT-23 | 信息 | 非正常复位后最新 CP 双 plane 读失败 ⇒ 全量 rebuild | 2026-10-30 对照 |
| RISK-BOOT-24 | 信息 | anomalies≠0 不是缺陷（池块有数据的正常对账） | 2026-10-31 上板 |
| RISK-BOOT-25 | 待定位 | 重启后整段覆盖写触发大量 dangling | 2026-11-01 复现 |
| RISK-BOOT-26 | 待上板 | 恢复统计 WORD 回绕导致快照被错误接受 | 2026-11-04 编译通过 |
| RISK-BOOT-28 | 待上板 | block_words 隐式 in/out，恢复顺序只靠注释 | 2026-11-04 编译通过 |
| RISK-MD-14 | 待上板 | 双副本账本/Anchor 原子性 | v0.90 |
| RISK-MD-15 | 待上板 | 副本感知压缩（主/备逐副本搬移） | v0.90 |
| RISK-MD-16 | 待上板 | victim region 页读失败 ⇒ 压缩每轮同页中止 | 2026-11-04 编译通过 |

### 详情

#### RISK-BOOT-02 carve 先于 full rebuild

- **后果**：carve 先于 full metadata rebuild ⇒ 写过块同时是池成员与 CLOSED 条目 ⇒ 活数据可被复用。
- **缓解**：三条路径统一 full metadata rebuild / 快照 → carve；`pfr_boot_carve_pools` 只把 `state == NONE` 的块入池，其余只计入 `total_blocks`。
- **下一步**：预置数据在旧 carve 会选中的块上，走 full rebuild fallback 后验证不在池。
- **证据**：`src/ftl/power_failure_recovery.c`。
- **历史/验证**：
  - 2026-10-25 `md_abort_keeps_victim` RAM 语义上板；物理注入未做

#### RISK-BOOT-03 CP 读失败被当需要格式化

- **后果**：一次瞬时读错会导致整片擦除。
- **缓解**：`cp_media_status_probe` 区分 `CURRENT / NO_RECORD / VERSION_MISMATCH / IO_ERROR`；`pfr_media_check_format` 对 IO_ERROR 返回 `FTL_ERROR`，`main` 拒绝格式化并停止启动。UT `cp_corrupt_refuse_format` 已上板。
- **下一步**：VERSION_MISMATCH 分支补单测；真读失败注入仍无 seam。
- **证据**：`src/ftl/checkpoint.c`；`src/main.c`；`tests/unit/test_ftl_media.c:test_cp_corrupt_refuse_format`。
- **历史/验证**：
  - 2026-10-24 IO_ERROR 分支由 UT 覆盖 → 部分上板

#### RISK-BOOT-04 两阶段格式化中途掉电

- **后果**：重启不能判断「格式化未完成」。
- **缓解**：`FORMAT_IN_PROGRESS` 写入 `bbt_header_s.reserved[0]`，`bbt_load` 恢复；`cp_media_status_probe` 见该标志返回 NO_RECORD ⇒ 继续格式化。
- **下一步**：save 后 / erase 中 / CP marker 前各断电一次。
- **证据**：`src/ftl/bbt.c`；`src/ftl/checkpoint.c`。
- **历史/验证**：
  - 2026-10-25 `format_in_progress_resume` 覆盖「save 后」断电点；erase 中/CP marker 前未做 → 部分上板

#### RISK-BOOT-05 快照恢复校验/计数未板验

- **后果**：restore 第 1 遍全量校验（不再部分应用 BAD）、`recovery_materialize_pba` 校验 ch/ce/lun、`recovery_add_closed` 传播列表满、`valid_cw` 改 `uint32_t`，均未板验。
- **缓解**：编译零告警；失败路径回退 full metadata rebuild。
- **下一步**：快照中途注入非法 word；journal 注入越界 PBA；满盘重建确认 `valid_cw` 不截断。
- **证据**：`src/ftl/power_failure_recovery.c`。
- **历史/验证**：
  - （日期未记录）上述改动 → 未上板

#### RISK-BOOT-06 full rebuild 写指针漏计 hole

- **后果**：full rebuild 原按第一个空白页当写指针，hole 之后的页漏计 valid_count。
- **缓解**：`pfr_scan_write_pointer` 检测「空白页后仍有写过页」并返回全块。
- **下一步**：造 page 0 空洞、后续页有数据：valid_count 不少算。
- **证据**：`src/ftl/power_failure_recovery.c:pfr_scan_write_pointer`。
- **历史/验证**：
  - （日期未记录）修复 → 未上板

#### RISK-BOOT-07 判空依赖 raw USER 区

- **后果**：原判空依赖 raw USER 区，DATA 页加扰后不可靠。
- **缓解**：改看每 CW 的 META（4B 明文）；`RANDOM_DISABLE` 不影响。
- **下一步**：空片/写满块判空仍正确。
- **证据**：`src/ftl/ftl_fc_interface.c:ftl_page_is_blank_raw`（后续由 RISK-IF-15 取代）。
- **历史/验证**：
  - （日期未记录）修复 → 未上板

#### RISK-BOOT-08 Journal 扫描/回放读失败

- **后果**：Journal 扫描/回放读失败被当作「无更多条目」，可能少回放。
- **缓解**：`jrn_page_scan_latest(..., out_read_error)` / `jrn_replay` 返回 `FTL_ERROR`，启动回退 full metadata rebuild。
- **下一步**：注入 journal 页读失败：不复用「无更多条目」。
- **证据**：`src/ftl/journal.c:jrn_page_scan_latest` / `jrn_replay`。
- **历史/验证**：
  - （日期未记录）修复 → 未上板

#### RISK-BOOT-09 md_load 失败静默新建 Directory

- **后果**：`md_load` 失败原静默新建 Directory，旧 region 映射可能整体丢失。
- **缓解**：Directory 加载失败不再静默建新表：介质有有效 CP 时置 `ftl->md_load_failed` 并拒绝启动。
- **下一步**：Anchor 损坏 + 有效 CP：启动停止并 LOGE。
- **证据**：`src/ftl/metadata.c:md_load`；`src/ftl/power_failure_recovery.c:pfr_boot_recovery`。
- **历史/验证**：
  - （日期未记录）修复 → 未上板

#### RISK-BOOT-10 jrn_resume 忽略跨块擦除结果

- **后果**：擦失败后首次写会落在未擦页。
- **缓解**：擦失败 ⇒ `LOGE` + 重新挂起 `job.need_erase`；`jrn_handle` / `flush_now` 先补擦，`jrn_freeze` 在 `need_erase != 0` 时拒绝写。
- **下一步**：注入 resume 跨块擦失败，首次写前必须补擦成功。
- **证据**：`src/ftl/journal.c:jrn_resume`。
- **历史/验证**：
  - （日期未记录）修复 → 未上板

#### RISK-BOOT-11 快照 probe 反查失败只 LOGW

- **后果**：valid_count 少算仍声称 `restored`。
- **缓解**：读失败计数向上传播；非 0 ⇒ 快照结果作废、回退 full metadata rebuild。
- **下一步**：注入 L2P 读失败：不声称 `blocks_restored=1`。
- **证据**：`src/ftl/power_failure_recovery.c:pfr_probe_blocks`。
- **历史/验证**：
  - （日期未记录）修复 → 未上板

#### RISK-BOOT-13 format 后未重启就写

- **后果**：`format` 完成后未 `POWER-CYCLE NOW` 就下发 `wr`：曾出现 CLI 请求永久 busy；`.noinit` 记录显示 HardFault（PC 落 CLI `g_wr_buf` 内）。但一次 POR 后记录消失，可能是旧会话残留；本次 `wr` 返回 `status=-3`，未再挂死。
- **缓解**：`format` 已打印 `POWER-CYCLE NOW`；未做运行期拒绝。
- **下一步**：继续正常读写/断电回归；若 `previous crash` 再现，按当场镜像符号定位；考虑 format 后置 FTL 为不可提交直到重启。
- **证据**：rev25 日志；`arm-none-eabi-nm g_wr_buf=0x240002dc`。
- **历史/验证**：
  - 2026-10-24 21:14 真实断电后启动无 `previous crash` 行 ⇒ 旧镜像残留

#### RISK-BOOT-14 full rebuild 空闲块扫描优化

- **后果**：原逐页扫满 64 页（4096 块 × 64 ≈ 26 万次页读），是 rebuild 慢的主因。
- **缓解**：改为 page 0 的 4 个 CW META 全 FF ⇒ 整块空白，空闲块从 64 次读降到 1 次。假设：写模型顺序追加，page 0 编程失败会关闭 SB、不会继续写后续页。
- **下一步**：clean/PF/fresh 三条路径；部分写块 `partial`/`valid_cw` 与优化前一致；若引入续写，恢复全块扫描。
- **证据**：`src/ftl/power_failure_recovery.c:pfr_scan_write_pointer`；`docs/design/ftl_design.md §3.1.5`。
- **历史/验证**：
  - （日期未记录）性能优化 → 未上板

#### RISK-BOOT-15 每次开机都 full rebuild

- **后果**：`pfr_rebuild_closed_sb_list` 把动态元数据 LOG 块直接 `continue`，`pfr_boot_carve_pools` 也跳过 ⇒ 这些块 BST state 停在 NONE 且不属于任何池；`cp_block_snapshot` 写成 `NONE + no pool tag` ⇒ 下次开机 `pfr_restore_blocks` Pass 0 拒绝整份快照 ⇒ 又走 full rebuild，再次留下同样状态。空片 ~0.5s；312 MiB 介质实测 2–4 分钟。
- **缓解**：动态元数据块归 metadata_dir 所有，CARVE 时需要专门处理（见修复）。
- **下一步**：clean/PF 启动路径验证不再循环 rebuild。
- **证据**：`src/ftl/power_failure_recovery.c:pfr_rebuild_closed_sb_list` / `pfr_boot_carve_pools` / `pfr_restore_blocks`；`src/ftl/checkpoint.c:cp_block_snapshot`。
- **历史/验证**：
  - 2026-10-20 L0 修复 → 未上板

#### RISK-BOOT-16 full rebuild 日志刷屏

- **后果**：`pfr_close_block` 对每个降级/读失败 CW 打一行，介质多处损坏时一次重建可产生数千行，写满 4KB TX 环后阻塞；重建完成前 CLI 不可用。
- **缓解**：扫描级汇总（`s_rebuild_read_fail_cw / s_rebuild_degraded_cw / s_rebuild_first_region`），结束时每类一行；L2P 降级 LOGE 改 5s 限流 + suppressed 计数。
- **下一步**：多处 region 降级时重建日志 ≤3 行。
- **证据**：`src/ftl/power_failure_recovery.c`；`src/ftl/l2p_cache.c:l2p_degrade_log`。
- **历史/验证**：
  - 2026-10-21 修复 → 未上板

#### RISK-BOOT-17 空片首启可能拒绝自动格式化

- **后果**：`bbt_init` 在空片上出厂扫描后 `bbt_factory_scan` 会 `bbt_save` 写一条 BBT record；随后 `cp_media_status_probe` 在「CP 池全 FF」分支调用 `bbt_medium_has_ftl_data()`，探到这条合法 BBT 页 ⇒ 返回 true ⇒ 判成 `CP_MEDIA_IO_ERROR` 并拒绝格式化。未在真空白片上实测。
- **缓解**：无；空片可先 CLI `format`（绕过 `pfr_media_check_format`）。不要直接删 `bbt_factory_scan` 的 `bbt_save`：两阶段格式化前置 BAD 图依赖它。
- **下一步**：真空白片/擦净介质确认；方案 A = 出厂扫描只置 dirty 不 save；方案 B = probe 区分「本次 boot 刚扫出的 BBT」。
- **证据**：`src/ftl/bbt.c:bbt_factory_scan` / `bbt_medium_has_ftl_data`；`src/ftl/checkpoint.c:cp_media_status_probe`。
- **历史/验证**：
  - 2026-10-21 读码发现 → 待决策

#### RISK-BOOT-18 单 region 不可读 ⇒ 全局重建

- **后果**：`[L2P] region N load failed 3 times — degrade to unmapped` ⇒ Journal 回放命中降级基线 ⇒ `block_ctx.ok=0`（全局布尔）⇒ 整份块元数据快照被丢弃，full metadata rebuild 扫描全片（312 MiB 实测 2–4 分钟）。
- **缓解**：v0.90 范围化 + L2 region 副本；本仓缺器件侧 read-retry、映射表冗余/parity、范围化恢复三项主流手段。
- **下一步**：上板验证范围化恢复：单 region 不可读不再触发全盘重建。
- **证据**：`src/ftl/power_failure_recovery.c`；`src/ftl/l2p_cache.c`。
- **历史/验证**：
  - v0.90 范围化 → 未上板

#### RISK-BOOT-21 降级位图被误当成禁止加载

- **后果**：`l2p_load_start` 命中 `region_degraded_bitmap` 时直接合成全 INVALID 行、不读 Directory。region 被 journal 回放 / host 写入恢复并 flush 到新页后，行被驱逐再重载会再次合成 INVALID ⇒ 已恢复的映射读不到；GC 迁移反查也会把这些 CW 当无效 ⇒ 可能漏搬、随后擦除有效数据。
- **缓解**：删除短路，加载一律走 Directory（主→备）；位图只用于「未映射的 LBA 报 IO_ERROR」判定。
- **下一步**：制造降级 region → 写入并 flush → cache 压力驱逐 → 重读应命中且 `map`/`rd` 不报错。
- **证据**：`src/ftl/l2p_cache.c:l2p_load_start` / `l2p_is_region_degraded`。
- **历史/验证**：
  - v0.90 修复 → 未上板

#### RISK-BOOT-22 格式化中断后拒绝格式化/启动

- **后果**：两阶段格式化在「BBT 已落盘（带 FORMAT_IN_PROGRESS）、initial CP 尚未写」时掉电；重启时若 `bbt_init` 因 RISK-BBT-18 选中旧 BBT record（不带标志），`cp_media_status_probe` 把「有 BBT、无 CP」判成 `CP_MEDIA_IO_ERROR` ⇒ `pfr_media_check_format` 拒绝格式化，`boot failed, fallback to fc_service`，需人工 CLI `format` 恢复。可用性问题，不是数据破坏。
- **缓解**：无产品侧缓解；UT `format_in_progress_resume` 用更大 BBT sequence 让新 record 胜出以隔离变量。
- **下一步**：优先修 RISK-BBT-18（locate 按 sequence 最大值选）；修好后用例可去掉 bootstrap 清池。
- **证据**：板端 2026-10-24 21:32:27 日志；`src/ftl/checkpoint.c:cp_media_status_probe`；`src/ftl/bbt.c:bbt_load` / `bbt_save`。
- **历史/验证**：
  - 2026-10-24 板端复现，未修

#### RISK-BOOT-23 非正常复位后最新 CP 双 plane 读失败

- **后果**：`[CP] find latest seq=17 idx=2` → `[BLK] read failed plane=0/1 page=0 rec=2` → `fallback to full metadata rebuild`；重建成功，`bad areas cp=0`。若每次 dirty reset 都复现，说明最新 CP 记录的坏页退避不足，代价是启动慢，无数据丢失。
- **缓解**：Journal 回放成功；全量重建成功；同次 H2testw 417 MB 0 错误。
- **下一步**：干净断电上电看是否仍有 `read failed ... rec=`/`fallback`；只在 dirty reset 后出现 ⇒ 预期接受；干净上电也复现 ⇒ 查 boot 读 CP 失败时是否应标坏该块并切换 CP 块。
- **证据**：`src/ftl/checkpoint.c` find latest；`src/ftl/power_failure_recovery.c:572/1012`；板端 2026-10-30 启动日志。
- **历史/验证**：
  - 2026-10-30 对照：dirty reset 重建一次；format 后 PIN reset 恢复 `type=1` 无重建 → 解释为预期

#### RISK-BOOT-24 anomalies≠0 不是缺陷

- **后果**：`pfr_probe_blocks` 按 CP 快照探测「池块却有数据」——发生在 CP 快照之后才分配、且其 Journal 条目落在丢失尾部的块上。处理：转 CLOSED、按 L2P 全块重建 valid_count，并计入 `stats.anomalies`。`closed=X` 与 `info closed=` 相差 N 是正常对账。
- **缓解**：安全网本身；`dng=0`/`rrab=0` 是数据完整性的旁证。
- **下一步**：判据：`anomalies` 可在任意掉电点非零；只要 `info closed = restore closed + anomalies`、`dng=0`、无 `mapping lost` 即预期；`closed` 对不上或出现 dangling 按缺陷处理。
- **证据**：`src/ftl/power_failure_recovery.c:pfr_probe_blocks` / `pfr_close_block`；`include/ftl/power_failure_recovery.h:anomalies` 注释。
- **历史/验证**：
  - 2026-10-31 板端：`restore closed=3086 … anomalies=2` → `info closed=3088`；UT `boot_restore_reconcile` 已上板通过

#### RISK-BOOT-25 重启后整段覆盖写触发大量 dangling

- **后果**：`/speed/ftl_rw` chunk=16/32 先 `reboot_from_media`，再对同一 25 MiB 区间整段覆盖写；跑完 `info` 显示 `dng=992`（自 `ftl_init` 起累计的「覆盖写旧映射指向 `valid_count==0` 块」次数），同窗口 `rrab=0`、`mig=0/0`、数据校验 `err=0`、全量 UT 156/156。两种解释：(a) 旧块已标 BAD（测试介质有 3 个标记坏块）；(b) 重启重建 `valid_count` 欠计。后果：(b) 会容量无声流失、极端时 GC 误回收仍被引用块。
- **缓解**：测试期间项目日志默认关闭，当前分不清 (a)/(b)；数据面未受影响。
- **下一步**：`test --log-project info --param chunk 32 /speed/ftl_rw`，看 `[FTL] dangling … st=` 是 BAD（预期）还是 CLOSED/FREE/LOG/NONE（缺陷）；写前/写后各 `info` 看 `dng` 增量。若为 (b)，查 `pfr_boot_recovery` 对 CP 快照 + Journal 回放的 valid_count 记账。
- **证据**：板端 2026-11-01 release UT（`built 04:31:58`）：`dng=992 rrab=0 free=2766 bad=26 closed=499`；`src/ftl/ftl.c` dangling 守卫；`src/ftl/power_failure_recovery.c`。
- **历史/验证**：
  - 2026-11-01 复现；`count underrun` 探针多次未命中（抽样 32 块 × 4 页）

#### RISK-BOOT-26 恢复统计 WORD 回绕

- **后果**：恢复统计用 `WORD`，失败计数到 65536 整倍数回绕为 0 ⇒ `stats.l2p_read_errors != 0U` 判据失效 ⇒ 快照被错误接受。
- **缓解**：`pfr_stats_s` 的 `closed/partial/log_blocks/anomalies/l2p_read_errors/l2p_degraded_cw/valid_cw/sub_zero` 全为 `DWORD`，回绕点提到 2^32。
- **下一步**：上板看 probe 汇总行计数与量级一致；若将来缩窄回 WORD，必须同时把判据改成独立 bool。
- **证据**：`src/ftl/power_failure_recovery.c:pfr_stats_s` / `pfr_probe_blocks`。
- **历史/验证**：
  - 2026-11-04 统一 DWORD → 编译通过，未上板

#### RISK-BOOT-28 block_words 隐式 in/out

- **后果**：`block_words[]` 隐式 in/out，restore → probe → build_pools 的顺序只靠注释。
- **缓解**：新增 `pfr_block_table_s {word, loaded, restored, probed}`，三段都通过它传递；`pfr_build_pools()` 入口 `ASSERT(tbl->probed)`，顺序由结构强制。
- **下一步**：clean/PF 启动走一遍；新增恢复阶段时必须经过 `pfr_ledger_snapshot`。
- **证据**：`src/ftl/power_failure_recovery.c:pfr_block_table_s` / `pfr_ledger_snapshot` / `pfr_build_pools`。
- **历史/验证**：
  - 2026-11-04 修复 → 编译通过，未上板

#### RISK-MD-14 双副本账本/Anchor 原子性

- **后果**：① 两份 PBA 都在 `l2p_live` 里计数（回收判据 ==0）；② `md_live_rebuild` / `md_block_is_metadata` 的 debug 差分必须同时扫主/备；③ Anchor 记录 8→16 页，主半必须完整同 sequence 才算有效，备份半 best-effort；④ 两份都写成功才 `md_commit_region(MD_COMMIT_PAIR)`，否则旧 pair 仍是真值。
- **缓解**：代码已按此实现；Anchor 记录主/备分半校验。
- **下一步**：满盘写 + CP/压缩 + 反复重启；注入单个备份页损坏应仍能加载最新 Directory。
- **证据**：`src/ftl/metadata.c:md_anchor_page` / `md_load_assemble` / `md_anchor_record_probe_full` / `md_commit_region(MD_COMMIT_PAIR)`；`src/ftl/metadata_job.c`。
- **历史/验证**：
  - v0.90 实现 → 未上板

#### RISK-MD-15 副本感知压缩

- **后果**：victim 上可能只有主、只有备、或两份都在；压缩必须逐副本搬移，且两份都离开 victim 前不得推进 region 游标；前台 flush 双副本占两个槽 ⇒ 预留槽位 1→2。
- **缓解**：`j->cur_copy` + 两副本扫描 + `md_commit_region(MD_COMMIT_BACKUP/PRIMARY)`；`MD_COMPACT_RESERVED_SLOTS=L2P_REGION_COPIES`。
- **下一步**：满盘触发压缩，`live=` 稳定、无 `no directory entry`/`clear stale count`；重启后数据完整。
- **证据**：`src/ftl/metadata_job.c:md_compact_read` / `md_cp_write_complete` / `md_compact_begin`。
- **历史/验证**：
  - v0.90 实现 → 未上板

#### RISK-MD-16 victim region 页读失败

- **后果**：板端 GC 期间反复 `[META] md job abort phase=2 victim=46 moved=10 (victim 不擦)`；`phase=2 = MD_JOB_CP_READ`，无 `submit stalled` ⇒ 读命令完成状态带错误位。每次 `victim=46/moved=10` 完全相同 ⇒ 确定性坏页。后果：该元数据日志块无法搬空、live 水位偏高、压缩反复白做；无数据丢失。
- **缓解**：abort 语义本身安全；已改：`md_cp_read_complete` 检查 `ftl_cmd_read_data`，源副本读失败先读另一副本并重写同一目标副本，两份都失败才 abort。
- **下一步**：① abort 行打出完成状态位（`fc=`）；② 双副本下跳过不可读副本、保留另一份，不中止整轮；③ 两份都不可读按 RISK-BOOT-18 降级。
- **证据**：`src/ftl/metadata_job.c:md_handle_abort` / `md_cp_read_complete`；`src/ftl/metadata.c:md_compact_read`。
- **历史/验证**：
  - 2026-11-04 读失败先回退另一副本 → 编译通过，未上板

### 已闭合（详情见 archive）

| ID | 日期 | 一句话 | 归档 |
|---|---|---|---|
| RISK-MD-17 | 2026-11-02 | status.error 看不到整理 job 最近一次中止（status 上报层已删除） | `docs/archive/known_risks_batches.md` |
| RISK-BOOT-27 | 2026-11-04 | initial CP 判据把格式知识复制进 PFR（整段删除） | `docs/archive/known_risks_batches.md` |
| RISK-BOOT-29 | 2026-11-04 | pfr_boot_recovery 注释描述已删除的只读取证（入口重写） | `docs/archive/known_risks_batches.md` |


## 2C. L2P cache

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-L2P-01 | 待上板 | region 加载失败曾立即降级为全未映射，映射静默不可用 | 未上板 |
| RISK-L2P-02 | 待上板 | flush_kick 先取槽后建链，命令池空重试浪费日志槽 | 未上板 |
| RISK-L2P-03 | 待上板 | flush_kick 背压/资源预检/CLOCK 引用位的行为未板验 | 2026-11-12 常规回归通过（/ftl/ + H2testw）；残余：坏 region 读不活锁 |
| RISK-L2P-04 | 待上板 | 降级启动被误报 restored，丢 journal 未覆盖的旧映射 | 未上板 |
| RISK-L2P-05 | 待上板 | 备副本提交时主命令未 release，命令池吃紧时 flush 假失败 | 未上板 |
| RISK-L2P-06 | 待上板 | 批量读越界分支被压成 FTL_AGAIN，会永久重试（当前不可达） | 未上板 |
| RISK-L2P-07 | 待上板 | 顺序读预取白占 cache 行/资源，可能延迟 FLUSH | 未上板 |
| RISK-L2P-11 | 信息 | 写回 CMT 为唯一写缓存；新增映射变更路径必须同步失效 | 2026-11-04 代码核对 |
| RISK-L2P-12 | 待上板 | 读/oldp 路径漏查 CMT ⇒ 返回陈旧值 | 未上板 |
| RISK-L2P-13 | 待上板 | Header 先于 dirty CMT 落盘 ⇒ 掉电丢映射 | 未上板 |
| RISK-L2P-14 | 待上板 | Journal replay 写进 dirty CMT 后被 PFR 误清 | 未上板 |
| RISK-L2P-15 | 待上板 | 失败页撤销误保留 dirty CMT ⇒ 读到 FAILC 页 | 未上板 |
| RISK-L2P-16 | 待上板 | 容量竞态导致批量写半更新 ⇒ Journal 重复记账 | 未上板 |
| RISK-L2P-17 | 待上板 | thrashing 门挡住必需写回 ⇒ CMT 满后 host 写停住 | 未上板 |
| RISK-L2P-18 | 待上板 | dirty CMT 低于水位只在 RAM，掉电依赖 Journal | 未上板 |
| RISK-L2P-19 | 待上板 | 孤儿实时脏位让 CP 空转/Header 谎报 | 未上板 |
| RISK-L2P-20 | 待上板 | line hint 陈旧/冲突，只掉命中率不返回错行 | 未上板 |
| RISK-L2P-21 | 待上板 | Directory/备份 PBA 未校验就进 FC ⇒ 越界路由 | 2026-11-05 编译通过 |
| RISK-L2P-22 | 待上板 | CMT dirty 计数在 0 处静默夹取 ⇒ 丢映射或 FLUSH 超时 | 2026-11-05 编译通过 |
| RISK-L2P-23 | 待上板 | l2p_line_invalidate 不执行「仅 CLEAN」契约 | 2026-11-05 编译通过 |

### 详情

#### RISK-L2P-01 region 加载失败降级

- **后果**：region 加载失败时立即降级为全未映射，映射静默不可用。
- **缓解**：先重试 3 次（`L2P_LOAD_RETRY_MAX`），仍失败才置 CLEAN + 全 INVALID；不活锁。
- **下一步**：造瞬态读错验证映射不丢；持续坏 region 时不活锁。
- **证据**：`src/ftl/l2p_cache.c:ftl_l2p_callback`。
- **历史/验证**：
  - （日期未记录）加载失败改为重试 3 次后降级 → 未上板
  - （日期未记录）原归档条目回迁（`docs/archive/known_risks_batches.md §2C`）

#### RISK-L2P-02 flush_kick 资源预检

- **后果**：先取槽后建链，命令池空重试浪费 L2P 日志槽。
- **缓解**：`flush_kick` 在 `append_slot` 前用 `fc_get_resource_stats()` 预检命令/描述符/buffer/SQ/HSQ；不足即返 `FTL_AGAIN`，不消耗槽位。
- **下一步**：命令池高压下长时写，日志块换块频率不异常。
- **证据**：`src/ftl/l2p_cache.c:l2p_flush_resources_available`。
- **历史/验证**：
  - （日期未记录）建链前资源预检 → 未上板
  - （日期未记录）原归档条目回迁（`docs/archive/known_risks_batches.md §2C`）

#### RISK-L2P-04 降级启动被误报 restored

- **后果**：降级路径把行置 CLEAN + entries 全 INVALID，并让 `l2p_read_sync` 返回 OK ⇒ probe 的 `l2p_read_errors` 保持 0，启动仍报 `restored`；若该 region 有 journal 未覆盖的旧映射，会静默丢失且不触发 full metadata rebuild。
- **缓解**：范围化降级：cache 级 `region_degraded_bitmap`（行驱逐后仍识别）+ host 读该范围返回 `IO_ERROR` + probe 降级 CW 单独计数；只有两份副本都读失败才降级，且不再据此回退 full metadata rebuild（见 RISK-BOOT-18）。
- **下一步**：正常启动不得误报（`restored` 仍正常）；人工破坏一个 L2P region 页后启动应出现 `L2P region N degraded — snapshot incomplete`，而不是 `restored`。
- **证据**：`src/ftl/l2p_cache.c` / `include/ftl/l2p_cache.h`；`src/ftl/power_failure_recovery.c:pfr_probe_blocks`；`src/ftl/journal.c:jrn_replay`。
- **历史/验证**：
  - （日期未记录）v0.90 改为范围化降级（主备副本，不再全量重建） → 未上板
  - （日期未记录）原归档条目回迁（`docs/archive/known_risks_batches.md §2C`）

#### RISK-L2P-07 顺序读预取白占资源

- **后果**：预取可能白占 cache 行/资源：顺序检测误判、目标 region 未映射，或预取 load 在飞延迟 `l2p_flush_owner_idle`（LOADING 算非 idle），使 host FLUSH 等一个 region load；`load_inflight` 无硬上限（只靠 prefetch 自己的 2 上限）。
- **缓解**：只对顺序读、距 region 末尾 64 LBA 内触发；thrashing/flush/trim/资源不足时停；最多 2 个在飞；遇 DIRTY 不 kick 写回；不计 miss/hit。
- **下一步**：上板看 `info` 的 `pf skipped/started/hits` 与 `miss_pct`；若 FLUSH 延迟或 miss 变差，先降 `AHEAD`/`MAX_INFLIGHT` 或停用。
- **证据**：`src/ftl/ftl.c:ftl_seq_note_read`；`src/ftl/l2p_cache.c:l2p_region_prefetch` / `l2p_load_begin`。
- **历史/验证**：
  - 2026-10-23 顺序读预取实现 → 未上板
  - （日期未记录）原归档条目回迁（`docs/archive/known_risks_batches.md §2C`）

#### RISK-L2P-03 flush_kick 背压与加载降级

- **后果**：flush_kick 背压返 `FTL_AGAIN`、写回前资源预检、CLOCK 引用位 1、加载失败降级为全未映射；这些路径未板验，有误会导致 flush/CP 中止或热点 region 长期钉住。
- **缓解**：无（仅编译通过）。
- **下一步**：命令池压力下 `fl`/CP 不中止、不烧日志槽；坏 region 读不活锁。
- **证据**：`src/ftl/l2p_cache.c`。
- **历史/验证**：
  - （日期未记录）flush_kick 背压改报 `FTL_AGAIN`、写回前资源预检、CLOCK 引用位改 1、加载失败降级 → 未上板

#### RISK-L2P-05 备副本提交时主命令未 release

- **后果**：备副本在完成回调提交时主命令尚未 `fc_command_release`；命令池吃紧时写页失败 ⇒ 整次 flush 判失败、两槽作废、行回 DIRTY。无数据破坏（旧 pair 仍是真值），高压力下 flush 假失败 + 日志槽浪费。
- **缓解**：失败不提交、行保持 DIRTY 下轮重试；`l2p_flush_resources_available()` kick 时预检一次。
- **下一步**：上板观察 flush 失败与换块率；异常时改为回调内先 release 主命令，或 kick 时一次建两条命令。
- **证据**：`src/ftl/l2p_cache.c:l2p_flush_kick` / `ftl_l2p_callback`；`include/ftl/l2p_cache.h:L2P_FLUSH_MAX_INFLIGHT`。
- **历史/验证**：
  - （日期未记录）L2P region 主备双副本落地，备副本改为完成回调提交 → 未上板

#### RISK-L2P-06 批量读越界分支永久重试

- **后果**：越界分支返 `FTL_ERROR` 后被 `ftl_confirm_write` 压成 `FTL_AGAIN` ⇒ 永久重试；当前 `FTL_LBA_MAX` 是 8 的整数倍，不可达。
- **缓解**：miss 已记数并起加载；唯一调用点有 `l2p_is_ready` 前置；页内 8 LBA 同 region 由 `_Static_assert` 保证。
- **下一步**：新增调用点或改 ready 语义时复核：显式错误传播，或入口 `ASSERT`。
- **证据**：`src/ftl/l2p_cache.c:l2p_entries_read`；`src/ftl/ftl.c:ftl_confirm_write`。
- **历史/验证**：
  - 2026-10-22 批量读 miss 语义对齐（记 miss + 起加载）→ 未上板；越界分支留待复核

#### RISK-L2P-11 写回 CMT 的失效点必须同步

- **后果**：写回 CMT 是唯一写缓存；新增绕过 `l2p_write` / `l2p_entries_write` 的映射变更路径若不同步 CMT，host 读会命中旧 PBA（读旧数据/悬空块）。
- **缓解**：失效入口已收敛（init / write / entries_write / owner_invalidate / note_region_lost / line_invalidate）；CMT 1024 set × 4 way，冲突只降命中率。
- **下一步**：新增 L2P 变更入口必须同步 CMT；跑 TRIM / 失败页撤销 / PFR 行作废负例，看 `cmt hit/miss`。
- **证据**：`src/ftl/l2p_cache.c:l2p_cmt_*`；`include/ftl/l2p_cache.h:L2P_CMT_SETS/WAYS`。
- **历史/验证**：
  - 2026-11-04 写回 CMT 取代旧写缓存，核对全部失效点 → 代码核对通过，未上板

#### RISK-L2P-12 读/取旧值路径绕过 CMT

- **后果**：dirty CMT 可与 CLEAN line 共存；读/oldp 路径漏查 CMT ⇒ 写确认拿到旧 oldp（valid_count 虚高）或读拿到旧 PBA。
- **缓解**：所有读/oldp 路径 CMT 优先；`l2p_entries_read` 已逐 LBA CMT 优先；`l2p_line_get` 仅诊断。
- **下一步**：新增读入口时复核；跑 line CLEAN + dirty CMT 读回负例。
- **证据**：`src/ftl/l2p_cache.c:l2p_read` / `l2p_entries_read` / `l2p_is_ready` / `l2p_entries_ready`。
- **历史/验证**：
  - 2026-11-04 写回 CMT 改造，读/oldp 路径改为 CMT 优先 → 未上板

#### RISK-L2P-13 Header 先于 dirty CMT 落盘

- **后果**：CMT 只在 RAM；CP 未等页写回就提交 Header，Journal truncate 后丢唯一副本 ⇒ 掉电丢映射。
- **缓解**：dirty 插入即置 `region_dirty_bitmap`；`cp_regions_flush` 按快照位驱动；DONE 前 `l2p_owner_snapshot_covered` 再验。
- **下一步**：上板 dirty CMT + 掉电/重启 + `flash_reboot_readback`；出现 `snapshot region not covered` 立即停。
- **证据**：`src/ftl/checkpoint.c:cp_regions_flush`；`src/ftl/l2p_cache.c:l2p_owner_snapshot_covered` / `l2p_flush_kick`。
- **历史/验证**：
  - 2026-11-04 写回 CMT 改造，增加快照覆盖校验 → 未上板

#### RISK-L2P-14 Journal replay 走 dirty CMT 后被 PFR 误清

- **后果**：PFR replay 后用 `l2p_line_invalidate` 作废不在 CP bitmap 的 CLEAN 行；若 replay 写进 dirty CMT，清除路径会丢新映射。
- **缓解**：replay 前后 `l2p_cmt_set_enabled(false/true)`，强制走 region line，恢复后 CMT 为空。
- **下一步**：上板 `boot_restore_reconcile` + 掉电重启；replay 后 CMT 为空、line DIRTY 正常写回。
- **证据**：`src/ftl/power_failure_recovery.c:jrn_replay`；`src/ftl/l2p_cache.c:l2p_cmt_set_enabled`。
- **历史/验证**：
  - 2026-11-04 写回 CMT 改造，replay 期禁用 CMT → 未上板

#### RISK-L2P-15 失败页撤销误保留 dirty CMT

- **后果**：失败页 dirty CMT 指向 FAILC 的 PBA，撤销完成前读必须失败；整区保留读到坏页，整区清掉丢同 region 其他成功写。
- **缓解**：`l2p_note_region_lost` 只清失败页 LBA 范围 + 全部 CLEAN；残余：同 LBA 后续新写会被一并清，依赖 VWC/Controller 串行化。
- **下一步**：上板 `cache_failc_abort` 负例；确认契约后决定是否带失败 PBA 过滤。
- **证据**：`src/ftl/l2p_cache.c:l2p_note_region_lost` / `l2p_cmt_invalidate_region`；`src/ftl/ftl.c:ftl_cache_wr_mark_failed`。
- **历史/验证**：
  - 2026-11-04 写回 CMT 改造，失败页按 LBA 范围撤销 → 未上板

#### RISK-L2P-16 容量竞态导致批量写半更新

- **后果**：`l2p_entries_ready` 到 `l2p_entries_write` 之间 CMT 满 ⇒ 部分插入后返 AGAIN，而 Journal 已 append ⇒ 重复记账。
- **缓解**：`entries_ready` 一次判完整批；`entries_write` 失败整批回退 line DIRTY；单 LBA 同样回退；`dirty_written` 守卫。
- **下一步**：新增批量写调用点必须走 `l2p_entries_ready`；观察 `cmt defer` / `cmt evict`。
- **证据**：`src/ftl/l2p_cache.c:l2p_entries_ready` / `l2p_entries_write` / `l2p_write`；`src/ftl/ftl.c:ftl_confirm_write`。
- **历史/验证**：
  - 2026-11-04 写回 CMT 改造，批量写加 ready 预判与整批回退 → 未上板

#### RISK-L2P-17 thrashing 门挡住必需写回

- **后果**：dirty CMT / line DIRTY 占写缓存且阻塞同 region 新写，不是可省的推测性 flush；被 thrashing 门挡住后 CMT 满 ⇒ host 写停住。
- **缓解**：`l2p_handle` 不用 `thrash.thrashing` 门控 flush；thrashing 仅保留观测/预取判据。
- **下一步**：上板 thrashing / 满盘随机写；看 `cmt wb defer` 与 host 写是否在有限时间恢复。
- **证据**：`src/ftl/l2p_cache.c:l2p_handle` / `l2p_owner_flush_kick`。
- **历史/验证**：
  - 2026-11-04 写回 CMT 改造，flush 不再受 thrashing 门控 → 未上板

#### RISK-L2P-18 dirty CMT 低于水位时只在 RAM

- **后果**：host 不 FLUSH 且 CP T3 门未触发时依赖 Journal；Journal 未持久则最近映射丢失（既有写缓存窗口）。
- **缓解**：低于水位只推迟主动写回；IDLE 强制排空、FLUSH 走 force、CP/容量 eviction 也强制；Journal append 先于 dirty 写入。
- **下一步**：低负载写后 idle，看 `cmt` 是否归零；再断电重启读回。
- **证据**：`src/ftl/l2p_cache.c:L2P_CMT_WB_WATERMARK` / `l2p_handle`；`src/ftl/ftl.c:ftl_flush_step`。
- **历史/验证**：
  - 2026-11-04 写回 CMT 水位机制引入 → 未上板

#### RISK-L2P-19 孤儿实时脏位让 CP 空转

- **后果**：`region_dirty_bitmap` 置位但无 DIRTY line、无 dirty CMT（孤儿位）⇒ pending 到 stall timeout；误当已持久则 Header 谎报。
- **缓解**：`l2p_region_flush_kick` 对孤儿位返 `FTL_ERROR` 并自愈 `cmt_dirty_region_bitmap`；CP DONE 前 `l2p_owner_snapshot_covered` 兜底。
- **下一步**：测试源构造孤儿位，或看 `snapshot region not covered` / CP abort；新增清 dirty 路径时复核两个 bitmap。
- **证据**：`src/ftl/l2p_cache.c:l2p_region_flush_kick` / `l2p_cmt_merge_region`；`src/ftl/checkpoint.c:cp_regions_flush`。
- **历史/验证**：
  - 2026-11-04 孤儿脏位自愈 + CP 兜底 → 未上板

#### RISK-L2P-20 line hint 陈旧/冲突

- **后果**：`line_hint[]` 只是 region→line 提示；region/state 变更漏清 hint ⇒ 先命中错误槽位；tags 校验 + 线性 fallback 保证不返回错行，只掉命中率。
- **缓解**：`l2p_line_find` 命中必校验 tags；evict / invalidate / load 失败清；init 全 0xFF；CLI 看 `line hint=hit/miss`。
- **下一步**：改 `tags[].region_id/state` 的路径必须同步 hint；看 hint 命中率是否接近 1。
- **证据**：`src/ftl/l2p_cache.c:l2p_line_find` / `l2p_line_hint_clear` / `l2p_load_begin` / `l2p_line_evict`；`include/ftl/l2p_cache.h:L2P_LINE_HINT_SLOTS`。
- **历史/验证**：
  - 2026-11-04 line hint 表引入，带 tags 校验与线性 fallback → 未上板

#### RISK-L2P-21 Directory/备份 PBA 未校验就进 FC

- **后果**：`l2p_load_begin` 把介质读回的 PBA 直接交给 `l2p_load_submit`，release 下只有 `pba_to_cmd` 的 ASSERT；字段越界的 PBA 造出越界路由，`fc_poll_fill_queue` 索引 `suspended[][][]` ⇒ 越界读写。
- **缓解**：两个候选 PBA 做 `pba_is_valid`；损坏副本按缺失 + `corrupt`，两份都坏走 `l2p_load_fail_settle`；备份回退/主备交换/`md_compact_read` 同样校验。
- **下一步**：测试源注入坏 PBA，验证 host 读 IO_ERROR、不越界。
- **证据**：`src/ftl/l2p_cache.c:l2p_load_begin` / `ftl_l2p_callback`；`src/ftl/ftl_fc_interface.c:pba_to_cmd`；`src/ftl/metadata.c:md_entry_find` / `md_backup_find`；`src/ftl/metadata_job.c:md_compact_read`。
- **历史/验证**：
  - 2026-11-05 加载候选 PBA 加 `pba_is_valid`，损坏副本按缺失处理 → 编译通过，未上板

#### RISK-L2P-22 CMT dirty 计数在 0 处静默夹取

- **后果**：计数与条目不守恒时错误被吞：少计 ⇒ idle 提前为真 ⇒ FLUSH/CP 在 dirty CMT 未落盘时完成 ⇒ 重启丢映射；多计 ⇒ FLUSH 超时 / CP 中止。
- **缓解**：`l2p_cmt_dirty_release` 唯一释放闸门；零值前 ASSERT + LOGE，保留夹取防回绕。
- **下一步**：负例（同一 dirty 条目 drop 两次）；上板观察。
- **证据**：`src/ftl/l2p_cache.c:l2p_cmt_drop` / `l2p_cmt_clear_region` / `l2p_flush_owner_idle`。
- **历史/验证**：
  - 2026-11-05 新增 `l2p_cmt_dirty_release` 单闸门 + 零值 ASSERT/LOGE → 编译通过，未上板
  - 2026-11-04 写回 CMT 改造引入 dirty 计数 → 未上板

#### RISK-L2P-23 l2p_line_invalidate 不执行「仅 CLEAN」契约

- **后果**：契约写「仅 CLEAN/INVALID」，实现只判 `state != INVALID` ⇒ 误传 DIRTY/FLUSHING 行会静默丢弃未落盘映射。
- **缓解**：`state != INVALID` 分支内 ASSERT(state == CLEAN)；唯一调用者 PFR 只传 CLEAN 行。
- **下一步**：新增调用者时复核。
- **证据**：`src/ftl/l2p_cache.c:l2p_line_invalidate`；`include/ftl/l2p_cache.h` 契约。
- **历史/验证**：
  - 2026-11-05 加「仅 CLEAN」ASSERT → 编译通过，未上板

### 已闭合（详情见 archive）

| ID | 日期 | 一句话 | 归档 |
|---|---|---|---|
| RISK-L2P-08 | 2026-10-30 | ITCM 野写（根因与写者已定位） | `docs/archive/known_risks_batches.md §2026-10-30` |
| RISK-L2P-10 | 2026-10-31 | Journal 写失败后 WAL 闸门与 CP 互等（上板 1.11 s 通过） | `docs/archive/known_risks_batches.md §2026-10-31` |

---

## 2D. Checkpoint

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-CP-01 | 待上板 | journal 预擦失败仍记 pre_erased_block，回切时跳过擦除 | 未上板 |
| RISK-CP-02 | 待上板 | checkpoint 回调缺 waiting 守卫，迟到完成会按当前 phase 重走 | 未上板 |
| RISK-CP-03 | 待上板 | 状态机改动未板验：错误传播 / 游标复位 / 水位条目化 | 2026-11-12 常规回归通过（/ftl/ + H2testw）；残余：各 phase 掉电、85%/95% 触发点 |
| RISK-CP-04 | 待定位 | host 写流下 CP 提交饥饿，submit stalled 后 abort | 2026-11-12 上板复现 |
| RISK-CP-05 | 待上板 | abort 被调两次，fail_phase 被覆盖为 CP_DONE + 日志刷屏 | 未上板 |
| RISK-CP-10 | 待上板 | T2 触发无 latch，写计数停在整数倍时 CP 空转 | 2026-11-04 编译通过 |
| RISK-CP-11 | 待上板 | initial record 失败块写死 plane 0/1 | 2026-11-04 编译通过 |

### 详情

#### RISK-CP-01 journal 预擦失败仍记 pre_erased_block

- **后果**：journal 预擦失败仍记 `pre_erased_block` ⇒ journal 回切时跳过擦除、写进未擦块。
- **缓解**：失败改 `jrn_truncate_commit(JRN_BLOCK_NONE)`：只复位水位、不记账；回切时由 `need_erase` 重试，下一次 CP 的 plan 也会再报 NEEDS_ERASE。
- **下一步**：擦除失败注入，确认不跳过擦除、CP 不因 T1 反复触发。
- **证据**：`src/ftl/checkpoint.c:ftl_checkpoint_callback`（CP_TRUNC_ERASE）。
- **历史/验证**：
  - （日期未记录）修复：失败不再记账，回切重试 → 未上板

#### RISK-CP-02 checkpoint 回调缺 waiting 守卫

- **后果**：`ftl_checkpoint_callback` 无 `waiting` 守卫：重复/迟到完成会按当前 phase 再走一遍。
- **缓解**：入口 `ASSERT(waiting)` + release 拒绝（LOGE + release + return）。
- **下一步**：FC 重复完成注入 / 上板回归。
- **证据**：`src/ftl/checkpoint.c:ftl_checkpoint_callback`。
- **历史/验证**：
  - （日期未记录）修复：入口守卫 + release 拒绝 → 未上板

#### RISK-CP-03 状态机改动未板验

- **后果**：提交状态传播（FTL_ERROR 立即中止）、`find_latest` 失败复位游标、水位公式精确到条目，这三处行为改动未经板验。
- **缓解**：编译零告警。
- **下一步**：正常 CP、各 phase 掉电、journal 回切、85%/95% 触发点。
- **证据**：`src/ftl/checkpoint.c`。
- **历史/验证**：
  - （日期未记录）上述三处改动 → 未上板

#### RISK-CP-04 持续写流下 CP 提交饥饿

- **后果**：H2testw 期间反复 `[CP] submit stalled 25ms — abort phase=1`。CP 用 `ftl_op_submit` 申请 FC 命令，host 写可占满 `FC_COMMAND_POOL_DEPTH=64`（`FTL_PENDING_MAX=32` 个写各占 2 条命令）⇒ CP 连续 10ms 拿不到槽即中止重试。后果：checkpoint 长期不推进、journal 环增长；环满后 host 写被 Backpressure 拒绝（写停顿，无静默损坏）。尚不能区分它是 GC 活锁的症状还是独立缺陷。
- **缓解**：中止后可重试；journal 环满是安全背压；abort 日志 1s/实例。序列打开或仍有未完成主机组时 `ftl_op_bg_submit_deferred()` 挂起 CP 的提交 deadline。
- **下一步**：先修 RISK-GC-07 后复跑 H2testw；若仍复现，给 CP 预留命令槽或延长 deadline；上板确认 journal 水位与 CP 成功率。
- **证据**：`src/ftl/checkpoint.c:cp_job_stall` / `cp_command_result_handle`；`src/ftl/ftl_op.c:ftl_op_submit`；`include/fc/fc.h:FC_COMMAND_POOL_DEPTH`；串口 rev30 日志。
- **历史/验证**：
  - 2026-11-12 18:04:36 H2testw 通过；启动空闲期一次 `submit stalled 10ms`（cmd=1 buf=8、held ops md=1）⇒ 池未耗尽，是 deadline 与 metadata job 重叠的误 abort，无数据损失
  - 2026-11-12 H2testw 复现 `submit stalled 10ms`（free=3207、cmd=1/buf=4，见 RISK-HOST-18）
  - 2026-10-30 H2testw 未复现（cp=1724 持续递增、无 stalled 行）
  - 2026-10-21 复现时日志刷屏，已加 abort 日志限流（见 RISK-CP-05），饥饿本身未修

#### RISK-CP-05 abort 被调两次覆盖 fail_phase

- **后果**：`cp_regions_flush` 的 `cp_job_stall` 内部已 `cp_job_abort`（记 fail_phase=1、phase=CP_DONE），返回 `CP_FLUSH_FAILED` 后 `cp_job_step` 又调一次 abort，第二次看到 phase 已是 CP_DONE ⇒ `fail_phase=12`；日志出现 `abort at phase=12`（真实相位丢失），每次重试打两条 WRN，每 10~25ms 一次。
- **缓解**：`cp_job_abort` 幂等：只在 `!failed` 时记首次 `fail_phase`；abort 日志按实例 1s 限流（`log_abort_ms`）。
- **下一步**：持续写流下 CP 日志 1s 最多一行，`abort at phase=` 显示真实相位（1 而非 12）；若仍为 12，说明另有路径在 CP_DONE 调 abort。
- **证据**：`src/ftl/checkpoint.c:cp_job_abort` / `cp_job_stall` / CP_DONE 分支；`include/ftl/checkpoint.h:log_abort_ms`。
- **历史/验证**：
  - 2026-10-21 修复：abort 幂等 + 日志限流 → 未上板


#### RISK-CP-10 T2 触发无 latch 导致 CP 空转

- **后果**：T2 判据 `total_writes % CP_WRITE_INTERVAL == 0` 没有 latch；`total_writes` 只在 host 写确认时自增，空闲时不变 ⇒ CP 完成后每轮 TIMER 仍命中 ⇒ 持续空转写 21 页 record + 块轮转擦除。
- **缓解**：`cp_state_s` 新增 `last_cp_writes`，`cp_job_begin` 成功后记录起点；T2 改为 `(total_writes - last_cp_writes) >= CP_WRITE_INTERVAL`（无符号减法处理回绕）。无数据损坏；CP 中途 abort 时基准已更新，避免重试风暴；压力场景由 T1 水位 / emergency 兜底。
- **下一步**：写完恰好 N×1024 页后静置 5s，确认 `[CP] done` 不再连续出现；重启后 `total_writes` 归零，基准同样从 0 起。
- **证据**：`src/ftl/checkpoint.c:cp_should_start` / `cp_handle`；`include/ftl/checkpoint.h:cp_state_s.last_cp_writes`；`src/ftl/ftl.c:529:ftl_confirm_write`。
- **历史/验证**：
  - 2026-11-04 新增 `last_cp_writes`，T2 改增量判据 → 编译通过，未上板

#### RISK-CP-11 initial record 失败块写死 plane

- **后果**：`cp_initial_record_write` 对每个失败块只 `bbt_mark_bad(ftl_gblock(0/1, bad_block))`，假定 `PLANE_NUM==2`；维度变大时其余 plane 的同号块不会被标坏、仍可能被 CP 选中。
- **缓解**：改为 `for (p = 0; p < PLANE_NUM; p++) bbt_mark_bad(ftl_gblock(p, bad_block))`；当前 `PLANE_NUM==2`，行为与改前等价。
- **下一步**：维度常量变化时随 H39 一起检查其他硬编码下标。
- **证据**：`src/ftl/checkpoint.c:cp_initial_record_write`（failed_blocks 收尾循环）。
- **历史/验证**：
  - 2026-11-04 改为按 PLANE_NUM 循环 → 编译通过，未上板

### 已闭合（详情见 archive）

| ID | 日期 | 一句话 | 归档 |
|---|---|---|---|
| RISK-CP-07 | 2026-10-31 | SYNCHRONIZE CACHE 不产生 Checkpoint | `docs/archive/known_risks_batches.md` |
| RISK-CP-12 | 2026-11-04 | 注释与 initial CP 格式不符（已修正） | `docs/archive/known_risks_batches.md` |

---

## 2E. 磨损计数 / 静态 WL

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-WL-01 | 待上板 | WL 擦完用 RETIRE，RETIRED 计入 erase 统计导致容量流失 | 未上板 |
| RISK-WL-02 | 待上板 | 擦除不重置 read_count，read disturb 跨周期累计被误退役 | 未上板 |
| RISK-WL-03 | 待上板 | Closed SB 含 BAD plane 仍被 WL 选中，migrate 永久卡死 | 未上板 |
| RISK-WL-04 | 待上板 | WL 收尾 FREE / read_count 复位 / BAD plane 过滤 / 磨损表校验未板验 | 2026-11-12 常规回归通过（/ftl/ + H2testw）；残余：掉电、长时 soak |
| RISK-WL-05 | 待后续 | wl_should_trigger 与 wl_select_victim 各做一次全表扫描 | — |

### 详情

#### RISK-WL-01 WL 擦完用 RETIRE 导致容量流失

- **后果**：WL 擦完 victim 用 `MIG_POST_RETIRE`：RETIRED 仍计入 erase 统计的 min/max ⇒ 低擦除块永久压低 min，每次 WL 退役一块 ⇒ 容量持续流失。
- **缓解**：改 `MIG_POST_FREE`：擦完归还 Free Pool，动态 WL 优先选中它写入，擦除计数回升。
- **下一步**：触发 WL 后 Free Pool/容量不下降，低擦除块被重新分配。
- **证据**：`src/ftl/migrate.c:mig_wl_begin`。
- **历史/验证**：
  - （日期未记录）修复：MIG_POST_RETIRE → MIG_POST_FREE → 未上板

#### RISK-WL-02 擦除不重置 read_count

- **后果**：`alc_bst_note_erase` 不重置 `read_count`：read disturb 跨擦除周期累计 ⇒ 正常块越过 1000 被预测路径退役。
- **缓解**：擦除成功清 `read_count`；`rr.health / last_ecc / rising_count` 保留（慢死趋势跨周期）。
- **下一步**：长时 soak，正常块不被 `rr_predictive_scan` 误退役。
- **证据**：`src/ftl/allocator.c:alc_bst_note_erase`。
- **历史/验证**：
  - （日期未记录）修复：擦除清 read_count → 未上板

#### RISK-WL-03 Closed SB 含 BAD plane 仍被选中

- **后果**：Closed SB 的某 plane 标 BAD 后仍被 WL 选中：`plane_blocks` 含 BAD 块 ⇒ migrate 读失败 abort，且每轮重选 ⇒ WL 永久卡死。
- **缓解**：`wl_select_victim` 预检：任一 present plane 的块 BAD ⇒ 跳过该 SB。
- **下一步**：运行期标坏后 WL 仍能前进。
- **证据**：`src/ftl/wear_leveling.c:wl_select_victim`。
- **历史/验证**：
  - （日期未记录）修复：victim 预检跳过 BAD plane → 未上板

#### RISK-WL-04 WL 行为变化未板验

- **后果**：WL 收尾 FREE、read_count 复位、BAD plane 过滤、磨损表 `record_pages` 校验，四处行为改动未经板验。
- **缓解**：编译零告警。
- **下一步**：正常 WL/GC、断电、长时 soak。
- **证据**：`src/ftl/migrate.c` / `allocator.c` / `wear_leveling.c`。
- **历史/验证**：
  - （日期未记录）上述四处改动 → 未上板

#### RISK-WL-05 触发与选块各做一次全表扫描

- **后果**：`wl_should_trigger` 与 `wl_select_victim` 各做一次全表扫描（RISK-PERF-01 的一部分）；RETIRED 块仍计入 erase stats。
- **缓解**：5s 节拍门。
- **下一步**：需要时把 `alc_bst_erase_stats_s` 从触发点透传。
- **证据**：`src/ftl/wear_leveling.c`。
- **历史/验证**：
  - （日期未记录）识别为重复全表扫描 → 待后续



### 已闭合（详情见 archive）

| ID | 日期 | 一句话 | 归档 |
|---|---|---|---|
| RISK-WL-07 | 2026-11-12 | host 持续写时 WL 仍启动，占住单飞行 migrate 引擎 | `docs/archive/known_risks_batches.md` |

---

## 2F. FTL↔FC 接口

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-IF-01 | 待上板 | 写位图用 1U<<cw（plane 盲），迁移 plane 1 被填 0xFF | 未上板 |
| RISK-IF-02 | 待上板 | 同步等待超时后迟到完成解引用 NULL ctx + 槽泄漏 | 未上板 |
| RISK-IF-03 | 待上板 | 同步原语忽略数据搬运返回值，短链读静默返回 OK | 未上板 |
| RISK-IF-04 | 待上板 | plane1 迁移 / wbuf 稀疏页 / 越段位图 / 迟到完成未板验 | 2026-11-12 常规回归通过（media 30/30 + H2testw）；残余：wbuf 稀疏页/越段位图/迟到完成负例 |
| RISK-IF-06 | 待决策 | chain 派发路径不处理 RAID stripe encode | — |
| RISK-IF-07 | 待上板 | 批次 chain 混优先级或末条 MORE 无 END 会错乱/锁死 | 正常路径上板（2026-10-30） |
| RISK-IF-08 | 待上板 | page-read 聚合忽略 NAND 结果位，TIMEOUT 可静默返回陈旧数据 | 未上板 |
| RISK-IF-09 | 待上板 | 放弃序列只清 RAM 不发设备 LAST | 未上板 |
| RISK-IF-10 | 信息 | 被放弃的 MORE 链尾占住 chain_owner（现状单队列不成立） | 2026-11-04 复核 |
| RISK-IF-11 | 待上板 | 逐页确认依赖完成顺序与 FAILC 语义 | 正常路径上板（2026-10-30） |
| RISK-IF-12 | 待上板 | 确认待入账 × 序列门控可能退化到 1s idle 守卫 | 满盘压测上板（2026-10-30） |
| RISK-IF-13 | 信息 | 门控覆盖与后台/RR 交互（判定结论，非缺陷） | 2026-10-30 上板验证 |
| RISK-IF-15 | 待上板 | RAW 读判空把全 0xFF 用户区的已编程页当空白 | 正常路径上板（2026-11-01） |
| RISK-IF-16 | 待上板 | timeout_us 名不副实，实为自旋轮数 | 2026-11-04 编译通过 |
| RISK-IF-17 | 待上板 | 超时后调用者释放仍在飞的命令，可能跨命令破坏 | 2026-11-05 编译通过 |
| RISK-IF-18 | 待上板 | 接口层 4 处 release 守卫缺口（H14） | 2026-11-05 编译通过 |

### 详情

#### RISK-IF-01 写位图 plane 盲

- **后果**：`ftl_cmd_write_data_cw` 用 `1U << cw` 判位（plane 盲）：flat 位图里 plane 1 的 bits 4..7 在 bits 0..3 全 0 ⇒ GC/WL 迁移 plane 1 的每个 CW 被填 0xFF。
- **缓解**：从 `descriptor->paa` 解 plane，判位改用 `ftl_cw_bit(plane, cw)`。
- **下一步**：迁移 plane 1 数据后逐 CW 读回校验。
- **证据**：`src/ftl/ftl_fc_interface.c:ftl_cmd_write_data_cw`。
- **历史/验证**：
  - （日期未记录）修复：位图改 `ftl_cw_bit(plane, cw)` → 未上板

#### RISK-IF-02 超时后迟到完成解引用 NULL

- **后果**：`ftl_sync_submit_wait` 超时摘表后命令迟到完成 ⇒ dispatch `take` 得 NULL ctx，`ftl_sync_wait_callback` 解引用 NULL + 命令槽位泄漏。
- **缓解**：`ftl_sync_wait_callback` 对 `ctx == NULL`：LOGE + `fc_command_release`；合并 §2K GC-06 的迟到完成早退。
- **下一步**：超时后迟到完成不 HardFault、池不泄漏。
- **证据**：`src/ftl/ftl_op.c`。
- **历史/验证**：
  - （日期未记录）修复：NULL ctx 早退 + release → 未上板

#### RISK-IF-03 同步原语忽略数据搬运返回值

- **后果**：写失败仍编程、读失败仍 OK；raw 读缺 CW 静默跳过；短链读返回 OK 且尾部未初始化。
- **缓解**：三处均检查返回值并返回 `FTL_ERROR`。
- **下一步**：故障注入验证三处均上报错误。
- **证据**：`src/ftl/ftl_fc_interface.c`。
- **历史/验证**：
  - （日期未记录）修复：返回值检查 → 未上板

#### RISK-IF-04 接口行为变化部分板验

- **后果**：plane 1 迁移数据、wbuf 稀疏页、越段位图 release 拒绝、迟到完成路径，这些行为未经板验。
- **缓解**：编译零告警（release 两条 unused 已消失）。
- **下一步**：按 §5 板端清单回归。
- **证据**：`src/ftl/ftl_fc_interface.c` / `ftl_op.c`。
- **历史/验证**：
  - 2026-10-30 全盘写/校验 0 错误；GC 搬迁 41.4 万页，plane1 迁移与背压回退已走量 → 部分上板
  - （日期未记录）上述路径改动 → 未上板

#### RISK-IF-06 chain 派发不处理 RAID encode

- **后果**：`fc_poll_dispatch_chain` 没有单命令路径的 `#if CONFIG_FC_RAID` 分支。若 FTL 把写命令标成 `FC_CHAIN_MORE` 且 `raid=ENCODE`，整链写入会静默不写 parity（`CONFIG_FC_RAID=0` 时 ENCODE 分支不编译，不构成实际失效）。
- **缓解**：RAID 关闭（R5）⇒ `raid` 固定 NORMAL、ENCODE 分支不编译；FTL 当前不设置 `raid=ENCODE`。
- **下一步**：启用 RAID 或 FTL 开始设置 `raid=ENCODE` 之前，在 chain 路径补 stripe 分支或 `ASSERT(raid != ENCODE)`；加 UT 负例。
- **证据**：`src/fc/fc.c:1491-1499`（single 的 RAID 分支）vs `:1540-1593`（chain 无该分支）；`include/fc/fc.h:38-52`。
- **历史/验证**：
  - 2026-10-30 读码判定 → 待决策（RAID 关闭，无实际失效）

#### RISK-IF-07 批次 chain 违反契约

- **后果**：① 混优先级 ⇒ `fc_submit_batch` 把链拆到 SQ/HSQ 两段，`fc_poll_chain_length` 只沿单队列 peek，跳过的成员被当成不存在 ⇒ cache read 输出错页 / cache program 写错序列；② 末条 `MORE` 无后续 END ⇒ `chain_owner` 永不解锁，该 LUN 的另一队列被永久饿死。
- **缓解**：`fc_submit_batch` 拒绝混优先级；末条 MORE 改为合法（流式中间窗口），由 FTL cache 序列状态机保证最终 END；`ftl_op_submit*` 门控保证窗口间无插入；孤儿序列由 `ftl_cache_seq_idle_guard()`（1s）关闭。
- **下一步**：读/写流式窗口跨 chunk 的 END、D4 断点、wild-chain 负例。
- **证据**：`src/fc/fc_interface.c:fc_submit_batch`；`src/ftl/ftl_op.c` 门控；`src/ftl/cache_batch.c:cache_read_submit`；`include/ftl/ftl.h:ftl_cache_seq_s`。
- **历史/验证**：
  - 2026-10-30 5.2 万窗口、全盘 0 错误；跨 chunk 链正常 → 正常路径上板
  - 混优先级 / wild-chain 负例未注入

#### RISK-IF-08 page-read 聚合忽略 NAND 结果位

- **后果**：`fc_complete_from_nand` 的 page-read 分支只 OR `desc->status`，不 OR `nand_completion->cw_status`。① NAND 读 FAIL 不以 `NAND_FAIL` 上报；② NAND 读 TIMEOUT 若 buffer 仍是上一次有效数据且 `meta=FC_META_UNKNOWN`，ECC/CRC/META 都能过 ⇒ 命令报 READY，静默返回陈旧数据。
- **缓解**：状态字重构已修：page-read 分支按 flat CW 把 `cw_status[]` 并进 desc；命令级从 `pre_status`/`nand_status`/`execution_status` 映射 `NAND_FAIL`/`FAIL_CACHE`/`RDY`/`AREADY`/`WP`/`TIMEOUT`；TIMEOUT 一定进命令状态。
- **下一步**：注入 NAND 读超时并保留旧 buffer，确认命令状态带 `TIMEOUT`、FTL 不进成功路径。
- **证据**：`src/fc/fc.c:fc_complete_from_nand` Step 3；`src/nand/nand_engine.c:pre_jump_index`；`src/nand/nand_dispatch_tables.c:completion_per_cw`；`include/fc/fc_command.h:FC_CMD_STATUS_*`。
- **历史/验证**：
  - 2026-10-30 状态字重构修复 → 未上板

#### RISK-IF-09 放弃序列不发设备 LAST

- **后果**：跨 chunk 序列在「命令上下文不同 / 续接点不一致 / 续窗口首 pair 收不到 / 组被弃或失败 / 空闲超时」时由 `ftl_cache_seq_release()` 释放状态，但 NAND 侧仍停在 cache read 模式（预取页未消费）。若器件要求显式 `47h LAST` 或 `FFh RESET`，残序列会让后续读出错/挂起。
- **缓解**：只有异常入口才发生（正常路径必在 run 头 `START`、run 尾 `LAST`）；放弃后立刻允许普通读，并由 `ftl_cache_seq_idle_guard()` 限时 1s；读数据错误按 D7 逐页上报。
- **下一步**：读中途拔线/主机中止 → 再读同一/相邻 LBA 逐字节比对；若错页/超时，改为放弃时下发 `FFh RESET` 并复核 datasheet（R12）。
- **证据**：`src/ftl/cache_batch.c:ftl_cache_read_try`；`src/ftl/ftl.c:ftl_cache_seq_release` / `ftl_cache_seq_idle_guard`；`include/ftl/ftl.h:ftl_cache_seq_s`。
- **历史/验证**：
  - 2026-10-30 P1 读流式实现 → 未上板

#### RISK-IF-10 放弃的 MORE 链尾占住 chain_owner

- **后果**：流式窗口末条 `MORE` 后若下一窗口不再到来，链在 FC 侧没有 `END`；`chain_owner` 只由「末条 END」或「派发失败」清。期间同 LUN 的 HSQ 后台命令被永久饿死，直到下一次主机提交。
- **缓解**：FTL 门控 + `ftl_cache_seq_idle_guard()` 只释放 FTL 状态，不碰 FC 链；正常路径下一 chunk 在 ms 级到达并续链；USB BOT 串行 + CLI gate ⇒ 同一时刻只有一个主机命令。
- **下一步**：大顺序读中途中止/拔线，观察之后 CP/journal 是否恢复。2026-11-04 复核：全仓只有 RAID parity 命令设置过 `high_priority`，FTL 从未设置 ⇒ 所有命令进 SQ，`chain_owner` 恒为 SQ，不存在另一条队列 ⇒ 本风险后果在现状下不成立；只有把 host I/O 与后台拆到 SQ/HSQ 后才成立。
- **证据**：`src/fc/fc.c:fc_poll_fill_queue`；`src/ftl/cache_batch.c:cache_read_build`；`src/ftl/ftl.c:ftl_cache_seq_idle_guard`。
- **历史/验证**：
  - 2026-11-04 复核：单队列（SQ），后果不成立 → 信息
  - 2026-10-30 P1 读流式实现 → 未上板

#### RISK-IF-11 逐页确认依赖完成顺序与 FAILC

- **后果**：缓存程序第 i 页的最终结果由第 i+1 条命令的 `FAIL_CACHE` 给出，确认 FIFO 按提交顺序 pop（头==自己的页才跳过）。若 FC 完成顺序被破坏，或器件 FAILC 的「上一页」归属与实现不符，会把成功页标失败/把失败页当成功（静默坏数据）。
- **缓解**：FC 每 LUN FIFO 严格先进先出；FAILC 只在 op44/op48 映射（op49 首页不映射）；任一错误位 ⇒ 整个序列中止、未确认页一律不确认映射、关闭 hot SB。
- **下一步**：注入单 plane/双 plane 中间页 program 失败，确认失败页及其后页不确认、之前页已确认；若器件 FAILC 归属与假设不符，改为按 desc 逐 CW 状态归因。
- **证据**：`src/ftl/ftl.c:ftl_cache_wr_member_done`；`src/fc/fc.c:778`；`src/nand/nand_fmc.c:nand_fmc_exec_check_fail_cache`。
- **历史/验证**：
  - 2026-10-30 10.1 万页确认、cw abort=0、全盘 0 错误 → 正常路径上板；失败注入未做

#### RISK-IF-12 确认待入账 × 序列门控退化

- **后果**：序列打开时 L2P 加载 / journal 刷页的提交被 `ftl_op_seq_gate` 挡住；若确认因此待入账且最老的组被占住（两槽用满），下一 chunk 无空闲槽 ⇒ 序列看不到下一窗口、待入账无法推进，直到 idle 守卫（1s）中止序列（尾页按错误结算，host 重试）。
- **缓解**：开序列前预加载窗口 L2P region；每窗口 `jrn_reserve` 预留本窗口条目，失败即本窗口 END；下一窗口 L2P 驻留 lookahead；`ftl_cache_seq_idle_guard()` 兜底。
- **下一步**：大顺序写 + free pool 低位测吞吐、IO_ERROR 计数与最长写停顿；若出现 >100ms 停顿或非注入性 IO_ERROR，优先增加第三槽或把「预留失败立即 END」扩展到 ring 级。
- **证据**：`src/ftl/cache_batch.c:ftl_cache_write_try`；`src/ftl/ftl.c:ftl_cache_seq_idle_guard`；`src/hil/usb_msc.c` 槽状态机。
- **历史/验证**：
  - 2026-10-30 free=2 + 持续 GC，未观测 1s 停顿/IO_ERROR → 满盘压测上板；注入式验证未做

#### RISK-IF-13 门控覆盖与后台/RR 交互

- **后果**：判定结论（非缺陷）：`ftl_op_submit*` 是唯一提交漏斗，门控覆盖 host/GC/WL/CP/journal/metadata/L2P/RR。各后台路径都把拒绝当 `FTL_AGAIN` 重试；被拒提交置 `break_requested`，序列在下一窗口收尾。
- **缓解**：后台提交 deadline（10ms）短于 USB 一个 chunk 间隔 ⇒ 写序列打开期间 META/CP/MIG/RR 每 10ms abort 并刷屏。修复：四类 job 的 stall 检查先看 `ftl_op_cache_seq_active()`，序列打开时挂起 deadline；2026-11-01 统一为 `ftl_op_bg_submit_deferred()`（序列打开或仍有未完成主机组）。
- **下一步**：大顺序读写 + 后台 GC 压测，观察 `d4=`、`mig=`、`rrab=`；后台长时间无进展时检查门控是否未收尾。
- **证据**：`src/ftl/ftl_op.c` 门控；`src/ftl/migrate.c:265`；`src/ftl/read_recovery.c:146`；`src/ftl/journal.c:451`。
- **历史/验证**：
  - 2026-11-01 判据收敛为 `ftl_op_bg_submit_deferred()` → 审计结论
  - 2026-10-30 17:33 满盘写+校验无 stalled 刷屏，GC/CP/journal 均推进 → 上板验证

#### RISK-IF-15 RAW 读判空把已编程页当空白

- **后果**：FC 只向 FTL 暴露 USER 512B，`ftl_sync_read_page_raw()` 用 LDPC_RAW 刻意跳过 ECC；于是「用户数据恰好全 0xFF」的已编程页被判成未写过。触发：TRIM 掉整个 region ⇒ flush 写出 2048B 全 0xFF 的 region 页（META/CRC/ECC 已编程）⇒ `md_log_block_erased()` 判整块已擦净 ⇒ `md_log_area_restore` 置 NONE 而不擦 ⇒ 下次分配同页二次编程（AND）⇒ CRC/ECC 不符。同一误判还影响 full rebuild 写指针扫描与 BBT/CP 空片探针。
- **缓解**：判据搬回解码路径：① FC `fc_cw_is_blank()` = 整条 528B CW 的 1 比特占比 ≥99%，仅 ECC 解码失败后调用；② 新增 `fc_get_cmd_all_cw_empty()`；③ FTL `ftl_page_blank_check()` / `ftl_page_is_blank()` 普通读一次判空，读不通按非空；④ 块状态路径不再判空（`md_log_area_restore` 全部非活块置 LOG + 登记回收，擦除按水位懒执行）；⑤ 介质探针改 `ftl_page_is_blank`；⑥ 删除 `ftl_page_is_blank_raw()`。
- **下一步**：format → 整 region TRIM → 断电重启，确认日志块被擦除而非 release，再写无 `region degrade`/CRC 错；观测 full rebuild 耗时；99% 阈值为推导值，若空页被判非空下调到 98%。
- **证据**：`src/fc/fc_data_path.c:fc_cw_is_blank`；`src/fc/fc_interface.c:fc_get_cmd_all_cw_empty`；`src/ftl/ftl_fc_interface.c:ftl_page_blank_check` / `ftl_page_is_blank`；`src/ftl/metadata.c:md_log_area_restore`；`src/ftl/power_failure_recovery.c:pfr_scan_write_pointer`；`src/ftl/bbt.c:bbt_medium_has_ftl_data`；`src/ftl/journal.c:jrn_page_raw_blank`。
- **历史/验证**：
  - 2026-11-01 判据搬到解码路径 + 新增 `ftl_page_is_blank` → 正常路径上板
  - 2026-11-01 release 全量 UT 156/156、format --verify + H2testw 415/423 MB 0 错误、拔插后校验 195 MB 0 错误
  - 负例（全 0xFF 用户区的已编程页）未构造

#### RISK-IF-16 timeout_us 名不副实

- **后果**：`ftl_sync_submit_wait` 的 `timeout_us` 实现是自旋轮数，头文件却承诺微秒。
- **缓解**：改用 `delay_time_now_us` / `delay_time_elapsed_us` 实现真实微秒 deadline；0 仍默认 10ms；时钟不可用时退化为同值轮数兜底；上限 65ms（`delay_time_elapsed_us` 的 timeout 是 WORD），当前调用点最大 20ms。20ms 对擦除足够：NAND busy 预算 5ms（tBERS typ 700µs × ~7）。
- **下一步**：`format --verify` / 全片擦除不再出现「好块被判擦除失败」；将来 >65ms 的同步等待改用 `now_us` 差值。
- **证据**：`src/ftl/ftl_fc_interface.c:ftl_sync_submit_wait`；`include/ftl/ftl_fc_interface.h` 契约；`src/nand/onfi_1_0.c:NAND_TIME_TBERS_US/TPROG/TR`。
- **历史/验证**：
  - 2026-11-04 改为真实微秒 deadline → 未上板

#### RISK-IF-17 超时后释放仍在飞的命令

- **后果**：`ftl_sync_submit_wait` 超时只 `ftl_op_req_remove` 后返回 FTL_ERROR，命令已提交、仍在 FC/NAND 管线；8 个同步封装与 `gc_read_page_with_lba` 都在 `!= FTL_OK` 时 `fc_command_release`。释放会立即归还 desc 链与 buffer（NAND 可能仍在用 ⇒ 跨命令数据破坏），并把槽位放回 free list；迟到完成走 drain 的 missing-request 分支再释放一次 ⇒ 可能释放已被新命令复用的槽。
- **缓解**：超时只在「已提交但 10/20ms 内无完成」时发生；已改：`ftl_sync_submit_wait` 增加 `bool *submitted` 出参；新增 `ftl_sync_wait_owned` 唯一关口（未提交才 release），8 个同步封装改用它。
- **下一步**：注入一次「提交后不产生完成」，确认无二次 release/错槽释放日志；设计文档 §3.10.5/§3.10.6 待定稿同步（RISK-DOC-04）。
- **证据**：`src/ftl/ftl_fc_interface.c:ftl_sync_submit_wait` 及 8 个 `ftl_sync_*`；`src/ftl/gc.c:gc_read_page_with_lba`；`src/ftl/ftl_op.c:ftl_op_drain`；`include/ftl/ftl_op.h`。
- **历史/验证**：
  - 2026-11-05 加 `submitted` 出参与 `ftl_sync_wait_owned` 关口 → 未上板

#### RISK-IF-18 接口层 release 守卫缺口

- **后果**：4 处（H14）：① `ftl_cmd_setup_ppa_pair_cw` 空位图时 `first_plane == PLANE_NUM`，release 裁空 ASSERT 后仍读 `pba[PLANE_NUM]`（越界）再把垃圾 PBA 填进命令头；② `ftl_async_write_pair_cmd` 的 cache-op 覆盖全 plane 只有 ASSERT；③ 同一函数 pair ch/ce/lun 一致性只有 ASSERT；④ `ftl_sync_submit_wait` 的 `max_wait <= UINT16_MAX` 只有 ASSERT，release 下截断。
- **缓解**：已改：① 空位图在算 `first_plane` 前拒绝；② cache-op 单 plane 改为 release 拒绝；③ pair 跨 die 改为 release 拒绝；④ `max_wait` 入口 clamp 到 `UINT16_MAX`。
- **下一步**：UT 构造负例。
- **证据**：`src/ftl/ftl_fc_interface.c:ftl_cmd_setup_ppa_pair_cw` / `ftl_async_write_pair_cmd` / `ftl_sync_submit_wait`。
- **历史/验证**：
  - 2026-11-05 四处 release 守卫补齐 → 未上板

---

## 2G. Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`）

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-HOST-01 | 待上板 | host 读成败只看 NAND_FAIL，UNC/MATA 被当成功 | 未上板 |
| RISK-HOST-02 | 待上板 | multi-plane 读只记首个 desc 的块，第二块漏记 ECC/read_count | 未上板 |
| RISK-HOST-03 | 待上板 | 短链读返回值被丢弃，未初始化缓冲交给 host | 未上板 |
| RISK-HOST-05 | 待定位 | confirm park timeout = host 写失败，上游三支未定 | 2026-10-31 上板复现 |
| RISK-HOST-06 | 待上板 | 写 program fail 直接返回 IO_ERROR，未重发 | 未上板 |
| RISK-HOST-07 | 待上板 | 停泊确认路径重入，oldp 陈旧/映射分叉 | 未上板 |
| RISK-HOST-08 | 待上板 | TRIM 已异步；CLI 诊断与 CP 冷启动 locate 仍同步 | 部分上板（2026-10-22） |
| RISK-HOST-09 | 待后续 | TRIM 命中 wbuf 未编程 LBA 时不失效旧映射 | — |
| RISK-HOST-11 | 待定位 | Journal 压力下 TRIM 持久化可能丢失 | 2026-10-31 上板通过（journal-only 路径） |
| RISK-HOST-12 | 待议 | 组槽耗尽返回 FTL_ERROR；修法在 controller | — |
| RISK-HOST-14 | 待上板 | 拔线后门控环形等待导致读回 MATA（根因已改，死锁路径未验） | 2026-11-01 无回归 |
| RISK-HOST-16 | 待上板 | 「无进展时长」预算实际未生效（死赋值） | 2026-11-05 编译通过 |
| RISK-HOST-17 | 待上板 | host queue entry 生命周期，漏 release/重复 release | 2026-11-05 代码复核 |

### 详情

#### RISK-HOST-01 host 读成败判据不全

- **后果**：host 读成败只看 `FC_CW_STATUS_NAND_FAIL`：ECC 不可纠只置 `UNC`、页内 META 不符只置 `MATA_ERROR`，两者的 `NAND_FAIL` 恒清 ⇒ 加扰垃圾按 `SSD_STATUS_OK` 交给 host。
- **缓解**：改为 `fc_cw_status_has_error(completion->status)`（全集）；失败分支本就走「托管给 RR」。
- **下一步**：造一次 ECC 不可纠 / MATA 读（写某 LBA → 擦掉其映射所指块 → 读该 LBA），确认 host 得 `IO_ERROR` 且 `[RR]` 心跳可见。
- **证据**：`src/ftl/ftl.c:ftl_host_io_callback`；`include/fc/fc_command.h:fc_cw_status_has_error`。
- **历史/验证**：
  - （日期未记录）判据改为全集 → 未上板

#### RISK-HOST-02 multi-plane 读只记首个 desc

- **后果**：host 读成功路径只记首个 desc 的 pba，multi-plane 第二块漏记 read_count/ECC。
- **缓解**：`ftl_cmd_block_ecc_list` 逐 plane 反查命令访问的全局块与 ECC。
- **下一步**：multi-plane 读：两个块的 read_count / ECC 都增长。
- **证据**：`src/ftl/ftl_fc_interface.c`；`src/ftl/ftl.c:ftl_host_io_read_ok`。
- **历史/验证**：
  - （日期未记录）修复：逐 plane 反查 → 未上板

#### RISK-HOST-03 短链读返回值被丢弃

- **后果**：`ftl_cmd_read_data` 返回值被丢弃：desc 链比请求短时尾部不写，未初始化缓冲被当数据交给 host。
- **缓解**：检查返回值：失败 ⇒ `LOGE` + `SSD_STATUS_IO_ERROR`，并跳过该次的 ECC/read_count 记账。
- **下一步**：短链读必须报错（故障注入）。
- **证据**：`src/ftl/ftl.c:ftl_host_io_callback`。
- **历史/验证**：
  - （日期未记录）修复：返回值检查 → 未上板

#### RISK-HOST-05 confirm park timeout 是 host 写失败

- **后果**：待入账超过 100ms（固定预算）后 `ftl_confirm_service` 释放 pending 槽并按 `IO_ERROR` 完成组——数据已编程但 Journal/L2P 未记账 ⇒ 写按失败返回、物理页作废（无静默错值）。上游三支：(a) `l2p_load_start` 的三种失败（无候选行 / 命令池空 / 背压）只返 false 不打日志，`l2p_is_ready` 永远 false——最可能是 64 行全 DIRTY + Free Pool 空；(b) 同 region 加载 3 次失败后降级（LOGE）；(c) 行卡在 FLUSHING（未观测到）。日志 5s 限流且只打扫描到的第一条，`idx=0` 不代表只有它在待入账。
- **缓解**：超时按失败结算；`pend_confirm_park` 只改 op/owners。已补诊断：`l2p_load_start` 三因限流日志（`no_evictable` 打现场计数）；park 日志加 `parked=`。后台 job 提交 deadline 挂起判据放宽为 `ftl_op_bg_submit_deferred()`；待入账超时预算改为「无进展时长」（`ftl_confirm_progress_pending()`）。
- **下一步**：复现时取判据：debug 档 `[L2P] load copy failed` / 心跳；`info` 的 free/op/bad 与 FC 资源；有无 `region ... degrade`。修法候选：失败原因分级限流、超时日志带条数+首个 LBA、预算按后台进展动态判定。
- **证据**：`src/ftl/pending.c:pend_confirm_settle`；`src/ftl/ftl.c:ftl_confirm_service` / `ftl_confirm_write`；`src/ftl/l2p_cache.c:l2p_line_evict` / `l2p_load_start` / `l2p_flush_kick`；`include/ftl/ftl_module.h:FTL_GROUP_DRAIN_TIMEOUT_MS`。
- **历史/验证**：
  - 2026-11-01 release UT 在 degraded 介质上命中一次 `confirm park timeout lba=40448`（RISK-MD-06 下游症状，非独立复现）
  - 2026-10-31 23:47 cache 写压测复现：`lba=22512/50672`，前置 `[CP] submit stalled` + `[JRN] erase kick failed` ⇒ 支路 (a) 稳定可达
  - 2026-10-21 H2testw 确认支路 (a)：`region=568 cause=no_evictable` 与 `confirm park timeout` 同窗口，CP 卡在 phase=1
  - 2026-10-21 rev33 命中 `load start failed cause=no_evictable` 瞬态，同窗口无 confirm timeout

#### RISK-HOST-06 写 program fail 直接返回 IO_ERROR

- **后果**：坏块被标 BAD 后本页数据未重发，H2testw 等 host 会看到一次写失败。
- **缓解**：从失败命令的 desc 链读回本页，分配新页对、重建命令并重发一次（`FTL_WRITE_RETRY_MAX=1`）；同一 pending 保持，组在飞数不递减。
- **下一步**：debug `bbt fault` 人为标坏即将写入的 data 块，`[FTL] write retry` 出现且 host 写仍成功；第二次失败仍如实 IO_ERROR。
- **证据**：`src/ftl/ftl.c:ftl_write_retry` / `ftl_host_io_callback`；`include/ftl/pending.h:retry_count`；`include/ftl/ftl.h:FTL_WRITE_RETRY_MAX`。
- **历史/验证**：
  - 2026-10-21 写重试路径落地 → 未上板

#### RISK-HOST-07 停泊确认路径重入

- **后果**：正常完成路径在 `ftl_op_dispatch` 内，`jrn_append` 页满且 `jrn_freeze` 失败时只丢弃不泵；但 `pend_confirm_service` 在主循环调它，同一条件下 `jrn_append` 会走 `jrn_flush_page` → `ftl_sync_write_page` → `ftl_op_drain` 重入 `ftl_host_io_callback`/`ftl_confirm_write`。后果：① 批量化在 Journal 循环前一次读完 `oldp[]`，重入确认改同一页 LBA ⇒ `oldp` 陈旧（old_pba 链断、valid_count 重复扣减）；② 重入 `l2p_load_start` 可把本行 kick 成 FLUSHING ⇒ `l2p_entries_write` 返 `FTL_AGAIN`，release 下 `(void)wst` 静默跳过 L2P 更新（Journal 已追加）⇒ 运行期映射分叉，掉电回放才恢复。
- **缓解**：`ftl_op_is_in_dispatch()` 挡不住 `pend_confirm_service`；需在停泊路径加「禁止重入确认」或把 Journal 泵移出。
- **下一步**：上板在 Journal 页满 + 停泊确认同窗口构造；观察 old_pba 链与 valid_count。
- **证据**：`src/ftl/ftl.c:ftl_confirm_write` / `ftl_confirm_service`；`src/ftl/journal.c:jrn_append` / `jrn_flush_page`。
- **历史/验证**：
  - 2026-10-22 改为全异步/背压方向 → 未上板

#### RISK-HOST-08 TRIM 已异步，残留同步点

- **后果**：TRIM 已改异步；残余非数据面同步点：① CLI 诊断 `map`/`rdpaa` 用 `l2p_read_sync`/`ftl_sync_read_page`；② CP 冷启动首次 `md_ring_locate` 同步扫池。后果：诊断/冷启动首次 CP 会阻塞主循环一次；不造成静默数据破坏。
- **缓解**：`ftl_trim_begin/step` + Controller 每轮按 run 推进；CLI 是交互式控制面；CP locate 只在游标未建立时发生一次。
- **下一步**：TRIM 后 `map` 为 INVALID、立即断电重启不复活；长 TRIM 期间 USB/后台不停摆；若要求字面全异步，拆 `md_ring_locate` 或裁决「控制面例外」。
- **证据**：`src/ftl/ftl.c:ftl_trim_begin/step`；`src/hil/controller.c:ssd_controller_trim_service`；`src/hil/cli.c:map` / `rdpaa`；`src/ftl/checkpoint.c:CP_DIR_CKPT_PREP` / `md_ring_locate`。
- **历史/验证**：
  - 2026-10-22 TRIM 写/擦/断电回放已验、全盘 H2testw 已过 → 部分上板

#### RISK-HOST-09 TRIM 命中 wbuf 未编程 LBA

- **后果**：`ftl_trim_step` 的 `wb_has(l)` 分支认为「它从未落盘，无旧映射可失效」，但覆盖写场景下该 LBA 的旧映射仍指向旧 PBA。若 TRIM 与未完成 host 写并发，drop 后旧映射继续可读（TRIM 语义被破坏），valid_count 也不减。
- **缓解**：Controller 串行化（CLI/USB gate + TRIM 单飞行）；`wb_handle(DROP)` 只清未编程槽并结算 owners；2026-11-05 USB UNMAP 已加内部 FLUSH 屏障 + 数据槽 idle 后才 TRIM。
- **下一步**：接入并发 TRIM 前必须裁决：TRIM 前等该 LBA 写完成/落盘，或 drop 时同步失效旧 L2P。上板：`wr` 一串后立即 `tr` 同 LBA，`map` 必须 INVALID。
- **证据**：`src/ftl/ftl.c:ftl_trim_step` 的 `wb_has` 分支；`src/hil/controller.c` TRIM 单飞行；`src/hil/cli.c` gate。
- **历史/验证**：
  - 2026-10-22 读码判定：当前串行化下不可达 → 待后续
  - 2026-11-05 USB UNMAP 加屏障后仍不可达

#### RISK-HOST-11 Journal 压力下 TRIM 持久化可能丢失

- **后果**：media 套件在 trim 前大量写映射后，`trim_no_resurrection` 重启前读回为 0、模拟重启后读到旧数据；trim 的 Journal 失效条目在回放时未生效（或 flush 屏障未覆盖该页），旧 L2P 快照胜出。
- **缓解**：UT 顺序规避；TRIM 仍依赖 host FLUSH。
- **下一步**：复现：trim 前用 matrix 写入同 LBA 区并 flush，再 trim+flush+模拟重启；检查 journal page_seq/truncate 与 CP 快照顺序。原始复现（先大量写映射再 trim）未重跑。
- **证据**：`src/ftl/ftl.c:ftl_trim_step` / `ftl_flush_step`；`src/ftl/journal.c:jrn_append` / `jrn_replay`。
- **历史/验证**：
  - 2026-10-31 `trim_no_resurrection` journal-only 路径板端通过
  - 2026-10-24 板端 UT 观测到重启后旧数据复活

#### RISK-HOST-12 组槽耗尽返回 FTL_ERROR

- **后果**：`ftl_submit_common` 在 `pend_group_alloc` 失败时返回 `FTL_ERROR`；组槽是瞬态资源，按 H38 应 `FTL_AGAIN`，当前会让 Controller/USB 把瞬态耗尽当永久失败（极难触发，需 32 组同时在飞）。
- **缓解**：组槽 32 个、正常负载远达不到；未观察到现场。
- **下一步**：裁决（2026-11-05）：只改 FTL 返回码无效——`ssd_request_split` 把任何非 OK 当永久失败；正确修法在 controller：split 遇 `FTL_AGAIN && submitted == 0` 时返回 BUSY 并保留请求，或加续跑游标；届时再改 FTL 返回码并补 UT。
- **证据**：`src/ftl/ftl.c:ftl_submit_common`；`include/ftl/pending.h:pend_group_alloc`；`src/hil/request.c:159`；`CLAUDE.md` H38。
- **历史/验证**：
  - 2026-11-05 读码核对调用链后裁决：暂不改 FTL，修法在 controller → 待议

#### RISK-HOST-14 拔线后门控环形等待

- **后果**：主机中途拔线后，L2P 64 行全脏 ⇒ 写路径需未驻留 region ⇒ `l2p_load_start()` 失败但 4 个调用点全部 `(void)` 丢弃 ⇒ 上层仍返 `FTL_AGAIN` ⇒ `l2p_defer` 被置 true（实际无加载在飞）⇒ `bg_progress` 把 `l2p_defer` 算作进展 ⇒ stall 永不累加、序列永不关闭 ⇒ `ftl_op_seq_gate` 拒绝一切后台提交 ⇒ 脏行永远排不出去，环闭合。宿主侧表现为重枚举后读回 MATA、H2testw Code 1392、`Only 191 of 423 MB tested`。
- **缓解**：新增 `l2p_cache_any_inflight()`（纯谓词），`l2p_defer` 的 4 个置位点改为按在飞计数派生；`cache_batch.c` 三处「序列收尾」不再借 `l2p_defer` 表达进展（`seq_closing` 已在 `bg_progress`）。加载启动失败不再被当进展 ⇒ stall 检测恢复区分力 ⇒ 组超时上报 IO_ERROR、序列关闭、门控打开。
- **下一步**：写到接近满盘（free 几十、`[GC] mode=emg`）时拔线，看 `drain stalled` 是否快速触发并自愈；UT 负例：构造 64 行全脏 + 一次加载失败，断言 `l2p_defer` 不被置位（未做）。
- **证据**：`src/ftl/ftl_op.c` 门控/组计数；`src/ftl/l2p_cache.c:l2p_cache_any_inflight` / `l2p_load_start`；`src/ftl/checkpoint.c:CP_FLUSH_REGIONS`；`src/ftl/read_recovery.c` MATA 退休；`src/hil/usb_msc.c` 拔线中止路径。
- **历史/验证**：
  - 2026-11-01 正常路径上板：写 200+ MB 后只拔 USB 不断电，重插 H2testw 校验 195 MB 0 错误；无 `dirty=64` 刷屏、无 phase=1 长等待。⚠ 未进入故障条件（起点 free=3390），只证无回归
  - 2026-11-01 读码定位环形等待并实施 `l2p_cache_any_inflight` → 未上板（死锁路径）

#### RISK-HOST-16 「无进展时长」预算未生效

- **后果**：`ftl_confirm_progress_pending()` 的唯一调用点在 `ftl_host_io_write_ok` 入队之前，随后 `pend_confirm_park()` 用同一时刻覆盖 `park_ms`；`ftl_confirm_service` 只查 `pend_confirm_is_expired`，不再刷新 ⇒ 预算仍是「从 park 起固定 100ms」。后台确有进展时也可能把写结算成 IO_ERROR。
- **缓解**：已改：判据移入 `ftl_confirm_service` 的 `FTL_AGAIN` 分支（`progress_pending == true` ⇒ `park_ms = now_ms` 并 continue，否则再判超时）；`ftl_host_io_write_ok` 的死赋值已删；「另一 host 组在飞」收紧为「任一非待入账 pending 项」。
- **下一步**：上板复现 `confirm park timeout` 时验证预算按进展延长；上界依赖 Journal 页写/擦除 deadline 与 NAND 每命令超时。
- **证据**：`src/ftl/ftl.c:ftl_host_io_write_ok` / `ftl_confirm_service` / `ftl_confirm_progress_pending`；`src/ftl/pending.c:pend_confirm_park`；`include/ftl/pending.h:pend_confirm_is_expired`。
- **历史/验证**：
  - 2026-11-05 判据移入服务重试路径 → 编译通过，未上板

#### RISK-HOST-17 host queue entry 生命周期

- **后果**：`ssd_host_queue` 的 entry 由 USB slot 提交/unmap start 分配，由 `usb_msc_slot_release` / CLI 完成 poll 释放；若某条失败/复位路径漏 release，8 个 entry 用尽后所有 host 提交都失败（USB I/O error）；重复 release 则 ASSERT 停机或 release 下 `used--` 下溢。
- **缓解**：entry 分配点只有两处，所有释放路径统一走 `usb_msc_slot_release`；CLI 在 submit 失败与完成 poll 两处释放；2026-11-05 复核：`q` 当前未输出 `hostq`（`ssd_host_queue_used()` 仅 UT 使用）⇒ 现场观测路径尚不存在。
- **下一步**：补一行 `hostq` 到 `q`；H2testw + 拔插/复位后 `hostq` 必须回 0；`/hil/host_queue` 覆盖 alloc/free/owner。
- **证据**：`src/hil/host_queue.c/.h`；`src/hil/usb_msc.c:usb_msc_slot_release` / `usb_msc_slot_submit` / `usb_msc_unmap_start`；`src/hil/cli.c:uart_cli_host_submit` / `uart_cli_pending_poll`；`tests/unit/test_hil.c:host_queue`。
- **历史/验证**：
  - 2026-11-05 复核：`q` 未输出 hostq，观测路径待补 → 未上板

### 已闭合（详情见 archive）

| ID | 日期 | 一句话 | 归档 |
|---|---|---|---|
| RISK-HOST-18 | 2026-11-12 | H2testw 近满盘确认超时/已回执写丢失（FC 预算修复后 406/420 MB 0 错误） | `docs/archive/known_risks_batches.md` |

---

## 2H. 完成派发面（`ftl_op.c/h`）

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-CR-01 | 待上板 | ctx 旁表不按 command_id 去重，id 复用得别人 ctx | 未上板 |
| RISK-CR-04 | 待上板 | request 表改 command_id 直接索引，删 op_tag | 2026-11-03 编译通过 |
| RISK-CR-05 | 待上板 | pending 表 command_id 直接索引 | 2026-11-04 编译通过 |
| RISK-CR-06 | 信息 | request add/remove 防御与注释口径不一致（已议不改断言） | 2026-11-04 代码核对 |
| RISK-FC-05 | 待定位 | 后台双 plane 页写留 SQ 头不发，teardown ctx 残留 | 2026-10-30 复现条件隔离 |

### 详情

#### RISK-CR-01 ctx 旁表不按 command_id 去重

- **后果**：`ftl_op_ctx_add` 只找首个空槽、不按 `command_id` 去重 ⇒ 旁表可存在两条同 id 项；`ftl_op_ctx_take` 取首个匹配 ⇒ id 复用后服务函数拿到别人的 ctx（可能是失效的 `cp_handle_t *` / 栈上 waiter），本命令表项永久泄漏。可达路径：CQ 满强制置 COMPLETED ⇒ 表项泄漏 → id 复用 → 再次 add。
- **缓解**：`ftl_op_ctx_add` 改两遍扫：先全表查 `cmd_id`，命中则原地覆盖 ctx + `LOGW`；未命中才取空槽 ⇒ 同一 cmd_id 至多一条表项（H10）。
- **下一步**：不可直接构造（需 CQ 满/丢完成）⇒ 静态收口 + LOGW；随常规回归上板，不宣称已取证。
- **证据**：`src/ftl/ftl_op.c:ftl_op_ctx_add`。
- **历史/验证**：
  - （日期未记录）修复：两遍扫 + 原地覆盖 → 未上板


#### RISK-CR-04 request 表改用 command_id 直接索引

- **后果**：`g_op_req` 改为按 command_id 直接索引（容量 = `FC_COMMAND_POOL_DEPTH`，元素 `{ctx, op, pending}`），删除线性扫描与 `used / cmd_id:15`；`fc_cmd_status_s` / `fc_cmd_s` 删除 `op_tag`。失败场景：① 非法 command_id 直接索引越界（`ftl_op_req_add` 入口返 `FTL_ERROR`）；② 仍读 `completion.op_tag` / `cmd->op_tag` 的代码会编译失败；③ 超时/迟到完成时表项已摘 ⇒ dispatch 走 missing-request 只 release；④ 要求 ctx 的 op 传 NULL 属编程错误：debug ASSERT，release 无运行期拒绝（回调用 NULL 崩溃，不是静默介质破坏）。
- **缓解**：表容量 = 池深、无表满；直接索引 O(1)；每条在飞命令都有 request。
- **下一步**：上板 H2testw + `[FTL]` 日志；UT `ftl_op_req_count` / request 表行为。
- **证据**：`src/ftl/ftl_op.c:ftl_op_req_add` / `ftl_op_drain`；`include/fc/fc_command.h:fc_cmd_s` / `fc_cmd_status_s`；`include/ftl/ftl_op.h`。
- **历史/验证**：
  - 2026-11-03 完成派发表主流化 → 编译通过，未上板

#### RISK-CR-05 pending 表 command_id 直接索引

- **后果**：`pend_ctx_s.slot_of[FC_COMMAND_POOL_DEPTH]` 保存 command_id → slots 下标，`pend_find` 不再线性扫。失败场景：① 释放/脱钩未清映射 ⇒ 新命令按旧 id 撞到已释放槽；② 同一 command_id 绑定两个槽 ⇒ `pend_bind` 断言；③ `FTL_PENDING_CMD_NONE` 脱钩项留在映射 ⇒ 按 id 误查；④ command_id ≥ 池深直接索引越界。
- **缓解**：绑定/解绑收敛到 `pend_bind` / `pend_unbind`；`pend_init` 全 0xFF；`pend_find` 再核对 `used && command_id`；`pend_alloc` 越界返回 NULL。
- **下一步**：上板 H2testw + UT pending；观察错误完成/槽泄漏。
- **证据**：`include/ftl/pending.h:pend_ctx_s`；`src/ftl/pending.c:pend_bind` / `pend_find` / `pend_remove` / `pend_release_index`；`src/ftl/ftl.c:ftl_write_retry` / `ftl_host_io_read_fail`。
- **历史/验证**：
  - 2026-11-04 改直接索引 → 编译通过，未上板

#### RISK-CR-06 request 表防御与注释口径

- **后果**：① `ftl_op_req_remove` 注释曾写「未登记由 debug ASSERT 停机」，实现没有该 ASSERT；② `ftl_op_req_add` 注释曾写「重复登记是编程错误（ASSERT）」，实现是 LOGW + 覆盖。
- **缓解**：按 H2，add/remove 的单次配对由调用链保证，内部入口不重复断言；覆盖分支保留为不变量损坏时的现场日志；只把 `.c` 注释改成与实现一致。
- **下一步**：若出现 `completion entry reused` 日志，按调用链断裂排查（谁释放了命令却没摘 request）。
- **证据**：`src/ftl/ftl_op.c:ftl_op_req_add` / `ftl_op_req_remove`；`include/ftl/ftl_op.h` 契约。
- **历史/验证**：
  - 2026-11-04 判定：不新增断言，只修注释 → 代码核对

#### RISK-FC-05 后台双 plane 页写留 SQ 头不发

- **后果**：`/speed/ftl_rw` 连跑 4 组合时，`cache rd=4` 结束后一条带 ctx 的后台双 plane 页写（`op=31`、`chain=END`）留在 SQ 头，`lun_used=0/lun_phase=IDLE/owner=NONE`，填充轮次在跑却没把命令发出去，teardown 断言 `ftl_op_req_count()==0` 失败。单独跑该组合不复现 ⇒ 依赖前序组合留下的状态。
- **缓解**：测试侧隔离：非首个组合开始时先 `ftl_ut_reboot_from_media()`（quiesce + `fc_init` + 重新武装 + PFR），只清 RAM、保留介质；FC 侧只读诊断 `fc_debug_state_get()` 保留。
- **下一步**：出现「host I/O 变慢/停住」或 teardown ctx 残留时，`ftl teardown leaked …` 行会带 `sq_head/st/bound/sched_sq/fill`，按该行定位。这不是修复，泄漏可能在，只是复现条件被隔离。
- **证据**：`src/fc/fc.c:fc_poll_fill_queue` / `fc_debug_fill_counters`；`src/fc/fc_interface.c:fc_debug_state_get`；`tests/unit/test_speed.c`；`tests/unit/ftl_ut_common.c`。
- **历史/验证**：
  - 2026-10-30 全套 test 148/148 通过（组合隔离后） → 复现条件消除，底层原因未证实


### 已闭合（详情见 archive）

| ID | 日期 | 一句话 | 归档 |
|---|---|---|---|
| RISK-CR-02 | 2026-11-12 | die→(ch,ce,lun) 分解改走统一公式，坐标类未板验 | `docs/archive/known_risks_batches.md` |

---

## 2I. 页打包写缓冲 wbuf（`wbuf.c` + `wbuf.h`）

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-WB-01 | 待上板 | FLUSH 失败丢弃页对，allocator 无归还 API，容量无声流失 | 未上板 |
| RISK-WB-02 | 待上板 | TRIM/RESET 由外部模块手工改 wbuf 内部不变量 | 未上板 |
| RISK-WB-03 | 待上板 | 模块抽出 + 位运算收敛 + 日志降级，wbuf 是写路径核心 | 未上板 |
| RISK-WB-04 | 待上板 | 组在飞数原由多处手工维护，漏减挂死 host 写 | 未上板 |
| RISK-WB-07 | 待定位 | wbuf 半页卡死，后续写全被拒、组永不推进 | 2026-11-01 复现 |
| RISK-WB-08 | 待上板 | WBUF_STATE_FAILED 不可达，错误锁存契约与实现矛盾 | 2026-11-04 编译通过 |

### 详情

#### RISK-WB-01 FLUSH 失败丢弃页对导致容量流失

- **后果**：`wbuf_handle(FLUSH)` 取到页对后 4 条失败返回（命令池满 / 建链 / pending 满 / SQ 满）丢弃该页对，而 allocator 无归还 API ⇒ 缓冲保留、每轮重试白烧 2 页，容量无声流失。
- **缓解**：资源次序改为按有无归还路径排序（命令槽先于页对）+ 新增 `alc_super_open_rewind_pair` + 统一错误出口归还页对。
- **下一步**：`next_page` 属地址/容量类量，必须上板；背压下命令池满不易构造 ⇒ 静态收口 + 可达路径收窄，不宣称已取证。
- **证据**：`src/ftl/wbuf.c:wbuf_handle(FLUSH)`；`src/ftl/allocator.c:alc_super_open_rewind_pair`。
- **历史/验证**：
  - （日期未记录）修复：资源次序 + 页对归还 → 未上板

#### RISK-WB-02 TRIM/RESET 手工改 wbuf 内部不变量

- **后果**：TRIM 清空窗口的内部不变量（清 `occupied` + 退 `owners` 在飞数 + 复位）由 `ftl_trim` 手工维护；`cli.c` 的 `bbt --erase` 直接 `memset(&ftl->wbuf,…)` 丢弃 `owners` ⇒ 那些写组在飞数永不归零、host 写挂死。
- **缓解**：不变量收进 wbuf 模块：新增 `wbuf_handle(DROP)`（TRIM）/ `wbuf_handle(RESET)`（介质擦净，按写失败收尾）；调用点改为调接口。
- **下一步**：`trim` 走新路径不回归；`bbt --erase` 后不再有写组挂死。
- **证据**：`src/ftl/wbuf.c`；`src/ftl/ftl.c:ftl_trim`；`src/hil/cli.c`。
- **历史/验证**：
  - （日期未记录）修复：DROP/RESET 接口收敛 → 未上板

#### RISK-WB-03 模块抽出与等价改写

- **后果**：模块抽出（`ftl.c` §4 → `wbuf.c`/`wbuf.h`）+ 位运算收敛到 `ftl_cw_bit` + hot-path 日志降 `LOGD`：纯搬移与等价改写，但 wbuf 是写路径核心。
- **缓解**：逐段比对搬出前后函数体一致；`make all` / `make release` 零新告警。
- **下一步**：随常规回归上板（`wr`/`rd`/`fl`/`trim` + `wp <lba> 1 8` 合并仍为 1 次编程）。
- **证据**：`src/ftl/wbuf.c` / `include/ftl/wbuf.h`。
- **历史/验证**：
  - （日期未记录）模块抽出 → 未上板

#### RISK-WB-04 组在飞数多处手工维护

- **后果**：组在飞数原由 wbuf/ftl/pending 多处手工维护，漏减会挂死 host 写组。
- **缓解**：收敛为 `pend_group_add_subop` / `pend_group_drop_subop` 唯一闸门。
- **下一步**：写路径回归：组完成/回滚均在飞数归零。
- **证据**：`src/ftl/pending.c`；`src/ftl/wbuf.c` / `ftl.c` 调用点。
- **历史/验证**：
  - （日期未记录）修复：唯一闸门 → 未上板

#### RISK-WB-07 wbuf 半页卡死

- **后果**：`[FTL] drain stalled gi=1 next=0/8` 的 dump 显示 `wb_busy=1` 但 FC 池全空（cmd/desc/buf/sq/cq 全 0）、`pend=0`——没有任何在飞，也没触发 wbuf 空页刷出；`cache_stream.wb_declines` 涨到 41,948、组一个子操作都发不出去，180s 后 phase 超时。默认深度（normal 4 / cache 2）下未观测到。怀疑与「半页只集齐一个 plane、同伴 plane 的数据由另一组提供而该组被挡」有关。
- **缓解**：无（默认深度不复现）；测试深度已改回默认。
- **下一步**：用 8-LBA × 深度 2 做最小复现，检查 `ftl_issue_subop` 在「wbuf 占用且本组 LBA 不匹配」时是否真的触发 `WBUF_TRIGGER_FLUSH`、flush 起不来时返回路径是否被当成 DECLINED 而非 AGAIN/阻塞。
- **证据**：`src/ftl/wbuf.c`；`src/ftl/ftl.c:ftl_issue_subop` / `ftl_drain_stall_dump`；板端 2026-11-01 02:00 日志。
- **历史/验证**：
  - 2026-11-01 02:00 normal 档写深度 2 复现 → 待定位

#### RISK-WB-08 WBUF_STATE_FAILED 不可达

- **后果**：`wb_handle` 每次调用都执行 `h->error = (st == FTL_ERROR) ? FTL_ERROR : FTL_OK`，而 flush 步唯一带 out 的调用是 `MAYBE_FLUSH_IDLE`，其分支不设置 `st` ⇒ idle flush 撞上永久错误（建链失败 / closed 列表满）后，下一次 flush 步立刻把 error 清回 OK ⇒ `ftl_flush_step` 的 `wb_fail` 恒假，host FLUSH 只能等 1s 超时后 best-effort 收尾，而不是快速失败。文档「error 只由下一次成功 FLUSH/RESET 清除」与实现不符。
- **缓解**：`wb_maybe_flush_idle` 改返回 `ftl_status_e` 并在 switch 里赋给 `st`；错误锁存改为「任一 kind 返回 FTL_ERROR 置位，只有成功的 FLUSH / MAYBE_FLUSH_IDLE / RESET 清除（APPEND/DROP 成功不清）」。
- **下一步**：构造建链永久失败（closed 列表满或注入），确认 host FLUSH 快速返回失败而不是 1s 超时。
- **证据**：`src/ftl/wbuf.c:448/477-486`；`src/ftl/ftl.c:3306-3329/2223`。
- **历史/验证**：
  - 2026-11-04 错误锁存改为成功 FLUSH 才清 → 编译通过，未上板

---

## 2J. FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`）

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-BG-01 | 待上板 | 时钟缺失时 RR 巡检门恒假，预测巡检永久停摆 | 未上板 |
| RISK-BG-04 | 待上板 | 任务表装载无边界检查，加项越界踩 task_count | 未上板 |
| RISK-BG-05 | 待上板 | 调度器死字段 + priority 与表序两套坐标漂移 | 未上板 |
| RISK-BG-06 | 待上板 | l2p_flush_max 经 level 反查，第二套配置读取 | 未上板 |
| RISK-BG-07 | 待上板 | journal_erase 手工读 journal 内部 phase/need_erase | 未上板 |
| RISK-BG-10 | 待上板 | 后台节拍 flag + token bucket（grant 太小/太大风险） | 2026-11-03 编译通过 |
| RISK-BG-11 | 待上板 | 档位下标无值域断言，越界静默读相邻数据 | 2026-11-04 编译通过 |
| RISK-DOC-03 | 待上板 | ftl.c 注释/代码分叉（H6/H37） | 2026-11-05 编译通过 |
| RISK-LOG-02 | 待上板 | GC 事件在极端 victim 速率下仍可能占 UART | 2026-10-23 上板 |
| RISK-LOG-03 | 待上板 | GC no-gain 刷屏（边沿复用） | 2026-10-23 复现 |

### 详情

#### RISK-BG-01 时钟缺失时 RR 巡检门恒假

- **后果**：`bg_task_rr` 的内部门 `(now_ms - rr_last_scan_ms) >= RR_SCAN_INTERVAL_MS` 在时钟缺失（`ftl_now_ms()` 恒 0）下恒为假 ⇒ 慢死预测巡检永久停摆；与 `mig_gc_evaluate` 的哨兵语义方向相反。
- **缓解**：补 `|| (rr_last_scan_ms == 0U)` 哨兵，与 GC 同构。
- **下一步**：上板确认 1s 节流仍在（巡检不逐轮跑）。
- **证据**：`src/ftl/bg_scheduler.c:bg_task_rr`。
- **历史/验证**：
  - （日期未记录）修复：哨兵 → 未上板

#### RISK-BG-04 任务表装载无边界检查

- **后果**：加第 14 项会 `tasks[13]` 越界写踩掉 `task_count`（静默失效，编译期无声）；适配器注释里的位号（12/13）与真实下标（11/12）不符。
- **缓解**：改表驱动 `s_bg_tasks` + `_Static_assert` 容量门 + `bg_scheduler_init` 直接取编译期表长装载；任务位改由表项 `.mask` 显式声明。
- **下一步**：IDLE/NORMAL/BUSY 各跑一轮，确认 10 项照常执行。
- **证据**：`src/ftl/bg_scheduler.c:s_bg_tasks` / `bg_scheduler_init`。
- **历史/验证**：
  - （日期未记录）修复：表驱动 + 静态断言 → 未上板

#### RISK-BG-05 调度器死字段与两套坐标

- **后果**：`bg_scheduler_t` 死字段 `priority`/`round_total_us`/`round_used_us`/`round_seq`/`level`/`clock_ok` 无生产消费者；`priority` 与表序是同一语义的两套坐标且已漂移（取值跳过 5）。
- **缓解**：删除；`clock_ok` 降为 `bg_scheduler_run` 局部变量；表序即优先级（C9）。
- **下一步**：上板确认无行为变化（表序未变）。
- **证据**：`include/ftl/bg_scheduler.h`；`src/ftl/bg_scheduler.c`。
- **历史/验证**：
  - （日期未记录）删除死字段 → 未上板

#### RISK-BG-06 l2p_flush_max 第二套配置读取

- **后果**：`bg_task_l2p_flush` 经 `ftl->bg.level` 反查 `s_bg_level_cfg` 取 `l2p_flush_max`，与引擎实际收到的 `cfg` 可能不同步（引擎按形参跑、适配器按字段读）。
- **缓解**：引擎在 `bg_scheduler_run` 起点发布 `sched->cfg = cfg`，适配器读 `ftl->bg.cfg->l2p_flush_max`（单写点）。
- **下一步**：上板 BUSY 档 flush 条数应为 1（`BG_BUSY_FLUSH_COUNT`）。
- **证据**：`src/ftl/bg_scheduler.c:bg_task_l2p_flush`。
- **历史/验证**：
  - （日期未记录）修复：单写点 → 未上板

#### RISK-BG-07 journal_erase 手工读 journal 内部状态

- **后果**：`bg_task_journal_erase` 手工读 `job.phase` / `job.need_erase`；journal 改内部表示后该守卫静默恒假 ⇒ 换块擦除永不启动 ⇒ 新环形块未擦就写。
- **缓解**：判据收回 owner 模块：`jrn_handle(ERASE_ONLY)`，行为与原守卫逐字等价（`phase == JRN_IDLE && need_erase != 0` 才调）。
- **下一步**：跑 journal 换块路径（写满一个环形块），确认擦除照常补起。
- **证据**：`src/ftl/journal.c:jrn_handle(ERASE_ONLY)`。
- **历史/验证**：
  - （日期未记录）修复：判据收回 owner → 未上板

#### RISK-BG-10 后台节拍 flag + token bucket

- **后果**：CP T3 / L2P flush / BBT 起新 job 改由 `ssd_ftl_bg_tick_task`（10ms）置 flag；`total_units` 每轮配额改为 `grant_units`/10ms 的 credit（IDLE/NORMAL 4、BUSY 1，上限 4 个 interval）。失败场景：① grant 太小 ⇒ 后台吞吐下降，Free Pool drain 后 host 写 `FTL_AGAIN`；② grant 太大 ⇒ 后台抢 host 带宽；③ flag 漏置/未消费 ⇒ L2P 脏行累积、BBT 不落盘、CP T3 延迟；④ timer 注册失败（表满）⇒ flag 永不置（有 LOGE）；⑤ 紧急 CP 不重置 T3。
- **缓解**：must-run 与在飞推进不变；L2P flush 10ms、BBT 1s、CP T3 10s 判据；flag 消费即清；`BG_ROUND_HARD_LIMIT_US` 保留；注册失败打 LOGE。
- **下一步**：GC/WL 吞吐、host 写延迟、`[GC] trig` ≤10ms/次、H2testw 0 错误；观察 `[SYS] bg tick task add failed`。
- **证据**：`src/main.c:ssd_ftl_bg_tick_task`；`src/ftl/bg_scheduler.c:bg_scheduler_run` / `bg_scheduler_refill`；`src/ftl/checkpoint.c:cp_timer_poll` / `cp_should_start`；`src/ftl/bbt.c:bbt_handle`；`src/ftl/l2p_cache.c:l2p_handle`。
- **历史/验证**：
  - 2026-11-03 实现节拍 flag + token bucket → 编译通过，未上板

#### RISK-BG-11 档位下标无值域断言

- **后果**：`bg_scheduler_service` 用 `level` 直接索引 `s_bg_level_cfg[level]`；越界不会崩，会静默读相邻静态数据 ⇒ 预算/掩码全错而无声。
- **缓解**：入口 `ASSERT(level < BG_LEVEL_NUM)`；并在表旁注明「grant_units 必须 ≥ 本档位预算任务数」。
- **下一步**：改选档逻辑或加预算任务时重访：档位越界与 grant < 任务数都在这里核对。
- **证据**：`src/ftl/bg_scheduler.c:bg_scheduler_service` / `s_bg_level_cfg` 注释。
- **历史/验证**：
  - 2026-11-04 加值域断言 + 预算说明 → 编译通过，未上板

#### RISK-DOC-03 ftl.c 注释/代码分叉

- **后果**：① `ftl_service` 注释称 NULL 属 ASSERT，函数体没有；② 917/927 行两个同名分区标题，旧流水线语义与早回执矛盾；③ 1803 行空分区标题；④ 文件头分区表只列 4 项；⑤ `ftl_confirm_write` 契约块与函数之间隔了 `ftl_confirm_progress_pending`（违反 S5）；⑥ `s_cache_pad_page` 注释称启动清零，实际 `.dtcm_data (NOLOAD)` 且 `ftl_global_init` 未 memset。
- **缓解**：已改：删重复旧段落；契约块移回函数上方；补两处 `ASSERT(ftl != NULL)`；`ftl_global_init` 补 memset；重写文件头分区表；删空标题。
- **下一步**：`make all` / `make release` 零新告警。
- **证据**：`src/ftl/ftl.c` 文件头 / `ftl_confirm_write` / `ftl_service` / `s_cache_pad_page` / `ftl_global_init`；`linker/stm32h743xx_flash.ld` `.dtcm_data (NOLOAD)`。
- **历史/验证**：
  - 2026-11-05 纯注释 + 两行断言 + 一行 memset → 编译通过，未上板

#### RISK-LOG-02 GC 事件占 UART

- **后果**：GC 事件在极端 victim 速率下仍可能占用 UART；限流类型已收口：`ftl.c` 的 10 处状态已全部改用 `log_ratelimit_s`，本地 `ftl_log_throttle_t` 与 wrapper 已删除。
- **缓解**：默认 INFO 只打 `[GC] start`（每会话一次）+ `[GC] progress`（5s）+ `[GC] end`；逐 victim `[MIG] move` 仅 DEBUG 且只带变化字段；空块只进 `noop`；`[STAT]` 10s。
- **下一步**：上板 30s 抓包：INFO >5 行/s 或 UART 阻塞导致吞吐下降 ⇒ 复核节拍。
- **证据**：`src/ftl/ftl.c`（限流对象与 `ftl_global_init`）；`include/common/log.h:log_ratelimit_s`；`src/ftl/migrate.c` 同口径。
- **历史/验证**：
  - 2026-10-23 限流类型收口 → 上板
  - 2026-11-04 前提更新 + `ftl.c` 迁移

#### RISK-LOG-03 GC no-gain 刷屏

- **后果**：`mig_gc_begin` 在选块前/启动后过早清 `gc_nogain`；选块成功但目的页分配失败（free=1）时，每轮「起 READ → dest 分配失败 → abort」都重新把边沿日志当首次，日志连续刷屏。
- **缓解**：只在 `mig_job_finish` 且实际 `migrated>0` 或成功回收后清 `gc_nogain`；仅启动/中止不清；no-gain 降 DEBUG，INFO 不可见。
- **下一步**：`free=1` 持续写：INFO 不应再出现 no-gain；DEBUG 下每个 no-gain 状态最多一行。
- **证据**：`src/ftl/migrate.c:mig_job_finish` / `mig_gc_nogain_note`；板端 2026-10-23 `free=1` 刷屏日志。
- **历史/验证**：
  - 2026-10-23 复现并修清零点 → 未上板

---

## 2L. FTL 模块 handle 契约

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-ARCH-02 | 待后续 | 跨 LUN superblock 条带 / 跨 LUN GC/WL 未实现 | — |
| RISK-ARCH-05 | 待上板 | 独立 confirm_handle 未采用，confirm/TRIM 两处顺序实现 | 未上板 |
| RISK-ARCH-07 | 待上板 | mig/wbuf/rr_page_scratch 改挂 ftl_storage_t，数组落 AXI | 未上板 |
| RISK-ARCH-08 | 待上板 | 提交/等待计数改绝对 ms deadline | 未上板 |
| RISK-ARCH-09 | 待上板 | PFR 收为 boot orchestrator，内部原语全部 static | 未上板 |
| RISK-ARCH-11 | 待上板 | 端口删除后模块直接持有 owner 指针 | 未上板 |

### 详情

#### RISK-ARCH-02 跨 LUN 条带/GC 未实现

- **后果**：跨 LUN superblock 条带 / 跨 LUN GC/WL 未实现；当前 migrate 严格 per-LUN（源/目的/账本都在本实例）。
- **缓解**：per-LUN `mig_handle`；`lba_routes_to` 守卫。
- **下一步**：上板基线稳定、需要多 die 均衡/条带性能时重估；需要设备级 relocation 引擎 + 跨实例事务，不是改 handle 入参。
- **证据**：`docs/archive/multi_lun_generalization.md:26,358-359`；`src/ftl/migrate.c:553-557,524`；`src/ftl/allocator.c:26-34`。
- **历史/验证**：
  - （日期未记录）设计判定：暂不实现 → 待后续

#### RISK-ARCH-05 独立 confirm_handle 未采用

- **后果**：write-confirm park 已收进 `pend_confirm_service()`；TRIM 仍留在 `ftl.c`；Journal→L2P→BST 顺序有两处实现（confirm/TRIM），任何顺序变更必须同步两处。
- **缓解**：停泊列表 = pending 表的 `PENDING_OP_WRITE_CONFIRM` 项；超时 `FTL_GROUP_DRAIN_TIMEOUT_MS` 后 complete group IO_ERROR。
- **下一步**：顺序变更时同步两处（C2/R9）；若将来抽独立 handle，需同时收口 TRIM。
- **证据**：`src/ftl/pending.c:pend_confirm_*`；`src/ftl/ftl.c:ftl_confirm_handler`。
- **历史/验证**：
  - （日期未记录）设计判定：不采用独立 handle → 未上板

#### RISK-ARCH-07 mig/wbuf/rr_page_scratch 改挂 ftl_storage_t

- **后果**：`mig_handle` / `wbuf` / `rr_page_scratch` 从 `ftl_ctx_t` 改为 `ftl_storage_t` 挂载指针；数组落 AXI `.bss`（`g_ftl_migrate` / `g_ftl_wbuf` / `g_ftl_rr_page_scratch`）。
- **缓解**：`ftl_init` 绑定 + 显式 memset；AXI 预算 `_Static_assert` 已把三块计入。
- **下一步**：上板确认 RAM 实测与 cache/局部性；需要域优化时再调整放置。
- **证据**：`src/main.c`；`include/ftl/ftl.h:ftl_storage_t`。
- **历史/验证**：
  - （日期未记录）Phase 1.5 挂载改造 → 未上板

#### RISK-ARCH-08 提交/等待计数改绝对 ms deadline

- **后果**：migrate / RR / CP 的提交/等待计数已由轮数改为绝对 ms deadline：`FTL_SUBMIT_TIMEOUT_MS = 10` / `FTL_WAIT_TIMEOUT_MS = 50`；confirm park 用 `FTL_GROUP_DRAIN_TIMEOUT_MS = 100`。
- **缓解**：时钟缺失（now_ms == 0）时 deadline 不生效（退化为无超时；本板 time_us 恒非空 ⇒ 不可达）。
- **下一步**：上板标定这三个值（NAND 页操作 ~700µs + 主循环轮速）；观察 `timed_out`/`timeout_phase` 上报。
- **证据**：`include/ftl/ftl_module.h`；`src/ftl/migrate.c` / `read_recovery.c` / `checkpoint.c` / `pending.c`。
- **历史/验证**：
  - （日期未记录）deadline 改造 → 未上板

#### RISK-ARCH-09 PFR 收为 boot orchestrator

- **后果**：fresh/clean/PF 子流程与 full metadata rebuild 原语（restore/probe/build_pools/reset/page_is_blank/scan_write_pointer/close_block/carve/rebuild）全部改为 `static`；公开头只留 `pfr_media_check_format` / `pfr_media_format` / `pfr_boot_recovery`。
- **缓解**：`checkpoint_block_words()` / `metadata_live_count()` / `l2p_cache_invalidate_line()` 三个穿透已收口。
- **下一步**：上板回归 fresh/clean/PF/full metadata rebuild 四条启动路径。
- **证据**：`src/ftl/power_failure_recovery.c`；`include/ftl/power_failure_recovery.h`。
- **历史/验证**：
  - （日期未记录）PFR 收敛 → 未上板

#### RISK-ARCH-11 模块直接持有 owner 指针

- **后果**：端口删除后，模块直接持有 owner 结构指针（`alloc` / `journal` / `cache` / `metadata_dir` / `pending`），C 类型仍可见；「不直接改对方字段」从类型约束降为 owner API + 单写者约定。
- **缓解**：跨模块写只走 owner API；新增 `alc_block_*` / `rr_record_block_ecc` / `rr_retire_block_at` / `pend_view_op` / `pend_group_has_window_pending` 收敛访问；`contract_check_module_contexts` 证明绑定。
- **下一步**：若出现模块直接写对方字段的 review 案例，或需要多后端/测试替身，再评估 opaque 类型/更强接口；当前不做，避免过度封装。
- **证据**：`src/ftl/ftl.c:contract_check_module_contexts`；`include/ftl/allocator.h`；`include/ftl/pending.h`；`docs/archive/ftl_module_contracts.md §3.3/§8`。
- **历史/验证**：
  - （日期未记录）设计判定：保持现状 → 未上板

---

## 2M. USB MSC / 平台

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-PLAT-01 | 待上板 | `.noncacheable_buffer` 段匹配漏前导点，页缓冲落进可缓存 AXI RAM | PIO 正常路径上板；DMA 未验 |
| RISK-PLAT-07 | 待上板 | 各构建配置共用 debug.elf，UT 互相覆盖 | 2026-11-11 编译通过 |
| RISK-PLAT-08 | 待定位 | `.noinit` 出现一次 HardFault（PC 落 newlib printf） | 2026-10-25 记录，镜像未定 |
| RISK-USB-02 | 待上板 | MSC 异步完成/deferred/discard 路径未实测 | 未上板 |
| RISK-USB-05 | 待上板 | WRITE(10) FUA 原被忽略（已实现等确认后回执） | 2026-11-05 编译通过 |
| RISK-USB-07 | 待上板 | request->io 模板的 cmd_remaining 跨命令残留 | USB 路径上板；CLI 未专项验证 |
| RISK-USB-08 | 待上板 | 写 lookahead 提前回执/BUSY 背压未注入失败 | 正常路径上板（2026-10-30） |
| RISK-USB-09 | 待上板 | fsync 被自己的在飞写槽拒掉（已加 FLUSH 专用槽） | 2026-11-05 编译通过 |
| RISK-USB-10 | 待上板 | slot_match 把真正的下一条相同写当事件重放丢弃 | 2026-11-05 编译通过 |
| RISK-USB-11 | 待上板 | TinyUSB 本地补丁清单不完整（cbw_info 钩子） | 2026-11-05 文档 |
| RISK-USB-12 | 待上板 | SYNCHRONIZE CACHE(16) 0x91 未支持 | 2026-11-05 编译通过 |
| RISK-USB-13 | 待上板 | 两写槽占用时 READ 返回 ERROR 而非 BUSY | 2026-11-05 编译通过 |
| RISK-USB-14 | 待上板 | SCSI UNMAP 已实现，未上板 | 2026-11-05 编译通过 |
| RISK-USB-15 | 待决策 | UNMAP 未向主机广告，fstrim 可能不下发 | — |
| RISK-USB-16 | 待上板 | UNMAP descriptor 按 8B 步进（规范 16B）已改 | 2026-11-05 编译通过 |

### 详情

#### RISK-PLAT-01 .noncacheable_buffer 段匹配漏前导点

- **后果**：linker 的 `.noncacheable_buffer` 输入段匹配原只有无前导点形式，`nand_dispatch.o` 的页缓冲被当孤儿段启发式放置：实测落进可缓存 AXI RAM `0x24000204`，与源注释「D-Cache 关闭」不符。`CONFIG_NAND_DMA=1` 时是潜在静默数据破坏。
- **缓解**：链接脚本段匹配补前导点（`*(.noncacheable_buffer)`）；PIO 路径已上板。
- **下一步**：开 `CONFIG_NAND_DMA=1` 上板验证页缓冲地址与 D-Cache 维护。
- **证据**：`linker/stm32h743xx_flash.ld`；`src/nand/nand_dispatch.c`。
- **历史/验证**：
  - 2026-10-2x PIO 正常路径上板；DMA 未验

#### RISK-PLAT-07 各构建配置共用 debug.elf

- **后果**：旧 `TARGET` 只按 debug/release 分，不按 UT 分：`make tests UT=FTL` 后再 `make tests UT=FC`，后者镜像只含 FC 套件并覆盖前者；板端现象是 `test /ftl/` 打印 `NO test run`。
- **缓解**：`config.mk` 的 `TARGET` 按 `FC_NAND_TEST_ENABLE` 加 `_ut` ⇒ 产品与 UT 镜像分名；`.build_cfg` 按产物分置。残留：`UT=FC` 与 `UT=FTL` 仍共用 `debug_ut.elf`。
- **下一步**：分别构建产品 debug / UT debug / UT release / 产品 release，确认四个 ELF 同时存在、F5 按路径烧到预期镜像。
- **证据**：`config.mk` §目标文件命名；`makefile:check-build-cfg`；`.vscode/launch.json`；`docs/design/ftl_ut_design.md §6`。
- **历史/验证**：
  - 2026-11-11 按 UT 分名 → 编译通过，未上板
  - 2026-10-24 真实踩到 `NO test run`

#### RISK-PLAT-08 .noinit 一次性 HardFault

- **后果**：`.noinit` 故障记录出现一次 HardFault：`PC=0x0801665e LR=0xffffffff CFSR=0x00000400 HFSR=0x40000000`（IMPRECISERR + FORCED）。用当前 debug 镜像符号解，PC 落在 newlib `_svfiprintf_r`；故障镜像无法确定（记录跨 PIN 复位保留、只有断电才清），不能据此说「又崩了一次」。
- **缓解**：无。
- **下一步**：先断电清空 `.noinit`，再用同一镜像复现；若再现，用当场镜像的 `.sym`/`.elf` 解 PC，检查 printf 调用点与栈哨兵/帧指针。
- **证据**：板端 2026-10-25 01:09 启动 `previous crash:` 行；`docs/design/fault_capture.md`；`src/platform/stm32h743/fault_port.c`。
- **历史/验证**：
  - 2026-10-25 记录；同日 01:09 镜像跑 `/ftl/` 85/85 正常，运行期无新故障

#### RISK-USB-02 MSC 异步完成路径未实测

- **后果**：`tud_msc_async_io_done()` 的 deferred 回调、主机复位期间的 `discard` 收尾都只在静态路径成立。
- **缓解**：`discard` 标志 + `tud_ready()` 守卫；FTL 只持有静态 bounce buffer。
- **下一步**：读写过程中拔线/总线复位/重新枚举，确认无死命令、无残留 CSW 错误；确认 `pending_io` 不会永久为真。
- **证据**：`src/hil/usb_msc.c:usb_msc_task` / `tud_umount_cb`。
- **历史/验证**：
  - （日期未记录）静态实现 → 未上板

#### RISK-USB-05 WRITE(10) FUA 原被忽略

- **后果**：主机只发 FUA 不发 SYNCHRONIZE CACHE 时，命令完成时数据仍在 FTL cache，掉电即丢已回执写。
- **缓解**：CBW 钩子把 FUA 位带到应用；FUA 写 chunk 不做早期回执，等 `ssd_controller_is_complete`（program + 映射确认）后才回 CSW；失败直接 CSW failed。非 FUA 写仍走早回执。
- **下一步**：自定义 SCSI 工具置 FUA；测回执延迟、写后立刻断电/复位读回；确认非 FUA 路径仍早回执。
- **证据**：`src/hil/usb_msc.c:tud_msc_cbw_info_cb` / `usb_msc_slot_s.fua` / `usb_msc_slot_complete`；`third_party/tinyusb/src/class/msc/msc_device.c`（CBW 钩子）。
- **历史/验证**：
  - 2026-11-05 用户改判：实现 FUA → 编译通过，未上板

#### RISK-USB-07 request->io 模板跨命令残留

- **后果**：`ssd_request_split()` 把本 run 的 `cmd_remaining` 写回 `request->io`（跨命令复用模板）。USB 路径每次提交都 memset 并重设；CLI 路径是 `static g_cli_pending.req`，残留的 `cmd_remaining` 会让下一条 CLI 命令误以为自己还有后续数据（序列过晚 END / 门控挂到 1s 空闲守卫）。
- **缓解**：已改：split 入口把 chunk 级 `cmd_remaining` 取成本地常量，循环内只写 per-run 值，所有出口还原模板。
- **下一步**：连续两条不同长度的 CLI `rd`，确认第二条仍走 cache 读且 `45h/47h` 成对；若改流水提交，必须去掉「还原模板」改为按请求保存。
- **证据**：`src/hil/request.c:ssd_request_split`。
- **历史/验证**：
  - 2026-10-30 USB 路径 2.7 万写 chunk + 全盘 0 错误；CLI static 路径未专项验证

#### RISK-USB-08 写 lookahead 提前回执/BUSY 背压

- **后果**：非末写 chunk 在 FTL 确认前就回 `tud_msc_async_io_done`（纯流控），若中途失败最终由末 chunk 的 CSW 报错；主机对「提前回执后 CSW 失败」的行为未验证。两槽都忙时返回 `TUD_MSC_RET_BUSY`，依赖 `tud_task` 单次最多 16 个事件才不饿死 FTL。
- **缓解**：末 chunk 回执前用 `s_wr_error` 汇总前序失败；FTL 失败不确认映射，host 重试走新 SB。
- **下一步**：写大文件 + 程序失败注入；观察 CSW 是否失败、主循环是否被 BUSY 重投拖慢；若开销显著，改第三槽或调小事件上限。
- **证据**：`src/hil/usb_msc.c:usb_msc_task` / `usb_msc_slot_complete`；`third_party/tinyusb/src/tusb_option.h:651`。
- **历史/验证**：
  - 2026-10-30 满盘写流下 lookahead/BUSY 走量，CSW 全成功 → 正常路径上板；失败 CSW 未注入

#### RISK-USB-09 fsync 被自己的在飞写槽拒掉

- **后果**：写 chunk 提交即回执后槽仍 IN_FLIGHT 到 FTL 完成；旧 `tud_msc_scsi_cb` 用 `usb_msc_host_busy()`（任一槽非 FREE）做门 ⇒ 主机在写 CSW 后立刻发 fsync 时，若最后一个写 chunk 尚未完成，flush 返回 `TUD_MSC_RET_ERROR`（CSW failed）。FTL 侧 `ftl_flush_step` 已显式等待 `wr_inflight` / `WRITE_CONFIRM`。
- **缓解**：删除 scsi_cb 的 `usb_msc_host_busy()` 检查；新增 FLUSH 专用槽（`USB_MSC_SLOT_NUM=3`，2 号槽无 bounce）；数据槽回收逻辑不变。
- **下一步**：持续写 + 每 chunk 后 fsync，CSW 不得失败；两个写 chunk 在飞时紧接 SYNCHRONIZE CACHE，观察 flush 正常完成。
- **证据**：`src/hil/usb_msc.c:tud_msc_scsi_cb` / `usb_msc_slot_alloc` / `USB_MSC_SLOT_NUM`；`src/ftl/ftl.c:ftl_flush_step`。
- **历史/验证**：
  - 2026-11-05 加 FLUSH 专用槽 → 编译通过，未上板

#### RISK-USB-10 slot_match 丢弃真正的下一条相同写

- **后果**：旧匹配键是 `is_write/lba/offset/length` 且不过滤槽状态。写路径早回执后，主机可在旧槽仍 IN_FLIGHT 时发下一条 WRITE(10)；若 LBA/长度完全相同，匹配到旧槽 ⇒ 新命令返回 ASYNC 但不会再被提交/回执（旧写槽 `ack_sent=true` 不调 `async_io_done`）⇒ 主机等 CSW 超时。
- **缓解**：`usb_msc_slot_match` 只匹配 `WAIT_GATE` 槽（旧命令未回执 ⇒ BOT 下主机不可能发下一条真命令，只可能是同一事件重复投递）；`IN_FLIGHT` 写槽不再参与匹配。
- **下一步**：连续两次写同一 LBA（间隔小于 FTL 完成），两条命令都必须完成。
- **证据**：`src/hil/usb_msc.c:usb_msc_slot_match` / `usb_msc_transfer`。
- **历史/验证**：
  - 2026-11-05 修复匹配条件 → 编译通过，未上板

#### RISK-USB-11 TinyUSB 本地补丁清单不完整

- **后果**：`third_party/tinyusb/README.soft_ssd.md` 写「三处最小补丁」，实际还有 `tud_msc_cbw_info_cb` 弱钩子——它把 CBW 的总 block_count/起始 LBA 交给应用，是 FTL cache 序列 `cmd_remaining` 的唯一来源。升级时若只按 README 重套三处，该钩子会静默消失 ⇒ `cmd_remaining` 恒 0 ⇒ 顺序写序列提前收尾（功能不坏，吞吐/序列行为退化）。
- **缓解**：补丁代码本身有 `【本仓补丁】` 注释，可 diff 查；升级时按 grep `本仓补丁` 全量重套。
- **下一步**：修 README：列出 4 组补丁与升级回归项（含「连续大顺序写跨 chunk」）；下次升级按 grep 验证。
- **证据**：`third_party/tinyusb/README.soft_ssd.md`；`third_party/tinyusb/src/class/msc/msc_device.c:tud_msc_cbw_info_cb`；`src/hil/usb_msc.c:tud_msc_cbw_info_cb`。
- **历史/验证**：
  - 2026-11-05 文档待修 → 编译无关

#### RISK-USB-12 SYNCHRONIZE CACHE(16) 未支持

- **后果**：旧 `tud_msc_scsi_cb` 只认 0x35，0x91 落到 `unsupported SCSI opcode` ⇒ CSW failed。若主机用 0x91 做 fsync/卸载，设备报 I/O error，且此前已早回执的写仍在 FTL cache 里——fsync 语义没有兑现。
- **缓解**：新增 `USB_SCSI_SYNCHRONIZE_CACHE_16 (0x91U)`；0x35/0x91 走同一无数据异步 FLUSH 路径；回调契约写明「不能返回 `TUD_MSC_RET_BUSY`：0 会被 TinyUSB 当成无数据命令成功」。
- **下一步**：Linux/Windows 各跑一次 fsync/卸载；用 0x91 的主机应正常完成。
- **证据**：`src/hil/usb_msc.c:tud_msc_scsi_cb`；`third_party/tinyusb/src/class/msc/msc_device.c`。
- **历史/验证**：
  - 2026-11-05 支持 0x91 → 编译通过，未上板

#### RISK-USB-13 两写槽占用时 READ 返回 ERROR

- **后果**：`usb_msc_transfer` 在槽分配失败时对写返回 BUSY、对读返回 ERROR。写路径提交即回执，主机可在两个写 chunk 都在飞时切到读；此时读拿到 ERROR（CSW failed），而 TinyUSB 的 `proc_read_io_data` 支持 `TUD_MSC_RET_BUSY`（伪完成重投）。
- **缓解**：槽分配失败统一返回 `TUD_MSC_RET_BUSY`（读/写一致）；主循环事件上限已保证不饿死 FTL。
- **下一步**：连续写两个 chunk 后立刻读同一区域，读不得报 I/O error。
- **证据**：`src/hil/usb_msc.c:usb_msc_transfer`；`third_party/tinyusb/src/class/msc/msc_device.c:proc_read_io_data`（BUSY 分支）。
- **历史/验证**：
  - 2026-11-05 统一 BUSY → 编译通过，未上板

#### RISK-USB-14 SCSI UNMAP 已实现

- **后果**：`tud_msc_scsi_cb` 新增 0x42 解析（data-out 参数表，Big-Endian），最多 16 个 range；逐段提交 controller TRIM，全部完成后才回 CSW。开始前先做一次内部 FLUSH 屏障并等所有数据槽空闲（防止提前回执的写与 TRIM 顺序反转）。参数表长度/对齐错、range 越界、>16 range ⇒ ILLEGAL REQUEST；TRIM 失败 ⇒ MEDIUM ERROR；总线复位/拔出 ⇒ discard。
- **缓解**：参数表严格校验；逐段 `ssd_controller_lba_range_ok`；控制槽与 FLUSH 复用（BOT 串行下不共存）。
- **下一步**：`blkdiscard`/`fstrim` 后读回零；多 range；UNMAP 后立刻写同一 LBA（写必须胜出）；UNMAP 中拔线/复位无挂死；>16 range 被拒绝。
- **证据**：`src/hil/usb_msc.c:tud_msc_scsi_cb` / `usb_msc_unmap_parse` / `usb_msc_unmap_step` / `usb_msc_slot_s`；`third_party/tinyusb/src/class/msc/msc_device.c`（data-out ASYNC 补丁）。
- **历史/验证**：
  - 2026-11-05 实现 → 编译通过，未上板

#### RISK-USB-15 UNMAP 未向主机广告

- **后果**：TinyUSB 内建的 INQUIRY 不返回 VPD B0/B2，READ CAPACITY(10) 也不带 LBPME/LBPRZ。Linux `sd` 只有在设备广告 thin provisioning 后才会下发 UNMAP；当前 0x42 已能解析，但普通 `fstrim`/`blkdiscard` 可能直接报 "discard not supported"，永远走不到本路径。
- **缓解**：RISK-USB-14 的 handler/异步路径；主机若强制发 0x42 则可用。
- **下一步**：上板抓包确认主机是否发 0x42；若不发，扩展 TinyUSB：READ CAPACITY(16) 置 LBPME/LBPRZ，或实现 VPD B0/B2（需 SPC/SBC 原文，R12）。
- **证据**：`src/hil/usb_msc.c:tud_msc_scsi_cb`；`third_party/tinyusb/src/class/msc/msc_device.c` INQUIRY/READ CAPACITY；`docs/design/usb_msc.md` Part A。
- **历史/验证**：
  - 2026-11-05 判定：未广告 → 待决策

#### RISK-USB-16 UNMAP descriptor 按 8B 步进

- **后果**：SBC-4 的 descriptor = 8B LBA + 4B NLB + 4B reserved。`usb_msc_unmap_parse()` 用 `desc_len % 8`、`count = desc_len/8`，并从 `param+8+i*8` 读 4B LBA、`param+12+i*8` 读 4B blocks ⇒ 规范主机发 16B 描述符时，第 1 个 range 的 blocks 实际读的是 LBA 高 32 位（通常 0）⇒ 命中 `blocks == 0` ⇒ ILLEGAL REQUEST，UNMAP 无法完成。
- **缓解**：descriptor 按 16B 步进（8B LBA 的 high/low + 4B NLB + 4B reserved）；NLB==0 按规范跳过；LBA 高 32 位非零拒绝。
- **下一步**：用自定义 SCSI 工具发 Linux 风格 16B 参数表（含多 range / NLB=0 / 高 32 位非零负例），确认逐段 TRIM 与 CSW。
- **证据**：`src/hil/usb_msc.c:758-838:usb_msc_unmap_parse`。
- **历史/验证**：
  - 2026-11-05 改 16B 步进 → 编译通过，未上板

---

## 2N. 容量 / 几何

> 本节 4 条（`RISK-CAP-01` ~ `RISK-CAP-04`）已于 **2026-10-22** 全部闭合
> （v12 首启格式化 + 全盘 H2testw 415/423 MB 通过）。详见文末「归档索引」与 archive。

---

## 2O. NAND / FMC 时序与吞吐

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-NAND-08 | 信息 | FMC NAND 无专用 DMA 请求线；mem2mem 不是 FMC DMA | 未上板 |
| RISK-NAND-05 | 待定位 | used 介质不可纠读页，L2P 仍指向坏块，每次读重试 8 次 | 2026-10-20 上板观测 |
| RISK-NAND-06 | 待定位 | 运行期 BAD 标记来源未定：Mode 5 时序 / 同页二次编程 / 边缘块 | 2026-10-21 上板观测 |

### 详情

#### RISK-NAND-08 FMC NAND 无专用 DMA 请求线

- **后果**：FMC NAND 没有专用 DMA 请求线；DMA2 memory-to-memory 是软件触发路径，不是 FMC 外设 DMA。把它当 FMC DMA 引入会让数据面依赖一条从未上板的路径。
- **缓解**：数据面固定 PIO（CPU 逐字节 + `__DMB`）；`nand_dma.c`、`include/platform/dma.h` 的 NAND DMA 段、`nand_fmc.c` 的 DMA 分支均不存在。
- **下一步**：不要把 mem2mem 重新当作 FMC DMA 引入；若换带 DMA 请求线/专用控制器的硬件，另做接口与上板验证。
- **证据**：`src/platform/stm32h743/nand_dma.c`（已删除）；`include/platform/dma.h`；`src/nand/nand_fmc.c`；`docs/design/testing_guide.md:513`（"FMC NAND 无专用 DMA 请求"）。
- **历史/验证**：
  - 2026-11-05 FC/NAND 重构删除 NAND DMA 实验路径 → 未上板

#### RISK-NAND-05 used 介质出现不可纠读页

- **后果**：rev32 启动后 host 读 LBA 129 反复报 `[ERR] [FTL] read unrecoverable lba=129 gblock=1064 retry=8`。RR Stage 1 读重试用 Feature 地址 `0x89`，本器件该地址是保留段，没有电压/软判决重试；ECC 不可纠 ⇒ `bbt_mark_bad(1064)` + host `IO_ERROR`。L2P 仍指向坏块（悬空），同一 LBA 每次读都重试 8 次并重复打日志；若该 LBA 属文件系统元数据，Windows 可能把卷判为损坏/RAW。已登记坏块 305（GC 活锁）、1064（本次）。这是芯片物理页失效（或早期写坏），不是本仓 FTL 逻辑产生；该 LBA 上的数据本就不可读。
- **缓解**：`bbt_mark_bad` 隔离（RAM + 后台 BBT 持久化）；格式化后该 LBA 重映射到新页，日志停止。
- **下一步**：① 现场 Windows 快速格式化（或 CLI `format` + POR）重建文件系统后再跑 H2testw；② 观察 1064 重启后是否仍为 BAD；③ 若坏块持续增长，评估换芯片 / 真实 read-retry（需器件支持，`0x89` 不可用）。
- **证据**：板端 rev32 串口（两次）；`src/ftl/read_recovery.c:284-293`；§四「`0x89` 保留地址」。
- **历史/验证**：
  - 2026-10-20 rev32 板端两次 `read unrecoverable lba=129 gblock=1064 retry=8` → 坏块被隔离，L2P 悬空，未定位

#### RISK-NAND-06 运行期 BAD 标记来源未定

- **后果**：BAD 标记来源与 NAND 时序/FTL 写路径强相关，且旧 verify 不测已知 BAD：`format --verify` 日志 `tested=3810 bad=0` 跳过了 BBT 里已有的 282 个 BAD ⇒ 这 282 个是运行期累积（host 写 program fail / RR 不可纠读 / 擦除失败）。全片低速 re-scan（Mode 5 切换前）得到 `old_bad=282 recovered=282 new_bad=0`、`tested=4092 bad=0` ⇒ 不是硬坏块。三个候选：① Mode 5 时序边缘；② FTL 写路径 bug（同页二次编程）；③ 真正边缘块。`pfr_format_verify_blocks` 是单 plane 写，运行期 pair 写走 multi-plane，多 plane 问题不被捕获；图样块分量是 `(BYTE)block`（只覆盖低 8 位）且每块测完立即擦除 ⇒ 块号高位错误 / 两块地址别名不会被检出。
- **缓解**：当前 0 BAD、428 MiB；运行期有 write retry + Spare/OP，但读侧仍可能丢数据。
- **下一步**：① 正常镜像 0 BAD 跑 H2testw；② 失败时看标记来源（`program failed` / `read unrecoverable` / erase failed）；③ 用 `map <lba>` + `rdpaa <paa> --raw` 区分映射错还是物理页错；④ 再跑一次 Mode 5 下 `format --verify`：直测仍失败 ⇒ 时序/物理；直测通过但 FTL 又标坏 ⇒ 查 FTL 写路径（二次编程）。
- **证据**：板端 2026-10-21 日志；`src/main.c` `ssd_ftl_boot`；`src/nand/devices/mt29f___g08a.c` `nand_enable_high_speed`；RISK-NAND-01/03；`include/nand/nand_config.h` 几何。
- **历史/验证**：
  - 2026-10-21 夜 rev33 追加 +264 BAD 全为 `fc_err`(UNC)；低速 re-scan `old_bad=282 recovered=282 new_bad=0`，用户报告高速 re-scan 亦通过（命令形态/日志待补）→ 候选 ① 进一步削弱，主嫌疑转向 ② FTL 写路径与 GC 读范围；现有 re-scan/verify 是单 plane 顺序写满再读，不能排除 pair 写与半写块空洞页
  - 2026-10-21 板端全片低速 re-scan `old_bad=282 recovered=282 new_bad=0`、`tested=4092 bad=0`；此前 `format --verify tested=3810 bad=0` 跳过已有 BAD → 282 个是运行期标记

---

## 2P. allocator

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-ALLOC-09 | 待上板 | 低 free 时成对分配失败导致写路径永久 stall（plane 不平衡） | 2026-10-31 上板通过（生路） |
| RISK-ALLOC-06 | 待后续 | `ftl_gblock` 无入参契约，哨兵可截断成合法 gblock | 未上板 |
| RISK-ALLOC-12 | 待上板 | 出池后 state 仍为 FREE，漏赋新态时块永久漏账 | 未上板 |
| RISK-ALLOC-13 | 待上板 | `alc_super_open_rewind_pair` 无下溢守卫，`next_page--` 掩码成页 63 | 未上板 |
| RISK-ALLOC-15 | 待上板 | 全 plane BAD 的 SB 关闭时追加零 plane 条目，列表槽泄漏 | 未上板 |
| RISK-ALLOC-16 | 待上板 | `alc_spare_pop` 不跳 BAD，坏块替换假报成功并丢 spare | 未上板 |
| RISK-ALLOC-03 | 待上板 | 纯转发访问器与 `alloc->blocks[]` 直接访问形成两套坐标 | 未上板 |
| RISK-ALLOC-07 | 待上板 | `ftl_tables_t` 五数组打包，单表无法换内存域 | 未上板 |
| RISK-ALLOC-08 | 待上板 | `alc_super_close_live` 吞 append 失败码，Closed 列表满时漏账 | 未上板 |

### 详情

#### RISK-ALLOC-09 低 free 时成对分配失败导致写路径永久 stall

- **后果**：GC 压力测试 `free=264` 时 `drain stalled`；dump `seq act=0`、`pend=0`、`wb_busy=0`、FC 池全空、`sb_open=0 np=64 p0=03` ⇒ 唯一阻塞是打不开新 SB（每 plane 各需一块）。机制：恢复路径把 closed 列表建成单 plane 条目（`pfr_closed_sb_add` 每物理块一条，`closed+free=3385` 是 block 口径），GC 逐条回收空块（`nop` 54→671、`mv=0`），free 池逐块增加但两 plane 不平衡；某 plane 先耗尽后每次分配从 OP 池补（204/196），OP 耗尽 ⇒ 分配永久失败。次生问题：`cache_wr_build` 把 `alc_super_open_alloc_pair` 的 `FTL_ERROR` 映射成 `FTL_AGAIN`（H38）⇒ drain 重试 8 轮判 stall → chunk deferred error → 主机 FS 脏（H2testw Code 1392）。持久化 block→SB 归属的结构性根治在 ISS-FTL-17。
- **缓解**：每 plane free 计数 `free_plane_count[PLANE_NUM]` 由唯一闸门维护，读口 `alc_free_get_count` / `alc_free_min_plane_count`；GC T1 增加「任一 plane < `FREE_POOL_PLANE_LOW`(4)」；victim 选择优先 plane 平衡（成对条目 + 饥饿 plane 单条目优先，偏好组空才退回）；`cache_wr_build` / `wb_flush` 把 alloc 的 `FTL_ERROR` 原样上报；stall dump 与 `info` 加 `fpmin`。
- **下一步**：断电视角另测；观察 `op=204/0`（plane 1 OP 耗尽、free 10/20 仍不平衡，偏好阈值 4 才生效）与 `mv=0`（victim 全是空块，ISS-FTL-18）；板端走 format→POR、GC 压力、重启恢复。
- **证据**：`src/ftl/allocator.c` `alc_super_open` / `alc_free_find_block` / `alc_free_push`；`src/ftl/power_failure_recovery.c` `pfr_closed_sb_add`；`src/ftl/migrate.c` `mig_job_finish`；`src/ftl/cache_batch.c` `cache_wr_build`。
- **历史/验证**：
  - 2026-10-31 20:13 镜像板端：同场景 `free` 跌破 32（稳态 30~32）后 H2testw 写不中断（239 MB 时 894 KB/s）、`fpmin=10`、`nop` 持续增长 → 生路验收通过；观察 `op=204/0`、`dng=2`、`mv=0`
  - 2026-10-31 修前板端：`drain stalled gi=0 next=0/32`、`free=264 sb_open=0 np=64 p0=03`、`nop=54→671`、`mv=0`；H2testw offset 0x13000000 Code 1392 → 复现

#### RISK-ALLOC-06 `ftl_gblock` 无入参契约

- **后果**：`ftl_gblock(plane, block)`（`bst.h:147-150`）无入参断言，返回 `WORD`：`plane * BLOCKS_PER_PLANE + block` 在 `int` 里算完再隐式截断到 16 位。`FTL_BLOCK_NO_PLANE=65535` 是 `super_block_s.plane_blocks[]` 的合法哨兵：`ftl_gblock(0,65535)=65535`，访问器的 `gblock < FTL_BLOCK_TOTAL` 能抓到；但 `ftl_gblock(1,65535)=67583` 截断成 **2047**，落在合法区间、实际指向 plane 0 block 2047，断言抓不到。这不是算式错，是输入契约（`plane < PLANE_NUM`、`block < BLOCKS_PER_PLANE`、不得为哨兵）没有被编码；编译期无声（无 `-Wconversion`），普通 UT 也不会走到。分层上 gblock 是 FTL 的 BST 数组下标，不进入 FC：FC 的 `pba_is_valid` / `paa_is_valid_*` 只校验格式；读路径 `fc_data_path_meta_check` 比对页内 META 与期望 LBA，只在读且 LBA 已知时生效（GC/RR 用 `FC_META_UNKNOWN` 主动跳过），写路径无语义校验；CRC/ECC 只保证 content integrity，不保证 placement / mapping 语义。
- **缓解**：所有展开 `plane_blocks[]` 的调用点都有 `super_block_has_plane` 守卫（`migrate.c:64/94`、`allocator.c:413/459/545`、`power_failure_recovery.c`、`read_recovery.c:881`、`wear_leveling.c:133`）⇒ 不是活 bug；`alc_free_push` 的四条断言（含 `gblock`）未动，且带 release 拒绝。
- **下一步**：在 `ftl_gblock`（单一构造漏斗，C9）内加 `ASSERT(plane < PLANE_NUM); ASSERT(block < BLOCKS_PER_PLANE);`，让哨兵在构造处即被拒。触发：新增 `ftl_gblock` 调用点 / 有人提出删访问器断言。
- **证据**：`include/ftl/bst.h:145-150`；几何 `include/nand/devices/mt29f___g08a.h:29-30`（`BLOCKS_PER_PLANE=2048` / `PLANE_NUM=2`）、`include/common/ssd_types.h:31`（`WORD=uint16_t`）；哨兵 `include/ftl/allocator.h`；`super_block_has_plane`（`allocator.h:151-154`）；`pba_is_valid`（`ftl_config.h:505-514`）；`paa_is_valid_paa`（`nand_config.h:224`，0 调用点）。
- **历史/验证**：
  - 2026-10-21 按用户裁决删除访问器 `gblock < FTL_BLOCK_TOTAL` 断言 → 未上板

#### RISK-ALLOC-12 出池后 state 仍为 FREE

- **后果**：`alc_free_remove` 只把块从 `free_blocks[]` 摘除，不改 state；调用者若漏赋新态，块永久漏账（不在任何池，`alc_free_push` 又以 double-free 拒绝）——典型的静默容量流失。
- **缓解**：remove 命中即置 `NONE`，push/remove 两个闸门各维护不变量的一边（push→FREE，remove→NONE），调用者随后赋 OPEN/LOG/CLOSED/BAD；未命中仍为 no-op，并加 `ASSERT(state != FREE)` 抓池账分叉。所有现网调用点在 remove 后立即赋目标态。
- **下一步**：测试源 `tests/unit/test_ftl_pure.c` `test_free_pool` 仍断言「出池后 state 仍为 FREE」，按 R4 由用户同步；上板走 format→POR、GC、BBT 标坏。
- **证据**：`src/ftl/allocator.c` `alc_free_remove`；`include/ftl/allocator.h` 不变量②；`src/ftl/bbt.c` `bbt_mark_bad`。
- **历史/验证**：
  - 2026-11-04 remove 命中即置 NONE，push/remove 两闸门各维护一边 → 未上板

#### RISK-ALLOC-13 `alc_super_open_rewind_pair` 无下溢守卫

- **后果**：`next_page` 是 `BYTE`，误调用（未紧邻一次成功 alloc）时 `0--` = 255；PBA 页域只有 6 bit（`PAGES_PER_BLOCK=64`）⇒ 255 被掩码成页 63，静默二次编程最后一页。
- **缓解**：入口 `ASSERT(next_page > 0)` + release 拒绝（LOGE 后 return，不递减）；所有调用点都紧跟一次成功的 `alc_super_open_alloc_pair`。
- **下一步**：若将来允许跨函数 rewind，需重新评估。
- **证据**：`src/ftl/allocator.c` `alc_super_open_rewind_pair`；`include/ftl/ftl_config.h` `PBA_PAGE_MASK`。
- **历史/验证**：
  - 2026-11-04 入口加 `ASSERT(next_page > 0)` + release 拒绝 → 未上板

#### RISK-ALLOC-15 全 plane BAD 的 SB 关闭时追加零 plane 条目

- **后果**：`alc_super_close_live` 把 BAD plane 过滤成 `FTL_BLOCK_NO_PLANE`，原实现无条件追加条目。触发：一次 pair 写失败可同时标坏两个 plane（`ftl_host_io_write_fail` 的 `bad_plane_mask` 可为 `0x3`），随后 `alc_super_close(FTL_TEMP_HOT)` 关闭该 SB ⇒ 条目 `plane_count==0`。消费方（`alc_super_get_stats`/GC/WL）永久跳过；`bbt_mark_bad` 发生在 OPEN 期间（不在列表里），ALLOC-14 的 drop_plane 修不到它 ⇒ 每次事件泄漏 1 个槽；累计到 1024 后 `close_live` 全部返回 `NO_SB` ⇒ `alloc_pair` 返 `FTL_ERROR`、写入失败。
- **缓解**：过滤后若无剩余 plane，`is_open=false` 且不追加条目，返回当前 `closed_sb_count`（调用者只与 `FTL_BLOCK_NO_SB` 比较）；`alc_super_close_live` / `alc_super_closed_add` / `alc_super_close` 文档同步。
- **下一步**：上板/注入：让一次 pair 写的两个 plane 都 FAIL，确认 `closed_sb_count` 不增长、后续写正常（不出现 `closed_sb_list full`）。
- **证据**：`src/ftl/allocator.c` `alc_super_close_live`；`src/ftl/ftl.c` `ftl_host_io_write_fail`；`src/ftl/power_failure_recovery.c` `pfr_closed_sb_add`。
- **历史/验证**：
  - 2026-11-05 过滤后零 plane 不入表并返回 `closed_sb_count` → 未上板

#### RISK-ALLOC-16 `alc_spare_pop` 不跳过 BAD

- **后果**：`alc_free_refill_from_op` 会丢弃 BAD 的 OP 条目，但 `alc_spare_pop` 只按下标弹出。若 Spare 池里的块在运行期被标坏（CLI `bbt --erase` 诊断 / 将来复用路径），`bbt_spare_refill` 弹出它 → `alc_free_push` 拒绝 BAD → 仍返回 `FTL_OK` ⇒ `bbt_mark_bad` 不再走 OP 兜底，本次替换落空，spare 计数静默 -1。
- **缓解**：`alc_spare_pop` 循环弹出并丢弃 BAD，直到取到好块或池空（与 `alc_free_refill_from_op` 同口径）；全坏时返回 false，上层走 OP 兜底。
- **下一步**：上板/注入：把 Spare 池尾块标 BAD 后触发一次坏块替换，确认取到下一个好 spare（或正确回退 OP）。
- **证据**：`src/ftl/allocator.c` `alc_spare_pop`；`src/ftl/bbt.c` `bbt_spare_refill`。
- **历史/验证**：
  - 2026-11-05 `alc_spare_pop` 循环丢弃 BAD → 未上板

#### RISK-ALLOC-03 纯转发访问器与直接数组访问并存

- **后果**：`gblock` / `alloc` 断言按用户裁决删除后，除 `op_at` / `spare_at` 外的访问器成了纯一行转发（`return ftl_block_xxx(&alloc->blocks[g])`）。`blocks[]` 本来就是公开指针字段：`allocator.c` 之外已有 37 处直接写 `&alloc->blocks[g]` + `ftl_block_*`（metadata / bbt / read_recovery / wear_leveling…）⇒ 同一语义存在两套访问坐标（C9），封装并未真正生效。
- **缓解**：访问器集合收敛为 `op_at` / `spare_at`（`index < count` 边界 + 池数组无其它直接访问）、`wear_get` / `restore_wear`（多字段快照/恢复）、`bbt_format_in_progress`（跨模块读 bbt 子状态）、`count_blocks_below`（整表扫描）；其余块字段统一经 `bst.h` 的 `ftl_block_*`，`valid_count ±1` 走 `ftl_block_add_valid` / `sub_valid_if_live`。
- **下一步**：不要通过改表挂载来消除封装（那是内存域布局，需重排 AXI 预算且一个访问器也减不掉）；新增访问器须有第二个消费者或封装住不变量。
- **证据**：`include/ftl/allocator.h:390-469`；`src/ftl/allocator.c:719-843`；直接访问 `grep -rn "alloc->blocks\[" src/ftl`（去掉 `allocator.c`）；内存域 `src/main.c:184-239`。
- **历史/验证**：
  - 2026-10-21 删除 7 个纯转发访问器，统一到 `ftl_block_*`；测试源不引用这 7 个 → 未上板

#### RISK-ALLOC-07 `ftl_tables_t` 五数组打包

- **后果**：`ftl_tables_t`（内含 5 个数组，123,896 B）作为单个对象，任一表想单独换内存域都做不到；地址/布局类改动编译期无法证明放置正确。
- **缓解**：`main.c` 拆成 5 个独立对象 `g_ftl_blocks` / `g_ftl_free_blocks` / `g_ftl_op_blocks` / `g_ftl_spare_blocks` / `g_ftl_closed_sb_list`，各自带 `SRAM12_DATA`；FTL 收 `ftl_table_refs_t`（5 指针），`ftl_ctx_t.table_refs` 按值保存；逐表 memset（NOLOAD）；`_Static_assert` 五者 `sizeof` 之和 ≤ SRAM12；`contract_check_handles` 比对 `alloc->* == table_refs.*`。
- **下一步**：上板判据：启动/格式化恢复、`info`（429 MiB/free）、`wr/rd ... c`、GC、重启恢复、`map`；`make tests UT=FTL` 若失败，原因在冻结夹具（`F->tables` 旧形状），非生产代码（R2/R4）。
- **证据**：`src/main.c`（五数组 + refs 装配 + 断言）；`include/ftl/allocator.h` `ftl_table_refs_t`；`include/ftl/ftl.h` `table_refs`；`src/ftl/ftl.c`（逐表 memset / alc_init / audit）；`src/ftl/{migrate,journal,checkpoint}.c` 调用点。
- **历史/验证**：
  - 2026-10-21 五数组拆分为独立对象并改为 refs 装配 → 未上板

#### RISK-ALLOC-08 `alc_super_close_live` 吞 append 失败码

- **后果**：若 Closed 列表满，live SB 已关闭但未进 `closed_sb_list` ⇒ GC/容量账看不到该块，永久漏账（H30/C5）。
- **缓解**：append + live 标志合并为唯一 `alc_super_close_live`：先检查列表容量，满时保持 live 状态不变并返回 `FTL_BLOCK_NO_SB`；调用者按失败码处理。`RISK-ALLOC-01` 的唯一 live setter 保留；`alc_super_open_alloc_pair` 与 `alc_super_close` 上抛失败；`alc_super_prepare_wl_destination` 的 void 边界显式丢弃。
- **下一步**：上板 `format` + H2testw；新增关闭路径必须复用同一函数并检查返回值。
- **证据**：`src/ftl/allocator.c` `alc_super_close_live`；`include/ftl/allocator.h`；RISK-ALLOC-01。
- **历史/验证**：
  - 2026-10-23 收口 append + live 标志为唯一入口 → 未上板

---

## 2Q. FTL 类型 / 命名 / 对齐收口

> 本节条目已归档（见 `docs/archive/known_risks_batches.md` §2026-10-24）：RISK-ARCH-12（原归档，待上板）


## 2R. 位运算公共宏

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-BIT-01 | 待后续 | 通用掩码构造器挂在 NAND 层，FTL/FC 各自另写字面量宏 | 未上板 |

### 详情

#### RISK-BIT-01 `NAND_FIELD_MASK` 跨层归属

- **后果**：全仓唯一的通用（按位宽生成）掩码构造器 `NAND_FIELD_MASK(width)` 挂在 NAND 层，而 FTL / FC 同样需要按位宽生成掩码 ⇒ 跨层取用（H13）。实际后果不是编译错误，而是各自就地另写一套掩码宏：NAND 位宽一改，FTL 侧的字面量宏不跟着变，位判断静默错位（C2「第 3 次必须提炼」的失败形态）。
- **缓解**：FTL / FC 当前未直接使用它，各自掩码均为本地字面量宏；H15 已要求新增前先找它。
- **下一步**：下次出现「按位宽生成掩码」需求时下沉到 `include/common/`（NAND 层保留 `PAA_*` 别名），同步改 `CLAUDE.md` H15 的落点描述。触发：FTL/FC 出现第 2 个同类宏。
- **证据**：`include/nand/nand_config.h:100-104`（定义 + `PAA_*` 消费者）、`:176/:199/:207/:213/:279`（NAND 内部使用）。
- **历史/验证**：
  - （日期未记录）从 `CLAUDE.md` H15 迁入并判定 → 未改代码

---

## 2S. FTL cache read

> 本节条目已归档（见 `docs/archive/known_risks_batches.md` §2026-10-24）：RISK-CACHE-01（原归档，待上板）
> 本节条目已归档（见 `docs/archive/known_risks_batches.md` §2026-10-30）：RISK-CACHE-14（已闭合 2026-10-30，并发 cache 读门控早开）


## 2T. FTL cache program

> 本节条目已归档（见 `docs/archive/known_risks_batches.md` §2026-10-24）：RISK-CACHE-02（已改、host 正常路径上板；FAILC 负例未验） · RISK-CACHE-03（已改、正常路径上板；失败注入未做） · RISK-CACHE-04（已改、正常路径上板；HSQ/chain 未引入） · RISK-CACHE-05（已改、上板） · RISK-CACHE-06（已改、正常路径上板；掉电注入未做） · RISK-CACHE-12（已改、上板） · RISK-CACHE-13（已改、上板） · RISK-CACHE-07（已改、正常路径上板；失败归因未注入） · RISK-CACHE-08（已改、正常路径上板；失败注入未做） · RISK-CACHE-09（已改、正常路径上板；瞬态背压未注入） · RISK-CACHE-10（已改、正常路径上板；丢完成未注入） · RISK-CACHE-11（已改、上板）

## 2U. ITCM / DTCM 热路径

> 本节条目已归档（见 `docs/archive/known_risks_batches.md` §2026-10-24）：RISK-TCM-01（已上板验证） · RISK-TCM-02（已上板验证） · RISK-TCM-03（已上板验证） · RISK-TCM-04（已上板验证） · RISK-TCM-09（已上板验证） · RISK-TCM-05（已上板验证） · RISK-TCM-06（已上板验证） · RISK-TCM-08（已上板验证） · RISK-TCM-07（已判定）
> 本节条目已归档（见 `docs/archive/known_risks_batches.md` §2026-10-30）：RISK-TCM-10（已闭合 2026-10-30，ITCM 运行期只读）

| ID | 状态 | 风险 / 判定 | 当前缓解 | 重访触发 / 下一步 | 证据 |
|---|---|---|---|---|---|

## 2V. 板端读完整性事件

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-RD-01 | 待定位 | 单次不可纠读，状态 `CRC\|MATA`：页内 META 的 LBA 与请求不符 | 2026-10-23 上板观测 |

### 详情

#### RISK-RD-01 单次不可纠读（`CRC|MATA`）

- **后果**：单次不可纠读，状态 `CRC|MATA`：`lba=220 pba=0x000404cc g=2562 pl=1 bk=514 pg=25 cw=0`。`MATA` 表示页内 META 的 LBA 与请求不一致，不是单纯单元磨损；RR 按设计退役该块（`bad=1`、`free+1`），LBA 220 数据丢失，容量 424→423 MiB（`lba_max=867840`）。可能是旧介质页/旧映射，也可能指向 cache program / GC 批处理或 L2P 恢复的映射问题。
- **缓解**：RR 退役块并持久化 BBT；断电重启后 `bad areas data=1`、无新 `unc`，系统正常。
- **下一步**：跑 H2testw 415/423 MB；若再出现任意 `st` 含 `MATA` 的 `unc`（尤其不同 LBA），停下并抓完整日志 + 前后 STAT，按映射问题查 `cache_write` / `migrate` / L2P journal。
- **证据**：板端串口日志 2026-10-23；`src/ftl/read_recovery.c` `rr_read_complete`；`src/fc/fc_data_path.c` MATA 判据。
- **历史/验证**：
  - 2026-10-23 板端串口观测：RR 退役块并持久化 BBT，断电重启无新 `unc` → 未定位

---

## 2W. pending 表 / 完成组

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-HOST-15 | 待上板 | 写确认 handler 用文件级全局 + `void *`，UT 需 save/restore | 未上板 |

### 详情

#### RISK-HOST-15 写确认 handler 的全局状态

- **后果**：写确认 handler 采用「文件级全局 + setter/getter + `void *`」：`pending.c` 持 `s_confirm_handler`，`ftl_init` 每实例注册一次；UT 必须 save/restore 全局，且 restore 不在 longjmp 保护内（任一 `munit_assert_*` 失败即让后续用例静默使用假 handler）。`void *ctx` 的类型擦除也未换来解耦——`pending.h` 本就依赖 `ftl_type.h`（`ftl_status_e` / `pending_cb_fn_t`），`ftl_ctx_s` 已是不透明 typedef。
- **缓解**：按控制器写管线形态收口：推进循环在 FTL（`ftl_confirm_service`），pending 只保留表原语 `pend_confirm_is_parked` / `is_expired`（header inline）/ `settle` / `get_count`；不存在 handler typedef / setter / getter 与 `ftl_confirm_handler`。描述符构造在 `ftl_host_io_write_ok` 与写确认服务各写一次（用户判定两个调用点不值得 helper，见 RISK-ARCH-11）。结算顺序不变量（owners → release → complete）收敛在 `pend_confirm_settle` 内；超时阈值语义在 `pend_confirm_is_expired`（吃 `now_ms` 实参）。
- **下一步**：上板回归待入账路径：写入中构造 region 未驻留，确认 `confirm park timeout` 语义不变、结算计数与组完成不重复。若将来重新引入事务注入，先重读本条。
- **证据**：`src/ftl/pending.c` `pend_confirm_is_parked` / `pend_confirm_settle` / `pend_confirm_get_count`；`src/ftl/ftl.c` `ftl_confirm_service`；`include/ftl/pending.h` `pend_confirm_is_expired`；`tests/unit/test_ftl_pure.c` `confirm_park_timeout` / `confirm_is_parked`。
- **历史/验证**：
  - 2026-11-03 收口：推进循环搬回 FTL，删除 handler 全局与 `void *` → 未上板

---

## 2X. 日志格式化 — 编译器格式检查收口

> 触发：`log_printf` 补上 `__attribute__((format(printf, 2, 3)))`，格式串与实参不匹配从
> 「板端才看得出乱码」变成编译期告警。同时链接改为完整 newlib（`config.mk` 去掉
> `-specs=nano.specs`），`%llu` / `%zu` / 浮点格式化均可用。

> 本节条目已归档（见 `docs/archive/known_risks_batches.md` §2026-10-24）：RISK-LOG-05（信息）

---

## 2Y. UT 框架输出

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-UT-04 | 信息 | FC 速度测试擦写 live FTL 的 per-plane 块 | 2026-10-24 上板确认 |
| RISK-UT-06 | 信息 | 每次跑 /ftl/media 永久标坏 3 个用户块 | 2026-11-11 上板实测 |
| RISK-UT-10 | 信息 | L3 随机块窗口与 live FTL 块池冲突 | 2026-11-11 上板（UT 镜像） |
| RISK-UT-12 | 待后续 | chain_priority_lock 断言早于 feature 还原 | 2026-11-11 编译通过（板端 SKIP） |
| RISK-UT-13 | 信息 | teardown 不收在途资源，longjmp 后泄漏 | 未上板 |
| RISK-UT-14 | 信息 | level1 两条 validate 用例不含断言 | 2026-11-11 复查确认 |
| RISK-UT-25 | 信息 | test /ftl/ 后 BAD 计数多 3 个 | 2026-11-11 上板对比 |

### 详情

#### RISK-UT-04 FC 速度测试擦写 live FTL 的 per-plane 块

- **后果**：`/speed/fc_nand_rw` 用 FC 公共接口对原始块做 multi-plane 擦写，FTL 的 RAM 映射/日志区可能指向这些块。测试期间 FTL 主循环被 CLI 阻塞、`bg_suspend=1`，不会并发分配；但测试返回后 FTL 状态可能已失效。跑完 FC-only 直接交给 host/USB 可能读到旧映射或写失败。
- **缓解**：测试 summary 打印 "raw blocks destroyed — run format + POR"；文档要求 FC-only 后 `format` + POR；完整 `/` 运行由 `/speed/ftl_rw` 的 format 修复。同一上电周期内连跑多条 `test` 时，media 套件在每条 `test` 内首次必格式化一次（`RISK-UT-08`）。
- **下一步**：任何改变 FC 速度测试块范围/是否擦写的改动都要重新评估；不要把它并入默认的 `test /ftl/` 流程。
- **证据**：`tests/unit/test_speed.c:test_fc_speed_mp`；`docs/design/fc_nand_ut.md` §4.4。
- **历史/验证**：
  - 2026-11-01 media 套件改为每条 `test` 复位 → 已上板（release -O2 全量 156/156）
  - 2026-10-24 23:13 上板确认风险可见（信息）

#### RISK-UT-06 每次跑 /ftl/media 永久标坏 3 个用户块

- **后果**：`bbt_persist` 从 Free Pool 取块做 `bbt_mark_bad`，验证 save→reload；该 BAD 经 BBT 持久化，`format` 不清除坏块图。每跑一次完整 `/ftl/media/` 永久消耗 3 个用户块（`bbt_persist` 1 + `bbt_ring_reload` 2），约 256 KiB/次（423 MiB 盘约 0.06%/次），不影响正确性。
- **缓解**：无（用例必须真实标坏才能验证持久化）。
- **下一步**：长期反复跑时记录 `bad` 累计，到达阈值后接受容量下降或换片；测试计划写明每次消耗的块数。
- **证据**：`tests/unit/test_ftl_media.c:test_bbt_persist` / `test_bbt_ring_reload`；`src/ftl/bbt.c:bbt_mark_bad`；板端 2026-10-25 01:09:23 镜像（boot `bad=2` → 全量 85/85 后 `bad=3`，`lba_max` 867,328→866,816）；板端 2026-11-11 21:22:39 `test /ftl/`（boot `bad=11` → `[STAT] bad=14`，+3）。
- **历史/验证**：
  - 2026-11-11 21:22:39 `test /ftl/`：boot bad=11 → bad=14（+3） → 板上复现
  - 2026-10-25 01:09:23 全量 85/85：bad=2→3，容量减 1 gblock（256 KiB） → 板上实测

#### RISK-UT-10 L3 随机块窗口与 live FTL 块池冲突

- **后果**：`/fc-nand/` L3 窗口 `[614,1433]` 只避开固定 metadata 区（L2P 0..31 / 目录 33,34 / BBT 36,39 / CP 37,38 / Journal 40,41）；UT 镜像里 FTL 全程存活（`ssd_ftl_boot_all` 无条件跑，`test` 从主循环 CLI 进入），动态 L2P region 块（≥32）与用户数据块都可能在窗口内 ⇒ L3 的 erase/prog/read 直接改写 live 映射块，FC 层不报错，FTL RAM 映射仍指向被擦/被写的块 ⇒ 测试后转 USB 可静默丢数据。暴露面由 2 个块扩大到 820 个。`/fc-nand/` 59/59 全绿只证 FC 命令成功，不证块归属安全（R1）。
- **缓解**：生产镜像不编入 `test` 命令（`src/hil/cli.c:150`、`:2139` 的 `#if FC_NAND_TEST_ENABLE`；只有 `make tests` 置 1，`makefile:163`）⇒ 风险只存在于开发用 UT 镜像。UT 跑完仍须 `format` + POR 再交给 host（与 RISK-UT-04 同一收尾要求）。
- **下一步**：若要让 `/fc-nand/` 与 live FTL 共存：由 FTL 提供保留块，或 UT 镜像不 boot FTL；改块窗口 / FTL 启动顺序 / 池划分时重读本条。
- **证据**：`tests/unit/test_fc_level3.c:96-108:L3_BLOCK_LO/HI`；`src/main.c:ssd_ftl_boot_all`；板端 `/fc-nand/` 59/59（2026-11-11）。
- **历史/验证**：
  - 2026-11-11 判定：接受（UT 镜像专属，非产品路径） → 信息
  - 2026-11-11 板端 `/fc-nand/` 59/59（只证 FC 命令成功） → 上板

#### RISK-UT-12 chain_priority_lock 断言早于 feature 还原

- **后果**：Step 3 断言 `g_l3_prio_slots[hsq_get].out[0] == y[0]`（`:850`）排在还原 01h 原值（`:854`）之前；断言失败经 munit longjmp 跳过还原 ⇒ 器件停在非默认 timing mode 直到断电，同 power cycle 后续用例仍按默认 FMC 时序发命令 ⇒ 级联失败/时序错乱。用例头注释（`:686`）声称「还原排在断言之前」，与代码不符（H6）。
- **缓解**：高速度构建（`CONFIG_NAND_TIMING_MODE != 0`）下该用例 `MUNIT_SKIP`，不执行；Mode 0 构建可触发。restore 排在断言之前（读终值 → 还原 → 复读 → 断言）；`make tests UT=FC` 通过。
- **下一步**：需专门的 Mode 0 构建跑 `/fc-nand/lifecycle/chain_priority_lock` 才能闭合；按 R8 不临时改 `CONFIG_NAND_TIMING_MODE` 验证。
- **证据**：`tests/unit/test_fc_level3.c:850-858:test_chain_priority_lock`。
- **历史/验证**：
  - 2026-11-11 板端 rev=33 Mode 5 档 `MUNIT_SKIP`，修复未执行 → 待后续
  - 2026-11-11 `make tests UT=FC` 通过 → 编译通过

#### RISK-UT-13 teardown 不收在途资源，longjmp 后泄漏

- **后果**：`test_tear_down` 只 free 两块 malloc 暂存，不归还该用例已取出而未 release 的 command/desc/buffer；`test_setup` 有意每进程只 `fc_init` 一次（避免洗掉泄漏）。任一用例断言失败 ⇒ 池容量永久递减，后续用例可能以 `FTL_AGAIN`/池空形式失败，掩盖首因。
- **缓解**：setup 有意只 `fc_init` 一次以暴露泄漏；断言失败后以首个失败为准，重烧镜像再跑。夹具不做在途回收：回收需读未导出的 `command_state`，对在飞命令调 `fc_command_release` 会命中 release 守卫（debug 停机），收益不抵风险。
- **下一步**：引入用例级「在途登记 + teardown 全量 release」（与 level3 的 CID 槽表同源），或让 setup 检测池泄漏时 re-init 并打印诊断。
- **证据**：`tests/unit/fc_test_common.c:242-301:test_setup` / `test_tear_down`。
- **历史/验证**：
  - 2026-11-11 复查：判定接受 / 不修 → 信息

#### RISK-UT-14 level1 两条 validate 用例不含断言

- **后果**：`fc_command_validate` 三条守卫转 ASSERT 后函数体只剩 `return FC_STATUS_OK`；用例价值是「用合法输入把内层 ASSERT 跑一遍」，在 `NDEBUG` 构建下 ASSERT 被裁空 ⇒ 用例零判定、恒绿。
- **缓解**：`make tests` 当前走 debug 档（ASSERT 有效）；无意改。
- **下一步**：若将来允许 release 档跑 UT，这两条须显式标为 smoke 或删除，不得当覆盖证据。
- **证据**：`tests/unit/test_fc_level1.c:54-111:test_command_validate_op_range` / `test_command_validate_set_get_no_desc`。
- **历史/验证**：
  - 2026-11-11 复查确认：有意设计 → 信息

#### RISK-UT-25 test /ftl/ 后 BAD 计数多 3 个

- **后果**：同一镜像 boot `bad=5`；media-only 39/39 结束 `bad=8`（`bbt_persist` 1 + `bbt_ring_reload` 2，符合 RISK-UT-06）；随后 `test /ftl/`（pure 先跑 + media）99/99 结束 `bad=11`，多出 3 个未归因。候选来源：pure 对 live 实例的副作用 / format 擦除失败 / `gc_progress`/`wl_progress` 的 mig 读失败标坏。当前无证据区分。
- **缓解**：无；测试仍全过。用户裁决：bad 增量是已知问题，不再归因/追查。
- **下一步**：不追；以后若 BAD 增量影响容量或出现新的 mark-BAD 日志再重开。
- **证据**：板端 2026-11-11 20:22:43 `test /ftl/` 日志（boot `bad=5`、`bbt_persist bad_count=6`、结束 `[STAT] bad=11`）；同日 media-only 日志（结束 `bad=8`）。
- **历史/验证**：
  - 2026-11-11 20:22:43 板端对比 `test /ftl/` 与 media-only → 复现 +3，判定不追

### 已闭合（详情见 archive）

| ID | 日期 | 一句话 | 归档 |
|---|---|---|---|
| RISK-UT-01 | 2026-11-11 | munit SKIP/FAIL 行不闭合方括号与换行 | `docs/archive/known_risks_batches.md` |
| RISK-UT-05 | 2026-11-03 | DWT 32 位回绕使 munit 计时按 10.737 s 取模 | `docs/archive/known_risks_batches.md` |
| RISK-UT-09 | 2026-11-01 | media_ledger_total 漏计用户区借用日志块 | `docs/archive/known_risks_batches.md` |
| RISK-UT-11 | 2026-11-11 | block_erase_verify 判据无区分力（缺写后擦） | `docs/archive/known_risks_batches.md` |
| RISK-UT-15 | 2026-11-11 | L2P 纯 RAM 用例直改行状态，CMT/tags 跨用例残留 | `docs/archive/known_risks_batches.md` |
| RISK-UT-16 | 2026-11-11 | 两条用例语义随生产 API 改变，只改名仍必红 | `docs/archive/known_risks_batches.md` |
| RISK-UT-17 | 2026-11-11 | bbt_mark_bad 幂等断言写死 plane 0 | `docs/archive/known_risks_batches.md` |
| RISK-UT-18 | 2026-11-11 | 证据行硬编码值 / 打印非判据所用调用 | `docs/archive/known_risks_batches.md` |
| RISK-UT-19 | 2026-11-11 | 声称的覆盖没有判据（WL victim / GC 统计） | `docs/archive/known_risks_batches.md` |
| RISK-UT-20 | 2026-11-11 | WAL 兜底判据只扫 line DIRTY，漏 dirty CMT | `docs/archive/known_risks_batches.md` |
| RISK-UT-21 | 2026-11-11 | 板端三处测试假设/驱动缺失（NONE、ring 跳槽、timer poll） | `docs/archive/known_risks_batches.md` |
| RISK-UT-22 | 2026-11-11 | media 尾窗按编译期 FTL_LBA_MAX，越过 host_lba_max | `docs/archive/known_risks_batches.md` |
| RISK-UT-23 | 2026-11-11 | 早回执用例把内部读路径钉死（covered 断言过强） | `docs/archive/known_risks_batches.md` |
| RISK-UT-24 | 2026-11-11 | gc_progress 首次 urgent GC 可能只回收 valid=0 SB | `docs/archive/known_risks_batches.md` |
| RISK-UT-26 | 2026-11-11 | 测试临时强制诊断/预期失败打印违反证据分级 | `docs/archive/known_risks_batches.md` |
| RISK-UT-27 | 2026-11-11 | 结束不变量不完整（cache_stream/CMT 未查） | `docs/archive/known_risks_batches.md` |
| RISK-UT-28 | 2026-11-11 | 测试注释混有变更史/日期（H37） | `docs/archive/known_risks_batches.md` |
| RISK-UT-29 | 2026-11-11 | CMT 用例证据行硬编码 | `docs/archive/known_risks_batches.md` |
| RISK-UT-30 | 2026-11-11 | printf/断言系统性冗余强转与格式不匹配 | `docs/archive/known_risks_batches.md` |

---

## 2Z. cache 长流 + Journal 顺序映射页

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-CACHE-15 | 待上板 | 无数据 END 缺失，padding 页盘满例外未验 | 2026-10-31 上板（正常路径） |
| RISK-CACHE-16 | 待上板 | 跨命令序列 + VWC 早回执，USB 端到端未跑 | 2026-11-01 上板（UT 156/156） |
| RISK-CACHE-18 | 待上板 | 提前回执扩大掉电暴露窗口，VWC 窗口未量化 | 2026-10-31 上板（flush 屏障） |
| RISK-CACHE-23 | 待议 | 持续写期间后台 CP 被序列门控饿死 | 2026-10-31 上板观察 |
| RISK-CACHE-26 | 待定位 | cache 写压测 1536 LBA 读回全 0 | 2026-11-01 上板复现 |
| RISK-CACHE-27 | 待上板 | 早回执后读路径缺覆盖层，写完立刻读回旧数据 | 2026-11-01 上板（FTL/UT 层） |
| RISK-CACHE-28 | 待上板 | FAILC 撤销按 region 粒度降级，跨重启丢失 | 2026-11-01 上板（三档注入） |
| RISK-CACHE-29 | 待上板 | 早回执改变 flush/断电口径，长流+断电未量 | 2026-11-01 上板（flush 用例） |
| RISK-CACHE-30 | 待上板 | padding 收尾把已证明未确认页误判失败 | 未上板 |
| RISK-CACHE-31 | 待上板 | padding 回调早退分支泄漏 FC 命令 | 未上板 |
| RISK-CACHE-32 | 待上板 | 环满在 release 下静默丢已回执写 | 未上板 |
| RISK-CACHE-36 | 待上板 | 干净释放写序列时不看在飞成员，迟到完成把未证明页推进死序列 | 2026-11-13 常规回归通过（残余：组级失败+在飞成员负例未注入） |
| RISK-USBC-01 | 待上板 | deferred error 以下一条命令失败近似，非标准 sense | 2026-10-31 被动触发一次 |
| RISK-USBC-03 | 待定位 | USB 盘间歇报无介质，主机侧已止血 | 2026-11-12 主机侧止血 |

### 详情

#### RISK-CACHE-15 padding END 收尾（本仓自拟）

- **后果**：器件没有不带数据的 END。padding END（本仓自拟）：提交一个不映射的零页（META=UNKNOWN）作为 48h，用其 FAIL_CACHE 结算最后一页。**盘满例外**：padding 页也分配不到时，为避免「序列门控 ↔ GC 等 Journal 容量」死锁，放弃序列（未确认页 IO_ERROR + deferred error），让 GC 回收。剩余风险：padding 页落在新块 page0 时若失败，会关闭 hot SB（丢弃该块剩余空间）；padding 页被 GC 视为 invalid（无害）。
- **缓解**：有序收尾路径不 abort（盘满例外除外）；padding 页成功且为 page0 时记 `page0_programmed`，失败关 SB。
- **下一步**：盘满例外（无页可分配 → abort）未验；观察 `pc=`；若失败率异常，查 padding 页的 META/ECC 与 page0 记账。
- **证据**：`src/ftl/ftl.c:ftl_cache_pad_submit` / `ftl_cache_pad_callback`；`src/ftl/cache_batch.c`。
- **历史/验证**：
  - 2026-10-31 H2testw 415 MB 全程 `pc=51`、无 `drain stalled` → 正常路径上板通过（padding 页落 page0 的 plane1 屏障问题由 RISK-CACHE-24 修复并验证）
  - 2026-10-31 之前的旧方案：空闲 1 s 后 abort，未确认页按 IO_ERROR 结算并丢已提前回执的数据 → 已弃用
  - （日期未记录）盘满例外未验

#### RISK-CACHE-16 跨命令序列 + VWC 早回执

- **后果**：① 所有写段（含末段）回执；设备序列跨 host 命令延续；② 顺序页跨命令续用到 63 pair 再断（`sw` ≈ 1/63 pair）；③ padding END 收尾；④ deferred error 由下一条命令上报。回执点 = FTL 页编程完成（早回执）；序列边界不变（`next_lba`、≈63 页/段），组的完成点与未确认页的可见性见 RISK-CACHE-27/28。
- **缓解**：五关构建零新告警（2026-11-01）；`info` 仍看 `sw=`/`pc=`/`jf=`。
- **下一步**：上板判据：`pages/starts≈63` 不退化、`aborts=0`、H2testw 415 MB 0 错误、写完立刻读回正确；旧模型 2026-10-31 的 415/415 MB + `sw=1663/1610`（62.8）只作基线。
- **证据**：`docs/design/ftl_design.md` §3.16.5/§3.16.10；`src/ftl/cache_batch.c` / `ftl.c` / `journal.c`。
- **历史/验证**：
  - 2026-11-01 release -O2 全量 `test` 156/156（含 media 36/36 与 `/speed` 5/5） → UT 路径已上板；H2testw/USB 端到端未跑
  - 2026-10-31 旧模型 H2testw 415/415 MB、`sw=1663/1610`（62.8） → 上板通过

#### RISK-CACHE-18 提前回执扩大掉电暴露窗口

- **后果**：host 已 ack 但尚未确认的写（最后 1 页 + Journal 顺序页 ≤252 KiB 的映射）在掉电后可能回滚到旧映射。`SYNCHRONIZE CACHE` 在正常路径是持久性屏障（flush 先 padding END 再刷 journal/L2P）；超时仍按既有 best-effort 语义完成。
- **缓解**：volatile write cache 的固有取舍；flush 语义完整。
- **下一步**：设备 VWC 窗口量化 = 可选 / 暂缓（用户 2026-10-31 决定不做）——flush 后零丢失；未 flush 运行里主机发送 159.8 MB、设备实际占用 1222 块 × 128 KiB = 160.2 MB（数据页全部落盘），尺寸缺口是主机 FS 元数据；只剩 ≤1 页 JSEG 映射（≤252 KiB）未量，属设计上限。若将来要做，方法见 `testing_guide.md` 的可选条目（预分配 + WriteThrough + pattern 比对）。
- **证据**：`src/ftl/ftl.c:ftl_flush_begin` / `ftl_flush_step`；`docs/design/ftl_design.md` §3.16.5。
- **历史/验证**：
  - 2026-10-31 `chkdsk /f` 修正主机 FS 后 verify 127 MB 0 错误、`dng=0`/`rrab=0` → 已落盘数据零损坏；丢失 11 MB 尾 = 主机写回缓存 + VWC 窗口 + FS 元数据未落盘（未分离）
  - 2026-10-31 H2testw 写 168 MB → 安全弹出 → 断设备电 → 上电 `boot type=1`、`journal_entries=0`、`anomalies=0`；H2testw verify 167 MB 0 错误、无尺寸缺口 → flush 屏障上板通过
  - 2026-10-31 写 130 MB 断电重启，存活 117 MB verify 0 错误、`anomalies=0` → 窗口外无损坏成立；117→130 MB 尾差无法归因

#### RISK-CACHE-23 持续 host 写期间后台 CP 被序列门控饿死

- **后果**：持续 host 写期间，写序列 `active` 时门控拒绝非所有者提交，后台不置 `break_requested`；序列在 Journal 配额处结束时 drain 立刻接手同一 host 命令剩余 LBA，门控空窗只有 µs 级（仅当配额恰落在大块边界时约 18 ms）⇒ Checkpoint 推进很慢（后台让步窗口过小），掉电恢复点较旧；不是死锁。元数据收效变慢。
- **缓解**：CP 自带 1 s 心跳可见；序列门控不改。
- **下一步**：重访触发：RISK-CACHE-22 归因后若指向 Journal/元数据回收，本项优先；可选方案：must-run 后台在连续被拒 N ms 后允许请求一次断点（`ftl_op_seq_gate_breaks` 加年龄条件），或序列结束后空出让位一轮（实现项与判据见 `known_iss.md` ISS-FTL-14）。
- **证据**：`src/ftl/ftl_op.c:ftl_op_seq_gate`；`src/ftl/checkpoint.c:cp_job_step`；板端 `[CP] job phase=8` 日志。
- **历史/验证**：
  - 2026-10-31 上板观察：t=80 s 起 `phase=8`（WEAR_WRITE）持续 460 s、心跳 `age=1000~2003ms`、`cp=100`；上一失败运行的 `closed=1896`/`dir_ckpt seq=458` 下次开机正常恢复 → 实测为慢，非死锁

#### RISK-CACHE-26 cache 写压测 1536 LBA 读回全 0

- **后果**：连续写 51,200 LBA（16 LBA/请求、深度 2）→ flush → 同参数重铺 → flush → 读回；`lba=11264..12800`（region 22~24）整段 `got=0x00`，而 `wr=0`/`prep=0`（两轮写主机都看到成功）。同组合日志另有 5 条 `[FTL] dangling lba=40128/42600/47144/49320/51192 … st=CLOSED`（RISK-MIG-03 同型）与 `[JRN] erase kick failed`。功能套件（FC/FTL pure/FTL media）全绿，仅该压测组合触发。
- **缓解**：无缓解；压测侧 `vfy` 计数保留并接入 pass/fail。
- **下一步**：失败后立刻 `map 11264` 分流：INVALID ⇒ 映射在 L2P 层丢失（查 eviction/区域写回是否把脏行洗成已落盘）；非 INVALID ⇒ 映射在、页内容为 0（查 cache program 的页是否真编程/是否被 GC 覆盖）。再取 `info` 的 `dng=`/`rrab=`。2026-11-04 写回 CMT 改造后必须复跑（dirty 单元已从 line 移到 CMT，原失败模式可能变化）。
- **证据**：`src/ftl/l2p_cache.c:l2p_line_evict` / `l2p_flush_kick` / `l2p_load_begin`；`src/ftl/ftl.c:ftl_confirm_write` / `ftl_cache_wr_confirm_step`；板端 2026-10-31 23:47 `/speed/ftl_rw` 日志（rd=1 组合）。
- **历史/验证**：
  - 2026-11-01 00:53 镜像复现于写阶段结束后的全量校验（`lba=18944..19000` 共 8 个 8-LBA 请求读回 0），L2P cache 处于「64 行全 DIRTY、`no evictable line`、CP `region=39 dirty but state=LOADING — abort`」饱和态 → 待定位
  - 2026-10-31 23:47 首次复现（`lba=11264..12800` 整段 0） → 待定位

#### RISK-CACHE-27 早回执后读不到新数据

- **后果**：控制器层提交即回执（`usb_msc.c` 写槽 `ack_sent`），但 FTL 读路径只有 wbuf 一个覆盖层，已回执但未确认的 cache-program 页不在其中 ⇒ 主机「写完立刻读同一 LBA」会读到旧映射的旧数据。
- **缓解**：`cache_stream.seq.wr` 升级为「已编程页表」（lba/pba0/pba1/cw_bitmap/state），读路径在 `wb_read` 之后、`l2p_read` 之前查该表，命中直接读刚编程页；`cache_read_probe_pair` 对未确认 pair 判不可批量。页在自身命令完成时入表（提交时不压下标）；结论用独立游标 `pend_idx`（不能复用队头：确认背压会让队头停在「已证明等确认」的页，新结论会打错页 ⇒ 环排不空、flush 超时）。
- **下一步**：上板：写完立刻读回（未确认窗口）逐字节比对；H2testw 415 MB 0 错误。
- **证据**：`src/ftl/ftl.c:ftl_issue_subop` / `ftl_cache_wr_member_done` / `ftl_cache_wr_take_verdict`；`src/ftl/cache_batch.c:cache_read_probe_pair`；`include/ftl/ftl.h:ftl_cache_wr_s`。
- **历史/验证**：
  - 2026-11-01 media 36/36 含 `cache_failc_abort` 三档；`/speed` 写后读回 err=0 → FTL/UT 层上板通过；USB 端到端未注入
  - （日期未记录）五关构建零新告警 → 编译通过

#### RISK-CACHE-28 FAILC 撤销的语义边界（本仓自拟取舍）

- **后果**：① 只对已确认（已进 L2P）的失败页做「L2P 作废 + region 降级 ⇒ 读 IO_ERROR」；同批未确认页被中止丢弃时保留旧映射，读仍返回旧数据（失败由 deferred error 上报，符合「一次失败写不影响旧数据可见性」）；② region 降级位是 RAM 级、重启后丢失（`l2p_owner_invalidate` 会清），所以「失败 LBA 读 IO_ERROR」只在本开机周期成立；重启后这些 LBA 是「未映射 ⇒ 零填」。主流对照：NVMe 用命名空间级策略——DLFEAT 规定去分配/未写 LBA 的读值（`001b` = 全零，与本仓重启后一致），另有 DULBE 特性让整个命名空间「读去分配区间」报错；没有 per-LBA 持久丢失标记这种形态。⇒ 裁决不升级（不持久化降级位）：无出处支持、要改介质格式（版本+1、首启格式化）、收益仅跨重启可见，失败本身已经由 deferred error 上报。FAILC 撤销的 region 粒度降级：已早回执的页被器件 FAILC 判失败时，撤销做法是把该页 LBA 在 L2P 中写 `NAND_PBA_INVALID` 并标记整 region degraded（复用 RISK-BOOT-18 机制）⇒ 该 region 内其它未映射 LBA 的 host 读也会报 IO_ERROR（粗于「只影响失败 LBA」）。主流做法是 per-LBA 的丢失标记/deferred sense，本仓因无自定义 sense 且不想新增持久状态而取 region 粒度。
- **缓解**：失败可见（不静默、不回旧数据）；粘性错误由下一条 host 命令上报一次。region degraded 在 `mark_failed` 时立即置位（不等撤销）——否则撤销走完前 L2P 仍指着编程失败的页，读会返回可疑内容；读路径用 `ftl_cache_wr_failed_covers()` 把覆盖失败页的 LBA 直接判 IO_ERROR，cache 读批量对该 pair 判不可批量。
- **下一步**：上板 + 注入：`fi_cache_fail_arm` 注入 FAILC ⇒ 下一条命令 IO_ERROR 一次、失败 LBA 读 IO_ERROR、同 region 未映射 LBA 也 IO_ERROR（把粒度后果记为预期）；若将来需要精确粒度，先裁决「per-LBA 丢失位图 + 掉电语义」。
- **证据**：`src/ftl/ftl.c:ftl_cache_wr_member_done` / `ftl_cache_wr_failed_covers`；`src/ftl/l2p_cache.c:l2p_is_region_degraded` / `l2p_note_region_lost`。
- **历史/验证**：
  - 2026-11-01 `cache_failc_abort` arm=1/2/last 全过（失败页读 IO_ERROR、deferred 只报一次、重启后读零） → FTL/UT 层上板通过；USB 端到端未注入

#### RISK-CACHE-29 早回执改变 flush/断电口径

- **后果**：① `SYNCHRONIZE CACHE` 等「已编程页表」全部被证明并确认进 L2P（`wr.count==0`）且没有在飞成员（`wr_inflight==0`）才算完成——若确认长期背压（Journal 容量 / L2P region 加载起不动），flush 会等到 `FTL_FLUSH_TIMEOUT_MS` 走 best-effort，那一路不是绝对屏障；② 掉电窗口 = 已编程未证明页 + 未落盘 Journal 映射（口径需重新量化，RISK-CACHE-18）；③ FAILC 撤销依赖 `jrn_reserve` 成功，撤销被背压卡住期间失败页已判失败但映射未作废——读已被 `failed_covers` 拦成 IO_ERROR，不会读到坏页，但环会占用直到撤销推进。
- **缓解**：flush 判据为 `wr.count==0 && wr_inflight==0`（页在完成时才入表，只看环会漏掉仍在编程的页）；窗口构建的环/顺序页余量同样计入在飞成员；失败页立即置 region degraded + 读路径拦截；撤销有硬错误时丢弃条目（留痕后不永久卡队头）。
- **下一步**：上板：`/speed/ftl_rw` 长流 + flush 后读回；断电重启（RISK-CACHE-18 方法）重新量化窗口；注入 FAILC 后确认读 IO_ERROR 且环能排空。若 flush 超时在正常负载下可复现 ⇒ 查确认背压来源（Journal 容量 / region 加载）。
- **证据**：`src/ftl/ftl.c:ftl_flush_step` / `ftl_cache_wr_confirm_step`；`src/ftl/l2p_cache.c`；`include/ftl/ftl.h:FTL_FLUSH_TIMEOUT_MS`。
- **历史/验证**：
  - 2026-11-01 media 套件的 flush/重启用例通过 → 部分上板；长流+断电窗口未量

#### RISK-CACHE-30 padding 收尾误判已证明未确认页失败

- **后果**：`ftl_cache_pad_callback` 的防御循环只判 `state != FTL_CACHE_PAGE_FAILED`，而页表里的页要先确认（Journal→L2P→BST）才弹出；确认背压时（Journal 容量 / region 加载），收尾页完成那一刻表里仍可能有 `proven == true` 的页 ⇒ 被判 `pad: stray page` ⇒ 撤销有效映射 + `deferred write error` ⇒ host 命令失败（现场：`cache-wr failed lba=2136 (pad: stray page)` + `deferred write error from an earlier acked segment`，`/speed` 命令 3/4 直接 FAIL / `WR=58447 RD=0`）。与 `ftl_cache_wr_abort` 的语义不一致——后者明确跳过 `proven == true` 的页（「器件已证明该页编程成功：数据有效」）。
- **缓解**：判定条件为 `proven == false`：只判「等不到结论」的页；已证明页留给 `ftl_cache_wr_confirm_step` 正常确认并弹出。
- **下一步**：上板判据：原 4 条命令下不再出现 `pad: stray page` / `deferred write error`；`/speed` 写阶段 `err(wr=0)`、`pc=` 正常。代码审计同时排除「L2P region 页被双写」：槽游标在 (block,plane) 内单调；块置 NONE 只有两条路（擦除完成 / 开机 `md_log_block_erased` 逐页判空），前者在 `md_log_block_is_reclaimable` 下不可能擦当前块，后者读失败按非空处理（保守）。
- **证据**：`src/ftl/ftl.c:ftl_cache_pad_callback`（stray loop）/ `ftl_cache_wr_confirm_step` / `ftl_cache_wr_abort`。
- **历史/验证**：
  - 2026-11-01 判定补 `proven == false` → 未上板

#### RISK-CACHE-31 ftl_cache_pad_callback 早退分支泄漏命令

- **后果**：序列已中止／已关闭时函数在 `ftl.c:1554` 直接 return，未释放 completion 对应的 FC 命令；本服务函数拥有命令，只有正常路径 `fc_command_release`。`ftl_op_drain` 在回调前已清 request 表项，迟到完成的兜底分支不会救它。失败场景：padding END 在飞期间成员失败或 `ftl_cache_wr_abort`（正是该分支注释描述的场景）⇒ 每条完成泄漏 1 个 command + desc 链 + buffer；重复触发耗尽 FC 池 ⇒ 全系统 `FTL_AGAIN`，`ftl_teardown` 报泄漏。
- **缓解**：`cmd` 在守卫之前查找；早退路径先 `fc_command_release` 再返回。UT 负例（pad 在飞 → abort → pad 完成）待补，测试源由用户维护（R4）。
- **下一步**：上板判据：注入成员失败后 `fc_get_resource_stats().command_used` 回到基线。
- **证据**：`src/ftl/ftl.c:ftl_cache_pad_callback` / `ftl_cache_pad_submit` / `ftl_cache_wr_abort`；`src/ftl/ftl_op.c:ftl_op_drain`；同表 RISK-CACHE-15 / RISK-CACHE-30。
- **历史/验证**：
  - 2026-11-05 早退路径先 release 命令 → 未上板

#### RISK-CACHE-32 ftl_cache_wr_ring_push 环满静默丢已回执写

- **后果**：`ftl_cache_wr_member_done` 只 `ASSERT(false)`（release 裁空），随后仍按 `SSD_STATUS_OK` 结算组 ⇒ 已提前回执的页不进页表、映射永不确认。结构性不可达：环深 16 ≫ 在飞窗口（≤4 页/窗口 + 前窗尾页）。
- **缓解**：push 失败走 LOGE（限流）+ `ASSERT_MSG` + `ftl_cache_wr_abort()` + 本组结算 `SSD_STATUS_IO_ERROR`（host 重试），不按 OK 结算；当前页未进环、无需 undo（映射从未确认）。
- **下一步**：重开触发：调小环深 / 放大 chunk 或窗口 / 新增绕过 `wr_inflight` 门的提交路径。上板：正常路径不触发（需临时改环深注入）。
- **证据**：`src/ftl/ftl.c:ftl_cache_wr_member_done` / `ftl_cache_wr_ring_push` / `ftl_cache_wr_abort`。
- **历史/验证**：
  - 2026-11-05 环满改 abort + IO_ERROR → 未上板


#### RISK-CACHE-36 干净释放写序列不看 `wr_inflight`

- **后果**：`ftl_cache_seq_release` 只按页表（`has_unproven`）判断「能否干净清除」。而页在**完成时**才入表（早回执）⇒ 组级失败若正好落在一个窗口的命令全部在飞、页表为空时，会**干净清除**序列：`kind=NONE`、不升 `gen`。随后完成的命令按 `gen` 匹配把页推进页表，这批页永远等不到后继 `FAIL_CACHE`：确认步只能判 `no verdict` 并把**已编程成功的页**当失败撤销 ⇒ 置 `deferred_error` ⇒ host 下一条命令失败、写阶段中止、flush 不执行（teardown `cmt_dirty != 0`）。与 RISK-CACHE-19「失败序列其后页一律不确认」直接矛盾——那条语义靠 `abort` 升 `gen` 实现，而该路径绕过了 `abort`。
- **缓解**：① `ftl_cache_seq_release` / `ftl_cache_seq_discard` 的判据加上 `wr_inflight != 0`：有成员在飞一律走 `ftl_cache_wr_abort`（升 `gen` ⇒ 迟到完成丢弃本页并按 IO_ERROR 结算组）；② 确认步的安全网不再要求 `kind == WRITE`——`wr_inflight != 0` 时一律等（在飞成员的 FAILC 仍可能证明该页）。
- **下一步**：上板回归 `/speed/ftl_rw` 三档 + 全量 `test`。若仍失败，看故障 shell 的 `DBG … seq-close by=… wr=…` 判定是哪个路径关的序列（release/abort/pad/discard），并确认组级失败的触发点（`subop failed` / `drain stalled`，两者都受 5 s 限流）。
- **证据**：`src/ftl/ftl.c:ftl_cache_seq_release` / `ftl_cache_wr_abort` / `ftl_cache_wr_confirm_step` / `ftl_cache_fail_note`。
- **历史/验证**：
  - 2026-11-13 板端现场（02:00 镜像，`/speed/ftl_rw chunk=32`，`base=495392`）：`wr-fail lba=497632 why=no verdict`，快照 `ctx kind=0 act=0 clos=0/0 wr=1/3`（页表 1 页、**3 条命令仍在飞**）⇒ 序列在命令在飞时被干净清除。修复 + 加粘滞现场（`last_fail` 含页表快照、`last_close` 含关闭路径）→ 编译通过，未上板
  - 2026-11-13 板端（02:09 镜像）复现：`seq-close by=pad` 且关闭时 `wr_inflight=4` ⇒ 实际触发路径是 **RISK-CACHE-37**（pad 在飞时续写窗口），本条判据保留为同族防护（组级失败+在飞成员的正确结算）


#### RISK-USBC-01 deferred error 的近似

- **后果**：已提前回执的写段后来失败时，本仓让下一条 READ/WRITE/FLUSH 命令直接失败一次（承载错误的那条命令会设置 MEDIUM ERROR / WRITE ERROR sense；仍不是 CHECK CONDITION + REQUEST SENSE 形态）。与公开 SCSI 语义（CHECK CONDITION + deferred sense）不同：主机拿不到失败 LBA/异常类别（本仓出处层无 SPC/SBC 原文，具体 ASC 不作断言），FS 只能整条命令重试；若下一条是读，读被失败一次。
- **缓解**：错误不吞：下一条命令必失败一次并清标志；日志 LOGE 记录失败 lba/len。`usb_msc_take_deferred_error()` 先问各 FTL 实例（`ftl_take_deferred_error`，早回执后失败结论只存在于 FTL 的粘性标志里），再落回控制器自己的标志。
- **下一步**：主动注入仍未做（ISS-FTL-16）；主机侧后果（重挂/只读）符合预期，未改回末段等待。
- **证据**：`src/hil/usb_msc.c:usb_msc_take_deferred_error` / `usb_msc_slot_complete`。
- **历史/验证**：
  - 2026-11-01 `usb_msc_take_deferred_error()` 改为先问各 FTL 实例 → 未上板
  - 2026-10-31 首轮失败运行被动触发：`write chunk failed after ack … — deferred` + 下一条命令失败一次，主机判卷被改并重挂 → 近似路径可见；主动注入未做

#### RISK-USBC-03 USB 盘间歇报无介质

- **后果**：主机每 0.5 s 采样 `Get-Disk -Number 2`，30 s 内 7/54 次返回 `Size=0` / `PartitionStyle=RAW` / 0 分区，其余为 440,139,776 B / MBR / 1 分区；同一时段 PnP 实例全程 Present/OK（无重枚举）。共存异常：① 现存 MBR 分区 `offset=65,536 size=440,598,528`，结束地址 440,664,064 超出盘尾 524,288 B（容量变小后未重建的陈旧 MBR）；② 卷 F: 无文件系统（历史 FAT 格式化失败遗留，`Get-Volume` 报 `Size=0 / OperationalStatus=Unknown`）。Windows 对无 FS 的可移动卷反复重扫 ⇒ 弹「需要格式化 / 无法访问」。
- **缓解**：主机侧摘掉 F: 盘符（`mountvol F: /D` + `Remove-PartitionAccessPath`，未动介质数据）；60 s 复测 F: 未回挂、`Get-Disk` 异常采样 0/30 ⇒ 间歇 Size=0 至少部分由主机反复探测无 FS 的无效卷触发，非纯固件。⚠ 重启/重插后 Windows 可能重新自动分配盘符，需复测；根治仍是重建 MBR + 格式化。固件侧：`tud_msc_test_unit_ready_cb` 只返回 `g_ctrl.state == READY`，`tud_msc_capacity_cb` 恒返回 `ssd_controller_host_lba_max()`（常量），瞬时 0 的另一来源未确认。
- **下一步**：复现窗口开串口抓 `g_ctrl.state` 与 SCSI trace，区分「主机未发 READ CAPACITY」与「固件返回 NOT READY」；止血（主机侧）= 管理员删 F: 盘符，或跑 `tools/usb_verify/usb_rebuild_mbr.ps1` 重建分区并格式化（同时修越界 MBR）。
- **证据**：`src/hil/usb_msc.c:1212`（test_unit_ready）/ `:1224`（capacity）/ `src/hil/controller.h:138`；主机 `Get-Disk`/`Get-Partition`/`Get-Volume` 采样 2026-11-12 21:06:04–21:08:21；同表 RISK-USBC-02。
- **历史/验证**：
  - 2026-11-12 21:11 主机侧摘掉 F: 盘符 → 异常采样 7/54 → 0/30（止血）
  - 2026-11-12 21:06–21:08 采样：30 s 内 7/54 次 Size=0/RAW → 待定位

### 已闭合（详情见 archive）

| ID | 日期 | 一句话 | 归档 |
|---|---|---|---|
| RISK-CACHE-33 | 2026-11-13 | padding END 早于数据成员排空，迟到完成卡死队头 | `docs/archive/known_risks_batches.md` |
| RISK-CACHE-34 | 2026-11-13 | pad 收尾漏掉未证明的 head（根因实为 RISK-CACHE-37） | `docs/archive/known_risks_batches.md` |
| RISK-CACHE-35 | 2026-11-13 | 残留 cache 序列门控拒绝格式化同步擦写 | `docs/archive/known_risks_batches.md` |
| RISK-CACHE-37 | 2026-11-13 | padding END 在飞时续写窗口，新窗口末页永无 FAIL_CACHE | `docs/archive/known_risks_batches.md` |
| RISK-USBC-02 | 2026-11-12 | 固件 format 后 Windows 非 NTFS 格式化失败（非代码问题） | `docs/archive/known_risks_batches.md` |

---
## 2AA. 平台定时调度器（`timer`）

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-PLAT-09 | 待上板 | SysTick 时基与 TIM6 调度心跳的超时判据未板验 | 2026-11-12 常规回归通过（HIL UT + /speed）；残余：超时判据直接断言 |
| RISK-PLAT-10 | 待上板 | mig_eval 混用 TIM6 与 SysTick 两套时间坐标 | 未上板 |
| RISK-PLAT-11 | 信息 | 不引入 TIM2/TIM5 作独立 µs 时基的判定 | 未上板 |
| RISK-PLAT-12 | 待上板 | µs 时间戳从 DWT 改 SysTick，未板验 | 2026-11-12 常规回归通过（HIL UT + /speed）；残余：µs 时间戳边界 |

### 详情

#### RISK-PLAT-09 系统时基与调度心跳分离

- **后果**：系统 ms 读 `HAL_GetTick()`，µs 用 `SysTick->LOAD/VAL` 在 1 ms 窗口内插值；TIM6 只服务 timer 任务表的到期判定（`g_timer_tick_ms`）。失败场景：① SysTick 停止 / 被改配置 ⇒ 所有 ms/µs 超时与 NAND 忙期判据失效；② HAL tick 与 VAL 在重装边界的相位差（高优先级 ISR 延迟 `HAL_IncTick`）⇒ µs 时间戳最多滞后约 1 ms 后跳回；③ TIM6 中断停止 ⇒ watchdog / `[STAT]` / FTL 周期评估停摆（IWDG 硬件仍独立）；④ `timer_scheduler_poll()` 被长任务拖住 ⇒ 调度节拍抖动。
- **缓解**：SysTick 由 HAL 配成 1 ms；`delay_init()` 从 LOAD 反推每微秒周期数；ms / VAL 双读避免边界错配；µs 超时一律无符号差值；NAND 忙期仍有轮询计数兜底；TIM6 启动点紧跟 `sys_init()`；ISR 只 `tick++`（H21）。
- **下一步**：上板核 SysTick µs 时间戳单调、NAND 忙期超时不退化、`t=` 递增、`[STAT]` 10 s ±1 轮、GC/CP 节拍不退化、watchdog 30 min 无复位、UART flush 超时仍生效；异常先查 `HAL_InitTick` / 时钟树。
- **证据**：`src/platform/delay.c` `delay_init` / `delay_time_now_us` / `delay_time_now_us64`；`src/platform/stm32h743/delay_port.c`；`src/platform/platform.c` `.time_ms`；`src/platform/timer.c` `timer_scheduler_now_ms`；`src/platform/stm32h743/timer_port.c`；`src/main.c` `timer_timx_int_init`。
- **历史/验证**：
  - 2026-11-03 时基改为 SysTick + TIM6 心跳、删除 DWT → 未上板

#### RISK-PLAT-10 mig_eval 时间坐标混用

- **后果**：`mig_eval` 保留原 `else if` 链（GC→WL→predict）由 10 ms timer 任务调用，`mig_handle` 只留在飞推进 / 会话 / 紧急 GC。失败场景：① 注册失败（表满 / 未初始化）⇒ GC/WL/predict 永不评估（有 LOGE）；② timer 回调在 `ftl_service` 之后 ⇒ GC kick 延迟一圈；③ `m->level` 未发布（`mig_handle` 未跑）⇒ 按 IDLE 评估；④ 无时钟时依赖 `mig_handle` 每轮回退。若 `mig_eval` 直接用 timer 传入的 TIM6 `now_ms`，而 `mig_handle` / `mig_job_*` 用 SysTick，则 `gc_session_*` / `gc_nogain_ms` / `wl_eval_last_ms` / `predict_last_ms` 混用两套时间坐标。
- **缓解**：`mig_eval(m)` 内部读 `platform_now_ms()`（SysTick），TIM6 只负责唤醒；注册失败打 LOGE；`mig_handle` 仍推进在飞 job 与紧急 GC；`platform_now_ms() == 0` 时 `mig_handle` 直接调 `mig_eval(m)`；WL 5 s / predict 1 s 内部门不变。
- **下一步**：上板看 `[GC] trig` 不高于 10 ms/次、WL/RR 仅 IDLE、H2testw 415/423 MB 0 错误、吞吐不降、无 `confirm park timeout`；观察 `[SYS] migrate timer register failed`。
- **证据**：`src/ftl/migrate.c` `mig_eval` / `mig_timer_register`；`src/main.c` 注册循环；`include/ftl/migrate.h`。
- **历史/验证**：
  - 2026-11-03 周期评估改 timer 表驱动，`mig_eval` 内部统一读 `platform_now_ms()` → 未上板

#### RISK-PLAT-11 不引入 TIM2/TIM5 作独立时基

- **后果**：timer 调度器心跳只需周期中断，TIM6（basic）足够；系统 ms/µs 时基由 ARM 核心 SysTick 提供。若将来要求超过 SysTick 1 ms 窗口的 32-bit µs 时间戳，当前设计没有独立硬件计数器。⚠「基本定时器 / 通用定时器」是 STM32 器件分类（RM0433），不是存储主控架构判据；`docs/reference/` 没有「SSD 主控只有基本定时器」的出处，不得写成行业标准（§〇 / R12）。
- **缓解**：`delay_time_now_us64` 用 HAL tick 扩展毫秒，不受 1 ms 窗口限制；NAND 忙期还有 SysTick µs + 轮询计数双保险（`nand_engine.c`）。
- **下一步**：重访触发 = 需要不依赖 SysTick 的 32-bit µs 自由计数器，或 SysTick 在目标平台不可用；届时把 T2/T5 作为独立设计项评估，不顺手改现有 TIM6 心跳。
- **证据**：`docs/design/platform_abstraction.md` §8.4；`src/platform/timer.c`；`src/platform/stm32h743/timer_port.c`；`include/platform/delay.h` `delay_time_*`；`src/nand/nand_engine.c`。
- **历史/验证**：
  - 2026-11-03 判定 TIM6 心跳足够，T2/T5 仅在需要独立 µs 计数器时评估 → 信息归档

#### RISK-PLAT-12 DWT 从产品路径删除

- **后果**：`platform_time_us`、NAND 忙期、NAND dispatch 最短忙期、USB submit→complete 计时改读 SysTick µs 时间戳（字段 `start_cycles` / `wait_start_cycles` / `submit_cycles` 改名 `*_us`）。失败场景：① `HAL_IncTick` 被高优先级 ISR 延迟时，ms 与 VAL 的相位差让 µs 时间戳最多滞后约 1 ms（随后跳回）；② SysTick 被停 / 改 LOAD ⇒ µs 时间戳失效；③ `tests/framework/munit.c` 仍用 DWT 计时（仅测试基础设施，不影响产品固件）。
- **缓解**：SysTick 由 HAL 配置并随 `HAL_RCC_ClockConfig` 重配；`delay_init()` 从 LOAD 反推速率；ms / VAL 双读；µs 超时无符号差值；NAND 忙期轮询计数兜底；UT 用注入时钟。
- **下一步**：上板对比 NAND 忙期、USB latency、FTL `time_us` 与 DWT 基线不出现数量级偏差；长时运行无异常超时；移植到无 SysTick 的 target 前先补独立计数器。
- **证据**：`src/platform/delay.c` `delay_time_*`；`src/platform/platform.c` `platform_time_us`；`src/nand/nand_engine.c` `nand_engine_wait_latch/timeout`；`src/nand/nand_dispatch.c` `wait_start_us`；`src/hil/usb_msc.c` `submit_us`；`include/platform/delay.h`。
- **历史/验证**：
  - 2026-11-03 DWT 改 SysTick µs，`*_cycles` 字段改名 `*_us` → 未上板

---

## 2AB. NAND 拓扑扫描（`nand_target_probe` / `ftl_topology_build`）

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-TOP-01 | 待上板 | 扫描得到的多 target / 多 LUN 拓扑路径未板验 | 未上板（单 die 硬件，多 target 路径不可达） |
| RISK-TOP-02 | 待上板 | sparse 拓扑下 controller FLUSH 按 3D 槽遍历会空指针 | 未上板 |
| RISK-TOP-03 | 待上板 | LUN 数按器件探测，ONFI 偶发失败即拒绝启动 | 未上板 |

### 详情

#### RISK-TOP-01 板级拓扑是扫描结果

- **后果**：`nand_scan_targets` 在 armed 前逐个探测 (ch,ce)，`g_nand_targets` 记录 present / lun_count；`ftl_topology_build` 给实际存在的 LUN 分配紧凑 die 号并维护两张逆映射表。Phase 2a 实例遍历（boot / `[STAT]` / 后台节拍 / `ftl_service` / 容量统计 / ftl_op 服务查询 / controller cancel / USB deferred / CLI）走 `for die < DIE_MAX` + `g_die_table`；Phase 2b 的 L2P 路由、容量、介质布局版本、controller 槽位校验按运行时 `DIE_MAX`，数组按静态上限 `FTL_DIE_MAX` 分配。失败场景：① 当前硬件只有 FMC Bank3 + 单 NCE，`nand_select_target` 通路表只有 (0,0) ⇒ 多 target 未真正验证；② 主 target (0,0) 不存在或 ID / ONFI 几何不符 ⇒ 拒绝启动；③ ONFI `logical_units > LUN_MAX` ⇒ 拒绝启动；④ `DIE_MAX == 0` 时 `lba_2_die` / `l2p_global_*` 会除零（`ftl_topology_build` 无 die 即失败，boot 不进 FTL）；⑤ 多 die 介质版本 = bit31 标记 + `boot_crc32`(die 数 + topology map)，哈希碰撞 / 拓扑变化未识别时会误读旧布局；⑥ 稀疏拓扑下 die 槽为 NULL，`ftl_op_route_get` 对 absent 返回 NULL；⑦ 当前器件 `logical_units=1`，多 LUN 路径未上板；⑧ `ftl_ut_common` 每个套件 setup 用 `g_nand_targets` 重建 die 表；⑨ die 号为 `BYTE` + `0xFF` 哨兵，`FTL_DIE_MAX > 255` 编译期拒绝。
- **缓解**：扫描在 armed 前、低速 Mode 0；两张表往返自检；满配时 `die == topo_2_die(topo)`；L2P / 容量 / 介质版本统一走 `DIE_MAX`；单 die 版本保持 BASE；稀疏槽位只校验 present。
- **下一步**：上板（地址 / 布局类，必须）：`[NAND] scan ch=0 ce=0 present luns=1` + `[FTL ] topology: dies=1`；单 die 介质不触发重格式化（版本仍 14）；H2testw 读写 / 坏块 / 吞吐；每实例 `[FTL ] instance ...` 一行无遗漏。多 target 需扩展 `nand_select_target` 板级通路表；多 LUN 需换器件再验。
- **证据**：`src/nand/devices/mt29f___g08a.c` `nand_target_probe` / `nand_scan_targets`；`include/nand/nand_config.h` `g_nand_targets`；`src/ftl/ftl.c` `ftl_topology_build`；`include/ftl/ftl_config.h`；`include/ftl/l2p_cache.h`；`src/ftl/metadata.c` `ftl_media_layout_version`；`src/main.c` / `src/hil/controller.c`。
- **历史/验证**：
  - 2026-11-03 Phase 1 / 2a / 2b 落地（扫描拓扑、实例遍历、L2P / 容量 / 版本按 `DIE_MAX`）→ 未上板

#### RISK-TOP-02 sparse 拓扑下 FLUSH 遍历 3D 槽

- **后果**：`ssd_controller_submit` 的 FLUSH 分支与 `ssd_controller_flush_service` 遍历全部 `CH×CE×LUN`，sparse 拓扑下 absent 槽为 NULL ⇒ debug `ASSERT` 失败 / release 空指针。
- **缓解**：两处改为 `for die < DIE_MAX` + `controller->ftl[die]`，只推进 present die；`ssd_controller_init` 已 memset die 槽并只校验 present。
- **下一步**：上板在 sparse 拓扑（或手工把某 (ch,ce,lun) 标 absent）下执行 FLUSH，确认不崩且 present die 都被推进；当前单 die 板无法覆盖。
- **证据**：`src/hil/controller.c` `ssd_controller_submit` / `ssd_controller_flush_service`。
- **历史/验证**：
  - 2026-11-03 Phase 2b 审计改为按 die 遍历 → 未上板

#### RISK-TOP-03 LUN 数是器件属性

- **后果**：`nand_target_probe` 不再在 ONFI 读失败时回退固定 `LUN_NUM`，而是按 target 失效处理；随后对 `logical_units` 个 LUN 逐个发 `READ STATUS ENHANCED (78h)`，读到 0xFF 视为无响应。失败场景：① ONFI 参数页偶发读失败 ⇒ 原本可启动的 target 让 `nand_scan_targets` 报错、`nand_init` 失败；② 78h 时序 / 状态语义错误 ⇒ LUN0 被判 absent，同样拒绝启动；③ 当前器件 `logical_units=1`，多 LUN 分支未上板。
- **缓解**：ONFI 是标准命令且 ID 匹配后才读；LUN0 必须 present；多 LUN 循环按运行时 `logical_units` 走，不需改代码；扫描仍在 armed 前、Mode 0。
- **下一步**：上板必看 `[NAND] lun=0 status=... present=1` 与 `scan ... present luns=1`；若 ONFI 偶发失败，补有界重试而不是回退固定值；多 LUN 需换器件验证 `lun_present[]`。
- **证据**：`src/nand/devices/mt29f___g08a.c` `nand_target_probe` / `nand_fc_read_status_enhanced` / `nand_select_target`；`include/nand/nand_config.h` `nand_target_info_s`。
- **历史/验证**：
  - 2026-11-03 LUN 数改器件属性 + 逐 LUN 78h 探测 → 未上板

---

## 2AC. CLI 调试前端（`controller/cli.c`）

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-CLI-01 | 待上板 | 长命令内联执行阻塞主循环，IWDG 启用后会复位 | 未上板 |
| RISK-CLI-02 | 待上板 | format/bbt erase 不检查 host 在飞，会造成介质状态分叉 | 未上板 |
| RISK-CLI-03 | 待上板 | wp 复用静态缓冲无在飞守卫，重入会写错字节 | 未上板 |
| RISK-CLI-04 | 待上板 | release help 宣传被禁用的 bbt 子命令 | 未上板 |
| RISK-CLI-05 | 待上板 | rd/bbt 语法变更，旧脚本会报 unknown option | 未上板 |
| RISK-CLI-06 | 待上板 | q/cp/jrn/stat/id/ecc/wear 字段口径未板端核对 | 未上板 |
| RISK-CLI-07 | 待上板 | rd 失败仍打印 rd ok 并 dump 旧缓冲 | 未上板 |
| RISK-CLI-08 | 待上板 | wp 绕过 host gate，可与 USB/CLI 并发进入 FTL | 未上板 |

### 详情

#### RISK-CLI-01 长命令阻塞 superloop 导致看门狗/USB 饥饿

- **后果**：CLI handler 在主循环内联执行；`format --verify`（数分钟）、`bbt erase`（十几秒）、`test`（61 s）期间 `timer_scheduler_poll()` 不转 ⇒ 看门狗任务不跑。看门狗要求 scheduler 域每 100 ms 上报进展（`domain_timeout_ms[SCHEDULER]=100`）；IWDG 启用后 >100 ms 的命令会 latch 超时故障并停止喂狗 ⇒ 约 30 s 后复位（破坏性命令做到一半被复位）。`SSD_WATCHDOG_ENABLE` 当前恒为 0（所有构建）。
- **缓解**：`storage_wdg_kick()`（≤10 ms 节流，先上报 scheduler 域再 `service()`）挂在 `ftl_sync_submit_wait` 自旋泵 —— format / bbt erase / 出厂扫描的每块擦/读都经此同步原语，覆盖最长路径。IWDG 仍未启用（开关保持 0）；`test`（纯 RAM、不走同步泵）仍需 job 化或在测试期间禁用 IWDG。USB 在长命令期间仍得不到服务（调试前端预期代价）。
- **下一步**：① release + `SSD_WATCHDOG_ENABLE=1`，跑 `format --verify` / `bbt erase` 不被复位；② 满盘 GC 压力 + H2testw 跑过 30 s 不误复位；③ 决定 `test` 路径（禁用 IWDG 或 job 化）；以上验证通过前不得把开关改成 1。
- **证据**：`src/platform/watchdog.c:storage_wdg_kick`；`src/ftl/ftl_fc_interface.c:ftl_sync_submit_wait`；`src/main.c:56` `SSD_WATCHDOG_ENABLE` / `g_storage_wdg_config`；`src/hil/cli.c:uart_cli_format` / `uart_cli_bbt` / `uart_cli_test`。
- **历史/验证**：
  - 2026-11-05 加 `storage_wdg_kick` 长操作喂狗点 → 未上板

#### RISK-CLI-02 破坏性命令不检查 host 在飞且在量产档注册

- **后果**：`format` / `bbt erase` 会擦掉元数据；USB 在飞请求的完成会在 format 的同步泵里被派发，落到正在擦/重写的介质上（L2P/journal 记账与介质状态分叉）。
- **缓解**：策略收口到命令表：`UART_CLI_FLAG_BUSY`（wr/rd/tr/fl/rdpaa/format）由 dispatcher 单点拒绝 host 冲突；`format` 整行由 `UART_CLI_DESTRUCTIVE_ENABLE` 编译期隐藏；release 档 `bbt erase` / `bbt fault` 统一返回 `subcommand '...' disabled in this build`，`help` 的 bbt 语法只列 `bbt`。`wp` 保留专用门（按设计绕过 pending gate，另查 `g_wp_inflight`）；`bbt erase` 的子命令级 busy 检查留在 handler（命令级 flag 会连只读 bbt 一起拒）。
- **下一步**：USB 写入过程中执行 `format` / `bbt erase`，应被拒绝而不是形成半格式化状态；release 档 `help` 不出现 destructive 节、`bbt` 语法只显示 `bbt`。
- **证据**：`src/hil/cli.c:UART_CLI_FLAG_BUSY` / `uart_execute_command` / `UART_CLI_DESTRUCTIVE_ENABLE` / `g_uart_cli_commands` / `uart_cli_bbt`；`src/hil/usb_msc.c:usb_msc_host_busy`。
- **历史/验证**：
  - 2026-11-05 命令表收口 BUSY/destructive 门 → 未上板

#### RISK-CLI-03 wp 复用静态缓冲无在飞守卫

- **后果**：`wp` 按设计绕过 `g_cli_pending.busy`，提交 k 个组，`req.buffer` 指向 `g_wp_buf`；`wb_append` 立即 memcpy 进 wbuf 窗口，但背压时未下发的 run 仍留在组里（`g->completion.buffer` 指向 `g_wp_buf` 切片），完成前才消费。再次 `wp`（或部分提交失败后重试）会重写 `g_wp_buf` / `g_wp_done` / `g_wp_status`，旧组未下发的 run 会写到新字节、完成标志被复用。
- **缓解**：`g_wp_inflight` 在飞计数：每组提交前 +1，`req.on_complete` 回调递减；非 0 时拒绝新 `wp`。提交失败时回滚本组 +1（已提交组仍各自递减）。
- **下一步**：连续两次 `wp` 应被拒绝；单次 `wp` 后 `rd` 回读仍是各组自己的 0xA0+i。
- **证据**：`src/hil/cli.c:uart_cli_wp` / `uart_cli_wp_complete` / `g_wp_inflight` / `g_wp_buf` / `g_wp_done`；`src/ftl/wbuf.c:wb_append`。
- **历史/验证**：
  - 2026-11-05 加 `g_wp_inflight` 在飞守卫 → 未上板

#### RISK-CLI-04 release 档 help 宣传不可用子命令

- **后果**：`help` 若无条件给出 `bbt [--erase | fault <plane> <block>]`，而 `--erase` 在 NDEBUG 档被拒、`fault` 整段被 `#ifndef NDEBUG` 排除 ⇒ 操作者按 `help` 操作得到相反结果（无数据损失）。`help` 若用固定列宽 `%-38s` 硬对齐，45~160 字符的 usage 会折行。
- **缓解**：`UART_CLI_SYNTAX_BBT` 随构建档切换（release 只列 `bbt`）；release 下 `bbt erase`/`fault` 统一返回 disabled 文本；`help` 改为「分组一行 + `help <cmd>` 两行式」（usage + 说明），destructive 组在 release 为空、自动不打印。
- **下一步**：release 串口 `help` 无 destructive 行、`help bbt` 只显示 `bbt`；debug 档核对完整语法与分组。
- **证据**：`src/hil/cli.c:UART_CLI_SYNTAX_BBT` / `uart_cli_help` / `uart_cli_help_group` / `uart_cli_bbt`。
- **历史/验证**：
  - 2026-11-05 语法按构建档切换 + help 改版 → 未上板

#### RISK-CLI-05 用户可见语法变更

- **后果**：`rd <lba> <n> c` → `rd <lba> <n> --compare`；`bbt --erase` → `bbt erase`（`bbt fault` 不变）。板端手工脚本若仍用旧写法会收到 unknown option/subcommand。判据：动作/模式用裸子命令、修饰项用 `--全词`。
- **缓解**：旧写法明确报错，不会静默降级为只读/hex dump；`help` 显示新语法。
- **下一步**：上板时按新语法逐条点一遍；旧脚本同步改名。
- **证据**：`src/hil/cli.c:uart_cli_rd` / `uart_cli_bbt` / `g_uart_cli_commands`。
- **历史/验证**：
  - 2026-11-05 语法规范化 → 未上板

#### RISK-CLI-06 观测命令字段口径未经板端核对

- **后果**：`q`/`cp`/`jrn`/`stat`/`id`/`ecc`/`wear` 直接读 FC/FTL 内部计数（`fc_resource_stats_s`、`pend_*`、`cp_state_s`、`jrn_state_s`、`bst_block_s.rr`）。若字段语义读错（如把 `persisted_seq` 当当前序号、把 `wb busy` 读反、把 `l2p_load` 当 flush、把 `last_ecc` 当终身值），会让排障结论指向错误模块（无介质副作用）。`stat` 另把原本在 `info` 里的计数行搬走，习惯看 `info` 的人会以为计数消失；`wear` 的 WL 差值在 `writes < 1000`（`WL_MIN_WRITES` 启动门）时 `valid=0` 是预期。
- **缓解**：只读命令，无 host-busy 门（主循环单线程）；复用 `fc_get_resource_stats` / `pend_*_count` / `jrn_*` accessor / `rr_get_health` / `wl_ec_spread_get`，字段名即语义；`info` 保留身份/状态，计数在 `stat`。
- **下一步**：① `q` 的 sq/hsq/cq/三池与 `[FTL] stall` 日志里的同一组值对得上；② `cp`/`jrn` 在 idle、写压力、CP 进行中各看一次；③ `stat` 的 `up` 随串口时间增长，`d4/wd/cmt` 与 `wp`/`rd` 操作次数对得上；④ `ecc` 的 health 计数与 `rr_predictive_scan` 触发一致、`read_blocks` 随读增长；⑤ `wear` 的 EC 直方图与 `info` 的 free/bad 一致，WL 触发前后 `max_free−min_used` 与 `[WL ]` 日志一致；⑥ `id` 与 boot 横幅/`info` 一致。
- **证据**：`src/hil/cli.c:uart_cli_q` / `uart_cli_cp` / `uart_cli_jrn` / `uart_cli_stat` / `uart_cli_id` / `uart_cli_ecc` / `uart_cli_wear`；`src/ftl/wear_leveling.c:wl_ec_spread_get`；`src/fc/fc_interface.c:fc_get_resource_stats`；`include/ftl/checkpoint.h:cp_state_s`；`include/ftl/journal.h:jrn_state_s`；`include/ftl/bst.h:rr_ecc_track_s`。
- **历史/验证**：
  - 2026-11-05 观测命令批次落地 → 未上板

#### RISK-CLI-07 rd 失败仍打印 rd ok

- **后果**：`uart_cli_pending_poll()` 对 READ 无条件调用 `uart_cli_rd_report()`；若不检查 `request->completion_status`，读失败（FTL IO_ERROR/ECC/范围）时会把 `g_wr_buf` 的旧内容当作本次读回做 hex dump/compare，并固定打印 `rd ok n=...`。
- **缓解**：`uart_cli_rd_report(status)` 先判 `completion_status`，非 OK 打印 `rd FAIL status=...` 并跳过 dump/compare。
- **下一步**：构造读失败（未映射 PBA / 坏块）确认串口输出 `rd FAIL` 且不再打印 `rd ok`；`/hil/` 不覆盖 CLI 层，需 CLI 脚本。
- **证据**：`src/hil/cli.c:2002-2037:uart_cli_pending_poll`；`src/hil/cli.c:1967-1997:uart_cli_rd_report`。
- **历史/验证**：
  - 2026-11-05 按完成状态分流输出 → 未上板

#### RISK-CLI-08 wp 绕过 host gate 且不被 gate 看见

- **后果**：`uart_cli_wp()` 只检查 `usb_msc_host_busy()` 与 `g_wp_inflight`，不检查 `g_cli_pending.busy`，并直接 `ftl_submit_write()` 不占 host queue；反向 `cli_host_busy()` 也不覆盖 wp ⇒ wp 可与在飞的 CLI `wr/rd/tr/fl` 并发，USB 也可在 wp 在飞时提交 controller。
- **缓解**：`cli_host_busy()` 纳入 `g_wp_inflight`；`wp` / dispatcher / `bbt erase` 统一用 `cli_host_busy() || usb_msc_host_busy()`；USB 在 wp 在飞时让路。
- **下一步**：wp 在飞时发 USB 读写，确认 USB 返回 BUSY/排队而不是并发进入 FTL；长期按 RISK-HOST-18 收敛到单一 claim。
- **证据**：`src/hil/cli.c:527-617:uart_cli_wp`；`src/hil/cli.c:223-226:cli_host_busy`。
- **历史/验证**：
  - 2026-11-05 统一 host busy 判据 → 未上板

---
## 2AD. Controller 请求拆分（`request.c` / `controller.c`）

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-CTRL-01 | 待上板 | 多 run 请求中途失败时父请求提前完成、错误被覆盖 | 未上板 |
| RISK-CTRL-02 | 待上板 | HIL 入口未校验 LBA 范围，越界交给 FTL | 未上板 |
| RISK-CTRL-03 | 待决策 | `ssd_controller_init` 稀疏拓扑注释与实现相反 | 未上板 |
| RISK-CTRL-04 | 待后续 | HIL submit 未校验 buffer NULL，FTL 兜底但状态分类错误 | 未上板 |

### 详情

#### RISK-CTRL-01 中途失败提前完成与错误覆盖

- **后果**：多 run 请求中途提交失败时父请求被提前置完成 + 错误可被后续成功覆盖：`ssd_controller_submit` 在 `ssd_request_split` 返回错误时无条件 `complete_flag=1` ⇒ 调用者（USB slot / CLI）可立即释放 / 复用 bounce 或请求对象，而在飞 run 仍持有它们（UAF / 数据踩踏）；`on_sub_complete` 只递减计数、不聚合状态 ⇒ 后续成功 run 会把共享 `completion_status` 写回 OK，掩盖中途错误；split 失败路径把 `sub_remaining` 重置成 `submitted`（含已同步完成的 run）⇒ 可能永久等待。
- **缓解**：① `on_sub_complete` 用独立 `agg_status` 锁存首个非 OK，归零时把它写回 `completion_status`；② split 失败时扣减「未提交 run 数」（`total_runs - submitted`），不覆盖计数；③ `ssd_controller_submit` 在 `sub_remaining != 0` 时返回 `SSD_STATUS_OK`（请求已受理，错误由异步完成状态承载），仅零在飞时同步失败。
- **下一步**：多 die 或注入 `ftl_submit` 中途失败，确认请求不提前完成、bounce 不被释放、最终状态为错误；单 die 下跑现有读写回归。
- **证据**：`src/hil/request.c` `on_sub_complete` / `ssd_request_split`；`src/hil/controller.c` `ssd_controller_submit`；`include/hil/ssd_request.h` `agg_status`。
- **历史/验证**：
  - 2026-11-05 聚合状态 + 失败计数扣减 + 受理语义修正 → 未上板

#### RISK-CTRL-02 HIL 入口缺 LBA 范围校验

- **后果**：`ssd_controller_submit` 对 READ / WRITE 只校验 `buffer_size`，范围校验依赖 USB / CLI 前端；直接调用 controller 的测试或新前端可把运行时越界 LBA 交给 FTL（FTL 只按编译期上限检查，不知道运行时容量）。
- **缓解**：submit 中对 READ / WRITE 先调 `ssd_controller_lba_range_ok`，越界返回 `INVALID_ARGUMENT` 并置 complete；`test_hil submit_edges` 覆盖越界。
- **下一步**：上板跑 `/hil/`；越界请求返回 INVALID。
- **证据**：`src/hil/controller.c` `ssd_controller_submit`；`tests/unit/test_hil.c` `test_hil_submit_edges`。
- **历史/验证**：
  - 2026-11-05 入口增加范围校验 → 未上板

#### RISK-CTRL-03 稀疏拓扑注释与实现相反

- **后果**：`.h` 说「稀疏拓扑的空洞不访问」、`.c` 说「稀疏拓扑允许部分 die 槽为 NULL」，实际对任一 `slot == NULL` 直接转 FAILED 并返回 `INVALID_ARGUMENT` ⇒ 当前实现要求全部 `DIE_MAX` 个 die 都有实例。
- **缓解**：当前 `main.c` 总是提供全部实例，无运行时影响。
- **下一步**：二选一：把注释改为「当前不支持稀疏拓扑，任一槽缺失即 FAILED」；或实现稀疏语义（路由跳过空洞、缺失 die 的 LBA 拒绝）。按 §〇 先定主流做法再改代码。
- **证据**：`include/hil/controller.h:52-54`；`src/hil/controller.c:121-142`；`ssd_controller_flush_service` 的 `ASSERT(fftl != NULL)`。
- **历史/验证**：
  - 2026-11-05 审视判定为注释 / 实现分叉，未改 → 待决策

#### RISK-CTRL-04 HIL submit 未校验 buffer NULL

- **后果**：`ssd_controller_submit` 的 READ/WRITE 分支只校验 `buffer_size`，不校验 `request->buffer == NULL`；NULL buffer 会进入 `ssd_request_split`，`request->io.buffer = (BYTE *)NULL + offset`。单 run（offset=0）通常仍为 NULL，由 `ftl_submit_common` 的 `req->buffer == NULL` 兜底，但 controller 会返回 `SSD_STATUS_NO_SPACE` / `IO_ERROR` 而不是 `INVALID_ARGUMENT`；多 run（offset>0）会得到非 NULL 小地址，FTL 不再拦截，存在踩踏风险。当前单 die 下多 run 不可达。
- **缓解**：FTL 入口 `ftl_submit_common` 有 NULL 兜底；HIL `submit_edges` 已覆盖 buffer_size 不足，未覆盖 NULL。
- **下一步**：在 controller READ/WRITE 入口加 `request->buffer == NULL` → `INVALID_ARGUMENT` + `complete_flag=1`；`test_hil_submit_edges` 补 NULL buffer 负例；上板判据 = 返回 `INVALID_ARGUMENT` 且不进入 FTL。
- **证据**：`src/hil/controller.c` `ssd_controller_submit`；`src/hil/request.c:184`；`src/ftl/ftl.c` `ftl_submit_common`；`docs/design/hil_ut_design.md` §6。
- **历史/验证**：
  - 2026-11-13 设计文档补记：HIL UT 设计见 `docs/design/hil_ut_design.md`；本轮不改代码
  - 2026-11-13 用户裁决：留以后检验 → 本轮不修
  - 2026-11-13 读码判定 → 未上板

---

## 2AE. HIL 主机前端 / host queue

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-HOST-20 | 待决策 | HIL host 前端互斥不是单一闸门，同一不变量 5 处手工维护 | 未上板 |
| RISK-HOST-21 | 待后续 | HIL 没有 host 命令超时 / 中止 | 未上板 |

### 详情

#### RISK-HOST-20 host 前端互斥不是单一闸门

- **后果**：dispatcher 检查 `g_cli_pending.busy` 与 `usb_msc_host_busy()`；`bbt erase` 再查一遍；`wp` 只查 USB；USB 的 transfer / unmap / flush 各自查 `cli_host_busy()`，而 `cli_host_busy()` 不覆盖 `wp` / 直连 FTL ⇒ 同一不变量由 5 处手工维护（§3.1 / C3）。
- **缓解**：当前 BOT 串行且各路径自检；正常路径未见并发。
- **下一步**：在 `ssd_host_queue` / controller 增加 claim / release（owner + 唯一 host gate），所有前端提交前 claim；直连 FTL 的 `wp` 也纳入 claim 或按 P5 显式登记例外；删除各 handler 的重复检查。
- **证据**：`src/hil/cli.c:2449` / `:1252` / `:542`；`src/hil/usb_msc.c:827/908/1302`；`include/hil/host_queue.h`。
- **历史/验证**：
  - 2026-11-05 读码判定 → 待决策

#### RISK-HOST-21 缺 host 命令超时 / 中止

- **后果**：`ssd_controller_submit()` 受理后只等 `complete_flag`；USB IN_FLIGHT 槽与 CLI pending 均无 deadline。FTL 内部有 CP / confirm / journal 超时，但若某个组 / TRIM job 永久不完成，HIL 不会回收槽 / host entry，也不会给主机失败 CSW。
- **缓解**：discard 路径也要等 FTL 完成；正常路径由 FTL 各 job 自带超时兜底。
- **下一步**：评估在 `ssd_controller_service()` 加 per-request deadline（超时置 IO_ERROR 并 cancel flush / trim），或在 `usb_msc_task()` 对 IN_FLIGHT 超时回 CSW；先定义超时后 FTL 在飞资源如何收尾（需与 FTL 协同）。
- **证据**：`src/hil/controller.c` `ssd_controller_submit` / `ssd_controller_service`；`src/hil/usb_msc.c` `usb_msc_task`。
- **历史/验证**：
  - 2026-11-05 读码判定 → 待后续

---
## 2AF. FC 资源池 / 生命周期

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-POOL-01 | 待上板 | buffer 归属表缺失，双释放 / 在飞释放无运行期判据 | 未上板 |
| RISK-POOL-02 | 信息 | 命令 release 不校验 CQ / 在飞，依赖 FTL 调用链 | 未上板 |
| RISK-POOL-03 | 待上板 | desc 链释放 / 遍历缺上界与环检测 | 未上板 |
| RISK-POOL-04 | 待上板 | 空闲链哨兵混用裸常量与具名 ID | 未上板 |
| RISK-POOL-05 | 待上板 | 资源统计靠三池全扫，计数可能漂移 | 未上板 |
| RISK-POOL-06 | 待上板 | 单字段双语义，失败坐标字段无读者 | 未上板 |
| RISK-POOL-07 | 待上板 | 内部 API 后缀 / 命名不合 H49 | 未上板 |
| RISK-POOL-08 | 待上板 | FC 注释承载变更史与版本号 | 未上板 |
| RISK-POOL-11 | 待上板 | 链校验把无 desc 的 multi-plane 擦除判非法 | 2026-11-11 上板复现 |

### 详情

#### RISK-POOL-01 buffer 归属表缺失

- **后果**：buffer 没有归属表时，双释放 / 在飞释放只能靠调用者契约；同一节点二次头插会让空闲链成环。
- **缓解**：`fc_core_buffer_release` 要求传入期望 owner，owner 不符或未占用即拒绝；`buffer_owner[]` 在 alloc 时置 `FC_COMMAND_ID_INVALID`、建链时改命令 ID；释放闸门同时校验下标 / 状态 / owner；公开 `fc_buffer_release` 恢复 `void`，只允许释放未挂链 buffer；内部返回 bool 作为判据 9 闸门。
- **下一步**：上板阴性用例：双释放、命令在飞时释放、上层释放已挂链 buffer 均被拒；UT 同步。
- **证据**：`src/fc/fc.c` `fc_core_buffer_alloc` / `fc_core_buffer_release`；`src/fc/fc_interface.c` `fc_buffer_release`。
- **历史/验证**：
  - 2026-11-06 增加 `buffer_owner[]` 与带 owner 的释放闸门 → 未上板

#### RISK-POOL-02 命令 release 依赖调用链

- **后果**：`fc_core_command_release` 只 ASSERT `ALLOCATED` / `COMPLETED`，release 档不能识别「CQ 未取走」或「仍在飞」；正确性由 FTL 调用链保证（`ftl_sync_wait_owned` 未提交才 release、`ftl_op_drain` 处理迟到完成，RISK-IF-17）。
- **缓解**：公开 `fc_command_release` 恢复 `void`；指针身份 ASSERT；`FREE` 幂等早退；FC 不再引入可失败 release API。
- **下一步**：若 FTL 新增第二个释放入口，重新评估是否把 CQ / 状态守卫作为判据 9 显式例外；上板注入在飞 release，验证 ASSERT 停机 + 调用链回归。
- **证据**：`src/fc/fc.c` `fc_core_command_release`；`src/fc/fc_interface.c` `fc_command_release`；`src/ftl/ftl_fc_interface.c` `ftl_sync_wait_owned`；`src/ftl/ftl_op.c` `ftl_op_drain`。
- **历史/验证**：
  - 2026-11-06 判定为「依赖调用链的有意设计」，不改 release API → 未上板

#### RISK-POOL-03 desc 链遍历缺上界与环检测

- **后果**：释放、buffer 回收、append、完成回填四条链式遍历无上界与环检测时，成环会死循环 ⇒ 看门狗复位，或顺链释放别人的 desc。
- **缓解**：四条遍历改为有界 for/while + 出口 `ASSERT`；超界时 release 档截断并置链头 `FC_DESCRIPTOR_ID_INVALID`，不再逐点 `ASSERT(false)+LOGE`。
- **下一步**：上板 / 负例构造成环链，确认超界只截断并留日志，不继续写。
- **证据**：`src/fc/fc.c` `fc_core_chain_release_all` / `fc_core_command_release_buffers` / `fc_command_chain_append` / `fc_complete_from_nand`。
- **历史/验证**：
  - 2026-11-06 四条遍历加上池深上界 → 未上板

#### RISK-POOL-04 空闲链哨兵混用

- **后果**：空闲链哨兵混用裸 `UINT16_MAX` 与具名 ID 哨兵时，池深或哨兵一旦调整会出现编译期无声的不一致。
- **缓解**：descriptor / buffer 空闲链终止值与空池判断统一用 `FC_DESCRIPTOR_ID_INVALID` / `FC_BUFFER_ID_INVALID`，共 6 处；注释同步为宏名，不再写死 63 / 255 / 127。
- **下一步**：上板回归分配 / 释放；`grep UINT16_MAX` 于 FC 池路径应为 0。
- **证据**：`src/fc/fc.c` 三池初始化与 alloc。
- **历史/验证**：
  - 2026-11-06 哨兵改为具名 ID（H4 / C9）→ 未上板

#### RISK-POOL-05 资源统计扫描三池

- **后果**：`used` 计数若由扫描派生，扫描窗口内的分配 / 释放会让 `q` 与真值不一致；三池全扫（448 次迭代）的开销也落在统计路径上。
- **缓解**：`fc_get_resource_stats` 改读 O(1) 计数；`command_used` / `descriptor_used` / `buffer_used` 只在分配 / 释放闸门增减，另维护 high-water；debug 档保留一次全池扫描并 `ASSERT` 计数与真值一致（证明不漂移），release 档只读计数器。
- **下一步**：上板跑 `q` 核对 used / hwm；UT 同步新增字段。
- **证据**：`src/fc/internal/fc_internal.h` `fc_ctl_s`；`src/fc/fc.c` 三池闸门 / `fc_get_resource_stats`；`src/hil/cli.c` `q`。
- **历史/验证**：
  - 2026-11-06 统计改 O(1) 计数 + debug 差分断言 → 未上板

#### RISK-POOL-06 字段双语义与无读者的失败坐标

- **后果**：一个 32-bit 字段承载双语义会让命令与 descriptor 的计数 / 索引互相污染；CQ 的 `failed_cw` / `failed_desc_id` 无读者，失败 CW 实际由 desc 链的 per-CW `status` 派生。
- **缓解**：`fc_cmd_cfg_s.cw` 拆为命令专用 `cw_count` + descriptor 专用 `cw_index`；删除 `failed_cw` / `failed_desc_id`；NAND completion 同步删除 `failed_cw`；CQ 只保留 `cmd_id/status/param`。
- **下一步**：UT 同步字段名；上板覆盖多 CW 失败定位（从 desc 链派生）。
- **证据**：`include/fc/fc_command.h`；`include/fc/fc_nand_dispatch.h`；`src/fc/fc.c` `fc_complete_from_nand`。
- **历史/验证**：
  - 2026-11-07 删除 NAND completion 的 `failed_cw`，CQ 失败坐标字段移除 → 未上板
  - 2026-11-06 拆分 `cw` 为 `cw_count` / `cw_index` → 未上板

#### RISK-POOL-07 内部 API 命名不合 H49

- **后果**：`fc_*_internal` 后缀与 H49 冲突，`real_buffer` 名称含糊，内部 API 命名不一致。
- **缓解**：20 个内部函数统一改为 `fc_core_*`；`fc_command_release_internal_api` → `fc_core_command_release`；`fc_buffer_id_to_real_buffer_internal` → `fc_core_buffer_cw_base`；公开 `fc_buffer_id_to_real_buffer` → `fc_buffer_get_user_data`；`fc_internal.h` 命名约定同步为 `fc_core_*(controller, ...)`。
- **下一步**：UT / 文档同步旧名；grep 旧符号应为 0（测试源除外）。
- **证据**：`src/fc/internal/fc_internal.h`；`src/fc/fc.c`；`src/ftl/ftl_fc_interface.c`。
- **历史/验证**：
  - 2026-11-06 内部函数统一 `fc_core_*`（H49）→ 未上板

#### RISK-POOL-08 注释承载变更史

- **后果**：FC 注释写版本号 / 旧模型对照会误导读者把历史实现当现状。
- **缓解**：注释中的旧模型对照、版本号与撤销式表述统一改写为当前事实 + why；S6 版本号机械检查只剩 GPL v2 例外。
- **下一步**：review 时用 S6 正则复查；新注释不得再写变更史。
- **证据**：`src/fc/fc.c` / `fc_nand_dispatch.[ch]` / `onfi_1_0.c` / `fc_data_path*.c`。
- **历史/验证**：
  - 2026-11-06 注释收口（H37 / S6）→ 未上板

#### RISK-POOL-11 链校验误拒无 desc 的 multi-plane 擦除

- **后果**：`fc_pool_chain_validate` 对 `config.multi_plane > 0` 无条件要求 `planes_with_desc == all_planes`，但 multi-plane 擦除 / 纯加载按契约不建 desc 链（`ftl_cmd_needs_desc_chain` 为 false，dispatch 从 `cmd->paa` 逐 plane 派生行地址）⇒ `planes_with_desc=0` 必然判 false。debug 档 `ASSERT: submit: chain invalid` 停机；release 档返回 `FC_STATUS_INVALID_ARGUMENT` ⇒ 所有 multi-plane 擦除经 `fc_submit` 失败（GC / migrate / `ftl_sync_page_erase`），功能不可用但非静默写坏。
- **缓解**：覆盖校验改为只对 `nand_operation_has_data(op)` 的 multi-plane 数据命令生效；无数据命令仍走通用逐 desc 坐标 / 归属 / 行一致检查，平面跨度由 `fc_command_plane_span` 与 `cmd->paa` 校验。
- **下一步**：上板跑 `/fc-nand/lifecycle/fail_cw_attribution`（② 无 desc 擦除）+ `/fc-nand/lifecycle/block_multi_plane` + FTL 擦除回归；负例：multi-plane **数据**命令缺 plane desc 仍须拒绝。
- **证据**：`src/fc/fc_pool.c` `fc_pool_chain_validate`；`src/fc/fc.c` `fc_build_dispatch_request` 的 `OP_ERASE_BLOCK_MULTI_PLANE` 分支；`src/ftl/ftl_fc_interface.c` `ftl_sync_page_erase`；`tests/unit/test_fc_level3.c` `test_fail_cw_attribution` ②。
- **历史/验证**：
  - 2026-11-11 覆盖校验按数据 / 无数据命令分流 → 未上板
  - 2026-11-11 板端 `fail_cw_attribution` ② 首次触发 `submit: chain invalid` → 上板复现

### 已闭合（详情见 archive）

| ID | 日期 | 一句话 | 归档 |
|---|---|---|---|
| RISK-POOL-10 | 2026-11-07 | `fc_command_to_id` 池上界误用指针宽度，接口已删除 | `docs/archive/known_risks_batches.md` |

**已判定为有意设计（重访时先读契约）**：

- `fc_core_chain_release_head` 对 `*head >= FC_DESCRIPTOR_POOL_DEPTH` 早退是空链哨兵语义，改成 `ASSERT` 会让合法空链释放炸掉（`src/fc/fc.c`）。
- `fc_core_command_release` 对 `FREE` 早退是幂等释放语义，防止重复释放顺着 desc 0 释放别人的链；与 RISK-POOL-02 的「在飞释放」是两件事，不能互相替代（`src/fc/fc.c`）。
- `fc_debug_state_get` 只采样 `chain_owner[0][0][0]` / LUN0 是诊断采样口径，不是全维遍历；要覆盖全 LUN 时另加循环字段，不改采样语义（`src/fc/fc_interface.c`）。

---

## 2AG. 数据面 / NAND dispatch

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-DP-01 | 待上板 | ECC 引擎不可用被归一为成功，写不编码、读不校验 | 未上板 |
| RISK-DP-02 | 待上板 | 数据路径编码错误被调用方丢弃，ECC 失败仍提交 NAND | 未上板 |
| RISK-DP-03 | 待上板 | INVALID buffer 进入数据路径，release 下越界/空指针 | 未上板 |
| RISK-DP-04 | 待上板 | desc 链遍历无上界，成环会死循环 | 未上板 |
| RISK-DP-05 | 待上板 | RAID 边界不完整，恢复后丢 CRC/META | 未上板 |
| RISK-DP-06 | 信息 | multi-plane uop 硬编码双 plane，维度变大静默丢 plane | 2026-11-06 代码核对 |
| RISK-DP-07 | 待上板 | 链式失败生命周期半状态，整链完成时断言 ACTIVE | 未上板 |
| RISK-DP-08 | 待上板 | 通道轮询注释称 round-robin，实际每轮从 CH0 开始 | 未上板 |
| RISK-DP-09 | 待上板 | FC 内 engine↔dispatch 双向依赖，armed 闩锁泄漏进引擎 | 未上板 |
| RISK-DP-10 | 待上板 | dispatch enqueue 入口检查只有 debug ASSERT | 未上板 |
| RISK-DP-11 | 待上板 | dispatch 忽略 µs 时钟失败，LUN 永久停在 ISSUED | 未上板 |
| RISK-DP-12 | 待上板 | 单 plane cache 读失败只标最低位 CW | 未上板 |
| RISK-DP-13 | 待上板 | ecc_bit_count 无生产者，恒为 0 | 未上板 |
| RISK-DP-14 | 待上板 | ECC 枚举 0 命名为 BCH，运行时模式与实现不一致 | 未上板 |
| RISK-DP-15 | 待后续 | LDPC 分支引用不存在的 g_qc，切构建即编译失败 | 2026-11-07 代码核对 |
| RISK-DP-16 | 待上板 | dispatch poll 用 BYTE 返完成数，256 回绕为 0 | 未上板 |
| RISK-DP-17 | 待上板 | JUMP_IF_NOT_READY 回跳减法无 release 下溢守卫 | 未上板 |
| RISK-DP-18 | 待上板 | random read UOP 缺 CHECK_FAIL，中间 CW 失败被吞 | 未上板 |
| RISK-DP-19 | 待上板 | 单 plane cache 读 UOP 缺 CHECK_FAIL | 未上板 |
| RISK-DP-20 | 待上板 | cache 编程首页 FAILC 误报，误判好页失败 | 未上板 |
| RISK-DP-21 | 待上板 | 参数页/唯一 ID/GET_FEATURES 的 FAIL 被当成成功 | 未上板 |
| RISK-DP-22 | 待上板 | WAIT_READY 后端阻塞，引擎协作让出路径未生效 | 未上板 |
| RISK-DP-23 | 待上板 | fc_nand_start 依赖调用者预绑定 engine 指针，契约未声明 | 未上板 |
| RISK-DP-24 | 待上板 | 高速模式失败后没有回滚到低速 | 未上板 |
| RISK-DP-25 | 待上板 | dispatch 不校验 req/completion 表项非空，空指针崩溃 | 未上板 |
| RISK-DP-26 | 待上板 | 带 DATA_OUT 的稀疏 CW 读首输出读错 CW | 2026-11-07 编译通过 |
| RISK-DP-27 | 待后续 | LDPC 解码栈占用 4.2KB，超 2KiB 栈预算 | 2026-11-07 代码核对 |
| RISK-DP-28 | 待后续 | CRC 初始化失败在 release 下静默，校验恒通过 | 2026-11-07 代码核对 |

### 详情

#### RISK-DP-01 ECC 引擎不可用被当成成功

- **后果**：`fc_data_path_ecc_encode` 把 `FC_STATUS_NOT_SUPPORTED` 归一为 `OK`，读路径也把它当「成功且零纠正」⇒ BCH/LDPC 初始化失败时写不编码、读不校验，完全静默（C8/§3.2）。
- **缓解**：`fc_data_path_ecc_encode/decode` 原样上抛 `NOT_SUPPORTED`；写路径由 `fc_poll_dispatch_*` 判失败，读路径置 UNC/命令 DATA_ERROR；`fc_ecc_init_bch` 失败只 LOG 一次。
- **下一步**：上板注入 ECC 初始化失败：写命令必须失败、读命令必须报错；不能出现「成功写入」。
- **证据**：`src/fc/fc_data_path.c:fc_data_path_ecc_encode/decode` / `fc_data_path_process_read`；`src/fc/fc_ecc_engine.c:fc_ecc_init_bch`。
- **历史/验证**：
  - 2026-11-06 修复：`NOT_SUPPORTED` 原样上抛，读写路径均判失败 → 未上板

#### RISK-DP-02 数据路径编码错误被调用方丢弃

- **后果**：`fc_poll_dispatch_single/chain` 用 `(void)fc_data_path_encode()`，ECC 失败仍提交 NAND。
- **缓解**：单/链路径都检查 encode 返回值；失败走 `fc_poll_fail_command`，不提交；整链先置 ACTIVE，失败时整链了结。
- **下一步**：上板注入 encode 失败；确认命令失败、无半编码数据写入。
- **证据**：`src/fc/fc.c:fc_poll_dispatch_single / fc_poll_dispatch_chain`。
- **历史/验证**：
  - 2026-11-06 修复：检查 encode 返回值，失败走 `fc_poll_fail_command` → 未上板

#### RISK-DP-03 INVALID buffer 进入数据路径

- **后果**：`fc_buffer_get_base(FC_BUFFER_ID_INVALID)` 在 release 会越界；dispatch 侧还可能 `memcpy(NULL)`。
- **缓解**：`fc_build_request_data_planes` 拒绝无 desc / INVALID buffer / 重复 CW；`fc_data_path_encode/process_read` 也显式拒绝并报失败；fail completion 跳过 data path。
- **下一步**：上板构造成 buffer 池耗尽后不释放直接 submit 的负例；确认命令失败且不崩溃。
- **证据**：`src/fc/fc.c:fc_build_request_data_planes`；`src/fc/fc_data_path.c:fc_data_path_encode / fc_data_path_process_read`；`src/fc/fc_nand_dispatch.c:task_load`。
- **历史/验证**：
  - 2026-11-06 修复：拒绝无 desc / INVALID buffer / 重复 CW → 未上板

#### RISK-DP-04 desc 链遍历无上界

- **后果**：build request / encode / RAID parity / FC 查询 helper 的 desc 链遍历只判 `desc_id != INVALID` 或 `< DEPTH`，成环会死循环。
- **缓解**：关键路径加 `steps < FC_DESCRIPTOR_POOL_DEPTH`；跑满仍未到链尾返回失败；FC 查询 helper 同样有界。
- **下一步**：上板/负例构造成环链；确认失败而非挂死。
- **证据**：`src/fc/fc.c:fc_build_request_data_planes`；`src/fc/fc_data_path.c` encode/parity；`src/fc/fc_interface.c` 查询 helper。
- **历史/验证**：
  - 2026-11-06 修复：关键路径链遍历加池深上界 → 未上板

#### RISK-DP-05 RAID 边界不完整

- **后果**：RAID 上下文 `end` 不校验 `processed==total`；recover 对 NULL/INVALID buffer 直接 memcpy；恢复后把所有 desc 清成 UNUSED，丢失 CRC/META。
- **缓解**：`fc_raid_end` 校验累加数；`fc_collect_decode_buffers` 遇 INVALID 返回 0；recover 检查 count/failed index；恢复后重新 CRC/META 检查。
- **下一步**：RAID 保持 `CONFIG_FC_RAID=0`（R5）；启用前必须编译+上板验证 `RAID` 恢复路径。
- **证据**：`src/fc/fc_data_path.c` RAID 段；`include/fc/fc_data_path.h` RAID 段。
- **历史/验证**：
  - 2026-11-06 修复：补 RAID 累加数 / 无效 buffer / CRC-META 检查 → 未上板（RAID 关闭）

#### RISK-DP-06 multi-plane uop 硬编码 row[0]/row[1]

- **后果**：`onfi_1_0.c` 的 multi-plane 序列只支持双 plane；`PLANE_NUM` 变大时 plane 2..N-1 会被静默忽略（H39）。
- **缓解**：`_Static_assert(PLANE_NUM == 2U, ...)`；`planes_with_desc` 另有 `PLANE_NUM <= 8` 约束。
- **下一步**：维护规则——换多 plane 器件时必须生成 uop 序列并改断言，不能只改 `PLANE_NUM`。
- **证据**：`src/nand/onfi_1_0.c` 顶部；`src/fc/fc.c:fc_build_request_data_planes`。
- **历史/验证**：
  - 2026-11-06 判定为静态约束：`_Static_assert(PLANE_NUM == 2U)` → 代码核对

#### RISK-DP-07 链式失败生命周期半状态

- **后果**：build 失败时只有前缀命令 ACTIVE，却对整链调 `fc_complete_from_nand`（断言 ACTIVE）。
- **缓解**：整链先统一置 ACTIVE，再 encode/build；任一失败整链走同一 fail 了结。
- **下一步**：上板注入链中第 1 条 build/encode 失败；确认无 ASSERT、无挂死、整链失败完成。
- **证据**：`src/fc/fc.c:fc_poll_dispatch_chain / fc_poll_fail_command`。
- **历史/验证**：
  - 2026-11-06 修复：整链先统一置 ACTIVE 再 encode/build → 未上板

#### RISK-DP-08 通道轮询注释与实现不一致

- **后果**：注释称从上次完成通道的下一个继续，实际每轮从 CH0 开始。
- **缓解**：`s_poll_next_channel` 实现通道 round-robin；init 清零。
- **下一步**：上板多通道构造两通道持续有完成，确认两者都获得推进。
- **证据**：`src/fc/fc_nand_dispatch.c:fc_nand_dispatch_poll`。
- **历史/验证**：
  - 2026-11-06 修复：`s_poll_next_channel` 实现 round-robin → 未上板

#### RISK-DP-09 FC 内 engine ↔ dispatch 双向依赖

- **后果**：`fc_nand_engine.c` include `fc_nand_dispatch.h` 仅为 `fc_nand_dispatch_is_armed()` 的同步入口断言；dispatch 又调用 engine 的 `fc_nand_start/step`。两者同属 `src/fc/`，但 engine 无法脱离调度器单独编译/测试，armed 闩锁（调度层概念）泄漏进命令引擎。
- **缓解**：armed 闩锁已移到 `fc_nand_engine.c`（`fc_nand_engine_set_armed` / `fc_nand_engine_is_armed`）；dispatch 只查询；`fc_nand_dispatch_init` 负责复位；engine 不 include `fc_nand_dispatch.h`。
- **下一步**：上板回归 init → set_armed(1) → 正常 IO；确认放行时机与改前一致。
- **证据**：`src/fc/fc_nand_engine.c:fc_nand_engine_set_armed / fc_nand_engine_is_armed`；`src/fc/fc_nand_dispatch.c:fc_nand_dispatch_enqueue / fc_nand_dispatch_init`；`src/main.c`。
- **历史/验证**：
  - 2026-11-06 修复：armed 闩锁下沉到 engine，dispatch 只查询 → 未上板

#### RISK-DP-10 fc_nand_dispatch_enqueue 入口检查只有 debug ASSERT

- **后果**：`requests != NULL`、`count ∈ [1, DEPTH]`、`ch/ce/lun < NUM`、链成员同 target、`cw_bitmap` 上界只有 debug ASSERT；release 下 NULL/count=0 会解引用 `requests[0]`，越界 route 会按错误 LUN 入队/越界索引，错 target 链会静默进同一 FIFO。
- **缓解**：上述检查全部改 `ASSERT_REJECT(NAND_RESULT_INVALID_ARGUMENT)`；operation 原有 release 拒绝保留。
- **下一步**：上板/负例：NULL、count=0、越界 route、跨 target 链，确认 debug 停机/release 返回 INVALID。
- **证据**：`src/fc/fc_nand_dispatch.c:fc_nand_dispatch_enqueue`；`src/fc/fc.c:fc_submit / fc_poll_fill_queue`；RISK-SCH-04。
- **历史/验证**：
  - 2026-11-07 修复：enqueue 入口检查全部改 `ASSERT_REJECT` → 未上板

#### RISK-DP-11 fc_nand_dispatch_lun_drive 忽略 µs 时钟失败

- **后果**：ISSUE→ISSUED 时 `(void)delay_time_now_us(&wait_start_us)`；若 SysTick 未初始化，`wait_start_us` 保留旧值，`delay_time_elapsed_us` 恒返回 false ⇒ LUN 永久停在 ISSUED，任务永不 FETCH/完成（静默活锁）。
- **缓解**：`fc_nand_dispatch_lun_drive` 在 `delay_time_now_us` 失败时置 `s_dispatch_fatal` 并 `LOGE`；poll 停止推进，`fc_service` 转 controller fatal 返回 IO_ERROR。
- **下一步**：上板负例：未初始化 time 源直接 poll，确认不再永久停在 ISSUED。
- **证据**：`src/fc/fc_nand_dispatch.c:fc_nand_dispatch_lun_drive / fc_nand_dispatch_issued_advance`；`src/platform/delay.c:delay_time_now_us / delay_time_elapsed_us`。
- **历史/验证**：
  - 2026-11-07 修复：时钟失败置 `s_dispatch_fatal` 并转 IO_ERROR → 未上板

#### RISK-DP-12 单 plane cache 读失败只标最低位 CW

- **后果**：`OP_READ_PAGE_CACHE_SEQ/RANDOM/LAST` 整页读入 `page_buffer` 后 scatter 到全部选中 CW，但 completion 表用 `completion_per_cw`，只写 `task->cw_index`；多 CW 失败时其余 desc 保持 UNUSED，FTL/RR 的 per-CW 归因不完整。
- **缓解**：三个 op 改 `completion_all_cws`，失败时选中 CW 全部置 `FAIL`；与 `START_SEQ`/MP 变体同口径。
- **下一步**：上板/负例：单 plane cache 多 CW 注入 NAND 失败，确认所有选中 desc 为 `FAIL`；正向回归 cache 读。
- **证据**：`src/fc/fc_nand_dispatch.c:g_fc_nand_dispatch_completion_table / completion_per_cw`；`req_cache_read / scatter_page_buffer`。
- **历史/验证**：
  - 2026-11-07 修复：三个 cache 读 op 改 `completion_all_cws` → 未上板

#### RISK-DP-13 ecc_bit_count 无生产者

- **后果**：`fc_nand_dispatch_comp_s.ecc_bit_count` 恒 0，`fc_complete_from_nand` 却回写 `desc->ecc_bits`；3D NAND 的可靠性/搬移判断拿不到实际纠正 bit 数。
- **缓解**：completion sink 改为可写；`fc_complete_from_nand` 在数据路径后把 `desc->ecc_bits` 按 flat CW 回填到 `ecc_bit_count[]`；当前 CQ/FTL 尚无该字段消费者，现有可靠性判断仍走 desc / `fc_get_cmd_ecc_max`。
- **下一步**：上板/负例：page read 注入可纠正错误，确认 `desc->ecc_bits` 与 `completion.ecc_bit_count` 同源且非零；空页/UNC 为 0。
- **证据**：`include/fc/fc_nand_dispatch.h`；`src/fc/fc.c:fc_complete_from_nand`；`src/fc/fc_data_path.c:fc_data_path_process_read`。
- **历史/验证**：
  - 2026-11-07 修复：按 flat CW 回填 `ecc_bit_count[]` → 未上板

#### RISK-DP-14 ldpc 的 0 被命名为 BCH

- **后果**：枚举值 0 原命名 BCH，实际 0 = normal，当前默认编译引擎是 BCH；1/2 LDPC 未完整支持，数据路径只区分 raw，文档却写成运行时 BCH/LDPC。
- **缓解**：枚举 0 改 `FC_ECC_MODE_NORMAL`，全生产调用点同步；`fc_ecc_engine.h` 说明 0=normal、当前编译引擎 BCH、LDPC hard/soft 未完整支持。
- **下一步**：新增 ECC 配置/文档时确认不再使用 `FC_ECC_MODE_BCH`；启用 LDPC 前先完成实现与验证。
- **证据**：`include/fc/fc_command.h`；`include/fc/fc_ecc_engine.h`；`src/ftl/*`；`include/ftl/ftl_fc_interface.h`。
- **历史/验证**：
  - 2026-11-07 修复：枚举 0 改名 `FC_ECC_MODE_NORMAL` → 未上板

#### RISK-DP-15 FC_ECC_ALGO_LDPC 分支未编译

- **后果**：`fc_ecc_engine.c` 使用 `g_qc`，但文件作用域只有 `s_qc`，`g_qc` 只是 `fc_ldpc_qc_init` 内的 static；一旦切 LDPC 构建会编译失败。
- **缓解**：默认 BCH 构建不受影响；未按 R8 切开关验证。
- **下一步**：启用 LDPC 构建前修正符号/生命周期并补负例；当前保持 BCH。
- **证据**：`src/fc/fc_ecc_engine.c:s_qc / g_qc / fc_ldpc_qc_init`。
- **历史/验证**：
  - 2026-11-07 审视：`g_qc` / `s_qc` 符号不一致 → 待后续，未验证

#### RISK-DP-16 fc_nand_dispatch_poll 用 BYTE 返回完成数

- **后果**：上限 `CH_NUM×CE_NUM×LUN_NUM` 可由静态上界到 256，恰好 256 会回绕为 0，`fc_service` 误判无完成。
- **缓解**：返回类型与本地计数改为 `DWORD`；`fc_service` 同步使用 `DWORD`。当前板 1/1/1 无行为变化。
- **下一步**：上板多通道/多 CE 配置，确认一轮完成数与实际完成条数一致，不再回绕。
- **证据**：`include/fc/fc_nand_dispatch.h`；`src/fc/fc_nand_dispatch.c:fc_nand_dispatch_poll`；`src/fc/fc.c:fc_service`。
- **历史/验证**：
  - 2026-11-07 修复：poll 返回类型与本地计数改 `DWORD` → 未上板

#### RISK-DP-17 JUMP_IF_NOT_READY 回跳无 release 下溢守卫

- **后果**：`state->uop_index -= uop->operand`，静态 UOP 表若写错 operand 会让 WORD 下溢并越界读表；原只有 debug `ASSERT`，release 下仍会继续执行。
- **缓解**：`fc_uop_engine_step` 在减法前改 `ASSERT_REJECT(uop->operand <= state->uop_index, NAND_RESULT_INVALID_ARGUMENT, ...)`；UOP 表静态，当前 operand 均合法。
- **下一步**：负例：构造回跳 operand > index，确认 debug 停机、release 返回 INVALID 而非越界读表。
- **证据**：`src/fc/fc_nand_engine.c:fc_uop_engine_step`。
- **历史/验证**：
  - 2026-11-07 修复：减法前加 `ASSERT_REJECT` → 未上板

#### RISK-DP-18 random read UOP 缺 CHECK_FAIL

- **后果**：`OP_RANDOM_DATA_READ` / `OP_RANDOM_DATA_READ_MULTI_PLANE` 在逐 CW 链中使用，末尾只做 status→JUMP，不把 FAIL 转成 `execution_status`；后续 CW 成功会覆盖 `nand_status`，中间 CW 失败可被静默吞掉。
- **缓解**：两个 UOP 数组末尾补 `NAND_UOP_CHECK_FAIL`；失败时 chain 在当前 CW 停止并保留错误完成。
- **下一步**：上板/负例：多 CW 读链中间 CW 注入 FAIL（后一个 CW 成功），确认 completion/desc 为失败且不被后续成功覆盖；正向读回归。
- **证据**：`src/nand/onfi_1_0.c:g_nand_random_read_uops / g_nand_random_read_multi_plane_uops`；`src/fc/fc_nand_dispatch.c:fc_nand_dispatch_task_cw_chain`。
- **历史/验证**：
  - 2026-11-07 修复：两个 random read UOP 末尾补 `CHECK_FAIL` → 未上板

#### RISK-DP-19 单 plane cache 读 SEQ/LAST/RANDOM 缺 CHECK_FAIL

- **后果**：这三个 UOP 末尾只有 status→JUMP，没有把 FAIL 转成 `execution_status`；completion handler 看到 OK，`completion_all_cws` 会把选中 CW 标成 `UNUSED`，desc 级失败归因不完整。与 START/MP 变体不一致。
- **缓解**：三个 UOP 数组末尾补 `NAND_UOP_CHECK_FAIL`；失败时 completion result 为错误，选中 CW 统一标 `FAIL`。
- **下一步**：上板/负例：单 plane cache 多 CW 读注入 NAND FAIL，确认命令与所有选中 desc 均失败；正向 cache 读回归。
- **证据**：`src/nand/onfi_1_0.c:g_nand_cache_read_seq_uops / g_nand_cache_read_last_uops / g_nand_cache_read_random_uops`；`src/fc/fc_nand_dispatch.c:g_fc_nand_dispatch_completion_table`。
- **历史/验证**：
  - 2026-11-07 修复：三个 cache 读 UOP 末尾补 `CHECK_FAIL` → 未上板

#### RISK-DP-20 单 plane cache 编程的首页 FAILC 误报

- **后果**：`OP_PROGRAM_PAGE_CACHE` 的 `CHECK_FAIL_CACHE` 对首页也查 `FAIL|FAILC`；首页前没有本批 cache 页，上一批残留 FAILC 会把好页误判失败。
- **缓解**：新增 `OP_PROGRAM_PAGE_CACHE_FIRST`（50），共用 `g_nand_program_cache_uops`；FMC 对两个 FIRST op 只查 FAIL；`fc_complete_from_nand` 的 `failc_valid` 只含中间页/末页 op；单 plane 中间页仍走 `OP_PROGRAM_PAGE_CACHE` 查 `FAIL|FAILC`。
- **下一步**：上板/负例：单 plane cache 中间页注入 FAILC，确认命令失败；上一批注入失败后开新批，确认首页不被残留 FAILC 误杀；正向 cache 写回归。
- **证据**：`include/nand/onfi_1_0.h:OP_PROGRAM_PAGE_CACHE_FIRST`；`src/nand/onfi_1_0.c:g_nand_program_cache_uops`；`src/nand/nand_fmc.c:nand_fmc_exec_check_fail_cache`；`src/fc/fc.c:fc_complete_from_nand`；`src/fc/fc_nand_dispatch.c` request/completion 表。
- **历史/验证**：
  - 2026-11-07 修复：新增 FIRST op，只查 FAIL → 未上板

#### RISK-DP-21 参数页 / 唯一 ID / GET_FEATURES 的 FAIL 被当成成功

- **后果**：三个 UOP 末尾只有 status→JUMP，没有 `CHECK_FAIL`；`fc_nand_execute()` 只返回引擎 `execution_status`，状态字节 FAIL 仍可能返回 OK，参数页坏数据会进入解码/几何检查。
- **缓解**：三个 UOP 末尾补 `NAND_UOP_CHECK_FAIL`；`fc_nand_target_probe()` 的 `fc_nand_read_parameter_page()` 在参数页 FAIL 时返回失败，调用方停止使用可疑参数页。
- **下一步**：上板/负例：参数页读注入 FAIL，确认 target probe 失败；正向开机扫描回归。`OP_READ_ID` 仍按原设计不查 70h。
- **证据**：`src/nand/onfi_1_0.c:g_nand_read_parameter_page_uops / g_nand_read_unique_id_uops / g_nand_get_features_uops`；`src/fc/fc_nand_engine.c:fc_nand_execute`。
- **历史/验证**：
  - 2026-11-07 修复：三个同步读 UOP 末尾补 `CHECK_FAIL` → 未上板

#### RISK-DP-22 WAIT_READY 后端阻塞

- **后果**：`nand_fmc_exec_wait_ready()` 在 FMC handler 内自旋 `while (RB# LOW) delay_us(1)`，只返回 OK/TIMEOUT ⇒ `fc_uop_engine_step()` 的 `NAND_RESULT_NOT_READY` / `wait_latch` / `wait_timeout` 让出路径不触发；一个 LUN 的 tR/tPROG/tBERS 等待占住整个 `fc_nand_step`/dispatch，其他 LUN/通道不能推进，与头文件「`NOT_READY` 是让出点」及多 LUN 并发设计不一致。
- **缓解**：`nand_fmc_exec_wait_ready()` 改 one-shot：RB# LOW → `delay_us(1)` + `NAND_RESULT_NOT_READY`；`fc_uop_engine_step()` 在首次 NOT_READY 时才 `wait_latch`，由 `wait_timeout` 统一判超时；同步初始化路径仍通过 `fc_uop_engine_execute` 循环推进。
- **下一步**：上板回归单 LUN 常规读/写/擦除、cache、multi-plane、reset；双 LUN 并发观察 tPROG/tBERS 期间另一 LUN 是否仍推进；负例确认 `WAIT_READY` 超时仍返回 TIMEOUT。⚠ 行为变更，必须上板后才能宣称修复。
- **证据**：`src/nand/nand_fmc.c:nand_fmc_exec_wait_ready`；`src/fc/fc_nand_engine.c:fc_uop_engine_step / fc_uop_engine_wait_latch / fc_uop_engine_wait_timeout`；`include/fc/fc_nand_engine.h` 顶部「NOT_READY 是让出点」。
- **历史/验证**：
  - 2026-11-07 修复：WAIT_READY 改 one-shot 协作让出 → 未上板

#### RISK-DP-23 fc_nand_start 依赖调用者预绑定 engine 指针

- **后果**：`fc_nand_start()` 把 request 拷进 `transaction->request`，随后 `fc_uop_engine_start()` 使用 `state->request/state->completion`；这两个指针只有 `fc_nand_dispatch_task_load()` 和高速切换函数会绑定；新调用者用零初始化 `fc_nand_txn_s` 直接 start 会触发 debug 断言，release 下解引用空指针。
- **缓解**：`fc_nand_start()` 统一绑定 `engine.request = &transaction->request`、`engine.completion = &transaction->completion`；保留 `engine.backend` 由调用者按 target 绑定，入口用 `ASSERT_REJECT` 拒绝空 backend/非法参数；头文件写明契约。
- **下一步**：负例：零初始化 txn（不绑定 request/completion）调 start，确认 debug 停机、release 返回 INVALID；上板回归 dispatch 与高速切换。
- **证据**：`src/fc/fc_nand_engine.c:fc_nand_start`；`include/fc/fc_nand_engine.h:fc_nand_start`；`src/fc/fc_nand_dispatch.c:fc_nand_dispatch_task_load`；`fc_nand_enable_high_speed_target`。
- **历史/验证**：
  - 2026-11-07 修复：start 统一绑定 request/completion → 未上板

#### RISK-DP-24 高速模式失败后没有回滚到低速

- **后果**：`fc_nand_enable_high_speed_target()` 先发 `SET_FEATURES` 让 NAND 进入 Mode N，再调 `dev->set_timing()` 更新 FMC PCC；若 `set_timing`（HAL_NAND_Init 或 backend 重配）失败，NAND 已在 Mode N、FMC 仍是旧时序，后续 IO 可能不稳定。`main` 只告警并继续运行；注释「失败保持低速」不成立。多 target 时还会留下前面 target 已切高速、后面失败的混合状态。
- **缓解**：单 target：先 `set_timing(0)` 恢复低速 FMC，再用低速 FMC 发 `SET_FEATURES` Mode 0；`mt29f_set_timing` 已支持 mode 0 低 PCC。多 target：`fc_nand_enable_high_speed` 失败时遍历全部 present target 调 `fc_nand_enable_low_speed_target`，回滚失败逐条 `LOGE`。
- **下一步**：上板负例：注入 HAL_NAND_Init 失败，确认单 target 回到低速且后续 IO 可用；多 target 板卡逐 target 注入，确认全部回滚；回滚失败只记录，仍不能宣称「保持低速」。
- **证据**：`src/fc/fc_nand_engine.c:fc_nand_enable_high_speed_target / fc_nand_enable_low_speed_target / fc_nand_set_timing_mode`；`src/nand/devices/mt29f___g08a.c:mt29f_fill_low_pcc / mt29f_set_timing`；`src/main.c` 高速切换失败分支。
- **历史/验证**：
  - 2026-11-07 修复：切换失败回滚低速 FMC/NAND → 未上板

#### RISK-DP-25 dispatch 不校验 req/completion 表项非空

- **后果**：`fc_nand_dispatch_enqueue()` 只拒绝 `operation >= OP_COUNT`；未使用槽位（表项为 NULL）仍可入队，随后 `fc_nand_dispatch_task_start()` 的 `g_fc_nand_dispatch_req_table[op](task)` 或 `fc_nand_dispatch_task_complete()` 的 completion 表调用会空指针崩溃。
- **缓解**：enqueue 对每个 request 增加 req/completion 表项非空 `ASSERT_REJECT`，debug 停机、release 返回 `INVALID_ARGUMENT`；当前 FC/FTL 已实现 op 均通过检查。
- **下一步**：负例：构造未使用/保留 op 提交，确认 debug 停机、release 拒绝；上板回归正常 op。
- **证据**：`src/fc/fc_nand_dispatch.c:fc_nand_dispatch_enqueue / fc_nand_dispatch_task_start / fc_nand_dispatch_task_complete`；`include/nand/onfi_1_0.h:OP_COUNT`。
- **历史/验证**：
  - 2026-11-07 修复：enqueue 增加 req/completion 表项非空检查 → 未上板

#### RISK-DP-26 带 DATA_OUT 的稀疏 CW 读首输出读错 CW

- **后果**：直接下发 `OP_NORMAL_READ_PAGE_WITH_DATA_OUT` / `OP_OTP_DATA_READ` 的稀疏 CW 读时，UOP 在随机链之前先做一次 `NAND_UOP_DATA_OUT`，`req_read_page()` 把这块 buffer 指向掩码最低 CW，但列地址仍为 0；最低 CW 不是 CW0 时，这一块首输出读的是 CW0。生产 FC 已把普通读改写成 WITHOUT/LOAD（无 DATA_OUT），OTP 当前无生产调用者，暂未触发。
- **缓解**：`fc_nand_dispatch_select_read_op()` 按位图分流：单 LUN 严格全页保留 WITH_DATA_OUT，整页读入 page buffer；稀疏位图或多 LUN 走 WITHOUT/LOAD，所有选中 CW 由 05h/06h 取。`req_read_page` 对全页 direct 设置 `PAGE_SIZE` page buffer；`completion_all_cws` + scatter 恢复 full single/MP；`cw_chain` 没有「首 CW 已 DATA_OUT」分支。
- **下一步**：上板：严格全页普通/MP/OTP 读走 direct；稀疏 CW1/CW2、多 LUN 走 LOAD+05/06；全页与稀疏的数据/ECC/完成状态回归。
- **证据**：`src/fc/fc_nand_dispatch.c:fc_nand_dispatch_bitmap_is_full_single/multi / fc_nand_dispatch_select_read_op / fc_nand_dispatch_task_load / req_read_page / fc_nand_dispatch_scatter_page_buffer`；`src/fc/fc.c:fc_build_dispatch_request`；`src/nand/onfi_1_0.c:g_nand_normal_read_page_uops`。
- **历史/验证**：
  - 2026-11-07 修复：按位图分流 direct/load 读 op（UOP 表未改）→ `make all` / `make release` 通过，未上板

#### RISK-DP-27 LDPC 解码栈占用超过 2 KiB 栈预算

- **后果**：`fc_ldpc_qc_decode()` 局部 `BYTE col_failures[QC_N]`（QC_N=4224）本身 4.2KB，另有 `syndromes[64]`；`_Min_Stack_Size=0x800`。若切 LDPC 构建，运行到解码就会踩栈。
- **缓解**：默认 BCH 不编译该分支；LDPC 未启用。
- **下一步**：启用 LDPC 前把 `col_failures` 移到静态/全局（或按 bit 压缩），再按 `stack_guard` 上板验证；与 RISK-DP-15 的 `g_qc` 符号问题一起修。
- **证据**：`src/fc/fc_ecc_engine.c:fc_ldpc_qc_decode`；`linker/stm32h743xx_flash.ld:_Min_Stack_Size`。
- **历史/验证**：
  - 2026-11-07 审视：局部 `col_failures` 4.2KB 超出栈预算 → 待后续，未修

#### RISK-DP-28 CRC 初始化失败在 release 下是静默的

- **后果**：`crc_init()` 只通过 `ASSERT(crc_port_get_idr() == 0xA5U)` 验证外设应答，`crc_port_init()` 也只 `ASSERT(HAL_CRC_Init(...) == HAL_OK)`；`NDEBUG` 下两处都裁空。若 CRC 时钟/基址再错，`HAL_CRC_Calculate` 返回 0，`fc_crc_compute` 写 0、`fc_crc_verify` 比 `0 == 0` 恒通过 ⇒ 整层 CRC 保护静默失效。
- **缓解**：基址已修正，debug 档有 IDR 回环；release 无运行期拒绝。
- **下一步**：CRC 是静默介质破坏类校验，按 H14/判据 9 需要 release 守卫：`crc_init`/`fc_crc_init` 返回状态，`fc_init` 传播或置 CRC 不可用，`fc_data_path` 在不可用时拒绝写/读并报错；上板注入 CRC 初始化失败做负例。
- **证据**：`src/platform/crc.c:crc_init`；`src/platform/stm32h743/crc_port.c:crc_port_init / crc_port_compute`；`src/fc/fc_crc_engine.c`；`src/fc/fc_data_path.c:fc_data_path_crc_write / fc_data_path_crc_check`。
- **历史/验证**：
  - 2026-11-07 审视：CRC 初始化只有 ASSERT → 待后续，release 仍无守卫
  - （日期未记录）CRC 基址修正 → debug 档 IDR 回环可用

### 已闭合（详情见 archive）

| ID | 日期 | 一句话 | 归档 |
|---|---|---|---|
| RISK-DP-30 | 2026-11-11 | multi-plane 编程误删 `11h` 后 1µs 延时致读回 META 错，补 TWB+WAIT_READY 后 `/fc-nand/` 59/59 全绿 | `docs/archive/known_risks_batches.md` |
| RISK-DP-29 | 2026-11-13 | 三条新增 FC-NAND UT 覆盖上板通过（61 OK + 3 SKIP） | `docs/archive/known_risks_batches.md` |

---

## 2AH. FC 主循环 / 调度 / CQ

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-SCH-01 | 待上板 | 链成员跨目标只在 debug 拦截，release 仍按链头 LUN 入队 | 未上板 |
| RISK-SCH-02 | 待上板 | fc_core_poll 只在无完成时填充，持续完成下提交延迟 | 未上板 |
| RISK-SCH-03 | 待决策 | CQ 满会丢完成项，FTL 永远收不到命令 | 2026-11-06 代码核对 |
| RISK-SCH-04 | 信息 | 路由字段越界在 release 会越界访问 suspended 数组 | 2026-11-06 代码核对 |
| RISK-SCH-05 | 待上板 | fill_pops 少计链成员 pop，链长 N 少计 N-1 | 未上板 |
| RISK-SCH-06 | 待上板 | 断言终端路径再次断言，递归覆盖原始现场 | 未上板 |
| RISK-SCH-07 | 待上板 | fc_submit_batch 不拒绝批内重复命令指针 | 未上板 |
| RISK-SCH-08 | 待上板 | fc_get_cmd_data_ptr 的 cw_index 不含 plane | 未上板 |
| RISK-SCH-09 | 待上板 | staging helper 无 plane/cw0/cmd_id 范围守卫 | 未上板 |
| RISK-SCH-10 | 待上板 | 合成失败完成项忽略 CQ 满，FTL 等不到完成 | 未上板 |
| RISK-SCH-11 | 待上板 | 完成侧不变量只有 debug ASSERT，release 继续写 CQ | 未上板 |
| RISK-SCH-12 | 待上板 | fc_submit_batch 入参/提交不变量只有 debug ASSERT | 未上板 |
| RISK-SCH-13 | 待上板 | 合成失败完成项固定写 cw_status[0]，plane1/CW>0 归因丢失 | 未上板 |

### 详情

#### RISK-SCH-01 链成员跨目标只在 debug 拦截

- **后果**：`fc_poll_chain_length` 发现下一成员 ch/ce/lun 与链头不同，release 下仍把它算进链，随后按链头 LUN 选 FIFO，错目标命令被塞进同一条 per-LUN 队列。
- **缓解**：target 不一致时 debug `ASSERT_MSG`；release 截断在已确认前缀，剩余成员留在队列下一轮独立处理；`fc_nand_dispatch_enqueue` 的同 LUN 断言仍保留。
- **下一步**：上板/负例构造跨 LUN 链；确认不执行错 LUN、命令各自失败/重试。
- **证据**：`src/fc/fc.c:fc_poll_chain_length`；`src/fc/fc_nand_dispatch.c:fc_nand_dispatch_enqueue`。
- **历史/验证**：
  - 2026-11-06 修复：release 截断跨 target 链，剩余成员下一轮独立处理 → 未上板

#### RISK-SCH-02 fc_core_poll 只在无完成时填充

- **后果**：持续有完成时提交被让位，单次调用 `/service` 的前端会看到提交延迟。
- **缓解**：同一轮完成处理后再执行一次 `fc_poll_fill`；填充 `IO_ERROR` 上升，`QUEUE_EMPTY/NOT_READY` 不改变「该轮有完成」的成功语义。
- **下一步**：上板观察持续完成下的提交延迟；确认 FTL/HIL 的单次/多次 `fc_service` 都能量正常推进。
- **证据**：`src/fc/fc.c:fc_core_poll`；`src/ftl/ftl.c:ftl_service_pump`。
- **历史/验证**：
  - 2026-11-06 修复：完成处理后补一次 `fc_poll_fill` → 未上板

#### RISK-SCH-03 CQ 满会丢完成项

- **后果**：`fc_complete_from_nand` 在 CQ 满时把命令置 COMPLETED 但没有 CQ entry，FTL 永远收不到该命令；FC 侧标记只是防槽位永久 ACTIVE，不能恢复 FTL 侧完成。
- **缓解**：`LOGE` + 返回 `IO_ERROR`；`fc_core_poll` 把该轮上报为 fatal；正常情况下 CQ 深度=命令池深度且 release 只在取走后发生，CQ 满结构上不可达。
- **下一步**：二选一：提交侧按 CQ 高水位/预留禁止把 CQ 填到危险水位；或把 CQ 满定义为系统复位/停机路径。先确认是否需要支持「FTL 长时间不抽 CQ」。
- **证据**：`src/fc/fc.c:fc_complete_from_nand / fc_core_poll`；`src/fc/fc_queue.c:fc_completion_queue_push`。
- **历史/验证**：
  - 2026-11-06 审视：判定 CQ 满为 fatal，待二选一决策 → 未改

#### RISK-SCH-04 路由字段越界在 release 会越界访问 suspended

- **后果**：`fc_poll_fill_queue` 只有 ASSERT；越界 PBA 若绕过 FTL 校验，填充循环会写 `suspended[ch][ce][lun]` 栈外。
- **缓解**：FTL 侧 `pba_is_valid`（RISK-L2P-21）已覆盖生产路径；FC 内 DEBUG 断言保留。
- **下一步**：维护规则——新增第二个 host/前端入口前，把 ch/ce/lun 校验上移到 FC 提交边界（需同步 v19 返回契约）。
- **证据**：`src/fc/fc.c:fc_poll_fill_queue`；`include/ftl/ftl_fc_interface.h:pba_to_cmd`；RISK-L2P-21。
- **历史/验证**：
  - 2026-11-06 判定：依赖 FTL `pba_is_valid` 覆盖生产路径 → 保持现状

#### RISK-SCH-05 fill_pops 口径与实现不符

- **后果**：`fill_pops` 注释为「累计从队列 pop 的命令数」（`include/fc/fc_interface.h:361`），但 `fc_poll_fill_queue` 只在队头 pop 后调 `fc_fill_pops_inc()`（`src/fc/fc.c:1262`）；链成员 pop（`src/fc/fc.c:1307`）若不走同一计数 ⇒ 链长 N 的一次下发少计 N-1，`fill_delta`（RISK-FC-05 的填充活性证据）口径失真会误导定位。
- **缓解**：链成员 pop 循环内也调用 `fc_fill_pops_inc()`，口径等于真实 pop 事件数；仅 debug 档计数，release 为空函数。
- **下一步**：上板观察 cache run 下 `fill_pops` 与 `fill_calls` 的比值，确认不再少计；若后续只想看队头弹出/扫描轮次，再改字段名与注释并同步 `fc_debug_state_s`。
- **证据**：`src/fc/fc.c:fc_poll_fill_queue`（`fc_fill_pops_inc` 调用点）；`include/fc/fc_interface.h:361:fill_pops`；RISK-FC-05。
- **历史/验证**：
  - 2026-11-06 修复：链成员 pop 补 `fc_fill_pops_inc` → 未上板

#### RISK-SCH-06 断言终端路径会再次断言

- **后果**：`assertion_platform_context` 调 `fc_get_resource_stats`；后者在 debug 档扫描全池并 `ASSERT` 计数一致。断言往往正由池计数/状态不一致触发 ⇒ 扫描再次失败 ⇒ `assertion_fail` → `assertion_terminate` → 再次调用本钩子，递归、原始断言现场被覆盖或栈溢出。
- **缓解**：钩子改为直接读 `g_fc_controller` 的计数字段（不扫描、不断言）；`fc_get_resource_stats` 注释明确禁止用于断言/故障路径。
- **下一步**：上板/负例触发一次断言，确认 hook 不递归、原始现场保留；确认 `fc_pool_init` 前断言也能安全打印。
- **证据**：`src/fc/fc.c:assertion_platform_context / fc_get_resource_stats`；`src/common/assertion.c:assertion_terminate`。
- **历史/验证**：
  - 2026-11-06 修复：钩子直接读 `g_fc_controller` 计数 → 未上板

#### RISK-SCH-07 fc_submit_batch 不拒绝批内重复命令指针

- **后果**：校验循环在提交前对每条都查 `ALLOCATED`，同一 `fc_cmd_s*` 出现两次时两次都通过；提交循环 push 两次，第二次 `fc_pool_command_state_set(QUEUED)` 被拒绝但队列里已经多了一条；fill 会把同一命令当两条派发（setter 拒绝后 dispatch 仍继续），可能对同一 desc/buffer 二次下发；FTL `ftl_op_req_add` 也只在 pending 表里覆盖并打 `LOGW`。
- **缓解**：批量校验循环加 O(n²) 重复指针检查，命中即返回 `FC_STATUS_INVALID_ARGUMENT`，且在 `sq_need/hsq_need` 计数之前返回。
- **下一步**：构造批内重复命令负例（debug 停机 / release 拒绝），上板确认队列里不会出现重复 ID。
- **证据**：`src/fc/fc.c:fc_submit_batch / fc_sched_submit / fc_poll_dispatch_single`；`src/ftl/ftl_op.c:ftl_op_req_add`。
- **历史/验证**：
  - 2026-11-06 修复：批内加 O(n²) 重复指针检查 → 未上板

#### RISK-SCH-08 fc_get_cmd_data_ptr 的 cw_index 不带 plane

- **后果**：multi-plane 命令的链上同一 CW 号在两个 plane 各有一条 desc，函数返回链序第一个匹配（通常 plane 0）⇒ 若用它们取 plane 1 的数据/META，会静默拿到 plane 0。当前调用者 `ftl_sync_read_page_raw` 与 `gc_decode_cw_lba` 都只用于单 plane 命令，未触发。
- **缓解**：头文件明确「仅单 plane」；实现加 debug ASSERT + release 返回 NULL（不静默交出错 plane）；multi-plane 逐 CW 读取走 `ftl_cmd_read_data` 沿链遍历。
- **下一步**：上板回归单 plane 读/GC；若将来新增 multi-plane 调用者，改加 plane 形参或新 API。
- **证据**：`src/fc/fc_pool.c:fc_get_cmd_data_ptr / fc_buffer_get_meta`；`src/ftl/ftl_fc_interface.c:ftl_sync_read_page_raw`；`src/ftl/gc.c:gc_decode_cw_lba`。
- **历史/验证**：
  - 2026-11-06 修复：头文件限定单 plane，release 返回 NULL → 未上板

#### RISK-SCH-09 fc_request_use_staging 没有平面/下标守卫

- **后果**：`plane = paa_to_plane(cmd->paa)` 直接用于 `fc_cw_flat` 和 `cw_buffers[cw0]`。单 plane 命令在提交边界由 `fc_pool_chain_validate` 的 plane_span 拒绝越界；但若 staging 类命令（FEATURES/META/ONFI/COLUMN）被误配 `multi_plane > 0`，plane_span 从 0 起、不校验 `cmd->paa` 的 plane，release 下可越界写 request。
- **缓解**：helper 加 `ASSERT(plane < PLANE_NUM)` / `ASSERT(cw0 < CW_TOTAL_MAX)` / `ASSERT(cmd_id < DEPTH)`；提交边界 `fc_pool_chain_validate` 对**所有**命令校验 `paa_to_plane(cmd->paa) < PLANE_NUM`（release 拒绝）。
- **下一步**：构造「staging op + multi_plane>0 + 非法 paa plane」负例，确认 release 在提交边界拒绝；上板回归 FEATURES/META/COLUMN/ONFI。
- **证据**：`src/fc/fc.c:fc_request_use_staging`；`src/fc/fc_pool.c:fc_pool_chain_validate`；`include/fc/fc_command.h:fc_command_plane_span`。
- **历史/验证**：
  - 2026-11-06 修复：helper 断言 + 提交边界校验 plane → 未上板

#### RISK-SCH-10 合成失败完成项忽略 CQ 满

- **后果**：`fc_poll_fail_command`（dispatch build/submit 失败兜底）与 `fc_stripe_submit` 的 `stripe_error` 都 `(void)fc_complete_from_nand(...)`。该函数仅在 CQ 满时返回 IO_ERROR；忽略后命令没有 CQ entry，FTL 永远等不到完成，且 `fc_poll_fail_command` 为 void 无法回传。
- **缓解**：CQ 满在 `fc_complete_from_nand` 置 `controller->fatal`；sink 直接使用 controller；`fc_poll_fail_command` 与 `stripe_error` 检查返回值并置 fatal。
- **下一步**：上板负例：CQ 满时合成失败完成项也被 fatal 捕获。
- **证据**：`src/fc/fc.c:fc_poll_fail_command / fc_stripe_submit / fc_complete_from_nand`；RISK-SCH-03。
- **历史/验证**：
  - 2026-11-07 修复：合成失败路径检查返回值并置 fatal → 未上板

#### RISK-SCH-11 fc_complete_from_nand 的完成侧不变量只有 debug ASSERT

- **后果**：`command_state == ACTIVE`、Step 1 desc 链有界遍历后 `desc_id == INVALID` 只有 debug ASSERT；release 下状态/链损坏仍继续写 desc/CQ，可能产生重复或错误完成。
- **缓解**：ACTIVE 与 Step 1/Step 3 链尾均改 `ASSERT_REJECT(..., FC_STATUS_IO_ERROR)`；sink/合成路径收到 IO_ERROR 后置 controller fatal。
- **下一步**：上板负例：非 ACTIVE 完成 / 成环 desc，确认 release 返回 IO_ERROR 而非继续写 CQ。
- **证据**：`src/fc/fc.c:fc_complete_from_nand`；RISK-DP-04。
- **历史/验证**：
  - 2026-11-07 修复：完成侧不变量改 `ASSERT_REJECT(FC_STATUS_IO_ERROR)` → 未上板

#### RISK-SCH-12 fc_submit_batch 的入参/提交不变量只有 debug ASSERT

- **后果**：`cmds != NULL`、`n > 0` 在 release 下分别崩溃/返回 OK；提交循环 `ASSERT(st == OK)` + `(void)st` 忽略预检后的 `fc_sched_submit` 失败，可能半批入队。
- **缓解**：`cmds/n` 改 `ASSERT_REJECT(INVALID_ARGUMENT)`；提交循环失败置 controller fatal 并返回 IO_ERROR。
- **下一步**：上板负例：NULL 数组、n=0、预检后人为破坏队列不变量。
- **证据**：`src/fc/fc.c:fc_submit_batch / fc_sched_submit`；RISK-SCH-07。
- **历史/验证**：
  - 2026-11-07 修复：入口改 `ASSERT_REJECT`，提交失败置 fatal → 未上板

#### RISK-SCH-13 合成失败完成项固定写 cw_status[0]

- **后果**：`fc_poll_fail_command`（encode/build/submit 失败兜底）只标 flat CW 0；命令只访问 plane1 或 CW>0 时，该位对不上任何 desc，desc 级失败归因丢失，FTL 的 `bad_plane_mask` 只能回退 `pending->new_pba`，可能漏标/错标 plane。
- **缓解**：`fc_poll_fail_command` 用 `fc_get_cmd_cw_bitmap(cmd_id)` 按实际 flat CW 集合布 `FAIL`；仅无 desc 命令回退 `[0]`；`fc_complete_from_nand` 仍按 desc 的 flat 下标聚合。
- **下一步**：上板/负例：plane1-only 或 CW>0 命令分别注入 encode/build 失败，确认对应 desc 全部为 `FAIL`、FTL 的 `bad_plane_mask` 指向正确 plane。
- **证据**：`src/fc/fc.c:fc_poll_fail_command / fc_complete_from_nand`；`src/ftl/ftl.c:ftl_host_io_write_fail`。
- **历史/验证**：
  - 2026-11-07 修复：按实际 flat CW 集合布 `FAIL` → 未上板

---
## 2AI. NAND 器件 / PHY

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-NANDP-01 | 待上板 | 多 target 无真实 FMC bank/CE# 切换，命令可能打错 target | 未上板 |
| RISK-NANDP-02 | 待上板 | UOP operand 越界时 release 无守卫，会越界访问 buffer/plane | 未上板 |
| RISK-NANDP-03 | 信息 | Mode 5 运行期写/读回 UNC 仍未定位 | — |
| RISK-NANDP-04 | 待上板 | 多 target 板卡未逐 target 初始化，backend handlers 为空 | 未上板 |
| RISK-NANDP-05 | 待后续 | 多 target timing 共享单套 HAL 句柄，切模式影响整块 FMC | — |
| RISK-NANDP-06 | 待上板 | target 选择与 backend 获取拆成两半，漏调用会静默写错 target | 未上板 |
| RISK-NANDP-07 | 待上板 | HIL 依赖器件专用 timing 查询，换器件会编译失败 | 未上板 |
| RISK-NANDP-08 | 待上板 | timing 表部分字段无消费者，改表不生效 | 未上板 |
| RISK-NANDP-10 | 待上板 | timing 零值 release 下静默 delay(0)，时序不足 | 未上板 |
| RISK-NANDP-11 | 待上板 | timing 表命名 Mode0 实为 Mode5，误改会拖慢高速 | 2026-11-07 编译通过 |
| RISK-NANDP-12 | 待上板 | 多 target 无真实 bank 切换，跨轮次可能选错 target | 未上板 |
| RISK-NANDP-13 | 待上板 | timing 查询/设置 release 无边界，越界会写坏状态 | 未上板 |
| RISK-NANDP-14 | 待上板 | 多条 busy 启动命令缺 tWB，WAIT_READY 提前返回读到旧数据 | 未上板 |
| RISK-NANDP-15 | 待上板 | 首次 RESET timeout 500µs 小于 datasheet 1ms，冷启动可能误判 | 未上板 |
| RISK-NANDP-16 | 待上板 | tADL 位置错误；11h 后 1µs 实为 plane 切换等待 | 2026-11-11 上板通过 |
| RISK-NANDP-17 | 待上板 | tR timeout 38µs 小于 tR_ECC 70µs，启用内部 ECC 会提前超时 | 未上板 |
| RISK-NANDP-18 | 信息 | 01h timing mode 的 tITC 1µs 已被 700µs timeout 覆盖 | — |

### 详情

#### RISK-NANDP-01 多 target 物理通路未实现

- **后果**：`nand_select_target()` 已接入 dispatch task start 与同步 API 入口，backend 按 `(ch,ce)` 取；但 `nand_board.c` 仍只有 `(0,0)`，`nand_select_target` 只做通路校验，没有真实 FMC bank/CE# 切换。多 target 板卡上命令仍可能打到当前 target。
- **缓解**：当前板只有 `(0,0)`；单 target 下行为不变。
- **下一步**：增加第二个物理 target 前，在 `nand_board.c` 扩展 target 表并实现 FMC bank/CE# 切换；上板验证。
- **证据**：`src/nand/nand_board.c:nand_select_target`；`src/fc/fc_nand_dispatch.c:fc_nand_dispatch_task_fc_start`；`src/nand/nand_fmc.c:nand_phy_backend_get`。
- **历史/验证**：
  - 2026-11-06 接入 target 选择到 dispatch start 与同步 API → 未上板

#### RISK-NANDP-02 FMC PIO buffer/row 边界依赖静态 UOP 表

- **后果**：`DATA_IN/OUT` 用 `uop->operand` 作 buffer 偏移，`ROW_*` 用 operand 作 plane 下标；handler 级若只有 debug `ASSERT`，release 下 UOP 表新增错误 operand 会越界。
- **缓解**：`nand_fmc_row_byte` 为 `BOOL + 出参`，三个 ROW handler 用 `ASSERT_REJECT` 拒绝 plane 越界；`nand_fmc_data_range_is_valid` 检查 buffer 非空、长度非零、`operand + data_length <= PAGE_BUFFER_SIZE`，`DATA_IN/OUT` 用 `ASSERT_REJECT` 拒绝。实际 buffer 容量仍由 `req_*` 构建层保证（`nand_cmd_req_s` 不带容量字段）。
- **下一步**：新增 UOP 类型或改 operand 语义时，同步补 request 层范围校验；上板前用负例 UOP 表检查 `ASSERT_REJECT` 的 release 分支确实返回 INVALID；若要精确校验实际容量，再给 request 增加 buffer capacity 字段。
- **证据**：`src/nand/nand_fmc.c:nand_fmc_row_byte` / `nand_fmc_data_range_is_valid` / `nand_fmc_exec_data_in` / `nand_fmc_exec_data_out`；`src/nand/onfi_1_0.c` UOP 表。
- **历史/验证**：
  - 2026-11-07 增加 ROW/data 范围校验并改 `ASSERT_REJECT` → 未上板

#### RISK-NANDP-03 Mode 5 运行期写/读回 UNC 未定位

- **后果**：Mode 5 运行期写/读回出现 UNC，根因未定位；PHY 结构收口未改变 `CONFIG_NAND_TIMING_MODE=5` 的默认值或时序表。
- **缓解**：低速启动；高速切换由 `fc_nand_enable_high_speed` 控制；RISK-NAND-06 已有定位步骤。
- **下一步**：按 RISK-NAND-06 的 re-scan/直测步骤继续；不要因结构改动把它当已解决。
- **证据**：`include/nand/devices/mt29f___g08a.h:CONFIG_NAND_TIMING_MODE`；`src/nand/devices/mt29f___g08a.c:mt29f_set_timing`；`docs/design/known_risks.md:RISK-NAND-06`。
- **历史/验证**：
  - （日期未记录）沿用 RISK-NAND-06 定位步骤 → 信息

#### RISK-NANDP-04 多 target FMC 后端初始化覆盖不完整

- **后果**：多 target 板卡若不逐个初始化，`nand_phy_backend_get(ch,ce)` 会返回 handlers 为空的实例。
- **缓解**：`include/nand/nand_board.h` 提供 `nand_board_init_all_targets()`；`mt29f_hw_init()` 遍历板上 target 表调用 `nand_board_init_target()`；`nand_target_get()` 对 handlers 为 NULL 的 backend 返回 `NOT_READY`。当前 target 表仍只有 `(0,0)`。
- **下一步**：多 target 板卡在 `src/nand/nand_board.c` 填充实际 target 表并实现 NCE/R-B#/FMC bank 选择；上板逐 target 验证。
- **证据**：`include/nand/nand_board.h`；`src/nand/nand_board.c:nand_board_init_all_targets` / `nand_board_init_target` / `nand_target_get`；`src/nand/devices/mt29f___g08a.c:mt29f_hw_init`。
- **历史/验证**：
  - 2026-11-07 新增 target 表遍历初始化 + `NOT_READY` → 未上板

#### RISK-NANDP-05 多 target timing 共享单个 HAL/FMC 句柄

- **后果**：`mt29f_set_timing(ch,ce,...)` 按 target 记录 `s_timing_mode[ch][ce]` 并重配 `nand_fmc_init_target`，但 `g_nand_handle`/HAL PCC 仍是单套；多 target 板卡切一个 target 的 Mode 会重配整块 FMC。
- **缓解**：当前板一个 target；CLI 的 timing mode 查询按 `(ch,ce)` 读状态。
- **下一步**：多 target 板卡把 HAL 句柄/FMC bank 也按 target 分化；上板逐 target 验证模式切换。
- **证据**：`src/nand/devices/mt29f___g08a.c:mt29f_set_timing` / `g_nand_handle`；`src/fc/fc_nand_engine.c:fc_nand_enable_high_speed`。
- **历史/验证**：
  - 2026-11-06 审视判定 → 待后续

#### RISK-NANDP-06 target 选择曾拆成两个半接口

- **后果**：多 target 板卡上漏一次通路切换 ⇒ 命令打到当前 bank，**静默写错 target**。
- **缓解**：target 选择收成唯一入口 `nand_target_get(ch,ce,&backend)`（通路校验 + backend 一次完成）；`nand_select_target` 为 `static`，`nand_phy_backend_get` 收进 `nand_fmc.h`（NAND 内部）；engine/dispatch 全部使用它。
- **下一步**：单 target 回归；实现多 target 时补双通路负例上板。
- **证据**：`include/nand/nand_device.h:nand_target_get`；`src/nand/nand_board.c`；`src/fc/fc_nand_engine.c:fc_nand_execute` / `fc_nand_enable_high_speed_target`；`src/fc/fc_nand_dispatch.c:fc_nand_dispatch_task_fc_start`。
- **历史/验证**：
  - 2026-11-06 收口为 `nand_target_get` → 未上板

#### RISK-NANDP-07 HIL 依赖器件专用查询

- **后果**：换器件/加第二颗器件时，若新器件头不提供同名 `nand_get_timing_mode`，HIL 直接编译失败或被迫感知具体器件。
- **缓解**：`nand_device_desc_s` 增加 `get_timing_mode` 函数指针；`nand_get_timing_mode` 是 `nand_device.h` 的通用查询（实现在 `nand_board.c`）；器件头不再导出，`cli.c` 直接 include `nand_device.h`。
- **下一步**：加第二颗器件时确认 descriptor 填该字段并上板读回。
- **证据**：`include/nand/nand_device.h`；`src/nand/nand_board.c:nand_get_timing_mode`；`src/nand/devices/mt29f___g08a.c:mt29f_get_timing_mode` / `g_mt29f_desc`；`src/hil/cli.c`。
- **历史/验证**：
  - 2026-11-06 timing 查询下沉到 descriptor → 未上板

#### RISK-NANDP-08 NAND 时序双轨且字段无消费者

- **后果**：`nand_op_timing_s.twb_us/trst_us/twhr_us` 只出现在表初始化，实际 tWB/tWH 延时写死在 UOP 的 `NAND_UOP_DELAY_US`；tADL 写死 1U；改 timing 表不生效。
- **缓解**：方案 B：新增 `NAND_UOP_DELAY_TIMING` + `NAND_TIMING_TWB/TWH/TADL` 选择器；UOP 表按当前 op 的 timing 字段延时；`nand_op_timing_s` 改为 `twb_us/twhr_us/tadl_us/busy_timeout_us`，删除无消费者的 `trst_us`；`NAND_TIME_*` 只作表初始化默认值。
- **下一步**：上板回归常规读/写/擦除、cache read/program、multi-plane、reset；负例确认 UOP 字段越界返回 INVALID。`fc_nand_design.md` 待上板通过后统一定稿，至少同步 timing B、CHECK_FAIL/CHECK_FAIL_CACHE 表、单 plane `OP_PROGRAM_PAGE_CACHE_FIRST`（50）与 `OP_COUNT=51`。
- **证据**：`include/nand/nand_protocol.h`；`src/nand/nand_fmc.c:nand_fmc_exec_delay_timing`；`src/nand/onfi_1_0.c:g_nand_operation_map` / UOP 数组；`docs/design/fc_nand_design.md:797-825`。
- **历史/验证**：
  - 2026-11-07 改为按 op timing 字段延时 → 未上板
  - 2026-11-07 用户明确设计文档待上板后定稿


#### RISK-NANDP-10 NAND_UOP_DELAY_TIMING 零值只在 debug 拦截

- **后果**：timing 表字段为 0 时 release 会静默执行 `delay_us(0)`，对应时序不等待 ⇒ NAND 时序不足，状态采样过早或命令/地址建立保持不足，可能静默写坏/读错。
- **缓解**：`nand_fmc_exec_delay_timing()` 用 `ASSERT_REJECT(delay_us_value != 0U, NAND_RESULT_INVALID_ARGUMENT, ...)`；debug 保留现场，release 拒绝而非静默 `delay_us(0)`。当前表被 UOP 实际引用的字段均非零。
- **下一步**：负例：构造选中字段为 0 的 timing profile，确认 debug 停机、release 返回 INVALID；上板回归常规读/写/擦除、cache 与 multi-plane。
- **证据**：`src/nand/nand_fmc.c:nand_fmc_exec_delay_timing`；`src/nand/onfi_1_0.c:g_nand_operation_map` / `NAND_UOP_DELAY_TIMING` 序列。
- **历史/验证**：
  - 2026-11-07 `ASSERT` 改 `ASSERT_REJECT` → 未上板

#### RISK-NANDP-11 timing 表名称/注释与实际口径相反

- **后果**：timing 表是 3.3V Mode 5 AC 参数（`t_rc_ns=20`、`t_wc_ns=20`，Table 29/31）；若命名/注释写成 Mode 0 典型值，后续按名字改成真正 Mode0（tRC=50ns）会使高速 FMC 变慢/时序错，或误以为低速 PCC 也来自该表。
- **缓解**：当前命名为 `g_nand_timing_3v3_mode5`；文件头写“一张 3.3V Mode5 AC 表 + 低速 PCC 常量”；表注释、`nand_fmc_timing_to_pcc` 注释和 tIR 注释一致；低速 PCC 由 `MT29F_LOW_PCC_*` 提供。改动只涉及重命名/注释，不改表值。
- **下一步**：上板对照 Mode 5 PCC 日志；定稿时同步 `fc_nand_design.md` 的 Table 29/31 说明。
- **证据**：`src/nand/devices/mt29f___g08a.c:16-19` / `:48-58` / `:79` / `:125` / `:344`；`include/nand/devices/mt29f___g08a.h:CONFIG_NAND_TIMING_MODE`。
- **历史/验证**：
  - 2026-11-07 重命名 + 注释同步 → `make all` / `make release` 通过，未上板

#### RISK-NANDP-12 多 target 的 bus ownership

- **后果**：`nand_target_get()` 只在任务开始时校验通路并返回 backend；`nand_board_select_target()` 不切 NCE/FMC bank；`fc_nand_start/step` 跨多轮，另一个 target 可在中间改变选择 ⇒ 命令/数据落到错误 target。
- **缓解**：B2-L1/L2 框架：`nand_board_target_s` 增加 `bank`；`nand_board_acquire_target/release_target` 做 per-bank ownership；dispatch task 增加 `target_acquired`（首次 start 获取、同步完成/失败即释放、异步事务在完成时释放）；同一 bank 被占用时返回 WAIT，不启动。当前 board 只有 `(0,0)`，single target 行为不变。
- **下一步**：仍需实际 NCE/R-B#/FMC bank 选择、per-bank HAL/timing，以及双 target A 写/B 读隔离上板测试；当前不能宣称多 target 已验证。
- **证据**：`include/nand/nand_board.h`；`src/nand/nand_board.c:nand_board_acquire_target` / `release_target` / `ownership_reset`；`src/fc/fc_nand_dispatch.c:fc_nand_dispatch_task_fc_start` / `fc_nand_dispatch_target_release` / `fc_nand_dispatch_lun_step`。
- **历史/验证**：
  - 2026-11-07 增加 per-bank ownership 框架 → 未上板

#### RISK-NANDP-13 timing mode 查询/设置的 release 边界不完整

- **后果**：`mt29f_set_timing()` 的 `ch/ce/mode` 仅 debug `ASSERT`，release 可越界写 `s_timing_mode` 或接受非法 mode；`mt29f_get_timing_mode()` 只 debug ASSERT ch/ce；`nand_get_timing_mode()` 若返回 `BYTE` 则无错误通道，也不先走 `nand_board_select_target()`。当前 CLI 用合法 die/ch/ce，engine 只传 0/4/5，暂不触发。
- **缓解**：按 H38 采用 status+出参：`nand_result_e nand_get_timing_mode(ch,ce,BYTE *mode)`；descriptor 回调和 `mt29f_get_timing_mode` 同步；`nand_get_timing_mode` 先 `nand_board_select_target()`；`mt29f_set_timing` 对 ch/ce/mode/fmc_hz 用 `ASSERT_REJECT`；CLI 两个调用点失败显示 `?`。
- **下一步**：上板：CLI `info`/`id` 正常显示 mode；负例用非法 ch/ce/absent target，确认 release 返回错误而不是越界；回归高速/低速切换。
- **证据**：`src/nand/devices/mt29f___g08a.c:mt29f_set_timing` / `mt29f_get_timing_mode`；`src/nand/nand_board.c:nand_get_timing_mode`；`include/nand/nand_device.h:nand_timing_mode_fn_t` / `nand_get_timing_mode`；`src/hil/cli.c:uart_cli_info` / `uart_cli_id`。
- **历史/验证**：
  - 2026-11-07 改 status+出参、`ASSERT_REJECT` → 未上板

#### RISK-NANDP-14 UOP 表 busy 启动后缺 tWB

- **后果**：`nand_fmc_exec_wait_ready()` 只要 R/B# 为高就返回 OK；随机读 `E0h` 后、cache 读 `31h`/`3Fh` 后、multi-plane `30h` 后等序列未插 TWB ⇒ R/B# 尚未拉低时立即 OK ⇒ 紧随的 `DATA_OUT` 读旧 cache/旧页缓冲 ⇒ **静默数据错误**。`g_nand_read_status_uops` 另缺 `70h` 后 `TWH`。
- **缓解**：A 组 11 处插入 `{NAND_UOP_DELAY_TIMING, NAND_TIMING_TWB}`：随机读 2 处（`E0h` 后）、cache 读 7 处（`31h`/`3Fh` 后）、multi-plane 带 DATA_OUT 1 处（`30h` 后）；`g_nand_read_status_uops` 在 `70h` 与 `DATA_OUT_STATUS` 间插入 `NAND_TIMING_TWHR`。B 组按 datasheet：`ECh`/`EDh`/`EEh` 地址后加 `TWB`，`EFh` 在 `DATA_IN` 后加 `TWB`，`7Ah` 地址后加 `TWH`；`RESET` 由 RISK-NANDP-15 处理。
- **下一步**：上板回归 sparse read（05h/06h）、cache read、multi-plane read、`OP_READ_STATUS`；负例用短/零延时 profile 确认读出的 CW 与写入不一致。
- **证据**：`src/nand/onfi_1_0.c` UOP 数组（随机读/cache 读/multi-plane 读/READ_STATUS）；`src/nand/nand_fmc.c:nand_fmc_exec_wait_ready`；`include/nand/nand_protocol.h:NAND_TIMING_TWB`。
- **历史/验证**：
  - 2026-11-07 A/B 组补 `TWB`/`TWH` → 未上板

#### RISK-NANDP-15 首次 RESET busy timeout 小于 datasheet 上限

- **后果**：MT29F4G08 Table 31 Note 3：设备空闲时第一次 RESET（FFh）最大 busy 1ms，此后最大 5µs。busy timeout 若取 500µs，上电首次 reset 会被判超时，导致 boot 复位失败/重试。
- **缓解**：`NAND_TIME_TRST_US` = `1100U`（首次 1ms + 10% 余量）；`g_nand_reset_uops` 在 FFh 后补 `NAND_TIMING_TWB`，再进入 `WAIT_READY`。
- **下一步**：上板冷启动和热复位各验证一次；确认 boot 不再出现 reset timeout。
- **证据**：`src/nand/onfi_1_0.c:NAND_TIME_TRST_US` / `g_nand_reset_uops` / `g_nand_operation_map[OP_RESET]`；MT29F4G08 Table 31 Note 3。
- **历史/验证**：
  - 2026-11-07 改 1100µs + FFh 后 `TWB` → 未上板

#### RISK-NANDP-16 tADL 放置位置错误

- **后果**：`tADL` 原放在 `11h` 后，MT29F4G08 Table 29 Note 2 定义为“最后一个地址周期的 WE# 上升沿 → 第一个数据输入的 WE# 上升沿”；单 plane program 无 `tADL`、多 plane/cache program 只在 `11h` 后延时 ⇒ 地址→数据之间没有延时，数据输入可能早于 `tADL`，编程数据/地址采样不可靠。板端二分结论：`tADL` 移到地址→数据之间无害；但删掉 `11h` 后那 1µs 会使双 plane 编程整页丢失（`block_multi_plane` 首读即 META）⇒ 该 1µs 是 `11h` 切 plane 后器件需要的切换等待，**不能删**。
- **缓解**：`TADL` 在每组 `ROW_2` 之后、`DATA_IN` 之前（`g_nand_normal_program_page_uops`、`g_nand_program_cache_uops`、`g_nand_program_page_issue_uops`、`g_nand_multi_plane_program_uops`、`g_nand_two_plane_program_cache_uops`、`g_nand_two_plane_program_cache_end_uops`）；`11h` 后延时删除；对应 op 的 `timing.tadl_us` 设为 `NAND_TIME_TADL_US`（1µs > 70ns）。`g_nand_set_features_uops` 的地址→数据也补 `TADL`；三张双 plane 表的 `11h` 后等待改为 `DELAY_TIMING NAND_TIMING_TWB` + `WAIT_READY`（先等 tWB 确保 busy 已生效，再轮询 R/B# 引脚回高；不插 70h，避免在两段 plane 地址之间插入总线命令）⇒ 独立 timing 字段不再需要。
- **下一步**：重跑 `block_cache_multi_plane` / `block_cache_multi_plane_cross_block` 确认 cache 双 plane 写恢复；已绿：`block_multi_plane`、`block_cache_multi_plane`、`block_cache_multi_plane_cross_block`；仍待回归：normal program / SET_FEATURES / H2testw 写校验、整页 `WITH_DATA_OUT` 读表（RISK-DP-30）。datasheet 已确认 `EFh` 在 `DATA_IN` 后 busy `tFEAT`（max 1µs），`TWB` 已补，`OP_SET_FEATURES` 的 700µs timeout 覆盖。
- **证据**：`src/nand/onfi_1_0.c` 上述 UOP 数组与 `g_nand_operation_map`（`OP_NORMAL_PROGRAM_PAGE` / `OP_PROGRAM_PAGE_CACHE` / `OP_PROGRAM_PAGE_CACHE_FIRST` / `OP_PROGRAM_PAGE_ISSUE` / `OP_OTP_DATA_PROGRAM` / `OP_*MULTI_PLANE*`）；MT29F4G08 Table 29 Note 2；RISK-DP-30。
- **历史/验证**：
  - 2026-11-11 轮询版上板：`/fc-nand/` 59/59 全绿（3 skip），含 `block_multi_plane` 92.24 ms、`block_cache_multi_plane` 80.52 ms、`block_cache_multi_plane_cross_block` 80.50 ms（debug_ut 横幅 12:56:46）
  - 2026-11-11 板端二分：`tADL` 移地址→数据无害；删 `11h` 后 1µs 致双 plane 整页丢失 → 加回
  - 2026-11-07 按 Table 29 移动 `TADL`、双 plane 表改 TWB + WAIT_READY → 未上板

#### RISK-NANDP-17 on-die ECC 未启用时的 tR timeout 边界

- **后果**：MT29F4G08 Table 33 给出 `tR_ECC` max **70µs**（内部 ECC 使能）；`NAND_TIME_TR_US` 若仍为 38µs，启用内部 ECC 后读等待会在 38µs 超时。
- **缓解**：`NAND_TIME_TR_MIN_US = 38µs` 只作 ISSUE 段 `min_busy_us`；`NAND_TIME_TR_US = 80µs` 作为读类 op 的 `busy_timeout_us`，覆盖 tR_ECC max 70µs。当前 on-die ECC 仍未启用。
- **下一步**：启用内部 ECC（feature 90h）时上板验证读路径；确认 80µs timeout 足够且不误超时。
- **证据**：MT29F4G08 Table 33；`src/nand/onfi_1_0.c:NAND_TIME_TR_MIN_US` / `NAND_TIME_TR_US`；`src/nand/devices/mt29f___g08a.c:FMC_NAND_ECC_DISABLE`。
- **历史/验证**：
  - 2026-11-07 拆分 `TR_MIN` / `TR` 常量 → 未上板

#### RISK-NANDP-18 SET FEATURES feature 01h 的 tITC

- **后果**：MT29F4G08 正文只说改接口时序后 device busy 最多 `tITC`，本 PDF 表 33/AC 表未给数值；ONFI 2.3a 及 Linux `nand_timings.c` 给出 `tITC` max = **1µs**（`1000000 ps`）。
- **缓解**：无需改 timeout；`OP_SET_FEATURES` 继续使用 `NAND_TIME_TPROG_US = 700µs`（远大于 1µs）。
- **下一步**：上板验证 Mode 0→5 切换；若换器件，按新器件 `tITC` 重新确认。
- **证据**：`src/nand/onfi_1_0.c:g_nand_set_features_uops` / `g_nand_operation_map[OP_SET_FEATURES]`；MT29F4G08 p.50；ONFI 2.3a / Linux `nand_timings.c`。
- **历史/验证**：
  - 2026-11-07 查证 `tITC` → 判定 700µs timeout 足够，无需改


### 已闭合（详情见 archive）

| ID | 日期 | 一句话 | 归档 |
|---|---|---|---|
| RISK-NANDP-09 | 2026-11-11 | 命令/地址窗口写缺 __DMB，字节可能被合并重排 | `docs/archive/known_risks_batches.md` |

---

## 2AJ. 平台返回状态

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-PLAT-13 | 待上板 | `dma_port_uart_tx_init` 失败返 SUCCESS、成功返 ERROR | 未上板 |

### 详情

#### RISK-PLAT-13 `dma_port_uart_tx_init` 返回语义反转

- **后果**：HAL DMA 初始化失败时返回 `FUNC_STATUS_SUCCESS`，成功时返回 `FUNC_STATUS_ERROR`。调用方 `usart_port.c` 恰好用 `dma_ready != 0` 判成功，所以当前「功能上能用」依赖了这个反转；任何按 0=成功语义使用该 API 的调用点都会把失败当成功。
- **缓解**：`dma_port_uart_tx_init` 为 0=成功（失败返回 `FUNC_STATUS_ERROR`）；`uart_tx_dma_init` / `dma_port_uart_tx_init` 返回类型为 `func_status_e`；`usart_port.c` 用 `== FUNC_STATUS_SUCCESS` 生成 0/1 `dma_ready`。
- **下一步**：上板验证 UART TX DMA 路径与 TXE 回退；确认 DMA 成功时 `dma_ready=1`、失败时回退。
- **证据**：`src/platform/stm32h743/dma_port.c` `dma_port_uart_tx_init`；`src/platform/stm32h743/usart_port.c` `dma_ready = (uart_tx_dma_init() == FUNC_STATUS_SUCCESS)`；`include/platform/dma.h`。
- **历史/验证**：
  - 2026-11-06 返回类型改 `func_status_e`、0=成功，调用点按相等判断 → 未上板

---

## 2AK. 构建档 / launch 配置

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-BUILD-01 | 待上板 | `DEBUG_OPT=-O0` 静默复用 -O2 对象 | 未上板 |
| RISK-BUILD-02 | 待上板 | launch 配置档位不符，产品/UT 共用 ELF 路径，易烧错镜像 | 未上板 |

### 详情

#### RISK-BUILD-01 `DEBUG_OPT=-O0` 静默复用 -O2 对象

- **后果**：`OBJ_DIR` 只按 `BUILD_TYPE` / `FC_NAND_TEST_ENABLE` / `UT` / `NAND_DEVICE` 分置，不含优化档；本工程没有「CFLAGS 变化 ⇒ 重编译」依赖 ⇒ `make tests DEBUG_OPT=-O0` 会继续链接 `obj_ut_*` 里 -O2 编出的陈旧 `.o`，产物仍是 -O2，排查汇编/时序缺陷时拿到错误档位。
- **缓解**：`DEBUG_OPT ?= -O2` 提前到目录配置之前；debug 下非默认档在 `OBJ_DIR` / `LIB_DIR` / `DEP_DIR` 加 `_O0` 一类后缀。`make -n tests UT=FC DEBUG_OPT=-O0` 确认对象目录为 `obj_ut_FC_O0` 且编译命令带 `-O0`。
- **下一步**：真正需要 -O0 定位时再跑一次全量 -O0 构建并上板；若加入其它优化档（`-Os` / `-O3`），确认后缀生成不产生非法目录名。
- **证据**：`config.mk` §2 `OBJ_OPT_SUFFIX` / `DEBUG_OPT`；`make -n tests UT=FC DEBUG_OPT=-O0`。
- **历史/验证**：
  - 2026-11-11 `DEBUG_OPT ?= -O2` 提前 + 非默认档对象目录加后缀 → `make -n` 确认 `obj_ut_FC_O0`，未上板

#### RISK-BUILD-02 launch 配置与实际构建档不符

- **后果**：launch 注释写「-O0」，2026-11-01 起 debug 实为 `-O2`；`postLaunchCommands: monitor halt` 与另一条配置行为不一致；产品固件与 UT 固件都写 `debug.elf` / `release.elf`，launch 无法从路径区分，按 F5 前未确认最后一条构建就会烧错内容（板端曾把 debug 镜像当 release）。
- **缓解**：注释改为 `debug(-O2+ASSERT)` / `release(-O2+NDEBUG)`；去掉 `postLaunch halt`，两条配置一致；`TARGET` 按 `FC_NAND_TEST_ENABLE` 分名；launch 扩成 8 条（4 产物 × 外部/WSL），分别指向 `debug_ut.elf` / `debug.elf` / `release_ut.elf` / `release.elf`。
- **下一步**：待上板：按 8 条配置逐一点 F5，确认横幅时间戳与所烧产物一致；release 配置只烧录/运行（strip 后无 `runToEntryPoint`/断点）。
- **证据**：`.vscode/launch.json`；`config.mk` §目标文件命名；`build/firmware/{debug,release}/bin/`。
- **历史/验证**：
  - 2026-11-11 修正注释、去掉 postLaunch halt、按配置分名并扩成 8 条 launch → 未上板

### 已闭合（详情见 archive）

| ID | 日期 | 一句话 | 归档 |
|---|---|---|---|
| RISK-BUILD-03 | 2026-11-11 | UT=FTL/UT=FC 的破坏性说明与构建条件不符 | `docs/archive/known_risks_batches.md` |

---

## 2AL. HIL UT

### 索引

| ID | 状态 | 一句话 | 最新验证 |
|---|---|---|---|
| RISK-UT-34 | 待后续 | HIL 契约覆盖缺口：`is_complete`、NULL、队列账本、flush 持久化 | 2026-11-13 上板通过 |

### 详情

#### RISK-UT-34 HIL 契约覆盖缺口

- **后果**：① `ssd_controller_is_complete` 未测（NULL 视为完成、flag 0/1）；② 状态机未测 NULL 守卫与 `READY->INITIALIZING` 非法转换；③ `host_queue` 未断言 owner / `used` / 释放后 request 清零，只测 tag 与表满；④ `submit` 未测 NULL controller/request、buffer NULL、TRIM BUSY/越界；⑤ 真实 I/O 只有 4 条，`write_read_roundtrip` 不 flush，flush 后持久化/跨重启未覆盖。后果：HIL 入口守卫、队列账本、flush 语义回归无判据；15/15 全绿不代表这些路径正确。
- **缓解**：已补 `is_complete`、状态机 NULL、host_queue owner/used/释放、submit NULL、TRIM BUSY、`write_flush_readback`，总数 15 例。
- **下一步**：上板跑 15 例；后续再补 buffer NULL / TRIM 越界 / 可做的跨重启。buffer NULL 见 `RISK-CTRL-04`；TRIM 越界可直接加用例；跨重启需把 `ftl_ut_common.c` 链入 `UT=HIL` 或走 `UT=` 全量构建。多 run/USB 槽留待多 die / PC 主机矩阵。**2026-11-13 用户决定：以上剩余缺口留以后检验，本轮不补用例**；测试设计见 `docs/design/hil_ut_design.md`。
- **证据**：`tests/unit/test_hil.c` `test_hil_is_complete` / `test_hil_trim_busy` / `test_hil_write_flush_readback` 等；`include/hil/controller.h:84`；`src/hil/host_queue.c:35-72`；`docs/design/hil_ut_design.md` §6。
- **历史/验证**：
  - 2026-11-13 设计文档补记：HIL UT 设计见 `docs/design/hil_ut_design.md`；本轮不补用例
  - 2026-11-13 用户裁决：以上剩余缺口留以后检验 → 本轮不补
  - 2026-11-13 13:21:29 镜像 `test /hil/` → 15/15，0 skip，10.56 ms（seed `0x5a4f21e4`）；残余缺口不变
  - 2026-11-11 补 6 类用例至 15 例 → 上板 15/15 通过（剩余覆盖缺口）

### 已闭合（详情见 archive）

| ID | 日期 | 一句话 | 归档 |
|---|---|---|---|
| RISK-UT-31 | 2026-11-11 | 12 用例缺注释，且头文件与源文件口径矛盾 | `docs/archive/known_risks_batches.md` |
| RISK-UT-32 | 2026-11-11 | HIL 等待循环缺 `timer_scheduler_poll()` | `docs/archive/known_risks_batches.md` |
| RISK-UT-33 | 2026-11-11 | teardown 超时残留 `bg_suspend=1` 与在飞后台 | `docs/archive/known_risks_batches.md` |
| RISK-UT-35 | 2026-11-11 | HIL 用例缺 `[EV]` 证据行 | `docs/archive/known_risks_batches.md` |
| RISK-UT-36 | 2026-11-12 | `/speed/ftl_rw` 长写失败：cache ring 满环形等待 + `bg_progress` 漏计 | `docs/archive/known_risks_batches.md` |

---
## 3. 类型 / 显式强转（H44）

> 本节条目已归档（见 `docs/archive/known_risks_batches.md` §2026-10-24）：RISK-TYPE-01（原归档，待上板）

- **RISK-TYPE-04（已闭合 / 上板 174/174，2026-11-12）**：① UT 强转清理已删 499 处（705→206，`tests/unit/`，不含 munit 框架）：`(void)` 加在 void 函数、`munit_assert_uint32` 右值 `(DWORD)`、`(int)` 包 BOOL/枚举配 `%d`、`(char)` 配 `%c`、`(QWORD)` 加宽、`(DWORD)sizeof`、常量/同型/加宽；BYTE/WORD accessor 断言改用 `munit_assert_uint8/uint16`，`%lu/%lX/%hhu` 与实参宽度不匹配处改为 `%u/%X/%d`。② **`paa_to_*` 已收窄**（`include/nand/nand_config.h`）：`paa_to_page/plane/lun→BYTE`、`paa_to_block→WORD`、`paa_to_cw→BYTE`，与 `nand_addr_s` 字段及 `pba_to_*`（`include/ftl/ftl_config.h:327-393`）对齐；原先生产侧 27 处隐式窄化（`addr_from_paa` 5 处；`fc.c` 15、`fc_pool.c` 4、`fc_command.h` 1、`ftl_fc_interface.c` 1、`fc_nand_dispatch.c` 1）全部变为同型/加宽。`NAND_PAA_TO_ROW` 删掉 page 的冗余 `(DWORD)`；plane/block/lun 的 3 个 cast 由同型变为合法的移位前加宽（H48）。测试侧同步把 `plane=%lu`→`%hhu`（4 处）、`cw=%hu`→`%hhu`，并删 `(BYTE)paa_to_*` 同型强转。③ `pba_make` 七形参全 `DWORD`（`include/ftl/ftl_config.h:450`）**判定不改**：生产调用点 0 强转；函数逐字段 mask；open SB 由 `alc_super_open` 保证每 plane 有块，closed SB 路径有 `super_block_has_plane` 守卫（`migrate.c:116/160/181/931/969/1186`），allocator 直接索引点亦全部守卫 ⇒ 当前哨兵 `FTL_BLOCK_NO_PLANE` 不会流入；且收窄 `block→WORD` 不能消除哨兵截断（0xFFFF 仍能装进 WORD），反会与 `nand_pba_fields_s` 的 DWORD 位域实参产生新窄化。可选廉价加固：`super_block_page_pba` 入口加 `ASSERT(super_block_has_plane(sb, plane))`（C12），未做。**后果**：类型契约已对齐，取值不变；收窄使代码段小幅变化（debug 276012 B / release 246492 B）。**已上板**：2026-11-12 `test` **174/174 通过**（3 skipped = `lifecycle/init_sequence` / `feature_write_read` / `chain_priority_lock`，FC 已初始化后按设计跳过，非失败），覆盖 `/fc-nand/` 全量（含 `addr_from_paa` / `NAND_PAA_TO_ROW` 路径）、`/ftl/pure/mem/address_roundtrip`、`/ftl/media/e2e/` 全量、`/hil/` 全量与 `/speed/` 两套。**证据**：`make all` / `make release` / `make tests` 0 error / 0 warning；板端 2026-11-12 `test` 174/174、wall-clock 343.33 s。

## 4. 跨模块 / 历史欠账（已在 memory，不重复展开）

| 主题 | 记录位置 | 要点 |
|---|---|---|
| 介质布局 v9 未上板 | memory `ftl-media-layout-v9-unverified.md` | 目录检查点/CP P0+P1、Journal multi-plane 擦、Wear 并入 CP；老介质首启格式化 |
| allocator 不变量 / 列表容量 | memory `allocator-invariant-api-v0p77.md` | `alc_super_close_live` 列表满静默丢弃 vs `add_closed_sb` ASSERT 不对称；阶段 3「让 FREE 只说真话」未做、须上板 |
| FTL 入口守卫欠账 | memory `ftl-entry-guard-debt-census.md` | 77 处未覆盖；不是活 bug，但未来重构可达即静默降级 |
| FTL UT / 异步套件计划 | `docs/design/ftl_ut_design.md`、memory `ftl-ut-async-plan-v0p75.md` | UT 已重建并上板（85 次运行）；异步套件（`/ftl/async/`、`/ftl-hil/`）仍只有计划、未实现 |
| MemManage / 栈指针被写坏 | memory `hardfault-open-v0p71.md` | 已按用户裁决降级关闭、只记录；`-fstack-protector-strong` 未做 |
| 擦除 op 18→17 未决 | memory `cmd-operation-18-to-17-open.md` | 固件侧实测零改写，原因未闭合 |
| IO 路线建议点 | `.claude/io_review_backlog.md` | 只登记不分析；编号 `IO-NNN` |
| `read_recovery` 0x89 保留地址 | memory `nand-feature-addr-table-mt29f4g08.md` | 用户明确不处理 |

---

## 5. 板端回归入口

历史“已修待上板”批次与 2026-10-24 收敛归档均在 `docs/archive/known_risks_batches.md`。
当前板端回归入口以 `docs/design/testing_guide.md` 为准；本表只保留仍成立的风险。

---

## 归档索引

> 历史归档条目已按模块回迁到上方各节的索引 / 详情或「已闭合」表；
> `docs/archive/known_risks_batches.md` 仅作历史对照（冻结，不再维护）。
## 判定归档（信息 / 已议）

> 非活跃风险，保留判定与重开触发，避免下轮重复分析；不是已闭合缺陷。

> 本节条目已归档（见 `docs/archive/known_risks_batches.md` §2026-10-24）：RISK-MIG-04（信息） · RISK-BBT-08（信息） · RISK-BBT-09（信息） · RISK-BOOT-19（信息） · RISK-BOOT-20（信息） · RISK-WL-06（信息） · RISK-WL-09（信息） · RISK-WB-05（信息） · RISK-BG-02（已议） · RISK-BG-03（信息） · RISK-BG-08（信息） · RISK-ARCH-01（信息） · RISK-ARCH-03（信息） · RISK-ARCH-04（已议） · RISK-ARCH-10（信息） · RISK-USB-04（信息） · RISK-ALLOC-01（信息） · RISK-MD-04（信息） · RISK-PLAT-06（信息，2026-10-24）

- **RISK-BBT-22（信息，2026-11-05 读码审计）**：`REBUILD_REQUIRED` 只随**最新有效 BBT record** 持久化（`reserved[1]`）；若最新 record 损坏/不可读，`md_ring_locate` 会回退到更旧的 record ⇒ 标志丢失，本应执行的 full metadata rebuild 被跳过。缓解：该标志只在「元数据日志块丢失」时置位，此时 CP/目录大概率也不可读，启动路径的其它检查可能兜底；但不等价。重开触发：看到日志块丢失日志后重启未出现 full rebuild 时，先查最新 BBT record 是否可读。证据：`src/ftl/bbt.c` `bbt_save` / `bbt_load`；`src/ftl/metadata.c` `md_ring_locate_fallback`。
- **TRIM / cancel 矩阵（信息，2026-11-05 审视）**：`ssd_controller_trim_service` 的 `FTL_AGAIN`/错误/超时路径与 `ftl_trim_begin` 的 1 s 无进展超时闭合，不会永久挂起；TRIM 仅 CLI 入口，前端串行化由 host gate（`UART_CLI_FLAG_BUSY` + `usb_msc_host_busy`）+ controller 单飞行 + `g_cli_pending.busy` 保证，`ftl_trim_step` 的 wbuf-drop「不失效旧 L2P」依赖该串行化（并发 TRIM 前 RISK-HOST-09 不可达）。`ssd_controller_cancel_flush` 只摘 flush job、不清 trim；当前无 `cancel_trim` 是范围决策（TRIM 只能靠超时/复位结束）。重开触发：接入 host UNMAP/TRIM 或给 CLI 加 abort。证据：`src/hil/controller.c` `ssd_controller_trim_service` / `ssd_controller_cancel_flush`；`src/ftl/ftl.c` `ftl_trim_begin/step` / `ftl_flush_cancel`。

- **RISK-DOC-04（待后续，2026-11-05 用户定「定稿后批量」）**：本轮代码改动已落地，但 `docs/design/` 的对应叙述仍是旧实现；**代码注释与 `known_risks` 已同步**，只有设计文档待批量更新。清单：① `ftl_design.md:1858-1867` 的 `md_ring_locate` ①~⑥（旧「第一个末槽无效块 = 游标块」→ 新「每个非满块降序找末条 + 全局最大 sequence + 写点由已校验胜者推」，RISK-BBT-18）；② 同文 `:1532` 「扫 2 块池」（引导环实为名义块 + 全部 shared spare，候选池 2..4 块）；③ 同文 `:1543-1544` BBT 异步 job「失败保留 dirty 下轮重试」（擦失败现为标坏 ring 块 + 重建候选池，RISK-BBT-19；另有同步/异步重叠守卫与 `dirty_gen` 快照，RISK-BBT-20）；④ 同文 `:2967-2972` 待入账预算（判据已移入 `ftl_confirm_service`，RISK-HOST-16）；⑤ 可选：`ftl_ut_design.md` 补「BAD 块末页读失败 + 后续块半写」负例规格（测试源由用户维护，R4）；⑥ `ftl_design.md` §3.10.5/§3.10.6 的同步等待「超时后调用者 release、迟到完成由 drain release」改为新的**命令归属**契约（`submitted` 出参，RISK-IF-17）。**后果**：设计文档与代码分叉，下轮读者可能按旧算法理解 ring 定位。**触发/下一步**：用户说「定稿 / 收尾 / 文档一起更新」时一次性回扫上述位置。

- **RISK-ARCH-13（已议，2026-11-03，有意偏离 C2）**：`ftl_confirm_write` 的 `ftl_page_desc_s` 构造在 `ftl_host_io_write_ok` 与 `ftl_confirm_service` **各写一次**，不抽 helper。用户判定：「只有频繁重复的才需要构造，两个调用点完全不必要，还付出函数本身开销」。**失败场景**：将来给 `ftl_page_desc_s` 增字段时只改一处 ⇒ 另一处漏填（静默用 0，编译期无声）。**缓解**：两处构造相邻且字段完全相同，改字段时按 grep 回扫。**重访触发**：出现第三个构造点，或描述符字段发生变化。证据：`src/ftl/ftl.c` `ftl_host_io_write_ok` / `ftl_confirm_service`。

- **RISK-TCM-11（信息，2026-10-31）**：release 的 ITCM 集（`.ssd_code_high`）**63,856 B / 65,536 B，余量 1,680 B**（drain stall 转储 +320 B、padding 屏障 +32 B、per-plane 水位/victim 平衡 +152 B、L2P WAL 闸门 +192 B）（v1.05 把 padding/cache-close 与 Journal 开机路径（scan/resume/replay）改放 `.ssd_code_fast`（Flash）后收窄；v1.06 读写跨 block 判据 + `alc_super_can_open` 再增 ~464 B。v1.04 曾到 64,352 B / 余量 1,184 B）。后果：余量再被吃掉时**链接期** `ASSERT(SIZEOF(.ssd_code_high) <= LENGTH(ITCM))` 会直接失败（不是静默），但排障时会阻塞构建。下一步：下次把 release-only 模块（journal / allocator / metadata / migrate 等）移出 ITCM 集之前，先复核本余量；不要靠删热路径函数换空间。证据：`arm-none-eabi-size -A build/firmware/release/bin/release.elf`；`linker/stm32h743xx_flash.ld` 的 ITCM 段与 ASSERT。

- **RISK-DOC-02（已改，2026-10-31，原为注释表述不准）**：`src/ftl/cache_batch.c:243-244` 写「同 block 是本仓的保守选择（不跨块），跨块直接回退普通读——不是器件强制」。**前半正确**：datasheet 原文「The READ PAGE CACHE SEQUENTIAL (31h) command **can be used to cross block boundaries** ... Do not issue 31h to cross die (LUN) boundaries」（本地摘录 `docs/reference/datasheet_mt29f_cache_ops.md` §2）⇒ 同 block 确实是本仓保守选择，不是器件强制。**后半不准**：断点后 drain 循环 `continue`（`src/ftl/ftl.c:2151-2154`），剩余 LBA 会**重新 START 一条 cache read**，不是回退普通读（只有下一次 START 条件不满足时才回退）。**失败场景**：只在维护者按注释误解"跨 block 必须回退普通读"时把行为描述错，不影响数据正确性（判据本身保守且安全）。已改（2026-10-31）：这两行注释已改为「46h 无地址、由器件自增；31h 可跨 block（同 die，禁止跨 die/LUN）」并把断点后行为写清楚；跨 block 续读的实现登记为 RISK-CACHE-20。证据：`src/ftl/cache_batch.c:283-284`、`src/nand/onfi_1_0.c:1028-1030`、`src/ftl/ftl.c:2151-2154`、`docs/reference/datasheet_mt29f_cache_ops.md`。


## 已闭合

> 各模块末尾的「已闭合」表 + 本文末尾的全局表是唯一索引；完整详情已移入
> `docs/archive/known_risks_batches.md`（历史批次 2026-11-12 搬迁；新条目追加到文末）。
> 重开时按新条目重新登记，不复活旧 ID。

| ID | 日期 | 一句话 | 归档 |
|---|---|---|---|
| RISK-DOC-01 | 2026-11-13 | 删除 ftl_api_usage.md，API 以 include/ftl/*.h 为准 | docs/archive/known_risks_batches.md |

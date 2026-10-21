# FTL 模块 handle 契约

> **性质**：FTL 内部模块的接口设计文档，不是 host API。
> **范围**：FTL 主调度与模块的边界、各模块 `*_handle()` 的输入/输出、时间与超时、
> 多 LUN/owner 路由。设计已在代码落地（Phase 0~8，未上板）；**状态口径以
> `ftl_design.md` §7.1 与 `known_risks.md` RISK-ARCH-06/10/11 为准**，本文 §12 仅保留
> 落地时的清单。
> **结论先行**：共享数据面账本保持共享 device context；模块只拥有执行状态
> （FSM / 单飞行 / 计时器 / 进度计数）；主流程通过 typed
> `*_handle(module_handle, trigger, status)` 推进模块，不读模块内部 phase。
> SPDK FTL 的 `spdk_ftl_dev` + `core_poller` + 模块 `process` 是同构参照
> （`docs/reference/pages/spdk_ftl.html:175-188` 确认模块划分与 `reloc`；
> 结构级细节来自 SPDK upstream，按 R10 标为外部对照）。

---

## 1. 为什么要做、做到什么程度

当前耦合集中在四处（详见 `docs/design/ftl_design.md` 各模块章）：

1. **共享数据面事务由 `ftl.c` 代管**：Journal→L2P→BST 的顺序、`valid_count`、
   读失败/写失败策略、`l2p_defer` 侧信道。
2. **pending/groups 被多个模块直摸**：`ftl.c` 扫 `pending[]`/`groups[]`，
   RR 直接读写 pending 槽，wbuf 直接扫 `groups[]`。
3. **跨 job 字段直读/直写**：migrate 读 `journal->handle.phase/need_erase/
   current_entry` 与 `write_fail_pending`；checkpoint 直接写
   `journal->merge_floor`；l2p owner 回调返回整个 `ftl_context_t*`。
4. **任务表/定时器/观测计数散落**：13 项固定表 + 掩码位=表下标；
   `cp_timer_start` / `rr_last_scan_ms` / `gc_eval_last_ms` 写在主上下文。

**要解耦的**：

- 模块执行状态（FSM、单飞行、计时器、阈值、进度计数）；
- 模块间字段直读/直写；
- pending/groups 直摸；
- 固定任务表与掩码下标。

**不能解耦的**（数据面本质约束）：

- 共享账本：L2P cache、BST/allocator、journal 持久性边界、metadata directory、
  pending/groups、完成表；
- Journal→L2P 写序、完成时重查 L2P、`valid_count` 一致性；
- host 先于后台的顺序、must-run liveness 集、各模块单飞行。

**结论**：解耦的是“执行状态与调度”，不是“共享账本”。模块可以拥有自己的
handle 和状态机，但 L2P/BST/allocator/journal 水位仍是单点共享。

---

## 2. 主流对照

| 项 | 主流形态 | 本仓选择 |
|---|---|---|
| device 对象 | 一个 per-controller/per-FTL 的共享对象（SPDK `spdk_ftl_dev`） | `ftl_context_t` / `ftl_storage_t` 共享 device context |
| 模块状态 | 模块子结构/指针挂在 device 上 | 各模块 handle 挂在 `ftl_context_t`（或模块上下文内） |
| 调度 | 固定顺序 poller 调模块 `process/run` | `bg_wiring.c` 任务表 + typed 适配器 |
| 模块入口 | typed process（SPDK）或 ops/vtable（Linux/Zephyr） | **typed module pointer**；不引入 vtable |
| 完成回调 | op tag / 队列回调到模块 | `g_op_callbacks[]` + ctx = 模块 handle/line |
| 超时 | NAND 引擎 per-op + 固件 per-command + 看门狗 | NAND/FC 引擎 per-op + 模块 ms deadline + IWDG liveness |
| GC 搬移 | `reloc` 模块，通常 per die/LUN | `migrate` 模块，严格 per-LUN（跨 LUN 未实现） |

**明确非主流、但本仓有意的选择**：

- RR 保留 UNC/CRC desc 链、由 FTL 完成处理函数做链手术（本仓自拟；主流通常
  以 LBA/PBA 重新下发新命令 + 新 buffer）；
- 每实例 FTL，而非单 device FTL（部分非主流；共享 L2P 已跨实例）；
- 无 supervisor / 无 per-engine watchdog（当前简化；真实主控常有多层超时/健康监控）；
- 无跨 LUN 条带 / 跨 LUN GC（性能缺口，登记为后续项）。

---

## 3. 统一 handle 契约

### 3.1 状态与上报

```c
typedef enum {
    FTL_MODULE_IDLE = 0,
    FTL_MODULE_RUNNING,
    FTL_MODULE_WAITING,   /* 等外部资源/完成 */
    FTL_MODULE_BLOCKED,   /* 需要上层动作 */
    FTL_MODULE_FAILED,
} ftl_module_state_t;

typedef struct {
    ftl_module_state_t state;
    bool     critical;    /* must-run liveness */
    bool     wants_run;   /* 下一轮是否还需要被调 */
    uint32_t progress;    /* 模块自定单调计数 */
    uint32_t next_ms;     /* 0 = 随时；否则最早下次调用时间 */
    int32_t  error;       /* FTL_* / FTL_TIMEOUT 等 */
} ftl_module_status_t;
```

约定：

- `*_handle()` 每次只推进一步（最多提交一条命令）或跑完一段纯计算；
- 提交即返回，不阻塞、不嵌套泵；
- 状态/进度只走上报结构或纯查询，不让外部读内部 phase；
- 模块自持单飞行；主流程不窥探 `active/phase/waiting/stage`。

### 3.2 下发信息分类（不要传定值）

| 类别 | 例子 | 承载 |
|---|---|---|
| 编译期常量 | `CW_NUM` / `PLANE_NUM` / `PAGES_PER_BLOCK` / `FTL_BLOCK_TOTAL`、`GC_EVAL_INTERVAL_MS` / `WL_EVAL_INTERVAL_MS` / `RR_SCAN_INTERVAL_MS` / `CP_TIME_INTERVAL_MS`、`FREE_POOL_TARGET/EMERGENCY`、`BG_*_FLUSH_COUNT`、`L2P_CACHE_SIZE` | 宏，模块自己 include（H4） |
| 只读策略表 | 档位 → budget/flush_max、GC 权重 | 模块内 `static const` 表，按 `load_level` 索引 |
| 每实例不可变配置 | `topo` / `ch,ce,lun` / 几何相关阈值 | `*_init` 绑定进 handle；**不用可变全局**（多 LUN 会串，H10） |
| 动态运行时量 | `now_ms`、`load_level`、`free_blocks`、`pending/group`、`journal_seq`、`total_writes`、`emergency`、`force`、`error` | 传参、owner API 查询或只读实例字段 |
| 事件/上下文 | trigger request、UNC desc 链、`rr_owner_t` | 逐次传入 |

通用 `ftl_bg_info_t` 收敛为：

```c
typedef struct {
    uint32_t now_ms;
    uint8_t  load_level;  /* IDLE / NORMAL / BUSY */
} ftl_bg_info_t;
```

`free_blocks` / `pending` 等由需要它的模块通过 allocator / pending 的 **owner API** 查询，
或放进该模块自己的 `*_trigger_t`。

### 3.3 入口形态（已定）

- **FTL 上下文挂模块指针**：`ftl->checkpoint.handle`、`ftl->journal`、
  `ftl->migrate.handle`、`ftl->rr.handle`、`ftl->wbuf.handle`、
  `alloc->bbt.handle` 等；
- **模块 API 用 typed 入口**：
  `*_handle(module_handle_t *, const module_trigger_t *, module_status_t *)`；
- **不把整个 `ftl_context_t *` 传进模块**：handle 只存实际需要的窄上下文指针
  （如 `alloc` / `journal` / `cache` / `metadata_dir` / `pending` / `migrate`），
  跨模块操作走 owner API，不经过 `*_port_t` 结构体；
- **调度器边界保留统一适配器**：`bg_task_t.run(ftl)` 是唯一的“通用入口”，
  适配器内部转 typed 调用；
- **不引入 per-module vtable / 通用 `void *` 模块 API**：固定 8 个 handle，
  typed 更安全；需要多实现/运行时选择时再评估。

---

## 4. 各模块 handle 专项

### 4.1 BBT

```c
void bbt_handle(ftl_allocator_t *alloc, const ftl_bg_info_t *info,
                bbt_status_t *out);
```

- 性质：dirty 驱动；`bbt_mark_bad` / `bbt_note_dirty` 仍是同步数据面 API。
- 上报：`state/phase`、`dirty`、`dirty_gen`、`bad_count`、`sequence`、
  `write_idx`、`progress`、`wants_run`、`next_ms`、`timed_out`、
  `timeout_phase`、`error`。
- 时间：仅提交背压；无周期。
- 上下文：allocator 池操作、metadata ring（bbt 与 allocator 同属块管理层，直接调 owner API）。
- 归属：异步 FSM 在 `bbt_handle_t`；池游标可留在 `bbt_state_t`。
- 禁止：FTL 读 `alloc->bbt.handle.phase/waiting`。

### 4.2 Journal

```c
void journal_handle(l2p_journal_t *journal, const ftl_bg_info_t *info,
                    journal_status_t *out);
```

- `l2p_journal_t` 是模块上下文；`journal_handle_t` 是其中的异步 job 子状态。
- 上报：`state(IDLE/WRITE_WAIT/ERASE_WAIT/RING_FULL/FAILED)`、`sequence`、
  `write_failed`、`ring_full`、`erase_pending`、`current_block/page/entry`、
  `flushed_entries`、`progress`、`wants_run`、`next_ms`、`error`。
- 查询（替代外部直读）：`journal_is_idle`、`journal_can_erase_victim`、
  `journal_sequence`、`journal_watermark`、`journal_write_failed`。
- 事件 API：`journal_append`、`journal_set_merge_floor(seq)`、
  `journal_clear_write_failure(seq)`、`journal_truncate_plan/commit`、
  `journal_flush_page`（同步屏障）。
- 移入 handle：写游标、`sequence_counter`、`page_buf`、`merge_floor`、
  `pre_erased_block`、`bad_mask/ring_full`、`write_fail_*`。
- 上下文：`journal->alloc`；坏块查询/标坏走 `allocator_block_is_bad` / `bbt_mark_bad`
  （不再有 `bad_block_port`）。replay 记账仍走 `journal_replay_block_ctx_t`。
- 禁止：migrate 读 `journal->handle.phase/need_erase/current_entry`；
  checkpoint 直接写 `journal->merge_floor`。

### 4.3 Metadata

```c
void metadata_handle(metadata_dir_cache_t *dir, const ftl_bg_info_t *info,
                     metadata_status_t *out);
```

- 事件驱动：`reclaim_pending` / `compact_wanted` 由 Directory 函数置位，
  通常不需要 trigger。
- 上报：`state/phase`、`victim`、`moved`、`reclaimed`、`compact_wanted`、
  `anchor_ok`、`reclaim_pending`、`progress`、`wants_run`、`next_ms`、
  `timed_out`、`timeout_phase`、`error`。
- 时间：仅提交背压；无周期。
- 上下文：`dir->alloc`、`dir->total_writes`（窄指针）；allocator 操作走 owner API；
  Anchor ring 仍由 metadata 自己管理。`dir->handle` 是异步 substate。
- 禁止：FTL 读 `dir->handle.phase/waiting`；用 `metadata_dir_busy`。
- 完成回调：`ftl_metadata_callback` 只记录结果 + 改 phase，不推进状态机。

### 4.4 Migrate（GC / WL / predict 共用）

```c
void migrate_handle(migrate_handle_t *h, const ftl_bg_info_t *info,
                    migrate_status_t *out);
void migrate_trigger(migrate_handle_t *h, migrate_reason_t reason);
```

- 每轮 must-run：先服务在飞 job，再按 `load_level != BUSY` + 自持节拍评估新工作。
- `migrate_trigger` 只登记紧急/显式原因（如 wbuf 分配不到页对的 urgent GC）；
  `mig_begin_gc/wl/predict` 收为模块内部实现。
- victim 由 handle 通过静态 policy 函数选（`gc_select_victim` / `wl_select_victim` /
  `rr_predictive_scan`；不是运行期端口）。
- 上报：`state`、`kind(GC/WL/PREDICT)`、`migrated`、`erased`、`aborted`、
  `erase_failed`、`progress`、`wants_run`、`next_ms`、`timed_out`、
  `timeout_phase`、`error`；查询 `migrate_is_active`、
  `migrate_reclaimed_count`、`migrate_novictim_count`。
- 时间：handle 自持 GC 10ms / WL 5s / predict 1s 节拍；等 L2P/Journal 用
  `FTL_WAIT_TIMEOUT_MS`；提交背压用 `FTL_SUBMIT_TIMEOUT_MS`。
- 上下文：`alloc` / `journal` / `cache` / `total_writes` / `topo` 五个窄指针；
  policy（选块/ECC/退役）仍是编译期静态调用。
- 禁止：FTL 读 `active/phase`；读 journal handle 字段；直接调 `mig_begin_*`。

### 4.5 Read Recovery

> 本模块的 desc 保留方案是**本仓自拟**（非主流；主流通常以 LBA/PBA 重新下发
> 新命令 + 新 buffer）。保留它是有意的取舍；FC 所有权契约与资源上界见下。

**完成处理函数（FTL 侧）**

1. 识别命令类型：只处理读；写错误走原路径。
2. 按错误优先级分类：
   1. `NAND_FAIL` / `TIMEOUT` → 整子操作 `IO_ERROR`，不启动 RR；
   2. `MATA_ERROR` / `RAID_ERROR` → 同上；
   3. `UNC` → 可恢复，摘链交 RR；
   4. `CRC_ERROR` → 可恢复，摘链交 RR；
   5. `READY` / `PROTECTED` / `UNUSED` → 无错误。
   任一 desc 命中 1/2 ⇒ 整子操作失败、不启动 RR。
3. 只收集 UNC/CRC desc，按 `(ch, ce, lun, block, page, plane, cw)` 排序，
   同 page 的 plane 相邻。
4. 逐 desc 摘链：修复原链 `next_id`；`buf_own=FC` 的 buffer 还池，
   `buf_own=upper` 留给 pending/上层；`buffer_id = FC_BUFFER_ID_INVALID`；
   `paa/config/meta/status` 保留。
5. 原 command 正常 release。
6. `rr_trigger(&ftl->rr.handle, &owner, sorted_head, count)`。

**FC 只提供机制原语**（策略在 FTL 完成处理函数）：

```c
fc_descriptor_t *fc_descriptor_get(fc_descriptor_id_t id);
fc_status_t fc_descriptor_set_next(fc_descriptor_id_t id, fc_descriptor_id_t next);
fc_status_t fc_descriptor_set_buffer(fc_descriptor_id_t id, fc_buffer_id_t buf);
fc_status_t fc_descriptor_buffer_release(fc_descriptor_id_t id);
fc_status_t fc_descriptor_release(fc_descriptor_id_t id);
void        fc_descriptor_chain_release(fc_descriptor_id_t head);
```

**RR 入口与上报**

```c
void rr_trigger(read_recovery_handle_t *h, const rr_owner_t *owner,
                fc_descriptor_id_t head, uint16_t count);
void rr_handle(read_recovery_handle_t *h, const ftl_bg_info_t *info,
               rr_status_t *out);
```

`rr_owner_t`：`owners` / `pending_idx` / `upper_buf` / `lba` / `n_cw`。
`rr_status_t`：`stage`、`waiting`、`all_ok`、`retries`、`planes_recovered`、
`planes_failed`、`desc_count`。

**RR 内部**

1. 遍历保留 desc，按 `paa` 的 plane 分组；
2. 每个 desc 重新申请 buffer，写回 `buffer_id`；
3. 同 plane 挂链；两个 plane 都有 UNC 且 retry mode 相同才合 multi-plane，
   否则按 plane 分开发单 plane 读；
4. 重读 → 失败换 retry mode / 邻居页；成功把 CW 数据写回 `upper_buf`；
5. 成功/失败都释放保留 desc 链；成功通过 pending owner API 结算组，
   失败报 `DONE_IO_ERROR`。

**资源上界**：RR 单飞行，最多保留一次失败读的 UNC/CRC desc
（≤ 2 plane × `CW_NUM` = 8）；buffer 重新申请，不长期占用。

### 4.6 Checkpoint

```c
void checkpoint_handle(checkpoint_handle_t *h,
                       const checkpoint_trigger_t *trigger,
                       checkpoint_status_t *out);
```

- 统一为 trigger + handle；`checkpoint_begin` 不再是公开运行时入口。
  boot/format 的 `checkpoint_write_initial`、开机
  `find_latest/probe_media_status/load_*` 保持独立同步入口。
- FTL 汇总触发信息后传入：`{now_ms, total_writes, journal_seq,
  journal_watermark, emergency, force, reason}`。
- 上报：`state/phase`、`active`、`journal_seq`、`flushed_regions`、
  `anchor_pages`、`header_written`、`truncated`、`fail_phase`、`progress`、
  `wants_run`、`next_ms`、`timed_out`、`timeout_phase`、`error`。
- 查询：`checkpoint_is_active`；外部不读 `active/phase`。
- 移入 handle：`ftl->cp` 游标、`last_cp_ms/next_run_ms`。
- 上下文：`cache` / `journal` / `metadata_dir` / `alloc` / `topo` 五个窄指针；
  `total_writes` 原端口字段无人读取，已删除。
- 时间：`CP_TIME_INTERVAL_MS` 是宏；等 flush/anchor 与提交背压用 ms deadline。

### 4.7 L2P cache

```c
void l2p_handle(l2p_cache_t *cache, uint32_t dim,
                const ftl_bg_info_t *info, l2p_status_t *out);
```

- 不是单 job：多行状态机 + 全局 credit。
- **只做后台脏行写回**：按 `load_level` 查 `static const` 表决定 `max_kick`；
  load 仍由 `l2p_cache_ready/read` 的 miss 需求驱动。
- 上报：`load_inflight`、`flush_inflight`、`dirty_regions`、`resident_lines`、
  `thrashing`、`miss_pct`、`evictions`、`wants_run`、`error`。
- 时间：后台不需要时间；NAND 完成超时归 FC；同步逃生口
  `l2p_*_sync` 自己用 drain deadline。
- 上下文：owner 回调（不是模块端口）返回 `{dim, metadata_dir, alloc, topo}`；FC async read/write；
  metadata directory；同步逃生口的 service pump。
- 禁止：FTL 遍历 `cache->lines[]`、直读 `flush_inflight`/`thrashing`。
- 完成回调：`ftl_l2p_callback` 不变，ctx 仍是 `l2p_cache_line_t *`。

### 4.8 Wbuf

```c
void wbuf_handle(wbuf_handle_t *h, const wbuf_trigger_t *t,
                 wbuf_status_t *out);   /* out 可为 NULL：只关心返回值时丢弃状态上报 */
```

- 事件驱动：`APPEND(lba, data, n, gi)` / `FLUSH` / `DROP(range)` / `RESET` /
  `MAYBE_FLUSH_IDLE`。
- 上报：`state(IDLE/OPEN/FLUSHING/FAILED)`、`open`、`occupied`、`owners`、
  `flushed_pages`、`drop_count`、`wants_run`、`timed_out`、`error`。
- 时间：仅提交背压。
- `wbuf_handle` 的 `out` 为可选出参：调用者只判断返回值时传 NULL，不分配 dummy status。
- 上下文：`alloc` + `ftl_pending_table_t *pending` + `migrate_handle_t *migrate`；
  直接调 `pending_*` / `migrate_trigger(GC_URGENT)` / FC 建链。
- 移入 handle：`ftl->wbuf` 的 data/occupied/open/base/owners。
- 禁止：直接扫 pending 表的 `groups[]`（走 `pending_group_window_pending`）；
  直接结算 pending；分配页对失败时自己起 GC。
- 完成：保留 `FTL_OP_HOST_IO`；wbuf 暴露 `wbuf_complete()`；
  Journal→L2P→BST 事务仍由 pending confirm 服务承担。

---

## 5. 不是 handle 的模块

| 模块/功能 | 定位 |
|---|---|
| GC 策略 `gc.c` / WL 策略 `wear_leveling.c` | 静态 policy 调用，由 migrate handle 调用；不拆 `gc_handle`/`wl_handle` |
| allocator / BST | 共享数据面服务；唯一内嵌异步 job 是 BBT |
| pending / groups | 共享数据面服务；含 write-confirm park |
| completion table / op dispatch | 基础设施；`g_op_callbacks[]` 是模块回调注册表 |
| wear table / 磨损计数 | 随 CP 快照持久化 |
| L2P owner 路由 / 多 LUN | 基础设施 |
| FC/NAND 层 | FTL 之外；操作超时已在引擎 |
| PFR | boot orchestrator，无 `*_handle()` |

### Confirm（write-confirm park）→ pending 服务

```c
void pending_confirm_park(uint16_t pending_idx, uint32_t owners);
void pending_confirm_service(const ftl_bg_info_t *info);
```

- 停泊列表归 pending 模块；FTL 不再扫 `pending[]` 找
  `PENDING_OP_WRITE_CONFIRM`。
- 事务顺序：Journal append → L2P write → BST valid ± →
  checkpoint watermark → total_writes；先取 owners 快照，再 pending 结算。
- 时间：等 L2P ready 用 ms deadline；超时 complete group `IO_ERROR`、
  释放 pending、LOGE。
- TRIM 留在 `ftl.c`，不共用 confirm 事务；两处顺序变更必须同步
  （C2/R9 风险）。

### PFR（boot orchestrator）

- 顶层入口：`ftl_media_check_format` / `ftl_media_format` /
  `ftl_boot_recovery`；fresh/clean/PF 子流程与 full metadata rebuild 原语收为 static/私有头。
- 输入：已绑定 handles / 窄上下文 + `need_format` + media status。
- 输出：`ftl_boot_result_t`。
- 时间：只受 NAND 操作超时约束。
- 穿透收口：`block_words` → `checkpoint_block_snapshot()`；`l2p_live[]` →
  `metadata_live_count()`；allocator 直读 → 查询；cache line 直写 →
  `l2p_cache_invalidate_line()`。

---

## 6. 时间与超时

### 6.1 谁需要时间

| 功能 | 需要时间？ | 用途 |
|---|---|---|
| Checkpoint | 是 | T3 周期 + 等 flush/anchor + 提交背压 |
| GC 求值 / WL / RR 预测 | 是 | 10ms / 5s / 1s 节拍门 |
| Migrate | 是 | 等 L2P/Journal + 提交背压 |
| Journal | 是 | drain 超时 + 提交背压 |
| Read Recovery | 是 | 等 L2P/pending + 提交背压 |
| Metadata | 仅提交背压 | 事件驱动，无周期 |
| BBT | 仅提交背压 | dirty 驱动 |
| Wbuf | 仅提交背压 | 事件驱动 |
| L2P cache | 仅同步逃生口 | 后台无周期；NAND 超时归 FC |
| allocator/BST/pending/completion | 否 | 纯数据面/账本 |
| PFR | 否 | boot 同步编排 |

### 6.2 四层归属

1. **NAND/FC 操作超时**：NAND/FC 引擎（DWT + 轮询计数双条件，
   `NAND_RESULT_TIMEOUT` / `FC_CW_STATUS_TIMEOUT`）；FTL 只消费完成。
2. **FTL 模块等待**：等待方 handle 拥有 ms deadline 与安全收尾。
3. **周期触发**：模块 handle 的 `last_run_ms` / `next_run_ms`。
4. **系统 liveness**：IWDG 30s；不在 ISR 喂狗，不替代业务超时。

### 6.3 计时口径

- 统一 **ms 绝对 deadline**，不用 rounds（rounds 随主循环速率漂移）。
- 建议常量（本仓取值，须上板标定）：
  `FTL_WAIT_TIMEOUT_MS = 50`、`FTL_SUBMIT_TIMEOUT_MS = 10`、
  `FTL_GROUP_DRAIN_TIMEOUT_MS = 100`。
- 模块 deadline 必须 > NAND 超时 + 余量；NAND 已返回 TIMEOUT 时不再叠加。
- 上报：`ftl_status_t` 增加 `FTL_TIMEOUT`（值 3，语义与 NVMe timeout /
  `-ETIMEDOUT` 同构；**本仓状态值一律非负**），模块 status 带 `timeout_phase`。
- 命令槽：NAND 保证在飞命令必有完成；超时模块不得擅自释放可能仍在飞的命令，
  迟到完成走 orphan 守卫。

### 6.4 检查放在哪

- 时间状态在 handle；检查与动作在 `*_handle()`，由主循环/poller 调用。
- 定时器 ISR 只维护时基/置事件标志：不调 handle、不遍历状态机、不 LOG、
  不做资源清理（H21）。
- **本仓决定：不加模块级 liveness supervisor**；主循环轮询 + 模块 deadline +
  IWDG。不新增定时器 ISR。

---

## 7. 多 LUN / owner 路由

- **handle 归属**：除 L2P cache 外，全部 per-instance（每个 `(ch,ce,lun)`
  一份）：checkpoint / journal / metadata / migrate / RR / wbuf / bbt。
  不得用可变全局 handle。
- **L2P cache**：按 `(ch,ce)` 共享，跨 LUN；`l2p_handle` 逐实例调用并带
  owner `dim`，只 flush 自己 owner 的脏行。
- **owner 回调（不是模块端口）**：

  ```c
  typedef struct {
      uint32_t               dim;
      metadata_dir_cache_t  *metadata_dir;
      ftl_allocator_t       *alloc;
      ftl_topology_t         topo;
  } l2p_owner_t;
  ```

  不再返回整个 `ftl_context_t*`。
- **全局 region 单漏斗**：`global = local * FTL_DIM_TOTAL + dim`；只在
  `l2p_global_to_local_region` / owner 回调转换（C9）。
- **调度**：`ftl_service_pump()` 全局一次；逐实例 `ftl_service` 调自己的
  handle；共享 L2P 按 owner/dim 调用。
- **完成路由**：命令 `ch/ce/lun` → 实例；ctx 是模块 handle/line；回调只用
  handle / 窄上下文。
- **初始化顺序**：设备级 `l2p_cache_init` / `completion_table_init` /
  owner 路由注入，先于逐实例 `ftl_init` + PFR。

### 7.1 GC/migrate 跨 LUN 现状

**不支持，且是刻意的**：

- `mig_lba_usable` 要求 `lba_routes_to(lba, ftl->topo)`（`migrate.c:553-557`）；
- 目标页来自本实例 `allocator_alloc_pair(&ftl->alloc, ...)`（`:524`）；
- allocator 在 init 绑定本实例 `ch/ce/lun`（`allocator.c:26-34`）；
- 源页 `super_block_page_pba(ftl->topo, &m->victim, ...)`（`:194,536`）；
- `../archive/multi_lun_generalization.md:26,358-359` 明确跨 LUN 条带/GC 不在当前范围。

跨 LUN 需要设备级 relocation 引擎 + 跨实例事务（两套 allocator/BST/
journal/checkpoint 账本同步），不是改 `migrate_handle_t` 入参能解决的。
**已登记为后续性能项**（见 `known_risks.md`）。

---

## 8. 字段所有权与单写者

| 字段/资源 | owner（唯一写者） | 读者/接口 |
|---|---|---|
| L2P cache / region 映射 | `l2p_cache.c` | `l2p_cache_read/ready/write`、owner 回调 |
| BST / allocator / 池 | `allocator.c`、`bst.h` | `allocator_*`、`ftl_block_*` |
| journal 持久性状态 | `journal.c` | `journal_append` / 查询 |
| metadata directory | `metadata.c` / `metadata_job.c` | `metadata_dir_*` / `metadata_dir_busy` |
| pending / groups | `pending.c` | `pending_*`（含 `pending_view_op` / `pending_group_window_pending`） |
| completion table | `completion_table.c` | `completion_table_*`、`g_op_callbacks` |
| 各 job FSM | 各模块 handle | status / 查询 |
| 调度定时器 | 各模块 handle | `now_ms` + status |
| 观测计数 | 各模块 handle | status / log |

**规则**：每个共享字段只有一个写者；其他模块只读通过 owner API 或只读实例字段，
写必须走 owner API（不再有 `*_port_t` 结构体）。
`ftl_context_t` 是 device 描述符，不承载业务状态。

### 8.1 `ftl_context_t` / `ftl_storage_t` 布局：内嵌 vs 挂载指针

**主流不是“全部指针化”**。SPDK `spdk_ftl_dev` 也是混合的：`nv_cache`、
`writer_user/gc` 等小/热状态内嵌，`l2p` / `reloc` / `valid_map` 用指针。
判据是：

1. **大小与放置域**：是否需要落特定 SRAM/AXI 段；
2. **生命周期**：完成回调要持有到命令结束 ⇒ 静态存储，不能是栈；
3. **共享 / 可选 / 后端相关**：共享或可替换的才需要指针；
4. **访问热度**：每个 host IO 都摸的热数据优先内嵌，避免多一层间接。

**现状**：`ftl_storage_t` 已把四个大数组外置成指针：
`tables` / `metadata_dir` / `journal` / `checkpoint_handle`。
`ftl_context_t` 仍内嵌 `pending[]` / `groups[]` / `wbuf` /
`migrate_handle` / `rr_page_scratch`。

**候选**（大小为量级估算，落地前用 DWARF `sizeof` 实证，C6）：

| 成员 | 量级 | 热度 | 建议 |
|---|---:|---|---|
| `migrate_handle_t` | ~4.2 KB（`page_buf` 2×2048） | 冷/中 | **挂 storage 指针** |
| `ftl_wbuf_t` | ~4.1 KB（`data` 4096B） | 热（写路径） | **挂 storage 指针**；或按域预算决定 |
| `rr_page_scratch[2048]` | 2 KB | 冷 | **挂 storage 指针**（或并入 RR storage） |
| `pending[32]` | ~1 KB | 很热（host IO） | 可选；域/局部性优先，没压力就内嵌 |
| `groups[32]` | ~1 KB | 很热 | 可选；同上 |
| `bg` | ~300 B | 中 | 内嵌 |
| `alloc` | ~100 B | 中 | 内嵌（内部指针已指向 `tables`） |
| `read_recovery_handle_t` | ~100 B | 中 | 内嵌 |
| 定时器 / 计数 / flag | 几十 B | 热 | 内嵌 |

**设计方向（本轮未改代码）**：

- 挂到 `ftl_storage_t`：`migrate_handle_t *migrate`、`ftl_wbuf_t *wbuf`、
  `uint8_t *rr_page_scratch`；
- 暂不挂：`pending[]` / `groups[]`（热数据；除非域预算/实测证明需要）；
- 保持内嵌：`bg` / `alloc` / `read_recovery_handle_t` / 定时器/计数/flag；
- `cp` 按 handle 契约并入 `checkpoint_handle_t`，不再单独占 context。

**`ftl_storage_t` 扩展草案**：

```c
typedef struct {
    const ftl_table_refs_t *table_refs;
    metadata_dir_cache_t  *metadata_dir;
    l2p_journal_t         *journal;
    checkpoint_handle_t   *checkpoint_handle;
    migrate_handle_t      *migrate;          /* 新增 */
    ftl_wbuf_t            *wbuf;             /* 新增 */
    uint8_t               *rr_page_scratch;  /* 新增 */
} ftl_storage_t;
```

`main.c` 定义这些数组并指定 `.sram*` / AXI 段；`ftl_init` 只绑指针；
容量 `_Static_assert` 跟着改。

**关键点：挂指针不减少总 RAM。** 数组仍在某个 SRAM/AXI 域里，总量不变；
收益是 `ftl_context_t` 变小、放置可控、handle 挂载形态统一。若目标是省 RAM，
只有共享/可选缓冲或真正删字段才有效。

**代价**：

- `wbuf` / `migrate` 从内嵌变指针后，调用点 `.` → `->`，diff 机械但不小；
- 测试夹具（冻结）会再多一批编译错误（R4，不改测试源）；
- 指针生命周期必须覆盖完成回调（静态存储即可，不能是栈）；
- 跨 SRAM 域访问有 cache/局部性影响，需要上板量；
- `pending` / `groups` 每个 host IO 都摸，挂指针可能得不偿失。

**验证**：先 `arm-none-eabi-readelf --debug-dump=info` 取 `sizeof`/偏移
（C6），再 `make all` / `make release`；收益（context 尺寸、域占用）上板后
用实测确认，未上板不声称。

---

## 9. 迁移阶段（未来代码工作，不在本轮）

1. **Phase 0**：本文件 + 字段所有权表（本轮）。
2. **Phase 1**：BBT（依赖最少，先验证模式）。
   **Phase 1.5（可选，独立小步）**：按 §8.1 把 `migrate_handle` / `wbuf` /
   `rr_page_scratch` 挂到 `ftl_storage_t`；`pending` / `groups` 暂保持内嵌；
   用 DWARF 实证 `sizeof` / 偏移，不靠估算。
3. **Phase 2**：Journal（写状态并入 handle；提供 `journal_is_idle` /
   `journal_can_erase` / `journal_write_failed` / `journal_sequence` /
   `journal_set_merge_floor`）。
4. **Phase 3**：Metadata（allocator/total_writes 窄指针）。
5. **Phase 4**：Migrate + RR（去掉 journal FSM 直读、pending 表直摸）。
6. **Phase 5**：Checkpoint（`ftl->cp` 并入 handle；窄上下文）。
7. **Phase 6**：L2P cache（owner 回调；cache 级 handle）。
8. **Phase 7**：Wbuf（新增 handle；保留 `FTL_OP_HOST_IO` + confirm 服务）。
9. **Phase 8**：PFR 收为 boot orchestrator；调度器任务表改描述符/任务 ID；
   定时器移入各 handle；confirm park 收进 pending 服务。

每个 Phase 都应是行为保持的；代码阶段按
`make all` / `make release` 回归，行为变化项上板。

---

## 10. 非目标

- 不拆共享数据面账本（L2P / BST / allocator / journal 水位 / metadata /
  pending / completion）；
- 不改变完成回调契约与 release 归属；
- 不引入动态分配；
- 不改介质格式；
- 不把 CP/metadata 改成回调内续跑（会嵌套同步泵）；
- 不做跨 LUN GC/WL（后续性能项）；
- 不引入 per-module vtable 或通用 `void *` 模块 API。

---

## 11. 参考

- `docs/design/ftl_design.md`：各模块职责与不变量。
- `docs/design/fc_nand_design.md §26 / §13.2`：层间判据与“硬件消费原则”
  （可平移到模块边界的字段级判据）。
- `docs/archive/multi_lun_generalization.md`：多 LUN / 共享 cache / 跨 LUN 边界。
- `docs/reference/pages/spdk_ftl.html:175-188`：SPDK FTL 的 GC/relocation
  模块划分（`reloc`）。
- `docs/design/known_risks.md`：共享账本决策、跨 LUN GC/WL、无 supervisor 等
  残余项。

---

## 12. 实现状态（2026-10-19）

> 本节记录**代码落地状态**与偏离；契约正文（§1~§11）仍是目标设计。
> 未上板，按 R1 一律标「未上板」。

| Phase | 状态 | 落地内容 |
|---|---|---|
| 0 | 完成 | 本文档 + §8 字段所有权表 |
| 1 BBT | 已改未上板 | `bbt_handle(alloc, info, bbt_status_t)`；状态上报 |
| 2 Journal | 已改未上板 | 写游标/sequence/merge_floor/page_buf/失败观测并入 `journal_handle_t`；`journal_handle` + 查询；allocator owner API；replay 走 journal_replay_block_ctx_t |
| 3 Metadata | 已改未上板 | `metadata_handle`；allocator/total_writes 窄指针；`metadata_live_count`；状态上报 |
| 4 Migrate | 已改未上板 | `migrate_handle`（自持 GC 10ms / WL 5s / predict 1s 节拍）；`migrate_trigger(GC_URGENT)`；窄上下文；reclaimed/novictim 计数入 handle |
| 4 RR | 已改未上板 | `rr_handle` / `rr_trigger` / `rr_status_t`；pending 表窄指针；**desc 保留方案未实现**（保留 compact 重下发，见 RISK-ARCH-04） |
| 5 Checkpoint | 已改未上板 | `checkpoint_handle` + trigger/status；`ftl->cp` 并入 handle；窄上下文；T1/T2/T3 求值收进模块 |
| 6 L2P cache | 已改未上板 | `l2p_handle`（档位→max_kick 模块内表）；owner 回调返回 `l2p_owner_t`；行查询/失效原语 |
| 7 Wbuf | 已改未上板 | `wbuf_handle_t` + `wbuf_handle(trigger)`；alloc/pending/migrate 窄指针 |
| 8 调度/Confirm | 已改未上板 | confirm park 收进 `pending_confirm_service()`（含 ms 超时）；任务表改具名 `bg_task_id_t`（13→10 项）；PFR 公开头只留 3 个顶层入口，full metadata rebuild / fresh/clean/PF 原语全部 static |
| Phase 1.5 | 已改未上板 | `migrate` / `wbuf` / `rr_page_scratch` 改 `ftl_storage_t` 挂载指针；数组落 AXI `.bss`，AXI 预算断言已计入 |
| 端口移除 | 已改未上板 | 6 个 `*_port_t` + 6 个 bind + 12 个 adapter 删除；模块 handle 只存窄上下文；`pending[]/groups[]` 提取为 `ftl_pending_table_t`；两个非平凡 seam 移到 `pending.c` owner API |
| ms deadline | 已改未上板 | migrate / RR / CP 的轮数计数改绝对 ms：`FTL_SUBMIT_TIMEOUT_MS=10`、`FTL_WAIT_TIMEOUT_MS=50`、`FTL_GROUP_DRAIN_TIMEOUT_MS=100`（待上板标定） |

**有意偏离**（已登记 RISK-ARCH-10/11）：

- **删除 `*_port_t` 端口层**（2026-10-20，用户要求）：模块 handle 只存实际需要的窄上下文
  （`alloc` / `journal` / `cache` / `metadata_dir` / `pending` / `migrate`），跨模块操作走
  owner API；不再有 6 个 port 结构体、6 个 bind、12 个一行 adapter。
- `pending[]` / `groups[]` 提取为 `ftl_pending_table_t`，RR/wbuf 只拿表指针，不拿整个
  `ftl_context_t`；两个非平凡 seam 移到 `pending.c` 的 `pending_view_op` /
  `pending_group_window_pending`。
- `journal_handle` 增加 `journal_trigger_t`（FLUSH / ERASE_ONLY），保留 must-run 擦除与
  预算刷页两条调度路径的原有分工；
- `wbuf_handle` 返回 `ftl_status_t`（写路径必须区分 `FTL_AGAIN` 背压与永久失败）；
- `pending_confirm_*` 现收 `ftl_pending_table_t *` + `confirm_ctx`（不再收整个
  `ftl_context_t *`；confirm handler 的事务上下文由 ftl.c 注入）；
- RR 采用 compact 重下发而非 §4.5 的 desc 链保留（§〇 主流判据 + 无实测收益）。

**验证**：`make all` / `make release` 零新告警；`make tests UT=FC` 通过；
`make tests UT=FTL` 仍在冻结的 `tests/unit/test_ftl.c` 上编译失败（既有旧符号；
本次还新增 `journal_init` / `metadata_dir_init` 签名变化。R4：测试源由用户维护，
不回改生产代码、不加兼容垫片）。

**符号/尺寸证据**（debug ELF，`arm-none-eabi-nm`）：

- PFR 只有 `ftl_media_check_format` / `ftl_media_format` / `ftl_boot_recovery` 为 `T`；
  fresh/clean/PF 子流程与 full metadata rebuild 原语全部为 `t`。
- Phase 1.5：`g_ftl` 3352 B（原 ~13.8KB）、`g_ftl_migrate` 4268 B、
  `g_ftl_wbuf` 4164 B、`g_ftl_rr_page_scratch` 2048 B；AXI 预算 `_Static_assert` 已计入。

**仍未验证**：整组改动**未上板**；需回归正常读写 / GC / WL / RR / CP / journal / BBT /
格式化 / 掉电重启（RISK-ARCH-06）。

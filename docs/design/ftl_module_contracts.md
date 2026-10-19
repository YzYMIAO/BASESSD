# FTL 模块 handle 契约

> **性质**：FTL 内部模块的接口设计文档，不是 host API。
> **范围**：FTL 主调度与模块的边界、各模块 `*_handle()` 的输入/输出、时间与超时、
> 多 LUN/owner 路由。**本轮只出设计，不改代码。**
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
| 动态运行时量 | `now_ms`、`load_level`、`free_blocks`、`pending/group`、`journal_seq`、`total_writes`、`emergency`、`force`、`error` | 传参或端口查询 |
| 事件/上下文 | trigger request、UNC desc 链、`rr_owner_t` | 逐次传入 |

通用 `ftl_bg_info_t` 收敛为：

```c
typedef struct {
    uint32_t now_ms;
    uint8_t  load_level;  /* IDLE / NORMAL / BUSY */
} ftl_bg_info_t;
```

`free_blocks` / `pending` 等由需要它的模块通过 allocator/pending 端口查询，
或放进该模块自己的 `*_trigger_t`。

### 3.3 入口形态（已定）

- **FTL 上下文挂模块指针**：`ftl->checkpoint.handle`、`ftl->journal`、
  `ftl->migrate.handle`、`ftl->rr.handle`、`ftl->wbuf.handle`、
  `alloc->bbt.handle` 等；
- **模块 API 用 typed 入口**：
  `*_handle(module_handle_t *, const module_trigger_t *, module_status_t *)`；
- **不把整个 `ftl_context_t *` 传进模块**；
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
- 端口：allocator 池操作、metadata ring。
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
- 端口：`valid_count_port`（replay 记账，替代 `ftl_context_t*` 强转）、
  `bad_block_port`（持续擦失败标坏）。
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
- 端口：allocator（note_erase/free/bbt_mark_bad/free_count/pool 状态）、
  `total_writes`、Anchor ring；`dir->handle` 是异步 substate。
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
- victim 由 handle 通过 policy 端口选（`gc_select_victim` / `wl_select_victim` /
  `rr_predictive_scan`）。
- 上报：`state`、`kind(GC/WL/PREDICT)`、`migrated`、`erased`、`aborted`、
  `erase_failed`、`progress`、`wants_run`、`next_ms`、`timed_out`、
  `timeout_phase`、`error`；查询 `migrate_is_active`、
  `migrate_reclaimed_count`、`migrate_novictim_count`。
- 时间：handle 自持 GC 10ms / WL 5s / predict 1s 节拍；等 L2P/Journal 用
  `FTL_WAIT_TIMEOUT_MS`；提交背压用 `FTL_SUBMIT_TIMEOUT_MS`。
- 端口：alloc、journal、l2p、policy（选块/ECC/退役）。
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
5. 成功/失败都释放保留 desc 链；成功通过 pending 端口结算组，
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
- 端口：l2p / journal / metadata / allocator。
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
- 端口：owner 端口返回 `{dim, metadata_dir, alloc, topo}`；FC async read/write；
  metadata directory；同步逃生口的 service pump。
- 禁止：FTL 遍历 `cache->lines[]`、直读 `flush_inflight`/`thrashing`。
- 完成回调：`ftl_l2p_callback` 不变，ctx 仍是 `l2p_cache_line_t *`。

### 4.8 Wbuf

```c
void wbuf_handle(wbuf_handle_t *h, const wbuf_trigger_t *t,
                 wbuf_status_t *out);
```

- 事件驱动：`APPEND(lba, data, n, gi)` / `FLUSH` / `DROP(range)` / `RESET` /
  `MAYBE_FLUSH_IDLE`。
- 上报：`state(IDLE/OPEN/FLUSHING/FAILED)`、`open`、`occupied`、`owners`、
  `flushed_pages`、`drop_count`、`wants_run`、`timed_out`、`error`。
- 时间：仅提交背压。
- 端口：alloc（alloc_pair/rewind）、pending（alloc/group add/complete/
  has_pending_writes）、migrate（`migrate_trigger(GC_URGENT)`）、FC 建链。
- 移入 handle：`ftl->wbuf` 的 data/occupied/open/base/owners。
- 禁止：扫 `ftl->groups[]`；直接结算 pending；分配页对失败时自己起 GC。
- 完成：保留 `FTL_OP_HOST_IO`；wbuf 暴露 `wbuf_complete()`；
  Journal→L2P→BST 事务仍由 pending confirm 服务承担。

---

## 5. 不是 handle 的模块

| 模块/功能 | 定位 |
|---|---|
| GC 策略 `gc.c` / WL 策略 `wear_leveling.c` | policy 端口，由 migrate handle 调用；不拆 `gc_handle`/`wl_handle` |
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
  `ftl_boot_recovery`；fresh/clean/PF 子流程与 F2 原语收为 static/私有头。
- 输入：已绑定 handles/ports + `need_format` + media status。
- 输出：`ftl_boot_result_t`。
- 时间：只受 NAND 操作超时约束。
- 穿透收口：`block_words` → `block_snapshot_port`；`l2p_live[]` →
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
- 上报：`ftl_status_t` 增加 `FTL_TIMEOUT`（与 NVMe timeout /
  `-ETIMEDOUT` 同构），模块 status 带 `timeout_phase`。
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
- **owner 端口**：

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
  `l2p_global_to_local_region` / owner 端口转换（C9）。
- **调度**：`ftl_service_pump()` 全局一次；逐实例 `ftl_service` 调自己的
  handle；共享 L2P 按 owner/dim 调用。
- **完成路由**：命令 `ch/ce/lun` → 实例；ctx 是模块 handle/line；回调只用
  handle/端口。
- **初始化顺序**：设备级 `l2p_cache_init` / `completion_table_init` /
  owner 路由注入，先于逐实例 `ftl_init` + PFR。

### 7.1 GC/migrate 跨 LUN 现状

**不支持，且是刻意的**：

- `mig_lba_usable` 要求 `lba_routes_to(lba, ftl->topo)`（`migrate.c:553-557`）；
- 目标页来自本实例 `allocator_alloc_pair(&ftl->alloc, ...)`（`:524`）；
- allocator 在 init 绑定本实例 `ch/ce/lun`（`allocator.c:26-34`）；
- 源页 `super_block_page_pba(ftl->topo, &m->victim, ...)`（`:194,536`）；
- `multi_lun_generalization.md:26,358-359` 明确跨 LUN 条带/GC 不在当前范围。

跨 LUN 需要设备级 relocation 引擎 + 跨实例事务（两套 allocator/BST/
journal/checkpoint 账本同步），不是改 `migrate_handle_t` 入参能解决的。
**已登记为后续性能项**（见 `known_risks.md`）。

---

## 8. 字段所有权与单写者

| 字段/资源 | owner（唯一写者） | 读者/接口 |
|---|---|---|
| L2P cache / region 映射 | `l2p_cache.c` | `l2p_cache_read/ready/write`、owner 端口 |
| BST / allocator / 池 | `allocator.c`、`bst.h` | `allocator_*`、`ftl_block_*` |
| journal 持久性状态 | `journal.c` | `journal_append` / 查询 |
| metadata directory | `metadata.c` / `metadata_job.c` | `metadata_dir_*` / `metadata_dir_busy` |
| pending / groups | `pending.c` | `pending_*` / 端口 |
| completion table | `completion_table.c` | `completion_table_*`、`g_op_callbacks` |
| 各 job FSM | 各模块 handle | status / 查询 |
| 调度定时器 | 各模块 handle | `now_ms` + status |
| 观测计数 | 各模块 handle | status / log |

**规则**：每个共享字段只有一个写者；其他模块只读查询或走端口。
`ftl_context_t` 是 device 描述符，不承载业务状态。

---

## 9. 迁移阶段（未来代码工作，不在本轮）

1. **Phase 0**：本文件 + 字段所有权表（本轮）。
2. **Phase 1**：BBT（依赖最少，先验证模式）。
3. **Phase 2**：Journal（写状态并入 handle；提供 `journal_is_idle` /
   `journal_can_erase` / `journal_write_failed` / `journal_sequence` /
   `journal_set_merge_floor`）。
4. **Phase 3**：Metadata（allocator/total_writes 端口）。
5. **Phase 4**：Migrate + RR（去掉 journal FSM 直读、pending 表直摸）。
6. **Phase 5**：Checkpoint（`ftl->cp` 并入 handle；四端口）。
7. **Phase 6**：L2P cache（owner 端口；cache 级 handle）。
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
- `docs/design/multi_lun_generalization.md`：多 LUN / 共享 cache / 跨 LUN 边界。
- `docs/reference/pages/spdk_ftl.html:175-188`：SPDK FTL 的 GC/relocation
  模块划分（`reloc`）。
- `docs/design/known_risks.md`：共享账本决策、跨 LUN GC/WL、无 supervisor 等
  残余项。

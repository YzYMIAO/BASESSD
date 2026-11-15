# FTL 单元测试设计（板端）

> 状态：活跃设计文档。最近一次全量板端结果见 §9；用例名与参数以 §4 表为准。
> 范围：`src/ftl/`（FTL 层）。HIL（`src/hil/`）的 UT 不在本文，见
> `docs/design/known_iss.md` ISS-HIL-01。
> 被测设计：`docs/design/ftl_design.md`。风险状态：`docs/design/known_risks.md`。
> 分层坐标：沿用 `docs/design/testing_guide.md` §3 的 L0~L6；本文属 **L2 FTL**，
> `/bench/speed/` 属 **L4**。

---

## 0. 单一事实源

本文只维护「测试怎么设计、用例契约是什么」。下面这些信息**不在本文复制**：

| 信息 | 唯一事实源 | 本文怎么引用 |
|---|---|---|
| 用例是否存在、参数是什么 | 本文 §4 + `tests/unit/test_ftl_pure.c` / `test_ftl_media.c` | §4 只写契约与覆盖点，不写实现细节 |
| 风险状态、历史、是否闭合 | `docs/design/known_risks.md`（已闭合条目另见 `docs/archive/known_risks_batches.md`） | §5 只写 `RISK-ID → 用例` 映射，**不写状态词** |
| 未实现 / 未验证的设计项 | `docs/design/known_iss.md` | §7 只给链接 |
| 每次运行结果与证据 | `build/test-results/` + `docs/design/testing_guide.md` §8.7 | §9 只留最近快照 |
| 分层与跨层测试流程 | `docs/design/testing_guide.md` | 本文沿用其 L0~L6 坐标 |

> 判据：读者要知道「某风险现在什么状态」，去 `known_risks.md`；要知道「这条用例断言
> 什么」，看测试源注释。本文两处都不重复。

---

## 1. 目的与原则

1. **只在目标板跑**（`CLAUDE.md` R4）：`make tests UT=FTL` 生成镜像，串口 CLI `test`
   执行；不做 host 模拟。
2. **两层并重**：
   - **FTL-U / pure（RAM）**：模块内部功能与不变量。池闸门、组槽耗尽、同 LBA 合并、
     L2P 驻留行状态、Directory 主备提交等分支必须在 RAM 里构造。
   - **FTL-I / media（真 NAND）**：跨模块契约、持久化、恢复、GC/WL 搬移、cache 序列。
3. **模拟规模小、结构体同型**：pure 用例使用生产数组/结构体，只填少量状态；不引入
   第二套编译期几何。
4. **不新增生产 seam**：只用现有 `time_us`、`l2p_owner_fn_t`、
   `metadata_ring_probe_fn_t`、`jrn_replay_ctx_s`、`fault_inject.h`。
5. **一个用例只钉一个契约**：判据必须能回答「断言比较的是哪两个值」；没有区分力的
   断言不算覆盖（C7 / C8）。
6. **负例必须能红**：每个正向判据旁写一条「什么输入会让它失败」；构造不出负例的用例，
   按缺陷登记。
7. **状态如实**：未上板标「未上板」；SKIP 必须打印原因；不复用 H2testw 结果冒充 UT（R1）。

---

## 2. 测试架构

### 2.1 层与套件

| 层 | 根套件 | 环境 | 内容 | 破坏性 |
|---|---|---|---|---|
| **FTL-U** | `/ftl/pure/` | RAM，测试自有对象 | 模块单元、不变量、错误分支 | 否 |
| **FTL-I** | `/ftl/media/` | 真 NAND（首次 format，部分用例再 format） | 跨模块契约、持久化、GC/WL、cache | 是 |
| **PERF** | `/bench/speed/ftl_rw` | 真 NAND | 吞吐基准；只判「跑不跑得完」 | 是 |
| **MANUAL** | 板端流程 | 真板 | POR、USB、H2testw、坏块注入 | 是 |

```text
/ftl/
  pure/
    mem/    allocator / BST / BBT / wear
    md/     metadata ring / Directory / checkpoint / journal
    host/   pending / group / wbuf / ftl_op / bg
    io/     L2P / CMT / GC / WL / RR
  media/
    e2e/    format / 写读 / wbuf / flush / TRIM / 模拟重启 / GC / WL / BBT / cache
/bench/speed/
  ftl_rw    FTL host API 25 MiB 读写（破坏性）
```

> `FTL-U` / `FTL-I` 是**本仓自拟**的套件标签，对应主流测试分层的 unit / integration；
> 不声称行业标准术语。media 侧只保留一个 `e2e/` 子套件：用例共享一次 format 与
> `s_media_ready` 状态，拆成 l2p/md/cp/bbt 多个套件会让「哪条用例负责准备介质」变成
> 跨套件隐式依赖。

执行顺序：`pure/` → `media/`。`test /ftl/pure/` 安全；`test /ftl/media/` 与裸 `test`
**破坏介质**。

**资源钩子**：每条用例都挂 setup/teardown：

- pure → `ftl_ut_pure_setup`（重建测试自有 RAM 表 + `fi_reset`）/ `ftl_ut_pure_teardown`
  （断言活实例资源与后台任务归零）；
- media → `media_setup`（拓扑重建 + `fi_reset`）/ `media_teardown`（quiesce 后再断言
  资源归零）；整片 format 仍由用例内的 `media_prepare` 首次获取，避免只跑
  `media_live` 也擦片。

### 2.2 测试技术矩阵

| 技术 | 判据 | 本仓落点 |
|---|---|---|
| **Directed / contract** | 一个不变量或契约一条用例 | `alloc_free_pool_invariants`、`l2p_wal_gate` |
| **Negative** | 非法状态/越界/错误码必须被拒绝或降级 | `host_submit_invalid_args`、`pend_group_exhaustion` |
| **Boundary / parameterized** | 边界值成组覆盖，不写死单点 | `host_io_matrix`、`wbuf_density`、`cache_failc_abort` |
| **Seeded random** | 记录 seed；失败可按 seed 复现 | `host_io_random`、`/bench/speed/ftl_rw` |
| **Fault injection** | 注入点 → 预期降级/恢复 → 清除后可恢复 | `fi_cache_fail_arm`、`fi_journal_fail_arm` |
| **Reboot / crash point** | 模拟重启或真 POR 后映射/账本一致 | `*_reboot_*`、`trim_no_resurrection` |
| **Stress / perf** | 只作基准，不作功能通过率 | `/bench/speed/ftl_rw` |

> 随机用例必须把 seed 写进证据行；没有 seed 的「随机跑过」不算可复现判据。
> 压力/性能不进功能通过率：`/bench/speed/ftl_rw` 只判「跑不跑得完」，写返回 `IO_ERROR`
> 与读数据不符只计入 `err(wr/rd)` 上报。

### 2.3 覆盖模型（CP-*）

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

## 3. 夹具与隔离

### 3.1 活实例

- `ftl_ut_live()` 返回 `g_ctrl.ftl[0]`（die 0；`extern ssd_ctl_s g_ctrl`）。
  `ftl_ctx_s` 是大型静态对象，按值复制不现实。
- 每个套件 setup 调 `ftl_ut_topology_init()`：用 `g_nand_targets` 重建 die 表，UT 不依赖
  主 boot 留下的表顺序；route/reboot 也按 die 索引。
- pure 用例**不 reset 活实例的 allocator/metadata**；使用测试自有对象。
- 需要活实例 L2P 行的用例只借少量行，teardown 原样恢复。

### 3.2 测试自有 RAM 对象

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

### 3.3 伪时钟

- 复用 `ssd_platform_register` / `ssd_platform_ops_s.time_us`；`ftl_ut_clock_install()`
  保存原 ops，`ftl_ut_clock_restore()` 恢复。
- 仅需要固定 now 的用例安装（当前为 `md_cp_init_timer`）；用例结束显式调用
  `ftl_ut_clock_restore()`，不要依赖 teardown。真 NAND I/O 一律用真实时钟。

### 3.4 结束不变量（INV-*）

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

### 3.5 真 NAND 安全模型

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

### 3.6 模拟重启夹具（`ftl_ut_reboot_from_media`）

顺序是承重的，缺一步就在板端以断言/日志风暴的形式暴露：

1. `ftl_ut_quiesce()`：泵到 FC 池、CQ、`ftl_op_req_count()` 全 0（否则旧 CP 的迟到完成
   撞 `checkpoint.c` 的 `h->waiting != 0`）；
2. `fc_init()` 后必须 `fc_nand_engine_set_armed(1U)`（`fc_init` 会清派发放行闩锁）；
3. `ftl_op_init()` + 恢复实例路由表（`ftl_op_route_set`）；
4. 先保存 cache/metadata/journal/CP/migrate/wbuf/rr/table_refs 句柄（`ftl_init` 会
   memset 整个上下文），重建 `ftl_storage_s`，再 `ftl_init` + `l2p_init`；
5. `pfr_boot_recovery(ftl, &result)`。

只标「模拟重启」，**不标 POR**（真掉电仍是人工流程）。

### 3.7 media 用例顺序与状态依赖

- `media_prepare()` 只在第一次调用时执行 `ftl_ut_media_format()`；后续用例共享已格式化的介质。
- 写介质的重用例（`host_io_matrix` / `host_io_random`）排在 TRIM 用例**之后**：
  Journal 压力下 TRIM 的持久化时序有问题（`RISK-HOST-11`），先跑 TRIM、再压大量重写
  是其当前缓解；不要为了「参数矩阵在前」调换二者顺序。
- `trim_no_resurrection` 必须 `trim → flush → 强制 CP → 模拟重启`：TRIM 只记 Journal，
  不 flush 就重启会「复活」（`RISK-HOST-08/11`）。等待 CP 时要等 `phase == CP_IDLE`，
  CP 基准要在**用例起点**取（写入/trim 之前），并在调用 `cp_pool_find_latest` **之前**
  保存目标序号（否则断言恒真，`RISK-UT-02`）。
- `bbt_persist` 先擦 BBT 池并清 `a->bbt` 环形状态再标坏：多圈 ring 的 save→reload 会选旧 record
  （`RISK-BBT-18`），clean-pool 路径才能稳定验证持久化。
- `format_in_progress_resume` 用**更大的 BBT sequence**（`0xFFFFFF00` 起步）写一条带
  FORMAT_IN_PROGRESS 的 record，让它在真实 bootstrap 扫描中胜出（隔离 `RISK-BBT-18` /
  `BOOT-22`），再擦 CP 池制造「格式化未完成」状态；**不擦 BBT 载体**（擦 spare 会在
  失败路径上丢坏块图，`RISK-UT-03`）。用例末尾跑完整 format 恢复介质；它排在 media
  套件末尾（`boot_restore_reconcile` 之前）。

### 3.8 可测性 seam 清单

| seam | 用途 | 产品固件是否编入 |
|---|---|---|
| `time_us` | 伪时钟 / 超时 | 是（平台 ops） |
| `l2p_owner_fn_t` | L2P 归属探测 | 是 |
| `metadata_ring_probe_fn_t` | ring locate 假 probe | 是 |
| `jrn_replay_ctx_s` | Journal 回放注入 | 是 |
| `fault_inject.h` | cache / journal 失败注入 | **否**（仅测试镜像） |

> 新增 seam 前先过 `CLAUDE.md` C2 / H13：能复用现有 seam 就不新增；新增 seam 必须
> 说明「封装了什么不变量/边界」，并在本表登记。

---

## 4. 用例清单

> 首列是 `test --list` 里的用例名（稳定 ID）；测试函数名以 `test_` 开头，
> 不要求与用例名逐字相同。
> 参数列用 `/` 分隔取值，例如 `mode=hot/cold/high_wear`。
> 本表只写**契约 / 覆盖点 / RISK**；具体输入、断言值、负例写在测试源注释里（§8 模板）。

### 4.1 `/ftl/pure/mem/`（allocator / BST / BBT / wear）

| 用例 | 参数 | 契约 / 覆盖点 | RISK |
|---|---|---|---|
| `pba_roundtrip` | — | LBA/PBA 编解码往返、`ftl_gblock` plane-major 编号；`CP-MAP` | `RISK-ALLOC-06` |
| `alloc_free_pool_invariants` | — | `state==FREE ⟺ 在池`、per-plane 两本账同步、OP refill LIFO；`CP-LEDGER` / `CP-STATE` | `RISK-ALLOC-09` |
| `alloc_super_open_close` | `mode=hot/cold/high_wear` | 开 SB 两 plane 同页号、rewind 不烧块、close 记录 `sb_id`；`CP-STATE` | — |
| `alloc_closed_list_add_drop` | — | Closed 列表按 plane 摘除，最后一个 plane 才释放条目；`CP-LEDGER` / `CP-STATE` | — |
| `alloc_valid_count_guard` | — | `valid_count` 记账、BAD 后继续 sub 必须无效；`CP-LEDGER` / `CP-STATE` | `RISK-MIG-03` |
| `alloc_op_spare_pools` | — | OP/Spare per-plane 独立计数、Spare LIFO、空池 pop 失败；`CP-BOUND` / `CP-LEDGER` | — |
| `alloc_erase_stats` | — | 擦除后 `erase_count+1`、`last_pe_ts`、`valid/read_count` 清零、state OPEN；`CP-STATE` / `CP-LEDGER` | — |
| `alloc_bbt_mark_bad` | `plane=0/1` | 标坏置 BAD、清 valid/sb_id、bad/dirty_gen 推进、Spare 补池、幂等；`CP-STATE` / `CP-LEDGER` | `RISK-BBT-01` / `RISK-BBT-10` |
| `alloc_bbt_metadata_bad` | — | 元数据块标坏不弹用户 Spare，置 `pools.dirty` / `rebuild_required`；`CP-STATE` / `CP-LEDGER` | — |
| `pba_cw_masks` | — | CW flat 绝对编号、span/plane 掩码、popcount、`NAND_FIELD_MASK`；`CP-MAP` | `RISK-BIT-01` |
| `alloc_spare_full_refill_none` | — | Spare 满拒收、清空后 refill、Free Pool 空时 refill 返回 0；`CP-BOUND` | — |

### 4.2 `/ftl/pure/md/`（metadata / checkpoint / journal）

| 用例 | 参数 | 契约 / 覆盖点 | RISK |
|---|---|---|---|
| `md_idle` | — | metadata job 初始态、`md_handle_is_idle`、live block 计数 0；`CP-STATE` | — |
| `md_ring_geometry` | — | `records_per_block` / `total_records` / `has_block` / `plane_at` 哨兵；`CP-BOUND` / `CP-MAP` | — |
| `md_directory_commit` | — | primary 覆盖写 live 计数迁移、PAIR 双副本、entry/backup 查询；`CP-DUR` / `CP-LEDGER` | — |
| `md_journal_append` | — | init/reserve/append 序号推进、pending 置位、未到水位不请求 CP；`CP-DUR` | — |
| `md_ring_locate` | — | 中段/块末/空环定位；「空环」与「找不到最新记录」区分；`CP-STATE` / `CP-BOUND` | — |
| `md_dir_ckpt_three_step` | — | begin → page/advance → commit、`dirty_gen` 竞态不清脏；`CP-DUR` / `CP-STATE` | `RISK-MD-06` |
| `md_log_alloc_rotate_release` | — | 日志块分配/释放/轮转、页游标归零；`CP-LEDGER` | — |
| `md_cp_init_timer` | — | `active/waiting=0`、`phase=IDLE`、T3 绝对到期时刻重装；`CP-STATE` | — |
| `md_status_error` | — | `handle.error` 进入 IDLE 被清，采样 `FTL_OK`（当前语义特征化）；`CP-ERR` | `RISK-MD-17` |
| `md_journal_write_fail_clear` | — | `write_fail_seq` 只有 CP 水位追上才清除；`CP-ERR` | `RISK-JN-03` |
| `md_abort_keeps_victim` | — | 压缩写 abort 只回 IDLE，不回收/擦除 victim；`CP-STATE` / `CP-ERR` | `RISK-MD-06` / `RISK-MD-16` / `RISK-BOOT-02` |
| `md_reclaim_erase_fail` | `kind=fixed/borrowed` | 连续 3 次擦失败→`bbt_mark_bad`、清 reclaim、phase 回 IDLE；borrowed 块 `log_borrow_count` 结算；`CP-LEDGER` / `CP-STATE` | `RISK-MD-01` / `RISK-MD-22` |
| `md_cp_read_fail_no_reclaim` | — | CP_READ 失败且无备份→abort；`l2p_live>0` 的 victim 不登记回收；`CP-ERR` / `CP-STATE` | `RISK-MD-16` |
| `md_journal_truncate_idempotent` | — | 水位/0 保护、环形下一块、重复 plan=ALREADY、`commit(NONE)` 不清预擦块；`CP-DUR` | — |
| `md_cp_encoders` | — | block word 位段、快照、OP/Spare 标签、磨损页字节布局；`CP-DUR` / `CP-MAP` | — |

### 4.3 `/ftl/pure/host/`（pending / group / wbuf / ftl_op / bg）

| 用例 | 参数 | 契约 / 覆盖点 | RISK |
|---|---|---|---|
| `host_idle` | — | pending/group/ctx 全 0、wbuf idle、bg 任务表长度 = `BG_TASK_COUNT`；`CP-LEDGER` | — |
| `pend_slots` | — | 槽满返回 NULL、按 `command_id` 查找/移除、移除后可再分配；`CP-BOUND` / `CP-STATE` | — |
| `pend_group_exhaustion` | — | 组槽满返回 0，已有组字段不被覆盖；`CP-BOUND` | `RISK-PEND-02` |
| `pend_group_complete_once` | — | 两个子操作 outstanding 2→0，只回调一次并释放组槽；`CP-STATE` | — |
| `pend_group_failure_aggregation` | — | 子操作 IO_ERROR 后 `agg_status` 粘滞，后续 OK 不清错；`CP-ERR` | — |
| `pend_confirm_park_timeout` | — | `pend_confirm_is_expired` 到点判定、`settle` 释放槽并按 IO_ERROR 结算一次；`CP-STATE` / `CP-ERR` | `RISK-HOST-05` |
| `pend_confirm_is_parked` | — | 待入账判别只看 `WRITE_CONFIRM`，普通 used 写槽为 false；`CP-STATE` | `RISK-HOST-05` |
| `wbuf_append_read` | — | 单 CW append/read、同窗口稀疏两位、owners/在飞计数；`CP-MAP` / `CP-LEDGER` | `RISK-WB-03` |
| `wbuf_drop_reset_owners` | — | DROP 清位/最后一个 CW 按 OK 结算、RESET 按 IO_ERROR 结算；`CP-STATE` / `CP-LEDGER` | `RISK-WB-03` |
| `wbuf_density` | `n=1/2/4/8` | `occupied==(1<<n)-1`、组在飞只 +1、逐 CW 读回一致；`CP-MAP` / `CP-BOUND` | `RISK-WB-03` / `RISK-IF-04` |
| `op_ctx` | — | request 表 add/remove 计数；`CP-LEDGER` | — |
| `op_ctx_capacity` | — | 旁表容量、同 `cmd_id` 覆盖不追加；`CP-BOUND` | `RISK-IF-05` |
| `bg_engine` | — | 任务表长度 = `BG_TASK_COUNT`、`level_select` 返回合法档位；`CP-STATE` | — |
| `op_route` | — | `(ch,ce,lun)` 与 region 路由取回同一实例，结束恢复原表；`CP-STATE` | `RISK-CR-02` |
| `bg_level_select` | — | pending 0→IDLE、8→NORMAL、17→BUSY，used group 保守升 BUSY；`CP-STATE` / `CP-BOUND` | — |
| `host_submit_invalid_args` | — | count=0 / buffer=NULL / LBA 越界 / ftl=NULL 在分配池资源前拒绝；`CP-ERR` | — |

### 4.4 `/ftl/pure/io/`（L2P / CMT / GC / WL / RR）

| 用例 | 参数 | 契约 / 覆盖点 | RISK |
|---|---|---|---|
| `gc_policy_entry` | — | GC/WL 入口无副作用、空盘基线；`CP-STATE` | — |
| `mig_eval_fake_clock` | — | 伪时钟 12345 下写计数变化⇒`write_last_ms=12345`，证 `mig_eval` 读 SysTick 而非 TIM6 快照；`CP-STATE` | `RISK-PLAT-10` |
| `gc_trigger` | — | 空池 EMERGENCY、TARGET 以上不触发、总水位达标但单 plane 低仍 WATERMARK；`CP-BOUND` | `RISK-ALLOC-09` |
| `gc_select_victim` | `mode=off/idle/emergency` | 三种 benefit 模式选有效数据更少者、饥饿 plane 优先成对/单条目；`CP-STATE` / `CP-BOUND` | `RISK-GC-06` / `RISK-GC-14` / `RISK-ALLOC-09` |
| `gc_decode_cw_lba` | — | OK / EMPTY / CORRUPT / 空洞四态；`CP-MAP` / `CP-ERR` | — |
| `wl_trigger_and_victim` | — | UBI 判据 `max_free_ec − min_used_ec ≥ 200`、victim 取最低擦除 used SB；`CP-STATE` | `RISK-WL-04` |
| `l2p_resident_states` | — | CLEAN/DIRTY/FLUSHING/LOADING 状态语义、批量掩码同源；`CP-STATE` | `RISK-L2P-03` |
| `l2p_miss_unmapped_evict` | — | miss→AGAIN→CLEAN+未映射、写后读回；该 region 已驻留、无空行或 cache 有脏/在飞时 SKIP；`CP-STATE` / `CP-MAP` | `RISK-L2P-06` |
| `l2p_cmt_write_readback` | — | CMT dirty 安装、读命中、flush 前合并、写完 retire；`CP-STATE` / `CP-MAP` | — |
| `l2p_cmt_capacity_evict` | — | 同 set 4 路全 DIRTY ⇒ install 失败回退 line；`make_room` 选 LRU dirty victim；inflight 饱和只背压不淘汰；`CP-BOUND` | — |
| `l2p_cmt_multi_region_merge` | — | flush 只合并目标 region 的 dirty CMT；`CP-MAP` | — |
| `l2p_cmt_entries_fallback` | — | set 满时整批回退 line，不部分插入；`CP-BOUND` / `CP-MAP` | — |
| `l2p_cmt_line_invalidate` | — | 行失效只清 CLEAN、保留 DIRTY；`CP-STATE` | — |
| `cache_wr_lookup` | — | PROGRAMMED 返回真实 PBA；CONFIRMED/FAILED 不返回；FAILED 由 covers 判 IO_ERROR；`CP-MAP` / `CP-STATE` | `RISK-CACHE-27` / `RISK-CACHE-28` |
| `l2p_wal_gate` | — | `persisted_seq < dirty_seq` 时退让（AGAIN + `wal_defer_count+1`），追上后放行；`CP-DUR` | `RISK-L2P-09` |
| `wl_victim_threshold` | — | 低有效 CW 跳过、≥70% 接受；`CP-BOUND` | `RISK-WL-04` |
| `rr_health` | — | HEALTHY→WARNING→CRITICAL→DYING 单调不回落；`CP-STATE` | `RISK-RD-01` |
| `rr_predictive_retire` | — | ECC 达 `RR_ECC_MAX` 命中、退役清健康计数；`CP-STATE` | `RISK-RD-01` |

### 4.5 `/ftl/media/e2e/`（真 NAND）

| 用例 | 参数 | 契约 / 覆盖点 | RISK |
|---|---|---|---|
| `media_live` | — | 活实例 free/op/spare/bad 账本与池/组/ctx 基线；`CP-LEDGER` | — |
| `format_and_roundtrip` | — | format → 8 LBA 写 → flush → 读回逐字节一致；`CP-MAP` / `CP-DUR` | — |
| `cross_region_roundtrip` | — | 起点 504、长度 16 跨 512 LBA region 边界；`CP-MAP` | — |
| `flush_reboot_readback` | — | 写 → flush → 模拟重启 → 读回，数据来自介质而非进程 cache；`CP-DUR` / `CP-MAP` | — |
| `trim_no_resurrection` | — | trim → flush → 强制 CP → 重启不复活；journal-only 段不复活；`CP-DUR` | `RISK-HOST-08` / `RISK-HOST-11` |
| `preload_region_zero_and_high` | — | region 0 与高位 region 预载不别名；`CP-MAP` | `RISK-BOOT-01` |
| `journal_barrier_cp_progress` | — | flush 后无 pending、CP 序号推进且 Header 落盘、重启一致；`CP-DUR` | `RISK-UT-02` |
| `gc_progress` | — | 构造唯一有效 CW victim → urgent GC → moved 增长、未 trim 源 LBA 可读；`CP-MAP` / `CP-LEDGER` | `RISK-GC-06` / `RISK-MIG-03` |
| `wl_progress` | — | 构造 UBI 擦除差 → WL 进入 `MIG_KIND_WL`、moved 增长、数据可读；`CP-STATE` / `CP-MAP` | `RISK-WL-04` |
| `cp_probe_clean_media` | — | `cp_media_status_probe=CURRENT`、版本匹配、不误格式化；`CP-STATE` / `CP-DUR` | — |
| `host_io_matrix` | `count=1/7/8/9/64` | 8 是 pair 边界、9 是跨 pair 非对齐、7 是奇尾；起点按种子随机；`CP-MAP` / `CP-BOUND` | — |
| `host_io_random` | — | 固定种子 8 轮随机 LBA/长度写读，每轮 flush 后读回；`CP-MAP` | — |
| `bbt_persist` | — | 标坏 → save → 模拟重启 → 坏块仍在；先擦池隔离 `RISK-BBT-18`；`CP-DUR` | `RISK-BBT-01` / `RISK-BBT-10` / `RISK-BBT-18` |
| `format_in_progress_resume` | — | FORMAT_IN_PROGRESS record + 擦 CP 池 → 重启判 NO_RECORD/继续格式化；`CP-DUR` / `CP-STATE` | `RISK-BOOT-04` / `RISK-BOOT-17` / `RISK-BOOT-22` |
| `cp_corrupt_refuse_format` | — | CP 池非 FF 页 → 探针 IO_ERROR、拒绝格式化；`CP-ERR` / `CP-DUR` | `RISK-BOOT-03` |
| `journal_write_fail_wal_stall` | — | 注入 2 次 Journal 页写失败 → 标记置位、`persisted_seq` 落后；放开后有界收尾、数据可读；`CP-ERR` / `CP-DUR` | `RISK-JN-03` / `RISK-L2P-10` |
| `cache_failc_abort` | `arm=1/2/last` | FAILC 结算上一页：已确认前缀保留、失败页 LBA 读 IO_ERROR、`wb_aborts+1`、同批未确认页保留旧映射；`CP-ERR` / `CP-STATE` | `RISK-CACHE-19` / `RISK-CACHE-24` / `RISK-CACHE-27` / `RISK-CACHE-28` |
| `cache_seq_cross_block` | — | 连续写 1024 LBA（2 物理 block）分段读回逐字节一致；`read_pairs` 覆盖整段、`fallbacks` 不增长；`CP-MAP` | `RISK-CACHE-20` / `RISK-CACHE-21` |
| `cache_early_ack_read_visible` | — | 写新数据不 flush 立刻读回新数据；flush + 重启仍一致；`CP-DUR` | `RISK-CACHE-27` |
| `cache_failc_undo_read_error` | — | FAILC 后下一条 host 命令报一次 deferred error；失败页/同 region 未映射 LBA 读 IO_ERROR；清注入后落盘、环排空；`CP-ERR` / `CP-STATE` | `RISK-CACHE-28` |
| `cache_early_ack_flush_barrier` | — | flush 完成前 `wr.count>0` 阻止完成；完成后 `wr.count==0`、L2P 指向新 PBA、重启一致；`CP-DUR` | `RISK-CACHE-29` |
| `cache_cmt_writeback_retire` | — | CMT 写回后 dirty retire 成 CLEAN（若驻留），TRIM 负条目丢弃；`CP-STATE` / `CP-MAP` | — |
| `bbt_ring_reload` | — | 两条 record 后重启走生产 `bbt_init`，两个坏块都在（载入最新）；`CP-DUR` | `RISK-BBT-18` |
| `boot_restore_reconcile` | — | 写 + flush → 记录 free/OP/Spare/Closed/open + 借用的用户日志块合计 → 重启后守恒、抽样读回一致；`CP-LEDGER` / `CP-DUR` | `RISK-BOOT-24` |

### 4.6 `/bench/speed/ftl_rw`（真 NAND，破坏性）

| 用例 | 参数 | 契约 / 覆盖点 | RISK |
|---|---|---|---|
| `ftl_rw` | `chunk=8/16/32` | 100 gblock（51,200 LBA = 25 MiB）host 读写；只判「跑不跑得完」，写 IO_ERROR 与读数据不符只计入 `err(wr/rd)`；`CP-CONC` | — |

> `/bench/speed/ftl_rw` 是基准，不是功能判据。与 `/bench/speed/fc_nand_rw` 的分工：FC 直接选器件
> 读写方式，给出速度上下限；FTL 只报实际路径与速度。

---

## 5. 风险追溯

> 只写「哪个 RISK 由哪些用例/人工流程覆盖」。**状态与历史见
> `docs/design/known_risks.md`**；若 ID 不在主表索引，按
> `docs/archive/known_risks_batches.md` 查已闭合历史。未列出的 RISK 可能仍是
> BOARD/OUT，本表不声称完整。

| RISK | 处置 | 用例 / 流程 |
|---|---|---|
| `RISK-ALLOC-06` | UT-P | `pba_roundtrip` |
| `RISK-ALLOC-09` | UT-P + UT-M | `alloc_free_pool_invariants`、`gc_trigger`、`gc_select_victim`、`gc_progress` |
| `RISK-BIT-01` | UT-P | `pba_cw_masks` |
| `RISK-MD-06` | UT-P + BOARD | `md_dir_ckpt_three_step`、`md_abort_keeps_victim`；回收注入未落地 |
| `RISK-MD-16` | UT-P + BOARD | `md_abort_keeps_victim` + `md_cp_read_fail_no_reclaim`（RAM 语义）；物理不可读 victim 的 fallback 成功路径仍未注入 |
| `RISK-MD-17` | UT-P | `md_status_error` |
| `RISK-BOOT-01` | UT-M | `preload_region_zero_and_high` |
| `RISK-BOOT-02` | UT-P + UT-M + BOARD | `md_abort_keeps_victim`；物理注入未落地 |
| `RISK-BOOT-03` | UT-M | `cp_probe_clean_media`、`cp_corrupt_refuse_format` |
| `RISK-BOOT-04` | UT-M + BOARD | `format_in_progress_resume`；三点掉电人工 |
| `RISK-BOOT-17` | UT-M + BOARD | `format_in_progress_resume`、`cp_probe_clean_media`；真空白片需另测 |
| `RISK-BOOT-22` | UT-M + BOARD | `format_in_progress_resume` |
| `RISK-BOOT-24` | UT-M + BOARD | `boot_restore_reconcile`；真掉电人工 |
| `RISK-GC-06` | UT-P + UT-M + BOARD | `gc_trigger`、`gc_select_victim`、`gc_decode_cw_lba`、`gc_progress` |
| `RISK-GC-14` | UT-P + BOARD | `gc_select_victim`；NO_SPACE 假报需 host 负载 |
| `RISK-WL-04` | UT-P + UT-M + BOARD | `wl_trigger_and_victim`、`wl_victim_threshold`、`wl_progress` |
| `RISK-L2P-03` | UT-P + UT-M + BOARD | `l2p_resident_states`、`cross_region_roundtrip` |
| `RISK-L2P-06` | UT-P | `l2p_miss_unmapped_evict`；越界分支待后续 |
| `RISK-L2P-09` | UT-P + UT-M + FI | `l2p_wal_gate`、`journal_barrier_cp_progress` |
| `RISK-L2P-10` | UT-M + FI | `journal_write_fail_wal_stall` |
| `RISK-JN-03` | UT-P + UT-M + FI | `md_journal_write_fail_clear`、`journal_write_fail_wal_stall` |
| `RISK-CR-02` | UT-P | `op_route`、`pba_roundtrip` |
| `RISK-WB-03` | UT-P + UT-M | `wbuf_append_read`、`wbuf_drop_reset_owners`、`wbuf_density`、`host_io_matrix` |
| `RISK-IF-04` | UT-P + UT-M | `wbuf_density`、`host_io_matrix` |
| `RISK-IF-05` | UT-P | `op_ctx_capacity` |
| `RISK-PEND-02` | UT-P | `pend_group_exhaustion` |
| `RISK-HOST-05` | UT-P + BOARD | `pend_confirm_park_timeout`、`pend_confirm_is_parked`；现场根因仍需板端日志 |
| `RISK-HOST-08` | UT-M + BOARD | `trim_no_resurrection`；极端时序人工 |
| `RISK-HOST-11` | UT-M + BOARD | `trim_no_resurrection`（强制 CP + journal-only 两段） |
| `RISK-CACHE-19` | UT-M + FI + BOARD | `cache_failc_abort` |
| `RISK-CACHE-20` / `RISK-CACHE-21` | UT-M | `cache_seq_cross_block`；`/bench/speed/ftl_rw` 仅作压力观测 |
| `RISK-CACHE-24` | UT-M + BOARD | `cache_seq_cross_block`、`cache_failc_abort` |
| `RISK-CACHE-27` | UT-M + FI | `cache_early_ack_read_visible`、`cache_wr_lookup`、`cache_failc_abort` |
| `RISK-CACHE-28` | UT-M + FI + BOARD | `cache_failc_abort`、`cache_failc_undo_read_error`、`cache_wr_lookup` |
| `RISK-CACHE-29` | UT-M | `cache_early_ack_flush_barrier` |
| `RISK-BBT-01` | UT-P + UT-M | `alloc_bbt_mark_bad`、`bbt_persist` |
| `RISK-BBT-06` | UT-P + FI-NA | `alloc_bbt_metadata_bad`；重复完成需注入 |
| `RISK-BBT-10` | UT-P + UT-M | `alloc_bbt_mark_bad`、`alloc_bbt_metadata_bad`、`bbt_persist` |
| `RISK-BBT-18` | UT-M | `bbt_persist`（先擦池规避）、`bbt_ring_reload`（两条 record 后重启） |
| `RISK-MIG-03` | UT-P + UT-M + BOARD | `alloc_valid_count_guard`、`trim_no_resurrection` |
| `RISK-RD-01` | UT-P + BOARD | `rr_health`、`rr_predictive_retire`；映射/介质人工 |
| `RISK-UT-02` | UT-M | `journal_barrier_cp_progress`、`trim_no_resurrection` |
| `RISK-UT-06` | UT-M | `bbt_persist`、`bbt_ring_reload`；每次全量跑永久标坏用户块 |

> §4 的 RISK 列只列该用例的主判据风险；§5 主表与下列补充映射不要求逐项写回 §4
> （状态见 `known_risks.md`）：
> `RISK-BOOT-05` → `md_cp_encoders` / `flush_reboot_readback` / `journal_barrier_cp_progress`；
> `RISK-CP-03` → `md_cp_init_timer` / `md_cp_encoders` / `journal_barrier_cp_progress`；
> `RISK-ALLOC-10` → `l2p_wal_gate` / `journal_barrier_cp_progress`；
> `RISK-HOST-09` → `trim_no_resurrection`；
> `RISK-BBT-02` → `format_in_progress_resume`（部分）；
> `RISK-ARCH-02` → `pba_roundtrip`（跨 LUN 未实现）；
> `RISK-NAND-06` → `format_and_roundtrip` / `gc_progress`（BOARD）；
> `RISK-BOOT-13` → `format_and_roundtrip`（BOARD）；
> `RISK-MIG-08` → `bbt_persist` 的恢复段（BOARD）。

> 处置词：`UT-P` 纯 RAM；`UT-M` 真 NAND；`FI` 已有测试镜像专属注入 seam；
> `FI-NA` 仍无注入 seam；`BOARD` 人工板端；`OUT` 性能/格式演进债，非 UT 目标。

---

## 6. 运行与证据

### 6.1 构建

```text
make all
make release
make tests UT=FTL
```

- `make all` → `debug.elf`；`make tests UT=...` → `debug_ut.elf`，产品与 UT 产物分名。
- ⚠ `UT=FTL` / `UT=FC` / `UT=HIL` 共用 `debug_ut.elf`；烧 FTL UT 前必须让
  `make tests UT=FTL` 是最后一次 UT 链接。
- 症状对照：`test` 命令不存在 ⇒ 烧了产品镜像；`test /ftl/` 打 `NO test run` ⇒ 镜像里
  是 FC-only 或 HIL-only 套件。两者都先查产物路径与横幅时间戳
  （`RISK-PLAT-05` / `RISK-PLAT-07`）。

### 6.2 运行

1. pure：`test --list` → `test /ftl/pure/`（可只跑子套件；安全）。
2. media：拔 USB → `test /ftl/media/`（破坏性，内部先 format）。
3. speed：`test /bench/speed/ftl_rw`（破坏性，单独运行）。
4. 抓完整串口日志；记录套件/用例数、耗时、失败项、跳过的用例名。
5. 收尾：`format` + POR，恢复可用的空盘。

### 6.3 日志与观测

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
  不是打印开关。想看 FTL/FC 内部过程必须另加 `--log-project info/debug`。
- 串口工具若开启「接收设置 → 自动换行」，会在静默期后补一个换行，把 munit 的名字行
  与 `[ OK ]` 拆成两行；这是采集端行为，不是固件输出（`RISK-PLAT-06`）。
- `l2p_miss_unmapped_evict` 在该 region 已驻留、无空行，或 cache 有
  DIRTY/FLUSHING/LOADING 行时按设计 SKIP；同一上电先跑过 media 写用例再跑
  `test /ftl/` 就会看到 SKIP。

### 6.4 证据归档

原始串口日志与运行元数据放 `build/test-results/`（`build/` 已被 `.gitignore` 整目录忽略），
**不放 `tests/`**：

```text
build/test-results/<YYYYMMDD-HHMMSS>_<build>_<target>/
  run.log      # 串口终端工具的 log（原始输出）
  meta.txt     # commit / 镜像横幅时间戳 / 命令 / 结果 / 备注
```

`meta.txt` 的四件套是证据判据（P6）：`commit` = 代码版本，`image` 的横幅时间戳 =
烧的是哪个镜像，`command` / `result` = 跑了什么、结果如何。需要长期留证的结论写
`docs/design/known_risks.md` 或 `docs/design/known_iss.md`，原始日志不进版本库。

---

## 7. 不覆盖项

- 真 POR / 掉电：人工断电流程，不进 munit（允许 `ftl_init` + `pfr_boot_recovery` 模拟，
  但只标「模拟重启」）。
- NAND 页/擦/写失败注入：当前无 `ftl_sync_*` 注入 seam（覆盖回收/目录检查点/abort
  不擦 victim 的注入部分）。
- 真空白片与格式化中断：真空白片需要全新芯片；`FORMAT_IN_PROGRESS` 的「save 后」分支
  已由 `format_in_progress_resume` 覆盖，erase 中 / CP marker 前掉电仍需人工
  （`RISK-BOOT-04/17`、`RISK-BBT-02`）。
- USB 主机行为、外部吞吐（H2testw）、长时 soak；内部 FC/FTL 速度见 `/bench/speed/*`。
- release 档特有行为：功能验收以 debug UT 为准；release `-O2` 全量 UT 也用于回归，
  但不能替代 debug 档的断言覆盖——release 下 `ASSERT` 被裁空，行为差异须按 H14
  另补运行期拒绝。
- 跨 LUN 条带/GC/WL（未实现）。
- HIL/USB 槽状态机、多 run 拆分/跨实例部分失败：见 `docs/design/known_iss.md`
  ISS-HIL-01 / ISS-HIL-03。

> 未实现/未验证的设计项统一登记在 `docs/design/known_iss.md`，本文不复制清单。

---

## 8. 维护规则

- **新增/删除用例**：同批更新 §4 对应表 + 测试源注释；不得只改源或只改文档。
- **风险状态变化**：只改 `docs/design/known_risks.md`；§5 仅在「映射关系」变化时改。
- **API / 夹具 / 地址 / 位图变化**：同批更新 §2/§3。
- **运行结果**：写 `build/test-results/` 与 `known_risks.md` 历史；§9 只留最近快照。
- **禁止**：§5 写状态词；§4 复制实现细节；文档头写「N 函数 / M 次运行」快照。
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

---

## 9. 最近验收快照

> 只作时间点快照；状态以 `docs/design/known_risks.md` / `docs/design/known_iss.md` 为准。
> 构建门禁不在此表：`make all` / `make release` / `make tests UT=FTL` 按 R2 每次改动执行。

| 日期 | 镜像横幅 | 命令 | 结果 | 证据 |
|---|---|---|---|---|
| 2026-11-11 | 21:44:15 | `test /ftl/ --log-visible debug` | 95/95，0 skip，235.63 s | `docs/design/known_iss.md` ISS-UT-05 |

> 2026-11-12 另有 `/ftl/media/` **30/30** 常规回归与 `/bench/speed/ftl_rw` **3/3，err=0**；
> 它们不是全量 `/ftl/` 快照，故不替换上表（记录见 `docs/design/known_risks.md`
> RISK-IF-04 / RISK-CACHE-36/37 的历史/验证行）。
> 每次全量运行后只更新上表一行，不在此展开用例级结果。

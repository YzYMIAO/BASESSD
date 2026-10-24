# FTL 单元测试设计（板端）

> 状态：**已落地并上板**（2026-10-24，镜像 22:08:11）：`/ftl/` **85/85 通过**、61.05 s。
> 2026-10-25 cache read 修复后已重跑全量（debug 01:09:23 镜像）：**85/85 通过、61.20 s**，
> media 读用例已包含 ≥16 LBA 的 cache read 路径。
> `/speed/ftl_rw` 不属于 `/ftl/` 树；`/ftl/` 仍为 85 次运行。
> 剩余未覆盖：M-11 的**真介质注入**（construct 不可读 victim 页）、CP `VERSION_MISMATCH` 分支、
> 真 POR/空白片/NAND 写擦失败注入。
> 多圈 BBT ring 的 reload 选旧 record 已登记 `RISK-BBT-18`；Journal 压力下的 TRIM 持久化时序
> 见 `RISK-HOST-11`；munit 的 SKIP/FAIL 行格式修复见 `RISK-UT-01`。
> 本文替代已归档的 `docs/archive/ftl_ut_munit.md`（后者对应 v0.75 API，已失效）。
> 被测设计见 `docs/design/ftl_design.md`；风险清单见 `docs/design/known_risks.md`。

---

## 1. 目的与原则

1. **测试只在目标板跑**（`CLAUDE.md` R4）：`make tests UT=FTL` 生成固件镜像，串口 CLI `test` 执行；不做 host 侧模拟。
2. **两层并重**：
   - **pure（RAM）**：测模块内部功能与不变量。真 NAND 只能打到正常路径，池闸门、组槽耗尽、同 LBA 合并、L2P 三态、Directory 主备提交等分支必须在 RAM 里构造。
   - **media（真 NAND）**：测跨模块契约与恢复路径，走 FTL host API + 真实 FC/NAND 完成。
3. **模拟规模小、结构体同型**：pure 用例使用生产数组/结构体，只填少量状态（8 gblock、4 页、2~4 cache 行），不引入第二套编译期几何。
4. **不新增生产 seam**：只用现有 `time_us`、`l2p_owner_fn_t`、`metadata_ring_probe_fn`、`jrn_replay_ctx_t`、`pending_confirm_handler_t`。
5. **证据分级**：默认只打用例名 + 结果；`--log-visible info` 打简单前后对比（输入→输出关键值），
   `--log-visible debug` 打完整对比（逐项 inputs/outputs）；项目日志由 `--log-project` 单独控制，
   测试期间默认静默。证据必须能回答「断言比较的是哪两个值」，否则通过也不能服人（C8）。
6. **状态如实**：未上板的用例标「未上板」；失败必须打印用例名与不变量计数；不复用 H2testw 结果冒充 UT。

---

## 2. 分层与套件树

| 层 | 路径 | 碰 NAND | 内容 |
|---|---|---|---|
| L0 | `/ftl/pure/` | 否 | 模块内部功能与不变量（RAM，测试自有对象） |
| L1 | `/ftl/media/e2e/` | 是 | 格式化、主机写读、wbuf、flush、TRIM、模拟重启 |
| L2 | `/ftl/media/e2e/` | 是 | 跨模块契约、持久化、GC/WL 搬移、BBT（与 L1 同套件，按用例名区分） |
| L3 | 人工流程 | 是 | 真 POR、坏块注入、NAND 时序、USB、H2testw |

套件树（实际接线）：

```
/ftl/
  pure/
    mem/    allocator / BST / BBT / wear
    md/     metadata ring/Directory / checkpoint / journal
    host/   pending / group / wbuf / ftl_op / bg
    io/     L2P 驻留行 / GC / WL / RR 策略
  media/
    e2e/    格式化、写读、wbuf、flush、TRIM、模拟重启、GC/WL、BBT
```
`/speed/` 根套件（与 FC 侧共用 `test_speed.c`）：
```
/speed/
  fc_nand_rw   FC 原始块 25 MiB 读写（normal / cache；仅 UT=FC 或 both）
  ftl_rw       FTL host API 25 MiB 读写（normal / cache；仅 UT=FTL 或 both；破坏性）
```

> media 侧只用 **一个 `e2e/` 子套件**：用例之间共享一次 format 与 `s_media_ready` 状态，
> 拆成 l2p/md/cp/bbt 多个套件会让「哪条用例负责准备介质」变成跨套件隐式依赖。
> 覆盖范围由用例名与 §4.5 表表达，不再按计划阶段拆套件。

执行顺序：`pure/` → `media/`。`test /ftl/pure/` 安全；`test /ftl/media/` 与裸 `test` **破坏介质**。

**资源钩子**：每条用例都挂 setup/teardown（与 FC UT 的 `test_setup` / `test_tear_down` 同角色）：
- pure → `ftl_ut_pure_setup`（重建测试自有 RAM 表）/ `ftl_ut_pure_teardown`（断言活实例 pending/group/ctx/FC 归零）；
- media → `media_setup` / `media_teardown`（活实例存在性检查；整片 format 这种重资源仍由用例内的 `media_prepare` 首次获取，避免只跑 `media_live` 也擦片）。

---

## 3. 夹具与隔离

### 3.1 活实例

- `ftl_ut_live()` 返回 `g_ctrl.ftl[0][0][0]`（`extern ssd_controller_t g_ctrl`）。`ftl_ctx_t` 约 287 KB，无法复制。
- pure 用例**不 reset 活实例的 allocator/metadata**；使用测试自有对象。
- 需要活实例 L2P 行的用例，只借 2~4 行，teardown 原样恢复。

### 3.2 测试自有 RAM 对象（小状态）

| 对象（`ftl_ut_common.c`） | 尺寸（`nm -S`） | 放置 | 用法 |
|---|---|---|---|
| `s_ut_blocks[FTL_BLOCK_TOTAL]` | 0x14000 = 80 KB | SRAM12 | 只用 gblock 0..7，其余 NONE |
| `s_ut_free[FTL_BLOCK_TOTAL]` | 0x2000 = 8 KB | SRAM12 | 只填 0..7 |
| `s_ut_closed[SUPER_BLOCK_LIST_MAX]` | 0x8000 = 32 KB | SRAM12 | 只填前 4 条 |
| `s_ut_op[PLANE_NUM]` / `s_ut_spare[PLANE_NUM]` | 0x330 / 0xC8 | SRAM12 | 只填前 2 项 |
| `s_ut_md`（`md_dir_cache_t`） | 0x83D8 ≈ 33 KB | AXI `.bss` | 只填 2~3 个 region |
| `s_ut_alloc`（`alc_t`） | 0x56C ≈ 1.4 KB | AXI `.bss` | 指向上面各表 |
| **合计** | **≈ 155 KB** | | 链接期区域溢出即为硬门禁 |

若链接放不下：退回借用活实例的表 + 小状态，不缩减数组尺寸（结构体必须与生产同型）。
当前实测放在 SRAM12 + AXI `.bss`，`make tests UT=FTL` 链接通过。

### 3.3 伪时钟

- 复用 `ssd_platform_register` / `ssd_platform_ops_t.time_us`；保存原 ops，用例 teardown 恢复。
- 仅 pure 时间策略用例（bg 调度、pending park 超时、GC/WL 节拍）安装；真 NAND I/O 一律用真实时钟。

### 3.4 结束不变量

每个用例 teardown 断言：

- `pend_slot_get_count()==0`、`pend_group_get_count()==0`、`ftl_op_ctx_count()==0`；
- FC 资源统计 `sq/hsq/cq/command/descriptor/buffer` 全 0；
- migrate/checkpoint/metadata/journal/wbuf/L2P 均 idle；
- allocator 对账：`state==FREE ⟺ 在 Free Pool`，BAD 不在任何池/Closed，计数不越界。

### 3.5 真 NAND 安全模型

- 破坏性：`/ftl/media/` 入口 `format`（`verify=false`）一次；跑完人工 `format` + POR。
- LBA 窗口：容量尾部 8192 个 LBA，按 8 LBA 对齐，用例互不重叠。
- 运行前检查 `cli_host_busy()` / `usb_msc_host_busy()`，并要求拔掉 USB。
- 不恢复 host 数据；结束后打印提示。

### 3.6 模拟重启夹具（`ftl_ut_reboot_from_media`）

顺序是承重的，缺一步就在板端以断言/日志风暴的形式暴露：

1. `ftl_ut_quiesce()`：泵到 FC 池、CQ、`ftl_op_ctx_count()` 全 0（否则旧 CP 的迟到完成撞 `checkpoint.c` 的 `h->waiting != 0`）；
2. `fc_init()` 后必须 `nand_dispatch_set_armed(1)`（`fc_init` 会清派发放行闩锁）；
3. `ftl_op_init()` + 恢复实例路由表（`ftl_op_route_set`）；
4. 先拷出 `storage` 指针（`ftl_init` 会 memset 整个上下文），再 `ftl_init` + `l2p_init`；
5. `pfr_boot_recovery(ftl, &result)`。

只标「模拟重启」，**不标 POR**（真掉电仍是人工流程）。

### 3.7 media 用例顺序与状态依赖

- `media_prepare()` 只在第一次调用时执行 `ftl_ut_media_format()`；后续用例共享已格式化的介质。
- 写介质的重用例（`host_io_matrix` / `host_io_random`）排在 TRIM 用例**之后**：
  Journal 压力下 TRIM 的持久化时序有问题（`RISK-HOST-11`），先跑 TRIM、再压大量重写
  是其当前缓解；不要为了“参数矩阵在前”调换二者顺序。
- `trim_no_resurrection` 必须 `trim → flush → 强制 CP → 模拟重启`：TRIM 只记 Journal，
  不 flush 就重启会「复活」（`RISK-HOST-08/11`）。等待 CP 时要等 `phase == CP_IDLE`，
  CP 基准要在**用例起点**取（写入/trim 之前），并在调用 `cp_pool_find_latest` **之前**保存目标序号
  （否则断言恒真，`RISK-UT-02`）。
- `bbt_persist` 先擦 BBT 池并清 RAM 副本再标坏：多圈 ring 的 save→reload 会选旧 record
  （`RISK-BBT-18`），clean-pool 路径才能稳定验证持久化。
- `format_in_progress_resume` 用**更大的 BBT sequence**（`0xFFFFFF00` 起步）写一条带
  FORMAT_IN_PROGRESS 的 record，让它在真实 bootstrap 扫描中胜出（隔离 RISK-BBT-18 / BOOT-22），
  再擦 CP 池制造“格式化未完成”状态；**不擦 BBT 载体**（擦 spare 会在失败路径上丢坏块图，
  RISK-UT-03）。用例末尾跑完整 format 恢复介质，因此放在 media 套件**最后**。

---

## 4. 用例清单

### 4.1 `/ftl/pure/mem/`（allocator / BST / BBT / wear）

11 个用例，参数化后 14 次运行。

| 用例 | 参数 | 覆盖 |
|---|---|---|
| `address_roundtrip` | — | LBA 与 PBA 字段编解码往返、`ftl_gblock` 的 plane-major 编号（RISK-ALLOC-06） |
| `free_pool_invariants` | — | 入池/出池账本、`state==FREE ⟺ 在池`、出池不改 state、OP refill LIFO |
| `super_block_open_close` | `mode=hot\|cold\|high_wear` | 双 plane / 温度 / open-close-rewind |
| `closed_list_add_drop` | — | swap-remove、部分 plane 释放 |
| `valid_count_guard` | — | BAD 后减计数不回声（RISK-MIG-03） |
| `op_spare_pools` | — | OP/Spare per-plane 独立计数、容量上限、Spare LIFO 与空池 pop 失败 |
| `cw_masks` | — | `ftl_cw_bit` / span / plane mask（RISK-BIT-01） |
| `erase_stats` | — | erase 计数与时间戳 |
| `bbt_mark_bad` | `plane=0\|1` | 幂等、Spare 只减一次（RISK-BBT-01/10） |
| `bbt_metadata_bad` | — | 元数据块标记坏 + 池重建标志 |
| `alloc_spare_full_refill_none` | — | Spare 满拒收、pop 到空、Free 空时 refill 返回 false |

### 4.2 `/ftl/pure/md/`（metadata / checkpoint / journal）

13 个用例，均无参数。

| 用例 | 覆盖 |
|---|---|
| `metadata_idle` | Directory 初始态 / live 计数 |
| `ring_geometry` | ring 记录数、地址、plane 掩码 |
| `directory_commit` | 主/备双副本提交、`dirty_gen` |
| `journal_append` | init/reserve/append 序号推进、pending 置位、未到水位不请求 CP |
| `ring_locate` | 假 probe 下 locate 选最新记录 |
| `anchor_three_step` | Anchor prepare/page/commit 门槛（RISK-MD-06 纯 RAM 部分） |
| `log_alloc_rotate_release` | 日志区分配、轮转、回收 |
| `cp_init_timer` | CP 初始化与定时触发复位 |
| `md_status_error` | `status.error` 清零点特征化（RISK-MD-17） |
| `journal_write_fail_clear` | 写失败 pending 与 CP 清账（RISK-JN-03 纯 RAM 部分） |
| `cp_encoders` | 快照 / 磨损页 / word 打包校验 |
| `journal_truncate_idempotent` | 水位不足/0 保护、下一块选择、重复 plan=ALREADY、`commit(NONE)` 不清预擦块 |
| `md_abort_keeps_victim` | 压缩写阶段取不到槽 ⇒ abort：victim 不登记回收、live 不丢、job 回 IDLE（RISK-MD-06/16/BOOT-02 的 RAM 语义部分） |

### 4.3 `/ftl/pure/host/`（pending / group / wbuf / ftl_op / bg）

14 个用例，参数化后 17 次运行。

| 用例 | 参数 | 覆盖 |
|---|---|---|
| `host_idle` | — | pending/group/ctx 全空闲基线 |
| `pending_slots` | — | 槽满返回 NULL、按 command_id 查找/移除、移除后可再分配 |
| `group_exhaustion` | — | 组槽耗尽返回 false 且不覆盖（RISK-PEND-02） |
| `group_complete_once` | — | 多 owner 只完成一次 |
| `group_failure_aggregation` | — | 子操作失败后 `agg_status` 粘滞，后续 OK 不清错 |
| `wbuf_append_read` | — | 单 CW append/read、同窗口稀疏两位、owners 与组在飞计数 |
| `wbuf_drop_reset_owners` | — | DROP 清空页后 owners 结算一次（OK）、RESET 按 IO_ERROR 结算 |
| `wbuf_density` | `n=1\|2\|4\|8` | pair 布局、稀疏 CW 位图（RISK-WB-03/IF-04） |
| `op_ctx` | — | ctx add/take/remove 生命周期 |
| `op_ctx_capacity` | — | 旁表容量与同 id 覆盖（RISK-IF-05） |
| `bg_engine` | — | 任务表长度 = `BG_TASK_COUNT`、`level_select` 返回合法档位 |
| `op_route` | — | dim→(ch,ce,lun) 路由（RISK-CR-02） |
| `bg_level_select` | — | 负载档位边界 |
| `host_submit_invalid_args` | — | count=0 / buffer=NULL / LBA 越界 / ftl=NULL 均在分配池资源前拒绝 |

### 4.4 `/ftl/pure/io/`（L2P / GC / WL / RR）

10 个用例，参数化后 12 次运行（`l2p_miss_unmapped_evict` 可能按状态 SKIP）。

| 用例 | 参数 | 覆盖 |
|---|---|---|
| `policy_entry` | — | GC/WL 触发位图入口 |
| `gc_trigger` | — | 空池必 EMERGENCY、回到 TARGET 以上不触发、低于 TARGET 只 WATERMARK |
| `gc_select_victim` | `mode=off\|idle\|emergency` | 三模式、valid 上界（RISK-GC-06/14） |
| `gc_decode_cw_lba` | — | OK / EMPTY / CORRUPT / 空洞四态 |
| `wl_trigger_and_victim` | — | UBI 判据 `max_free_ec − min_used_ec ≥ 200` |
| `l2p_resident_states` | — | 驻留行 CLEAN/DIRTY/FLUSHING/LOADING、批量读 |
| `l2p_miss_unmapped_evict` | — | miss→AGAIN→CLEAN+未映射、写后读回；cache 有脏/在飞行时按设计 `MUNIT_SKIP` |
| `wl_victim_threshold` | — | 低有效 CW 跳过、≥70% 接受 |
| `rr_health` | — | ECC 趋势、health 不降级 |
| `rr_predictive_retire` | — | 预测巡检命中并退役 |

### 4.5 `/ftl/media/e2e/`（真 NAND）

15 个用例，参数化后 29 次运行。

| 用例 | 类型 | 参数 | 覆盖 |
|---|---|---|---|
| `media_live` | 冒烟 | — | 活实例、池账本基线 |
| `format_and_roundtrip` | e2e | — | format → 写 → flush → 读回（含一次 format 10–21 s，视优化档） |
| `cross_region_roundtrip` | e2e | — | 跨 region 写读、L2P 分区 |
| `flush_reboot_readback` | e2e | — | flush → 模拟重启 → 读回（恢复路径正常分支） |
| `trim_no_resurrection` | e2e | — | trim → flush → 强制 CP（基准取用例起点，等 job IDLE + Header 落盘）→ 模拟重启不复活（RISK-HOST-08/11、UT-02） |
| `preload_region_zero_and_high` | l2p | — | region 0 与高 region 预载不别名（RISK-BOOT-01） |
| `journal_barrier_cp_progress` | e2e | — | flush 后无 pending、CP seq 前进且 Header 落盘、重启恢复（RISK-UT-02） |
| `gc_progress` | e2e | — | 关闭 open SB 造 victim → urgent GC → moved/reclaimed 增长且数据可读（RISK-GC-06/MIG-03） |
| `wl_progress` | e2e | — | 构造擦除差 → 一轮 WL（RISK-WL-04） |
| `cp_probe_clean_media` | cp | — | `cp_media_status_probe` 判 CURRENT、不误格式化 |
| `host_io_matrix` | e2e | `count=1\|7\|8\|9\|64` × `off=0\|3\|7` | 奇尾 / 跨 pair / 跨 region 写读矩阵 |
| `host_io_random` | e2e | — | 固定种子 8 轮随机 LBA/长度写读 |
| `bbt_persist` | bbt | — | 标坏 → save → 模拟重启仍 BAD（RISK-BBT-01/10；clean-pool 规避 RISK-BBT-18）。⚠ 该 BAD 持久化且 `format` 不清除 ⇒ **每次运行永久消耗 1 块**（RISK-UT-06） |
| `format_in_progress_resume` | bbt | — | BBT 置 FORMAT_IN_PROGRESS + 擦 CP 池 → 重启后判 NO_RECORD/继续格式化（RISK-BOOT-04/17 的续做分支）；跑完完整格式化恢复介质 |
| `cp_corrupt_refuse_format` | cp | — | 擦 CP 池 + 写非 FF 页 → `cp_media_status_probe=IO_ERROR`、`pfr_media_check_format` 拒绝格式化（RISK-BOOT-03） |

### 4.6 `/speed/ftl_rw`（真 NAND，破坏性）

| 用例 | 参数 | 覆盖 |
|---|---|---|
| `ftl_rw` | `mode=normal\|cache` | 100 gblock（51,200 LBA = 25 MiB）host 读写速度；首次运行先完整 format。`normal`：8-LBA chunk 写 + 8-LBA 读（cache 路径不触发）。`cache`：32-LBA cache 写（计时，断言 `wb_batches>0`）→ 8-LBA 普通写重铺同一范围（不计时，让物理页严格按 LBA 顺序落盘）→ 32-LBA cache 读（计时，**断言 `rd_batches>0`**）→ 8-LBA 普通读基线。cache read 未触发时打印 `DIAG ftl` 窗口（LBA0..15 的 PBA/plane/topo 判据）并失败。**2026-10-25 板端实跑已暴露 RISK-HOST-13 的根因**（`cache_read_collect` 跨 plane 误用 `pba_page()`），修复已上板（2026-10-25）：`rd_path=cache`、`rdc` batches=1600 |

输出只在 `--log-visible info/debug` 可见。运行：
`test --log-visible info /speed/ftl_rw`。FC 侧的 `/speed/fc_nand_rw` 与本套件都有破坏性，
建议分次运行；跑完 media 由用例自行格式化。

未落地（按板端/注入项处理）：

- M-11 的**真介质注入**部分：构造不可读 victim 页，断言压缩 abort 后 victim 的
  `erase_count` 不变、另一副本仍可读（RISK-MD-06/16/BOOT-02）。RAM 语义已由
  `md_abort_keeps_victim` 覆盖；物理“不擦”仍需白盒构造或注入 seam。
- **真空白片**：需要全新芯片；擦净冒充空片会毁掉出厂坏块标记，不能作为 UT 判据
  （RISK-BOOT-17）。
- NAND **program/erase 失败**注入：当前无 `ftl_sync_*` seam（RISK-BBT-02/JN-03）。

## 5. 风险覆盖矩阵（与 `known_risks.md` 同步）

处置：`UT-P` 纯 RAM；`UT-M` 真 NAND；`BUILD` 构建/测试框架；`BOARD` 人工板端流程；
`FI-NA` 当前无故障注入 seam；`OUT` 性能 / 格式演进债，非 UT 目标。

| ID | 状态 | 处置 | 用例 / 流程 | 备注 |
|---|---|---|---|---|
| RISK-MD-06 | 已修未上板 | UT-P + BOARD | `anchor_three_step`；回收注入未落地 | 需释放日志块 + Anchor 落盘 |
| RISK-PERF-01 | 待后续 | OUT | — | 重复全表扫描 |
| RISK-PERF-02 | 待后续 | OUT | — | 单 plane 目的写 |
| RISK-GC-05 | 待后续 | OUT | — | GC bitmap 未实现 |
| RISK-GC-06 | 部分已上板（2026-10-25：gc_trigger/select/decode/progress 已过；WL 5 s 节拍未直接断言） | UT-P + UT-M + BOARD | `gc_trigger`、`gc_select_victim`、`gc_decode_cw_lba`、`gc_progress` | 并发 host 写/POR 人工 |
| RISK-JN-03 | 待上板 | UT-P + FI-NA | `journal_write_fail_clear` | 失败时序需注入 |
| RISK-MIG-03 | 已修/部分上板 | UT-P + UT-M + BOARD | `valid_count_guard`、`trim_no_resurrection` | release 回绕负例未做 |
| RISK-MIG-07 | 已复现 | BOARD | — | 需 H2testw / `fc=` 归因 |
| RISK-MIG-08 | 待定位 | UT-M 部分 + BOARD | `bbt_persist` 的恢复段 | 全盘 dangling 需 H2testw |
| RISK-GC-14 | 待上板 | UT-P + BOARD | `gc_select_victim` | NO_SPACE 假报需 host 负载 |
| RISK-BBT-01 | 部分已上板（2026-10-25：bbt_mark_bad/bbt_persist clean-pool 已过；写飞行中再标坏未验） | UT-P + UT-M | `bbt_mark_bad`、`bbt_persist` | 幂等/持久化 |
| RISK-BBT-02 | 待上板 | UT-M 部分 + FI-NA | —（format-in-progress 未落地） | 读失败需注入 |
| RISK-BBT-04 | 待后续 | OUT | — | 格式版本演进债 |
| RISK-BBT-06 | 待后续 | UT-P 部分 + FI-NA | `bbt_metadata_bad` | 重复完成需注入 |
| RISK-BBT-18 | 待定位 | UT-M 部分 | `bbt_persist`（先擦池） | 多圈 ring locate 选旧 record |
| RISK-BBT-10 | 部分覆盖（2026-10-25：`FORMAT_IN_PROGRESS` 恢复已由 UT 覆盖；factory-scan/format_erase 未做） | UT-P + UT-M | `bbt_mark_bad`、`bbt_metadata_bad`、`bbt_persist` | factory-scan / FORMAT_IN_PROGRESS 缺 |
| RISK-BOOT-01 | 已闭合（2026-10-25：`preload_region_zero_and_high` 上板 85/85） | UT-M | `preload_region_zero_and_high` | region 0/非 0 预载 |
| RISK-BOOT-02 | UT-P 已上板（2026-10-25：`md_abort_keeps_victim` RAM 语义）；物理注入未做 | UT-P + UT-M + BOARD | `md_abort_keeps_victim`；物理注入未落地 | carve 不选中已写块 |
| RISK-BOOT-03 | 待上板 | UT-M | `cp_probe_clean_media`、`cp_corrupt_refuse_format` | 版本不符分支仍未覆盖 |
| RISK-BOOT-04 | 部分上板（2026-10-25：`format_in_progress_resume` 覆盖“save 后”断电点；erase 中/CP marker 前未做） | UT-M + BOARD | `format_in_progress_resume` | 三点掉电人工 |
| RISK-BOOT-05 | 待上板 | UT-P + UT-M + BOARD | `cp_encoders`、`flush_reboot_readback`、`journal_barrier_cp_progress` | 真掉电人工 |
| RISK-BOOT-13 | 待定位 | UT-M smoke + BOARD | `format_and_roundtrip` | HardFault 归因人工 |
| RISK-MD-16 | 待定位 | UT-P + BOARD | `md_abort_keeps_victim`（RAM 语义） | 物理不可读 victim 页仍需注入 |
| RISK-MD-17 | 待决策 | UT-P | `md_status_error` | 固定当前 `status.error` 语义 |
| RISK-BOOT-17 | 信息/待决策 | UT-M 部分 + BOARD | `format_in_progress_resume`、`cp_probe_clean_media` | 真空白片需另测 |
| RISK-L2P-03 | 待上板 | UT-P + UT-M + BOARD | `l2p_resident_states`、`cross_region_roundtrip` | 命令池压力人工 |
| RISK-L2P-05 | 待上板 | FI-NA + BOARD | — | 需精确命令池饥饿 |
| RISK-L2P-06 | 部分已改 | UT-P | `l2p_miss_unmapped_evict` | 越界分支待后续 |
| RISK-CP-03 | 待上板 | UT-P + UT-M + BOARD | `cp_init_timer`、`cp_encoders`、`journal_barrier_cp_progress` | 逐阶段掉电人工 |
| RISK-CP-04 | 待定位 | FI-NA + BOARD | — | 持续写压与池饥饿 |
| RISK-WL-04 | 待上板 | UT-P + UT-M + BOARD | `wl_trigger_and_victim`、`wl_victim_threshold`、`wl_progress` | POR/soak 人工 |
| RISK-WL-05 | 待后续 | OUT | — | 重复全表扫描 |
| RISK-IF-04 | 待上板 | UT-P + UT-M | `wbuf_density`、`host_io_matrix` | 布局/位图/迟到完成 |
| RISK-IF-05 | 已闭合（2026-10-25：`op_ctx_capacity` 上板 85/85） | UT-P | `op_ctx_capacity` | 64 项旁表 |
| RISK-HOST-05 | 待定位 | BOARD | —（停泊超时纯 RAM 用例未落地） | 根因需现场日志 |
| RISK-HOST-08 | 部分上板 | UT-M + BOARD | `trim_no_resurrection` | 极端时序人工 |
| RISK-HOST-11 | 待定位 | UT-M + BOARD | `trim_no_resurrection`（顺序规避） | Journal 压力下 trim 持久化 |
| RISK-HOST-09 | 待后续 | UT-M 特征化 + BOARD | `trim_no_resurrection` | 并发当前不可达 |
| RISK-CR-02 | 待上板 | UT-P | `op_route`、`address_roundtrip` | dim 分解/路由 |
| RISK-CR-03 | 已闭合（2026-10-25：`/ftl/` 85/85 行为回归） | UT-P + UT-M | `op_route`、media 回归 | 分 handler/孤儿完成 |
| RISK-WB-03 | 待上板 | UT-P + UT-M | `wbuf_append_read`、`wbuf_density`、`host_io_matrix` | 记账/位图/一次编程 |
| RISK-LOG-02 | 待上板 | BOARD | — | 需 UART 速率观测 |
| RISK-ARCH-02 | 待后续 | UT-P + OUT | `address_roundtrip` | 跨 LUN 未实现 |
| RISK-ARCH-06 | 已闭合（2026-10-25：`/ftl/` 85/85 + H2testw 415 MB 回归） | UT-M + BOARD | media 回归 | 真 POR 人工 |
| RISK-USB-02 | 待上板 | BOARD | — | 拔线/复位/重枚举 |
| RISK-USB-05 | 待上板 | BOARD | — | 需 USB 抓包看 FUA |
| RISK-PLAT-07 | 信息/待决策 | BUILD | §6 构建顺序 | 共用 `debug.elf`，最后一次构建生效 |
| RISK-NAND-04 | 已闭合（2026-10-25） | BOARD | — | USB 写 200 KB/s 已消除（release -O2 H2testw 843/889 KB/s） |
| RISK-NAND-05 | 待定位 | BOARD | — | 物理介质失效 |
| RISK-NAND-06 | 待定位 | UT-M 部分 + BOARD | `format_and_roundtrip`、`gc_progress` | 全片 re-scan 人工 |
| RISK-ALLOC-06 | 待后续 | UT-P | `address_roundtrip` | 固定 `ftl_gblock` 截断行为 |
| RISK-BIT-01 | 待后续 | UT-P | `cw_masks` | 掩码构造器交叉校验 |
| RISK-RD-01 | 观察中 | UT-P + BOARD | `rr_health`、`rr_predictive_retire` | 映射/介质人工 |
| RISK-PEND-02 | 已闭合（2026-10-25：`group_exhaustion` 上板 85/85） | UT-P | `group_exhaustion` | 组槽耗尽 |
| RISK-UT-01 | 已改未上板 | BUILD | 下次跑出 SKIP 的会话 | SKIP/FAIL 行格式 |
| RISK-UT-02 | 已上板 | UT-M | `journal_barrier_cp_progress`、`trim_no_resurrection` | CP 判据已用 `cp 1->2` / `cp 2->3` 板端验证 |
| RISK-UT-05 | 信息 | BUILD | — | munit 计时按 10.737 s（DWT CYCCNT）取模；速度只看行内 `*_ms` |
| RISK-UT-06 | 信息 | UT-M | `bbt_persist` | 每次全量跑永久标坏 1 用户块（256 KiB）；反复跑会缩容量 |
| RISK-HOST-13 | 已闭合（2026-10-25） | UT-M + OUT | `/speed/ftl_rw`（cache 读断言）、media 读用例 | cache read 判据修复已上板（`batches=1600`） |
| RISK-PERF-03 | 信息 | OUT | `/speed/ftl_rw` | cache read 生效后读速无提升 |
| RISK-PERF-04 | 信息 | OUT | `/speed/*` | 数据面 CPU-bound，阵列利用率见 RISK-PERF-04 |

## 6. 运行流程

1. 编译：`make tests UT=FTL`（生成 `build/firmware/debug/bin/debug.{elf,bin}`）。
   ⚠ **测试镜像必须是最后一次构建**：所有配置共用同一 `debug.elf`/`debug.bin`，
   `make all` / `make release` / `make tests UT=FC` 都会覆盖它；烧 FTL UT 前必须让
   `make tests UT=FTL` 是最后一次链接。静态库已按配置分目录（`lib_ut_FTL`）。
   症状对照：`test` 命令不存在 ⇒ 无 UT 定义；`test /ftl/` 打 `NO test run` ⇒ 镜像里是
   FC-only 套件。两者都先查构建顺序与横幅时间戳（`RISK-PLAT-05`、`RISK-PLAT-07`）。
2. 烧写：JLink（见 `.claude/skills/board-run/SKILL.md`）；核对横幅时间戳。
3. pure：`test --list` → `test /ftl/pure/`（可只跑子套件）。
4. media：拔 USB → `test /ftl/media/`（破坏性，内部先 `format`）。
5. 抓完整串口日志；记录套件/用例数、耗时、失败项、跳过的用例名。
6. 收尾：`format` + POR，恢复可用的空盘。
7. 证据归档到 `docs/archive/`（按需）。

日志与观测：

- **三层输出互相独立**，不要再把「看证据」和「看项目日志」混在一个开关里：

  | 开关 | 额外可见内容 |
  |---|---|
  | 默认（无开关） | 用例名 + `[ OK ]/[ SKIP ]` + 耗时 |
  | `--log-visible info` | `[EV]` **简单前后对比**（每个用例一条 summary，含输入→输出关键值） |
  | `--log-visible debug` | `[EV]` **完整对比**（inputs / outputs / 中间计数逐项列出） |
  | `--log-project info\|debug` | 额外打开 FTL/FC **项目日志**（默认 `LOG_OFF`，避免 boot/format/GC 刷屏） |

- 项目日志（`LOG_*`）在测试期间默认整档关闭；munit 的 `--log-visible` **每次 `test`
  调用重置为 ERROR**（不跨调用粘滞），`--log-fatal` 是「达到该级别即中止」阈值，不是打印开关。
- 串口工具若开启「接收设置 → 自动换行」，会在**静默期后**补一个换行，把 munit 的
  名字行与 `[ OK ]` 拆成两行；这是采集端行为，不是固件输出（`RISK-PLAT-06`）。
- `l2p_miss_unmapped_evict` 在 cache 有 DIRTY/FLUSHING/LOADING 行时按设计 SKIP；
  同一上电先跑过 media 写用例再跑 `test /ftl/` 就会看到 SKIP。

预期（2026-10-24 板端实测）：`test /ftl/` **85 次运行、约 61 s**（含 format 10.5 s；
85/85 通过，或 84 通过 + 1 SKIP 视 cache 状态）。

---

## 7. 不覆盖项

- 真 POR / 掉电：人工断电流程，不进 munit（允许 `ftl_init`+`pfr_boot_recovery` 模拟，但只标“模拟重启”）。
- NAND 页/擦/写失败注入：当前无 `ftl_sync_*` 注入 seam（覆盖回收/Anchor/abort 不擦 victim 的注入部分）。
- 真空白片与格式化中断：`format_in_progress_and_used_medium_refusal` 需要真空白片或可注入的
  `FORMAT_IN_PROGRESS` 介质状态（RISK-BOOT-04/17、RISK-BBT-02）。
- USB 主机行为、外部吞吐（H2testw）、长时 soak；内部 FC/FTL 速度见 `/speed/fc_nand_rw` 与
  `/speed/ftl_rw`（破坏性，单独运行）。
- release 档特有行为：功能验收用 debug UT；release `-O2` UT 仅用于速度测量（`/speed/`），不作为功能验收。
- 跨 LUN 条带/GC/WL（未实现）。

---

## 8. 维护规则

- 新增 FTL 模块或 `ftl_design.md` §3 小节 → 同步 §4 用例表。
- 新增/重开 `known_risks.md` 条目 → 在 §5 增加一行处置。
- 新增 pure 用例：禁止调用 `ftl_init` / `pfr_*` / 任何 `ftl_sync_*` / NAND 命令。
- 新增 media 用例：必须走 `media_prepare()`（首次调用做一次 format）、使用 LBA 尾窗、不得依赖 host 文件系统。
- 每次改 API 后先跑 `make tests UT=FTL`；测试源与生产同仓维护，不再冻结。
- 新增/修改用例必须保留「覆盖 / 判据 / 备注」注释块（对齐 `test_ftl_pure.c` /
  `test_ftl_media.c` 现有风格），并同步 §4 用例表与本文件开头的运行数。

---

## 9. 验收状态

| 项 | 结果 | 证据 |
|---|---|---|
| 编译门禁 | `make all` / `make release` / `make tests UT=FTL` / `make tests UT=FC` 零告警；format-check 通过 | 构建日志 |
| 板端全量 | `test /ftl/` → **85/85 通过**、61.05 s（镜像 2026-10-24 22:08:11）；cache read 修复后 **85/85、61.20 s**（debug 镜像 2026-10-25 01:09:23） | 串口日志 |
| 纯 RAM | 56 次运行（48 个函数 + 参数化）已上板（21:57:59，10.38 ms） | 串口日志 |
| media | 29 次运行（15 个函数 + `host_io_matrix` 15 组合 + 随机 8 轮；`cp_corrupt_refuse_format` 已上板） | 同上 |
| 条件 SKIP | `l2p_miss_unmapped_evict` 在 cache 有脏/在飞行时按设计跳过 | 代码内条件 |
| CP 落盘证据 | `journal barrier cp 1->2`、`trim cp 2->3`（用例起点→落盘序号，RISK-UT-02） | 2026-10-24 21:14 隔离重跑 |
| 未覆盖 | M-11 真介质注入、真空白片、NAND program/erase 注入、真 POR、USB/H2testw | §7 |
| 速度（FTL） | `test --log-visible info /speed/ftl_rw` → normal/cache 两档各 25 MiB；**板端 2/2 通过**（2026-10-25 00:04 镜像；真实约 98 s，munit 显示 13.51 s 是 CYCCNT 回绕，见 RISK-UT-05）：normal 写 1065 / 读 1356 KB/s；cache 写 1107 / 读 1363 KB/s，`rd_path=cache`、`rdc batches=1600`、`err=0`（RISK-HOST-13 修复后首次真正走 cache）；读速与 normal 档基本相同（RISK-PERF-03） | 2026-10-25 00:04 镜像；release -O2（00:35 镜像）4/4：FC 写 3670 / 读 3533（cache 写 4319 / 读 3591），FTL 写 2499 / 读 3369（cache 写 2687 / 读 3369，`rd_path=cache`）；munit 计时受 CYCCNT 回绕影响，见 RISK-UT-05 |

> 每一阶段结束都跑 `make all` / `make release` / `make tests UT=FTL`；未上板的用例标「未上板」。

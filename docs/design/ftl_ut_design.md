# FTL 单元测试设计（板端）

> 状态：**已落地并上板**（2026-10-24）：`/ftl/pure/` 23/23、`/ftl/media/` 5/5 全绿（media 总耗时 22.5 s，含一次 format）。
> 本版实现 28 条用例；§4 中尚未落地的条目（BBT 持久化、L2P 三态、CP 编码器、GC/WL media 等）按 §9 继续补。
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
5. **证据与状态如实**：未上板的用例标「未上板」；失败必须打印用例名与不变量计数；不复用 H2testw 结果冒充 UT。

---

## 2. 分层与套件树

| 层 | 路径 | 碰 NAND | 内容 |
|---|---|---|---|
| L0 | `/ftl/pure/` | 否 | 模块内部功能与不变量 |
| L1 | `/ftl/media/e2e/` | 是 | 格式化、主机写读、flush、TRIM、恢复 |
| L2 | `/ftl/media/{l2p,md,cp,bbt}/` | 是 | 跨模块契约、持久化、搬移 |
| L3 | 人工流程 | 是 | 真 POR、坏块注入、NAND 时序、USB、H2testw |

套件树：

```
/ftl/
  pure/
    mem/    allocator / BST / BBT / wear
    md/     metadata ring/Directory / checkpoint / journal
    host/   pending / group / wbuf / ftl_op / bg
    io/     L2P 驻留行 / GC / WL / RR 策略
  media/
    e2e/    格式化、写读、wbuf、flush、TRIM、模拟重启
    l2p/    region 预载
    md/     回收/压缩/Anchor
    cp/     池扫描、完整记录、重启恢复
    bbt/    save/load、FORMAT_IN_PROGRESS
```

执行顺序：`pure/` → `media/`。`test /ftl/pure/` 安全；`test /ftl/media/` 与裸 `test` **破坏介质**。

---

## 3. 夹具与隔离

### 3.1 活实例

- `ftl_ut_live()` 返回 `g_ctrl.ftl[0][0][0]`（`extern ssd_controller_t g_ctrl`）。`ftl_ctx_t` 约 287 KB，无法复制。
- pure 用例**不 reset 活实例的 allocator/metadata**；使用测试自有对象。
- 需要活实例 L2P 行的用例，只借 2~4 行，teardown 原样恢复。

### 3.2 测试自有 RAM 对象（小状态）

| 对象 | 尺寸 | 放置 | 用法 |
|---|---|---|---|
| `bst_block_t blocks[FTL_BLOCK_TOTAL]` | 68 KB | SRAM12 | 只用 gblock 0..7，其余 NONE |
| `WORD free_blocks[FTL_BLOCK_TOTAL]` | 7 KB | SRAM12 | 只填 0..7 |
| `super_block_t closed_sb_list[FTL_BLOCK_TOTAL]` | 41 KB | SRAM12 | 只填前 4 条 |
| OP/Spare per-plane 表 | ~2 KB | SRAM12 | 只填前 2 项 |
| `md_dir_cache_t` | 34 KB | AXI `.bss` | 只填 2~3 个 region |
| **合计** | **~152 KB** | | 链接期区域溢出即为硬门禁 |

若链接放不下：退回借用活实例的表 + 小状态，不缩减数组尺寸（结构体必须与生产同型）。

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

---

## 4. 用例清单

### 4.1 `/ftl/pure/mem/`（allocator / BST / BBT / wear）

| ID | 用例 | 覆盖 |
|---|---|---|
| FT-P-MEM-01 | `address_encode_and_gblock_sentinel` | LBA/PBA/PAA 往返、`ftl_gblock` 边界（RISK-ALLOC-06） |
| FT-P-MEM-02 | `cw_masks_and_bitmap_popcount` | `ftl_cw_bit`/span/plane mask（RISK-BIT-01） |
| FT-P-MEM-03 | `free_pool_push_pop_dedup_bad_bounds` | `state==FREE ⟺ 在池`、BAD/越界/池满拒绝 |
| FT-P-MEM-04 | `sub_valid_if_live_and_state_transitions` | BAD 后减计数不回声（RISK-MIG-03） |
| FT-P-MEM-05 | `op_spare_per_plane_counts_and_refill` | 两池 per-plane 隔离、refill 上限 |
| FT-P-MEM-06 | `super_open_pair_close_rewind_degenerate` | 双 plane/退化单 plane、temp |
| FT-P-MEM-07 | `closed_list_add_remove_drop_plane_swap` | swap-remove、部分 plane 释放 |
| FT-P-MEM-08 | `bbt_inline_bitmap_bounds_and_mark_bad_idempotent` | 位图边界、重复标坏只减一次 Spare（RISK-BBT-01/10） |
| FT-P-MEM-09 | `bbt_dirty_gen_candidate_pool_skip_bad` | dirty_gen、候选池跳过 BAD、孤儿完成守卫（RISK-BBT-06） |
| FT-P-MEM-10 | `bst_erase_stats_and_wear_get_set` | erase stats、wear 计数 |

### 4.2 `/ftl/pure/md/`（metadata / checkpoint / journal）

| ID | 用例 | 覆盖 |
|---|---|---|
| FT-P-MD-01 | `ring_geometry_addr_plane_mask` | 记录数/地址/plane 掩码 |
| FT-P-MD-02 | `ring_locate_fast_slow_wrap_fake_probe` | 假 probe 下的 locate 三态 |
| FT-P-MD-03 | `anchor_prepare_page_commit_dirty_gen` | Anchor 落盘门槛（RISK-MD-06 纯 RAM 部分） |
| FT-P-MD-04 | `dir_init_update_find_live_count` | Directory 基本账 |
| FT-P-MD-05 | `commit_region_primary_backup_l2p_live` | 主/备双副本提交、live 计数 |
| FT-P-MD-06 | `log_alloc_rotate_release_restore` | 日志区分配/轮转/回收/恢复 |
| FT-P-MD-07 | `cp_block_snapshot_wear_page_word_pack` | 快照、磨损页、word 打包/校验 |
| FT-P-MD-08 | `cp_timer_trigger_gates` | 定时/水位触发、游标复位 |
| FT-P-MD-09 | `journal_reserve_append_merge_watermark` | 同 LBA 合并、merge floor、watermark/emergency |
| FT-P-MD-10 | `md_status_error_idle_clear_characterization` | `status.error` 清零点（RISK-MD-17） |
| FT-P-MD-11 | `journal_write_fail_pending_clear_by_cp` | 写失败 pending 与 CP 清账（RISK-JN-03 纯 RAM 部分） |

### 4.3 `/ftl/pure/host/`（pending / group / wbuf / ftl_op / bg）

| ID | 用例 | 覆盖 |
|---|---|---|
| FT-P-HOST-01 | `pending_alloc_find_remove_release_full32` | 槽满、重复释放、索引 |
| FT-P-HOST-02 | `group_slot_exhaustion_returns_false_no_overwrite` | 组槽耗尽（RISK-PEND-02） |
| FT-P-HOST-03 | `group_multi_owner_complete_once` | 多 owner 只完成一次、状态粘滞 |
| FT-P-HOST-04 | `confirm_park_timeout_io_error` | 伪时钟超时（RISK-HOST-05 纯 RAM 部分） |
| FT-P-HOST-05 | `wbuf_append_drop_reset_owner_settlement` | owners 记账、idle 判定 |
| FT-P-HOST-06 | `wbuf_pair_layout_sparse_bitmap` | pair 数据布局、稀疏 CW 位图（RISK-WB-03/IF-04） |
| FT-P-HOST-07 | `op_ctx_capacity_64_and_duplicate_id` | 旁表容量/同 id 覆盖（RISK-IF-05） |
| FT-P-HOST-08 | `op_ctx_add_take_remove` | ctx 生命周期 |
| FT-P-HOST-09 | `op_route_set_get_orphan_dispatch_release` | dim→(ch,ce,lun)、孤儿完成释放（RISK-CR-02/CR-03） |
| FT-P-HOST-10 | `bg_task_table_interval_budget_must_run` | 10 项任务表、预算/间隔 |
| FT-P-HOST-11 | `bg_level_select` | 负载档位选择 |

### 4.4 `/ftl/pure/io/`（L2P / GC / WL / RR）

| ID | 用例 | 覆盖 |
|---|---|---|
| FT-P-IO-01 | `gc_should_trigger_watermark_emergency` | 触发位图、plane 空 |
| FT-P-IO-02 | `gc_select_victim_modes_and_bounds` | 三模式、valid 上界（RISK-GC-06/14） |
| FT-P-IO-03 | `gc_decode_cw_lba_tri_state` | OK/空/损坏三态 |
| FT-P-IO-04 | `l2p_resident_three_states_clock_backpressure` | 读/写/LOADING/FLUSHING、CLOCK（RISK-L2P-03） |
| FT-P-IO-05 | `l2p_entries_miss_unmapped_out_of_range` | 批量 miss/未映射/越界（RISK-L2P-06） |
| FT-P-IO-06 | `l2p_thrashing_hysteresis` | 90%/70% 滞回 |
| FT-P-IO-07 | `wl_trigger_ubi_boundary` | `max_free_ec − min_used_ec ≥ 200` 边界 |
| FT-P-IO-08 | `wl_victim_filter_valid_cw_temp` | 有效 CW/温度过滤 |
| FT-P-IO-09 | `rr_ecc_trend_health_retire` | ECC 单调/不降级/退役（RISK-RD-01 纯 RAM 部分） |
| FT-P-IO-10 | `rr_predictive_scan_and_error_classification` | 预测巡检、状态位分类 |

### 4.5 `/ftl/media/`

| ID | 用例 | 类型 | 覆盖 |
|---|---|---|---|
| FT-M-01 | `format_then_clean_boot_invariants` | e2e | 容量/池对账、L2P 日志区、无 dangling（RISK-BOOT-04 部分） |
| FT-M-02 | `host_write_read_roundtrip` | e2e | 8/16/64 扇区写→flush→读回 |
| FT-M-03 | `wbuf_sparse_flush_one_program` | e2e | 稀疏写、一次 flush（RISK-WB-03/IF-04） |
| FT-M-04 | `l2p_cross_region_flush_evict` | e2e | 跨 region 回写/驱逐（RISK-L2P-03） |
| FT-M-05 | `preload_region_zero_and_nonzero` | l2p | 无别名（RISK-BOOT-01） |
| FT-M-06 | `journal_barrier_and_cp_trigger` | e2e | 写→fl→无 pending→CP seq 前进 |
| FT-M-07 | `trim_no_resurrection` | e2e | TRIM 后读未映射、重启不复活（RISK-HOST-08/09） |
| FT-M-08 | `simulated_reboot_recovery` | e2e | 重跑 `ftl_init`+`pfr_boot_recovery`（RISK-BOOT-05） |
| FT-M-09 | `gc_progress_real_nand` | e2e | 压 `free_count`→GC 一轮搬移（RISK-GC-06/MIG-03） |
| FT-M-10 | `wl_progress_real_nand` | e2e | 构造擦除差→WL 一轮（RISK-WL-04） |
| FT-M-11 | `md_reclaim_anchor_and_abort_keeps_victim` | md | Anchor 先于擦除；abort 不擦 victim（RISK-MD-06/16/BOOT-02） |
| FT-M-12 | `bbt_save_load_persistence` | bbt | 标坏→save→模拟重启仍 BAD（RISK-BBT-01/10） |
| FT-M-13 | `cp_find_latest_full_record_reboot_restore` | cp | 池扫描/完整记录/恢复（RISK-CP-03） |
| FT-M-14 | `format_in_progress_and_used_medium_refusal` | bbt | 续做格式化/拒绝误判空片（RISK-BOOT-04/17/BBT-02） |

---

## 5. 风险覆盖矩阵（53 条活跃风险）

处置：`UT-P` 纯 RAM；`UT-M` 真 NAND；`BUILD` 由本重写消除；`BOARD` 人工板端流程；`FI-NA` 当前无故障注入 seam；`OUT` 性能/格式演进债，非 UT 目标。

| ID | 状态 | 处置 | 用例 / 流程 | 理由 |
|---|---|---|---|---|
| RISK-MD-03 | 待测试源 | BUILD | `make tests UT=FTL` | 旧测试源删除，构建本身成为回归 |
| RISK-MD-06 | 已修未上板 | UT-P + UT-M + BOARD | FT-P-MD-03、FT-M-11；真 POR 人工 | dirty_gen/Anchor 门槛可测；真掉电不可注入 |
| RISK-PERF-01 | 待后续 | OUT | — | 重复全表扫描是性能债 |
| RISK-PERF-02 | 待后续 | OUT | — | 单 plane 写是吞吐债 |
| RISK-GC-05 | 待后续 | OUT | — | GC bitmap 未实现，属性能/重启成本 |
| RISK-GC-06 | 待上板 | UT-P + UT-M + BOARD | FT-P-IO-01/02/03、FT-M-09 | 选择/门控/三态可测；并发 host 写与 POR 人工 |
| RISK-JN-03 | 待上板 | UT-P + FI-NA | FT-P-MD-11 | 失败时序需注入 |
| RISK-MIG-03 | 已修/部分上板 | UT-P + UT-M + BOARD | FT-P-MEM-04、FT-M-07 | 减计数守卫纯 RAM；TRIM 路径真 NAND；release 回绕不覆盖 |
| RISK-MIG-07 | 已复现 | BOARD | — | 需 H2testw/soak 与 `fc=` 归因 |
| RISK-MIG-08 | 待定位 | UT-M + BOARD | FT-M-11/12 部分 | 快照一致性可测；全盘 dangling 需 H2testw |
| RISK-GC-14 | 待上板 | UT-P + BOARD | FT-P-IO-02 | 收益模式可测；NO_SPACE 假报需 host 负载 |
| RISK-BBT-01 | 待上板 | UT-P + UT-M | FT-P-MEM-08/09、FT-M-12 | 幂等/dirty_gen 纯；持久化真 NAND |
| RISK-BBT-02 | 待上板 | UT-M 部分 + FI-NA | FT-M-14 | 读失败需注入 |
| RISK-BBT-04 | 待后续 | OUT | — | 格式版本演进债 |
| RISK-BBT-06 | 待后续 | UT-P 部分 + FI-NA | FT-P-MEM-09 | 重复完成需注入 |
| RISK-BBT-10 | 待测试源 | UT-P + UT-M | FT-P-MEM-07/08/09、FT-M-12/14 | 新 BBT 用例覆盖 load/save/refill/format |
| RISK-BOOT-01 | 待上板 | UT-M | FT-M-05 | region 0/非 0 预载 |
| RISK-BOOT-02 | 待上板 | UT-M | FT-M-11 | 重建后数据可读、carve 不选中已写块 |
| RISK-BOOT-03 | 待上板 | UT-M 部分 + FI-NA | FT-M-14 | CURRENT/NO_RECORD/VERSION 可造；IO_ERROR 需注入 |
| RISK-BOOT-04 | 待上板 | UT-M + BOARD | FT-M-01/14；三点掉电人工 | FORMAT_IN_PROGRESS 续做可模拟 |
| RISK-BOOT-05 | 待上板 | UT-P + UT-M + BOARD | FT-P-MD-07、FT-M-08 | 校验/回放可测；真掉电人工 |
| RISK-BOOT-13 | 待定位 | UT-M smoke + BOARD | FT-M-01 | format 后写 smoke；HardFault 归因人工 |
| RISK-MD-16 | 待定位 | UT-M | FT-M-11 | 构造不可读 victim 页，断言不擦 |
| RISK-MD-17 | 待决策 | UT-P | FT-P-MD-10 | 固定当前 `status.error` 语义 |
| RISK-BOOT-17 | 信息/待决策 | UT-M 部分 + BOARD | FT-M-14 | 真空白片需另测 |
| RISK-L2P-03 | 待上板 | UT-P + UT-M + BOARD | FT-P-IO-04、FT-M-04 | 背压/CLOCK 可测；命令池压力人工 |
| RISK-L2P-05 | 待上板 | FI-NA + BOARD | — | 需精确命令池饥饿 |
| RISK-L2P-06 | 部分已改 | UT-P | FT-P-IO-05 | miss/未映射/越界 |
| RISK-CP-03 | 待上板 | UT-P + UT-M + BOARD | FT-P-MD-08、FT-M-13 | 触发/扫描/完整记录；逐阶段掉电人工 |
| RISK-CP-04 | 待定位 | FI-NA + BOARD | — | 需持续写压与池饥饿 |
| RISK-WL-04 | 待上板 | UT-P + UT-M + BOARD | FT-P-IO-09、FT-M-10 | 过滤/一轮搬移可测；POR/soak 人工 |
| RISK-WL-05 | 待后续 | OUT | — | 重复全表扫描 |
| RISK-IF-04 | 待上板 | UT-P + UT-M | FT-P-HOST-06/09、FT-M-03 | 布局/位图/迟到完成 |
| RISK-IF-05 | 待上板 | UT-P | FT-P-HOST-07 | 64 项旁表容量 |
| RISK-HOST-05 | 待定位 | UT-P + BOARD | FT-P-HOST-04 | 停泊超时纯 RAM；根因人工 |
| RISK-HOST-08 | 部分上板 | UT-M + BOARD | FT-M-07 | TRIM+重启真 NAND；极端时序人工 |
| RISK-HOST-09 | 待后续 | UT-M 特征化 + BOARD | FT-M-07 | 固定当前语义；并发当前不可达 |
| RISK-CR-02 | 待上板 | UT-P | FT-P-HOST-09、FT-P-MEM-01 | dim 分解/路由 |
| RISK-CR-03 | 待上板 | UT-P + UT-M | FT-P-HOST-09、media 回归 | 分 handler/孤儿完成 |
| RISK-WB-03 | 待上板 | UT-P + UT-M | FT-P-HOST-05/06、FT-M-03 | 记账/位图/一次编程 |
| RISK-LOG-02 | 待上板 | BOARD | — | 需 UART 速率观测 |
| RISK-BG-09 | 待测试源 | BUILD | `make tests UT=FTL` | 旧 bg 测试删除 |
| RISK-ARCH-02 | 待后续 | UT-P + OUT | FT-P-MEM-01 | per-LUN 路由可测；跨 LUN 未实现 |
| RISK-ARCH-06 | 待后续 | UT-M + BOARD | FT-M-02/03/04/06/07/12 | 正常路径媒体回归；真 POR 人工 |
| RISK-USB-02 | 待上板 | BOARD | — | 拔线/复位/重枚举 |
| RISK-USB-05 | 待上板 | BOARD | — | 需 USB 抓包看 FUA |
| RISK-NAND-04 | 部分定位 | BOARD | — | 吞吐测量 |
| RISK-NAND-05 | 待定位 | BOARD | — | 物理介质失效 |
| RISK-NAND-06 | 待定位 | UT-M 部分 + BOARD | FT-M-02/09 | 正常写读可测；全片 re-scan 人工 |
| RISK-ALLOC-06 | 待后续 | UT-P | FT-P-MEM-01 | 固定 `ftl_gblock` 截断行为 |
| RISK-BIT-01 | 待后续 | UT-P | FT-P-MEM-02 | 掩码构造器交叉校验 |
| RISK-RD-01 | 观察中 | UT-P + BOARD | FT-P-IO-09/10 | 状态分类纯 RAM；映射/介质人工 |
| RISK-PEND-02 | 已改未上板 | UT-P | FT-P-HOST-02 | 组槽耗尽 |

---

## 6. 运行流程

1. 编译：`make tests UT=FTL`（生成 `build/firmware/debug/bin/debug.bin`）。
   ⚠ **测试镜像必须是最后一次构建**：`make all`/`make release` 会覆盖同一 `debug.bin`（普通固件无 `test` 命令）。静态库已按配置分目录（`lib_ut_FTL`）；若 `test` 缺失，先查 `LIB_DIR` 与构建顺序（`RISK-PLAT-05`）。
2. 烧写：JLink（见 `.claude/skills/board-run/SKILL.md`）；核对横幅时间戳。
3. pure：`test --list` → `test /ftl/pure/`（可只跑子套件）。
4. media：拔 USB → `test /ftl/media/`（破坏性，内部先 `format`）。
5. 抓完整串口日志；记录套件/用例数、耗时、失败项。
6. 收尾：`format` + POR，恢复可用的空盘。
7. 证据归档到 `docs/archive/`（按需）。

预期：pure < 10 s；media（含一次 format）< 5 min。日志用 `--log-visible info`，单用例调试才用 `debug`。

---

## 7. 不覆盖项

- 真 POR / 掉电：人工断电流程，不进 munit（允许 `ftl_init`+`pfr_boot_recovery` 模拟，但只标“模拟重启”）。
- NAND 页/擦/写失败注入：当前无 `ftl_sync_*` 注入 seam。
- USB 主机行为、吞吐、H2testw、长时 soak。
- release 档特有行为（UT 为 debug 构建）。
- 跨 LUN 条带/GC/WL（未实现）。

---

## 8. 维护规则

- 新增 FTL 模块或 `ftl_design.md` §3 小节 → 同步 §4 用例表。
- 新增/重开 `known_risks.md` 条目 → 在 §5 增加一行处置。
- 新增 pure 用例：禁止调用 `ftl_init` / `pfr_*` / 任何 `ftl_sync_*` / NAND 命令。
- 新增 media 用例：必须走 `ftl_ut_media_require()`、使用 LBA 尾窗、不得依赖 host 文件系统。
- 每次改 API 后先跑 `make tests UT=FTL`；测试源与生产同仓维护，不再冻结。

---

## 9. 实施阶段

| 阶段 | 内容 | 门禁 |
|---|---|---|
| P0 | 本文 + 用例/风险矩阵 | 评审 |
| P1 | `ftl_ut_common`、空套件、makefile/ut_main 接线 | 三关编译零告警；`test --list` |
| P2 | pure/mem、md | `test /ftl/pure/mem/`、`md/` 全绿 |
| P3 | pure/host、io | `test /ftl/pure/` 全绿 |
| P4 | media e2e 基础（format/写读/flush/重启） | `test /ftl/media/e2e/` 全绿 |
| P5 | media l2p/md/cp/bbt + 收尾 | `test /ftl/media/` 全绿；风险表更新 |

> 每一阶段结束都跑 `make all` / `make release` / `make tests UT=FTL`；未上板的用例标「未上板」。

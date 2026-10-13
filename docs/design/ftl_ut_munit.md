# FTL 层测试用例设计（L0 munit 纯 RAM 套件）

> **状态：设计文档（供审查）。实施（测试文件 / makefile 接线）在审查通过后进行。**
>
> **分层边界**（判据 `fc_nand_design.md §26.1`）：本文**只管 FTL 层（`src/ftl/`）**。
> 非 FTL 的部分已拆出（2026-10-13）：
>
> | 内容 | 归属 | 文档 |
> |---|---|---|
> | HIL 层（`src/controller/`）纯逻辑 UT | L0 | **`hil_ut_design.md` Part A** |
> | 对外接口 / 拆分聚合 / 跨重启 / 后台观测（真 NAND + CLI） | L2 系统集成 | **`hil_ut_design.md` Part B~D** |
> | FC-NAND 三级 UT | — | `fc_nand_ut.md` |
>
> **为什么这样分**：`src/controller/` 是 **HIL**，与 FTL **平级**（不是 FTL 的一部分）。
> 原本文档把 CLI 端到端验收混作「FTL 层测试」，标题名不副实，故拆出。本文只剩纯 FTL 逻辑。

## 1. 背景与目标

FC-NAND 已有三级 munit UT（`tests/unit/test_fc_level1/2/3.c`，经 `make tests` 上板 + UART `test`
命令跑），但 **FTL 层没有自己的 UT**：原有 `tests/unit/test_gc.c`/`test_l2p.c`/`test_nand.c`
只是各 1 行头部注释的孤儿占位、`test_rebuild.c` 因夹具超容被摘除——**这四个文件已于 2026-10-13 删除**
（有效用例并入本文档 §4）；`src/ftl/makefile.mk` 仍无测试块。

FTL 层落地了大量**纯逻辑**（Free Pool 三级水位、journal 水位公式、GC/WL victim 选择、
read disturb 慢死预测、坏块替换池、L2P thrashing、**组完成语义**），目前**全部依赖上板验证**
——缺一层纯 RAM 单元测试把纯逻辑钉住，编译期 + 上板前能回归。

**目标**：
- 复用 FC-NAND 的 munit 架构（`MunitTest` 数组 + `ut_main.c` 根套件 + `make tests` + UART `test`），
  零改动 `tests/framework`
- **只测纯 RAM 函数**（不碰 NAND、不需要 mock），上板稳、不依赖真 NAND 时序
- 需真 NAND 的路径（GC 搬移 / journal replay / boot recovery / 跨重启）→ **`hil_ut_design.md`**

## 2. 关键架构约束（决定 fixture 形态）

| # | 发现（文件行号） | 结论 |
|---|---|---|
| 1 | munit 无测试宏，`MunitTest` 数组（`tests/framework/munit.h`）+ `MunitSuite` 根套件（`ut_main.c`）逐项注册 | 新增 FTL 套件 = `extern MunitTest g_ftl_*_tests[]` + 根套件一行 |
| 2 | **板上堆仅 512B**：`linker/stm32h743xx_flash.ld:19` `_Min_Heap_Size=0x200` | 新 FTL 测试一律**静态 fixture + setup 里 memset 复位**。**禁止 malloc** |
| 3 | `ftl_block_t`=**20B**（`bst.h:139` static_assert）；`super_block_t`=**12B**（`allocator.h:80`） | 见下预算表 |
| 4 | **`allocator_init` 现取 `ftl_tables_t*`**（3 参，`allocator.c:49`）——大数组**按值内含**，HIL 不再传指针（P2-5 表自持） | **fixture 必须提供 `ftl_tables_t`**，不能再散落数组 |
| 5 | `allocator_init` **不填 free pool**（只把非坏块置 FREE 状态、`free_count=0`），由 HIL/`main.c` 回填（`allocator.c:59` 注释 + `main.c:230`） | 用例须区分「init 后」与「回填后」两种状态 |
| 6 | `pending_*` / `pending_group_*` 取 **`ftl_context_t*`**（`pending.h:134`），需**完整上下文** | 见下「fixture 内存」一节 |

### 2.1 尺寸与内存预算（P2-5 后重算）

| 结构 | 组成 | 尺寸 |
|---|---|---|
| `ftl_tables_t` | `blocks[4096]×20B`=80KB + `free_blocks[4096]×2B`=8KB + `op_blocks[4096]×2B`=8KB + `closed_sb_list[1024]×12B`=12KB + `spare_blocks[50]×2B`=100B | **≈108KB** |
| `metadata_dir_cache_t` | `entries[2048]×7B` | **≈14.4KB** |
| `ftl_context_t` | 内含 `tables`(108KB) + `cache`(128KB) + `metadata_dir`(14.4KB) + journal + … | **≈287KB** |

> **⚠ 关键约束**：`ftl_tables_t` 的数组尺寸由 `FTL_BLOCK_TOTAL`（=4096）**固定**——原文档
> 「用 2112 项省内存」**不可能**（结构体按值含全 4096 项）。
> 且 **AXI_RAM(384KB) 已被 `main.c` 的 `g_ftl`(287KB) 占满**（`main.c:131` 的 `_Static_assert`），
> 独立 287KB 的 `ftl_context_t` fixture **放不下**。

### 2.2 fixture 内存：复用活实例（推荐）

`main.c` 的 `g_ftl[0][0][0]` 由 `g_ctrl.ftl[0][0][0]` 暴露（`g_ctrl` 是 `extern`，`cli.h:15`；
CLI 已这么用）。**FTL L0 fixture 复用该实例**，`setup` 里 `memset` 复位其子结构：

```c
/* setup: 复位活实例的 FTL 子结构到已知态（不改拓扑指针） */
ftl_context_t *F = g_ctrl.ftl[0][0][0];
memset(&F->tables,       0, sizeof(F->tables));        /* BST + 各池 + closed 列表 */
memset(&F->metadata_dir, 0, sizeof(F->metadata_dir));
memset(F->pending,       0, sizeof(F->pending));
memset(F->groups,        0, sizeof(F->groups));
allocator_init(&F->alloc, F->topo, &F->tables);        /* 重绑表 + 状态机归零 */
metadata_dir_init(&F->metadata_dir);
```

- **好处**：零额外内存、单一数据源（不复制 287KB）。
- **代价**：fixture 不再「独立」；须保证 `test` 执行期间**后台泵不并发**——`test` 命令在
  `cli_service_poll` 里**同步执行**（不重入泵），天然满足。
- **备选**（不推荐）：独立 `ftl_context_t` 落 SRAM（`.sram1/2/3_data`=288KB，**仅 1KB 余量**），
  或只对不需要上下文的用例用独立小 fixture（`ftl_tables_t`=108KB 落 SRAM1）。

## 3. 单文件测试组织（仿 fc_nand 一文件多套件）

**1 个文件** `tests/unit/test_ftl.c`（仿 `test_fc_level1.c` 一文件多套件风格），fixture 见 §2.2
（复用 `g_ctrl.ftl[0][0][0]`，不另声明大数组）：

```c
/* setup 统一 ftl_fixture_reset()：复位活实例子结构（见 §2.2），返回 NULL fixture */
static void ftl_fixture_reset(void);

/* 小工具：把 n 个好块回填进 free pool（复刻 main.c 的回填循环，供 allocator 用例用） */
static void ftl_fixture_backfill(ftl_allocator_t *alloc, uint16_t first, uint16_t count);
```

文件内按 munit 风格分节，**导出 8 个套件数组**（非 static，供 `ut_main` extern）：
`g_ftl_allocator_tests[]` / `g_ftl_watermark_tests[]` / `g_ftl_gc_wl_tests[]` / `g_ftl_rr_tests[]` /
`g_ftl_bbt_tests[]` / `g_ftl_metadata_tests[]` / `g_ftl_pending_ct_tests[]` / **`g_ftl_group_tests[]`**。
setup 统一 `ftl_fixture_reset()`，teardown NULL，`SINGLE_ITERATION`。

## 4. 用例清单（逐条）

> 表式：**用例名 | 前置 | 动作 | 断言**。断言优先用**不变量/边界**（判据 7 参数化 vs 迭代、
> 判据 8 负例钉边界）。**全部为纯 RAM，板上 `test` 跑，无 mock。**

### 4.1 `g_ftl_allocator_tests[]`（`src/ftl/allocator.c`）

| 用例 | 断言 |
|---|---|
| `init_zeroes_pool` | `allocator_init` 后 `free_count==0`、`op_count==0`、`spare_count==0`、非坏块 state==FREE（**init 不回填**，§2 行 5） |
| `backfill_fills_pool` | 回填 n 个好块后 `free_count==n`；池内每块 state==FREE |
| `free_block_clears_state` | 对 OPEN/CLOSED/RESERVED/TEMP 块调 `allocator_free_block` → state==FREE + 入池 + 计数+1 |
| `close_open_sb` | `allocator_close_open_sb`：新 closed SB 两 plane 块 state==CLOSED + `sb_id` 正确 + `closed_sb_count`+1 |
| `add_closed_sb` | `allocator_add_closed_sb` → count+1、两块 CLOSED + sb_id；**条目字段**：`next_page==PAGES_PER_BLOCK`、`temperature` 透传、`valid==0`、`topo` 正确；两块 **清 FREE/OPEN** 位 |
| `add_closed_sb_full` | **边界**：塞满 `SUPER_BLOCK_LIST_MAX` 后返回 `UINT16_MAX` 且**不写入**、`closed_sb_count` 不变（满列表拒绝） |
| `remove_closed_sb_swap_remove` | 删中间项：末项搬到该位**且 `sb_id` 修正**、count-1（swap-remove 索引坑）；被删块 `sb_id` 复位 `FTL_BLOCK_NO_SB` |
| `remove_from_free` | `allocator_remove_from_free`（swap-remove）→ count-1、块不在池；**移除不存在的块无害**（count 不变，负例） |
| `reserved_alloc_free` | `allocator_alloc_reserved_block` 置 RESERVED；`allocator_free_reserved_block` 归还池 |
| **`open_sb_never_picks_metadata`** | **回归用例**：metadata 块 0..62 被 `allocator_init` 标 FREE 但**不在 Free Pool** ⇒ 新建 SB 两 plane 块都 **≥63**（钉住历史真缺陷） |
| `open_sb_min_erase_count` | 池内多块时选该 plane `erase_count` 最小者（动态 WL） |

> ⚠ **L0 只能调公开函数**：`allocator_open_sb`（`allocator.c:200`）、
> `allocator_select_plane_block_from_pool`（`:157`）、`allocator_pull_from_op`（`:128`）都是 `static`
> ⇒ 上两条经**公开入口 `allocator_close_open_sb`（`:359`，内部调 `open_sb`）间接测**：
> 调用后断言新 closed SB 的 `plane_blocks[p] ≥ 63` / `erase_count` 最小。

### 4.2 `g_ftl_watermark_tests[]`（`src/ftl/checkpoint.c`）

| 用例 | 断言 |
|---|---|
| `journal_wm_below_85` | 使用率 <85% → 不触发（边界下 1 条 entry） |
| `journal_wm_at_85` | 恰 85% → 置 `checkpoint_needed`（边界上 1 条） |
| `journal_wm_at_95_emergency` | ≥95% → 置 `emergency`（`JOURNAL_CRITICAL`） |
| `emergency_set_reset` | ≥95% 置位；回落清位 |
| `checkpoint_trigger_T1_T2_T3` | `checkpoint_check_trigger` 三种条件各自命中（参数化）——实现仍为 T1 journal 水位 / T2 写入量 / T3 定时（`checkpoint.c`） |
| `checkpoint_is_emergency` | `checkpoint_is_emergency` 查询位与 watermark 一致 |

### 4.3 `g_ftl_gc_wl_tests[]`（`src/ftl/gc.c` + `src/ftl/wear_leveling.c`）

| 用例 | 断言 |
|---|---|
| `gc_victim_lowest_vpc` | `gc_select_victim` 选 VPC 最低 closed SB |
| `gc_victim_skips_open` | 跳过 `valid!=0` 的 open SB（负例，判据 8） |
| `gc_trigger_T1_T4_T5` | `gc_check_triggers` **三位**各自置位（参数化）：T1 `GC_TRIGGER_WATERMARK(0x01)` / T4 `GC_TRIGGER_EMERGENCY(0x08)` / T5 `GC_TRIGGER_WEAR(0x10)`（`gc.h:49-51`）——**v0.63 已删 T2 写数 / T3 定时** |
| `wl_trigger_delta` | 条件 1：`max-min ≥ WL_DELTA_THRESHOLD(5000)` |
| `wl_trigger_ratio` | 条件 2：`max ≥ avg×WL_MAX_AVG_RATIO(3)` |
| `wl_trigger_low_zone` | 条件 3：低擦除区块占比 `≥ WL_LOW_BLOCK_PCT(5)%` |
| `wl_victim_high_valid_low_erase` | `wl_select_victim` 选有效页最多 + 擦除最少（`valid≥89`） |
| `wl_victim_read_count_weight` | 平手时 `read_count` 高者优先（B5 权重） |
| `wl_max_min_avg_erase` | 三统计量与手工构造一致 |
| `wl_count_blocks_below` | 低于阈值块计数正确 |

### 4.4 `g_ftl_rr_tests[]`（`src/ftl/read_recovery.c`）

| 用例 | 断言 |
|---|---|
| `record_ecc_ring_depth8` | 8 深度环形；第 9 次覆盖第 1 |
| `record_ecc_rising` | `rising_count≥4`→WARNING；单次 `≥6`→CRITICAL；`≥15`→DYING |
| `check_predictive_critical` | `rr_check_predictive` 只命中 `health≥CRITICAL` |
| `get_health` | `rr_get_health` == 记录推导值（含 `NULL`→HEALTHY） |
| `predictive_scan_read_disturb` | `rr_predictive_scan`：`RR_READ_DISTURB_THRESHOLD(1000)` 上下边界 |
| `retire_block` | `rr_retire_block` 置 RESERVED + 清健康/read_count，不再被选中 |

### 4.5 `g_ftl_bbt_tests[]`（`src/ftl/bbt.c`）

| 用例 | 断言 |
|---|---|
| `mark_bad_sets_bits` | `bbt_mark_bad`：BAD 位 + `clear_valid` + `bad_count`+1 |
| `mark_bad_idempotent` | 重复标坏**不**重复计数（负例） |
| `spare_pool_refill` | `bbt_spare_pool_refill`：spare→free 补充 + 计数守恒 |
| `is_bad_set_bad_inline` | `bbt_is_bad`/`bbt_set_bad` 纯 bitmap（含 `NULL`→0） |

### 4.6 `g_ftl_metadata_tests[]`（`src/ftl/metadata.c`）

| 用例 | 断言 |
|---|---|
| `dir_init_entries` | `metadata_dir_init` 后 `count==2048`（= `L2P_REGIONS_PER_INSTANCE`）、全 UNKNOWN |
| `dir_update_lookup_roundtrip` | `metadata_dir_update` 写一个 region → `metadata_dir_lookup` 回同 entry |
| `dir_lookup_missing_unknown` | 未写 region → `0xFFFFFFFF`（负例） |
| `is_metadata_block` | `ftl_is_metadata_block`：**0..62 true、63 false**（边界，判据 8）。区间来自 `g_ftl_partitions[]`：L2P[0,16)+Anchor[16,48)+Ckpt[48,56)+BBT[56,58)+Journal[58,62)+Wear[62,63) ⇒ `FTL_METADATA_BLOCK_END==63`。⚠ **勿写死旧单块布局**（Anchor 33/34、BBT 36/39… 是 P0-2 前的旧值，已随池化失效） |
| `is_metadata_block_dynamic` | 动态 region：`metadata_dir_update(L2P_REGION, r, paa)` 登记 `block≥63` 后 `ftl_is_metadata_block` 命中；**`md_dir==NULL`** 时仍命中固定区（0..62）、动态块不命中 |
| `ring_addr_geometry` | `metadata_ring_addr` 纯地址换算：idx∈{0, rpb-1, rpb, 末} → (block,page,plane) 期望值 |
| `ring_addr_wrap` | 跨 block 边界换算 |

### 4.7 `g_ftl_pending_ct_tests[]`（`src/ftl/pending.c`）

| 用例 | 断言 |
|---|---|
| `pending_add_lookup_remove` | 增删查一致（按 `cmd_id`）——`pending_add` 现 **11 参**（`ftl, cmd_id, owners, lba, buffer, n_cw, new_pba, new_pba1, cache_intermediate, op, cw_bitmap`，`pending.h:134`）；读路径传 `owners=1<<gi, cw_bitmap=0` |
| `pending_full_32_rejects` | 满 `FTL_PENDING_MAX(32)` 槽拒绝（负例） |
| `pending_count` | 计数与槽位一致 |
| `ct_add_remove_lookup_count` | `completion_table_*` 同族 |

### 4.8 `g_ftl_group_tests[]`（组完成语义——**纯 RAM，板上可直测**）

| 用例 | 断言 |
|---|---|
| `group_alloc_free` | `pending_group_alloc/free`；`pending_group_count` 一致 |
| `group_maybe_complete_gating` | `next<total` 或 `outstanding!=0` 时**不**完成（负例） |
| `group_agg_status_sticky` | 任一子操作 IO_ERROR ⇒ 组聚合状态**粘性**为 IO_ERROR |
| `group_complete_once` | `next==total && outstanding==0` 时 `complete_flag`/`status_out`/`on_complete` **各触发一次** |
| `group_reentrant_callback` | 回调里再 `pending_group_alloc` 不踩已释放槽 |
| **`group_complete_subop_multi`** | **新增（v0.66）**：`pending_group_complete_subop_multi(ftl, owners, status)` 按位掩码逐组置位——同一子操作同时完成多个组（页打包写）；断言掩码里**每个**组各推进一次、掩码外的组**不动** |

> `FTL_GROUP_NONE`（`pending.h:35`）现为**死常量**（源码自注「当前无使用者」）⇒ **不测**；
> 要么删常量，要么保留待用，不在用例里断言它。

## 5. 测试技法：随机化 / 参数化 / 压力 / 异步

> 沿用 FC-NAND 三级 UT 已固化的技法（`fc_nand_ut.md §2.3 / §5.4 / §5.5`），把 FTL L0 从
> 「固定输入」升级为「随机化 + 参数化 + 可加压 + 可模拟异步」。**全部是测试写法，不改 FTL 生产码**
> （结论见 §5.6）。

### 5.1 随机化（沿用 FC-NAND Level 3 的 salted PRNG）

FTL 层**确定性**（无 PRNG；`config.random` 是 FC 配置位，非 FTL 逻辑）⇒ 随机化只施加在
**测试输入**上，结果可复现：

- 抽样 = `munit_rand_uint32() ^ hash(用例名) ^ hash(用途标签)`——munit 每 exec 前 reseed，
  不加盐会让所有用例的**第一次**抽样取值相同（`munit.c:774/503`；`fc_nand_ut.md §2.3`）。
- 复现契约 = `(seed, 用例名, 第几次抽样)`；`test --seed 0x…`（**空格**）复现，抽到的值经
  `test_trace_summary` 打出。
- 用例注册用 **`MUNIT_TEST_OPTION_NONE`**（不是 `SINGLE_ITERATION`），否则 `--iterations` 被压成 1。

| 套件 | 用例 | 抽什么 |
|---|---|---|
| allocator | `open_sb_min_erase_count` | 池内 n 块 × 各 plane `erase_count` 随机 → 断言选该 plane 最小 |
| gc_wl | `gc_victim_lowest_vpc` | closed 列表 n 条 `valid_count` 随机 → 断言 argmin |
| gc_wl | `wl_victim_high_valid_low_erase` | n 块 (valid, erase_count) 随机 → 断言 valid 最大 / erase 最小 |
| gc_wl | `gc_trigger_T1_T4_T5` | `free_count` 在 T1/T4 阈值附近随机 → 断言命中位 |
| watermark | `journal_wm_at_85` / `_95` | journal 使用率在 85%/95% ±1 条处随机 |
| bbt | `mark_bad_sets_bits` | 随机坏块集合 → 断言 `bad_count` 与逐块 BAD 位一致 |
| metadata | `dir_update_lookup_roundtrip` | 随机 (region → paa) 若干对 → 断言回读一致 |
| pending_ct | `pending_add_lookup_remove` | 随机 `cmd_id` 集合 + 随机增删序 |
| group | `group_complete_subop_multi` | 随机 `owners` 位掩码 + 随机完成序 |

### 5.2 参数化（`MunitParameterEnum`）

窄候选集用 munit 参数表；宽区间**在用例体内抽**（参数表是 `const char*` 数组，表达不了宽窗口
——FC-NAND 的教训，`fc_nand_ut.md §2.3`）：

| 参数 | 候选 | 用于 |
|---|---|---|
| `plane` | `0` / `1` | LBA↔(plane,cw) 编解码、`super_block_has_plane` |
| `cw` | `0`..`3` | 位图索引 `plane*CW_NUM+cw` |
| `temp` | `HOT` / `COLD` | `allocator_alloc_pair` 的 SB 选择 |
| `op` | `READ` / `WRITE` | `ftl_confirm_write` 位图驱动 |

### 5.3 压力

- **L0**：`test --iterations N /ftl/`——随机化用例的**用例体各跑 N 次**（各抽 N 组输入）。
  ⚠ 报告条数不变（`--iterations` 只乘用例体，`fc_nand_ut.md §2.3`），看**总耗时**判断加量。
  压力断言 = **不变量每次迭代后仍成立**（§9.3 套路）：
  - `free_count + open(2) + closed + bad == total_blocks`
  - 反复 alloc/free N 次后各池计数守恒、无重复块
  - 随机 `owners` 掩码下 `pending_group_count` 增减恒等
- **L2**：长循环 `wr`/`rd`/`tr`（真 NAND，见 `hil_ut_design.md` B.4）。

### 5.4 异步

FTL 异步模型（`ftl.h`）：`ftl_submit_read/write` **非阻塞**（建组 → 逐子操作下发 → 返回 `FTL_OK`
只表示受理）⇒ `ftl_service_pump()` + `ftl_service(ftl)` 推进 ⇒ 全部子操作完成后
`complete_flag`/`status_out`/`on_complete` **恰一次**触发。测试分两层：

- **L0 模拟（无 NAND、无 mock）**：**组簿记是纯的**——`pending_group_alloc` /
  `pending_group_complete_subop(_multi)` / `pending_group_maybe_complete` 只动表（`pending.h`）。
  故可**直接驱动「提交 → 乱序完成 → 组聚合」**：
  ① `pending_group_alloc` 建组 + `pending_add(owners, cw_bitmap, …)` 挂 N 个子操作；
  ② 用**随机顺序**调 `pending_group_complete_subop(_multi)` 模拟完成；
  ③ 断言 `next==total && outstanding==0` 时 `complete_flag`/`status_out`/`on_complete` **各恰一次**，
  任一子操作 `IO_ERROR` ⇒ 组状态**粘性**。覆盖：乱序完成、多组共享一子操作（`owners` 掩码）、
  回调重入、组槽耗尽。
- **L2 集成（真 NAND）**：`ftl_submit_*` 连续下发不等完成 + 泵循环 + 收 `complete_flag`——即
  `hil_ut_design.md` B.2 的异步压力（v0.66 的 `wp <lba> <n> <k>` 已能一次提交 k 组）。

> **边界**：`ftl_submit_*` 内部要 `fc_command_alloc`（FC 层）⇒ **「提交」本身不是 L0 纯函数**；
> L0 只测**簿记**，「提交 → 泵 → 完成」的整链在 L2。

### 5.5 时间注入（节流类用例）

FTL 的时间源经**平台抽象**：`ftl_now_ms` / `checkpoint_now_ms` 读 `ssd_platform_get()->time_us()`
（`ftl.c:83` / `checkpoint.c:50`）。⇒ **时间可注入**——测试克隆 `ssd_platform_ops_t`、把
`time_us` 换成受控实现、`ssd_platform_register(&fake)`（Stage 1 v55 已建该抽象），用例后还原。
故 `ftl_service` 内部的**节流时机**（T3 定时 / RR 扫描节流）也能 L0 确定性驱动——**无需给 FTL 加 seam**。

### 5.6 回到「FTL 设计需不需要改」——结论：**不需要**

FTL 生产码已满足全部测试条件，**零改动**：

| 条件 | 证据 |
|---|---|
| **确定性** | FTL 无 PRNG（`config.random` 是 FC 位）；时间源单一 |
| **输入可构造** | BST（`alloc->blocks[]`）、各池、`pending[]`/`groups[]` 都是普通数组，fixture 直写 |
| **完成可观测** | `ftl_io_request_t.complete_flag`/`status_out`/`on_complete`（`ftl_types.h:44-47`） |
| **时间可注入** | `ssd_platform_ops_t.time_us` 经 `ssd_platform_register`（`platform.h:14/19`） |
| **无 test hook** | 生产码零注入点（测的就是真实路径） |

**唯一张力**：fixture 287KB（P2-5 后果）⇒ 复用活实例 `g_ctrl.ftl[0][0][0]` + reset（§2.2）。
这是**内存约束**，非设计缺陷——不构成对 FTL 生产码的修改要求。

## 6. 接线（实施阶段）

**`src/ftl/makefile.mk`**（现在只有 `MODULE_SRCS` 两行）追加（**一行测试源**，fixture 复用活实例）：

```makefile
ifeq ($(FC_NAND_TEST_ENABLE),1)
# FTL 纯 RAM 单元测试（munit 框架由 fc/makefile.mk 的块编入）
MODULE_INCS += $(PROJECT_ROOT)/tests/framework
MODULE_INCS += $(PROJECT_ROOT)/tests/unit
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_ftl.c
endif
```

**`tests/unit/ut_main.c`**：根套件名从 `/fc-nand/` 起，加一个 **`/ftl/` 根**（`ut_run_all` 里把
`g_fc_nand_suite` 与新的 `g_ftl_suite` 并列），8 个子套件前缀统一 `xxx/`（**必须以 `/` 结尾**，
`ut_main.c:24-27` 注释：munit 只做首尾相接、不补分隔符）。

## 7. 验证

1. `make clean && make tests`（`FC_NAND_TEST_ENABLE=1`）**零错误零新增警告**——fixture 复用活实例，
   不新增 NOLOAD 段；确认 AXI_RAM 用量不超（§2.1）
2. `make debug / release` 不受影响（`FC_NAND_TEST_ENABLE` 默认关，测试文件不编入）
3. 上板：
   - `test --list` → 确认 8 个 `/ftl/*/` 套件全部列出
   - `test /ftl/`（或逐个）→ 全 PASS（尤其 `open_sb_never_picks_metadata`）
   - 随后执行 **`hil_ut_design.md` Part B~D**（接口行为 / 跨重启 / 后台观测）

## 8. 不做（第一版边界）

- **不做 mock**：不建 `fc_submit` 桩 / Mock NAND / 掉电注入器
- 不改 `tests/framework`（munit 适配已完成）；不建 host（x86）target
- ~~不动孤儿 `test_gc.c`/`test_l2p.c`/`test_nand.c` 占位~~ ✅ **2026-10-13 已删**（连同 `test_rebuild.c`），
  本方案落在一个**新**的独立测试文件（`test_ftl.c`，用户自建）
- 不扩 linker 加 region
- **不纳入**：`allocator_alloc`（v0.66 已删死码）；`metadata_dir_append_slot`/`metadata_dir_compact`
  （需真 NAND 擦写 → `hil_ut_design.md`）；`ftl_wbuf_*`（全 `static`，需真 NAND → `hil_ut_design.md`）

## 9. 测试分层与路线

### 9.1 行业分层（L0→L3）

| 层 | 运行环境 | 测什么 | 本项目 |
|---|---|---|---|
| **L0 单元** | 板上 RAM（纯函数） | 纯算法：allocator / L2P / GC-WL victim / 水位 / 坏块 / 磨损 / 组语义 | **本文档 §4** |
| L1 固件集成 | 板上 + Mock NAND | write / GC / 掉电恢复全生命周期 | **不做**（见 §9.2） |
| L2 系统 | 真 NAND 上板 | 压力 / 写后读 / **跨重启** | **`hil_ut_design.md` Part B~D** |
| L3 HIL | host 协议模型 | 协议级命令流（NVMe/UFS） | 不适用（本项目无 NVMe） |

### 9.2 本项目路线：**不做 mock，全用板子**

行业 FTL UT 的主体是 **L0 + L1（Mock NAND）**，但**本项目刻意不走 L1**，理由：
- Mock NAND 的唯一拦截点是 `fc_submit`（`include/fc/fc_interface.h:95`）。**桩不存在，且用户定调不建**。
- 纯逻辑（水位 / victim / 磨损 / 组语义）本就能在 **L0** 用静态 fixture 钉死，不需要 NAND 替身。
- 需要真 NAND 的路径（GC 搬移 / journal replay / boot recovery / **跨重启持久化**）直接在**真板上**
  跑——尤其跨重启**只能用物理断电**，反而更接近真实失效场景。**见 `hil_ut_design.md` Part B~D。**

### 9.3 断言套路（行业经典，抄入用例）

| 模式 | 例子 |
|---|---|
| 不变量 | `free + open + closed + bad == 总块数`（P1-3 后 `free_count` 是唯一池计数） |
| 状态机全路径 | allocator 块 FREE→OPEN→CLOSED→FREE 每步断言 state/sb_id |
| 水位边界 | 85%/95% 阈值上下 1 条 entry 处断言触发/不触发（v0.46 B1 就是水位公式 bug） |
| round-trip | 写 pattern → 读回 → 校验 |
| GC/WL victim 正确性 | 选到 VPC 最低 / read_count 最高的 closed SB |

## 10. 覆盖矩阵与用例总数

| 层 | 套件 | 被测模块 | 用例数 |
|---|---|---|---|
| L0 | `allocator` | `allocator.c` | 10 |
| L0 | `watermark` | `checkpoint.c` | 6 |
| L0 | `gc_wl` | `gc.c` / `wear_leveling.c` | 10 |
| L0 | `rr` | `read_recovery.c` | 6 |
| L0 | `bbt` | `bbt.c` | 4 |
| L0 | `metadata` | `metadata.c` | 6 |
| L0 | `pending_ct` | `pending.c` | 4 |
| L0 | `group` | `pending.c` | 6 |
| **合计** | **8 L0 套件** | — | **52** |

## 关键文件

| 文件 | 动作 |
|---|---|
| `docs/design/ftl_ut_munit.md`（本文档） | 设计（FTL 层 §4 逐条用例 + §5 技法 + §10 覆盖矩阵） |
| `tests/unit/test_ftl.c` | 实施：单文件（fixture 复用活实例 + 8 套件数组，L0）——**用户自建** |
| `src/ftl/makefile.mk` | 实施：`FC_NAND_TEST_ENABLE` 块（一行 `TEST_C_SRCS` + INCS） |
| `tests/unit/ut_main.c` | 实施：8 个 extern + `/ftl/*` 子套件注册 |
| `docs/design/hil_ut_design.md` | HIL 层 UT + 系统集成验收（非 FTL 部分） |

## 登记（本轮不做）

- **mock `fc_submit` 桩 / Mock NAND / 掉电注入器**：**用户定调不做**（见 §9.2）。
- `l2p_cache_note_access` 结算步导出为公开原语（否则 §4 测不到 thrashing）——属实施接线决策。
- host（x86）编译链：本仓只支持 `arm-none-eabi` 上板。

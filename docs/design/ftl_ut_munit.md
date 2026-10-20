# FTL 层测试用例设计（L0 munit 纯 RAM 套件）

> **状态：已实施（2026-10-14）**——`tests/unit/test_ftl.c`（8 套件 **55 例**；v0.74 加
> `op_fallback_per_plane` + `spare_pool_plane_isolated` 两例回归）已落地并接线（§6），
> `make tests` / `UT=FC` / `UT=FTL` 三档构建通过（0 warning）。**尚未上板验证**。
>
> **异步扩展计划（2026-10-15）**——见 §5.4 / §4.9 / §4.10 / §7 / §8 / §10：
> v0.75 已把「提交与完成」解耦（op tag + `g_op_callbacks[]` + 8 个 job 状态机），原 §5.4/§5.6
> 的「L0 只测簿记、提交→泵→完成 整链在 L2」边界**已过时**——提交与完成各自都是**纯 RAM**
> 可驱动点。新增 **L0 异步链套件**（`/ftl/async/`，测试侧伪完成注入 seam，生产码零改动）+
> **HIL 全链套件**（`/ftl-hil/`，真 NAND 上板）。
> ⚠ 本计划**只更新测试计划**：异步套件实现待 FTL 层改动落定后再推进（§5.6 列出
> 测试可能顶出的 FTL 生产候选改动）；共享夹具 `tests/unit/ftl_test_common.c/.h`
> （§3）已先行抽出并编译锚定（`make tests UT=FTL` 0 warning，53 例编译回归通过）。
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
（有效用例并入本文档 §4）；`src/ftl/makefile.mk` 当时无测试块（**2026-10-14 已补，见 §6**）。

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
| 2 | **堆与栈共享 128KB DTCM**：`_Min_Heap_Size=0x200`（`linker/stm32h743xx_flash.ld:19`）**不是堆上限**，只是 `._user_heap_stack` 的**最小预留**（链接期静态溢出检查，`:146-153`）。真实堆 = DTCM 底部 `end` 向上长到 `MSP - SBRK_STACK_GUARD(0x400)`（`src/platform/syscalls.c:73-96`），测试期栈峰值≈55KB ⇒ 可用堆**数十 KB** | 新 FTL 测试一律**静态 fixture + setup 里 memset 复位**，**禁止 malloc**——理由不是「堆只有 512B」，而是**大夹具（如已删的 `test_rebuild.c` ~176KB）必然撞栈 ⇒ HardFault** |
| 3 | `ftl_block_t`=**20B**（`bst.h:139` static_assert）；`super_block_t`=**12B**（`allocator.h:80`） | 见下预算表 |
| 4 | **`allocator_init` 现取 `const ftl_table_refs_t *`**（3 参）——五张表由 HIL **独立定义**，FTL 只收五个指针 | **fixture 必须提供五个独立数组 + `ftl_table_refs_t`** |
| 5 | `allocator_init` **不填 free pool**（只把非坏块置 FREE 状态、`free_count=0`），由 HIL/`main.c` 回填（`allocator.c:59` 注释 + `main.c:230`） | 用例须区分「init 后」与「回填后」两种状态 |
| 6 | `pending_*` / `pending_group_*` 取 **`ftl_context_t*`**（`pending.h:134`），需**完整上下文** | 见下「fixture 内存」一节 |

### 2.1 尺寸与内存预算（P2-5 后重算）

| 结构 | 组成 | 尺寸 |
|---|---|---|
| 五张表（独立对象，合计） | `blocks[4096]×20B`=80KB + `free_blocks[4096]×2B`=8KB + `op_blocks[2][204]×2B`=816B + `closed_sb_list[4096]×8B`=32KB + `spare_blocks[2][50]×2B`=200B | **123,896 B**（实测） |
| `metadata_dir_cache_t` | `entries[2048]×7B` | **≈14.4KB** |
| `ftl_context_t` | 内含 `tables`(101KB，**FTL 自持表** P2-5) + `cache`(128KB，**L2P 缓存**——非「表」；真 L2P 表在 NAND 的 region 池，SRAM 只驻 64 个 region) + `metadata_dir`(14.4KB) + journal + … | **≈263KB** |

> **⚠ 关键约束**：五张表的数组尺寸由 `FTL_BLOCK_TOTAL`（=4096）**固定**——原文档
> 「用 2112 项省内存」**不可能**（结构体按值含全 4096 项）。
> 且 **AXI_RAM(384KB) 已被 `main.c` 的 `g_ftl`(263KB，`sizeof(ftl_context_t)`) 占去 68.5%**
> （`main.c:131` 的 `_Static_assert`），独立 263KB 的 `ftl_context_t` fixture **放不下**
> （两份即 526KB > 384KB）——故 UT 复用固件本体：fixture 取 `g_ctrl.ftl[0][0][0]`
> （= `&g_ftl[0][0][0]`），见下。

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

> **⚠ `setup` 的复位范围 = 用例实际读写的子结构，按组补。** 上面只清了 §4.1/4.3/4.4/4.6/4.7/4.8
> 用到的 `tables` / `metadata_dir` / `pending` / `groups`。其余子结构**按用例组补**，否则会看到
> 开机残留或上一用例的脏状态：
>
> | 子结构 | 何时需要补 | 复位方式（同 `ftl.c:648/658` 的开机路径） |
> |---|---|---|
> | `F->cache`（`l2p_cache_t`，128KB） | L2P 命中/失效用例（如 thrashing） | `l2p_cache_init(&F->cache, F->topo.ch, F->topo.ce, F->topo.lun)` |
> | `F->journal`（`l2p_journal_t`） | §4.2 journal 水位（`checkpoint.c` 读 journal 用量） | `journal_init(&F->journal, F->topo)` |
> | `F->wbuf`（`ftl_wbuf_t`） | 页打包写缓冲用例（`ftl_wbuf_*` 全 `static`，本属 L2） | `memset` |
> | `F->cp` / `cp_timer_start` / `total_writes` | checkpoint 触发 / 计数断言 | `memset` / 置 0 |
> | `F->rr` / `rr_last_scan_ms` / `rr_page_scratch` | read recovery 用例 | `memset` |
> | `F->bg_suspend` | 后台闸门相关 | 置 0 |
>
> `F->topo` **保持不动**（NAND 路由，必须有效）。稳妥做法：每个套件的 `setup` 把**该套件会碰的
> 字段全清**，别只清一半——只读不写的字段可留。

- **好处**：零额外内存、单一数据源（不复制 263KB）。
- **代价**：fixture 不再「独立」；须保证 `test` 执行期间**后台泵不并发**——`test` 命令在
  `cli_service_poll` 里**同步执行**（不重入泵），天然满足。
- **备选**（不推荐）：独立 `ftl_context_t` 落 SRAM（`.sram1/2/3_data`=288KB，**仅 1KB 余量**），
  或只对不需要上下文的用例用独立小 fixture（五张表合计 123,896 B 落 SRAM1）。

## 3. 测试文件组织（仿 fc_nand 一文件多套件）

**共享夹具已抽**（2026-10-15）：`ftl_fixture()` / `ftl_fixture_reset()` 从 test_ftl.c
搬进 **`tests/unit/ftl_test_common.c/.h`**（与 L0 异步套件共用；`ftl_async_fixture_reset()`
在基础复位之上**额外零化异步 job 结构**——`alloc.bbt` / `alloc.wear` / `mig` / `checkpoint_handle`，
见 §5.4）。test_ftl.c 保留本地 `ftl_setup()`（包共享复位）+ `ftl_fixture_backfill()`。

**同步套件（已实施）**：`tests/unit/test_ftl.c`，fixture 见 §2.2（复用 `g_ctrl.ftl[0][0][0]`，
不另声明大数组）。文件内按 munit 风格分节，**导出 8 个套件数组**（非 static，供 `ut_main` extern）：
`g_ftl_allocator_tests[]` / `g_ftl_watermark_tests[]` / `g_ftl_gc_wl_tests[]` / `g_ftl_rr_tests[]` /
`g_ftl_bbt_tests[]` / `g_ftl_metadata_tests[]` / `g_ftl_pending_ct_tests[]` / **`g_ftl_group_tests[]`**。
setup 统一 `ftl_fixture_reset()`，teardown NULL，`SINGLE_ITERATION`。

**L0 异步套件（计划）**：新文件 `tests/unit/test_ftl_async.c`，挂 `/ftl/` 根下 `/ftl/async/`
叶子（`UT_FTL_ENABLE` 门控，`UT=FTL` 仍纯 RAM）——用例清单见 §4.9，驱动模型见 §5.4。

**HIL 全链套件（计划）**：新文件 `tests/unit/test_ftl_hil.c`，**独立根 `/ftl-hil/`**、
新 `UT_HIL_ENABLE` 门控**仅 ALL**——与 `/ftl/` 的 L0 破坏性 memset 互斥（L0 复位活实例会
毁掉 HIL 需要的 boot 态）。用例清单见 §4.10，上板纪律见 §7。

## 4. 用例清单（逐条）

> 表式：**用例名 | 前置 | 动作 | 断言**。断言优先用**不变量/边界**（判据 7 参数化 vs 迭代、
> 判据 8 负例钉边界）。**全部为纯 RAM，板上 `test` 跑，无 mock。**

### 4.1 `g_ftl_allocator_tests[]`（`src/ftl/allocator.c`）

| 用例 | 断言 |
|---|---|
| `init_zeroes_pool` | `allocator_init` 后 `free_count==0`、`op_count[plane]==0`、`spare_count[plane]==0`（**逐 plane**）、非坏块 state==FREE（**init 不回填**，§2 行 5） |
| `backfill_fills_pool` | 回填 n 个好块后 `free_count==n`；池内每块 state==FREE |
| `free_block_clears_state` | 对 OPEN/CLOSED/RESERVED/TEMP 块调 `allocator_free_block` → state==FREE + 入池 + 计数+1 |
| `close_open_sb` | `allocator_close_open_sb`：新 closed SB 两 plane 块 state==CLOSED + `sb_id` 正确 + `closed_sb_count`+1 |
| `add_closed_sb` | `allocator_add_closed_sb` → count+1、两块 CLOSED + sb_id；**条目字段**：`next_page==PAGES_PER_BLOCK`、`temperature` 透传、`valid==0`、`topo` 正确；两块 **清 FREE/OPEN** 位 |
| `add_closed_sb_full` | ~~**边界**：塞满 `SUPER_BLOCK_LIST_MAX` 后返回 `UINT16_MAX` 且**不写入**~~ **已撤销**：v0.78 起容量 = `FTL_BLOCK_TOTAL`（每条占 1 块 ⇒ 满结构上不可能），实现是 ASSERT 而非返回码，本用例不可测（见 `test_ftl.c` 文件头 ①） |
| `remove_closed_sb_swap_remove` | 删中间项：末项搬到该位**且 `sb_id` 修正**、count-1（swap-remove 索引坑）；被删块 `sb_id` 复位 `FTL_BLOCK_NO_SB` |
| `remove_from_free` | `allocator_remove_from_free`（swap-remove）→ count-1、块不在池；**移除不存在的块无害**（count 不变，负例） |
| `reserved_alloc_free` | `allocator_alloc_log_block` 置 LOG；`allocator_free_block` 归还池 |
| **`open_sb_never_picks_metadata`** | **回归用例**：metadata 块 0..62 被 `allocator_init` 标 FREE 但**不在 Free Pool** ⇒ 新建 SB 两 plane 块都 **≥63**（钉住历史真缺陷） |
| `open_sb_min_erase_count` | 池内多块时选该 plane `erase_count` 最小者（动态 WL） |
| **`op_fallback_per_plane`** | **回归用例（v0.74）**：某 plane Free Pool 空、其 OP 池有块 ⇒ `allocator_alloc_pair` 必须成功且该 plane 拿到 **OP 池里那块**（修复前 OP 池全局，拉进来的是 plane 0 的块 ⇒ 选不中） |

> ⚠ **L0 只能调公开函数**：`allocator_open_sb`（`allocator.c:209`）、
> `allocator_select_plane_block_from_pool`（`:166`）、`allocator_pull_from_op`（`:136`）都是 `static`
> ⇒ 上两条经**公开入口 `allocator_close_open_sb`（`:370`，内部调 `open_sb`）间接测**：
> 调用后断言新 closed SB 的 `plane_blocks[p] ≥ 63` / `erase_count` 最小。
> （`op_fallback_per_plane` 走公开入口 `allocator_alloc_pair`，可直接测 OP 兜底。）

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
| `gc_trigger_T1_T4_T5` | `gc_check_triggers` **三位**各自置位（参数化）：T1 `GC_TRIGGER_WATERMARK(0x01)` / T4 `GC_TRIGGER_EMERGENCY(0x08)` / T5 `GC_TRIGGER_WEAR(0x10)`（`GC_TRIGGER_*`）——T2 写数 / T3 定时不实现 |
| `wl_trigger_delta` | 条件 1：`max-min ≥ WL_DELTA_THRESHOLD(5000)` |
| `wl_trigger_ratio` | 条件 2：`max ≥ avg×WL_MAX_AVG_RATIO(3)` |
| `wl_trigger_low_zone` | 条件 3：低擦除区块占比 **>** `WL_LOW_BLOCK_PCT(5)%` |
| `wl_victim_high_valid_low_erase` | `wl_select_victim` 选有效页最多 + 擦除最少（`valid≥89`） |
| `wl_victim_read_count_weight` | 平手时 `read_count` 高者优先（B5 权重） |

> `wl_max_erase` / `wl_avg_erase` / `wl_count_blocks_below` 已删除（零生产消费者）；
> 原 `wl_max_min_avg_erase` / `wl_count_blocks_below` 两个用例需由测试源所有者同步删除。

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
| `mark_bad_idempotent` | 重复标坏**不**重复计数、**不**重复弹 Spare（负例）。⚠ 现有用例只钉位级；`bbt_mark_bad` 幂等修复后应由测试补钉 `bad_count` 与 `spare_count` |
| `spare_pool_refill` | `bbt_spare_pool_refill(alloc, plane)`：spare→free 补充 + 该 plane 计数守恒；耗尽 / `NULL` / plane 越界 → `FTL_ERROR` 且不动 free pool |
| **`spare_pool_plane_isolated`** | **回归用例（v0.74）**：refill plane 1 ⇒ 只减 `spare_count[1]`、`spare_count[0]` 不变，且补进 free pool 的是 **plane 1 的块**（修复前全局池 ⇒ 补的是 plane 0 的块） |
| `is_bad_set_bad_inline` | `bbt_is_bad`/`bbt_set_bad` 纯 bitmap（含 `NULL`→0；越界块保守判坏） |

> **未覆盖**：`bbt_load` / `bbt_save` / `bbt_handle` 的池定位与持久化、`bbt_scan_factory`、
> `bbt_format_erase`（两阶段格式化 / `FORMAT_IN_PROGRESS` 中断恢复）、
> `bbt_medium_has_ftl_data`——需要 NAND seam 或上板验证。

### 4.6 `g_ftl_metadata_tests[]`（`src/ftl/metadata.c`）

| 用例 | 断言 |
|---|---|
| `dir_init_entries` | `metadata_dir_init` 后 `count==2048`（= `L2P_REGIONS_PER_INSTANCE`）、全 UNKNOWN |
| `dir_update_lookup_roundtrip` | `metadata_dir_update` 写一个 region → `metadata_dir_lookup` 回同 entry |
| `dir_lookup_missing_unknown` | 未写 region → `0xFFFFFFFF`（负例） |
| `is_metadata_block` | `ftl_is_metadata_block(dir, plane, block)`：**0..77 true、78 false**（边界，判据 8）。区间来自分区表：Anchor[0,32)+Ckpt[32,40)+BBT[40,42)+Journal[42,46)+**L2P log[46,78)** ⇒ `FTL_METADATA_AREA_END==78`。固定区按 `plane_mask` 比较 plane，用例覆盖两 plane。⚠ **勿写死旧布局**（Wear 块已删；0..45/46 是 v9 旧值，介质版本 v10 已改）。⚠ 测试源当前冻结（RISK-MD-03），用例仍按旧边界，需测试所有者同步 |
| `is_metadata_block_dynamic` | 动态 region：`metadata_dir_update(L2P_REGION, r, pba)` 登记 `block>=46` 后 `ftl_is_metadata_block` 命中；**`md_dir==NULL`** 时仍命中固定区（0..45）、动态块不命中。⚠ 断言**另一个 plane 的同号块不受掩**（`(d, 1U, 100U) == 0`）——动态日志块取自用户区，只比块号会连 plane1 的用户块一起排除 |
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

### 4.9 `g_ftl_async_tests[]`（计划，L0 异步链 —— `/ftl/async/`）

> 新文件 `tests/unit/test_ftl_async.c`；驱动模型见 §5.4。统一模式：`ftl_async_*` →
> `ftl_op_submit(cmd, op, ctx)` → `ut_complete_and_drain(cmd, 0x40)` → 断言。
> **每用例 setup = `ftl_async_setup`**（基础复位 + 零化异步 job 结构）；单命令在飞；收尾断言
> `completion_table_count()==0` + `command_used` 回基线。表式：**用例名 | 驱动 | 断言**。

| 组 | 用例 | 驱动 / 前置 | 断言 |
|---|---|---|---|
| **C.1 op 框架** | `op_tag_and_registration` | `ftl_async_erase_block` → `ftl_op_submit(cmd, FTL_OP_SYNC_WAIT, &waiter)` | `cmd->op_tag==FTL_OP_SYNC_WAIT`、`sq.count==1`、`completion_table_count()==1` |
| | `sync_wait_not_release` | 伪完成 + drain（waiter.done==1） | **命令不释放**（`command_used` 仍占用）；手动 `fc_command_release` 才回落 |
| | `async_job_self_release` | journal 写提交 → 伪完成 + drain | 服务函数**自行 release**（`command_used` 回基线） |
| | `backpressure_ct_full` | 32 条 `ctx≠NULL` 提交在飞 → 第 33 条 `ftl_op_submit` | `FTL_ERROR` + `completion_table_count()` 不变（32）；逐条伪完成 + release 收尾 |
| | `backpressure_pool_exhaust` | 64 条 erase 提交在飞 → 第 65 次 `ftl_async_erase_block` | `FTL_AGAIN` + 在飞命令数不变；逐条伪完成 + release 收尾 |
| **C.2 journal** | `write_complete_idle` | `journal_append`×2 → `journal_handle`（JRN_WRITE）→ 伪完成 | JRN_IDLE + 指针推进到 target + `current_entry==0` + `page_buf` 0xFF |
| | `cross_block_erase_chain` | 预置 `current_page=PAGES_PER_BLOCK-1` → append → kick（need_erase）→ 伪完成 | 服务函数**当场起擦**（JRN_ERASE）→ 再伪完成 → IDLE + `current_block` 回绕 |
| | `write_fail_advance_past_page` | kick 后伪完成 status=0x01 | 回 IDLE + 指针**推进到 target**（失败页不可复用）+ `write_fail_pending` 置位 + release 正常 |
| **C.3 bbt** | `write_complete_clear_dirty` | 预置 `cursor_valid=1`、`dirty=1`，`dirty_gen` 不变 → `bbt_handle`（WRITE）→ 伪完成 | IDLE + `dirty==0` + `write_idx` 前移 1 |
| | `write_complete_dirty_gen_advanced` | 写飞行期间 `bbt_mark_bad`（`dirty_gen++`）→ 伪完成成功 | IDLE + **`dirty` 保留**（本条 record 是旧快照，需再落一条） |
| | `erase_then_write_chain` | 预置 `cursor_valid=1`、`dirty=1`、`write_idx=ring_total-1` → kick（ERASE）→ 伪完成 | 服务函数**链起 WRITE**（不回 IDLE）→ 再伪完成 → IDLE + `dirty==0` |
| | `write_fail_keep_dirty` | 伪完成 status=0x01 | 游标**照推**（半编程槽不可复用）+ `dirty` 保留 |
| **C.4 wear** | `incremental_batch` | `wear_mark_dirty`×2 → `wear_kick(alloc, DELTA+1)`（节拍到）→ 伪完成 | IDLE + `next_page==1` + 本批起点快照写全 |
| | `compact_erase_snapshot_chain` | 预置 `next_page=PAGES_PER_BLOCK-1` → kick（ERASE）→ 伪完成 | `next_page=0` + 起全量快照链 → `while(phase!=IDLE) 伪完成` → `next_page==WEAR_SNAPSHOT_PAGES` |
| | `incremental_fail_restore_dirty` | 伪完成 status=0x01 | `next_page` 不推进 + 脏位并回（`dirty |= snapshot`） |
| **C.5 mig（快路径）** | `gc_fastpath_erase_done` | `closed_sb_list[0]` 塞 `valid_total==0` victim + `free_count<FREE_POOL_TARGET` → `mig_begin_gc`（MIG_ERASE）→ 伪完成 | `mig_step` 自动续 → DONE + `free_count` 增加 |
| | `gc_full_chain`（**标记可选**） | 预置 cache 行 CLEAN/DIRTY + entries + META → READ→SETTLE→WRITE→FLUSH_JRN→ERASE 各命令伪完成 | 状态机走全链 → DONE；**实现期评估 §5.6 候选 1**；完整搬移放 HIL |
| **C.6 cp/md** | `cp_kick_complete_alternate` | `cp_begin` → 循环「`checkpoint_handle`（waiting 则 return）→ 伪完成」至 CP_DONE；预置游标 | 各提交阶段恰一命令在飞、终态 DONE |
| | `md_reclaim_kick_complete` | `metadata_handle` ↔ 伪完成交替 | `md_job` 各阶段推进至 IDLE |
| **C.7 l2p_cache** | `miss_loading_clean` | `l2p_cache_read` miss → LOADING + `FTL_AGAIN` → 伪完成（预填 region 数据进 desc buffer） | CLEAN + entries 生效 + 再 `l2p_cache_read` 命中 `FTL_OK` |
| | `flush_dirty_flushing_clean` | `l2p_cache_write`（DIRTY）→ `l2p_cache_flush_kick` → FLUSHING → 伪完成 | CLEAN + region 脏位清 |
| | `flush_fail_stays_dirty` | 伪完成 status=0x01 | 置回 DIRTY 保留下轮重试 |
| **C.8 rr（简化）** | `stage_pure_progression` | SF_SET→READ→…→FINISH 每命令一伪完成 | 只钉 stage 纯推进（不结算 host 组——结算交 HIL） |

> 长链模板（C.4 compact / C.5 全链 / C.2 跨块）：`while (job.phase != IDLE) ut_complete_and_drain(ut_sq_head(), 0x40);`
> ——`ut_sq_head()` 每次取**新**在飞命令（单命令在飞纪律下即刚提交那条）。

### 4.10 `g_ftl_hil_tests[]`（计划，HIL 全链 —— 独立根 `/ftl-hil/`）

> 新文件 `tests/unit/test_ftl_hil.c`；`UT_HIL_ENABLE` 门控**仅 ALL**（顶层 makefile 的
> `UT` 空分支才置位）；**fresh boot 后单独跑 `test /ftl-hil/`**，不得并入全量 `test`
> （全量先跑 FC level3 会重置 FC 池 + 破坏 NAND，污染 HIL 介质前提）。**setup 不做任何
> reset**（boot 态即活实例，main 已 armed）。泵模板：`ftl_service_pump()`（全局 fc_service≤4 +
> completion_table_drain）+ `ftl_service(F)`（Phase 2.5 补发 + wbuf flush + Phase 3 kick）。
> 用例（裸 `ftl_submit_*` + 静态 done/status 数组 + `ftl_flush` 保 wbuf 确定性）：

| 用例 | 驱动 | 断言 |
|---|---|---|
| `async_pipeline` | k 组同窗口小写 → 泵至 done==k 且 pending 空 | **恰一次**完成 + status OK + 回读逐字节比对 |
| `backpressure` | 大请求多子操作 | 组只结算一次、无请求丢失 |
| `group_complete_once` | 多子操作组 | 钉「提前完成」缺陷（`complete_flag`/`status_out`/`on_complete` 各恰一次） |
| `read_write_roundtrip` | 随机 lba 往返 | 写后读逐字节一致；未映射读全零 |
| `journal_flush_barrier` | `hil_flush` | `current_entry==0`（屏障已排空） |

## 5. 测试技法：随机化 / 参数化 / 压力 / 异步

> 沿用 FC-NAND 三级 UT 已固化的技法（`fc_nand_ut.md §2.3 / §5.4 / §5.5`），把 FTL L0 从
> 「固定输入」升级为「随机化 + 参数化 + 可加压 + 可模拟异步」。§5.4 的 L0 异步 seam 是
> **测试写法、生产零改动**；异步链测试可能顶出的 FTL 生产改动见 §5.6。

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

### 5.4 异步（v0.75 提交/完成解耦后重写）

**旧模型（§5.4 原稿）已过时**：解耦前「提交」要 `fc_command_alloc`（FC 层）、完成只能经
真 NAND 触达 ⇒ L0 只测组簿记、整链留 L2。v0.75 把「提交」与「完成」**解耦**后，两端各自
都是**纯 RAM 可驱动点**——L0 现在能驱动**完整异步链**（`ftl_async_*` → `ftl_op_submit` →
测试侧伪完成 → `completion_table_drain` → `g_op_callbacks[]` 服务函数），零 NAND、零生产改动。

#### 5.4.1 链路结构（一次异步 NAND 访问的两半）

| 半 | 入口 | 行为 | L0 驱动方式 |
|---|---|---|---|
| 提交 | `ftl_async_write_page/erase/read_page/set_features`（`ftl_fc_interface.h`） | 建 desc 链 + 搬数据，**不提交**，命令交回调用者 | 直调（返回 `FTL_OK` 才继续） |
| 提交 | `ftl_op_submit(cmd, op, ctx)`（`ftl_op.h:89`） | 打 `cmd->op_tag` + `ctx≠NULL` 时 `completion_table_add`（**早于** `fc_submit`）→ `fc_submit`；失败回滚登记 | 直调；`ctx==NULL` 主动清表项 |
| 完成 | 测试侧 `ut_fake_complete(cmd, status)`（新 seam） | 摘 SQ/HSQ → 沿 desc 链回写 status → 构 `fc_command_status_t` → `fc_completion_queue_push` + 置 `FC_COMMAND_STATE_COMPLETED` | **纯测试代码**，见 5.4.2 |
| 完成 | `completion_table_drain()` | 消费 CQ → `ftl_op_dispatch`（按 `op_tag` 查 `g_op_callbacks[]`，`completion_table_take` 取 ctx，`completion_table_route` 按命令自身 ch/ce/lun 解析实例） | 直调（`fc_service` 无需调——NAND 不真跑） |

**为什么 L0 够得到**（seam 可行性依据，已逐行核实）：
- `g_fc_controller` 是 **extern**（`fc_internal.h:85`）、`fc_t` 字段公开 ⇒ SQ/HSQ/CQ 环形队列
  与 `command_state[]` 可从测试侧直接操作；
- `fc_completion_queue_push` 公开（`fc_queue.h:55`）；`FC_COMMAND_STATE_COMPLETED`
  （`fc_internal.h:94`）——置位与 `fc_complete_from_nand` 同构；
- 命令身份：`ftl_cmd_id`（`ftl_fc_interface.h:77`）、`fc_command_id_to_command` /
  `fc_descriptor_id_to_descriptor`（`fc_interface.h:156/165`）；
- 成功态 = `FC_CW_STATUS_SUCCESS = 0x40`（`fc_command.h:144`）；完成项 `desc_id =
  FC_DESCRIPTOR_ID_INVALID`（成功无失败 CW）。

#### 5.4.2 seam 纪律（`ut_fake_complete` 的三条铁律）

1. **必须同时从 SQ/HSQ 摘除该命令**（环形左移压缩）：否则 `test` 返回后 superloop 的
   `fc_service` 会把 **stale QUEUED** 命令派给真 NAND，`fc_poll_dispatch_single`（`fc.c:1516`）
   的 `ASSERT(state==QUEUED)` 在 debug 下必爆 ⇒ 测试把板子打崩。
2. **只接受「`ftl_op_submit` 已返 `FTL_OK`」的命令**（在 SQ/HSQ、`state==QUEUED`）；对
   `FTL_AGAIN`（背压回滚）路径**绝不能**调它。
3. **每用例结束断言 `completion_table_count()==0` + 命令池回基线**
   （`fc_get_resource_stats` 的 `command_used`）：漏 release 即命令池泄漏 ⇒ 后续用例全系统
   `FTL_AGAIN`——测试把「release 归属」变成可断言的硬不变量。

#### 5.4.3 release 归属（测试要对它下断言的核心语义）

| op tag | 谁 release | L0 断言法 |
|---|---|---|
| `FTL_OP_SYNC_WAIT` | **不 release**（调用者 wait 返回后读 `ftl_cmd_read_data` 才 release；服务函数只唤醒 waiter） | 伪完成后 `command_used` 仍占用；手动 `fc_command_release` 才回落 |
| `FTL_OP_HOST_IO` 与各异步 job（JOURNAL/L2P/CHECKPOINT/BBT/WEAR/METADATA/GC/WL/RR） | **服务函数自己 release** | 伪完成 + drain 后 `command_used` 回基线 |

#### 5.4.4 背压面（可测的真实路径）

| 背压 | 触发 | 断言 | 备注 |
|---|---|---|---|
| **命令池耗尽**（真实背压） | 64 条命令全在飞（如 64 条 `ftl_async_erase_block` 提交在飞）⇒ 第 65 次 `ftl_async_*` 返 `FTL_AGAIN` | `FTL_AGAIN` + 在飞命令数不变 | 系统实际撞到的就是它 |
| **completion_table 满** | 32 条 `ctx≠NULL` 提交未完成 ⇒ 第 33 次 `ftl_op_submit` 返 `FTL_ERROR` | `FTL_ERROR` + `completion_table_count()` 不变（32） | `COMPLETION_TABLE_SIZE=32`（`completion_table.h:46`） |
| **SQ 满 → `ftl_op_submit` 的 `FTL_AGAIN` 回滚** | — | — | **结构性不可达**：命令池 64 == SQ 深 64（`fc.h:56/70`），池满先于 SQ 满 ⇒ 该分支在本配置是死路径，不测 |

#### 5.4.5 job 状态机驱动（链式 vs 踢↔伪完成交替）

按服务函数是否**自动续提交**分两类（§4.9 用例按此驱动）：

- **链式**（服务函数回调里续提交下一条命令，`ut_fake_complete` 后 `drain` 一次自动起下一条）：
  journal（写→跨块擦）、bbt（擦→写）、wear（擦→快照多页链）、migrate（搬移多命令链）。
  ⇒ 用例循环 `while (phase != IDLE) { ut_complete_and_drain(ut_sq_head(), 0x40); }`。
- **踢↔伪完成交替**（服务函数只记完成 + 改 phase，**不**调 kick——kick 的 PREP 阶段含同步
  NAND 原语，在完成泵里跑就是嵌套泵）：checkpoint（`checkpoint_handle` ↔ 伪完成）、
  metadata 整理（`metadata_handle` ↔ 伪完成）。
  ⇒ 用例手动交替「kick → 伪完成 → kick → …」直到 DONE。

**job 专用前置**（L0 不碰 NAND，须预置避开同步 NAND 路径）：
- `bbt_handle` / `checkpoint` 的游标定位（`metadata_ring_locate` / Anchor 定位）是**同步读 NAND**
  ⇒ 预置 `cursor_valid=1`（bbt）/ `anchor_cursor_valid=1`（cp）跳过；
- migrate 的 READ/WRITE_SETTLE 会 `l2p_cache_ready==0` **原地返回**（§5.6 候选改动 1）⇒ 全链
  用例预置 cache 行 CLEAN/DIRTY + entries，或只测不依赖该等路径的快路径（valid_total==0 → ERASE）。

#### 5.4.6 与既有「组簿记」的关系

§4.8 的组完成语义（`pending_group_*`）仍是 L0 纯簿记测试（不依赖伪完成）；host 读写的完成
服务（`FTL_OP_HOST_IO`：建组 → 逐子操作提交 → 伪完成 → 组聚合恰一次）可随 C.1 框架驱动、
用例并 §4.8（组簿记）+ C.1（op 框架）覆盖，不在 §4.9 单列；`l2p_cache` 的加载/写回链
（§4.9 C.7）进 L0；真 NAND 的搬移全链（GC/WL/RR 组结算）归 HIL（§4.10）。

### 5.5 时间注入（节流类用例）

FTL 的时间源经**平台抽象**：`ftl_now_ms` / `checkpoint_now_ms` 读 `ssd_platform_get()->time_us()`
（`ftl.c:83` / `checkpoint.c:50`）。⇒ **时间可注入**——测试克隆 `ssd_platform_ops_t`、把
`time_us` 换成受控实现、`ssd_platform_register(&fake)`（Stage 1 v55 已建该抽象），用例后还原。
故 `ftl_service` 内部的**节流时机**（T3 定时 / RR 扫描节流）也能 L0 确定性驱动——**无需给 FTL 加 seam**。

### 5.6 回到「FTL 设计需不需要改」——L0 异步 seam：**生产零改动**；但 FTL 层**还可能改**（用户定调）

**L0 异步 seam 不需要改生产码**（§5.4.1 的 extern `g_fc_controller` + 公开队列 API 已核实）：
伪完成是**测试侧 helper**，不是 mock 生产函数——`fc_complete_from_nand` 仍原样存在，只是
测试不经过真 NAND 就够不到它，改用公开 CQ API 注入同构完成项。故 §5.4 的全部 L0 链驱动
**生产码零改动**。

**但异步测试会顶出 FTL 生产候选改动**（§8 的旧边界「零改动」随 v0.75 异步化失效；
用户 2026-10-15 已确认 FTL 层还可能改）。测试实现期如被顶出来，按提交纪律走、不混进
测试提交：

| # | 候选改动 | 测试怎么顶出 | 风险面 |
|---|---|---|---|
| 1 | **migrate READ/WRITE_SETTLE 等待 `l2p_cache_ready`**（`mig_step_read_settle` / `mig_step_write_settle` 原地返回；`l2p_cache_ready` 在 miss 时会**起异步加载**） | 全链 GC 用例预置非就绪 region ⇒ 状态机停在 SETTLE，等 region 加载完成后下一轮继续 | 该 region 恰无人访问时由 `l2p_cache_ready` 自行触发加载；等待期间 `stall_ms` 被清零，**不是**上限兜底 |
| 2 | **checkpoint 水位只置不撤**（`checkpoint_journal_watermark` 置位，撤位在 CP_TRUNC_* → `journal_truncate_commit`，需真 NAND） | EMERGENCY 回落用例 L0 无法纯 RAM 收口；要么驱动 CP 走到 TRUNC 阶段再断言，要么列 HIL | 只影响测试收口方式，非缺陷 |
| 3 | completion seam fallback | 若个别读路径需要 `process_read` 数据语义（desc 每 CW status 回写 + `failed_descriptor_id` 聚合）而测试侧失真 | fc.c `#if FC_NAND_TEST_ENABLE` 薄 seam（包现成 `fc_complete_from_nand`） |
| 4 | **红线**：派发上下文不得调同步原语（`ftl_op_in_dispatch` ASSERT，`ftl_fc_interface.c:535`） | 测试泵只能在非回调处（`ut_complete_and_drain` 外）用 | 服务函数里嵌套泵 = 解耦要消除的东西 |

> 时间源：`ssd_platform_ops_t.time_us` 经 `ssd_platform_register`（`platform.h:14/19`）仍可注入，
> 供 `wear_kick` 节拍 / `checkpoint` 定时类用例用（§5.5）。
> 内存张力（fixture 263KB ⇒ 复用活实例 `g_ctrl.ftl[0][0][0]` + reset，§2.2）是内存约束，
> 非设计缺陷——不构成对 FTL 生产码的修改要求。

## 6. 接线（已实施）

### 6.1 UT 范围开关（`make tests UT=…`）

顶层 `makefile` 解析 `UT`（空 = 全部 / `FC` / `FTL`），派生出 `UT_FC_ENABLE` / `UT_FTL_ENABLE`；
两个模块 makefile 据此决定编入哪些测试源，`ut_main.c` 用同名宏做条件编译。三者一一对应：

| `UT` | `UT_FC_ENABLE` | `UT_FTL_ENABLE` | 编入的测试源 | 对象目录 |
|---|---|---|---|---|
| （空） | 1 | 1 | munit + ut_main + fc_test_common + level1/2/3 + test_ftl | `obj_ut` |
| `FC` | 1 | — | munit + ut_main + fc_test_common + level1/2/3 | `obj_ut_FC` |
| `FTL` | — | 1 | munit + ut_main + test_ftl | `obj_ut_FTL` |

- **`munit.c` + `ut_main.c` 恒编入**：`cli.c` 的 `test` 命令（`#if FC_NAND_TEST_ENABLE`）引用
  `ut_run_all`，摘掉即链接期 undefined symbol。
- **对象目录按配置分置**（`config.mk` 的 `OBJ_DIR` 含 `FC_NAND_TEST_ENABLE` 与 `UT`）：本工程无
  「CFLAGS 变化 ⇒ 重编译」依赖，共用目录会复用带不同 `-D` 的陈旧 `.o`（症状：产品固件里没有
  `test` 命令 / `ut_main.o` 引用未编入的套件数组而链接失败）。分置后 `make all` ↔ `make tests` ↔
  各 `UT` 之间**互不污染，无需 `make clean`**。
- **`ut_main.c` 是两层测试的唯一耦合点**：`test_ftl.c` 只依赖 FTL/NAND/munit/`g_ctrl`；
  `fc_test_common.*` + `test_fc_level{1,2,3}.c` 只依赖 FC/NAND/munit —— 两侧互不引用，
  只有 `ut_main.c` 同时 extern 两边的套件数组，故条件编译只需加在那里。

**`src/ftl/makefile.mk`**（`FC_NAND_TEST_ENABLE` 块内再套 `UT_FTL_ENABLE`，**一行测试源**）：

```makefile
ifeq ($(FC_NAND_TEST_ENABLE),1)
ifeq ($(UT_FTL_ENABLE),1)
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_ftl.c
CFLAGS += -DUT_FTL_ENABLE
endif
MODULE_INCS += $(PROJECT_ROOT)/tests/unit
MODULE_INCS += $(PROJECT_ROOT)/tests/framework
endif
```

**`tests/unit/ut_main.c`**：8 个 `extern MunitTest g_ftl_*_tests[]` 与 `/ftl/` 根套件项整体包在
`#if defined(UT_FTL_ENABLE)` 内（FC 侧同理包在 `#if defined(UT_FC_ENABLE)` 内）。子套件前缀统一
`xxx/`（**必须以 `/` 结尾**，`ut_main.c` 注释：munit 只做首尾相接、不补分隔符）。

### 6.2 异步 / HIL 接线（计划）

| 文件 | 改动 |
|---|---|
| `src/ftl/makefile.mk` | `UT_FTL_ENABLE` 块加 `ftl_test_common.c`（**已实施**）+ `test_ftl_async.c`（计划）；新增 `UT_HIL_ENABLE` 块加 `test_ftl_hil.c` |
| 顶层 `makefile` | `UT` 空分支（ALL）加 `UT_HIL_ENABLE := 1`（**仅 ALL**——HIL 上板前须 fresh boot，不入 `UT=FTL` 的纯 RAM 档） |
| `tests/unit/ut_main.c` | `/ftl/` children 加 `{"async/", g_ftl_async_tests, …}`（前缀必带尾斜杠）；`#if defined(UT_HIL_ENABLE)` 加 `extern g_ftl_hil_tests[]` + 独立根 `/ftl-hil/` |

> 三档语义：`make tests UT=FTL` = 同步 53 例 + L0 异步链（全纯 RAM，`obj_ut_FTL` 无 HIL 目标）；
> `make tests`（ALL）额外编入 `/ftl-hil/`（`obj_ut`）。对象目录分置机制（§6.1）保证三档互不污染。

## 7. 验证

1. `make clean && make tests`（`FC_NAND_TEST_ENABLE=1`）**零错误零新增警告**——fixture 复用活实例，
   不新增 NOLOAD 段；确认 AXI_RAM 用量不超（§2.1）
2. `make debug / release` 不受影响（`FC_NAND_TEST_ENABLE` 默认关，测试文件不编入）
2b. `make tests UT=FC` / `UT=FTL` 各自 0 警告，`nm` 确认只含本层套件数组；三档之间切换
   **无需 `make clean`**（对象目录分置）
3. 上板（同步 53 例 + L0 异步，均纯 RAM）：
   - `test --list` → 确认 8 个 `/ftl/*/` 套件 + `/ftl/async/` 全部列出
   - `test /ftl/` → 全 PASS（尤其 `open_sb_never_picks_metadata`）；**`test /ftl/async/` 逐用例**，
     重点 C.1 的 SYNC_WAIT 不 release / 背压回滚、C.2~C.4 链式推进、C.7 LOADING/FLUSHING
     > ⚠ munit 的过滤要求「过滤串 = 套件累积前缀 + 用例名前缀」（`munit.c:1297-1303`），
     > 套件前缀本身匹配不到任何用例 ⇒ 裸 `test /ftl/` 原会打印 `NO test run`。
     > **2026-10-14 已在 `ut_main.c` 的 `ut_run_all` 里加了「以 `/` 结尾的实参展开为叶子套件路径」**，
     > 故 `test /ftl/`、`test /fc-nand/`、`test /` 现按直觉工作；`test /ftl/allocator/<用例名>`
     > 这类全路径过滤串行为不变。
   - **异步链不变量**：每个用例跑完后确认 `completion_table_count()==0` + `command_used` 回基线；
     `test` 返回后 superloop 不崩（伪完成已摘 SQ ⇒ 无 stale QUEUED）。
4. 上板（HIL 全链，**独立一次调用**）：
   - **fresh boot 后单独 `test /ftl-hil/`**（不得并入全量 `test`——FC level3 会重置 FC 池 + 破坏 NAND）；
     全绿：`async_pipeline` 恰一次完成 + 回读一致；`group_complete_once` 钉死提前完成。
   - 确认 `test` 返回后 superloop 继续跑不崩（HIL 用真完成路径，天然无 stale 问题）。
5. 随后执行 **`hil_ut_design.md` Part B~D**（接口行为 / 跨重启 / 后台观测）

## 8. 不做（边界）

- **不做 mock（生产函数桩）**：不建 `fc_submit` 桩 / Mock NAND / 掉电注入器。
  ⚠ **L0 的伪完成 seam（§5.4）不是 mock 生产码**——`fc_complete_from_nand` 原样存在，
  测试只经**公开** CQ API（`fc_completion_queue_push` + 手动置 `FC_COMMAND_STATE_COMPLETED`）
  注入与生产同构的完成项；生产码零改动。区别：mock = 替换被测路径的一部分；seam = 在
  测试侧构造输入的触发条件。
- 不改 `tests/framework`（munit 适配已完成）；不建 host（x86）target
  （**2026-10-14 例外**：`tests/unit/ut_main.c` 的 `ut_run_all` 加了套件路径展开——见 §7.3；
  `tests/framework/munit.c` 本身仍未动）
- ~~不动孤儿 `test_gc.c`/`test_l2p.c`/`test_nand.c` 占位~~ ✅ **2026-10-13 已删**（连同 `test_rebuild.c`），
  本方案落在一个**新**的独立测试文件（`test_ftl.c`，用户自建）
- 不扩 linker 加 region
- **不纳入（仍属 HIL）**：`allocator_alloc`（v0.66 已删死码）；`ftl_wbuf_*`（全 `static`，
  需真 NAND → `hil_ut_design.md` + §4.10 的 wbuf flush 泵）。`metadata_handle` 已**改纳入**
  L0 异步套件（§4.9 C.6 的 `md_reclaim_kick_complete`——kick 与伪完成交替即可驱动整理 job，
  不再需要真 NAND 擦写）。
- **不做（待 FTL 层改动落定）**：§4.9 的异步套件实现（计划已定、seam 已编译锚定，
  但 FTL 生产候选改动未定，避免按会变的接口写死测试——见 §5.6）。

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

**已实施（`tests/unit/test_ftl.c`，8 套件 55 例）**：

| 层 | 套件 | 被测模块 | 用例数 |
|---|---|---|---|
| L0 | `allocator` | `allocator.c` | 10 |
| L0 | `watermark` | `checkpoint.c` | 6 |
| L0 | `gc_wl` | `gc.c` / `wear_leveling.c` | 10 |
| L0 | `rr` | `read_recovery.c` | 6 |
| L0 | `bbt` | `bbt.c` | 4 |
| L0 | `metadata` | `metadata.c` | 7 |
| L0 | `pending_ct` | `pending.c` | 4 |
| L0 | `group` | `pending.c` | 6 |
| **已实施小计** | **8 L0 套件** | — | **53** |

**计划（待 FTL 层改动落定，见 §5.6/§8）**：

| 层 | 套件 | 被测模块 | 用例数 |
|---|---|---|---|
| L0 | `async`（§4.9） | op 框架 / journal / bbt / wear / migrate / checkpoint+md / l2p_cache / rr | 22（含 1 可选） |
| L0+真NAND | `ftl-hil`（§4.10） | 全链：async pipeline / 背压 / 组结算 / 读写往返 / journal 屏障 | 5 |
| **计划合计** | **2 套件** | — | **27** |

## 关键文件

| 文件 | 动作 |
|---|---|
| `docs/design/ftl_ut_munit.md`（本文档） | 设计（FTL 层 §4 逐条用例 + §5 技法 + §10 覆盖矩阵 + §5.4 异步计划） |
| `tests/unit/test_ftl.c` | 实施：单文件（fixture 复用活实例 + 8 套件数组，L0）——**用户自建** |
| `tests/unit/ftl_test_common.c/.h` | 实施（**已编译锚定**）：共享活实例夹具 + `ftl_async_fixture_reset()` + `ut_fake_complete` 伪完成 seam（§3/§5.4） |
| `tests/unit/test_ftl_async.c` | **计划**：L0 异步链套件 `/ftl/async/`（§4.9）——待 FTL 层改动落定 |
| `tests/unit/test_ftl_hil.c` | **计划**：HIL 全链套件 `/ftl-hil/`（§4.10，`UT_HIL_ENABLE` 仅 ALL） |
| `src/ftl/makefile.mk` | 实施：`FC_NAND_TEST_ENABLE` + `UT_FTL_ENABLE` 块（`test_ftl.c` + `ftl_test_common.c` + INCS）；计划：`UT_HIL_ENABLE` 块 |
| `src/fc/makefile.mk` | 实施：拆「框架+入口（恒编）」与「FC 三级（`UT_FC_ENABLE` 门控）」 |
| `makefile` §1.5 / `config.mk` | 实施：`UT` 解析 + 对象目录按测试配置分置 |
| `tests/unit/ut_main.c` | 实施：两套 extern + 根套件项各包 `#if defined(UT_*_ENABLE)` |
| `docs/design/hil_ut_design.md` | HIL 层 UT + 系统集成验收（非 FTL 部分） |

## 登记（本轮不做）

- **mock `fc_submit` 桩 / Mock NAND / 掉电注入器**：**用户定调不做**（见 §9.2）。
- `l2p_cache_note_access` 结算步导出为公开原语（否则 §4 测不到 thrashing）——属实施接线决策。
- host（x86）编译链：本仓只支持 `arm-none-eabi` 上板。
- **异步套件实现（`test_ftl_async.c` / `test_ftl_hil.c`）本轮不写**：FTL 层还可能改（§5.6
  候选 1/2 未定），计划先定案、seam 已编译锚定，实现等 FTL 侧落定再推进——见 §8。

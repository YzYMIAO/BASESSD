# FTL 层测试用例设计（上板路线：L0 munit 套件 + L2 真 NAND CLI）

> **状态：设计文档（供审查）。实施（测试文件 / makefile 接线）在审查通过后进行。**
>
> **2026-10-13 更新**：
> - 本文从"覆盖清单"深化为**逐条用例表**（§4），并**新增 §9 上板验收矩阵**、**§10 覆盖矩阵**。
> - **路线定调：不做 mock**。全部用板子测——① L0 纯 RAM munit 套件（编进 `debug.bin`，板上
>   `test` 命令跑）；② L2 真 NAND + UART CLI（`wr/rd/tr/fl/info`）+ **物理断电重启**。
>   原 §8.2/§8.3/§8.6 的 "mock `fc_submit` / Mock NAND / 掉电注入器" 二期设计**降级为路线说明**（不做）。
> - §2 SRAM 预算已按 `ftl_block_t` **20B**（v0.56 删死字段 24B→20B，`bst.h:95` static_assert）重算。
>
> **2026-10-04 事实**（仍有效）：`_sbrk` 已有实现（`src/platform/stm32h743/syscalls.c`，带实时 MSP
> 上界守卫 + 1KB 保留），`_Min_Heap_Size` 仍是 512B；`test_rebuild.c` 已从板端构建摘除
> （`src/fc/makefile.mk` 注释 + `ut_main.c` 摘除），§3「顺带修复」与 §7「malloc→静态 fixture」暂缓。

## 1. 背景与目标

FC-NAND 已有三级 munit UT（`tests/unit/test_fc_level1/2/3.c`，经 `make tests` 上板 + UART `test`
命令跑套件），但 **FTL 层没有自己的 UT**：`tests/unit/test_gc.c`/`test_l2p.c`/`test_nand.c`
各只有 1 行头部注释（孤儿占位，无 makefile 编入）；`src/ftl/makefile.mk` 无测试块。

A/B/C 档（v0.44~v0.47）+ v0.49~v0.58 落地了大量 FTL 纯逻辑（Free Pool 三级水位、journal 水位公式、
GC/WL victim 选择、read disturb 慢死预测、坏块替换池、L2P thrashing、**组完成语义**），目前**全部依赖
上板验证**——缺一层纯 RAM 单元测试把纯逻辑钉住，编译期 + 上板前能回归。

**最近的教训**：v0.58 上板暴露 **L2P 映射不跨重启持久化**（`wr 0 1` → 断电 → `rd 0 1 c` FAIL，
`anchor_seq=0` / `journal_entries=0`），根因未闭合。这类"跨重启一致性"缺陷**正是 FTL 测试该钉住的**
（见 §9.2），却因为没有 FTL 测试而只能靠人工上板发现。

**目标**：
- 复用 FC-NAND 的 munit 架构（`MunitTest` 数组 + `ut_main.c` 根套件 + `make tests` + UART `test`
  命令），零改动 `tests/framework`
- **L0 只测纯 RAM 函数**（不碰 NAND、不需要 mock），上板稳、不依赖真 NAND 时序
- **L2 走真 NAND + CLI**，覆盖 NAND 路径（GC/WL 搬移、journal flush、checkpoint、BBT 持久化、
  boot recovery、**跨重启持久化**）

## 2. 关键架构约束（决定 fixture 形态）

| # | 发现（文件行号） | 结论 |
|---|---|---|
| 1 | munit 无测试宏，`MunitTest` 结构体数组（`tests/framework/munit.h`）+ `MunitSuite` 根套件（`ut_main.c:28-46`）逐项注册 | 新增 FTL 套件 = `extern MunitTest g_ftl_*_tests[]` + 根套件一行 |
| 2 | **板上堆仅 512B**：`linker/stm32h743xx_flash.ld:19` `_Min_Heap_Size=0x200` | 新 FTL 测试一律**静态全局 fixture + setup 里 memset 复位**（仿 main.c 布局：blocks 落 SRAM1、池落 SRAM2）。**禁止 malloc** |
| 3 | `ftl_block_t` = **20B**（`bst.h:95` `_Static_assert(sizeof==20)`）；`valid_map` 每块 `PLANE_NUM×u64`=**16B**；`super_block_t`=**12B**（`allocator.h:73-80`） | 见下预算表 |
| 4 | `allocator_select_plane_block_from_pool` 只扫 **Free Pool 数组**（`allocator.c:125`），不是全 4096 遍历 | 2112 项 fixtures 足够覆盖选块；`allocator_alloc` 的 `BLOCKS_PER_PLANE` 遍历按 20B 亦可容纳（见预算） |
| 5 | SRAM 预算（tests build 增量，按 20B 重算） | `blocks[2112]`=**41.25KB** + `valid_map[2112][2]`=**33KB** + `closed_sb_list[1024]`=**12KB** + 池（128+128+64）×2B=0.64KB ≈ **87KB**。SRAM1(128KB) + SRAM2 可容；若上 4096 项 ≈ **157KB**，跨 SRAM1+SRAM2 仍可容（实施时按 region 布局确认） |

> 结论：**fixtures 用 2112 项（2048+64）**——测试块号 0..63（双 plane 覆盖 gblock 0..2111）。
> 按 20B 重算后 4096 项也进得了预算，`allocator_alloc` 是否纳入留待实施时定（见 §4.1 备注）。

## 3. 单文件测试组织（仿 fc_nand 一文件多套件）

**1 个文件** `tests/unit/test_ftl.c`（仿 `test_fc_level1.c` 一文件多套件风格），fixture **内联**在文件内
（数组只定义一次，无独立 fixture 文件）：

```c
/* 文件内静态全局（.sram1_data / .sram2_data NOLOAD，setup 统一 ftl_fixture_reset() memset 复位） */
static ftl_block_t      g_ftl_blocks[BLOCKS_PER_PLANE + 64];  /* 2112 × 20B = 41.25KB SRAM1 */
static uint64_t         g_ftl_valid[BLOCKS_PER_PLANE + 64][PLANE_NUM]; /* 2112 × 16B = 33KB SRAM1 */
static uint16_t         g_ftl_free[128];                        /* SRAM2 */
static uint16_t         g_ftl_op[128];
static uint16_t         g_ftl_spare[64];
static super_block_t    g_ftl_closed[SUPER_BLOCK_LIST_MAX];     /* 1024 × 12B = 12KB SRAM2 */
static ftl_allocator_t      g_ftl_alloc;
static metadata_dir_cache_t g_ftl_md_dir;

static void ftl_fixture_reset(void);  /* memset + allocator_init(params topo={0,0,0}+指针+counts) + metadata_dir_init */
```

文件内按 munit 风格分节，**导出 8 个套件数组**（非 static，供 ut_main extern）：
`g_ftl_allocator_tests[]` / `g_ftl_watermark_tests[]` / `g_ftl_gc_wl_tests[]` / `g_ftl_rr_tests[]` /
`g_ftl_bbt_tests[]` / `g_ftl_metadata_tests[]` / `g_ftl_pending_ct_tests[]` / **`g_ftl_group_tests[]`**。
setup 统一 `ftl_fixture_reset()` 返回 NULL fixture，teardown NULL，`SINGLE_ITERATION`。

## 4. 用例清单（逐条）

> 表式：**用例名 | 前置 | 动作 | 断言**。断言优先用**不变量/边界**（判据 7 参数化 vs 迭代、
> 判据 8 负例钉边界，见 `assertion_guidelines.md` §二十三）。**全部为纯 RAM，板上 `test` 跑，无 mock。**

### 4.1 `g_ftl_allocator_tests[]`（`src/ftl/allocator.c`）

| 用例 | 断言 |
|---|---|
| `init_pool_full` | `allocator_init` 后 `free_count` == 非坏块总数；池内每块 state==FREE |
| `free_block_clears_state` | 对 OPEN/CLOSED/RESERVED/TEMP 块调 `allocator_free_block` → state==FREE + 入池 + 计数+1 |
| `close_open_sb` | `allocator_close_open_sb`：新 closed SB 两 plane 块 state==CLOSED + `sb_id` 正确 + `closed_sb_count`+1 |
| `add_closed_sb` | `allocator_add_closed_sb` → count+1、两块 CLOSED + sb_id |
| `remove_closed_sb_swap_remove` | 删中间项：末项搬到该位**且 `sb_id` 修正**、count-1（swap-remove 索引坑） |
| `remove_from_free` | `allocator_remove_from_free` → count-1、块不在池 |
| `reserved_alloc_free` | `allocator_alloc_reserved_block` 置 RESERVED；`allocator_free_reserved_block` 归还池 |
| **`open_sb_never_picks_metadata`** | **回归用例**：metadata 块 0..62 被 `allocator_init` 标 FREE 但**不在 Free Pool** ⇒ 新建 SB 两 plane 块都 **≥63**（钉住本轮真缺陷） |
| `open_sb_min_erase_count` | 池内多块时选该 plane `erase_count` 最小者（动态 WL） |

> ⚠ **L0 只能调公开函数**：`allocator_open_sb`（`allocator.c:168`）、
> `allocator_select_plane_block_from_pool`（`:125`）、`allocator_pull_from_op`（`:96`）都是 `static`
> ⇒ 上两条经**公开入口 `allocator_close_open_sb`（`:318`，内部 `:357` 调 `open_sb`）间接测**：
> 调用后断言新 closed SB 的 `plane_blocks[p] ≥ 63` / `erase_count` 最小。
> `allocator_alloc`/`allocator_alloc_pair` 是否纳入第一版：按 20B 预算可行（§2 行 5），留待实施定。

### 4.2 `g_ftl_watermark_tests[]`（`src/ftl/checkpoint.c`）

| 用例 | 断言 |
|---|---|
| `journal_wm_below_85` | 使用率 <85% → 不触发（边界下 1 条 entry） |
| `journal_wm_at_85` | 恰 85% → 置 `checkpoint_needed`（边界上 1 条） |
| `journal_wm_at_95_emergency` | ≥95% → 置 `emergency`（`JOURNAL_CRITICAL`） |
| `emergency_set_reset` | ≥95% 置位；回落清位 |
| `checkpoint_trigger_T1_T2_T3` | `checkpoint_check_trigger` 三种条件各自命中（参数化，判据 7） |
| `checkpoint_is_emergency` | `checkpoint_is_emergency` 查询位与 watermark 一致 |

### 4.3 `g_ftl_gc_wl_tests[]`（`src/ftl/gc.c` + `src/ftl/wear_leveling.c`）

| 用例 | 断言 |
|---|---|
| `gc_victim_lowest_vpc` | `gc_select_victim` 选 VPC 最低 closed SB |
| `gc_victim_skips_open` | 跳过 `valid!=0` 的 open SB（负例，判据 8） |
| `gc_trigger_T1..T5` | `gc_check_triggers` 五位（水位/写数/定时/紧急/磨损）各自置位（参数化） |
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
| `dir_init_entries` | `metadata_dir_init` 后 `count==2048`、全 UNKNOWN |
| `dir_update_lookup_roundtrip` | `metadata_dir_update` 写一个 region → `metadata_dir_lookup` 回同 entry |
| `dir_lookup_missing_unknown` | 未写 region → `0xFFFFFFFF`（负例） |
| `is_metadata_block` | `ftl_is_metadata_block`：**0..62 true、63 false**（边界，判据 8） |
| `ring_addr_geometry` | `metadata_ring_addr` 纯地址换算：idx∈{0, rpb-1, rpb, 末} → (block,page,plane) 期望值 |
| `ring_addr_wrap` | 跨 block 边界换算 |

### 4.7 `g_ftl_pending_ct_tests[]`（`src/ftl/pending.c`）

| 用例 | 断言 |
|---|---|
| `pending_add_lookup_remove` | 增删查一致（按 `cmd_id`） |
| `pending_full_32_rejects` | 满 `FTL_PENDING_MAX(32)` 槽拒绝（负例） |
| `pending_count` | 计数与槽位一致 |
| `ct_add_remove_lookup_count` | `completion_table_*` 同族 |

### 4.8 `g_ftl_group_tests[]`（新，v0.58 组完成语义——**纯 RAM，板上可直测**）

| 用例 | 断言 |
|---|---|
| `group_alloc_free` | `pending_group_alloc/free`；`FTL_GROUP_NONE` 哨兵；`pending_group_count` 一致 |
| `group_maybe_complete_gating` | `next<total` 或 `outstanding!=0` 时**不**完成（负例） |
| `group_agg_status_sticky` | 任一子操作 IO_ERROR ⇒ 组聚合状态**粘性**为 IO_ERROR |
| `group_complete_once` | `next==total && outstanding==0` 时 `complete_flag`/`status_out`/`on_complete` **各触发一次** |
| `group_reentrant_callback` | 回调里再 `pending_group_alloc` 不踩已释放槽（`used=0` 后置，`pending.c:170`） |

> L0 **不纳入**：`allocator_alloc`（可选，见 §4.1）、`metadata_dir_append_slot`（需真 NAND 擦写）；
> `l2p_cache_note_access` 结算步（是 `static`，要测须先导出——属实施接线决策）。

## 5. 接线（实施阶段）

**`src/ftl/makefile.mk`**（现在只有 `MODULE_SRCS` 两行）追加（**一行测试源**，fixture 内联无需单独文件）：

```makefile
ifeq ($(FC_NAND_TEST_ENABLE),1)
# FTL 纯 RAM 单元测试（munit 框架由 fc/makefile.mk 的块编入）
MODULE_INCS += $(PROJECT_ROOT)/tests/framework
MODULE_INCS += $(PROJECT_ROOT)/tests/unit
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_ftl.c
endif
```

**`tests/unit/ut_main.c`**：根套件名从 `/fc-nand/` 起，建议加一个 **`/ftl/` 根**（`ut_run_all` 里把
`g_fc_nand_suite` 与新的 `g_ftl_suite` 并列），8 个子套件前缀统一 `xxx/`（**必须以 `/` 结尾**，
`ut_main.c:24-27` 注释：munit 只做首尾相接、不补分隔符）。

## 6. 验证

1. `make clean && make tests`（`FC_NAND_TEST_ENABLE=1`）**零错误零新增警告**——`.sram1_data`/`.sram2_data`
   是 NOLOAD，编译期无感知，size 脚本确认 SRAM1 ≤128KB、SRAM2 ≤128KB
2. `make debug / release` 不受影响（`FC_NAND_TEST_ENABLE` 默认关，测试文件不编入）
3. 上板：
   - `test --list` → 确认 8 个 `/ftl/*/` 套件全部列出
   - `test /ftl/`（或逐个）→ 全 PASS（尤其 `open_sb_never_picks_metadata`）
   - 随后执行 **§9 上板验收矩阵**（尤其 §9.2 跨重启）

## 7. 不做（第一版边界）

- **不做 mock**：不建 `fc_submit` 桩 / Mock NAND / 掉电注入器（用户定调，见 §8.2）
- 不改 `tests/framework`（munit 适配已完成）；不建 host（x86）target
- 不动孤儿 `test_gc.c`/`test_l2p.c`/`test_nand.c` 占位（用户自建域，本次新建独立测试文件）
- 不扩 linker 加 region

## 8. 测试分层与路线

### 8.1 行业分层（L0→L3）

| 层 | 运行环境 | 测什么 | 核心替身 | 本项目 |
|---|---|---|---|---|
| **L0 单元测试** | 板上 RAM（纯函数） | 纯算法：allocator / L2P / GC-WL victim / 水位 / 坏块 / 磨损 / 组语义 | 无 | **本文档 §4** |
| **L1 固件集成** | 板上 + **Mock NAND** | write / GC / 掉电恢复全生命周期 | Mock NAND + 掉电注入器 | **不做**（见 §8.2） |
| **L2 系统** | 真 NAND 上板 | 压力 / 写后读 / **跨重启** | 真硬件（UART CLI `wr/rd/tr/fl`） | **本文档 §9** |
| **L3 HIL** | host 协议模型 | 协议级命令流（NVMe/UFS） | 可编程负载模型 | 不适用（本项目无 NVMe） |

### 8.2 本项目路线：**不做 mock，全用板子**（替代原"二期 Mock NAND"）

行业 FTL UT 的主体是 **L0 + L1（Mock NAND）**，但**本项目刻意不走 L1**，理由：
- Mock NAND 的唯一拦截点是 `fc_submit`（`include/fc/fc_interface.h:95`，FTL 侧调用点
  `ftl.c` / `gc.c` / `read_recovery.c` / `ftl_fc_interface.c`）。**桩不存在，且用户定调不建**。
- 纯逻辑（水位 / victim / 磨损 / 组语义）本就能在 **L0** 用静态 fixture 钉死，不需要 NAND 替身。
- 需要真 NAND 的路径（GC 搬移 / journal replay / boot recovery / **跨重启持久化**）直接在**真板上**
  跑——尤其跨重启**只能用物理断电**（没有掉电注入器），反而更接近真实失效场景。

### 8.3 为什么"跨重启"不用 mock 也能测

"重启" = **物理断电再上电**。板上执行：擦净 → 写 → 断电 → 上电 → 读回校验。断言 = `rd … c`
的 PASS/FAIL + 开机日志 `[FTL] boot type` / `[META] anchor_seq` / `journal_entries`。
**这比 mock 注入更真**，代价是每次要人工断电（用例数控制在 ~7 条，见 §9.2）。

### 8.4 断言套路（行业经典，抄入用例）

| 模式 | 例子 |
|---|---|
| 不变量 | `free + open + closed + bad == 总块数`（P1-3 后 `free_count` 是唯一池计数） |
| 状态机全路径 | allocator 块 FREE→OPEN→CLOSED→FREE 每步断言 state/sb_id |
| 水位边界 | 85%/95% 阈值上下 1 条 entry 处断言触发/不触发（v0.46 B1 就是水位公式 bug） |
| round-trip | 写 pattern → 读回 → 校验（本项目 `wr/rd c` 同构） |
| 覆盖写回滚 | 旧页失效 + L2P 指新页 + 断电后回滚到最新胜 |
| GC/WL victim 正确性 | 选到 VPC 最低 / read_count 最高的 closed SB |
| 掉电一致性 | 确认写不丢 / GC 擦除前搬移已落盘 |

### 8.5 范围评估

- **要做**：`test_ftl.c`（§4，L0 纯 RAM，板上 `test`）+ §9 上板验收矩阵（CLI + 物理断电）
- **不做**：`fc_submit` 桩 / Mock NAND / 掉电注入器 / host（x86）编译链
- **L3（HIL）**：本项目无 NVMe host，天然不适用（UART CLI 即 HIL 面）

## 9. L2 上板验收用例矩阵（真 NAND + UART CLI，含物理断电重启）

> 前置纪律：**擦净芯片**（`bbt --erase` → **立刻断电**，见 P0-2：运行中的 FTL 空闲会重写元数据）；
> 每条用例前记录开机 `[FTL] boot type`。断言以 `rd … c` 的 PASS/FAIL 与开机日志为准。

### 9.1 基础读写（对外接口行为）

| 用例 | 命令 | 断言 |
|---|---|---|
| `cli_rw_1` | `wr 0 1` → `rd 0 1 c` | PASS |
| `cli_rw_2`（multi-plane） | `wr 0 2` → `rd 0 2 c` | PASS；`log level debug` 见 **1 条** multi-plane `op=16` |
| `cli_rw_5`（自拆分 2+2+1） | `wr 0 5` → `rd 0 5 c` | PASS；debug 见 3 条命令（末条单 plane） |
| `cli_rw_8`（CLI 上限） | `wr 0 8` → `rd 0 8 c` | PASS（`g_wr_buf=16KB` ⇒ `n≤8`） |
| `cli_large_rejected` | `wr 0 40` | 被容量校验**拒**（`INVALID_ARGUMENT`），不写爆 16KB 缓冲 |
| `cli_trim` | `wr 0 4` → `tr 0` → `rd 0 1 c` | 全 0 / 未映射语义 |
| `cli_flush` | `wr 0 2` → `fl` | 无错误；L2P/journal 落盘 |
| `cli_info` | `info` | `free/op/spare` 与几何相符 |

### 9.2 跨重启一致性（**物理断电**，本次重点）

| 用例 | 步骤 | 断言 |
|---|---|---|
| **`reboot_persist_single`** | 擦净 → `wr 0 1` → **断电** → 上电 → `rd 0 1 c` | **PASS**（当前 FAIL 的最小复现——用例先红） |
| `reboot_persist_multi` | `wr 0 8` → 断电 → 上电 → `rd 0 8 c` | PASS |
| `reboot_after_flush` | `wr 0 2` → `fl` → 断电 → 上电 → `rd 0 2 c` | PASS |
| `reboot_no_flush` | `wr 0 1` → **不 `fl`** → 断电 → 上电 → `rd 0 1 c` | **记录本仓持久化契约**（该丢还是该留）——正是当前 bug 所在 |
| `overwrite_then_reboot` | `wr 0 1`(A) → `wr 0 1`(B) → 断电 → 上电 → `rd 0 1 c` | 读回 **B**（最新胜） |
| `reboot_after_trim` | `wr 0 4` → `tr 0` → 断电 → 上电 → `rd 0 1 c` | 仍全 0 |
| `boot_type_classify` | 三种镜像（空 / 干净关机 / 写中掉电）分别启动 | `[FTL] boot type` = FRESH / CLEAN / POWER_FAILURE |

> **落盘触发语义必须先定死**（否则 `reboot_no_flush` 无判据）：`ftl_submit_write` 返回 `FTL_OK` 后，
> 映射**何时**必须持久化？设计里写明本仓契约（flush 时机 / journal 追加时机），并让用例断言它
> ——这正是当前 bug 的根因面（`anchor`/`journal` 从不被触发）。

### 9.3 后台 / 寿命观测（上板压力）

| 用例 | 步骤 | 断言 |
|---|---|---|
| `gc_watermark_observe` | 连写直至 `free` 逼近 `FREE_POOL_TARGET(32)` | 观察 GC 触发（`info` free 回升） |
| `journal_watermark_observe` | 连写触发 journal 85%/95% | 观察水位告警 / emergency（§4.2 的板上印证） |
| `wear_persist_observe` | 重启后 `[WEAR] seq/pages` | 与上次写次数相符 |

## 10. 覆盖矩阵与用例总数

| 层 | 套件/分组 | 被测模块 | 用例数 | 引用判据 |
|---|---|---|---|---|
| L0 | `allocator` | `allocator.c` | 9 | 判据 8 |
| L0 | `watermark` | `checkpoint.c` | 6 | 判据 7、8 |
| L0 | `gc_wl` | `gc.c` / `wear_leveling.c` | 10 | 判据 7、8 |
| L0 | `rr` | `read_recovery.c` | 6 | 判据 8 |
| L0 | `bbt` | `bbt.c` | 4 | 判据 8 |
| L0 | `metadata` | `metadata.c` | 6 | 判据 8 |
| L0 | `pending_ct` | `pending.c` | 4 | 判据 8 |
| L0 | `group` | `pending.c` | 5 | 判据 8 |
| L2 | 9.1 对外接口 | HIL/CLI + FTL | 8 | — |
| L2 | 9.2 跨重启 | boot recovery / journal / anchor | 7 | — |
| L2 | 9.3 后台观测 | gc / journal / wear | 3 | — |
| **合计** | **8 L0 套件 + 3 L2 组** | — | **~68**（L0 50 + L2 18） | — |

## 关键文件

| 文件 | 动作 |
|---|---|
| `docs/design/ftl_ut_munit.md`（本文档） | 设计（§4 逐条用例 + §9 上板矩阵 + §10 覆盖矩阵，审查后进入实施） |
| `tests/unit/test_ftl.c` | 实施：单文件（fixture 内联 + 8 套件数组，L0）——**用户自建** |
| `src/ftl/makefile.mk` | 实施：`FC_NAND_TEST_ENABLE` 块（一行 TEST_C_SRCS + INCS） |
| `tests/unit/ut_main.c` | 实施：8 个 extern + `/ftl/*` 子套件注册 |
| `docs/design/ftl_host_interface.md` | Part C 指向本文档 §8.6/§9（防两处漂移） |

## 登记（本轮不做）

- **mock `fc_submit` 桩 / Mock NAND / 掉电注入器**：**用户定调不做**——全部改用板子测
  （L0 munit 套件 + L2 真 NAND CLI + 物理断电）。
- `l2p_cache_note_access` 结算步导出为公开原语（否则 §4 测不到 thrashing）——属实施接线决策。
- host（x86）编译链：本仓只支持 `arm-none-eabi` 上板。

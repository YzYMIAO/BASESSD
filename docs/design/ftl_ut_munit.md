# FTL 层 munit 单元测试设计方案（纯 RAM 层，单文件组织）

> **状态：设计文档（供审查）。实施（测试文件/makefile 接线）在审查通过后进行。**
>
> **2026-10-04 事实更新（本文正文数字尚未按此重算，实施前需重新核算 SRAM 预算）**：
> - `_sbrk` 已有实现：`src/platform/stm32h743/syscalls.c`（带实时 MSP 上界守卫 + 1KB 保留），§2 表中「全树无 `_sbrk` 实现」已过时；`_Min_Heap_Size` 仍是 512B。见 `debug_sbrk_heap_stack_20261004.md`。
> - `test_rebuild.c` **已从板端构建摘除**（`src/fc/makefile.mk` + `tests/unit/ut_main.c`），§3「顺带修复」与 §7 中「实施：malloc→静态 fixture」两项暂缓。见 `fault_capture.md` §7。
> - `ftl_block_t` **56B → 40B**（v0.48，`d8fe6d1`）：§2 表的 224KB / AXI 377KB 等数字按旧尺寸算的，需按 40B 重算。

## 1. 背景与目标

FC-NAND 已有三级 munit UT（`tests/unit/test_fc_level1/2/3.c` + `test_rebuild.c`，经 `make tests` 上板 + UART `test` 命令跑套件），但 **FTL 层没有自己的 UT**：`tests/unit/test_gc.c`/`test_l2p.c`/`test_nand.c` 各只有 1 行头部注释（孤儿占位，无 makefile 编入）；`src/ftl/makefile.mk` 无测试块。

A/B/C 档（v0.44~v0.47）落地了大量 FTL 纯逻辑（Free Pool 三级水位、journal 水位公式、GC/WL victim 选择、read disturb 慢死预测、坏块替换池；
原文所列的 `PRE_COND` 状态位已于 v0.52 删除，见 `ftl_revision_history.md`），目前**全部依赖上板验证**——缺一层纯 RAM 单元测试把纯逻辑钉住，编译期 + 上板前能回归。

**目标**：
- 复用 FC-NAND 的 munit 架构（`MunitTest` 数组 + `ut_main.c` 根套件 + `make tests` + UART `test` 命令），零改动 tests/framework
- 第一版只测**纯 RAM 函数**（不碰 NAND），上板稳、不依赖真 NAND 时序
- NAND 路径（GC/WL 搬移、journal flush、checkpoint、BBT 持久化、boot recovery）由既有 UART CLI `wr/rd/tr/fl` + FC Level 3 上板覆盖，不在本 UT 范围

## 2. 关键架构约束（决定 fixture 形态）

| # | 发现（文件行号） | 结论 |
|---|---|---|
| 1 | munit 无测试宏，`MunitTest` 结构体数组（`tests/framework/munit.h:425-433`）+ `MunitSuite` 根套件（`ut_main.c:29-52`）逐项注册 | 新增 FTL 套件 = `extern MunitTest g_ftl_*_tests[]` + 根套件一行 |
| 2 | **板上堆仅 512B**：`linker/stm32h743xx_flash.ld:19` `_Min_Heap_Size=0x200`；全树无 `_sbrk` 实现（grep 零命中，`config.mk:204` `-specs=nosys.specs`，libnosys 默认 sbrk 无上限从 `_end` 起） | **现有 `test_rebuild.c` 的 `malloc(sizeof(rebuild_fixture_t))≈250KB` 在板上必失败**（返回 NULL → 空指针解引用崩）。新 FTL 测试一律**静态全局 fixture + setup 里 memset 复位**（仿 main.c 布局：blocks 落 SRAM1、池落 SRAM2） |
| 3 | `ftl_block_t` 56B × `FTL_BLOCK_TOTAL`4096 = **224KB 单数组**；AXI_RAM 剩 ~7KB、SRAM2 剩 84KB、SRAM1 空 128KB | 单 region 放不下 4096 项。**用 2112 项（2048+64）blocks = 118KB 落 SRAM1**，测试 block 号 0..63（双 plane 覆盖 gblock 0..2111，`allocator_select_block` 安全下限 `BLOCKS_PER_PLANE+max_b`，allocator.c:129-131） |
| 4 | `allocator_select_block`/`allocator_alloc` 全遍历 `b<2048`（allocator.c:129-131）→ 需 4096 项 | **第一版不测这两个**（上板 UART `wr` 命令已覆盖 allocator_alloc 全流程） |
| 5 | SRAM 预算（tests build 增量） | SRAM1 +118KB/128KB（原本 0）、SRAM2 +~17KB（44→61KB）、AXI 不变（377KB/384KB）。不溢出 |

## 3. 单文件测试组织（仿 fc_nand 一文件多套件）

**1 个文件** `tests/unit/test_ftl.c`（仿 `test_fc_level1.c` 一文件多套件风格），fixture **内联**在文件内（数组只定义一次，无独立 fixture 文件）：

```c
/* 文件内静态全局（.sram1_data / .sram2_data NOLOAD，setup 统一 ftl_fixture_reset() memset 复位） */
static ftl_block_t      g_ftl_blocks[BLOCKS_PER_PLANE + 64];   /* 2112 × 56B = 118KB SRAM1 */
static uint16_t         g_ftl_free[128];                        /* SRAM2 */
static uint16_t         g_ftl_op[128];
static uint16_t         g_ftl_spare[64];
static super_block_t    g_ftl_closed[SUPER_BLOCK_LIST_MAX];     /* 12KB SRAM2 */
static ftl_allocator_t      g_ftl_alloc;
static metadata_dir_cache_t g_ftl_md_dir;

static void ftl_fixture_reset(void);  /* memset + allocator_init(params topo={0,0,0}+指针+counts) + metadata_dir_init */
```

文件内按 munit 风格分节，**导出 7 个套件数组**（非 static，供 ut_main extern）：`g_ftl_allocator_tests[]` / `g_ftl_watermark_tests[]` / `g_ftl_gc_wl_tests[]` / `g_ftl_rr_tests[]` / `g_ftl_bbt_tests[]` / `g_ftl_metadata_tests[]` / `g_ftl_pending_ct_tests[]`。setup 统一 `ftl_fixture_reset()` 返回 NULL fixture，teardown NULL，`SINGLE_ITERATION`。

**顺带修复（建议，纳入实施）**：`test_rebuild.c` 的 `rebuild_setup` 改静态全局 fixture——修板上必崩 bug（malloc 250KB 必失败，堆仅 512B），`test /fc-nand/rebuild/` 上板才能跑。独立于 test_ftl.c 不动其组织。

## 4. 覆盖清单（7 个套件数组，纯 RAM 函数；全部内联在 test_ftl.c 一节）

| 套件数组 | 覆盖（纯 RAM 函数 + 文件行号） |
|---|---|
| `g_ftl_allocator_tests[]` | `allocator_init`、`allocator_free_block`（置 FREE + 清 RESERVED/OPEN/CLOSED/TEMP）、`allocator_close_open_sb`、`allocator_add_closed_sb`（CLOSED+sb_id）、`allocator_remove_closed_sb`（swap-remove sb_id 修正）、`allocator_remove_from_free`、`allocator_alloc_reserved_block`/`allocator_free_reserved_block` |
| `g_ftl_watermark_tests[]` | `checkpoint_journal_watermark`(393：4 块 256 页 85%/95% + emergency 置位 + 复位)、`checkpoint_check_trigger`(352：T1/T2/T3)、`checkpoint_is_emergency`(383) |
| `g_ftl_gc_wl_tests[]` | `gc_select_victim`(318)、`gc_check_trigger`(474)、`wl_should_trigger`(71)、`wl_select_victim`(117：有效页+read_count 权重)、`wl_max/min/avg_erase`、`wl_count_blocks_below` |
| `g_ftl_rr_tests[]` | `rr_record_ecc`(69：8 深度环形+rising)、`rr_check_predictive`(345)、`rr_get_health`(368)、`rr_predictive_scan`(378：READ_DISTURB_THRESHOLD)、`rr_retire_block`(410) |
| `g_ftl_bbt_tests[]` | `bbt_mark_bad`(356：BAD 位+clear_valid+bad_count)、`bbt_spare_pool_refill`(518：spare→free) |
| `g_ftl_metadata_tests[]` | `metadata_dir_init`、`metadata_dir_update`、`metadata_dir_lookup`、`ftl_is_metadata_block`、`metadata_ring_addr`（**纯地址换算，可纯 RAM 测**；⚠ v0.50 起原 `metadata_dir_anchor_paa` / `metadata_dir_alloc_region` 已删除） |
| `g_ftl_pending_ct_tests[]` | `pending_add/lookup/lookup_by_lba/remove/count`（32 槽满拒绝）+ `completion_table_init/add/remove/lookup/count` |

**不纳入第一版**：`allocator_alloc`、`metadata_dir_append_slot`（需全 4096 遍历 + 真 NAND 擦写）；
**P1-4 thrashing 窗口**（`l2p_cache_note_access` 的结算+滞回是**纯逻辑**，但当前是 `l2p_cache.c` 的 `static` 内部函数，L0 要测需先把结算步导出成公开原语——属测试接线决策，实施时再定；若走 L1 Mock NAND，则随 read/write 路径自然覆盖）；所有 NAND 路径（`l2p_cache` 未命中加载/flush、journal flush/replay、checkpoint_create、gc_migrate、bbt_flush/load、wl_relocate、rr_recover_read、boot recovery、`metadata_ring_locate` 的快/慢路径）——由 UART CLI + FC Level 3 上板覆盖。

## 5. 接线（实施阶段）

**`src/ftl/makefile.mk`**（现在只有 MODULE_SRCS 两行）追加（**一行测试源**，fixture 内联无需单独文件）：

```makefile
ifeq ($(FC_NAND_TEST_ENABLE),1)
# FTL 纯 RAM 单元测试（munit 框架由 fc/makefile.mk 的块编入）
MODULE_INCS += $(PROJECT_ROOT)/tests/framework
MODULE_INCS += $(PROJECT_ROOT)/tests/unit
TEST_C_SRCS += $(PROJECT_ROOT)/tests/unit/test_ftl.c
endif
```

**`tests/unit/ut_main.c`** 根套件加 7 个子套件（`extern MunitTest g_ftl_*_tests[];` + `{(char*)"allocator", g_ftl_allocator_tests, NULL, 1, ...}` 等），前缀统一 `/ftl/` 下子套件名。

## 6. 验证

1. `make tests`（FC_NAND_TEST_ENABLE=1）**零错误零新增警告**——`.sram1_data`/`.sram2_data` 是 NOLOAD，编译期无感知，size 脚本确认 SRAM1 ≤128KB、SRAM2 ≤128KB
2. `make debug / release` 不受影响（FC_NAND_TEST_ENABLE 默认关，测试文件不编入）
3. 上板：
   - `test --list` → 确认 `/ftl/allocator/` `/ftl/watermark/` `/ftl/gc_wl/` `/ftl/rr/` `/ftl/bbt/` `/ftl/metadata/` `/ftl/pending_ct/` 全部列出
   - `test /ftl/`（或逐个）→ 全 PASS
   - `test /fc-nand/rebuild/` → 改静态后应正常（修复前板上必崩）
   - 随后 `wr 0 2 / rd 0 2 c / tr 0 / fl` 走通 NAND 路径（既有）

## 7. 不做（第一版边界）

- 不测 NAND 路径；不改 `tests/framework`（munit 适配已完成）
- 不动孤儿 `test_gc.c`/`test_l2p.c`/`test_nand.c` 占位（用户自建域，本次新建独立测试文件）
- 不建桩/target；不扩 linker 加 region

## 8. 行业 FTL UT 分层与本项目二期方向（后续规划）

> 本项目的 UT 体系对标存储主控行业（三星/Phison/企业级）的分层测试。第一版只做 **L0**；L1（Mock NAND + 掉电注入）是二期方向，本节约定其形态，不在本轮实施。

### 8.1 行业分层（L0→L3）

| 层 | 运行环境 | 测什么 | 核心替身 |
|---|---|---|---|
| **L0 单元测试** | host 编译（x86），零硬件 | 纯算法：allocator / L2P / GC-WL victim / 水位 / 坏块 / 磨损 | 无（只测纯 RAM 函数）——**本项目第一版** |
| **L1 固件集成** | 板上 + **Mock NAND** | write / GC / 掉电恢复全生命周期 | **Mock NAND（RAM 模拟）+ 掉电注入器** |
| **L2 系统** | 真 NAND 上板 | 压力 / 耐力 / 随机流 / 写后读 | 真硬件（本项目 UART CLI `wr/rd/tr/fl`） |
| **L3 HIL** | host 协议模型 | 协议级命令流（NVMe/UFS） | 可编程负载模型（本项目无 NVMe，不适用） |

行业 FTL UT 的主体是 **L0 + L1**；真 NAND 的 L2 是最终把关，不作日常回归（慢、不可重复）。

### 8.2 行业两大核心基建（本项目二期）

**① Mock NAND（NAND 模拟器）**：RAM 数组模拟 NAND 物理行为，可注入坏块（出厂/PROGRAM/ERASE）、读 ECC 错误（可配 bit 数，测 read disturb 预测/read recovery）、编程扰动、部分写（半页写后掉电）。时序可跳过（UT 直接完成）或可配。

**② 掉电注入器（power-cut injection）**：在任意指令边界模拟掉电 → 重启走 boot recovery → 验证一致性不变量：
```
journal replay 后 L2P == 最后一次确认的写（不丢写）
GC 擦除前的搬移条目已落盘（不指已擦除块）
```
本项目 F10（journal_replay 全量回放 + GC/WL 擦除前强制 flush）正是为此设计——但没有掉电注入器就只能手动断电验证。

### 8.3 本项目 L1 落地路径（关键发现：拦截点唯一）

FTL 层 NAND 访问全部收敛到 `ftl_internal_sync_wait`（`src/ftl/ftl_fc_interface.c:57`）经 `fc_service` 泵完，`fc_command_alloc → fc_submit → nand_dispatch_poll → nand_fc_start/step → nand_fmc_*`（写 FMC Bank3 @0x80000000）。**L1 只需拦截 `fc_submit` 换成 RAM 桩**即可让 write / GC / journal / 掉电恢复全部自动可测：

```
（二期）测试用 fc_submit 桩：分配 buffer → 按 operation 模拟 NAND 行为（PROGRAM 写 page_buffer、
READ 回 page_buffer + 可配 ECC bit 注入、ERASE 清零 + 可配坏块）→ 直接推 completion 到 CQ
→ ftl_internal_sync_wait 原样收到 → FTL 层零改动。
```

掉电注入 = 在桩里记录"已确认写"点 + 测试任意处截断（丢弃未确认写）→ 重启走 `ftl_boot_recovery` → 断言 L2P 一致。

### 8.4 二期测试模式参考（行业经典断言套路，抄入 L1）

| 模式 | 例子 |
|---|---|
| 不变量 | `free + open + closed + bad == 总块数`（P1-3 后 free_count 是唯一池计数，无派生计数） |
| 状态机全路径 | allocator 块 FREE→OPEN→CLOSED→FREE 每步断言 state/sb_id |
| 水位边界 | 85%/95% 阈值上下 1 条 entry 处断言触发/不触发（v0.46 B1 就是水位公式 bug） |
| round-trip | 写 pattern → 读回 → 校验（本项目 `wr/rd c` 同构） |
| 覆盖写回滚 | 旧页失效 + L2P 指新页 + 掉电后回滚到旧页 |
| GC/WL victim 正确性 | 选到 VPC 最低 / read_count 最高的 closed SB |
| 掉电一致性 | 确认写不丢 / GC 擦除前搬移已落盘 |

### 8.5 二期范围评估（不在本轮，仅定方向）

- **要做**：`fc_submit` 桩（mock NAND 行为）+ 掉电注入点 + `test_ftl_integration.c`（write/GC/journal/掉电恢复，全 RAM）
- **依赖**：`fc_command`/`completion_table` 抽象已就位（`fc_submit` 是唯一 NAND 入口）——正是 mock 友好的结构
- **不做**：host（x86）编译链（当前构建只支持 arm-none-eabi 交叉编译上板）；Mock NAND 已可在板上 RAM 跑（桩是纯 RAM），无需 host target
- **L3（HIL）**：本项目无 NVMe host，天然不适用（D10 已定 UART CLI）

## 关键文件

| 文件 | 动作 |
|---|---|
| `docs/design/ftl_ut_munit.md`（本文档） | 设计文档（含 §8 行业分层 + 二期 Mock NAND 方向，审查后进入实施） |
| `tests/unit/test_ftl.c` | 实施：单文件（fixture 内联 + 7 套件数组，L0） |
| `src/ftl/makefile.mk` | 实施：FC_NAND_TEST_ENABLE 块（一行 TEST_C_SRCS + INCS） |
| `tests/unit/ut_main.c` | 实施：7 个 extern + `/ftl/*` 子套件注册 |
| `tests/unit/test_rebuild.c` | 实施：malloc(250KB) → 静态 fixture（板上必崩 bug） |
| `tests/unit/test_ftl_integration.c`（二期预留） | 预留：fc_submit 桩（Mock NAND）+ 掉电注入（§8.3/8.4） |

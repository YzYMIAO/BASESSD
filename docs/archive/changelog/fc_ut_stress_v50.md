# v50：压力用例 `mixed_pipeline` + FC 批量收取 API `fc_poll_completions()`

- **日期**：2026-10-11
- **范围**：**测试 + src**。`include/fc/fc_interface.h`、`src/fc/fc_interface.c`、
  `include/fc/fc_internal.h`、`src/fc/fc.c`（新增一个公开 API，**纯增量**）、
  `tests/unit/fc_test_common.{c,h}`、`tests/unit/test_fc_level3.c`、
  `docs/design/fc_nand_ut.md` + 本文档。**`src/ftl/**` 一行不动**。
- **介质格式**：无变更。
- **测试**：**用例数 54 → 55**（Level 3 **12 → 13**，其中地址随机化 **9 → 10**）。
  `make format-check` 过；`make clean && make tests` **0 warning 0 error**、
  链接 **232860 B**（text 231084 + data 1736 = 232820）。
- ✅ **上板已验**（2026-10-11）：`/fc-nand/lifecycle/mixed_pipeline` **[ OK ]**、
  完整 `test` **55 of 55 (100%)**、0 skipped。⇒ 本版新增的 `fc_poll_completions` **无回归**（见「四」）。

## 〇、起因

用户要一条**压力测试**，形状是：

> 批量提交若干 → 批量收取若干，读/写/擦**交替**下发；收取条数与 `nand_dispatch_poll`
> 的推进上限（`CH_NUM×CE_NUM×LUN_NUM`）对齐。

关键约束（用户明确）：

1. **批量条数不能由 FC 定死**——`CH×CE×LUN` 这个建议值由**上层**算好、作为参数传进来。
2. **UT 必须按多 LUN 设计**——今天 `LUN_NUM=1`，但代码要写成拓扑无关，换多 die 器件时自动变真并行。
3. **不改现有顺序用例**——压力用例作为**新条目**并入，原有 12 条不动。

盘点（决定了设计边界）：

- `fc_poll_completion` 是**唯一**的 CQ 出口，一次一条；**全仓没有**任何批量/drain-N 原语。
- `nand_dispatch_poll`（`src/nand/nand_dispatch.c:755`）三层循环 ch→ce→lun，**全遍历不早退**，
  每 LUN 每轮推进一步 ⇒ 单次调用最多交付 `CH×CE×LUN` 条完成（今天 = 1）。
- `fc_service()`（`src/fc/fc_interface.c:259`）→ `fc_poll_internal`（`src/fc/fc.c:1793`）：
  本轮**只要交付过 ≥1 条完成，就整段跳过填充 pass**；填充只在「零完成」的那次调用里跑。
- per-LUN FIFO 深 `NAND_DISPATCH_CHAIN_DEPTH = 4` ⇒ NAND 层**最多 4 条在途**；
  SQ 深 64、CQ 深 64、命令池 64、desc 池 256、buffer 池 128。
- CQ pop 即逻辑释放槽位，但**命令本身要靠 `fc_command_release` 归还**（幂等；契约：命令不得还在
  CQ 里 ⇒ **必须先 poll 再 release**）。
- CQ 满（64）是 fatal——`fc_complete_from_nand` 强制标 COMPLETED 并丢条目、返回 `IO_ERROR`。
  本测试在途 ≤ 窗口，不会撞到。

**既有先例**：`tests/integration/test_fc_nand.c:351` 的 `fc_nand_test_run_block_pipeline` 就是
「槽表 24、不轮询连发、一次 `fc_service`、再 `while (fc_poll_completion)…` 抽干」——但它**抽干到空**，
且是集成测试（用户文件，不动）。本版把它收敛成「**批量上限由调用方指定**」的原语，落到单元夹具。

## 一、决策

| 问题 | 结论 |
|---|---|
| 批量收取函数放哪层 | **新增 FC 公开 API `fc_poll_completions()`**——FC 给机制（弹到上限或空为止），`max_count` 由调用方给。直接兑现「FC 不定死这个参数」。 |
| 用例怎么落地 | **并入 `tests/unit/test_fc_level3.c` 的 `g_fc_lifecycle_tests[]`**（新条目），路径 `/fc-nand/lifecycle/mixed_pipeline`。**零注册改动**（不动 `makefile.mk`、不动 `ut_main.c`），且**直接复用同文件的 `l3_*` 随机件与 `L3_CONF`**（它们是 `static`，新建文件就得复制一份）。 |
| 拓扑无关怎么做 | 批量上限 `batch = CH_NUM*CE_NUM*LUN_NUM` 在**测试里**算；窗口 `W = NAND_DISPATCH_CHAIN_DEPTH * 2`。两处都是编译期表达式，换拓扑自动变。 |
| 交替形态 | 窗口内 op 轮转 `PROGRAM / READ / ERASE`，**三种 op 各打自己的专用块**（互不干扰，无数据冒险）；多轮提交-抽干。 |

> 备选（未采纳）：把批量收取只做成测试夹具里的 `test_drain_batch()`、不动 FC。不采纳的理由：
> 用户明确「FC 不应该定死这个参数，这个参数由上层指定」，语义上要的就是 FC 侧带参函数；
> 且 FTL 的 `completion_table_drain()`（`src/ftl/completion_table.c:143`）将来也能直接吃这个 API。

## 二、改动

### `include/fc/fc_interface.h`（A1）

`fc_poll_completion` 之后（`:102`）新增声明：

```c
/* 从 CQ 批量取完成项：最多取 max_count 条，实际条数写入 *count。
 * **上限由调用方给**（FC 不写死任何批量常量）……
 * 返回：取到 ≥1 条 = OK；一条没取到 = QUEUE_EMPTY（含 max_count==0）。 */
fc_status_t fc_poll_completions(fc_command_status_t *completions, uint16_t max_count, uint16_t *count);
```

### `src/fc/fc_interface.c`（A2）

`fc_poll_completion` 之后（`:280`）新增壳：

```c
fc_status_t fc_poll_completions(fc_command_status_t *completions, uint16_t max_count, uint16_t *count)
{
    ASSERT(completions != NULL);
    ASSERT(count != NULL);
    return fc_poll_completions_internal(&g_fc_controller, completions, max_count, count);
}
```

### `include/fc/fc_internal.h`（A3）

`fc_poll_completion_internal` 之后（`:135`）新增 internal 声明。

### `src/fc/fc.c`（A4）

第 10 节「CQ 出口」（ToC `:26`）新增实现（`:1844`）：

```c
fc_status_t fc_poll_completions_internal(fc_t *controller, fc_command_status_t *completions, uint16_t max_count, uint16_t *count)
{
    uint16_t n = 0U;
    while ((n < max_count) && (fc_completion_queue_pop(&controller->cq, &completions[n]) == FC_STATUS_OK))
    {
        n++;
    }
    *count = n;
    return (n != 0U) ? FC_STATUS_OK : FC_STATUS_QUEUE_EMPTY;
}
```

要点：

- **不新增任何常量**（尤其不写 `FC_POLL_COMPLETION_MAX`）——上限完全由调用方给，兑现「不定死」。
- 循环天然被 CQ 占用数封顶（≤ `FC_CQ_DEPTH`），故 `max_count` 给多大都不会越界；
  `max_count==0` 时 `*count=0`、返回 `QUEUE_EMPTY`，无需特判。
- 返回语义对齐单条版：有取到 = `OK`，一条没取到 = `QUEUE_EMPTY`。
- **对既有路径零影响**：新函数，`fc_poll_completion` 原样保留。

### `tests/unit/fc_test_common.{c,h}`（B1）

把静态的 `test_capture_user_paa`（`fc_test_common.c:198`）**提升为导出** `test_capture_cmd(const fc_command_t *cmd)`
（声明在 `fc_test_common.h:349`）。

**理由**：读路径收尾——FC 已在 `fc_complete_from_nand` 里跑过 `process_read` 原地解码，
此处把 desc 的 USER 区逐槽拷回 `g_source_data`。批量读要「先捕获、再校验、最后才 release」，
所以必须能在提交-抽取循环里**单独调用**它（不能藏在某个 helper 内部）。

复用的现成原语（**不新写**）：`test_cmd_build`（只建链不提交）、`test_fill_user_paa`、
`test_memset_read_buffer`、`test_verify_page_paa`、`test_plane_cw_bitmap`、`test_first_plane`、
`test_erase_block` / `test_prog_page`（Phase 0 前置用）。

### `tests/unit/test_fc_level3.c`（B2、B3）

新用例 `mixed_pipeline`（`MUNIT_TEST_OPTION_NONE`，地址由 `l3_plane_draw`/`l3_block_draw` 抽样，
**同文件直接复用，不新写随机件**）。注册为 `g_fc_lifecycle_tests[]` 新条目（`:1687`，
`block_lock_ops` 之后、终止符之前）。

规模常量（编译期）：

| 宏 | 值 | 说明 |
|---|---|---|
| `L3_STRESS_WINDOW` | `FC_SCHEDULER_CHAIN_MAX * 2 = 8` | 每轮批量提交条数；**必须 > FIFO 深 4** |
| `L3_STRESS_ROUNDS` | `4` | 提交-抽干轮数 |
| `L3_STRESS_PAGES` | `16` | `blk_read` 预写页数；读命令页号在此范围内循环 |
| `L3_STRESS_BATCH` | `CH_NUM*CE_NUM*LUN_NUM` | 单次收取上限（**测试侧算**，FC 不知道） |
| `L3_STRESS_BUDGET` | `TEST_SERVICE_LOOP_MAX * L3_STRESS_WINDOW` | 抽干预算，超了报「流水线卡住」 |

流程：

```
Phase 0  前置（顺序，用现成 helper）：
          从安全窗口抽 base 块，取 blk_read / blk_write / blk_erase（base, +1, +2 mod span）
          blk_read  : test_erase_block + 逐页 test_prog_page（供读校验）
          blk_write : test_erase_block（program 落这里）
          blk_erase : test_erase_block + 逐页 test_prog_page（让 erase 真有事可做）
Phase 1  压力轮（for r in 0..R）：
          ① 批量提交：for k in 0..W
               op = (r*W + k) % 3 ∈ {PROGRAM, READ, ERASE}
               addr = 该 op 的专用块 + 页号
               cmd  = test_cmd_build(&addr, test_plane_cw_bitmap(...), meta, &L3_CONF(op, crc))
               PROGRAM: test_fill_user_paa(cmd)；READ: test_memset_read_buffer(cmd)
               fc_submit(cmd)；槽表记 {cmd, operation, block, plane, page}（键 = fc_command_to_id）
          ② 批量抽干：while (本轮已收 < W && guard < BUDGET)
               fc_service()                                  ← 每轮最多推进 CH*CE*LUN
               n = 0; fc_poll_completions(batch_buf, batch, &n)   ← batch 由测试算，FC 不知道
               for j in 0..n:
                 slot = 按 batch_buf[j].command_id 查槽表
                 断言 batch_buf[j].status 无 FC_CW_STATUS_ERROR_MASK 位
                 READ: test_capture_cmd(slot->cmd); test_verify_page_paa(g_source_data, ...)
                 fc_command_release(slot->cmd)                ← pop 之后才 release
                 本轮已收++
             超预算未收齐 ⇒ munit_errorf 报「流水线卡住」
```

- **无数据冒险**：READ 只碰 `blk_read`（Phase 0 已写好）、PROGRAM 只碰 `blk_write`、
  ERASE 只碰 `blk_erase`（不被读）⇒ 三种 op 同窗口在途互不影响。
- PROGRAM 页号跨轮单调递增，`W*R` 控制在 `PAGES_PER_BLOCK` 内 ⇒ 不重复写同一页。
- 规模：`W=8`、`R=4` ⇒ 32 条命令（约 1/3 是 program）。板上耗时可接受，远低于各池上限。

**B3 注册——无需改动**：用例是 `g_fc_lifecycle_tests[]` 的新条目，该数组已在 `ut_main.c:40`
注册为子套件 `lifecycle/` ⇒ **不碰 `makefile.mk`、不碰 `ut_main.c`**。

### `docs/design/fc_nand_ut.md`

用例表加 `mixed_pipeline` 行（Level 3 用例数 12 → 13）；「地址相关用例」列表 9 → 10；
新增「批量提交 / 批量收取（v1.32）」小节；op 覆盖表补 v1.32 说明（**不新增任何 op**）；
§7 修订历史加 **v1.32** 行。

## 三、设计说明：为什么是「批量上限由调用方给」

对齐行业模型（NVMe）：**SQ/CQ 由主机驱动、完成项靠 CID 配对，控制器不规定主机一次收几条**。

- FC 只给**机制**——`fc_poll_completions` 弹到上限或 CQ 空为止；**策略**（一次收几条）留给上层。
- 测试传的 `batch = CH_NUM*CE_NUM*LUN_NUM` 正好是 `nand_dispatch_poll` 单次调用的推进上限：
  收满这一批，就是「把这一轮 `fc_service` 能推进的完成项全部取走」。
- **拓扑无关**：`batch` 与 `W` 都是**测试侧**的编译期表达式；FC 不知道也不该知道拓扑。
  今天 `LUN_NUM=1` ⇒ `batch=1`，测试仍串行推进；多 die 器件上同一份代码自动变 `batch>1` 真并行。
- **配对靠 `command_id`**：`fc_command_status_t.command_id` 是命令的稳定身份（v15 起由命令指针派生），
  测试用它把完成项配回本地槽表，再取回 `fc_command_t*` 释放——**先 poll 后 release**。

> ⚠ `fc_service()` 本轮**只要交付过 ≥1 条完成，就整段跳过填充 pass** ⇒ 推进节奏是
> 「交付轮 / 填充轮」交替。**这是刻意行为，不是缺陷**；窗口 `W > 4` 正是为此选的——
> 只有 `W` 大于 per-LUN FIFO 深，SQ 里才留得住命令、才压得出调度行为。

## 四、验证

```bash
make format-check                  # ✅ Format check passed.（须走 clang-format-15）
make clean && make tests           # ✅ 0 warning 0 error；用例数 55（54 → 55）
                                   #    text 231084 + data 1736 = 232820 B（.bin 232860 B）
```

**上板（用户跑，本版唯一强判据）**：

```
test /fc-nand/lifecycle/mixed_pipeline   # 单条，看批量提交 → 分批收取的推进
test                                     # 完整套，确认新 API 不影响既有 54 条
```

验收点：`mixed_pipeline` **1 of 1**；证据里能看到「每轮批量提交 W 条 → 分批收取（每批 ≤ batch）」
的推进；全套 **55 of 55**。

### 实测 —— ✅ 绿

```
/fc-nand/lifecycle/mixed_pipeline:                     [ OK     ] [ 39.48 ms ]
…
55 of 55 (100%) tests successful, 0 (0%) test skipped. Total wall-clock: 739.57 ms
```

- `mixed_pipeline` 通过 ⇒ 4 轮 × 8 条的批量提交 / 分批收取流水线**全部收齐**（未触预算上限），
  每条完成项都按 `command_id` 配回槽位、状态无错误位、READ 读回逐 CW 校验通过；
  Phase 0 的前置块（擦 + 写 + 读）也都成立。
- **完整套 55 of 55** ⇒ 新增的 `fc_poll_completions`（A1~A4，四处纯增量）**无回归**——
  「没有遗漏的调用点」这条只有完整套能判（先例 = v47、v49）。
- ⚠ 本次上板用的是**默认日志档** ⇒ 用例体内的 `test_trace_summary` 证据行（每轮 submitted/drained/services
  计数）未落到串口。用例本身已通过（判据不依赖证据行），故**不重跑**；若要留存推进节奏，
  可另跑一次 `test --log-visible info /fc-nand/lifecycle/mixed_pipeline`。

## 五、风险 / 需上板确认（如实记录）

- **`fc_service()` 跳过填充 pass**：本轮只要有完成交付就不填充。故批量提交后，推进节奏是
  「交付轮 / 填充轮」交替——窗口必须 > FIFO 深（`W=8 > 4`）才能让 SQ 里压住命令、真正给调度器
  加压。**这是刻意**，不是缺陷。
- **拓扑无关只是「写对了」，今天跑不出并行**：`LUN_NUM=1` 时 `batch=1`、NAND 最多 1 条在途，
  测试仍串行推进。多 die 器件上同一份代码自动变成 `batch>1` 真并行——**这正是设计目标**，
  但本板无法验证并行正确性。
- **UT 期间 FTL 活着**（既有风险，同 v40）：随机窗口只挡**固定**元数据区（per-plane 0..41），
  动态 L2P 块拦不住。沿用 Level 3 的 30%~70% 窗口（614..1433），不改 `src/ftl/**`。
- **新 API 是公开面扩张**：`fc_poll_completions` 与单条版语义一致、纯增量；唯一需要留意的是
  `count` 出参必须非空（ASSERT 守）。
- **本版动了 `src/`**（A1~A4），只有真实器件能判——但四处全是**增量**（新函数/新声明），
  `fc_poll_completion` 与既有路径一行未改。

## 不做

- 不改 `fc_poll_completion`、不动 `fc_service` / `nand_dispatch_poll` 的调度语义。
- 不做 `--iterations` 批量（那是 munit 全局旋钮，v40 已记录它不改报告条数）；压力规模靠
  `W`/`R` 编译期常量。
- 不动 `tests/integration/**`（用户文件）、`tests/framework/**`、`src/ftl/**`；不翻 `CONFIG_FC_RAID`。
- 不把 `l3_*` 随机件上提到共享层（本版直接复用，不动它们）。

## 出处

- 批量原语：`include/fc/fc_interface.h:102`、`src/fc/fc_interface.c:280`、
  `include/fc/fc_internal.h:135`、`src/fc/fc.c:1844`。
- 单条 CQ 出口（未动）：`fc_poll_completion_internal`（`src/fc/fc.c`）。
- CQ pop：`src/fc/fc_queue.c:125`；命令释放：`src/fc/fc.c:578`（幂等）。
- 调度推进上限：`src/nand/nand_dispatch.c:755`（三层循环、全遍历不早退）。
- `fc_service` 跳过填充：`src/fc/fc_interface.c:259`（壳）→ `src/fc/fc.c:1793`（`fc_poll_internal`）。
- 夹具导出：`tests/unit/fc_test_common.h:349`、`tests/unit/fc_test_common.c:198`。
- 用例：`tests/unit/test_fc_level3.c:1437`（`test_mixed_pipeline`）、`:1687`（注册）。
- 同构的集成先例：`tests/integration/test_fc_nand.c:351`（`fc_nand_test_run_block_pipeline`）。
- FTL 侧潜在消费方：`src/ftl/completion_table.c:143`（`completion_table_drain`）。

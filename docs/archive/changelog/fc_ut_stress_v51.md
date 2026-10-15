# v51：`mixed_pipeline` 重写为**流式提交 / 池满即抽干**的背压形态

- **日期**：2026-10-11
- **范围**：**纯测试 + 文档**。`tests/unit/fc_test_common.{c,h}`、`tests/unit/test_fc_level3.c`、
  `docs/design/fc_nand_ut.md` + 本文档。**`src/**` 一行不动**（含 `src/ftl/**`、`src/fc/**`、`src/nand/**`）。
- **介质格式**：无变更。
- **测试**：**用例数不变（仍 55）**——`mixed_pipeline` 是**重写**，不是新增。
  `make format-check` 过；`make clean && make tests` **0 warning 0 error**、
  `.bin` **232260 B**（text 230480 + data 1736 = 232216；v50 是 232860 / 231084 + 1736）。
- ✅ **上板已验**（2026-10-11，seed `0x5a4f21e4`）：
  `/fc-nand/lifecycle/mixed_pipeline` **[ OK ] [ 164.47 ms ]**、完整 `test` **55 of 55 (100%)**、0 skipped。

## 〇、起因：v50 的形状压不出压力

v50 的 `mixed_pipeline` 形状是「每轮批量提交 `W=8` 条（op 轮转 PROGRAM/READ/ERASE、各打自己的专用块）
→ 抽干 → 下一轮，共 `R=4` 轮」。用户判定这个形状**没有施加任何压力**，并给出期望的节奏（逐字）：

> 写page0获取命令下发，polling，没有完成的 / 写page1获取命令下发，polling，没有完成的 / ……
> 写pagex获取命令下发，polling有命令完成，处理释放 / …… 写pagey获取命令失败没有资源，polling，
> 有命令完成处理释放 / 写pagey获取命令成功，polling，有命令完成处理释放 / ……
> 退出循环后看还有多少CQ没处理，一起处理了 / 完成一次写block

即：**连续下发、不等完成**，靠**资源耗尽**自然形成背压——发到池满 ⇒ 建链失败 ⇒ 抽 CQ 腾资源 ⇒
**重试同一条**；粒度是「整块先写后读」，遍历 `ch → ce → lun → plane → page`。

## 一、诊断：两次上板量出来的两点

### 第一版（单 plane、逐页 polling）——压不到池顶

按上面的节奏重写后，**逐页**做 polling + 取 CQ。上板实测（`--log-visible debug`）在途命令只爬到
**`buf=104/128`（26 条）**，而 buffer 池的阈值是 **32 条** ⇒ **全程一次 `retry` 都没有**，
背压闭环根本没进过。原因是爬升率太低：`fc_service()` 单次调用**最多交付 1 条完成**
（`nand_dispatch_poll` 今天 `CH×CE×LUN = 1`），且**只要交付过 ≥1 条就整段跳过填充 pass**
（`fc_poll_internal`，`src/fc/fc.c:1793`）⇒ 逐页 poll 时在途净增速只有约 **0.2 条/页**，
128 页（2 plane × 64）跑完也够不着 32。

### 第二版（multi-plane、单块、每 4 页 polling）——必到顶

用户给的最终形状：**multi-plane、单块、每提交 4 个 page（或本条失败）做一次 polling + completions**。
两点同时变：

| | 第一版 | 第二版 |
|---|---|---|
| 命令形态 | 单 plane 页命令（4 desc + 4 buf） | **multi-plane 页命令（8 desc + 8 buf）** |
| 一块的页数 | 128（2 plane × 64） | **64**（plane 由 op 一次覆盖） |
| buffer 池阈值 | 32 条 | **16 条** |
| 在途爬升率 | ≈ 0.2 条/页 | ≈ **0.84 条/页** |

阈值砍半 + 爬升率翻两番 ⇒ **64 页内必然触顶**。上板实测：`p18` 那条命令落下后
`buf=128/128`（= 16 条），`p19` 起进入稳态 `retry +poll → sub`，与预测一致（见「四」）。

## 二、决策

| 问题 | 结论 |
|---|---|
| 怎么造背压 | **连续下发不等完成**；每成功下发 4 条、**或本条下发失败**（池满 / SQ 满）才 polling + 取 CQ。在途数顶到池上限即形成闭环。 |
| 用什么 op | **multi-plane 变体** `OP_PROGRAM_PAGE_MULTI_PLANE`(31) / `OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT`(30) / `OP_ERASE_BLOCK_MULTI_PLANE`(32)。一条命令覆盖两个 plane ⇒ 遍历维度只剩 `ch→ce→lun→page`，且每条命令吃**双倍** desc/buffer。 |
| 遍历什么 | 整块：Phase 1 写全 64 页 → 抽干 → Phase 2 读全 64 页并逐 CW 校验。**先写后读**（不是 v50 的读写擦交替）⇒ 读的每一页必然已写完，**无数据冒险**。 |
| 建链失败怎么办 | 夹具新增 **`test_cmd_try_build()`**——同 `test_cmd_build` 的配置校验，但**资源申请失败返回 NULL**（不 abort）。配置错误（坐标越界 / 掩码漏 plane）仍是 ASSERT：那是调用者 bug，不是资源问题。 |
| 重试用什么 | **同一条 `addr`**：`while (sent == 0) { try_build → submit → 失败则 polling + 取 CQ }`。下发成功才进下一页。 |
| 怎么收尾 | 遍历完再 `while (drained < submitted) { fc_service(); reap(); }` 把剩余 CQ 一起处理完（兑现用户「退出循环后看还有多少CQ没处理，一起处理了」）。 |

> 备选（未采纳）：把「池满」当成**失败**直接跳过该页。不采纳的理由：那样就测不到
> 「资源回流后同一条能建成」这条不变式——而它正是背压闭环的全部意义。

## 三、改动

### `tests/unit/fc_test_common.{c,h}`

`test_cmd_build` 拆成 **`test_cmd_try_build`（实体）+ `test_cmd_build`（断言薄壳）**：

```c
fc_command_t *test_cmd_try_build(const nand_addr_t *addr, uint32_t cw_bitmap, uint32_t meta, const fc_cmd_conf_t *config)
{
    /* …原有配置校验（坐标越界 / cw==0 例外 / multi-plane 掩码覆盖）原样保留，仍是 ASSERT… */
    cmd = fc_command_alloc();
    if (cmd == NULL) { return NULL; }                 /* 命令池空 */
    …
    if (ftl_cmd_configure_descriptors(cmd, cw_bitmap) != FTL_OK)
    {
        (void)fc_command_release(cmd);                /* 沿链回收已挂的 desc/buffer + 归还命令槽 */
        return NULL;                                  /* desc / buffer 池空 */
    }
    return cmd;
}

fc_command_t *test_cmd_build(...)   /* 断言薄壳：munit_assert_not_null(try_build(...)) */
```

**理由**：背压闭环要求「建不出来」是一个**可恢复的返回值**，而 `test_cmd_build` 的
`munit_assert_not_null` 会当场 abort。**失败时已整条 `release`**（`fc_command_release` 幂等，
v16 起沿链回收 desc + buffer）⇒ 调用者拿到 NULL 即可，**无需再释放**，也不会泄漏半条链。

**对既有 54 条零影响**：`test_cmd_build` 的行为逐字不变（仍是「建不出来就 abort」），
只是实现挪进 `test_cmd_try_build`。上板完整套 55 of 55 印证。

### `tests/unit/test_fc_level3.c`

`mixed_pipeline` 整体重写（**注册行不变**，仍在 `g_fc_lifecycle_tests[]`）：

```c
#define L3_STRESS_BATCH      ((uint16_t)(CH_NUM * CE_NUM * LUN_NUM))  /* 单次收 CQ 上限 */
#define L3_STRESS_SLOTS      (FC_COMMAND_POOL_DEPTH)                  /* 在途登记表容量 */
#define L3_STRESS_BUDGET     (TEST_SERVICE_LOOP_MAX)                  /* 每页重试预算 */
#define L3_STRESS_POLL_EVERY (4U)                                     /* 每 4 条成功下发 poll 一次 */
```

新增件（全部 `static`，只服务本用例）：

| 件 | 作用 |
|---|---|
| `g_l3_stress_slots[]` + `l3_stress_slots_reset/alloc/find` | 在途命令登记表（容量 = 命令池；`cmd == NULL` 即空槽）。完成项只带 `command_id`，靠 `fc_command_to_id` 配回命令 + 地址（CID 配对的土办法）。 |
| `l3_stress_try_build(op, addr, plane_bm)` | 建一条 multi-plane 命令；PROGRAM 填源数据、READ 预填 `0xCC` 残留哨兵。资源不足返回 NULL。 |
| `l3_stress_reap()` | `fc_poll_completions(buf, batch, &n)` → 逐条配槽 → 断言无 `FC_CW_STATUS_ERROR_MASK` → READ 先 `test_capture_cmd` 再 `test_verify_page_paa` → **最后才** `fc_command_release`（release 会还 buffer，顺序不能反）。返回处理条数。 |
| `l3_stress_stream(op, block)` | 一个 phase：`for ch/ce/lun/page` 流式下发，`while (sent == 0)` 内重试；退出后再把剩余 CQ 收干净，并打收尾证据。 |

流程：

```
Phase 0  擦块（multi-plane 一次覆盖两个 plane）—— 硬断言无错误位
Phase 1  l3_stress_stream(PROGRAM, block)   写全块 64 页
Phase 2  l3_stress_stream(READ,    block)   读全块 64 页并逐 CW 校验
```

`l3_stress_stream` 内核（每页）：

```
while (sent == 0) {
    cmd = l3_stress_try_build(op, &addr, TEST_PLANE_BM_ALL);
    if (cmd == NULL)                     { /* 池满：本页还没发出去 */ }
    else if (fc_submit(cmd) != OK)       { fc_command_release(cmd); /* SQ 满 */ }
    else                                 { 登记槽位; sent = 1; since_poll++; }
    if (sent == 0 || since_poll >= 4)    { fc_service(); drained += l3_stress_reap(); since_poll = 0; }
    证据行（--log-visible debug）;
    munit_assert_uint32(++guard, <, L3_STRESS_BUDGET);   /* 死循环护栏 */
}
```

**证据行**（`test_trace_detail` = `MUNIT_LOG_DEBUG` ⇒ 只在 `--log-visible debug` 下打印；
默认档输出**逐字不变**）：

```
[EV]   p19 retry +poll submitted=19 drained=3 | cmd=16/64 desc=128/256 buf=128/128 sq=15 cq=1
```

`sub`/`retry` = 本条下发成功 / 没发出去；`+poll` = 本轮做了 polling。**一眼看出**
「发到第几条开始 retry、retry 后完成项回流把哪个池腾空」。

**抽样**：`l3_block_draw("mixed_pipeline")` 抽一次（base block），`ch/ce/lun/page` 全遍历
⇒ 复现契约 = (seed, 用例名, 第 1 次抽样)。不再抽 plane（plane 由 multi-plane op 派生）。

## 四、上板实测（seed `0x5a4f21e4`，`test --log-visible debug`）

### 爬升段 `p00..p18`：只 `sub`，池逐条涨满

```
[EV]   p00 sub         submitted=0  drained=0 | cmd= 1/64 desc=  8/256 buf=  8/128 sq=1 cq=0
[EV]   p08 sub         submitted=8  drained=0 | cmd= 9/64 desc= 72/256 buf= 72/128 sq=9 cq=0
[EV]   p18 sub         submitted=18 drained=0 | cmd=16/64 desc=128/256 buf=128/128 sq=15 cq=0
```

### 饱和段 `p19..p63`：稳态 `retry +poll → sub`

```
[EV]   p19 retry +poll submitted=19 drained=3 | cmd=16/64 desc=128/256 buf=128/128 sq=15 cq=1
[EV]   p19 sub         submitted=19 drained=3 | cmd=16/64 desc=128/256 buf=128/128 sq=16 cq=1
[EV]   p20 retry +poll submitted=20 drained=4 | cmd=16/64 desc=128/256 buf=128/128 sq=15 cq=0
[EV]   p20 retry +poll submitted=20 drained=5 | cmd=15/64 desc=120/256 buf=120/128 sq=15 cq=0
[EV]   p20 sub         submitted=20 drained=5 | cmd=16/64 desc=128/256 buf=128/128 sq=16 cq=0
```

三点读数：

1. **触顶位置与算出来的一致**：撑满的是 **buffer 池 128/128 = 16 条命令**（16 × 8 buf）；
   desc 只到 `128/256`（50%）⇒ **buffer 是绑定池，desc 不是**——与「每 CW 一条 desc + 一条
   buffer，multi-plane 命令吃 8 份」的算法吻合。
2. **`retry` 不等于必有一次完成**：`p20` 第一次 poll 走了**填充 pass**（没交付完成 ⇒ buf 仍 128），
   第二次才交付、腾出 8 buf。这正是 `fc_poll_internal`「交付过完成就跳过填充」那条语义。
3. **收尾干净**：两个 phase 都打

   ```
   [EV] phase done: submitted=64 drained=64 | cmd=0 desc=0 buf=0 sq=0 cq=0 (应为全 0)
   ```

   ⇒ 64 条命令的 desc / buffer / 命令槽**一条不漏全还回去了**。

### 用例与整套

```
/fc-nand/lifecycle/block_lock_ops:   [ OK     ] [   4.04 ms ]
/fc-nand/lifecycle/mixed_pipeline:   [ OK     ] [ 164.47 ms ]     ← v50 是 39.48 ms
55 of 55 (100%) tests successful, 0 (0%) test skipped. Total wall-clock: 863.25 ms
```

耗时 4 倍于 v50，多出来的正是那上百次 `retry` + `poll`——**不是异常，是要压的东西**。
其余 54 条耗时与 v50 逐条对得上 ⇒ 夹具拆分零影响。

## 五、验证

```bash
make format-check                  # ✅ Format check passed.（须走 clang-format-15）
make clean && make tests           # ✅ 0 warning 0 error；用例数仍 55
                                   #    text 230480 + data 1736 = 232216（.bin 232260 B）
```

**上板（用户跑，本版唯一强判据）**：

```
test --log-visible debug /fc-nand/lifecycle/mixed_pipeline   # 看逐页资源快照与 retry 起点
test                                                        # 完整套，确认夹具拆分不影响既有 54 条
```

验收点：`mixed_pipeline` **1 of 1**；证据里能看到 **`retry` 出现**（背压真的进了）；
两个 phase 收尾三池归零；全套 **55 of 55**。

## 六、风险 / 需上板确认（如实记录）

- **`retry` 出现是"设计生效"的唯一证据，但它不是硬判据**：本用例的硬判据是
  「全块写-读完成 + 逐 CW 校验通过 + 三池归零」。`retry` 只在 `--log-visible debug` 的证据行里
  可见；默认档下**即使背压没触发，用例也会绿**。⇒ 要看背压必须显式加 `--log-visible debug`。
  （这是刻意的：判据不该依赖某个池深的具体数值，否则调池深就会假红。）
- **buffer 是绑定池，且与拓扑绑定**：阈值 = `FC_BUFFER_POOL_DEPTH / (PLANE_NUM * CW_NUM)` = 16 条。
  改 `PLANE_NUM` / `CW_NUM` / 池深都会挪动 `retry` 的起点，但**不影响用例是否通过**。
- **拓扑无关仍只是「写对了」**：今天 `LUN_NUM=1` ⇒ `batch=1`、NAND 最多 1 条在途，测试仍串行推进。
  多 die 器件上同一份代码自动变 `batch>1` 真并行——**这正是设计目标，但本板无法验证并行正确性**。
- **UT 期间 FTL 活着**（既有风险，同 v40）：随机窗口只挡**固定**元数据区（per-plane 0..41），
  动态 L2P 块拦不住。沿用 Level 3 的 30%~70% 窗口（614..1433），不改 `src/ftl/**`。
- **本版不改 `src/`** ⇒ 无新增上板风险面；完整套通过即等价于「夹具拆分对 src 无影响」。

## 不做

- 不改 `src/**`（含 `fc_poll_completions`——它 v50 就落地并已上板验）。
- 不恢复 v50 的「读写擦交替、三个专用块」形状（已被判定不加压）。
- 不动 `tests/integration/**`（用户文件）、`tests/framework/**`、`ut_main.c`、`makefile.mk`。
- 不翻 `CONFIG_FC_RAID`；不把 `l3_*` 随机件上提到共享层。

## 出处

- 用例：`tests/unit/test_fc_level3.c`（`test_mixed_pipeline` 及其上方 `l3_stress_*`）。
- 夹具：`tests/unit/fc_test_common.c`（`test_cmd_try_build` / `test_cmd_build` 薄壳）、
  `tests/unit/fc_test_common.h`（`test_cmd_try_build` 声明）。
- 批量收 CQ：`fc_poll_completions`（`include/fc/fc_interface.h`、`src/fc/fc.c`，v50 落地）。
- 单次交付上限 / 填充 pass：`nand_dispatch_poll`（`src/nand/nand_dispatch.c:755`）、
  `fc_poll_internal`（`src/fc/fc.c:1793`）。
- 命令释放（幂等、沿链回收）：`fc_command_release`（`src/fc/fc.c`）。
- 前作：`docs/archive/changelog/fc_ut_stress_v50.md`（v50 的批量提交/批量收取形状 + `fc_poll_completions`）。

# v0.68：GC 触发求值加节拍门 + 触发日志走自己的节拍

**日期**：2026-10-14
**分支**：main
**涉及模块**：`include/ftl/ftl.h`、`src/ftl/ftl.c`（`ftl_gc_evaluate` / `ftl_init`）
**起因**：板端跑完 FTL L0 UT 后 `[INF] [GC] trig=0x09 free=0 op=0 writes=0` 刷屏

---

## 〇、起因

跑完 FTL L0 UT 后，UART 开始全速刷：

```
[INF] [GC] trig=0x09 free=0 op=0 writes=0
[INF] [GC] trig=0x09 free=0 op=0 writes=0
...
```

**其中一半是既定代价**：`ftl_fixture_reset`（`tests/unit/test_ftl.c:71`）memset 掉活实例的
表后调 `allocator_init`，而它**不回填 Free Pool**、`total_writes` 也在 `:85` 被清零 ⇒
`free=0 op=0 writes=0`。这一半复位即消，文件头 `:14-17` 早已声明。

**另一半是生产缺陷**：这条日志没有节拍，任何把空闲池压在水位以下的**持续写负载**都会刷屏。

## 一、根因：求值点没有节拍

`ftl_gc_evaluate`（`src/ftl/ftl.c`）是 GC 的**统一触发求值点**，挂在 `ftl_service` 的
Phase 3 两条后台路径上：

| 路径 | 位置 | 命中后的动作 |
|---|---|---|
| IDLE | `ftl_service_idle` | 补池突发，回收 `⌈deficit/PLANE_NUM⌉` 个 SB |
| NORMAL | `ftl_service_normal` | 回收**一个** SB |

而主循环（`src/main.c:458`）是**没有节拍的 `while(1)`**：

```c
while (1)
{
    cli_service_poll();
    if (g_ftl_ready)
    {
        ftl_service_pump();                 /* ① 全局泵 */
        for (ch..) for (ce..) for (lun..)   /* ② 逐实例后台 */
            ftl_service(&g_ftl[ch][ce][lun]);
    }
}
```

⇒ 求值**每轮一次**。两个后果：

**① 日志刷屏。** `trig != 0` **不是异常**——`GC_TRIGGER_WATERMARK`(T1) 的定义就是
`free_count < FREE_POOL_TARGET(32)`，那是 GC 的**正常工作区间**（IDLE 补池的目标水位就是
32）。所以只要写负载把空闲池压在 32 以下，NORMAL 就逐轮打一行。
`trig=0x09` = T1 | T4（`0x08` = `free_count <= FREE_POOL_EMERGENCY(8)`），与 `free=0` 一致。

**② 每轮一次全表扫描。** `gc_check_triggers` → `gc_table_extremes`（`gc.c:305`）：

```c
for (uint16_t b = 0U; b < FTL_BLOCK_TOTAL; b++)   /* 2048 × PLANE_NUM(2) = 4096 块 */
```

T5 判据要全表磨损极值（min/max `erase_count`），所以这次扫描无法避免；但**它的频率不该
等于主循环频率**。GC 选不出 victim 的卡死态下（本次 UT 后的状态），`ftl_gc_reclaim_one`
立刻返 0、循环空转 ⇒ 每轮扫 80 KB、每轮打一行，纯烧 CPU。

## 二、为什么「加节拍」是对的

触发判据本身是**事件**（空间压力/磨损），不是「每轮算一次」：`free_count` 与磨损表变化
都很慢。**但主流的判据比本仓便宜得多**——这才是必须门控、而主流不必的原因：

| | 触发判据 | 求值成本 | 求值频率 |
|---|---|---|---|
| **SPDK FTL** | `ftl_needs_reloc()`：`dev->num_free <= ftl_get_limit(SPDK_FTL_LIMIT_START)` | **O(1)**（一次计数比较） | core poller **周期 0**（连续轮询）⇒ **每轮求值** |
| **本仓** | `gc_check_triggers`：T1/T4 读 `free_count`（O(1)）**＋ T5 全表磨损极值（O(N)）** | **O(FTL_BLOCK_TOTAL)** ≈ 4096 块 / 80 KB | 改造前同样每轮 |

⚠ **本文件初版把这一条写反了**（"主流跑在有节拍的调度单元上，没有哪家每个 pass 无条件
求值"）——**错**。SPDK FTL 的 `ftl_core_poller` 注册周期就是 `0`
（`lib/ftl/mngt/ftl_mngt_misc.c:208`：`SPDK_POLLER_REGISTER(ftl_core_poller, dev, 0)`），
即**连续轮询、每轮求值**。主流之所以能这么做，是因为它的判据是**常数时间**的：
`ftl_needs_reloc`（`lib/ftl/ftl_core.c:291`）只做一次 `dev->num_free` 与水位（默认 5，
`lib/ftl/utils/ftl_conf.c:20`）的比较。

⇒ 门控**不是**「向主流的节拍对齐」（SPDK 恰恰是每轮求值），而是**成本补偿**：同一件事
（空间压力）主流的判据是 O(1)，本仓因 T5 的全表扫描而昂贵。**真正的对齐做法是把
T1/T4（O(1)）与 T5（O(N)）拆开**——见 §四。

本仓既有同族做法，全部是 `ftl_now_ms()` 软件节拍 + per-instance 基准：

| 节拍 | 字段 | 周期 |
|---|---|---|
| 慢死预测巡检 | `rr_last_scan_ms` | `RR_SCAN_INTERVAL_MS` = 1 s |
| Wear 落盘 | `alloc.wear.last_flush_ms` | `WEAR_FLUSH_INTERVAL_MS` = 120 s |
| Checkpoint 定时 | `cp_timer_start` | `CP_TIME_INTERVAL_MS` = 10 s |

## 三、修法

**① 节拍门**（`ftl_gc_evaluate` 入口）：

```c
if ((ftl->gc_eval_last_ms != 0U) && ((now_ms - ftl->gc_eval_last_ms) < GC_EVAL_INTERVAL_MS))
{
    return 0U;
}
ftl->gc_eval_last_ms = now_ms;
```

`GC_EVAL_INTERVAL_MS` = 10 ms（`include/ftl/ftl.h`）。

⚠ **`0` 是「尚未求值」哨兵，不是普通时间戳**：`ftl_now_ms()` 在平台未注册时**恒返回 0**
（`ftl.c` 的既有取舍），若直接写 `(now_ms - last) < INTERVAL` 就会**永远为真 ⇒ GC 永不触发**。
用 `!= 0U` 短路后，缺时钟时每轮都求值（行为与加门前一致）——**GC 不能因缺时钟而停摆**。
这与 T3 定时的方向相反（那里「恒不触发」是安全的），故特意在字段注释里写明。

**② 日志走自己的节拍**（`GC_LOG_INTERVAL_MS` = 1 s，与求值同一套时基）：

```c
if (triggers != 0U)
{
    if ((ftl->gc_log_last_ms == 0U) || ((now_ms - ftl->gc_log_last_ms) >= GC_LOG_INTERVAL_MS))
    {
        ftl->gc_log_last_ms = now_ms;
        LOGI("[GC] trig=0x%02x free=%u op=%u writes=%lu\r\n", ...);
    }
}
else
{
    ftl->gc_log_last_ms = 0U;   /* 收敛：归零哨兵 ⇒ 下次进入立即打 */
}
```

**⚠ 求值节拍不能兼作日志节拍**——这两件事问的不是同一个问题：

| 节拍 | 回答的问题 | 周期 |
|---|---|---|
| `GC_EVAL_INTERVAL_MS` | 「多久**算一次**触发判据」（成本约束：全表扫描） | 10 ms |
| `GC_LOG_INTERVAL_MS` | 「同一状态**多久报一次**」（可读性约束：别刷屏） | 1 s |

只挂求值节拍的话，10 ms 拍 = **100 行/秒**，仍是刷屏。日志要的是「同一状态别重复报」。

**语义 = 进入触发态立即一行 + 触发态内每 1 s 一行心跳**。两个性质靠**一个字段**拿到：

- **进入即打**：退出触发态时把 `gc_log_last_ms` 归零 ⇒ 下次进入第一行立即出现，
  **不需要另存触发掩码**（早期草案用 `gc_trig_last` 做边沿触发，已废弃——多一个概念）。
- **卡死态有心跳**：稳态静默是对的（T1 是常态），但「一行之后再无消息」会被误读成
  **已恢复**。1 s 一行心跳让卡死可辨识。

**③ 心跳行带进展计数**（`gc_reclaimed_count` / `gc_novictim_count`）：

```c
/* ftl_gc_reclaim_one —— 唯一自增点，两条后台路径天然合一 */
if (!gc_select_victim(&ftl->alloc, &victim))
{
    ftl->gc_novictim_count++;   /* 触发但选不出 victim = 卡死嫌疑 */
    return 0;
}
...
ftl->gc_reclaimed_count++;      /* 确实回收了一个 SB */
```

**为什么**：`trig=` 常亮**不能区分**「触发但在推进」与「触发但卡死」——两种情况打的是
同一行；唯一的进展证据 `[GC] victim`（`gc.c:449`）只在 `found != 0` 时才打，卡死时
**根本不出现**，其"消失"又与"已收敛"**同形**。把计数放进心跳行本身 ⇒ **一行即可判读**：
两次心跳之间 `reclaimed` 不动、`novictim` 在涨 = 卡死。

⚠ 这不是假想问题。`allocator_close_sb` 的 valid 顺序缺陷（**cf604ba，已修**）就是
"触发常亮且永不收敛"的**生产形态**：运行期关闭的 SB 对 GC/WL 永久不可见 ⇒
`gc_select_victim` 恒选不出 victim ⇒ 空闲池只出不进 ⇒ `free_count` 掉到 0 后永不回升、
**写最终永久失败**。

**新增字段**（`ftl_context_t`，per-instance）：`uint32_t gc_eval_last_ms`、`uint32_t gc_log_last_ms`、
`uint32_t gc_reclaimed_count`、`uint32_t gc_novictim_count`，在 `ftl_init` 里清零。
前两者用 `0` 作哨兵；后两者是**只增不减**的累计计数。

## 四、代价（如实登记）

T1/T4 的空间压力响应也被门到同一周期 ⇒ IDLE 补池突发与 NORMAL 的「一轮回收一个 SB」
**最坏延迟 10 ms**。可接受的理由：

- 真卡死仍由写路径的**按需 GC** 兜底（`ftl_issue_subop` 的紧急 GC，等价主流的 demand GC），
  它**不经过**本函数、不受节拍影响；
- 10 ms 远小于搬一个 SB（2 plane × 64 页 = 128 页）的 NAND 时间，实际不构成瓶颈。

若日后要更快的空间响应，正确做法是把 **T1/T4（O(1)，只读 `free_count`）与 T5（O(N)，全表）拆开**：
便宜的每轮求值、贵的上节拍。本版**未做**——那需要给 T5 结果加缓存位并改动 `gc_check_triggers`
的语义，会牵动 `tests/unit/test_ftl.c:869`（直接断言 `gc_check_triggers` 返回 T5）的既有用例。

## 五、验证

| 项 | 结果 |
|---|---|
| `make all` | 0 warning / 0 error（`Binary size: 193804 bytes`） |
| `make tests` | 0 warning / 0 error（`Binary size: 323968 bytes`） |
| AXI_RAM | 291.2 KB / 384 KB（+16 B/实例 = 4 × `uint32_t`） |
| UT 用例 | **未改动**（`gc_check_triggers` 语义不变，`test_ftl.c:853-883` 不受影响） |

**上板判据**：

1. 跑完 FTL L0 UT 后**不再刷屏**：卡死态（`free=0`）下是 **1 行/秒**的心跳，
   而不是每轮一行（改前实测全速刷）。
2. 正常写负载下：**进入水位立刻一行**，之后每 1 s 一行心跳，**离开水位后静默**；
   `[GC] victim …`（`gc.c:449`）仍每个 SB 一行，作为回收进度的证据。
3. **卡死可辨**（本版新增的判据）：UT 后的卡死态心跳应形如
   `[GC] trig=0x09 free=0 op=0 writes=0 reclaimed=<不动> novictim=<持续上涨>`；
   正常回收中则 `reclaimed` 每行都在涨。**一行即可区分「卡死」与「在推进」**。

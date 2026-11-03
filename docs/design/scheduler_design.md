# 统一定时调度器（timer scheduler）设计与实施计划

> 状态：**Phase 0~5 已实现（2026-11-03，未上板）；Phase 4 随本次文档更新收尾**。
> Phase 5 = 后台节拍 flag（CP T3 / L2P flush / BBT 起新 job）+ token bucket；
> must-run 与在飞推进不变。
> 登记见 `known_iss.md` ISS-PLAT-04；时间基准风险见 `known_risks.md` RISK-PLAT-09，
> `mig_handle` 拆分风险见 RISK-PLAT-10。
> 目标：把 `btim` 的周期任务表与 FTL 的 `bg_scheduler` 收敛为**一个时间调度器**；
> 平台层只提供时间源，不再持有应用任务表。

## 1. 背景：现状是两套调度器

| 项 | 现状 | 问题 |
|---|---|---|
| 平台任务表 | `src/platform/btim.c`，表项 `{name, first_delay_ms, period_ms, callback, context}`，唯一静态任务 = watchdog 100 ms | 名为定时器，实为软件调度器；放在 `platform/` 层 |
| FTL 后台 | `src/ftl/bg_scheduler.c`，`bg_task_t {func, mask, units, must_run}`，预算/档位驱动 | 第二套「什么时候跑什么」 |
| 时间源 | TIM6 `g_btim_tick_ms`（btim） vs DWT `time_us`（`ftl_job_now_ms`） | 两套毫秒时基，不能混用；**DWT 现已删除**，系统时基统一为 SysTick |
| 派发点 | `btim_scheduler_poll()` 在主循环末尾；`bg_scheduler_service()` 在 `ftl_service` 内 | 两个派发点、两种顺序模型 |
| 周期门 | `btim` 表内；FTL 侧散落在 `mig_handle` / `cp_should_start` / 各心跳 | 同一语义多套写法 |

主流形态是**一张表 + 一个时间源 + 主循环统一派发**：SPDK `spdk_poller`（`fn/arg/period_us`，
`period_us=0` 表示每轮）、DPDK `rte_timer`（每 lcore 一张表，主循环 `rte_timer_manage()`）、
RTOS 单调度器。本仓的两套是历史分层产物，不是目标形态。

## 2. 目标架构

```
platform：系统 ms/µs 时基 = ARM 核心 SysTick（HAL tick + VAL/LOAD）；
          timer 调度器心跳 = 基本定时器 TIM6（只 tick++）
timer    ：一张任务表 { fn, ctx, first_due, period, enabled, runs }
           main loop → timer_poll()
注册者  ：watchdog（静态）、[STAT]（动态）、FTL 每实例的周期评估（动态，ctx=ftl）
ftl_service：前台流水线保持显式顺序（confirm → cache confirm → host 补发 → …）
bg_scheduler：降级为**预算策略子引擎**（由 ftl_service 每轮调用，决定「做多少」）
```

分工：

- **timer 管「何时」**：一次性、周期、绝对时刻、取消/暂停/改周期；
- **bg_scheduler 管「做多少」**：档位、掩码、工作量配额、must-run；
- **`ftl_service` 管「按什么顺序」**：前台流水线的顺序不变量不交给调度器。

## 3. API

```c
typedef void (*timer_task_fn)(void *ctx, DWORD now_ms);

/* 相对首次延迟 + 周期；period_ms == 0 ⇒ 一次性 */
bool timer_add(timer_task_fn fn, void *ctx, DWORD first_delay_ms, DWORD period_ms, DWORD *out_id);

/* 绝对时刻（相位对齐用）；内部换算成 first_delay */
bool timer_add_at(timer_task_fn fn, void *ctx, DWORD first_abs_ms, DWORD period_ms, DWORD *out_id);

bool timer_cancel(DWORD id);
bool timer_set_period(DWORD id, DWORD period_ms);
bool timer_enable(DWORD id, bool enabled);
void timer_poll(void);
DWORD timer_now_ms(void);
WORD timer_task_count(void);
```

语义：

| 需求 | 参数 |
|---|---|
| 延迟 N ms 执行一次 | `first=N, period=0` |
| 每 M ms 周期执行 | `first=首次相位, period=M` |
| 绝对时刻 T 执行一次 | `timer_add_at(T, 0)` |
| 取消 / 暂停 / 改周期 | `cancel` / `enable` / `set_period` |

约束：

- **回调在 main 上下文执行**，不在 ISR；到期后在**下一次 `timer_poll()`** 执行。
  抖动 = 主循环一轮 + 前序任务耗时；不提供硬实时。
- **周期重装**：`next_due = prev_due + period`；落后超过一个周期时钳到 `now + period`，
  不追赶风暴（沿用 `btim.c:182-198` 的既有策略）。
- **一次性**：执行后 `enabled = 0`；动态槽可 `cancel` 回收。
- **ID**：`index + 1`，0 = 无效；`cancel` 后 ID 失效，复用槽位后旧 ID 属编程错误
  （debug `ASSERT`）。单线程 main 上下文，不引入 generation 计数。
- **重入**：回调可 `add/cancel`；`timer_poll` 每轮重新读槽位，已取消项（`fn == NULL`）跳过；
  回调内新增且 `first_delay=0` 的任务允许在同一轮稍后执行，文档写明。

## 4. 数据结构与时间基准

```c
#define TIMER_MAX_TASKS (64U)   /* 8 实例 × 4 个 FTL 周期任务 + 平台任务，留余量 */

typedef struct
{
    timer_task_fn fn;
    void         *ctx;
    DWORD         next_due_ms;
    DWORD         period_ms;   /* 0 = 一次性 */
    DWORD         runs;
    BYTE          enabled;
} timer_task_t;
```

- **系统时基 = ARM 核心 SysTick（ms + µs）**：ms 读 `HAL_GetTick()`，µs 在 1 ms
  窗口内用 `SysTick->LOAD/VAL` 插值（`delay_time_now_us` / `delay_time_now_us64`）。
  delay / usart / watchdog / TinyUSB / FTL 平台 `time_ms` 全部用它。
- **timer 调度器心跳 = 基本定时器 TIM6**：`g_timer_tick_ms` 只服务 timer 任务表的
  到期判定与 `timer_scheduler_now_ms()`；ISR 只 `tick++`，到期扫描在主循环。
  它与系统时基是**不同语义**：前者是「何时跑后台任务」，后者是「现在的系统时间」。
- **DWT 已删除**：DWT 是调试/跟踪单元，不是存储主控的产品块；本仓不再把
  `CYCCNT` 当时间源。NAND 忙期 / USB 分段计时改用 SysTick µs 时间戳。
- 回绕：32 位毫秒约 49.7 天；比较一律用有符号差 `(int32_t)(now - due) >= 0`
  （`timer.c` 的既有写法）。32 位 µs 约 71.6 min 回绕；超时一律用无符号差值。

### 4.1 定时器选型判据（TIM6 vs TIM2/TIM5）

- **「基本定时器 / 通用定时器」是 STM32 器件分类（RM0433），不是存储主控的架构判据。**
  本仓 `docs/reference/` 没有「SSD 主控只有基本定时器」的出处，不能把它写成行业标准。
  主控真正需要的是：单调时间源、周期 tick、watchdog、超时时间戳；这些由**功能**决定，
  不由定时器类别决定。
- **本仓选型**：timer 调度器只需要「能产生周期中断」，TIM6（basic）是满足要求的最小
  外设，不占用带输入捕获/比较输出的通用定时器。系统 ms/µs 时基由 ARM 核心 SysTick
  提供，不额外占定时器。**不使用 TIM2/TIM5。**
- **T2/T5 的唯一合理场景**：需要 32-bit 自由运行计数器（例如长周期 µs 时间戳）。
  本仓 µs 测量已由 SysTick 覆盖（1 ms 窗口插值 + 软件 ms 扩展），不需要 T2/T5。
  只有在「需要不依赖 SysTick 的 32-bit µs 自由计数器」时才重新评估，届时作为独立
  设计项登记。
- 测试：`ftl_ut_clock_install/set_ms/restore` 注入 `time_us` 与 `time_ms`
  （`tests/unit/ftl_ut_common.c`），纯 UT 可确定性地推进到期/周期/回绕。
- 容量：64 × sizeof(timer_task_t) ≈ 1.8 KiB `.bss`（DTCM/`.dtcm_data` 预算需在 Phase 1 复核）。

## 5. 迁移顺序

### Phase 0 — `btim` → `timer` 改名（机械，无行为变化）

- 文件：`include/platform/btim.h` → `timer.h`；`src/platform/btim.c` → `timer.c`；
  `src/platform/stm32h743/btim_port.c/.h` → `timer_port.c/.h`。
- 符号：`btim_*` → `timer_*`、`BTIM_*` → `TIMER_*`、`g_btim_*` → `g_timer_*`。
  **不改** `TIM6_DAC_IRQHandler` / `TIM6_IRQHandler`（startup 向量名固定）与 `HAL_TIM_*`。
- 删除遗留 `include/platform/stm32h743/btim.h.utf8`（不是编译输入）。
- 同步：`src/main.c` 的 include/注释/启动日志、`include/platform/watchdog.h` 注释、
  `docs/design/platform_abstraction.md`、`docs/architecture.md`、`docs/boot_flow.md`、
  `docs/design/usb_msc_udisk.md`、`CLAUDE.md` §5.6 的同名清单。
- h7r3 目录**不碰**（用户后续删除）。
- 验证：三关零告警；`grep -rn "btim" src include docs` 只剩 h7r3/归档；`make clean` 清旧对象。

### Phase 1 — 调度器核心 + 平台任务

- 实现 `timer_add/add_at/cancel/set_period/enable/poll/now_ms/task_count`；
  静态表保留 watchdog（100 ms）；动态槽从静态表之后分配。
- timer 调度器心跳用 TIM6 1 ms tick；系统 ms/µs 时基用 ARM 核心 SysTick（DWT 删除）。
- `[STAT]` 迁移：
  - `main.c` 增加 `ssd_ftl_stat_task(void *ctx, DWORD now_ms)`，内部检查 `g_ftl_ready`；
  - 启动时 `timer_add(..., 10000U, 10000U, &id)`；
  - 删除 `ssd_ftl_log_status()` 里的 `static DWORD last_ms` + `HAL_GetTick()` 门。
- 可选：CLI `timer` 打印 `id/name/period/next/enabled/runs`，用于上板取证。
- 验证：三关；新增纯 UT（一次性/周期/绝对/取消/暂停/改周期/回绕/落后钳位/回调自取消）；
  上板 watchdog 不复位、`[STAT]` 10 s。

### Phase 2 — FTL 周期评估迁入（**已实现 2026-11-03，未上板**）

- `mig_handle` 保留每轮职责：在飞 job 推进、会话收尾/进展、紧急 GC；每轮把 `level`
  发布到 `m->level`。
- 新增 `mig_eval(m)`：原 GC → WL → predict 的 `else if` 链整体迁入；GC 的 10 ms
  节拍由 timer 表提供（删除 `gc_eval_last_ms` 门）；WL 5 s / predict 1 s 的间隔门仍在
  handle 内（predict 依赖 `wl_eval_last_ms` 让位），保持原顺序语义。
- **时钟分配（关键）**：TIM6 只决定 `mig_eval` 的**唤醒频率**；`mig_eval` 内部的
  deadline/interval 一律读 `ftl_job_now_ms()`（SysTick）。`mig_handle` 的会话收尾/
  紧急 GC、`mig_job_abort/finish` 也用 SysTick ⇒ `gc_session_*` / `gc_nogain_ms` /
  `wl_eval_last_ms` / `predict_last_ms` 全部是同一时间坐标，不会出现「TIM6 写的
  时间戳拿 SysTick 去比」。
- `mig_timer_register(m)` 每实例注册**一个** `mig_eval` 任务（period = `GC_EVAL_INTERVAL_MS`，
  ctx = `mig_handle_t*`）；`main` 在 `timer_scheduler_init()` 之后、`g_ftl_ready` 为真时
  遍历实例注册。未采用「三个独立任务」：单任务保留原 `else if` 的「每轮最多一个 job」
  与 WL/predict 让位关系，减少行为漂移；代价是表里只看到 10 ms 一行。
- 无时钟退化：`ftl_job_now_ms() == 0` 时 `mig_handle` 直接调 `mig_eval(m)`，避免 timer
  任务永不到期导致 GC 停摆（与旧「无时钟 ⇒ 每轮求值」一致；此时内部 `now_ms` 也是 0）。
- CP T3（10 s）**本阶段不动**：`cp_handle` 有 ADVANCE/EMERGENCY/EXPLICIT/水位/写入量
  多个入口，T3 门与 `journal delta ≥64` 耦合，先保持现状，Phase 3 再评估。
- 顺序：`timer_poll()` 仍在主循环末尾；eval 在 `ftl_service` 之后触发 kick，job 下一轮
  由 `mig_handle` 推进。10 ms 级延迟一圈可忽略，但需上板确认无 stall。
- 验证：上板 `[GC] trig` 不高于 10 ms/次；WL/RR 仅 IDLE；H2testw 415/423 MB 0 错误；
  写/读吞吐不低于基线；无 `confirm park timeout` 回归。

### Phase 3 — 时间基准收口（**已实现 2026-11-03，未上板**）

- **系统时基 = ARM 核心 SysTick**：ms 读 `HAL_GetTick()`，µs 用 `SysTick->LOAD/VAL`
  在 1 ms 窗口内插值（`delay_time_now_us` / `delay_time_now_us64`）。
  delay / usart / watchdog / TinyUSB 与 FTL 平台 `time_ms` 全部用它。
- **timer 调度器心跳 = TIM6**：`timer_timx_int_init()` 紧跟 `sys_init()` 启动；
  `TIM6_DAC_IRQHandler` → `timer_scheduler_tick()` 只做 `g_timer_tick_ms++`，
  回调仍在主循环。TIM6 与系统时基是**不同语义**：调度节拍 vs 系统时间。
- **DWT 删除**：`platform_time_us`、NAND 忙期、NAND dispatch 最短忙期、USB
  submit→complete 计时全部改用 SysTick µs 时间戳；字段 `start_cycles` /
  `wait_start_cycles` / `submit_cycles` 改为 `*_us`。
- **不做的事**：不引入 TIM2/TIM5；不把 DWT 留在产品路径。测试框架 `tests/framework`
  的 munit 计时仍可用 DWT（测试基础设施，不是产品固件）。
- 验证：TIM6 1 ms 中断只 `tick++`；SysTick µs 时间戳单调；NAND 忙期超时不退化；
  watchdog、`[STAT]`、GC/WL/RR/CP 节拍不变；H2testw；长时运行无 IWDG 复位。

### Phase 4 — 文档与登记

- `platform_abstraction.md`：btim → timer，表结构与调用点更新；
- `ftl_design.md`：后台调度章节区分「周期（timer）」与「预算（bg_scheduler）」；
- `logging_design.md`：`[STAT]` 的节拍归 timer；
- `known_risks.md`：新增时间基准切换、`mig_handle` 拆分节拍、容量/重入风险；
- `known_iss.md`：更新 ISS-PLAT-04 状态与证据。

### Phase 5 — 后台节拍 flag + token bucket（**已实现 2026-11-03，未上板**）

目标：把「时间节拍」从每轮热路径移到 timer 任务，把 `total_units` 的每轮配额升级为
**时间速率**；must-run 与在飞推进不变。

- **一个后台节拍任务** `ssd_ftl_bg_tick_task`（`main.c`，period = `L2P_FLUSH_INTERVAL_MS`
  = 10 ms）遍历全部 FTL 实例：
  - `bg_scheduler_refill(&ftl->bg)`：按当前档位 `grant_units` 补 credit；
  - `ftl->l2p_flush_due = 1`：L2P 后台写回节拍；
  - `cp_timer_poll(ftl->cp_handle)`：CP T3 到期判定，到期只置 `cp_handle.t3_due`；
  - 每 `BBT_FLUSH_INTERVAL_MS`（1 s，SysTick 判据）置 `alloc.bbt.flush_due`。
- **CP T3**：`cp_should_start` 不再做 `now - last_ms`；只读 `t3_due`。CP 启动（非紧急）
  时清 `t3_due` 并重置 `last_ms`。T1（水位）/ T2（写入量）仍每轮判定。
- **L2P flush**：`bg_task_l2p_flush` 只在 `ftl->l2p_flush_due` 置位时踢；条数仍由
  `max_kick` 限制。
- **BBT**：`bbt_handle` 的在飞路径（waiting / phase != IDLE）不变；只把「起新 job」门
  在 `alloc->bbt.flush_due` 上。无变更时清 flag。
- **Token bucket**：`bg_scheduler_t.credits`；`bg_level_cfg_t.grant_units`（IDLE/NORMAL
  4、BUSY 1）；`BG_TOKEN_INTERVAL_MS=10`、上限 `grant_units × BG_TOKEN_CAP_TICKS(4)`。
  `bg_scheduler_run` 预算段消费 credit，must-run 不占；`BG_ROUND_HARD_LIMIT_US` 仍保留。
- **验证**：上板看 `[GC] trig` 不高于 10 ms/次、WL/RR 仅 IDLE、H2testw 0 错误；
  **重点看 GC/WL 吞吐与 host 写延迟**（token 速率是行为变化）；UART 无
  `migrate timer register failed` / `bg tick task add failed`。
- **调参**：若后台太慢 ⇒ 提高 `grant_units` 或缩短 `BG_TOKEN_INTERVAL_MS`；若 host
  延迟变差 ⇒ 降低 grant。两者改完都要上板重测。

## 6. 不在本计划内

| 项 | 原因 |
|---|---|
| 抢占式调度 | 协作式主循环是数据面 poll-mode 的既有选择；需要抢占时另选 RTOS |
| ISR 上下文回调 | H21 已改为能力判据；本仓无 ISR-safe API 子集，现有回调仍放主循环 |
| µs 级硬实时 | 需要硬件比较中断 + ISR-safe 子集，单独设计 |
| flush/trim/待入账超时 | 事件 deadline，不是周期；继续用「起点 + 每轮比 elapsed」 |
| cache 序列 idle guard | 顺序约束：必须在 host 补发之前跑，留在 `ftl_service` |
| h7r3 | 用户后续删除，本轮不碰 |

## 7. 验证判据汇总

| 层级 | 判据 |
|---|---|
| 编译 | `make all` / `make release` / `make tests UT=FTL`，零新告警 |
| 纯 UT | 一次性/周期/绝对时刻/取消/暂停/改周期/回绕/落后钳位/回调内自取消；注入 `time_ms` / `time_us`（两者的产品来源都是 SysTick） |
| 上板（Phase 1） | watchdog 不复位；`[STAT]` 每 10 s 一行（抖动 ≤1 轮）；CLI `timer` 表项与注册一致 |
| 上板（Phase 2） | `[GC] trig` ≥10 ms/次；WL 5 s、RR 1 s 仅 IDLE；`mv/nop/abort` 正常；H2testw 415/423 MB 0 错误；读写吞吐不低于基线；无 `confirm park timeout` 回归 |
| 上板（Phase 3） | TIM6 1 ms 中断只 `tick++`；30 min 无 IWDG 复位；上述节拍不变 |
| 机械检查 | `grep -rn "btim" src include` 为 0（h7r3 除外）；`timer` 头文件无同名冲突 |

## 8. 风险与登记

- **系统时基 = SysTick，timer 心跳 = TIM6**：`time_ms` / `time_us` 同源 SysTick；
  TIM6 只驱动 timer 任务表。**DWT 删除**，避免把调试/跟踪单元当产品时间源。
  登记到 `known_risks.md`（平台层 RISK-PLAT-09 / RISK-PLAT-12）。
- **`mig_handle` 拆分**：GC/WL/predict 的评估节拍从「handler 内部」变为「调度器行」，
  上板必须确认触发频率与 IDLE 条件不变（GC/性能风险条）。
- **容量 64**：`.bss` 约 +1.8 KiB，需复核 DTCM 预算；不足时降到 48 或静态/动态分池。
- **回调重入**：回调内 `cancel/add` 的边界要写进 `timer.h` 契约；UT 覆盖自取消与新增。
- **顺序**：`timer_poll()` 固定在主循环末尾；任何需要「在 host 补发之前」的步骤不得迁移。

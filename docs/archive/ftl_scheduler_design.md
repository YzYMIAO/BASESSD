# FTL Request Scheduler 架构调整（设计稿，已归档）

> **已归档（2026-11-21）**：本文是 E1~E13 迁移期的设计稿与阶段记录。现行调度器设计、
> 调度策略与代码定位见 `docs/design/ftl_design.md` §3.12.12；本文仅用于追溯迁移过程与
> 阶段决策，不再作为现状依据。


> 状态：**E1~E13 代码已落（scheduler 核心 + L2P + host + 后台 job + lease + completion hook + stats/CLI + bg_policy 完全触发化 + E11 统一分派 + E13 后台仲裁收归 scheduler）；E12 驳回（class credit 不实现，理由见 Phase E12）；22:58 镜像首轮验收通过（H2testw 938/866、0 错误，CDM 随机无 host 错误）；E9/E10 与 E1~E8 组合路径已过 2026-11-21 20:11 镜像首轮上板（启动无 sched corrupt、H2testw 933/866 0 错误、CDM 0.75/0.80、sched 计数闭合），未做掉电/满盘专项。desc/buffer 随 command chain 释放，不单独预租。B/C 上板通过只覆盖过渡路径，不能据此宣称 request scheduler 已验证完成。Phase D 只完成 cache/GC 策略评估：不是瓶颈，不调策略；D 不是本架构的终点**。本文固定“请求调度器”这一层的边界、对象、接口与迁移顺序；
> 代码落地时按 §8 的阶段逐步替换，不把调度重构与 cache/GC 策略调参混在同一批。
>
> 相关文档：
> [`ftl_design.md`](../design/ftl_design.md)（FTL 当前主设计）、
> [`storage_controller_design.md`](../design/storage_controller_design.md)（主控设计要点：队列/仲裁/前后台协同）、
> [`known_open_items.md`](../design/known_open_items.md)（`ISS-FTL-19` 跟踪本项）。

---

## 1. 问题

当前 FTL 没有“请求调度器”，只有两层互不衔接的机制：

- `src/ftl/bg_policy.c/h`：后台任务**预算器**。它决定每轮是否调用
  `l2p_handle` / `md_handle` / `cp_handle` / `mig_handle`，不拥有 host IO、
  FC 资源或请求依赖。
- `src/ftl/ftl.c:ftl_service()`：固定顺序的模块服务链；
  `src/ftl/ftl_op.c`：命令提交与完成派发原语；
  `src/fc/`：FC 命令队列与执行。

模块之间通过**直接函数调用 + bool/FTL_AGAIN 重试**交互，例如：

```text
CP phase1
  → l2p_owner_flush_kick
  → l2p_flush_kick
  → md_slot_pair_append
  → md_handle_is_idle / md_log_rotate
```

任一步拿不到资源就返回 false/`FTL_AGAIN`，由上层模块自行重试。结果是：

- 没有统一的请求队列与完成依赖；
- L2P flush、metadata、CP、GC、host write 互相让路，形成环形等待；
- `no evictable line`、`flush_defer_md_slot`、`CP phase=1` 长期不结束；
- host 随机写绕过 cache sequence，`demand_window` 管不到；
- FC 资源只按 BG/CRIT 两类粗略分配，不能在类内保证读/元数据/回写。

`RISK-CACHE-26` 的板端现场不是单个资源数字问题，而是这一控制面结构问题。

## 2. 目标

引入一个真正的 **FTL request scheduler**，作为 FTL 与 FC 之间的唯一请求/资源仲裁层：

```text
host read/write
  │
  ▼
┌────────────────────────────────────────────┐
│ ftl_sched                                   │
│  ready queues（按 class/priority）          │
│  wait queues（依赖：region / md slot ...）  │
│  class credits（cmd/desc/buffer）           │
│  completion → 唤醒依赖                      │
│  host admission                             │
└────────────────────────────────────────────┘
  │
  ▼
ftl_cmd_lease（命令/描述符/buffer 的唯一申请口）
  │
  ▼
FC submit / completion
```

核心目标：

1. **单一请求所有权**：host IO、L2P load/flush、Journal、CP、metadata、GC 都以请求项进入调度器。
2. **单一资源所有权**：命令/描述符/buffer 由调度器按类租用，模块不再直接调
   `fc_command_alloc_class()`。
3. **依赖而不是重试**：请求可以等待资源/前置请求；完成回调唤醒等待者。
4. **读优先与有界背压**：host read 优先；host write 在高脏水位时被 admission 限制，
   不再无限灌入后由 `drain stalled` 转 `IO_ERROR`。
5. **前后台可解释**：后台不再是固定表序的“函数调用”，而是带类/信用/截止时间的请求。

## 3. 与主流对照

- 队列 + 门铃 + 完成 + 仲裁：`docs/design/storage_controller_design.md` §6.2 / §7.3；
- 读饥饿保护：同文档 §7.4；
- 前台 IO 与 GC/WL 协同：同文档 §7.6；
- DRAM-less FTL 的 L2P/GC/元数据分离：`docs/reference/README.md` A4（SPDK FTL）、
  A1（Cosmos+ OpenSSD）、B2（MQSim）。

本仓的 `bg_policy` 只对应“后台节拍/预算”，缺少主控中**命令调度器**这一层；
本设计补的是这一层，不复制任何具体主控私有实现。

## 3.1 设计评审补充（进入实现前必须先满足）

以下五条是这套调度设计能否落地的边界；不先写清，Phase A 以后会变成“第二个 ftl_op / 第二套 pending”。

### 3.1.1 两层调度与唯一所有权

本设计只新增 **FTL request scheduler**，不替代 FC 命令队列/硬件调度：

| 对象 | 唯一所有者 | 说明 |
|---|---|---|
| host request / work item | `ftl_sched` | 请求级：host IO、L2P、CP、metadata、GC、RR |
| host group / pending 子操作 | `pending` + `ftl_op` | FC 子操作状态、完成计数、request 表 |
| FC command / desc / buffer | `fc` | 物理资源池与命令状态机 |
| cache sequence chain | `ftl_op` / `cache_batch` | 45h/46h/47h、49h/44h/48h 整链原子提交 |

`ftl_sched` 只决定“何时开始一个请求、拿哪一类资源”；不得自己再维护一套
host group / pending 坐标。

### 3.1.2 work item 粒度

| work item | 粒度 | 说明 |
|---|---|---|
| `HOST_READ` / `HOST_WRITE` | 一个 host 组（可能再拆 run） | 不拆到单个 FC 子操作 |
| `L2P_LOAD` | 一个 region | 与 L2P cache 行一一对应 |
| `L2P_FLUSH` | 一个 dirty region | 主/备两份页仍由 L2P 模块串行提交 |
| `CP_META` | 一个 CP phase step / 一个 md 压缩 step | 状态机自持，不把每一步都变成新 item |
| `JOURNAL` | 一次页写/换块擦 | 与 Journal handle 对应 |
| `GC` | 一个迁移 job（可分页推进） | 低优先级，保留信用 |
| `RECOVERY` | 一个 RR job | 高优先级，host 读依赖 |

work item 数量必须有上界（静态数组或固定池），不能按 host 请求数量动态分配。

### 3.1.3 依赖图与无环规则

固定依赖方向：

```text
HOST_WRITE
  → L2P_LOAD（region 未驻留）
  → L2P_FLUSH（dirty eviction）
  → MD_SLOT（L2P log slot / metadata directory）
  ← metadata job / CP 完成释放

CP_META
  → L2P_FLUSH（CP_FLUSH_REGIONS）
  → DIR_CKPT
```

`MD_SLOT` 的生产者是 metadata job / CP 的完成，不是 `L2P_FLUSH`；`L2P_FLUSH`
只是等待者。任何新增依赖必须先证明不会形成环；debug 侧在提交时检测依赖环并停机，
release 侧拒绝提交。

### 3.1.4 guaranteed 与 must-run

当前 `bg_policy` 的 must-run（CP advance、RR、migrate、metadata、Journal erase、
紧急 CP）不能简单改成普通 credit 任务，否则 BUSY 档会先饿死恢复/CP。

设计保留三类：

| 类 | 语义 |
|---|---|
| `GUARANTEED` | 掉电恢复/读完成依赖；每轮有硬保留额度，不被 host/GC 抢占 |
| `RESERVED` | L2P flush / CP / Journal 等；有保底信用，可按水位提升 |
| `BEST_EFFORT` | GC / BBT / WL / 预取；用剩余信用 |

### 3.1.5 cache sequence 原子性

ADR-17 的 cache read/program run 必须整链连续提交；`ftl_sched` 只能决定“何时开始
owner 序列”，不能把 chain 拆成多个独立 work item 让别的类插入。`ftl_op_submit_batch`
与 `fc` 的 chain 语义保持不变，调度器只做准入与资源租约。

### 3.1.6 host admission 语义

USB BOT 单命令下没有真正的多 outstanding 队列；Phase C 前必须二选一并登记：

1. **有界延迟提交**：host 写在高水位时暂缓启动，读仍优先；命令延迟完成；
2. **BUSY 回执**：有界等待后返回 BUSY，由主机重试。

推荐先做 1，避免 CDM/Explorer 把 BUSY 误判成设备错误；Phase C 验收必须覆盖
`usb request timeout` / `drain stalled → IO_ERROR` 的变化。

## 4. 分层与命名

### 4.1 文件布局

| 文件 | 职责 | 状态 |
|---|---|---|
| `src/ftl/ftl_sched.c/h` | 请求调度器：work item、类、队列、credit、lease、completion | 新增 |
| `src/ftl/bg_policy.c/h` | 后台档位/周期触发策略；向 `ftl_sched` 提交后台 work item | 由 `bg_policy.c/h` 改名并收窄 |
| `src/ftl/ftl_op.c` | 提交原语与完成派发 | 保留，不承担调度策略 |
| `src/fc/` | FC 命令队列/硬件执行 | 保留 |
| `src/ftl/ftl.c:ftl_service()` | 服务入口：pump、sched_run、实例维护 | 收窄，不再串联模块业务 |

`bg_policy` 这个名字容易让人误以为它是“FTL 各模块的命令调度”；改名后：

```text
ftl_sched  = 请求/资源调度器
bg_policy  = 后台档位与周期触发策略
```

### 4.2 类型命名

按 H50：结构体/枚举定义处带 `typedef` 与 tag，alias 小写。

```c
typedef enum FTL_WORK_CLASS_E
{
    FTL_WORK_CLASS_HOST_READ = 0,
    FTL_WORK_CLASS_L2P_LOAD,
    FTL_WORK_CLASS_L2P_FLUSH,
    FTL_WORK_CLASS_CP_META,
    FTL_WORK_CLASS_JOURNAL,
    FTL_WORK_CLASS_HOST_WRITE,
    FTL_WORK_CLASS_GC,
    FTL_WORK_CLASS_RECOVERY,
    FTL_WORK_CLASS_NUM
} ftl_work_class_e;

typedef enum FTL_WORK_STATE_E
{
    FTL_WORK_PENDING = 0,
    FTL_WORK_READY,
    FTL_WORK_WAIT_DEP,
    FTL_WORK_INFLIGHT,
    FTL_WORK_DONE
} ftl_work_state_e;
```

具体字段在实现时按最小集合落地，不在本设计稿复制完整头文件。

## 5. 对象模型

### 5.1 请求项（work item）

一个请求项表达“要做的一件事”，而不是“调用哪个函数”：

- **class**：资源/优先级类别；
- **state**：待处理 / 就绪 / 等依赖 / 在飞 / 完成；
- **dependency mask**：等待哪些资源或前置请求（如 `MD_SLOT`、`L2P_FLUSH`）；
- **deadline / retry**：有界等待，不用无限 bool 重试；
- **context**：host io、L2P line、metadata page、GC page 等；
- **completion**：由 FC 完成回调或前置请求完成触发。

### 5.2 类与方向

建议初始 classes（优先级从高到低，具体数值后续按板端调）：

| class | 内容 | 说明 |
|---|---|---|
| `HOST_READ` | 主机读子操作 | 读优先；不能被写/GC 无界推迟 |
| `L2P_LOAD` | confirm/GC 依赖的 region 加载 | 与 L2P flush 同属映射推进 |
| `L2P_FLUSH` | dirty region 写回 | 驱动 eviction，必须能拿到保底资源 |
| `CP_META` | CP / 目录检查点 / metadata job | 掉电恢复推进 |
| `JOURNAL` | Journal 页写/擦 | WAL 次序前置 |
| `HOST_WRITE` | 主机写（wbuf / cache sequence） | 可被 admission 限制 |
| `GC` | GC/WL 搬移 | 后台低优先级，有保留信用 |
| `RECOVERY` | Read Recovery | 主机读依赖，取高优先级 |

不把类名绑定到具体 FC opcode；类属于 FTL 调度层，FC 只执行。

### 5.3 资源租约（lease）

资源申请只有一个入口：

```c
ftl_status_e ftl_cmd_lease_acquire(ftl_work_class_e cls, BYTE cmd_n,
                                   BYTE desc_n, BYTE buf_n,
                                   ftl_cmd_lease_s *out);
void         ftl_cmd_lease_release(ftl_cmd_lease_s *lease);
```

约束：

- 模块不直接 `fc_command_alloc_class()` / 直接建链取 buffer；
- lease 与请求绑定，完成/失败统一释放；
- 每类有保留额度，BG 不能吃掉 L2P/CP 的保底；
- 资源不足时请求进入 `WAIT_DEP`，不是模块内 `while` 重试。

### 5.4 依赖图

首批依赖关系：

```text
HOST_WRITE（cache/wbuf）
  → L2P_LOAD（region 未驻留）
  → L2P_FLUSH（dirty line eviction）
  → MD_SLOT（L2P log slot）
  → CP_META / metadata job（目录检查点 / compaction）

CP_META
  → L2P_FLUSH（CP_FLUSH_REGIONS）
  → DIR_CKPT
```

调度器负责在依赖完成时唤醒；`md_slot_pair_append` 不再用 bool 让上层 CP 重试。
metadata slot 不足是一个等待事件，不是“调用失败”。

## 6. 调度策略

### 6.1 主机入口

- host read：直接进入 `HOST_READ` ready queue，优先取资源；
- host write：先过 admission：
  - class credit 不足 / host write queue 满；
  - L2P dirty 行数或 CMT dirty 超水位；
  - `evict_fail` / `flush_defer_*` 超阈值；
  - 任一条件成立 ⇒ 不启动新写，保留请求在 HIL/FTL 队列；
- USB BOT 单命令模型下，host 看到的是**延迟提交/BUSY**，不是 FTL 内部
  `drain stalled → IO_ERROR`。

admission 的 host 侧语义需要在实现时二选一并登记：
1. HIL 暂缓提交（命令延迟完成）；
2. 有界等待后回 BUSY（主机重试）。

### 6.2 后台

`bg_policy` 只做：

- `IDLE/NORMAL/BUSY` 档位；
- 周期触发 CP timer、GC evaluate、WL、BBT、Journal erase、RR；
- 把触发结果提交为 `ftl_sched` 的 work item；
- 不直接调用模块 handle 去抢 FC 资源。

后台信用由 class credit 保障；`must-run` 的语义改为“保留信用 + 可抢占的空闲信用”，
不再靠固定表序。

### 6.3 读优先

- read 有独立 class credit；
- 写/GC 消耗到阈值后必须给 read 让路；
- read 的完成不能被 L2P dirty flush 或 metadata compaction 无界阻塞；
- 读超时由调度器的 deadline 暴露，不由 host 协议层猜测。

## 7. 不变量

1. **唯一申请口**：只有 `ftl_sched` 能调用 FC 资源分配；模块只拿 lease。
2. **唯一完成口**：FC 完成先进 `ftl_sched_complete`，再由调度器唤醒依赖。
3. **请求不丢**：任何 `FTL_AGAIN` 都有队列/状态承载，不在调用栈里丢请求。
4. **有界等待**：每个等待都有 deadline；超时是请求错误，不是永久挂起。
5. **读写分离**：host read 不被 host write/GC 的信用耗尽无界阻塞。
6. **依赖单向**：依赖图不允许环；出现环是设计错误，debug 断言、release 拒绝提交。
7. **资源守恒**：acquire/release 配对，按 class 统计；不靠“池最终会回收”兜底。

## 8. 迁移阶段

每阶段独立提交、独立构建；涉及地址/位图/FC 资源的阶段按 R2 上板。前阶段未通过不得进入下一阶段。

### Phase A0：新增 `ftl_sched` 骨架（不改行为）

> 当前状态：A0 的 work item / class credit / lease 骨架在代码审视中已删除——它没有任何真实提交者，`submit()` 返回成功后无人执行，属于“能入队不执行”的死接口。A0 交付物未完成；接入 host/GC 请求时必须重新实现并与实际提交点同时落地。

**目标**：建立唯一请求/资源接口，不改变现有执行路径。

交付物：

- `src/ftl/ftl_sched.h/c`：
  - `ftl_work_class_e` / `ftl_work_state_e` / `ftl_work_item_s`；
  - `ftl_sched_s`（静态队列、class credit、busy mask）；
  - `ftl_sched_init` / `ftl_sched_submit` / `ftl_sched_complete` / `ftl_sched_run` 的空实现或直通实现；
  - `ftl_cmd_lease_acquire/release` 先包装现有 `fc_command_alloc_class`，不改变调用点；
- `ftl_service` 末尾调用 `ftl_sched_run(ftl)`，其实现先为空；
- 不把任何现有模块切到新接口；不改变 `bg_policy`、`ftl_op`、`pending` 行为。

退出判据：

- `make all` / `make release` 通过；
- 现有 H2testw 顺序路径保持原结果（未上板则明确标注）；
- 无新增告警；`ftl_sched` 未产生实际命令。

### Phase A1：命名与模块边界（已完成）

**目标**：后台预算器统一命名为 `bg_policy`，明确它不是 FTL 请求调度器。

- `src/ftl/bg_policy.c/h` 承担后台档位/触发策略；
- `bg_policy_s` / `bg_policy_*` / `BG_POLICY_TASK_*` 统一命名；
- `ftl.h` / `ftl.c` / `ftl_type.h` / timer 适配点同步；
- `docs/design/ftl_design.md` §3.12 与新设计文档链接同步；
- **冻结测试源**仍可能 include/调用旧名，按 R4 如实报告，不修改测试源、不加兼容垫片；
  测试源重建时再同步。

退出判据：

- 语义/布局不变；`make all` / `make release` 通过；
- release `.text`/`.data` 不因纯改名改变（除调试字符串除外）。

### Phase B：先搬失败链（L2P_FLUSH / MD_SLOT / CP）

> 当前状态：代码用 **L2P bitmap + `ftl_sched_run` 轮询**完成 CP 写回，**不是**下面的 work item / `WAIT_DEP` / `ftl_sched_complete` 形态。它解决了 B2/C 的现场死锁，但没有完成本阶段的请求对象化；后续接入 host/GC 时仍需补 work item/credit/lease。

**目标**：拆掉当前 `CP → L2P flush → md_slot` 的同步 bool 重试。

交付物：

- `L2P_FLUSH` 成为 work item；资源不足进入 `WAIT_DEP(MD_SLOT)`，不是返回 false 让 CP 重试；
- metadata job 完成时由 `ftl_sched_complete` 唤醒等待的 `L2P_FLUSH`；
- CP 不直接调用 `l2p_owner_flush_kick` 并 10ms abort；只提交/等待 `CP_META` 请求；
- 保留 `pending`/`ftl_op` 的 FC 子操作状态，不新建第二套 group 坐标；
- cache sequence chain 仍整链提交，调度器只控制起点。

退出判据：

- 随机写（CDM9 5 线程 / 128MiB / format 后）无持续 `no evictable line`；
- `flush_defer_md_slot` / `evict_fail` 显著下降（以当前 `RISK-CACHE-26` 现场为基线）；
- CP `phase=1` 不再出现百秒级不结束；
- 顺序 H2testw 不回归。

### Phase C：host IO 与 admission

**目标**：把 host read/write 纳入 scheduler，切断“无限灌 dirty”。

交付物：

- host read 进入 `HOST_READ` 高优先级队列；
- host write 进入 `HOST_WRITE`，先过 admission（class credit / dirty 水位 / host queue 深度）；
- 读优先：写/GC 达阈值后必须让读；
- USB BOT 语义按 §3.1.6 选择“有界延迟提交”并登记；
- HIL `host_queue` 与 `ftl_sched` 的请求生命周期明确（同一请求只释放一次）。

退出判据：

- 同一随机写无 `hil timeout` / `drain stalled → IO_ERROR`；
- host 读在写压力下仍有有界延迟；
- 顺序 H2testw 不回归；速度不低于同条件基线。

### Phase D：GC / cache / metadata 策略

**目标**：在调度器之外做策略优化，不把策略塞进引擎。

- GC 作为低优先级 `BEST_EFFORT`，有保留信用；GC 触发/选 victim 仍归 migrate；
- L2P cache/CMT 容量、write policy、metadata log 目标块按 `RISK-CACHE-26` 复测结果评估；
- checkpoint/metadata 的日志区水位、压缩目标块策略与调度器 dependency 解耦；
- 不在本阶段改介质格式/地址坐标。

**评估结论（2026-11-21，基于 17:40 板端 `stats`，未新增测试）**：

- **L2P cache 没有压力**：随机写结束时 `hit_count=657,466` / `miss_count=7,482`
  （行命中 98.9%），`load_start=8,435`、`load_lat_us max=7.8ms`、
  `cmt_evict_dirty=88`、`evict_fail=0`、`cmt_wb_defer=35`；再跑完 H2testw 后
  `hit_count=2,813,368` / `miss_count=450`（行命中 99.98%）、`cmt_evict_dirty=0`。
  扩大 64 行 cache 或 CMT 没有数据支持。
- **GC 没有压力**：`gc.moved=423`、`noop/nogain/novictim=0`、`free_count=50`
  （远高于紧急水位 8）；GC 不是该轮吞吐的限制项。
- **metadata/CP 不再有死锁**：无 `CP abort`、无 `no evictable`、无 `hil timeout`；
  CP job `max=1.58s`，但主机吞吐仍由传输层限制（见下），不是 CP 单独造成的。
- **主机侧是上限**：H2testw `944 KB/s` 已接近 USB 2.0 Full-Speed 理论 1.5 MB/s；
  单个 4 KiB 数据在 12 Mbit/s 总线上仅传输就需 ~2.7 ms。`hil.cmd_us` 无
  `>50ms` 样本、`sys.loop` 无长卡顿；内部历史基准 `/bench/speed/ftl_rw`
  的 FTL 读写约 3.3 MB/s，明显高于 USB FS 上限。⇒ CDM 的 4 KiB 随机
  `0.65 MB/s` 不能再归因于 FTL cache/GC 策略；继续调 cache/CMT/GC 会在错误的层
  调参。
- **不做**：容量重排（`s_l2p_cache` 已占 AXI 大半）、CMT 水位调参、GC 权重调参。
  只有在出现**内部随机 4 KiB 基准**且显示 FTL 自身低于传输层时，才重开本阶段。

退出判据：

- 随机写吞吐提升有可解释归因；若归因证明瓶颈在 host 传输层，则本条以
  「FTL 非瓶颈 + 证据」结案，不要求 FTL 侧提速；
- 顺序 H2testw 0 错误；
- 掉电恢复（涉及 CP/metadata 时）按 `docs/design/common/fault_capture.md` 取证。

### Phase E1：scheduler 核心 + 第一个真实消费者 `L2P_FLUSH`

> 当前状态：代码已落（work item 静态池、class credit、CP 提交、run 启动/回收、
> `WAIT_DEP(MD_SLOT)`），未上板。FC 物理 lease 按本阶段边界**不在 E1 伪造**。

**目标**：把“能入队不执行”的空壳换成有真实生产者/消费者的核心，只用 L2P 写回验证。

- work item 静态池（容量按在飞上限 + 背压余量定，CP 的 snapshot 位图继续充当 backlog）；
- item 状态 `FREE / READY / WAIT_DEP / INFLIGHT`，`dep_mask` 支持 `MD_SLOT`；
- `L2P_FLUSH` class 有独立 credit；资源不足进 `WAIT_DEP`，不是模块内 while 重试；
- CP 把 dirty region 提交为 item；`ftl_sched_run` 按 item 启动
  `l2p_region_flush_kick`；完成由 scheduler 观察脏位清零后回收 item；
- **删除 scheduler 自己的 `l2p_flush_bitmap`**，避免与 item 状态形成第二套坐标；
- FC command/desc/buffer 的物理 lease 不在 E1 伪造：现有 builder 仍直接取池，
  E2 引入可接收预留的 lease 后再把 L2P load/flush 的取池点改过去。

退出判据：

- 行为与当前 B/C 等价：H2testw 0 错误、无 CP abort/no evictable/host timeout；
- `grep` 证明 scheduler 不再有 bitmap 第二坐标；
- `make all` / `make release`；上板一次（同时验证当前 admission 阈值）。

### Phase E2：`L2P_LOAD` + FC command lease

> 当前状态：代码已落 **demand `L2P_LOAD` item + command lease**。load 的启动仍在
> L2P 模块（scheduler 负责 class credit 准入与完成结算，不另建 READY 队列）；
> desc/buffer 的物理预留留到 E6，避免在 E2 伪造“全资源 lease”。

- demand load 成为 `L2P_LOAD` work item（预取在 E10 纳入同一 class）；
- `ftl_cmd_lease_acquire/release/take` 先只租 command slot；
- L2P load 的 command 不再直接 `fc_command_alloc_class()`；
- 完成统一到 `ftl_sched_l2p_load_complete()`，失败重试/备份副本期间 item 保持；
- desc/buffer 的 lease 扩展在 E6 收口。

### Phase E3：`HOST_READ` / `HOST_WRITE`

> 当前状态：代码已落 host group item；读组先于写组补发，写 admission 保持原判据。
> host item 总数与 pending 组表（32）同量级；另外给读保留 4 个组槽（写组最多
> `32-4=28`），这是本仓自拟的读写隔离，不是行业值。`pending` 仍拥有组状态，
> scheduler 只拥有调度坐标。

- host 组成为请求项；`HOST_WRITE` 仍走 admission；
- `HOST_READ` 优先于 `HOST_WRITE`，并有 4 个保留组槽；
- `pending`/`ftl_op` 仍只拥有 FC 子操作，不建第二套 group 坐标。

### Phase E4：`CP_META` / `JOURNAL` / `MD_SLOT`

> 当前状态：代码已落。`bg_policy` 的 CP advance/emergency/trigger、metadata、Journal
> erase/flush 六个适配器只调用 `ftl_sched_bg_request()`；执行由
> `ftl_sched_run_bg()` 完成，并且每个任务执行后立即 run，保持「表序 = 执行序」。
> RR/migrate/BBT/L2P 后台 flush 仍直接调用，留到后续阶段。
> `WAIT_DEP(MD_SLOT)` 在 metadata job 执行后的同一轮 `ftl_sched_run` 中由 scheduler
> 观察 `md_handle_is_idle + !compact_wanted` 唤醒。

- CP step、metadata、Journal erase/flush 成为后台 job work item；
- `bg_policy` 只产生触发并提交，不再直接调 `cp_handle` / `md_handle` / `jrn_handle`；
- `MD_SLOT` 依赖由 `WAIT_DEP` 表示，scheduler 在 metadata step 后唤醒。

### Phase E5：`GC` / `RECOVERY`

> 当前状态：代码已落。`bg_policy` 的 RR / migrate 适配器改为提交 `RECOVERY` / `GC`
> job；`ftl_sched_run_bg()` 执行，RR 仍排在 migrate 之前。
> migrate 同时承载写路径紧急 GC，仍是 must-run（不能降成会被预算饿死的普通 credit）；
> 自愿 GC 的节拍/收益判断仍在 migrate 内部，并按 `bg.level` 执行。
> BBT / L2P 后台 CMT flush 仍直接调用，归后续收口。

- GC/WL 迁移进入 `GC` class；RR 进入 `RECOVERY` class；
- RR 先于 migrate 执行；
- 写路径紧急 GC 仍每轮有确定推进，不被 host 预算饿死。

### Phase E6：lease 全面收口 + 唯一完成口

> 当前状态：代码已落 **command lease 单一漏斗**。FTL 内所有命令分配经
> `ftl_cmd_alloc_class()` → `ftl_cmd_lease_acquire()`；命令在建链时产生的
> desc/buffer 属于该 command chain，随 `fc_command_release()` 一起释放，因此不再
> 单独预租 desc/buffer。FC 完成由 `ftl_op_drain()` 在模块数据收尾后统一调用
> `ftl_sched_note_completion()`：回收 host/L2P item 并唤醒 `WAIT_DEP(MD_SLOT)`。
> 模块数据面仍先于 scheduler 执行（scheduler 不拥有 page buffer/映射数据）。

- FTL 命令分配统一走 lease；`grep` 证明 `fc_command_alloc_class()` 只出现在 lease 实现；
- desc/buffer 生命周期绑定 command，不单独预租；
- FC 完成统一进入 scheduler 完成钩子，回收 item / 唤醒依赖；
- 模块内遗留的资源 while 重试仍按后续审查逐个收口。

### Phase E7：观测与验收

> 当前状态：代码已落 `ftl_ctx sched` 与 `stats ftl.sched` 计数；板端验收未做。
> `ftl_ctx sched` 输出各 class 活动数、WAIT_DEP、credit、host_running、
> flush_failed；`stats ftl.sched` 记录 submit/reap/completion 计数。

- `ftl_ctx sched`：队列深度、class credit、in-flight、WAIT_DEP；
- 统计 `host_begin/host_reaped/load_begin/load_complete/flush_request/flush_reaped/bg_request/completion_note`；
- 板端验收矩阵：
  - `make all` / `make release`；
  - format 后顺序 H2testw 415/423、0 错误；
  - CDM 随机写不出现 `hil timeout` / `drain stalled → IO_ERROR`；
  - 无 CP abort / no evictable；
  - `stats ftl.sched` 的 begin/reaped 收敛，`ftl_ctx sched` 无长期 WAIT_DEP 堆积；
  - 涉及 CP/metadata 的掉电恢复按 `docs/design/common/fault_capture.md` 取证。
- UT 解冻后补 scheduler 用例与负例。

### Phase E8：`bg_policy` 完全触发化

> 当前状态：代码已落。`bg_policy` 的 10 个任务适配器全部只调用
> `ftl_sched_bg_request()`；后台 job 用 `bg_job` 标志与普通 `L2P_FLUSH` region
> item 区分，L2P CMT flush 与 BBT 也由 `ftl_sched_run_bg()` 执行。

- BBT / L2P 后台 CMT flush 进入后台 job；
- `bg_policy` 不再直接调用任何模块 handle；
- `bg_job` 标志隔离后台 job 与普通 region flush。
- 退出判据：`grep` 证明 `bg_policy.c` 无 `cp_handle/md_handle/jrn_handle/mig_handle/rr_handle/bbt_handle/l2p_handle` 调用；构建通过。

### Phase E9：每轮开销收敛

> 当前状态：代码已落。目标是消除每次 service 的 item 分配/释放与全表扫描，
> 不改变调度语义；性能效果与启动修复均**未上板验证**。

- 后台 job 使用 item 池前 `FTL_BG_JOB_NUM` 个**永久预留项**，`ftl_sched_bg_request()`
  只把预留项置 READY，不再分配；池满路径消失，返回类型改为 `void`。
- `host_items` / `flush_items` 计数由唯一释放漏斗 `ftl_sched_item_release()`
  维护；`ftl_sched_l2p_flush_pending()`、`ftl_sched_reap()`、
  `ftl_sched_host_reap()`、`ftl_sched_run()`、`ftl_sched_run_host()` 在对应
  计数为 0 时 O(1) 返回，不再每轮扫 128 项。
- `bg_policy_run` 先提交全部 must-run 任务再执行一次 `ftl_sched_run_bg()`，
  预算任务同理；表序不变，但每轮扫描次数从「每任务一次」降为「每阶段一次」。
- 退出判据：`bg_policy` 无逐任务 `run_bg`；`stats ftl.sched` 的
  `bg_request` 绝对值不再随 service 次数膨胀；`make all` / `make release`
  零新告警。计数是 release 活分支，**必须上板**确认无 `sched corrupt`、
  H2testw 0 错误且速度不回退。

### Phase E10：预取纳入 `L2P_LOAD` 调度

> 当前状态：2026-11-21 20:11 镜像首轮上板通过；预取原本在 L2P 内部直接 `l2p_load_begin` 提交
> BG 读命令，既不进 scheduler credit，也不出现在 `stats ftl.sched`，属于
> 附录 A「`l2p_region_preload` → `L2P_LOAD`」映射的最后一个旁路。

- 预取加载登记 `L2P_LOAD` item，带 `prefetch` 标志；demand 与预取**分开计
  credit**：demand 仍为 8，预取新增 `FTL_SCHED_L2P_PREFETCH_CREDIT = 2`，
  预取永远不能占用 demand 的保底额度。
- `ftl_sched_l2p_load_begin/complete()` 增加 `prefetch` 参数并按
  `region + prefetch` 匹配 item；同一 region 的 demand 与预取互不吞 item。
- L2P 完成回调在进入失败结算前捕获 `line->prefetch`：`l2p_load_fail_settle`
  会清掉行标志，用清后的值结算会漏释放 item。
- 未映射直接合成 CLEAN / 提交失败没有异步完成 ⇒ 当场释放 item，与 demand 同构。
- 观测：`stats ftl.sched` 增加 `prefetch_begin/prefetch_complete`，
  `ftl_ctx sched` 增加 `pf=` 活动预取 item 数。
- 代价：`ftl_work_item_s` 16 → 20 B（两 die 共 +1 KiB SRAM3）。
- 退出判据：`l2p_region_prefetch` 不再直接调用 `l2p_load_begin`；
  `stats ftl.sched` 的 `prefetch_begin` 随顺序读增长；H2testw 速度不回退
  （预取仍受 2 个独立 credit + 原 `L2P_PREFETCH_MAX_INFLIGHT` / host budget 门控）。

### Phase E11 + E13：统一分派与后台仲裁收归 scheduler

> 当前状态：代码已落，未上板。E11 把 `ftl_sched_run_bg()` + `ftl_sched_run()` 合并为
> `ftl_sched_dispatch()`；E13 让 `bg_policy` 只提交，执行顺序由 `FTL_SCHED_PRIO_E` 决定。

- 新增 `FTL_SCHED_PRIO_E` 作为唯一顺序来源：
  `HOST_READ → HOST_WRITE → BG_BASE + job（`ftl_bg_job_e` 表序）→ L2P_FLUSH`。
  改 QoS 只改这张表。
- 顺序只由 `FTL_SCHED_PRIO_E` / `ftl_bg_job_e` 推导，item **不新增坐标**（class/job
  已编码类别）；`_Static_assert` 钉住表内相对顺序，item 仍 20 B。
- `ftl_sched_dispatch()`：host 读 → host 写（带重入保护，`host_items==0` 不扫表）→
  后台 job 表序 → L2P flush；`bg_suspend` 时只放行 host。
- `ftl_sched_run_host()` 保留为**提交快路径**的 host-only 入口（`ftl_submit_common`
  使用），避免提交时顺带执行后台。
- `bg_policy_run()` 不再调用 `ftl_sched_run_bg()`；档位、任务掩码、token bucket
  仍是策略层，只决定**提交**哪些 job；执行顺序归 scheduler。
- 行为边界：host 仍在 `ftl_service` 原位派发（早于 seq close / wbuf idle flush）；
  后台与 L2P flush 在 `bg_policy_service` 之后统一执行。⇒ 不是“一次调用覆盖全轮”，
  而是 host 快路径 + 统一后台分派；这样保住现有 host 序列与后台时序。
- 退出判据：CP/RR 时序无回归；`bg_request` 变化可解释；H2testw/CDM 不低于
  2026-11-21 20:11 镜像（933/866、0.75/0.80）。

### Phase E12：驳回（class credit 不实现）

> 不实现“每个 class 一套 credit”。

- 后台 job 是单实例状态机，credit 只能取 0/1，不 gate 任何并发；
- host 已有组槽上限 + admission；
- 真正的共享资源信用已在 FC 池的 BG/CRIT 额度。再建一套 scheduler class credit
  是同一资源的第二套坐标（C9），两处调参会漂移。
- 若以后需要 QoS：扩展 FC 池 class 额度（资源信用），或 scheduler 的每轮 dispatch
  预算（仲裁配额，不叫 credit）。

### 阶段门禁

- 每个阶段只改一个控制面边界；不把 B/C/D/E 混在同一提交；
- **每个阶段必须同时有真实生产者与真实消费者**；禁止再保留“submit 成功但无人执行”的接口；
- 每阶段报告：改动文件、失败归因计数变化、构建证据、上板证据（或明确“未上板”）；
- **任何阶段都不得用调 timeout / 加 reserve 代替调度结构变化**。

## 9. 验收判据

### 顺序路径

- H2testw 415/423 MB，0 错误；
- `confirm park timeout` = 0；
- 速度不低于同条件基线；
- `complete_err` / `bad_status` 不增加。

### 随机写路径

条件：CDM9 / 5 线程 / 128–256 MiB / format 后干净盘。

- 不再出现持续 `no evictable line`；
- `CP phase=1` 不出现百秒级不结束；
- `hil timeout`、`drain stalled → IO_ERROR` 为 0；
- 吞吐较当前 0.84/0.51 MB/s 有可解释提升；
- `load_fail_no_buf` / `flush_defer_md_slot` / `evict_fail` 作为归因指标保留。

### 回归

- 顺序 H2testw 与现有发布基线对比；
- 掉电恢复（若改动涉及 CP/metadata）按 `docs/design/common/fault_capture.md` 取证；
- 不修改测试源来适配 API 变化；冻结测试源调用旧 API 时如实报告。

## 10. 非目标 / 边界

- **不**在本项中引入 NVMe 多队列/门铃；USB BOT 仍是单命令模型；
- **不**改变介质格式、地址坐标、L2P region 尺寸；
- **不**把 cache/GC 策略调参混进 scheduler；
- **不**为了兼容冻结测试源回退生产 API 或加垫片；
- **不**声称“主流标准实现”——本仓具体队列/信用数值是本仓自拟，主流依据见 §3。

## 11. 待决策

0. **跨 die / 多 LUN 仲裁**：结论是**不新增设备级 arbiter**。FC 池已定稿并承担跨 die
   共享资源仲裁；FTL scheduler 保持每 die 一份，只做本 die 内顺序。离线出处不足以
   引用主流的多 die FTL 请求调度形态（NVMe 仲裁是主机队列级；Cosmos+ 仓库未克隆），
   按 §〇 不自拟。记录见 `known_open_items.md` ISS-FTL-19。

1. host 写 admission 的可见语义：延迟提交还是 BUSY（影响 USB/CDM 表现）。
2. 首批 class credit 数值与保留量；需按板端测量调整，不写死成“行业值”。
3. `bg_policy` 是否保留现有 10 项表作为触发器，还是拆成事件源。
4. Phase B 是否先只搬 L2P/metadata，还是连同 Journal/CP 一起。
5. `ftl_op` 与 `ftl_sched` 的接口边界：FC 完成回调用哪一个入口统一登记。

---

## 附录 A：当前模块 → 目标请求类

| 当前入口 | 目标归属 |
|---|---|
| `ftl_submit_read` / `ftl_drain_group` 读 | `HOST_READ` |
| `ftl_submit_write` / `wbuf` / cache write | `HOST_WRITE` |
| `l2p_load_start` / `l2p_region_preload` | `L2P_LOAD` |
| `l2p_flush_kick` / `l2p_owner_flush_kick` | `L2P_FLUSH` |
| `jrn_append` / Journal 页写/擦 | `JOURNAL` |
| `cp_regions_flush` / 目录检查点 / Header | `CP_META` |
| `md_handle` 压缩/回收 | `CP_META` |
| `mig_handle` GC/WL/predict | `GC` |
| `rr_handle` | `RECOVERY` |

## 附录 B：参考位置

- `src/ftl/bg_policy.c/h`：当前后台预算器（未来 `bg_policy`）；
- `src/ftl/ftl.c:ftl_service`：当前固定服务链；
- `src/ftl/ftl_op.c`：提交/完成派发原语；
- `src/ftl/l2p_cache.c:l2p_flush_kick` / `l2p_line_evict`：dirty 驱逐；
- `src/ftl/metadata.c:md_slot_pair_append` / `md_log_rotate`：metadata slot/日志区；
- `src/ftl/checkpoint.c:cp_job_step`：CP 相位；
- `src/fc/fc_pool.c`：FC 命令/desc/buffer 池；
- `docs/design/storage_controller_design.md` §6.2 / §7.3 / §7.4 / §7.6；
- `docs/reference/README.md` A4 / A1 / B2。

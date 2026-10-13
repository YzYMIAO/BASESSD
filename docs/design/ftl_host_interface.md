# FTL 对外接口：主流对照、本仓落地与接口级 UT 设计

> 判据 = **有出处的主流存储主控 FTL 设计**，不是本仓现状。凡与主流不一致且可改的，本方案改。
> 出处：`fc_nand_design.md` §26.1/§26.2（HIL / FTL / FCC 分层）。
> 本文回答四个问题：**主流里 FTL 的对外接口是什么、外部传什么给 FTL、本仓怎么做、如何设计 UT**。

---

## Part A — 分析

### A.1 主流里 FTL 对外接口是什么 / 外部传什么

行业分层：

```
Host ──► HIL（协议解析、队列、doorbell、命令/数据搬运的发起）
      ──► FTL（L2P / GC / WL / 坏块 / 掉电恢复 / 缓冲调度）
      ──► FCC（硬件：通道/CE/LUN/plane 时序、描述符链、ECC、加扰、DMA）
      ──► NAND
```

HIL 交给 FTL 的是**一条已解析的请求**（§26.1 原文）：

> 设备侧固件拿到的是一条**已解析的请求**（目标 LBA/地址 + 数据缓冲指针 + 操作类型），
> **访问哪些物理单元、数据怎么搬都是固件的事**。

| 传给 FTL | NVMe | eMMC CMDQ | 本仓对应 |
|---|---|---|---|
| 操作类型 | opcode | Task Descriptor 方向位 | 调用哪个 `ftl_submit_*` / `ssd_request_opcode_t` |
| 目标地址 | SLBA | 起始块地址 | `lba` |
| **长度** | NLB | 块数 | **`count`** |
| 数据缓冲 | PRP/SGL | 描述符内地址 | `buffer` |
| 完成回执 | CQ + MSI-X | Task 完成 + 中断 | `complete_flag` / `status_out` / `on_complete` |

**FTL 自己决定**：单/双 plane 配对、通道/CE/LUN 交错并行、页/块分配、GC 时机、L2P 策略——
HIL 一概不知（§26.4："越靠近硬件，接口越只谈机制；策略越往上放"）。

**关键推论**：multi-plane 配对正是"访问哪些物理单元"⇒ **属 FTL，不属 HIL**。

### A.2 本仓现状（重构前）

```
UART CLI (src/controller/cli.c) → ssd_request_t{opcode, start_lba, block_count, buffer, buffer_size, ...}
  → ssd_controller_submit()  (src/controller/controller.c)
      ├─ block_count==1 → ftl_submit_read/write(ftl, io{pages=1})
      ├─ block_count>1  → ssd_request_split()  (src/controller/request.c)
      │      ★ HIL 决定 plane 配对：连续两 LBA → pages=2 Multi-Plane
      │      ★ HIL 子请求聚合：on_sub_complete / sub_remaining（有提前完成缺陷）
      ├─ TRIM  → ftl_trim(ftl, start_lba)   ★ 单 LBA，忽略 block_count
      └─ FLUSH → 直接 l2p_cache_flush_all(&fftl->cache)+journal_flush_page(&ftl->journal)
                 ★ HIL 伸手进 FTL 内部（layering violation）
FTL 对外：ftl_init / ftl_service / ftl_submit_read / ftl_submit_write / ftl_trim（无 ftl_flush）
入参 ftl_io_request_t{lba, buffer, pages, complete_flag, status_out, on_complete, on_complete_ctx}
```

### A.3 差异表（本仓 vs 主流）

| # | 项 | 主流 | 本仓（重构前） | 处置 |
|---|---|---|---|---|
| **D1** | **plane 配对（访问哪些物理单元）** | FTL 内部策略 | **HIL 的 `ssd_request_split` 决定 `pages=2`** | **已下沉到 FTL** |
| **D2** | **FLUSH 落地** | FTL 的一条 op / `ftl_flush` | **HIL 直接调 FTL 内部 `l2p_cache_flush_all`/`journal_flush_page`** | **已新增 `ftl_flush(ftl)`** |
| **D3** | TRIM 粒度 | 带长度的 deallocate | `ftl_trim(ftl,lba)` 单 LBA（忽略 `block_count`） | **已改 `ftl_trim(ftl,lba,count)`** |
| **D4** | 请求 vs 通知 | 请求纯输入；完成走 CQ | 通知字段（`complete_flag`/`status_out`/`on_complete`）与请求混在一起 | **不改**（本仓无 CQ，轮询+回调是既定异步契约；登记） |
| **D5** | Host 队列+doorbell+CQ | HIL 有 SQ/CQ/doorbell | **HIL 无队列**；队列机制只在 **FC 内部**（SQ64/HSQ16/CQ64） | **不改**（§26.2 已定位在 FC；HIL 侧属更大重构，登记） |
| **D6** | LBA→实例路由 | ⚠ **本仓断言，无出处**（见下方 D6 注） | Controller 按 LBA 路由到 `ftl[ch][ce][lun]` 分片 | **保留**（分片是刻意模型；路由=后端选择，非 plane 策略） |

> **D6 注（v0.65 查证）**：本行「主流」列原写「单一 FTL 拥有全 LBA 空间」，但该主张
> **无外部出处**——它只出现在本仓自己的 D6 对照表里；唯一被援引的
> `fc_nand_design.md §26` 通篇不谈 L2P 在通道/die 间的分布。仓内甚至还有**反方向**的
> 同类无出处断言（`ftl_revision_history.md v0.35`：「行业标准：每 LUN 独立 region 表」）。
> 按「判据必须有出处」⇒ **不得**据此改代码；分片模型保留（见 §登记）。
> 另：`CH_NUM=CE_NUM=LUN_NUM=1` ⇒ 今天只有一个 FTL 实例，「单一前端」性质平凡成立。

### A.4 顺带修掉的真缺陷

1. **大请求会崩（且提前完成）**：旧 `ssd_request_split` 同步循环提交，`ftl_submit_*` 中途失败时
   置 `complete_flag=1`，而 `sub_remaining` 已被预置为全量 ⇒ **父请求提前完成、余下计数永不归零**。
   真实容量瓶颈是 **FC buffer 池**（128 buffer ÷ 每多 plane 命令 8 buffer = **16** 个并发子操作），
   `wr 0 40` 旧实现必失败。重构后：FTL 自拆分 + 按需 drain，瞬态背压（`FTL_AGAIN`）排队重试。
2. **CLI 谎报缓冲容量**：`cli.c` 把 `buffer_size` 设为 `n × FTL_READ_USER_SIZE`，而 `g_wr_buf`
   实为 16 KB ⇒ `wr 0 40` 会**写爆 16 KB 数组**（内存损坏）。已改为上报真实 `sizeof(g_wr_buf)`，
   由 Controller 的容量校验拒绝。

---

## Part B — 接口契约（重构后）

### B.1 分层

```
UART CLI / Host
  └─ ssd_controller_submit(ssd_request_t)          ← HIL：按 LBA 路由 + 按**实例边界**切 run
       └─ ftl_submit_read/write(ftl, ftl_io_request_t)   ← FTL：自拆 plane + 队列 + 完成聚合
            └─ ftl_service(ftl)                          ← 主循环：推进 / 抽干 / 补发 / 后台
```

HIL 只做两件事：**按 LBA 路由到实例**、**在实例边界切连续 run**（一个 run 一次 `ftl_submit_*`）。
plane 配对、通道并行、GC、缓冲调度全在 FTL 内。

### B.2 请求结构

```c
typedef struct
{
    uint64_t               lba;             /* 起始 LBA（NVMe 协议宽度） */
    void                  *buffer;          /* 读：写回处；写：数据源 */
    uint32_t               count;           /* ★ 连续 LBA 数（= HIL block_count） */
    volatile uint8_t      *complete_flag;   /* 全部子操作完成后置 1 */
    volatile ssd_status_t *status_out;      /* 组完成后写入 OK / IO_ERROR */
    pending_cb_t           on_complete;     /* 组完成后回调一次 */
    void                  *on_complete_ctx;
} ftl_io_request_t;
```

- **无 `pages`**：长度即 `count`，单/双 plane 由 FTL 决定（旧 `pages` 字段已删）。
- 数据缓冲长度 = `count × FTL_READ_USER_SIZE`。

### B.3 对外接口

| 接口 | 语义 |
|---|---|
| `ftl_init(ftl, params)` | 初始化 |
| `ftl_service(ftl)` | 主循环：Phase 1 推进 / Phase 2 抽干完成 / **Phase 2.5 补发子操作** / Phase 3 后台 |
| `ftl_submit_read(ftl, req)` | 非阻塞提交读，长度任意 |
| `ftl_submit_write(ftl, req)` | 非阻塞提交写，长度任意 |
| `ftl_flush(ftl)` | 落已持久化映射（L2P cache 写回 + journal 尾页刷出） |
| `ftl_trim(ftl, lba, count)` | 取消 `[lba, lba+count)` 映射（非阻塞、同步完成） |

### B.4 完成语义：**组（group）**

一次 `ftl_submit_*`（= 一条 host 请求）= **一个组**，组内可含多个 FC 子操作
（每个子操作 = 一条 FC 命令 = 一个 pending 槽）。**通知字段只在全部子操作完成后触发一次**：

- `complete_flag`：全部子操作完成后置 1
- `status_out`：写入**粘性**聚合状态（任一子操作失败 ⇒ 整组 `IO_ERROR`）
- `on_complete`：全部子操作完成后调用一次

`ftl_submit_*` 返回 `FTL_OK` 仅表示**已受理**（含部分子操作排队待发），**不代表已完成**。

### B.5 三态返回与背压

子操作下发 `ftl_issue_subop` 三态：

| 返回 | 含义 | 处置 |
|---|---|---|
| `FTL_OK` | 已下发（或未映射读般同步完成） | 推进 `next` |
| `FTL_AGAIN` | **瞬态背压**：命令/desc/buffer 池满、SQ 满、pending 表满 | 排队，`ftl_service` Phase 2.5 补发 |
| `FTL_ERROR` | 永久失败（无可用块 / 配置错误） | 弃未下发者，组报 `IO_ERROR` |

连续 `FTL_AGAIN` 且无进展超过 `FTL_DRAIN_STALL_MAX`（8 轮）⇒ 判真卡死，弃未下发者并报错。

### B.6 调度顺序（`ftl_service`）

```
Phase 1   fc_service() 推进 NAND
Phase 2   completion_table_drain() 处理完成（腾出 FC 资源）
Phase 2.5 遍历本实例各未完成组 → ftl_drain_group 补发子操作（host IO 优先于后台）
Phase 3   三级后台（IDLE / NORMAL / BUSY）
```

> ⚠ `main.c` 目前只对 `g_ftl[0][0][0]` 调 `ftl_service` ⇒ Phase 2.5 只 drain 实例 0。
> 当前几何（CH=CE=LUN=1）正确；维度扩展时须遍历全部实例（既有结构缺口，本步使其显形）。

---

## Part C — 测试用例设计

> **路线：不做 mock，全部上板测**（用户定调）。原「mock `fc_submit` 驱动 L1 接口套件」的设计**不做**。
> **文档按层拆为两份**（2026-10-13）：
> - **FTL 层**纯 RAM UT → `ftl_ut_munit.md`（§4 的 8 套件，板上 `test` 跑）
> - **HIL 层 + 系统集成**（`src/controller/` 不是 FTL 的一部分）→ `hil_ut_design.md`
>   （Part A HIL 纯逻辑 UT、Part B~D 真 NAND CLI 上板验收，含物理断电跨重启）

### C.1 对外接口行为——上板验证（替代原 mock 套件）

自拆分 / 组完成 / 背压这类行为**用真 NAND + CLI 观察**，不 mock
（完整矩阵见 **`hil_ut_design.md` Part B**）：

| 行为 | 上板验证（`hil_ut_design.md §B`） | 断言 |
|---|---|---|
| 自拆分（`count=5/32`） | `wr 0 5` / `wr 0 32` → `rd … c` | PASS；CW 映射 + 页打包后 **1 次**（`wr 0 5`）/ **4 次**（`wr 0 32`）编程 |
| 组完成（通知一次） | 观察单次 `wr` 的完成日志 | 恰一次完成（不重复） |
| 背压 / 大请求 | `wr 0 33` | 被容量校验拒（`g_wr_buf` 16 KB ⇒ `n ≤ 32`） |
| `ftl_flush` | `wr 0 2` → `fl` → 断电 → 重启 → `rd 0 2 c` | PASS（§B.3） |
| `ftl_trim(lba,n)` | `wr 0 4` → `tr 0` → `rd 0 1 c` | 全 0 |

### C.2 L0（纯 RAM，板上 `test`）

- **FTL 层** 8 套件（allocator / watermark / gc_wl / rr / bbt / metadata / pending_ct / **group**）
  —— 逐条用例见 `ftl_ut_munit.md §4`。
- **HIL 层** `hil` 套件（state_machine / is_complete / schedule / route / run_end）
  —— 逐条用例见 `hil_ut_design.md` Part A。

### C.3 L2（真 NAND 上板）

UART CLI `wr/rd/tr/fl/info` + **物理断电重启**。CLI 缓冲 16 KB ⇒ `n ≤ 32` 才能通过容量校验；
要上板验证更大请求需先扩大 `g_wr_buf`。完整矩阵见 `hil_ut_design.md` Part B~D。

---

## 登记（本轮不做）

- **D4/D5**：HIL 侧 host 队列 + doorbell + CQ（更大重构）。
- ~~**D6**：LBA→实例路由上移到单一 FTL 前端（与本仓分片模型冲突，需单独立项）。~~
  ✅ **已裁决（v0.65）：保留分片模型并登记理由。** 「主流是单一前端」经查证**无出处**
  （见 A.3 的 D6 注）⇒ 按 `ftl_optimization_plan.md §九 序 6` 明文允许的「或明确保留分片
  并登记理由」处理；本项**不再是**「与主流偏离」。
- `ftl_trim` 大范围同步阻塞（超循环长停）——本轮只加 API，范围上界/分块后续定。
- `ftl_flush` 语义 = "落已持久化映射"，**非** NVMe 式 barrier（不等待在飞/排队写；
  当前 CLI 靠 `g_cli_pending.busy` 串行化）。
- 跨组 LBA 重叠时组间写序未定义（当前 HIL 串行，不成问题）。

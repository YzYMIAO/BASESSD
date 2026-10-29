# FC 读写序列引擎（草案 / 临时设计）

> **状态**：草案（临时）。**未实现、未上板、未评审**；评审通过前不作为权威设计，
> 也不按本文改代码。
>
> **归宿**：评审通过后并入 `docs/design/ftl_design.md`（读写路径 / FC 序列章节）
> 并删除本文；未通过前保留为临时稿。
>
> **关联**：`ISS-CACHE-01`（HSQ / 高优先级命令与 cache 序列互斥）、`ADR-4`（FTL 通过
> FC 接口访问 NAND）、`ADR-15`（NAND 流水线：Multi-Plane + 受限 Cache Program）。
>
> **目的**：把 cache read / cache program 从「FTL 按 4 pair 分段构造 NAND 序列」
> 收敛为「FTL 提交读写 run，FC 维护 per-LUN 连续序列」——这是存储主控的分层形态。

---

## 1. 问题

当前实现（`src/ftl/cache_batch.c`）：

- cache read 在 `ftl_drain_group` 里按 **32 LBA（4 pair）边界**尝试一次；
- 每批自己构造 `START_SEQ_MP → SEQ_MP×(N-2) → LAST_MP`（45h/46h/47h）；
- cache program 同理，每批 `FIRST → 44h×2 → END`（49h/44h/48h）；
- 批次上限 = `NAND_DISPATCH_CHAIN_DEPTH = 4`。

后果：

1. **FTL 在编码 NAND 协议序列**（45h/46h/47h 出现在 FTL 模块），与 ADR-4 的分层相悖；
2. 一条 host 连续读被切成 N 段，每段重新 `00h-addr-30h`（START），**批边界暴露 tR**；
3. 序列状态分散在 FTL 的每次 `*_try` 调用里，HSQ/RR 一旦插入同一 LUN 就会劈开序列
   （`ISS-CACHE-01`）；
4. 批次长度被 FIFO 深度 4 硬绑，而不是由 run 长度决定。

---

## 2. 目标分层

| 层 | 职责 | 不做什么 |
|---|---|---|
| FTL | L2P → 物理页列表（read/write run）；组完成记账 | 不出现 31h/3Fh/45h/46h/47h/44h/48h/49h |
| FC | per-LUN 序列状态机：START/SEQ/LAST、refill、owner 锁、完成聚合 | 不做 L2P / 映射决策 |
| NAND dispatch | per-LUN FIFO（深度 `NAND_DISPATCH_CHAIN_DEPTH`）执行命令 | 不理解“序列”，只看到连续命令 |

**判据出处**：`docs/design/fc_nand_design.md` §26（HIL / FTL / FCC / DRAM + 队列·门铃·完成）；
ONFI / MT29F4G08 datasheet 的 31h/3Fh 与 Two-Plane Read Cache 45h/46h/47h 语义
（`include/nand/onfi_1_0.h:226-292`）。具体主控内部调度不可见；公开可对照的是
Cosmos+ / SimpleSSD 的 channel/die 调度模型（`docs/reference/`，**待查具体章节**）。

---

## 3. 现状 vs 目标

| 维度 | 现状 | 目标 |
|---|---|---|
| 序列边界 | 每 4 pair 一段 | 一条 run 一段 |
| START/LAST | 每批各一次 | 每条 run 各一次 |
| 序列长度 | 受 FIFO 深度 4 限制 | 由 run 页数决定；FIFO 只限制在飞条数 |
| 序列状态 | 无（每次 `*_try` 自包含） | FC per-LUN `read_seq` / `write_seq` |
| FTL 是否知道 NAND op | 知道（`cache_batch.c`） | 不知道 |
| HSQ 插入 | 依赖“全 FTL 命令走 SQ”的全局假设 | owner 锁：序列打开期间另一队列回推 |
| 错误关闭 | 批内失败直接报组错 | 先发 LAST 关闭，再报错 / 启动 RR |

---

## 4. 设计

### 4.1 FTL 提交 read / write run

概念接口（最终签名待评审）：

```c
typedef struct
{
    BYTE               ch, ce, lun;
    const DWORD       *paa;       /* run 的物理页列表（页粒度） */
    BYTE *const       *bufs;      /* 每页数据缓冲（读=写回处，写=源） */
    WORD               count;
    void             (*complete)(void *ctx, WORD index, fc_status_e st);
    void              *ctx;
} fc_run_req_t;
```

- FTL 从 L2P 得到物理页列表后，**一次**提交 run；
- run 的页数只受 `WORD` 与缓冲资源限制，不再固定 4；
- FTL 不再调用 `ftl_cache_read_try` / `ftl_cache_write_try`；`cache_batch.c` 退化为
  “run 构造 + 连续性判据”，不再选择 op code。

### 4.2 FC per-LUN 序列状态机

```text
IDLE ──submit──> OPEN ──首命令完成──> ACTIVE ──页耗尽──> CLOSING ──LAST 完成──> DONE
                   │                    │                    │
                   └──错误/回退──────────┴────────────────────┘
                              （先发 LAST，再 DONE）
```

- **OPEN**：发首命令（读 45h / 写 49h）；
- **ACTIVE**：每完成一页，按 FIFO 空槽补下一条（读 46h / 写 44h）；不重发 START；
- **CLOSING**：最后一页用 LAST（读 47h / 写 48h）；
- **DONE**：聚合状态交回 FTL；清 owner 锁。

关键约束：

- 序列长度 **不受** `NAND_DISPATCH_CHAIN_DEPTH` 限制；FIFO 深度只决定“同时在飞几条”；
- 序列成员必须同 (ch,ce,lun)；跨 die 拆成多条序列；
- 页连续性由 FTL 的 run 保证（物理页 +1 / 同 block）；FC 只做断言。

### 4.3 owner 锁与队列

- 复用/扩展现有 `chain_owner` 思路（`include/fc/fc_scheduler.h:28-55`）：
  per-LUN `seq_owner`，序列打开期间另一队列的命令**回推**，直到 LAST 完成；
- HSQ 不再能插入同 LUN 的 cache 序列（`ISS-CACHE-01` 的核心）；
- 任何普通命令（普通读 / GC / RR / CP）要插入前，FC 必须先 CLOSE 序列；
- 与现有 chain 的区别：chain 是“FC 内部的短命令组”；seq 是“跨多次填充的长序列”，
  两者应统一成同一套 **per-LUN 序列所有权**，避免两套锁。

### 4.4 完成模型

两种候选（待定）：

| 方案 | 描述 | 优点 | 缺点 |
|---|---|---|---|
| A. 每页 completion + run 聚合 | FC 每页回调；FTL 用 run 计数聚合 | 与现有 pending per-page 接近 | FTL 仍要为每页建 pending |
| B. run completion + 页状态数组 | FC 只在 run 结束时回调，带每页状态 | FTL 记账简单 | 错误定位/部分完成需要额外结构 |

推荐先 A：保留现有 pending/group 的逐子操作记账，把“序列”作为 FC 内部对象；
后续若 FTL 记账简化，再评估 B。

### 4.5 错误 / 回退

- 任一页失败：停止 refill、发 LAST 关闭序列、剩余页报错；
- Read Recovery **必须等序列关闭后**再向同一 LUN 插入重试命令；
- 回退普通读：先 CLOSE，再走普通路径；
- release 下不得留半开序列（设备可能拒绝后续命令）。

### 4.6 掉电

- 读序列：瞬态，不落介质，掉电语义不变；
- 写序列：中间页（44h）**不确认 L2P/Journal**；END（48h）成功后才逐页确认——
  与当前 `ftl_confirm_write` 语义一致，只是序列由 FC 维护；
- 掉电恢复仍按“上一个 Checkpoint + Journal 回放”，与序列无关。

---

## 5. 待定决策

| ID | 决策 | 选项 | 建议 |
|---|---|---|---|
| D1 | 序列对象放哪层 | FC / NAND dispatch / FTL | **FC**（协议序列属于闪存控制器） |
| D2 | FTL 提交什么 | 物理页列表 / 预建命令 | 目标：页列表；迁移期可先收预建命令 |
| D3 | 完成粒度 | 每页+聚合 / run 级 | 先 A（每页+聚合） |
| D4 | 与 chain 的关系 | 两套锁 / 统一 owner | 统一 owner（避免 C9 双坐标） |
| D5 | cache program 确认归属 | FTL / FC | 保持 FTL（映射语义属于 FTL） |

---

## 6. 迁移阶段

| 阶段 | 内容 | 判据 |
|---|---|---|
| P0 | 本文评审 | 决策 D1~D5 定稿 |
| P1 | FC 序列引擎（读）+ FC UT | 状态机 / refill / owner / 错误关闭用例通过 |
| P2 | FTL 读路径切 run，删 read batch | FTL UT + 板端顺序读 |
| P3 | 写路径切 run（cache program），删 write batch | 板端 H2testw + 掉电 |
| P4 | 统一 owner 锁 / HSQ / RR（合并 ISS-CACHE-01） | RR 与 HSQ 插入用例 |
| P5 | 删旧 `cache_batch`，更新文档 | grep 旧路径为零 |

---

## 7. 验证

- **FC UT**：序列状态机、refill 节奏、owner 锁、错误关闭、LAST 必发；
- **FTL UT**：run 构造（连续性判据）、组完成、错误传播；
- **板端**：H2testw、`/speed/ftl_rw`、失败注入（若可构造）、掉电注入；
- **判据**：序列连续性看命令序列（45h 一次 / 46h N-1 次 / 47h 一次），
  而不是看当前吞吐；tR 是否被预取隐藏可作为观测项。

---

## 8. 出处与待查

- `include/nand/onfi_1_0.h:226-292`：31h / 3Fh / 45h / 46h / 47h 的序列语义；
- `docs/design/fc_nand_design.md` §26：分层判据；
- `docs/reference/README.md`：Cosmos+ / SPDK FTL / SimpleSSD 等公开资料；
  **具体到 cache sequence 的实现代码/章节待查**，本文不冒充已查证；
- 本仓现状：`src/ftl/cache_batch.c`、`src/ftl/ftl.c:1735`、`src/fc/fc.c:1667`、
  `include/fc/fc_scheduler.h:28-55`。

---

## 9. 风险

- 改动 **读路径 + FC 调度 + 完成记账**，按 R2 必须上板，不能只靠编译；
- 序列错误/掉电/部分完成的路径比当前多，UT 必须先覆盖；
- 不能以“当前吞吐没有提升”否定该设计——判据是主流形态，不是当前性能。

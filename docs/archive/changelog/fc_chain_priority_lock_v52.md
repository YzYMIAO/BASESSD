# v52：FC 链锁 —— 链未结束期间独占该 LUN 的 per-LUN FIFO（防 HSQ 插入）

- **日期**：2026-10-11
- **范围**：**src + 测试 + 文档**。`include/fc/fc_scheduler.h`、`src/fc/fc.c`、
  `tests/unit/test_fc_level3.c`、本文档。**`src/ftl/**` 一行不动**。
- **介质格式**：无变更。
- **测试**：用例数 **55 → 56**（Level 3 新增 `chain_priority_lock`）。
  `make format-check` 过；`make clean && make tests` **0 warning 0 error**、
  `.bin` **234988 B**（v51 是 232260 B）。
- ✅ **上板已验**（2026-10-11，seed `0x5a4f21e4`）：
  `/fc-nand/lifecycle/chain_priority_lock` **[ OK ] [ 597.37 us ]**、
  完整 `test` **56 of 56 (100%)**、0 skipped。

## 〇、起因

用户要一条单测，验 `fc_command_t.high_priority`（SQ/HSQ）与 `fc_cmd_conf_t.chain`
（MORE/END）的组合语义。设计过程中定位到一个**真实缺口**，且用户明确这是**需求**：

> 「防止 fifo 里面有要执行的链，但有的链还没加进来，且 HSQ 上有命令，防止被插入」

## 一、缺口（已核实）

一条链长于 NAND per-LUN FIFO 深（`FC_SCHEDULER_CHAIN_MAX = NAND_DISPATCH_CHAIN_DEPTH = 4`）
时，`fc_poll_chain_length`（`src/fc/fc.c:1584`）会把批截到 4，链被拆成多批、分多次填充
pass 下发。**批与批之间没有任何锁**：

- `fc_poll_fill`（`src/fc/fc.c:1722`）的 `suspended[]` 每趟 `memset` 清零 ⇒ 无跨 pass 状态；
- `nand_dispatch_enqueue` 只保证**一批内**原子（空间不足整体拒收），跨批不管；
- `src/nand/nand_dispatch.c` 里 grep `lock/open_chain/chain_active` 零命中 ⇒ NAND 侧看不到链
  （设计如此：「每槽 = 一条命令」）。

⇒ 8 命令链被拆成 4+4，第二组之前 HSQ 可插。

## 二、设计

**新增一个跨填充 pass 持久的 per-LUN「链归属队列」锁。锁的是「哪条队列能喂这个 LUN」，
不是「这个 LUN 不能用」。**

状态（`include/fc/fc_scheduler.h`，随 `fc_t` 内嵌、`fc_init_internal` 的
`*controller = (fc_t){0};` 天然清零）：

```c
typedef enum
{
    FC_CHAIN_OWNER_NONE = 0U,
    FC_CHAIN_OWNER_SQ,
    FC_CHAIN_OWNER_HSQ
} fc_chain_owner_t;
/* fc_scheduler_t 里： */ uint8_t chain_owner[CH_NUM][CE_NUM][LUN_NUM];
```

`fc_poll_fill_queue`（`src/fc/fc.c`）三处：

1. **准入**：`chain_owner[ch][ce][lun] != NONE && != 本队列` → 回推跳过
   （**不置 `suspended`**——置了会把链自己的续批一起锁死）。
2. **更新**：一批下发后看**批尾**的 chain 位——`MORE` ⇒ 锁继续（转本队列）；`END` ⇒ 解锁。
3. **失败清锁**：整链注 `NAND_FAIL`（`dispatch_status != OK`）时清 `chain_owner[L]`，不留残锁。

`fc_poll_fill` 两次调用各传 `FC_CHAIN_OWNER_HSQ` / `FC_CHAIN_OWNER_SQ`。

**独占语义（核心不变式）**：链一旦开始，该 LUN 的填充权归链所在队列独占，直到链的 END
下发。是**提交队列级**独占（链所在队列照常能喂，续批正从这条队列取），**不是**「LUN 被
占死」；代价是打到该 LUN 的另一条队列命令被延后到链结束——「链不可被插入」优先于
「HSQ 抢占」。与既有的「共享 `suspended` 防优先级倒挂」正交（准入只回推、不置 suspended）。

**前置契约**：链成员必须在**同一条提交队列**里**连续入队**（现有链机制本就依赖这一点；
`fc_submit` 一次一条，连续性无人保证，本版只写进注释，不新加强制）。

**对生产路径零影响**：FTL 全仓恒 `FC_CHAIN_END`（设计文档 §23.9）⇒ `chain_owner` 恒 NONE
⇒ 准入判断恒假、更新恒写 NONE ⇒ **生产路径逐字不变**。这是本改动能上板的前提。

## 三、用例 `chain_priority_lock`

`tests/unit/test_fc_level3.c`，并入 `g_fc_lifecycle_tests[]`（零注册改动）、
`MUNIT_TEST_OPTION_NONE`。

```
Step 0  GET(01h) 原值，断言 == 00h（手册默认模式 0）
Step 1  SQ 基线：set0(x) → get0                     读回 x
Step 2  HSQ 真插队：set0(x) → get0 → set1(y) 一起提交；get0 与终值都是 x（y 被覆盖）
Step 3  ★ 链锁：连续提交 8 条 SQ 链（前 7 条 MORE、末条 END，set/get 交替）
        fc_service() 一次（前 4 条进 FIFO，chain_owner=SQ）
        投 HSQ set1(y)/get1(y) → 抽干 → get1 读到 y、终值 y
Step 4  还原 01h
```

**判据的区分力**：无锁时 HSQ 的 y 插在链的第 4、5 条之间 ⇒ 终值 **x**；有锁 ⇒ **y**。
x = 01h、y = 02h（手册 Table 15 的 00h..05h 内，都是合法时序模式）。

新增件（全部 `static`，只服务本用例）：`g_l3_prio_slots[]` 在途登记表 +
`l3_prio_reset` / `l3_prio_submit`（建 + 设 priority/chain + 提交）/
`l3_prio_drain`（按 `command_id` 配回槽位、GET 存值、release）。与 v50 的 CID 配对同路。

## 四、验证

```bash
make format-check                  # ✅ Format check passed.
make clean && make tests           # ✅ 0 warning 0 error；用例数 55 → 56；.bin 234988 B
```

**上板（用户跑，本版唯一强判据）**：

```
test /fc-nand/lifecycle/chain_priority_lock   # ✅ [ OK ] [ 597.37 us ]
test                                          # ✅ 56 of 56 (100%)，既有 55 条逐字不变
```

`test --log-visible debug /fc-nand/lifecycle/chain_priority_lock` 的证据行（seed `0x5a4f21e4`）：

```
[EV] step1 SQ 基线: set(01) -> get 读回 01
[EV] step2 HSQ 插队: get 读回 01（set1(02) 先落盘后被 set0(01) 覆盖）
[EV] step3 链锁: HSQ get 读回 02、终值 02（无锁时终值会是 01=x）
```

Step 2 与 Step 3 合起来才说明问题：**HSQ 确实能抢占**（step2 读回 01 而非 02），
**但链一旦开始就抢不进来**（step3 终值 02）。

## 风险 / 不做

- 改的是所有命令的热路径，但 `chain_owner` 恒 NONE ⇒ 生产路径零行为变化。
- 不动 `nand_dispatch.c`（链语义留在 FC 侧）、`fc_poll_chain_length` 的 4 上限
  （链仍分批，锁负责跨批原子性）。
- 「链必须连续入队」不新加强制，只写进注释。
- 不动 `tests/integration/**`、`tests/framework/**`、`ut_main.c`、`src/ftl/**`；
  不翻 `CONFIG_FC_RAID`。

## 出处

- 链锁：`include/fc/fc_scheduler.h`（`fc_chain_owner_t` + `chain_owner[]`）、
  `src/fc/fc.c`（`fc_poll_fill_queue` 准入/更新/清锁、`fc_poll_fill` 队列标签）。
- 用例：`tests/unit/test_fc_level3.c`（`test_chain_priority_lock` 及其上方 `l3_prio_*`）。
- 前作：`docs/archive/changelog/fc_ut_stress_v51.md`。

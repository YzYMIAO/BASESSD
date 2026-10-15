# v48：订正 `OP_READ_PAGE_CACHE_RANDOM` 的语义——「跳页」而非「随机读单页」

- **日期**：2026-10-11
- **范围**：**纯测试 + 注释/文档**。`tests/unit/fc_test_common.{c,h}`、`tests/unit/test_fc_level3.c`、
  `include/nand/onfi_1_0.h`（**仅 op 注释**）、`docs/design/fc_nand_ut.md` + 本文档。
  **`src/**` 一行不动**（含 `src/ftl/**`）——uop 序列本身没错。
- **介质格式**：无变更。
- **测试**：**用例数不变（仍 53）**。`make format-check` 过；`make clean && make tests` **0 warning 0 error**。
- ✅ **上板已验**（2026-10-11 seed `0x5a4f21e4`）：`/fc-nand/lifecycle/block_cache_ops` **1 of 1 (100%)**、
  完整 `test` **53 of 53 (100%)**、0 skipped。`00h-addr-31h` 的「输出上一页」语义在真器件上逐条坐实（见下「四」）。
  ⇒ **v44~v48 全部上板通过**。
## 〇、起因：上板失败

`test` 跑到 `/fc-nand/lifecycle/block_cache_ops` 时在 `tests/unit/fc_test_common.c:717` 停下：

```
assertion failed:completion.status & ((1U << 0) | … | (1U << 5)) == 0U (16 == 0)
[ERR] [RES] cmd=1/64 desc=4/256 buf=4/128 sq=0 hsq=0 cq=0
kind : ABORT
```

`16 = 1<<4 = FC_CW_STATUS_MATA_ERROR`，只可能由 `src/fc/fc_data_path.c:623` 的
`fc_data_path_meta_check(buffer, command->meta, desc->paa)` 置位 —— **读回来的 CW 里 META 对不上**。
失败点是 Phase 3（v44 新增的 `test_read_cache_random`），读 `PAGES_PER_BLOCK/2` = page 32。

关键旁证：**Phase 2 刚刚逐页读过 0..63 并逐 CW 校验过**（START + SEQ×63 + LAST 全绿才轮到 Phase 3）
⇒ 器件上 page 32 的数据本身是对的。所以只能是**这条命令没有把 page 32 输出出来**。

## 一、为什么不是 v47（先排除）

v44~v47 都未提交，必须先把「是不是刚改的地址重构干的」排掉。做了两件事：

1. **host 侧穷举数值核对**（直接编译 `include/nand/nand_config.h`，plane×page×block×cw 全组合）：

   | 核对项 | 新（v47） | 旧（v46） | 结果 |
   |---|---|---|---|
   | `cmd->paa` | `paa_from_addr(addr)` | `pba_to_paa(pba_from_addr(addr))` | **0 处不符** |
   | `desc->paa` | `paa_with_cw(paa_with_plane(row_paa, plane), cw)` | `pba_to_paa((row_pba & ~PLANE_CW_MASK) \| plane_cw)` | **0 处不符** |

2. **`git diff src/fc/fc.c`**：本路径（cache 读走 `nand_operation_has_data` 分支 →
   `fc_build_request_data_planes` 单 plane 支）的改动是**纯换名**——
   `pba_to_plane(command->pba)` → `paa_to_plane(command->paa)`、`pba_to_paa(command->pba)` → `command->paa`。
   `fc_is_page_read` 的成员、op 表、uop、NAND 侧散落逻辑（`nand_dispatch.c:662`）全未动。

3. `OP_READ_PAGE_CACHE_RANDOM` 的 op 表项与 uop 序列（`src/nand/onfi_1_0.c:356/829`）
   **在 HEAD（v43）里就存在、本轮未改**；`test_read_cache_random` 是 v44 新加、从未上板。

## 二、根因：v44 把语义理解反了

v44 changelog 自己就写了这条风险（原文）：

> ⚠ 本版的风险点（如实记录）：`00h-addr-31h` **从未在本板上跑过**。若器件不接受它独立执行
> （例如要求 cache 管线已激活），Phase 3 会当场红——那本身就是本版要暴露的结论。

手册原文（用户提供，逐字）：

> The READ PAGE CACHE RANDOM (00h-31h) command **reads the specified block and page into the data
> register while the previous page is output from the cache register**. … The column address in the
> address specified is ignored. The die (LUN) address must match the same die (LUN) address as the
> **previous READ PAGE (00h-30h)** command or, if applicable, the **previous READ PAGE CACHE RANDOM
> (00h-31h)** command.

⇒ 三条硬结论：

| # | 手册依据 | 含义 |
|---|---|---|
| 1 | 「reads the specified … page **into the data register** while the **previous page is output from the cache register**」 | **地址页只进 data register（预取）；当场输出的是 cache 寄存器里的「上一页」** ⇒ **输出页 ≠ 地址页**，地址页要等**下一次 `31h`** 才出 |
| 2 | 「must match … the previous READ PAGE (00h-30h) … or … previous READ PAGE CACHE RANDOM (00h-31h)」 | 该命令**必须在 `00h-30h` 或 `00h-31h` 之后**下发 ⇒ **不能当独立入口单发**（Phase 3 原来正是独立单发，且落在 Phase 2 的 `3Fh` 之后） |
| 3 | 「column address … is ignored」 | uop 里 COLUMN_0/1 送 0 无害；`req_cache_read` 不用改 |

**定位**：本 op 等价于「**带显式下一页地址的 `31h`**」，而不是「随机读单页」。
这与本项目**早已跑通**的 SEQ 模型完全同构——`test_read_block_cache` 的注释写的就是
「第 i 次 SEQ 输出 page i-1、同时把 page i 载入 cache」；把 `31h` 换成 `00h-addr-31h`，
唯一差别就是「下一页」由设备自增改成**显式地址**。

⇒ **`src/**` 的 uop 序列（`g_nand_cache_read_random_uops`）没错**，错的是测试的预期，
以及 `include/nand/onfi_1_0.h:209-214` 那条 op 注释（画成 `00h-addr-31h → Dout`，读起来就是「输出地址页」）。

> **旁证**：`docs/design/fc_nand_ut.md:382` 的 op 覆盖表早就把本 op 的 `standalone` 列标成 **❌**
> （而 `OP_READ_PAGE_CACHE_SEQ`/`_LAST` 同为 ❌、`OP_NORMAL_READ_PAGE_WITH_DATA_OUT` 为 ✅）
> ——即「本 op 不能独立下发」这条**在表里本来就是对的**，只是 v44 写用例时没照着它写。
> 该表**无需改动**。

## 三、改动

### `tests/unit/fc_test_common.c`

| 函数 | 改动 |
|---|---|
| `test_cache_start()`（**新增**，static） | 把 `test_read_block_cache` ① 里内联的 START（00h-addr-30h）抽成 helper（`page` 参数化），长注释一并搬过去。Phase 2 的 ① 改为一行调用——**行为不变** |
| `test_assert_read_meta()`（**新增**，static） | **自描述断言**：直接比对 `test_cw_desc(cmd,0)->meta` 与「期望输出页」的 LBA。`desc->meta` 在 Pass 4 被写成**从介质实际读回**的 META ⇒ 失败信息里 munit 会打出 actual/expected 两个数，「**实际输出了哪一页**」一眼可见——不再像这次一样只能看到 `16 == 0` |
| `test_read_cache_random()`（**重写**） | 签名 `(block, plane_bitmap, page_index, config)` → **`(block, plane_bitmap, prev_page, addr_page, config)`**；体内自建最小管线：① `START(prev_page)` ② `RANDOM(addr_page)`（**期望输出 `prev_page`**，`meta` 取 `prev_page` 的 PBA）③ `SEQ(31h)`（**期望输出 `addr_page`**，这一步才证「地址真的生效」）④ `LAST(3Fh)`（输出 `addr_page+1` 并收尾）。`ASSERT`：`prev_page` / `addr_page` 都不得是块尾（手册：cache 读不能跨块边界） |

### `tests/unit/fc_test_common.h`

`test_read_cache_random` 的声明与契约注释按新语义重写（原文「载入 + 输出合一、输出页 = 地址页」标为 **v44 的错解、已证伪**）。

### `tests/unit/test_fc_level3.c`

`block_cache_ops` Phase 3 改为 `prev_page = PAGES_PER_BLOCK/4 = 16`、`addr_page = PAGES_PER_BLOCK/2 = 32`
（**从 page 16 跳到 page 32**——不是顺序递增，跳页意图明确）；用例头注释、验证点、摘要行同步。

### `include/nand/onfi_1_0.h`（**仅注释**）

`OP_READ_PAGE_CACHE_RANDOM` 的 op 注释补上语义（地址页进 data register、输出的是上一页、
必须在 00h-30h / 00h-31h 之后下发、列地址被忽略）。**不改任何宏值 / 表项**。

### `docs/design/fc_nand_ut.md`

- §2.3 的「cache 读随机」bullet 改写为「cache 读随机跳页（v1.26 加、v1.30 订正语义）」，写明 v1.26 的错解与手册原文；
- §用例表 `block_cache_ops` 行的流程与说明同步；
- §修订历史新增 v1.30 行（v1.26 行**原样保留**，作为历史）。

## 四、验证

```bash
make format-check                  # ✅ Format check passed.
make clean && make tests           # ✅ 0 warning 0 error；用例数 53（不变）
                                   #    text 224828 + data 1688 = 226516 B（.bin 226556 B）
                                   #    AXI 81.2% / DTCM 1.5% —— 与 v47 同档，+1.3 KB 是新 helper
```

**上板（用户跑，本版唯一强判据）—— ✅ 已跑，绿**：

```
test --log-visible debug /fc-nand/lifecycle/block_cache_ops
```

实测输出（seed `0x5a4f21e4`，block=647、plane=1、bm=0x02）：

```
[EV]   cache random addr=p32 -> out=p16 declared_meta=0x00050E84 bm=0x02 cw=4 status=0x40
[EV]   cache out=p32 declared_meta=0x00050F04 bm=0x02 cw=4 status=0x40
[EV]   cache out=p33 declared_meta=0x00050F0C bm=0x02 cw=4 status=0x40
[EV] cache random block=647 bm=0x02: START(p16) -> 00h-addr(p32)-31h out=p16 -> 31h out=p32 -> 3Fh out=p33, 3 pages PAA-verified
1 of 1 (100%) tests successful
```

三条 META 与 Phase 1 的 prog 表**逐位吻合**（p16=`0x00050E84`、p32=`0x00050F04`、p33=`0x00050F0C`），
`status=0x40` 只有 READY、零错误位。⇒ 手册那三条**在本器件上全部成立**：

| # | 待验假设 | 上板结论 |
|---|---|---|
| 1 | `00h-addr-31h` 当场输出的是**上一页**而非地址页 | ✅ 地址打 p32、输出 **p16** |
| 2 | 地址页被预取进 data register，下一次 `31h` 才出 | ✅ 紧跟的 `31h` 输出 **p32**（META 与 p32 的 PBA 一致） |
| 3 | 管线可正常收尾 | ✅ `3Fh` 输出 **p33**，无残留 |

随后跑**完整 `test`**（同 seed）：

```
/fc-nand/logic/*                    [ OK ]   (12 条)
/fc-nand/data_path/*                [ OK ]   (含 3 组参数化 case)
/fc-nand/lifecycle/init_sequence          [ OK ]
/fc-nand/lifecycle/feature_write_read     [ OK ]
/fc-nand/lifecycle/status_query           [ OK ]
/fc-nand/lifecycle/internal_data_move     [ OK ]
/fc-nand/lifecycle/block_erase_verify     [ OK ]
/fc-nand/lifecycle/block_write_read       [ OK ]
/fc-nand/lifecycle/block_cache_ops        [ OK ]
/fc-nand/lifecycle/block_multi_plane      [ OK ]
/fc-nand/lifecycle/block_random_data      [ OK ]
/fc-nand/lifecycle/block_load_fetch       [ OK ]
/fc-nand/lifecycle/block_read_meta        [ OK ]
53 of 53 (100%) tests successful, 0 (0%) test skipped. Total wall-clock: 707.13 ms
```

⇒ **v44~v48 全部上板通过**（含 v47 的 `pba`→`paa` 重构——「没有遗漏的调用点」这条只有完整套能判）。

## 五、风险 / 需上板确认（如实记录）

- ~~**`00h-addr-31h` 在本板上仍未被证明「能用」**~~ ⇒ **已由本次上板坐实**（见「四」）。
  「该命令在 `00h-30h` 之后下发、器件接受、`SEQ` 能取出被预取的地址页」三条**全部成立**，
  `test_assert_read_meta` 这条自描述断言在本轮未触发（可作为**回归哨兵**留着：若将来有人
  再改坏语义，它会直接把「实际读回了哪一页」打出来）。
- ~~**v44~v47 的其余部分仍未上板**~~ ⇒ **已由完整 `test` 53 of 53 补齐**。
  v47 的 PBA→PAA 重构除穷举核对「值逐位不变」外，「没有遗漏的调用点」这条也由上板坐实。
- 该 op **生产侧零调用者**（`src/fc/fc.c:125` 的 `fc_is_page_read`、`nand_dispatch.c:662` 的散落分支、
  `req_cache_read` 是「接住它」的，但没有任何生产代码发它）⇒ 本版修正**只影响单测**，不影响固件行为。

## 不做

- 不动 `src/**`（含 `src/ftl/**`）——uop 序列 / op 表 / 派发逻辑本身没错；
- 不动 `tests/integration/**`、`tests/framework/**`、`ut_main.c`；
- 不改 v44 changelog 与 §修订历史 v1.26 行的正文（历史不改，只在 v1.30 行里说明）；
- 不翻 `CONFIG_FC_RAID`；不处理 OTP。

## 出处

- 手册语义：用户提供的 ONFI **READ PAGE CACHE RANDOM (00h-31h)** 描述与两幅时序图
  （「reads the specified block and page into the data register while the previous page is output
  from the cache register」/「The column address … is ignored」/「must match … the previous READ PAGE
  (00h-30h) … or … the previous READ PAGE CACHE RANDOM (00h-31h)」）。
- 位序与换算：`include/nand/nand_config.h`（PAA `:150-260`、PBA `:340-398`、`pba_to_paa` `:360`）。
- 失败点：`tests/unit/fc_test_common.c:717`（v48 后行号已变）、`src/fc/fc_data_path.c:623`。
- op 表 / uop：`src/nand/onfi_1_0.c:356`（`g_nand_cache_read_random_uops`）、`:829`（op 表 14 行）。
- 生产可达：`src/fc/fc.c:125`、`src/nand/nand_dispatch_tables.c:581`、`src/nand/nand_dispatch.c:662`。
- 同构的已跑通模型：`tests/unit/fc_test_common.c` 的 `test_read_block_cache`（START 入口）。

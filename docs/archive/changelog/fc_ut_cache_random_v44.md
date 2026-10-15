# v44：补 `OP_READ_PAGE_CACHE_RANDOM` 覆盖 —— cache 读的第二条入口

- **日期**：2026-10-11
- **范围**：**纯测试**——`tests/unit/fc_test_common.{c,h}` + `tests/unit/test_fc_level3.c`。
  **`src/**` 一行不动**（含 `src/ftl/**`）、`tests/framework/**`、`ut_main.c`、`tests/integration/**` 一律不动。
- **介质格式**：无变更。
- **测试**：**用例数不变（仍 51，9 条 Level 3 未增减）**——本版只给已有用例加一个阶段。
  `make format-check` 过；`make clean && make tests` **0 warning 0 error**、链接 **220908 B**（原 220132）。
- ⚠ ~~**需上板**：该 op 此前从未上过板。~~
  ✅ **上板已验**（2026-10-11，seed `0x5a4f21e4`）：全套 **53 of 53 (100%)**。
  ⚠ 但本版的 Phase 3 **理解是错的**（当成「输出页 = 地址页」），上板当场 `MATA_ERROR`——
  已在 **v48 按手册订正**（`00h-addr-31h` 输出的是**上一页**）。本文件正文**保持原样**作为历史。

## 〇、起因：盘点 Level 3 的 OP 码覆盖

逐条核对 `test_fc_level3.c` 实际发出的 op 码后发现，`OP_COUNT` = 42 槽（含 9/11/20 三个删除空位）里
**live 的 39 个 op 只有 20 个被 Level 3 覆盖**（≈51%）。其中 4 个（`OP_BLOCK_UNLOCK_LOW/HIGH`、
`OP_BLOCK_LOCK`、`OP_BLOCK_LOCK_TIGHT`）在文件里有代码但被 `#if 0` / `__attribute__((unused))` 挡住；
6 个是 Phase V 多 LUN 拆分码（`OP_READ_PAGE_LOAD` 等，只走 FTL 的 auto-split）。

**本版只处理 cache 读那一个**：`OP_READ_PAGE_CACHE_RANDOM`（op 14）。

## 一、为什么它是真缺口，不是「没人用的 op」

`block_cache_ops` 的 cache 读管线走的是 **START 入口**：

```
START(00h-addr-30h) → SEQ(31h) × 63 → LAST(3Fh)      /* 夹具 test_read_block_cache */
```

`OP_READ_PAGE_CACHE_RANDOM` 是**另一条入口**：`00h + 5 地址周期 + 31h`——**载入 + 输出合一**，
即「随机访问 cache 管线」。夹具走了 30h 那条，于是带地址的 31h 从未被执行。

而它**在生产里是活的**，三处都收它：

| 位置 | 作用 |
|---|---|
| `src/fc/fc.c:125` | `fc_is_page_read()` 把它列为「带 ECC 编码的 Page Read」⇒ 走 `process_read`（ECC 解码 + 条件解扰） |
| `src/nand/nand_dispatch_tables.c:542` | req handler = `req_cache_read`；`:484` completion = `completion_per_cw` |
| `src/nand/nand_dispatch.c:662` | 数据散落分支**显式列出**本 op（与 SEQ / LAST / FETCH 同组，页缓冲 offset = CW 号 × CW_SIZE） |

⇒ 它的 uop 序列、`page-read` 分类、页缓冲散落三者**此前零测试背书**，写错了没人拦。

## 二、做法

### 夹具新增 `test_read_cache_random(block_index, plane_bitmap, page_index, config)`

单次 RANDOM 读 + 逐 CW PAA 校验（`fc_test_common.c`，紧跟 `test_read_block_cache` 之后）。
与既有两个 cache helper 的关键差别：

| | START / SEQ / LAST | **RANDOM（本版）** |
|---|---|---|
| 地址周期 | START 有 5 个；**SEQ / LAST 一个都没有**（设备自增） | **5 个**——传哪页读哪页 |
| 输出的是 | START 不输出；SEQ 输出**上一轮加载**的页 | **地址所指的那一页**（载入 + 输出合一） |
| `meta` | SEQ 取「真正输出页」的 PBA（与命令地址**刻意解耦**，地址恒传 0） | 取**同一页** PBA（输出页 = 地址页） |

`conf.operation` 由 helper 内部覆盖为 `OP_READ_PAGE_CACHE_RANDOM`（与 `test_read_block_cache`
「operation 逐步覆盖」同约定）。`cw_bitmap` 传本 plane 段全位图——op 表里 `has_data = 1U`
⇒ **必须建链**（`ftl_cmd_needs_desc_chain` 只排除 SET/GET_FEATURES），传 0 会被
`ftl_cmd_resolve_cw_bitmap` 当场 ASSERT（v35 踩过的坑）。

证据：一条 `test_trace_detail("cache random out=p%02u meta=0x… bm=0x… cw=… status=0x…")`——
与 SEQ 的明细形成对照（那边 `out=pNN` 与地址页号不是一回事）。

### `block_cache_ops` 加 Phase 3

```
Phase 0 擦除 → Phase 1 cache 编程（15h ×63 + 10h） → Phase 2 START + 31h ×63 + 3Fh
→ Phase 3  test_read_cache_random(block, plane, PAGES_PER_BLOCK/2)
```

回读**中间页**（`PAGES_PER_BLOCK/2` = 32）——既不撞 Phase 2 已覆盖的边界页，又明确是「随机跳页」。
摘要行补一句「00h-addr-31h 随机页回读一致」。

## 三、验证

```bash
make format-check                  # 过
make clean && make tests           # 0 warning 0 error；链接 220908 B；用例数仍 51
```

**上板（用户跑，本版唯一验收方式）**：

```
test --log-visible debug /fc-nand/lifecycle/block_cache_ops
```

验收点：Phase 3 打出一条 `[EV] cache random out=p32 …`，`status` 无错误位，逐 CW PAA 校验通过；
`block_cache_ops` 仍 1 of 1。

⚠ **本版的风险点（如实记录）**：`00h-addr-31h` **从未在本板上跑过**。若器件不接受它独立执行
（例如要求 cache 管线已激活），Phase 3 会当场红——那本身就是本版要暴露的结论（要么 op 表编码有误，
要么该命令不能独立用）。此时按「断言失败即真实信号」处理，不绕过。

## 不做

- 不动 `src/**`（含 `src/ftl/**`）、`tests/framework/**`、`ut_main.c`、`tests/integration/**`；
- 不补其余未覆盖 op（4 个 `#if 0` 死代码 + 9 个纯未实现 + 6 个 Phase V 拆分码）——本版只处理 cache 读这一条；
- 不加用例、不改断言集（只是给 `block_cache_ops` 多走一个阶段）。

## 出处

- 覆盖盘点：`include/nand/onfi_1_0.h`（`OP_COUNT` / `OP_*`）、`src/nand/onfi_1_0.c:828`（op 表 14 行）、
  `src/nand/onfi_1_0.c:356`（`g_nand_cache_read_random_uops`）。
- 生产可达：`src/fc/fc.c:125`、`src/nand/nand_dispatch_tables.c:484/542`、`src/nand/nand_dispatch.c:662`。
- 夹具体例：`tests/unit/fc_test_common.c` 的 `test_read_block_cache`（START 入口）。

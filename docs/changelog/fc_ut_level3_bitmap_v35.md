# v35：修 Level 3 两处 cw_bitmap 建链缺陷（v27 语义变更的漏扫调用点）

- **日期**：2026-10-10
- **范围**：`tests/unit/fc_test_common.{c,h}`、`tests/unit/test_fc_level3.c`
- **前置**：v27/v28（`cw_bitmap` 由「0 = 本 plane 段全部 CW」改为**字面位图**，0 = 空集）、
  v31（Level 3 迁移进共享夹具）、v34（META 换页 PBA）
- **生产代码**：`src/**` **零改动**。两个 ASSERT 都在 FTL 策略层，是**被测代码正确地拦住了
  测试的错误**，不是生产缺陷。
- **介质格式**：无变更。
- **测试**：`make format-check` 过；`make clean && make tests` **0 warning 0 error**、
  链接 **213156 B**；**上板已验：77 of 77 (100%) tests successful, 0 skipped**。

## 背景：v27 把 0 的含义翻了过来，调用点没回扫

v23~v26 的契约是 `cw_bitmap == 0` = 「本命令 plane 段内的**全部** CW」；
v27 取消这个重载，改成**字面位图**——置 1 的位才是要访问的 CW，**0 = 空集**，
并新增 `ftl_cmd_resolve_cw_bitmap()` 的两条断言（`src/ftl/ftl_fc_interface.c:34-35`）：

```c
ASSERT(bitmap != 0U);               /* 字面位图：0 = 空集 = 调用者忘了给 */
ASSERT((bitmap & ~allowed) == 0U);  /* 越出本命令 plane 段 = 编程错误 */
```

v31 把 Level 3 迁进共享夹具、v32 做参数化，两版都**只保证编译通过**（`make tests` 只证链接），
从未上板。于是两处陈旧的建链调用点一直潜伏到首次上板才炸。

## 缺陷一：`block_cache_ops` 的 cache START 传了空位图

`fc_test_common.c` 的 `test_read_block_cache()` Phase ①：

```c
cmd = test_cmd_build(&(nand_addr_t){...page = 0U}, 0U, 0U, &conf);   /* OP_READ_PAGE_CACHE_START */
```

v31 写的注释是「NAND 侧是 `req_no_data`（无数据阶段）⇒ **cw_bitmap = 0（无数据链）**」。
**这一步推理错在两处**：

1. **「无数据阶段」≠「不用建链」。** `g_nand_operation_map[OP_READ_PAGE_CACHE_START].has_data = 1U`
   （`src/nand/onfi_1_0.c:830`）——与 `OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT` 同口径，
   **「加载型」op 也算占数据路径**。`fc_command_validate` 断言「has_data ⇒ desc_id 有效」
   （`src/fc/fc_command.c:67-70`），所以链是**必须**的。
2. **0 不再是「整段」。** 旧代码 `test_page_cmd_ex(..., cw_mask = 0, ...)` 传 0 之所以能跑，
   靠的正是 v23 的隐式展开（会建出 4 条 desc 的满链）。v27 之后同一个 0 是空集 ⇒ 第一条断言炸。

**改**：传 `test_plane_cw_bitmap(plane_bitmap)`（本 plane 段全位图，与写路径同口径）；
meta 一并给本页 PBA，保持「meta 恒为页 PBA」这条无例外（START 不在 `fc_is_page_read` 里，
不跑 process_read，值本身被忽略）。

## 缺陷二：`block_random_data` 的随机列读位图是 plane 盲的

`test_fc_level3.c:901`：

```c
(uint32_t)1U << cw,          /* plane=0 侥幸对；plane=1 位落在段外 */
```

cw_bitmap 是 **flat 绝对编号**（bit n = `plane * CW_NUM + cw`，v9 统一、v27 定为字面位图）。
**plane 1 的 CW0 是 bit 4，不是 bit 0**。plane=1 时 `0x01` 落在 plane 0 段，而本命令段是
`[1, 2)` ⇒ `allowed = 0xF0` ⇒ 第二条断言（越段）炸。plane=0 那一行走同一条代码，位图恰好合法，
所以只有参数化新增的 plane=1 用例暴露它。

**改**：`(uint32_t)1U << ((uint32_t)plane * CW_NUM + cw)`。

## 改动

| 文件 | 改动 |
|---|---|
| `fc_test_common.c` | `test_read_block_cache` 的 START 建链：`0U` → `test_plane_cw_bitmap(plane_bitmap)` + 本页 PBA；注释重写（has_data=1 ⇒ 必须建链；v27 起 0 = 空集） |
| `fc_test_common.h` | cache 段文档同步；§三 的「离散 CW 自己传 `1U << cw`」→ flat 绝对编号写法 |
| `test_fc_level3.c` | `block_random_data` 随机列读位图改 flat；文件头 § 的同一处写法同步 |

**未新增 / 删除用例**：参数化展开后仍是 **77** 条，与 v34 一致，只是全部转绿。

## 全仓同类扫描（两轮都做过）

- `test_cmd_build(..., 0U, ...)` 的其余调用点：擦除（`has_data = 0`，走
  `ftl_cmd_needs_desc_chain` 短路分支，不看位图）、`OP_RESET`（`has_data = 0`）、
  `GET/SET_FEATURES`（被 `ftl_cmd_needs_desc_chain` 显式排除）——**均合法**。
- plane 盲位图：Level 2 的 `l2_bm_cases` 已正确（`plane1-only` 用 `0xF0U`，注释专门点了
  「绝对编号，不是 0x0F」）；Level 3 其余位图全走 `test_plane_cw_bitmap()`。**只此一处**。

## 勘误：v34 记录中的一句

v34 的 changelog（`fc_ut_meta_pba_v34.md`）写「**上板已验**：……全套 FC UT 0 失败」——
**当时只单独跑了 2 条用例**（`process_read_mata_error` / `internal_data_move`），
并未跑全套；全套的首次全绿是本条（77/77）。该文件按 `docs/changelog/` 的惯例不改写正文，
在此更正。

## 教训

v27 的语义翻转（`0` 从「整段」变「空集」）**没有回扫所有调用点**，而受影响的 Level 3 又因
「只保证编译通过」而长期没上板——两个条件叠加才让它潜伏三版。
**`make tests` 只证链接**（项目硬规则）；夹具/测试改动，只要碰了建链、位图、地址这三类
「静默可用错值通过编译」的量，就必须上板。

# v31：Level 3 迁移到共享夹具——v27~v30 遗留的编译阻塞点闭合

- **日期**：2026-10-09
- **范围**：`tests/unit/test_fc_level3.c`、`tests/unit/fc_test_common.{c,h}`、文档（`fc_nand_ut.md`）
- **前置**：v26（共享夹具 `fc_test_common` 落地，Level 2 迁完）、v27~v29（CW 位图收敛 + 步骤函数化 + 参数化）、v30（Level 2 三组用例参数化）
- **生产代码**：**零改动**——纯测试重构。
- **介质格式**：无变更。
- **上板未验**：本项目硬规则。
- **测试**：**未编译**（用户要求重构期不编译）。`make format-check` 通过（101 文件）；
  `-fsyntax-only`（套 makefile 全量告警集）对全部 9 个 `tests/unit/*.c` **0 warning 0 error**。
  `make tests` 的编译阻塞点（`test_fc_level3.c` 引用已删符号）**已闭合**，但**未实跑链接**。

## 背景：为什么 Level 3 一直是「未编译」

v26 把 Level 2/3 的夹具抽成共享模块 `fc_test_common.{c,h}` 时，只完成了 Level 2 的全量迁移。
Level 3 被留下引用 7 个已删/改签名的符号：

| 旧符号 | v26 后的替代 |
|---|---|
| `test_page_cmd_ex` / `test_page_cmd` | `test_cmd_build()` + 页 helper |
| `test_run_lifecycle` | 夹具的 `test_submit_wait()` + `fc_command_release()` |
| `test_fill_user_from_source` | `test_fill_user_paa()`（按 desc->paa 填，天然含 block/page） |
| `test_verify_against_source` | 本文件新增 `verify_chain_paa()`（链驱动，见下） |
| `TEST_OP_READ` | `L3_CONF(OP_NORMAL_READ_PAGE_WITH_DATA_OUT, …)` |
| `test_write_page` | 夹具零调用者，已删；写走 `test_prog_page` |

另有 `test_read_page` 少传一个参数（v26 起补了 config 形参）、`test_erase_block` 的每个调用点
把 `0U` 当 config 传。这就是 v27~v30 四版 changelog 里反复出现的「⚠ `make tests` 仍编不过」。

## 改动

### A. 夹具补三件低层零件（`fc_test_common.{c,h}`）

页 helper 把「建链 → 搬数据 → 提交 → 轮询 → 校验 → 释放」打包好了，但 Level 3 有一批用例
**形态对不上**：READ_ID / GET_FEATURES / 状态查询 / 内部搬移 / RANDOM_DATA_READ——它们要么
不要 buffer 搬数据，要么一次只访问 1 个 CW（部分掩码），要么完成项本身就是要验的对象。
于是把打包好的流程拆出三个零件（原为 `static`，本轮去 `static` 并加声明）：

```c
uint32_t test_plane_cw_bitmap(uint8_t plane_bitmap);  /* plane 段 → flat CW 位图（plane1 单平面 ⇒ 0xF0） */
uint8_t  test_first_plane(uint8_t plane_bitmap);      /* 段内序号最小的 plane（test_cmd_build 的 addr.plane） */
fc_command_status_t test_submit_wait(fc_command_t *cmd); /* 提交 + 轮询到完成，命令仍归调用者 */
```

`test_plane_cw_bitmap()` 与 `test_cmd_build()` 内部「按 `cmd->pba` / `config.multi_plane` 反推段」
互为**交叉检查**：调用者正推、夹具反推，两边不一致就会在建链几何上炸出来。

### B. Level 3 的「裸页」config：`L3_CONF`（`test_fc_level3.c`）

改造前每个页 helper 各带一份 config，字段散落。本轮收成一个复合字面量宏，与改造前
`program_page` / `read_page` / `erase_block` 的字段**逐字一致**：

```c
#define L3_CONF(operation_, crc_)                             \
    ((const fc_cmd_conf_t){.operation = (DWORD)(operation_),  \
                           .crc       = (crc_),               \
                           .random    = FC_RANDOM_DISABLE,    \
                           .ldpc      = FC_ECC_MODE_LDPC_RAW, \
                           .raid      = FC_RAID_MODE_NORMAL})
```

- **不加扰 + LDPC RAW + 不 RAID**：裸页形态，数据原样落盘，比对的是明文。
- **crc 逐处给**：改造前裸页读写**不设** `config.crc`，而 `FC_CRC_ENABLE == 0` 是池清零后的
  默认 ⇒ 实际是**开**；cache 与 multi-plane 路径改造前显式关 CRC（纯 RAW 比对）。逐处显式写出，
  意图不再依赖「池清零」这个隐含前提。
- `ch`/`ce`/`lun` 由 `test_cmd_build` 从 `addr` 覆盖，`multi_plane` 由 operation 派生，故宏里不写。
- ⚠ cache helper 只取本 config 的**模式位**，`operation` 被逐步覆盖（填什么都行）。

用法：`&L3_CONF(OP_READ_ID, FC_CRC_ENABLE)`（复合字面量，可取地址）。

### C. 两个新校验函数取代 `test_verify_against_source`

**`verify_chain_paa(cmd)`——链驱动**：

```c
static void verify_chain_paa(const fc_command_t *cmd)
{
    const uint8_t n = test_cw_count(cmd);
    for (uint8_t i = 0U; i < n; i++)
    {
        const fc_descriptor_t *desc = test_cw_desc(cmd, i);
        const uint8_t         *user = test_cw_user(cmd, i);
        munit_assert_not_null(desc);
        munit_assert_not_null(user);
        munit_assert_uint8(test_check_paa(user, desc->paa), ==, 1U);
    }
}
```

与 `test_verify_page_paa(data, block, page, plane_bitmap)` 的分工：

| | 期望怎么来 | 覆盖范围 | 适用 |
|---|---|---|---|
| `test_verify_page_paa` | 按 `(block, page, plane_bitmap)` **重算** | 段内**全部** CW | 整段读 |
| `verify_chain_paa` | 取 `desc->paa` 本身 | 链上**实际有的** desc | 部分掩码读 |

部分掩码读（如 RANDOM_DATA_READ 只抽 1 个 CW）**不能用** `test_verify_page_paa`：段内其余 CW
的源槽根本没被这次读填过，是陈旧值。

**订正**：被删的 `test_verify_against_source` 比的是**与页无关**的源表——「读错页」在旧判据下
**不可见**。`verify_chain_paa` 取 `desc->paa`（写路径填源槽用的就是它），天然含 block/page 维度，
第一次能抓到「读到了别的页」。

**`read_page_verify(block, page, plane_bitmap, erased, config)`——把两种判据分开**：

```c
static void read_page_verify(uint16_t block, uint16_t page, uint8_t plane_bitmap, uint8_t erased, const fc_cmd_conf_t *config)
{
    fc_command_t *cmd = test_cmd_build(&(nand_addr_t){.block = block, .plane = test_first_plane(plane_bitmap), .page = page},
                                       test_plane_cw_bitmap(plane_bitmap), (DWORD)page << CW_WIDTH, config);
    test_memset_read_buffer(cmd);
    (void)test_submit_wait(cmd);
    munit_assert_uint8(test_verify_cw_status_all_success(cmd), ==, 1U);  /* release 前逐 CW 查 SUCCESS */
    if (erased != 0U) { test_verify_erased(cmd); }
    else              { verify_chain_paa(cmd); }
    munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
}
```

**擦除页的判据只能是逐 CW `FC_CW_STATUS_SUCCESS`**：擦除页 META = 0xFF… ⇒ 数据路径 Pass 4 ①
自洽必然不过 ⇒ 该 CW 带 `MATA_ERROR`，**聚合 status 的错误掩码非零**。而 `FC_CW_STATUS_SUCCESS`
是**另一个位**、仍会置起。故擦除读走 `erased=1` 分支，在 `fc_command_release()` **之前**逐 CW
查 SUCCESS——用聚合判据（`test_read_block` 那套）会误判为失败。

### D. 逐条用例的迁移

| 用例 | 迁移方式 |
|---|---|
| `init_sequence` | RESET 走 `run_lifecycle`；READ_ID / READ_PARAMETER_PAGE 用 `test_cmd_build(…, 0x01U, 0U, …)`（1 个 CW）+ `test_submit_wait` + 逐 CW SUCCESS + `test_cw_user(cmd, 0)`；GET_FEATURES 用 `cw_bitmap = 0`（无 buffer）再设 `cmd->feature_addr` |
| `feature_write_read` / `status_query` | `test_cmd_build` + `test_submit_wait`；status_query 用 `L3_CONF(status_ops[i], FC_CRC_ENABLE)` |
| `internal_data_move` | 擦除 → `L3_CONF(OP_ERASE_BLOCK, …)`；逐页搬移 → 逐页 `test_cmd_build`（读/写内部搬移 op）+ `run_lifecycle` |
| `block_erase_verify` | 4 处 `test_erase_block(BLOCK_TEST, TEST_PLANE_BM(0), 0U)` → `…, &L3_CONF(OP_ERASE_BLOCK, FC_CRC_ENABLE))` |
| `block_write_read` | 写走 `test_prog_block(…, &L3_CONF(OP_NORMAL_PROGRAM_PAGE, FC_CRC_ENABLE), NULL)`；读循环 `read_page_verify(…, TEST_PLANE_BM(plane), 0U, &L3_CONF(OP_NORMAL_READ_PAGE_WITH_DATA_OUT, FC_CRC_ENABLE))` |
| `block_cache_ops` | 写侧 `test_prog_block_cache(…, &L3_CONF(OP_PROGRAM_PAGE_CACHE, FC_CRC_DISABLE), NULL)`；Phase 2 整段换成**一条** `test_read_block_cache(BLOCK_TEST, TEST_PLANE_BM(0), &L3_CONF(OP_READ_PAGE_CACHE_SEQ, FC_CRC_DISABLE), NULL)` |
| `block_multi_plane` | 三处 config 换 `L3_CONF(OP_*_MULTI_PLANE, FC_CRC_DISABLE)`，几何 `TEST_PLANE_BM_ALL` |
| `block_random_data` | 先 `read_page_verify` 载页，再逐采样 CW 用 `test_cmd_build(…, 1U << cw, page << CW_WIDTH, &L3_CONF(OP_RANDOM_DATA_READ, FC_CRC_ENABLE))` + 断言链长 == 1 + `verify_chain_paa` |

`write_page`（夹具零调用者的旧薄封装）随迁移删除。9 条注册场景数**不变**。

### E. 新增 cache 整块读写 helper（`fc_test_common.{c,h}`）

`test_prog_block_cache()` / `test_read_block_cache()`——**只有单 plane 变体**（ONFI 无 multi-plane
cache 读/编程）。

- **写侧**：前 `PAGES_PER_BLOCK-1` 页 `OP_PROGRAM_PAGE_CACHE`（80h-addr-data-15h），末页
  `OP_NORMAL_PROGRAM_PAGE`（10h）收尾——15h 编的是 cache 寄存器里的当前内容，**没有末页的 10h
  就没有最后一页真正落盘**。
- **读侧**：**不能**由 `test_read_page` 循环拼出——31h / 3Fh **无地址周期**（设备自增），且 31h
  输出的是**上一轮加载的页**，「命令地址 = 页、校验该页」的模型在此结构性不成立。故显式编排
  `START(p0) → SEQ ×(PAGES_PER_BLOCK-1) → LAST`：
  - START（00h-addr-30h）把 page 0 载入 cache，**不输出数据**（`cw_bitmap = 0`，FC 不跑 process_read）；
  - 第 i 次 SEQ 输出 page i-1、同时载入 page i ⇒ `meta` 取 **(i-1) << CW_WIDTH**（真正输出的那页）；
  - LAST（3Fh）输出 cache 里最后一页。
  - 共 **65 条命令输出 64 页**。
- 两者都是**同步**的（逐条 submit → 轮询 → 释放）⇒ cache 的流水线收益在本夹具里**测不到**；
  这里验的是**数据正确性**：cache 命令序列走得通、META / CRC / 加扰 / ECC 全程不串。

## 铁律提醒（本次迁移直接相关）

FC 自己会跑数据路径 ⇒ **提交出去的命令绝不能再手工调** `fc_data_path_encode()` /
`fc_data_path_process_read()`（或 v29 的步骤原语）——重复编码会破坏数据。Level 3 全程走
`test_cmd_build()` → `test_submit_wait()`，不碰数据路径。

## 未跟进

- **未编译、未跑 `make tests`**（用户要求）；上板未验。
- `#if 0` 段（`test_unlock_erase` / `test_block_lock_tight` / `cmd_snapshot` / `cmd_diff_report`
  及未注册的套件行）**原样保留**，其中的 `test_run_lifecycle` 引用仍在 `#if 0` 内。
- 内部搬移 / cache 序列的**异步流水**（一次 submit 多条、批量轮询）未覆盖。

# v32：Level 3 去转发层 + 随机化参数化（munit 种子驱动）

- **日期**：2026-10-10
- **范围**：`tests/unit/test_fc_level3.c`、`src/platform/stm32h743/usart.c`（帮助串一词）、文档（`fc_nand_ut.md`）
- **前置**：v31（Level 3 迁到共享夹具，编译阻塞点闭合）、v30（Level 2 三组用例参数化）
- **生产代码**：除 `usart.c` 的 `test` 帮助串补 `--single` 外**零改动**——纯测试重构。
- **介质格式**：无变更。
- **测试**：`make format-check` 过（101 文件）；`make clean && make tests` **0 warning 0 error**、
  链接 **201580 B**（v31 基线 199176 B）；RAID=1 档（直调子 make）**0/0**、**209180 B**。
  **上板未验**（本项目硬规则：`make tests` 只证链接，上板才是终审）。

## 背景：两件事

**一、本地 helper 里有一层纯转发。** 夹具 `fc_test_common` 已经提供 `test_prog_block` /
`test_prog_block_cache` / `test_read_block`（签名一律 `(block_index, plane_bitmap, config, data)`），
而 Level 3 又在上面套了一层：

```c
static void write_block(uint16_t block, uint8_t plane)       { test_prog_block(block, TEST_PLANE_BM(plane), &L3_CONF(...), NULL); }
static void write_block_cache(uint16_t block, uint8_t plane) { test_prog_block_cache(block, TEST_PLANE_BM(plane), &L3_CONF(...), NULL); }
static void read_block_verify(uint16_t block, uint8_t plane) { /* 64 页循环，与 test_read_block 重合 */ }
static void run_lifecycle(fc_command_t *cmd)                 { (void)test_submit_wait(cmd); munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK); }
```

唯一附加值是把 `plane` **索引**换成 `TEST_PLANE_BM(plane)` **位图** + 烘焙一个 `L3_CONF`，
顺带在夹具的「一律位图」约定之外引入了第二套「索引」约定。

**二、9 条注册用例把地址全写死。** `BLOCK_TEST=100` / `BLOCK_DEST=102`、plane 位图 `0x01`、
页 `0..63`、CW 位图 `0x01`。后果：每条用例只在一个地址元组上自证——**只在 plane 1、只在奇数页、
只在 CW2、只在别的块上才暴露的回归，这个套件永远看不见**。

## 约束（用户明确）

- **不加层**：不建命令工厂、不建资源模块、不引入块池抽象；**也不统一 plane 约定**——helper 一律
  直接用夹具那套 `plane_bitmap`，`plane` 索引只在用例体里用 `TEST_PLANE_BM(plane)` 转一次。
- Level 3 的 setup/teardown 继续用 `fc_test_common` 的 `test_setup` / `test_tear_down`
  ——**和 Level 2 是同一份**，本轮一行不动。
- 夹具（`fc_test_common.{h,c}`）、`ut_main.c`：**零改动**。

## 改动

### A. 去层：删 4 个纯转发 / 本地 helper

| 删除 | 调用点改为 |
|---|---|
| `write_block(block, plane)` | `test_prog_block(block, TEST_PLANE_BM(plane), &L3_CONF(OP_NORMAL_PROGRAM_PAGE, FC_CRC_ENABLE), NULL)` |
| `write_block_cache(block, plane)` | `test_prog_block_cache(block, TEST_PLANE_BM(plane), &L3_CONF(OP_PROGRAM_PAGE_CACHE, FC_CRC_DISABLE), NULL)` |
| `read_block_verify(block, plane)` | `test_read_block(block, TEST_PLANE_BM(plane), &L3_CONF(OP_NORMAL_READ_PAGE_WITH_DATA_OUT, FC_CRC_ENABLE), NULL)` |
| `run_lifecycle(cmd)` | `(void)test_submit_wait(cmd); munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);` |

⚠ **换 `test_read_block` 会换判据**（收进夹具的代价，属**加强**）：`read_block_verify` 走
`read_page_verify` → `test_verify_cw_status_all_success`（只看 READY 位）+ `verify_chain_paa`
（链驱动）；`test_read_block` 逐页断言聚合 `completion.status & FC_CW_STATUS_ERROR_MASK == 0`
（更严）+ `test_verify_page_paa(g_source_data, block, page, plane_bitmap)`（按坐标**重算**期望）。

**保留**（有真实逻辑，不是转发）：
- `verify_chain_paa`（链驱动 PAA 校验）——`block_random_data` 的部分掩码读只有它能用。
- `read_page_verify`（建链 / 预填 0xCC / 提交 / 双判据 / 释放）。
- `read_block_verify_erased`——**夹具没有对应原语**：`test_read_block` 的聚合 `ERROR_MASK`
  判据在擦除页上必然失败（META=0xFF ⇒ Pass 4 ① 不过 ⇒ 带 `MATA_ERROR`）。签名里的 `plane`
  索引按约束改成 `plane_bitmap`。
- `unlock_block`（零活调用者，唯一调用者在 `#if 0` 里）——保留原样。

### B. 参数化（值表 + 取值函数都在 `test_fc_level3.c` 里）

值一律是 NULL 结尾的 `const char*` 数组；`MunitParameterEnum` 数组**非 const**
（`munit.h:436` 的 `MunitTest.parameters` 是非 const 指针）。本 fork 的 typedef 是
`const char *name; const char **values;`（`munit.h:423-427`）⇒ **不需要 `(char*)` 转换**，
与 Level 2 v30（`test_fc_level2.c:235/241`）同形。munit 上游文档里那份带 `(char*)` 的示例是
上游旧版 typedef 用 `char*` 的遗留。

| 参数 | 取值 | 用在 |
|---|---|---|
| `block` | `100` / `104` | 除 multi-plane 外的地址相关用例 |
| `plane` | `0` / `1` | `block_erase_verify` / `block_write_read` / `block_cache_ops` / `block_random_data` |
| `sample` | `edges` / `spread` / `middle` / `dense` | 仅 `block_random_data`（页集 + 列集；`spread` 逐字等于改造前现状，留作回归基线） |

- `l3_param_block` / `l3_param_plane`：`munit_parameters_get` + 断言挡表外名字。
- `sample` 走 Level 2 v30 的**查表**形态：`l3_sample_case_t`（name + pages/page_count +
  cws/cw_count），`_Static_assert` 钉死「名字表条数 == 场景表条数」，`l3_sample_case_find()`
  线性查找 + `munit_assert_true(c != NULL)`。

块池**不能**放开到 `BLOCKS_PER_PLANE`：跑 UT 时 FTL 是活的（`src/main.c` 无条件
`ssd_ftl_boot_all()`，`test` 命令从主循环的 `uart_service_poll()` 进来），free pool 覆盖整片
device。这是**既有**问题（改造前就写 100/102），参数化只是多写了 104/106。

### C. 随机性全走 munit 的种子（不在用例体内）

| 事实 | 位置 |
|---|---|
| `runner.seed` 一次运行一个值，默认由 `time(NULL)` 派生、开场打印，`--seed N` 可指定 | `munit.c:1395` `:1419` `:1608` |
| `munit_rand_seed(runner->seed)` 在**参数展开前**调用 | `:1066` |
| 同上，在**迭代循环前**再次调用（⇒ 每个用例体都从同一状态起步） | `:715` |
| `--single` ⇒ 每个参数枚举**随机挑一个值**：`munit_rand_at_most(munit_str_hash(test_name), n-1)`；**salt = 用例名哈希**，故各用例挑到的组合互不相同 | `:1139` `:1500-1502` |
| 无 `--single` ⇒ 全组合枚举（笛卡尔积，`run_test_wild` 递归展开） | `:1160` `:970-1001` |
| CLI `--param name value` **优先于**随机挑值（`filled == 1` 先短路） | `:1102-1120` |
| 框架**不校验** `--param` 的值，表外名字直接进用例 ⇒ 用例须自查 | `:1102-1115` |
| 导出的 PRNG 只有 `munit_rand_uint32` / `munit_rand_memory` / `munit_rand_int_range` / `munit_rand_double`；**带 salt 的 `munit_rand_at_most` 是 static，不导出** | `munit.h:465-479` |
| `runner.iterations` 默认 0 ⇒ 回落到 suite 的 `iterations`（`ut_main.c` 里 = 1）⇒ 去掉 `SINGLE_ITERATION` 不改迭代次数 | `:1375` `:706-709` |

⇒ 随机性只走「参数取值」这一条路：可复现、各用例天然互不相关、`--single` 下每轮成本 = 现状。
⚠ 用例体内**不调** `munit_rand_*`：`exec` 在用例体前把 PRNG 重置回 `runner->seed`（`:715`），
而导出的 `munit_rand_int_range` 不带 salt ⇒ 两个用例体内的**第一个**随机数完全相同。

### D. 注册行

参数化行改成 `MUNIT_TEST_OPTION_NONE` + 参数数组；`init_sequence` / `feature_write_read` /
`status_query` 是定长命令序列、地址不参与，**保持** `SINGLE_ITERATION` + `NULL` 不动。
组合数（不加 `--single`）：`block` 档 2 组/用例，`block×plane` 档 4 组，`block×plane×sample`
档 16 组；`--single` 下每用例恒 1 组。

### E. `usart.c` 帮助串

`test` 命令的帮助串补 `--single`（此前只有 `[--seed N] [--list] [--list-params]`）。

## 修复的既有缺陷

- **`internal_data_move` Step 5 的期望坐标**：原 `test_read_block(dest)` 的期望值是**目标块坐标**
  的 PAA（`test_verify_page_paa` 按入参 block 重算，`fc_test_common.c:644-675`），而 copyback 搬的
  是源页内容（`READ_FOR_INTERNAL_DATA_MOVE` / `PROGRAM_FOR_INTERNAL_DATA_MOVE` 都不是 page
  read/write，数据路径不介入、不重铸）⇒ 目标页的 USER 仍是**源块坐标**的 PAA，两者**必然对不上**。
  改造前用 `verify_chain_paa` 同样对不上（`desc->paa` 也是目标块坐标），只是那时「4 字节周期 pattern +
  源表与页无关」把它掩盖了；v26 换成「数据 = 地址」后暴露。
  **改法**：Step 5 逐页 `test_read_page(dest, TEST_PLANE_BM(0), page, &L3_CONF(OP_NORMAL_READ_PAGE_WITH_DATA_OUT, FC_CRC_ENABLE), NULL)`
  （断聚合 `FC_CW_STATUS_ERROR_MASK == 0`）+ `test_verify_page_paa(g_source_data, src, page, TEST_PLANE_BM(0))`
  （期望取**源块**）。不新增夹具原语——`test_read_page` / `test_verify_page_paa` / `g_source_data`
  都是既有导出件。
  **数据路径侧照常通过**（已逐行核对）：`fc_data_path_meta_check` 的自洽项取 `paa_to_plane/paa_to_cw(desc->paa)`
  （源/目标同 plane、同 cw 枚举）、LBA 项比的是同页 META ⇒ Pass 4 两项都过；CRC 保护 USER 且原样搬过 ⇒ 一致。

## 标注的既有缺陷（本轮只标注，不改判据）

- **`block_erase_verify` 无前置写**：直接擦完读擦除态，判据弱且依赖前序用例的残留状态。
- **multi-plane 的 BA6 约束**：Micron datasheet 要求 two-plane 两条地址的最低块地址位（BA6）
  不同，而 `ftl_cmd_configure_descriptors`（`src/ftl/ftl_fc_interface.c:93`）里两 plane 共用
  同一个 block（只翻 plane 位）。项目侧怎么满足这条未查证，不在本轮范围。

## 上板（项目硬规则，由用户执行）

```
ssd> test --list-params /fc-nand/lifecycle/                 # 看参数词汇表
ssd> test --single /fc-nand/lifecycle/                      # 每用例按种子随机抽一组
ssd> test --single --seed 0x1a2b3c4d /fc-nand/lifecycle/    # 复现上一轮
ssd> test /fc-nand/lifecycle/                               # 全枚举（耗时长，按需）
ssd> test --single --param block 104 /fc-nand/lifecycle/    # 定点覆盖（CLI 值优先于随机）
```

判定：`--single` 与全枚举都必须 0 失败；记下 `--seed` 值以便复现。

## 未跟进

- 上板未验。
- `#if 0` 段（`test_unlock_erase` / `test_block_lock_tight` / `cmd_snapshot` / `cmd_diff_report`）
  原样保留，其中的 `test_run_lifecycle` / `BLOCK_TEST` 引用仍在 `#if 0` 内。
- 块池与活 FTL 的冲突（根治要在 FTL 侧预留测试块）未做。

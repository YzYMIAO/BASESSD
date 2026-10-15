# v40：Level 3 地址随机化——用例体内 salted PRNG，窗口 30%~70%

- **日期**：2026-10-11
- **范围**：**测试 + 文档**——`tests/unit/test_fc_level3.c` + 文档（`fc_nand_ut.md` /
  `assertion_guidelines.md`）；另 **`src/platform/stm32h743/usart.c` 一行**（`test` 帮助串补
  `--iterations`，见下）。**`src/ftl/**` 一行不动**，其余 `src/**` 无逻辑改动。
- **介质格式**：无变更。
- **测试**：**用例数 77 → 51**（Level 3 **35 → 9**；Level 1 = 11、Level 2 = 31 不变）。
  `make format-check` 过；`make clean && make tests` **0 warning 0 error**、链接 **219836 B**；
  `make clean && make`（固件）**0 warning 0 error**、**166108 B**。
- ⚠ **需上板**：这是本版唯一的验收方式（随机地址 + 证据打印）。

## 一、动机：地址选取太固定

Level 3 的 6 条地址相关用例原先挂在 `MunitParameterEnum` 上，取值只有：

| 参数 | 取值 | 用在 |
|---|---|---|
| `block` | `100` / `104` | 除 multi-plane 外的地址相关用例 |
| `plane` | `0` / `1` | `block_erase_verify` / `block_write_read` / `block_cache_ops` / `block_random_data` |
| `sample` | `edges` / `spread` / `middle` / `dense` | 仅 `block_random_data`（页集 + 列集） |

两档块号、内部搬移目标恒 = 源 + 2 —— **地址维度基本没在动**。用户要求：**按 munit 的种子随机化**，
且 **block 落在每 plane 块数的 30%~70%** 以避开元数据区。

三个已确认的决定：

| 问题 | 选择 |
|---|---|
| 随机化走哪条路 | **用例体内 `munit_rand_*`**（不扩参数表）——与 Level 2 的 `test_scramble_whiten` 同法 |
| 30%~70% 的基数 | **per-plane 2048**（地址里的 `block` 字段就是 per-plane 号）⇒ 窗口 **614..1433** |
| 「其他选取」范围 | **内部搬移目标块** + **`block_random_data` 的页集/CW 集** + **plane** |

**为什么参数表做不到**：`MunitParameterEnum.values` 是 NULL 结尾的 `const char*` 数组，表达不了
820 宽的窗口，只能写死几个候选值——那还是「固定选取」。所以必须改成用例体内抽样。

## 二、为什么必须给抽样**加盐**（本版的核心坑）

munit fork 在每次 `exec` **之前**把 PRNG 重置回 `runner->seed`（`munit.c:774`），而**导出的**
取数函数 `munit_rand_uint32` / `munit_rand_int_range` **不带盐**（带盐的 `munit_rand_at_most`
是 `static`，`munit.c:503`，不导出）。后果：

> **用例体内的第一次抽样，在所有用例之间取值完全相同**——六条用例会抽到**同一个块**，
> 整轮只出现**一个 plane**。

Level 3 文件头此前因此写着「⚠ 用例体内**不用** `munit_rand_*`」。但那是**过度概括**：
Level 2 早就在用例体内用了（`test_scramble_whiten` / `test_crc_step_detect` / `ecc_step`）——
那几条**只有一条用例在抽**，撞不上，所以不需要盐。

**修法**：本文件自带 `l3_rand(test, tag)`：

```c
static uint32_t l3_rand(const char *test, const char *tag)
{
    return munit_rand_uint32() ^ l3_hash(test) ^ l3_hash(tag);
}
```

- `l3_hash` 是 DJB2，与 `munit.c:386` 的 `munit_str_hash` **逐字同实现**（那份是 `static`，只能照抄）。
- **用例名**让用例之间互异；**用途标签**（`"plane"` / `"block"` / `"dest"` / `"npages"` / `"page"` /
  `"ncws"` / `"cw"`）让同一用例内各用途互异。

**复现契约 = (seed, 用例名, 第几次抽样)**：reseed 是**每用例一次**，故复现单条用例**不需要**
重放其它用例（比 v1.15「参数按用例名哈希挑值」更强）。

## 三、改动清单（`tests/unit/test_fc_level3.c`）

### 删

- 参数表 `l3_blocks` / `l3_planes` / `l3_samples` 与三个 `MunitParameterEnum`；
- 取值函数 `l3_param_block` / `l3_param_plane`；
- 抽样策略段 `l3_sample_case_t` + 四张页表 / 三张 CW 表 + `_Static_assert` + `l3_sample_case_find`；
- `#define L3_BLOCK_DEST_OFF (2U)` 与「测试块池」注释。
- **保留** `BLOCK_TEST` / `BLOCK_DEST`——`#if 0` 段（`unlock_erase` / `block_lock_tight`）还在引用。

### 加

**窗口常数**（编译期由 `BLOCKS_PER_PLANE` 算出，不写死数字）：

```c
#define L3_BLOCK_LO ((uint16_t)(BLOCKS_PER_PLANE * 30U / 100U)) /* 2048 ⇒ 614 */
#define L3_BLOCK_HI ((uint16_t)(BLOCKS_PER_PLANE * 70U / 100U)) /* 2048 ⇒ 1433 */
_Static_assert(L3_BLOCK_LO < L3_BLOCK_HI, "随机窗口为空");
_Static_assert(L3_BLOCK_HI < BLOCKS_PER_PLANE, "随机窗口越过本 plane 块数");
```

**抽样函数**：

| 函数 | 抽什么 | 备注 |
|---|---|---|
| `l3_block_draw(test)` | 窗口内一个块号（闭区间 `[614,1433]`） | 标签 `"block"` |
| `l3_plane_draw(test)` | `[0, PLANE_NUM)` 一个 plane | 标签 `"plane"` |
| `l3_dest_draw(test, src)` | 内部搬移目标块：同窗口**另抽**，撞上 `src` 就**环移一格** | **不额外抽** ⇒ 抽样次数固定 |
| `l3_pages_draw(test, out)` | n ∈ [2,8] 个**互不重复**的页（`[0,64)`），返回 n | 拒绝采样 |
| `l3_cws_draw(test, out)` | n ∈ [1,4] 个**互不重复**的 CW（`[0,4)`），返回 n | 拒绝采样 |

`l3_dest_draw` 的环移：`src ∈ [LO, HI]` 且 `dest == src` ⇒ `dest+1`，越过 `HI` 时回卷到 `LO`。
窗口 span = 820 ≥ 2 ⇒ 环移后必 `≠ src`，且**不消耗额外随机数**（否则「第 i 次迭代看到第 i 段
随机流」这句复现契约就说不清了）。

### 抽样顺序即复现契约

| 用例 | 顺序 |
|---|---|
| `internal_data_move` | ① plane ② block ③ dest（**plane 由固定的 0 改成抽到的值**） |
| `block_erase_verify` / `block_write_read` / `block_cache_ops` | ① plane ② block |
| `block_multi_plane` | ① block（plane 仍由 multi-plane op 派生） |
| `block_random_data` | ① plane ② block ③ 页集（先抽个数再抽元素）④ CW 集（同） |

六条用例体各加 `(void)params;`（`-Wextra` 开着）。主循环（`block_random_data` 的页 × CW 探针）
**不再消耗随机流**。

**上限不变**：8 页 × 4 CW = 32 个探针，与旧 `dense` 档的最坏值相同 ⇒ **板上耗时上界不变**。

### 注册表

三处参数列 `l3_block_params` / `l3_block_plane_params` / `l3_rnd_params` → **`NULL`**；
**保留 `MUNIT_TEST_OPTION_NONE`**（`--iterations N` 才是覆盖旋钮，换成 `SINGLE_ITERATION`
会把旋钮锁死）。

### 证据打印（`--log-visible info`）

**不打印就无法从 seed 复现**，故每条随机化的用例都补/改了一句 `test_trace_summary`：

| 用例 | 动作 |
|---|---|
| `internal_data_move` | 改：plane / src / dest（点明 `src≠dest`）/ 页数 / CW 数 |
| `block_write_read` | 保留（已打 block/plane） |
| `block_erase_verify` | **补**一条摘要 |
| `block_cache_ops` | **补**一条摘要 |
| `block_multi_plane` | 保留（已打 block） |
| `block_random_data` | 改：plane / block / **64 位页掩码** / **CW 掩码** / 探针数 |

`block_random_data` 的两个掩码由抽到的集合**就地算出** ⇒ 掩码的 popcount **等于**打印的个数，
**顺带自证互不重复**。页掩码是 64 位，**拆两个 `%08lX` 打印**——newlib-nano 不认 `%llX`
（见 memory `newlib-nano-no-longlong-printf`）。

### `src/platform/stm32h743/usart.c`（一行，唯一的 `src/**` 改动）

`test` 帮助串原先列了 `--seed` / `--single` / `--list` / `--list-params` / `--log-visible`，
**没有 `--iterations`**——而它正是本版 Level 3 的覆盖旋钮。补上，位置对齐 munit 自己的
`munit_print_help`（`munit.c:619-636` 的顺序是 `--seed` → `--iterations` → `--param` → …）：

```c
"test [--seed N] [--iterations N] [--single] [--list] [--list-params] [--log-visible info|debug] [test_name...]"
```

⚠ 帮助串变长 ⇒ clang-format 的 `AlignArrayOfStructures: Right` 把**整张 CLI 表**重新列对齐
（只有空白变化，内容不变）。这是 `make format-check` 的硬要求，不是手改。

## 四、判据 7 的再应用

本改动是 `assertion_guidelines.md` **§二十三 判据 7** 的应用（同一断言、不同输入 ⇒ 用**迭代**
`--iterations N`，不用参数化）。复查中发现：**旧的 block/plane 参数化本身就不合判据 7**——
`100` 与 `104`、plane `0` 与 `1` 的断言**完全相同**，正是判据 7 警告的「一堆重复项」；
`sample` 是边界情形（探针集不同，勉强算参数化）。已在该节补一段如实记录（含下面的取舍）。

## 五、代价（如实记录）

1. **用例数 77 → 51**（Level 3 **35 → 9**）。默认一轮每条只抽一次；覆盖改由 `--iterations N` 给：
   `test --iterations 20 /fc-nand/lifecycle/` ⇒ 六条随机化用例的**用例体各跑 20 次**。
   ⚠ **报告里的条数不变（仍 `9 of 9`）**——本版初稿在此写成「`3 + 6×20 = 123` 条」，是**错的**，
   2026-10-11 上板后按 `munit.c:803` / `:989` 与实测订正，见
   `docs/archive/changelog/fc_ut_board_verify_v42.md`。
   ⚠ `--iterations` 是 **runner 级全局量**，且 `runner->suite` **恒指根套件**（`munit.c:1359`）
   ⇒ 子套件自己的 `iterations` 字段无效，**别去改 `ut_main.c`**（改了会把 Level 1/2 也乘 N）。
   它按调用范围生效：只跑 `/fc-nand/lifecycle/` 就只乘 Level 3。
2. **失去 `--param block N` 钉值**。替代：`--seed` + 打印出来的取值复现；一次性排查可临时把抽样
   改成常数。
3. **单轮 plane 覆盖有限**：只有 5 条抽 plane（`block_multi_plane` 不吃）⇒ 某一轮**一个 plane 都
   没抽到**的概率 ≈ `C(5,0)·2^-5 ≈ 3.1%`。加 `--iterations` 即可。这是「不再枚举」换来的取舍。
4. **块碰撞无害**：同轮各用例拿到不同块靠 salt；碰撞概率 ≈ `C(6,2)/820 ≈ 1.8%`，且用例串行、
   各自先擦后写。

## 六、⚠ 既有风险：FTL 在 UT 期间是活的（本版只是放大了暴露面）

`src/main.c` 无条件 `ssd_ftl_boot_all()`，`test` 命令从主循环的 `uart_service_poll()` 进来
⇒ free / OP / spare 池**覆盖整片 device**。

- 窗口 **614..1433 只保证避开固定元数据区**（本仓 per-plane 编号下 0..41 一带：L2P 0..31 /
  Anchor 33,34 / BBT 36,39 / Checkpoint 37,38 / Journal 40,41，见 `src/ftl/metadata.c`）；
- **动态** L2P region 块（≥32 的任意块）与 FTL **用户数据块** —— **拦不住**。

这是**既有**风险（改造前就写死 100/102），窗口只是把暴露面从 **2 个块**放大到 **820 个**。

**未做的加固**（按硬规则 `src/ftl/**` 不动，且收益不足）：测试里用
`ftl_is_metadata_block(g_ctrl.ftl[0][0][0]->metadata_dir, block)` 重抽、把**已登记**的动态元数据块
排掉——它编译链接都通，但**排不掉 FTL 用户数据块**（那才是主要风险），且会重新引入
测试 → FTL 依赖（v32「去转发层」刚拆掉的）。**只记录不做。**

## 验证

```bash
make format-check                  # 过
make clean && make tests           # 0 warning 0 error；链接 219836 B
make clean && make                 # 固件 0 warning 0 error；166108 B
```

**宿主机复核抽样逻辑**（无板可跑，用一份复制品验不变式）：把 `l3_hash` / `l3_rand` / 五个
`l3_*_draw` 连同 munit 的 PRNG 抄进一个 host 程序，对 4 个种子逐个跑六条用例，断言：

- 六条抽到的 `block` **都在 614..1433**；
- `internal_data_move` 的 **`src ≠ dest`** 且都在窗口内；
- `block_random_data` 的页集 / CW 集**互不重复**，个数落在 `[2,8]` / `[1,4]`。

结果：**全部通过**（例：seed 0 ⇒ `internal_data_move plane=0 block=741 dest=679`；
`block_random_data plane=0 block=1053 n_pages=3[37 63 30] n_cws=1[2]`）。

### 上板（**这是真正的验收**，由用户跑）

```
test --log-visible info /fc-nand/lifecycle/                        # 9 条，各自一个随机地址
test --log-visible info --iterations 5 /fc-nand/lifecycle/         # 六条各跑 5 次（报告仍 9 of 9）
test --log-visible info --seed 0x<上一轮打印的> /fc-nand/lifecycle/  # 复现同一组地址
```

验收点：六条打印的 `block` 都在 **614..1433**；`internal_data_move` 的 **src ≠ dest** 且都在窗口内；
`block_random_data` 打印的两个掩码的 **popcount == 打印的个数**。

## 不做

- 不动 `src/ftl/**`（一行不动）；其余 `src/**` 只改了 `usart.c` 的那行帮助串；
  不动 `tests/framework/munit.*`（不导出带盐 API——留在测试文件里，避免 fork 漂移）；
  不动 `ut_main.c`（**v40 自身不动**——随后 v41 只改了三个套件前缀字面量，
  见 `docs/archive/changelog/fc_ut_suite_prefix_slash_v41.md`）；不翻 `CONFIG_FC_RAID`；不重写既有 changelog 正文。

## 出处

- 判据 7（参数化 vs 迭代）：`docs/design/assertion_guidelines.md` §二十三。
- munit fork 的 reseed / salt 机制：`tests/framework/munit.c:774`（reseed）、`:386`（`munit_str_hash`）、
  `:503`（`munit_rand_at_most`）、`:1198`（参数按用例名哈希挑值）。
- 上一版 Level 3 形态（参数化 + 去转发层）：`docs/archive/changelog/fc_ut_level3_param_v32.md`。
- 证据打印：`docs/archive/changelog/fc_ut_evidence_trace_v36.md`。
- 元数据块布局：`src/ftl/metadata.c` 的 `ftl_is_metadata_block`。

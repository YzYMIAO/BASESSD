# v41：子套件前缀补尾斜杠——打印路径与过滤串修正

- **日期**：2026-10-11
- **范围**：**测试注册 + 文档**——`tests/unit/ut_main.c` 三个字符串字面量 + 一段注释；
  `docs/design/{fc_nand_ut.md,munit_adapter.md,wsl_debug_environment.md}`。
  **`src/**` 一行不动**（含 `src/ftl/**`）；`tests/framework/munit.*` 一行不动；
  其余测试文件（`test_fc_level1/2/3.c`）一行不动。
- **介质格式**：无变更。
- **测试**：用例数不变（**51**）；`make format-check` 过；`make clean && make tests`
  **0 warning 0 error**、链接 **219836 B**。
- ⚠ **需上板**：`test --list` 可一眼确认用例名（无需跑用例）。

## 一、现象

串口打印的用例全路径**粘在一起**：

```
/fc-nand/lifecycleblock_multi_plane:      ← 实际
/fc-nand/lifecycle/block_multi_plane:     ← 应该是
```

## 二、根因：`munit_maybe_concat` 不补分隔符

`munit_maybe_concat`（`tests/framework/munit.c:314-339`）是**纯首尾相接**（照抄上游，无分隔符逻辑），
两处调用点：

| 位置 | 拼接 | 作用 |
|---|---|---|
| `munit.c:1285` | `pre = prefix + suite->prefix` | 下探子套件 |
| `munit.c:1103` | `test_name = pre + test->name` | 拼用例名（`:1128` `fputs` + `fputc(':')` 打印） |

⇒ **完整路径 = 各级前缀与用例名的逐字相加，谁都不补 `/`**。本仓的**用例名不带前导斜杠**
（`test_fc_level3.c:1317` 的 `{"block_multi_plane", …}`），所以**每一级套件前缀都必须自己带尾斜杠**：

| 级 | 前缀 | 状态 |
|---|---|---|
| 根套件 | `"/fc-nand/"` | ✅ 一直带尾斜杠 |
| 三个子套件 | `"logic"` / `"data_path"` / `"lifecycle"` | ❌ 漏了尾斜杠 ⇒ 本 bug |

拼接结果：`"/fc-nand/" + "lifecycle" + "block_multi_plane"` = `/fc-nand/lifecycleblock_multi_plane`。

## 三、改动

### `tests/unit/ut_main.c`（唯一代码改动）

三个字面量补尾斜杠，并加一段警示注释（写清 `munit.c:1285` / `:1103` 两处拼接点）：

```c
/* ⚠ 子套件前缀**必须以 '/' 结尾**：munit 只把前缀和用例名**首尾相接**
 * （munit.c:1285 下探子套件、:1103 拼用例名），**不补分隔符**。根前缀已带尾斜杠；
 * 子套件漏了就会打印成 "/fc-nand/lifecycleblock_multi_plane"，且过滤串
 * "/fc-nand/lifecycle/…" 一条都匹配不上（用例名不带前导斜杠）。 */
static MunitSuite g_fc_nand_suite =
    {
        (char *)"/fc-nand/",
        NULL,
        (MunitSuite[]){
                       {(char *)"logic/", g_fc_logic_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE},
                       {(char *)"data_path/", g_fc_data_path_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE},
                       {(char *)"lifecycle/", g_fc_lifecycle_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE},
                       {NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE},
                       },
        1,
        MUNIT_SUITE_OPTION_NONE,
};
```

**用例名 / 断言 / 用例数一律不变**（仍 51）；不动 `tests/framework/munit.*`（这是**使用者**的约定问题，
不是框架 bug）。

### 文档

- `docs/design/fc_nand_ut.md`：§3 / §4.1 两处套件树补尾斜杠 + 一段「前缀必须带 `/`」的警示；
  §4.2 的 CLI 示例补「**不加过滤串**才是全套」与两条过滤串约束；§7 新增 v1.23 行。
- `docs/design/munit_adapter.md`：新增 **v1.5** 节——路径构造（两处拼接点）、过滤匹配的两条约束、
  已知缺陷（只记录不修）、副作用。
- `docs/design/wsl_debug_environment.md`：§4.2 的 `test` 示例修正（补尾斜杠、删失效的 `--param block 10`、
  `test --log-visible info /fc-nand/` → `test --log-visible info`）+ §9.4 加一行。

## 四、副作用与边界（如实记录）

### (a) 过滤串的行为变更

| 过滤串 | 改前 | 改后 |
|---|---|---|
| `/fc-nand/lifecycle/` | ❌ 不匹配 | ✅ 该套件全部 |
| `/fc-nand/lifecycle/block_write_read` | ❌ 不匹配 | ✅ 单条 |
| `/fc-nand/lifecycle`（缺尾斜杠） | ✅ 该套件全部 | ❌ 不匹配 |

即「文档里一路写的带斜杠形式」由**全部失效**变成**全部可用**；代价是**不带**尾斜杠的形式反转。
两者都不是新东西——文档（`fc_nand_ut.md` / `munit_adapter.md` / `wsl_debug_environment.md`）
一直写的是**带斜杠**形式，而历史 changelog（v33 / v34）里记的是**粘连**形式（那才是当时真能跑的）。
按「`docs/changelog/*` 是历史，不改正文」的规则，历史记录保留原样。

### (b) `--single` 档的参数挑值会变

`munit.c:1198` 用 `munit_str_hash(test_name)` 给参数挑值，全路径变了 ⇒ 挑到的值可能不同。
**仅 `--single` 档可见**（默认档是全组合枚举）。v40 的 Level 3 盐 `l3_rand(test, tag)` 用的是
**裸用例名**字面量，与套件路径无关 ⇒ **不受影响**。

### (c) 已知缺陷：`/fc-nand/` 匹配不了任何用例（**只记录不修**）

过滤串（9 字节）比子套件路径（`/fc-nand/lifecycle/`，19 字节）**短**，
`munit.c:1302` 的 `strncmp(pre, filter, pre_len)` 走到 `filter` 的 `'\0'` 就判不等
（`strncmp` 遇 `'\0'` 即停）⇒ 根套件名单独用作过滤串**一条都跑不了**。
这与 `munit.c:1275` 自述的「用户指定的测试名与**完整路径**进行前缀匹配」不符——
正确语义应是「用例完整路径以过滤串为前缀」（`strncmp(pre + test->name, filter, strlen(filter)) == 0`）。

**修它要动 `tests/framework/munit.c`**（框架侧），本轮**不做**：改动面超出「前缀约定」这件事，
且**跑全套有现成的替代**——**不传过滤串**的 `test`。文档已按此改正（原先多处示例写
`test /fc-nand/` 并声称「全套」，是错的）。

### (d) 历史文档里的粘连形式

`docs/changelog/fc_crc_hal_driver_v33.md`、`fc_ut_meta_pba_v34.md`、
`docs/design/debug_pio_dmb_20260927.md` 里的 `test /fc-nand/data_pathcrc_step_detect` 之类
是**当时真实可用**的写法（v34 那条还标注了上板已验）。按规则**不改历史正文**。

## 验证

```bash
make format-check                  # 过
make clean && make tests           # 0 warning 0 error；链接 219836 B
```

**宿主机复核路径构造与过滤匹配**（无板可跑）：复刻 `munit_maybe_concat`（纯拼接）与
`munit.c:1294-1306` 的匹配式，对三个子套件各造一条用例，逐个过滤串判定，结果：

```
printed: /fc-nand/logic/push_pop:
   filter /fc-nand/                            -> -
   filter /fc-nand/logic/                      -> MATCH
   filter /fc-nand/logic/push_pop              -> MATCH
   filter /fc-nand/logic                       -> -
printed: /fc-nand/lifecycle/block_multi_plane:
   filter /fc-nand/lifecycle/                  -> MATCH
   filter /fc-nand/lifecycle/block_multi_plane -> MATCH
   filter /fc-nand/lifecycle                   -> -
```

### 上板（**真正的验收**，由用户跑）

```
test --list                        # 全部 51 条路径应带分隔斜杠，如 /fc-nand/lifecycle/block_multi_plane
test /fc-nand/lifecycle/           # 应跑 9 条（改前这个串一条都跑不了）
test                               # 全套 51 条（不加过滤串）
```

验收点：`--list` 里**每条路径都有分隔斜杠**；`/fc-nand/lifecycle/` 能跑出 9 条而不是 0 条。

## 不做

- 不动 `src/**`（本版是测试注册 + 文档）；`src/ftl/**` 一行不动；
- 不动 `tests/framework/munit.*`（`/fc-nand/` 的匹配缺陷只记录不修，见 §四(c)）；
- 不动 `test_fc_level1/2/3.c` 的用例名（本仓约定就是**用例名不带前导斜杠**，改它要动全部用例）；
- 不重写既有 changelog 正文；不翻 `CONFIG_FC_RAID`。

## 出处

- 拼接点：`tests/framework/munit.c:314`（`munit_maybe_concat`）、`:1285`（子套件）、`:1103` / `:1128`（用例名与打印）。
- 过滤匹配：`tests/framework/munit.c:1294-1306`；自述语义在 `:1275`。
- 套件树：`tests/unit/ut_main.c`；本文档同步 `docs/design/fc_nand_ut.md` §3 / §4.1 / §4.2 / §7。
- 框架适配记录：`docs/design/munit_adapter.md` v1.5。
- 同批未提交的 v40（Level 3 地址随机化）：`docs/changelog/fc_ut_level3_seed_random_v40.md`。

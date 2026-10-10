# v43：Level 2 地址随机化 —— 与 v40 同法（用例体内 salted PRNG）

- **日期**：2026-10-11
- **范围**：**测试 + 文档**——`tests/unit/test_fc_level2.c` + 文档。**`src/**` 一行不动**
  （含 `src/ftl/**`），`tests/framework/munit.*`、`ut_main.c`、`tests/integration/**` 一律不动。
- **介质格式**：无变更。
- **测试**：**用例数不变（仍 51）**——本版不加用例，只改输入来源。
  `make format-check` 过；`make clean && make tests` **0 warning 0 error**、链接 **220132 B**。
- ✅ **上板已验**（2026-10-11，stm32h743 + MT29F4G08）：`test --log-visible debug /fc-nand/data_path/`
  **31 of 31 (100%)**，11 条编译进去的用例地址**逐条与宿主机预测吻合**，`--seed 0x5a4f21e4` 复现同组（见 §四）。

## 〇、起因与一处**自我订正**

v42 的「顺带观察」里我写了「Level 2 `scramble_whiten` 仍写死 `block=1328` / `2042`」——**这是错的**，
已在 v42 正文就地撤回。`test_scramble_whiten`（`test_fc_level2.c:502-508`）**早就**在用
`munit_rand_int_range` 抽 block / plane / page / cw；上板日志里的 `1328` / `2042` / `1018`
是三条探针**各抽一次**的**三个不同值**（`comtool3.0.log:417` / `:432` / `:449`），不是写死的常量。

真正仍然写死的地址是 **`TEST_BLOCK`(100) / `TEST_PAGE`(32) / plane 0**——经 `L2_META` / `L2_PAA`
两个宏与 `l2_cmd` 的建链地址，服务 Level 2 的 **13 条**用例。本版把它们改成按种子抽样。

## 一、值不值得做（如实记录）

- Level 2 **不碰介质**（文件头 `:9-15` 自述：端到端往返由 Level 3 的页 helper 覆盖）。地址只喂两样：
  **加扰种子**（`fc_random_seed_from_meta`）与 **USER 区的 PAA 图案**（`test_fill_user_paa`）。
- 所以收益是**输入多样性**（13 条用例拿到 13 组不同的种子 / 图案），**不是** v35 那条
  「地址错值编译期无声、必须上板」的风险——那条**不适用**于本层。
- 但有一个实打实的理由：这批用例多数是 `MUNIT_TEST_OPTION_SINGLE_ITERATION`
  （`munit.c:765-767` 把迭代数硬压成 1）⇒ **`--iterations N` 对它们完全无效**，
  用例体内抽样是**唯一**的加量杠杆。

## 二、做法（对齐 v40 的 `l3_rand`）

### 新增：`l2_hash` / `l2_rand` / `l2_addr_draw`

- `l2_hash` 是 DJB2，与 `munit.c:386` 的 `munit_str_hash` **逐字同实现**（那份是 `static`，只能照抄）。
- `l2_rand(test, tag) = munit_rand_uint32() ^ l2_hash(test) ^ l2_hash(tag)`，**再过一道终混**（见 §三）。
- `l2_addr_draw(test, plane)`：block / page 抽样，plane 由调用者钉死，**cw 恒 0**
  （`test_cmd_build` 断言 `cw == 0`，`fc_test_common.c:308`）。
  就地 emit **一条** `test_trace_detail("addr block=… page=… plane=…")`——一处覆盖全部用例。

**为什么必须加 salt**：munit 每次 `exec` 前把 PRNG 重置回 `runner->seed`（`munit.c:774`），
导出的 `munit_rand_*` **不带盐** ⇒ 用例体内的第一次抽样在所有用例之间取值完全相同。
复现契约 = **(seed, 用例名, 第几次抽样)**。

**不设窗口**：Level 3 的 30%~70% 是为了避开固定元数据区；本层**不写介质**，那理由不成立，
故用整段 `[0, BLOCKS_PER_PLANE)` / `[0, PAGES_PER_BLOCK)`，与 `test_scramble_whiten` 同口径。

### 删 / 改

| 对象 | 改动 |
|---|---|
| `L2_META` / `L2_PAA` 两个宏 | **删**（它们的唯一作用就是那个固定地址） |
| `l2_cmd` | 加 `const nand_addr_t *addr` 首参；`meta` 形参删掉，内部用 `pba_from_addr(addr)`（原调用者传的就是同一地址的 PBA ⇒ 等价替换）。命令的 **pba 与 meta 同源**这条性质不变 |
| `l2_buffer_round_trip` / `l2_ecc_cw_build` / `do_raid_recover_test` | 各加 `const nand_addr_t *addr` 首参 |

**选显式传递而不是一个可变全局**（如 `g_l2_addr` + `l2_addr_begin()`）：忘了传是**编译错误**，
忘了 `begin()` 是**静默用错地址**——本项目最恨后者。

### 13 条用例体各抽一次

`crc_step_detect` / `ecc_step` / `encode` / `encode_crc_disabled` / `process_read_ok` /
`process_read_unc` / `process_read_masked_chain` / `process_read_mata_error` /
`process_read_crc_error` / `process_read_ldpc_raw` / `raid_recover_no_random` /
`generate_stripe_parity` / `cw_bitmap`。

⚠ 参数化的用例（`crc_step_detect` / `ecc_step` / `encode` / `process_read_ok` / `cw_bitmap`）
**各 case 会拿到同一个地址**——盐只有用例名，而 PRNG 每次 `exec` 重置。**这是刻意的**：
case 之间变的本来就不是地址，同地址反而把变量隔离干净。

### 三处特殊

- **`test_process_read_mata_error`**：篡改目标由 `TEST_BLOCK + 1U` 改成
  `(addr.block + 1U) % BLOCKS_PER_PLANE`。必须与命令地址**同 page / 同 plane、异 block**——
  PBA 里 block 是**最高**字段（`PBA_BLOCK_SHIFT = 9`），改它不动低 3 位，正是本条用例要的
  「低 3 位同、高位异」（v34 踩过的坑：动低 3 位会被 `lba_to_meta()` 整段擦除，MATA_ERROR 恒不置位）。
- **`test_cw_bitmap`**：plane 必须 = `c->addr_plane`（那是场景表的判据，**不能**随机）
  ⇒ `l2_addr_draw("cw_bitmap", c->addr_plane)`，只随机 block / page。
- **`test_generate_stripe_parity`**：data 与 parity 两条命令**必须同一地址**（用例体把 data 的
  `desc->meta` 同步给 parity）⇒ 抽一次，两处都传同一个 `&addr`。

### 不动

- `test_scramble_whiten`——**已经**是随机的（本版的起因就是它被误判为写死）。

## 三、⚠ 抽样必须过一道**终混**（本版真正的技术发现）

第一版实现照抄 v40 的 `l3_rand`，**只做 XOR、不做终混**。宿主机复刻一跑就发现：

> `process_read_mata_error` 与 `process_read_ldpc_raw` 在**四条种子下逐位抽到同一个地址**
> （block 与 page 都相同）。

**根因**：DJB2 是 `h = h*33 + c`，而 `33 ≡ 1 (mod 32)` ⇒ **h 的低 5 位只等于各字符之和 mod 32**。
抽样又要 `% 2048` / `% 64`，等于把哈希**截到低位**用。两个哈希只要**低位相同**就必然撞。实测：

```
h("process_read_mata_error") = 0xE9E586CA
h("process_read_ldpc_raw")   = 0x0A6286CA
XOR                          = 0xE3870000   ← 低 16 位全 0
```

**修法**：`l2_rand` 末尾加一道 murmur3 `fmix32` 终混
（`h ^= h>>16; h *= 0x85EBCA6B; h ^= h>>13; h *= 0xC2B2AE35; h ^= h>>16`），
让结果的每一位都受全 32 位影响，撞的概率回到 1/2^11 量级。

⚠ **v40 的 `l3_rand` 有同样的结构**，但**实测 Level 3 六条用例在四个种子下无碰撞**
（宿主机复刻，含 `--seed 0x5a4f21e4` 与上板日志逐字一致），故**本轮不动 Level 3**——
动它会改变 v40 已验证的 seed→地址映射，代价大于收益。**只记录**：若将来 Level 3 的用例名或
窗口变了，应先跑一次碰撞检查。

## 四、验证

```bash
make format-check                  # 过
make clean && make tests           # 0 warning 0 error；链接 220132 B；用例数仍 51
```

**宿主机复刻**（无板可跑，抄 munit 的 PRNG + 本文件的 `l2_hash` / `l2_rand` / `l2_addr_draw`）：
对 4 个种子逐个跑 13 条用例，断言 ① 各自的地址**互不相同**、② block ∈ [0,2047]、
③ page ∈ [0,63]、④ `mata_error` 的篡改与声明地址**低 3 位相同、整体不同**。结果**全部通过**。

seed `0x5A4F21E4` 的一组（**上板实测与此逐条一致**，见下方「上板」）：

| 用例 | block | page |
|---|---|---|
| `crc_step_detect` | 282 | 41 |
| `ecc_step` | 1945 | 2 |
| `encode` | 170 | 30 |
| `encode_crc_disabled` | 1098 | 62 |
| `process_read_ok` | 949 | 38 |
| `process_read_unc` | 283 | 0 |
| `process_read_masked_chain` | 800 | 60 |
| `process_read_mata_error` | 951 | 3 |
| `process_read_crc_error` | 1672 | 40 |
| `process_read_ldpc_raw` | 1710 | 49 |
| `raid_recover_no_random` | 924 | 60 |
| `generate_stripe_parity` | 448 | 62 |
| `cw_bitmap` | 528 | 60 |

（plane 一律 0，除 `cw_bitmap` 的 `plane1-only` 一行为 1；RAID 两条默认不编译。）

### 上板（**真正的验收**，用户 2026-10-11 已跑）

```
test --log-visible debug /fc-nand/data_path/              # 每条一条 [EV] addr block=… page=… plane=…
test --log-visible debug /fc-nand/data_path/ --seed 0x5a4f21e4   # 同 seed 复现同一组地址
```

**结果：31 of 31 (100%)**（`/fc-nand/data_path/` = Level 2 展开后的 31 条），两轮地址**逐字相同**。
11 条编译进去的用例与宿主机预测**逐条吻合**：

| 用例 | 实测 block/page（plane） |
|---|---|
| `crc_step_detect` | 282 / 41 (0) |
| `ecc_step` | 1945 / 2 (0) |
| `encode` | 170 / 30 (0) |
| `encode_crc_disabled` | 1098 / 62 (0) |
| `process_read_ok` | 949 / 38 (0) |
| `process_read_unc` | 283 / 0 (0) |
| `process_read_masked_chain` | 800 / 60 (0) |
| `process_read_mata_error` | 951 / 3 (0) |
| `process_read_crc_error` | 1672 / 40 (0) |
| `process_read_ldpc_raw` | 1710 / 49 (0) |
| `cw_bitmap` | 528 / 60（`plane1-only` 那行 plane=1，其余 0） |

验收点全中：block ∈ `[0,2047]`、page ∈ `[0,63]`；`process_read_mata_error` 打
`声明 PBA=0x00077018 vs 页内 META=0x00076E18（低 3 位同、高位异）`——block 951→952，低 3 位不变 ✅；
`cw_bitmap` 三 case 的 plane 与场景表一致（`flat-mixed` 0 / `plane1-only` 1 / `span-multi-plane` 0）✅。

⚠ 附带观察（既有行为，非本版引入）：`--seed=0x…`（带 `=`）报 `unknown argument`，须写 `--seed 0x…`（空格）——
与 usage 串 `test [--seed N] …` 一致。

## 五、遗留（未处理）

- `tests/unit/fc_test_common.h:157` 的 `#define TEST_PAGE (32U)` 现在**零读者**
  （`TEST_BLOCK` / `TEST_BLOCK_DEST` 还被子套件 Level 3 的 `#if 0` 段经 `BLOCK_TEST` / `BLOCK_DEST` 引用）。
  本版**不动**共享夹具头文件，只记录。

## 不做

- 不动 `src/**`（含 `src/ftl/**`）、`tests/framework/munit.*`、`ut_main.c`、`tests/integration/**`；
- 不动 Level 3 的 `l3_rand`（见 §三 末）；
- 不翻 `CONFIG_FC_RAID`（RAID 两条仍在 `#if` 内，**同步改**以免将来翻开关时编译不过）；
- 不重写 v42 的结论性内容，只撤回那一处**事实错误**的观察。

## 出处

- 同法的上一版（Level 3）：`docs/changelog/fc_ut_level3_seed_random_v40.md`。
- 被撤回的观察：`docs/changelog/fc_ut_board_verify_v42.md` §三。
- 种子来源与 PBA 位序：`src/fc/fc_random_engine.c`、`include/nand/nand_config.h`
  （`PBA_BLOCK_SHIFT = 9` / `PLANE_CW_MASK = 0x7`）。
- munit 的 reseed / salt / iterations 语义：`tests/framework/munit.c:774` / `:386` / `:765-767`。

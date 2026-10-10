# v39：multi-plane 窄位图当场拒绝 + Level 2 审查（注释 + 文档同步）

- **日期**：2026-10-10
- **范围**：**生产代码 1 处守卫**（`src/fc/fc.c`）+ **测试注释**（`tests/unit/test_fc_level2.c`）
  + **文档**（`fc_nand_design.md` / `fc_nand_ut.md` / `assertion_guidelines.md`）
- **介质格式**：无变更。
- **测试**：**不改断言、不改用例数（77）**。`make format-check` 过；
  `make clean && make tests` **0 warning 0 error**、链接 **219484 B**；
  `make clean BUILD_TYPE=release && make BUILD_TYPE=release` 0 error、**100884 B**（2 处**既有**
  告警，均在不属于本次改动的文件里——见下）。
- ⚠ **需上板**：本轮改了 `fc_build_dispatch_request` 的**签名**与两条派发路径，回归必须上板跑。

## 一、multi-plane 窄位图：从「静默写坏」改成「当场拒绝」

### 缺陷

`fc_build_request_data_planes`（`src/fc/fc.c`）为命令 plane 段里的**每个** plane 填 `plane_paa[]`。
对**链上无 desc** 的 plane，旧实现凭空造一条回退地址：

```c
pba_to_paa((command->pba & ~PLANE_CW_MASK) | ((uint32_t)plane << PBA_PLANE_SHIFT))
```

而 NAND 侧的多 plane handler（`nand_dispatch_tables.c` 的 `req_multi_plane_{program,read,erase}`）
**无条件遍历 `PLANE_NUM`** 下行地址——**既不看掩码也不看 `plane_count`**。器件侧多 plane program
会把点名过的 plane **全部编程**。于是：一个 `multi_plane = 2` + 掩码只盖 plane 0（如 `0x0F`）的命令，
会**静默地**给 plane 1 编一页**全 0xFF**（页缓冲 `memset 0xFF`、只填掩码内的 CW），
写到一个**调用者从未指定**的地址，**调用者完全不知道**。

这不是理论问题，是**层次分工的漏洞**：`ftl_cmd_resolve_cw_bitmap` 只查**越段**
（`bitmap & ~allowed == 0`），`0x0F` 对整段 `0xFF` 而言是「合法」的。唯一守着这条不变式的是
**测试夹具**（`fc_test_common.c` 的 `test_cmd_build`）。

### 为什么生产没炸（但仍然是错）

FTL 唯一的多 plane 入口 `ftl_cmd_setup_ppa_pair()`（`ftl.c:638` / `:747`）自己传
`fc_command_plane_span_cw_bitmap(cmd)` 的**整段**位图；`ftl_cmd_configure_descriptors` 只被
单 plane 的 `ftl_cmd_setup_ppa` 调用。所以**生产路径不可达**——但踩进来的门槛很低
（直接 `ftl_cmd_configure_descriptors(cmd, 部分位图)` 配 `multi_plane > 0`），
而代价是**静默的介质破坏**。

### 修法：双档拦截（新判据）

| 档 | 手段 | 行为 |
|---|---|---|
| debug（`make tests`） | `ASSERT((planes_with_desc & all_planes) == all_planes)` | 停机，现场最可定位（§十三 / §24 判据 2） |
| release（`NDEBUG`） | `if (…) return FC_STATUS_INVALID_ARGUMENT;` | **拒绝命令，绝不提交** |

`fc_build_dispatch_request` 由 `void` 改 `fc_status_t`，透传该状态；两条派发路径
（`fc_poll_dispatch_single` / `fc_poll_dispatch_chain`）收到非 OK 就 `LOGW` + `fc_poll_fail_command`
注一条 `NAND_FAIL` 完成项 —— **FTL 的 pending 照常释放**（否则命令永不完成，表现为「挂死」，
比报错难查）。链式路径一次给**整条链**注失败，不留半截链，与它既有的 submit 失败兜底口径一致。

于是 `fc_build_dispatch_request` 从「构建不可失败」变成有**唯一**失败出口，函数头注释同步更正。
`fc_build_request_data_planes` 里那句「else 支回退到 `command->pba` 同页地址」保留为
**NDEBUG 档的最后兜底**（生产不可达）。

### 新判据（已写入 `assertion_guidelines.md` §二十四）

> **判据 9**：当配置错误的后果是**静默的数据 / 介质破坏**（不崩、不报错、上层无法检测）时，
> 除 debug 档的 `ASSERT` 外，**必须**紧跟一道 release 档运行期拒绝。其余配置错误仍只写 `ASSERT`。

`ASSERT` 与 `return` **不是二选一**：debug 档在 `ASSERT` 处就停机，`return` 走不到；release 档
`ASSERT` 被整条裁空，只剩 `return`。两条都要写。

> **判据 10**：被 `ASSERT` 守的变量在 `NDEBUG` 下可能变成 unused —— **release 档的编译告警
> 是「这条断言在 release 下没有等价守卫」的唯一提示**。本仓已知一处：FTL 侧
> `ftl_cmd_resolve_cw_bitmap` 的越段 `ASSERT`（§十八）在 release 档把 `allowed` 裁成 unused
> （`src/ftl/ftl_fc_interface.c:32`）⇒ **FTL 侧 release 档同样无守卫**。
> 按硬规则本次**不动 `src/ftl/**`**，只记录。

### 想只访问某个 plane 怎么办

用**单 plane op**（`multi_plane = 0`）——它的 plane 段就是 `pba_to_plane(cmd->pba)` 那一个，
窄位图**合法**。多 plane op 配窄位图从来不是「访问一个 plane 的捷径」，而是**编程错误**。

## 二、Level 2 用例审查：12 个函数补注释

`tests/unit/test_fc_level2.c` 给 12 个此前无注释的函数补了「功能 / 测什么 / 为什么这么判」，
**纯注释、零逻辑改动**：

| 函数 | 补的重点 |
|---|---|
| `l2_verify_parity_nonzero` | 判据取「非全 0」而非期望值：parity 是生成多项式余式，期望值得另写一份编码器；「整段全 0」才是「根本没编码」的指纹 |
| `l2_crc_case_find` / `l2_ecc_case_find` / `l2_enc_case_find` / `l2_rt_case_find` / `l2_bm_case_find` | 五张场景表各配一份同形 helper：munit 只传字符串，行索引靠名字查回来，避免「名字数组」与「场景表」两处错位 |
| `test_crc_step_detect` | 四段（哨兵 / 只开 CRC / **先自证干净** / 注入后按期望判） |
| `test_ecc_step` | 四段 + 三个 scratch 缓冲的分工；⚠ 各 `case` **从同一段随机流起抽** ⇒ 注入位置**逐 case 嵌套**（`data-4 ⊇ data-3 ⊇ data-2`），是「每次多一个错」的叠加，不是 N 组独立抽样 |
| `test_encode` | 判据两层：`expect_plain` 是**自证**（fixture 退化时往返恒等式会照样成立、把问题盖过去） |
| `test_process_read_ok` | 为什么用 `l2_verify_all_ready` 而非夹具的 `test_verify_cw_status_all_success`（后者只看 SUCCESS 位，`CRC_ERROR` / `MATA_ERROR` 会与它共存而不被发现） |
| `do_raid_recover_test` | 5 步；4 个 CW 填**不同** pattern（相同则「XOR 还原」与「根本没动」分不开）；整 528B 翻 0xFF 保证走 XOR 重建而非 ECC 顺手纠回 |
| `test_raid_recover_no_random` | 为何是 `random=OFF` + 中间那条 CW |
| `test_cw_bitmap` | 钉三件事（flat 绝对编号 / plane-major 链序 / `config.cw` 不回写）+ 为何位图必须显式给 |

另更新 `test_process_read_masked_chain` 的 ⚠ 注释：那条约束**此前只在夹具里**，v39 起生产侧
（`fc_build_request_data_planes`）也守同一不变式，「不再是裸奔」。

## 三、文档同步

| 文档 | 变更 |
|---|---|
| `fc_nand_design.md` §2.5 | 新增 **「FC 返回码 (`fc_status_t`)」** 小节：三层分类表（成功 / 调用者写错 / 运行结果）、本仓实际返回 `IO_ERROR` 的**四类情形**（A 介质 ECC / B NAND 驱动 / C FC 基础设施 / D 上一层 FTL 的另一套枚举）、两个易混点（FC 的 `IO_ERROR` ≠ 主机的；数据路径返回值在生产里**被丢弃**，判据只看 `desc->status`） |
| `fc_nand_design.md` §24 | 补「判据 2 的补丁」段，指向 `assertion_guidelines.md` §二十四 判据 9 |
| `fc_nand_design.md` §25.4 | 「空 plane 的行地址」改写 + **v39 块引**：旧文写「flat 位图下某 plane 可以是 0 个 CW（如只读 plane 1 = `0xF0`）」——**这个组合现在是调用者的编程错误** |
| `fc_nand_ut.md` §5.7（v1.20） | `ecc_step` 的**六处覆盖缺口**与**四步提高计划**（详见该节；**只记录不改测试**） |
| `fc_nand_ut.md` §2.2 | 加指针：`ecc_step` 全绿只说明「没复现出实现 bug」 |
| `fc_nand_ut.md` §7 | 新增 v1.20 行；**修**一处被 v1.20 编辑吃掉表头的 **v1.18 行**（`feature_write_read` 回环那条，此前与 v1.20 挤在同一行） |
| `assertion_guidelines.md` | 新增 **§二十四**（判据 9 / 判据 10） |

## 验证

```bash
make format-check                                        # 过
make clean && make tests                                 # 0 warning 0 error，链接 219484 B
make clean BUILD_TYPE=release && make BUILD_TYPE=release # 0 error，100884 B
```

release 档 2 处告警均为**既有**、且不在本次改动的文件里：
`src/ftl/ftl_fc_interface.c:32`（`unused variable 'allowed'` —— 正是判据 10 记的那处，`src/ftl/**` 不动）
与 `src/platform/stm32h743/crc.c:38`。

两个 ELF 里都已确认含新增日志串（`strings … | grep "build request failed"`）。

### ⚠ 未覆盖的部分（如实记录）

新守卫**没有单测覆盖**：夹具 `test_cmd_build` 在上游就把非法输入挡掉了，所以
debug 档的 `ASSERT` 与 release 档的拒绝**都是靠代码走查确认的**，不是跑出来的。
要让它们有覆盖，得让测试**绕过夹具**直接调 `fc_build_dispatch_request`——目前没做。

## 出处

- 缺陷的原始表述（本仓自陈）：`fc_nand_design.md` §25.4 旧文「空 plane 的行地址（flat 独有）」。
- 位图语义翻转（`cw_bitmap` 0 = 空集）与漏扫调用点的历史：`docs/changelog/fc_ut_level3_bitmap_v35.md`。
- FTL 侧越段 `ASSERT`：`assertion_guidelines.md` §十八。
- FC 层四条断言判据：`fc_nand_design.md` §24。
- 前一轮文档（ECC / CRC 保护什么）：`docs/changelog/fc_ecc_crc_coverage_v38.md`。

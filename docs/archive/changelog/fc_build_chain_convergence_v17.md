# v17：FC 建链收敛——三层接力 → 一条公共入口

**日期**: 2026-10-08
**相关设计**: `docs/design/fc_nand_design.md` §2.9（接口表）、§2.10（生命周期）、§25.4（版本对照）
**相关文档**: `docs/design/fc_api_usage.md`、`docs/design/assertion_guidelines.md`
**前置**: `fc_chain_ownership_v16.md`（v16 出池即挂链）、`fc_policy_layer_uplift.md`（v13 策略层上移 FTL）

---

## 概述

| # | 项 | 改动前 | 改动后 |
|---|---|---|---|
| 1 | **建链入口层数** | **3 条接力**：`fc_command_attach_cw`（公共壳：身份/状态校验）→ `fc_command_attach_cw_internal`（plane 段/CW 范围校验）→ `fc_command_configure_descriptor_internal`（机制体：出池 desc/buffer + 填字段 + 挂链） | **1 条**：`fc_command_configure_descriptor`（校验段 + 机制段一体），后两条**删除** |
| 2 | **机制体归属** | `fc.c`（static） | `fc_interface.c`（公共入口本体）；`fc.c` 只留池/链原语 |
| 3 | **命名** | `fc_command_attach_cw` | `fc_command_configure_descriptor`（**单数**） |

---

## 1. 为什么合并（证据，不是审美）

- **中间层与机制体签名逐字相同**：`(fc_t*, fc_command_t*, uint8_t plane, uint8_t cw, nand_paa_t row_paa)`。
- **`fc_command_configure_descriptor_internal` 只有唯一调用者**（`fc_command_attach_cw_internal`），
  函数体内只有「分配 desc → 填字段 → 挂链 → 分配 buffer」一条直线。
- `fc_internal.h` 原注释声称 `_internal` 版是「供公共入口与 RAID parity 建链**共用**的机制层原语」——
  但 RAID 调的就是 `_internal` 本身（`fc.c` parity 建链），**从来没有任何调用者**需要
  「跳过 plane 段校验、只分配 desc」那个更底层的东西。属**投机性分层**，与 v13~v16 的收敛方向相反。

## 2. 划分规则（v17 起）

> **fc.c = 池与链的原语；fc_interface.c = 编排。**

| 函数 | 去向 | 理由 |
|---|---|---|
| `fc_command_configure_descriptor_internal` | 函数体**搬进 fc_interface.c**，成为公共入口本体 | 它是编排，不是池原语 |
| `fc_command_attach_cw_internal` | **删除** | 校验逻辑并入公共入口 |
| `fc_command_cw_attached` | **搬进 fc_interface.c**（唯一调用者是重复 CW 断言） | 随调用者走；保留 `__attribute__((unused))` |
| `fc_descriptor_alloc_internal` | **留在 fc.c**，去 `static` + 进 `fc_internal.h` | 直接读写 desc 池的 static 状态，与 `fc_descriptor_release_internal` 成对 |
| `fc_command_chain_append` | **留在 fc.c**，去 `static` + 进 `fc_internal.h` | 直接操作 `command->desc_id` + `descriptor_pool[].next_id`，属链原语 |
| 释放链三个原语 | 原地不动 | — |

搬迁后的建链函数**全程不直接读写池内部字段**，只调用 fc.c 的三个原语
（`fc_descriptor_alloc_internal` / `fc_command_chain_append` / `fc_buffer_alloc_internal`）。

## 3. 校验顺序（本次最要紧的不变量）

合并后**校验段必须在机制段之前**——任何非法实参都在出池之前返回，不污染池子：

1. `ASSERT(command != NULL)`
2. 指针身份（`fc_command_to_id`）+ 生命周期（须 `ALLOCATED`）→ `INVALID_ARGUMENT`
3. `fc_command_plane_span` + `cw >= CW_NUM` + plane 越段 → `INVALID_ARGUMENT`
4. `ASSERT(fc_command_cw_attached(...) == 0U)`（重复 CW = 链构造错误）
5. `ASSERT(paa_to_plane(row_paa) == plane)`（调用者契约）

若写成「先分配再校验」，非法坐标会先占掉一个 desc（甚至一个 buffer）再失败，
破坏 v16 建立的「失败即沿链完整回收」的干净语义。

返回码语义不变：`INVALID_ARGUMENT`（校验失败，池子未动）/ `RESOURCE_EMPTY`
（desc 或 buffer 池空；desc 已在链上、`buffer_id` 保持 INVALID，不回滚）。
原机制体里的 `ASSERT(cw < CW_NUM)` 删除（已被步骤 3 的运行时判断覆盖）。

## 4. 改名波及面

新名 **`fc_command_configure_descriptor`（单数）**。

⚠ **绝不能**定成复数 `fc_command_configure_descriptors` —— 那是 `tests/` 里失效的旧 FC 符号
（v13 已删），同名会意外「修好」测试引用；且它只收 1 个实参，仍编不过。
**本次不碰 `tests/`**，`make -k tests` 的既有错误数必须不增不减。

调用点（2 处）：
- `src/ftl/ftl_fc_interface.c`（`ftl_cmd_configure_descriptors` / `ftl_cmd_setup_ppa_pair` 的循环内）
- `src/fc/fc.c`（Stripe RAID parity 建链）

## 5. 改动文件

| 文件 | 改动 |
|---|---|
| `src/fc/fc.c` | 删除机制体 + `_internal` + `cw_attached` + 前向声明（-166 行）；`descriptor_alloc` / `chain_append` 去 `static`；文件头结构图第 5 项改为「desc 链原语」；RAID 调用点与 4 处注释改名 |
| `src/fc/fc_interface.c` | 文件头定位改写（不再自称「薄壳」）；新增 `fc_command_cw_attached`（static）+ 合并后的 `fc_command_configure_descriptor` |
| `include/fc/fc_internal.h` | 删 `fc_command_attach_cw_internal` 声明；新增两个池/链原语声明（含「必须立刻挂链」的调用者契约） |
| `include/fc/fc_interface.h`、`include/fc/fc.h` | 声明改名 + 文件分工/生命周期/接口分组文案 |
| `include/fc/fc_command.h`、`include/ftl/ftl_fc_interface.h` | 注释里的函数名同步 |
| `src/nand/nand_dispatch.c`、`src/fc/fc_data_path.c` | 注释引用点改名 |

## 6. 验证

| 项 | 结果 |
|---|---|
| `make debug` | 0 error / **0 warning**，binary 148640 B |
| `make release` | 0 error / **0 warning**，binary 85376 B |
| `make debug CFLAGS_EXTRA=-DCONFIG_FC_RAID=1` | 0 error / **0 warning**，binary 152272 B（RAID 分支编译验证，见 §7） |
| `make format-check` | passed（101 文件） |
| `make -k tests` | **29 error / 7 warning，与改动前基线逐项一致**（全部在 `tests/`，v15 删 `command_id` 等既有问题） |
| 上板 | **未做**（由用户按 `make tests` 上板执行） |

## 7. 已知 trade-off / 未验证项

1. **「desc 出池即挂链」不变量跨文件了**。原由 fc.c 的 `configure_descriptor_internal` 保证，
   现在由 `fc_interface.c` 的建链入口保证，而 `fc_descriptor_alloc_internal` 仍留在 fc.c。
   已在 `fc_internal.h` 的声明处写明契约（分配后必须立刻 `fc_command_chain_append`），
   防止将来 fc.c 内直接调用它而产生游离 desc。
2. **RAID 路径硬绑单例**。RAID parity 建链改调公共入口，而公共入口固定使用 `g_fc_controller`。
   全仓当前只有这一个 `fc_t` 实例（无第二个实例、无测试桩），故可行；但若将来要做多实例，
   这条路径要改回 `controller` 参数化入口。已在 `fc.c` 的 parity 建链注释里点明。
3. **`make asan` 目标本身是坏的（与本次改动无关）**：`config.mk` 的 `BUILD_TYPE` 分支只有
   `debug` / `release`，`asan` 会直接 `$(error)`；`SAN_FLAGS` 变量被引用但从未定义。
   `config.mk` / `makefile` 本次未改动，属既有状态。
4. **RAID 只是「能编译」**：`CONFIG_FC_RAID` 默认 0，RAID 功能本身未启用、未上板验证。
5. **`fc_descriptor_chain_release_all` 的措辞澄清（无行为变化）**：该函数名里的 `all` 指
   **本命令的 desc 链**（= 该命令已申请的全部 desc + buffer），**不是**清空 desc/buffer 池、
   也不触及别的命令——链本身就是这条命令的资源集合（v15 起唯一真值）。实现本来就如此
   （从链头沿 `next_id` 走），本轮只在 `fc_interface.h` / `fc.c` 的注释与
   `fc_api_usage.md` / `fc_nand_design.md` 的措辞里把这一点写死，避免被读成「清池」。

---

## 8. 附带：走链终止条件统一为哨兵（去掉计数上界）

**问题**：`fc_descriptor_chain_release_all_internal` 与 `fc_command_descriptor_count` 的走链循环
写的是 `while ((id < FC_DESCRIPTOR_POOL_DEPTH) && (count < CW_TOTAL_MAX))`——第一个条件是**链哨兵**
（`next_id != FC_DESCRIPTOR_ID_INVALID`，写成区间判断），第二个是**计数上界**。而 `fc.c` 里另外
四处走链循环（`fc_complete_from_nand` 回写、完成项折叠、填 META、`fc_build_request_data_planes`）
**只有哨兵**。即 6 处里 4 处纯哨兵，这 2 处是少数派。

**改动**：两处一起改成哨兵单判据：

```c
while (id < FC_DESCRIPTOR_POOL_DEPTH)   /* 链尾 next_id == INVALID 时自然结束 */
```

**为什么去掉计数上界**：
- **它是错的界**。`CW_TOTAL_MAX`（8）既是「合法链长」的不变量，又被拿来当「防成环」的界——
  两者混在一起，导致一条**合法**但超 8 的链会被**静默截断**（我上一条 review 里点出的「尾巴泄漏」
  就是这么来的）。真要防成环，界应取池深（256），但那也只是把「挂死」换成「静默腐坏」——
  成环时循环会重复释放同一条 desc（已清零、`next_id` 变 0），池子照样坏。
- **成环在当前代码路径下不可达**：`fc_command_chain_append` 只做「新尾 `next_id` 置 INVALID、
  旧尾指向新尾」的线性追加，永远不产生环。
- **与全仓主流写法一致**，不再有「同一条链、两种终止判据」的不一致。

**行为影响**：对任何合法链**逐字节中性**（链长 ≤ 8，哨兵先于计数触发；`fc_command_descriptor_count`
返回值不变，`fc_command_descriptors_owned_internal` 不受影响）。debug binary 148640 → 148616 B。

⚠ `fc_descriptor_chain_release_all_internal` 末尾的 `*head = FC_DESCRIPTOR_ID_INVALID;` **保留**——
它是显式后置条件（保证返回恒为 INVALID），不是循环判据。

> v16 changelog（`fc_chain_ownership_v16.md`）与 v15 changelog（`fc_command_identity_convergence.md`）
> 里描述的「循环上界 CW_TOTAL_MAX 防损坏链成环」是**当时的形态**，保留不改。

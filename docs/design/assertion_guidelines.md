# 全工程断言判据（ASSERT / ASSUME）

断言设施见 `include/common/assertion.h`。本文是**全工程**判据，各层（FC / NAND / FTL / controller / platform / common）共用同一套规则。FC 层最初在 `docs/design/fc_nand_design.md` §24 定稿，本轮推广到这里；§24 保留 FC 特有的例外说明并指向本文。

> **测试源口径（2026-10-24）**：本文引用的 `tests/unit/test_ftl.c` / `test_ftl_bg.c` /
> `ftl_test_common.*` 是**已删除的旧套件**。这些用例名与行号保留为当时的审计证据，不再指向
> 现存文件；现役 FTL UT 见 `docs/design/ftl_ut_design.md`。判据 4 中「被 UT 钉住」的结论，
> 在新套件里复核后才能继续沿用。

## 一、四条判据

1. **外部输入校验保留返回码**
   只有**来自外部**的输入才需要返回错误码：host 的 opcode/LBA、UART CLI 的参数、从介质读回的字节（见 §六辨析 C）。这些值的合法范围由外部世界决定，损坏/非法是**预期情形**，必须返回 `FC_STATUS_INVALID_ARGUMENT` / `FTL_ERROR` / `SSD_STATUS_INVALID_ARGUMENT` / `NAND_RESULT_INVALID_ARGUMENT` 等让上层处置。
   **参数是不是"层对外头文件的入口"不再是判据**——关键在**值从哪来**：调用者（哪怕是跨层的 FTL/controller）用自己铸出的句柄、自己算出的索引、自己持有的指针调进来的，一律算前置条件（判据 2），即使该函数声明在 `nand_dispatch.h` / `fc_interface.h` / `nand_mt29f4g08abadawp.h` 这类"边界头"里。
   （本判据原先把"边界头的全部入口"整体划为外部边界，属**过度概括**，见 §七辨析 D 与 §九。）

2. **调用者已保证的前置条件转断言**
   `static` 函数、层内模块头里的接口，以及**跨层入口中调用者必然传有效值的指针/索引参数**，前置条件由调用者保证。守卫一律写成 `ASSERT(条件)`，并**删掉原来的 `return` 与恢复/降级分支**（"跳过坏 ID 继续找"、"回滚分配再返回"、"防御性降级"这类）。
   层内模块头指：`include/nand/nand_engine.h`、`nand_fmc.h`；`include/ftl/*.h`（除 `ftl.h` 外，含 `allocator.h`/`gc.h`/`bbt.h`/`journal.h`/`metadata.h`/`l2p_cache.h`/`checkpoint.h`/`pending.h`/`read_recovery.h`/`wear_leveling.h`/`ftl_op.h`/`bst.h`）。
   **`static inline` 的纯 getter/setter 一并适用**（2026-10-17 C 批明确）：判据**不是**"它是不是模块入口"，
   而是**该函数的 NULL 语义有没有被文档化或被 UT 钉住** —— 有 ⇒ 是契约，保留（辨析 B / 判据 4）；
   没有 ⇒ 指针就是前置条件，`if (p != NULL)` 是**恒真的静默吞**（§十附判据 1）⇒ 转 `ASSERT`。
   ⚠ 转 `ASSERT` 是 **release 的净收益**：`if (p != NULL)` 在 `NDEBUG` 下仍是活分支，
   `ASSERT` 被裁成 `((void)0)` ⇒ 零开销。见 §二十八 五。
   **跨层入口一并适用**：`nand_dispatch_submit/submit_chain/poll`、`nand_fmc_init/backend_get`、`nand_fc_execute/start/step/read_id/set_features`、`fc_submit`、`fc_command_configure_descriptor`、`fc_command_release`、`fc_data_path_*`、`fc_scheduler_*`、`fc_command_status_*`、`fc_ldpc_*`、`fc_crc_*`、`fc_random_*`、`fc_raid_*`、`ftl_cmd_write/read_data`（v13 由 FC 上移）、`fc_get_cmd_data_ptr`、`fc_get_cmd_cw_bitmap`、`fc_get_resource_stats` 的**指针/索引参数**。唯一的调用者都是层内代码（FTL/controller/fc.c），且已持有有效值。

3. **资源类结果保留返回码**
   池耗尽（`FC_STATUS_RESOURCE_EMPTY`）、队列满背压（`QUEUE_FULL` / `IO_ERROR`）、`fc_command_alloc()` 返回 `NULL`、`mem_alloc` 返回 `NULL`、NAND 报告的错误——都是**预期内**的运行结果，不是不变量破坏，必须返回给上层。
   同理**运行时状态**保留返回码：`g_nand_dispatch_armed == 0U`（异步路径未放行）、`g_initialized == 0U`（惰性初始化未完成）、`ctx->active == 0U`、`fc_buffer_is_occupied(...) == 0U`（生命周期）、`map->uops == NULL`（操作表缺项）——这些是"此刻能不能做"，不是"调用者传错"。（原条目还列了 `g_crc_ready == 0U`——**v33 起该标志随 CRC 改走 HAL 驱动整体删除**，CRC 自检失败转 `ASSERT`，不再有「静默降级」这条运行结果，见 `docs/archive/changelog/fc_crc_hal_driver_v33.md`。）

4. **单测固化为契约的守卫保留返回码**
   这是判据 2 的例外，但**只在测试是被测函数的正当调用者时成立**。`tests/` 里拿**文档化合法输入**调内部函数验证行为（如 `allocator_remove_from_free(..., 越界)` 是文档化的 no-op、`md_block_is_metadata(NULL,…)` 是文档化分类——两者都是 `../archive/ftl_ut_munit.md §4.1/§4.6` 的用例）的，按判据 1 保留返回码。
   反之，**只有测试会走"传非法参数"这条分支**（生产调用者从不传）的，按判据 2 转断言并**删掉对应用例**——见 §四。

## 二、两条辨析（最容易搞错的地方）

**辨析 A：`ptr == NULL` 要看指针来源。**
来自 `fc_command_alloc()` / `mem_alloc` 的是**资源**（判据 3，保留）；来自函数入参的才是**前置条件**（判据 2，转断言）。FTL 里满屏的 `cmd == NULL` 绝大多数是前者。

**辨析 B：语义分支不是前置条件。**
用 `NULL` 表达"这个可选参数没给"的，是有意设计的语义，不是调用者的编程错误。例：`ds_deque.c` 的 `cp == NULL`（浅拷贝选择器）、`md_block_is_metadata(md_dir == NULL, …)`（固定区间照常分类）、`fc_ecc_decode(..., err_bits == NULL)`（不取纠正位数）。~~`fc_submission_queue_pop_n(ids == NULL)`（仅消耗队列）、`fc_command_status_init(status, command == NULL)`（无命令来源时填哨兵值）~~ **（2026-10-08：两函数因零生产调用者被整体删除，见 §十七）**。

## 三、`ASSUME` 的取舍

**全工程不用 `ASSUME`。** 理由（FC 层定稿时已论证）：
- 恢复分支既然已经手工删掉，`ASSUME` 的 `__builtin_unreachable()` 换不来额外代码生成收益——工程未开 LTO，跨 TU 调用点无从利用该假设；
- 它会把"违反即 UB"引入一条本来只会 HardFault 的路径——同样是崩，`ASSERT` 的现场更可定位。

`ASSUME` 宏保留在 `assertion.h` 里，留给将来确有热路径需要向编译器证明的场合。确需表达"逻辑不可达"时用 `ASSERT_UNREACHABLE()`。

## 四、什么时候可以删用例而不是保留守卫

判据 4 只适用于"这个函数在别的场景下确实需要优雅返回"的情况。反例是 FC 的 `fc_queue.c` 指针判空：生产调用者只有 `fc.c` 与 `fc_scheduler.c`，**全部传有效指针**——唯一会传 `NULL` 的就是测试自己。这种"只有测试会走的分支"应当按判据 2 转 `ASSERT`，并**删掉对应用例**，而不是为了让用例继续通过而保留返回码。

FC 层第一轮据此删除了 `test_fc_level1.c` 的 7 个 `*_null` 用例；第二轮"全部入口守卫"补扫又删除了 `test_fc_level1.c` 的 6 个（`status_init_null` / `status_set_cw_overflow` / `status_set_cw_null` / `init_null` / `select_null` / `submit_null` / `poll_completion_null` / `configure_descriptors_null`）与 `test_fc_level2.c` 的 3 个（`encode_null` / `process_read_null` / `raid_recover_null`）。

## 五、落地注意

- `make tests` 走 `BUILD_TYPE=debug`（`-DDEBUG`，无 `NDEBUG`），**断言在板上测试固件里是活的**：任何一条误判都会让板子停机而不是报失败。改动守卫后必须上板跑套件。
- `release`（`-DNDEBUG`）下 `ASSERT` 被完全裁掉。若某处实际可达，表现会从"返回错误码"变成 HardFault——所以转断言的前提是"调用者已保证"，必须逐处核对调用链。
- 每处改动在注释里保留原判据的语义（如 `/* 队列只存 fc_command_alloc() 产出的 ID */`），避免下一个人以为是误删。
- **故障路径禁止断言**：`src/platform/stm32h743/fault.c` 的故障打印、`assertion.c` 自身的终端路径、`syscalls.c` 的堆守卫、`src/common/crash.c` 的崩溃记录路径——这些必须能在任何状态下继续运行。`crash.c` 的 `crash_copy(source == NULL)` 因此**保留返回式降级**（写空串），不转断言。
- 本仓链接**完整 newlib**：`%llu`/`%hhu`/`%zu` 均可用；QWORD 直接用 `%llu`，DWORD 用 `%lu`。

## 六、辨析 C：校验"不可信持久化数据"不是前置条件

判据 2 只适用于**调用者编程错误**造成的违反。若某处的"异常值"来自**上电/掉电后从介质读回的字节**，那它属于判据 3——损坏数据是掉电的**预期情形**，必须返回错误码让上层走恢复流程，不能断言。

典型：`journal.c` 回放时 `count > JRN_ENTRIES_PER_PAGE`——回放读的是 NAND 上的 journal 页，页头可能因掉电撕裂而非法。这里返回错误、停止回放才是正确行为。同理，`journal.c` 里页/块翻转的控制流（`page++`、`block` 切换）也只是普通分支，不是守卫。

> ⚠️ **2026-10-17 收窄**：本节原结论写作「**`journal.c` 整个文件不转断言**」——那是把
> **文件**当成了豁免单位，而判据的适用面是**值从哪来**，不是**代码在哪个文件**。
> 同一次落地的 §二十六 已经给出正确口径：「边界 = host 入口返回码 + 子模块公开入口 ASSERT」。
> 按该口径重扫 `journal.c`，其中的**句柄类守卫**（`journal == NULL`、`cache == NULL`、
> `out_addr == NULL` …）收的是**调用者自己持有的指针**，属判据 2，**已全部转 `ASSERT`**
> （9 处，见 §二十八）；只有**下面这两类**留在本节豁免内：
>
> 1. **回放读回的字节**（`count > JRN_ENTRIES_PER_PAGE` 一类）——数据来自介质，
>    坏数据是掉电的**预期情形**（判据 3，H29）；
> 2. **页/块翻转的控制流**（`page++`、`block` 切换）——普通分支，不是守卫。
>
> **结论（收窄后）：`journal.c` 中「校验回放内容」与「环翻转控制流」不转断言；
> 其余入口守卫按判据 2 转 `ASSERT`。**

## 七、辨析 D：FC 自铸句柄的越界检查是判据 2，不是判据 1

判据 1 的"公共 API 参数校验"针对的是**外部输入**——host 的 opcode/LBA、CLI 参数、介质读回的字节。但 `fc_interface.h` 里有一类入口，收到的是 **FC 自己铸出来的句柄**：

- `fc_command_id_to_command(command_id)` 的 `command_id` 只来自 `fc_command_alloc()`（队列里只存分配产出的 ID）；
- `fc_descriptor_id_to_descriptor(descriptor_id)` 的 `descriptor_id` 只来自命令的 desc 链（`fc_command_configure_descriptor` 铸出）。

调用者**不可能**传入越界值——越界即不变量破坏。因此这类"句柄越界 → 返回 NULL/错误码"的守卫按判据 2 转 `ASSERT`，并同步修掉头文件里"越界返回 NULL"的契约描述。同理，FC 内部对句柄的越界检查（`fc_stripe_release_parity` 的 `parity_id`、完成路径的 `completed_id`、desc 链遍历里的 `desc_id`）一律转 `ASSERT`。

**保留返回码的部分（⚠️ 已于 2026-10-08 全部撤销，见 §十四）**：`fc_submit` / `fc_command_release` 这两个**顶层生命周期入口**里，`fc_command_to_id()` 返回 0（指针不在本池）、状态非 `ALLOCATED`、desc 链未配置这类检查曾返回 `FC_STATUS_INVALID_ARGUMENT`——校验的是"这个指针是不是本池的命令 + 生命周期阶段"（指针身份 + 状态）。**同一入口里的 `command == NULL` 是纯参数前置条件，转 `ASSERT`**（第二轮补扫，见 §九）。
> ⚠️ **2026-10-08 修订**：`fc_command_configure_descriptor`（原 `fc_command_attach_cw`）已**退出**本例外——它只保留 `RESOURCE_EMPTY`，身份/生命周期/坐标等**配置错误一律 `ASSERT`**。判据见 §十三。
> ⚠️ **2026-10-08 修订（二）**：`fc_submit` / `fc_command_release` 亦**全部退出**本例外（本轮 v19）——`fc_submit` 只留 `OK`/`QUEUE_FULL`，`fc_command_release` 恒 `OK`。§七 的"顶层生命周期入口保留返回码"例外**至此清零**。见 §十四。
（v13 前这三个入口是 `fc_command_configure_descriptors` / `fc_submit` / `fc_command_release`；前者已上移 FTL，FC 侧建链入口变成 `fc_command_attach_cw`，v17 再收敛为 `fc_command_configure_descriptor`。v17 时它沿用"保留身份/状态返回码"的契约；**该契约已于 2026-10-08 撤销**，见 §十三。）

> 判据 1 原先把"`fc_interface.h` 的全部入口"整体划为边界，属**过度概括**；按句柄来源细分：外部输入（host / CLI / 介质）→ 判据 1，FC 自铸句柄 → 判据 2。

## 八、落地记录（2026-10-05）—— ⚠️ 2026-10-17 复核后改为**状态表**

> 详细记录已移入 `docs/archive/assertion_rounds.md`。

## 九、第二轮：FC + NAND "全部入口守卫"补扫（2026-10-05）

> 详细记录已移入 `docs/archive/assertion_rounds.md`。

## 十、第三轮：per-LUN FIFO 重构后的同类补扫（2026-10-06）

> 详细记录已移入 `docs/archive/assertion_rounds.md`。

## 十一、v13：FC 策略层上移 FTL 后的接口变化（2026-10-07）

> 详细记录已移入 `docs/archive/assertion_rounds.md`。

## 十三、v18：建链入口的"配置错误 → ASSERT"定调（2026-10-08）

> 详细记录已移入 `docs/archive/assertion_rounds.md`。

## 十四、v19：FC 公开 API 壳的"配置错误 → ASSERT"（2026-10-08）

> 详细记录已移入 `docs/archive/assertion_rounds.md`。

## 十五、v20：FC 内部 / 数据路径 / 引擎的"配置错误 → ASSERT"（B 档，2026-10-08）

> 详细记录已移入 `docs/archive/assertion_rounds.md`。

## 十六、v21：FC 层 C 档·形态三"缺失的入参/边界校验"（2026-10-08）

> 详细记录已移入 `docs/archive/assertion_rounds.md`。

## 十七、用例清理："名不副实"的用例与其代码（2026-10-08）

> 详细记录已移入 `docs/archive/assertion_rounds.md`。

## 十八、v23：FTL 建链掩码越段 → ASSERT（§十三 判据的 FTL 侧漏网，2026-10-08）

> 详细记录已移入 `docs/archive/assertion_rounds.md`。

## 十九、v24：地址布局统一 + 加扰器重做（2026-10-08）

> 详细记录已移入 `docs/archive/assertion_rounds.md`。

## 二十、v25：同一个头文件里的两种调用约定 = 隐患（2026-10-08）

> 详细记录已移入 `docs/archive/assertion_rounds.md`。

## 二十一、v27：取消「0 = 全部」重载后的唯一拦网（2026-10-09）

> 详细记录已移入 `docs/archive/assertion_rounds.md`。

## §二十二 CW 集合退出命令结构体（v28）——「派生量不进结构体」的判据应用

> 详细记录已移入 `docs/archive/assertion_rounds.md`。

## 二十三、v29：步骤级用例的形态选择——参数化 vs 迭代（2026-10-09）

> 详细记录已移入 `docs/archive/assertion_rounds.md`。

## 二十四、v39：ASSERT 被 `NDEBUG` 裁空时——配置错误必须再补一道**运行期拒绝**（2026-10-10）

判据 2 说「调用者已保证的前置条件转 `ASSERT`」。这条在**被断言路径的失败后果是崩溃 / 可检测错误**时
是完备的：`NDEBUG` 下断言消失，代码继续跑，坏状态很快以别的方式暴露。但有一类配置错误**不**满足
这个前提——违反之后程序**照常跑完、不报错、结果看不出来**，只是**悄悄写坏了介质**。对这类错误，
只写 `ASSERT` 等于**在 release 档没有守卫**。

> **判据 9：当配置错误的后果是**静默的数据 / 介质破坏**（不崩、不报错、上层无法检测）时，
> 除了 debug 档的 `ASSERT`，**必须**紧跟一道 release 档的运行期拒绝
> （`if (违反) return FC_STATUS_INVALID_ARGUMENT;`），由调用者转成命令失败。
> 其余配置错误仍只写 `ASSERT`（判据 2 不变）。**

**两档各管什么，不是二选一**：

| 档 | 手段 | 给什么 |
|---|---|---|
| debug（`make tests`） | `ASSERT` | 最可定位的现场（判据 2 的理由不变）；板上任何误判停机而不是静默 |
| release（`NDEBUG`） | 返回码拒绝 | 生产里「**宁可不发这条命令**」的出口 |

拒绝**不是** `ASSERT` 的替代品——`ASSERT` 在 debug 档先停机，`return` 那行根本走不到；反过来，
`return` 也换不来 debug 档的现场。**两条都要写。**

**拒绝之后必须让调用链有个了结**：命令不能就这么丢掉。本工程的落地形态是
`fc_poll_fail_command(controller, command_id)`——注一条 `NAND_FAIL` 完成项，FTL 的 pending 照常释放。
不这么做的话，命令**永不完成**，上层表现为「挂死」，比报错难查得多。

**首次应用（v39）**：`fc_build_request_data_planes` 的 multi-plane 窄位图检查
（见 `fc_nand_design.md` §25.4 与 `docs/archive/changelog/fc_multiplane_narrow_bitmap_v39.md`）。违反时
NAND 侧多 plane handler 会**无条件遍历 `PLANE_NUM`** 下行地址，未覆盖的 plane 既无数据也无地址，
旧实现凭空造一条回退地址 ⇒ **静默给该 plane 编一页全 0xFF**，调用者完全不知道。这正是判据 9 说的
「静默写坏介质」。同时它让 `fc_build_dispatch_request` 从「构建不可失败」变成有**唯一**失败出口——
函数头注释与两条派发路径的兜底注释都已同步。

**统一写法（2026-11-06）**：判据 9 的「debug 断言 + release 拒绝」统一走
`include/common/assertion.h` 新增的生产宏，不再手写失败分支：

- `ASSERT_MSG(cond, fmt, ...)`：debug 打印条件/消息并停机；release 裁空。
- `ASSERT_REJECT(cond, ret, fmt, ...)`：debug 同上；release 做一次条件判断后 `return ret`。
- `ASSERT_REJECT_VOID(cond, fmt, ...)`：同 `ASSERT_REJECT`，用于 `void` 函数。
- `ASSERT_DEFAULT(type)`：`switch` 的 `default` 专用，debug 打印未处理的枚举类型名；release 裁空（H7/C7）。

禁止写成 `if (bad) { ASSERT(false); LOGE(...); return ...; }`：debug 下 `LOGE` 不可达
（`assertion_fail` 是 `_Noreturn`），`ASSERT(false)` 丢掉条件表达式，且同一判断被拆成
两条互斥的失败路径。`ASSERT_TRUE` 等富断言集仍只供测试使用，不随 `NDEBUG` 裁剪。

> **判据 10：被 `ASSERT` 守的变量，在 `NDEBUG` 下可能变成 unused——编译告警是唯一的信号。**
> `ASSERT` 展开为空后，只为它服务的局部变量（如 `const uint8_t all_planes`）会留下
> `unused variable` 告警。**release 档的告警要单独看一眼**：它同时是「这条断言在 release 下
> 没有等价守卫」的提示。本仓已知一处：FTL 侧 `ftl_cmd_resolve_cw_bitmap` 的越段 `ASSERT`
> （`assertion_guidelines.md` §十八）在 `NDEBUG` 下把 `allowed` 裁成 unused
> （`src/ftl/ftl_fc_interface.c:32` 告警）⇒ **FTL 侧 release 档同样无守卫**。
> 按硬规则本次**不动 `src/ftl/**`**，只记录。

## 二十五、v0.75：把"架构约定"变成可测断言（2026-10-14）

判据 2 处理的是「调用者已保证的前置条件」。本仓 v0.75（FTL 命令提交/完成解耦）引入了
一条**跨模块的架构约定**——「**完成服务函数内不得同步等 NAND**」。它没有单一调用点，
违反它的后果也不是崩溃，而是**嵌套泵**：`ftl_sync_submit_wait` 在完成派发循环里再泵一轮，
吞掉本应交给其它 job 的 completion，表现为**偶发的命令丢失 / 进度错乱**，极难复现。

> **判据 11：当一条约定是「某个函数**不得**在某上下文里被调用」时，把该上下文做成一个
> 可查询的**深度计数**（`ftl_op_is_in_dispatch()`），并在被禁函数的入口写
> `ASSERT(ftl_op_is_in_dispatch() == 0)`。**约定从此可测**——任何新调用点一旦越界，debug 档
> 当场停机，而不是留到上板后靠"偶发丢命令"去猜。**

**落地形态**：`src/ftl/ftl_op.c` 里 `g_dispatch_depth` 深度计数（单线程顺序主循环 ⇒ 普通变量，
无需原子），`ftl_op_drain` 的循环体用 `g_dispatch_depth++` / `g_dispatch_depth--` 包住服务函数调用
（含它内部的嵌套派发，故计数天然覆盖嵌套）。`l2p_sync_pump()`（`l2p_cache.c`，四个 `*_sync`
逃生口的自旋泵）第一行就是 `ASSERT(ftl_op_is_in_dispatch() == 0)`。

**与判据 2 / 9 的关系**：判据 2 的断言守的是**值**（指针/索引非空/在域内），判据 9 守的是
**静默介质破坏**，本条守的是**控制流位置**。三者互补：本条不检查任何入参，只回答
"此刻我在哪一层"。它也**不**在 release 档保留运行期拒绝——违反它不会静默写坏介质，只会在
debug 档暴露；release 档即便误调，也只是多泵一轮（后果与阶段 1 前的旧行为同级），
不值得为它增加一条生产路径上的分支。

**目标终态**（`ftl_op.h` 顶部已写明）：`ftl_sync_submit_wait` / `l2p_cache_*_sync` 只被
开机 / 格式化 / 出厂扫描 / CLI 路径调用；这些路径都在 `main` 进入 superloop **之前**
（或 CLI 的独立上下文）执行，此时没有别人 drain CQ，改异步反而死锁——它们是**结构性同步**，
不是漏网。断言把"还有谁在重入"从口头约定变成编译期之后仍可验证的事实。

## 二十六、FTL 层断言瘦身：模块内部不逐函数复检同一入参（2026-10-15）

用户指出（2026-10-15）：FTL 层「每个函数判断一次参数是否合理」——同一前提在调用链**多层重复
断言**，判断分支开销大；断言应主要落在对外接口，**以一个模块为界限**，模块内部默认没问题。

**盘点**：`src/ftl/*.c` 共 **111 处 `ASSERT`**（16 文件；无 `ASSUME`），按「值从哪来」分四类：
[I] 接口 27 / [BOUND] 子模块公开入口 10 / [INV] 不变量 48 / [REDUND] 冗余 26。
本轮删 **[REDUND] 26 处**，保留 85 处（I / BOUND / INV 一个不动）。

**删的是什么（各按既有判据，无新规则）**：
- **同源守卫恒真 → 删**（§十附判据 1）：`pending.c` 全族 **13× `ASSERT(ftl != NULL)`**——host 入口
  `ftl_submit_*` 已 if-guard，`pending_init` 注释自认「唯一调用者 ftl_init 已判过」，全模块恒真。
- **内层已断言 → 外层死守卫删**（§十附判据 2）：**`cmd != NULL` 10×**——`fc_command_id_to_command`
  公共壳已断言 ID 在池内恒非空，派发（`ftl_op_dispatch`）+ 9 个完成服务回调（`ftl_op_*`）各自再
  断一次是死守卫。
- **重复守卫收敛到共用漏斗**（§十附判据 2）：`n_cw` 范围三层重复（`ftl_issue_subop` →
  `pending_add` → `ftl_drain_group`）收敛到 **`pending_add`**（pending.h 公开入口）唯一权威点。
- **注释自认冗余**：`power_failure_recovery.c` 的 `sb_idx != UINT16_MAX`，注释明说
  `allocator_add_closed_sb` 内部已断言列表未满。

**保留什么（一处不动）**：host 入口（用返回码，判据 1，本就无 ASSERT）+ **FTL↔FC 命令边界**（[I]，
「配置错误一律 ASSERT」）+ **子模块公开入口**（[BOUND]）+ 内部**高价值不变量**（[INV]：状态机非法态
`ASSERT(0)` / 实例归属 `&ftl->xxx == ctx` / 簿记计数 / 运行时上下文 `ftl_op_is_in_dispatch()`）。

**固化约定（防回加）**：**模块内部不逐函数复检同一入参。** 边界 = host 入口返回码 +
FTL↔FC 命令边界 / 子模块公开入口 ASSERT；内部只留高价值不变量。新增内部函数时，入参有效性
已由唯一调用者保证就不必再断言；只有「调用方可能传错」才在边界断言。

**行为影响**：release（NDEBUG）下断言整体裁空、表达式不求值——本轮删的是 debug 档冗余 +
代码噪音，**不改任何 return / 分支行为**。编译三关全过（`make all` / `make release` /
`make tests UT=FTL`），**零新告警**（release 档 `ftl_fc_interface.c` 的 `allowed`/`plane_bits`
未用告警是保留的 [I] 断言被裁空的既有告警，见文件头注释，非本轮引入）。

**跟进（同日，用户定调「不该发生的分支用 ASSERT 而不是返回」）**：完成回调里 3 处
`if (ftl == NULL) { LOGE; release; return; }`（`ftl_host_io_callback` / `ftl_l2p_callback` /
`ftl_read_recovery_callback`）与 `pending_group_complete_subop` 的「组已释放防御早退」
一并收口成 `ASSERT`——路由表 boot 前设好、完成派发保证实例恒非空，原 release-and-return
是 debug 走不到、release 静默吞错误的死分支；release（NDEBUG 裁空）下若真走到则
NULL 解引用留现场（可诊断停机）。**⚠ 形态 B 不动**：非法 phase 的 default 分支
（journal / wear / bbt / checkpoint / metadata）分支体有 `fc_command_release` + 复位 job
的**资源副作用**，裁掉会命令池泄漏 + job 卡死——它们是清理逻辑，不是错误返回。

## 二十七、allocator.c 入口守卫收口：补做 §八 Tier 1 的欠账（2026-10-17）

**背景**：用户读 `src/ftl/allocator.c` 时问「这些与预期不符的判断不是应该用 `ASSERT` 吗」。
核对后确认这是**已登记、未闭合的欠账**，不是新判据——两处早已点名：

- 判据 2 的层内模块头名单里**逐字写了 `allocator.h`**（§一）；
- §八 的 Tier 1 表里，`ftl/` 一行第一个就是 **`allocator.c`**。

而 `.c` 里 10 处入口守卫仍是 `if (…) return 错误码`。本轮按判据 2 收口。

### 转换清单（判据 2：值从哪来）

| 函数 | 转换的守卫 | 唯一调用者 / 保证 |
|---|---|---|
| `allocator_init` | `alloc/tables == NULL` | `ftl.c` 传 `&ftl->alloc` / `&ftl->tables` |
| `allocator_select_plane_block_from_pool`(static) | `alloc/blocks == NULL`、`plane >= PLANE_NUM` | `allocator_open_sb` 循环变量；`allocator_alloc_log_block` 的 `dir->cur_metadata_plane` |
| `allocator_remove_closed_sb` | `alloc == NULL`、`closed_idx >= closed_sb_count` | `migrate.c` 找 victim 时**已把越界换成 `UINT16_MAX` 并拦掉** ⇒ 内层是死守卫（§十附判据 2） |
| `allocator_close_open_sb` | `alloc == NULL`、`temperature >= FTL_TEMP_NUM` | `ftl.c` 传 `&ftl->alloc` + 编译期 HOT/COLD 常量 |
| `allocator_alloc_pair` | `alloc/out == NULL`、`temperature >= FTL_TEMP_NUM` | `ftl.c` 栈上 `new_pba`；`migrate.c` 的 `m->dst` + 编译期 HOT/COLD 常量 |
| `allocator_free_block` | `alloc == NULL`；判据 9 四条：越界 / BAD / 重复归还（`state == FREE`）/ 池满 | `migrate.c` / `main.c` / `bbt.c` / `allocator.c` 内部 OP 补池 |
| ~~`allocator_free_log_block`~~ | v0.80 已删除（纯转发薄包装）⇒ 直接走 `allocator_free_block` | — |
| `allocator_alloc_log_block` | `out_gblock == NULL`（`alloc` / `blocks` / `plane` 由 `allocator_select_plane_block_from_pool` 统一校验） | `metadata.c`（`plane` 来自 `dir->cur_metadata_plane`） |
| `allocator_add_closed_sb` | `alloc/plane_blocks == NULL`、`temperature >= FTL_TEMP_NUM` | `power_failure_recovery.c` 局部数组 + 默认温度常量 |
| `allocator_remove_from_free` | `alloc == NULL` | 唯一出池闸门：`power_failure_recovery.c` / `bbt.c` / `migrate.c` + `allocator.c` 内部 open_sb / alloc_log |

### 保留项（**不要**"顺手"转成 ASSERT）

| 位置 | 类别 | 依据 |
|---|---|---|
| `allocator_alloc_pair` 池空 → `FTL_AGAIN` | 资源类结果 | 判据 3 |
| `allocator_alloc_log_block` 池空 → `FTL_AGAIN` | 资源类结果 | 判据 3（`test_ftl.c` 测的正是它） |
| `allocator_close_open_sb` 已关闭 → `FTL_ERROR` | **幂等早退** | §十三 `fc_command_release` 先例；调用者在 program 失败路径上会连续两次关同一 SB |
| `allocator_remove_from_free` 不在池内 → no-op | 文档化契约 | **判据 4 明文举的就是这个例子** |

### 新增守卫（判据 9）—— 本轮唯一的新增

`allocator_free_block` 两处，形态照抄 `fc.c` 的 `fc_build_request_data_planes`（§二十四）：
`ASSERT` 先行 + 紧跟 `if (违反) 拒绝`，注释写明"`ASSERT` 在 `NDEBUG` 下被裁空"。

1. **缺 `gblock < FTL_BLOCK_TOTAL` 上界**（当时与孪生兄弟 `allocator_free_log_block` 不对称；
   该薄包装已于 v0.80 删除）。
   `blocks[]` 之后**紧跟** `free_blocks[]`（同为 `ftl_tables_t` 成员）⇒ 越界索引**静默改坏
   Free Pool 数组** = 漏账（不崩、不报错、容量无声流失）⇒ `ASSERT` + release 拒绝（`LOGE` + `return`）。
2. **`free_count` 满时静默丢弃**（与 v0.78 修掉的 Closed 列表静默丢弃同形态）：池满 = 某块被
   重复归还（double free），入池同样越界写 `free_blocks[]` ⇒ `ASSERT` + 既有 `if` 作 release
   拒绝，补 `LOGE` 把"静默"变成"可诊断"。

⚠ 因为 `allocator_free_block` 现在自己把 `gblock` 范围，薄包装 `allocator_free_log_block`
里的同名检查**删掉不重复**（§十附判据 2：重复守卫收敛到共用漏斗）；该包装已于 v0.80 整体删除。

### 连带：删用例（§四）

`tests/unit/test_ftl.c` 删 3 处**子断言块**（整条用例保留，用例数不变，仍 53）：

- `allocator_add_closed_sb(a, NULL, …)` 的哨兵返回
- `allocator_remove_closed_sb(a, 越界)` 的 no-op
- `allocator_alloc_log_block(a, PLANE_NUM / …, NULL)` 的拒绝

**不动**：池空（判据 3）、幂等早退、`remove_from_free` 越界 no-op（判据 4）。

### 头文件契约同步

`include/ftl/allocator.h` 四处 `@return` / `@param` 去掉"参数非法"（§十三 落地注意），
文件头补**不变量 ⑤**（2026-10-17 重编号为 **④**）：指针 / 索引类实参是前置条件，由调用者保证，非法即 `ASSERT`。

### 附带决定（待议）

`allocator_init` 转后**恒返 `FTL_OK`**，但保留 `ftl_status_t` 签名。§十附判据 3 主张
「返回值恒为常量 ⇒ 改 `void`」——本轮**不做**（会波及 `ftl.c` 与夹具），登记为待议。

### 验证

`make all` / `make release` / `make tests UT=FTL` 三关**零新告警**（release 档
`ftl_fc_interface.c` 的 `allowed` / `plane_bits` 与 `crc_port.c` 的 `status` 是既有告警，
见判据 10）。**未上板**——按 R4 需上板跑 `/ftl/` 叶子套件（断言在板上是活的，误判会停机
而非报失败）。

## 二十八、FTL 层入口守卫全量收口（2026-10-17，B 分批）

**起因**：用户读 `src/ftl/allocator.c` 时指出——模块里一批「与预期不符就 `return` 错误码」
的入口守卫，按本仓既有判据本应写成 `ASSERT`。用户随后定调：「**这么说来你有很多欠账啊，
还是静默的，我都不知晓**」「**B 分批全做**」——即：把 §二十七 的口径推到 FTL 层**全部**同类
守卫，**按文件分批**；用户当前在**读码 + 重构**，故**整轮改完再上板验证**（不是每批验）。

**已批准的两条口径**（上一轮 AskUserQuestion）：
① 收口方式 = **混合：死守卫删 + 其余转 `ASSERT`**；② §六 的 `journal.c` 豁免 = **一并收窄**。

### 一、本轮落位（可复核）

实测口径：`git diff -U0 -- src include | grep -cE '^\+[[:space:]]*ASSERT'` 等（**只数语句**）。

| 文件 | 新增 `ASSERT` | 移除入口守卫 `if` | 说明 |
|---|---|---|---|
| `allocator.c` | 22 | 10 | §二十七 已记（本轮起点） |
| `metadata.c` | 31 | 20 | 最大一批；含 `metadata_ring_locate` 的**废形参删除**（见三、） |
| `journal.c` | 12 | 9 | 句柄族转 `ASSERT`；**回放数据校验与环翻转控制流不动**（§六 收窄） |
| `l2p_cache.c` | 12 | 8 | 含 `line_idx` 界内断言；`line == NULL`（cache miss）与依赖态判空**保留** |
| `bbt.c` | 7 | 7 | 5 转 + 2 删（`bbt_factory_scan` / `bbt_load` 同文件调用者） |
| `ftl_fc_interface.c` | 7 | 6 | 全为**出参**（FTL↔FC 边界）；`cmd == NULL`（池空，判据 3）**保留** |
| `migrate.c` | 4 | 5 | `mig_gc_begin` / `mig_wl_begin` / `mig_predict_begin` / `mig_handle` 转；`mig_job_start`（static）删 |
| `power_failure_recovery.c` | 4 | 9 | 3 个**对外入口**转；6 个同文件内部路径函数删 |
| `bg_scheduler.c` | 3 | 2 | 任务表项判空 |
| `ftl.c` | 3 | 3 | `ftl_init` + 本轮补的 `ftl_flush` / `ftl_trim`（C5：唯一调用点已 `ASSERT`） |
| `gc.c` | 3 | 1 | — |
| `checkpoint.c` | 2 | 4 | `cp_job_begin` / `checkpoint_journal_watermark` 转；另 2 处见三、 |
| `wear_leveling.c` | 0 | 6 | **纯删**（恒真）——见三、的分叉 |
| `wear.c` | 0 | 2 | **纯删**（恒真） |
| `ftl_op.c` | 0 | 1 | **纯删**（同文件调用者） |
| **合计** | **110** | **93** | 其中**约 15 处是纯删**（未落断言） |

> ⚠ **2026-10-19 补记**（§3.12 审查，见 `known_risks.md §2J`）：上表是**当轮批次记录**，
> 不随之后改动回写。当前实际状态：
> `bg_scheduler.c` 的任务装载 = `bg_scheduler_init`（装载内部静态表 `s_bg_tasks`，
> **无 ASSERT**：表长是编译期常量，`_Static_assert` 已证明 == `BG_TASK_COUNT`）；
> 接线（`bg_task_*` 适配器 / 任务表 `s_bg_tasks` / 档位表 `s_bg_level_cfg` / Phase 3 入口）
> 与引擎同处 `src/ftl/bg_scheduler.c`；`bg_scheduler_init` / `bg_scheduler_service` 的入参由
> `ftl_init` / `ftl_service` 调用链保证，不重复 `ASSERT`。

配套：8 个头文件（`allocator.h` / `bbt.h` / `migrate.h` / `metadata.h` / `journal.h` /
`l2p_cache.h` / `ftl_fc_interface.h` / `ftl.h`）的 `@param` / `@return` **契约同步**
（删「参数非法」、改「**前置条件**，非法即 ASSERT」），`allocator.h` 补**不变量 ⑤**（2026-10-17 重编号为 **④**）。

### 二、本轮用的判据（一句话）

> **入参是不是「调用者自己持有的指针 / 自己算出的索引」**（判据 2）——
> 是 ⇒ 收口成 `ASSERT`（§二十七）；不是（host 输入 / 介质读回 / 池空 / 表满）⇒ **保留返回码**。

具体到本轮的**保留项**（**不要**"顺手"转）：

| 类别 | 实例 | 依据 |
|---|---|---|
| host 入口返回码 | `ftl_submit_read`/`ftl_submit_write` 的 `req`/`req->buffer` | 判据 1 |
| 资源类结果 | 全部 `fc_command_alloc()` 后的 `cmd == NULL`、`pending_alloc()` 后的 `p == NULL`、`ftl_fc_interface.c` 的 `buf == NULL` | 判据 3 |
| 介质读回数据 | `journal.c` 的 `count > JRN_ENTRIES_PER_PAGE` | 判据 3 + H29（§六） |
| 运行时/依赖态 | `l2p_cache.c` 的 `cache->md_dir == NULL \|\| cache->alloc == NULL`（Region 加载中）、`ftl.c` 的 `platform->time_us == NULL`、`gc_should_trigger` 的 `alloc == NULL`（0 无触发） | 判据 3 |
| **UT 钉住的文档化契约** | `checkpoint_check_trigger(NULL)`/`checkpoint_is_emergency(NULL)`/`gc_should_trigger(NULL)`/`rr_check_predictive(NULL,…)`/`rr_get_health(NULL)`、`bbt.c` 两处 | **判据 4**（`tests/unit/test_ftl.c` 逐条钉住 ⇒ 转 `ASSERT` 会当场打红 UT） |
| 可选参数 / 可选出参 | `ftl_op_submit(cmd, op, ctx)` 的 `ctx != NULL`、`l2p_cache_flush_*` 的 `out`、`journal_scan_latest`/`journal_resume` 的 `latest` | §四 可选参数语义、辨析 B |
| 幂等早退 | `allocator_close_open_sb` 已关闭 → `FTL_ERROR` | §十三 `fc_command_release` 先例 |

> ⚠️ **判据 4 是本轮最容易被"顺手"破坏的一条**：这 5 个查询函数的 `NULL → 默认值` 不是守卫，
> 是**有 UT 钉住的公开契约**（"查不到就问不出"，语义上等同"没有这件事"）。把它们转成
> `ASSERT` 会让 UT 当场红——**这正是判据 4 存在的意义**。

### 三、口径分叉的**裁决结果**（2026-10-17 用户定）

本轮暴露出本仓**两条已落地判据在同一类守卫上给出相反答案**：

| | §二十六（2026-10-15，`adf897e`） | §二十七（2026-10-17） |
|---|---|---|
| 口径 | **恒真 → 删**（§十附判据 1）：「入参有效性已由唯一调用者保证就不必再断言」 | **判据 2 → `ASSERT`**：「`allocator.h` 在模块头名单里 ⇒ 是边界」 |
| 实例 | `pending.c` **13 处 `ASSERT(ftl != NULL)` 删除**——**含 `pending_lookup`/`pending_remove`/`pending_count`/`pending_group_*` 等头文件公开入口** | `allocator.c` 10 处守卫**转 `ASSERT`** |
| 冲突点 | 二者对「子模块公开入口的恒真指针守卫」给出**相反处理** | |

**用户裁决：保持本轮现状 —— 删掉的就不再补回**（原话「**删除就删除了，暂时这样就行**」）。
⇒ **不做任何反向操作**：

- §二十六 已删的 `pending.c` 13 处 **不补回**；
- 本轮按「调用者全在本 .c 内 ⇒ 纯删」处理的 `bbt_load` / `metadata_handle_reset` / `ftl_boot_fresh_start`
  /`clean_start`/`power_failure` 等 **不补回**；
- 落在争议线的 `wear_leveling.c` 6 / `wear.c` 2 / `checkpoint.c` 2 **保持删除**。

**⚠️ 遗留（未闭合，登记不掩盖）**：上列第三组共 10 处的调用者**在别的 .c**
（`wl_should_trigger`/`wl_select_victim` ← `migrate.c`；`checkpoint_handle` ← `ftl.c`/`bg_scheduler.c`；
`checkpoint_find_latest` ← `ftl.c`/`power_failure_recovery.c`），按 §二十七 的判据 2 本应 `ASSERT`，
本轮按「恒真」删了。⇒ **全仓口径暂未强行归一**，现行事实口径为：

> **入参由唯一调用者结构性保证 ⇒ 删**；跨 .c 调用且便于调试停机处**留 `ASSERT`**（无硬边界，
> 由改动者按「debug 停机信息值不值得」裁量）。

两条判据的**边界待后续统一**，不阻塞本轮（R7：读码/重构期只分析不改造，且反向操作会波及
已推送的 `adf897e`）。

### 四、判据 10 的一次**真实命中**（本轮最有价值的副产物）

`metadata_ring_locate` 的 `const ftl_topology_t *topo` 形参**在本函数体内从未被使用**——
唯一的"用途"是入口的 `topo == NULL` 守卫。该守卫按判据 2 收口成 `ASSERT` 后，`topo` 在
**release 档**当场报 `unused parameter`（判据 10）。

⇒ 据此确认它是**废形参**（H5），而不是"漏用"。处理：**删形参**（不是 `(void)topo;`，也不是
把告警记成既有）。调用点 7 处同步（`checkpoint.c` ×2、`bbt.c` ×3、`metadata.c` ×2）。
头文件已留 ⚠ 注记。**这是判据 10「release 档的编译告警是断言在 release 下没有等价守卫的
唯一提示」的正面用法**——它把一个**静默的**废参数变成了编译期可判的事实。

### 五、`static inline` 访问器族的 `NULL` 守卫（C 批，2026-10-17 **已闭合**）

**原先登记为「未做」（B 批）**：`include/ftl/` 里一族 **`static inline` 访问器**带
`NULL → 默认值` 守卫，共约 20 处，理由记的是「形态与 `.c` 入口守卫不同 —— 是纯 getter/setter，
`NULL → 默认值` 是这套访问器库的一致约定（且部分被 UT 或文档钉住）」+「`bst.h` 在热路径上」。

**C 批裁决：那个「形态不同」的理由不成立**，逐族按**同一条判据**重判：

> 判据 = **该函数的 NULL 语义有没有被文档化或被 UT 钉住**。
> 有 ⇒ 是**契约**，按辨析 B / 判据 4 **保留**；
> 没有 ⇒ 指针属前置条件（判据 2），守卫是**恒真的静默吞**（§十附判据 1）⇒ 转 `ASSERT` + 无条件执行。

#### 已转 `ASSERT`（13 处）

| 位置 | 处数 | 依据 |
|---|---|---|
| `bst.h` 全部 12 个访问器（`ftl_block_is_bad`/`set_bad`/`state_is`/`state_set`/`state_clr`/`set_temp`/`get_temp`/`sb_id`/`set_sb`/`add_valid`/`sub_valid`/`clear_valid`） | 12 | 见下 |
| `l2p_cache.h` 的 `l2p_cache_is_thrashing` 的 `cache != NULL` | 1 | 唯一调用点传 `ftl->cache`（阶段 B 起 cache 为共享指针；`l2p_cache_set_md_dir` 已随共享 cache 删除） |

**`bst.h` 的判据链**：

1. **判据 2 的层内模块头名单逐字含 `bst.h`**（§一）。
2. **无文档化 NULL 语义**：12 个访问器的注释**没有一条**写过 NULL；且其默认值**语义上是错的**
   （`get_temp(NULL) → HOT` 读作"没有这个块 ⇒ 是热的"、`state_is(NULL, st) → false` 读作
   "没有这个块 ⇒ 不是这个状态"）。对比判据 4 那 5 个被 UT 钉住的查询函数
   （`gc_should_trigger(NULL) → 0` = "没有 allocator ⇒ 无触发"），后者的默认值是**连贯的**。
3. **调用点普查：全仓零 NULL**（逐条核实 `src/` 11 个 .c + `tests/unit/test_ftl.c`）。
   实参只有两种形态：`&<…>->blocks[<索引>]`，或紧邻使用前赋值的局部/形参（其右值同样是
   `&…blocks[<索引>]`）。**根因**：`ftl_allocator_t.blocks` 由 `ftl_table_refs_s` 绑定到
   HIL **独立定义**的数组（生命周期覆盖全程）⇒ 取元素地址**结构性恒非空**。
   （附带：调用点传的是 `&blocks[i]` 而非基址 `blocks`，故即使基址真为 NULL，
   `&blocks[i]`（`i > 0`）也不是 NULL 字面量 —— 守卫**连它想防的情形都拦不住**。）
4. **「热路径」这条理由方向是反的**：现写法 `if (b != NULL) { … }` 在 release 档是**活的**
   运行期分支（`ftl_block_add_valid` 每 CW 一次）；转 `ASSERT` 后 `NDEBUG` 把它裁成
   `((void)0)` ⇒ **release 零开销、debug 保留停机检查**（H14）。

⚠ **只删 NULL 守卫，不动任何返回值语义**。尤其 `ftl_block_get_temp`：`0=HOT / 1=COLD` 是
**两值域不是真值域**（不是 `bool`），UT 逐条钉住该值域。

#### **保留**（3 组，**不要**"顺手"转）

| 位置 | 为什么保留 | 依据 |
|---|---|---|
| `metadata.h` 的 `ftl_l2p_region_placement` 的 3 处 | **文档化可选出参**（`metadata.h:249` 逐字「输出指针可为 NULL（只取需要的一项）」） | 辨析 B |
| `metadata.h` 的 `metadata_dir_busy` 的 `dir != NULL` | 其 `@return` 注释**逐字写了**「false = 空闲（**或 dir == NULL**）」 | 辨析 B |
| `bbt.h` 的 `bbt_is_bad` / `bbt_set_bad` | **UT 逐条钉住**：`test_ftl.c:1452` `bbt_is_bad(NULL, 5U) == 0`、`:1466` `bbt_set_bad(NULL, 1U)` 不崩 | **判据 4** |

另注：`platform_now_ms`（`platform == NULL || platform->time_us == NULL`）**不在本族** ——
它属 §二 的「运行时/依赖态」（判据 3），保留。

#### 验证状态（R1）

`make all` / `make release` / `make tests UT=FTL` 三关见文末。**未上板**（按用户定调
「重构完再上版验证」）。⚠ 断言在板上是活的（`make tests` 走 debug 档）⇒ **最终必须上板**
跑 `/ftl/` 叶子套件：一旦有调用点真传 NULL，表现会从「静默吞」变成**板子停机**。

### 六、验证状态（R1）

- `make all` / `make release` / `make tests UT=FTL` **三关全过**，**零新告警**
  （release 档仅 `ftl_fc_interface.c` 的 `allowed` / `plane_bits` 两条**既有**告警，
  即判据 10 形态，非本轮引入）。
- **未上板**。按用户定调「**重构完再上版验证**」，本轮**不**上板。
- **未提交、未推送**（R3）。

---

## 二十九、故障 shell（`SSD_FAULT_ACTION`，2026-11-12）

断言仍然 fail-fast：**不恢复业务**、不跑 FTL/NAND、不喂狗。区别只在停机后的处置，
由编译期宏选择：

| 模式 | 行为 | 默认 |
|---|---|---|
| `SSD_FAULT_ACTION_HALT` | 打印记录后死循环（接调试器看现场） | **所有构建** |
| `SSD_FAULT_ACTION_SHELL` | 进入只读故障 shell（诊断用，opt-in） | 否 |
| `SSD_FAULT_ACTION_RESET` | 打印后 `NVIC_SystemReset` | 否 |

打开 shell：`make ... CFLAGS_EXTRA=-DSSD_FAULT_ACTION=1`（0/1/2）。
默认 HALT 与主流「停机 + 调试器」一致；shell 只作为调试器不可用时的兜底。
⚠ 对象目录不包含 `CFLAGS_EXTRA` 的哈希：切换模式后先 `touch src/common/assertion.c
src/common/fault_shell.c`（或 `make clean`），否则会复用旧 `.o`、模式不生效。

### 只读边界（必须遵守）

- 白名单：`ftl_ctx [module]` / `crash` / `stack` / `pc` / `help`。
- **禁止**任何写操作、分配、NAND 访问、后台任务推进、喂看门狗。
- 不走主 CLI（主 CLI 会调业务逻辑），单独最小解析器。
- 输出走 `fault_port_putc`（轮询 USART）+ `log_set_sink` 重定向；
  **不走异步日志环** —— 断言路径已经关掉 TX DMA/TXE，走环会丢输出。
- 输入走 `uart_rx_read`（USART IRQ 生产者）。
- 防御式读取：断言后指针可能已损坏，dump 函数必须容忍 NULL/边界。

### 为什么不是简单 `while(1)`

`while(1)` 与现有 `for(;;)` 行为相同，仍然无法查询现场。SHELL 模式是在
「停业务」与「保留现场」之间取平衡；release 默认 HALT，需要现场时单独出镜像。

### 验证状态

`make all` / `make release` / `make tests UT=FTL` 均 0 warning；debug/UT 镜像含
`FAULT SHELL` 字符串，release 不含。**未上板**（R1）。

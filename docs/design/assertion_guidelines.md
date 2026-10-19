# 全工程断言判据（ASSERT / ASSUME）

断言设施见 `include/common/assertion.h`。本文是**全工程**判据，各层（FC / NAND / FTL / controller / platform / common）共用同一套规则。FC 层最初在 `docs/design/fc_nand_design.md` §24 定稿，本轮推广到这里；§24 保留 FC 特有的例外说明并指向本文。

## 一、四条判据

1. **外部输入校验保留返回码**
   只有**来自外部**的输入才需要返回错误码：host 的 opcode/LBA、UART CLI 的参数、从介质读回的字节（见 §六辨析 C）。这些值的合法范围由外部世界决定，损坏/非法是**预期情形**，必须返回 `FC_STATUS_INVALID_ARGUMENT` / `FTL_ERROR` / `SSD_STATUS_INVALID_ARGUMENT` / `NAND_RESULT_INVALID_ARGUMENT` 等让上层处置。
   **参数是不是"层对外头文件的入口"不再是判据**——关键在**值从哪来**：调用者（哪怕是跨层的 FTL/controller）用自己铸出的句柄、自己算出的索引、自己持有的指针调进来的，一律算前置条件（判据 2），即使该函数声明在 `nand_dispatch.h` / `fc_interface.h` / `nand_mt29f4g08abadawp.h` 这类"边界头"里。
   （本判据原先把"边界头的全部入口"整体划为外部边界，属**过度概括**，见 §七辨析 D 与 §九。）

2. **调用者已保证的前置条件转断言**
   `static` 函数、层内模块头里的接口，以及**跨层入口中调用者必然传有效值的指针/索引参数**，前置条件由调用者保证。守卫一律写成 `ASSERT(条件)`，并**删掉原来的 `return` 与恢复/降级分支**（"跳过坏 ID 继续找"、"回滚分配再返回"、"防御性降级"这类）。
   层内模块头指：`include/nand/nand_engine.h`、`nand_fmc.h`；`include/ftl/*.h`（除 `ftl.h` 外，含 `allocator.h`/`gc.h`/`bbt.h`/`journal.h`/`metadata.h`/`l2p_cache.h`/`checkpoint.h`/`pending.h`/`read_recovery.h`/`wear_leveling.h`/`completion_table.h`/`bst.h`）。
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
   这是判据 2 的例外，但**只在测试是被测函数的正当调用者时成立**。`tests/` 里拿**文档化合法输入**调内部函数验证行为（如 `allocator_remove_from_free(..., 越界)` 是文档化的 no-op、`ftl_is_metadata_block(NULL,…)` 是文档化分类——两者都是 `ftl_ut_munit.md §4.1/§4.6` 的用例）的，按判据 1 保留返回码。
   反之，**只有测试会走"传非法参数"这条分支**（生产调用者从不传）的，按判据 2 转断言并**删掉对应用例**——见 §四。

## 二、两条辨析（最容易搞错的地方）

**辨析 A：`ptr == NULL` 要看指针来源。**
来自 `fc_command_alloc()` / `mem_alloc` 的是**资源**（判据 3，保留）；来自函数入参的才是**前置条件**（判据 2，转断言）。FTL 里满屏的 `cmd == NULL` 绝大多数是前者。

**辨析 B：语义分支不是前置条件。**
用 `NULL` 表达"这个可选参数没给"的，是有意设计的语义，不是调用者的编程错误。例：`ds_deque.c` 的 `cp == NULL`（浅拷贝选择器）、`ftl_is_metadata_block(md_dir == NULL, …)`（固定区间照常分类）、`fc_ldpc_decode(..., err_bits == NULL)`（不取纠正位数）。~~`fc_submission_queue_pop_n(ids == NULL)`（仅消耗队列）、`fc_command_status_init(status, command == NULL)`（无命令来源时填哨兵值）~~ **（2026-10-08：两函数因零生产调用者被整体删除，见 §十七）**。

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
- newlib-nano 不支持 `%ll`：日志/断言文本里 64 位一律 `(unsigned long)` + `%lu`。

## 六、辨析 C：校验"不可信持久化数据"不是前置条件

判据 2 只适用于**调用者编程错误**造成的违反。若某处的"异常值"来自**上电/掉电后从介质读回的字节**，那它属于判据 3——损坏数据是掉电的**预期情形**，必须返回错误码让上层走恢复流程，不能断言。

典型：`journal.c` 回放时 `count > JOURNAL_ENTRIES_PER_PAGE`——回放读的是 NAND 上的 journal 页，页头可能因掉电撕裂而非法。这里返回错误、停止回放才是正确行为。同理，`journal.c` 里页/块翻转的控制流（`page++`、`block` 切换）也只是普通分支，不是守卫。

> ⚠️ **2026-10-17 收窄**：本节原结论写作「**`journal.c` 整个文件不转断言**」——那是把
> **文件**当成了豁免单位，而判据的适用面是**值从哪来**，不是**代码在哪个文件**。
> 同一次落地的 §二十六 已经给出正确口径：「边界 = host 入口返回码 + 子模块公开入口 ASSERT」。
> 按该口径重扫 `journal.c`，其中的**句柄类守卫**（`journal == NULL`、`cache == NULL`、
> `out_addr == NULL` …）收的是**调用者自己持有的指针**，属判据 2，**已全部转 `ASSERT`**
> （9 处，见 §二十八）；只有**下面这两类**留在本节豁免内：
>
> 1. **回放读回的字节**（`count > JOURNAL_ENTRIES_PER_PAGE` 一类）——数据来自介质，
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

设施 `fd2a0d9` 建好后，先在 FC 层落地（§24，见 `fc_nand_design.md`），本轮推广到**全部非 FC 层**。

> ⚠️ **本表原为「计划 + 已完成」的混合体，措辞让人读成「已全部落地」。** 2026-10-17 逐文件
> 实测（`grep -cE '^[[:space:]]*(ASSERT|ASSERT_[A-Z_]+)\(' <file>`，只数语句、不数注释提及）
> 后发现**三处与事实不符** —— 这正是 R1「验证状态必须如实标注」要防的形态：**没落地的东西
> 写成了「已落地」，下次读的人就把它当既成事实**。故改为状态表：只承认**能在代码里数出
> ASSERT 的行**；「原计划」列写明当初打算做什么；不符处单列 `⚠️`。
>
> 复核口径：**断言是活的**（debug 档停机），所以「文件里有几条 ASSERT」是可直接读出的证据，
> 不需要靠回忆。

| 层 | 文件 | 原计划 | 实测（2026-10-17） | 状态 |
|---|---|---|---|---|
| `nand/` | `nand_engine.c` | UOP type 前置条件 | **18**（`state`/`backend`/`request`/`completion`/`timing` 前置条件、`uop->type < NAND_UOP_COUNT`、`handler != NULL`） | ✅ |
| | `nand_dispatch_tables.c` | 5×`cw_buffers` | **5**，全部 `task->cw_buffers[n] != NULL` / `cw_buffer != NULL` | ✅（与计划逐字一致） |
| | `nand_fmc.c` | DATA_IN/OUT | **5**（`config != NULL` + DATA_IN 2 + DATA_OUT 2） | ✅ |
| | `nand_dispatch.c` | default → `ASSERT_UNREACHABLE()` | **9**（ch/ce/lun 界、整批前置校验、链成员同 ch/ce/lun、`sink != NULL`）；⚠️ **全仓 `ASSERT_UNREACHABLE` 命中 0** | ⚠️ 措辞与实现不符（见下注 1） |
| | `nand_engine_start`/`step` | 前置条件（头文件已文档化） | 已含在 `nand_engine.c` 的 18 条内 | ✅ |
| `ftl/` | `allocator.c` | Tier 1 | **24** | ✅（2026-10-17 §二十七 补做） |
| | `power_failure_recovery.c` | Tier 1 | **34**（对外入口 `ftl_media_check_format`/`ftl_media_format`/`ftl_boot_recovery` + 恢复/重建/Carve 内部前置条件） | ✅（2026-10-19 掉电恢复审查） |
| | `wear_leveling.c` | Tier 1 | **0** | ⚠️ 见下注 2 |
| | `migrate.c` | Tier 1（**v0.75 阶段 4**：`ftl_migrate_callback` 的 `completion`/`ctx` 非空与 `ctx == &ftl->migrate_handle`、`mig_begin_predict` 的 `closed_idx` 界内、`mig_finish` 的单飞行门） | **10** | ✅ |
| | `pending.c` | Tier 2：6×`ftl == NULL` | **6**，但**全是下标越界**（`index < FTL_PENDING_MAX` / `gi < FTL_GROUP_MAX`），**没有 `ftl == NULL`** | ⚠️ 措辞与实现不符（见下注 3） |
| | `read_recovery.c` | Tier 2：`rr_record_ecc`/`rr_predictive_scan`/`rr_retire_block`/`rr_handle` | **10** | ✅ |
| `controller/` | `controller.c` | Tier 1 | **6**（`controller != NULL`、ch/ce/lun 界、路由结果 `tftl`/`fftl`） | ✅ |
| | `request.c` | Tier 1（route 越界、ftl 槽位恒非空） | **2**，均为路由结果 `ftl != NULL`；⚠️ route 的 ch/ce/lun 越界断言在**被调方** `controller.c`，不在本文件 | ⚠️ 措辞与实现不符（见下注 4） |
| `common/` | — | 无 | `crash.c` 0（故障路径，见 §五）；`assertion.c` 5（设施自身） | — |

**三条不符的订正**：

1. **`nand_dispatch.c` 的 `default → ASSERT_UNREACHABLE()`**：`ASSERT_UNREACHABLE` 全仓只有
   `assertion.h` 里的**定义**，**零调用**。该文件实际走的是「前置条件 `ASSERT`」这条路
   （ch/ce/lun 界、整批链校验、`sink != NULL`），**没有** `default` 的不可达断言。计划措辞
   与实现是两条不同的路，别按计划去代码里找不存在的 `ASSERT_UNREACHABLE`。
2. **`wear_leveling.c` 从未落地过断言**（实测 0 条）。它被列进 Tier 1「计划内确定项」，但
   §八 之后**没有任何一轮**动过它 —— 直到 **2026-10-17 本轮**才处理，且处理方式是
   **按「恒真 → 删」（§十附判据 1）删掉 6 处守卫**（`wl_should_trigger` 等，调用者 `migrate.c`
   已持实例、且 v0.75 阶段 4 后搬移主体已整体迁入 `migrate.c`）⇒ 它**不是欠账，是口径变更**。
   ⚠️ 但这 6 处与 §二十六「子模块公开入口 ASSERT」的关系见 **§二十八 的三、**（口径分叉的
   **裁决结果**：用户定「保持现状，删掉的不补回」）。
3. **`pending.c` 的「6×`ftl == NULL`」**：原表写的这个计划**已作废**。实际发生过两件事：
   (a) 该文件确实曾有一族 `ASSERT(ftl != NULL)`，但 §二十六（2026-10-15，`adf897e`）把它们
   **当 [REDUND] 删了**（13 处，含 `pending_lookup`/`pending_remove`/`pending_count`/
   `pending_group_*` 等**头文件公开入口**）；(b) 现存 6 条是**下标越界**（[BOUND] 类），
   与「`ftl == NULL`」无关。
4. **`request.c` 的「route 越界」**：`request.c` 里只有两处**调用点**的路由结果前置条件
   （`ASSERT(ftl != NULL)`）；真正的 ch/ce/lun 越界断言在 `ssd_controller_route` 的**实现方**
   `controller.c`。同一件事实现在两个文件里，描述时须分清「谁断言」。

- **全工程 0 处 `ASSUME`**（实测：`ASSUME` 只出现在 `assertion.h` 的定义处，业务代码零使用）。
- 刻意**不**转断言的：~~`journal.c` 整个文件（辨析 C）~~ → **已于 2026-10-17 收窄**，见 §六与
  §二十八；`crash.c`（故障路径）、所有 `fc_command_alloc()` 后的 `cmd == NULL`（资源类，判据 3）、
  `status_out`/`optional-out` 判空（辨析 B / §四 可选参数语义）。

**补扫（辨析 D，同日）**：非 FC 层落地后回扫 FC 层，把"FC 自铸句柄越界"的守卫转 `ASSERT`：

| 文件 | 转换点 |
|---|---|
| `fc_interface.c` | `fc_command_to_id` 两处 NULL（入参/出参前置条件，调用者已判）、`fc_get_cmd_data_ptr` 的 `desc_id >= FC_DESCRIPTOR_POOL_DEPTH`、`fc_command_id_to_command`/`fc_descriptor_id_to_descriptor` 的越界 → NULL |
| `fc.c` | `fc_stripe_release_parity` 的 `parity_id` 越界、完成路径 `completed_id` 的冗余范围检查（`fc_complete_from_nand` 已断言） |

顶层生命周期入口（`fc_command_configure_descriptor`/`fc_submit`/`fc_command_release`，v13 前第一项为 `fc_command_configure_descriptors`，v13~v16 为 `fc_command_attach_cw`）的句柄校验**保留返回码**（有文档契约），理由见 §七。
- 与断言同批：FTL / controller 补齐错误路径日志（层边界与阶段跃迁处 `LOGE/LOGW`，不逐分支）；platform 里并行的 `uart_log_*` 等级栈并入 `common/log.h`（`uart_log_printf`→`LOGE/LOGW/LOGI`，`uart_log_set_level`→`log_set_level`），裸 `uart_log_write` 的终端 I/O 保留。

## 九、第二轮：FC + NAND "全部入口守卫"补扫（2026-10-05）

辨析 D 只处理了 **FC 自铸句柄的越界**，用户随后指出 `fc_command_status_set_cw`、`fc_scheduler_select_chain`、`nand_dispatch_poll` 等**入参 NULL/值域**守卫也还是返回码，要求把 FC + NAND 两层的入口守卫一次性扫干净。判据 1 据此**再次收窄**（见 §一）：边界判定标准从"函数声明在哪个头文件"改为"**值从哪来**"——调用者自己铸出/自己算出/自己持有的值 → 判据 2 转 `ASSERT`，即使该函数声明在 `nand_dispatch.h` / `fc_interface.h` 这类边界头里。

转换范围（两层）：

| 文件 | 转换点 |
|---|---|
| `fc_command.c` | `fc_command_status_init` 的 `status == NULL`、`fc_command_status_set_cw` 的 `status/cw`（两函数已于 2026-10-08 整体删除，此行成历史记录，见 §十七） |
| `fc_scheduler.c` | `fc_scheduler_init`（scheduler/sq/hsq）、`fc_scheduler_select`（scheduler/command_id）、`fc_scheduler_select_chain`（scheduler/chain） |
| `fc_registers.c` | `fc_registers_reset` 的 `registers`（该模块已于 v10 整体删除，此行成历史记录） |
| `fc_raid_engine.c` | `fc_raid_init_ctx` 的 `lun`、`fc_raid_start/accumulate/end` 的 `lun/buffer/total`、`fc_raid_recover` 的 `recovered/buffers/count/failed_idx/length` |
| `fc_random_engine.c` | 三种算法 `fc_random_transform_impl` 的 `data`；XORSHIFT 分支的 `seed == 0U`（种子生成器已保证非零） |
| `fc_crc_engine.c` | `fc_crc_compute`/`fc_crc_verify` 的 `user_data` |
| `fc_ldpc_engine.c` | `fc_ldpc_encode/decode` 的 `data`/`data_length`/`data_length < 8U` |
| `fc_ldpc_qc_engine.c` | `fc_ldpc_qc_decode` 的 `qc`/`data` |
| `fc_data_path.c` | `fc_data_path_encode`/`process_read`/`raid_recover` 的 `command`/`cmd`/`desc_id`/`failed_cw_idx` |
| `fc_interface.c` | `fc_command_configure_descriptors`/`fc_submit`/`fc_command_release` 的 `command == NULL`（保留身份/状态返回码）、`fc_poll_completion` 的 `completion`、`fc_buffer_get_meta` 的 `command`、`fc_write_cmd_data`/`fc_read_cmd_data` 的 `command`/`data`/`desc_id`、`fc_get_cmd_data_ptr` 的 `command`/`desc_id`、`fc_get_resource_stats` 的 `stats` |
| `nand_dispatch.c` | `nand_dispatch_submit` 的 `request`/`ch`/`ce`/`lun`/`cw_count`（保留 `g_nand_dispatch_armed`）、`nand_dispatch_submit_chain` 的 `requests`/`count`/`ch`/`ce`/`lun`、`nand_dispatch_poll` 的 `sink` |
| `nand_fmc.c` | `nand_fmc_init` 的 `config` |
| `nand_mt29f4g08abadawp.c` | `nand_fc_execute`/`nand_fc_start`/`nand_fc_step` 的 `transaction`/`operation`/`request`/`completion`/`active`（保留 `map->uops == NULL` 表缺项）、`nand_fc_read_id` 的 `id`、`nand_fc_set_features` 的 `feature_data` |

**仍然保留返回码**（这些**不是**调用者编程错误）：

| 位置 | 类别 |
|---|---|
| ~~`fc_command_validate` 整个函数~~ **（2026-10-08 撤销：`operation` 全仓都是 FTL 硬编码常量、从不是 host 输入，三处前置违反皆判据 2 → `ASSERT`，见 §十五）** | ~~校验器（输入可能来自 host）~~ |
| `fc_queue.c` 的 ~~`FC_COMMAND_ID_INVALID`~~ **（2026-10-08 撤销：入队 ID 由 FC 自铸，传哨兵是调用者 bug → `ASSERT`，见 §十五）** / `offset >= count` | 哨兵值域 |
| `fc_command_alloc_internal` / `fc_descriptor_alloc_internal` 的池空 NULL | 资源类 |
| ~~`fc_buffer_is_occupied(...) == 0U` / `fc_buffer_release` 未占用~~ **（2026-10-08 撤销：未占用 = 配置错误 → `ASSERT`，见 §十四）** | ~~生命周期状态~~ |
| `g_nand_dispatch_armed == 0U` / `g_initialized == 0U` / `ctx->active == 0U`（~~`g_crc_ready == 0U`~~ 已于 v33 删除，见 `docs/archive/changelog/fc_crc_hal_driver_v33.md`） | 运行时状态 |
| `map->uops == NULL` / `nand_engine` 的 `handler == NULL` / UOP 无 END | 表完整性 |
| `fc_ldpc_decode(..., err_bits)` 的 NULL（~~`fc_command_status_init(..., command)` / `pop_n(ids)`~~ 两函数已于 2026-10-08 整体删除，见 §十七） | 辨析 B 语义分支 |
| desc 链遍历里带 `break` / `return 错误码` 的"链损坏"降级 | 降级语义（非纯 fence） |
| `fc_bch_init` 的 `m`/`t` 值域 | 库构造函数文档化合法范围 |

删除的对应用例见 §四。

## 十、第三轮：per-LUN FIFO 重构后的同类补扫（2026-10-06）

v8（`f8c005c`）把 `fc_poll_fill` 里 ch/ce/lun 的防御性回推改成 `ASSERT` 后，同类的「防御分支」还剩若干——它们的共同点是：**守卫的值与已断言过的值同源，守卫本身不可达；而一旦真的走到，后果是静默降级（活锁 / 丢 completion / 丢链语义）而非停机**。

| 文件 | 转换点 | 原防御的后果 |
|---|---|---|
| `nand_dispatch.c` | `nand_dispatch_lun_free` 的 ch/ce/lun 越界 | 返回 0 → 该 LUN 在 `fc_poll_fill` 里永久挂起（静默活锁） |
| `nand_dispatch.c` | `nand_dispatch_enqueue` 的逐条前置校验：**扩到整批**（原只查 `requests[0]`），并新增「链成员同 ch/ce/lun」 | 链成员异 LUN 时队列按 `requests[0]` 选错 LUN，`cw_count` 越界则在 `completion_multi_plane` 越界写 `cw_status[]` |
| `completion_table.c` | `completion_table_route` 的 ch/ce/lun 越界 | 返回 NULL → 上层释放命令后静默丢弃该 completion |
| `completion_table.c` | `ftl_sync_waiter_handler_with_release` 的 `cmd != NULL`（**v0.75 起该函数已删**，同点由 `ftl_op_dispatch` 的 `cmd != NULL` 承担） | 静默不释放命令 |
| `ftl_op.c` | `ftl_op_dispatch` 的 `cmd == NULL`（v0.75 前在 `ftl_handle_completion`） | 静默吞 completion，命令永不完成也不报错 |
| `fc.c` | `fc_command_configure_descriptors_internal` 的 `plane_count > PLANE_NUM` | 与紧邻的 `ASSERT` 重复；release 下由返回码兜底（本次删除，接受 UB） |
| `fc.c` | `fc_poll_chain_length` 的「链成员同 target」`break` | 静默把一条链截成两条，链语义丢失 |
| `fc.c` | `fc_poll_dispatch_single` / `fc_poll_dispatch_chain` 的 `is_valid` + `command_state == QUEUED` | 命令已出队却未置 ACTIVE/未回推 → **永久泄漏** |

连带删除：`fc.c` 的 `fc_command_id_is_valid()`——两处调用点都进了 `ASSERT`，而 `ASSERT` 在 `NDEBUG` 下整体裁剪为空，静态函数会变成无引用（`-Wunused-function`）；故改为内联 `command_id < FC_COMMAND_POOL_DEPTH` 并删除该包装。

**仍然保留返回码**（本次核实后**不改**）：

| 位置 | 类别 |
|---|---|
| `completion_table_route` 的「`g_route[..] == NULL`（`set_route` 之前）」 | 运行时状态（判据 3）；调用方 `ftl.c` 据此释放命令 |
| `fc_data_path.c` 11 处 `desc/dsc == NULL`、`fc.c` 的 `desc_id < POOL_DEPTH ? … : NULL` | 链遍历降级（§九 已裁定） |
| `fc_interface.c` 的 `buffer_id == FC_BUFFER_ID_INVALID` | 生命周期状态 |
| `fc_interface.c` / `fc.c` 的 `*_get` 查询函数越界哨兵 | 查询 API 契约 |
| `fc_poll_chain_length` 的 `next_id == FC_COMMAND_ID_INVALID` / `chain == FC_CHAIN_END` 两处 `break` | 合法终止（队列尾 / 链尾），非契约违反 |

**未纳入**：`fc_raid_engine.c` 的 `fc_raid_save/restore` 越界守卫——该对函数全仓零调用者（Phase Z 撤销 FMC 数组化后悬空），属死 API，去留待定。

**§十附：同源守卫的两种收法（本次同时出现，可作为下次的模板）**

同一道守卫在多个入口重复时，先判断它是不是**恒真**：

1. **恒真 → 删**。`nand_dispatch_task_load` 里曾有一条「单 plane 有效行的 plane 必须等于 `request->plane`」的断言——但 `cws` 第一维正是从同一个 PAA 值算出的（见 `fc_build_request_data_planes`），断言把同一条表达式算了两遍。更糟的是它**防不住自己注释里写的失败模式**：真错位时数据落到别的行、`cw_count[plane]` 变 0，恰好被它自己的 `cw_count > 0` 前置条件跳过；而那个场景已由生产者侧更强的 `ASSERT(cw_count[plane] == fc_command_descriptor_count(...))` 拦下。**判据：一段守卫若与它要防的对象同源，且其前置条件恰好跳过了真正出错的取值，就是纯噪声，删。**

2. **不恒真但重复 → 收敛到共用漏斗，不要逐个入口复制**。异步提交的放行闩锁（`g_nand_dispatch_armed`，原 `g_nand_device_ready`）曾只在 `nand_dispatch_submit` 里检查、`submit_chain` 漏掉——两个入口各自实现队列逻辑时漏搬的，不是有意取舍。收进两者共用的 `nand_dispatch_enqueue()` 之后结构上不可能再分叉。**判据：守卫属于「这件事能不能做」，而两个入口最终都走同一个函数时，门就装在漏斗上。**

3. **整条失败路径不存在 → 连返回码一起删，不留「表契约」的壳**。`nand_dispatch_req_table` 的 12 个 `req_*` handler 原签名 `uint8_t (*)(task)`，每个都无条件 `return NAND_RESULT_OK;`；调用点仍写着 `if (handler(task) != NAND_RESULT_OK) return REQ_ERROR;`，上层再 `switch` 把 `REQ_ERROR` 映射成 `LUN_ERROR`（丢弃该 LUN 队列 + 返回 `INVALID_ARGUMENT`）。三层守卫守的是同一件不可能的事：handler 的输入在 `nand_dispatch_task_load()` 时已就绪且不变量已断言，构建**没有**失败路径。**判据：当一个函数的返回值被证明恒为某常量时，「保留返回码当作表的扩展点」不是保守而是负担——它让每个调用点都要写一条永远不走的错误分支，并连带虚构出 `REQ_ERROR`/`LUN_ERROR` 两个枚举成员和 poll 里一段死丢弃逻辑。改 `void`、删三层，才是真实契约。**（与判据 2 的差别：判据 2 是守卫**可达**但重复，要收敛位置；本条是守卫**不可达**，要整条删掉。若某天真要引入会失败的 handler，那时再引入一个显式的 `nand_result_t` 出口即可。）

> 命名提示：`g_nand_device_ready` 已改名 `g_nand_dispatch_armed`——旧名与 ONFI 的 "device ready"（R/B# / status bit 6，**per-die 且瞬态**）撞名，而此闩锁是 boot 期一次性的全局属性（"FMC 已由同步初始化路径移交给异步 dispatch"）。per-LUN 的运行时就绪由 `nand_dispatch_lun_free()`（软件容量）+ 引擎 WAIT_READY（现查 R/B#）承担，**不需要**缓存成标志。

## 十一、v13：FC 策略层上移 FTL 后的接口变化（2026-10-07）

v13 把 FC 的"策略层"（按掩码建链 + 数据搬运）上移 FTL，**断言判据本身不变**，但受影响入口的名单要跟着改：

| 变化 | 说明 |
|---|---|
| `fc_command_configure_descriptors` 删除 | 掩码枚举 → `ftl_cmd_configure_descriptors`（`src/ftl/ftl_fc_interface.c`）。它是 FTL 侧入口，按**判据 1**（外部输入：`cmd->param.cw_bitmap` 由 FTL 上层给）处理，越段返回 `FTL_ERROR`（**v23 起改 ASSERT**，见 §十八） |
| `fc_write_cmd_data` / `fc_read_cmd_data` 删除 | → `ftl_cmd_write_data` / `ftl_cmd_read_data`。原 FC 侧 `desc_id` 的 `ASSERT` 一并搬过去 |
| `fc_command_attach_cw` 成为 FC 唯一建链入口 | 沿用"顶层生命周期入口"契约：身份/状态返回码保留，`command == NULL` 转 `ASSERT` |
| 新增 `fc_get_cmd_cw_bitmap` | 从 desc 链重建位图（无 desc → 0）。只服务 RAID 断言；`command` 转 `ASSERT`（v13 时名 `fc_get_cmd_cw_mask`） |
| `fc_command_validate` 删掩码段校验 | 掩码不再是 FC 的输入概念；`param.cw_bitmap` 在 FC 内既无写入者也无读者 |
| `fc_command_resources_rollback` | 唯一调用者移入 `#if CONFIG_FC_RAID` → 标 `__attribute__((unused))`（与 `fc_command_cw_attached` 同法） |

§八 / §九 表格里出现的 `fc_command_configure_descriptors(_internal)`、`fc_write_cmd_data`/`fc_read_cmd_data`
是**当时的落地记录**，保留不改——它们描述的是 v13 之前的形态。

> 详见 `docs/archive/changelog/fc_policy_layer_uplift.md`。

### §十一附 2：v17 建链入口改名（2026-10-08）

v17 把 FC 建链从三层接力收敛为一条：`fc_command_attach_cw` → `_internal` → `_internal`
合并为 **`fc_command_configure_descriptor`（单数）**，实现在 `src/fc/fc_interface.c`。
§十一表里 `fc_command_attach_cw` 那一行是 v13 当时的名字，**契约不变**——它仍是"顶层
生命周期入口"（身份/状态返回码保留，`command == NULL` 转 `ASSERT`；**该身份/状态返回码
契约已于 2026-10-08 撤销，见 §十三**）。**新增的不变量**：
全部校验（指针身份 / 生命周期 / plane 段 / CW 范围）都在出池之前，非法实参不污染池子。
详见 `docs/archive/changelog/fc_build_chain_convergence_v17.md`。

### §十一附：v13.1 删掉一条同源查重断言（2026-10-07）

按 **§十附判据 1**（同源 → 删）：`fc_build_request_data_planes` 单 plane 分支曾有一条
`ASSERT(fc_cw_mask_popcount(request->cw_bitmap) == fc_command_descriptor_count(...))`，
查的是「没有两个 desc 落在同一个 `n`」。但 `request->cw_bitmap` 正是从同一条 desc 链按
`plane*CW_NUM + cw` OR 出来的——与它要防的对象**同源**（同一条表达式算两遍）；而且它只覆盖
单 plane 路径，多 plane 根本不查，覆盖面反而不对称。

真正该留的是 `fc_command_attach_cw_internal` 的 attach 侧
`ASSERT(fc_command_cw_attached(controller, command, plane, cw) == 0U)`：在**污染点**就炸，
定位更准，且覆盖单/多 plane 全部路径。故保留后者，前者连同 `fc_cw_mask_popcount` 删除
（`fc_command_descriptor_count` 不受影响——`fc_command_release` 等仍在用）。
（v17 后该断言随建链入口一并移入 `fc_command_configure_descriptor`，函数名见 §十一附 2。）

> 详见 `docs/archive/changelog/fc_plane_span_consolidation.md`。

### §十二附：v16 建链失败语义与 `fc_command_cw_attached` 判据更正（2026-10-07）

**① `fc_command_attach_cw` 失败不再回滚。** v16 起 desc **出池即挂链**
（`fc_command_configure_descriptor_internal`：分配 desc → 填字段 → 挂链 → 分配 buffer；
v17 后该机制体即 `fc_command_configure_descriptor`，见 §十一附 2），
故「已分配的 desc 全部在命令的数据链上」恒成立。`RESOURCE_EMPTY` 时链上可能已有若干 desc
（含刚挂上、尚无 buffer 的那个），**全部在链上**——调用方沿链释放即完整回收
（`fc_descriptor_chain_release_all` / `fc_command_release`）。FC 层不再有任何就地回滚。

**② `fc_command_resources_rollback` 删除**（第 191 行的记录已作废）。它唯一调用者后面紧跟一次
`fc_command_release_internal_api()`，后者把 desc 链 + buffer + 命令槽位一并做完，前者纯属冗余。
删除后 desc 链释放只剩 `fc_descriptor_chain_release_all_internal` 一份实现。

**③ `fc_command_cw_attached` 保留，但理由更正。** §十一附说它「覆盖单/多 plane 全部路径」——
这是对的；但**「掩码迭代可能产生重复」不成立**：掩码是集合，枚举不会重复置位。
真正可达的重复路径是 `ftl_cmd_setup_ppa_pair`：plane 号取自 `pba_to_plane(pba[i])`
（L2P 查表结果），两个 LBA 可能解析到**同一个 plane** → 重复 `(plane, cw)`。
该断言因此**仍应保留**（在污染点就炸、定位准）。⚠ 但 release 构建下无任何守卫这条
数据驱动的违规——留待后续（可改为返回码而非仅 ASSERT）。

> 详见 `docs/archive/changelog/fc_chain_ownership_v16.md`。

## 十三、v18：建链入口的"配置错误 → ASSERT"定调（2026-10-08）

**背景**：用户指出建链入口 `fc_command_configure_descriptor` 的失败模式应当二分：

- **申请不到**（desc / buffer 池空 → `RESOURCE_EMPTY`）：这是**模仿硬件的行为**，返回码在意料之中 → **保留返回码**（判据 3）。
- **配置出错**（句柄非法 / 生命周期不符 / 坐标越界 / 重复挂同一 n / `row_paa` 的 plane 与实参不符）：**调用者 bug，绝不允许带着错配置发到硬件** → **一律 `ASSERT`**（判据 2）。

**据此撤销 §七 对 `fc_command_configure_descriptor` 的"保留身份/状态返回码"例外**：该入口不再返回 `INVALID_ARGUMENT`，只返回 `OK` / `RESOURCE_EMPTY`。

| 守卫 | 改动前 | 改动后 |
|---|---|---|
| `command != NULL` | `ASSERT` | 不变 |
| 指针身份（`fc_command_to_id`）/ 生命周期（`ALLOCATED`） | `return INVALID_ARGUMENT` | **`ASSERT`** |
| plane 段 / CW 范围 | `return INVALID_ARGUMENT` | **`ASSERT`** |
| 重复 n（`fc_command_cw_attached`） | `ASSERT` | 不变 |
| `paa_to_plane(row_paa) == plane` | `ASSERT` | 不变 |
| desc / buffer 池空 | `return RESOURCE_EMPTY` | 不变（模仿硬件行为） |

**判据来源**：§一"值从哪来"——这些实参全是调用者（FTL / RAID）自己铸出的句柄、自己算出的坐标，属判据 2；§七 的例外只是"文档契约"，而该契约无调用者区分消费（三处调用点都是 `!= FC_STATUS_OK → 沿链释放`），且配置错误本就该在污染点炸而非优雅退出。

**release 注意**：`command_id` 仅用于 `ASSERT`，NDEBUG 下被裁空 → 触发 `-Wunused-variable`，用 `(void)command_id;` 吞掉（与 `fc_command.c` 的 `(void)cw;` 同法）。

**未跟进项**：`fc_submit` 的身份/状态检查按同一判据也应转 `ASSERT`（其"队列满"等资源结果仍保留返回码）；`fc_command_release` 的幂等早退是**运行时分支**（对已 FREE 的命令释放是合法 no-op），不是配置错误，**不转 ASSERT**。
> ⚠️ **2026-10-08 已跟进（v19，见 §十四）**：`fc_submit` 的身份/状态检查已转 `ASSERT`（只留 `OK`/`QUEUE_FULL`）；`fc_command_release` 的**指针身份**检查（栈上 `fc_command_t`）亦转 `ASSERT`（恒 `OK`），**幂等早退仍保留在 internal 版**，未转。

> 详见 `docs/archive/changelog/fc_configure_guard_assert_v18.md`。

## 十四、v19：FC 公开 API 壳的"配置错误 → ASSERT"（2026-10-08）

v18（§十三）只改了 `fc_command_configure_descriptor` 一个入口。按同一判据回扫 `src/fc/` 的**公开 API 壳**，发现五个入口仍把「调用者自铸句柄 / ID 非法」当返回码。本轮（A 档）全部收敛：

| 入口 | 守卫 | 改动前 | 改动后 |
|---|---|---|---|
| `fc_submit` | 指针身份 / 状态 `ALLOCATED` / `validate` / desc 链所有权 | `return INVALID_ARGUMENT` | **四条 `ASSERT`**；只留 `OK` / `QUEUE_FULL`（队列满 = 资源） |
| `fc_command_release` | 指针不在命令池内 | `return INVALID_ARGUMENT` | **`ASSERT`**；恒 `OK`（幂等早退在 internal 版） |
| `fc_buffer_release` | buffer 未占用 | `return INVALID_ARGUMENT` | **`ASSERT`**；恒 `OK` |
| `fc_buffer_id_to_real_buffer` | buffer 未占用（+ 内部恒不返 NULL 的死判） | `return NULL` | **`ASSERT`**；删死判 |
| `fc_buffer_get_base` | buffer 未占用 | `return NULL` | **`ASSERT`** |

**判据**：§一"值从哪来"——这些实参全是调用者自铸/自持的值（`fc_command_alloc` 铸出的指针、它自己设的状态、它自己填的 config、`fc_command_configure_descriptor` 分配的 buffer_id），属判据 2。§七 的"顶层生命周期入口保留返回码"例外、§九 的"buffer 生命周期状态"保留项**据此撤销**——至此 §七 例外表清零。

**release 陷阱**：`fc_submit` / `fc_command_release` 的 `command_id` 仅用于 ASSERT → `(void)command_id;`；`fc_buffer_is_occupied` 转纯 ASSERT 用途后，static 且零调用者 → 加 `__attribute__((unused))`（与 `fc_command_cw_attached` 同法）。

**未跟进（B / C 档，另议）**：

- **B 档**（同类返回码，内部 / 数据路径）：`fc_data_path_generate_stripe_parity` 的 7× `desc==NULL` 死守卫、`fc_bch_engine.c:918/927` 的 `-1` 与"不可纠"哨兵撞车、`fc_command_validate` 的 `desc_id==INVALID`、`fc_queue.c` 的 `command_id==INVALID`。
  > ⚠️ **2026-10-08 已跟进（v20，见 §十五）**：以上 B 档四处全部转 `ASSERT`；并顺带清掉 RAID-off 侧同类死守卫（`fc_data_path_generate_stripe_parity` 7 处、`fc_raid_engine.c` 超累加、`fc.c` stripe_submit 两处 desc 越界）。
- **C 档**（反向病：静默忽略）：`fc_data_path_encode` 的 `if (buffer != NULL)` 静默跳过 META/CRC/加扰/ECC（形态一，**保留**）。
  > ⚠️ **2026-10-08 部分跟进（v21，见 §十六）**：C 档拆成三形态后，**形态三（完全不校验）**已清——`fc.c` 三个 state getter 的越界静默 clamp、`pop_n` 不校验 `count`、`fc_bch_engine.c` / `fc_ldpc_qc_engine.c` 缺入参 ASSERT、`fc_ldpc_engine.c` 的 `data_length` 无上界全部转断言。**形态一 / 二仍保留**。
- **遗留**：本改后 `fc_data_path.c` 里对 `fc_buffer_get_base` 的 `if (buffer != NULL)`（:113/132/153/179 等）**恒真**，成死守卫——归 C 档。

> 详见 `docs/archive/changelog/fc_shell_guard_assert_v19.md`。

## 十五、v20：FC 内部 / 数据路径 / 引擎的"配置错误 → ASSERT"（B 档，2026-10-08）

v18 / v19（§十三 / §十四）清完了 FC 的**公开 API 壳**。本轮（B 档）按同一判据扫**内部函数 / 数据路径 / 引擎**里同类毛病——把「调用者自铸值非法」当返回码。用户定夺三个岔路：RAID-off 死代码一并转、BCH 库的 `-1` 转 ASSERT、`fc_command_validate` 整函数转。

### 默认构建（`CONFIG_FC_RAID=0`）

| 文件:位置 | 守卫 | 改动前 | 改动后 |
|---|---|---|---|
| `fc_command.c` `fc_command_validate` | `command==NULL` / `operation ≥ OP_COUNT` / 数据操作无 desc 链 | 三处 `return INVALID_ARGUMENT` | **三条 `ASSERT`**；恒返回 `OK` |
| `fc_queue.c` `fc_submission_queue_push` | `command_id == FC_COMMAND_ID_INVALID` | `return INVALID_ARGUMENT` | **`ASSERT`**；只留 `OK` / `QUEUE_FULL` |
| `fc_bch_engine.c` `fc_bch_decode` | `8*len > n - ecc_bits` / `!data \|\| !recv_ecc` | 两处 `return -1` | **三条 `ASSERT`** |

**要点**

- **`fc_command_validate`**：撤销 §九 的保留理由——查证 `operation` **全仓都是 FTL 硬编码常量**（`sync_page_prepare` 的实参是 `OP_NORMAL_PROGRAM_PAGE` / `OP_NORMAL_READ_PAGE_WITH_DATA_OUT` / `OP_ERASE_BLOCK` 等），**从不是 host 输入**，三处皆判据 2。函数退化为「恒 OK」的校验钩子；debug 跑内层断言，release 下调用点 `fc_submit` 的 `ASSERT(... == OK)` 整条裁空。
- **`fc_bch_decode`**：`-1` 既是「不可纠」哨兵又是参数错误返回——**撞车**会让参数错误被 `fc_ldpc_engine` 误判为不可纠、CW 静默置 UNC。转 `ASSERT` 后哨兵语义唯一。`:927` 的 `ASSERT(recv_ecc != NULL)` 只在该分支成立（另一分支的 `if (recv_ecc)` 是可选语义，不受影响）。

### RAID-off（`#if CONFIG_FC_RAID`，默认不编译）

| 文件:位置 | 守卫 | 改动后 |
|---|---|---|
| `fc_data_path.c` `fc_data_path_generate_stripe_parity` | 5× `desc/dsc == NULL` + 2× `buf == NULL` → `return INVALID_ARGUMENT` | 逐处 **`ASSERT(x != NULL)`**（函数不再返回 `INVALID_ARGUMENT`） |
| `fc_raid_engine.c` `fc_raid_accumulate` | `processed >= total` → `return NOT_READY` | **`ASSERT(processed < total)`** |
| `fc.c` `fc_stripe_submit` | desc 越界 → `NULL` → `release + goto stripe_error` | **`ASSERT(desc_id < FC_DESCRIPTOR_POOL_DEPTH)`**，直取池指针 |

**判据**：`fc_data_path` 的 NULL 判是**死守卫**——`fc_descriptor_id_to_descriptor`（链内 id 恒在池内）与 `fc_buffer_get_base`（恒已占用）内部已各自 ASSERT，外层判不可能为真；`fc_raid_accumulate` 的 `total` 由 `fc_raid_start` 给定，超累加 = 计数 bug；`fc_stripe_submit` 的 `desc_id` 由链遍历产生，越界即链损坏。全部属判据 2。

**保留不动**：`fc_raid_start` 的 `active != 0` → `NOT_READY`、`fc_raid_accumulate` / `fc_raid_end` 的 `active == 0` → `INVALID_ARGUMENT`——运行时状态，判据 3。

**release 陷阱**：`fc_command_validate` 的 `command` 转纯 ASSERT 用途后 NDEBUG 下变未用形参 → `-Wunused-parameter`；用 `(void)command;` 吞掉（同 `fc_submit` 的 `(void)command_id;`）。`fc_bch_engine.c` 需新增 `#include "common/assertion.h"`。

**验证**：debug / release / RAID=1 debug / RAID=1 release 四档 0 warning 0 error（RAID=1 档用 `make <tier> CFLAGS_EXTRA=-DCONFIG_FC_RAID=1`，并 `nm` 确认 `fc_raid_accumulate` 等符号真的进了目标文件）；`make format-check` passed；`make -k tests` 29 error / 7 warning 与基线逐项一致（全在 `tests/`）；上板未做。

> 详见 `docs/archive/changelog/fc_internal_guard_assert_v20.md`。

## 十六、v21：FC 层 C 档·形态三"缺失的入参/边界校验"（2026-10-08）

§十三~§十五（A / B 档）清的是「**有**守卫但把配置错误当返回码」。C 档是**反向病**——配置错误**既不返回码也不 ASSERT**，静默吞掉。按病灶拆成三形态：

| 形态 | 表现 | 例 |
|---|---|---|
| 一 · 静默跳过 | `if (x != NULL) { ... }` 把该做的活整段略过 | `fc_data_path_encode` 的 META/CRC/加扰/ECC 分支 |
| 二 · 静默降级 | 出问题就 `break` / 回退默认值继续跑 | `if (desc == NULL) break;`、三元 NULL |
| 三 · **完全不校验** | 拿调用者给的 id/size **直接用**，一个守卫都没有 | 本轮 |

本轮只清**形态三**（判据最清楚，且唯一含真内存破坏点）。形态一 / 二另议。

### 改动清单

| 文件:位置 | 缺口 | 改动后 |
|---|---|---|
| `fc_queue.c` `fc_submission_queue_pop_n` | `count` 无上界（`count > queue->count` 静默少弹） | `ASSERT(count <= queue->count)`（该函数 2026-10-08 因零生产调用者被删除，见 §十七） |
| `fc.c` `fc_command_state_get` | `command_id` 越界 → 静默回退 `FC_COMMAND_STATE_FREE` | `ASSERT(command_id < FC_COMMAND_POOL_DEPTH)` |
| `fc.c` `fc_descriptor_owner_get` | `descriptor_id` 越界 → 静默回退 `FC_COMMAND_ID_INVALID` | `ASSERT(descriptor_id < FC_DESCRIPTOR_POOL_DEPTH)` |
| `fc.c` `fc_buffer_state_get` | `buffer_id` 越界 → 静默回退 `0U` | `ASSERT(buffer_id < FC_BUFFER_POOL_DEPTH)` |
| `fc.c` `fc_buffer_id_to_real_buffer_internal` | `buffer_id` 零校验直接索引 `buffer_pool[]` | `ASSERT(buffer_id < FC_BUFFER_POOL_DEPTH)` |
| `fc.c` `fc_buffer_release_internal` | `buffer_id` 零校验直接索引 `buffer_state[]` | `ASSERT(buffer_id < FC_BUFFER_POOL_DEPTH)` |
| `fc_bch_engine.c` `fc_bch_encode` | `bch` / `data` 零校验（`BCH_ECC_WORDS(bch)` 即解引用 `bch`） | 两条 `ASSERT`，**前置**于任何解引用 |
| `fc_ldpc_qc_engine.c` `fc_ldpc_qc_encode` | `qc` / `data_in` / `data_out` 零校验 | 三条 `ASSERT`（`qc` 为 vestigial，纯契约对称） |
| `fc_ldpc_engine.c` `fc_ldpc_encode` / `fc_ldpc_decode` | `data_length` 无上界 | `ASSERT(data_length <= FC_BUFFER_DATA_SIZE)` |

**对称补齐**：`fc_bch_decode` 也补 `ASSERT(bch != NULL)`（原先只查 `data` / `recv_ecc`，而 `bch` 在 `BCH_ECC_WORDS(bch)` 处早已被解引用——断言写在解引用之后等于没有，故**必须置于函数体首条语句**）。

### 判据

全部判据 2（"值从哪来"）——实参都是调用者自铸/池派生的值，不是 host 输入：

- 三个 getter 的 6 个调用点**全在范围内**：3 处 `fc_command_to_id` 派生（`fc_interface.c:162/242`）、3 处 `index < DEPTH` 循环上界（`:503/510/517`）、1 处先过 `fc_buffer_id_is_valid`（`:62`）。clamp 分支是**死代码**；且 `FC_COMMAND_STATE_FREE` 回退尤其误导——把"没有这个槽位"说成"槽位空闲可用"。同文件兄弟 `fc_command_id_to_command_internal` / `fc_descriptor_id_to_descriptor_internal` 正是这么写的。
- 两个 buffer 内部函数的调用者都已先校验：公共壳 `ASSERT(fc_buffer_is_occupied(...))`、`fc_get_cmd_data_ptr` 与 `fc_descriptor_chain_release_head_internal` 都先判 `!= FC_BUFFER_ID_INVALID`。
- `pop_n` 的 `count` 是调用者算的值；生产调用者为**零**（调度器走逐条 `fc_submission_queue_pop`），唯一调用者是测试常量 `3U`（队列 `count == 5`）——不受影响。

### 唯一的内存破坏点

`fc_ldpc_*` 的 `data_length` 无上界是 C 档里**唯一真会写坏内存**的：encode 的 LDPC 分支 `uint8_t tmp[FC_BUFFER_POOL_CW_SIZE]`(528) + `memcpy(tmp, data, data_length + 8)` → `data_length > 520` 即**栈溢出**；BCH 分支 `memcpy(data + data_length, ecc_buf, 8U)` → **写穿 CW buffer**；decode BCH 分支 `recv_ecc = data + data_length` 后读 8B → 同样越界。

⚠ **`fc_bch_decode` 内新加的 `8*len <= n - ecc_bits` 挡不住它**：BCH(m=13, t=4) 下 `n - ecc_bits = 8139`，`8 × 521 = 4168 < 8139` 照样通过。所以必须在 `fc_ldpc_*` 入口加 `<= 520`。

### 辨析（不动）

- ~~`pop_n(ids == NULL)`~~（函数 2026-10-08 因零生产调用者删除，见 §十七） / `fc_bch_encode(..., ecc == NULL)` / `fc_ldpc_decode(..., err_bits == NULL)`：**有意的可选参数语义**（辨析 B），不是调用者错误，保留。
- `fc_ldpc_qc_encode` 的 `g_initialized` 静默返回：运行时状态（判据 3），且 void 返回无从给码。
- `fc_ldpc_qc_encode` 的 `qc` 参数 vestigial（编码只用文件内 `g_col_weight` / `g_col_to_check`）本身另议——本轮只补断言做契约对称，**不动其"未用"**。

### 连带发现（顺带修）

`fc_ldpc_engine.c` **未 include `fc.h`**——而 LDPC 分支的 `uint8_t tmp[FC_BUFFER_POOL_CW_SIZE]`（该分支默认不编译）与新增的 `FC_BUFFER_DATA_SIZE` 同出 `fc.h`。补 `#include "fc.h"` 后 LDPC 分支（`-DFC_ECC_ALGO=FC_ECC_ALGO_LDPC`）首次可编译。

### release 陷阱

- `fc_ldpc_qc_encode` 的 `qc` 在 NDEBUG 下断言裁空 → `-Wunused-parameter`；用 `(void)qc;` 吞掉。
- 三个 getter / 两个 buffer 内部函数的参数在 **return 表达式**里使用；`pop_n` 的 `count` 在 for 循环里用——均不产生 unused。

### 验证

四档（debug / release / RAID=1 debug / RAID=1 release）**0 warning 0 error**；`-DFC_ECC_ALGO=FC_ECC_ALGO_LDPC` 补 include 后 0 error（余 1 条 pre-existing `fc_ecc_init_bch defined but not used`，属该实验分支既有、非本轮）；`make -k tests` 29 error / 7 warning 与基线逐项一致（全在 `tests/`）；`make format-check` passed；上板未做。

> 详见 `docs/archive/changelog/fc_c_tier_missing_guard_v21.md`。

---

## 十七、用例清理："名不副实"的用例与其代码（2026-10-08）

本节收两条同源判据——都在回答「**一条用例什么时候不该再留着**」。判据 5 是**被测代码**失效
（没人调用），判据 6 是**用例自己**失效（构造不出真实前置条件）。

### 判据 5（Level 1）：被测函数失去生产调用者

**背景**：走读 Level 1 单测时发现，v13~v21 的接口/策略收敛让**一批被测代码失去了生产
调用者**，但用例还在——于是测试不再"验证代码"，而是"养着代码"。据此补一条判据：

> **判据 5：用例的被测函数在 `src/` 里已无生产调用者时，函数与用例一并处置**
> （删函数 + 删头声明 + 删用例），而不是保留用例替它续命。

### 删除清单

| 函数 | 何时失去调用者 | 处置 |
|---|---|---|
| `fc_command_status_init` | 2026-09-20（`83fe922`：CQ 完成项改由 `fc_complete_from_nand()` 内联组装，此前由 `a4013dd` 的 `fc.c:532` 调用） | 函数 + 头声明 + 2 条用例删 |
| `fc_command_status_set_cw` | 同上（原 `fc.c:537`） | 函数 + 头声明 + 1 条用例删 |
| `fc_submission_queue_pop_n` | 从未有过（调度器一直走逐条 `fc_submission_queue_pop`） | 函数 + 头声明 + 1 条用例删 |

另删 1 条**零增量覆盖**的用例 `test_queue_empty_full`——其断言集是 `test_queue_peek_empty` ∪
`test_queue_overflow` 的真子集（后者还多测了"pop 后恢复可用"）。

Level 1 用例数 16 → **11**；三个子套件至此**零 `setup`**（`setup_fc_suite` 随 `status_init`
两条用例删除，测试文件不再 include `fc/fc.h`）。

### 与 §十六 的关系

§十六 清的是"配置错误既不返回码也不 ASSERT"（C 档形态三）。本节清的是**另一种反向病**：
守卫 / API 本身没问题，但**没人调用了**——它既不破坏不变量，也不产生运行时开销，
只是让"用例数"这个指标失真，并让后来者误以为该函数是活的。

同理可推：§十四 / §十五 / §十六 里那些"转成 ASSERT 后只被测试调用"的守卫，
以及本节删掉的函数，都是同一模式的产物。

### 删前必查：`set_cw` 与生产内联版的语义分叉（**已闭合**）

`fc_command_status_set_cw` 记录的是「**任一**错误位（NAND_FAIL / TIMEOUT / CRC / UNC /
MATA / RAID）的首个 `desc_id`」；而生产内联版（`fc.c` 的 `fc_complete_from_nand`，:772-776）
**只认 UNC**：

```c
if ((found_unc == 0U) && (d->status == FC_CW_STATUS_UNC)) { completion.desc_id = did; ... }
```

两者语义分叉——删掉前者等于删掉"正确语义"的唯一书面记录。本次按用户决定照删。

**2026-10-08 后续闭合**：判据取「多数方」——`fc_command_status_t.desc_id` 的头文件契约写的是
"first-failing desc on error"，被删的 `set_cw` 也是全集口径，只有内联循环是少数派，
故**改内联版去对齐契约**，而不是改文档：

- `include/fc/fc_command.h` 新增 `FC_CW_STATUS_ERROR_MASK`（六个错误位的规范并集），
  把原先只存在于 `set_cw` 里的那份"失败位集合"固化成唯一一份命名定义；
- `fc.c` 的完成项聚合循环改用该掩码判失败（`found_unc` → `found_fail`）；
- 顺带把 `desc_id` 的字段注释补全：初值来自 NAND 透传的 `failed_descriptor_id`
  （**实为 flat CW 编号**，与 desc id 不同空间），page read 才被覆盖为 desc id，
  FTL 侧目前无读者。

### 判据 6（Level 2）：用例构造不出被测函数的真实前置条件

> **判据 6：当一条用例**构造不出**被测函数的真实前置条件时，退化的用例应**删**，
> 而不是留着"续命"——尤其当它能通过只是靠若干巧合的乘积时。**

**背景**：走读 Level 2 时发现 RAID 组 5 条用例里有 4 条属于此类。`fc_data_path_raid_recover`
**结构上只收集单条命令的 desc 链**（`fc_data_path.c:591`），而 parity CW 在**另一条**命令的
链上（`fc.c:888/924-933`）——跨命令 buffer 收集是 `include/fc/fc_raid_engine.h:22` 明写的
未办事项。也就是说这个函数在本层**根本拿不到 parity**。

那 4 条（`encode_raid_on` / `raid_recover_first` / `_mid` / `_last`）之所以长期通过，靠的是
三个巧合相乘：

1. 退化 fixture 让加扰变成空操作（`paa=0` → keystream 除 byte0 外全 0，见判据 6 的连带发现）；
2. 4 个 CW 的 pattern 恰好异或为 0（`0x10^0x11^0x12^0x13 == 0`），于是"没有 parity 的 XOR"
   也能还原被擦 CW；
3. BCH 是线性的，ECC 奇偶区跟着一起异或也不会破坏码字有效性。

**处置**：删 4 条，只留 `raid_recover_no_random`（random=OFF → 巧合 1 不参与，退化 stripe 在
无加扰下成立）与真 stripe 的 `generate_stripe_parity`（parity 命令真实参与 XOR）。

**与判据 5 的差别**：判据 5 删的是"**被测代码**已死"的用例；判据 6 删的是"**用例**自己
构造不出场景"的用例——被测函数活得好好的，只是本层喂不出它要的输入。后者更隐蔽：它
不但不失败，还会把「三个巧合的乘积」伪装成「一条通过的用例」，从而在有人修好 fixture 时
突然炸掉（本次即是如此：改 `cmd->paa` 后 random=ON 的 4 条必挂）。

**连带发现（同一次走读）**：Level 2 的 fixture 从不设 `cmd->paa`，命令结构体被 `{0}` 清零
→ `paa=0` → 单 plane 建链分支（`fc_interface.c:189`）把 `desc->paa` 落成 `cw` 本身 →
BASIC 种子退化成 `seed == cw` → `{1,1,2,3}`，LFSR 低 16 位取这几个小值后 **keystream 除
byte0 外全 0**。于是所有 `random=ON` 用例的加扰都是空转，"encode→decode 还原原文"照样通过。
**修 fixture 之外还补了自证断言**（"encode 后 USER 区 ≠ 明文"）——否则下次 fixture 再退化，
仍然无人察觉。

### 验证

四档（debug / release / RAID=1 debug / RAID=1 release）**0 warning 0 error**；
`make tests` 0 error 0 warning、链接通过（**193956 B**，改前 197468 B）；`make format-check` passed；上板未做。

判据 6 的 Level 2 改动：`make -k tests` 0 error 0 warning、链接 **197344 B**；RAID=1 档
（`CFLAGS_EXTRA="-DMUNIT_ENABLE_TIMING -DCONFIG_FC_RAID=1"`）0/0、**204328 B**，`nm` 确认
`test_raid_recover_no_random` / `test_generate_stripe_parity` 在、被删的 4 个测试符号消失；
`make format-check` passed；上板未做。

## 十八、v23：FTL 建链掩码越段 → ASSERT（§十三 判据的 FTL 侧漏网，2026-10-08）

> ⚠ **已被 v27 部分取代**：`cw_mask` 改名 `cw_bitmap`、`ftl_cmd_resolve_cw_mask` 改名
> `ftl_cmd_resolve_cw_bitmap`，且「`0` = 段内全部 CW」这条重载**已取消**（`0` 现在是空集）。
> 本节保留 v23 当时的记述；当前语义见 **§二十一**。

**背景**：v23 把 `param.cw_mask == 0` 重定义为「本命令 plane 段内的全部 CW」，并让两个建链入口
（`ftl_cmd_configure_descriptors` / `ftl_cmd_setup_ppa_pair`）共用同一个 static
`ftl_cmd_resolve_cw_mask()`。这次合并暴露出一处**同判据的漏网**：

| 层 | 入口 | v22 前对"掩码越出 plane 段"的处置 |
|---|---|---|
| FC（机制） | `fc_command_configure_descriptor` | v18 起 `ASSERT`（§十三） |
| FTL（策略） | `ftl_cmd_configure_descriptors` | **`return FTL_ERROR`** ← 漏网 |

**处置**：FTL 侧改为 `ASSERT((mask & ~allowed) == 0U)`。依据就是 §十三 那一行
「plane 段 / CW 范围：`return INVALID_ARGUMENT` → **`ASSERT`**」——同判据、同一类实参
（调用者自己铸出的掩码），不该因为"落在另一个文件"就退化成返回码。

**为什么这处检查不能省**（比 FC 侧那条更关键）：FTL 的枚举循环只走
`plane_first..plane_last` × `cw 0..CW_NUM-1`，**段外的高位 bit 根本不会被枚举到**——
不检查就是**静默忽略**，而机制层的越段 ASSERT 只在"逐条调用时"生效，同样拦不到段外那些
从未被枚举的位。所以 `ftl_cmd_resolve_cw_mask()` 是这类越段的**唯一检测点**。

**保留返回码的部分**：`FTL_ERROR` 仍保留在 `cmd == NULL`、`pba == NULL` 与 desc/buffer
池空（`RESOURCE_EMPTY` 语义）三条路径上——按判据 3，池空是"模仿硬件行为"，在意料之中。

**连带**：`ftl_cmd_setup_ppa_pair` 此前**完全不读** `param.cw_mask`（只认 `config.cw`），
本轮改为读掩码；其越段检查同样由 `ftl_cmd_resolve_cw_mask` 承担，未另加"plane 落在段内"
的断言——`PLANE_NUM = 2` 时该断言恒真，属 §十七 判据 5 要清的死代码。

**release 注意**：`allowed` 同时参与 `mask == 0` 的返回路径，不依赖 `ASSERT`，
NDEBUG 下无 `-Wunused-variable` 风险（与 §十三 的 `command_id` 情形不同）。

**验证**：debug / release / tests / RAID=1 四档 0 warning 0 error；`make -k tests` 链接
197388 B（改前 197344 B，+44 B）；`nm` 确认 `ftl_cmd_resolve_cw_mask` 是 **local（`t`）**，
未引入新的外部链接符号；`make format-check` passed；上板未做。

> 详见 `docs/archive/changelog/ftl_cw_mask_single_knob_v23.md`。

---

## 十九、v24：地址布局统一 + 加扰器重做（2026-10-08）

v24 把 PBA 从"与 PAA 共用位序"改为"与 LBA 逐位同构"，并把加扰种子从物理地址改为逻辑地址。
**本轮没有新增断言判据**——涉及的断言全部落在既有条目上，这里只记两则"沿用既有判据"的处置。

### 沿用 §十三：`fc_command_configure_descriptor` 的 row 校验

v24 把该入口的第三个形参由 `nand_paa_t row_paa` 改为 `nand_pba_t row_pba`，原有那一条

```c
ASSERT((uint8_t)pba_to_plane(row_pba) == plane);
```

**只是换了访问器，判据与位置不变**——"调用者自己铸出的实参"（§十三 判据 2）依旧是 `ASSERT`，
不因为地址空间改名而回到返回码。这是 §十三 的直接延续，不另立条目。

### 沿用 §十七 判据 5：两个地址比较函数删除

| 函数 | 问题 | 处置 |
|---|---|---|
| `paa_same_block(a, b)` | 直接 `a & PAA_BLOCK_MASK` 而**没有先右移**——本来就是错的（block 字段不在低位） | **删除** |
| `pba_same_block(a, b)` | 同源同病 | **删除** |
| `fc_fill_cw_meta_paa()` | 写 `md->paa` 到 `buffer[0..3]`，被 `fc_data_path_encode` 的 Stage 0 用 `desc->meta` 覆盖同一 4 字节 ⇒ **零可观测效果**；内容已错（PAA ≠ META）；遍历终止条件还是 v17 前的池深式 | **删除** |

判据 5 的原文是"被测函数失去生产调用者"。这里是它的**生产侧版本**：函数**从来没有**生产调用者
（grep 全仓零引用），且实现本身就错——**零调用者 + 实现错误 = 不需要先"修好再观察"，直接删**。
（对比 §十六 的"形态三"：那是**缺**校验，要补；这是**多**一个错误函数，要删。）

`fc_fill_cw_meta_paa` 是判据 5 的一个**变体**：它有调用者（`fc_poll_dispatch_single`），
但**效果被下游整段覆盖**——即"有调用者、无效果"。判据要扩充一句：

> **有调用者但效果被下游完全覆盖的函数，与无调用者等价**——尤其当它的**内容**也已被后续
> 重构改错时（本例：v24 把 `buffer[0..3]` 的语义从 PAA 改成 META，函数没跟）。
> 判断方法不是看调用图，而是问"删掉它，有没有任何一条路径的输出会变"。

删除后 `fc_poll_dispatch_single` 的 page write 分支与 `fc_poll_dispatch_chain` **完全一致**，
原注释里那条"行为不对称（既有，勿统一）"随之消失——**这本身就是判据的旁证**：
不对称若只是冗余步骤造成的，删掉冗余就对称了，不需要"保留差异"。

### 新则：哨兵值不得取合法值

v24 引入 `FC_META_UNKNOWN`（`include/fc/fc_command.h`），用来表达"本命令不知道 LBA"
（GC / 掉电重建路径——它们**正是要从页内 META 读回** LBA）。

```c
#define FC_META_UNKNOWN (0xFFFFFFFFU)   /* 不是 0 */
```

**为什么不能用 0**：`command->meta` 在 v24 起是**扇区 LBA**，而 0 是**合法值**
（页 0 / plane 0 / CW 0，板上第一页就是这个地址）。用 0 当哨兵会把"未指定"与"就是第 0 扇区"
混为一谈——GC 路径本该跳过"读错页"校验，却会因为 `command->meta == 0` 与 `read_meta` 不等而
**误报 `MATA_ERROR`**（这正是修前的既有行为，GC 只看 `NAND_FAIL` 所以一直无害）。

**推广**：本工程凡引入"无值 / 未知"哨兵，一律取该类型**不可能出现的值**，
并在定义处写清"为什么不是 0"。同类先例：`FC_DESCRIPTOR_ID_INVALID = 0xFFFF`、
`FTL_BLOCK_NO_PLANE`（v12）。反例参照 `request->cw_bitmap` 的 0 = 合法空集——
那是**合法值**，所以它不能当哨兵，只能靠文档说清（v23 §十八 的辨析）。

### 验证

debug / release / tests / RAID=1 四档 0 warning 0 error（149920 / 84988 / 197088 / 204252 B）；
`make format-check` passed；`nm` 确认 `pba_to_paa` / `lba_to_meta` / `paa_to_pba` 无全局符号；
六条加扰判据 + 四条布局判据离线自证全过（见 changelog）；**上板未验**。

> 详见 `docs/archive/changelog/fc_scrambler_lba_seed_v24.md`。

---

## 二十、v25：同一个头文件里的两种调用约定 = 隐患（2026-10-08）

**新判据**：

> **同一个头文件里，同一个操作存在两种调用约定（同义不同序），本身就是隐患，应收敛为一种。**
> 判据不是"形参太多"——形参多只是症状；病根是**同一组字段出现了两套顺序**，
> 而编译器**不检查这两套顺序是否一致**。

**首个案例**：`include/nand/nand_config.h` 里

```c
static inline nand_pba_t pba_make(uint32_t ch, uint32_t ce, uint32_t page, uint32_t plane,
                                  uint32_t block, uint32_t lun, uint32_t cw);
static inline nand_pba_t pba_from_addr(const nand_addr_t *addr);   /* 体内就是上一行 */
```

`pba_from_addr` 的函数体**逐字**就是 `pba_make(addr->ch, addr->ce, addr->page, addr->plane,
addr->block, addr->lun, addr->cw)`——**它们是同一个函数的两种调用约定**，而不是两个函数。
之所以要有 `pba_from_addr`，唯一原因就是 `nand_addr_t` 的字段序（`ch, ce, lun, block, plane,
page, cw`）与 `paa_make` / `pba_make` 的形参序（`…, page, plane, block, lun, …`）**恰好相反**。

**为什么这是隐患而不是风格问题**：七个**同型小整数**，写反了

- **依然编译通过**（全是 `uint32_t` 形参，无 `-Wconversion` 之外的任何提示）；
- **依然落在合法值域内**（`page < 64`、`plane < 2`、`block < 2048` 全都成立）；
- ⇒ **静默算出错地址**，直到上板读到错页才发现。

对照 §十三/§十四：那两条判据解决的是"配置错误该 ASSERT 还是返回码"；本条解决的是
**让配置错误根本写不出来**——`nand_addr_t` 具名字段（`.page = p, .plane = q`）把"顺序写反"
从**静默错地址**变成**编译错误**。这比任何 ASSERT 都早一步。

**推广到同类**：判断方法不是数形参，而是问——**这组形参里有没有两个以上同类型、可互换的字段？**
有，且另一个入口用的是另一种顺序 ⇒ 收敛。本轮 `paa_make` / `pba_make` 删除，13 处生产调用点
迁到 `paa_from_addr` / `pba_from_addr`；`pba_to_paa` / `paa_to_pba` / `addr_to_cmd` 三处原本
**为跨这两套顺序而存在的拆包再打包**随之消失。

**保留项（对 §十七 判据 5 的显式例外，勿"清理"）**：本轮 3 个零调用者函数
`paa_is_valid_addr`（原 `paa_is_valid`）/ `paa_to_pba` / `pba_to_nand_row` **按用户要求保留**。
注意 `paa_is_valid` 的**形参序本身就是**被消灭的那一套，所以它即便保留也必须换成
`nand_addr_t` 版本——**"保留函数"不等于"保留隐患"**。
（v0.73 更新：`pba_to_nand_row` 已随 `nand_addr_t` 退出 FTL 而删除；`paa_to_pba` 保留，
作为 `ftl_fc_interface.h` `ftl_cmd_pba()` 用的边界转换器。）

**已跟进（v0.73）**：`nand_addr_t` 从 FTL 层**整体退出**——FTL 的地址一律 `nand_pba_t`，
具名字段访问改经**位域视图** `nand_pba_fields_t`（`pba_fields()` / `pba_pack()` /
`pba_make()`，定义见 `include/ftl/ftl_config.h §三.2`）。视图是 4 字节位域、位序与 PBA
一致，可强转。⚠ **v25 否决它的那两条硬约束依然成立**，本轮是**逐条给了应对**而非推翻：
① 本构建 `LUN_ADDR_WIDTH == 0`，而 C11 6.7.2.1p4 规定 0 宽位域不得有声明符（具名
`lun : 0` 直接编译不过）⇒ 视图里 `lun` 成员用 `#if (LUN_ADDR_WIDTH > 0U)` 隐去，那一处
`#if` 收敛在 `pba_make()` 内（`PBA_LUN_MASK` 为 0 ⇒ lun 自然被忽略）；
② 位域布局 implementation-defined 且 `offsetof` 非法 ⇒ **没有任何 `_Static_assert`**
能证明它与 `PBA_*_SHIFT` 一致（复合字面量的 union 读不是 C11 整型常量表达式，GCC 实测报
`expression in static assertion is not constant`）⇒ 退化为 **boot 期一次性自检**
`pba_fields_layout_matches()`（`ftl_init` 里 `ASSERT`）：逐字段置单比特、双关读出，证明
`*(nand_pba_t *)&fields == pba_pack(fields)`。**值语义路径（`pba_pack` / `pba_fields`）
走 `PBA_*_SHIFT` 宏，与位域布局无关**——自检只保证"强转可用"这一条性质。

**验证**：debug / release / RAID=1 三档 0 warning 0 error（150028 / 84944 / 154372 B）；
`make format-check` passed；`nm` 确认 `paa_from_addr` / `pba_from_addr` 只有 local（`t`）符号、
`paa_make` / `pba_make` 全局消失；**离线等价性自证**（本轮唯一强判据）——旧实现抄成参照，
1,048,576 组 × 7 条判据 + 2,097,152 组 × 1 条判据，**0 mismatch**；**上板未验**。
（`make tests` 本轮未跑——测试源由用户同步重构中。）

> 详见 `docs/archive/changelog/nand_config_addr_ctor_v25.md`。

---

## 二十一、v27：取消「0 = 全部」重载后的唯一拦网（2026-10-09）

**新判据**：

> **字段一旦叫 `bitmap`，就必须逐位字面：置 1 的位 = 选中，全 0 = 一个都没选。**
> 把 `0` 重载成"未指定 → 展开为默认集合"是**借用了另一个字段的语义**（v7~v22 借
> `config.cw`，v23~v26 借"plane 段"），代价是同一层内出现两个 0 方向相反的口径，
> 且"生产者忘了填"与"有意选中空集"不可区分。

**改动**：`cw_mask` → `cw_bitmap`（FC 侧 `fc_command_t.param.cw_bitmap`、NAND 侧
`nand_dispatch_request_t.cw_bitmap`），v23~v26 的「`0` = 本命令 plane 段内全部 CW」取消。

**由此产生的 ASSERT 及它的边界**（本节的核心）：

```c
/* src/ftl/ftl_fc_interface.c :: ftl_cmd_resolve_cw_bitmap */
ASSERT(bitmap != 0U);              /* ← 新增：0 = 空集 = 生产者忘了填 */
ASSERT((bitmap & ~allowed) == 0U); /* ← §十八 既有条目 */
```

第一条是 §十三 判据 2 的直接应用（"调用者自己铸出的实参"→ `ASSERT`），但**必须记下它的失效面**：

| | debug / tests | release（NDEBUG） |
|---|---|---|
| 越段位图（第二条） | 当场 ASSERT | **静默**（段外的位不被枚举） |
| 全 0 位图（第一条） | 当场 ASSERT | **静默空操作**（建出空链，命令照发） |

即：**忘填位图的代价在 release 下是无声的**——这不同于 §十三~§二十 各条（那些是"配置错误
不再返回码"，错误本身仍会以某种形式显形）。本轮接受了这个代价，理由与备选方案：

- 备选一：`config.reserved` 里放一个 "bitmap_valid" 标志 ⇒ **引入第二个旋钮**，正是 §十八
  批判的形态（两个入口各认一个旋钮）。
- 备选二：保留 `0` = 全部，另加一个"显式空集"的哨兵值 ⇒ 又造了一个不可达状态。
- 结论：**字面语义 + 显式整段写法**（`fc_command_plane_span_cw_bitmap()`）比"隐式默认值"
  更少歧义，代价是忘填时 release 无守卫。**整段入口自己填位图**（`pba_to_cmd` /
`ftl_cmd_setup_ppa_pair_cw`）就是为了把"忘填"从调用者手里拿走。

**易错点（写代码时注意）**：`fc_command_plane_span_cw_bitmap(cmd)` 读的是 `cmd->pba` 与
`config.multi_plane`，**必须在两者都写好之后**调用。段算小了不会触发越段 ASSERT
（少访问几个 CW 全部落在 `allowed` 内）⇒ 属静默错，无守卫。

**连带消除**：v23~v26 期间 FC 侧 `param.cw_bitmap`（0 = 全部）与 NAND 侧
`request->cw_bitmap`（0 = 合法空集）**方向相反**，是结构性的、只能靠文档说清。v27 起
SQ 请求集 / CQ 失败集 / NAND 请求集**三处同口径**（0 = 空集），该条说明作废。

**保留项**：NAND 层 `request->cw_bitmap == 0` 仍是**合法空集**（无 desc 的退化命令，
`cw_buffers[]` 由位图自身决定填不填）——这条契约**不变**，且现在与 FC 侧同向。

**验证**：`clang-format-15` + `make format-check` passed（101 文件）。**未编译**——本轮处于
测试代码重构期（Level 2/3 尚引用旧夹具符号），`make tests` / `make debug` / `make release`
与上板验证均留到调用点迁移完成之后。

> 详见 `docs/archive/changelog/fc_cw_bitmap_literal_v27.md`。

---

## §二十二 CW 集合退出命令结构体（v28）——「派生量不进结构体」的判据应用

**日期**：2026-10-09　**前置**：§十三（建链入口配置错误 → ASSERT）、§十八（FTL 侧漏网）、
§二十一（字面位图）

### 判据

> **一个字段若「写入后只在同一个函数的同一次调用里被读一次」，它就不该住在共享结构体里。**

`cmd->param.cw_bitmap`（v27 时的形态）正是这种字段：唯一写入者是 FTL 的建链调用点，
唯一读者是 `ftl_cmd_resolve_cw_bitmap()`——就在建链那一刻把它取出来用掉，之后再无回读。
它是**建链的输入**，不是命令的属性。把它留在命令里，代价是：

1. **多一处可被误读的真值**（`fc_command_validate` 要不要校验它？RAID 断言该读字段还是读链？——
   两个问题都因此产生过）。
2. **命令结构体对"访问哪些 CW"这件事撒了两个谎**：字段值（意图）与 desc 链（事实）可以不一致，
   而事实由链决定。

⇒ **改作形参** `ftl_cmd_configure_descriptors(cmd, cw_bitmap)`。字段删除。

### 同判据的第二处：CQ 的失败位图

`completion.param.cw_bitmap`（失败 CW 集）是**纯派生量**：它就是「desc 链上
`status & (UNC|CRC_ERROR)` 的那些 desc 的 `n`」。上层要它，遍历链 OR 一次就有
（链保留到 `fc_command_release()`）。FC 代为聚合只省了上层一个循环，却多维护了一份
与链可能不一致的快照 ⇒ **删除聚合，不删 `param` 字段**（GET_FEATURES 的寄存器回读仍需要它）。

### 第三处：擦除的「假 CW 位」（v7~v27）

FC 的 ERASE 分支给请求位图硬置一个 CW0 位，唯一目的是让 `completion_all_cws` 的
`cw_next(bitmap, 0)` 循环至少跑一次、把 `cw_status[]` 写上——否则 FC 的非 page read 聚合分支
（全表 OR）读不到擦除成败。这是**为 completion 服务的假数据**：位图本该只表达"访问哪些 CW"。

⇒ 移到 NAND 侧：`completion_all_cws` 补一条 `bitmap == 0 ⇒ 写 cw_status[0]` 分支
（与 `completion_single` 早已有的一条**同口径**）。**空集语义统一**，FC 不再伪造。

### 新增/扩大的失效面（写代码时注意）

| 面 | 变化 | 守卫 |
|---|---|---|
| `ftl_cmd_configure_descriptors` 的位图 | 由命令字段改为**形参** | 两条 ASSERT 照旧（§二十一）：`bitmap != 0`、位图落在段内 |
| `request->cw_bitmap == 0` 的消费点 | 擦除加入（此前只有 RESET/STATUS/块锁等） | `nand_dispatch_task_load` 对空掩码一格都不搬 `cw_buffers[]`（残留旧值）；**`req_erase` 只用 `plane_paa[]` 不读 `cw_buffers[]`** ⇒ 安全。**新增无数据 op 时须复核这一点** |
| `completion_all_cws` 空集分支 | 新增 | 与 `completion_single` 同口径，非新语义 |

⚠ **"空掩码 = 无 buffer 可读"这条契约现在依赖消费点真的不读**——`task_load` 的
`cw_buffers[]` 不清零（slot 复用），掩码外的格是残留指针。这条已写进 `nand_dispatch.c` 的
`task_load` 注释。

### 验证

`clang-format-15` + `make format-check` passed（101 文件）。**未编译**——测试代码重构期
（Level 2/3 尚引用旧夹具符号），`make tests` / `make debug` / `make release` 与上板验证
留到调用点迁移完成之后。

> 详见 `docs/archive/changelog/fc_cw_bitmap_param_v28.md`。

## 二十三、v29：步骤级用例的形态选择——参数化 vs 迭代（2026-10-09）

本轮**不新增生产侧断言**（数据路径步骤函数化是**纯提取**，行为逐字不变），但确立了两条**用例形态**
判据——它们回答「同一条用例，多个场景该怎么摆」与「边界该怎么钉」。判据编号接 §十七 的判据 5/6。

### 判据 7：不同分支/期望 → 参数化；同一断言、不同输入 → 迭代

> **判据 7：一组场景若**期望不同**（走不同分支、断言不同的值），用**参数化**（munit 的
> `MunitTest.parameters`，每组一次独立 `setup`/`tear_down`，运行器在测试名下打印 `case=…`）；
> 若**断言相同、只是输入不同**，用**迭代**（`--iterations N`，同一段代码多抽几次）。
> 把后者硬做成参数化会造出一堆重复项；把前者做成迭代则会让「某次恰好没抽到该分支」变成**覆盖漏洞**。

正例与反例（`tests/unit/test_fc_level2.c` 的三条步骤级用例）：

| 用例 | 形态 | 理由 |
|---|---|---|
| `crc_step_detect` | **参数化** 4 case | 4 个 case 的**期望不同**——`user-1`/`crc-field-1` 必检出，`meta-1`/`parity-1` **必不检出**，是四个分支 |
| `ecc_step` | **参数化** 11 case | 1..4 bit 错「精确纠正」与 ≥5 bit 的 BOUND 是**两种期望** |
| `scramble_whiten` | 内部 `kind = 0..2` 循环 | 三种输入（全 0x00 / 全 0xFF / PAA 重复）走**同一组断言**，只是输入不同 |

**只有 `case` 一个维度**，不是 k×region 的笛卡尔积——后者会造出一堆重复/无意义的组合（§2.2 已记）。

**追加应用（同轮）**：`encode` / `process_read_ok` 两组原本各是 3 条独立用例
（`encode_random_off` / `_random_on` / `_single_cw`；`process_read_ok_random_on` / `_random_off` /
`_single_cw`），按判据 7 各收成**一条参数化用例**：

| 用例 | 形态 | 理由 |
|---|---|---|
| `encode` | **参数化** 3 case | 三条断言集相同（`cw_count` / META / CRC / parity），只有两列**期望**不同：cw 数、USER 区是否仍是明文（`random-on-4cw` 期望**已非明文**，另两条期望仍为明文） |
| `process_read_ok` | **参数化** 3 case | 三条的断言**全在 `l2_buffer_round_trip()` 内**，入参恰好就是 helper 的三个形参 |
| `cw_bitmap` | **参数化** 3 case | 三条共用「建链 → 收链 → 断言 desc 数 → 逐条断言 (plane, cw)」骨架，差异全在**段宽 / `addr.plane` / 位图**三个字段；期望链以显式表给出 |

> `cw_bitmap` 一组带一处**代价**：原 `plane1-only` / `span-multi-plane` 把期望写成**规则**
> （`plane[i]==1 && cw[i]==i`、`plane[i]==(i<CW_NUM)?0:1 && cw[i]==i%CW_NUM`），规则本身是
> 「plane-major」「cw 循环」两条不变量的代码表达；改显式表后不变量只剩注释在扛。仍合的理由是
> 三行的 (段宽, `addr.plane`, 位图) 组合摆在一张表里、缺口一眼可见，且失败诊断不差。

**反例（刻意不并）**：`process_read_unc` / `masked_chain` / `mata_error` / `crc_error` / `ldpc_raw`
各自断言**不同的可观测量**（返回码 / 不同的 `status` 位 / 不同的哨兵区）——合表须给每行挂 setup
回调，收益不足。`mata_error` 与 `crc_error` 断言形状相同（逐 CW 查一个 `status` 位）但 setup
机制不同（改 `cmd->meta` vs 翻 `config.crc` + 预置哨兵），同样不并。

### 判据 7 的再应用（v1.22，2026-10-11）：Level 3 的 block/plane 参数化**本身就不合判据 7**

Level 3 的 6 条地址相关用例原先挂了 `block`（`100`/`104`）× `plane`（`0`/`1`）× `sample`
（`edges`/`spread`/`middle`/`dense`）的参数表。按判据 7 复查：

- **`block` / `plane` 是反例**：换 `100` → `104` 或 `0` → `1`，六条用例的**断言集完全相同**，
  只是输入不同——正是判据 7 警告的「把迭代硬做成参数化，造出一堆重复项」。参数化在这里的唯一
  实际收益是**能枚举**（不加 `--single` 时全组合展开），而它把默认一轮从 9 条撑到 35 条。
- **`sample` 是边界情形**：四档的**探针集不同**（2/5/3/8 页 × 2/2/1/4 CW），勉强算「不同分支」；
  但它同样是**有限枚举**，钉不住「任意页 × 任意 CW」。

**改法**（见 `docs/archive/changelog/fc_ut_level3_seed_random_v40.md`）：六条改**用例体内抽样**，参数表删除；
覆盖改由 `--iterations N` 给（同一断言、不同输入 ⇒ 迭代，**正是判据 7 对「同一断言」那一支的规定**）。
随机性用 `l3_rand(test, tag) = munit_rand_uint32() ^ hash(用例名) ^ hash(用途标签)` —— **必须自带
salt**，理由见 `fc_nand_ut.md` §5.4（munit 每 `exec` 前 reseed 回 `runner->seed`，导出的 `munit_rand_*`
不带盐 ⇒ 无盐时各用例的第一次抽样完全相同）。

⚠ **代价（如实记录）**：默认一轮 Level 3 由 35 条降为 **9 条**，全套 77 → **51**；且每条只抽一次，
单轮 plane 只覆盖 5 次抽取（`block_multi_plane` 不吃 plane）⇒ 有约 3% 的概率某一轮**一个 plane 都没抽到**
（`C(5,0)·2^-5 ≈ 3.1%`）。要覆盖两者就加 `--iterations`。这是「不再枚举」换来的，判据 7 选择迭代时
本就接受这个取舍。

### 判据 8：负例钉的是**覆盖边界**，不是「没坏就行」

`crc_step_detect` 的两条负例（`meta-1` / `parity-1` 必须**不**被 CRC 检出）来自
`fc_data_path_crc_check` 的结构：存的 CRC 读自 `cw[4..8)`，算的 CRC 吃 `cw[8..520)`
（`FC_CRC_USER_SIZE = 512`）。META 与 parity **结构性**在覆盖之外。

> **判据 8：当被测函数的**覆盖范围**小于它操作的缓冲区时，边界**两侧**都要有用例。
> 只验「范围内的坏了会被抓到」是不够的——一个「把整块 buffer 都拿去算」的**错误实现**照样能过。
> 负例（范围外必须**不**被抓到）才把覆盖范围钉死。

同一条也适用于 ECC：`ecc_step` 的 `parity-1` / `parity-4` 场景验的是「校验位坏了会被记入
`ecc_bits` 但**不**被纠正」（`fc_ldpc_decode` 只翻 `bit_pos < data_length * 8` 的位）——同样是边界两侧。

### 连带：munit 的三处移植分歧（v29 修复，对照上游 nemequ/munit）

| 位置 | 症状 | 修复 |
|---|---|---|
| `munit_parameters_get()` 缺失 | 参数化用例只能自己手写 ~8 行扫描 `params[]` 的循环 | 补回（上游逐字同名同实现） |
| `run_test_wild` 递归传 `p` | **≥2 个参数枚举**时在同一参数上无限递归 | 传 `next` |
| `munit_test_runner_exec` 的 `runner->iterations` | 套件级 `iterations` 字段失效（自赋值） | `runner->suite->iterations` |

**只有第一条会挡住写参数化用例**——runner 早已把 `params[]` 填好，缺的只是按名取值的函数。
后两条是潜伏 bug：全仓此前零个用例带参数（`tests/unit` 有 48 处 `(void)params;`、零个消费者），
所以没人踩到。第三条**当前观测不到差别**——所有套件的 `iterations` 都是 1，而 `iterations == 0` 时
`do-while` 也正好只跑一遍。

⚠ `--param name value` 的值**不做合法性校验**，原样塞进 `params[]` ⇒ 参数化用例必须自己挡表外的
名字（`munit_assert_true(case_find(...) != NULL)`），否则打错场景名会**静默跑一个不存在的场景**。

### 验证

`clang-format-15` + `make format-check` passed（101 文件）；`-fsyntax-only`（makefile 全量告警集）
对 `test_fc_level2.c` 与 `munit.c` 均 0 warning 0 error。**未编译**——`make tests` 因
`test_fc_level3.c` 仍引用旧夹具符号而编不过（v27/v28/v29 共同的遗留中间态），上板未验。

> 详见 `docs/archive/changelog/fc_data_path_step_primitives_v29.md`。

---

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
> 可查询的**深度计数**（`ftl_op_in_dispatch()`），并在被禁函数的入口写
> `ASSERT(ftl_op_in_dispatch() == 0)`。**约定从此可测**——任何新调用点一旦越界，debug 档
> 当场停机，而不是留到上板后靠"偶发丢命令"去猜。**

**落地形态**：`src/ftl/ftl_op.c` 里 `g_op_depth` 深度计数（单线程顺序主循环 ⇒ 普通变量，
无需原子），`ftl_op_dispatch` 用 `g_op_depth++` / `g_op_depth--` 包住服务函数调用
（含它内部的嵌套派发，故计数天然覆盖嵌套）。`l2p_sync_pump()`（`l2p_cache.c`，四个 `*_sync`
逃生口的自旋泵）第一行就是 `ASSERT(ftl_op_in_dispatch() == 0)`。

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
`ASSERT(0)` / 实例归属 `&ftl->xxx == ctx` / 簿记计数 / 运行时上下文 `ftl_op_in_dispatch()`）。

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
| `bbt.c` | 7 | 7 | 5 转 + 2 删（`bbt_scan_factory` / `bbt_load` 同文件调用者） |
| `ftl_fc_interface.c` | 7 | 6 | 全为**出参**（FTL↔FC 边界）；`cmd == NULL`（池空，判据 3）**保留** |
| `migrate.c` | 4 | 5 | `mig_begin_*`/`migrate_handle` 转；`mig_start`（static）删 |
| `power_failure_recovery.c` | 4 | 9 | 3 个**对外入口**转；6 个同文件内部路径函数删 |
| `bg_scheduler.c` | 3 | 2 | 任务表项判空 |
| `ftl.c` | 3 | 3 | `ftl_init` + 本轮补的 `ftl_flush` / `ftl_trim`（C5：唯一调用点已 `ASSERT`） |
| `gc.c` | 3 | 1 | — |
| `checkpoint.c` | 2 | 4 | `cp_begin` / `checkpoint_journal_watermark` 转；另 2 处见三、 |
| `wear_leveling.c` | 0 | 6 | **纯删**（恒真）——见三、的分叉 |
| `wear.c` | 0 | 2 | **纯删**（恒真） |
| `ftl_op.c` | 0 | 1 | **纯删**（同文件调用者） |
| **合计** | **110** | **93** | 其中**约 15 处是纯删**（未落断言） |

> ⚠ **2026-10-19 补记**（§3.12 审查，见 `known_risks.md §2J`）：上表是**当轮批次记录**，
> 不随之后改动回写。当前实际状态：
> `bg_scheduler.c` = **5 处** `ASSERT`（原 3 + `bg_scheduler_init` 新增的 `table != NULL`
> 与 `count <= BG_TASK_COUNT` **容量门**——容量门必须在拷贝循环**之前**，否则越界已经发生）；
> 接线（13 个 `bg_a_*` 适配器 / 任务表 `s_bg_tasks` / 档位表 `g_bg_level_cfg` / Phase 3 入口）
> 已从 `ftl.c` 抽到新文件 `src/ftl/bg_wiring.c`，其 `ASSERT` = `ftl_bg_init` 1 处 +
> `ftl_bg_service` 1 处。

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
| 介质读回数据 | `journal.c` 的 `count > JOURNAL_ENTRIES_PER_PAGE` | 判据 3 + H29（§六） |
| 运行时/依赖态 | `l2p_cache.c` 的 `cache->md_dir == NULL \|\| cache->alloc == NULL`（Region 加载中）、`ftl.c` 的 `platform->time_us == NULL`、`gc_check_triggers` 的 `alloc == NULL`（0 无触发） | 判据 3 |
| **UT 钉住的文档化契约** | `checkpoint_check_trigger(NULL)`/`checkpoint_is_emergency(NULL)`/`gc_check_triggers(NULL)`/`rr_check_predictive(NULL,…)`/`rr_get_health(NULL)`、`bbt.c` 两处 | **判据 4**（`tests/unit/test_ftl.c` 逐条钉住 ⇒ 转 `ASSERT` 会当场打红 UT） |
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
   （`gc_check_triggers(NULL) → 0` = "没有 allocator ⇒ 无触发"），后者的默认值是**连贯的**。
3. **调用点普查：全仓零 NULL**（逐条核实 `src/` 11 个 .c + `tests/unit/test_ftl.c`）。
   实参只有两种形态：`&<…>->blocks[<索引>]`，或紧邻使用前赋值的局部/形参（其右值同样是
   `&…blocks[<索引>]`）。**根因**：`ftl_allocator_t.blocks` 绑定到
   `ftl_context_t.tables.blocks` 这个**内嵌数组**（`ftl_tables_t.blocks[FTL_BLOCK_TOTAL]`，
   非指针）⇒ 取元素地址**结构性恒非空**。
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

另注：`ftl_job.h` 的 `ftl_job_now_ms`（`platform == NULL || platform->time_us == NULL`）**不在本族** ——
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

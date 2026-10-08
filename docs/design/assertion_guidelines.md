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
   **跨层入口一并适用**：`nand_dispatch_submit/submit_chain/poll`、`nand_fmc_init/backend_get`、`nand_fc_execute/start/step/read_id/set_features`、`fc_submit`、`fc_command_configure_descriptor`、`fc_command_release`、`fc_data_path_*`、`fc_scheduler_*`、`fc_command_status_*`、`fc_ldpc_*`、`fc_crc_*`、`fc_random_*`、`fc_raid_*`、`ftl_cmd_write/read_data`（v13 由 FC 上移）、`fc_get_cmd_data_ptr`、`fc_get_cmd_cw_mask`、`fc_get_resource_stats` 的**指针/索引参数**。唯一的调用者都是层内代码（FTL/controller/fc.c），且已持有有效值。

3. **资源类结果保留返回码**
   池耗尽（`FC_STATUS_RESOURCE_EMPTY`）、队列满背压（`QUEUE_FULL` / `IO_ERROR`）、`fc_command_alloc()` 返回 `NULL`、`mem_alloc` 返回 `NULL`、NAND 报告的错误——都是**预期内**的运行结果，不是不变量破坏，必须返回给上层。
   同理**运行时状态**保留返回码：`g_nand_dispatch_armed == 0U`（异步路径未放行）、`g_crc_ready == 0U`、`g_initialized == 0U`（惰性初始化未完成）、`ctx->active == 0U`、`fc_buffer_is_occupied(...) == 0U`（生命周期）、`map->uops == NULL`（操作表缺项）——这些是"此刻能不能做"，不是"调用者传错"。

4. **单测固化为契约的守卫保留返回码**
   这是判据 2 的例外，但**只在测试是被测函数的正当调用者时成立**。`tests/` 里拿**文档化合法输入**调内部函数验证行为（如 `test_rebuild.c` 的 `allocator_remove_from_free(..., 越界)` 是文档化的 no-op、`ftl_is_metadata_block(NULL,…)` 是文档化分类）的，按判据 1 保留返回码。
   反之，**只有测试会走"传非法参数"这条分支**（生产调用者从不传）的，按判据 2 转断言并**删掉对应用例**——见 §四。

## 二、两条辨析（最容易搞错的地方）

**辨析 A：`ptr == NULL` 要看指针来源。**
来自 `fc_command_alloc()` / `mem_alloc` 的是**资源**（判据 3，保留）；来自函数入参的才是**前置条件**（判据 2，转断言）。FTL 里满屏的 `cmd == NULL` 绝大多数是前者。

**辨析 B：语义分支不是前置条件。**
用 `NULL` 表达"这个可选参数没给"的，是有意设计的语义，不是调用者的编程错误。例：`ds_deque.c` 的 `cp == NULL`（浅拷贝选择器）、`ftl_is_metadata_block(md_dir == NULL, …)`（固定区间照常分类）、`fc_submission_queue_pop_n(ids == NULL)`（仅消耗队列）、`fc_ldpc_decode(..., err_bits == NULL)`（不取纠正位数）、`fc_command_status_init(status, command == NULL)`（无命令来源时填哨兵值）。

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

**结论：`journal.c` 整个文件不转断言。**

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

## 八、落地记录（2026-10-05）

设施 `fd2a0d9` 建好后，先在 FC 层落地（§24，见 `fc_nand_design.md`），本轮推广到**全部非 FC 层**：

| 层 | Tier 1（计划内确定项） | Tier 2（逐点核对调用链后转） |
|---|---|---|
| `nand/` | `nand_engine.c`（UOP type）、`nand_dispatch_tables.c`（5×`cw_buffers`，索引后由 `[0]` 改为 `[task->plane]`，计数不变）、`nand_fmc.c`（DATA_IN/OUT）、`nand_dispatch.c`（default → `ASSERT_UNREACHABLE()`） | `nand_engine_start`/`step`（头文件已文档化前置条件） |
| `ftl/` | `allocator.c`、`power_failure_recovery.c`、`wear_leveling.c` | `pending.c`（6×`ftl == NULL`）、`read_recovery.c`（`rr_record_ecc`/`rr_predictive_scan`/`rr_retire_block`/`rr_recover_read`） |
| `controller/` | `controller.c`、`request.c`（route 越界、ftl 槽位恒非空） | — |
| `common/` | 无（`crash.c` 属故障路径，见 §五） | — |

- **全工程 0 处 `ASSUME`**。
- 刻意**不**转断言的：`journal.c` 整个文件（辨析 C）、`crash.c`（故障路径）、所有 `fc_command_alloc()` 后的 `cmd == NULL`（资源类）、`status_out`/`optional-out` 判空（辨析 B）。

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
| `fc_command.c` | `fc_command_status_init` 的 `status == NULL`、`fc_command_status_set_cw` 的 `status/cw` |
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
| `g_nand_dispatch_armed == 0U` / `g_crc_ready == 0U` / `g_initialized == 0U` / `ctx->active == 0U` | 运行时状态 |
| `map->uops == NULL` / `nand_engine` 的 `handler == NULL` / UOP 无 END | 表完整性 |
| `fc_ldpc_decode(..., err_bits)` / `fc_command_status_init(..., command)` / `pop_n(ids)` 的 NULL | 辨析 B 语义分支 |
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
| `completion_table.c` | `ftl_sync_waiter_handler_with_release` 的 `cmd != NULL` | 静默不释放命令 |
| `ftl.c` | `ftl_handle_completion` 的 `cmd == NULL` | 静默吞 completion，命令永不完成也不报错 |
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
| `fc_command_configure_descriptors` 删除 | 掩码枚举 → `ftl_cmd_configure_descriptors`（`src/ftl/ftl_fc_interface.c`）。它是 FTL 侧入口，按**判据 1**（外部输入：`cmd->param.cw_mask` 由 FTL 上层给）处理，越段返回 `FTL_ERROR` |
| `fc_write_cmd_data` / `fc_read_cmd_data` 删除 | → `ftl_cmd_write_data` / `ftl_cmd_read_data`。原 FC 侧 `desc_id` 的 `ASSERT` 一并搬过去 |
| `fc_command_attach_cw` 成为 FC 唯一建链入口 | 沿用"顶层生命周期入口"契约：身份/状态返回码保留，`command == NULL` 转 `ASSERT` |
| 新增 `fc_get_cmd_cw_mask` | 从 desc 链重建掩码（无 desc → 0）。只服务 RAID 断言；`command` 转 `ASSERT` |
| `fc_command_validate` 删掩码段校验 | 掩码不再是 FC 的输入概念；`param.cw_mask` 在 FC 内既无写入者也无读者 |
| `fc_command_resources_rollback` | 唯一调用者移入 `#if CONFIG_FC_RAID` → 标 `__attribute__((unused))`（与 `fc_command_cw_attached` 同法） |

§八 / §九 表格里出现的 `fc_command_configure_descriptors(_internal)`、`fc_write_cmd_data`/`fc_read_cmd_data`
是**当时的落地记录**，保留不改——它们描述的是 v13 之前的形态。

> 详见 `docs/changelog/fc_policy_layer_uplift.md`。

### §十一附 2：v17 建链入口改名（2026-10-08）

v17 把 FC 建链从三层接力收敛为一条：`fc_command_attach_cw` → `_internal` → `_internal`
合并为 **`fc_command_configure_descriptor`（单数）**，实现在 `src/fc/fc_interface.c`。
§十一表里 `fc_command_attach_cw` 那一行是 v13 当时的名字，**契约不变**——它仍是"顶层
生命周期入口"（身份/状态返回码保留，`command == NULL` 转 `ASSERT`；**该身份/状态返回码
契约已于 2026-10-08 撤销，见 §十三**）。**新增的不变量**：
全部校验（指针身份 / 生命周期 / plane 段 / CW 范围）都在出池之前，非法实参不污染池子。
详见 `docs/changelog/fc_build_chain_convergence_v17.md`。

### §十一附：v13.1 删掉一条同源查重断言（2026-10-07）

按 **§十附判据 1**（同源 → 删）：`fc_build_request_data_planes` 单 plane 分支曾有一条
`ASSERT(fc_cw_mask_popcount(request->cw_mask) == fc_command_descriptor_count(...))`，
查的是「没有两个 desc 落在同一个 `n`」。但 `request->cw_mask` 正是从同一条 desc 链按
`plane*CW_NUM + cw` OR 出来的——与它要防的对象**同源**（同一条表达式算两遍）；而且它只覆盖
单 plane 路径，多 plane 根本不查，覆盖面反而不对称。

真正该留的是 `fc_command_attach_cw_internal` 的 attach 侧
`ASSERT(fc_command_cw_attached(controller, command, plane, cw) == 0U)`：在**污染点**就炸，
定位更准，且覆盖单/多 plane 全部路径。故保留后者，前者连同 `fc_cw_mask_popcount` 删除
（`fc_command_descriptor_count` 不受影响——`fc_command_release` 等仍在用）。
（v17 后该断言随建链入口一并移入 `fc_command_configure_descriptor`，函数名见 §十一附 2。）

> 详见 `docs/changelog/fc_plane_span_consolidation.md`。

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

> 详见 `docs/changelog/fc_chain_ownership_v16.md`。

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

> 详见 `docs/changelog/fc_configure_guard_assert_v18.md`。

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

> 详见 `docs/changelog/fc_shell_guard_assert_v19.md`。

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

> 详见 `docs/changelog/fc_internal_guard_assert_v20.md`。

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
| `fc_queue.c` `fc_submission_queue_pop_n` | `count` 无上界（`count > queue->count` 静默少弹） | `ASSERT(count <= queue->count)` |
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

- `pop_n(ids == NULL)` / `fc_bch_encode(..., ecc == NULL)` / `fc_ldpc_decode(..., err_bits == NULL)`：**有意的可选参数语义**（辨析 B），不是调用者错误，保留。
- `fc_ldpc_qc_encode` 的 `g_initialized` 静默返回：运行时状态（判据 3），且 void 返回无从给码。
- `fc_ldpc_qc_encode` 的 `qc` 参数 vestigial（编码只用文件内 `g_col_weight` / `g_col_to_check`）本身另议——本轮只补断言做契约对称，**不动其"未用"**。

### 连带发现（顺带修）

`fc_ldpc_engine.c` **未 include `fc.h`**——而 LDPC 分支的 `uint8_t tmp[FC_BUFFER_POOL_CW_SIZE]`（该分支默认不编译）与新增的 `FC_BUFFER_DATA_SIZE` 同出 `fc.h`。补 `#include "fc.h"` 后 LDPC 分支（`-DFC_ECC_ALGO=FC_ECC_ALGO_LDPC`）首次可编译。

### release 陷阱

- `fc_ldpc_qc_encode` 的 `qc` 在 NDEBUG 下断言裁空 → `-Wunused-parameter`；用 `(void)qc;` 吞掉。
- 三个 getter / 两个 buffer 内部函数的参数在 **return 表达式**里使用；`pop_n` 的 `count` 在 for 循环里用——均不产生 unused。

### 验证

四档（debug / release / RAID=1 debug / RAID=1 release）**0 warning 0 error**；`-DFC_ECC_ALGO=FC_ECC_ALGO_LDPC` 补 include 后 0 error（余 1 条 pre-existing `fc_ecc_init_bch defined but not used`，属该实验分支既有、非本轮）；`make -k tests` 29 error / 7 warning 与基线逐项一致（全在 `tests/`）；`make format-check` passed；上板未做。

> 详见 `docs/changelog/fc_c_tier_missing_guard_v21.md`。

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
   **跨层入口一并适用**：`nand_dispatch_submit/submit_chain/poll`、`nand_fmc_init/backend_get`、`nand_fc_execute/start/step/read_id/set_features`、`fc_submit`、`fc_command_configure_descriptors`、`fc_command_release`、`fc_data_path_*`、`fc_scheduler_*`、`fc_command_status_*`、`fc_ldpc_*`、`fc_crc_*`、`fc_random_*`、`fc_raid_*`、`fc_write/read_cmd_data`、`fc_get_cmd_data_ptr`、`fc_get_resource_stats` 的**指针/索引参数**。唯一的调用者都是层内代码（FTL/controller/fc.c），且已持有有效值。

3. **资源类结果保留返回码**
   池耗尽（`FC_STATUS_RESOURCE_EMPTY`）、队列满背压（`QUEUE_FULL` / `IO_ERROR`）、`fc_command_alloc()` 返回 `NULL`、`mem_alloc` 返回 `NULL`、NAND 报告的错误——都是**预期内**的运行结果，不是不变量破坏，必须返回给上层。
   同理**运行时状态**保留返回码：`g_nand_device_ready == 0U`（设备未就绪）、`g_crc_ready == 0U`、`g_initialized == 0U`（惰性初始化未完成）、`ctx->active == 0U`、`fc_buffer_is_occupied(...) == 0U`（生命周期）、`map->uops == NULL`（操作表缺项）——这些是"此刻能不能做"，不是"调用者传错"。

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
- `fc_descriptor_id_to_descriptor(descriptor_id)` 的 `descriptor_id` 只来自命令的 desc 链（`fc_command_configure_descriptors` 铸出）。

调用者**不可能**传入越界值——越界即不变量破坏。因此这类"句柄越界 → 返回 NULL/错误码"的守卫按判据 2 转 `ASSERT`，并同步修掉头文件里"越界返回 NULL"的契约描述。同理，FC 内部对句柄的越界检查（`fc_stripe_release_parity` 的 `parity_id`、完成路径的 `completed_id`、desc 链遍历里的 `desc_id`）一律转 `ASSERT`。

**保留返回码的部分**：`fc_command_configure_descriptors` / `fc_submit` / `fc_command_release` 这三个**顶层生命周期入口**里，`fc_command_to_id()` 返回 0（指针不在本池）、状态非 `ALLOCATED`、desc 链未配置这类检查仍返回 `FC_STATUS_INVALID_ARGUMENT`——它们校验的是"这个指针是不是本池的命令 + 生命周期阶段"（指针身份 + 状态），是有文档契约的入口校验；`fc_command_release` 的 `command_id >= FC_COMMAND_POOL_DEPTH` 因此也保留（它在调用 `fc_command_id_to_command()` 之前先挡一道，避免后者断言）。**但同一入口里的 `command == NULL` 是纯参数前置条件，转 `ASSERT`**（第二轮补扫，见 §九）。

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
| `fc_interface.c` | `fc_command_to_id` 两处 NULL（静态 helper，调用者已判）、`fc_write_cmd_data`/`fc_read_cmd_data`/`fc_get_cmd_data_ptr` 的 `desc_id >= FC_DESCRIPTOR_POOL_DEPTH`、`fc_command_id_to_command`/`fc_descriptor_id_to_descriptor` 的越界 → NULL |
| `fc.c` | `fc_stripe_release_parity` 的 `parity_id` 越界、完成路径 `completed_id` 的冗余范围检查（`fc_complete_from_nand` 已断言） |

顶层生命周期入口（`fc_command_configure_descriptors`/`fc_submit`/`fc_command_release`）的句柄校验**保留返回码**（有文档契约），理由见 §七。
- 与断言同批：FTL / controller 补齐错误路径日志（层边界与阶段跃迁处 `LOGE/LOGW`，不逐分支）；platform 里并行的 `uart_log_*` 等级栈并入 `common/log.h`（`uart_log_printf`→`LOGE/LOGW/LOGI`，`uart_log_set_level`→`log_set_level`），裸 `uart_log_write` 的终端 I/O 保留。

## 九、第二轮：FC + NAND "全部入口守卫"补扫（2026-10-05）

辨析 D 只处理了 **FC 自铸句柄的越界**，用户随后指出 `fc_command_status_set_cw`、`fc_scheduler_select_chain`、`nand_dispatch_poll` 等**入参 NULL/值域**守卫也还是返回码，要求把 FC + NAND 两层的入口守卫一次性扫干净。判据 1 据此**再次收窄**（见 §一）：边界判定标准从"函数声明在哪个头文件"改为"**值从哪来**"——调用者自己铸出/自己算出/自己持有的值 → 判据 2 转 `ASSERT`，即使该函数声明在 `nand_dispatch.h` / `fc_interface.h` 这类边界头里。

转换范围（两层）：

| 文件 | 转换点 |
|---|---|
| `fc_command.c` | `fc_command_status_init` 的 `status == NULL`、`fc_command_status_set_cw` 的 `status/cw` |
| `fc_scheduler.c` | `fc_scheduler_init`（scheduler/sq/hsq）、`fc_scheduler_select`（scheduler/command_id）、`fc_scheduler_select_chain`（scheduler/chain） |
| `fc_registers.c` | `fc_registers_reset` 的 `registers` |
| `fc_raid_engine.c` | `fc_raid_init_ctx` 的 `lun`、`fc_raid_start/accumulate/end` 的 `lun/buffer/total`、`fc_raid_recover` 的 `recovered/buffers/count/failed_idx/length` |
| `fc_random_engine.c` | 三种算法 `fc_random_transform_impl` 的 `data`；XORSHIFT 分支的 `seed == 0U`（种子生成器已保证非零） |
| `fc_crc_engine.c` | `fc_crc_compute`/`fc_crc_verify` 的 `user_data` |
| `fc_ldpc_engine.c` | `fc_ldpc_encode/decode` 的 `data`/`data_length`/`data_length < 8U` |
| `fc_ldpc_qc_engine.c` | `fc_ldpc_qc_decode` 的 `qc`/`data` |
| `fc_data_path.c` | `fc_data_path_encode`/`process_read`/`raid_recover` 的 `command`/`cmd`/`desc_id`/`failed_cw_idx` |
| `fc_interface.c` | `fc_command_configure_descriptors`/`fc_submit`/`fc_command_release` 的 `command == NULL`（保留身份/状态返回码）、`fc_poll_completion` 的 `completion`、`fc_buffer_get_meta` 的 `command`、`fc_write_cmd_data`/`fc_read_cmd_data` 的 `command`/`data`/`desc_id`、`fc_get_cmd_data_ptr` 的 `command`/`desc_id`、`fc_get_resource_stats` 的 `stats` |
| `nand_dispatch.c` | `nand_dispatch_submit` 的 `request`/`ch`/`ce`/`lun`/`cw_count`（保留 `g_nand_device_ready`）、`nand_dispatch_submit_chain` 的 `requests`/`count`/`ch`/`ce`/`lun`、`nand_dispatch_poll` 的 `completion` |
| `nand_fmc.c` | `nand_fmc_init` 的 `config` |
| `nand_mt29f4g08abadawp.c` | `nand_fc_execute`/`nand_fc_start`/`nand_fc_step` 的 `transaction`/`operation`/`request`/`completion`/`active`（保留 `map->uops == NULL` 表缺项）、`nand_fc_read_id` 的 `id`、`nand_fc_set_features` 的 `feature_data` |

**仍然保留返回码**（这些**不是**调用者编程错误）：

| 位置 | 类别 |
|---|---|
| `fc_command_validate` 整个函数 | 校验器（输入可能来自 host） |
| `fc_queue.c` 的 `FC_COMMAND_ID_INVALID` / `offset >= count` | 哨兵值域 |
| `fc_command_alloc_internal` / `fc_descriptor_alloc_internal` 的池空 NULL | 资源类 |
| `fc_buffer_is_occupied(...) == 0U` / `fc_buffer_release` 未占用 | 生命周期状态 |
| `g_nand_device_ready == 0U` / `g_crc_ready == 0U` / `g_initialized == 0U` / `ctx->active == 0U` | 运行时状态 |
| `map->uops == NULL` / `nand_engine` 的 `handler == NULL` / UOP 无 END | 表完整性 |
| `fc_ldpc_decode(..., err_bits)` / `fc_command_status_init(..., command)` / `pop_n(ids)` 的 NULL | 辨析 B 语义分支 |
| desc 链遍历里带 `break` / `return 错误码` 的"链损坏"降级 | 降级语义（非纯 fence） |
| `fc_bch_init` 的 `m`/`t` 值域 | 库构造函数文档化合法范围 |

删除的对应用例见 §四。

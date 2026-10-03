# 计划：FTL 未落地项全量落地（A/B/C 档）

## Context
体检确认 `ftl_design.md`/`ftl_revision_history.md`/记忆点声称"FTL 全部模块完成"，但源码存在未落地缺口（已逐一 `grep` 验证零消费）：
- **A 档**：`FTL_OP_PERCENT(10)`/`FTL_SPARE_POOL_SIZE(50)` 只定义不消费；`spare_pool_count` 仅 init 赋 0；`bbt_spare_pool_refill` 是 `return FTL_ERROR` 桩；`FTL_BLOCK_STATE_PRE_COND/EMERGENCY` 全库从未 set/clr。容量语义当前 = 全好块进 free pool（OP 0%）。
- **B 档**：`checkpoint_journal_watermark` 分母用 `PAGES_PER_BLOCK=64`（单块），容量实为双块 128 页；`journal.checkpoint_needed/emergency` 只有 init 置 0、只有 append 置 1，永不复位；`ftl_service` 三级模式 = `submitted==0→IDLE/≤2→NORMAL/else→BUSY`（"本轮有无提交 NAND"代理）；`rr_record_ecc`/`read_count++` 只在 host 读 completion（ftl.c:107-119），GC/WL 搬移读 `gc_read_page_with_lba`（gc.c:188/wear_leveling.c:236）不记录；`read_count/program_count` 只写不读。
- **C 档**：`src/controller/state_machine.c`（1 行 #warning）、`scheduler.c`（1 行 #warning）、`src/host/nvme/`（5 桩 #warning）——全部编译进来；`SSD_REQUEST_FLUSH` 在 controller.c:151/request.c:230 返回 NOT_SUPPORTED；`FC_COMMAND_MODULE_HOST_IO` 定义但零使用。（**v0.47 已全部落地/删除**：state_machine/scheduler 真实现 + NVMe 删除 + FLUSH 落地，见 C 档节）

用户已确认：OP/Spare **容量不动**（512MiB 不变，只做 GC 预留 + 坏块替换池）。**C 档实现时按学习主控定位重规划**（见下方 C 档节）：NVMe host 砍掉，改 UART CLI 驱动 FTL。F8/F9 依赖上板。

> 记忆点交叉：`ftl-per-block-bst-skeleton.md` 二期项"档三分层" = 本计划 A3；`project-soft-ssd-overview.md`"FTL 全部模块完成"与 A 档冲突，落地后需更新。

## A 档 — OP 10% + Spare Pool + 三级 Free Pool（容量不动）

### A1+A2（容量不动语义）
- `include/ftl/allocator.h`：`ftl_init_params_t` 加 `op_blocks`/`spare_blocks`（`uint16_t*` 存储，main.c 提供）+ `op_count`/`spare_count`；`ftl_allocator_t` 增 `op_blocks/spare_blocks/op_count/spare_count`（`FTL_OP_PERCENT=10`/`FTL_SPARE_POOL_SIZE=50` 保持，**不改** `L2P_REGIONS_TOTAL`/`L2P_BLOCKS_PER_LUN`/`FTL_LBA_MAX`/metadata/checkpoint）。
- `src/main.c`：free pool 回填循环（main.c:126-137）加两档分流——非坏非元数据块**先扣 `spare_count=50`** 入 `g_spare_blocks`，**再扣 `FTL_OP_PERCENT`**（约 10% 好块）入 `g_op_blocks`，**其余才 `allocator_free_block`**。`g_op_blocks`/`g_spare_blocks` 落 SRAM2（现剩 104KB 足够）。
- `src/ftl/allocator.c`：`allocator_init` 读新 params 字段；`allocator_alloc` 在 free pool 耗尽时回退 OP 池分配（OP 块置 `FTL_BLOCK_STATE_RESERVED`，擦除即用）；`allocator_select_block` 继续只扫 FREE。
- `src/ftl/bbt.c`：`bbt_spare_pool_refill` 从 `return FTL_ERROR` 改为真实现——从 `spare_blocks` 取块 `allocator_free_block` 补 free pool（保 free_count 水位）；`bbt_mark_bad` 后调用。

### A3（三级 Free Pool 状态位）
- `src/ftl/allocator.c`：`allocator_alloc`/`allocator_free_block`/`gc_erase_and_free` 处真正 set/clr `FTL_BLOCK_STATE_PRE_COND`/`EMERGENCY`；`pre_cond_count`/`emergency_count` 从"free_count 别名"改为真实分级计数。GC 预擦除块置 PRE_COND（`gc_erase_and_free` 擦完回 free 时置）。

## B 档 — FTL 语义逻辑
- **B1**：`checkpoint_journal_watermark`（checkpoint.c:400-402）分母改 `(uint32_t)JOURNAL_BLOCK_COUNT * PAGES_PER_BLOCK`（=128 页），与 §7.5.2 对齐（85%→剩 19 页、95%→剩 6 页）。
- **B2**：`checkpoint_create` 成功末尾（checkpoint.c:278 后）+ `journal_truncate`（journal.c:545 后）加 `journal->checkpoint_needed=0; journal->emergency=0`（设计 §7.5.4 复位语义，杜绝 T1 永久命中 + BUSY 每圈强制 CP）。
- **B3**：`ftl_service`（ftl.c:536-546）模式判定改为**基于 pending 占用**（`ftl->pending[i].used` 计数 / `completion_table` 占用）近似空闲度；新增 `pending_count(ftl)`/`completion_table_count()` 计数接口（当前都不存在）。文档标注为近似方案。
- **B4**：`gc_read_page_with_lba`（gc.c:134）从 `ftl_internal_sync_wait` 的 `completion` 读 `ecc_bit0~3` → `rr_record_ecc` + `read_count++`（签名加 `ftl_block_t*` 或 `ftl_context_t*` 参数，调用点 gc.c:359/wear_leveling.c:236/power_failure_recovery.c:701 同步）。
- **B5**：`rr_predictive_scan`（read_recovery.c:378）加 `read_count` 高水位判据；`wl_select_victim`（wear_leveling.c:117）冷块判据加 `read_count` 权重。

## C 档 — ✅ 已落地（v0.47 a8dbab2 + 6c127ef，已推送）

> **本计划原 C 档（NVMe host 全量）在实现前按用户确认重新规划**：学习主控用 UART CLI 驱动 FTL（设计文档 D10），**NVMe 砍掉（C2）**。落地详情见 `ftl_revision_history.md` v0.47 条目。实现为：
> - **C1** `src/controller/state_machine.c`：最小状态机（`ssd_controller_state_transition`，INITIALIZING→READY，失败→FAILED；`ssd_controller_init` 走状态机）
> - **C1b** `src/controller/scheduler.c`：`ssd_controller_schedule` 轻量钩子（host IO 到来降后台预算；实际预算由 `ftl_service` B3 空闲度承担）
> - **C3** `controller.c` `SSD_REQUEST_FLUSH`：全实例 `l2p_cache_flush_all`+`journal_flush_page`
> - **UART CLI** `src/platform/stm32h743/usart.c`：`g_wr_buf[16KB]` SRAM2（plane0=0x55555555/plane1=0xAAAAAAAA）+ `wr/rd [c]/tr/fl/info` 两段式
> - **NVMe 删除**：`src/host/nvme/`+`include/host/nvme/`+`config.mk` MODULES 移除 host（零引用零编译，`make debug/release/tests` 全零警告）

以下原 C 档方案（NVMe host）**已作废**，仅留档：
- **C1**：`src/controller/state_machine.c` 完整状态机（RESET→INITIALIZING→READY→FAILED），`ssd_controller_init` 走 INITIALIZING→READY，任一 FTL 槽位失败转 FAILED。
- **C1b**：`src/controller/scheduler.c` 优先级调度（host IO 优先于后台 GC/元数据），复用 FC `fc_scheduler` 的 HSQ 优先机制做 controller 级仲裁。
- **C2**：`src/host/nvme/nvme_io.c`（Read/Write → `ssd_request_t` → `ssd_controller_submit` + `is_complete` 轮询）、`nvme_admin.c`（Identify 返回 NVM 容量/命名空间）、`submission_queue.c`+`completion_queue.c`（环形队列，SQ=64/CQ=64）、`nvme_controller.c`（按 opcode 分发 Read/Write/Flush/Identify/Trim）。`include/host/nvme/nvme.h` 增 `buffer`/`buffer_size` 字段 + opcode 常量。`FC_COMMAND_MODULE_HOST_IO` 开始被 NVMe host 命令使用。
- **C3**：`controller.c:151` `SSD_REQUEST_FLUSH` 改为对全部 `controller->ftl[ch][ce][lun]` 调 `l2p_cache_flush_all` + `journal_flush_page`（循环完置 complete_flag）；`request.c:230` default 分支同步（FLUSH 走全实例 flush 而非 NOT_SUPPORTED）。

## F8/F9（上板验证，路线图项）
F8（紧急 CP 回切校验）/ F9（CQ/SQ 深度实测）需真 NAND 上板（`make tests` 上板验证），本轮不改代码，体检文档标注"依赖上板"。

## 关键文件
- `include/ftl/allocator.h`、`src/ftl/allocator.c`、`src/main.c`（A）
- `src/ftl/checkpoint.c`、`include/ftl/checkpoint.h`（B1/B2）
- `src/ftl/ftl.c`、`src/ftl/gc.c`、`src/ftl/read_recovery.c`、`src/ftl/wear_leveling.c`、`include/ftl/read_recovery.h`、`include/ftl/pending.h`、`src/ftl/pending.c`、`include/ftl/completion_table.h`（B3/B4/B5）
- `src/controller/state_machine.c`、`scheduler.c`、`controller.c`（C1/C1b/C3）
- `src/platform/stm32h743/usart.c`（C 核心：UART CLI 驱动 FTL）
- 体检文档：`docs/design/ftl_revision_history.md` 末尾新节（记录体检结论 + A/B/C 落地状态 + F8/F9 路线图）
- 记忆点：更新 `ftl-per-block-bst-skeleton.md`（档三分层完成）、`project-soft-ssd-overview.md`（OP/Spare 上线）、新增体检记忆 + MEMORY.md 索引

## 验证
- `make`（debug/release）零错误零新增警告（桩移除后应全零）
- `make FC_NAND_TEST_ENABLE=1` 上板测试固件零错误编译
- 上板：UART 命令 `wr/rd/tr/fl/info` 驱动 FTL 全链路（读回数据一致、写后读、trim、flush、info 看 free/op/spare）；`test` 跑 munit 套件；高压写观察 journal 水位（B1）/ emergency 复位（B2）/ GC 时序（B3）
- 记忆点：更新 `ftl-*` 记忆（OP/Spare/三级池落地、水位公式修复、C 档 UART CLI + NVMe 删除）并核对 MEMORY.md 索引

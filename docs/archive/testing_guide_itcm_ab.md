# testing_guide §9.4 ITCM/DTCM A/B 历史归档

> 从 `docs/design/testing_guide.md` §9.4 移出；正文不改写。

### 9.4 ITCM / DTCM（2026-10-23，正常路径已上板；压力/掉电未覆盖）

| 项 | 当前值 | 判据 |
|---|---|---|
| `.ssd_code_high`（ITCM） | release 61,344 B / debug 57,168 B / 64 KB | VMA `0x00000000`；启动复制后 USB/NAND 正常；debug 余量 8,368 B（2026-10-30）。**逐函数下放**：boot-only 的 `ftl_init` / `ftl_contract_audit` / `contract_check_*` / `l2p_init` / `fc_init_internal` / `fc_bch_init` / `build_mod8_tables` 标 `SSD_CODE_FAST`（内部 Flash），不占 ITCM |
| `.dtcm_data` | release 67,832 B / 128 KB | 预留 20 KB stack；交换版：BCH 进 DTCM、`g_ftl_md` 回 SRAM4 |
| DTCM 内容 | 整对象放置：`ftl.o`（`g_ftl`/`g_ftl_wbuf`/`g_ftl_journal`）、`fc.o`（命令池+描述符池+SQ/HSQ/CQ+簿记）、`nand_dispatch.o`（dispatch queues）、`fc_bch_engine.o`（BCH 静态表） | 均为 CPU-only；**NAND 页缓冲不放 DTCM** |
| SRAM4 内容 | `metadata.o` 的 `g_ftl_md`（33,752 B，NOLOAD，init 时 memset） | 回到 TCM 之前的基线域；基线 843/889 已验证 |
| ITCM 内容 | 公共集（debug/release）：`ftl.o`、`l2p_cache.o`、`wbuf.o`、`pending.o`、`cache_batch.o`、`fc.o`、`fc_data_path.o`、`fc_bch_engine.o`、`nand_dispatch.o`、`nand_fmc.o`（对象内可用 `SSD_CODE_FAST` 逐函数下放）。release 额外集：`allocator.o`、`journal.o`、`metadata.o`、`metadata_job.o`、`migrate.o`、`gc.o`、`wear_leveling.o`、`bg_scheduler.o`、`ftl_op.o`、`fc_interface.o`、`fc_queue.o`、`fc_command.o`、`nand_engine.o` | 用对象路径 `*release/obj/...` 做 build-type 选择，不是 C 预处理器；debug 对象在 `debug/obj/`，不匹配额外集。超过 64 KB 或出现 relocation truncated 时按目标文件收窄 |
| 板端 A/B | 基线 843/889 KB/s；TCM 版（release ITCM 29,728 B）840/885 KB/s；当前 release ITCM 61,632 B（板端结果为重构前 61,384 B 镜像）+ BCH↔md 交换版 **843/887 KB/s**，`info avg=4171us`、`cw=24906/99624/7891/0`、`bad=0`、`mig=0/0`、`gcm=0`，H2testw 415/423 MB 无错误 | 与 840/885 差 +3/+2 KB/s，属测量抖动；本次未触发 GC/迁移，额外 ITCM 集收益未覆盖 |
| BCH/堆 A/B | 旧实现 BCH 需 41 KB 表 + 32 KB roots 的 DTCM 堆；TCM 版只剩约 67 KB 堆，疑似初始化失败并静默跳过 ECC | 启动日志应出现 `[FC ] BCH ready: static tables, no heap`；写速恢复 ≥843 KB/s |
| BCH↔md 交换（正常路径已上板） | BCH 42,656 B → DTCM；`g_ftl_md` 33,752 B → SRAM4；DTCM 67,832 B，free≈63 KB | 2026-10-23 boot `BCH ready` + 恢复 + H2testw 415/423 MB 0 错误（843/887 KB/s）通过；压力/掉电仍按 RISK-TCM-04 |
| 页缓冲域 A/B | 页缓冲放 DTCM：multi-plane handler 数据正确但 NAND 读回全 FF，`format` 全候选失败；放 AXI_NOCACHE：`format` 成功且 H2testw 415/423 MB 通过（840/885 KB/s） | 页缓冲必须 AXI_NOCACHE；不得再条件放 DTCM |
| ITCM 收益判据 | H2testw 是 USB FS（12 Mbit/s 链路，实测 843/887 KB/s 已接近可用带宽）+ NAND tPROG 约束下的顺序负载；本次 `mig=0/0`、`gcm=0`，额外 ITCM 模块基本未执行 | **不能用 H2testw 判断 ITCM 收益**；用 BCH/metadata/GC CPU-bound 微基准，或填盘后随机覆盖触发 GC 再 A/B |

⚠ `DTCM_DATA` / `.dtcm_data` 是 NOLOAD：`g_ftl` 必须在 `ftl_init` 入口全量 memset；
其余候选 handle 已有显式 memset。H743 startup 原先不复制 `.ssd_code_high`，本版已补
`__ssd_code_high_load_start/start/end` 复制循环。

⚠ DMA/NAND 页缓冲：`g_fc_buffer_pool` / `g_fc_return_staging` / `g_nand_channel_page_buffer`
恒在 `.noncacheable_buffer`（`AXI_NOCACHE`，`0x24060000..0x24080000`）。
**页缓冲放 DTCM 会静默破坏 multi-plane program**（NAND 读回全 FF），已由板端 A/B 证实；
不得再按 `CONFIG_NAND_DMA` 条件把它放回 DTCM。

**不建议**为了跑分提前回 CSW 或跨 BOT 命令合并写：会破坏掉电语义。

---

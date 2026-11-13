# FTL 设计文档：历史未验证状态归档

> 从 `docs/design/ftl_design.md` §7.1 移出；正文不改写。

### 7.1 未验证状态

| 项 | 状态 | 说明 |
|---|---|---|
| 多 LUN 阶段 A/B/C（器件配置分层 / 共享 cache / 描述符化与域散列） | **已提交、未上板** | DIM=1 的构建与 DIM=2 的影子链接通过；**不得**据此声称多 LUN 可用 |
| 介质版本 9 布局（哨兵层删除 / 固定元数据 P0+P1 镜像 / Journal multi-plane 擦 / 磨损并入 CP） | **已提交、未上板** | 老介质首启自动格式化是预期；**不得**据此声称掉电恢复与磨损持久化已验证 |
| 介质版本 10：固定 L2P 日志区 + 容量 429 MiB（`RISK-MD-07` / `RISK-CAP-02`） | **已提交、未上板（rev33）** | 首启自动格式化（v9→v10，20–40 s）；需验 429 MiB、`live=` 有界、H2testw 全量写满 |
| 介质版本 11：元数据 shared spare（78..81）+ 候选块池 + 精确容量 + OP 坏块替换 + 写失败重试 | **已改、未上板（2026-10-21）** | 首启自动格式化（v10→v11）；需验 `[BLK] bad areas` 保留 282 data BAD、`pool` 候选池、`[CAP]` ≈ 392.75 MiB、`bbt fault` 负例、H2testw 全盘 |
| 介质版本 12：L2P region 主备双副本（日志区 96 块、目录检查点 16 页、容量 424 MiB） | **已上板（2026-10-22）** | 首启自动格式化 v11→v12；全盘 H2testw 415/423 MB 写+校验通过、`bad=0`；掉电/故障注入未做（见 `known_risks.md`） |
| DIM=1 上板回归 | **部分完成（2026-10-22）** | 已覆盖：格式化启动、全盘写+校验、GC/CP、容量；未覆盖：掉电重启、格式化中断、故障注入 |
| 阶段 D（审计/文档/验证） | 未开始 | 不是新功能；见 §7.3 |
| FTL UT | **已重建并上板；cache read 修复后已重跑全量（2026-10-25）** | `test /ftl/` 85 次运行全过（63 个函数 + 参数化，61.05 s；修复后 debug 01:09:23 镜像 **85/85、61.20 s**）；用例与风险矩阵见 `docs/design/ftl_ut_design.md`。⚠ 每次完整 media 运行会永久标坏 1 块（RISK-UT-06） |
| 文档重构 | 本文 | 文档改动不涉及固件，不需要上板；不得写成“功能已验证” |
| v0.83 回收前目录检查点 + L2P 降级可见性（`RISK-MD-06` / `RISK-L2P-04`） | **已提交、未专项上板** | 编译三关通过；需按 `known_risks` 的 64-region 复现 / 故障注入；**不得**据“普通读写通过”声称已验证 |
| v0.94 cache program（host 16 KiB + GC/WL 批次） | **host 正常路径已上板；GC batch/负例未验** | H2testw 415/423 MB 通过，`cw=24906/99624/0 abort`；`mb=0`（GC batch 未触发）；FAILC 负例、失败注入、掉电注入仍见 `known_risks` RISK-CACHE-02..07 |
| v0.97 DTCM/ITCM 热路径 | **正常路径已上板；压力/掉电未覆盖** | 页缓冲 AXI_NOCACHE 后 `format` + H2testw **840/885 KB/s、415/423 MB 无错误**（`.dtcm_data` 58,952 B、BCH SRAM4）。交换版：BCH 42,656 B→DTCM、`g_ftl_md`→SRAM4，`.dtcm_data`=67,832 B、free≈63 KB；release 额外 ITCM 集使 `.ssd_code_high` 29,728→61,384 B（debug 61,992 B 不变）。2026-10-23 该组合 H2testw **843/887 KB/s、415/423 MB 0 错误**，`bad=0`；相对 840/885 为噪声级，且 `mig=0/0`/`gcm=0` ⇒ 额外 ITCM 集收益未证实（RISK-TCM-05/09） |
| v0.98 可读性重构 | **未上板** | BBT helper、allocator close/refill 收薄、cache read/write 长函数拆分、`bst_wear_s` 快照、bg 时钟去重；行为目标不变。ITCM release 61,296 B / debug 62,064 B；`make all/release/tests UT=FC` 零新告警；当时 UT=FTL 因冻结测试源失败（**2026-10-24 已重建，85 次全过**） |
| v1.00 cache read 判据修正 | **已上板（2026-10-25）** | `cache_read_collect` 跨 plane 改用 `pba_to_page()`、连续性改为页号 +1；修复前 cache read 自实现以来从未生效（`batches` 恒 0）。`/speed/ftl_rw` 2/2、`rd_path=cache`、`rdc batches=1600`；全量 `/ftl/` 已在修复后镜像上重跑 **85/85（61.20 s）** |
| `RISK-MIG-02` / `RISK-GC-07` 迁移读失败活锁 | **已修；rev32 上板已观测 3 次标坏** | 改为按 plane 源块累计失败计数、单页读成功不清零；rev32 实机出现 `x3 — mark BAD` 并继续换候选。rev33（固定日志区）未上板 |
| cache read/write run 的 FC chain（ADR-17） | **已改、未上板（2026-10-30）** | 读 run 长度改由 FC 资源决定（不再等于 FIFO 深度）、整条 run 标 `FC_CHAIN_MORE`/`END`、`ftl_op_submit_batch` 原子入队；cache program 与 GC/WL batch 同标。编译三关零新告警；**必须上板**验证跨轮次 owner / HSQ 回推 / 失败页状态（R2 建链类改动） |
| P1 cache 读流式序列 | 正常路径已上板（2026-10-30 debug：H2testw 415/423 MB 0 错误；**release -O2（23:14 镜像）H2testw 415/422 MB 0 错误、写 867 / 读 887 KB/s**，`cr=26475/104138/1793/0`（declines 上升 = `draining` 门控让其他组退避）；format 后空盘基线读 887 KB/s，确认上一轮 779 是 GC 竞争；cr=26.4k 窗口/103.7k pair、fallback=0）；命令 trace / 中止负例未做 | 序列状态跨窗口/跨 chunk（`ftl_cache_seq_s` + `pend_group_s.cmd_start/cmd_total`）；START 一次、中间 SEQ、LAST 只在断点；8 条 LBA 驻留 lookahead（不触发加载）；同 LUN 门控 + D4 `break_requested`；孤儿序列 1s 守卫（`ftl_cache_seq_idle_guard`，放 `.ssd_code_fast`）。编译四关（`all`/`release`/`tests UT=FTL`/`tests UT=FC`）零新告警；boot-only 代码（`ftl_init`/`ftl_contract_audit`/`contract_check_*`/`l2p_init`/`fc_init_internal`/`fc_bch_init`/`build_mod8_tables`）移 `.ssd_code_fast` 后 debug `.ssd_code_high` 57,168 B（余量 8,368 B），release 61,344 B。**必须上板**：45 一次 / 46×N / 47 一次且跨 chunk 无插入、读中途中止后再读、中止后 CP/journal 不饿死（RISK-IF-09/10）；顺带修 `request.c` io 模板残留（RISK-USB-07） |
| P2 cache 写流式序列 | 正常路径已上板（2026-10-30 debug：写 869 KB/s、cw=25.5k 窗口/100.8k 页、abort=0、全盘 0 错误；**release -O2（23:14 镜像）：写 867 KB/s、`cw=25538/100912/7983/0`（abort=0）、全盘 0 错误**）；程序失败 / 掉电注入未做 | 写序列跨窗口/跨 chunk（首 49/中间 44/断点 48）；逐页确认 FIFO（第 i 页由 i+1 的 `FAIL_CACHE` 确认、末页由 END）；窗口前 L2P 预加载 + journal 预留，失败即以 END 收尾；控制器双槽（1 chunk lookahead、非末 chunk 立即流控回执、`TUD_MSC_RET_BUSY` 背压）。编译四关零新告警；debug `.ssd_code_high` 58,664 B（余量 6,872 B），DTCM 84,528 B。**必须上板**：写 trace（49 一次 / 44×N / 48 一次、跨 chunk 无插入）、H2testw、程序失败/掉电注入（RISK-IF-11/12/13） |
| P3 门控 / 后台 / RR 审计 | 审计结论 + deadline 挂起 + 限流均上板验证（2026-10-30 新镜像：无 submit-stalled 刷屏，GC/CP/journal 推进；`d4=13882` 空盘 vs 24820 满盘） | `src/ftl` 下无绕过 `ftl_op_submit*` 的 `fc_submit` ⇒ 门控覆盖 host / GC / WL / CP / journal / metadata / L2P / RR；各后台路径把门控拒绝当 `FTL_AGAIN` 重试（迁移/RR 释放命令后下轮重试），被拒提交置 `break_requested`，序列在下一窗口收尾；`info` 新增 `d4=` 计数。BOT 串行下 RR（host 读触发）与写序列不共存。 |
| 上板刷屏（META/CP/MIG/RR submit stalled、JRN erase kick、L2P load fail） | 已上板验证（2026-10-30 新镜像 17:33：JRN/L2P 单行带 suppressed、无 META 刷屏） | 后台提交 deadline（10 ms）短于 USB chunk 间隔（~19 ms），写序列门控期间四类 job 每 10 ms abort + WRN 刷屏；另有 `[JRN] erase kick failed`（无节流 ERR）与 `[L2P] load start failed` 的 64 条计数突发。修复：① `ftl_op_cache_seq_active()` 在序列打开时挂起四类 job 的提交 deadline；② 限流策略统一为**时间优先**（有钟按 5 s/1 s，无钟才用 64 条兜底）——`ftl.c` 共享限流、META abort、L2P load/evict；③ gate 造成的 L2P 加载失败降为 5 s 慢节拍；④ JRN erase-kick 日志加 5 s 限流 + suppressed。2026-10-30 上板 H2testw（未 format，417/423 MB）：META 刷屏消失，`JRN`/`L2P` 仍见旧镜像的节流前日志；四关零新告警，待新镜像复测 |
| P4 文档合并 / 旧路径清理 | **已完成（2026-10-30，未上板）** | 临时设计稿并入 §3.16 并删除；删除 `cache_wb_t` 旧整批状态机、`FTL_CACHE_WRITE_LBAS` 死宏、`wr.abort` 死字段；四关零新告警；上板回归清单见 §7.1。**上板前 review 又修 2 处**：① USB 槽回收 `memset` 后 `bounce` 指针变 NULL ⇒ 保存/恢复；② 写 END 完成回调过早清 `kind` / 断言 FIFO 空 ⇒ 改为只验证本页已摘除（多子序列共存） |
| **v1.04 设计 A：FTL 顺序流 + Journal 顺序映射页** | **已改、未上板（2026-10-31）** | 序列开启改由 `cache_stream.rd/wr` 检测器（连续 LBA ≥64）判定；延续只看 `next_lba`（传输段不是断点）；`cmd_remaining` 只给 END 落点；写段配额 = 一张 JSEG 页 504 条（63 pair 页 ≈252 KiB）；后台元数据被门控但不请求断点。介质版本 **v13（首启自动格式化）**。四关（`all`/`release`/`tests UT=FTL`/`tests UT=FC`）零新告警。**上板判据**：`info` 的 `cw=`/`sw=` ⇒ `pages/starts` 接近 63；跨段只有一次 START；掉电重启回放 JSEG。见 §3.16.10 与 `known_risks.md` RISK-CACHE-15/16/17 |

| **v1.05 设备序列跨 host 命令（volatile write cache）** | **已改、未上板（2026-10-31）** | 写段（含末段）提交即回执；设备序列跨命令延续；主动断点 = Journal 顺序页写满（63 pair ≈252 KiB）；padding END 收尾无数据断点；写失败 = deferred error（下一条命令失败一次）。`info`：`sw=starts/page_ends pc=pad_closes jf=page_writes`。四关零新告警。**上板判据**：`pages/starts≈63`、`jf≈sw+断点`、`pc` 少量、deferred 注入可见、断电窗口符合 RISK-CACHE-18。见 §3.16 与 `known_risks.md` RISK-CACHE-15/16/18、RISK-USBC-01 |

> 上表中 v1.05 之前的行是**当时**的验证记录（保留原话）；其中 `cmd_start/cmd_total`、
> `FTL_CACHE_READ_TRIAL_LBAS` 已在 v1.04 删除；v1.05 又取消了「命令边界 END」，当前口径
> 以 §3.16 与 v1.05 行为准。
> 符号级演进见 `ftl_revision_history.md` v1.04。

> `RISK-WB-06`（wbuf pair 镜像填装）已于 **rev27 上板验证**：Windows FAT 格式化 +
> `notepad.exe` / 16KB 非均匀 pattern 写入 → `Write-VolumeCache` → 拔插 USB →
> SHA256 与 2KB 块指纹一致。验证步骤见 `docs/design/usb_msc.md` Part B。

**cache 流式序列（P1/P2）上板清单**（未执行；R1：执行前不得声称已验证）

1. 命令 trace（逻辑分析仪 / FC 日志）：读 `45` 一次 / `46` N−1 / `47` 一次；写 `49` 一次 /
   `44` N−1 / `48` 一次；窗口之间无其他命令插入。
2. 大文件顺序读写（H2testw / `dd`）：读速与写速不劣于 v1.03 基线；`info` 的
   `cr=`（读窗口/pair/段数/回退）与 `cw=`（写窗口/页/回退/中止）+ `sw=`（写段数/
   配额断点）非零，且 `batches/starts`（读）、`pages/starts`（写）明显大于 4，
   证明 cache 路径**确实生效且跨传输段延续**（C8 / §3.16.10）。
3. D4：大顺序 I/O 期间后台（GC/CP/journal）有进展；`d4=` > 0 且无 `IO_ERROR`。
4. 读中止：读中途中止/拔线 → 再读同一/相邻 LBA，逐字节比对（RISK-IF-09/10）。
5. 写失败注入：单/双 plane 中间页 program 失败 ⇒ 失败页及其后不确认、之前页已确认；
   CSW 报错（RISK-IF-11、RISK-USB-08）。
6. 掉电注入：写中途断电 → 重启后 Journal/Checkpoint 恢复、部分写块关闭、无陈旧映射。
7. free pool 低位压测：大顺序写下观察最长写停顿与 `IO_ERROR`（RISK-IF-12）。

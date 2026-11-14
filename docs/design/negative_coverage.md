# 负例覆盖判定（302 条）

> 判定口径（用户 2026-11-14 定稿）：
> - **M**（已覆盖）= 有能变红的负例且上板通过；
> - **N**（可覆盖未实现）= 注入点、触发步骤、断言三样已从源码确认，只差写用例；
> - **K**（明确不覆盖）= 现有手段/硬件做不到，写明原因；
> - **待判定** = 信息不足，写明缺什么。
>
> 全量清单来自 `docs/design/known_risks.md` 的开放索引（302 条）。
> ⚠ 设计文档 §11 原表遗漏 19 条 `RISK-L2P-*`，已补齐。
>
> 填写规则：每条必须有判定与一句理由；N 必须给出 seam / 触发 / 断言 / 用例名。
> **禁止**把「没设计过」写成 N。

| # | 模块 | ID | 状态 | 一句话 | 判定 | 理由 | seam / 触发 / 断言 / 用例名 |
|---|---|---|---|---|---|---|---|

| 1 | 1 metadata 模块 | RISK-MD-01 | 待上板 | 日志块擦除持续失败无限重试，无坏块策略 | K | K（触发不可确定性构造）：需目录压力才触发 reclaim；未证明可确定性构造 | |
| 2 | 1 metadata 模块 | RISK-MD-02 | 待上板 | 压缩预算按启动时 l2p_live，在飞 region 写可能超预算 | K | K-deferred：压缩与前台写交错时序不可控（并发窗口） | |
| 3 | 1 metadata 模块 | RISK-MD-06 | 待定位 | L2P 指向已归还/已擦块，恢复回退 full rebuild 丢映射 | M | 已上板（2026-11-14 debug UT=FTL `test /ftl/media/negative/` 36/36、0 fail、200.10 s）；format + 3 代写 + 4 次重启保最新映射| seam: 无；触发: 3 代写后 reboot×4；断言: region 未降级、读回第 3 代；用例 md_alias_full_rebuild_repro |
| 4 | 1 metadata 模块 | RISK-MD-18 | 待上板 | 目录检查点未覆盖最新 Directory 时压缩擦 victim | K | K（触发不可确定性构造）：需压缩 job 与前台 commit 交错；触发未证明 | |
| 5 | 1 metadata 模块 | RISK-MD-19 | 待上板 | victim 未排除在飞槽位，stale count 被清零 | K | K（触发不可确定性构造）：需在飞写回与扫描重叠；触发未证明 | |
| 6 | 1 metadata 模块 | RISK-MD-20 | 待上板 | 压缩 read→commit 无 CAS，陈旧映射持久化 | K | K（触发不可确定性构造）：需压缩读→commit 间前台搬移同一副本；触发未证明 | |
| 7 | 1 metadata 模块 | RISK-MD-21 | 待上板 | 目录检查点 abort 不消费槽位，二次编程 | M | 已上板（2026-11-14 debug UT=FTL `test /ftl/media/negative/` 36/36、0 fail、200.10 s）（cp_dir_ckpt_abort 补槽位推进断言）；中止后 write_idx 必须前进| seam: FI_CP_ABORT_PHASE(CP_DIR_CKPT_WRITE)；触发: 置脏+强制CP；断言: 中止后 ckpt.write_idx 前进、恢复 CP 另写一槽、reboot 数据完整；用例 cp_dir_ckpt_abort |
| 8 | 1 metadata 模块 | RISK-MD-22 | 待上板 | 借来的日志块标 BAD 时漏减 borrow 配额 | K | K（触发不可确定性构造）：需 reclaim 连续擦失败触发标坏；触发未证明 | |
| 9 | 1 metadata 模块 | RISK-MD-23 | 待上板 | ring frontier 落在块中时复用半编程槽 | K | 需写目录检查点期间掉电（人工断电） |  |
| 10 | 1 metadata 模块 | RISK-MD-24 | 待上板 | initial 目录检查点只写单 plane，回退到旧 record | K | 需 format 中途掉电 |  |
| 11 | 2 GC / migrate / journal | RISK-GC-01 | 待上板 | 永久不可读/ META 损坏 victim 反复 abort，容量不增长 | M | 已上板（2026-11-14 debug UT=FTL `test /ftl/media/negative/` 36/36、0 fail、200.10 s）；预置 2 次失败 + 注入 1 次验证第 3 次标坏；不依赖 victim 重选| seam: FI_FC_STATUS_DATA(FI_KEY_ANY, DATA_ERROR) 持续；触发: 预选有数据 victim + 预置 fail_count=2 + 强制 GC；断言: bad_count+1、该 gblock BAD、数据可读、放开后 GC 继续；用例 gc_read_unc_marks_bad |
| 12 | 2 GC / migrate / journal | RISK-GC-02 | 待上板 | 目的页写失败不能整块标坏已有成功迁移页的 Cold SB | N | 已上板（54/54）；行为断言通过，但已迁移页读回 0xFF → 风险转待定位，仍按 N 计 | 用例 gc_write_complete_fail_closes_cold |
| 13 | 2 GC / migrate / journal | RISK-GC-04 | 待上板 | 部分 plane 擦除后中止，旧 Closed 条目误认已释放 plane | M | 已上板（2026-11-14 release UT=FTL `/ftl/media/negative/` 54/54、0 SKIP、219.32 s）；N：已写/未上板；成对 victim plane0 擦成功后 plane1 擦失败；断言 aborted、失败 plane 标 BAD、已擦 plane 不被 Closed 条目引用| 用例 gc_partial_plane_erase_abort |
| 14 | 2 GC / migrate / journal | RISK-GC-05 | 待后续 | GC Bitmap 未实现，掉电后 GC 从块头重来 | K | 性能优化项（GC bitmap），非失败场景 |  |
| 15 | 2 GC / migrate / journal | RISK-GC-06 | 待上板 | WL 5s 节拍/CLOSED 门控/valid_count 上界/三态未板验 | K | K（需长压测）：WL 5s 节拍/CLOSED 门控的直接断言需白盒或长压测 | |
| 16 | 2 GC / migrate / journal | RISK-GC-10 | 待上板 | 系统性 victim 读失败无熔断，好块成批标 BAD | M | 已上板（2026-11-14 release UT=FTL `/ftl/media/negative/` 54/54、0 SKIP、219.32 s）；N：已写/未上板；预置 streak=MAX-1 + victim 第 3 次失败；断言标 BAD、paused、后续 GC 不再起| 用例 gc_systemic_read_fuse |
| 17 | 2 GC / migrate / journal | RISK-GC-14 | 待上板 | no-gain 判据依赖 total_writes/free 快照 | K | K（需长压测）：需满盘/慢写窗口，短负例不可控 | |
| 18 | 2 GC / migrate / journal | RISK-GC-15 | 待上板 | BUSY 档挡掉 urgent GC，free 掉到 1 才急救 | K | K（需长压测）：需写到 free<32 的长序列，属压测 | |
| 19 | 2 GC / migrate / journal | RISK-GC-17 | 待上板 | cache-program batch 页基址误判，batch 永久禁用 | M | 已上板（2026-11-14 debug UT=FTL 全量 36/36）；成功 GC 后可直接读 batch 计数| 已写/未上板；seam: 无需注入；触发: 强制GC完成；断言: state.batch_jobs>0、!batch_disabled；用例 gc_batch_program_fail |
| 20 | 2 GC / migrate / journal | RISK-GC-18 | 待上板 | 空 victim 快路径绕过 Journal 持久性闸门 | K | K（触发不可确定性构造）：需上一 job abort 且 journal 页缓冲未落盘的窗口 | |
| 21 | 2 GC / migrate / journal | RISK-GC-19 | 待上板 | 解码缺 meta 的 CW 被静默当未写，静默丢数据 | K | K（白盒/夹具不可达）：需构造缺 meta 的 FC 读链（白盒/FC夹具） | |
| 22 | 2 GC / migrate / journal | RISK-MIG-01 | 待上板 | 等待命令丢失/卡住时永久 active | M | 已上板（2026-11-14 debug UT=FTL 全量 36/36）；用 L2P 加载失败让 job 等待超时| 已写/未上板；seam: FI_L2P_LOAD_FAIL 持续；触发: 强制GC；断言: FTl_WAIT_TIMEOUT 内 job.aborted、不永久 active；用例 gc_l2p_load_wait_timeout |
| 23 | 2 GC / migrate / journal | RISK-MIG-02 | 待上板 | predictive 迁移活锁（同 GC-07 修法） | K | K（触发不可确定性构造）：需 predictive 扫描触发与坏页组合 | |
| 24 | 2 GC / migrate / journal | RISK-MIG-03 | 待上板 | 标 BAD 后 L2P 仍指向该块，valid_count 回绕 | M | 已上板（2026-11-14 release UT=FTL `/ftl/media/negative/` 54/54、0 SKIP、219.32 s）；N：已写/未上板；ftl_block_sub_valid_if_live：BAD/零计数不变、正常块 −1、0 不回绕| 用例 mig_block_sub_valid_no_wrap |
| 25 | 2 GC / migrate / journal | RISK-MIG-05 | 待上板 | GC 读失败 abort 日志刷屏阻塞 UART | K | 日志速率/刷屏，UT 无采集判据 |  |
| 26 | 2 GC / migrate / journal | RISK-MIG-06 | 待上板 | mark BAD 日志单位/原因不区分 | K | 日志字段格式，非失败场景 |  |
| 27 | 2 GC / migrate / journal | RISK-MIG-07 | 待定位 | 大批块被 GC 读失败退役，L2P 悬空到 BAD 块 | M | 已上板（2026-11-14 release UT=FTL `/ftl/media/negative/` 54/54、0 SKIP、219.32 s）；N：已写/未上板；同 GC-10（熔断后不再成批标坏）| 用例 gc_systemic_read_fuse |
| 28 | 2 GC / migrate / journal | RISK-MIG-09 | 待上板 | host 写失败只标 plane0 的块 | K | K（触发不可确定性构造）：需逐 CW/plane 完成态失败；现有 FC_STATUS 只按 opcode 键 | |
| 29 | 2 GC / migrate / journal | RISK-MIG-10 | 待上板 | 迟到完成可能按新 job 的 phase 派发 | K | 需人为丢完成/迟到完成，FC 引擎无注入点 |  |
| 30 | 2 GC / migrate / journal | RISK-JN-01 | 待上板 | Journal 环形块擦除失败无限重挂 need_erase | M | 已上板（2026-11-14 release UT=FTL `/ftl/media/negative/` 54/54、0 SKIP、219.32 s）；N：已写/未上板；环形块连续擦失败；断言 bad_mask、两 plane BAD、计数清零、未 ring_full| 用例 journal_ring_erase_fail |
| 31 | 2 GC / migrate / journal | RISK-JN-02 | 待上板 | 同步刷页返回 void，调用者不知屏障未达成 | M | 已上板（2026-11-14 debug UT=FTL `test /ftl/media/negative/` 36/36、0 fail、200.10 s）；journal 写失败注入已存在；flush 返回码可断言| 已写/未上板；seam: FI_JOURNAL_WRITE_FAIL；触发: 写+fl；断言: ftl_flush 返回 false/错误可见、pending 阻止 victim 擦；用例 flush_reports_journal_error |
| 32 | 2 GC / migrate / journal | RISK-JN-03 | 待上板 | write_fail_pending + CP 清账未做故障注入 | M | 既有 e2e 用例 journal_write_fail_wal_stall 覆盖（Pages 写失败→WAL 闸门挡住→放开恢复）；历史板端通过 | |
| 33 | 2 GC / migrate / journal | RISK-JN-04 | 待上板 | drain 超时后仍同步刷页，同页二次编程 | K | K（触发不可确定性构造）：需命令丢失/超时（FC 引擎无注入） | |
| 34 | 2 GC / migrate / journal | RISK-JN-05 | 待上板 | 回放失败/降级日志逐条打印，拖慢开机 | K | 日志速率测量 |  |
| 35 | 2 GC / migrate / journal | RISK-JN-06 | 待上板 | 回放工作缓冲从栈改文件级 static | K | 栈帧是构建测量项，非负例 |  |
| 36 | 2 GC / migrate / journal | RISK-JN-07 | 待上板 | jrn_block_next 把 from 自身算作下一块 | K | K-deferred：需连续 3 块环形擦失败（烧 6 块）；本轮未写 | |
| 37 | 2 GC / migrate / journal | RISK-JN-08 | 待上板 | 回放第二遍读失败不上报，静默跳过条目 | K | K（触发不可确定性构造）：boot 回放期间定点读注入不可控 | |
| 38 | 2 GC / migrate / journal | RISK-JN-09 | 待上板 | 32-bit Journal 序号无回绕处理 | M | 已上板（2026-11-14 debug UT=FTL 全量 36/36）；白盒把 sequence 置近回绕后断言串行比较| 已写/未上板；seam: 无（直接置 journal->state.sequence_counter）；触发: 置 0xFFFFFFF0 后 append/scan；断言: 串行比较不倒退、写点正确；用例 journal_seq_serial_compare |
| 39 | 2 GC / migrate / journal | RISK-JN-10 | 待上板 | 撕裂 frontier 页重复编程 + 每次开机 full rebuild | K | 需写 journal 期间掉电 |  |
| 40 | 2 GC / migrate / journal | RISK-RR-02 | 待上板 | 恢复成功仍报 IO_ERROR / recovered 只看最后 plane | M | 已上板（2026-11-14 debug UT=FTL `test /ftl/media/negative/` 36/36、0 fail、200.10 s）；需按 plane 键的完成态注入（扩展 FC_STATUS）| 已写/未上板；seam: 新 FI_FC_STATUS plane key（完成 cmd->paa 推 plane）；触发: 1次触发RR+仅一plane重读失败；断言: all_ok=false、host IO_ERROR；用例 rr_mixed_plane_fail |
| 41 | 2 GC / migrate / journal | RISK-RR-03 | 待上板 | ECC 健康状态可降级（CRITICAL→WARNING） | M | 已上板（2026-11-14 debug UT=FTL `test /ftl/media/negative/` 36/36、0 fail、200.10 s）；可直接调 rr_record_ecc 合成 ECC 序列| 已写/未上板；seam: rr_record_ecc 公开 API；触发: 升到 CRITICAL 后再喂低 ECC；断言: 状态不回落；用例 rr_ecc_monotonic |
| 42 | 2 GC / migrate / journal | RISK-RR-04 | 待上板 | 预测巡检命中含 BAD plane 的 SB，migrate 卡死 | M | 已上板（2026-11-14 release UT=FTL `/ftl/media/negative/` 54/54、0 SKIP、219.32 s）；N：已写/未上板；两条目都达阈值，第一个含 BAD plane；断言返回第二个| 用例 rr_predictive_skip_bad_plane |
| 43 | 2 GC / migrate / journal | RISK-RR-05 | 待上板 | SLC 无 read-retry table 却做 8 次重试 + 邻居辅助 | K | 用户明确不处理 0x89；行为是快速失败，否定例 |  |
| 44 | 2 GC / migrate / journal | RISK-RR-06 | 待上板 | RR 重试读被 read sequence draining 挡住，host 读组永不完成 | K | K（触发不可确定性构造）：需 ≥16 LBA 读序列 + RR 互锁；现有 8-LBA 缓冲/序列构造未证明 | |
| 45 | 2 GC / migrate / journal | RISK-WL-10 | 待上板 | 静态 WL 快照恢复后从不触发（分母用整 SB） | M | 已上板（2026-11-14 release UT=FTL `/ftl/media/negative/` 54/54、0 SKIP、219.32 s）；N：已写/未上板；单 plane 快照条目 + free 高擦除块；断言 spread≥THRESHOLD、wl_should_trigger| 用例 wl_snapshot_entry_trigger |
| 46 | 2 GC / migrate / journal | RISK-WL-11 | 待上板 | WL 目的准备失败静默退化为普通分配 | M | 已上板（2026-11-14 release UT=FTL `/ftl/media/negative/` 54/54、0 SKIP、219.32 s）；N：已写/未上板；closed 列表满；断言 prepare 返回 false、cold SB 保持打开| 用例 wl_destination_prepare_fail |
| 47 | 2 GC / migrate / journal | RISK-ALLOC-14 | 待上板 | 运行期标 BAD 让 Closed 条目永久卡死 | M | 已上板（2026-11-14 release UT=FTL `/ftl/media/negative/` 54/54、0 SKIP、219.32 s）；N：已写/未上板；bbt_mark_bad 同步摘除 Closed 条目的 plane| 用例 alloc_closed_plane_bad_drop |
| 48 | 2 GC / migrate / journal | RISK-PERF-01 | 待后续 | GC/WL 选块重复全表扫描 | K | 性能项，非失败场景 |  |
| 49 | 2 GC / migrate / journal | RISK-PERF-02 | 待后续 | GC/WL 目的页逐 plane 单条写，无 multi-plane 命令 | K | 性能项 |  |
| 50 | 2A BBT（Bad Block Table） | RISK-BBT-01 | 待上板 | 运行时标坏的幂等 / 持久化 / Spare 记账未板验 | M | 已上板（2026-11-14 debug UT=FTL `test /ftl/media/negative/` 36/36、0 fail、200.10 s）（幂等 + 持久化）；Spare 池收支未断言| seam: bbt_mark_bad/bbt_save；触发: FREE 块标坏两次 + save + reboot；断言: bad_count 只 +1、重启后仍 BAD；用例 bbt_mark_bad_idempotent |
| 51 | 2A BBT（Bad Block Table） | RISK-BBT-02 | 待上板 | 工厂扫描对列读失败页的判坏行为未实测 | K | 需新增 ftl_sync_read_column wrap + 空片扫描（破坏性） |  |
| 52 | 2A BBT（Bad Block Table） | RISK-BBT-03 | 待上板 | 格式化擦除与 BBT save 间掉电丢失工厂坏块图 | K | 需 format 处理中掉电 |  |
| 53 | 2A BBT（Bad Block Table） | RISK-BBT-04 | 待后续 | BBT header 无布局版本字段，格式演进无法识别 | K | 介质格式演进项，非负例 |  |
| 54 | 2A BBT（Bad Block Table） | RISK-BBT-05 | 待上板 | 元数据区为空时可能误判空片并重扫 | K | 需只擦元数据保用户数据，破坏性且无可靠判据 |  |
| 55 | 2A BBT（Bad Block Table） | RISK-BBT-06 | 待后续 | BBT 回调不保存 command_id，重复完成可能错配 phase | K | 当前 NAND 无重复完成源，不可构造 |  |
| 56 | 2A BBT（Bad Block Table） | RISK-BBT-07 | 待上板 | CP 快照并集的 BAD 不补 Spare 池 | K | K（触发不可确定性构造）：需 CP 快照含 BAD 且 Spare 不足的介质 | |
| 57 | 2A BBT（Bad Block Table） | RISK-BBT-10 | 待后续 | kick / factory-scan / format_erase 缺单测 | N | 部分已上板：kick 路径（bbt_kick_candidates）通过；factory-scan / format_erase 破坏性路径未覆盖 | 用例 bbt_kick_candidates |
| 58 | 2A BBT（Bad Block Table） | RISK-BBT-12 | 待上板 | 固定元数据池无 BAD 跳过/替换 | K | K（触发不可确定性构造）：标记元数据池块会持久破坏同一 test 会话的后续用例；无隔离/复原手段 | |
| 59 | 2A BBT（Bad Block Table） | RISK-BBT-13 | 待上板 | 固定元数据分区无法按 BBT 重映射 | K | 设计判定：不做 remap 表 |  |
| 60 | 2A BBT（Bad Block Table） | RISK-BBT-14 | 待上板 | FORMAT_IN_PROGRESS 提交点过早 | K | 需 format 中途掉电 |  |
| 61 | 2A BBT（Bad Block Table） | RISK-BBT-15 | 待上板 | 运行期元数据块变坏，候选池/游标不更新 | K | K（触发不可确定性构造）：运行期标坏 journal/CP 块会破坏同一会话；需独立介质会话或快照复原 | |
| 62 | 2A BBT（Bad Block Table） | RISK-BBT-16 | 待上板 | 全片体检可能恢复边缘坏块 | K | 需边缘坏块介质与 format --verify |  |
| 63 | 2A BBT（Bad Block Table） | RISK-BBT-17 | 待上板 | BBT 候选池判断散落多处，易误擦 / 误留 | K | 重构/收敛项，非失败场景 |  |
| 64 | 2A BBT（Bad Block Table） | RISK-BBT-18 | 待上板 | 多圈 BBT ring 新写 record 后 reload 选到上一条 | K | K（触发不可确定性构造）：需多圈 ring + BAD 读失败的特定布局 | |
| 65 | 2A BBT（Bad Block Table） | RISK-BBT-19 | 待上板 | BBT ring 块擦除失败导致持久化永久 livelock | K | K-deferred：需把 BBT ring 写指针推到末槽（白盒）+ 擦失败；本轮未写 | |
| 66 | 2A BBT（Bad Block Table） | RISK-BBT-20 | 待上板 | 同步 bbt_save 与异步 job / 并发标坏无协议 | K | CLI format 与 1ms 在飞窗口重叠不可控 |  |
| 67 | 2A BBT（Bad Block Table） | RISK-BBT-21 | 待上板 | 出厂扫描跳过 Block 0 的器件依据无出处 | K | 需空片/器件出厂坏块证据 |  |
| 68 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-02 | 待上板 | carve 先于 full rebuild，写过块同时是池成员与 CLOSED | K | K（触发不可确定性构造）：需预置数据并强制 full rebuild fallback；触发未证明 | |
| 69 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-03 | 待上板 | CP 读失败被当「需要格式化」，一次瞬时读错擦整片 | K | K-deferred：若缺陷存在会擦整片，单 UT 会毁掉同会话后续用例；需独立会话 | |
| 70 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-04 | 待上板 | 两阶段格式化中途掉电，重启无法判断未完成 | K | 两阶段格式化中途掉电（人工断电） |  |
| 71 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-05 | 待上板 | 快照恢复的校验/计数改动未板验 | M | 已上板（2026-11-14 debug UT=FTL `test /ftl/media/negative/` 36/36、0 fail、200.10 s）；可用 raw 写破坏 CP 页后 reboot| 已写/未上板；seam: FC raw program 写坏 CP record；触发: reboot；断言: 回退 full rebuild、旧映射可读；用例 boot_corrupt_cp_restore |
| 72 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-06 | 待上板 | full rebuild 按第一个空白页当写指针，hole 后漏计 | K | K（触发不可确定性构造）：需 raw 写造 page0 空洞 + 强制 full rebuild；raw 构造未设计 | |
| 73 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-07 | 待上板 | 判空依赖 raw USER 区，加扰后不可靠 | K | K（触发不可确定性构造）：已由 IF-15 取代；判空判据未确定 | |
| 74 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-08 | 待上板 | Journal 扫描/回放读失败被当无更多条目 | K | K（触发不可确定性构造）：boot 期定点读失败注入不可控（无 PBA 键） | |
| 75 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-09 | 待上板 | md_load 失败静默新建 Directory，旧映射整体丢失 | K | K（白盒/夹具不可达）：需 Anchor 损坏且可观测「拒绝启动」；boot 夹具无拒绝返回通道 | |
| 76 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-10 | 待上板 | jrn_resume 忽略跨块擦除结果，首次写落未擦页 | K | K-deferred：需把介质预置成 resume 跨块待擦状态，当前无构造 | |
| 77 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-11 | 待上板 | 快照 probe L2P 反查失败只 LOGW，仍声称 restored | K | K-deferred：需 boot 快照 probe 的 L2P 反查失败定点，PBA 取址未设计 | |
| 78 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-13 | 待定位 | format 后未重启就写，CLI 永久 busy（旧镜像残留） | K | 旧镜像残留/HardFault 现场，不可复现 |  |
| 79 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-14 | 待上板 | full rebuild 空闲块扫描从 64 页降到 1 页 | K | 性能优化项，非失败场景 |  |
| 80 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-15 | 待上板 | full rebuild → 下次 CP 快照失效 → 每次开机 rebuild | M | 已上板（2026-11-14 debug UT=FTL `test /ftl/media/negative/` 36/36、0 fail、200.10 s）；无需注入：format 后连续两次 reboot| 已写/未上板；seam: 无；触发: format→reboot→reboot；断言: boot type 稳定、无循环 rebuild；用例 boot_no_rebuild_loop |
| 81 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-16 | 待上板 | full rebuild 逐 CW/逐 region 日志刷屏拖慢启动 | K | 日志速率测量 |  |
| 82 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-17 | 待决策 | 空片首启可能拒绝自动格式化 | K | 需真空白片 |  |
| 83 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-18 | 待上板 | 单个 L2P region 不可读 ⇒ 全局回退全盘重建 | K | K-deferred：需单 region 读失败 + 快照引用窗口；未设计 | |
| 84 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-21 | 待上板 | 降级位图被误当成「禁止加载」开关 | M | 已上板（2026-11-14 debug UT=FTL `test /ftl/media/negative/` 36/36、0 fail、200.10 s）；先造降级 region 再写/flush/驱逐/重读| 已写/未上板；seam: FI_FC_STATUS(normal read) 造降级；触发: 写新数据+flush+neg_drop_l2p_cache；断言: 重读命中 Directory 而非合成 INVALID；用例 boot_degraded_reload |
| 85 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-22 | 待定位 | 格式化中断后可能拒绝格式化/启动 | K | 需 format 中途掉电 |  |
| 86 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-25 | 待定位 | 重启后整段覆盖写触发大量 dangling | K | K（需长压测）：dangling 归因需日志采集与长序列 | |
| 87 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-26 | 待上板 | 恢复统计 WORD 回绕导致快照被错误接受 | K | 需 65536 次失败才回绕，不可达 |  |
| 88 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-28 | 待上板 | block_words 隐式 in/out，恢复顺序只靠注释 | K | 结构/顺序约束，非失败场景 |  |
| 89 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-30 | 待上板 | Verify 报 1.3 MB 数据丢失，实为读路径瞬时错读（数据完好） | K | K（触发不可确定性构造）：依赖 RR-06 复现窗口，当前未复现 | |
| 90 | 2B 掉电恢复 / 启动路径 | RISK-MD-14 | 待上板 | 双副本账本/Anchor 原子性 | M | 已上板（2026-11-14 release UT=FTL `/ftl/media/negative/` 54/54、0 SKIP、219.32 s）；N：已写/未上板；破坏单份备份副本后 reboot；断言数据完好、未降级| 用例 md_backup_copy_corrupt |
| 91 | 2B 掉电恢复 / 启动路径 | RISK-MD-15 | 待上板 | 副本感知压缩（主/备逐副本搬移） | K | K（需长压测）：需满盘压缩与双副本搬移压力 | |
| 92 | 2B 掉电恢复 / 启动路径 | RISK-MD-16 | 待上板 | victim region 页读失败 ⇒ 压缩每轮同页中止 | K | K-deferred：需强制压缩 + victim 副本读失败；forced compaction 夹具未做 | |
| 93 | 2D Checkpoint | RISK-CP-01 | 待上板 | journal 预擦失败仍记 pre_erased_block，回切时跳过擦除 | K | K-deferred：需 CP truncate 相位定点控制 + 多 plane 擦失败；相位无法从公共 API 定点 | |
| 94 | 2D Checkpoint | RISK-CP-02 | 待上板 | checkpoint 回调缺 waiting 守卫，迟到完成会按当前 phase 重走 | K | 需重复/迟到完成，FC 引擎无注入 |  |
| 95 | 2D Checkpoint | RISK-CP-03 | 待上板 | 状态机改动未板验：错误传播 / 游标复位 / 水位条目化 | M | 已上板（2026-11-14 debug UT=FTL `test /ftl/media/negative/` 36/36、0 fail、200.10 s）；已写 cp_abort_phase/cp_dir_ckpt_abort；残余为掉电| seam: FI_CP_ABORT_PHASE；触发: 强制CP；断言: 中止相位/恢复；用例 cp_abort_phase + cp_dir_ckpt_abort（已写） |
| 96 | 2D Checkpoint | RISK-CP-04 | 待定位 | host 写流下 CP 提交饥饿，submit stalled 后 abort | K | K（触发不可确定性构造）：需 host 写流占满命令池；无池满注入 | |
| 97 | 2D Checkpoint | RISK-CP-05 | 待上板 | abort 被调两次，fail_phase 被覆盖为 CP_DONE + 日志刷屏 | M | 已写/已上板：cp_abort_phase 持续注入触发多次 abort，断言 fail_phase/timeout_phase 保持真实相位（非 CP_DONE）；日志限流未断言 | seam: FI_CP_ABORT_PHASE 持续；触发: 有脏区强制 CP；断言: fail_phase 保持首相位；用例 cp_abort_phase |
| 98 | 2D Checkpoint | RISK-CP-10 | 待上板 | T2 触发无 latch，写计数停在整数倍时 CP 空转 | K | K（需长压测）：需写满 N×1024 页的长序列 | |
| 99 | 2D Checkpoint | RISK-CP-11 | 待上板 | initial record 失败块写死 plane 0/1 | K | 需 PLANE_NUM>2 的构建，硬件不支持 |  |
| 100 | 2E 磨损计数 / 静态 WL | RISK-WL-01 | 待上板 | WL 擦完用 RETIRE，RETIRED 计入 erase 统计导致容量流失 | K | K-deferred：风险条目缓解/下一步为空，期望行为未定义；需先裁决 RETIRED 是否计入擦除统计 | |
| 101 | 2E 磨损计数 / 静态 WL | RISK-WL-02 | 待上板 | 擦除不重置 read_count，read disturb 跨周期累计被误退役 | M | 已上板（2026-11-14 debug UT=FTL `test /ftl/media/negative/` 36/36、0 fail、200.10 s）；擦除清 read_count、保留 ECC 趋势| seam: alc_bst_note_erase；触发: 置 read_count 后调用；断言: read_count==0 / erase_count+1 / rr.health 保留；用例 wl_erase_resets_read_count |
| 102 | 2E 磨损计数 / 静态 WL | RISK-WL-03 | 待上板 | Closed SB 含 BAD plane 仍被 WL 选中，migrate 永久卡死 | M | 已上板（2026-11-14 debug UT=FTL `test /ftl/media/negative/` 36/36、0 fail、200.10 s）；含 BAD plane 的 Closed 条目被跳过| seam: wl_select_victim；触发: Closed 条目 plane1 标坏；断言: 对照组可选、标坏后返回 false；用例 wl_skip_bad_plane |
| 103 | 2E 磨损计数 / 静态 WL | RISK-WL-04 | 待上板 | WL 收尾 FREE / read_count 复位 / BAD plane 过滤 / 磨损表校验未板验 | K | K-hard：需正常 WL/GC + 断电 + soak，属长时压测 | |
| 104 | 2E 磨损计数 / 静态 WL | RISK-WL-05 | 待后续 | wl_should_trigger 与 wl_select_victim 各做一次全表扫描 | K | 性能项（重复扫描） |  |
| 105 | 2F FTL↔FC 接口 | RISK-IF-01 | 待上板 | 写位图用 1U<<cw（plane 盲），迁移 plane 1 被填 0xFF | M | 已上板（2026-11-14 release UT=FC `/fc-nand/negative/` 9/9、0 skip、129 µs）（链级）；plane1 位图建链已断言，端到端 GC 迁移后逐 CW 读回未覆盖| seam: ftl_cmd_setup_ppa_pair_cw；触发: 仅 plane1 位图建链；断言: cmd->paa 与全链 desc 都在 plane1、CW 覆盖 0..3；用例 cw_write_plane1_only |
| 106 | 2F FTL↔FC 接口 | RISK-IF-02 | 待上板 | 同步等待超时后迟到完成解引用 NULL ctx + 槽泄漏 | K | 需超时后迟到完成（FC 无丢完成注入） |  |
| 107 | 2F FTL↔FC 接口 | RISK-IF-03 | 待上板 | 同步原语忽略数据搬运返回值，短链读静默返回 OK | M | 已上板（2026-11-14 release UT=FTL `/ftl/media/negative/` 54/54、0 SKIP、219.32 s）；N：已写/未上板；同 HOST-03（同步原语返回值检查）| 用例 short_chain_read_short |
| 108 | 2F FTL↔FC 接口 | RISK-IF-04 | 待上板 | plane1 迁移 / wbuf 稀疏页 / 越段位图 / 迟到完成未板验 | M | 已上板（2026-11-14 debug UT=FTL `test /ftl/media/negative/` 36/36、0 fail、200.10 s）（稀疏页）；越段位图未覆盖（release 拒绝分支无独立 seam）| seam: 无；触发: 同页写 LBA0/LBA2 后读 8 LBA；断言: 两 CW 数据正确、其余零填；用例 iface_sparse_page |
| 109 | 2F FTL↔FC 接口 | RISK-IF-06 | 待决策 | chain 派发路径不处理 RAID stripe encode | K | RAID 关闭（R5），无实际失效 |  |
| 110 | 2F FTL↔FC 接口 | RISK-IF-07 | 待上板 | 批次 chain 混优先级或末条 MORE 无 END 会错乱/锁死 | M | 已上板（2026-11-14 release UT=FC `/fc-nand/negative/` 9/9、0 skip、129 µs）；混优先级批被拒绝、队列占用不变| seam: fc_submit_batch；触发: 一条 chain=MORE 低优先级 + 一条高优先级；断言: INVALID_ARGUMENT、SQ/HSQ 不变；用例 batch_arg_guards |
| 111 | 2F FTL↔FC 接口 | RISK-IF-08 | 待上板 | page-read 聚合忽略 NAND 结果位，TIMEOUT 可静默返回陈旧数据 | K | 需 NAND 读超时+旧 buffer，引擎级注入未实现 |  |
| 112 | 2F FTL↔FC 接口 | RISK-IF-09 | 待上板 | 放弃序列只清 RAM 不发设备 LAST | K | 需主机中途拔线/中止，PC 侧手动 |  |
| 113 | 2F FTL↔FC 接口 | RISK-IF-11 | 待上板 | 逐页确认依赖完成顺序与 FAILC 语义 | M | cache_failc_abort 已注入中间页 FAILC 并上板通过 | 证据: tests/unit/test_ftl_media.c:test_cache_failc_abort（三档 arm_page） |
| 114 | 2F FTL↔FC 接口 | RISK-IF-12 | 待上板 | 确认待入账 × 序列门控可能退化到 1s idle 守卫 | K | K（需长压测）：需满盘大顺序写+序列门控，属长压测 | |
| 115 | 2F FTL↔FC 接口 | RISK-IF-15 | 待上板 | RAW 读判空把全 0xFF 用户区的已编程页当空白 | M | 已上板（2026-11-14 debug UT=FTL `test /ftl/media/negative/` 36/36、0 fail、200.10 s）；整 region TRIM→flush→reboot→重写| seam: 无；触发: 写数据→trim 512 LBA→flush→reboot→重写；断言: 数据一致、region 未降级；用例 blank_ff_trim_rewrite |
| 116 | 2F FTL↔FC 接口 | RISK-IF-16 | 待上板 | timeout_us 名不副实，实为自旋轮数 | K | 真实微秒 deadline 是行为修正，否定例 |  |
| 117 | 2F FTL↔FC 接口 | RISK-IF-17 | 待上板 | 超时后调用者释放仍在飞的命令，可能跨命令破坏 | K | 需提交后无完成，FC 无注入 |  |
| 118 | 2F FTL↔FC 接口 | RISK-IF-18 | 待上板 | 接口层 4 处 release 守卫缺口（H14） | N | 部分已上板：空位图（iface_pair_cmd_guards）+ 单 plane cache-op（iface_release_guards）在 debug/release 均过；跨 die pair 在本板 LUN_NUM=CH_NUM=CE_NUM=1 下不可构造（K 类）；sync_submit_wait 上限未覆盖 | seam: ftl_cmd_setup_ppa_pair_cw / ftl_async_write_pair_cmd；触发: 空位图、单 plane cache-op；断言: 返 FTL_ERROR 不越界；用例 iface_pair_cmd_guards + iface_release_guards |
| 119 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-01 | 待上板 | host 读成败只看 NAND_FAIL，UNC/MATA 被当成功 | M | 已写/已上板：DATA_ERROR 完成态进入 RR 并有界失败（rr_read_fail_bounded）；NAND_FAIL 被消费并触发重读（fc_status_nand_fail）；META_ERROR 未单验 | seam: FI_FC_STATUS(FI_KEY_OP(read), DATA_ERROR) + FI_RR_READ_FAIL；触发: host 读；断言: IO_ERROR / planes_recovered 增加；用例 rr_read_fail_bounded + fc_status_nand_fail |
| 120 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-02 | 待上板 | multi-plane 读只记首个 desc 的块，第二块漏记 ECC/read_count | M | 已上板（2026-11-14 debug UT=FTL `test /ftl/media/negative/` 36/36、0 fail、200.10 s）；multi-plane 读给两个 gblock 都记 read_count| seam: 无；触发: 8 LBA 读；断言: 两 gblock read_count 都 +1；用例 host_read_multiplane_stats |
| 121 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-03 | 待上板 | 短链读返回值被丢弃，未初始化缓冲交给 host | M | 已上板（2026-11-14 release UT=FTL `/ftl/media/negative/` 54/54、0 SKIP、219.32 s）；N：已写/未上板；ftl_cmd_read_data 链短于请求返回 FTL_ERROR| 用例 short_chain_read_short |
| 122 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-05 | 待定位 | confirm park timeout = host 写失败，上游三支未定 | 待定位 | 上游三支未定；需日志现场采集 |  |
| 123 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-06 | 待上板 | 写 program fail 直接返回 IO_ERROR，未重发 | M | 已上板（gc_batch_program_fail + data_write_fail_bounded）；用户裁决：cache-program 成员失败不重发，走 deferred error | |
| 124 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-07 | 待上板 | 停泊确认路径重入，oldp 陈旧/映射分叉 | K | K（触发不可确定性构造）：需 Journal 页满+停泊确认同窗口 | |
| 125 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-08 | 待上板 | TRIM 已异步；CLI 诊断与 CP 冷启动 locate 仍同步 | K | 同步点是控制面/冷启动，非失败场景 |  |
| 126 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-09 | 待后续 | TRIM 命中 wbuf 未编程 LBA 时不失效旧映射 | K | 当前串行化下不可达 |  |
| 127 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-11 | 待定位 | Journal 压力下 TRIM 持久化可能丢失 | K | K（触发不可确定性构造）：需大量写映射后 trim 的特定序列 | |
| 128 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-12 | 待议 | 组槽耗尽返回 FTL_ERROR；修法在 controller | M | 已上板（2026-11-14 release UT=FTL `/ftl/media/negative/` 54/54、0 SKIP、219.32 s）；N：已写/未上板；组表耗尽返回 FTL_AGAIN（H38）；代码已改| 用例 host_group_exhaustion |
| 129 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-14 | 待上板 | 拔线后门控环形等待导致读回 MATA（根因已改，死锁路径未验） | K | 需拔线+满盘 GC 窗口，人工触发 |  |
| 130 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-16 | 待上板 | 「无进展时长」预算实际未生效（死赋值） | K | K（触发不可确定性构造）：需 park timeout 与后台进展同窗口 | |
| 131 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-17 | 待上板 | host queue entry 生命周期，漏 release/重复 release | M | 已上板：HIL 15/15 每例 teardown 断言三池/队列/request 表归零（漏 release 会红）；在飞释放由 in_flight_release_guard 覆盖；重复 release 是 fc_command_release 的幂等早退（§四 有意设计） | seam: hil_quiesce（fc_get_resource_stats + ftl_op_req_count）；触发: 每例结束；断言: 全部计数归零；用例 HIL 15 例 teardown |
| 132 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-22 | 待上板 | host 写取不到页对时无界重试，USB 命令可能永久挂起 | K | K（触发不可确定性构造）：需 free+OP 同时耗尽；无注入点 | |
| 133 | 2H 完成派发面（`ftl_op.c/h`） | RISK-CR-01 | 待上板 | ctx 旁表不按 command_id 去重，id 复用得别人 ctx | K | 需 CQ 满/丢完成触发 id 复用 |  |
| 134 | 2H 完成派发面（`ftl_op.c/h`） | RISK-CR-04 | 待上板 | request 表改 command_id 直接索引，删 op_tag | M | 已上板（2026-11-14 release UT=FTL 全量 39/39、0 skip、186.77 s）；越界拒绝 + 重复登记不双计数| seam: ftl_op_req_add/remove；触发: cmd_id == FTL_OP_REQ_TABLE_SIZE、同 id 加两次；断言: FTL_ERROR / count 不增；用例 op_req_table_guard |
| 135 | 2H 完成派发面（`ftl_op.c/h`） | RISK-CR-05 | 待上板 | pending 表 command_id 直接索引 | M | 已上板（2026-11-14 release UT=FTL 全量 39/39、0 skip、186.77 s）；越界 id 与释放后查找；双绑定 release 无拒绝（仅 debug ASSERT）| seam: pend_find/pend_alloc/pend_remove；触发: 越界 id、分配→摘除→再查；断言: NULL 且不越界写；用例 pend_table_guard |
| 136 | 2H 完成派发面（`ftl_op.c/h`） | RISK-FC-05 | 待定位 | 后台双 plane 页写留 SQ 头不发，teardown ctx 残留 | 待定位 | 依赖前序组合状态，复现条件已隔离；原因未证实 |  |
| 137 | 2I 页打包写缓冲 wbuf（`wbuf.c` + `wbuf.h`） | RISK-WB-01 | 待上板 | FLUSH 失败丢弃页对，allocator 无归还 API，容量无声流失 | M | 已上板（2026-11-14 debug UT=FTL `test /ftl/media/negative/` 36/36、0 fail、200.10 s）；free 账本断言已加进 data_write_fail_bounded| seam: FI_MIG_WRITE_FAIL；触发: 5s 重试；断言: free_count 不随重试流失；用例 data_write_fail_bounded（已写） |
| 138 | 2I 页打包写缓冲 wbuf（`wbuf.c` + `wbuf.h`） | RISK-WB-02 | 待上板 | TRIM/RESET 由外部模块手工改 wbuf 内部不变量 | K | K（主机侧矩阵）：bbt --erase/RESET 走 CLI，UT 未接入 | |
| 139 | 2I 页打包写缓冲 wbuf（`wbuf.c` + `wbuf.h`） | RISK-WB-03 | 待上板 | 模块抽出 + 位运算收敛 + 日志降级，wbuf 是写路径核心 | K | 模块抽出/等价改写，非失败场景 |  |
| 140 | 2I 页打包写缓冲 wbuf（`wbuf.c` + `wbuf.h`） | RISK-WB-04 | 待上板 | 组在飞数原由多处手工维护，漏减挂死 host 写 | K | 收敛为唯一闸门，属结构项 |  |
| 141 | 2I 页打包写缓冲 wbuf（`wbuf.c` + `wbuf.h`） | RISK-WB-07 | 待定位 | wbuf 半页卡死，后续写全被拒、组永不推进 | 待定位 | depth2 半页卡死未定位 |  |
| 142 | 2I 页打包写缓冲 wbuf（`wbuf.c` + `wbuf.h`） | RISK-WB-08 | 待上板 | WBUF_STATE_FAILED 不可达，错误锁存契约与实现矛盾 | K | K（用户裁决）：删除不可达 WBUF_STATE_FAILED；错误经 base.error 上报、flush 超时兜底，无独立失败模式 | |
| 143 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | RISK-BG-01 | 待上板 | 时钟缺失时 RR 巡检门恒假，预测巡检永久停摆 | 待定位 | 待定位：风险描述与当前实现不符（已无 rr_last_scan 门），需重审该风险条目 | |
| 144 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | RISK-BG-04 | 待上板 | 任务表装载无边界检查，加项越界踩 task_count | K | 表容量由 _Static_assert 编译期保证，运行期不可构造 |  |
| 145 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | RISK-BG-05 | 待上板 | 调度器死字段 + priority 与表序两套坐标漂移 | K | 删死字段/坐标收敛，非失败场景 |  |
| 146 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | RISK-BG-06 | 待上板 | l2p_flush_max 经 level 反查，第二套配置读取 | K | K（触发不可确定性构造）：需强制 BUSY 档并读 flush 条数，档位选择不可控 | |
| 147 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | RISK-BG-07 | 待上板 | journal_erase 手工读 journal 内部 phase/need_erase | K | K（触发不可确定性构造）：需 journal 换块路径；触发未证明 | |
| 148 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | RISK-BG-10 | 待上板 | 后台节拍 flag + token bucket（grant 太小/太大风险） | K | K（需长压测）：token bucket/节拍是吞吐行为，需长压测 | |
| 149 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | RISK-BG-11 | 待上板 | 档位下标无值域断言，越界静默读相邻数据 | K | 档位越界属编程错误，入口 ASSERT 已挡 |  |
| 150 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | RISK-DOC-03 | 待上板 | ftl.c 注释/代码分叉（H6/H37） | K | 注释/断言同步，非失败场景 |  |
| 151 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | RISK-LOG-02 | 待上板 | GC 事件在极端 victim 速率下仍可能占 UART | K | 日志速率测量 |  |
| 152 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | RISK-LOG-03 | 待上板 | GC no-gain 刷屏（边沿复用） | K | 日志速率测量 |  |
| 153 | 2L FTL 模块 handle 契约 | RISK-ARCH-02 | 待后续 | 跨 LUN superblock 条带 / 跨 LUN GC/WL 未实现 | K | 跨 LUN 条带需多实例硬件 |  |
| 154 | 2L FTL 模块 handle 契约 | RISK-ARCH-05 | 待上板 | 独立 confirm_handle 未采用，confirm/TRIM 两处顺序实现 | K | 设计判定（不采用独立 handle） |  |
| 155 | 2L FTL 模块 handle 契约 | RISK-ARCH-07 | 待上板 | mig/wbuf/rr_page_scratch 改挂 ftl_storage_t，数组落 AXI | K | 内存域布局项 |  |
| 156 | 2L FTL 模块 handle 契约 | RISK-ARCH-08 | 待上板 | 提交/等待计数改绝对 ms deadline | M | 已写/已上板（第一批 9/9 含 cp_abort_phase、rr_read_fail_bounded）；10ms/1s deadline 生效 | seam: 已有 cp_abort_phase/rr_read_fail_bounded；触发: 持续提交失败；断言: deadline 内中止；用例 cp_abort_phase + rr_read_fail_bounded |
| 157 | 2L FTL 模块 handle 契约 | RISK-ARCH-09 | 待上板 | PFR 收为 boot orchestrator，内部原语全部 static | K | K（触发不可确定性构造）：需 fresh/clean/PF 四条启动路径回归 | |
| 158 | 2L FTL 模块 handle 契约 | RISK-ARCH-11 | 待上板 | 端口删除后模块直接持有 owner 指针 | K | 接口封装形态，非失败场景 |  |
| 159 | 2M USB MSC / 平台 | RISK-PLAT-01 | 待上板 | `.noncacheable_buffer` 段匹配漏前导点，页缓冲落进可缓存 AXI RAM | K | 需 CONFIG_NAND_DMA=1 构建，非目标默认档 |  |
| 160 | 2M USB MSC / 平台 | RISK-PLAT-07 | 待上板 | 各构建配置共用 debug.elf，UT 互相覆盖 | K | 构建产物命名，非负例 |  |
| 161 | 2M USB MSC / 平台 | RISK-PLAT-08 | 待定位 | `.noinit` 出现一次 HardFault（PC 落 newlib printf） | 待定位 | 一次性 HardFault，故障镜像未定 |  |
| 162 | 2M USB MSC / 平台 | RISK-USB-02 | 待上板 | MSC 异步完成/deferred/discard 路径未实测 | K | 需主机拔线/复位，PC 侧手动 |  |
| 163 | 2M USB MSC / 平台 | RISK-USB-05 | 待上板 | WRITE(10) FUA 原被忽略（已实现等确认后回执） | K | 需自定义 SCSI FUA 工具 |  |
| 164 | 2M USB MSC / 平台 | RISK-USB-07 | 待上板 | request->io 模板的 cmd_remaining 跨命令残留 | K | K（主机侧矩阵）：cmd_remaining 在 usb_msc.c 内设置；HIL 只到 controller，无法从请求对象断言跨命令残留；由 PC 主机侧矩阵覆盖 | |
| 165 | 2M USB MSC / 平台 | RISK-USB-08 | 待上板 | 写 lookahead 提前回执/BUSY 背压未注入失败 | K | 需主机早期回执+程序失败组合 |  |
| 166 | 2M USB MSC / 平台 | RISK-USB-09 | 待上板 | fsync 被自己的在飞写槽拒掉（已加 FLUSH 专用槽） | K | K（白盒/夹具不可达）：fsync/slot 逻辑在 usb_msc.c（static），HIL 不到 MSC 层；写后 flush 已由 write_flush_readback 覆盖，但「在飞写槽不拒 flush」无法从 HIL 构造 | |
| 167 | 2M USB MSC / 平台 | RISK-USB-10 | 待上板 | slot_match 把真正的下一条相同写当事件重放丢弃 | K | K（白盒/夹具不可达）：slot_match 为 usb_msc.c 的 static；HIL 无法构造 SCSI 重复写事件重放场景 | |
| 168 | 2M USB MSC / 平台 | RISK-USB-11 | 待上板 | TinyUSB 本地补丁清单不完整（cbw_info 钩子） | K | 文档补丁清单，非负例 |  |
| 169 | 2M USB MSC / 平台 | RISK-USB-12 | 待上板 | SYNCHRONIZE CACHE(16) 0x91 未支持 | K | 需主机发 0x91，PC 侧验证 |  |
| 170 | 2M USB MSC / 平台 | RISK-USB-13 | 待上板 | 两写槽占用时 READ 返回 ERROR 而非 BUSY | K | K（触发不可确定性构造）：需两写槽在飞时 READ；槽状态构造未证明 | |
| 171 | 2M USB MSC / 平台 | RISK-USB-14 | 待上板 | SCSI UNMAP 已实现，未上板 | K | 需主机 blkdiscard/fstrim |  |
| 172 | 2M USB MSC / 平台 | RISK-USB-15 | 待决策 | UNMAP 未向主机广告，fstrim 可能不下发 | 待决策 | 是否广告 UNMAP 未定 |  |
| 173 | 2M USB MSC / 平台 | RISK-USB-16 | 待上板 | UNMAP descriptor 按 8B 步进（规范 16B）已改 | K | K（白盒/夹具不可达）：需直调 usb_msc_unmap_parse；若为 static 则不可测 | |
| 174 | 2O NAND / FMC 时序与吞吐 | RISK-NAND-05 | 待定位 | used 介质不可纠读页，L2P 仍指向坏块，每次读重试 8 次 | K | 需芯片物理坏页 |  |
| 175 | 2O NAND / FMC 时序与吞吐 | RISK-NAND-06 | 待定位 | 运行期 BAD 标记来源未定：Mode 5 时序 / 同页二次编程 / 边缘块 | 待定位 | BAD 来源未定，需板端复现 |  |
| 176 | 2P allocator | RISK-ALLOC-09 | 待上板 | 低 free 时成对分配失败导致写路径永久 stall（plane 不平衡） | K | K（需长压测）：需 plane 不平衡的低 free 长压测 | |
| 177 | 2P allocator | RISK-ALLOC-06 | 待后续 | `ftl_gblock` 无入参契约，哨兵可截断成合法 gblock | K | 契约断言属死亡测试，UT 无机制 |  |
| 178 | 2P allocator | RISK-ALLOC-12 | 待上板 | 出池后 state 仍为 FREE，漏赋新态时块永久漏账 | 待测试源 | 生产已改；test_free_pool 仍断言旧语义（R4 用户维护） |  |
| 179 | 2P allocator | RISK-ALLOC-13 | 待上板 | `alc_super_open_rewind_pair` 无下溢守卫，`next_page--` 掩码成页 63 | M | 已上板（2026-11-14 release UT=FTL 全量 39/39、0 skip、186.77 s）（release-only）；next_page==0 时 rewind 不下溢| seam: alc_super_open_rewind_pair；触发: next_page=0 调用；断言: next_page 保持 0；用例 alloc_rewind_underflow |
| 180 | 2P allocator | RISK-ALLOC-15 | 待上板 | 全 plane BAD 的 SB 关闭时追加零 plane 条目，列表槽泄漏 | M | 已上板（2026-11-14 release UT=FTL `/ftl/media/negative/` 54/54、0 SKIP、219.32 s）；N：已写/未上板；两 plane 全 BAD 关闭不产生零 plane 条目| 用例 alloc_close_bad_pair_no_entry |
| 181 | 2P allocator | RISK-ALLOC-16 | 待上板 | `alc_spare_pop` 不跳 BAD，坏块替换假报成功并丢 spare | M | 已上板（2026-11-14 debug UT=FTL `test /ftl/media/negative/` 36/36、0 fail、200.10 s）；Spare 栈顶 BAD 被跳过、返回好块| seam: alc_spare_pop；触发: 栈顶块标 BAD；断言: 返回下一好块、count 递减；用例 alloc_spare_skip_bad |
| 182 | 2P allocator | RISK-ALLOC-03 | 待上板 | 纯转发访问器与 `alloc->blocks[]` 直接访问形成两套坐标 | K | 访问器坐标收敛，非失败场景 |  |
| 183 | 2P allocator | RISK-ALLOC-07 | 待上板 | `ftl_tables_t` 五数组打包，单表无法换内存域 | K | 内存域布局项 |  |
| 184 | 2P allocator | RISK-ALLOC-08 | 待上板 | `alc_super_close_live` 吞 append 失败码，Closed 列表满时漏账 | M | 已上板（2026-11-14 release UT=FTL `/ftl/media/negative/` 54/54、0 SKIP、219.32 s）；N：已写/未上板；列表满时 alc_super_close 返回 FTL_ERROR、live 保持| 用例 alloc_closed_list_full |
| 185 | 2R 位运算公共宏 | RISK-BIT-01 | 待后续 | 通用掩码构造器挂在 NAND 层，FTL/FC 各自另写字面量宏 | K | 宏归属/代码组织项 |  |
| 186 | 2V 板端读完整性事件 | RISK-RD-01 | 待定位 | 单次不可纠读，状态 `CRC\ | 待定位 | 单次 CRC|MATA 事件，需复现与映射链路定位 |  |
| 187 | 2W pending 表 / 完成组 | RISK-HOST-15 | 待上板 | 写确认 handler 用文件级全局 + `void *`，UT 需 save/restore | K | 确认 handler 全局收口属结构项，非失败场景 |  |
| 188 | 2Z cache 长流 + Journal 顺序映射页 | RISK-CACHE-15 | 待上板 | 无数据 END 缺失，padding 页盘满例外未验 | K | K（触发不可确定性构造）：盘满例外需 free 耗尽窗口 | |
| 189 | 2Z cache 长流 + Journal 顺序映射页 | RISK-CACHE-16 | 待上板 | 跨命令序列 + VWC 早回执，USB 端到端未跑 | K | 需 USB 主机端到端，PC 侧矩阵 |  |
| 190 | 2Z cache 长流 + Journal 顺序映射页 | RISK-CACHE-18 | 待上板 | 提前回执扩大掉电暴露窗口，VWC 窗口未量化 | K | VWC 窗口需掉电量化 |  |
| 191 | 2Z cache 长流 + Journal 顺序映射页 | RISK-CACHE-23 | 待上板 | 持续写期间后台 CP/L2P 被序列门控饿死 | M | 让行机制 2026-11-14 板端满盘写验证：GC abort=0、无 l2p load timeout | 证据: 板端 H2testw 满盘 + RISK-CACHE-23 历史 |
| 192 | 2Z cache 长流 + Journal 顺序映射页 | RISK-CACHE-26 | 待定位 | cache 写压测 1536 LBA 读回全 0 | 待定位 | 1536 LBA 读回全 0 未定位，需压测复现 |  |
| 193 | 2Z cache 长流 + Journal 顺序映射页 | RISK-CACHE-27 | 待上板 | 早回执后读路径缺覆盖层，写完立刻读回旧数据 | M | cache_early_ack_read_visible 已板端通过 | 证据: tests/unit/test_ftl_media.c:test_cache_early_ack_read_visible |
| 194 | 2Z cache 长流 + Journal 顺序映射页 | RISK-CACHE-28 | 待上板 | FAILC 撤销按 region 粒度降级，跨重启丢失 | M | cache_failc_abort 三档注入板端通过（失败页读 IO_ERROR） | 证据: tests/unit/test_ftl_media.c:test_cache_failc_abort |
| 195 | 2Z cache 长流 + Journal 顺序映射页 | RISK-CACHE-29 | 待上板 | 早回执改变 flush/断电口径，长流+断电未量 | K | K（需人工掉电窗口）：长流+掉电窗口未量化 | |
| 196 | 2Z cache 长流 + Journal 顺序映射页 | RISK-CACHE-30 | 待上板 | padding 收尾把已证明未确认页误判失败 | K | K（触发不可确定性构造）：需 padding 在飞 + 已证明未确认页窗口 | |
| 197 | 2Z cache 长流 + Journal 顺序映射页 | RISK-CACHE-31 | 待上板 | padding 回调早退分支泄漏 FC 命令 | K | K（触发不可确定性构造）：需成员失败 + pad 在飞后泄漏计数；构造未证明 | |
| 198 | 2Z cache 长流 + Journal 顺序映射页 | RISK-CACHE-32 | 待上板 | 环满在 release 下静默丢已回执写 | K | 环深 16 远大于在飞窗口，结构不可达 |  |
| 199 | 2Z cache 长流 + Journal 顺序映射页 | RISK-CACHE-36 | 待上板 | 干净释放写序列时不看在飞成员，迟到完成把未证明页推进死序列 | K | K（触发不可确定性构造）：需组级失败且全部成员在飞的窗口 | |
| 200 | 2Z cache 长流 + Journal 顺序映射页 | RISK-USBC-01 | 待上板 | deferred error 以下一条命令失败近似，非标准 sense | K | deferred sense 语义需主机侧观察 |  |
| 201 | 2Z cache 长流 + Journal 顺序映射页 | RISK-USBC-03 | 待定位 | USB 盘间歇报无介质，主机侧已止血 | K | 主机侧采样/盘符问题，需 PC 侧 |  |
| 202 | 2AA 平台定时调度器（`timer`） | RISK-PLAT-09 | 待上板 | SysTick 时基与 TIM6 调度心跳的超时判据未板验 | K | K（需长压测）：时基行为需板端长跑；UT 只能测伪时钟 | |
| 203 | 2AA 平台定时调度器（`timer`） | RISK-PLAT-10 | 待上板 | mig_eval 混用 TIM6 与 SysTick 两套时间坐标 | K | K（触发不可确定性构造）：time 坐标混用需伪时钟+调度对拍，未设计 | |
| 204 | 2AA 平台定时调度器（`timer`） | RISK-PLAT-12 | 待上板 | µs 时间戳从 DWT 改 SysTick，未板验 | K | K（触发不可确定性构造）：DWT→SysTick 需板端对比 | |
| 205 | 2AB NAND 拓扑扫描（`nand_target_probe` / `ftl_topology_build`） | RISK-TOP-01 | 待上板 | 扫描得到的多 target / 多 LUN 拓扑路径未板验 | K | 多 target/多 LUN 硬件不具备 |  |
| 206 | 2AB NAND 拓扑扫描（`nand_target_probe` / `ftl_topology_build`） | RISK-TOP-02 | 待上板 | sparse 拓扑下 controller FLUSH 按 3D 槽遍历会空指针 | K | sparse 拓扑需多 die 硬件 |  |
| 207 | 2AB NAND 拓扑扫描（`nand_target_probe` / `ftl_topology_build`） | RISK-TOP-03 | 待上板 | LUN 数按器件探测，ONFI 偶发失败即拒绝启动 | K | ONFI 偶发失败需器件/注入 |  |
| 208 | 2AC CLI 调试前端（`controller/cli.c`） | RISK-CLI-01 | 待上板 | 长命令内联执行阻塞主循环，IWDG 启用后会复位 | K | IWDG 未启用（待决策），无法验证看门狗 |  |
| 209 | 2AC CLI 调试前端（`controller/cli.c`） | RISK-CLI-02 | 待上板 | format/bbt erase 不检查 host 在飞，会造成介质状态分叉 | K | K（主机侧矩阵）：需 CLI+USB 并发操作，主机侧 | |
| 210 | 2AC CLI 调试前端（`controller/cli.c`） | RISK-CLI-03 | 待上板 | wp 复用静态缓冲无在飞守卫，重入会写错字节 | K | K（主机侧矩阵）：需连续 wp；CLI 交互 | |
| 211 | 2AC CLI 调试前端（`controller/cli.c`） | RISK-CLI-04 | 待上板 | release help 宣传被禁用的 bbt 子命令 | K | help 文本需串口人工核对 |  |
| 212 | 2AC CLI 调试前端（`controller/cli.c`） | RISK-CLI-05 | 待上板 | rd/bbt 语法变更，旧脚本会报 unknown option | K | 语法变更需人工点测 |  |
| 213 | 2AC CLI 调试前端（`controller/cli.c`） | RISK-CLI-06 | 待上板 | q/cp/jrn/stat/id/ecc/wear 字段口径未板端核对 | K | 观测字段需人工对拍 |  |
| 214 | 2AC CLI 调试前端（`controller/cli.c`） | RISK-CLI-07 | 待上板 | rd 失败仍打印 rd ok 并 dump 旧缓冲 | K | K（触发不可确定性构造）：需制造读失败并抓串口输出 | |
| 215 | 2AC CLI 调试前端（`controller/cli.c`） | RISK-CLI-08 | 待上板 | wp 绕过 host gate，可与 USB/CLI 并发进入 FTL | K | K（主机侧矩阵）：需 wp 与 USB 并发，主机侧 | |
| 216 | 2AD Controller 请求拆分（`request.c` / `controller.c`） | RISK-CTRL-01 | 待上板 | 多 run 请求中途失败时父请求提前完成、错误被覆盖 | K | 单 die 仅 1 run；多 run 需多 die |  |
| 217 | 2AD Controller 请求拆分（`request.c` / `controller.c`） | RISK-CTRL-03 | 待决策 | `ssd_controller_init` 稀疏拓扑注释与实现相反 | 待决策 | 注释/实现二选一，需裁决 |  |
| 218 | 2AD Controller 请求拆分（`request.c` / `controller.c`） | RISK-CTRL-04 | 待后续 | HIL submit 未校验 buffer NULL，FTL 兜底但状态分类错误 | 待决策 | buffer NULL 需先改 controller（用户已裁决留以后） |  |
| 219 | 2AE HIL 主机前端 / host queue | RISK-HOST-20 | 待决策 | HIL host 前端互斥不是单一闸门，同一不变量 5 处手工维护 | 待决策 | host gate 单一 claim 需设计裁决 |  |
| 220 | 2AE HIL 主机前端 / host queue | RISK-HOST-21 | 待后续 | HIL 没有 host 命令超时 / 中止 | 待后续 | HIL 缺 host 命令超时，需设计 |  |
| 221 | 2AF FC 资源池 / 生命周期 | RISK-POOL-01 | 待上板 | buffer 归属表缺失，双释放 / 在飞释放无运行期判据 | M | 已上板（2026-11-14 release UT=FC `/fc-nand/negative/` 9/9、0 skip、129 µs）（在飞释放）；buffer 双释放是 fc_command_release 幂等早退（§四 有意设计，不构成负例）| seam: fc_submit→fc_command_release；触发: 提交后立刻释放；断言: release 拒绝、command_used 不变；用例 in_flight_release_guard |
| 222 | 2AF FC 资源池 / 生命周期 | RISK-POOL-03 | 待上板 | desc 链释放 / 遍历缺上界与环检测 | K | K（白盒/夹具不可达）：需构造成环 desc 链（白盒） | |
| 223 | 2AF FC 资源池 / 生命周期 | RISK-POOL-04 | 待上板 | 空闲链哨兵混用裸常量与具名 ID | K | 哨兵命名/常量收敛，非失败场景 |  |
| 224 | 2AF FC 资源池 / 生命周期 | RISK-POOL-05 | 待上板 | 资源统计靠三池全扫，计数可能漂移 | M | 已上板（2026-11-14 release UT=FC `/fc-nand/negative/` 9/9、0 skip、129 µs）；分配 +1 / 释放回落、desc/buffer 不变| seam: fc_get_resource_stats；触发: 分配/释放命令；断言: command_used 差分一致；用例 stats_consistency |
| 225 | 2AF FC 资源池 / 生命周期 | RISK-POOL-06 | 待上板 | 单字段双语义，失败坐标字段无读者 | K | 字段拆分，非失败场景 |  |
| 226 | 2AF FC 资源池 / 生命周期 | RISK-POOL-07 | 待上板 | 内部 API 后缀 / 命名不合 H49 | K | 命名规范项 |  |
| 227 | 2AF FC 资源池 / 生命周期 | RISK-POOL-08 | 待上板 | FC 注释承载变更史与版本号 | K | 注释项 |  |
| 228 | 2AG 数据面 / NAND dispatch | RISK-DP-01 | 待上板 | ECC 引擎不可用被归一为成功，写不编码、读不校验 | K | 需 ECC 初始化失败注入，引擎无 seam |  |
| 229 | 2AG 数据面 / NAND dispatch | RISK-DP-02 | 待上板 | 数据路径编码错误被调用方丢弃，ECC 失败仍提交 NAND | K | 需 encode 失败注入 |  |
| 230 | 2AG 数据面 / NAND dispatch | RISK-DP-03 | 待上板 | INVALID buffer 进入数据路径，release 下越界/空指针 | K | K（白盒/夹具不可达）：需构造 desc 指向 INVALID buffer 的原始命令；FC 夹具未提供该构造 | |
| 231 | 2AG 数据面 / NAND dispatch | RISK-DP-04 | 待上板 | desc 链遍历无上界，成环会死循环 | K | K（白盒/夹具不可达）：需成环 desc 链（白盒） | |
| 232 | 2AG 数据面 / NAND dispatch | RISK-DP-05 | 待上板 | RAID 边界不完整，恢复后丢 CRC/META | K | RAID 关闭（R5） |  |
| 233 | 2AG 数据面 / NAND dispatch | RISK-DP-07 | 待上板 | 链式失败生命周期半状态，整链完成时断言 ACTIVE | K | 需 encode/build 失败注入 |  |
| 234 | 2AG 数据面 / NAND dispatch | RISK-DP-08 | 待上板 | 通道轮询注释称 round-robin，实际每轮从 CH0 开始 | K | 需多通道硬件 |  |
| 235 | 2AG 数据面 / NAND dispatch | RISK-DP-09 | 待上板 | FC 内 engine↔dispatch 双向依赖，armed 闩锁泄漏进引擎 | K | engine/dispatch 契约，非失败场景 |  |
| 236 | 2AG 数据面 / NAND dispatch | RISK-DP-10 | 待上板 | dispatch enqueue 入口检查只有 debug ASSERT | M | 已上板（2026-11-14 release UT=FC `/fc-nand/negative/` 9/9、0 skip、129 µs）；直调 enqueue 非法入参| 已写/未上板；seam: fc_nand_dispatch_enqueue；触发: NULL/count=0/越界route/跨target链；断言: release 返回 INVALID_ARGUMENT；用例 dispatch_enqueue_guards |
| 237 | 2AG 数据面 / NAND dispatch | RISK-DP-11 | 待上板 | dispatch 忽略 µs 时钟失败，LUN 永久停在 ISSUED | K | 需 µs 时钟失败注入 |  |
| 238 | 2AG 数据面 / NAND dispatch | RISK-DP-12 | 待上板 | 单 plane cache 读失败只标最低位 CW | K | K（白盒/夹具不可达）：需 FC 缓存读命令级构造（夹具的读 helper 内部断言成功），未设计 | |
| 239 | 2AG 数据面 / NAND dispatch | RISK-DP-13 | 待上板 | ecc_bit_count 无生产者，恒为 0 | K | 需可纠正位翻转，介质不可控 |  |
| 240 | 2AG 数据面 / NAND dispatch | RISK-DP-14 | 待上板 | ECC 枚举 0 命名为 BCH，运行时模式与实现不一致 | K | 枚举命名项 |  |
| 241 | 2AG 数据面 / NAND dispatch | RISK-DP-15 | 待后续 | LDPC 分支引用不存在的 g_qc，切构建即编译失败 | K | LDPC 未启用 |  |
| 242 | 2AG 数据面 / NAND dispatch | RISK-DP-16 | 待上板 | dispatch poll 用 BYTE 返完成数，256 回绕为 0 | K | 需多通道×CE×LUN=256 配置 |  |
| 243 | 2AG 数据面 / NAND dispatch | RISK-DP-17 | 待上板 | JUMP_IF_NOT_READY 回跳减法无 release 下溢守卫 | K | UOP 表静态正确，需改表构造 |  |
| 244 | 2AG 数据面 / NAND dispatch | RISK-DP-18 | 待上板 | random read UOP 缺 CHECK_FAIL，中间 CW 失败被吞 | K | 需引擎执行中注入 FAIL（非 CQ 状态） |  |
| 245 | 2AG 数据面 / NAND dispatch | RISK-DP-19 | 待上板 | 单 plane cache 读 UOP 缺 CHECK_FAIL | K | 同上 |  |
| 246 | 2AG 数据面 / NAND dispatch | RISK-DP-20 | 待上板 | cache 编程首页 FAILC 误报，误判好页失败 | K | K（触发不可确定性构造）：需残留 FAILC + 新批首页的时序窗口 | |
| 247 | 2AG 数据面 / NAND dispatch | RISK-DP-21 | 待上板 | 参数页/唯一 ID/GET_FEATURES 的 FAIL 被当成成功 | K | 需参数页 FAIL 注入（引擎级） |  |
| 248 | 2AG 数据面 / NAND dispatch | RISK-DP-22 | 待上板 | WAIT_READY 后端阻塞，引擎协作让出路径未生效 | K | WAIT_READY 协作让出需多 LUN 观察 |  |
| 249 | 2AG 数据面 / NAND dispatch | RISK-DP-23 | 待上板 | fc_nand_start 依赖调用者预绑定 engine 指针，契约未声明 | M | 已上板（2026-11-14 release UT=FC `/fc-nand/negative/` 9/9、0 skip、129 µs）；零初始化 txn 调 start| 已写/未上板；seam: fc_nand_start；触发: 不绑定 request/completion；断言: ASSERT_REJECT/INVALID；用例 nand_start_unbound_txn |
| 250 | 2AG 数据面 / NAND dispatch | RISK-DP-24 | 待上板 | 高速模式失败后没有回滚到低速 | K | 需 HAL_NAND_Init 失败注入 |  |
| 251 | 2AG 数据面 / NAND dispatch | RISK-DP-25 | 待上板 | dispatch 不校验 req/completion 表项非空，空指针崩溃 | M | 已上板（2026-11-14 release UT=FC `/fc-nand/negative/` 9/9、0 skip、129 µs）；提交未使用/保留 op| 已写/未上板；seam: fc_nand_dispatch_enqueue；触发: op 表项为空；断言: debug 停机/release 拒绝；用例 dispatch_enqueue_guards |
| 252 | 2AG 数据面 / NAND dispatch | RISK-DP-26 | 待上板 | 带 DATA_OUT 的稀疏 CW 读首输出读错 CW | K | K-deferred：需 FC 稀疏 CW + WITH_DATA_OUT 的数据通路夹具；未做 | |
| 253 | 2AG 数据面 / NAND dispatch | RISK-DP-27 | 待后续 | LDPC 解码栈占用 4.2KB，超 2KiB 栈预算 | K | LDPC 未启用 |  |
| 254 | 2AG 数据面 / NAND dispatch | RISK-DP-28 | 待后续 | CRC 初始化失败在 release 下静默，校验恒通过 | K | 需 CRC 初始化失败注入 |  |
| 255 | 2AH FC 主循环 / 调度 / CQ | RISK-SCH-01 | 待上板 | 链成员跨目标只在 debug 拦截，release 仍按链头 LUN 入队 | M | 已上板（2026-11-14 release UT=FC `/fc-nand/negative/` 10/10）；release 预检 + 单 target 板用 ch 越界值模拟 | |
| 256 | 2AH FC 主循环 / 调度 / CQ | RISK-SCH-02 | 待上板 | fc_core_poll 只在无完成时填充，持续完成下提交延迟 | K | 提交延迟行为，需持续完成压测 |  |
| 257 | 2AH FC 主循环 / 调度 / CQ | RISK-SCH-03 | 待决策 | CQ 满会丢完成项，FTL 永远收不到命令 | 待决策 | CQ 满二选一需裁决 |  |
| 258 | 2AH FC 主循环 / 调度 / CQ | RISK-SCH-05 | 待上板 | fill_pops 少计链成员 pop，链长 N 少计 N-1 | K | debug 计数字段，非失败场景 |  |
| 259 | 2AH FC 主循环 / 调度 / CQ | RISK-SCH-06 | 待上板 | 断言终端路径再次断言，递归覆盖原始现场 | K | 断言递归需制造池账分叉，风险高 |  |
| 260 | 2AH FC 主循环 / 调度 / CQ | RISK-SCH-07 | 待上板 | fc_submit_batch 不拒绝批内重复命令指针 | M | 已上板（2026-11-14 release UT=FC `/fc-nand/negative/` 9/9、0 skip、129 µs）；批内重复 ID 整批拒绝| seam: fc_submit_batch；触发: 同一 cmd_id 两次；断言: INVALID_ARGUMENT、不入队；用例 batch_arg_guards |
| 261 | 2AH FC 主循环 / 调度 / CQ | RISK-SCH-08 | 待上板 | fc_get_cmd_data_ptr 的 cw_index 不含 plane | M | 已上板（2026-11-14 release UT=FC `/fc-nand/negative/` 9/9、0 skip、129 µs）；multi-plane 命令查询数据指针返回 NULL| seam: fc_get_cmd_data_ptr；触发: 2-plane 命令 + cw0；断言: release 返回 NULL；用例 data_ptr_single_plane_only |
| 262 | 2AH FC 主循环 / 调度 / CQ | RISK-SCH-09 | 待上板 | staging helper 无 plane/cw0/cmd_id 范围守卫 | K | K（用户裁决）：内部 helper 保持 ASSERT；公共入口保证 plane/cw0/cmd_id 合法（H2） | |
| 263 | 2AH FC 主循环 / 调度 / CQ | RISK-SCH-10 | 待上板 | 合成失败完成项忽略 CQ 满，FTL 等不到完成 | K | 需 CQ 满（结构不可达） |  |
| 264 | 2AH FC 主循环 / 调度 / CQ | RISK-SCH-11 | 待上板 | 完成侧不变量只有 debug ASSERT，release 继续写 CQ | K | K（白盒/夹具不可达）：需非 ACTIVE 完成/成环 desc（白盒） | |
| 265 | 2AH FC 主循环 / 调度 / CQ | RISK-SCH-12 | 待上板 | fc_submit_batch 入参/提交不变量只有 debug ASSERT | M | 已上板（2026-11-14 release UT=FC `/fc-nand/negative/` 9/9、0 skip、129 µs）；NULL 数组 / n=0 / ID 越界整批拒绝| seam: fc_submit_batch；触发: NULL 数组、n=0、越界 id；断言: INVALID_ARGUMENT；用例 batch_arg_guards |
| 266 | 2AI NAND 器件 / PHY | RISK-NANDP-01 | 待上板 | 多 target 无真实 FMC bank/CE# 切换，命令可能打错 target | K | 多 target 硬件 |  |
| 267 | 2AI NAND 器件 / PHY | RISK-NANDP-02 | 待上板 | UOP operand 越界时 release 无守卫，会越界访问 buffer/plane | K | 需自定义错误 UOP 表（白盒） |  |
| 268 | 2AI NAND 器件 / PHY | RISK-NANDP-04 | 待上板 | 多 target 板卡未逐 target 初始化，backend handlers 为空 | K | 多 target 硬件 |  |
| 269 | 2AI NAND 器件 / PHY | RISK-NANDP-05 | 待后续 | 多 target timing 共享单套 HAL 句柄，切模式影响整块 FMC | K | 多 target 硬件 |  |
| 270 | 2AI NAND 器件 / PHY | RISK-NANDP-06 | 待上板 | target 选择与 backend 获取拆成两半，漏调用会静默写错 target | K | 多 target 硬件 |  |
| 271 | 2AI NAND 器件 / PHY | RISK-NANDP-07 | 待上板 | HIL 依赖器件专用 timing 查询，换器件会编译失败 | K | 需第二颗器件 |  |
| 272 | 2AI NAND 器件 / PHY | RISK-NANDP-08 | 待上板 | timing 表部分字段无消费者，改表不生效 | K | 时序改动需板端回归，非负例 |  |
| 273 | 2AI NAND 器件 / PHY | RISK-NANDP-10 | 待上板 | timing 零值 release 下静默 delay(0)，时序不足 | K | 需自定义零值 timing profile（白盒） |  |
| 274 | 2AI NAND 器件 / PHY | RISK-NANDP-11 | 待上板 | timing 表命名 Mode0 实为 Mode5，误改会拖慢高速 | K | 命名/注释项 |  |
| 275 | 2AI NAND 器件 / PHY | RISK-NANDP-12 | 待上板 | 多 target 无真实 bank 切换，跨轮次可能选错 target | K | 多 target 硬件 |  |
| 276 | 2AI NAND 器件 / PHY | RISK-NANDP-13 | 待上板 | timing 查询/设置 release 无边界，越界会写坏状态 | M | 已上板（2026-11-14 release UT=FC `/fc-nand/negative/` 9/9、0 skip、129 µs）（get 路径）；NULL mode 与 ch/ce 越界均拒绝。set_timing 是器件 static 回调，不在对外契约| seam: nand_get_timing_mode；触发: mode=NULL、ch/ce==上限；断言: INVALID_ARGUMENT；用例 nand_timing_bounds |
| 277 | 2AI NAND 器件 / PHY | RISK-NANDP-14 | 待上板 | 多条 busy 启动命令缺 tWB，WAIT_READY 提前返回读到旧数据 | K | 需板端 cache/multi-plane 数据回归 |  |
| 278 | 2AI NAND 器件 / PHY | RISK-NANDP-15 | 待上板 | 首次 RESET timeout 500µs 小于 datasheet 1ms，冷启动可能误判 | K | 需冷启动/热复位观察 |  |
| 279 | 2AI NAND 器件 / PHY | RISK-NANDP-17 | 待上板 | tR timeout 38µs 小于 tR_ECC 70µs，启用内部 ECC 会提前超时 | K | 需启用 on-die ECC |  |
| 280 | 2AJ 平台返回状态 | RISK-PLAT-13 | 待上板 | `dma_port_uart_tx_init` 失败返 SUCCESS、成功返 ERROR | K | K（触发不可确定性构造）：重初始化在用的 UART/DMA 会打断日志与后续用例；需独立 UART 实例或可控 fake | |
| 281 | 2AK 构建档 / launch 配置 | RISK-BUILD-01 | 待上板 | `DEBUG_OPT=-O0` 静默复用 -O2 对象 | K | K（非板端用例）：构建系统检查（make -n / 对象目录）不属于板端 munit；需在 makefile 侧加检查目标，非本套件职责 | |
| 282 | 2AK 构建档 / launch 配置 | RISK-BUILD-02 | 待上板 | launch 配置档位不符，产品/UT 共用 ELF 路径，易烧错镜像 | K | launch 配置需人工 F5 对横幅 |  |
| 283 | 2AL HIL UT | RISK-UT-34 | 待后续 | HIL 契约覆盖缺口：`is_complete`、NULL、队列账本、flush 持久化 | N | 已写（部分）/已上板：is_complete、NULL controller/request、flush 持久化、队列账本已由既有 HIL 用例覆盖；buffer NULL 按用户裁决留以后（CTRL-04）、跨重启待链入 ftl_ut_common |  |
| 284 | 2C L2P cache | RISK-L2P-01 | 待上板 | region 加载失败曾立即降级为全未映射，映射静默不可用 | M | 已上板（2026-11-14 debug UT=FTL `test /ftl/media/negative/` 36/36、0 fail、200.10 s）；瞬态+持续两半已写用例，未上板| 已写/未上板；seam: FI_FC_STATUS(normal read, DATA_ERROR)；触发: write+flush+drop+read；断言: 瞬态恢复/持续降级 IO_ERROR；用例 l2p_read_error_degrades |
| 285 | 2C L2P cache | RISK-L2P-02 | 待上板 | flush_kick 先取槽后建链，命令池空重试浪费日志槽 | M | 已上板（2026-11-14 release UT=FTL `/ftl/media/negative/` 54/54、0 SKIP、219.32 s）；N：已写/未上板；占满命令池后 flush 有界、恢复后 CMT 排空、无槽泄漏| 用例 l2p_pool_starve_flush |
| 286 | 2C L2P cache | RISK-L2P-03 | 待上板 | flush_kick 背压/资源预检/CLOCK 引用位的行为未板验 | M | 已上板（2026-11-14 debug UT=FTL `test /ftl/media/negative/` 36/36、0 fail、200.10 s）；用分配/门控注入压 flush| 已写/未上板；seam: FI_ALLOC_FAIL / FI_SEQ_GATE_STARVE；触发: 写+flush；断言: flush 有界完成、无 L2P 槽泄漏；用例 l2p_flush_backpressure |
| 287 | 2C L2P cache | RISK-L2P-04 | 待上板 | 降级启动被误报 restored，丢 journal 未覆盖的旧映射 | K | K-deferred：需人工破坏 L2P region 页后启动；未设计 | |
| 288 | 2C L2P cache | RISK-L2P-05 | 待上板 | 备副本提交时主命令未 release，命令池吃紧时 flush 假失败 | M | 已上板（2026-11-14 release UT=FTL `/ftl/media/negative/` 54/54、0 SKIP、219.32 s）；N：已写/未上板；同 L2P-02（备副本提交拿不到命令）| 用例 l2p_pool_starve_flush |
| 289 | 2C L2P cache | RISK-L2P-06 | 待上板 | 批量读越界分支被压成 FTL_AGAIN，会永久重试（当前不可达） | K | 当前 FTL_LBA_MAX 为 8 的倍数，不可达 |  |
| 290 | 2C L2P cache | RISK-L2P-07 | 待上板 | 顺序读预取白占 cache 行/资源，可能延迟 FLUSH | K | 预取资源占用是性能/阈值项，非失败场景 |  |
| 291 | 2C L2P cache | RISK-L2P-12 | 待上板 | 读/oldp 路径漏查 CMT ⇒ 返回陈旧值 | M | 已上板（2026-11-14 debug UT=FTL `test /ftl/media/negative/` 36/36、0 fail、200.10 s）；写后读应命中 dirty CMT 而非 CLEAN line| 已写/未上板；seam: 无；触发: l2p_write 后立刻 l2p_read/oldp；断言: 返回新 PBA；用例 l2p_cmt_read_priority |
| 292 | 2C L2P cache | RISK-L2P-13 | 待上板 | Header 先于 dirty CMT 落盘 ⇒ 掉电丢映射 | K | 需 dirty CMT + 掉电 |  |
| 293 | 2C L2P cache | RISK-L2P-14 | 待上板 | Journal replay 写进 dirty CMT 后被 PFR 误清 | K | K（触发不可确定性构造）：需写后不 flush 且 journal 页已落盘再 reboot 的回放窗口；quiesce/CP 会改变窗口 | |
| 294 | 2C L2P cache | RISK-L2P-15 | 待上板 | 失败页撤销误保留 dirty CMT ⇒ 读到 FAILC 页 | K | K（触发不可确定性构造）：需 FAILC 撤销与新写并发；当前 host 串行化下不可构造 | |
| 295 | 2C L2P cache | RISK-L2P-16 | 待上板 | 容量竞态导致批量写半更新 ⇒ Journal 重复记账 | K | K（触发不可确定性构造）：需 CMT 满窗口；构造未证明 | |
| 296 | 2C L2P cache | RISK-L2P-17 | 待上板 | thrashing 门挡住必需写回 ⇒ CMT 满后 host 写停住 | K | K（白盒/夹具不可达）：thrashing 由统计驱动，需白盒置位 | |
| 297 | 2C L2P cache | RISK-L2P-18 | 待上板 | dirty CMT 低于水位只在 RAM，掉电依赖 Journal | M | 已上板（2026-11-14 debug UT=FTL `test /ftl/media/negative/` 36/36、0 fail、200.10 s）；低负载写后 idle 排空 CMT| 已写/未上板；seam: 无；触发: 写后泵 idle；断言: cmt_dirty→0；用例 l2p_idle_drain |
| 298 | 2C L2P cache | RISK-L2P-19 | 待上板 | 孤儿实时脏位让 CP 空转/Header 谎报 | K | K（白盒/夹具不可达）：孤儿脏位需白盒置位 | |
| 299 | 2C L2P cache | RISK-L2P-20 | 待上板 | line hint 陈旧/冲突，只掉命中率不返回错行 | K | 只影响命中率，不返回错行；无数据正确性负例 |  |
| 300 | 2C L2P cache | RISK-L2P-21 | 待上板 | Directory/备份 PBA 未校验就进 FC ⇒ 越界路由 | K | K（触发不可确定性构造）：需向介质写入坏 Directory PBA（raw 记录构造），未设计 | |
| 301 | 2C L2P cache | RISK-L2P-22 | 待上板 | CMT dirty 计数在 0 处静默夹取 ⇒ 丢映射或 FLUSH 超时 | K | K（白盒/夹具不可达）：需双重 drop 白盒；公开 API 不可达 | |
| 302 | 2C L2P cache | RISK-L2P-23 | 待上板 | l2p_line_invalidate 不执行「仅 CLEAN」契约 | K | 需要“断言必须触发”的死亡测试，UT 无此机制 |  |

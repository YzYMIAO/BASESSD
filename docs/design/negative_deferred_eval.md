# K 类风险的逐条可行性评估（2026-11-14）

> 对象：`docs/design/negative_coverage.md` 中当前 **216 条 K**。
> 目的：把「K」拆成可执行的类别：现在能做、只差 seam、需要新夹具、或确实不划算。
> 本表只做评估，**不改变 M/N/K 判定**；后续按 D1 → D2 → D3 排期。

## 类别定义与数量

| 类别 | 含义 | 数量 |
|---|---|---|
| D1 | 可做·现有手段 | 17 |
| D2 | 可做·需新增注入 seam | 23 |
| D3 | 可做·需新夹具/白盒/隔离会话 | 43 |
| D5 | 待裁决 | 1 |
| D4 | 不划算（硬件/掉电/主机侧/长压测） | 69 |
| D0 | 非负例（结构/性能/命名/设计判定） | 63 |
| **合计** | | **216** |

- **D1**：现有注入/公共 API/白盒字段已够，加小夹具即可写用例。
- **D2**：缺一个明确的注入点（掉完成、引擎级失败、按 PBA 的数据读、按 plane 的 program 状态等）。
- **D3**：需要新夹具（并发交错、强制压缩、隔离会话、介质快照）或白盒置位，成本中高。
- **D4**：硬件缺失 / 人工掉电 / 主机侧矩阵 / 长压测，按之前口径**不划算**。
- **D0**：不是失败场景（命名/注释/性能/结构/设计判定/不可达），建议转信息或关闭。
- **D5**：需要用户先裁决契约。

## D1 — 可做·现有手段（17 条）

> **落地状态（2026-11-14）**：17 条全部复核完成；其中 16 条已写/扩展用例，
> ARCH-09 合并到现有 4 条 boot 用例（不新增重复用例）。新用例均在代码中，
> **未上板**。用例名与覆盖账见 `docs/design/negative_coverage.md`：
> `md_reclaim_erase_fail`、`gc_decode_cw_lba`（扩展）、`mig_read_success_keeps_fail_count`、
> `journal_ring_erase_fail`（扩展）、`bbt_ring_erase_fail_advances`、
> `boot_read_fail_policy`、`l2p_pool_starve_flush`（扩展）、`mig_eval_fake_clock`、
> `cache_read_completion_all_cws`、`sparse_data_out_read`、`md_cp_read_fail_no_reclaim`、
> `make check-opt`。

> 建议下一批先做这 17 条：都不需要新产品 seam，主要是把现有 helper 组合起来。

| ID | 模块 | 一句话 | 缺口 / 需要的手段 | 成本 |
|---|---|---|---|---|
| MD-01 | 1 metadata 模块 | 日志块擦除持续失败无限重试，无坏块策略 | 需目录压力才触发 reclaim；未证明可确定性构造 | 低~中 |
| MD-22 | 1 metadata 模块 | 借来的日志块标 BAD 时漏减 borrow 配额 | 需 reclaim 连续擦失败触发标坏；触发未证明 | 低~中 |
| GC-19 | 2 GC / migrate / journal | 解码缺 meta 的 CW 被静默当未写，静默丢数据 | 需构造缺 meta 的 FC 读链（白盒/FC夹具） | 低~中 |
| MIG-02 | 2 GC / migrate / journal | predictive 迁移活锁（同 GC-07 修法） | 需 predictive 扫描触发与坏页组合 | 低~中 |
| JN-07 | 2 GC / migrate / journal | jrn_block_next 把 from 自身算作下一块 | 需连续 3 块环形擦失败（烧 6 块）；本轮未写 | 低~中 |
| BBT-19 | 2A BBT（Bad Block Table） | BBT ring 块擦除失败导致持久化永久 livelock | 需把 BBT ring 写指针推到末槽（白盒）+ 擦失败；本轮未写 | 低~中 |
| BOOT-08 | 2B 掉电恢复 / 启动路径 | Journal 扫描/回放读失败被当无更多条目 | boot 期定点读失败注入不可控（无 PBA 键） | 低~中 |
| BOOT-11 | 2B 掉电恢复 / 启动路径 | 快照 probe L2P 反查失败只 LOGW，仍声称 restored | 需 boot 快照 probe 的 L2P 反查失败定点，PBA 取址未设计 | 低~中 |
| BOOT-18 | 2B 掉电恢复 / 启动路径 | 单个 L2P region 不可读 ⇒ 全局回退全盘重建 | 需单 region 读失败 + 快照引用窗口；未设计 | 低~中 |
| MD-16 | 2B 掉电恢复 / 启动路径 | victim region 页读失败 ⇒ 压缩每轮同页中止 | 需强制压缩 + victim 副本读失败；forced compaction 夹具未做 | 低~中 |
| CP-04 | 2D Checkpoint | host 写流下 CP 提交饥饿，submit stalled 后 abort | 需 host 写流占满命令池；无池满注入 | 低~中 |
| ARCH-09 | 2L FTL 模块 handle 契约 | PFR 收为 boot orchestrator，内部原语全部 static | 需 fresh/clean/PF 四条启动路径回归 | 低~中 |
| PLAT-10 | 2AA 平台定时调度器（`timer`） | mig_eval 混用 TIM6 与 SysTick 两套时间坐标 | time 坐标混用需伪时钟+调度对拍，未设计 | 低~中 |
| DP-12 | 2AG 数据面 / NAND dispatch | 单 plane cache 读失败只标最低位 CW | 需 FC 缓存读命令级构造（夹具的读 helper 内部断言成功），未设计 | 低~中 |
| DP-26 | 2AG 数据面 / NAND dispatch | 带 DATA_OUT 的稀疏 CW 读首输出读错 CW | 需 FC 稀疏 CW + WITH_DATA_OUT 的数据通路夹具；未做 | 低~中 |
| BUILD-01 | 2AK 构建档 / launch 配置 | `DEBUG_OPT=-O0` 静默复用 -O2 对象 | 构建系统检查（make -n / 对象目录）不属于板端 munit；需在 makefile 侧加检查目标，非本套件职责 | 低~中 |
| L2P-04 | 2C L2P cache | 降级启动被误报 restored，丢 journal 未覆盖的旧映射 | 需人工破坏 L2P region 页后启动；未设计 | 低~中 |

## D2 — 可做·需新增注入 seam（23 条）

> **框架状态（2026-11-14）**：已搭第一批共享 seam（未上板）：
> - `FI_FC_ENGINE_FAIL`（H，dispatch completion 策略前，key=opcode）
> - `FI_COMPLETION_DROP` / `FI_COMPLETION_LATE`（W，CQ 出口，key=opcode）
> - `FI_READ_COLUMN_FAIL`（W，`ftl_sync_read_column`，key=PBA）
> - `fc_nand_dispatch_test_completion()`（测试 API，只驱动 completion 策略表）
>
> 证明用例：`engine_fail_all_cws`、`completion_drop_bounded`、
> `completion_late_delivered`、`read_column_fail_seam`、`host_write_plane_fail`、
> `data_path_encode_fail`、`data_path_read_fail`、`clock_now_fail`、
> `engine_timeout_sets_status`、`engine_fail_param_page`、`engine_fail_random_read`、
> `uop_jump_operand_guard`、`uop_data_range_guard`、`uop_zero_timing_guard`、
> `cp_duplicate_completion_guard`。
> **尚缺的 seam**：FC 初始化/HAL timing 失败（DP-24/28）、cache pad 早退
> （CACHE-31）、ONFI 偶发失败（TOP-03）、回放第二遍定点读失败（JN-08）。
> D2 业务用例仍有部分未逐条编写（JN-04/08、IF-02/08/17、CACHE-31、TOP-03、
> DP-07/24/28、MIG-10 的业务场景）。


> 每条前面的「缺口」就是需要新增的注入点；可合并成少数几个 seam。

| ID | 模块 | 一句话 | 缺口 / 需要的手段 | 成本 |
|---|---|---|---|---|
| MIG-09 | 2 GC / migrate / journal | host 写失败只标 plane0 的块 | 需逐 CW/plane 完成态失败；现有 FC_STATUS 只按 opcode 键 | 中 |
| MIG-10 | 2 GC / migrate / journal | 迟到完成可能按新 job 的 phase 派发 | 需人为丢完成/迟到完成，FC 引擎无注入点 | 中 |
| JN-04 | 2 GC / migrate / journal | drain 超时后仍同步刷页，同页二次编程 | 需命令丢失/超时（FC 引擎无注入） | 中 |
| JN-08 | 2 GC / migrate / journal | 回放第二遍读失败不上报，静默跳过条目 | boot 回放期间定点读注入不可控 | 中 |
| BBT-02 | 2A BBT（Bad Block Table） | 工厂扫描对列读失败页的判坏行为未实测 | 需新增 ftl_sync_read_column wrap + 空片扫描（破坏性） | 中 |
| CP-02 | 2D Checkpoint | checkpoint 回调缺 waiting 守卫，迟到完成会按当前 phase 重走 | 需重复/迟到完成，FC 引擎无注入 | 中 |
| IF-02 | 2F FTL↔FC 接口 | 同步等待超时后迟到完成解引用 NULL ctx + 槽泄漏 | 需超时后迟到完成（FC 无丢完成注入） | 中 |
| IF-08 | 2F FTL↔FC 接口 | page-read 聚合忽略 NAND 结果位，TIMEOUT 可静默返回陈旧数据 | 需 NAND 读超时+旧 buffer，引擎级注入未实现 | 中 |
| IF-17 | 2F FTL↔FC 接口 | 超时后调用者释放仍在飞的命令，可能跨命令破坏 | 需提交后无完成，FC 无注入 | 中 |
| CACHE-31 | 2Z cache 长流 + Journal 顺序映射页 | padding 回调早退分支泄漏 FC 命令 | 需成员失败 + pad 在飞后泄漏计数；构造未证明 | 中 |
| TOP-03 | 2AB NAND 拓扑扫描（`nand_target_probe` / `ftl_topology_build`） | LUN 数按器件探测，ONFI 偶发失败即拒绝启动 | ONFI 偶发失败需器件/注入 | 中 |
| DP-01 | 2AG 数据面 / NAND dispatch | ECC 引擎不可用被归一为成功，写不编码、读不校验 | 需 ECC 初始化失败注入，引擎无 seam | 中 |
| DP-02 | 2AG 数据面 / NAND dispatch | 数据路径编码错误被调用方丢弃，ECC 失败仍提交 NAND | 需 encode 失败注入 | 中 |
| DP-07 | 2AG 数据面 / NAND dispatch | 链式失败生命周期半状态，整链完成时断言 ACTIVE | 需 encode/build 失败注入 | 中 |
| DP-11 | 2AG 数据面 / NAND dispatch | dispatch 忽略 µs 时钟失败，LUN 永久停在 ISSUED | 需 µs 时钟失败注入 | 中 |
| DP-17 | 2AG 数据面 / NAND dispatch | JUMP_IF_NOT_READY 回跳减法无 release 下溢守卫 | UOP 表静态正确，需改表构造 | 中 |
| DP-18 | 2AG 数据面 / NAND dispatch | random read UOP 缺 CHECK_FAIL，中间 CW 失败被吞 | 需引擎执行中注入 FAIL（非 CQ 状态） | 中 |
| DP-19 | 2AG 数据面 / NAND dispatch | 单 plane cache 读 UOP 缺 CHECK_FAIL | 同上 | 中 |
| DP-21 | 2AG 数据面 / NAND dispatch | 参数页/唯一 ID/GET_FEATURES 的 FAIL 被当成成功 | 需参数页 FAIL 注入（引擎级） | 中 |
| DP-24 | 2AG 数据面 / NAND dispatch | 高速模式失败后没有回滚到低速 | 需 HAL_NAND_Init 失败注入 | 中 |
| DP-28 | 2AG 数据面 / NAND dispatch | CRC 初始化失败在 release 下静默，校验恒通过 | 需 CRC 初始化失败注入 | 中 |
| NANDP-02 | 2AI NAND 器件 / PHY | UOP operand 越界时 release 无守卫，会越界访问 buffer/plane | 需自定义错误 UOP 表（白盒） | 中 |
| NANDP-10 | 2AI NAND 器件 / PHY | timing 零值 release 下静默 delay(0)，时序不足 | 需自定义零值 timing profile（白盒） | 中 |

## D3 — 可做·需新夹具/白盒/隔离会话（43 条）

| ID | 模块 | 一句话 | 缺口 / 需要的手段 | 成本 |
|---|---|---|---|---|
| MD-02 | 1 metadata 模块 | 压缩预算按启动时 l2p_live，在飞 region 写可能超预算 | 压缩与前台写交错时序不可控（并发窗口） | 中~高 |
| MD-18 | 1 metadata 模块 | 目录检查点未覆盖最新 Directory 时压缩擦 victim | 需压缩 job 与前台 commit 交错；触发未证明 | 中~高 |
| MD-19 | 1 metadata 模块 | victim 未排除在飞槽位，stale count 被清零 | 需在飞写回与扫描重叠；触发未证明 | 中~高 |
| MD-20 | 1 metadata 模块 | 压缩 read→commit 无 CAS，陈旧映射持久化 | 需压缩读→commit 间前台搬移同一副本；触发未证明 | 中~高 |
| GC-18 | 2 GC / migrate / journal | 空 victim 快路径绕过 Journal 持久性闸门 | 需上一 job abort 且 journal 页缓冲未落盘的窗口 | 中~高 |
| RR-06 | 2 GC / migrate / journal | RR 重试读被 read sequence draining 挡住，host 读组永不完成 | 需 ≥16 LBA 读序列 + RR 互锁；现有 8-LBA 缓冲/序列构造未证明 | 中~高 |
| BBT-05 | 2A BBT（Bad Block Table） | 元数据区为空时可能误判空片并重扫 | 需只擦元数据保用户数据，破坏性且无可靠判据 | 中~高 |
| BBT-07 | 2A BBT（Bad Block Table） | CP 快照并集的 BAD 不补 Spare 池 | 需 CP 快照含 BAD 且 Spare 不足的介质 | 中~高 |
| BBT-12 | 2A BBT（Bad Block Table） | 固定元数据池无 BAD 跳过/替换 | 标记元数据池块会持久破坏同一 test 会话的后续用例；无隔离/复原手段 | 中~高 |
| BBT-15 | 2A BBT（Bad Block Table） | 运行期元数据块变坏，候选池/游标不更新 | 运行期标坏 journal/CP 块会破坏同一会话；需独立介质会话或快照复原 | 中~高 |
| BBT-16 | 2A BBT（Bad Block Table） | 全片体检可能恢复边缘坏块 | 需边缘坏块介质与 format --verify | 中~高 |
| BBT-18 | 2A BBT（Bad Block Table） | 多圈 BBT ring 新写 record 后 reload 选到上一条 | 需多圈 ring + BAD 读失败的特定布局 | 中~高 |
| BBT-21 | 2A BBT（Bad Block Table） | 出厂扫描跳过 Block 0 的器件依据无出处 | 需空片/器件出厂坏块证据 | 中~高 |
| BOOT-02 | 2B 掉电恢复 / 启动路径 | carve 先于 full rebuild，写过块同时是池成员与 CLOSED | 需预置数据并强制 full rebuild fallback；触发未证明 | 中~高 |
| BOOT-03 | 2B 掉电恢复 / 启动路径 | CP 读失败被当「需要格式化」，一次瞬时读错擦整片 | 若缺陷存在会擦整片，单 UT 会毁掉同会话后续用例；需独立会话 | 中~高 |
| BOOT-06 | 2B 掉电恢复 / 启动路径 | full rebuild 按第一个空白页当写指针，hole 后漏计 | 需 raw 写造 page0 空洞 + 强制 full rebuild；raw 构造未设计 | 中~高 |
| BOOT-09 | 2B 掉电恢复 / 启动路径 | md_load 失败静默新建 Directory，旧映射整体丢失 | 需 Anchor 损坏且可观测「拒绝启动」；boot 夹具无拒绝返回通道 | 中~高 |
| BOOT-10 | 2B 掉电恢复 / 启动路径 | jrn_resume 忽略跨块擦除结果，首次写落未擦页 | 需把介质预置成 resume 跨块待擦状态，当前无构造 | 中~高 |
| BOOT-30 | 2B 掉电恢复 / 启动路径 | Verify 报 1.3 MB 数据丢失，实为读路径瞬时错读（数据完好） | 依赖 RR-06 复现窗口，当前未复现 | 中~高 |
| CP-01 | 2D Checkpoint | journal 预擦失败仍记 pre_erased_block，回切时跳过擦除 | 需 CP truncate 相位定点控制 + 多 plane 擦失败；相位无法从公共 API 定点 | 中~高 |
| HOST-07 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | 停泊确认路径重入，oldp 陈旧/映射分叉 | 需 Journal 页满+停泊确认同窗口 | 中~高 |
| HOST-11 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | Journal 压力下 TRIM 持久化可能丢失 | 需大量写映射后 trim 的特定序列 | 中~高 |
| HOST-16 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | 「无进展时长」预算实际未生效（死赋值） | 需 park timeout 与后台进展同窗口 | 中~高 |
| BG-06 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | l2p_flush_max 经 level 反查，第二套配置读取 | 需强制 BUSY 档并读 flush 条数，档位选择不可控 | 中~高 |
| BG-07 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | journal_erase 手工读 journal 内部 phase/need_erase | 需 journal 换块路径；触发未证明 | 中~高 |
| PLAT-01 | 2M USB MSC / 平台 | `.noncacheable_buffer` 段匹配漏前导点，页缓冲落进可缓存 AXI RAM | 需 CONFIG_NAND_DMA=1 构建，非目标默认档 | 中~高 |
| CACHE-15 | 2Z cache 长流 + Journal 顺序映射页 | 无数据 END 缺失，padding 页盘满例外未验 | 盘满例外需 free 耗尽窗口 | 中~高 |
| CACHE-30 | 2Z cache 长流 + Journal 顺序映射页 | padding 收尾把已证明未确认页误判失败 | 需 padding 在飞 + 已证明未确认页窗口 | 中~高 |
| CACHE-36 | 2Z cache 长流 + Journal 顺序映射页 | 干净释放写序列时不看在飞成员，迟到完成把未证明页推进死序列 | 需组级失败且全部成员在飞的窗口 | 中~高 |
| POOL-03 | 2AF FC 资源池 / 生命周期 | desc 链释放 / 遍历缺上界与环检测 | 需构造成环 desc 链（白盒） | 中~高 |
| DP-03 | 2AG 数据面 / NAND dispatch | INVALID buffer 进入数据路径，release 下越界/空指针 | 需构造 desc 指向 INVALID buffer 的原始命令；FC 夹具未提供该构造 | 中~高 |
| DP-04 | 2AG 数据面 / NAND dispatch | desc 链遍历无上界，成环会死循环 | 需成环 desc 链（白盒） | 中~高 |
| DP-20 | 2AG 数据面 / NAND dispatch | cache 编程首页 FAILC 误报，误判好页失败 | 需残留 FAILC + 新批首页的时序窗口 | 中~高 |
| SCH-06 | 2AH FC 主循环 / 调度 / CQ | 断言终端路径再次断言，递归覆盖原始现场 | 断言递归需制造池账分叉，风险高 | 中~高 |
| SCH-11 | 2AH FC 主循环 / 调度 / CQ | 完成侧不变量只有 debug ASSERT，release 继续写 CQ | 需非 ACTIVE 完成/成环 desc（白盒） | 中~高 |
| PLAT-13 | 2AJ 平台返回状态 | `dma_port_uart_tx_init` 失败返 SUCCESS、成功返 ERROR | 重初始化在用的 UART/DMA 会打断日志与后续用例；需独立 UART 实例或可控 fake | 中~高 |
| L2P-14 | 2C L2P cache | Journal replay 写进 dirty CMT 后被 PFR 误清 | 需写后不 flush 且 journal 页已落盘再 reboot 的回放窗口；quiesce/CP 会改变窗口 | 中~高 |
| L2P-15 | 2C L2P cache | 失败页撤销误保留 dirty CMT ⇒ 读到 FAILC 页 | 需 FAILC 撤销与新写并发；当前 host 串行化下不可构造 | 中~高 |
| L2P-16 | 2C L2P cache | 容量竞态导致批量写半更新 ⇒ Journal 重复记账 | 需 CMT 满窗口；构造未证明 | 中~高 |
| L2P-17 | 2C L2P cache | thrashing 门挡住必需写回 ⇒ CMT 满后 host 写停住 | thrashing 由统计驱动，需白盒置位 | 中~高 |
| L2P-19 | 2C L2P cache | 孤儿实时脏位让 CP 空转/Header 谎报 | 孤儿脏位需白盒置位 | 中~高 |
| L2P-21 | 2C L2P cache | Directory/备份 PBA 未校验就进 FC ⇒ 越界路由 | 需向介质写入坏 Directory PBA（raw 记录构造），未设计 | 中~高 |
| L2P-22 | 2C L2P cache | CMT dirty 计数在 0 处静默夹取 ⇒ 丢映射或 FLUSH 超时 | 需双重 drop 白盒；公开 API 不可达 | 中~高 |

## D5 — 已裁决（1 条）

| ID | 模块 | 一句话 | 缺口 / 需要的手段 | 成本 |
|---|---|---|---|---|
| WL-01 | 2E 磨损计数 / 静态 WL | WL 擦完用 RETIRE，RETIRED 计入 erase 统计导致容量流失 | **已裁决（2026-11-14）**：WL post=FREE；RETIRED 不参与 WL 触发/容量，仅作物理磨损诊断；关闭 | — |

## D4 — 不划算（硬件/掉电/主机侧/长压测）（69 条）

> 这些保持 K，不再逐条排期；重开条件见备注。

| ID | 模块 | 一句话 | 缺口 / 需要的手段 | 成本 |
|---|---|---|---|---|
| MD-23 | 1 metadata 模块 | ring frontier 落在块中时复用半编程槽 | 需写目录检查点期间掉电（人工断电） | — |
| MD-24 | 1 metadata 模块 | initial 目录检查点只写单 plane，回退到旧 record | 需 format 中途掉电 | — |
| GC-06 | 2 GC / migrate / journal | WL 5s 节拍/CLOSED 门控/valid_count 上界/三态未板验 | WL 5s 节拍/CLOSED 门控的直接断言需白盒或长压测 | — |
| GC-14 | 2 GC / migrate / journal | no-gain 判据依赖 total_writes/free 快照 | 需满盘/慢写窗口，短负例不可控 | — |
| GC-15 | 2 GC / migrate / journal | BUSY 档挡掉 urgent GC，free 掉到 1 才急救 | 需写到 free<32 的长序列，属压测 | — |
| JN-10 | 2 GC / migrate / journal | 撕裂 frontier 页重复编程 + 每次开机 full rebuild | 需写 journal 期间掉电 | — |
| BBT-03 | 2A BBT（Bad Block Table） | 格式化擦除与 BBT save 间掉电丢失工厂坏块图 | 需 format 处理中掉电 | — |
| BBT-14 | 2A BBT（Bad Block Table） | FORMAT_IN_PROGRESS 提交点过早 | 需 format 中途掉电 | — |
| BBT-20 | 2A BBT（Bad Block Table） | 同步 bbt_save 与异步 job / 并发标坏无协议 | CLI format 与 1ms 在飞窗口重叠不可控 | — |
| BOOT-04 | 2B 掉电恢复 / 启动路径 | 两阶段格式化中途掉电，重启无法判断未完成 | 两阶段格式化中途掉电（人工断电） | — |
| BOOT-17 | 2B 掉电恢复 / 启动路径 | 空片首启可能拒绝自动格式化 | 需真空白片 | — |
| BOOT-22 | 2B 掉电恢复 / 启动路径 | 格式化中断后可能拒绝格式化/启动 | 需 format 中途掉电 | — |
| BOOT-25 | 2B 掉电恢复 / 启动路径 | 重启后整段覆盖写触发大量 dangling | dangling 归因需日志采集与长序列 | — |
| MD-15 | 2B 掉电恢复 / 启动路径 | 副本感知压缩（主/备逐副本搬移） | 需满盘压缩与双副本搬移压力 | — |
| CP-10 | 2D Checkpoint | T2 触发无 latch，写计数停在整数倍时 CP 空转 | 需写满 N×1024 页的长序列 | — |
| CP-11 | 2D Checkpoint | initial record 失败块写死 plane 0/1 | 需 PLANE_NUM>2 的构建，硬件不支持 | — |
| WL-04 | 2E 磨损计数 / 静态 WL | WL 收尾 FREE / read_count 复位 / BAD plane 过滤 / 磨损表校验未板验 | 需正常 WL/GC + 断电 + soak，属长时压测 | — |
| IF-09 | 2F FTL↔FC 接口 | 放弃序列只清 RAM 不发设备 LAST | 需主机中途拔线/中止，PC 侧手动 | — |
| IF-12 | 2F FTL↔FC 接口 | 确认待入账 × 序列门控可能退化到 1s idle 守卫 | 需满盘大顺序写+序列门控，属长压测 | — |
| HOST-14 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | 拔线后门控环形等待导致读回 MATA（根因已改，死锁路径未验） | 需拔线+满盘 GC 窗口，人工触发 | — |
| HOST-22 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | host 写取不到页对时无界重试，USB 命令可能永久挂起 | 需 free+OP 同时耗尽；无注入点 | — |
| WB-02 | 2I 页打包写缓冲 wbuf（`wbuf.c` + `wbuf.h`） | TRIM/RESET 由外部模块手工改 wbuf 内部不变量 | bbt --erase/RESET 走 CLI，UT 未接入 | — |
| BG-10 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | 后台节拍 flag + token bucket（grant 太小/太大风险） | token bucket/节拍是吞吐行为，需长压测 | — |
| ARCH-02 | 2L FTL 模块 handle 契约 | 跨 LUN superblock 条带 / 跨 LUN GC/WL 未实现 | 跨 LUN 条带需多实例硬件 | — |
| USB-02 | 2M USB MSC / 平台 | MSC 异步完成/deferred/discard 路径未实测 | 需主机拔线/复位，PC 侧手动 | — |
| USB-05 | 2M USB MSC / 平台 | WRITE(10) FUA 原被忽略（已实现等确认后回执） | 需自定义 SCSI FUA 工具 | — |
| USB-07 | 2M USB MSC / 平台 | request->io 模板的 cmd_remaining 跨命令残留 | cmd_remaining 在 usb_msc.c 内设置；HIL 只到 controller，无法从请求对象断言跨命令残留；由 PC 主机侧矩阵覆盖 | — |
| USB-08 | 2M USB MSC / 平台 | 写 lookahead 提前回执/BUSY 背压未注入失败 | 需主机早期回执+程序失败组合 | — |
| USB-09 | 2M USB MSC / 平台 | fsync 被自己的在飞写槽拒掉（已加 FLUSH 专用槽） | fsync/slot 逻辑在 usb_msc.c（static），HIL 不到 MSC 层；写后 flush 已由 write_flush_readback 覆盖，但「在飞写槽不拒 flush」无法从 HIL 构造 | — |
| USB-10 | 2M USB MSC / 平台 | slot_match 把真正的下一条相同写当事件重放丢弃 | slot_match 为 usb_msc.c 的 static；HIL 无法构造 SCSI 重复写事件重放场景 | — |
| USB-11 | 2M USB MSC / 平台 | TinyUSB 本地补丁清单不完整（cbw_info 钩子） | 文档补丁清单，非负例 | — |
| USB-12 | 2M USB MSC / 平台 | SYNCHRONIZE CACHE(16) 0x91 未支持 | 需主机发 0x91，PC 侧验证 | — |
| USB-13 | 2M USB MSC / 平台 | 两写槽占用时 READ 返回 ERROR 而非 BUSY | 需两写槽在飞时 READ；槽状态构造未证明 | — |
| USB-14 | 2M USB MSC / 平台 | SCSI UNMAP 已实现，未上板 | 需主机 blkdiscard/fstrim | — |
| USB-16 | 2M USB MSC / 平台 | UNMAP descriptor 按 8B 步进（规范 16B）已改 | 需直调 usb_msc_unmap_parse；若为 static 则不可测 | — |
| NAND-05 | 2O NAND / FMC 时序与吞吐 | used 介质不可纠读页，L2P 仍指向坏块，每次读重试 8 次 | 需芯片物理坏页 | — |
| ALLOC-09 | 2P allocator | 低 free 时成对分配失败导致写路径永久 stall（plane 不平衡） | 需 plane 不平衡的低 free 长压测 | — |
| CACHE-16 | 2Z cache 长流 + Journal 顺序映射页 | 跨命令序列 + VWC 早回执，USB 端到端未跑 | 需 USB 主机端到端，PC 侧矩阵 | — |
| CACHE-18 | 2Z cache 长流 + Journal 顺序映射页 | 提前回执扩大掉电暴露窗口，VWC 窗口未量化 | VWC 窗口需掉电量化 | — |
| CACHE-29 | 2Z cache 长流 + Journal 顺序映射页 | 早回执改变 flush/断电口径，长流+断电未量 | 长流+掉电窗口未量化 | — |
| USBC-01 | 2Z cache 长流 + Journal 顺序映射页 | deferred error 以下一条命令失败近似，非标准 sense | deferred sense 语义需主机侧观察 | — |
| USBC-03 | 2Z cache 长流 + Journal 顺序映射页 | USB 盘间歇报无介质，主机侧已止血 | 主机侧采样/盘符问题，需 PC 侧 | — |
| PLAT-09 | 2AA 平台定时调度器（`timer`） | SysTick 时基与 TIM6 调度心跳的超时判据未板验 | 时基行为需板端长跑；UT 只能测伪时钟 | — |
| PLAT-12 | 2AA 平台定时调度器（`timer`） | µs 时间戳从 DWT 改 SysTick，未板验 | DWT→SysTick 需板端对比 | — |
| TOP-01 | 2AB NAND 拓扑扫描（`nand_target_probe` / `ftl_topology_build`） | 扫描得到的多 target / 多 LUN 拓扑路径未板验 | 多 target/多 LUN 硬件不具备 | — |
| TOP-02 | 2AB NAND 拓扑扫描（`nand_target_probe` / `ftl_topology_build`） | sparse 拓扑下 controller FLUSH 按 3D 槽遍历会空指针 | sparse 拓扑需多 die 硬件 | — |
| CLI-01 | 2AC CLI 调试前端（`controller/cli.c`） | 长命令内联执行阻塞主循环，IWDG 启用后会复位 | IWDG 未启用（待决策），无法验证看门狗 | — |
| CLI-02 | 2AC CLI 调试前端（`controller/cli.c`） | format/bbt erase 不检查 host 在飞，会造成介质状态分叉 | 需 CLI+USB 并发操作，主机侧 | — |
| CLI-03 | 2AC CLI 调试前端（`controller/cli.c`） | wp 复用静态缓冲无在飞守卫，重入会写错字节 | 需连续 wp；CLI 交互 | — |
| CLI-07 | 2AC CLI 调试前端（`controller/cli.c`） | rd 失败仍打印 rd ok 并 dump 旧缓冲 | 需制造读失败并抓串口输出 | — |
| CLI-08 | 2AC CLI 调试前端（`controller/cli.c`） | wp 绕过 host gate，可与 USB/CLI 并发进入 FTL | 需 wp 与 USB 并发，主机侧 | — |
| CTRL-01 | 2AD Controller 请求拆分（`request.c` / `controller.c`） | 多 run 请求中途失败时父请求提前完成、错误被覆盖 | 单 die 仅 1 run；多 run 需多 die | — |
| DP-08 | 2AG 数据面 / NAND dispatch | 通道轮询注释称 round-robin，实际每轮从 CH0 开始 | 需多通道硬件 | — |
| DP-13 | 2AG 数据面 / NAND dispatch | ecc_bit_count 无生产者，恒为 0 | 需可纠正位翻转，介质不可控 | — |
| DP-15 | 2AG 数据面 / NAND dispatch | LDPC 分支引用不存在的 g_qc，切构建即编译失败 | LDPC 未启用 | — |
| DP-16 | 2AG 数据面 / NAND dispatch | dispatch poll 用 BYTE 返完成数，256 回绕为 0 | 需多通道×CE×LUN=256 配置 | — |
| DP-22 | 2AG 数据面 / NAND dispatch | WAIT_READY 后端阻塞，引擎协作让出路径未生效 | WAIT_READY 协作让出需多 LUN 观察 | — |
| DP-27 | 2AG 数据面 / NAND dispatch | LDPC 解码栈占用 4.2KB，超 2KiB 栈预算 | LDPC 未启用 | — |
| SCH-02 | 2AH FC 主循环 / 调度 / CQ | fc_core_poll 只在无完成时填充，持续完成下提交延迟 | 提交延迟行为，需持续完成压测 | — |
| NANDP-01 | 2AI NAND 器件 / PHY | 多 target 无真实 FMC bank/CE# 切换，命令可能打错 target | 多 target 硬件 | — |
| NANDP-04 | 2AI NAND 器件 / PHY | 多 target 板卡未逐 target 初始化，backend handlers 为空 | 多 target 硬件 | — |
| NANDP-05 | 2AI NAND 器件 / PHY | 多 target timing 共享单套 HAL 句柄，切模式影响整块 FMC | 多 target 硬件 | — |
| NANDP-06 | 2AI NAND 器件 / PHY | target 选择与 backend 获取拆成两半，漏调用会静默写错 target | 多 target 硬件 | — |
| NANDP-07 | 2AI NAND 器件 / PHY | HIL 依赖器件专用 timing 查询，换器件会编译失败 | 需第二颗器件 | — |
| NANDP-12 | 2AI NAND 器件 / PHY | 多 target 无真实 bank 切换，跨轮次可能选错 target | 多 target 硬件 | — |
| NANDP-14 | 2AI NAND 器件 / PHY | 多条 busy 启动命令缺 tWB，WAIT_READY 提前返回读到旧数据 | 需板端 cache/multi-plane 数据回归 | — |
| NANDP-15 | 2AI NAND 器件 / PHY | 首次 RESET timeout 500µs 小于 datasheet 1ms，冷启动可能误判 | 需冷启动/热复位观察 | — |
| NANDP-17 | 2AI NAND 器件 / PHY | tR timeout 38µs 小于 tR_ECC 70µs，启用内部 ECC 会提前超时 | 需启用 on-die ECC | — |
| L2P-13 | 2C L2P cache | Header 先于 dirty CMT 落盘 ⇒ 掉电丢映射 | 需 dirty CMT + 掉电 | — |

## D0 — 非负例（结构/性能/命名/设计判定）（63 条）

> **裁决状态（2026-11-14）**：53 条已从负例转出（47 条信息 + 6 条小修后归档为「已闭合」）：
> - 45 条直接转信息（已修/已取代、设计判定、不可达/不可复现）
> - 8 条做小操作后转信息：BOOT-28（block_words in/out 契约）、DOC-03（注释回扫）、
>   ALLOC-06（ftl_gblock 前置契约）、BIT-01（SSD_FIELD_MASK 下沉 common）、
>   CLI-04/05（帮助/文档语法核对）、NANDP-11（Mode5 命名核对）、BUILD-02（launch 配置核对）
>
> 其中 6 条（BOOT-28/DOC-03/CLI-04/CLI-05/NANDP-11/BUILD-02）已移入归档
> `docs/archive/known_risks_batches.md`（2026-11-14 D0 小修后闭合）。
>
> 仍保留 10 条 K：GC-05、PERF-01/02、WL-05、L2P-07（性能/功能债）、
> BBT-04（介质格式演进）、ARCH-07/ALLOC-07（内存域布局）、CLI-06（板端字段）、
> NANDP-08（timing 板端回归）。


| ID | 模块 | 一句话 | 缺口 / 需要的手段 | 成本 |
|---|---|---|---|---|
| GC-05 | 2 GC / migrate / journal | GC Bitmap 未实现，掉电后 GC 从块头重来 | 性能优化项（GC bitmap），非失败场景 | — |
| MIG-05 | 2 GC / migrate / journal | GC 读失败 abort 日志刷屏阻塞 UART | 日志速率/刷屏，UT 无采集判据 | — |
| MIG-06 | 2 GC / migrate / journal | mark BAD 日志单位/原因不区分 | 日志字段格式，非失败场景 | — |
| JN-05 | 2 GC / migrate / journal | 回放失败/降级日志逐条打印，拖慢开机 | 日志速率测量 | — |
| JN-06 | 2 GC / migrate / journal | 回放工作缓冲从栈改文件级 static | 栈帧是构建测量项，非负例 | — |
| RR-05 | 2 GC / migrate / journal | SLC 无 read-retry table 却做 8 次重试 + 邻居辅助 | 用户明确不处理 0x89；行为是快速失败，否定例 | — |
| PERF-01 | 2 GC / migrate / journal | GC/WL 选块重复全表扫描 | 性能项，非失败场景 | — |
| PERF-02 | 2 GC / migrate / journal | GC/WL 目的页逐 plane 单条写，无 multi-plane 命令 | 性能项 | — |
| BBT-04 | 2A BBT（Bad Block Table） | BBT header 无布局版本字段，格式演进无法识别 | 介质格式演进项，非负例 | — |
| BBT-06 | 2A BBT（Bad Block Table） | BBT 回调不保存 command_id，重复完成可能错配 phase | 当前 NAND 无重复完成源，不可构造 | — |
| BBT-13 | 2A BBT（Bad Block Table） | 固定元数据分区无法按 BBT 重映射 | 设计判定：不做 remap 表 | — |
| BBT-17 | 2A BBT（Bad Block Table） | BBT 候选池判断散落多处，易误擦 / 误留 | 重构/收敛项，非失败场景 | — |
| BOOT-07 | 2B 掉电恢复 / 启动路径 | 判空依赖 raw USER 区，加扰后不可靠 | 已由 IF-15 取代；判空判据未确定 | — |
| BOOT-13 | 2B 掉电恢复 / 启动路径 | format 后未重启就写，CLI 永久 busy（旧镜像残留） | 旧镜像残留/HardFault 现场，不可复现 | — |
| BOOT-14 | 2B 掉电恢复 / 启动路径 | full rebuild 空闲块扫描从 64 页降到 1 页 | 性能优化项，非失败场景 | — |
| BOOT-16 | 2B 掉电恢复 / 启动路径 | full rebuild 逐 CW/逐 region 日志刷屏拖慢启动 | 日志速率测量 | — |
| BOOT-26 | 2B 掉电恢复 / 启动路径 | 恢复统计 WORD 回绕导致快照被错误接受 | 需 65536 次失败才回绕，不可达 | — |
| BOOT-28 | 2B 掉电恢复 / 启动路径 | block_words 隐式 in/out，恢复顺序只靠注释 | 结构/顺序约束，非失败场景 | — |
| WL-05 | 2E 磨损计数 / 静态 WL | wl_should_trigger 与 wl_select_victim 各做一次全表扫描 | 性能项（重复扫描） | — |
| IF-06 | 2F FTL↔FC 接口 | chain 派发路径不处理 RAID stripe encode | RAID 关闭（R5），无实际失效 | — |
| IF-16 | 2F FTL↔FC 接口 | timeout_us 名不副实，实为自旋轮数 | 真实微秒 deadline 是行为修正，否定例 | — |
| HOST-08 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | TRIM 已异步；CLI 诊断与 CP 冷启动 locate 仍同步 | 同步点是控制面/冷启动，非失败场景 | — |
| HOST-09 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | TRIM 命中 wbuf 未编程 LBA 时不失效旧映射 | 当前串行化下不可达 | — |
| CR-01 | 2H 完成派发面（`ftl_op.c/h`） | ctx 旁表不按 command_id 去重，id 复用得别人 ctx | 需 CQ 满/丢完成触发 id 复用 | — |
| WB-03 | 2I 页打包写缓冲 wbuf（`wbuf.c` + `wbuf.h`） | 模块抽出 + 位运算收敛 + 日志降级，wbuf 是写路径核心 | 模块抽出/等价改写，非失败场景 | — |
| WB-04 | 2I 页打包写缓冲 wbuf（`wbuf.c` + `wbuf.h`） | 组在飞数原由多处手工维护，漏减挂死 host 写 | 收敛为唯一闸门，属结构项 | — |
| WB-08 | 2I 页打包写缓冲 wbuf（`wbuf.c` + `wbuf.h`） | WBUF_STATE_FAILED 不可达，错误锁存契约与实现矛盾 | 删除不可达 WBUF_STATE_FAILED；错误经 base.error 上报、flush 超时兜底，无独立失败模式 | — |
| BG-04 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | 任务表装载无边界检查，加项越界踩 task_count | 表容量由 _Static_assert 编译期保证，运行期不可构造 | — |
| BG-05 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | 调度器死字段 + priority 与表序两套坐标漂移 | 删死字段/坐标收敛，非失败场景 | — |
| BG-11 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | 档位下标无值域断言，越界静默读相邻数据 | 档位越界属编程错误，入口 ASSERT 已挡 | — |
| DOC-03 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | ftl.c 注释/代码分叉（H6/H37） | 注释/断言同步，非失败场景 | — |
| LOG-02 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | GC 事件在极端 victim 速率下仍可能占 UART | 日志速率测量 | — |
| LOG-03 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | GC no-gain 刷屏（边沿复用） | 日志速率测量 | — |
| ARCH-05 | 2L FTL 模块 handle 契约 | 独立 confirm_handle 未采用，confirm/TRIM 两处顺序实现 | 设计判定（不采用独立 handle） | — |
| ARCH-07 | 2L FTL 模块 handle 契约 | mig/wbuf/rr_page_scratch 改挂 ftl_storage_t，数组落 AXI | 内存域布局项 | — |
| ARCH-11 | 2L FTL 模块 handle 契约 | 端口删除后模块直接持有 owner 指针 | 接口封装形态，非失败场景 | — |
| PLAT-07 | 2M USB MSC / 平台 | 各构建配置共用 debug.elf，UT 互相覆盖 | 构建产物命名，非负例 | — |
| ALLOC-06 | 2P allocator | `ftl_gblock` 无入参契约，哨兵可截断成合法 gblock | 契约断言属死亡测试，UT 无机制 | — |
| ALLOC-03 | 2P allocator | 纯转发访问器与 `alloc->blocks[]` 直接访问形成两套坐标 | 访问器坐标收敛，非失败场景 | — |
| ALLOC-07 | 2P allocator | `ftl_tables_t` 五数组打包，单表无法换内存域 | 内存域布局项 | — |
| BIT-01 | 2R 位运算公共宏 | 通用掩码构造器挂在 NAND 层，FTL/FC 各自另写字面量宏 | 宏归属/代码组织项 | — |
| HOST-15 | 2W pending 表 / 完成组 | 写确认 handler 用文件级全局 + `void *`，UT 需 save/restore | 确认 handler 全局收口属结构项，非失败场景 | — |
| CACHE-32 | 2Z cache 长流 + Journal 顺序映射页 | 环满在 release 下静默丢已回执写 | 环深 16 远大于在飞窗口，结构不可达 | — |
| CLI-04 | 2AC CLI 调试前端（`controller/cli.c`） | release help 宣传被禁用的 bbt 子命令 | help 文本需串口人工核对 | — |
| CLI-05 | 2AC CLI 调试前端（`controller/cli.c`） | rd/bbt 语法变更，旧脚本会报 unknown option | 语法变更需人工点测 | — |
| CLI-06 | 2AC CLI 调试前端（`controller/cli.c`） | q/cp/jrn/stat/id/ecc/wear 字段口径未板端核对 | 观测字段需人工对拍 | — |
| POOL-04 | 2AF FC 资源池 / 生命周期 | 空闲链哨兵混用裸常量与具名 ID | 哨兵命名/常量收敛，非失败场景 | — |
| POOL-06 | 2AF FC 资源池 / 生命周期 | 单字段双语义，失败坐标字段无读者 | 字段拆分，非失败场景 | — |
| POOL-07 | 2AF FC 资源池 / 生命周期 | 内部 API 后缀 / 命名不合 H49 | 命名规范项 | — |
| POOL-08 | 2AF FC 资源池 / 生命周期 | FC 注释承载变更史与版本号 | 注释项 | — |
| DP-05 | 2AG 数据面 / NAND dispatch | RAID 边界不完整，恢复后丢 CRC/META | RAID 关闭（R5） | — |
| DP-09 | 2AG 数据面 / NAND dispatch | FC 内 engine↔dispatch 双向依赖，armed 闩锁泄漏进引擎 | engine/dispatch 契约，非失败场景 | — |
| DP-14 | 2AG 数据面 / NAND dispatch | ECC 枚举 0 命名为 BCH，运行时模式与实现不一致 | 枚举命名项 | — |
| SCH-05 | 2AH FC 主循环 / 调度 / CQ | fill_pops 少计链成员 pop，链长 N 少计 N-1 | debug 计数字段，非失败场景 | — |
| SCH-09 | 2AH FC 主循环 / 调度 / CQ | staging helper 无 plane/cw0/cmd_id 范围守卫 | 内部 helper 保持 ASSERT；公共入口保证 plane/cw0/cmd_id 合法（H2） | — |
| SCH-10 | 2AH FC 主循环 / 调度 / CQ | 合成失败完成项忽略 CQ 满，FTL 等不到完成 | 需 CQ 满（结构不可达） | — |
| NANDP-08 | 2AI NAND 器件 / PHY | timing 表部分字段无消费者，改表不生效 | 时序改动需板端回归，非负例 | — |
| NANDP-11 | 2AI NAND 器件 / PHY | timing 表命名 Mode0 实为 Mode5，误改会拖慢高速 | 命名/注释项 | — |
| BUILD-02 | 2AK 构建档 / launch 配置 | launch 配置档位不符，产品/UT 共用 ELF 路径，易烧错镜像 | launch 配置需人工 F5 对横幅 | — |
| L2P-06 | 2C L2P cache | 批量读越界分支被压成 FTL_AGAIN，会永久重试（当前不可达） | 当前 FTL_LBA_MAX 为 8 的倍数，不可达 | — |
| L2P-07 | 2C L2P cache | 顺序读预取白占 cache 行/资源，可能延迟 FLUSH | 预取资源占用是性能/阈值项，非失败场景 | — |
| L2P-20 | 2C L2P cache | line hint 陈旧/冲突，只掉命中率不返回错行 | 只影响命中率，不返回错行；无数据正确性负例 | — |
| L2P-23 | 2C L2P cache | l2p_line_invalidate 不执行「仅 CLEAN」契约 | 需要“断言必须触发”的死亡测试，UT 无此机制 | — |

## 建议批次

1. **D1（17）**：先写，改动只涉及测试侧 helper；
2. **D2（23）**：按 seam 合并成 4 组——① 掉完成/迟到完成；② 引擎级 ECC/编码/HAL/UOP 失败；③ 按 PBA 的数据读注入；④ 按 plane 的 program 完成状态；
3. **D3（43）**：按夹具分三组——并发交错窗口、强制压缩/介质快照、隔离会话（破坏性用例）；
4. **D0（63）**：建议逐条转「信息」或在风险表关闭，不做负例；
5. **D5（1）**：`WL-01` 需先裁决 RETIRED 是否计入擦除统计。

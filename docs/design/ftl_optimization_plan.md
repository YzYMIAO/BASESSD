# FTL 优化计划：对照开源设计

> **本文档性质**：优化**计划**（不含实现）。判据锚定 `docs/design/fc_nand_design.md` §26 对 FTL 层的定义——
> **「逻辑→物理映射、GC、WL、坏块管理、掉电恢复、缓冲调度」**（§26.1）。
> 对照物取自 `docs/reference/`（SPDK FTL / Jasmine / SoftSSD / LeaFTL / dhara / SPDIFTL / FEMU / MQSim …），
> 分析见 `docs/reference/README.md`。
>
> **事实来源与可信度**：现状以**源码通读为准**（下附 `文件:行号`）；凡「设计文档写了、代码没有」的，
> 明确标注为**缺口**；凡**文档与代码不符**的，标为**一致性债**并给出核对建议。

---

## 〇、结论先行

本仓 FTL 的**结构**（纯页映射 + DRAM-less L2P + CLOCK cache + Journal/Checkpoint + 三级水位 + 静态 WL + 三模式调度）
与开源/企业级设计的**大方向是对齐的**，甚至比 dhara/SPIFTL 这类极简 FTL 完整得多。差距不在「有没有这个概念」，
而在三处：

1. **有效性缺口**：一批机制**接上了但没有真正生效**——~~最严重的是擦除计数跨重启归零~~（**P0-1 ✅ 已落地**：
   Wear 表跨重启持久化）；~~Emergency 池死代码~~（P1-3 ✅ 定性为文档误述，非实现缺陷）；
   ~~Metadata Directory 容量 128 < 2048 region（被截断）~~ ⇒ 该描述**已过时**（常量早已 = `L2P_REGIONS_PER_INSTANCE`），
   实际缺陷是**元数据就地覆盖**（P0-2 ✅ 已修）。~~**剩余**：L2P 日志块压缩（**P0-3 ⬜ 未做**，长时压测前必须补）、
   rr 持久化（⬜）。~~ ⇒ **两者均已落地（v0.65，未上板）**：P0-3 = `metadata_dir_compact`；rr = 三字段并入
   Wear 条目（8B→12B，`FTL_MEDIA_LAYOUT_VERSION` 2→3）。**本轮有效性缺口已清空。**
2. **算法缺口**：~~GC victim 单维贪心~~（P1-1 ✅ 已加权）；~~CLOCK 无 thrashing 降级~~（P1-4 ✅ 已落地）；
   ~~GC 触发只落 3/6 条~~ ⇒ **5/6 条**（P1-2 ✅；T6 碎片率**刻意不实现**，见 `gc.h:60`）。
3. **一致性债**：~~映射粒度（设计 CW 级 512B vs 代码页级 2048B）~~ ⇒ **已迁移（波 2 序 2，未上板）**；
   ~~Checkpoint 注释（全量 vs 增量）~~ ⇒ **已订正**（全量快照语义 + 增量 flush）。
   ~~L2P 表尺寸（相对物理页疑似 4× 超额）~~ ⇒ **已核对：非缺陷**（表按目标 IU = 页/4 = 512B 定尺寸、
   与主流同构，见 §九 波 1）。

**优先级建议**：先补 P0（有效性），再补 P1（算法），P2/P3 视内存压力与学习目标排期。

> **全部未排期项一览**（偏离 / 缺口 / 债 / 边界）见 **§八 登记未排期总表**；
> **以主流为准的执行顺序（早期方案必改）**见 **§九**。

---

## 一、本仓 FTL 现状快照（源码为准）

| 维度 | 现状 | 证据 |
|---|---|---|
| 映射粒度 | **IU 级（1 LBA = 1 CW = 512B = 页/4）**——**已迁移（波 2 序 2，未上板）**：1 次 512B 写更新 1 条 L2P entry + 1 条 journal；一个 pair 窗口 8 LBA = plane0 页 + plane1 页 | `ftl_config.h §三`；`ftl.c ftl_issue_subop/ftl_confirm_write`；`request.c` |
| L2P 存储 | NAND 元数据区，**DRAM-less**；全表 2048 region × 512 entry × 4B = **4MB** | `l2p_cache.h:29-51` |
| L2P 位置解析 | Metadata Directory（`L2P_REGIONS_PER_INSTANCE` = 2048 entry）动态分配 + 哨兵层（0-15 块）回退 | `metadata.c:35-355`；`metadata.h`（`ftl_l2p_region_placement`）|
| L2P cache | SRAM **64 region** CLOCK（近似 LRU），write-back + journal | `l2p_cache.c:78-132,239-264` |
| **thrashing 检测** | ~~**无**（只有 hit/miss 计数）~~ ⇒ P1-4 已落地：窗口化 miss 率 + 滞回 + 停主动 flush | `l2p_cache.{h,c}`、`ftl.c` |
| GC victim | **多维加权评分**（P1-1 ✅ 已落地）：有效页 / 磨损 / 温度 / 时间 四维（权重 500/200/200/100） | `gc.h:46-49,111` |
| GC 触发 | **3 条**（P1-2 ✅ + **v0.63 收敛**）：T1 水位 / T4 紧急 / T5 磨损均衡；**T2 写入、T3 定时、T6 碎片率均不实现**（T2/T3 参考层零出处且求值点不可达，见 v0.63） | `gc.h`；`gc.c`（`gc_check_triggers`） |
| 静态 WL | **已实现**（三条件触发 + 冷块 victim + read_count 权重） | `wear_leveling.c:14-306`；`ftl.c:392` |
| 动态 WL | allocator 每 plane 选擦除最少块 | `allocator.c:119-148` |
| **擦除计数持久化** | **有**（P0-1 ✅ 已落地）：Wear 表落块 62（页式追加日志），开机回放回填 BST 的 `erase_count`/`read_count` | `include/ftl/wear.h`、`src/ftl/wear.c`；`ftl.c:586` |
| Free Pool | **单一 free 池 + OP 池**；三级**水位**（P1-3 ✅ 定案）；`pre_cond_count ≡ free_count`（**非缺陷**，GC 先擦后归还） | `allocator.c`；`gc.h` |
| 温度分区 | Hot/Cold 两个 Open SB | `allocator.c:43-44,348` |
| Super Block | 双 plane **各自独立选块**（Policy B，v0.12）；不再要求 `block1 = block0` 同号 | `allocator.h:59,192`；`allocator.c:152,192` |
| Journal | **4 块**环形 log（`JOURNAL_BLOCKS = CW_NUM`），条目 **16B**（packed：lba + old/new pba + seq），**127/page**（推导）；truncate 已消除双倍擦除（P1-6 ✅） | `journal.h:35-59`；`metadata.h` |
| Checkpoint | **全量快照**语义 + **增量 flush**（只写脏 region）；池 8 块环形，一条 = 1 页 | `checkpoint.c:63-93`；`checkpoint.h:62` |
| 掉电恢复 | fresh/clean/power-failure 三类；closed SB 重建 = **全 NAND 扫描 + L2P 反查** | `power_failure_recovery.c:30-341,563-634` |
| 调度/QoS | IDLE/NORMAL/BUSY，按 `pending+completion` 占用判空闲度 | `ftl.c:510-558` |
| 流水线 | pending 深 32 + completion 深 32，异步提交 + 完成泵 | `pending.h:20`；`completion_table.h:38` |
| FTL↔FC | 用户数据走**完整 CRC+加扰+ECC(BCH)**；元数据走 **per-CW CRC + ECC（v0.62 起开启 CRC）** | `ftl.c:632-637,741-746` |
| BST | 条目 **20B** × 4096 = **80KB 全常驻 SRAM**，**无位图表**（v0.55 位图搬出条目；v0.56 删死字段 24B→20B；v0.60 删位图表 64KB → 切 DRAM-less approach ③） | `bst.h` |
| Read Recovery | Stage 1/2 有；**Stage 3（XOR/RAID）未实现** | `read_recovery.c:263` |

---

## 二、与开源设计的差异（逐维度对照）

| 维度 | 本仓 | SPDK FTL | Jasmine | dhara/SPIFTL | 差异性质 |
|---|---|---|---|---|---|
| L2P 存储 | NAND（DRAM-less） | **DRAM/缓存设备**（默认常驻 2GiB MRU，溢出换页） | DRAM 全表 | 无表/块级 | **路线不同**（本仓对标 UFS/eMMC，SPDK 对标有 DRAM 的企业盘） |
| 映射粒度 | **512B = 页/4（IU = 1 CW）** | **4K = 页/4** | 页 | 块 | 本仓与 SPDK **同构**（皆 = 页/4）——**代码已迁移（波 2 序 2，未上板）** |
| GC victim | 有效页最少（单维） | **validity ratio**（band） | Greedy 等 | 整块 | 本仓**少一维以上** |
| 数据年龄/序号 | `last_pe_ts`（P1-1 已消费；~~`gc_generation`~~ v0.56 删） | **seq_id**（band 级，恢复用） | — | — | 本仓**未成体系** |
| 恢复机制 | Journal + Checkpoint | **P2L 重放**（无 journal） | 重建 | 扫描 | 路线不同，本仓更接近消费级 |
| Free Pool | 单池 + OP + 三级**水位** | nvcache chunk + band 状态机 | — | — | 本仓**三级水位已生效**（v0.52 定案） |
| 多流/温度 | Hot/Cold 双 Open SB | band 顺序写 | — | — | 本仓对齐消费级，**弱于企业级多流** |
| 学习型压缩 | 无 | 无 | 无 | 无 | **LeaFTL 是前沿方向**，本仓未涉及 |
| 有效性基建（计数持久化） | ~~**缺**（归零）~~ ⇒ **P0-1 ✅ 已落地**：Wear 表（块 62）持久化 `erase_count`/`read_count`，v0.65 起并入 rr 三字段 | 有（shm 脏跟踪） | 有 | 有 | ~~本仓**落后**~~ ⇒ **已补齐** |

**一句话**：本仓在「DRAM-less 路线 + 分层结构」上不落后；~~**落后的是 GC/WL 的算法深度与计数持久化**~~
⇒ **计数持久化已补齐（P0-1/v0.65）**；剩余差距是 GC/WL 的算法深度（波 4 未排）。

---

## 三、差异分类

**A. 已对齐/领先（不动）**
- 纯页映射、DRAM-less L2P + SRAM region cache、Journal+Checkpoint 双保险、Hot/Cold 温度分区、
  三模式后台调度、FTL 走 FC 完整保护路径（CRC/ECC/加扰）。

**B. 有实现但弱（补强）**
- ~~GC victim 单维贪心~~（P1-1 ✅）；~~GC 触发仅 3/6~~（P1-2 ✅ 5/6，T6 刻意不实现）；
  ~~CLOCK 无降级~~（P1-4 ✅）；~~Super Block 同号连坐~~（Policy B ✅ 独立选块）；
  ~~Journal 2 块寿命瓶颈~~（P1-6 ✅ 双倍擦除已修，4 块环）。

**C. 设计有、代码无（缺口）**
- ~~Emergency Free Pool~~（P1-3 文档误述）；~~GC 多维加权~~（P1-1）；~~thrashing 检测~~（P1-4）；GC Bitmap（Layer 2）；Prefetch；
  Read Recovery Stage 3（**且 RAID 开关必须保持 0，见 §六**）。

**D. 有效性缺口（最该先修）**
- ~~擦除计数/read_count/rr 不持久化~~ ⇒ **P0-1 ✅ 已落地**：`erase_count`/`read_count` 由 Wear 表持久化
  （**rr 仍 ⬜ 未持久化**，登记为后续）；
- ~~Metadata Directory 容量 128 < 2048 region~~ ⇒ **该描述已过时**（常量早已 = `L2P_REGIONS_PER_INSTANCE`）。
  实际缺陷是**元数据就地覆盖**（NAND 只能 1→0，二次编程 = 静默损坏）+ Anchor 从未落盘 +
  Checkpoint 二次写即坏（→ P0-2 ✅）；
- ~~**元数据日志块抗碎片**：`metadata_dir_reclaim` 只回收活计数为 0 的块 ⇒ 挂着冷 region 的块永久占块~~
  ⇒ **P0-3 ✅ 已落地（v0.65）**：`metadata_dir_compact` 给「多活」日志块一条出路（victim = `l2p_live` 最小的
  非当前块，搬活 region 后 **Anchor 先落盘、再擦**）。**D 档缺口至此全部闭合。**

**E. 一致性债（订正文档或代码）**
- ~~映射粒度（§5.1 写 CW 级 512B vs 代码页级 2048B）~~ ⇒ **已迁移（波 2 序 2，未上板）**：数据路径
  升到 IU = 1 CW = 512B，与设计/主流同构；声明地址空间回到 1,048,576 × 512B = **512 MiB = 容量**。
  波 1 核对结论（非表设计缺陷、是在途迁移）见 §九 波 1。
- ~~Checkpoint 注释（全量 vs 增量）~~ ⇒ **已订正**（全量快照语义 + 增量 flush，见 `checkpoint.h:62`）；
- ~~L2P 表 1,048,576 entry 相对 262,144 物理页疑似 4× 超额（若按页级映射）~~ ⇒ **已核对：非缺陷**。
  表按**映射单元（IU）数**定尺寸（IU = 页/4 = 512B ⇒ 4 × 页数），与主流主控同构（主流 IU = 页/4）；
  「4×」只在相对**物理页数**而非 IU 数时才看似超额。**结论见 §九 波 1。**
- `ftl_boot_scan_write_pointer` 死代码（旧单 SB 路径，主路径不调用）且 `buf[16]` 传给按 2048B 拷贝的
  raw read（风险，已自承）⇒ 登记 §8.4。

---

## 四、优化方向（按优先级）

### P0 — 有效性（不做则「机制在跑但没生效」）

**P0-1 计数持久化（最高优先）** — ✅ **已落地**（2026-10-12）
- **问题**：`ftl.c` 每次 `ftl_init` memset 全 BST ⇒ `erase_count`/`read_count`/`program_count` 归零。
  静态 WL 的「max-min ≥ 5000」、动态 WL 的「选擦除最少块」、read-disturb 预测**跨重启全部失效**。
- **对照**：SPDK FTL 用 shm 跟踪脏页；Jasmine/企业级持久化 block metadata。
- **落地方式**：新增 `include/ftl/wear.h` + `src/ftl/wear.c` —— Wear 表落在已预留的固定块
  `WEAR_BLOCK`（**v0.50 布局后 = 62**；条目 8B、快照 17 页），介质上是**块内页式追加日志**（sequence + 页序，与 Anchor/Journal
  同一套日志惯例；绝对值 + 最新胜），而非"每次整表重写"——块 62 只有 64 页、全量快照 17 页，
  整表重写会让它 ~3.8 次落盘就被擦一次（磨损远超数据块）。
  开机 `wear_init` 逐页回放回填 BST（非致命：无有效页 = 冷启动）；运行期 5 个变更点
  `wear_mark_dirty` 置脏位，`ftl_service_idle`/`normal` 里 `wear_kick` 按节拍
  （`WEAR_FLUSH_INTERVAL_MS` 120 s 或 `WEAR_FLUSH_ERASE_DELTA` 1024 次写入）批量落盘。
  持久化 `erase_count`/`read_count` 两个**有消费方**的字段（条目 8B）；
  `last_pe_ts` 不落（时间戳跨重启无意义）；`rr`（3B）登记为后续扩展。
  ⚠ v0.56 后 `program_count` 已从 BST 删除，持久化槽一并撤出。
  未复用 `METADATA_TYPE_ERASE_COUNT`：Wear 块位置固定，无需走 Directory 登记（同 BBT）。
- **代价**：两次落盘之间掉电丢 ≤ 1024 次擦除 / ≤ 120 s 读计数（软状态，只退化 WL 精度）。
- **判据**：断电重启后 `erase_count` 非零且与掉电前一致（需掉电注入或人为复位验证）。
  开机 `[WEAR] seq=… pages=… max_ec=… min_ec=…` 是观测口。

**P0-2 元数据写模型统一（追加日志 + 块池轮转）** — ✅ **已落地**（2026-10-12）
- **问题（本条原文已过时，但真问题更严重）**：`METADATA_DIR_ENTRIES` 早已改成
  `= L2P_REGIONS_PER_INSTANCE`（2048），「循环被截断」不复存在。但那次「扩容」把机制从
  「半工作」翻成「**全死**」：`metadata_dir_fill_fixed` 把 2048 个 region **全部预登记**
  ⇒ `metadata_dir_lookup` 永远命中 ⇒ `dir->dirty` 恒 0 ⇒ `metadata_dir_flush_to_anchor`
  早退 ⇒ **Anchor 块一次都没写过** ⇒ `metadata_dir_load` 永远失败。
- **真根缺陷在下面一层**：`l2p_cache_flush_one` 把每个 L2P region 写回**固定地址且从不擦除**。
  NAND 只能 1→0 ⇒ 对已编程页二次编程 = 新数据 **AND** 旧数据 ⇒ **静默映射损坏**。
  同病遍布全部元数据：全仓 `ftl_sync_erase_block` 的调用者原先只有 `journal.c` / `gc.c` /
  `wear.c` —— **没有任何 L2P / Anchor / BBT / Checkpoint 块被擦过**。
- **顺带挖出的第二个致命项**：`checkpoint_create` 每次重写**同 6 页**，第二次起头即损坏 ⇒
  `checkpoint_find_latest` 失败 ⇒ `ftl_boot_fresh_start` ⇒ **映射全丢**。而 T3 定时触发是
  「elapsed ≥ 10 s 且 IDLE/NORMAL」，`ftl_service_idle` 在 `inflight==0` 时必被调
  ⇒ **板子空闲跑过 10 s 就必中**（不是潜伏，只是从未断言）。
- **落地方式（用户定：B 方案，按存储主控方向）**：全部元数据统一到一套写模型 ——
  **只写擦除过的空页 + 每条 record 带单调 sequence + 池内块轮转（满则擦池内下一块）+
  读取取「整条校验通过」中 sequence 最大者**。共用原语 `metadata_ring_addr` /
  `metadata_ring_locate` / `metadata_ring_prepare_write`（`src/ftl/metadata.c`）。
  - **L2P region**：日志块（从 Free Pool 取整块，`allocator_alloc_reserved_block`），
    64 槽/块；`l2p_cache_flush_one` 每次 flush 无条件追加；旧块活 region 归零即归还。
  - **Anchor 池**：块 16-47（**N=32**，用户定），一条 8 页，256 槽。
  - **Checkpoint 池**：块 48-55（8 块，一条 1 页）；**写序重排**为
    ① 快照 bitmap → ② flush 脏 region → ③ flush Anchor → ④ 写 header → ⑤ 截断 journal
    （旧序 header 早于 Anchor ⇒ 两者之间掉电会「header 新 / Directory 旧」⇒ 丢更新）。
  - **BBT 池**：块 56-57（2 块，一条 1 页）。
  - **Journal 不动**（4 块环 + 擦除 + 两遍重放本来就是对的；只挪块号 58-61）。
  - **块布局**：L2P 基础层 0-15 是**哨兵层（永不写入）**，读回全 0xFF = 未映射；
    `FTL_METADATA_BLOCK_END` 27 → **63**（物理 126/4096 = 3.1% 系统区）。
- **代价**：池满擦旧块 ⇒ 元数据有**回收成本**（原先文档写「不需要 GC」是错的，见
  `ftl_design.md` §3.3 订正）。Anchor 每块擦一次 ≈65,536 次主机写（8 份/块）⇒ 磨损是
  数据块的 **8×**（≈13 TB 全盘写，本仓不是约束；选 N=32 是为**主控形态**）。
- **未做（登记）**：**阶段 3 L2P 日志块压缩**——若某日志块混着「永不再写」的冷 region
  （63 冷 + 1 热），它永远不归零 ⇒ 永久占块；碎片化负载下池会耗尽。补法 = 池紧张时取
  `l2p_live` 最小的块作 victim 搬活 region 再擦（需「正在回收」重入标志）。
  **长时压测前必须补**。
- **判据（用户定：两者都要）**：
  ① 同一 region 连续两次 flush 后读回正确（**改前必失败**）；
  ② `region_id > 128` 的 LBA 写 → flush → 读回正确，且该 region 的 PAA 落在
  `block ≥ FTL_METADATA_BLOCK_END`（动态块）；
  ③ 复位后数据仍在（`[META] anchor_seq` 非 0、`dyn` 非 0）——**根因已闭合（v0.61）**：
  原先必然失败的原因是 `metadata.c` 的 `ANCHOR_PAGE_CRC_LEN = sizeof(...) - 4` 把 `crc`
  字段**自己**也算进覆盖面（`crc` 实际在 offset 1808，其后还有 236B padding）⇒ 写时（crc=0）
  与读回时（crc≠0）算出的值必然不等 ⇒ `anchor_page_valid` 恒 0 ⇒ `metadata_dir_load` 永远
  失败 ⇒ 每次开机回退 `metadata_dir_init`、**L2P 映射跨重启全丢**。v0.61 改为
  `offsetof(metadata_anchor_page_t, crc)`（=1812），自校验成立。详见 `ftl_revision_history.md v0.61`。
  观测口 = 开机 `[META] anchor_seq=… dyn=… cur_blk=… live_blks=…`。
- ⚠ **介质格式变更** ⇒ ~~上板前**必须先擦除芯片**~~ **v0.61 起由开机自动格式化兜底**
  （介质布局版本标记不符 ⇒ `ftl_media_format` 全片擦 + 保留出厂坏块 + 写当前版本标记）；
  手动 `bbt --erase` 仍可作兜底。

**P0-3 L2P 日志块压缩（抗碎片）** — ✅ **已落地（v0.65，2026-10-13，未上板）**

> **落地实现 = `metadata_dir_compact`（`src/ftl/metadata.c`）**：victim = `l2p_live` **最小**的非当前块；
> 把 victim 的活 region 逐个搬进**当前**日志块；**触发点**在「刚换新日志块 + Free Pool 低水位」；
> `dir->compacting` 防重入并抑制压缩期间的逐次立即回收。顺序照下方「方向」的纪律 ——
> **搬移 → `commit_region` → flush Anchor → 擦 victim → 归还 Free Pool**（④ 必须在 ③ 之后）。
> ⚠ 无新增介质格式变更（复用同一套日志块 + Directory 语义）。
> 判据见下（原判据照用）；**依赖「P0-2 先上板验证」**仍然成立，见「依赖」条。

- **问题（原文留档）**：`metadata_dir_reclaim`（`metadata.c:573-596`）只回收 `l2p_live[g] == 0` 的块：

  ```c
  for (g = 0U; g < FTL_BLOCK_TOTAL; g++) {
      if (dir->l2p_live[g] != 0U) continue;   /* ← 有一个冷 region 就永远不回收 */
      ...
  }
  ```

  一个日志块只要挂着一个**再也不会被改写**的冷 region，它就**永久占块**。
  场景：顺序写满 2048 个 region 用掉 32 块，之后只反复写其中一小撮 ⇒ 没被碰到的块全卡住，
  Free Pool 慢性流失。**泄漏上限 = 「每个被引用的块至少 1 个活 region」⇒ 最多 2048 块**
  （每实例共 `FTL_BLOCK_TOTAL` = 2048 × 2 plane = **4096 块** ⇒ 最坏近**半个 die**）。
  稳态随机写会自愈（region 反复改写 ⇒ 旧块自然归零），**碎片化负载不会**。
- **方向**（照企业级「日志块回收」惯例）：
  - **触发**：`allocator_alloc_reserved_block` 取不到块（或 free 水位低于阈值）且**无** `l2p_live == 0` 的块；
  - **victim**：`l2p_live` **最小**的块（排除当前写入块）；
  - **搬移**：把 victim 的活 region 逐个读回 → 写进**当前**日志块（`metadata_dir_append_slot` +
    `metadata_dir_commit_region`）⇒ Directory 就地更新；
  - **顺序（照 F10「擦 victim 前先 flush」的同一纪律）**：
    ① 搬移 → ② `commit_region` 更新 Directory → ③ **flush Anchor** → ④ 擦 victim → ⑤ 归还 Free Pool。
    **④ 必须在 ③ 之后**：否则 Directory 仍指向已擦页 ⇒ 掉电即丢。
  - **重入切断**：搬移会走 `l2p_cache_write → evict → flush → 又要块`。加 `dir->compacting` 标志，
    压缩期间 `metadata_dir_append_slot` 走「不再触发新压缩」的路径（否则递归）。
- **崩溃安全**：搬移途中掉电 ⇒ victim 未擦、新页也已写 ⇒ 同一 region 的 Directory 条目指向新页，
  旧页成为死数据，**两份都在**不破坏「最新有效胜」；恢复后重复一次搬移即可。
- **判据**：
  ① 造冷热混块（写满 32 块后只反复写其中 1 块所含的 region）⇒ 观察 victim 被选、活 region 被搬、
  块被归还（`[META] live_blks` 不单调上升）；
  ② 长时随机写压测：`live_blks` **稳定不发散**，`allocator_alloc_reserved_block` 不返回失败。
- **依赖**：**P0-2 必须先上板验证**。压缩是建在环之上的**新机制**（victim 选择 + 搬移 + 重入切断），
  与环同时上板 ⇒ 失败无法定位是环错还是压缩错。计划 §三 已定「阶段 1 必须一次落地、不可拆」，
  正因为那次**拆掉了「固定位置」安全网**——在这张网验证之前不要再叠一层。
  ⚠ **实际执行偏离了本条**：v0.65 是**同批落地**（未分批）⇒ **上板若失败，先排除环（P0-2）再怀疑压缩**。
- ⚠ 不新增介质格式变更（复用同一套日志块 + Directory 语义）。

### P1 — 算法（对照开源可显著提升）

**P1-1 GC victim 多维加权评分** — ✅ **已落地**（`gc.h:46-49,111`）
- 公式：`cost = GC_WEIGHT_VALID·valid_ratio + GC_WEIGHT_ERASE·erase_ratio + GC_WEIGHT_TEMP·cold_penalty + GC_WEIGHT_AGE·youth_penalty`（权重 500/200/200/100）。
- 对照 SPDK FTL 的 validity ratio（单维）→ 本仓加上**磨损/温度/时间**三维，已超 SPDK 的 GC 选块维度。
- **判据**：同一 victim 集下，加权选出的块擦除次数分布更均匀、COLD 块被搬次数下降。

**P1-2 GC 触发补齐 T4/T5** — ✅ **已落地**（T6 经论证**废弃**；⚠ **T2/T3 亦已删，见 v0.63**）
- T4 强制（`free_count ≤ FREE_POOL_EMERGENCY 8`）、T5 磨损比（`max/min > 10`，加 `max ≥ 64` 地板 + `min > 0`）。
  新增 `gc_check_triggers()` 返回位掩码（`GC_TRIGGER_*`）；观测口 `[GC] trig=0x%02x free=… op=… writes=…`。
  ⚠ **v0.63 订正**：当时的 `gc_check_trigger()` 布尔薄封装**零调用者，已删**；原五路触发里
  T2（写入量整除 1024）/T3（500ms 定时）**参考层零出处且求值点不可达，已删** ⇒ 现存 **T1/T4/T5 三条**，
  求值点统一到 `ftl_gc_evaluate`（IDLE + NORMAL）。
- 复用 `gc_table_extremes()`（P1-1 的 `gc_score_scale` 扩成同时出 min/max 擦除）⇒ T5 不额外扫全表。
- **T6 废弃**：指标方向与"碎片率"标签相反、自激、与 T1/T4 语义重复；行业对高占用率的杠杆是 OP + GC 限速。
  论证见 `ftl_design.md` §4.4 表下注。
- 依赖 P0-1（T5 要持久化擦除计数才有意义）。

**P1-3 Free Pool 三级水位（原题「真三级化」）** — ✅ **已落地**（原判据作废，见下）
- 前提核对：`pre_cond_count ≡ free_count` **不是缺陷**——`gc_erase_and_free` **先擦后归还** ⇒
  池内每个块都已擦除 ⇒ 不存在「预擦除块」与「普通 free 块」的物理区分 ⇒ **没有东西可以分区**。
  设计 §3.7 把「三级水位」画成了「三级池」，是文档误述（v0.52 已更正）。
- 主流对照：真实主控 = **一个已擦除 free pool + 软/硬水位**，另配**独立保留池**（本仓 = OP 池
  性能缓冲 / Spare 池 坏块替换）。没有主流主控在 free 池内部再切「预擦除 / 未擦除」两级。
- **落地**：删 `pre_cond_count`/`emergency_count`/`FTL_BLOCK_STATE_PRE_COND`/`FTL_BLOCK_STATE_EMERGENCY`/
  `allocator_needs_gc`；三级水位直接建在 `free_count` 上（`≥32` 正常 / `8<x<32` 后台 GC / `≤8` 紧急）；
  CLI 状态行去掉 `pre_cond=`。
- **原判据「`pre_cond_count` 与 `free_count` 解耦」作废**——两者本就不该并存，正确做法是删掉冗余的那个。
  「前台 IO 不再等 GC」由水位①保证（池里始终有 ≥32 个已擦除块），不由第二个池保证。

**P1-4 L2P cache thrashing 检测 + 降级** — ✅ **已落地**（初稿三条降级只保留一条，见下）
- 检测：窗口化 miss 率（每 `L2P_THRASH_WINDOW=256` 次访问结算），滞回进入 `≥90%` / 退出 `<70%`；
  累计 `hit_count`/`miss_count` 随开机渐近、反映不了当前行为 ⇒ 降级判定必须看窗口。
- 降级：thrashing 下停 `ftl_service_normal` 的**主动**后台 flush（脏行只由驱逐路径按需写回），
  把 NAND 带宽让给前台、并免掉「本会被驱逐的行被提前 flush、随后再 flush 一次」的重复写。
  持久性不受影响：L2P 更新**先入 Journal**，且 `checkpoint_create` 无条件 flush 全部脏 region。
- 观测口：状态翻转时 `[L2P] thrash=… miss=…% hit=… miss_total=…`；CLI 状态行加 `thrash=%u miss=%u%%`。
- **刻意不实现**：① write-through（本仓 L2P 是**整 region 页追加**，改 1 个 entry 也要 RMW 读整页
  ⇒ 省不掉 load，还丢掉 write-back 的合并收益）；② 预取连续 region（依赖空间局部性，而 thrashing
  成因是**随机**访问 ⇒ 正交）。
- **判据核对**：原判据「随机写压测下不出现活锁」由**写路径紧急 GC**（v0.33）结构性保证；本轮
  降级是**降本**（少白写、让带宽）而非防活锁 ⇒ 该判据属上板压力观测，非纯 RAM UT 可判。

**P1-5 Super Block 独立块号** — ✅ **早已实现**（原「现状」描述过期，无需改动）
- 核对源码：`allocator_open_sb`（`allocator.c`）**逐 plane 独立选块**——每个 plane 各调
  `allocator_select_plane_block(alloc, plane)`（按块号扫 BST 的 FREE 位取 `erase_count` 最小者），
  结果存 `super_block_t.plane_blocks[PLANE_NUM]`；`FTL_BLOCK_NO_PLANE` 哨兵承载重建产生的退化
  单 plane 条目（`super_block_has_plane()` 守卫）。
- 即：SB 内两 plane 的物理块号**互不绑定**，plane0 坏块**不连坐** plane1。
- 该实现属 **Policy B**（v12 `cc09580`+`1927762`「两 plane 独立选块 + 坏块替换」），非 P1 批次产物。
- ⇒ **本批无改动**；原判据「人为标坏 plane0 某块，验证 plane1 同号好块仍可用」已由 Policy B 覆盖。

**P1-6 Journal 寿命瓶颈** — ✅ **已落地**（两条前提过期，真缺陷是「环形块双倍擦除」，见下）
- 前提①过期：「2 块 ≈ 800GB」——**实现早已是 4 块**（`JOURNAL_BLOCKS = CW_NUM = 4`，块 58-61），
  设计自荐的「2→4」本就落地 ⇒ 寿命 ~1.6 TB。文档 §14.1 的「2 块」已更正。
- 前提②过期：「region 更新合并」**本就固有**——region cache 把同 region 的多次写合并进一条脏行、
  只 flush 一次，无需改动。
- **真缺陷（已修）**：**环形块双倍擦除**。`journal_truncate` 擦「环形下一块」，`journal_flush_page`
  回切到该块时**又擦一次** ⇒ 同一块每圈擦两次 ⇒ Journal 磨损凭空 +25%，正打在 §14.1 认定的
  寿命瓶颈上。修法：块擦除**收敛到唯一一点**（`flush_page` 回切），`journal_truncate` 只**预擦除 +
  记账** `pre_erased_block`，回切时命中记账则跳过擦除。
- **附带**：`up_to_sequence` 不再是死形参——水位不足以覆盖全部已写条目时**拒绝截断**（最旧块可能
  仍持有回放需要的条目）；`journal_append` 增加**同一 LBA 合并**（页缓冲内就地更新 new_pba，
  不新增条目、不消耗 sequence）⇒ 覆盖写工作集下减少 Journal 页数。
- **判据核对**：原判据「寿命估算翻倍」已由「4 块」满足；「journal 水位不长期贴顶」属上板压力观测，
  非纯 RAM UT 可判。⚠ 无介质格式变更（Journal 块布局/条目格式未变）。

### P2 — 内存/产品化

**P2-1 BST 产品化裁剪**（设计 §19 档三） — ✅ **已无剩余动作（v0.60）**
- ✅ **剩余唯一杠杆已消解（v0.60，2026-10-13）**：原「160KB → ~100KB（核心 40KB + valid_map 64KB）」
  的两半都已落地——条目 24B→**20B**（v0.56 删死字段），位图 **64KB→0**（v0.60 切 DRAM-less
  approach ③：删常驻 `valid_map`，有效性逐 CW 由页内 META 反查 L2P 判定，见
  `cw_level_mapping_design.md §八`）。⇒ BST 常驻 = **80KB**（20B × 4096），AXI_RAM 319 → **255 KB**
  （⚠ 该 255 KB 是 **v0.60 当时**的值；此后 v0.61~v0.69 各批新增 + P2-5 表收进 `ftl_context_t.tables`
  ⇒ **当前实测 ~291.3 KB / 384 KB**，见 §8.4）。
- **扩展字段表在 v0.56 后已空**（两个候选冷字段 `program_count`/`gc_generation` 已删、
  `last_pe_ts` 有消费者须常驻）⇒ 已无字段可裁。
- **结论**：P2-1 **已无剩余动作**，仅保留登记（若将来 SRAM 再紧张，杠杆只剩 L2P cache 的
  region 数，属另一项）。

**P2-2 SB 索引粒度** — ✅ **已结案（v0.57），不实现**
- **前提已死**：变更史 v0.48 备注的推导前提是 `allocator.c` 的 `block1 = block0`——SB 两 plane 永远
  配对**同号物理块** ⇒ 两 plane 的 `erase_count`/`state`/`sb_id`/`valid_count` 天然相同 ⇒ per-block
  存是重复的。**v0.12 Policy B 已删掉该前提**：`allocator_open_sb`（`allocator.c:159-189`）**逐 plane
  独立**调 `allocator_select_plane_block(alloc, plane)`，两 plane 是不同物理块号 ⇒ 7 个字段
  （`erase_count`/`last_pe_ts`/`read_count`/`valid_count`/`state`/`sb_id`/`rr`）各自属于不同物理块，
  **无一重复** ⇒ 没有东西可去重。
- **数字双重过期**：备注写「再省 96KB」；当前条目 20B × 4096 = **80KB**，且按 SB 索引（2048 条）
  只是 80KB → 40KB，省下的是**分辨率**而非重复。
- **主流方向相反**：SPDK FTL（`reference/pages/spdk_ftl.html`）是**两级**——band 级记 sequence ID /
  validity / write pointer / OPEN-FREE-CLOSE，底层仍是 **per-block BIT**（valid map = 「all the valid
  physical addresses」的位掩码）。本仓 `super_block_t`（SB 级）+ `ftl_block_t`（block 级）**逐项对应**，
  已对齐 ⇒ 改成按 SB 索引 = **删掉 block 级 BIT**，往主流反方向走。
- **「删 `sb_id` 省 8KB」一并撤回**：删掉 2B 的 `sb_id` 后 `ftl_block_t` **仍是 20B**
  （4+4+2+2+2+3 = 17B → u32 对齐 20B）⇒ **省 0 字节**，与 v0.48「删死字段省 0 字节」是**同一个对齐
  陷阱**（见设计 §4.9.6）。且主流 DRAM 控制器**有** block→band 反查表。要真省字节只能砍**活**字段
  （`rr` 3B / `last_pe_ts` 4B），两者都有消费者（慢死预测 / GC youth penalty）⇒ 不砍。
- **唯一剩余偏离**见 **P2-4**。

**P2-3 OOB/spare 通路 + 出厂坏块检测走 OOB**（主流对照） — ⚠ **部分落地（v0.65）**
- **主流做法**（`storage_controller_design_essentials.md:420-424,146`，自述无外部引文）：出厂坏块在
  **初始化时读每块首页的 OOB/spare 坏块标记**建 BBT；OOB/spare 区放 **ECC / 元数据 / 坏块标记**。
- ~~**本仓偏离**：`bbt.c:145-153` 明说「FC 层没有独立 OOB 读取接口」⇒ 出厂坏块检测**靠「读失败 / 全 FF」
  替代**~~ ⇒ ✅ **已消解（v0.65）**：`ftl_sync_read_column`（`OP_READ_COLUMN`，显式列地址）落地，
  `bbt_scan_factory` 改读 **Page 0 列 2048 的 spare 首字节**（`mark[0] != 0xFF`，Micron AN1819 判据），
  且**先读后擦**。根因（FC `cw` 只有 2 位 ⇒ 列上限 1584 够不到 spare 列 2048）由列地址走命令 `param` 绕开。
- **残留（未排期）**：器件 **64B/页 物理 spare 区**仍未用于存 FTL 元数据——本仓 OOB=META 走的是
  **页内 META**（USER 区前的 4B，见 `cw_level_mapping_design.md §六`），物理 spare 除出厂标记外全空置。

**P2-4 band 级 sequence ID + validity 聚合**（SPDK 对照，仅登记，不实现）
- **主流**（`reference/pages/spdk_ftl.html` 逐字）：每个 band 记 "its **sequence ID** (to detect the
  relative age of the data), as well as their validity"；且 "The L2P mapping can be restored from the SSD
  by reading this information **in order from all bands based on the sequence IDs**"。即 band 级**另存**
  validity 聚合，不只存序号。
- **本仓偏离**：`super_block_t` 无序号字段；数据年龄由 `temperature`（HOT/COLD，粗粒度）近似；
  validity **不存**、由两 plane 的 `ftl_block_t.valid_count` 求和派生（语义等价，O(PLANE_NUM)）。
- **为何不实现（消费者被取代）**：SPDK 的 sequence ID **唯一消费者是「按 band 序号重放重建 L2P」**；
  本仓掉电恢复走 **Journal/Checkpoint 的 `page_seq` 排序**（`power_failure_recovery.c`，排序粒度是
  **日志页**而非 band，比 band 级更精确）⇒ 该消费者在本仓不存在，加了就是**死字段**（v0.56 教训）。
  GC victim 侧 SPDK 用的是 **validity ratio**（"The lower the ratio, the higher the chance the band will
  be chosen for gc"），本仓 `GC_WEIGHT_VALID` 已对齐，**不需要**序号。

**P2-5 FTL 表所有权（HIL 传指针 vs 固件自持）** — ✅ **已落地（v0.65，见 §8.1）**
- **主流做法**：主流主控的 FTL 是**固件本体**，表是固件自己的固定内存区——OpenSSD `address_translation.c`
  用**全局数组**、SPDK `spdk_ftl_dev` 实例**内含**表（`storage_controller_learning.md:87`、
  `reference/pages/spdk_ftl.html`）。**主流固件里没有「外层调用者」给 FTL 喂表指针**。
- ~~**本仓偏离**：HIL（`src/main.c`）声明大数组、经 `ftl_init_params_t` **传指针**进 FTL，FTL 不持有表
  （`include/ftl/allocator.h:84-100`、`bst.h:20`）。~~
  ⇒ ✅ **已改**：五大数组（BST / free+op / closed / spare）移进 `ftl_context_t.tables`（`ftl_tables_t`），
  `ftl_init_params_t` **整个结构体删除**；`ftl_init(ftl, topo)` / `allocator_init(alloc, topo, tables)`。
- **根因（三条，均本仓特有）**：① 多实例（F6）——`g_ftl[CH][CE][LUN]`，每实例要一份独立表
  （`bst.h:22`）；② **MCU 分段 RAM**——BST（当时 224KB，现 80KB）塞不进 SRAM1，被迫 BST→AXI_RAM / 池子→SRAM2 分置
  （`revision_history.md:1108`），放置权落在 HIL；③ FTL 作为 §26 分层中 HIL 之下的**层**。
- ~~**代价**：与主流「固件自持表」形态不同；换统一 DRAM 平台无收益。~~
- ~~**改造成本**：高（把指针收进 `ftl_context_t`、FTL 自持并按 id 索引全局表）⇒ **仅登记不排期**。~~
  ⇒ ✅ **已落地（v0.65）**：改造范围 = 把五大数组搬进 `ftl_tables_t` + 删 `ftl_init_params_t`，
  实际改动可界定（`allocator.h` / `ftl.h` / `main.c` / 各模块 `alloc->xxx` 取址）。
- **时间线注**：本设计定于 **v0.30（2026-09-30）**，**早于**「主流 FTL 为判据」转向（P2-1，2026-10-12）
  ⇒ **从没被主流判据审过**（v0.65 落地前）。同类「早于转向、未按主流重审」的还有 v0.28~v0.30 那批
  **形参优化**（把形参打包进结构体，纯 API 形态，未涉主流判据），建议一并补审。

### P3 — 学习/前沿（非必要）

**P3-1 Prefetch 顺序读**（设计 §6.4）：检测顺序访问 → 预加载后续 region。
**P3-2 多流（host stream hint）**：Hot/Cold 双温区 → 多级温区，对照 SPDK/企业级 multi-stream。
**P3-3 学习型 L2P 压缩**：LeaFTL 方向，DRAM-less 前沿，学习价值高、工程量大。

### 验证基建（横切，独立排期）

**V-1 L1 Mock NAND + 掉电注入**（⚠ **用户定调不做**，见 `docs/design/ftl_ut_munit.md §8.2`；下列为原提案留档）
- 关键发现：FTL 的 NAND 访问**唯一收敛到 `ftl_internal_sync_wait` → `fc_submit`** ⇒ 只需把 `fc_submit`
  换成 RAM 桩，write/GC/journal/掉电恢复全部自动可测。
- 掉电注入 = 桩记录「已确认写」点 + 任意处截断 → 重启走 `ftl_boot_recovery` → 断言 L2P 一致。
- **这是 P0/P1 各项「判据」能被自动验证的前提**——强烈建议与 P0 并行。

---

## 五、分阶段实施计划

> ⚠ **本节为旧排序**（S1~S4，判据「先补有效性」）。**以「对齐主流」为准的新排序见 §九**；
> 两者冲突时**以 §九 为准**。

| 阶段 | 内容 | 依赖 | 判据 | 上板 |
|---|---|---|---|---|
| **S1 有效性** | ✅ **已落地（波 0）** P0-1 计数持久化 + P0-2 元数据写模型统一 + E 一致性债订正 | 无 | 重启后计数保持；同一 region 二次 flush 后读回正确；region>128 走动态块 | 是 |
| **S1.5 抗碎片** | ✅ **已落地（v0.65）** P0-3 L2P 日志块压缩（`metadata_dir_compact`） | **S1 上板验证通过** | 冷热混块下 victim 被回收；长时压测 `live_blks` 不发散 | 是 |
| **S2 算法** | ✅ **已落地（波 0）** P1-1 GC 加权 + P1-2 触发补齐 + P1-3 三级水位 | S1.5（T5 需计数） | victim 分布更均匀；水位分级生效 | 是 |
| **S3 稳态** | ~~P1-4 thrashing~~（已落地）+ ~~P1-5 SB 独立块号~~（Policy B 早已实现）+ ~~P1-6 Journal~~（已落地）| S2 | 随机写无活锁；坏块不连坐；journal 水位健康 | 是 |
| **S4 产品化/前沿** | P2-1 BST 裁剪（待触发）；P3-1/2/3（**P2-2 已结案、P2-3/P2-4 仅登记**） | S3 | 按需 | 是 |
| **V 基建（并行）** | L1 Mock NAND + 掉电注入 | 无 | 掉电一致性不变量全绿 | 是 |

> 每阶段沿用本仓纪律：`make format && make format-check` → `make clean && make tests`（0 warning）→
> **上板**跑 `test` 套件 + UART CLI `wr/rd/tr/fl/wp/info/map/crash/bbt/test`。

---

## 六、不做 / 边界

- **`CONFIG_FC_RAID` 保持 0**（用户 2026-10-10 明确）⇒ **Read Recovery Stage 3（XOR）不实现**，
  现状 `read_recovery.c:263` 的「不实现」是**符合边界的**，不是待办。P0/P1 不碰它。
- 不动 `src/fc/**` 的对外接口形态（§26.3 判据：FC 只留硬件 FCC 寄存器表的对应项）。
- 不新增单元测试源文件（项目规则：测试源用户自建）——V-1 只出**方案**，实现由用户定。
- 不改 `L2P_REGIONS_TOTAL`/容量语义（512MiB 不变）；介质格式变更**按 v0.61 惯例登记**（`FTL_MEDIA_LAYOUT_VERSION`
  自增 ⇒ 开机自动格式化兜底，见 §七.6）。
- 不为「对齐开源」而引入 DRAM 常驻 L2P（违背本仓 DRAM-less 定位）。

---

## 七、风险与诚实登记

1. ~~**映射粒度存疑**（§三 E）~~ ⇒ **已核对（§九 波 1，2026-10-13）：非表设计缺陷**。表 1,048,576 entry
   按**目标 IU = 页/4 = 512B** 定尺寸（与主流同构），**表不改小**；**代码数据路径已迁移到 IU 级**
   （v0.59，§九 波 2 序 2）⇒ 声明空间回到 `1,048,576 × 512B = 512 MiB = 容量`，原先「2 GiB 四倍超额」已消除。
2. **计数持久化**（P0-1）：**不新增**介质格式变更——Wear 块 62 早在分区表里预留且被
   `ftl_is_metadata_block` 排除，本次只是第一次往里写内容。块号/分区一个没动
   （v24 教训：只有改块号/布局才须重新格式化；本条只改了 `FTL_WEAR_TABLE_BYTES` 的
   逻辑表字节常量，不影响介质寻址）。
3. ~~Emergency 池真三级化~~（P1-3 已判定为文档误述，无此改动）。
4. **P1-1 加权评分**引入浮点/定点的取舍：嵌入式无 FPU 便利 ⇒ 用整数加权（本仓惯例），避免 `%f`（newlib-nano）。
5. 本文档的「现状」以**源码为准**；若与 `ftl_design.md` 冲突，**以代码为准**并在 S1 一并订正文档。
6. **上板前置（P0-2 引入，v0.61 起取消）**：~~**必须先擦除芯片**~~。本次改的是介质格式
   （块布局 + Anchor 分页 + Journal 挪号），旧镜像写下的内容按新布局解读会错位。
   **v0.61 起由开机自动格式化兜底**（`FTL_MEDIA_LAYOUT_VERSION` 不符 ⇒ `ftl_media_format`
   全片擦 + 保留出厂坏块 + 写当前版本标记），不再需要手动 `bbt --erase`。
7. **未做（本轮登记，见 `ftl_revision_history.md` v0.50「登记不做」）**：
   - ~~**阶段 3 / P0-3**（上条）——**长时压测前必须补**~~ ⇒ ✅ **已落地（v0.65）**：`metadata_dir_compact`；
   - **多实例**：`checkpoint.c` / `bbt.c` / `wear.c` 的 file-scope static 游标/序号在多 LUN 下
     所有实例共享一份（`metadata.c` 的 Directory 是 per-instance，不受影响）；
   - **SPDK FTL 式 P2L 重建**（每 band 尾部存 P2L、开机顺序扫描重建 L2P）——替换整层持久化机制的
     大改，登记为后续方向；本仓 `ftl_rebuild_closed_sb_list` 已有雏形；
   - ~~**`ftl_is_metadata_block` 的 per-plane 粒度**~~ ⇒ **✅ v0.69 已修**（原判断「偏保守、
     安全」**是错的**：被掩的块既进不了 Free Pool、又被 F2 重建跳过 ⇒ 落在所有池之外、
     `valid_count` 恒 0，而 L2P 若指向它，覆盖写就撞 `bst.h` 断言。见 §8.3 与 v0.69 ADR）；
   - ~~**`program_count` 无消费方**~~ / ~~**`last_pe_ts` / `gc_generation` 死字段**~~ —— ✅ v0.56 已删
     `program_count`/`bad_reason`/`gc_generation`；`last_pe_ts` 由 P1-1 消费（原判据已过期）；
     **`rr` 仍不持久化**。
8. **文档一致性债**：
   - ~~`ftl_design.md` / `ftl_api_usage.md` 旧布局标识符~~ ⇒ **✅ 本轮已清理**（2026-10-13）：L2P 分区
     0-15、Journal 58-61、Wear 62、Checkpoint 池 8 块等常量已对齐 `metadata.h`；
   - ~~`src/main.c` 两条内存注释仍是旧值~~ ⇒ **✅ 已订正**：`g_ftl` = **270.4 KB**（`.bss.g_ftl` = 0x43968），
     AXI_RAM 实用 **~291.3 KB / 384 KB**（`.bss` 0x48B44 + `.data` 0x1E4 = 298,280 B，
     取 `build/firmware/debug/bin/debug.map`；`src/main.c:131` 有 `_Static_assert` 兜底）。
     ⚠ **P2-5（v0.65）后大表收进 `ftl_context_t.tables`** ⇒ `g_ftl` 一项即含 BST/L2P/池
     （此前的 157.6 KB 是表仍在独立全局时的值）。

---

## 八、登记未排期总表（本仓「已知偏离 / 缺口 / 未做」一览）

> **本节性质**：把散落在 §四（P2/P3）、§六（边界）、§七（风险登记）以及
> `ftl_revision_history.md` 各版「登记不做」中的**全部未排期项**汇成一张总表，供排期/复盘总览。
> **本轮（2026-10-13）新增**：**P2-5**（FTL 表所有权）+ 本节。
>
> **类型**：`偏离`=有主流出处、可改；`缺口`=设计有/机制弱、未生效；`未决`=原因未闭合；
> `债`=文档与代码不符；`边界`=刻意不做（**非待办**）。
> **状态**：`待触发` / `仅登记` / `不实现` / `待补` / `保留未改`。

### 8.1 主流对照偏离（有出处、可改）

| 编号 | 项 | 本仓 vs 主流 | 为何未排期 | 出处 |
|---|---|---|---|---|
| P2-1 | BST 产品化裁剪（§19 档三） | 全常驻 vs 分层裁剪 | ✅ **已消解（v0.60）**：条目 24B→20B + 位图 64KB→0 ⇒ BST 80KB；无剩余杠杆 | 设计 §19 |
| P2-3 | OOB/spare 通路 + 出厂坏块检测走 OOB | 靠「读失败 / 全 FF」vs 读 OOB 标记 | **部分落地（v0.65）**：**出厂坏块检测**改读 Page0 **spare 首字节**（列 2048，`bbt_scan_factory`，只读不擦）。**残留**：器件的 64B/page 物理 **spare 通路**（拿 spare 存 FTL 元数据）仍未用——本仓 OOB=META 走的是**页内 META**（USER 区前的 4B，见 `cw_level_mapping_design.md §六`），物理 spare 全空置 | `essentials:420-424`；`bbt.c:167` |
| P2-4 | band 级 sequence ID + validity 聚合 | 无序号 / validity 派生 vs band 另存 | 消费者被 journal 恢复取代 ⇒ 死字段 | `spdk_ftl.html` |
| **P2-5**（本轮新增） | **FTL 表所有权 = HIL 传指针** | HIL 声明数组传指针 vs 固件自持表 | ✅ **已落地（v0.65）**：BST/free/op/spare/closed 五大数组移进 `ftl_context_t.tables`（`ftl_tables_t`），HIL 只给拓扑、不再传指针——与主流同构（OpenSSD 全局数组 / SPDK `spdk_ftl_dev` 内含表） | `learning.md:87` / `spdk_ftl.html` |
| **P2-6**（CW 迁移新增） | **单 plane 子操作浪费另一 plane 的页** | `allocator_alloc_pair` 一次给**两 plane 同页号**并推进写指针；CW 级子操作常只覆盖半个 pair（如 512B 写只落 plane0）⇒ 另一 plane 该页被占用却从不编程 | ⛔ **不实现（器件/标准约束）**：登记修法（per-plane 独立写指针）**不可实现**——multi-plane 要求两 plane **同页地址**（Micron `MT29F4G08ABADAWP` §Two-Plane Addressing：「The page address bits, PA[5:0], must be identical」；ONFI 3.2 §3.1.1：「The page address shall be the same」），独立指针分叉页号即拆掉 multi-plane 前提。**唯一解 = §七 方案 A 页打包写缓冲（v0.66 已落地）**：同一 pair 窗口的多次写攒成一页再编程 ⇒ 页内利用率回到 100%。GC/WL 双 plane 写 v0.59 已修（`gc.c:579/584/597`、`wear_leveling.c:305/310/320`），残留仅命令形式 | `allocator.c`；`ftl.c ftl_wbuf_*` |
| — | SPDK FTL 式 P2L 重建 | Journal/Checkpoint vs 每 band 尾存 P2L | 替换整层持久化的大改 | v0.50 登记 |
| — | **介质布局版本 + 开机自动格式化**（2026-10-13，v0.61） | ✅ **已落地**：`FTL_MEDIA_LAYOUT_VERSION` 写进 Checkpoint 头 + Anchor 页；版本不符/无记录 ⇒ `ftl_media_format` 自动全片擦（**保留出厂坏块标记**）+ 写当前版本标记。此前每次改介质格式都得手动 `bbt --erase` | `ftl_revision_history.md v0.61` |
| — | **元数据校验和下沉到下层 FC**（2026-10-13，v0.62） | ✅ **已落地**：删 FTL 记录内 CRC（Checkpoint/Anchor/BBT/Wear 四结构体字段 + 算/校验点）+ `FTL_PAGE_CFG_META` 由 `FC_CRC_DISABLE` 翻 `FC_CRC_ENABLE`。判据 = **DRAM-less 主流**（dhara：checksum 归 NAND 层、FTL 只留结构判据）；SPDK 自加 CRC32C 属 DRAM 型分岔，不适用本仓 | `ftl_revision_history.md v0.62` |
| — | **GC 触发对齐 DRAM-less 主流**（2026-10-13，v0.63） | ✅ **已落地**：删 T2（写入量整除 1024）/T3（500ms 定时）——参考层**零出处**且求值点（只在 `ftl_service_normal`）不可达 ⇒ 触发收敛为**空间压力 + 磨损**（T1/T4/T5）；求值点统一到 `ftl_gc_evaluate`（IDLE+NORMAL 共用，BUSY 让路）；顺带修 IDLE 补池循环 `target -= 2U` 的 **uint32 下溢**（deficit 为奇数 ⇒ 回绕 0xFFFFFFFF ⇒ 把 closed 链全搬全擦） | `ftl_revision_history.md v0.63`；SPDK `spdk_ftl.html:180` |

### 8.2 有效性 / 算法缺口

| 编号 | 项 | 现状 | 为何未排期 |
|---|---|---|---|
| P0-3 | L2P 日志块压缩（抗碎片） | ✅ **已落地（v0.65，未上板）**：`metadata_dir_compact` 给「多活」日志块一条出路（`metadata_dir_reclaim` 只回收活计数 0 的块 ⇒ 冷 region 块永久占块，最坏吃近半个 die） | `ftl_revision_history.md v0.65` |
| — | **Anchor 页 CRC 覆盖面缺陷**（`c5cb8a0` 引入，v0.61 修，v0.62 字段整体删除） | ✅ **已消解**：v0.61 修覆盖面（原 `ANCHOR_PAGE_CRC_LEN = sizeof - 4`（2044）把 `crc` 字段自己算进去 ⇒ `anchor_page_valid` **恒 0** ⇒ `metadata_dir_load` 永远失败 ⇒ **L2P 映射跨重启全丢**；改 `offsetof(..., crc)`=1812）；v0.62 起 Anchor 页**不再有 `crc` 字段**，完整性由下层 FC per-CW CRC + ECC 承担 ⇒ 该缺陷从根上不存在 | `ftl_revision_history.md v0.61/v0.62` |
| — | `rr` 持久化 | ✅ **已落地（v0.65，未上板）**：`last_ecc`/`rising_count`/`health` 并入 Wear 条目（8B→12B，布局版本 2→3）；`health` 回放时范围钳制 | `ftl_revision_history.md v0.65` |
| — | GC Bitmap（Layer 2） | 设计有、代码无。**正确性窗口已由 v0.43「擦除前 flush」闭合**；Layer 2 只是掉电后**断点续搬**的性能优化 | 缺口，未排（非正确性） |
| — | **有效性 = DRAM-less approach ③**（2026-10-13，v0.60） | ✅ **已落地**：删常驻页级 `valid_map`（64KB）⇒ 有效性**唯一**真值 = 逐 CW 读**页内 META（=OOB 反向映射）**取回 LBA → 反查 L2P。原先位图覆盖的「整块空洞」主场景改由 `gc_migrate_sb` 的 `valid_count == 0` 快路径覆盖。出处 = DFTL / arXiv 1504.01666 / KIISE / LeaFTL / VPFTL | `cw_level_mapping_design.md §八` |
| P3-1 | Prefetch 顺序读 | 无 | 前沿，未排 |
| P3-2 | 多流 host stream hint | 仅双温区 | 企业级方向，未排 |
| P3-3 | 学习型 L2P 压缩（LeaFTL） | 无 | 工程量大，未排 |
| — | **页打包写缓冲（§七 方案 A）** | ✅ **已落地（v0.66，未上板）**：`ftl_wbuf_t` 把同一 pair 窗口的多次 host 写攒成一页、一次编程（两 plane 有数据即 multi-plane）⇒ 消除单 plane 子操作浪费另一 plane 同页（P2-6）。512B 稀疏写仍走 CW 子集直接编程，但改 `ftl_cmd_write_data_cw` **散射**（合并后位图可能非连续）。掉电**不做数据日志、改「推迟完成」**（journal/L2P 只在 flush 编程成功的 completion 更新） | `ftl_revision_history.md v0.66`；`cw_level_mapping_design.md §七` |

### 8.3 多实例 / 结构性缺口

| 项 | 现状（出处） | 处置 |
|---|---|---|
| ~~`main.c` 只 service 实例 0~~ | ✅ **已修（v0.65）**：泵/实例分离——`ftl_service_pump()`（实例无关）每轮一次 + 对**每一个** `g_ftl[ch][ce][lun]` 调 `ftl_service()` | 已闭合 |
| ~~file-scope static 游标多 LUN 共享~~ | ✅ **已修（v0.65）**：`checkpoint.c`/`bbt.c`/`wear.c` 的 static 游标/序号收进 `ftl->cp` / `alloc.bbt` / `alloc.wear`（per-instance） | 已闭合 |
| `ftl_is_metadata_block` per-plane 粒度 | 按 per-plane 块号比较 ⇒ 连另一 plane 同号块一起排除 | ✅ **已修（v0.69）**：判据加 `plane` 参数（固定区仍与 plane 无关，动态日志块/游标块带 plane 比较）。⚠ 原判「偏保守安全、不修」**是错的**——被掩的块落在所有池之外、`valid_count` 恒 0 ⇒ L2P 悬空 ⇒ 覆盖写撞 `bst.h` 的 `valid_count > 0U` 断言 |
| `read_recovery` 写保留地址 0x89 | `read_recovery.c:83,99` 读重试写保留段、自始空转 | **用户明确不处理** |
| 读失败无条件 `IO_ERROR` | `ftl.c:271-275` 即使 rr 救回也报错 | 「行为等价」保留 + ⚠ 注释，待单独立项 |
| `ftl_trim` 大范围同步阻塞 | 超循环长停 | 范围上界/分块后续定 |
| `ftl_flush` 语义 | = 「落已持久化映射」，**非** NVMe barrier | 语义界定，非缺陷 |
| 跨组 LBA 重叠写序 | 未定义 | 当前 HIL 串行，不成问题 |

### 8.4 文档一致性债

| 项 | 债 | 处置 |
|---|---|---|
| 映射粒度 | §5.1 写 CW 级 vs 代码页级 | **✅ 已迁移（波 2 序 2，未上板）**；波 1 核定为「非缺陷、在途迁移」（`cw_level_mapping_design.md §十一`） |
| Checkpoint 注释 | 头注释「全量」vs 实现增量 | **✅ 已订正**（全量快照语义 + 增量 flush，`checkpoint.h:62`） |
| ~~L2P 表尺寸~~ | ~~1,048,576 entry vs 262,144 物理页 疑似 4× 超额~~ | **波 1 已核：非缺陷**（表按 IU = 页/4 = 512B 定尺寸，与主流同构） |
| ~~`ftl_boot_scan_write_pointer`~~ | ✅ **已删（v0.65）**：死代码（旧单 SB 路径，零调用者）+ `buf[16]` 传给按 2048B 拷贝的 raw read（栈溢出）一并消失 | 已闭合 |
| 旧布局标识符 | `ftl_design.md`/`ftl_api_usage.md` 散落 `L2P_REGIONS_TOTAL` 等 | **✅ 本轮已清理**（2026-10-13） |
| `main.c` 内存注释 | ~~`g_ftl` 标 "131.5KB"；AXI_RAM 标 "≈355.5KB"~~ | **✅ 已订正**：`g_ftl` = **270.4KB**（`.bss.g_ftl` 0x43968，含表，P2-5 后）、AXI_RAM ~**291.3KB**/384KB（`.bss` 0x48B44 + `.data` 0x1E4） |

### 8.5 跨层 / 未决（非 FTL 主体，但影响本仓判据）

| 项 | 现状 | 处置 |
|---|---|---|
| 擦除 op 18→17 未决 | 固件实测零改写（0/28），原因未闭合 | 插桩 `#if 0` 停在 `test_fc_level3.c` |
| ~~FTL UT 未接线~~ | ✅ **已接线（`9ff5a77`）**：`tests/unit/test_ftl.c` = **8 套件 53 例**（v0.74 增至 **55 例**：allocator / watermark / gc_wl / rr / bbt / metadata / pending_ct / group），板端 `test` 跑；开关 `make tests` / `UT=FC` / `UT=FTL` | 方案见 `ftl_ut_munit.md`（FTL 层）+ `hil_ut_design.md`（HIL/集成） |
| F8 / F9 | 依赖上板 | 待上板 |
| v0.58 对外接口对齐 | 未上板 | 待上板 |
| HAL_RCCEx_GetPeriphCLKFreq 被裁 | 恒返回 0（无 FMC/QSPI/USART1 分支） | platform 待修 |
| FMC HiZ 是写方向 | 待上板写压力测试 | 待上板 |
| D4/D5 HIL 队列 + 门铃 + CQ | 更大重构 | 未排 |
| ~~D6 LBA→实例路由上移~~ | ✅ **已裁决（v0.65）**：「主流单一 FTL 前端」经查证**无出处**（仅本仓 D6 表自述；`fc_nand_design.md §26` 不谈 L2P 分布；仓内另有反方向的同类无出处断言）⇒ 按 §九 序 6 明文允许，**保留分片并登记理由**；`CH=CE=LUN=1` 下「单一前端」平凡成立 | 不再是「偏离」 |

### 8.6 边界（刻意不做，**非待办**）

- `CONFIG_FC_RAID` 保持 0 ⇒ Read Recovery Stage 3（XOR）**不实现**（`read_recovery.c:263`）。
- 不引入 DRAM 常驻 L2P（违背 DRAM-less 定位）。
- 不为「对齐开源」而改 `L2P_REGIONS_TOTAL` / 容量语义（512MiB 不变）。

---

## 九、优化顺序（以主流为准；**早期方案必改**）

> **本节性质**：对 §八 全部未排期项**拍一个执行顺序**。判据 = **有出处的主流存储主控 FTL 设计**。
> **与 §五 的关系**：§五（S1~S4）是旧排序（判据「先补有效性」）；本节判据换成「**对齐主流**」，
> **冲突时以本节为准**。
>
> **两类必改**（**不再是「仅登记」**）：
> ① **早期方案偏离主流** —— v0.30 表所有权、早期 FC 无 OOB 通路、**早期映射粒度（页级 → IU 级）**、单实例遗留；
> ② **机制没生效的有效性缺口**。
> **排序原则**：先决核对 → 结构对齐主流（越晚越贵）→ 有效性 → 器件通路 → 算法 → 文档债；
> 边界不改；跨层按依赖穿插。**每波末尾上板**。
>
> ⚠ **当前几何 `CH=CE=LUN=1`（单实例）**：多实例类改动是**潜伏债**（dims>1 才咬），**非活跃缺陷**；
> 且 AXI_RAM 已用 **~291.3 / 384 KB**（`src/main.c:131` 有 `_Static_assert` 挡住加实例）⇒ **多实例前必须先重规划内存**。
> ⚠ **若以「活跃缺陷优先」而非「主流对齐优先」**，则**波 3 的 P0-3 应提到最前**（它是活跃容量泄漏；波 2 多为潜伏债）。
> 本排序按用户指定判据（**主流**）排。

### 波 0 — 已落地批次（2026-10-13 收口；**上板状态见下注**）

| 批次 | 内容 | 结论 |
|---|---|---|
| P0-1 | Wear 表跨重启持久化（块 62 页式追加日志） | ✅ |
| P0-2 | 元数据写模型统一（只写擦除页 + sequence + 池内轮转 + 整条校验） | ✅ |
| P1-1..P1-6 | GC 多维加权 / 触发 5 条 / 三级水位 / thrashing 降级 / SB 独立块号 / Journal 双倍擦除 | ✅ |
| v0.55~v0.58 | BST 位图分离 → 删死字段 → 对外接口对齐主流 | ✅ |
| **v0.59** | **CW 级映射迁移（IU = 512B）** — 见波 2 序 2 | ✅ |
| **v0.60** | **有效性切 DRAM-less approach ③** — 见波 2 序 2.5 | ✅ |
| **v0.61** | **介质布局版本 + 开机自动格式化**（顺带修 Anchor 页 CRC 覆盖面缺陷 ⇒ 跨重启丢映射） | ✅ |
| **v0.62** | **元数据校验和下沉下层 FC**（删 FTL 记录内 CRC + `FTL_PAGE_CFG_META` 翻 `FC_CRC_ENABLE`） | ✅ |
| **v0.63** | **GC 触发对齐 DRAM-less 主流**（删 T2/T3 + 统一求值点 IDLE/NORMAL + 修 IDLE 补池 uint32 下溢） | ✅ |
| **v0.64** | **启动恢复 Journal 写态**：新增 `journal_scan_latest`（只读）/ `journal_resume`，接三条启动路径。真根因 = `journal_init` 复位 RAM 写态但 NAND 环没擦 ⇒ 序号回退 + 二次编程 | ✅ |
| **v0.65** | **波 2 剩余 + 波 3**：P2-5 表所有权（`ftl_context_t.tables`）/ 多实例 static 收进 per-instance / P2-3 出厂坏块走 OOB 列 2048 / **P0-3 `metadata_dir_compact`** / **rr 持久化**（Wear 条目 8B→12B ⇒ 版本 2→3）/ 泵实例分离 / 删 `ftl_boot_scan_write_pointer` 死码 | ✅ |
| **v0.66** | **页打包写缓冲 `ftl_wbuf_t`**（§七 方案 A）——P2-6「单 plane 子操作浪费另一 plane 页」的**唯一解**（两 plane 必须同页地址） | ✅ |
| **v0.67** | `allocator_close_sb` valid 顺序修复——**FTL UT 上板首跑顶出的生产缺陷**（非 UT 自身问题） | ✅ |
| **v0.68** | **GC 求值/日志节拍**（10ms / 1s）+ 心跳带 `reclaimed`/`novictim` 进展计数（一行区分卡死 vs 推进） | ✅ |
| **v0.69** | **`ftl_is_metadata_block` plane 掩码修复**——被掩的块落在所有池之外 ⇒ `valid_count` 恒 0 ⇒ L2P 悬空撞 `bst.h` 断言 | ✅ |
| **v0.70** | **诊断 CLI**：`map <lba>`（L2P + BST + VERDICT）/ `crash [clear]`（查看/清 `.noinit` 记录）。纯新增诊断口，不动算法路径 | ✅ |

> ⚠ **上板前提**：波 0 全部改动**必须一次上板验证**（介质格式已变：v0.59 块布局 / v0.61 版本标记
> + v0.62 字段删除 + **v0.65 Wear 条目 8B→12B ⇒ `FTL_MEDIA_LAYOUT_VERSION` 3**）
> ⇒ 首次上板会触发**自动格式化**，不再需要手动 `bbt --erase`。
> v0.63/v0.68/v0.69/v0.70 **无介质变更**（RAM 侧调度/触发/诊断），可与波 0 同批上板；
> §7 的 GC 配方已随 v0.63 重写。
> **上板进度**：v0.64（§6 主判据过）、v0.69、v0.70（`crash clear` 验证过）**已上板**；
> **v0.65/v0.66/v0.67/v0.68 未单独上板验证**（v0.65 的 P0-3 / rr 持久化判据尚未跑）。
> ⚠ **v0.71 未闭合**：v0.70 上板验证通过（`crash clear` → 复位不再出现），但记录内容是
> **HardFault**（`PC=0x2400038c LR=0x24000b09 CFSR=0x1 HFSR=0x40000000`，PC/LR 落在 `.bss` 内
> ⇒ 控制流被写坏）而非断言，**不知如何复现**。见 `ftl_revision_history.md v0.71` +
> `docs/changelog/diag_crash_cli_board_verify_v71.md`。

### 波 1 — 先决核对（不改代码）✅ **已完成（2026-10-13）**

| 序 | 项 | 做什么 | 为何第一 |
|---|---|---|---|
| 1 | L2P 表尺寸 | 核对 1,048,576 entry vs 262,144 物理页是否 **4× 超额** | 决定映射层要不要重做 ⇒ 所有映射改动的**前提**（§七.1） |

**波 1 结论（2026-10-13）**：**「4× 超额」不是表设计错误**。表按**目标 IU = 页/4 = 512B** 定尺寸
（`ftl_config.h:66` `FTL_IU_BYTES = FC_BUFFER_USER_SIZE`、`:79` `FTL_CW_TOTAL = 4096×64×4`；
`metadata.h:184` `FTL_L2P_TABLE_BYTES` 表 4 MiB），与主流主控 **IU = 页/4** 结构同构 ⇒ **尺寸正确、表不改小**。
真正缺口 = **代码数据路径仍页级（2048B/LBA）**（`FTL_READ_USER_SIZE = 2048`、`request.c:148` /
`ftl.c:909` 缓冲步进、`ftl.c:843` `internal_lba << CW_WIDTH(=2)` 佐证、`lba_to_region = lba>>9` /
`lba_to_entry = lba&511` 无 `/4`），即 `cw_level_mapping_design.md §十一 ⬜` **已登记未落地的 5 项**。
真实副作用 = `FTL_LBA_MAX = 1,048,576`（`ftl.c:981/1045/1123` + `cli.c:154` 强制）声明地址空间
**2 GiB 超容量 512 MiB 四倍**。
**决策（用户 2026-10-13）**：**方向 = 补完 CW 级（IU=512B）数据路径**（对齐主流；表/常量不动）⇒ **波 2 序 2**。
✅ **已落地（v0.59，未上板）**：上述页级数据路径全部升到 IU 级，声明地址空间回到 **512 MiB = 容量**。

### 波 2 — 早期方案对齐主流（**必改**）

**已落地（波 0 内）**：

| 序 | 项 | 改了什么 | 主流依据 |
|---|---|---|---|
| 2 | ~~**CW 级映射迁移**（波 1 结论）~~ ✅ **代码已落地（未上板）** | 数据路径 页级 2048B/LBA → **IU 级 512B/LBA**：请求层步进 / 写路径 4 entry / 读路径 512B 查 / GC·WL 逐 CW / BST per-CW。**实际做法**：器件支持按 CW 编程（FC v27 CW 位图）⇒ 页内允许空洞，稀疏位图走 `ftl_cmd_write_data_cw` 散射。~~不走「页缓冲」~~ ⇒ **v0.66 补上页打包写缓冲（§七 方案 A）**：同一 pair 窗口的多次写攒成一页再编程，消除「单 plane 子操作浪费另一 plane 页」（§8.1 **P2-6**） | 主流 IU = 页/4；本仓页 2048B ⇒ IU = 512B |
| **2.5** | ~~**有效性切 DRAM-less approach ③**~~ ✅ **已落地（v0.60，2026-10-13）** | 删常驻页级 `valid_map`（64KB）⇒ 有效性**唯一**真值 = 逐 CW 读**页内 META（=OOB 反向映射）**取回 LBA → 反查 L2P；`valid_count` 留作块级聚合；整块空洞由 `valid_count == 0` 快路径覆盖。AXI_RAM 319 → **255 KB** | DFTL（OOB 存 LBA）/ arXiv 1504.01666（活页识别三法，DRAM-less 选 ③）/ KIISE（逐页位图放 flash 会引发大量 I/O）/ LeaFTL·VPFTL（P2L 存 OOB + 交叉验证） |

**剩余（本波待做，**均为潜伏债**，dims=1 时不咬）**：✅ **本波全部落地（v0.65，2026-10-13，未上板）**

| 序 | 项 | 改什么 | 主流依据 | 前提 |
|---|---|---|---|---|
| 3 | ~~**P2-5 表所有权**~~ ✅ **已落地（v0.65）** | FTL **自持表**（per-instance 上下文内含 / linker 固定区），HIL 不再传指针 | OpenSSD 全局数组 / SPDK `spdk_ftl_dev` 内含表 | 无 |
| 4 | ~~**多实例化**~~ ✅ **已落地（v0.65）** | `checkpoint.c`/`bbt.c`/`wear.c` static 游标 → per-instance（`alloc.bbt` / `alloc.wear` / `ftl->cp`）；`main.c` 泵/实例分离（`ftl_service_pump` 一次 + 逐实例 `ftl_service`） | 主流 per-channel 独立 | **须先重规划内存**（AXI_RAM ~291.3/384KB，加实例装不下）——当前单实例 `.bss` 0x48B44 + `.data` 0x1E4 = 291.3KB，`_Static_assert` 编译期拦下 |
| 5 | ~~**P2-3 OOB/spare 通路**~~ ✅ **已落地（v0.65）** | 出厂坏块检测改读 **OOB 标记**（列 2048，`OP_READ_COLUMN` 早已存在）；扫描阶段只读不擦 | Micron AN1819「spare 区首字节 = 出厂标记，**擦除即永久丢失**」 | 无（改 FC 命令通路） |
| 6 | ~~**D6 LBA→实例路由**~~ ✅ **已裁决（v0.65）：保留分片模型并登记理由** | 见下方「序 6 裁决」 | ⚠ **「主流单一 FTL 前端」经查证无出处**（仅本仓 D6 表自述；§26 不谈 L2P 分布；仓内还有反方向的同类无出处断言） | 序 4 之后 |

> 3/4 同源（「FTL 作为固件本体、per-instance 自持」），建议**一并做**；且**先做 3/4，再动 UT 接线**
> （夹具依赖表所有权形态，见 `ftl_ut_munit.md`）。
>
> **序 6 裁决**：按本表明文允许的「**或**明确保留分片并登记理由」⇒ **保留分片**。
> 理由：① 分片是**本仓刻意模型**（`cw_level_mapping_design.md` §3.1/§3.2，模型 B 已删）；
> ② 判据要求「有出处」，而「主流是单一前端」**无出处** ⇒ 不得据此改代码（否则是以另一个
> 无出处断言替换刻意设计）；③ 平台现状 `CH=CE=LUN=1` ⇒ 只有一个 FTL 实例，
> 「单一前端」性质**平凡成立**。⇒ 本项**不是**「与主流偏离」，已在 §8.5 订正。
> 详见 `ftl_revision_history.md v0.65`。
> ⚠ 序 2（CW 迁移）**已落地**：`valid_count` 改 per-CW **未增加**内存（仍是 `uint16_t`，值域 0..512）。
> 序 2.5 已随之落地：页级位图**已删**（不再保留为「写过」粗提示）⇒ AXI_RAM **减 64KB**
> （319 → 255 KB，`src/main.c` 的 `_Static_assert` 仍成立且更宽松）。

### 波 3 — 有效性（机制没生效）— ✅ **本波全部落地（v0.65，2026-10-13，未上板）**

| 序 | 项 | 依赖 | 结果 |
|---|---|---|---|
| 7 | ~~**P0-3 L2P 日志块压缩**~~（**长时压测前必须**） | P0-2 上板 | ✅ `metadata_dir_compact`：victim = `l2p_live` 最小非当前块，搬活 region 后 **Anchor 先落盘、再擦**；触发点在「刚换新日志块 + Free Pool 低水位」；`dir->compacting` 防重入 + 抑制逐次立即回收 |
| 8 | ~~**rr 持久化**~~（慢死预测跨重启归零） | P0-1 | ✅ 三字段（`last_ecc`/`rising_count`/`health`）并入 Wear 条目（8B→12B）⇒ `FTL_MEDIA_LAYOUT_VERSION` 2→3 |

### 波 4 — 算法深度

| 序 | 项 |
|---|---|
| 9 | GC Bitmap（Layer 2） |
| 10 | Prefetch 顺序读 |
| 11 | 多流 host stream hint |

### 波 5 — 文档债（随行订正）— ✅ **本轮已基本清空（2026-10-13）**

| 项 | 状态 |
|---|---|
| 映射粒度 | ✅ 已迁移（波 2 序 2） |
| Checkpoint 注释（全量 vs 增量） | ✅ 已订正 |
| L2P 表尺寸（4× 超额） | ✅ 已核：非缺陷 |
| `main.c` 内存注释（131.5/355.5/147.7KB） | ✅ 已订正为 **270.4KB / ~291.3KB**（P2-5 后表收进 `g_ftl`；见 §8.4） |
| 旧布局标识符（`L2P_REGIONS_TOTAL` 等） | ✅ 已清理 |
| `ftl_boot_scan_write_pointer` 死代码 | ✅ **已删（v0.65）**（旧单 SB 路径，零调用者；`buf[16]` 传 2048B 拷贝的栈溢出随之消失） |

### 不改（边界）

`CONFIG_FC_RAID`=0（RR Stage 3 不实现）、不引 DRAM 常驻 L2P、**P2-4** band seq（消费者不存在）、
**P2-1** BST 裁剪（待触发）。

### 跨层穿插（按依赖）

op 18→17（NAND 层）、~~**FTL UT 接线**（**波 2 之后**）~~ ✅ **已接线（`9ff5a77`，8 套件 53 例）**、
F8/F9、v0.58 上板、HAL_RCCEx、FMC HiZ。

### 一句话顺序

**（波 0~3 已全部落地：计数持久化 / 写模型 / P1 算法 / CW 迁移 / 有效性 ③ / 版本标记 / CRC 下沉 /
表所有权 / 多实例 / OOB 出厂坏块 / 路由裁决 / P0-3 压缩 / rr 持久化 / 页打包写缓冲 / GC 节拍 /
元数据块 plane 掩码 / 诊断 CLI）→ 上板验证（v0.69/v0.70 已过）→ **波 4 算法深度** → 文档债**；
边界不动，跨层按依赖插入；**每波上板**。

### 下一步（当前悬空项，按紧急度）

1. ~~**上板验证波 0**~~ ⇒ ✅ **已推进至 v0.70**：首启自动格式化（v0.65 把 `FTL_MEDIA_LAYOUT_VERSION`
   提到 3）、`wr/rd/tr/fl/info/map/crash/bbt` 冒烟、断电重启数据仍在均过。
   ⚠ **唯一悬空项 = v0.71 的 HardFault**（记录见 `ftl_revision_history.md v0.71`）：
   **不知如何复现** ⇒ 下一步 = 按 `diag_crash_cli_board_verify_v71.md` 的**正确顺序**
   （**先 `crash` 看详细、再 `crash clear`**，保留当场 `=== CPU FAULT ===` dump）抓一次现场。
   ⇒ 逐项清单仍见 `docs/design/ftl_wave0_board_verify.md`。
2. ~~**P0-3 L2P 日志块压缩**~~ ✅ **已落地（v0.65）**。
3. ~~波 2 剩余四项~~ ✅ **已落地（v0.65）**；~~波 3 两项~~ ✅ **已落地（v0.65）**；
   ~~波 5 死代码~~ ✅ **已删（v0.65）**。
4. ~~**FTL UT 接线**（用户自建；夹具依赖表所有权形态，v0.65 已定形 ⇒ 现在可以动）。~~
   ✅ **已接线（`9ff5a77`）**：`tests/unit/test_ftl.c` = 8 套件 53 例（v0.74 增至 55 例），板端 `test` 跑。
5. **波 4（算法深度，未排）**：GC Bitmap Layer 2（正确性已闭合，只是断点续搬）/
   Prefetch 顺序读 / 多流 host stream hint。~~**P2-6（per-plane 写指针）**~~ ⇒ **已定案（v0.66）**：
   器件/标准约束（两 plane 必须同页地址）⇒ **不实现**；唯一解 §七 方案 A 页打包写缓冲已落地。

---

## 附：关键文件索引

| 主题 | 文件 |
|---|---|
| L2P 映射/cache | `include/ftl/l2p_cache.h`、`src/ftl/l2p_cache.c` |
| GC | `include/ftl/gc.h`、`src/ftl/gc.c` |
| 磨损均衡 | `include/ftl/wear_leveling.h`、`src/ftl/wear_leveling.c` |
| 分配器/池 | `include/ftl/allocator.h`、`src/ftl/allocator.c` |
| Journal/Checkpoint | `src/ftl/journal.c`、`src/ftl/checkpoint.c` |
| 掉电恢复 | `src/ftl/power_failure_recovery.c` |
| BST | `include/ftl/bst.h` |
| 调度 | `src/ftl/ftl.c`、`src/ftl/pending.c`、`src/ftl/completion_table.c` |
| 设计文档 | `docs/design/ftl_design.md`、`ftl_revision_history.md`、`ftl_ut_munit.md` |
| 对照资料 | `docs/reference/README.md`、`docs/reference/papers/`、`docs/reference/pages/` |

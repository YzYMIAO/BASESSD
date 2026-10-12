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

1. **有效性缺口**：一批机制**接上了但没有真正生效**——最严重的是**擦除计数跨重启归零**（WL 只在单次上电内有效）；
   其次是 **Emergency 池是死代码**（P1-3 已定性：那是文档误把「水位」写成「池」，非实现缺陷）、
   **Metadata Directory 容量 128 < 2048 region（被截断）**。
2. **算法缺口**：~~GC victim 单维贪心~~（P1-1 已加权）；~~CLOCK 无 thrashing 降级~~（P1-4 已落地）；
   GC 触发只落了 3/6 条。
3. **一致性债**：映射粒度（文档 CW 级 vs 代码页级）、Checkpoint 注释（全量 vs 增量）、L2P 表尺寸（相对物理页疑似 4× 超额）——
   文档与代码有偏差，需订正。

**优先级建议**：先补 P0（有效性），再补 P1（算法），P2/P3 视内存压力与学习目标排期。

---

## 一、本仓 FTL 现状快照（源码为准）

| 维度 | 现状 | 证据 |
|---|---|---|
| 映射粒度 | **页级**：1 次 2048B 写更新 1 条 L2P entry；`io.lba`/`io.lba+1` 对应 plane0/plane1 页 | `ftl.c:718-721,169-171`；`ftl.h` 接口注释 |
| L2P 存储 | NAND 元数据区，**DRAM-less**；全表 2048 region × 512 entry × 4B = **4MB** | `l2p_cache.h:29-51` |
| L2P 位置解析 | Metadata Directory（128 entry）动态分配 + 固定位置回退 | `metadata.c:35-355` |
| L2P cache | SRAM **64 region** CLOCK（近似 LRU），write-back + journal | `l2p_cache.c:78-132,239-264` |
| **thrashing 检测** | ~~**无**（只有 hit/miss 计数）~~ ⇒ P1-4 已落地：窗口化 miss 率 + 滞回 + 停主动 flush | `l2p_cache.{h,c}`、`ftl.c` |
| GC victim | **单维贪心：有效页最少**（无权重） | `gc.h:10,58`；`gc.c:198-241` |
| GC 触发 | **3 条**：水位 32 / 写入 1024 / 定时 500ms（T1/T2/T3） | `gc.c:364-385` |
| 静态 WL | **已实现**（三条件触发 + 冷块 victim + read_count 权重） | `wear_leveling.c:14-306`；`ftl.c:392` |
| 动态 WL | allocator 每 plane 选擦除最少块 | `allocator.c:119-148` |
| **擦除计数持久化** | **无**——`ftl_init` memset 全 BST，无 load/save ⇒ **每次上电归零** | `ftl.c:465`；`METADATA_TYPE_ERASE_COUNT` 零引用 |
| Free Pool | **单一 free 池 + OP 池**；`pre_cond_count ≡ free_count`；**Emergency 死代码** ⇒ P1-3 定案为「一个池 + 三级水位」，冗余计数与状态位已删 | `allocator.c`；`bst.h` |
| 温度分区 | Hot/Cold 两个 Open SB | `allocator.c:43-44,348` |
| Super Block | 双 plane 配对，**`block1 = block0`（同号）** | 变更史 v0.48 备注 |
| Journal | 2 块环形 log，16B/entry，128/page；truncate 简化（不扫 sequence） | `journal.c:129-142,340-353` |
| Checkpoint | **增量**（只 flush 脏 region）——但头注释写「全量」 | `checkpoint.c:63-93` vs `checkpoint.h:52` |
| 掉电恢复 | fresh/clean/power-failure 三类；closed SB 重建 = **全 NAND 扫描 + L2P 反查** | `power_failure_recovery.c:30-341,563-634` |
| 调度/QoS | IDLE/NORMAL/BUSY，按 `pending+completion` 占用判空闲度 | `ftl.c:510-558` |
| 流水线 | pending 深 32 + completion 深 32，异步提交 + 完成泵 | `pending.h:20`；`completion_table.h:38` |
| FTL↔FC | 用户数据走**完整 CRC+加扰+ECC(BCH)**；元数据只保 ECC | `ftl.c:632-637,741-746` |
| BST | 条目 24B × 4096 = 96KB + 独立位图表 64KB = **160KB 全常驻 SRAM**（v0.55 位图已搬出条目） | `bst.h` |
| Read Recovery | Stage 1/2 有；**Stage 3（XOR/RAID）未实现** | `read_recovery.c:263` |

---

## 二、与开源设计的差异（逐维度对照）

| 维度 | 本仓 | SPDK FTL | Jasmine | dhara/SPIFTL | 差异性质 |
|---|---|---|---|---|---|
| L2P 存储 | NAND（DRAM-less） | **DRAM/缓存设备**（默认常驻 2GiB MRU，溢出换页） | DRAM 全表 | 无表/块级 | **路线不同**（本仓对标 UFS/eMMC，SPDK 对标有 DRAM 的企业盘） |
| 映射粒度 | 页（2048B） | 4K 块 | 页 | 块 | 粒度相近 |
| GC victim | 有效页最少（单维） | **validity ratio**（band） | Greedy 等 | 整块 | 本仓**少一维以上** |
| 数据年龄/序号 | `last_pe_ts`（P1-1 已消费；~~`gc_generation`~~ v0.56 删） | **seq_id**（band 级，恢复用） | — | — | 本仓**未成体系** |
| 恢复机制 | Journal + Checkpoint | **P2L 重放**（无 journal） | 重建 | 扫描 | 路线不同，本仓更接近消费级 |
| Free Pool | 单池 + OP + 三级**水位** | nvcache chunk + band 状态机 | — | — | 本仓**三级水位已生效**（v0.52 定案） |
| 多流/温度 | Hot/Cold 双 Open SB | band 顺序写 | — | — | 本仓对齐消费级，**弱于企业级多流** |
| 学习型压缩 | 无 | 无 | 无 | 无 | **LeaFTL 是前沿方向**，本仓未涉及 |
| 有效性基建（计数持久化） | **缺**（归零） | 有（shm 脏跟踪） | 有 | 有 | 本仓**落后** |

**一句话**：本仓在「DRAM-less 路线 + 分层结构」上不落后；**落后的是 GC/WL 的算法深度与计数持久化**。

---

## 三、差异分类

**A. 已对齐/领先（不动）**
- 纯页映射、DRAM-less L2P + SRAM region cache、Journal+Checkpoint 双保险、Hot/Cold 温度分区、
  三模式后台调度、FTL 走 FC 完整保护路径（CRC/ECC/加扰）。

**B. 有实现但弱（补强）**
- GC victim 单维贪心；GC 触发仅 3/6；CLOCK 无降级；Super Block 同号连坐；Journal 2 块寿命瓶颈。

**C. 设计有、代码无（缺口）**
- ~~Emergency Free Pool~~（P1-3 文档误述）；~~GC 多维加权~~（P1-1）；~~thrashing 检测~~（P1-4）；GC Bitmap（Layer 2）；Prefetch；
  Read Recovery Stage 3（**且 RAID 开关必须保持 0，见 §六**）。

**D. 有效性缺口（最该先修）**
- **擦除计数/read_count/rr 不持久化** ⇒ WL 与 read-disturb 预测**每次上电归零**（→ P0-1 ✅）；
- ~~Metadata Directory 容量 128 < 2048 region~~ ⇒ **该描述已过时**（常量早已 = `L2P_REGIONS_PER_INSTANCE`）。
  实际缺陷是**元数据就地覆盖**（NAND 只能 1→0，二次编程 = 静默损坏）+ Anchor 从未落盘 +
  Checkpoint 二次写即坏（→ P0-2 ✅）；
- **元数据日志块抗碎片**：`metadata_dir_reclaim` 只回收活计数为 0 的块 ⇒ 挂着冷 region 的块永久占块（→ P0-3）。

**E. 一致性债（订正文档或代码）**
- 映射粒度（§5.1 写 CW 级 512B vs 代码页级 2048B）；
- Checkpoint 注释（全量 vs 增量）；
- L2P 表 1,048,576 entry 相对 262,144 物理页**疑似 4× 超额**（若按页级映射）；
- `ftl_boot_scan_write_pointer` 死代码且 `buf[16]` 传给按 2048B 拷贝的 raw read（风险，已自承）。

---

## 四、优化方向（按优先级）

### P0 — 有效性（不做则「机制在跑但没生效」）

**P0-1 计数持久化（最高优先）** — ✅ **已落地**（2026-10-12）
- **问题**：`ftl.c` 每次 `ftl_init` memset 全 BST ⇒ `erase_count`/`read_count`/`program_count` 归零。
  静态 WL 的「max-min ≥ 5000」、动态 WL 的「选擦除最少块」、read-disturb 预测**跨重启全部失效**。
- **对照**：SPDK FTL 用 shm 跟踪脏页；Jasmine/企业级持久化 block metadata。
- **落地方式**：新增 `include/ftl/wear.h` + `src/ftl/wear.c` —— Wear 表落在已预留的固定块
  `WEAR_BLOCK`（26），介质上是**块内页式追加日志**（sequence + 每页 CRC，与 Anchor/Journal
  同一套日志惯例；绝对值 + 最新胜），而非"每次整表重写"——块 26 只有 64 页、全量快照 25 页，
  整表重写会让它 ~2.6 次落盘就被擦一次（磨损远超数据块）。
  开机 `wear_init` 逐页回放回填 BST（非致命：无有效页 = 冷启动）；运行期 5 个变更点
  `wear_mark_dirty` 置脏位，`ftl_service_idle`/`normal` 里 `wear_flush` 按节拍
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
  ③ 复位后数据仍在（`[META] anchor_seq` 非 0、`dyn` 非 0）——**当前必然失败**。
  观测口 = 开机 `[META] anchor_seq=… dyn=… cur_blk=… live_blks=…`。
- ⚠ **介质格式变更** ⇒ 上板前**必须先擦除芯片**。

**P0-3 L2P 日志块压缩（抗碎片）** — 📋 **待做**（P0-2 的阶段 3，**未实现**）
- **问题**：`metadata_dir_reclaim`（`metadata.c:573-596`）只回收 `l2p_live[g] == 0` 的块：

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
- ⚠ 不新增介质格式变更（复用同一套日志块 + Directory 语义）。

### P1 — 算法（对照开源可显著提升）

**P1-1 GC victim 多维加权评分**（设计 §4.3 已写，未实现）
- 公式：`score = 0.50·valid_ratio + 0.20·(1-erase_ratio) + 0.20·temp_factor + 0.10·(1-age_ratio)`。
- 对照 SPDK FTL 的 validity ratio（单维）→ 本仓加上**磨损/温度/时间**三维，已超 SPDK 的 GC 选块维度。
- **判据**：同一 victim 集下，加权选出的块擦除次数分布更均匀、COLD 块被搬次数下降。

**P1-2 GC 触发补齐 T4/T5** — ✅ **已落地**（T6 经论证**废弃**）
- T4 强制（`free_count ≤ FREE_POOL_EMERGENCY 8`）、T5 磨损比（`max/min > 10`，加 `max ≥ 64` 地板 + `min > 0`）。
  新增 `gc_check_triggers()` 返回位掩码（`GC_TRIGGER_*`），`gc_check_trigger()` 退化为 `!= 0` 薄封装；
  观测口 `[GC] trig=0x%02x pre=… free=… writes=…`。
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

**P2-1 BST 产品化裁剪**（设计 §19 档三，本期不做，触发条件「SRAM 紧张 + 字段有消费方」）
- ⚠ **数字与前提待重算（v0.56 后）**：原「160KB → ~100KB（核心 40KB + valid_map 64KB）」基于 24B 条目；
  现条目 **20B**（80KB）+ 位图 64KB = **144KB**。且 §19 档三的**扩展字段表在 v0.56 后已空**
  （两个候选冷字段 `program_count`/`gc_generation` 已删、`last_pe_ts` 有消费者须常驻）
  ⇒ 剩余唯一杠杆是 **`valid_map`（64KB）的驻留策略**。
- **先决**：P0-1 的持久化子系统落地后，扩展字段才有意义。

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

**P2-3 OOB/spare 通路 + 出厂坏块检测走 OOB**（主流对照，仅登记，暂不排期）
- **主流做法**（`storage_controller_design_essentials.md:420-424,146`，自述无外部引文）：出厂坏块在
  **初始化时读每块首页的 OOB/spare 坏块标记**建 BBT；OOB/spare 区放 **ECC / 元数据 / 坏块标记**。
- **本仓偏离**：`bbt.c:145-153` 明说「FC 层没有独立 OOB 读取接口」⇒ 出厂坏块检测**靠「读失败 / 全 FF」
  替代**；器件 **64B/页 spare 区全空置**（`nand_config.h:25`）。
- **根因**：FC 命令通路的 `cw` 只有 2 位 ⇒ 列上限 1584，**够不到 spare 区**（列 2048，`onfi_1_0.h:457`）。
- **改动范围**：给 FC 加 spare 区读写通路（显式列地址），出厂坏块检测改读 OOB 标记。
- **为何登记不排期**：有主流依据 + 器件能力，改动可界定，但与 BST 主线（P2-1）不同源，单独排。

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

### P3 — 学习/前沿（非必要）

**P3-1 Prefetch 顺序读**（设计 §6.4）：检测顺序访问 → 预加载后续 region。
**P3-2 多流（host stream hint）**：Hot/Cold 双温区 → 多级温区，对照 SPDK/企业级 multi-stream。
**P3-3 学习型 L2P 压缩**：LeaFTL 方向，DRAM-less 前沿，学习价值高、工程量大。

### 验证基建（横切，独立排期）

**V-1 L1 Mock NAND + 掉电注入**（`docs/design/ftl_ut_munit.md` §8 已定方向）
- 关键发现：FTL 的 NAND 访问**唯一收敛到 `ftl_internal_sync_wait` → `fc_submit`** ⇒ 只需把 `fc_submit`
  换成 RAM 桩，write/GC/journal/掉电恢复全部自动可测。
- 掉电注入 = 桩记录「已确认写」点 + 任意处截断 → 重启走 `ftl_boot_recovery` → 断言 L2P 一致。
- **这是 P0/P1 各项「判据」能被自动验证的前提**——强烈建议与 P0 并行。

---

## 五、分阶段实施计划

| 阶段 | 内容 | 依赖 | 判据 | 上板 |
|---|---|---|---|---|
| **S1 有效性** | P0-1 计数持久化 + P0-2 元数据写模型统一 + E 一致性债订正 | 无 | 重启后计数保持；同一 region 二次 flush 后读回正确；region>128 走动态块 | 是 |
| **S1.5 抗碎片** | **P0-3 L2P 日志块压缩** | **S1 上板验证通过** | 冷热混块下 victim 被回收；长时压测 `live_blks` 不发散 | 是 |
| **S2 算法** | P1-1 GC 加权 + P1-2 触发补齐 + P1-3 三级水位 | S1.5（T5 需计数） | victim 分布更均匀；水位分级生效 | 是 |
| **S3 稳态** | ~~P1-4 thrashing~~（已落地）+ ~~P1-5 SB 独立块号~~（Policy B 早已实现）+ ~~P1-6 Journal~~（已落地）| S2 | 随机写无活锁；坏块不连坐；journal 水位健康 | 是 |
| **S4 产品化/前沿** | P2-1 BST 裁剪（待触发）；P3-1/2/3（**P2-2 已结案、P2-3/P2-4 仅登记**） | S3 | 按需 | 是 |
| **V 基建（并行）** | L1 Mock NAND + 掉电注入 | 无 | 掉电一致性不变量全绿 | 是 |

> 每阶段沿用本仓纪律：`make format && make format-check` → `make clean && make tests`（0 warning）→
> **上板**跑 `test` 套件 + UART CLI `wr/rd/tr/fl/info`。

---

## 六、不做 / 边界

- **`CONFIG_FC_RAID` 保持 0**（用户 2026-10-10 明确）⇒ **Read Recovery Stage 3（XOR）不实现**，
  现状 `read_recovery.c:263` 的「不实现」是**符合边界的**，不是待办。P0/P1 不碰它。
- 不动 `src/fc/**` 的对外接口形态（§26.3 判据：FC 只留硬件 FCC 寄存器表的对应项）。
- 不新增单元测试源文件（项目规则：测试源用户自建）——V-1 只出**方案**，实现由用户定。
- 不改 `L2P_REGIONS_TOTAL`/容量语义（512MiB 不变）；不改介质格式（除 S1 若确需新增元数据表，
  须按 v24 教训「介质格式变更须重新格式化」登记）。
- 不为「对齐开源」而引入 DRAM 常驻 L2P（违背本仓 DRAM-less 定位）。

---

## 七、风险与诚实登记

1. **映射粒度存疑**（§三 E）：若代码确为页级，则 L2P 表 1,048,576 entry 相对 262,144 物理页**超额 4×**
   （浪费 ~3MB 表空间 + 24 个元数据块）。**实施 S1 前必须先核对**——这可能是比「优化」更该先修的结构问题。
2. **计数持久化**（P0-1）：**不新增**介质格式变更——Wear 块 26 早在分区表里预留且被
   `ftl_is_metadata_block` 排除，本次只是第一次往里写内容。块号/分区一个没动
   （v24 教训：只有改块号/布局才须重新格式化；本条只改了 `FTL_WEAR_TABLE_BYTES` 的
   逻辑表字节常量，不影响介质寻址）。
3. ~~Emergency 池真三级化~~（P1-3 已判定为文档误述，无此改动）。
4. **P1-1 加权评分**引入浮点/定点的取舍：嵌入式无 FPU 便利 ⇒ 用整数加权（本仓惯例），避免 `%f`（newlib-nano）。
5. 本文档的「现状」以**源码为准**；若与 `ftl_design.md` 冲突，**以代码为准**并在 S1 一并订正文档。
6. **上板前置（P0-2 引入）**：**必须先擦除芯片**。本次改的是介质格式（块布局 + Anchor 分页 +
   Journal 挪号），旧镜像写下的内容按新布局解读会错位。
7. **未做（本轮登记，见 `ftl_revision_history.md` v0.50「登记不做」）**：
   - **阶段 3 / P0-3**（上条）——**长时压测前必须补**；
   - **多实例**：`checkpoint.c` / `bbt.c` / `wear.c` 的 file-scope static 游标/序号在多 LUN 下
     所有实例共享一份（`metadata.c` 的 Directory 是 per-instance，不受影响）；
   - **SPDK FTL 式 P2L 重建**（每 band 尾部存 P2L、开机顺序扫描重建 L2P）——替换整层持久化机制的
     大改，登记为后续方向；本仓 `ftl_rebuild_closed_sb_list` 已有雏形；
   - **`ftl_is_metadata_block` 的 per-plane 粒度**（按 per-plane 块号比较 ⇒ 会连另一个 plane 的
     同号块一起排除；偏保守、安全，不修）；
   - ~~**`program_count` 无消费方**~~ / ~~**`last_pe_ts` / `gc_generation` 死字段**~~ —— ✅ v0.56 已删
     `program_count`/`bad_reason`/`gc_generation`；`last_pe_ts` 由 P1-1 消费（原判据已过期）；
     **`rr` 仍不持久化**。
8. **文档一致性债（`cw_level_mapping_design.md` §十二 项 6 已登记，本轮未改）**：
   - `ftl_design.md` / `ftl_api_usage.md` 仍散落**旧布局标识符**（`L2P_REGIONS_TOTAL` /
     `L2P_REGIONS_PER_LUN` / `L2P_BLOCKS_PER_LUN`、L2P 32 块、Journal 40/41 块、
     Directory 128 条 / 904B）——v0.50 已订正**与写模型直接矛盾**的段落（§3.3 / §5.3 / §5.5 /
     §14 / §16），其余旧标识符**待 S1.5 一并清理**；
   - `src/main.c` 两条内存注释仍是旧值：`g_ftl` 标 "131.5KB"（实测 `sizeof(ftl_context_t)`
     = **147.7 KB**）；AXI_RAM 标 "≈355.5KB（剩 11.5KB）"（实测 **332.3 / 384 KB，余 51.7 KB**）。

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

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

本仓 FTL 的**结构**（纯页映射 + DRAM-less L2P + CLOCK cache + Journal/Checkpoint + 三级池 + 静态 WL + 三模式调度）
与开源/企业级设计的**大方向是对齐的**，甚至比 dhara/SPIFTL 这类极简 FTL 完整得多。差距不在「有没有这个概念」，
而在三处：

1. **有效性缺口**：一批机制**接上了但没有真正生效**——最严重的是**擦除计数跨重启归零**（WL 只在单次上电内有效）；
   其次是 **Emergency 池是死代码**、**Metadata Directory 容量 128 < 2048 region（被截断）**。
2. **算法缺口**：GC victim 仍是**单维贪心**（设计写了多维加权，未实现）；CLOCK **无 thrashing 降级**；
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
| **thrashing 检测** | **无**（只有 hit/miss 计数） | `l2p_cache.h:116-117`（无降级逻辑） |
| GC victim | **单维贪心：有效页最少**（无权重） | `gc.h:10,58`；`gc.c:198-241` |
| GC 触发 | **3 条**：水位 32 / 写入 1024 / 定时 500ms（T1/T2/T3） | `gc.c:364-385` |
| 静态 WL | **已实现**（三条件触发 + 冷块 victim + read_count 权重） | `wear_leveling.c:14-306`；`ftl.c:392` |
| 动态 WL | allocator 每 plane 选擦除最少块 | `allocator.c:119-148` |
| **擦除计数持久化** | **无**——`ftl_init` memset 全 BST，无 load/save ⇒ **每次上电归零** | `ftl.c:465`；`METADATA_TYPE_ERASE_COUNT` 零引用 |
| Free Pool | **单一 free 池 + OP 池**；`pre_cond_count ≡ free_count`；**Emergency 死代码** | `allocator.c:12-14,31-32`；`bst.h:69` 无置位点 |
| 温度分区 | Hot/Cold 两个 Open SB | `allocator.c:43-44,348` |
| Super Block | 双 plane 配对，**`block1 = block0`（同号）** | 变更史 v0.48 备注 |
| Journal | 2 块环形 log，16B/entry，128/page；truncate 简化（不扫 sequence） | `journal.c:129-142,340-353` |
| Checkpoint | **增量**（只 flush 脏 region）——但头注释写「全量」 | `checkpoint.c:63-93` vs `checkpoint.h:52` |
| 掉电恢复 | fresh/clean/power-failure 三类；closed SB 重建 = **全 NAND 扫描 + L2P 反查** | `power_failure_recovery.c:30-341,563-634` |
| 调度/QoS | IDLE/NORMAL/BUSY，按 `pending+completion` 占用判空闲度 | `ftl.c:510-558` |
| 流水线 | pending 深 32 + completion 深 32，异步提交 + 完成泵 | `pending.h:20`；`completion_table.h:38` |
| FTL↔FC | 用户数据走**完整 CRC+加扰+ECC(BCH)**；元数据只保 ECC | `ftl.c:632-637,741-746` |
| BST | 40B × 4096 = **160KB 全常驻 SRAM**，`valid_map` per-plane 位图 | `bst.h:123-147` |
| Read Recovery | Stage 1/2 有；**Stage 3（XOR/RAID）未实现** | `read_recovery.c:263` |

---

## 二、与开源设计的差异（逐维度对照）

| 维度 | 本仓 | SPDK FTL | Jasmine | dhara/SPIFTL | 差异性质 |
|---|---|---|---|---|---|
| L2P 存储 | NAND（DRAM-less） | **DRAM/缓存设备**（默认常驻 2GiB MRU，溢出换页） | DRAM 全表 | 无表/块级 | **路线不同**（本仓对标 UFS/eMMC，SPDK 对标有 DRAM 的企业盘） |
| 映射粒度 | 页（2048B） | 4K 块 | 页 | 块 | 粒度相近 |
| GC victim | 有效页最少（单维） | **validity ratio**（band） | Greedy 等 | 整块 | 本仓**少一维以上** |
| 数据年龄/序号 | `gc_generation`/`last_pe_ts`（写了少用） | **seq_id**（band 级，恢复用） | — | — | 本仓**未成体系** |
| 恢复机制 | Journal + Checkpoint | **P2L 重放**（无 journal） | 重建 | 扫描 | 路线不同，本仓更接近消费级 |
| Free Pool | 单池 + OP | nvcache chunk + band 状态机 | — | — | 本仓**三级池名存实亡** |
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
- Emergency Free Pool（死代码）；GC 多维加权评分；thrashing 检测；GC Bitmap（Layer 2）；Prefetch；
  Read Recovery Stage 3（**且 RAID 开关必须保持 0，见 §六**）。

**D. 有效性缺口（最该先修）**
- **擦除计数/read_count/rr 不持久化** ⇒ WL 与 read-disturb 预测**每次上电归零**；
- **Metadata Directory 容量 128 < 2048 region** ⇒ 超出部分退化为固定位置，动态分配失效。

**E. 一致性债（订正文档或代码）**
- 映射粒度（§5.1 写 CW 级 512B vs 代码页级 2048B）；
- Checkpoint 注释（全量 vs 增量）；
- L2P 表 1,048,576 entry 相对 262,144 物理页**疑似 4× 超额**（若按页级映射）；
- `ftl_boot_scan_write_pointer` 死代码且 `buf[16]` 传给按 2048B 拷贝的 raw read（风险，已自承）。

---

## 四、优化方向（按优先级）

### P0 — 有效性（不做则「机制在跑但没生效」）

**P0-1 计数持久化（最高优先）**
- **问题**：`ftl.c:465` 每次 `ftl_init` memset 全 BST ⇒ `erase_count`/`read_count`/`rr` 归零。
  静态 WL 的「max-min ≥ 5000」、动态 WL 的「选擦除最少块」、read-disturb 预测**跨重启全部失效**。
- **对照**：SPDK FTL 用 shm 跟踪脏页；Jasmine/企业级持久化 block metadata。
- **方向**：复活 `METADATA_TYPE_ERASE_COUNT`（已定义零引用）——周期落盘擦除计数表 + 启动回填；
  或按 `ftl_design.md` §19 档三，把扩展字段（含计数）落 NAND 扩展表。
- **判据**：断电重启后 `erase_count` 非零且与掉电前一致（需掉电注入或人为复位验证）。

**P0-2 Metadata Directory 扩容**
- **问题**：`METADATA_DIR_ENTRIES=128` 但 region 有 2048；`metadata_dir_init` 循环被截断（`metadata.c:52`），
  超出部分退化为 `l2p_region_pba` 固定位置，**动态分配名存实亡**。
- **方向**：扩容（SRAM 允许）或改**哈希/两级目录**；明确「固定回退」是兜底还是主路径。
- **判据**：对 region_id > 128 的 LBA 做写→flush→读回，验证走动态分配。

### P1 — 算法（对照开源可显著提升）

**P1-1 GC victim 多维加权评分**（设计 §4.3 已写，未实现）
- 公式：`score = 0.50·valid_ratio + 0.20·(1-erase_ratio) + 0.20·temp_factor + 0.10·(1-age_ratio)`。
- 对照 SPDK FTL 的 validity ratio（单维）→ 本仓加上**磨损/温度/时间**三维，已超 SPDK 的 GC 选块维度。
- **判据**：同一 victim 集下，加权选出的块擦除次数分布更均匀、COLD 块被搬次数下降。

**P1-2 GC 触发补齐 T4/T5/T6**
- T4 强制（Emergency）、T5 磨损比（max/min > 10）、T6 碎片率（平均有效页 > 70%）。
- 依赖 P0-1（T5 要持久化擦除计数才有意义）。

**P1-3 Pre-conditioned Pool 真三级化**
- 现状 `pre_cond_count ≡ free_count`、Emergency 死代码 ⇒ 设计 §3.7 的「BG GC 预擦除池」未生效。
- 对照企业级 background GC 池：预擦除块与普通 free 块分开计数、分开水位。
- **判据**：`pre_cond_count` 与 `free_count` 解耦，前台 IO 不再等 GC。

**P1-4 L2P cache thrashing 检测 + 降级**（设计 §6.5 已写）
- 全盘随机写时命中率可跌至 1.6~6.3%；`miss_rate > 90%` 时切 write-through + 停 background flush。
- 对照 SPDK FTL 的换页策略（它靠 DRAM 缓冲规避，本仓只能靠降级）。
- **判据**：随机写压测下不出现「evict→flush→load」活锁。

**P1-5 Super Block 独立块号**
- 现状 `block1 = block0`（同号）⇒ plane0 坏块**连坐**浪费 plane1 同号好块。
- 真实主控允许 SB 内每 plane 用不同物理块号。
- **判据**：人为标坏 plane0 某块，验证 plane1 同号好块仍可用。

**P1-6 Journal 寿命瓶颈**
- 现状 2 块 ≈ 800GB 寿命（设计 §14.1 自承瓶颈）。
- 方向：增 `JOURNAL_BLOCKS`（2→4/8，最简单）+ region 更新合并（同一 region 多次写合并为一次 flush）。
- **判据**：寿命估算翻倍；写压测下 journal 水位不长期贴顶。

### P2 — 内存/产品化

**P2-1 BST 产品化裁剪**（设计 §19 档三，本期不做，触发条件「SRAM 紧张 + 字段有消费方」）
- 160KB → ~100KB（核心 40KB + valid_map 64KB，扩展字段落 NAND）。
- **先决**：P0-1 的持久化子系统落地后，扩展字段才有意义。

**P2-2 SB 索引粒度**（变更史 v0.48 备注，已评估否决）
- 按 SB 索引可再省 96KB，但丢 per-plane 分辨率、与 BBT 粒度不一致。**保持不做**，仅登记。

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
| **S1 有效性** | P0-1 计数持久化 + P0-2 Directory 扩容 + E 一致性债订正 | 无 | 重启后计数保持；region>128 走动态分配 | 是 |
| **S2 算法** | P1-1 GC 加权 + P1-2 触发补齐 + P1-3 三级池 | S1（T5 需计数） | victim 分布更均匀；池分级生效 | 是 |
| **S3 稳态** | P1-4 thrashing + P1-5 SB 独立块号 + P1-6 Journal | S2 | 随机写无活锁；坏块不连坐；journal 水位健康 | 是 |
| **S4 产品化/前沿** | P2-1 BST 裁剪；P3-1/2/3 | S3 | 按需 | 是 |
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
2. **计数持久化会改介质格式**（新增擦除计数表）⇒ 需重新格式化 + 版本号，按 v24 教训处理。
3. **Emergency 池真三级化**会改 allocator 水位语义 ⇒ 波及 GC 触发与写路径，需回归。
4. **P1-1 加权评分**引入浮点/定点的取舍：嵌入式无 FPU 便利 ⇒ 用整数加权（本仓惯例），避免 `%f`（newlib-nano）。
5. 本文档的「现状」以**源码为准**；若与 `ftl_design.md` 冲突，**以代码为准**并在 S1 一并订正文档。

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

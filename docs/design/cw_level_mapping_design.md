# CW 级映射与布局设计（IU = CW = 512B）

> **文档性质**：布局事实 + 设计决策。**不含实现**。
> **判据**：`docs/design/fc_nand_design.md` §26 分层（HIL / FTL / FCC(硬件) / DRAM + 队列 + 门铃 + 完成）。
> **决策标尺**：**真实存储主控（SSD / eMMC / UFS）怎么做**——每条决策都给出「主流做法 → 本仓建议」。
> **现状来源**：以源码为准，**引用符号名而非行号**（行号会漂，符号名不会）。

---

## 〇、怎么读这份文档

本文分三条线，**按需查阅，不必顺序读**：

| 部分 | 内容 | 什么时候读 |
|---|---|---|
| **第一部分 · 布局事实** | 几何、容量、块号、多维度路由——**可查表** | 想知道"块 X 是什么""多 LUN 时 LBA 去哪了" |
| **第二部分 · 设计讨论** | 为什么 IU = CW、为什么打包、为什么 per-CW 有效位 | 想改设计、想知道取舍 |
| **第三部分 · 状态与索引** | 什么已落地、什么还是设计、风险、文件索引 | 想知道"这条现在跑得到吗" |

**两条口径约定**（全文严格遵守，混了就会算错）：

1. **每实例 vs 全局**。本 FTL 是**分片模型**——每个 `(ch,ce,lun)` 是一个**完整独立的 FTL 实例**。
   表尺寸与块号都是**每实例**量（不乘维度数）；只有"设备级"量（`FTL_IU_TOTAL` /
   `FTL_USER_CAPACITY_BYTES`）才乘 `FTL_DIM_TOTAL`。**见 §1.3、§三**。
2. **维度默认值**。当前 `nand_config.h` 是 `CH_NUM = CE_NUM = LUN_NUM = 1`（`FTL_DIM_TOTAL = 1`），
   所以正文里不带维度标注的数字都是 **dim=1 的展开值**。多维度情形见 **§三**。

---

# 第一部分 · 布局事实

## 一、几何与容量

### 1.1 三层粒度

| 层 | 单位 | 本仓 | 谁决定 |
|---|---|---|---|
| 编程单位 | page | 2048B（主区） | **器件决定，不可改** |
| 映射单位 (IU) | CW | **512B（选定）** | 设计选择，见 §五 |
| 主机 IO | 扇区 | **512B（选定）** | 设计选择 |

### 1.2 几何推导

```
1 page  = 4 CW            = 4 个 LBA
1 block = 64 page × 4 CW  = 256 CW = 256 LBA = 128 KiB
每实例   = 4096 block × 256 = 1,048,576 槽位 = 512 MiB
```

**物理槽位数 = L2P entry 数，精确 1:1**（每实例都是 1,048,576）。这是 CW 级与页级的本质差别——
页级时表会超额 4×（表 1,048,576 vs 物理页 262,144）。选 CW 后
`FTL_LBA_MAX`（`ftl.h`）、`L2P_REGIONS_PER_INSTANCE`（`ftl_config.h`）、`lba_t = 512B`
（`ftl_type.h`）全部自洽，**不用改常量**。

⚠ **扇区写落页**：器件编程单位仍是 page（2048B），**不能单独编 1 个 CW**。512B 写必须变成
2048B 页编程 ⇒ 见 §七。

### 1.3 每实例 vs 全局（**最容易算错的一处**）

| 量 | 口径 | 公式 | dim=1 |
|---|---|---|---|
| `FTL_DIM_TOTAL` | 设备 | `CH_NUM × CE_NUM × LUN_NUM` | 1 |
| `FTL_BLOCK_TOTAL` | **每实例** | `BLOCKS_PER_PLANE × PLANE_NUM` | 4096 |
| `FTL_CW_TOTAL` | **每实例** | `FTL_BLOCK_TOTAL × PAGES_PER_BLOCK × CW_NUM` | 1,048,576 |
| `L2P_REGIONS_PER_INSTANCE` | **每实例** | `FTL_CW_TOTAL ÷ 每 region 条目数(512)` | 2048 |
| `FTL_L2P_TABLE_BYTES` | **每实例** | `L2P_REGIONS_PER_INSTANCE × 512 × 4B` | 4 MiB |
| **全部块号**（§2.1） | **每实例** | §2.3 顺序累积 | 0..26 |
| `FTL_IU_TOTAL`（= `FTL_LBA_MAX`） | **全局** | `L2P_REGIONS_PER_INSTANCE × 512 × FTL_DIM_TOTAL` | 1,048,576 |
| `FTL_USER_CAPACITY_BYTES` | **全局** | `FTL_IU_TOTAL × 512B` | 512 MiB |

> **容量是推论不是输入**：`FTL_USER_CAPACITY_BYTES = FTL_IU_TOTAL × FTL_IU_BYTES`
> （条目总数 × 每 CW 用户区 512B）。改 `nand_config.h` ⇒ 容量自动跟随。
> `FTL_IU_BYTES` 取 `FC_BUFFER_USER_SIZE`（512）而非 `CW_SIZE`（528）：CW 槽 =
> `[META:4][CRC:4][USER:512][PARITY:8]`，其中 16B 由 FC 内部管理、不占 LBA 空间。

### 1.4 容量 / 地址空间换算表

| dim | CH×CE×LUN | 每实例 region | 全局 region | 每实例 LBA | **设备总 LBA** | **设备容量** |
|---|---|---|---|---|---|---|
| 1 | 1×1×1 | 2048 | 2,048 | 1,048,576 | 1,048,576 | **512 MiB** |
| 2 | 2×1×1 | 2048 | 4,096 | 1,048,576 | 2,097,152 | 1 GiB |
| 4 | 2×2×1 | 2048 | 8,192 | 1,048,576 | 4,194,304 | 2 GiB |
| 8 | 2×2×2 | 2048 | 16,384 | 1,048,576 | 8,388,608 | **4 GiB** |

**要点**：**每实例的数字永远不变**（2048 region / 1,048,576 LBA / 512 MiB），
变的只有设备总量 ×`FTL_DIM_TOTAL`。条带粒度恒为 1 个 region = 512 LBA = **256 KiB**（见 §3.6）。

---

## 二、分区布局

> **单一事实来源**：`include/ftl/ftl_config.h`（宏 + `ftl_partition_t` 分区表 +
> 打包映射 `ftl_l2p_region_placement()`），运行期分区表在 `src/ftl/ftl_config.c`
> 的 `g_ftl_partitions[]`（7 项）。**改布局只改这一处。**

### 2.1 Flash 分区图（当前几何下的展开值）

**⚠ 每个 `(ch,ce,lun)` 用同一份块号**：分片模型下每个 die 一套完整元数据，分区**不随维度错开**
（旧版按 `lun_stride` 错开是"LUN 作为地址维度"的模型，已删——见 §3.2）。

```
块号         用途                          块数来历
────────────────────────────────────────────────────────────
0 .. 15      L2P Table                     ⌈2048 region ÷ 128 region/块⌉ = 16
16 .. 17     Anchor（Metadata Directory）   ANCHOR_COPIES = 2（主备）
18 .. 19     BBT（主 + 备）                 BBT_COPIES = 2
20 .. 21     Checkpoint（主备）             CHECKPOINT_COPIES = 2
22 .. 25     Journal（4 块）                JOURNAL_BLOCKS = CW_NUM（CW 级 4× 映射流量）
26           Wear 表（erase_count）         WEAR_COPIES = 1（软状态单副本）
27 .. 4095   用户数据 / OP / Spare / 坏块
────────────────────────────────────────────────────────────
固定元数据 = 16 + 2 + 2 + 2 + 4 + 1 = 27 块（FTL_METADATA_BLOCK_END = 27）
```

**对账**（开机 free pool 统计）：

```
4096 − 27(元数据) − 50(spare) − 204(op) − 40(出厂坏块) ≈ 3775 free
```

### 2.2 每张表的尺寸推导

| 表 | 覆盖实体 | 每实体 | 字节数 ① | 页数 ② | 块数 ③ | 冗余 ④ | **实占** | 块位置 |
|---|---|---|---|---|---|---|---|---|
| **L2P Table** | 1,048,576 LBA（每实例） | 4B PBA | 4 MiB | 2048 | 16 | — | **16** | 0..15 |
| Metadata Directory | 2048 region | 7B | 14,336 B | **8** | 1 | ×2 块 ×3 份 | **2** | 16..17 |
| BBT | 4096 block | 1 bit | 512 B | 1 | 1 | ×2 | **2** | 18..19 |
| Checkpoint | 2048 bit + 头 | — | ~920 B | 1 | 1 | ×2 | **2** | 20..21 |
| Journal | — | 16B/entry | — | — | — | 环形 | **4** | 22..25 |
| Wear 表 | 4096 block | 12B | 48 KB | **25** | 1 | 单副本 | **1** | 26 |
| BST | 4096 block | 40B | 160 KB | — | — | SRAM | — | (RAM) |
| **合计** | | | | | | | **27 块** | 0..26 |

**逐条推导**（全部由 `nand_config.h` 几何 + `ftl_config.h` §二 策略常量算出）：

- **L2P**：1,048,576 × 4B = 4 MiB ÷ 2048B = 2048 页 ÷ 128 槽/块 = **16 块**（打包后，见 §2.2.1）。
- **Directory**：2048 region × 7B = 14,336B；每页装 256 条（`ANCHOR_ENTRIES_PER_PAGE`，
  256×7 = 1792 ≤ 主区 2048）⇒ 一份 **8 页**（`ANCHOR_PAGE_COUNT`）。每块内 3 份冗余
  （`ANCHOR_REDUNDANCY`）⇒ 每块占 24 页（≤ 64，`ANCHOR_PAGE_PER_BLOCK`）；主备 2 块
  （`ANCHOR_COPIES`）= **2 块**，共 **6 份** Directory，任一份可独立重建全表。
  Anchor 块的 plane 1 留空（全部页落 plane 0）。
- **BBT**：4096 × 1 bit = 512B = 1 页 → 1 块 ×2（主+备）= **2 块**。
- **Checkpoint**：头 ~920B（`checkpoint.h` 的 Header 格式，< 1 页）= 1 页 ×2 冗余 = **2 块**。
- **Journal**：**4 块**（策略 = `CW_NUM`，CW 级 4× 映射流量，见 §十）。
- **Wear**：4096 × 12B = 48KB → 全量快照 **25 页** → 1 块（软状态，单副本）。
  ⚠ 介质上**不是** 25 页顺序表，而是页式追加日志（见 §十）：25 页是"块满时一次
  compact 写全量快照"的页数，常态落盘只写 1–4 页。

#### 2.2.1 L2P region 打包公式

`ftl_l2p_region_placement(region_id, &block, &page, &plane)`（`ftl_config.h` §六，**唯一映射公式**，
`l2p_cache` 与 `metadata` 共用）：

```
block = L2P_BASE_BLOCK + region_id / 128
page  = (region_id % 128) / 2        ← 同页两 plane 各放一个 region
plane = region_id % 2
```

```
         plane0        plane1
page0    region0       region1     ← 同页两 plane 都用上
page1    region2       region3
...
page63   region126     region127
⇒ 每块 128 槽全用满 ⇒ 2048 ÷ 128 = 16 块
```

> 旧公式（`block = r >> 6`、`page = r & 0x3F`、`plane = r & 1`）**每块只用一半槽位**
> （64 region 占 128 槽），导致 L2P 要 32 块。改打包后省 16 块 = 2 MiB，
> 且连续 region 落在同页两 plane ⇒ L2P 写可走 multi-plane。**块位置变了 ⇒ 介质格式变更。**

### 2.3 为什么块号是"算出来的"而不是写死的

四层推导，**没有任何块号/容量是手写输入**：

```
① 几何量：页/块/plane/LUN 数、页主区字节、CW 数        ← 全部来自 nand_config.h
② 容量  ：CW 总数 ÷ 每 region 条目数 = region 数        ← 容量是几何的推论，不是输入
③ 块数  ：每个分区 = f(几何量, 策略常量)               ← 策略常量集中在 ftl_config.h §二
④ 起始块：前一分区起点 + 前一分区块数                   ← §三 顺序累积
```

换芯片（改 `nand_config.h`）或改策略（改 `ftl_config.h` §二）⇒ 容量与全表自动重排。
唯一的"写死"是 §二 的策略常量（副本数、每副本冗余页数、Journal 容量系数），
**每个都写明了策略理由**。

### 2.4 运行期分区表

`g_ftl_partitions[]`（`ftl_config.c`，7 项：L2P / Anchor / BBT / BBT(backup) / Checkpoint /
Journal / Wear），每项 `{name, first_block, block_count, redundancy, kind, table_bytes}`
**全部由宏派生**。消费方：

- `ftl_is_metadata_block(md_dir, block)`（`metadata.c`）——**表驱动**判元数据块
  （不再硬编码块号）：遍历 `FTL_PART_FIXED` 项判区间，再扫 Directory 的动态 L2P region 块。
  free pool 回填（`main.c`）与掉电恢复重建（`power_failure_recovery.c`）共用同一判据。
- 开机分区打印。

---

## 三、多维度（CH / CE / LUN）

> 本节是本文档**理解成本最高**的部分。三个层次：**物理形态（§3.1）→ 地址换算（§3.3）
> → 路由公式与算例（§3.4/§3.5）**。看不懂路由时，先回 §3.1 建立"每个 die 是一台小 SSD"的画面。

### 3.1 物理形态：一个 die = 一个完整 SSD

本 FTL 的模型是**分片（sharding）**：`g_ftl[CH_NUM][CE_NUM][LUN_NUM]` 里
**每个 `(ch,ce,lun)` 是一个完整独立的 FTL 实例**，各自持有 L2P 表、BBT、Anchor、Journal、
Checkpoint、Wear、分配器、BST。

**dim = 2×2×2 = 8 时的物理形态**（每个 die 内部**逐块相同**）：

```
      die(0,0,0)                die(1,0,0)                        die(1,1,1)
 ┌──────────────────┐      ┌──────────────────┐              ┌──────────────────┐
 │ 块 0..15   L2P    │      │ 块 0..15   L2P    │              │ 块 0..15   L2P    │
 │ 块 16..17  Anchor │      │ 块 16..17  Anchor │              │ 块 16..17  Anchor │
 │ 块 18..19  BBT    │      │ 块 18..19  BBT    │              │ 块 18..19  BBT    │
 │ 块 20..21  CP     │  ≡   │ 块 20..21  CP     │   ≡   …      │ 块 20..21  CP     │
 │ 块 22..25  Journal│      │ 块 22..25  Journal│              │ 块 22..25  Journal│
 │ 块 26     Wear    │      │ 块 26     Wear    │              │ 块 26     Wear    │
 │ 块 27..4095 用户  │      │ 块 27..4095 用户  │              │ 块 27..4095 用户  │
 └──────────────────┘      └──────────────────┘              └──────────────────┘
      512 MiB                   512 MiB                           512 MiB
      └─────────────────────── 设备总计 8 × 512 MiB = 4 GiB ───────────────────────┘
```

> **一句话记住**：**元数据是"复制"了 DIM 份，不是"条带"切成 DIM 段。**
> 8 个 die 各有一份**完整的 4 MiB L2P 表**（覆盖各自的 2048 个 region），
> 不是 8 个 die 拼出一份 L2P 表。这是理解多 LUN 的关键，也是 §3.2 的对比点。

**实测验证**（影子 `nand_config.h`，dim=1 → dim=8）：`L2P_BLOCKS`、`ANCHOR_BLOCK0`、`BBT_BLOCK`、
`JOURNAL_BLOCK0`、`FTL_METADATA_BLOCK_END`、`L2P_REGIONS_PER_INSTANCE`、`FTL_L2P_TABLE_BYTES`
**全部不变**；只有 `FTL_IU_TOTAL`（1,048,576 → 8,388,608）与容量（512 MiB → 4 GiB）变。

### 3.2 两模型对比：分片（本仓）vs LUN 条带

| 维度 | **模型 A：分片（本仓）** | 模型 B：LUN 作为地址维度（已删） |
|---|---|---|
| FTL 实例数 | `CH_NUM×CE_NUM×LUN_NUM` 个，各自独立 | 1 个全局 |
| L2P 表份数 | **DIM 份**，各 4 MiB（每实例） | **1 份**，按 LUN 条带化 |
| 分区块号 | **各 die 相同**（0..26） | 按 `lun_stride` 跨 LUN 错开 |
| 一个 region 的物理位置 | 自己 die 的块 `r / 128` | 跨 die 条带 |
| 一次 region flush | **只碰一个 die** | 可能跨多个 die（串行化） |
| 坏块影响 | 限于一个 die | 可能波及全表 |
| SRAM 代价 | **× DIM**（当前装不下，见 §3.7） | 省 SRAM |
| 主机 LBA 归属 | 按 region **切分**到实例 | 全局表内统一寻址 |

**为什么选分片（模型 A）**：

1. **对齐主控正解**（`fc_nand_design.md` §26）：多通道 / 多 CE / 多 die 并行，靠的是
   "不同实例的命令**同时在各自通道上飞**"。分片让每个 die 的写/擦/元数据更新互不干扰。
2. **一次 region flush 不跨 die**：region 是 flush 单位（见 §3.6），分片下它天然落在一个 die 上，
   flush 只碰一个实例；模型 B 会让一次 flush 跨 die，把并行变成串行。
3. **坏块隔离**：一个 die 的坏块不会让"全局唯一那份表"受损。

> **代价**：每实例 `g_ftl`(147.7 KB，`sizeof(ftl_context_t)`) + `g_blocks`(160 KB) ≈ **308 KB**，
> AXI_RAM 只有 384 KB ⇒ 第二个实例装不下。`main.c` 已加编译期断言把这条变成**编译错误**（见 §3.7）。

### 3.3 地址空间换算表

| dim | 每实例 region | **全局 region** | 每实例 LBA | **设备总 LBA** | **设备容量** |
|---|---|---|---|---|---|
| 1 | 2048 | 2,048 | 1,048,576 | 1,048,576 | 512 MiB |
| 2 | 2048 | 4,096 | 1,048,576 | 2,097,152 | 1 GiB |
| 4 | 2048 | 8,192 | 1,048,576 | 4,194,304 | 2 GiB |
| 8 | 2048 | 16,384 | 1,048,576 | 8,388,608 | 4 GiB |

（与 §1.4 同表；`FTL_IU_TOTAL = 全局 region × 512`，`容量 = FTL_IU_TOTAL × 512B`。）

### 3.4 路由：全局 region → (ch, ce, lun)

路由键是 **region 序号**（`region = lba >> L2P_REGION_SHIFT`，`L2P_REGION_SHIFT = 9`），
**不是页号也不是条目号**——一个 region（512 个 CW）整体落在一个 die 上。

`l2p_cache.h` 的四个 static inline（`controller.c` 的 `ssd_controller_route()` 调用）：

```
dim = region % FTL_DIM_TOTAL
ch  = dim % CH_NUM
ce  = (dim / CH_NUM) % CE_NUM
lun = dim / (CH_NUM × CE_NUM)
```

**dim = 2×2×2 = 8 时的逐步算例**：

| 全局 region | `dim` | `ch` | `ce` | `lun` | 落到 |
|---|---|---|---|---|---|
| 0 | 0 | 0 | 0 | 0 | die(0,0,0) |
| 1 | 1 | 1 | 0 | 0 | die(1,0,0) |
| 2 | 2 | 0 | 1 | 0 | die(0,1,0) |
| 3 | 3 | 1 | 1 | 0 | die(1,1,0) |
| 4 | 4 | 0 | 0 | 1 | die(0,0,1) |
| 5 | 5 | 1 | 0 | 1 | die(1,0,1) |
| 6 | 6 | 0 | 1 | 1 | die(0,1,1) |
| 7 | 7 | 1 | 1 | 1 | die(1,1,1) |
| 8 | 0 | 0 | 0 | 0 | die(0,0,0) ← **回到起点** |

> `FTL_DIM_TOTAL = 1` 时 `dim ≡ 0` ⇒ 恒为 `(0,0,0)`，单维度行为与旧版逐字一致。
> 分解顺序是 **ch 最低位、lun 最高位**——即"同一 lun 内先把 ch 铺满，再铺 ce，再铺 lun"。

### 3.5 反交织：全局 LBA → 实例内 local LBA

各实例的 L2P 表只覆盖**自己的** 2048 个 region，所以路由后要把全局 LBA 换算成**实例内 local LBA**。
**按 region 粒度反交织**（不是条目粒度）：

```
local = (region / FTL_DIM_TOTAL) × L2P_ENTRIES_PER_REGION + (lba & L2P_REGION_MASK)
```

**dim = 8 时的算例**：

| 全局 LBA | `region` | → die | 实例内 local LBA |
|---|---|---|---|
| 0 .. 511 | 0 | (0,0,0) | 0 .. 511 |
| 512 .. 1023 | 1 | (1,0,0) | 0 .. 511 |
| 1024 .. 1535 | 2 | (0,1,0) | 0 .. 511 |
| … | | | |
| 3584 .. 4095 | 7 | (1,1,1) | 0 .. 511 |
| 4096 .. 4607 | 8 | (0,0,0) | **512 .. 1023** |

> 每个实例收到的 region 序号**连续**（die(0,0,0) 收到全局 region 0, 8, 16, …），
> 所以 local = `region / DIM` 再乘回 512。`FTL_DIM_TOTAL = 1` 时退化为**恒等映射**
> （`local == lba`）。
>
> ⚠ **旧版是真 bug**：`lba_route_ch/ce/lun` 曾是**三个完全相同的表达式** `(lba>>9) % N`，
> `lba_to_local` 又是按**条目粒度**除维度数。维度 ≥2 时只有部分实例可达、其余容量不可寻址。
> 现已修正并验证：dim=8 下 8 个实例各收到恰好 1,048,576 个 LBA，**不可寻址 0、越界 0、
> 实例内重复 0**（双射）。

### 3.6 为什么条带粒度 = 1 个 region（256 KiB）

1 region = 512 LBA = 512 × 512B = **256 KiB**。条带就是"连续 256 KiB 分给同一个 die"。

**为什么这么"粗"**：region 是 **flush 单位**——一次 L2P 写回就是写一个 2 KB 的 region 页
（`l2p_cache_flush_one`）。region 整体落一个 die ⇒ **一次 flush 只碰一个 die**。
更细的条带（比如按页或按 CW 轮转）会让一次 flush 的 512 条 entry 跨多个 die，
把"一次同步写"变成"多次跨 die 同步写"，**反而串行化**。

### 3.7 提维度的前置条件（**未落地，登记在案**）

代码现在"维度通用且正确"（dim=1 下行为不变，dim≥2 的数学已修），但**真正把 `LUN_NUM` 提到 2
还做不到**，因为：

| # | 阻断点 | 说明 |
|---|---|---|
| 1 | **`LUN_ADDR_WIDTH = 0`** | `nand_config.h` 中 `LUN_ADDR_WIDTH = 0` ⇒ `PAA_LUN_MASK = PBA_LUN_MASK = 0` ⇒ **PAA/PBA 都装不下 LUN 字段**，L2P 表无法记录 die 号。这是**根因**。改它需同时改 `LUN_NUM`（芯片配置，未动）。 |
| 2 | **地址构造缺 lun** | `journal.c` 的 `pba_from_addr` 未填 `.lun`；`metadata.c` 的 `paa_with_lun()` 在当前配置下是**空操作**（随 1 生效）。 |
| 3 | **内存** | 每实例 `g_ftl`(147.7 KB) + `g_blocks`(160 KB) ≈ **308 KB**，AXI_RAM 只有 384 KB ⇒ 第二个实例装不下。`main.c` 已加编译期断言把这条变成**编译错误**。 |
| 4 | **共享状态需 per-instance 化** | `bbt.c` 的文件级 `g_bbt_sequence`/`g_bbt_dirty`；`completion_table.c` 的全局 handler/ctx；`main.c` 主循环只服务 `g_ftl[0][0][0]`。 |

> 本轮（2026-10-12）只做**布局 + 路由**，上列 1~4 未动。

---

## 四、地址与介质格式

### 4.1 三个地址空间

| 空间 | 位布局（**低位→高位**） | 用途 |
|---|---|---|
| **PAA**（芯片内） | `[CW:2][PAGE:6][PLANE:1][BLOCK:11][LUN:0]` | FC 层 / NAND 命令 |
| **PBA**（全局） | `[CW:2][PLANE:1][PAGE:6][BLOCK:11][LUN:0][CE:0][CH:0]` | FTL 层 / L2P entry |
| **LBA** | 与 PBA **位同构** | 主机寻址 |

⚠ **PAA 与 PBA 的 `PLANE`/`PAGE` 位序相反**（PAA 是 PLANE 在 PAGE 之上，PBA 是 PLANE 在 PAGE 之下）。
两套空间互相转换必须走 `paa_from_addr` / `pba_from_addr` / `pba_to_paa` 等构造函数，
**不能靠位运算手工拼**——历史上 10+ 处现场转换都在静默算错地址（v24/v47 教训）。

⚠ 当前 `LUN_ADDR_WIDTH = 0`、`CH_NUM = CE_NUM = 1` ⇒ PBA 的 `LUN`/`CE`/`CH` 三个字段宽度都是 0，
实际只用了低 20 位（CW+PLANE+PAGE+BLOCK）。见 §3.7。

### 4.2 介质格式变更清单（**须重新格式化**）

本布局相对旧版**变更了介质格式**，首次上板**必须先擦除芯片**（v24 教训）：

- L2P 表块位置（32 块 → 16 块打包）；
- Journal 2 块 → 4 块（`JOURNAL_BLOCKS = CW_NUM`）；
- 新增 Wear 块（块 26）；
- **Anchor 改为分页自包含格式**（2048 条 → 8 页/份，每页带 magic/sequence/chunk_index/CRC）。

本仓无 CLI format 命令，用调试器 / 外部工具擦芯片。

---

# 第二部分 · 设计讨论

## 五、为什么 IU = CW

- **主流主控**：页 16KB，IU 4KB，CW 1–2KB ⇒ **IU = 页/4**，且 **IU 比 CW 粗**（一个 IU 跨 4 个 CW）。
- **本仓**：页 2048B ⇒ **IU = 页/4 = 512B = CW**。
- 两个类比（`IU/页 = 1/4` 与 `IU/CW = 4`）在本仓**冲突**——因为本仓 page 只有 4 个 CW，
  真实器件有 16 个。**按"每页 4 个 IU"这个结构类比，IU = CW 与主流同构。**

⚠ 严格说主流主控**不按 CW 映射**（IU 比 CW 粗）；"按 CW"的正确理由**不是"照抄主流"**，
而是"把 IU 定在本仓器件的页/4"。选 CW 后 §1.2 的 1:1 自洽才成立。

## 六、L2P 表打包到 16 块

公式与前后对照见 §2.2.1。要点：

- 旧公式每块只用一半槽位 ⇒ 32 块；打包后 16 块，**省 16 块 = 2 MiB**。
- 连续 region 落在**同页两 plane** ⇒ L2P 写可走 **multi-plane**（旧公式不行）。
- 不动表内容；但**块位置变了 ⇒ 介质格式变更**（§4.2）。

## 七、写缓冲页打包（方案 A）

**问题**：器件编程单位 = page（2048B），不能单独编 1 个 CW。512B 扇区写必须变成页编程。

| 方案 | 做法 | 代价 | 主控真实性 |
|---|---|---|---|
| **(A) 页打包缓冲** | FTL 持「当前页缓冲」，攒够 4 个 CW 才编程 | 需写缓冲 + 掉电语义 | ✅ 主流做法 |
| (B) 读改写 RMW | 写 1 扇区 = 读旧页 + 改 1 CW + 编新页 | 读放大 1× | ⚠ 老式 |
| (C) 单 CW 编程 | 编 1 CW + 3 个 0xFF | 写放大 4×，部分页编程不可靠 | ❌ 不推荐 |

**主流 = (A)**，选 (A)。

**掉电语义**：本仓无电容 ⇒ 写缓冲内容随 **journal** 落盘；或写缓冲只做「同页扇区合并」，
不跨掉电窗口。

## 八、有效位粒度：per-CW + L2P 权威

**为什么必须 per-CW**：页打包后，一页 4 个 CW 来自**不同 LBA**；覆盖其中 1 个 LBA，
**只失效那 1 个 CW**，页里剩 3 个还有效。**per-page 位图表达不了「半有效」的页。**

**主流**：有效位粒度 = 映射粒度（IU）⇒ 本仓 per-CW。
代价：`valid_map` 16B → 64B/块，BST 40B（`bst.h`）→ **88B**，160KB → **352KB**。

**本仓现实**：AXI_RAM 余量不足以放下 per-CW 位图（当前 323.8 / 384 KB，余 60.2 KB；
+48 KB 后与其他需求挤在一起）。

**关键洞察——有效位能从 L2P 推出来**：某物理槽有效 ⟺ 有 LBA 的 L2P 指向它。
**证据**：`power_failure_recovery.c` 开机重建时，正是**反查 L2P 把 `valid_map` 填回去**的。
⇒ **`valid_map` 不是权威状态，是 L2P 的缓存/加速结构。**

**结论**：

- BST 存 **per-CW `valid_count`**（GC victim 选择用；`gc.c`、`wear_leveling.c`）。
- 页内逐 CW 有效性 **查 L2P**（`gc.c` 现在就是这么设计的「读 META → 查 L2P → 判有效」，
  只是要从 1 LBA/页 改成 **4 LBA/页**）。
- 位图降级为 **per-page 粗提示**（扫位图跳过整页全无效的页）或砍掉。
- **备选**：若 L2P cache 从 64 region 降到 32（省 64KB），per-CW 位图可装下
  ——这是「缓存命中率 ↔ 有效性精度」的取舍。

## 九、L2P 表存储（保持 DRAM-less）

**主流两条路**：有 DRAM → L2P 常驻 DRAM（企业级）；**DRAM-less（eMMC/UFS）→ L2P 在 NAND，
SRAM 缓存热点 region**。

**本仓对标 eMMC/UFS** ⇒ 保持现状（NAND L2P Table + 64 region CLOCK cache，`l2p_cache.h`）。

**补充**：表块走**预留固定区**（不从用户池动态拿），但**表块本身要做磨损均衡**
（元数据块也会被反复擦写）——Metadata Directory 就是干这个的。

## 十、Journal / Wear / Checkpoint 的容量策略

- **Journal = `CW_NUM` 块（4）**：CW 级下每次页写 = **4 条 journal**（页级时 1 条）
  ⇒ 流量 **4×**、寿命 ÷4。容量取 `CW_NUM` 使 Journal 能容纳两次 checkpoint 之间的映射更新。
  （或做「同 region 多次写合并」。）
- **Wear = 1 块（单副本）**：块元数据（擦除/读/写计数）持久化。**软状态**——丢了只退化 WL，
  不丢数据 ⇒ 单副本即可。**已落地**（`src/ftl/wear.c`，见 §十一）。
  ⚠ **介质上是"块内页式追加日志"，不是顺序全量表**：页序 == sequence 序，每页自包含
  （magic + sequence + entry_count + CRC），语义 = 绝对值 + 最新胜；块满才擦一次并
  从头写全量快照（17 页）。用日志而非"每次整表重写"是因为块 26 只有 64 页，
  整表重写会让它在 ~2.6 次落盘后被擦一次（磨损远超数据块）。
  两个计数各 8B 一条目（`block` + `read_count` + `erase_count`），
  32 KB 是**逻辑表字节**（`FTL_WEAR_TABLE_BYTES`），不是介质占用量。
  （v0.56：`program_count` 槽随 BST 字段删除撤出，条目 12B → 8B、快照 25 → 17 页。）
- **Checkpoint = 2 块（主备）**：**硬状态**（丢了丢数据）⇒ ×2。
- **冗余取舍通则**：**硬状态**（L2P / BBT / Checkpoint / Directory）必须 ×2；
  **软状态**（Wear）单副本。

---

# 第三部分 · 状态与索引

## 十一、落地状态

### ✅ 已落地（2026-10-12）

| 层 | 改动 | 文件 |
|---|---|---|
| **分区布局** | 分区宏/大小/偏移/公式集中一处（单一事实来源） | `include/ftl/ftl_config.h`、`src/ftl/ftl_config.c` |
| **L2P 布局** | 打包公式（16 块，每块 128 region） | `ftl_config.h` §六、`l2p_cache.h` |
| **元数据** | Journal 2→4 块；Wear 块位预留（宏 + 表项 + 谓词） | `journal.h`/`journal.c`、`metadata.c` |
| **元数据** | `ftl_is_metadata_block` 表驱动（消硬编码块号） | `src/ftl/metadata.c` |
| **维度** | 布局维度无关化：删 `lun_stride`/`ANCHOR_BLOCK_STRIDE`/`*_PER_LUN`，各 die 同一份块号 | `ftl_config.h`、`ftl_config.c`、`metadata.c` |
| **维度** | LBA 路由修正：`lba_route_*` 三维分解 + `lba_to_local` 按 region 反交织 | `include/ftl/l2p_cache.h` |
| **元数据** | Directory 扩容 128→2048 entry；Anchor 改**分页自包含**（8 页/份 × 3 份 × 2 块） | `metadata.h`、`metadata.c` |
| **磨损** | **Wear 表 load/save 落地**（P0-1）：块内页式追加日志 + 开机回填 BST + 节拍落盘 | `include/ftl/wear.h`、`src/ftl/wear.c`、`ftl_config.h` |

### ⬜ 仍为设计（未改代码）

| 层 | 改动 | 文件 |
|---|---|---|
| 请求层 | `io.lba = start_lba + i` → 每页步进 **4**（扇区 LBA） | `src/controller/request.c` |
| FTL 写路径 | 1 页 → **4 条** L2P entry + 4 条 journal；页缓冲打包 | `src/ftl/ftl.c` |
| FTL 读路径 | 按 512B 扇区查 L2P | `src/ftl/ftl.c` |
| GC / WL | 逐 CW 查 L2P（1 LBA/页 → 4 LBA/页） | `src/ftl/gc.c`、`src/ftl/wear_leveling.c` |
| BST | `valid_count` 改 per-CW；位图降级/砍掉 | `include/ftl/bst.h` |

### ⚠ 上板前置

**介质格式变更**（§4.2）⇒ 首次上板**须先擦除芯片**。

## 十二、待决与风险

1. **写缓冲落盘**：方案 A 的缓冲内容掉电恢复方案待定（进 journal？只做同页合并？）。
2. **per-CW 位图 vs L2P cache 大小的取舍**：若要 per-CW 位图，需把 L2P cache 64→32 region 腾 SRAM。
3. **介质格式变更**：新增 Wear 块 + L2P 打包改位置 + Journal 4 块 + Anchor 分页 ⇒ **须重新格式化**。
   Wear 表 load/save 落地（P0-1）**不新增**格式变更：块 26 早已在分区表里预留且被
   `ftl_is_metadata_block` 排除，本次只是第一次往里写内容 ⇒ 不额外要求重新格式化。
4. **`CONFIG_FC_RAID` 保持 0**（用户 2026-10-10 明确）——本方案不碰。
5. **P0-2 Metadata Directory 容量** — ✅ **已落地**。旧版 `METADATA_DIR_ENTRIES = 128` < 2048 region
   ⇒ 刷新 region ≥128 时 `metadata_dir_update` 静默丢弃新位置，而 `metadata_dir_alloc_region`
   照旧返回一个写在用户区的 PAA ⇒ **重启后该 region 回落到固定位置，更新永久丢失**。
   现改为 `METADATA_DIR_ENTRIES = L2P_REGIONS_PER_INSTANCE`（2048），所有 region 在
   `metadata_dir_fill_fixed()` 一次性登记 ⇒ "满则丢弃"分支对 L2P region **不可达**。
   ⚠ **连带后果**：所有 region 都预登记 ⇒ `metadata_dir_lookup` 必然命中 ⇒
   `metadata_dir_alloc_region` 目前**不可达**，L2P region 不会被搬到动态块（与旧版 region <128
   的行为一致，属已跑过的路径）。

   **⚠ v0.50 补一层（真缺陷在此）**：上面这条「扩容」把机制从「半工作」翻成了「**全死**」——
   `fill_fixed` 预登记 ⇒ `dirty` 恒 0 ⇒ `metadata_dir_flush_to_anchor` 早退 ⇒ **Anchor 块一次
   都没写过**。但**根缺陷在更下面**：`l2p_cache_flush_one`（`l2p_cache.c`）把每个 L2P region
   写回**固定地址且从不擦除**。NAND 只能 1→0，对已编程页二次编程 = 新数据 **AND** 旧数据
   ⇒ **静默的映射损坏**。同病遍布全部元数据：全仓 `ftl_sync_erase_block` 的调用者原先只有
   `journal.c` / `gc.c` / `wear.c`——**没有任何 L2P / Anchor / BBT / Checkpoint 块被擦过**。
   潜伏原因：顺序冒烟测试每个 region 只 flush 一次（64 行 × 512 项 = 32768 LBA，测试写不到）。
   顺带挖出的第二个致命项：`checkpoint_create` 每次重写**同 6 页**，第二次起头即损坏 ⇒
   `checkpoint_find_latest` 失败 ⇒ `ftl_boot_fresh_start` ⇒ **映射全丢**（T3 定时触发
   「10 s 空闲」⇒ 板子空闲跑过 10 s 就必中）。
   ⇒ 全部元数据统一到「只写擦除过的空页 + 每条带 sequence + 块池轮转 + 最新有效胜」，
   见 `ftl_revision_history.md` 的 ADR（v0.50）。

   **⚠ 尚未做完（P0-2 的阶段 3）**：`metadata_dir_reclaim` 只回收活计数为 0 的日志块 ⇒
   挂着**永不再改写**的冷 region 的块**永久占块**（顺序写满 2048 region 用 32 块，之后只热写
   一小撮 ⇒ 未触及的块全卡住，Free Pool 慢性流失）。补法 = victim（`l2p_live` 最小）+ 搬移 +
   重入切断，**完整设计已写入 `ftl_optimization_plan.md` 的 P0-3**（含判据与依赖）。
   **依赖本次 v0.50 先上板验证通过；长时压测前必须补。**
6. **登记（本次未改，属其它文档/代码）**：
   - `src/main.c` 中两条内存注释仍是旧值：`g_ftl` 标 "131.5KB"（实测 `sizeof(ftl_context_t)`
     = **147.7 KB**）；AXI_RAM 标 "≈355.5KB（剩 11.5KB）"。
     最新实测（v0.50 构建，`make all` 输出的 SRAM Usage）：**AXI_RAM 332.3 / 384 KB（86.5%，余 51.7 KB）**、
     AXI_NOCACHE 70.1 / 128 KB、SRAM2 44.1 / 128 KB、Total 446.8 / 880 KB（50.8%）。
     （v0.50 的 `l2p_live[FTL_BLOCK_TOTAL]` 又吃掉 ~8 KB，故与上一轮实测值不同。）
   - `docs/design/ftl_design.md`、`docs/design/ftl_api_usage.md` 仍写着旧布局标识符
     （`L2P_REGIONS_TOTAL` / `L2P_REGIONS_PER_LUN` / `L2P_BLOCKS_PER_LUN`、L2P 32 块、
     Journal 40/41 块、Directory 128 条 / 904B）。本次未改。

## 十三、关键文件索引

| 主题 | 文件 |
|---|---|
| **分区布局（单一事实来源）** | `include/ftl/ftl_config.h`、`src/ftl/ftl_config.c` |
| L2P 映射 / cache / **路由** | `include/ftl/l2p_cache.h` |
| 元数据 / Directory / Anchor | `include/ftl/metadata.h`、`src/ftl/metadata.c` |
| BST / 有效位 | `include/ftl/bst.h` |
| GC / WL | `src/ftl/gc.c`、`src/ftl/wear_leveling.c` |
| Journal / Checkpoint | `include/ftl/journal.h`、`include/ftl/checkpoint.h` |
| 写 / 读路径 | `src/ftl/ftl.c` |
| 请求拆分 / **路由调用点** | `src/controller/request.c`、`src/controller/controller.c` |
| 几何 / 地址空间 | `include/nand/nand_config.h` |
| 内存预算断言 | `src/main.c`（`BOARD_AXI_RAM_SIZE` / `FTL_AXI_RAM_RESERVED`） |
| 分层判据 | `docs/design/fc_nand_design.md` §26 |
| 优化计划 | `docs/design/ftl_optimization_plan.md` |

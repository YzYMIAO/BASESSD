# FTL 层完整设计方案

> 本文档记录 soft_ssd 项目中 FTL（Flash Translation Layer）模块的完整设计，包括架构选型、子模块设计、行业对比、接口定义和实现规划。
>
> 本文档与 `fc_nand_design.md`（FC + NAND 底层）平行，二者共同构成 SSD Firmware 的完整设计方案。

---

## 变更历史

| 版本 | 日期 | 变更内容 | 作者 |
|------|------|---------|------|
| v0.1 | 2026-09-21 | 初始框架：页级 L2P + 顺序分配 | Claude |
| v0.2 | 2026-09-23 | 完整设计：DRAM-less L2P cache、Region-based caching、CLOCK、Journal、Checkpoint | Claude |
| v0.3 | 2026-09-23 | 架构决策记录、行业对比、Block 分类、Block 生命周期图、FTL 接口说明 | Claude |

---

## 目录

1. [架构选型与行业背景](#1-架构选型与行业背景)
2. [NAND 几何与资源预算](#2-nand-几何与资源预算)
3. [NAND Block 分类](#3-nand-block-分类)
4. [Block 生命周期](#4-block-生命周期)
5. [L2P 映射表设计](#5-l2p-映射表设计)
6. [SRAM Cache 设计](#6-sram-cache-设计)
7. [Journal 与掉电恢复](#7-journal-与掉电恢复)
8. [Checkpoint 机制](#8-checkpoint-机制)
9. [架构决策记录](#9-架构决策记录)
10. [企业级实现对比](#10-企业级实现对比)
11. [FTL 对外接口](#11-ftl-对外接口)
12. [与 FC 层接口约定](#12-与-fc-层接口约定)
13. [未解决问题](#13-未解决问题)
14. [Phase 规划](#14-phase-规划)
15. [附录：核心数据结构原型](#15-附录核心数据结构原型)

---

## 1. 架构选型与行业背景

### 1.1 三种映射策略对比

| 维度 | 块级映射 (Block Mapping) | 混合映射 (Hybrid BAST/FAST) | **纯页级映射 (Page Mapping)** ✅ |
|------|-------------------------|----------------------------|-------------------------------|
| 原理 | LBA → Block# + 固定页偏移 | 日志块(页级) + 数据块(块级) | LBA → Page/CW# |
| 表大小 | ~16 KB / 512MiB | 几百 KB | **4 MB / 512MiB** |
| 写放大 | 5~10x ❌ | 2~3x | **1.0~1.3x** ✅ |
| 灵活性 | 最低 | 中 | **最高** |
| GC 复杂度 | 简单（整块操作） | 中（merge 策略复杂） | **高（逐 CW 搬移）** |
| 实现复杂度 | 最简单 | **最复杂** | 中等 |
| 主流应用 | IoT / 极低端控制器 | 已淘汰（2012 年前 U 盘） | **所有现代 SSD** ✅ |

### 1.2 行业现状

```
SSD 类型                      映射策略                原因
─────────────────────────────────────────────────────────────────
企业级（Samsung PM9A3、       ✅ 纯页级映射（4KB 粒度）   性能/WA/延迟，几 GB DRAM 不差表空间
 Intel DC P5800X）
消费级旗舰（990 PRO、         ✅ 纯页级映射（4KB 粒度）   1~2GB DRAM，全表常驻
 SN850X、KC3000）
消费级入门（SN580、           ✅ 纯页级映射（4KB 粒度）   HMB 用主机内存做 cache，还是页级
 NV2）
UFS 4.0（手机）               ✅ 纯页级映射（4KB 粒度）   L2P table on NAND + SRAM cache
─────────────────────────────────────────────────────────────────
块映射 / 混合映射              ❌ 已经不用                2010 年前技术
```

**结论**：纯页级映射是绝对主流。

### 1.3 混合映射（BAST/FAST）为何被淘汰

BAST（Block Associative Sector Translation）和 FAST（Fully Associative Sector Translation）是 2007~2012 年的技术，核心原因是：

| 变化 | 影响 |
|------|------|
| NAND Page 增大到 16~32 KB | 表大小相对缩小（同样容量的 entry 更少）|
| 控制器 SRAM 增大到 1~2 MB | 能放下更大的 cache |
| DRAM 成本暴跌 | 入门级也可配 256MB~1GB DRAM |
| 无 DRAM 方案用 HMB | 借用主机内存，空间管够 |
| QLC/TLC P/E 寿命有限 | 混合映射的 merge 产生大量额外写入 |

混合映射要解决的（表空间不足），已被硬件成本下降解决。块映射的 WA 在 QLC/TLC 时代更是致命。

### 1.4 当前设计选择

| 决策项 | 选择 | 理由 |
|--------|------|------|
| 映射策略 | **纯 CW 级（512B）页映射** | 与 FC 完全对齐，行业标准做法 |
| L2P 存储 | **NAND 元数据 Block（DRAM-less）** | 学习嵌入式/UFS 主流方案 |
| L2P 缓存 | **SRAM region cache (64~256 KB)** | 1MB SRAM 可支持 |
| 替换策略 | **CLOCK** | LRU 命中率，O(1) 开销 |
| 写回策略 | **Write-back + Journal** | 性能与掉电安全的折中 |
| FTL → NAND | **通过 FC 接口** | 经过 CRC/ECC/Random 完整数据保护 |
| RAID | **不实现** | 复杂度高，当前 SLC 暂不需要 |

---

## 2. NAND 几何与资源预算

### 2.1 NAND 几何

| 参数 | 值 |
|------|------|
| NAND 型号 | MT29F4G08ABADAWP（SLC） |
| Page main | 2048 B |
| Page spare | 64 B |
| CW 布局 | [META:4][CRC:4][USER:512][PARITY:8] = 528 B |
| CW/Page | 4 |
| Pages/Block | 64 |
| Blocks/Plane | 2048 |
| Planes | 2 |
| 总 Pages | 262,144 |
| 总用户容量 | 512 MiB |

### 2.2 映射粒度关系

```
你的 SLC 2048B page:                  TLC 16KB page:
┌──────┬──────┬──────┬──────┐         ┌──────────┬──────────┬──────────┬──────────┐
│ CW0  │ CW1  │ CW2  │ CW3  │         │ CW0      │ CW1      │ CW2      │ CW3      │
│ 512B │ 512B │ 512B │ 512B │         │ 4KB      │ 4KB      │ 4KB      │ 4KB      │
└──────┴──────┴──────┴──────┘         └──────────┴──────────┴──────────┴──────────┘
映射粒度 = 512B = 1 CW              映射粒度 = 4KB = 1 CW
1 Page = 4 LBA entries              1 Page = 4 LBA entries
总 LBA = 1,048,576                  总 LBA = (容量 / 4KB)
```

**本质**：结构一致，只是 CW USER 区大小不同。ECC 引擎的编解码单位（CW）决定了映射粒度。

### 2.3 SRAM 预算

| 占用方 | 大小 | 占比 |
|--------|------|------|
| FC 控制结构（Pool + Queue） | ~77 KB | 7.5% |
| L2P Cache (64 regions) | ~131 KB | 12.8% |
| FTL 元数据（allocator + bitmap + BBT + 擦除计数） | ~45 KB | 4.4% |
| Journal 缓冲 | ~2 KB | 0.2% |
| 栈 + 其他运行时 | ~32 KB | 3.1% |
| **小计** | **~287 KB** | **28%** |
| 剩余（Firmware / 其他） | ~737 KB | 72% |
| **总计** | **1 MB** | **100%** |

### 2.4 SDRAM（8MB）用途

| 用途 | 大小 | 说明 |
|------|------|------|
| GC 搬运缓冲 | 256 KB | 读/写各 128 KB |
| Host NVMe 命令缓冲区 | ~1 MB | PRP/SGL 数据传输 |
| FTL 元数据暂存 | 32 KB | 持久化到 NAND 前暂存 |
| 调试日志、性能计数器 | ~6.5 MB | 剩余空间 |
| **总计** | **8 MB** | |

> SDRAM 不参与 L2P 缓存——DRAM-less 设计。

---

## 3. NAND Block 分类

纯页映射（Page Mapping）下，NAND 物理空间划分为四类 Block：

### 3.1 元数据区（Metadata Blocks）

固定位置，不由 allocator 管理，出厂时必须是好块：

```
┌──────────────┬──────┬──────────────┬─────────────────────────────┐
│ 类型          │ 数量  │ 位置          │ 内容                         │
├──────────────┼──────┼──────────────┼─────────────────────────────┤
│ L2P 表        │ ~32  │ Block 0~31   │ 完整 L2P 映射表（2048 regions）│
│ Checkpoint   │  2   │ Block 32~33  │ L2P 快照 + header           │
│ Journal      │  2   │ Block 34~35  │ 元数据变更日志               │
│ BBT          │  1   │ Block 36     │ 坏块表（出厂 + 运行时）       │
│ Block 擦除计数 │  1   │ Block 37     │ 4096 Blocks × 2B 擦除计数表   │
│ Reserved     │  2   │ Block 38~39  │ 替换备用（坏块替换）          │
├──────────────┼──────┼──────────────┼─────────────────────────────┤
│ 小计          │ ~40  │              │ 约 1% 的容量                 │
└──────────────┴──────┴──────────────┴─────────────────────────────┘
```

### 3.2 数据区（Data Blocks）

纯页映射下所有 Data Block 地位相等——没有"日志块"和"数据块"之分：

```
每个 Data Block 的状态机：
  Open Block    → 当前正在顺序写入，每 Plane 一个（可扩展多温区）
  Closed Block  → 已写满 64 页，含有效页 + 无效页，等待 GC 回收
  Free Block    → 已擦除（全 0xFF），在 free pool 中待分配
```

### 3.3 空闲池（Free Pool）

所有已擦除的 Free Block 组成的池子。Allocator 从中取 Block 分配写入。

### 3.4 坏块（Bad Blocks）

出厂坏块 + 运行时新增坏块，BBT 管理，分配时跳过。

### 3.5 与企业方案对比

| 维度 | 本方案 | 企业级（Samsung / Solidigm） |
|------|--------|-----------------------------|
| Metadata 位置 | 固定 Block 0~39 | 动态分配 + RAID1 镜像保护 |
| 数据区分区 | 单一 | Hot/Cold/Normal 多温区 |
| 空闲池 | 单池 | Pre-conditioned pool（预擦除保证 QoS）|
| 坏块替换 | Block skip | Block sparing + RAID stripe 重映射 |
| Over-provisioning | 无单独预留 | 7~28% 系统预留 |

---

## 4. Block 生命周期

### 4.1 完整流程

```
                ┌──────────────────┐
                │   Free Pool      │  ← 已擦除，全 0xFF，等待分配
                │   初始有空闲块    │
                └────────┬─────────┘
                         │ allocator: 选最少擦除块分配（动态磨损均衡）
                         ▼
                ┌──────────────────┐
                │   Open Block     │  ← 当前写入位置，顺序写页
                │   [已写47页][空]  │    每 Plane 1 个
                └────────┬─────────┘
                         │ 写满 64 页
                         ▼
                ┌──────────────────┐
                │  Closed Block    │  ← 等待 GC 回收
                │  V=有效 I=无效    │    VVI VIVV ...
                │  [VVVIVIIIV...]  │    无效页由 L2P 更新产生
                └────────┬─────────┘
                         │ free pool < 阈值 → 触发 GC
                         ▼
                ┌──────────────────────────────────────────┐
                │  GC Victim Block                          │
                │  ① 贪心选有效页最少的 Block                │
                │  ② 读所有有效页（通过 FC）                 │
                │  ③ 写到新的 Open Block                    │
                │  ④ 更新 L2P（cache write → journal）      │
                │  ⑤ Erase victim                          │
                │  ⑥ 归还 Free Pool                        │
                └──────────────────────────────────────────┘
```

### 4.2 关键说明

- 不存在"日志区→数据区"的 merge 操作（那是混合映射的概念）
- 纯页映射下，数据落地就是最终位置
- Block 生命周期是 Free → Open → Closed → GC Erase → Free 的循环
- GC 时的"搬移"是：读 Closed Block 的有效页 → 写到新的 Open Block → 更新映射 → 擦除原 Block

### 4.3 各阶段状态详情

```
一个 Block 的内部状态（64 页 × 4 CW = 256 CW）：
  ┌──────────────────────────────────────┐
  │ Page  0: [LBA=0][LBA=1][LBA=2][LBA=3] 有效
  │ Page  1: [LBA=4][ L=I  ][L=I  ][L=I  ] 页部分无效
  │ Page  2: [LBA=8][LBA=9][ L=I  ][ L=I  ] 无效页
  │ ...
  │ Page 63: [LBA=252][LBA=253][LBA=254][LBA=255] 全部有效
  └──────────────────────────────────────┘

GC 视角：
  - 有效 CW：有 L2P entry 指向它
  - 无效 CW：已被新版本覆盖（L2P 指向另一个 PPA）
  - 无效比 = 1 - (有效 CW 数 / 256)
  - GC 选 victim 时优先选无效比最高的 Block
```

---

## 5. L2P 映射表设计

### 5.1 映射粒度

```
LBA 0x00000 ──→ CW 0 of Page X ──→ PAA(page, plane, block, page, cw=0)
LBA 0x00001 ──→ CW 1 of Page X ──→ PAA(page, plane, block, page, cw=1)
LBA 0x00002 ──→ CW 2 of Page X ──→ PAA(page, plane, block, page, cw=2)
LBA 0x00003 ──→ CW 3 of Page X ──→ PAA(page, plane, block, page, cw=3)
LBA 0x00004 ──→ CW 0 of Page X+1 ──→ ...
```

- 1 entry = `nand_paa_t`（uint32_t），`[CW:2][PAGE:6][PLANE:1][BLOCK:11]` = 20 bit 有效
- 每 1 个 LBA 对应 1 个 CW（512B user data）
- **必须整页写入**：SLC NAND 不支持 partial page program，FTL 写 512B 的物理操作始终是整页（4 CW）写入

### 5.2 Region 分片

| 概念 | 值 | 说明 |
|------|----|------|
| Entry 大小 | 4 B | `nand_paa_t` |
| Region 大小 | 2,048 B | 512 entries × 4B = 1 NAND page |
| Region 数 | 2,048 | 4 MB / 2 KB |
| 全表占用 NAND | 32 blocks | 2,048 pages / 64 pages-per-block |

```
L2P 表（NAND 元数据区 Block 0~31）：
  Block 0:  Region 0000 ~ Region 0063 (64 pages)
  Block 1:  Region 0064 ~ Region 0127
  ...
  Block 31: Region 1984 ~ Region 2047

每 Region = 512 条 entry：
  entry[0]   → PAA for LBA = region_id × 512 + 0
  entry[1]   → PAA for LBA = region_id × 512 + 1
  ...
  entry[511] → PAA for LBA = region_id × 512 + 511
```

### 5.3 Region 物理地址计算

```c
#define L2P_BASE_BLOCK      (0)     // 元数据从 Block 0 开始
#define L2P_TOTAL_BLOCKS    (32)    // 32 blocks for full table

static nand_paa_t l2p_region_ppa(uint16_t region_id) {
    uint16_t block_offset = region_id / 64;   // pages per block
    uint16_t page_in_block = region_id % 64;
    return paa_make(page_in_block, 0, L2P_BASE_BLOCK + block_offset, 0, 0);
}
```

### 5.4 LBA 分解

```c
uint16_t region_id  = lba >> 9;      // lba / 512 → 0..2047
uint16_t entry_idx  = lba & 0x1FF;   // lba % 512 → 0..511
```

---

## 6. SRAM Cache 设计

### 6.1 数据结构

```c
#define L2P_ENTRIES_PER_REGION   (512)                     // region 内 PAA 数
#define L2P_REGION_SIZE          (L2P_ENTRIES_PER_REGION * sizeof(uint32_t))  // 2KB
#define L2P_REGIONS_TOTAL        (2048)                     // 总 region 数
#define L2P_CACHE_SIZE           (64)                       // 可配置：32/64/128

typedef enum {
    L2P_CACHE_INVALID = 0,      // cache 行无效（冷启动或已驱逐）
    L2P_CACHE_CLEAN,            // cache 行有效，未修改
    L2P_CACHE_DIRTY,            // cache 行有效，已修改（需写回）
    L2P_CACHE_FLUSHING,         // 正在写回 NAND（不可驱逐）
} l2p_cache_state_t;

typedef struct {
    nand_paa_t         entries[L2P_ENTRIES_PER_REGION];   // 512 条 PAA = 2KB
    uint16_t           region_id;                          // 0..2047
    l2p_cache_state_t  state       : 2;                    // 状态
    uint16_t           access_count : 14;                  // CLOCK 访问计数
    uint32_t           last_access;                        // 时间戳
} l2p_cache_line_t;

typedef struct {
    l2p_cache_line_t lines[L2P_CACHE_SIZE];                // 64 × (2KB + 8B) ≈ 129 KB
    uint16_t         clock_hand;                           // CLOCK 替换指针
    uint32_t         hit_count, miss_count;                // 统计
} l2p_cache_t;
```

### 6.2 查找流程

```
l2p_cache_read(LBA)
    │
    ├── region_id = LBA / 512
    ├── entry_idx = LBA % 512
    │
    ├── Cache hit: 遍历 lines, region_id 匹配且 state ≥ CLEAN
    │   → access_count++, return entries[entry_idx]
    │
    └── Cache miss:
        ├── 1. evict: CLOCK 算法选 victim
        ├── 2. 如果 victim dirty → 写回 NAND（flush）
        ├── 3. 从 NAND 读 region（通过 FC 接口，读 1 page = 2KB）
        ├── 4. 拼回 victim->entries（4 CW × 512B）
        ├── 5. 更新 region_id, state = CLEAN, access_count = 1
        └── 6. return entries[entry_idx]
```

### 6.3 更新流程（Write-back）

```c
l2p_cache_write(LBA, new_paa) {
    // 查找或加载 region（同 lookup）
    line = l2p_cache_find_or_load(cache, region_id);
    // 修改 entry
    line->entries[entry_idx] = new_paa;
    // 标记 dirty（延迟写回 NAND）
    line->state = L2P_CACHE_DIRTY;
}
```

### 6.4 CLOCK 替换算法

```
逐出流程：

  ① clock_hand 指向当前检查的 line
  ② line->state == FLUSHING → 跳过（写回中）
  ③ line->state == DIRTY    → 返回该 line（调用者 flush 后再用）
  ④ line->state == INVALID  → 直接占用
  ⑤ access_count > 0        → access_count--，检查下一个（给第二次机会）
  ⑥ access_count == 0       → 回收

CLOCK 是 LRU 的近似。嵌入式环境无动态内存分配，CLOCK 只需一个整数字段。
命中率接近 LRU，驱逐 O(1) amortized。

#### 未来升级方向（标记待定）

以下为 CLOCK 的优化方向，当前阶段不实现，留作后续 Phase 3 优化选项：

| 升级方向 | 说明 | 触发条件 | 预期收益 |
|---------|------|---------|---------|
| **CLOCK-pro** | access_count 从固定位宽改为动态阈值，热数据更易留在 cache 中 | Phase 3 性能优化 | 命中率 +3~5% |
| **自适应模式切换** | 检测到 sequential 访问时临时启用 prefetch（预加载后续 region）；随机访问时回到 CLOCK 保守模式 | Sequential 检测 | 顺序读场景命中率接近 100% |
| **Thrashing 降级** | 命中率低于 10% 时，切换 write-through + 停 background flush | miss_rate > 90% | 避免性能断崖 |
```

### 6.5 Thrashing 检测

全盘随机写入时 cache 命中率可能跌至 1.6~6.3%，触发 thrashing：

```c
if (miss_rate > 90%) {
    // 降级策略 1：write-through（避免 dirty 写回）
    // 降级策略 2：增大 batch，一次加载多个连续 region
    // 降级策略 3：停 background flush，节省 NAND 带宽
}
```

---

## 7. Journal 与掉电恢复

### 7.1 为什么需要 Journal

Write-back cache 的致命问题：掉电时 dirty cache 丢失，L2P 映射无法恢复。

```
时序：
  ① FTL 写数据到 NAND ✅（数据在 NAND 上安全）
  ② l2p_cache_update(LBA, new_ppa) → cache dirty
  ③ [掉电] ❌（cache 内容未写回 NAND L2P 区域）

后果：数据在 NAND 上，但 L2P 映射丢失 → 数据永远不可达
```

### 7.2 Log-structured Journal

```
每次 L2P 修改前，先在 Journal 中追加一条记录：

┌──────────────────────────────────────────────────┐
│ Journal Entry（16B）                              │
│ ┌────────────────────────────────────────────┐   │
│ │ LBA (4B) | old_PAA (4B) | new_PAA (4B)    │   │
│ │         | sequence (4B)                    │   │
│ └────────────────────────────────────────────┘   │
│                                                   │
│ Journal 区域（Block J0, J1，环形）：               │
│ Page 0: [entry_0][entry_1]...[entry_127]          │
│ Page 1: [entry_128]...                            │
│                                                   │
│ 每个 NAND Page = 128 entries × 16B = 2048B       │
│ 写满一页才提交 NAND（批量化）                      │
│ Journal 满后配合 Checkpoint 截断回收               │
└──────────────────────────────────────────────────┘
```

### 7.3 写入流程（完整序列）

```c
int ftl_write(ftl_context_t *ftl, ssd_lba_t lba, const void *data) {
    // 1. Allocator 分配新物理地址
    new_paa = allocator_alloc(&ftl->alloc);

    // 2. 读旧的 PAA（cache 查找 → 可能触发 cache miss）
    old_paa = l2p_cache_read(&ftl->cache, lba);

    // 3. ★ 通过 FC 写数据到 NAND
    cmd = fc_command_alloc();
    cmd->config = { NORMAL_PROGRAM_PAGE, CRC_ENABLE, RANDOM_ENABLE, BCH, ... };
    cmd->paa = new_paa;
    cmd->faa = lba;
    fc_command_configure_descriptors(cmd);
    fc_write_cmd_data(cmd, data, FC_BUFFER_USER_SIZE);
    fc_submit(cmd);
    fc_poll_completion(&completion);
    // 检查 completion.status → 如果 ECC 失败，走恢复路径

    // 4. ★ 写 Journal（NAND 确认后才安全）
    journal_append(&ftl->journal, lba, old_paa, new_paa);

    // 5. 更新 L2P Cache（标记 dirty）
    l2p_cache_write(&ftl->cache, lba, new_paa);

    fc_command_release(cmd);
    return SSD_STATUS_OK;
}
```

### 7.4 启动恢复

```
冷启动（无 journal）:
  l2p_cache_init() → cache 全 INVALID
  首次访问时从 NAND 加载 region

热启动（有 journal）:
  ① 扫描 Checkpoint 区域 → 加载最新 Checkpoint
  ② 回放 Journal（sequence > checkpoint 的所有条目）
  ③ Cache 中所有 line 标记 DIRTY
  ④ 后台 flush dirty regions 到 NAND L2P 表
```

### 7.5 Journal 回收

- Journal 填满一块后，配合 Checkpoint 截断
- Checkpoint 时记下全局 sequence 号
- 回收 sequence ≤ checkpoint_seq 的 journal 条目
- 擦除 journal block 复用

---

## 8. Checkpoint 机制

### 8.1 为什么需要 Checkpoint

Journal 恢复的缺点：如果 journal 很长，启动回放时间长。Checkpoint 定期保存 L2P 快照，截断 journal。

### 8.2 设计

```c
typedef struct {
    uint32_t magic;                 // 0xCCCCCCCC
    uint32_t sequence;              // 单调递增（最新有效）
    uint32_t timestamp;             // 创建时间
    uint32_t region_count;          // 本 checkpoint 包含的 region 数
    uint32_t region_bitmap[64];     // 2048 bit，哪些 region 在 checkpoint 中
    uint32_t journal_sequence;      // 截断到此 sequence 之前的 journal
    uint32_t crc;                   // 校验和
} l2p_checkpoint_header_t;
```

**全量 Checkpoint**：flush 所有 dirty cache → 写 header → 截断 journal
**增量 Checkpoint**：只 flush 变化的 region → 写 header（含 region bitmap）→ 截断 journal

### 8.3 启动恢复完整流程

```
冷启动恢复：
  ┌─────────────────────────────────────────────────────┐
  │ ① 扫描 Checkpoint 区域 → 最新 header（seq 最大）    │
  │ ② 加载 Checkpoint 中的 L2P regions                  │
  │ ③ 回放 Journal（seq > cp_seq 的条目）               │
  │ ④ Cache 标记 DIRTY → 后台 flush                     │
  │ ⑤ Controller → Ready                                │
  └─────────────────────────────────────────────────────┘
```

---

## 9. 架构决策记录

### ADR-1：选择纯页映射而非块映射/混合映射

| 项目 | 决策 |
|------|------|
| 选型 | **纯页映射（CW 级 512B）** |
| 理由 | 写放大最小（1.0~1.3x）、行业标准、与 FC 对齐 |
| 拒绝方案 | 块映射（WA 5~10x，QLC/TLC 寿命不够）、混合映射（2012 年前技术，已被淘汰）|
| 代价 | 表最大（4MB），需 SRAM cache + NAND 存储 |

### ADR-2：选择 DRAM-less L2P Cache

| 项目 | 决策 |
|------|------|
| 选型 | **NAND 存全表 + SRAM region cache** |
| 理由 | 学习行业主流嵌入式/UFS/eMMC 方案；有 1MB SRAM 可支持 64~128 regions |
| 拒绝方案 | 全表放 SDRAM（不是真正的 DRAM-less，降低学习价值）、page 级映射全表塞 SRAM（粒度粗，WA 大）|
| 代价 | 全盘随机访问时性能下降（命中率 1.6~6.3%）|

### ADR-3：RAID 不实现

| 项目 | 决策 |
|------|------|
| 选型 | **不实现 RAID（CONFIG_FC_RAID = 0）** |
| 理由 | RAID 增加 GC 复杂度、parity block 管理复杂、SLC 的 BER 较低不需要 |
| 未来 | 如需数据保护增强，优先考虑加强 ECC |

### ADR-4：FTL 通过 FC 接口访问 NAND

| 项目 | 决策 |
|------|------|
| 选型 | **FTL → FC 接口 → NAND** |
| 理由 | 经过 CRC/ECC/Random 完整数据保护；与 FC 异步模型集成 |
| 代价 | 每次读写需 alloc/submit/poll/release 序列，代码量增加 |

### ADR-5：CLOCK 替换策略

| 项目 | 决策 |
|------|------|
| 选型 | **CLOCK 算法** |
| 理由 | 命中率接近 LRU，无需链表/内存分配，O(1) amortized |
| 拒绝方案 | LRU（需链表，嵌入式麻烦）、FIFO（命中率低）|

---

## 10. 企业级实现对比

### 10.1 Block 管理对比

| 维度 | 本方案 | 消费级（Phison/SMI） | 企业级（Samsung/Solidigm） |
|------|--------|---------------------|--------------------------|
| 数据块分区 | 单一 | 2~4 温区 | 热/冷/普通 + SLC cache |
| 空闲池 | 单池 | 多层池（不同 P/E） | Pre-conditioned + 紧急池 |
| GC 触发 | free count < 10% | 多级阈值 | 后台预测性 GC |
| Over-provision | 无单独预留 | 7~15% | 7~28% |
| 写缓冲 | 无 | SLC mode buffer | SLC / pseudo-SLC cache |

### 10.2 L2P 管理对比

| 维度 | 本方案 | 消费级（DRAM-less） | 企业级（有 DRAM） |
|------|--------|--------------------|-------------------|
| 表位置 | NAND | NAND | DRAM 全表 |
| SRAM cache | 64~128 regions | 32~256 regions | N/A |
| 替换策略 | CLOCK | CLOCK / 2Q | N/A（全表常驻）|
| 写策略 | Write-back + J | Write-back + J | 直写 DRAM |
| 启动时间 | ~秒级（checkpoint） | ~秒级 | 毫秒级 |

### 10.3 企业级的"多流写入"

本方案当前只有单 Open Block。企业级按数据温度分多 Open Block：

```
本方案（单 Open Block）:
  Open Block: [A(热)][B(冷)][C(热)][D(GC)] ← 冷热混写
  GC 时整 Block 搬 → 无效页率不集中 → GC 效率差

企业级（多温区）:
  Hot Open Block:  [A][C][E][G]...   ← 频繁更新
  Cold Open Block: [B][D][F][H]...   ← 几乎不变
  GC Hot: 无效页率高 → 很快回收 ✅
  GC Cold: 无效页率低 → 很少需回收 ✅
```

扩展方案：

```c
#define FTL_TEMP_HOT  0
#define FTL_TEMP_COLD 1
#define FTL_TEMP_NUM  2

typedef struct {
    uint16_t open_block[PLANE_NUM][FTL_TEMP_NUM];  // 每 Plane 2 个 open block
    uint8_t  next_page[PLANE_NUM][FTL_TEMP_NUM];
} ftl_allocator_t;
```

---

## 11. FTL 对外接口

### 11.1 FTL 主上下文

```c
typedef struct ftl_context {
    /* FC 相关 */
    fc_command_pool_t   cmd_pool;          // FC command pool
    fc_buffer_pool_t    buf_pool;          // FC buffer pool

    /* L2P Cache */
    l2p_cache_t         cache;             // SRAM L2P cache（64~128 KB）

    /* Allocator */
    ftl_allocator_t     alloc;             // 空闲 block / 写指针

    /* Journal */
    l2p_journal_t       journal;           // log-structured journal

    /* Checkpoint */
    uint32_t            cp_sequence;       // 当前 checkpoint 序号
    uint32_t            cp_interval;       // 写多少次触发一次 checkpoint

    /* 统计 */
    uint64_t            total_writes;
    uint32_t            cache_hits, cache_misses;
} ftl_context_t;
```

### 11.2 FTL API

```c
/* ── 初始化 ── */
int ftl_init(ftl_context_t *ftl);
// 初始化 L2P cache → allocator → journal → checkpoint 区域

/* ── 读写（用户数据）── */
int ftl_read(ftl_context_t *ftl, ssd_lba_t lba, void *data, uint32_t count);
// L2P cache lookup → FC read → memcpy to data buffer
// cache miss 时自动从 NAND 加载 region

int ftl_write(ftl_context_t *ftl, ssd_lba_t lba, const void *data, uint32_t count);
// allocator alloc → FC program → journal append → cache write (dirty)

int ftl_trim(ftl_context_t *ftl, ssd_lba_t lba, uint32_t count);
// L2P entry → FTL_PPA_INVALID, 标记 bitmap 无效

/* ── 主循环任务 ── */
void ftl_service(ftl_context_t *ftl);
// 每次调用执行：
//   1. fc_service()             → 推进 FC 调度
//   2. fc_poll_completion()     → 处理完成回调
//   3. l2p_cache_flush_bg()     → 后台写回 dirty region
//   4. gc_check()               → 检查是否需要 GC
//   5. cp_check()               → 检查是否需要 checkpoint

/* ── 后台任务（可选在 ftl_service 外单独调用）── */
void ftl_background_flush(ftl_context_t *ftl);
void ftl_checkpoint(ftl_context_t *ftl);
void ftl_gc_execute(ftl_context_t *ftl);
```

### 11.3 FTL 内部模块 API

**L2P Cache**（`l2p_cache.h`）：

```c
void       l2p_cache_init(l2p_cache_t *cache);
nand_paa_t l2p_cache_read(l2p_cache_t *cache, ssd_lba_t lba);
void       l2p_cache_write(l2p_cache_t *cache, ssd_lba_t lba, nand_paa_t pa);
void       l2p_cache_flush_one(l2p_cache_t *cache, int idx);
void       l2p_cache_flush_all(l2p_cache_t *cache);
void       l2p_cache_flush_background(l2p_cache_t *cache, int max_count);
```

**Journal**（`journal.h`）：

```c
void journal_init(l2p_journal_t *journal);
void journal_append(l2p_journal_t *journal, ssd_lba_t lba, nand_paa_t old, nand_paa_t new);
int  journal_replay(l2p_journal_t *journal, l2p_cache_t *cache);
void journal_truncate(l2p_journal_t *journal, uint32_t seq);
```

**Allocator**（`allocator.h`）：

```c
int       allocator_init(ftl_allocator_t *alloc);
nand_paa_t allocator_alloc(ftl_allocator_t *alloc);
void      allocator_free_block(ftl_allocator_t *alloc, uint16_t block);
void      allocator_mark_bad(ftl_allocator_t *alloc, uint16_t block);
int       allocator_check_gc(const ftl_allocator_t *alloc);
```

### 11.4 性能估算

| 场景 | cache 命中率 | 平均读延迟 | 说明 |
|------|-------------|-----------|------|
| 64 reg, 全随机 | ~3.1% | ~50 μs | 最差情况 |
| 64 reg, 80/20 | ~60~70% | ~30 μs | 典型工作负载 |
| 128 reg, 80/20 | ~75~85% | ~27.6 μs | 接近理想（25 μs tR）|

Cache miss 代价 ≈ 26 μs（region 加载：一次 NAND read）
Write-back flush 代价 ≈ 200~300 μs（一次 NAND program）

---

## 12. 与 FC 层接口约定

### 12.1 FTL 使用 FC 的约束表

```
┌──────────────────────────────────────────────────────────────┐
│  FTL 通过 FC 访问 NAND 的接口约束                            │
├─────────────────────────────────────┬────────────────────────┤
│           用户数据读写               │ L2P / Journal 元数据   │
├─────────────────────────────────────┼────────────────────────┤
│  operation = NORMAL_PROGRAM_PAGE    │ operation = 同上       │
│             /NORMAL_READ_PAGE       │                       │
│  CRC    = ENABLE (数据保护)         │ CRC    = DISABLE      │
│  Random = ENABLE (加扰)             │ Random = DISABLE      │
│  LDPC   = BCH                      │ LDPC   = BCH          │
│  module = FTL                      │ module = METADATA     │
│  faa    = lba (写入 META)           │ faa    = 不填          │
│  page_type = LP (SLC)              │ page_type = LP        │
└─────────────────────────────────────┴────────────────────────┘
```

### 12.2 FTL 写数据的标准序列

```c
nand_paa_t ftl_write_data(ssd_lba_t lba, const uint8_t *data, uint32_t len) {
    fc_command_t *cmd = fc_command_alloc();

    cmd->config.operation    = OP_NORMAL_PROGRAM_PAGE;
    cmd->config.cw           = 3;                 // 4 CW per page
    cmd->config.crc          = FC_CRC_ENABLE;
    cmd->config.random       = FC_RANDOM_ENABLE;
    cmd->config.ldpc         = FC_ECC_MODE_BCH;
    cmd->config.page_type    = FC_PAGE_TYPE_LP;
    cmd->config.module       = FC_COMMAND_MODULE_FTL;
    cmd->paa                 = new_paa;           // allocator 分配
    cmd->faa                 = lba;               // 写入 META

    fc_command_configure_descriptors(cmd);
    fc_write_cmd_data(cmd, data, len);

    fc_submit(cmd);                               // → SQ
    // ftl_service() 中 fc_poll_completion 会拿到完成状态
    // 检查 completion.status0~3, ecc_bit0~3
    // 成功后 journal_append + l2p_cache_write

    fc_command_release(cmd);
}
```

### 12.3 FTL 应当避免的操作

```
❌ 直接调用 nand_read_page() / nand_program_page()
❌ 直接操作 NAND 引擎状态机
❌ 直接读写 NAND 寄存器（SET/GET_FEATURES 通过 FC 完成）
❌ 绕过 fc_command_configure_descriptors 手动分配 desc

✅ 通过 CQ + completion 获取 per-CW 状态
✅ 通过 fc_data_path_raid_recover() 触发 RAID 恢复（未来）
✅ 遍历 desc 链获取每 CW 的详细错误信息
```

---

## 13. 未解决问题

| # | 问题 | 影响 | 当前选项 |
|---|------|------|---------|
| 1 | Region 物理存储：固定 vs 动态元数据 block | 固定简单但磨损集中 | 固定 32 blocks（SLC 100k P/E 足够） |
| 2 | Journal 可靠性：journal 本身怎么防掉电 | journal page program 到一半掉电 | 双页 + CRC 校验 |
| 3 | 启动加载策略：加载 checkpoint 全表还是按需 miss | 启动延迟 vs 首次访问延迟 | 干净关机 → 按需 miss；异常掉电 → checkpoint |
| 4 | Journal 回收时机 | 需要确保 L2P 表已同步 | 配合 checkpoint 同步截断 |
| 5 | L2P 表磨损：元数据 block 擦除是否超过数据 block | 寿命不均衡 | region 更新合并 + checkpoint 批量写 |
| 6 | 多 Plane 下 region 分布：是否跨 plane 并行写入 | L2P 写回性能 | 可优化为 Multi-Plane PROGRAM |

---

## 14. Phase 规划

### Phase 0：基础设施（核心 cache + FC 适配）

| 任务 | 说明 | 估算 |
|------|------|------|
| P0.1 | L2P cache 数据结构 + CLOCK 替换 | 小 |
| P0.2 | Cache lookup/miss/evict（通过 FC 读 NAND） | 中 |
| P0.3 | Cache update/dirty 标记 | 小 |
| P0.4 | Cache flush（通过 FC 写 NAND） | 中 |
| P0.5 | FTL 读写改用 FC 接口 | 中 |

### Phase 1：掉电恢复

| 任务 | 说明 | 估算 |
|------|------|------|
| P1.1 | Journal 格式 + NAND 写入 | 中 |
| P1.2 | Write-back + journal 集成 ftl_write | 中 |
| P1.3 | 启动时 journal 回放 | 中 |
| P1.4 | Checkpoint 全量/增量 | 小 |
| P1.5 | 完整启动恢复流程 | 中 |

### Phase 2：完整 FTL

| 任务 | 说明 | 估算 |
|------|------|------|
| P2.1 | Allocator（free block + 顺序写指针） | 中 |
| P2.2 | Bad block 扫描 + BBT | 中 |
| P2.3 | 反向映射 bitmap | 小 |
| P2.4 | GC 贪心 victim + 同步搬移 | 大 |
| P2.5 | Background flush 集成 ftl_service | 小 |
| P2.6 | Thrashing 检测 + 降级 | 中 |

### Phase 3：优化

| 任务 | 说明 | 估算 |
|------|------|------|
| P3.1 | Prefetch 顺序读预加载 | 中 |
| P3.2 | GC L2P 批量更新优化 | 中 |
| P3.3 | Warm boot 保留 cache | 小 |
| P3.4 | 性能计数器 + 调试接口 | 小 |

---

## 15. 附录：核心数据结构原型

### L2P Cache 接口

```c
// l2p_cache.h — 完整接口

#define L2P_ENTRIES_PER_REGION  (512)
#define L2P_REGION_SIZE         (2048)      // 512 × 4B
#define L2P_CACHE_SIZE          (64)        // 可调: 32/64/128
#define L2P_REGIONS_TOTAL       (2048)

typedef struct {
    nand_paa_t entries[L2P_ENTRIES_PER_REGION];  // 512 PAA = 2KB
    uint16_t   region_id;
    uint8_t    state : 2;      // INVALID=0, CLEAN=1, DIRTY=2, FLUSHING=3
    uint16_t   access_count : 14;
} l2p_cache_line_t;

typedef struct {
    l2p_cache_line_t lines[L2P_CACHE_SIZE];
    uint16_t         clock_hand;
    uint32_t         hit_count, miss_count;
} l2p_cache_t;

void       l2p_cache_init(l2p_cache_t *cache);
nand_paa_t l2p_cache_read(l2p_cache_t *cache, ssd_lba_t lba);
void       l2p_cache_write(l2p_cache_t *cache, ssd_lba_t lba, nand_paa_t pa);
void       l2p_cache_flush_one(l2p_cache_t *cache, int idx);
void       l2p_cache_flush_all(l2p_cache_t *cache);
```

### Journal 接口

```c
// journal.h — 完整接口

#define JOURNAL_BLOCKS           (2)
#define JOURNAL_ENTRIES_PER_PAGE  (128)

typedef struct {
    uint32_t    lba;
    nand_paa_t  old_paa;
    nand_paa_t  new_paa;
    uint32_t    sequence;
    uint32_t    crc;
} __attribute__((packed)) l2p_journal_entry_t;

typedef struct {
    uint32_t current_block;
    uint32_t current_page;
    uint32_t current_entry;
    uint32_t sequence_counter;
} l2p_journal_t;

void journal_init(l2p_journal_t *journal);
void journal_append(l2p_journal_t *journal, ssd_lba_t lba, nand_paa_t old, nand_paa_t new);
int  journal_replay(l2p_journal_t *journal, l2p_cache_t *cache);
void journal_truncate(l2p_journal_t *journal, uint32_t seq);
void journal_flush_page(l2p_journal_t *journal);
```

### Allocator 接口

```c
// allocator.h — 完整接口

typedef struct {
    uint16_t  open_block[PLANE_NUM];      // 每 Plane 当前写入 Block
    uint8_t   next_page[PLANE_NUM];       // 每 Plane 当前写入页
    uint16_t *free_blocks;                // 空闲 Block 列表
    uint16_t  free_count;                 // 空闲 Block 数
    uint16_t *bad_blocks;                 // 坏 Block 列表
    uint16_t  bad_count;                  // 坏 Block 数
    uint32_t *erase_counts;               // 每 Block 擦除次数
    uint16_t  total_blocks;               // 总好 Block 数
} ftl_allocator_t;

int        allocator_init(ftl_allocator_t *alloc);
nand_paa_t allocator_alloc(ftl_allocator_t *alloc);
void       allocator_free_block(ftl_allocator_t *alloc, uint16_t block_no);
void       allocator_mark_bad(ftl_allocator_t *alloc, uint16_t block_no);
int        allocator_needs_gc(const ftl_allocator_t *alloc);
```

### FTL 读写接口

```c
// ftl.h — FTL 对外 API

typedef struct ftl_context {
    l2p_cache_t       cache;
    ftl_allocator_t   alloc;
    l2p_journal_t     journal;
    uint32_t          cp_sequence;
    uint32_t          cp_interval;
    uint64_t          total_writes;
    uint32_t          cache_hits, cache_misses;
} ftl_context_t;

int  ftl_init(ftl_context_t *ftl);
int  ftl_read(ftl_context_t *ftl, ssd_lba_t lba, void *data, uint32_t count);
int  ftl_write(ftl_context_t *ftl, ssd_lba_t lba, const void *data, uint32_t count);
int  ftl_trim(ftl_context_t *ftl, ssd_lba_t lba, uint32_t count);
void ftl_service(ftl_context_t *ftl);
void ftl_background_flush(ftl_context_t *ftl);
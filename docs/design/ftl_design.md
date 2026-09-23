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
| v0.4 | 2026-09-23 | Write-back 四级触发条件 + 企业级 flush 对比 + 6 个补充设计要点 | Claude |
| v0.5 | 2026-09-23 | GC 多维度加权评分 + 六级触发 + 原子性保证（Journal/GC Bitmap）+ 企业级参考；Block 分类重写（拓扑/Super Block/温度/OP/Pre-conditioned）；Phase 规划扩展（P2.8~P2.11）；Allocator 接口增加温度参数 | Claude |
| v0.6 | 2026-09-23 | ftl_service 主循环调度设计：三级模式（IDLE/NORMAL/BUSY）+ 企业级后台调度器参考 + 主循环完整形态 | Claude |
| v0.7 | 2026-09-23 | Journal 自身可靠性设计（四层保护：ECC/CRC/entry_count/sequence）+ Journal Page 完整格式；BBT 完整设计（出厂扫描 + 运行时隔离 + Block Skip 替换 + 企业级参考） | Claude |
| v0.8 | 2026-09-24 | 设计总结章节（§15）：设计定位/决策总览/硬件依赖审查/企业消费级一致性/Phase 实施顺序；ADR 扩展至 11 项（ADR-6~11）；§10 企业对比表更新；§13 小节编号修复；§1.4 决策表扩展；§5.3 Region 地址修正为 ch/ce/lun 参数 | Claude |

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
12. [ftl_service 主循环调度](#12-ftl_service-主循环调度)
13. [与 FC 层接口约定](#13-与-fc-层接口约定)
14. [未解决问题](#14-未解决问题)
15. [设计总结](#15-设计总结)
16. [Phase 规划](#16-phase-规划)
17. [附录：核心数据结构原型](#17-附录核心数据结构原型)

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
| Super Block | **跨双 Plane** | 学习企业级跨 Die 条带化，当前双 Plane 配对 |
| 温度分区 | **Hot / Cold 双 Open Super Block** | 提升 GC 效率，冷热分离 |
| OP | **10% 预留（可配置 7~28%）** | RMA 坏块替换 + 性能保速 |
| Free Pool | **三级（Pre-conditioned/Normal/Emergency）** | 学习 Background GC 和前台 IO 隔离 |
| SLC Cache | **不实现** | 本身就是 SLC NAND |

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

### 3.1 拓扑结构

整个 FTL 设计以 `[ch][ce][lun]` 为拓扑维度，不假设任何维度为 1。

```c
/* nand_config.h 中定义的物理拓扑 */
#define CH_NUM      (1)     // 通道数
#define CE_NUM      (1)     // 每通道片选数
#define LUN_NUM     (1)     // 每片选 LUN 数
#define PLANE_NUM   (2)     // 每 LUN Plane 数
```

所有全局数据结构按此维度声明：

```c
ftl_lun_allocator_t g_allocator[CH_NUM][CE_NUM][LUN_NUM];

for (ch = 0; ch < CH_NUM; ch++)
    for (ce = 0; ce < CE_NUM; ce++)
        for (lun = 0; lun < LUN_NUM; lun++)
            process_lun(ch, ce, lun);
```

### 3.2 每 LUN Block 分类

```
每 LUN 的 NAND 物理空间（当前 2 Plane × 2048 Block = 4096 Block / LUN）

┌──────────────────────────────────────────────────────────────┐
│  Metadata Blocks（元数据区）  每 LUN 独立一份                │
│  ├── L2P Table (32 blocks)    完整 L2P 映射表                │
│  ├── Checkpoint (2 blocks)    L2P 快照                      │
│  ├── Journal (2 blocks)       掉电恢复日志                   │
│  ├── BBT (1 block)            坏块表                         │
│  ├── Erase Count (1 block)    擦除计数表                     │
│  └── Reserved (2+ blocks)     替换预留                      │
│                                                              │
│  User Data Area（数据区）      跨 Plane Super Block 管理    │
│  ├── OP Blocks (10%)          预留空间（不映射给用户）       │
│  ├── Open Super Block         当前写入位置（跨双 Plane）     │
│  │     ├── Plane 0: Block N                                │
│  │     └── Plane 1: Block M                                │
│  ├── Closed Super Blocks      已写满，有效+无效页混合        │
│  ├── Pre-conditioned Pool     预擦除池（BG GC 持续补充）     │
│  └── Free Blocks              已擦除待分配                   │
│                                                              │
│  Bad Blocks（坏块）            BBT 标记                      │
└──────────────────────────────────────────────────────────────┘
```

### 3.3 元数据区（Metadata Blocks）

每 LUN 固定位置，不由 allocator 管理，出厂时必须是好块：

```
┌──────────────┬──────┬────────────────────────────────┐
│ 类型          │ 数量  │ 内容                            │
├──────────────┼──────┼────────────────────────────────┤
│ L2P 表        │ ~32  │ L2P mapping regions（本 LUN 的）│
│ Checkpoint   │  2   │ L2P 快照 + header               │
│ Journal      │  2   │ 元数据变更日志                   │
│ BBT          │  1   │ 本 LUN 坏块表                   │
│ Erase Count  │  1   │ 本 LUN 擦除计数表                │
│ Reserved     │  2   │ 坏块替换备用                    │
├──────────────┼──────┼────────────────────────────────┤
│ 小计          │ ~40  │ 每 LUN 约 1% 容量               │
└──────────────┴──────┴────────────────────────────────┘
```

多 LUN 时元数据各 LUN 独立。L2P region 地址计算含 ch/ce/lun 参数：

```c
static nand_paa_t l2p_region_ppa(uint8_t ch, uint8_t ce, uint8_t lun,
                                  uint16_t region_id)
{
    uint16_t block_offset = region_id / PAGES_PER_BLOCK;
    uint16_t page_in_block = region_id % PAGES_PER_BLOCK;
    return paa_make(page_in_block, 0, L2P_BASE_BLOCK + block_offset, lun, 0);
}
```

### 3.4 Super Block（跨 Plane 条带化）

Super Block 是跨双 Plane 的逻辑写入单元。当前硬件 PLANE_NUM=2，双 Plane 各取一个物理 Block 配对。

```
Open Super Block:
  Plane 0: Block A（由 allocator 分配）
  Plane 1: Block B（由 allocator 分配，双 Plane 同步写入）

写入以 Super Block 为粒度，双 Plane 并行（Multi-Plane PROGRAM）：
  Page 0: Plane0-Page0 + Plane1-Page0
  ...
  Page 63: Plane0-Page63 + Plane1-Page63 → Super Block 写满
```

```c
typedef struct {
    uint16_t plane_blocks[PLANE_NUM];   // Plane 0 和 Plane 1 的 Block 号
    uint8_t  next_page;                  // 当前写入页（0~63，双 Plane 同步）
} super_block_t;

nand_paa_t super_block_page_paa(const super_block_t *sb, uint8_t plane, uint8_t page) {
    return paa_make(page, plane, sb->plane_blocks[plane], 0, 0);
}
```

### 3.5 温度分区（Multi-Stream）

为提升 GC 效率，数据按更新频率分温度写入不同的 Open Super Block。

```
Hot Super Block:   ← 频繁写入的数据（用户 IO 热数据）
  数据特征：更新频繁，无效页比例增长快
  GC 效率：高（80~90% 无效页）

Cold Super Block:  ← 几乎不更新的数据（GC 搬移、元数据）
  数据特征：几乎不变，无效页比例极低
  GC 效率：低（0~5% 无效页），但几乎不用回收
```

温度归类策略：

| 数据来源 | 温度 | 理由 |
|---------|------|------|
| 用户写入（Host IO） | HOT | 可能频繁更新 |
| GC 搬移的有效页 | COLD | 已至少存活一轮 GC |
| GC 搬移的热数据 | HOT | 仍可能频繁更新 |
| L2P/Journal/元数据 | COLD | 几乎不更新 |

### 3.6 OP 区域（Over-Provisioning 10%）

OP 是数据区中**不映射给用户**的好块，由 allocator 统一管理但 L2P 不覆盖。

```c
#define FTL_OP_PERCENT    (10)    // 10% 预留空间，可调 7~28，设为 0 禁用
```

**OP 在 RMA 和性能恢复中的作用**：

```
RMA 场景：
  出厂时：总好块 4056，用户可见 3611 块（OP=10% 约 405 块）
  使用中：新增 50 个坏块 → 从 OP 池替换 → 用户容量不变 ✅
  没有 OP：坏块替换 → 用户容量减少 → 设备"变小" ❌

性能恢复场景：
  写入压力大时：OP 池提供 400+ Block GC 缓冲
  GC 有充足空间后台搬移 → 前台写入速度稳定 ✅
  没有 OP：free pool 耗尽 → 前台 IO 等 GC → 写入断崖下降 ❌
```

### 3.7 Pre-conditioned Pool（预擦除池 — 三级 Free Pool）

```
┌──────────────────────────────────────────────────────────────┐
│  Free Pool 三级结构                                            │
│                                                              │
│  Pre-conditioned Pool（预擦除池）                              │
│  ├── 已擦除好，前台 IO 零等待分配                              │
│  ├── 由 Background GC 持续补充                                 │
│  └── 目标水位：>= FREE_POOL_TARGET                            │
│                                                              │
│  Free Pool（普通池）                                          │
│  ├── 已擦除待分配                                             │
│  └── Pre-conditioned 耗尽时从此取                              │
│                                                              │
│  Emergency Pool（紧急池）                                     │
│  ├── 最后防线                                                │
│  ├── 仅当 free pool 耗尽时启用                                │
│  └── 启用瞬间触发强制 GC                                      │
└──────────────────────────────────────────────────────────────┘
```

```c
#define FREE_POOL_TARGET      (32)     // Pre-conditioned 目标水位
#define FREE_POOL_EMERGENCY   (8)      // 紧急水位

void ftl_refill_pre_cond(ftl_context_t *ftl) {
    if (ftl->free_pool.pre_cond_count < FREE_POOL_TARGET) {
        if (ftl->free_pool.free_count > FREE_POOL_EMERGENCY) {
            ftl_gc_background(ftl);  // 非阻塞后台 GC
        }
    }
}
```

### 3.8 坏块管理（BBT）

坏块管理分三个阶段：出厂扫描 → 运行时隔离 → 替换机制。

#### 3.8.1 出厂坏块扫描

```
NAND 出厂时，每个 Block 的前 2 页（Page 0~1）的 OOB 区域
标记了该 Block 是否为出厂坏块。

OOB 标记规则（JEDEC/ONFI 标准）：
  非坏块：OOB[0]（Block 0）或 OOB[0..1]（其余 Block）= 0xFF ✅
  出厂坏块：OOB[0] ≠ 0xFF ❌
```

**扫描流程**：

```c
int bbt_scan_factory(ftl_allocator_t *alloc, uint8_t ch, uint8_t ce, uint8_t lun) {
    uint16_t bad_count = 0;

    /* 遍历所有 Block */
    for (uint16_t block = 0; block < BLOCKS_PER_PLANE * PLANE_NUM; block++) {
        for (uint32_t plane = 0; plane < PLANE_NUM; plane++) {
            nand_paa_t check_page = paa_make(0, plane, block, lun, 0);  // Page 0

            /* 通过 FC 读该页的 OOB（Spare 区前 2 字节）*/
            uint8_t oob_mark[2];
            fc_read_oob(check_page, oob_mark, 2);

            if (oob_mark[0] != 0xFF) {
                alloc->bad_blocks[bad_count++] = block;
                break;  // 任一 Plane 坏即整 Block 坏
            }
        }
    }

    alloc->bad_count = bad_count;

    /* 排除元数据区中的坏块 */
    for (int i = 0; i < alloc->bad_count; i++) {
        if (alloc->bad_blocks[i] < METADATA_BLOCK_COUNT) {
            /* 元数据 Block 坏块 → 从 Reserved 替换 */
            bbt_replace_metadata_block(alloc->bad_blocks[i]);
        }
    }

    return bad_count;
}
```

**扫描性能估算**：

```
每个 Block 读 2 页（Page 0 + 1），每次 ~25μs
总 Block ≈ 4096
总时间 ≈ 4096 × 2 × 25μs ≈ 205ms
加上 OOB 数据传输（~10μs/页）≈ 40ms
合计 ≈ 250ms
→ 在启动过程中可接受（放在 firmware_boot_stage_storage）
```

**坏块替换策略**：

```
元数据区坏块（Block 0~39）：
  → 从 Reserved Block（38~39）替换
  → 若 Reserved 用完 → 从 OP 池取一块替换
  → 若 OP 池无剩余 → DEVICE_FAILED

数据区坏块：
  → 加入 bad_blocks 列表，allocator 跳过即可
  → 不替换（数据区坏块不损失容量——OP 池就是为此存在的）
```

**坏块表存储格式**：

```c
#define BBT_MAGIC       0x424254AB      // "BBT\xAB"
#define BBT_MAX_BLOCKS  (BLOCKS_PER_PLANE * PLANE_NUM)  // 4096

typedef struct {
    uint32_t magic;                 // 魔数验证
    uint32_t sequence;              // 单调递增（最新有效）
    uint32_t total_blocks;          // 总 Block 数
    uint32_t bad_count;             // 坏块数
    uint32_t reserved[3];           // 保留
    uint8_t  bad_map[BBT_MAX_BLOCKS / 8];  // 4096 bit = 512B
    uint32_t crc;                   // 校验和
} __attribute__((packed)) bbt_header_t;
```

- 在 NAND 元数据区 Block 36 存储
- 占用 1 page（2048B），实际 ~520B header + bitmap，剩余 padding
- 冗余存 3 份（同一 Block 的 Page 0/1/2），防 BBT 自身损坏

#### 3.8.2 运行时坏块隔离

运行时坏块的检测时机：

```
检测时机       触发条件                     处理方式
──────────    ──────────────────────────   ─────────────────
PROGRAM 完成    FC_CW_STATUS_NAND_FAIL      标记坏块 → 数据搬移
ERASE 完成      FC_CW_STATUS_NAND_FAIL      标记坏块
READ 重试超限   ECC 连续多次升高 + retry 失败  标记坏块 → 数据搬移
```

**隔离流程**：

```c
/* FC 完成后检查坏块 */
void ftl_check_bad_block(ftl_context_t *ftl, fc_command_status_t *completion) {
    /* 检查是否有 NAND_FAIL 状态 */
    if ((completion->status0 & FC_CW_STATUS_NAND_FAIL) == 0 &&
        (completion->status1 & FC_CW_STATUS_NAND_FAIL) == 0 &&
        (completion->status2 & FC_CW_STATUS_NAND_FAIL) == 0 &&
        (completion->status3 & FC_CW_STATUS_NAND_FAIL) == 0) {
        return;  // 无失败
    }

    /* 定位失败的 Block */
    fc_command_t *cmd = fc_command_id_to_command(completion->command_id);
    uint16_t fail_block = paa_to_block(cmd->paa);
    uint8_t  fail_plane = paa_to_plane(cmd->paa);

    /* 隔离坏块 */
    allocator_mark_bad(&ftl->alloc, fail_block);

    /* 更新 BBT（写 NAND） */
    bbt_update(&ftl->bbt, fail_block, 1);  // 1 = bad

    /* 如果是 PROGRAM 失败：尝试在其他位置重写 */
    if (cmd->config.operation == OP_NORMAL_PROGRAM_PAGE) {
        ftl_rewrite_failed_data(ftl, cmd);
    }

    /* 如果是 ERASE 失败：无需搬移，直接标记 */
}
```

#### 3.8.3 替换机制

```
坏块替换策略：Block Skip（跳过）

  allocator_alloc() 中跳过坏块：

    nand_paa_t allocator_alloc(ftl_allocator_t *alloc, uint8_t temperature) {
        uint16_t candidate;

        do {
            /* 从 free pool 取一个 Block */
            candidate = alloc->free_blocks[--alloc->free_count];

            /* 跳过坏块 */
        } while (bbt_is_bad(&alloc->bbt, candidate));  

        /* 检查是否 OP Block */
        if (alloc->op_count > 0 && alloc->op_used < alloc->op_count) {
            /* 还有 OP 余额 → 从 OP 池分配 */
            return bbt_alloc_from_op(alloc, candidate);
        }

        return candidate;
    }
```

**为什么用 Block Skip 而不是 Block Sparing**：

```
Block Skip（本方案）：                              Block Sparing（企业级）：
  alloc 时跳过坏块 → 容量自然减少                    预留替换块 → 坏块替换 → 容量不变

  你的场景（512MiB SLC）：                            企业级场景（8TB QLC）：
  出厂坏块 ≈ 2~10 个（SLC 优秀良率）                    出厂坏块 ≈ 数千个（QLC 良率差）
  跳过 ~10 个对 512MiB 无感                             需要 sparing 弥补大容量损失

结论：你的方案用 Block Skip 足够了。
  OP 池（10%）自然吸收了坏块替换的容量损失。
  即使新增 50 个坏块，OP 池也能覆盖。
```

#### 3.8.4 企业级 BBT 参考

| 维度 | 本方案 | 消费级 | 企业级 |
|------|--------|--------|--------|
| 出厂扫描 | 全量扫描 4096 Block | 全量扫描 | 增量扫描（只查未查过的）|
| BBT 存储 | NAND 固定 Block ×3 冗余 | NAND ×3 冗余 | NAND + SRAM cache + RAID1 |
| 运行时检测 | PROGRAM/ERASE 完成后 | 同左 | 同左 + ECC 趋势预测 |
| 替换策略 | Block Skip | Block Skip | Stripe Skip + Sparing |
| 新增坏块 | alloc 跳过 | 同左 | 动态映射表更新 |
| 坏块预测 | 无 | 无 | 根据 ECC 上升趋势预测即将坏的 Block |

**企业级的坏块预测（Predictive Failures）**：

```
企业级 SSD 不等到 PROGRAM 失败才标记坏块——太晚了：

  监控每个 Block 的 ECC 纠正比特数趋势
  ECC 连续 N 次上升 → "预测"为即将坏块
  → 主动搬移该 Block 的数据
  → 标记为"慢死"（Slow Death）Block
  → 不再分配新数据

好处：
  在数据彻底损坏之前搬移 ✅
  用户无感 ✅
  避免 Read Error 触发读恢复的性能抖动 ✅
```

#### 3.8.5 参数建议

| 参数 | 建议值 | 说明 |
|------|--------|------|
| BBT_REDUNDANCY | 3 | BBT 冗余份数 |
| BBT_STORE_BLOCK | 36 | BBT 存储 Block 号 |
| SCAN_PAGES_PER_BLOCK | 2 | 出厂扫描每 Block 读的页数 |
| MAX_BAD_BLOCKS | 200 | BBT bitmap 容量上限 |
| ECC_PREDICT_THRESHOLD | 8 次 | ECC 连续上升触发预测 |

### 3.9 与企业方案对比

| 维度 | 本方案 | 企业级（Samsung / Solidigm） |
|------|--------|-----------------------------|
| Super Block | 跨双 Plane Super Block | 跨 N Die Super Block |
| 温度分区 | Hot / Cold | 多级温度 |
| OP | 10%（可配置 0~28%） | 7~28% |
| Free Pool | 三级（Pre-conditioned/Normal/Emergency） | 多层 + 动态调节 |
| GC | Background GC 持续补充 | 后台预测性 GC |
| SLC Cache | 不需要（本身就是 SLC） | 是（TLC/QLC） |
| 元数据分布 | 每 LUN 固定位置 | 跨 Die 动态分配 + RAID1 |

---

## 4. Block 生命周期

### 4.1 完整流程

当前生命周期以 Super Block 为粒度，三级 Free Pool 管理，多维度触发 GC：

```
                ┌────────────────────────┐
                │      Free Pool          │
                │      三级池              │
                │  ┌────────────────────┐ │
                │  │ Pre-conditioned     │ │ ← 提前擦好，前台零等待分配
                │  │ Normal              │ │
                │  │ Emergency           │ │ ← 最后防线
                │  └────────────────────┘ │
                └───────────┬────────────┘
                            │ allocator: 双 Plane 各取一个配对 + 磨损均衡
                            ▼
                ┌──────────────────────────┐
                │  Open Super Block          │  ← 当前写入位置
                │  Plane0: Block A            │    每温度等级 1 个
                │  Plane1: Block B            │    Hot / Cold
                │  [已写 30/64 页]            │
                └──────────┬───────────────┘
                            │ 64 页写满
                            ▼
                ┌──────────────────────────┐
                │  Closed Super Block        │  ← 等待 GC
                │  Plane0: [VVI..IVV]        │    跨双 Plane
                │  Plane1: [IVV..VVI]        │    有效页 + 无效页
                └──────────┬───────────────┘
                            │ 多维度触发 GC
                            ▼
                ┌──────────────────────────────────────────┐
                │  GC 执行                                  │
                │  ① 多维度选 victim（有效页,擦除,温度,时间）│
                │  ② 遍历双 Plane，搬移有效页（通过 FC）     │
                │  ③ 原子性保证：journal + GC bitmap         │
                │  ④ 擦除双 Plane victim Block              │
                │  ⑤ 归还 Pre-conditioned Pool              │
                └──────────────────────────────────────────┘
                            │
                            ▼
                  回到 Free Pool（Pre-conditioned）
```

### 4.2 关键说明

- 不存在"日志区→数据区"的 merge 操作（那是混合映射的概念）
- 纯页映射下，数据落地就是最终位置
- 生命周期是 Free → Open Super Block → Closed Super Block → GC Erase → Free 的循环
- GC 以 Super Block 为粒度，一次回收双 Plane 的两个物理 Block
- GC 的"搬移"是：读 Super Block 中所有有效页 → 写到新的 Open Super Block → 更新映射 → 擦除原双 Block
- Pre-conditioned Pool 由 Background GC 持续补充，前台 IO 无需等待 GC

### 4.3 GC Victim 选择算法（多维度加权评分）

纯贪心（选有效页最少的 Block）在 SLC 场景下基本可用，但存在缺陷：

- **磨损均衡缺失**：可能选中擦除次数 5000 次快报废的 Block，而非 20 次几乎全新的
- **温度维度缺失**：COLD 数据有效页几乎不变，下次 GC 还得搬

本方案使用多维度加权评分，四个维度同时考虑。

加权公式：`score = 0.50 × valid_ratio + 0.20 × (1 - erase_ratio) + 0.20 × temp_factor + 0.10 × (1 - age_ratio)`

| 权重 | 值 | 说明 |
|------|----|------|
| w1 (有效页) | 0.50 | 核心指标，直接影响写放大 |
| w2 (擦除次数) | 0.20 | 磨损均衡，避开临近寿命的 Block |
| w3 (温度) | 0.20 | HOT 数据"自然死亡"快，投资未来无效收益 |
| w4 (时间) | 0.10 | 保证每个 Block 都有机会被回收 |

### 4.4 GC 触发条件（多维度触发）

不只依赖 free pool 水位线，而是多维度综合判断：

| 触发 | 条件 | 说明 | 紧急度 |
|------|------|------|--------|
| T1: 水位线 | Pre-conditioned < 32 | 核心触发，空闲块不足 | 🔴 紧急 |
| T2: 写入量 | 累计写入 > 1024 次 | 写入量触发，分散 GC 压力 | 🟡 主动 |
| T3: 定时 | 500ms 定时器到期 | 空闲时有空就做一点 | 🟢 后台 |
| T4: 强制 | Emergency pool 启用 | 无空闲块时的最后手段 | 🔴 紧急 |
| T5: 磨损均衡 | 最大/最小擦除比 > 10 | 防止局部 Block 磨损太快 | 🟡 均衡 |
| T6: 碎片率 | 平均有效页 > 70% | Block 碎片化时提前整理 | 🟡 预防 |

### 4.5 GC 搬移的原子性保证

GC 搬移（读 → 写新位置 → 更新 L2P → 擦除旧 Block）不是原子的。掉电可能发生在中间任何一步。

三层保护机制：

- **Layer 1: Journal** — 每个 L2P 更新先写 Journal 后改 cache，掉电后 Journal 回放重建 L2P
- **Layer 2: GC Bitmap** — 在 NAND 元数据区记录搬移进度 bitmap，每搬 16 页更新一次，掉电恢复后从断点继续
- **Layer 3: 双拷贝容忍** — 任何掉电场景最多产生一个"孤悬"Block，不会丢数据，下次 GC 回收

### 4.6 企业级 GC 参考

企业级 GC 不是为了"回收空间"，而是"维持性能一致"。Idle Time GC 持续在后台做，前台 IO 永远不感知 GC。同时动态调节 GC 占用的 NAND 带宽（Host 活跃时 ≤ 10%，空闲时 100%）。

三星 GC 策略 = 多维度加权 × 动态优先级 × Soft/Hard 多级阈值。

### 4.7 GC 参数配置建议

| 参数 | 建议值 | 说明 |
|------|--------|------|
| PRE_COND_LOW_WATER | 32 blocks | Pre-conditioned 低水位 |
| GC_INTERVAL_WRITES | 1024 | 写入量触发间隔 |
| GC_TIMER_INTERVAL_MS | 500 ms | 定时触发间隔 |
| GC_WEAR_LEVEL_RATIO | 10 | 最大/最小擦除比阈值 |
| GC_FRAGMENT_THRESHOLD | 0.7 | 平均有效页 > 70% 触发 |
| GC_WEIGHT_VALID | 0.50 | 有效页权重 |
| GC_WEIGHT_ERASE | 0.20 | 擦除次数权重 |
| GC_WEIGHT_TEMP | 0.20 | 温度权重 |
| GC_WEIGHT_AGE | 0.10 | 时间公平权重 |
| GC_BANDWIDTH_HOST_ACTIVE | 10% | Host 活跃时 GC 带宽上限 |
| GC_BANDWIDTH_HOST_IDLE | 100% | Host 空闲时 GC 带宽 |

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

static nand_paa_t l2p_region_ppa(uint8_t ch, uint8_t ce, uint8_t lun,
                                  uint16_t region_id)
{
    uint16_t block_offset = region_id / PAGES_PER_BLOCK;
    uint16_t page_in_block = region_id % PAGES_PER_BLOCK;
    return paa_make(page_in_block, 0, L2P_BASE_BLOCK + block_offset, lun, 0);
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

### 7.6 Journal 自身可靠性（掉电保护）

Journal 保护 L2P 映射不丢失，但 Journal 本身也写在 NAND 上。写 Journal 时掉电怎么办？这是 Journal 设计的"递归可靠性问题"。

#### 三种掉电场景

```
Journal Page PROGRAM 的三个阶段：

  ① SRAM buffer 中累积 128 条 entry（未写 NAND）
     掉电 → 最多丢失 128 条 entry                        ← 掉电点 A

  ② FC PROGRAM 提交，NAND 开始烧录（~300μs）
     半途掉电 → 页面上数据不完整                           ← 掉电点 B

  ③ PROGRAM 成功，但返回途中掉电
     FTL 不知是否写入成功                                  ← 掉电点 C
```

#### 四层保护（不需要双写）

本方案的四层保护，充分利用 FC 层已有的 BCH ECC，不需要双写（2×写放大）：

```
Layer 1: BCH ECC（FC 层自带，零额外开销）
  ────────────────────────────────────────────────────
  每次 FC PROGRAM/READ 都经过 BCH ECC（t=4）
  ECC 可纠正 ≤4 bit 的错误，超过时返回 UNC
  掉电点 B 导致的半烧录页面 → 如有少量 bit 错 → ECC 自动修 ✅
                              超过纠错能力 → ECC 报 UNC → 整页丢弃 ✅

Layer 2: Entry 内置 CRC
  ────────────────────────────────────────────────────
  Journal entry 的 CRC 字段做二次校验：
  ECC 虽然能纠正 ≤4 bit，但不能保证"纠正后的数据一定正确"
  CRC 做最终验证 → 消除 ECC 误纠可能 ✅

Layer 3: Header entry_count 精确定位
  ────────────────────────────────────────────────────
  Header 中记录本页实际有效的 entry 数（0~128）：
    恢复时只解析前 entry_count 条，之后的忽略 ✅
    Header 受 ECC 保护 → entry_count 可信 ✅

Layer 4: 单调递增 sequence
  ────────────────────────────────────────────────────
  每个 entry 有连续递增的 sequence 号
  恢复时检测连续性：gap → 截断于此
  即使 page CRC 假阳性 → sequence 不连续也能检测 ✅

  双 Block 轮换（不是双写！）：
  Block J0 写满 → 切 J1 → J0 擦除
  恢复时读 J0 和 J1 最后一页，取 sequence 大的
  无需双写代价，只需多读一页 ✅
```

#### Journal Page 完整格式

```
每个 Journal Page（2048B = 1 NAND page = 4 CW × 512B）：

┌─────────────────────────────────────────────────────────┐
│ Header: 12B                                              │
│  ├── magic       (4B) = 0x4A4F5552 ("JOUR")            │
│  ├── page_seq    (4B) = 本页第一条 entry 的全局序号      │
│  ├── entry_count (2B) = 本页有效 entry 数（0~128）       │
│  └── block_seq   (2B) = 本 Journal Block 写入轮次       │
├─────────────────────────────────────────────────────────┤
│ Entries: N × 16B （N = entry_count，≤ 128）              │
│  ├── entry[0]: lba(4B) | old_paa(4B) | new_paa(4B)     │
│  │         : seq(4B) = 单调递增全局序号                  │
│  ├── entry[1]                                            │
│  ├── ...                                                 │
│  └── entry[N-1]                                          │
├─────────────────────────────────────────────────────────┤
│ Padding: (128 - N) × 16B = 0xFF（未使用区域）            │
└─────────────────────────────────────────────────────────┘
注意：Header + Entries 整页由 FC 的 BCH ECC 保护（写入 4 CW）。
      Entry 内部 CRC 已在 entry 结构中（16B含 sequence 和 CRC）。
```

#### entry_count 应对掉电点 B

```
掉电发生在 PROGRAM 中途（掉电点 B）：
  NAND 页面部分烧录 → entry_count 可能是正确值或 0xFF

恢复时读该页：
  FC READ → BCH ECC 解码
    ├── 成功  → entry_count 可信 → 只解析前 entry_count 条 ✅
    └── 失败  → 整页无效 → 只用上一页 ✅
```

#### 企业级做法参考

```
企业级 SSD 有硬件掉电保护方案：

三星/铠侠：
  Journal 有独立电源轨 + 大电容（~5ms）
  检测到掉电 → 电容支撑完成最后一次 PROGRAM
  → "掉电点 B" 在硬件层面消除

消费级 DRAM-less：
  无掉电电容 → 纯软件方案
  → 方案与本节设计一致：ECC + CRC + sequence + batch flush + 定时 flush

UFS/eMMC：
  硬件加速 Journal（Replay Protected Memory Block）
  硬件保证 write atomicity
```

#### 本方案总结

| 保护层 | 实现 | 成本 | 保护能力 |
|--------|------|------|---------|
| BCH ECC (t=4) | FC 层自带 | 0 | 纠正 ≤4 bit，检测更多 ✅ |
| Entry CRC | 16B entry 包含 CRC | 4B/entry | 二次校验 ✅ |
| entry_count header | Header 2B | ~0 | 精确定位有效 entry ✅ |
| Monotonic sequence | 已有字段 | 4B/entry | 检测 gap 和乱序 ✅ |
| 双块轮换 | 2 Block 轮流写 | ~0 | 避免双写代价 ✅ |
| 双写 | ❌ 不需要 | — | ECC 四层已足够 |

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

### ADR-6：Super Block（跨双 Plane 条带化）

| 项目 | 决策 |
|------|------|
| 选型 | **跨双 Plane Super Block** |
| 理由 | 学习企业级跨 Die 条带化逻辑；双 Plane 并行 Multi-Plane PROGRAM 提升写入带宽；FC 层已支持 Multi-Plane 操作 |
| 拒绝方案 | 单 Block 独立写入（无法利用双 Plane 并行）|
| 代价 | Super Block 配对增加 allocator 复杂度 |

### ADR-7：温度分区（Multi-Stream）

| 项目 | 决策 |
|------|------|
| 选型 | **Hot / Cold 双 Open Super Block** |
| 理由 | 冷热数据分离提升 GC 效率；HOT 数据无效页增长快、回收效率高；COLD 数据几乎不变、几乎不用回收；纯软件逻辑，无硬件依赖 |
| 拒绝方案 | 单温区混合写入（GC 效率差）、多级温度（512MiB 容量不需要）|
| 代价 | allocator 需维护双 Super Block；FTL 写操作需指定温度参数 |

### ADR-8：OP 10% 预留

| 项目 | 决策 |
|------|------|
| 选型 | **10% 预留（可配置 7~28%）** |
| 理由 | 消费级典型值；RMA 场景坏块替换后用户容量不变；写入压力大时提供 GC 缓冲、防止性能断崖 |
| 拒绝方案 | 0% OP（坏块替换直接减少用户容量，写入压力时 GC 阻塞前台 IO）|
| 代价 | 用户可见容量减少约 10%（512MiB → ~456MiB）|

### ADR-9：三级 Free Pool（Pre-conditioned/Normal/Emergency）

| 项目 | 决策 |
|------|------|
| 选型 | **三级结构：Pre-conditioned + Normal + Emergency** |
| 理由 | 学习企业级 Background GC 和前台 IO 隔离；Pre-conditioned 池保证前台 IO 零等待分配；Emergency 池作为最后防线 |
| 拒绝方案 | 单池（前台 IO 可能等 GC 擦除）|
| 代价 | 需维护三级池计数和 Background GC 补充逻辑 |

### ADR-10：SLC Cache 不实现

| 项目 | 决策 |
|------|------|
| 选型 | **不实现 SLC Cache** |
| 理由 | 当前 NAND 本身就是 SLC，不存在 TLC/QLC 的慢速写入问题；SLC Cache 是 TLC/QLC SSD 的写加速技术 |
| 拒绝方案 | （不适用）|

### ADR-11：坏块管理（BBT）

| 项目 | 决策 |
|------|------|
| 选型 | **出厂全量扫描 + 运行时隔离 + Block Skip 替换** |
| 理由 | SLC 出厂坏块极少（~2~10 个），全量扫描 250ms 可接受；Block Skip 零额外硬件成本；OP 池自然吸收坏块替换的容量损失 |
| 拒绝方案 | Block Sparing（企业级大容量 QLC 才需要）；ECC 趋势坏块预测（SLC 不需要）|
| 代价 | 出厂扫描增加启动时间 ~250ms；需要 FC 支持 OOB/Spare 区读取 |

---

## 10. 企业级实现对比

### 10.1 Block 管理对比

| 维度 | 本方案（当前设计） | 消费级（Phison/SMI） | 企业级（Samsung/Solidigm） |
|------|-----------------|---------------------|--------------------------|
| 数据块分区 | Hot / Cold 双温区 | 2~4 温区 | 多级温度 + SLC cache |
| 空闲池 | 三级（Pre-conditioned/Normal/Emergency） | 多层池（不同 P/E） | Pre-conditioned + 紧急池 |
| GC 触发 | 六级（水位线/写入量/定时/磨损/碎片/强制） | 多级阈值 | 后台预测性 GC |
| Over-provision | 10% 预留 | 7~15% | 7~28% |
| 写缓冲 | 无（SLC 不需要） | SLC mode buffer | SLC / pseudo-SLC cache |
| Super Block | 跨双 Plane | 跨 Die | 跨 N Die Super Block |

### 10.2 L2P 管理对比

| 维度 | 本方案 | 消费级（DRAM-less） | 企业级（有 DRAM） |
|------|--------|--------------------|-------------------|
| 表位置 | NAND | NAND | DRAM 全表 |
| SRAM cache | 64~128 regions | 32~256 regions | N/A |
| 替换策略 | CLOCK | CLOCK / 2Q | N/A（全表常驻）|
| 写策略 | Write-back + J | Write-back + J | 直写 DRAM |
| 启动时间 | ~秒级（checkpoint） | ~秒级 | 毫秒级 |

### 10.3 企业级的"多流写入"

本方案已实现 Hot / Cold 双 Open Super Block，与企业级多温区写入方向一致：

```
本方案（Hot / Cold 双 Open Super Block）:
  Hot  Super Block:  [A(热)][C(热)][E(热)]...   ← 用户 IO 数据
  Cold Super Block:  [B(冷)][D(冷)][F(冷)]...   ← GC 搬移的有效页、元数据
  GC Hot:  无效页率高 → 很快回收 ✅
  GC Cold: 无效页率低 → 几乎不用回收 ✅

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
/* 温度等级 */
#define FTL_TEMP_HOT    0
#define FTL_TEMP_COLD   1
#define FTL_TEMP_NUM    2

int        allocator_init(ftl_allocator_t *alloc, uint8_t op_percent);
nand_paa_t allocator_alloc(ftl_allocator_t *alloc, uint8_t temperature);
void       allocator_free_block(ftl_allocator_t *alloc, uint16_t block);
void       allocator_mark_bad(ftl_allocator_t *alloc, uint16_t block);
int        allocator_needs_gc(const ftl_allocator_t *alloc);
void       allocator_refill_pre_cond(ftl_allocator_t *alloc);
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

## 12. ftl_service 主循环调度

`ftl_service()` 是整个 FTL 的"心脏"，负责协调前台 IO 和后台任务的执行时序。

### 12.1 调度模型概述

```
每次 ftl_service() 调用的执行顺序：

  ┌─────────────────────────────────────────────────────────────┐
  │  ftl_service()                                              │
  │                                                             │
  │  Phase 1 ── fc_service() 最多 NAND_SUBMIT_BUDGET 次         │
  │  ├── 推进 NAND 命令执行                                     │
  │  └── 计数 submitted（本轮新提交的 NAND 命令数）               │
  │                                                             │
  │  Phase 2 ── 处理完成回调                                    │
  │  ├── while (fc_poll_completion() == OK)                     │
  │  └── 根据 completion.module 路由到不同 handler               │
  │                                                             │
  │  Phase 3 ── 后台任务（三级模式切换）                          │
  │  ├── IDLE 模式：NAND 完全空闲 → 全力做后台                   │
  │  ├── NORMAL 模式：相对空闲 → 适度做后台                      │
  │  └── BUSY 模式：前台 IO 活跃 → 最小后台                      │
  └─────────────────────────────────────────────────────────────┘
```

**三级模式切换依据**：SQ 队列深度 + 上轮提交的 NAND 命令数。

| 模式 | 条件 | 前台 IO 带宽 | 后台任务带宽 |
|------|------|------------|------------|
| IDLE | SQ 空 + submitted == 0 | 0% | 100% |
| NORMAL | SQ 使用率 < 25% | ~70% | ~30% |
| BUSY | SQ 使用率 ≥ 25% | ~95% | ~5% |

### 12.2 三种后台模式的行为

#### IDLE 模式（NAND 完全空闲）

```
触发条件：SQ 队列空 + 上一轮没有提交新命令
行为：
  flush:    l2p_cache_flush_all()          → 清空所有 dirty
  GC:       refill_pre_cond(32)             → 补满 pre-conditioned 池
  Journal:  journal_flush_page()            → 强制写 journal buffer
  Checkpoint: 累计写入量够就 cp_create()
设计思路：NAND 闲着也是闲着，把后台活做完，下次 Host IO 来时系统处于最佳状态
```

#### NORMAL 模式（系统相对空闲）

```
触发条件：SQ 使用率 < 25%
行为：
  flush:    l2p_cache_flush_background(4)  → 最多 flush 4 条
  GC:       if (pre_cond < 水位) 搬 1~4 页
  Journal:  不单独 flush（让 page_buf 自然填满）
  Checkpoint: 检查但不一定执行
设计思路：留 NAND 带宽给突发 IO，后台任务"慢慢做"
```

#### BUSY 模式（前台 IO 活跃）

```
触发条件：SQ 使用率 ≥ 25%
行为：
  flush:    l2p_cache_flush_background(1)  → 只 flush 1 条（防 evict 阻塞）
  GC:       跳过（除非 Emergency pool 启用）
  Journal:  跳过
  Checkpoint: 跳过
设计思路：前台 IO 优先。唯一例外：Emergency pool 启用时强制 GC。
```

### 12.3 实现

```c
#define NAND_SUBMIT_BUDGET      (4)      // 每轮最多提交 4 条
#define BG_NORMAL_FLUSH_COUNT   (4)
#define BG_BUSY_FLUSH_COUNT     (1)
#define BG_NORMAL_GC_PAGES      (4)
#define BG_BUSY_GC_PAGES        (0)
#define BG_IDLE_GC_TARGET       (32)
#define CP_WRITE_INTERVAL       (1024)
#define CP_TIME_INTERVAL_MS     (10000)

void ftl_service(ftl_context_t *ftl) {
    /* Phase 1: fc_service 推进 NAND */
    uint32_t submitted = 0;
    for (int i = 0; i < NAND_SUBMIT_BUDGET; i++) {
        if (fc_service() != FC_STATUS_OK) break;
        submitted++;
    }

    /* Phase 2: 处理完成回调 */
    fc_command_status_t completion;
    while (fc_poll_completion(&completion) == FC_STATUS_OK) {
        ftl_process_completion(ftl, &completion);
    }

    /* Phase 3: 判断总线状态，选后台模式 */
    uint8_t sq_used = fc_get_sq_usage();

    if (sq_used == 0 && submitted == 0) {
        ftl_service_idle(ftl);
    } else if (sq_used < FC_SQ_DEPTH * 25 / 100) {
        ftl_service_normal(ftl);
    } else {
        ftl_service_busy(ftl);
    }
}

static void ftl_service_idle(ftl_context_t *ftl) {
    l2p_cache_flush_all(&ftl->cache);
    uint32_t to_free = FREE_POOL_TARGET - ftl->free_pool.pre_cond_count;
    while (to_free > 0) {
        if (!ftl_gc_background(ftl, 16)) break;
        to_free -= 2;
    }
    journal_flush_page(&ftl->journal);
    ftl_cp_check(ftl);
}

static void ftl_service_normal(ftl_context_t *ftl) {
    l2p_cache_flush_background(&ftl->cache, BG_NORMAL_FLUSH_COUNT);
    if (ftl->free_pool.pre_cond_count < PRE_COND_LOW_WATER) {
        ftl_gc_background(ftl, BG_NORMAL_GC_PAGES);
    }
    ftl->total_writes++;
    if (ftl->total_writes % CP_WRITE_INTERVAL == 0) {
        ftl_cp_check(ftl);
    }
}

static void ftl_service_busy(ftl_context_t *ftl) {
    l2p_cache_flush_background(&ftl->cache, BG_BUSY_FLUSH_COUNT);
    if (ftl->free_pool.pre_cond_count == 0) {
        ftl_gc_emergency(ftl);  /* 同步 GC，前台 IO 会等 */
    }
}
```

### 12.4 企业级主循环参考

```
企业级 SSD 主循环（三星/Solidigm）：

  while (1) {
      process_host_doorbell();      // NVMe 提交队列
      ftl_submit_host_io();         // 查 L2P、构造 FC 命令
      fc_service();                 // 推进 NAND
      process_fc_completions();     // 中断下半部
      background_scheduler();       // 独立后台任务调度器
  }
```

企业级后台调度器使用**独立时间预算**控制每个任务：

```c
typedef struct {
    void     (*task_func)(void);
    uint32_t  interval_ticks;        // 最小执行间隔（主循环次数）
    uint32_t  ticks_since_run;
    uint8_t   priority;              // 0=最高, 3=最低
    uint32_t  budget_us;             // 每轮最多花多少 μs
} background_task_t;

void background_scheduler(void) {
    uint32_t budget_remaining = BG_TOTAL_BUDGET_US;  // 每轮总预算
    for (int i = 0; i < BG_TASK_COUNT; i++) {
        background_task_t *task = &g_bg_tasks[i];
        task->ticks_since_run++;
        if (task->ticks_since_run < task->interval_ticks) continue;

        uint32_t start = timer_get_us();
        task->task_func();                              // 传预算执行
        uint32_t elapsed = timer_get_us() - start;
        budget_remaining -= MIN(elapsed, task->budget_us);
        task->ticks_since_run = 0;

        if (budget_remaining < MIN_TASK_BUDGET_US) break;
    }
}
```

**企业级 vs 本方案对比**：

| 维度 | 本方案（三级模式） | 企业级方案 |
|------|-----------------|-----------|
| 调度粒度 | 按 SQ 深度分三级 | 每个任务独立时间预算 + 间隔 |
| 后台抢占 | Phase 3 整体让路 | 高优任务可打断低优 |
| 时间控制 | 按"做几条"控制 | 精确到 μs 硬件定时器 |
| 前台保护 | SQ > 75% 停后台 | NAND 带宽比例动态调节 10~100% |
| 多核 | 无 | CPU0 = 前台 IO, CPU1 = 后台任务 |

**企业级反直觉做法——Inline mini-GC**：

```
ftl_write() → allocator_alloc()
  → 发现 pre_cond_pool 太低
  → 不等后台，inline 做一次 mini-GC（搬 1 页）
  → 继续写

原理：写操作要等 NAND program（~300μs），
      CPU 在等待间隙闲着也是闲着，顺便做后台
```

### 12.5 主循环完整形态

```c
int main(void) {
    platform_init();         // 时钟、外设
    fc_init();               // FC 层初始化
    ftl_init(&g_ftl);        // FTL 层初始化（含 L2P cache 加载）
    controller_init(&g_ctrl, &g_ftl);

    while (1) {
        if (host_io_pending()) {
            host_process_requests(&g_ctrl);
        }
        ftl_service(&g_ftl);           // FTL 主循环（含 fc_service）
        watchdog_refresh();
        if (ftl_is_idle(&g_ftl)) {
            cpu_sleep();               // 空闲时低功耗
        }
    }
}
```

---

## 13. 与 FC 层接口约定

### 13.1 FTL 使用 FC 的约束表

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

### 13.2 FTL 写数据的标准序列

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

### 13.3 FTL 应当避免的操作

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

## 14. 未解决问题

| # | 问题 | 影响 | 当前选项 |
|---|------|------|---------|
| 1 | Region 物理存储：固定 vs 动态元数据 block | 固定简单但磨损集中 | 固定 32 blocks（SLC 100k P/E 足够） |
| 2 | Journal 可靠性：journal 本身怎么防掉电 | journal page program 到一半掉电 | 双页 + CRC 校验 |
| 3 | 启动加载策略：加载 checkpoint 全表还是按需 miss | 启动延迟 vs 首次访问延迟 | 干净关机 → 按需 miss；异常掉电 → checkpoint |
| 4 | Journal 回收时机 | 需要确保 L2P 表已同步 | 配合 checkpoint 同步截断 |
| 5 | L2P 表磨损：元数据 block 擦除是否超过数据 block | 寿命不均衡 | region 更新合并 + checkpoint 批量写 |
| 6 | 多 Plane 下 region 分布：是否跨 plane 并行写入 | L2P 写回性能 | 可优化为 Multi-Plane PROGRAM |

---

## 15. 设计总结

### 15.1 设计定位

```
本方案定位：消费级 DRAM-less SSD（UFS/eMMC 主控档次）
对标产品：Phison E21T / WD SN580 / UFS 4.0 嵌入式方案
硬件平台：STM32H743（1MB SRAM + 8MB SDRAM）+ MT29F4G08 SLC NAND（512MiB）
```

### 15.2 设计决策总览

| 决策 | 选择 | 行业对标 | 硬件可行性 |
|------|------|---------|-----------|
| 映射策略 | 纯 CW 级（512B）页映射 | 所有现代 SSD 标准做法 | ✅ 与 FC 完全对齐 |
| L2P 存储 | NAND 元数据区（DRAM-less） | UFS/eMMC 主流 | ✅ 1MB SRAM 可做 cache |
| L2P 缓存 | SRAM region cache (64~128 regions) | Phison/SMI DRAM-less 方案 | ✅ 64KB~256KB |
| 替换策略 | CLOCK | 嵌入式主流，Phison/UFS 使用 | ✅ O(1)，无动态内存 |
| 写回策略 | Write-back + Journal | 消费级 DRAM-less 标配 | ✅ 纯软件 |
| Super Block | 跨双 Plane | 企业级跨 Die 的简化版本 | ✅ FC Multi-Plane 已支持 |
| 温度分区 | Hot / Cold 双 Open Super Block | 消费级 2~4 温区 | ✅ 纯软件 |
| OP 预留 | 10%（可配置 0~28%） | 消费级典型值 7~15% | ✅ 编译期配置 |
| Free Pool | 三级（Pre-conditioned/Normal/Emergency） | 企业级 Background GC | ✅ 纯软件 |
| Bad Block 管理 | 出厂全量扫描 + Block Skip | 消费级标准 | ⚠️ 需要 FC OOB 接口 |
| GC 算法 | 多维度加权评分 + 六级触发 | 与企业级一致 | ✅ 纯软件计算 |
| GC 原子性 | Journal + GC Bitmap + 双拷贝容忍 | 消费级/企业级做法 | ✅ 纯软件 |
| 主循环调度 | 三级模式（IDLE/NORMAL/BUSY） | 与消费级一致 | ✅ 纯软件 |
| RAID | 不实现（CONFIG_FC_RAID = 0） | — | SLC 不需要 |
| SLC Cache | 不实现 | — | 本身就是 SLC |

### 15.3 硬件依赖审查

所有设计功能中，除 BBT 出厂扫描需要 FC 支持 OOB/Spare 区读取外，其余功能均为纯软件逻辑，当前 FC 层和硬件平台已全部支持：

| 功能 | 硬件依赖 | 状态 |
|------|---------|------|
| Multi-Plane PROGRAM/READ/ERASE | FC 层操作码 | ✅ FC Phase E 已实现 |
| BCH ECC（t=4） | FC 数据路径 | ✅ FC Phase K 已实现 |
| CRC-32 | FC 数据路径 | ✅ FC Phase L 已实现 |
| Random 加扰 | FC 数据路径 | ✅ FC Phase J 已实现 |
| DMA 传输 | FC/NAND 层 | ✅ FC Phase F/G/H 已实现 |
| OOB/Spare 区读写 | FC 接口 | ⚠️ 需确认/添加 |

### 15.4 与企业/消费级的一致性

```
本方案   vs   消费级 DRAM-less (Phison E21T)  vs   企业级 (Samsung PM9A3)
─────────────────────────────────────────────────────────────────────────
纯页映射 ✅       纯页映射                        纯页映射（全表在 DRAM）
CLOCK ✅          CLOCK / 2Q                     不需要（全表常驻）
Write-back+J ✅   Write-back+J                   直写 DRAM
Hot/Cold 分区 ✅  2~4 温区                       多级 + SLC cache
三级 Free Pool ✅ 多层池                          Pre-conditioned + 紧急池
OP 10% ✅         7~15%                          7~28%
BBT Block Skip ✅ Block Skip                     Stripe Skip + Sparing
无掉电电容 ✅     无掉电电容                      有掉电电容
三级主循环 ✅     类似                            硬件时间预算调度器
```

**结论**：本方案与消费级 DRAM-less SSD 主流做法一致，定位合理。没有"为了炫技而做的无用功能"，也没有"当前硬件无法支持的设计"。唯一的缺口是 FC 层的 OOB 读写接口。

### 15.5 Phase 实施顺序建议

```
Phase 0 — L2P Cache 基础设施
  └── P0.1 → P0.2 → P0.3 → P0.4 → P0.5

Phase 1 — 掉电恢复
  └── P1.1 → P1.2 / P1.6 → P1.3 → P1.4 → P1.5
       └── P1.7（Journal 掉电窗口）

Phase 2 — 完整 FTL
  └── P2.1 → P2.2 → P2.3 → P2.9（OP）→ P2.10（三级池）
       └── P2.8（温度区）→ P2.4 / P2.11（GC）
       └── P2.5 / P2.6 / P2.7（ftl_service）

Phase 3 — 优化
  └── P3.1 → P3.2 → P3.3 → P3.4
```

---

## 16. Phase 规划

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
| P1.1 | Journal 格式 + NAND 写入（含 buffer 半满提前 flush）| 中 |
| P1.2 | Write-back + journal 集成 ftl_write | 中 |
| P1.3 | 启动时 journal 回放 | 中 |
| P1.4 | Checkpoint 全量/增量 | 小 |
| P1.5 | 完整启动恢复流程 | 中 |
| P1.6 | Write-back 四级触发（水位线 + 定时 + evict + 同步）| 中 |
| P1.7 | Journal 定时 flush + 掉电窗口控制 | 小 |

### Phase 2：完整 FTL

| 任务 | 说明 | 估算 |
|------|------|------|
| P2.1 | Allocator（Super Block 分配 + 双 Plane 配对 + 顺序写指针 + 温度参数） | 中 |
| P2.2 | Bad block 扫描 + BBT | 中 |
| P2.3 | 反向映射 bitmap | 小 |
| P2.4 | GC 多维度加权 victim + 同步搬移（含搬移中阶段性 flush + 原子性保证） | 大 |
| P2.5 | Background flush 集成 ftl_service（水位线 + 定时） | 小 |
| P2.6 | NAND 带宽动态感知（FC 队列深度调节 flush/GC 速度） | 中 |
| P2.7 | Thrashing 检测 + 降级 | 中 |
| P2.8 | Hot/Cold 温度分区（allocator 双 Super Block + 归类策略） | 中 |
| P2.9 | OP 预留实现（默认 10%，支持 RMA 坏块替换 + 性能保速） | 小 |
| P2.10 | Pre-conditioned pool + Background GC 三级 Free Pool | 中 |
| P2.11 | GC 按 Super Block 粒度回收（双 Plane 擦除 + GC Bitmap） | 中 |

### Phase 3：优化

| 任务 | 说明 | 估算 |
|------|------|------|
| P3.1 | Prefetch 顺序读预加载 | 中 |
| P3.2 | GC L2P 批量更新优化 | 中 |
| P3.3 | Warm boot 保留 cache | 小 |
| P3.4 | 性能计数器 + 调试接口 | 小 |

---

## 16. 附录：核心数据结构原型

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
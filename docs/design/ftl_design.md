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
| v0.9 | 2026-09-24 | Super Block 坏块处理升级：Block Skip → Block Sparing + Spare Pool；"两层映射"方案废弃，回归行业标准物理 PAA 直映射（§3.8.3/ADR-11/§9/§15 联动更新） | Claude |
| v0.10 | 2026-09-24 | 新增静态磨损均衡（Static Wear Leveling）完整设计：三星企业级独立 Daemon 方案，三条件触发、冷 Block 选择算法、数据搬移流程、ftl_service 集成（§4.8 新增） | Claude |
| v0.11 | 2026-09-24 | Journal 回收触发条件重写：三级触发（水位线 85%/95% + 写入量 4096 + 定时 10s），基于企业级"预算推导"法，配套"无阻塞原则"（§7.5 重写） | Claude |
| v0.12 | 2026-09-24 | L2P 表磨损均衡定量分析（§14 U5 已解决+详细推导表）；§14 未解决问题列表更新（U2/U3/U4/U5 已解决，新增 §14.1 完整分析小节） | Claude |
| v0.13 | 2026-09-24 | 元数据区从固定 32 blocks 改为动态分配：新增 Metadata Directory + Anchor Block 设计（§3.3/§5.5）；Region 写回双 Plane 交错 Multi-Plane 加速（§5.6）；§14 U1/U6 已解决 | Claude |
| v0.14 | 2026-09-28 | FTL 接口层决策确认：LBA 粒度 512B、lba_t(32bit)/pba_t(32bit) 地址体系（LBA→PBA→PAA）、几何统一到 nand_config.h、异步提交+ftl_service 驱动完成、新增 ftl_fc_interface.h 命令构造 helper 层 | Claude |
| v0.15 | 2026-09-28 | 地址体系全面定稿：LAA/LBA/PBA/PAA 行业标准定义、cmd->meta 代替 cmd->faa、PBA=[CH][CE][PAA] bitfield、ftl_cmd_setup_ppa/ftl_cmd_write_data/ftl_cmd_read_data 三函数设计 | Claude |
| v0.16 | 2026-09-28 | Step 1~5 完整实现：PBA/PAA 全套 inline、ftl_fc_interface 桩、L2P Cache 纯 SRAM+CLOCK、Allocator+Super Block、Journal 固定 Block、GC+ftl_service 三级主循环 | Claude |
| v0.17 | 2026-09-29 | FTL 全面打通 NAND 路径：17 个核心桩全部替换为真实 FC 调用；新增同步 NAND 操作层（ftl_internal_read/write/erase）；新增 Pending Request 表；新增 ftl_read/write/trim 同步包装；GC 完整 7 步搬移（读 META→查 L2P→分配 Cold→写→L2P→Journal）；Metadata Directory 完整实现（Anchor Block 33-34 + 动态分配 + CRC 保护）；Allocator 全部桩消除（pre_cond/needs_gc/refill 同步）；controller 层 3 个隐式声明消除 | Claude |
| v0.18 | 2026-09-29 | FTL 全部剩余模块实现：Checkpoint、BBT、Read Recovery、Power Failure Recovery、Wear Leveling；代码审视修复4个逻辑断裂 | Claude |
| v0.19 | 2026-09-29 | Completion Table 统一完成路由：消除 ftl_service Phase2 和 ftl_internal_sync_wait 两个"泵"互相吞 completion 的 bug；新增 completion_table.h/c（16 项路由表 + 同步等待器 + 统一泵）；ftl_internal_sync_wait 改为注册 sync_waiter 后走 completion_table_drain，不再独占 fc_poll_completion；L2P Region 动态分配接入：cache 增加 alloc 指针，flush_one 中首次写 Region 改为真实调用 metadata_dir_alloc_region（取消回退固定位置） | Claude |
| v0.20 | 2026-09-29 | 返回值体系统一 + 历史死代码清理：新增 ftl_status_t{FTL_OK=0,FTL_ERROR=-1} 企业级风格枚举，~32 个操作函数从 int 改为 ftl_status_t，谓词/计数函数保持 int；修复 checkpoint_find_latest 因 FTL_OK=0 导致的调用反转 bug；ftl_cmd_setup_ppa 等 3 函数从 (int) 强转改为显式 FC_STATUS_OK 映射；删除历史遗留文件 nand.h/c、nand_geometry.h/c、config/nand_geometry.h、config/ftl_config.h、__patch*.txt；删除死代码 pba_t、wl_init()、allocator_refill_pre_cond()；wl_relocate 复用 gc_read_page_with_lba + gc_erase_and_free（~60 行重复代码消除）；ssd_types.h 清除 ssd_ppa_t；l2p_cache.h 显式 include allocator.h 消除隐式依赖 | Claude |
| v0.21 | 2026-09-29 | FTL 拓扑信息重构：删除 ftl_context_t/allocator_t/journal_t 中的 ch/ce/lun 散落字段，统一为 ftl_topology_t 结构体；ftl_init 新增 ftl_topology_t 参数替代原 ch/ce/lun 3 个参数；全部 pba_make 的 CH/CE 源改为从 topology 或 alloc->ch/ce 获取，消除硬编码 0,0；allocator/journal/metadata/checkpoint/bbt/power_failure_recovery/wear_leveling 各模块全部接入拓扑路由 | Claude |
| v0.22 | 2026-09-29 | FTL 对外接口异步化：删除 ftl_read/ftl_write 同步包装，FTL 对外只提供非阻塞接口；ftl_submit_write 新增 complete_flag 参数跟踪 NAND PROGRAM 完成；Controller 层改为纯异步提交 + complete_flag 轮询；新增 ssd_controller_is_complete()；ssd_request_t.completion_status 改为 ssd_status_t | Claude |

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
| OP | **10% 预留（可配置 7~28%）** | 性能保速 + GC 缓冲 |
| Free Pool | **三级（Pre-conditioned/Normal/Emergency）** | 学习 Background GC 和前台 IO 隔离 |
| Bad Block | **Block Sparing（Spare Pool 保水位）** | 学习企业级坏块替换，不做额外映射层 |
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

### 3.3 元数据区（Metadata Blocks）— 动态分配

传统消费级设计使用固定元数据区，但企业级（Samsung/Solidigm）使用**动态分配**：
元数据和用户数据共享 Free Pool，通过 Metadata Directory 追踪元数据位置。

```
Anchor Block（固定位置，出厂好块）:
  Block A0, A1（2 blocks）— 只存 Metadata Directory
  Directory 记录每份元数据当前的物理位置
  每次元数据位置变更时更新 Directory

元数据（动态分配，从 Free Pool 取）:
  ┌──────────────────────────────────────────────────┐
  │  L2P Region     ── 分散在任意物理 Block 中        │
  │  Journal        ── 在数据区中动态分配的 Block      │
  │  Checkpoint     ── 同上                          │
  │  BBT            ── 同上                          │
  │  Erase Count    ── 同上                          │
  └──────────────────────────────────────────────────┘

数据区（User + OP + Spare Pool）:
  与元数据共享同一个 Free Pool
  分配时通过类型标记区分
```

固定方案 vs 动态方案对比：

| 维度 | 固定 32 blocks | 动态分配（企业级）|
|------|--------------|------------------|
| 磨损分布 | 集中在 32 块 | ✅ 均匀分布到所有好块 |
| 坏块影响 | 需要 Reserved 替换 | ✅ 自动跳过，Free Pool 自然替换 |
| 元数据 GC | 周期 GC 压缩 32 块 | ✅ 不需要（写新位置，旧位无效）|
| 启动时间 | 扫描固定位置 | ✅ 只需读 Anchor 2 pages |
| 复杂度 | 低 | ⚠️ 需维护 Directory |

#### Anchor Block

```c
#define ANCHOR_BLOCKS      (2)     // 2 个固定 Block

#define METADATA_TYPE_L2P_REGION    (0)
#define METADATA_TYPE_JOURNAL       (1)
#define METADATA_TYPE_CHECKPOINT    (2)
#define METADATA_TYPE_BBT           (3)
#define METADATA_TYPE_ERASE_COUNT   (4)

typedef struct {
    uint8_t  type;               // 元数据类型
    uint16_t id;                 // L2P region_id / 0 for others
    nand_paa_t paa;              // 物理位置
} __attribute__((packed)) metadata_entry_t;  // 7 bytes × 128

typedef struct {
    uint32_t          magic;        // "ANDIR"
    uint32_t          sequence;     // 单调递增
    uint32_t          entry_count;  // 有效 entry 数
    metadata_entry_t  entries[128]; // 128 × 7 = 896B
    uint32_t          crc;
} metadata_directory_t;             // ~904B，占 1 NAND page
```

- Anchor Block 出厂时必须为好块（预留 2 个固定好块）
- Directory 冗余存 3 份（同 Block 的 Page 0/1/2）
- 序列号单调递增，启动时取最大 sequence 有效

#### 元数据分配 API

```c
/* 从 Free Pool 分配一个物理位置存放元数据 */
nand_paa_t allocator_alloc_metadata(ftl_allocator_t *alloc,
                                     uint8_t metadata_type,
                                     uint16_t id,
                                     uint8_t preferred_plane) {
    /* 从 Free Pool 取一个块（元数据走 COLD 温度）*/
    uint16_t block = allocator_select_block(alloc, FTL_TEMP_COLD,
                                             preferred_plane);
    nand_paa_t paa = paa_make(0, preferred_plane, block, 0, 0);

    /* 更新 Metadata Directory */
    metadata_dir_update(&g_metadata_dir, metadata_type, id, paa);

    return paa;
}
```

分配时优先选低擦除次数的 Block，元数据自身也参与全局磨损均衡。
Anchor Block 本身几乎不擦（只在 Directory 满页时才写一次，~16K 条元数据
变更才写一次，1 个 Anchor Block Page = 64 次变更才满 → 非常非常低的磨损）。

### 3.4 Super Block（跨 Plane 条带化）

Super Block 是跨双 Plane 的逻辑写入单元。当前硬件 PLANE_NUM=2，双 Plane 各取一个物理 Block 配对。

```
Open Super Block（每个温度等级一个）:
  Hot:  Plane 0: Block A,  Plane 1: Block B  ← 用户 IO 写入
  Cold: Plane 0: Block C,  Plane 1: Block D  ← GC 搬移/元数据写入

写入以 Super Block 为粒度，双 Plane 并行（Multi-Plane PROGRAM）：
  Page 0: Plane0-Page0 + Plane1-Page0
  ...
  Page 63: Plane0-Page63 + Plane1-Page63 → Super Block 写满

写满后从 Open → Closed，等待 GC 回收。
```

```c
#define FTL_TEMP_HOT    0
#define FTL_TEMP_COLD   1
#define FTL_TEMP_NUM    2

typedef struct {
    uint16_t plane_blocks[PLANE_NUM];   // Plane 0 和 Plane 1 的 Block 号
    uint8_t  next_page;                  // 当前写入页（0~63，双 Plane 同步）
    uint8_t  temperature;                // FTL_TEMP_HOT / FTL_TEMP_COLD
    uint8_t  valid;                      // 1=正在使用（Open），0=已关闭
} super_block_t;

/* 返回 Super Block 中指定 Plane/Page 的 PBA（含 CH/CE 拓扑）*/
static inline nand_pba_t super_block_page_pba(uint8_t ch, uint8_t ce,
                                               const super_block_t *sb,
                                               uint8_t plane, uint8_t page)
{
    return pba_make(ch, ce, page, plane, sb->plane_blocks[plane], 0, 0);
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

### 3.6 OP 区域（Over-Provisioning 10%）与 Spare Pool（替换池）

OP 是数据区中**不映射给用户**的好块，由 allocator 统一管理但 L2P 不覆盖。

```c
#define FTL_OP_PERCENT    (10)    // 10% 预留空间，可调 7~28，设为 0 禁用
```

Spare Pool 是**独立于 OP 的替换储备**，专用于运行时坏块替换后补充 free pool 水位：

```c
#define FTL_SPARE_POOL_SIZE   (50)    // 替换池大小，覆盖生命周期新增坏块
```

#### OP 与 Spare Pool 的分工

| 池 | 用途 | 大小 | 是否可以回收 |
|----|------|------|------------|
| OP 10% | GC 缓冲 + 性能保速 | ~405 blocks（总好块 × 10%） | ✅ GC 正常使用 |
| Spare Pool | 坏块替换专用 | ~50 blocks | ❌ 只出不进，耗尽后容量下降 |

#### OP 在 RMA 和性能恢复中的作用

```
RMA 场景：
  出厂时：总好块 4056，Spare Pool ~50，OP ~400，用户可见 ~3600 块
  使用中：新增 50 个坏块 → Spare Pool 补充 → 用户容量不变 ✅
  Spare Pool 耗尽后新增坏块 → 用户容量自然减少

性能恢复场景：
  写入压力大时：OP 池提供 400+ Block GC 缓冲
  GC 有充足空间后台搬移 → 前台写入速度稳定 ✅
  没有 OP：free pool 耗尽 → 前台 IO 等 GC → 写入断崖下降 ❌
```

#### 运行时坏块替换流程

```
Super Block {P0-BlockA, P1-BlockB} 中 P0-BlockA PROGRAM 失败：

  ① 关闭这个 Super Block（不再写入新数据）
  
  ② 数据拯救：
     P0-BlockA 的已写页 → 能读就搬，不能读就丢
     P1-BlockB 的已写页 → 全部读到 buffer → 写入新 Super Block
  
  ③ 更新 L2P：
     被搬移的数据 → L2P 指向新的物理地址
  
  ④ 标记坏块：
     BBT 标记 P0-BlockA = BAD
  
  ⑤ 处理好的 Block：
     P1-BlockB 擦除 → 成功则回 free pool，失败也标记坏块
  
  ⑥ Spare Pool 补充：
     从 Spare Pool 取一个好块 → 加入 free pool（保容量水位）
  
  ⑦ 分配新的 Super Block：
     从 free pool 取双 Plane Block 配对
```

**重要**：运行时 Multi-Plane PROGRAM 中一个 Plane 的 Block 损坏，
**不需要将两个 Block 的编号对齐**——ONFI Multi-Plane 命令中
每个 Plane 有独立的行地址（含自己的 Block#），异号不影响并行操作。

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
坏块替换策略：Block Sparing（替换池补充）

  Sparing 不是"坏块→替换块"的静态映射，而是在坏块发生后
  从 Spare Pool 取一个好块补充到 free pool，保容量水位。

  运行时坏块替换流程（以 Super Block PROGRAM 失败为例）:

    Super Block {P0-BlockA, P1-BlockB} 中 P0-BlockA PROGRAM 失败：

      ① 关闭该 Super Block（不再写入新数据）
      
      ② 数据拯救：
         P0-BlockA 的已写页 → 能读就搬，不能读就丢
         P1-BlockB 的已写页 → 全部读到 buffer → 写入新 Super Block
      
      ③ 更新 L2P：
         被搬移的数据 → L2P 指向新的物理地址
      
      ④ 标记坏块：
         BBT 标记 P0-BlockA = BAD
      
      ⑤ 处理好的 Block：
         P1-BlockB 擦除 → 成功则回 free pool，失败也标记坏块
      
      ⑥ Spare Pool 补充：
         spare_pool_alloc() → 取一个好块加入 free pool（保容量水位）
      
      ⑦ 分配新的 Super Block：
         从 free pool 取双 Plane Block 配对

  allocator 分配时跳过坏块 + 通过 Spare Pool 保水位：

    nand_pba_t allocator_alloc(ftl_allocator_t *alloc, uint8_t temperature) {
        uint16_t candidate;

        /* 从当前温度对应的 Open Super Block 取位置 */
        super_block_t *sb = (temperature == FTL_TEMP_HOT) ? &alloc->hot_sb : &alloc->cold_sb;

        /* 当前 Super Block 已写满 → 关闭它，分配新的 Super Block */
        if (sb->valid == 0U || sb->next_page >= PAGES_PER_BLOCK)
        {
            sb->valid = 0;  /* → closed */
            /* 从 Free Pool 取两个 Plane 的 Block 配对 */
            uint16_t block0 = allocator_select_block(alloc);
            uint16_t block1 = allocator_select_block(alloc);
            sb->plane_blocks[0] = block0;
            sb->plane_blocks[1] = block1;
            sb->next_page = 0;
            sb->valid     = 1;
        }

        /* 返回当前写入位置的 PBA */
        nand_pba_t pba = super_block_page_pba(alloc->ch, alloc->ce,
                                               sb, 0, sb->next_page);
        return pba;
    }

    /* 从 Free Pool 选擦除次数最少的 Block（磨损均衡） */
    static uint16_t allocator_select_block(ftl_allocator_t *alloc) {
        uint16_t best_idx = 0;
        uint32_t min_erase = UINT32_MAX;

        for (uint16_t i = 0; i < alloc->free_count; i++) {
            uint16_t block = alloc->free_blocks[i];
            if (bbt_is_bad(&alloc->bbt, block)) continue;
            if (alloc->erase_counts[block] < min_erase) {
                min_erase = alloc->erase_counts[block];
                best_idx  = i;
            }
        }

        uint16_t block = alloc->free_blocks[best_idx];
        alloc->free_blocks[best_idx] = alloc->free_blocks[--alloc->free_count];
        return block;
    }

    void ftl_handle_bad_block(ftl_allocator_t *alloc) {
        if (alloc->spare_pool_count > 0) {
            // 从 Spare Pool 取一个补充到 Free Pool
            // spare_pool_blocks[--alloc->spare_pool_count]
        }
    }
```

**为什么用 Spare Pool 而不是 Block Remap Table**：

```
Block Remap Table（"两层映射"）                   Spare Pool（本方案）：
  维护 bad_block → replacement_block 的映射       只补充 free pool，不做映射
  读写需查 remap 表翻译                              PAA 直接物理，无需翻译
  企业级不用这层间接                                  企业级也用 spare + data migration
```

ONFI Multi-Plane 命令中每个 Plane 有独立的行地址，
坏块导致配对异号不影响 Multi-Plane 并行操作，
所以不需要 "Block Remap" 来对齐编号。

#### 3.8.4 企业级 BBT 参考

| 维度 | 本方案 | 消费级 | 企业级 |
|------|--------|--------|--------|
| 出厂扫描 | 全量扫描 4096 Block | 全量扫描 | 增量扫描（只查未查过的）|
| BBT 存储 | NAND 固定 Block ×3 冗余 | NAND ×3 冗余 | NAND + SRAM cache + RAID1 |
| 运行时检测 | PROGRAM/ERASE 完成后 | 同左 | 同左 + ECC 趋势预测 |
| 替换策略 | Block Sparing（Spare Pool 保水位） | Block Skip | Stripe Skip + Sparing |
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
| FTL_SPARE_POOL_SIZE | 50 | 替换池大小（覆盖生命周期坏块）|

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

当前 Step 5 实现使用简化贪心（选 closed 时写页最少的 Super Block），后续接入多维加权评分。

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

### 4.8 静态磨损均衡（Static Wear Leveling）

消费级 SSD 大多不做静态 WL，依赖动态均衡就够了。但 TLC/QLC（3000~5000 P/E）下冷热不均会缩短设备寿命，所以企业级（Samsung）有独立的静态 WL Daemon。当前设计虽运行在 SLC 上，但以学习 TLC/QLC 企业级做法为目标，全量实现静态 WL。

#### 4.8.1 总体架构

静态 WL 作为 `ftl_service()` 中的独立后台任务，与 GC 并列：

```
ftl_service() Phase 3:
  ├── flush:        dirty cache 写回
  ├── GC:           回收有效页少的 Block（回收空间）
  ├── WL:           ✅ 静态磨损均衡（搬冷数据，均衡擦除）
  └── Checkpoint:   定期保存 L2P 快照
```

WL 与 GC 的分工：

```
           有效页少         有效页多
  ┌────────┬──────────────┬──────────────┐
  │擦除高   │ GC 选         │ 不管          │ ← 擦除次数已够
  ├────────┼──────────────┼──────────────┤
  │擦除低   │ GC/WL 都选    │ WL 选         │ ← WL 的目标
  └────────┴──────────────┴──────────────┘
```

#### 4.8.2 触发条件（三条件 OR）

```c
#define WL_DELTA_THRESHOLD    (5000)    // max-min 差值
#define WL_MAX_AVG_RATIO      (3)       // max/avg 比值
#define WL_LOW_ZONE_WIDTH     (1000)    // 低擦除区宽度
#define WL_LOW_BLOCK_PCT      (5)       // 低区 Block 占比阈值
#define WL_MIN_INTERVAL       (1000)    // 最小间隔（写入次数）

bool wl_should_trigger(ftl_context_t *ftl) {
    if (ftl->total_writes - ftl->wl_last_run < WL_MIN_INTERVAL)
        return false;

    uint32_t max_ec = allocator_max_erase(&ftl->alloc);
    uint32_t min_ec = allocator_min_erase(&ftl->alloc);
    uint32_t avg_ec = allocator_avg_erase(&ftl->alloc);

    /* 条件 1：最大/最小差值（核心触发） */
    if (max_ec - min_ec >= WL_DELTA_THRESHOLD)
        return true;

    /* 条件 2：最大/平均比值（防止少数 Block 过热） */
    if (max_ec >= avg_ec * WL_MAX_AVG_RATIO)
        return true;

    /* 条件 3：低擦除区 Block 数量（大量 Block 未充分磨损） */
    uint32_t low_zone = min_ec + WL_LOW_ZONE_WIDTH;
    uint32_t low_count = count_blocks_below(&ftl->alloc, low_zone);
    if (low_count > ftl->alloc.total_blocks * WL_LOW_BLOCK_PCT / 100)
        return true;

    return false;
}
```

三星企业级 SSD 使用此类多维度触发条件，确保在静负载等极端场景下也不会出现单个 Block 过度磨损。

#### 4.8.3 受害块选择算法

WL 选 victim 的目标与 GC 相反——**选有效页最多 + 擦除次数最少的 Block**（最冷的冷数据）。

```c
typedef struct {
    bool      found;
    uint16_t  plane_blocks[PLANE_NUM];   // 双 Plane Block 号
    uint32_t  avg_erase;                 // victim 平均擦除次数
    uint16_t  valid_pages;               // 有效页数
} wl_victim_t;

wl_victim_t wl_select_victim(ftl_context_t *ftl) {
    wl_victim_t best = { .found = false, .valid_pages = 0 };
    uint32_t min_ec = allocator_min_erase(&ftl->alloc);

    for (int i = 0; i < ftl->alloc.closed_sb_count; i++) {
        super_block_t *sb = &ftl->alloc.closed_sb_list[i];
        if (sb_is_open(sb)) continue;

        uint32_t sb_ec = super_block_avg_erase(ftl, sb);
        if (sb_ec > min_ec + WL_LOW_ZONE_WIDTH) continue;  // 不在低区

        uint16_t valid = reverse_bitmap_sb_valid_count(ftl, sb);
        if (valid < sb->total_pages * WL_MIN_VALID_RATIO) continue;  // 有效页不够多

        /* 评分：有效页越多越优先（越冷越值得搬） */
        if (valid > best.valid_pages) {
            best.found = true;
            best.avg_erase = sb_ec;
            best.valid_pages = valid;
            best.plane_blocks[0] = sb->blocks[0];
            best.plane_blocks[1] = sb->blocks[1];
        }
    }
    return best;
}
```

关键区别：

| 维度 | GC victim | WL victim |
|------|-----------|-----------|
| 有效页 | 越少越好（快速回收空间） | 越多越好（冷数据标志） |
| 擦除次数 | 越高越好（保护低擦除块） | 越低越好（需要被擦一次） |
| 筛选条件 | valid_ratio < 30% | valid_ratio > WL_MIN_VALID_RATIO |

#### 4.8.4 数据搬移流程

```c
void wl_relocate(ftl_context_t *ftl, wl_victim_t *victim) {
    SSD_LOGI("[WL] relocating SB, erase=%u, valid=%d/%d",
             victim->avg_erase, victim->valid_pages, PLANE_NUM * PAGES_PER_BLOCK);

    /* Step 1: 遍历双 Plane 所有有效页，搬移到 Cold Super Block */
    for (int plane = 0; plane < PLANE_NUM; plane++) {
        for (int page = 0; page < PAGES_PER_BLOCK; page++) {
            nand_paa_t paa = paa_make(page, plane, victim->plane_blocks[plane], 0, 0);
            if (!reverse_bitmap_is_valid(ftl, paa)) continue;

            uint8_t buf[PAGE_SIZE];
            ftl_fc_read_page(paa, buf, PAGE_SIZE);
            nand_paa_t new_paa = allocator_alloc(&ftl->alloc, FTL_TEMP_COLD);
            ftl_fc_write_page(new_paa, buf, PAGE_SIZE, 0);
            l2p_cache_remap_paa(&ftl->cache, paa, new_paa);
            journal_append_remap(&ftl->journal, paa, new_paa);
        }
    }

    /* Step 2: 擦除双 Block */
    for (int plane = 0; plane < PLANE_NUM; plane++) {
        uint16_t block = victim->plane_blocks[plane];
        ftl_erase_block(ftl, block, victim->avg_erase, plane);
        ftl->alloc.erase_counts[block]++;
    }

    /* Step 3: 归还到 Pre-conditioned Pool */
    allocator_free_blocks(&ftl->alloc, victim->plane_blocks, PLANE_NUM);
}
```

流程与 GC 搬移一致，区别在于：
- 搬移目标是 **Cold Super Block**（温度分区到 COLD）
- Journal 记录的是 **PAA 重映射**（不是 LBA→PAA 的变化）
- 搬完后归还 Pre-conditioned Pool

#### 4.8.5 WL 在 ftl_service 中的调度

```c
void ftl_service_phase3(ftl_context_t *ftl) {
    switch (ftl->mode) {
    case IDLE:
        l2p_cache_flush_all(ftl);
        gc_background(ftl, GC_IDLE_BUDGET);
        ftl_wear_leveling(ftl);                 // ✅ IDLE 全力做 WL
        journal_flush_page(ftl);
        cp_check(ftl);
        break;

    case NORMAL:
        l2p_cache_flush_background(ftl, 4);
        gc_background(ftl, GC_NORMAL_BUDGET);
        if (wl_should_trigger(ftl))
            wl_relocate(ftl, wl_select_victim(ftl));  // ✅ NORMAL 可执行
        break;

    case BUSY:
        l2p_cache_flush_background(ftl, 1);
        if (ftl->free_pool.emergency > 0)
            gc_emergency(ftl);
        /* WL 不执行，前台 IO 优先 */
        break;
    }
}
```

#### 4.8.6 对写放大（WA）的影响

静态 WL 增加写放大，但在企业级场景中代价极低：

```
TLC（3000~5000 P/E，~400GB 容量）：
  每次 WL：搬移 ~80 有效页（~320KB 写入）
  生命周期内：~200 次 WL → ~64MB 额外写入
  总写入能力：~1.2PB（100k per 4KB × 各因子）
  WL 贡献的 WA：< 0.0001% → 可忽略

当前 SLC（100k P/E，512MiB）：
  触发次数更少，WA 更可忽略

结论：静态 WL 对 WA 的影响微乎其微。
```

#### 4.8.7 参数建议

| 参数 | 建议值 | 说明 |
|------|--------|------|
| WL_DELTA_THRESHOLD | 5000 | max-min 擦除差值触发 |
| WL_MAX_AVG_RATIO | 3 | max/avg 擦除比 ≥ 3x 触发 |
| WL_LOW_ZONE_WIDTH | 1000 | 低擦除区宽度 |
| WL_LOW_BLOCK_PCT | 5 | 低区 Block 占比 ≥ 5% 触发 |
| WL_MIN_INTERVAL | 1000 writes | 两次 WL 最小间隔 |
| WL_MIN_VALID_RATIO | 70% | 只搬有效页 ≥ 70% 的冷 Block |
| WL_BUDGET_SB | 1 SB/cycle | 每轮最多搬 1 个 Super Block |

---

## 5. L2P 映射表设计

### 5.1 映射粒度

```
LBA 0x00000 ──→ CW 0 of Page X ──→ PBA(page, plane, block, page, cw=0)
LBA 0x00001 ──→ CW 1 of Page X ──→ PBA(page, plane, block, page, cw=1)
LBA 0x00002 ──→ CW 2 of Page X ──→ PBA(page, plane, block, page, cw=2)
LBA 0x00003 ──→ CW 3 of Page X ──→ PBA(page, plane, block, page, cw=3)
LBA 0x00004 ──→ CW 0 of Page X+1 ──→ ...
```

- 1 entry = `nand_pba_t`（uint32_t），`[CH:2][CE:2][PAA]`，PAA=`[CW:2][PAGE:6][PLANE:1][BLOCK:11]`
- 每 1 个 LBA 对应 1 个 CW（512B user data）
- **必须整页写入**：SLC NAND 不支持 partial page program，FTL 写 512B 的物理操作始终是整页（4 CW）写入

### 5.2 Region 分片

| 概念 | 值 | 说明 |
|------|----|------|
| Entry 大小 | 4 B | `nand_pba_t` |
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
  entry[0]   → PBA for LBA = region_id × 512 + 0
  entry[1]   → PBA for LBA = region_id × 512 + 1
  ...
  entry[511] → PBA for LBA = region_id × 512 + 511
```

### 5.3 Region 物理地址计算（通过 Metadata Directory）

动态分配下，Region 的物理位置通过 Metadata Directory 查询，
不再使用固定公式：

```c
/* 启动时加载 Metadata Directory */
metadata_directory_t g_metadata_dir;

/* 读取 LBA 所在 Region 的物理位置 */
static nand_paa_t l2p_region_ppa(uint16_t region_id) {
    /* ① 查 Directory：优先 SRAM 中的缓存 */
    metadata_entry_t *entry = metadata_dir_lookup(&g_metadata_dir,
                                                   METADATA_TYPE_L2P_REGION,
                                                   region_id);
    if (entry != NULL) {
        return entry->paa;   // 已有记录 → 直接返回物理 PAA
    }

    /* ② 首次访问（或 Directory 被 evict）：从 Free Pool 分配 */
    /*    偶数 region_id → Plane 0，奇数 → Plane 1（双 Plane 交错）*/
    uint8_t plane = (uint8_t)(region_id & 1U);

    nand_paa_t paa = metadata_dir_alloc_region(&g_metadata_dir,
                                                region_id, plane);
    return paa;
}

/* 分配新的元数据物理位置 */
nand_paa_t metadata_dir_alloc_region(metadata_directory_t *dir,
                                      uint16_t region_id,
                                      uint8_t plane) {
    /* 从 Free Pool 取块（元数据走 COLD 温度）*/
    uint16_t block = allocator_select_block(&g_alloc, FTL_TEMP_COLD, plane);

    /* 起始页 = 0（每次分配从 Page 0 开始顺序写入）*/
    nand_paa_t paa = paa_make(0, plane, block, 0, 0);

    /* 更新 Directory */
    metadata_dir_add(dir, METADATA_TYPE_L2P_REGION, region_id, paa);

    return paa;
}
```

### 5.4 LBA 分解

```c
uint16_t region_id  = lba >> 9;      // lba / 512 → 0..2047
uint16_t entry_idx  = lba & 0x1FF;   // lba % 512 → 0..511
```

### 5.5 Metadata Directory（元数据目录）

Metadata Directory 是动态元数据分配的核心数据结构，
SRAM 常驻（~1KB），启动时从 Anchor Block 加载。

#### 数据结构

```c
#define METADATA_DIR_ENTRIES    (128)

typedef struct {
    uint8_t   type;                // METADATA_TYPE_*
    uint16_t  id;                  // L2P region_id / 0 for others
    nand_paa_t paa;                // 物理 NAND 位置
    uint32_t  sequence;            // 最近更新时间戳
} metadata_dir_entry_t;             // 11 bytes

typedef struct {
    metadata_dir_entry_t entries[METADATA_DIR_ENTRIES];
    uint16_t             count;                // 有效 entry 数
    uint32_t             anchor_sequence;      // 当前 Anchor sequence
    bool                 dirty;                // 是否有未写回 Anchor 的变更
} metadata_dir_cache_t;             // SRAM 常驻 ~1.4KB
```

#### 核心操作

```c
/* 查找元数据位置 */
nand_paa_t metadata_dir_lookup(metadata_dir_cache_t *dir,
                                uint8_t type, uint16_t id) {
    for (int i = 0; i < dir->count; i++) {
        if (dir->entries[i].type == type && dir->entries[i].id == id)
            return dir->entries[i].paa;
    }
    return PAA_INVALID;   // 未找到
}

/* 添加/更新元数据位置 */
void metadata_dir_update(metadata_dir_cache_t *dir,
                          uint8_t type, uint16_t id, nand_paa_t new_paa) {
    /* 查找现有 entry（覆盖旧位置）*/
    for (int i = 0; i < dir->count; i++) {
        if (dir->entries[i].type == type && dir->entries[i].id == id) {
            dir->entries[i].paa = new_paa;
            dir->entries[i].sequence++;
            dir->dirty = true;
            return;
        }
    }
    /* 新增 entry */
    dir->entries[dir->count].type = type;
    dir->entries[dir->count].id = id;
    dir->entries[dir->count].paa = new_paa;
    dir->entries[dir->count].sequence = dir->anchor_sequence;
    dir->count++;
    dir->dirty = true;
}

/* 将目录写回 Anchor Block（满 64 变更或 Checkpoint 时调用）*/
void metadata_dir_flush_to_anchor(metadata_dir_cache_t *dir) {
    if (!dir->dirty) return;

    metadata_directory_t anchor;
    anchor.magic = 0x414E4449;    // "ANDIR"
    anchor.sequence = ++dir->anchor_sequence;
    anchor.entry_count = dir->count;
    for (int i = 0; i < dir->count; i++) {
        anchor.entries[i].type = dir->entries[i].type;
        anchor.entries[i].id = dir->entries[i].id;
        anchor.entries[i].paa = dir->entries[i].paa;
    }
    anchor.crc = crc32(&anchor, offsetof(typeof(anchor), crc));

    /* 写入 Anchor Block（Multi-Plane 双份冗余）*/
    fc_write_page(ANCHOR_BLOCK_PAA(anchor_page), &anchor, sizeof(anchor));
    fc_write_page(ANCHOR_BLOCK_PAA(anchor_page + 1), &anchor, sizeof(anchor));

    dir->dirty = false;
}
```

#### Anchor 同步触发条件

| 触发条件 | 说明 |
|---------|------|
| Directory 变更 ≥ 64 次 | 批次写入，减少 Anchor 擦写 |
| Checkpoint 时 | 与 L2P flush 同步 |
| ftl_service(IDLE) | 空闲时保证最新 |

#### 启动加载 Anchor

```c
int metadata_dir_load(metadata_dir_cache_t *dir) {
    /* 扫描两个 Anchor Block 各 3 个冗余页 */
    metadata_directory_t candidates[6];
    for (int i = 0; i < 6; i++) {
        fc_read_page(anchor_paa(i), &candidates[i], sizeof(metadata_directory_t));
    }

    /* 选 magic 正确 + CRC 正确 + sequence 最大的 */
    metadata_directory_t *best = NULL;
    for (int i = 0; i < 6; i++) {
        if (candidates[i].magic != 0x414E4449) continue;
        if (crc32(&candidates[i], offsetof(typeof(candidates[i]), crc))
            != candidates[i].crc) continue;
        if (!best || candidates[i].sequence > best->sequence)
            best = &candidates[i];
    }

    if (!best) return SSD_STATUS_IO_ERROR;  // 无有效 Anchor

    /* 重建 SRAM cache */
    dir->count = best->entry_count;
    dir->anchor_sequence = best->sequence;
    for (int i = 0; i < dir->count; i++) {
        dir->entries[i].type = best->entries[i].type;
        dir->entries[i].id = best->entries[i].id;
        dir->entries[i].paa = best->entries[i].paa;
    }
    dir->dirty = false;
    return SSD_STATUS_OK;
}
```

### 5.6 双 Plane Region 交错与 flush 加速

#### 背景

传统的 L2P region 写回固定在 Plane 0 操作，浪费 Multi-Plane 能力：

```
传统（单 Plane）:
  Plane 0: [R0][R1][R2][R3]...   64 次 flush = 64 × 300μs = 19.2ms

企业级（双 Plane 交错）:
  Plane 0: [R0][R2][R4][R6]...   ┐  32 次 Multi-Plane
  Plane 1: [R1][R3][R5][R7]...   ┘  = 32 × 300μs = 9.6ms
  → 写回速度翻倍！
```

#### Region 到 Plane 的映射

```c
/* Region 0 → Plane 0, Region 1 → Plane 1, Region 2 → Plane 0, ... */
static uint8_t region_to_plane(uint16_t region_id) {
    return (uint8_t)(region_id & 1U);
}
```

偶数 Region 固定在 Plane 0，奇数在 Plane 1，保证任意相邻两次 flush 可配对。

#### Cache Flush 的 Multi-Plane 优化

```c
/* 在 §6.2 的 Cache evict 和 background flush 中使用 */
void l2p_cache_flush_pair(l2p_cache_t *cache) {
    /* 找两个 DIRTY region，分别在 Plane 0 和 Plane 1 */
    int p0_idx = -1, p1_idx = -1;

    for (int i = 0; i < L2P_CACHE_SIZE; i++) {
        if (cache->lines[i].state != L2P_CACHE_DIRTY) continue;
        if (region_to_plane(cache->lines[i].region_id) == 0)
            p0_idx = i;
        else
            p1_idx = i;
        if (p0_idx >= 0 && p1_idx >= 0) break;
    }

    if (p0_idx >= 0 && p1_idx >= 0) {
        /* 两页同 page#，不同 plane → Multi-Plane PROGRAM */
        nand_paa_t paa0 = metadata_dir_lookup(&g_metadata_dir,
                           METADATA_TYPE_L2P_REGION,
                           cache->lines[p0_idx].region_id);
        nand_paa_t paa1 = metadata_dir_lookup(&g_metadata_dir,
                           METADATA_TYPE_L2P_REGION,
                           cache->lines[p1_idx].region_id);

        /* 确保 page# 相同且分属不同 plane */
        uint8_t page = allocator_get_next_metadata_page(
                         &g_alloc, 0, 1);

        /* 构造 Multi-Plane 命令 */
        fc_command_t *cmd = fc_command_alloc();
        cmd->config.operation = OP_MULTI_PLANE_PROGRAM_PAGE;
        // desc[0]: Plane0 region data
        // desc[1]: Plane1 region data
        fc_submit(cmd);

        cache->lines[p0_idx].state = L2P_CACHE_CLEAN;
        cache->lines[p1_idx].state = L2P_CACHE_CLEAN;

    } else if (p0_idx >= 0) {
        /* 只有一个 Plane 有 DIRTY → 单 Plane fallback */
        flush_single_region(cache, p0_idx);
    } else if (p1_idx >= 0) {
        flush_single_region(cache, p1_idx);
    }
}
```

#### 性能对比

| flush 方式 | 每次耗时 | 64 次 flush | 加速比 |
|-----------|---------|------------|--------|
| 单 Plane 逐次 | 300μs | 19.2ms | 1x |
| 双 Plane 交错 Multi-Plane | 300μs | 9.6ms | **2x** |
| 批量 4 条（每次 2 对） | 4×300μs | 4.8ms | **4x** |

#### Directory 与双 Plane 的联动

```
启动时：
  ① 读 Anchor → Metadata Directory → 知道每 Region 的位置
  ② 偶 region → P0, 奇 region → P1（交错分配）
  
写入时：
  flush(a) → metadata_dir_update(region_a, new_paa)
  flush(b) → metadata_dir_update(region_b, new_paa)
  
读回时：
  cache_miss(region_x) → metadata_dir_lookup(region_x) → 直接读
```

Metadata Directory 是双 Plane 交错的基础——没有 Directory 就不知道
Region 当前在哪，也就无法利用双 Plane 并行。

---

## 6. SRAM Cache 设计

### 6.1 数据结构

```c
#define L2P_ENTRIES_PER_REGION   (512)                     // region 内 PBA 数
#define L2P_REGION_SIZE          (L2P_ENTRIES_PER_REGION * sizeof(uint32_t))  // 2KB
#define L2P_REGIONS_TOTAL        (2048)                     // 总 region 数
#define L2P_CACHE_SIZE           (64)                       // 可配置：32/64/128
#define FTL_PBA_INVALID          (0xFFFFFFFFU)              // 无效 PBA，全 F 不可能为合法地址

typedef enum {
    L2P_CACHE_INVALID = 0,      // cache 行无效（冷启动或已驱逐），entries 全 FTL_PBA_INVALID
    L2P_CACHE_CLEAN,            // cache 行有效，未修改
    L2P_CACHE_DIRTY,            // cache 行有效，已修改（需写回）
    L2P_CACHE_FLUSHING,         // 正在写回 NAND（不可驱逐）
} l2p_cache_state_t;

typedef struct {
    nand_pba_t         entries[L2P_ENTRIES_PER_REGION];   // 512 条 PBA = 2KB
    uint16_t           region_id;                          // 0..2047
    uint8_t            state;                              // l2p_cache_state_t
    uint16_t           access_count;                       // CLOCK 访问计数（~0 时可被驱逐）
} l2p_cache_line_t;

typedef struct {
    l2p_cache_line_t lines[L2P_CACHE_SIZE];                // 64 × (2KB + 4B) ≈ 128 KB
    uint16_t         clock_hand;                           // CLOCK 替换指针
    uint32_t         hit_count, miss_count;                // 统计
} l2p_cache_t;
```

### 6.2 查找流程

Cache 对外只暴露 `l2p_cache_read()` 和 `l2p_cache_write()` 两个入口，
evict 和 region load 是内部实现，不对外暴露接口。

```
l2p_cache_read(cache, lba):
    region_id = lba / 512
    entry_idx = lba % 512

    ═══ Cache hit ═══
    遍历 lines:
      line->region_id == region_id 且 line->state >= L2P_CACHE_CLEAN
        → line->access_count++
        → return line->entries[entry_idx]

    ═══ Cache miss ═══
    CLOCK 逐 line 检查（从 clock_hand 开始循环查找）:
      state == FLUSHING   → 跳过（写回中，不可驱逐）
      access_count > 0    → access_count--，检查下一个
      state == DIRTY      → 选中为 victim（后续需 flush）
      state == INVALID    → 直接占用

    选中 victim line 后:
      victim 是 DIRTY → l2p_cache_flush_one(victim_idx) 写回 NAND（Step 2 不实现）
      l2p_cache_load_region(victim, region_id) 从 NAND 加载（Step 2 不实现，填 FTL_PBA_INVALID）
      victim->region_id = region_id
      victim->state     = L2P_CACHE_INVALID（Step 2 暂不置 CLEAN）
      victim->access_count = 1
      return victim->entries[entry_idx]（当 region 未加载时为 FTL_PBA_INVALID）
```

### 6.3 更新流程（Write-back）

```c
l2p_cache_write(cache, lba, pba) {
    // 查找或加载 region（同 l2p_cache_read 的 cache miss 逻辑）
    line = internal_find_or_load(cache, region_id);
    // 修改 entry
    line->entries[entry_idx] = pba;
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

Journal 使用**固定位置**存储在元数据区，不通过 Metadata Directory 管理：

```
元数据区 Block 分配（固定）：
  Block 0~31: L2P Table（2048 Region pages）
  Block 32:   BBT / Erase Count 等（后续实现）
  Block 33~34: Anchor Block（Metadata Directory）
  Block 35~36: 保留
  Block 37~38: Checkpoint（后续实现）
  Block 40:    Journal Block J0
  Block 41:    Journal Block J1

J0/J1 的双 PBA 在 journal_init() 中固定构造，启动时扫描 NAND 确定当前写入位置。
```

```
每次 L2P 修改前，先在 Journal 中追加一条记录：

┌──────────────────────────────────────────────────┐
│ Journal Entry（16B）                              │
│ ┌────────────────────────────────────────────┐   │
│ │ LBA (4B) | old_PBA (4B) | new_PBA (4B)    │   │
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
int ftl_write(ftl_context_t *ftl, lba_t lba, const void *data) {
    l2p_journal_entry_t entry;

    // 1. Allocator 分配新物理地址
    nand_pba_t new_pba = allocator_alloc(&ftl->alloc, FTL_TEMP_HOT);
    if (new_pba == NAND_PBA_INVALID) return SSD_STATUS_NO_SPACE;

    // 2. 读旧的 PBA（cache 查找 → 可能触发 cache miss）
    nand_pba_t old_pba = l2p_cache_read(&ftl->cache, lba);

    // 3. ★ 通过 FC 写数据到 NAND（异步提交，不等待完成）
    fc_command_t *cmd = fc_command_alloc();
    cmd->config.operation = OP_NORMAL_PROGRAM_PAGE;
    cmd->config.cw        = 3;
    cmd->config.crc       = FC_CRC_ENABLE;
    cmd->config.random    = FC_RANDOM_ENABLE;
    cmd->config.ldpc      = FC_ECC_MODE_BCH;
    cmd->config.module    = FC_COMMAND_MODULE_FTL;
    cmd->meta             = lba;                    // META 写入 LBA
    ftl_cmd_setup_ppa(cmd, new_pba);                // 配置 PBA + desc 链
    ftl_cmd_write_data(cmd, data, 2048);             // 用户数据拷贝
    fc_submit(cmd);                                  // 非阻塞提交

    // 4. ★ 写 Journal（NAND 确认后才安全）
    //    Journal 记录在 NAND page buffer 中，满页才真正写 NAND
    journal_append(&ftl->journal, lba, old_pba, new_pba);

    // 5. 更新 L2P Cache（标记 dirty）
    l2p_cache_write(&ftl->cache, lba, new_pba);

    // 命令在后续 ftl_service() 中完成，FTL 不需要立即等
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

### 7.5 Journal 回收（与 Checkpoint 协同）

#### 7.5.1 核心问题

Journal 是环形双 Block（J0 ↔ J1），写满后需要回切。
但旧 Block 中的条目在 Checkpoint 截断之前不能安全擦除：

```
时序：
  Phase 1: J0 写满 64 页（8192 条 entry，seq=1~8192）→ 切 J1
  Phase 2: J1 写满 → 需回切 J0
  Phase 3: J0 中还有 seq=1~8192 → 不能擦！
  Phase 4: 必须先 Checkpoint（sequence ≥ 8192）→ 截断 → 擦 J0 复用
```

**Checkpoint 的作用**：记录一个全局 sequence 号 checkpoint_seq，
Journal 中 sequence ≤ checkpoint_seq 的条目已写入 L2P table，可回收。

#### 7.5.2 触发条件（三级触发）

企业级 SSD（三星/铠侠）的 Journal 回收使用多级触发策略，
核心原则是**前台 IO 不能被后台任务阻塞**。

**条件 1：水位线触发（Journal 容量预警）**

由系统最坏情况 NAND 带宽预算推导：

```
已知：
  - Journal 容量：2 Block × 64 页 × 128 条 = 16,384 条
  - NAND PROGRAM：~300μs/页（批量 flush 4 条 dirty cache 约 1.2ms）
  - Checkpoint 需要 flush 最多 64 条 dirty cache
  - 最坏情况：BUSY 模式下 Checkpoint 被延迟

推导 85%（JOURNAL_FULL_THRESHOLD）：
  剩余空间 = 15% × 128 页 ≈ 19 页
  19 页 × 128 条 = 2,432 条 entry 的写入时间
  2,432 次 ftl_write 期间 ftl_service 有足够多轮次分批完成 Checkpoint
  → 85% 安全

推导 95%（JOURNAL_CRITICAL）：
  剩余空间 = 5% × 128 页 ≈ 6 页 = 768 条 entry
  768 次写入 ≈ 768 × 300μs ≈ 230ms
  这是最后防线——230ms 内必须完成 Checkpoint
  触发后在 BUSY 模式下也强制执行
```

```c
#define JOURNAL_FULL_THRESHOLD   (85)      // 水位线触发（%）
#define JOURNAL_CRITICAL         (95)      // 紧急水位（阻塞触发）

void journal_check_watermark(l2p_journal_t *j) {
    uint8_t used = (j->current_page * 100 +
                    j->current_entry * 100 / JOURNAL_ENTRIES_PER_PAGE)
                   / PAGES_PER_BLOCK;

    if (used >= JOURNAL_CRITICAL)
        j->emergency = true;            // 强制 Checkpoint，前台可能等待
    else if (used >= JOURNAL_FULL_THRESHOLD)
        j->checkpoint_needed = true;    // 设标志，后台执行
}
```

**条件 2：写入量触发（CP_WRITE_INTERVAL）**

由 Journal 容量与安全余量推导：

```
Journal 总容量 = 16,384 条（2 Block × 64 页 × 128 条）
CP_WRITE_INTERVAL = 4,096 次
  4,096 × 4 = 16,384 ← 刚好 Journal 满
  留 4 倍余量：Journal 至少可承受 4 次 Checkpoint 失败后写满

消费级 DRAM-less 典型值 1,024~4,096 writes。
企业级（有 DRAM Journal buffer）可达 100k~1M。
```

```c
#define CP_WRITE_INTERVAL        (4096)    // 写入量触发
```

**条件 3：定时触发（CP_TIME_INTERVAL_MS）**

Host 写入量很小时，条件 2 不会触发，但 Journal 中可能有少量残留。
定时 Checkpoint 保证下次启动回放量有限：

```
场景：
  Host 每 30 秒写 1 条数据
  条件 2 永远不会触发（4096 条写入要 3.4 小时）
  Journal 可能积累几百条
    
定时触发（10 秒）：
  即使只写了几条，也会 Checkpoint
  下次启动 Journal 几乎为空
```

```c
#define CP_TIME_INTERVAL_MS      (10000)   // 定时触发（10 秒）
```

#### 7.5.3 Checkpoint 执行流程

```c
void ftl_checkpoint(ftl_context_t *ftl) {
    /* ① flush 所有 dirty cache → L2P table（NAND 元数据区）*/
    l2p_cache_flush_all(&ftl->cache);

    /* ② 写 Checkpoint header（含当前 sequence）*/
    l2p_checkpoint_header_t cp;
    cp.sequence = ftl->journal.sequence_counter;
    cp.journal_sequence = cp.sequence;
    cp.timestamp = timer_get_ms();
    cp.region_bitmap[...] = ...;        // 增量 checkpoint 只需 bitmap
    checkpoint_write(&cp);

    /* ③ 截断 Journal */
    journal_truncate(&ftl->journal, cp.sequence);

    /* ④ 擦除已回收的 Journal Block */
    // journal_truncate 内部处理擦除

    ftl->cp_sequence = cp.sequence;
    SSD_LOGI("[CP] seq=%u done", cp.sequence);
}
```

#### 7.5.4 在 ftl_service 中的决策

```c
void ftl_cp_check(ftl_context_t *ftl) {
    l2p_journal_t *j = &ftl->journal;

    /* ① 紧急水位 → 强制 Checkpoint */
    if (j->emergency) {
        ftl_checkpoint(ftl);
        j->emergency = false;
        return;
    }

    /* ② 写入量触发 */
    if (ftl->total_writes % CP_WRITE_INTERVAL == 0) {
        ftl_checkpoint(ftl);
        return;
    }

    /* ③ 定时触发（仅 IDLE/NORMAL，BUSY 跳过）*/
    if (ftl->mode != BUSY &&
        timer_elapsed_ms(ftl->cp_timer) >= CP_TIME_INTERVAL_MS) {
        ftl_checkpoint(ftl);
        timer_reset(ftl->cp_timer);
        return;
    }

    /* ④ 水位线标志（由 journal_check_watermark 在 append 时设置）*/
    if (j->checkpoint_needed && ftl->mode != BUSY) {
        ftl_checkpoint(ftl);
        j->checkpoint_needed = false;
        return;
    }
}
```

企业级"无阻塞原则"总结：

| 条件 | 执行模式 | 是否阻塞前台 IO |
|------|---------|----------------|
| 水位线（85%） | NORMAL / IDLE | 否 |
| 写入量（4096） | NORMAL / IDLE | 否 |
| 定时（10s） | NORMAL / IDLE | 否 |
| 紧急（95%） | 任何模式 | ⚠️ 可能阻塞 |

#### 7.5.5 参数汇总

| 参数 | 值 | 推导依据 |
|------|----|---------|
| CP_WRITE_INTERVAL | 4,096 writes | Journal 容量 16,384 条，4 倍余量 |
| JOURNAL_FULL_THRESHOLD | 85% | 剩余 19 页够分批做完 Checkpoint |
| JOURNAL_CRITICAL | 95% | 剩余 6 页，230ms 最后防线 |
| CP_TIME_INTERVAL_MS | 10,000 ms | Host 静负载保底 |
| FLUSH_BUDGET (NORMAL) | 4 line/轮 | 4 × 300μs ≈ 1.2ms，不阻塞前台 |
| FLUSH_BUDGET (IDLE) | 全部 | NAND 空闲，全速 flush |

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
│  ├── entry[0]: lba(4B) | old_pba(4B) | new_pba(4B)     │
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

本文档的 Boot 框架（`firmware_boot_stage_storage`）提供了启动阶段入口，
FTL 的恢复在此阶段完成。

#### 8.3.1 核心矛盾

Checkpoint 保存的是 L2P 全表（2048 regions = 4MB），
而 SRAM cache 只有 64 个 line（~131KB）。
**不能预加载全表，必须按需加载**。

因此 Checkpoint 的作用不是"把全表搬到 SRAM"，
而是**标记 Journal 回放的起点**——Journal 中 sequence ≤ cp_seq 的条目
已通过后台 flush 写入了 NAND L2P 表，无需回放。

#### 8.3.2 关机类型判断

```c
int ftl_boot_recovery(ftl_context_t *ftl) {
    /* 1. 扫描 Checkpoint 区域 → 最新 header */
    l2p_checkpoint_header_t cp;
    int cp_found = find_latest_checkpoint(&cp);

    /* 2. 扫描 Journal → 获取最后 sequence */
    uint32_t journal_last_seq = journal_get_last_sequence(&ftl->journal);

    /* 3. 判断关机类型 */
    if (!cp_found) {
        return boot_fresh_start(ftl);          // 出厂首次启动
    }

    if (journal_last_seq <= cp.journal_sequence) {
        return boot_clean_start(ftl, &cp);     // 干净关机
    } else {
        return boot_power_failure_recovery(ftl, &cp);  // 异常掉电
    }
}
```

#### 8.3.3 出厂首次启动（Fresh Start）

```c
int boot_fresh_start(ftl_context_t *ftl) {
    l2p_cache_init(&ftl->cache);               // Cache 全 INVALID
    allocator_init(&ftl->alloc);               // 扫描 BBT、构建 free pool
    bbt_load(&ftl->bbt);
    erase_count_load(&ftl->alloc);
    journal_init(&ftl->journal);               // Journal 空
    ftl->cp_sequence = 0;
    return SSD_STATUS_OK;
}
```

启动时间 ≈ BBT 扫描（250ms）+ Allocator（50ms）≈ **~300ms**。

#### 8.3.4 干净关机启动（Clean Start）

条件：Journal 中所有条目的 sequence ≤ Checkpoint 的 journal_sequence，
即 Journal 已全部回放到 L2P table，无需回放。

```c
int boot_clean_start(ftl_context_t *ftl, l2p_checkpoint_header_t *cp) {
    /* ① Cache 全 INVALID（不预加载任何 region）*/
    l2p_cache_init(&ftl->cache);

    /* ② Allocator 初始化 */
    allocator_init(&ftl->alloc);               // 扫描 BBT
    allocator_rebuild_sb_list(&ftl->alloc);    // 重建 Super Block 列表
    allocator_recover_write_pointer(&ftl->alloc);  // 恢复写指针

    /* ③ BBT 加载 */
    bbt_load(&ftl->bbt);

    /* ④ Erase Count 表加载 */
    erase_count_load(&ftl->alloc);

    /* ⑤ Controller → Ready */
    ftl->cp_sequence = cp->sequence;

    SSD_LOGI("[Boot] clean: free=%u, SB next_page=%d",
             ftl->alloc.free_count, ftl->alloc.open_sb.next_page);
    return SSD_STATUS_OK;
}
```

**关键设计**：Cache 全空，首次 Host IO 触发的 cache miss 才从 NAND
按需加载对应的 Region。不需要预加载 Checkpoint 中的数据。

启动时间 ≈ BBT 扫描（250ms）+ Allocator 重建（50ms）+ 写指针恢复（2ms）
≈ **~350ms**。

#### 8.3.5 异常掉电恢复（Power Failure Recovery）

条件：Journal 有未回放到 L2P table 的条目（sequence > cp_seq）。

```c
int boot_power_failure_recovery(ftl_context_t *ftl,
                                 l2p_checkpoint_header_t *cp) {
    /* ① Cache 全 INVALID */
    l2p_cache_init(&ftl->cache);

    /* ② Journal 批量回放 */
    journal_replay_batched(&ftl->journal, &ftl->cache,
                            cp->journal_sequence);

    /* ③ Allocator 初始化 + 写指针扫描恢复 */
    allocator_init(&ftl->alloc);
    allocator_rebuild_sb_list(&ftl->alloc);
    allocator_scan_write_pointer(&ftl->alloc);   // 异常掉电需扫描 NAND

    /* ④ Controller → Ready */
    ftl->cp_sequence = cp->sequence;

    /* Cache 中的 DIRTY lines 由后台 flush 逐步写回 NAND L2P 表 */
    SSD_LOGI("[Boot] power failure: %u journal entries replayed",
             ftl->journal.replayed_count);
    return SSD_STATUS_OK;
}
```

#### 8.3.6 Journal 批量回放优化

异常掉电时 Journal 可能包含数千条未回放条目。逐条回放会导致
严重的 cache 颠簸——每次 LBA 变更都可能触发 region load/evict/flush。

优化策略：按 Region 分组批量回放。

```c
#define JOURNAL_BATCH_SIZE  (2048)    // SDRAM 中暂存

int journal_replay_batched(l2p_journal_t *journal, l2p_cache_t *cache,
                            uint32_t since_seq) {
    /* ① 把需要回放的 entry 读到 SDRAM buffer */
    l2p_journal_entry_t batch[JOURNAL_BATCH_SIZE];
    int count = journal_read_entries_since(journal, since_seq,
                                           batch, JOURNAL_BATCH_SIZE);

    /* ② 按 region_id 排序（同 Region 的聚在一起）*/
    sort_by_region(batch, count);

    /* ③ 逐 Region 回放：每个 Region 只加载一次 */
    int i = 0;
    while (i < count) {
        uint16_t cur_region = lba_to_region(batch[i].lba);

        /* 加载这个 Region（可能触发 evict + flush）*/
        l2p_cache_line_t *line = l2p_cache_find_or_load(cache, cur_region);

        /* 应用该 Region 的所有条目 */
        while (i < count && lba_to_region(batch[i].lba) == cur_region) {
            line->entries[batch[i].lba & 0x1FF] = batch[i].new_paa;
            line->state = L2P_CACHE_DIRTY;
            i++;
        }
    }

    SSD_LOGI("[Journal] batch replay: %d entries in ~%d regions",
             count, count / 32 + 1);
    return count;
}
```

**优化效果**：回放 2048 条条目，最多只需 64 次 Region load。
平均每个 Region 含 ~32 条，比逐条回放的 2048 次 cache 操作减少 32 倍。

#### 8.3.7 写指针恢复（Write Pointer Recovery）

异常掉电时，Open Super Block 写到第 N 页时掉电，不知道当前写到哪一页。

```c
void allocator_scan_write_pointer(ftl_allocator_t *alloc) {
    super_block_t *sb = &alloc->hot_open_sb;
    uint8_t page_buf[16];         // 只读 16B 就够了

    for (int page = 0; page < PAGES_PER_BLOCK; page++) {
        for (int plane = 0; plane < PLANE_NUM; plane++) {
            nand_paa_t paa = paa_make(page, plane, sb->blocks[plane], 0, 0);

            fc_read_page_raw(paa, page_buf, 16);   // RAW 读，跳过 ECC

            if (is_all_ff(page_buf, 16)) {
                /* 找到第一个空白页 */
                sb->next_page = page;
                SSD_LOGI("[Boot] write pointer: SB next_page=%d", page);
                return;
            }
        }
    }
}
```

干净关机时写指针由 allocator 的 next_page 在内存中跟踪，
不需要扫描 NAND（`allocator_recover_write_pointer` 直接取内存值）。

#### 8.3.8 启动时间总结

| 阶段 | 干净关机 | 异常掉电 | 说明 |
|------|---------|---------|------|
| FW Stage: PLATFORM | ~10ms | ~10ms | 平台初始化 |
| FW Stage: MEMORY | ~5ms | ~5ms | 内存初始化 |
| FW Stage: IMAGE | ~2ms | ~2ms | 镜像验证 |
| FC Init + NAND Probe | ~50ms | ~50ms | FC 层初始化 |
| BBT 扫描 | ~250ms | ~250ms | 全量 4096 Block |
| Checkpoint 扫描 | ~1ms | ~1ms | 读最新 header |
| Journal 扫描 | ~1ms | ~1ms | 读最后 sequence |
| Journal 回放 | 0ms | ~100ms | 仅异常掉电需要 |
| Allocator 重建 | ~50ms | ~50ms | Super Block 列表 |
| Write Pointer | ~2ms | ~5ms | 干净取内存值，异常需扫描 |
| FW Stage: HOST | ~5ms | ~5ms | Host 初始化 |
| **总计** | **~370ms** | **~480ms** | |

异常掉电恢复后，Cache 中的 DIRTY lines 由后台 flush 逐步写回，
不阻塞 Controller → Ready。

#### 8.3.9 参数建议

| 参数 | 建议值 | 说明 |
|------|--------|------|
| BOOT_CLEAN_TIME | ~370ms | 干净关机启动时间目标 |
| BOOT_DIRTY_TIME | ~500ms | 异常掉电恢复时间目标 |
| JOURNAL_BATCH_SIZE | 2048 entries | 批量回放 buffer（SDRAM）|
| WRITE_PTR_SCAN_GRANULE | 16 bytes | 写指针扫描每次读取大小 |

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

### ADR-11：坏块管理（BBT）— Block Sparing

| 项目 | 决策 |
|------|------|
| 选型 | **出厂全量扫描 + 运行时隔离 + Block Sparing（Spare Pool 保水位）** |
| 理由 | 学习企业级坏块替换做法；Spare Pool 保证运行时坏块不减少用户容量；出厂坏块扫描 250ms 可接受；Sparing 不需要 block remap 表——只补充 free pool，不做映射 |
| 拒绝方案 | Block Skip（容量自然减少，不符合企业级学习目标）；Block Remap Table（行业不用两层映射）|
| 代价 | 出厂扫描增加启动时间 ~250ms；SPARE_POOL_SIZE × 1 Block 容量占用；需要 FC 支持 OOB/Spare 区读取 |

**与 §3.6 OP 的关系**：Spare Pool 替换坏块，OP 池负责 GC 缓冲和性能保速，二者职责分离。

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
    /* 拓扑参数 */
    uint8_t  ch, ce, lun;

    /* L2P Cache */
    l2p_cache_t         cache;             /* SRAM L2P cache（~132 KB）*/

    /* Allocator */
    ftl_allocator_t     alloc;             /* 空闲 block / Super Block 写指针 */

    /* Journal */
    l2p_journal_t       journal;           /* log-structured journal */

    /* Metadata Directory */
    metadata_dir_cache_t metadata_dir;     /* 元数据位置追踪 */

    /* GC 计时 */
    uint32_t            gc_last_run_writes;
    uint32_t            gc_timer_start;

    /* 统计 */
    uint32_t            total_writes;
} ftl_context_t;
```

### 11.2 FTL API

```c
/* ── 初始化 ── */
int ftl_init(ftl_context_t *ftl);
// 初始化 L2P cache → allocator → journal → checkpoint 区域

/* ── 读写（用户数据）── */
int ftl_read(ftl_context_t *ftl, lba_t lba, void *data, uint32_t count);
// L2P cache lookup → FC read → memcpy to data buffer
// cache miss 时自动从 NAND 加载 region

int ftl_write(ftl_context_t *ftl, lba_t lba, const void *data, uint32_t count);
// allocator alloc → FC program → journal append → cache write (dirty)

int ftl_trim(ftl_context_t *ftl, lba_t lba, uint32_t count);
// L2P entry → FTL_PBA_INVALID, 标记 bitmap 无效

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
nand_pba_t l2p_cache_read(l2p_cache_t *cache, lba_t lba);
void       l2p_cache_write(l2p_cache_t *cache, lba_t lba, nand_pba_t pba);
void       l2p_cache_flush_one(l2p_cache_t *cache, int idx);
void       l2p_cache_flush_all(l2p_cache_t *cache);
void       l2p_cache_flush_background(l2p_cache_t *cache, int max_count);
```

**Journal**（`journal.h`）：

```c
/* Journal 使用固定数据区 Block 40~41 */
#define JOURNAL_BLOCK_COUNT     (2)
#define JOURNAL_BLOCK0          (40)
#define JOURNAL_BLOCK1          (41)

typedef struct {
    uint32_t    lba;            // 修改的 LBA
    nand_pba_t  old_pba;        // 修改前的 PBA（FTL_PBA_INVALID=新写入）
    nand_pba_t  new_pba;        // 修改后的 PBA
    uint32_t    sequence;       // 单调递增序号
} __attribute__((packed)) l2p_journal_entry_t;

typedef struct {
    /* 固定参数 */
    uint8_t     ch;             // CH
    uint8_t     ce;             // CE
    uint8_t     lun;            // LUN
    nand_pba_t  block_pba[2];   // J0/J1 的 PBA 基址（page=0, cw=0）

    /* 写入指针 */
    uint8_t     current_block;  // 0 or 1（当前写入的 journal block）
    uint8_t     current_page;   // 0..63
    uint8_t     current_entry;  // 0..127
    uint32_t    sequence_counter;

    /* SRAM buffer：攒满 128 条才写 NAND */
    uint8_t     page_buf[2048]; // 1 NAND page = 4 CW × 512B

    /* 水位 */
    uint8_t     checkpoint_needed;
    uint8_t     emergency;
} l2p_journal_t;

void journal_init(l2p_journal_t *journal, uint8_t ch, uint8_t ce, uint8_t lun);
void journal_append(l2p_journal_t *journal, lba_t lba, nand_pba_t old, nand_pba_t new);
void journal_flush_page(l2p_journal_t *journal);
int  journal_replay(l2p_journal_t *journal, l2p_cache_t *cache);
void journal_truncate(l2p_journal_t *journal, uint32_t seq);
```

**Allocator**（`allocator.h`）：

```c
/* 温度等级 */
#define FTL_TEMP_HOT    0
#define FTL_TEMP_COLD   1
#define FTL_TEMP_NUM    2

/* 三级 Free Pool 水位 */
#define FREE_POOL_TARGET      (32)     /* Pre-conditioned 目标水位 */
#define FREE_POOL_EMERGENCY   (8)      /* 紧急水位 */
#define FTL_OP_PERCENT        (10)     /* OP 预留百分比 */
#define FTL_SPARE_POOL_SIZE   (50)     /* 坏块替换池大小 */

/* Super Block 列表容量：每 Plane 2048 Block，最多 1024 个 Super Block 配对 */
#define SUPER_BLOCK_LIST_MAX  (1024)

/* 单温度 Open Super Block */
typedef struct {
    super_block_t *sb;                  /* 当前写入的 Super Block */
} ftl_open_sb_t;

/* Allocator 上下文 */
typedef struct {
    /* ── 固定参数 ── */
    uint8_t  ch;                        /* 本 Allocator 所属 CH（当前固定 0）*/
    uint8_t  ce;                        /* 本 Allocator 所属 CE（当前固定 0）*/
    uint8_t  lun;                       /* 本 Allocator 所属 LUN（当前固定 0）*/
    uint16_t total_blocks;             /* 总好 Block 数（出厂扫描后确定）*/

    /* ── Free Pool（三级） ── */
    uint16_t *free_blocks;              /* free block 编号数组 */
    uint16_t  free_count;               /* 空闲 block 数 */
    uint16_t  pre_cond_count;           /* 预擦除池 Block 数 */
    uint16_t  emergency_count;          /* 紧急池 Block 数 */

    /* ── Open Super Block（每温度一个） ── */
    super_block_t  hot_sb;              /* Hot Open Super Block */
    super_block_t  cold_sb;             /* Cold Open Super Block */

    /* ── Closed Super Block 列表（等待 GC） ── */
    super_block_t *closed_sb_list;      /* 已写满的 Super Block 数组 */
    uint16_t       closed_sb_count;     /* 已关闭 Super Block 数 */

    /* ── 磨损信息 ── */
    uint32_t *erase_counts;             /* 每 Block 擦除次数 */

    /* ── 坏块 ── */
    uint16_t *bad_blocks;               /* 坏 Block 编号数组 */
    uint16_t  bad_count;                /* 坏 Block 数 */
    uint16_t  spare_pool_count;         /* Spare Pool 剩余 Block 数 */
} ftl_allocator_t;

int        allocator_init(ftl_allocator_t *alloc, uint8_t ch, uint8_t ce, uint8_t lun, uint8_t op_percent);
nand_pba_t allocator_alloc(ftl_allocator_t *alloc, uint8_t temperature);
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
│  lba    = lba (写入 META)           │ lba    = 不填          │
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
    cmd->lba                 = lba;               // 写入 META

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

### 13.4 2026-09-28 接口层设计决策

本节记录 2026-09-28 讨论中确认的 FTL 接口层设计决策：

| # | 决策项 | 结论 | 说明 |
|---|--------|------|------|
| D1 | LBA 粒度 | **512B** | 一个 CW 的 USER 数据区（FC_BUFFER_USER_SIZE），CW 大小 = PAGE_SIZE / 4 |
| D2 | 几何模型 | **统一到 nand_config.h** | 废弃 `config/nand_geometry.h`，FTL 直接用 nand_config.h 的宏 |
| D3 | 地址类型 LBA | `lba_t` = **uint32_t** | Host 逻辑地址，传入 cmd->lba 用于 META 校验；替代 `ssd_lba_t`（uint64_t） |
| D4 | 地址类型 PBA | `pba_t` = **uint32_t** | 完整物理地址，L2P 映射的目标值，含拓扑（CH/CE/LUN）+ 芯片内地址（PAA）|
| D5 | 地址类型 PAA | `nand_paa_t` = **uint32_t** bitfield | 芯片内地址 [CW:2][PAGE:6][PLANE:1][BLOCK:11]；替代 `ssd_ppa_t`（uint64_t）|
| D5 | FTL 提交模型 | **非阻塞异步提交** | ftl_submit_*() 立即返回，ftl_service() 驱动完成 |
| D6 | 完成通知 | **module 路由** | fc_completion.module 区分来源，FTL 内部自行处理 |
| D7 | FTL→FC 访问 | **命令构造 helper**（不是隔离层） | 通过 `ftl_fc_interface.h` 的 `ftl_cmd_setup_*()`，不绕过 FC |
| D8 | 命名 | **ftl_fc_interface** | 头文件 `include/ftl/ftl_fc_interface.h`，源文件 `src/ftl/ftl_fc_interface.c` |
| D9 | RTOS | **暂不加** | 裸机超级循环 + 中断，存储主控标准做法 |
| D10 | Host 命令来源 | **UART Shell CLI** | 通过 read/write/trim 命令模拟 NVMe IO |
| D11 | Host 非阻塞 | **pending 请求表** | UART 来的命令入 pending 表，FTL 异步完成后再输出结果 |

#### 13.4.1 地址体系总结

##### 行业标准缩写定义

| 缩写 | 全称 | 阶段 | 用途 | 你的代码中对应 |
|------|------|------|------|--------------|
| LAA | Logical Application Address | Host → Controller | 应用程序下发的原始逻辑地址 | UART CLI 的 `lba` 参数（NVMe 场景 LAA = LBA）|
| LBA | Logical Block Address | L2P 索引 | 行业标准逻辑地址，`cmd->lba` 用于 META 校验 | `cmd->lba` |
| PBA | Physical Block Address | L2P 映射值 | 完整物理地址，含拓扑路由（CH/CE/LUN）+ 芯片内地址（PAA）| FTl 层 `pba_t`（需新增）|
| PAA | Physical Array Address | NAND 芯片寻址 | 芯片内地址（Block + Plane + Page + CW），不含拓扑 | `nand_paa_t` |
| FAA | （本项目曾用） | — | 原代码中 `cmd->faa` 曾用名，现统一为 LBA | 已废弃 |

##### 地址转换流程（行业标准 LBA → PBA → PAA）

```
Host（UART / NVMe）
  │ LAA = Application 下发的地址（= LBA，1:1）
  ▼
FTL
  │ LBA → L2P → PBA（完整物理地址）
  │ PBA(uint32_t) = [CH:?][CE:?][LUN:?][PAA]
  │                = [CH:?][CE:?][LUN:?][BLOCK:11][PLANE:1][PAGE:6][CW:2]
  │
  │ FTL 从 PBA 拆出：
  │   cmd->config.ch/ce/lun = PBA 高位（路由到哪个 NAND 芯片）
  │   cmd->paa              = PBA & PAA_MASK = PAA（芯片内地址）
  ▼
FC → NAND Dispatch → Engine → FMC
  │ cmd->config.ch/ce/lun → 选中目标 NAND 芯片
  │ cmd->paa(PAA)          → ONFI 行地址周期，发给芯片
  ▼
NAND Chip（由 CH/CE/LUN 选中的那颗芯片）
```

##### 地址编码关系

```
PBA(uint32_t):
  ┌──────────┬──────────┬──────────┬────────────────────────────┐
  │ CH(2b)   │ CE(2b)   │ LUN(2b)  │     PAA(20b)               │
  │ 路由信息  │ 路由信息  │ 路由信息  ├─────┬──────┬──────┬──────┤
  │          │          │          │ CW │ PAGE │PLANE│BLOCK │
  │          │          │          │ 2b │  6b  │ 1b  │ 11b  │
  └──────────┴──────────┴──────────┴────┴──────┴──────┴──────┘

cmd->lba = Host 逻辑地址（用于 META 校验，不参与地址路由）
cmd->paa = PAA（芯片内地址）= PBA & PAA_MASK
```

#### 13.4.2 FTL 主循环形态

```c
void main(void)
{
    platform_init();
    fc_init();
    ftl_init(&g_ftl);

    while (1)
    {
        uart_service_poll();     /* 收 Host 命令 → 入 pending 表 */
        ftl_service(&g_ftl);     /* 驱动 FTL + FC + 后台任务 */
        check_pending_done();    /* 完成 → UART 打印结果 */
    }
}
```

---

## 14. 未解决问题

| # | 问题 | 影响 | 状态 |
|---|------|------|------|
| 1 | Region 物理存储：固定 vs 动态元数据 block | 固定简单但磨损集中 | ✅ 已解决（见 §3.3/§5.5）：Metadata Directory + Anchor Block 动态分配，磨损均匀分布 |
| 2 | Journal 可靠性：journal 本身怎么防掉电 | journal page program 到一半掉电 | ✅ 四层保护（见 §7.6） |
| 3 | 启动加载策略：加载 checkpoint 全表还是按需 miss | 启动延迟 vs 首次访问延迟 | ✅ 已解决（见 §8.3）：干净关机按需 miss，异常掉电批量回放 |
| 4 | Journal 回收时机 | 需要确保 L2P 表已同步 | ✅ 已解决（见 §7.5）：三级触发（水位线 85%/95% + 写入量 4096 + 定时 10s） |
| 5 | L2P 表磨损：元数据 block 擦除是否超过数据 block | 寿命不均衡 | ✅ 有余量（见 §14.1）：动态分配后磨损均匀分布，Anchor 几乎无磨损 |
| 6 | 多 Plane 下 region 分布：是否跨 plane 并行写入 | L2P 写回性能 | ✅ 已解决（见 §5.6）：偶/奇 Region 交错 Plane 0/1，Multi-Plane flush 并行，速度翻倍 |

### 14.1 L2P 表磨损均衡分析

#### 负载模型

```
NAND 规格：SLC, 100k P/E, 512MiB
L2P 表：32 blocks × 64 pages = 2,048 页（日志结构写入）
Cache：64 line，每条 DIRTY 触发一次 Region flush（写 1 页）
全盘写 = 1,048,576 次 LBA 写入
```

#### L2P 表磨损推导

每次全盘写产生的 Region flush 次数：

```
Cache 64 line，全盘随机写：
  64 次 Write → Cache 满 → evict 1 个 DIRTY Region → flush 1 页
  1,048,576 / 64 = 16,384 次 evict flush
  加上 background flush / checkpoint flush ≈ 2,000 次
  合计 ≈ 18,000 次 Region flush / 全盘写
```

元数据区 GC 频率：

```
元数据区总页数 = 32 × 64 = 2,048 页
每次元数据 GC = 所有 32 blocks 擦除一次（压缩后重新分布）
GC 频率 = 18,000 / 2,048 ≈ 8.8 次 / 全盘写
每个 metadata block 擦除次数 = 8.8 次 / 全盘写
寿命内全盘写 = 100,000 / 8.8 ≈ 11,363 次
总写入量 = 11,363 × 512MiB ≈ 5.68 TB
```

#### 与数据区寿命对比

| 区域 | 寿命（TB 写入） | 说明 |
|------|---------------|------|
| User + OP | ~12.98 TB | 4056 block × 3.2GB/block |
| **L2P 表（32 blocks）** | **~5.68 TB** | 数据区的 44%，仍有余量 |
| **Journal（2 blocks）** | **~800 GB** | ⚠️ 最短的瓶颈 |

#### 每日写入 vs 实际寿命

| 每日写入 | L2P 表寿命 | Journal 寿命 |
|---------|-----------|-------------|
| 10 GB（极端） | 1.6 年 | 80 天 ❌ |
| 1 GB（较忙） | 15.6 年 | 2.2 年 ⚠️ |
| 100 MB（学习/开发） | 156 年 ✅ | 22 年 ✅ |

#### 结论

**L2P 表（32 blocks）在学习和开发场景下有余量。**
实际消耗比最坏模型更低——顺序写入的 cache 命中率高、批量 flush 合并更新、Journal 重复 LBA 会被截断。

**Journal 是真正的瓶颈**（2 blocks 仅 ~800GB），但容易解决：

```
JOURNAL_BLOCKS: 2 → 4
→ Journal 寿命从 800GB 翻倍到 1.6TB
→ 10GB/天场景下从 80 天到 160 天

如果还不够：JOURNAL_BLOCKS → 8，寿命 3.2TB
代价仅仅是多占 6 个好 block（OP 池 10% = ~400 block 中占极小比例）
```

#### 缓解措施（可选实现）

- **Region 更新合并**：多次写同一 Region 的 LBA 合并为一次 flush
- **Checkpoint 批量写**：Checkpoint 统一更新多个 Region，分摊元数据 GC 开出
- **增大 JOURNAL_BLOCKS**（推荐，最简单）

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
| Bad Block 管理 | 出厂全量扫描 + Block Sparing（Spare Pool） | 企业级标准做法 | ⚠️ 需要 FC OOB 接口 |
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
BBT Sparing ✅    Block Skip                     Stripe Skip + Sparing
无掉电电容 ✅     无掉电电容                      有掉电电容
三级主循环 ✅     类似                            硬件时间预算调度器
```

**结论**：本方案与消费级 DRAM-less SSD 主流做法一致，定位合理。没有"为了炫技而做的无用功能"，也没有"当前硬件无法支持的设计"。唯一的缺口是 FC 层的 OOB 读写接口。

### 15.5 实施顺序与当前状态

#### 已实现（2026-09-29 最终状态：FTL 全部模块完成 + Completion Table 统一路由 + Region 动态分配）

```
Step 1 — ftl_fc_interface 层 + 地址体系
  ├── include/ftl/ftl_fc_interface.h      ✅ ftl_cmd_setup_ppa / write_data / read_data
  ├── src/ftl/ftl_fc_interface.c          ✅ + ftl_internal_read/write/erase/sync_wait + read_raw
  ├── include/ftl/ftl_type.h              ✅ lba_t / ftl_status_t 类型
  │                                       ✅ 企业级两值枚举 FTL_OK=0 / FTL_ERROR=-1
  │                                       ✅ 删除 pba_t（已由 nand_pba_t 替代）
  ├── include/nand/nand_config.h          ✅ PBA/PAA 全套 40+ inline 函数
  └── 废弃: firm_boot / l2p.h / garbage_collection / nand.h / nand_geometry.h 已清理

Step 2 — L2P Cache + CLOCK + 真实 NAND 读写
  ├── include/ftl/l2p_cache.h             ✅ 数据结构 + md_dir/alloc 指针 + l2p_region_pba()
  └── src/ftl/l2p_cache.c                 ✅ Region load（读 NAND）+ flush（写 NAND）
                                           ✅ 通过 Metadata Directory 动态定位（含动态分配）
                                           ✅ l2p_cache_set_md_dir 增加 alloc 指针
                                           ✅ flush_one 首次写 Region → metadata_dir_alloc_region

Step 3 — Allocator + Super Block
  ├── include/ftl/allocator.h             ✅ 三级 Free Pool + Super Block 配对
  └── src/ftl/allocator.c                 ✅ 磨损均衡（pre_cond 同步 free_count）+ needs_gc
                                           ✅ allocator_close_sb → Closed 列表

Step 4 — Journal
  ├── include/ftl/journal.h               ✅ 固定 Block 40~41 双 Block 环形
  ├── src/ftl/journal.c                   ✅ flush_page（NAND 写）+ replay（扫描回放）
  │                                       ✅ Header [magic+page_seq+entry_count+block_seq]
  │                                       ✅ journal_truncate（擦除非当前 Block）
  └── 开放常量移至 .h                      ✅ JOURNAL_MAGIC / JOURNAL_HEADER_SIZE

Step 5 — GC + ftl_service 主循环
  ├── include/ftl/gc.h                    ✅ gc_migrate_sb 增加 journal 参数
  ├── src/ftl/gc.c                        ✅ 完整 7 步搬移（读 META→查 L2P→分配→写→L2P→Journal）
  │                                       ✅ gc_read_page_with_lba（保持 FC 命令存活读 META）
  │                                       ✅ gc_erase_and_free（真实擦除 + 失败标记坏块）
  ├── include/ftl/ftl.h                   ✅ ftl_context_t + 全部子模块字段
  └── src/ftl/ftl.c                       ✅ submit_read/write（异步）+ read/write/trim（同步包装）
                                           ✅ handle_completion 路由（FTL/METADATA/GC/BBT/CHECKPOINT）
                                           ✅ GC/WL/Checkpoint 三级后台集成

Step 13 — Completion Table 统一完成路由
  ├── include/ftl/completion_table.h      ✅ 16 项路由表 + 同步等待器 + 统一泵声明
  └── src/ftl/completion_table.c          ✅ completion_table_add/lookup/remove/drain
                                           ✅ ftl_sync_waiter_handler（带/不带 release）
                                           ✅ completion_table_set_default（= ftl_handle_completion）

Step 6 — Metadata Directory
  ├── include/ftl/metadata.h              ✅ Anchor Block (33-34) + 128 entry Directory
  └── src/ftl/metadata.c                  ✅ init/lookup/update/alloc/flush/load 完整实现
                                           ✅ 预填充 128 个 Region（满后固定位置回退）
                                           ✅ CRC 保护（boot_crc32）

Step 7 — Pending Request 表
  ├── include/ftl/pending.h               ✅ 8 项固定表 + lba lookup
  └── src/ftl/pending.c                   ✅ add/lookup/lookup_by_lba/remove

Step 8 — Checkpoint
  ├── include/ftl/checkpoint.h            ✅ 全量 + Journal 截断协同 + 三级触发
  └── src/ftl/checkpoint.c                ✅ flush_all → 写6页冗余 → md flush → truncate
                                           ✅ 水位线/写入量/定时三级触发 + 紧急95%强制
                                           ✅ checkpoint_journal_watermark（ftl_submit_write 中调用）

Step 9 — BBT（Bad Block Table）
  ├── include/ftl/bbt.h                   ✅ Bitmap 4096 bit + CRC + 6页冗余
  └── src/ftl/bbt.c                       ✅ bbt_init（加载/出厂扫描）+ bbt_mark_bad + bbt_save
                                           ✅ 扫描策略：LDPC_RAW 读 Page 0 检测坏块
                                           ✅ Block 36 + Block 39 各3页冗余

Step 10 — Read Recovery
  ├── include/ftl/read_recovery.h         ✅ Per-Block ECC 趋势追踪 + 4级健康状态
  └── src/ftl/read_recovery.c             ✅ Stage 1: Read Retry（SET_FEATURES 8次重试）
                                           ✅ Stage 2: 邻近页辅助（page±1 读验证）
                                           ✅ Stage 3+: XOR/RAID 预留
                                           ✅ ECC 预测性坏块检测（rr_check_predictive）
                                           ⚠️ rr_record_ecc 未接入读完成路径（需 timer + completion）

Step 11 — Power Failure Recovery
  ├── include/ftl/power_failure_recovery.h ✅ 三类启动类型枚举 + 结果结构体
  └── src/ftl/power_failure_recovery.c     ✅ ftl_boot_recovery（fresh/clean/PF 判断）
                                            ✅ ftl_boot_fresh_start / clean_start / power_failure
                                            ✅ ftl_boot_scan_write_pointer（NAND 扫描空白页）
                                            ✅ 扫描 Journal header 判断（不调 replay，不破坏状态）

Step 12 — Wear Leveling
  ├── include/ftl/wear_leveling.h         ✅ 动态（alloc 选低擦除）+ 静态（三星 Daemon）
  └── src/ftl/wear_leveling.c             ✅ 三条件触发（差值/比值/低区占比）
                                           ✅ wl_select_victim（选有效页多+擦除少的冷 Block）
                                           ✅ wl_relocate（读→L2P查→写→Journal→擦→归还）
                                           ✅ wl_execute 接入 ftl_service（IDLE全力/NORMAL按需）

#### 代码审视修复（v0.18）
  ├── l2p_cache_set_md_dir()              ✅ ftl_init 中连接 Cache → Directory
  ├── bbt_init()                          ✅ ftl_init 中加载 BBT
  ├── sync_wait "other" release           ✅ 删除（单 LUN 无并发）
  └── boot_recovery journal_replay        ✅ 改为直接读 header，不修改 journal 状态

#### 代码审视修复（v0.19 — Completion Table 统一完成路由）
  ├── ftl_internal_sync_wait              ✅ 改为 completion_table_add + sync_waiter
  │                                       ✅ 不再独占 fc_poll_completion
  │                                       ✅ completion_table_drain 自动路由非目标 completion 到 ftl_handle_completion
  ├── ftl_service Phase 2                 ✅ 改为 completion_table_drain() 统一泵
  │                                       ✅ ftl_handle_completion 注册为 default handler
  ├── ftl_read/ftl_write 同步循环         ✅ 改为 completion_table_drain()，两泵合并为一个
  ├── l2p_cache_flush_one Region 分配     ✅ TODO 消除：首次写 Region → metadata_dir_alloc_region
  └── ftl_init 连接 Cache → Allocator     ✅ l2p_cache_set_md_dir 增加 alloc 指针参数
```

#### 未实现（非 FTL 核心模块，后续方向）

| 模块 | 文件 | 说明 |
|------|------|------|
| Controller 层 | `request.c` / `state_machine.c` / `scheduler.c` | 主机请求拆分、状态机、优先级调度 |
| Platform BSP | `timer.c` / `uart.c` / `cache.c` / `dma.c` / `interrupt.c` | 硬件外设驱动（已有 soc.h + DWT 实现）|
| NVMe Host | `nvme_admin.c` / `nvme_io.c` / `submission_queue.c` / `completion_queue.c` | NVMe 协议层（当前用 UART CLI）|
| Host Sim | `flash_simulator.c` / `file_backend.c` / `event_loop.c` / `fault_injection.c` / `simulated_time.c` | PC 端模拟环境（当前跑在真实硬件）|

**FTL 层 22 个源文件，0 个 `#warning` 核心桩。全部模块已实现并通过编译。**

---

## 16. Phase 规划

### 已实现（截至 2026-09-29 v0.22）

| 任务 | 说明 | 状态 |
|------|------|------|
| L2P Cache + CLOCK | 64 row SRAM region cache，CLOCK 替换，Region NAND load/flush | ✅ |
| Allocator + Super Block | 三级 Free Pool，跨双 Plane，磨损均衡，Hot/Cold 双温区 | ✅ |
| Journal | 固定 Block 40-41 双 Block 环形，SRAM buffer + flush_page，Header 4 字段 | ✅ |
| GC | 7 步搬移：读 META→查 L2P→分配 Cold→写→L2P→Journal，贪心 victim | ✅ |
| Checkpoint | 全量 L2P 快照 + 6 页冗余 + CRC，Journal 截断协同，三级触发 | ✅ |
| BBT | 出厂扫描（LDPC_RAW）+ NAND 持久化 + 运行时标记 + 6 页冗余 + CRC | ✅ |
| Read Recovery | Read Retry（8 次）+ 邻近页辅助 + ECC 趋势预测 + 4 级健康状态 | ✅ |
| Power Failure Recovery | 三类启动判断 + 写指针 NAND 扫描 | ✅ |
| Wear Leveling | 动态（alloc 选低擦除）+ 静态三星 Daemon（三条件触发）| ✅ |
| Metadata Directory | Anchor Block 33-34 + 动态分配 + CRC，L2P Region 双 Plane 交错 | ✅ |
| Pending Request 表 | 8 项固定表，command_id + LBA lookup，支持 complete_flag 通知 | ✅ |
| ftl_service 主循环 | 三级模式（IDLE/NORMAL/BUSY），GC/WL/Checkpoint 集成 | ✅ |
| ftl_fc_interface | 3 PBA helper + 4 内部同步 NAND 操作（read/write/erase/sync_wait/read_raw）| ✅ |
| **Completion Table** | 统一完成路由，消除两泵冲突，sycn_waiter 不走独立 fc_poll_completion | ✅ |
| **L2P Region 动态分配** | flush_one 首次写通过 metadata_dir_alloc_region 从 Free Pool 分配 | ✅ |
| **FTL 对外接口异步化** | 删除 ftl_read/ftl_write 同步包装，纯异步 ftl_submit_read/write + complete_flag 跟踪 | ✅ |
| **ftl_submit_write pending** | 写请求注册 pending + complete_flag，NAND PROGRAM 真完成后才通知 caller | ✅ |
| **Controller 异步提交** | ssd_controller_submit 纯异步，ssd_controller_is_complete 轮询完成状态 | ✅ |

### 剩余待办（对外接口待讨论）

以下为 FTL 对外接口层面已识别但未最终决定的问题，待后续讨论：

| 编号 | 问题 | 说明 |
|------|------|------|
| D3/D4/D5 | **LBA 地址体系统一** | ssd_lba_t(uint64_t) vs lba_t(uint32_t) 边界，API 参数是否统一 |
| D4 | **多 LBA 拆分** | block_count != 1 的请求在 Controller 层还是 FTL 层拆分 |
| D5 | **写失败回滚** | NAND PROGRAM 失败后 L2P/Journal 已更新但数据未写入，需标记坏块+回滚 |
| D6 | **队列深度/背压** | 多 LUN 并发时 FC SQ 深度有限，需流控机制 |
| D7 | **多命令流水线** | Multi-Plane / Cache Program 多命令并行提交 |
| D8 | **错误通知** | ftl_submit_read/write 的 complete_flag 只能通知"完成"，不能细粒度传递错误码 |

以上编号 D3~D8 为暂定讨论序号，将在后续 FTL 对外接口讨论中逐一确定。

### 剩余待办（非对外接口，未实现待处理）

#### P-A：多 LUN 并发前置（依赖 Completion Table 已就绪）

#### P-A：多 LUN 并发前置（依赖 Completion Table 已就绪）

| 任务 | 说明 | 优先级 |
|------|------|--------|
| P-A.1 | `fc_poll_completion` CQ 深度确认 — 确保 CQ 能同时容纳多 LUN 的完成事件 | 高 |
| P-A.2 | pending 表扩展 — `FTL_PENDING_MAX` 从 8 增大到 16/32，支持多 LUN 并发 pending | 高 |
| P-A.3 | `ftl_write` 同步等待改为 per-cmd sync_waiter — 脱离全局 SQ/CQ 空判定，改用 `completion_table_add` + 等具体 cmd_id | 高 |
| P-A.4 | `rr_record_ecc` 接入读完成路径 — 当前为 ⚠️ 未接入，需 timer + completion 集成 | 中 |

#### P-B：Controller / Host 层

| 任务 | 说明 | 状态 |
|------|------|------|
| P-B.1 | Controller 请求拆分（`request.c`） | ⚠️ `#warning` |
| P-B.2 | 优先级调度（`scheduler.c`） | ⚠️ `#warning` |
| P-B.3 | 状态机管理（`state_machine.c`） | ⚠️ `#warning` |
| P-B.4 | NVMe Admin 命令（`nvme_admin.c`） | ⚠️ `#warning` |
| P-B.5 | NVMe IO 命令（`nvme_io.c`） | ⚠️ `#warning` |
| P-B.6 | Submission/Completion Queue（`submission_queue.c` / `completion_queue.c`） | ⚠️ `#warning` |

#### P-C：优化

| 任务 | 说明 | 备注 |
|------|------|------|
| P-C.1 | Prefetch 顺序读预加载 | 后续 Phase |
| P-C.2 | GC L2P 批量更新优化 | 后续 Phase |
| P-C.3 | Warm boot 保留 cache | 后续 Phase |
| P-C.4 | 性能计数器 + 调试接口 | 后续 Phase |
| P-C.5 | 反向映射 bitmap（每 SB 64 bit 有效页 bitmap） | GC 加速 |
| P-C.6 | Thrashing 检测 + 降级 | miss_rate > 90% 时切换 write-through |

### 已废弃（旧 Phase 规划中的已完成项）

以下为旧 §16 Phase 规划中已实现的任务，不再追踪：

~~Phase 0~~ ✅ Phase 1 ~~掉电恢复~~ ✅ Phase 2 ~~完整 FTL~~ ✅

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
    nand_pba_t entries[L2P_ENTRIES_PER_REGION];  // 512 PBA = 2KB
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
nand_pba_t l2p_cache_read(l2p_cache_t *cache, lba_t lba);
void       l2p_cache_write(l2p_cache_t *cache, lba_t lba, nand_pba_t pba);
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
    nand_pba_t  old_pba;
    nand_pba_t  new_pba;
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
void journal_append(l2p_journal_t *journal, lba_t lba, nand_pba_t old, nand_pba_t new);
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
int  ftl_read(ftl_context_t *ftl, lba_t lba, void *data, uint32_t count);
int  ftl_write(ftl_context_t *ftl, lba_t lba, const void *data, uint32_t count);
int  ftl_trim(ftl_context_t *ftl, ssd_lba_t lba, uint32_t count);
void ftl_service(ftl_context_t *ftl);
void ftl_background_flush(ftl_context_t *ftl);
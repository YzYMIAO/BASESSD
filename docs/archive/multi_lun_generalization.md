# 多 LUN 泛化 + 共享 L2P Cache —— 实施计划

> **状态：阶段 A/B/C 已实现（未上板）；阶段 D 仍是计划**。
> 默认构建仍为 `CH_NUM=CE_NUM=LUN_NUM=1`，行为不变；N=2 已通过影子构建链接。
> 本文是这项工作的唯一清单；实施时逐项勾选，避免遗漏。
>
> 相关文档：`docs/design/cw_level_mapping_design.md` §3（分片模型）、
> `docs/design/ftl_design.md` §2.3/§3.5/§3.6（内存、元数据目录、L2P）、
> `docs/design/fc_nand_design.md` §18（1×1×1 拓扑与多 LUN 预留）、
> `docs/hardware_contract.md`（AXI_RAM 预算）、`include/ftl/l2p_cache.h`（路由）。

---

## 0. 目的与边界

做两件事：

1. **代码按 `CH_NUM/CE_NUM/LUN_NUM` 泛化**：默认 1×1×1 行为逐位不变；
   `LUN_NUM>1` 时地址、路由、账本、恢复都成立。
2. **实现主流 DRAM-less 的共享 L2P cache**：一套 cache 服务同一 `(ch,ce)` 下的所有 LUN，
   不再每 LUN 复制 132KB。

当前硬件只有一块 MT29F4G08（单 die、单 CE、单 LUN），所以：

- 多 LUN 只做**代码就绪**；**不承诺物理并行**，也**不做多 LUN 上板验证**（R1）。
- 真正的多 die 性能（跨 LUN superblock 条带、跨 LUN GC/WL）**不在本轮**，见 §8。

---

## 1. 主流 DRAM-less 怎么组织多 LUN（判据出处）

| 结论 | 出处 |
|---|---|
| 物理层级 channel → package/way → die(LUN) → plane → block/page；LUN 地址在行地址高位 | ONFI 2.3 spec（`https://onfi.org/files/onfi_2_3a_gold.pdf`） |
| 多 die 的并行来自 channel（外部）、die/LUN（interleaved）、plane（multi-plane） | PSU 博士论文（`https://etda.libraries.psu.edu/files/final_submissions/27001`）、MSST "Parallel all the time" |
| superblock/band 把每个 die/plane 一块组成逻辑擦写单元，写入 channel-first 条带化，GC 以 superblock 选 victim | SPDK FTL 文档（本仓 `docs/reference/pages/spdk_ftl.html:146-205`）、`ftl_design.md §3.2` |
| DRAM-less：L2P 表在 NAND，SRAM 只放**共享** L2P cache + 每 die 小块元数据 | DFTL（`https://dl.acm.org/doi/pdf/10.1145/1640457.1640463`）、HP-UFS（`https://www.usenix.org/system/files/fast23-kim-yoona.pdf`）、MSST'24 DRAM-less（`https://msstconference.org/MSST-history/2024/Papers/msst24-7.3.pdf`） |
| UFS/eMMC 的多个 LU/LUN 共享同一 FTL、同一 L2P cache，不是 N 套 FTL | HP-UFS；`storage_controller_learning.md:96-103` |
| 主流控制器固件用 **descriptor + 指针** 持有各表，scatter/linker 决定落 DRAM/SRAM/TCM | `ftl_optimization_plan.md:371-381`（OpenSSD 全局数组 / SPDK `spdk_ftl_dev` 内嵌表） |

**本仓现状**：每 `(ch,ce,lun)` 一个完整 `ftl_context_t`（305,712 B），
是学习简化的「分片模型」，不是主流的多 die 共享模型。

---

## 2. 当前事实与阻断点

| # | 事实 / 阻断点 | 位置 |
|---|---|---|
| 1 | `CH_NUM=CE_NUM=LUN_NUM=1`；单 FMC NCE（PG9）、单 R/B#（PD6） | `include/nand/devices/mt29f___g08a.h`、`src/nand/devices/mt29f___g08a.c` |
| 2 | ~~`LUN_ADDR_WIDTH=0` ⇒ PBA/PAA 装不下 LUN~~ **已修**：位宽由 `LUN_NUM` 推导（≤4 bit） | `include/nand/nand_config.h`、`include/ftl/ftl_config.h` |
| 3 | ~~`paa_to_nand_row()` 丢弃 LUN~~ **已修**：器件头提供 `NAND_PAA_TO_ROW`，LUN 在行 bit 18（仍在 ROW_2，3 个行周期内） | `include/nand/devices/mt29f___g08a.h` |
| 4 | ~~Phase V 读拆分未与 per-CW LOAD/FETCH 对齐~~ **已修**：读 = LOAD(ISSUE) → WAIT → 逐 CW 05h/06h；`LUN_NUM==1` 仍走原子 `WITHOUT_DATA_OUT` | `src/fc/fc.c`、`src/nand/nand_dispatch.c`、`src/nand/onfi_1_0.c` |
| 5 | 阶段 B 后：`g_ftl` 173,832 B/实例 + 共享 `g_l2p_cache` 132,136 B/(ch,ce) = 305,968 B（DIM=1）；`LUN_NUM=2` 当时装不进 384KB AXI_RAM（**阶段 C 散列后已可链接**） | `src/main.c`（FTL 实例/内存预算段） |
| 6 | FTL/控制器大部分已按维度写：`g_ftl[CH][CE][LUN]`、`lba_route_*`、逐实例 boot/service、完成按 ch/ce/lun 路由、逐 LUN NAND 队列 | `src/main.c:284-355`、`l2p_cache.h:70-112`、`fc.c:1713+` |

内存实测（DWARF / nm，当前 commit）：

| 结构 | 大小 |
|---|---:|
| `ftl_context_t`（阶段 C 后：描述符） | 14,664 B（0x3948） |
| `l2p_cache_t` | 132,136 B（0x20428；64 line + 全局脏位图；`region_id` 为 u32） |
| 五张表（独立对象，合计） | 123,896 B（BST 81,920 + closed 32,768 + free 8,192 + 池 1,016） |
| `metadata_dir_cache_t` | 25,164 B（DWARF 实测） |
| `checkpoint_handle_t` | 8,524 B（含 8KB 块元数据快照） |
| `ftl_wbuf_t` | 4,108 B |
| `l2p_journal_t` | 2,104 B |
| `ftl_allocator_t` | 1,148 B |

可用内存域（`linker/stm32h743xx_flash.ld` + `sys_port.c:15-28` MPU，均为 cacheable）：

| 域 | 容量 | 当前占用（DIM=1） |
|---|---:|---:|
| AXI_RAM | 384 KB | 184.6 KB（描述符 + `g_l2p_cache` + CLI + 其他 .bss） |
| AXI_NOCACHE | 128 KB | 70 KB（FC/DMA，保持） |
| DTCM | **128 KB**（此前误写 16） | 0.2 KB（栈/heap + `.noinit`） |
| SRAM12（SRAM1+SRAM2） | 256 KB | 121 KB（五张表独立对象） |
| SRAM3 | 32 KB | 10.4 KB（`g_ftl_checkpoint_handle` + `g_ftl_journal`） |
| SRAM4 | 64 KB | 24.1 KB（`g_ftl_md`） |

---

## 3. 设计

### 3.1 地址 / 拓扑（Phase A，已实现，未上板）

**器件配置分层**（`nand_config.h` 不是唯一配置）：

- 器件事实放在 `include/nand/devices/mt29f___g08a.h`（几何/拓扑/地址位宽/
  `NAND_PAA_TO_ROW`/FMC/ID/`nand_init`），由 makefile 的宏选择：
  `NAND_DEVICE ?= MT29F___G08A` → `-DNAND_DEVICE_MT29F___G08A`。
- `include/nand/nand_config.h` 只留通用软件布局（CW、PAA 打包、访问器、LBA/PBA
  共享低位编码），顶部按宏包含器件头；`LUN_ADDR_WIDTH` 由 `LUN_NUM` 推导。
- 器件驱动在 `src/nand/devices/<family>.c`，makefile 只编选中的一颗。
- 器件头**平台纯净**（只含 `<stdint.h>`）；R/B# GPIO 属板级接线，放在
  `include/nand/nand_fmc.h`。

**地址泛化**：

- PAA：`[CW][PAGE][PLANE][BLOCK][LUN]`；LUN 位宽 ≤4 bit，静态断言
  `(1U << LUN_ADDR_WIDTH) >= LUN_NUM`、`LUN_NUM <= 16`。
- `paa_to_nand_row()` 转发器件头的 `NAND_PAA_TO_ROW`：LUN 在行 bit 18 起。
  行地址总位数 = 6+1+11+LUN 位宽 ≤ 24 ⇒ **仍在 ROW_2 内，不需要 ROW_3**；
  `LUN_ADDR_WIDTH==0` 时输出与旧实现逐位相同。
- 开机用 ONFI 参数页 `addrcyc`（低 nibble = 列周期、高 nibble = 行周期）与
  本仓 `NAND_COLUMN_ADDR_CYCLES` / `NAND_ROW_ADDR_CYCLES` 比对；不符则
  `nand_init` 返回 `NAND_RESULT_INVALID_ARGUMENT`（release 也拒绝）。
- `ftl_config.h` 增加 `CH_NUM/CE_NUM <= PBA_*_WIDTH`、PBA ≤32 bit；
  `pba_fields_layout_matches()` 的往返自检改用 `PBA_USED_MASK`。
- **Phase V 读对齐**：读 = `OP_READ_PAGE_LOAD`(ISSUE) → `OP_READ_PAGE_WAIT`
  (tR + 状态) → 逐 CW `05h/06h`；多 plane 用
  `OP_READ_PAGE_MULTI_PLANE_LOAD`(43)。`LUN_NUM==1` 仍走原子
  `WITHOUT_DATA_OUT`，行为不变。不新增 `CONFIG_FC_MULTI_LUN`，用
  `LUN_NUM > 1` 编译期决定。

### 3.2 共享 L2P Cache（Phase B，已实现，未上板）

- `l2p_cache_t` 从 `ftl_context_t` 挪出，成为 `g_l2p_cache[CH_NUM][CE_NUM]`：
  同一 `(ch,ce)` 下所有 LUN 共享；不同通道/片选各一份。
- `ftl_context_t.cache` 改为指针；`ftl_init(ftl, topo, cache)` 只绑定指针。
  `l2p_cache_init` **每个 cache 只调一次**（设备级，在逐实例 boot 之前）；
  三个开机恢复路径不再 per-instance re-init，fresh/format 走
  `l2p_cache_invalidate_owner`。
- cache line 的 `region_id` 改为 **`uint32_t` 全局 region**（`lba >> 9`，覆盖
  `L2P_REGIONS_PER_INSTANCE × FTL_DIM_TOTAL`）；`region_dirty_bitmap` 按全局尺寸
  （DIM=1 仍是 64 word）。u32 使 DIM≥32 时不溢出；实测 `l2p_cache_t` = 132,136 B。
- **owner 解析**：全局 region 经 `completion_table_route_region`
  （`dim = region % FTL_DIM_TOTAL`，与 `lba_route_*` 同一套公式）返回 owner。
  load / flush / commit / eviction 全部用 owner 的 `metadata_dir` / `alloc` / `topo`；
  cache 不再保存单一 `md_dir/alloc`。回调由 main.c 注入。
- **owner 边界**：
  - A 的 service 可以驱逐/写回 B 的 line ⇒ load/flush 命令的 ch/ce/lun 必须取 owner，
    不是调用者；L2P region 必须写到 B 的 die。
  - `flush_inflight` **保持单个共享计数**（主流共享写缓冲 credit 模型）；
    跨 LUN 公平性登记为后续实测项（一个 LUN 可短暂占满 4 条写回）。
  - `ftl_flush` / `l2p_flush_kick_owner` / `l2p_cache_flush_owner_sync` /
    `cp_flush_regions` / `l2p_sync_pump` 全部按 owner 过滤/推进。
- 共享后每个活跃 LUN 的有效 cache 行数约 `64 / LUN 数`：这是主流 DRAM-less 的
  固有取舍；需要时后续增大 cache 或做 per-LUN 配额。
- thrashing 降级是 **per-cache**：一个 LUN 的 thrashing 会影响共享 cache 的所有 LUN。

### 3.3 LBA 语义：全链路全局（Phase B，已实现）

- **数据通路一律用 host 全局 LBA**：`group` / `pending->lba` / `wbuf.base` /
  `cmd->meta`（页 META）/ journal 条目 / migrate 的 `cw_lba` / read recovery /
  full metadata rebuild 全部是全局。
- **唯一局部坐标**：映射页在 owner die 元数据区里的位置
  `local_region = global_region / FTL_DIM_TOTAL`，只出现在 cache 的 metadata 边界
  （`metadata_dir_lookup/append_slot/commit_region`、`l2p_region_pba`）。
- `lba_to_local()` 退出数据通路（作为地址原语保留，生产代码零调用）；
  entries 下标 `lba & 511` 在全局/局部下相同。
- **不可信 LBA 先校验**（META/journal/full metadata rebuild 读回值）：
  `FTL_LBA_INVALID` / `>= L2P_GLOBAL_LBA_MAX` / `!lba_routes_to(..., topo)` 一律跳过。
- `FTL_DIM_TOTAL=1` 时 global == local，默认行为逐位不变；介质值不变。
- 全量调用点回扫清单见 §4 Phase B 清单。

### 3.4 掉电恢复（Phase B，已实现）

- 每 LUN 保留自己的 `metadata_dir` / `journal` / `cp`（固定块号各 die 相同）。
- 设备级 cache init 一次；**不得**在三个恢复路径里 per-instance re-init
  （会抹掉前面 LUN 刚回放的 DIRTY 行与全局脏位）。fresh/format 用
  `l2p_cache_invalidate_owner`（只清本 owner 的行与脏位）。
- 每 LUN 的 Checkpoint 只快照**自己的 region 脏位**：
  `l2p_cache_snapshot_owner_bitmap` 把全局位图按 owner 过滤、global→local 映射成
  **本地 2048-bit `region_bitmap`**。⚠ Header 的 `region_bitmap[64]` 仍是 2048 bit；
  `checkpoint_handle.region_bitmap`（本地）与 `cache.region_dirty_bitmap`（全局）的
  同尺寸 static assert 已拆掉。
- `cp_flush_regions` 遍历共享 cache：跳过别的 owner 的行；用局部 region 查快照；
  LOADING 中止、FLUSHING 等待，语义不变。
- journal replay 按本 LUN 的 journal；entry 用全局 LBA，先校验再写共享 cache。
- `ftl_boot_apply_region_bitmap`：跳过 DIRTY / FLUSHING / LOADING / 别的 owner；
  用局部 region 查 CP 位图；owner 的 metadata_dir + 局部 region 预载。
- 介质版本：`FTL_MEDIA_LAYOUT_VERSION = 6 + (FTL_DIM_TOTAL > 1)`；
  DIM=1 不格式化，DIM>1 首次启用自动格式化。

### 3.5 描述符化与内存域散列（Phase C，已实现，未上板）

`ftl_context_t` 已变成**描述符**（实测 14,664 B/实例）：只留指针 + 热小状态：

```c
typedef struct ftl_context {
    l2p_cache_t          *cache;         /* 共享：g_l2p_cache[ch][ce] */
    ftl_table_refs_t      table_refs;    /* 五张表的引用；表本体各自独立（当前 SRAM12） */
    metadata_dir_cache_t *metadata_dir;  /* per instance；SRAM4 */
    l2p_journal_t        *journal;       /* per instance；SRAM3 */
    checkpoint_handle_t             *checkpoint_handle;        /* per instance；SRAM3 */
    pending/groups/wbuf/alloc/mig/rr/bg  /* 热状态，留在描述符（AXI） */
    ...
} ftl_context_t;
```

数组按 `FTL_DIM_TOTAL` 声明，section-attributed（`main.c`）：

| 数组 | 段 / 域 | 实测/实例 | N=2 实测 |
|---|---|---:|---:|
| `g_l2p_cache[CH][CE]` | AXI_RAM（cacheable） | 132,136 B | 132,136 B |
| 五张表（`g_ftl_blocks` / `free` / `op` / `spare` / `closed`，各 `FTL_DIM_TOTAL` 份） | `.sram12_data`（SRAM1+SRAM2 连续 256KB） | 123,896 B | 247,792 B（242.0 KiB） |
| `g_ftl_checkpoint_handle[]` + `g_ftl_journal[]` | `.sram3_data`（32KB） | 10,628 B | 21,256 B（20.8 KiB） |
| `g_ftl_md[FTL_DIM_TOTAL]` | `.sram4_data`（64KB） | 25,164 B | 50,328 B（49.1 KiB） |
| 描述符 + pending/groups/wbuf/mig/rr/bg | AXI_RAM | 14,664 B | 29,328 B |

- 新增 `SRAM12_DATA/SRAM3_DATA/SRAM4_DATA` 宏；linker 把 SRAM1/2 合并为 `SRAM12`，
  新增 `.sram12_data`，每段 `KEEP` + `ASSERT(SIZEOF(...) <= LENGTH(...))`。
- ⚠ `.sram*_data` 是 **NOLOAD**：启动只清 AXI `.bss` ⇒ `ftl_init` 对
  `tables/metadata_dir/journal/checkpoint_handle` **整表 memset**，不能依赖 BSS。
- 每域独立 `_Static_assert`；`main.c` 的 AXI 断言现在覆盖
  描述符 + 共享 cache + CLI 缓冲（16KB，从 `.sram2_data` 迁入 `.bss`）。
- MPU 已把 SRAM1/2/3/4 配成 cacheable；这些表是 **CPU-only**（无 DMA），
  不需要 cache 维护。DTCM 实测 **128KB**（此前文档误写 16KB），本阶段未使用。
- 热路径在函数入口取一次局部指针（`l2p_cache_t *cache = ftl->cache;`）。

**N=2 结论（影子构建实测）**：共享 cache + scatter 可装下 ——
AXI 195.5KB / SRAM12 242.0KB / SRAM3 20.8KB / SRAM4 48.2KB，**链接成功**；
N=4 仍需表瘦身或 SDRAM。

### 3.6 NAND/ONFI（Phase A 登记，硬件验证待后续）

- row cycle 数用 ONFI 参数页 `addrcyc` 解码；当前 MT29F4G08 是 3 row cycles，
  多 die 可能 4/5。
- **Read Status Enhanced（0x78 + LUN 地址）**：一个 target 多 LUN 共享 R/B# 时，
  `WAIT_READY` 只反映 target；每 LUN 的完成状态必须单独查。当前没有这条路径，
  多 LUN 真正的完成判定需要补（本轮不实现）。
- 每 LUN 的 ONFI 参数页 / 坏块 / ECC / timing 可能不同；多 die 封装可能是
  compound target，需按 LUN 读取/校验。
- `DIE_NUM` 与 `LUN_NUM` 的关系写清并加 static assert。
- `nand_fc_request_t` 不带 ch/ce/lun；单 target 多 LUN 时 LUN 在 row address 里即可；
  多 CE 需要后端 CS/状态扩展（本轮不做）。

---

## 4. 改动清单（按阶段，实施时勾选）

### Phase A：器件分层 + 地址 / 拓扑泛化（已实现，未上板）

- [x] 器件配置分层：
  - [x] `include/nand/devices/mt29f___g08a.h`（几何/拓扑/位宽/行编码/FMC/ID/init）
  - [x] `nand/nand_config.h` 按 `-DNAND_DEVICE_MT29F___G08A` 选择器件头
  - [x] `src/nand/devices/mt29f___g08a.c` + makefile 宏/源选择 + `OBJ_DIR` 器件后缀
  - [x] typed `nand_fc_*` 声明归 `nand/nand_fc_interface.h`；R/B# GPIO 归平台侧
- [x] `include/nand/nand_config.h`
  - [x] `LUN_ADDR_WIDTH` 由 `LUN_NUM` 推导 + 静态断言
  - [x] `paa_to_nand_row()` 转发器件 `NAND_PAA_TO_ROW`（LUN 在 bit 18）
  - [x] `NAND_COLUMN_ADDR_CYCLES` / `NAND_ROW_ADDR_CYCLES`
- [x] `include/ftl/ftl_config.h`
  - [x] `CH_NUM/CE_NUM <= PBA_*_WIDTH`、PBA ≤32 bit
  - [x] `PBA_USED_MASK` 往返自检
  - [ ] 全局 region 数 ≤65535（**阶段 B** 共享 cache 键才需要）
- [x] `src/nand/devices/mt29f___g08a.c`：ONFI `addrcyc` 一致性检查（不符拒绝启动）
- [x] Phase V 读对齐：`LOAD(35)` / `WAIT(36)` / `MULTI_PLANE_LOAD(43)` +
      逐 CW 取数；ISSUE 失败短路
- [x] 潜伏单 LUN 假设：
  - [x] `src/controller/cli.c` 的 `[0][0][0]` → `lba_route_*` 路由
  - [x] `src/ftl/bbt.c` 三处 `pba_make(...,0U,...)` → `alloc->lun`
  - [x] `src/ftl/read_recovery.c` 的 LUN 恒 0 注释
  - [x] `include/ftl/bst.h`、`src/main.c` 的措辞/实测值（305,712 B）
  - [ ] RAID `[LUN_NUM]` → `[CH][CE][LUN]`：**不做**（RAID 关闭，启用前必须泛化）

### Phase B：共享 L2P Cache（已实现，未上板）

- [x] `l2p_cache_t` 移到 `g_l2p_cache[CH][CE]`；`ftl_context_t.cache` 改指针
- [x] `l2p_cache_init` 设备级一次（main.c，逐实例 boot 之前）；
      `ftl_init(ftl, topo, cache)` 只绑定；恢复路径改 `l2p_cache_invalidate_owner`
- [x] line `region_id` 改 **u32 全局 region**；`region_dirty_bitmap` 全局尺寸
- [x] owner 解析：`completion_table_route_region`（`lba_route_*` 同一套公式），
      load/flush/commit/evict 全覆盖；cache 不再保存 `md_dir/alloc`
- [x] `flush_inflight` **单个共享计数**（主流共享 credit）；owner 过滤的 flush/sync/CP
- [x] `ftl_l2p_callback` 提交到 owner 的 `metadata_dir`（local region = global/DIM）
- [x] Checkpoint：`l2p_cache_snapshot_owner_bitmap` 生成**本地** 2048-bit 快照；
      `cp_flush_regions` owner 过滤；拆同尺寸断言；Header 格式不变
- [x] 数据通路全链路全局 LBA：group/pending/wbuf/page META/journal/migrate/rr/full metadata rebuild / trim
- [x] 不可信 LBA 校验：`FTL_LBA_INVALID` / `< L2P_GLOBAL_LBA_MAX` / `lba_routes_to`
- [x] 介质版本：`6 + (FTL_DIM_TOTAL > 1)`；DIM=1 不格式化
- [x] 内存实测：`g_ftl` 173,832 B/实例 + `g_l2p_cache` 132,136 B/(ch,ce)
      = 305,968 B（DIM=1）；DIM=2 当时超 AXI（**阶段 C 已解除：N=2 链接成功**）
- [ ] 默认 LUN=1 行为逐位不变（**板端回归待做**）

### Phase C：描述符化 + 内存域散列（已实现，未上板）

- [x] `ftl_context_t`：`tables/metadata_dir/journal/checkpoint_handle` 指针化（描述符 14,664 B）
- [x] 五张表独立对象（各 `FTL_DIM_TOTAL` 份）→ `.sram12_data`（SRAM1+SRAM2 合并）
- [x] `g_ftl_checkpoint_handle/journal[FTL_DIM_TOTAL]` → `.sram3_data`
- [x] `g_ftl_md[FTL_DIM_TOTAL]` → `.sram4_data`
- [x] `g_l2p_cache[CH][CE]` → AXI_RAM（共享热 cache）
- [x] `linker/stm32h743xx_flash.ld`：SRAM12 区域 + `.sram12_data` + 段 KEEP/ASSERT
- [x] `include/ssd_compiler.h`：`SRAM12_DATA/SRAM3_DATA/SRAM4_DATA`
- [x] **NOLOAD 显式初始化**：`ftl_init` 对四个数组整表 memset
- [x] 每域 `_Static_assert` 预算；`main.c` AXI 断言改为描述符 + cache + CLI
- [x] MPU/cache：SRAM1/2/3/4 已 cacheable；四表 CPU-only（无 DMA）
- [x] 热路径局部指针化（`ftl->cache` 已是局部指针；新增指针只影响冷路径）
- [x] CLI `g_wr_buf` 从 `.sram2_data` 迁入 AXI `.bss`（SRAM12 预算所迫）
- [x] **N=2 影子构建链接成功**：AXI 195.5KB / SRAM12 242.0KB /
      SRAM3 20.8KB / SRAM4 48.2KB（无硬件，仅链接证明）

### Phase D：回扫、文档、验证

- [ ] 全局 LBA 全量回扫（C10）：
      `ftl_submit_*` / `ftl_issue_subop` / `ftl_confirm_write` / `ftl_wbuf_*` /
      `pending_*` / `group` / `journal_*` / `metadata_dir_append_slot` /
      `l2p_cache_*` / `checkpoint` / `migrate` / `read_recovery` / `ftl_trim` /
      `ssd_request_split/run_end` / CLI `map/wr/rd`
- [x] `FTL_MEDIA_LAYOUT_VERSION`：`6 + (FTL_DIM_TOTAL > 1)`（阶段 B 已做）；
      DIM=1 不 bump/不格式化，DIM>1 首次启用自动格式化
- [ ] 文档：本文、`cw_level_mapping_design.md §3.7`、`ftl_design.md §2.3/§3.5/§3.6`、
      `fc_nand_design.md §18`、`hardware_contract.md`
- [ ] 不新增 UT（R4）；冻结 UT 的既有编译错误不修

---

## 5. 内存预算（N=2，共享 cache + scatter，不做表瘦身）

| 域 | 内容 | 实测（影子 N=2） | 容量 |
|---|---|---:|---:|
| AXI_RAM | 2×描述符 + 共享 cache + CLI + FC/其他 | **195.5 KB** | 384 KB |
| SRAM12 | 2×五张表 | **242.0 KB** | 256 KB |
| SRAM3 | 2×`checkpoint_handle` + 2×`journal` | **20.8 KB** | 32 KB |
| SRAM4 | 2×`metadata_dir` | **49.1 KB** | 64 KB |

N=2 已通过影子构建**链接成功**；N=4 需要表瘦身（`ftl_block_t` 20B→12B 等）或 SDRAM。
DIM=1 下：描述符 14,664 B + 共享 cache 132,136 B（AXI ~180KB）+
tables 123,896 B（SRAM12）+ checkpoint_handle/journal 10,628 B（SRAM3）+
metadata_dir 25,164 B（SRAM4）。

---

## 6. 验证

1. `make all`（debug）/ `make release`：零新告警；默认 LUN=1 行为一致
   （`FTL_DIM_TOTAL=1` 时 global==local，地址路径逐位相同）。
2. `make tests UT=FTL` / `UT=FC`：冻结 UT 仍有既有符号错误；不新增测试文件。
3. **影子构建**（临时宏/scratch 头，不提交）：
   - `LUN_NUM=2, LUN_ADDR_WIDTH=1` + 64-line 共享 cache + scatter 段编译通过；
   - 静态断言全过；PBA/PAA LUN 往返正确；`lba_route_*`/`lba_to_local` 双射；
   - `nm`/map 确认数组落段（`g_ftl_blocks` / `g_ftl_closed_sb_list` 等 0x3000_xxxx、`g_ftl_md` 0x3800_xxxx、
     `g_l2p_cache` 0x2400_xxxx），各段不溢出。
4. **上板**：只验证 LUN=1 回归（boot、`wr/rd` 冒烟、FC/FTL UT）；
   多 LUN 无硬件，**不得声称上板验证**（R1）。
5. 后续多 die 硬件：验证 4 字节 row address、Read Status Enhanced、
   Phase V 对齐、superblock 跨 die 调度。

---

## 7. 风险与回滚

- **owner 解析漏路径**：load/flush/commit/evict 任一处用错 owner ⇒ region 写到错误的 die。
  用统一的 `l2p_cache_owner_of_region()`，并在影子 dim=2 构建里覆盖。
- **Checkpoint bitmap 尺寸**：误把 `checkpoint_handle.region_bitmap` 做成全局，会让 Header 超一页
  或 per-LUN 快照错位；保持本地 2048 bit + global→local 映射。
- **NOLOAD 未初始化**：`.sram*_data` 上电垃圾 ⇒ 必须显式 memset/初始化。
- **LBA 语义混用**：global 与 local 在同一路径混用，编译期无声；按 §4 Phase D 全量回扫。
- **默认行为漂移**：LUN=1 下 global==local，若 `.bin`/板端回归异常，先查
  `lba_to_local` 的调用点是否被误改。
- 回滚：Phase A/B/C 分阶段提交；任一步失败可单独回退（不涉及介质格式的 Phase 除外）。

---

## 8. 明确不做 / 后续

- **跨 LUN superblock / page-level 条带化**：当前是 region 级分片（一个 region 整体落
  一个 die），不是主流多 die superblock 条带；跨 LUN GC/WL 调度也未做。
  这是多 LUN 性能的真正来源，需硬件/后续阶段单独设计。
- 多 CE/多通道后端、FMC 多 target、DMA 多流。
- 共享 cache 的 per-LUN 命中率/QoS 隔离、cache 扩容。
- `ftl_block_t` 瘦身/表压缩（N=4 才可能需要）。
- UT 不新增（R4）；冻结 UT 的既有编译错误不修。

---

## 9. 待确认

- 目标 LUN 数：2（可装下）还是 4（需表瘦身/SDRAM）？
- 多 LUN 是否需要**物理并行**（多 die/多 CE 硬件），还是仅代码泛化？
- 是否需要把「跨 LUN superblock 条带」纳入下一阶段（性能导向）？

# FTL Policy B：每 plane 独立块分配 + 坏块替换

**日期**: 2026-10-07
**相关设计**: `docs/design/ftl_design.md` §3.6（Allocator）/§3.8（坏块替换）
**前置**: `docs/changelog/fc_per_plane_row_address.md`（FC 层 per-plane 行地址，本改动的前提）

---

## 概述

旧实现里 Super Block 的两个 plane **必须用同一个 block 号**：

| 位置 | 旧行为 |
|---|---|
| `src/ftl/allocator.c` `allocator_select_block` | 要求 `ftl_gblock(0,b)` 与 `ftl_gblock(1,b)` **同时** FREE 且非坏，否则跳过 |
| `src/ftl/allocator.c` `allocator_alloc` | `block1 = block0;` |
| `src/ftl/allocator.c` `allocator_alloc` 返回值 | 恒返回 **plane 0** 的 PBA |
| `src/fc/fc.c` desc PAA / `plane_paa[]` | 复制 `command->paa` 的行地址 + 替换 plane 位 |

后果：plane0 的坏块**连坐** plane1 的同号好块；且 plane0 无空闲同号块时整对分配失败。

v12 改为 **Policy B**（主流做法）：两 plane **各自独立选块** + 独立坏块替换。
本改动含三段，可分别上板回归。

---

## 关键洞察：配对只被创建，从不被查找

这是整段设计的地基。核实后确认，Super Block 的"plane0 块 ↔ plane1 块"对应关系在整个 FTL 里：

- **创建**：仅由 allocator 在写入时决定（`allocator_alloc_pair`）；
- **查找**：**唯一**需要它的地方是掉电重建（旧 `ftl_rebuild_closed_sb_list` 按同号块推配对）。

其余全部路径都不需要它：

- multi-plane **读**：两个 PBA 各自从 L2P 查（`internal_lba` / `internal_lba+1`）；
- 写完成**簿记**：用写入时记下的 `pending->new_pba1`（本改动新增字段）；
- **GC / WL**：本来就逐 plane 独立迭代（`ftl_gblock(plane, block)`、`valid_map[PLANE_NUM]`）。

于是把**重建改成 per-block**（段 3）之后，配对就不需要任何持久化，free-pool 驱动的磨损均衡完整保留。

> 为什么不做"确定性推导"：free pool 是**动态**的——`gc_erase_and_free` 把 victim 擦完
> `allocator_free_block` 归还 free pool，`allocator_select_plane_block` 再从池里挑下一块。
> 所以"pair 索引 → 物理块"取决于 **GC 历史**，而 GC 历史不在任何持久化位图里
> （`bbt_header_t` 只有一张 `bad_map[512]`，且 `src/ftl/bbt.c` 把载入的坏块 `bad_reason`
> 一律置 `FTL_BLOCK_BAD_FACTORY`，RAM-only）。确定性推导只覆盖"没跑过 GC"的开机态。

---

## 段 1：分配器逐 plane 选块（`src/ftl/allocator.c` / `include/ftl/allocator.h`）

| 函数 | 改动 |
|---|---|
| `allocator_select_plane_block(alloc, plane)` | **新增**（替代 `allocator_select_block`）：只挑**该 plane** 的 FREE 非坏块中 `erase_count` 最小者 |
| `allocator_open_sb(alloc, sb, temperature)` | **新增**（从 `allocator_alloc` 里抽出）：逐 plane 取块，某 plane 拿不到时从 OP 池补（每 plane 最多补 `PLANE_NUM` 次），全部就位后才从 Free Pool 移除、置 OPEN、绑拓扑、扣 `pre_cond_count` |
| `allocator_alloc_pair(alloc, temperature, out[PLANE_NUM])` | **新增**：SB 打开时两 plane 各自选块，`out[]` 填当前页在两 plane 的 PBA，`next_page++`，返回 `out[0]` |
| `allocator_alloc(alloc, temperature)` | **保留**为单 plane 便捷 = `allocator_alloc_pair()` 取 plane 0 那半（GC / WL / metadata 的单 plane 写路径继续用它） |
| `allocator_select_block` | **删除**——它的"双 plane 同号"配对语义正是 Policy B 要拆掉的东西 |

`allocator_close_sb` / `allocator_add_closed_sb` / `allocator_remove_closed_sb` 已经是
`plane_blocks[PLANE_NUM]` 签名，只需加哨兵判断（见段 3）。

**`allocator_alloc` 的行为不变**：它本来就返回 plane0 的 PBA 并推进一次 `next_page`
（plane1 同页槽位从未被写）。冷 SB 的 plane1 块"被占用却不写"是**既有缺口**，见文末。

---

## 段 2：命令构造 + 完成簿记（`src/ftl/ftl.c` / `include/ftl/pending.h` / `src/ftl/ftl_fc_interface.c`）

| 位置 | 改动 |
|---|---|
| `include/ftl/pending.h` `ftl_pending_req_t` | 新增 `nand_pba_t new_pba1;`（此前只有 `old_pba1`，plane1 的新位置靠"同 block + plane=1"派生）；`pending_add()` 签名同步增参 |
| `src/ftl/ftl_fc_interface.c` | 新增 `ftl_cmd_setup_ppa_pair(cmd, const nand_pba_t pba[PLANE_NUM])`：逐 plane 调 `fc_command_attach_cw(cmd, plane, cw, pba_to_paa(pba[plane]))` 建链。保留 `ftl_cmd_setup_ppa`（单 plane 路径 / GC 用） |
| `src/ftl/ftl.c` completion（plane1 分支） | plane1 的 PBA 改从 `pending->new_pba1` 取，删除"同 block + plane=1"派生 |
| `src/ftl/ftl.c` `ftl_submit_write` | 改调 `allocator_alloc_pair` + `ftl_cmd_setup_ppa_pair`，`pending_add(..., new_pba[0], old_pba1, new_pba[1], ...)` |
| `src/ftl/ftl.c` `ftl_submit_read` | **pages=2 时查两次 L2P**（`internal_lba` 与 `internal_lba+1`），两次都给 `ftl_cmd_setup_ppa_pair`。今天只查一次 + FC 派生 plane1，Policy B 下会读到错误的 plane1 块 |
| `src/ftl/ftl.c` 写失败路径注释 | 更新为"只退失败的那个 plane"（旧注释写"同号双 plane 一起退役"） |

**读路径的未映射语义**：pages=2 时**任一** LBA 未映射 → 整请求按全零返回（与单页未映射的既有语义一致）。
部分映射的 multi-plane 读无法只读一半 plane；旧代码在 plane1 未映射时会返回该页残值，现在返回零（更安全）。

---

## 段 3：掉电重建改 per-block（`src/ftl/power_failure_recovery.c` / `include/ftl/allocator.h`）

### 3.1 `super_block_t` 支持退化单 plane 条目

新增哨兵 `FTL_BLOCK_NO_PLANE`（`= UINT16_MAX`）：`plane_blocks[plane] == FTL_BLOCK_NO_PLANE`
表示该 plane 无块。**Open SB（`hot_sb`/`cold_sb`，multi-plane 写的单元）恒双 plane**；
Closed 条目允许退化单 plane（重建产物）。新增 helper：

```c
static inline uint8_t super_block_has_plane(const super_block_t *sb, uint8_t plane);
```

受影响的读者一律加哨兵判断：

| 位置 | 处理 |
|---|---|
| `super_block_page_addr` / `_pba`（`include/ftl/allocator.h`） | 加 `ASSERT(has_plane)`——哨兵不是合法块号，不可展开 |
| `allocator_close_sb` / `allocator_remove_closed_sb` / `allocator_add_closed_sb`（`src/ftl/allocator.c`） | 平面循环 `continue` 跳过哨兵 |
| `gc_select_victim` 的有效页统计、`gc_migrate_sb` 的 plane 循环、`gc_erase_and_free` 的擦除循环（`src/ftl/gc.c`） | 同上 |
| `gc_erase_and_free` 的 closed 条目定位 | **两 plane 都比对**（只比 `plane_blocks[0]` 会命中另一条同哨兵条目） |
| `wl_select_victim` 的 EC 均值 / 有效页统计、`wl_relocate` 的 plane 循环、`wl_predictive_relocate` 的退役循环（`src/ftl/wear_leveling.c`） | 跳过哨兵；EC 均值按实际 plane 数取平均 |

### 3.2 重建逐 plane 独立扫描

`ftl_rebuild_closed_sb_list` 从"按 block 号、双 plane 一起判"改为：

```c
for (plane = 0; plane < PLANE_NUM; plane++)
  for (block = 0; block < BLOCKS_PER_PLANE; block++) {
      g = ftl_gblock(plane, block);
      if (is_bad(b) || is_metadata_block(block)) continue;
      np = scan_block_write_pointer(ftl, plane, block, raw_buf);
      if (np == 0) continue;                    /* 未写过 → 保持 FREE */
      allocator_remove_from_free(alloc, g);      /* 写过 → 拉出 free pool */
      close_full_block(ftl, plane, block, FTL_TEMP_HOT, stats);  /* 一律进 Closed */
  }
```

- `close_full_block` 签名从 `(ftl, block, temp, stats)` 改为 `(ftl, plane, block, temp, stats)`，
  内部建**退化单 plane 条目**（`plane_blocks[plane] = block`，另一 plane = `FTL_BLOCK_NO_PLANE`）。
- **`recover_open_sb` 删除**。
- `ftl_rebuild_stats_t`：`sb_rebuilt` → `blocks_rebuilt`（Closed 条目数 = 写过的物理块数），
  `partial_pairs` → `partial_blocks`，`open_restored` 删除（不再有 Open SB 恢复）。

### 3.3 BBT 不变

`bbt_header_t` **不动**（不改格式、不加 `factory_map` / `runtime_remap`）——per-block 重建不需要
出厂 / 运行时的区分，坏块本来就逐块标记在 `ftl_block_t.state` 里，重启后由 `bbt_init` 统一恢复。
`src/ftl/bbt.c` 把 `bad_reason` 一律置 `FTL_BLOCK_BAD_FACTORY` 的既有行为**保持**
（不影响正确性，只是诊断信息丢失——作为已知缺口记录，不在本轮修）。

---

## 行为变更（相对 v0.40 / d53d092）

**掉电时未写满的 Open SB 不再"续写"**：旧 `recover_open_sb` 会把部分写块恢复为 Open SB
（保留写指针前页 + 无条件置 bitmap，不判 L2P）。per-block 下无法确定两个部分写块是否曾是一对，
强行恢复会让后续 multi-plane 写落到**错误的 plane1 块**。现改为：

- 部分写块也作为 **Closed 条目**，有效位图由 L2P 反查回填（torn / 覆盖页自然不置位），整块交 GC 迁移。

**安全性等价**（块不会被当空白复用，数据由 L2P 反查保护），代价是掉电后多一次 GC 搬移。
另外新路径的 bitmap 是 **L2P 校验过的**（旧的 `recover_open_sb` 是无条件置位），严格更准。

---

## 已知缺口（本轮不修，记录在案）

1. **GC / WL 写路径仍是单 plane**（用户决定）：`src/ftl/gc.c` 的 `allocator_alloc(FTL_TEMP_COLD)`
   仍返回 plane0 PBA，`sync_write_page` 的 `multi_plane = 0U` → 冷 SB 的 plane1 块被占用却从不写入，
   冷池容量打对折。**这是既有行为**（旧的同号配对同样消耗 plane1 块），本轮只把它显式记下来。
   修复方向：GC/WL 改走 `allocator_alloc_pair` + `multi_plane` 写（需 GC 搬移路径支持双 plane 写）。
2. **`bbt.c` 的 `bad_reason` 重启后全变 `FTL_BLOCK_BAD_FACTORY`**：诊断信息丢失（见 3.3）。
3. **`src/ftl/power_failure_recovery.c` 的 `ftl_boot_scan_write_pointer` 是死代码**（无调用者），
   它仍按"双 plane 同号"逐 plane 扫同一 `sb->plane_blocks[plane]`——虽已 per-plane，但语义与新
   rebuild 重复。本轮不动。

---

## 不做的事

- **不改任何测试源文件**（平台规则：测试归用户），不新建测试 / 桩 / target。
- **不动 `fc_stripe_submit` / `fc_data_path`**（`CONFIG_FC_RAID = 0`）。
- **不提交、不推送**（除用户另行要求）。

---

## 验证

| 项 | 结果 |
|---|---|
| `make format-check` | 通过 |
| debug | 零警告（153776 B；+1576 B vs v11 的 152200 B） |
| release（`NDEBUG`） | 零警告（86988 B；+132 B vs v11 的 86856 B） |
| `make -k tests` | `test_fc_level2.o` **零诊断**；仅卡在**用户维护**的 `test_fc_level1.c`（旧 scheduler API）与 `test_fc_level3.c:48`（旧 `nand_dispatch_set_device_ready`），与本次改动无关（改动前后基线一致） |
| 静态核对 | `allocator_alloc_pair` 在「两 plane 可用块数不等」时各自独立选到块；「单 plane 无可用块」时返回 `NAND_PBA_INVALID` 触发紧急 GC。重建 per-block 路径：只写 plane0 的块 → 生成单 plane Closed 条目（plane1 = `FTL_BLOCK_NO_PLANE`），GC/WL 跳过哨兵 plane |

**上板验证未做**（平台规则：用户执行）。段 1 对现有行为中性，先跑
`info → wr 0 2 → rd 0 2 c → tr 0 → fl → info` 回归。段 2 / 段 3 需构造用例：
**写入跨满一个 SB 后断电重启**，`info` 看容量与 closed_sb_list 重建结果，再 `rd` 校验数据。
**这是唯一能验证配对正确性的路径**——配错不会报错，只会让 `close_full_block` 的 L2P 反查全不匹配、
块静默变空，必须靠读回数据确认。

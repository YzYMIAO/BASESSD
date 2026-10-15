# v47：`fc_command_t.pba` → `.paa` —— PBA 退出 FC 层（FTL 侧 `pba_to_paa()` 一次转出）

- **日期**：2026-10-11
- **范围**：**源码 + 测试 + 文档**。`include/fc/fc_command.h`、`include/fc/fc_interface.h`、`src/fc/fc_interface.c`、
  `src/fc/fc.c`、`include/ftl/ftl_fc_interface.h`、`src/ftl/ftl_fc_interface.c`、`src/ftl/ftl.c`、
  `include/nand/{onfi_1_0.h,nand_dispatch.h}`、`src/nand/{onfi_1_0.c,nand_dispatch.c,nand_dispatch_tables.c}`（注释）；
  `tests/unit/fc_test_common.{c,h}`、`tests/unit/test_fc_level3.c`；本文档 + `docs/design/fc_nand_design.md` +
  `docs/design/ftl_design.md` + `docs/design/fc_nand_ut.md`。
  **`tests/integration/**`、`tests/framework/**`、`ut_main.c` 不动**。
- **介质格式**：无变更（只改地址的**传递路径**，不改任何写进 NAND 的位）。
- **测试**：**用例数不变（仍 53）**。`make format-check` 过；`make clean && make tests` **0 warning 0 error**、
  链接 **225212 B**（原 225140）；固件 `make` 0/0、**166708 B**。
- ⚠ ~~**需上板**：PAA 与 PBA **位序不同** ⇒ 任何遗漏的转换都**静默算错地址**（编译期无声），只有真实器件能判。~~
  ✅ **上板已验**（2026-10-11，seed `0x5a4f21e4`）：全套 **53 of 53 (100%)**、0 skipped。
  ⚠ 首次上板时红在 `block_cache_ops`——**不是本版引起**（是 v44 的 CACHE_RANDOM 语义理解错，
  见 v48）。本版的 PBA→PAA 重构经 host 侧穷举核对 + 完整套上板双重确认无误。

## 〇、起因

用户原话：*「我感觉 SQ 里面的 pba 会[经] FC 接口去 FC-NAND 层，这样感觉不是很好，我希望把 pba 位域改为 paa
物理 ARRAY 地址，然后组合命令时是组合 pba，然后 pba 转 paa 赋值给 cmd->paa，这样设计 pba 就不会和 FC 层有牵扯」*

现状：`fc_command_t.pba`（v24 起）装的是**系统 PBA**——带 ch/ce/lun 的 FTL 侧编码。它跨 FTL→FC 边界进来后，
FC 层在 **10+ 处** 用 `pba_to_paa()` 现场转换（`fc_build_dispatch_request` 的五个分支、
`fc_build_request_data_planes`、`fc_stripe_submit`）。而 FC 层本来就有 `desc->paa`（PAA）作为原生物理地址。
两套编码并存 ⇒ 每处转换都是一次静默算错地址的机会。

**PBA 与 PAA 位序不同**，`pba_to_paa()` 是**真正的字段重排**（不是掩码截取）：

```
PBA 低位优先： [cw:2][plane:1][page:6][block:11][lun][ce:2][ch:2]
PAA 低位优先： [cw:2][page:6][plane:1][block:11][lun]
                    └────── plane 与 page 位置对调 ──────┘
```

## 一、设计定案（已与用户确认）

| 决策 | 结论 |
|---|---|
| 字段 | `fc_command_t.pba`（`DWORD`）→ `fc_command_t.paa`（仍是 `DWORD`，PAA ≤ 21 bit） |
| 建链入口 | **一并改成收 PAA**：`fc_command_configure_descriptor(cmd, plane, cw, nand_paa_t row_paa)`——FC 层最后一个收 PBA 的地方 |
| FTL 侧 API | **名字与签名不变**（`ftl_cmd_setup_ppa(cmd, nand_pba_t)` / `_pair(cmd, const nand_pba_t[PLANE_NUM])`）——FTL 仍说 PBA，只是内部转 PAA 后赋给 `cmd->paa` |
| RAID 段 | **一并机械改成 PAA**（`#if CONFIG_FC_RAID`，恒关 ⇒ 本轮**无法编译验证**，逐行人工核对） |
| `command->meta` | **不动**——它仍是扇区 LBA（写进 NAND 的逻辑身份），与本次无关 |

## 二、`src/` 改动

| # | 文件 | 改动 |
|---|---|---|
| A1 | `include/fc/fc_command.h` | `DWORD pba;` → `DWORD paa;`（注释重写）；文件头「三套地址」分界句改为「FTL 交 LBA + PAA（PBA 在 FTL 侧转出），FC 只认 PAA」；`fc_command_plane_span` 内 `pba_to_plane(command->pba)` → `paa_to_plane(command->paa)` |
| A2 | `include/fc/fc_interface.h` | 建链入口签名 `nand_pba_t row_pba` → `nand_paa_t row_paa`；上方 doc 同步 |
| A3 | `src/fc/fc_interface.c` | `ASSERT((uint8_t)pba_to_plane(row_pba) == plane)` → `paa_to_plane(row_paa)`；`descriptor->paa = pba_to_paa((row_pba & ~PLANE_CW_MASK) \| plane_cw)` → `paa_with_cw(paa_with_plane(row_paa, plane), cw)`（`plane_cw` 局部仍用于 `descriptor->meta`，LBA 空间） |
| A4 | `src/fc/fc.c` | 三类机械替换（见下）+ RAID 段 |
| B1 | `include/ftl/ftl_fc_interface.h` | `addr_to_cmd`：`cmd->pba = pba_from_addr(addr)` → `cmd->paa = pba_to_paa(pba_from_addr(addr))`；doc 同步 |
| B2 | `src/ftl/ftl_fc_interface.c` | `ftl_cmd_configure_descriptors`：`row_pba = (cmd->pba & ~PLANE_CW_MASK) \| (plane << PBA_PLANE_SHIFT)` → `row_paa = paa_with_plane(cmd->paa, plane)`；`ftl_cmd_setup_ppa`：`cmd->paa = pba_to_paa(pba)`；`ftl_cmd_setup_ppa_pair`：`cmd->paa = pba_to_paa(pba[0])`，逐 plane 调用改传 `pba_to_paa(pba[i])` |
| B3 | `src/ftl/ftl.c` | 三处 `ftl_pba_gblock(cmd->pba)` → `ftl_paa_gblock(cmd->paa)`（**逐位等价**，`bst.h:162` 里前者本就是后者的展开）；`rr_recover_read` 的形参是 PBA ⇒ 新增 `read_pba = paa_to_pba(topo.ch, topo.ce, cmd->paa)` 还原（与转换前的值逐位相同） |
| C | NAND 层注释 | `nand_dispatch_tables.c` / `nand_dispatch.c` / `onfi_1_0.h` / `nand_dispatch.h` 里描述 `command->pba` 的注释改 `command->paa`（**纯注释，无行为**） |

### A4 明细（`src/fc/fc.c`）——三类机械替换

1. `pba_to_paa(command->pba)` → `command->paa`（擦除单 plane、加载单 plane、Feature、READ_META、单 plane 数据）
2. `pba_to_paa((command->pba & ~PLANE_CW_MASK) \| (plane << PBA_PLANE_SHIFT))` → `paa_with_plane(command->paa, plane)`（擦除多 plane、加载多 plane、`fc_build_request_data_planes` 的 NDEBUG 回退）
3. `pba_to_plane(command->pba)` → `paa_to_plane(command->paa)`；`:1205-1207` 的 ASSERT `pba_to_block/page(command->pba)` → `paa_to_block/page(command->paa)`

**RAID 段**（`fc_stripe_submit`，整段在 `#if CONFIG_FC_RAID` 内）：`pba_to_page/block/lun(data_cmd->pba)` → `paa_to_*`；
`pba_to_ch/ce(data_cmd->pba)` → `(uint8_t)data_cmd->config.ch/ce`；`parity_cmd->pba = pba_from_addr(...)` →
`parity_cmd->paa = paa_with_plane(data_cmd->paa, PLANE_NUM - 1U)`；`:985` → `paa_with_plane(data_cmd->paa, p)`。

### 关键机制（已核实）

- **`config.ch/ce/lun` 独立存在**，`fc_build_dispatch_request`（`fc.c:1266-1268`）的路由取自 `config`，
  **不从 pba 推导** ⇒ 换成 PAA（不含 ch/ce）**不丢路由**。本版把这条写死为结构不变式。
- **PAA 的 plane 与 cw 不相邻**（`[cw][page][plane][block]`）⇒ v24 的 `plane_cw` / `PLANE_CW_MASK`
  建链技巧在 PAA 空间**失效**，改用现成 helper `paa_with_plane()` / `paa_with_cw()`。
- **`ftl_paa_gblock(nand_paa_t)` 已存在**（`include/ftl/bst.h:157`），
  而 `ftl_pba_gblock(pba)` 本就是 `ftl_paa_gblock(pba_to_paa(pba))`（`:162`）
  ⇒ FTL 侧读 `cmd->pba` 的三处换成 `ftl_paa_gblock(cmd->paa)` **逐位等价**。
- **`paa_from_addr()` 忽略 ch/ce**（`nand_config.h:169` 注释已写明）⇒ 测试夹具用 `paa_from_addr(addr)`
  之前必须先把 ch/ce/lun 镜像进 `config`（`fc_test_common.c:332-334` 本就如此，无拓扑损失）。

### ⚠ 生产路径影响（已核实）

- **写进 NAND 的位逐字节不变**：`pba_to_paa()` 的输入与输出在边界处一一对应，只是**转换发生的地点**
  从 FC 层挪到 FTL 层。`desc->paa` / `desc->meta` 的最终值、`plane_paa[]` 的最终值都不变。
- **唯一新增的还原**：`ftl.c` 的 Read Recovery 分支——`rr_recover_read` 的形参是 PBA（内部还要
  `addr_from_pba`），故用 `paa_to_pba(topo.ch, topo.ce, cmd->paa)` 还原。这是 `pba_to_paa` 的逆，
  与转换前的值逐位相同。

## 三、`tests/` 改动

- `tests/unit/fc_test_common.c`：`test_cmd_build` 的 `cmd->pba = pba_from_addr(addr)` →
  `cmd->paa = paa_from_addr(addr)`（`paa_from_addr` 忽略 ch/ce——`:332-334` 已把 ch/ce/lun 镜像进
  `config`，无损失）；注释里的 `cmd->pba` 同步。
- `tests/unit/fc_test_common.h`：`test_plane_cw_bitmap` 的注释 `cmd->pba` → `cmd->paa`。
- `tests/unit/test_fc_level3.c`：BLOCK_UNLOCK 用例的 `cmd->pba = pba_from_addr(...)` →
  `cmd->paa = paa_from_addr(...)`；`#if 0` 快照块的 `CMD_FIELD_U(pba)` → `CMD_FIELD_U(paa)`；注释同步。
- **`tests/unit/test_fc_level2.c` 不动**——其中的 `pba_from_addr(...)` 全是 `meta` 实参（LBA 空间）。

## 四、验证

```bash
make format-check                  # 过
make clean && make tests           # 0 warning 0 error；链接 225212 B；用例数 53（不变）
make                               # 固件，0/0，166708 B
```

**上板（用户跑，本版唯一强判据）**：全套 **53 of 53**。
`make tests` 只证明「编译+链接+RAM 级用例」；**PAA/PBA 位序不同** ⇒ 任何遗漏的 `pba_to_paa` / `pba_to_*`
都会**静默算错地址**，只有真实器件能判。建议上板跑一次完整 `test`（不带过滤串）。

## 五、风险 / 需上板确认（如实记录）

- **RAID 段无法编译验证**（`CONFIG_FC_RAID` 恒关，且按规则不为验证临时打开）⇒ `fc_stripe_submit`
  的改动只能逐行人工核对。
- **PAA 无 ch/ce**：所有路由必须走 `config.ch/ce/lun`。已核实生产路径与测试本就如此，但这是一条
  **新的结构不变式**，已在 `fc_command.h` 写死。
- **`paa_from_addr` 忽略 ch/ce**：任何把 PBA 语义地址喂给它的地方都会静默丢拓扑。
- `tests/integration/test_fc_nand.c` 仍调用已不存在的 `fc_command_configure_descriptors(command)`
  （单参复数版），**早就是坏的**，本轮不动（板测文件）。

## 不做

- 不动 `command->meta` 的语义（仍扇区 LBA）、不动 `OP_COUNT` / op 表 / `fc_is_page_read` / rewrite；
- 不动 `tests/integration/**`、`tests/framework/**`、`ut_main.c`；
- 不翻 `CONFIG_FC_RAID`；不处理 OTP；
- 不改 FTL 侧 API 的名字/签名（`ftl_cmd_setup_ppa*` 仍收 `nand_pba_t`）。

## 出处

- 三套地址位序：`include/nand/nand_config.h`（PAA `:150-260`、PBA `:340-398`）。
- `pba_to_paa()` / `paa_to_pba()`：`nand_config.h:360` / `:389`。
- PAA 便利函数：`paa_with_plane()`（`:246`）、`paa_with_cw()`（`:216`）。
- FTL gblock：`include/ftl/bst.h:157`（`ftl_paa_gblock`）、`:162`（`ftl_pba_gblock`）。
- 建链入口：`src/fc/fc_interface.c:152`；派发：`src/fc/fc.c:1260`。
- FTL 边界：`src/ftl/ftl_fc_interface.c:86/94/115/139/171`；`include/ftl/ftl_fc_interface.h:169`。

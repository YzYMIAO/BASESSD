# FC 层 per-plane 行地址：`fc_command_attach_cw()` 增 `row_paa`

**日期**: 2026-10-07
**相关设计**: `docs/design/fc_nand_design.md` §2.9（接口表 + 两层说明）、§2.10（生命周期）、§25.4（建链，v12 小节）
**前置**: `fc_descriptor_attach.md`（v11 机制 / 策略拆分）、`fc_cw_index_unification.md`（CW 索引空间统一）
**后续**: `ftl_policy_b_per_plane_block.md`（FTL 侧 per-plane 块分配，本改动是其前提）

---

## 概述

v11 把建链拆成「策略 / 机制」两层后，机制原语 `fc_command_attach_cw()` **不收行地址**——
各 plane 的行地址仍由 `fc_build_request_data_planes` 从 `command->paa` **复制 + 替换 plane 位**，
隐含"multi-plane 命令的两个 plane 同 block"。

v12 把这条隐含约束打开：`fc_command_attach_cw(_internal)` 增 `nand_paa_t row_paa` 形参，
`fc_build_request_data_planes` 的 `plane_paa[]` 改为**从 desc 链推导**（v9 起 `cw_mask` 已从链推导，
行地址走同一条路）。于是 multi-plane 命令的两个 plane 可以指向**不同 block**——
Policy B（每 plane 独立分配块 + 坏块替换）在 FC 侧的前提就位。

**对现有调用路径逐字节中性**：现有全部调用者传进来的 `row_paa` 都是
"`command->paa` 的 page/block/lun + 目标 plane"，与 v11 的
`paa_make(paa_to_page(cmd->paa), plane, paa_to_block(cmd->paa), paa_to_lun(cmd->paa), cw)` 完全相同。

---

## 动机

用户问：FTL 层一个 multi-plane 写，要求 `[plane0,block0,page0,cw0-3][plane1,block1,page1,cw0-3]`
（两 plane 不同 block）能办到不。核实后确认这是**主流策略 B**（super block 两 plane 独立分配
物理块 + 坏块替换），且：

- NAND 层**完全就绪**：`nand_dispatch_request_t.plane_paa[PLANE_NUM]` 已是 per-plane，
  multi-plane uop 已发两条独立 `row[0]` / `row[1]`。
- `fc_descriptor_t.paa` **本来就是 per-descriptor**——per-plane 的载体已经存在，只是没人往里填不同值。
- **唯一被写死的是"配对 = 同号"**，散在 FC 两处 + FTL 三处（见 `ftl_policy_b_per_plane_block.md`）。

`src/ftl/allocator.c` 的旧注释「NAND 行地址 plane 只差 BA6 位，block 必须同号」**是错的**——
不是 NAND 约束，是本项目当时的取舍（已在 FTL 侧改动中清掉）。

**用户决策**：FC 层形状 = **desc 链携带地址**（而非新增一个 per-plane 数组或改请求结构），
理由是 v9 起"链即真值"已是既定方向（`cw_mask` 从链推导），行地址走同一条路最一致。

---

## 改动

### 1. `fc_command_configure_descriptor_internal`（`src/fc/fc.c`）

签名增 `nand_paa_t row_paa`：

```c
/* multi_plane > 0：PAA = row_paa 的 block/page/lun + 传入的 plane/cw
 *   （保留 row_paa 的 LUN 字段——多 LUN 并发必需：desc PAA/META/随机种子/ECC 都依赖它）
 * 单 plane：row_paa 已完整描述该 plane 的地址，只改写 CW 位 */
descriptor->paa = (command->config.multi_plane > 0U)
                      ? paa_make(paa_to_page(row_paa), plane, paa_to_block(row_paa), paa_to_lun(row_paa), cw)
                      : ((row_paa & ~NAND_FIELD_MASK(CW_WIDTH)) | cw);
```

新增 `ASSERT((uint8_t)paa_to_plane(row_paa) == plane)`——row_paa 必须就是该 plane 的行地址。

### 2. `fc_command_attach_cw_internal` / `fc_command_attach_cw`（`src/fc/fc.c` / `src/fc/fc_interface.c`）

两者签名同步增 `row_paa` 并透传。公共入口仍是"身份 + `ALLOCATED` 校验 → 转 internal"的薄壳。

### 3. 策略层 `fc_command_configure_descriptors_internal`（`src/fc/fc.c`）

内层循环改为逐 plane 组装同页行地址再传下去——与 v11 及以前逐字节相同：

```c
for (plane = plane_first; plane < plane_last; plane++) {
    /* 本命令所有 plane 共用 command->paa 的行地址（同页语义），只把 plane 位换成目标 plane */
    const nand_paa_t row_paa = paa_make(paa_to_page(command->paa), plane, paa_to_block(command->paa), paa_to_lun(command->paa), 0U);
    for (cw = 0U; cw < CW_NUM; cw++) { ... fc_command_attach_cw_internal(controller, command, plane, cw, row_paa); }
}
```

### 4. `fc_build_request_data_planes`（`src/fc/fc.c`）

`plane_paa[plane]` 从"复制 `command->paa` + 替换 plane 位"改为**扫 desc 链推导**：

```c
uint8_t    planes_with_desc = 0U;
nand_paa_t plane_row[PLANE_NUM];

/* 链走一遍：每个 plane 记下链上第一条 desc 的行地址 */
if ((planes_with_desc & (1U << plane)) == 0U) {
    plane_row[plane] = paa_make(paa_to_page(descriptor->paa), plane, paa_to_block(descriptor->paa), paa_to_lun(descriptor->paa), 0U);
    planes_with_desc |= (1U << plane);
} else {
    /* 同一 plane 的所有 desc 必须指向同一物理位置 */
    ASSERT(paa_to_block(descriptor->paa) == paa_to_block(plane_row[plane]));
    ASSERT(paa_to_page(descriptor->paa) == paa_to_page(plane_row[plane]));
    ASSERT(paa_to_lun(descriptor->paa) == paa_to_lun(plane_row[plane]));
}

/* 组装请求 */
request->plane_paa[plane] = (planes_with_desc & (1U << plane)) ? plane_row[plane]
                                                              : paa_make(page(cmd->paa), plane, block(cmd->paa), lun(cmd->paa), 0U);
```

**链上该 plane 无 desc 时保留旧行为**（从 `command->paa` 复制）——只读 plane1 的 `0xF0` 掩码
命令在 plane0 上没有 desc，不填就会让多 plane uop 退化成 block0/page0。

单 plane 分支也加了一条兜底断言：若该 plane 在链上，其行地址须与 `command->paa` 一致。

---

## 约束

| 约束 | 行为 |
|---|---|
| `row_paa` 的 plane 位 | 必须等于 `plane` 实参（`ASSERT`） |
| 同一 plane 的多个 CW | 必须传同一 `row_paa`——一个 plane 一次只访问一个物理位置（`fc_build_request_data_planes` 的 `ASSERT` 兜底） |
| `row_paa` 的 CW 位 | 被忽略（行地址只含 page/block/lun + plane） |
| `plane` 范围 | 必须落在本命令 plane 段内（单 plane = `paa_to_plane(cmd->paa)`；multi-plane = `[0, multi_plane)`），越段返回 `INVALID_ARGUMENT` |

---

## 行为中性论证

- **策略层路径**：传的 `row_paa` 与 v11 内部组装的表达式逐字节相同。
- **FTL 单 plane 路径**（`addr_to_cmd` / `ftl_cmd_setup_ppa`）：单 plane 分支本来就只用
  `row_paa & ~CW_MASK | cw`，而它们传的 `row_paa` 就是 `command->paa`——等价。
- **`plane_paa[]` 推导**：同页命令下链上任意 desc 的行地址都等于 `command->paa` 的行地址，
  故推导结果与"复制 `command->paa`"相同。
- **erase / SET/GET_FEATURES 分支不动**：非数据命令无 desc 链，仍读 `command->paa`。
- **`fc_command_plane_span` 不动**：仍按 `command->paa` 定 plane 段；`multi_plane > 0` 时恒 `[0, multi_plane)`，
  与行地址来源无关。

---

## 不做的事

- **不动 `fc_stripe_submit` / `fc_data_path`**（`CONFIG_FC_RAID = 0`）。
- **不改测试源文件**（平台规则：测试归用户）；不新建测试。
- **不新增公共 per-plane 数组**：per-plane 的载体是 desc 自己的 `paa`（本已存在）。

---

## 验证

| 项 | 结果 |
|---|---|
| `make format-check` | 通过 |
| debug | 零警告（本段单独编译时为 153288 B，+1088 B vs v11 的 152200 B） |
| release（`NDEBUG`） | 零警告 |
| `make -k tests` | `test_fc_level2.o` **零诊断**；仅卡在**用户维护**的 `test_fc_level1.c`（旧 scheduler API）与 `test_fc_level3.c:48`（旧 `nand_dispatch_set_device_ready`），与本次改动无关（改动前后基线一致） |
| 静态核对 | `row_paa` plane 位不符 → `ASSERT`；同 plane 行地址不一致 → `ASSERT`；越段 plane → `INVALID_ARGUMENT` |

**上板验证未做**（平台规则：用户执行）。本段为纯拆分 / 参数透传，正常路径行为不变，
常规 `info → wr 0 2 → rd 0 2 c → tr 0 → fl → info` 回归即可。

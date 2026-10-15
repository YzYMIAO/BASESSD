# CW 索引空间统一：跨 FC/NAND 边界只剩一个坐标

**日期**: 2026-10-06
**相关设计**: `docs/design/fc_nand_design.md` §2.2、§4.2（图 2）、§13.3 / §13.4 / §13.5、§14.3、§17.1 注 / §17.10、§23.8、**§25.11（新增）**
**前置**: `fc_flat_cw_mask_cq_param.md`（v7 flat 掩码）、`fc_cw_subset_read.md`（v6 按 CW 子集读）

---

## 概述

v7 把 `param.cw_mask` 改成了 flat 绝对编号（`bit n = plane*CW_NUM + cw`），但**掩码下游不是 flat 的**：
FC 建请求时仍按 plane 分桶、在每个 plane 内**密排**成槽位（`cws[plane][slot]`），NAND 侧再用
`cw_count[PLANE_NUM]` + 走减（`nand_dispatch_slot_to_plane_cw()`）把槽位换算回 `(plane, ordinal)`。
于是"掩码"与"槽位"是两套坐标，靠一条隐式不变量对齐：

> 链序 == plane-major（先 plane 0 的全部置位 CW，再 plane 1）

本次（**v9**）把这条不变量连同整套中间坐标一起删除，跨边界统一为**一个**坐标——
flat 绝对 CW 号 `n = plane*CW_NUM + cw`。行地址从 CW 项里拆出来，改由 per-plane 的
`plane_paa[PLANE_NUM]` 承载。

净效果：**532 行删除 vs 442 行新增**（7 个文件）。

---

## 动机（用户提出）

> 我的意见是如果 `param.cw_mask` 是稀疏的，下面应该也是稀疏的，统一一下如何，是不是增加判断的消耗

两套坐标的代价有三：

1. **隐式不变量**：链序一旦与 plane-major 不符，数据静默落到错误的列——而链序由建链处的枚举顺序
   决定，离消费点很远（§25.3 曾列过 5 个必须一起改的消费点）。
2. **空 plane 的补丁**：多 plane 加载的行地址取自 `cws[plane][0].paa`，空 plane（如只读 plane1 =
   `0xF0`）必须补一条 CW=0 的 PAA，否则行地址退化成 block0/page0（§25.8 风险 6）。
3. **两套坐标的换算代码**：`cw_count[PLANE_NUM]`、`slot_to_plane_cw()`、`total_slots()`，以及
   `completion_multi_plane`——后者与 `completion_all_cws` 在统一后**逐字节相同**。

**性能中性**（用户问的"判断的消耗"）：稀疏遍历由 `nand_dispatch_cw_next(mask, n)` 的位扫描完成，
替代原来的"槽位走减"，两者都是 ≤ 8 次迭代的小循环；掩码是集合，落位顺序无关紧要。
零额外 RAM（`plane_paa[PLANE_NUM]` 由删掉的 `cw_paa[][]` 腾出的空间承担）。

---

## 改动

### 1. 契约结构（`include/nand/nand_dispatch.h`）

删 `nand_dispatch_cw_info_t`（`{nand_paa_t paa; uint8_t *buffer;}`），请求结构改为：

```c
typedef struct {
    uint16_t command_id;
    uint8_t  operation;
    uint8_t  ch, ce, lun;
    uint32_t cw_mask;                    /* bit n = plane*CW_NUM + cw */
    uint8_t  plane_count;
    uint8_t  plane;
    uint8_t  page_type : 2;
    uint8_t  reserved  : 6;
    uint8_t  feature_addr;
    nand_paa_t plane_paa[PLANE_NUM];     /* 各 plane 的行地址承载 PAA */
    uint8_t   *cw_buffers[CW_TOTAL_MAX]; /* [n]，仅 n ∈ cw_mask 有效 */
} nand_dispatch_request_t;
```

完成项的 `cw_status[CW_TOTAL_MAX]` / `ecc_bit_count[CW_TOTAL_MAX]` **位宽不变**，但下标语义从
"plane-major 链走位"改为 flat 绝对号 `n`——与请求掩码、FC 侧 `param.cw_mask` 同一空间。

### 2. 掩码遍历工具（`src/nand/nand_dispatch_internal.h`）

新增两个 `static inline`（放头文件是因为 `nand_dispatch.c` 与 `nand_dispatch_tables.c` 共用；
`static inline` 无外部链接，`NDEBUG` 下也不产生 `-Wunused-function`）：

```c
static inline uint8_t nand_dispatch_cw_next(uint32_t cw_mask, uint8_t n);   /* ≥n 的下一个置位，哨兵 CW_TOTAL_MAX */
static inline uint8_t nand_dispatch_cw_first(uint32_t cw_mask);             /* 最低置位，空掩码返回 0 */
```

### 3. 冗余状态删除（`nand_dispatch.c` / `nand_dispatch_internal.h`）

- `task->cw_count[PLANE_NUM]` 删除 —— "本命令有几个 CW"= `popcount(cw_mask)`。
- `task->cw_paa[PLANE_NUM][CW_NUM]` 删除 —— PAA 的 page/plane/block/lun 是命令级常量（现在住
  `plane_paa[]`），CW 号 = `n % CW_NUM`。**该数组整体冗余**，不是换下标。
- `nand_dispatch_task_total_slots()` / `nand_dispatch_slot_to_plane_cw()` 删除。
- `task->cw_index` 语义改为 **flat 绝对号 `n`**（生产：`task_load` 置掩码最低位；消费：
  `completion_per_cw` 用它当 `cw_status[n]` 下标并作为 `failed_descriptor_id`）。

`nand_dispatch_task_cw_chain()` 的推进改为 `n = nand_dispatch_cw_next(mask, cw_index + 1)`，
列地址/plane 直接由 `n` 算出：

```c
uint8_t  plane     = (uint8_t)(task->cw_index / CW_NUM);
uint8_t  cw        = (uint8_t)(task->cw_index % CW_NUM);
uint16_t cw_column = (uint16_t)(cw * CW_SIZE);
task->request = (nand_fc_request_t){ .column = cw_column,
                                     .row = {paa_to_nand_row(task->plane_paa[plane])},
                                     .data = task->cw_buffers[task->cw_index],
                                     .data_length = (uint16_t)CW_SIZE };
```

### 4. Completion / Request handler（`nand_dispatch_tables.c`）

- `completion_all_cws` 按掩码遍历写 `cw_status[n]`；**`completion_multi_plane` 删除**，3 个表项
  （`OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT` / `OP_PROGRAM_PAGE_MULTI_PLANE` /
  `OP_ERASE_BLOCK_MULTI_PLANE`）改指 `completion_all_cws`。
- `completion_single` 加掩码分支——**这是本次唯一的"潜伏回归修复"**：它原写 `cw_status[0]`，
  而 FC 现在按 `n` 读，故 `OP_READ_PAGE_LOAD`（带掩码、走 `completion_single`）的失败状态会丢。
  改为"掩码为空才写 `[0]`（RESET/STATUS/块锁等常态，FC 非 page read 聚合是全表 OR），否则按 `n`"。
- 12 个 `req_*` 全部改按掩码遍历：写侧拼接偏移 `(n % CW_NUM) * CW_SIZE`（单 plane）/
  `(n / CW_NUM) * PAGE_SIZE + (n % CW_NUM) * CW_SIZE`（multi-plane）；行地址一律
  `paa_to_nand_row(task->plane_paa[...])`。

### 5. FC 侧（`src/fc/fc.c`）

- `fc_build_request_data_planes`：沿 desc 链把每个 desc 落位到**它自己 PAA 算出的** `n` 那一格
  （置掩码位 + 写 `cw_buffers[n]`），落位顺序无关紧要。末尾按 plane 集合填 `plane_paa[]`
  （含空 plane，**取代**旧的"空 plane 补一条 CW=0 PAA"补丁）。
- `fc_build_dispatch_request`：`OP_ERASE_BLOCK` 置本 plane 的 CW0 位；`OP_ERASE_BLOCK_MULTI_PLANE`
  逐 plane 置 CW0 位；`SET/GET_FEATURES` 置本 plane CW0 位 + `cw_buffers[plane*CW_NUM] =
  g_fc_return_staging[command_id]`（旧的 `nand_operation_has_data ? config.cw+1 : 0` 槽位数口径删除
  ——Feature 恒 4 字节单次传输，与 `config.cw` 无关）。
- `fc_complete_from_nand` Step 1：按 desc 自己的 PAA 现算 `n`，`descriptor->status = cw_status[n]`，
  不再沿链数序号。
- `fc_stripe_submit`（`CONFIG_FC_RAID`，当前编掉）：`cw_count[p]` / `cws[p][cw]` 机械适配为
  `plane_paa[p]` + 掩码置位 + `cw_buffers[cw]` / `cw_buffers[CW_NUM + cw]`。

### 6. 新增断言

| 位置 | 断言 | 理由 |
|------|------|------|
| `fc_build_request_data_planes` | 同一 `n` 不被两条 desc 覆盖 | 掩码是集合，重复置位会**静默丢数据** = desc 链构造错误 |
| `fc_build_request_data_planes` | 单 plane 下 `popcount(cw_mask) == 链上 desc 数` | 多一条或少一条都说明有 desc 落到了别的 plane |
| `nand_dispatch_enqueue` | `cw_mask` 不超出低 `CW_TOTAL_MAX` 位 | 掩码来自 FC，越界位即构造错误 |
| `fc_complete_from_nand` Step 1 | `n < CW_TOTAL_MAX` | 由 PAA 现算，越界即 PAA 构造错误 |

---

## 影响面

- **满掩码（全部 FTL 现状）逐字节等价**：`n` 与旧槽位一一对应，落位/取数/回写结果相同。
  差异只在稀疏掩码下，且差异是"修对了"（旧写法依赖链序 == plane-major）。
- **契约变更**：`nand_dispatch_request_t` / `nand_dispatch_completion_t` 是 FC↔NAND 的唯一接口。
  全仓无测试构造这两个结构（`tests/integration/test_fc_nand.c` 未编入 `src/main.c`），
  故无测试源需改。
- **用户维护的测试**：`test_fc_level1.c`（旧 scheduler API）与 `test_fc_level3.c:48`
  （旧 `nand_dispatch_set_device_ready`）仍失败，与本次改动无关。
- **上板未验证**（平台规则：用户执行）。

---

## 验证

| 项 | 结果 |
|---|---|
| `make format-check` | 通过 |
| debug | 零警告（152128B） |
| release（`NDEBUG`） | 零警告（87016B）——`ASSERT` 被裁空后无 `-Wunused-function`（新工具是 `static inline` 头文件函数） |
| `make -k tests` | 仅卡在用户维护的 `test_fc_level1.c` / `test_fc_level3.c:48`，与本次改动无关 |
| `src/` 静态审计 | `cws[` / `cw_paa` / `slot_to_plane` / `cw_info` / `completion_multi_plane` 全仓零命中；`cw_count` 仅剩 stripe/RAID 的局部计数器（`fc_stripe_submit` / `fc_data_path.c` parity 循环） |

**上板验证未做**（用户执行）。建议回归：常规 `wr/rd/tr/fl/info` 序列（覆盖满掩码单/多 plane 读写擦
与 FEATURES）；若要覆盖稀疏掩码，需在 FTL 侧造出非满 `param.cw_mask` 的读命令（当前无生产者）。

# v53：命令级 buffer 归属位域 —— 谁申请 / 谁释放（`fc_cmd_conf_t.buf_own : 2`）

- **日期**：2026-10-11
- **范围**：**src + 测试 + 文档**。`include/fc/fc_command.h`、`include/fc/fc_interface.h`、
  `src/fc/fc_interface.c`、`src/fc/fc.c`、`tests/unit/test_fc_level3.c`、本文档。
  **`src/ftl/**` 一行不动**。
- **介质格式**：无变更。
- **测试**：用例数 **56 → 57**（Level 3 新增 `buf_own_release`）。
  `make format-check` 过；`make clean && make tests` **0 warning 0 error**、
  `.bin` **238980 B**（v52 是 234988 B）。
- ✅ **上板已验**（2026-10-11，seed `0x5a4f21e4`）：
  `test /fc-nand/lifecycle/buf_own_release` **[ OK ] [ 2.05 ms ]**（`base=0`）、
  完整 `test` **57 of 57 (100%)**、0 skipped——既有 56 条逐字不变
  （`mixed_pipeline` 165.26 ms、`chain_priority_lock` 597.13 us）。

## 〇、起因

用户要一个**命令级 2-bit 位域**，表达「这条命令的数据 buffer 由谁申请、由谁归还」，把上层
（FTL）频繁的 buffer 申请/释放动作按需**下沉到 FC 层**。四档语义（用户定稿，且用户自己把
编号改成了「`00` = 今天的生产行为」这一版）：

| 值 | 申请 | 释放 | 语义（用户原话） |
|---|---|---|---|
| `00` | **FC** | 上层 | 读命令路径，需要拿到 buffer，上层把控 buffer 生命周期 |
| `01` | FC | **FC** | 读命令，不需要拿到 buffer，只需要知道命令状态 |
| `10` | 上层 | 上层 | 上层把控数据场景 |
| `11` | 上层 | **FC** | 写命令等场景，只要命令状态 |

> 「fc_command_configure_descriptor 只有 (cmd, plane, cw, row_paa) 四个形参）这个就是给 FC
> 申请用的，要不改一下编号？ … 这样改动是不是小点」

## 一、两条已拍板的语义决定

1. **编码刻意让 `00` = 今天的生产行为。** `fc_cmd_conf_t` 恰好 32 bit，末字段原为
   `reserved : 3`——正好放 2 bit。命令出池整体清零（`(fc_command_t){0}`，`src/fc/fc.c`）
   ⇒ 新字段默认 0 = `00` = FC 申请 + 上层释放 = **既有语义** ⇒ **`src/ftl/**` 一行不用改**。
   这是本方案成立的前提，也是相比用户最初编号的关键改动（原编号里 `00` 是「上层申请」，会让
   默认值变成新模式，FTL 必须同步改）。
2. **「上层释放」= 上层调 `fc_command_release`**（读法 A）：`fc_command_release` **恒归还链上
   所有未归还的 buffer，不管谁申请的**。「desc 链 = 命令的资源集」这条不变式保留，
   **释放原语一行不改** ⇒ 无双重释放、无泄漏。于是 `10` 与 `00` 的差别**只在谁调
   `fc_buffer_alloc()`**；`01`/`11` 的差别只在 **FC 是否在完成时自动归还**。

## 二、现状（已核实）

| 事实 | 位置 |
|---|---|
| `fc_cmd_conf_t` 恰 32 bit，末字段 `reserved : 3` | `include/fc/fc_command.h:178` |
| 命令出池整体清零 ⇒ 新字段默认 0 | `src/fc/fc.c:434`（`fc_command_alloc_internal` 的 `(fc_command_t){0}`） |
| buffer **申请**在 FC 建链时自动做（在 `fc_command_chain_append` **之后**） | `src/fc/fc_interface.c:217`（挂链）/ `:228`（申请） |
| buffer **释放**在上层调 `fc_command_release` 时做，守卫 `buffer_id != FC_BUFFER_ID_INVALID` | `src/fc/fc.c:497-513` |
| `fc_buffer_alloc()` / `fc_buffer_release()` **已是公开 API**（「手动分配接口」） | `include/fc/fc_interface.h:119,122` |
| `fc_resource_stats_t.buffer_used` 可观测池占用（单测判据） | `src/fc/fc_interface.c:563-572` |
| 建链入口调用者：FTL ×2、RAID parity ×1、单测 | `src/ftl/ftl_fc_interface.c`、`src/fc/fc.c`（`CONFIG_FC_RAID=0`，休眠） |
| `descriptor->config = command->config` 整体拷贝 ⇒ desc 侧零改动 | `src/fc/fc_interface.c:212` |

⇒ 今天生产路径 = FC 申请 + 上层释放 = **`00`**。

## 三、改动清单

### 1. 字段与枚举 —— `include/fc/fc_command.h`

`fc_cmd_conf_t` 末两行由 `reserved : 3` 改为：

```c
DWORD buf_own  : 2; /* fc_buf_own_t：谁申请 / 谁释放（bit1=申请方 0=FC 1=上层；bit0=释放方 0=上层 1=FC）。 */
DWORD reserved : 1; /* Reserved for future configuration. */
```

结构体仍 4 字节（8+2+2+2+2+1+2+3+1+1+2+3+2+1 = 32）。新增：

```c
typedef enum
{
    FC_BUF_OWN_FC_ALLOC_UPPER_FREE    = 0U, /* 00：FC 申请，上层释放（默认） */
    FC_BUF_OWN_FC_ALLOC_FC_FREE       = 1U, /* 01：FC 申请，FC 释放 */
    FC_BUF_OWN_UPPER_ALLOC_UPPER_FREE = 2U, /* 10：上层申请，上层释放 */
    FC_BUF_OWN_UPPER_ALLOC_FC_FREE    = 3U, /* 11：上层申请，FC 释放 */
} fc_buf_own_t;

#define FC_BUF_OWN_ALLOC_BY_UPPER (0x2U) /* bit1：1 = 上层申请 */
#define FC_BUF_OWN_FREE_BY_FC     (0x1U) /* bit0：1 = FC 完成时归还 */
```

### 2. 建链入口 —— `include/fc/fc_interface.h` + `src/fc/fc_interface.c`

4 参 `fc_command_configure_descriptor(cmd, plane, cw, row_paa)` **签名不动**（FTL 的 2 个调用点、
RAID parity、既有单测全部无需改），退化为薄包装：

```c
fc_status_t fc_command_configure_descriptor(fc_command_t *command, uint8_t plane, uint8_t cw, nand_paa_t row_paa)
{
    return fc_command_configure_descriptor_with_buffer(command, plane, cw, row_paa, FC_BUFFER_ID_INVALID);
}
```

新增 5 参公开入口，函数体 = 现有函数体 + 两处分支：

- **校验段**（与既有 ASSERT 同批、出池之前）：
  ```c
  if ((command->config.buf_own & FC_BUF_OWN_ALLOC_BY_UPPER) != 0U) /* 上层申请 */
  {
      ASSERT((buffer_id != FC_BUFFER_ID_INVALID) && (buffer_id < FC_BUFFER_POOL_DEPTH));
  }
  else /* FC 申请（默认 00/01）：上层不得自带 buffer */
  {
      ASSERT(buffer_id == FC_BUFFER_ID_INVALID);
  }
  ```
- **机制段**（`fc_command_chain_append` 之后）：
  ```c
  if ((command->config.buf_own & FC_BUF_OWN_ALLOC_BY_UPPER) != 0U)
  {
      descriptor->buffer_id = buffer_id;   /* 上层已 fc_buffer_alloc 好，无失败路径 */
      return FC_STATUS_OK;
  }
  /* FC 申请：原样保留既有失败语义（池空 → RESOURCE_EMPTY，desc 已挂链、buffer_id 留 INVALID） */
  alloc_id = fc_buffer_alloc_internal(&g_fc_controller);
  if (alloc_id == FC_BUFFER_ID_INVALID) return FC_STATUS_RESOURCE_EMPTY;
  descriptor->buffer_id = alloc_id;
  return FC_STATUS_OK;
  ```

⚠ **NDEBUG**：上面两条 ASSERT 在 release 下被裁 ⇒ 非法 `buffer_id` 会一路带到
`fc_buffer_get_base` 的 OOB。这与本仓「配置错误一律 ASSERT」的既有约定一致，接受；
**申请/释放的判定必须写成运行期 `if`，绝不放进 `ASSERT`**（`src/platform/stm32h743/crc.c:47-51`
有先例：release 下把副作用裁掉）。

### 3. 释放分支 —— `src/fc/fc.c`

新增 static helper（沿 desc 链归还 buffer 并把 `buffer_id` 置 `INVALID`，**幂等**）：

```c
static void fc_command_release_buffers_internal(fc_t *controller, fc_command_t *command);
```

在 `fc_complete_from_nand` 的 **Step 3 组装完 CQ 条目之后、`fc_complete_internal` 之前**插入
**Step 4**：

```c
if ((command->config.buf_own & FC_BUF_OWN_FREE_BY_FC) != 0U)
{
    fc_command_release_buffers_internal(controller, command);
}
```

落点安全性（已逐条核对）：`fc_complete_internal` 只 push CQ；`fc_poll_completion_internal`
只 pop；NAND sink 在 `fc_complete_from_nand` 之后只做 RAID parity 清理；`fc_get_cmd_ecc_max`
读 desc 不读 buffer。**Step 3 之后没有任何读者碰 buffer**。放在 `fc_complete_internal` 之前还能
顺带覆盖 CQ 满强制 COMPLETED 的 fatal 分支。

**`fc_descriptor_chain_release_head_internal` 一行不改**（读法 A）：

- `01`/`11`（FC 释放）：完成时已归还并抹成 `INVALID` ⇒ 之后 `fc_command_release` 跳过 ⇒ 无双重释放；
- `01`/`11` 但命令**没走完成**（建链中途失败 / 提交前主动 release）：buffer 仍有效，链释放照常归还 ⇒ 不泄漏；
- `00`/`10`（上层释放）：`fc_complete_from_nand` 不碰，链释放照常归还 ⇒ 与今天逐字相同。

### 4. RAID parity（休眠，`#if CONFIG_FC_RAID`）—— `src/fc/fc.c`

`parity_cmd->config = data_cmd->config;` 会**继承** `buf_own`，而 parity 是 FC 内部命令
（FC 申请 + FC 释放）。在其后补一行：

```c
parity_cmd->config.buf_own = FC_BUF_OWN_FC_ALLOC_FC_FREE;
```

⚠ RAID 段恒关、**无法编译验证**（v47 同款先例）。本版只加这一行，**不翻开关**。

### 5. 单测 —— `tests/unit/test_fc_level3.c`

并入 `g_fc_lifecycle_tests[]`（`MUNIT_TEST_OPTION_NONE`）。判据 =
`fc_get_resource_stats()->buffer_used` 的**增量**（测试不 include `fc_internal.h`）。
载体用 `OP_READ_ID` 单 CW 链（单 plane、`has_data=1`、无需建块）。

```
Step 0  记基线 buffer_used = B
Step 1  模式 00（默认，必须逐字不变）：test_cmd_build → 提交前 B+1 → 完成仍 B+1
        → fc_command_release → 回 B
Step 2  模式 01（FC 申请 / FC 释放）：test_cmd_build → 提交前 B+1 → 完成即回 B（自动归还）
        → 再 fc_command_release 仍 B（幂等，无双重释放）
Step 3  模式 11（上层申请 / FC 释放）：fc_buffer_alloc()（B+1）→ 手工建链（5 参）→ 完成即回 B
        → 再 fc_command_release 仍 B
Step 4  模式 10（上层申请 / 上层释放）：fc_buffer_alloc()（B+1）→ 手工建链 → 完成仍 B+1
        → fc_command_release → 回 B      ← 读法 A 的回归守卫
Step 5  「建了链但没提交」：模式 01 建链后直接 fc_command_release → 回 B（不泄漏）
Step 6  空闲链完整性守卫（抓双重释放造成的重复入链）：做完 01 与 11 后连续 fc_buffer_alloc()
        FC_BUFFER_POOL_DEPTH 次，断言 id 两两不同，再全部 release
```

模式 `10`/`11` **不能**用 `test_cmd_build`（它走 `ftl_cmd_configure_descriptors`，即 4 参入口）；
本文件内新增 `l3_buf_own_build_upper()`，照 `test_cmd_try_build`（`fc_test_common.c:343-350`）
的样子手工建：`fc_command_alloc` → 填 `config`（含 `buf_own`）/`ch`/`ce`/`lun`/`module`/`paa`
→ 单次 `fc_command_configure_descriptor_with_buffer`。跑完用现有 `test_submit_wait` 抽干夹具，
不新造基础设施。

### 6. 文档

- `docs/design/fc_nand_design.md`：头部 changelog 加 v53 条；§12 新增 **§12.10 buffer 归属位域**；
  §13.3.1 的字段贯穿表**本就有错**（漏 `multi_plane`、`reserved` 写成 6），一并订正并加
  `buf_own : 2`（❌ 不过 dispatch——纯 FC 侧资源策略）。
- `docs/design/fc_nand_ut.md`：用例表加新用例行 + 新增「buffer 归属用例（v53）」小节、
  版本表加 v1.35、四处用例数 14 → 15。
- 本文件。

## 四、存储主控怎么做（回答用户的第二问）

| 类型 | buffer 来源 | 谁释放 | 对应本设计的档 |
|---|---|---|---|
| 企业级 NVMe（有 DDR） | Host 给**物理地址**（PRP/SGL），控制器 DMA 直达 | **Host 自己**，控制器从不 free | `10` 上层申请/上层释放 |
| DRAM-less 主控（本项目最接近） | 控制器内部 **SRAM Buffer Pool + ID**（~128 个） | 控制器自己（命令 retire 时归还 free list） | `01`/`11` FC 申请/FC 释放 |
| UFS | 两层：Host 侧 PRDT 物理地址 + 内部 SRAM pool | 分层各自管 | 混合 |

要点：真实主控里**「谁释放」不是靠一个位域，而是靠 buffer 的归属类**——host DMA buffer 归 host、
内部 staging buffer 归控制器。控制器内部的 buffer manager 通常带 **refcount**，因为同一个 buffer
会被流水线多级引用（ECC 引擎 / DMA 引擎 / 固件）；本项目的 buffer 是**单引用**（只有 desc 链持有），
所以 0/1 占用位 + 一次显式归还就够——2-bit 归属位是把「谁申请 / 谁归还」**显式化**，补齐这条。
（§12.2 已有完整行业对照；§12.6 的 `own_buf:1` 双模是本设计的直系前身，§12.9 因 UAF 风险否决了
裸指针那半；本设计保留 `buffer_id` 单一路径、只用 2 bit 表达归属，规避了该风险。）

## 五、验证

```bash
make format-check              # clang-format-15
make clean && make tests       # 0 warning 0 error；用例数 56 → 57、.bin 238980 B
```

上板（2026-10-11，seed `0x5a4f21e4`）：

```
test /fc-nand/lifecycle/buf_own_release    # [ OK ] [ 2.05 ms ]（base=0）
test                                       # 57 of 57 (100%)、0 skipped
```

## 六、风险 / 不做

- **默认值即既有语义** ⇒ 生产路径（FTL 全走 `00`）**逐字不变**，这是能上板的前提。
- **不动** `src/ftl/**`、`tests/integration/**`、`tests/framework/**`、`ut_main.c`；**不翻** `CONFIG_FC_RAID`。
- 新模式 `01`/`11` 下，完成之后上层**不得**再调 `fc_get_cmd_data_ptr`（buffer 已归还、`buffer_id`
  已 `INVALID`，该函数会返回 NULL）——写进注释契约。
- `fc_command_validate` **不需要改**：`buf_own` 不是 operation 派生量，其校验归建链入口。

# v21：FC 层 C 档·形态三「缺失的入参/边界校验」

- **日期**：2026-10-08
- **范围**：`src/fc/`（队列 / 控制器 / BCH 引擎 / QC-LDPC 引擎 / ECC 统一入口）
- **前置**：v18（§十三）/ v19（§十四）/ v20（§十五）—— 三档清完了「**有**守卫但把配置错误当返回码」。

## 背景

v18~v20 治的是「配置错误 → 返回码」。C 档是**反向病**：配置错误**既不返回码也不 ASSERT**，静默吞掉。按病灶拆三形态：

| 形态 | 表现 |
|---|---|
| 一 · 静默跳过 | `if (x != NULL) { ... }` 把该做的活整段略过 |
| 二 · 静默降级 | 出问题就 `break` / 回退默认值继续跑 |
| 三 · **完全不校验** | 拿调用者给的 id/size **直接用**，一个守卫都没有 |

本轮只做**形态三**：判据最清楚（全是判据 2「值从哪来」），且其中藏着 C 档唯一真会写坏内存的点。形态一 / 二另议。

## 改动

| 文件:位置 | 缺口 | 改动后 |
|---|---|---|
| `fc_queue.c` `fc_submission_queue_pop_n` | `count` 无上界 | `ASSERT(count <= queue->count)` |
| `fc.c` `fc_command_state_get` | 越界 → 静默回退 `FC_COMMAND_STATE_FREE` | `ASSERT(command_id < FC_COMMAND_POOL_DEPTH)` |
| `fc.c` `fc_descriptor_owner_get` | 越界 → 静默回退 `FC_COMMAND_ID_INVALID` | `ASSERT(descriptor_id < FC_DESCRIPTOR_POOL_DEPTH)` |
| `fc.c` `fc_buffer_state_get` | 越界 → 静默回退 `0U` | `ASSERT(buffer_id < FC_BUFFER_POOL_DEPTH)` |
| `fc.c` `fc_buffer_id_to_real_buffer_internal` | 零校验直接索引池 | `ASSERT(buffer_id < FC_BUFFER_POOL_DEPTH)` |
| `fc.c` `fc_buffer_release_internal` | 零校验直接索引池 | `ASSERT(buffer_id < FC_BUFFER_POOL_DEPTH)` |
| `fc_bch_engine.c` `fc_bch_encode` | `bch` / `data` 零校验 | 两条 `ASSERT`，**前置**于任何解引用 |
| `fc_bch_engine.c` `fc_bch_decode` | 缺 `bch` 校验 | 补 `ASSERT(bch != NULL)`，**置于函数体首条语句** |
| `fc_ldpc_qc_engine.c` `fc_ldpc_qc_encode` | `qc` / `data_in` / `data_out` 零校验 | 三条 `ASSERT` |
| `fc_ldpc_engine.c` `fc_ldpc_encode` | `data_length` 无上界 | `ASSERT(data_length <= FC_BUFFER_DATA_SIZE)` |
| `fc_ldpc_engine.c` `fc_ldpc_decode` | `data_length` 无上界 | 同上 |

**头文件契约同步**：`fc_queue.h`(pop_n)、`fc_internal.h`(三 getter + 两 buffer 内部)、`fc_bch_engine.h`(encode/decode)、`fc_ldpc_qc_engine.h`(encode)、`fc_ldpc_engine.h`(data_length 范围)。

## 关键点

### 1. 唯一的内存破坏点

`fc_ldpc_*` 的 `data_length` 无上界是 C 档里**唯一真会写坏内存**的：

- encode LDPC 分支：`uint8_t tmp[FC_BUFFER_POOL_CW_SIZE]`(528) + `memcpy(tmp, data, data_length + 8)` → `data_length > 520` 即**栈溢出**。
- encode BCH 分支：`memcpy(data + data_length, ecc_buf, 8U)` → **写穿 CW buffer**。
- decode BCH 分支：`recv_ecc = data + data_length` 后读 8B → 越界。

⚠ **`fc_bch_decode` 的上界断言挡不住这个**：BCH(m=13, t=4) 下 `n - ecc_bits = 8139`，`8 × 521 = 4168 < 8139` 照样通过。所以必须在 `fc_ldpc_*` 入口加 `<= 520`（= `FC_BUFFER_DATA_SIZE`）。

### 2. 断言必须在解引用之前

`fc_bch_encode` / `fc_bch_decode` 的 `bch` 在 `BCH_ECC_WORDS(bch)`（`l` / `ecc_words` 的初始化式）处**已被解引用**。断言若写在那个声明之后，等于没有——故 `fc_bch_decode` 的 `ASSERT(bch != NULL)` 必须放在函数体**首条语句**（C99 允许声明出现在语句之后）。

### 3. 死代码 clamp

三个 getter 原先的越界回退分支是**死代码**——6 个调用点全在范围内（池派生 id / 循环上界 / 前置 `fc_buffer_id_is_valid`）。`FC_COMMAND_STATE_FREE` 回退尤其误导：把"没有这个槽位"说成"槽位空闲可用"。

## 辨析（保留不动）

- `pop_n(ids == NULL)` / `fc_bch_encode(..., ecc == NULL)` / `fc_ldpc_decode(..., err_bits == NULL)`：**有意的可选参数语义**，非调用者错误。
- `fc_ldpc_qc_encode` 的 `g_initialized` 静默返回：运行时状态（判据 3），void 返回无从给码。
- `fc_ldpc_qc_encode` 的 `qc` 参数 vestigial（编码只用文件内静态表）本身另议——本轮只补断言做契约对称。

## 连带发现（顺带修）

`fc_ldpc_engine.c` **未 include `fc.h`**。新增的 `FC_BUFFER_DATA_SIZE` 与 LDPC 分支既有的 `FC_BUFFER_POOL_CW_SIZE` 同出 `fc.h`——后者所在分支（`#if (FC_ECC_ALGO == FC_ECC_ALGO_LDPC)`）默认不编译，故该缺失**潜伏至今**。补 `#include "fc.h"` 后该分支首次可编译。

## release 陷阱

`fc_ldpc_qc_encode` 的 `qc` 在 NDEBUG 下断言裁空 → `-Wunused-parameter`；用 `(void)qc;` 吞掉。三个 getter / 两个 buffer 内部函数的参数在 return 表达式里用；`pop_n` 的 `count` 在 for 循环里用——均不产生 unused。

## 验证

- 四档（debug / release / RAID=1 debug / RAID=1 release）**0 warning / 0 error**。
- `-DFC_ECC_ALGO=FC_ECC_ALGO_LDPC` 补 include 后 **0 error**（余 1 条 pre-existing `fc_ecc_init_bch defined but not used`，属该实验分支既有，非本轮引入）。
- `make -k tests` **29 error / 7 warning**，与基线逐项 `diff` 一致，全部落在 `tests/`。
- `make format-check` passed。
- **上板未做**（项目硬规则：`make tests` 上板由用户执行）。

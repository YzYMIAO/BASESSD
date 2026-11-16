# 负例测试框架设计（分层故障注入）

> 状态：活跃设计文档。框架与首批负例已落地；2026-11-15 release 全量 UT `306/306`、3 SKIP。
> 用例清单以测试源注册表与 `test --list` 为准；各 UT 设计文档只写模块设计点与代表用例（本文件只写框架与 seam 语义）。
> 范围：`fc-nand` / `ftl` / `hil` 三层的负例（negative test）注入框架。
> 关联：`docs/design/testing_guide.md §11`（负例执行清单）、`docs/design/ftl_ut_design.md`、
> `docs/design/fc_nand_ut.md`、`docs/design/hil_ut_design.md`、`docs/design/known_risks.md`。
> 本文是**框架设计**；具体用例由各 UT 文档的用例清单维护（测试源由用户维护，R4）。

---

## 0. 目的与判据

### 0.1 为什么要单独的负例框架

正例（happy path）只证明「正常情况下没坏」；负例证明「故障/非法输入时不会静默写坏、丢数据或挂死」。
风险表中绝大多数 `待上板` 条目的残余都是负例：

> 常规回归通过；**残余：FAIL 注入 / 掉电 / 超时 / 多圈 / 混优先级**

继续跑 UT/H2testw 不会提高这类条目的闭合率；要闭合必须能**主动、可重复地制造故障**。

### 0.2 判据（与 `CLAUDE.md` 一致）

- **C7**：每条负例必须先在**未修复的实现**上变红（证明有区分力），修复后转绿；恒真断言不算覆盖。
- **C8**：必须给出「故障确实发生」的证据（注入计数、状态位、日志），不能只看返回码。
- **H14**：注入只存在于测试镜像；产品构建零运行期开销（宏隔离，§2.1）。
- **R4**：注入代码属于生产源码的测试分支；用例属于测试源（用户维护）。
  **用例编写归属待用户拍板**（只写框架 vs 连用例骨架一起写）。
- **P3**：新增 `ASSERT` 前先读 `docs/design/assertion_guidelines.md`（判据 9/10）。

---

## 1. 借鉴与出处

本仓**没有**可直接链接的通用方案：目标是裸机 STM32、无 OS、无动态加载；下面这些工具
用于借鉴**模型**，不是直接依赖。实现形态是**本仓自拟**。

| 工具 / 项目 | 机制（出处） | 借什么 | 为什么不直接用 |
|---|---|---|---|
| [libfiu](https://github.com/albertito/libfiu) | C 库（public domain）：故障点 + 控制 API + 次数/概率 | 「故障点 + 控制 API + 按次数」的 API 形状 | userspace/POSIX 向，裸机无法链接 |
| [QEMU blkdebug](https://qemu-project.gitlab.io/qemu/devel/testing/blkdebug.html) | 规则式：`event` + `errno` + `sector` + `once` | 「按事件/地址/次数定点」的三维 key | 依赖 QEMU/Linux 块层 |
| [dm-error / dm-flakey / dm-dust](https://github.com/ligurio/sqa-wiki/wiki/fault-injection) | 块层注入：永久 IOERR / 按比例失败 / 坏扇区 | 故障分类（永久 / 瞬态 / 介质坏区） | Linux Device Mapper，板端不可用 |
| [mtd-utils `nandbiterrs`](https://git.infraroot.at/mtd-utils.git/plain/tests/mtd-tests/nandbiterrs.c?h=v2.2.0) | 逐位翻转 NAND 页，测 ECC 纠正上限 | 「位翻转 / 逐 CW 注入」的介质故障模型 | Linux MTD 用户态工具 |
| [FAIL*](https://github.com/danceos/fail) | GPLv3，C++ 实验描述；含 OpenOCD（真实 ARM）后端 | fault space / campaign / 结果库的概念 | 体量与依赖远超本仓需要 |
| [FFF](https://github.com/meekrosoft/fff) | 单头文件 C fake 框架：`FAKE_VALUE_FUNC` + 调用历史 | 需要函数替身时的轻量工具 | 本仓用 munit + 端口夹具，换框架无收益 |

**结论**：API 形状照 libfiu；key 维度照 blkdebug；故障分类照 dm-flakey/nandbiterrs；
实现保持本仓自拟、零外部依赖。

### 1.1 术语

| 中文 | 主流英文 | 说明 |
|---|---|---|
| 正例 / 负例 | positive / negative test case | 正常路径 / 故障或非法输入路径 |
| 故障注入 | fault injection | 主动制造故障 |
| 注入点 / 缝 | fault injection point / seam | 代码中允许注入的边界 |
| 链接期包装 | link-time wrapping（`-Wl,--wrap`） | 测试构建替换符号、生产源码不动（**本仓自拟的落地方式**） |
| 宏隔离 | compile-time isolation | `#if defined(FC_NAND_TEST_ENABLE)` 整体裁掉（**本仓自拟**） |
| 故障目录 | fault catalog | 本文 §3 的分类 + §5 的 seam 表 |

> 「W / H 机制」是本文的简写（W = wrap，H = 静态 hook），**本仓自拟**，不冒充行业术语。
> NAND 语义（FAIL_CACHE 指提交序前一页、`7Ah` 状态字节）以
> `docs/reference/datasheet_mt29f_cache_ops.md` 与 MT29F4G08 datasheet 为准；
> 找不到原文的说法标「本仓自拟」（`CLAUDE.md R12`）。

---

## 2. 分层模型

分层沿用 `docs/design/fc_nand_design.md §26` 的 HIL / FTL / FCC（本仓 FC-NAND）划分；
**诚实边界**：专有主控固件不可见，验证方法只能来自公开工具、公开规范与本文列出的出处，
不把「本仓自拟」写成「行业标准」（`CLAUDE.md §〇`）。

原则：**在最底层注入，让上层走真实路径**。不要在 FTL UT 里伪造「NAND 写失败」——
那验证的是 FTL 策略，不是 FC 的状态映射；也不要让 HIL 重复 FTL 的注入。

| 层 | 注入什么 | 注入点（缝） | 不注入什么 |
|---|---|---|---|
| HIL / controller | 非法入参、队列满、SCSI 错误传播 | `ssd_controller_submit` 入口 / host queue / SCSI 回调 | 介质故障（交给 FTL/FC） |
| FTL | 策略与状态机故障：L2P 加载失败、分配耗尽、Journal/CP/BBT 失败、mig/RR 失败 | FTL 模块函数入口 / 完成回调 | 器件状态位（交给 FC 缝） |
| FC-NAND | 器件状态：FAIL_CACHE / NAND_FAIL / TIMEOUT / DATA_ERROR / META / 7Ah | CQ 出口（`fc_poll_completion`）或 NAND 引擎 | 上层策略（交给 FTL） |

每层的负例都必须回答：

1. 故障从哪来（注入点 `文件:符号`）；
2. 期望系统怎么反应（状态码 + 不变量 + 可观测后果）；
3. 故障消失后能否恢复（下一条操作成功、无泄漏）。

### 2.1 耦合评估与选型（关键设计决定）

初稿曾计划在生产模块里直接埋 `fi_take*`（8 个文件、11 个点）。评估后**放弃该形态**：
它让每个业务模块都 include 测试头、知道测试语义，且每个埋点都要讨论「放在状态修改前还是后」，
正是「修补式修改」的来源。

改为两种机制，**以链接期 wrap 为主**：

| 维度 | A. 生产源码埋点（初稿） | B. 链接期 wrap（选定） |
|---|---|---|
| 生产源码改动 | 8 个文件各加 include + 调用 | **0**（只保留已有 2 个静态缝 hook） |
| include 扩散 | `fault_inject.h` 扩散到 FC/FTL 各模块 | 只在测试侧 |
| 语义耦合 | 埋点必须理解模块内部状态（plane/region/phase 放在哪一步） | 只依赖**公开 API 参数**（pba / op / cfg / phase） |
| 编译开关 | 每个模块都要 `FC_NAND_TEST_ENABLE` 分支 | 只在 UT 链接参数 + 测试侧 `.c` |
| 覆盖能力 | 可精确到内部静态函数 | 覆盖所有跨 TU 的公开边界；静态函数需少量 hook |
| 主要风险 | 埋点位置错误 → 语义被扰动（泄漏/跳过清理） | makefile 漏加 wrap、wrapper 未调用 `__real_*` |

**为什么 wrap 在本仓成立**（已核对调用关系）：

- `fc_poll_completion` 只被 `ftl_op.c` 调用（跨 TU）；
- `ftl_op_submit` 被 `checkpoint/journal/l2p_cache/bbt/migrate` 等调用（跨 TU）；
- `ftl_async_read_page / write_page / write_page_cw / erase_block` 被
  `migrate/read_recovery/journal/l2p_cache/metadata_job` 调用（跨 TU）；
- `alc_super_open_alloc_pair` 被 `cache_batch/ftl/wbuf/migrate` 调用（跨 TU）。
  ⇒ 这些符号在**测试构建**里加 `-Wl,--wrap=<symbol>`，测试侧提供 `__wrap_<symbol>`
  即可注入；正常路径必须调用 `__real_<symbol>`。

**wrap 的边界（必须在文档/用例里写清）**：

- 同一 TU 内的调用**不会**被 wrap（例如 `ftl_fc_interface.c` 内部的 mirror 变体）；
- 通过函数指针 / 回调表调用的不 wrap；
- 静态函数（`l2p_load_begin` / `cp_job_step` / `jrn_write_complete` 等）不 wrap
  ⇒ 这些缝要么保留**最小 hook**，要么改用其公开入口的 wrap（如 `ftl_op_submit` 带 ctx/phase）。
- wrapper 只允许做「注入或调用 real」两件事；不得修改参数/状态。

**结论**：

- 生产源码只保留**已有的 2 个静态缝 hook**（cache program FAIL_CACHE、Journal 页写）；
- FC/FTL 的其余故障用 **UT 构建的 wrap 库**（`tests/unit/fi_wrap.c` + `-Wl,--wrap`）；
- 若某条负例只能靠静态缝覆盖，先证明「公开边界无法表达」，再加**一个**hook 并在本文登记；
- 测试侧的注入状态仍用 `fi_arm*` / `fi_reset`，wrapper 与 hook 共用同一张表。

**宏隔离硬约束（解耦不了的静态缝）**：

- 静态缝的 `#include "fault_inject.h"` 与调用点都必须包在 `#if defined(FC_NAND_TEST_ENABLE)` 内，
  **不允许**「无条件调用空实现」——产品构建里该模块不应出现任何测试符号引用；
- 形态：

  ```c
  #if defined(FC_NAND_TEST_ENABLE)
  #  include "fault_inject.h"
  #endif

  /* ... */
  #if defined(FC_NAND_TEST_ENABLE)
      if (fi_take_at(FI_XXX, key) == true)
      {
          /* 注入结果 */
      }
  #endif
  ```

- 既有 2 个 hook 在本框架落地时按同一形态改造（行为不变，只是把生产构建的调用点裁掉）；
- 宏名只用 `FC_NAND_TEST_ENABLE`，不新增第二套开关（C9）。

---

## 3. 故障分类

| 类别 | 例子 | 主要层 | 注入方式 |
|---|---|---|---|
| 输入 / 契约 | 非法 opcode、LBA 越界、`NULL`、`count=0`、非法状态转移 | HIL / FTL 入口 | 直接调用（无需钩子） |
| 资源 / 背压 | 命令池空、SQ/CQ 满、pending 满、free/OP/spare 耗尽 | FTL / FC | `FI_ALLOC_FAIL`、池填充、入口拒绝 |
| 介质 / 坏块 | program fail、erase fail、UNC、META/CRC、bad block、位翻转 | FC-NAND | `FI_FC_STATUS`、`bbt fault`、引擎级注入 |
| 持久化 | Journal 页写失败、CP 各 phase 中止、Directory/Anchor 损坏、BBT 写失败 | FTL | `FI_JOURNAL_WRITE_FAIL` / `FI_CP_ABORT_PHASE` / `FI_BBT_WRITE_FAIL` |
| 并发 / 时序 | 迟到完成、重复 release、混优先级 chain、wild-chain、命令超时、门控饥饿 | FC / FTL | 序列/队列构造、`FI_SEQ_GATE_STARVE` |
| 掉电 / 恢复 | 写中断电、CP 各 phase 断电、format 中断电、快照撕裂 | 系统 | 手动断电（当前无程控电源）；UT 用 reboot-from-media |
| 迁移 / 回收 | GC/WL victim 读失败、目的页写失败、RR 恢复读失败 | FTL | `FI_MIG_READ_FAIL` / `FI_MIG_WRITE_FAIL` / `FI_RR_READ_FAIL` |

---

## 4. API 契约

### 4.1 故障类型

```c
#define FI_KEY_ANY (UINT32_MAX)

typedef enum FI_FAULT_E
{
    FI_CACHE_FAIL = 0,      /* cache program 完成：伪造 FAIL_CACHE（key = 页基址 LBA） */
    FI_JOURNAL_WRITE_FAIL,  /* Journal 页写失败（key 未用） */
    FI_FC_STATUS,           /* FC 完成状态位注入（key = NAND opcode，arg = status 位） */
    FI_ALLOC_FAIL,          /* 分配拿不到块（粗粒度：按次数，key 未用） */
    FI_L2P_LOAD_FAIL,       /* 元数据读启动失败（key = PBA；FI_KEY_ANY = 全部元数据读） */
    FI_CP_ABORT_PHASE,      /* 指定 CP phase 中止（key = cp_phase_e） */
    FI_BBT_WRITE_FAIL,      /* 元数据（镜像）写失败（key = PBA；FI_KEY_ANY = 按次数） */
    FI_MIG_READ_FAIL,       /* GC/WL victim 读失败（key = plane） */
    FI_MIG_WRITE_FAIL,      /* GC/WL 目的页写失败（key = plane） */
    FI_RR_READ_FAIL,        /* RR 恢复读硬失败（key = plane） */
    FI_SEQ_GATE_STARVE,     /* 序列门控无条件拒绝，模拟后台被饿死 */
    FI_FAULT_NUM
} fi_fault_e;
```

### 4.2 控制 API

```c
void  fi_reset(void);                                   // setup/teardown 必调
void  fi_arm(fi_fault_e f, DWORD count);                // 任意 key
void  fi_arm_at(fi_fault_e f, DWORD key, DWORD count);  // 指定 key
void  fi_arm_ex(fi_fault_e f, DWORD key, DWORD count, DWORD arg); // 带故障参数
BOOL  fi_take(fi_fault_e f);                            // 只匹配无 key 的武装
BOOL  fi_take_at(fi_fault_e f, DWORD key);              // 匹配「无 key 或同 key」
BOOL  fi_take_ex(fi_fault_e f, DWORD key, DWORD *out_arg);
```

### 4.3 语义与约束

- **按次数注入**，不是布尔开关；命中即自减，`0 = 不注入`。
- `fi_take_at`：槽的 key 为 `FI_KEY_ANY` 或与现场 key 相同才命中。
- `fi_take`：只匹配 `FI_KEY_ANY` 的武装（避免误消费定点注入）。
- 状态是文件级静态量；**每个用例 setup 与 teardown 必须 `fi_reset()`**（否则跨用例静默污染）。
- 只在 `FC_NAND_TEST_ENABLE` 下存在；产品构建为空实现（H14）。
- 旧 API（`fi_cache_fail_*` / `fi_journal_fail_*`）保留为薄封装，已有用例不改。
- **调用方只有两类**：测试侧 wrap 库（`tests/unit/fi_wrap.c`）与既有的 2 个静态缝 hook；
  生产模块**不新增** `#include "fault_inject.h"`（§2.1）。
- **H 类 hook 的 include 与调用点必须 `#if defined(FC_NAND_TEST_ENABLE)` 整体包住**；
  不允许「无条件调用空实现」（见 §2.1 宏隔离硬约束）。

### 4.4 表示链契约（P10）

故障注入引入的是一个**新的表示**（故障种类 + key + 次数 + 参数），按 P10 先写清六行：

```text
1. 坐标：key 的坐标域由故障类型唯一决定 ——
   plane（FI_MIG_* / FI_RR_*）、
   PBA（FI_L2P_LOAD_FAIL / FI_BBT_WRITE_FAIL）、
   page LBA（FI_CACHE_FAIL）、
   NAND opcode（FI_FC_STATUS）、
   cp_phase_e（FI_CP_ABORT_PHASE）；
   FI_ALLOC_FAIL 未用 key（粗粒度，按次数）；无 key 的故障用 FI_KEY_ANY。
2. 表：fi_slot_s s_fi[FI_FAULT_NUM]；元素 {left, key, arg}；
   left==0 = 未武装；key==FI_KEY_ANY = 任意；arg 的类型由故障类型定义。
3. 计数：left 是「还能命中几次」，命中即减；上界由调用者给的 count 决定，
   框架不设隐式上限；越界/非法枚举属调用者错误（C5/H34：ASSERT）。
4. 归属：状态在 `fault_inject.c`（测试镜像）；控制 API 在 `fault_inject.h`；
   生产构建整段裁剪（宏隔离）；测试侧 wrap 库与 H 类 hook 共用同一张表。
5. 表示链：fi_fault_e → fi_slot_s.key/arg → wrap/hook 的 fi_take* →
   被注入模块的返回值/状态位 → 用例断言；测试代码不另造第二套故障编号（C9）。
6. 判据：§1 的公开工具形状 + 本仓分层（§26）；不是「本仓现状」也不是「教科书」。
```

**坐标域不得混用（C11）**：plane / PBA / phase / opcode 都是 `DWORD`，但语义不同；
API 用命名构造函数表达（`FI_KEY_PLANE` / `FI_KEY_REGION` / `FI_KEY_LBA` / `FI_KEY_OP` /
`FI_KEY_PHASE`，只做语义标注，不改变值域）。测试代码不得把 region 当 plane 传。

**入口守卫（C5 / H34）**：`fi_arm_ex` 的 `f == FI_FAULT_NUM` 属调用者自己持有的枚举
常量 ⇒ `ASSERT((DWORD)f < (DWORD)FI_FAULT_NUM)`；release UT 下越界直接 return（测试侧
内部 API，不是 host 输入）。

**类型形态（H50）**：

```c
typedef struct FI_SLOT_S
{
    DWORD left;
    DWORD key;
    DWORD arg;
} fi_slot_s;
```

---

## 5. Seam 表（注入点）

机制分两种：**W** = 测试构建 `-Wl,--wrap`（生产源码零改动）；**H** = 生产源码静态缝 hook
（只在无法 wrap 时使用，逐条登记）。

| 故障 | 机制 | 缝 / wrap 符号 | key | 期望可观测 |
|---|---|---|---|---|
| `FI_FC_STATUS` | **W** | `fc_poll_completion` / `fc_poll_completions` | NAND opcode | 完成状态带注入位；FTL 不进成功路径 |
| `FI_CACHE_FAIL` | **H**（已有） | `src/ftl/ftl.c:ftl_cache_wr_member_done` | 页基址 LBA | 页不确认、映射撤销、无 `stray page` |
| `FI_JOURNAL_WRITE_FAIL` | **H**（已有） | `src/ftl/journal.c:jrn_write_complete` | — | WAL 水位不动、`write_fail_pending` |
| `FI_ALLOC_FAIL` | **W** | `alc_super_open_alloc_pair`（命中即返回 `FTL_AGAIN`，粗粒度） | 未用（按次数） | 分配失败走 `FTL_AGAIN`/有界失败 |
| `FI_L2P_LOAD_FAIL` | **W** | `ftl_async_read_page` / `ftl_sync_read_page` + `cfg` 为 META | PBA（`FI_KEY_ANY` = 全部元数据读） | 行保持 INVALID，读/确认走有界失败 |
| `FI_CP_ABORT_PHASE` | **W** | `ftl_op_submit` + `op==FTL_OP_CHECKPOINT`，从 `ctx` 读 `cp_handle_s->phase`（**持续**注入） | `cp_phase_e` | `cp_write_submit` 把提交失败归为瞬态（单次只重试）⇒ 针对某 phase 的持续拒绝由 `FTL_SUBMIT_TIMEOUT_MS`（10 ms）的 submit deadline 中止；判据用 `fail_phase` / `timeout_phase`（`failed` 会被 CP_DONE 清） |
| `FI_BBT_WRITE_FAIL` | **W** | `ftl_async_write_page_mirror` / `ftl_sync_write_page_mirror`（以及 META 普通写） | PBA（`FI_KEY_ANY` = 按次数） | dirty 保留、重试；不误清 |
| `FI_MIG_READ_FAIL` | **W** | `ftl_async_read_page`（DATA 类）+ `ftl_op_submit`（`op==HOST_IO` 且读 opcode） | 次数 / plane（host 读入口取不到 plane ⇒ 只能用无 key 武装） | victim 不擦、不标坏好块、job abort；host 读有界失败 |
| `FI_MIG_WRITE_FAIL` | **W** | `ftl_async_write_page_cw` + `ftl_async_write_pair_cmd`（DATA 类；后者是 host 写 / cache program 批次） | 次数 / plane | 目的 SB 关闭/回滚、无泄漏 |
| `FI_RR_READ_FAIL` | **W** | `ftl_async_read_page`（RR 调用点） | 次数 / plane | RR 有界失败、组按 IO_ERROR 结算 |
| `FI_SEQ_GATE_STARVE` | **W** | `ftl_op_submit` + `ftl_op_submit_batch` 强制返回 `FTL_AGAIN` | — | 组被持续拒绝时不完成；放开后同一组完成 |
| `FI_FC_ENGINE_FAIL` | **H**（D2） | `src/fc/fc_nand_dispatch.c:fc_nand_dispatch_task_complete`，在策略表前把 result 改为 `NAND_RESULT_OPERATION_FAILED` | NAND opcode | completion 带错误位；completion 策略把选中 CW 全部标 FAIL |
| `FI_COMPLETION_DROP` | **W**（D2） | `fc_poll_completion`，消费 real 后不投递 | NAND opcode | 调用者只能走超时/丢完成路径；`fi_hits>0` |
| `FI_COMPLETION_LATE` | **W**（D2） | `fc_poll_completion`，把完成暂存到下一轮 | NAND opcode | 本轮 `QUEUE_EMPTY`，下一轮返回同一 cmd_id/status |
| `FI_READ_COLUMN_FAIL` | **W**（D2） | `ftl_sync_read_column` | PBA | BBT 工厂扫描/列读走失败分支 |
| `FI_PLANE_STATUS` | **H**（D2） | `fc_nand_dispatch_task_complete`，按 plane 覆盖 `cw_status` | plane（arg = status 位） | 单 plane 写/完成失败只影响该 plane 的 CW |
| `FI_DATA_PATH_ENCODE_FAIL` | **W**（D2） | `fc_data_path_encode` | 次数 | 写命令在提交 NAND 前失败 |
| `FI_DATA_PATH_READ_FAIL` | **W**（D2） | `fc_data_path_process_read` | 次数 | 读命令按错误完成，不交付未校验数据 |
| `FI_CLOCK_NOW_FAIL` | **W**（D2） | `delay_time_now_us` | 次数 | 时基失败路径（IST 让出/超时） |

> ⚠ **host 数据读不走 `ftl_async_read_page`**：`ftl_issue_subop` 内联建链后直接
> `ftl_op_submit(op==FTL_OP_HOST_IO)`。读失败注入因此挂两处（async + submit）；
> 且 8 LBA = 两 plane ⇒ host 读的 opcode 是 **multi-plane** 变体，FC 状态注入的
> key 必须用它（`OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT`）。
>
> 说明：W 类注入都发生在 **FTL↔FC 公开边界**或 **FC CQ 出口**，wrapper 只看公开参数；
> 需要「哪个调用者」的用例（如区分 L2P 加载 / RR 恢复读）通过 `cfg`（META/DATA）+
> 次数/PBA 表达；若表达不了，再加一个静态缝 hook，并在本表登记原因。
> **H 类的 include 与调用点必须 `#if defined(FC_NAND_TEST_ENABLE)` 整体隔离**，
> 产品构建不出现测试符号（§2.1、§4.3）。

**wrapper 契约（H30 / C5）**：

- `__wrap_<symbol>` 默认必须调用 `__real_<symbol>` 并把**返回值原样传出**；
  不得静默丢弃、不得改参数、不得改状态；
- 注入时只允许两种形态：
  a. 在 real 返回后修改输出（如 `fc_poll_completion` 的 `status`）；
  b. 不调用 real 直接返回故障码（如 `ftl_async_*` 提前返回 `FTL_ERROR`）——
     此时必须保证没有分配资源（否则泄漏）；
- wrapper 的状态来自 `fi_arm*`，用例结束 `fi_reset()`；
- `FI_COMPLETION_DROP` / `FI_COMPLETION_LATE` 是**刻意的完成投递抑制**，不属于
  「静默丢弃返回值」：命中由 `fi_hits` 证明，且只影响测试镜像；normal 路径仍原样投递。
- wrapper 只在 UT 构建链接（`tests/unit/fi_wrap.c` + `-Wl,--wrap`），产品构建不存在。

### 5.1 HIL 负例（无 hook）

HIL 是入口契约层，负例**不需要任何注入**：测试直接以非法/边界参数调用公开 API。

| 用例方向 | 调用 | 期望 |
|---|---|---|
| NULL controller / request / buffer | `ssd_controller_submit` | `INVALID_ARGUMENT` + complete，不解引用 |
| LBA 越界 / count=0 | `ssd_controller_submit` | 入口拒绝，不下传 FTL |
| 队列 / 组槽耗尽 | 连续 submit | 返回 BUSY/AGAIN；释放后恢复 |
| TRIM 越界 / BUSY | submit TRIM | 契约状态码 |
| SCSI 未知命令 / MODE SENSE / SELECT | SCSI 回调 | ILLEGAL REQUEST / 合法页响应 |
| deferred error 传播 | 用 FTL 注入制造失败 | 下一条命令如实报错 |

### 5.2 FC 引擎级注入（待定，先评估复用）

FC 自身的状态解码（desc → FAIL_CACHE 归属、`7Ah` 状态、R/B 超时）**不在** wrap 覆盖内：
它需要「伪造 NAND 完成」而不是「修改 CQ 里的状态」。

- 优先评估现有 `tests/unit/fc_test_common.c` 夹具是否已能直接构造
  `fc_nand_dispatch_comp_s` / 完成项（若可以，不新增生产代码）；
- **评估结论（已做）**：该夹具只提供 `test_submit_wait` / `test_prog_page` 等
  「真 NAND 驱动」入口，没有可注入的完成/引擎 seam ⇒ 零生产改动做不了；
- **D2 已补**：`FI_FC_ENGINE_FAIL`（H 类，dispatch 完成策略前）、
  `FI_COMPLETION_DROP` / `FI_COMPLETION_LATE`（W 类，CQ 出口）；另有
  `fc_nand_dispatch_test_completion()` 测试 API 只驱动 completion 策略表，
  不提交 NAND。后续 FC 解码负例必须复用这些 seam，不得用 FTL 侧状态注入冒充。
>
> 引擎级注入（真正伪造 NAND 状态字节、7Ah、R/B 超时）不在此表：它们属于
> FC-NAND UT 的 case 构造（测试侧 fake engine），不改生产源码。

---

## 6. 用例模板

```text
1. 前置：干净状态（pure RAM / format+POR），确认基线判据
2. 对照：不注入时同一条路径成功（正例对照）
3. 注入：fi_arm_at(f, key, n)
4. 触发：调用目标 API（写/读/流控/完成）
5. 断言：
   a. 返回状态 == 预期错误码（FTL_AGAIN / FTL_ERROR / SSD_STATUS_*）
   b. 不变量成立（pending=0、L2P 未被错误撤销、valid_count 一致、队列无泄漏）
   c. 无静默破坏（读回数据 / 介质内容 / map 结果）
   d. 注入被消费：fi_take 计数 == n（C7）
   e. 恢复：清除注入后下一条操作成功
6. 收尾：fi_reset() + teardown assert_clean
7. 证据：`ftl_ut_trace_begin` / `ftl_ut_trace_detail` / `ftl_ut_trace_summary`
   （FC/HIL 用各自夹具的等价证据行）；内容含 fault / point / expected / observed
```

### 6.1 用例命名与放置

- 用例**主键 = munit 用例名**（稳定 ID；测试函数名以 `test_` 开头，二者不要求逐字相同；与 `ftl_ut_design.md` §2 模块表 / `fc_nand_ut.md` §2 模块表 /
  `hil_ut_design.md` 的设计点一致），不引入 `NEG-*` 独立编号；
- 放置：FTL pure 放 `/ftl/pure/` 对应模块套件；media/e2e 放 `/ftl/media/e2e/`；
  **注入负例放 `/ftl/negative/`（`tests/unit/test_negative_pure.c` + `test_negative_media.c`）**；
  FC 放 `pure` / `lifecycle` 套件；HIL 放 `/hil/` 套件；
- **测试代码注释不得写 RISK 编号**（用户规则）：风险映射只维护在本文 §11 与
  `docs/design/known_risks.md`，测试代码只写「故障 / 预期 / 判据」。

**当前落地用例**（`/ftl/negative/media/{gc,l2p,meta,boot,if}/` 真 NAND 49 条；`/ftl/negative/pure/` RAM/纯函数 42 条）：`alloc_fail_bounded` /
`l2p_load_fail_bounded` / `data_read_fail_bounded` / `data_write_fail_bounded` /
`rr_read_fail_bounded` / `fc_status_nand_fail` / `seq_gate_starve` /
`cp_abort_phase` / `bbt_write_fail` / `gc_read_fail_bounded` / `gc_write_fail_bounded` /
`gc_erase_fail_bounded` / `l2p_read_error_degrades` / `cp_dir_ckpt_abort`；HIL 侧新增 `trim_edges`
（`tests/unit/test_hil.c`）。已有用例继续覆盖 `cache_failc_abort` 与
`journal_write_fail_wal_stall`（H 类 hook）。

**内存约束**：combined UT（FC+FTL）的 AXI_RAM 已接近上限，负例缓冲放在
`SRAM4_DATA`（`.sram4_data` 是 NOLOAD，用例内先填后读）；新增负例避免再往 `.bss`
放大数组，否则 `make tests` 会在链接期溢出。

### 6.2 复用优先（R9 / C2）

落地每个 hook 或 wrapper 之前先查已有 seam，避免新造等价物：

| 需要的能力 | 已有 seam / helper |
|---|---|
| FTL 读/写/擦失败 | `ftl_async_read_page` / `ftl_async_write_page` / `ftl_async_write_page_cw` / `ftl_async_erase_block` |
| 分配耗尽 | `alc_super_open_alloc_pair`（`alc_free_*` / `alc_op_*` 计数访问器已存在） |
| CP 阶段 | `ftl_op_submit` 的 `FTL_OP_CHECKPOINT` + `cp_handle_s->phase` |
| 完成状态 | `fc_poll_completion` / `fc_poll_completions` |
| Journal 写失败 | 既有 `fi_journal_fail_take` |
| cache FAIL_CACHE | 既有 `fi_cache_fail_take` |
| 坏块注入 | 既有 CLI `bbt fault` |

机械自查：新增 hook 前 `grep -rn "fi_take\|ftl_async_\|alc_super_open_alloc_pair"
src include` 确认没有现成入口。

---

## 7. 风险追溯矩阵

### 7.1 汇总（当前 302 条未闭合风险的启发式映射）

| 建议故障类别 | 条数 |
|---|---|
| 待归类（需按 detail 再定） | 103 |
| FC 状态 / 完成 | 41 |
| L2P / 元数据 | 45 |
| 介质 / 坏块 | 23 |
| 资源 / 背压 | 23 |
| 并发 / 时序 | 21 |
| 掉电 / 恢复 | 17 |
| GC / WL / 迁移 | 16 |
| 输入 / 契约 | 13 |

> 映射按标题关键词自动生成，**只作分流**；具体注入点以每条风险的「下一步」与证据为准。
> 全量清单见 §11 附录。

### 7.2 覆盖口径

- 一条风险算「框架可覆盖」= 存在至少一个故障类型 + 注入点，可构造出它的失败场景；
- 算「已覆盖」= 该风险至少有一条能红的用例（C7）并上板通过（R1）；
- 本文只做「可覆盖」映射；「已覆盖」计数在实现后按 P13 报告。

---

## 8. 执行与证据

| 项 | 约定 |
|---|---|
| 镜像 | `make tests UT=FC/FTL/HIL`（只有 `FC_NAND_TEST_ENABLE` 才有注入） |
| 运行前 | `format` + POR；**拔掉 USB**（`test` 入口会拒绝 host 在飞的请求） |
| 复位 | 失败后按**复位**而不是断电，保留 `.noinit` 证据 |
| 失败定位 | `crash` / `pc`（同一镜像符号表）；`ftl_ctx` / `q` / `jrn` / `cp` |
| 证据 | 串口原始日志 + `ftl_ut_trace_*` 证据行 + UT 报告；未上板前不勾选 |

### 8.1 构建门禁（R2）

- 实现每个阶段后必须跑：`make all` / `make release` / `make tests UT=FTL`（涉及 FTL）
  以及 `UT=FC` / `UT=HIL`（涉及对应层）；**零新告警**。
- 宏隔离的机械证据：产品 `debug.elf` / `release.elf` 中 `nm | grep ' fi_'` 为空；
  与框架落地前的 `.text` 对比应**逐字节不变**（纯测试侧改动 + 宏裁掉）。
- `make tests` 只证链接不证正确（R2）⇒ 负例必须上板跑。
- P11：批量收尾跑一次，不逐文件构建；纯文档改动不编译。

### 8.2 文档同步（P12）

框架落地定稿时，一次性回扫并更新：

- `docs/design/ftl_ut_design.md` §2（模块设计）与 §4.8 / §4.9（seam 与负例框架）；
- `docs/design/fc_nand_ut.md` §2（模块设计）与 §4（夹具 / seam）；
- `docs/design/hil_ut_design.md` §3 / §6；
- `docs/design/testing_guide.md §11`（负例执行清单）；
- `docs/design/known_risks.md`（覆盖状态与残余）。

### 8.3 偏离留痕（P5）

- 新增静态缝 hook = 偏离「生产零耦合」原则，必须在代码注释与 §5 写明
  「偏离原因 + 为何公开边界表达不了」；
- wrap 无法覆盖的用例（同 TU / 函数指针 / 静态函数）在用例处注明，避免下次误以为已覆盖。

---

## 9. 覆盖统计与验收（P13）

实现后必须给出：

1. 故障类型总数 / 已实现数 / 例外；
2. 每个故障类型至少 1 条负例，且有**正例对照**；
3. 每条负例的「不修复即红」证据（C7）；
4. **生产源码 hook 数**：必须等于 2（既有）+ 本文登记的新增静态缝；新增一个就要写理由；
5. **宏隔离机械检查**：所有 `fault_inject.h` / `fi_*` 引用都在 `#if defined(FC_NAND_TEST_ENABLE)`
   内（`git grep` 人工核对 + 产品 `nm` 无 `fi_` 符号）；不允许无条件调用空实现；
6. **产品二进制不变**：框架落地前后 `debug.elf` / `release.elf` 的 `.text` 逐字节对比
   （P13 / H50 口径）；UT 侧 `.text` 变化需给出原因；
7. **检查器要有区分力（C7/P13）**：覆盖率脚本先用一个必然缺项的输入验证会报错，
   再宣称通过；
8. 风险映射：可覆盖 N / 已覆盖 M / 明确不覆盖 K（硬件/环境），三者相加 = 总条目；
9. 不得用「旧名 grep=0 / 编译通过」替代。

### 9.1 规则对照审计（本设计自查）

| 规则 | 本设计的落点 | 结论 |
|---|---|---|
| §〇 / P0 | §1 出处、§2 §26 分层、诚实边界 | 已补出处；不把自拟写成标准 |
| R1 | 文档状态「设计稿，未实现」；§8 证据 | 已符合 |
| R2 | §8.1 四套构建 + 零告警 + 上板 | 已补 |
| R3 | 提交/推送由用户决定 | 本文未提交 |
| R4 | §0.2、§6.1；**用例编写归属待用户确认** | ⚠ 需拍板 |
| R9 / C2 | §6.2 复用清单 + grep 自查 | 已补 |
| R10 | §1.1 术语表；W/H 标「本仓自拟」 | 已补 |
| R11 | §7 / §10 与 known_risks 的映射；残余登记 | 已补映射；实现后逐条登记 |
| R12 | §1.1 引用 datasheet；NAND 语义出处 | 已补 |
| C1 | §1 + §2 §26 | 已符合 |
| C5 / H34 | §4.4 入口守卫（`ASSERT` + release return） | 已补 |
| C7 / C8 | §6 模板 d/e、§9.3 | 已符合 |
| C9 / C11 | §4.4 坐标域；禁止第二套故障编号 | 已补 |
| C10 | 旧 `fi_*` API 语义不变；落地时回扫 2 个既有调用点 | 已补（见下） |
| H30 | §5 wrapper 契约：返回值原样传出 | 已补 |
| H50 | §4.4 `fi_slot_s` 定义形态 | 已补 |
| P3 | 新增 `ASSERT` 前读 `docs/design/assertion_guidelines.md` | 已补 |
| P5 | §8.3 偏离留痕 | 已补 |
| P10 | §4.4 六行契约 | 已补 |
| P2 | 文档引用一律 `docs/...` 全路径 | 已修正 |
| P11 | §8.1 批量构建 | 已补 |
| P12 | §8.2 文档回扫清单 | 已补 |
| P13 | §9 覆盖率 + 检查器区分力 | 已补 |
| HIL | §5.1 直接调用公开 API，无 hook | 已补 |
| FC 引擎级 | §5.2 先评估 `fc_test_common` 夹具，必要时宏隔离 test-only API | ⚠ 待定（实现前登记） |

**C10 落地要求**：现有 `fi_cache_fail_arm/take`、`fi_journal_fail_arm/take` 改为新 API 的薄封装时，
语义必须保持不变；落地后回扫 `src/ftl/ftl.c` / `src/ftl/journal.c` 两个调用点，确认参数
（page LBA / 次数）与命中顺序不变。

---

### 9.2 落地覆盖账（2026-11-14）

**结论先说：框架 + 第一批 9 例已上板，不等于风险列表覆盖完成。** 302 条风险的逐条判定见
`docs/design/negative_coverage.md`（每条给 M/N/K/待判定 + seam / 触发 / 断言 / 用例名）。

**故障类型（§4.1，共 12）**：**12/12 均有用例**（第二批补上 `FI_MIG_ERASE_FAIL` →
`gc_erase_fail_bounded`）。其中 2 个 H 类由既有用例覆盖：`FI_CACHE_FAIL` →
`cache_failc_abort`，`FI_JOURNAL_WRITE_FAIL` → `journal_write_fail_wal_stall`。

**用例**：`/ftl/negative/` **91** 条（pure 42 + media 49） + `/fc-nand/negative/` **24** 条 + HIL
`trim_edges`（HIL 共 17 条）。**2026-11-14 板端**：debug UT=FTL `36/36 + 3 SKIP`、
release UT=FTL `54/54 + 0 SKIP`（219.32 s）；`/fc-nand/negative/` 的 release 逐条结果未单列，
已在 2026-11-15 release 全量 `test --seed 0x5ee5cfbc` 的 **306/306、3 SKIP** 中执行
（原始日志见 `build/test-results/`，结案记录见 `docs/design/known_risks.md` 的 `RISK-UT-40` / `RISK-UT-42`）。

**风险（302 条）**：

| 处置 | 数量 | 说明 |
|---|---|---|
| 已覆盖 M | **67** | 58 条由 2026-11-14 两套 UT + 新增负例转 M；其余为既有板端证据 |
| 可覆盖 N · 已写用例（未上板） | **4** | `GC-02`（读回 0xFF 待定位）、`IF-18`（2/4）、`BBT-10`（kick 已验，factory-scan/format-erase 未覆盖）、`UT-34`（用户裁决） |
| 可覆盖 N · 无用例（用户裁决） | **1** | `RISK-UT-34`：buffer NULL 按裁决留以后；其余契约已由既有 HIL 覆盖 |
| 明确不覆盖 K | **216** | 主机侧矩阵 / 人工掉电窗口 / 长压测 / 白盒与夹具不可达 / 触发不可确定性构造 / 用户裁决无失败模式 |
| 待判定 | **0** | 110 条已于 2026-11-14 全部落定 |
| 待决策 / 待定位 | **5 / 8** | 剩余契约裁决 / 风险描述需重审 |
| 待定位 / 待决策 / 待测试源 / 待后续 | **7 / 5 / 1 / 1** | 非负例可解项 |

合计 302。**禁止**把「未设计」写成 N：N 必须三样齐（注入点、触发步骤、判据）。

**用例结构（2026-11-14 收敛）**：media 负例统一为
`neg_prepare → arrange → arm/act/assert → teardown` 四段；清理与跨用例隔离只在
`neg_teardown` 的 `neg_drain_to_clean`（fi_reset → 恢复旋钮 → flush → 脏 CMT 用 CP
落盘 → quiesce → assert_clean）。GC 类用例通过 `neg_gc_pick_victim` 建立确定性的
victim 形态；LBA 统一走 `neg_lba(offset)`（窗口内断言）。判据：用例不得依赖 victim
重选、缓存驻留或 flush 顺带 CP 这类动态状态；每条注入必须断言 `fi_hits > 0`。

**批次进度**：

| 批次 | 状态 | 用例 |
|---|---|---|
| B1 GC/WL 迁移 | ✅ 已写/未上板 | `gc_read_fail_bounded` / `gc_write_fail_bounded` / `gc_erase_fail_bounded` / `gc_read_unc_marks_bad` / `gc_batch_program_fail` / `gc_l2p_load_wait_timeout` / `rr_mixed_plane_fail` |
| B2 L2P/元数据完成态 | ✅ 已写/未上板 | `l2p_read_error_degrades` / `l2p_cmt_read_priority` / `l2p_idle_drain` / `l2p_flush_backpressure` / `cp_dir_ckpt_abort` |
| B3 启动/Journal | ✅ 已写/未上板 | `boot_corrupt_cp_restore` / `boot_no_rebuild_loop` / `boot_degraded_reload` / `journal_seq_serial_compare` / `flush_reports_journal_error` |
| B4 数据面/FC | ✅ 已写/未上板 | `iface_sparse_page` / `blank_ff_trim_rewrite` / `iface_pair_cmd_guards` / `iface_release_guards` / `cw_write_plane1_only` / `stats_consistency` / `batch_arg_guards` / `in_flight_release_guard` / `data_ptr_single_plane_only` / `op_req_table_guard` / `pend_table_guard` |
| B5 其余模块 | ✅ 已写/未上板 | `host_read_multiplane_stats` / `md_alias_full_rebuild_repro` / `bbt_mark_bad_idempotent` / `alloc_rewind_underflow`（release-only） / `alloc_spare_skip_bad` / `wl_erase_resets_read_count` / `wl_skip_bad_plane` / `nand_timing_bounds`（release-only） |
| B6 收尾 | ✅ 全部 N 已写 | `cp_dir_ckpt_abort` 补 `ckpt.write_idx` 推进断言；其余 13 条降为待判定（见 coverage 表） |
| K 人工/硬件 | — | 逐 phase 掉电、多 LUN、空白片 |

## 10. 非覆盖（明确不做）

- **多 target / 多 die**：硬件不具备（`RISK-TOP-01`、`RISK-NANDP-01/04/05/12`）；
- **LDPC**：已明确延后（`RISK-DP-15/27`）；
- **自动掉电 / 程控电源**：当前无硬件，掉电仍为手动；
- **多主机 USB 矩阵**：只有一台 PC；
- **长时 soak / 高低温**：需要独立环境。

---

## 11. 附录：当前未闭合风险全量映射

> 共 302 条（`待上板` / `待定位` / `待决策` / `待后续` / `待议` / `待测试源`）。
> ⚠ 原表遗漏了 19 条 `RISK-L2P-*`（2026-11-14 补齐）。
> 「建议故障类别」是按标题关键词的启发式分流，**不是**注入点结论。

| # | 模块 | ID | 状态 | 一句话 | 建议故障类别 |
|---|---|---|---|---|---|
| 1 | 1 metadata 模块 | RISK-MD-01 | 待上板 | 日志块擦除持续失败无限重试，无坏块策略 | 介质/坏块 |
| 2 | 1 metadata 模块 | RISK-MD-02 | 待上板 | 压缩预算按启动时 l2p_live，在飞 region 写可能超预算 | 资源/背压 |
| 3 | 1 metadata 模块 | RISK-MD-06 | 待定位 | L2P 指向已归还/已擦块，恢复回退 full rebuild 丢映射 | L2P/元数据 |
| 4 | 1 metadata 模块 | RISK-MD-18 | 待上板 | 目录检查点未覆盖最新 Directory 时压缩擦 victim | L2P/元数据 |
| 5 | 1 metadata 模块 | RISK-MD-19 | 待上板 | victim 未排除在飞槽位，stale count 被清零 | GC/WL/迁移 |
| 6 | 1 metadata 模块 | RISK-MD-20 | 待上板 | 压缩 read→commit 无 CAS，陈旧映射持久化 | L2P/元数据 |
| 7 | 1 metadata 模块 | RISK-MD-21 | 待上板 | 目录检查点 abort 不消费槽位，二次编程 | L2P/元数据 |
| 8 | 1 metadata 模块 | RISK-MD-22 | 待上板 | 借来的日志块标 BAD 时漏减 borrow 配额 | 介质/坏块 |
| 9 | 1 metadata 模块 | RISK-MD-23 | 待上板 | ring frontier 落在块中时复用半编程槽 | 待归类 |
| 10 | 1 metadata 模块 | RISK-MD-24 | 待上板 | initial 目录检查点只写单 plane，回退到旧 record | L2P/元数据 |
| 11 | 2 GC / migrate / journal | RISK-GC-01 | 待上板 | 永久不可读/ META 损坏 victim 反复 abort，容量不增长 | FC 状态/完成 |
| 12 | 2 GC / migrate / journal | RISK-GC-02 | 待上板 | 目的页写失败不能整块标坏已有成功迁移页的 Cold SB | GC/WL/迁移 |
| 13 | 2 GC / migrate / journal | RISK-GC-04 | 待上板 | 部分 plane 擦除后中止，旧 Closed 条目误认已释放 plane | 介质/坏块 |
| 14 | 2 GC / migrate / journal | RISK-GC-05 | 待后续 | GC Bitmap 未实现，掉电后 GC 从块头重来 | GC/WL/迁移 |
| 15 | 2 GC / migrate / journal | RISK-GC-06 | 待上板 | WL 5s 节拍/CLOSED 门控/valid_count 上界/三态未板验 | GC/WL/迁移 |
| 16 | 2 GC / migrate / journal | RISK-GC-10 | 待上板 | 系统性 victim 读失败无熔断，好块成批标 BAD | 介质/坏块 |
| 17 | 2 GC / migrate / journal | RISK-GC-14 | 待上板 | no-gain 判据依赖 total_writes/free 快照 | 掉电/恢复 |
| 18 | 2 GC / migrate / journal | RISK-GC-15 | 待上板 | BUSY 档挡掉 urgent GC，free 掉到 1 才急救 | GC/WL/迁移 |
| 19 | 2 GC / migrate / journal | RISK-GC-17 | 待上板 | cache-program batch 页基址误判，batch 永久禁用 | 待归类 |
| 20 | 2 GC / migrate / journal | RISK-GC-18 | 待上板 | 空 victim 快路径绕过 Journal 持久性闸门 | L2P/元数据 |
| 21 | 2 GC / migrate / journal | RISK-GC-19 | 待上板 | 解码缺 meta 的 CW 被静默当未写，静默丢数据 | 待归类 |
| 22 | 2 GC / migrate / journal | RISK-MIG-01 | 待上板 | 等待命令丢失/卡住时永久 active | 待归类 |
| 23 | 2 GC / migrate / journal | RISK-MIG-02 | 待上板 | predictive 迁移活锁（同 GC-07 修法） | GC/WL/迁移 |
| 24 | 2 GC / migrate / journal | RISK-MIG-03 | 待上板 | 标 BAD 后 L2P 仍指向该块，valid_count 回绕 | L2P/元数据 |
| 25 | 2 GC / migrate / journal | RISK-MIG-05 | 待上板 | GC 读失败 abort 日志刷屏阻塞 UART | GC/WL/迁移 |
| 26 | 2 GC / migrate / journal | RISK-MIG-06 | 待上板 | mark BAD 日志单位/原因不区分 | 介质/坏块 |
| 27 | 2 GC / migrate / journal | RISK-MIG-07 | 待定位 | 大批块被 GC 读失败退役，L2P 悬空到 BAD 块 | L2P/元数据 |
| 28 | 2 GC / migrate / journal | RISK-MIG-09 | 待上板 | host 写失败只标 plane0 的块 | 待归类 |
| 29 | 2 GC / migrate / journal | RISK-MIG-10 | 待上板 | 迟到完成可能按新 job 的 phase 派发 | FC 状态/完成 |
| 30 | 2 GC / migrate / journal | RISK-JN-01 | 待上板 | Journal 环形块擦除失败无限重挂 need_erase | L2P/元数据 |
| 31 | 2 GC / migrate / journal | RISK-JN-02 | 待上板 | 同步刷页返回 void，调用者不知屏障未达成 | 待归类 |
| 32 | 2 GC / migrate / journal | RISK-JN-03 | 待上板 | write_fail_pending + CP 清账未做故障注入 | 待归类 |
| 33 | 2 GC / migrate / journal | RISK-JN-04 | 待上板 | drain 超时后仍同步刷页，同页二次编程 | 并发/时序 |
| 34 | 2 GC / migrate / journal | RISK-JN-05 | 待上板 | 回放失败/降级日志逐条打印，拖慢开机 | 掉电/恢复 |
| 35 | 2 GC / migrate / journal | RISK-JN-06 | 待上板 | 回放工作缓冲从栈改文件级 static | 掉电/恢复 |
| 36 | 2 GC / migrate / journal | RISK-JN-07 | 待上板 | jrn_block_next 把 from 自身算作下一块 | 待归类 |
| 37 | 2 GC / migrate / journal | RISK-JN-08 | 待上板 | 回放第二遍读失败不上报，静默跳过条目 | 掉电/恢复 |
| 38 | 2 GC / migrate / journal | RISK-JN-09 | 待上板 | 32-bit Journal 序号无回绕处理 | L2P/元数据 |
| 39 | 2 GC / migrate / journal | RISK-JN-10 | 待上板 | 撕裂 frontier 页重复编程 + 每次开机 full rebuild | 待归类 |
| 40 | 2 GC / migrate / journal | RISK-RR-02 | 待上板 | 恢复成功仍报 IO_ERROR / recovered 只看最后 plane | 介质/坏块 |
| 41 | 2 GC / migrate / journal | RISK-RR-03 | 待上板 | ECC 健康状态可降级（CRITICAL→WARNING） | FC 状态/完成 |
| 42 | 2 GC / migrate / journal | RISK-RR-04 | 待上板 | 预测巡检命中含 BAD plane 的 SB，migrate 卡死 | 介质/坏块 |
| 43 | 2 GC / migrate / journal | RISK-RR-05 | 待上板 | SLC 无 read-retry table 却做 8 次重试 + 邻居辅助 | 待归类 |
| 44 | 2 GC / migrate / journal | RISK-RR-06 | 待上板 | RR 重试读被 read sequence draining 挡住，host 读组永不完成 | FC 状态/完成 |
| 45 | 2 GC / migrate / journal | RISK-WL-10 | 待上板 | 静态 WL 快照恢复后从不触发（分母用整 SB） | GC/WL/迁移 |
| 46 | 2 GC / migrate / journal | RISK-WL-11 | 待上板 | WL 目的准备失败静默退化为普通分配 | 资源/背压 |
| 47 | 2 GC / migrate / journal | RISK-ALLOC-14 | 待上板 | 运行期标 BAD 让 Closed 条目永久卡死 | 介质/坏块 |
| 48 | 2 GC / migrate / journal | RISK-PERF-01 | 待后续 | GC/WL 选块重复全表扫描 | GC/WL/迁移 |
| 49 | 2 GC / migrate / journal | RISK-PERF-02 | 待后续 | GC/WL 目的页逐 plane 单条写，无 multi-plane 命令 | GC/WL/迁移 |
| 50 | 2A BBT（Bad Block Table） | RISK-BBT-01 | 待上板 | 运行时标坏的幂等 / 持久化 / Spare 记账未板验 | 待归类 |
| 51 | 2A BBT（Bad Block Table） | RISK-BBT-02 | 待上板 | 工厂扫描对列读失败页的判坏行为未实测 | 待归类 |
| 52 | 2A BBT（Bad Block Table） | RISK-BBT-03 | 待上板 | 格式化擦除与 BBT save 间掉电丢失工厂坏块图 | 介质/坏块 |
| 53 | 2A BBT（Bad Block Table） | RISK-BBT-04 | 待后续 | BBT header 无布局版本字段，格式演进无法识别 | 介质/坏块 |
| 54 | 2A BBT（Bad Block Table） | RISK-BBT-05 | 待上板 | 元数据区为空时可能误判空片并重扫 | L2P/元数据 |
| 55 | 2A BBT（Bad Block Table） | RISK-BBT-06 | 待后续 | BBT 回调不保存 command_id，重复完成可能错配 phase | FC 状态/完成 |
| 56 | 2A BBT（Bad Block Table） | RISK-BBT-07 | 待上板 | CP 快照并集的 BAD 不补 Spare 池 | 资源/背压 |
| 57 | 2A BBT（Bad Block Table） | RISK-BBT-10 | 待后续 | kick / factory-scan / format_erase 缺单测 | 介质/坏块 |
| 58 | 2A BBT（Bad Block Table） | RISK-BBT-12 | 待上板 | 固定元数据池无 BAD 跳过/替换 | 资源/背压 |
| 59 | 2A BBT（Bad Block Table） | RISK-BBT-13 | 待上板 | 固定元数据分区无法按 BBT 重映射 | L2P/元数据 |
| 60 | 2A BBT（Bad Block Table） | RISK-BBT-14 | 待上板 | FORMAT_IN_PROGRESS 提交点过早 | 待归类 |
| 61 | 2A BBT（Bad Block Table） | RISK-BBT-15 | 待上板 | 运行期元数据块变坏，候选池/游标不更新 | 资源/背压 |
| 62 | 2A BBT（Bad Block Table） | RISK-BBT-16 | 待上板 | 全片体检可能恢复边缘坏块 | 介质/坏块 |
| 63 | 2A BBT（Bad Block Table） | RISK-BBT-17 | 待上板 | BBT 候选池判断散落多处，易误擦 / 误留 | 资源/背压 |
| 64 | 2A BBT（Bad Block Table） | RISK-BBT-18 | 待上板 | 多圈 BBT ring 新写 record 后 reload 选到上一条 | 介质/坏块 |
| 65 | 2A BBT（Bad Block Table） | RISK-BBT-19 | 待上板 | BBT ring 块擦除失败导致持久化永久 livelock | 介质/坏块 |
| 66 | 2A BBT（Bad Block Table） | RISK-BBT-20 | 待上板 | 同步 bbt_save 与异步 job / 并发标坏无协议 | 并发/时序 |
| 67 | 2A BBT（Bad Block Table） | RISK-BBT-21 | 待上板 | 出厂扫描跳过 Block 0 的器件依据无出处 | 待归类 |
| 68 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-02 | 待上板 | carve 先于 full rebuild，写过块同时是池成员与 CLOSED | 资源/背压 |
| 69 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-03 | 待上板 | CP 读失败被当「需要格式化」，一次瞬时读错擦整片 | 掉电/恢复 |
| 70 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-04 | 待上板 | 两阶段格式化中途掉电，重启无法判断未完成 | FC 状态/完成 |
| 71 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-05 | 待上板 | 快照恢复的校验/计数改动未板验 | 掉电/恢复 |
| 72 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-06 | 待上板 | full rebuild 按第一个空白页当写指针，hole 后漏计 | 待归类 |
| 73 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-07 | 待上板 | 判空依赖 raw USER 区，加扰后不可靠 | 待归类 |
| 74 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-08 | 待上板 | Journal 扫描/回放读失败被当无更多条目 | L2P/元数据 |
| 75 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-09 | 待上板 | md_load 失败静默新建 Directory，旧映射整体丢失 | L2P/元数据 |
| 76 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-10 | 待上板 | jrn_resume 忽略跨块擦除结果，首次写落未擦页 | 介质/坏块 |
| 77 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-11 | 待上板 | 快照 probe L2P 反查失败只 LOGW，仍声称 restored | L2P/元数据 |
| 78 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-13 | 待定位 | format 后未重启就写，CLI 永久 busy（旧镜像残留） | 掉电/恢复 |
| 79 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-14 | 待上板 | full rebuild 空闲块扫描从 64 页降到 1 页 | 待归类 |
| 80 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-15 | 待上板 | full rebuild → 下次 CP 快照失效 → 每次开机 rebuild | 掉电/恢复 |
| 81 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-16 | 待上板 | full rebuild 逐 CW/逐 region 日志刷屏拖慢启动 | L2P/元数据 |
| 82 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-17 | 待决策 | 空片首启可能拒绝自动格式化 | 掉电/恢复 |
| 83 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-18 | 待上板 | 单个 L2P region 不可读 ⇒ 全局回退全盘重建 | L2P/元数据 |
| 84 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-21 | 待上板 | 降级位图被误当成「禁止加载」开关 | 待归类 |
| 85 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-22 | 待定位 | 格式化中断后可能拒绝格式化/启动 | 掉电/恢复 |
| 86 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-25 | 待定位 | 重启后整段覆盖写触发大量 dangling | 掉电/恢复 |
| 87 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-26 | 待上板 | 恢复统计 WORD 回绕导致快照被错误接受 | 掉电/恢复 |
| 88 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-28 | 待上板 | block_words 隐式 in/out，恢复顺序只靠注释 | 掉电/恢复 |
| 89 | 2B 掉电恢复 / 启动路径 | RISK-BOOT-30 | 待上板 | Verify 报 1.3 MB 数据丢失，实为读路径瞬时错读（数据完好） | 待归类 |
| 90 | 2B 掉电恢复 / 启动路径 | RISK-MD-14 | 待上板 | 双副本账本/Anchor 原子性 | L2P/元数据 |
| 91 | 2B 掉电恢复 / 启动路径 | RISK-MD-15 | 待上板 | 副本感知压缩（主/备逐副本搬移） | GC/WL/迁移 |
| 92 | 2B 掉电恢复 / 启动路径 | RISK-MD-16 | 待上板 | victim region 页读失败 ⇒ 压缩每轮同页中止 | L2P/元数据 |
| 93 | 2D Checkpoint | RISK-CP-01 | 待上板 | journal 预擦失败仍记 pre_erased_block，回切时跳过擦除 | L2P/元数据 |
| 94 | 2D Checkpoint | RISK-CP-02 | 待上板 | checkpoint 回调缺 waiting 守卫，迟到完成会按当前 phase 重走 | FC 状态/完成 |
| 95 | 2D Checkpoint | RISK-CP-03 | 待上板 | 状态机改动未板验：错误传播 / 游标复位 / 水位条目化 | FC 状态/完成 |
| 96 | 2D Checkpoint | RISK-CP-04 | 待定位 | host 写流下 CP 提交饥饿，submit stalled 后 abort | 并发/时序 |
| 97 | 2D Checkpoint | RISK-CP-05 | 待上板 | abort 被调两次，fail_phase 被覆盖为 CP_DONE + 日志刷屏 | 待归类 |
| 98 | 2D Checkpoint | RISK-CP-10 | 待上板 | T2 触发无 latch，写计数停在整数倍时 CP 空转 | 待归类 |
| 99 | 2D Checkpoint | RISK-CP-11 | 待上板 | initial record 失败块写死 plane 0/1 | 待归类 |
| 100 | 2E 磨损计数 / 静态 WL | RISK-WL-01 | 待上板 | WL 擦完用 RETIRE，RETIRED 计入 erase 统计导致容量流失 | 资源/背压 |
| 101 | 2E 磨损计数 / 静态 WL | RISK-WL-02 | 待上板 | 擦除不重置 read_count，read disturb 跨周期累计被误退役 | FC 状态/完成 |
| 102 | 2E 磨损计数 / 静态 WL | RISK-WL-03 | 待上板 | Closed SB 含 BAD plane 仍被 WL 选中，migrate 永久卡死 | 介质/坏块 |
| 103 | 2E 磨损计数 / 静态 WL | RISK-WL-04 | 待上板 | WL 收尾 FREE / read_count 复位 / BAD plane 过滤 / 磨损表校验未板验 | 介质/坏块 |
| 104 | 2E 磨损计数 / 静态 WL | RISK-WL-05 | 待后续 | wl_should_trigger 与 wl_select_victim 各做一次全表扫描 | GC/WL/迁移 |
| 105 | 2F FTL↔FC 接口 | RISK-IF-01 | 待上板 | 写位图用 1U<<cw（plane 盲），迁移 plane 1 被填 0xFF | GC/WL/迁移 |
| 106 | 2F FTL↔FC 接口 | RISK-IF-02 | 待上板 | 同步等待超时后迟到完成解引用 NULL ctx + 槽泄漏 | FC 状态/完成 |
| 107 | 2F FTL↔FC 接口 | RISK-IF-03 | 待上板 | 同步原语忽略数据搬运返回值，短链读静默返回 OK | 并发/时序 |
| 108 | 2F FTL↔FC 接口 | RISK-IF-04 | 待上板 | plane1 迁移 / wbuf 稀疏页 / 越段位图 / 迟到完成未板验 | FC 状态/完成 |
| 109 | 2F FTL↔FC 接口 | RISK-IF-06 | 待决策 | chain 派发路径不处理 RAID stripe encode | 并发/时序 |
| 110 | 2F FTL↔FC 接口 | RISK-IF-07 | 待上板 | 批次 chain 混优先级或末条 MORE 无 END 会错乱/锁死 | 并发/时序 |
| 111 | 2F FTL↔FC 接口 | RISK-IF-08 | 待上板 | page-read 聚合忽略 NAND 结果位，TIMEOUT 可静默返回陈旧数据 | 待归类 |
| 112 | 2F FTL↔FC 接口 | RISK-IF-09 | 待上板 | 放弃序列只清 RAM 不发设备 LAST | 并发/时序 |
| 113 | 2F FTL↔FC 接口 | RISK-IF-11 | 待上板 | 逐页确认依赖完成顺序与 FAILC 语义 | FC 状态/完成 |
| 114 | 2F FTL↔FC 接口 | RISK-IF-12 | 待上板 | 确认待入账 × 序列门控可能退化到 1s idle 守卫 | 并发/时序 |
| 115 | 2F FTL↔FC 接口 | RISK-IF-15 | 待上板 | RAW 读判空把全 0xFF 用户区的已编程页当空白 | 待归类 |
| 116 | 2F FTL↔FC 接口 | RISK-IF-16 | 待上板 | timeout_us 名不副实，实为自旋轮数 | 待归类 |
| 117 | 2F FTL↔FC 接口 | RISK-IF-17 | 待上板 | 超时后调用者释放仍在飞的命令，可能跨命令破坏 | 并发/时序 |
| 118 | 2F FTL↔FC 接口 | RISK-IF-18 | 待上板 | 接口层 4 处 release 守卫缺口（H14） | 输入/契约 |
| 119 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-01 | 待上板 | host 读成败只看 NAND_FAIL，UNC/MATA 被当成功 | FC 状态/完成 |
| 120 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-02 | 待上板 | multi-plane 读只记首个 desc 的块，第二块漏记 ECC/read_count | FC 状态/完成 |
| 121 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-03 | 待上板 | 短链读返回值被丢弃，未初始化缓冲交给 host | 并发/时序 |
| 122 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-05 | 待定位 | confirm park timeout = host 写失败，上游三支未定 | 待归类 |
| 123 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-06 | 待上板 | 写 program fail 直接返回 IO_ERROR，未重发 | 介质/坏块 |
| 124 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-07 | 待上板 | 停泊确认路径重入，oldp 陈旧/映射分叉 | L2P/元数据 |
| 125 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-08 | 待上板 | TRIM 已异步；CLI 诊断与 CP 冷启动 locate 仍同步 | 待归类 |
| 126 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-09 | 待后续 | TRIM 命中 wbuf 未编程 LBA 时不失效旧映射 | L2P/元数据 |
| 127 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-11 | 待定位 | Journal 压力下 TRIM 持久化可能丢失 | L2P/元数据 |
| 128 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-12 | 待议 | 组槽耗尽返回 FTL_ERROR；修法在 controller | 资源/背压 |
| 129 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-14 | 待上板 | 拔线后门控环形等待导致读回 MATA（根因已改，死锁路径未验） | 待归类 |
| 130 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-16 | 待上板 | 「无进展时长」预算实际未生效（死赋值） | 资源/背压 |
| 131 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-17 | 待上板 | host queue entry 生命周期，漏 release/重复 release | 待归类 |
| 132 | 2G Host 提交 / 完成组 / pending（`ftl.c` + `pending.c`） | RISK-HOST-22 | 待上板 | host 写取不到页对时无界重试，USB 命令可能永久挂起 | 待归类 |
| 133 | 2H 完成派发面（`ftl_op.c/h`） | RISK-CR-01 | 待上板 | ctx 旁表不按 command_id 去重，id 复用得别人 ctx | 待归类 |
| 134 | 2H 完成派发面（`ftl_op.c/h`） | RISK-CR-04 | 待上板 | request 表改 command_id 直接索引，删 op_tag | 待归类 |
| 135 | 2H 完成派发面（`ftl_op.c/h`） | RISK-CR-05 | 待上板 | pending 表 command_id 直接索引 | 待归类 |
| 136 | 2H 完成派发面（`ftl_op.c/h`） | RISK-FC-05 | 待定位 | 后台双 plane 页写留 SQ 头不发，teardown ctx 残留 | 资源/背压 |
| 137 | 2I 页打包写缓冲 wbuf（`wbuf.c` + `wbuf.h`） | RISK-WB-01 | 待上板 | FLUSH 失败丢弃页对，allocator 无归还 API，容量无声流失 | 资源/背压 |
| 138 | 2I 页打包写缓冲 wbuf（`wbuf.c` + `wbuf.h`） | RISK-WB-02 | 待上板 | TRIM/RESET 由外部模块手工改 wbuf 内部不变量 | 待归类 |
| 139 | 2I 页打包写缓冲 wbuf（`wbuf.c` + `wbuf.h`） | RISK-WB-03 | 待上板 | 模块抽出 + 位运算收敛 + 日志降级，wbuf 是写路径核心 | 待归类 |
| 140 | 2I 页打包写缓冲 wbuf（`wbuf.c` + `wbuf.h`） | RISK-WB-04 | 待上板 | 组在飞数原由多处手工维护，漏减挂死 host 写 | 待归类 |
| 141 | 2I 页打包写缓冲 wbuf（`wbuf.c` + `wbuf.h`） | RISK-WB-07 | 待定位 | wbuf 半页卡死，后续写全被拒、组永不推进 | 待归类 |
| 142 | 2I 页打包写缓冲 wbuf（`wbuf.c` + `wbuf.h`） | RISK-WB-08 | 待上板 | WBUF_STATE_FAILED 不可达，错误锁存契约与实现矛盾 | FC 状态/完成 |
| 143 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | RISK-BG-01 | 待上板 | 时钟缺失时 RR 巡检门恒假，预测巡检永久停摆 | 介质/坏块 |
| 144 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | RISK-BG-04 | 待上板 | 任务表装载无边界检查，加项越界踩 task_count | 输入/契约 |
| 145 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | RISK-BG-05 | 待上板 | 调度器死字段 + priority 与表序两套坐标漂移 | 待归类 |
| 146 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | RISK-BG-06 | 待上板 | l2p_flush_max 经 level 反查，第二套配置读取 | 待归类 |
| 147 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | RISK-BG-07 | 待上板 | journal_erase 手工读 journal 内部 phase/need_erase | L2P/元数据 |
| 148 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | RISK-BG-10 | 待上板 | 后台节拍 flag + token bucket（grant 太小/太大风险） | 待归类 |
| 149 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | RISK-BG-11 | 待上板 | 档位下标无值域断言，越界静默读相邻数据 | 输入/契约 |
| 150 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | RISK-DOC-03 | 待上板 | ftl.c 注释/代码分叉（H6/H37） | 待归类 |
| 151 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | RISK-LOG-02 | 待上板 | GC 事件在极端 victim 速率下仍可能占 UART | GC/WL/迁移 |
| 152 | 2J FTL service 与后台调度器（`ftl.c` + `bg_scheduler.c/h`） | RISK-LOG-03 | 待上板 | GC no-gain 刷屏（边沿复用） | GC/WL/迁移 |
| 153 | 2L FTL 模块 handle 契约 | RISK-ARCH-02 | 待后续 | 跨 LUN superblock 条带 / 跨 LUN GC/WL 未实现 | GC/WL/迁移 |
| 154 | 2L FTL 模块 handle 契约 | RISK-ARCH-05 | 待上板 | 独立 confirm_handle 未采用，confirm/TRIM 两处顺序实现 | 待归类 |
| 155 | 2L FTL 模块 handle 契约 | RISK-ARCH-07 | 待上板 | mig/wbuf/rr_page_scratch 改挂 ftl_storage_t，数组落 AXI | 待归类 |
| 156 | 2L FTL 模块 handle 契约 | RISK-ARCH-08 | 待上板 | 提交/等待计数改绝对 ms deadline | 并发/时序 |
| 157 | 2L FTL 模块 handle 契约 | RISK-ARCH-09 | 待上板 | PFR 收为 boot orchestrator，内部原语全部 static | 掉电/恢复 |
| 158 | 2L FTL 模块 handle 契约 | RISK-ARCH-11 | 待上板 | 端口删除后模块直接持有 owner 指针 | 待归类 |
| 159 | 2M USB MSC / 平台 | RISK-PLAT-01 | 待上板 | `.noncacheable_buffer` 段匹配漏前导点，页缓冲落进可缓存 AXI RAM | 待归类 |
| 160 | 2M USB MSC / 平台 | RISK-PLAT-07 | 待上板 | 各构建配置共用 debug.elf，UT 互相覆盖 | 待归类 |
| 161 | 2M USB MSC / 平台 | RISK-PLAT-08 | 待定位 | `.noinit` 出现一次 HardFault（PC 落 newlib printf） | 待归类 |
| 162 | 2M USB MSC / 平台 | RISK-USB-02 | 待上板 | MSC 异步完成/deferred/discard 路径未实测 | FC 状态/完成 |
| 163 | 2M USB MSC / 平台 | RISK-USB-05 | 待上板 | WRITE(10) FUA 原被忽略（已实现等确认后回执） | 待归类 |
| 164 | 2M USB MSC / 平台 | RISK-USB-07 | 待上板 | request->io 模板的 cmd_remaining 跨命令残留 | 待归类 |
| 165 | 2M USB MSC / 平台 | RISK-USB-08 | 待上板 | 写 lookahead 提前回执/BUSY 背压未注入失败 | 资源/背压 |
| 166 | 2M USB MSC / 平台 | RISK-USB-09 | 待上板 | fsync 被自己的在飞写槽拒掉（已加 FLUSH 专用槽） | 待归类 |
| 167 | 2M USB MSC / 平台 | RISK-USB-10 | 待上板 | slot_match 把真正的下一条相同写当事件重放丢弃 | 待归类 |
| 168 | 2M USB MSC / 平台 | RISK-USB-11 | 待上板 | TinyUSB 本地补丁清单不完整（cbw_info 钩子） | 待归类 |
| 169 | 2M USB MSC / 平台 | RISK-USB-12 | 待上板 | SYNCHRONIZE CACHE(16) 0x91 未支持 | 待归类 |
| 170 | 2M USB MSC / 平台 | RISK-USB-13 | 待上板 | 两写槽占用时 READ 返回 ERROR 而非 BUSY | 介质/坏块 |
| 171 | 2M USB MSC / 平台 | RISK-USB-14 | 待上板 | SCSI UNMAP 已实现，未上板 | 待归类 |
| 172 | 2M USB MSC / 平台 | RISK-USB-15 | 待决策 | UNMAP 未向主机广告，fstrim 可能不下发 | 待归类 |
| 173 | 2M USB MSC / 平台 | RISK-USB-16 | 待上板 | UNMAP descriptor 按 8B 步进（规范 16B）已改 | 待归类 |
| 174 | 2O NAND / FMC 时序与吞吐 | RISK-NAND-05 | 待定位 | used 介质不可纠读页，L2P 仍指向坏块，每次读重试 8 次 | L2P/元数据 |
| 175 | 2O NAND / FMC 时序与吞吐 | RISK-NAND-06 | 待定位 | 运行期 BAD 标记来源未定：Mode 5 时序 / 同页二次编程 / 边缘块 | 介质/坏块 |
| 176 | 2P allocator | RISK-ALLOC-09 | 待上板 | 低 free 时成对分配失败导致写路径永久 stall（plane 不平衡） | 资源/背压 |
| 177 | 2P allocator | RISK-ALLOC-06 | 待后续 | `ftl_gblock` 无入参契约，哨兵可截断成合法 gblock | 输入/契约 |
| 178 | 2P allocator | RISK-ALLOC-12 | 待上板 | 出池后 state 仍为 FREE，漏赋新态时块永久漏账 | 资源/背压 |
| 179 | 2P allocator | RISK-ALLOC-13 | 待上板 | `alc_super_open_rewind_pair` 无下溢守卫，`next_page--` 掩码成页 63 | 输入/契约 |
| 180 | 2P allocator | RISK-ALLOC-15 | 待上板 | 全 plane BAD 的 SB 关闭时追加零 plane 条目，列表槽泄漏 | 介质/坏块 |
| 181 | 2P allocator | RISK-ALLOC-16 | 待上板 | `alc_spare_pop` 不跳 BAD，坏块替换假报成功并丢 spare | 资源/背压 |
| 182 | 2P allocator | RISK-ALLOC-03 | 待上板 | 纯转发访问器与 `alloc->blocks[]` 直接访问形成两套坐标 | 待归类 |
| 183 | 2P allocator | RISK-ALLOC-07 | 待上板 | `ftl_tables_t` 五数组打包，单表无法换内存域 | 待归类 |
| 184 | 2P allocator | RISK-ALLOC-08 | 待上板 | `alc_super_close_live` 吞 append 失败码，Closed 列表满时漏账 | 资源/背压 |
| 185 | 2R 位运算公共宏 | RISK-BIT-01 | 待后续 | 通用掩码构造器挂在 NAND 层，FTL/FC 各自另写字面量宏 | 待归类 |
| 186 | 2V 板端读完整性事件 | RISK-RD-01 | 待定位 | 单次不可纠读，状态 `CRC\ | FC 状态/完成 |
| 187 | 2W pending 表 / 完成组 | RISK-HOST-15 | 待上板 | 写确认 handler 用文件级全局 + `void *`，UT 需 save/restore | 掉电/恢复 |
| 188 | 2Z cache 长流 + Journal 顺序映射页 | RISK-CACHE-15 | 待上板 | 无数据 END 缺失，padding 页盘满例外未验 | 资源/背压 |
| 189 | 2Z cache 长流 + Journal 顺序映射页 | RISK-CACHE-16 | 待上板 | 跨命令序列 + VWC 早回执，USB 端到端未跑 | 并发/时序 |
| 190 | 2Z cache 长流 + Journal 顺序映射页 | RISK-CACHE-18 | 待上板 | 提前回执扩大掉电暴露窗口，VWC 窗口未量化 | 掉电/恢复 |
| 191 | 2Z cache 长流 + Journal 顺序映射页 | RISK-CACHE-23 | 待上板 | 持续写期间后台 CP/L2P 被序列门控饿死 | L2P/元数据 |
| 192 | 2Z cache 长流 + Journal 顺序映射页 | RISK-CACHE-26 | 待定位 | cache 写压测 1536 LBA 读回全 0 | 待归类 |
| 193 | 2Z cache 长流 + Journal 顺序映射页 | RISK-CACHE-27 | 待上板 | 早回执后读路径缺覆盖层，写完立刻读回旧数据 | 待归类 |
| 194 | 2Z cache 长流 + Journal 顺序映射页 | RISK-CACHE-28 | 待上板 | FAILC 撤销按 region 粒度降级，跨重启丢失 | FC 状态/完成 |
| 195 | 2Z cache 长流 + Journal 顺序映射页 | RISK-CACHE-29 | 待上板 | 早回执改变 flush/断电口径，长流+断电未量 | 掉电/恢复 |
| 196 | 2Z cache 长流 + Journal 顺序映射页 | RISK-CACHE-30 | 待上板 | padding 收尾把已证明未确认页误判失败 | 待归类 |
| 197 | 2Z cache 长流 + Journal 顺序映射页 | RISK-CACHE-31 | 待上板 | padding 回调早退分支泄漏 FC 命令 | 待归类 |
| 198 | 2Z cache 长流 + Journal 顺序映射页 | RISK-CACHE-32 | 待上板 | 环满在 release 下静默丢已回执写 | 资源/背压 |
| 199 | 2Z cache 长流 + Journal 顺序映射页 | RISK-CACHE-36 | 待上板 | 干净释放写序列时不看在飞成员，迟到完成把未证明页推进死序列 | FC 状态/完成 |
| 200 | 2Z cache 长流 + Journal 顺序映射页 | RISK-USBC-01 | 待上板 | deferred error 以下一条命令失败近似，非标准 sense | 待归类 |
| 201 | 2Z cache 长流 + Journal 顺序映射页 | RISK-USBC-03 | 待定位 | USB 盘间歇报无介质，主机侧已止血 | 待归类 |
| 202 | 2AA 平台定时调度器（`timer`） | RISK-PLAT-09 | 待上板 | SysTick 时基与 TIM6 调度心跳的超时判据未板验 | 并发/时序 |
| 203 | 2AA 平台定时调度器（`timer`） | RISK-PLAT-10 | 待上板 | mig_eval 混用 TIM6 与 SysTick 两套时间坐标 | 待归类 |
| 204 | 2AA 平台定时调度器（`timer`） | RISK-PLAT-12 | 待上板 | µs 时间戳从 DWT 改 SysTick，未板验 | 待归类 |
| 205 | 2AB NAND 拓扑扫描（`nand_target_probe` / `ftl_topology_build`） | RISK-TOP-01 | 待上板 | 扫描得到的多 target / 多 LUN 拓扑路径未板验 | 待归类 |
| 206 | 2AB NAND 拓扑扫描（`nand_target_probe` / `ftl_topology_build`） | RISK-TOP-02 | 待上板 | sparse 拓扑下 controller FLUSH 按 3D 槽遍历会空指针 | 待归类 |
| 207 | 2AB NAND 拓扑扫描（`nand_target_probe` / `ftl_topology_build`） | RISK-TOP-03 | 待上板 | LUN 数按器件探测，ONFI 偶发失败即拒绝启动 | 待归类 |
| 208 | 2AC CLI 调试前端（`controller/cli.c`） | RISK-CLI-01 | 待上板 | 长命令内联执行阻塞主循环，IWDG 启用后会复位 | 待归类 |
| 209 | 2AC CLI 调试前端（`controller/cli.c`） | RISK-CLI-02 | 待上板 | format/bbt erase 不检查 host 在飞，会造成介质状态分叉 | FC 状态/完成 |
| 210 | 2AC CLI 调试前端（`controller/cli.c`） | RISK-CLI-03 | 待上板 | wp 复用静态缓冲无在飞守卫，重入会写错字节 | 并发/时序 |
| 211 | 2AC CLI 调试前端（`controller/cli.c`） | RISK-CLI-04 | 待上板 | release help 宣传被禁用的 bbt 子命令 | 待归类 |
| 212 | 2AC CLI 调试前端（`controller/cli.c`） | RISK-CLI-05 | 待上板 | rd/bbt 语法变更，旧脚本会报 unknown option | 待归类 |
| 213 | 2AC CLI 调试前端（`controller/cli.c`） | RISK-CLI-06 | 待上板 | q/cp/jrn/stat/id/ecc/wear 字段口径未板端核对 | 待归类 |
| 214 | 2AC CLI 调试前端（`controller/cli.c`） | RISK-CLI-07 | 待上板 | rd 失败仍打印 rd ok 并 dump 旧缓冲 | 待归类 |
| 215 | 2AC CLI 调试前端（`controller/cli.c`） | RISK-CLI-08 | 待上板 | wp 绕过 host gate，可与 USB/CLI 并发进入 FTL | 并发/时序 |
| 216 | 2AD Controller 请求拆分（`request.c` / `controller.c`） | RISK-CTRL-01 | 待上板 | 多 run 请求中途失败时父请求提前完成、错误被覆盖 | FC 状态/完成 |
| 217 | 2AD Controller 请求拆分（`request.c` / `controller.c`） | RISK-CTRL-03 | 待决策 | `ssd_controller_init` 稀疏拓扑注释与实现相反 | 待归类 |
| 218 | 2AD Controller 请求拆分（`request.c` / `controller.c`） | RISK-CTRL-04 | 待后续 | HIL submit 未校验 buffer NULL，FTL 兜底但状态分类错误 | FC 状态/完成 |
| 219 | 2AE HIL 主机前端 / host queue | RISK-HOST-20 | 待决策 | HIL host 前端互斥不是单一闸门，同一不变量 5 处手工维护 | 待归类 |
| 220 | 2AE HIL 主机前端 / host queue | RISK-HOST-21 | 待后续 | HIL 没有 host 命令超时 / 中止 | 并发/时序 |
| 221 | 2AF FC 资源池 / 生命周期 | RISK-POOL-01 | 待上板 | buffer 归属表缺失，双释放 / 在飞释放无运行期判据 | 待归类 |
| 222 | 2AF FC 资源池 / 生命周期 | RISK-POOL-03 | 待上板 | desc 链释放 / 遍历缺上界与环检测 | 并发/时序 |
| 223 | 2AF FC 资源池 / 生命周期 | RISK-POOL-04 | 待上板 | 空闲链哨兵混用裸常量与具名 ID | 并发/时序 |
| 224 | 2AF FC 资源池 / 生命周期 | RISK-POOL-05 | 待上板 | 资源统计靠三池全扫，计数可能漂移 | 资源/背压 |
| 225 | 2AF FC 资源池 / 生命周期 | RISK-POOL-06 | 待上板 | 单字段双语义，失败坐标字段无读者 | 待归类 |
| 226 | 2AF FC 资源池 / 生命周期 | RISK-POOL-07 | 待上板 | 内部 API 后缀 / 命名不合 H49 | 输入/契约 |
| 227 | 2AF FC 资源池 / 生命周期 | RISK-POOL-08 | 待上板 | FC 注释承载变更史与版本号 | 待归类 |
| 228 | 2AG 数据面 / NAND dispatch | RISK-DP-01 | 待上板 | ECC 引擎不可用被归一为成功，写不编码、读不校验 | FC 状态/完成 |
| 229 | 2AG 数据面 / NAND dispatch | RISK-DP-02 | 待上板 | 数据路径编码错误被调用方丢弃，ECC 失败仍提交 NAND | FC 状态/完成 |
| 230 | 2AG 数据面 / NAND dispatch | RISK-DP-03 | 待上板 | INVALID buffer 进入数据路径，release 下越界/空指针 | 输入/契约 |
| 231 | 2AG 数据面 / NAND dispatch | RISK-DP-04 | 待上板 | desc 链遍历无上界，成环会死循环 | 并发/时序 |
| 232 | 2AG 数据面 / NAND dispatch | RISK-DP-05 | 待上板 | RAID 边界不完整，恢复后丢 CRC/META | FC 状态/完成 |
| 233 | 2AG 数据面 / NAND dispatch | RISK-DP-07 | 待上板 | 链式失败生命周期半状态，整链完成时断言 ACTIVE | FC 状态/完成 |
| 234 | 2AG 数据面 / NAND dispatch | RISK-DP-08 | 待上板 | 通道轮询注释称 round-robin，实际每轮从 CH0 开始 | 待归类 |
| 235 | 2AG 数据面 / NAND dispatch | RISK-DP-09 | 待上板 | FC 内 engine↔dispatch 双向依赖，armed 闩锁泄漏进引擎 | 待归类 |
| 236 | 2AG 数据面 / NAND dispatch | RISK-DP-10 | 待上板 | dispatch enqueue 入口检查只有 debug ASSERT | 输入/契约 |
| 237 | 2AG 数据面 / NAND dispatch | RISK-DP-11 | 待上板 | dispatch 忽略 µs 时钟失败，LUN 永久停在 ISSUED | 待归类 |
| 238 | 2AG 数据面 / NAND dispatch | RISK-DP-12 | 待上板 | 单 plane cache 读失败只标最低位 CW | 待归类 |
| 239 | 2AG 数据面 / NAND dispatch | RISK-DP-13 | 待上板 | ecc_bit_count 无生产者，恒为 0 | 待归类 |
| 240 | 2AG 数据面 / NAND dispatch | RISK-DP-14 | 待上板 | ECC 枚举 0 命名为 BCH，运行时模式与实现不一致 | FC 状态/完成 |
| 241 | 2AG 数据面 / NAND dispatch | RISK-DP-15 | 待后续 | LDPC 分支引用不存在的 g_qc，切构建即编译失败 | FC 状态/完成 |
| 242 | 2AG 数据面 / NAND dispatch | RISK-DP-16 | 待上板 | dispatch poll 用 BYTE 返完成数，256 回绕为 0 | FC 状态/完成 |
| 243 | 2AG 数据面 / NAND dispatch | RISK-DP-17 | 待上板 | JUMP_IF_NOT_READY 回跳减法无 release 下溢守卫 | 输入/契约 |
| 244 | 2AG 数据面 / NAND dispatch | RISK-DP-18 | 待上板 | random read UOP 缺 CHECK_FAIL，中间 CW 失败被吞 | FC 状态/完成 |
| 245 | 2AG 数据面 / NAND dispatch | RISK-DP-19 | 待上板 | 单 plane cache 读 UOP 缺 CHECK_FAIL | FC 状态/完成 |
| 246 | 2AG 数据面 / NAND dispatch | RISK-DP-20 | 待上板 | cache 编程首页 FAILC 误报，误判好页失败 | FC 状态/完成 |
| 247 | 2AG 数据面 / NAND dispatch | RISK-DP-21 | 待上板 | 参数页/唯一 ID/GET_FEATURES 的 FAIL 被当成成功 | FC 状态/完成 |
| 248 | 2AG 数据面 / NAND dispatch | RISK-DP-22 | 待上板 | WAIT_READY 后端阻塞，引擎协作让出路径未生效 | 待归类 |
| 249 | 2AG 数据面 / NAND dispatch | RISK-DP-23 | 待上板 | fc_nand_start 依赖调用者预绑定 engine 指针，契约未声明 | 输入/契约 |
| 250 | 2AG 数据面 / NAND dispatch | RISK-DP-24 | 待上板 | 高速模式失败后没有回滚到低速 | 待归类 |
| 251 | 2AG 数据面 / NAND dispatch | RISK-DP-25 | 待上板 | dispatch 不校验 req/completion 表项非空，空指针崩溃 | 输入/契约 |
| 252 | 2AG 数据面 / NAND dispatch | RISK-DP-26 | 待上板 | 带 DATA_OUT 的稀疏 CW 读首输出读错 CW | 待归类 |
| 253 | 2AG 数据面 / NAND dispatch | RISK-DP-27 | 待后续 | LDPC 解码栈占用 4.2KB，超 2KiB 栈预算 | FC 状态/完成 |
| 254 | 2AG 数据面 / NAND dispatch | RISK-DP-28 | 待后续 | CRC 初始化失败在 release 下静默，校验恒通过 | FC 状态/完成 |
| 255 | 2AH FC 主循环 / 调度 / CQ | RISK-SCH-01 | 待上板 | 链成员跨目标只在 debug 拦截，release 仍按链头 LUN 入队 | 并发/时序 |
| 256 | 2AH FC 主循环 / 调度 / CQ | RISK-SCH-02 | 待上板 | fc_core_poll 只在无完成时填充，持续完成下提交延迟 | FC 状态/完成 |
| 257 | 2AH FC 主循环 / 调度 / CQ | RISK-SCH-03 | 待决策 | CQ 满会丢完成项，FTL 永远收不到命令 | FC 状态/完成 |
| 258 | 2AH FC 主循环 / 调度 / CQ | RISK-SCH-05 | 待上板 | fill_pops 少计链成员 pop，链长 N 少计 N-1 | 并发/时序 |
| 259 | 2AH FC 主循环 / 调度 / CQ | RISK-SCH-06 | 待上板 | 断言终端路径再次断言，递归覆盖原始现场 | 待归类 |
| 260 | 2AH FC 主循环 / 调度 / CQ | RISK-SCH-07 | 待上板 | fc_submit_batch 不拒绝批内重复命令指针 | 待归类 |
| 261 | 2AH FC 主循环 / 调度 / CQ | RISK-SCH-08 | 待上板 | fc_get_cmd_data_ptr 的 cw_index 不含 plane | 待归类 |
| 262 | 2AH FC 主循环 / 调度 / CQ | RISK-SCH-09 | 待上板 | staging helper 无 plane/cw0/cmd_id 范围守卫 | 输入/契约 |
| 263 | 2AH FC 主循环 / 调度 / CQ | RISK-SCH-10 | 待上板 | 合成失败完成项忽略 CQ 满，FTL 等不到完成 | FC 状态/完成 |
| 264 | 2AH FC 主循环 / 调度 / CQ | RISK-SCH-11 | 待上板 | 完成侧不变量只有 debug ASSERT，release 继续写 CQ | FC 状态/完成 |
| 265 | 2AH FC 主循环 / 调度 / CQ | RISK-SCH-12 | 待上板 | fc_submit_batch 入参/提交不变量只有 debug ASSERT | 待归类 |
| 266 | 2AI NAND 器件 / PHY | RISK-NANDP-01 | 待上板 | 多 target 无真实 FMC bank/CE# 切换，命令可能打错 target | 待归类 |
| 267 | 2AI NAND 器件 / PHY | RISK-NANDP-02 | 待上板 | UOP operand 越界时 release 无守卫，会越界访问 buffer/plane | 资源/背压 |
| 268 | 2AI NAND 器件 / PHY | RISK-NANDP-04 | 待上板 | 多 target 板卡未逐 target 初始化，backend handlers 为空 | 待归类 |
| 269 | 2AI NAND 器件 / PHY | RISK-NANDP-05 | 待后续 | 多 target timing 共享单套 HAL 句柄，切模式影响整块 FMC | 待归类 |
| 270 | 2AI NAND 器件 / PHY | RISK-NANDP-06 | 待上板 | target 选择与 backend 获取拆成两半，漏调用会静默写错 target | 待归类 |
| 271 | 2AI NAND 器件 / PHY | RISK-NANDP-07 | 待上板 | HIL 依赖器件专用 timing 查询，换器件会编译失败 | 待归类 |
| 272 | 2AI NAND 器件 / PHY | RISK-NANDP-08 | 待上板 | timing 表部分字段无消费者，改表不生效 | 待归类 |
| 273 | 2AI NAND 器件 / PHY | RISK-NANDP-10 | 待上板 | timing 零值 release 下静默 delay(0)，时序不足 | 待归类 |
| 274 | 2AI NAND 器件 / PHY | RISK-NANDP-11 | 待上板 | timing 表命名 Mode0 实为 Mode5，误改会拖慢高速 | 输入/契约 |
| 275 | 2AI NAND 器件 / PHY | RISK-NANDP-12 | 待上板 | 多 target 无真实 bank 切换，跨轮次可能选错 target | 待归类 |
| 276 | 2AI NAND 器件 / PHY | RISK-NANDP-13 | 待上板 | timing 查询/设置 release 无边界，越界会写坏状态 | FC 状态/完成 |
| 277 | 2AI NAND 器件 / PHY | RISK-NANDP-14 | 待上板 | 多条 busy 启动命令缺 tWB，WAIT_READY 提前返回读到旧数据 | 待归类 |
| 278 | 2AI NAND 器件 / PHY | RISK-NANDP-15 | 待上板 | 首次 RESET timeout 500µs 小于 datasheet 1ms，冷启动可能误判 | 待归类 |
| 279 | 2AI NAND 器件 / PHY | RISK-NANDP-17 | 待上板 | tR timeout 38µs 小于 tR_ECC 70µs，启用内部 ECC 会提前超时 | FC 状态/完成 |
| 280 | 2AJ 平台返回状态 | RISK-PLAT-13 | 待上板 | `dma_port_uart_tx_init` 失败返 SUCCESS、成功返 ERROR | 介质/坏块 |
| 281 | 2AK 构建档 / launch 配置 | RISK-BUILD-01 | 待上板 | `DEBUG_OPT=-O0` 静默复用 -O2 对象 | 资源/背压 |
| 282 | 2AK 构建档 / launch 配置 | RISK-BUILD-02 | 待上板 | launch 配置档位不符，产品/UT 共用 ELF 路径，易烧错镜像 | 待归类 |
| 283 | 2AL HIL UT | RISK-UT-34 | 待后续 | HIL 契约覆盖缺口：`is_complete`、NULL、队列账本、flush 持久化 | 资源/背压 |
| 284 | 2C L2P cache | RISK-L2P-01 | 待上板 | region 加载失败曾立即降级为全未映射，映射静默不可用 | L2P/元数据 |
| 285 | 2C L2P cache | RISK-L2P-02 | 待上板 | flush_kick 先取槽后建链，命令池空重试浪费日志槽 | L2P/元数据 |
| 286 | 2C L2P cache | RISK-L2P-03 | 待上板 | flush_kick 背压/资源预检/CLOCK 引用位的行为未板验 | L2P/元数据 |
| 287 | 2C L2P cache | RISK-L2P-04 | 待上板 | 降级启动被误报 restored，丢 journal 未覆盖的旧映射 | L2P/元数据 |
| 288 | 2C L2P cache | RISK-L2P-05 | 待上板 | 备副本提交时主命令未 release，命令池吃紧时 flush 假失败 | L2P/元数据 |
| 289 | 2C L2P cache | RISK-L2P-06 | 待上板 | 批量读越界分支被压成 FTL_AGAIN，会永久重试（当前不可达） | L2P/元数据 |
| 290 | 2C L2P cache | RISK-L2P-07 | 待上板 | 顺序读预取白占 cache 行/资源，可能延迟 FLUSH | L2P/元数据 |
| 291 | 2C L2P cache | RISK-L2P-12 | 待上板 | 读/oldp 路径漏查 CMT ⇒ 返回陈旧值 | L2P/元数据 |
| 292 | 2C L2P cache | RISK-L2P-13 | 待上板 | Header 先于 dirty CMT 落盘 ⇒ 掉电丢映射 | L2P/元数据 |
| 293 | 2C L2P cache | RISK-L2P-14 | 待上板 | Journal replay 写进 dirty CMT 后被 PFR 误清 | L2P/元数据 |
| 294 | 2C L2P cache | RISK-L2P-15 | 待上板 | 失败页撤销误保留 dirty CMT ⇒ 读到 FAILC 页 | L2P/元数据 |
| 295 | 2C L2P cache | RISK-L2P-16 | 待上板 | 容量竞态导致批量写半更新 ⇒ Journal 重复记账 | L2P/元数据 |
| 296 | 2C L2P cache | RISK-L2P-17 | 待上板 | thrashing 门挡住必需写回 ⇒ CMT 满后 host 写停住 | L2P/元数据 |
| 297 | 2C L2P cache | RISK-L2P-18 | 待上板 | dirty CMT 低于水位只在 RAM，掉电依赖 Journal | L2P/元数据 |
| 298 | 2C L2P cache | RISK-L2P-19 | 待上板 | 孤儿实时脏位让 CP 空转/Header 谎报 | L2P/元数据 |
| 299 | 2C L2P cache | RISK-L2P-20 | 待上板 | line hint 陈旧/冲突，只掉命中率不返回错行 | L2P/元数据 |
| 300 | 2C L2P cache | RISK-L2P-21 | 待上板 | Directory/备份 PBA 未校验就进 FC ⇒ 越界路由 | L2P/元数据 |
| 301 | 2C L2P cache | RISK-L2P-22 | 待上板 | CMT dirty 计数在 0 处静默夹取 ⇒ 丢映射或 FLUSH 超时 | L2P/元数据 |
| 302 | 2C L2P cache | RISK-L2P-23 | 待上板 | l2p_line_invalidate 不执行「仅 CLEAN」契约 | L2P/元数据 |
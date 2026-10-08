# v25：`nand_config.h` 地址构造收敛——位置形参 → `nand_addr_t` 具名字段

- **日期**：2026-10-08
- **范围**：`include/nand/nand_config.h`、`include/ftl/{ftl_fc_interface,l2p_cache}.h`、`src/fc/{fc,fc_interface}.c`、`src/ftl/{bbt,journal,metadata,power_failure_recovery,read_recovery,wear_leveling}.c`、`tests/{unit/test_rebuild.c,integration/test_fc_nand.c}`（**构建外**，随构造器删除机械同步）、文档
- **前置**：v24（地址布局统一 LBA / PBA / PAA）
- **介质格式**：**无变更**——本轮的字段值逐位不变，不重新格式化。
- **上板未验**：本项目硬规则。
- **测试**：本轮**未跑 `make tests`**。构建内的 `test_fc_level2.c` / `test_fc_level3.c` 由用户同步重构中
  （本轮提交时其调用点未改，`make tests` 会编译失败）；构建外的 `test_rebuild.c` / `test_fc_nand.c`
  已随构造器删除一并改掉（二者不在任何 makefile 的 `TEST_C_SRCS` 里）。

## 背景

用户提问：「`nand_config.h` 中有些函数参数太多，需不需要定义一个结构体之后传指针」。

**核查结论：结构体已经有了，指针版构造器也已经有了一个。**

| 项 | 位置 | 状态 |
|---|---|---|
| `nand_addr_t`（7 个具名字段 `ch, ce, lun, block, plane, page, cw`） | `nand_config.h:453-462` | 已存在 |
| `pba_from_addr(const nand_addr_t *)` | `nand_config.h:465-468` | 已存在，**函数体就是 `pba_make(...)`** |
| `paa_from_addr(const nand_addr_t *)` | — | **不存在**（PAA 只有 5 形参 `paa_make`） |

也就是说 **`pba_make` 与 `pba_from_addr` 是同一个函数的两种调用约定**，而 `pba_from_addr`
存在的唯一理由是跨过两套字段顺序：

| | 顺序 |
|---|---|
| `paa_make` / `pba_make` | `…, page, plane, block, lun, …` |
| `nand_addr_t` 字段 | `ch, ce, lun, block, plane, page, cw` |

**恰好相反。** 七个同型小整数，写反了**仍然编译通过、仍然落在合法值域内**
（`page<64`、`plane<2`、`block<2048` 全成立）⇒ 静默算出错地址。这才是真缺陷——
不是形参个数。

## 现状普查（全部 grep 过，非推断）

| 函数 | 形参 | 生产调用点 | 头内调用点 | 处置 |
|---|---|---|---|---|
| `pba_make` | 7 | 6（journal×2、fc×2、wear_leveling×1）+ 测试 3 | 4（`l2p_cache.h:50`、`ftl_fc_interface.h:151`、`nand_config.h:353/467`） | **删**，迁到 `pba_from_addr` |
| `paa_make` | 5 | 7（bbt、metadata×3、pfr、read_recovery、`fc.c:1104`）+ 测试 2 | 1（`nand_config.h:328`） | **删**，迁到新增的 `paa_from_addr` |
| `paa_is_valid` | 5 | **0** | 0 | **保留**（换签名） |
| `paa_to_pba` | 3 | **0** | 0 | **保留**（改内部实现） |
| `pba_to_nand_row` | 1 | **0** | 0 | **保留**（随之自动跟上） |

## 用户已定的决策

1. **传指针**（`const nand_addr_t *`），与既有的 `pba_from_addr` 一致。
2. **`pba_from_addr` 不改名**。
3. **3 个零调用者函数保留**（不按 §十七判据 5 删）。
4. **测试源由用户自己重构** ⇒ 本轮不动 `tests/`、不跑 `make tests`。
5. **允许前移 `nand_addr_t` 的 typedef**。

## 改动

### A. `include/nand/nand_config.h`（109 行）

1. **`nand_addr_t` typedef 前移**到 PAA 段之前（`typedef uint32_t nand_paa_t;` 之后），
   附设计动机注释。它是纯 POD、只依赖 `stdint.h`；前移后 `paa_from_addr` 才能定义在
   `pba_to_paa` 之前。
2. **`paa_make` → `paa_from_addr(const nand_addr_t *addr)`**（原位替换）。函数体沿用原打包
   表达式，改读结构体字段。**PAA 不含 ch/ce，函数忽略 `addr->ch/ce`，注释写明。**
3. **`pba_make` → `pba_from_addr(const nand_addr_t *addr)`**（原位替换），**并删掉尾部那份
   重复定义**——同一函数此后只有一处。
4. **`pba_to_paa` 改用结构体重排**（删 `paa_make` 后必须改）。
5. **`paa_to_pba` 改用结构体**（保留函数，内部实现重写）。
6. **`paa_is_valid` → `paa_is_valid_addr(const nand_addr_t *addr)`**。它的形参序
   `page, plane, block, lun, cw` **就是**我们正在消灭的那一套——保留函数不等于保留隐患。
7. 尾部段头注释与各处过时注释同步（「不再先 `pba_make` 打包」→「不再打包」等）。

定义顺序（已验证可编译）：
`nand_addr_t`(≈121) → `paa_from_addr`(≈143) → `paa_is_valid_addr`(≈152) → `paa_to_*`
→ `pba_to_*`(290-323) → `pba_to_paa`(326) → `pba_from_addr`(337) → `paa_to_pba`(351)
→ `addr_from_pba` / `addr_from_paa`（尾部）。

### B. 生产调用点（13 处，机械替换）

模式：`f(a, b, c, …)` → `f_from_addr(&(nand_addr_t){.x = …, .y = …})`，**只写非零字段**
（其余自动为 0，语义与原来传 `0` 完全一致）。

| 文件:行 | 改为 |
|---|---|
| `src/ftl/journal.c:49/50` | `pba_from_addr(&(nand_addr_t){.ch = topo.ch, .ce = topo.ce, .block = JOURNAL_BLOCKn})` |
| `src/fc/fc.c:909/922` | 同 7 字段具名版（RAID stripe，默认不编译） |
| `src/ftl/wear_leveling.c:202` | `pba_from_addr(&(nand_addr_t){…, .block = victim->plane_blocks[plane], .plane = plane, .page = page})` |
| `include/ftl/l2p_cache.h:50` | `pba_from_addr(&(nand_addr_t){…})` |
| `include/ftl/ftl_fc_interface.h:151` | **`pba_from_addr(addr)`** ← 白捡；与 `:136` 注释早已写明的写法一致 |
| `src/ftl/bbt.c:132` | `paa_from_addr(&(nand_addr_t){.plane = …, .block = …})` |
| `src/ftl/metadata.c:60/170/382` | 同形 |
| `src/ftl/power_failure_recovery.c:375` | 同形 |
| `src/ftl/read_recovery.c:188` | 同形 |
| `src/fc/fc.c:1104` | **可选简化**：`paa_with_plane(paa_page_base(descriptor->paa), plane)`（逐位等价） |

**顺带消失的三处「拆包再打包」**：`pba_from_addr` 体内、`addr_to_cmd`（`:151`）、
`pba_to_paa` —— 它们存在的唯一理由就是跨这两套顺序。

## 关于「要不要把 `nand_addr_t` 改成位域、压到一个 DWORD」：**不改**

用户问了这一点。五条理由，都查证过：

**① 本仓已有明文决策，方向正相反。** `include/common/ftl_types.h:20-22`：

> 设计原则：请求结构体用普通字段而非位域（访问更快，C 编译器无位域可移植性问题），
> 地址一律用 `nand_addr_t`（展开）或 `nand_pba_t`（L2P 表紧凑存储）。

位域化 `nand_addr_t` 就是直接反转这条已落文档的原则。

**② 本构建下编译不过。** `LUN_ADDR_WIDTH = 0U`（`nand_config.h:96`）。C11 6.7.2.1p4：
宽度为 0 的位域**不得有声明符**，具名 0 宽位域是约束违反。实测：

```
$ printf 'typedef struct{unsigned a:2; unsigned lun:0; unsigned b:3;}S;' | gcc -x c -std=c11 -fsyntax-only -
<stdin>:1:41: error: zero width for bit-field 'lun'
```

要绕开只能 `#if LUN_ADDR_WIDTH > 0` 包一层（结构体随宏变形）或写死宽度（与 `LUN_ADDR_WIDTH` 分家）。

**③ 布局对应关系无法被任何静态断言检查。** 位域在一个存储单元内的分配顺序、是否跨单元都是
implementation-defined（C11 6.7.2.1p11）；`offsetof` 对位域**非法**。所以没有任何
`_Static_assert` 能证明 `plane` 落在 bit 2。今天的 `pba_from_addr` 与 `pba_to_*` 都从同一组
`PBA_*_SHIFT` 宏派生——改移位宏两边同时跟着变，不可能失配；位域版把布局**抄了第二遍**，
而编译器不检查两遍是否一致。
（实测 GCC 10.3 arm-none-eabi 今天**确实**给出正确布局：`sizeof==4`、`block` 在 bit 9、
`ce` 在 bit 20、`ch` 在 bit 22。问题不在今天对不对，而在**一旦不对，没有任何机制会发现**。）

**④ 它买不到「更快」，反而更慢。** 位域字段写入在 -O0 下是 read-modify-write
（实测 `ldrb r3,[r7,#4] / orr.w r3,r3,#3 / strb r3,[r7,#4]`），普通 `uint8_t` 字段是一条
`strb`。省下的 4 字节只作用在**永远位于栈上的临时量**上——全部 40 处 `nand_addr_t` 都是
局部变量或按值形参，**没有数组**（已 grep 确认），不省任何常驻内存。唯一真实收益是把
`pba_from_addr` / `addr_from_pba` 变成一次 4 字节 `memcpy`，而这两个是 `static inline`：
-O2 下整个消失，-O0 下各约 7 条移位/或，相对一次 µs 级 NAND 页操作可忽略。
按值传也不构成理由：`ftl_sync_write_page(nand_addr_t addr, …)`（`ftl_fc_interface.h:209`）
**今天就是按值传**。

**⑤ 如果真要「一个 DWORD 的具名字段地址」，那个东西已经存在** —— 就是 `nand_pba_t` +
`pba_to_*` / `pba_with_*`（26 个访问器）。`nand_addr_t` 的定位本来就是它的**展开**形式
（`ftl_types.h:21` 原文「展开」），两者是刻意的分工：遍历路径要字段直取就用展开形式，
要紧凑 32-bit 就用 `nand_pba_t`。

> 附：`nand_addr_t` 实际是 **10 字节**（不是 8）——`block` 是第 4 个字段（`uint16_t`），
> 前面 ch/ce/lun 占 0/1/2，故 offset 3 有 1 字节填充，9 向上对齐到 10。不影响任何结论
> （仍是栈上临时量、仍无数组），但记录准确数字。

## 验证

```bash
make clean && make debug    # 0 warning 0 error，150028 B（基线 149920，+108）
make clean && make release  # 0 warning 0 error， 84944 B（基线  84988， −44）
make clean && make debug CFLAGS_EXTRA=-DCONFIG_FC_RAID=1   # 0/0，154372 B
make format-check           # Format check passed.
```

`nm`：`paa_from_addr` / `pba_from_addr` 只以 **local（`t`）** 符号出现（`static inline` 未引入
外部链接符号）；`paa_make` / `pba_make` / `paa_is_valid` **全局消失**。

**离线等价性自证（本轮唯一的强判据）**：把 v24 的旧 `paa_make` / `pba_make` / `pba_to_paa` /
`paa_to_pba` / `paa_is_valid` 抄成参照实现，与新实现逐值比对：

| 判据 | 组合数 | mismatch |
|---|---|---|
| ① `paa_from_addr(&addr_from_paa(ch, ce, paa)) == paa` | 1,048,576 | 0 |
| ② `pba_from_addr(&addr_from_pba(pba)) == pba` | 1,048,576 | 0 |
| ③ 新 `pba_to_paa` vs 旧实现 | 1,048,576 | 0 |
| ④ 新 `paa_to_pba` vs 旧实现 | 1,048,576 | 0 |
| ⑤ `paa_is_valid_addr` vs 旧 `paa_is_valid` | 1,048,576 | 0 |
| ⑥ `addr_from_paa/pba` 往返（字段级比较） | 1,048,576 | 0 |
| ⑦ `paa_to_nand_row` 不变 | 1,048,576 | 0 |
| ⑧ `paa_with_plane(paa_page_base(x), plane)` vs 旧 `paa_make(...)`（`fc.c:1104`） | 2,097,152 | 0 |

（踩过的坑：第一版用 `memcmp(&a, &r, sizeof(a))` 比较结构体，`nand_addr_t` 有填充字节、
值未定义 ⇒ 报 917,504 处 mismatch。改为**逐字段比较**后 0 mismatch。）

**上板未验**。`make tests` 本轮未跑（见文首）。

## 风险

| 风险 | 规避 |
|---|---|
| 漏改调用点 | 正是本轮的目的：`paa_make` / `pba_make` 删除后**编译报错**，不会静默 |
| 复合字面量窄化（`.block = some_uint32`） | 本仓**未开 `-Wconversion`**（`config.mk:96` 只有 `-Wall -Wextra`），不告警；值域由现有 ASSERT 覆盖 |
| 复合字面量在 -O0 下的栈开销 | 每处仅一个 10 字节临时量；旧 7 形参版在 -O0 下同样溢栈（4 寄存器 + 3 栈字），实测无回归（debug +108 B 是 13 处合计） |
| typedef 前移破坏头文件顺序 | 只在 `nand_config.h` 内部移动；已验证新顺序成立 |
| `addr_to_cmd` 换写法后语义 | 逐字段相同；`:136` 注释本来就这么描述 |

## 未跟进

- `paa_same_page` / `pba_same_page` / `pba_is_valid` 等其余 2 形参函数不动。
- **历史文档不重写**：`docs/design/ftl_revision_history.md`、`docs/design/ftl_design.md` 的
  版本历史行与示意伪代码、以及既有 changelog（`fc_scrambler_lba_seed_v24.md`、
  `fc_per_plane_row_address.md`、`fc_ut_level2_rework_v22.md`）保留 `paa_make` / `pba_make`
  原名——它们是**当时那一轮的记录**，不是当前接口。当前接口以
  `fc_nand_design.md` §6.2 / §11.2 为准（本轮已更新）。
- 测试源文件的同步由用户负责（构建外的两个已机械改掉，见「范围」）。

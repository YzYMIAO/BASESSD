> 常驻一句话规则在 `.claude/rules/c-coding.md`；本文件是细则 / why / 反例，按需读。
> 通用 15 章见 [coding_rules_generic.md](coding_rules_generic.md)。

# C 编码规则细则（本项目 H1~H53）

三层关系：

- `.claude/rules/c-coding.md`：触碰 `src/` / `include/` / `tests/` 时自动加载的**一句话规则**。
- 本文件（docs/guides/coding_rules.md）：每条规则的 **why / 反例 / 落点**，需要解释或做取舍时读。
- [coding_rules_generic.md](coding_rules_generic.md)：通用 15 章 + 本仓不采纳理由（L3，仅深审 / 争议时读）。

> 本文件不复制通用条款，只写本仓改写过的 H 规则。规则正文以
> `.claude/rules/c-coding.md` 为准；两处不一致时按那条修。

## 资源、错误与断言

### H1 资源申请与释放必须配对

三池：命令 / 描述符 / 缓冲。op release 归属：`SYNC_WAIT` 不 release，其余由发起者 release；
buffer 看 `buf_own`。漏配对的症状是**池泄漏 → 全系统 `FTL_AGAIN`，且不报错**。
收尾时对每条申请问「谁释放、哪条路径漏了」。

### H2 参数合法性检查只在模块入口做一次

边界 = host 入口返回码 + FTL↔FC / 子模块入口 `ASSERT`。模块内部逐函数复检同一入参是纯开销。
判据见 `docs/design/common/assertion_guidelines.md §九`。配 C5 / H14。

### H3 整数运算必须考虑溢出

反例：`SUPER_BLOCK_LIST_MAX` 的 1024 是**估值**不是上界，写满约 264 MiB 必溢出，
运行时静默丢条目 = 永久漏账。判据：**先判后算** ——
`if (a > SIZE_MAX - b) 报错; else c = a + b;`。无符号回绕不是 UB 但同为缺陷；
有符号溢出是 UB，事后检测会被优化掉。

### H4 禁止魔鬼数字

尺寸 / 容量常量用宏，**必须注明来源与推导**。把「估值」写成「上界」是本仓已发生的缺陷。

### H5 废弃代码及时清除

注释掉的代码、死函数、过时守卫，收口时删干净。删掉的函数 / 字段不留墓碑注释。

### H6 改代码时同步维护周边注释

注释与代码不一致 = 缺陷。改语义 / 默认值 / 调用约束时，同步改函数契约、字段注释、文件头。

### H7 `switch` 必须有 `default`

非法 `phase` 的 `default` 带 release / 复位副作用是**有意设计**，见 `CLAUDE.md §四`，
不要改成空 `default`。

### H8 禁止 `goto`，唯一例外是统一错误出口

`src/fc/fc.c` 的 4 处 `goto stripe_error` 是合规的统一错误出口，见 `CLAUDE.md §四`。

### H14 关键处用 `ASSERT`

位置：不变量、前置条件、**不该发生**的状态（`switch default`、坐标越界、内部自铸句柄越界）。
机制：debug 检查 + `assertion_fail` 停机；release（`NDEBUG`）展开为 `((void)0)`。
⚠ **`NDEBUG` 下表达式不求值** ⇒ 有副作用的调用绝不能放进 `ASSERT`。
⚠ 后果是**静默介质破坏**时，除 `ASSERT` 外**必须**再补 release 运行期拒绝（判据 9）。
新增 `ASSERT` 必须能构造失败输入（C7）。

### H30 库函数 / HAL 返回值必须处理

允许显式 `(void)HAL_...`，但必须写成 `(void)`；「静默忽略」与「显式丢弃」的差别，
是下次读代码能否看出你考虑过。失败后果是静默介质破坏时，不能只丢弃，走 H14 release 拒绝。

## 类型、位运算与整数

### H9 固定宽度整数统一用项目别名

`BYTE / WORD / DWORD / QWORD`（`include/common/ssd_types.h`），不写 `uint8_t`；
不提供小写别名。**不设二级地址别名**：LBA / PAA / PBA 直接 `DWORD`。
第三方 / 厂商边界（`third_party/`、CMSIS/HAL）保持原生 `uint*_t`；跨边界按值传递即可。
用别名必须 `#include "ssd_types.h"`（裸名，H41）。

### H15 位运算与位判断必须走公共宏

判断要封装：状态机判断 / 值判断一律走宏，不手写 `(x & MASK) != 0U`。
**两种语义必须由宏名区分**：`(x & m) != 0` 是**任一位置位**；`(x & m) == m` 是**全部位置位**。
唯一通用掩码构造器 `NAND_FIELD_MASK(width)` 挂在 NAND 层；跨层复用是欠账 RISK-BIT-01，
新增位运算先找它。

### H22 隐式窄化转换必须显式化

宽类型赋给窄类型必须写显式转换，让「我知道这里会截断」变成读得出来的事。
反例：`expected_byte` 用 `(block << 8 | page << 4 | plane)` 后截断到 8 位 ⇒ `block`
参数完全不影响结果，调用处传 102 与写入用的 100 碰巧截断相同 ⇒ 测试碰巧通过。
H48 是其落地细则。

### H23 移位量必须小于类型位宽

`x << n` 中 `n >= 位宽` 是 UB；对有符号负数移位也是 UB。掩码用带 `U` 的无符号常量；
移位前先与位宽比一次（`ASSERT` 或 `_Static_assert`）。

### H31 数组下标与指针必须做边界判断

落点：位图是 **flat 绝对编号** —— plane 1 的 CW0 是 `bit 4` 不是 `bit 0`，
`1U << cw` 是 plane 盲位图。下标越界**不会崩溃**，只会**改错 plane 的位**。
配 H15 / H23。

## 嵌入式运行时

### H16 `volatile` 只用于三处

硬件寄存器 / ISR 与主循环共享变量 / DMA 可见缓冲区。⚠ 它不是同步手段：
不保证访问顺序（顺序敏感处配 `__DMB()`），也不保证原子性（多字节共享仍需临界区，H18）。

### H17 DMA 缓冲区必须做缓存维护

CPU→设备用 `SCB_CleanDCache_by_Addr`；设备→CPU 用 `SCB_InvalidateDCache_by_Addr`。
地址与长度按 32 字节对齐。范本：`src/platform/soc/stm32h743/usart_port.c`。
漏 Invalidate = **拿错数据继续跑**；该操作不免费，不要对同一缓冲区反复做。

### H18 临界区必须用可嵌套的 PRIMASK 保存 / 恢复

不用裸 `__disable_irq()` / `__enable_irq()`：裸开中断在嵌套时会让内层退出提前开中断，
外层临界区被击穿。正例：`usart_port.c`；反例：`sys_port.c`，新代码不要照抄。

### H19 递归必须有深度上界

本仓不禁止递归，禁止无界递归。深度必须由**数据结构**保证（树高 / log n），
不是输入决定的。当前 `src/common` 的排序 / 树实现已随死代码删除；新增递归时
在注释里写清深度上界来自哪个不变量。

### H20 看门狗：长操作必须喂狗

喂狗点不能被慢路径饿死；不在 ISR 喂狗（会抹掉「主循环卡死」信号）。
IWDG 当前所有构建档关闭（`SSD_WATCHDOG_ENABLE` 硬编码 0），启用与否是待上板决策
（RISK-CLI-01）。长同步操作走 `storage_wdg_kick()`（≤10 ms 节流）。

### H21 ISR 必须短、不阻塞、可重入

允许：置标志、递增计数、打时间戳、读写 GPIO/ADC（有界、可重入、不阻塞、栈有界）。
禁止：`LOG*`、`ASSERT`、动态分配、等待 / 轮询外设、FTL / NAND 事务。
重活延后到主循环：ISR 置「有待办」标志，主循环 / 工作队列执行。本仓当前无 ISR-safe API
子集，回调仍全放主循环。

## 命名、宏、变量、表达式

### H24 命名规范

`snake_case` 变量 / 函数、**宏全大写**、全局 `g_` 前缀、`static` 文件级 `s_` 前缀（弱约定）。
禁止拼音、禁止无意义单字符名（循环 `i/j/k` 除外）。

### H25 宏的写法

表达式宏**每个参数与整体都加括号**；多条语句必须 `do { ... } while (0)`；
**宏参数不得有副作用**（展开次数 ≠ 求值次数，与 H14 同一个坑）。

### H26 变量必须先初始化后使用

未初始化变量作右值是 UB 级缺陷；`.noinit` 段不清零，容易把「复位残留」误当「初值」。
初始化位置离使用越近越好。

### H27 通讯 / 持久化结构体必须显式约定字节序

落点：ONFI 参数页解析、NAND 元数据区结构。跨边界每个多字节字段注释写清字节序；
主机序↔小端只允许在**单一漏斗**发生。本仓无通用字节序 helper，只有 `__builtin_bswap32`。

### H28 表达式

不依赖求值顺序、禁止嵌套赋值、自增自减独立成句。`f(i++, i)`、`a = b = c`、
`x = arr[i++] + arr[i]` 都是未定义或未指定行为。

### H29 严禁使用未经验证的输入数据

数据来源是**介质 / 上位机 / CLI**，一律按不可信处理，边界必须校验。
这解释了 `src/ftl/journal.c` 为何**刻意不转断言**：回放内容来自 NAND 读回，坏数据是掉电的预期情形。

## 结构与模块

### H10 全局可变状态必须单点修改

嵌入式静态分配是常态（`g_ftl[]` 等），适用的是**唯一闸门**；
allocator 的 free / op / spare / closed_sb_list 四个账本（本仓简称「四本账」）教训即此条。

### H11 函数嵌套 ≤4 层

不设函数行数上限；行数不是拆分函数的判据，可读主干优先。
**提炼阈值**：同一段逻辑第 2 次出现就该提炼，第 3 次必须立刻提炼；
仅为对外 API 完整性保留的 helper 不算重复。

### H12 头文件职责单一

只放声明不放变量定义，必须自包含，有 `#ifndef/#define/#endif` 守卫，
**禁止 `#pragma once`**，禁止循环依赖。`ssd_types.h` 的自包含注释记录了必要性
（避免把 `stdio.h` 拉进 FTL include 闭包）。

### H13 模块化

一个模块只做一件事；模块间只通过接口通信，不碰对方内部数据。
**耦合过重的信号 = 改 A 模块被迫去改 B 的内部** ⇒ 该给 B 加接口，不是改 B。
accessor 过三问：① 检查值域 / 边界 / 状态转移？② 隐藏数组 / 位布局？③ 跨模块账本唯一读写点？
三者皆否 ⇒ 直接写 `s->field`。字段数量不是判据；薄包装（只转发 / 只 `(void)` 返回值）没有封装价值。
但若它是**跨模块访问的唯一入口**（防止调用者直接碰对方字段 / 数组 / 账本），
或它是**公共头的实现文件**（`*_interface.c`），则按 H49「模块边界」计。
accessor 三问之外再加一问：④ 是否阻止了跨模块直接操作？
**公共头（层对外）声明的函数一律带当前层前缀**（本层 `ftl_`）；内部模块保留模块前缀
（`pfr_` / `mig_` / ...）。把内部函数直接暴露到公共头前，先改名为层前缀。

## 状态、返回、守卫、声明、注释

### H32 状态字段用互斥枚举

不用软件位图。**一位两义即缺陷**；正交维度拆独立字段；转移收敛到唯一 setter；
已有同值域枚举时**不新建第二套**（如 `temp` 保持 `uint8_t` 并注明值域同 `FTL_TEMP_*`）。

### H33 布尔与多态返回码

项目布尔一律 `BOOL`（1 字节 C99 `_Bool`）；`bool/true/false` 只留 typedef、第三方边界、冻结测试源；
**不新增** `xxx_bool_t`。多态返回码拆**具名枚举**；**同值反义不得共用通用枚举**；
值域不是真值域的不改 `BOOL`（计数 / 档位 / 下标 / 操作码 / 温度码）。
**依据**：本仓 `-Wextra -Werror` 下 `-Wenum-conversion` 生效——不同域的枚举即使同值也必须分开，
否则失去方向混用的编译期保护（`arm-none-eabi-gcc 10.3.1` 已实证）。

### H34 入口守卫的收口判据

调用者自己持有的指针 / 自己算出的索引 ⇒ 转 `ASSERT`；host 输入 / 介质读回 / 池空 / 表满 ⇒
保留返回码。`static inline` getter/setter 同样适用。⚠ 「热路径」不是保留 `if` 的理由：
`if` 在 release 是活分支，`ASSERT` 裁成 `((void)0)` ⇒ release 净收益。
被 `ASSERT` 守的形参 release 变 unused 时，先判是否废形参；不要用 `(void)param` 糊。

### H35 变量声明就近使用处

判据 = **首次使用的那一行声明**，不提到函数 / 块开头。刻意偏离「声明集中块首」，按 P5 留痕。

### H36 命名即语义：字段名不得撒谎

字段名与语义相反 ⇒ **改名**，不靠谓词 / 注释兜；为兜坏名字而存在的谓词，改名后就是纯仪式，应删。

### H37 注释只描述当前设计

不承载变更史。删版本号 / 日期 / 计划代号 / 「原先是…」/ 历史复盘；变更史归 git /
`docs/archive/changelog/`。删叙事时**不得丢信息**：改写成「现在是什么、为什么必须这样」。
已删除的函数 / 字段**不留墓碑注释**。

### H38 返回值与数据分离

状态用 `ftl_status_t` / `bool`，**数据只走出参**；不要用「合法值 + 哨兵」兼职状态。
返回值恒为某常量、或唯一调用点恒丢弃 ⇒ 改 `void`。
`FTL_ERROR` = **永久失败**；`FTL_AGAIN` = **瞬态资源**（池空 / 背压）。

### H39 维度常量不得写死下标

凡由 `*_NUM` 定义的维度（`PLANE_NUM` / `LUN_NUM` …），初始化、复制、比较、日志一律 `for` 循环，
不写 `[0]/[1]` 字面量。收尾 `grep -rn 'plane_blocks\[[0-9]\]' src/ include/` 应为 0。

### H40 枚举 / 值域参数入口断言

枚举型 / 有界值参数在模块入口 `ASSERT(x < X_NUM)`；`*_NUM` 宏用于边界与遍历。
**零消费者 = 死宏**（H5），要么用于断言，要么删（实例：`FTL_TEMP_NUM`）。

## 头文件、内联、强转、实参、状态码、初始化

### H41 项目头文件一律纯文件名

禁止目录前缀与相对路径：`#include "metadata.h"`，不写 `"ftl/metadata.h"`、`"../..."`、`"./..."`。
项目头用 `""`，系统头用 `<>`；厂商 CMSIS/HAL 沿用既有写法。
裸名由各模块 `makefile.mk` 的 `MODULE_INCS` 解析；新增头文件目录必须同步加入。
**同名风险**：裸名依赖 `-I` 顺序，新增前必须 `find include tests -name '<name>.h'` 查重。
机械检查：`grep -rnE '^\s*#\s*include\s*"[^"]*/' src include tests` 应为 0。

### H42 函数式宏 / `static inline` / 普通函数的分层

**按「契约 / 求值次数 / 作用域」三件事分层，不按行数。**

1. **`.c` 普通函数**：有返回码；有副作用（I/O、日志、回调、状态写）；非平凡逻辑。
2. **宏（编译期）**：编译期常量表达式 / 数组维度；token paste / `__FILE__`；`_Generic` 类型泛型；寄存器访问。
3. **有契约 → `static inline` 或 `static` 函数，禁止宏**：
   - 跨 TU、纯、≤5 NBNC → 放模块头 `static inline`；
   - 文件内、或用 `ASSERT` 守调用者自持值、或访问文件内 `static` 数组 / 不透明句柄 → `.c` `static` 函数。
   - 契约 = 一个 `ASSERT` / 边界检查 / 需要原型类型检查。
4. **无契约纯表达式 → 允许函数式宏**，但必须同时满足：
   a. 无副作用、无 I/O、无日志、无回调；
   b. 每个参数**无条件出现且仅出现一次**（不能藏在 `&&` / `||` / `?:` / `if` 的短路分支）；
   c. 宏体**只引用形参与大写编译期常量**，不引用文件级变量 / 函数；
   d. 全括号（H25）、全大写命名（H24）、只放本 `.c` 顶部；
   e. 单调用点优先并回调用点（H49），不新增宏。

`static inline` 不是内联保证（两档均 -O2）；不为「强制内联」引入 `always_inline`，除非有正确性依据或代表负载的测量。

**为什么 b 要「无条件」**：PRE31-C 把「求值 >1 次或 0 次」都算 unsafe macro；只要求"出现一次"仍可能因短路而 0 次求值，副作用被静默跳过。
**为什么 c 要「只碰参数和大写常量」**：PRE00-C 的作用域绑定问题——宏体里的标识符在调用点解析，可能被同名局部变量劫持。

**偏离留痕（P5）**：允许运行期纯表达式用宏，偏离 CERT C PRE00-C（Recommendation）/ MISRA C:2012 Dir 4.9（Advisory）。理由：本仓 `.c` 纯算术小封装较多，宏化减少函数定义、提高主干可读性；护栏 a~c 已结构性排除多求值与作用域绑定风险。代价：放弃原型类型检查与可断点；-O2 下无代码生成收益。

反例：
- ❌ `#define ABS(x) (((x) < 0) ? -(x) : (x))` —— `x` 两次，`ABS(++n)` 双求值（PRE31-C）。
- ❌ `#define IS_BP(c) ((c) == GATE || (c) == SUBMIT)` —— `c` 两次；单次也落在 `||` 里（护栏 b）。
- ❌ `#define SLOT(r) ((r) ^ (((r) >> 8) & MASK))` —— `r` 两次；应写成 `static` 函数。
- ✅ `#define L2P_CMT_SET(lba) ((WORD)((lba) & (L2P_CMT_SETS - 1U)))` —— 参数一次、只碰大写常量、纯表达式。

### H43 显式强转只用于有信息损失的转换

保留：窄化；有符号 / 无符号变化；宽度变化（含 printf 可变参数 ABI）；指针 / 位域 / union
reinterpret；`(void)` 显式丢弃。删除：同类型转同类型（`(uint32_t)some_uint32`）、
字面量 / 常量转其固有类型（`(uint32_t)0U`）、纯装饰性强转。
⚠ 本工具链 `DWORD` ≡ `unsigned long` ⇒ 配 `%lu` 的 `(unsigned long)` 是**同型强转，必须删**；
`uint64_t` 用 `%llu`（完整 newlib 支持）；不要再配 `%lu` 做窄化。

### H44 强转先查被调方

同一 API 的返回值在**多个调用点**被反复窄化 ⇒ 先改 API 返回类型，不是逐个调用点补强转。
顺序：有自然值域（位域宽度 / 枚举上界 / 索引范围）⇒ 用能装下它的**最窄标准类型**，
窄化收敛到生产者内部一次；无自然值域（计数 / 容量 / 地址 / 坐标）⇒ 保持宽类型。
三方向：同型转 ⇒ 删；加宽强转通常无损失 ⇒ 删（例外：printf ABI、刻意加宽避免溢出并注释原因）；
窄化在 N 处重复 ⇒ 改 API 返回类型。printf 格式一律用与实参类型匹配的长度修饰符：
`uint8_t`/`BYTE` → `%hhu`、`%hhx`/`%hhX`；`uint16_t`/`WORD` → `%hu`、`%hx`/`%hX`；
`DWORD`/`uint32_t` → `%lu`、`%lx`/`%lX`；`uint64_t`/`QWORD` → `%llu`、`%llx`/`%llX`；
`bool`/枚举 → `%d`；位域按位宽（≤8 → `hh`，9~16 → `h`，17~32 → `l`）。
实参本身是 `unsigned int` ⇒ 直接用 `%u`，不要用 `(unsigned)` 迁就 `%u`。

### H45 函数实参不得承载「第二段逻辑」

先落语义化局部变量，再调用。判据：实参本身需要读者再解析一次 —— 结构体字面量、指针差、
嵌套调用、下标 / 解引用、三元 / 逻辑式。
是 ⇒ 先在调用点之前建局部变量，再传名字。零运行期收益，只影响可读性。

### H46 返回值状态码一律非负

0 = 成功 / 默认，其余为具名非负码；禁止 `-1/-2`。按**具名常量**比较，禁止 `status < 0`。
已有 0 语义的步进枚举不重排；新增错误码取下一个未用正数。
例外（不是状态码）：libc `_write()` 必须返回 `int`、失败 `-1`；比较器 `-1/0/1`；
BCH `deg(0) = -1` 等局部算术；`third_party/`、CMSIS/HAL、munit 保持上游。

### H47 局部结构体初始化

一律「先声明、后逐字段赋值」；初始化列表只用于**数组表**与**文件级 / 全局 / static** 结构体。
禁止局部 `const T x = {.kind = ...}`、赋值式复合字面量、局部 `T x = {0}`（改 `memset`）。
理由：初始化列表静默补零未写字段，读者看不出「刻意 0」还是「漏了」。

### H48 强转最小化

优先用「声明宽度 / 有界值 / 赋值转换」消除强转。消除顺序：
① **改声明**（局部循环变量 / 偏移量 / 环形索引直接声明 `DWORD` / `WORD`）；
② **有界值可隐式截断**（枚举常量、`x % N`、`x & MASK`、已做范围检查的值）；
③ **外层 cast 冗余则删**。必须保留：指针↔整数；printf ABI；移位 / 乘法**之前**的加宽
（`(DWORD)id[1] << 24`、`(QWORD)x * 1000`）；API 边界符号转换；不可信输入的窄化（H22 / H29）。

### H49 模块优化顺序与命名

固定：**命名 → 布局 → 调用链 → 封装 / 断言**。不得先堆小函数再改名。
封装必须换来**复用、守卫或模块边界**之一，皆无 ⇒ 并回调用点或唯一漏斗；行数不是拆分判据。
**调用点可读性优先**：包装若只是单表达式地给已有漏斗补参数（`pba_make` / `pba_to_paa`），
即使多处调用也优先并回调用点——展开后调用点直接可见漏斗参数，包装名改为注释。
复用 / 守卫 / 模块边界仍适用于多语句或有独立语义的封装。

#### 命名格式

`<module>_<resource|sub_module>[_<state>]_<action>[_<detail>]`

- **module**：公共 API = `ftl_`；内部 = 文件模块前缀（H50 表；`cache` = `cache_batch` 缩写）。
- **resource / sub_module**：操作对象（`block` / `page` / `cmt` / ...）**或**功能子模块
  （`batch` / `seq` / `ckpt_record` / ...），二者并列；缩写查 H50 表。
- **action**：resource/sub_module 后**第一个动词 token**；其后是 state/detail。
- 不用裸 `try_`，不用 `_impl` / `_internal` 后缀。

#### 动作词表

- 通用动作：`get/set/find/add/remove/push/pop/alloc/free/release/refill/close/open/rewind/reset/note/record/scan/register/eval/build/take/finish/kick/start/read/write/flush/commit/dispatch/drain/gate/select/progress/tick/stall/wait/yield/cleanup/abort/store/erase/reap/dump/prepare/request/issue/append/freeze/reserve/resume/update/plan/replay/apply/claim/install/touch/merge/invalidate/drop/lookup/decode/detach/evict/settle/park/snapshot/finalize/preload/prefetch/pump/print/collect/locate/advance/probe/copy/restore/retry/rotate/rebuild/fill/load/assemble/check/format/execute/carve/solve/verify/report/materialize/bind/mark/next`
- 入口角色（H51 收尾）：`init/service/handle/step/submit/begin/run`
- 回调角色：`op_callback` / `handle_callback`
- 谓词前缀：`is` / `has`

**双用词**：`read` / `write` / `flush` 后面还有动词时是子模块，否则是动作。
`mig_read_submit` → sub=`read`, action=`submit`；`l2p_read` → action=`read`。

**语义区分**：`take` = 读出并清位 / 租约，`pop` = FIFO；`start` = 启动 job/序列，
`kick` = 触发一次 flush/host kick；`note` = 轻量记账，`record` = Trace/日志落盘；
`finish` = job/group 收尾（`complete` 不再作动作词）；`eval` = 周期求值；`build` = 构造值/结构；
`solve` = 数学方程/多项式求根（保留算法语义，如 `fc_bch_solve_cubic`）。

#### 值、谓词与回调

- **值**：可变数据 → `get`/`set`/`clear`；只读派生值（计数/名字/容量）→ 值名词结尾，不加动作；
  对象/句柄 → `_get`/`_find`。
- **谓词**：`is_<具体条件>` / `has_<具体条件>`；条件必须具体
  （`is_valid` / `is_sufficient` / `is_ready` / `is_due` / `is_blank` / ...），不用 `is_ok`。
- **完成回调**：`*_op_callback`（op 级注册项）/ `*_handle_callback`（单命令完成处理）。
- **测试入口**：`*_test_*`。

### H50 类型命名

复合类型 / ID / 函数指针 / 数组的类型别名统一定义、统一后缀、统一缩写。
定义处带 `typedef` + tag：`typedef struct FC_COMMAND_S { ... } fc_cmd_s;`
tag 全大写 `_S/_E/_U`，alias 全小写 `_s/_e/_u`，与 tag 不同名。
派生后缀：`_id_t`（资源 ID）、`_fn_t`（函数指针）、`_arr_t`（数组）、`_t`（其他有语义标量）。
统一用 `cfg`，不混 `conf`。新增缩写先查此表：

| 全称 | 缩写 |
|---|---|
| command / configuration / descriptor / parameter | `cmd` / `cfg` / `desc` / `param` |
| buffer / context / topology / metadata | `buf` / `ctx` / `topo` / `md` |
| journal / statistics / address / length | `jrn` / `stats` / `addr` / `len` |
| count / index / information / operation | `cnt` / `idx` / `info` / `op` |
| request / response / segment / header | `req` / `rsp` / `seg` / `hdr` |
| migrate / pending / allocator / scheduler | `mig` / `pend` / **`alc`** / `sched`（`alloc` 只作动作词） |
| recovery / read recovery / watchdog / wear leveling | `rec` / `rr` / `wdg` / `wl` |
| controller | `ctl` |

**FTL 域缩写**（模块 / 子模块 / 资源）：

| 全称 | 缩写 |
|---|---|
| L2P cache | `l2p` |
| metadata | `md` |
| migrate | `mig` |
| journal | `jrn` |
| checkpoint | `cp` |
| power failure recovery | `pfr` |
| bad block table | `bbt` |
| allocator | `alc`（动作 `alloc` 不混用） |
| pending | `pend` |
| write buffer | `wb` |
| read recovery | `rr` |
| background | `bg` |
| cache_batch | `cache` |
| cached mapping table | `cmt` |
| directory | `dir` |
| checkpoint record | `ckpt` |
| destination | `dst` |
| sequence | `seq` |
| **write** | **`write`（不用 `wr`，与 `read` 对齐）** |
| workspace | `workspace`（保留全称） |

**缩写规则**：命名段必须查表；新缩写**先在表里加一行**再用，不在代码里临时造；
同一全称只允许一个缩写；长名只要有信息量就不强行缩短。

`make types-check` 是正向不变量检查。

统一入口为 `tools/check_c.py`（`make c-rules-check` = H41 / H50 / H51 / H52；
`types-check` / `layout-check` / `headers-check` / `check-pairs` 暂保留为兼容目标）。
H53 不机检（判据依赖链接图，文本检查器拿不到编译产物）。

### H51 文件内布局（.c / .h）

目标是「先看定义，再看实现，最后看主入口」。

`.c`：

```text
1. 文件头注释
2. #include
3. 宏 / 常量
4. 类型：enum / struct / union / typedef
5. 文件级变量：g_* / s_* / static const 表
6. static 函数前向声明（含最多一行“注册表自身声明”）
7. 实现函数：按名称前缀分节，节间 / 节内按依赖顺序
8. 注册 / 分发表：函数指针表 / 命令表 / 任务表 / 设备描述表
9. 模块主函数 / 几大入口：
   *_init / *_service / *_handle / *_step / *_submit / *_begin / *_run / main
```

`.h`：

```text
1. include guard
2. 文件头注释
3. #include（系统 <...> → 项目 "..."）
4. 宏 / 常量
5. 类型：enum / struct / union / typedef
6. extern 声明（只声明，不定义变量）
7. static inline 短 fast path（纯函数，≤5 NBNC，H42）
8. 函数原型：按名称前缀（资源 / 子模块）分组；查询 / 谓词 / getter 在前，
   *_init / *_service / *_handle / *_step / *_submit / *_begin / *_run 在后
9. #endif
```

- **文件级**宏、类型、变量必须在**第一个函数定义 / 原型之前**；不夹在后面。
  函数内局部变量/局部类型仍按 H35 就近声明。
- **注册表例外**：函数指针表 / 命令表 / 任务表 / 设备描述表若以本文件函数为初始化项，
  放在第 9 段（被引用函数之后、主入口组之前）；也可放文件最末作为数据附录，
  此时 `main` 仍是最后一个函数。不要为表前置声明整批 handler 原型；表若被更早函数引用，
  只在第 6 段保留**一条表自身声明**（例：
  `static const bg_task_s s_bg_tasks[FTL_BG_JOB_NUM];`）。
  普通 `static const` 常量表（不含本文件函数）仍算第 5 段变量。
- **主入口即使很短也放最后**（入口组见上）。
- **前向声明默认消除**：`.c` 先按**名称前缀**分节、前缀组间按依赖排序；组内按依赖排序。
  前向声明只允许：① **前缀组之间成环（互调）——每个环取声明少的方向；不得为 0 声明破坏前缀分组**；
  ② 组内互递归。每条旁写明理由；能靠移动函数 / 换节序消除的必须消除。
- **文件头写模块职责 / 关键机制 / 与其他模块的联动；不写节目录**
  （符号列表由 IDE 提供，Source Insight 的 Symbol Name 已覆盖）。
- `.h` 只放声明（H12）；变量定义、普通函数定义禁止；只有 `static inline` 快路径例外（H42）。
- 模块头声明**本模块自己的接口**；umbrella 头只 `#include` 模块头，不复制原型。
- **普通** `static const` 表算变量，放第 5 段；不要夹在函数中间。
  以本文件函数为初始化项的注册 / 分发表按上面的例外放第 9 段（或文件最末）。
- 条件编译：定义放对应顶部段；测试 / 统计 / 调试优先单独头/源文件，不塞回 umbrella/core。
- 与 H49 分工：H49 管「优化/重构时的命名→布局→调用链→封装」顺序；
  H51 管**文件内物理布局**。H51 不设函数行数上限（H11）。

反例：`.c` 中 `s_write_retry_logged` / `FTL_WRITE_RETRY_LOG_EVERY` 出现在重试函数附近，
`s_cache_pad_page` 出现在 padding 函数中间；`.h` 中宏/类型夹在原型之后，或模块头只有类型、
函数原型全堆在 umbrella 里。定义应上移；原型应回到模块头。

### H52 模块文件成对（.c / .h）

目标是「模块边界可发现、声明只有一个权威位置」：改一个模块，先看同名 `.h`；不存在“一头多实现”或“无头实现文件”。

**默认规则（1:1）**

- 每个模块 `.c` 必须有同名 `.h`；同名按 **basename** 判定，可跨目录：
  `src/ftl/foo.c` ↔ `src/ftl/foo.h` 或 `include/ftl/foo.h`。
- 该 `.c` 的**全部跨 TU 符号**只在其同名 `.h` 声明。
- 一个内部 `.h` 也只能对应一个 `.c`：一个头的原型不得分散在多个 `.c` 实现。
- 公共契约头 `include/<layer>/*.h` 是库表面，允许由多个 `.c` 实现（不强制 1:1）。

**例外（必须机检，文件头前 40 行标注）**

| 标记 | 适用 | 说明 |
|---|---|---|
| `header-only: config` | `*_config.h` | 层次配置 / 调参头，无 `.c` |
| `header-only: contract` | `include/<layer>/*_type.h` / `*_geometry.h` / `*_capacity.h` | 公共类型 / 几何 / 容量契约头 |
| `header-only: inline-access` | 如 `bst.h` | 只含 `static inline` 访问器的内部头 |
| `implements <header.h>` | 平台端口 `*_port.c` | 实现另一个接口头（`delay_port.c` → `delay.h`） |
| `internal-only` | 无跨 TU 导出的拆分 `.c` | 本仓不鼓励，新增需评审理由 |
| `multi-impl: <理由>` | 注册 / 分发表头（如 `ftl_op.h`） | 一头声明多个模块的完成服务函数，属有意例外 |

`main.c` 与启动文件不适用；`tests/`、`third_party/` 不扫描。

**检查**

- `make check-pairs`（或 `make c-rules-check` 全量）跑 `tools/check_c.py --rule H52`；
- `make check-pairs-test` 跑正 / 负例自检（缺头、缺 `header-only` 标记、一头多实现、`implements` 目标缺失等）；
- 扫描范围：`src/ftl` / `src/fc` / `src/nand` / `src/platform` / `src/hil`；扩到新层时先把该层例外标记补齐再挂进 `make check`。

**反例**

- 一个模块头声明的函数由两个 `.c` 实现（例如主实现 + 拆分文件）⇒ 改模块要先找两个 `.c`；
- 模块 `.c` 没有同名头，声明散落在 umbrella 头或其他模块头 ⇒ 无法判断谁是权威；
- 为了配 1:1，给纯类型 / 纯 inline 头建空 `.c` ⇒ 与 H42（fast path 留在头）冲突，且增加无意义文件。

**与主流的关系**

本规则是**本仓自定约定**，不冒充行业标准：主流项目常见模块 `.c/.h` 配对，但也普遍存在多个 `.c` 共用一个内部头、无头 `.c`（例如 Linux 内核）。本仓用它换取 FTL 层模块边界与声明归属的可机械审计；代价是新增文件时多一次配对判断。

### H53 内部函数的 static 与单测可见性（条件链接）

目标是「生产代码不为测试牺牲封装」：只看 `.c` 内部调用的函数一律 `static`，但单测要直接构造内部状态 / 覆盖分支时仍能调到。两种坏做法都不接受：

- 为测试保留**无条件外部链接** ⇒ 内部实现成为整层的隐式 API，改它就要怕别处调用；
- 把函数体搬进头文件做 `static inline` ⇒ 违反 H42（有逻辑的实体该在 `.c`）。

**做法：按模块定义一个「条件链接宏」**，产品档展开为 `static`，测试构建展开为空。

```c
/* src/fc/fc_data_path.h —— 宏放在文件头之后、原型之前 */
#if defined(FC_NAND_TEST_ENABLE)
#    define FC_DATA_PATH_STEP_LINKAGE
#else
#    define FC_DATA_PATH_STEP_LINKAGE static
#endif

#if defined(FC_NAND_TEST_ENABLE)
FC_DATA_PATH_STEP_LINKAGE fc_status_e fc_data_path_ecc_decode(BYTE *cw, BYTE *ecc_bits);
#endif /* 原型与定义用同一宏；产品档 .h 里不出现这个原型 */
```

```c
/* src/fc/fc_data_path.c */
FC_DATA_PATH_STEP_LINKAGE fc_status_e fc_data_path_ecc_decode(BYTE *cw, BYTE *ecc_bits)
{
    ...
}
```

**判据**

- **只有「单测必须直接调用」才用**。能经公共入口覆盖、且不丢介质格式语义（字段放置位置、步骤顺序）的，保持无条件 `static`；为了让测试少建一次链而开 seam 不算理由。
- **宏名带模块前缀**（`FC_DATA_PATH_STEP_LINKAGE` / `FC_UOP_STEP_LINKAGE`），**禁用通用 `STATIC`**。通用名会与第三方头冲突：Zephyr 曾因 `STATIC` 宏与外部库同名而改为 `ZTESTABLE_STATIC`（PR #104385）。
- **`.h` 原型与 `.c` 定义用同一宏门控**。产品档头里不出现该原型；测试构建下两处都导出。漏一处会在测试构建报错，可接受。
- **`#if` 内只切链接属性**，不夹带行为分支。测试专用行为（hook / 计数 / 跳过硬件）走既有 `FC_NAND_TEST_ENABLE` 块，用普通 `#if`，不要塞进链接宏。
- **seam 只进模块自己的 `src/<layer>/*.h`**，不进 `include/<layer>/` 公共契约头。公共头是库表面（H12 / S5.1），不因测试扩宽。
- 不以 seam 代替 `--wrap`：`make tests` 已对整函数打桩的用 `-Wl,--wrap=`（见 `tests/makefile.mk`），seam 只解决「要调 static 原语」这一类。

**当前实例**

| 宏 | 位置 | 导出的符号 | 为什么不能走公共入口 |
|---|---|---|---|
| `FC_DATA_PATH_STEP_LINKAGE` | `src/fc/fc_data_path.h` | `fc_data_path_meta_write` / `crc_write` / `scramble` / `ecc_encode` / `ecc_decode` / `crc_check` | 单测要验证 META[0,4) / CRC[4,8) / USER[8,520) / ECC[520,528) 的**字段放置语义**；走 `fc_data_path_encode` 只能看到「成功」，看不出写在哪 |
| `FC_UOP_STEP_LINKAGE` | `src/fc/fc_nand_engine.h` | `fc_uop_engine_step` | 单测要手工构造引擎状态，覆盖 `JUMP_IF_NOT_READY` 回跳守卫与 release 下溢 |

**反例**

- 函数只在本 TU 调用，却因为 UT 要调而保持外链 ⇒ 应 H53 seam。
- `#define STATIC static` 这种全局宏 ⇒ 撞名 + 隐式改写所有声明。
- `#if defined(TEST) void foo(void); #else ...` 把整个函数体复制成两份 ⇒ 链接宏只该改 `static` 这个词。
- 把 seam 原型放进 `include/fc/fc_interface.h` ⇒ 公共契约被测试需求污染。

**与主流的关系**

形态出处是 Zephyr 的 `ZTESTABLE_STATIC`（`zephyr/sys/util.h`）：*"Define symbol as static unless we are building with ZTEST. Use this for private symbols which need to be accessible from tests."* 本仓按模块拆宏（一个模块一个，不共用一个全局 `STATIC`），因为 C 预处理器宏无命名空间，全局名容易和 HAL / 第三方头冲突。这是**有出处的主流形态 + 本仓命名约束**，不是纯自拟。

## 注释风格补充（S5 / S6）

- 函数契约 = `/** ... */` + `@brief` / `@param` / `@return` / `@note`。
- `@param` / `@return` **只允许出现在 `/** */` 块内**；多返回值一行一值；`@param` 一形参一行。
- 契约块必须**紧贴其函数**，标题等于紧随函数名；标题 ≠ 函数名即缺陷。
- 实现细节用 `//`，只写 why / 不变量 / 时序 / 失败后果。
- 当前不接 Doxyfile；`/** */` 的收益是形状统一、可被工具抽取。

### S5.1 注释深度按可见性分档

函数注释的详细程度由**函数的可见性**决定，不按文件作者喜好决定：

| 档 | 范围 | 注释要求 |
|---|---|---|
| 公共接口 | `include/<layer>/*.h`（层对外头） | 每个导出函数 / 类型 / 宏**完整契约**；这一层若独立成库，调用者只看头 |
| inline 实现 | 任意 `.h` 中的 `static inline` | 实现处**完整契约**（头内就是唯一实现） |
| 内部跨模块声明 | `src/<layer>/*.h` 中声明、定义在 `.c` 的原型 | 只写**一行 `@brief`**；`@param` / `@return` 放 `.c` 定义处 |

**保留项**：内部头即使不写逐参数契约，以下内容仍必须留在 `.h`——类型、宏、不变量、资源归属
（谁分配 / 谁 release）、前置条件、跨模块调用约束（例如“必须在某状态 / 某锁下调用”）。
这些是调用者必须知道的共享表示；可以写成文件级注释或原型上的一行 `@note`，不能只藏在 `.c`。

**反例**：
- 内部头把 `.c` 定义的 `@param` / `@return` 整段复制 → 两份合同漂移，改一处必漏一处。
- 公共接口头缺 `@param` → 库的使用者无法只看头完成调用。
- 内部头以“没注释”为由连**前置条件 / 归属**也省略 → 调用者误用，缺陷不在定义处暴露。

**与命名 / 布局的关系**：N1/N2/N3 命名规则不分档，公共头与内部头一致适用；
H51 `.h` 原型顺序（查询 / 谓词 / getter 在前，`*_init` / `*_handle` / `*_submit` 等入口在后）
也不分档。

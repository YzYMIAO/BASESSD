# CLAUDE.md — BASESSD 项目规则

> 本文件只放**常驻判据**（每次会话都需要的“必须 / 禁止”），目标 ≤ 200 行。
> C 编码规则 → `.claude/rules/c-coding.md`（触碰代码路径自动加载）；细则见 `docs/guides/coding_rules.md`；
> 领域约束 → `.claude/rules/{ftl,fc-nand,platform,tests,docs}.md`；知识 / 契约 → `docs/`。
> 条目编号可寻址（review 直接说「违反 R2」）。返工大多来自**方向判断错** ⇒ §〇 / 判据 C / §三 最重要。

STM32H743 + MT29F4G08 软 SSD 固件（FC-NAND 层 + FTL 层）。

## 〇、项目目的 —— 第一判据

学习存储主控（SSD controller）设计，代码向主控方向靠。

**动手前先问：「主流主控怎么实现这件事？」说得出主流做法和出处再谈改动。**
判据 = **有出处的主流设计**，不是 Linux，也不是本仓现状。

**出处层**（说「主流 / 行业标准」必须从这里引）：
- `docs/reference/README.md`（资料索引与逐份分析；PDF/HTML 原文不随仓库分发）
- `docs/design/storage_controller_design.md`（设计要点 + 附录参考资料）
- SPDK FTL 官方文档 <https://spdk.io/doc/ftl.html>
⚠ 说不出出处只能标「本仓自拟」，不得写成「行业标准形态」。

**诚实边界**：专有主控固件不可见 ⇒ 判断来自 NVMe 规范、公开架构资料、
`fc_nand_design.md §26`（HIL / FTL / FCC / DRAM + 队列·门铃·完成；不讲 BIT 内部）。
主流分岔（DRAM 型 vs DRAM-less）要说清对齐哪支。

**主控设计考点**：分层 / 队列+门铃+完成 / 数据面轮询 / 通道并行仲裁 / ECC-LDPC / 掉电恢复。
⚠「ISR 放哪个文件」不在考点上（属工程整洁度），别过度投入。

**学习产出**：除改动本身，还要讲清主流怎么做、为什么、本仓在哪、差在哪。

## 〇.5 工作方式 —— 效率协议

**任务说明五要素**：目标 / 范围（可改、不可改）/ 判据 / 产出 / 验证关卡。缺哪个就问，不猜。

**三种默认模式**：
- `只分析`：不产生 diff，只给结论 + `file:line`。
- `只登记`：写台账 / backlog，不改代码。
- `改+证据`：批量改 + 一次里程碑构建，汇报「改了什么 + 证据 + 未做什么」。

**输出纪律**：结论 + `file:line`；不贴整段文件；diff 先 `--stat`；长行 grep 截断；
不重复已建立的事实。

**批量与里程碑**：机械改动走脚本（P8）；一批一验（P11）；纯注释 / 文档不编译。
**计划先行**：跨文件 / 表示链 / 介质格式改动先给方案（P10）。
**长任务**：`.claude/review/STATE.md` 断点；按模块加载 inbox；不重读整本台账。
**不做**：未请求的子代理 / 工作流；反复上板试；「顺便验证」（R8）。

## 一、规则 R —— 违反即返工

### 验证与交付
**R1 验证状态必须如实标注。** 未上板一律标「未上板」；禁止「已完成 / 已修复 / 已验证」。声称验证通过必须给证据（编译输出、板端串口）。
**R2 编译三关 + 上板判据。** `make all` / `make release` / `make tests UT=<FC|FTL|HIL>`（涉及哪层跑对应 UT）；零新告警。`make tests` 只证链接；碰**建链语义**（链接脚本 `MEMORY` / `SECTIONS` / 地址 / 启动 / 符号）、**位图**、**地址**必须上板；**纯路径 / 文件名移动且脚本内容不变**，构建 + `make -n` 确认 `-T` 即可。测试源由用户维护：UT 编不过如实报告，不回退生产代码、不加兼容垫片。
**R3 提交与推送由用户决定。** `commit` / `push` 都等用户明确发话；改完只报告「未提交、未推送」。
**R4 只跑目标平台。** stm32h743 + MT29F4G08；不用 host 模拟；测试源由用户自建。
测试代码注释**不得写 RISK 编号**；风险映射只在台账。
**R5 `CONFIG_FC_RAID` 保持 0。**（`include/fc/fc_interface.h`；功能代码完成但未验证）
**R6 排版不手工调。** `.clang-format` 唯一权威；`make all` 已前置 `format`。

### 读码与改动
**R7 读码 / 讨论期默认只分析不改造。** 不主动改代码，除非明确说「帮我改」。IO 路线读码：分批贴建议时**只登记、不分析**，原话进 `.claude/io_review_backlog.md`；全面审查用 `.claude/review/inbox/`。
**R8 不要「顺便验证一下」。** 不临时改开关再改回；验证构建只跑默认档。
**R9 复用优先：写之前先 grep，收尾自查。** 新增位操作 / 地址转换 / 结构体构造 / 相似分支前，先找已有 helper 与相似实现（C2）。

### 术语与登记
**R10 术语用行业主流词汇。** 主流词是英文就直接写；自造词首次出现必须给「主流英文术语 + 本仓自拟 + 差异」。不得把自造词写成行业标准。
**R11 缺陷与判定结论必须当场登记。** 登记处：`docs/design/known_open_items.md` 对应模块（模块 1~16）；`已闭合 / 不实现` 迁 `docs/design/known_closed_items.md`（只追加）。五要素：状态 / 后果 / 缓解 / 下一步 / 证据定位。对话里报了但表里没有 = 未完成。
**R12 硬件 / 协议「要求」必须先找到原文。** 说「协议要求 / 规范规定 / 硬件定死」前，先在 `docs/`、`include/nand/*.h` 或 datasheet / ONFI 找依据并给定位；找不到就说「这是本项目的选择」。

## 二、判据 C —— 取舍时的裁决标准

### 设计与复用
**C1 设计判据 = §〇。** 引**有出处的主流做法**，不是 Linux、不是本仓现状；§26 只给分层，具体机制另找出处。
**C2 代码复用：判据是「有没有手工等价物」。** 该用没用 → 收敛调用点；helper 零调用 → 先查是否有手工内联，有则收敛。同一逻辑第 2 次出现该提炼，第 3 次必须提炼；仅为 API 完整性保留的 helper 不算重复。
**C3 可读性难点的归因。** 先怀疑**「不变量靠外部模块手工维护」**，不是命名或文件结构。

### 断言与证据
**C4 编译期断言的边界。** `_Static_assert` 证不了位域布局等实现相关性质；证不了的不变量退化为运行期 `ASSERT`。
**C5 静默吞 vs ASSERT。** 无返回码的 `if (p == NULL) return;` → 若唯一调用点结构性恒非空，收口成 `ASSERT`；有返回码的 host 入口保留 if-guard。`ASSERT` 在 release 裁空 ⇒ 后果是静默介质破坏时，必须再补 release 运行期拒绝（H14）。
**C6 体积口径。** `.text`/`.bss` 是 `arm-none-eabi-size` 输出（text 含 .rodata+.data，bss 另含 NOLOAD），不是 `*.size` 段尺寸；`sizeof` / 字段偏移用 `readelf --debug-dump=info`。
**C7 写断言前先问「它能否失败」。** 恒真断言比没有更糟；必须构造**一个必然让它失败的负例**。
**C8 新增校验 / 功能必须给「确实生效过」的证据。** 跑通、零告警、断言全绿**不等于**功能生效。

### 坐标与语义
**C9 同一语义只允许一套坐标。** 第二套坐标即为缺陷；PBA↔PAA 等跨边界转换只允许在**单一漏斗**发生，现场手写位操作 = 缺陷。
**C10 改语义 / 默认值 / 含义时必须回扫全部调用点。**「值不变但含义变了」编译期判不出来。
**C11 同值 ≠ 同一坐标。** 判据是「同一语义」不是「值相同」；不同语义即使同值也必须分开命名。
**C12 用状态不变量做 O(1) 守卫。** 已有不变量能直接判错时，不要用「池满 / 数组越界」等晚期症状做代理。

## 三、分层缺陷形态 —— 三层主导失效模式不同

**3.1 FTL 层：不变量靠外部模块手工维护。** 同一不变量由多个模块各自维护，任一处漏掉就
**永久漏账**，不报错、不崩溃，只表现为容量无声流失。对策：唯一闸门 + 账本访问器。

**3.2 FC-NAND 层：静默失效 + 判据没有区分力。** 两类都能通过全部编译与断言：
- 静默失效：跑通、零告警、断言全绿，但功能**从未生效**（CRC 恒通过、状态字节丢弃、
  `plane_paa[]` 未填）。
- 判据无区分力：断言恒真；只验一条就宣称全验过。
- **编译期无声的三类量：建链语义 / 位图 / 地址** ⇒ 碰了必须上板；纯路径 / 文件名移动且脚本内容不变，构建 + `make -n` 即可。位图是 flat 绝对编号。

**3.3 平台 / 工具链层：判据与证据本身不可靠。** 工具 / 库行为与直觉不符且不报错
（HAL 频率查询恒返 0、CMSIS `optimize("Os")` TU 泄漏；本仓用完整 newlib，`hh/ll/j/z/t` 可用）；
「看起来像证据的东西」不是证据（SWD 读可缓存内存、`.noinit` 只有断电才清、
完整 banner ≠ 没崩、`.bin` 逐字相同在动 include 图时不成立）。

## 四、别「修好」清单

看起来像 bug，其实是有意设计。**动之前先问用户。**

- `src/ftl/read_recovery.c` 的 `RR_FEATURE_READ_RETRY = 0x89` 在本器件是保留地址、自始空转。
- `src/fc/fc.c` 的 4 处 `goto stripe_error` 是合规的统一错误出口。
- `switch` 里非法 `phase` 的 `default` 带 release / 复位副作用，是有意设计。
- `fc_bch_decode` 的 ASSERT 必须在**函数体首条语句**。
- `fc_command_release` 的幂等早退、`pop_n(ids == NULL)`、`fc_bch_encode(ecc == NULL)`
  是有意的运行时分支 / 可选参数语义。
- `fault_port.c` 的 FPU 帧极性判断（`EXC_RETURN` bit4）是对的。
- `FTL_BLOCK_STATE_FREE` 会说谎；选块以数组为准；`[FTL] dangling` 是常驻守卫勿删。
- `src/ftl/journal.c` 整体不转断言（校验从 NAND 读回的不受信数据，H29）。
- 历史 changelog 正文不改写（更正写进新文档）。

## 五、C 语言规范

- 一句话规则：`.claude/rules/c-coding.md`（触碰 `src/` / `include/` / `tests/` 自动加载）。
- 细则 / why / 反例：`docs/guides/coding_rules.md`。
- 通用 15 章 + 不采纳理由：`docs/guides/coding_rules_generic.md`。
- 编号 H1~H50 / S1~S6；出处映射见 `.claude/rules/c-coding.md` 顶部。
- 用户问某条 H 规则时，先读 `docs/guides/coding_rules.md` 对应小节，不凭记忆答。

## 六、流程 P

### 动手前
**P0 动手前先查主流。** ① 主流怎么实现、出处；② 本仓现在怎么做、差在哪；③ 再读设计文档、动手。
**P1 改 FTL 前先读设计文档。** `ftl_design.md` 对应章节「N.1 最新策略」+「N.2 策略选取」；演进史 `revision_history/ftl_revision_history.md`。
**P2 引用文档一律 `docs/...` 全路径。**
**P3 动断言前先读 `assertion_guidelines.md`。** 判据 9：静默介质破坏除 debug `ASSERT` 外必须补 release 运行期拒绝；判据 10：release 档 unused 告警是「没有等价守卫」的唯一提示。

### skill 与留痕
**P4 skill 入口**（`.claude/skills/`，按需加载）：

| skill | 性质 | 何时用 |
|---|---|---|
| `basessd-project` | 判据型 | 新功能 / 新项目立项 |
| `basessd-design` | 判据型 | 方案 / ADR |
| `basessd-verify` | 操作型 | 测试矩阵 / 验收 |
| `basessd-maintain` | 操作型 | 变更 / 回归 / 兼容性 |
| `basessd-release` | 操作型 | 版本 / 许可 / CI / 打包 |
| `basessd-verify-board` | 操作型 | 编译 → 烧写 → 串口 → 判定 |
| `basessd-maintain-fault` | 操作型 | HardFault / 断言 / 静默失效 / 结果不对 |
| `basessd-develop-ftl` | 约束型 | 改 `src/ftl/`、`include/ftl/` |
| `basessd-develop-fc` | 约束型 | 改 `src/fc/`、`src/nand/` |
| `basessd-develop-hil` | 约束型 | 改 `src/hil/`（USB/CLI/请求拆分） |
| `basessd-develop-platform` | 约束型 | 改 `src/platform/`、移植 |
| `basessd-review` | 约束型 | 审查 C 代码 / 设计文档 |
| `basessd-design-arch` | 判据型 | 分层决策 / 增删接口 / 跨层 |

命名：生命周期级 `basessd-<stage>`；子流程 `basessd-<stage>-<domain>`；
`name` 只用小写字母、数字、连字符（VS Code / Claude Code 校验），中文放 `description`。
⚠ 约束型说「不要做什么」，操作型说「怎么做」，判据型说「怎么决定」。
**P5 偏离必须留痕。** 有意不遵守某条时，在该处写「偏离 Hxx，因为…」。沉默偏离 = 下次分不清「有意」与「漏了」。
**P6 故障排查先质疑证据。** ① 判据本身成立吗（当场镜像？断电后记录？绕过 D-Cache？）；② 再定位；③ 结论必须给**可复现负例**。入口 `docs/design/common/fault_capture.md`。

### 批量与效率
**P7 检视 / 修复类任务一次性改完。** 确定项一次改完（含同类停更），不逐档停下；只在真正有取舍或高风险时提问。汇报「改了什么 + 证据 + 未做什么」。
**P8 机械改词 / 重命名用批量替换。** `grep -rl` 取目标集 + 一条脚本；先长词再短词；历史 / 归档显式 skip；验证只跑 `grep -c` / `git diff --stat`。
- 机械改名（目录 / symlink / 引用）：一个映射表 + 一个 Python 脚本；`git status --short` 一次只看异常；一条命令跑 rules/docs/review/diff-check；汇报只给计数 + 失败项；不反查第三方 bundle。
**P9 改完 / 推完即止；「看效果」是用户自己做的事。** 不把它转成我的任务，只报告事实。⚠ 不省 R1：声称验证通过仍须给证据。
**P10 表示链改动先写契约。** 改语义 / 类型 / 宽度 / 哨兵 / 位置前写六行：坐标 / 表 / 计数 / 归属 / 表示链 / 判据。六行写不出 = 还没想清，不得动代码。
**P11 构建频率：批量收尾跑一次；注释 / 文档不编译。** 纯注释 / 文档：`git diff --check` + 定点 grep；分析阶段不构建；代码语义改动一批一验；头文件 / API / 配置 / 地址 / 位图类按 R2 全量。汇报只给最后一次构建证据。
**P12 文档同步节流。** 迭代期只写代码内注释 + 台账新增；不逐次改 `docs/` 下设计 / 接口 / UT / 平台文档（含主干六个；`ftl_api_usage.md` 定稿前完全不改）。定稿收尾以 `git diff --stat` + 符号变更为清单一次性回扫。

### 规范化与文档
**P13 机械规范化的完成判据。** 不得用「旧名 grep = 0 / 编译通过」证明全仓完成；必须用正向不变量检查器（H50 用 `make types-check`），并给覆盖率表。检查器必须有负例。
**P14 文档分层。** design / platform / verification / ledger / revision / archive 分离；design 文档不写平台细节、代码坐标、当前测量、账本 ID；例外用 `doc-policy` 标注。检查：`make docs-check` / `docs-report` / `docs-test`；规则正文在 `docs/README.md`。

## 七、建议 S

S1~S6 正文在 `.claude/rules/c-coding.md`。一句话：对齐周围代码；注释解释 why；中文注释；
函数契约用 `/** */`，实现细节用 `//`。

## 八、项目坐标

| 项 | 位置 |
|---|---|
| 文档地图 | `docs/README.md` |
| 未闭合台账（R11） | `docs/design/known_open_items.md` |
| 已闭合 / 不实现 | `docs/design/known_closed_items.md` |
| 审查工作状态 | `.claude/review/STATE.md`（gitignored） |
| 构建 | `make all` / `make release` / `make tests UT=FC\|FTL\|HIL` |
| 检查 | `make check`（format / types / docs / ledger / rules / headers / 布局 / 优化档；详见 `make help`） |
| 故障取证 | `docs/design/common/fault_capture.md` |
| C 规则细则 | `docs/guides/coding_rules.md` / `coding_rules_generic.md` |
| 本文件变更史 | `docs/archive/claude_md_history.md` |

## 九、本文件自身的变更记录

规则会演进，但变更史不写在本文件：见 `docs/archive/claude_md_history.md`。
本文件正文始终是当前最新版本，只放改变行为的规则。

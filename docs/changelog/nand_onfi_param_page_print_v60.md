# v60：开机读 ONFI 参数页（ECh）并打印设备信息 —— 纯函数解码 + 结构体

## 〇、起因：手册 Table 11 那张参数页从没读过

用户贴出 MT29F4G08 手册 **Table 11「Parameter Page Data Structure」**（256 字节：
签名 / ONFI 版本 / 特性位 / 20 字节型号串 / 厂商 ID / 页块几何 / ECC 位数 /
时序模式支持位图 …），问「这个是不是应该弄一个函数读取并打印信息？」，随后明确
**「在初始化使用，打印设备信息」**。

**为什么值得做**：开机日志只有一行 `nand: ID=2C DC 90 95 56`（`nand_mt29f4g08abadawp.c`
的 Step 6），是 5 字节裸 ID —— 看不出**型号**（Table 11 bytes 44–63 =
`MT29F4G08ABADAWP`）、页/块几何、ECC 位数，以及 **bytes 129–130「Timing mode support」**
（本器件 `3Fh, 00h` = 支持 Mode 0–5，直接关系到 `CONFIG_NAND_TIMING_MODE_4`）。

**现状核查**（改之前）：
- `OP_READ_PARAMETER_PAGE`（`2U`，`ECh`，`.has_data=1U`，`.data_length=256U`）**早已存在**
  （`onfi_1_0.c` op 表），但**同步路径从未读过它** —— `nand_fc_execute` 至今只跑过
  RESET / READ_ID / SET_FEATURES。单测 `init_sequence` Step 3 读参数页走的是 **FC 异步
  desc 链**路径（`test_submit_wait` → `fc_service`），字节落 USER 缓冲。
- 全仓**没有**参数页结构体、没有字段偏移宏、没有小端读取 helper、没有设备信息打印例程。

## 一、设计定案

用户已定的三件事：① 解码做成**纯函数 + 结构体**；② **补单测**（并入既有套件，不新建文件）；
③ 打印**精选字段 ~3 行**。

| 决策 | 结论 |
|---|---|
| 分层 | ONFI 协议层（`onfi_1_0.h/.c`）放**纯函数**解码 + 结构体；器件层（`nand_mt29f4g08abadawp.h/.c`）放 typed 同步壳 + 打印 + 调用点 |
| 读取 | `nand_fc_execute(OP_READ_PARAMETER_PAGE, …)`，栈上 `uint8_t page[256]`。同步契约 `ASSERT(nand_dispatch_is_armed() == 0U)` 在 `nand_init` 内成立（此刻未 arm） |
| 字节序 | ONFI 多字节字段**小端** ⇒ 新增 `nand_onfi_le16()/le32()` 两个 `static inline`（全仓无现成 LE helper） |
| 失败处理 | 读失败**或**签名不符 ⇒ 只 `LOGW` 一行，**继续启动**。ID 已匹配 ⇒ 器件是好的，参数页读打嗝不该 brick 开机 |
| CRC（bytes 254–255） | **不做** —— 手册只写「Set at test」，多项式不在仓库内，无法查证 ⇒ 按「协议断言必须先查证」**不猜** |
| 时序字段量纲 | `tprog/terase/tr/tccs` **只打原值、不打单位后缀** —— Table 11 未给量纲，同上不猜 |

### 打印内容（精选 3 行，字段全部来自 Table 11）

```
nand: ONFI sig=ONFI rev=0x0002 model=MT29F4G08ABADAWP
nand: ONFI page=2048+64 partial=512+16 block=64p blocks=4096 lun=1 addrcyc=0x23 bitcell=1
nand: ONFI ecc=4bit prog/pg=4 tprog=600 terase=3000 tr=25 tccs=100 timingmodes=0x003F
```

> ⚠ **每行必须 < 192 字符** —— `src/common/log.c` 的 `log_vprintf` 落 `char message[192]`，
> 超长会截断并打 `...[truncated]`。三行实测都在 80 字符内。

## 二、`src/` 改动

| # | 文件 | 改动 |
|---|---|---|
| 1 | `include/nand/onfi_1_0.h` | 新增 §8：`NAND_ONFI_PARAM_PAGE_SIZE (256U)`、`NAND_ONFI_PARAM_MODEL_LEN (20U)`、22 个 `NAND_ONFI_PARAM_OFF_*` 字段偏移宏、`nand_onfi_le16/le32` inline、结构体 `nand_onfi_param_info_t`、`nand_onfi_parameter_page_decode()` 声明 |
| 2 | `src/nand/onfi_1_0.c` | 实现 `nand_onfi_parameter_page_decode()` —— **纯函数**（零 HAL / 零日志 / 零硬件），返回签名是否成立 |
| 3 | `include/nand/nand_mt29f4g08abadawp.h` | 声明 `nand_fc_read_parameter_page(uint8_t page[256], nand_fc_completion_t *)`（紧挨 `nand_fc_read_id`） |
| 4 | `src/nand/nand_mt29f4g08abadawp.c` | ① 同步壳（照 `nand_fc_read_id` 写法）；② `static void nand_onfi_param_print(const nand_onfi_param_info_t *)`（3 行 `LOGI`）；③ `nand_init` 插 **Step 6b**（ID 校验之后、`#if CONFIG_NAND_TIMING_MODE_4` 之前）：读 → 解码 → 打印 |

### ⚠ `nand_init` 的 Step 6b 为什么排在 Mode 4 之前

`CONFIG_NAND_TIMING_MODE_4` 的 SET_FEATURES 会改器件时序寄存器 —— 参数页是**只读、无副作用**的
查询，放在任何写配置**之前**最干净（读到的就是上电默认态下的器件自述）。

### ⚠ 生产路径零影响（已核实）

- 解码函数是**纯函数**，除 `nand_init` 的 Step 6b 外**零调用者**，不进 ECC / 数据路径。
- `OP_READ_PARAMETER_PAGE` 的 op 表定义**一行未动**（`data_length=256` 本就正确）。
- `src/ftl/**` 一行不动。
- 同步壳只被 `nand_init` 调用一次，不影响 FC 异步路径。

## 三、`tests/` 改动

并入既有 `tests/unit/test_fc_level1.c` 的 `logic` 套件，**不新建文件**。

新增 **Part 4：ONFI 参数页解码**，一条纯 RAM 用例 `onfi_param_page_decode`：

1. 用 `test_put_le16()/test_put_le32()` 在栈上按 Table 11 合成一张 256B 参数页
   （`ONFI` 签名 / rev `0x0002` / features `0x0018` / model `MT29F4G08ABADAWP` 空格补齐 /
   厂商 `0x2C` / page `2048+64` / partial `512+16` / block `64p` / blocks `4096` / lun `1` /
   addrcyc `0x23` / bitcell `1` / prog/pg `4` / ecc `4` / timingmodes `0x003F` /
   tprog `600` / tbers `3000` / tr `25` / tccs `100`）；
2. 喂 `nand_onfi_parameter_page_decode()`，**逐字段断言**，并经 `test_trace_summary` 打一行证据；
3. 再破坏签名（改 `page[0]`），断言解码函数**返回 0**（如实报「签名不符」）而字段仍被填满。

⚠ 计数器变量必须是 `uint16_t i;` —— `NAND_ONFI_PARAM_PAGE_SIZE == 256U`，`uint8_t` 会让
`i < 256` **恒真**（死循环），编译期由 `-Wtype-limits` 抓出。

⚠ **上板第一跑就把这条用例跑红了 —— 是真缺陷，不是测试写错**：器件把型号串用**空格（0x20）右填充**
到 20 字节，而解码侧原样拷贝 ⇒ `info.model == "MT29F4G08ABADAWP    "`（4 个尾空格），
与用例期望的 `"MT29F4G08ABADAWP"` 不符。开机日志里也肉眼可见同一现象
（`model=MT29F4G08ABADAWP    ` 后面一截空格）。**修在解码侧**（不是改断言）：拷完 20 字节后
从尾部回退吃掉 `0x20` 再补 NUL ⇒ 型号串成可比、可打的 C 串；顺带把开机日志的尾空格也清掉。

**用例数 57 → 58**（Level 1 `logic` **11 → 12**）。`ut_main.c` 不动，不新增注册表。

## 四、验证

- `make format` + `make format-check` ⇒ **通过**。
- `make clean && make tests` ⇒ **0 warning / 0 error**，链接 **248352 B**（v59 是 242776 B）。
- `arm-none-eabi-nm` 确认四个符号全部链接：
  `nand_fc_read_parameter_page` (T) / `nand_onfi_param_print` (t) /
  `nand_onfi_parameter_page_decode` (T) / `test_onfi_parameter_page_decode` (t)。
- `git diff --stat`：5 文件、**392 插入 / 7 删除**，无无关 churn
  （仅有的删除是 v59 墓碑、Stage 2.6 的 `sys.h`→`sys_port.h` 改名、以及我改写的两条注释计数行）。

### 上板已验（2026-10-11，seed `0x5a4f21e4`）：**58 of 58 (100%)**、0 skipped、870.45 ms

**开机日志 —— 三行全对，且这是本版唯一真风险（同步路径首次读参数页）的实测闭合**：

```
[INF] nand: ONFI sig=ONFI rev=0x0002 model=MT29F4G08ABADAWP
[INF] nand: ONFI page=2048+64 partial=512+16 block=64p blocks=4096 lun=1 addrcyc=0x23 bitcell=1
[INF] nand: ONFI ecc=4bit prog/pg=4 tprog=600 terase=3000 tr=25 tccs=100 timingmodes=0x003F
```

`model` / 页块几何 / ECC 位数 / `timingmodes=0x003F`（Mode 0–5 全支持）全部与手册 Table 11 吻合
⇒ **`nand_fc_execute` 走同步路径读参数页成立**，读失败降级分支未被触发。`model=` 后**无尾空格**
（trim 生效）。

**中间有一跑是红的 —— 修完才全绿**：第一版解码原样拷贝 20 字节型号串，器件用空格右填充 ⇒

```
assertion failed: string info.model == "MT29F4G08ABADAWP" ("MT29F4G08ABADAWP    "=="MT29F4G08ABADAWP")
```

即上面 §三 记的那个**真缺陷**。修在解码侧（去尾部 `0x20`）后重建 248352 B，重跑：

```
/fc-nand/logic/onfi_param_page_decode:                 [ OK     ] [ 26.79 us ]
58 of 58 (100%) tests successful, 0 (0%) test skipped. Total wall-clock: 870.45 ms
```

既有 57 条逐字不变。

## 五、风险（如实记录）

1. ~~**同步路径首次读参数页**~~ —— **已上板闭合（2026-10-11）**：`nand_fc_execute` 此前只跑过
   RESET / READ_ID / SET_FEATURES，参数页**从未走同步路径**（单测走的是 FC 异步链）。
   上板开机三行 `nand: ONFI …` 全对 ⇒ 机械上成立（`DATA_OUT` 的 handler 用的正是
   `request->data_length`）。**若读失败仍只降级为 `LOGW`、不 brick 开机** ——
   这也正是把它做成**非致命**的原因（本轮没被触发）。
2. **时序字段量纲未查证**：`tprog/terase/tr/tccs` 只打原值、不打单位后缀，
   避免凭记忆断言（见记忆 `no-unverified-protocol-claims`）。
3. **型号串尾空格 —— 已由测试抓出并修复**：见 §三/§四。器件按 ONFI 约定用空格右填充，
   解码侧必须 trim，否则 `%s` 打出的串带尾空格（日志里肉眼可见、字符串比较直接红）。
   这是本版**测试真正拦下的唯一缺陷**，也说明「补一条纯函数单测」这个决定是划算的。
4. **`g_fc_logic_tests[]` 整体重排**：该数组受 `AlignArrayOfStructures` 管辖
   （记忆 `clang-format-aas-*`），加一行会重排整块 —— `make format` 是权威，此 churn 可接受。

## 不做

- **不用参数页的时序支持位去驱动 Mode 4** —— 用户只要求打印；改行为是另一件事。
- **不加 CLI 命令** —— 用户明确「在初始化使用」。
- **不做 CRC 校验** —— 多项式不在仓库内，无法查证。
- 不动 `src/ftl/**`、不动 h7r3、不翻 `CONFIG_FC_RAID`、不碰
  `tests/integration/**`、`tests/framework/**`、`ut_main.c`。

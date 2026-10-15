# v24：地址布局统一（LBA / PBA / PAA）+ 加扰器对齐存储主控

- **日期**：2026-10-08
- **范围**：`include/nand/nand_config.h`、`include/fc/fc_command.h`、`include/fc/fc_interface.h`、`include/fc/fc_random_engine.h`、`include/ftl/ftl_fc_interface.h`、`include/nand/nand_dispatch.h`、`src/fc/*`、`src/ftl/{ftl,gc,ftl_fc_interface}.c`、`src/nand/nand_dispatch.c`、`tests/unit/test_fc_level2.c`、`tests/unit/test_fc_level3.c`、文档
- **前置**：v13（策略层上移 FTL）、v17（建链收敛为一条）、v23（建链收敛为单一掩码旋钮）
- **介质格式变更**：META 字段语义由"页 LBA 低 2 位塞 CW 号"变为"完整扇区 LBA"，且 PBA 编码的 plane/page 位序对调 ⇒ **板上既有数据不可读，必须重新格式化**。
- **上板未验**：本项目硬规则，`make tests` 上板由用户执行。

## 背景

起因是用户的三条要求：

1. 加扰种子改为 `desc->meta`（该 CW 的**逻辑**地址），不再用物理地址；
2. 效果向存储主控靠齐——既要破除 `01010101` 这类间隔分布，**总数也要 1:1 均衡**；
3. 用户观察「当前好像办不到」。

走查确认 ③ 属实，并牵出四组缺陷。

## 四组缺陷

### D1 · LFSR 抽头错，状态空间坍缩到 63

`src/fc/fc_random_engine.c` 的 `fc_lfsr_byte`：

```c
uint8_t fb = (uint8_t)(((lfsr >> 15U) ^ (lfsr >> 13U) ^ (lfsr >> 12U) ^ (lfsr >> 10U)) & 1U);
lfsr       = (uint16_t)((lfsr >> 1U) | ((uint16_t)fb << 15U));
```

本实现是**右移** LFSR（输出 bit0、反馈进 bit15）。右移方向下反馈抽头是多项式系数的**镜像**，
而这里照抄了左移写法的 `{15,13,12,10}` ⇒ 新状态的 16 位**全部只依赖旧 bit1..bit15**，
被移出的 bit0 没有反馈回去 ⇒ 映射不可逆（`1 → 0` 是吸收态）⇒ 状态空间坍缩。实测：

| 指标 | 实测（修前） | 应然 |
|---|---|---|
| LFSR 状态周期 | **63** | 65535 |
| 字节流最终周期 | **63 字节** | — |
| 同页 4 CW 密钥流差异 | **1/520 字节**（仅 byte0） | 数百字节 |
| 0/1 均衡 | 50.0–50.7%（巧合达标） | — |
| 最长游程 | 6–7 bit | — |

520 字节的 CW = 63 字节图案重复 8.25 遍。写 `0101…` 进去出来仍是周期 63 字节的图案——
加扰器对要求 ② 完全失效。正确抽头是 `{0,2,3,5}`：

```c
uint8_t fb = (uint8_t)((lfsr ^ (lfsr >> 2U) ^ (lfsr >> 3U) ^ (lfsr >> 5U)) & 1U);
```

（穷举 1820 组 4 抽头验证，52 组为最大长度；`{0,2,3,5}` 是多项式 x¹⁶+x¹⁴+x¹³+x¹¹+1 的镜像。）

### D2 · 种子取自物理地址 PAA

三处 `fc_random_seed_from_paa(desc->paa, 0U)`。物理地址随 GC 搬家而变 ⇒ 密文随之变，
正是业界要避免的（业界做法是**种子 = 被写页的逻辑地址**，搬页后逻辑地址不变 ⇒ 不需要
"解扰再按新物理地址重加扰"）。且第二形参 `program_sequence` 恒传 `0U`，该维度从未接上。

### D3 · `command->meta` 是页 LBA，且 plane 隐式占 LBA bit0，被 `& ~3` 抹掉

`ftl.c:585` 的 `l2p_cache_read(internal_lba + 1U)` 与 `ftl.c:169` 的
`l2p_cache_write(io.lba + 1, plane1页)` 证明 **FTL 的页号 `internal_lba` 里 plane 是最低位**。
而旧 `lba_to_cw_meta(lba, cw) = (lba & ~3) | cw` 把 cw 写进 bit[1:0]，**同时覆盖掉 plane 位**，
还顺带抹掉页索引的 bit1。加上 `fc_interface.c` 对两个 plane 传的是同一个 `command->meta`（单标量）：

- plane1 的 4 条 desc 拿到 **plane0 的 meta**；plane0 CW1 与 plane1 CW0 **撞成同一个值**；
- 连续 4 页 LBA（L=4,5,6,7）全部得到 `4|cw`，4 个不同页共用 4 条密钥流；
- 连带潜伏 bug：`gc.c` 用 `*lba_out = meta_cw0 & ~3` 反查 L2P，页 LBA 非 4 倍数时反查到别的页 →
  `l2p_cache_read` 不匹配 → GC 把该页当无效页**清掉有效位却不搬迁**（静默数据丢失）。
  板测从 LBA 0 起步所以没暴露。

### D4 · LBA / PBA / PAA 三套布局互不相同

| | 低位优先布局（v24 前） | 定义处 |
|---|---|---|
| PAA | `[cw:2][page:6][plane:1][block:11][lun]` | `nand_config.h` |
| PBA | PAA + 高位 `[ce][ch]`（**与 PAA 共用位序**，plane 也在 page 之上） | `nand_config.h` |
| FTL 页号 `internal_lba` | `[block][page][plane]`（plane **最低**） | `ftl.c` / `ftl.h` |

PAA 与 `internal_lba` 今天就是反的；PBA 继承 PAA 的位序，所以 PBA 与 LBA 也反着。
功能上无害（L2P 是真正的翻译表，已核所有 `paa_to_*`/`paa_make`/`pba_make` 调用点全走访问器），
但代价是边界上要维护两套编码：`fc_interface.c` 的 `desc->paa` 与 `desc->meta` 用完全不同的公式，
还分叉成 if/else 两条分支。

## 改动

### A. 地址布局：PAA 不动，PBA 另立一套（`include/nand/nand_config.h`）

**PAA 一个字节都没动**——它是**芯片地址**，由 MT29F4G08 的 BA6/PA 规则决定，
`paa_to_nand_row()` 负责把它编成行地址（`PA[5:0] | BA6<<6 | BA[17:7]`）。
19 处 `paa_make` 调用点、全部 `paa_to_*` 访问器、`paa_to_nand_row` 保持原样。

**PBA 按 LBA 的结构另立一套**（用户明确："我的意思是 PBA 的按照我给你的结构另外建立一个，不是共用一个。要不然就乱了"）：

| | 布局（低位优先） |
|---|---|
| PAA（**不动**） | `[cw:2][page:6][plane:1][block:11][lun]` |
| **PBA（新）** | `[cw:2][plane:1][page:6][block:11][lun][ce:2][ch:2]` |
| **LBA（新）** | 同 PBA —— `[ch:ce:lun:block:page:plane:cw]` |

```c
#define PBA_CW_SHIFT    (0U)
#define PBA_PLANE_SHIFT (PBA_CW_SHIFT + CW_WIDTH)            /* = 2 */
#define PBA_PAGE_SHIFT  (PBA_PLANE_SHIFT + PLANE_ADDR_WIDTH) /* = 3 */
#define PBA_BLOCK_SHIFT (PBA_PAGE_SHIFT + PAGE_ADDR_WIDTH)   /* = 9 */
#define PBA_LUN_SHIFT   (PBA_BLOCK_SHIFT + BLOCK_ADDR_WIDTH) /* = 20 */
```

CE/CH 移位（20/22）不变；`PBA_BLOCK_SHIFT` / `PBA_LUN_SHIFT` 数值与今天相同，**只有 plane/page 互换**。

**为什么 PBA 的 plane 在 page 之下**：FTL 页号 `internal_lba` 里 plane 就是最低位。
取 `LBA >> CW_WIDTH` 必须**逐位等于**今天的 `internal_lba`，否则 plane1 会变成
`internal_lba + 64`，L2P 编号、journal / checkpoint 页号、所有 `+1` 约定全要改。
**这是选 `[page][plane]` 而不是跟 PAA 的 `[plane][page]` 的唯一理由。**

转换函数重构（"重构格式转换函数"的正题）：

| 函数 | 现在 | 改为 |
|---|---|---|
| `pba_to_paa(pba)` | `pba & PAA_FROM_PBA_MASK`（纯掩码） | 按字段重排：`paa_make(pba_to_page(pba), pba_to_plane(pba), pba_to_block(pba), pba_to_lun(pba), pba_to_cw(pba))` |
| `pba_from_paa(ch, ce, paa)` | 直接或上 ch/ce | 改名 **`paa_to_pba(ch, ce, paa)`**，用 `paa_to_*` 取字段后按 PBA 布局重排 |
| `pba_make(...)` | 复用 `paa_make` | 内部改按 `PBA_*_SHIFT` 打包（**签名不变**） |
| `pba_to_page/plane/block/lun/cw/nand_row` | 经 `pba_to_paa` 中转 | 直接按 `PBA_*_SHIFT` 取字段（**签名不变**） |
| `pba_with_page/plane/block/lun` | 经 `paa_with_*` + `pba_from_paa` 往返 | 直接按 PBA 移位改字段 |
| `PAA_FROM_PBA_MASK` | 掩码宏 | **删除**（新位序下不再成立） |
| `lba_to_cw_meta(lba, cw)` | 低 2 位塞 cw | 改名 **`lba_to_meta(lba, plane, cw)`**：`(lba & ~PLANE_CW_MASK) \| (plane << PBA_PLANE_SHIFT) \| cw` |

新增 `PLANE_CW_MASK = 0x7`（`[plane:1][cw:2]`），PBA 与 LBA 共用。

**死代码清理**：`paa_same_block()` 直接 `a & PAA_BLOCK_MASK` 而没先右移，本来就是错的且零调用者
（按 `assertion_guidelines.md` §十七 判据 5 删除）；`pba_same_block()` 同删。

### B. FTL → FC 边界改传 PBA

- `fc_command_t.paa` → **`.pba`**（类型 `nand_pba_t`）。旧版 FTL 侧 `ftl_fc_interface.h` 用
  `paa_make(...)` 打包，**ch/ce 被丢掉**；改传 PBA 后拓扑信息不再在边界上蒸发（本构建 CH/CE 均为 1，属潜伏缺口）。
- 建链入口签名：`fc_command_configure_descriptor(command, plane, cw, nand_paa_t row_paa)`
  → `(..., nand_pba_t row_pba)`。
- `src/fc/fc.c` 里所有 `paa_to_*(command->paa)` → `pba_to_*(command->pba)`。

**建链公式收敛**（`src/fc/fc_interface.c`）——if/else 两条分支合并成一条，且与 meta 共用同一套宏：

```c
/* 一条公式造出两个值：基值不同（物理 PBA vs 逻辑 LBA），布局同构 */
const uint32_t plane_cw = ((uint32_t)plane << PBA_PLANE_SHIFT) | cw;

descriptor->paa  = pba_to_paa((row_pba & ~PLANE_CW_MASK) | plane_cw);
descriptor->meta = (command->meta & ~PLANE_CW_MASK) | plane_cw;   /* 等价 lba_to_meta(command->meta, plane, cw) */
```

单 plane 时 `plane == pba_to_plane(row_pba)`，`(row & ~0x7) | plane<<2 | cw` 恒等于 `(row & ~0x7) | cw`
⇒ 原 else 分支被吸收。既有 `ASSERT(paa_to_plane(row_paa) == plane)` 改为 `pba_to_plane(row_pba)`。

### C. `command->meta` 提升为扇区 LBA（D3 的修法）

| 文件 | 改动 |
|---|---|
| `src/ftl/ftl.c` | `cmd->meta = (DWORD)internal_lba;` → `(DWORD)(internal_lba << CW_WIDTH)`（读、写两处） |
| `src/ftl/ftl_fc_interface.c` | `sync_page_prepare` 同式 |
| `src/ftl/gc.c` | `*lba_out = (lba_t)(meta_cw0 & ~3UL);` → `(lba_t)(meta_cw0 >> CW_WIDTH)`（还原**页** LBA，喂给页粒度的 L2P） |

**L2P 索引不动**：`LBA >> CW_WIDTH` 逐位等于今天的 `internal_lba`（plane 最低），
故今天所有 `+1` 约定、journal / checkpoint 里的页号、`L2P_REGIONS_TOTAL × L2P_ENTRIES_PER_REGION`
的规模全部不变。

### D. 加扰器

**D1** 修 LFSR 抽头（见上）。

**D2** 新增 `fc_random_seed_from_meta(uint32_t cw_meta)`，删除 `fc_random_seed_from_paa()` 与
**`FC_RANDOM_SEED_STRATEGY` 三选一旋钮**（用户："1c 策略删掉"）——种子来源塌缩为一种：

```c
uint32_t fc_random_seed_from_meta(uint32_t cw_meta)
{
    uint32_t seed = cw_meta ^ (cw_meta >> 16U);   /* 折叠 32 → 16：下游 LFSR 只用 seed & 0xFFFF */
    seed *= 0x9E3779B9UL;                          /* 雪崩混合（splitmix32 常量） */
    seed ^= seed >> 16U;
    seed *= 0x85EBCA6BUL;
    seed ^= seed >> 13U;
    return (seed != 0U) ? seed : 0x1U;             /* LFSR 全零是吸收态 */
}
```

**必须折叠 32→16**：下游 `lfsr_state = seed & 0xFFFF`，若不折叠，扇区 LBA 相差 65536（= 32 MB）
的两页会撞种子，而器件有 1M 扇区。混合则保证同页 4 条只差 1 的 LBA 充分散开。
与物理地址无关：GC 搬页后逻辑地址不变 ⇒ 密文不变。

头注释里"ONFI 标准加扰算法 / ONFI 标准多项式"的措辞一并修正——**ONFI 不规定加扰器**
（`no-unverified-protocol-claims` 判据）。

**D3** 加扰范围 `[0,520)` → `[8,520)`（`FC_CRC_USER_OFFSET` 起、`FC_BUFFER_USER_SIZE` 长）：
META 与 CRC 保持明文。三处同步：encode Stage 1、process_read Pass 2、`fc_data_path_raid_recover`。

**D4** Pass 4 改为自洽 + 交叉校验：

```c
uint32_t read_meta = ...;                 /* 明文 */
desc->meta = read_meta;
if ((read_meta & PLANE_CW_MASK) != ((paa_to_plane(desc->paa) << PBA_PLANE_SHIFT) | paa_to_cw(desc->paa))) {
    desc->status |= FC_CW_STATUS_MATA_ERROR;        /* META 自身损坏（种子也错了） */
} else if ((command->meta != FC_META_UNKNOWN) &&
           (read_meta != lba_to_meta(command->meta, paa_to_plane(desc->paa), paa_to_cw(desc->paa)))) {
    desc->status |= FC_CW_STATUS_MATA_ERROR;        /* 读到了别的页 */
}
```

新增哨兵 `FC_META_UNKNOWN = 0xFFFFFFFFU`；`gc.c` 的 `cmd->meta = 0U` 改用它，GC 不再误报。
**不用 0 当哨兵**：0 是合法的扇区 LBA（页 0 / plane 0 / CW 0）。
必须用 `paa_to_plane(desc->paa)` + `paa_to_cw(desc->paa)` 而不是链走位——v22 已确立：
掩码不满时两者不同。

**顺带修掉一个既有误报**：今天 GC 读命令的 Pass 4 必然置 `MATA_ERROR`
（`command->meta = 0` ⇒ 期望 `cw`，页里存的是真实 LBA，必然不等）。GC 只看 `NAND_FAIL`
所以一直无害；改成自洽校验后消失。

### D5 · 删除被架空的 `fc_fill_cw_meta_paa`（§十七 判据 5）

`src/fc/fc.c` 的 `fc_fill_cw_meta_paa(controller, command)` 曾是"encode 前把每个 CW 的 PAA
写进其 buffer 的 META 字段"的一步，**双重失效**：

1. **内容错**：写的是 `md->paa`（物理地址），而 `buffer[0..3]` 自 v24 起该放 `desc->meta`（扇区 LBA）；
2. **被架空**：`fc_data_path_encode` 的 Stage 0 紧接着用 `desc->meta` 覆盖同一 4 字节
   ⇒ **零可观测效果**（自 Stage 0 引入以来一直如此）；
3. **遍历方式陈旧**：终止条件仍是 `mid < FC_DESCRIPTOR_POOL_DEPTH`，v17 已把走链终止统一为
   **哨兵**（`next_id == FC_DESCRIPTOR_ID_INVALID`），这处没跟上。

且它与 `fc_data_path_encode` 只覆盖单命令路径、链式路径（`fc_poll_dispatch_chain`）本来就不调它
——"单命令路径独有的预填"本身就是一处行为不对称（原注释还标着"勿统一"）。

**处置**：整函数删除，META→`buffer[0..3]` 的拷贝**只留 Stage 0**（两条派发路径都经 `fc_data_path_encode`，
覆盖面反而更完整）。删除后 `fc_poll_dispatch_single` 的 page write 分支只剩一行
`(void)fc_data_path_encode(command);`，与链式路径**完全一致**，上面那条"行为不对称"的注记随之删除。
RAID 路径不受影响：parity desc 的 META 本就由 XOR 从各数据 CW 的 META 线性还原，
不经这两个写入者。


（`command->meta = 0` ⇒ 期望 `cw`，页里存的是真实 LBA，必然不等）。GC 只看 `NAND_FAIL`
所以一直无害；改成自洽校验后消失。

### 为什么必须同时把 META 移出加扰区（承重设计点）

种子既然取自逻辑地址，读路径就需要 LBA。主机路径命令里带着 `command->meta`，
但 **GC / 掉电重建绕不开**：`gc.c` 设 `cmd->meta` 为哨兵，LBA 只能**从页内 META 读回来**。
若 META 仍在加扰区内 ⇒ **要 LBA 才能解扰 / 要解扰才能拿 LBA**，循环依赖。

强行改种子会让 GC：`seed = f(0)` 常量 → 解扰出垃圾 → `lba` 是垃圾 →
`l2p_cache_read(垃圾) != pba` → `ftl_block_clr_valid(源块, plane, page)` → `continue`，
即**源块有效页被逐个标记无效却从未搬迁**，随后块被擦除 = 静默数据丢失。
`power_failure_recovery.c` 走同一函数，同样中招。

**把 META（及 CRC）移出加扰区后，读路径自举**：Pass 2 直接读明文 META 得种子 ⇒
`gc.c`、`power_failure_recovery.c`、`ftl.c` 都**不需要为"取种子"而改**。

### E. 测试（只改不删）

- `test_fc_level2.c`：`cw_seed()` 改返回 `fc_random_seed_from_meta(desc->meta)`；
  `alloc_cmd_full()` 显式设 `cmd->pba` + `cmd->meta = L2_META_LBA`（非零扇区 LBA）——
  否则 4 条 CW 种子为 0,1,2,3，"种子随 LBA 变"测不出来；
  两处解扰点改 `buffer + FC_CRC_USER_OFFSET` / `FC_BUFFER_USER_SIZE`；
  MATA_ERROR 用例与 RAID 退化 stripe 的理由注释随 D4 更新。
- `test_fc_level3.c`：`make_paa()` 辅助 → `make_pba()`（`pba_make`，不是 `paa_make`）；
  15 处 `cmd->paa` → `cmd->pba`；`#if 0` 调试块里的 `CMD_FIELD_U(paa)` → `pba`
  （顺带删掉早已不存在的 `reserved0` 一行）。
- **无**用例依赖 `FC_RANDOM_SEED_STRATEGY`（已 grep 确认）。

### F. 文档

- `docs/design/fc_nand_design.md`：新增 §2.3.1「三套地址空间（v24 定稿）」；§2.3 per-CW META 补充；
  §5.1 编解码流水线（Stage 0/1 与 Pass 2/4）；§21.3.5 重写；§11 / §12 加"历史章节（v13 前接口）"横幅。
- 本文件。

## 验证

四档构建（`make clean` 后）：

| 档 | 结果 | 基线（v23） |
|---|---|---|
| `make debug` | **0 warning / 0 error**，149920 B | 150060 B |
| `make release` | **0 warning / 0 error**，84988 B | 85076 B |
| `make -k tests` | **0 warning / 0 error**，197088 B | 197388 B |
| RAID=1（直接调子 make） | **0 warning / 0 error**，204252 B | 204372 B |
| `make format-check` | **passed** | — |

`nm` 验：`pba_to_paa` / `lba_to_meta` / `paa_to_pba` **无全局符号**（debug 档只有 `t` 局部副本，
release 档全部 inline 掉）；`fc_random_seed_from_meta` 是唯一 `T`。

**离线自证（六条加扰判据，本轮唯一能在本地闭环的部分）**：

| # | 判据 | 实测 |
|---|---|---|
| ① | LFSR 状态周期 = 65535 | 65535 ✅ |
| ② | 单个 CW（4160 bit）内 16-bit 窗口无重复 | 无重复 ✅ |
| ③ | 0/1 均衡落在 49.5%–50.5% | 2092/4160 = **50.29%** ✅ |
| ④ | 最长游程 ≤ 20 bit | **12 bit** ✅ |
| ⑤ | 同页 4 条 CW 密钥流两两差异 ≥ 500/520 字节 | 最差 **516/520** ✅ |
| ⑥ | 扇区 LBA 相差 65536 的两页种子不同 | `0x9d347790` ≠ `0x8e784f3d` ✅ |

（修前 ⑤ 只有 1/520、① 只有 63。）

**离线自证（布局统一，遍历 4096 组合）**：

| # | 判据 | 实测 |
|---|---|---|
| ① | `paa_to_pba(ch, ce, pba_to_paa(pba)) == pba`（转换互逆，字段不丢） | 0 处不符 ✅ |
| ② | `lba_to_meta(lba, plane, cw)` 的低 3 位 == `plane<<PBA_PLANE_SHIFT \| cw`，高位保持 | 0 处不符 ✅ |
| ③ | `LBA >> CW_WIDTH == internal_lba`（今天所有 `+1` 约定不变） | 0 处不符 ✅ |
| ④ | `paa_to_nand_row` 对同一 (block,page,plane) 值不变 | `0x3245`（未变）✅ |

**上板未验**。**上板前须重新格式化**（介质格式变更）。

## 风险与未跟进

| 风险 | 规避 |
|---|---|
| PBA 位序对调后漏改某处按位比较 | 已核全部 `pba_to_*` / `pba_make` / `addr_from_paa` 都走访问器；落盘 **PBA** 编码变（L2P 表、journal 的 `block_pba`）——本轮反正重新格式化；`metadata.c` 存的是 **PAA**，不受影响 |
| `command->paa` → `pba` 漏改某处 | 已穷举：写入者 `ftl_fc_interface.h` 的 `addr_to_cmd`、测试两处；读者 `fc.c` 十余处 + `fc_command.h` |
| 加扰范围变更后凡"假设 `[0,520)` 全被加扰"的地方失配 | 已穷举 `fc_random_transform` 的 3 个生产调用点 + 1 个测试辅助 |
| Pass 2 自举后 META 损坏 ⇒ 种子错 ⇒ 静默解出垃圾 | Pass 4 的自洽校验（低 3 位 == plane/cw）专门抓这个；`all_ok == 0` 的早退仍在（不解扰，交给 RAID） |
| RAID 档 `fc_collect_decode_buffers` 不含 parity CW，恢复本就退化 | 既有状况（v22 已记），本轮只同步种子与范围，不修 |

**未跟进**：

1. **L2P 表 4× 冗余**：`internal_lba` 只需 18 bit（262,144 个 plane-page），而 L2P 表按 20 bit 建
   （1,048,576 条）⇒ 可缩到 512 region × 512 entry。属独立的存储优化，另立一轮。
3. **扇区粒度 L2P 未采纳**：`pba_make` 的 cw 字段仍闲置。若将来要让单个扇区独立映射
   （valid bitmap 降到 CW 级、GC 逐 CW 搬、journal 4×），才需要。本轮保持 plane-page 粒度。
4. **无加密**：业界高端做法是 AES-XTS 加密后数据天然随机；本项目只做加扰。
5. **`fc_random_transform` 的 `length` 是 `uint16_t`**：512 够用，将来扩大范围需复核。
6. **`ftl_cmd_setup_ppa_pair` 零测试覆盖**（v23 遗留）：本轮又改了它的 PBA 入参。
   按项目规则测试源文件归用户，补 `test_setup_ppa_pair_mask` 待用户点头。
7. **`tests/integration/test_fc_nand.c` 已失效**（调用 v13 删除的复数符号，且不在 `TEST_C_SRCS` 里）
   ——用户维护的上板测试，不动。

## 出处

- [Error Characterization, Mitigation, and Recovery in Flash-Memory-Based Solid-State Drives](https://arxiv.org/pdf/1706.08642v3.pdf)
  —— 主控加扰动机（0/1 均衡）、LFSR、**种子 = 页逻辑地址**（GC 免重加扰）、
  逻辑块（典型 4 kB）由若干 512 B–2 kB 的 ECC 码字组成。
- [Macronix AN1051: The Introduction of Randomizer Feature](https://www.macronix.com/Lists/ApplicationNote/Attachments/2151/AN1051V1-The%20Introduction%20of%20Randomizer%20Feature%20on%20MX30xFxG28AD_MX35xFxG24AD.pdf)
  —— 器件内建 randomizer（`RANDEN`/`RANDOPT`，Feature B0h）。
- [3x Faster Speed Solid-State Drive with a Write Order based Garbage Collection Scheme](https://www.sci-hub.st/storage/2024/4977/aa383ba9032e06d863560f6ccfc4c6a4/matsui2015.pdf)
  —— "LBA scrambler" 是 LBA→SLBA 的**地址重映射**，与比特级加扰无关，勿混淆。

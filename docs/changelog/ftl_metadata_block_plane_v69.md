# v0.69：修 `ftl_is_metadata_block` 的 plane 掩码 —— 被掩的块落在所有池之外，L2P 悬空撞断言

**日期**：2026-10-14
**分支**：main
**涉及模块**：`include/ftl/metadata.h` + `src/ftl/metadata.c`（判据加 `plane`）、`include/ftl/bst.h`
（`ftl_gblock_plane/block` 单点解码）、`src/main.c` / `tests/unit/test_ftl.c`（调用点）、
`src/ftl/power_failure_recovery.c`（`page_is_blank` 读失败判非空）、`include/ftl/journal.h` +
`src/ftl/journal.c`（`journal_replay` 回填实际回放条数）、`src/controller/cli.c`（usage 只在参数个数非法时打）
**起因**：板端**重启后写一次**即撞 `bst.h` 断言，且重启日志本身看不出任何异常

---

## 〇、起因

板端在重启后执行第一次覆盖写即死机：

```
[ERR] [FAULT] previous assert: b->valid_count > 0U (include/ftl/bst.h:261)
```

断言在 `ftl_block_sub_valid`（`bst.h`）里。调用它的只有一条路径：`ftl_confirm_write`
覆盖写时对**旧映射所在块**减有效计数。断言说 `valid_count > 0` ⇒ **L2P 指向了一个 BST
认为「零有效」的块**。

形态特殊在：**重启本身完全正常**（`F2` 重建跑完、`free` 合理、映射读回正确），只在**下一
次写**时炸。所以问题不在恢复的「结果」，在恢复**漏掉了某个块**。

## 一、根因：元数据块判据只比 per-plane 块号

`ftl_is_metadata_block` 三个分支：

| # | 分支 | 与 plane 的关系 |
|---|---|---|
| ① | 固定元数据分区 `[0, FTL_METADATA_BLOCK_END]` | **无关**（分片模型下各 die 用同一份块号，是设计） |
| ② | 当前日志块 `md_dir->cur_metadata_block` | 该字段存的是 **gblock**，取值随 allocator 动态分配 |
| ③ | 已登记日志块（`METADATA_TYPE_L2P_REGION` 条目） | 同上，动态分配 |

分支 ②③ 是**从用户区动态取的块**，旧实现却一律只比 **per-plane 块号**：

```c
/* 旧：cur_metadata_block 是 gblock，却只比了它的低 11 位 */
if ((md_dir->cur_block_valid != 0U) &&
    (block == (uint16_t)(md_dir->cur_metadata_block % BLOCKS_PER_PLANE)))
{
    return 1U;
}

/* 旧：paa_to_block 解出来的是 per-plane 块号，拿它去比 plane 0 的区域块 */
if ((entries[i].type == METADATA_TYPE_L2P_REGION) &&
    (paa_to_block(entries[i].paa) == block))
{
    return 1U;
}
```

⇒ **一个 plane 的日志块会连带把另一 plane 的同号块一起排除**。被掩掉的块会怎样：

1. **进不了 Free Pool** —— `main.c` 的空闲池回填用的是同一个判据；
2. **被 F2 重建跳过** —— `ftl_rebuild_closed_sb_list` 也用同一个判据 ⇒ 该块既不进 Open
   也不进 Closed ⇒ `valid_count` **恒 0**；
3. **而日志块会迁移** —— `metadata_dir_reclaim` / `metadata_dir_compact` 会把
   `cur_metadata_block` 换成另一个块号 ⇒ **掩码的块号随之漂移**。

于是历史上只要有一刻某个块没被掩住（前一个日志块已被换掉、新日志块又恰好不同号），
它就会被 allocator 当**普通用户块**发出去并写入数据，L2P 因此指向它。之后日志块换号、
掩码重新罩住它 ⇒ 下次重启重建时它被跳过 ⇒ `valid_count` 归 0 ⇒ **此后每次覆盖写该 LBA
都撞断言**。

⚠ **原判「偏保守、安全，不修」是错的**。v0.67 那轮体检时对这条判据的结论是「偏保守，
不修」（`ftl_optimization_plan.md §七.7` / §8.3）。当时的推理是：被掩的块**落在所有池之外**
（既不在 Free Pool、也不在 Closed），GC 永远选不到它 ⇒ 数据不会被擦掉 ⇒ 安全。

**这个推理的前半段对、后半段错**：数据确实不会被擦，但「`valid_count` 恒 0」这件事
**本身就是 `bst.h` 的断言条件**。被掩的块不是「保守」，是**定时炸弹**——它把「一个块
悄悄退出所有池」变成了「下次写这个 LBA 就死机」。本次已订正该文档两处。

## 二、修法

### ① 判据加 `plane`（主修）

签名改为 `ftl_is_metadata_block(const metadata_dir_cache_t *md_dir, uint8_t plane, uint16_t block)`：

- 分支 ① 固定分区**继续不看 plane**（这是设计，不是遗漏）；
- 分支 ②③ **必须带 plane 比较**（gblock → plane 后再比）。

三个调用点同步：`src/main.c`（空闲池回填）、`src/ftl/power_failure_recovery.c`
（F2 重建）、`tests/unit/test_ftl.c`（夹具）。

顺带在 `bst.h` 加两个 inline（`ftl_gblock` 的逆，**单点解码**）：

```c
static inline uint8_t  ftl_gblock_plane(uint16_t g) { return (uint8_t)((uint32_t)g / BLOCKS_PER_PLANE); }
static inline uint16_t ftl_gblock_block(uint16_t g) { return (uint16_t)((uint32_t)g % BLOCKS_PER_PLANE); }
```

`metadata.c` 里原有的 `gblock / BLOCKS_PER_PLANE` 内联算式改为调用它——**这类「gblock
还是 per-plane 块号」的混淆正是本缺陷的成因**，收敛到单一解码点。

### ② `page_is_blank` 读失败判「非空」（同一类「F2 静默漏块」的另一支）

`ftl_sync_read_page_raw` 的契约（`ftl_fc_interface.h:292`）明确写着：**返回 `FTL_ERROR`
时命令已释放、buffer 内容未定义**；它**刻意不判定 NAND 失败**（坏页也要拿到原始位）。
所以返回 `FTL_ERROR` 只代表**命令本身**没走通（提交失败 / 超时 / 拿不到 buffer）。

而 `page_is_blank` 旧实现 memset 成 0xFF 后**忽略返回码**：

```c
memset(raw_buf, 0xFF, PAGE_SIZE);
ftl_sync_read_page_raw(*addr, raw_buf, FC_COMMAND_MODULE_BBT);   /* ← 返回码丢弃 */
```

⇒ 命令没走通时 buffer 仍是全 0xFF ⇒ 该页被判「空白」⇒ `scan_block_write_pointer`
对该块返回 0 ⇒ 块被当成「**没写过**」**留在 Free Pool** ⇒ 既可能被 allocator 拿去
复用覆盖活数据，又让 L2P 指向一个 `valid_count` 恒 0 的块（与上面同一类后果）。

**修法**：读失败一律判「**非空**」（保守方向）：

```c
if (ftl_sync_read_page_raw(*addr, raw_buf, FC_COMMAND_MODULE_BBT) != FTL_OK)
{
    return 0U; /* 命令没走通 ⇒ 内容不可知 ⇒ 一律判非空 */
}
```

代价：真为空的块会被拉出 Free Pool、多交 GC 擦一次——**不丢数据**。

### ③ `journal_replay` 回填实际回放条数

`ftl_boot_power_failure` 的 `journal_entries` 观测口，旧实现用 journal 的 **RAM 写指针**
反推（`current_entry > 0` → `current_page × ENTRIES_PER_PAGE + current_entry`）——**恒假**：
`journal_replay` 末尾必调 `journal_resume`，后者把落点放到最新页之后并清 `current_entry`。

⇒ 三条启动路径的 `journal_entries` **全是 0**，该观测口不具判别力（本次定位时正是被它
误导了一轮）。修法：`journal_replay` 新增 `uint32_t *out_entries`，在**应用循环**里逐页
累加 `entry_count`（**读失败的页不计**），返回前写回。`out_entries` 可传 `NULL`。

### ④ CLI usage 只在参数个数非法时打

`cli.c` 旧写法把 handler 的返回值和参数个数判断并进同一个 `||`：

```c
if ((argc != 3) || (cli_cmd_wr(...) != 0)) { print_usage; }
```

⇒ handler 返回「**执行失败**」也被当成「参数错」⇒ `wr 0 32` 在 busy 时会紧跟一条
`usage: wr <lba> <n>`，看着像语法错误、实际参数完全合法。**wr/rd/tr/fl 四个命令的
busy 分支全中**。修法：usage 只在 `argc` 非法时打，handler 返回值单独处理。

## 三、上板证据（定位用的临时探针，结论已入档，探针已删）

板端日志四个问题一次说清：

| 探针 | 结论 |
|---|---|
| `[F2] judge page_seq=169 entry_count=32 sum=201 cp_jseq=169` | `sum(201) > cp_jseq(169)` ⇒ 判型**正确**走掉电路径，**不是** v0.64 那个 off-by-one 假绿 |
| `[L2P] r=0 dir=1 b=322 pl=0 pg=8 … e=00027a00..03` | 条目解出来是 plane0 block317 page0 cw0..3，与 `[F2] blk b=317 ok=4` 严丝合缝 ⇒ **L2P 不是垃圾数据** |
| 5 条 `[F2] skip … bad=0 meta=1`（块 16/48/49/58/62 + 322） | 全落在固定元数据分区 / 活日志块 ⇒ **判据无假阳性**（且 5 条全是 `bad=0`，坏块支也干净） |
| `[F2] rebuild blocks=8 partial=8 valid_cw=39` | 4(b317) + 16(b321) + 3(p1 b63) + 16(p1 b2043) = **39**，与各块 ok 计数一分不差 ⇒ 重建**完整自洽** |

⇒ 重建结果本身没问题，**漏的是池归属**：L2P 指向的块不在 Closed 列表里。

**旁证（plane 修复已在运行镜像里）**：`free=3715` 反证——

```
4096 − 126（固定 63 块 × 2 plane）− 1（活日志块） = 3969 = 3715 + 204(FTL_OP_PERCENT) + 50(SPARE)
```

若判据还是旧的只比块号，活日志块 322 会连 plane1 的同号块一起掩掉 ⇒ 3968 ⇒ **free 会是 3714**。

## 四、保留的守卫（非临时探针，勿删）

`ftl_confirm_write` 里保留一条**常驻守卫**：覆盖写时若旧块 `valid_count == 0`，先打一行
`[FTL] dangling …` 再进断言。理由：这是「**F2 重建漏认**」这一类缺陷的**唯一故障点**——
重建只要静默跳过一个仍被 L2P 指向的块（坏块 / 元数据块判据过宽 / 页 0 判空），该块就落在
所有池之外，且此后**每次**覆盖写该 LBA 都会死机，而重启本身看不出任何异常。

日志带旧块 `state` ⇒ **一次归因**：

| `st` | 含义 |
|---|---|
| `0x01` BAD | 坏块，重建按判据跳过（数据本已不可读） |
| `0x08` FREE | 已归还空闲池 ⇒ 块还会被 allocator 复用，**丢数据** |
| RESERVED | 元数据日志块 ⇒ 判据过宽（v0.69 已修 plane 掩码） |
| `0x0000` | 重建从未认领（页 0 判空 / 未扫描到） |

代价 = 每次覆盖写多一次比较；收益 = 把「重启后写一次就死机」变成**一条可读日志**。

**同轮删除的临时探针**：`[F2] judge`、`[F2] blk`、`[F2] skip`、`[L2P] r=`（
`grep -c` 已确认 `src/ftl/*.c` 中零残留）。`src/ftl/metadata.c` 中两处
`[META] log-block` / Anchor 写入点的 LOGD 是 a23c185 引入的既有调试日志，仅 LOGD 级别，**未动**。

## 五、文档同步

| 文件 | 改动 |
|---|---|
| `ftl_optimization_plan.md` | §七.7 与 §8.3 两处：**「偏保守，不修」→「✅ 已修（v0.69）」**，并写明原判断错在哪 |
| `ftl_revision_history.md` | 追加 v0.69 ADR（本文件同源） |
| `cw_level_mapping_design.md` | `ftl_is_metadata_block` 描述加 `plane`；表格行注明「v0.69 起带 plane 比较」 |
| `ftl_design.md` | `journal_replay` 签名（2 处接口列表 + 1 处启动示例调用） |
| `ftl_api_usage.md` | `journal_replay` 签名 + v0.69 注 |
| `ftl_ut_munit.md` | UT 表格 2 行（新签名 + 新增断言） |

## 六、验证

| 项 | 结果 |
|---|---|
| `make all` | 0 warning / 0 error（product 194196 B） |
| `make tests`（全套） | 0 warning / 0 error（324608 B） |
| `test_ftl.c` 新增断言 | `ftl_is_metadata_block(d, 1U, 100U) == 0`（plane1 同号块不再被 plane0 遮蔽） |
| 探针残留 | `grep -c 'F2] blk\|F2] skip\|F2] judge\|L2P] r='` in `src/ftl/*.c` = 0 |

**上板判据**（尚未上板）：

1. 重启后跑写负载，不再出现 `[FTL] dangling` 与 `bst.h` 断言；
2. `free` 比修复前**多 1**（每个活日志块不再白掩一个同号用户块）；
3. `boot type=N journal_entries=N` 的条数**不再恒 0**。

⚠ **上板前须全片格式化**：判据 ② 依赖空闲池计数，介质上残留的旧布局会干扰读数。

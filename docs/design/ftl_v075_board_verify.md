# v0.75 上板验证清单（命令提交/完成解耦 —— 八阶段 + 收口 + 首启修复）

> **性质**：`ftl-async-decouple`（计划书 `~/.claude/plans/crystalline-honking-shamir.md`）
> 全部落地后的**整机回归清单**。计划书里逐阶段列的上板判据，这里是可执行版本。
> **背景**：v0.75 把 FTL 里**除结构性顺序路径外**的全部 NAND 访问从「阻塞原语」改成
> 「提交即返回 + 完成在服务函数里处理」（op tag + `g_op_service[]`）。10 个提交：
> `a4e8a7c`(1 机制) → `aa9ed8a`(2 Journal) → `2f55520`(3 RR) → `a6e0f2b`(4 GC/WL) →
> `27addb8`(5 L2P) → `687bb73`(6 CP) → `5d62641`(7 BBT/Wear) → `4f74d8d`(8 metadata) →
> `e37aab5`(收口：`[JOB]` 心跳 + 同步入口断言) → `c1157cd`(修：完成表路由在 boot 期被
> `ftl_init` 抹掉)。
> **现状**：`c1157cd` 已上板验证**首启 + 二次上电**；**整机回归（本清单）未做**。
>
> ⚠ 本清单**只读串口**，不新增测试源文件（项目规则：测试源用户自建）。
> 所有输出字符串均逐字取自源码，可直接 `grep` 核对。

---

## 0. 前提与构建

```bash
rm -rf build/firmware/release            # ⚠ make clean 不删 release 对象 ⇒ 会假"干净"
make clean && make all                   # 必须 0 warning
```

预期末行附近：

```
AXI_RAM             384 KB    291.1 KB   75.8%
Binary size: 212024 bytes
```

烧录产物：`build/firmware/debug/bin/debug.bin`（无 `make flash` 目标，按既有 SWD 流程烧）。

> ⚠ **构建坑（沿用）**：
> ① 无源改动时 `make` **只跑 format、不编译、也不打 SRAM 报告** ⇒ 此时「0 warning」是假象；
>    判前先 `touch` 任一 `.c`。
> ② `BIN_DIR`/`OBJ_DIR` 在 UT 与固件之间共享 ⇒ 跑过 `make tests` 后**直接** `make debug`
>    是空操作（CFLAGS 不被跟踪），必须 `make clean && make debug`，否则烧的是测试镜像
>    （`.text` ≈ 272 KB）。

**上板前须知**：v0.75 未改介质格式，但**介质版本停在 5**（`metadata.h:195`）⇒ 若板子上
还是 v0.74 之前的镜像，**首次上电会自动全片擦除**（约 4096 块 × ~3 ms ≈ 十余秒），
不需要手动 `bbt --erase`。

---

## 1. 首启自动格式化（仅在介质版本不符时）

| | |
|---|---|
| **操作** | 烧 `debug.bin` → 复位 → 观察串口 |
| **预期** | 若版本不符：`[WRN] [FTL ] media layout mismatch -> format (erase all blocks)` **且仅一次**；随后 `[INF] [FTL ] instance ch=0 ce=0 lun=0 free=<F> op=204/204 spare=50/50` |
| **判据** | `op`/`spare` **逐 plane 打印**（v0.74 起）且健康片两个分量都满（204/204、50/50）。若板子已是 v5 介质 ⇒ **不出现**格式化横幅，`free` 与上次一致。 |
| **失败** | 横幅后紧跟 `[ERR] [FTL ] media format failed ch=0 ce=0 lun=0` ⇒ `ftl_media_format` 内 `bbt_erase_all_blocks` / `bbt_save` / `checkpoint_write_initial` 之一失败。回退看 `power_failure_recovery.c` + `ftl_revision_history.md v0.61`。 |

> 格式化耗时十余秒，期间**不要**断电。

---

## 2. 二次上电（**`c1157cd` 的判据 —— 本批最值得盯的回归点**）

> v0.75 首启绿、**第二次上电崩**：`[L2P] read_sync timeout` ×8 + `[L2P] job inflight load=1`
> 每秒重复 + `ASSERT(ftl != NULL)` 崩板。根因是 `completion_table_init()` 在**逐实例**的
> `ftl_init` 里清**全局**路由表 ⇒ 第一个实例就把 `set_route` 抹成 NULL。
> **首启不暴露**是因为 F2 走 `blocks=0` 分支、从不读 L2P；只有已有已关闭 SB 时才触发。

| | |
|---|---|
| **操作** | 步骤 1 完成后**断电** → 重新上电 → 观察串口 |
| **预期** | `[INF] [F2] rebuild blocks=<B> partial=<P> valid_cw=<V>`，且 **`B > 0` 时 `V > 0`** |
| **判据** | ① **`valid_cw > 0`** —— 修复前恒为 0（证明 F2 期的 L2P 加载真的完成并解出了映射）；② **无** `[ERR] [L2P] service: NULL instance (route not set?)`；③ **无** `[L2P] job inflight load=1` 每秒重复；④ **无** `assertion failed: ftl != NULL (src/ftl/checkpoint.c:653)`。 |
| **失败** | `valid_cw=0` 且 `blocks>0` ⇒ 路由仍被抹掉 / L2P 加载不完成。回退看 `main.c` `ssd_ftl_boot_all` ① 与 `ftl.c` `ftl_init`（全仓只有两处写 `g_route`：`completion_table_init` 的 `memset` 与 `set_route`）。 |

> ⚠ 三条 `NULL instance (route not set?)` 是**同一类**隐患的三个观测点：
> `[L2P] service:` / `[RR] service:` / `[FTL] host_io:`。整轮回归里**一条都不该出现**——
> 出现即说明某个服务函数拿到了 NULL 实例（`ftl_op_dispatch` 的实例归属按命令自身
> ch/ce/lun 解析，路由表在**逐实例 boot 之前**就设好了，NULL 只可能是编程错误）。

---

## 3. 冒烟：wr / rd / tr / fl / info / map

> ⚠ **`n` 是扇区数（IU = 512B），上限 32**（`g_wr_buf` = 16KB ÷ 512B）。

| 命令 | 预期输出 |
|---|---|
| `wr 0 8` | `wr submit ok lba=0 n=8, waiting...` → `[INF] wr done lba=0 n=8 status=0` |
| `rd 0 8 c` | `rd submit ok lba=0 n=8 c, waiting...` → `[INF] rd ok n=8, first 64 bytes:` + 4 行 hex → `rd PASS` |
| `tr 0` | `tr submit ok` → `[INF] tr done lba=0 status=0` |
| `fl` | `fl submit ok` → `[INF] fl done status=0` |
| `info` | `[INF] controller state=READY lba_max=1048576`<br>`[INF] ftl[0][0][0] free=<F> op=<O> spare=<S> bad=<B> closed=<C> thrash=0 miss=<M>%` |
| `map 0` | 打印 L2P 映射 + 目标块 BST 条目（只读诊断） |

**判据**：`status=0` / `rd PASS`；`lba_max=1048576`（= 512MiB ÷ 512B）。

**失败**：`rd FAIL at byte <off>: got <g> expect <e>` ⇒ 数据不一致（见 §6/§8）。
`error: n exceeds g_wr_buf (max 32 sectors)` ⇒ n 给大了。

---

## 4. Anchor 落盘 / 跨重启不丢映射

| | |
|---|---|
| **操作** | 观察**每次**开机日志 |
| **预期** | `[INF] [META] anchor load: loc=0 newest=<N> seq=<N>`（`loc=0` 且 `newest != 65535`）<br>`[INF] [META] anchor_seq=<非0> dyn=<非0> cur_blk=<B> live_blks=<L>` |
| **判据** | 写数据 → 断电 → 重启后 `anchor_seq` 应 **≥** 上一轮（单调），`dyn` 非 0。**若 `anchor_seq=0` 且 `dyn=0`，说明映射跨重启全丢。** |
| **失败** | `loc!=0` 或 `newest=65535` ⇒ 池内无有效 record。回退看 `metadata.c` `metadata_dir_load` / `checkpoint.c`。 |
| **⚠ v0.75 专项** | Anchor 落盘现在是 **CP job 的 `CP_ANCHOR_*` 阶段**（阶段 6 拆出的三段式 `metadata_dir_anchor_prepare/_page/_commit`）。判据不变，但失败分支多了两条：**逐页写无论成败都推进 `anchor_chunk`**、**换块擦失败 ⇒ 游标不动**（不重擦）。 |

---

## 5. Wear 表跨重启持久化

| | |
|---|---|
| **操作** | 观察每次开机日志 |
| **预期** | `[INF] [WEAR] seq=<S> pages=<P> max_ec=<M> min_ec=<m>` |
| **判据** | `max_ec` **非 0** 且**随磨损增长**（写 + GC 后二次开机 `max_ec` ≥ 首次）。 |
| **失败** | `max_ec=0` 且 `min_ec=0` ⇒ 回填失败。回退看 `wear.c`。 |
| **前提** | ⚠ `max_ec` 要非 0 **必须先发生过一次真实擦除** ⇒ **先跑 §7 再验 §5**。 |
| **⚠ v0.75 专项** | Wear 现在是**逐页一条命令**的 `wear_job_t`（`cursor` 只在提交成功后推进）。判据新增一条：**擦成功后写提交被 `FTL_AGAIN` 拒时，不得重走换块判定**——否则每轮重擦同一块（磨损放大，可持续上千次）。表现为 `[WEAR] job ... full=1 cursor=0` 心跳里 `cursor` 长期不动而 `phase` 在 ERASE/WRITE 间反复。 |

---

## 6. 断电重启数据仍在（脏环配方）

> ⚠ **2026-10-15 上板订正：原配方「敲几次 `wr 0 32` 把环弄脏」的前提不成立。**
>   ① `journal_append`（`journal.c:143`）对**同一 LBA** 在当前页内**就地合并**——不占新槽位、
>      不推进 `current_entry` ⇒ 反复写同一段 LBA **永远填不满一页**（`JOURNAL_ENTRIES_PER_PAGE = 127`）。
>   ② `journal_kick`（`journal.c:347`）的判据是「`current_entry > 0` 就 freeze」，**不是等页满**，
>      且 IDLE / NORMAL 档**每轮 superloop 都调**（`ftl.c:522` / `:587`）
>      ⇒ **任何一次写之后最多一轮（微秒级），Journal 页就已经落到 NAND 了**，根本不需要「攒几页」。
>   ⇒ 真正决定 `type=1` / `type=2` 的**不是「环脏不脏」，而是断电前有没有让 Checkpoint 跑过**。
>   `CP_TIME_INTERVAL_MS = 10000`（`ftl.h:36`）⇒ **空闲 10 秒就自动落一次 CP**，会把刚写的条目
>   全覆盖掉。**所以配方的真正约束是：写完尽快断电（几秒内，别拖过 10 秒）。**
>
> ⚠ 在刚格式化过的干净介质上直接跑，仍会得到 `type=1 journal_entries=0` + `rd PASS` 的**假绿**
>   （数据靠 Checkpoint 活下来，回放路径根本没被走到）——这是本项要避开的。

| | |
|---|---|
| **操作（脏环配方）** | ① `wr 0 32` → 等 `wr done ... status=0`（数据页确已编程）→ ② **停 ~1 秒**（让 Journal 页落 NAND）→ **直接断电**（⚠ **别超过 10 秒**，否则 CP 会覆盖掉）→ ③ 重新上电 → ④ `rd 0 32 c` |
| **预期** | 重启日志 `[INF] [FTL ] boot type=2 cp_seq=<N> journal_entries=<E>`（`type=2` = `BOOT_POWER_FAILURE`）<br>`rd PASS` |
| **判据** | ① `rd PASS`；② `journal_entries` 应是**本轮实际写量**的量级（`wr 0 32` ⇒ 32）；**若出现 `30×127+1 = 3811` 这类「整圈数」** ⇒ 回放扫到了上一圈遗留的旧页。③ `type=1` = `BOOT_CLEAN_START`，判据是「掉电前恰好有一次 Checkpoint 覆盖了全部已写条目」，**不是**「敲过 `fl`」（`fl` 走 `ftl_flush`，**不调** `checkpoint_create`）。 |
| **对照组（强烈建议做）** | 同样 `wr 0 32`，但**等 15 秒**再断电 ⇒ 应得 `type=1 journal_entries=0`，而 `rd 0 32 c` 仍 `PASS`。**两次一比才证明「`type=1` 的 PASS 确实没走回放」**，即前一次的 `type=2` 是真证据。上板实测：2026-10-15 两条都 PASS。 |
| **失败** | `rd FAIL at byte 0: got 00000000 expect 55555555` ⇒ 掉电一致性破。回退看 `journal.c`（`journal_scan_latest` / `journal_resume`）。 |
| **⚠ v0.75 专项** | Journal 刷页已异步（阶段 2）。**前提不变**：Journal 是 write-back 且本仓**无电容** ⇒ acked 写本就容忍丢一页条目，异步化**不改变掉电模型**。但要盯 `[JRN] job phase=%u waddr=0x%08lx age=%lums` 心跳是否卡在同一 phase。 |

---

## 7. GC 触发 + 搬移异步（阶段 4）

> ⚠ **可达性**：GC 是**纯水位驱动**——`gc_check_triggers` 只看 T1（`free < FREE_POOL_TARGET(32)`）
> / T4（`≤ FREE_POOL_EMERGENCY(8)`）/ T5（磨损比）。空闲板 `free ≈ 3455 ≫ 32` ⇒
> **按设计就不该跑 GC**，上板看不到 `[GC]` 行是**正确行为**。
>
> **手测入口 = 临时抬高水位**（唯一的临时源码改动，验完改回）：
> 1. 上板敲 `info` → 记下 `free=F`（`info` 的 `free` = `ftl->alloc.free_count`，**全局一个数**，`cli.c:521`）。
> 2. 把 `include/ftl/allocator.h` 的 `FREE_POOL_TARGET` 由 `32U` 改成 `F + 2U`
>    （T1 判据是 `alloc->free_count < FREE_POOL_TARGET`，`gc.c:405` ⇒ `F+2` 必然命中，且只差 2 块）。
> 3. `make clean && make all` → 烧 → 复位。验完**改回 `32U`** 并重编。
>
> ⚠ 抬高会**顺带激活** `metadata.c:924` 的目录回收请求（同一个水位宏），测试期可接受，但**别忘改回**。
> ⚠ 该改动**不得提交**（`git status` 应只有 `M include/ftl/allocator.h`）。

| | |
|---|---|
| **操作** | 按上方抬高水位后复位，观察开机日志（**无需敲任何命令**） |
| **预期** | `[INF] [GC] trig=0x01 free=<F'> op=<O> writes=<W> reclaimed=<R> novictim=<N>`<br>`[INF] [GC] victim p0=<b0> p1=<b1> vr=<v> er=<e> cp=<c> yp=<y> cost=<k>` |
| **判据** | ① `trig` 含 **`0x01`**（T1）；② victim 四项 `vr/er/cp/yp` 都参与 cost；③ **`[GC] victim` 行数 = 到水位的差额**（见下方订正，**不是**恒为 1）；④ **`reclaimed` 涨、`novictim` 不变**——这两个是**进展计数**（只增不减）⇒ 一行即可区分「卡死」（`reclaimed` 不动、`novictim` 涨）与「在推进」。 |
| **判据订正（2026-10-15）** | 原写「行数 = 1」**不对**。F2 重建产物是**退化单 plane SB**（`plane_blocks[1] == FTL_BLOCK_NO_PLANE == 65535`，`allocator.h:76`；`close_full_block(..., FTL_TEMP_HOT, ...)`，`power_failure_recovery.c:665`）⇒ 一个 victim **只净归还 1 块**（不是 2）⇒ 差 2 块就需要 **2 个** victim。上板实测：`free=3444`、水位 `3446` ⇒ 恰好 2 行 victim，`free` 精确到 `3446`、`closed` 11→9。`gc.c:368` 注释里的「最多 `TARGET/PLANE_NUM` 行」是按双 plane 写的。 |
| **⚠ 不要这样判** | 拿「`free` 上升」当判据——一次 GC 迭代是「取 2 块目标页 + 还 2 块 victim」，**净变化 ≈ 0**。 |
| **⚠ v0.75 专项** | 搬移已是 `mig_job_t` 状态机（三份同构搬移收敛成一台，`[FTL_OP_GC]` 与 `[FTL_OP_WL]` 填**同一个** `ftl_op_migrate`）。盯 `[MIG] job kind=%u phase=%u page=%u plane=%u aborted=%u age=%lums` 心跳：**`aborted=1`** = 分配不到目标页而中止（**victim 不擦，数据无损**）。三条刻意行为变更：① 分配不到 ⇒ 中止且不擦 victim（原实现会**静默丢数据**）；② 擦除失败**不再退役**；③ 写路径紧急 GC 改**背压**（`FTL_AGAIN`，不再是 `FTL_ERROR`）。 |
| **专项（背压 + 池耗尽）** | 池耗尽 + 持续写 ⇒ 写最终成功、不丢数据、不死锁（验证 OP 预留池兜底）。GC 因池空中止时打 `[MIG] no free block … abort, victim kept`。 |

---

## 8. CW 级映射 + 页打包写缓冲

| | |
|---|---|
| **操作** | ① 单 CW 写：`wr 0 1` → `rd 0 1 c`；② 非整页写：`wr 0 3` → `rd 0 3 c`；③ 触发一轮 GC 后再 `rd`；④ 页打包：`wp <lba> <n> <k>` |
| **预期** | 全部 `rd PASS`；`wp` 打一行 `[INF] [FTL] wbuf flush base=<b> cw=<c> nplanes=<p> owners=0x<m>` |
| **判据** | ① 一次 512B 写 = 1 条 L2P entry + 1 条 journal；② 页内允许空洞；③ GC/WL 逐 CW 读 META 反查 L2P，空洞不误判；④ **`wp` 一次提交 k 组同窗口小写 ⇒ 只打一行 flush、`owners = (1<<k)-1`**（`info` 的 `total_writes` 相对本次只 +1）。 |
| **⚠ `wp` 参数约束** | `n ≥ 1`、`k ≥ 2`、**`n*k ≤ FTL_LBA_PER_PAIR (8)`**、`lba` 8 对齐。合法例：`wp 0 4 2` / `wp 8 2 4` / `wp 16 1 8`。`n*k > 8` ⇒ `error: n*k must be <= 8 (one pair window)`。 |
| **失败** | ③ 在 GC 后 `rd FAIL` ⇒ 逐 CW 有效性/搬移有误。④ flush 行数 ≠ 1 ⇒ 打包失效。 |

---

## 9. 栈哨兵（v0.75 收尾新增诊断）

> `include/common/stack_guard.h` + `src/common/stack_guard.c`。开机把 `[&end, 当时 SP)`
> 整段填 `"STAK"` pattern，superloop 每轮校验。**这是诊断，不是功能**——它的存在是为了
> 让「栈被踩到哪」当场可读，而不是靠事后符号反推。

| | |
|---|---|
| **操作** | 任意时刻敲 `stack` |
| **预期** | `[INF] [STK ] paint=[<lo>,<hi>) size=<N> B  dtcm_bottom=200000f8`<br>`[INF] [STK ] boot_msp=<b> now_msp=<n> peak=<P> B low=<l> headroom=<H> B`<br>`[INF] [STK ] alarm=0x0 hits=0` |
| **判据** | ① 开机有且仅有一行 `[STK ] guard armed paint=[200000f8,<hi>) size=~127000 B`；② **`alarm=0x0`**（跑完全部回归后仍应为 0）；③ `peak` 是自开机起的栈峰值，`headroom = low − dtcm_bottom` 应远大于 0。 |
| **失败** | `alarm` 含 `0x1`（`OVERFLOW`：空闲区底部被踩）或 `0x2`（`MSP`：MSP 落在 `[paint_lo, _estack)` 之外）⇒ 真栈越界，回退看对应的 `[STK ]` LOGE 行（**只打一次**，`hits` 持续增长说明还在踩）。 |
| **⚠ 语义边界** | `peak` 只在**堆没被用过**时才是栈峰值——堆从 `dtcm_bottom` 向上长，同样会抹掉 pattern。`low` 贴着 `dtcm_bottom` ⇒ 那是堆顶不是栈底。另：本模块**只看得见空闲区被踩**；踩坏**已用栈里的保存寄存器**时看不见（debug 构建 `-O0` 处处帧指针 ⇒ 每个函数尾声 `mov sp, r7`，压栈的 r7 被写坏会让 SP 直接变成垃圾值）——那一类要 `-fstack-protector-strong`，见 `stack_guard.c` 尾注。 |

---

## 10. `bbt --erase` + 手动格式化兜底

| | |
|---|---|
| **操作** | `bbt`（只读）→ `bbt --erase`（**破坏性**）→ 断电重启 |
| **预期** | 只读版打印坏块图，与器件出厂标记一致；`--erase` 后重启出现自动格式化横幅（介质被抹 ⇒ 版本校验失败） |
| **判据** | 坏块判据 = 每块首页 spare 首字节（列 2048），非「读失败/全 FF」。`--erase` 后 `free`/`op`/`spare` 回到健康片初值。 |
| **⚠ 两个来源不同（2026-10-15 记录，**待验证**）** | `bbt` 打的是**逐块实时探测**（`bbt_print_bad_map` → `bbt_probe_block_bad`，读 spare 首字节，`bbt.c:283`）；`info` 的 `bad` 是 `alloc.bad_count`（**FTL 自己的 BBT**，`cli.c:526`）⇒ **两者本来就不必相等**。上板观察：用过一段时间的芯片上 `bbt` 标记数（约 17）**多于** `bad=11`，且标记集中在元数据区（0..62）与近期写过的数据块。**假设**：FTL 自己写过的页在 spare 里也有内容 ⇒ 探测无法区分「出厂标记」与「已写过」。**决定性试验**：刚格式化/擦净后立刻 `bbt` + `info`，若两者一致、用过之后才分叉，则假设成立。（与 §13 的 `bad=11` 未确认项可能是同一件事。） |
| **⚠** | `--erase` 会抹掉 FTL 元数据与用户数据，只在专门的坏块体检时用。 |

---

## 11. v0.75 专项：`[JOB]` 心跳与「**静默即健康**」

> 异步化之后，「卡在哪个 phase」不再有阻塞调用点可看 ⇒ 收口补丁给**每个 job** 加了心跳。
> **判据统一为：两次心跳之间 `phase` 不动 ⇒ 卡死。** 空闲板**不打心跳**是正确行为。

| 心跳行 | 出处 |
|---|---|
| `[JRN] job phase=%u waddr=0x%08lx age=%lums` | `journal.c` |
| `[RR] job stage=%u plane=%u attempt=%u retry=%u stall=%u age=%lums` | `read_recovery.c` |
| `[MIG] job kind=%u phase=%u page=%u plane=%u aborted=%u age=%lums` | `migrate.c` |
| `[L2P] job inflight load=%u flush=%u` | `l2p_cache.c`（**只报计数不报 age**） |
| `[CP] job phase=%u wait=%u fail=%u age=%lums` | `checkpoint.c` |
| `[BBT] job phase=%u waiting=%u seq=%lu idx=%u age=%lums` | `bbt.c` |
| `[WEAR] job phase=%u waiting=%u full=%u cursor=%u age=%lums` | `wear.c` |
| `[MD] job phase=%u victim=%u moved=%u reclaim=%u stall=%u age=%lums` | `metadata.c` |

| | |
|---|---|
| **操作** | 全程观察串口；正常负载下应**完全静默** |
| **判据** | ① 空闲 + 正常读写**不出现**任何 `[JOB]` 族行（出现即某个 job 卡住超 1 s）；② `[L2P] job inflight load=1` 每秒重复 = **§2 那个缺陷的签名**，不是正常心跳；③ 各 `stall` 字段持续增长 ⇒ 提交侧被背压卡死（上限 `*_SUBMIT_STALL_MAX (256)` 后 abort）。 |
| **⚠ 心跳永不触发的条件** | 平台时钟缺失（`stm32_platform_register()` 失败）⇒ `ftl_job_now_ms()` 恒 0 ⇒ 心跳打不出来。开机若见 `platform register failed; T3 timers disabled`，本节的判据**全部失效**。 |

---

## 12. 同步入口断言（贯穿机制 2）

| | |
|---|---|
| **操作** | 全程观察 `[ERR] assertion failed:` |
| **预期** | **无** `ftl_op_in_dispatch()` 相关断言 |
| **判据** | `ftl_sync_submit_wait` 首行 `ASSERT(ftl_op_in_dispatch() == 0)`。触发 ⇒ 某个功能**仍在完成服务函数里同步等 NAND**（没 job 化干净）。全仓**不应再有**这种路径：阶段 4 消除 host 写路径紧急 GC、阶段 5 消除完成回调里的 L2P 同步加载、阶段 8 消除完成服务函数里的 metadata 擦块。 |
| **失败** | 断言里带 `ftl_op_in_dispatch` ⇒ 回退看 `ftl_sync_submit_wait` 的调用栈。`ftl_sync_submit_wait` 的**合法**调用者只有结构性顺序路径（开机初始化 / 全片格式化 / 出厂坏块扫描 / 开机回放 / CLI `map`·`bbt --erase`）。 |

---

## 13. 已知未修（**不要**当成本次失败）

| 项 | 说明 |
|---|---|
| **`ftl_confirm_write` 停泊重试** | 阶段 5 起，写完成时若 L2P region 未驻留 ⇒ pending 停泊成 `PENDING_OP_WRITE_CONFIRM`、**组在飞数不递减**，由 `ftl_confirm_kick` 每轮重试。这是设计，不是泄漏。 |
| **`read_recovery` 写保留地址 0x89** | 用户明确不处理。 |
| **`CONFIG_FC_RAID`=0** | RR Stage 3 不实现（边界，**勿翻**）。 |
| **`bbt_kick` 游标未建立时的扫池** | 内含 `ftl_sync_read_page`，但一次性冷启动且**不在完成泵内** ⇒ 有意保留。 |
| **`metadata_dir_reclaim` / `_reclaim_block`** | 仍是同步的（`append_slot` 换块分支的全池 sweep，不在完成泵内）⇒ 有意保留。 |
| **首启 `bad=11` 而 `free=3456`** | 按 `free = 4096 − bad − metadata_good − 508` 反推有 **5 个出厂坏块落在 per-plane 元数据区（块号 0..62）**——`ftl_is_metadata_block` 只按块号预留、**不看坏块位**。次生观察，未确认，与 v0.75 无关。**2026-10-15：与 §10 的「`bbt` 两个来源不同」可能是同一件事，合并看。** |
| **v0.71 那条 MemManage 记录** | `.noinit` 陈旧记录，非本次上电（见 `docs/design/fault_capture.md` 与 §9 的栈哨兵）。**先 `crash` 再 `crash clear`**，顺序不能反。 |

---

## 失败诊断索引

| 现象 | 先看 |
|---|---|
| 首启无格式化横幅 | §1（**若介质已是 v5 则是正常的**） |
| `[F2] rebuild ... valid_cw=0` | **§2 / `main.c` `ssd_ftl_boot_all` ① + `ftl.c` `ftl_init`** |
| `NULL instance (route not set?)` | **§2** |
| `[L2P] job inflight load=1` 每秒重复 | **§2** |
| `anchor_seq=0 dyn=0`（丢映射） | §4 / `metadata.c` |
| `max_ec=0`（Wear 没回填） | §5（**先跑 §7**） |
| `rd FAIL`（静态） | §3 / §8 |
| `rd FAIL`（掉电后） | §6 / `journal.c` |
| `[JOB]` 族行持续 | §11（看 phase 是否在动） |
| `assertion failed: ftl_op_in_dispatch` | §12 |
| `[STK ] alarm=0x1/0x2` | §9 |
| `IO_ERROR` | 记上下文，回退对应阶段的提交 |

---

## 附：观测口速查（逐字取自源码）

| 串口行 | 出处 |
|---|---|
| `[WRN] [FTL ] media layout mismatch -> format (erase all blocks)` | `power_failure_recovery.c` |
| `[INF] [FTL ] instance ch=%u ce=%u lun=%u free=%u op=%u/%u spare=%u/%u` | `src/main.c:232` |
| `[INF] [FTL ] boot type=%d cp_seq=%lu journal_entries=%lu` | `src/main.c:247` |
| `[INF] [F2] rebuild blocks=%u partial=%u valid_cw=%u` | `power_failure_recovery.c:680` |
| `[INF] [META] anchor load: loc=%d newest=%u seq=%lu` | `metadata.c:1834` |
| `[INF] [META] anchor_seq=%u dyn=%u cur_blk=%u live_blks=%u` | `metadata.c:516` |
| `[INF] [WEAR] seq=%u pages=%u max_ec=%u min_ec=%u` | `wear.c:531` |
| `[INF] [L2P] thrash=%u miss=%u%% hit=%lu miss_total=%lu` | `l2p_cache.c:158` |
| `[INF] ftl[%u][%u][%u] free=%u op=%u/%u spare=%u/%u bad=%u closed=%u thrash=%u miss=%u%%` | `src/controller/cli.c:517` |
| `[INF] [STK ] guard armed paint=[%08lx,%08lx) size=%lu B` | `src/common/stack_guard.c` |
| `[INF] [STK ] boot_msp=... now_msp=... peak=... low=... headroom=...` | `src/common/stack_guard.c` |
| `[INF] [FTL] wbuf flush base=%u cw=%u nplanes=%u owners=0x%x` | `ftl.c`（`ftl_wbuf_flush`） |

> 日志级别前缀：`LOGE→[ERR]`、`LOGW→[WRN]`、`LOGI→[INF]`、`LOGD→[DBG]`（`include/common/log.h`）。
> 开机期（`ftl_init` 内）的 `[META]`/`[WEAR]` 是 **INFO**，`log level debug` 在 CLI 里敲、看不到它们。
> ⚠ newlib-nano 只认 `h/l/L` 长度修饰符，**不认 `ll/j/z/t`**（会静默错位）。

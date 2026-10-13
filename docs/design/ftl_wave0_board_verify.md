# 波 0 上板验证清单（v0.59~v0.62 + P0/P1 批次）

> **性质**：`ftl_optimization_plan.md` §九「下一步」第 1 项的可执行清单。
> **背景**：波 0 全部改动（v0.59 CW 级映射 / v0.60 有效性 ③ / v0.61 介质版本 / v0.62 CRC 下沉 +
> P0-1/P0-2/P1-1..P1-6）**代码已推送但从未上板**，且**改了介质格式**（块布局 + 版本标记 + 字段删除）。
> 在本次验证通过之前，§九 的波 2/波 3 全部被阻塞。
> **判据出处**：每项标注对应 ADR（`ftl_revision_history.md`）或 P 项（`ftl_optimization_plan.md` §四）。
>
> ⚠ 本清单**只读串口**，不新增测试源文件（项目规则：测试源用户自建）。
> 所有输出字符串均逐字取自源码，可直接 `grep` 核对。

---

## 0. 前提与构建

```bash
make clean && make all        # 必须 0 warning
```

预期末行附近：

```
AXI_RAM             384 KB    255.0 KB   66.4%
Binary size: 189728 bytes
```

烧录产物：`build/firmware/debug/bin/debug.bin`（无 `make flash` 目标，按既有 SWD 流程烧）。

**上板前须知**：本次介质格式已变 ⇒ **首次上电会自动全片擦除**（约 4096 块 × ~3ms ≈ 十余秒），
**不再需要手动 `bbt --erase`**。`bbt --erase` 仍保留作手动兜底。

---

## 1. 首启自动格式化（v0.61 核心判据）

| | |
|---|---|
| **操作** | 烧 `debug.bin` → 复位 → 观察串口 |
| **预期** | 横幅 + `[SYS ]` 分区之后，出现 **且仅出现一次**：<br>`[WRN] [FTL ] media layout mismatch -> format (erase all blocks)`<br>随后：<br>`[INF] [FTL ] instance ch=0 ce=0 lun=0 free=<F> op=<O> spare=50` |
| **判据** | `ftl_media_needs_format` = 「`checkpoint_find_latest` 失败」（版本校验在 `checkpoint_header_valid` 内）。首次上板介质是旧格式 ⇒ 必中。 |
| **失败** | ① 没打印 ⇒ 介质竟已是 v2 版本（不该）；② 打印了但紧跟 `[ERR] [FTL ] media format failed ch=0 ce=0 lun=0` ⇒ `ftl_media_format` 里 `bbt_erase_all_blocks` / `bbt_save` / `checkpoint_write_initial` 之一失败。回退看 `power_failure_recovery.c:106` 与 `ftl_revision_history.md v0.61`。 |

> 格式化耗时十余秒，期间**不要**断电。

---

## 2. 二次上电不再格式化（v0.61 自终止）

| | |
|---|---|
| **操作** | 步骤 1 完成后断电 → 重新上电 |
| **预期** | **不再**出现 `media layout mismatch` 行；`[FTL ] instance ...` 正常打印 |
| **判据** | 步骤 1 末尾 `checkpoint_write_initial` 已写入当前版本标记 ⇒ 版本判据命中 ⇒ 只格式化一次。 |
| **失败** | 又打印一次 ⇒ 版本标记没落盘（`checkpoint_write_initial` 写失败或读不回）。 |

---

## 3. Anchor 落盘 / 跨重启不丢映射（v0.61 修的 CRC 缺陷 —— 本批最值得盯的回归点）

| | |
|---|---|
| **操作** | 观察**每次**开机日志 |
| **预期** | `[INF] [META] anchor load: loc=0 newest=<N> seq=<N>`（`loc=0` 且 `newest != 65535`）<br>`[INF] [META] anchor_seq=<非0> dyn=<非0> cur_blk=<B> live_blks=<L>` |
| **判据** | 历史缺陷：`ANCHOR_PAGE_CRC_LEN = sizeof-4` 把 `crc` 自己算进覆盖面 ⇒ `anchor_page_valid` **恒 0** ⇒ `metadata_dir_load` 永远失败 ⇒ **每次开机丢 L2P 映射**。v0.61 改 `offsetof(...,crc)`；v0.62 起 Anchor 页已无 `crc` 字段。 |
| **关键** | 写数据 → 断电 → 重启后，`anchor_seq` 应 **≥** 上一轮的值（单调），`dyn` 非 0。**若 `anchor_seq=0` 且 `dyn=0`，说明映射仍跨重启全丢**——即缺陷未修净。 |
| **失败** | `loc!=0` 或 `newest=65535` ⇒ 池内无有效 record（写没落盘 / 读不回）。回退看 `ftl_revision_history.md v0.61`、`metadata.c:485/981`。 |

---

## 4. Wear 表跨重启持久化（P0-1）

| | |
|---|---|
| **操作** | 观察每次开机日志 |
| **预期** | `[INF] [WEAR] seq=<S> pages=<P> max_ec=<M> min_ec=<m>` |
| **判据** | Wear 表落块 62（页式追加日志）。`max_ec` 应**非 0** 且**随磨损增长**（写+GC 后二次开机 `max_ec` ≥ 首次）。 |
| **失败** | `max_ec=0` 且 `min_ec=0` ⇒ 回填失败（等同旧行为「每次上电归零」）。回退看 `wear.c` / P0-1。 |

---

## 5. 冒烟：wr / rd / tr / fl / info

> ⚠ **`n` 是扇区数（IU = 512B），上限 32**（`g_wr_buf` = 16KB ÷ 512B）。CW 级迁移后步进 512B。

| 命令 | 预期输出 |
|---|---|
| `wr 0 8` | `[INF] wr submit ok lba=0 n=8, waiting...` → `[INF] wr done lba=0 n=8 status=0` |
| `rd 0 8 c` | `[INF] rd submit ok lba=0 n=8 c, waiting...` → `[INF] rd ok n=8, first 64 bytes:` + 4 行 hex → `rd PASS` |
| `tr 0` | `tr submit ok` → `[INF] tr done lba=0 status=0` |
| `fl` | `fl submit ok` → `[INF] fl done status=0` |
| `info` | `[INF] controller state=READY lba_max=1048576`<br>`[INF] ftl[0][0][0] free=<F> op=<O> spare=<S> bad=<B> closed=<C> thrash=0 miss=<M>%` |

**判据**：`status=0` / `rd PASS`；`lba_max=1048576`（= 512MiB ÷ 512B，**不再是旧 2GiB 四倍**，v0.59）。

**失败**：`rd FAIL at byte <off>: got <g> expect <e>` ⇒ 数据不一致（见 §6/§10）。
`error: n exceeds g_wr_buf (max 32 sectors)` ⇒ n 给大了。

---

## 6. 断电重启数据仍在（P0-2 + v0.61 联合判据）

| | |
|---|---|
| **操作** | `wr 0 8` → **直接断电**（不敲 `fl`）→ 重新上电 → `rd 0 8 c` |
| **预期** | 重启日志 `[INF] [FTL ] boot type=2 cp_seq=<N> journal_entries=<E>`（`type=2` = `BOOT_POWER_FAILURE`）<br>`rd PASS` |
| **判据** | 掉电回放：Journal 先落（写即入 journal），重启按 `page_seq` 回放 ⇒ L2P 一致。`type=1` = `BOOT_CLEAN_START`（若先 `fl` 再断电）。 |
| **失败** | `rd FAIL` ⇒ 掉电一致性破。回退看 P0-2 与 `ftl_revision_history.md v0.50`。 |

---

## 7. GC 触发 + Free Pool 不流失（P1-1 / P1-2 / P1-3 / P0-2）

| | |
|---|---|
| **操作** | 持续 `wr`（轮转 LBA，写量 > 1024 或空闲 ≥ 500ms）触发 T2/T3 |
| **预期** | `[INF] [GC] trig=0x<XX> free=<F> op=<O> writes=<W>`（`trig` 位见 `gc.h` `GC_TRIGGER_*`：T1=0x01 … T5=0x10，可多位）<br>`[INF] [GC] victim p0=<b0> p1=<b1> vr=<v> er=<e> cp=<c> yp=<y> cost=<k>` |
| **判据** | ① `free` **不单调下降**（GC 擦后归还）；② `trig` 命中位与场景相符；③ victim 的 `vr/er/cp/yp` 四项**都参与** cost（P1-1 四维加权）。 |
| **失败** | `free` 慢性流失 ⇒ 疑似 P0-3（日志块冷 region 卡块，本批**未修**，见 §9）；`IO_ERROR` 出现 ⇒ 记下上下文。 |

---

## 8. thrashing 降级（P1-4）

| | |
|---|---|
| **操作** | 随机 LBA 大范围写（miss 率 > 90%） |
| **预期** | 状态翻转时**一次性**打印 `[INF] [L2P] thrash=1 miss=<M>% hit=<H> miss_total=<T>`；`info` 行 `thrash=1` |
| **判据** | 窗口化 miss 率 + 滞回 90%/70%；降级 = 停 NORMAL 主动 flush。**不是** write-through。 |
| **失败** | 从不翻转 ⇒ 写范围不够大（窗口 `L2P_THRASH_WINDOW=256` 未结算）。 |

---

## 9. v0.62 元数据 CRC 通路（间接覆盖）

| | |
|---|---|
| **操作** | 无独立命令——由 §3/§4/§6 覆盖 |
| **预期** | `FTL_PAGE_CFG_META` 已翻 `FC_CRC_ENABLE`（`ftl_fc_interface.h:317`），元数据读写走 per-CW CRC + ECC |
| **判据** | 若元数据 CRC 校验失败 ⇒ 读侧拿不到有效记录 ⇒ 表现即 §3 的 `loc!=0` / §4 的 `max_ec=0`。**所以 §3/§4 绿 = v0.62 通路通。** |
| **失败** | 见 §3/§4 回退。 |

---

## 10. v0.59 CW 级映射 + 页内空洞（数据路径）

| | |
|---|---|
| **操作** | ① 单 CW 写：`wr 0 1` → `rd 0 1 c`；② 非整页写：`wr 0 3` → `rd 0 3 c`；③ 触发一轮 GC 后再 `rd` |
| **预期** | 全部 `rd PASS` |
| **判据** | ① 一次 512B 写 = 1 条 L2P entry + 1 条 journal；② 页内允许空洞（CW 子集编程），页基址由**首个写过 CW** 反推；③ GC/WL 逐 CW 读 META 反查 L2P（v0.60 有效性 ③），空洞不误判。 |
| **失败** | ③ 在 GC 后 `rd FAIL` ⇒ 逐 CW 有效性/搬移有误（v0.59/v0.60 交叉点）。 |
| **已知代价** | 单 plane 子操作浪费另一 plane 同页（**P2-6**，登记未修，非本批判据）。 |

---

## 11. 已知未修（**不要**当成本次失败）

| 项 | 说明 |
|---|---|
| **P0-3 L2P 日志块压缩** | 冷 region 卡块 ⇒ 碎片化负载下 `free` 会缓慢流失。**本批刻意未修**（依赖 P0-2 先上板验证）。长时压测前才必须补。 |
| **P2-6 单 plane 子操作** | 随机 512B 写浪费另一 plane 页。登记未修。 |
| **`ftl_boot_scan_write_pointer`** | 死代码（主路径不调用），待删。 |
| **`read_recovery` 写保留地址 0x89** | 用户明确不处理。 |
| **`CONFIG_FC_RAID`=0** | RR Stage 3 不实现（边界，勿翻）。 |

---

## 失败诊断索引

| 现象 | 先看 |
|---|---|
| 首启无格式化横幅 | §1 / `ftl_media_needs_format` |
| 每次都格式化 | §2 / `checkpoint_write_initial` |
| `anchor_seq=0 dyn=0`（丢映射） | §3 / `metadata.c:485,981` |
| `max_ec=0`（Wear 没回填） | §4 / `wear.c` |
| `rd FAIL`（静态） | §5 / §10 |
| `rd FAIL`（掉电后） | §6 / P0-2 |
| `free` 慢性流失 | §7 / **P0-3（未修）** |
| `IO_ERROR` | 记上下文，回退对应 ADR |

---

## 附：观测口速查（逐字取自源码）

| 串口行 | 出处 |
|---|---|
| `[WRN] [FTL ] media layout mismatch -> format (erase all blocks)` | `src/main.c:186` |
| `[INF] [FTL ] instance ch=%u ce=%u lun=%u free=%u op=%u spare=%u` | `src/main.c:240` |
| `[INF] [FTL ] boot type=%d cp_seq=%lu journal_entries=%lu` | `src/main.c:253` |
| `[INF] [META] anchor load: loc=%d newest=%u seq=%lu` | `src/ftl/metadata.c:981` |
| `[INF] [META] anchor_seq=%u dyn=%u cur_blk=%u live_blks=%u` | `src/ftl/metadata.c:485` |
| `[INF] [WEAR] seq=%u pages=%u max_ec=%u min_ec=%u` | `src/ftl/wear.c:289` |
| `[INF] [GC] trig=0x%02x free=%u op=%u writes=%lu` | `src/ftl/ftl.c:492` |
| `[INF] [GC] victim p0=%u p1=%u vr=%u er=%u cp=%u yp=%u cost=%lu` | `src/ftl/gc.c:450` |
| `[INF] [L2P] thrash=%u miss=%u%% hit=%lu miss_total=%lu` | `src/ftl/l2p_cache.c:138` |
| `[INF] ftl[%u][%u][%u] free=%u op=%u spare=%u bad=%u closed=%u thrash=%u miss=%u%%` | `src/controller/cli.c:401` |
| `[INF] wr done lba=%lu n=%lu status=%d` / `rd PASS` / `tr done` / `fl done` | `src/controller/cli.c:527-554` |

> 日志级别前缀：`LOGE→[ERR]`、`LOGW→[WRN]`、`LOGI→[INF]`、`LOGD→[DBG]`（`include/common/log.h:39-44`）。
> 开机期（`ftl_init` 内）的 `[META]`/`[WEAR]` 是 **INFO**，`log level debug` 在 CLI 里敲、看不到它们。

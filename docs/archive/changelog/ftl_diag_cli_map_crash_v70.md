# v0.70 — 断言记录是陈旧的：加 `map <lba>` / `crash [clear]` 两条诊断 CLI

**日期**：2026-10-14
**范围**：`src/controller/cli.c`（两条命令 + 命令表）。**纯新增诊断口，不动任何算法路径。**

## 症状

v0.69（`ftl_is_metadata_block` 补 plane 掩码）上板后，重启仍然打印：

```
[ERR] [FAULT] previous assert: b->valid_count > 0U (/home/user/projects/soft_ssd/include/ftl/bst.h:261)
```

## 结论：陈旧记录，不是新崩溃

| # | 证据 | 含义 |
|---|---|---|
| ① | `.noinit` 是 `NOLOAD` 段（`linker/stm32h743xx_flash.ld:144`），启动不清零 | 记录跨**软复位**保留 |
| ② | `crash_record_clear()`（`include/common/crash.h:50`）**无调用者** | 没有路径会清它 ⇒ 一次断言被此后每次复位重放 |
| ③ | 当前树里该断言在 **`bst.h:272`**（v0.69 的 `ftl_gblock_plane/block` 推下去 11 行）；`debug.lst` 里三份 `ftl_block_sub_valid` 全是 `mov.w r2, #272` | 记录里的 `:261` **只能**来自 v0.69 之前的镜像 |
| ④ | 断电重启（`reset reason: POR PIN BOR`）**没有** `previous assert` 行 | POR 清 `.noinit` ⇒ 那是干净基线 |

⇒ `previous assert` 的语义是「上次复位前记录里的东西」，不是「这次启动崩了」。
没有 `crash clear` 之前它会被无限重放，看着像"还在崩"。

## 缺口

用户**不知道如何复现**，也拿不出一次当前代码的崩溃现场。于是把「复现」从必要条件
降成可选项：加两条 CLI 直接读现场。

## `map <lba>`（只读）

打该 LBA 的 L2P 映射 + **目标块**的 BST 条目 + 末行 VERDICT：

```
map lba=100 pba=00027a00 ch=0 ce=0 lun=0 plane=0 blk=317 pg=0 cw=0 g=317
    block g=317 state=0x0004 valid=4 sb=3 ec=12
    VERDICT: ok —— 覆盖写安全
```

- 判据：**L2P 指向某块 ⇒ 至少该 CW 有效 ⇒ `valid_count ≥ 1` 是不变量**。
  `valid_count == 0` 即悬空，与 v0.69 的 `[FTL] dangling` 守卫**同一判据**。
- `state` 一次归因：BAD(0x01)=本不可读 / FREE(0x08)=会被复用**丢数据** /
  RESERVED=判据过宽 / `0x0000`=从未认领。
- 关键性质：**不需要复现崩溃**——某个 LBA 若在写之前就已指向 `valid_count == 0` 的块，
  直接就能看到，且能**事先**指出哪些 LBA 一写就死。
- 固定打实例 `[0][0][0]`（与 `rd`/`wr` 经 controller 路由到的实例一致）。

## `crash [clear]`

查看 / 清除 `.noinit` 记录（`crash_record_get` / `crash_record_clear`）。

- 给一个**显式归零点**：清完再复位，记录重新出现才是真的又崩了。
- ⚠ 掉电（POR）本来就清 `.noinit` ⇒ 断电重启后看到的记录一定是新的，无需 clear。
- 副作用：`crash_record_clear()` 从"有声明无调用者"变为 CLI 可达。
- 子参数**严格**校验（同 `bbt`）：拼错就报错返回，不静默降级为"查看"。
- `CRASH_KIND_FAULT` 时追加打 pc/lr/sp/xpsr/cfsr/hfsr/mmfar/bfar。

## 判据

`make all` **196236 B** / `make tests` **326648 B**，均 0 warning。

上板：
1. `crash` 打出 `bst.h:261` 的完整记录 ⇒ 证实陈旧；`crash clear` 后复位不再出现；
2. `map <lba>` 对写过的 LBA 出 `valid=≥1` + `VERDICT: ok`；
3. 若日后真复现，`map` 与 `[FTL] dangling` 两条线应指向同一个 `g`。

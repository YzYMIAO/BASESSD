# MT29F4G08 原文摘录：Read Cache / Two-Plane Read Cache

出处：Micron `MT29F4G08` 4Gb/8Gb/16Gb x8 NAND datasheet（Rev. N），
"Read Cache Operations" 与 "Two-Plane Read Cache Operations" 两节。
链接见 `docs/reference/README.md`「外部 datasheet」；本文件是**用户 2026-10-31 提供的原文摘录**，
用于 R12「协议断言必须先找到原文」，不在场的推断不得越出以下语句。

## 1. 基本语义

> To increase data throughput, the READ PAGE CACHE series (31h, 00h-31h) commands can be used to
> output data from the cache register while concurrently copying a page from the NAND Flash array to
> the data register.
>
> To begin a read page cache sequence, begin by reading a page from the NAND Flash array to its
> corresponding cache register using the READ PAGE (00h-30h) command. ... After tR ... issue either of
> these commands:
> • READ PAGE CACHE SEQUENTIAL (31h) – copies the next sequential page from the NAND Flash array to the
>   data register
> • READ PAGE CACHE RANDOM (00h-31h) – copies the page specified in this command from the NAND Flash
>   array to its corresponding data register
>
> After outputting the desired number of bytes from the cache register, either an additional READ PAGE
> CACHE series (31h, 00h-31h) operation can be started or the READ PAGE CACHE LAST (3Fh) command can be
> issued. ... 3Fh ... copies the data register into the cache register ... RDY = 1 and ARDY = 1.

## 2. 关键结论：31h 可以跨 block，不能跨 die

> The READ PAGE CACHE SEQUENTIAL (31h) command reads the next sequential page within a block into the
> data register while the previous page is output from the cache register. ...
>
> **The READ PAGE CACHE SEQUENTIAL (31h) command can be used to cross block boundaries. If the READ
> PAGE CACHE SEQUENTIAL (31h) command is issued after the last page of a block is read into the data
> register, the next page read will be the next logical block in which the 31h command was issued.
> Do not issue the READ PAGE CACHE SEQUENTIAL (31h) to cross die (LUN) boundaries. Instead, issue the
> READ PAGE CACHE LAST (3Fh) command.**

⇒ 「cache read 序列只能在一个物理 block 内」**不是器件约束**；跨 block 合法，跨 die/LUN 非法。
FTL 若允许跨 block 续 46h，判据是：下一 pair 在**每个 plane** 上都满足
`block == 上一 pair 的 block + 1` 且 `page == 0`（两 plane 同步前进，见 §3）。

## 3. Two-plane read cache

> Two-plane read cache operations can be used to output data from more than one cache register while
> concurrently copying one or more pages from the NAND Flash array to the data register. This is done by
> prepending READ PAGE TWO-PLANE (00h-00h-30h) commands in front of the PAGE READ CACHE RANDOM (00h-31h)
> command. ...
> • READ PAGE CACHE SEQUENTIAL (31h) – copies the next sequential pages from the previously addressed
>   planes from the NAND Flash array to the data registers.
> • READ PAGE TWO-PLANE (00h-00h-30h) [in some cases, followed by READ PAGE CACHE RANDOM (00h-31h)] –
>   copies the pages specified from the NAND Flash array to the corresponding data registers.

⇒ multi-plane 的 31h 让**每个已寻址 plane** 各前进一页；跨 block 时两 plane 一起跨，因此 FTL 的
连续性判据必须对两 plane 同时成立。

## 4. 与本仓 opcode 的对应

| 本仓 op（`include/nand/onfi_1_0.h`） | 命令 | 地址周期 | 说明 |
|---|---|---|---|
| `OP_READ_PAGE_CACHE_START_SEQ_MP`（45h） | 00h + 两 plane 地址 + 30h | 10 | 起链，带地址 |
| `OP_READ_PAGE_CACHE_SEQ_MP`（46h） | 31h | **0** | 下一页由器件自增；**可跨 block（同 die）** |
| `OP_READ_PAGE_CACHE_LAST_MP`（47h） | 3Fh | 0 | 收尾 + 输出最后一对 |
| `OP_READ_PAGE_CACHE_RANDOM`（00h-31h，单 plane） | 00h + 地址 + 31h | 5 | 跳到指定页；本仓无 two-plane 版实现 |

（opcode 表见 `src/nand/onfi_1_0.c`；45h/46h/47h 的 uop 见同文件
`g_nand_cache_start_seq_mp_uops` / `g_nand_cache_seq_mp_uops` / `g_nand_cache_last_mp_uops`。）

## 5. Program Cache Operations（写侧，用户 2026-10-31 提供）

> The PROGRAM PAGE CACHE (80h-15h) command can be used to improve program operation system
> performance. When this command is issued, the die (LUN) goes busy (RDY = 0, ARDY = 0) while the
> cache register contents are copied to the data register, and the die (LUN) is busy with a program
> cache operation (RDY = 1, ARDY = 0). While the contents of the data register are moved to the NAND
> Flash array, the cache register is available for an additional PROGRAM PAGE CACHE (80h-15h) or
> PROGRAM PAGE (80h-10h) command.
>
> To input a page to the cache register to move it to the NAND array **at the block and page address
> specified**, write 80h to the command register. Unless this command has been preceded by a PROGRAM
> PAGE TWO-PLANE (80h-11h) command, issuing the 80h to the command register clears all of the cache
> registers' contents on the selected target. **Then write n address cycles containing the column
> address and row address.** Data input cycles follow. ... When data input is complete, write 15h to
> the command register.
>
> The PROGRAM PAGE CACHE (80h-15h) command is used as the final command of a two-plane program cache
> operation. It is preceded by one or more PROGRAM PAGE TWO-PLANE (80h-11h) commands.

⇒ **写侧的 cache program 命令每条都自带完整地址**（80h + column/row；two-plane 前面再缀 80h-11h）。
没有"顺序自增"语义，因此**不存在物理连续性约束**：链里下一对可以落在任意 block / page，
跨 block 是天然的，不像读 31h 需要器件自增进位。

## 6. 与本仓写 opcode 的对应

| 本仓 op | 命令 | 地址周期 | 说明 |
|---|---|---|---|
| `OP_TWO_PLANE_PROGRAM_CACHE_FIRST`（49h） | 80h-11h（批首页） | 10 | 带地址 |
| `OP_TWO_PLANE_PROGRAM_CACHE`（44h） | 80h … 15h | 10 | 带地址；中间页只等 tCBSY |
| `OP_TWO_PLANE_PROGRAM_CACHE_END`（48h） | 80h … 10h | 10 | 带地址；末页 |

（opcode 表见 `src/nand/onfi_1_0.c:1025-1027`。写侧的断点因此全部是 FTL 侧决定：
Journal 顺序页配额、Open SB 到头、命令尾——不是器件地址约束。）

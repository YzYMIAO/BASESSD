# v34：FC 单测的 META 改用页 PBA（并修掉 `process_read_mata_error` 的恒真断言）

- **日期**：2026-10-10
- **范围**：`tests/unit/fc_test_common.{c,h}`、`tests/unit/test_fc_level2.c`、
  `tests/unit/test_fc_level3.c`、文档（`fc_nand_ut.md` 等）
- **前置**：v33（CRC 改用 HAL 驱动——**本条 bug 是它修好之后才被暴露出来的**）、
  v24（META 低 3 位 = plane/cw 的编码契约）
- **生产代码**：`src/**` **零改动**。FTL / FC 的生产契约**仍是 LBA**，本轮只动单测的取值。
- **介质格式**：无变更。
- **测试**：`make format-check` 过；`make clean && make tests` **0 warning 0 error**、
  链接 **213132 B**（v33 基线 212180 B）；**上板已验**：`data_pathprocess_read_mata_error`
  与 `data_pathinternal_data_move` 均过，全套 FC UT 0 失败。

## 背景：单测里的 meta 一直叫「扇区 LBA」，其实谁都不是

`tests/unit/` 里每个 CW 的 META 填的是 `(DWORD)page << CW_WIDTH`——注释管它叫
「扇区 LBA」，但它既不是真 LBA（没经过 FTL 分配），也不是真地址，**是夹具自己造的页索引**，
只借了 LBA 的名字。低 3 位留给 plane/cw，高位放页号。

用户的目标：**只在 FC 单测里**换成**页 PBA**（`pba_from_addr`）。PBA 是唯一地址、
可查表，对「验 FC 的机制」已经够用，且不必让单测假装知道 LBA 分配。

夹具只验**机制**——「写进去的 tag 必须能原样读回来」。tag 是 LBA 还是 PBA 对它无差别；
换 PBA 的收益是**让 tag 变成真地址**（块、页、plane、cw 全在数里），比自造的页索引更可读、
更可查表。生产路径的 LBA 语义（`ftl_page_cfg_t.lba` → `command->meta` → 建链
`desc->meta = (command->meta & ~PLANE_CW_MASK) | plane_cw`）**一行未动**。

## 顺带修的既有 bug：`process_read_mata_error` 的篡改被整段擦除

```c
cmd->meta = (DWORD)(TEST_PAGE + 1U) << CW_WIDTH;   /* 0x80 + 4 = 0x84 */
```

`+4` 落在 **bit 2**，而 `PLANE_CW_MASK = 0x7` 的 bit 2 正是 **plane** 位。
`lba_to_meta()` 会把低 3 位掩掉再 OR 回真实 plane/cw ⇒ **这次篡改被整段擦除**，
校验 ② 拿到的期望值与页内 META 相同 ⇒ **MATA_ERROR 永不置位**，断言恒不成立。

它此前一直没暴露，是因为 CRC 恒真（v33 的根因）让整段跑不到这里。宿主程序逐值验证过旧行为：
`cmd_declared->lba_to_meta == page_meta`，逐 CW 相等。

改用 PBA 后此 bug 自然消失——篡改点改到 **block 维**，与低 3 位无关：

```c
cmd->meta = (DWORD)pba_from_addr(&(nand_addr_t){.block = TEST_BLOCK + 1U, .plane = 0U, .page = TEST_PAGE});
```

数值验证（`TEST_BLOCK=100` / `TEST_PAGE=32`，`PLANE_CW_MASK=0x7`）：

```
L2_META=0xC900  mutated=0xCB00
cw=0 stored=0xC900 expect=0xCB00  DIFFER -> MATA_ERROR (OK)   自洽① OK
cw=1 stored=0xC901 expect=0xCB01  DIFFER -> MATA_ERROR (OK)   自洽① OK
cw=2 stored=0xC902 expect=0xCB02  DIFFER -> MATA_ERROR (OK)   自洽① OK
cw=3 stored=0xC903 expect=0xCB03  DIFFER -> MATA_ERROR (OK)   自洽① OK
```

## 关键决策：搬移用例的 Step 5 声明**源页** PBA

**META 存的是「内容的身份」，不是「内容现在在哪」。** 内部搬移（`00h-35h` +
`85h-10h`）都**不是** page read/write，数据路径不介入，源页写入时铸的 META 原样跟到目标块。
所以 Step 5 读**目标块**时，读命令必须**声明源页的 PBA**——它读的正是「从源页搬来的那一页」，
Pass 4 ② 比的也正是「页内 META」对「命令声明的 PBA」。

这与改造前 `page << CW_WIDTH` 的行为**同构**：那个标记本来就是块无关的，源页与目标页自然相等。
改 PBA 只是把这个「块无关」的性质**显式化**。

> ⚠ 附带说明（不阻塞本轮）：PBA 做 tag 时，**任何页重定位都会让该页此后读不过 Pass 4 ②**。
> 生产路径正是靠 LBA 规避这一点。本改动只落在单测——FC 的契约是「写进去的 tag 必须能读回来」，
> 单测提供 tag，调用者能不能凑出这个 tag 是调用者的事。

## 改动

| 文件 | 改动 |
|---|---|
| `fc_test_common.c` | `test_prog_page` / `test_read_cache_step` 的 meta → `pba_from_addr(本页地址)`；**`test_read_page` 加 `meta` 形参**（`test_read_block` 内传本页 PBA） |
| `fc_test_common.h` | 「六、meta 的语义」整段重写为页 PBA 口径；`test_read_page` 新签名 + 搬移用例说明；cache 段文档同步 |
| `test_fc_level2.c` | `L2_META` 改 `pba_from_addr`（15 个使用点不动）；`test_scramble_whiten` 直接用 `addr` 的 PBA 并**删掉 `& ~3`**（该掩码原本是为 `page << 2` 让出 plane/cw 位，PBA 自带低 3 位，理由消失）；修 `process_read_mata_error` |
| `test_fc_level3.c` | `read_page_verify` / 两个内部搬移 op / `RANDOM_DATA_READ` 的 meta 换 PBA；搬移用例 Step 5 读命令声明**源页 PBA** |

**为什么 `test_read_page` 加形参而不是自推**：「命令声明自己期待读到哪一页」正是这个 tag
的全部意义，改造后它就该在调用点可见。全仓只有两个调用点（`test_read_block` 内循环、
搬移用例 Step 5），面很小。`test_prog_page` 保持自推——写路径的 tag 天然来自自己的地址，
没有「外来 tag」场景。

**`test_read_cache_step` 的地址与 tag 刻意解耦**：命令地址页号仍是 `0`（31h / 3Fh
无地址周期），但 tag 必须取**真正输出的那一页**（`output_page`）。

## 不改的

- `src/**` 全部——含 `fc_data_path_meta_check`、建链公式、`gc.c` 的 `FC_META_UNKNOWN` 路径、
  `ftl_fc_interface.h` 的 `ftl_page_cfg_t.lba`。**FC/FTL 生产契约仍是 LBA。**
- `tests/integration/*`（用户的板级测试）。
- `test_cw_seed()`：只读 `desc->meta` 喂 `fc_random_seed_from_meta`（纯哈希），
  输入是 LBA 还是 PBA 都无所谓，零改动。
- `include/nand/nand_config.h`：`lba_to_meta` / `pba_from_addr` 都保留。

## 风险

1. **擦除页**：`read_page_verify(erased=1)` 路径下页内 META 是 `0xFFFFFFFF`，Pass 4 ①
   必然不过、该 CW 带 MATA_ERROR——这是**改造前就有的**性质，判据走
   `test_verify_cw_status_all_success`（查 SUCCESS 位，另一个位），不受 tag 口径影响。
2. **`L2_META` 由宏改成含函数调用**：`pba_from_addr` 是 `static inline`，不是常量表达式
   ⇒ 该宏不能用于静态初始化 / `case` 标签。现有 15 处全是函数实参，安全。
3. **RAID 档未编**：`test_fc_level2.c` 的 `#if CONFIG_FC_RAID` 段用了 `L2_META`，
   其宏定义变了。该段未做编译验证（`CONFIG_FC_RAID` 保持 0，见 v34 同期约定）；
   但全仓 grep 确认测试里已无 `<< CW_WIDTH` / `lba_to_meta` 旧编码，该段只是把
   `L2_META` 当不透明值往下传，宏换实现不影响它。
4. **用例数不变**：本改动不新增 / 删除用例。

## 上板

```
ssd> test /fc-nand/data_pathprocess_read_mata_error   # 本轮修的 bug，逐 CW MATA_ERROR
ssd> test /fc-nand/data_pathinternal_data_move        # 搬移用例，Step 5 按源页 PBA 读回
ssd> test /fc-nand/                                   # 全套
```

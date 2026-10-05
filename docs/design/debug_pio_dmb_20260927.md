# PIO __DMB() 屏障调试记录 — 根因确认与修复

> **调试日期**: 2026-09-27 ~ 2026-09-28
> **涉及模块**: `src/nand/nand_fmc.c` (DATA_IN/DATA_OUT PIO handler)
> **测试命令**: `test /fc-nand/lifecycleinternal_data_move`
> **芯片**: STM32H743II + MT29F4G08ABADAWP NAND
> **现状**: 已修复并验证全链路通过

---

## 问题演变：从 page=63 到全貌

### 2026-09-27 初步定位

仅 `internal_data_move` 测试在 page=63 出错，错误数据 `48 44 04 00`（重复 4 字节模式，前 63 页通过）。Step 2.5 只验证 page 0，错误未被发现。测试流程：

```
1. erase_block(100) + erase_block(102)
2. write_block(100) → 64 pages
3. read_page(100,0) verify → ✅ 仅 page 0
4. 64× internal_data_move (READ+PROGRAM)
5. read_block_verify(102) → ❌ page=63 cw=0
```

### 2026-09-28 全面排查

#### 发现 1: 验证函数有 bug

`expected_byte()` 用 `(block << 8 | page << 4 | plane)` 做 seed，然后 `(uint8_t)` 截断。block 参数进了 seed 高字节后被 uint8_t cast 完全吃掉，不影响结果。

且 `read_block_verify(BLOCK_DEST, 0U)` 传 block=102，而写入时 `write_block(BLOCK_TEST, 0U)` 用 block=100。**碰巧 uint8_t 截断相同**，测试才通过。

修复：改为固定 pattern `0x12345678`（plane 0）/ `0x87654321`（plane 1），`expected_byte(plane, offset)` 直接用 `offset % 4` 取 pattern 字节。

#### 发现 2: Step 2.5 改为全量验证立即暴露 page=1 错误

`read_block_verify(BLOCK_TEST, 0U)` 循环 64 页全部校验 → page=1 CW0 全 `CC`（memset 预填残留），CW1-2 也全 `CC`，CW3 正确（`78 56 34 12`）。

#### 发现 3: CC 模式说明的问题

CW0-2 全 CC 表示 DATA_OUT PIO 循环没覆盖这些 buffer。但 dispatch chaining 代码（`nand_dispatch_poll()` 中 CW chaining 循环 line 384-416）**无法跳过某个 CW 的 DATA_OUT**——PIO 模式下 `nand_fc_start()` 同步完成。CW3 正确说明 chaining 至少到了 CW3。

根本原因不在 dispatch 层，而是**前一步 write_block 的 DATA_IN 写入 NAND 时数据已损坏**。

---

## 根因：DATA_IN PIO 缺少 __DMB()

```c
// nand_fmc_exec_data_in() 中 PIO 路径：
for (index = 0U; index < request->data_length; index++)
{
    *config->data = src[index];  // ← 连续 STRB 到 FMC 数据寄存器
                                 // ← 无 DMB → AXI write buffer 合并
}
```

| 条件 | Step 2.5 | Step 5 | 根因 |
|------|----------|--------|------|
| DATA_IN 无 DMB | ❌ page=1 CW0-2 全 CC | ❌ page=63 全 CC | AXI write merging 损坏写入 |
| DATA_IN 有 DMB | ✅ 全 64 页正确 | ✅ 全 64 页正确 | 每个 STRB 独立抵达 FMC |

### 机制解释

MPU Region 10 已将 0x80000000（FMC NAND Bank3）配置为 Device 内存（`sys.c:26`：TEX=0 / C=0 `NOT_CACHEABLE` / B=0 `NOT_BUFFERABLE` / S=1 `SHAREABLE`，按 ARMv7-M 编码即 **Shareable Device**），ARMv7-M 保证 Device memory 的 store 不合并。但 **STM32H743 AXI bus matrix 内部 write buffer 不受 MPU 控制**——这是该 MCU 的已知设计特性。

> **勘误（2026-10-05）**：原文写作 "Device-nGnRnE（TEX=0, C=0, B=0）"。但 nGnRnE 要求 S=0，而 Region 10 的 `IsShareable` 是 `MPU_ACCESS_SHAREABLE`（HAL 把它写进 `MPU_RASR_S` 位，见 `stm32h7xx_hal_cortex.c:355`），故实际是 **Shareable Device**。两者对"禁止推测访问"的保证相同，不影响本节的结论。

`__DMB()` 通过 drain 所有 pending store，确保每个 STRB 独立到达 FMC 控制器，NAND 正确接收完整写入周期。

### 勘误：DATA_OUT 路径并没有 __DMB()

> **2026-10-05 更正**：原文此处写 "`nand_fmc_exec_data_out()` 在 line 311 已包含 `__DMB()`，所以读方向不需要额外修改"——**这句与事实不符**。
>
> 证据：`git log -S "__DMB" -- src/nand/nand_fmc.c` 只有两个提交——`dbfe745`（唯一的修复提交）往 `nand_fmc_exec_data_in()` 加了**一处** `__DMB()`；`8b7c374` 只给它补了行尾注释。**`nand_fmc_exec_data_out()` 从来没有过 `__DMB()`**，全仓 `grep -rn __DMB src/` 至今也只有 `nand_fmc.c:317` 一处（另加 `:263` 的注释）。
>
> 当时多半是把 DATA_IN 那段（那时行号约 311/317）误看成 DATA_OUT 的了。
>
> **结论不变**（读方向确实不需要屏障），但理由必须重写——见下一节。

---

## 读方向为什么不需要 __DMB()

写方向翻车是"**总线合并 store**"；读方向没有对应机制。分四层看：

| 层 | 写方向 DATA_IN | 读方向 DATA_OUT |
|---|---|---|
| 编译器 | `volatile uint8_t *data`（`nand_fmc.h:68`）已挡住合并/重排 | 同上，已挡住 |
| CPU | Cortex-M7 不重排 Device 访问 | **不推测** Device 读 ← 读侧最大风险被架构禁止 |
| 总线 | ⚠ AXI write buffer 合并 STRB → 需要 `__DMB()` | 总线**不合并读** |
| 缓存 | Region 10 非缓存 | Region 10 非缓存 |

### 读方向的风险清单，逐条为什么被排除

1. **推测读 / prefetch —— 读方向真正的杀手。**
   FMC 数据窗口是 **FIFO**：每读一次，NAND 弹出下一个字节。CPU 若提前多读几个字节（预取），整个数据流就错位了。
   **ARMv7-M 明确禁止对 Device / Strongly-ordered 内存的推测访问**——这是写进架构手册的硬规则，不是"应该没事"。Region 10 是 Device 区，所以推测读在硬件上不可能发生。
2. **读合并。** 合并读会直接改变返回值语义（两次读必须返回两个不同字节），任何总线都不会做。写合并只是延迟/批量化，读合并是改变语义——性质不同。
3. **读乱序。** Cortex-M7 对 Device 内存的访问保序。
4. **陈旧缓存。** Region 10 非缓存。（对比：DMA 路径需要 `nand_dma_cache_invalidate`，那是**另一个**机制——CPU D-Cache 一致性，与总线无关。）
5. **编译器重排。** `volatile` 已挡住。

### 这个论证的边界（诚实说明）

写方向的教训恰恰是"架构手册说应该没问题"被这颗 MCU 的**未文档化总线行为**推翻了。所以不能说"架构上安全 ⇒ 绝对安全"：

- **机制上**：读侧没有已知的对应物。"禁止推测"是硬保证；"总线不合并读"是从语义推的。
- **经验上**：`nand_fmc_exec_data_out` 从项目开始就没有 `__DMB()`，而**当初抓出写方向损坏的那个测试**——`internal_data_move` 的 64 页全量 `read_block_verify`——一直通过。读方向若有对称问题，会以完全相同的方式暴露。
- **但这是弱证据**：写方向当初也是"前 63 页全对，第 63 页才炸"。跑一遍没坏 ≠ 证明安全。

### 如果将来想要保险

给 `nand_fmc_exec_data_out` 的 PIO 循环也加 `__DMB()` 是**语义安全**的——对读循环它只起"排空 pending store"的作用，不会改变读的结果，代价是性能。

代价不小：`CONFIG_NAND_DMA` **默认是 0**（`nand_config.h:87`），也就是说 **PIO 是默认路径，不是回退**。每页 2112 字节 × 一次 `__DMB()`，读方向的开销与写方向同量级——而写方向已经在付这个代价了。

**当前决定：不加。** 保持"写侧有、读侧无"的不对称。
建议（**未执行**）：在 `nand_fmc_exec_data_out` 的 PIO 循环上方留一行注释指向本节，下次有人问同样的问题时代码里就有答案。

---

## 代码改动

```diff
// src/nand/nand_fmc.c: nand_fmc_exec_data_in()
 for (index = 0U; index < request->data_length; index++)
 {
     *config->data = src[index];
+    __DMB();
 }
```

```diff
// tests/unit/test_fc_level3.c: 验证函数修正
-fill_page_pattern(buf, block, page, plane)
+fill_page_pattern(buf, plane)              // 固定 0x12345678 pattern

-expected_byte(block, page, plane, offset)
+expected_byte(plane, offset)               // 去掉 block/page 参数

-verify_page_data(cmd, block, page, plane)
+verify_page_data(cmd, plane)               // 简化签名

-Step 2.5: 只验 page 0
+Step 2.5: read_block_verify(BLOCK_TEST, 0U)  // 全验 64 页
```

---

## 修复后验证

`test /fc-nand/lifecycleinternal_data_move` ✅ PASS (544ms)
- 全 64 页 CW0-CW3 全部 `78 56 34 12` 正确
- 内部搬移后全 64 页一致
- 所有 `[VERIFY]` 输出确认每页每 CW 数据完整

---

## 经验教训

| 教训 | 说明 |
|------|------|
| 校验函数必须有语义 | 旧 `expected_byte` 用 block/page 做 seed 但被 uint8_t 截断 → block 参数完全无效 |
| 写后必须全量验证 | 只验 page 0 掩盖了 page 1~63 的写入损坏 |
| PIO 到 FMC 的**写**方向必须有 DMB | 即使 MPU 配了非缓存 Device 区，AXI write buffer 仍可能合并连续 STRB；**读**方向无对应机制，见"读方向为什么不需要 __DMB()" |
| 不要假设"内部搬移不走 DATA_IN" | 即使 uop 序列没有 DATA_IN，前一步的 write_block 已经写坏了源数据 |
| 文档里的"已验证"也要可追溯 | "DATA_OUT 已有 __DMB()" 是误记，一周后才被 `git log -S` 推翻。写进调试记录的结论要附 commit / 行号，否则后人会去"恢复"一个从来不存在的东西 |

---

## 修订历史

| 日期 | 版本 | 修改内容 |
|------|------|----------|
| 2026-09-27 | v1.0 | 初始版本，page=63 边界条件排查记录 |
| 2026-09-28 | v2.0 | 全面重写：根因确认为 DATA_IN AXI write merging，修正验证函数，全量验证通过 |
| 2026-10-05 | v3.0 | 勘误两处：① DATA_OUT **从未**有过 `__DMB()`（原记录误记，附 `git log -S` 证据）；② Region 10 属性名 Device-nGnRnE → Shareable Device（S=1）。新增"读方向为什么不需要 __DMB()"一节（四层分析 + 风险清单 + 论证边界）。**仅改文档，未动代码** |
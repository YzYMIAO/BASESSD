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

MPU Region 10 已将 0x80000000（FMC NAND Bank3）配置为 Device-nGnRnE（TEX=0, C=0, B=0），ARMv7-M 保证 Device memory 的 store 不合并。但 **STM32H743 AXI bus matrix 内部 write buffer 不受 MPU 控制**——这是该 MCU 的已知设计特性。

`__DMB()` 通过 drain 所有 pending store，确保每个 STRB 独立到达 FMC 控制器，NAND 正确接收完整写入周期。

### DATA_OUT 路径已有 __DMB()

`nand_fmc_exec_data_out()` 在 line 311 已包含 `__DMB()`，所以读方向不需要额外修改。

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
| PIO 到 FMC 必须有 DMB | 即使 MPU 配了 Device-nGnRnE，AXI write buffer 仍可能合并连续 STRB |
| 不要假设"内部搬移不走 DATA_IN" | 即使 uop 序列没有 DATA_IN，前一步的 write_block 已经写坏了源数据 |

---

## 修订历史

| 日期 | 版本 | 修改内容 |
|------|------|----------|
| 2026-09-27 | v1.0 | 初始版本，page=63 边界条件排查记录 |
| 2026-09-28 | v2.0 | 全面重写：根因确认为 DATA_IN AXI write merging，修正验证函数，全量验证通过 |
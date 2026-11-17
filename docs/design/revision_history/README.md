# 设计变更历史（revision_history/）

> 本目录回答「**为什么是现在这样**」，不是当前设计。当前设计以
> [`../ftl_design.md`](../ftl_design.md) 与 [`../fc_nand_design.md`](../fc_nand_design.md) 为准。
>
> 记录口径：历史事实不改写；后续版本的更正写在新条目里；验证记录只写当时结论，
> 不因后续版本通过而追认；被否方案单列，避免重新提议。

| 文档 | 范围 |
|---|---|
| [`ftl_revision_history.md`](ftl_revision_history.md) | FTL：映射 / GC / WL / Journal / Checkpoint / 掉电恢复 / 多 LUN / cache |
| [`fc_nand_revision_history.md`](fc_nand_revision_history.md) | FC + NAND：队列 / 池 / 引擎 / ECC / CRC / 调度 / 时序 |

## 阅读顺序

1. 先看目标文档的 **§1 版本总览**，定位到阶段或版本。
2. 再读 **§2 功能阶段** 或 **§3 版本条目** 的对应条目。
3. 需要方案对比时跳 **§4 专题深挖**；准备提新方案前先读 **§5 被否方案**。
4. 旧符号/旧文件名先查 **§6 旧名→现行名对照**。

## 与其它文档的分工

- 当前设计：[`../ftl_design.md`](../ftl_design.md)、[`../fc_nand_design.md`](../fc_nand_design.md)。
- 缺陷 / 未实现项：[`../known_open_items.md`](../known_open_items.md)。
- 逐提交 diff：[`../../archive/changelog/`](../../archive/changelog/)。

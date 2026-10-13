# FTL 设计

本仓 FTL 的**完整设计**见 **[`docs/design/ftl_design.md`](design/ftl_design.md)** ——
DRAM-less：CW 级映射（IU = 1 CW = 512 B = 页/4）+ SRAM L2P cache（CLOCK）+ Journal/Checkpoint +
GC/静态 WL + 三级水位 + 掉电恢复。

> 本文件此前是「第一阶段采用**页级** L2P 映射」的提纲，**已作废**：数据路径早已迁移到
> **CW 级（IU = 1 CW = 512 B）**，GC / WL / Journal / 掉电恢复均已落地（见上）。

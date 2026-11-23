---
name: basessd-develop-ftl
description: 修改 FTL 层代码的流程与检查清单。用户说「改 FTL」「加个 FTL 功能」「优化 FTL」时使用。仅限 FTL 层；不用于 FC/NAND、平台或纯文档改动。
---

# FTL 改动流程

硬约束见 `.claude/rules/ftl.md`；设计见 `docs/design/ftl_design.md`。

## 0. 动手前

1. **先问「主流主控怎么实现」，并找出处**（§〇）。出处层：
   `docs/reference/README.md`、`docs/design/storage_controller_design.md`、
   SPDK FTL <https://spdk.io/doc/ftl.html>。说不出出处就标「本仓自拟」。
   ⚠ 主流分岔（DRAM 型 vs DRAM-less）—— 说清对齐哪一支。
2. **读设计文档**：`docs/design/ftl_design.md` 对应章节「N.1 / N.2」；
   演进史查 `docs/design/revision_history/ftl_revision_history.md`。
3. **先 grep 已有 helper**（R9 / C2）。

## 1. 实施

- **不变量只走唯一闸门**：细则见 `.claude/rules/ftl.md` / `ftl_design.md`，
  不在本 skill 复制 API 名或字段名（会随重构失效）。
- **断言**：入口一次（H2）；静默介质破坏必须有 release 拒绝（判据 9）；
  有副作用的调用不能进 `ASSERT`（H14）。
- **坐标与语义**：一套坐标（C9）；改语义 / 默认值回扫全部调用点（C10）。
- **批量**：同类改动一次改完；机械改动走脚本；不顺便验证（R8）。

## 2. 验证（R2）

```bash
make all            # debug，零新告警
make release        # 体积 + release 专属告警（判据 10）
make tests UT=FTL   # FTL 单测
```

⚠ **`make tests` 只证链接，不证正确。** 碰了 **建链 / 位图 / 地址** 三类量
**必须上板**（`basessd-verify-board`）。

⚠ 改介质布局抬 `FTL_MEDIA_LAYOUT_VERSION`；首启自动格式化是预期行为，提前告知用户。

## 产物格式

- 改动说明：文件 / 不变量 / 验证证据 / 未上板项。

## 常见失败

- 同一不变量多模块手抄 → 漏账。
- 选块依赖会说谎的状态，而不是池数组。
- 改语义 / 默认值不回扫调用点。
- 编译期无声的量只看编译。

## 完成

- [ ] 新逻辑有没有等价物已经存在？（R9）
- [ ] 新增校验 / 功能有「确实生效过」的证据吗？（C8）
- [ ] 新写的断言能否失败？负例在哪？（C7）
- [ ] 周边注释同步了吗？（H6）
- [ ] 体积口径用对了吗？（C6）
- [ ] 验证状态如实标注了吗？（R1）
- [ ] 未提交、未推送，由用户决定（R3）

- [ ] 唯一闸门 / 一套坐标
- [ ] 新断言有负例
- [ ] 三关零新告警
- [ ] 建链 / 位图 / 地址已上板

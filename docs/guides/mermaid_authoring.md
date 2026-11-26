# Mermaid 图写作约束（GitHub 渲染）

> 本文件只记录 GitHub 渲染环境的约束与排障，不描述任何设计内容。
> 设计文档里的图仍遵循 `docs/README.md` 的 doc policy。

## 渲染环境

- GitHub viewscreen 使用 Mermaid **12.1.0**。
- `securityLevel: antiscript`；`flowchart.diagramPadding: 48`；
  `sequence.diagramMarginY: 40`。

## 写作约束

- 不使用 `click`。
- 不用 `%%{init}%%` 覆盖 `securityLevel` / `maxTextSize`。
- 不用 FontAwesome / Emoji、裸 HTML、反引号。
- 标签统一加引号。
- `subgraph` ID 不要当作节点 / 连线端点。
- `block-beta` 是实验性图种：`columns N` 定列数，`block:num` / `space:num` 跨列；
  整页 DW/Byte 网格用它，发布前必须实际确认 GitHub 渲染。

## 排障

GitHub 报 “Unable to render rich display” 时：

1. 先硬刷新页面或用无痕窗口重试；
2. 再查 `viewscreen.githubusercontent.com` 是否可达；
3. 仍失败时把图缩成最小可复现片段，逐项排除上面的约束。

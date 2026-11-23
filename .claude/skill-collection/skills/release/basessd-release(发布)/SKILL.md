---
name: basessd-release(发布)
description: 发布 / 交付：版本、changelog、许可、构建/CI、打包与回滚。用户说「发布」「开源检查」「打包」「版本」时使用。不用于功能开发、测试设计或故障排查。
---

# 发布 / 交付

## Quick Start
用 basessd-release 准备「<版本>」：先过构建/测试/许可/CI，再出发布清单。

## 入口
- 验证完成，准备交付/公开。
- 已读 `docs/guides/open_source_checklist.md` 与 CI 配置。
- 功能/测试未完成 → 回 `basessd-verify` / develop。

## 工作流
1. 版本与横幅：版本号、镜像时间戳。
2. changelog：只追加，指向提交/版本。
3. 许可：根 LICENSE/NOTICE、vendor 许可、第三方资料。
4. 构建门：`make all` / `make release` / `make tests UT=FC|FTL|HIL`。
5. CI：与 `.github/workflows` 一致；失败不发布。
6. 打包 / 回滚：发布物、回滚方式、证据。
7. 提交 / 推送 / 打 tag：由用户决定（R3）。

## 产物格式
- 发布清单：版本 / 构建证据 / 测试证据 / 许可 / CI / 回滚。
- 未验证项明确列出（R1）。

## 边界
- 测试设计 → `basessd-verify(验证)`。
- 代码改动 → develop；文档 → 根 `docs/`。
- 不做未经用户授权的提交 / 推送 / tag（R3）。

## 命令
`make all` / `make release` / `make tests UT=...`；CI 见 `.github/workflows/`。

## 按需知识
- `docs/guides/open_source_checklist.md`
- `docs/guides/testing_guide.md`
- `docs/reference/README.md`
- `docs/design/known_open_items.md`

## 修订
发布记录只追加；不修改旧 changelog 正文。

## 常见失败

- 只过编译，不跑测试 / 上板。
- 许可与第三方资料未核。
- CI 与本地不一致。
- 未经用户授权提交 / 推送 / tag。

## 完成
构建 / 测试 / 许可 / CI / 回滚五项有证据；未提交/推送由用户决定；未上板项如实标注。

- [ ] 构建 / 测试 / 许可 / CI / 回滚有证据
- [ ] 未验证项如实列出（R1）
- [ ] 提交 / 推送 / tag 由用户决定

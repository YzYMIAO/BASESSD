---
paths:
  - "src/hil/**"
  - "include/hil/**"
---

# HIL 规则（常驻于 HIL 代码路径）

> 契约见 `docs/design/hil_design.md` / `docs/design/usb_msc.md`；
> 改动流程见 `basessd-develop-hil`；CLI 语义见 `docs/design/common/cli.md`。

## 请求与完成

- 一个 host 请求只允许一个拆分/完成闸门；子请求全部完成前不得回 host 成功。
- 超时只解 host 回执（tombstone）；迟到完成不得覆盖 TIMEOUT；slot/entry 必须等 reclaimable 才释放。
- 完成/abort 必须配对；不得静默丢完成项或永久占用槽位。
- bounce buffer 静态；host 输入按不可信处理（H29）。

## USB

- BOT 同一时刻只处理一条命令；deferred error / async completion 的会话代次必须正确。
- mount/umount 推进 session 代次；ISR 不参与应用状态机。

## CLI

- 长命令不得阻塞主循环；需要等待时走 service pump，不在命令内自旋。
- CLI 参数是外部输入：边界校验（H29/H31）；错误返回具名状态。

## 禁止

- host 热路径新增同步 `while (!done)` 等待；必须异步 + 重试/背压。
- 在 ISR 里做 USB/CLI/FTL 事务或日志。

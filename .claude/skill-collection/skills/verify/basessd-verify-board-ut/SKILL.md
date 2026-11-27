---
name: basessd-verify-board-ut
description: 串口 UT 测试：全量 → 父套件 → 剩余单例分层定位，常驻串口、故障复位与临时日志清理。用户说「跑 UT」「串口测试」「全量测试」「单测」「复跑」时使用。不用于烧录/普通上板日志（basessd-verify-board）、测试设计（basessd-verify）或代码审查。
---

# 串口 UT 测试

一次烧录 = 一个串口会话。先全量，再按“倒数第二颗粒度”的父套件复跑，仍失败才单跑后续用例；
串口**只打开一次**，assert 后通过 J-Link 复位，不反复开关串口。

## 0. 串口工具（本机环境）

人工观测（GUI，不脚本化）：

- 位置：`D:\StudyTool\Uart\comtool\comtool.exe`
- 配置：`config.json` — `COM9 / 115200 / 8N1 / XON-XOFF / UTF-8`
- 连接：serial、TCP/UDP client、SSH
- 插件：
  - **收发**：ASCII/hex、自动换行、CRLF、时间戳、定时发送、自定义按键/快捷键、发送历史、日志自动分文件
  - **协议**：Python `encode/decode`，内置 CRC16 / sum
  - **终端**、**图表**
- 日志：`comtool.log`、`run.log`、`comtool_*.log`、`tmp/*.log`
- 自动化前先关闭 GUI（它持有 COM9）：`CloseMainWindow` 即可

自动化：

- 工具：`tools/serial_ut/serial_ut.ps1`（用法见 `tools/serial_ut/README.md`）
- 实现要点：常驻一个 `System.IO.Ports.SerialPort COM9,115200,None,8,One`；
  读缓冲 ≥64 KB、循环 `ReadExisting()`；命令排队发送，读到 summary/prompt 再发下一条
- 本机路径/端口是环境信息，不作为产品依赖；换机器只改工具参数

## 1. 前置

- 已烧 UT 镜像，记录 **banner 时间戳**（镜像身份必须与构建一致）
- **卸掉 Windows 侧 USB 卷**（`mountvol X: /P` 或 diskpart）；否则 CLI 会拒跑：
  `error: host I/O in flight ??? unplug USB / stop host access before 'test'`
- 关闭 comtool；确认 COM9 未被占用
- J-Link GDB server `localhost:2331` 可用（assert 后自动复位）
- 记录 seed（与基线一致，便于对比）

## 2. 串口常驻（硬规则）

- 整个会话只 `Open()` 一次；不要每个用例 open/close：
  固定开销大、DTR 抖动可能复位、缓冲区容易丢数据
- assert 后通过 J-Link 复位，**串口保持打开**；只有 USB CDC 重枚举导致端口消失才重开
- `test --list` 输出可 >4 KB；只读一次会被默认 4 KB 缓冲截断，必须持续读

## 3. 粒度与定位策略

层级（从粗到细）：

```text
/（全量）
  /fc-nand/  /ftl/  /hil/  /bench/
    /ftl/{pure,media,negative}/
      /ftl/negative/{pure,media}/
        /ftl/negative/media/{gc,meta,boot,if}/
          <单例>
```

流程：

1. **先全量**（固定 seed），从头跑；
2. 出问题 → 用**倒数第二颗粒度**的父套件复跑
   （失败在 `/ftl/negative/media/gc/<test>` ⇒ 先跑 `/ftl/negative/media/gc/`），不直接单跑；
3. 父套件仍失败 → 按 `test --list` 的顺序，单跑该父套件中失败点之后的用例，
   收集剩余失败；同一串口会话内连续发；
4. 父套件通过 → 判定为全量顺序/状态相关，记录，不立刻改代码。

约束：

- 单选串长度 ≤ 127 字节（`CLI_LINE_CAPACITY`），超长拆多条；
- 过滤串是前缀匹配；用 `test --list` 拿顺序与完整名单；
- 整包基线有 3 个预期 SKIP：`init_sequence` / `feature_write_read` /
  `chain_priority_lock`，单跑会挂住，**不算失败**；
- 超时分层：普通 30 s；`/ftl/media/` 120–240 s；negative media 300–600 s；
  `/bench/speed/` 600–900 s。

## 4. 失败与卡死

判据：

- `[ERR] abort() called` / `[ASSERT] console active; FTL/FC background frozen`
- 无输出超过该层超时

处理：

- 复位（串口保持打开）：`tools/serial_ut/serial_ut.ps1` 内置调用
  `wsl.exe -e gdb-multiarch -batch -ex 'target extended-remote localhost:2331'
  -ex 'monitor reset' -ex 'monitor go' -ex 'detach'`
- 复位后等 banner/prompt 再发下一条（否则命令会丢在启动窗口里）；
- 记录：用例名、断言原文、seed、镜像 banner；不把“复位成功”当成用例结果。

## 5. 临时日志清理

- 会话结束先写 summary：总数 / OK / SKIP / FAIL / ABORT / TIMEOUT + 失败明细；
- **保留**：summary、失败用例原始日志、复位记录；
- **删除/归档**：每用例临时日志（`%TEMP%\basessd\ut\*.log`、comtool `tmp/*.log`）；
  删除前确认 summary 已落盘；`serial_ut.ps1 -CleanTemp` 做这一步；
- **不删**：`docs/archive/test_logs/` 下的证据、用户自己的 comtool 历史会话日志
  （除非用户明确要求）。

## 6. 收尾

- UT 会破坏介质：`format --verify` + 断电/重启 → 重烧产品 `debug.elf`；
- 失败项按 R11 登记（`docs/design/known_open_items.md`），通过项更新验证级别；
- 证据格式：镜像 banner + seed + summary + 失败断言原文。

## 产物格式

```text
命令：tools/serial_ut/serial_ut.ps1 -Mode <...>
镜像：<banner 时间戳>
seed：<seed>
关键日志：<summary 计数 / 失败断言原文>
结论：通过 / 失败 / 未上板
```

## 常见失败

- 旧 `.noinit` 的 previous crash 记录跨软件复位保留，只有断电才清；先确认不是旧记录；
- assert 后 FTL/FC 后台冻结，串口仍可通，但必须复位才能继续；
- USB 卷未卸 → 大量测试报 `host I/O in flight` 或超时；
- 单跑通过、全量失败 = 状态/顺序相关，优先查测试夹具隔离，不要先改生产代码。

## 完成

- [ ] 镜像 banner / seed 已记录
- [ ] 全量或分层结果完整（OK/SKIP/FAIL/ABORT/TIMEOUT）
- [ ] 失败断言原文与复位记录齐全
- [ ] 临时日志已清理或归档
- [ ] UT 后 `format --verify` + 重烧产品镜像

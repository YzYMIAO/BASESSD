---
name: basessd-verify-board
description: 目标板上板运行：编译、烧写、抓串口日志、判定结果。用户说「上板」「烧一下」「跑一下板子」「看串口」「跑单测」「串口命令」「help」时使用。不用于 host 模拟、代码审查或架构决策。
---

# 上板跑固件

平台唯一：**stm32h743 + MT29F4G08**（R4）。

## 前置

- 测试源由用户自建，不自行发明测试文件（R4）。
- 不主动 `commit` / `push`（R3）；未上板一律标「未上板」（R1）。
- 同一批改动一次编译、一次烧写、一次串口抓取；不重复烧。

## 流程

1. **编译**：`make all` / `make release` / `make tests UT=<FC|FTL|HIL>`；零新告警。
   ⚠ `make tests` 只证链接；碰**建链 / 位图 / 地址**三类量必须上板（R2）。
2. **链路**：`sh/debug_env.sh` → VS Code F5（自动烧写复位）。
   手动分步见 `docs/guides/wsl_debug_environment.md`。
3. **串口**：USART1 115200。**命令表以 `help` / `src/hil/cli.c` 为准**，
   本文不维护副本；参数、开关、过滤串都查 `help`。
   常用入口：`info` / `map` / `crash` / `stack` / `bbt` / `test`。
4. **判定**：先质疑证据（P6 / §3.3）—— 横幅时间戳、`.noinit` 只在断电清、
   SWD 绕过 D-Cache、完整 banner ≠ 没崩、PC 用当场镜像符号解、
   `.bin` 逐字相同在动 include 图时不成立。

## 稳定坑

- `--log-visible debug` 是阻塞式 UART，只对单条用例开；`[ N ms ]` 不可信。
- `test` 过滤串：跑全套不带过滤串；套件名必须带尾斜杠。
- 开卡镜像 `make format-image` 每次上电擦全片；用完换回正常镜像。
- 对象目录按配置分置；切换构建 / UT 无需 `make clean`。

## 产物格式

```text
命令：<make ... / test ...>
镜像：<文件名 + 横幅时间戳>
关键日志：<原文 2~5 行>
计数 / 判据：<free=/bad=/err=/... 或 UT 通过数>
结论：通过 / 失败 / 未上板
```

## 常见失败

- 烧的不是改后镜像。
- 把旧 .noinit 记录当新崩溃。
- SWD 读缓存拿到陈旧值。
- 跑通 ≠ 功能生效。

## 完成

未提交、未推送（R3）；如实报告，没跑的说没跑（R1）。

- [ ] 命令 / 镜像 / 输出 / 计数 / 结论齐全
- [ ] 镜像为改后版本
- [ ] 结论如实（R1）

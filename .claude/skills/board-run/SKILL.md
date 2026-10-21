---
name: board-run
description: 在 stm32h743 目标板上编译、烧写、抓串口日志、判定结果。当用户说「上板」「烧一下」「跑一下板子」「看串口」「验证一下」「跑 UT」时使用。
---

# 上板跑固件

平台唯一：**stm32h743 + MT29F4G08**（CLAUDE.md R4）。不用 host 侧模拟替代。

⚠ 前置纪律：
- **测试源由用户自建**，不要自行发明测试文件（R4）。
- **不主动 `git commit` / `git push`**（R3）。
- 未经上板验证的结论一律标「**未上板**」（R1）。

---

## 1. 编译

| 场景 | 命令 |
|---|---|
| 日常改动 | `make all`（debug，**已前置 `format`**，会自动改盘） |
| 跑 FC 单测 | `make tests UT=FC` |
| 跑 FTL 单测 | `make tests UT=FTL` |
| 体积/告警对比 | `make release` |
| **开卡（破坏性）** | `make format-image`（每次启动擦全片；可加 `FORMAT_VERIFY=1`） |

要求 **零新告警**（既有告警不计）。产物在 `build/firmware/<debug\|release>/bin/`。

⚠ **开卡镜像**：`make format-image` 的产物路径与正常镜像**相同**（`build/firmware/<type>/bin/debug.bin`；
对象目录带 `_fmt` / `_fmt_vf` 隔离），固件在**每次上电**先 `ftl_media_format` 再继续 boot——用于介质账本已坏、
开机 full metadata rebuild 太慢、等不到 CLI 的情况。开卡完成后**必须换回正常镜像**，
否则每次上电都会清数据。

⚠ 对象目录按配置分置（`obj` / `obj_ut` / `obj_ut_FC` / `obj_ut_FTL`），
**切换构建或 UT 无需 `make clean`**。

⚠ **`make tests` 只证链接，不证正确。** 碰了 **建链 / 位图 / 地址** 三类量的改动，
错值在编译期**完全无声**，**必须上板**才能判（R2）。

---

## 2. 起调试链路（WSL → Windows JLink）

```bash
sh/debug_env.sh
```

该脚本做四件事：杀残留进程 → 在 Windows 起 `JLinkGDBServerCL.exe`
（`-device STM32H743II -if SWD -speed 4000 -port 2331 -LocalHostOnly 0`）
→ 在 WSL 起 `socat TCP-LISTEN:2331 → Windows_IP:2331` → 更新 `launch.json` 的 `gdbTarget`。

然后在 VS Code **按 F5**。`launch.json` 的 `preLaunchCommands`
（`monitor reset halt` → `load` → `monitor reset`）会**自动烧录并复位运行**。

手动分步（脚本失败时）见 `docs/design/wsl_debug_environment.md` §4.3。
另一条路：`sh/flash.jlink`（JLink Commander 脚本，`loadfile ... .elf` + `r` + `g`），
⚠ 该文件在 `.gitignore` 里，是本机路径。

---

## 3. 串口 CLI（`src/controller/cli.c` 的命令表）

串口 = **USART1，115200 波特**（`docs/design/fc_nand_ut.md:767` 的实测口径；
本仓**未用 RTT**）。⚠ 读串口的终端工具**本仓文档未记录** —— 不要臆造命令，按用户当前用的方式读。

命令：

| 命令 | 用途 |
|---|---|
| `help` / `version` / `status` | 命令表 / 固件版本 / UART 计数器 |
| `log level <error\|warn\|info\|debug>` | 运行期日志级别 |
| `wr <lba> <n>` / `rd <lba> <n> [c]` | 写/读 n 块（`c` = 比对 pattern） |
| `tr <lba>` / `fl` | trim 一个 lba / flush 脏 L2P + journal 到 NAND |
| `wp <lba> <n> <k>` | 一个 pair 窗口内提交 k 次写（pack-buffer 诊断） |
| `info` | controller + FTL 统计 |
| `map <lba>` | 打印 L2P 映射 + 目标块 BST 条目（只读诊断） |
| `crash [clear]` | 显示/清除 `.noinit` 崩溃记录 |
| `stack` | 栈哨兵：涂色区、峰值用量、告警 |
| `bbt [--erase]` | 打印出厂坏块图 |
| `test [--seed N] [--iterations N] [--list] [--log-visible info\|debug] [name...]` | 跑 munit 套件（**仅 `FC_NAND_TEST_ENABLE` 档有**） |

UT 过滤串坑：
- 跑全套用**不带过滤串**的 `test`；`test /fc-nand/` **一条都不匹配**。
- 套件名**必须带尾斜杠**：`/fc-nand/lifecycle` ❌ / `/fc-nand/lifecycle/` ✅。
- `--log-visible debug` 是阻塞式 UART（≈11.5 KB/s），**只对单条用例开**，且开了之后 `[ N ms ]` 不可信。
- ⚠ 别用 `--log-fatal info`（与 `--log-visible` 共用取值解析，会让每条日志 `abort()`）。

---

## 4. 判定——先质疑证据（CLAUDE.md §3.3 / P6）

**「跑通、零告警、断言全绿」不等于功能生效**（C8）。报「验证通过」前逐条自问：

- **烧的是这个镜像吗？** 判据 = **横幅时间戳**（`__DATE__`/`__TIME__`），不是「刚 load 过」。
- **「又看到同一行崩溃」是又一次崩溃吗？** `.noinit` **只有断电才清** —— 软复位后会重放旧记录。
- **读到的值是 CPU 看到的吗？** SWD 读**可缓存内存**绕过 D-Cache ⇒ 可能读到陈旧值。
- **「能打出完整 banner」= 本次没崩吗？** 不是——故障处理器 `for(;;)` 从不复位。
- **PC 落点**必须用**当场那个镜像**的符号表解。
- **`.bin` 逐字相同**在动了 include 图时不成立。

---

## 5. 收尾

- 改动**未提交、未推送**（R3）；由用户决定。
- 如实报告：命令、串口输出原文、结论；没跑的说没跑（R1）。

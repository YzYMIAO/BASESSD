# serial_ut — 串口 UT runner

配套 skill：`.claude/skills/basessd-verify-board-ut`。

## 串口工具（本机）

- GUI：`D:\StudyTool\Uart\comtool\comtool.exe`
- 配置：`config.json` — `COM9 / 115200 / 8N1 / XON-XOFF / UTF-8`
- 连接：serial、TCP/UDP client、SSH
- 插件：**收发**（ASCII/hex、自动换行、CRLF、时间戳、定时发送、自定义按键、发送历史、
  日志自动分文件）、**协议**（Python encode/decode，内置 CRC16/sum）、**终端**、**图表**
- 日志：`comtool.log`、`run.log`、`comtool_*.log`、`tmp/*.log`
- GUI 持有 COM9；跑 UT 前先关掉（`CloseMainWindow`）

本 runner 直接用 Windows PowerShell 的 `System.IO.Ports.SerialPort`
（`COM9,115200,None,8,One`），不依赖 GUI。

## 用法

```powershell
# 1) 列用例（默认 COM9；结果 >> $env:TEMP\basessd\ut\）
powershell -NoProfile -ExecutionPolicy Bypass -File .\serial_ut.ps1 -Mode list

# 2) 全量（固定 seed）；失败后自动按“父套件 → 剩余单例”收敛
powershell -NoProfile -ExecutionPolicy Bypass -File .\serial_ut.ps1 -Mode full -Seed 0x81a7c024

# 3) 只跑一个父套件
powershell -NoProfile -ExecutionPolicy Bypass -File .\serial_ut.ps1 -Mode suite -Suite /ftl/negative/media/gc/

# 4) 从某个用例之后逐个跑（全局 --list 顺序）
powershell -NoProfile -ExecutionPolicy Bypass -File .\serial_ut.ps1 -Mode after -After /ftl/negative/media/gc/gc_partial_plane_erase_abort

# 5) 单跑
powershell -NoProfile -ExecutionPolicy Bypass -File .\serial_ut.ps1 -Mode one -Test /ftl/pure/mem/pba_roundtrip

# 6) 按名单复跑（同一串口会话；每行一个用例）
powershell -NoProfile -ExecutionPolicy Bypass -File .\serial_ut.ps1 -Mode names -NamesFile .\rerun.txt
```

常用参数：

| 参数 | 默认 | 说明 |
|---|---|---|
| `-Port` / `-Baud` | `COM9` / `115200` | 串口 |
| `-Seed` | `0x81a7c024` | 全量 seed |
| `-LogDir` | `%TEMP%\basessd\ut` | summary / session / 每用例日志 |
| `-IdleMs` | `800` | `--list` 等无 summary 命令的静默判定 |
| `-NoReset` | off | 不调用 GDB 复位（手动复位） |
| `-CleanTemp` | off | 写 summary 后删除每用例临时日志（保留失败项） |

## 实现要点

- **一个会话只打开一次串口**；命令排队发送，读到 `Total wall-clock` /
  abort / 静默才发下一条。
- 读缓冲 1 MB，循环 `ReadExisting()`；避免默认 4 KB 缓冲截断 `test --list`。
- assert / 超时后自动复位：
  `wsl.exe -e gdb-multiarch -batch -ex 'target extended-remote localhost:2331'
  -ex 'monitor reset' -ex 'monitor go' -ex 'detach'`，然后等 `ssd>` 再继续。
- 分层定位：全量 → 父套件（倒数第二颗粒度）→ 该父套件中失败点之后的单例。
- 每用例超时：普通 30 s；`/ftl/media/` 240 s；`/ftl/negative/media/` 600 s；
  `/bench/speed/` 900 s。
- `-CleanTemp` 保留 summary / session / 失败用例日志，删除其余每用例日志。

## 输出

```text
%TEMP%\basessd\ut\
├── summary_<时间>.txt      # 总数 / OK / SKIP / FAIL / ABORT / TIMEOUT + 失败明细
├── session_<时间>.log      # 发送命令 / 结果 / 复位记录
└── <用例名>.log            # 每用例原始串口输出
```

## 限制

- 需要 J-Link GDB server 在 `localhost:2331`（`tools/debug_env.sh`）；
  不满足时用 `-NoReset` 手动复位。
- 板子若以 USB CDC 作为串口，复位/重烧会导致端口消失；这是唯一需要重开串口的场景。
- UT 会破坏介质；跑完 `format --verify` + 断电，再烧回产品镜像。

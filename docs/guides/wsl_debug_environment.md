# WSL 调试环境搭建指南

> **本文档是 VS Code (WSL Ubuntu) 下 STM32H743 项目调试环境的唯一权威参考。**
> 所有环境搭建、工具链配置、链路故障排查，均以本文档为准。
> 遇到任何环境问题，先查本文档，更新也只改本文档。
>
> 最后更新: 2026-10-04 | §5.3 同步为当前 4 步直连脚本；新增「portproxy 残留抢占 2331」故障（§7.2 #8）

---

## 目录

1. [环境概览](#1-环境概览)
2. [一次性环境配置](#2-一次性环境配置)
3. [架构与链路](#3-架构与链路)
4. [每日调试工作流](#4-每日调试工作流)
5. [一键启动脚本](#5-一键启动脚本)
6. [VS Code 调试配置](#6-vs-code-调试配置)
7. [故障排查实录](#7-故障排查实录)
8. [历史问题与解决方案](#8-历史问题与解决方案)
9. [附录](#9-附录)

---

## 1. 环境概览

### 1.1 硬件

| 项目 | 规格 |
|------|------|
| MCU | STM32H743II (2 MiB Flash, 480 KB + 128 KB AXI RAM) |
| 调试器 | J-Link V9 (S/N: 602717758), SWD 接口, 3.27V–3.29V |
| 连接 | USB → J-Link → SWD → 目标板 |

### 1.2 软件

| 组件 | 位置 | 版本 |
|------|------|------|
| **VS Code** | Windows (Remote-WSL 连 Ubuntu) | — |
| **cortex-debug** 扩展 | VS Code 扩展 | v1.12.1 |
| **GDB** | WSL Ubuntu | gdb-multiarch 12.1 |
| **交叉编译器** | WSL Ubuntu | arm-none-eabi-gcc 10.3.1 |
| **socat** | WSL Ubuntu | 由系统包管理器安装 |
| **JLinkGDBServerCL** | Windows `D:\StudyTool\Develop\JLink_V972\` | V9.72 |

### 1.3 目录结构

```
~/projects/soft_ssd/
├── .vscode/
│   └── launch.json                # VS Code 调试配置
├── sh/
│   └── debug_env.sh               # 一键启动脚本
├── docs/guides/
│   └── wsl_debug_environment.md   # ⬅ 本文档
└── build/firmware/debug/bin/
    ├── debug.elf                  # 产品调试 ELF（make all）
    └── debug_ut.elf               # UT 调试 ELF（make tests）
```

---

## 2. 一次性环境配置

> 以下配置**只需执行一次**，除非重装系统或工具链升级。

### 2.1 WSL Ubuntu 工具链

```bash
# 交叉编译工具链
sudo apt install -y gcc-arm-none-eabi binutils-arm-none-eabi

# GDB（多架构支持）
sudo apt install -y gdb-multiarch

# socat（端口转发）
sudo apt install -y socat

# 验证安装
which gdb-multiarch        # → /usr/bin/gdb-multiarch
which arm-none-eabi-gcc    # → /usr/bin/arm-none-eabi-gcc
which socat                # → /usr/bin/socat
```

### 2.2 Windows 防火墙规则（已不再需要）

> **2026-09-26：直连方案不需要防火墙规则。WSL 虚拟网卡和 Windows 在同一 Hyper-V 虚拟交换机上，不受 Windows 防火墙入站规则影响。** 保留此节供历史参考。

以**管理员身份**打开 CMD 或 PowerShell，执行：

```cmd
netsh advfirewall firewall add rule name="JLink 2331" dir=in action=allow protocol=TCP localport=2331
```

确认输出：`Ok.`

### 2.3 Windows 端口转发（portproxy）— ⚠️ 已废弃，且必须删除残留

> **2026-09-26：已切换到 `-LocalHostOnly 0` 直连方案，不再需要 portproxy。保留此节仅作历史参考。**

> **⚠️ 若曾按旧方案配置过 portproxy，务必删除这条规则。** 它会让 Windows 内核（svchost）持续监听 `0.0.0.0:2331`，与新方案 JLink（`-LocalHostOnly 0` 也绑 `0.0.0.0:2331`）**抢占同一端口**：JLink 绑不上、启动即退出（`Failed to open listener port 2331`），脚本会卡在「等待 JLink 进程」直到 30 秒超时。删除（管理员 PowerShell，一次即可）：
>
> ```powershell
> netsh interface portproxy delete v4tov4 listenport=2331 listenaddress=0.0.0.0
> netsh interface portproxy show v4tov4      # 确认 2331 那条已消失
> ```

以**管理员身份**执行一次，重启后持续生效（**旧方案，勿再使用**）：

```cmd
netsh interface portproxy add v4tov4 listenport=2331 listenaddress=0.0.0.0 connectport=2331 connectaddress=127.0.0.1
```

#### 为什么要 portproxy？（历史背景）

JLinkGDBServer 默认只绑定 `127.0.0.1`（不接受外部连接）。portproxy 是 Windows 内核级 NAT，把 `0.0.0.0:2331` 的入站流量转发到 `127.0.0.1:2331`。

2026-09-26 之前这是唯一方案。`-LocalHostOnly 0` 发现后该方案已废弃。

### 2.4 VS Code 扩展

在 VS Code (WSL Remote) 安装 **Cortex-Debug** 扩展。

---

## 3. 架构与链路

### 3.1 完整数据流（直连方案）

**JLink 绑定 `0.0.0.0:2331`（所有网卡），WSL socat 直连到 Windows IP，完全绕过 portproxy。**

```
┌─ WSL (Ubuntu) ────────────────────────────────────────────────────┐
│                                                                    │
│  VS Code (WSL Remote)                                              │
│    └─ cortex-debug 扩展                                             │
│         └─ gdb-multiarch                                           │
│              └─ target extended-remote localhost:2331               │
│                   ↓                                                │
│  socat TCP-LISTEN:2331                                             │
│    └─ 转发到 TCP:Windows_IP:2331                                   │
│       (即 172.29.32.1:2331)                                        │
└───────────────────────────┬───────────────────────────────────────┘
                            │ TCP 直连 (WSL eth0 → Windows vEthernet)
                            ▼
┌─ Windows ─────────────────────────────────────────────────────────┐
│                                                                    │
│  JLinkGDBServerCL.exe (0.0.0.0:2331, -LocalHostOnly 0)            │
│    └─ 绑定所有网络接口，接受来自 WSL 的直接连接                      │
│                                                                    │
│  USB → J-Link V9 → SWD → STM32H743II                               │
└───────────────────────────────────────────────────────────────────┘
```

### 3.2 历史方案（已废弃）

> **portproxy + WSL2 内建转发方案已废弃（2026-09-26）。保留此节作为历史参考，不再使用。**

之前的双层 fallback 方案依赖 portproxy（Windows 内核 NAT）和 WSL2 内建转发 (wslrelay)，但 portproxy 反复出现 zombie rule 失效，WSL2 内建转发也同步不可用。详见 `docs/archive/wsl_debug_history.md` §8.11。

### 3.3 端口分配

| 端口 | 用途 | 绑定 |
|------|------|------|
| 2331 | GDB 调试端口（主端口） | JLink: `0.0.0.0`（`-LocalHostOnly 0`） |
| 2332 | SWO 原始输出端口（JLink 自动分配，非 GDB） | `127.0.0.1` |
| 2333 | 终端 I/O 端口（JLink 自动分配） | `127.0.0.1` |

> ⚠️ **重要**：JLink 绑定的 2332 端口是 SWO 输出端口，不是 GDB 退端口。脚本自动检测到 2332 时不要误以为是 JLink 退端口。

### 3.4 关键参数说明

- **`-single`**: JLink V9.72 不识别此参数（Warning，但忽略后不影响功能）
- **JLink 绑定地址**: JLinkGDBServerCL 带 `-LocalHostOnly 0` 时绑定 `0.0.0.0:2331`（所有网络接口），WSL 可以直接通过 Windows 虚拟网卡 IP 连接。不再需要 portproxy。
- **`-LocalHostOnly 0`**: 让 JLink 接受来自非 localhost 的连接。默认值为 1（仅 localhost）。
- **`servertype: "external"`**: VS Code 不自动启动 GDB server，等已有的连接
- **`armToolchainPath: "/usr/bin"`**: 消除 cortex-debug `nm-multiarch` / `objdump-multiarch` ENOENT 警告

---

## 4. 每日调试工作流

### 4.1 完整流程（推荐）

```bash
# 终端：一键启动调试环境
cd ~/projects/soft_ssd
./sh/debug_env.sh
# 会自动：杀残留 → 启动 JLink(Windows弹出窗口) → 启动 socat → 验证链路

# VS Code：按 F5 开始调试（按所选配置烧对应产物）
#   → debug 配置烧 debug.elf / debug_ut.elf → 停在 main()
#   → release 配置只烧录/运行（strip 后无符号，不停 main）
#   → 按 F5 继续运行
```

### 4.2 重启后恢复（最重要）

Windows 重启后，**只需两步**：

```bash
# 第 1 步：一键启动（自动清理旧进程 + 启动 JLink + socat）
cd ~/projects/soft_ssd
./sh/debug_env.sh

# 第 2 步：VS Code 按 F5 烧录调试
```

不需要手动启动 JLink，不需要配置 portproxy（它是系统级配置，重启后自动保留）。

### 4.3 快捷操作（手动分步执行）

如果你想自己分步操作（不依赖脚本）：

```bash
# 1. Windows 手动启动 JLinkGDBServer（保持窗口可见）
#    PowerShell:
#    & "D:\StudyTool\Develop\JLink_V972\JLinkGDBServerCL.exe" -device STM32H743II -if SWD -speed 4000 -port 2331 -LocalHostOnly 0

# 2. WSL 启动 socat
WINDOWS_IP=$(ip route show default | awk '{print $3}')
socat TCP-LISTEN:2331,reuseaddr,fork TCP:$WINDOWS_IP:2331 &

# 3. VS Code F5 调试
```

### 4.4 运行单元测试

```bash
# 全跑三级 UT（**不加过滤串**才是全套）
test

# 分层运行（套件名**必须带尾斜杠**）
test /fc-nand/logic/          # Level 1（11 条：command + queue + cqueue + scheduler）
test /fc-nand/data_path/      # Level 2
test /fc-nand/lifecycle/      # Level 3（地址随机化：--iterations N 是覆盖旋钮）

# 其他
test --list                    # 列出所有套件
test --seed 0x1234             # 指定随机种子

# 证据打印：开了才打「这条用例证明了什么」（默认档输出逐字不变）
test --log-visible info                            # 每用例 1-3 行 [EV] 摘要
test --log-visible debug /fc-nand/lifecycle/block_write_read   # 单条用例逐页明细
test --log-visible error                            # 改回默认（可见级跨 test 调用残留）
```

> ⚠ **`/fc-nand/` 这个根套件名单独用不了**——`test /fc-nand/` **一条都不匹配**
> （过滤串比子套件路径短，`strncmp` 撞上 `'\0'` 即判不等），跑全套请用**不带过滤串**的 `test`。
> 套件名也**必须带尾斜杠**（`/fc-nand/lifecycle` ❌ / `/fc-nand/lifecycle/` ✅）。

> ⚠ 别用 `--log-fatal info`——它与 `--log-visible` 共用取值解析，会让每条日志 `abort()`。
> `debug` 档是阻塞式 UART 输出（≈11.5 KB/s），只对**单条**用例跑；开证据后 `[ N ms ]` 不再可信。
> 细节见 `fc_nand_ut.md` §5.5。

### 4.5 修改代码后

```bash
make tests    # 重新编译
# VS Code 按 F5（或 Restart）重新烧录
```

### 4.6 目标板掉电/上电恢复

如果调试过程中目标板断电（或手动复位后 JLink 连接异常）：

1. **JLink 窗口显示 `Target voltage: 0.00V` 或 `Connection lost`** → 无需立即操作，JLink 在电压恢复后会自动重连（等待 3-5 秒）
2. **JLink 窗口卡死或显示异常** → `./sh/debug_env.sh` 重新启动 JLink + socat
3. **JLink USB 指示灯异常（不亮/常闪）** → 拔插 J-Link USB，再跑脚本
4. **GDB 烧录失败报 `vMustReplyEmpty: timeout`** → 跑脚本重启链路，然后 F5
5. **始终**：跑脚本 → F5。`preLaunchCommands` 中的 `monitor reset halt → load → monitor reset` 会自动处理 MCU 未知状态

> 注意：JLink 在掉电后 MCU 电压恢复时会自动重连目标，不需要每次断电都重启 JLink。

---

## 5. 一键启动脚本

### 5.1 脚本位置

`sh/debug_env.sh`

### 5.2 功能

该脚本自动完成以下 4 步：

| 步骤 | 操作 | 说明 |
|------|------|------|
| 1 | 全清理 | 杀 socat → 杀 JLink，清除残留进程 |
| 2 | 启动 JLink | 在 Windows 弹出 JLinkGDBServer 窗口（带 `-LocalHostOnly 0`），等待绑定 `0.0.0.0:2331` |
| 3 | socat 直连 | socat → `Windows_IP:2331`，然后 `/dev/tcp` 验证 |
| 4 | 更新 launch.json | 确保 gdbTarget 指向 localhost:2331 |

> **关于 portproxy**：脚本不再管理 portproxy。portproxy 是 §2.3 中配置的永久系统级 NAT 规则，重启后持续生效，与 JLink 在同一端口 2331 上共存（内核 NAT + 应用层 socket，不同层级不冲突）。

### 5.3 脚本内容（当前版本，2026-10-04）

> 与仓库中的 `sh/debug_env.sh` **保持一致**；改动脚本后请同步本节。
> 注意：历史版本（含 portproxy / WSL2 内建转发双层 fallback 的 6 步版）已废弃，勿再参考。

```bash
#!/bin/bash
# debug_env.sh — 一键启动 WSL → Windows JLinkGDBServer 调试环境
#
# 架构（直连方案，无 portproxy）:
#   GDB → localhost:2331 → socat → Windows_IP:2331 → JLink(0.0.0.0:2331)
#
#   JLink 用 -LocalHostOnly 0 绑定 0.0.0.0:2331（所有接口），
#   socat 直接把 WSL localhost 的流量转发到 Windows IP，
#   完全绕过 portproxy 或 WSL2 内建转发。
#
# 前置条件（只需一次）:
#   无。不再依赖 portproxy / 防火墙规则 / WSL2 内建转发。
#
# 历史教训:
#   - 2026-09-26: portproxy zombie rule 反复出现（`docs/archive/wsl_debug_history.md` §8.7/§8.9/§8.10），
#     IP Helper 重启重建仍无法恢复。改用 -LocalHostOnly 0 直连后一次成功。
#     → 彻底移除 portproxy/WSL2 内建转发两条 fallback 路径
#   - 2026-09-25: nc -zv localhost 只能测 socat listen 端口，不能测转发是否可达
#     → 先 socat 再 /dev/tcp 验证
#   - 2026-09-25: netstat 看到 0.0.0.0:2331 是 svchost(portproxy) 不是 JLink
#     → Step 1 不再暴力杀 0.0.0.0:PORT 的进程

JLINK_EXE="D:\\StudyTool\\Develop\\JLink_V972\\JLinkGDBServerCL.exe"
DEVICE="STM32H743II"
PORT=2331
WINDOWS_IP=$(ip route show default | awk '{print $3}')

echo ""
echo "============================================"
echo "  WSL -> J-Link 调试环境启动脚本"
echo "============================================"
echo "  设备     : $DEVICE"
echo "  端口     : $PORT"
echo "  Win IP  : $WINDOWS_IP"
echo "============================================"
echo ""

# ========== Step 1: 清干净所有旧进程 ==========
echo "[1/4] 全清理旧进程..."

# 杀 WSL 侧的 socat（避免端口占用）
pkill -9 -f "socat.*$PORT" 2>/dev/null || true
sleep 1

# 杀 Windows 侧的 JLinkGDBServer（确保下次启动绑定正确的端口 + -LocalHostOnly 0）
powershell.exe "taskkill /F /IM JLinkGDBServerCL.exe" 2>/dev/null || true
sleep 2

echo "  所有旧进程已清理"

# ========== Step 2: 启动 JLink（-LocalHostOnly 0 直接绑定 0.0.0.0）==========
echo "[2/4] 启动 Windows JLinkGDBServer..."

echo "  $JLINK_EXE -device $DEVICE -if SWD -speed 4000 -port $PORT -LocalHostOnly 0"
# -LocalHostOnly 0: JLink 绑定 0.0.0.0（所有网络接口），
#                   不再需要 portproxy / WSL2 内建转发
powershell.exe -Command "Start-Process -WindowStyle Normal -FilePath '$JLINK_EXE' -ArgumentList '-device $DEVICE -if SWD -speed 4000 -port $PORT -LocalHostOnly 0'" > /dev/null 2>&1 || true

echo "  等待 JLink 绑定端口 $PORT..."
JLINK_READY=false
for i in $(seq 1 30); do
    sleep 1
    jpid=$(powershell.exe "Get-Process JLinkGDBServerCL -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id" 2>/dev/null | tr -d '\r')
    if [ -z "$jpid" ]; then
        echo "  等待 JLink 进程... (${i}s)"
        continue
    fi
    # JLink 带 -LocalHostOnly 0 时会绑定 0.0.0.0:PORT
    # 先查 0.0.0.0，再查 127.0.0.1（兼容旧 JLink 版本）
    bind_info=$(powershell.exe "netstat -ano | findstr '0.0.0.0:$PORT ' | findstr LISTENING" 2>/dev/null | head -1 | tr -d '\r')
    if [ -z "$bind_info" ]; then
        bind_info=$(powershell.exe "netstat -ano | findstr '127.0.0.1:$PORT ' | findstr LISTENING" 2>/dev/null | head -1 | tr -d '\r')
    fi
    if [ -n "$bind_info" ]; then
        echo "  JLink 已绑定 (${i}s)"
        JLINK_READY=true
        break
    fi
    echo "  JLink 进程存在，等待绑定... (${i}s)"
done

if [ "$JLINK_READY" = false ]; then
    echo "  自动启动失败，请手动打开 Windows -> 启动 JLinkGDBServer:"
    echo "     & \"$JLINK_EXE\" -device $DEVICE -if SWD -speed 4000 -port $PORT -LocalHostOnly 0"
    echo "  启动后按回车键继续..."
    read -p "  按回车键继续..."
fi

# ========== Step 3: socat 直连 Windows_IP ==========
echo "[3/4] 启动 socat 隧道（直连 Windows_IP）..."

pkill -9 -f "socat.*$PORT" 2>/dev/null || true
sleep 1

echo "  启动 socat: localhost:$PORT -> $WINDOWS_IP:$PORT"
socat TCP-LISTEN:$PORT,reuseaddr,fork TCP:$WINDOWS_IP:$PORT &
SOCAT_PID=$!
sleep 2

# 最终验证: 用 /dev/tcp 做真实连接测试
# nc -zv localhost 只能测 socat 的 listen fd 是否打开，
# 不能测 socat fork 出的子进程能否连上下游（2026-09-25 `docs/archive/wsl_debug_history.md` §8.8 教训）。
# /dev/tcp 是 bash 内建的 TCP 连接，模拟 GDB 的真实连接行为。
if timeout 3 bash -c "echo >/dev/tcp/localhost/$PORT" 2>/dev/null; then
    echo "  socat 运行中 (PID: $SOCAT_PID, localhost:$PORT -> $WINDOWS_IP:$PORT)"
else
    echo "  socat 启动了但连接测试失败。请检查:"
    echo "     1. JLinkGDBServer 是否在 Windows 运行（带 -LocalHostOnly 0）"
    echo "     2. WSL 到 Windows 网络: nc -zv $WINDOWS_IP $PORT"
    echo "     3. 如不通，手动启动 socat 后按 F5:"
    echo "        socat TCP-LISTEN:$PORT,reuseaddr,fork TCP:$WINDOWS_IP:$PORT &"
fi

# ========== Step 4: 更新 launch.json ==========
echo "[4/4] 更新 launch.json..."
LAUNCH_FILE="$(cd "$(dirname "$0")/.." && pwd)/.vscode/launch.json"
if [ -f "$LAUNCH_FILE" ]; then
    sed -i "s/\"gdbTarget\": \"localhost:[0-9]*\"/\"gdbTarget\": \"localhost:$PORT\"/" "$LAUNCH_FILE"
    echo "  gdbTarget -> localhost:$PORT"
fi

echo ""
echo "============================================"
echo "  调试环境就绪！请按 F5 开始调试"
echo "============================================"
echo ""

# 捕获 Ctrl+C 只杀 socat，不影响 Windows 上的 JLink 窗口
trap "echo '正在退出（不影响 JLink 窗口）'; kill $SOCAT_PID 2>/dev/null; true" INT TERM
wait $SOCAT_PID
```

---

## 6. VS Code 调试配置

### 6.1 launch.json

实际配置见 `.vscode/launch.json`（8 条 = 4 种产物 × 外部 / WSL 自启两种起法）：

| 编号 | 用途 | 产物 | 构建命令 |
|---|---|---|---|
| ①② | UT / 逻辑调试 | `build/firmware/debug/bin/debug_ut.elf` | `make tests UT=...` |
| ③④ | 产品调试 | `build/firmware/debug/bin/debug.elf` | `make all` |
| ⑤⑥ | 发布 / 速度（UT） | `build/firmware/release/bin/release_ut.elf` | `make tests BUILD_TYPE=release UT=...` |
| ⑦⑧ | 发布（产品） | `build/firmware/release/bin/release.elf` | `make release` |

奇数号 = 外部 JLink（先跑 `sh/debug_env.sh`，连 `localhost:2331`）；偶数号 = VS Code
通过 `serverpath` 自启 Windows JLink。代表配置（① UT 调试）：

```json
{
  "name": "① UT/逻辑调试 · debug_ut · 外部 JLink 2331",
  "cwd": "${workspaceFolder}",
  "type": "cortex-debug",
  "request": "launch",
  "servertype": "external",
  "device": "STM32H743II",
  "gdbTarget": "localhost:2331",
  "executable": "${workspaceFolder}/build/firmware/debug/bin/debug_ut.elf",
  "gdbPath": "/usr/bin/gdb-multiarch",
  "runToEntryPoint": "main",
  "preLaunchCommands": ["monitor reset halt", "load", "monitor reset"],
  "svdFile": "${workspaceFolder}/STM32H743xI.svd",
  "armToolchainPath": "/usr/bin"
}
```

⚠ release 产物链接时带 `-s`（剥符号），不能设 `runToEntryPoint` / 打断点；
`load` 只依赖 program header，烧录本身正常。

### 6.2 关键字段说明

| 字段 | 值 | 说明 |
|------|------|------|
| `servertype` | `"external"` | 不自动启动 GDB server |
| `gdbTarget` | `"localhost:2331"` | 通过 socat 隧道连 JLink |
| `gdbPath` | `"/usr/bin/gdb-multiarch"` | WSL 上的 GDB（非 arm-none-eabi-gdb） |
| `armToolchainPath` | `"/usr/bin"` | 让 cortex-debug 找到 arm 工具链 |
| `runToEntryPoint` | `"main"` | 停在 main() 入口 |
| `preLaunchCommands` | `reset halt → load → reset` | 自动烧录并复位运行 |
| `executable` | `${workspaceFolder}/build/firmware/<type>/bin/<artifact>.elf` | 按用途选 `debug_ut.elf` / `debug.elf` / `release_ut.elf` / `release.elf` |

---

## 7. 故障排查实录

### 7.1 快速诊断流程

遇到烧录失败时，按顺序排查：

```
[1] JLink 硬件连接
  → Windows 上 JLinkGDBServer 窗口显示什么？
    - "Connecting to J-Link..." → USB/驱动问题
    - "J-Link is connected." → ✅
    - "Failed to open listener port 2331" → 端口被占（可能之前的 JLink 未杀掉）
    - "Waiting for GDB connection..." → ✅

[2] WSL→Windows 网络
  → nc -zv -w4 172.29.32.1 2331
    确认 Windows 的 JLink 端口从 WSL 可达

[3] socat 隧道
  → nc -zv localhost 2331
    确认 socat 在 WSL 端监听

[4] GDB 直连测试
  → gdb-multiarch -q -batch \
      -ex "target extended-remote localhost:2331" \
      -ex "monitor halt" \
      -ex "info registers r0" \
      build/firmware/debug/bin/debug.elf
```

### 7.2 已知问题与对策

| # | 症状 | 原因 | 解决方案 |
|---|------|------|----------|
| 1 | `Failed to launch GDB: Remote connection closed` | JLinkGDBServer 未运行 或 socat 隧道断开 | 检查 JLink 窗口状态；重启 socat |
| 2 | `Failed to launch GDB: Connection timed out` | JLink 未带 `-LocalHostOnly 0`，只绑 `127.0.0.1` | 杀掉 JLink 重新启动（脚本已自动加 `-LocalHostOnly 0`） |
| 3 | `nm-multiarch`/`objdump-multiarch` ENOENT | cortex-debug 需要符号链接 | `launch.json` 加 `"armToolchainPath": "/usr/bin"` |
| 4 | `Remote replied unexpectedly to 'vMustReplyEmpty': timeout` | JLink 卡在 `Connecting to J-Link...`（硬件未就绪） | 检查 J-Link USB 连接和 SWD 线 |
| 5 | 烧录后程序不运行 | `monitor reset` 后没有 halt | `postLaunchCommands` 已配好 `monitor halt` |
| 6 | WSL IP 变动 | WSL 重启后虚拟网卡 IP 变化 | 脚本自动获取：`ip route show default` |
| 7 | socat 启动后 /dev/tcp 验证失败 | WSL→Windows 网络不通，或 JLink 未正确绑定 | `nc -zv $WINDOWS_IP 2331` 手动测可达性，检查 JLink 窗口 |
| 8 | 脚本卡在「等待 JLink 进程」直到 30s 超时；`Get-Process *JLink*` 为空，但 `netstat :2331` 显示 **svchost** 在 LISTEN | 旧方案遗留的 portproxy 规则（`0.0.0.0:2331 → 127.0.0.1:2331`）抢占端口，JLink 带 `-LocalHostOnly 0` 绑不上、启动即退出 | 管理员 PowerShell：`netsh interface portproxy delete v4tov4 listenport=2331 listenaddress=0.0.0.0`，再重跑 `sh/debug_env.sh`（详见 §2.3） |

### 7.3 常用诊断命令速查

```bash
# WSL 端
ip route show default          # Windows IP
hostname -I                    # WSL IP
pgrep -f "socat.*2331"         # 检查 socat
nc -zv localhost 2331          # 测试 socat 监听
nc -zv -w4 172.29.32.1 2331    # 测试 JLink 直连可达性
pkill -9 -f "socat.*2331"      # 杀 socat

# Windows 端（WSL 中执行）
powershell.exe "netstat -ano | findstr ':2331'"               # 查看端口占用
powershell.exe "netstat -ano | findstr '0.0.0.0:2331 ' | findstr LISTENING"  # 确认 JLink 绑 0.0.0.0
powershell.exe "Get-Process JLinkGDBServerCL"                 # 查看 JLink 进程
powershell.exe "taskkill /F /IM JLinkGDBServerCL.exe"         # 杀 JLink
powershell.exe "netsh interface portproxy show v4tov4"        # 查 portproxy 残留（2331 那条必须删）
# 删残留（需管理员 PowerShell，非 WSL）:
#   netsh interface portproxy delete v4tov4 listenport=2331 listenaddress=0.0.0.0
```

---

## 8. 历史问题与解决方案

> 详细历史记录已移入 `docs/archive/wsl_debug_history.md`；当前排障入口见 §7。

## 9. 附录

### 9.1 网络拓扑参考

```
WSL 虚拟网卡: 172.29.47.241/20
Windows 虚拟网卡: 172.29.32.1（默认网关）
Windows 物理网卡: DHCP 获取（对局域网 IP）
```

查询命令：

```bash
# WSL
ip route show default   # → default via 172.29.xx.1 dev eth0
hostname -I             # → 172.29.xx.xx

# Windows
ipconfig                # → vEthernet (WSL) 适配器 IP
```

### 9.2 工具链版本记录

| 组件 | 版本 | 安装路径 |
|------|------|----------|
| Linux内核 | 5.15.167.4-microsoft-standard-WSL2 | — |
| gcc (Ubuntu) | 11.4.0 | WSL |
| arm-none-eabi-gcc | 10.3.1 (15:10.3-2021.07-4) | WSL `/usr/bin` |
| gdb-multiarch | 12.1 | WSL `/usr/bin` |
| socat | 由包管理器安装 | WSL `/usr/bin` |
| JLinkGDBServerCL | V9.72 | Windows `D:\StudyTool\Develop\JLink_V972\` |
| J-Link 硬件 | V9, Firmware May 7 2021, HW V9.70 | USB S/N 602717758 |
| cortex-debug | v1.12.1 | VS Code 扩展 |

### 9.3 调试链路备忘

```
链路（从左到右 = 从 WSL 到硬件）:

  gdb-multiarch
    ↓ target extended-remote localhost:2331
  socat (WSL)
    ↓ TCP
  Windows_IP:2331 (172.29.32.1:2331)
    ↓ 直连（JLink 绑 0.0.0.0:2331, -LocalHostOnly 0）
  JLinkGDBServer 0.0.0.0:2331
    ↓ USB
  J-Link V9 (S/N 602717758)
    ↓ SWD (4000kHz)
  STM32H743II (3.27V-3.29V)
```

### 9.4 版本历史

| 日期 | 变更 | 作者 |
|------|------|------|
| 2026-10-11 | §4.2 的 `test` 示例修正：套件名补尾斜杠（`/fc-nand/logic/` 等，`ut_main.c` v41 起前缀带 `/`）；删掉已失效的 `--param block 10`（v40 起 Level 3 不再参数化）；`test --log-visible info /fc-nand/` → `test --log-visible info`（根套件名单独用不匹配任何用例）；补两条过滤串约束的警示 | Claude Code |
| 2026-10-04 | §5.3 脚本同步为当前 4 步直连版（直接从 `sh/debug_env.sh` 生成）；§2.3 补充 portproxy 残留必须删除的说明与命令；§7.2 新增故障 #8（portproxy 抢占 2331）；§4.3 手动命令补 `-LocalHostOnly 0`；§7.3 补查/删 portproxy 命令 | Claude Code |
| 2026-09-26 | 切换到 -LocalHostOnly 0 直连方案；移除 portproxy/WSL2 内建转发双层 fallback；脚本 6→4 步；新增 `docs/archive/wsl_debug_history.md` §8.11 | Claude Code |
| 2026-09-25 | 终版修复：脚本不再管理 portproxy，回归一次性配置原则；修正 `docs/archive/wsl_debug_history.md` §8.5 中"JLink 绑 0.0.0.0"的误判；新增 `docs/archive/wsl_debug_history.md` §8.6 全链路排查记录 | Claude Code |
| 2026-09-24 | 重写全文：增加链路架构、故障排查实录、历史问题追踪、一键脚本完整内容 | Claude Code |
| 2026-09-23 | 初始创建：WSL 调试环境搭建指南、一键脚本、故障排查实录 | Claude Code |
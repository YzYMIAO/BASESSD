# WSL 调试环境搭建指南：VS Code (WSL) + Windows JLink + STM32H743

## 1. 架构概览

```
┌─────────────────────────────────────────────────────────────┐
│ WSL (Ubuntu)                                               │
│                                                             │
│  VS Code (WSL Remote)                                       │
│  ├─ cortex-debug 扩展                                       │
│  ├─ gdb-multiarch (GDB 客户端)                              │
│  └─ socat 隧道 (localhost:2331 → Windows IP:2331)           │
│                                                             │
│  make tests → build/firmware/debug/bin/debug.elf            │
└──────────────────────────┬──────────────────────────────────┘
                           │ TCP :2331
                           ▼
┌─────────────────────────────────────────────────────────────┐
│ Windows                                                     │
│                                                             │
│  JLinkGDBServerCL.exe (127.0.0.1:2331)                      │
│    └─ USB → J-Link → SWD → STM32H743II                      │
│                                                             │
│  netsh portproxy (0.0.0.0:2331 → 127.0.0.1:2331)           │
│    └─ 允许外部 IP 连接 JLinkGDBServer                       │
│                                                             │
│  Windows 防火墙放行 TCP/2331 入站                            │
└─────────────────────────────────────────────────────────────┘
```

**关键组件：**
- **WSL 侧**：`gdb-multiarch` + `socat` + VS Code `cortex-debug`
- **Windows 侧**：`JLinkGDBServerCL.exe` + `netsh portproxy` + 防火墙规则
- **目标板**：STM32H743II，SWD 接口，J-Link 序列号 602717758

## 2. 环境要求

### WSL (Ubuntu) 需安装

```bash
# 交叉编译工具链（含 gcc、as、ld 等，不含 GDB）
sudo apt install -y gcc-arm-none-eabi binutils-arm-none-eabi

# GDB（gdb-multiarch 支持多种架构）
sudo apt install -y gdb-multiarch

# socat（端口转发）
sudo apt install -y socat
```

验证安装：
```bash
which gdb-multiarch        # → /usr/bin/gdb-multiarch
which arm-none-eabi-gcc    # → /usr/bin/arm-none-eabi-gcc
which socat                # → /usr/bin/socat
```

### Windows 需安装

- **JLink 驱动 + JLinkGDBServer**（V9.72，位于 `D:\StudyTool\Develop\JLink_V972\`）
- **VS Code** + **Cortex-Debug 扩展**（v1.12.1+）
- **USB 驱动**：J-Link 通过 USB 连接，Windows 已识别

### 目标板

- STM32H743II（2 MiB Flash, 480 KB + 128 KB AXI RAM）
- SWD 接口，J-Link S/N: `602717758`，电压 3.27V–3.29V

## 3. 一次性 Windows 配置

### 3.1 防火墙放行 JLink 端口

以**管理员身份**打开 CMD 或 PowerShell，执行：

```cmd
netsh advfirewall firewall add rule name="JLink 2331" dir=in action=allow protocol=TCP localport=2331
```

确认成功输出：`Ok.`

### 3.2 端口转发（portproxy）

以**管理员身份**执行一次（重启后持续生效）：

```cmd
netsh interface portproxy add v4tov4 listenport=2331 listenaddress=0.0.0.0 connectport=2331 connectaddress=127.0.0.1
```

验证：
```cmd
netsh interface portproxy show v4tov4
# 输出应显示:
# 0.0.0.0         2331        127.0.0.1       2331
```

> ⚠️ 以上两条命令**只需执行一次**，Windows 重启后仍保留。

### 3.3 （可选）清理

如果将来想移除：
```cmd
netsh interface portproxy delete v4tov4 listenport=2331 listenaddress=0.0.0.0
netsh advfirewall firewall delete rule name="JLink 2331"
```

## 4. WSL 启动脚本

创建 `sh/debug_env.sh`，一键启动调试环境：

```bash
#!/bin/bash
# debug_env.sh — 一键启动 WSL → Windows JLinkGDBServer 调试环境
#
# 用法:
#   chmod +x scripts/debug_env.sh
#   ./scripts/debug_env.sh
#
# 本脚本自动完成:
#   1. 检查并启动 Windows JLinkGDBServer
#   2. 检查并重建 Windows portproxy (需管理员权限)
#   3. 启动 WSL socat 端口转发
#   4. 验证全链路连通性
#
# 依赖:
#   - Windows: JLinkGDBServerCL.exe (V9.72, D:\StudyTool\Develop\JLink_V972\)
#   - WSL:    socat, gdb-multiarch

set -e

# ========================
# 配置区（按需修改）
# ========================
JLINK_EXE="D:\\StudyTool\\Develop\\JLink_V972\\JLinkGDBServerCL.exe"
DEVICE="STM32H743II"
PORT=2331
# Windows 端 IP（WSL 默认网关 — 通过 ip route show default 自动获取）
WINDOWS_IP=$(ip route show default | awk '{print $3}')
# WSL 端 IP（仅用于输出参考）
WSL_IP=$(hostname -I | awk '{print $1}')

echo ""
echo "============================================"
echo "  WSL → J-Link 调试环境启动脚本"
echo "============================================"
echo "  设备     : $DEVICE"
echo "  端口     : $PORT"
echo "  WSL IP  : $WSL_IP"
echo "  Win IP  : $WINDOWS_IP"
echo "============================================"
echo ""

# -----------------------
# Step 1: 检查 Windows JLinkGDBServer
# -----------------------
echo "[1/5] 检查 Windows JLinkGDBServer..."

JLINK_PID=$(powershell.exe "Get-Process JLinkGDBServerCL -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id" 2>/dev/null | tr -d '\r')

if [ -n "$JLINK_PID" ]; then
    echo "  ✅ JLinkGDBServer 已在运行 (PID: $JLINK_PID)"
else
    echo "  ⚠️  JLinkGDBServer 未启动，正在启动..."
    powershell.exe -Command "& {
        Start-Process -WindowStyle Hidden -FilePath '$JLINK_EXE' -ArgumentList '-device $DEVICE -if SWD -speed 4000 -port $PORT';
        Start-Sleep 3;
        Write-Host 'JLinkGDBServer started';
    }" 2>&1 | grep -v "^$"
    sleep 2

    JLINK_PID=$(powershell.exe "Get-Process JLinkGDBServerCL -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id" 2>/dev/null | tr -d '\r')
    if [ -n "$JLINK_PID" ]; then
        echo "  ✅ JLinkGDBServer 启动成功 (PID: $JLINK_PID)"
    else
        echo "  ❌ JLinkGDBServer 启动失败!"
        echo "     请在 Windows 手动启动:"
        echo "     \"$JLINK_EXE\" -device $DEVICE -if SWD -speed 4000 -port $PORT"
        exit 1
    fi
fi

# -----------------------
# Step 2: 检查并重建 portproxy
# -----------------------
echo "[2/5] 检查 Windows portproxy..."

PORTPROXY_EXISTS=$(powershell.exe "netsh interface portproxy show v4tov4 | findstr '0.0.0.0.*$PORT'" 2>/dev/null | tr -d '\r')

if [ -n "$PORTPROXY_EXISTS" ]; then
    echo "  ✅ portproxy 已存在: 0.0.0.0:$PORT → 127.0.0.1:$PORT"
else
    echo "  ⚠️  portproxy 不存在，正在创建..."
    echo "  ⚠️  如后续提示错误，请以管理员身份运行:"
    echo "       netsh interface portproxy add v4tov4 listenport=$PORT listenaddress=0.0.0.0 connectport=$PORT connectaddress=127.0.0.1"
    echo ""
    powershell.exe -Command "Start-Process -Verb RunAs -WindowStyle Hidden -FilePath 'netsh' -ArgumentList 'interface portproxy add v4tov4 listenport=$PORT listenaddress=0.0.0.0 connectport=$PORT connectaddress=127.0.0.1'" 2>&1 | grep -v "^$"
    sleep 2

    PORTPROXY_EXISTS=$(powershell.exe "netsh interface portproxy show v4tov4 | findstr '0.0.0.0.*$PORT'" 2>/dev/null | tr -d '\r')
    if [ -n "$PORTPROXY_EXISTS" ]; then
        echo "  ✅ portproxy 创建成功"
    else
        echo "  ⚠️  portproxy 可能未创建成功（无管理员权限）"
        echo "     手动执行（以管理员身份打开 CMD）:"
        echo "     netsh interface portproxy add v4tov4 listenport=$PORT listenaddress=0.0.0.0 connectport=$PORT connectaddress=127.0.0.1"
    fi
fi

# -----------------------
# Step 3: 清理并重启 socat
# -----------------------
echo "[3/5] 设置 WSL socat 隧道..."

OLD_SOCAT=$(pgrep -f "socat.*TCP-LISTEN:$PORT" 2>/dev/null || true)
if [ -n "$OLD_SOCAT" ]; then
    kill $OLD_SOCAT 2>/dev/null || true
    echo "  ⚠️  已清理旧 socat (PID: $(echo $OLD_SOCAT | tr '\n' ' '))"
fi

# 启动新的 socat
# 架构: WSL localhost → WSL socat → Windows IP → Windows portproxy(0.0.0.0) → 127.0.0.1 → JLinkGDBServer
#
# ⚠️ 为什么用 Windows IP 而不是 localhost？
#    WSL2 的 localhost 转发只适用于 Windows 进程绑定的 127.0.0.1。
#    但 portproxy 绑定在 0.0.0.0（所有接口），用 localhost 转发时 WSL2 内核
#    不会将流量路由到 portproxy。所以需要显式使用 Windows 的 WSL 虚拟网卡 IP
#    来保证流量正确到达 portproxy。
socat TCP-LISTEN:$PORT,reuseaddr,fork TCP:$WINDOWS_IP:$PORT &
SOCAT_PID=$!
sleep 1

if kill -0 $SOCAT_PID 2>/dev/null; then
    echo "  ✅ socat 隧道运行中 (PID: $SOCAT_PID)"
    echo "     localhost:$PORT → $WINDOWS_IP:$PORT"
else
    echo "  ❌ socat 启动失败"
    exit 1
fi

# -----------------------
# Step 4: 验证连通性
# -----------------------
echo "[4/5] 验证全链路连通性..."

if nc -zv localhost $PORT 2>/dev/null; then
    echo "  ✅ localhost:$PORT 连接成功"
else
    echo "  ❌ localhost:$PORT 连接失败"
    echo "     请检查:"
    echo "       1. JLinkGDBServer 是否在 Windows 运行"
    echo "       2. portproxy 是否创建成功 (管理员 CMD)"
    echo "       3. Windows 防火墙是否放行 TCP/$PORT"
    exit 1
fi

# -----------------------
# Step 5: 检查 GDB 可用性
# -----------------------
echo "[5/5] 检查 GDB..."

if command -v gdb-multiarch &>/dev/null; then
    echo "  ✅ gdb-multiarch: $(which gdb-multiarch)"
else
    echo "  ⚠️  gdb-multiarch 未安装"
fi

# -----------------------
# 完成
# -----------------------
echo ""
echo "============================================"
echo "  ✅ 调试环境就绪！请按 F5 开始调试"
echo "============================================"
echo ""
echo "  终止方式:"
echo "    - 按 Ctrl+C 终止本脚本（会清理 socat）"
echo "    - 单独停止 JLinkGDBServer:"
echo "        powershell.exe \"taskkill /F /IM JLinkGDBServerCL.exe\""
echo "    - 单独停止 socat:"
echo "        kill \$(pgrep -f 'socat.*$PORT')"
echo ""

trap "echo ''; echo '正在清理 socat...'; kill \$SOCAT_PID 2>/dev/null; echo '已清理。'; exit 0" INT TERM

wait $SOCAT_PID

使用方法：
```bash
chmod +x scripts/debug_env.sh
./scripts/debug_env.sh
```

> **升级说明**：新版 `debug_env.sh` 比旧版 `debug.sh` 新增了自动获取 Windows IP、portproxy 检查与重建、socat 自动清理、全链路连通性验证和按 Ctrl+C 自动清理等功能。旧版 `debug.sh` 可安全删除。

## 5. VS Code 调试配置 (launch.json)

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "Debug STM32H743II (WSL → Win JLink)",
      "cwd": "${workspaceFolder}",
      "type": "cortex-debug",
      "request": "launch",
      "servertype": "external",

      "device": "STM32H743II",
      "gdbTarget": "localhost:2331",

      "executable": "${workspaceFolder}/build/firmware/debug/bin/debug.elf",
      "gdbPath": "/usr/bin/gdb-multiarch",

      "runToEntryPoint": "main",

      "preLaunchCommands": [
        "monitor reset halt",
        "load",
        "monitor reset"
      ],

      "postLaunchCommands": [
        "monitor halt"
      ],

      "svdFile": "${workspaceFolder}/STM32H743xI.svd"
    }
  ]
}
```

**关键字段说明：**
- `servertype: "external"` — 不自动启动 GDB server，手动连已有的
- `gdbTarget: "localhost:2331"` — 通过 socat 隧道连到 Windows JLinkGDBServer
- `gdbPath: "/usr/bin/gdb-multiarch"` — WSL 上的 GDB（支持 ARM ELF）
- `preLaunchCommands` — 自动复位 → 烧录 → 再复位
- `runToEntryPoint: "main"` — 停在 main() 入口

## 6. 每日工作流

### 编译 + 调试

```bash
# 终端 1：编译带 UT 的固件
cd ~/projects/soft_ssd
make tests

# 终端 2：启动调试环境
./scripts/debug.sh

# VS Code：按 F5 调试
#   → 自动烧录 debug.elf → 停在 main()
#   → 按 F5 继续运行 → UART 输出就绪
```

### 运行单元测试

通过串口终端（PuTTY / VSCode Serial Monitor / 其他）连到板子 UART：

```
# 全跑三级 UT（Level 1/2/3）
test --param block 10

# 分层运行
test /fc-nand/command/      # Level 1 — command 测试（13）
test /fc-nand/queue/        # Level 1 — queue 测试（17）
test /fc-nand/scheduler/    # Level 1 — scheduler 测试（12）
test /fc-nand/data_path/    # Level 2 — 数据路径测试（15）
test --param block 10 /fc-nand/lifecycle/  # Level 3 — 全链路 34 OP

# 其他 CLI 选项
test --list                 # 列出所有套件
test --list-params          # 列出参数化测试参数
test --seed 0x1234          # 指定随机种子
```

### 修改代码后

```bash
make tests   # 重新编译
# 按 F5 重新烧录调试（或直接在 VS Code 中点 Restart）
```

## 7. 已知问题

| 问题 | 原因 | 解决 |
|------|------|------|
| `nmPath`/`objdumpPath` 不认 | cortex-debug 不支持这些字段 | 删除，cortex-debug 自动找 GDB 同目录工具 |
| `Could not start GDB process` | `arm-none-eabi-gdb` 未安装 | 改用 `gdb-multiarch` |
| `Connection timed out` | 防火墙未放行 TCP/2331 | 执行防火墙规则（第 3.1 节） |
| `Failed to listen at socket` | JLinkGDBServer 端口被占用 | `Stop-Process -Name JLinkGDBServerCL` 重开 |
| `target-select extended-remote` 失败 | socat 隧道断开 | `kill $(pgrep socat)` 重跑 `debug.sh` |
| 烧录后程序不运行 | `monitor reset` 后未 `monitor halt` | `postLaunchCommands` 已配好 |
| Windows IP 变动 | WSL 重启后虚拟网卡 IP 可能变 | 用 `ip route show default` 查新 IP |
| portproxy 不生效 | 未以管理员身份运行 | 右键 CMD → 以管理员身份运行 |
| **`Failed to launch GDB: timeout`** | JLinkGDBServer 默认只绑定 `127.0.0.1`，外部 IP 连不上 | **根因**：WSL socat 转发到 `172.29.32.1`（Windows 外部 IP），但 JLink 只接受 `127.0.0.1` 连接。<br>**解决方案**：不能只靠 `socat localhost`（WSL2 的 localhost 转发不覆盖 portproxy 的 `0.0.0.0` 绑定）。必须用 portproxy + socat 双转发完整链路：<br>`WSL socat → Windows IP → portproxy(0.0.0.0) → 127.0.0.1 → JLink` |
| **`nm-multiarch`/`objdump-multiarch` ENOENT** | cortex-debug 扩展需要 `nm-multiarch` 和 `objdump-multiarch`，但 Ubuntu 的 `binutils` 只提供 `nm` 和 `objdump` | 创建符号链接：`sudo ln -s /usr/bin/nm /usr/bin/nm-multiarch && sudo ln -s /usr/bin/objdump /usr/bin/objdump-multiarch`<br>或在 `launch.json` 中添加 `"armToolchainPath": "/usr/bin"` 让扩展自动查找。<br>⚠️ **这只是符号分类的警告，不影响烧录调试** |

## 8. 故障排查实录（2026-09-23）

### 8.1 现象

VS Code F5 调试报错：
```
Failed to launch GDB: Remote replied unexpectedly to 'vMustReplyEmpty': timeout
(from target-select extended-remote localhost:2331)
```

同时 cortex-debug 输出两个不影响功能的警告：
```
Error: /usr/bin/nm-multiarch failed! Error: spawn /usr/bin/nm-multiarch ENOENT
Error: objdump failed! Error: spawn /usr/bin/objdump-multiarch ENOENT
```

### 8.2 排查步骤

| # | 检查项 | 命令 | 发现 |
|---|--------|------|------|
| 1 | JLinkGDBServer 进程 | `powershell.exe "Get-Process JLinkGDBServerCL"` | ❌ 未运行 |
| 2 | socat 隧道 | `pgrep -f "socat.*2331"` | ✅ 运行中 |
| 3 | 网络连通性 | `nc -zv 172.29.32.1 2331` | ✅ 连接成功（实际连到的是 portproxy） |
| 4 | JLink 监听地址 | `netstat -ano \| findstr :2331` | ❌ 只监听 `127.0.0.1:2331`（localhost only） |

**端口占用排查：**
- `netsh interface portproxy show v4tov4` 发现 portproxy 占用着 `0.0.0.0:2331`
- 删除 portproxy 后 JLinkGDBServer 仍报 `Failed to listen at port 2331` → portproxy 需要管理员权限删除
- 重建 portproxy 后 JLink 正常启动

**连通性根因：**
- JLinkGDBServer V9.72 的 `-localhost off` 参数无效（该版本不支持此参数）
- JLink 始终只绑定 `127.0.0.1`
- socat 如果转发到 `localhost:2331`（WSL2 内建 localhost 隧道）→ 不经过 portproxy → 流量直接到 JLink 的 127.0.0.1
- socat 如果转发到 `172.29.32.1:2331`（Windows 外部 IP）→ 必须经过 portproxy(0.0.0.0) → 再转到 127.0.0.1 → JLink

### 8.3 完整链路图

```
┌─ WSL ───────────────────────────────────────────────────────┐
│  VS Code cortex-debug                                        │
│    └─ gdb-multiarch                                          │
│         └─ gdbTarget: localhost:2331                          │
│              ↓                                                │
│  socat TCP-LISTEN:2331 → TCP:172.29.32.1:2331                │
└──────────────────────────────┬───────────────────────────────┘
                               │ TCP 172.29.32.1:2331
                               ▼
┌─ Windows ────────────────────────────────────────────────────┐
│  portproxy (0.0.0.0:2331 → 127.0.0.1:2331)                   │
│    ↓                                                          │
│  JLinkGDBServerCL.exe (127.0.0.1:2331)                        │
│    ↓                                                          │
│  USB → J-Link → SWD → STM32H743II                             │
└──────────────────────────────────────────────────────────────┘
```

### 8.4 最终已验证的解决方案

1. **Windows 管理员 CMD** 重建 portproxy：
   ```cmd
   netsh interface portproxy add v4tov4 listenport=2331 listenaddress=0.0.0.0 connectport=2331 connectaddress=127.0.0.1
   ```

2. **启动 JLinkGDBServer**（Windows CMD）：
   ```cmd
   "D:\StudyTool\Develop\JLink_V972\JLinkGDBServerCL.exe" -device STM32H743II -if SWD -speed 4000 -port 2331
   ```

3. **WSL 启动 socat**（指向 Windows IP，走 portproxy）：
   ```bash
   socat TCP-LISTEN:2331,reuseaddr,fork TCP:172.29.32.1:2331 &
   ```

4. **VS Code F5** 即可正常烧录调试。

### 8.5 用 `debug_env.sh` 一键启动

日常调试推荐使用 `sh/debug_env.sh` 脚本（见第 4 节）：

```bash
./sh/debug_env.sh
```

该脚本自动完成以上全部步骤。

## 9. 网络拓扑参考

```
WSL 虚拟网卡: 172.29.47.241/20
Windows 虚拟网卡: 172.29.32.1 (默认网关)
Windows 物理网卡: DHCP 获取（对局域网 IP）

WSL localhost → socat → Windows 172.29.32.1:2331 → portproxy → JLink 127.0.0.1:2331
```

查询当前 IP：
```bash
# WSL 中查
ip route show default    # → default via 172.29.32.1 dev eth0
hostname -I              # → 172.29.47.241

# Windows 中查
ipconfig                 # → 查看 "vEthernet (WSL)" 适配器
```
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
    # 用 Start-Process 在后台启动（隐藏窗口）
    powershell.exe -Command "& {
        Start-Process -WindowStyle Hidden -FilePath '$JLINK_EXE' -ArgumentList '-device $DEVICE -if SWD -speed 4000 -port $PORT';
        Start-Sleep 3;
        Write-Host 'JLinkGDBServer started';
    }" 2>&1 | grep -v "^$"
    sleep 2

    # 再次检查是否启动成功
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

# 检查 portproxy 是否已存在
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

    # 确认
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

# 杀掉所有旧的 socat 实例
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

# 检查 socat 是否运行
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
    echo "  ⚠️  gdb-multiarch 未安装，正在安装..."
    echo "     请手动执行: sudo apt-get install -y gdb-multiarch"
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
echo "        kill $(pgrep -f 'socat.*$PORT')"
echo ""

# 等待 Ctrl+C 以清理
trap "echo ''; echo '正在清理 socat...'; kill $SOCAT_PID 2>/dev/null; echo '已清理。'; exit 0" INT TERM

# 保持脚本运行，等待用户 Ctrl+C
wait $SOCAT_PID
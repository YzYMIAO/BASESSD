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
#   - 2026-09-26: portproxy zombie rule 反复出现（§8.7/§8.9/§8.10），
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
# 不能测 socat fork 出的子进程能否连上下游（2026-09-25 §8.8 教训）。
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
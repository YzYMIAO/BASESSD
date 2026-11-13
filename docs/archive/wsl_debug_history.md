# WSL 调试环境：历史问题与解决方案归档

> 从 `docs/design/wsl_debug_environment.md` §8 移出；正文不改写。

## 8. 历史问题与解决方案

### 8.1 2026-09-24：portproxy 规则指向旧端口导致 timeout

> **历史记录（portproxy 方案已废弃）**

**症状**：GDB 能连上、`monitor halt` 正常、`info registers` 正常，但 VS Code F5 烧录时 `vMustReplyEmpty: timeout`。

**排查**：JLink 正常在 2331，但 portproxy 指向 2332（上一轮脚本遗留问题）。

**根因**：上一轮 `debug_env.sh` 的动态端口检测功能在发现 JLink 在 2332 后，修改了 portproxy 指向 2332，但 JLink 重启后回到 2331，portproxy 未恢复。

**对策**：
1. 重建 portproxy: `add v4tov4 listenport=2331 listenaddress=0.0.0.0 connectport=2331 connectaddress=127.0.0.1`
2. 从 `debug_env.sh` 中移除自动修改 portproxy 的逻辑
3. 现在 `debug_env.sh` 始终保持 `portproxy 0.0.0.0:2331 → 127.0.0.1:2331` 不变

### 8.2 2026-09-23：JLink 端口被 portproxy 劫持

> **历史记录（portproxy 方案已废弃）**

**症状**：JLink 报 `Failed to open listener port 2331`，退到 2332 启动。

**排查**：
- portproxy 的 svchost.exe 占了 `0.0.0.0:2331`
- JLink 试图绑定 `0.0.0.0:2331` 失败
- JLink 自动退到随机端口（2332/2333/19030...）
- `netstat` 看到 JLink 在 2332，但 WSL 连 2331

**根因**：portproxy 和 JLink 都想绑定 `0.0.0.0:2331`，但 portproxy (svchost) 先占，JLink 只能退。

**对策**：
- 确认 JLink 只绑定 `127.0.0.1`（不是 `0.0.0.0`）
- portproxy 绑定 `0.0.0.0`，JLink 绑定 `127.0.0.1`，两者**不冲突**
- 之前出现冲突是因为 JLink 退到 2332 后，旧 portproxy 未更新指向
- 目前方案：先杀 portproxy（管理员 CMD 删规则），后启动 JLink

### 8.3 2026-09-23：WSL socat 转发链路不通

> **历史记录（portproxy 方案已废弃）**

**症状**：socat 到 `localhost:2331` 能连上，但 GDB timeout。

**排查**：

| # | 测试 | 结果 |
|---|------|------|
| 1 | `nc -zv localhost 2331` | ✅ 成功 |
| 2 | `nc -zv Windows_IP 2331` | ✅ 成功（连到 portproxy） |
| 3 | JLink 监听地址 | ❌ 只绑定 `127.0.0.1` |
| 4 | portproxy 转发目标 | JLink 实际在 2332，portproxy 指向 2331 |

**根因**：socat 数据走到了 Windows，portproxy 转发到了 `127.0.0.1:2331`，但 JLink 在 2332，所以 GDB 连接超时。

**对策**：确保 portproxy + socat 的转发目标端口一致。

### 8.5 2026-09-25（已修正）：错误地假设 JLink 绑 0.0.0.0，盲改脚本导致链路断裂

> **历史记录（portproxy 方案已废弃）**

> ⚠️ **本条目的分析已被 §8.6 证伪，保留作为错误方向的记录。** 实际 JLink V9.72 绑 `127.0.0.1`，`0.0.0.0:2331` 列表中的是 portproxy (svchost)。

**症状**：脚本 Step 4 显示 "❌ JLinkGDBServer 启动失败!"，Windows 手动启动 JLink 报 `ERROR: Failed to open listener port 2331`。

**排查**：
- `netstat -ano | findstr ':2331 '` 看到 PID 7088 占了 `0.0.0.0:2331`，实际是 portproxy(svchost) 的监听
- 被错误判断为"JLink 绑 0.0.0.0"
- 对 JLink V9.72 绑定行为的假设有误

**对策（错误的，已被 §8.6 覆盖）**：
- 修改脚本时序为：删 portproxy → 启动 JLink → 再加回 portproxy
- 改为 portproxy 2330 方案（但没加防火墙规则，导致 socat 超时）

**教训**：应用层 socket 的绑定行为需要区分进程。`netstat` 看到 `0.0.0.0:2331` 可能是 portproxy，不是 JLink。用 PID 关联确认。

### 8.6 2026-09-25（终版修复）：脚本不再管理 portproxy，恢复一次性配置原则

> **历史记录（portproxy 方案已废弃）**

**症状**：经过 §8.5 的修复后，VS Code F5 烧录仍报 `Remote connection closed` / `timeout`。

**排查过程**：
1. portproxy 从 2331 改到 2330 后，`ping` 通 Windows，但 `nc -zv 2330` 不通 → 防火墙只有 2331 规则，没有 2330
2. 重新检查 JLink V9.72 实际绑定：`netstat -ano | findstr 2331 | findstr LISTENING` 看到**两条记录**：
   - `127.0.0.1:2331` → JLink (PID 24468)
   - `0.0.0.0:2331` → svchost (PID 7088) = portproxy
3. 确认了之前把 `0.0.0.0:2331` 误当作 JLink 的错误

**根因**：
1. §8.5 的分析有误：JLink V9.72 **并非**绑 `0.0.0.0`，而是 `127.0.0.1`。`netstat` 看到的 `0.0.0.0:2331` 是 portproxy(svchost) 的监听，不是 JLink 的
2. portproxy 和 JLink 在同一端口 2331 上**确实可以共存**（§2.3 有说明）：portproxy 是内核 NAT，JLink 是应用层 socket，Windows 网络栈的不同层级
3. 改到 2330 的方案没有添加对应防火墙规则，导致链路断裂

**最终对策**：
1. **`sh/debug_env.sh` 完全移除 portproxy 管理逻辑** — 不再创建、删除、修改 portproxy
2. portproxy 回归**一次性配置**原则（§2.3），作为永久系统级配置，重启后持续生效
3. 脚本简化为 5 步：杀残留 → 启动 JLink → 启动 socat → 验证 → 更新 launch.json
4. 文档中错误的"JLink 绑 0.0.0.0"描述已修正回 `127.0.0.1`

**最终链路（稳定运行中）**：

```
GDB → localhost:2331 → socat → 172.29.32.1:2331
      → portproxy(0.0.0.0:2331, 内核NAT) → 127.0.0.1:2331 → JLink
```

**重启后恢复步骤**（仅需两步）：
```bash
./sh/debug_env.sh   # 自动完成所有
# VS Code F5 调试
```

### 8.7 2026-09-25：portproxy IPv4 转发失效，引入 WSL2 内建转发首选方案

> **历史记录（portproxy + WSL2 内建转发方案已废弃）**

**症状**：socat 和 portproxy 规则均正常显示，但 WSL → `Windows_IP:2331` 超时，GDB 报 `vMustReplyEmpty: timeout`。关防火墙后仍不通。Windows 本地 `Test-NetConnection 127.0.0.1 2331` 正常（JLink 在跑）。

**排查过程**：
1. 全链路检查：JLink ✅ → `nc -zv localhost 2331` ✅ → `nc -zv Windows_IP 2331` ❌ 超时
2. portproxy `show v4tov4` 显示规则存在（`0.0.0.0:2331 → 127.0.0.1:2331`）
3. 防火墙规则存在，关闭防火墙仍不通
4. Windows 本地能连 JLink（`Test-NetConnection 127.0.0.1:2331` ✅）
5. 结论：portproxy 的 IPv4 转发规则实际失效，但 `show v4tov4` 仍显示存在

**根因**：Windows 网络栈重启后（掉电/重启/网络重置），portproxy 内核 NAT 规则虽然 `show` 出来，但实际转发可能已失效。这不是首次见——portproxy 存在 "zombie rule" 问题。

**对策**：
1. 文档架构更新为**双层 fallback**：
   - **首选** WSL2 内建转发（wslrelay）：socat → `127.0.0.1:2331`
   - **回退** portproxy + Windows IP：socat → `Windows_IP:2331`
2. `debug_env.sh` Step 5 策略反转：原来先试 `Windows_IP`（portproxy），回退 `127.0.0.1`（WSL2 转发）；现在**先试 `127.0.0.1`（WSL2 内建转发）**，不通才回退 portproxy
3. 文档 §3 新增双层数据流图
4. §7.2 新增已知问题和下一条快速修复方案

**教训**：
- portproxy 的 `show` 输出不能作为转发是否生效的依据——必须在 WSL 用 `nc -zv Windows_IP 2331` 实测
- WSL2 内建转发（wslrelay）其实更稳定，之前"有时不生效"的判断可能混入了 portproxy 失效的误报
- 调转 fallback 顺序：先试最简路径，复杂链路做兜底

### 8.8 2026-09-25：socat 伪阳性 + 脚本回退逻辑修复 + 掉电恢复

**症状**：`debug_env.sh` 启动后 socat ✅、`nc -zv localhost 2331` ✅，但 VS Code F5 报 `vMustReplyEmpty: timeout`。

**排查过程**：
1. 脚本输出显示 socat 走的是 WSL2 内建转发路径（`127.0.0.1:2331`）
2. `nc -zv localhost 2331` ✅（socat listen 端口正常）
3. 但实际 GDB 连接后 socat fork 的子进程连 `127.0.0.1:2331` 被拒绝（WSL2 内建转发不通）
4. 直接测 **目标地址** 确认 WSL2 转发不通、portproxy 通：
   - `nc -zv 127.0.0.1 2331` ❌
   - `nc -zv 172.29.32.1 2331` ✅
5. 手动启动 socat → portproxy 路径 → F5 ✅

**根因**：
1. **`nc -zv localhost` 只能测 socat 的 listen 端口是否打开**，不能测 socat fork 出的子进程能否连上下游目标
2. 旧脚本逻辑"先启动 socat，再用 `nc -zv localhost` 验证"是典型的**伪阳性验证**
3. 当 socat `fork` 子进程失败时，父进程的 listen socket 依然存活，造成"链路正常"的假象

**对策**：
1. `debug_env.sh` Step 5 重写为**先验证目标地址、再启动 socat**：
   - `nc -zv -w2 127.0.0.1:$PORT` → 测 WSL2 内建转发
   - 不通则 `nc -zv -w2 $WINDOWS_IP:$PORT` → 测 portproxy
   - 通哪个用哪个做 socat 目标
   - 都不通 → 报错退出并给出排查指引
2. 启动后改用 `echo >/dev/tcp/localhost/$PORT` 做真实连接测试
3. 文档新增掉电/上电恢复步骤（现 §4.6）

**教训**：
- `nc -zv localhost` 在 socat 场景下**只能说明 socat 在监听**，不能说明"链路通"
- 验证 socat 转发是否可达，必须测 socat 的**目标地址**（不是 localhost）
- 掉电后无需特殊操作：JLink 自动重连 + preLaunchCommands 的 `monitor reset halt → load → monitor reset` 处理未知状态

### 8.9 2026-09-25：portproxy zombie rule 重建无效，重启 IP Helper 恢复

> **历史记录（portproxy 方案已废弃）**

**症状**：portproxy 规则被误删重建后，`show v4tov4` 显示正常，但 WSL 侧 `nc -zv Windows_IP 2331` 始终 `Connection refused`。

**排查过程**：
1. portproxy `show` 显示规则存在（`0.0.0.0:2331 → 127.0.0.1:2331`）
2. 但在 WSL 中用 `nc -zv 172.29.32.1 2331` 一直拒绝
3. Windows 上 `Test-NetConnection 127.0.0.1:2331` ✅（JLink 正常）
4. Windows 上 `Test-NetConnection 172.29.32.1:2331` ❌（portproxy 不转发）
5. 添加了普通防火墙规则 + Hyper-V 防火墙规则（`New-NetFirewallHyperVRule`），仍无效
6. 加了 Hyper-V 规则后 `EnforcementStatus` 显示 `ProfileDisabled`（配置文件不匹配）
7. 重启 IP Helper 服务后恢复

**根因**：
1. portproxy 在内核层维护的 NAT 状态与用户态 `show` 展示的规则可脱离——§8.7 已记录此 zombie rule 现象
2. **从 WSL 的 `powershell.exe` 可以无管理员权限删除 portproxy 规则**，但重建需要管理员权限。这导致意外删除规则后无法恢复
3. 新添加的规则即使 `show` 正常，也需要 IP Helper 服务重启才能真正加载到内核 NAT 表
4. 此前能工作的规则是长期运行积累的有效状态，删除后新规则即使 `add` 成功也不自动生效

**对策**：
1. **portproxy 重建后重启 IP Helper 服务**（管理员 PowerShell）：
   ```powershell
   net stop iphlpsvc; net start iphlpsvc
   netsh interface portproxy add v4tov4 listenport=2331 connectaddress=127.0.0.1 connectport=2331 listenaddress=0.0.0.0
   ```
2. 从 WSL 误删 portproxy 后的完整恢复流程已加入 §8.10（快速诊断流程补充）

**教训**：
- 永远不要在 WSL 中用 `powershell.exe netsh interface portproxy delete ...` —— 它会无权限检查地删除规则
- portproxy 的 `show v4tov4` 输出**不能证明内核 NAT 转发正在生效**（zombie rule 问题反复出现）
- 重启 IP Helper 服务是解决 zombie rule 的最终手段

### 8.10 2026-09-25：快速诊断命令补充

> **历史记录（portproxy 方案已废弃）。诊断命令已更新为直连方案**

以下是完整调试链路排查命令（整合 §7.1 与 2026-09-25 新增经验）：

```bash
# === WSL 侧 ===
# 测试 portproxy 是否有效（必须测 Windows_IP，不能只测 localhost）
nc -zv -w4 172.29.32.1 2331

# 测试 WSL2 内建转发
nc -zv -w4 127.0.0.1 2331

# 测试 socat 监听
nc -zv localhost 2331

# === Windows 侧（管理员 PowerShell）===
# 检查 JLink 是否在监听
netstat -ano | findstr '127.0.0.1:2331'

# 检查 portproxy 规则是否存在
netsh interface portproxy show v4tov4

# 从 Windows 本地测试 JLink（loopback 直接可达）
Test-NetConnection 127.0.0.1 -Port 2331

# 从 Windows 本地测试 portproxy（走虚拟网卡则走 NAT）
Test-NetConnection 172.29.32.1 -Port 2331

# === 如果 portproxy不通但 JLink 在跑 ===
# 方案 A：重启 IP Helper 服务 + 重建 portproxy
net stop iphlpsvc; net start iphlpsvc
netsh interface portproxy add v4tov4 listenport=2331 connectaddress=127.0.0.1 connectport=2331 listenaddress=0.0.0.0

# 方案 B：重启 WSL（更彻底，但会中断对话）
# 在 Windows PowerShell（非 WSL）：wsl --shutdown
```

**重要**：PowerShell 使用 `;` 而不是 `&&` 或 `||` 分隔命令。`&&` 在 PowerShell 中不是有效语法。

### 8.11 2026-09-26：portproxy 三次失效，切换到 -LocalHostOnly 0 直连方案

**症状**：portproxy zombie rule 反复出现（§8.7 / §8.9 / §8.10），即使按 §8.9 重建 portproxy + 重启 IP Helper 服务，转发仍然不通。WSL2 内建转发也同步不可用。三步链路验证结果：

| 检查项 | 结果 |
|--------|------|
| JLink 在 Windows `127.0.0.1:2331` | ✅ 监听正常 |
| WSL2 内建转发 `127.0.0.1:2331` | ❌ Connection refused |
| portproxy `Windows_IP:2331` | ❌ zombie rule，规则 show 存在但转发失效 |

**排查过程**：
1. `net stop iphlpsvc` 发现 IP Helper 服务已不在运行状态（`NET HELPMSG 3521`）
2. 重启服务 + 重建 portproxy 后规则 `show` 正常，但内核 NAT 仍然不转发
3. WSL 和 Windows 网络本身正常（ping ✅，端口 135 ✅）
4. 尝试 `-LocalHostOnly 0` 启动 JLink，socat 直连 Windows_IP 一次性成功

**根因**：
1. portproxy 依赖 IP Helper 服务的内核 NAT 表，该服务停止后新加的规则即使 `show` 正常也不实际生效
2. WSL2 内建转发（wslrelay）也同时不可用，可能是 WSL2 内核的转发表与 Windows 的 NAT 状态耦合
3. portproxy 的 zombie rule 问题本质上是 Windows 内核 NAT 表状态无法可靠恢复——文档已记录三次不同场景的失效，说明这不是偶发 bug，而是该方案的固有缺陷

**对策**：
1. 脚本和文档全线切换到 **`-LocalHostOnly 0` 直连方案**，JLink 绑定 `0.0.0.0:2331`，socat 直连 `Windows_IP:2331`
2. 脚本从 6 步缩减为 4 步（去掉 portproxy 检查、防火墙检查、双层 fallback 校验）
3. Step 1 不再暴力杀 `0.0.0.0:PORT` 的进程（避免误杀 svchost）
4. portproxy 和历史方案标记为 **已废弃 (deprecated)**，保留文档作为历史参考

**新链路**：

```
GDB → localhost:2331 → socat → 172.29.32.1:2331 → JLink(0.0.0.0:2331, -LocalHostOnly 0)
```

**新前置条件**：无。不再依赖 portproxy / 防火墙规则 / WSL2 内建转发。

**教训**：
- portproxy 经过三次修复（§8.6 脚本移除管理、§8.7 双层 fallback、§8.9 IP Helper 重启），仍无法稳定工作——这不是配置问题，是 Windows 内核 NAT 的固有缺陷
- `-LocalHostOnly 0` 是 JLinkGDBServer 的内置功能，让 JLink 直接绑定 `0.0.0.0`，不需要任何中间层。这是架构上最简路径
- 之前认为 JLink V9.72 只绑 `127.0.0.1` 的判断（§8.5/§8.6）其实只看到了默认行为——`-LocalHostOnly 0` 参数可以改变这个行为

---

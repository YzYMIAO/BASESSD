# 使用 Claude Code（重点：日常切换模型）

本文说明 Claude Code（CLI 工具）的日常用法，重点是**如何在会话内、会话间切换不同模型**。
内容依据 2026-10-03 官方文档整理，侧重“嵌入式/固件开发日常”会用到的部分。

## 一、模型切换的 5 种方式（优先级从高到低）

| 优先级 | 方式 | 生效范围 | 何时用 |
|--------|------|----------|--------|
| 1 | `/model` 命令 | 会话内，且可保存为默认 | 正在干活时临时换模型，或固定默认 |
| 2 | `claude --model` 启动参数 | 仅本次会话 | 一次性用某个模型启动 |
| 3 | `ANTHROPIC_MODEL` 环境变量 | 仅本次会话 | 同终端 session 注入 |
| 4 | `settings.json` 的 `model` 字段 | 下次启动生效 | 固定某个项目/全局默认 |
| 5 | `ANTHROPIC_DEFAULT_MODEL` / 账户默认 | 新会话兜底 | 其余都没指定时 |

下面逐条展开。

---

## 二、`/model` 命令（最常用）

在交互式会话里直接敲：

```
/model            # 打开模型选择器（无参数）
/model sonnet     # 直接切换到 Sonnet
/model opus       # 直接切换到 Opus
/model haiku      # 直接切换到 Haiku
/model fable      # 直接切换到 Fable
```

要点：

- **`/model`（不带参数）** 弹出选择器，用方向键选择。
  - 按 `Enter` = 切换模型**并保存为默认**（写入 `~/.claude/settings.json` 的 `model` 字段）。
  - 按 `s` = **仅本次会话**切换，不动你保存的默认值。
- **`/model <别名>`（带参数）** 直接切换，行为等同 `Enter`：会持久化为默认。
- 部分支持 effort 的模型，可在选择器里用左右方向键调 effort 档位。
- 非交互模式（`-p`）下 `/model` 只对当前会话生效，不保存默认。
- 切换还会传导给继承主会话模型的子代理（subagent）；要让某个子代理固定用小模型，在其定义里单独设 `model`。

等价写法：`/config model=sonnet` 也能切。

---

## 三、`--model` 启动参数

```
claude --model opus
claude --model claude-sonnet-5-5      # 也接受完整模型 ID
```

- 覆盖 `model` 设置和 `ANTHROPIC_MODEL`，优先级高于二者。
- 只影响本次启动的会话。想同时开多个终端跑不同模型，各自带各自的 `--model` 即可。

---

## 四、`ANTHROPIC_MODEL` 环境变量

```bash
export ANTHROPIC_MODEL=sonnet
claude
```

- 只对当前终端 session 生效。
- 优先级高于任何 settings 文件的 `model` 字段（包括 managed 配置），低于 `--model`。
- 与 `--model` 相同，不预校验拼写——写错会在首次请求时报 “There's an issue with the selected model”。

---

## 五、settings.json 的 `model` 字段

```json
{ "model": "claude-sonnet-5-5" }
```

- 键名 `model`，值可用别名（`sonnet`）或完整 ID（`claude-sonnet-5-5`）。
- 作用域：任意 settings 文件均可。
- **关键点：`model` 只在会话启动时读取一次。** 会话中直接改文件不会切当前会话；要立刻切换用 `/model`，改文件影响“下一次启动”。
- 多文件之间的优先级（高→低）：

  1. Managed 配置（`managed-settings.json` / MDM / claude.ai 控制台）
  2. 命令行参数（含 `--settings`）
  3. 项目本地：`.claude/settings.local.json`（你个人、单项目，不入库）
  4. 项目共享：`.claude/settings.json`（团队共享、入库）
  5. 用户级：`~/.claude/settings.json`（你个人、全部项目）

  注意：`/model` 写的是**用户级**文件，所以项目里若设了 `model`，会盖过用户级默认。

---

## 六、别名与模型 ID 对照

### 别名

| 别名 | 含义 |
|------|------|
| `default` | 清除一切覆盖，回退到账户运行时默认（不是某个模型） |
| `best` | Fable 可用时即 Fable，否则同 `opus` |
| `fable` | Fable（最难的长期任务） |
| `opus` | 最新 Opus（复杂推理） |
| `sonnet` | 最新 Sonnet（日常编码） |
| `haiku` | Haiku（快速/低开销的简单任务） |
| `sonnet[1m]` / `opus[1m]` | 1M 上下文的 Sonnet/Opus |
| `opusplan` | Plan 阶段用 `opus`，执行阶段切 `sonnet` |

### 当前常用模型 ID

| 模型 | 模型 ID |
|------|---------|
| Fable 5.1 | `claude-fable-5-1` |
| Opus 5.5 | `claude-opus-5-5` |
| Sonnet 5.5 | `claude-sonnet-5-5` |
| Haiku 4.5 | `claude-haiku-4-5` |

> 别名指向的具体版本会随官方更新而变化，想**钉死**某版本就用完整 ID（或配合下文 `ANTHROPIC_DEFAULT_*_MODEL`）。

---

## 七、`/fast` 快速模式

- 交互会话中敲 `/fast`（或 `/fast on` / `/fast off`）切换。
- **不是另一个模型**：它仍用 Opus，只是换了一套“重速度、轻成本”的 API 配置，最快可到 2.5x，单 token 单价更高。
- 仅 Opus 5.5 / Opus 5 / Opus 4.8 支持；Sonnet、Haiku 等不支持。
- 当前模型不支持 fast 模式时，开启它会自动把你切到 Opus；关闭时不自动切回（用 `/model` 手动换）。
- 对应 settings 键：`"fastMode": true`（跑 `/fast` 会写入 `~/.claude/settings.json`，关闭则删除该键）。

---

## 八、其它切换 / 型号相关机制

| 机制 | 作用 |
|------|------|
| `ANTHROPIC_DEFAULT_OPUS_MODEL` 等 | **不改选模型**，只改别名解析到的具体 ID（值必须为完整模型名）。`OPUS`/`SONNET`/`HAIKU`/`FABLE` 各一族，另有 `ANTHROPIC_DEFAULT_MODEL` 设新会话默认模型 |
| `ANTHROPIC_CUSTOM_MODEL_OPTION` | 往 `/model` 选择器加一个自定义条目（例如走第三方网关的模型 ID） |
| `CLAUDE_CODE_SUBAGENT_MODEL` | 子代理/队友/工作流代理的默认模型 |
| `--fallback-model a,b` / `fallbackModel` 设置 | 主模型过载/不可用时的自动回退 |
| `modelPicker` 设置 | 自定义 `/model` 选择器的行、顺序、标签 |

---

## 九、本机实际模型配置（第三方网关示例）

下面是本机当前生效的一份配置（`settings.json` 的 `env` 块写法）。它把 Claude Code 指向了
**火山方舟（Volcano Ark）网关**，通过 Anthropic 兼容协议实际调用的是 deepseek / glm 等
第三方模型。注释仅作说明——**JSON 本身不支持 `//`，实际写入时需去掉注释**：

```json
{
  "env": {
    // 火山方舟的认证 token（敏感凭据，勿提交到公开仓库 / 勿外泄）
    "ANTHROPIC_AUTH_TOKEN": "<your-token-here>",

    // 网关入口，替换官方 api.anthropic.com
    "ANTHROPIC_BASE_URL": "https://ark.cn-beijing.volces.com/api/plan",

    // 未显式指定模型时的默认模型（网关侧模型名，非 Anthropic 模型）
    "ANTHROPIC_MODEL": "deepseek-v4.1-flash",

    // —— 别名重映射：让 /model 里的 opus/sonnet/haiku 落到具体第三方模型 ——
    // haiku 别名 → 小模型（简单任务 / 后台功能）
    "ANTHROPIC_DEFAULT_HAIKU_MODEL": "deepseek-v4.1-flash",
    // sonnet 别名 → 日常编码主力
    "ANTHROPIC_DEFAULT_SONNET_MODEL": "deepseek-v4-pro[1m]",
    // opus 别名 → 复杂推理
    "ANTHROPIC_DEFAULT_OPUS_MODEL": "glm-5.3[1m]",

    // 子代理 / 队友 / 工作流代理默认模型
    "CLAUDE_CODE_SUBAGENT_MODEL": "deepseek-v4-pro[1m]",

    // 自动压缩上下文窗口阈值（token）
    "CLAUDE_CODE_AUTO_COMPACT_WINDOW": "1000000",

    // 关闭非必要的遥测 / 后台网络流量
    "CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC": "1"
  }
}
```

要点：

- **别名被重定向了**：在这份配置下，敲 `/model opus` 实际切到 `glm-5.3[1m]`，
  `/model sonnet` → `deepseek-v4-pro[1m]`，`/model haiku` → `deepseek-v4.1-flash`。
  第六节那张“别名 → Anthropic 模型”的对照表在此被覆盖。
- **`[1m]` 是网关侧模型名的一部分**：`deepseek-v4-pro[1m]`、`glm-5.3[1m]` 是火山方舟上的
  模型 ID，不要和 Anthropic 官方的 `sonnet[1m]` / `opus[1m]` 别名混淆。
- **两种写法等价**：这份 `env` 块写在某个 settings 文件（走第五节的 settings 优先级），
  也可以改成 shell 里逐个 `export`（走环境变量规则），效果相同、仅归属层级不同。
- `ANTHROPIC_AUTH_TOKEN` 是敏感凭据，建议只保留在本机配置、避免入库或进公开仓库；
  确需入库时请替换成占位符。

---

## 十、日常使用速查

### 启动

```bash
claude                     # 进入交互式会话
claude "帮我重构 XX"        # 带初始提示进入
claude -p "查一下 YY"       # 非交互：打印结果后退出（脚本/管道用）
claude -c                  # 继续当前目录最近一次会话
claude -r "<会话ID/名>"     # 按 ID/名字恢复指定会话（-r 无参打开会话选择器）
```

### 常用交互命令

| 命令 | 作用 |
|------|------|
| `/help` | 查看帮助与可用命令 |
| `/model [别名]` | 切换模型（见上文） |
| `/config [key=value]` | 打开设置界面；`/config model=sonnet` 直接设键 |
| `/clear [名]` | 清空上下文、开新会话（别名 `/reset` `/new`） |
| `/compact [说明]` | 压缩会话历史以释放上下文（保留同一会话） |
| `/status` | 查看版本/模型/账户/连通性，及已加载的 settings 来源 |
| `/memory` | 编辑 CLAUDE.md、开关 auto memory、查看已记条目 |
| `/effort` | 调 reasoning 档位：low/medium/high/xhigh/max |
| `/fast` | 开/关快速模式 |
| `/context`、`/cost`（别名 `/usage`）、`/doctor`、`/permissions`（别名 `/allowed-tools`） | 上下文占用、token 费用、诊断、权限 |

> 命令只在消息**开头**才被识别；输入 `/` 可呼出菜单。

### 配置与权限相关

- 用户级设置 `~/.claude/settings.json`（全局）。
- 项目设置 `.claude/settings.json`（团队共享、入库）与 `.claude/settings.local.json`（个人、不入库）。
- 用 `/status` 的 `Setting sources` 行或 `claude doctor` 确认哪些设置文件被实际加载。

---

## 十一、排查：为什么“下次会话”不是我以为的模型

`/model` 写的是用户级默认，但下次启动却跑了别的模型，通常是这几种原因：

1. 你按了 `s`、用了 `--model`、或用了 `-p`（只在当前会话生效，没存默认）。
2. 项目或 managed 配置里的 `model` 优先级更高，下次启动时又盖了回来。
3. `ANTHROPIC_MODEL` 环境变量在，它会盖过所有文件里的 `model`。
4. 你“恢复”（resume）了某个旧会话——被恢复的会话保留它 transcript 里存的那个模型，不看当前 `model` 设置。
5. `model` 字段只在会话启动时读取；会话中改文件不生效，要立刻切换得用 `/model`。

对照上文第五节的优先级，逐个往上查即可。
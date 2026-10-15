# v38：把「ECC / CRC 到底保护什么」写入设计文档

- **日期**：2026-10-10
- **范围**：**纯文档 + 注释**。`docs/design/fc_nand_design.md`（六处插入 + 一处孤儿行删除）、
  `src/fc/fc_data_path.c`（**仅** `fc_data_path_meta_check` 的注释）
- **生产代码**：`src/**` 的**逻辑零改动**——唯一 `src/` 变更是 `fc_data_path.c` 的一段注释。
- **介质格式**：无变更。
- **测试**：**不改断言、不改用例数（77）**。`make format-check` 过；
  `make clean && make tests` **0 warning 0 error**、链接 **219012 B**（与 v36/v37 逐字节同）。

## 背景：一次关于「CRC 只保护 USER」的追问

用户提问：**「对于 FC 路径而言 CRC 是保证不出错的关键，按理说应该保护 META 和 USER，但当前只保护 USER 是不是有问题？」**

逐层追问后收敛出的结论：

> **CRC 的职责不是「保护某几个字节」，而是抓 ECC 误纠。**
> **「CRC 只盖 USER」是分工，不是漏——META 有比 CRC 更强的手段。**

### 1. ECC 报成功 ≠ 数据正确（两种失败模式）

| 失败模式 | 表现 | 上层能否发现 |
|---|---|---|
| ① **不可纠** | 解码器报失败（`UNC`） | ✅ 可检测 |
| ② **误纠**（miscorrection） | 解码器**报成功**，输出**另一个合法码字** | ❌ **不可检测** |

迭代解码的三种收尾里，**收敛到错误码字**（校验子 = 0）是唯一危险的——报成功但错。
**LDPC 的 error floor 就是它**：trapping set / absorbing set 内部的校验节点连着**偶数个**错误 bit，
校验和正好为 0（文献称 "deceptively satisfied" checks），校验子权重不再下降，解码器卡在不动点。
**BCH 同病**（有界距离解码：≥ t+1 错且落进另一码字的 t 球内）。

**推论**：`UE ≈ P(误纠) × 2⁻³²`——UBER 目标（1e-15 / 1e-16）靠的是这个**乘积**，单靠 ECC 打不到。
**CRC 不是可选项。**

### 2. META 不是没人保护——四条路径都能检出

| 路径 | `random` | `crc` | META 靠什么查 |
|---|---|---|---|
| 主机数据读 | ON | ON | Pass 4 **精确比对**（100% 检出，强于 CRC 的 1−2⁻³²） |
| GC / 搬移读 | ON | ON | Pass 4 自洽(3 位) + **种子耦合 → CRC 失败** |
| 元数据页读 | OFF | OFF | Pass 4 **精确比对**（`lba=0` ⇒ 期望值即 `plane_cw`） |
| RAW 读 | OFF | OFF | 无——**刻意** |

**行业对照**：T10 PI / DIF 的 guard（CRC-16）也**只盖逻辑块数据**，地址正确性靠 reference tag **比对**。
判据不是「CRC 该盖哪些字节」，而是「**这个字段出错时，你能不能用别的手段发现**」。

### 3. 但有两处真实差异点（本次只记录）

- **GC / 掉电重建路径**（期望值**未知**）靠的是**种子耦合**——加扰的**副产品**，没写进契约，
  且一旦 `random=OFF` 就失效。行业在这里放**显式**校验。
- **FTL 只判 `NAND_FAIL`**（`ftl.c:92`、`ftl_fc_interface.c:431/459/527`、`gc.c:75`、`read_recovery.c:96`），
  而 FC 把 `UNC` / `CRC_ERROR` / `MATA_ERROR` 都 OR 进了 `completion.status`（`fc.c:772`）⇒
  **已检出的错误当成功交给主机**。本工程无 read retry / RAID 兜底（`CONFIG_FC_RAID=0`）⇒
  **等效 UBER ≈ `P(误纠)`，而不是 `P(误纠) × 2⁻³²`**——CRC 那 32 位不是不工作，是**没人读它的判定结果**。
  这条在 **FTL 层**，按硬规则本次**不动 `src/ftl/**`**。

## 改动

| 位置 | 插入内容 |
|---|---|
| `fc_nand_design.md` §21.3.3（无硬件 ECC） | **ECC 的两种失败模式**、三种收尾表、LDPC error floor ← trapping set、BCH 误纠、`UE ≈ P(误纠) × 2⁻³²`、出处 |
| 同上 §5.2（解码流程） | **三道校验各抓什么**（Pass 1 随机位错 / Pass 3 误纠 + 数据路径错 / Pass 4 META）+ 种子耦合为何让 GC 路径安全 |
| 同上 §5.3（CW 布局） | **字段 × 保护者**表 + **四条路径 META 保护矩阵** + 行业对照（T10 PI）+ 本工程差异点 |
| 同上 §2.4（CW 状态位） | **谁在读这些失败位**：FC 读 `ERROR_MASK` 全集，FTL 只读 `NAND_FAIL` |
| 同上 §8.2（待改进项） | 新增两行：**FTL 只判 `NAND_FAIL`**、**GC 路径 META 缺显式校验**（均标注在 FTL 层、本次不动） |
| 同上 §21.3.5（无硬件加扰） | 一句：种子耦合是「期望值未知」路径上 META 的**唯一动态保护**，且是**隐式**的 |
| 同上 §5.3（CW 布局） | **删**孤儿行「数据路径编码流程（加扰/ECC/RAID）：」——它在布局代码块与 §5.4 之间、后面没有正文，且与代码块末行「数据路径（加扰/ECC/RAID）操作全 528 字节」重复 |
| `src/fc/fc_data_path.c` | `fc_data_path_meta_check` 注释补一行指针（`FC_META_UNKNOWN` 时只剩 3 位自洽） |

**不改任何断言、不改用例数（77）、不改任何 `src/**` 逻辑。**

## 验证

```bash
make format-check              # 过
make clean && make tests       # 0 warning 0 error，链接 219012 B（与 v36/v37 逐字节同）
```

**纯文档 + 注释，无需上板。**

## 出处

- 讨论中确认的理论与出处：`fc_nand_design.md` §21.3.3 / §5.2 / §5.3。
- 前一轮（加扰 = 统计独立）：`docs/archive/changelog/fc_scrambler_theory_v37.md`。
- CRC 引擎缺陷史（「基址写成 ADC3 ⇒ 校验恒通过」）：`docs/archive/changelog/fc_crc_hal_driver_v33.md`。
- 加扰种子改逻辑地址（META 明文 + 种子自举的由来）：`docs/archive/changelog/fc_scrambler_lba_seed_v24.md`。

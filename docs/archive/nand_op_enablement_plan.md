# NAND OP 启用方案：multi-plane / cache / copy-back

> 状态：**方案文档**。未改代码、未上板；所有收益数字是估算，不是实测（R1）。
> 适用范围：STM32H743 + MT29F4G08ABADAWP，`LUN_NUM=1`、2 plane、FMC PIO、USB Full Speed。
> 相关文档：[`ftl_design.md`](../design/ftl_design.md)（ADR-15）、[`fc_nand_design.md`](../design/fc_nand_design.md)
> §17.6/§25.8、[`storage_controller_design_essentials.md`](../design/storage_controller_design_essentials.md) §3.4。

## 0. 结论摘要

当前生产路径只用了 **普通页读写 + multi-plane 页读写**（外加逐 CW 的 `05h/06h`
随机读、擦除、META/COLUMN/FEATURE）。NAND 层还实现了 cache read / cache program /
copy-back 三族 op，但只有 FC 单测在用。**能用，但不是“把 op 表里剩下的码全部打开”**：

| 优先级 | 方案 | 结论 | 主要理由 |
|---|---|---|---|
| **P1** | **GC/WL 用现有 multi-plane 写（OP 31）** | **先做** | `migrate.c` 现在逐 plane 写，一个 pair 两次 tPROG；OP 31 已实现且 host 写路径在用。GC 写吞吐估算 ~1.45× |
| **P2** | **Copy-Back 复合 op 45**（`00h-35h` + `85h-10h`） | **条件启用** | 只适合整页全有效、同 plane、非 metadata 的数据页；省 FMC data-out/data-in，但绕过 ECC/CRC 解码 |
| **P3** | **Two-plane Cache Program 新 op 44**（`80h-11h-80h-15h`） | **最后做，高风险** | 器件支持，但需新增 op + 批次提交 + 最终 `10h` 确认 + 掉电恢复；收益主要在 NAND 侧，host 受 USB FS 限制 |
| **不做** | Cache Read（13/14/15/34） | **当前不做** | 器件没有 two-plane cache read；4KB pair 跨 plane，单 plane 流水反而更慢（估算 314µs vs 302µs） |
| **不做** | ISSUE/WAIT/FETCH（35–40/43） | **`LUN_NUM=1` 不做** | 没有第二个 LUN 可交错；单 LUN 下仍是同一槽内等 WAIT |
| **不做** | Block Lock / OTP / on-die ECC | 不做数据路径 | 会永久改变介质状态；on-die ECC 出处尚未逐字核实（R12） |

⚠ **先量再改**：78 KB/s ≈ **52.5 ms/4KB**，而 NAND 单 pair typ 写约 **0.45 ms**、
USB FS 上限约 **1.1 MB/s**。52.5 ms 比 USB FS 上限还慢约 14 倍 ⇒ **当前瓶颈大概率
不是 NAND op 选择**。Phase 0 必须先分解，否则开任何 op 都看不见效果。

## 1. 背景与现状

### 1.1 硬件与系统上限

| 项 | 值 | 出处 |
|---|---|---|
| 器件 | MT29F4G08ABADAWP，SLC，1 ch / 1 CE / **1 LUN** / 2 plane | `include/nand/devices/mt29f___g08a.h:24-35` |
| 页几何 | 4 CW/页，CW=528B，页数据=2112B，pair 数据=4224B | `include/nand/nand_config.h` |
| FMC | PIO（`CONFIG_NAND_DMA=0`），200 MHz；数据阶段约 60 ns/byte | `docs/design/known_risks.md:317` 的口径 |
| 数据阶段上限 | 2112B ≈127µs，4224B ≈253µs ⇒ **≈16.2 MB/s** | 估算 |
| USB | **TUSB_SPEED_FULL（12 Mbps）**，MSC EP buffer 4096 | `include/tusb_config.h:5,38`；`src/platform/stm32h743/usb_port.c:59` |
| USB FS 上限 | payload 约 **1.1–1.2 MB/s**；每 4KB 至少 3.4–3.7 ms | 估算 |

### 1.2 当前 OP 使用矩阵

| OP 家族 | 具体 op | 生产使用 | 现状/限制 |
|---|---|---|---|
| 普通页读/写 | `12/16` | 单 plane 窗口 | 保持 |
| Multi-plane 读 | `29/30` | 4KB pair 读 | 保持；加载后逐 CW `05h/06h` 取数 |
| Multi-plane 写 | `31` | **host 写（wbuf）** | **GC/WL 未用**：`migrate.c` 逐 plane 写 |
| Multi-plane 擦 | `32` | journal/allocator | 保持 |
| Random Data Read | `8/41` | 逐 CW 取数 | 保持 |
| Cache Read | `34/13/14/15` | 仅 FC UT | 器件无 two-plane cache read；pair 读不划算 |
| Cache Program | `17` | 仅 FC UT | UOP 尾部仍等 tPROG；无 two-plane 版本 |
| Two-plane Cache Program | `80h-11h-80h-15h` | **op 表缺失** | 器件支持（待 R12 逐字复核） |
| Copy-Back | `19/10` | 仅 FC UT | 两条 op 之间不能插入同 LUN 命令 |
| ISSUE/WAIT/FETCH | `35–40/43` | 仅 `LUN_NUM>1` 改写 | `LUN_NUM=1` 下是死路径 |
| Block Lock / OTP / Enhanced Status | `21–28/7` | boot/UT | 非吞吐路径 |

### 1.3 三个关键事实

1. **GC 漏用已有 multi-plane 写**：`src/ftl/migrate.c:138-168` 的
   `mig_submit_read` / `mig_submit_write` 都是 **逐 plane** 提交；一个 pair 是两个
   独立的 tPROG。`alc_alloc_pair` 已经一次分配两个 plane 的目标页，目标地址是成对的。
2. **host 写已经没有可重叠的下一页**：USB MSC BOT 串行，`wb_flush` 在 NAND program
   完成后才回 CSW（write-through）；因此 cache program 对 host 4KB 写没有直接收益，
   除非把确认点提前到 `15h`（ADR-15 的重估项，本方案默认不做）。
3. **`cache_intermediate` 只有半成品语义**：`include/ftl/pending.h:125` 有字段，
   `src/ftl/ftl.c:374-378` 对中间页跳过 L2P/Journal，但**没有批次上下文**把中间页的
   映射统一推迟到最终页提交。直接启用 cache program 会在掉电后留下“映射指向未编程页”。

## 2. 主流控制器怎么做（出处）

> 诚实边界：专有主控固件源码不可见。以下来自公开架构资料 + ONFI/器件手册 + 本仓
> `docs/reference/` 与 `docs/design/`；不把“主流”写成“行业标准形态”（§〇 / R12）。

| 机制 | 主流做法 | 本仓出处 |
|---|---|---|
| **Multi-plane** | 一个 die 内多 plane 并行，作为基线；要求两 plane 同页地址（ONFI 3.2 起标准化） | `storage_controller_design_essentials.md §3.4`；`fc_nand_design.md §17/§25.8` |
| **Cache Read** | 顺序读时用 `31h/3Fh` 让下一页 `tR` 与当前页 data-out 重叠；多用于 read-ahead | `storage_controller_design_essentials.md §3.4`（“数据寄存器与阵列操作重叠”） |
| **Cache Program** | 顺序写/GC 搬移时让下一页 data-in 与上一页 `tPROG` 重叠；需要写缓冲或掉电保护 | 同上；ADR-15（`ftl_design.md:4357`） |
| **Copy-Back** | GC 搬整页时片上搬移，省主控总线带宽；现代主控**选择性使用**，因为绕过 ECC 解码/重编码，且不能改写页内数据 | `storage_controller_design_essentials.md §3.4/§11`；`fc_nand_design.md §17.6` |
| **Multi-die/LUN 交错** | 多 LUN 时间片掩藏 tR/tPROG | `essentials §3.4`；本仓 Phase V 已实现但 `LUN_NUM=1` 不生效 |
| **Program/Erase Suspend** | 读延迟敏感时暂停写/擦 | 本仓 op 表未实现，本方案不涉及 |

**结论**：主流不是“全开”，而是按 **介质并行度 + 写缓冲/掉电保护 + 数据是否要重编码**
三件事选。本仓当前最缺的不是“更多 op 码”，而是：
- **把已有的 multi-plane 写用到 GC**（P1）；
- **在决定 cache/copy-back 之前先量清 host 路径的真实瓶颈**（Phase 0）。

**学习轴对照（§〇 的考点）**：multi-plane → 通道/plane 并行；cache read/program →
数据面轮询与流水线；copy-back → 数据面是否经过 ECC/CRC；cache program 的批次确认 →
掉电恢复与写确认语义。这四点正是本仓目前没有真正练到的部分。

## 3. 方案选项

### 3.1 方案 A（P1）：GC/WL 用现有 multi-plane 写 `OP 31`

**现状**

`migrate.c` 的 job 按 plane 循环：读 plane0 的 victim 页 → 写 plane0 目标页 →
读 plane1 → 写 plane1。每次写是一条单 plane 命令，一个 pair 两次 tPROG。

**方案**

把 migrate 的写阶段改成 **pair 级**：

1. 两个 plane 的 victim 页都读进 `page_buf[plane]`（读仍可逐 plane）；
2. 目标页已经由 `alc_alloc_pair` 成对分配；
3. 用一条 `OP_PROGRAM_PAGE_MULTI_PLANE (31)` 同时写两个 plane 的有效 CW；
4. 位图仍是 flat CW 位图；某 plane 无有效 CW 时退化为单 plane 写（不白写空 plane）；
5. L2P/Journal/valid_count 的收尾逻辑不变。

**改动点**

| 文件 | 改动 |
|---|---|
| `src/ftl/migrate.c` / `include/ftl/migrate.h` | 写阶段改成 pair 级；新增“两 plane 都已读齐”的状态；单 plane 回退 |
| `include/ftl/ftl_fc_interface.h` / `.c` | 新增 pair 写原语（复用 `ftl_cmd_setup_ppa_pair_cw` + `ftl_cmd_write_data_pair_cw`，形状同 `wb_program_page`） |
| `src/main.c` / 内存预算 | 若需要两 plane 暂存，确认 `page_buf` 尺寸与 AXI/SRAM 预算（现 `migrate` 已有 per-plane `page_buf`，可能无需新增） |

**收益**：GC 写阶段 typ 约 **1.45×**（两次 tPROG → 一次）；worst-case tPROG=700µs
时约 1.74×。读阶段不变，所以整个 GC job 的收益小于这个数。

**风险**：migrate 状态机改动属中等；不影响主机确认语义；不新增 op 码。**先上板验证
GC 数据比对与 valid_count**。

### 3.2 方案 B（P2）：Copy-Back 复合 op

**为什么用复合 op，而不是 FTL 连发 19、10 两条命令**

`35h` 把源页读进 NAND 内部页寄存器后，`85h-10h` 必须紧接着执行；中间若插入任何同
LUN 命令，页寄存器内容会被覆盖。当前 FC 生产路径恒 `config.chain = FC_CHAIN_END`，
`fc_poll_dispatch_chain` 的原子链路径仅 UT 覆盖（`src/fc/fc.c:1592` 注释），把它作为
copy-back 的前置依赖风险偏高。更稳的做法是**新增一个复合 op**，把两段时序放进同一条
UOP 序列。

**复合 op 45 设计**

```
70h 轮询 ready
00h + C1 C2 + R1 R2 R3 (src)     ; row[0]
35h
tWB
WAIT_READY(tR)
70h + Status + CHECK_FAIL        ; 源读失败则终止，不发 85h
85h + C1 C2 + R1 R2 R3 (dst)     ; row[1]
10h
tWB
WAIT_READY(tPROG)
70h + Status + CHECK_FAIL
END
```

- `has_data=0`、`data_length=0`，不建 desc 链、不占 buffer；
- 源/目标地址分别走 `request.row[0]` / `request.row[1]`，由 `req_copy_back` 从
  `task->plane_paa[0]` / `[1]` 填入；不扩 `nand_command_request_t`；
- `op_type=ATOMIC`，不要拆 ISSUE/WAIT（拆了会在同槽内等 tPROG，等于串行化）。

**适用条件（缺一不可）**

1. **整页全有效**：该 plane 的 `cw_bitmap == (1<<CW_NUM)-1`；部分有效页回退普通读+写。
2. **同 plane**：源、目标 PBA 的 plane 位相同；跨 plane 不保证内部 data register 语义。
3. **非 metadata**：journal/anchor/checkpoint/BBT 等页含 sequence/chunk 自描述字段，
   原样搬会破坏序号；migrate 只处理数据 super block，但仍要在接口层显式拒绝
   `page_type != LP`。
4. **源页 ECC 干净**：copy-back 不经过 FC 的 ECC/CRC 解码，源页的不可纠错误会被
   **静默复制**。建议先用 Option A（先整页读验证，再发 copy-back）；Option B
   （只读 META 4 字节验证，省整页 data-out）依赖“`35h` 后还能 `05h-E0h` 读 cache”，
   必须查 datasheet 并上板确认，不能直接做。

**收益**：Option A 相对普通 GC 写 pair 约 **1.24×**；Option B 估算约 **1.95×**，
但未验证。收益来自省掉 FMC data-out/data-in，不省 tR/tPROG。

**风险**：可靠性（绕过 ECC）+ 复合 op 的地址/时序正确性 + 35h 后状态语义。若无法
证明收益 > 风险，**建议不做**或默认关闭。

### 3.3 方案 C（P3）：Two-plane Cache Program 新 op 44

**器件能力**：`docs/design/fc_nand_design.md:4862` / `docs/design/fc_nand_ut.md:276`
记录本器件有 `TWO-PLANE PROGRAM PAGE CACHE MODE 80h-11h-80h-15h`，**但没有
two-plane cache read**。⚠ `fc_nand_design.md:3307` 仍写着“硬件不支持 Multi-Plane
Cache Read/Program”，与上述结论矛盾；实施前必须按 R12 逐字复核 datasheet 并修正该行。

**op 设计**

| 项 | 值 |
|---|---|
| OP 码 | 44（`OP_COUNT` 45；先确认无墓碑槽可复用） |
| UOP | `70h轮询 → 80h+addr(P0) → DATA_IN(P0) → 11h → tADL → 80h+addr(P1) → DATA_IN(P1) → 15h → tWB → WAIT_READY → 70h+Status → CHECK_FAIL` |
| `has_data` | 1 |
| `data_length` | `PAGE_BUFFER_SIZE`（4224） |
| `data_input_cycles` | 2 |
| `op_type` | `OP_TYPE_ATOMIC`，`pair_op=0` |
| timing | tPROG 作为超时上限 |

**批次模型**

把 N 个 pair 写成：

```
cache op 44 × (N-1)              ; 每次 15h 后返回，阵列在后台做 tPROG
+ OP_PROGRAM_PAGE_MULTI_PLANE(31) ; 最后一页 80h-11h-80h-10h，起始 70h 轮询等整批完成
```

- 这样 **dispatch 仍保持“每 LUN 一条在飞命令”**；不需要双活跃槽；
- 代价是最后一页 data-in 不重叠（1/N 损失）；
- 批次成员必须在 LUN FIFO 里连续：用 FC chain（`FC_CHAIN_MORE/END`）原子提交；
  该路径当前生产零调用，必须先上板验证失败回滚、FIFO 满、完成顺序；
- **不要用 ISSUE/WAIT 拆 cache program**：`OP_PROGRAM_PAGE_ISSUE` 的
  `min_busy_us=tPROG`，`nand_dispatch_issued_advance` 会在同一槽内等 tPROG 再发
  WAIT，等于把整个 batch 串行化。

**FTL 批次语义**

- 中间页 `cache_intermediate=1`：15h 完成时**不更新 L2P/Journal、不完成 host group**；
- 最后一页 31/10h 完成时，按顺序统一更新 batch 内所有页的 L2P/Journal，再递减
  group outstanding；
- 失败按 **batch 原子**处理：任一中间页失败 ⇒ 整批不更新映射；目标块按“可能已编程”
  关闭/标记；GC 源页仍有效，重选 victim；
- **掉电**：未确认 batch 的目标页可能已编程但无映射 ⇒ 恢复时必须把它们视为已消费，
  不能重新编程（modify-after-program）。需要专门的恢复设计（扫描目标块写指针 /
  持久化 “batch in flight” 标记 / 只对 GC/WL 使用 cache program）。

**host 侧是否值得**：host 4KB 写在 BOT 串行 + write-through 下没有下一页；要形成
host 批次，需要把 `CFG_TUD_MSC_EP_BUFSIZE` 从 4096 提到 16384（或 8192），让一次
`write10_cb` 覆盖多个 pair。**但这不会突破 USB FS 的 ~1.1 MB/s 上限**，只是让 NAND
侧有机会重叠。

**收益**：NAND 写吞吐 typ 最高约 **1.8×**（458µs → 258µs/4KB pair）；worst-case
tPROG=700µs 时约 1.36×。host 可见吞吐受 USB FS 限制，收益只在 GC 与 host 竞争时体现。

**决策点**：ADR-15 目前明确不启用 cache program。要做本方案，必须先由用户决定是否
修订 ADR-15，并接受掉电窗口与 modify-after-confirm 的恢复设计。

### 3.4 方案 D：Cache Read（当前不做）

- 器件只有单 plane `31h/3Fh`，没有 two-plane cache read；
- 4KB pair = plane0 的 4 CW + plane1 的 4 CW，两个不同 row，无法用一条 cache 管线覆盖；
- 两个 plane 各跑一条串行管线：`2×(tR 25 + data-out 127 + 开销 5) ≈ 314µs`，
  与当前“加载 + 8×05h/06h 取数”约 302µs 相比**没有收益，甚至更慢**；
- 只有“同一 plane 内连续页”的顺序扫描（boot PFR/BBT）可能有用，收益约 10–20%；
- ⚠ 现有 UOP 是 `31h → WAIT_READY → DATA_OUT → 70h poll`，且每个 op 开头还有
  `70h` 轮询；若 `WAIT_READY` 等的是下一页读入 cache，则当前页数据要等下一页 tR
  完成才输出，**流水线被抵消**。即使将来做 boot 扫描，也要先按 datasheet 时序图
  确认 `31h/3Fh` 的 DATA_OUT 与 R/B# 先后，必要时新增“流水线版”UOP。

### 3.5 明确不做

| 项 | 理由 |
|---|---|
| ISSUE/WAIT/FETCH（35–40/43） | `LUN_NUM=1` 没有交错对象；`issued_advance` 仍在同槽内等 WAIT |
| host 提前 ACK / write-back | ADR-15 的掉电语义；除非正式修订并引入掉电保护/恢复不变量 |
| Two-plane Cache Read | 器件没有此命令，不要凭想象加 op |
| Block Lock / OTP 数据路径 | 会永久改变介质状态，只属工厂/测试 |
| 运行时开启 on-die ECC | 文档称多 plane/copy-back 依赖其关闭，但出处尚未逐字核实（R12） |
| 对 partial CW / metadata 页做 copy-back | 会污染新页或破坏序号/映射 |

## 4. 收益估算（每 4KB pair）

数据阶段按 FMC 200 MHz PIO、~60 ns/byte 估算；tR typ 25µs、tPROG typ 200µs
（`src/nand/onfi_1_0.c:38-40` 的 38/700/5000 是超时上限，ATOMIC op 的 `WAIT_READY`
按 R/B# 实际返回）：

| 路径 | typ 时间 / 4KB pair | typ 吞吐 | 备注 |
|---|---:|---:|---|
| 普通 multi-plane 写（31） | 253 + 200 + ~5 = **458µs** | **8.9 MB/s** | 当前 host 写 |
| Two-plane cache program（批量流水） | max(253, 200) + ~5 = **258µs** | **15.9 MB/s** | 理论上限接近 PIO |
| 普通 multi-plane 读（加载 + 8×05h/06h） | 25 + 8×(31.7+3) ≈ **302µs** | **13.6 MB/s** | 当前生产读 |
| Cache read（两 plane 串行，无跨 plane 流水） | 2×(25+127+5) ≈ **314µs** | 13.0 MB/s | 不比当前读快 |
| Copy-Back Option A（先整页验证再搬） | pair ≈ **798µs** | **5.1 MB/s** | 相对普通 GC 写 pair 992µs，约 1.24× |
| Copy-Back Option B（META-only，未验证） | pair ≈ **510µs** | **8.0 MB/s** | 约 1.95×，依赖 35h 后读 cache |
| **GC 用 multi-plane 写（31）** | pair ≈ **458µs** | **8.9 MB/s** | 相对当前逐 plane 写 2×332=664µs，约 **1.45×** |

**系统上限**：

- FMC PIO 数据阶段：4096B 用户数据 / 253µs ≈ **16.2 MB/s**；任何 cache/multi-plane
  优化都不可能超过它。
- USB FS：payload 约 **1.1–1.2 MB/s**；每 4KB 至少 3.4–3.7 ms。
- 当前 78 KB/s：4KB / 78 KB/s ≈ **52.5 ms**，约为 USB FS 上限的 1/14、NAND typ 写的
  1/115。**OP 码选择不可能解释这个数字。**

⚠ 所有数字都是上界估算；`fc_nand_ut.md:277` 明确“只验数据正确性，不验流水线收益”。

## 5. Phase 0 测量方案（Go/No-Go 门）

### 5.1 待验证假设

| 假设 | 若成立的现象 | 对应措施 |
|---|---|---|
| H1：USB BOT/FS 是主瓶颈 | 空盘顺序写也只有 ~1 MB/s 或更低；CBW 分块 4KB，每块等 CSW | 修 USB 路径（HS、增大 EP buffer、减少 BOT 往返），不做 OP 码 |
| H2：GC/写放大是主瓶颈 | 空盘快，接近满盘骤降；`info` free 低、GC 心跳密集；NAND program 次数/4KB 远大于 1 | 先做 GC multi-plane 写 + copy-back |
| H3：host 请求被拆成 512B | CBW/FTL 请求长度分布 512B 占多数；NAND program 次数 ≈ 8×host 4KB 数 | 修主机请求聚合/驱动；不要先做 cache program |
| H4：UART/日志拖慢 | `log error` 后速度明显提升；日志字节数与时间相关 | 修日志路径，默认 INFO 不逐页打印 |
| H5：FTL 同步等待/背压 | 大量 `FTL_AGAIN`、pending 满、命令池满、confirm 停泊 | 修资源预算/完成泵，不是 NAND op |

### 5.2 埋点（只加计数器，不改行为）

1. **USB**（`src/controller/usb_msc.c`）：`write10_cb/read10_cb` 的 `length` 分布、
   DWT 时间戳（回调进入 → submit → 完成观测 → `async_io_done`）、读/写分列。
2. **FTL 写路径**（`src/ftl/wbuf.c` / `ftl.c`）：submit 次数、总 LBA 数、长度分布、
   wbuf flush 次数、pair/partial 页数、host outstanding、`FTL_AGAIN`、pending 满、
   confirm 停泊时间。
3. **NAND**（`src/nand/nand_dispatch.c` / `nand_engine.c`）：每 OP 完成次数/总时间/
   DATA_IN/OUT 字节、WAIT_READY 轮询次数、每 4KB host 写对应的 NAND program 次数。
4. **GC/后台**（`src/ftl/migrate.c` / `journal.c` / `checkpoint.c`）：migrate job 数、
   每 job 页数/耗时/失败、journal/CP 页写次数、free pool 最低水位、GC 触发次数。
5. **日志**：UART 发送字节数/秒、log sink 阻塞时间；`log error` vs `log info` 对比。

建议做 **每 4KB 延迟直方图**（0–1ms、1–5ms、5–20ms、20–50ms、>50ms）。若 52ms 是
双峰（少数极慢 GC 停顿 + 多数正常），说明是 GC；若全部均匀 50ms，说明是 USB/FTL 固定
开销。

### 5.3 实验矩阵

| 实验 | 目的 |
|---|---|
| 空盘 `dd` 顺序写 64MB、`log error`、无 verify | host 路径基线 |
| 同一实验走 CLI `wr`（不经 USB） | 分离 USB 与 NAND/FTL |
| 50%/80%/近满盘重复 | GC/写放大影响 |
| H2testw | 复现用户报告；区分写阶段与校验阶段 |
| 现有 cache program UT 微基准（op 17） | 测 15h 后 R/B# 行为：每页耗时 ≈tPROG 还是 ≈tCBSY |
| 现有 copy-back UT 微基准（19→10） | copy-back 是否可用、每页耗时 |
| `log info` vs `log error` | 排除 UART |
| 观察 `info` free/op/spare、`[MIG]` 心跳 | GC 是否在 host 写期间运行 |

### 5.4 Stop/Go 判据

- **Stop（不做 OP 码）**：空盘 host 顺序写 < 1 MB/s，而 CLI `wr` > 5 MB/s
  ⇒ 瓶颈在 USB/FTL，先修系统路径。
- **Stop（先修 GC/写放大）**：NAND program 次数 / host 4KB > 4，或 GC 在 host 写期间
  持续运行 ⇒ 先做 GC multi-plane 写 + copy-back，而不是 cache program/cache read。
- **Go（GC multi-plane 写 + copy-back）**：空盘 host 写已达 USB FS 上限（~1 MB/s），
  但满盘明显下降，且 NAND 后台时间占 host 延迟 >50%。
- **Go（cache program）**：仅当 (a) 15h 微基准证明 cache-ready 早于 tPROG；
  (b) 已完成掉电恢复设计；(c) P1 之后 NAND 后台仍是瓶颈。
- **上板收益门槛**：相对基线，host 顺序写或 GC 页/秒提升 **≥15%**，且无数据完整性/
  掉电回归；否则不启用，保留 op 代码但默认关闭。

## 6. 实施顺序与文件改动

| 阶段 | 内容 | 风险 | 预期收益 |
|---|---|---|---|
| **Phase 0** | 埋点 + 实验矩阵 + Stop/Go | 无行为改动 | 找到 78 KB/s 主因 |
| **Phase 1a** | GC/WL 用现有 `OP_PROGRAM_PAGE_MULTI_PLANE (31)` 成对写两 plane | 中低 | GC 写吞吐 ~1.45× typ |
| **Phase 1b** | Copy-Back 复合 op 45，Option A（先验证再搬） | 中 | GC 每页再省约 20% |
| **Phase 2** | Two-plane cache program op 44 + 批次 + 最终 10h + 掉电恢复 | 高 | NAND 写吞吐最高 ~1.8× typ；host 受 USB 限制 |
| **Phase 3** | Cache read：仅 boot 单 plane 顺序扫描，或不做 | 低 | boot 扫描约 10–20%，非热路径 |
| **Phase 4** | `LUN_NUM>1` 后再启用 ISSUE/WAIT/FETCH、78h、interleave | 高 | 换多 die 器件后才有意义 |

**文件级改动（按阶段）**

| 文件 | 阶段 | 改动 |
|---|---|---|
| `src/ftl/migrate.c` / `include/ftl/migrate.h` | 1a/1b | 写阶段改 pair 级；copy-back kind + 回退条件 |
| `include/ftl/ftl_fc_interface.h` / `.c` | 1a/1b/2 | pair 写原语；copy-back 命令构造；cache write 选路 |
| `src/fc/fc.c` | 1b/2 | `OP_COPY_BACK_INTERNAL` 请求分支；cache op 分类/建链；链批次 |
| `include/nand/onfi_1_0.h` | 1b/2 | 新 op 码 45/44 与槽位；`OP_COUNT` 更新；先确认无墓碑槽可复用 |
| `src/nand/onfi_1_0.c` | 1b/2 | copy-back 复合 UOP；two-plane cache program UOP + op 表项 |
| `src/nand/nand_dispatch_tables.c` | 1b/2 | `req_copy_back` / req 表 / completion 表 |
| `src/nand/nand_dispatch.c` | 1b/2 | copy-back 地址 `row[0]/row[1]`；cache program 批次准入 |
| `src/ftl/wbuf.c` | 2 | 批量页提交时选 cache op；中间页 `cache_intermediate=1` |
| `src/ftl/ftl.c` | 2 | 批次上下文：中间页不更新 L2P/Journal，最终页统一提交 |
| `include/ftl/pending.h` | 2 | 复用 `cache_intermediate`；新增 batch 结构（避免扩 32B `pend_req_t`） |
| `include/tusb_config.h` / `src/controller/usb_msc.c` | 2（可选） | 若要 host 批次，EP buffer 4096 → 16384；不改 BOT 语义 |
| `docs/design/ftl_design.md` | 1~2 | ADR-15 重估；GC multi-plane / copy-back / cache program 策略 |
| `docs/design/fc_nand_design.md` | 1b/2 | 修正 `:3307` 的过期表述；补 two-plane cache program 查证结论 |
| `docs/design/known_risks.md` | 全部 | 按 R11 登记缺陷/判定/待上板项 |
| `docs/README.md` | 文档落盘时 | 登记本方案文档 |

## 7. 验证

### 7.1 编译关（R2）

- `make all`（debug）
- `make release`
- `make tests UT=FTL`（动了 `src/ftl/` 时）
- 零新告警；若 UT 因冻结/未同步测试源编不过，如实报告，不回退生产代码（R4/R2）。

### 7.2 现有 UT（不修改测试源）

- `tests/unit/test_fc_level3.c:1046-1110`：Copy-Back `19 → 10`；
- `tests/unit/test_fc_level3.c:1215-1270`、`tests/unit/fc_test_common.c:638-810`：
  cache program 写整块、cache read START/SEQ/RANDOM/LAST；
- 这些 UT 只证链接/基本路径，不证生产正确性（R2）。

### 7.3 上板功能验证

- `wr/rd/tr/fl/info` 基本读写、TRIM、flush、资源统计；
- 写满 → GC → 再读写：`info` 的 free/op/spare/closed/valid、`[MIG]` 心跳、
  无 `dangling` valid_count；
- copy-back 后逐 CW 校验 META 仍等于原 LBA、L2P 指向新 PBA、旧页失效；
- cache program batch 后逐页读回校验，确认中间页没有提前更新 L2P；
- 故意让中间页失败（坏块/重复编程），确认整批报错且不更新 L2P。

### 7.4 性能验证

- 空盘顺序写/读（`dd`/`fio`，关闭 verify）；
- 50%、80%、接近满盘重复；
- H2testw（注意它写+校验，报告值可能混合）；
- `log error` 与 `log info` 对比，排除 UART；
- 记录 GC 页/秒、host 每 4KB 延迟、NAND 每命令时间、USB CBW 分块大小。

### 7.5 掉电验证（cache program 必做）

- 在 batch 的每个阶段（15h 后、最终 10h 前、10h 中）切断电源；
- 重启后：已 ACK 的 host 写可读、L2P/Journal 一致、目标块不被重编程、无静默数据损坏；
- 重复多次；未通过前 cache program 只能标“未上板”，不得默认启用（R1）。

### 7.6 规则

- R1：上板前一律标“未上板”；声称验证通过必须给编译/串口证据。
- R2：碰建链/位图/地址必须上板。
- R4：只跑目标平台，不新增/修改测试。
- R12：协议/硬件断言先找原文；找不到就写“本项目的选择”。
- R3：不 commit/push；由用户决定。

## 8. 风险与待决策

| # | 风险 | 说明/缓解 |
|---|---|---|
| 1 | **15h 状态语义未上板确认** | 15h 后 R/B# 是 cache-ready 还是 array-ready，决定 cache program 是否有收益；先用现有 op 17 做微基准 |
| 2 | **中间页失败不可见** | 最终 10h 状态可能只反映最后一页；用坏块/重复编程做上板实验；不能确认就禁止用于 host 数据 |
| 3 | **掉电后 modify-after-program** | 未确认 batch 的目标页可能已编程但无映射；恢复必须视为已消费 |
| 4 | **批量 L2P/Journal 顺序与 group 完成** | 中间页不能递减 outstanding；最终页统一更新；半成品会导致 host 挂死或重复计数 |
| 5 | **Chain 路径生产未验证** | `fc_poll_dispatch_chain` 仅 UT 覆盖；batch 原子提交依赖它 |
| 6 | **Copy-Back 验证不足** | 只对全有效、同 plane、数据页；源 ECC 必须干净；Option B 未查原文不做 |
| 7 | **Cache read 无 two-plane 支持** | 不要套到 pair 读；单 plane 串行收益很小 |
| 8 | **on-die ECC 出处未核实** | 文档自己标注出处待重核；逐字核实前不要写成协议约束 |
| 9 | **文档旧结论误导** | `fc_nand_design.md:3307` 必须修正 |
| 10 | **FMC PIO / USB FS 上限** | host 可见上限约 1.1 MB/s；不要承诺“开 op 码到 9 MB/s” |

**待决策（需要用户拍板）**

- **D1**：是否修订 ADR-15、允许 cache program 用于 GC/WL（默认：不做，仅保留 op）。
- **D2**：Copy-Back 是否只做 Option A（先验证再搬）？Option B 需先查 datasheet。
- **D3**：Phase 0 的埋点范围（USB/FTL/NAND/GC 全量，还是先只做 USB+GC 两项）。
- **D4**：是否允许把 `CFG_TUD_MSC_EP_BUFSIZE` 提到 16384 以形成 host batch
  （会增加 USB 缓冲内存）。
- **D5**：`fc_nand_design.md:3307` 的修正措辞（按 datasheet 逐字复核后写入）。

**R11 登记计划**：Phase 0 的判定、copy-back 的条件、cache program 的掉电语义、
文档矛盾，均登记到 `docs/design/known_risks.md` 对应模块分节（状态词 + 失败场景 +
缓解 + 重访触发 + 证据定位）。

## 9. 证据定位

| 主题 | 定位 |
|---|---|
| `LUN_NUM=1` | `include/nand/devices/mt29f___g08a.h:35` |
| 单槽在飞 / ISSUE 推进 | `src/nand/nand_dispatch.c:445-482`、`:722-758` |
| cache UOP | `src/nand/onfi_1_0.c:98-119`、`:330-402` |
| op 表 | `src/nand/onfi_1_0.c:846-891` |
| tPROG/tR/tBERS 常量 | `src/nand/onfi_1_0.c:38-40` |
| FC 读/写分类 | `src/fc/fc.c:118-154` |
| ISSUE/WAIT 仅在 LUN>1 | `src/fc/fc.c:1265-1292` |
| 原子链提交 | `src/fc/fc.c:1592-1660` |
| wbuf 选 op 31 / cache_intermediate | `src/ftl/wbuf.c:76-111,190` |
| 中间页跳过 L2P/Journal | `src/ftl/ftl.c:374-378` |
| GC 逐 plane 读写 | `src/ftl/migrate.c:138-168` |
| 目标页成对分配 | `src/ftl/migrate.c:597`（`alc_alloc_pair`） |
| USB FS / MSC buffer | `include/tusb_config.h:5,38`；`src/platform/stm32h743/usb_port.c:59` |
| 主流加速机制 | `docs/design/storage_controller_design_essentials.md §3.4`（约 236-245） |
| ADR-15 | `docs/design/ftl_design.md:4357-4368` |
| copy-back | `docs/design/fc_nand_design.md §17.6`（约 3309-3330） |
| 器件 two-plane cache program / 无 cache read | `docs/design/fc_nand_design.md:4862`；`docs/design/fc_nand_ut.md:276` |
| 待修正的旧结论 | `docs/design/fc_nand_design.md:3307` |

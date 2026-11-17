# v62：开机日志补全 + 分区横幅（boot banner）

## 〇、起因

用户提出：**「当前初始化打印的信息配置是否完整，希望打印完整格式正确，方便观看」**。

逐条核对后确认两个问题：

1. **信息不全**：一批「已在内存、且有现成 getter」的开机事实没打 —— 设备总容量、活动
   ECC 引擎/强度、数据通路 PIO/DMA、扰码算法、FC 三池与队列深度、通道拓扑、CPU/HCLK
   时钟、Cache/MPU 状态、构建时间。
2. **格式不统一**：模块标签三套并存（`nand:` / `[FTL]` / `[FAULT]`），且
   `fc and nand raw path ready`、`soft_ssd console ready`、`type 'help'`、
   `btim task table ready…` **完全无标签**；无分区、条目顺序=执行顺序，不便扫读。

用户拍板：**补齐关键缺口** + **开机横幅 + 分区标题**。范围只做 `stm32h743`。

## 一、目标开机日志

```
=========== soft_ssd rev=24 built Oct 12 2026 00:20:27 ===========
[INF] [FAULT] reset reason: PIN SFT (rsr=0x01460000)
[ERR] [FAULT] previous abort (assertion/test terminal)        ← 有崩溃记录时才打（ERR 级）
--- system ---
[INF] [SYS ] CPU=400MHz HCLK=200MHz ICache=on DCache=on MPU=12
[INF] [SYS ] clk sys=400 hclk=200 pclk=100 fmc=220 qspi=200 uart=100
--- nand ---
[INF] [NAND] FMC ready, R/B#=1
[INF] [NAND] reset complete, status=0xE0
[INF] [NAND] ID=2C DC 90 95 56
[INF] [NAND] ONFI sig=ONFI rev=0x0002 model=MT29F4G08ABADAWP
[INF] [NAND] ONFI 512MiB page=2048+64 partial=512+16 block=64p blocks=4096 lun=1 addrcyc=0x23 bitcell=1
[INF] [NAND] ONFI ecc=4bit prog/pg=4 tprog=600 terase=3000 tr=25 tccs=100 timingmodes=0x003F
[INF] [NAND] keeping NAND at default timing (CONFIG_NAND_TIMING_MODE_4=0)
[INF] [NAND] init complete
--- controller ---
[INF] [FC  ] cmd=64 desc=256 buf=128 sq=64 hsq=16 cq=64
[INF] [FC  ] topology ch=1 ce=1 lun=1 plane=2 raid=off
[INF] [FC  ] data path=PIO ecc=BCH(13,4) random=LFSR 16 (x^16+x^14+x^13+x^11+1)
[INF] [FC  ] raw path ready
--- ftl ---
[INF] [FTL ] instance ch=0 ce=0 lun=0 free=3762 op=204 spare=50
[INF] [FTL ] boot type=0 cp_seq=0 journal_entries=0
[INF] [FTL ] controller ready, lba_max=1048576 (512MiB)
--- console ---
[INF] [SYS ] console ready, type 'help' for commands
[INF] [SYS ] btim task table ready, IWDG disabled for debug
```

分区归属（照 `docs/design/fc_nand_design.md` §26 分层）：
**`[SYS]`=平台/系统，`[NAND]`=器件，`[FC]`=主控（数据通路/ECC/扰码归 FC），`[FTL]`=固件层。**
横幅与 `--- 分区 ---` 用裸 `uart_log_write`（不带 `[INF]` 前缀）；信息行一律走 `LOGI`。

## 二、改动清单

| # | 文件 | 改动 |
|---|---|---|
| 1 | `src/main.c` | 新增编译期横幅 `g_boot_banner[]`（`SSD_FIRMWARE_DEBUG_REVISION` + `__DATE__` + `__TIME__`，走 `SSD_STR` 字符串化）+ 静态 `boot_section()`；`main()` 插入横幅与 5 个分区标题；新增 `[SYS ]` 时钟/Cache/MPU 行；`fc and nand raw path ready`→`[FC  ] raw path ready`；console/btim 行→`[SYS ]`；`[FTL]`→`[FTL ]`；`lba_max` 行追加 `(512MiB)` |
| 2 | `src/nand/nand_mt29f4g08abadawp.c` | 全部 `"nand: "`→`"[NAND] "`；`nand_onfi_param_print()` 几何行插 `%luMiB`（容量 = 主区×页/块×块/单元×逻辑单元） |
| 3 | `src/nand/nand_fmc.c` / `nand_engine.c` / `nand_dispatch.c` | 同步 `"nand: "`→`"[NAND] "`（含 LOGD 与一处引用旧串的注释） |
| 4 | `src/fc/fc_interface.c` + `include/fc/fc.h` | 新增 `fc_boot_report()`：池/队列深度、通道拓扑、数据通路+ECC+扰码 三行 |
| 5 | `src/fc/fc_ldpc_engine.c` + `include/fc/fc_ldpc_engine.h` | 新增 `fc_ecc_algo_name()`（与 `fc_random_algo_name()` 对称；字符串与 `fc_bch_init(13,4,…)` 就地一致） |
| 6 | `include/platform/sys.h` / `src/platform/sys.c` / `src/platform/stm32h743/sys_port.c` | 新增 `sys_mpu_region_count()`（沿既有 `sys_port_*` 模式，返回 MPU 区域表项数 = 12） |
| 7 | 同上三文件 + `src/main.c` | 新增 `sys_clock_tree_t` POD 结构 + `sys_clock_tree_get()`：`sys_port_clock_tree_get()` **实读**整棵时钟树（sysclk/hclk/pclk1..4 走 HAL getter；FMC/QSPI/USART1 读 mux 位 + HAL PLL getter 换算）；`main()` 在 `--- system ---` 下加一行（APB1/2/3/4 相等时压成 `pclk=`，不等则四个全列） |


新增信息全部接**现成 getter/常量**，无新逻辑：
- 时钟：`delay_port_core_clock_hz()` / `delay_port_hclk_hz()`（`include/platform/delay.h`）
- Cache：`get_icache_sta()` / `get_dcache_sta()`（`include/platform/sys.h`）
- MPU：`sys_mpu_region_count()`（本版新增）
- 池/队列：`FC_COMMAND_POOL_DEPTH`/`FC_DESCRIPTOR_POOL_DEPTH`/`FC_BUFFER_POOL_DEPTH`/
  `FC_SQ_DEPTH`/`FC_HSQ_DEPTH`/`FC_CQ_DEPTH`（`include/fc/fc.h`）
- 拓扑：`CH_NUM`/`CE_NUM`/`LUN_NUM`/`PLANE_NUM`（`include/nand/nand_config.h`）、`CONFIG_FC_RAID`
- 数据通路：`CONFIG_NAND_DMA`（`include/platform/dma.h`）
- ECC/扰码：`fc_ecc_algo_name()`（新）/ `fc_random_algo_name()`（已有、此前零调用者）

## 三、验证

```bash
make format && make format-check    # ✅ 通过
make clean && make tests            # ✅ 0 warning / 0 error（250856 B）
make clean && make all              # ✅ 0 warning / 0 error（172740 B，production debug）
```

**字符串核对**（`strings …/debug.elf`）：横幅 `=========== soft_ssd rev=24 built … ===========`
就位；`[NAND]`/`[FC  ]`/`[SYS ]`/`[FTL ]` 全部到位；全仓 `"nand: "` 残留 **0** 处。

**行长**：所有新增/改动 LOGI 行最长约 103 字符，远低于 `src/common/log.c:54` 的
`message[192]` 上限。

### 上板（用户跑）

✅ **开机日志与 §一 逐行一致**（横幅 `built Oct 12 2026 00:20:27`）：
`[SYS ] clk sys=400 hclk=200 pclk=100 fmc=220 qspi=200 uart=100` 三数全对
（FMC 源=PLL2_R=220 / QSPI 源=PLL1_Q=200 / USART1 源=PCLK2=100）。

⚠ 中间踩过一次「烧了旧镜像」：日志逐字相同、只有横幅时间戳不同 ⇒ **横幅时间戳正好可当
「是否真的重烧」的判据**。

完整 `test`：✅ **58 of 58 (100%)、0 skipped、869.55 ms**（本版只改打印，行为零变化）。

## 四、风险与已知影响（如实登记）

1. **`__DATE__/__TIME__` 使每次构建横幅不同** ⇒ 每次重编 `.bin` 必变（本就是现状）。
2. **标签重贴波及 NAND 模块 30 处字符串**（含 LOGD 空实现）—— 机械改动，编译期已确认。
3. **新增 4 个纯读 API**：`sys_mpu_region_count` / `sys_port_mpu_region_count` /
   `fc_ecc_algo_name` / `fc_boot_report`。均无副作用。
4. 已核对 `tests/` 对开机日志字符串**零依赖** ⇒ 重贴标签不影响单测。
5. 本版**不以 `.bin` 为判据**（横幅时间戳 + 标签重贴必漂）。
6. **⚠ 上板踩坑：`HAL_RCCEx_GetPeriphCLKFreq` 对本工程三大外设恒返回 0**。第一版用它取
   FMC/QSPI/USART1，上板打出 `fmc=0 qspi=0 uart=0`。根因：**本仓 HAL 的该函数只覆盖
   12 种外设（ADC/FDCAN/SAI×5/SDMMC/SPI×3），没有 FMC/QSPI/USART 分支**，不支持即返回 0。
   修法：改为读各自的 mux 位（`__HAL_RCC_GET_{FMC,QSPI,USART1}_SOURCE`）+ HAL 的
   `HAL_RCCEx_GetPLL{1,2,3}ClockFreq` 换算，只做 mux 译码、不重算 PLL 参数。
   副作用：`.bin` 净增从 +4100 B 降到 **+1780 B**（甩掉 2816 B 的大 switch；新增
   mux 译码 552 B + per_ck 76 B + 三个 PLL getter 2036 B）。

## 不做

- 不动 `src/ftl/**`、`tests/**`、`ut_main.c`、h7r3；不加新功能开关；不翻 `CONFIG_FC_RAID`。
- 不把 ONFI 解码结果落全局（容量在 `nand_onfi_param_print()` 内就地算；若日后要 `nand info`
  CLI 再谈持久化）。
- 不新增单元测试（项目规则：测试源文件用户自建）。

# known_iss 已闭合 / 不实现归档

> 本文件保存 `docs/design/known_iss.md` 中已闭合、用户明确不实现条目的原文。
> 主文件只保留索引表的一行；归档内容**不改写**，需要追溯时查本文件。
> 新归档条目追加到对应分节末尾。

---

## 已闭合

### ISS-UT-04 FTL 测试源按当前生产 API 全量同步

- **原状态**：已同步并上板（2026-11-11 21:22:39 `test /ftl/` 100/100）
- **原文**：FTL 测试源已按当前生产 API 全量同步并重写语义漂移项；`make tests UT=FTL` 0 error / 0 warning。板端结果：`test /ftl/` **100/100**（2026-11-11 21:22:39 镜像，pure 61 + media 39，235.70 s，0 skip）。本轮收口覆盖 teardown 强约束、临时诊断清理、强转/格式清理、`cache_wr_lookup` 与 GC/WL flush。原 103 个编译错误（`test_ftl_pure.c` 78 / `test_ftl_media.c` 25）已消除；字段映射见 `known_risks.md` §2Y RISK-UT-15~21。
- **来源**：`tests/unit/test_ftl_pure.c`、`tests/unit/test_ftl_media.c`、`tests/unit/ftl_ut_common.c`
- **归档原因**：后续 `ISS-UT-05` 已把覆盖缺口补齐并以 95/95 上板；本条作为历史里程碑保留。

### ISS-UT-05 FTL UT 覆盖缺口

- **原状态**：已闭合（2026-11-11 21:44:15 上板 95/95，0 skip）
- **原文**：① v1.10 早回执 3 用例已补：`cache_early_ack_read_visible` / `cache_failc_undo_read_error` / `cache_early_ack_flush_barrier`；② CMT 基础用例 `l2p_cmt_write_readback`（dirty 安装 → 读命中 → flush 合并 → FAILC undo 清 dirty）；③ CMT 专项已补 4 条纯 RAM 用例：`l2p_cmt_capacity_evict` / `l2p_cmt_multi_region_merge` / `l2p_cmt_entries_fallback` / `l2p_cmt_line_invalidate`，另加媒体用例 `cache_cmt_writeback_retire`；④ `host_io_matrix` 改为 count 参数化、起点按种子随机；⑤ 现有 `md_dir_ckpt_write_*`、`cp_timer_reset` 已由重建用例覆盖。
- **上板记录**：镜像横幅 `built Nov 11 2026 21:44:15`，`test /ftl/ --log-visible debug` → `95 of 95 (100%) tests successful, 0 skipped, 235.63 s`（seed `0x5a4f21e4`）。
- **来源**：`tests/unit/test_ftl_media.c`、`tests/unit/test_ftl_pure.c`；`docs/design/ftl_ut_design.md` §4.5/§7
- **归档原因**：覆盖缺口已闭合；剩余未覆盖项转入 `ISS-UT-01` 与 `docs/design/ftl_ut_design.md` §7。

### ISS-PLAT-02 删除 stm32h7r3 平台

- **原状态**：已关闭（2026-11-07）
- **原文**：用户裁决删除 `stm32h7r3` 平台：`src/platform/stm32h7r3/`、`include/platform/stm32h7r3/`、`third_party/stm32h7r3/`、`third_party/stm32h7r3_vendor.mk`、两个 `stm32h7r3xx` 链接脚本全部移除；构建配置只保留 `stm32h743`。
- **来源**：`docs/design/platform_abstraction.md` §五
- **归档原因**：用户裁决，无残余动作；历史变更记录保留在 `docs/archive/` 与 `docs/design/ftl_revision_history.md`，不回写。

---

## 不实现

### ISS-OP-06 Cache Read / ISSUE/WAIT/FETCH / Block Lock/OTP/on-die ECC

- **原状态**：未实现（边界）——当前明确不做，不属于待办
- **原文**：Cache Read、ISSUE/WAIT/FETCH、Block Lock/OTP/on-die ECC 当前明确不做。
- **重估触发**：换器件或多 LUN 后重估；不要按「op 表剩余码全开」实现。
- **来源**：`docs/archive/nand_op_enablement_plan.md` §3.4/§3.5
- **归档原因**：用户/设计明确排除，保留重估触发。

### ISS-USB-03 WRITE(10) 的 FUA 位语义

- **原状态**：不实现（用户裁决 2026-11-01）
- **原文**：WRITE(10) 的 **FUA** 位语义（只发 FUA 不发 SYNCHRONIZE CACHE）不做（保持简单）：主机 fsync 走 SYNCHRONIZE CACHE；若抓包发现只发 FUA 的主机，再按 `RISK-USB-05` 的要点补（末 chunk 等一次 flush）。
- **重估触发**：抓包发现只发 FUA、不发 SYNCHRONIZE CACHE 的主机。
- **来源**：`RISK-USB-05`
- **归档原因**：用户 2026-11-01 明确不实现。

### ISS-UT-02 FC/NAND 测试源同步

- **原状态**：待上板（测试源已同步）
- **原文**：FC/NAND 测试源已按 R4 同步：`make tests UT=FC` / `UT=HIL` 通过，新增 3 条用例（`fail_cw_attribution` / `read_column` / `process_read_ecc_max`，见 `RISK-DP-29`）。原有报错（`test_speed` / `fc_test_common` / level1 / level3 / 数据路径头拆分 / 返回值与字段改名）已全部消除。
- **上板记录**：2026-11-13 13:37:20 镜像 `test` → **61 OK + 3 SKIP**（`/fc-nand/` 59 OK + 3 SKIP，其中 `init_sequence` / `feature_write_read` / `chain_priority_lock` 因 Mode 5 按设计跳过），总 27.95 s，seed `0x5a4f21e4`；`/speed/fc_nand_rw` normal 3685/3555 KB/s、cache 4340/3614 KB/s。
- **Mode 0 专项**：2026-11-13 18:36:56 镜像（`CONFIG_NAND_TIMING_MODE=0`）`test /fc-nand/` → **62/62，0 SKIP，823.49 ms**；`init_sequence` / `feature_write_read` / `chain_priority_lock` 均 OK。
- **归档原因**：测试源同步与全套上板均完成；后续只随 FC API/测试源变化做常规回归。

---
paths:
  - "src/fc/**"
  - "include/fc/**"
  - "src/nand/**"
  - "include/nand/**"
---

# FC-NAND 规则（常驻于 FC / NAND 代码路径）

> 设计契约见 `docs/design/fc_nand_design.md`（§26 分层判据）；
> 分层决策见 `basessd-design-arch`；故障分诊见 `basessd-maintain-fault`。

## 坐标

- FC↔NAND 唯一索引空间 = **flat 绝对 CW 号** `n = plane*CW_NUM + cw`；
  行地址走 `plane_paa[plane]`，不走这个空间。
- flat 位图：plane 1 的 CW0 是 `bit 4`；`1U << cw` 是 **plane 盲**位图，禁止。
- PBA↔PAA 只在 FTL↔FC 接口的单一漏斗转换；FC / NAND 内不得另造坐标。
- 维度常量（`PLANE_NUM` / `LUN_NUM` / CH / CE）用 `for` 循环，不写死下标。

## 队列、池、完成

- 命令 / 描述符 / 缓冲三池申请释放必须配对。
- 描述符池按 `cw_bitmap` 置位数估算；满额双 plane 命令并发有上限。
- desc 链遍历必须有上界与环检测；CQ 满不得静默丢完成项（FTL 会永远等不到）。
- 完成项必须读取 NAND 状态字节（`70h` READ STATUS / `78h` READ STATUS ENHANCED；见 `include/nand/onfi_1_0.h:29,37`），不得静默丢弃。
- release 归属：`SYNC_WAIT` 不 release，其余自己 release。

## ECC / CRC / 数据面

- ECC 引擎不可用不得当成成功；CRC 初始化失败在 release 必须拒绝，
  不能让 `fc_crc_compute()` 恒返 0。
- 带 `DATA_OUT` 的稀疏 CW、单 plane cache 读 / 编程的 SEQ / LAST / RANDOM 分支必须有 `CHECK_FAIL`。
- 失败坐标必须按实际 flat CW 集合布 FAIL，不能只标最低位或 plane 0。

## 时序

- `WAIT_READY` 必须处理器件 busy 与超时；`JUMP_IF_NOT_READY` 回跳必须有 release 下溢守卫。
- timing mode 失败必须回滚到低速；tR / tWB / tPROG 与 datasheet 上限一致。
- 多 target / 多 LUN 的 bank / CE 切换必须先确认目标再发命令。

## 断言

- 内部入参不逐函数复检（H2）；子模块入口用 `ASSERT`。
- 静默介质破坏类错误必须有 release 运行期拒绝（P3 判据 9）。

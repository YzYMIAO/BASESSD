---
paths:
  - "src/ftl/**"
  - "include/ftl/**"
---

# FTL 规则（常驻于 FTL 代码路径）

> 设计契约见 `docs/design/ftl_design.md`；改动流程见 skill `basessd-develop-ftl(FTL改动)`；
> 故障分诊见 `basessd-maintain-fault(故障)`。

## 不变量：唯一闸门

- 同一不变量只能有一个读写点。allocator 的四个账本
  （`free_blocks` / `op_blocks` / `spare_blocks` / `closed_sb_list`；本仓简称「四本账」）
  只通过 `alc_free_push/remove`、`alc_op_push/alloc_log_block`、`alc_spare_push/pop`、
  `alc_super_*` 的入/出池闸门，以及只读计数 `alc_pool_sum_counts` /
  `alc_super_get_stats` 维护。
- `state == FREE ⟺ 在池`。入池前 `state == FREE` 即重复归还（C12）。
- `FTL_BLOCK_STATE_FREE` 会说谎；**选块一律以 Free Pool 数组为准**。
- 模块内部状态不直接摸数组；accessor 只在封装**数组 / 不变量 / 账本**时保留。

## 坐标

- 同一语义只允许一套坐标（C9）；PBA↔PAA 转换只在 `ftl_fc_interface.h` 单一漏斗。
- 位图是 **flat 绝对编号**：plane 1 的 CW0 是 `bit 4` 不是 `bit 0`。
- 维度常量用 `for` 循环，不写死下标（H39）。

## 持久化次序

- Journal 先于映射可见：写回 / checkpoint 的 WAL 次序不能反。
- Checkpoint 快照必须覆盖最新 Directory；CP 阶段推进只能由主循环状态机驱动。
- 早期回执（VWC）改变 flush / 断电口径：读路径必须有覆盖层；FAILC 撤销必须可跨重启。
- 改介质布局必须抬 `FTL_MEDIA_LAYOUT_VERSION`；上板首启自动格式化是预期行为，提前告知用户。
- 元数据 / 账本 / 地址三类量的改动，`make tests` 只证链接，**必须上板**（R2）。

## 断言与失败路径

- 边界 = host 入口返回码 + FTL↔FC / 子模块入口 `ASSERT`；内部不逐函数复检（H2）。
- 后果是**静默介质破坏**的，除 debug `ASSERT` 外必须有 release 运行期拒绝（P3 判据 9）。
- 被 `ASSERT` 守的变量在 `NDEBUG` 下 unused 的 release 告警不是噪音（判据 10）。
- **有副作用的调用不能进 `ASSERT`**（H14）。

## 禁止

- host 热路径不得新增 `ftl_sync_submit_wait()` 自旋；必须异步 + `FTL_AGAIN` 重试。
- L2P 预加载失败必须回退 wbuf 或返回 `FTL_AGAIN`，不得原地等待。
- 不得用第二套坐标 / 位图 / 账本。

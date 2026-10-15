# FTL 层断言瘦身：删 26 处冗余参数断言，留接口 / 子模块入口 / 高价值不变量

**日期**: 2026-10-15
**分支**: main
**类型**: 重构（断言处置，非行为变更）
**范围**: `src/ftl/` 12 个文件 + `docs/design/assertion_guidelines.md`（§二十六）

---

## 概述

用户指出 FTL 层「每个函数判断一次参数是否合理」——同一前提在调用链**多层重复断言**，
判断分支开销大；断言应主要落在对外接口，**以一个模块为界限**。经全量盘点（111 处 ASSERT，
分类 [I] 接口 27 / [BOUND] 子模块入口 10 / [INV] 不变量 48 / [REDUND] 冗余 26），
**删 [REDUND] 26 处**，保留 85 处一个不动。host 入口本就用返回码（判据 1），
内部不变量、FTL↔FC 命令边界、子模块公开入口的断言全部保留。

## 删除明细（26 处）

| 位置 | 断言 | 理由（对应判据） |
|---|---|---|
| `pending.c` 全族 13 处（init/add/lookup/remove/find_op/release_index/count/group_alloc/free/count/maybe_complete/complete_subop/complete_subop_multi） | `ftl != NULL` | 同源守卫恒真→删（§十附判据 1）：host 入口已 if-guard；`pending_init` 注释自认「唯一调用者 ftl_init 已判过」 |
| `ftl.c:248` `ftl_op.c:124` `l2p_cache.c:768` `journal.c:552` `checkpoint.c:649` `metadata.c:1522` `bbt.c:842` `wear.c:358` `migrate.c:755` `read_recovery.c:510` | `cmd != NULL` | 死守卫→删（§十附判据 2）：`fc_command_id_to_command` 公共壳已断言 ID 在池内恒非空；派发 + 9 个完成服务回调各自复检是重复 |
| `ftl.c:1238`（ftl_issue_subop）`ftl.c:1489`（ftl_drain_group） | `n_cw` 范围 | 收敛到共用漏斗（§十附判据 2）：权威校验留在 `pending_add`（pending.h 公开入口） |
| `power_failure_recovery.c:546` | `sb_idx != UINT16_MAX` | 注释自认冗余：`allocator_add_closed_sb` 内部已断言列表未满 |

同步删除只为被删断言辩护的注释（如「公共壳已断言……此处防御不可达」）；
`cmd = fc_command_id_to_command(...)` 赋值行保留（cmd 在后续完成/路由逻辑仍被使用）。

## 保留（一处不动）

- **[I] 27**：host 入口（本就无 ASSERT，if-guard+返回码）+ FTL↔FC 命令边界
  （`ftl_cmd_*` 建链位图/PBA 校验、「配置错误一律 ASSERT」）。
- **[BOUND] 10**：子模块公开入口（gc_decode_cw_lba / mig_begin_predict /
  allocator_add_closed_sb / rr_* 等）。
- **[INV] 48**：状态机非法态（`ASSERT(0)`）/ 实例归属（`&ftl->xxx == ctx`）/
  簿记计数 / 运行时上下文（`ftl_op_in_dispatch()`）。

## 验证

- `make all`（debug）编译过，`.bin` 212884B。
- `make release`（NDEBUG）编译过，`.bin` 128676B，**零新告警**（`ftl_fc_interface.c` 的
  `allowed`/`plane_bits` 未用告警是保留的 [I] 断言被裁空的**既有**告警，非本轮引入）。
- `make tests UT=FTL` 编译过（上板运行待用户）。
- grep 复核：`ASSERT(cmd != NULL)` 在 src/ftl 仅剩 6 处（全为保留的 [I]/[BOUND]）；
  `ASSERT(ftl != NULL)` 在 pending.c 归零；无连续空行。

## 固化约定（防回加）

**模块内部不逐函数复检同一入参。** 边界 = host 入口返回码 + FTL↔FC 命令边界 /
子模块公开入口 ASSERT；内部只留高价值不变量。详见 `docs/design/assertion_guidelines.md` §二十六。

## 跟进（同日，提交 `d8088ea`）：「不该发生」分支收口成 ASSERT

用户定调「那些不应该出现的分支，应该使用 assert 而不是返回」（判据 2，FC v18~v21 同法）。
完成回调里 3 处 `if (ftl == NULL) { LOGE; release; return; }`
（`ftl_op_host_io` / `ftl_op_l2p` / `ftl_op_read_recovery`）与
`pending_group_complete_subop` 的「组已释放防御早退」一并收口成 `ASSERT`：

| 位置 | 原形态 | 收口后 |
|---|---|---|
| `ftl.c` ftl_op_host_io | `if (ftl==NULL) { LOGE; release; return; }` | `ASSERT(ftl != NULL)` |
| `l2p_cache.c` ftl_op_l2p | 同上 | `ASSERT(ftl != NULL)` |
| `read_recovery.c` ftl_op_read_recovery | 同上 | `ASSERT(ftl != NULL)` |
| `pending.c` pending_group_complete_subop | `if (g->used==0U) return; /* 防御 */` | `ASSERT(g->used != 0U)` |

理由：路由表 boot 前设好、完成派发保证实例恒非空；原 release-and-return 是 debug 走不到、
release 静默吞错误的死分支。release（NDEBUG 裁空）下若真走到则 NULL 解引用留现场（可诊断停机）。

**形态 B 明确不动**：非法 phase 的 default 分支（journal/wear/bbt/checkpoint/metadata）
分支体有 `fc_command_release` + 复位 job 的资源副作用——裁掉会命令池泄漏 + job 卡死，
它们是清理逻辑不是错误返回。编译两关（all/release）过，零新告警。

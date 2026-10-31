# FTL 模块接口说明文档

> 本文档是 `ftl_design.md` 的配套文档，专注于 FTL 模块的对外接口定义、使用方法和内部模块间的接口约定。
> 实现时以此为参考，实际接口以头文件为准。
> 代码注释遵循 `CLAUDE.md` S5/S6：函数契约用 `/** */`（@brief/@param/@return），
> 实现细节用 `//`；对外契约只写在头文件。

---

## 1. FTL 对外 API

FTL 模块通过 `include/ftl/ftl.h` 对外暴露以下接口。
FTL 对外只提供**非阻塞、长度任意的接口**，不提供同步包装；**plane 配对（访问哪些物理单元）由
FTL 自行决定**，Controller 只按 FTL 实例边界切连续 run 后提交。一次 `ftl_submit_*` = 一个
**完成组**，Controller 通过 `complete_flag` 轮询或 `on_complete` 回调跟踪组完成。
（设计依据与主流对照见 `../archive/ftl_host_interface.md`。）

### 1.1 核心数据结构

```c
/* FTL 主上下文（完整定义见 include/ftl/ftl.h；模块契约见 ftl_design.md §5.1） */
typedef struct ftl_context {
    ftl_topo_t        topo;             /* 本 FTL 绑定的 NAND 芯片路由 */
    l2p_cache_t      *cache;            /* 共享 L2P cache（同一 (ch,ce) 共用） */
    pend_table_t      pending;          /* 子操作表 + 完成组（per-instance） */
    wb_handle_t      *wbuf;             /* 页打包写缓冲 */
    ftl_table_refs_t  table_refs;       /* 五张表的引用（按值保存，可审计） */
    alc_t             alloc;            /* allocator（指针已绑到 table_refs） */
    l2p_jrn_t        *journal;          /* Journal */
    md_dir_cache_t   *metadata_dir;     /* Metadata Directory */
    cp_handle_t      *cp_handle;        /* Checkpoint job */
    DWORD             total_writes;     /* 累计编程次数 */
    DWORD             dangling_total;   /* L2P 指向 valid_count==0 的累计次数 */
    bool              bg_suspend;       /* 后台写闸门（bbt --erase 置位） */
    bool              md_load_failed;   /* Directory 加载失败（RISK-BOOT-09） */
    bool              l2p_defer;        /* L2P region 加载在飞（drain 不计卡死） */
    bg_scheduler_t    bg;               /* 时间预算后台调度器（10 项任务） */
    mig_handle_t     *mig_handle;       /* GC / WL / predict 共用搬移引擎 */
    rr_handle_t       read_recovery_handle; /* Read Recovery job 状态机 */
    BYTE             *rr_page_scratch;  /* 恢复整页暂存 */
    BYTE              write_retry_buf[FTL_LBA_PER_PAIR * FTL_IU_BYTES]; /* 写失败重试页暂存 */
} ftl_ctx_t;
```

> 模块执行状态（FSM / 单飞行 / 计时器 / 进度计数）在各自 handle 内；主流程只读
> `*_status_t` 与查询，不窥探 handle 内部 phase。共享账本（L2P/BST/allocator/journal/
> metadata/pending/completion）仍是单点。

### 1.2 接口列表

| 接口 | 类型 | 说明 | 备注 |
|------|------|------|------|
| `ftl_init()` | 初始化 | 初始化 L2P cache、allocator、journal、Metadata Directory、BBT | 在 `fc_init()` 之后调用 |
| `ftl_submit_read()` | 异步读 | 非阻塞提交读，长度任意（组内自拆 plane） | L2P→PBA→FC submit→pending |
| `ftl_submit_write()` | 异步写 | 非阻塞提交写，长度任意 + Journal + watermark | alloc→FC submit→journal→L2P→pending |
| `ftl_flush_begin/step()` | 落盘 | 落已持久化映射：L2P cache 写回 + journal 尾页刷出 | 异步 job（step 每轮推进） |
| `ftl_trim_begin/step()` | 取消映射 | 取消 `[lba, lba+count)` 映射（写 NAND_PBA_INVALID 到 L2P Cache，标记 DIRTY） | 异步 job（step 每轮推进；L2P/Journal 未就绪返 FTL_AGAIN） |
| `ftl_service()` | 主循环 | 推进 FC 调度 + 统一完成泵 + **Phase 2.5 补发子操作** + 后台任务 | 在 main loop 中频繁调用 |

**注**：
- **长度交给 FTL**：`ftl_io_req_t.count` 为连续 LBA 数，FTL 自行按 **pair 窗口**（`FTL_LBA_PER_PAIR` = 8 个 CW = 2 个 plane 页）拆成子操作，各子操作长度由现场 L2P 分组/窗口边界决定。旧 `pages` 字段已删——plane 配对属「访问哪些物理单元」，是 FTL 策略，不属 HIL。
- **组完成**：一次 `ftl_submit_*` = 一个组，组内可含多个 FC 子操作；通知字段只在**全部子操作完成后触发一次**（详见 §1.4）。
- FTL 不提供同步包装（删除 `ftl_read`/`ftl_write`）。
- Controller 层通过 `ssd_controller_submit()` 异步提交请求，通过 `ssd_controller_is_complete()` 或 `on_complete` 回调跟踪完成。

### 1.3 函数签名

```c
/* ftl.h — 当前签名 */

#include "ftl_type.h"  /* ftl_io_req_t, pending_cb_t */

/**
 * @brief 非阻塞提交读请求（长度任意）。
 *
 * req->count = 连续 LBA 数；FTL 按 pair 窗口（FTL_LBA_PER_PAIR = 8 个 CW
 * = 2 个 plane 页）自拆子操作，数据缓冲长度 = req->count × FTL_IU_BYTES。
 *
 * @param ftl FTL 上下文。
 * @param req IO 请求描述（见 ftl_io_req_t）。
 *
 * @return FTL_OK 已受理（含部分子操作排队待发）；FTL_ERROR 永久失败。
 *
 * @note 返回 FTL_OK 不代表已完成，完成走组通知（§1.4）。
 */
ftl_status_e ftl_submit_read(ftl_ctx_t *ftl, const ftl_io_req_t *req);

/**
 * @brief 非阻塞提交写请求（长度任意）。
 *
 * @param ftl FTL 上下文。
 * @param req IO 请求描述（同 ftl_submit_read）。
 *
 * @return FTL_OK 已受理；FTL_ERROR 永久失败。
 */
ftl_status_e ftl_submit_write(ftl_ctx_t *ftl, const ftl_io_req_t *req);

/**
 * @brief 启动异步 FLUSH job（幂等）。
 *
 * @param ftl FTL 上下文。
 */
void ftl_flush_begin(ftl_ctx_t *ftl);

/**
 * @brief 推进异步 FLUSH job。
 *
 * @param ftl FTL 上下文。
 *
 * @return true = 本次 flush 完成。
 */
bool ftl_flush_step(ftl_ctx_t *ftl);

/**
 * @brief 启动异步 TRIM job，取消 [lba, lba+count) 的 L2P 映射。
 *
 * @param ftl   FTL 上下文。
 * @param lba   起始 LBA。
 * @param count 连续 LBA 数。
 *
 * @return FTL_OK 已启动；FTL_AGAIN 已有 job；FTL_ERROR 永久失败。
 */
ftl_status_e ftl_trim_begin(ftl_ctx_t *ftl, DWORD lba, DWORD count);

/**
 * @brief 推进异步 TRIM job。
 *
 * @param ftl FTL 上下文。
 *
 * @return FTL_OK 完成；FTL_AGAIN 本轮未完成（L2P 加载 / Journal 背压）；FTL_ERROR 失败。
 */
ftl_status_e ftl_trim_step(ftl_ctx_t *ftl);
```

**`ftl_io_req_t` 完整定义**（`include/ftl/ftl_type.h`）：

```c
typedef struct
{
    QWORD                  lba;             /* Host 逻辑地址（NVMe 协议宽度） */
    void                  *buffer;          /* 读：数据写回处；写：数据源 */
    DWORD                  count;           /* ★ 连续 LBA 数（= 上层 block_count） */
    volatile BYTE         *complete_flag;   /* 非 NULL：全部子操作完成后置 1 */
    volatile ssd_status_e *status_out;      /* 非 NULL：组完成后写入 SSD_STATUS_OK/IO_ERROR */
    pending_cb_t           on_complete;     /* 组完成后回调一次（可选） */
    void                  *on_complete_ctx; /* 回调上下文（ssd_request_t*） */
} ftl_io_req_t;
```

> **字段语义（消费级 DRAM-less）**：
> - `buffer`：读 = 数据写回处；写 = 数据源（`ftl_cmd_write_data(cmd, buffer, data_size)`）
> - `count`：连续 LBA 数，FTL 内部按 2 个一组拆成 FC 子操作；**单/双 plane 由 FTL 决定**，
>   不暴露给上层（旧 `pages` 字段已删）。子操作的 `data_size = FTL_IU_BYTES × n_cw`（n_cw 个 CW）
> - `complete_flag` + `status_out`：controller 异步跟踪双通道（完成信号 + 成功/失败），
>   **仅组完成后触发一次**
> - `on_complete` + `on_complete_ctx`：Controller 子请求聚合（`on_sub_complete` 递减 `sub_remaining`）

> **接口演进**：v0.29 `ftl_submit_read/write` 形参由 8 个散落参数收敛为 `(ftl, req)` 结构体传参；
> **v0.58（策略下沉）** `pages` → `count`（长度交给 FTL，plane 配对离开 HIL）、新增 `ftl_flush`、
> `ftl_trim` 加 `count`、引入**组完成语义**。设计依据见 `../archive/ftl_host_interface.md`。

**`ftl_init` / `alc_init` 当前签名**（`include/ftl/ftl.h` / `include/ftl/allocator.h`）：

```c
ftl_status_e ftl_init(ftl_ctx_t *ftl, ftl_topo_t topo, l2p_cache_t *cache,
                      const ftl_storage_t *storage);
void         alc_init(alc_t *alloc, ftl_topo_t topo, const ftl_table_refs_t *refs);
```

> **阶段 B/C 契约**：`cache` 是设备级共享 L2P cache（同一 `(ch,ce)` 一份，设备级 init 一次）；
> `storage` 是四个大数组的指针（tables/md/journal/checkpoint_handle），由 HIL 按域放置
> （SRAM12/SRAM4/SRAM3）。`ftl_init` 只**绑定指针**并整表 memset（NOLOAD 段不归 BSS 清）；
> `alc_init` 第三参只绑定指针、不复制。
>
> **free pool 仍由调用者回填**：`alc_init` 内部硬清零 `free_count`、`bbt` 只标坏块，
> 故 `ftl_init` 后须遍历 BST 非坏块 `alc_free_push`（见 §1.7）。

### 1.4 异步完成机制（组完成）

**组（group）**：一次 `ftl_submit_*`（= 一条 host 请求）= 一个组，组内可含多个 FC 子操作
（每个子操作 = 一条 FC 命令 = 一个 pending 槽）。**通知字段只在全部子操作完成后触发一次**，
且顺序固定：先置 `complete_flag`，再写 `status_out`，最后调 `on_complete`。

三种通知机制：

1. **`complete_flag` 轮询**：组内**全部**子操作完成后置位 `volatile uint8_t` 标志，caller 在主循环中轮询
2. **`status_out` 状态码**：写入**粘性聚合**状态——组内任一子操作失败 ⇒ 整组 `SSD_STATUS_IO_ERROR`
3. **`on_complete` 回调**：在 `complete_flag`/`status_out` 之后调用一次（Controller 子请求计数器聚合）

完成时序（组级）：
- **读组**：各子操作 NAND READ 完成 + 数据拷贝到各自 buffer slice → 全部完成后 `status_out=OK` →
  `complete_flag=1` → `on_complete(ctx)`
- **写组**：各子操作 NAND PROGRAM 完成后 → 同上
- **PBA 无效（读子操作）**：直接 memset 零 → 视同该子操作已同步完成（无需 NAND 操作）
- **NAND 失败**：读返回全零 / 写标记坏块 → 该子操作置 IO_ERROR → 组聚合为 IO_ERROR
- **TRIM**：`ftl_trim_begin/step` 异步 job（只改 L2P Cache + Journal，不写数据 NAND），
  由 Controller 每轮推进；不走读写的组完成机制，完成由 HIL 的 `complete_flag` 回执

> **背压（`FTL_AGAIN`）**：`ftl_submit_*` 受理后，子操作可能因 FC 资源暂空（命令/desc/buffer
> 池满、SQ 满、pending 表满）而**排队待发**，由 `ftl_service` Phase 2.5 补发。因此
> `ftl_submit_*` 返回 `FTL_OK` 仅表示**已受理**，不代表已完成。连续背压且无进展超过
> `FTL_DRAIN_STALL_MAX`（8 轮）⇒ 判真卡死，弃未下发者并报 IO_ERROR。

### 1.5 ftl_op 完成派发面（ctx 旁表 + op tag，v0.75 起）

`ftl_op.h/c` 提供统一完成事件分发，消除两处各自 `fc_poll_completion` 造成的
completion 丢失问题。**v0.75 起**派发**唯一出口**是 `ftl_op_dispatch()`：旁表只存 `ctx`，
handler 由命令携带的 **op tag** 查 `g_op_callbacks[]` 得到（见 `ftl_op.h`）。

```c
// ftl_op.h — 接口（v0.75 起表项只留 ctx）

#define FTL_OP_CTX_TABLE_SIZE (64U)

/* 登记一条命令的私有状态（服务函数的选择由命令的 op_tag 决定） */
ftl_status_e ftl_op_ctx_add(WORD cmd_id, void *ctx);

/* 查 + 摘链 + 返回 ctx（先摘再回调，容许服务函数重入登记） */
void *ftl_op_ctx_take(WORD cmd_id);

/* 消费所有 CQ entry → ftl_op_dispatch() */
void ftl_op_drain(void);

/* 同步等待器（FTL_OP_SYNC_WAIT 的 ctx，由同步原语在**自己栈上**持有） */
typedef struct {
    WORD                 cmd_id;
    bool                 done;               /* 服务函数置 1 */
    fc_command_status_t  completion;          /* 保存的完成状态 */
} ftl_sync_waiter_t;

// ftl_op.h — 提交侧 / 服务函数数组
ftl_status_e ftl_op_submit(fc_command_t *cmd, ftl_op_id_e op, void *ctx);
void         ftl_op_dispatch(const fc_command_status_t *completion);
bool         ftl_op_is_in_dispatch(void);
```

**路由规则**（`ftl_op_dispatch`）：
1. `op = completion->op_tag`；越界或 `g_op_callbacks[op] == NULL` ⇒ **孤儿命令**：
   `ftl_op_ctx_remove` + `fc_command_release` + LOGE（正常不应发生）。
2. 命中 ⇒ `ctx = ftl_op_ctx_take(command_id)`（**先摘链再回调**）。
3. 实例由**命令自身**的 `config.ch/ce/lun` 经 `ftl_op_route_get` 解析
   （路由表在逐实例 boot 之前设好；NULL 属编程错误）。

**op tag 的语义 = 完成服务函数选择器，不是「这是哪个功能」**：同步提交的命令一律带
`FTL_OP_SYNC_WAIT`（其服务函数 = 唤醒等待者），因为阻塞调用方的收尾是**调用者在
`ftl_sync_submit_wait` 返回后自己做的**；给同步命令打功能 tag 会让服务函数与调用者对
同一份状态各推进一次。某个功能站点被改成异步之后，该站点才把 tag 换成它的功能 op ID。

**命令 release 归属**（命令生命周期的唯一权威，写在 `ftl_op.h`）：
`FTL_OP_SYNC_WAIT` 的服务函数**不** release（调用者读完 cmd 数据才释放，否则悬垂）；
`FTL_OP_HOST_IO` 与各异步 job 的服务函数**自己** release。

**已点亮的槽**（`g_op_callbacks[]`，随各站点改造逐个填）：

| 槽 | 服务函数 | ctx | 备注 |
|---|---|---|---|
| `FTL_OP_SYNC_WAIT` | `ftl_sync_wait_callback` | `ftl_sync_waiter_t *`（调用者栈上） | 所有 `ftl_sync_*` |
| `FTL_OP_HOST_IO` | `ftl_host_io_callback` | **NULL** | 服务函数自己 `pend_find(cmd_id)`（pending 表比任何新 ctx 都全） |
| `FTL_OP_JOURNAL` | `ftl_journal_callback` | `ftl->journal` | 阶段 2 |
| `FTL_OP_READ_RECOVERY` | `ftl_read_recovery_callback` | `&ftl->read_recovery_handle` | 阶段 3 |
| `FTL_OP_GC` / `FTL_OP_WL` | `ftl_migrate_callback` | `&ftl->mig_handle` | 阶段 4，**GC 与 WL 共用同一引擎**，tag 分开只为日志可辨 |
| `FTL_OP_L2P` | `ftl_l2p_callback` | `l2p_cache_line_t *`（**该 cache line**） | 阶段 5，加载与写回共用（按 `line->state` 收尾） |
| `FTL_OP_CHECKPOINT` | `ftl_checkpoint_callback` | `ftl->checkpoint_handle` | 阶段 6 |
| `FTL_OP_BBT` | `ftl_bbt_callback` | `&ftl->alloc` | 阶段 7 |
| `FTL_OP_METADATA` | `ftl_metadata_callback` | `&ftl->metadata_dir->handle` | 阶段 8，日志块回收 / 压缩 |

⚠ `g_op_callbacks[]` 是 **C99 designated initializer 的 `const` 数组**（非「注册函数」）：
编译期即可发现重复 / 越界，无 init-order 依赖。当前 `sizeof` = 48 B = 12 项；
**`[1]`~`[11]` 全部有主**（阶段 8 是最后一阶段），`[0]` = `FTL_OP_NONE` 恒 NULL。
⚠ `FTL_OP_CHECKPOINT` / `FTL_OP_METADATA` 的服务函数**不推进状态机**（它们的 PREP 阶段含
同步原语，在完成泵里跑就是嵌套泵）——推进交给下一轮 `ftl_service` 的 `checkpoint_handle` /
`metadata_handle`；其余 job 的完成服务函数可以自行续跑。

#### 1.5.1 读失败子操作的「托管」约定（v0.75 阶段 3）

异步化后，读失败的 pending 子操作**不**在完成服务函数里当场恢复，而是原地改写成
「已被 RR job 托管」：

```c
pending->op         = PENDING_OP_READ_RECOVERY;   /* 新 op 值，与 READ/WRITE 并列 */
pending->command_id = FTL_PENDING_CMD_NONE;       /* 0xFFFF：本项已与 FC 命令脱钩 */
(void)fc_command_release(cmd);                    /* 命令当场归还池 */
rr_handle(ftl);                                     /* 交给 job，本函数返回 */
```

- **pending 项保留、组在飞数不递减** ⇒ host 读组继续 pending，直到 RR job 收尾时
  统一 `pend_release_index` + `pend_group_complete_subop_multi(owners, …)`。
- ⚠ `FTL_PENDING_CMD_NONE` 哨兵是**必需**的：命令一 release，其 `command_id`（= FC 命令池
  下标）就会被后续 `fc_command_alloc` 复用，而 `pend_find` 按 id 扫**第一项** ⇒
  陈旧 id 会让一条新命令的完成撞上这个停泊项。
- 停泊区就是 pending 表本身（深度 32）⇒ **零额外内存**；配套新增
  `pend_find_op(ftl, op, &idx)` / `pend_release_index(ftl, idx)`。
- `rr_handle` 由 `ftl_service_idle/normal/busy` 每轮调用一次（job 空闲且无托管项时只是
  一次 32 项线性扫描）。

#### 1.5.2 写确认的「停泊」约定（v0.75 阶段 5）

同构的第二处停泊：写**已编程成功**、但 L2P region 尚未就绪（正在异步加载）⇒ 不能当场
`ftl_confirm_write`（那会在完成泵里同步等 NAND）。

```c
pending->op         = PENDING_OP_WRITE_CONFIRM;   /* 新 op 值 */
pending->command_id = FTL_PENDING_CMD_NONE;
(void)fc_command_release(cmd);
return;                                           /* 组在飞数**不**递减 */
```

- 推进者**不是**外部引擎，而是 `pend_confirm_service()`——`ftl_service` Phase 2.5 的**开头**，
  按下标升序扫（停泊项的 `command_id` 已是哨兵，`pend_find` 查不到）：
  仍返 `FTL_AGAIN` 就原地留着下轮再来；否则走与 `ftl_host_io_callback` **完全相同**的收尾
  （`pend_release_index` + `pend_group_complete_subop_multi(owners, SSD_STATUS_OK)`）。
- ⚠ 顺序**必须**早于 Phase 2.5 的补发循环（读旧映射的陈旧数据问题，见 §6.1 注）。
- 停泊发生在 NAND 编程**成功之后** ⇒ 收尾的子操作状态恒 `SSD_STATUS_OK`（没有写失败分支）。

### 1.6 写完成确认（modify-after-confirm）

写请求（`ftl_submit_write`）的 Journal + L2P 更新不在提交路径中执行，而是后移到 completion handler 中、确认 NAND PROGRAM 成功后执行：

```
ftl_submit_write(ftl, &req)
  ├─ allocator_alloc() → new_pba（逐 plane 独立选块）
  ├─ fc_submit(NAND PROGRAM)            ← 异步，立即返回
  └─ return FTL_OK                      ← Journal/L2P 未更新

       ↓ 几十~几百 μs 后

  ftl_op_dispatch()
    ├─ NAND_SUCCESS → ftl_confirm_write(ftl, pending)   ← ★ v0.75 阶段 5：**两遍式**
    │   ├─ 第一遍：逐 CW l2p_cache_ready(lba) → 任一未就绪 ⇒ 返 FTL_AGAIN（**一条都不动**）
    │   │            ⇒ 本项**停泊**（op=PENDING_OP_WRITE_CONFIRM、command_id 置哨兵、
    │   │               组在飞数**不**递减），由 pend_confirm_service 每轮推进
    │   └─ 第二遍（全部就绪）：
    │        l2p_cache_read(lba) → old_pba  ← ★ 完成时**重查 L2P**（不存提交时快照）
    │        journal_append(lba, old, new)  ← 确认后记录
    │        l2p_cache_write(lba, new_pba)
    │
    └─ NAND_FAIL →
        bbt_mark_bad(new_pba.block)       ← RAM only（后台 BBT flush）
        L2P 不动 → 自动回滚（旧映射完好） ← 旧数据完好
```

⚠ **两遍式不可退化成一遍**：`l2p_cache_write` 对在飞（LOADING / FLUSHING）的行返
`FTL_AGAIN`，边查边改中途放弃会让同一页里部分 CW 已记账、部分没记，重试时**重复记账**
（`valid_count` 虚高、journal 重复条目）。两遍之间无泵（单线程顺序执行）⇒ 就绪判定与
第二遍之间状态不可能变。

**Pending 表条目**（v0.58 起**不再**内嵌 `ftl_io_req_t`——子操作字段显式化，避免 `count`
（host run 长）与子操作自身长度共用一个字段产生二义）：

```c
typedef struct {
    uint16_t used;               /* 1: 槽位占用 */
    uint16_t command_id;         /* FC command_id */
    uint16_t group;              /* ★ 所属完成组 ftl->pending.groups[] 下标 */
    uint8_t  op;                 /* ftl_pending_op_t：完成路径判别（读/写），显式给出不可反推 */
    uint8_t  n_cw;               /* ★ 本子操作搬运的 CW 数（字节数 ÷ FTL_IU_BYTES） */
    DWORD    lba;                /* 子操作起始 local LBA（CW 级：1 LBA = 1 CW = 512B） */
    void    *buffer;             /* 子操作缓冲（= 组基缓冲 + LBA 偏移 × FTL_IU_BYTES） */
    /* ↓↓↓ 写完成确认（写专用，读请求时无效） ↓↓↓ */
    DWORD new_pba;            /* 新分配 plane0 页 PBA（未覆盖 plane0 ⇒ NAND_PBA_INVALID） */
    DWORD new_pba1;           /* 新分配 plane1 页 PBA（未覆盖 plane1 ⇒ NAND_PBA_INVALID） */
    /* ⚠ 不存 old_pba：完成时按 LBA 重查 L2P 得"此刻被替换的映射"（比提交时快照更正确）*/
    uint8_t    cache_intermediate; /* 1: Cache Program 中间页（15h）：不确认 L2P/Journal、不递减组 */
} ftl_pending_req_t;
```

**完成组 `ftl_io_group_t`**（`ftl->pending.groups[FTL_GROUP_MAX]`，`FTL_GROUP_MAX = FTL_PENDING_MAX = 32`）：

```c
typedef struct {
    uint8_t  used, op, stalls;   /* 占用 / 读或写 / 连续无进展 drain 轮数（背压预算） */
    uint16_t outstanding;        /* 已下发、未完成的子操作数（被 FC buffer 池限到 ~16） */
    uint32_t total, next;        /* 本请求 LBA 总数（= count）/ 已下发到的 LBA 偏移（uint32：count 可达 1M） */
    uint64_t lba;                /* host 起始 LBA */
    void    *buffer;             /* host 基缓冲 */
    uint32_t count;              /* host 连续 LBA 数 */
    ssd_status_e agg_status;     /* 粘性错误：任一子操作失败 → 非 OK */
    volatile uint8_t      *complete_flag;   /* 全部子操作完成后置 1 */
    volatile ssd_status_e *status_out;      /* 组完成后写入 agg_status */
    pending_cb_t           on_complete;     /* 组完成后回调一次 */
    void                  *on_complete_ctx;
} ftl_io_group_t;
```

> **v0.58**：`ftl_io_req_t io` **不再内嵌**（`pending_add` 形参改为显式 `group/n_cw/lba/buffer/…`）；
> 新增 `group` 关联到完成组。组管理 `pend_group_alloc/free/maybe_complete/count` 在 `pending.c`。
> `pend_group_maybe_complete` 先发通知、**最后**才 `used=0`（回调可能重入提交）。

**坏块标记两阶段化**：

| 阶段 | 位置 | 操作 | NAND 访问 |
|------|------|------|-----------|
| 快速路径 | `ftl_host_io_callback` | BBT bitmap 置位 + allocator 标记 | ❌ 无 |
| 后台持久化 | `ftl_service_idle/normal` → `bbt_handle()`（v0.75 阶段 7 起为异步 job） | 攒批写 NAND BBT（池 56-57） | ✅ 一次一页，提交即返回 |

**回滚**：写失败后 L2P 不动（仍指向被替换前的映射），Super Block 空洞由 GC 扫描时读失败自然跳过。无需额外回滚逻辑。

### 1.7 Pending 表完整数据结构

**v0.58 起**：多 LBA 请求的**长度交给 FTL**——Controller 只在**实例边界**切连续 run，每个 run
一次 `ftl_submit_*(count=run_len)`；FTL 内部再拆成 FC 子操作：**每个子操作 = 一个 pair 窗口内的
连续 LBA 段**，消费 1..`FTL_LBA_PER_PAIR`（= `PLANE_NUM × CW_NUM` = **8**）个 CW
（读路径按 L2P 分组收缩、写路径按 pair 窗口边界收缩，见 `ftl_issue_subop`）。**plane 配对彻底离开 HIL**。

**数据流**：

```
ssd_controller_submit(req{block_count=N})
  ├── buffer_size 校验：block_count × FTL_IU_BYTES(512) ≤ buffer_size，否则拒
  └── ssd_request_split()（READ/WRITE 统一路径，含单块）
        ├─ sub_remaining = ssd_split_count()      // ★ 先精确预统计 run 段数（同步完成会立即递减）
        ├─ 逐 run：
        │    request->io.lba    = run_start;
        │    request->io.buffer = base + (run_start - start_lba)*FTL_IU_BYTES;
        │    request->io.count  = run_len;        // ★ 长度交给 FTL，plane 拆分由 FTL 决定
        │    ftl_submit_read/write(ssd_controller_route(ctrl, run_start), &request->io)
        └─ 每个 run（= 一个组）完成 → on_sub_complete → sub_remaining--
              → sub_remaining == 0 → req->complete_flag = 1
```

**关键实现** (`src/controller/request.c`)：

```c
static void on_sub_complete(void *ctx)
{
    ssd_request_t *req = (ssd_request_t *)ctx;
    if (req->sub_remaining > 0U)
        req->sub_remaining--;
    if (req->sub_remaining == 0U)
        req->complete_flag = 1U;
}

int ssd_request_split(ssd_controller_t *controller, ssd_request_t *request)
{
    /* 子请求数 = 实例边界切出的 run 段数（ssd_split_count），**必须**先于提交精确置位：
     *   - 未映射读会同步完成（组通知同步触发 on_sub_complete）→ 计数为 0 时提前置完成
     *   - 中途提交失败时把 sub_remaining 收敛到"实际在飞数"（= submitted），
     *     避免父请求提前完成（旧实现直接置 complete_flag 即此缺陷）。 */
    request->sub_remaining = ssd_split_count(controller, request);
    ...
}
```

写请求同理（`complete_flag=NULL`，子请求用 `on_complete` 聚合，不用轮询）。

**结构体变化** (`ssd_request_t`)：

```c
typedef struct {
    ssd_request_opcode_t opcode;
    uint64_t             start_lba;
    uint32_t             block_count;
    void                *buffer;
    size_t               buffer_size;
    ssd_status_e         completion_status;
    volatile uint8_t     complete_flag;
    volatile uint32_t    sub_remaining;      /* 未完成的子请求数 */
    ftl_io_req_t     io;                 /* v0.29: 子请求复用模板 */
} ssd_request_t;
```

```c
/* 系统启动（v0.37 生产路径：见 src/main.c ssd_ftl_boot / ssd_ftl_boot_all） */
fc_init();                              // ① FC 初始化
nand_init();                            // ② NAND 初始化
nand_dispatch_set_armed(1U);            // ③ 放行 NAND 异步提交（submit / submit_chain）

/* 每 (ch,ce,lun) 一个 FTL 描述符（多维声明，宏改动即扩展）
 * 阶段 C：g_ftl / g_ftl_wbuf / g_ftl_journal 定义在 ftl.c（DTCM），
 * g_ftl_md 定义在 metadata.c（SRAM4）；五张表落 SRAM12、cp_handle 落 SRAM3。
 * 由 ftl_storage_t 绑定。 */
extern ftl_ctx_t             g_ftl[CH_NUM][CE_NUM][LUN_NUM]; /* DTCM */
static ftl_block_t   g_ftl_blocks[FTL_DIM_TOTAL][FTL_BLOCK_TOTAL] SRAM12_DATA;
static WORD          g_ftl_free_blocks[FTL_DIM_TOTAL][FTL_BLOCK_TOTAL] SRAM12_DATA;
static WORD          g_ftl_op_blocks[FTL_DIM_TOTAL][PLANE_NUM][FTL_OP_POOL_CAP] SRAM12_DATA;
static WORD          g_ftl_spare_blocks[FTL_DIM_TOTAL][PLANE_NUM][FTL_SPARE_POOL_PER_PLANE] SRAM12_DATA;
static super_block_t g_ftl_closed_sb_list[FTL_DIM_TOTAL][SUPER_BLOCK_LIST_MAX] SRAM12_DATA;
static checkpoint_handle_t              g_ftl_checkpoint_handle[FTL_DIM_TOTAL]  SRAM3_DATA;
extern l2p_jrn_t             g_ftl_journal[FTL_DIM_TOTAL];   /* DTCM */
extern md_dir_cache_t        g_ftl_md[FTL_DIM_TOTAL];        /* SRAM4 */
static ssd_controller_t      g_ctrl;

/* ④ FTL 初始化（阶段 C：拓扑 + 共享 cache + storage）
 *    内部：绑定 storage → 五张表逐表 memset → pend_init
 *        → alc_init（绑定 table_refs，free pool 空）
 *        → journal_init → metadata_dir_load → bbt_init（填充 BST bad 位） */
const uint32_t dim = ftl_topo_dim((ftl_topo_t){.ch = ch, .ce = ce, .lun = lun});

ftl_table_refs_t refs;
refs.blocks         = g_ftl_blocks[dim];
refs.free_blocks    = g_ftl_free_blocks[dim];
refs.op_blocks      = g_ftl_op_blocks[dim];
refs.spare_blocks   = g_ftl_spare_blocks[dim];
refs.closed_sb_list = g_ftl_closed_sb_list[dim];

ftl_storage_t storage;
storage.table_refs        = &refs;
storage.metadata_dir      = &g_ftl_md[dim];
storage.journal           = &g_ftl_journal[dim];
storage.checkpoint_handle = &g_ftl_checkpoint_handle[dim];
ftl_init(&g_ftl[ch][ce][lun], (ftl_topo_t){.ch = ch, .ce = ce, .lun = lun},
         &g_l2p_cache[ch][ce], &storage);

/* ⑤ free pool 回填（v0.37 最大缺口）：
 *    alc_init 不填 free pool（free_count 硬清零）、bbt 只标坏块不产 free_blocks。
 *    必须遍历 BST 非坏块逐个 alc_free_push，否则首次写 allocator_alloc 失败。 */
for (uint16_t g = 0U; g < FTL_BLOCK_TOTAL; g++) {
    if (!ftl_block_is_bad(&g_ftl[ch][ce][lun].table_refs.blocks[g])) {
        alc_free_push(&g_ftl[ch][ce][lun].alloc, g);
    }
}

pfr_boot_recovery(&g_ftl[ch][ce][lun], &result);  // ⑥ 启动恢复（FRESH/CLEAN/POWER_FAILURE）

/* ⑦ controller 绑定：g_ftl_ptrs 三维指针表（类型匹配 ssd_controller_init 形参） */
ssd_controller_init(&g_ctrl, g_ftl_ptrs);

/* 主循环（企业级异步模式；v0.37 g_ftl_ready 守卫） */
while (1) {
    cli_service_poll();                 // 收 Host 命令 → ssd_controller_submit()
    if (g_ftl_ready) {
        ftl_service_pump();             // ⑧ 实例无关：NAND 推进 + 完成表抽干（每轮一次）
        for (ch = 0; ch < CH_NUM; ch++)     // ⑨ 逐实例：补发子操作 + 三级后台
            for (ce = 0; ce < CE_NUM; ce++)
                for (lun = 0; lun < LUN_NUM; lun++)
                    ftl_service(&g_ftl[ch][ce][lun]);
    } else {
        (void)fc_service();             //    boot 失败降级裸 fc_service
    }
    /* 请求完成状态由 ssd_controller_is_complete() 轮询 */
}
```

---

## 2. L2P Cache 内部接口

### 2.1 数据结构

```c
#define L2P_ENTRIES_PER_REGION   (PAGE_MAIN_SIZE / sizeof(uint32_t))   // 512
#define L2P_REGION_SIZE          (L2P_ENTRIES_PER_REGION * 4U)         // 2048 B
#define L2P_CACHE_SIZE           (64U)   // cache 行数（32/64/128）
#define L2P_REGIONS_PER_INSTANCE (FTL_CW_TOTAL / L2P_ENTRIES_PER_REGION)  // 2048（每 (ch,ce,lun) 一份）
#define L2P_REGION_PER_BLOCK     (FTL_SLOTS_PER_BLOCK)                 // 128
#define L2P_BLOCKS               ((L2P_REGIONS_PER_INSTANCE + L2P_REGION_PER_BLOCK - 1U) / L2P_REGION_PER_BLOCK)  // 16（打包）
//   ↑ `+ L2P_REGION_PER_BLOCK - 1U` 是向上取整 ceil(a/b) ≡ (a+b-1)/b：整数除法截断成
//     floor，除不尽时会少算一块、丢掉尾部 region。当前 2048 ÷ 128 恰好整除 ⇒ 该项不起作用。
#define L2P_BASE_BLOCK           (FTL_METADATA_BASE_BLOCK)             // 0

// Cache line 状态
typedef enum {
    L2P_CACHE_INVALID  = 0,    // cache 行无效
    L2P_CACHE_CLEAN    = 1,    // 有效，未修改
    L2P_CACHE_DIRTY    = 2,    // 有效，已修改（需写回）
    L2P_CACHE_FLUSHING = 3,    // 正在写回 NAND
} l2p_cache_state_t;

/* 单条 cache line = 2KB + 元数据 */
typedef struct {
    DWORD  entries[L2P_ENTRIES_PER_REGION];   // 512 × 4B = 2048B
    uint32_t    region_id;                           // **全局** region（共享 cache 的键）
    uint8_t     state;                               // l2p_cache_state_t
    uint16_t    access_count;                        // CLOCK 访问计数
} l2p_cache_line_t;

/* 整体 cache（阶段 B 起：同一 (ch,ce) 下所有 LUN 共享） */
typedef struct {
    l2p_cache_line_t  lines[L2P_CACHE_SIZE];
    uint16_t          clock_hand;        // CLOCK 替换指针
    uint32_t          hit_count;         // 统计（开机累计）
    uint32_t          miss_count;
    // P1-4 thrashing 检测窗口（阈值见 l2p_cache.h L2P_THRASH_*）
    uint32_t          window_access;     // 本窗口访问数
    uint32_t          window_miss;       // 本窗口 miss 数
    uint8_t           miss_pct;          // 最近一次窗口结算 miss 率（0..100）
    uint8_t           thrashing;         // 1 = 降级态（停 NORMAL 主动 flush）
    uint32_t region_dirty_bitmap[L2P_DIRTY_BITMAP_WORDS]; // **全局** region 脏位图
    uint8_t  ch, ce;                     // 本 cache 服务的 (ch,ce)（跨 LUN 共享，无 lun）
    l2p_owner_fn_t owner_of_region;      // 全局 region → owner 实例（设备级注入）
    void          *owner_ctx;
} l2p_cache_t;

/* 全局 LBA → (ch,ce,lun) 路由（行业主控哈希；LUN_NUM=1 时恒 0） */
static inline uint32_t lba_route_ch(uint64_t lba);
static inline uint32_t lba_route_ce(uint64_t lba);
static inline uint32_t lba_route_lun(uint64_t lba);

/* 全局 region → owner die 内的局部 region（映射页落盘位置；单一漏斗） */
static inline uint32_t l2p_global_to_local_region(uint32_t global_region);

/* 全局 LBA → 片内 local LBA（地址原语；阶段 B 起数据通路不再调用） */
static inline DWORD lba_to_local(uint64_t lba);

/* ⚠ 本仓没有"region 的固定位置"：未写过的 region 在 Metadata Directory 里登记为
 * NAND_PBA_INVALID（未映射，读取不产生 NAND I/O）；写过的 region 只在日志块里，
 * 位置由 Directory 携带。 */
```

### 2.2 接口（⚠ v0.75 阶段 5 起**三态**）

```c
/* 初始化**共享** cache（设备级每个 (ch,ce) 一次；所有 line 置 INVALID；
 * 注入 全局 region → owner 的解析回调。不能放进逐实例 ftl_init） */
void l2p_cache_init(l2p_cache_t *cache, uint8_t ch, uint8_t ce,
                    l2p_owner_fn_t owner_of_region, void *owner_ctx);

/*
 * 读取 LBA 的 PBA 映射 —— 三态（v0.75 阶段 5 起，见下）。
 *   FTL_OK    命中（CLEAN / DIRTY / FLUSHING）→ *out = 条目值
 *   FTL_AGAIN region 未驻留（**本次已起异步加载**）或正在加载 → *out 未动
 *   FTL_ERROR 参数非法 / 越界 → *out = NAND_PBA_INVALID（**也回填**）
 * ⚠ FTL_OK + NAND_PBA_INVALID = **未映射**；FTL_AGAIN = **还不知道**。绝不可混同。
 */
ftl_status_e l2p_cache_read(l2p_cache_t *cache, DWORD lba, DWORD *out);

/*
 * 就绪前置探针：lba 所在 region 是否已在 RAM。
 * 命中（CLEAN / DIRTY）→ 1；未命中 → 起异步加载并返回 0；LOADING / FLUSHING → 0。
 * 供「先就绪、再一次性应用多条改动」的调用者用（写确认 / 搬移收尾）。
 * ⚠ 纯探针：命中不记账、不消费 fresh、不动 access_count；未命中记一次 miss。
 */
int l2p_cache_ready(l2p_cache_t *cache, DWORD lba);

/*
 * 写入 LBA 的 PBA 映射（三态）。
 * 只改 cache line + 置 DIRTY + region 脏位，不写 NAND（写回由 flush 起）。
 * 行在飞（LOADING / FLUSHING）→ FTL_AGAIN 且**不改任何状态**。
 */
ftl_status_e l2p_cache_write(l2p_cache_t *cache, DWORD lba, DWORD pba);

/*
 * 起一行 DIRTY 的**异步**写回（提交即返回；完成由 ftl_l2p_callback 收尾）。
 * ⚠ 三态：FTL_OK = 已提交；FTL_AGAIN = 取不到空槽（池空 / 整理 job 在飞 / 刚换块且池吃紧）
 *   ——**背压不是失败**，下轮重试（v0.75 阶段 8 前返回 FTL_ERROR，会让 CP 误中止）；
 *   FTL_ERROR = 建链失败 / 无依赖。
 */
ftl_status_e l2p_cache_flush_kick(l2p_cache_t *cache, int line_idx);

/* 后台写回推进：每轮 ftl_service 调一次，只处理本 owner，最多起 max_kick 行 */
void l2p_owner_flush_kick(l2p_cache_t *cache, uint32_t owner_dim, uint8_t max_kick);

/* ── 同步逃生口（自旋泵；只许结构性同步上下文，服务函数禁用）── */
ftl_status_e l2p_cache_read_sync(l2p_cache_t *cache, DWORD lba, DWORD *out);
ftl_status_e l2p_cache_write_sync(l2p_cache_t *cache, DWORD lba, DWORD pba);
ftl_status_e l2p_cache_flush_one_sync(l2p_cache_t *cache, int line_idx);
ftl_status_e l2p_cache_flush_owner_sync(l2p_cache_t *cache, uint32_t owner_dim);

/* 恢复/格式化：只失效本 owner 的行与脏位（不能整 cache re-init） */
void l2p_cache_invalidate_owner(l2p_cache_t *cache, uint32_t owner_dim);
/* Checkpoint 快照：全局脏位图 → 本 die 的局部 2048-bit 位图 */
void l2p_cache_snapshot_owner_bitmap(const l2p_cache_t *cache, uint32_t owner_dim,
                                     uint32_t out[L2P_LOCAL_DIRTY_WORDS]);
```

### 2.3 内部挂载点

```
l2p_cache_read()       → 11 个调用点全部按三态改写（host 读下发 / 写确认 / TRIM /
                         RR / 搬移 SETTLE / 开机回放 / full metadata rebuild / CLI map）
l2p_cache_write()      → 写确认 / 搬移 WRITE_SETTLE / journal_replay（F10：回放写 cache）
l2p_cache_flush_kick   → l2p_cache_evict_one（CLOCK 驱逐脏行时）、l2p_owner_flush_kick
l2p_owner_flush_kick / jrn_handle（异步推进）→ ftl_flush_begin/step（HIL `fl` / USB flush）
l2p_cache_flush_one_sync → l2p_cache_flush_owner_sync / Checkpoint（逐行）
                          （CP job 的 CP_FLUSH_REGIONS 用 l2p_cache_flush_kick：Header 落盘前
                           保证 region 已持久，且不阻塞完成泵）
l2p_cache_invalidate_owner / snapshot_owner_bitmap → 开机恢复 / Checkpoint
```

### 2.4 Region 加载流程（v0.75 阶段 5 起**异步**）

```
l2p_cache_load_start(cache, region_id):            ← static，由 read/ready/write 的 miss 分支调
  l2p_cache_evict_one(cache)  → CLOCK 扫
    → LOADING/FLUSHING: 跳过 | access_count>0: --, 下一个 | INVALID: 直接占
    → DIRTY: **起异步写回（flush_kick）后跳过**（本轮不复用）
    → CLEAN(ac==0): 直接占
    全不可驱逐 ⇒ 返回 0（调用者下轮重试；收敛性由 L2P_FLUSH_MAX_INFLIGHT 保证）

  占位后:
    metadata_dir_lookup(region_id) → 查 Directory
    ＝ INVALID → entries 已是全 INVALID ⇒ 直接 state = CLEAN, fresh = 1，返回
                 （未映射；**不提交读命令、不占 load_inflight**）
    否则 → ftl_async_read_page(addr) + ftl_op_submit(tag=FTL_OP_L2P, ctx=line)
           state = LOADING；load_inflight++
           ⇒ **立即返回 FTL_AGAIN**（不等 NAND）

  完成时（g_op_callbacks[FTL_OP_L2P] = ftl_l2p_callback）:
    成功 → ftl_cmd_read_data 拷进 line->entries（必须早于 release）
    失败 → entries 保持全 INVALID
    两者都 → state = CLEAN；fresh = 1；load_inflight--
```

⚠ 加载**失败**刻意置 `CLEAN`（= 全部未映射）而非 `INVALID`：后者会让每次访问都重试加载
（坏 region 无限重读 NAND），异步化后更会让 host 读**永久停在 `FTL_AGAIN`**。

---

## 3. Journal 内部接口

### 3.1 数据结构

> **权威定义在 `include/ftl/journal.h`**（本文不复制结构体/签名，避免两处漂移）。
> 这里只给页格式常量与阅读要点。

```c
#define JOURNAL_BLOCK_COUNT       (CW_NUM)                       /* 环形 4 blocks（= CW_NUM） */
#define JRN_PAGE_SIZE             (PAGE_MAIN_SIZE)               /* = 2048 */
#define JRN_HEADER_SIZE           (16U)                          /* magic+page_seq+entry_count+block_seq+base_lba */
#define JRN_ENTRIES_PER_PAGE      ((JRN_PAGE_SIZE - JRN_HEADER_SIZE) / 16) /* 通用页推导 = 127 */
#define JRN_SEG_ENTRIES_PER_PAGE  ((JRN_PAGE_SIZE - JRN_HEADER_SIZE) / 4)  /* 顺序映射页推导 = 508 */
#define JRN_MAGIC                 (0x524F554AUL)                 /* "JOUR"：LBA 显式页 */
#define JRN_MAGIC_SEG             (0x4745534AUL)                 /* "JSEG"：顺序流映射页 */
#define JRN_BLOCK_NONE            (0xFFU)                        /* 「无预擦除块」哨兵 */
```

| 对象 | 要点 |
|---|---|
| 两种页型 | `JOUR`：16 B/条（lba / old_pba / new_pba / sequence），127 条；`JSEG`：4 B/条（new_pba），508 条，lba = base_lba + 下标。两种页不混装 |
| `jrn_handle_t.current_entry` | **WORD**（顺序页可到 508）；`page_kind` / `page_base` 是当前页缓冲的页型与顺序页基准（首条 append 落定） |
| 预留 / 追加 | 通用页：`jrn_reserve` + `jrn_append`；顺序写确认：`jrn_seq_begin` / `jrn_seq_reserve` / `jrn_seq_ready` / `jrn_append_seq` |
| 回放 / 截断 | `jrn_page_scan_latest` → `jrn_resume` → `jrn_replay`；`jrn_truncate_plan/commit` |

### 3.2 接口

> 见 `include/ftl/journal.h` 的 Doxygen 契约（按页型分四组：通用页、顺序映射页、
> 查询/截断、回放）。本文不复制签名——签名仍在演进（见 `ftl-api-change-pending` 的约定）。

### 3.3 Page 格式

```
Header (16B, 两种页型共用):
  magic       (4B) = 0x524F554A ("JOUR" 通用页) / 0x4745534A ("JSEG" 顺序映射页)
  page_seq    (4B) = 本页第一条 entry 的全局序号
  entry_count (2B) = 本页有效 entry 数（JOUR 0~127 / JSEG 0~508）
  block_seq   (2B) = 本 Journal Block 写入轮次
  base_lba    (4B) = JSEG 的首条 LBA（JOUR 恒 0）

JOUR Entries (N × 16B, N ≤ 127):
  lba(4B) | old_pba(4B) | new_pba(4B) | sequence(4B)

JSEG Entries (N × 4B, N ≤ 508):
  new_pba(4B)；lba = base_lba + 下标，sequence = page_seq + 下标（隐式坐标）

Padding: 余量 = 0xFF
```

`JSEG` 是**顺序写确认专用**的 Journal 页（cache program 页）：按提交序到达的确认条目
LBA 连续，故可省掉 lba/old_pba/sequence，每页容纳 508 条（≈ 63.5 个 pair 页）。
`base_lba` 由首条 append 落定；回放时 `old_pba` 由 L2P 基线现读（见
`docs/design/ftl_design.md` §3.7.3 / §3.16.5）。

---

## 4. Allocator 内部接口

### 4.1 数据结构

```c
/* 温度等级 */
#define FTL_TEMP_HOT    0U      /* 用户 IO 数据 */
#define FTL_TEMP_COLD   1U      /* GC 搬移 / 元数据 */

/* Free Pool 三级水位（一个已擦除池，见 ftl_design §3.7） */
#define FREE_POOL_TARGET        (32U)  /* 水位①/②分界 */
#define FREE_POOL_EMERGENCY     (8U)    /* 水位②/③分界 */
#define SUPER_BLOCK_LIST_MAX    (FTL_BLOCK_TOTAL) /* Closed SB 列表容量 = 块总数（结构上界，见 §3.6） */

/* OP / Spare 池（**按 plane 各留一份**，见 ftl_design §3.6） */
#define FTL_OP_PERCENT           (10U)  /* OP 预留百分比（按 plane 计）*/
#define FTL_OP_BLOCKS_PER_PLANE  ((BLOCKS_PER_PLANE * FTL_OP_PERCENT) / 100U)  /* = 204/plane */
#define FTL_OP_POOL_CAP          ((FTL_OP_BLOCKS_PER_PLANE) > 0U ? (FTL_OP_BLOCKS_PER_PLANE) : 1U)
#define FTL_SPARE_POOL_PER_PLANE (50U)  /* 每 plane 坏块替换池（总 100）*/

/* Super Block */
typedef struct {
    ftl_topo_t topo;                 /* 本 SB 绑定的 NAND 芯片路由 */
    uint16_t plane_blocks[PLANE_NUM];   /* 各 Plane 的 Block 号；可为 FTL_BLOCK_NO_PLANE（退化单 plane）*/
    uint8_t  next_page;                  /* 当前写入页（0~63，双 Plane 同步）*/
    uint8_t  temperature;               /* FTL_TEMP_HOT / FTL_TEMP_COLD */
    uint8_t  valid;                     /* 1=Open, 0=Closed */
} super_block_t;

/* Allocator 上下文 */
typedef struct {
    uint8_t     ch, ce, lun;            // 拓扑参数
    uint16_t    total_blocks;           // 总好 Block 数
    uint16_t   *free_blocks;            // → tables.free_blocks（P2-5：绑到实例自持表）
    uint16_t    free_count;             // 空闲（且已擦除）block 数——三级水位都建在它上面
    uint16_t  (*op_blocks)[FTL_OP_POOL_CAP];             // → tables.op_blocks（GC 缓冲池，**每 plane 一份**）
    uint16_t    op_count[PLANE_NUM];                     // 各 plane OP 池剩余 Block 数
    uint16_t  (*spare_blocks)[FTL_SPARE_POOL_PER_PLANE]; // → tables.spare_blocks（坏块替换池，**每 plane 一份**）
    uint16_t    spare_count[PLANE_NUM];                  // 各 plane Spare Pool 剩余 Block 数
    super_block_t hot_sb;               // Hot Open Super Block
    super_block_t cold_sb;              // Cold Open Super Block
    super_block_t *closed_sb_list;      // → tables.closed_sb_list
    uint16_t    closed_sb_count;
    ftl_block_t *blocks;               // → tables.blocks（BST 20B：erase_count/last_pe_ts/read_count/valid_count/state/sb_id/rr）
    uint16_t    bad_count;             // 坏块数
    bbt_state_t  bbt;                  // BBT 池状态（序 4：per-instance，原 bbt.c 的 file-scope static）
} alc_t;
```

### 4.2 接口

```c
/* 初始化 allocator（P2-5：形参 = 拓扑 + 自持表指针；第三参只绑定不复制） */
void alc_init(alc_t *alloc, ftl_topo_t topo, const ftl_table_refs_t *refs);

/*
 * 分配一页的**各 plane** PBA（CW 级映射 + Policy B：每 plane 独立块）。
 * 从当前温度的 Open Super Block 顺序分配；Super Block 写满 → 关闭（→Closed），
 * 从 Free Pool 选新 Block。Free Pool 耗尽 → 返回 FTL_AGAIN（瞬态资源结果）。
 * （旧的单 plane `allocator_alloc` 已删，见 v0.56/v0.65。）
 */
ftl_status_e allocator_alloc_pair(alc_t *alloc, uint8_t temperature,
                                  DWORD out[PLANE_NUM]);

/* GC 擦除后归还 block 到 Free Pool */
void alc_free_push(alc_t *alloc, uint16_t gblock);

/* 运行时标记坏块（已删除：统一走 bbt_mark_bad，BBT 单数据源）*/
/* 检查是否需要 GC（已删除：GC 触发单一数据源 = ftl/gc.h 的 gc_should_trigger，
 * 唯一求值点 = ftl.c 的 mig_gc_evaluate，v0.63）*/
```

---

## 5. FTL → FC 接口映射

### 5.1 PBA 操作 Helper

见 `include/ftl/ftl_fc_interface.h`：

```c
/* PBA→CH/CE/PAA + descriptor 链配置（static inline，include/ftl/ftl_fc_interface.h） */
static inline ftl_status_e pba_to_cmd(fc_command_t *cmd, DWORD pba);

/* 用户数据写入 desc buffer */
int ftl_cmd_write_data(fc_command_t *cmd, const void *data, uint32_t length);

/* 从 desc buffer 读取用户数据 */
int ftl_cmd_read_data(fc_command_t *cmd, void *buffer, uint32_t length);
```

### 5.2 地址入口：`pba_to_cmd`（v0.73 起）

见 `include/ftl/ftl_fc_interface.h`。**FTL 层地址一律 `DWORD`**——遍历路径不再
经 `nand_addr_t` 展开，直接由 PBA 填 FC 命令；PAA 只在 `pba_to_cmd` 内部
（`pba_to_paa()` 一次）转出，FC 层只见 PAA：

```c
/* DWORD → FC 命令（ch/ce/lun → config 位域，block/plane/page/cw → paa） */
ftl_status_e pba_to_cmd(fc_command_t *cmd, DWORD pba);

/* FC 命令 → DWORD（反向：ch/ce 取 config 位域，其余由 paa_to_pba() 反重排） */
DWORD ftl_cmd_pba(const fc_command_t *cmd);

/* Super Block 中指定 Plane/Page 的 PBA（ch/ce/lun 取自 sb->topo） */
DWORD super_block_page_pba(uint8_t ch, uint8_t ce, const super_block_t *sb,
                                uint8_t plane, uint8_t page);
```

**具名字段构造 PBA** 用位域视图（`ftl_config.h §三.2`）：`pba_make(ch, ce, lun, block,
plane, page, cw)` 或 `pba_pack((nand_pba_fields_t){…})`；反向读取用 `pba_fields(pba)`。

⚠ v0.28 引入的展开式 `nand_addr_t` 与 `addr_to_cmd` / `addr_from_pba` / `addr_from_paa`
**已于 v0.73 删除**——`nand_addr_t` 整体退出 FTL（只在 `pba_to_paa()` 内部出现）。

### 5.3 内部同步 NAND 操作（内联化）

**v0.28 起，`ftl_internal_read_page / write_page / erase_block / read_page_raw` 四个包装函数已删除**，
各模块内联为 `static` 同步助手（`alloc → addr_to_cmd → submit → sync_wait → release`），
仅保留统一等待入口：

```c
/*
 * 同步等待指定 command_id 完成。
 * v0.75：内部走 ftl_op_submit(cmd, FTL_OP_SYNC_WAIT, &waiter)，再自旋
 * fc_service + ftl_op_drain 直到 waiter.done。**只用于结构性顺序路径**
 * （开机初始化 / 全片格式化 / 出厂坏块扫描 / CLI）；host I/O 与后台 job 一律异步。
 */
ftl_status_e ftl_sync_submit_wait(fc_command_t *cmd, fc_command_status_t *status_out,
                                  uint32_t timeout_us);
```

各模块内联助手一览（⚠ **v0.75 阶段 2~5 起大半已异步化**，下表是当前状态）：

| 模块 | 是否仍有同步 NAND 调用 | 说明 |
|------|----------------------|------|
| `l2p_cache.c` | ❌ **零**（阶段 5） | 加载/写回全走 `ftl_async_*` + `ftl_op_submit(FTL_OP_L2P)`；对外只留 `*_sync` 自旋逃生口 |
| `wear_leveling.c` | ❌ **零**（阶段 4） | 搬移主体并入 `migrate.c` |
| `read_recovery.c` | ❌ **零**（阶段 3/5） | 三级恢复全异步；`rr_select_plane` 的 L2P 查询已三态化 |
| `gc.c` | ⚠ 1 处 | `gc_read_page_with_lba`（`ftl_sync_submit_wait`）——唯一调用者是 `pfr_rebuild_closed_sb_list`（**开机**重建，结构性同步） |
| `journal.c` | ⚠ 4 处 | 全在开机 / 结构性路径：`jrn_page_scan_latest` 的页头读与裸空白页判定、`jrn_resume` 的换块擦、`jrn_replay` 的页读；运行期只有**异步** freeze（`jrn_freeze`），无同步回退 |
| `checkpoint.c` | ⚠ 4 处 | 记录读 ×2 / 写 ×2（**阶段 6** 转 job） |
| `metadata.c` | ⚠ 3 处 | 目录压缩 / 回收 / Anchor 落盘全在异步 job（`metadata_job.c`：`metadata_handle` + `ftl_metadata_callback`）；运行期无同步擦除。剩下 3 处全是结构性的：`metadata_ring_prepare_write` 的换块擦（`bbt_save` / `checkpoint_write_initial` 用）、`metadata_ring_locate` 的扫池读（冷启动定位游标）、`metadata_dir_load` 的 Anchor 读（开机） |
| `bbt.c` | ⚠ 7 处 | 记录读 ×2、出厂标记读、擦除、持久化写（**阶段 7**） |
| `power_failure_recovery.c` | ⚠ 4 处 | 开机 region 预载读、空白页裸读（**结构性同步**，长期保留） |
| `ftl_fc_interface.c` | — | 同步原语与 `ftl_async_*` 异步原语的定义处（机制层） |

⚠ **`ftl_sync_submit_wait` 的目标终态已达成**（v0.75 阶段 8）：只被开机 / 格式化 / 出厂扫描 /
CLI 路径调用，剩余同步站点**全部**落在上表标 ⚠ 的行里，且**全是结构性同步**（在 `main`
进 superloop 前执行，无人 drain CQ）或非派发上下文的回退（`journal.c` ①），**会长期保留**。
`checkpoint.c`（阶段 6）、`bbt.c`（阶段 7）、`metadata.c` 的压缩与 Anchor（阶段 8）
四条异步化路径**都已不再出现**在这一栏里——即"由 job 承载"的那些同步站点已经清零。

### 5.4 约束汇总

| 操作 | CRC | Random | ECC | module | lba |
|------|-----|--------|------|--------|-----|
| 用户数据读/写 | ENABLE | ENABLE | BCH | FTL | lba |
| L2P region 读/写 | **ENABLE** | DISABLE | BCH | METADATA | 不填 |
| Journal 写 | **ENABLE** | DISABLE | BCH | METADATA | 不填 |
| GC 搬移读/写 | ENABLE | ENABLE | BCH | GC | lba |
| BBT 读/写 | **ENABLE** | DISABLE | BCH | BBT | 不填 |
| Checkpoint 读写 | **ENABLE** | DISABLE | BCH | CHECKPOINT | 不填 |
| Anchor (Directory) 读写 | **ENABLE** | DISABLE | BCH | METADATA | 不填 |
| BBT 出厂扫描（`OP_READ_COLUMN` 读列 2048，v63） | DISABLE | DISABLE | BCH | BBT | 不填 |

> ⚠ **v0.62 起元数据页 CRC = ENABLE**（`FTL_PAGE_CFG_META` 由 `FC_CRC_DISABLE` 翻为
> `FC_CRC_ENABLE`）——FTL 记录内不再自带 CRC，元数据完整性由**下层 FC 的 per-CW CRC + ECC**
> 承担（DRAM-less 主流做法，见 `ftl_revision_history.md` v0.62）。上表「ENABLE」即此。

---

## 6. ftl_service() 调度时序

### 6.1 三级模式

```
每轮 superloop 的执行顺序（**两个函数**，序 3/4 拆分）：

  ftl_service_pump()  ── 实例无关，**每轮只调一次**（NAND 引擎与完成表都是全局单份）

    Phase 1 ── fc_service() 最多 NAND_SUBMIT_BUDGET (4) 次
    └── 推进 NAND 命令执行

    Phase 2 ── 处理完成回调
    ├── ftl_op_drain()（统一完成泵）
    └── ftl_op_dispatch() 按 op_tag 查 g_op_callbacks[]
          ├── FTL      → 子操作簿记（数据拷贝到该 slice）→ 组聚合 → pending_remove
          │              → 组可能完成（发一次组通知）
          ├── METADATA → release
          └── GC       → release

  ftl_service(ftl)  ── 对**每个**实例各调一次

    Phase 2.5 ── 补发 host 子操作
    ├── 遍历本实例各未完成组 → ftl_drain_group()
    │     完成先腾出 FC 资源（命令/desc/buffer 池、SQ 空位），再补 host 子操作，
    │     保证 host IO 优先于 Phase 3 后台。子操作三态：
    │       FTL_OK    → 推进 next
    │       FTL_AGAIN → 瞬态背压，本轮到此为止，下轮再补（stalls++ 超预算即判卡死）
    │       FTL_ERROR → 弃未下发者，组报 IO_ERROR
    └── ftl_wbuf_maybe_flush_idle()  ← 页打包写缓冲的确定性空闲 flush（v0.66）
          补发循环跑完后若再无写组会向当前窗口 append，就把缓冲编程掉
          （否则这些 host 写永不完成）。在 bg_suspend 判定**之前**——它是 host IO
          的收尾，不是后台任务。

    Phase 3 ── 三级后台模式（判据 = 未决命令数近似空闲度，非"本轮有无提交 NAND"）
    ⚠ `bg_suspend` 置位（`bbt --erase` 后）⇒ 整体跳过 Phase 3，直接 return。
    ⚠ v0.75 阶段 3/4 起：rr_handle() / mig_handle() **任何档位都要跑**——RR 的 host 读
      在等结果，搬移 job 可能是在 BUSY 档被写路径紧急 GC 起的，不占后台带宽预算。
    ⚠ v0.75 阶段 6 起：checkpoint_handle() 在**三档之外**先跑一次（Phase 3 首）——CP job
      可能在 BUSY 档被紧急水位起，后续阶段要跨档位走完；只在起它的那一档推进会把 job 钉死。
    ├── 三档之前（must-run 正确性关键集，无条件每轮跑）
    │   ├── checkpoint_handle(ADVANCE)       ← 推进在飞 CP（每轮只推进一次）
    │   ├── rr_handle()                      ← 推进在飞 RR
    │   ├── mig_handle()                 ← 推进在飞搬移 + 自持节拍（GC/WL/predict）
    │   ├── metadata_handle()                ← 目录整理
    │   ├── journal_handle(ERASE_ONLY)       ← 新环形块没擦不能写
    │   └── checkpoint_handle(EMERGENCY)     ← Journal ≥95%
    │
    ├── IDLE 模式 (inflight == 0):               全力后台
    │   ├── l2p_handle(cache, dim)            ← 档位 → max_kick（模块内表）
    │   ├── journal_handle(FLUSH)             ← 异步刷页
    │   ├── bbt_handle()                      ← 攒批持久化
    │   └── checkpoint_handle(TIMER)          ← T1/T2/T3 触发评估
    │
    ├── NORMAL 模式 (pending ≤ MAX/2 且无排队组):  适度后台
    │   └── 同 IDLE 的四项（l2p max_kick 降到 4）
    │
    └── BUSY 模式 (pending > MAX/2 或 queued != 0):  最小后台（host 饿着，让路）
        └── l2p_handle(cache, dim)            ← max_kick = 1
```

> `mig_handle` 的自持节拍：GC 10ms / WL 5s / predict 1s（仅 IDLE）；
> `load_level == BUSY` 时不评估新工作，只推进在飞 job。写路径取不到页对时
> `mig_gc_request_urgent()` 只登记，下一轮由 must-run 启动。

> ⚠ **Phase 2.5 的开头是 `pend_confirm_service()`**——推进**停泊**的写确认，
> 必须早于补发循环：停泊意味着该 LBA 的 L2P 还是旧映射，先补发一条读同一 LBA 的子操作
> 就会读到旧物理页。
> ⚠ 每实例的写回在飞上限 `L2P_FLUSH_MAX_INFLIGHT = 4`（一次写回 = 1 命令 + 4 buffer，
> 不设上限会把 FC 池吃光 ⇒ host I/O 拿不到命令、全系统 `FTL_AGAIN`）。

> `inflight = pend_slot_get_count + pend_group_get_count`（⚠ v0.75 阶段 4 起**不再计入
> `ftl_op_ctx_count`**：后台 job 的命令也登记在那里，计进来会让"真空闲"永不成立，
> 后台反被自己的命令饿死；后台活跃度由各自的单飞行门单独管）。
> ⚠ `main.c` 的主循环**遍历全部实例**调 `ftl_service`（序 4：只跑 `[0][0][0]` 会让其余实例的
> journal / checkpoint / bbt / GC 永不推进）；`ftl_service_pump()` 每轮只调一次。

### 6.2 完整实现

```c
/* 实例无关的每轮泵（superloop 每轮一次） */
void ftl_service_pump(void) {
    for (uint8_t i = 0U; i < NAND_SUBMIT_BUDGET; i++) {      /* Phase 1 */
        if (fc_service() != FC_STATUS_OK) break;
    }
    ftl_op_drain();                                /* Phase 2 */
}

/* 单个实例（对每个实例都要调） */
void ftl_service(ftl_ctx_t *ftl) {
    uint16_t inflight, queued;

    if (ftl == NULL) return;

    /* Phase 2.5: 补发本实例各组的待下发子操作 */
    for (uint8_t i = 0U; i < FTL_GROUP_MAX; i++) {
        if (ftl->pending.groups[i].used != 0U) {
            ftl_drain_group(ftl, (uint16_t)i);
        }
    }
    ftl_wbuf_maybe_flush_idle(ftl);   /* 页打包写缓冲的确定性空闲 flush */

    /* Phase 3: 三级后台模式（判据 = 未决命令数近似空闲度） */
    queued   = pend_group_get_count(&ftl->pending);
    inflight = (uint16_t)(pend_slot_get_count(&ftl->pending) + queued + ftl_op_ctx_count());

    if (ftl->bg_suspend != 0U) return;   /* `bbt --erase` 后整体跳过 Phase 3 */

    if (inflight == 0U) {
        ftl_service_idle(ftl);
    } else if ((pend_slot_get_count(&ftl->pending) > (FTL_PENDING_MAX / 2U)) || (queued != 0U)) {
        ftl_service_busy(ftl);
    } else {
        ftl_service_normal(ftl);
    }
}
```

---

## 7. 接口文件清单

```
include/ftl/
├── ftl.h                        # FTL 对外 API + ftl_ctx_t
├── ftl_type.h                   # DWORD / pba_t 基础类型
├── ftl_config.h                 # 几何推导（§一）+ 地址编码（§三：LBA 侧 + PBA 侧）
├── ftl_fc_interface.h           # PBA helper + 同步 NAND 操作
├── bst.h                        # Block State Table（ftl_block_t[4096]，20B/项）
├── l2p_cache.h                  # L2P Cache + CLOCK 替换 + thrashing 检测 + 三态 API（v0.75 阶段 5）
├── allocator.h                  # Allocator + Super Block + OP/Spare 池
├── journal.h                    # Journal + 四层保护
├── gc.h                         # GC 触发条件 + 选 victim（搬移在 migrate.h）
├── migrate.h                    # 搬移 job（mig_handle_t / mig_gc_begin / mig_wl_begin / mig_predict_begin / mig_handle），v0.75 阶段 4
├── metadata.h                   # ★ 固定元数据区布局（单一事实来源）+ FTL_MEDIA_LAYOUT_VERSION + Metadata Directory
├── pending.h                    # Pending Request 表 + 完成组表
├── ftl_op.h                     # ctx 旁表 + 路由 + op tag/回调 + CQ 排空（v0.75）
├── checkpoint.h                 # Checkpoint + Journal 截断协同
├── bbt.h                        # BBT 坏块表（池 + 全片擦除原语）
├── read_recovery.h              # 读恢复（CRC 重读一次）+ ECC 趋势预测 + 恢复 job（rr_stage_e/rr_handle_t）
├── power_failure_recovery.h     # 三类启动恢复 + 介质格式化
└── wear_leveling.h              # 动态 + 静态磨损均衡

src/ftl/
├── makefile.mk                  # 通配符自动发现
├── ftl.c                        # FTL 对外 API + ftl_service 三级模式
├── ftl_fc_interface.c           # PBA helper + 同步/异步 NAND 操作实现
├── l2p_cache.c                  # CLOCK evict + 异步 Region load/flush + thrashing + ftl_l2p_callback
├── allocator.c                  # Free Pool + OP/Spare + 磨损均衡
├── journal.c                    # append/flush_page/replay/truncate + 刷页 job（v0.75 阶段 2）
├── gc.c                         # 选 victim（gc_select_victim）+ gc_read_page_with_lba
├── migrate.c                    # GC/WL/慢死预测共用搬移 job（v0.75 阶段 4）
├── metadata.c                   # Directory init/lookup/update/flush/load + g_ftl_partitions[] 分区表
├── pending.c                    # Pending 表 + 组 add/lookup/remove
├── ftl_op.c                     # ctx 旁表 + 路由 + 提交 + 派发/排空（v0.75）
├── checkpoint.c                 # 全量 Checkpoint + 三级触发
├── bbt.c                        # BBT 加载/扫描/标记/保存 + 全片擦除
├── read_recovery.c              # 单次读 + CRC 重读 + 恢复 job 状态机
├── power_failure_recovery.c     # 三类启动 + 介质版本校验/自动格式化
└── wear_leveling.c              # UBI 差值触发 + 最冷 victim 选择（搬移在 migrate.c）
```

---

## 8. 三层架构与接口关系

```
                 ┌───────────────────────────────────┐
                 │         Host / UART CLI 层          │   ← D10：学习主控 Host 命令来源
                 │   uart_cli_wr/rd/tr/fl/wp/info/    │      （NVMe 已删除，v0.47）
                 │   map/crash/bbt/test()             │
                 │   ———————————————                  │
                 │   lba + n → ssd_request_t           │
                 └──────────────┬────────────────────┘
                                │ ssd_controller_submit()
                    ┌───────────▼──────────────────────────┐
                    │       Controller 层      src/controller/ │
                    │                                        │
                    │  ┌────────────────────────────────┐   │
                    │  │ request.c: ssd_request_split()  │   │
                    │  │  仅按**实例边界**切连续 run     │   │
                    │  │  每 run 一次 ftl_submit_*       │   │
                    │  │  on_sub_complete → sub_remaining│   │
                    │  │  → 0 → complete_flag = 1        │   │
                    │  │  ★ plane 配对已下沉 FTL         │   │
                    │  └────────────────────────────────┘   │
                    │           │ ftl_submit_read/write      │
                    │           │ ({lba, count, buffer}, …)  │
                    └───────────┼──────────────────────────┘
                 ┌──────────────▼────────────────────────────┐
                 │          FTL 层         src/ftl/           │
                 │                                            │
                 │  ftl_submit_read(ftl, &req)                │ ← 长度任意
                 │  ftl_submit_write(ftl, &req)               │   自拆单/双 plane
                 │  ftl_flush_begin/step / ftl_trim_begin/step           │
                 │    req = ftl_io_req_t{lba,count,buffer}│
                 │                                            │
                 │  ┌────────────────────────────────────┐   │
                 │  │ 组：一次 submit = 一个 ftl_io_group_t│   │
                 │  │  ftl_issue_subop × N（三态）        │   │
                 │  │  ftl_drain_group（快路径 + 2.5 补发）│   │
                 │  └────────────────────────────────────┘   │
                 │                                            │
                 │  ┌────────────────────────────────────┐   │
                 │  │ ftl_host_io_callback()（逐子操作）        │   │
                 │  │   ① 数据拷贝 → 该 slice buffer     │   │
                 │  │   ② 组聚合 agg_status（粘性）      │   │
                 │  │   ③ pending_remove                 │   │
                 │  │   ④ 组全部完成 → 发一次组通知：     │   │
                 │  │      status_out / complete_flag /  │   │
                 │  │      on_complete(ctx)              │   │
                 │  └────────────────────────────────────┘   │
                 │                                            │
                 │  ┌────────────────────────────────────┐   │
                 │  │ Phase 3 后台 IDLE/NORMAL/BUSY       │   │
                 │  │ GC / WL / Checkpoint                │   │
                 │  └────────────────────────────────────┘   │
                 └──────────────┬──────────────────────────┘
                                │ pba_to_cmd / fc_submit
                 ┌──────────────▼────────────────────────────┐
                 │     FC 层          src/fc/                  │
                 │  fc_submit / fc_service / fc_poll_completion│
                 └─────────────────────────────────────────────┘
```
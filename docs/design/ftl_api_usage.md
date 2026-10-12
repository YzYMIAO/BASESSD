# FTL 模块接口说明文档

> 本文档是 `ftl_design.md` 的配套文档，专注于 FTL 模块的对外接口定义、使用方法和内部模块间的接口约定。
> 实现时以此为参考，实际接口以头文件为准。

---

## 1. FTL 对外 API

FTL 模块通过 `include/ftl/ftl.h` 对外暴露以下接口。
FTL 对外只提供**非阻塞单页接口**，不提供同步包装。多页请求由 Controller 层拆分后逐个提交 FTL。
Controller 层通过 `complete_flag` 轮询或 `on_complete` 回调跟踪异步完成状态。

### 1.1 核心数据结构

```c
/* FTL 主上下文 */
struct ftl_context {
    ftl_topology_t      topo;               // 拓扑信息（ch/ce/lun）
    l2p_cache_t         cache;              // SRAM L2P cache (~132KB)
    ftl_allocator_t     alloc;              // 空闲 block / Super Block 写指针
    l2p_journal_t       journal;            // log-structured journal
    metadata_dir_cache_t metadata_dir;      // Metadata Directory
    uint32_t            cp_timer_start;     // Checkpoint 计时
    uint32_t            cp_sequence;        // 上次 Checkpoint sequence
    uint32_t            gc_last_run_writes; // GC 计时
    uint32_t            gc_timer_start;
    uint32_t            total_writes;       // 写入总次数
};
```

### 1.2 接口列表

| 接口 | 类型 | 说明 | 备注 |
|------|------|------|------|
| `ftl_init()` | 初始化 | 初始化 L2P cache、allocator、journal、Metadata Directory、BBT | 在 `fc_init()` 之后调用 |
| `ftl_submit_read()` | 异步读 | 非阻塞提交单页读请求（pending 表 + complete_flag + on_complete 回调） | L2P→PBA→FC submit→pending |
| `ftl_submit_write()` | 异步写 | 非阻塞提交单页写请求 + Journal + watermark + complete_flag + on_complete 回调 | alloc→FC submit→journal→L2P→pending |
| `ftl_trim()` | 异步取消映射 | 写 FTL_PBA_INVALID 到 L2P Cache，标记 DIRTY | 非阻塞 |
| `ftl_service()` | 主循环 | 推进 FC 调度 + 统一完成泵 + 后台任务（flush/GC/WL/Checkpoint） | 在 main loop 中频繁调用 |

**注**：
- FTL 提供**单页和多 Plane 接口**（pages=1=单 Plane 2048 字节，pages=2=双 Plane Multi-Plane 4096 字节）。多 LBA 请求由 Controller 层拆分配对后提交。
- FTL 不提供同步包装（删除 `ftl_read`/`ftl_write`）。
- Controller 层通过 `ssd_controller_submit()` 异步提交请求，通过 `ssd_controller_is_complete()` 或 `on_complete` 回调跟踪完成。

### 1.3 函数签名

```c
/* ftl.h — 当前签名（v0.29：ftl_io_request_t 结构体传参，8 形参 → 2） */

#include "common/ftl_types.h"  /* ftl_io_request_t, pending_cb_t */

/*
 * ftl_submit_read - 非阻塞提交读请求（单页或多 Plane）
 *
 * @param ftl  FTL 上下文
 * @param req  IO 请求描述（见 ftl_io_request_t 定义，§1.1）
 *             req->pages  = 1 → 单 Plane 读（2048B）
 *             req->pages  = 2 → 双 Plane Multi-Plane 读（4096B，req->lba 须为偶数）
 * @return FTL_OK 成功提交，FTL_ERROR 失败
 */
ftl_status_t ftl_submit_read(ftl_context_t *ftl, const ftl_io_request_t *req);

/*
 * ftl_submit_write - 非阻塞提交写请求（单页或双 Plane Multi-Plane）
 *
 * @param ftl  FTL 上下文
 * @param req  IO 请求描述
 *             req->pages  = 1 → 单 Plane PROGRAM（2048B）
 *             req->pages  = 2 → 双 Plane Multi-Plane PROGRAM（4096B，req->lba 须为偶数）
 * @return FTL_OK 成功提交，FTL_ERROR 失败
 */
ftl_status_t ftl_submit_write(ftl_context_t *ftl, const ftl_io_request_t *req);
```

**`ftl_io_request_t` 完整定义**（`include/common/ftl_types.h`，v0.29 起 pending 表按值嵌入）：

```c
typedef struct
{
    uint64_t               lba;             /* Host 逻辑地址（NVMe 协议宽度） */
    void                  *buffer;          /* 读：caller 数据 buffer；写：数据源 */
    uint32_t               pages;           /* 页数：1=单 Plane，2=双 Plane Multi-Plane */
    volatile uint8_t      *complete_flag;   /* 非 NULL 时 NAND 完成后置 1（异步跟踪） */
    volatile ssd_status_t *status_out;      /* 非 NULL 时完成 handler 写入 OK/IO_ERROR */
    pending_cb_t           on_complete;     /* 完成后回调（可选，Controller 子请求聚合） */
    void                  *on_complete_ctx; /* 回调上下文（ssd_request_t*） */
} ftl_io_request_t;
```

> **字段语义（消费级 DRAM-less）**：
> - `buffer`：读 = 数据写回处（completion handler `ftl_cmd_read_data` → `pending->io.buffer`）；
>   写 = 数据源（`ftl_cmd_write_data(cmd, req->buffer, data_size)`）
> - `pages`：决定 OP 码（单 Plane `OP_NORMAL_*` / 双 Plane `OP_*_MULTI_PLANE`）与 `data_size = FTL_READ_USER_SIZE × pages`
> - `complete_flag` + `status_out`：controller 异步跟踪双通道（完成信号 + 成功/失败）
> - `on_complete` + `on_complete_ctx`：Controller 子请求聚合（`on_sub_complete` 递减 `sub_remaining`）

> **v0.29**：`ftl_submit_read/write` 形参由 8 个散落参数收敛为 `(ftl, req)` 结构体传参；
> `ftl_pending_req_t` 按值嵌入 `ftl_io_request_t io`（`pending_add` 形参 12→6），
> `pending_cb_t` 从 `pending.h` 迁至 `ftl_types.h` 打破循环 include；
> `ssd_request_t` 增加 `ftl_io_request_t io` 复用模板（单块直接填，拆分循环内只改 lba/buffer/pages）。

**`ftl_init_params_t` 完整定义**（`include/ftl/allocator.h`，v0.30 起 ftl_init / allocator_init 共用）：

```c
typedef struct
{
    ftl_topology_t  topo;           /* 本 FTL 绑定的 NAND 芯片路由（ch/ce/lun） */
    uint16_t       *free_blocks;    /* free block 数组（全局块号，由调用者提供存储） */
    uint16_t        free_count;     /* free block 数 */
    ftl_block_t    *blocks;         /* Block State Table（v0.32，容量 FTL_BLOCK_TOTAL，必填） */
    super_block_t  *closed_sb_list; /* closed Super Block 数组（由调用者提供存储） */
} ftl_init_params_t;
```

> **v0.30**：`ftl_init` / `allocator_init` 形参 7→2 / 8→2，统一收敛为
> `(ftl_context_t *ftl, const ftl_init_params_t *params)`。
> **v0.32**：`bad_blocks` / `erase_counts` 两指针**删除**——per-block 状态统一进
> `ftl_block_t *blocks`（BST 单数据源），坏块位 + 擦除计数 + VPC + 位图 + ECC 全在表内。
> **定义位置**：`ftl_init_params_t` 依赖 `super_block_t`（allocator.h 定义），放
> `common/ftl_types.h` 会与 allocator.h 形成循环 include → 定位于 `allocator.h`（super_block_t 之后）。
> **v0.37 补充**：`free_count` 传 0 即可（allocator_init 内部硬清零），free pool 由调用者在
> `ftl_init` 后遍历 BST 非坏块 `allocator_free_block` 回填（allocator_init 不填、bbt 只标坏块）。
> 启动示例见 §1.7（v0.37 多维生产版）。

### 1.4 异步完成机制

所有异步请求通过三种机制通知完成：

1. **`complete_flag` 轮询**：completion handler 置位 `volatile uint8_t` 标志，caller 在主循环中轮询
2. **`status_out` 状态码**：completion handler 写入 `SSD_STATUS_OK` 或 `SSD_STATUS_IO_ERROR`，与 `complete_flag` 配合使用
3. **`on_complete` 回调**：completion handler 在 `complete_flag` 置位后调用回调（用于 Controller 子请求计数器聚合）

完成时序：
- **读请求**：NAND READ 完成 + 数据拷贝到 caller buffer → `status_out=OK` → `complete_flag=1` → `on_complete(ctx)`
- **写请求**：NAND PROGRAM 完成后 → `status_out=OK` → `complete_flag=1` → `on_complete(ctx)`
- **PBA 无效（读）**：直接 memset 零 → `status_out=OK` → `complete_flag=1` → `on_complete(ctx)` (无需 NAND 操作)
- **NAND 失败**：读返回全零 / 写标记坏块 → `status_out=IO_ERROR` → `complete_flag=1` → `on_complete(ctx)`
- **TRIM**：立即完成（只改 L2P Cache，不写 NAND）

### 1.5 Completion Table（统一完成路由）

`completion_table.h/c` 提供统一完成事件分发，消除 `ftl_service` 和 `ftl_internal_sync_wait` 各自 `fc_poll_completion` 造成的 completion 丢失问题（`ftl_internal_sync_wait` 仍保留，v0.28 后为唯一同步等待入口）。

```c
// completion_table.h — 完整接口

#define COMPLETION_TABLE_SIZE (16U)

/* 注册一个 completion handler */
int completion_table_add(uint16_t cmd_id,
                         completion_handler_fn_t handler,
                         void *ctx);

/* 消费所有 CQ entry 并路由到各 handler */
void completion_table_drain(void);

/* 设置默认 handler（未注册精确路由的 completion 走此路径） */
void completion_table_set_default(completion_handler_fn_t handler, void *ctx);

/* 同步等待器 */
typedef struct {
    uint16_t             cmd_id;
    uint8_t              done;               /* handler 置 1 */
    fc_command_status_t  completion;          /* 保存的完成状态 */
} ftl_sync_waiter_t;

void ftl_sync_waiter_handler(const fc_command_status_t *completion, void *ctx);
void ftl_sync_waiter_handler_with_release(const fc_command_status_t *completion,
                                          void *ctx);
```

**路由规则**：
1. `completion_table` 中有注册 handler → 精确路由（用于同步等待、专用回调）
2. 无注册但设置了 default → 走 `ftl_handle_completion`（FTL pending 表、complete_flag、命令释放等）
3. 都没有 → 静默丢弃（不应发生）

### 1.6 写完成确认（modify-after-confirm）

写请求（`ftl_submit_write`）的 Journal + L2P 更新不在提交路径中执行，而是后移到 completion handler 中、确认 NAND PROGRAM 成功后执行：

```
ftl_submit_write(ftl, &req)
  ├─ allocator_alloc() → new_pba
  ├─ l2p_cache_read() → old_pba
  ├─ fc_submit(NAND PROGRAM)            ← 异步，立即返回
  └─ return FTL_OK                      ← Journal/L2P 未更新

       ↓ 几十~几百 μs 后

  ftl_handle_completion()
    ├─ NAND_SUCCESS →
    │   journal_append(lba, old, new)     ← 确认后记录
    │   l2p_cache_write(lba, new_pba)
    │   complete_flag = 1
    │
    └─ NAND_FAIL →
        bbt_mark_bad(new_pba.block)       ← RAM only（后台 BBT flush）
        L2P 不动 → 自动回滚到 old_pba     ← 旧数据完好
        complete_flag = 1
```

**Pending 表新增字段**（v0.29 起 `ftl_io_request_t io` 按值嵌入，写专用字段保留在此层）：

```c
typedef struct {
    uint16_t           used;              /* 1: 槽位占用 */
    uint16_t           command_id;        /* FC command_id */
    ftl_io_request_t   io;                /* ★ 嵌入请求描述（lba/buffer/pages/通知/回调） */
    /* ↓↓↓ 写完成确认/回滚（写专用，读请求时无效） ↓↓↓ */
    nand_pba_t         old_pba;           /* 写前旧 PBA（journal/回滚用） */
    nand_pba_t         new_pba;           /* 新分配的 PBA（坏块标记用） */
    ftl_context_t     *ftl;               /* completion handler 访问 ftl */
    uint8_t            cache_intermediate;/* 1: Cache Program 中间页（15h），跳过 L2P/Journal */
} ftl_pending_req_t;
```

> **v0.29**：`io` 按值嵌入（`pending_add` 内 `g_pending[i].io = *req` 一次性拷贝 7 字段），
> `pending_add` 形参 12→6（`(cmd_id, io, old_pba, new_pba, ftl, cache_intermediate)`）；
> 位域全部展开为普通字段（`used:1/command_id:15/pages:2/cache_intermediate:1`）。

**坏块标记两阶段化**：

| 阶段 | 位置 | 操作 | NAND 访问 |
|------|------|------|-----------|
| 快速路径 | `ftl_handle_completion` | BBT bitmap 置位 + allocator 标记 | ❌ 无 |
| 后台持久化 | `ftl_service_idle/normal` → `bbt_flush()` | 攒批写 NAND BBT（Block 36+39） | ✅ 一次 6 页 |

**回滚**：写失败后 L2P 不动（自动指向 `old_pba`），Super Block 空洞由 GC 扫描时读失败自然跳过。无需额外回滚逻辑。

### 1.7 Pending 表完整数据结构

多 LBA 请求（`block_count > 1`）由 Controller 层 **`request.c`** 拆分为 N 个单页子请求，按序提交 FTL。

**数据流**：

```
ssd_controller_submit(req{block_count=N})
  ├── block_count == 1 → 直通 ftl_submit_read/write(&req->io)（单页）
  └── block_count > 1  → ssd_request_split()
        ├─ request->io.lba    = start_lba + i;      // 循环内复用 io 模板
        ├─ request->io.buffer = buf + i*2048;
        ├─ request->io.pages  = (成对) 2 / (奇数尾) 1;
        ├─ ftl_submit_read(ssd_controller_route(ctrl, io.lba), &request->io)  // v0.36 按 LBA 路由
        ├─ ...
        └─ 每个完成 → ftl_handle_completion
              → io.on_complete() → sub_remaining--
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
    /* v0.36：子请求数预统计（ssd_split_count），替代硬公式 (block_count+1)/2：
     *   - 连续 LBA 同实例才配对 pages=2（ssd_split_use_multi_plane），
     *     跨 region/实例边界切实例时降级单 Plane → 实际子请求数 > 公式值
     *   - 未映射读会同步完成（ftl_submit_read PBA INVALID 路径），必须先计数后提交
     *   计数不足 → sub_remaining 永不归零 → 父请求 complete_flag 永不置位（host 悬挂） */
    request->sub_remaining = ssd_split_count(controller, request);
    ...
}
```

写请求类似，但传 `complete_flag=NULL`（子请求用回调聚合，不用轮询）。

**结构体变化** (`ssd_request_t`)：

```c
typedef struct {
    ssd_request_opcode_t opcode;
    uint64_t             start_lba;
    uint32_t             block_count;
    void                *buffer;
    size_t               buffer_size;
    ssd_status_t         completion_status;
    volatile uint8_t     complete_flag;
    volatile uint32_t    sub_remaining;      /* 未完成的子请求数 */
    ftl_io_request_t     io;                 /* v0.29: 子请求复用模板 */
} ssd_request_t;
```

```c
/* 系统启动（v0.37 生产路径：见 src/main.c ssd_ftl_boot / ssd_ftl_boot_all） */
fc_init();                              // ① FC 初始化
nand_init();                            // ② NAND 初始化
nand_dispatch_set_armed(1U);            // ③ 放行 NAND 异步提交（submit / submit_chain）

/* 每 (ch,ce,lun) 一个 FTL 实例（多维声明，宏改动即扩展；v0.35 多维化） */
static ftl_context_t     g_ftl[CH_NUM][CE_NUM][LUN_NUM];
static uint16_t          g_free_blocks[CH_NUM][CE_NUM][LUN_NUM][FTL_BLOCK_TOTAL]; /* → SRAM2 */
static super_block_t     g_closed_sb_list[CH_NUM][CE_NUM][LUN_NUM][SUPER_BLOCK_LIST_MAX]; /* → SRAM2 */
static ftl_block_t       g_blocks[CH_NUM][CE_NUM][LUN_NUM][FTL_BLOCK_TOTAL];   /* BST 224KB → AXI_RAM */
static ssd_controller_t  g_ctrl;

ftl_init_params_t params = {
    .topo           = { .ch = ch, .ce = ce, .lun = lun },  // 本实例拓扑
    .free_blocks    = g_free_blocks[ch][ce][lun],
    .free_count     = 0U,                                  // 回填前为 0（allocator_init 硬清零）
    .blocks         = g_blocks[ch][ce][lun],               // BST（必填，未提供则 ftl_init 失败）
    .closed_sb_list = g_closed_sb_list[ch][ce][lun],
};

ftl_init(&g_ftl[ch][ce][lun], &params);   // ④ FTL 初始化（v0.30：结构体指针传参）
                                          //    内部：completion_table_init → pending_init
                                          //         → BST memset（v0.32）→ l2p_cache_init
                                          //         → allocator_init（free pool 空）
                                          //         → metadata_dir_load → bbt_init（填充 BST bad 位）
                                          //    BST 由调用者提供存储，未提供（NULL）→ FTL_ERROR

/* ⑤ free pool 回填（v0.37 最大缺口）：
 *    allocator_init 不填 free pool（free_count 硬清零）、bbt 只标坏块不产 free_blocks。
 *    必须遍历 BST 非坏块逐个 allocator_free_block，否则首次写 allocator_alloc 失败。 */
for (uint16_t g = 0U; g < FTL_BLOCK_TOTAL; g++) {
    if (!ftl_block_is_bad(&g_blocks[ch][ce][lun][g])) {
        allocator_free_block(&g_ftl[ch][ce][lun].alloc, g);
    }
}

ftl_boot_recovery(&g_ftl[ch][ce][lun], &result);  // ⑥ 启动恢复（FRESH/CLEAN/POWER_FAILURE）

/* ⑦ controller 绑定：g_ftl_ptrs 三维指针表（类型匹配 ssd_controller_init 形参） */
ssd_controller_init(&g_ctrl, g_ftl_ptrs);

/* 主循环（企业级异步模式；v0.37 g_ftl_ready 守卫） */
while (1) {
    uart_service_poll();                // 收 Host 命令 → ssd_controller_submit()
    if (g_ftl_ready) {
        ftl_service(&g_ftl[0][0][0]);   // ⑧ 推进 FTL（FC + 完成路由 + 后台任务）
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
#define L2P_ENTRIES_PER_REGION   512     // region 内 PBA 数
#define L2P_CACHE_SIZE           64      // cache 行数（32/64/128）
#define L2P_REGIONS_TOTAL        2048    // LBA 空间总 region 数（全设备）
#define L2P_REGIONS_PER_LUN      (L2P_REGIONS_TOTAL / LUN_NUM)  // 每 (ch,ce,lun) 表容量（v0.35 分片）
#define L2P_BLOCKS_PER_LUN       (L2P_REGIONS_PER_LUN / 64U)    // 每 LUN 表块数（1: 32, 2: 16）
#define L2P_BASE_BLOCK           (0U)    // L2P 表起始 Block

// Cache line 状态
typedef enum {
    L2P_CACHE_INVALID  = 0,    // cache 行无效
    L2P_CACHE_CLEAN    = 1,    // 有效，未修改
    L2P_CACHE_DIRTY    = 2,    // 有效，已修改（需写回）
    L2P_CACHE_FLUSHING = 3,    // 正在写回 NAND
} l2p_cache_state_t;

/* 单条 cache line = 2KB + 元数据 */
typedef struct {
    nand_pba_t  entries[L2P_ENTRIES_PER_REGION];   // 512 × 4B = 2048B
    uint16_t    region_id;                           // 0..L2P_REGIONS_PER_LUN-1（本表内编号，v0.35）
    uint8_t     state;                               // l2p_cache_state_t
    uint16_t    access_count;                        // CLOCK 访问计数
} l2p_cache_line_t;

/* 整体 cache（v0.35 起绑定本 (ch,ce,lun) 拓扑） */
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
    uint8_t           ch, ce, lun;       // 本 cache 绑定拓扑（固定位置寻址 + 路由）
    metadata_dir_cache_t *md_dir;        // Metadata Directory 指针
    ftl_allocator_t      *alloc;         // Allocator 指针（Region 动态分配；v0.19 加）
} l2p_cache_t;

/* 设置 Metadata Directory + Allocator 指针（v0.19 加 alloc 参数） */
static inline void l2p_cache_set_md_dir(l2p_cache_t *cache,
                                        metadata_dir_cache_t *md_dir,
                                        ftl_allocator_t *alloc);

/* 全局 LBA → (ch,ce,lun) 路由（行业主控哈希；LUN_NUM=1 时恒 0） */
static inline uint32_t lba_route_ch(uint64_t lba);
static inline uint32_t lba_route_ce(uint64_t lba);
static inline uint32_t lba_route_lun(uint64_t lba);

/* 全局 LBA → 片内 local LBA（交错：lun 取低位 → 顺序写摊多 die） */
static inline lba_t lba_to_local(uint64_t lba);

/* L2P 哨兵层位置：Region → PBA（L2P 基础层块 0-15，**永不写入**，读回全 0xFF = 未映射）。
 * ⚠ v0.50 起这不是"region 的固定存储位置"——region 实际写在**日志块**里（从 Free Pool
 * 动态取整块，位置由 Metadata Directory 携带）。本函数只在两处用：
 * ① Directory 尚未登记的 region 读回"未映射"；② 开机回退基础层。 */
static inline nand_pba_t l2p_region_pba(uint8_t ch, uint8_t ce, uint8_t lun, uint16_t region_id);
```

### 2.2 接口

```c
/* 初始化 cache（所有 line 置 INVALID；v0.35 起绑定 ch/ce/lun） */
void l2p_cache_init(l2p_cache_t *cache, uint8_t ch, uint8_t ce, uint8_t lun);

/*
 * 读取 LBA 的 PBA 映射。
 * 可能触发 cache miss → CLOCK evict → 从 NAND 加载 region。
 * 加载时机：miss 时同步从 NAND 读（各模块内联 sync_read_page，见 §5.3）。
 * 返回 FTL_PBA_INVALID 表示该 LBA 未映射。
 */
nand_pba_t l2p_cache_read(l2p_cache_t *cache, lba_t lba);

/*
 * 写入 LBA 的 PBA 映射。
 * 只修改 cache，标记 DIRTY。写回 NAND 由 flush 决定。
 */
void l2p_cache_write(l2p_cache_t *cache, lba_t lba, nand_pba_t pba);

/* 写回单条 DIRTY cache line → NAND（同步等待完成） */
int l2p_cache_flush_one(l2p_cache_t *cache, int line_idx);

/* 写回所有 DIRTY cache lines */
int l2p_cache_flush_all(l2p_cache_t *cache);
```

### 2.3 内部挂载点

```
l2p_cache_read()    → ftl_submit_read / ftl_submit_write 查旧 PBA
l2p_cache_write()   → ftl_submit_write 更新 L2P / journal_replay（F10：回放写 cache）
l2p_cache_flush_one → l2p_cache_evict_one（CLOCK 驱逐时）、ftl_service 后台 flush
l2p_cache_flush_all → checkpoint_create / ftl_service_idle
```

### 2.4 Region 加载流程

```
l2p_cache_find_or_load(region_id):
  l2p_cache_find(cache, region_id)
    → hit: access_count++, return line

  → miss: l2p_cache_evict_one(cache)  → CLOCK 扫
    → FLUSHING: 跳过 | access_count>0: --, 下一个 | INVALID: 直接占
    → DIRTY: flush_one 后占位 | CLEAN(ac==0): 直接占

  占位后:
    metadata_dir_lookup(region_id) → 查 Directory
    ＝ INVALID → l2p_region_pba() 哨兵层（块 0-15，读回全 0xFF = 未映射）
    sync_read_page(addr) → 从 NAND 加载 2KB region 数据（内联，见 §5.3）
    state = CLEAN
```

---

## 3. Journal 内部接口

### 3.1 数据结构

```c
#define JOURNAL_BLOCK_COUNT       (CW_NUM)  // 环形 4 blocks（= CW_NUM）
#define JOURNAL_BLOCK0            (58)      // J0 起始 Block（块 58-61，见 ftl_config.h）
#define JOURNAL_PAGE_SIZE         (PAGE_MAIN_SIZE)  // = 2048
#define JOURNAL_ENTRIES_PER_PAGE  ((JOURNAL_PAGE_SIZE - JOURNAL_HEADER_SIZE) / 16)  // 推导 = 127
#define JOURNAL_HEADER_SIZE       (12)      // Header 大小
#define JOURNAL_MAGIC             (0x524F554AUL)  // "JOUR"
#define JOURNAL_BLOCK_NONE        (0xFFU)   // 「无预擦除块」哨兵（v0.54）

/* Journal entry — 16B */
typedef struct {
    uint32_t    lba;            // LBA
    nand_pba_t  old_pba;        // 修改前的 PBA
    nand_pba_t  new_pba;        // 修改后的 PBA
    uint32_t    sequence;       // 单调递增序号
} __attribute__((packed)) l2p_journal_entry_t;

/* Journal 上下文 */
typedef struct {
    ftl_topology_t topo;                       // 目标 NAND 芯片路由
    nand_pba_t     block_pba[JOURNAL_BLOCK_COUNT];  // 各 Journal Block PBA 基址
    uint8_t        current_block;              // 当前写入 block (0..3)
    uint8_t        current_page;               // 当前写入 page (0..63)
    uint8_t        current_entry;              // 当前页内 entry 偏移 (0..126)
    uint32_t       sequence_counter;           // 全局序号
    uint8_t        pre_erased_block;           // v0.54：被 truncate 预擦除的环形块号
    uint8_t        page_buf[JOURNAL_PAGE_SIZE];// SRAM buffer
    uint8_t        checkpoint_needed;          // 水位标志
    uint8_t        emergency;                  // 紧急标志（≥95%）
} l2p_journal_t;
```

### 3.2 接口

```c
/* 初始化 journal */
void journal_init(l2p_journal_t *journal, ftl_topology_t topo);

/*
 * 追加一条 journal entry。
 * 内部：写入 page_buf → page_buf 满时 flush 到 NAND。
 * v0.54：页缓冲里已有同一 LBA 时就地更新 new_pba（不新增条目、不消耗 sequence）。
 */
void journal_append(l2p_journal_t *journal, lba_t lba,
                    nand_pba_t old_pba, nand_pba_t new_pba);

/*
 * 将当前 page_buf 写入 NAND（即使未满也可强制写）。
 * 写满块时回切到下一 Block（环形共 JOURNAL_BLOCK_COUNT 块）。
 * v0.54：本函数是环形块擦除的**唯一**一点；若该块已被 journal_truncate 预擦除
 * （pre_erased_block 记账）则跳过擦除——旧实现在 truncate 与本函数各擦一次 = 双倍磨损。
 */
void journal_flush_page(l2p_journal_t *journal);

/*
 * 启动时回放 journal entries → 重建 cache。
 * F10：只回放 page_seq > cp_sequence 的页（≤ cp_sequence 的条目已在
 * checkpoint 时 flush 进 NAND L2P 表），按 (page_seq, entry_index) 递增序
 * 应用（per-LBA last-write-wins）。旧实现只回放最大一页 → 掉电跨 ≥2 页丢写。
 */
int journal_replay(l2p_journal_t *journal, l2p_cache_t *cache,
                   uint32_t cp_sequence);

/*
 * 截断 sequence ≤ up_to_sequence 的条目（Checkpoint 已把它们写进 NAND L2P 表）。
 * v0.54：预擦除环形最旧块并记账（pre_erased_block），由 journal_flush_page 回切时消费；
 *        水位不足以覆盖全部已写条目（(up_to_sequence + 1) < sequence_counter）时**不截断**。
 */
void journal_truncate(l2p_journal_t *journal, uint32_t up_to_sequence);
```

### 3.3 Page 格式

```
Header (12B):
  magic       (4B) = 0x524F554A ("JOUR")
  page_seq    (4B) = 本页第一条 entry 的全局序号
  entry_count (2B) = 本页有效 entry 数 (0~JOURNAL_ENTRIES_PER_PAGE=127)
  block_seq   (2B) = 本 Journal Block 写入轮次

Entries (N × 16B, N ≤ 127):
  lba(4B) | old_pba(4B) | new_pba(4B) | sequence(4B)

Padding: (127 - N) × 16B = 0xFF
```

---

## 4. Allocator 内部接口

### 4.1 数据结构

```c
/* 温度等级 */
#define FTL_TEMP_HOT    0U      /* 用户 IO 数据 */
#define FTL_TEMP_COLD   1U      /* GC 搬移 / 元数据 */

/* Free Pool 三级水位（一个已擦除池，见 ftl_design §3.7） */
#define FREE_POOL_TARGET        (32U)   /* 水位①/②分界 */
#define FREE_POOL_EMERGENCY     (8U)    /* 水位②/③分界 */
#define SUPER_BLOCK_LIST_MAX    (1024U) /* Closed Super Block 列表容量 */

/* Super Block */
typedef struct {
    uint16_t plane_blocks[PLANE_NUM];   /* Plane 0 和 Plane 1 的 Block 号 */
    uint8_t  next_page;                  /* 当前写入页（0~63） */
    uint8_t  temperature;               /* FTL_TEMP_HOT / FTL_TEMP_COLD */
    uint8_t  valid;                     /* 1=Open, 0=Closed */
} super_block_t;

/* Allocator 上下文 */
typedef struct {
    uint8_t     ch, ce, lun;            // 拓扑参数
    uint16_t    total_blocks;           // 总好 Block 数
    uint16_t   *free_blocks;            // free block 数组
    uint16_t    free_count;             // 空闲 block 数
    super_block_t hot_sb;               // Hot Open Super Block
    super_block_t cold_sb;              // Cold Open Super Block
    super_block_t *closed_sb_list;      // Closed SB 数组
    uint16_t    closed_sb_count;
    ftl_block_t *blocks;               // Block State Table（v0.32；v0.55 位图独立、v0.56 删死字段 ⇒ 20B：erase_count/last_pe_ts/read_count/valid_count/state/sb_id/rr）
    uint16_t    bad_count;             // 坏块数
    uint16_t    spare_pool_count;
} ftl_allocator_t;
```

### 4.2 接口

```c
/* 初始化 allocator（v0.30：ftl_init_params_t 结构体指针传参，8 形参 → 2） */
int allocator_init(ftl_allocator_t *alloc, const ftl_init_params_t *params);

/*
 * 分配一个 PBA（一页，4 CW）。
 * 从当前温度的 Open Super Block 顺序分配。
 * Super Block 写满 → 关闭（→Closed），从 Free Pool 选新 Block（最小擦除次数）。
 * Free Pool 耗尽 → 返回 NAND_PBA_INVALID。
 */
nand_pba_t allocator_alloc(ftl_allocator_t *alloc, uint8_t temperature);

/* GC 擦除后归还 block 到 Free Pool */
void allocator_free_block(ftl_allocator_t *alloc, uint16_t block);

/* 运行时标记坏块（已删除：统一走 bbt_mark_bad，BBT 单数据源）*/
/* 检查是否需要 GC（已删除：GC 触发单一数据源 = ftl/gc.h 的 gc_check_triggers）*/
```

---

## 5. FTL → FC 接口映射

### 5.1 PBA 操作 Helper

见 `include/ftl/ftl_fc_interface.h`：

```c
/* PBA→CH/CE/PAA + descriptor 链配置 */
int ftl_cmd_setup_ppa(fc_command_t *cmd, nand_pba_t pba);

/* 用户数据写入 desc buffer */
int ftl_cmd_write_data(fc_command_t *cmd, const void *data, uint32_t length);

/* 从 desc buffer 读取用户数据 */
int ftl_cmd_read_data(fc_command_t *cmd, void *buffer, uint32_t length);
```

### 5.2 地址直填 Helper（Step 1/2 新增）

见 `include/ftl/ftl_fc_interface.h`。遍历路径不再打包 → `ftl_cmd_setup_ppa` 拆包，
直接用展开地址（`nand_addr_t`，定义于 `include/nand/nand_config.h`）填入 FC 命令：

```c
/* nand_addr_t → FC 命令（ch/ce/lun → config 位域，block/plane/page/cw → paa） */
ftl_status_t addr_to_cmd(fc_command_t *cmd, const nand_addr_t *addr);

/* nand_pba_t → nand_addr_t（allocator/L2P 返回的紧凑 PBA 展开） */
nand_addr_t addr_from_pba(nand_pba_t pba);

/* PAA + CH/CE → nand_addr_t（元数据固定位置 / Directory 查找结果） */
nand_addr_t addr_from_paa(uint8_t ch, uint8_t ce, nand_paa_t paa);

/* Super Block 中指定 Plane/Page 的展开地址（ch/ce/lun 取自 sb->topo） */
nand_addr_t super_block_page_addr(const super_block_t *sb, uint8_t plane, uint8_t page);
```

语义上 `addr_to_cmd(cmd, &addr)` 等价于 `ftl_cmd_setup_ppa(cmd, pba_from_addr(&addr))`，
但省去打包→拆包往返。**随机 IO 路径（从 L2P 拿到 PBA）仍走 `ftl_cmd_setup_ppa`（保留）。**

### 5.3 内部同步 NAND 操作（内联化）

**v0.28 起，`ftl_internal_read_page / write_page / erase_block / read_page_raw` 四个包装函数已删除**，
各模块内联为 `static` 同步助手（`alloc → addr_to_cmd → submit → sync_wait → release`），
仅保留统一等待入口：

```c
/*
 * 同步等待指定 command_id 完成。
 * 使用 completion_table 统一路由：注册 sync_waiter → fc_service + completion_table_drain。
 */
ftl_status_t ftl_internal_sync_wait(uint16_t cmd_id, fc_command_status_t *status_out,
                                    uint32_t timeout_us);
```

各模块内联助手一览：

| 模块 | 助手（static） | 地址来源 | 特征 |
|------|---------------|---------|------|
| `l2p_cache.c` | `sync_read_page` / `sync_write_page` | `addr_from_paa` / `addr_from_pba` | 元数据（CRC/Random DISABLE） |
| `journal.c` | `sync_read/write_page`、`sync_erase_block` | `journal_page_addr` / `journal_block_addr` | 元数据，20ms 超时擦除 |
| `checkpoint.c` | `sync_read/write_page` | `metadata_ring_addr`（Checkpoint 池 48-55） | CHECKPOINT module，池内环内轮转 |
| `metadata.c` | `sync_read/write_page` | `metadata_ring_addr`（Anchor 池 16-47）/ `paa_from_addr`（L2P 日志块） | METADATA module |
| `bbt.c` | `sync_read/write_page`、`sync_read_page_raw` | `metadata_ring_addr`（BBT 池 56-57） | BBT module，池内环内轮转 |
| `gc.c` | `sync_write_page`、`sync_erase_block` | `addr_from_pba` / `super_block_page_addr` | 用户数据（CRC/Random ENABLE），20ms 擦除 |
| `wear_leveling.c` | `sync_write_page` | `addr_from_pba` | 同 GC |
| `read_recovery.c` | `sync_read_page` | `addr_from_pba` / `addr_from_paa` | CRC/Random ENABLE + LBA META |
| `power_failure_recovery.c` | `sync_read_page`、`sync_read_page_raw` | `journal_page_addr` | 元数据 + LDPC_RAW |

### 5.4 约束汇总

| 操作 | CRC | Random | LDPC | module | lba |
|------|-----|--------|------|--------|-----|
| 用户数据读/写 | ENABLE | ENABLE | BCH | FTL | lba |
| L2P region 读/写 | DISABLE | DISABLE | BCH | METADATA | 不填 |
| Journal 写 | DISABLE | DISABLE | BCH | METADATA | 不填 |
| GC 搬移读/写 | ENABLE | ENABLE | BCH | GC | lba |
| BBT 读/写 | DISABLE | DISABLE | BCH | BBT | 不填 |
| Checkpoint 读写 | DISABLE | DISABLE | BCH | CHECKPOINT | 不填 |
| Anchor (Directory) 读写 | DISABLE | DISABLE | BCH | METADATA | 不填 |
| BBT 出厂扫描 | DISABLE | DISABLE | LDPC_RAW | BBT | 不填 |

---

## 6. ftl_service() 调度时序

### 6.1 三级模式

```
每次 ftl_service() 调用的执行顺序：

  Phase 1 ── fc_service() 最多 NAND_SUBMIT_BUDGET (4) 次
  ├── 推进 NAND 命令执行
  └── 计数 submitted（本轮新提交的 NAND 命令数）

  Phase 2 ── 处理完成回调
  ├── completion_table_drain()（统一完成泵，v0.19）
  └── ftl_handle_completion() 按 module 路由
        ├── FTL      → 查 pending 表 → 读数据（pending->io.buffer）/ release
        ├── METADATA → release
        └── GC       → release

  Phase 3 ── 三级后台模式
  ├── IDLE 模式 (submitted == 0):        全力后台
  │   ├── l2p_cache_flush_all()
  │   ├── GC refill pre-cond pool
  │   ├── wl_execute()                  ← 磨损均衡
  │   ├── journal_flush_page()
  │   └── checkpoint_create() (按需)
  │
  ├── NORMAL 模式 (submitted ≤ 2):      适度后台
  │   ├── l2p_cache_flush_background(4)
  │   ├── GC (按 gc_check_trigger)
  │   ├── wl_execute() (按 wl_should_trigger)
  │   └── checkpoint_create() (按需)
  │
  └── BUSY 模式 (submitted > 2):        最小后台
      ├── l2p_cache_flush_background(1)
      └── checkpoint_create() (仅 emergency≥95%)
```

### 6.2 完整实现

```c
void ftl_service(ftl_context_t *ftl) {
    /* Phase 1: fc_service 推进 NAND */
    uint32_t submitted = 0;
    for (int i = 0; i < NAND_SUBMIT_BUDGET; i++) {
        if (fc_service() != FC_STATUS_OK) break;
        submitted++;
    }

    /* Phase 2: 处理完成回调 */
    fc_command_status_t completion;
    while (fc_poll_completion(&completion) == FC_STATUS_OK) {
        ftl_handle_completion(ftl, &completion);
    }

    /* Phase 3: 三级后台模式 */
    if (submitted == 0) {
        ftl_service_idle(ftl);
    } else if (submitted <= NAND_SUBMIT_BUDGET / 2) {
        ftl_service_normal(ftl);
    } else {
        ftl_service_busy(ftl);
    }
}
```

---

## 7. 接口文件清单

```
include/ftl/
├── ftl.h                        # FTL 对外 API + ftl_context_t
├── ftl_type.h                   # lba_t / pba_t 基础类型
├── ftl_fc_interface.h           # PBA helper + 同步 NAND 操作
├── l2p_cache.h                  # L2P Cache + CLOCK 替换
├── allocator.h                  # Allocator + Super Block
├── journal.h                    # Journal + 四层保护
├── gc.h                         # GC 搬移 + 触发条件
├── metadata.h                   # Metadata Directory + Anchor Block
├── pending.h                    # Pending Request 表（异步读回调）
├── checkpoint.h                 # Checkpoint + Journal 截断协同
├── bbt.h                        # BBT 坏块表
├── read_recovery.h              # Read Retry + ECC 趋势预测
├── power_failure_recovery.h     # 三类启动恢复
└── wear_leveling.h              # 动态 + 静态磨损均衡

src/ftl/
├── makefile.mk                  # 通配符自动发现
├── ftl.c                        # FTL 对外 API + ftl_service 三级模式
├── ftl_fc_interface.c           # PBA helper + 同步 NAND 操作实现
├── l2p_cache.c                  # CLOCK evict + Region load/flush
├── allocator.c                  # Free Pool + 磨损均衡
├── journal.c                    # append/flush_page/replay/truncate
├── gc.c                         # gc_read_page_with_lba + 7 步搬移
├── metadata.c                   # Directory init/lookup/update/flush/load
├── pending.c                    # Pending 表 add/lookup/remove
├── checkpoint.c                 # 全量 Checkpoint + 三级触发
├── bbt.c                        # BBT 加载/扫描/标记/保存
├── read_recovery.c              # Read Retry + 邻近页辅助
├── power_failure_recovery.c     # 三类启动 + 写指针扫描
└── wear_leveling.c              # 三条件触发 + victim 选择 + 搬移
```

---

## 8. 三层架构与接口关系

```
                 ┌───────────────────────────────────┐
                 │         Host / UART CLI 层          │   ← D10：学习主控 Host 命令来源
                 │   uart_cli_wr/rd/tr/fl/info()      │      （NVMe 已删除，v0.47）
                 │   ———————————————                  │
                 │   lba + n → ssd_request_t           │
                 └──────────────┬────────────────────┘
                                │ ssd_controller_submit()
                    ┌───────────▼──────────────────────────┐
                    │       Controller 层      src/controller/ │
                    │                                        │
                    │  ┌────────────────────────────────┐   │
                    │  │ request.c: ssd_request_split()  │   │
                    │  │  block_count > 1 → N 个单页子请求│   │
                    │  │  on_sub_complete → sub_remaining│   │
                    │  │  → 0 → complete_flag = 1       │   │
                    │  └────────────────────────────────┘   │
                    │           │ ftl_submit_read/write      │
                    │           │ (on_complete, complete_flag)│
                    └───────────┼──────────────────────────┘
                 ┌──────────────▼────────────────────────────┐
                 │          FTL 层         src/ftl/           │
                 │                                            │
                 │  ftl_submit_read(ftl, &req)                │ ← 单页原语
                 │  ftl_submit_write(ftl, &req)               │
                 │    req = ftl_io_request_t                  │
                 │                                            │
                 │  ┌────────────────────────────────────┐   │
                 │  │ ftl_handle_completion()             │   │
                 │  │   ① 数据拷贝 → buffer              │   │
                 │  │   ② complete_flag = 1              │   │
                 │  │   ③ on_complete(ctx) ← 回调聚合    │   │
                 │  │   ④ pending_remove                 │   │
                 │  └────────────────────────────────────┘   │
                 │                                            │
                 │  ┌────────────────────────────────────┐   │
                 │  │ Phase 3 后台 IDLE/NORMAL/BUSY       │   │
                 │  │ GC / WL / Checkpoint                │   │
                 │  └────────────────────────────────────┘   │
                 └──────────────┬──────────────────────────┘
                                │ ftl_cmd_setup_ppa / fc_submit
                 ┌──────────────▼────────────────────────────┐
                 │     FC 层          src/fc/                  │
                 │  fc_submit / fc_service / fc_poll_completion│
                 └─────────────────────────────────────────────┘
```
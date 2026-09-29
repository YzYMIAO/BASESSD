# FTL 模块接口说明文档

> 本文档是 `ftl_design.md` 的配套文档，专注于 FTL 模块的对外接口定义、使用方法和内部模块间的接口约定。
> 实现时以此为参考，实际接口以头文件为准。

---

## 1. FTL 对外 API

FTL 模块通过 `include/ftl/ftl.h` 对外暴露以下接口。
FTL 对外只提供**非阻塞接口**，不提供同步包装。Controller 层通过 `complete_flag` 轮询异步完成状态。

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
| `ftl_submit_read()` | 异步读 | 非阻塞提交读请求（pending 表 + complete_flag 驱动完成） | L2P→PBA→FC submit→pending |
| `ftl_submit_write()` | 异步写 | 非阻塞提交写请求 + Journal + watermark + complete_flag | alloc→FC submit→journal→L2P→pending |
| `ftl_trim()` | 异步取消映射 | 写 FTL_PBA_INVALID 到 L2P Cache，标记 DIRTY | 非阻塞 |
| `ftl_service()` | 主循环 | 推进 FC 调度 + 统一完成泵 + 后台任务（flush/GC/WL/Checkpoint） | 在 main loop 中频繁调用 |

**注**：FTL 不提供同步包装（删除 `ftl_read`/`ftl_write`）。Controller 层通过 `ssd_controller_submit()` 异步提交请求，通过 `ssd_controller_is_complete()` 轮询完成状态。

### 1.3 异步完成机制

所有异步请求通过 `volatile uint8_t complete_flag` 通知完成：

- **读请求**：NAND READ 完成 + 数据拷贝到 caller buffer 后，completion handler 置位 `complete_flag`
- **写请求**：NAND PROGRAM 完成后，completion handler 置位 `complete_flag`
- **TRIM**：立即完成（只改 L2P Cache，不写 NAND）

### 1.3 Completion Table（统一完成路由）

`completion_table.h/c` 提供统一完成事件分发，消除 `ftl_service` 和 `ftl_internal_sync_wait` 各自 `fc_poll_completion` 造成的 completion 丢失问题。

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

### 1.4 完整使用流程

```c
/* 系统启动 */
fc_init();                              // ① FC 初始化
ftl_context_t ftl;

/* 供 FTL 使用的静态数组（由调用者提供存储） */
static uint16_t g_free_blocks[BLOCKS_PER_PLANE * PLANE_NUM];
static uint16_t g_bad_blocks[BLOCKS_PER_PLANE * PLANE_NUM];
static uint32_t g_erase_counts[BLOCKS_PER_PLANE * PLANE_NUM];
static super_block_t g_closed_sb_list[SUPER_BLOCK_LIST_MAX];

ftl_topology_t topo = {0, 0, 0};        // 拓扑参数（当前单芯片）

ftl_init(&ftl, topo,
         g_free_blocks, BLOCKS_PER_PLANE * PLANE_NUM,
         g_bad_blocks,
         g_erase_counts,
         g_closed_sb_list);             // ② FTL 初始化

/* 主循环（企业级异步模式） */
while (1) {
    uart_service_poll();                // 收 Host 命令 → ssd_controller_submit()
    ftl_service(&ftl);                  // ③ 推进 FTL（FC + 完成路由 + 后台任务）
    /* 请求完成状态由 ssd_controller_is_complete() 轮询 */
}
```

---

## 2. L2P Cache 内部接口

### 2.1 数据结构

```c
#define L2P_ENTRIES_PER_REGION   512     // region 内 PBA 数
#define L2P_CACHE_SIZE           64      // cache 行数（32/64/128）
#define L2P_REGIONS_TOTAL        2048    // 总 region 数
#define L2P_BASE_BLOCK           (0U)    // L2P 表起始 Block
#define L2P_TABLE_BLOCKS         (32U)   // L2P 表总 Block 数

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
    uint16_t    region_id;                           // 0..2047
    uint8_t     state;                               // l2p_cache_state_t
    uint16_t    access_count;                        // CLOCK 访问计数
} l2p_cache_line_t;

/* 整体 cache */
typedef struct {
    l2p_cache_line_t  lines[L2P_CACHE_SIZE];
    uint16_t          clock_hand;        // CLOCK 替换指针
    uint32_t          hit_count;         // 统计
    uint32_t          miss_count;
    metadata_dir_cache_t *md_dir;        // Metadata Directory 指针
} l2p_cache_t;

/* 设置 Metadata Directory 指针 */
static inline void l2p_cache_set_md_dir(l2p_cache_t *cache, metadata_dir_cache_t *md_dir);

/* L2P Table 固定位置映射：Region → PBA */
static inline nand_pba_t l2p_region_pba(uint8_t ch, uint8_t ce, uint16_t region_id);
```

### 2.2 接口

```c
/* 初始化 cache（所有 line 置 INVALID） */
void l2p_cache_init(l2p_cache_t *cache);

/*
 * 读取 LBA 的 PBA 映射。
 * 可能触发 cache miss → CLOCK evict → 从 NAND 加载 region。
 * 加载时机：miss 时同步从 NAND 读（ftl_internal_read_page）。
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
l2p_cache_write()   → ftl_submit_write 更新 L2P / journal_replay
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
    ＝ INVALID → l2p_region_pba() 固定位置回退
    ftl_internal_read_page() → 从 NAND 加载 2KB region 数据
    state = CLEAN
```

---

## 3. Journal 内部接口

### 3.1 数据结构

```c
#define JOURNAL_BLOCK_COUNT       (2)     // 环形 2 blocks
#define JOURNAL_BLOCK0            (40)    // J0 固定 Block
#define JOURNAL_BLOCK1            (41)    // J1 固定 Block
#define JOURNAL_ENTRIES_PER_PAGE  (128)   // 每 page 128 条
#define JOURNAL_PAGE_SIZE         (2048)  // 128 × 16B
#define JOURNAL_HEADER_SIZE       (12)    // Header 大小
#define JOURNAL_MAGIC             (0x524F554AUL)  // "JOUR"

/* Journal entry — 16B */
typedef struct {
    uint32_t    lba;            // LBA
    nand_pba_t  old_pba;        // 修改前的 PBA
    nand_pba_t  new_pba;        // 修改后的 PBA
    uint32_t    sequence;       // 单调递增序号
} __attribute__((packed)) l2p_journal_entry_t;

/* Journal 上下文 */
typedef struct {
    uint8_t     ch, ce, lun;            // 拓扑参数
    nand_pba_t  block_pba[2];           // J0/J1 PBA 基址
    uint8_t     current_block;           // 当前写入 block (0/1)
    uint8_t     current_page;            // 当前写入 page (0..63)
    uint8_t     current_entry;           // 当前页内 entry 偏移 (0..127)
    uint32_t    sequence_counter;        // 全局序号
    uint8_t     page_buf[JOURNAL_PAGE_SIZE];  // SRAM buffer
    uint8_t     checkpoint_needed;       // 水位标志
    uint8_t     emergency;               // 紧急标志（≥95%）
} l2p_journal_t;
```

### 3.2 接口

```c
/* 初始化 journal */
void journal_init(l2p_journal_t *journal, uint8_t ch, uint8_t ce, uint8_t lun);

/*
 * 追加一条 journal entry。
 * 内部：写入 page_buf → page_buf 满时 flush 到 NAND。
 */
void journal_append(l2p_journal_t *journal, lba_t lba,
                    nand_pba_t old_pba, nand_pba_t new_pba);

/*
 * 将当前 page_buf 写入 NAND（即使未满也可强制写）。
 * 写满页时回切到另一个 Block（先擦除）。
 */
void journal_flush_page(l2p_journal_t *journal);

/*
 * 启动时回放所有 journal entries → 重建 cache。
 * 扫描 J0/J1，验证 magic/entry_count/sequence，更新 L2P Cache。
 */
int journal_replay(l2p_journal_t *journal, l2p_cache_t *cache);

/*
 * 截断 sequence ≤ up_to 的所有 journal entries。
 * 擦除非当前 Journal Block。
 */
void journal_truncate(l2p_journal_t *journal, uint32_t up_to_sequence);
```

### 3.3 Page 格式

```
Header (12B):
  magic       (4B) = 0x524F554A ("JOUR")
  page_seq    (4B) = 本页第一条 entry 的全局序号
  entry_count (2B) = 本页有效 entry 数 (0~128)
  block_seq   (2B) = 本 Journal Block 写入轮次

Entries (N × 16B, N ≤ 128):
  lba(4B) | old_pba(4B) | new_pba(4B) | sequence(4B)

Padding: (128 - N) × 16B = 0xFF
```

---

## 4. Allocator 内部接口

### 4.1 数据结构

```c
/* 温度等级 */
#define FTL_TEMP_HOT    0U      /* 用户 IO 数据 */
#define FTL_TEMP_COLD   1U      /* GC 搬移 / 元数据 */

/* 三级 Free Pool */
#define FREE_POOL_TARGET        (32U)   /* Pre-conditioned 目标水位 */
#define FREE_POOL_EMERGENCY     (8U)    /* 紧急水位 */
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
    uint16_t    pre_cond_count;         // 预擦除池
    uint16_t    emergency_count;        // 紧急池
    super_block_t hot_sb;               // Hot Open Super Block
    super_block_t cold_sb;              // Cold Open Super Block
    super_block_t *closed_sb_list;      // Closed SB 数组
    uint16_t    closed_sb_count;
    uint32_t   *erase_counts;           // 擦除次数数组
    uint16_t   *bad_blocks;             // 坏块数组
    uint16_t    bad_count;
    uint16_t    spare_pool_count;
} ftl_allocator_t;
```

### 4.2 接口

```c
/* 初始化 allocator */
int allocator_init(ftl_allocator_t *alloc,
                   uint8_t ch, uint8_t ce, uint8_t lun,
                   uint16_t *free_blocks, uint16_t free_block_count,
                   uint16_t *bad_blocks, uint32_t *erase_counts,
                   super_block_t *closed_sb_list);

/*
 * 分配一个 PBA（一页，4 CW）。
 * 从当前温度的 Open Super Block 顺序分配。
 * Super Block 写满 → 关闭（→Closed），从 Free Pool 选新 Block（最小擦除次数）。
 * Free Pool 耗尽 → 返回 NAND_PBA_INVALID。
 */
nand_pba_t allocator_alloc(ftl_allocator_t *alloc, uint8_t temperature);

/* GC 擦除后归还 block 到 Free Pool */
void allocator_free_block(ftl_allocator_t *alloc, uint16_t block);

/* 运行时标记坏块 */
void allocator_mark_bad(ftl_allocator_t *alloc, uint16_t block);

/* 检查是否需要 GC */
int allocator_needs_gc(const ftl_allocator_t *alloc);

/* 同步 pre_cond_count 与 free_count */
void allocator_refill_pre_cond(ftl_allocator_t *alloc);
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

### 5.2 内部同步 NAND 操作

```c
/*
 * 同步等待指定 command_id 完成。
 * 循环 fc_service() + fc_poll_completion()。
 */
int ftl_internal_sync_wait(uint16_t cmd_id, fc_command_status_t *status_out,
                            uint32_t timeout_us);

/*
 * 同步读一页 NAND。
 * 内部：alloc → config → submit → sync_wait → read_data → release
 */
int ftl_internal_read_page(nand_pba_t pba, void *buffer, uint32_t length,
                            uint8_t crc, uint8_t random, uint8_t module,
                            lba_t lba);

/*
 * 同步读全页（LDPC_RAW 模式，跳过 ECC）。
 * 用于出厂坏块扫描。输出 PAGE_SIZE = 2112 字节。
 */
int ftl_internal_read_page_raw(nand_pba_t pba, void *buffer);

/*
 * 同步写一页数据到 NAND。
 */
int ftl_internal_write_page(nand_pba_t pba, const void *data, uint32_t length,
                             uint8_t crc, uint8_t random, uint8_t module,
                             lba_t lba);

/*
 * 同步擦除一个 Block。
 */
int ftl_internal_erase_block(nand_pba_t block_pba);
```

### 5.3 约束汇总

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
  ├── while (fc_poll_completion() == OK)
  └── ftl_handle_completion() 按 module 路由
        ├── FTL      → 查 pending 表 → 读数据 / release
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

## 8. 与 FTL 子模块的接口关系

```
                 ┌─────────────────────────┐
                 │    Controller 层          │
                 │  ssd_controller_submit()  │
                 └────────────┬────────────┘
                              │ ftl_read/write/trim
                 ┌────────────▼────────────┐
                 │      ftl_service()       │←── main loop 频繁调用
                 │                         │
                 │  ┌───────────────────┐  │
                 │  │  ftl_submit_read  │  │  pending 表驱动
                 │  │  ftl_submit_write │  │  journal + cache
                 │  └────────┬──────────┘  │
                 │           │             │
                 │  ┌────────▼──────────┐  │
                 │  │ ftl_handle_       │  │  completion 路由
                 │  │ completion()      │  │  FTL/METADATA/GC
                 │  └───────────────────┘  │
                 │                         │
                 │  ┌───────────────────┐  │
                 │  │ Phase 3 后台      │  │
                 │  │ IDLE/NORMAL/BUSY  │  │
                 │  │ GC/WL/Checkpoint  │  │
                 │  └───────────────────┘  │
                 └────────────┬────────────┘
                              │ ftl_cmd_setup_ppa / ftl_internal_*
                 ┌────────────▼────────────┐
                 │     FC 层               │
                 │  fc_submit / fc_service  │
                 │  fc_poll_completion     │
                 └─────────────────────────┘
```
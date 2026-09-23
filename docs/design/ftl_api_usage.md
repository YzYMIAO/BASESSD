# FTL 模块接口说明文档

> 本文档是 `ftl_design.md` 的配套文档，专注于 FTL 模块的对外接口定义、使用方法和内部模块间的接口约定。
> 实现时以此为参考，实际接口以头文件为准。

---

## 1. FTL 对外 API

FTL 模块通过 `include/ftl/ftl.h` 对外暴露以下接口。

### 1.1 核心数据结构

```c
/* FTL 主上下文 */
struct ftl_context {
    l2p_cache_t       cache;             // SRAM L2P cache
    ftl_allocator_t   alloc;             // 空闲 block / 写指针
    l2p_journal_t     journal;           // log-structured journal
    uint32_t          cp_sequence;       // 当前 checkpoint 序号
    uint32_t          cp_interval;       // checkpoint 触发间隔
    uint32_t          total_writes;      // 写入总次数
    uint32_t          cache_hits;        // L2P cache 统计
    uint32_t          cache_misses;
};
```

### 1.2 接口列表

| 接口 | 类型 | 说明 | 备注 |
|------|------|------|------|
| `ftl_init()` | 初始化 | 初始化 L2P cache、allocator、journal | 在 `fc_init()` 之后调用 |
| `ftl_read()` | 读写 | 按 LBA 读取用户数据 | 内部通过 FC 完成 |
| `ftl_write()` | 读写 | 按 LBA 写入用户数据 | alloc → FC 写 → journal → cache |
| `ftl_trim()` | 读写 | 取消 LBA 映射 | 仅改 L2P+bitmap，不擦除数据 |
| `ftl_service()` | 主循环 | 推进 FC 调度 + 后台任务 | 在 main loop 中频繁调用 |
| `ftl_background_flush()` | 后台 | 强制 flush dirty cache | 可在 gc/checkpoint 前调用 |
| `ftl_gc_execute()` | 后台 | 执行 GC（同步阻塞） | 不推荐直接调用，由 ftl_service 触发 |
| `ftl_checkpoint()` | 后台 | 创建 checkpoint | 由 ftl_service 按 cp_interval 触发 |

### 1.3 完整使用流程

```c
/* 系统启动 */
fc_init();                              // ① FC 初始化
ftl_context_t ftl;
ftl_init(&ftl);                         // ② FTL 初始化（加载 checkpoint/journal）

/* 主循环 */
while (1) {
    ftl_service(&ftl);                  // ③ 推进 FTL（含 fc_service 内部调用）
    
    /* 处理 Host IO 请求（在 ftl_service 之外或内部集成） */
    if (host_request_available()) {
        ssd_request_t req = host_dequeue();
        switch (req.opcode) {
            case SSD_REQUEST_READ:
                ftl_read(&ftl, req.lba, req.buffer, req.count);
                break;
            case SSD_REQUEST_WRITE:
                ftl_write(&ftl, req.lba, req.buffer, req.count);
                break;
            case SSD_REQUEST_TRIM:
                ftl_trim(&ftl, req.lba, req.count);
                break;
        }
    }
}
```

### 1.4 同步模型说明

```
FTL 对外接口目前是同步阻塞式的：
  ftl_read/write() 内部：
    fc_submit() → loop { fc_service() } → fc_poll_completion() → return

这意味着 ftl_read/write() 返回时，数据已经写入/读取完成。
主循环中的 ftl_service() 处理后台任务（flush、GC、checkpoint）。

未来可改为完全异步：ftl_submit_read/write() 返回 command_id，
由 ftl_service() 中的 completion handler 处理回调。
```

---

## 2. L2P Cache 内部接口

### 2.1 数据结构

```c
// 以下三个值可配置（由 l2p_cache.h 宏定义确定编译期值）
#define L2P_ENTRIES_PER_REGION   512     // region 内 PAA 数
#define L2P_CACHE_SIZE           64      // cache 行数（32/64/128）
#define L2P_REGIONS_TOTAL        2048    // 总 region 数

// Cache line 状态
typedef enum {
    L2P_CACHE_INVALID  = 0,    // cache 行无效
    L2P_CACHE_CLEAN    = 1,    // 有效，未修改
    L2P_CACHE_DIRTY    = 2,    // 有效，已修改（需写回）
    L2P_CACHE_FLUSHING = 3,    // 正在写回 NAND
} l2p_cache_state_t;

/* 单条 cache line = 2KB + 元数据 */
typedef struct {
    nand_paa_t  entries[L2P_ENTRIES_PER_REGION];   // 512 × 4B = 2048B
    uint16_t    region_id;                           // 0..2047
    uint8_t     state;             // l2p_cache_state_t
    uint16_t    access_count;      // CLOCK 访问计数
} l2p_cache_line_t;

/* 整体 cache */
typedef struct {
    l2p_cache_line_t  lines[L2P_CACHE_SIZE];
    uint16_t          clock_hand;        // CLOCK 替换指针
    uint32_t          hit_count;         // 统计
    uint32_t          miss_count;
} l2p_cache_t;
```

### 2.2 接口

```c
/* 初始化 cache（所有 line 置 INVALID） */
void l2p_cache_init(l2p_cache_t *cache);

/*
 * 读取 LBA 的 PAA 映射。
 * 可能触发 cache miss → 从 NAND 加载 region。
 * 返回 FTL_PAA_INVALID 表示该 LBA 未映射。
 */
nand_paa_t l2p_cache_read(l2p_cache_t *cache, ssd_lba_t lba);

/*
 * 写入 LBA 的 PAA 映射。
 * 只修改 cache，标记 DIRTY。写回 NAND 由 flush 决定。
 */
void l2p_cache_write(l2p_cache_t *cache, ssd_lba_t lba, nand_paa_t pa);

/* 写回单条 cache line */
void l2p_cache_flush_one(l2p_cache_t *cache, int line_idx);

/* 写回所有 DIRTY cache lines */
void l2p_cache_flush_all(l2p_cache_t *cache);

/* 后台写回：最多写回 max_count 条最旧的 DIRTY line */
void l2p_cache_flush_background(l2p_cache_t *cache, int max_count);
```

### 2.3 内部挂载点

```
l2p_cache_read()    → 被 ftl_read() / ftl_write() 调用
l2p_cache_write()   → 被 ftl_write() 的 L2P 更新步骤调用
l2p_cache_flush_*   → 被 ftl_service() / background_flush() 调用
```

---

## 3. Journal 内部接口

### 3.1 数据结构

```c
#define JOURNAL_BLOCKS           (2)     // 环形 2 blocks
#define JOURNAL_ENTRIES_PER_PAGE (128)   // 每 page 128 条
#define JOURNAL_PAGE_SIZE        (2048)  // 128 × 16B

/* Journal entry — 16B */
typedef struct {
    uint32_t    lba;            // LBA
    nand_paa_t  old_paa;        // 修改前的 PAA
    nand_paa_t  new_paa;        // 修改后的 PAA
    uint32_t    sequence;       // 单调递增序号
} __attribute__((packed)) l2p_journal_entry_t;

/* Journal 上下文 */
typedef struct {
    uint16_t    current_block;      // 当前写入 block
    uint8_t     current_page;       // 当前写入 page
    uint8_t     current_entry;      // 当前页内 entry 偏移
    uint32_t    sequence_counter;   // 全局序号
    uint8_t     page_buf[JOURNAL_PAGE_SIZE];  // 缓存在 SRAM 中，写满再提交
} l2p_journal_t;
```

### 3.2 接口

```c
/* 初始化 journal（扫描 NAND 确定当前写入位置） */
void journal_init(l2p_journal_t *journal);

/*
 * 追加一条 journal entry。
 * 内部：写入 page_buf → page_buf 满时 flush 到 NAND。
 * 在 NAND program 完成后返回（同步）。
 */
void journal_append(l2p_journal_t *journal, ssd_lba_t lba,
                    nand_paa_t old_paa, nand_paa_t new_paa);

/*
 * 将当前 page_buf 写入 NAND（即使未满也可强制写）。
 * 用于 checkpoint 前确保 journal 完整性。
 */
void journal_flush_page(l2p_journal_t *journal);

/*
 * 启动时回放所有 journal entries → 重建 cache。
 * 返回回放的 entry 数。
 */
int journal_replay(l2p_journal_t *journal, l2p_cache_t *cache);

/*
 * 截断 sequence ≤ up_to 的所有 journal entries。
 * 标记已应用的条目可回收，必要时擦除 journal block。
 */
void journal_truncate(l2p_journal_t *journal, uint32_t up_to_sequence);
```

### 3.3 写入流程

```
journal_append(lba, old, new)：
  ① page_buf[current_entry++] = (lba, old, new, seq++)
  ② 如果 current_entry == 128：
     ③ flush page_buf → NAND（通过 FC PROGRAM）
     ④ current_page++
     ⑤ 如果 current_page == 64：
         ⑥ current_block = (current_block + 1) % 2
         ⑦ 擦除新 block
         ⑧ current_page = 0
     ⑨ current_entry = 0
  ③ 返回
```

---

## 4. Allocator 内部接口

### 4.1 数据结构

```c
typedef struct {
    uint16_t  open_block[PLANE_NUM];     // 每 Plane 当前写入 block
    uint8_t   next_page[PLANE_NUM];      // 每 Plane 当前写入 page
    uint16_t *free_blocks;               // 空闲 block 列表（数组）
    uint16_t  free_count;                // 空闲 block 数
    uint16_t *bad_blocks;                // 坏 block 列表
    uint16_t  bad_count;
    uint32_t *erase_counts;              // 每 block 擦除次数
    uint16_t  total_blocks;              // 总好 block 数
} ftl_allocator_t;
```

### 4.2 接口

```c
/*
 * 初始化 allocator：
 *   1. 扫描坏块 → 构建 bad_blocks 列表
 *   2. 排除元数据 block（Block 0~39）
 *   3. 构建 free_blocks 列表
 *   4. 初始化 open_block 为 free_blocks[0]
 */
int allocator_init(ftl_allocator_t *alloc);

/*
 * 分配一个物理页面（PAA, cw=0）。
 * 从当前 open_block 按顺序分配。
 * open_block 写满 → 自动切到 free_blocks 下一个。
 * 无空闲块时返回 FTL_PAA_INVALID。
 */
nand_paa_t allocator_alloc(ftl_allocator_t *alloc);

/* GC 擦除后归还 block 到 free pool */
void allocator_free_block(ftl_allocator_t *alloc, uint16_t block_no);

/* 运行时标记坏块 */
void allocator_mark_bad(ftl_allocator_t *alloc, uint16_t block_no);

/* 检查是否需要 GC */
int allocator_needs_gc(const ftl_allocator_t *alloc);
```

### 4.3 磨损均衡

```c
/* 分配时选擦除次数最少的 block */
static uint16_t allocator_select_block(ftl_allocator_t *alloc) {
    // 遍历 free_blocks，选 erase_counts 最小的
    uint16_t best_idx = 0;
    uint32_t min_erase = alloc->erase_counts[alloc->free_blocks[0]];

    for (int i = 1; i < alloc->free_count; i++) {
        uint32_t cnt = alloc->erase_counts[alloc->free_blocks[i]];
        if (cnt < min_erase) {
            min_erase = cnt;
            best_idx = i;
        }
    }
    return alloc->free_blocks[best_idx];
}
```

---

## 5. FTL → FC 接口映射

### 5.1 FC 接口包装

FTL 不直接调用 `fc_command_alloc/submit/poll` 裸接口，而是通过以下内部包装函数使用 FC：

```c
/* FTL write: 通过 FC 写入一页数据 */
static int ftl_fc_write_page(nand_paa_t paa, const uint8_t *data, 
                              uint16_t len, ssd_lba_t lba)
{
    fc_command_t *cmd = fc_command_alloc();
    cmd->config.operation = OP_NORMAL_PROGRAM_PAGE;
    cmd->config.cw = 3;
    cmd->config.crc = FC_CRC_ENABLE;
    cmd->config.random = FC_RANDOM_ENABLE;
    cmd->config.ldpc = FC_ECC_MODE_BCH;
    cmd->config.page_type = FC_PAGE_TYPE_LP;
    cmd->config.module = FC_COMMAND_MODULE_FTL;
    cmd->paa = paa;
    cmd->faa = lba;

    fc_command_configure_descriptors(cmd);
    fc_write_cmd_data(cmd, data, len);
    fc_submit(cmd);

    // 同步等待（简单的 polling 方式）
    fc_command_status_t completion;
    do {
        fc_service();
    } while (fc_poll_completion(&completion) != FC_STATUS_OK);

    int status = (completion.status0 == FC_CW_STATUS_READY) 
                 ? SSD_STATUS_OK : SSD_STATUS_IO_ERROR;
    fc_command_release(cmd);
    return status;
}

/* FTL read: 通过 FC 读取一页数据 */
static int ftl_fc_read_page(nand_paa_t paa, uint8_t *data, 
                             uint16_t len, ssd_lba_t lba)
{
    fc_command_t *cmd = fc_command_alloc();
    cmd->config.operation = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
    cmd->config.cw = 3;
    cmd->config.crc = FC_CRC_ENABLE;
    cmd->config.random = FC_RANDOM_ENABLE;
    cmd->config.ldpc = FC_ECC_MODE_BCH;
    cmd->config.page_type = FC_PAGE_TYPE_LP;
    cmd->config.module = FC_COMMAND_MODULE_FTL;
    cmd->paa = paa;
    cmd->faa = lba;

    fc_command_configure_descriptors(cmd);
    fc_submit(cmd);

    fc_command_status_t completion;
    do {
        fc_service();
    } while (fc_poll_completion(&completion) != FC_STATUS_OK);

    // 读取数据（从 CW0 取 512B）
    uint8_t *cw_data = fc_get_cmd_data_ptr(cmd, 0);
    memcpy(data, cw_data, len);

    fc_command_release(cmd);
    return SSD_STATUS_OK;
}
```

### 5.2 元数据读写（L2P region / journal）

```c
/* 元数据读取：不加扰、无 CRC */
static int ftl_fc_read_meta(nand_paa_t paa, uint8_t *data, uint16_t len) {
    // 同 ftl_fc_read_page，但 CRC=RANDOM=DISABLE
    // 且 module = FC_COMMAND_MODULE_METADATA
}

/* 元数据写入：不加扰、无 CRC */
static int ftl_fc_write_meta(nand_paa_t paa, const uint8_t *data, uint16_t len) {
    // 同 ftl_fc_write_page，但 CRC=RANDOM=DISABLE
    // 且 module = FC_COMMAND_MODULE_METADATA
}
```

### 5.3 约束汇总

| 操作 | CRC | Random | LDPC | module | faa |
|------|-----|--------|------|--------|-----|
| 用户数据读/写 | ENABLE | ENABLE | BCH | FTL | lba |
| L2P region 读/写 | DISABLE | DISABLE | BCH | METADATA | 不填 |
| Journal 写 | DISABLE | DISABLE | BCH | METADATA | 不填 |
| GC 搬移读/写 | ENABLE | ENABLE | BCH | GC | lba |

---

## 6. ftl_service() 调度时序

### 6.1 单次 ftl_service() 的执行顺序

```c
void ftl_service(ftl_context_t *ftl) {
    /* Step 1: FC 调度（驱动 NAND 命令推进） */
    fc_service();

    /* Step 2: 处理 FC 完成回调 */
    fc_command_status_t completion;
    while (fc_poll_completion(&completion) == FC_STATUS_OK) {
        ftl_process_completion(ftl, &completion);
    }

    /* Step 3: L2P cache 后台 flush */
    // 如果 dirty line 数 > cache_size / 2，flush 最旧的几条
    l2p_cache_flush_background(&ftl->cache, 4);

    /* Step 4: 检查 GC 触发条件 */
    if (allocator_needs_gc(&ftl->alloc) && !ftl->gc_in_progress) {
        ftl_gc_execute(ftl);  // 同步 GC
    }

    /* Step 5: 检查 Checkpoint 触发条件 */
    ftl->total_writes++;
    if (ftl->total_writes % ftl->cp_interval == 0) {
        ftl_checkpoint(ftl);
    }
}
```

### 6.2 执行频率

| 任务 | 频率 | 说明 |
|------|------|------|
| fc_service() | 每次 ftl_service() | NAND 命令推进 |
| Background flush | 每次 | 最多 4 条 dirty line |
| GC 检查 | 每次 | 仅在 free_count < 阈值时执行 |
| Checkpoint | 每 N 次写入 | cp_interval 控制（如每 1000 次写）|

---

## 7. 接口文件清单

```
include/ftl/
├── ftl.h              # FTL 对外 API（ftl_init/read/write/trim/service）
├── l2p_cache.h        # L2P Cache 内部接口
├── journal.h          # Journal 内部接口
├── allocator.h        # Allocator 内部接口
├── bbt.h              # 坏块表接口
└── gc.h               # GC 接口

src/ftl/
├── ftl.c              # FTL 对外 API 实现
├── l2p_cache.c        # L2P Cache 实现（CLOCK、miss/evict/flush）
├── journal.c          # Journal 实现（append、replay、truncate）
├── allocator.c        # Allocator 实现（自由列表、磨损均衡）
├── bbt.c              # 坏块表实现（扫描、隔离）
├── gc.c               # GC 实现（victim选择、搬移、擦除）
├── checkpoint.c       # Checkpoint 实现（全量/增量）
└── read_recovery.c    # Read Recovery（桩）
```

---

## 8. 与现有代码的兼容性

FTL 重构后，以下现有文件需要改造：

| 文件 | 改动 | 原因 |
|------|------|------|
| `src/ftl/ftl.c` | 重构：通过 FC 接口访问 NAND | 绕过 FC 问题 |
| `src/ftl/l2p.c` | 替换为 `l2p_cache.c` | 改为 region cache + CLOCK |
| `include/ftl/ftl.h` | 修改 `ftl_context_t` | 去掉 nand_device_t 依赖 |
| `include/ftl/l2p.h` | 替换为 `l2p_cache.h` | 接口变更 |
| `config/ftl_config.h` | 新增配置项 | L2P_CACHE_SIZE、cp_interval 等 |
| `src/controller/controller.c` | 改为非阻塞 + 通过 FTL | 同步→异步适配 |
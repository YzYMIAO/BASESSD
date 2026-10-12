#ifndef FTL_JOURNAL_H
#define FTL_JOURNAL_H

/*
 * journal.h — L2P 增量日志：页/条目格式 + 环形状态 + API
 *
 * 职责：
 *   定义 Journal 的磁盘/内存布局（页头、条目、环形状态 l2p_journal_t）与对外 API。
 *   实现见 src/ftl/journal.c。
 *
 * 与 checkpoint.h 的分工：
 *   Journal  —— L2P 的**增量**日志（页追加，掉电回放的最小单位）；
 *   Checkpoint —— L2P 的**全量**快照（把 journal 的已写条目固化进 NAND L2P 表并截断 journal）。
 *
 * 布局：
 *   Journal 是 JOURNAL_BLOCK_COUNT 个块的环形池（块位置 JOURNAL_BLOCK0 起）；
 *   每页 = 页头（12B：magic + page_seq + entry_count + block_seq）+ N 条 l2p_journal_entry_t。
 *   N 由 (页主区 − 页头) ÷ 条目大小**推导**，不写死（见 JOURNAL_ENTRIES_PER_PAGE）。
 *
 * ⚠ 本模块整个不转断言（assertion_guidelines §六 辨析 C）：replay 读的是 NAND 页，
 *   页头可能因掉电撕裂而非法，属预期情形，返回错误/跳过而非断言。
 */

#include "ftl/ftl_config.h" /* JOURNAL_BLOCK0 / JOURNAL_BLOCK_COUNT 块位置 */
#include "ftl/ftl_type.h"
#include "ftl/l2p_cache.h"
#include "nand/nand_config.h"

#include <stdint.h>

/* Journal 块位置：第 b 块 = JOURNAL_BLOCK0 + b，共 JOURNAL_BLOCK_COUNT 块
 * （块位置与块数见 ftl/ftl_config.h §二/§三）*/

/* Journal 页大小 = 一页主区（由 nand_config.h 推出）*/
#define JOURNAL_PAGE_SIZE (PAGE_MAIN_SIZE)

#define JOURNAL_FULL_THRESHOLD (85U)
#define JOURNAL_CRITICAL       (95U)

/* 「无预擦除块」哨兵（P1-6：journal_truncate 预擦除记账，见 l2p_journal_t） */
#define JOURNAL_BLOCK_NONE (0xFFU)

/* Journal Page Header */
#define JOURNAL_HEADER_SIZE (12U)          /* magic(4) + page_seq(4) + entry_count(2) + block_seq(2) */
#define JOURNAL_MAGIC       (0x524F554AUL) /* "JOUR" */

/* 一条 L2P 更新：lba 的映射从 old_pba 变为 new_pba（16B，packed 无填充） */
typedef struct
{
    uint32_t   lba;      /* 逻辑地址 */
    nand_pba_t old_pba;  /* 更新前物理地址（回放不读；仅首条保留，供诊断） */
    nand_pba_t new_pba;  /* 更新后物理地址（回放按此写 L2P） */
    uint32_t   sequence; /* 全局单调序号（回放按 (page_seq, 页内下标) 定序） */
} __attribute__((packed)) l2p_journal_entry_t;

/* 每页条目数 = (页主区 − 页头) ÷ 条目大小，向下取整。
 * ⚠ 必须推导：旧版写死 128 而条目 16B ⇒ 12 + 128×16 = 2060 > 2048，
 *   第 127 条会写出 page_buf 之外（踩到 l2p_journal_t 的后续字段）。*/
#define JOURNAL_ENTRIES_PER_PAGE ((JOURNAL_PAGE_SIZE - JOURNAL_HEADER_SIZE) / sizeof(l2p_journal_entry_t))

typedef struct
{
    ftl_topology_t topo; /* 目标 NAND 芯片路由 */
    nand_pba_t     block_pba[JOURNAL_BLOCK_COUNT];

    uint8_t  current_block;
    uint8_t  current_page;
    uint8_t  current_entry;
    uint32_t sequence_counter;

    /* P1-6：被 journal_truncate **预擦除**、等待 flush_page 回切复用的环形块号
     * （JOURNAL_BLOCK_NONE = 无）。记账目的 = 同一块每圈只擦一次：旧实现 truncate
     * 擦环形下一块、flush_page 回切又擦一次 ⇒ 双倍磨损（见 journal.c truncate 注释）。 */
    uint8_t pre_erased_block;

    uint8_t page_buf[JOURNAL_PAGE_SIZE];

    uint8_t checkpoint_needed;
    uint8_t emergency;
} l2p_journal_t;

/* journal_init - 初始化 Journal：记录拓扑、预铸各块 PBA、复位写指针与序号 */
void journal_init(l2p_journal_t *journal, ftl_topology_t topo);
/* 追加一条 L2P 更新。P1-6：若当前页缓冲里已有**同一 LBA** 的条目，就地更新
 * （只改 old/new PBA，不新增条目、不消耗 sequence）——覆盖写工作集下减少 Journal 页数。 */
void journal_append(l2p_journal_t *journal, lba_t lba, nand_pba_t old_pba, nand_pba_t new_pba);
/* journal_flush_page - 把当前页缓冲（含页头）同步写进 NAND 并推进写指针；
 *   本函数是**唯一**的环形块擦除点，块满换块时先擦后写（跳过 pre_erased_block 预擦块） */
void journal_flush_page(l2p_journal_t *journal);
/* F10：全量回放 page_seq > cp_sequence 的页（按 (page_seq, entry_index) 递增序
 * 应用，per-LBA last-write-wins）。cp_sequence 为 checkpoint 截断水位（旧
 * journal_replay 只回放最大一页，掉电跨 ≥2 页时其余条目静默丢失）。 */
ftl_status_t journal_replay(l2p_journal_t *journal, l2p_cache_t *cache, uint32_t cp_sequence);
/*
 * journal_truncate - 截断 Journal（丢弃 ≤ up_to_sequence 的条目，P1-6）
 *
 * 语义：Checkpoint 已把 sequence ≤ up_to_sequence 的条目 flush 进 NAND L2P 表 ⇒
 * 环形里**最旧的一块**（= 环形下一块）整块可弃。本函数预擦除该块并记账
 * （pre_erased_block），使 journal_flush_page 回切到它时**跳过**擦除——保证同一块
 * 每圈只擦一次（旧实现在两处各擦一次 = 双倍磨损，直击 §14.1 的寿命瓶颈）。
 *
 * @param up_to_sequence 截断水位；**必须覆盖全部已写条目**（调用者传 sequence_counter）。
 *        若水位不足以覆盖（有未 checkpoint 的条目），本函数**不截断**——最旧块可能仍
 *        持有回放需要的条目，擦掉即丢数据。
 */
void journal_truncate(l2p_journal_t *journal, uint32_t up_to_sequence);

#endif /* FTL_JOURNAL_H */
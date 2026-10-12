#ifndef FTL_JOURNAL_H
#define FTL_JOURNAL_H

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

/* Journal Page Header */
#define JOURNAL_HEADER_SIZE (12U)          /* magic(4) + page_seq(4) + entry_count(2) + block_seq(2) */
#define JOURNAL_MAGIC       (0x524F554AUL) /* "JOUR" */

typedef struct
{
    uint32_t   lba;
    nand_pba_t old_pba;
    nand_pba_t new_pba;
    uint32_t   sequence;
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

    uint8_t page_buf[JOURNAL_PAGE_SIZE];

    uint8_t checkpoint_needed;
    uint8_t emergency;
} l2p_journal_t;

void journal_init(l2p_journal_t *journal, ftl_topology_t topo);
void journal_append(l2p_journal_t *journal, lba_t lba, nand_pba_t old_pba, nand_pba_t new_pba);
void journal_flush_page(l2p_journal_t *journal);
/* F10：全量回放 page_seq > cp_sequence 的页（按 (page_seq, entry_index) 递增序
 * 应用，per-LBA last-write-wins）。cp_sequence 为 checkpoint 截断水位（旧
 * journal_replay 只回放最大一页，掉电跨 ≥2 页时其余条目静默丢失）。 */
ftl_status_t journal_replay(l2p_journal_t *journal, l2p_cache_t *cache, uint32_t cp_sequence);
void         journal_truncate(l2p_journal_t *journal, uint32_t up_to_sequence);

#endif /* FTL_JOURNAL_H */
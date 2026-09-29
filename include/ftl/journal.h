#ifndef FTL_JOURNAL_H
#define FTL_JOURNAL_H

#include "ftl/ftl_type.h"
#include "ftl/l2p_cache.h"
#include "nand/nand_config.h"

#include <stdint.h>

#define JOURNAL_BLOCK_COUNT (2U)
#define JOURNAL_BLOCK0      (40U)
#define JOURNAL_BLOCK1      (41U)

#define JOURNAL_ENTRIES_PER_PAGE (128U)
#define JOURNAL_PAGE_SIZE        (2048U)

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

typedef struct
{
    ftl_topology_t topo;          /* 目标 NAND 芯片路由 */
    nand_pba_t     block_pba[2];

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
ftl_status_t journal_replay(l2p_journal_t *journal, l2p_cache_t *cache);
void journal_truncate(l2p_journal_t *journal, uint32_t up_to_sequence);

#endif /* FTL_JOURNAL_H */
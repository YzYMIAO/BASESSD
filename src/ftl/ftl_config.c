#include "ftl/ftl_config.h"

/*
 * g_ftl_partitions — 固定元数据分区表（单一事实来源）
 *
 * 每项完全由 ftl_config.h 的宏派生（块号/块数/冗余/表字节均无手写数字）。
 * 消费方：
 *   ① ftl_is_metadata_block()（src/ftl/metadata.c）—— 判固定元数据块
 *   ② 开机分区打印（可选）
 *
 * 区间为 [first_block, first_block + block_count)；**各 (ch,ce,lun) 用同一份块号**
 * （分片模型：每个 die 一套完整元数据，分区不随维度错开）。
 * BBT 主/备、Checkpoint 主备各拆成独立项（便于命名与单独打印）。
 */
const ftl_partition_t g_ftl_partitions[] = {
    {   "L2P Table",    L2P_BASE_BLOCK,          L2P_BLOCKS,                    1U, FTL_PART_FIXED,        FTL_L2P_TABLE_BYTES},
    {      "Anchor",     ANCHOR_BLOCK0,       ANCHOR_BLOCKS,     ANCHOR_REDUNDANCY, FTL_PART_FIXED,     FTL_ANCHOR_TABLE_BYTES},
    {         "BBT",         BBT_BLOCK,                  1U,        BBT_REDUNDANCY, FTL_PART_FIXED,        FTL_BBT_TABLE_BYTES},
    {"BBT (backup)",  BBT_BLOCK_BACKUP,                  1U,        BBT_REDUNDANCY, FTL_PART_FIXED,        FTL_BBT_TABLE_BYTES},
    {  "Checkpoint", CHECKPOINT_BLOCK0,   CHECKPOINT_BLOCKS, CHECKPOINT_REDUNDANCY, FTL_PART_FIXED, FTL_CHECKPOINT_TABLE_BYTES},
    {     "Journal",    JOURNAL_BLOCK0, JOURNAL_BLOCK_COUNT,                    1U, FTL_PART_FIXED,    FTL_JOURNAL_TABLE_BYTES},
    {        "Wear",        WEAR_BLOCK,         WEAR_BLOCKS,           WEAR_COPIES, FTL_PART_FIXED,       FTL_WEAR_TABLE_BYTES},
};

const uint32_t g_ftl_partition_count = (uint32_t)(sizeof(g_ftl_partitions) / sizeof(g_ftl_partitions[0]));

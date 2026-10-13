#ifndef FTL_BBT_H
#define FTL_BBT_H

/*
 * BBT（Bad Block Table）— 坏块表持久化
 *
 * 三个阶段：
 *   1. 出厂扫描：遍历所有 Block，读第 0 页判断坏块标记
 *   2. 运行时隔离：PROGRAM/ERASE 失败 → BBT 标记 + allocator 跳过
 *   3. 替换机制：Block Sparing — 从 Spare Pool 取好块补充 Free Pool
 *
 * BBT 存储在固定元数据区的 BBT 池（`BBT_POOL_BLOCKS` 块轮转，
 * **一条 record = 1 页**，块满换池内下一块并先擦）——池首块/块数由
 * ftl/ftl_config.h 按几何推导，见 §二/§三。
 * bad_map 覆盖全部 FTL_BLOCK_TOTAL 个 Block（NAND 持久化格式，保持不变）。
 * RAM 运行时坏块位统一存于 ftl_block_t.state（Block State Table 单数据源），
 * bbt_save 时从 BST 重建 bad_map。
 *
 * 写模型与 Anchor / Checkpoint 一致（见 ftl/metadata.h §元数据写模型）。
 * ⚠ 旧实现把同一份 6 页（主 + 备各 3 页）**就地重写**：第二个坏块出现时 BBT 即
 *   损坏 ⇒ bbt_load 失败 ⇒ 开机不知道哪些块坏 ⇒ 用到坏块丢数据。
 */

#include "ftl/allocator.h"
#include "ftl/ftl_config.h" /* BBT_BLOCK / BBT_POOL_BLOCKS / BBT_MAX_BLOCKS / BBT_BITMAP_SIZE */

#include <stddef.h>
#include <stdint.h>

/* ── BBT 尺寸与块位置（BBT_BLOCK / BBT_POOL_BLOCKS / BBT_MAX_BLOCKS /
 *    BBT_BITMAP_SIZE）见 ftl/ftl_config.h ── */

/* ── BBT Header 格式 ── */
#define BBT_MAGIC (0x424254ABUL) /* "BBT\xAB" */

typedef struct
{
    uint32_t magic;        /* BBT_MAGIC */
    uint32_t sequence;     /* 单调递增，最新有效 */
    uint32_t total_blocks; /* 总 Block 数 */
    uint32_t bad_count;    /* 坏块数 */
    uint32_t reserved[3];
    uint8_t  bad_map[BBT_BITMAP_SIZE];  /* 4096 bit = 512B */
} __attribute__((packed)) bbt_header_t; /* 4+4+4+4+12+512 = 540B（无独立 CRC，见 ADR v0.62）*/

/* ── 接口 ── */

/*
 * bbt_init - BBT 初始化
 *
 * 从 NAND BBT 池加载 BBT。如果无有效 BBT（首次使用），
 * 执行出厂扫描构建初始 BBT。坏块位写入 alloc->blocks[].state。
 *
 * @param alloc  Allocator 上下文（blocks、bad_count 等输出）
 * @return 0 成功，-1 失败
 */
ftl_status_t bbt_init(ftl_allocator_t *alloc);

/*
 * bbt_scan_factory - 出厂坏块扫描
 *
 * 遍历所有 Block（**跳过 Block 0**）的 Page 0，以 **LDPC_RAW 模式**（跳过 ECC）
 * 读取原始 CW 数据（`ftl_sync_read_page_raw`）。
 * 扫描策略（因 FC 层无独立 OOB 接口）：
 *   - 读取返回非 0（FTL_RESULT_FAIL）→ 出厂坏块
 *   - 读取成功 → 好块
 *
 * 设计文档中的 OOB[0] != 0xFF 检测需要 FC 层支持
 * 单独的 OOB/Spare 区读取，当前 fallback 到「读失败」判据（raw 模式无 ECC，
 * 物理失败未必置 NAND status，故以返回码为准）。
 *
 * @param alloc   Allocator 上下文（blocks 输出）
 * @return 检测到的坏块数
 */
int bbt_scan_factory(ftl_allocator_t *alloc);

/*
 * bbt_print_bad_map - 逐块探测并打印坏块图（**诊断用**）
 *
 * 默认**只读**：对每个块读 Page 0 的 spare 首字节（**列 2048** = `PAGE_MAIN_SIZE`），
 * 非 0xFF ⇒ 出厂坏块。不写介质，可反复调用。
 *
 * ⚠ do_erase != 0 时**额外擦除本实例每个 plane 的每一个块**（含
 *   0..FTL_METADATA_BLOCK_END 的元数据区）⇒ 会抹掉该实例的全部 FTL 元数据与用户数据。
 *   只在专门的坏块体检时用，绝不可放进开机路径。
 *   擦除是**无条件**的：即使 ① 已判定该块坏，也照擦（"全片擦除"必须连坏块一起抹掉，
 *   否则元数据块被跳过 ⇒ 擦完 FTL 仍读回旧状态）。
 *
 * 判定（两条任一成立即坏块）：
 *   ① **擦除前**读该块 Page 0 列 2048 的 spare 首字节，非 0xFF（出厂坏块标记，x8 为 00h）；
 *   ② do_erase 非 0 且擦除命令失败（stuck-at-0 / 不可擦）。
 *
 * ① 必须**先于任何 PROGRAM/ERASE** 读——器件手册（Micron AN1819）逐字：
 *   *"the bad block information is erasable and cannot be recovered once erased"*。
 *   旧实现先擦后读、且读的是 CW0 列 0（FC 逻辑 META），故永远读不到出厂标记。
 *   列 2048 由 ftl_sync_read_column()（OP_READ_COLUMN）够到。
 *
 * 打印（每排 BBT_MAP_COLS_PER_PLANE 个块，各 plane 一组、组间留空；每格定宽 4）：
 *     CH=0  CE=0  LUN=0
 *                      PLANE=0                          PLANE=1
 *       FF   FF 2047   FF   FF   FF   FF   FF      FF   FF   FF   FF   FF   FF   FF   FF
 *   好块打 `FF`，坏块打**本 plane 的绝对块号**（0..BLOCKS_PER_PLANE-1，十进制）。
 *   ⚠ 不是排内列号——列号每排重新计数（0..15），读图的人得自己算 `排号×列数+列号`，
 *     且 1~2 位的数字与 `FF` 并排极易被误读成十六进制字节值。
 *
 * ⚠ 本片芯片若已被整片擦过，出厂标记即已丢失 ⇒ 只会读到全 0xFF（不是 bug）。
 *
 * @param alloc     Allocator 上下文（取 ch/ce/lun 与块几何）
 * @param do_erase  0 = 只读（非破坏性）；非 0 = 逐块擦除并额外判擦除失败
 */
void bbt_print_bad_map(ftl_allocator_t *alloc, uint8_t do_erase);

/*
 * bbt_erase_all_blocks - 擦除本实例全部块（所有 plane），供开机自动格式化调用
 *
 * 逐块先读出厂坏块标记（列 2048，擦前读）再擦；擦除失败也判坏。新发现的坏块落进 BST。
 * ⚠ 必须在 bbt_init 之后调用（出厂标记此时已先读进 BST，擦除不丢坏块信息）；
 *   擦完由调用方 bbt_save() 重新持久化坏块图。
 *
 * @param alloc  Allocator 上下文（取 ch/ce 与块几何）
 * @return FTL_OK / FTL_ERROR（参数非法）
 */
ftl_status_t bbt_erase_all_blocks(ftl_allocator_t *alloc);

/*
 * bbt_mark_bad - 运行时标记坏块
 *
 * ① ftl_block_t.state 置 BAD + 清有效页位图（BST 单数据源）
 * ② bbt_flush() — 后台写回 NAND
 *
 * @param alloc  Allocator 上下文
 * @param gblock 坏 Block 编号（全局 Block 号）
 * @return 0 成功，-1 失败
 */
ftl_status_t bbt_mark_bad(ftl_allocator_t *alloc, uint16_t gblock);

/*
 * bbt_is_bad - 查询 Block 是否坏块
 *
 * @param bad_map  BBT bitmap
 * @param block    Block 编号
 * @return 1 坏块，0 好块
 */
static inline int bbt_is_bad(const uint8_t *bad_map, uint16_t block)
{
    if (bad_map == NULL)
    {
        return 0;
    }
    return (bad_map[block >> 3] >> (block & 7U)) & 1U;
}

/*
 * bbt_set_bad - 在 bitmap 中将 Block 标记为坏块
 *
 * @param bad_map  BBT bitmap
 * @param block    Block 编号
 */
static inline void bbt_set_bad(uint8_t *bad_map, uint16_t block)
{
    if (bad_map != NULL)
    {
        bad_map[block >> 3] |= (uint8_t)(1U << (block & 7U));
    }
}

/*
 * bbt_save - 将 BBT 追加写入 NAND BBT 池
 *
 * 池内下一槽追加一条 record（块满先擦池内下一块）。sequence 递增。
 *
 * @param alloc  Allocator 上下文
 * @return 0 成功，-1 失败
 */
ftl_status_t bbt_save(const ftl_allocator_t *alloc);

/*
 * bbt_flush - 将脏 BBT 写回 NAND（后台调用，两阶段持久化）
 *
 * 当 g_bbt_dirty 置位时，调用 bbt_save 追加一条 record 到 BBT 池。
 * 不需要每次写失败都立即写 BBT，后台攒批批量写入。
 *
 * @param alloc  Allocator 上下文
 * @return FTL_OK 成功（含无需写入），FTL_ERROR 写入失败
 */
ftl_status_t bbt_flush(ftl_allocator_t *alloc);

/*
 * bbt_load - 从 NAND BBT 池加载 BBT
 *
 * 池内轮转扫描（metadata_ring_locate），选 magic 正确且 sequence 最大者。
 * 命中时同时恢复本池的 sequence 计数与写游标。
 *
 * @param bad_map     输出：bt_bitmap[512]
 * @param bad_count   输出：坏块数
 * @param sequence    输出：最新 sequence
 * @param topo        拓扑信息（BBT 池所在芯片路由）
 * @return FTL_OK 成功，FTL_ERROR 无有效 BBT
 */
ftl_status_t bbt_load(uint8_t *bad_map, uint32_t *bad_count, uint32_t *sequence, ftl_topology_t topo);

/*
 * bbt_spare_pool_refill - 从 Spare Pool 补充 Free Pool
 *
 * PROGRAM/ERASE 失败标记坏块后，从 Spare Pool 取一个好块
 * 加入 alloc->free_blocks 保容量水位。
 *
 * @param alloc  Allocator 上下文
 * @return 0 补充成功，-1 Spare Pool 耗尽
 */
ftl_status_t bbt_spare_pool_refill(ftl_allocator_t *alloc);

#endif /* FTL_BBT_H */
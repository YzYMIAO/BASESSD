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
 * ftl/metadata.h 按几何推导（「固定元数据区布局」）。
 * bad_map 覆盖全部 FTL_BLOCK_TOTAL 个 Block（NAND 持久化格式，保持不变）。
 * RAM 运行时坏块位统一存于 ftl_block_t.state（Block State Table 单数据源），
 * bbt_save 时从 BST 重建 bad_map。
 *
 * 写模型与 Anchor / Checkpoint 一致（见 ftl/metadata.h §元数据写模型）。
 * ⚠ 旧实现把同一份 6 页（主 + 备各 3 页）**就地重写**：第二个坏块出现时 BBT 即
 *   损坏 ⇒ bbt_load 失败 ⇒ 开机不知道哪些块坏 ⇒ 用到坏块丢数据。
 */

#include "ftl/allocator.h"
#include "ftl/metadata.h" /* BBT_BLOCK / BBT_POOL_BLOCKS / BBT_MAX_BLOCKS / BBT_BITMAP_SIZE */

#include <stddef.h>
#include <stdint.h>

/* ── BBT 尺寸与块位置（BBT_BLOCK / BBT_POOL_BLOCKS / BBT_MAX_BLOCKS /
 *    BBT_BITMAP_SIZE）见 ftl/metadata.h「固定元数据区布局」── */

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
 * 从 NAND BBT 池加载 BBT，坏块位写入 alloc->blocks[].state。
 *
 * 无有效 BBT 时**不无条件出厂扫描**：
 *   · 介质是空片（固定元数据区没有一页是合法 FTL 页）⇒ 执行出厂扫描；
 *   · 介质已被格式化过 ⇒ **拒绝扫描** + LOGE，坏块账本从空开始（bad_count = 0、
 *     sequence 保持 0、不置 dirty ⇒ 不落盘）。真坏块由运行时 bbt_mark_bad 兜底。
 *
 * ⚠ 为什么：用过的块 Page 0 的 spare 首字节是 FTL 自己的数据（页编程 2112B 把 CW3
 *   尾巴 64 字节溢进备区），不是出厂标记 ⇒ 重扫会把所有写过的块（含活的 Anchor/
 *   Checkpoint/Wear）判成坏块并 bbt_save 落盘。详见 bbt.c 的 bbt_medium_has_ftl_data。
 *
 * @param alloc  Allocator 上下文（blocks、bad_count 等输出）
 * @return 0 成功（含"拒绝扫描"），-1 参数非法
 */
ftl_status_t bbt_init(ftl_allocator_t *alloc);

/*
 * bbt_scan_factory - 出厂坏块扫描
 *
 * 遍历所有 Block（**跳过 Block 0**）的 Page 0，读 **spare 区首字节**（列 2048，
 * `OP_READ_COLUMN`）的**出厂坏块标记**：
 *   - 标记非 0xFF → 出厂坏块
 *   - 标记 == 0xFF → 好块
 *   - 列读失败 → 坏块（保守）
 *
 * 判据与器件手册的建 BBT 流程一致（Micron AN1819：出厂标记在每块首页的 spare 区，
 * **擦除即永久丢失**，故必须在使用该块前先读）——见 `bbt.c:bbt_probe_block_bad` 的引文。
 * 本函数**只读不擦**（do_erase=0），擦除会毁掉标记。
 *
 * ⚠ **前置条件：介质必须是空片**。用过的块 spare 首字节是 FTL 自己的数据（页编程
 *   2112B 把 CW3 尾巴 64 字节溢进备区）⇒ 扫出来的不是坏块图，而是「哪些块被写过」图，
 *   且会 bbt_save 落盘。`bbt_init` 是唯一调用者，它用 bbt_medium_has_ftl_data 把关；
 *   其他调用方必须自己保证这一点。
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
 * 逐块**无条件**擦（含已判坏的块）；擦除失败 ⇒ 判坏并落进 BST。
 * **不读出厂坏块标记**——用过的块 spare 首字节是 FTL 数据，读它会把写过的块全判坏
 * （版本升级这条正常路径上就是灾难：连元数据块一起标坏 ⇒ 开不了机）。标记的读取职责
 * 唯一归 bbt_init（且只在空片时做）。
 * ⚠ 因此「必须在 bbt_init 之后调用」是**承重的**（出厂标记此时已在 BST 里）；
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
 * ② 只置 alloc->bbt.dirty —— 写回 NAND 由后台 bbt_kick 攒批执行
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
 * @param alloc  Allocator 上下文（游标/序号在 alloc->bbt，序 4 起 per-instance）
 * @return 0 成功，-1 失败
 */
ftl_status_t bbt_save(ftl_allocator_t *alloc);

/*
 * bbt_kick - 后台批量持久化入口（**提交即返回**，v0.75 阶段 7）
 *
 * 当 alloc->bbt.dirty 置位时，装配一个 BBT job 并提交第一条命令：
 *   换块需擦 ⇒ 异步擦（phase = BBT_JOB_ERASE）→ 擦完成再起写；
 *   否则直接异步写（phase = BBT_JOB_WRITE）。
 * 成败由 g_op_service[FTL_OP_BBT]（ftl_op_bbt）收尾：写完成后推进池游标、
 * 成功清 dirty、失败保留 dirty 供下轮重试。
 *
 * ⚠ 与旧 bbt_flush 的差别：旧版在**一次调用内**同步写完并返回成败；本函数只保证
 *   「已起步」。判据从返回值变成「job 走完且 dirty 清零」。
 * ⚠ **单飞行**：命令在飞 ⇒ 空操作（池内一条 record 一次写，重入会争同一槽）；
 *   若上一步提交被背压拒（job 停在 WRITE 但无命令在飞），本函数只**续提交写**，
 *   不重走换块判定——擦已成功，重擦纯属磨损。
 * ⚠ 只由 ftl_service 的后台档（idle/normal）调用。开机/格式化路径仍走同步 bbt_save。
 *
 * @param alloc  Allocator 上下文
 */
void bbt_kick(ftl_allocator_t *alloc);

/*
 * bbt_load - 从 NAND BBT 池加载 BBT
 *
 * 池内轮转扫描（metadata_ring_locate），选 magic 正确且 sequence 最大者。
 * 命中时同时恢复本实例的 sequence 计数与写游标（alloc->bbt）。
 *
 * @param alloc       Allocator 上下文（池路由取其 ch/ce/lun；游标/序号写回 alloc->bbt）
 * @param bad_map     输出：bt_bitmap[512]
 * @param bad_count   输出：坏块数
 * @param sequence    输出：最新 sequence
 * @return FTL_OK 成功，FTL_ERROR 无有效 BBT
 */
ftl_status_t bbt_load(ftl_allocator_t *alloc, uint8_t *bad_map, uint32_t *bad_count, uint32_t *sequence);

/*
 * bbt_spare_pool_refill - 从**指定 plane 的** Spare Pool 补充 Free Pool
 *
 * PROGRAM/ERASE 失败标记坏块后，从 Spare Pool 取一个好块
 * 加入 alloc->free_blocks 保容量水位。
 *
 * ⚠ **按 plane 取**（本次修复点）：Spare 池按 super block 粒度预留（每 plane 各一份），
 *   坏块属于哪个 plane 就必须补哪个 plane 的块——补错 plane 不恢复该 plane 的配对能力。
 *
 * @param alloc  Allocator 上下文
 * @param plane  坏块所属 plane（取同 plane 的替换块）
 * @return 0 补充成功，-1 Spare Pool 耗尽 / 参数非法
 */
ftl_status_t bbt_spare_pool_refill(ftl_allocator_t *alloc, uint8_t plane);

#endif /* FTL_BBT_H */
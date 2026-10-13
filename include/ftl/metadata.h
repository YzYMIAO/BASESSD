#ifndef FTL_METADATA_H
#define FTL_METADATA_H

/*
 * Metadata Directory — 动态元数据位置追踪
 *
 * 元数据和用户数据共享 Free Pool，通过 Metadata Directory 追踪每份元数据的
 * 当前物理位置。Directory SRAM 常驻（14.4KB = 2048 条 × 7B），启动时从 Anchor Block 加载。
 *
 * Anchor 池（固定元数据区内的 ANCHOR_POOL_BLOCKS 个块，出厂好块预留）：
 *   只存 Metadata Directory。一份 Directory 有 ANCHOR_PAGE_COUNT 页（chunk），
 *   每页自包含 Magic + Sequence + ChunkIndex + EntryCount + Entries；
 *   **一条 record = 一整份 Directory（ANCHOR_PAGE_COUNT 页）**，任一条可独立重建
 *   整个 Directory。池内块顺序轮转（见 §元数据写模型），最新有效 record 胜。
 *
 * 元数据（从 Free Pool 动态分配）:
 *   L2P Region       — 固定表区（哨兵，永不写入）+ 日志块（通过 Directory 查找）
 *   Journal          — 固定元数据区（块位置见 ftl/ftl_config.h）
 *   Checkpoint/BBT   — 固定元数据区（见 ftl/ftl_config.h）
 *
 * ── 元数据写模型（P0-2 统一；Anchor / Checkpoint / BBT / L2P 日志共用）──
 *  ① **只追加**：元数据页只写擦除态页。对已编程页二次编程 = 新数据 AND 旧数据
 *     = 静默损坏（NAND 只能 1→0）。就地覆盖一律禁止。
 *  ② **带 sequence**：每条 record 带单调递增 sequence；池内写序 == sequence 序。
 *  ③ **池内轮转**：写满一块 ⇒ 擦池内**下一块** ⇒ 从该块页 0 续写。
 *  ④ **最新有效胜**：读取 = 扫描池内所有 record，取「整条校验通过」中 sequence 最大者。
 *     整条校验 = 一条 record 的全部页有效且 sequence 一致（不取半条）。
 *
 * 崩溃安全：先擦后写 ⇒ 任意时刻「擦完未写」只让池里少一份，其余份仍可读；
 * 每条 record 是完整快照 ⇒ 擦掉最旧一份不损失任何信息。
 */

#include "ftl/allocator.h"
#include "ftl/ftl_config.h" /* ANCHOR_* 块位置 + 固定元数据区边界 */
#include "ftl/ftl_type.h"
#include "nand/nand_config.h"

#include <stdint.h>

/* ── Anchor 池位置与页几何（ANCHOR_BLOCK0、ANCHOR_POOL_BLOCKS、ANCHOR_ENTRIES_PER_PAGE、
 *    ANCHOR_PAGE_COUNT、ANCHOR_RECORDS_PER_BLOCK）见 ftl/ftl_config.h ── */

/* ── 元数据类型 ── */
#define METADATA_TYPE_L2P_REGION  (0U) /* L2P Region 数据 */
#define METADATA_TYPE_JOURNAL     (1U) /* Journal Block */
#define METADATA_TYPE_CHECKPOINT  (2U) /* Checkpoint */
#define METADATA_TYPE_BBT         (3U) /* Bad Block Table */
#define METADATA_TYPE_ERASE_COUNT (4U) /* 擦除计数表 */
#define METADATA_TYPE_COUNT       (5U)

/* ── Directory 容量 ──
 * = 每实例 L2P region 数：**每个 region 都必须能登记自己的位置**，否则
 * metadata_dir_update 的"满则丢弃"分支会把已落到动态块上的 region 位置丢掉
 * （重启后回落到固定位置 = 更新永久丢失）。容量取满即按构造消除该分支。*/
#define METADATA_DIR_ENTRIES          (L2P_REGIONS_PER_INSTANCE)
#define METADATA_DIR_ENTRIES_PER_PAGE (ANCHOR_ENTRIES_PER_PAGE)

/* ── 序列号锚定 ── */
#define ANCHOR_MAGIC (0x524F444EUL) /* "ANDIR" */

/* ── Entry: 7 字节（packed） ── */
typedef struct
{
    uint8_t    type;                        /* METADATA_TYPE_* */
    uint16_t   id;                          /* L2P region_id / 0 for others */
    nand_paa_t paa;                         /* 物理 NAND 位置 */
} __attribute__((packed)) metadata_entry_t; /* 7 bytes */

/* ── NAND 上持久化格式：**每页自包含** ──
 * Directory 有 METADATA_DIR_ENTRIES 条（14KB）⇒ 一份跨 ANCHOR_PAGE_COUNT 页，
 * 每页装 METADATA_DIR_ENTRIES_PER_PAGE 条，各自带 magic/sequence。
 * 单页损坏只丢该页（其余页仍可用），且读写只需一个 2KB 缓冲。
 * ⚠ 无本层 CRC——页完整性由下层 FC 的 per-CW CRC + ECC 保证（ADR v0.62）。*/
typedef struct
{
    uint32_t         magic;                                                                /* ANCHOR_MAGIC */
    uint32_t         sequence;                                                             /* 单调递增（同一份的各 chunk 相同）*/
    uint16_t         chunk_index;                                                          /* 0..ANCHOR_PAGE_COUNT-1 */
    uint16_t         chunk_count;                                                          /* = ANCHOR_PAGE_COUNT */
    uint32_t         entry_count;                                                          /* 本 chunk 有效 entry 数 */
    uint32_t         layout_version;                                                       /* FTL_MEDIA_LAYOUT_VERSION（版本不符 ⇒ 该页作废）*/
    metadata_entry_t entries[METADATA_DIR_ENTRIES_PER_PAGE];                               /* 256 × 7 = 1792B */
    uint8_t          padding[PAGE_MAIN_SIZE - 20U - (METADATA_DIR_ENTRIES_PER_PAGE * 7U)]; /* 填满一页主区 */
} metadata_anchor_page_t;

/* 一页必须装得下：头 20B + 每页条目（无独立 CRC，见 ADR v0.62）*/
_Static_assert(sizeof(metadata_anchor_page_t) == PAGE_MAIN_SIZE, "Anchor 页结构体不等于一页主区");
_Static_assert((METADATA_DIR_ENTRIES % METADATA_DIR_ENTRIES_PER_PAGE) == 0U, "每页条目数不能整除总条目数");

/* ══════════════════════════════════════════════════════════════════
 * 元数据 record 池（环形轮转）—— Anchor / Checkpoint / BBT 共用
 *
 * 池 = 连续 block_count 个**同 plane** 的物理块（per-plane 块号 first_block..）。
 * 每块容纳 PAGES_PER_BLOCK / pages_per_record 条 record；一条 record 占
 * pages_per_record 个**连续页**（页序 = 记录内序）。
 *
 * 全池共 total = block_count × records_per_block 条槽位，**按 ring 序**（块 0 的
 * 槽 0..R-1 → 块 1 的槽 0..R-1 → … → 块 N-1 的槽 R-1 → 回块 0）顺序写。
 * 写序 == sequence 序。
 *
 * ⚠ block_count >= 2 是硬要求：写点换块时要先擦目标块，若池只有 1 块，
 *   擦的正是唯一持有效 record 的块 ⇒ 全丢。
 * ══════════════════════════════════════════════════════════════════ */
typedef struct
{
    uint16_t first_block;      /* 池首块（per-plane 块号）*/
    uint16_t block_count;      /* 池块数（>= 2）*/
    uint8_t  pages_per_record; /* 一条 record 占几页（须整除 PAGES_PER_BLOCK）*/
    uint8_t  plane;            /* 池所在 plane */
    uint8_t  module;           /* FC_COMMAND_MODULE_*（读写页配置的诊断标签）*/
} metadata_ring_t;

/* 每块容纳的 record 数 */
static inline uint16_t metadata_ring_records_per_block(const metadata_ring_t *ring)
{
    return (uint16_t)(PAGES_PER_BLOCK / ring->pages_per_record);
}

/* 池内 record 槽位总数 */
static inline uint16_t metadata_ring_total_records(const metadata_ring_t *ring)
{
    return (uint16_t)(ring->block_count * metadata_ring_records_per_block(ring));
}

/* 各池槽位数必须装得进 uint16_t（写游标 / record 下标都是 16 位）*/
_Static_assert((ANCHOR_POOL_BLOCKS * ANCHOR_RECORDS_PER_BLOCK) <= UINT16_MAX, "Anchor 池槽位数超出 uint16_t");
_Static_assert((CHECKPOINT_POOL_BLOCKS * PAGES_PER_BLOCK) <= UINT16_MAX, "Checkpoint 池槽位数超出 uint16_t");
_Static_assert((BBT_POOL_BLOCKS * PAGES_PER_BLOCK) <= UINT16_MAX, "BBT 池槽位数超出 uint16_t");

/*
 * record 槽位 → 芯片地址
 *
 * record_idx ∈ [0, total)；page_in_record ∈ [0, pages_per_record)。
 */
nand_addr_t metadata_ring_addr(const metadata_ring_t *ring,
                               const ftl_topology_t  *topo,
                               uint16_t               record_idx,
                               uint8_t                page_in_record);

/*
 * metadata_ring_probe_fn - 调用方的 record 探针（由 ring 模块回调）
 *
 * full == 0：**廉价探针** —— 只读 record 的第 0 页取 sequence（用于全池扫一遍找
 *            最大 sequence；每条只花 1 次页读）。
 * full != 0：**整条校验** —— 校验一条 record 的**全部**页（页内 magic、
 *            各页 sequence 一致、chunk 序完整），通过才给 sequence。
 *
 * @param ctx           调用方上下文
 * @param record_idx    槽位号
 * @param full          0 = 廉价探针，非 0 = 整条校验
 * @param out_sequence  输出：该 record 的 sequence
 * @return FTL_OK = 该 record 在本次探针强度下有效
 *
 * ⚠ 契约：**失败时不写 *out_sequence**（调用者据此把"探针失败"与"sequence=0"分开）。
 *   写侧用 sequence 从 1 起，故 0 恒表示"未写"。
 */
typedef ftl_status_t (*metadata_ring_probe_fn)(void     *ctx,
                                               uint16_t  record_idx,
                                               int       full,
                                               uint32_t *out_sequence);

/*
 * metadata_ring_locate - 定位池内最新有效 record 与下一个可写槽
 *
 * 算法（利用「record 在块内页对齐 + 池内顺序写」两个不变量，只读 O(块数) 页）：
 *   ① 逐块读该块**末条** record 的廉价探针 → 得每块末条的 sequence（或"该块未写满"）；
 *   ② 第一个末条无效的块 = **游标块**；若所有块末条都有效（池写满一圈）
 *      ⇒ 游标块 = (sequence 最大的块 + 1) mod block_count；
 *   ③ 游标块内降序找最后一条有效 record = 游标块末条；
 *   ④ 写点 = 游标块末条的下一条（末条是块内最后一条时跨到下一块槽 0）；
 *      游标块内无有效 record ⇒ 写点 = 游标块的槽 0；
 *   ⑤ 候选 = {全部块中 sequence 最大的末条, 游标块末条}，按 sequence 降序做**整条
 *      校验**，返回第一个通过者。
 *   ⑥ 快路径候选整条校验全失败（局部损坏）⇒ 慢路径：全池廉价探针取 sequence 最大者
 *      整条校验，失败则排除该 sequence 再找次新。**只在损坏时**付出全池代价。
 *
 * 快路径代价 = block_count 次页读 + 游标块内至多 records_per_block 次页读。
 *
 * @param ring           池几何
 * @param topo           ch/ce/lun
 * @param probe          调用方探针
 * @param ctx            探针上下文
 * @param out_newest_idx 输出：最新有效 record 槽位；**池内无有效 record 时为 UINT16_MAX**
 * @param out_sequence   输出：该 record 的 sequence；**池内无有效 record 时为 0**
 * @param out_write_idx  输出：下一个可写槽位
 * @return FTL_OK = 定位完成（含"池为空"）；FTL_ERROR = 参数非法
 */
ftl_status_t metadata_ring_locate(const metadata_ring_t *ring,
                                  const ftl_topology_t  *topo,
                                  metadata_ring_probe_fn probe,
                                  void                  *ctx,
                                  uint16_t              *out_newest_idx,
                                  uint32_t              *out_sequence,
                                  uint16_t              *out_write_idx);

/*
 * metadata_ring_prepare_write - 把写点槽位准备好，返回该 record 第 0 页地址
 *
 * 写点是块内槽 0 时**先擦该块**（池内轮转：该块持的是全池最旧的 record，且每条
 * record 都是完整快照 ⇒ 擦掉不丢信息）。调用方随后从返回地址起顺序写
 * pages_per_record 页。
 *
 * @param ring      池几何
 * @param topo      ch/ce/lun
 * @param write_idx 写点槽位（来自 metadata_ring_locate 或上次写点 + 1）
 * @param out_addr  输出：record 第 0 页地址
 * @return FTL_OK / FTL_ERROR（擦除失败）
 */
ftl_status_t metadata_ring_prepare_write(const metadata_ring_t *ring,
                                         const ftl_topology_t  *topo,
                                         uint16_t               write_idx,
                                         nand_addr_t           *out_addr);

/* ── SRAM 常驻 Directory Cache（约 18KB，随 ftl_context_t 落 AXI_RAM） ── */
typedef struct
{
    metadata_entry_t entries[METADATA_DIR_ENTRIES];
    uint16_t         count;           /* 有效 entry 数 */
    uint32_t         anchor_sequence; /* 当前 Anchor sequence */
    uint8_t          dirty;           /* 是否有未写回 Anchor 的变更 */
    uint32_t         change_count;    /* 自上次 Anchor flush 后的变更数（仅节流用）*/

    /* 是否已有 region 落到动态块（block >= FTL_METADATA_BLOCK_END）。
     * entries 现在有 2048 条，而 ftl_is_metadata_block() 在开机回填里按块调用
     * 数千次 ⇒ 无条件线性扫全表会明显拖慢开机。为 0 时直接跳过动态扫描。 */
    uint8_t have_dynamic;

    /* ── L2P 日志块（元数据写模型：只追加 + 块池轮转）──
     * 每个 region 写进「当前日志块」的下一空槽（一 region = 一页）。块满则从
     * Free Pool 取新块（Free Pool 保证已擦除）。l2p_live 记每个日志块上还有多少
     * region 的**最新位置**落在这里 —— 归零即可擦除归还（metadata_dir_reclaim）。 */
    uint16_t l2p_live[FTL_BLOCK_TOTAL]; /* 每 gblock 的活 region 数 */
    uint16_t cur_metadata_block;        /* 当前日志块 gblock（cur_block_valid=0 时无意义）*/
    uint8_t  cur_metadata_page;         /* 当前日志块内下一可用页 */
    uint8_t  cur_block_valid;           /* 游标是否已建立（0 = 尚未分配日志块）*/
    uint8_t  cur_metadata_plane;        /* 下一个新日志块选哪个 plane（轮转）*/

    /* 压缩进行中（P0-3）。两个作用，缺一不可：
     *   ① **抑制逐次立即回收**——搬移 victim 的最后一个活 region 时 l2p_live[victim]
     *      归零，metadata_dir_commit_region 的"归零即擦"分支会**在 Anchor 落盘之前**
     *      把 victim 擦掉（掉电后 Anchor 仍指向已擦块 ⇒ 那些 region 的映射永久丢失）；
     *   ② **防重入**——搬移走 metadata_dir_append_slot，换块分支会再触发一次压缩。
     * 非 0 时 append_slot 的换块分支直接失败让路（见 metadata_dir_compact 的 ③④ 顺序）。*/
    uint8_t compacting;

    /* Anchor 池游标（ring 写点槽位；anchor_cursor_valid=0 时尚未定位）*/
    uint16_t anchor_write_idx;
    uint8_t  anchor_cursor_valid;

    /* 拓扑信息（元数据所在芯片路由） */
    ftl_topology_t topo;
} metadata_dir_cache_t;

/* ── API ── */

/*
 * metadata_dir_init - 初始化 Metadata Directory
 *
 * 首次使用时建立固定的 L2P Region 映射（哨兵层：永不写入，读回 0xFF ⇒ 未映射；
 * 打包放置：Block/Page/Plane 由 ftl_l2p_region_placement() 给出，见 ftl_config.h）。
 *
 * 游标全部置**无效**（cur_block_valid = 0 / anchor_cursor_valid = 0）：首次追加
 * 才从 Free Pool 取块，避免"没拿块就用"。
 *
 * @param dir  Directory cache（由调用者提供存储）
 */
void metadata_dir_init(metadata_dir_cache_t *dir);

/*
 * metadata_dir_lookup - 查找元数据物理位置
 *
 * @param dir  Directory cache
 * @param type 元数据类型
 * @param id   L2P region_id / 0 for others
 * @return PAA，0xFFFFFFFF 表示未找到
 */
nand_paa_t metadata_dir_lookup(const metadata_dir_cache_t *dir,
                               uint8_t                     type,
                               uint16_t                    id);

/*
 * metadata_dir_update - 添加或更新元数据位置
 *
 * @param dir    Directory cache
 * @param type   元数据类型
 * @param id     L2P region_id / 0 for others
 * @param new_paa 新物理位置
 */
void metadata_dir_update(metadata_dir_cache_t *dir,
                         uint8_t               type,
                         uint16_t              id,
                         nand_paa_t            new_paa);

/*
 * metadata_dir_append_slot - 取当前日志块的下一个空槽（L2P region 的写点）
 *
 * **只取槽，不登记**：调用方先往返回的地址写 region 页，写成功后再调
 * metadata_dir_commit_region 登记（写失败则位置不变，槽被跳过——已编程页不可复用）。
 *
 * 块满（或尚无日志块）⇒ 先 metadata_dir_reclaim 回收无活 region 的日志块，再从
 * Free Pool 取**整块**新日志块（allocator_alloc_reserved_block；Free Pool 保证
 * 已擦除，故无需擦）。**绝不回退到固定位置**：分配失败返回 FTL_ERROR，由上层重试。
 *
 * 日志块在哪个 plane 由 dir->cur_metadata_plane 轮转决定（写入目标与 region 归属
 * 无关：位置由 paa 自身携带，读回一律经 Directory 查 paa）。
 *
 * @param dir    Directory cache
 * @param alloc  Allocator（取 Free Pool 块）
 * @return 写点 PAA；失败返回 NAND_PBA_INVALID
 */
nand_paa_t metadata_dir_append_slot(metadata_dir_cache_t *dir,
                                    ftl_allocator_t      *alloc);

/*
 * metadata_dir_commit_region - 写成功后登记 region 的新位置
 *
 * 更新 entries[region_id].paa + l2p_live（旧日志块 -1、新日志块 +1）+ dirty +
 * have_dynamic。旧块活计数归零且非当前块 ⇒ 立即回收（擦除 + 归还 Free Pool），
 * 把「已无引用却仍带旧内容」的窗口压到最短。
 *
 * @param dir       Directory cache
 * @param alloc     Allocator（回收时需要；可为 NULL = 只登记不回收）
 * @param region_id L2P region_id
 * @param new_paa   刚写成功的位置
 */
void metadata_dir_commit_region(metadata_dir_cache_t *dir,
                                ftl_allocator_t      *alloc,
                                uint16_t              region_id,
                                nand_paa_t            new_paa);

/*
 * metadata_dir_reclaim - 回收无活 region 的 L2P 日志块
 *
 * 遍历全部 gblock：l2p_live == 0、非当前日志块、且 BST 带 RESERVED 位（= 确实是
 * 本池独占的日志块，而非恰好活计数为 0 的用户块）⇒ 擦块 + allocator_free_reserved_block。
 *
 * 没有它会在稳态泄漏：每填满一块就永久占一块。
 *
 * ⚠ 本函数**只**回收 live == 0 的块。若某日志块混着永不再写的冷 region（如 63 冷 + 1 热），
 *   它永不归零 ⇒ 永久占块；最坏情况吃掉近半个 die（FTL_BLOCK_TOTAL/2 块）。
 *   抗碎片的「取 live 最小块做 victim、搬活 region 后擦」是 metadata_dir_compact。
 *
 * @param dir   Directory cache
 * @param alloc Allocator
 * @return FTL_OK（含无可回收块）/ FTL_ERROR（参数非法）
 */
ftl_status_t metadata_dir_reclaim(metadata_dir_cache_t *dir, ftl_allocator_t *alloc);

/*
 * metadata_dir_compact - 抗碎片压缩：搬空一个「多活」日志块后归还（P0-3）
 *
 * metadata_dir_reclaim 的补集：挑 l2p_live 最小（但 >0）的非当前日志块做 victim，
 * 把它上面**仍有引用**的 region 逐个读回、写进当前日志块的下一空槽并重新登记，
 * 最后落盘 Anchor、擦除 victim、归还 Free Pool。
 *
 * 掉电安全顺序（不可重排）：① 搬移 → ② 登记 → ③ flush_to_anchor → ④ 擦 victim → ⑤ 归还。
 * ③ 必须早于 ④——否则掉电后 Anchor 仍指向已擦块，那些 region 的映射永久丢失。
 *
 * ⚠ 调用时机受限：必须在**当前日志块有足够空槽**时调（本函数自己会核对
 *   `l2p_live[victim] <= PAGES_PER_BLOCK - cur_metadata_page`）。最自然的调用点是
 *   metadata_dir_append_slot 刚取到新块之后——此时块内有整 PAGES_PER_BLOCK 个空槽。
 * ⚠ 无候选（全池零活 / 只剩当前块）/ 读失败 / 写失败 / Anchor 落盘失败 ⇒ 返回
 *   FTL_ERROR 且**不擦 victim**（已搬 region 的新位置已登记 ⇒ 无信息丢失，下次再来）。
 *
 * @param dir   Directory cache
 * @param alloc Allocator（归还 victim 时需要）
 * @return FTL_OK = 确实压缩并归还了一块；FTL_ERROR = 未做（原因见上）
 */
ftl_status_t metadata_dir_compact(metadata_dir_cache_t *dir, ftl_allocator_t *alloc);

/*
 * metadata_dir_flush_to_anchor - 将 Directory 追加为 Anchor 池的一条新 record
 *
 * 变更 ≥ 64 次或 Checkpoint/ftl_service(IDLE) 时调用。
 * **按页写**：一条 record = ANCHOR_PAGE_COUNT 页，每页在一个 2KB 栈缓冲里组好后
 * 同步写入（勿在栈上放整份 Directory）。池内轮转由 metadata_ring_prepare_write 负责。
 *
 * 无变更（!dirty）直接返回 FTL_OK —— 保留的早退：Anchor 每写一条都消耗池内槽位，
 * 空转会让池无谓轮转。
 *
 * @param dir  Directory cache
 * @return FTL_OK（含无需写入）/ FTL_ERROR（擦除或写失败）
 */
ftl_status_t metadata_dir_flush_to_anchor(metadata_dir_cache_t *dir);

/*
 * metadata_dir_load - 从 Anchor 池加载 Directory
 *
 * ① 先 fill_fixed 铺基础层（全部 region 的固定位置 = 哨兵层，读回即"未映射"）；
 * ② metadata_ring_locate 定位池内最新**整条校验通过**的 record；
 * ③ 读胜者各页装配覆盖对应区间（逐页再校验 chunk_index/sequence）；
 * ④ 由装配后的 entries 重建 l2p_live / have_dynamic。
 *
 * **游标不跨重启恢复**：cur_block_valid 置 0，首次追加时从 Free Pool 新取一块
 * （绝不复用可能已编程的旧页）。旧日志块仍被 Directory 引用 ⇒ 由 l2p_live 保住，
 * 待其 region 全部改写后由 metadata_dir_reclaim 归还。
 *
 * @param dir  Directory cache（输出）
 * @return FTL_OK / FTL_ERROR（无有效 Anchor：调用者转 metadata_dir_init）
 */
ftl_status_t metadata_dir_load(metadata_dir_cache_t *dir);

/*
 * ftl_is_metadata_block - 判断物理 Block 是否属于固定/动态元数据区
 *
 * 掉电恢复重建 closed_sb_list 时，L2P Table（固定表区 + 动态 region）、
 * Anchor、BBT、Checkpoint、Journal、Wear 等元数据块可能写满全部页（尤其
 * Journal 环形回切），逐页判空会把它们误判为满 Super Block。本谓词统一排除：
 *
 *   固定块：遍历 g_ftl_partitions[]（单一事实来源，见 ftl/ftl_config.h），
 *           按 first_block/block_count 判区间（各 die 同一份块号）
 *   日志块：Metadata Directory 中登记的 L2P region 块（扫 entries）
 *   游标块：当前正在追加的 L2P 日志块（可能尚无任何 region 指向它，entries 扫不到）
 *
 * free pool 回填（main.c）与重建（ftl_rebuild_closed_sb_list）共用同一判据。
 *
 * ⚠ 现存不精确处（已登记、本次不修）：本函数按 **per-plane** 块号比较，会连另一个
 *   plane 的同号块一起排除——偏保守，安全（代价是 Anchor/Checkpoint 池在另一 plane
 *   的同号块被闲置）。
 *
 * @param md_dir Metadata Directory cache（可为 NULL：仅固定块区间判断）
 * @param block  物理 Block 号（per-plane 号，0..BLOCKS_PER_PLANE-1）
 * @return 1 = 元数据块，0 = 用户数据块
 */
int ftl_is_metadata_block(const metadata_dir_cache_t *md_dir, uint16_t block);

#endif /* FTL_METADATA_H */
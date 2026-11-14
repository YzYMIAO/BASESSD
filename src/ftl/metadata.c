/*
 * metadata.c — 元数据落盘基建：record 池环形轮转 + 目录检查点池 + L2P 日志块
 *
 * 职责：
 *   提供 FTL 全部持久化元数据（目录检查点 / Checkpoint / BBT / L2P region）共用的
 *   落盘原语：环形 record 池的寻址/定位/换块擦除；目录检查点（Metadata Directory 快照）
 *   的组页/落盘/加载；L2P 日志块的追加槽/登记；以及"这块是不是元数据块"的判定。
 *   日志块的**回收与压缩**状态机在 metadata_job.c（本文件只负责登记待回收位图）。
 *
 * 与 metadata.h 的分工：
 *   metadata.h —— 结构（md_ring_s / md_dir_ckpt_page_s / md_dir_cache_s）+
 *                 固定元数据区布局（池尺寸 / 块偏移 / 表字节 / 分区表声明）；
 *   本文件     —— 上述结构的全部算法实现 + 分区表本体（单一事实来源）。
 *
 * L2P region 的位置只有一层来源（见 metadata.h「固定元数据区布局」）：
 *   Directory 条目 = `NAND_PBA_INVALID` ⇒ region 从未写过（未映射，读取不碰 NAND）；
 *   非 INVALID ⇒ region 页在日志块上的真实 PBA（从固定 L2P 日志区取块、追加写）。
 *
 * 文件结构：
 *   〇、文件级类型与常量（探针上下文 / 池策略表 / 固定元数据分区表）
 *   一、元数据 record 池（环形轮转）—— 目录检查点 / Checkpoint / BBT 共用
 *   二、目录检查点池（Metadata Directory 的 record 池）
 *   三、Directory 基础层 / 查询 / 更新
 *   四、L2P 日志块：追加槽 / 登记
 *   五、目录检查点落盘 / 加载
 *   六、元数据块判定
 *   七、池诊断与服务
 *
 * ⚠ 同步 / 异步的分界：
 *   · **异步**：日志块的回收与压缩（`md_handle_s` + g_op_callbacks[FTL_OP_METADATA]，
 *      状态机在 metadata_job.c）。待回收块由 commit_region / append_slot 登记进
 *      `reclaim_pending` 位图，运行期没有任何同步擦除。
 *   · **同步（刻意）**：开机加载 / 格式化（md_load / init）、目录检查点池定位
 *      （md_ring_locate）——前者在 superloop 之前跑（无泵可等），后者只在
 *      冷启动后首次整理出现。
 *
 * 断言口径：init/bind 入口与 release 关键边界用 ASSERT；运行期模块入口的实例句柄/
 * 出参由调用链保证，不再逐次复检；ring 几何非法、槽位越界等配置错误在 release 也
 * 必须拒绝，因此保留 FTL_ERROR 返回。
 */

#include "metadata.h"

#include "assertion.h"
#include "bbt.h"
#include "boot_policy.h" /* boot_crc32：拓扑 fingerprint（复用已有 CRC32） */
#include "checkpoint.h"
#include "fc.h"
#include "ftl.h" /* ftl_ctx_s（服务函数要从实例取 &ftl->alloc） */
#include "ftl_fc_interface.h"
#include "ftl_op.h" /* ftl_op_submit / FTL_OP_METADATA */
#include "journal.h"
#include "log.h"
#include "metadata_job.h" /* slot_now / job_reset / 待回收位图（与 metadata_job.c 共用） */
#include "onfi_1_0.h"
#include "ssd_compiler.h" /* SSD_CODE_FAST：本节函数级放置到 Flash */

#include <stddef.h>
#include <string.h>

/**
 * @brief 计算当前介质布局版本
 *
 * 单 die = BASE；多 die 把 die 数与 topology map 编入版本，拓扑变化 ⇒ 版本不符
 * ⇒ 格式化。用 boot_crc32 哈希字段字节（不哈希 padding）。
 *
 * @return 介质布局版本
 */
DWORD ftl_media_layout_version(void)
{
    if (g_die_count <= 1U)
    {
        return FTL_MEDIA_LAYOUT_VERSION_BASE;
    }

    /* [count][ch0][ce0][lun0][ch1]... —— 只含已分配 die。 */
    BYTE   topo[1U + (FTL_DIE_MAX * 3U)];
    size_t used = 1U;

    topo[0] = g_die_count;

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        topo[used++] = g_die_table[die].ch;
        topo[used++] = g_die_table[die].ce;
        topo[used++] = g_die_table[die].lun;
    }

    const DWORD fingerprint = boot_crc32(topo, used);

    /* bit31 固定为 1：多 die 版本不可能与单 die 的 BASE 小版本号相撞。
     * 有效哈希 31 位，碰撞概率 2^-31。 */
    return 0x80000000UL | (fingerprint & 0x7FFFFFFFUL);
}

// 每实例 Metadata Directory cache（CPU-only）。链接脚本按整个 metadata.o 的 .bss
// 放进 SRAM4（与 BCH 交换：BCH 在 DTCM）；`.sram4_data` 是 NOLOAD ⇒ ftl_init
// 绑定后对该实例显式 memset。
md_dir_cache_s g_ftl_md[FTL_DIE_MAX];

// 候选池重建日志节流：第 1 次立即打印，此后每 N 次一条（元数据坏块风暴不刷屏）。
// N=16：重建本身较贵且罕见，16 条足以覆盖一次故障风暴。
// ⚠ 位于 DTCM NOLOAD ⇒ 必须在 md_init 显式复位。
#define MD_POOL_REBUILT_LOG_EVERY (16U)
static DWORD s_pool_rebuilt_logged;

/* ══════════════════════════════════════════════════════════════════
 * 文件级类型与常量
 * ══════════════════════════════════════════════════════════════════ */

// 目录检查点池探针上下文：record 扫描同时需要 Directory 与池几何。
typedef struct DIRECTORY_CKPT_PROBE_CONTEXT_S
{
    const md_dir_cache_s *dir;
    const md_ring_s      *ring;
} dir_ckpt_probe_ctx_s;

// 单个固定池的几何/补齐策略（池内容在 md_pool_build_all 里构建）。
typedef struct METADATA_POOL_SPEC_S
{
    WORD first;
    WORD nominal;
    BYTE pages_per_record;
    BYTE plane_mask;
    WORD min_blocks;
    WORD spare_budget;
} md_pool_spec_s;

// 固定池的行号（表下标）。
typedef enum METADATA_POOL_ID_E
{
    MD_POOL_BBT = 0,
    MD_POOL_CP,
    MD_POOL_JRN,
    MD_POOL_DIR_CKPT,
    MD_POOL_NUM,
} md_pool_id_e;

// 固定池的策略（几何从 s_md_partitions 派生；这里只留 min/spare/pages 策略）。
typedef struct METADATA_POOL_POLICY_S
{
    BYTE part;             /* s_md_partitions 行号 */
    WORD min_blocks;       /* 少于此数即不可用 */
    WORD spare_budget;     /* 可从 shared spare 补的最大块数 */
    BYTE pages_per_record; /* 一条 record 占几页 */
} md_pool_policy_s;

// 一次池扫描的中间结果。
//
// 语义：不再假设"第一个末槽无效的块 = 写 frontier"。引导环
// （BBT 名义块 + shared spare）里 BAD 块/读失败块的末槽同样无效，旧假设会把
// 真正 frontier 之后的中段 record 全部忽略。现在对**每个**非满块降序找最后
// 一条有效 record，再与满块末条比较取全局 sequence 最大者。
typedef struct METADATA_RING_SCAN_S
{
    WORD  cursor_block; /* 全空池：第一个空块（写点从它的槽 0 开始）；否则 = frontier 所在块 */
    WORD  frontier_idx; /* 池内 sequence 最大的 record 槽位（UINT16_MAX = 无有效 record）*/
    DWORD frontier_seq; /* 该 record 的 sequence */
} md_ring_scan_s;

// md_ring_scan 用 DWORD 位图记录"末槽无效"的块 ⇒ 块数必须装进 32 位。
_Static_assert(MD_RING_MAX_BLOCKS <= 32U, "open_blocks 位图按 32 块设计");

// 分区表行号：s_md_partitions 与 pool policy 共用同一套下标。
typedef enum METADATA_PART_ID_E
{
    MD_PART_DIR = 0,
    MD_PART_CP,
    MD_PART_BBT,
    MD_PART_JRN,
    MD_PART_L2P,
    MD_PART_NUM,
} md_part_id_e;

// 固定元数据分区表 —— 单一事实来源（类型声明在 metadata.h）
//
// 区间为 [first_block, first_block + block_count)；**各 (ch,ce,lun) 用同一份块号**
// （分片模型：每个 die 一套完整元数据，分区不随维度错开）。
//
// 目录检查点 / Checkpoint / BBT 都是**多块轮转池**：池内每块容纳 `records_per_block`
// 条 record，块满换池内下一块（先擦后写）。
//
// 每项完全由 metadata.h 的宏派生——块号 / 块数 / 每块 record 数 / 表字节均无手写数字。
static const md_partition_s s_md_partitions[] = {
  // name, first_block, block_count, plane_mask, redundancy, table_bytes
    {"目录检查点",   DIR_CKPT_BLOCK0,     DIR_CKPT_POOL_BLOCKS, METADATA_MIRROR_PLANE_MASK,       DIR_CKPT_RECORDS_PER_BLOCK,                     FTL_DIR_CKPT_TABLE_BYTES},
    {     "Checkpoint", CHECKPOINT_BLOCK0,   CHECKPOINT_POOL_BLOCKS, METADATA_MIRROR_PLANE_MASK, FTL_CHECKPOINT_RECORDS_PER_BLOCK,                   FTL_CHECKPOINT_TABLE_BYTES},
    {            "BBT",         BBT_BLOCK,          BBT_POOL_BLOCKS, METADATA_MIRROR_PLANE_MASK,                  PAGES_PER_BLOCK,                          FTL_BBT_TABLE_BYTES},
    {        "Journal",    JOURNAL_BLOCK0,      JOURNAL_BLOCK_COUNT, METADATA_MIRROR_PLANE_MASK,                               1U,                      FTL_JOURNAL_TABLE_BYTES},
 // L2P region 日志区：固定预留（/CAP-02）。redundancy 填 1（不是 record 池）；
  // table_bytes = region 数 × 每个 region 的 2 KB。
    {        "L2P log",    L2P_LOG_BLOCK0, L2P_LOG_BLOCKS_PER_PLANE, METADATA_MIRROR_PLANE_MASK,                               1U, (L2P_REGIONS_PER_INSTANCE * L2P_REGION_SIZE)},
};

// 分区数（表长）。sizeof 派生 ⇒ 加/删分区项时自动跟随。
static const DWORD s_md_partition_count = (sizeof(s_md_partitions) / sizeof(s_md_partitions[0]));

_Static_assert((sizeof(s_md_partitions) / sizeof(s_md_partitions[0])) == (size_t)MD_PART_NUM, "s_md_partitions rows must match md_part_id_e");

// 固定池策略：几何字段全部从 s_md_partitions 取，避免表间重复。
static const md_pool_policy_s s_pool_policy[MD_POOL_NUM] = {
    [MD_POOL_BBT]      = {MD_PART_BBT,                  2U, 2U,                          1U},
    [MD_POOL_CP]       = { MD_PART_CP,                  2U, 1U, FTL_CHECKPOINT_RECORD_PAGES},
    [MD_POOL_JRN]      = {MD_PART_JRN, JOURNAL_BLOCK_COUNT, 2U,                          1U},
    [MD_POOL_DIR_CKPT] = {MD_PART_DIR,                  2U, 0U,         DIR_CKPT_PAGE_COUNT},
};

/**
 * @brief 由分区表 + 策略生成一个池的收集参数
 *
 * @param id 池 id
 * @return md_pool_spec_s
 */
static md_pool_spec_s md_pool_spec_make(BYTE id)
{
    const md_pool_policy_s *pol  = &s_pool_policy[id];
    const md_partition_s   *part = &s_md_partitions[pol->part];
    md_pool_spec_s          spec;

    spec.first            = part->first_block;
    spec.nominal          = part->block_count;
    spec.pages_per_record = pol->pages_per_record;
    spec.plane_mask       = part->plane_mask;
    spec.min_blocks       = pol->min_blocks;
    spec.spare_budget     = pol->spare_budget;
    return spec;
}

/* ══════════════════════════════════════════════════════════════════
 * 一、元数据 record 池（环形轮转）—— 目录检查点 / Checkpoint / BBT 共用
 * ══════════════════════════════════════════════════════════════════ */

/**
 * @brief ring 几何自检：任何一条不满足都会让地址公式除零或产生越界槽位。
 * 这是**配置错误** ⇒ 调用者在 release 也必须拿到 FTL_ERROR（判据 9）。
 * 不要求 PAGES_PER_BLOCK % pages_per_record == 0：Checkpoint 的 21 页/record
 * 刻意向下取整，块尾剩余页不用。
 *
 * @param ring 待校验的池几何
 * @return true = 几何合法；false = 非法
 */
static BOOL md_ring_geom_valid(const md_ring_s *ring)
{
    const BYTE valid_plane_bits = ((1U << PLANE_NUM) - 1U);

    if ((ring->block_count < 2U) || (ring->pages_per_record == 0U) || (ring->pages_per_record > PAGES_PER_BLOCK))
    {
        return false;
    }
    if ((ring->plane_mask == 0U) || ((ring->plane_mask & ~valid_plane_bits) != 0U))
    {
        return false; /* 必须至少用一个 plane，且不得指向不存在的 plane */
    }

    const WORD rpb = (PAGES_PER_BLOCK / ring->pages_per_record);

    if ((ring->block_count * rpb) > UINT16_MAX)
    {
        return false; /* 槽位总数必须装进 WORD（写游标 / record 下标都是 16 位）*/
    }
    if ((ring->block_count > MD_RING_MAX_BLOCKS) || (ring->nominal_count > MD_RING_MAX_BLOCKS))
    {
        return false; /* 候选表容量 */
    }
    for (WORD i = 0U; i < ring->block_count; i++)
    {
        if (ring->blocks[i] >= BLOCKS_PER_PLANE)
        {
            return false; /* 候选块号越出本 plane */
        }
    }

    return true;
}

/**
 * @brief 唯一地址公式：record 槽位 + plane → PBA
 *
 * 候选池变化会让同一 record_idx 落到不同物理块，定位不依赖固定块号。
 *
 * @param ring           池几何
 * @param topo           ch/ce/lun
 * @param record_idx     池内槽位
 * @param page_in_record record 内页序
 * @param plane          目标 plane
 * @return 该槽位的 PBA；几何/槽位非法返回 NAND_PBA_INVALID
 */
SSD_CODE_FAST DWORD md_ring_addr_plane(const md_ring_s *ring, const ftl_topo_s *topo, WORD record_idx, BYTE page_in_record, BYTE plane)
{
    if (md_ring_geom_valid(ring) == false)
    {
        return NAND_PBA_INVALID; /* 配置错误：不产生"看起来合法"的错误地址 */
    }

    const WORD rpb = md_ring_records_per_block(ring);

    if (record_idx >= (ring->block_count * rpb))
    {
        return NAND_PBA_INVALID; /* 槽位越界：release 也必须拒绝（判据 9）*/
    }

    const DWORD record_in_block = (DWORD)(record_idx % rpb);
    const DWORD block           = ring->blocks[record_idx / rpb];
    const DWORD page            = (record_in_block * ring->pages_per_record) + page_in_record;

    // 页内序号来自调用者的循环上界、plane 来自 plane_mask：两者都是内部调用者自己
    // 算出的索引 ⇒ 越界只能是编程错误，断言（H2 / H34），不做运行期返回。
    // 越界后 pba_make 的位掩码会 alias 到别的页/plane，debug 档必须停在现场。
    // `page` 不单独判：record_in_block < PAGES_PER_BLOCK / pages_per_record 且
    // page_in_record < pages_per_record ⇒ page < PAGES_PER_BLOCK 恒成立。
    ASSERT(page_in_record < ring->pages_per_record);
    ASSERT(plane < PLANE_NUM);

    return pba_make(topo->ch, topo->ce, topo->lun, block, plane, page, 0U);
}

/* ══════════════════════════════════════════════════════════════════
 * 固定元数据分区的候选块池（BBT + shared spare）
 * ══════════════════════════════════════════════════════════════════ */

/**
 * @brief 物理块在 plane_mask 的所有 plane 上都非 BAD。
 * 镜象/多 plane 擦写要求**同一物理块号**在每个 plane 上都可用，否则
 * ftl_async_write_page_mirror / multi-plane erase 会写到坏 plane 的同号块。
 *
 * @param alloc      分配器（读 BST 坏块标记）
 * @param block      per-plane 块号
 * @param plane_mask 需要全部可用的 plane 位掩码
 * @return true = mask 内所有 plane 的同号块都非 BAD；false = 至少一个 BAD
 */
static BOOL md_block_good_on_mask(const alc_s *alloc, WORD block, BYTE plane_mask)
{
    const BYTE n = md_ring_plane_count(plane_mask);

    for (BYTE i = 0U; i < n; i++)
    {
        const BYTE plane = md_ring_plane_at(plane_mask, i);
        const WORD g     = ftl_gblock(plane, block);

        if (ftl_block_is_bad(&alloc->blocks[g]))
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 收名义区间的好块；不足 min_blocks 时从 shared spare 按升序补（受 spare_budget
 * 与全局 spare_used 双重限制）。**只补到硬下界，不强行补满名义数**：CP 少一块
 * 仍可轮转，把 spare 留给更紧的 BBT/Journal。
 *
 * @param ring       输出：构建好的池几何
 * @param alloc      分配器（读 BST 坏块标记）
 * @param spec       该池的几何/补齐策略
 * @param spare_used spare 占用标记（跨池共享；函数会置位已用项）
 * @return 实际收集到的候选块数（ring->block_count）
 */
static WORD md_pool_collect(md_ring_s *ring, const alc_s *alloc, const md_pool_spec_s *spec, BOOL spare_used[MD_SPARE_BLOCKS_PER_PLANE])
{
    ring->first_block      = spec->first;
    ring->nominal_count    = spec->nominal;
    ring->block_count      = 0U;
    ring->pages_per_record = spec->pages_per_record;
    ring->plane_mask       = spec->plane_mask;

    const WORD end = (WORD)(spec->first + spec->nominal);

    for (WORD b = spec->first; b < end; b++)
    {
        if (ring->block_count >= MD_RING_MAX_BLOCKS)
        {
            break; /* candidates 数组容量 */
        }
        if (md_block_good_on_mask(alloc, b, spec->plane_mask))
        {
            ring->blocks[ring->block_count++] = b;
        }
    }

    // 停止点 = min(硬下界, candidates 数组容量)：两者任一到达后继续扫也不会再收。
    const WORD spare_end = (WORD)FTL_METADATA_AREA_END;
    const WORD target    = (spec->min_blocks < MD_RING_MAX_BLOCKS) ? spec->min_blocks : MD_RING_MAX_BLOCKS;
    WORD       used      = 0U;

    for (WORD s = MD_SPARE_BLOCK0; s < spare_end; s++)
    {
        if ((used >= spec->spare_budget) || (ring->block_count >= target))
        {
            break;
        }

        const WORD si = (WORD)(s - MD_SPARE_BLOCK0);

        if (spare_used[si] || (md_block_good_on_mask(alloc, s, spec->plane_mask) == false))
        {
            continue;
        }
        ring->blocks[ring->block_count++] = s;
        spare_used[si]                    = true;
        used++;
    }

    return ring->block_count;
}

/**
 * @brief 用 BBT BAD 集重建四个固定元数据分区的候选池
 *
 * 候选 = 名义区间内（所有 plane）非 BAD 的块；不足时从 shared spare 补。
 *
 * @param alloc 分配器（读 BST；pools 为输出目标）
 * @return true 全部满足；false 某分区凑不齐（release 下拒绝启动）
 */
BOOL md_pool_build_all(alc_s *alloc)
{
    if ((alloc == NULL) || (alloc->pools == NULL))
    {
        return false; /* 绑定缺失是配置错误：release 也必须拒绝 */
    }

    md_pools_s *p = alloc->pools;

    // 先在局部构建，全部成功才整体替换：失败时保留上一份可用候选池，
    // 避免「BBT/CP 已换、Journal 失败」的半更新状态被在飞模块读到。
    md_pools_s next;

    memset(&next, 0, sizeof(next));

    // 分配顺序固定：先紧后松。BBT/Journal 的名义池最小，优先拿 spare。
    BOOL spare_used[MD_SPARE_BLOCKS_PER_PLANE];

    memset(spare_used, 0, sizeof(spare_used));

    md_pool_spec_s specs[MD_POOL_NUM];

    for (BYTE id = 0U; id < MD_POOL_NUM; id++)
    {
        specs[id] = md_pool_spec_make(id);
    }

    if (md_pool_collect(&next.bbt, alloc, &specs[MD_POOL_BBT], spare_used) < specs[MD_POOL_BBT].min_blocks)
    {
        LOGE(LOG_TAG_POOL "BBT candidates < %hu — metadata area unusable\r\n", specs[MD_POOL_BBT].min_blocks);
        return false;
    }

    if (md_pool_collect(&next.cp, alloc, &specs[MD_POOL_CP], spare_used) < specs[MD_POOL_CP].min_blocks)
    {
        LOGE(LOG_TAG_POOL "Checkpoint candidates < %hu\r\n", specs[MD_POOL_CP].min_blocks);
        return false;
    }

    if (md_pool_collect(&next.jrn, alloc, &specs[MD_POOL_JRN], spare_used) < specs[MD_POOL_JRN].min_blocks)
    {
        LOGE(LOG_TAG_POOL "Journal candidates < %hu\r\n", specs[MD_POOL_JRN].min_blocks);
        return false;
    }

    if (md_pool_collect(&next.dir_ckpt, alloc, &specs[MD_POOL_DIR_CKPT], spare_used) < specs[MD_POOL_DIR_CKPT].min_blocks)
    {
        LOGE(LOG_TAG_POOL "directory checkpoint candidates < %hu\r\n", specs[MD_POOL_DIR_CKPT].min_blocks);
        return false;
    }

    next.dirty = false;
    *p         = next;

    // 候选表可能变了 ⇒ BBT 的 record 下标到物理块的映射也可能变。
    // 下一次 bbt_save 必须在新池上重新定位（不能沿用旧 write_idx）。
    alloc->bbt.cursor_valid = false;

    return true;
}

/**
 * @brief 池内 (record, page) → PBA（镜像页的规范副本）
 *
 * 固定取 plane_mask 的第一个 plane。
 *
 * @param ring           池几何
 * @param topo           ch/ce/lun
 * @param record_idx     池内槽位
 * @param page_in_record record 内页序
 * @return PBA；几何/槽位非法返回 NAND_PBA_INVALID
 */
DWORD md_ring_addr(const md_ring_s *ring, const ftl_topo_s *topo, WORD record_idx, BYTE page_in_record)
{
    const BYTE plane = md_ring_plane_at(ring->plane_mask, 0U);

    return md_ring_addr_plane(ring, topo, record_idx, page_in_record, plane);
}

/**
 * @brief 逐块探末条 → 每个非满块降序找末条 → 取全局 sequence 最大者。
 *
 * @param ring  池几何（block_count 已由 md_ring_geom_valid 限在 32 以内）
 * @param probe 调用方探针
 * @param ctx   探针上下文
 * @param rpb   每块 record 数
 * @param s     输出：扫描中间结果
 *
 * @note 判据不再是"第一个末槽无效的块 = 写 frontier"：引导环里
 *       BAD 块/读失败块的末槽同样无效，旧假设会把真正 frontier 之后的中段
 *       record 全部忽略。代价：空块通常只付 1 次探针（末页空白），读失败的块
 *       最多 rpb 次；只在定位（开机/冷定位）时执行。
 */
static void md_ring_scan(const md_ring_s *ring, metadata_ring_probe_fn_t probe, void *ctx, WORD rpb, md_ring_scan_s *s)
{
    s->cursor_block = UINT16_MAX;
    s->frontier_idx = UINT16_MAX;
    s->frontier_seq = 0U;

    // ① 逐块廉价探末条：末槽有效 ⇒ 该块本圈写满；无效 ⇒ 记入非满块集合。
    WORD  best_full_idx = UINT16_MAX;
    DWORD best_full_seq = 0U;
    DWORD open_blocks   = 0U; /* bit b = 块 b 的末槽无效（block_count <= 32） */

    for (WORD b = 0U; b < ring->block_count; b++)
    {
        const WORD idx = (WORD)((b * rpb) + (rpb - 1U));
        DWORD      seq = 0U;

        if (probe(ctx, idx, false, &seq) == FTL_OK)
        {
            if ((best_full_idx == UINT16_MAX) || (seq > best_full_seq))
            {
                best_full_idx = idx;
                best_full_seq = seq;
            }
        }
        else
        {
            open_blocks |= (1UL << b);
        }
    }

    // ② 每个非满块降序找最后一条有效 record，取全局 sequence 最大者。
    //    全空块只记下"第一个空块"，供池空时的写点使用。
    WORD  cursor_last_idx = UINT16_MAX;
    DWORD cursor_last_seq = 0U;

    for (WORD b = 0U; b < ring->block_count; b++)
    {
        if ((open_blocks & (1UL << b)) == 0U)
        {
            continue;
        }

        WORD  last_idx = UINT16_MAX;
        DWORD last_seq = 0U;

        for (WORD k = 0U; k < rpb; k++)
        {
            const WORD idx = (WORD)((b * rpb) + (rpb - 1U - k));
            DWORD      seq = 0U;

            if (probe(ctx, idx, false, &seq) == FTL_OK)
            {
                last_idx = idx;
                last_seq = seq;
                break;
            }
        }

        if (last_idx != UINT16_MAX)
        {
            if (last_seq > cursor_last_seq)
            {
                cursor_last_idx = last_idx;
                cursor_last_seq = last_seq;
            }
        }
        else if (s->cursor_block == UINT16_MAX)
        {
            s->cursor_block = b; /* 全空块：池空时的写点候选 */
        }
    }

    // ③ 全局最新 = max(满块末条, 非满块末条)。满块末条胜出时 frontier 落在末槽，
    //    写点由调用者按"下一块槽 0"处理。
    if ((cursor_last_idx != UINT16_MAX) && (cursor_last_seq > best_full_seq))
    {
        s->frontier_idx = cursor_last_idx;
        s->frontier_seq = cursor_last_seq;
    }
    else if (best_full_idx != UINT16_MAX)
    {
        s->frontier_idx = best_full_idx;
        s->frontier_seq = best_full_seq;
    }
}

/**
 * @brief ⑥ 慢路径：快路径候选整条校验全失败（局部损坏）⇒ 全池廉价探针取 sequence 最大者，
 * 整条校验；失败则排除该 sequence 继续找次新。仅在损坏时付出全池代价。
 *
 * @param probe 调用方探针
 * @param ctx   探针上下文
 * @param total 池内槽位总数
 * @param out   输出：最新有效 record 槽位与 sequence
 * @return true = 找到有效 record；false = 池内已无更多候选
 */
static BOOL md_ring_locate_fallback(metadata_ring_probe_fn_t probe, void *ctx, WORD total, md_ring_locate_out_s *out)
{
    DWORD limit = UINT32_MAX;

    for (;;)
    {
        WORD  ci = UINT16_MAX;
        DWORD cs = 0U;

        for (WORD k = 0U; k < total; k++)
        {
            DWORD seq = 0U;

            if (probe(ctx, k, false, &seq) != FTL_OK)
            {
                continue;
            }
            if (seq >= limit)
            {
                continue; /* 已排除（含同 sequence 的重复份）*/
            }
            if ((ci == UINT16_MAX) || (seq > cs))
            {
                ci = k;
                cs = seq;
            }
        }

        if ((ci == UINT16_MAX) || (cs == 0U))
        {
            return false; /* 池内已无更多候选 */
        }

        // 整条校验用独立出参：探针契约是「失败时不写 *out_sequence」，
        // 不能把 cs（下一步的排除界）押在这个隐含约定上。
        DWORD full_seq = 0U;

        if (probe(ctx, ci, true, &full_seq) == FTL_OK)
        {
            out->newest_idx = ci;
            out->sequence   = full_seq;
            return true;
        }
        limit = cs;
    }
}

/**
 * @brief 定位池内最新有效 record 与下一个可写槽
 *
 * 出参先给确定值；几何非法返回 FTL_ERROR（配置错误，release 也拒绝）。
 *
 * @param ring  池几何
 * @param probe 调用方探针
 * @param ctx   探针上下文
 * @param out   输出：最新槽位 / sequence / 写游标
 * @return FTL_OK（含池空）；FTL_ERROR 几何非法
 */
SSD_CODE_FAST ftl_status_e md_ring_locate(const md_ring_s *ring, metadata_ring_probe_fn_t probe, void *ctx, md_ring_locate_out_s *out)
{
    // 出参先给确定值：几何非法时也能安全读；池空时 newest=UINT16_MAX、seq=0
    // （调用者据此续序号）。
    out->newest_idx = UINT16_MAX;
    out->sequence   = 0U;
    out->write_idx  = 0U;

    // ring 自身几何是**配置**（构造错误 ⇒ 寻址全错）⇒ 保留运行期拒绝（判据 9）。
    if (md_ring_geom_valid(ring) == false)
    {
        return FTL_ERROR;
    }

    const WORD rpb   = md_ring_records_per_block(ring);
    const WORD total = md_ring_total_records(ring);

    md_ring_scan_s scan;

    md_ring_scan(ring, probe, ctx, rpb, &scan);

    // ④ frontier 整条校验：廉价探针只验末页，多页 record 的中段损坏在这里暴露。
    //    失败 ⇒ 全池回退，取次新的**完整** record（损坏分支）。
    DWORD full_seq = 0U;

    if ((scan.frontier_idx != UINT16_MAX) && (probe(ctx, scan.frontier_idx, true, &full_seq) == FTL_OK))
    {
        out->newest_idx = scan.frontier_idx;
        out->sequence   = full_seq;
    }
    else
    {
        (void)md_ring_locate_fallback(probe, ctx, total, out);
    }

    // ⑤ 写点由**已校验的**最新 record 推（不能沿用扫描的廉价结果：它可能没通过
    //    整条校验，回退后的胜者是另一条）：
    //    · 末槽 record ⇒ 该块已满，下一块槽 0（换块时 prepare 会先擦）；
    //    · 中段 record ⇒ 下一条槽；块中槽保守跳过一槽（掉电可能留下半写页）；
    //    · 池空 ⇒ 第一个空块的槽 0。
    if (out->newest_idx != UINT16_MAX)
    {
        if ((out->newest_idx % rpb) == (rpb - 1U))
        {
            out->write_idx = (WORD)((((out->newest_idx / rpb) + 1U) % ring->block_count) * rpb);
        }
        else
        {
            WORD w = (WORD)((out->newest_idx + 1U) % total);

            if ((w % rpb) != 0U)
            {
                w = (WORD)((w + 1U) % total);
            }
            out->write_idx = w;
        }
    }
    else
    {
        out->write_idx = (scan.cursor_block != UINT16_MAX) ? (WORD)(scan.cursor_block * rpb) : 0U;
    }

    return FTL_OK;
}

/**
 * @brief 算写点地址 + 判定是否需先擦块（不动介质）
 *
 * write_idx 落在块首才需擦；池内轮转 ⇒ 该块持最旧完整快照，擦掉不丢信息。
 *
 * @param ring           池几何
 * @param topo           ch/ce/lun
 * @param write_idx      写点槽位
 * @param out_addr       输出：record 第 0 页地址
 * @param out_need_erase 输出：是否需先擦该块
 * @return FTL_OK；FTL_ERROR 几何/槽位非法
 */
ftl_status_e md_ring_prepare_addr(const md_ring_s *ring, const ftl_topo_s *topo, WORD write_idx, DWORD *out_addr, BOOL *out_need_erase)
{
    if (md_ring_geom_valid(ring) == false)
    {
        return FTL_ERROR;
    }

    const WORD total = md_ring_total_records(ring);
    const WORD rpb   = md_ring_records_per_block(ring);

    if (write_idx >= total)
    {
        return FTL_ERROR;
    }

    *out_addr       = md_ring_addr(ring, topo, write_idx, 0U);
    *out_need_erase = ((write_idx % rpb) == 0U);
    if (*out_addr == NAND_PBA_INVALID)
    {
        return FTL_ERROR;
    }

    return FTL_OK;
}

/**
 * @brief md_ring_prepare_addr + 同步擦除（仅开机/格式化用）
 *
 * 镜像池用 multi-plane 擦，所有副本一起清；运行期走异步擦。
 *
 * @param ring      池几何
 * @param topo      ch/ce/lun
 * @param write_idx 写点槽位
 * @param out_addr  输出：record 第 0 页地址
 * @return FTL_OK；FTL_ERROR 擦除失败
 */
ftl_status_e md_ring_prepare_write(const md_ring_s *ring, const ftl_topo_s *topo, WORD write_idx, DWORD *out_addr)
{
    DWORD addr;
    BOOL  need_erase = false;

    if (md_ring_prepare_addr(ring, topo, write_idx, &addr, &need_erase) != FTL_OK)
    {
        return FTL_ERROR;
    }

    if (need_erase)
    {
        // 换块：先擦目标块（池内轮转 ⇒ 该块持全池最旧 record，擦掉不丢信息）
        const int st = (md_ring_plane_count(ring->plane_mask) > 1U) ? ftl_sync_erase_block_multi_plane(addr) : ftl_sync_erase_block(addr);
        if (st != 0)
        {
            return FTL_ERROR;
        }
    }

    *out_addr = addr;
    return FTL_OK;
}

/* ══════════════════════════════════════════════════════════════════
 * 二、目录检查点池（Metadata Directory 的 record 池）
 * ══════════════════════════════════════════════════════════════════ */

/**
 * @brief 页头字段校验：magic + chunk_count + 布局版本。
 *
 * @param magic          页头 magic
 * @param chunk_count    页头 chunk 总数
 * @param layout_version 页头介质布局版本
 * @return true = 三个字段均与当前格式一致；false = 任一不符
 */
static BOOL md_dir_ckpt_header_valid(DWORD magic, WORD chunk_count, DWORD layout_version)
{
    return (magic == DIR_CKPT_MAGIC) && (chunk_count == DIR_CKPT_PAGE_COUNT) && (layout_version == FTL_MEDIA_LAYOUT_VERSION);
}

/**
 * @brief 目录检查点池几何：优先用 md_pool_build_all() 生成的候选池（跳过 BAD）；
 * 池尚未建立时退回名义区间（开机早期/测试），BAD 由后续 rebuild 修正。
 *
 * @param dir  Directory cache（取 alloc->pools）
 * @param ring 输出：目录检查点池几何
 */
static void md_dir_ckpt_ring_init(const md_dir_cache_s *dir, md_ring_s *ring)
{
    if ((dir->alloc != NULL) && (dir->alloc->pools != NULL))
    {
        *ring = dir->alloc->pools->dir_ckpt;
        return;
    }

    ring->first_block      = DIR_CKPT_BLOCK0;
    ring->nominal_count    = DIR_CKPT_POOL_BLOCKS;
    ring->block_count      = DIR_CKPT_POOL_BLOCKS;
    ring->pages_per_record = DIR_CKPT_PAGE_COUNT;
    ring->plane_mask       = METADATA_MIRROR_PLANE_MASK;
    for (WORD i = 0U; i < DIR_CKPT_POOL_BLOCKS; i++)
    {
        ring->blocks[i] = (WORD)(DIR_CKPT_BLOCK0 + i);
    }
}

/**
 * @brief 廉价探针：只读 chunk 0 的 20 字节页头取 sequence。
 * FC 读整页并校验 per-CW CRC + ECC，`length` 只限制拷出字节数。
 *
 * @param c            探针上下文（Directory + 池几何）
 * @param record_idx   槽位号
 * @param out_sequence 输出：该 record 的 sequence（成功时写）
 * @return FTL_OK = 某个 plane 上读到有效页头；FTL_ERROR = 所有 plane 都失败
 */
static ftl_status_e md_dir_ckpt_record_probe_cheap(const dir_ckpt_probe_ctx_s *c, WORD record_idx, DWORD *out_sequence)
{
    const BYTE plane_count = md_ring_plane_count(c->ring->plane_mask);

    for (BYTE pi = 0U; pi < plane_count; pi++)
    {
        const BYTE        plane = md_ring_plane_at(c->ring->plane_mask, pi);
        md_dir_ckpt_hdr_s hdr;
        DWORD             addr = md_ring_addr_plane(c->ring, &c->dir->topo, record_idx, 0U, plane);

        if (ftl_sync_read_page(addr, &hdr, sizeof(hdr), FTL_PAGE_CFG_META()) != 0)
        {
            continue;
        }
        if ((md_dir_ckpt_header_valid(hdr.magic, hdr.chunk_count, hdr.layout_version) == false) || (hdr.chunk_index != 0U) || (hdr.sequence == 0U))
        {
            continue;
        }

        *out_sequence = hdr.sequence;
        return FTL_OK;
    }

    return FTL_ERROR;
}

/**
 * @brief 整条校验：全部页有效、chunk 序完整、各页 sequence 一致；不跨 plane 拼半条。
 *
 * @param c            探针上下文（Directory + 池几何）
 * @param record_idx   槽位号
 * @param out_sequence 输出：整条校验通过时的 sequence
 * @return FTL_OK = 某个 plane 上整条有效；FTL_ERROR = 无 plane 通过
 */
static ftl_status_e md_dir_ckpt_record_probe_full(const dir_ckpt_probe_ctx_s *c, WORD record_idx, DWORD *out_sequence)
{
    md_dir_ckpt_page_s page;
    const BYTE         plane_count = md_ring_plane_count(c->ring->plane_mask);

    for (BYTE pi = 0U; pi < plane_count; pi++)
    {
        const BYTE plane      = md_ring_plane_at(c->ring->plane_mask, pi);
        DWORD      seq        = 0U;
        DWORD      dgen       = 0U; /* 本 record 主半的目录代数（必须全条一致）*/
        BOOL       primary_ok = true;

        for (BYTE chunk = 0U; chunk < DIR_CKPT_PAGE_COUNT; chunk++)
        {
            const BOOL backup = (chunk >= DIR_CKPT_DIR_PRIMARY_PAGES);
            DWORD      addr   = md_ring_addr_plane(c->ring, &c->dir->topo, record_idx, chunk, plane);

            // 备份半是 best-effort：单页坏/旧代只让对应 region 暂无双副本，
            // 不能让整条最新 Directory 作废（否则一次备份页失败就回退到旧快照）。
            if (ftl_sync_read_page(addr, &page, sizeof(page), FTL_PAGE_CFG_META()) != 0)
            {
                if (backup)
                {
                    continue;
                }
                primary_ok = false;
                break;
            }
            const BOOL header_ok = md_dir_ckpt_header_valid(page.magic, page.chunk_count, page.layout_version);

            if ((header_ok == false) || (page.chunk_index != chunk))
            {
                if (backup)
                {
                    continue;
                }
                primary_ok = false;
                break;
            }

            if (chunk == 0U)
            {
                seq = page.sequence;
                if (seq == 0U)
                {
                    primary_ok = false; /* sequence 从 1 起；0 = 未写 */
                    break;
                }
            }
            else if (page.sequence != seq)
            {
                if (backup)
                {
                    continue;
                }                   /* 旧代备份页：忽略 */
                primary_ok = false; /* 主半必须同 sequence */
                break;
            }

            if (backup == false)
            {
                // 整条记录的页间一致性：同一 sequence 下，各页还必须是**同一
                // 目录代数**。写入期间目录更新过 ⇒ 早写的页是旧代、晚写的是新代，这种
                // "混代记录"在媒体上看起来完全合法；被当作最新目录加载时，不同代次的
                // 条目会在复用过的页上互相碰撞（两个 region 指向同一页）。这里整条作废，
                // 回退到上一条一致记录；丢失的增量由 Journal 回放补回。
                if (chunk == 0U)
                {
                    dgen = page.dirty_gen;
                }
                else if (page.dirty_gen != dgen)
                {
                    primary_ok = false;
                    break;
                }
            }
        }

        if (primary_ok)
        {
            *out_sequence = seq;
            return FTL_OK;
        }
    }

    return FTL_ERROR;
}

/**
 * @brief 探针入口：按 full 分派到廉价/整条两档。
 *
 * @param ctx          探针上下文（dir_ckpt_probe_ctx_s）
 * @param record_idx   槽位号
 * @param full         false = 廉价探针，true = 整条校验
 * @param out_sequence 输出：该 record 的 sequence
 * @return 探针函数的返回值（见 metadata_ring_probe_fn_t）
 */
static ftl_status_e md_dir_ckpt_record_probe(void *ctx, WORD record_idx, BOOL full, DWORD *out_sequence)
{
    const dir_ckpt_probe_ctx_s *c = (const dir_ckpt_probe_ctx_s *)ctx;

    return full ? md_dir_ckpt_record_probe_full(c, record_idx, out_sequence) : md_dir_ckpt_record_probe_cheap(c, record_idx, out_sequence);
}

/* ══════════════════════════════════════════════════════════════════
 * 三、Directory 基础层 / 查询 / 更新
 * ══════════════════════════════════════════════════════════════════ */

/**
 * @brief 为全部 region 登记"未映射"（见 metadata.h「固定元数据区布局」）
 *
 * 每个实例的 L2P_REGIONS_PER_INSTANCE 个 region **全部登记为 NAND_PBA_INVALID**；
 * 某个 region 刷新到日志块后由 md_commit_region 就地覆盖成真实 PBA。
 * "全部预登记"同时保证 Directory 容量（= region 数）按构造不会溢出 ——
 * md_entry_update 的"满则丢弃"分支对 L2P region 不可达。
 *
 * 共用于 md_init（首次格式化）与 md_load（基础层）：
 * 目录检查点损坏时该层即"全部未映射"，不需要任何闪存读。
 *
 * @param dir Directory cache（输出：全部 region 预登记为未映射）
 */
static void md_fixed_fill(md_dir_cache_s *dir)
{
    dir->count = 0U;

    for (WORD i = 0U; i < L2P_REGIONS_PER_INSTANCE; i++)
    {
        md_entry_s *entry = &dir->entries[dir->count];

        entry->type        = METADATA_TYPE_L2P_REGION;
        entry->id          = i;
        entry->pba         = NAND_PBA_INVALID;
        dir->backup_pba[i] = NAND_PBA_INVALID;
        dir->count++;
    }
}

/**
 * @brief 把一个 region 副本的 PBA 计入 l2p_live（主/备共用同一套校验）。
 * `*warned` 保证整轮只警告一次；越界/固定非日志区一律跳过。
 *
 * @param dir       Directory cache（输出：l2p_live 计数）
 * @param pba       某个 region 副本的 PBA
 * @param entry_idx 该副本所属的 Directory 条目下标（仅用于告警定位）
 * @param warned    输入/输出：本轮是否已告警（防止刷屏）
 */
static void md_live_copy_add(md_dir_cache_s *dir, DWORD pba, WORD entry_idx, BOOL *warned)
{
    if (pba == NAND_PBA_INVALID)
    {
        return;
    }

    const BYTE plane = pba_to_plane(pba);
    const WORD block = pba_to_block(pba);

    if ((plane >= PLANE_NUM) || (block >= BLOCKS_PER_PLANE))
    {
        if (*warned == false)
        {
            LOGW(LOG_TAG_META "directory entry %hu: plane=%hhu block=%hu out of range — skipped\r\n", entry_idx, plane, block);
            *warned = true;
        }
        return;
    }

    // 只排除**非日志的**固定元数据区（0..L2P_LOG_BLOCK0−1）。
    if (block < L2P_LOG_BLOCK0)
    {
        return;
    }

    const WORD g = ftl_gblock(plane, block);

    if (g < FTL_BLOCK_TOTAL)
    {
        dir->l2p_live[g]++;
    }
    else if (*warned == false)
    {
        LOGW(LOG_TAG_META "directory entry %hu: gblock %hu out of range — skipped\r\n", entry_idx, g);
        *warned = true;
    }
}

/**
 * @brief 由 entries 重建 l2p_live（日志块 = 非固定元数据块）。
 *
 * @param dir Directory cache（输出：l2p_live 与 log_borrow_count）
 */
static void md_live_rebuild(md_dir_cache_s *dir)
{
    memset(dir->l2p_live, 0, sizeof(dir->l2p_live));

    BOOL warned = false;

    for (WORD i = 0U; i < dir->count; i++)
    {
        if (dir->entries[i].type != METADATA_TYPE_L2P_REGION)
        {
            continue;
        }

        // 主副本 + 备副本都计入 live：回收判据是“所有占用者都走了”。
        // ⚠ INVALID/越界/固定非日志区的校验在 helper 里统一（介质输入不可信，H29）。
        const WORD  rid         = dir->entries[i].id;
        const DWORD primary_pba = dir->entries[i].pba;

        md_live_copy_add(dir, primary_pba, i, &warned);
        if (rid < METADATA_DIR_ENTRIES)
        {
            const DWORD backup_pba = dir->backup_pba[rid];

            md_live_copy_add(dir, backup_pba, i, &warned);
        }
    }

    // 借用计数按**块**计，不按 entry 计（一个借来的日志块可承载多个 region）。
    dir->log_borrow_count = 0U;

    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        if (dir->l2p_live[g] == 0U)
        {
            continue;
        }

        const BYTE plane = ftl_gblock_plane(g);
        const WORD block = ftl_gblock_block(g);

        if (md_block_is_metadata(NULL, plane, block) == false)
        {
            dir->log_borrow_count++;
        }
    }
}

/**
 * @brief 开机观测口（判据②「走动态分配」/ 判据③「目录检查点真的落盘」的唯一观测口）
 *
 * @param dir Directory cache（只读，打一条开机观测行）
 */
static void md_boot_status_log(const md_dir_cache_s *dir)
{
    DWORD live_blocks = 0U;

    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        if (dir->l2p_live[g] != 0U)
        {
            live_blocks++;
        }
    }

    const unsigned dyn = (live_blocks != 0U) ? 1U : 0U;

    // 当前写块按 plane 打印（每 plane 一个，L2）。
    LOGI(LOG_TAG_META "dir_ckpt_seq=%lu dyn=%u cur_blk=%hu/%hu live_blks=%lu count=%hu\r\n",
         dir->ckpt.sequence,
         dyn,
         (dir->cur.valid[0]) ? dir->cur.block[0] : (WORD)UINT16_MAX,
         (PLANE_NUM > 1U && dir->cur.valid[1]) ? dir->cur.block[1] : (WORD)UINT16_MAX,
         live_blocks,
         dir->count);
}

/**
 * @brief 初始化 Directory：绑指针、清计数、游标置无效
 *
 * 首次追加才从 Free Pool 取块。
 *
 * @param dir          Directory
 * @param alloc        分配器
 * @param total_writes 累计写计数指针（只持有）
 */
void md_init(md_dir_cache_s *dir, alc_s *alloc, const DWORD *total_writes)
{
    // 前置条件：唯一调用点 ftl_init 传 &ftl->alloc / &ftl->stats.total_writes（调用链已保证）。
    dir->alloc        = alloc;
    dir->total_writes = total_writes;

    s_pool_rebuilt_logged = 0U; /* DTCM NOLOAD：文件级静态必须显式复位 */

    dir->ckpt.sequence       = 0U;
    dir->ckpt.busy           = false;
    dir->ckpt.erase_required = false;
    dir->ckpt.owner          = MD_DIR_CKPT_OWNER_NONE;
    dir->ckpt.dirty          = false;
    dir->ckpt.dirty_gen      = 0U;
    dir->ckpt.covered_gen    = 0U;

    md_slot_reset(dir);

    md_fixed_fill(dir);
    md_live_rebuild(dir);

    // 游标全部置无效：首次追加才从固定 L2P 日志区取块（绝不复用可能已编程的旧页）
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        dir->cur.block[p] = 0U;
        dir->cur.page[p]  = 0U;
        dir->cur.valid[p] = false;
    }
    dir->ckpt.write_idx    = 0U;
    dir->ckpt.cursor_valid = false;

    // 待回收位图 / 后台整理 job 归零（RAM 态，不跨重启；
    // 不清 job ⇒ append_slot 永久让路）。
    md_reclaim_reset(dir);
    md_handle_reset(dir);
}

/**
 * @brief 查 (type,id) 的下标；未找到返回 dir->count（调用者用它判 < count）。
 *
 * ⚠ 必须先走**稠密快路径**：Directory 的 L2P_REGION 条目由 `md_fixed_fill` 按
 * `entries[id].id == id` 稠密预登记（目录检查点也按同一顺序读写），查找是 O(1)。
 * 只做线性扫描时，`md_compact_read` 对每个 region_id 各查一次 ⇒ O(N²)
 * （N = 2048，约 400 万次比较 ≈ 100 ms），元数据压缩期间主循环被拖住、USB
 * 事件得不到服务（表现为设备消失）。慢路径保留给非稠密/历史格式。
 *
 * @param dir  Directory cache
 * @param type 元数据类型
 * @param id   元数据 id
 * @return 匹配条目的下标；未找到返回 dir->count
 */
static WORD md_entry_index_find(const md_dir_cache_s *dir, BYTE type, WORD id)
{
    if ((id < dir->count) && (dir->entries[id].type == type) && (dir->entries[id].id == id))
    {
        return id;
    }

    for (WORD i = 0U; i < dir->count; i++)
    {
        if ((dir->entries[i].type == type) && (dir->entries[i].id == id))
        {
            return i;
        }
    }

    return dir->count;
}

/**
 * @brief 查 (type,id) 的登记 PBA（dense id 快路径 + 线性兜底）
 *
 * 当前只登记 L2P_REGION；其余元数据是固定位置，不经过本函数。
 *
 * @param dir  Directory
 * @param type 条目类型
 * @param id   条目 id
 * @return 登记的 PBA；未找到返回 NAND_PBA_INVALID
 */
DWORD md_entry_find(const md_dir_cache_s *dir, md_type_e type, WORD id)
{
    const WORD i = md_entry_index_find(dir, type, id);

    return (i < dir->count) ? dir->entries[i].pba : NAND_PBA_INVALID;
}

/**
 * @brief 块被多少 L2P region 引用（活计数）
 *
 * 越界块号按「无引用」处理（来源可能是介质/快照）。
 *
 * @param dir    Directory
 * @param gblock 全局块号
 * @return 活 region 数
 */
WORD md_live_count_get(const md_dir_cache_s *dir, WORD gblock)
{
    if (gblock >= FTL_BLOCK_TOTAL)
    {
        return 0U; /* 来源可能是介质/快照（H29）：越界按“无引用”处理 */
    }
    return dir->l2p_live[gblock];
}

/**
 * @brief 更新/追加 (type,id) 的 PBA 登记
 *
 * 容量满时不追加并 LOGE；本项目容量 == region 数，fill_fixed 已登记全部。
 *
 * @param dir     Directory
 * @param type    条目类型
 * @param id      条目 id
 * @param new_pba 新 PBA
 */
void md_entry_update(md_dir_cache_s *dir, md_type_e type, WORD id, DWORD new_pba)
{
    // 查找现有 entry（覆盖旧位置）
    const WORD i = md_entry_index_find(dir, type, id);

    if (i < dir->count)
    {
        dir->entries[i].pba = new_pba;
        dir->ckpt.dirty     = true;
        dir->ckpt.dirty_gen++;
        return;
    }

    // 新增 entry（往 entries 数组尾部追加）
    if (dir->count < METADATA_DIR_ENTRIES)
    {
        md_entry_s *entry = &dir->entries[dir->count];

        entry->type = type;
        entry->id   = id;
        entry->pba  = new_pba;
        dir->count++;
        dir->ckpt.dirty = true;
        dir->ckpt.dirty_gen++;
        return;
    }

    // 满：不追加。L2P_REGION 按构造不可达；新增元数据类型前必须先扩容量。
    // 静默丢弃会让调用者以为位置已记录 ⇒ 至少留下错误日志。
    LOGE(LOG_TAG_META "directory full: type=%hhu id=%hu dropped\r\n", type, id);
}

/* ══════════════════════════════════════════════════════════════════
 * 四、L2P 日志块：追加槽 / 登记
 *（回收 / 压缩在 metadata_job.c；本区只维护写指针与 Directory 登记）
 * ══════════════════════════════════════════════════════════════════ */

/* ── 固定 L2P 日志区：分配 / 归还 / 开机恢复── */

/**
 * @brief 在固定 L2P 日志区分配一块「已擦净、空闲」的块并标 LOG
 *
 * @param dir   Directory
 * @param alloc 分配器
 * @param plane 目标 plane
 * @return gblock；FTL_GBLOCK_INVALID = 无可用块
 */
WORD md_log_alloc(md_dir_cache_s *dir, alc_s *alloc, BYTE plane)
{
    ASSERT(plane < PLANE_NUM);

    for (WORD block = L2P_LOG_BLOCK0; block < L2P_LOG_AREA_END; block++)
    {
        const WORD   g = ftl_gblock(plane, block);
        bst_block_s *b = &alloc->blocks[g];

        if (ftl_block_is_bad(b))
        {
            continue;
        }
        // 只有“已擦净、空闲”的固定日志块可分配（NONE 是本区的 free 标记）。
        if (ftl_block_state(b) != FTL_BLOCK_STATE_NONE)
        {
            continue;
        }
        if (dir->cur.valid[plane] && (g == dir->cur.block[plane]))
        {
            continue;
        }

        ftl_block_set_state(b, FTL_BLOCK_STATE_LOG);
        ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
        return g;
    }

    // 固定区无空闲块：从 **OP 池**有界借用动态日志块（L2P_LOG_BORROW_MAX）。
    // 不借则压缩没有目标块、CP 永远拿不到槽。借出的块由 l2p_live /
    // md_block_is_metadata 动态识别，擦空后由 md_log_release 回 OP 池。
    if (dir->log_borrow_count < L2P_LOG_BORROW_MAX)
    {
        const WORD borrowed = alc_op_alloc_log_block(alloc, plane);

        if (borrowed != FTL_GBLOCK_INVALID)
        {
            dir->log_borrow_count++;
            return borrowed;
        }
    }

    return FTL_GBLOCK_INVALID;
}

/**
 * @brief 归还一个日志块
 *
 * 固定区内留 NONE，借来的回 OP 池；BAD 是终态，不回收。
 *
 * @param dir    Directory
 * @param gblock 全局块号
 */
void md_log_release(md_dir_cache_s *dir, WORD gblock)
{
    ASSERT(gblock < FTL_BLOCK_TOTAL);

    alc_s *alloc = dir->alloc;

    bst_block_s *b = &alloc->blocks[gblock];

    // 固定日志区内的块 ⇒ 留在固定区（NONE）；借来的块 ⇒ 回 OP 池。
    const BOOL borrowed = (ftl_is_l2p_log_block(ftl_gblock_block(gblock)) == false);

    if (borrowed && (dir->log_borrow_count > 0U))
    {
        dir->log_borrow_count--;
    }

    // BAD 是终态：擦除失败后不得“释放”回可用池。
    if (ftl_block_is_bad(b))
    {
        return;
    }

    if (borrowed)
    {
        // 借来的动态日志块回 OP 池（与 alc_op_alloc_log_block 成对）；OP 满则回 Free Pool。
        ftl_block_set_state(b, FTL_BLOCK_STATE_NONE);
        ftl_block_clear_valid(b);
        ftl_block_set_sb(b, FTL_BLOCK_NO_SB);

        const BYTE plane = ftl_gblock_plane(gblock);

        if (alc_op_push(alloc, plane, gblock) == false)
        {
            alc_free_push(alloc, gblock);
        }
        return;
    }

    ftl_block_set_state(b, FTL_BLOCK_STATE_NONE);
    ftl_block_clear_valid(b);
    ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
}

/**
 * @brief 固定 L2P 日志区的空闲块数（state == NONE 且非 BAD）
 *
 * 压缩触发的主判据：按 free 水位压，而不是等 live 接近上限。
 *
 * @param alloc 分配器
 * @return 空闲块数
 */
WORD md_log_area_free_count(const alc_s *alloc)
{
    WORD n = 0U;

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        for (WORD block = L2P_LOG_BLOCK0; block < L2P_LOG_AREA_END; block++)
        {
            const WORD         g = ftl_gblock(plane, block);
            const bst_block_s *b = &alloc->blocks[g];

            if (ftl_block_is_bad(b))
            {
                continue;
            }
            if (ftl_block_state(b) == FTL_BLOCK_STATE_NONE)
            {
                n++;
            }
        }
    }

    return n;
}

/**
 * @brief 固定 L2P 日志区内活计数非 0 的块数（诊断 / info 用）
 *
 * @param dir Directory
 * @return 活块数
 */
WORD md_live_block_count_get(const md_dir_cache_s *dir)
{
    WORD n = 0U;

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        for (WORD block = L2P_LOG_BLOCK0; block < L2P_LOG_AREA_END; block++)
        {
            if (dir->l2p_live[ftl_gblock(plane, block)] > 0U)
            {
                n++;
            }
        }
    }

    return n;
}

/**
 * @brief 开机恢复日志区：按活计数分「存活 / 待回收」
 *
 * 非活块全部登记进 reclaim 位图。
 *
 * @param dir   Directory
 * @param alloc 分配器
 */
void md_log_area_restore(md_dir_cache_s *dir, alc_s *alloc)
{
    WORD n_live    = 0U;
    WORD n_reclaim = 0U;

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        for (WORD block = L2P_LOG_BLOCK0; block < L2P_LOG_AREA_END; block++)
        {
            const WORD   g = ftl_gblock(plane, block);
            bst_block_s *b = &alloc->blocks[g];

            if (ftl_block_is_bad(b))
            {
                continue;
            }

            const BOOL cur  = dir->cur.valid[plane] && (g == dir->cur.block[plane]);
            const BOOL live = (dir->l2p_live[g] > 0U);

            if (live || cur)
            {
                ftl_block_set_state(b, FTL_BLOCK_STATE_LOG);
                ftl_block_clear_valid(b);
                ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
                n_live++;
                continue;
            }

            // 非活块：Directory 不引用它 ⇒ **一律登记回收**，擦完由后台 md_log_release 置 NONE。
            // ⚠ 不读介质判「是否已擦净」：块状态不该由页面内容反推——FTL 只看得见
            // USER 512B，判不出「编程过没有」。擦除是幂等的，多擦一次
            // 只花 P/E；而判错一次就是在未擦页上编程 = 静默介质损坏。
            // 擦除按空闲水位**懒执行**（metadata_job.c 的 MD_JOB_RECLAIM 门控），
            // 所以开机不会把整个日志区重擦一遍。
            ftl_block_set_state(b, FTL_BLOCK_STATE_LOG);
            ftl_block_clear_valid(b);
            ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
            (void)md_reclaim_mark(dir, g);
            n_reclaim++;
        }
    }

    /* ── 动态借用块对账（用户区、非固定区）──
     * ① live>0 但 state 不是 LOG（CP 快照早于借用）：补置 LOG，防止被池化/重建误用；
     * ② live==0 且 state==LOG（借用块已搬空、掉电前没来得及擦/归还）：登记回收，
     *    擦除后 md_log_release 回 OP；在此之前占用借用配额，避免无限借。
     * 没有这一遍，② 会成为“有借不还”的孤儿块（不在任何池、也无回收登记）。 */
    for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        const BYTE   plane = ftl_gblock_plane(g);
        const WORD   block = ftl_gblock_block(g);
        bst_block_s *b     = &alloc->blocks[g];

        if (md_block_is_metadata(NULL, plane, block) || ftl_block_is_bad(b))
        {
            continue;
        }

        if (dir->l2p_live[g] > 0U)
        {
            if (ftl_block_state(b) != FTL_BLOCK_STATE_LOG)
            {
                ftl_block_set_state(b, FTL_BLOCK_STATE_LOG);
                ftl_block_clear_valid(b);
                ftl_block_set_sb(b, FTL_BLOCK_NO_SB);
            }
        }
        else if (ftl_block_state(b) == FTL_BLOCK_STATE_LOG)
        {
            if (md_reclaim_mark(dir, g) && (dir->log_borrow_count < UINT16_MAX))
            {
                dir->log_borrow_count++;
            }
        }
    }

    // 这里不报 free 数：非活块此刻全部是「待擦」而非「空闲」，空闲数由后台擦除按水位补足
    // （metadata_job.c 的 MD_JOB_RECLAIM 门控）。
    LOGI(LOG_TAG_META "L2P log area: live=%hu reclaim=%hu borrow=%hu (total=%u/plane)\r\n", n_live, n_reclaim, dir->log_borrow_count, L2P_LOG_BLOCKS_PER_PLANE);
}

// false = 没换（固定日志区无空闲块，或活块数超水位请求了压缩）。
// 换块只影响**指定 plane**，只从该 plane 的固定 L2P 日志区取块，不碰用户池。
// 活块数超 L2P_LOG_COMPACT_TRIGGER 时只登记 compact_wanted 并返回 false。
// 压缩 job 也复用本函数：当前块装不下 victim 时，新块的整块空槽是唯一
// destination。

/**
 * @brief 单个 PBA 的账本迁移：旧位置 -1、新位置 +1、旧块归零则登记回收。
 * 主/备副本共用（L2 双副本）。
 *
 * @param dir     Directory cache（输出：l2p_live 账本）
 * @param old     旧位置 PBA（NAND_PBA_INVALID = 未映射）
 * @param new_pba 新位置 PBA
 */
static void md_region_copy_commit(md_dir_cache_s *dir, DWORD old, DWORD new_pba)
{
    WORD old_g = UINT16_MAX;

    // 旧位置必须真的可能是 L2P region 槽位：只排除非日志固定区（0..L2P_LOG_BLOCK0−1）。
    // 目录检查点条目来自介质（H29），损坏条目可能指向目录检查点/CP/BBT/Journal ⇒ 跳过。
    // ⚠ **不能**用 `md_block_is_metadata(NULL, ...)`：L2P 日志区是固定分区，会被一起
    // 排除 ⇒ 旧位置的活计数永不递减、`l2p_live` 只增不减。
    if ((old != NAND_PBA_INVALID) && (pba_to_plane(old) < PLANE_NUM) && (pba_to_block(old) >= L2P_LOG_BLOCK0))
    {
        WORD g = ftl_pba_gblock(old);

        if ((g < FTL_BLOCK_TOTAL) && (dir->l2p_live[g] > 0U))
        {
            dir->l2p_live[g]--;
            old_g = g;
        }
    }

    // 新位置的活计数 +1
    {
        WORD g = ftl_pba_gblock(new_pba);

        if (g < FTL_BLOCK_TOTAL)
        {
            dir->l2p_live[g]++;
        }
    }

    // 旧块活计数归零且非当前块 ⇒ 登记进待回收位图，由后台 job 异步擦除。
    // ⚠ 压缩期间**抑制**本登记：搬移 victim 的最后一个 region 时它会归零，此时目录检查点
    // 还没指向新位置 ⇒ 擦了就会掉电丢映射（③④ 契约）。压缩期间归零的块由 job 自己
    // 在 CP_RECLAIM 里擦。
    if ((old_g != UINT16_MAX) && (md_handle_is_compacting(dir) == false) && md_log_block_is_idle(dir, old_g))
    {
        (void)md_reclaim_mark(dir, old_g);
    }
}

/**
 * @brief 提交一个 region 的新位置（账本迁移）
 *
 * 旧位置 -1、新位置 +1；旧块归零且非当前块 ⇒ 登记 reclaim_pending。
 * 不擦 NAND（运行在完成派发里，就地擦 = 嵌套泵）。
 *
 * @param dir         Directory
 * @param region_id   全局 region
 * @param mode        单副本 / 主备
 * @param new_primary 新主副本 PBA
 * @param new_backup  新备副本 PBA
 */
void md_commit_region(md_dir_cache_s *dir, WORD region_id, md_commit_mode_e mode, DWORD new_primary, DWORD new_backup)
{
    ASSERT(region_id < METADATA_DIR_ENTRIES);
    ASSERT(mode < MD_COMMIT_MODE_NUM);

    if (region_id >= METADATA_DIR_ENTRIES)
    {
        return;
    }

    BYTE first;
    BYTE count;

    switch (mode)
    {
        case MD_COMMIT_PRIMARY:
            first = 0U;
            count = 1U;
            break;
        case MD_COMMIT_BACKUP:
            first = 1U;
            count = 1U;
            break;
        case MD_COMMIT_PAIR:
            first = 0U;
            count = L2P_REGION_COPIES;
            break;
        default:
            ASSERT(0); /* H40：枚举入口断言；release 下静默 no-op */
            return;
    }

    // 成对提交先整体校验：避免只提交了主副本才发现备份参数无效（半对不是原子提交）。
    if (mode == MD_COMMIT_PAIR)
    {
        ASSERT(new_primary != new_backup);
        ASSERT(new_primary != NAND_PBA_INVALID);
        ASSERT(new_backup != NAND_PBA_INVALID);

        if ((new_primary == new_backup) || (new_primary == NAND_PBA_INVALID) || (new_backup == NAND_PBA_INVALID))
        {
            return;
        }
    }
    else
    {
        const DWORD pba = (mode == MD_COMMIT_PRIMARY) ? new_primary : new_backup;

        ASSERT(pba != NAND_PBA_INVALID);
        if (pba == NAND_PBA_INVALID)
        {
            return;
        }
    }

    const BYTE end = (BYTE)(first + count);

    for (BYTE copy = first; copy < end; copy++)
    {
        const DWORD new_pba = (copy == 0U) ? new_primary : new_backup;

        if (copy == 0U)
        {
            const DWORD old = md_entry_find(dir, METADATA_TYPE_L2P_REGION, region_id);

            md_region_copy_commit(dir, old, new_pba);
            md_entry_update(dir, METADATA_TYPE_L2P_REGION, region_id, new_pba);
        }
        else
        {
            const DWORD old = md_backup_find(dir, region_id);

            md_region_copy_commit(dir, old, new_pba);

            // 备份副本没有额外条目结构，只更新账本并置脏。
            dir->backup_pba[region_id] = new_pba;
            dir->ckpt.dirty            = true;
            dir->ckpt.dirty_gen++;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════
 * 五、目录检查点落盘 / 加载
 * ══════════════════════════════════════════════════════════════════ */

/* ── 目录检查点落盘的三段式（唯一形态）──
 *
 * 一条目录检查点记录 = DIR_CKPT_PAGE_COUNT 页。**所有**落盘站点（Checkpoint job、
 * 日志块整理 job）都拆成 prepare →（可选换块擦）→ 逐页写 → commit 四步，每步之间
 * 可以回到服务循环。三段式把「Directory 格式」留在本文件，「状态机」留给调用者。
 */

/**
 * @brief 目录检查点写入准备：定位游标 + 取槽
 *
 * 副作用：游标未建立时定位并置 cursor_valid；sequence 先自增再取槽。
 * 不动介质（换块擦由调用者做）。
 *
 * @param dir   Directory
 * @param out   输出：ring/base/idx/seq/need_erase
 * @param owner 单飞持有者（MD_DIR_CKPT_OWNER_*）
 * @return CLEAN / READY / FAILED / BUSY
 */
md_dir_ckpt_step_e md_dir_ckpt_prepare(md_dir_cache_s *dir, md_dir_ckpt_prepare_out_s *out, BYTE owner)
{
    if (dir->ckpt.dirty == false)
    {
        return MD_DIR_CKPT_CLEAN; /* 无变更早退：目录检查点每写一条都消耗池内槽位，空转会让池无谓轮转 */
    }

    // 单飞：CP job 与 metadata job 都会写目录检查点，两者共享
    // `dir_ckpt_sequence` / `dir_ckpt_write_idx`。并发 prepare 会把两个不同 sequence 的
    // 记录写进**同一个槽**（16 页按时间交错）⇒ 整条记录校验失败 ⇒ 开机 locate 只能
    // 回退到更旧的记录 ⇒ 目录陈旧 ⇒ 指向已回收的 L2P 日志块。同一时刻只允许一个
    // writer 处于 prepare→commit 之间。
    if (dir->ckpt.busy == true)
    {
        return MD_DIR_CKPT_BUSY;
    }

    // 记录本次目录检查点覆盖的变更代数：写入期间若有新的 commit，commit 时不得把
    // dirty 清掉（否则 on-media 目录检查点落后于 RAM Directory，旧块可能被误回收）。
    dir->ckpt.covered_gen = dir->ckpt.dirty_gen;

    md_dir_ckpt_ring_init(dir, &out->ring);

    // 游标未建立（冷启动后首次 flush）⇒ 先定位池内最新有效 record 的下一槽
    if (dir->ckpt.cursor_valid == false)
    {
        dir_ckpt_probe_ctx_s ctx;
        md_ring_locate_out_s loc;

        ctx.dir  = dir;
        ctx.ring = &out->ring;

        if (md_ring_locate(&out->ring, md_dir_ckpt_record_probe, &ctx, &loc) != FTL_OK)
        {
            return MD_DIR_CKPT_FAILED;
        }
        dir->ckpt.sequence     = loc.sequence;
        dir->ckpt.write_idx    = loc.write_idx;
        dir->ckpt.cursor_valid = true;
    }

    dir->ckpt.sequence++;
    out->idx = dir->ckpt.write_idx;
    out->seq = dir->ckpt.sequence;

    if (md_ring_prepare_addr(&out->ring, &dir->topo, out->idx, &out->base, &out->need_erase) != FTL_OK)
    {
        return MD_DIR_CKPT_FAILED;
    }

    dir->ckpt.erase_required = out->need_erase; /* abort 时块首槽要跳过整块 */
    dir->ckpt.busy           = true;            /* 单飞：commit / 自己 abort 才释放 */
    dir->ckpt.owner          = owner;

    return MD_DIR_CKPT_READY;
}

/**
 * @brief 组一页目录检查点
 *
 * padding 归零 ⇒ 介质内容确定；有效条目数 = 全局有效范围与本页区间的交集。
 *
 * @param dir   Directory
 * @param seq   本 record 的 sequence
 * @param chunk 页序
 * @param page  输出：一页
 */
void md_dir_ckpt_page(const md_dir_cache_s *dir, DWORD seq, BYTE chunk, md_dir_ckpt_page_s *page)
{
    // chunk 0..PRIMARY-1 承载主 PBA；PRIMARY..COUNT-1 承载 backup_pba[]（同一 id 区间）。
    const BOOL backup      = (chunk >= DIR_CKPT_DIR_PRIMARY_PAGES);
    const BYTE local_chunk = backup ? (BYTE)(chunk - DIR_CKPT_DIR_PRIMARY_PAGES) : chunk;

    const WORD base_entry = (WORD)(local_chunk * METADATA_DIR_ENTRIES_PER_PAGE);
    WORD       n;

    if (base_entry >= dir->count)
    {
        n = 0U;
    }
    else
    {
        n = (dir->count - base_entry);
        if (n > METADATA_DIR_ENTRIES_PER_PAGE)
        {
            n = METADATA_DIR_ENTRIES_PER_PAGE;
        }
    }

    memset(page, 0, sizeof(*page));
    page->magic          = DIR_CKPT_MAGIC;
    page->sequence       = seq;
    page->chunk_index    = chunk;
    page->chunk_count    = DIR_CKPT_PAGE_COUNT;
    page->entry_count    = n;
    page->layout_version = FTL_MEDIA_LAYOUT_VERSION;
    // 填充时刻的目录代数：整条 record 的 16 页必须同代，否则加载侧作废
    // 整条记录。页在写的那一刻才取值 ⇒ 写入期间若有 commit，这一页与先写的页代数不同。
    page->dirty_gen      = dir->ckpt.dirty_gen;

    // 有效条目：本页区间与全局有效范围 [0,count) 的交。
    for (WORD k = 0U; k < n; k++)
    {
        const WORD entry_idx = (WORD)(base_entry + k);

        page->entries[k] = dir->entries[entry_idx];
        if (backup)
        {
            // 备份半：type/id 沿用主条目，pba 换成本 region 的第二副本位置。
            page->entries[k].pba = dir->backup_pba[entry_idx];
        }
    }

    // 尾部 padding：⚠ memset 只给 0，不能替代这里的 NAND_PBA_INVALID 哨兵——
    // 否则会被当成"指向 block 0"的映射。
    for (WORD k = n; k < METADATA_DIR_ENTRIES_PER_PAGE; k++)
    {
        page->entries[k].type = 0U;
        page->entries[k].id   = 0U;
        page->entries[k].pba  = NAND_PBA_INVALID;
    }
}

/**
 * @brief 提交目录检查点：推进游标、按代际清 dirty
 *
 * 无论成败都推进游标（半编程槽不可复用）；ok == false 或写入期间 dirty_gen
 * 又前进时保留 dirty，下次在新槽重试整条。
 *
 * @param dir Directory
 * @param w   共享上下文（begin 填充）
 */
void md_dir_ckpt_commit(md_dir_cache_s *dir, md_dir_ckpt_write_s *w)
{
    dir->ckpt.write_idx = (WORD)((w->idx + 1U) % md_ring_total_records(&w->ring));

    if (w->ok && (dir->ckpt.dirty_gen == dir->ckpt.covered_gen))
    {
        dir->ckpt.dirty = false;
    }

    dir->ckpt.busy           = false; /* 单飞释放（成败都释放：残缺记录不会再被续写） */
    dir->ckpt.erase_required = false;
    dir->ckpt.owner          = MD_DIR_CKPT_OWNER_NONE;

    // 本次写已收尾：清掉上下文持有者，后续 abort 必须是 no-op（否则会再推进一次游标）。
    w->owner = MD_DIR_CKPT_OWNER_NONE;
}

/**
 * @brief 释放目录检查点单飞锁
 *
 * 只释放自己持有的那一个；别人的超时中止不能清本 writer 的锁。
 *
 * @param dir   Directory
 * @param owner 持有者
 */
SSD_CODE_FAST void md_dir_ckpt_abort(md_dir_cache_s *dir, BYTE owner)
{
    // 只释放**自己**的锁：另一个 job 的超时中止不能把本 writer 的单飞清掉。
    if (dir->ckpt.owner != owner)
    {
        return;
    }

    dir->ckpt.erase_required = false;
    dir->ckpt.busy           = false;
    dir->ckpt.owner          = MD_DIR_CKPT_OWNER_NONE;
}

/**
 * @brief 开始一次目录检查点 record 写入（prepare + 填充共享上下文）
 *
 * @param dir   Directory
 * @param w     共享上下文（出参）
 * @param owner 单飞持有者
 * @return md_dir_ckpt_step_e
 */
md_dir_ckpt_step_e md_dir_ckpt_write_begin(md_dir_cache_s *dir, md_dir_ckpt_write_s *w, BYTE owner)
{
    md_dir_ckpt_prepare_out_s out;
    const md_dir_ckpt_step_e  r = md_dir_ckpt_prepare(dir, &out, owner);

    if (r == MD_DIR_CKPT_READY)
    {
        w->ring       = out.ring;
        w->base       = out.base;
        w->seq        = out.seq;
        w->idx        = out.idx;
        w->chunk      = 0U;
        w->ok         = true;
        w->need_erase = out.need_erase;
        w->skip       = false;
        w->owner      = owner;
    }
    return r;
}

/**
 * @brief 一页写完：结算 ok/chunk，返回整条 record 是否写完
 *
 * @param w      共享上下文
 * @param failed 本页是否失败
 * @return true = 16 页已全部处理完；false = 还需写下一页
 */
BOOL md_dir_ckpt_write_advance(md_dir_ckpt_write_s *w, BOOL failed)
{
    if (failed)
    {
        w->ok = false;
    }
    w->chunk = (BYTE)(w->chunk + 1U);
    return (w->chunk >= DIR_CKPT_PAGE_COUNT);
}

/**
 * @brief 中止共享上下文持有的目录检查点单飞
 *
 * @param dir Directory
 * @param w   共享上下文
 */
void md_dir_ckpt_write_abort(md_dir_cache_s *dir, md_dir_ckpt_write_s *w)
{
    // owner=NONE 有两种合法来源：从未 begin，或已 commit 收尾 ⇒ 都不该再动游标。
    if (w->owner == MD_DIR_CKPT_OWNER_NONE)
    {
        return;
    }

    // 中止的那条 record 可能只写了半条：继续用同一槽会在未擦页上二次编程。
    // 用 **begin 时的 ring 几何**推进游标（运行期池可能已被 md_pool_build_all 重建，
    // 现取 pools 会拿到新块表、算错旧游标对应的块）。
    const WORD rpb   = md_ring_records_per_block(&w->ring);
    const WORD total = md_ring_total_records(&w->ring);

    if (w->need_erase)
    {
        // 块首槽要擦除：中止可能发生在擦除失败或擦除后写了一半 ⇒ 整块作废，
        // 跳到下一块的槽 0。
        const WORD next_block = (WORD)(((dir->ckpt.write_idx / rpb) + 1U) % w->ring.block_count);

        dir->ckpt.write_idx = (WORD)(next_block * rpb);
    }
    else
    {
        // 块中槽：本块已在块首擦过，当前槽可能被写了一半；消费该槽即可。
        dir->ckpt.write_idx = (WORD)((dir->ckpt.write_idx + 1U) % total);
    }

    md_dir_ckpt_abort(dir, w->owner);
    w->owner = MD_DIR_CKPT_OWNER_NONE;
}

/**
 * @brief 读胜者的全部页，逐页校验后覆盖对应区间。
 * 镜像池：每页先读首选 plane；该页不可信时回退到另一个 plane（sequence 必须仍等于
 * 被选中的 record sequence，绝不跨 record 拼页）。
 *
 * @param dir    Directory cache（输出：被覆盖的 entries / backup_pba）
 * @param ring   池几何
 * @param newest 胜出 record 的槽位号
 */
static SSD_CODE_FAST void md_load_assemble(md_dir_cache_s *dir, const md_ring_s *ring, WORD newest)
{
    md_dir_ckpt_page_s page;
    const BYTE         plane_count = md_ring_plane_count(ring->plane_mask);
    DWORD              dgen        = 0U; /* chunk 0 主半的目录代数*/
    BOOL               dgen_set    = false;

    for (BYTE chunk = 0U; chunk < DIR_CKPT_PAGE_COUNT; chunk++)
    {
        const BOOL backup      = (chunk >= DIR_CKPT_DIR_PRIMARY_PAGES);
        const BYTE local_chunk = backup ? (BYTE)(chunk - DIR_CKPT_DIR_PRIMARY_PAGES) : chunk;
        const WORD base_entry  = (WORD)(local_chunk * METADATA_DIR_ENTRIES_PER_PAGE);

        for (BYTE pi = 0U; pi < plane_count; pi++)
        {
            const BYTE plane = md_ring_plane_at(ring->plane_mask, pi);
            DWORD      addr  = md_ring_addr_plane(ring, &dir->topo, newest, chunk, plane);

            if (ftl_sync_read_page(addr, &page, sizeof(page), FTL_PAGE_CFG_META()) != 0)
            {
                continue;
            }
            const BOOL header_ok = md_dir_ckpt_header_valid(page.magic, page.chunk_count, page.layout_version);

            if ((header_ok == false) || (page.sequence != dir->ckpt.sequence) || (page.chunk_index != chunk))
            {
                continue; /* 该副本不可信：试另一个 plane；都失败则保留基础层的"未映射" */
            }

            // 防御性二次校验：主半各页必须同代。探针已整条校验过，
            // 这里再挡一次 —— 混代页宁可不加载（回落"未映射"），也不能把两个代次的
            // 条目拼成一份目录。
            if (backup == false)
            {
                if (dgen_set == false)
                {
                    dgen     = page.dirty_gen;
                    dgen_set = true;
                }
                else if (page.dirty_gen != dgen)
                {
                    continue;
                }
            }

            // entry_count 来自介质（不可信）：先夹到本页容量再窄化到 WORD。
            const WORD n = (WORD)((page.entry_count < METADATA_DIR_ENTRIES_PER_PAGE) ? page.entry_count : METADATA_DIR_ENTRIES_PER_PAGE);

            for (WORD k = 0U; k < n; k++)
            {
                const WORD entry_idx = (WORD)(base_entry + k);

                // entry_count 来自介质（不可信，H29）：越界即停。
                if (entry_idx >= METADATA_DIR_ENTRIES)
                {
                    break;
                }

                if (backup == false)
                {
                    // 主半同样不可信（H29）：只接受 id 与槽位一致的 L2P 条目，
                    // 否则保留 md_fixed_fill 的 INVALID 基础层。
                    if ((page.entries[k].type == METADATA_TYPE_L2P_REGION) && (page.entries[k].id == entry_idx))
                    {
                        dir->entries[entry_idx] = page.entries[k];
                    }
                }
                else if ((page.entries[k].type == METADATA_TYPE_L2P_REGION) && (page.entries[k].id == entry_idx))
                {
                    // 备份半 best-effort：缺页/损坏的页保留基础层的 INVALID（单副本运行）。
                    dir->backup_pba[entry_idx] = page.entries[k].pba;
                }
                else
                {
                    // 非本 region 的条目：忽略（介质输入不可信，H29）。
                }
            }
            break; /* 本页已从某个 plane 取到有效副本 */
        }
    }
}

/**
 * @brief 开机加载 Directory
 *
 * 先 fill_fixed 铺基础层（缺页/损坏回落未映射，无空洞），再 locate 最新目录
 * 检查点并逐页校验；定位失败/无有效记录 ⇒ 清游标返回错误，坏页保留基础层。
 *
 * @param dir          Directory
 * @param alloc        分配器
 * @param total_writes 累计写计数指针
 * @return FTL_OK；FTL_ERROR 无有效记录 / 读失败
 */
ftl_status_e md_load(md_dir_cache_s *dir, alc_s *alloc, const DWORD *total_writes)
{
    // 前置条件：唯一调用点传 ftl->metadata_dir / &ftl->alloc / &ftl->stats.total_writes。
    dir->alloc        = alloc;
    dir->total_writes = total_writes;

    // 目录检查点锁/擦除标记是 RAM 态；md_load 可能在 md_init 之前调用。
    dir->ckpt.busy           = false;
    dir->ckpt.erase_required = false;
    dir->ckpt.owner          = MD_DIR_CKPT_OWNER_NONE;
    dir->ckpt.write_idx      = 0U;
    dir->ckpt.cursor_valid   = false;

    // 在飞快照位是 RAM 态、不跨重启：上一次运行的写回全部随掉电终止。
    md_slot_reset(dir);

    // 基础层：全部 region = NAND_PBA_INVALID（未映射）。
    // 目录检查点缺页/损坏时该区间回落"未映射"，不会出现"未登记"的空洞。
    md_fixed_fill(dir);
    md_live_rebuild(dir); /* 失败路径也要给出确定的活计数（开机日志用）*/

    md_ring_s            ring;
    md_ring_locate_out_s loc;
    dir_ckpt_probe_ctx_s ctx;

    md_dir_ckpt_ring_init(dir, &ring);
    ctx.dir  = dir;
    ctx.ring = &ring;

    {
        const ftl_status_e status = md_ring_locate(&ring, md_dir_ckpt_record_probe, &ctx, &loc);

        dir->ckpt.sequence  = loc.sequence;
        dir->ckpt.write_idx = loc.write_idx;

        // 观测口（**INFO**，随开机必打印）：目录检查点读侧结论。
        // loc=0 且 newest=65535 ⇒ 池内一条有效 record 都没有（写没落盘 / 读不回来？）。
        // loc=0 且 newest<N ⇒ 找到了，看 seq 是否与写侧对得上。
        // ⚠ 必须 INFO：本函数在 ftl_init 里跑，早于 CLI ⇒ `log level debug` 是开机后
        // 才敲的，LOGD 在此处永远看不到。
        LOGI(LOG_TAG_META "dir_ckpt load: loc=%d newest=%hu seq=%lu\r\n", (int)status, loc.newest_idx, dir->ckpt.sequence);

        if ((status != FTL_OK) || (loc.newest_idx == UINT16_MAX))
        {
            dir->ckpt.sequence     = 0U;
            dir->ckpt.write_idx    = 0U;
            dir->ckpt.cursor_valid = false;
            dir->ckpt.dirty        = false;
            dir->ckpt.dirty_gen    = 0U;
            dir->ckpt.covered_gen  = 0U;
            md_boot_status_log(dir);
            return FTL_ERROR; /* 无有效目录检查点：调用者转 md_init */
        }
    }

    dir->ckpt.cursor_valid = true;

    md_load_assemble(dir, &ring, loc.newest_idx);

    dir->ckpt.dirty       = false;
    dir->ckpt.dirty_gen   = 0U;
    dir->ckpt.covered_gen = 0U;

    // 由装配后的 entries 重建活计数；**日志块游标不跨重启恢复**：首次追加时从**固定
    // L2P 日志区**新取一块（绝不复用可能已编程的旧页）。旧日志块仍被 Directory 引用 ⇒
    // 由 l2p_live 保住，待其 region 全部改写后由后台整理 job 异步擦除归还。
    md_live_rebuild(dir);
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        dir->cur.block[p] = 0U;
        dir->cur.page[p]  = 0U;
        dir->cur.valid[p] = false;
    }
    md_reclaim_reset(dir); /* 待回收位图不跨重启（同 job：全是 RAM 态）*/
    md_handle_reset(dir);  /* 整理 job 不跨重启（同游标：phase / 游标 / 请求位全是 RAM 态）*/

    md_boot_status_log(dir);

    return FTL_OK;
}

/* ══════════════════════════════════════════════════════════════════
 * 六、元数据块判定
 * ══════════════════════════════════════════════════════════════════ */

/**
 * @brief 判断物理块是否属于元数据
 *
 * 固定保留区 + 当前日志块 + 已登记日志块（l2p_live）；动态块必须带 plane
 * 比较，否则会掩掉另一 plane 的同号块。
 *
 * @param md_dir Directory（可 NULL = 只判固定分区）
 * @param plane  plane 号
 * @param block  该 plane 内的块号
 * @return true = 元数据块
 */
BOOL md_block_is_metadata(const md_dir_cache_s *md_dir, BYTE plane, WORD block)
{
    /* ── 固定元数据块：0..FTL_METADATA_AREA_END-1 **全部保留** ──
     * 包含 shared metadata spare；名义块即使 BAD 也不回归数据池（否则 BBT 记录的
     * 坏块会被 carve 当用户块再分出去）。这比逐分区区间判断更简单，且与候选池
     * 的物理块集合一致（C9：一个分类坐标）。 */
    if (block < FTL_METADATA_AREA_END)
    {
        return true;
    }

    // md_dir == NULL：只判固定分区（开机 carve / recovery 的"固定块"判据用）。
    // 这不是错误——调用者明确只要固定分区视图。
    if (md_dir == NULL)
    {
        return false;
    }

    /* ── 当前日志块：可能尚无任何 region 指向它（刚分配、还没 commit），
     *    Directory 扫不到 ⇒ 必须显式排除，否则会被 Free Pool 回填当用户块收走。 ── */
    if ((plane < PLANE_NUM) && (block < BLOCKS_PER_PLANE))
    {
        const WORD g_cur = ftl_gblock(plane, block);

        if (md_dir->cur.valid[plane] && (g_cur == md_dir->cur.block[plane]))
        {
            return true;
        }
    }

    /* ── 已登记日志块：l2p_live 是 Directory 条目的账本，O(1) 判定 ──
     * block 可能来自损坏介质：先挡掉越界值，避免 ftl_gblock 截断后误判。 */
    const BOOL in_range = (block < BLOCKS_PER_PLANE);
    const WORD g        = in_range ? ftl_gblock(plane, block) : FTL_BLOCK_TOTAL;
    const BOOL fast     = in_range && (g < FTL_BLOCK_TOTAL) && (md_dir->l2p_live[g] != 0U);

#ifndef NDEBUG
    // 差分自检：证明 O(1) 账本判据与 Directory 逐条扫描完全等价。release 编译期消失。
    // 扫描使用与 md_live_rebuild 相同的过滤条件（挡越界 / 固定块），
    // 避免损坏介质上的条目让调试版误报。
    BOOL slow = false;

    for (WORD i = 0U; i < md_dir->count; i++)
    {
        if (md_dir->entries[i].type != METADATA_TYPE_L2P_REGION)
        {
            continue;
        }

        // 主/备两份都参与差分（副本只占备份位置时，只扫主会让 fast != slow）。
        const WORD rid = md_dir->entries[i].id;

        for (BYTE copy = 0U; copy < L2P_REGION_COPIES; copy++)
        {
            DWORD pba;

            if (copy == 0U)
            {
                pba = md_dir->entries[i].pba;
            }
            else if (rid < METADATA_DIR_ENTRIES)
            {
                pba = md_dir->backup_pba[rid];
            }
            else
            {
                pba = NAND_PBA_INVALID;
            }

            if (pba == NAND_PBA_INVALID)
            {
                continue;
            }

            const BYTE ep = pba_to_plane(pba);
            const WORD eb = pba_to_block(pba);

            if ((ep >= PLANE_NUM) || (eb >= BLOCKS_PER_PLANE) || md_block_is_metadata(NULL, ep, eb))
            {
                continue;
            }
            if ((ep == plane) && (eb == block))
            {
                slow = true;
                break;
            }
        }

        if (slow)
        {
            break;
        }
    }
    ASSERT(fast == slow);
#endif

    return fast;
}

/* ══════════════════════════════════════════════════════════════════
 * 七、池诊断与服务
 * ══════════════════════════════════════════════════════════════════ */

/**
 * @brief 打印每个分区的候选池（nominal / 实际 / 被 spare 顶替）
 *
 * 只读 RAM，不碰介质。
 *
 * @param dir Directory
 */
void md_pool_print(const md_dir_cache_s *dir)
{
    if ((dir == NULL) || (dir->alloc == NULL) || (dir->alloc->pools == NULL))
    {
        LOGW(LOG_TAG_POOL "not initialized\r\n");
        return;
    }

    for (DWORD p = 0U; p < s_md_partition_count; p++)
    {
        const md_partition_s *part = &s_md_partitions[p];

        if (part->first_block == L2P_LOG_BLOCK0)
        {
            DWORD good = 0U;

            for (WORD block = L2P_LOG_BLOCK0; block < L2P_LOG_AREA_END; block++)
            {
                for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
                {
                    if (ftl_block_is_bad(&dir->alloc->blocks[ftl_gblock(plane, block)]) == false)
                    {
                        good++;
                    }
                }
            }

            LOGI(LOG_TAG_POOL "%-10s range=%u..%u good(plane-blocks)=%lu\r\n", part->name, L2P_LOG_BLOCK0, (L2P_LOG_AREA_END - 1U), good);
            continue;
        }

        const md_ring_s *ring = NULL;

        if (part->first_block == DIR_CKPT_BLOCK0)
        {
            ring = &dir->pools.dir_ckpt;
        }
        else if (part->first_block == CHECKPOINT_BLOCK0)
        {
            ring = &dir->pools.cp;
        }
        else if (part->first_block == BBT_BLOCK)
        {
            ring = &dir->pools.bbt;
        }
        else if (part->first_block == JOURNAL_BLOCK0)
        {
            ring = &dir->pools.jrn;
        }

        if (ring == NULL)
        {
            continue;
        }

        LOGI(LOG_TAG_POOL "%-10s nominal=%hu candidates=%hu\r\n", part->name, ring->nominal_count, ring->block_count);

        for (WORD i = 0U; i < ring->block_count; i++)
        {
            const WORD b = ring->blocks[i];

            if ((b < ring->first_block) || (b >= (ring->first_block + ring->nominal_count)))
            {
                LOGI(LOG_TAG_POOL "%-10s remapped -> spare block=%hu (candidate_idx=%hu)\r\n", part->name, b, i);
            }
        }
    }
}

/**
 * @brief 元数据候选池重算服务（ftl_service 第一步）
 *
 * 重算成功后作废目录检查点/BBT/CP 游标；失败则清脏位 + 置 rebuild_required，
 * 等下次开机兜底。必须排在本轮新 I/O 之前。
 *
 * @param ftl FTL 实例
 */
void md_pool_service(ftl_ctx_s *ftl)
{
    alc_s          *alloc   = &ftl->alloc;
    md_dir_cache_s *dir     = ftl->metadata_dir;
    l2p_jrn_s      *journal = ftl->journal;

    if ((alloc->pools == NULL) || (alloc->pools->dirty == false) || ftl->flags.bg_suspend)
    {
        return; /* bg_suspend：bbt --erase / format 期间 RAM 状态已不可信，不重建 */
    }

    // 等元数据写路径空闲：换游标可能让在飞命令的落点与新候选表不一致。
    // 单线程 superloop ⇒ 这里不会与完成回调并发；只是在下一轮再试。
    if (ftl->cp_handle->active || ftl->flush.active || ftl->mig_handle->job.active || (journal->handle.job.phase != JRN_IDLE) || alloc->bbt.handle.waiting || md_is_busy(dir))
    {
        return;
    }

    // 旧 Journal 候选表快照：只有它真的变了才复位环形游标（否则不应丢弃缓冲页）。
    WORD old_jrn[JOURNAL_BLOCK_COUNT];

    for (BYTE b = 0U; b < JOURNAL_BLOCK_COUNT; b++)
    {
        old_jrn[b] = journal->blocks[b];
    }

    if (md_pool_build_all(alloc) == false)
    {
        // 元数据区已无足够好块：停止重试（避免每轮刷屏），下次开机由 BBT/rebuild 兜底。
        alloc->pools->dirty         = false;
        alloc->bbt.rebuild_required = true;
        bbt_dirty_note(alloc);
        LOGE(LOG_TAG_POOL "rebuild failed — metadata area exhausted, rebuild required\r\n");
        return;
    }

    // 游标全部作废：下一次提交/定位会按新候选表重新扫池。
    dir->ckpt.cursor_valid                = false;
    alloc->bbt.cursor_valid               = false;
    ftl->cp_handle->cp_state.cursor_valid = false;

    // Journal 候选表变了 ⇒ 环形下标到物理块的映射已变。不能把游标简单归零
    // （新下标可能指向未擦块；直接写 = 二次编程）。按新候选表重新扫环、接回
    // 最新好页（jrn_page_scan_latest + jrn_resume），坏块上的条目由 REBUILD_REQUIRED
    // 在下次开机 full rebuild 恢复。
    BOOL jrn_changed = false;

    for (BYTE b = 0U; b < JOURNAL_BLOCK_COUNT; b++)
    {
        if (old_jrn[b] != alloc->pools->jrn.blocks[b])
        {
            jrn_changed = true;
        }
        journal->blocks[b] = alloc->pools->jrn.blocks[b];
    }

    if (jrn_changed)
    {
        jrn_page_ref_s latest;
        jrn_handle_s  *jrn        = &journal->handle;
        BOOL           read_error = false;

        // 旧下标的失败记录先清掉，否则 jrn_resume 会把新候选误判成坏块。
        jrn->state.bad_mask         = 0U;
        jrn->state.pre_erased_block = JRN_BLOCK_NONE;
        jrn->state.erase_fail_count = 0U;
        jrn->state.erase_fail_block = JRN_BLOCK_NONE;
        jrn->state.ring_full        = false; /* 新候选池可能已补回 spare */
        memset(jrn->job.page_buf, 0xFF, sizeof(jrn->job.page_buf));

        (void)jrn_page_scan_latest(journal, &latest, &read_error);

        // floor 取当前序号，保证 resume 不回退；merge_floor 取最近 CP 水位，
        // 阻止跨 CP 的旧条目就地合并。
        const DWORD floor = jrn->state.sequence_counter;

        jrn_resume(journal, &latest, floor);
        jrn->state.merge_floor = ftl->cp_handle->cp_state.last_journal_sequence;

        // 环里扫不到有效页时，jrn_resume 会把写点放到第一个可用块的 page0——
        // 该块在运行期不保证已擦（与开机 fresh 的假设不同）。挂起一次换块擦除，
        // 避免对未擦页二次编程。
        if (latest.entry_count == 0U)
        {
            jrn->job.target_block = jrn->state.current_block;
            jrn->job.need_erase   = true;
        }
    }

    // 任一 plane 的当前 L2P 日志块若已 BAD ⇒ 该 plane 下次 append 重新分配。
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        if (dir->cur.valid[p] && ftl_block_is_bad(&alloc->blocks[dir->cur.block[p]]))
        {
            dir->cur.valid[p] = false;
        }
    }

    // 触发一次目录检查点提交 + Checkpoint：把 RAM 里的权威状态重新落到新候选池。
    dir->ckpt.dirty = true;
    jrn_checkpoint_request(journal);
    bbt_dirty_note(alloc);

    if ((s_pool_rebuilt_logged++ % MD_POOL_REBUILT_LOG_EVERY) == 0U)
    {
        LOGW(LOG_TAG_POOL "rebuilt after metadata BAD (jrn_changed=%u rebuild_required=%u count=%lu)\r\n", (jrn_changed ? 1U : 0U), (alloc->bbt.rebuild_required ? 1U : 0U), s_pool_rebuilt_logged);
    }
}

/* ══════════════════════════════════════════════════════════════════
 * 九、L2P region 写回槽位账本
 *
 * ⚠ 本节的函数**全部标 `SSD_CODE_FAST`（内部 Flash）**：本文件其余部分整份进
 *   `.ssd_code_high`（ITCM，64 KB 硬上限），而这里每次 2 KiB 写回只调一次、
 *   不是热路径。函数级放置靠 `-ffunction-sections` + 链接脚本按段收集实现
 *   （`KEEP(*obj/ftl/metadata.o(.text .text.*))` 不含 `.ssd_code_fast`）。
 *
 * 不变量（单一来源）：`slot_resv[]` 的一项 = 一次「已发槽、尚未 commit/失败」的写回。
 * 这段时间该槽所在块**不得换出/回收**——槽里的数据还没进 Directory，`l2p_live`
 * 看不见它；只凭 `l2p_live == 0` 擦块会让写完成后的 commit 把 Directory 指向已擦页。
 *
 * 配对（H1）：`md_slot_pair_append` 成功一次，调用者必须恰好 `md_slot_release` 一次
 * （commit 成功，或建链/提交/写失败）。漏释放的方向是"块暂时不可回收"（容量损失，
 * 可见），不会写坏介质；重复释放由 debug 断言暴露。
 * ══════════════════════════════════════════════════════════════════ */

/**
 * @brief 复位在飞写回槽位预留表
 *
 * RAM 态，不跨重启；上一次运行的写回全部随掉电终止。
 *
 * @param dir Directory
 */
SSD_CODE_FAST void md_slot_reset(md_dir_cache_s *dir)
{
    for (BYTE i = 0U; i < MD_SLOT_RESV_MAX; i++)
    {
        dir->slot_resv[i] = NAND_PBA_INVALID; /* RAM 态，不跨重启 */
    }
}

/**
 * @brief 取一对写回槽位（主→plane0 当前块、备→plane1 当前块）
 *
 * 任一 plane 换不出块 ⇒ 背压，且未消耗任何槽。
 *
 * @param dir   Directory
 * @param alloc 分配器
 * @param out   输出：主/备 PBA
 * @return true 已取到；false 背压（未改状态）
 */
SSD_CODE_FAST BOOL md_slot_pair_append(md_dir_cache_s *dir, alc_s *alloc, DWORD out[L2P_REGION_COPIES])
{
    // 前置条件：调用者传 ftl->metadata_dir / &ftl->alloc / 栈上数组（调用链已保证）。
    if (md_handle_is_idle(dir) == false)
    {
        return false; /* 整理 job 在飞：背压语义（见 md_log_rotate 的 job 让路说明） */
    }

    // 预算：主副本 → plane0 当前块、备副本 → plane1 当前块，**每个 plane 各需 1 页**。
    // 先逐 plane 确认（必要时换该 plane 的块）：任一 plane 换不出块 ⇒ 背压，
    // **未消耗任何槽**（两副本不会出现"取到一个、另一个失败"的半对）。
    for (BYTE p = 0U; p < L2P_REGION_COPIES; p++)
    {
        if (p >= PLANE_NUM)
        {
            return false; /* 编译期断言保证不可达 */
        }

        if (dir->cur.valid[p])
        {
            const bst_block_s *b = &alloc->blocks[dir->cur.block[p]];

            if (ftl_block_is_bad(b))
            {
                dir->cur.valid[p] = false; /* 块被标坏：游标作废，下面换块 */
            }
        }

        if ((dir->cur.valid[p] == false) || ((WORD)dir->cur.page[p] + 1U > PAGES_PER_BLOCK))
        {
            if (md_log_rotate(dir, alloc, p) == false)
            {
                return false;
            }
        }
    }

    // 在飞快照位：主/备**各登记一条**（两个副本落在不同 plane 的块上，各自都要防误擦）。
    // 表容量 = 写回并发上限 × 副本数（l2p_cache.c 静态断言）。**先确认有足够空位**，
    // 且必须在 md_slot_take 之前：容量不足时不能白消耗两个当前块的页。
    {
        BYTE free_n = 0U;

        for (BYTE i = 0U; i < MD_SLOT_RESV_MAX; i++)
        {
            if (dir->slot_resv[i] == NAND_PBA_INVALID)
            {
                free_n++;
            }
        }
        if (free_n < L2P_REGION_COPIES)
        {
            return false; /* 背压；未取任何槽 */
        }
    }

    for (BYTE p = 0U; p < L2P_REGION_COPIES; p++)
    {
        out[p] = md_slot_take(dir, p);
        if (out[p] == NAND_PBA_INVALID)
        {
            return false; /* 预检保证不可达；已取到的槽作废（不登记 = 不会被误擦） */
        }
    }

    {
        for (BYTE p = 0U; p < L2P_REGION_COPIES; p++)
        {
            for (BYTE i = 0U; i < MD_SLOT_RESV_MAX; i++)
            {
                if (dir->slot_resv[i] == NAND_PBA_INVALID)
                {
                    dir->slot_resv[i] = out[p];
                    break;
                }
            }
        }
    }

    return true;
}

/**
 * @brief 释放在飞写回槽位（与 pair_append 严格配对）
 *
 * 期望恰好释放主/备两条；少/多由 ASSERT 暴露。
 *
 * @param dir     Directory
 * @param primary 主副本 PBA
 * @param backup  备副本 PBA
 */
SSD_CODE_FAST void md_slot_release(md_dir_cache_s *dir, DWORD primary, DWORD backup)
{
    BYTE released = 0U;

    for (BYTE i = 0U; i < MD_SLOT_RESV_MAX; i++)
    {
        if ((dir->slot_resv[i] == primary) || (dir->slot_resv[i] == backup))
        {
            dir->slot_resv[i] = NAND_PBA_INVALID;
            released++;
        }
    }

    // 期望恰好两条（主 + 备）。少 = 重复释放/漏登记，多 = 传错；账本其余项不动。
    ASSERT(released == L2P_REGION_COPIES);
}

/**
 * @brief 换指定 plane 的当前日志块
 *
 * 旧块无活 region 才登记回收；不在此擦介质。
 *
 * @param dir   Directory
 * @param alloc 分配器
 * @param plane 目标 plane
 * @return true 已换块；false 无可用块 / 背压
 */
SSD_CODE_FAST BOOL md_log_rotate(md_dir_cache_s *dir, alc_s *alloc, BYTE plane)
{
    if (plane >= PLANE_NUM)
    {
        return false;
    }

    // 该 plane 的当前块已写满且无活 region：先从"当前写块"身份上摘下来，否则
    // mark_all 会把它永远排除；日志区没有空闲块时就再也换不了块（死等）。
    // ⚠ 有在飞写回的槽位时不能摘身份：槽里的数据还没进 Directory，l2p_live 为 0
    //   不代表块已空——摘掉后它会被 mark_all 登记并擦除。
    if (dir->cur.valid[plane] && (dir->cur.page[plane] >= PAGES_PER_BLOCK) && (dir->l2p_live[dir->cur.block[plane]] == 0U))
    {
        const WORD gblock = dir->cur.block[plane];

        if (md_slot_reserved(dir, gblock) == false)
        {
            (void)md_reclaim_mark(dir, gblock);
            dir->cur.valid[plane] = false;
        }
    }

    // 换块时机扫一遍全池：只**登记**零活 LOG 块，绝不在这里擦。
    // 擦除由后台 job 异步做——同步 tERASE ~3 ms 会击穿服务预算。
    md_reclaim_mark_all(dir, alloc);

    // 只剩最后 1 个空闲块：先登记压缩再把它用作当前块——新块有整块空槽，
    // 压缩才有目标可搬。free 水位判据见 L2P_LOG_FREE_WATERMARK。
    if (md_log_area_free_count(alloc) <= 1U)
    {
        dir->handle.compact_wanted = true;
    }

    // 只从**本 plane 的固定 L2P 日志区**取块（md_log_alloc），不碰用户 Free Pool。
    const WORD gblock = md_log_alloc(dir, alloc, plane);

    if (gblock == FTL_GBLOCK_INVALID)
    {
        // 该 plane 的固定日志区无空闲块：所有块都是活/当前/待擦/BAD。**必须请求压缩**——
        // 只返回 false 而不置 compact_wanted 时，后台整理 job 无事可做，append_slot_pair
        // 永远返回 INVALID，CP 在 CP_FLUSH_REGIONS 每 10~25ms abort 一次、写不成 Header
        // ⇒ 掉电后每次开机全量重建（实测 `free=3241 cmd=0 buf=0` 仍 stall）。
        dir->handle.compact_wanted = true;
        LOGD(LOG_TAG_META "log area exhausted (plane=%hhu): live=%hu reclaim=%hu cur_page=%hhu — request compact\r\n", plane, md_live_block_count_get(dir), dir->reclaim.count, dir->cur.page[plane]);
        return false;
    }

    const WORD old_cur = dir->cur.block[plane];
    const BOOL had_cur = dir->cur.valid[plane];

    dir->cur.block[plane] = gblock;
    dir->cur.page[plane]  = 0U;
    dir->cur.valid[plane] = true;

    // 旧当前块可能已被搬空（压缩只搬活 region，零活块不会成为 victim）：
    // 换走后它再也不会被追加，登记给后台回收。
    // ⚠ 有在飞写回槽位时不能登记：槽里的数据还没进 Directory。
    if (had_cur && (dir->l2p_live[old_cur] == 0U) && (md_slot_reserved(dir, old_cur) == false))
    {
        (void)md_reclaim_mark(dir, old_cur);
    }

    // 抗碎片：刚换到新日志块 ⇒ 块内有整 PAGES_PER_BLOCK 个空槽，正好够把多个稀疏
    // victim 一次搬空。触发判据是**元数据空间水位**（live 日志块数），不是用户
    // Free Pool——旧实现只在 free≤32 时压，运行期大部分时间不压，日志碎片从理想
    // 32 块一路涨到上千块，把用户容量吃光。
    // 只**登记请求**，本次不发放槽位：调用者收到 NAND_PBA_INVALID 返回 FTL_AGAIN，
    // 下轮重试；若 job 已起则继续让路直到压缩走完。
    if (md_live_block_count_get(dir) > L2P_LOG_COMPACT_TRIGGER)
    {
        dir->handle.compact_wanted = true;
        return false;
    }

    return true;
}

/**
 * @brief 该 gblock 是否还有在飞写回槽位预留
 *
 * noinline 是放置约束：本文件其余部分在 ITCM，本函数留在 Flash 以省 ITCM。
 *
 * @param dir Directory
 * @param g   全局块号
 * @return true = 有预留（不得回收）
 */
SSD_CODE_FAST __attribute__((noinline)) BOOL md_slot_reserved(const md_dir_cache_s *dir, WORD g)
{
    if (g >= FTL_BLOCK_TOTAL)
    {
        return false;
    }

    for (BYTE i = 0U; i < MD_SLOT_RESV_MAX; i++)
    {
        const DWORD pba = dir->slot_resv[i];

        if ((pba != NAND_PBA_INVALID) && (ftl_pba_gblock(pba) == g))
        {
            return true;
        }
    }

    return false;
}

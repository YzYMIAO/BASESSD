/*
 * bbt.c — 坏块表（BBT）：出厂扫描 + 运行时标坏 + NAND 持久化 + Spare 池替换
 *
 * 职责：
 *   维护坏块表——开机从 NAND 读回 BBT（无则出厂扫描）、运行时标坏块、
 *   把坏块位图持久化到 NAND BBT 池（环形轮转）、坏块替换从 Spare Pool 补块。
 *
 * 与 bbt.h 的分工：
 *   bbt.h —— 结构（bbt_header_t）+ 位图存取原语（bbt_is_bad/bbt_set_bad）+ 尺寸宏；
 *   本文件 —— 扫描/标坏/持久化/替换的实现 + 坏块图打印诊断。
 *
 * 单一数据源：
 *   内存里坏块 = bst_block_t.state 的 BAD 位（BST 单数据源）；
 *   NAND 持久化仍用紧凑 bad_map[512] 格式（bbt_header_t，一字不改）；
 *   bbt_save 时从 BST 重建 bitmap，bbt_init 时从 bitmap 重建 BST。
 *
 * BBT 池（环形轮转，一条 record = 1 页，见 metadata_ring_*）：
 *   BBT_POOL_BLOCKS 块、P0+P1 镜像；游标/序号是**本池自己的**，开机由 bbt_load 恢复。
 *
 * 文件结构：
 *   1. 池几何 / 校验 / 探针
 *   2. 初始化 / 出厂扫描
 *   3. 格式化
 *   4. 标坏 / dirty
 *   5. 同步持久化（bbt_save / bbt_load）
 *   6. 异步 job（bbt_handle / ftl_bbt_callback）
 *   7. Spare 补池
 *   8. 诊断打印
 *
 * 两条持久化路径**刻意并存**：
 *   · `bbt_save`  —— **同步**，只给结构性同步上下文（开机出厂扫描 bbt_factory_scan、
 *                    全片格式化后的重落盘 power_failure_recovery）。此时 superloop
 *                    未建、无人 drain CQ，异步会死锁。
 *   · `bbt_handle`  —— **异步 job**，后台攒批（ftl_service 的 idle/normal 档）。
 *   两者写出的 record 格式与游标推进规则**逐字相同**。
 *
 * 游标/序号是 **per-instance**（收进 alc_t.bbt，见 bbt_state_t）——
 * 多实例（多 LUN）下每实例一份，互不干扰。
 */

#include "bbt.h"

#include "assertion.h"
#include "fc.h"
#include "fc_crc_engine.h" /* FC_CRC_META_SIZE（用户区抽样判空） */
#include "ftl_fc_interface.h"
#include "ftl_job.h" /* 心跳（[JOB] 观测口）*/
#include "ftl_op.h"  /* ftl_op_submit / FTL_OP_BBT */
#include "log.h"
#include "metadata.h" /* metadata_ring_*（BBT 池轮转原语）*/
#include "onfi_1_0.h"

#include <stdio.h>
#include <string.h>

// BBT 内部状态：**per-instance**，落在 alc_t.bbt（bbt_state_t）。
// 多实例（多 LUN）下每实例一份游标/序号，互不覆盖。
//
// 坏块状态存于 bst_block_t.state（Block State Table 单数据源；该字段是互斥枚举，
// BAD 为终态）。NAND 持久化用紧凑 bad_map[512] 格式（bbt_header_t，一字不改），
// bbt_save 时从 BST 重建 bitmap。
/* ── 模块常量 / 类型 / 文件级缓冲 ── */

// Spare/OP 双空日志节流：第 1 次立即打印，此后每 N 次一条（坏块风暴不刷屏）。
// N=64 是本仓观测折中：一次上电最多 64 条同类告警，足以判断趋势又不拖慢 UART。
#define BBT_RESERVE_LOW_LOG_EVERY (64U)
static DWORD s_reserve_low_logged;

// 探针/定位/写点共用的池 + 路由。
typedef struct
{
    ftl_topo_t topo;
    md_ring_t  ring;
} bbt_probe_ctx_t;

// 用户区抽样点数（每 plane）。见 bbt_medium_has_ftl_data：只在 BBT 载入失败时
// 判「介质是否被写过」，抽样而非全扫以控制开机路径成本。
#define BBT_USER_SAMPLE_COUNT (8U)

// 定义在下方「坏块图打印」节（与 bbt_print_bad_map 共用）；bbt_factory_scan 需要它在前面，故先声明。
static bool bbt_probe_block_bad(alc_t *alloc, BYTE plane, WORD block, BYTE do_erase);

// 一排每 plane 的块数。受**终端宽度**约束：每格定宽 BBT_MAP_ENTRY_W，两 plane 并排
// ⇒ 行长 = 2 × (n × (ENTRY_W+1) − 1) + 3，再加 "[INF] " 前缀。取 8 ⇒ 87 字符，
// 在 ~96 列的终端上不折行（取 16 会到 167，必折）。
#define BBT_MAP_COLS_PER_PLANE (8U)
#define BBT_MAP_ENTRY_W        (4U)                                                   /* 每格固定宽：最大块号 "2047" 也放得下 */
#define BBT_MAP_GROUP_W        (BBT_MAP_COLS_PER_PLANE * (BBT_MAP_ENTRY_W + 1U) - 1U) /* 一组宽度：n 个定宽格 + n-1 空格 */
#define BBT_MAP_LINE_MAX       (PLANE_NUM * (BBT_MAP_GROUP_W + 3U) + 1U)

_Static_assert((BLOCKS_PER_PLANE % BBT_MAP_COLS_PER_PLANE) == 0U, "每 plane 块数须被每排列数整除，否则末排漏块");

/* ── BBT 池：环形轮转（一条 record = 1 页）──
 * 候选池已建立时用它（跳过 BAD）；`bbt_init` 自举期间池尚未建立 ⇒ 退回
 * 「名义 BBT 块 + 全部 shared spare」的物理扫描列表，record 自带 magic/sequence，
 * 坏块/空块读失败会被 md_ring_locate 自然忽略。 */

/* ── 1. 池几何 / 校验 / 探针 ── */

/**
 * @brief 建立本实例的 BBT 池轮转视图
 *
 * 候选池已建立（`alloc->pools->bbt` 至少 2 块）时直接取用；否则退回自举池
 * （`md_pool_build_bbt_bootstrap`），供池尚未建立的初始化路径扫描。
 *
 * @param ring  输出：BBT 池轮转描述
 * @param alloc Allocator 上下文（取 `alloc->pools->bbt`；NULL 时走自举池）
 */
static void bbt_ring_init(md_ring_t *ring, const alc_t *alloc)
{
    if ((alloc != NULL) && (alloc->pools != NULL) && (alloc->pools->bbt.block_count >= 2U))
    {
        *ring = alloc->pools->bbt;
        return;
    }

    md_pool_build_bbt_bootstrap(ring);
}

/**
 * @brief 整条校验（一条 record = 1 页）：magic + total_blocks。
 *
 * total_blocks 是布局兼容性的代理判据：换块数/换 bitmap 语义后旧 record 必须作废。
 * 无本层 CRC——页完整性由下层 FC 的 per-CW CRC + ECC 保证。
 *
 * @param hdr record header
 * @return true 有效，false 无效
 */
static bool bbt_header_valid(const bbt_header_t *hdr)
{
    return (hdr->magic == BBT_MAGIC) && (hdr->total_blocks == BBT_MAX_BLOCKS);
}

/**
 * @brief 装配本实例的池 + 路由（探针/定位/写点都要这一对，四处共用）
 *
 * @param alloc Allocator 上下文（取实例路由与候选池）
 * @param ctx   输出：池轮转描述 + 拓扑路由
 */
static void bbt_ctx_init(const alc_t *alloc, bbt_probe_ctx_t *ctx)
{
    bbt_ring_init(&ctx->ring, alloc);
    ctx->topo = alc_topo(alloc);
}

/**
 * @brief BBT record 探针（见 metadata_ring_probe_fn）
 *
 * full 与廉价探针等价（record 只有一页）。镜像池：任一 plane 的副本
 * header 有效且 sequence 非 0 即命中。
 *
 * @param ctx          探针上下文（`bbt_probe_ctx_t *`：池 + 路由）
 * @param record_idx   池内槽下标
 * @param full         是否完整探测（本 record 只有一页，忽略）
 * @param out_sequence 输出：命中 record 的 sequence
 * @return FTL_OK 命中；FTL_ERROR 该槽无有效副本
 */
static ftl_status_e bbt_record_probe(void *ctx, WORD record_idx, bool full, DWORD *out_sequence)
{
    (void)full;

    bbt_probe_ctx_t *c = (bbt_probe_ctx_t *)ctx;

    // 镜像池：任一 plane 的副本有效即可。
    for (BYTE pi = 0U; pi < md_ring_plane_count(c->ring.plane_mask); pi++)
    {
        const BYTE plane = md_ring_plane_at(c->ring.plane_mask, pi);
        DWORD      addr  = md_ring_addr_plane(&c->ring, &c->topo, record_idx, 0U, plane);

        bbt_header_t hdr;
        if (ftl_sync_read_page(addr, &hdr, sizeof(hdr), FTL_PAGE_CFG_META()) != 0)
        {
            continue;
        }
        if ((bbt_header_valid(&hdr) == false) || (hdr.sequence == 0U))
        {
            continue; /* sequence 从 1 起；0 = 未写 */
        }

        *out_sequence = hdr.sequence;
        return FTL_OK;
    }

    return FTL_ERROR;
}

/* ── 2. 初始化 / 出厂扫描 ── */

// 实现要点：判据 = **分区表里每个固定分区实际使用的 plane（plane_mask）中，只要
// 有一页**不是擦除页**（`ftl_page_blank_check`：FC 判 ECC 是否成码字 + 整条 CW 的
// 1 比特占比）⇒ 介质上有 FTL 写过的页。
// · 空片：该区每页都是擦除页 ⇒ 返回 false。
// 判据放在**解码路径**，不在 RAW 读上：RAW 读（LDPC_RAW）刻意跳过 ECC，空/非空的
// 依据（ECC 是否成码字、parity 是否写成）全被跳过，FTL 又只看得到 USER 512B
// ⇒ 「RAW 读 + 数 FF」这种组合判不出「编程过没有」（RISK-IF-15）。
// · 读不通的页（出厂坏块块）⇒ `ftl_page_blank_check` 返回 FTL_ERROR ⇒ 跳过，
//   不参与「写过」判定，方向与"坏块块不是写过"一致。
// · 与 FTL 的**布局/格式版本无关**：FC 层的 CW 结构（META/CRC/ECC）不随
// FTL 版本变，故版本升级后的介质照样认得出——这正是必须覆盖的场景。
// ⚠ 只读不写；只在 bbt_load 失败时才调用（正常开机不付这份代价）。
// ⚠ 只探**固定元数据区**：该区的页一律以 FTL_PAGE_CFG_META()（RANDOM_DISABLE）写，
// 与这里的读配置无关（判空只看 ECC 与位占比）；用户数据页不在此列、也不需要。
bool bbt_medium_has_ftl_data(alc_t *alloc)
{
    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        for (WORD block = 0U; block < FTL_METADATA_AREA_END; block++)
        {
            if (md_block_is_metadata(NULL, plane, block) == false)
            {
                continue; /* 该 plane 的这个块号不是固定元数据（可能是用户数据） */
            }

            const DWORD addr  = pba_make(alloc->ch, alloc->ce, alloc->lun, block, plane, 0U, 0U);
            bool        blank = false;

            if ((ftl_page_blank_check(addr, &blank) == FTL_OK) && (blank == false))
            {
                return true;
            }
        }
    }

    // 固定元数据区被擦、用户区仍有数据：只查元数据区会把介质误判为空片，
    // 随后的出厂扫描会覆盖已失效的坏块图（RISK-BBT-05）。抽样用户区 Page 0，
    // 用 `ftl_page_is_blank` 判「是否从未编程」——抽样而不是全扫：本函数只在
    // BBT 载入失败时执行，全扫 4096 块 × 一次 NAND 读在开机路径上不可接受。
    // 残留风险：用户数据恰好全部落在抽样点之外 ⇒ 仍会误判；概率低，且该介质
    // 已丢失元数据，属于灾后场景。
    {
        const WORD start = FTL_METADATA_AREA_END;

        if (start < BLOCKS_PER_PLANE)
        {
            const WORD span = (BLOCKS_PER_PLANE - start);
            WORD       step = (span / BBT_USER_SAMPLE_COUNT);

            if (step == 0U)
            {
                step = 1U;
            }
            for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
            {
                for (WORD block = start; block < BLOCKS_PER_PLANE; block = (block + step))
                {
                    const DWORD addr = pba_make(alloc->ch, alloc->ce, alloc->lun, block, plane, 0U, 0U);

                    // 判空走解码路径（ECC 是否成码字 + 整条 CW 的 1 比特占比），
                    // 不是「RAW 读数 FF」：RAW 读跳过 ECC，也看不到 META/parity。
                    bool blank = false;

                    if ((ftl_page_blank_check(addr, &blank) == FTL_OK) && (blank == false))
                    {
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

// 实现要点：读到 BBT 后从 bitmap 重建 BST 的 BAD 位并清 OPEN/CLOSED/FREE/sb_id
// （单数据源方向：NAND bitmap → BST）。
void bbt_init(alc_t *alloc)
{
    alloc->bbt.sequence           = 0U;
    alloc->bbt.dirty              = false;
    alloc->bbt.dirty_gen          = 0U;
    alloc->bbt.format_in_progress = false;
    alloc->bbt.rebuild_required   = false;
    // 清异步 job（alc_init 已零化，此处再钉一次）
    alloc->bbt.handle.phase       = BBT_JOB_IDLE;
    alloc->bbt.handle.waiting     = false;

    // 尝试从 NAND 加载已存在的 BBT
    {
        BYTE tmp_bad_map[BBT_BITMAP_SIZE];

        DWORD seq;
        DWORD loaded_bad_count = 0U;
        int   ret              = bbt_load(alloc, tmp_bad_map, &loaded_bad_count, &seq);
        if (ret == 0)
        {
            WORD bad_idx = 0U;

            alloc->bbt.sequence = seq;

            // 从 NAND bitmap 重建 BST BAD 位（单数据源）；bad_count 以重建结果为准，
            // 不信任 record 里的冗余字段。
            for (WORD gblock = 0U; gblock < BBT_MAX_BLOCKS; gblock++)
            {
                if (bbt_is_bad(tmp_bad_map, gblock))
                {
                    ftl_block_set_bad(&alloc->blocks[gblock]); /* 终态：互斥，无需清位 */
                    ftl_block_clear_valid(&alloc->blocks[gblock]);
                    ftl_block_set_sb(&alloc->blocks[gblock], FTL_BLOCK_NO_SB);
                    bad_idx++;
                }
            }
            alloc->bad_count = bad_idx;

            return;
        }
    }

    // 无有效 BBT → **只有空片**才允许出厂扫描。
    //
    // ⚠ 为什么必须拦：用过的块，其 Page 0 的 spare 首字节（列 2048）**不是**出厂标记，
    // 而是 FTL 自己的数据——FC 一次页编程写 PAGE_SIZE = 2112（主区 2048 + 备区 64），
    // 页缓冲按 4 × CW(528B) 拼 ⇒ CW3 的尾巴 64 字节正好落进备区，覆盖掉出厂标记。
    // 于是「列 2048 非 0xFF ⇒ 坏块」这条判据在用过的介质上退化成「哪些块被写过」，
    // 重扫会把**所有写过的块**（含活的 Anchor/Checkpoint/Wear）判成坏块，随后
    // bbt_save 把这份垃圾落盘 —— 真坏块反而丢了，活块全废。
    //
    // 判据 = bbt_medium_has_ftl_data()（元数据区里有没有合法的 FTL 页）。
    // 空片路径不受影响：那时列 2048 确实就是出厂标记。
    //
    // 拒绝扫描后的状态：坏块表从空开始（bad_count = 0、sequence 仍 0、**不置 dirty**
    // ⇒ 不落盘，免得把「零坏块」写成既成事实）。真坏块由运行时 bbt_mark_bad 兜底
    // （PROGRAM/ERASE 失败即标）。
    if (bbt_medium_has_ftl_data(alloc))
    {
        LOGE(LOG_TAG_BBT "无有效 BBT，但介质已被格式化过 —— **拒绝出厂扫描**：\r\n");
        LOGE("      用过的块 spare[0] 是 FTL 数据而非出厂标记，重扫会把写过的块全判坏。\r\n");
        LOGE("      坏块表从空开始；真坏块由运行时 bbt_mark_bad 兜底。\r\n");

        alloc->bad_count = 0U;
        return;
    }

    // 空片 → 执行出厂扫描。坏块数只写 alloc->bad_count（H38：恒常量的返回码改 void）。
    (void)bbt_factory_scan(alloc);
}

// 实现要点：判据 = **OOB 出厂标记**（`bbt_probe_block_bad(..., do_erase=0)`），
// 与器件手册的建 BBT 流程一致（见 bbt_probe_block_bad 的引文）。
// 跳过 block 0（出厂总是好块）；扫描完置 dirty 并 bbt_save 落盘。
//
// ⚠ **前置条件：介质必须是空片**（`bbt_init` 用 `bbt_medium_has_ftl_data` 把关）。
// 用过的块 Page 0 的 spare 首字节是 FTL 自己的数据（页编程 2112B 把 CW3 尾巴 64
// 字节溢进备区），不是出厂标记 ⇒ 在用过的介质上扫出来的不是坏块图，而是
// 「哪些块被写过」图。本函数自己**判不了**这件事（它读的就是那个字节），
// 判据在调用方。
//
// ⚠ 判据不能用 raw 读返回码或“数据全 FF”：raw 跳过 ECC，物理失败页仍回
// READY；全 FF 既可能是空块也可能是坏块。唯一成立的判据是
// OP_READ_COLUMN 读 spare[0]（列 2048）。
int bbt_factory_scan(alc_t *alloc)
{
    WORD bad_idx = 0U;

    alloc->bbt.dirty = false;

    for (WORD gblock = 0U; gblock < BBT_MAX_BLOCKS; gblock++)
    {
        // 跳过 Block 0（出厂总是好块）
        if (gblock == 0U)
        {
            continue;
        }

        // 全局块号 → 各 Plane 的物理块号
        BYTE plane = ftl_gblock_plane(gblock);
        WORD block = ftl_gblock_block(gblock);

        // 只读标记（do_erase=0）：出厂扫描阶段**不得**擦块——擦除会永久毁掉标记
        if (bbt_probe_block_bad(alloc, plane, block, 0U))
        {
            ftl_block_set_bad(&alloc->blocks[gblock]); /* 终态：互斥，无需清位 */
            ftl_block_clear_valid(&alloc->blocks[gblock]);
            ftl_block_set_sb(&alloc->blocks[gblock], FTL_BLOCK_NO_SB);
            bad_idx++;
        }
    }

    alloc->bad_count = bad_idx;
    bbt_dirty_note(alloc);

    // 先按扫描结果建好候选池（BBT 本身可能落在 shared spare），再写 BBT record；
    // 否则 bbt_save 会沿用自举扫描列表，可能选中刚扫出的坏块。
    if (md_pool_build_all(alloc) == false)
    {
        LOGE(LOG_TAG_BBT "factory scan: metadata candidate pool build failed — BBT not saved\r\n");
        return bad_idx;
    }

    // 将扫描结果写入 NAND BBT
    (void)bbt_save(alloc);

    return bad_idx;
}

/* ══════════════════════════════════════════════════════════════════
 * 坏块图打印（诊断；默认**只读**，加 --erase 才逐块擦除）
 * ══════════════════════════════════════════════════════════════════ */

/**
 * @brief 探测单个块是否坏块（**全仓唯一的「这块坏不坏」判据**）
 *
 * ① 读 Page 0 的 spare 首字节（**列 2048**，`PAGE_MAIN_SIZE`），非 0xFF ⇒ 出厂坏块；
 * **读失败也判坏块**（列读不通 = 该页不可读，保守归入坏块，与 ② 的擦除失败同调）。
 * **必须在擦除之前读**——器件手册（Micron AN1819）逐字：*"the bad block information is
 * erasable and cannot be recovered once erased"*，且 *"System software should check the
 * first spare area location on the first page of each block prior to performing any
 * PROGRAM or ERASE operations"*。标记**可擦除**、擦后不可恢复 ⇒ 必须先读后擦。
 * ② do_erase 非 0 时**无条件**擦除该块；擦除失败 ⇒ 坏块（stuck-at-0 / 不可擦，运行时坏块的子集）。
 * ⚠ 擦除**不能**因为 ① 命中就跳过——见下方函数体内的说明。
 *
 * 列 2048 只能靠 OP_READ_COLUMN（列地址走命令 param）够到：OP_READ_META 的列 =
 * cw × CW_SIZE（cw 取自 PBA 的 cw 字段），cw ∈ {0,1,2,3} ⇒ 列只能是 {0, 528, 1056, 1584}。
 * 4 字节 = spare[0..3]（2 字节坏块信息 + 2 字节 user metadata II），标记看 [0]。
 *
 * ⚠ **① 只在空片上才是「出厂标记」**。用过的块，spare 首字节是 FTL 自己的数据
 * （一次页编程写 2112B，CW3 尾巴 64 字节溢进备区）⇒ 本函数在用过的介质上回答的
 * 其实是「这块被写过没有」。故它**不可**用在任何会落盘的路径上，除非调用方已经
 * 确认介质是空片（bbt_medium_has_ftl_data）。
 *
 * 调用者：bbt_factory_scan（出厂扫描，do_erase=0，**调用方已确认空片**）/
 * bbt_print_bad_map（诊断，按 CLI——读图的人须知这不是坏块图）/
 * 注：bbt_format_erase **不**调用本函数（标记读取职责只在 bbt_init）。
 *
 * @param alloc    分配器（取 ch/ce/lun 与几何）。
 * @param plane    plane 号。
 * @param block    plane 内块号。
 * @param do_erase 非 0 = 探测时无条件擦除该块（擦除失败 ⇒ 判坏）。
 *
 * @return true = 坏块（列读失败 / 出厂标记非 0xFF / 擦除失败）。
 */
static bool bbt_probe_block_bad(alc_t *alloc, BYTE plane, WORD block, BYTE do_erase)
{
    // page / cw 恒 0：擦除不看它们；列地址由 ftl_sync_read_column 显式给，
    // 不依赖 PBA 的 cw 字段。LUN 必须取本分配器实例的坐标（多 LUN 下不能写 0）。
    DWORD addr = pba_make(alloc->ch, alloc->ce, alloc->lun, block, plane, 0U, 0U);

    // ① 出厂标记（擦除前读，擦除会永久毁掉它）。读失败 ⇒ 坏块。
    BYTE mark[4];
    (void)memset(mark, 0xFF, sizeof(mark));

    bool marked = (ftl_sync_read_column(addr, PAGE_MAIN_SIZE, mark) != FTL_OK) || (mark[0] != 0xFFU);

    // ② 可选擦除——**无条件发**。
    // 擦除不能因为 ① 命中就提前 return：命中可能只是「写过」——元数据区
    // （0..FTL_METADATA_AREA_END）里的 L2P/Anchor/Checkpoint/BBT/Journal 块正是
    // 列 2048 非 0xFF 的那批。跳过它们 ⇒ 擦完断电重启，FTL 仍从原处读回旧
    // checkpoint/BBT，`free`/`cp_seq` 一个不少。
    if (do_erase != 0U)
    {
        if (ftl_sync_erase_block(addr) != FTL_OK)
        {
            return true; /* 擦不掉 ⇒ 坏块（stuck-at-0 / 不可擦） */
        }
    }

    return marked; /* 坏不坏仍按**擦除前**读到的出厂标记报（擦后标记已丢，不可再读） */
}

/* ── 3. 格式化 ── */

// 实现要点：① `format_in_progress=true` + `bbt_save`：当前坏块图（工厂标记 + CP 快照并集）
// 先落盘；失败立即返回，用户数据与工厂标记都还完整。
// ② 擦除 **BBT 池之外** 的全部块（所有 plane）；擦失败标 BAD。
// BBT 池保留，以保存 ① 的 record。
// ③ 再 save 一次，把擦除失败新标坏的块并入 BBT（best-effort；失败只留下
// dirty 给后台 bbt_handle，工厂坏块图已在 ① 持久化）。**不清**
// `format_in_progress`：格式化的提交点是 initial Checkpoint 落盘
// （`pfr_media_format` 末尾），不是擦除结束。
//
// ⚠ **不读出厂坏块标记**：标记读取职责唯一归 bbt_init（且只在空片时做）。
// 因此「必须在 bbt_init 之后调用」是承重的——出厂标记此时已在 BST 里。
// ⚠ ②/体检/写初始元数据任一步中途掉电：最新 BBT record 仍带
// FORMAT_IN_PROGRESS，下次开机会继续格式化，而不是把“有 BBT、无 CP”
// 误判成损坏介质（cp_media_status_probe 的 IO_ERROR 分支）。
ftl_status_e bbt_format_erase(alc_t *alloc)
{
    // ① 先持久化当前坏块图（工厂标记 + CP 快照并集都在 BST），并置“格式化进行中”。
    // 失败 ⇒ 不擦任何用户块：工厂标记仍完整，可下次重试。
    alloc->bbt.format_in_progress = true;
    bbt_dirty_note(alloc);

    if (bbt_save(alloc) != FTL_OK)
    {
        LOGE(LOG_TAG_BBT "format: pre-erase BBT save failed — abort before erasing data\r\n");
        return FTL_ERROR;
    }

    // ② 擦除 BBT 候选池之外的块；擦失败的块标 BAD。候选池保留，以保存 ① 的 record。
    // 候选池可能含 shared metadata spare（名义 BBT 块坏掉时），因此不能只按
    // `[BBT_BLOCK, +BBT_POOL_BLOCKS)` 判断。
    const md_ring_t *bbt_ring = (alloc->pools != NULL) ? &alloc->pools->bbt : NULL;

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        for (WORD block = 0U; block < BLOCKS_PER_PLANE; block++)
        {
            const bool keep_bbt = bbt_block_is_candidate(bbt_ring, block);

            if (keep_bbt)
            {
                continue; /* 保留 BBT 候选池（新坏块图 record 在这里） */
            }

            const WORD  gblock = ftl_gblock(plane, block);
            const DWORD addr   = pba_make(alloc->ch, alloc->ce, alloc->lun, block, plane, 0U, 0U);

            if (ftl_sync_erase_block(addr) == FTL_OK)
            {
                continue;
            }

            // 擦不掉 ⇒ 坏块（已是坏块的不重复计数）
            if (ftl_block_is_bad(&alloc->blocks[gblock]) == false)
            {
                ftl_block_set_bad(&alloc->blocks[gblock]); /* 终态：互斥，无需清位 */
                ftl_block_clear_valid(&alloc->blocks[gblock]);
                ftl_block_set_sb(&alloc->blocks[gblock], FTL_BLOCK_NO_SB);
                alloc->bad_count++;
            }
        }
    }

    // ③ 把擦除失败新标坏的块并入 BBT（best-effort；工厂坏块图已在 ① 落盘，
    // 这里失败只留下 dirty 给后台 bbt_handle）。
    // **不清 format_in_progress**：清标志归 pfr_media_format 的提交点
    // （initial Checkpoint 已读回），否则擦除后复位会让下次开机拒绝格式化。
    bbt_dirty_note(alloc);
    (void)bbt_save(alloc);

    return FTL_OK;
}

/* ── 4. 标坏 / dirty ── */

// 实现要点：清有效位图是必需的——坏块后残留的 VPC/bitmap 会让 GC/WL 误判其为可回收块；
// **不**同步 bbt_save（持久化交后台 bbt_handle 批量）；Spare 耗尽不影响标坏（容量自然下降）。
ftl_status_e bbt_mark_bad(alc_t *alloc, WORD gblock)
{
    if ((alloc == NULL) || (gblock >= BBT_MAX_BLOCKS) || (alloc->blocks == NULL))
    {
        return FTL_ERROR;
    }

    // BAD 是终态：重复标坏必须幂等——不重复计数、不重复弹 Spare、不重复置 dirty。
    // 多个调用点（host 写失败 / migrate 擦失败 / RR）可能对同一块重复调用。
    if (ftl_block_is_bad(&alloc->blocks[gblock]))
    {
        return FTL_OK;
    }

    // 驱逐出 Free Pool —— **必须先于置 BAD**：alc_free_remove 校验「在池 ⇒ state == FREE」
    // （allocator.h 不变量②）。块若在池里，此刻它正是 FREE；先置 BAD 再出池就会命中断言。
    // 不驱逐则留下「在池却不是 FREE」的反向不一致（运行时标坏：擦除失败 / 预测性标坏）。
    // 不在池内 ⇒ no-op（alc_free_remove 的文档化契约，判据 4）。
    // 注：出厂扫描 / BBT 加载路径跑在建池之前（池还是空的）⇒ 恒 no-op，无需特判。
    alc_free_remove(alloc, gblock);

    // BST 置 BAD（单数据源，后台 bbt_handle 持久化到 NAND）。BAD 是状态机**终态** ⇒
    // 一行赋值即抹掉 OPEN/CLOSED/FREE/LOG（原掩码漏位的风险消失）；温度字段不动（与状态正交）。
    // 同时清空有效计数——坏块后该块不会再产生有效 CW，
    // 残留的 valid_count 会让 GC/WL 误判其为可回收块。
    ftl_block_set_bad(&alloc->blocks[gblock]);
    ftl_block_clear_valid(&alloc->blocks[gblock]);
    ftl_block_set_sb(&alloc->blocks[gblock], FTL_BLOCK_NO_SB);
    alloc->bad_count++;
    bbt_dirty_note(alloc);

    // 不同步调 bbt_save() — NAND BBT 持久化由后台 bbt_handle 批量执行。
    //
    // 坏块替换分两类（主流 BBT + sparing）：
    // · 元数据保留区（block < FTL_METADATA_AREA_END）：由 md_pool_service 重算
    // 候选池、从 shared metadata spare 补块；**不**弹用户 Spare（否则会把
    // 元数据的容量损失伪装成用户容量损失）。
    // · 用户区：Spare 是第一道替换；Spare 空时从 OP 借一块（第二道），
    // 避免运行期坏块直接吃用户可用块。两道都空 ⇒ 低水位告警，容量自然下降。
    const BYTE plane = ftl_gblock_plane(gblock);
    const WORD block = ftl_gblock_block(gblock);

    // 元数据块有两类：固定保留区（< AREA_END），以及**从 OP 借用的 L2P 日志块**
    // （物理块在用户区，但 Directory 的 l2p_live/当前日志块指向它）。后者不能
    // 当数据坏块处理（会错误弹用户 Spare、并漏掉 rebuild 标志）。
    const bool has_md_dir     = (alloc->md_dir != NULL);
    const bool is_l2p_live    = has_md_dir && (alloc->md_dir->l2p_live[gblock] != 0U);
    const bool is_current_log = has_md_dir && (gblock < FTL_BLOCK_TOTAL) && alloc->md_dir->cur_valid[plane] && (alloc->md_dir->cur_block[plane] == gblock);
    const bool borrowed_log   = is_l2p_live || is_current_log;

    if ((block < FTL_METADATA_AREA_END) || borrowed_log)
    {
        if (alloc->pools != NULL)
        {
            alloc->pools->dirty = true;
        }

        // Journal / L2P 日志块（固定或借用）丢失无法在当前上电周期内恢复其映射 ⇒
        // 下次开机 full metadata rebuild（本仓自拟的 BBT 持久标志）。Anchor/CP/BBT
        // 不需要：候选池重算 + 下一次提交即可自愈。
        if (borrowed_log || ((block >= JOURNAL_BLOCK0) && (block < (JOURNAL_BLOCK0 + JOURNAL_BLOCK_COUNT))) || ftl_is_l2p_log_block(block))
        {
            alloc->bbt.rebuild_required = true;
            LOGE(LOG_TAG_BBT "metadata log block %hu lost — rebuild required\r\n", block);
        }
    }
    else if ((alloc->bbt.format_in_progress == false) && (bbt_spare_refill(alloc, plane) != FTL_OK))
    {
        if (alc_free_refill_from_op(alloc, plane) == FTL_GBLOCK_INVALID)
        {
            // 限速：保留区双空时每次坏块都打会刷屏；只报第 1/64 次。
            if ((s_reserve_low_logged++ % BBT_RESERVE_LOW_LOG_EVERY) == 0U)
            {
                LOGW(LOG_TAG_BLK "reserve low plane=%hhu spare=%hu op=%hu free=%hu count=%lu\r\n", plane, alc_spare_get_count(alloc, plane), alc_op_get_count(alloc, plane), alloc->free_count, s_reserve_low_logged);
            }
        }
    }

    return FTL_OK;
}

// bbt_dirty_note — BBT 的唯一 dirty 闸门。
// 每次置位推进 dirty_gen：写 job 组 header 时快照它，完成时只有代际未变才清 dirty，
// 从而覆盖“写命令在飞期间又标坏了一块”的丢更新窗口。
void bbt_dirty_note(alc_t *alloc)
{
    alloc->bbt.dirty = true;
    alloc->bbt.dirty_gen++;
}

/* ── 5. 同步持久化 ── */

/**
 * @brief 从 BST 重建一条 BBT record 的 header（同步 bbt_save 与异步 bbt_write_submit 共用）。
 *
 * `sequence` 由调用者决定：同步路径刚自增，异步路径在 kick 起步时已自增。
 *
 * @param alloc    Allocator 上下文（从 BST 重建坏块位图，取 bad_count 与标志）
 * @param sequence 写入 header 的 sequence
 * @param header   输出：填好的 record header
 */
static void bbt_build_header(const alc_t *alloc, DWORD sequence, bbt_header_t *header)
{
    memset(header, 0, sizeof(*header));
    header->magic        = BBT_MAGIC;
    header->sequence     = sequence;
    header->total_blocks = BBT_MAX_BLOCKS;
    header->bad_count    = alloc->bad_count;
    header->reserved[0]  = alloc->bbt.format_in_progress ? BBT_FLAG_FORMAT_IN_PROGRESS : 0U;
    header->reserved[1]  = alloc->bbt.rebuild_required ? BBT_FLAG_REBUILD_REQUIRED : 0U;

    for (WORD gblock = 0U; gblock < BBT_MAX_BLOCKS; gblock++)
    {
        if (ftl_block_is_bad(&alloc->blocks[gblock]))
        {
            bbt_set_bad(header->bad_map, gblock);
        }
    }
}

// 实现要点：游标未建立时先定位池内最新 record 的下一槽；**无论成败都推进游标**
// （半编程槽不可复用）；失败保留 dirty 供下次重试。NAND 格式一字不改。
ftl_status_e bbt_save(alc_t *alloc)
{
    if (alloc->bbt.dirty == false)
    {
        return FTL_OK;
    }

    bbt_probe_ctx_t ctx;
    bbt_ctx_init(alloc, &ctx);

    // 游标未建立 ⇒ 先定位池内最新有效 record 的下一槽。
    // ⚠ sequence 只取 max：候选池可能因 BAD/体检而**缩小**，被移出池的块上仍可能
    // 留着更高 sequence 的旧 record（bootstrap 扫描仍会读到它）。若这里把 sequence
    // 降到新池内的最大值，下一次开机可能让旧 record 胜出、读到旧坏块图。
    if (alloc->bbt.cursor_valid == false)
    {
        md_ring_locate_out_t loc;

        if (md_ring_locate(&ctx.ring, bbt_record_probe, &ctx, &loc) != FTL_OK)
        {
            return FTL_ERROR;
        }
        alloc->bbt.write_idx = loc.write_idx;
        if (loc.sequence > alloc->bbt.sequence)
        {
            alloc->bbt.sequence = loc.sequence;
        }
        alloc->bbt.cursor_valid = true;
    }

    alloc->bbt.sequence++;

    bbt_header_t header;
    bbt_build_header(alloc, alloc->bbt.sequence, &header);

    // 追加写：池内下一槽（块满则先擦池内下一块）。
    // 无论成败都推进游标（已写过的槽可能半编程，不可复用）。
    DWORD addr;
    if (md_ring_prepare_write(&ctx.ring, &ctx.topo, alloc->bbt.write_idx, &addr) != FTL_OK)
    {
        return FTL_ERROR;
    }

    // 开机/格式化：同步镜像写到 P0+P1（没有服务泵可等）。
    if (ftl_sync_write_page_mirror(addr, &header, sizeof(header), FTL_PAGE_CFG_META()) != 0)
    {
        alloc->bbt.write_idx = ((alloc->bbt.write_idx + 1U) % md_ring_total_records(&ctx.ring));
        return FTL_ERROR; /* dirty 保留 ⇒ 下次重试 */
    }

    alloc->bbt.write_idx = ((alloc->bbt.write_idx + 1U) % md_ring_total_records(&ctx.ring));

    alloc->bbt.dirty = false;
    return FTL_OK;
}

// 实现要点：进入即复位 alloc->bbt（找不到时保持 0/无效 ⇒ 后续 save 从池首开始）；
// locate 只回序号，header 本体要按该序号**再读一次**并复核 sequence。
ftl_status_e bbt_load(alc_t *alloc, BYTE *bad_map, DWORD *bad_count, DWORD *sequence)
{
    // 复位本实例池状态：无有效 record 时保持 0/无效（后续 bbt_save 从池首开始）
    alloc->bbt.sequence           = 0U;
    alloc->bbt.write_idx          = 0U;
    alloc->bbt.cursor_valid       = false;
    alloc->bbt.format_in_progress = false;

    bbt_probe_ctx_t ctx;
    bbt_ctx_init(alloc, &ctx);

    md_ring_locate_out_t loc;

    if (md_ring_locate(&ctx.ring, bbt_record_probe, &ctx, &loc) != FTL_OK)
    {
        return FTL_ERROR;
    }
    alloc->bbt.sequence  = loc.sequence;
    alloc->bbt.write_idx = loc.write_idx;
    if (loc.newest_idx == UINT16_MAX)
    {
        alloc->bbt.sequence  = 0U;
        alloc->bbt.write_idx = 0U;
        return FTL_ERROR; /* 池内无有效 record */
    }

    alloc->bbt.cursor_valid = true;

    // 重读胜者（locate 只返回序号，Header 本体要再读一次）。
    // 镜像池：逐个 plane 试；sequence 必须等于 locate 选中的 record sequence。
    bbt_header_t best;
    bool         found = false;
    for (BYTE pi = 0U; pi < md_ring_plane_count(ctx.ring.plane_mask); pi++)
    {
        const BYTE plane = md_ring_plane_at(ctx.ring.plane_mask, pi);
        DWORD      addr  = md_ring_addr_plane(&ctx.ring, &ctx.topo, loc.newest_idx, 0U, plane);

        if (ftl_sync_read_page(addr, &best, sizeof(best), FTL_PAGE_CFG_META()) != 0)
        {
            continue;
        }
        if ((bbt_header_valid(&best) == false) || (best.sequence != alloc->bbt.sequence))
        {
            continue;
        }
        found = true;
        break;
    }
    if (found == false)
    {
        return FTL_ERROR;
    }

    alloc->bbt.format_in_progress = ((best.reserved[0] & BBT_FLAG_FORMAT_IN_PROGRESS) != 0U);
    alloc->bbt.rebuild_required   = ((best.reserved[1] & BBT_FLAG_REBUILD_REQUIRED) != 0U);

    // 输出到 caller
    memcpy(bad_map, best.bad_map, BBT_BITMAP_SIZE);
    *bad_count = best.bad_count;
    *sequence  = best.sequence;

    return FTL_OK;
}

/* ── 6. 异步 job ── */

/**
 * @brief 把当前 BST 重建为一条 BBT record，**异步**提交写（phase = BBT_JOB_WRITE）。
 *
 * @param alloc 分配器（写点 = alloc->bbt.write_idx）。
 *
 * @return FTL_OK 已提交（waiting = 1）；
 *                FTL_ERROR 建链/提交失败（**waiting 保持 0**，调用者停在 BBT_JOB_WRITE 等下一轮 kick 续跑——**不重擦块**）。
 *
 * @note 实现要点：record 内容在**提交时**从 BST 重建（不是起步时快照）——换块擦的等待
 * 期间新标坏的块会被一并记进本条 record，是严格的信息超集。
 * header 是**栈上** 540 B：ftl_async_write_page 在 prepare 时已把它拷进
 * FC buffer 池（同 jrn_freeze 的做法，不需要为异步保留快照缓冲）。
 * 地址按 write_idx 重算（write_idx 写成功才推进 ⇒ 此刻它仍指向本槽）。
 */
static ftl_status_e bbt_write_submit(alc_t *alloc)
{
    bbt_probe_ctx_t ctx;
    bbt_ctx_init(alloc, &ctx);

    bbt_header_t header;
    bbt_build_header(alloc, alloc->bbt.sequence, &header);

    // 快照本条 record 的 dirty 代际：写飞行期间若又标坏（dirty_gen++），
    // 完成时不能清 dirty，必须再落一条 record。
    alloc->bbt.handle.dirty_gen = alloc->bbt.dirty_gen;

    DWORD addr = md_ring_addr(&ctx.ring, &ctx.topo, alloc->bbt.write_idx, 0U);

    fc_command_t *cmd;
    if (ftl_async_write_page_mirror(&cmd, addr, &header, sizeof(header), FTL_PAGE_CFG_META()) != FTL_OK)
    {
        return FTL_ERROR;
    }
    if (ftl_op_submit(cmd, FTL_OP_BBT, alloc) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    alloc->bbt.handle.phase   = BBT_JOB_WRITE;
    alloc->bbt.handle.waiting = true;
    return FTL_OK;
}

// 实现要点：· **单飞行**：① 命令在飞 ⇒ 直接返回；② phase == WRITE 且不在飞 ⇒ 上一步提交
// 被背压拒，只续提交写（**不重擦**——擦已成功，重擦纯属磨损）；③ 其余非 IDLE
// （= ERASE 在飞）⇒ 返回；④ IDLE 才装配新 job。
// · 游标未建立时的 `md_ring_locate` 是**同步**的（扫池逐页读），但只在
// 首次落盘发生一次，且本函数由后台调度调用（不在完成泵内）
// ⇒ 不是嵌套泵。job 的其余步骤全部"提交即返回"。
// · sequence 在**起步时**自增；续跑路径不会再自增。
// 擦失败 ⇒ 回 IDLE、dirty 保留、write_idx 不动，下轮重试。
void bbt_handle(alc_t *alloc)
{
    do
    {
        // 心跳（见 ftl_job.h）：job 在飞期间每满 1s 打一行 phase/age
        if (alloc->bbt.handle.phase == BBT_JOB_IDLE)
        {
            alloc->bbt.handle.log_ms   = 0U;
            alloc->bbt.handle.start_ms = 0U;
        }
        else if (ftl_job_heartbeat(&alloc->bbt.handle.log_ms, &alloc->bbt.handle.start_ms))
        {
            LOGI(LOG_TAG_BBT "job phase=%d waiting=%d seq=%lu idx=%u age=%lums\r\n", alloc->bbt.handle.phase, alloc->bbt.handle.waiting, alloc->bbt.sequence, alloc->bbt.write_idx, ftl_job_age_ms(&alloc->bbt.handle.start_ms));
        }

        if (alloc->bbt.handle.waiting == true)
        {
            break; /* ① 命令在飞 */
        }
        if (alloc->bbt.handle.phase == BBT_JOB_WRITE)
        {
            (void)bbt_write_submit(alloc); /* ② 背压后续提交（擦已完成） */
            break;
        }
        if (alloc->bbt.handle.phase != BBT_JOB_IDLE)
        {
            break; /* ③ ERASE 在飞（waiting 必为 1，此处只是防御） */
        }

        if (alloc->bbt.dirty == false)
        {
            break; /* ④ 无变更 */
        }

        bbt_probe_ctx_t ctx;
        bbt_ctx_init(alloc, &ctx);

        // 游标未建立 ⇒ 先定位池内最新有效 record 的下一槽
        if (alloc->bbt.cursor_valid == false)
        {
            md_ring_locate_out_t loc;

            if (md_ring_locate(&ctx.ring, bbt_record_probe, &ctx, &loc) != FTL_OK)
            {
                LOGE(LOG_TAG_BBT "pool locate failed — 下轮重试\r\n");
                break;
            }
            alloc->bbt.sequence     = loc.sequence;
            alloc->bbt.write_idx    = loc.write_idx;
            alloc->bbt.cursor_valid = true;
        }

        alloc->bbt.sequence++;

        DWORD addr;
        bool  need_erase = false;
        if (md_ring_prepare_addr(&ctx.ring, &ctx.topo, alloc->bbt.write_idx, &addr, &need_erase) != FTL_OK)
        {
            LOGE(LOG_TAG_BBT "prepare failed idx=%hu\r\n", alloc->bbt.write_idx);
            break;
        }

        if (need_erase)
        {
            fc_command_t *cmd;

            // 换块：先擦目标块（池内轮转 ⇒ 该块持全池最旧 record，擦掉不丢信息）。
            // 镜像池 ⇒ multi-plane 擦，两个副本一起清。
            if (ftl_async_erase_block_multi_plane(&cmd, addr) != FTL_OK)
            {
                break; /* 命令池空 / 建链失败 ⇒ 下轮重试（write_idx 未动） */
            }
            if (ftl_op_submit(cmd, FTL_OP_BBT, alloc) != FTL_OK)
            {
                (void)fc_command_release(cmd);
                break;
            }
            alloc->bbt.handle.phase   = BBT_JOB_ERASE;
            alloc->bbt.handle.waiting = true;
            break;
        }

        // 不必换块 ⇒ 直接起写。先落 phase，提交被背压拒就停在 WRITE/waiting=0 等续跑
        alloc->bbt.handle.phase   = BBT_JOB_WRITE;
        alloc->bbt.handle.waiting = false;
        (void)bbt_write_submit(alloc);
    } while (0);

    // 统一上报出口：无论推进是否早退，主流程都拿本轮状态。
}

/**
 * @brief 擦完成的服务函数
 *
 * 成功 ⇒ 起写 record；失败 ⇒ 回 IDLE，游标**不推进**（擦没成，该槽还没被消耗），
 * dirty 保留供下轮重试。
 *
 * @param alloc      Allocator 上下文
 * @param cmd        完成的单条命令（本函数负责 fc_command_release）
 * @param completion FC 完成状态（status 判成败）
 */
static void bbt_erase_complete(alc_t *alloc, fc_command_t *cmd, const fc_command_status_t *completion)
{
    if (fc_cmd_status_has_error(completion->status))
    {
        LOGE(LOG_TAG_BBT "ring block erase failed status=0x%02x — 游标不推进，下轮重试\r\n", completion->status);
        alloc->bbt.handle.phase   = BBT_JOB_IDLE;
        alloc->bbt.handle.waiting = false;
        (void)fc_command_release(cmd);
        return;
    }

    // 擦成功 ⇒ 起写。先落 phase，提交被背压拒就停在 WRITE/waiting=0 等续跑
    // （**不再回 IDLE**——回 IDLE 会让下轮 kick 重走 prepare_addr 再擦一次同一块）。
    alloc->bbt.handle.phase   = BBT_JOB_WRITE;
    alloc->bbt.handle.waiting = false;
    (void)bbt_write_submit(alloc);
    (void)fc_command_release(cmd);
}

/**
 * @brief record 写完成的服务函数
 *
 * **无论成败都推进游标**：已写过的槽可能半编程，不可复用。
 * dirty 的清除还要比 dirty_gen —— 写飞行期间若又标了坏块，本条 record 是旧快照。
 *
 * @param alloc      Allocator 上下文
 * @param cmd        完成的单条命令（本函数负责 fc_command_release）
 * @param completion FC 完成状态（status 判成败）
 */
static void bbt_write_complete(alc_t *alloc, fc_command_t *cmd, const fc_command_status_t *completion)
{
    bbt_probe_ctx_t rctx;

    bbt_ctx_init(alloc, &rctx);
    alloc->bbt.write_idx = ((alloc->bbt.write_idx + 1U) % md_ring_total_records(&rctx.ring));

    if (fc_cmd_status_has_error(completion->status))
    {
        LOGE(LOG_TAG_BBT "record write failed status=0x%02x — dirty 保留\r\n", completion->status);
    }
    else if (alloc->bbt.dirty_gen == alloc->bbt.handle.dirty_gen)
    {
        alloc->bbt.dirty = false;
    }
    else
    {
        LOGE(LOG_TAG_BBT "record written but dirty_gen advanced — 再落一条\r\n");
    }

    alloc->bbt.handle.phase   = BBT_JOB_IDLE;
    alloc->bbt.handle.waiting = false;
    (void)fc_command_release(cmd);
}

// 实现要点：按 job.phase 分派到 bbt_erase_complete / bbt_write_complete（见各自注释）。
// ⚠ 本函数**只**收尾/续提交，不调 bbt_handle（续提交是提交侧动作，无需嵌套泵）。
void ftl_bbt_callback(ftl_ctx_t *ftl, const fc_command_status_t *completion, void *ctx)
{
    alc_t *alloc = (alc_t *)ctx;

    (void)ftl; /* 实例无关：ctx 就是本实例的 allocator */

    fc_command_t *cmd = fc_command_id_to_command(completion->command_id);

    // 孤儿 / 迟到完成：当前没有在飞命令 ⇒ 只释放，不改状态。
    if (alloc->bbt.handle.waiting == false)
    {
        LOGE(LOG_TAG_BBT "orphan completion phase=%d — ignored\r\n", alloc->bbt.handle.phase);
        (void)fc_command_release(cmd);
        return;
    }

    if (alloc->bbt.handle.phase == BBT_JOB_ERASE)
    {
        bbt_erase_complete(alloc, cmd, completion);
    }
    else if (alloc->bbt.handle.phase == BBT_JOB_WRITE)
    {
        bbt_write_complete(alloc, cmd, completion);
    }
    else
    {
        // 不该发生：一条命令对应一个 job，phase 必为 ERASE / WRITE。
        LOGE(LOG_TAG_BBT "unexpected completion phase=%d\r\n", alloc->bbt.handle.phase);
        alloc->bbt.handle.phase   = BBT_JOB_IDLE;
        alloc->bbt.handle.waiting = false;
        (void)fc_command_release(cmd);
    }
}

/* ── 7. Spare 补池 ── */

// 实现要点：只出不进（出厂每 plane 预留 FTL_SPARE_POOL_PER_PLANE 个）。调用者保证
// 该块已擦除（alc_free_push 契约）。
// ⚠ 必须按 plane 取：Spare 池按 super block 粒度预留，补错 plane 不恢复该
// plane 的配对能力（原实现从全局池 LIFO 弹出，实际总补 plane 0）。
ftl_status_e bbt_spare_refill(alc_t *alloc, BYTE plane)
{
    // Spare Pool 坏块替换：出厂时 main.c 已为**每个 plane** 预留
    // FTL_SPARE_POOL_PER_PLANE=50 个好块到 alloc->spare_blocks[plane]。运行时
    // bbt_mark_bad 标坏一块后，从该块所属 plane 取一个好块 alc_free_push
    // 补进 free pool，保用户容量水位。
    // 只出不进，该 plane Spare Pool 耗尽后返回 FTL_ERROR（容量自然下降）。
    if ((alloc == NULL) || (alloc->spare_blocks == NULL) || (plane >= PLANE_NUM))
    {
        return FTL_ERROR;
    }

    WORD gblock;
    if (alc_spare_pop(alloc, plane, &gblock) == false)
    {
        return FTL_ERROR; /* 该 plane Spare Pool 耗尽 */
    }
    alc_free_push(alloc, gblock);

    return FTL_OK;
}

/* ── 8. 诊断打印 ── */

// 实现要点：每格定宽 BBT_MAP_ENTRY_W，两 plane 并排（行长受终端宽度约束，见宏注释）；
// 坏块打的是**本 plane 绝对块号**而非排内列号（避免与 FF 并排被误读成十六进制）。
void bbt_print_bad_map(alc_t *alloc, BYTE do_erase)
{
    char line[BBT_MAP_LINE_MAX];

    LOGI("CH=%hhu  CE=%hhu  LUN=%hhu\r\n", alloc->ch, alloc->ce, alloc->lun);
    {
        int n = 0;

        for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
        {
            const int label_w = 7; /* "PLANE=k" */
            const int pad     = (int)((BBT_MAP_GROUP_W - label_w) / 2U);
            const int trail   = (int)BBT_MAP_GROUP_W - label_w - pad;

            if (plane > 0U)
            {
                n += snprintf(&line[n], sizeof(line) - (size_t)n, "   ");
            }
            n += snprintf(&line[n], sizeof(line) - (size_t)n, "%*sPLANE=%hhu%*s", pad, "", plane, trail, "");
        }
        LOGI("%s\r\n", line);
    }
    WORD rows = (BLOCKS_PER_PLANE / BBT_MAP_COLS_PER_PLANE);
    for (WORD row = 0U; row < rows; row++)
    {
        int n = 0;

        for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
        {
            if (plane > 0U)
            {
                n += snprintf(&line[n], sizeof(line) - (size_t)n, "   ");
            }

            for (WORD col = 0U; col < BBT_MAP_COLS_PER_PLANE; col++)
            {
                const WORD block = ((row * BBT_MAP_COLS_PER_PLANE) + col);

                if (col > 0U)
                {
                    n += snprintf(&line[n], sizeof(line) - (size_t)n, " ");
                }

                if (block >= BLOCKS_PER_PLANE)
                {
                    n += snprintf(&line[n], sizeof(line) - (size_t)n, "%*s", (int)BBT_MAP_ENTRY_W, ""); /* 末排不满：留空 */
                }
                else if (bbt_probe_block_bad(alloc, plane, block, do_erase))
                {
                    // 坏块打**本 plane 的绝对块号**（0..BLOCKS_PER_PLANE-1），不是排内列号：
                    // 列号每排重新计数，读图的人得自己算 排号×列数+列号，且 1~2 位的数字
                    // 与 `FF` 并排极易被误读成十六进制字节值。
                    n += snprintf(&line[n], sizeof(line) - (size_t)n, "%*hu", (int)BBT_MAP_ENTRY_W, block);
                }
                else
                {
                    n += snprintf(&line[n], sizeof(line) - (size_t)n, "%*s", (int)BBT_MAP_ENTRY_W, "FF"); /* 好块 */
                }
            }
        }

        LOGI("%s\r\n", line);
    }
}

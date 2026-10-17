/*
 * bbt.c — 坏块表（BBT）：出厂扫描 + 运行时标坏 + NAND 持久化 + Spare 池替换
 *
 * 职责：
 *   维护坏块账本——开机从 NAND 读回 BBT（无则出厂扫描）、运行时标坏块、
 *   把坏块位图持久化到 NAND BBT 池（环形轮转）、坏块替换从 Spare Pool 补块。
 *
 * 与 bbt.h 的分工：
 *   bbt.h —— 结构（bbt_header_t）+ 位图存取原语（bbt_is_bad/bbt_set_bad）+ 尺寸宏；
 *   本文件 —— 扫描/标坏/持久化/替换的实现 + 坏块图打印诊断。
 *
 * 单一数据源（v0.56 起）：
 *   内存里坏块 = ftl_block_t.state 的 BAD 位（BST 单数据源）；
 *   NAND 持久化仍用紧凑 bad_map[512] 格式（bbt_header_t，一字不改）；
 *   bbt_save 时从 BST 重建 bitmap，bbt_init 时从 bitmap 重建 BST。
 *
 * BBT 池（环形轮转，一条 record = 1 页，见 metadata_ring_*）：
 *   BBT_POOL_BLOCKS 块、单 plane；游标/序号是**本池自己的**，开机由 bbt_load 恢复。
 *
 * 文件结构：
 *   1. 池几何/校验/探针   bbt_ring_init / bbt_header_valid / bbt_record_probe
 *   2. 初始化             bbt_init / bbt_scan_factory
 *   3. 坏块图打印（诊断） bbt_probe_block_bad / bbt_print_bad_map
 *   4. 标坏/持久化        bbt_mark_bad / bbt_save（同步，开机/格式化）
 *                         bbt_kick / ftl_op_bbt（异步 job，后台攒批，阶段 7）
 *                         bbt_load
 *   5. 坏块替换           bbt_spare_pool_refill
 *
 * 两条持久化路径**刻意并存**：
 *   · `bbt_save`  —— **同步**，只给结构性同步上下文（开机出厂扫描 bbt_scan_factory、
 *                    全片格式化后的重落盘 power_failure_recovery）。此时 superloop
 *                    未建、无人 drain CQ，异步会死锁。
 *   · `bbt_kick`  —— **异步 job**，后台攒批（ftl_service 的 idle/normal 档）。
 *   两者写出的 record 格式与游标推进规则**逐字相同**。
 *
 * 游标/序号是 **per-instance**（序 4：收进 ftl_allocator_t.bbt，见 bbt_state_t）——
 * 多实例（多 LUN）下每实例一份，互不干扰。
 */

#include "ftl/bbt.h"

#include "common/assertion.h"
#include "common/log.h"
#include "fc/fc.h"
#include "ftl/ftl_fc_interface.h"
#include "ftl/ftl_job.h"  /* 心跳（[JOB] 观测口）*/
#include "ftl/ftl_op.h"   /* ftl_op_submit / FTL_OP_BBT（阶段 7） */
#include "ftl/metadata.h" /* metadata_ring_*（BBT 池轮转原语）*/
#include "nand/onfi_1_0.h"

#include <stdio.h>
#include <string.h>

/*
 * BBT 内部状态：**per-instance**，落在 ftl_allocator_t.bbt（bbt_state_t，序 4）。
 * 原先是 file-scope static ⇒ 多实例（多 LUN）下所有实例共享一份游标/序号，互相覆盖。
 *
 * 坏块状态已并入 ftl_block_t.state（Block State Table 单数据源；该字段 2026-10-17 起
 * 是互斥枚举，BAD 为终态），
 * NAND 持久化仍用紧凑 bad_map[512] 格式（bbt_header_t，一字不改），
 * bbt_save 时从 BST 重建 bitmap。
 */

/* ── 1. 池几何 / 校验 / 探针 ── */

/* ── BBT 池：环形轮转（一条 record = 1 页）── */
static void bbt_ring_init(metadata_ring_t *ring)
{
    ring->first_block      = BBT_BLOCK;
    ring->block_count      = BBT_POOL_BLOCKS;
    ring->pages_per_record = 1U;
    ring->plane            = 0U;
}

/* 整条校验（一条 record = 1 页）：magic。
 * 无本层 CRC——页完整性由下层 FC 的 per-CW CRC + ECC 保证（ADR v0.62）。 */
static bool bbt_header_valid(const bbt_header_t *hdr)
{
    return (hdr->magic == BBT_MAGIC);
}

typedef struct
{
    ftl_topology_t  topo;
    metadata_ring_t ring;
} bbt_probe_ctx_t;

/* 本实例的池 + 路由（探针/定位/写点都要这一对，四处共用） */
static void bbt_ctx_init(const ftl_allocator_t *alloc, bbt_probe_ctx_t *ctx)
{
    bbt_ring_init(&ctx->ring);
    ctx->topo = allocator_topo(alloc);
}

/* BBT record 探针（见 metadata_ring_probe_fn）。full 与廉价探针等价（record 只有一页）。 */
static ftl_status_t bbt_record_probe(void *ctx, uint16_t record_idx, int full, uint32_t *out_sequence)
{
    (void)full;

    bbt_probe_ctx_t *c = (bbt_probe_ctx_t *)ctx;

    nand_pba_t addr = metadata_ring_addr(&c->ring, &c->topo, record_idx, 0U);

    bbt_header_t hdr;
    if (ftl_sync_read_page(addr, &hdr, sizeof(hdr), FTL_PAGE_CFG_META()) != 0)
    {
        return FTL_ERROR;
    }
    if ((!bbt_header_valid(&hdr)) || (hdr.sequence == 0U))
    {
        return FTL_ERROR; /* sequence 从 1 起；0 = 未写 */
    }

    *out_sequence = hdr.sequence;
    return FTL_OK;
}

/* ── 2. 初始化 ── */

/* 定义在下方「坏块图打印」节（与 bbt_print_bad_map / bbt_erase_all_blocks 共用）；
 * bbt_scan_factory 需要它在前面，故先声明。 */
static bool bbt_probe_block_bad(ftl_allocator_t *alloc, uint8_t plane, uint16_t block, uint8_t do_erase);

/* --------------------------------------------------------------------
 * bbt_medium_has_ftl_data
 *   功能：介质上是否**已有 FTL 写过的数据**（= 这块芯片被格式化过）——
 *         出厂坏块扫描能不能做的唯一判据。
 *   参数：alloc —— 分配器（取 ch/ce 与几何）。
 *   返回：true = 介质非空；false = 空片。
 *   实现要点：判据 = **固定元数据区（0..FTL_METADATA_BLOCK_END，两个 plane）里只要
 *             有一页能读成合法的 FTL 页**——即 FC 的 per-CW CRC 与 META 自洽校验
 *             全过（fc_data_path_meta_check：META 必须编码本页的 plane/cw）。
 *             · 空片：该区每页都是全 FF ⇒ META 自洽校验必挂（全 FF 的 META 解不出
 *               正确的 plane/cw 位）⇒ 读必失败 ⇒ 返回 0。
 *             · **不用「字节是否全 FF」这类内容启发式**：那会把「出厂坏块块」（主区
 *               本来就已损坏/带残数据）误判成「写过的块」，方向恰好反了。这里让 FC
 *               自己去判「这是不是一页合法的 FTL 页」，坏块块读不成 ⇒ 不误判。
 *             · 与 FTL 的**布局/格式版本无关**：FC 层的 CW 结构（META/CRC/ECC）不随
 *               FTL 版本变，故版本升级后的介质照样认得出——这正是必须覆盖的场景。
 *   ⚠ 只读不写；只在 bbt_load 失败时才调用（正常开机不付这份代价）。
 *   ⚠ 只探**固定元数据区**：该区的页一律以 FTL_PAGE_CFG_META()（RANDOM_DISABLE）写，
 *     与这里的读配置一致；用户数据页用 DATA 配置（带解扰）写，不在此列、也不需要。
 * -------------------------------------------------------------------- */
static bool bbt_medium_has_ftl_data(ftl_allocator_t *alloc)
{
    uint8_t scratch[16];

    for (uint8_t plane = 0U; plane < PLANE_NUM; plane++)
    {
        for (uint16_t block = 0U; block < (uint16_t)FTL_METADATA_BLOCK_END; block++)
        {
            const nand_pba_t addr = pba_make(alloc->ch, alloc->ce, 0U, block, plane, 0U, 0U);

            if (ftl_sync_read_page(addr, scratch, sizeof(scratch), FTL_PAGE_CFG_META()) == FTL_OK)
            {
                return true;
            }
        }
    }

    return false;
}

/* --------------------------------------------------------------------
 * bbt_init
 *   功能：开机初始化坏块账本——先尝试从 NAND 读回 BBT，无则执行出厂扫描。
 *   参数：alloc —— 分配器（承载 BST）；**前置条件**，非法即 ASSERT。
 *   返回：FTL_OK 读到 BBT 并重建 BST；btb_scan_factory 的返回值（坏块数）当无有效 BBT；
 *         FTL_ERROR BBT 加载路径失败。
 *   实现要点：读到 BBT 后从 bitmap 重建 BST 的 BAD 位并清 OPEN/CLOSED/FREE/sb_id
 *             （单数据源方向：NAND bitmap → BST）。
 * -------------------------------------------------------------------- */
ftl_status_t bbt_init(ftl_allocator_t *alloc)
{
    /* 前置条件（判据 2 / §二十六「子模块公开入口」）：唯一调用者 ftl.c 传 &ftl->alloc。 */
    ASSERT(alloc != NULL);

    alloc->bbt.sequence    = 0U;
    alloc->bbt.dirty       = 0U;
    /* 阶段 7：清异步 job（allocator_init 已零化，此处再钉一次） */
    alloc->bbt.job.phase   = (uint8_t)BBT_JOB_IDLE;
    alloc->bbt.job.waiting = 0U;

    /* 尝试从 NAND 加载已存在的 BBT */
    {
        uint8_t tmp_bad_map[BBT_BITMAP_SIZE];

        uint32_t seq;
        uint32_t loaded_bad_count = 0U;
        int      ret              = bbt_load(alloc, tmp_bad_map, &loaded_bad_count, &seq);
        if (ret == 0)
        {
            uint16_t bad_idx = 0U;

            alloc->bbt.sequence = seq;
            alloc->bad_count    = (uint16_t)loaded_bad_count;

            /* 从 NAND bitmap 重建 BST is_bad 位（单数据源） */
            for (uint16_t gblock = 0U; gblock < BBT_MAX_BLOCKS; gblock++)
            {
                if (bbt_is_bad(tmp_bad_map, gblock))
                {
                    ftl_block_set_bad(&alloc->blocks[gblock]); /* 终态：互斥，无需清位 */
                    ftl_block_set_sb(&alloc->blocks[gblock], FTL_BLOCK_NO_SB);
                    bad_idx++;
                }
            }
            alloc->bad_count = bad_idx;

            return FTL_OK;
        }
    }

    /* 无有效 BBT → **只有空片**才允许出厂扫描。
     *
     * ⚠ 为什么必须拦：用过的块，其 Page 0 的 spare 首字节（列 2048）**不是**出厂标记，
     *   而是 FTL 自己的数据——FC 一次页编程写 PAGE_SIZE = 2112（主区 2048 + 备区 64），
     *   页缓冲按 4 × CW(528B) 拼 ⇒ CW3 的尾巴 64 字节正好落进备区，覆盖掉出厂标记。
     *   于是「列 2048 非 0xFF ⇒ 坏块」这条判据在用过的介质上退化成「哪些块被写过」，
     *   重扫会把**所有写过的块**（含活的 Anchor/Checkpoint/Wear）判成坏块，随后
     *   bbt_save 把这份垃圾落盘 —— 真坏块反而丢了，活块全废。
     *
     * 判据 = bbt_medium_has_ftl_data()（元数据区里有没有合法的 FTL 页）。
     * 空片路径不受影响：那时列 2048 确实就是出厂标记。
     *
     * 拒绝扫描后的状态：坏块账本从空开始（bad_count = 0、sequence 仍 0、**不置 dirty**
     * ⇒ 不落盘，免得把「零坏块」写成既成事实）。真坏块由运行时 bbt_mark_bad 兜底
     * （PROGRAM/ERASE 失败即标）。 */
    if (bbt_medium_has_ftl_data(alloc))
    {
        LOGE("[BBT] 无有效 BBT，但介质已被格式化过 —— **拒绝出厂扫描**：\r\n");
        LOGE("      用过的块 spare[0] 是 FTL 数据而非出厂标记，重扫会把写过的块全判坏。\r\n");
        LOGE("      坏块账本从空开始；真坏块由运行时 bbt_mark_bad 兜底。\r\n");

        alloc->bad_count = 0U;
        return FTL_OK;
    }

    /* 空片 → 执行出厂扫描 */
    return bbt_scan_factory(alloc);
}

/* --------------------------------------------------------------------
 * bbt_scan_factory
 *   功能：出厂坏块扫描——逐块读 Page 0 **spare 区首字节**的出厂坏块标记，标记位非 0xFF 即坏块。
 *   参数：alloc —— 分配器；**前置条件**，非法即 ASSERT（唯一调用者 bbt_init 已断言）。
 *   返回：坏块数（int）；FTL_OK(0) 无坏块。
 *   实现要点：判据 = **OOB 出厂标记**（`bbt_probe_block_bad(..., do_erase=0)`），
 *             与器件手册的建 BBT 流程一致（见 bbt_probe_block_bad 的引文）。
 *             跳过 block 0（出厂总是好块）；扫描完置 dirty 并 bbt_save 落盘。
 *
 *   ⚠ **前置条件：介质必须是空片**（`bbt_init` 用 `bbt_medium_has_ftl_data` 把关）。
 *     用过的块 Page 0 的 spare 首字节是 FTL 自己的数据（页编程 2112B 把 CW3 尾巴 64
 *     字节溢进备区），不是出厂标记 ⇒ 在用过的介质上扫出来的不是坏块图，而是
 *     「哪些块被写过」图。本函数自己**判不了**这件事（它读的就是那个字节），
 *     判据在调用方。
 *
 *             ⚠ 历史：旧实现以 `ftl_sync_read_page_raw` 的**返回码**判坏块——但 raw 模式
 *               跳过 ECC，物理失败的页照样回 READY（返回码判不出坏块），而「数据全 FF」
 *               既可能是空块也可能是坏块，**两种判据都不成立**。根因是当时 FC 层够不到
 *               spare 列（cw 只有 2 位 ⇒ 列上限 1584 < 2048）；OP_READ_COLUMN 通路
 *               落地后该替代策略作废（P2-3 收口）。
 * -------------------------------------------------------------------- */
int bbt_scan_factory(ftl_allocator_t *alloc)
{
    uint16_t bad_idx = 0U;

    alloc->bbt.dirty = 0U;

    for (uint16_t gblock = 0U; gblock < BBT_MAX_BLOCKS; gblock++)
    {
        /* 跳过 Block 0（出厂总是好块）*/
        if (gblock == 0U)
        {
            continue;
        }

        /* 全局块号 → 各 Plane 的物理块号 */
        uint8_t  plane = ftl_gblock_plane(gblock);
        uint16_t block = ftl_gblock_block(gblock);

        /* 只读标记（do_erase=0）：出厂扫描阶段**不得**擦块——擦除会永久毁掉标记 */
        if (bbt_probe_block_bad(alloc, plane, block, 0U))
        {
            ftl_block_set_bad(&alloc->blocks[gblock]); /* 终态：互斥，无需清位 */
            ftl_block_set_sb(&alloc->blocks[gblock], FTL_BLOCK_NO_SB);
            bad_idx++;
        }
    }

    alloc->bad_count = bad_idx;
    alloc->bbt.dirty = 1U;

    /* 将扫描结果写入 NAND BBT */
    (void)bbt_save(alloc);

    return (int)bad_idx;
}

/* ══════════════════════════════════════════════════════════════════
 * 坏块图打印（诊断；默认**只读**，加 --erase 才逐块擦除）
 * ══════════════════════════════════════════════════════════════════ */

/* 一排每 plane 的块数。受**终端宽度**约束：每格定宽 BBT_MAP_ENTRY_W，两 plane 并排
 * ⇒ 行长 = 2 × (n × (ENTRY_W+1) − 1) + 3，再加 "[INF] " 前缀。取 8 ⇒ 87 字符，
 * 在 ~96 列的终端上不折行（取 16 会到 167，必折）。*/
#define BBT_MAP_COLS_PER_PLANE (8U)
#define BBT_MAP_ENTRY_W        (4U)                                                   /* 每格固定宽：最大块号 "2047" 也放得下 */
#define BBT_MAP_GROUP_W        (BBT_MAP_COLS_PER_PLANE * (BBT_MAP_ENTRY_W + 1U) - 1U) /* 一组宽度：n 个定宽格 + n-1 空格 */
#define BBT_MAP_LINE_MAX       (PLANE_NUM * (BBT_MAP_GROUP_W + 3U) + 1U)

_Static_assert((BLOCKS_PER_PLANE % BBT_MAP_COLS_PER_PLANE) == 0U, "每 plane 块数须被每排列数整除，否则末排漏块");

/*
 * bbt_probe_block_bad - 探测单个块是否坏块（**全仓唯一的「这块坏不坏」判据**）
 *
 * ① 读 Page 0 的 spare 首字节（**列 2048**，`PAGE_MAIN_SIZE`），非 0xFF ⇒ 出厂坏块；
 *    **读失败也判坏块**（列读不通 = 该页不可读，保守归入坏块，与 ② 的擦除失败同调）。
 *    **必须在擦除之前读**——器件手册（Micron AN1819）逐字：*"the bad block information is
 *    erasable and cannot be recovered once erased"*，且 *"System software should check the
 *    first spare area location on the first page of each block prior to performing any
 *    PROGRAM or ERASE operations"*。（旧实现的注释写「擦不掉的硬标记」是**错的**，
 *    这正是它先擦后读、结果永远全 FF 的根因。）
 * ② do_erase 非 0 时**无条件**擦除该块；擦除失败 ⇒ 坏块（stuck-at-0 / 不可擦，运行时坏块的子集）。
 *    ⚠ 擦除**不能**因为 ① 命中就跳过——见下方函数体内的说明。
 *
 * 列 2048 只能靠 OP_READ_COLUMN（列地址走命令 param）够到：OP_READ_META 的列 =
 * cw × CW_SIZE（cw 取自 PBA 的 cw 字段），cw ∈ {0,1,2,3} ⇒ 列只能是 {0, 528, 1056, 1584}。
 * 4 字节 = spare[0..3]（2 字节坏块信息 + 2 字节 user metadata II），标记看 [0]。
 *
 * ⚠ **① 只在空片上才是「出厂标记」**。用过的块，spare 首字节是 FTL 自己的数据
 *   （一次页编程写 2112B，CW3 尾巴 64 字节溢进备区）⇒ 本函数在用过的介质上回答的
 *   其实是「这块被写过没有」。故它**不可**用在任何会落盘的路径上，除非调用方已经
 *   确认介质是空片（bbt_medium_has_ftl_data）。
 *
 * 调用者：bbt_scan_factory（出厂扫描，do_erase=0，**调用方已确认空片**）/
 *         bbt_print_bad_map（诊断，按 CLI——读图的人须知这不是坏块图）/
 *         注：bbt_erase_all_blocks **已不再**调用本函数（见其注释）。
 */
static bool bbt_probe_block_bad(ftl_allocator_t *alloc, uint8_t plane, uint16_t block, uint8_t do_erase)
{
    /* page / cw / lun 恒 0：擦除不看它们；列地址由 ftl_sync_read_column 显式给，
     * 不依赖 PBA 的 cw 字段。 */
    nand_pba_t addr = pba_make(alloc->ch, alloc->ce, 0U, block, plane, 0U, 0U);

    /* ① 出厂标记（擦除前读，擦除会永久毁掉它）。读失败 ⇒ 坏块。 */
    uint8_t mark[4];
    (void)memset(mark, 0xFF, sizeof(mark));

    bool marked = (ftl_sync_read_column(addr, (uint16_t)PAGE_MAIN_SIZE, mark) != FTL_OK) || (mark[0] != 0xFFU);

    /* ② 可选擦除——**无条件发**。
     * 旧实现在 ① 命中时直接 `return 1` 提前返回，于是**被标记的块永远不被擦**。而"全片擦除"
     * 的全部意义恰恰是连这批块一起抹掉：元数据区（0..FTL_METADATA_BLOCK_END）里的 L2P/
     * Anchor/Checkpoint/BBT/Journal/Wear 块**正是** ① 会命中的那批（FTL 写过 ⇒ 列 2048
     * 非 0xFF —— 注意这批的命中含义是「写过」而不是「坏」）。跳过它们 ⇒ 擦完断电重启，
     * FTL 仍从原处读回旧 checkpoint/wear/BBT，`free`/`cp_seq` 一个不少 —— 表现出来
     * 就是"发了擦除命令但没擦掉"。 */
    if (do_erase != 0U)
    {
        if (ftl_sync_erase_block(addr) != FTL_OK)
        {
            return true; /* 擦不掉 ⇒ 坏块（stuck-at-0 / 不可擦） */
        }
    }

    return marked; /* 坏不坏仍按**擦除前**读到的出厂标记报（擦后标记已丢，不可再读） */
}

/* --------------------------------------------------------------------
 * bbt_erase_all_blocks
 *   功能：擦除本实例**全部**块（所有 plane），供开机自动格式化调用。
 *   参数：alloc —— 分配器；**前置条件**，非法即 ASSERT。
 *   返回：FTL_OK / FTL_ERROR（擦除失败）。
 *   实现要点：逐块**无条件**擦（含已判坏的块——"全片擦除"必须连坏块一起抹掉，
 *             否则元数据块被跳过 ⇒ 擦完 FTL 仍读回旧状态）。擦除失败 ⇒ 坏块
 *             （stuck-at-0 / 不可擦）——这条判据与介质是否空片无关，任何介质都成立。
 *
 *   ⚠ **本函数不再读出厂坏块标记**（改前它复用 bbt_probe_block_bad(..., do_erase=1)
 *     顺带读）。原因：用过的块 Page 0 的 spare 首字节是 FTL 数据而非标记（见
 *     bbt_medium_has_ftl_data 与 bbt_scan_factory 的说明），在**版本升级**这条正常
 *     路径上（checkpoint 版本不符 ⇒ 格式化，但介质是旧的、写满了数据）会把所有写过的
 *     块标成坏块，连元数据块一起——那不只是掉容量，是开不了机。
 *     标记的读取职责现在**唯一**归 bbt_init（且只在空片时做，见 bbt_scan_factory）。
 *
 *   ⚠ 因此「必须在 bbt_init 之后调用」这条**是承重的**：出厂标记此时已在 BST 里，
 *     擦除不会丢坏块信息。擦完由调用方 bbt_save 重新持久化坏块图。
 * -------------------------------------------------------------------- */
ftl_status_t bbt_erase_all_blocks(ftl_allocator_t *alloc)
{
    /* 前置条件（判据 2）：唯一调用者 power_failure_recovery.c 传 &ftl->alloc。 */
    ASSERT(alloc != NULL);
    ASSERT(alloc->blocks != NULL);

    for (uint8_t plane = 0U; plane < PLANE_NUM; plane++)
    {
        for (uint16_t block = 0U; block < BLOCKS_PER_PLANE; block++)
        {
            const uint16_t   gblock = ftl_gblock(plane, block);
            const nand_pba_t addr   = pba_make(alloc->ch, alloc->ce, 0U, block, plane, 0U, 0U);

            if (ftl_sync_erase_block(addr) == FTL_OK)
            {
                continue;
            }

            /* 擦不掉 ⇒ 坏块（已是坏块的不重复计数） */
            if (!ftl_block_is_bad(&alloc->blocks[gblock]))
            {
                ftl_block_set_bad(&alloc->blocks[gblock]); /* 终态：互斥，无需清位 */
                ftl_block_set_sb(&alloc->blocks[gblock], FTL_BLOCK_NO_SB);
                alloc->bad_count++;
            }
        }
    }

    alloc->bbt.dirty = 1U;

    return FTL_OK;
}

/* --------------------------------------------------------------------
 * bbt_print_bad_map
 *   功能：打印两 plane 并排的坏块图（CLI `bbt` 命令）；坏块打绝对块号，好块打 "FF"。
 *   参数：alloc —— 分配器（**前置条件**，非法即 ASSERT）；do_erase —— 非 0 时逐块擦除（全片擦除工具）。
 *   返回：无。
 *   实现要点：每格定宽 BBT_MAP_ENTRY_W，两 plane 并排（行长受终端宽度约束，见宏注释）；
 *             坏块打的是**本 plane 绝对块号**而非排内列号（避免与 FF 并排被误读成十六进制）。
 * -------------------------------------------------------------------- */
void bbt_print_bad_map(ftl_allocator_t *alloc, uint8_t do_erase)
{
    char line[BBT_MAP_LINE_MAX];

    /* 前置条件（判据 2）：唯一调用者 cli.c 已判 ftl 非空后传 &ftl->alloc。 */
    ASSERT(alloc != NULL);

    LOGI("CH=%u  CE=%u  LUN=%u\r\n", (unsigned)alloc->ch, (unsigned)alloc->ce, (unsigned)alloc->lun);

    /* ── PLANE 标签行：每 plane 一组，标签在组内居中 ── */
    {
        int n = 0;

        for (uint8_t plane = 0U; plane < PLANE_NUM; plane++)
        {
            const int label_w = 7; /* "PLANE=k" */
            const int pad     = (int)((BBT_MAP_GROUP_W - (uint16_t)label_w) / 2U);
            const int trail   = (int)BBT_MAP_GROUP_W - label_w - pad;

            if (plane > 0U)
            {
                n += snprintf(&line[n], sizeof(line) - (size_t)n, "   ");
            }
            n += snprintf(&line[n], sizeof(line) - (size_t)n, "%*sPLANE=%u%*s", pad, "", (unsigned)plane, trail, "");
        }
        LOGI("%s\r\n", line);
    }

    /* ── 数据行：一排同时打各 plane 的同段块号（块号 = row × 列数 + col，格内打的是块号本身）── */
    uint16_t rows = (uint16_t)(BLOCKS_PER_PLANE / BBT_MAP_COLS_PER_PLANE);
    for (uint16_t row = 0U; row < rows; row++)
    {
        int n = 0;

        for (uint8_t plane = 0U; plane < PLANE_NUM; plane++)
        {
            if (plane > 0U)
            {
                n += snprintf(&line[n], sizeof(line) - (size_t)n, "   ");
            }

            for (uint16_t col = 0U; col < BBT_MAP_COLS_PER_PLANE; col++)
            {
                const uint16_t block = (uint16_t)((row * BBT_MAP_COLS_PER_PLANE) + col);

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
                    /* 坏块打**本 plane 的绝对块号**（0..BLOCKS_PER_PLANE-1），不是排内列号：
                     * 列号每排重新计数，读图的人得自己算 排号×列数+列号，且 1~2 位的数字
                     * 与 `FF` 并排极易被误读成十六进制字节值（上一版正是这么被误读的）。 */
                    n += snprintf(&line[n], sizeof(line) - (size_t)n, "%*u", (int)BBT_MAP_ENTRY_W, (unsigned)block);
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

/* ── 3. 标坏 / 持久化 ── */

/* --------------------------------------------------------------------
 * bbt_mark_bad
 *   功能：运行时标坏一个块——置 BST BAD 位、清有效位图/状态/反向索引、计数、置 dirty，
 *         并从 Spare Pool 补一块进 Free Pool。
 *   参数：alloc —— 分配器；gblock —— 全局块号。
 *   返回：FTL_OK；FTL_ERROR 参数非法 / 越界。
 *   实现要点：清有效位图是必需的——坏块后残留的 VPC/bitmap 会让 GC/WL 误判其为可回收块；
 *             **不**同步 bbt_save（持久化交后台 bbt_kick 批量）；Spare 耗尽不影响标坏（容量自然下降）。
 * -------------------------------------------------------------------- */
ftl_status_t bbt_mark_bad(ftl_allocator_t *alloc, uint16_t gblock)
{
    if ((alloc == NULL) || (gblock >= BBT_MAX_BLOCKS) || (alloc->blocks == NULL))
    {
        return FTL_ERROR;
    }

    /* 驱逐出 Free Pool —— **必须先于置 BAD**：allocator_remove_from_free 校验「在池 ⇒ state == FREE」
     * （allocator.h 不变量②）。块若在池里，此刻它正是 FREE；先置 BAD 再出池就会命中断言。
     * 不驱逐则留下「在池却不是 FREE」的反向不一致（运行时标坏：擦除失败 / 预测性标坏）。
     * 不在池内 ⇒ no-op（allocator_remove_from_free 的文档化契约，判据 4）。
     * 注：出厂扫描 / BBT 加载路径跑在建池之前（池还是空的）⇒ 恒 no-op，无需特判。 */
    allocator_remove_from_free(alloc, gblock);

    /* BST 置 BAD（单数据源，后台 bbt_kick 持久化到 NAND）。BAD 是状态机**终态** ⇒
     * 一行赋值即抹掉 OPEN/CLOSED/FREE/LOG（原掩码漏位的风险消失）；温度字段不动（与状态正交）。
     * 同时清空有效计数——坏块后该块不会再产生有效 CW，
     * 残留的 valid_count 会让 GC/WL 误判其为可回收块。 */
    ftl_block_set_bad(&alloc->blocks[gblock]);
    ftl_block_clear_valid(&alloc->blocks[gblock]);
    ftl_block_set_sb(&alloc->blocks[gblock], FTL_BLOCK_NO_SB);
    alloc->bad_count++;
    alloc->bbt.dirty = 1U;

    /* 不再同步调 bbt_save() — NAND BBT 持久化由后台 bbt_kick 批量执行。
     * 坏块替换：从**本块所属 plane** 的 Spare Pool 取块补 free pool
     * （保容量水位，ADR-11 Block Sparing）。失败（Spare 耗尽）不影响标坏，容量自然下降。 */
    (void)bbt_spare_pool_refill(alloc, ftl_gblock_plane(gblock));

    return FTL_OK;
}

/* --------------------------------------------------------------------
 * bbt_save
 *   功能：把 BST 里的坏块集重建为紧凑 bitmap，组一条 BBT header 追加写进 BBT 池。
 *   参数：alloc —— 分配器；**前置条件**，非法即 ASSERT。
 *   返回：FTL_OK 成功（含 dirty==0 早退）；FTL_ERROR 池定位失败 / prepare 失败 / 写失败。
 *   实现要点：游标未建立时先定位池内最新 record 的下一槽；**无论成败都推进游标**
 *             （半编程槽不可复用）；失败保留 dirty 供下次重试。NAND 格式一字不改。
 * -------------------------------------------------------------------- */
ftl_status_t bbt_save(ftl_allocator_t *alloc)
{
    /* 前置条件（判据 2）：唯一调用者 power_failure_recovery.c 传 &ftl->alloc。 */
    ASSERT(alloc != NULL);

    if (alloc->bbt.dirty == 0U)
    {
        return FTL_OK;
    }

    bbt_probe_ctx_t ctx;
    bbt_ctx_init(alloc, &ctx);

    /* 游标未建立 ⇒ 先定位池内最新有效 record 的下一槽 */
    if (alloc->bbt.cursor_valid == 0U)
    {
        uint16_t newest = UINT16_MAX;

        if (metadata_ring_locate(&ctx.ring, bbt_record_probe, &ctx, &newest, &alloc->bbt.sequence, &alloc->bbt.write_idx) != FTL_OK)
        {
            return FTL_ERROR;
        }
        alloc->bbt.cursor_valid = 1U;
    }

    alloc->bbt.sequence++;

    bbt_header_t header;
    memset(&header, 0, sizeof(header));
    header.magic        = BBT_MAGIC;
    header.sequence     = alloc->bbt.sequence;
    header.total_blocks = BBT_MAX_BLOCKS;
    header.bad_count    = alloc->bad_count;

    /* 从 BST 重建 NAND 持久化 bitmap（NAND 格式一字不改） */
    for (uint16_t gblock = 0U; gblock < BBT_MAX_BLOCKS; gblock++)
    {
        if (ftl_block_is_bad(&alloc->blocks[gblock]))
        {
            bbt_set_bad(header.bad_map, gblock);
        }
    }

    /* 追加写：池内下一槽（块满则先擦池内下一块）。
     * 无论成败都推进游标（已写过的槽可能半编程，不可复用）。 */
    nand_pba_t addr;
    if (metadata_ring_prepare_write(&ctx.ring, &ctx.topo, alloc->bbt.write_idx, &addr) != FTL_OK)
    {
        return FTL_ERROR;
    }

    if (ftl_sync_write_page(addr, &header, sizeof(header), FTL_PAGE_CFG_META()) != 0)
    {
        alloc->bbt.write_idx = (uint16_t)((alloc->bbt.write_idx + 1U) % metadata_ring_total_records(&ctx.ring));
        return FTL_ERROR; /* dirty 保留 ⇒ 下次重试 */
    }

    alloc->bbt.write_idx = (uint16_t)((alloc->bbt.write_idx + 1U) % metadata_ring_total_records(&ctx.ring));

    alloc->bbt.dirty = 0U;
    return FTL_OK;
}

/* ── 3b. 异步持久化 job（v0.75 阶段 7）── */

/* --------------------------------------------------------------------
 * bbt_submit_write
 *   功能：把当前 BST 重建为一条 BBT record，**异步**提交写（phase = BBT_JOB_WRITE）。
 *   参数：alloc —— 分配器（写点 = alloc->bbt.write_idx）。
 *   返回：FTL_OK 已提交（waiting = 1）；FTL_ERROR 建链/提交失败（**waiting 保持 0**，
 *         调用者停在 BBT_JOB_WRITE 等下一轮 kick 续跑——**不重擦块**）。
 *   实现要点：record 内容在**提交时**从 BST 重建（不是起步时快照）——换块擦的等待
 *             期间新标坏的块会被一并记进本条 record，是严格的信息超集。
 *             header 是**栈上** 540 B：ftl_async_write_page 在 prepare 时已把它拷进
 *             FC buffer 池（同 journal_freeze 的做法，不需要为异步保留快照缓冲）。
 *             地址按 write_idx 重算（write_idx 写成功才推进 ⇒ 此刻它仍指向本槽）。
 * -------------------------------------------------------------------- */
static ftl_status_t bbt_submit_write(ftl_allocator_t *alloc)
{
    bbt_probe_ctx_t ctx;
    bbt_ctx_init(alloc, &ctx);

    bbt_header_t header;
    memset(&header, 0, sizeof(header));
    header.magic        = BBT_MAGIC;
    header.sequence     = alloc->bbt.sequence;
    header.total_blocks = BBT_MAX_BLOCKS;
    header.bad_count    = alloc->bad_count;

    /* 从 BST 重建 NAND 持久化 bitmap（NAND 格式一字不改，同 bbt_save） */
    for (uint16_t gblock = 0U; gblock < BBT_MAX_BLOCKS; gblock++)
    {
        if (ftl_block_is_bad(&alloc->blocks[gblock]))
        {
            bbt_set_bad(header.bad_map, gblock);
        }
    }

    nand_pba_t addr = metadata_ring_addr(&ctx.ring, &ctx.topo, alloc->bbt.write_idx, 0U);

    fc_command_t *cmd;
    if (ftl_async_write_page(&cmd, addr, &header, sizeof(header), FTL_PAGE_CFG_META()) != FTL_OK)
    {
        return FTL_ERROR;
    }
    if (ftl_op_submit(cmd, FTL_OP_BBT, alloc) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    alloc->bbt.job.phase   = (uint8_t)BBT_JOB_WRITE;
    alloc->bbt.job.waiting = 1U;
    return FTL_OK;
}

/* --------------------------------------------------------------------
 * bbt_kick
 *   功能：后台批量持久化的**起步 / 续跑**入口（提交即返回）。
 *   参数：alloc —— 分配器。
 *   返回：无（提交即返回；成败由 ftl_op_bbt 收尾）。
 *   实现要点：
 *     · **单飞行**：① 命令在飞 ⇒ 直接返回；② phase == WRITE 且不在飞 ⇒ 上一步提交
 *       被背压拒，只续提交写（**不重擦**——擦已成功，重擦纯属磨损）；③ 其余非 IDLE
 *       （= ERASE 在飞）⇒ 返回；④ IDLE 才装配新 job。
 *     · 游标未建立时的 `metadata_ring_locate` 是**同步**的（扫池逐页读），但只在
 *       首次落盘发生一次，且本函数由 ftl_service 的**后台档**调用（不在完成泵内）
 *       ⇒ 不是嵌套泵。job 的其余步骤全部"提交即返回"。
 *     · sequence 在**起步时**自增（与旧 bbt_save 同位置）；续跑路径不会再自增。
 *       擦失败 ⇒ 回 IDLE、dirty 保留、write_idx 不动，下轮 kick 从头再来
 *       （与旧 bbt_save 每次调用重试的语义一致）。
 * -------------------------------------------------------------------- */
void bbt_kick(ftl_allocator_t *alloc)
{
    /* 前置条件（判据 2）：唯一调用者 ftl.c 传 &ftl->alloc。 */
    ASSERT(alloc != NULL);
    ASSERT(alloc->blocks != NULL);

    /* 心跳（见 ftl_job.h）：job 在飞期间每满 1s 打一行 phase/age */
    if (alloc->bbt.job.phase == (uint8_t)BBT_JOB_IDLE)
    {
        ftl_job_heartbeat_reset(&alloc->bbt.job.log_ms, &alloc->bbt.job.start_ms);
    }
    else if (ftl_job_heartbeat(&alloc->bbt.job.log_ms, &alloc->bbt.job.start_ms))
    {
        LOGI("[BBT] job phase=%u waiting=%u seq=%lu idx=%u age=%lums\r\n",
             (unsigned)alloc->bbt.job.phase,
             (unsigned)alloc->bbt.job.waiting,
             (unsigned long)alloc->bbt.sequence,
             (unsigned)alloc->bbt.write_idx,
             (unsigned long)ftl_job_age_ms(&alloc->bbt.job.start_ms));
    }

    if (alloc->bbt.job.waiting != 0U)
    {
        return; /* ① 命令在飞 */
    }
    if (alloc->bbt.job.phase == (uint8_t)BBT_JOB_WRITE)
    {
        (void)bbt_submit_write(alloc); /* ② 背压后续提交（擦已完成） */
        return;
    }
    if (alloc->bbt.job.phase != (uint8_t)BBT_JOB_IDLE)
    {
        return; /* ③ ERASE 在飞（waiting 必为 1，此处只是防御） */
    }

    if (alloc->bbt.dirty == 0U)
    {
        return; /* ④ 无变更 */
    }

    bbt_probe_ctx_t ctx;
    bbt_ctx_init(alloc, &ctx);

    /* 游标未建立 ⇒ 先定位池内最新有效 record 的下一槽 */
    if (alloc->bbt.cursor_valid == 0U)
    {
        uint16_t newest = UINT16_MAX;

        if (metadata_ring_locate(&ctx.ring, bbt_record_probe, &ctx, &newest, &alloc->bbt.sequence, &alloc->bbt.write_idx) != FTL_OK)
        {
            LOGE("[BBT] pool locate failed — 下轮重试\r\n");
            return;
        }
        alloc->bbt.cursor_valid = 1U;
    }

    alloc->bbt.sequence++;

    nand_pba_t addr;
    int        need_erase = 0;
    if (metadata_ring_prepare_addr(&ctx.ring, &ctx.topo, alloc->bbt.write_idx, &addr, &need_erase) != FTL_OK)
    {
        LOGE("[BBT] prepare failed idx=%u\r\n", (unsigned)alloc->bbt.write_idx);
        return;
    }

    if (need_erase != 0)
    {
        fc_command_t *cmd;

        /* 换块：先擦目标块（池内轮转 ⇒ 该块持全池最旧 record，擦掉不丢信息） */
        if (ftl_async_erase_block(&cmd, addr) != FTL_OK)
        {
            return; /* 命令池空 / 建链失败 ⇒ 下轮重试（write_idx 未动） */
        }
        if (ftl_op_submit(cmd, FTL_OP_BBT, alloc) != FTL_OK)
        {
            (void)fc_command_release(cmd);
            return;
        }
        alloc->bbt.job.phase   = (uint8_t)BBT_JOB_ERASE;
        alloc->bbt.job.waiting = 1U;
        return;
    }

    /* 不必换块 ⇒ 直接起写。先落 phase，提交被背压拒就停在 WRITE/waiting=0 等续跑 */
    alloc->bbt.job.phase   = (uint8_t)BBT_JOB_WRITE;
    alloc->bbt.job.waiting = 0U;
    (void)bbt_submit_write(alloc);
}

/* --------------------------------------------------------------------
 * ftl_op_bbt
 *   功能：BBT 持久化 job 的完成服务函数（g_op_service[FTL_OP_BBT]）。
 *   参数：ftl —— 实例（**不用**：ctx 已是本实例的 allocator）；
 *         completion —— 完成状态；ctx —— `ftl_allocator_t *`（ftl_op_submit 时传）。
 *   返回：无。命令的 release 归本函数。
 *   实现要点：失败分支**逐条复刻**旧 bbt_save 的语义——
 *     · 擦失败 ⇒ write_idx **不推进**（旧 metadata_ring_prepare_write 返回 FTL_ERROR
 *       的那条分支），dirty 保留；
 *     · 写失败 ⇒ 游标**照推**（已写过的槽可能半编程，不可复用；旧 bbt_save 如此），
 *       dirty 保留供下次重试；
 *     · 写成功 ⇒ 游标照推 + 清 dirty。
 *   ⚠ 本函数**只**收尾/续提交，不调 bbt_kick（续提交是提交侧动作，无需嵌套泵）。
 * -------------------------------------------------------------------- */
void ftl_op_bbt(struct ftl_context *ftl, const fc_command_status_t *completion, void *ctx)
{
    ftl_allocator_t *alloc = (ftl_allocator_t *)ctx;

    (void)ftl; /* 实例无关：ctx 就是本实例的 allocator */

    ASSERT(alloc != NULL);
    ASSERT(completion != NULL);

    fc_command_t *cmd    = fc_command_id_to_command((uint16_t)completion->command_id);
    uint8_t       failed = ((completion->status & FC_CW_STATUS_ERROR_MASK) != 0U) ? 1U : 0U;

    if (alloc->bbt.job.phase == (uint8_t)BBT_JOB_ERASE)
    {
        if (failed != 0U)
        {
            LOGE("[BBT] ring block erase failed status=0x%02x — 游标不推进，下轮重试\r\n",
                 (unsigned)completion->status);
            alloc->bbt.job.phase   = (uint8_t)BBT_JOB_IDLE;
            alloc->bbt.job.waiting = 0U;
            (void)fc_command_release(cmd);
            return;
        }

        /* 擦成功 ⇒ 起写。先落 phase，提交被背压拒就停在 WRITE/waiting=0 等续跑
         * （**不再回 IDLE**——回 IDLE 会让下轮 kick 重走 prepare_addr 再擦一次同一块）。 */
        alloc->bbt.job.phase   = (uint8_t)BBT_JOB_WRITE;
        alloc->bbt.job.waiting = 0U;
        (void)bbt_submit_write(alloc);
        (void)fc_command_release(cmd);
        return;
    }

    if (alloc->bbt.job.phase == (uint8_t)BBT_JOB_WRITE)
    {
        bbt_probe_ctx_t rctx;

        bbt_ctx_init(alloc, &rctx);
        /* **无论成败都推进游标**（与旧 bbt_save 一致） */
        alloc->bbt.write_idx = (uint16_t)((alloc->bbt.write_idx + 1U) % metadata_ring_total_records(&rctx.ring));

        if (failed != 0U)
        {
            LOGE("[BBT] record write failed status=0x%02x — dirty 保留\r\n", (unsigned)completion->status);
        }
        else
        {
            alloc->bbt.dirty = 0U;
        }
        alloc->bbt.job.phase   = (uint8_t)BBT_JOB_IDLE;
        alloc->bbt.job.waiting = 0U;
        (void)fc_command_release(cmd);
        return;
    }

    /* 不该发生：一条命令对应一个 job，phase 必为 ERASE / WRITE */
    LOGE("[BBT] unexpected completion phase=%u\r\n", (unsigned)alloc->bbt.job.phase);
    alloc->bbt.job.phase   = (uint8_t)BBT_JOB_IDLE;
    alloc->bbt.job.waiting = 0U;
    (void)fc_command_release(cmd);
}

/* --------------------------------------------------------------------
 * bbt_load
 *   功能：开机从 BBT 池定位并读回最新有效 BBT header，输出 bitmap/坏块数/序号。
 *   参数：alloc —— 分配器（池路由取 alloc 的 ch/ce/lun，游标/序号写回 alloc->bbt）；
 *         bad_map[BBT_BITMAP_SIZE] —— 出参位图；bad_count/sequence —— 出参。
 *         ⚠ 四个指针都是**前置条件**（唯一调用者 bbt_init 传已断言的 alloc + 局部变量），
 *         非法即 ASSERT。
 *   返回：FTL_OK 成功；FTL_ERROR 池内无有效 record / 重读失败 / 校验不符。
 *   实现要点：进入即复位 alloc->bbt（找不到时保持 0/无效 ⇒ 后续 save 从池首开始）；
 *             locate 只回序号，header 本体要按该序号**再读一次**并复核 sequence。
 * -------------------------------------------------------------------- */
ftl_status_t bbt_load(ftl_allocator_t *alloc, uint8_t *bad_map, uint32_t *bad_count, uint32_t *sequence)
{
    /* 死守卫（判据 2 / §十附判据 2）：唯一调用者 bbt_init（同文件）传已断言的 alloc、
     * 局部数组 tmp_bad_map 与两个局部变量地址——四个指针结构上恒非空。 */

    /* 复位本实例池状态：无有效 record 时保持 0/无效（后续 bbt_save 从池首开始）*/
    alloc->bbt.sequence     = 0U;
    alloc->bbt.write_idx    = 0U;
    alloc->bbt.cursor_valid = 0U;

    bbt_probe_ctx_t ctx;
    bbt_ctx_init(alloc, &ctx);

    uint16_t newest = UINT16_MAX;
    if (metadata_ring_locate(&ctx.ring, bbt_record_probe, &ctx, &newest, &alloc->bbt.sequence, &alloc->bbt.write_idx) != FTL_OK)
    {
        return FTL_ERROR;
    }
    if (newest == UINT16_MAX)
    {
        alloc->bbt.sequence  = 0U;
        alloc->bbt.write_idx = 0U;
        return FTL_ERROR; /* 池内无有效 record */
    }

    alloc->bbt.cursor_valid = 1U;

    /* 重读胜者（locate 只返回序号，Header 本体要再读一次）*/
    nand_pba_t   addr = metadata_ring_addr(&ctx.ring, &ctx.topo, newest, 0U);
    bbt_header_t best;
    if (ftl_sync_read_page(addr, &best, sizeof(best), FTL_PAGE_CFG_META()) != 0)
    {
        return FTL_ERROR;
    }
    if ((!bbt_header_valid(&best)) || (best.sequence != alloc->bbt.sequence))
    {
        return FTL_ERROR;
    }

    /* 输出到 caller */
    memcpy(bad_map, best.bad_map, BBT_BITMAP_SIZE);
    *bad_count = best.bad_count;
    *sequence  = best.sequence;

    return FTL_OK;
}

/* ── 4. 坏块替换 ── */

/* --------------------------------------------------------------------
 * bbt_spare_pool_refill
 *   功能：坏块替换——从**指定 plane 的** Spare Pool 取一个好块归还 Free Pool，
 *         保用户容量水位。
 *   参数：alloc —— 分配器；plane —— 坏块所属 plane（取同 plane 的替换块）。
 *   返回：FTL_OK 成功；FTL_ERROR 参数非法 / 该 plane Spare Pool 耗尽（容量自然下降）。
 *   实现要点：只出不进（出厂每 plane 预留 FTL_SPARE_POOL_PER_PLANE 个）。调用者保证
 *             该块已擦除（allocator_free_block 契约）。
 *             ⚠ 必须按 plane 取：Spare 池按 super block 粒度预留，补错 plane 不恢复该
 *             plane 的配对能力（原实现从全局池 LIFO 弹出，实际总补 plane 0）。
 * -------------------------------------------------------------------- */
ftl_status_t bbt_spare_pool_refill(ftl_allocator_t *alloc, uint8_t plane)
{
    /*
     * Spare Pool 坏块替换：出厂时 main.c 已为**每个 plane** 预留
     * FTL_SPARE_POOL_PER_PLANE=50 个好块到 alloc->spare_blocks[plane]。运行时
     * bbt_mark_bad 标坏一块后，从该块所属 plane 取一个好块 allocator_free_block
     * 补进 free pool，保用户容量水位。
     * 只出不进，该 plane Spare Pool 耗尽后返回 FTL_ERROR（容量自然下降）。
     */
    if ((alloc == NULL) || (alloc->spare_blocks == NULL) || (plane >= PLANE_NUM))
    {
        return FTL_ERROR;
    }

    uint16_t gblock;
    if (!allocator_spare_try_pop(alloc, plane, &gblock))
    {
        return FTL_ERROR; /* 该 plane Spare Pool 耗尽 */
    }
    allocator_free_block(alloc, gblock);

    return FTL_OK;
}

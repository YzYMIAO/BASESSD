#include "ftl/bbt.h"

#include "common/boot_policy.h"
#include "common/log.h"
#include "fc/fc.h"
#include "ftl/ftl_fc_interface.h"
#include "ftl/metadata.h" /* metadata_ring_*（BBT 池轮转原语）*/
#include "nand/onfi_1_0.h"

#include <stdio.h>
#include <string.h>

/*
 * BBT 内部状态（全局单例）。
 * 坏块位图已并入 ftl_block_t.state（Block State Table 单数据源），
 * NAND 持久化仍用紧凑 bad_map[512] 格式（bbt_header_t，一字不改），
 * bbt_save 时从 BST 重建 bitmap。
 */
static uint32_t g_bbt_sequence = 0U; /* 当前 sequence（本池自己的单调序号）*/
static int      g_bbt_dirty    = 0;  /* 是否有未写回 NAND 的变更 */

/* BBT 池写游标（开机由 bbt_load 恢复）。⚠ file-scope static：多实例局限同 checkpoint/wear */
static uint16_t g_bbt_write_idx    = 0U;
static uint8_t  g_bbt_cursor_valid = 0U;

/* ── BBT 池：环形轮转（一条 record = 1 页）── */
static void bbt_ring_init(metadata_ring_t *ring)
{
    ring->first_block      = BBT_BLOCK;
    ring->block_count      = BBT_POOL_BLOCKS;
    ring->pages_per_record = 1U;
    ring->plane            = 0U;
    ring->module           = FC_COMMAND_MODULE_BBT;
}

/* 整条校验（一条 record = 1 页）：magic + CRC */
static int bbt_header_valid(const bbt_header_t *hdr)
{
    if (hdr->magic != BBT_MAGIC)
    {
        return 0;
    }

    return (boot_crc32(hdr, sizeof(*hdr) - sizeof(hdr->crc)) == hdr->crc) ? 1 : 0;
}

typedef struct
{
    ftl_topology_t  topo;
    metadata_ring_t ring;
} bbt_probe_ctx_t;

/* BBT record 探针（见 metadata_ring_probe_fn）。full 与廉价探针等价（record 只有一页）。 */
static ftl_status_t bbt_record_probe(void *ctx, uint16_t record_idx, int full, uint32_t *out_sequence)
{
    bbt_probe_ctx_t *c = (bbt_probe_ctx_t *)ctx;
    bbt_header_t     hdr;
    nand_addr_t      addr;

    (void)full;

    addr = metadata_ring_addr(&c->ring, &c->topo, record_idx, 0U);

    if (ftl_sync_read_page(addr, &hdr, sizeof(hdr), FTL_PAGE_CFG_META(FC_COMMAND_MODULE_BBT)) != 0)
    {
        return FTL_ERROR;
    }
    if ((bbt_header_valid(&hdr) == 0) || (hdr.sequence == 0U))
    {
        return FTL_ERROR; /* sequence 从 1 起；0 = 未写 */
    }

    *out_sequence = hdr.sequence;
    return FTL_OK;
}

ftl_status_t bbt_init(ftl_allocator_t *alloc)
{
    uint32_t seq;
    int      ret;
    uint32_t loaded_bad_count = 0U;

    if (alloc == NULL)
    {
        return FTL_ERROR;
    }

    g_bbt_sequence = 0U;
    g_bbt_dirty    = 0;

    /* 尝试从 NAND 加载已存在的 BBT */
    {
        uint8_t tmp_bad_map[BBT_BITMAP_SIZE];

        ftl_topology_t topo = {.ch = alloc->ch, .ce = alloc->ce, .lun = alloc->lun};
        ret                 = bbt_load(tmp_bad_map, &loaded_bad_count, &seq, topo);
        if (ret == 0)
        {
            uint16_t gblock;
            uint16_t bad_idx = 0U;

            g_bbt_sequence   = seq;
            alloc->bad_count = (uint16_t)loaded_bad_count;

            /* 从 NAND bitmap 重建 BST is_bad 位（单数据源） */
            for (gblock = 0U; gblock < BBT_MAX_BLOCKS; gblock++)
            {
                if (bbt_is_bad(tmp_bad_map, gblock))
                {
                    ftl_block_set_bad(&alloc->blocks[gblock]);
                    ftl_block_state_clr(&alloc->blocks[gblock],
                                        FTL_BLOCK_STATE_OPEN | FTL_BLOCK_STATE_CLOSED |
                                            FTL_BLOCK_STATE_FREE);
                    ftl_block_set_sb(&alloc->blocks[gblock], FTL_BLOCK_NO_SB);
                    bad_idx++;
                }
            }
            alloc->bad_count = bad_idx;

            return FTL_OK;
        }
    }

    /* 无有效 BBT → 执行出厂扫描 */
    return bbt_scan_factory(alloc);
}

int bbt_scan_factory(ftl_allocator_t *alloc)
{
    uint16_t gblock;
    uint16_t bad_idx = 0U;

    if (alloc == NULL)
    {
        return FTL_OK;
    }

    g_bbt_dirty = 0;

    /*
     * 扫描策略：
     *
     * 标准 NAND 出厂坏块标记在 Page 0/1 的 OOB 区域第一个字节。
     * 当前 FC 层没有独立 OOB 读取接口，采用替代策略：
     *
     * 以 LDPC_RAW 模式（跳过 ECC）读取 Page 0 的原始 CW 数据。
     * 如果读取完全失败（FTL_RESULT_FAIL），判断为出厂坏块。
     *
     * 注意：LDPC_RAW 模式下 CW 数据仍然可以被读取，但 full-FF
     *   可能是空块的特征，不是坏块特征。真正的 OOB 检测需要
     *   FC 层扩展 spare 区读取功能。
     */
    for (gblock = 0U; gblock < BBT_MAX_BLOCKS; gblock++)
    {
        uint8_t     raw_buf[PAGE_SIZE];
        nand_addr_t page_addr0;
        int         ret0;

        /* 跳过 Block 0（出厂总是好块）*/
        if (gblock == 0U)
        {
            continue;
        }

        /* 尝试读 Page 0（全局号 → 各 Plane 的物理块号） */
        {
            uint8_t  plane = (uint8_t)(gblock / BLOCKS_PER_PLANE);
            uint16_t block = (uint16_t)(gblock % BLOCKS_PER_PLANE);
            page_addr0     = addr_from_paa(alloc->ch, alloc->ce, paa_from_addr(&(nand_addr_t){.plane = (uint8_t)plane, .block = (uint16_t)block}));
        }

        memset(raw_buf, 0xFF, sizeof(raw_buf));
        ret0 = ftl_sync_read_page_raw(page_addr0, raw_buf, FC_COMMAND_MODULE_BBT);

        /*
         * LDPC_RAW 模式下，即使 NAND 物理失败，
         * fc_poll_completion 仍然返回 status=READY（raw 模式无 ECC 错误）。
         * 所以用返回码判断坏块不一定可靠。
         *
         * 替代方案：检查 Page 0 的数据是否为全 0xFF（空块特征）。
         * 出厂好块的 Page 0 数据区不为全 FF（有坏块标记等）。
         * 出厂坏块的 Page 0 可能全 FF 或读取失败。
         *
         * 当前实现：
         *   只以**返回码**判定——ret0 != 0 才标记坏块（见下方保守策略）。
         *   raw_buf 全 FF 判据**未启用**：空块与坏块的 raw 数据都可能是全 FF，
         *   无法区分；出厂商预写到 spare 区的标识被跳过（FC 层当前只读 USER 数据区）。
         *
         * 更准确的检测需要 FC 提供 spare/OOB 读取接口。
         */

        /* 保守策略：读取出错就标记坏块 */
        if (ret0 != 0)
        {
            ftl_block_set_bad(&alloc->blocks[gblock]);
            ftl_block_state_clr(&alloc->blocks[gblock],
                                FTL_BLOCK_STATE_OPEN | FTL_BLOCK_STATE_CLOSED |
                                    FTL_BLOCK_STATE_FREE);
            ftl_block_set_sb(&alloc->blocks[gblock], FTL_BLOCK_NO_SB);
            bad_idx++;
        }
    }

    alloc->bad_count = bad_idx;
    g_bbt_dirty      = 1;

    /* 将扫描结果写入 NAND BBT */
    (void)bbt_save(alloc);

    return (int)bad_idx;
}

/* ══════════════════════════════════════════════════════════════════
 * 坏块图打印（诊断；默认**只读**，加 --erase 才逐块擦除）
 * ══════════════════════════════════════════════════════════════════ */

#define BBT_MAP_COLS_PER_PLANE (16U)                              /* 一排每 plane 打几个块 */
#define BBT_MAP_GROUP_W        (BBT_MAP_COLS_PER_PLANE * 3U - 1U) /* 一组宽度：n 个 2 字符 + n-1 空格 */
#define BBT_MAP_LINE_MAX       (PLANE_NUM * (BBT_MAP_GROUP_W + 3U) + 1U)

_Static_assert((BLOCKS_PER_PLANE % BBT_MAP_COLS_PER_PLANE) == 0U, "每 plane 块数须被每排列数整除，否则末排漏块");

/*
 * bbt_probe_block_bad - 探测单个块是否坏块
 *
 * ① 读 Page 0 的 spare 首字节（**列 2048**，`PAGE_MAIN_SIZE`），非 0xFF ⇒ 出厂坏块。
 *    **必须在擦除之前读**——器件手册（Micron AN1819）逐字：*"the bad block information is
 *    erasable and cannot be recovered once erased"*，且 *"System software should check the
 *    first spare area location on the first page of each block prior to performing any
 *    PROGRAM or ERASE operations"*。（旧实现的注释写「擦不掉的硬标记」是**错的**，
 *    这正是它先擦后读、结果永远全 FF 的根因。）
 * ② do_erase 非 0 时才擦除；擦除失败 ⇒ 坏块（stuck-at-0 / 不可擦，运行时坏块的子集）。
 *
 * 列 2048 只能靠 OP_READ_COLUMN（列地址走命令 param）够到：OP_READ_META 的列 =
 * paa_to_cw × CW_SIZE，cw ∈ {0,1,2,3} ⇒ 列只能是 {0, 528, 1056, 1584}。
 * 4 字节 = spare[0..3]（2 字节坏块信息 + 2 字节 user metadata II），标记看 [0]。
 */
static int bbt_probe_block_bad(ftl_allocator_t *alloc, uint8_t plane, uint16_t block, uint8_t do_erase)
{
    nand_addr_t addr;
    uint8_t     mark[4];

    /* page / cw 由 paa_from_addr 给 0：擦除不看它们；列地址由 ftl_sync_read_column 显式给，
     * 不依赖 paa 的 cw 字段。 */
    addr = addr_from_paa(alloc->ch, alloc->ce, paa_from_addr(&(nand_addr_t){.plane = plane, .block = block}));

    /* ① 出厂标记（擦除前读） */
    (void)memset(mark, 0xFF, sizeof(mark));
    if ((ftl_sync_read_column(addr, (uint16_t)PAGE_MAIN_SIZE, mark) == FTL_OK) && (mark[0] != 0xFFU))
    {
        return 1;
    }

    /* ② 可选擦除 */
    if ((do_erase != 0U) && (ftl_sync_erase_block(addr) != FTL_OK))
    {
        return 1;
    }

    return 0;
}

void bbt_print_bad_map(ftl_allocator_t *alloc, uint8_t do_erase)
{
    char     line[BBT_MAP_LINE_MAX];
    uint16_t rows;
    uint16_t row;
    uint16_t col;
    uint8_t  plane;

    if (alloc == NULL)
    {
        return;
    }

    rows = (uint16_t)(BLOCKS_PER_PLANE / BBT_MAP_COLS_PER_PLANE);

    LOGI("CH=%u  CE=%u  LUN=%u\r\n", (unsigned)alloc->ch, (unsigned)alloc->ce, (unsigned)alloc->lun);

    /* ── PLANE 标签行：每 plane 一组，标签在组内居中 ── */
    {
        int n = 0;

        for (plane = 0U; plane < PLANE_NUM; plane++)
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

    /* ── 数据行：一排同时打各 plane 的同段块号（块号 = row × 列数 + col）── */
    for (row = 0U; row < rows; row++)
    {
        int n = 0;

        for (plane = 0U; plane < PLANE_NUM; plane++)
        {
            if (plane > 0U)
            {
                n += snprintf(&line[n], sizeof(line) - (size_t)n, "   ");
            }

            for (col = 0U; col < BBT_MAP_COLS_PER_PLANE; col++)
            {
                const uint16_t block = (uint16_t)((row * BBT_MAP_COLS_PER_PLANE) + col);

                if (col > 0U)
                {
                    n += snprintf(&line[n], sizeof(line) - (size_t)n, " ");
                }

                if (block >= BLOCKS_PER_PLANE)
                {
                    n += snprintf(&line[n], sizeof(line) - (size_t)n, "  "); /* 末排不满：留空 */
                }
                else if (bbt_probe_block_bad(alloc, plane, block, do_erase) != 0)
                {
                    n += snprintf(&line[n], sizeof(line) - (size_t)n, "%u", (unsigned)col); /* 坏块：本排列号 */
                }
                else
                {
                    n += snprintf(&line[n], sizeof(line) - (size_t)n, "FF"); /* 好块 */
                }
            }
        }

        LOGI("%s\r\n", line);
    }
}

ftl_status_t bbt_mark_bad(ftl_allocator_t *alloc, uint16_t gblock)
{
    if ((alloc == NULL) || (gblock >= BBT_MAX_BLOCKS) || (alloc->blocks == NULL))
    {
        return FTL_ERROR;
    }

    /* BST is_bad 置位（单数据源，后台 bbt_flush 持久化到 NAND）。
     * 同时清空有效页位图——坏块后该块不会再产生有效页，
     * 残留的 VPC/bitmap 会让 GC/WL 误判其为可回收块。 */
    ftl_block_set_bad(&alloc->blocks[gblock]);
    ftl_block_clear_valid(&alloc->blocks[gblock], alloc->valid_map[gblock]);
    ftl_block_state_clr(&alloc->blocks[gblock],
                        FTL_BLOCK_STATE_OPEN | FTL_BLOCK_STATE_CLOSED |
                            FTL_BLOCK_STATE_FREE | FTL_BLOCK_TEMP_MASK);
    ftl_block_set_sb(&alloc->blocks[gblock], FTL_BLOCK_NO_SB);
    alloc->bad_count++;
    g_bbt_dirty = 1;

    /* 不再同步调 bbt_save() — NAND BBT 持久化由后台 bbt_flush 批量执行。
     * 坏块替换：从 Spare Pool 取块补 free pool（保容量水位，ADR-11 Block Sparing）。
     * 失败（Spare 耗尽）不影响标坏，容量自然下降。 */
    (void)bbt_spare_pool_refill(alloc);

    return FTL_OK;
}

ftl_status_t bbt_save(const ftl_allocator_t *alloc)
{
    bbt_header_t    header;
    bbt_probe_ctx_t ctx;
    nand_addr_t     addr;
    uint16_t        gblock;

    if (alloc == NULL)
    {
        return FTL_ERROR;
    }
    if (!g_bbt_dirty)
    {
        return FTL_OK;
    }

    bbt_ring_init(&ctx.ring);
    ctx.topo = (ftl_topology_t){.ch = alloc->ch, .ce = alloc->ce, .lun = alloc->lun};

    /* 游标未建立 ⇒ 先定位池内最新有效 record 的下一槽 */
    if (g_bbt_cursor_valid == 0U)
    {
        uint16_t newest = UINT16_MAX;

        if (metadata_ring_locate(&ctx.ring, &ctx.topo, bbt_record_probe, &ctx, &newest, &g_bbt_sequence, &g_bbt_write_idx) != FTL_OK)
        {
            return FTL_ERROR;
        }
        g_bbt_cursor_valid = 1U;
    }

    g_bbt_sequence++;

    memset(&header, 0, sizeof(header));
    header.magic        = BBT_MAGIC;
    header.sequence     = g_bbt_sequence;
    header.total_blocks = BBT_MAX_BLOCKS;
    header.bad_count    = alloc->bad_count;

    /* 从 BST 重建 NAND 持久化 bitmap（NAND 格式一字不改） */
    for (gblock = 0U; gblock < BBT_MAX_BLOCKS; gblock++)
    {
        if (ftl_block_is_bad(&alloc->blocks[gblock]))
        {
            bbt_set_bad(header.bad_map, gblock);
        }
    }

    /* CRC（crc 字段置 0 后计算） */
    header.crc = 0U;
    header.crc = boot_crc32(&header, sizeof(header) - sizeof(header.crc));

    /* 追加写：池内下一槽（块满则先擦池内下一块）。
     * 无论成败都推进游标（已写过的槽可能半编程，不可复用）。 */
    if (metadata_ring_prepare_write(&ctx.ring, &ctx.topo, g_bbt_write_idx, &addr) != FTL_OK)
    {
        return FTL_ERROR;
    }

    if (ftl_sync_write_page(addr, &header, sizeof(header), FTL_PAGE_CFG_META(FC_COMMAND_MODULE_BBT)) != 0)
    {
        g_bbt_write_idx = (uint16_t)((g_bbt_write_idx + 1U) % metadata_ring_total_records(&ctx.ring));
        return FTL_ERROR; /* dirty 保留 ⇒ 下次重试 */
    }

    g_bbt_write_idx = (uint16_t)((g_bbt_write_idx + 1U) % metadata_ring_total_records(&ctx.ring));

    g_bbt_dirty = 0;
    return FTL_OK;
}

ftl_status_t bbt_flush(ftl_allocator_t *alloc)
{
    /* 仅当 dirty 时才写 NAND */
    if (!g_bbt_dirty)
    {
        return FTL_OK;
    }
    return bbt_save(alloc);
}

ftl_status_t bbt_load(uint8_t *bad_map, uint32_t *bad_count, uint32_t *sequence, ftl_topology_t topo)
{
    bbt_probe_ctx_t ctx;
    bbt_header_t    best;
    nand_addr_t     addr;
    uint16_t        newest = UINT16_MAX;

    if ((bad_map == NULL) || (bad_count == NULL) || (sequence == NULL))
    {
        return FTL_ERROR;
    }

    /* 复位本池状态：无有效 record 时保持 0/无效（后续 bbt_save 从池首开始）*/
    g_bbt_sequence     = 0U;
    g_bbt_write_idx    = 0U;
    g_bbt_cursor_valid = 0U;

    bbt_ring_init(&ctx.ring);
    ctx.topo = topo;

    if (metadata_ring_locate(&ctx.ring, &ctx.topo, bbt_record_probe, &ctx, &newest, &g_bbt_sequence, &g_bbt_write_idx) != FTL_OK)
    {
        return FTL_ERROR;
    }
    if (newest == UINT16_MAX)
    {
        g_bbt_sequence  = 0U;
        g_bbt_write_idx = 0U;
        return FTL_ERROR; /* 池内无有效 record */
    }

    g_bbt_cursor_valid = 1U;

    /* 重读胜者（locate 只返回序号，Header 本体要再读一次）*/
    addr = metadata_ring_addr(&ctx.ring, &ctx.topo, newest, 0U);
    if (ftl_sync_read_page(addr, &best, sizeof(best), FTL_PAGE_CFG_META(FC_COMMAND_MODULE_BBT)) != 0)
    {
        return FTL_ERROR;
    }
    if ((bbt_header_valid(&best) == 0) || (best.sequence != g_bbt_sequence))
    {
        return FTL_ERROR;
    }

    /* 输出到 caller */
    memcpy(bad_map, best.bad_map, BBT_BITMAP_SIZE);
    *bad_count = best.bad_count;
    *sequence  = best.sequence;

    return FTL_OK;
}

ftl_status_t bbt_spare_pool_refill(ftl_allocator_t *alloc)
{
    /*
     * Spare Pool 坏块替换：出厂时 main.c 已预留 FTL_SPARE_POOL_SIZE=50 个
     * 好块到 alloc->spare_blocks。运行时 bbt_mark_bad 标坏一块后，从这里
     * 取一个好块 allocator_free_block 补进 free pool，保用户容量水位。
     * 只出不进，Spare Pool 耗尽后返回 FTL_ERROR（容量自然下降）。
     */
    if ((alloc == NULL) || (alloc->spare_blocks == NULL) || (alloc->spare_count == 0U))
    {
        return FTL_ERROR;
    }

    alloc->spare_count--;
    allocator_free_block(alloc, alloc->spare_blocks[alloc->spare_count]);

    return FTL_OK;
}
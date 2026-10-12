#include "ftl/metadata.h"

#include "common/boot_policy.h"
#include "fc/fc.h"
#include "ftl/bbt.h"
#include "ftl/checkpoint.h"
#include "ftl/ftl_fc_interface.h"
#include "ftl/journal.h"
#include "ftl/l2p_cache.h"
#include "nand/onfi_1_0.h"

#include <string.h>

/* 一页 Anchor 的 CRC 覆盖长度 = 整页减去尾部 crc 字段（padding 不参与校验）*/
#define ANCHOR_PAGE_CRC_LEN (sizeof(metadata_anchor_page_t) - sizeof(uint32_t))

/*
 * anchor_page_index - Anchor 页索引 → (block, page)
 *
 *   idx = copy_index × ANCHOR_PAGE_COUNT + chunk_index
 *     copy_index  ∈ [0, ANCHOR_COPIES × ANCHOR_REDUNDANCY)：前 ANCHOR_REDUNDANCY 份在
 *                   ANCHOR_BLOCK0，其余在 ANCHOR_BLOCK1
 *     chunk_index ∈ [0, ANCHOR_PAGE_COUNT)：一份 Directory 的第几页
 *   page = (copy_index % ANCHOR_REDUNDANCY) × ANCHOR_PAGE_COUNT + chunk_index
 *        ⇒ 每块占用 ANCHOR_PAGE_PER_BLOCK 页（≤ PAGES_PER_BLOCK，见 ftl_config.h §七）
 *   plane 恒 0：Anchor 全部页落在块内 plane 0，plane 1 留空。
 */
static void anchor_page_index(uint8_t idx, uint16_t *block, uint8_t *page)
{
    uint8_t copy  = (uint8_t)(idx / ANCHOR_PAGE_COUNT);
    uint8_t chunk = (uint8_t)(idx % ANCHOR_PAGE_COUNT);

    *block = (uint16_t)((copy < ANCHOR_REDUNDANCY) ? ANCHOR_BLOCK0 : ANCHOR_BLOCK1);
    *page  = (uint8_t)((copy % ANCHOR_REDUNDANCY) * ANCHOR_PAGE_COUNT + chunk);
}

/* ── Anchor 固定位置页地址 ── */
static nand_addr_t anchor_page_addr(uint8_t ch, uint8_t ce, uint8_t lun, uint8_t idx)
{
    nand_addr_t addr;
    uint16_t    block;
    uint8_t     page;

    anchor_page_index(idx, &block, &page);

    addr.ch    = ch;
    addr.ce    = ce;
    addr.lun   = lun;
    addr.plane = 0U;
    addr.page  = page;
    addr.block = block;
    addr.cw    = 0U;

    return addr;
}

/* 校验一页 Anchor：magic + chunk_count + 本页 CRC */
static int anchor_page_valid(const metadata_anchor_page_t *page)
{
    if (page->magic != ANCHOR_MAGIC)
    {
        return 0;
    }
    if (page->chunk_count != ANCHOR_PAGE_COUNT)
    {
        return 0;
    }

    return (boot_crc32(page, ANCHOR_PAGE_CRC_LEN) == page->crc) ? 1 : 0;
}

/*
 * metadata_dir_fill_fixed - 建立全部 region 的固定位置（打包放置，见 ftl_config.h §六）
 *
 * 每个实例的 L2P_REGIONS_PER_INSTANCE 个 region **全部登记**；某个 region 刷新到
 * 动态块后由 metadata_dir_update 就地覆盖。共用于：
 *   metadata_dir_init —— 首次格式化
 *   metadata_dir_load —— 作为 Anchor 的"基础层"（Anchor 缺页/损坏时回落到固定位置）
 */
static void metadata_dir_fill_fixed(metadata_dir_cache_t *dir)
{
    uint16_t i;
    uint8_t  plane;
    uint16_t block;
    uint8_t  page;

    dir->count        = 0U;
    dir->have_dynamic = 0U;

    for (i = 0U; i < L2P_REGIONS_PER_INSTANCE; i++)
    {
        ftl_l2p_region_placement(i, &block, &page, &plane);

        dir->entries[dir->count].type = METADATA_TYPE_L2P_REGION;
        dir->entries[dir->count].id   = i;
        dir->entries[dir->count].paa  = paa_from_addr(&(nand_addr_t){.lun = dir->topo.lun, .block = block, .plane = plane, .page = page});
        dir->count++;
    }
}

void metadata_dir_init(metadata_dir_cache_t *dir)
{
    if (dir == NULL)
    {
        return;
    }

    dir->anchor_sequence = 0U;
    dir->dirty           = 0U;
    dir->change_count    = 0U;

    metadata_dir_fill_fixed(dir);

    /* 当前 metadata 分区块指针（初始指向固定元数据区之后第一个可用块）*/
    dir->cur_metadata_block = FTL_METADATA_BLOCK_END;
    dir->cur_metadata_page  = 0U;
    dir->cur_metadata_plane = 0U;
}

nand_paa_t metadata_dir_lookup(const metadata_dir_cache_t *dir,
                               uint8_t                     type,
                               uint16_t                    id)
{
    uint16_t i;

    if (dir == NULL)
    {
        return NAND_PBA_INVALID;
    }

    for (i = 0U; i < dir->count; i++)
    {
        if ((dir->entries[i].type == type) && (dir->entries[i].id == id))
        {
            return dir->entries[i].paa;
        }
    }

    return NAND_PBA_INVALID;
}

void metadata_dir_update(metadata_dir_cache_t *dir,
                         uint8_t               type,
                         uint16_t              id,
                         nand_paa_t            new_paa)
{
    uint16_t i;

    if (dir == NULL)
    {
        return;
    }

    /* 查找现有 entry（覆盖旧位置） */
    for (i = 0U; i < dir->count; i++)
    {
        if ((dir->entries[i].type == type) && (dir->entries[i].id == id))
        {
            dir->entries[i].paa = new_paa;
            dir->dirty          = 1U;
            dir->change_count++;
            return;
        }
    }

    /* 新增 entry（往 entries 数组尾部追加）*/
    if (dir->count < METADATA_DIR_ENTRIES)
    {
        dir->entries[dir->count].type = type;
        dir->entries[dir->count].id   = id;
        dir->entries[dir->count].paa  = new_paa;
        dir->count++;
        dir->dirty = 1U;
        dir->change_count++;
    }
    /*
     * 满：不追加。
     *
     * ⚠ 本分支对 METADATA_TYPE_L2P_REGION **不可达**：METADATA_DIR_ENTRIES 已等于
     *   L2P_REGIONS_PER_INSTANCE，且每个 region 在 metadata_dir_fill_fixed 就登记完毕，
     *   刷新后走的是上面的"覆盖"分支。旧版容量 128 < 2048 时本分支可达，结果是
     *   region ≥128 的刷新位置被**静默丢弃**（重启回落到固定位置 = 更新永久丢失）。
     *
     * ⚠ 只有"登记第 METADATA_DIR_ENTRIES+1 种元数据"才会走到这里。届时**不要**在此
     *   静默返回：调用者以为位置已记录，实际没有。必须先扩容量或实现淘汰。
     */
}

nand_paa_t metadata_dir_alloc_region(metadata_dir_cache_t *dir,
                                     ftl_allocator_t      *alloc,
                                     uint16_t              region_id)
{
    uint8_t    plane;
    uint16_t   block;
    uint8_t    page;
    nand_paa_t paa;

    if ((dir == NULL) || (alloc == NULL))
    {
        return NAND_PBA_INVALID;
    }

    plane = (uint8_t)(region_id & 1U); /* 偶 P0, 奇 P1 */

    /*
     * 在当前 metadata block 中顺序写入。
     * 如果当前 block 已满（page >= 64），从 Free Pool 取新 block。
     */
    if (dir->cur_metadata_page >= PAGES_PER_BLOCK)
    {
        /* 从 Free Pool 取一个好块。
         * 使用 allocator_alloc(FTL_TEMP_COLD) 分配一页，然后从中提取 block 号。
         * 这只占用了页分配中的 Plane 0 位置——后续写 region 时覆盖整页。 */
        nand_pba_t tmp_pba = NAND_PBA_INVALID;

        tmp_pba = allocator_alloc(alloc, FTL_TEMP_COLD);
        if (tmp_pba == NAND_PBA_INVALID)
        {
            return NAND_PBA_INVALID; /* Free Pool 空 */
        }
        block                   = (uint16_t)pba_to_block(tmp_pba);
        dir->cur_metadata_block = block;
        dir->cur_metadata_page  = 0U;
        dir->cur_metadata_plane = plane;
    }

    page  = dir->cur_metadata_page;
    block = dir->cur_metadata_block;

    paa = paa_from_addr(&(nand_addr_t){.lun = dir->topo.lun, .block = block, .plane = plane, .page = page});

    /* 更新 Directory（本函数只写动态块 ⇒ 位置必在 FTL_METADATA_BLOCK_END 之后）*/
    metadata_dir_update(dir, METADATA_TYPE_L2P_REGION, region_id, paa);
    dir->have_dynamic = 1U;

    /* 推进 page 指针 */
    dir->cur_metadata_page++;

    return paa;
}

ftl_status_t metadata_dir_flush_to_anchor(metadata_dir_cache_t *dir)
{
    metadata_anchor_page_t page; /* 2KB 栈缓冲：**按页**组好后写入，勿放整份 Directory */
    uint8_t                copy;
    uint8_t                chunk;
    uint32_t               seq;
    uint8_t                copy_count = (uint8_t)(ANCHOR_COPIES * ANCHOR_REDUNDANCY);

    if ((dir == NULL) || (!dir->dirty))
    {
        return FTL_OK;
    }

    seq = ++dir->anchor_sequence;

    /* 每份 = ANCHOR_PAGE_COUNT 页（chunk）；共 ANCHOR_COPIES 块 × ANCHOR_REDUNDANCY 份 */
    for (copy = 0U; copy < copy_count; copy++)
    {
        for (chunk = 0U; chunk < ANCHOR_PAGE_COUNT; chunk++)
        {
            uint16_t    base = (uint16_t)(chunk * METADATA_DIR_ENTRIES_PER_PAGE);
            uint16_t    n;
            uint16_t    k;
            nand_addr_t aaddr;
            int         ret;

            /* 本页有效条目数 = 全局有效范围 [0,count) 与本页区间 [base,base+PER_PAGE) 的交 */
            if (base >= dir->count)
            {
                n = 0U;
            }
            else
            {
                n = (uint16_t)(dir->count - base);
                if (n > METADATA_DIR_ENTRIES_PER_PAGE)
                {
                    n = METADATA_DIR_ENTRIES_PER_PAGE;
                }
            }

            memset(&page, 0, sizeof(page)); /* padding 归零 ⇒ 介质内容确定 */
            page.magic       = ANCHOR_MAGIC;
            page.sequence    = seq;
            page.chunk_index = chunk;
            page.chunk_count = ANCHOR_PAGE_COUNT;
            page.entry_count = n;

            for (k = 0U; k < METADATA_DIR_ENTRIES_PER_PAGE; k++)
            {
                if (k < n)
                {
                    page.entries[k] = dir->entries[base + k];
                }
                else
                {
                    page.entries[k].type = 0U;
                    page.entries[k].id   = 0U;
                    page.entries[k].paa  = NAND_PBA_INVALID;
                }
            }

            page.crc = boot_crc32(&page, ANCHOR_PAGE_CRC_LEN);

            aaddr = anchor_page_addr(dir->topo.ch, dir->topo.ce, dir->topo.lun, (uint8_t)(copy * ANCHOR_PAGE_COUNT + chunk));
            ret   = ftl_sync_write_page(aaddr, &page, sizeof(page), FTL_PAGE_CFG_META(FC_COMMAND_MODULE_METADATA));
            (void)ret; /* 单页写失败有其余份兜底 */
        }
    }

    dir->dirty        = 0U;
    dir->change_count = 0U;

    return FTL_OK;
}

ftl_status_t metadata_dir_load(metadata_dir_cache_t *dir)
{
    metadata_anchor_page_t page; /* 2KB 栈缓冲（按页读，不再整份进栈）*/
    uint8_t                copy_count = (uint8_t)(ANCHOR_COPIES * ANCHOR_REDUNDANCY);
    uint8_t                copy;
    uint8_t                best_copy = 0U;
    uint32_t               best_seq  = 0U;
    uint8_t                found     = 0U;
    uint16_t               i;

    if (dir == NULL)
    {
        return FTL_ERROR;
    }

    /* 基础层：全部 region 的固定位置。Anchor 缺页/损坏时该区间回落到固定位置
     * （= 尚未搬到动态块的状态），不会出现"未登记"的空洞。*/
    metadata_dir_fill_fixed(dir);

    /* ① 读每份的 chunk 0 页，选 sequence 最大的有效份 */
    for (copy = 0U; copy < copy_count; copy++)
    {
        nand_addr_t pba = addr_from_paa(dir->topo.ch, dir->topo.ce, paa_with_lun(metadata_dir_anchor_paa((uint8_t)(copy * ANCHOR_PAGE_COUNT)), dir->topo.lun));

        if (ftl_sync_read_page(pba, &page, sizeof(page), FTL_PAGE_CFG_META(FC_COMMAND_MODULE_METADATA)) != 0)
        {
            continue; /* 读失败，跳过 */
        }
        if (anchor_page_valid(&page) == 0)
        {
            continue; /* magic/chunk_count/CRC 任一不符，跳过 */
        }

        if ((found == 0U) || (page.sequence > best_seq))
        {
            best_seq  = page.sequence;
            best_copy = copy;
            found     = 1U;
        }
    }

    if (found == 0U)
    {
        return FTL_ERROR; /* 无有效 Anchor：调用者转 metadata_dir_init */
    }

    /* ② 读胜者的全部页，逐页校验后覆盖对应区间 */
    for (copy = 0U; copy < ANCHOR_PAGE_COUNT; copy++)
    {
        uint16_t    base = (uint16_t)(copy * METADATA_DIR_ENTRIES_PER_PAGE);
        nand_addr_t pba  = addr_from_paa(dir->topo.ch, dir->topo.ce, paa_with_lun(metadata_dir_anchor_paa((uint8_t)(best_copy * ANCHOR_PAGE_COUNT + copy)), dir->topo.lun));
        uint16_t    n;
        uint16_t    k;

        if (ftl_sync_read_page(pba, &page, sizeof(page), FTL_PAGE_CFG_META(FC_COMMAND_MODULE_METADATA)) != 0)
        {
            continue;
        }
        if ((anchor_page_valid(&page) == 0) || (page.sequence != best_seq) || (page.chunk_index != copy))
        {
            continue; /* 该页不可信：保留基础层的固定位置 */
        }

        n = (uint16_t)((page.entry_count < METADATA_DIR_ENTRIES_PER_PAGE) ? page.entry_count : METADATA_DIR_ENTRIES_PER_PAGE);
        for (k = 0U; (k < n) && ((uint16_t)(base + k) < METADATA_DIR_ENTRIES); k++)
        {
            dir->entries[base + k] = page.entries[k];
        }
    }

    dir->anchor_sequence = best_seq;
    dir->dirty           = 0U;
    dir->change_count    = 0U;

    /* 找到当前 metadata 分区块指针：取最大的"动态（flushed）region"位置。
     * 动态位置只能来自 Free Pool，而 Free Pool 只含 block >= FTL_METADATA_BLOCK_END
     * （固定元数据块被 ftl_is_metadata_block 排除）；无动态 region 时游标就停在该边界
     * （不可为 0——会撞 L2P 固定表区）。 */
    {
        uint16_t max_block = FTL_METADATA_BLOCK_END;
        uint8_t  max_page  = 0U;
        uint8_t  have_dyn  = 0U;

        for (i = 0U; i < dir->count; i++)
        {
            uint16_t b;
            uint8_t  p;

            if (dir->entries[i].type != METADATA_TYPE_L2P_REGION)
            {
                continue;
            }

            b = (uint16_t)paa_to_block(dir->entries[i].paa);

            /* 固定元数据区内的 region 不是动态位置 */
            if (b < FTL_METADATA_BLOCK_END)
            {
                continue;
            }

            p = (uint8_t)paa_to_page(dir->entries[i].paa);

            if ((have_dyn == 0U) || (b > max_block) || ((b == max_block) && (p > max_page)))
            {
                max_block = b;
                max_page  = p;
                have_dyn  = 1U;
            }
        }

        dir->cur_metadata_block = max_block;
        dir->cur_metadata_page  = (uint8_t)((have_dyn != 0U) ? (max_page + 1U) : 0U);
        dir->cur_metadata_plane = 0U;
        dir->have_dynamic       = have_dyn;
    }

    return FTL_OK;
}

nand_paa_t metadata_dir_anchor_paa(uint8_t idx)
{
    uint16_t block;
    uint8_t  page;

    /* 索引语义与 anchor_page_index 一致（idx = copy × ANCHOR_PAGE_COUNT + chunk）*/
    anchor_page_index(idx, &block, &page);

    /* lun 由调用侧（anchor_page_addr / 各模块 topo）带出——此处仅固定块/页/plane，
     * 调用点用 paa_with_lun(...) 补 lun（当前 LUN_ADDR_WIDTH=0 ⇒ 空操作）。 */
    return paa_from_addr(&(nand_addr_t){.block = block, .plane = 0U, .page = page});
}

int ftl_is_metadata_block(const metadata_dir_cache_t *md_dir, uint16_t block)
{
    uint32_t p;
    uint16_t i;

    /* ── 固定元数据块：遍历分区表（ftl/ftl_config.h §五）──
     * 每项覆盖 [first_block, first_block + block_count)。
     * **各 (ch,ce,lun) 用同一份块号**（分片模型：每 die 一套完整元数据），
     * 分区不随维度错开。block 为 per-plane 号（调用侧取模）。 */
    for (p = 0U; p < g_ftl_partition_count; p++)
    {
        const ftl_partition_t *part = &g_ftl_partitions[p];

        if (part->kind != FTL_PART_FIXED)
        {
            continue;
        }

        if ((block >= part->first_block) && (block < (uint16_t)(part->first_block + part->block_count)))
        {
            return 1;
        }
    }

    /* ── 动态元数据块：Metadata Directory 登记的 L2P region 物理位置 ──
     * L2P region 首写落在固定表区（上面已短路），之后从 Free Pool 动态取块
     * （flushed region，块号 >= FTL_METADATA_BLOCK_END）。这些块写满 64 页，
     * 逐页判空会误判为满 SB，必须排除。
     * ⚠ entries 有 2048 条而本函数在开机回填里按块调用数千次 ⇒ 用 have_dynamic
     *   先挡掉"根本没有动态块"的常见情形（全表线性扫会明显拖慢开机）。 */
    if ((md_dir != NULL) && (md_dir->have_dynamic != 0U))
    {
        for (i = 0U; i < md_dir->count; i++)
        {
            if ((md_dir->entries[i].type == METADATA_TYPE_L2P_REGION) &&
                (paa_to_block(md_dir->entries[i].paa) == block))
            {
                return 1;
            }
        }
    }

    return 0;
}
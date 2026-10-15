/*
 * gc.c — 垃圾回收（触发判定 + 选 victim + 共用读原语）
 *
 * 职责：
 *   按四项加权评分选 victim SB，并给出 GC 触发判定；搬移与擦除归还已迁入 migrate.c。
 *
 * 与 wear_leveling.c 的分工：
 *   GC —— 回收**无效页多**的块（空间回收，顺带磨损均衡）；
 *   WL —— 搬运**擦得少**的块（磨损拉平，顺带 read disturb 缓解）。两者共用
 *         gc_read_page_with_lba 读原语与 migrate.c 的搬移引擎，且 GC 是 WL 的擦除出口。
 *
 * 三条触发（gc_check_triggers，位掩码；T2/T3/T6 已废弃见 gc.h）：
 *   T1 空闲水位 < FREE_POOL_TARGET；T4 紧急 ≤ FREE_POOL_EMERGENCY；T5 磨损比超标。
 *
 * 文件结构：
 *   1. 共用原语        gc_read_page_with_lba / gc_decode_cw_lba
 *   2. 全表极值        gc_table_extremes（static）
 *   3. 选 victim       gc_select_victim（四项加权评分）
 *   4. 触发判定        gc_check_triggers
 *
 * ⚠ 搬移（原第 4 节 gc_migrate_sb）与擦除归还（原 gc_erase_and_free）已随阶段 4 迁入
 *   `ftl/migrate.c` 的异步 job（GC / WL 共用一台引擎，靠 kind/post 区分）——本文件
 *   只留选块策略与共用读原语。
 *
 * ⚠ 全部按**全局块号**索引 BST（跨 plane），退化单 plane 条目跳过哨兵 plane。
 */

#include "ftl/gc.h"

#include "common/log.h"
#include "fc/fc.h"
#include "ftl/ftl_fc_interface.h"
#include "nand/onfi_1_0.h"

#include <string.h>

/* ── 1. 共用原语 ── */

/*
 * gc_read_page_with_lba - 读取一页 NAND，返回用户数据 + **逐 CW 从 META 取回的逻辑地址**
 *
 * 内部管理 FC 命令生命周期：
 *   alloc → config → submit → sync_wait → read_data → get_meta → release
 *
 * 唯一调用者是掉电重建（ftl_rebuild_closed_sb_list 的逐页反查）。
 * ⚠ 搬移读（GC / WL / 慢死预测）已随阶段 4 改走 **异步** 路径：migrate.c 用
 *   ftl_async_read_page + gc_decode_cw_lba（本函数的后半段）在完成服务函数里收尾，
 *   不再经过本函数。原先为搬移读加的 ecc_out / read_count_out 两个出参随之删除——
 *   ECC 趋势与 read_count++ 现在记在 migrate.c 的读完成分支里（B4 闭环不变）。
 *
 * ⚠ CW 级映射（IU = 1 CW）+ DRAM-less approach ③：本函数读**整页**（该 plane 的 4 个 CW），
 *   把**每个 CW 自己的 META**（即本仓的「OOB」反向映射，头 4B = 该 CW 的扇区 LBA）解码到
 *   `cw_lba[c]`。未写过的 CW（META 全 FF）填 `FTL_LBA_INVALID`。调用者据此**逐 CW 反查 L2P**
 *   判有效性——不靠任何常驻位图（flash reverse-mapping，见 bst.h 文件头）。
 *   `page_base_out` 仍返回页基址（CW0 的扇区 LBA，由**首个写过的 CW** 反推：base = META(c_lo) − c_lo），
 *   供搬移目标页的 META 基址复用。
 *
 * @param pba            物理地址（**页基址**，cw 位为 0）
 * @param buffer         输出：整页用户数据 buffer（FTL_READ_USER_SIZE 字节，可为 NULL）
 * @param cw_lba         输出：逐 CW 的扇区 LBA（长度 CW_NUM；未写过 = FTL_LBA_INVALID）
 * @param page_base_out  输出：页基址（CW0 的扇区 LBA）（可为 NULL）
 * @return 0 成功, -1 失败（读失败 / 全页无写过 CW）
 */
ftl_status_t gc_read_page_with_lba(nand_pba_t pba, uint8_t *buffer, lba_t cw_lba[CW_NUM], lba_t *page_base_out)
{
    fc_command_t *cmd;
    uint32_t      meta_cw0;
    int           ret;

    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    cmd->config.operation   = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
    cmd->config.cw          = 3U;
    cmd->config.crc         = FC_CRC_ENABLE;
    cmd->config.random      = FC_RANDOM_ENABLE;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.multi_plane = 0U;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    cmd->meta               = FC_META_UNKNOWN; /* 不知道 LBA——正是要从页内 META 读回来（v24） */

    if (ftl_cmd_setup_ppa(cmd, pba) != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    {
        fc_command_status_t completion;

        ret = ftl_sync_submit_wait(cmd, &completion, 0U);
        if (ret != 0)
        {
            (void)fc_command_release(cmd);
            return FTL_ERROR;
        }

        if (completion.status & FC_CW_STATUS_NAND_FAIL)
        {
            (void)fc_command_release(cmd);
            return FTL_ERROR;
        }
    }

    /* 读取用户数据 */
    if (buffer != NULL)
    {
        (void)ftl_cmd_read_data(cmd, buffer, PAGE_MAIN_SIZE); /* 整页用户区 = CW_NUM × 512B */
    }

    /* 从 META 解码逐 CW 的扇区 LBA + 页基址（与异步搬移 job 共用同一份实现） */
    if (gc_decode_cw_lba(cmd, pba, cw_lba, &meta_cw0) != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR; /* 全页无写过 CW：无有效数据 */
    }

    (void)fc_command_release(cmd);

    if (page_base_out != NULL)
    {
        *page_base_out = (lba_t)meta_cw0;
    }

    return FTL_OK;
}

/* --------------------------------------------------------------------
 * gc_decode_cw_lba
 *   功能：从已完成的读命令的 desc 链解出逐 CW 的扇区 LBA，并反推页基址。
 *   参数：cmd —— 已完成的读命令（链恒覆盖该 plane 全 4 CW）；pba —— 该命令的目标页；
 *         cw_lba —— 输出：逐 CW 的扇区 LBA（CW_NUM 项）；page_base_out —— 输出页基址（可 NULL）。
 *   返回：0 成功（至少一个 CW 写过）；-1 全页无写过 CW。
 *   实现要点：每个 CW 的 META = 该 CW 自己的**扇区** LBA（低位 [plane][cw] 就是它的物理位置）；
 *             未写过的 CW 读回 0xFF…（META = 0xFFFFFFFF）。**不能直接取 CW0 的 META**——
 *             CW 级写/搬移允许页内只编一个 CW 子集（`wr 2 2` 只落 plane0 的 CW2/CW3；
 *             GC 搬移也只编有效 CW），此时 CW0 可能是空位，直接取会得到 0xFFFFFFFF。
 *
 *             故取**首个写过的 CW** c_lo，用 base = META(c_lo) − c_lo 反推页基址。不变量：
 *             一个物理页只被**一次**编程操作写（host 子操作或 GC/WL 搬移各占一个全新页），
 *             该操作以同一页基址 B 编程 ⇒ 每个写过的 CW c 的 META 恒为 B + c（搬移也保持
 *             「LBA 相对页基址的偏移」），故 base = META(c_lo) − c_lo = B，base + c 对**所有**
 *             写过的 c 都还原出正确 LBA（页内允许有空洞 CW，如源页只搬 CW0/CW2）。
 *
 * ⚠ 必须在 fc_command_release 之前调用（meta 指针指向命令的 desc 链）。
 * ⚠ 同步版（gc_read_page_with_lba）与异步搬移 job（migrate.c）共用本函数——解码规则
 *   只此一份，避免两处漂移。
 * -------------------------------------------------------------------- */
int gc_decode_cw_lba(fc_command_t *cmd, nand_pba_t pba, lba_t cw_lba[CW_NUM], lba_t *page_base_out)
{
    uint8_t       c;
    uint8_t       found      = 0U;
    uint32_t      base_meta  = 0U;
    const uint8_t phys_plane = (uint8_t)pba_to_plane(pba);

    ASSERT(cmd != NULL);
    ASSERT(cw_lba != NULL);

    for (c = 0U; c < CW_NUM; c++)
    {
        cw_lba[c] = FTL_LBA_INVALID; /* 默认未写过 */
    }

    for (c = 0U; c < CW_NUM; c++)
    {
        uint8_t *meta_ptr = fc_buffer_get_meta(cmd, c);
        uint32_t m;

        if (meta_ptr == NULL)
        {
            continue; /* 该 CW 不在链上（本入口链恒覆盖全 4 CW，防御） */
        }
        /* meta_ptr 指向 CW buffer，CW 基址 4 字节对齐，memcpy 避免 alignment 警告 */
        memcpy(&m, meta_ptr, sizeof(m));

        /* 合法 META 判据：非全 1（未写过）+ 低位 plane/cw 与本 CW 自洽 */
        if ((m == 0xFFFFFFFFU) || (ftl_lba_cw((lba_t)m) != c) || (ftl_lba_plane((lba_t)m) != phys_plane))
        {
            continue;
        }
        cw_lba[c] = (lba_t)m; /* 本 CW 自己的扇区 LBA（META 即 LBA，低位就是 plane/cw） */
        if (found == 0U)
        {
            base_meta = m; /* 首个写过的 CW，用于反推页基址 */
            found     = 1U;
        }
    }

    if (found == 0U)
    {
        return -1; /* 全页无写过 CW：无有效数据 */
    }

    if (page_base_out != NULL)
    {
        *page_base_out = ftl_lba_page_base((lba_t)base_meta); /* 页基址 = CW0 的扇区 LBA */
    }

    return 0;
}

/* ── 2. 全表极值 ── */

/*
 * gc_table_extremes - 全表（非坏块）磨损/时间极值，一次扫描
 *
 * 两个消费方共用（避免各扫一遍全表）：
 *   ① gc_select_victim（P1-1）——评分归一化分母（max_erase / max_pe_ts）；
 *   ② gc_check_triggers（P1-2）的 T5——磨损比 max_erase/min_erase。
 *
 * ⚠ 不复用 wear_leveling 的 wl_max_erase/wl_min_erase：它们形参是 ftl_context_t，
 *   而本文件两个调用者只拿得到 ftl_allocator_t（gc_select_victim 调用点
 *   ftl.c:323/389/718 保持不变）。
 *
 * @param alloc      Allocator 上下文（blocks 为 NULL 时输出中性值）
 * @param min_erase  输出：最小擦除次数（无任何可统计块时为 0 ⇒ T5 视为"比值无意义"）
 * @param max_erase  输出：最大擦除次数（兜底 >= 1U，防除零）
 * @param max_pe_ts  输出：最大 last_pe_ts（0 = 本上电周期尚无块被擦 ⇒ 时间项中性）
 */
static void gc_table_extremes(const ftl_allocator_t *alloc, uint32_t *min_erase, uint32_t *max_erase, uint32_t *max_pe_ts)
{
    uint32_t min_e = UINT32_MAX;
    uint32_t max_e = 0U;
    uint32_t max_t = 0U;

    if ((alloc != NULL) && (alloc->blocks != NULL))
    {
        for (uint16_t b = 0U; b < FTL_BLOCK_TOTAL; b++)
        {
            const ftl_block_t *blk = &alloc->blocks[b];

            if (ftl_block_is_bad(blk) != 0)
            {
                continue;
            }
            if (blk->erase_count < min_e)
            {
                min_e = blk->erase_count;
            }
            if (blk->erase_count > max_e)
            {
                max_e = blk->erase_count;
            }
            if (blk->last_pe_ts > max_t)
            {
                max_t = blk->last_pe_ts;
            }
        }
    }

    *min_erase = (min_e == UINT32_MAX) ? 0U : min_e;
    *max_erase = (max_e > 0U) ? max_e : 1U;
    *max_pe_ts = max_t;
}

/* ── 3. 选 victim ── */

/* --------------------------------------------------------------------
 * gc_select_victim
 *   功能：从 Closed SB 列表里按四项加权评分选回收代价最小的 SB 作 victim。
 *   参数：alloc —— 分配器；victim —— 出参（指向 closed_sb_list 内的条目）。
 *   返回：1 找到；FTL_OK(0) 未找到 / 参数非法。
 *   实现要点：四项各自归一化到 0..1000——vr 有效页率、er 磨损率（越大越该回收旧块）、
 *             cp 温度（COLD 加分）、yp 年龄（越老越优先）；cost = 加权和，取小者胜；
 *             cost 相等取有效页更少者（确定性）。erase 项用 uint64_t 防溢出；
 *             age 项 `max_pe_ts > pe_min` 兜住 max_pe_ts==0 与 pe_min 越界两种退化（取中性 0）。
 * -------------------------------------------------------------------- */
int gc_select_victim(ftl_allocator_t *alloc, super_block_t **victim)
{
    uint32_t min_erase;
    uint32_t max_erase;
    uint32_t max_pe_ts;
    uint32_t best_cost  = UINT32_MAX;
    uint32_t best_vr    = 0U;
    uint32_t best_er    = 0U;
    uint32_t best_cp    = 0U;
    uint32_t best_yp    = 0U;
    uint16_t best_valid = (uint16_t)((PLANE_NUM * PAGES_PER_BLOCK * CW_NUM) + 1U);
    int      found      = 0;

    if ((alloc == NULL) || (alloc->blocks == NULL) || (victim == NULL))
    {
        return FTL_OK;
    }

    /* 归一化基准：全表极值（一次扫描，与候选数无关）。min_erase 本函数不用（T5 用）。 */
    gc_table_extremes(alloc, &min_erase, &max_erase, &max_pe_ts);

    for (uint16_t i = 0U; i < alloc->closed_sb_count; i++)
    {
        super_block_t *sb      = &alloc->closed_sb_list[i];
        uint16_t       valid   = 0U;
        uint8_t        present = 0U;
        uint32_t       er_max  = 0U;         /* 本 SB 的磨损代表：各 plane 取大 */
        uint32_t       pe_min  = UINT32_MAX; /* 本 SB 的时间代表：各 plane 取小（越老越优先）*/
        uint32_t       vr;
        uint32_t       er;
        uint32_t       cp;
        uint32_t       yp;
        uint32_t       cost;

        if (sb->valid != 0U)
        {
            continue;
        }

        /* 有效 CW 数 = 各 plane 物理块 valid_count 之和（CW 级真实计数，替代 next_page 近似）。
         * 退化单 plane 条目（重建产物）跳过哨兵 plane。 */
        for (uint8_t p = 0U; p < PLANE_NUM; p++)
        {
            const ftl_block_t *blk;

            if (super_block_has_plane(sb, p) == 0U)
            {
                continue;
            }
            blk   = &alloc->blocks[ftl_gblock(p, sb->plane_blocks[p])];
            valid = (uint16_t)(valid + blk->valid_count);
            present++;
            if (blk->erase_count > er_max)
            {
                er_max = blk->erase_count;
            }
            if (blk->last_pe_ts < pe_min)
            {
                pe_min = blk->last_pe_ts;
            }
        }
        if (present == 0U)
        {
            continue;
        }

        /* 四项各自归一化到 0..1000；cost 取小 = 越优先回收。
         * erase 项用 uint64_t 中间量防溢出（erase_count 是 uint32_t，×1000 理论上可溢出）。
         * age 项的 `max_pe_ts > pe_min` 同时兜住两种退化：① max_pe_ts == 0（上电后尚无块被擦）；
         * ② 本 SB 的 plane 块已被标坏 ⇒ gc_score_scale 跳过它 ⇒ pe_min 可能大于 max_pe_ts
         * （相减会下溢成大数、把 yp 绕回）。两者一律取 age_ratio = 0（时间项中性）。 */
        vr = ((uint32_t)valid * 1000U) / ((uint32_t)present * PAGES_PER_BLOCK * CW_NUM);
        er = (uint32_t)(((uint64_t)er_max * 1000U) / max_erase);
        cp = (sb->temperature == FTL_TEMP_HOT) ? 0U : 1000U;
        yp = 1000U - (((max_pe_ts > pe_min) ? (uint32_t)(((uint64_t)(max_pe_ts - pe_min) * 1000U) / max_pe_ts) : 0U));

        cost = (GC_WEIGHT_VALID * vr) + (GC_WEIGHT_ERASE * er) + (GC_WEIGHT_TEMP * cp) + (GC_WEIGHT_AGE * yp);

        /* cost 相等时取有效页更少者（确定性，且退化为旧的单维贪心） */
        if ((cost < best_cost) || ((cost == best_cost) && (valid < best_valid)))
        {
            best_cost  = cost;
            best_valid = valid;
            best_vr    = vr;
            best_er    = er;
            best_cp    = cp;
            best_yp    = yp;
            *victim    = sb;
            found      = 1;
        }
    }

    if (found != 0)
    {
        /* 观测口：一次空闲 GC 突发最多打 FREE_POOL_TARGET/PLANE_NUM = 16 行
         * （ftl_service_idle 循环调用），相对 GC 自身的 NAND 擦除/搬移耗时可忽略。 */
        LOGI("[GC] victim p0=%u p1=%u vr=%u er=%u cp=%u yp=%u cost=%lu\r\n",
             (unsigned)(*victim)->plane_blocks[0],
             (unsigned)(*victim)->plane_blocks[1],
             (unsigned)best_vr,
             (unsigned)best_er,
             (unsigned)best_cp,
             (unsigned)best_yp,
             (unsigned long)best_cost);
    }

    return found;
}

/* ── 4. 触发判定 ── */

/* --------------------------------------------------------------------
 * gc_check_triggers
 *   功能：算触发位掩码（调用点据此决定要不要回收、以及回收多少）。
 *   参数：alloc —— 分配器。
 *   返回：GC_TRIGGER_* 位掩码；0 无触发（含 alloc==NULL）。
 *   实现要点：只看**空间与磨损**——T1 水位 < FREE_POOL_TARGET（主动补池）；
 *             T4 紧急 ≤ FREE_POOL_EMERGENCY（与 T1 同源但阈值更低，池见底）；
 *             T5 磨损比 max/min 超标（min==0 不触发——"有块从未被擦"是静态 WL 的职责；
 *             max 地板防早期空跑）。⚠ T2（写入量）/T3（定时）已废弃，见 gc.h 位注释。
 * -------------------------------------------------------------------- */
uint32_t gc_check_triggers(const ftl_allocator_t *alloc)
{
    uint32_t triggers = 0U;

    if (alloc == NULL)
    {
        return 0U;
    }

    /* T1：空闲块水位（主动补池，还留有余量就开工） */
    if (alloc->free_count < FREE_POOL_TARGET)
    {
        triggers |= GC_TRIGGER_WATERMARK;
    }
    /* T4：紧急（池已见底的最后手段）。与 T1 同源但阈值更低（8 vs 32）。 */
    if (alloc->free_count <= FREE_POOL_EMERGENCY)
    {
        triggers |= GC_TRIGGER_EMERGENCY;
    }
    /* T5：磨损均衡（全表擦除比）。min_erase == 0 时比值无意义（对 0 除）⇒ 不触发：
     * "有块从未被擦过"是静态 WL（mig_begin_wl 搬冷数据）的职责，T5 只管
     * "所有块都在轮转、但轮得不均"。max_erase 地板防早期（max 才几次）空跑。 */
    {
        uint32_t min_erase;
        uint32_t max_erase;
        uint32_t max_pe_ts;

        gc_table_extremes(alloc, &min_erase, &max_erase, &max_pe_ts);
        if ((max_erase >= GC_WEAR_LEVEL_MIN_ERASE) && (min_erase > 0U) &&
            ((max_erase / min_erase) > GC_WEAR_LEVEL_RATIO))
        {
            triggers |= GC_TRIGGER_WEAR;
        }
    }

    return triggers;
}

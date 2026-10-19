/*
 * gc.c — 垃圾回收（触发判定 + 选 victim + 共用读原语）
 *
 * 职责：
 *   按四项加权评分选 victim SB，并给出 GC 触发判定。搬移与擦除归还在 migrate.c
 *   的异步 job 里完成（GC / WL 共用一台引擎，靠 kind/post 区分）。
 *
 * 与 wear_leveling.c 的分工：
 *   GC —— 回收**无效页多**的块（空间回收，顺带磨损均衡）；
 *   WL —— 搬运**擦得少**的块（磨损拉平，顺带 read disturb 缓解）。两者共用
 *         migrate.c 的搬移引擎；`gc_decode_cw_lba` 是搬移读与掉电重建共用的解码入口，
 *         `gc_read_page_with_lba` 只服务掉电重建的同步路径。
 *
 * 三条触发（gc_check_triggers，位掩码）：
 *   T1 空闲水位 < FREE_POOL_TARGET；T4 紧急 ≤ FREE_POOL_EMERGENCY；T5 磨损比超标。
 *
 * 文件结构：
 *   1. 共用原语        gc_read_page_with_lba / gc_decode_cw_lba
 *   2. 全表极值        gc_table_extremes（static）
 *   3. 选 victim       gc_select_victim（四项加权评分）
 *   4. 触发判定        gc_check_triggers
 *
 * ⚠ 全部按**全局块号**索引 BST（跨 plane），退化单 plane 条目跳过哨兵 plane。
 */

#include "gc.h"

#include "fc.h"
#include "ftl_fc_interface.h"
#include "log.h"
#include "onfi_1_0.h"

#include <string.h>

/* ── 1. 共用原语 ── */

/*
 * gc_read_page_with_lba - 读取一页 NAND，取回**逐 CW 从 META 解出的逻辑地址**
 *
 * 内部管理 FC 命令生命周期：
 *   alloc → config → submit → sync_wait → get_meta → release
 *
 * 唯一调用者是掉电重建（PFR 的 F2 全盘重建（内部）的逐页反查）。
 * 搬移读（GC / WL / 慢死预测）走 **异步** 路径：migrate.c 用 ftl_async_read_page +
 * gc_decode_cw_lba 在完成服务函数里收尾——不经过本函数。
 *
 * ⚠ 读整页只是为了取回页内 META；**不向调用者拷用户数据**（本函数没有数据出参）。
 * ⚠ CW 级映射（IU = 1 CW）+ DRAM-less approach ③：本函数读**整页**（该 plane 的 4 个 CW），
 *   把**每个 CW 自己的 META**（本仓的「OOB」反向映射，头 4B = 该 CW 的扇区 LBA）解码到
 *   `cw_lba[c]`。未写过的 CW（META 全 FF）填 `FTL_LBA_INVALID`。调用者据此**逐 CW 反查 L2P**
 *   判有效性——不靠任何常驻位图（flash reverse-mapping，见 bst.h 文件头）。
 *
 * ⚠ 成败判据 = FC_CW_STATUS_ERROR_MASK 全集（fc_cw_status_has_error）：UNC / MATA_ERROR
 *   页的内容不可信，不得当成功解析 META。
 *
 * @param pba     物理地址（**页基址**，cw 位为 0）
 * @param cw_lba  输出：逐 CW 的扇区 LBA（长度 CW_NUM；未写过 = FTL_LBA_INVALID）
 * @return FTL_OK 成功；FTL_AGAIN 命令池空；FTL_ERROR 读失败 / 全页无写过 CW
 */
ftl_status_t gc_read_page_with_lba(nand_pba_t pba, lba_t cw_lba[CW_NUM])
{
    /* 命令池空是**资源类**结果（FTL_AGAIN）；唯一调用者在开机重建期，池结构性充足。 */
    fc_command_t *cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return FTL_AGAIN;
    }

    cmd->config.operation   = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
    cmd->config.cw          = CW_NUM - 1U;
    cmd->config.crc         = FC_CRC_ENABLE;
    cmd->config.random      = FC_RANDOM_ENABLE;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.multi_plane = 0U;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    cmd->meta               = FC_META_UNKNOWN; /* 不知道 LBA——正是要从页内 META 读回来 */

    if (pba_to_cmd(cmd, pba) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    {
        fc_command_status_t completion;

        ftl_status_t ret = ftl_sync_submit_wait(cmd, &completion, 0U);
        if (ret != FTL_OK)
        {
            (void)fc_command_release(cmd);
            return FTL_ERROR;
        }

        /* ⚠ [待上板] 成败判据 = ERROR_MASK 全集：只认 NAND_FAIL 会把 UNC / MATA_ERROR
         * 页当成功读并解析 META（重建期可能写回错误映射）。 */
        if (fc_cw_status_has_error(completion.status))
        {
            (void)fc_command_release(cmd);
            return FTL_ERROR;
        }
    }

    /* 从 META 解码逐 CW 的扇区 LBA（与异步搬移 job 共用同一份实现）。
     * 空页 = 无有效数据；页基址不一致 = META 损坏；两者都按失败返回。 */
    if (gc_decode_cw_lba(cmd, pba, cw_lba, NULL) != GC_DECODE_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    (void)fc_command_release(cmd);

    return FTL_OK;
}

/* --------------------------------------------------------------------
 * gc_decode_cw_lba
 *   功能：从已完成的读命令的 desc 链解出逐 CW 的扇区 LBA，并反推页基址。
 *   参数：cmd —— 已完成的读命令（链恒覆盖该 plane 全 4 CW）；pba —— 该命令的目标页；
 *         cw_lba —— 输出：逐 CW 的扇区 LBA（CW_NUM 项）；page_base_out —— 输出页基址（可 NULL）。
 *   返回：GC_DECODE_OK 成功（至少一个 CW 写过且页基址一致）；
 *         GC_DECODE_EMPTY 全页无写过 CW（**不是读失败**：该页无有效数据）；
 *         GC_DECODE_CORRUPT 写过的 CW 推出的页基址不一致（META 损坏）。
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
gc_decode_result_t gc_decode_cw_lba(fc_command_t *cmd,
                                    nand_pba_t    pba,
                                    lba_t         cw_lba[CW_NUM],
                                    lba_t        *page_base_out)
{
    const uint8_t phys_plane = pba_to_plane(pba);

    ASSERT(cmd != NULL);
    ASSERT(cw_lba != NULL);

    for (uint8_t c = 0U; c < CW_NUM; c++)
    {
        cw_lba[c] = FTL_LBA_INVALID; /* 默认未写过 */
    }

    uint8_t  found     = 0U;
    uint32_t base_meta = 0U;
    for (uint8_t c = 0U; c < CW_NUM; c++)
    {
        uint8_t *meta_ptr = fc_buffer_get_meta(cmd, c);

        if (meta_ptr == NULL)
        {
            continue; /* 该 CW 不在链上（本入口链恒覆盖全 4 CW，防御） */
        }
        /* meta_ptr 指向 CW buffer，CW 基址 4 字节对齐，memcpy 避免 alignment 警告 */
        uint32_t m;
        memcpy(&m, meta_ptr, sizeof(m));

        /* 合法 META 判据：非全 1（未写过）+ 低位 plane/cw 与本 CW 自洽 */
        if ((m == (uint32_t)FTL_LBA_INVALID) || (ftl_lba_cw((lba_t)m) != c) ||
            (ftl_lba_plane((lba_t)m) != phys_plane))
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
        return GC_DECODE_EMPTY; /* 全页无写过 CW：无有效数据 */
    }

    /* 页基址由**首个写过的 CW** 反推（CW0 可能是空洞）。同一物理页只被一次编程
     * 操作写入 ⇒ 所有写过的 CW 必须共享同一页基址；不一致说明 META 损坏。
     *
     * ⚠ 不能“作废个别 CW 后继续”：它们的 L2P 仍指向本页，若随后擦 victim 就会
     *   悬空。整页按不可迁移处理，由调用者中止 job（victim 不擦）。 */
    const lba_t base = ftl_lba_page_base((lba_t)base_meta);

    for (uint8_t c = 0U; c < CW_NUM; c++)
    {
        if ((cw_lba[c] != FTL_LBA_INVALID) && (ftl_lba_page_base(cw_lba[c]) != base))
        {
            return GC_DECODE_CORRUPT;
        }
    }

    if (page_base_out != NULL)
    {
        *page_base_out = base;
    }

    return GC_DECODE_OK;
}

/* ── 2. 全表极值 ── */

/*
 * gc_table_extremes - 全表（非坏块）磨损/时间极值
 *
 * 两个消费方**各自调用一次**（不共享扫描）：
 *   ① gc_select_victim —— 评分归一化分母（max_erase / max_pe_ts）；
 *   ② gc_check_triggers 的 T5 —— 磨损比 max_erase/min_erase。
 * 求值点由 `mig_gc_evaluate` 的 10 ms 节拍门兜成本；不要为省这一次扫描把
 * 结构体穿过 bg 适配器 / mig_begin_gc 的公开 API（会把策略耦合进引擎）。
 *
 * 统计口径（好块 / 坏块排除）由 allocator 定义；本函数只把退化值折算成 GC 需要的语义：
 *   · 无任何可统计块 ⇒ min_erase = 0（T5 视为"比值无意义"）；
 *   · max_erase 兜底 >= 1U，防除零。
 */
typedef struct
{
    uint32_t min_erase;
    uint32_t max_erase; /* >= 1U */
    uint32_t max_pe_ts; /* 0 = 本上电周期尚无块被擦 ⇒ 时间项中性 */
} gc_extremes_t;

static void gc_table_extremes(const ftl_allocator_t *alloc, gc_extremes_t *out)
{
    allocator_erase_stats_t st;

    allocator_erase_stats(alloc, &st);

    out->min_erase = st.erase_min;
    out->max_erase = (st.erase_max > 0U) ? st.erase_max : 1U;
    out->max_pe_ts = st.max_pe_ts;
}

/* ── 3. 选 victim ── */

/* --------------------------------------------------------------------
 * gc_select_victim
 *   功能：从 Closed SB 列表里按四项加权评分选回收代价最小的 SB 作 victim。
 *   参数：alloc —— 分配器；victim —— 出参（指向 closed_sb_list 内的条目）。
 *   返回：true 找到；false 未找到。
 *   实现要点：四项各自归一化到 0..1000——vr 有效页率、er 磨损率（越大越**避开**临近寿命块）、
 *             cp 温度（COLD 加分）、yp 年龄（越老越优先）；cost = 加权和，取小者胜；
 *             cost 相等取有效页更少者（确定性）。erase 项用 uint64_t 防溢出；
 *             age 项 `max_pe_ts > pe_min` 兜住 max_pe_ts==0 与 pe_min 越界两种退化（取中性 0）。
 * -------------------------------------------------------------------- */
bool gc_select_victim(const ftl_allocator_t *alloc, const super_block_t **victim)
{
    /* 前置条件（判据 2）：调用者 migrate.c 传 &ftl->alloc 与栈上 &victim。 */
    ASSERT(victim != NULL);

    /* 归一化基准：全表极值（一次扫描，与候选数无关）。min_erase 本函数不用（T5 用）。 */
    gc_extremes_t ex;
    gc_table_extremes(alloc, &ex);

    uint32_t best_cost  = UINT32_MAX;
    uint32_t best_vr    = 0U;
    uint32_t best_er    = 0U;
    uint32_t best_cp    = 0U;
    uint32_t best_yp    = 0U;
    uint16_t best_valid = (uint16_t)(FTL_CW_PER_SB + 1U);
    bool     found      = false;

    for (uint16_t i = 0U; i < alloc->closed_sb_count; i++)
    {
        const super_block_t *sb = allocator_closed_at(alloc, i);

        if (sb->is_open)
        {
            continue; /* 仍在写的 SB 不是回收对象（UT 钉住的契约） */
        }

        /* 各 plane 的聚合值（有效 CW / 磨损 / 时间）由 allocator 统一计算：
         * 有效 CW = 各 plane valid_count 之和（CW 级真实计数）；退化单 plane 条目跳过哨兵 plane。 */
        allocator_sb_stats_t st;
        allocator_sb_stats(alloc, sb, &st);
        if (st.plane_count == 0U)
        {
            continue; /* 全哨兵条目 */
        }

        /* 四项各自归一化到 0..1000；cost 取小 = 越优先回收。
         * erase 项用 uint64_t 中间量防溢出（erase_count 是 uint32_t，×1000 理论上可溢出）。
         * age 项的 `max_pe_ts > pe_min` 同时兜住两种退化：① max_pe_ts == 0（上电后尚无块被擦）；
         * ② 本 SB 的 plane 块已被标坏 ⇒ 全表统计跳过它 ⇒ pe_min 可能大于 max_pe_ts
         * （相减会下溢成大数、把 yp 绕回）。两者一律取 age_ratio = 0（时间项中性）。 */
        uint32_t vr = ((uint32_t)st.valid_cw * 1000U) / ((uint32_t)st.plane_count * FTL_CW_PER_BLOCK);
        uint32_t er = (uint32_t)(((uint64_t)st.erase_max * 1000U) / ex.max_erase);
        uint32_t cp = (sb->temperature == FTL_TEMP_HOT) ? 0U : 1000U;
        uint32_t yp = 1000U - (((ex.max_pe_ts > st.pe_min) ? (uint32_t)(((uint64_t)(ex.max_pe_ts - st.pe_min) * 1000U) / ex.max_pe_ts) : 0U));

        uint32_t cost = (GC_WEIGHT_VALID * vr) + (GC_WEIGHT_ERASE * er) + (GC_WEIGHT_TEMP * cp) + (GC_WEIGHT_AGE * yp);

        /* cost 相等时取有效页更少者（确定性，且退化为单维贪心） */
        if ((cost < best_cost) || ((cost == best_cost) && (st.valid_cw < best_valid)))
        {
            best_cost  = cost;
            best_valid = st.valid_cw;
            best_vr    = vr;
            best_er    = er;
            best_cp    = cp;
            best_yp    = yp;
            *victim    = sb;
            found      = true;
        }
    }

    if (found)
    {
        /* 观测口：一次空闲 GC 突发最多打 FREE_POOL_TARGET = 32 行
         * （每 victim 每 plane 一行；ftl_service_idle 循环调用），
         * 相对 GC 自身的 NAND 擦除/搬移耗时可忽略。哨兵 plane 不打印。 */
        for (uint8_t p = 0U; p < PLANE_NUM; p++)
        {
            if (!super_block_has_plane(*victim, p))
            {
                continue;
            }
            LOGI("[GC] victim plane%u=%u vr=%lu er=%lu cp=%lu yp=%lu cost=%lu\r\n",
                 (unsigned)p,
                 (unsigned)(*victim)->plane_blocks[p],
                 best_vr,
                 best_er,
                 best_cp,
                 best_yp,
                 best_cost);
        }
    }

    return found;
}

/* ── 4. 触发判定 ── */

/* --------------------------------------------------------------------
 * gc_check_triggers
 *   功能：算触发位掩码（调用点据此决定要不要回收、以及回收多少）。
 *   参数：alloc —— 分配器（前置条件：调用者持有实例；绑定由 ftl_contract_audit 证明）。
 *   返回：GC_TRIGGER_* 位掩码；0 无触发。
 *   实现要点：只看**空间与磨损**——T1 水位 < FREE_POOL_TARGET（主动补池）；
 *             T4 紧急 ≤ FREE_POOL_EMERGENCY（与 T1 同源但阈值更低，池见底）；
 *             T5 磨损比 max/min 超标（min==0 不触发——"有块从未被擦"是静态 WL 的职责；
 *             max 地板防早期空跑）。
 * -------------------------------------------------------------------- */
uint32_t gc_check_triggers(const ftl_allocator_t *alloc)
{
    /* NULL ⇒ 不触发：这是**文档化契约**（UT `test_gc_check_triggers` 钉住），
     * 不是静默吞（C5 判据 4）。 */
    if (alloc == NULL)
    {
        return 0U;
    }

    uint32_t triggers = 0U;

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
        gc_extremes_t ex;

        gc_table_extremes(alloc, &ex);
        if ((ex.max_erase >= GC_WEAR_LEVEL_MIN_ERASE) && (ex.min_erase > 0U) &&
            ((uint64_t)ex.max_erase > ((uint64_t)ex.min_erase * GC_WEAR_LEVEL_RATIO)))
        {
            triggers |= GC_TRIGGER_WEAR;
        }
    }

    return triggers;
}

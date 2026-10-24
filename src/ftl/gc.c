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
 * 三条触发（gc_should_trigger，位掩码）：
 *   T1 空闲水位 < FREE_POOL_TARGET；T4 紧急 ≤ FREE_POOL_EMERGENCY。
 *   磨损维护不在 GC 触发里，由 wear_leveling 的 UBI 判据独立启动。
 *
 * 文件结构：
 *   1. 共用原语        gc_read_page_with_lba / gc_decode_cw_lba
 *   2. 选 victim       gc_select_victim（四项加权评分）
 *   3. 触发判定        gc_should_trigger
 *
 * ⚠ 全部按**全局块号**索引 BST（跨 plane），退化单 plane 条目跳过哨兵 plane。
 */

#include "gc.h"

#include "assertion.h"
#include "fc.h"
#include "ftl_fc_interface.h"
#include "log.h"
#include "onfi_1_0.h"

#include <string.h>

/* ── 内部策略参数（仅 gc.c 使用）──
 * 权重为**千分比整数**：本仓无 FPU 便利、浮点格式化开销大 ⇒ 全整数，四项各自
 * 归一化到 0..1000 后再加权。符号约定：cost 取小 = 越优先回收；
 * 擦除项取 +erase_ratio（避开临近寿命块）；温度项 HOT 优先当 victim。 */
#define GC_WEIGHT_VALID (500U) /* 有效页占比（核心指标，直接决定写放大） */
#define GC_WEIGHT_ERASE (200U) /* 擦除次数占比（磨损均衡，避开临近寿命块） */
#define GC_WEIGHT_TEMP  (200U) /* 温度（HOT 数据自然死亡快） */
#define GC_WEIGHT_AGE   (100U) /* 时间（保证每个 Block 都有机会被回收） */

/* 收益判据阈值（本仓自拟经验值）。详见 gc_select_victim 的注释。 */
#define GC_VICTIM_MAX_VALID_PCT   (80U) /* 空闲补池：valid 占比 > 80% 视为无收益 */
#define GC_EMERGENCY_MIN_GAIN_PCT (1U)  /* 紧急水位：可回收占比 < 1% 视为无收益 */

/* ── 1. 共用原语 ── */

/*
 * gc_read_page_with_lba - 读取一页 NAND，取回**逐 CW 从 META 解出的逻辑地址**
 *
 * 内部管理 FC 命令生命周期：
 *   alloc → config → submit → sync_wait → get_meta → release
 *
 * 唯一调用者是掉电重建（PFR 的 full metadata rebuild（内部）的逐页反查）。
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
ftl_status_e gc_read_page_with_lba(DWORD pba, DWORD cw_lba[CW_NUM])
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

        ftl_status_e ret = ftl_sync_submit_wait(cmd, &completion, 0U);
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
gc_decode_e gc_decode_cw_lba(fc_command_t *cmd, DWORD pba, DWORD cw_lba[CW_NUM], DWORD *page_base_out)
{
    const BYTE phys_plane = pba_to_plane(pba);

    for (BYTE c = 0U; c < CW_NUM; c++)
    {
        cw_lba[c] = FTL_LBA_INVALID; /* 默认未写过 */
    }

    BYTE  found     = 0U;
    DWORD base_meta = 0U;
    for (BYTE c = 0U; c < CW_NUM; c++)
    {
        BYTE *meta_ptr = fc_buffer_get_meta(cmd, c);

        if (meta_ptr == NULL)
        {
            continue; /* 该 CW 不在链上（本入口链恒覆盖全 4 CW，防御） */
        }
        /* meta_ptr 指向 CW buffer，CW 基址 4 字节对齐，memcpy 避免 alignment 警告 */
        DWORD m;
        memcpy(&m, meta_ptr, sizeof(m));

        /* 合法 META 判据：非全 1（未写过）+ 低位 plane/cw 与本 CW 自洽 */
        if ((m == FTL_LBA_INVALID) || (ftl_lba_cw(m) != c) || (ftl_lba_plane(m) != phys_plane))
        {
            continue;
        }
        cw_lba[c] = m; /* 本 CW 自己的扇区 LBA（META 即 LBA，低位就是 plane/cw） */
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
    const DWORD base = ftl_lba_page_base(base_meta);

    for (BYTE c = 0U; c < CW_NUM; c++)
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

/* ── 2. 选 victim ── */

/* --------------------------------------------------------------------
 * gc_select_victim
 *   功能：从 Closed SB 列表里按四项加权评分选回收代价最小的 SB 作 victim，
 *         并在评分前按收益判据过滤「不值得搬」的候选。
 *   参数：alloc —— 分配器；mode —— 收益判据模式；out —— 出参（victim + 观测计数）。
 *   返回：GC_SELECT_OK / GC_SELECT_NO_CANDIDATE / GC_SELECT_NOT_WORTHWHILE。
 *   实现要点：四项各自归一化到 0..1000——vr 有效页率、er 磨损率（越大越**避开**临近寿命块）、
 *             cp 温度（COLD 加分）、yp 年龄（越老越优先）；cost = 加权和，取小者胜；
 *             cost 相等取有效页更少者（确定性）。erase 项用 QWORD 防溢出；
 *             age 项 `max_pe_ts > pe_min` 兜住 max_pe_ts==0 与 pe_min 越界两种退化（取中性 0）。
 *   ⚠ **先过滤、后评分**：若先选 cost 最小再判收益，可能出现「最差块仍太有效 ⇒ 报无收益，
 *     而其实还有更稀疏的候选」。过滤条件见本文件的收益判据阈值注释。
 *   ⚠ 分母用 `st.plane_count * FTL_CW_PER_BLOCK`（BAD/哨兵 plane 已被 alc_super_get_stats 排除）；
 *     不能按 next_page 缩分母——半写块的空洞页会让它看起来「100% 有效」而被误跳过。
 * -------------------------------------------------------------------- */
gc_select_e gc_select_victim(const alc_t *alloc, gc_benefit_mode_e mode, gc_select_out_t *out)
{
    /* 前置条件（判据 2）：调用者 migrate.c 传 &ftl->alloc、合法 mode 与栈上 &out。 */
    ASSERT(alloc != NULL);
    ASSERT(out != NULL);
    ASSERT((mode == GC_BENEFIT_OFF) || (mode == GC_BENEFIT_IDLE) || (mode == GC_BENEFIT_EMERGENCY));

    out->victim         = NULL;
    out->best_valid_pct = 0U;
    out->scanned        = 0U;
    out->eligible       = 0U;

    /* 归一化基准：全表最大擦除/时间（一次扫描，与候选数无关）。
     * 退化值：无任何可统计块 ⇒ max_erase 兜底 1 防除零；max_pe_ts==0 ⇒ 时间项中性。 */
    alc_bst_erase_stats_t erase_stats;

    alc_bst_get_erase_stats(alloc, &erase_stats);

    const DWORD max_erase = (erase_stats.erase_max > 0U) ? erase_stats.erase_max : 1U;
    const DWORD max_pe_ts = erase_stats.max_pe_ts;

    DWORD best_cost  = UINT32_MAX;
    WORD  best_valid = (FTL_CW_PER_SB + 1U);
    WORD  best_pct   = 101U; /* >100 = 尚未见到候选 */
    bool  found      = false;

    for (WORD i = 0U; i < alloc->closed_sb_count; i++)
    {
        const super_block_t *sb = alc_super_closed_get_at(alloc, i);

        if (sb->is_open)
        {
            continue; /* 仍在写的 SB 不是回收对象（UT 钉住的契约） */
        }

        /* 各 plane 的聚合值（有效 CW / 磨损 / 时间）由 allocator 统一计算：
         * 有效 CW = 各 plane valid_count 之和（CW 级真实计数）；退化单 plane 条目跳过哨兵 plane。 */
        alc_super_stats_t st;
        alc_super_get_stats(alloc, sb, &st);
        if (st.plane_count == 0U)
        {
            continue; /* 全哨兵条目 */
        }

        const DWORD total_cw = (DWORD)st.plane_count * FTL_CW_PER_BLOCK;
        const DWORD valid_cw = ((DWORD)st.valid_cw > total_cw) ? total_cw : (DWORD)st.valid_cw;
        const WORD  pct      = (WORD)((valid_cw * 100U) / total_cw);

        out->scanned++;
        if (pct < best_pct)
        {
            best_pct = pct;
        }

        /* ── 收益判据（先过滤，再评分）── */
        if (mode == GC_BENEFIT_IDLE)
        {
            if ((valid_cw * 100U) > (total_cw * GC_VICTIM_MAX_VALID_PCT))
            {
                continue; /* 有效率太高：搬它得到的空闲块还不够付目的页 */
            }
        }
        else if (mode == GC_BENEFIT_EMERGENCY)
        {
            if (((total_cw - valid_cw) * 100U) < (total_cw * GC_EMERGENCY_MIN_GAIN_PCT))
            {
                continue; /* 可回收比例不足：紧急水位下也不值得 churn */
            }
        }
        else
        {
            /* GC_BENEFIT_OFF：不筛 */
        }

        out->eligible++;

        /* 四项各自归一化到 0..1000；cost 取小 = 越优先回收。
         * erase 项用 QWORD 中间量防溢出（erase_count 是 DWORD，×1000 理论上可溢出）。
         * age 项的 `max_pe_ts > pe_min` 同时兜住两种退化：① max_pe_ts == 0（上电后尚无块被擦）；
         * ② 本 SB 的 plane 块已被标坏 ⇒ 全表统计跳过它 ⇒ pe_min 可能大于 max_pe_ts
         * （相减会下溢成大数、把 yp 绕回）。两者一律取 age_ratio = 0（时间项中性）。 */
        DWORD vr = (valid_cw * 1000U) / total_cw;
        DWORD er = (((QWORD)st.erase_max * 1000U) / max_erase);
        DWORD cp = (sb->temperature == FTL_TEMP_HOT) ? 0U : 1000U;
        DWORD yp = 1000U - (((max_pe_ts > st.pe_min) ? (((QWORD)(max_pe_ts - st.pe_min) * 1000U) / max_pe_ts) : 0U));

        DWORD cost = (GC_WEIGHT_VALID * vr) + (GC_WEIGHT_ERASE * er) + (GC_WEIGHT_TEMP * cp) + (GC_WEIGHT_AGE * yp);

        /* cost 相等时取有效页更少者（确定性，且退化为单维贪心） */
        if ((cost < best_cost) || ((cost == best_cost) && (st.valid_cw < best_valid)))
        {
            best_cost   = cost;
            best_valid  = st.valid_cw;
            out->victim = sb;
            found       = true;
        }
    }

    out->best_valid_pct = (best_pct <= 100U) ? best_pct : 0U;

    if (found)
    {
        return GC_SELECT_OK;
    }

    return (out->scanned == 0U) ? GC_SELECT_NO_CANDIDATE : GC_SELECT_NOT_WORTHWHILE;
}

/* ── 3. 触发判定 ── */

/* --------------------------------------------------------------------
 * gc_should_trigger
 *   功能：算触发位掩码（调用点据此决定要不要回收、以及回收多少）。
 *   参数：alloc —— 分配器（前置条件：调用者持有实例；绑定由 ftl_contract_audit 证明）。
 *   返回：GC_TRIGGER_* 位掩码；0 无触发。
 *   实现要点：只看**空间与磨损**——T1 水位 < FREE_POOL_TARGET（主动补池）；
 *             T4 紧急 ≤ FREE_POOL_EMERGENCY（与 T1 同源但阈值更低，池见底）；
 *             磨损维护由 wear_leveling 的 UBI 判据独立触发，不占 GC 触发位。
 * -------------------------------------------------------------------- */
DWORD gc_should_trigger(const alc_t *alloc)
{
    /* NULL ⇒ 不触发：这是**文档化契约**（UT `test_gc_check_triggers` 钉住），
     * 不是静默吞（C5 判据 4）。 */
    if (alloc == NULL)
    {
        return 0U;
    }

    DWORD triggers = 0U;

    /* T1：空闲块水位（主动补池，还留有余量就开工）。
     * ⚠ 分配器按 **plane 成对**取块；total free 在目标线附近时可能全部落在 plane0，
     *   而 plane1 已 0 块。旧判据只看 total ⇒ 不触发 GC，host 写只能吃 OP 池
     *   （现场：`free=32` 但 `op=204/157→112`）。因此总水位之外还要检查
     *   「任一 plane 无空闲块」。
     *   扫描到两个 plane 都出现即可提前退出，正常盘通常前几项就覆盖两 plane。 */
    bool seen[PLANE_NUM];
    memset(seen, 0, sizeof(seen));
    BYTE seen_n = 0U;

    for (WORD i = 0U; (i < alloc->free_count) && (seen_n < PLANE_NUM); i++)
    {
        const BYTE p = ftl_gblock_plane(alloc->free_blocks[i]);

        if ((p < PLANE_NUM) && (!seen[p]))
        {
            seen[p] = true;
            seen_n++;
        }
    }

    if ((alloc->free_count < FREE_POOL_TARGET) || (seen_n < PLANE_NUM))
    {
        triggers |= GC_TRIGGER_WATERMARK;
    }
    /* T4：紧急（池已见底的最后手段）。与 T1 同源但阈值更低（8 vs 32）。 */
    if (alloc->free_count <= FREE_POOL_EMERGENCY)
    {
        triggers |= GC_TRIGGER_EMERGENCY;
    }

    /* 磨损均衡不在这里触发：全表 max/min 比较会把“从未使用的空闲块”当作 min，
     * 比值可能永不收敛（板端曾因此持续 GC、free 被搬到 64）。磨损维护由
     * wear_leveling 的 UBI 判据（free-max vs used-min）在 host 空闲时独立启动。 */

    return triggers;
}

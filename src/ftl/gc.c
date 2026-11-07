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
 * 两条触发（gc_should_trigger，位掩码）：
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

// 收益判据阈值（本仓自拟经验值）。详见 gc_select_victim 的注释。
#define GC_VICTIM_MAX_VALID_PCT   (80U) /* 空闲补池：valid 占比 > 80% 视为无收益 */
#define GC_EMERGENCY_MIN_GAIN_PCT (1U)  /* 紧急水位：可回收占比 < 1% 视为无收益 */

/* ── 1. 共用原语 ── */

/**
 * @brief 读一页并逐 CW 取出 META LBA
 *
 * 成败判据 = FC_CMD_STATUS_ERROR_MASK 全集：UNC/META 页内容不可信。
 *
 * @param pba    目标页 PBA
 * @param cw_lba 输出：逐 CW 的 LBA（未写过 = FTL_LBA_INVALID）
 * @return FTL_OK；FTL_ERROR 读失败 / 内容不可信
 */
ftl_status_e gc_read_page_with_lba(DWORD pba, DWORD cw_lba[CW_NUM])
{
    // 命令池空是**资源类**结果（FTL_AGAIN）；唯一调用者在开机重建期，池结构性充足。
    fc_cmd_s *cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return FTL_AGAIN;
    }
    const fc_cmd_id_t cmd_id = cmd->cmd_id;

    cmd->config.operation   = OP_NORMAL_READ_PAGE_WITH_DATA_OUT;
    cmd->config.cw_count    = CW_NUM - 1U;
    cmd->config.crc         = FC_CRC_ENABLE;
    cmd->config.random      = FC_RANDOM_ENABLE;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.multi_plane = 0U;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    cmd->meta               = FC_META_UNKNOWN; /* 不知道 LBA——正是要从页内 META 读回来 */

    if (pba_to_cmd(cmd, pba) != FTL_OK)
    {
        (void)fc_command_release(cmd_id);
        return FTL_ERROR;
    }

    {
        fc_cmd_status_s completion;
        BOOL            submitted = false;

        const ftl_status_e ret = ftl_sync_submit_wait(cmd, &completion, 0U, &submitted);

        if (ret != FTL_OK)
        {
            // RISK-IF-17：只有「未提交」才由调用者 release；已提交但超时时命令仍在
            // FC/NAND，迟到完成由 ftl_op_drain 的 missing-request 分支释放。
            if (submitted == false)
            {
                (void)fc_command_release(cmd_id);
            }
            return FTL_ERROR;
        }

        // ⚠ [待上板] 成败判据 = ERROR_MASK 全集：只认 NAND_FAIL 会把 UNC / META_ERROR
        // 页当成功读并解析 META（重建期可能写回错误映射）。
        if (fc_cmd_status_has_error(completion.status))
        {
            (void)fc_command_release(cmd_id);
            return FTL_ERROR;
        }
    }

    // 从 META 解码逐 CW 的扇区 LBA（与异步搬移 job 共用同一份实现）。
    // 空页 = 无有效数据；页基址不一致 = META 损坏；两者都按失败返回。
    if (gc_decode_cw_lba(cmd, pba, cw_lba, NULL) != GC_DECODE_OK)
    {
        (void)fc_command_release(cmd_id);
        return FTL_ERROR;
    }

    (void)fc_command_release(cmd_id);

    return FTL_OK;
}

/**
 * @brief 解码命令 desc 链上的逐 CW LBA
 *
 * 只此一份，避免两处漂移。
 *
 * @param cmd           完成的读命令
 * @param pba           目标页 PBA
 * @param cw_lba        输出：逐 CW LBA
 * @param page_base_out 输出：页基址
 * @return 解码结果（OK / EMPTY / FAIL）
 */
gc_decode_e gc_decode_cw_lba(fc_cmd_s *cmd, DWORD pba, DWORD cw_lba[CW_NUM], DWORD *page_base_out)
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
        BYTE *meta_ptr = fc_buffer_get_meta(cmd->cmd_id, c);

        if (meta_ptr == NULL)
        {
            // 链上缺这条 CW 的 buffer ⇒ 无法证明它“没写过”。按未写处理会让它不被搬移、
            // 随后 victim 被擦 ⇒ L2P 悬空（静默丢数据）。整页按损坏上报，调用者不擦。
            return GC_DECODE_CORRUPT;
        }
        // meta_ptr 指向 CW buffer，CW 基址 4 字节对齐，memcpy 避免 alignment 警告
        DWORD m;
        memcpy(&m, meta_ptr, sizeof(m));

        // 合法 META 判据：非全 1（未写过）+ 低位 plane/cw 与本 CW 自洽
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

    // 页基址由**首个写过的 CW** 反推（CW0 可能是空洞）。同一物理页只被一次编程
    // 操作写入 ⇒ 所有写过的 CW 必须共享同一页基址；不一致说明 META 损坏。
    //
    // ⚠ 不能“作废个别 CW 后继续”：它们的 L2P 仍指向本页，若随后擦 victim 就会
    // 悬空。整页按不可迁移处理，由调用者中止 job（victim 不擦）。
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

/**
 * @brief 选一个 GC victim
 *
 * 有效率按 plane_count × 块容量算，不按 next_page 缩（半写块会被误判为
 * 100% 有效）。
 *
 * @param alloc 分配器
 * @param mode  收益模式
 * @param out   输出：victim 与统计
 * @return 选择结果（OK / NO_VICTIM / NO_GAIN）
 */
gc_select_e gc_select_victim(const alc_s *alloc, gc_benefit_mode_e mode, gc_select_out_s *out)
{
    ASSERT(alloc != NULL);
    ASSERT(out != NULL);
    ASSERT((mode == GC_BENEFIT_OFF) || (mode == GC_BENEFIT_IDLE) || (mode == GC_BENEFIT_EMERGENCY));

    out->victim         = NULL;
    out->best_valid_pct = 0U;
    out->scanned        = 0U;
    out->eligible       = 0U;

    // 归一化基准：全表最大擦除/时间（一次扫描，与候选数无关）。
    // 退化值：无任何可统计块 ⇒ max_erase 兜底 1 防除零；max_pe_ts==0 ⇒ 时间项中性。
    alc_bst_erase_stats_s erase_stats;

    alc_bst_get_erase_stats(alloc, &erase_stats);

    const DWORD max_erase = (erase_stats.erase_max > 0U) ? erase_stats.erase_max : 1U;
    const DWORD max_pe_ts = erase_stats.max_pe_ts;

    DWORD                best_cost      = UINT32_MAX;
    WORD                 best_valid     = (FTL_CW_PER_SB + 1U);
    DWORD                best_cost_any  = UINT32_MAX; /* 无偏好组（仅当偏好组为空时使用） */
    WORD                 best_valid_any = (FTL_CW_PER_SB + 1U);
    WORD                 best_pct       = 101U; /* >100 = 尚未见到候选 */
    BOOL                 found          = false;
    BOOL                 found_any      = false;
    const super_block_s *victim         = NULL;
    const super_block_s *victim_any     = NULL;

    // plane 平衡偏好（本仓自拟，RISK-ALLOC-09）：成对条目回收后两侧各 +1；单 plane 条目
    // 只补一侧。某 plane 低于单 plane 水位且两 plane 不均衡时，优先「成对条目 + 饥饿 plane
    // 的单条目」，避免 GC 把空闲块堆到一侧导致成对分配失败。
    BYTE starved = PLANE_NUM; /* == PLANE_NUM ⇒ 两 plane 持平，无偏好 */

    {
        WORD fmin = UINT16_MAX;
        WORD fmax = 0U;

        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            const WORD c = alc_free_get_count(alloc, p);

            if (c < fmin)
            {
                fmin    = c;
                starved = p;
            }
            if (c > fmax)
            {
                fmax = c;
            }
        }

        // 只在**真的饥饿**（低于单 plane 水位）时才引入偏好；两 plane 都健康时保持原贪心，
        // 避免为了持平去做无谓的高成本搬移。
        if ((fmin == fmax) || (fmin >= FREE_POOL_PLANE_LOW))
        {
            starved = PLANE_NUM;
        }
    }

    for (WORD i = 0U; i < alloc->closed_sb_count; i++)
    {
        const super_block_s *sb = alc_super_closed_get_at(alloc, i);

        if (sb->is_open)
        {
            continue; /* 仍在写的 SB 不是回收对象（UT 钉住的契约） */
        }

        // 各 plane 的聚合值（有效 CW / 磨损 / 时间）由 allocator 统一计算：
        // 有效 CW = 各 plane valid_count 之和（CW 级真实计数）；退化单 plane 条目跳过哨兵 plane。
        alc_super_stats_s st;
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
            // GC_BENEFIT_OFF：不筛
        }

        out->eligible++;

        // 四项各自归一化到 0..1000；cost 取小 = 越优先回收。
        // erase 项用 QWORD 中间量防溢出（erase_count 是 DWORD，×1000 理论上可溢出）。
        // age 项的 `max_pe_ts > pe_min` 同时兜住两种退化：① max_pe_ts == 0（上电后尚无块被擦）；
        // ② 本 SB 的 plane 块已被标坏 ⇒ 全表统计跳过它 ⇒ pe_min 可能大于 max_pe_ts
        // （相减会下溢成大数、把 yp 绕回）。两者一律取 age_ratio = 0（时间项中性）。
        DWORD vr = (valid_cw * 1000U) / total_cw;
        DWORD er = (((QWORD)st.erase_max * 1000U) / max_erase);
        DWORD cp = (sb->temperature == FTL_TEMP_HOT) ? 0U : 1000U;
        DWORD yp = 1000U - (((max_pe_ts > st.pe_min) ? (((QWORD)(max_pe_ts - st.pe_min) * 1000U) / max_pe_ts) : 0U));

        DWORD cost = (GC_WEIGHT_VALID * vr) + (GC_WEIGHT_ERASE * er) + (GC_WEIGHT_TEMP * cp) + (GC_WEIGHT_AGE * yp);

        // 偏好判定：成对条目（plane_count == PLANE_NUM）总是优先；单 plane 条目只有
        // 属于饥饿 plane 时才优先。
        const BOOL preferred = ((starved >= PLANE_NUM) || (st.plane_count != 1U)) ? true : super_block_has_plane(sb, starved);

        // cost 相等时取有效页更少者（确定性，且退化为单维贪心）
        if (preferred == true)
        {
            if ((cost < best_cost) || ((cost == best_cost) && (st.valid_cw < best_valid)))
            {
                best_cost  = cost;
                best_valid = st.valid_cw;
                victim     = sb;
                found      = true;
            }
        }
        else if ((cost < best_cost_any) || ((cost == best_cost_any) && (st.valid_cw < best_valid_any)))
        {
            best_cost_any  = cost;
            best_valid_any = st.valid_cw;
            victim_any     = sb;
            found_any      = true;
        }
    }

    out->best_valid_pct = (best_pct <= 100U) ? best_pct : 0U;

    if (found == true)
    {
        out->victim = victim;
        return GC_SELECT_OK;
    }

    if (found_any == true)
    {
        out->victim = victim_any; /* 偏好组为空（只有富 plane 的单条目）：退回原贪心 */
        return GC_SELECT_OK;
    }

    return (out->scanned == 0U) ? GC_SELECT_NO_CANDIDATE : GC_SELECT_NOT_WORTHWHILE;
}

/* ── 3. 触发判定 ── */

/**
 * @brief 求本轮 GC 触发位并集
 *
 * 磨损维护由 wear_leveling 独立触发，不占 GC 触发位。
 *
 * @param alloc 分配器
 * @return 触发位并集（0 = 不触发）
 */
DWORD gc_should_trigger(const alc_s *alloc)
{
    // NULL ⇒ 不触发：这是**文档化契约**（UT `test_gc_check_triggers` 钉住），
    // 不是静默吞（C5 判据 4）。
    if (alloc == NULL)
    {
        return 0U;
    }

    DWORD triggers = 0U;

    // T1：空闲块水位（主动补池，还留有余量就开工）。
    // ⚠ 分配器按 **plane 成对**取块；total free 在目标线附近时可能全部落在 plane0，
    // 而 plane1 已枯竭。判据 = 总水位 **或任一 plane 低于单 plane 水位**
    // （per-plane 计数由 allocator 闸门维护，O(PLANE_NUM)）。
    BOOL plane_low = false;

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        if (alc_free_get_count(alloc, p) < FREE_POOL_PLANE_LOW)
        {
            plane_low = true;
        }
    }

    if ((alloc->free_count < FREE_POOL_TARGET) || (plane_low == true))
    {
        triggers |= GC_TRIGGER_WATERMARK;
    }
    // T4：紧急（池已见底的最后手段）。与 T1 同源但阈值更低（8 vs 32）。
    if (alloc->free_count <= FREE_POOL_EMERGENCY)
    {
        triggers |= GC_TRIGGER_EMERGENCY;
    }

    // 磨损均衡不在这里触发：全表 max/min 比较会把“从未使用的空闲块”当作 min，
    // 比值可能永不收敛（板端曾因此持续 GC、free 被搬到 64）。磨损维护由
    // wear_leveling 的 UBI 判据（free-max vs used-min）在 host 空闲时独立启动。

    return triggers;
}

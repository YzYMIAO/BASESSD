/*
 * fi_wrap.c — 测试构建的链接期故障注入 wrapper
 *
 * 由 `-Wl,--wrap=<symbol>` 把生产模块对指定符号的调用转到本文件的 `__wrap_*`。
 * 生产源码不感知本文件；wrapper 默认调用 `__real_*` 并原样返回，只有 `fi_*`
 * 命中时才注入（H30：返回值不得丢弃）。
 *
 * 边界：
 *   · 只对**跨 TU**调用生效（同一 .c 内部的调用不会被 wrap）；
 *   · 通过函数指针 / 回调表的调用不会被 wrap；
 *   · 静态函数不可 wrap ⇒ 这些缝仍用生产源码的宏隔离 hook。
 *
 * 设计：docs/guides/testing_guide.md §12.4 / §12.10。
 */

#include "fi_wrap.h"

#include "fault_inject.h"

#if defined(FC_NAND_TEST_ENABLE)

#    include "allocator.h"
#    include "checkpoint.h"
#    include "fc_interface.h"
#    include "ftl_fc_interface.h"
#    include "ftl_op.h"
#    include "gc.h"

#    include <string.h>

/* ── __real_* 原型（必须与被 wrap 的函数签名逐字一致）── */

ftl_status_e __real_ftl_async_read_page(fc_cmd_s **out_cmd, DWORD pba, const ftl_page_cfg_s *cfg);
ftl_status_e __real_ftl_async_write_page(fc_cmd_s **out_cmd, DWORD pba, const void *data, DWORD length, const ftl_page_cfg_s *cfg);
ftl_status_e __real_ftl_async_write_page_cw(fc_cmd_s **out_cmd, DWORD pba, const BYTE *page_buf, DWORD cw_bitmap, const ftl_page_cfg_s *cfg);
ftl_status_e __real_ftl_async_write_pair_cmd(fc_cmd_s **out_cmd, const DWORD pba[PLANE_NUM], const BYTE *pair_buf, DWORD cw_bitmap, BYTE operation, const ftl_page_cfg_s *cfg);
ftl_status_e __real_ftl_async_write_page_mirror(fc_cmd_s **out_cmd, DWORD pba, const void *data, DWORD length, const ftl_page_cfg_s *cfg);
ftl_status_e __real_ftl_sync_write_page_mirror(DWORD pba, const void *data, DWORD length, const ftl_page_cfg_s *cfg);
ftl_status_e __real_ftl_sync_read_page_status(DWORD pba, void *buffer, DWORD length, const ftl_page_cfg_s *cfg, fc_cmd_status_s *completion_out);
ftl_status_e __real_ftl_async_erase_block(fc_cmd_s **out_cmd, DWORD pba);
ftl_status_e __real_ftl_async_erase_block_multi_plane(fc_cmd_s **out_cmd, DWORD pba);
ftl_status_e __real_ftl_sync_erase_block_multi_plane(DWORD pba);
ftl_status_e __real_ftl_sync_read_page(DWORD pba, void *buffer, DWORD length, const ftl_page_cfg_s *cfg);
ftl_status_e __real_ftl_sync_read_column(DWORD pba, WORD column, BYTE out[4]);
ftl_status_e __real_ftl_op_submit(fc_cmd_s *cmd, ftl_op_id_e op, void *ctx);
ftl_status_e __real_ftl_op_submit_batch(fc_cmd_s *const cmds[], DWORD n, ftl_op_id_e op, void *ctx);
fc_status_e  __real_fc_poll_completion(fc_cmd_status_s *completion);
fc_status_e  __real_fc_data_path_encode(fc_cmd_id_t cmd_id);
fc_status_e  __real_fc_data_path_process_read(fc_cmd_id_t cmd_id);
BOOL         __real_delay_time_now_us(DWORD *us);
fc_status_e  __real_fc_poll_completions(fc_cmd_status_s *completions, WORD max_count, WORD *count);
ftl_status_e __real_alc_super_open_alloc_pair(alc_s *alloc, alc_super_alloc_mode_e mode, DWORD out[PLANE_NUM]);

/* ── 强制 GC victim（测试侧确定性）── */

gc_select_e __real_gc_select_victim(const alc_s *alloc, gc_benefit_mode_e mode, gc_select_out_s *out);

static const super_block_s *s_forced_victim;
static fc_cmd_status_s      s_late_completion;
static BOOL                 s_late_valid;

#    define FI_HOLD_MAX 16U

static fc_cmd_status_s s_held[FI_HOLD_MAX];
static BYTE            s_held_count;
static BYTE            s_held_release;

void fi_wrap_release_held(void)
{
    s_held_release = 1U;
}

DWORD fi_wrap_held_count(void)
{
    return s_held_count;
}

void fi_wrap_gc_force_victim(const super_block_s *victim)
{
    s_forced_victim = victim;
}

void fi_wrap_reset(void)
{
    s_forced_victim = NULL;
    s_late_valid    = false;
    s_held_count    = 0U;
    s_held_release  = 0U;
}

gc_select_e __wrap_gc_select_victim(const alc_s *alloc, gc_benefit_mode_e mode, gc_select_out_s *out)
{
    if (s_forced_victim != NULL)
    {
        memset(out, 0, sizeof(*out));
        out->victim         = s_forced_victim;
        out->scanned        = 1U;
        out->eligible       = 1U;
        out->best_valid_pct = 0U;
        s_forced_victim     = NULL; /* 一次性 */
        return GC_SELECT_OK;
    }

    return __real_gc_select_victim(alloc, mode, out);
}

/**
 * @brief 是否数据页配置（random 使能 = 用户数据；禁用 = 元数据）。
 *
 * @param cfg 页配置
 * @return true = 数据页
 */
static BOOL fi_data_page(const ftl_page_cfg_s *cfg)
{
    return ((cfg != NULL) && (cfg->random == FC_RANDOM_ENABLE)) ? true : false;
}

/* ── FTL ↔ FC 页级原语 ── */

ftl_status_e __wrap_ftl_async_read_page(fc_cmd_s **out_cmd, DWORD pba, const ftl_page_cfg_s *cfg)
{
    if (fi_data_page(cfg) == true)
    {
        const BYTE plane = pba_to_plane(pba);

        if (fi_take_at(FI_MIG_READ_FAIL, plane) == true)
        {
            return FTL_ERROR;
        }
        if (fi_take_at(FI_RR_READ_FAIL, plane) == true)
        {
            return FTL_ERROR;
        }
    }
    else if (fi_take_at(FI_L2P_LOAD_FAIL, pba) == true)
    {
        // 元数据读失败：key = PBA（FI_KEY_ANY 时按次数命中全部元数据读）。
        return FTL_ERROR;
    }

    return __real_ftl_async_read_page(out_cmd, pba, cfg);
}

ftl_status_e __wrap_ftl_async_write_page(fc_cmd_s **out_cmd, DWORD pba, const void *data, DWORD length, const ftl_page_cfg_s *cfg)
{
    if (fi_data_page(cfg) == true)
    {
        const BYTE plane = pba_to_plane(pba);

        if (fi_take_at(FI_MIG_WRITE_FAIL, plane) == true)
        {
            return FTL_ERROR;
        }
    }
    else if (fi_take_at(FI_BBT_WRITE_FAIL, pba) == true)
    {
        // 元数据写失败：key = PBA（BBT / Journal / L2P 共用本入口，按 PBA 区分）。
        return FTL_ERROR;
    }

    return __real_ftl_async_write_page(out_cmd, pba, data, length, cfg);
}

ftl_status_e __wrap_ftl_async_write_page_cw(fc_cmd_s **out_cmd, DWORD pba, const BYTE *page_buf, DWORD cw_bitmap, const ftl_page_cfg_s *cfg)
{
    const BYTE plane = pba_to_plane(pba);

    if ((fi_data_page(cfg) == true) && (fi_take_at(FI_MIG_WRITE_FAIL, plane) == true))
    {
        return FTL_ERROR;
    }

    return __real_ftl_async_write_page_cw(out_cmd, pba, page_buf, cw_bitmap, cfg);
}

ftl_status_e __wrap_ftl_async_write_pair_cmd(fc_cmd_s **out_cmd, const DWORD pba[PLANE_NUM], const BYTE *pair_buf, DWORD cw_bitmap, BYTE operation, const ftl_page_cfg_s *cfg)
{
    if (fi_data_page(cfg) == true)
    {
        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            if (fi_take_at(FI_MIG_WRITE_FAIL, p) == true)
            {
                return FTL_ERROR; /* host 写 / cache program 批次（pair 页） */
            }
        }
    }

    return __real_ftl_async_write_pair_cmd(out_cmd, pba, pair_buf, cw_bitmap, operation, cfg);
}

ftl_status_e __wrap_ftl_async_write_page_mirror(fc_cmd_s **out_cmd, DWORD pba, const void *data, DWORD length, const ftl_page_cfg_s *cfg)
{
    if ((fi_data_page(cfg) == false) && (fi_take_at(FI_BBT_WRITE_FAIL, pba) == true))
    {
        return FTL_ERROR; /* 元数据镜像写（BBT record）失败 */
    }

    return __real_ftl_async_write_page_mirror(out_cmd, pba, data, length, cfg);
}

ftl_status_e __wrap_ftl_sync_write_page_mirror(DWORD pba, const void *data, DWORD length, const ftl_page_cfg_s *cfg)
{
    if ((fi_data_page(cfg) == false) && (fi_take_at(FI_BBT_WRITE_FAIL, pba) == true))
    {
        return FTL_ERROR; /* 同步镜像写（开机 / 格式化 / bbt_save）失败 */
    }

    return __real_ftl_sync_write_page_mirror(pba, data, length, cfg);
}

ftl_status_e __wrap_ftl_async_erase_block(fc_cmd_s **out_cmd, DWORD pba)
{
    const BYTE plane = pba_to_plane(pba);

    if (fi_take_at(FI_MIG_ERASE_FAIL, plane) == true)
    {
        return FTL_ERROR;
    }

    return __real_ftl_async_erase_block(out_cmd, pba);
}

ftl_status_e __wrap_ftl_async_erase_block_multi_plane(fc_cmd_s **out_cmd, DWORD pba)
{
    const BYTE plane = pba_to_plane(pba);

    if (fi_take_at(FI_MIG_ERASE_FAIL, plane) == true)
    {
        return FTL_ERROR;
    }

    return __real_ftl_async_erase_block_multi_plane(out_cmd, pba);
}

ftl_status_e __wrap_ftl_sync_erase_block_multi_plane(DWORD pba)
{
    const BYTE plane = pba_to_plane(pba);

    if (fi_take_at(FI_MIG_ERASE_FAIL, plane) == true)
    {
        return FTL_ERROR;
    }

    return __real_ftl_sync_erase_block_multi_plane(pba);
}

ftl_status_e __wrap_ftl_sync_read_page(DWORD pba, void *buffer, DWORD length, const ftl_page_cfg_s *cfg)
{
    if ((fi_data_page(cfg) == false) && (fi_take_at(FI_L2P_LOAD_FAIL, pba) == true))
    {
        return FTL_ERROR;
    }

    return __real_ftl_sync_read_page(pba, buffer, length, cfg);
}

ftl_status_e __wrap_ftl_sync_read_page_status(DWORD pba, void *buffer, DWORD length, const ftl_page_cfg_s *cfg, fc_cmd_status_s *completion_out)
{
    if ((fi_data_page(cfg) == false) && (fi_take_at(FI_L2P_LOAD_FAIL, pba) == true))
    {
        if (completion_out != NULL)
        {
            memset(completion_out, 0, sizeof(*completion_out));
        }
        return FTL_ERROR;
    }

    return __real_ftl_sync_read_page_status(pba, buffer, length, cfg, completion_out);
}

ftl_status_e __wrap_ftl_sync_read_column(DWORD pba, WORD column, BYTE out[4])
{
    if (fi_take_at(FI_READ_COLUMN_FAIL, pba) == true)
    {
        return FTL_ERROR;
    }

    return __real_ftl_sync_read_column(pba, column, out);
}

/* ── FTL 提交入口：CP phase 中止 / BBT 提交失败 / 门控饥饿 ── */

ftl_status_e __wrap_ftl_op_submit(fc_cmd_s *cmd, ftl_op_id_e op, void *ctx)
{
    if (fi_take(FI_SEQ_GATE_STARVE) == true)
    {
        return FTL_AGAIN; /* 模拟被序列门控拒绝 */
    }

    // host 数据读由 ftl_issue_subop **内联建链**后直接提交（不走 ftl_async_read_page），
    // 所以 DATA 读的提交层失败要在本入口补。RR/L2P/元数据读仍由 async wrap 覆盖。
    if ((op == FTL_OP_HOST_IO) && (cmd != NULL) && ((cmd->config.operation == OP_NORMAL_READ_PAGE_WITH_DATA_OUT) || (cmd->config.operation == OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT)) &&
        (fi_take(FI_MIG_READ_FAIL) == true))
    {
        return FTL_ERROR;
    }

    if ((op == FTL_OP_CHECKPOINT) && (ctx != NULL))
    {
        const cp_handle_s *h = (const cp_handle_s *)ctx;

        if (fi_take_at(FI_CP_ABORT_PHASE, h->phase) == true)
        {
            return FTL_ERROR;
        }
    }

    if ((op == FTL_OP_BBT) && (fi_take(FI_BBT_WRITE_FAIL) == true))
    {
        return FTL_ERROR;
    }

    return __real_ftl_op_submit(cmd, op, ctx);
}

ftl_status_e __wrap_ftl_op_submit_batch(fc_cmd_s *const cmds[], DWORD n, ftl_op_id_e op, void *ctx)
{
    if (fi_take(FI_SEQ_GATE_STARVE) == true)
    {
        // 批提交被门控整批拒绝：调用者按契约自行释放命令（同真实 FTL_AGAIN 路径）。
        return FTL_AGAIN;
    }

    return __real_ftl_op_submit_batch(cmds, n, op, ctx);
}

/* ── 分配入口 ── */

ftl_status_e __wrap_alc_super_open_alloc_pair(alc_s *alloc, alc_super_alloc_mode_e mode, DWORD out[PLANE_NUM])
{
    if (fi_take(FI_ALLOC_FAIL) == true)
    {
        return FTL_AGAIN; /* 粗粒度：模拟 free / OP 都拿不到块 */
    }

    return __real_alc_super_open_alloc_pair(alloc, mode, out);
}

/* ── 数据路径 / 时基（D2）── */

fc_status_e __wrap_fc_data_path_encode(fc_cmd_id_t cmd_id)
{
    if (fi_take(FI_DATA_PATH_ENCODE_FAIL) == true)
    {
        return FC_STATUS_NOT_SUPPORTED;
    }
    return __real_fc_data_path_encode(cmd_id);
}

fc_status_e __wrap_fc_data_path_process_read(fc_cmd_id_t cmd_id)
{
    if (fi_take(FI_DATA_PATH_READ_FAIL) == true)
    {
        return FC_STATUS_IO_ERROR;
    }
    return __real_fc_data_path_process_read(cmd_id);
}

BOOL __wrap_delay_time_now_us(DWORD *us)
{
    if (fi_take(FI_CLOCK_NOW_FAIL) == true)
    {
        return false;
    }
    return __real_delay_time_now_us(us);
}

/* ── FC 完成状态（CQ 出口）── */

static void fi_fc_status_apply(fc_cmd_status_s *completion)
{
    const fc_cmd_id_t cmd_id = completion->cmd_id;
    const fc_cmd_s   *cmd    = fc_command_id_to_command(cmd_id);
    const DWORD       key    = (cmd != NULL) ? cmd->config.operation : FI_KEY_ANY;
    DWORD             inj    = 0U;

    if (fi_take_ex(FI_FC_STATUS, key, &inj) == true)
    {
        completion->status |= (inj & 0xFFU); /* status 位域 8 bit（H22） */
    }

    /* 仅 DATA 命令：GC victim 读与 L2P 元数据读共用 opcode，靠 cfg.random 区分。 */
    if ((cmd != NULL) && (cmd->config.random == FC_RANDOM_ENABLE))
    {
        inj = 0U;
        if (fi_take_ex(FI_FC_STATUS_DATA, key, &inj) == true)
        {
            completion->status |= (inj & 0xFFU);
        }
    }

    /* 按 plane 的完成态注入：RR 的逐 plane 重读 / 多 plane 命令用。
     * 只对**数据命令**生效——否则 0 号 plane 上的 Journal/L2P 元数据写也会被标错，
     * 注入面远大于用例意图。 */
    if ((cmd != NULL) && (cmd->config.random == FC_RANDOM_ENABLE))
    {
        const BYTE plane = paa_to_plane(cmd->paa);

        inj = 0U;
        if (fi_take_ex(FI_FC_STATUS_PLANE, plane, &inj) == true)
        {
            completion->status |= (inj & 0xFFU);
        }
    }

    /* 按命令目标 PBA 精确注入：多 plane/后台并发时只命中指定块（如 Journal 换块擦）。 */
    if (cmd != NULL)
    {
        const DWORD pba = paa_to_pba((BYTE)cmd->ch, (BYTE)cmd->ce, cmd->paa);

        inj = 0U;
        if (fi_take_ex(FI_FC_STATUS_PBA, pba, &inj) == true)
        {
            completion->status |= (inj & 0xFFU);
        }
    }
}

fc_status_e __wrap_fc_poll_completion(fc_cmd_status_s *completion)
{
    /* 共享完成门：测试放行后，逐条投递被 FI_COMPLETION_HOLD 扣住的完成。 */
    if ((s_held_release != 0U) && (s_held_count != 0U))
    {
        if (completion != NULL)
        {
            *completion = s_held[0];
            s_held_count--;
            for (BYTE i = 0U; i < s_held_count; i++)
            {
                s_held[i] = s_held[i + 1U];
            }
            if (s_held_count == 0U)
            {
                s_held_release = 0U;
            }
            return FC_STATUS_OK;
        }
        return FC_STATUS_QUEUE_EMPTY;
    }

    /* 迟到的完成先投递（D2）：上一轮 poll 把一条完成压入暂存，调用者下次先看到它。 */
    if (s_late_valid == true)
    {
        if (completion != NULL)
        {
            *completion  = s_late_completion;
            s_late_valid = false;
            return FC_STATUS_OK;
        }
        return FC_STATUS_QUEUE_EMPTY;
    }

    const fc_status_e st = __real_fc_poll_completion(completion);

    if ((st == FC_STATUS_OK) && (completion != NULL))
    {
        const fc_cmd_s *cmd = fc_command_id_to_command(completion->cmd_id);
        const DWORD     key = (cmd != NULL) ? cmd->config.operation : FI_KEY_ANY;

        if (fi_take_ex(FI_COMPLETION_DROP, key, NULL) == true)
        {
            return FC_STATUS_QUEUE_EMPTY; /* 完成被丢弃：调用者只能超时 */
        }
        if (fi_take_ex(FI_COMPLETION_LATE, key, NULL) == true)
        {
            s_late_completion = *completion;
            s_late_valid      = true;
            return FC_STATUS_QUEUE_EMPTY; /* 本轮不投递，下一轮才可见 */
        }
        if (fi_take_ex(FI_COMPLETION_HOLD, key, NULL) == true)
        {
            if (s_held_count < FI_HOLD_MAX)
            {
                s_held[s_held_count++] = *completion;
            }
            return FC_STATUS_QUEUE_EMPTY; /* 扣住，等测试显式放行 */
        }

        fi_fc_status_apply(completion);
    }

    return st;
}

fc_status_e __wrap_fc_poll_completions(fc_cmd_status_s *completions, WORD max_count, WORD *count)
{
    const fc_status_e st = __real_fc_poll_completions(completions, max_count, count);

    if ((st == FC_STATUS_OK) && (completions != NULL) && (count != NULL))
    {
        for (WORD i = 0U; i < *count; i++)
        {
            fi_fc_status_apply(&completions[i]);
        }
    }

    return st;
}

#endif /* FC_NAND_TEST_ENABLE */

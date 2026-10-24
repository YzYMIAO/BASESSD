/*
 * test_speed.c — 板端速度测试（FC 裸块 / FTL host API）
 *
 * 两条用例：
 *   /speed/fc_nand_rw —— FC 公共接口异步读写 100 个块（两档：normal/cache）
 *   /speed/ftl_rw     —— FTL host API 读写 100 gblock（两档：normal/cache）
 *
 * 计时口径：
 *   · FC：每块用 DWT 计时后累加（避免 CYCCNT 约 10.7 s 回绕）；擦除单独统计，不计入写速度。
 *   · FTL：用 ftl_job_now_ms()（1 ms）计整个 phase；format / flush 单列，不计入 KB/s。
 *
 * 输出遵守本仓 UT 约定：只在 `--log-visible info/debug` 下打印（[EV] 行），
 * 默认档只有用例名 + 结果。KB/s 按 1 KB = 1024 B 计算。
 *
 * ⚠ 破坏性：
 *   · FC 用例直接擦写 per-plane 块 1024..，会破坏 live FTL 的用户映射；
 *     FC-only 跑完必须 CLI `format` + POR。
 *   · FTL 用例会先完整 format，再覆写 LBA 0..51199（25 MiB）。
 *
 * ⚠ 本文件只允许在构建系统里添加**一次**（src/fc/makefile.mk，FC_NAND_TEST_ENABLE 内、
 *   UT_FC_ENABLE 外）；两段实现由 UT_FC_ENABLE / UT_FTL_ENABLE 预处理选择，避免 UT=
 *   （两者都开）时重复定义套件数组。
 */

#include "cli.h" /* g_ctrl / ssd_controller_host_lba_max */
#include "delay.h"
#include "ftl_job.h"
#include "munit.h"
#include "ssd_types.h"

#include <string.h>

/* ══════════════════════════════════════════════════════════════════════
 * §1 公共计时 / 速率换算
 * ══════════════════════════════════════════════════════════════════════ */

/* 1 KB = 1024 B。bytes 最大 25 MiB，乘 1000 后仍在 64 位内。 */
static DWORD spd_kbps_from_us(DWORD bytes, DWORD us)
{
    if (us == 0U)
    {
        return 0U;
    }
    /* KB/s = bytes / (1024 * seconds) = bytes × 1e6 / (1024 × µs)。 */
    return (DWORD)(((QWORD)bytes * 1000000U) / ((QWORD)1024U * us));
}

/* ══════════════════════════════════════════════════════════════════════
 * §2 FC-NAND 裸块速度
 * ══════════════════════════════════════════════════════════════════════ */

#if defined(UT_FC_ENABLE)

typedef struct
{
    DWORD start_cycles;
    DWORD start_ms;
    bool  dwt;
} spd_clock_t;

static void spd_clock_start(spd_clock_t *clock)
{
    memset(clock, 0, sizeof(*clock));
    clock->dwt = delay_dwt_cycles_now(&clock->start_cycles);
    if (!clock->dwt)
    {
        clock->start_ms = ftl_job_now_ms();
    }
}

static DWORD spd_clock_elapsed_us(const spd_clock_t *clock)
{
    if (clock->dwt)
    {
        return delay_dwt_elapsed_cycles_us(clock->start_cycles);
    }

    const DWORD now = ftl_job_now_ms();

    return (now - clock->start_ms) * 1000U;
}

#    include "bst.h"
#    include "fc.h"
#    include "fc_command.h"
#    include "fc_interface.h"
#    include "fc_test_common.h"
#    include "ftl_config.h"
#    include "nand_config.h"
#    include "onfi_1_0.h"

#    define SPD_FC_BLOCKS      100U
#    define SPD_FC_BLOCK_START (BLOCKS_PER_PLANE / 2U) /* 用户要求：BLOCK_NUM/2 起步 */
#    define SPD_FC_REAP_MAX    16U
#    define SPD_FC_REAP_EVERY  8U
#    define SPD_FC_DRAIN_GUARD 20000U
#    define SPD_FC_BYTES       ((DWORD)(SPD_FC_BLOCKS * PAGES_PER_BLOCK * PLANE_NUM * PAGE_MAIN_SIZE))

typedef enum
{
    SPD_FC_MODE_NORMAL = 0,
    SPD_FC_MODE_CACHE,
} spd_fc_mode_e;

typedef struct
{
    fc_command_t *cmd; /* NULL = 空槽 */
    WORD          block;
    BYTE          page;
    BYTE          ch;
    BYTE          ce;
    BYTE          lun;
    bool          verify; /* true = 读命令，完成前要逐 desc 校验 */
} spd_fc_slot_t;

static spd_fc_slot_t s_fc_slots[FC_COMMAND_POOL_DEPTH];
static WORD          s_fc_inflight;

static DWORD spd_fc_write_op(spd_fc_mode_e mode, BYTE page)
{
    if (mode == SPD_FC_MODE_NORMAL)
    {
        return (DWORD)OP_PROGRAM_PAGE_MULTI_PLANE;
    }
    if (page == 0U)
    {
        return (DWORD)OP_TWO_PLANE_PROGRAM_CACHE_FIRST;
    }
    if (page == (BYTE)(PAGES_PER_BLOCK - 1U))
    {
        return (DWORD)OP_TWO_PLANE_PROGRAM_CACHE_END;
    }
    return (DWORD)OP_TWO_PLANE_PROGRAM_CACHE;
}

static DWORD spd_fc_read_op(spd_fc_mode_e mode, BYTE page)
{
    if (mode == SPD_FC_MODE_NORMAL)
    {
        return (DWORD)OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT;
    }
    if (page == 0U)
    {
        return (DWORD)OP_READ_PAGE_CACHE_START_SEQ_MP;
    }
    if (page == (BYTE)(PAGES_PER_BLOCK - 1U))
    {
        return (DWORD)OP_READ_PAGE_CACHE_LAST_MP;
    }
    return (DWORD)OP_READ_PAGE_CACHE_SEQ_MP;
}

static fc_command_t *spd_fc_try_build(spd_fc_mode_e mode, bool write, WORD block, BYTE page, BYTE ch, BYTE ce, BYTE lun)
{
    nand_addr_t   addr;
    fc_cmd_conf_t conf;
    fc_command_t *cmd;

    memset(&addr, 0, sizeof(addr));
    addr.ch    = ch;
    addr.ce    = ce;
    addr.lun   = lun;
    addr.block = block;
    addr.plane = 0U;
    addr.page  = page;

    /* 生产口径：CRC + 加扰 + BCH（不是 L3 压力用例的 RAW/LDPC 简化档）。 */
    memset(&conf, 0, sizeof(conf));
    conf.operation = write ? spd_fc_write_op(mode, page) : spd_fc_read_op(mode, page);
    conf.crc       = FC_CRC_ENABLE;
    conf.random    = FC_RANDOM_ENABLE;
    conf.ldpc      = FC_ECC_MODE_BCH;
    conf.raid      = FC_RAID_MODE_NORMAL;

    cmd = test_cmd_try_build(&addr, test_plane_cw_bitmap(TEST_PLANE_BM_ALL), pba_from_addr(&addr), &conf);
    if ((cmd != NULL) && write)
    {
        test_fill_user_paa(cmd);
    }
    return cmd;
}

static void spd_fc_slots_reset(void)
{
    memset(s_fc_slots, 0, sizeof(s_fc_slots));
    s_fc_inflight = 0U;
}

static spd_fc_slot_t *spd_fc_slot_alloc(void)
{
    for (WORD i = 0U; i < (WORD)FC_COMMAND_POOL_DEPTH; i++)
    {
        if (s_fc_slots[i].cmd == NULL)
        {
            return &s_fc_slots[i];
        }
    }
    return NULL;
}

static spd_fc_slot_t *spd_fc_slot_find(WORD command_id)
{
    for (WORD i = 0U; i < (WORD)FC_COMMAND_POOL_DEPTH; i++)
    {
        fc_command_id_t id = FC_COMMAND_ID_INVALID;

        if (s_fc_slots[i].cmd == NULL)
        {
            continue;
        }
        if ((fc_command_to_id(s_fc_slots[i].cmd, &id) != 0U) && ((WORD)id == command_id))
        {
            return &s_fc_slots[i];
        }
    }
    return NULL;
}

static void spd_fc_note_peak(fc_resource_stats_t *peak)
{
    fc_resource_stats_t now;

    memset(&now, 0, sizeof(now));
    if (fc_get_resource_stats(&now) != FC_STATUS_OK)
    {
        return;
    }

#    define SPD_FC_MAX_FIELD(field)      \
        do                               \
        {                                \
            if (now.field > peak->field) \
            {                            \
                peak->field = now.field; \
            }                            \
        } while (0)

    SPD_FC_MAX_FIELD(sq_used);
    SPD_FC_MAX_FIELD(hsq_used);
    SPD_FC_MAX_FIELD(cq_used);
    SPD_FC_MAX_FIELD(command_used);
    SPD_FC_MAX_FIELD(descriptor_used);
    SPD_FC_MAX_FIELD(buffer_used);
#    undef SPD_FC_MAX_FIELD
}

/* 读完成项：逐 desc 校验「PAA 是期望页/plane/CW」且「USER 区等于该 PAA 图案」。
 * 必须在 release 之前调用（release 会归还 buffer）。 */
static DWORD spd_fc_verify_read(const spd_fc_slot_t *slot)
{
    DWORD              errors = 0U;
    BYTE               idx    = 0U;
    fc_descriptor_id_t id     = (fc_descriptor_id_t)slot->cmd->desc_id;

    while ((id != FC_DESCRIPTOR_ID_INVALID) && (idx < CW_TOTAL_MAX))
    {
        const fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(id);

        if (desc == NULL)
        {
            errors++;
            break;
        }

        nand_addr_t addr;

        memset(&addr, 0, sizeof(addr));
        addr.ch    = slot->ch;
        addr.ce    = slot->ce;
        addr.lun   = slot->lun;
        addr.block = slot->block;
        addr.page  = slot->page;
        addr.plane = (BYTE)paa_to_plane(desc->paa);
        addr.cw    = (BYTE)paa_to_cw(desc->paa);

        const DWORD expected = paa_from_addr(&addr);
        const BYTE *user     = fc_buffer_id_to_real_buffer(desc->buffer_id);

        if (desc->paa != expected)
        {
            errors++;
        }
        else if ((user == NULL) || (test_check_paa(user, expected) == 0U))
        {
            errors++;
        }

        id = (fc_descriptor_id_t)desc->next_id;
        idx++;
    }

    if (idx != (BYTE)(PLANE_NUM * CW_NUM))
    {
        errors++; /* 链长不对：没拿到预期 8 个 CW */
    }
    return errors;
}

static WORD spd_fc_reap(DWORD *errors, fc_resource_stats_t *peak)
{
    fc_command_status_t buf[SPD_FC_REAP_MAX];
    WORD                n = 0U;

    (void)fc_poll_completions(buf, (WORD)SPD_FC_REAP_MAX, &n);

    for (WORD i = 0U; i < n; i++)
    {
        spd_fc_slot_t *slot = spd_fc_slot_find((WORD)buf[i].command_id);

        if (slot == NULL)
        {
            (*errors)++;
            continue;
        }

        if ((buf[i].status & FC_CW_STATUS_ERROR_MASK) != 0U)
        {
            (*errors)++;
        }
        if (slot->verify)
        {
            (*errors) += spd_fc_verify_read(slot);
        }
        if (fc_command_release(slot->cmd) != FC_STATUS_OK)
        {
            (*errors)++;
        }
        slot->cmd = NULL;

        if (s_fc_inflight != 0U)
        {
            s_fc_inflight--;
        }
    }

    spd_fc_note_peak(peak);
    return n;
}

static bool spd_fc_drain(DWORD *errors, fc_resource_stats_t *peak)
{
    DWORD guard = 0U;

    while (s_fc_inflight != 0U)
    {
        (void)fc_service();
        (void)spd_fc_reap(errors, peak);

        if (++guard > SPD_FC_DRAIN_GUARD)
        {
            return false;
        }
    }
    return true;
}

/* 一个 phase：block → ch → ce → lun → page。每块结束 drain 到在途归零：
 *   · cache 序列边界明确（FIRST..END 不跨块）；
 *   · 本块 DWT 计时不会跨 CYCCNT 回绕。
 * 池上限约 16 条 multi-plane 页命令（8 desc + 8 buf/条），块内仍会把在途顶到上限。 */
static bool spd_fc_run_phase(spd_fc_mode_e mode, bool write, const WORD blocks[SPD_FC_BLOCKS], DWORD *us_out, DWORD *errors, fc_resource_stats_t *peak)
{
    DWORD total_us = 0U;

    spd_fc_slots_reset();

    for (WORD b = 0U; b < SPD_FC_BLOCKS; b++)
    {
        spd_clock_t clock;
        WORD        since_reap = 0U;
        DWORD       guard      = 0U;
        bool        ok         = true;

        spd_clock_start(&clock);

        for (BYTE ch = 0U; (ch < (BYTE)CH_NUM) && ok; ch++)
        {
            for (BYTE ce = 0U; (ce < (BYTE)CE_NUM) && ok; ce++)
            {
                for (BYTE lun = 0U; (lun < (BYTE)LUN_NUM) && ok; lun++)
                {
                    for (BYTE page = 0U; (page < (BYTE)PAGES_PER_BLOCK) && ok; page++)
                    {
                        for (;;)
                        {
                            spd_fc_slot_t *slot = spd_fc_slot_alloc();
                            fc_command_t  *cmd;

                            if (slot == NULL)
                            {
                                (void)fc_service();
                                (void)spd_fc_reap(errors, peak);
                                if (++guard > SPD_FC_DRAIN_GUARD)
                                {
                                    (*errors)++;
                                    ok = false;
                                    break;
                                }
                                continue;
                            }

                            cmd = spd_fc_try_build(mode, write, blocks[b], page, ch, ce, lun);
                            if (cmd != NULL)
                            {
                                if (fc_submit(cmd) == FC_STATUS_OK)
                                {
                                    slot->cmd    = cmd;
                                    slot->block  = blocks[b];
                                    slot->page   = page;
                                    slot->ch     = ch;
                                    slot->ce     = ce;
                                    slot->lun    = lun;
                                    slot->verify = !write;
                                    s_fc_inflight++;
                                    since_reap++;

                                    if (since_reap >= (WORD)SPD_FC_REAP_EVERY)
                                    {
                                        (void)fc_service();
                                        (void)spd_fc_reap(errors, peak);
                                        since_reap = 0U;
                                    }
                                    break; /* 本页已提交 */
                                }
                                (void)fc_command_release(cmd);
                            }

                            (void)fc_service();
                            (void)spd_fc_reap(errors, peak);
                            if (++guard > SPD_FC_DRAIN_GUARD)
                            {
                                (*errors)++;
                                ok = false;
                                break;
                            }
                        }
                    }
                }
            }
        }

        if (!spd_fc_drain(errors, peak))
        {
            (*errors)++;
            ok = false;
        }
        total_us += spd_clock_elapsed_us(&clock);

        if (!ok)
        {
            return false;
        }
    }

    *us_out = total_us;
    return true;
}

static bool spd_fc_pair_is_good(const ftl_ctx_t *ftl, WORD block)
{
    if (ftl == NULL)
    {
        return true;
    }
    for (BYTE plane = 0U; plane < (BYTE)PLANE_NUM; plane++)
    {
        if (ftl_block_is_bad(&ftl->alloc.blocks[ftl_gblock(plane, block)]))
        {
            return false;
        }
    }
    return true;
}

static WORD spd_fc_pick_blocks(WORD *out)
{
    const ftl_ctx_t *ftl = g_ctrl.ftl[0][0][0];
    WORD             n   = 0U;

    /* 先按用户要求从 50% 处往上；不够再回扫 98..（避开固定元数据 0..41）。 */
    for (WORD block = (WORD)SPD_FC_BLOCK_START; (block < (WORD)BLOCKS_PER_PLANE) && (n < SPD_FC_BLOCKS); block++)
    {
        if (spd_fc_pair_is_good(ftl, block))
        {
            out[n++] = block;
        }
    }
    for (WORD block = 98U; (block < (WORD)SPD_FC_BLOCK_START) && (n < SPD_FC_BLOCKS); block++)
    {
        if (spd_fc_pair_is_good(ftl, block))
        {
            out[n++] = block;
        }
    }
    return n;
}

static void spd_fc_erase_blocks(const WORD blocks[SPD_FC_BLOCKS], DWORD *us_out, DWORD *errors)
{
    fc_cmd_conf_t conf;
    DWORD         total_us = 0U;

    memset(&conf, 0, sizeof(conf));
    conf.operation = (DWORD)OP_ERASE_BLOCK_MULTI_PLANE;
    conf.crc       = FC_CRC_ENABLE;
    conf.random    = FC_RANDOM_ENABLE;
    conf.ldpc      = FC_ECC_MODE_BCH;
    conf.raid      = FC_RAID_MODE_NORMAL;

    for (WORD i = 0U; i < SPD_FC_BLOCKS; i++)
    {
        spd_clock_t         clock;
        fc_command_status_t status;

        spd_clock_start(&clock);
        status = test_erase_block(blocks[i], TEST_PLANE_BM_ALL, &conf);
        total_us += spd_clock_elapsed_us(&clock);

        if ((status.status & FC_CW_STATUS_ERROR_MASK) != 0U)
        {
            (*errors)++;
        }
    }
    *us_out = total_us;
}

static void spd_fc_assert_pools_empty(void)
{
    fc_resource_stats_t stats;

    memset(&stats, 0, sizeof(stats));
    munit_assert_int(fc_get_resource_stats(&stats), ==, FC_STATUS_OK);
    munit_assert_uint32(stats.sq_used, ==, 0U);
    munit_assert_uint32(stats.hsq_used, ==, 0U);
    munit_assert_uint32(stats.cq_used, ==, 0U);
    munit_assert_uint32(stats.command_used, ==, 0U);
    munit_assert_uint32(stats.descriptor_used, ==, 0U);
    munit_assert_uint32(stats.buffer_used, ==, 0U);
    munit_assert_uint32(s_fc_inflight, ==, 0U);
}

static MunitResult test_fc_speed_mp(const MunitParameter params[], void *data)
{
    (void)data;

    const char *mode_s = munit_parameters_get(params, "mode");

    if ((mode_s == NULL) || ((strcmp(mode_s, "normal") != 0) && (strcmp(mode_s, "cache") != 0)))
    {
        munit_errorf("unknown speed mode '%s'", (mode_s != NULL) ? mode_s : "(null)");
    }

    const spd_fc_mode_e mode = (strcmp(mode_s, "cache") == 0) ? SPD_FC_MODE_CACHE : SPD_FC_MODE_NORMAL;

    test_trace_begin();

    ftl_ctx_t *ftl           = g_ctrl.ftl[0][0][0];
    const bool saved_suspend = (ftl != NULL) ? ftl->bg_suspend : false;

    /* 原始擦写期间禁止 FTL 后台/分配器动作；测试返回后由用户 format+POR 收尾。 */
    if (ftl != NULL)
    {
        ftl->bg_suspend = true;
    }

    WORD       blocks[SPD_FC_BLOCKS];
    const WORD good = spd_fc_pick_blocks(blocks);

    if (good != (WORD)SPD_FC_BLOCKS)
    {
        if (ftl != NULL)
        {
            ftl->bg_suspend = saved_suspend;
        }
        munit_errorf("only %hu good block pairs found (need %u)", good, SPD_FC_BLOCKS);
    }

    DWORD               errors   = 0U;
    DWORD               erase_us = 0U;
    DWORD               wr_us    = 0U;
    DWORD               rd_us    = 0U;
    fc_resource_stats_t peak;

    memset(&peak, 0, sizeof(peak));

    spd_fc_erase_blocks(blocks, &erase_us, &errors);
    if (!spd_fc_run_phase(mode, true, blocks, &wr_us, &errors, &peak))
    {
        (void)spd_fc_drain(&errors, &peak);
        if (ftl != NULL)
        {
            ftl->bg_suspend = saved_suspend;
        }
        munit_errorf("fc speed write phase did not drain (errors=%lu)", errors);
    }
    spd_fc_assert_pools_empty();

    if (!spd_fc_run_phase(mode, false, blocks, &rd_us, &errors, &peak))
    {
        (void)spd_fc_drain(&errors, &peak);
        if (ftl != NULL)
        {
            ftl->bg_suspend = saved_suspend;
        }
        munit_errorf("fc speed read phase did not drain (errors=%lu)", errors);
    }
    spd_fc_assert_pools_empty();

    if (ftl != NULL)
    {
        ftl->bg_suspend = saved_suspend;
    }

    const DWORD bytes = (DWORD)SPD_FC_BYTES;

    test_trace_summary("SPEED fc mode=%s  WR=%lu KB/s  RD=%lu KB/s  |  blocks=%u bytes=%lu wr_ms=%lu rd_ms=%lu erase_ms=%lu err=%lu", mode_s, spd_kbps_from_us(bytes, wr_us), spd_kbps_from_us(bytes, rd_us), SPD_FC_BLOCKS, bytes, wr_us / 1000U, rd_us / 1000U, erase_us / 1000U, errors);
    test_trace_detail("SPEED fc mode=%s peak cmd/desc/buf=%hu/%hu/%hu sq/cq=%hu/%hu config=crc+random+bch", mode_s, peak.command_used, peak.descriptor_used, peak.buffer_used, peak.sq_used, peak.cq_used);
    test_trace_summary("SPEED fc: raw per-plane blocks %hu.. destroyed — run format + POR after FC-only runs", blocks[0]);

    munit_assert_uint32(errors, ==, 0U);
    return MUNIT_OK;
}

static const char        *s_fc_speed_modes[]  = {"normal", "cache", NULL};
static MunitParameterEnum s_fc_speed_params[] = {
    {(char *)"mode", s_fc_speed_modes},
    {          NULL,             NULL}
};

#endif /* UT_FC_ENABLE */

/* ══════════════════════════════════════════════════════════════════════
 * §3 FTL host API 速度
 * ══════════════════════════════════════════════════════════════════════ */

#if defined(UT_FTL_ENABLE)

#    include "ftl_op.h"
#    include "ftl_ut_common.h"
#    include "l2p_cache.h"
#    include "metadata_job.h"
#    include "ssd_compiler.h"
#    include "wbuf.h"

#    define SPD_FTL_BLOCKS           100U
#    define SPD_FTL_LBAS_PER_BLOCK   512U /* 1 gblock = 2 plane × 64 page × 4 CW × 512 B */
#    define SPD_FTL_LBAS             (SPD_FTL_BLOCKS * SPD_FTL_LBAS_PER_BLOCK)
#    define SPD_FTL_LBA_BASE         0U
#    define SPD_FTL_BYTES            ((DWORD)(SPD_FTL_LBAS * FTL_IU_BYTES))
#    define SPD_FTL_SLOTS            4U
#    define SPD_FTL_CHUNK_MAX        32U
#    define SPD_FTL_PHASE_TIMEOUT_MS 180000U
#    define SPD_FTL_GUARD            2000000U

typedef enum
{
    SPD_FTL_MODE_NORMAL = 0,
    SPD_FTL_MODE_CACHE,
} spd_ftl_mode_e;

typedef struct
{
    volatile BYTE         done;
    volatile ssd_status_e status;
    DWORD                 lba;
    DWORD                 count;
    BYTE                 *buf;
    bool                  used;
} spd_ftl_req_t;

typedef struct
{
    DWORD wb_batches;
    DWORD wb_pages;
    DWORD wb_declines;
    DWORD wb_aborts;
    DWORD rd_batches;
    DWORD rd_pairs;
    DWORD rd_declines;
    DWORD rd_fallbacks;
} spd_ftl_cache_t;

/* 4 个 slot 共享 16 KiB 竞技场：normal 档每个 slot 4 KiB（chunk=8 LBA），
 * cache 档只有 slot 0 能装下 16 KiB（chunk=32 LBA）⇒ 写深度自然为 1。
 * 放 SRAM4：AXI .bss 已满、SRAM12 被生产/测试表占满。 */
#    define SPD_FTL_ARENA_BYTES (16U * 1024U)
static BYTE          s_ftl_arena[SPD_FTL_ARENA_BYTES] SRAM4_DATA;
static spd_ftl_req_t s_ftl_req[SPD_FTL_SLOTS];
static bool          s_ftl_formatted;

static BYTE spd_ftl_pattern(DWORD byte_addr)
{
    return (BYTE)(((byte_addr * 131U) ^ (byte_addr >> 7U) ^ (byte_addr >> 15U)) & 0xFFU);
}

static void spd_ftl_fill(DWORD lba, BYTE *buf, DWORD count)
{
    const DWORD base  = lba * FTL_IU_BYTES;
    const DWORD bytes = count * FTL_IU_BYTES;

    for (DWORD i = 0U; i < bytes; i++)
    {
        buf[i] = spd_ftl_pattern(base + i);
    }
}

static bool spd_ftl_check(DWORD lba, const BYTE *buf, DWORD count)
{
    const DWORD base  = lba * FTL_IU_BYTES;
    const DWORD bytes = count * FTL_IU_BYTES;

    for (DWORD i = 0U; i < bytes; i++)
    {
        if (buf[i] != spd_ftl_pattern(base + i))
        {
            return false;
        }
    }
    return true;
}

static void spd_ftl_slots_reset(void)
{
    memset(s_ftl_req, 0, sizeof(s_ftl_req));
}

static spd_ftl_req_t *spd_ftl_slot_alloc(BYTE chunk, BYTE *out_index)
{
    const DWORD need = (DWORD)chunk * FTL_IU_BYTES;

    for (BYTE i = 0U; i < (BYTE)SPD_FTL_SLOTS; i++)
    {
        const DWORD offset = (DWORD)i * need;

        if ((!s_ftl_req[i].used) && ((offset + need) <= (DWORD)SPD_FTL_ARENA_BYTES))
        {
            s_ftl_req[i].used   = true;
            s_ftl_req[i].done   = 0U;
            s_ftl_req[i].status = SSD_STATUS_IO_ERROR;
            s_ftl_req[i].buf    = &s_ftl_arena[offset];
            *out_index          = i;
            return &s_ftl_req[i];
        }
    }
    return NULL;
}

static bool spd_ftl_submit(ftl_ctx_t *ftl, spd_ftl_req_t *slot, DWORD lba, DWORD count, bool write)
{
    ftl_io_req_t req;

    slot->lba   = lba;
    slot->count = count;

    memset(&req, 0, sizeof(req));
    req.lba           = lba;
    req.buffer        = slot->buf;
    req.count         = count;
    req.complete_flag = &slot->done;
    req.status_out    = &slot->status;

    const ftl_status_e status = write ? ftl_submit_write(ftl, &req) : ftl_submit_read(ftl, &req);

    if (status != FTL_OK)
    {
        slot->used = false;
        return false;
    }
    return true;
}

static void spd_ftl_cache_get(const ftl_ctx_t *ftl, spd_ftl_cache_t *out)
{
    memset(out, 0, sizeof(*out));
    out->wb_batches   = ftl->cache_wb_batches;
    out->wb_pages     = ftl->cache_wb_pages;
    out->wb_declines  = ftl->cache_wb_declines;
    out->wb_aborts    = ftl->cache_wb_aborts;
    out->rd_batches   = ftl->cache_read_batches;
    out->rd_pairs     = ftl->cache_read_pairs;
    out->rd_declines  = ftl->cache_read_declines;
    out->rd_fallbacks = ftl->cache_read_fallbacks;
}

static void spd_ftl_cache_diff(const spd_ftl_cache_t *a, const spd_ftl_cache_t *b, spd_ftl_cache_t *out)
{
    out->wb_batches   = b->wb_batches - a->wb_batches;
    out->wb_pages     = b->wb_pages - a->wb_pages;
    out->wb_declines  = b->wb_declines - a->wb_declines;
    out->wb_aborts    = b->wb_aborts - a->wb_aborts;
    out->rd_batches   = b->rd_batches - a->rd_batches;
    out->rd_pairs     = b->rd_pairs - a->rd_pairs;
    out->rd_declines  = b->rd_declines - a->rd_declines;
    out->rd_fallbacks = b->rd_fallbacks - a->rd_fallbacks;
}

/* 一个读写 phase：按 chunk 拆分、最多 depth 个请求在飞；完成后立即校验并回收 slot。
 * 时间含图案 fill / 校验 CPU（全量 25 MiB 比较，量级远小于 NAND）。 */
static bool spd_ftl_phase(ftl_ctx_t *ftl, bool write, DWORD chunk, BYTE depth, DWORD *ms_out, DWORD *errors)
{
    DWORD next     = 0U;
    BYTE  inflight = 0U;
    DWORD t0       = ftl_job_now_ms();
    DWORD guard    = 0U;

    spd_ftl_slots_reset();

    while ((next < (DWORD)SPD_FTL_LBAS) || (inflight != 0U))
    {
        while ((inflight < depth) && (next < (DWORD)SPD_FTL_LBAS))
        {
            BYTE           index = 0U;
            spd_ftl_req_t *slot  = spd_ftl_slot_alloc(chunk, &index);

            if (slot == NULL)
            {
                break;
            }

            if (write)
            {
                spd_ftl_fill(SPD_FTL_LBA_BASE + next, slot->buf, chunk);
            }
            if (!spd_ftl_submit(ftl, slot, SPD_FTL_LBA_BASE + next, chunk, write))
            {
                if (inflight == 0U)
                {
                    (*errors)++;
                    *ms_out = ftl_job_now_ms() - t0;
                    return false;
                }
                break;
            }

            next += chunk;
            inflight++;
        }

        ftl_service_pump();
        ftl_service(ftl);

        for (BYTE i = 0U; i < (BYTE)SPD_FTL_SLOTS; i++)
        {
            spd_ftl_req_t *slot = &s_ftl_req[i];

            if ((!slot->used) || (slot->done == 0U))
            {
                continue;
            }
            if (slot->status != SSD_STATUS_OK)
            {
                (*errors)++;
            }
            else if ((!write) && (!spd_ftl_check(slot->lba, slot->buf, slot->count)))
            {
                (*errors)++;
            }
            slot->used = false;
            if (inflight != 0U)
            {
                inflight--;
            }
        }

        if ((ftl_job_now_ms() - t0) > (DWORD)SPD_FTL_PHASE_TIMEOUT_MS)
        {
            (*errors)++;
            *ms_out = ftl_job_now_ms() - t0;
            return false;
        }
        if (++guard > (DWORD)SPD_FTL_GUARD)
        {
            (*errors)++;
            *ms_out = ftl_job_now_ms() - t0;
            return false;
        }
    }

    *ms_out = ftl_job_now_ms() - t0;
    return true;
}

static bool spd_ftl_wait_idle(ftl_ctx_t *ftl)
{
    const DWORD t0 = ftl_job_now_ms();

    for (;;)
    {
        fc_resource_stats_t stats;

        ftl_service_pump();
        ftl_service(ftl);

        memset(&stats, 0, sizeof(stats));
        (void)fc_get_resource_stats(&stats);

        if ((stats.sq_used == 0U) && (stats.hsq_used == 0U) && (stats.cq_used == 0U) && (stats.command_used == 0U) && (stats.descriptor_used == 0U) && (stats.buffer_used == 0U) && (ftl_op_ctx_count() == 0U) && (pend_slot_get_count(&ftl->pending) == 0U) && (pend_group_get_count(&ftl->pending) == 0U) && (!ftl->mig_handle->active) && (!ftl->mig_handle->waiting) && (!ftl->cp_handle->active) && (!ftl->cp_handle->waiting) && md_handle_is_idle(ftl->metadata_dir) && jrn_is_idle(ftl->journal) && wb_is_idle(ftl->wbuf) && (ftl->cache->load_inflight == 0U) && (ftl->cache->flush_inflight == 0U))
        {
            return true;
        }

        if ((ftl_job_now_ms() - t0) > 10000U)
        {
            return false;
        }
    }
}

static void *spd_ftl_setup(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    (void)ftl_ut_live();
    return NULL;
}

static void spd_ftl_teardown(void *fixture)
{
    (void)fixture;

    ftl_ut_assert_clean(ftl_ut_live());
}

/* 等某个 LBA 的 L2P 行就绪并读映射；用于窗口诊断（会推进 load）。 */
static bool spd_ftl_l2p_ready_read(ftl_ctx_t *ftl, DWORD lba, DWORD *out_pba)
{
    DWORD guard = 0U;

    for (;;)
    {
        const ftl_status_e st = l2p_read(ftl->cache, lba, out_pba);

        if (st != FTL_AGAIN)
        {
            return st == FTL_OK;
        }

        ftl_service_pump();
        ftl_service(ftl);

        if (++guard > 200000U)
        {
            return false;
        }
    }
}

/* cache read 失败时的窗口级诊断：打印 LBA base..base+15 的 PBA 与逐条判据，
 * 用来区分「物理布局不连续」与「cache_read_probe_pair 判据不符」。 */
static void spd_ftl_diag_window(ftl_ctx_t *ftl, DWORD base)
{
    DWORD pba[16];
    BYTE  invalid   = 0U;
    BYTE  wb_hit    = 0U;
    BYTE  plane_bad = 0U;
    BYTE  topo_bad  = 0U;

    for (BYTE k = 0U; k < 16U; k++)
    {
        const DWORD lba = base + k;

        pba[k] = NAND_PBA_INVALID;
        if (wb_has(ftl->wbuf, lba))
        {
            wb_hit++;
        }
        if ((!spd_ftl_l2p_ready_read(ftl, lba, &pba[k])) || (pba[k] == NAND_PBA_INVALID))
        {
            invalid++;
            continue;
        }
        if (pba_to_plane(pba[k]) != ftl_lba_plane(lba))
        {
            plane_bad++;
        }
        if ((pba_to_ch(pba[k]) != ftl->topo.ch) || (pba_to_ce(pba[k]) != ftl->topo.ce) || (pba_to_lun(pba[k]) != ftl->topo.lun))
        {
            topo_bad++;
        }
    }

    ftl_ut_trace_summary("DIAG ftl window=%lu invalid=%hhu wb=%hhu plane=%hhu topo=%hhu pba=%08lX,%08lX,%08lX,%08lX,%08lX,%08lX,%08lX,%08lX,%08lX,%08lX,%08lX,%08lX,%08lX,%08lX,%08lX,%08lX", base, invalid, wb_hit, plane_bad, topo_bad, pba[0], pba[1], pba[2], pba[3], pba[4], pba[5], pba[6], pba[7], pba[8], pba[9], pba[10], pba[11], pba[12], pba[13], pba[14], pba[15]);
}

static MunitResult test_ftl_speed_host(const MunitParameter params[], void *data)
{
    (void)data;

    const char *mode_s = munit_parameters_get(params, "mode");

    if ((mode_s == NULL) || ((strcmp(mode_s, "normal") != 0) && (strcmp(mode_s, "cache") != 0)))
    {
        munit_errorf("unknown speed mode '%s'", (mode_s != NULL) ? mode_s : "(null)");
    }

    const spd_ftl_mode_e mode = (strcmp(mode_s, "cache") == 0) ? SPD_FTL_MODE_CACHE : SPD_FTL_MODE_NORMAL;

    ftl_ut_trace_begin();

    ftl_ctx_t *ftl = ftl_ut_live();

    /* 首次运行格式化：确定性，并修复 FC 原始写留下的脏状态。format 时间单列。 */
    DWORD fmt_ms = 0U;

    if (!s_ftl_formatted)
    {
        const DWORD t0 = ftl_job_now_ms();

        ftl_ut_trace_suppress(true);
        ftl_ut_media_format();
        ftl_ut_trace_suppress(false);
        fmt_ms          = ftl_job_now_ms() - t0;
        s_ftl_formatted = true;
        ftl             = ftl_ut_live();
    }

    if (((QWORD)SPD_FTL_LBA_BASE + SPD_FTL_LBAS) > (QWORD)ssd_controller_host_lba_max(&g_ctrl))
    {
        munit_error("speed range exceeds host capacity");
    }

    const DWORD wchunk = (mode == SPD_FTL_MODE_CACHE) ? 32U : 8U;
    const BYTE  wdepth = (mode == SPD_FTL_MODE_CACHE) ? 1U : 4U;
    const BYTE  rdepth = 4U;
    const DWORD bytes  = (DWORD)SPD_FTL_BYTES;

    spd_ftl_cache_t c0;
    spd_ftl_cache_t c1;
    spd_ftl_cache_t cr0;
    spd_ftl_cache_t c2;
    spd_ftl_cache_t c3;
    spd_ftl_cache_t dw;
    spd_ftl_cache_t dr_cache;
    spd_ftl_cache_t dr_after_normal;

    DWORD errors       = 0U;
    DWORD wr_ms        = 0U;
    DWORD prep_ms      = 0U;
    DWORD rd_cache_ms  = 0U;
    DWORD rd_normal_ms = 0U;
    DWORD flush_ms     = 0U;

    spd_ftl_cache_get(ftl, &c0);
    bool ok = spd_ftl_phase(ftl, true, wchunk, wdepth, &wr_ms, &errors);

    if (ok)
    {
        const DWORD t0 = ftl_job_now_ms();

        if (!ftl_ut_flush())
        {
            errors++;
        }
        flush_ms = ftl_job_now_ms() - t0;
    }

    spd_ftl_cache_get(ftl, &c1);
    spd_ftl_cache_diff(&c0, &c1, &dw);

    /* cache 档：cache program 的写布局不保证 cache read 需要的物理连续 pair。
     * 读之前用 8-LBA 普通 pair 重写同一范围（不计时），让物理页严格按 LBA 顺序落盘，
     * 这样 32-LBA 读窗口才真正满足 cache_read_collect 的「同块、页号+1」。 */
    if (ok && (mode == SPD_FTL_MODE_CACHE))
    {
        /* 深度 1：让 pair 的物理页严格按 LBA 顺序编程（page+1），这是 cache read
         * 的判据；并发提交会让多个 pair 的编程顺序不确定。 */
        ok = spd_ftl_phase(ftl, true, 8U, 1U, &prep_ms, &errors);
        if (ok)
        {
            if (!ftl_ut_flush())
            {
                errors++;
            }
        }
    }

    spd_ftl_cache_get(ftl, &cr0);

    /* 读 1：cache 档用 32-LBA（cache read），normal 档用 8-LBA（普通读）。 */
    const DWORD rd_chunk = (mode == SPD_FTL_MODE_CACHE) ? 32U : 8U;

    if (ok)
    {
        ok = spd_ftl_phase(ftl, false, rd_chunk, rdepth, &rd_cache_ms, &errors);
    }

    spd_ftl_cache_get(ftl, &c2);
    spd_ftl_cache_diff(&cr0, &c2, &dr_cache);

    /* 读 2：同一份数据再用 8-LBA 普通读一遍，作为 normal 基线（cache 档专有）。 */
    if (ok && (mode == SPD_FTL_MODE_CACHE))
    {
        ok = spd_ftl_phase(ftl, false, 8U, rdepth, &rd_normal_ms, &errors);
    }
    else if (ok)
    {
        rd_normal_ms = rd_cache_ms;
    }

    spd_ftl_cache_get(ftl, &c3);
    spd_ftl_cache_diff(&c2, &c3, &dr_after_normal);

    if (!spd_ftl_wait_idle(ftl))
    {
        errors++;
    }

    const bool  rd_is_cache = (mode == SPD_FTL_MODE_CACHE) && (dr_cache.rd_batches > 0U);
    const DWORD rd_ms       = (mode == SPD_FTL_MODE_CACHE) ? (rd_is_cache ? rd_cache_ms : rd_normal_ms) : rd_cache_ms;
    const char *rd_path     = rd_is_cache ? "cache" : "normal";

    ftl_ut_trace_summary("SPEED ftl mode=%s  WR=%lu KB/s  RD=%lu KB/s  |  rd_path=%s rd_normal=%lu KB/s gblocks=%u lbas=%u bytes=%lu wr_ms=%lu prep_ms=%lu rd_ms=%lu rd_normal_ms=%lu fmt_ms=%lu flush_ms=%lu wb(b/d)=%lu/%lu rdc(b/d/f)=%lu/%lu/%lu rdn(b/d)=%lu/%lu err=%lu", mode_s, spd_kbps_from_us(bytes, wr_ms * 1000U), spd_kbps_from_us(bytes, rd_ms * 1000U), rd_path, spd_kbps_from_us(bytes, rd_normal_ms * 1000U), SPD_FTL_BLOCKS, SPD_FTL_LBAS, bytes, wr_ms, prep_ms, rd_ms, rd_normal_ms, fmt_ms, flush_ms, dw.wb_batches, dw.wb_declines, dr_cache.rd_batches, dr_cache.rd_declines, dr_cache.rd_fallbacks, dr_after_normal.rd_batches, dr_after_normal.rd_declines, errors);
    ftl_ut_trace_detail("SPEED ftl mode=%s wchunk=%lu wdepth=%hhu rdchunk=%lu rdepth=%hhu wb(b/p/d/a)=%lu/%lu/%lu/%lu rdc(b/p/d/f)=%lu/%lu/%lu/%lu", mode_s, wchunk, wdepth, rd_chunk, rdepth, dw.wb_batches, dw.wb_pages, dw.wb_declines, dw.wb_aborts, dr_cache.rd_batches, dr_cache.rd_pairs, dr_cache.rd_declines, dr_cache.rd_fallbacks);

    if (!ok)
    {
        munit_errorf("ftl speed phase did not drain (errors=%lu)", errors);
    }

    /* C8：cache 档必须证明「本次真的走了 cache 读」，否则打印窗口诊断并失败。 */
    if (mode == SPD_FTL_MODE_CACHE)
    {
        munit_assert_uint32(dw.wb_batches, >, 0U);
        if (dr_cache.rd_batches == 0U)
        {
            spd_ftl_diag_window(ftl, SPD_FTL_LBA_BASE);
            munit_errorf("cache read did not engage: batches=0 declines=%lu fallbacks=%lu", dr_cache.rd_declines, dr_cache.rd_fallbacks);
        }
    }
    else
    {
        munit_assert_uint32(dw.wb_batches, ==, 0U);
        munit_assert_uint32(dr_cache.rd_batches, ==, 0U);
    }

    munit_assert_uint32(errors, ==, 0U);
    return MUNIT_OK;
}

static const char        *s_ftl_speed_modes[]  = {"normal", "cache", NULL};
static MunitParameterEnum s_ftl_speed_params[] = {
    {(char *)"mode", s_ftl_speed_modes},
    {          NULL,              NULL}
};

#endif /* UT_FTL_ENABLE */

/* ══════════════════════════════════════════════════════════════════════
 * §4 套件注册：/speed/fc_nand_rw 与 /speed/ftl_rw
 * 两段实现由 UT_FC_ENABLE / UT_FTL_ENABLE 预处理选择；UT=（both）时两条并列。
 * ══════════════════════════════════════════════════════════════════════ */

#if defined(UT_FC_ENABLE) || defined(UT_FTL_ENABLE)
MunitTest g_speed_tests[] = {
#    if defined(UT_FC_ENABLE)
    {(char *)"fc_nand_rw",    test_fc_speed_mp,    test_setup,   test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION,  s_fc_speed_params},
#    endif
#    if defined(UT_FTL_ENABLE)
    {    (char *)"ftl_rw", test_ftl_speed_host, spd_ftl_setup, spd_ftl_teardown, MUNIT_TEST_OPTION_SINGLE_ITERATION, s_ftl_speed_params},
#    endif
    {                NULL,                NULL,          NULL,             NULL,             MUNIT_TEST_OPTION_NONE,               NULL},
};
#endif

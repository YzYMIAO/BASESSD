/*
 * test_speed.c — 板端速度测试（FC 裸块 / FTL host API）
 *
 * 两条用例：
 *   /speed/fc_nand_rw —— FC 公共接口异步读写 100 个块（两档：normal/cache）
 *   /speed/ftl_rw     —— FTL host API 读写 100 gblock（两档：normal/cache）
 *
 * 计时口径：
 *   · FC：每块用 SysTick µs 计时后累加（`delay_time_now_us` / `delay_time_since_us`）；
 *     擦除单独统计，不计入写速度。
 *   · FTL：用 platform_now_ms()（1 ms）计整个 phase；format / flush 单列，不计入 KB/s。
 *
 * 输出：**每个读写用例的速度结果行**（`SPEED fc mode=…` / `SPEED ftl chunk=…`）
 * 走 `test_result()` / `ftl_ut_result()` —— 不受 `--log-visible` 限制，默认档也打印
 * （速度是"结果"而不是诊断）。其余诊断（`SPEED err#n …` 等）仍在 `--log-visible info`
 * 下才打印 ⇒ 排查失败仍要带 `--log-visible info` 重跑。
 * KB/s 按 1 KB = 1024 B 计算。
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
#include "fc.h"
#include "l2p_cache.h"
#include "munit.h"
#include "ssd_types.h"
#include "timer.h"

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
    SPD_CLOCK_S
{
    DWORD start_us;
    DWORD start_ms;
    BOOL  time_valid;
} spd_clock_s;

static void spd_clock_start(spd_clock_s *clock)
{
    memset(clock, 0, sizeof(*clock));
    clock->time_valid = delay_time_now_us(&clock->start_us);
    if (!clock->time_valid)
    {
        clock->start_ms = platform_now_ms();
    }
}

static DWORD spd_clock_elapsed_us(const spd_clock_s *clock)
{
    if (clock->time_valid)
    {
        return delay_time_since_us(clock->start_us);
    }

    const DWORD now = platform_now_ms();

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

typedef enum SPD_FC_MODE_E
{
    SPD_FC_MODE_NORMAL = 0,
    SPD_FC_MODE_CACHE,
} spd_fc_mode_e;

typedef struct
    SPD_FC_SLOT_S
{
    fc_cmd_s *cmd; /* NULL = 空槽 */
    WORD      block;
    BYTE      page;
    BYTE      ch;
    BYTE      ce;
    BYTE      lun;
    BOOL      verify; /* true = 读命令，完成前要逐 desc 校验 */
} spd_fc_slot_s;

static spd_fc_slot_s s_fc_slots[FC_COMMAND_POOL_DEPTH];
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

static fc_cmd_s *spd_fc_try_build(spd_fc_mode_e mode, BOOL write, WORD block, BYTE page, BYTE ch, BYTE ce, BYTE lun)
{
    nand_addr_s  addr;
    fc_cmd_cfg_s conf;
    fc_cmd_s    *cmd;

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

static spd_fc_slot_s *spd_fc_slot_alloc(void)
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

static spd_fc_slot_s *spd_fc_slot_find(WORD command_id)
{
    for (WORD i = 0U; i < (WORD)FC_COMMAND_POOL_DEPTH; i++)
    {
        fc_cmd_id_t id = FC_CMD_ID_INVALID;

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

static void spd_fc_note_peak(fc_resource_stats_s *peak)
{
    fc_resource_stats_s now;

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
static DWORD spd_fc_verify_read(const spd_fc_slot_s *slot)
{
    DWORD        errors = 0U;
    BYTE         idx    = 0U;
    fc_desc_id_t id     = (fc_desc_id_t)slot->cmd->desc_id;

    while ((id != FC_DESC_ID_INVALID) && (idx < CW_TOTAL_MAX))
    {
        const fc_desc_s *desc = fc_descriptor_id_to_descriptor(id);

        if (desc == NULL)
        {
            errors++;
            break;
        }

        nand_addr_s addr;

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

        id = (fc_desc_id_t)desc->next_id;
        idx++;
    }

    if (idx != (BYTE)(PLANE_NUM * CW_NUM))
    {
        errors++; /* 链长不对：没拿到预期 8 个 CW */
    }
    return errors;
}

static WORD spd_fc_reap(DWORD *errors, fc_resource_stats_s *peak)
{
    fc_cmd_status_s buf[SPD_FC_REAP_MAX];
    WORD            n = 0U;

    (void)fc_poll_completions(buf, (WORD)SPD_FC_REAP_MAX, &n);

    for (WORD i = 0U; i < n; i++)
    {
        spd_fc_slot_s *slot = spd_fc_slot_find((WORD)buf[i].command_id);

        if (slot == NULL)
        {
            (*errors)++;
            continue;
        }

        if ((buf[i].status & FC_CMD_STATUS_ERROR_MASK) != 0U)
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

static BOOL spd_fc_drain(DWORD *errors, fc_resource_stats_s *peak)
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

/* 一个 phase：block → die → page。每块结束 drain 到在途归零：
 *   · cache 序列边界明确（FIRST..END 不跨块）；
 *   · 本块计时区间远小于 32 位 µs 回绕周期（~71.6 min），差值比较安全。
 * 池上限约 16 条 multi-plane 页命令（8 desc + 8 buf/条），块内仍会把在途顶到上限。 */
static BOOL spd_fc_run_phase(spd_fc_mode_e mode, BOOL write, const WORD blocks[SPD_FC_BLOCKS], DWORD *us_out, DWORD *errors, fc_resource_stats_s *peak)
{
    DWORD total_us = 0U;

    spd_fc_slots_reset();

    for (WORD b = 0U; b < SPD_FC_BLOCKS; b++)
    {
        spd_clock_s clock;
        WORD        since_reap = 0U;
        DWORD       guard      = 0U;
        BOOL        ok         = true;

        spd_clock_start(&clock);

        for (BYTE die = 0U; (die < DIE_MAX) && ok; die++)
        {
            const BYTE ch  = die_2_ch(die);
            const BYTE ce  = die_2_ce(die);
            const BYTE lun = die_2_lun(die);

            for (BYTE page = 0U; (page < (BYTE)PAGES_PER_BLOCK) && ok; page++)
            {
                for (;;)
                {
                    spd_fc_slot_s *slot = spd_fc_slot_alloc();
                    fc_cmd_s      *cmd;

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

static BOOL spd_fc_pair_is_good(const ftl_ctx_s *ftl, WORD block)
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
    const ftl_ctx_s *ftl = g_ctrl.ftl[0];
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
    fc_cmd_cfg_s conf;
    DWORD        total_us = 0U;

    memset(&conf, 0, sizeof(conf));
    conf.operation = (DWORD)OP_ERASE_BLOCK_MULTI_PLANE;
    conf.crc       = FC_CRC_ENABLE;
    conf.random    = FC_RANDOM_ENABLE;
    conf.ldpc      = FC_ECC_MODE_BCH;
    conf.raid      = FC_RAID_MODE_NORMAL;

    for (WORD i = 0U; i < SPD_FC_BLOCKS; i++)
    {
        spd_clock_s     clock;
        fc_cmd_status_s status;

        spd_clock_start(&clock);
        status = test_erase_block(blocks[i], TEST_PLANE_BM_ALL, &conf);
        total_us += spd_clock_elapsed_us(&clock);

        if ((status.status & FC_CMD_STATUS_ERROR_MASK) != 0U)
        {
            (*errors)++;
        }
    }
    *us_out = total_us;
}

static void spd_fc_assert_pools_empty(void)
{
    fc_resource_stats_s stats;

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

    ftl_ctx_s *ftl           = g_ctrl.ftl[0];
    const BOOL saved_suspend = (ftl != NULL) ? ftl->bg_suspend : false;

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
    fc_resource_stats_s peak;

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

    const DWORD bytes   = (DWORD)SPD_FC_BYTES;
    const DWORD wr_kbps = spd_kbps_from_us(bytes, wr_us);
    const DWORD rd_kbps = spd_kbps_from_us(bytes, rd_us);

    /* 结果行**只留速度**（默认档可见）；原始计时 / 数据量 / 校验计数属于诊断，
     * 放到 gated 的 summary/detail 行（`--log-visible info/debug` 才打印）。 */
    test_result("SPEED fc mode=%s  WR=%lu KB/s  RD=%lu KB/s", mode_s, wr_kbps, rd_kbps);
    test_trace_summary("SPEED fc mode=%s detail: blocks=%u bytes=%lu wr_ms=%lu rd_ms=%lu erase_ms=%lu err=%lu", mode_s, SPD_FC_BLOCKS, bytes, wr_us / 1000U, rd_us / 1000U, erase_us / 1000U, errors);
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

typedef struct
    SPD_FTL_REQ_S
{
    volatile BYTE         done;
    volatile ssd_status_e status;
    DWORD                 lba;
    DWORD                 count;
    BYTE                 *buf;
    BOOL                  used;
} spd_ftl_req_s;

typedef struct
    SPD_FTL_CACHE_S
{
    DWORD wb_batches;
    DWORD wb_pages;
    DWORD wb_declines;
    DWORD wb_aborts;
    DWORD rd_batches;
    DWORD rd_pairs;
    DWORD rd_declines;
    DWORD rd_fallbacks;
} spd_ftl_cache_s;

/* 4 个 slot 共享 16 KiB 竞技场：normal 档每个 slot 4 KiB（chunk=8 LBA），
 * cache 档只有 slot 0 能装下 16 KiB（chunk=32 LBA）⇒ 写深度自然为 1。
 * 放 SRAM4：AXI .bss 已满、SRAM12 被生产/测试表占满。 */
#    define SPD_FTL_ARENA_BYTES (16U * 1024U)
static BYTE          s_ftl_arena[SPD_FTL_ARENA_BYTES] SRAM4_DATA;
static spd_ftl_req_s s_ftl_req[SPD_FTL_SLOTS];
static BOOL          s_ftl_formatted;

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

/* 首个不匹配字节的偏移；全等时返回 count*FTL_IU_BYTES。
 * 只用于失败时的定位打印：错在哪个 LBA、偏移多少、期望/实际各是什么。 */
static DWORD spd_ftl_first_mismatch(DWORD lba, const BYTE *buf, DWORD count)
{
    const DWORD base  = lba * FTL_IU_BYTES;
    const DWORD bytes = count * FTL_IU_BYTES;

    for (DWORD i = 0U; i < bytes; i++)
    {
        if (buf[i] != spd_ftl_pattern(base + i))
        {
            return i;
        }
    }
    return bytes;
}

/* 每次 phase 最多打 8 条错误明细，用来区分「状态错误」与「数据错」，
 * 并把错点收敛到具体 LBA/偏移。
 * ⚠ 走 `[EV]`（`--log-visible info` 才可见）——默认档只保留用例名 + 结果 + 耗时，
 *   与 munit 三层输出约定一致。 */
static DWORD s_spd_err_prints;

static void spd_ftl_note_status_error(BOOL write, DWORD chunk, const spd_ftl_req_s *slot)
{
    if (s_spd_err_prints >= 8U)
    {
        return;
    }
    s_spd_err_prints++;
    ftl_ut_trace_summary("SPEED err#%lu %s%lu lba=%lu count=%lu STATUS=%d",
                         s_spd_err_prints,
                         (write ? "wr" : "rd"),
                         chunk,
                         slot->lba,
                         slot->count,
                         (int)slot->status);
}

static void spd_ftl_note_data_error(BOOL write, DWORD chunk, const spd_ftl_req_s *slot, DWORD offset)
{
    if (s_spd_err_prints >= 8U)
    {
        return;
    }
    s_spd_err_prints++;

    const DWORD byte_addr = (slot->lba * FTL_IU_BYTES) + offset;
    const BYTE  expected  = spd_ftl_pattern(byte_addr);

    ftl_ut_trace_summary("SPEED err#%lu %s%lu lba=%lu off=%lu got=0x%02hhX exp=0x%02hhX",
                         s_spd_err_prints,
                         (write ? "wr" : "rd"),
                         chunk,
                         slot->lba,
                         offset,
                         slot->buf[offset],
                         expected);

    /* 诊断（RISK-CACHE-26）：当场问一次 L2P——映射丢了（INVALID）还是映射在但页内容为 0。
     * 只在失败路径上执行；`l2p_read` 可能触发一次加载，属可接受的代价。 */
    {
        ftl_ctx_s *ftl = g_ctrl.ftl[0];

        if ((ftl != NULL) && (ftl->cache != NULL))
        {
            DWORD              pba = NAND_PBA_INVALID;
            const DWORD        lba = slot->lba + (offset / FTL_IU_BYTES);
            const ftl_status_e st  = l2p_read(ftl->cache, lba, &pba);

            ftl_ut_trace_summary("SPEED err#%lu L2P view lba=%lu -> st=%d pba=%08lX (INVALID=0x%08lX)", s_spd_err_prints, lba, (int)st, pba, (DWORD)NAND_PBA_INVALID);
        }
    }
}

static void spd_ftl_slots_reset(void)
{
    memset(s_ftl_req, 0, sizeof(s_ftl_req));
}

static spd_ftl_req_s *spd_ftl_slot_alloc(BYTE chunk, BYTE *out_index)
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

static BOOL spd_ftl_submit(ftl_ctx_s *ftl, spd_ftl_req_s *slot, DWORD lba, DWORD count, BOOL write)
{
    ftl_io_req_s req;

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

static void spd_ftl_cache_get(const ftl_ctx_s *ftl, spd_ftl_cache_s *out)
{
    memset(out, 0, sizeof(*out));
    out->wb_batches   = ftl->cache_stream.wb_batches;
    out->wb_pages     = ftl->cache_stream.wb_pages;
    out->wb_declines  = ftl->cache_stream.wb_declines;
    out->wb_aborts    = ftl->cache_stream.wb_aborts;
    out->rd_batches   = ftl->cache_stream.read_batches;
    out->rd_pairs     = ftl->cache_stream.read_pairs;
    out->rd_declines  = ftl->cache_stream.read_declines;
    out->rd_fallbacks = ftl->cache_stream.read_fallbacks;
}

static void spd_ftl_cache_diff(const spd_ftl_cache_s *a, const spd_ftl_cache_s *b, spd_ftl_cache_s *out)
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
static BOOL spd_ftl_phase(ftl_ctx_s *ftl, BOOL write, DWORD chunk, BYTE depth, DWORD *ms_out, DWORD *errors)
{
    DWORD next     = 0U;
    BYTE  inflight = 0U;
    DWORD t0       = platform_now_ms();
    DWORD guard    = 0U;

    spd_ftl_slots_reset();
    s_spd_err_prints = 0U;

    while ((next < (DWORD)SPD_FTL_LBAS) || (inflight != 0U))
    {
        while ((inflight < depth) && (next < (DWORD)SPD_FTL_LBAS))
        {
            BYTE           index = 0U;
            spd_ftl_req_s *slot  = spd_ftl_slot_alloc(chunk, &index);

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
                    *ms_out = platform_now_ms() - t0;
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
            spd_ftl_req_s *slot = &s_ftl_req[i];

            if ((!slot->used) || (slot->done == 0U))
            {
                continue;
            }
            if (slot->status != SSD_STATUS_OK)
            {
                (*errors)++;
                spd_ftl_note_status_error(write, chunk, slot);
            }
            else if (!write)
            {
                const DWORD offset = spd_ftl_first_mismatch(slot->lba, slot->buf, slot->count);

                if (offset != (slot->count * FTL_IU_BYTES))
                {
                    (*errors)++;
                    spd_ftl_note_data_error(write, chunk, slot, offset);
                }
            }
            slot->used = false;
            if (inflight != 0U)
            {
                inflight--;
            }
        }

        if ((platform_now_ms() - t0) > (DWORD)SPD_FTL_PHASE_TIMEOUT_MS)
        {
            (*errors)++;
            *ms_out = platform_now_ms() - t0;
            return false;
        }
        if (++guard > (DWORD)SPD_FTL_GUARD)
        {
            (*errors)++;
            *ms_out = platform_now_ms() - t0;
            return false;
        }
    }

    *ms_out = platform_now_ms() - t0;
    return true;
}

static BOOL spd_ftl_wait_idle(ftl_ctx_s *ftl)
{
    const DWORD      t0 = platform_now_ms();
    fc_debug_state_s dbg0;

    memset(&dbg0, 0, sizeof(dbg0));
    (void)fc_debug_state_get(&dbg0);

    for (;;)
    {
        fc_resource_stats_s stats;

        ftl_service_pump();
        ftl_service(ftl);

        memset(&stats, 0, sizeof(stats));
        (void)fc_get_resource_stats(&stats);

        if ((stats.sq_used == 0U) && (stats.hsq_used == 0U) && (stats.cq_used == 0U) && (stats.command_used == 0U) && (stats.descriptor_used == 0U) && (stats.buffer_used == 0U) && (ftl_op_req_count() == 0U) && (pend_slot_get_count(&ftl->pending) == 0U) && (pend_group_get_count(&ftl->pending) == 0U) && (!ftl->mig_handle->active) && (!ftl->mig_handle->waiting) && (!ftl->cp_handle->active) && (!ftl->cp_handle->waiting) && md_handle_is_idle(ftl->metadata_dir) && jrn_is_idle(ftl->journal) && wb_is_idle(ftl->wbuf) && (ftl->cache->load_inflight == 0U) && (ftl->cache->flush_inflight == 0U))
        {
            return true;
        }

        if ((platform_now_ms() - t0) > 10000U)
        {
            // 排障：把「谁卡住」+ 本次等待期间填充 pass 的**增量**打出来。
            // 增量比绝对值有用：fill 不涨 ⇒ 填充没被调用；pops 不涨 ⇒ 扫描没看到命令。
            fc_debug_state_s dbg;

            memset(&dbg, 0, sizeof(dbg));
            (void)fc_debug_state_get(&dbg);
            ftl_ut_trace_summary("SPEED idle timeout: sq=%hu hsq=%hu owner=%hhu lun_used=%hhu lun_phase=%hhu fill_delta=%lu/%lu sq_head=%hu st=%hhu chain=%hu op=%hhu sched_sq=%hu bound=%hhu hsq_head=%hu chain=%hu op=%hhu req=%hu",
                                 dbg.sq_used,
                                 dbg.hsq_used,
                                 (unsigned)dbg.chain_owner,
                                 dbg.lun_used,
                                 dbg.lun_phase,
                                 dbg.fill_calls - dbg0.fill_calls,
                                 dbg.fill_pops - dbg0.fill_pops,
                                 dbg.sq_head_id,
                                 dbg.sq_head_state,
                                 dbg.sq_head_chain,
                                 (unsigned)dbg.sq_head_op,
                                 dbg.sched_sq_count,
                                 dbg.sched_sq_bound,
                                 dbg.hsq_head_id,
                                 dbg.hsq_head_chain,
                                 (unsigned)dbg.hsq_head_op,
                                 ftl_op_req_count());
            return false;
        }
    }
}

static void *spd_ftl_setup(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    ftl_ut_topology_init();
    (void)ftl_ut_live();
    return NULL;
}

static void spd_ftl_teardown(void *fixture)
{
    (void)fixture;

    ftl_ut_assert_clean(ftl_ut_live());
}

/* 等某个 LBA 的 L2P 行就绪并读映射；用于窗口诊断（会推进 load）。 */
static BOOL spd_ftl_l2p_ready_read(ftl_ctx_s *ftl, DWORD lba, DWORD *out_pba)
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
static void spd_ftl_diag_window(ftl_ctx_s *ftl, DWORD base)
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

/* ftl_rw — FTL host API 的 25 MiB 顺序读写速度（/speed/ftl_rw）。
 *
 * 定位（与 /speed/fc_nand_rw 的分工）：
 *   · FC 套件**直接选**器件读写方式（normal / cache）⇒ 给出速度的**上下限**；
 *   · 本套件只规定**主机请求形态**（chunk × 深度），走哪条内部路径由 FTL **自适应**
 *     决定 —— 不强制、不因路径不同判失败；结果里同时报告实际路径的计数。
 *
 * 参数：`chunk`=8|16|32 LBA/请求（槽缓冲 16 KiB ⇒ 深度 4/2/1，见下的 depth 推导）。
 * 流程：首组合完整 format（时间单列）→ 写 25 MiB（计时）→ flush（计时）→ 读 25 MiB（计时）。
 *       非首组合先 reboot 从介质重建（清 RAM 控制面，介质内容保留）。
 * 判据：**只判「跑不跑得完」**——phase 超时 ⇒ FAIL。写入返回 IO_ERROR 属背压、读阶段
 *       数据不符属数据问题，两者都计入 `err(wr/ rd)` **上报**，不单独判失败：本套件的
 *       产出是速度数字，正确性由功能套件（FC/FTL pure/media）负责。
 * 备注：破坏性，跑 LBA 0..51199；数字只在**同一 chunk/深度**下可比。
 */
static MunitResult test_ftl_speed_host(const MunitParameter params[], void *data)
{
    (void)data;

    const char *chunk_s = munit_parameters_get(params, "chunk");
    DWORD       chunk   = 0U;

    if ((chunk_s != NULL) && (strcmp(chunk_s, "8") == 0))
    {
        chunk = 8U;
    }
    else if ((chunk_s != NULL) && (strcmp(chunk_s, "16") == 0))
    {
        chunk = 16U;
    }
    else if ((chunk_s != NULL) && (strcmp(chunk_s, "32") == 0))
    {
        chunk = 32U;
    }
    else
    {
        munit_errorf("unknown chunk '%s'", (chunk_s != NULL) ? chunk_s : "(null)");
    }

    /* 槽缓冲 16 KiB（SRAM4）⇒ chunk × depth ≤ 16 KiB：8×4 / 16×2 / 32×1。
     * 深度是**测试夹具的约束**，不是对 FTL 路径的要求。 */
    const BYTE depth = (BYTE)((DWORD)SPD_FTL_ARENA_BYTES / (chunk * FTL_IU_BYTES));

    ftl_ut_trace_begin();

    ftl_ctx_s *ftl = ftl_ut_live();

    /* 首组合完整 format：确定性，并修复 FC 原始写留下的脏状态。format 时间单列。 */
    DWORD fmt_ms = 0U;

    if (!s_ftl_formatted)
    {
        const DWORD t0 = platform_now_ms();

        ftl_ut_trace_suppress(true);
        ftl_ut_media_format();
        ftl_ut_trace_suppress(false);
        fmt_ms          = platform_now_ms() - t0;
        s_ftl_formatted = true;
        ftl             = ftl_ut_live();
    }
    else
    {
        /* 非首组合：清 RAM 控制面（quiesce + fc_init + 重新武装 + pfr 恢复），介质保留。 */
        ftl_ut_trace_suppress(true);
        ftl_ut_reboot_from_media();
        ftl_ut_trace_suppress(false);
        ftl = ftl_ut_live();
        ftl_ut_trace_summary("SPEED isolation: reboot from media before chunk=%lu", chunk);
    }

    if (((QWORD)SPD_FTL_LBA_BASE + SPD_FTL_LBAS) > (QWORD)ssd_controller_host_lba_max(&g_ctrl))
    {
        munit_error("speed range exceeds host capacity");
    }

    spd_ftl_cache_s c0;
    spd_ftl_cache_s c1;
    spd_ftl_cache_s cr0;
    spd_ftl_cache_s c2;
    spd_ftl_cache_s dw;
    spd_ftl_cache_s dr;
    DWORD           errors   = 0U;
    DWORD           err_wr   = 0U;
    DWORD           err_rd   = 0U;
    DWORD           wr_ms    = 0U;
    DWORD           flush_ms = 0U;
    DWORD           rd_ms    = 0U;

    spd_ftl_cache_get(ftl, &c0);
    BOOL ok = spd_ftl_phase(ftl, true, chunk, depth, &wr_ms, &errors);

    if (ok)
    {
        const DWORD t0 = platform_now_ms();

        if (!ftl_ut_flush())
        {
            errors++;
        }
        flush_ms = platform_now_ms() - t0;
    }

    err_wr = errors;
    spd_ftl_cache_get(ftl, &c1);
    spd_ftl_cache_diff(&c0, &c1, &dw);

    spd_ftl_cache_get(ftl, &cr0);

    if (ok)
    {
        ok = spd_ftl_phase(ftl, false, chunk, depth, &rd_ms, &errors);
    }

    err_rd = errors - err_wr;
    spd_ftl_cache_get(ftl, &c2);
    spd_ftl_cache_diff(&cr0, &c2, &dr);

    if (!spd_ftl_wait_idle(ftl))
    {
        errors++;
    }

    const DWORD bytes       = (DWORD)SPD_FTL_BYTES;
    const BOOL  rd_is_cache = (dr.rd_batches > 0U);

    if ((rd_is_cache == false) && ok)
    {
        spd_ftl_diag_window(ftl, SPD_FTL_LBA_BASE); /* 证据：为什么没走 cache 读 */
    }

    ftl_ut_trace_detail("SPEED ftl chunk=%lu depth=%hhu wb(b/p/d/a)=%lu/%lu/%lu/%lu rd(b/p/d/f)=%lu/%lu/%lu/%lu",
                        chunk,
                        depth,
                        dw.wb_batches,
                        dw.wb_pages,
                        dw.wb_declines,
                        dw.wb_aborts,
                        dr.rd_batches,
                        dr.rd_pairs,
                        dr.rd_declines,
                        dr.rd_fallbacks);
    /* 结果行**只留速度**（默认档可见）；路径 / 原始计时 / 校验计数属于诊断，
     * 放到 gated 的 summary（`--log-visible info/debug` 才打印）。 */
    ftl_ut_result("SPEED ftl chunk=%lu  WR=%lu KB/s  RD=%lu KB/s", chunk, spd_kbps_from_us(bytes, wr_ms * 1000U), spd_kbps_from_us(bytes, rd_ms * 1000U));
    ftl_ut_trace_summary("SPEED ftl chunk=%lu detail: depth=%hhu rd_path=%s wb(b/d)=%lu/%lu rd(b/d/f)=%lu/%lu/%lu wr_ms=%lu flush_ms=%lu rd_ms=%lu fmt_ms=%lu err(wr=%lu rd=%lu)",
                         chunk,
                         depth,
                         (rd_is_cache == true) ? "cache" : "normal",
                         dw.wb_batches,
                         dw.wb_declines,
                         dr.rd_batches,
                         dr.rd_declines,
                         dr.rd_fallbacks,
                         wr_ms,
                         flush_ms,
                         rd_ms,
                         fmt_ms,
                         err_wr,
                         err_rd);

    /* 判据**只判「跑不跑得完」**：失败明细与背压计数都已在上面一行给出。 */
    if (!ok)
    {
        ftl_ut_trace_summary("SPEED ftl chunk=%lu depth=%hhu: phase did not drain (err 总=%lu)", chunk, depth, errors);
        return MUNIT_FAIL;
    }

    return MUNIT_OK;
}

/* chunk = 每请求 LBA 数；深度由夹具的槽缓冲推导（8→4 / 16→2 / 32→1）。 */
static const char        *s_ftl_speed_chunks[] = {"8", "16", "32", NULL};
static MunitParameterEnum s_ftl_speed_params[] = {
    {(char *)"chunk", s_ftl_speed_chunks},
    {           NULL,               NULL}
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

/*
 * ftl_test_common.c — FTL 层测试共享夹具 + L0 异步链伪完成注入 seam
 *
 * 内容：
 *   §1 活实例夹具（ftl_fixture / ftl_fixture_reset —— test_ftl.c §1 原样搬移）
 *   §2 异步专用复位（ftl_async_fixture_reset：基础复位 + 零化异步 job 结构）
 *   §3 伪完成注入（ut_fake_complete / ut_complete_and_drain / ut_sq_head）
 *
 * 设计要点（伪完成注入）：
 *   - g_fc_controller 是 extern（fc_internal.h:85），SQ/HSQ/CQ 字段公开（fc_queue.h）；
 *   - 完成 = `fc_completion_queue_push(cq, &completion)` + 手动置
 *     `command_state[id] = FC_COMMAND_STATE_COMPLETED`（与 fc.c 的 fc_complete_internal
 *     逐字同构，fc.c:713-725）；
 *   - ⚠ 必须先摘 SQ/HSQ（否则 superloop 会把 stale QUEUED 派给真 NAND，debug 断言爆）；
 *   - ⚠ 只接受「ftl_op_submit 已返 FTL_OK」的命令。
 */

#include "ftl_test_common.h"

#include "allocator.h"  /* ftl_allocator_t（含 bbt/wear 池游标） */
#include "checkpoint.h" /* cp_job_t */
#include "cli.h"        /* g_ctrl（活实例路由表，extern） */
#include "completion_table.h"
#include "fc_interface.h"     /* fc_command_id_to_command / fc_descriptor_id_to_descriptor */
#include "fc_internal.h"      /* g_fc_controller / fc_t / FC_COMMAND_STATE_* */
#include "ftl_fc_interface.h" /* ftl_cmd_id */
#include "journal.h"
#include "l2p_cache.h"
#include "metadata.h"
#include "migrate.h" /* mig_job_t */
#include "read_recovery.h"

#include <string.h>

/* ══════════════════════════════════════════════════════════════════════
 * §1  活实例夹具
 * ══════════════════════════════════════════════════════════════════════ */

ftl_context_t *ftl_fixture(void)
{
    ftl_context_t *F = g_ctrl.ftl[0][0][0];

    munit_assert_not_null(F);
    return F;
}

/*
 * ftl_fixture_reset — 把活实例复位到「开机初始、无介质内容」的 RAM 状态。
 *
 * 范围 = §2.2 表：只清会被用例写的子结构；**F->topo 保持不动**（reset 不该改路由）。
 * 顺序有依赖：先 memset 再 init（init 只建固定表/游标，不 memset 整表）。
 */
void ftl_fixture_reset(void)
{
    ftl_context_t *F = ftl_fixture();

    memset(&F->tables, 0, sizeof(F->tables));
    memset(&F->metadata_dir, 0, sizeof(F->metadata_dir));
    memset(F->pending, 0, sizeof(F->pending));
    memset(F->groups, 0, sizeof(F->groups));
    memset(&F->wbuf, 0, sizeof(F->wbuf));
    memset(&F->cp, 0, sizeof(F->cp));
    memset(&F->rr, 0, sizeof(F->rr));
    memset(F->rr_page_scratch, 0, sizeof(F->rr_page_scratch));

    F->cp_timer_start  = 0U;
    F->total_writes    = 0U;
    F->rr_last_scan_ms = 0U;
    F->bg_suspend      = 0U;

    /* 分配器（绑表指针 + 全块置 FREE/无归属；**不回填 Free Pool**，见 §2 行 5） */
    (void)allocator_init(&F->alloc, F->topo, &F->tables);

    /* 元数据目录：fill_fixed 用 dir->topo.lun 展开固定区地址 ⇒ 必须先绑 topo */
    F->metadata_dir.topo = F->topo;
    metadata_dir_init(&F->metadata_dir);

    /* L2P cache（不碰 md_dir / alloc 绑定，见 l2p_cache_init 头注） */
    l2p_cache_init(&F->cache, F->topo.ch, F->topo.ce, F->topo.lun);

    /* Journal（复位游标 + block_pba 展开 + page_buf 0xFF） */
    journal_init(&F->journal, F->topo);
}

/* ══════════════════════════════════════════════════════════════════════
 * §2  异步专用复位
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * ftl_async_fixture_reset — 异步套件专用复位。
 *
 * 在 ftl_fixture_reset() 之上**额外**零化异步 job 结构：
 *   · F->alloc.bbt / F->alloc.wear —— 池游标（sequence/write_idx/cursor_valid/dirty/
 *     脏位图）+ job（phase/waiting/节拍）——fixture 的 allocator_init 不 memset 它们；
 *   · F->mig / F->cp_job —— 搬移 / checkpoint job（fixture 只 memset 了 F->cp，没碰 cp_job）。
 * （rr / metadata_dir.job 已被 ftl_fixture_reset 的 memset 覆盖；journal job 由
 *   journal_init 建好，phase=JRN_IDLE，不需要再动。）
 *
 * 各 job 前置项（如 bbt 的 cursor_valid=1、cache 行预置等）由具体用例按需显式设。
 */
void ftl_async_fixture_reset(void)
{
    ftl_context_t *F = ftl_fixture();

    ftl_fixture_reset();

    memset(&F->alloc.bbt, 0, sizeof(F->alloc.bbt));
    memset(&F->alloc.wear, 0, sizeof(F->alloc.wear));
    memset(&F->mig, 0, sizeof(F->mig));
    memset(&F->cp_job, 0, sizeof(F->cp_job));
}

/* setup 一律返回 NULL：夹具是**活实例**，不是 malloc 的私有块（§2.2）。
 * 返回 NULL 是合法契约（测试体用 ftl_fixture() 自己取指针）。 */
void *ftl_async_setup(const MunitParameter params[], void *user_data)
{
    (void)params;
    (void)user_data;

    ftl_async_fixture_reset();
    return NULL;
}

/* ══════════════════════════════════════════════════════════════════════
 * §3  伪完成注入 seam
 * ══════════════════════════════════════════════════════════════════════ */

/* 从环形队列摘除指定 command_id（左移压缩），找到返回 1。 */
static uint8_t ut_fake_q_remove(fc_submission_queue_t *q, uint16_t cmd_id)
{
    uint16_t i;
    uint16_t k;

    for (i = 0U; i < q->count; i++)
    {
        if (q->entries[(q->head + i) % q->depth] == cmd_id)
        {
            break;
        }
    }
    if (i == q->count)
    {
        return 0U; /* 不在本队列 */
    }
    for (k = i; k + 1U < q->count; k++)
    {
        q->entries[(q->head + k) % q->depth] = q->entries[(q->head + k + 1U) % q->depth];
    }
    q->tail = (uint16_t)((q->tail + q->depth - 1U) % q->depth);
    q->count--;
    return 1U;
}

void ut_fake_complete(fc_command_t *cmd, uint8_t status)
{
    fc_command_status_t completion;
    fc_command_id_t     cmd_id;
    fc_descriptor_id_t  d;

    munit_assert_not_null(cmd);
    cmd_id = (fc_command_id_t)ftl_cmd_id(cmd);

    /* ① 摘 SQ/HSQ：命令只可能在这两个之一（high_priority 决定） */
    if (ut_fake_q_remove(&g_fc_controller.sq, cmd_id) == 0U)
    {
        (void)ut_fake_q_remove(&g_fc_controller.hsq, cmd_id);
    }

    /* ② 沿 desc 链写回 per-CW status（保真非承载：全仓 src/ftl 层无人读 desc->status，
     *    成败判据一律用 completion->status） */
    if (cmd->desc_id != FC_DESCRIPTOR_ID_INVALID)
    {
        d = (fc_descriptor_id_t)cmd->desc_id;
        while (d != FC_DESCRIPTOR_ID_INVALID)
        {
            fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(d);

            desc->status   = status;
            desc->ecc_bits = 0U;
            d              = (fc_descriptor_id_t)desc->next_id;
        }
    }

    /* ③ 构 CQ 完成项 → CQ push + 置 COMPLETED（与 fc.c fc_complete_internal 同构）。
     *    CQ 满：强制 COMPLETED 防命令泄漏（entry 丢失是 fatal，与生产一致）。 */
    memset(&completion, 0, sizeof(completion));
    completion.command_id = cmd_id;
    completion.desc_id    = FC_DESCRIPTOR_ID_INVALID; /* 成功：无失败 CW */
    completion.op_tag     = (DWORD)cmd->op_tag;       /* 派发键：g_op_service[op_tag] */
    completion.status     = status;
    if (fc_completion_queue_push(&g_fc_controller.cq, &completion) != FC_STATUS_OK)
    {
        g_fc_controller.command_state[cmd_id] = FC_COMMAND_STATE_COMPLETED;
    }
}

void ut_complete_and_drain(fc_command_t *cmd, uint8_t status)
{
    ut_fake_complete(cmd, status);
    completion_table_drain();
}

fc_command_t *ut_sq_head(void)
{
    if (g_fc_controller.sq.count == 0U)
    {
        return NULL;
    }
    return fc_command_id_to_command(g_fc_controller.sq.entries[g_fc_controller.sq.head]);
}

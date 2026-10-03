#include "ftl/completion_table.h"

#include "fc/fc.h"
#include "ftl/ftl.h"      /* ftl_context_t（route 表） */
#include "ftl/ftl_type.h"
#include "nand/nand_config.h" /* CH_NUM/CE_NUM/LUN_NUM */

#include <string.h>

/* ── 静态路由表 ── */

static completion_entry_t      g_table[COMPLETION_TABLE_SIZE];
static completion_handler_fn_t g_default_handler = NULL;
static void                   *g_default_ctx     = NULL;

/* ── 实例路由表（F6）：default handler 按命令 ch/ce/lun → FTL 实例 ── */
static struct ftl_context *g_route[CH_NUM][CE_NUM][LUN_NUM];

/* ── 路由表基础操作 ── */

void completion_table_init(void)
{
    memset(g_table, 0, sizeof(g_table));
    g_default_handler = NULL;
    g_default_ctx     = NULL;
    memset(g_route, 0, sizeof(g_route));
}

ftl_status_t completion_table_add(uint16_t                  cmd_id,
                         completion_handler_fn_t   handler,
                         void                     *ctx)
{
    uint8_t i;

    if (handler == NULL)
    {
        return FTL_ERROR;
    }

    for (i = 0U; i < COMPLETION_TABLE_SIZE; i++)
    {
        if (g_table[i].used == 0U)
        {
            g_table[i].used    = 1U;
            g_table[i].cmd_id  = cmd_id;
            g_table[i].handler = handler;
            g_table[i].ctx     = ctx;
            return FTL_OK;
        }
    }

    return FTL_ERROR;
}

void completion_table_remove(uint16_t cmd_id)
{
    uint8_t i;

    for (i = 0U; i < COMPLETION_TABLE_SIZE; i++)
    {
        if ((g_table[i].used != 0U) && (g_table[i].cmd_id == cmd_id))
        {
            g_table[i].used = 0U;
            return;
        }
    }
}

completion_entry_t *completion_table_lookup(uint16_t cmd_id)
{
    uint8_t i;

    for (i = 0U; i < COMPLETION_TABLE_SIZE; i++)
    {
        if ((g_table[i].used != 0U) && (g_table[i].cmd_id == cmd_id))
        {
            return &g_table[i];
        }
    }

    return NULL;
}

void completion_table_set_default(completion_handler_fn_t handler, void *ctx)
{
    g_default_handler = handler;
    g_default_ctx     = ctx;
}

uint16_t completion_table_count(void)
{
    uint8_t  i;
    uint16_t count = 0U;

    for (i = 0U; i < COMPLETION_TABLE_SIZE; i++)
    {
        if (g_table[i].used != 0U)
        {
            count++;
        }
    }

    return count;
}

void completion_table_set_route(struct ftl_context *route[][CE_NUM][LUN_NUM])
{
    uint8_t ch;
    uint8_t ce;
    uint8_t lun;

    for (ch = 0U; ch < CH_NUM; ch++)
    {
        for (ce = 0U; ce < CE_NUM; ce++)
        {
            for (lun = 0U; lun < LUN_NUM; lun++)
            {
                g_route[ch][ce][lun] = (route != NULL) ? route[ch][ce][lun] : NULL;
            }
        }
    }
}

struct ftl_context *completion_table_route(uint8_t ch, uint8_t ce, uint8_t lun)
{
    if ((ch >= CH_NUM) || (ce >= CE_NUM) || (lun >= LUN_NUM))
    {
        return NULL;
    }
    return g_route[ch][ce][lun];
}

/*
 * completion_table_drain — 消费所有 CQ entry 并通过路由表分发
 *
 * ① 表中有 handler → 精确路由，移除表项后执行 handler
 * ② 无 handler 但有 default → 走 default
 * ③ 都没有 → 静默丢弃
 *
 * handler 执行后表项已移除，调用者不需要手动 remove。
 */
void completion_table_drain(void)
{
    fc_command_status_t completion;

    while (fc_poll_completion(&completion) == FC_STATUS_OK)
    {
        completion_entry_t *entry = completion_table_lookup(
                                        (uint16_t)completion.command_id);

        if (entry != NULL)
        {
            completion_handler_fn_t h = entry->handler;
            void                   *c = entry->ctx;

            entry->used = 0U;

            h(&completion, c);
        }
        else if (g_default_handler != NULL)
        {
            g_default_handler(&completion, g_default_ctx);
        }
    }
}

/* ── 同步等待 handler ── */

void ftl_sync_waiter_handler(const fc_command_status_t *completion, void *ctx)
{
    ftl_sync_waiter_t *waiter = (ftl_sync_waiter_t *)ctx;

    if (waiter == NULL)
    {
        return;
    }

    waiter->completion = *completion;
    waiter->done       = 1U;

    /* 调用者负责后续 fc_command_release */
}

void ftl_sync_waiter_handler_with_release(const fc_command_status_t *completion,
                                          void                     *ctx)
{
    fc_command_t *cmd;

    ftl_sync_waiter_handler(completion, ctx);

    cmd = fc_command_id_to_command((uint16_t)completion->command_id);
    if (cmd != NULL)
    {
        (void)fc_command_release(cmd);
    }
}
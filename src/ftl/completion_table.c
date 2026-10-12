/*
 * completion_table.c — 完成路由表 + CQ 排空分发（FC 完成回调 → FTL 实例）
 *
 * 职责：
 *   把 FC 的完成回调（fc_command_status_t）按 command_id 路由到对应处理函数；
 *   未注册精确路由的命令走 default handler（默认 = ftl_handle_completion）。
 *
 * 与 completion_table.h 的分工：
 *   completion_table.h —— 表项结构（completion_entry_t）+ handler 签名 + 容量宏；
 *   本文件             —— 两张路由表的实现 + CQ 排空 + 同步等待 handler。
 *
 * 两张路由表（F6 起并存）：
 *   ① g_table[COMPLETION_TABLE_SIZE] —— command_id → {handler, ctx}，**精确路由**
 *      （同步等待、个别特殊命令注册用）；
 *   ② g_route[CH][CE][LUN] —— 按命令 ch/ce/lun 定位到 ftl_context_t，供 default
 *      handler 在实例化（多 ch/ce/lun）时找到正确的 FTL 实例。
 *
 * 文件结构：
 *   1. 静态资源         g_table / g_default_handler / g_route
 *   2. 路由表基础操作   init / add / remove / lookup / set_default / count / set_route / route
 *   3. CQ 排空分发      completion_table_drain
 *   4. 同步等待 handler ftl_sync_waiter_handler(_with_release)
 *
 * ⚠ 两张表都是**全局单例**（非 per-instance）——command_id 由 FC 全局池铸出、全局唯一。
 */

#include "ftl/completion_table.h"

#include "common/assertion.h"
#include "fc/fc.h"
#include "ftl/ftl.h" /* ftl_context_t（route 表） */
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

/* completion_table_init — 清空精确路由表 + default handler + 实例路由表（开机调用一次） */
void completion_table_init(void)
{
    memset(g_table, 0, sizeof(g_table));
    g_default_handler = NULL;
    g_default_ctx     = NULL;
    memset(g_route, 0, sizeof(g_route));
}

/* --------------------------------------------------------------------
 * completion_table_add
 *   功能：为一个 command_id 注册精确路由 handler（首个空闲槽）。
 *   参数：cmd_id —— FC command_id；handler —— 完成回调；ctx —— 回调上下文。
 *   返回：FTL_OK 成功；FTL_ERROR handler 为 NULL 或表满（COMPLETION_TABLE_SIZE）。
 *   实现要点：精确路由优先于 default（见 completion_table_drain）；handler 为 NULL
 *             属调用者传参错误，此处保留返回码（不改语义）。
 * -------------------------------------------------------------------- */
ftl_status_t completion_table_add(uint16_t                cmd_id,
                                  completion_handler_fn_t handler,
                                  void                   *ctx)
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

/* completion_table_remove — 按 command_id 释放精确路由表项（未找到则 no-op，幂等） */
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

/* completion_table_lookup — 按 command_id 查精确路由表项；未注册返回 NULL（线性扫描） */
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

/* completion_table_set_default — 设置未精确路由命令的兜底 handler（FTL 侧 = ftl_handle_completion） */
void completion_table_set_default(completion_handler_fn_t handler, void *ctx)
{
    g_default_handler = handler;
    g_default_ctx     = ctx;
}

/* completion_table_count — 精确路由表当前占用项数（观测/自检用） */
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

/* --------------------------------------------------------------------
 * completion_table_set_route
 *   功能：把调用者的 route 二维数组内容逐格拷进 g_route（F6 实例路由表）。
 *   参数：route —— [ch][ce][lun] → ftl_context_t* 数组；传 NULL 则清空 g_route。
 *   返回：无。
 *   实现要点：逐格拷贝而非存指针——调用者的数组可能是栈/局部量，逐格拷贝让 g_route
 *             成为唯一持有者，避免悬垂。
 * -------------------------------------------------------------------- */
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

/* --------------------------------------------------------------------
 * completion_table_route
 *   功能：按命令 ch/ce/lun 取所属 FTL 实例（default handler 用它找到正确实例）。
 *   参数：ch/ce/lun —— 命令自身 config 的三个坐标。
 *   返回：ftl_context_t*；set_route 之前为 NULL。
 *   实现要点：越界属编程错误（坐标由 FTL 从 PBA 灌入并经 pba_is_valid 校验）⇒ ASSERT；
 *             未注册（NULL）是运行时状态 ⇒ 保留返回 NULL。
 * -------------------------------------------------------------------- */
struct ftl_context *completion_table_route(uint8_t ch, uint8_t ce, uint8_t lun)
{
    /* ch/ce/lun 来自命令自身的 config（ftl_handle_completion 传 cmd->config.*），
     * 由 FTL 从 PBA 灌入并经 pba_is_valid() 校验——越界是编程错误，断言。 */
    ASSERT((ch < CH_NUM) && (ce < CE_NUM) && (lun < LUN_NUM));
    /* g_route 在 set_route 之前全为 NULL：运行时状态，保留返回 NULL。 */
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
 *
 * ⚠ 先取出 handler/ctx、**先置 used=0 再调用**：handler 内部可能重入注册/释放表项，
 *   先摘链可避免它在自己的回调里踩到本项。循环持续到 CQ 抽干为止。
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

/* ftl_sync_waiter_handler — 同步等待的完成回调：把 completion 存入 waiter、置 done=1（不 release 命令） */
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

/* ftl_sync_waiter_handler_with_release — 同上，但顺手 release 命令（免调用者再回收） */
void ftl_sync_waiter_handler_with_release(const fc_command_status_t *completion,
                                          void                      *ctx)
{
    fc_command_t *cmd;

    ftl_sync_waiter_handler(completion, ctx);

    cmd = fc_command_id_to_command((uint16_t)completion->command_id);
    ASSERT(cmd != NULL); /* 同 ftl_handle_completion：ID 恒在池内，壳已断言 */
    (void)fc_command_release(cmd);
}

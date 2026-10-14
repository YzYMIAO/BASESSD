/*
 * completion_table.c — cmd_id → ctx 旁表 + CQ 排空派发
 *
 * 职责：
 *   ① 维护旁表（command_id → ctx）与实例路由表（ch/ce/lun → ftl_context_t）；
 *   ② 排空 CQ：每条 completion 交 ftl_op_dispatch()（按命令的 op_tag 查服务函数数组）。
 *
 * 与 completion_table.h 的分工：
 *   completion_table.h —— 表项结构（completion_entry_t）+ 容量宏 + API 声明；
 *   本文件             —— 两张表的实现 + CQ 排空。
 *
 * ⚠ 命令该跑哪个完成服务函数**不在本表**——由命令自己携带（fc_command_t.op_tag →
 *   g_op_service[]，见 ftl_op.c）。本表只回答"这条命令的私有状态在哪"。
 *
 * ⚠ 两张表都是**全局单例**（非 per-instance）——command_id 由 FC 全局池铸出、全局唯一。
 *
 * 文件结构：
 *   1. 静态资源     g_table / g_route
 *   2. 旁表操作     init / add / remove / take / count
 *   3. 实例路由     set_route / route
 *   4. CQ 排空      completion_table_drain
 */

#include "ftl/completion_table.h"

#include "common/assertion.h"
#include "fc/fc.h"
#include "ftl/ftl.h" /* ftl_context_t（route 表） */
#include "ftl/ftl_op.h"
#include "ftl/ftl_type.h"
#include "nand/nand_config.h" /* CH_NUM/CE_NUM/LUN_NUM */

#include <string.h>

/* ── 1. 静态资源 ── */

static completion_entry_t g_table[COMPLETION_TABLE_SIZE];

/* ── 实例路由表（F6）：按命令 ch/ce/lun → FTL 实例 ── */
static struct ftl_context *g_route[CH_NUM][CE_NUM][LUN_NUM];

/* ── 2. 旁表操作 ── */

/* completion_table_init — 清空旁表 + 实例路由表（开机调用一次） */
void completion_table_init(void)
{
    memset(g_table, 0, sizeof(g_table));
    memset(g_route, 0, sizeof(g_route));
}

/* --------------------------------------------------------------------
 * completion_table_add
 *   功能：为一个 command_id 登记 ctx（首个空闲槽）。
 *   参数：cmd_id —— FC command_id；ctx —— 该命令的私有状态。
 *   返回：FTL_OK 成功；FTL_ERROR 表满（COMPLETION_TABLE_SIZE）。
 *   实现要点：ctx 允许为 NULL（表示"无私有状态"，服务函数自行派生）——与旧实现
 *             拒绝 NULL handler 不同：那时 NULL 是漏传回调，现在只是"不用旁表"。
 * -------------------------------------------------------------------- */
ftl_status_t completion_table_add(uint16_t cmd_id, void *ctx)
{
    uint8_t i;

    for (i = 0U; i < COMPLETION_TABLE_SIZE; i++)
    {
        if (g_table[i].used == 0U)
        {
            g_table[i].used   = 1U;
            g_table[i].cmd_id = cmd_id;
            g_table[i].ctx    = ctx;
            return FTL_OK;
        }
    }

    return FTL_ERROR;
}

/* completion_table_remove — 按 command_id 释放表项（未找到则 no-op，幂等） */
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

/* completion_table_take — 查 + 摘链 + 返回 ctx（未登记返回 NULL） */
void *completion_table_take(uint16_t cmd_id)
{
    uint8_t i;

    for (i = 0U; i < COMPLETION_TABLE_SIZE; i++)
    {
        if ((g_table[i].used != 0U) && (g_table[i].cmd_id == cmd_id))
        {
            void *ctx = g_table[i].ctx;

            g_table[i].used = 0U;
            return ctx;
        }
    }

    return NULL;
}

/* completion_table_count — 旁表当前占用项数（观测/自检用） */
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

/* ── 3. 实例路由 ── */

/* --------------------------------------------------------------------
 * completion_table_set_route
 *   功能：把调用者的 route 数组内容逐格拷进 g_route（F6 实例路由表）。
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
 *   功能：按命令 ch/ce/lun 取所属 FTL 实例（派发时用它找到正确实例）。
 *   参数：ch/ce/lun —— 命令自身 config 的三个坐标。
 *   返回：ftl_context_t*；set_route 之前为 NULL。
 *   实现要点：越界属编程错误（坐标由 FTL 从 PBA 灌入并经 pba_is_valid 校验）⇒ ASSERT；
 *             未注册（NULL）是运行时状态 ⇒ 保留返回 NULL。
 * -------------------------------------------------------------------- */
struct ftl_context *completion_table_route(uint8_t ch, uint8_t ce, uint8_t lun)
{
    ASSERT((ch < CH_NUM) && (ce < CE_NUM) && (lun < LUN_NUM));
    /* g_route 在 set_route 之前全为 NULL：运行时状态，保留返回 NULL。 */
    return g_route[ch][ce][lun];
}

/* ── 4. CQ 排空 ── */

/*
 * completion_table_drain — 消费所有 CQ entry 并派发
 *
 * 每条 completion 交 ftl_op_dispatch()：它按命令携带的 op_tag 查 g_op_service[]，
 * 从旁表摘 ctx，再调对应服务函数。命令该跑什么**不由本表决定**。
 *
 * ⚠ 摘链在调用服务函数**之前**（ftl_op_dispatch 内）——服务函数可能重入登记/释放
 *   表项，先摘可避免它在自己的回调里踩到本项。循环持续到 CQ 抽干为止。
 */
void completion_table_drain(void)
{
    fc_command_status_t completion;

    while (fc_poll_completion(&completion) == FC_STATUS_OK)
    {
        ftl_op_dispatch(&completion);
    }
}

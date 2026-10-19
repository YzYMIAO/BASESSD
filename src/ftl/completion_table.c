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
 *   g_op_callbacks[]，见 ftl_op.c）。本表只回答"这条命令的私有状态在哪"。
 *
 * ⚠ 两张表都是**全局单例**（非 per-instance）——command_id 由 FC 全局池铸出、全局唯一。
 *
 * 文件结构：
 *   1. 静态资源     g_table / g_route
 *   2. 旁表操作     init / add / remove / take / count
 *   3. 实例路由     set_route / route
 *   4. CQ 排空      completion_table_drain
 */

#include "completion_table.h"

#include "assertion.h"
#include "fc.h"
#include "ftl.h" /* ftl_context_t（route 表） */
#include "ftl_op.h"
#include "ftl_type.h"
#include "l2p_cache.h" /* ftl_dim_ch/ce/lun（dim → 坐标的唯一分解点） */
#include "log.h"
#include "nand_config.h" /* CH_NUM/CE_NUM/LUN_NUM */

#include <string.h>

/* 旁表按 command_id 键 ⇒ 容量上界 = 同时在飞命令数 ≤ FC 命令池深度。
 * 池深是 FC 层的事实上界，写成常量会在 FC 扩容时静默变小（H4）。 */
_Static_assert(COMPLETION_TABLE_SIZE >= FC_COMMAND_POOL_DEPTH,
               "旁表容量必须 ≥ FC 命令池深度（推导见 completion_table.h）");

/* ── 1. 静态资源 ── */

static completion_entry_t g_table[COMPLETION_TABLE_SIZE];

/* ── 实例路由表：按命令 ch/ce/lun → FTL 实例 ── */
static struct ftl_context *g_route[CH_NUM][CE_NUM][LUN_NUM];

/* ── 2. 旁表操作 ── */

/* completion_table_init — 清空旁表 + 实例路由表（开机调用一次）
 *
 * ⚠ 清的是**全局**资源（两个 file-static），只能调一次：由 main.c 的
 *   ssd_ftl_boot_all 在**所有实例 boot 之前**调用。**不可**放进逐实例的 ftl_init ——
 *   那样实例 N 的 init 会抹掉实例 N-1 在飞的 ctx 登记，以及已设好的路由表。 */
void completion_table_init(void)
{
    memset(g_table, 0, sizeof(g_table));
    memset(g_route, 0, sizeof(g_route));
}

/* --------------------------------------------------------------------
 * completion_table_add
 *   功能：为一个 command_id 登记 ctx。
 *   参数：cmd_id —— FC command_id；ctx —— 该命令的私有状态。
 *   返回：FTL_OK 成功；FTL_ERROR 表满（COMPLETION_TABLE_SIZE）。
 *   实现要点：本闸门保证**同一 cmd_id 至多一条表项**——命中已有项即**原地覆盖** ctx，
 *             不追加第二条。这个不变量必须在这里守：命令池只有 FC_COMMAND_POOL_DEPTH
 *             个 id，会被复用；若旧项因完成从未被观测而残留（CQ 满时 fc.c 强制置完成、
 *             FTL 感知不到那条 completion），再 add 同一 id 就会让表里出现两条同 id 项，
 *             而 completion_table_take 按**首个**匹配摘链 ⇒ 服务函数拿到**陈旧**的 ctx
 *             并据此改状态，本命令自己的表项则永久泄漏。
 *             覆盖时记 LOGW：这条路径一旦发生必须自己报出来，否则就是静默失效（C8）。
 *             ctx 允许为 NULL（表示"无私有状态"，服务函数自行派生）。
 * -------------------------------------------------------------------- */
ftl_status_t completion_table_add(uint16_t cmd_id, void *ctx)
{
    for (uint8_t i = 0U; i < COMPLETION_TABLE_SIZE; i++)
    {
        if ((g_table[i].used != 0U) && (g_table[i].cmd_id == cmd_id))
        {
            LOGW("[FTL] completion entry cmd=%u reused — stale ctx overwritten\r\n",
                 (unsigned)cmd_id);
            g_table[i].ctx = ctx;
            return FTL_OK;
        }
    }

    for (uint8_t i = 0U; i < COMPLETION_TABLE_SIZE; i++)
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
    for (uint8_t i = 0U; i < COMPLETION_TABLE_SIZE; i++)
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
    for (uint8_t i = 0U; i < COMPLETION_TABLE_SIZE; i++)
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
    uint16_t count = 0U;

    for (uint8_t i = 0U; i < COMPLETION_TABLE_SIZE; i++)
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
 *   功能：把调用者的 route 数组内容逐格拷进 g_route（实例路由表）。
 *   参数：route —— [ch][ce][lun] → ftl_context_t* 数组；传 NULL 则清空 g_route。
 *   返回：无。
 *   实现要点：逐格拷贝而非存指针——调用者的数组可能是栈/局部量，逐格拷贝让 g_route
 *             成为唯一持有者，避免悬垂。
 * -------------------------------------------------------------------- */
void completion_table_set_route(struct ftl_context *route[][CE_NUM][LUN_NUM])
{
    for (uint8_t ch = 0U; ch < CH_NUM; ch++)
    {
        for (uint8_t ce = 0U; ce < CE_NUM; ce++)
        {
            for (uint8_t lun = 0U; lun < LUN_NUM; lun++)
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
 *   实现要点：越界属编程错误（坐标由 FTL 从 PBA 灌入并经 pba_is_valid 校验）⇒ ASSERT。
 *             返回 NULL 同属编程错误（set_route 漏调）——但**不在此处断言**：判据在
 *             **被调方**（各服务函数入口的 ASSERT(ftl != NULL)，见 ftl_op.h 的所有权表
 *             与各服务函数），此处再断一次只是把同一判据写两遍（C7：它不会独立失败）。
 *             不用 ftl 的服务函数（SYNC_WAIT / JOURNAL / BBT）收到 NULL 也无需处理。
 * -------------------------------------------------------------------- */
struct ftl_context *completion_table_route(uint8_t ch, uint8_t ce, uint8_t lun)
{
    ASSERT((ch < CH_NUM) && (ce < CE_NUM) && (lun < LUN_NUM));
    return g_route[ch][ce][lun];
}

/*
 * completion_table_route_region
 *   功能：按**全局 L2P region** 取 owner FTL 实例（共享 cache 的 owner 解析）。
 *   参数：global_region —— 全局 region 序号（lba >> L2P_REGION_SHIFT）。
 *   返回：owner 实例；set_route 之前为 NULL。
 *   实现要点：dim = region % FTL_DIM_TOTAL，再按 ftl_dim_ch/ce/lun 分解——分解式与
 *             lba_route_* **共用同一份实现**（l2p_cache.h 的 ftl_dim_*），不是各写一套（C9）。
 * -------------------------------------------------------------------- */
struct ftl_context *completion_table_route_region(uint32_t global_region)
{
    const uint32_t dim = global_region % FTL_DIM_TOTAL;

    return completion_table_route(ftl_dim_ch(dim), ftl_dim_ce(dim), ftl_dim_lun(dim));
}

/* ── 4. CQ 排空 ── */

/*
 * completion_table_drain — 消费所有 CQ entry 并派发
 *
 * 每条 completion 交 ftl_op_dispatch()：它按命令携带的 op_tag 查 g_op_callbacks[]，
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

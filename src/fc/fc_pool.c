/*
 * fc_pool.c — FC 三池：命令 / 描述符 / 缓冲
 *
 * 职责：
 *   - 三池本体与簿记数组（命令状态、desc 归属、buffer 归属/状态）；
 *   - 空闲链表与 O(1) 计数；
 *   - 命令/描述符/缓冲的分配、释放、链遍历；
 *   - 池相关的公开 API（建链、命令释放、buffer 访问、链查询）。
 *
 * 文件结构（按职责分区）：
 *   1. 静态资源 / 初始化
 *   2. 池簿记 / 状态闸门
 *   3. 分配 / 释放（含链释放）
 *   4. 建链 / 链校验
 *   5. 公开 API / 查询
 *   6. ID / 指针转换（文件末尾）
 *
 * 重点机制体：fc_pool_command_state_set / fc_pool_chain_validate /
 *             fc_pool_command_release / fc_command_configure_descriptor_with_buffer。
 *
 * 提交/填充/完成调度在 fc.c；池私有契约见 fc_pool.h，控制器/调度器共享定义见
 * fc_internal.h（两个私有头都在 src/fc/）。
 */

#include "fc_pool.h"

#include "assertion.h"
#include "fc_internal.h"
#include "nand_interface.h"
#include "ssd_compiler.h"
#include "stats.h"

#include <string.h>

/* ── 三池本体 ──
 * 文件作用域 static，地址在固件生命周期内固定；fc_ctl_s 只存指针。
 * buffer 固定在 noncacheable 段：FMC 数据面按字节读写，避免 CPU D-Cache
 * 与总线视图不一致；也保证地址落在 DMA 可达区（DMA1/2 访问不到 DTCM）。 */
static fc_cmd_s           g_fc_command_pool[FC_COMMAND_POOL_DEPTH];
static fc_desc_s          g_fc_descriptor_pool[FC_DESCRIPTOR_POOL_DEPTH];
static fc_buffer_cw_arr_t g_fc_buffer_pool[FC_BUFFER_POOL_DEPTH] NONCACHEABLE_BUFFER;
static fc_cmd_id_t        g_fc_descriptor_owner[FC_DESCRIPTOR_POOL_DEPTH];
static fc_cmd_state_e     g_fc_command_state[FC_COMMAND_POOL_DEPTH];
static fc_cmd_id_t        g_fc_command_free_next[FC_COMMAND_POOL_DEPTH];
static fc_desc_id_t       g_fc_descriptor_free_next[FC_DESCRIPTOR_POOL_DEPTH];
static fc_buf_id_t        g_fc_buffer_free_next[FC_BUFFER_POOL_DEPTH];
static fc_buf_state_e     g_fc_buffer_state[FC_BUFFER_POOL_DEPTH];
static fc_cmd_id_t        g_fc_buffer_owner[FC_BUFFER_POOL_DEPTH];

/* 当前命令分配类：desc/buffer 分配跟随最近一次 fc_command_alloc_class()。
 * 单线程 superloop 的构造期上下文；默认 CRIT（测试/直接调用可用满池）。 */
static fc_cmd_class_e s_alloc_class = FC_CMD_CLASS_CRIT;

/* ── 统计（docs/design/common/statistics_design.md）──
 * 统计专用对象/表在 SSD_STATS_ENABLE=0 时整体不编译；自增点经 STATS_INC 自动消失。 */
#if SSD_STATS_ENABLE
typedef struct FC_ALLOC_STATISTICS_S
{
    DWORD cmd_fail_bg;
    DWORD cmd_fail_crit;
    DWORD desc_fail;
    DWORD buf_fail;
} fc_alloc_stats_s;

static fc_alloc_stats_s s_stats_fc_alloc;

/* fc.pool 的 base 是控制器本体；SQ/HSQ/CQ 当前占用用嵌套 offset 取（无影子字段）。 */
_Static_assert(sizeof(fc_ctl_s) <= 0xFFFFU, "fc_ctl_s exceeds stats WORD offset range");
static const stats_name_s s_stats_fc_pool_names[] = {
    {STATS_NAME_STR(command_used), STATS_OFFSET(fc_ctl_s, command_used), STATS_U16, STATS_GAUGE},
    {STATS_NAME_STR(descriptor_used), STATS_OFFSET(fc_ctl_s, descriptor_used), STATS_U16, STATS_GAUGE},
    {STATS_NAME_STR(buffer_used), STATS_OFFSET(fc_ctl_s, buffer_used), STATS_U16, STATS_GAUGE},
    {STATS_NAME_STR(command_high), STATS_OFFSET(fc_ctl_s, command_high), STATS_U16, STATS_GAUGE},
    {STATS_NAME_STR(descriptor_high), STATS_OFFSET(fc_ctl_s, descriptor_high), STATS_U16, STATS_GAUGE},
    {STATS_NAME_STR(buffer_high), STATS_OFFSET(fc_ctl_s, buffer_high), STATS_U16, STATS_GAUGE},
    {STATS_NAME_STR(sq_used), STATS_OFFSET_NESTED(fc_ctl_s, sq, fc_sub_queue_s, count), STATS_U16, STATS_GAUGE},
    {STATS_NAME_STR(hsq_used), STATS_OFFSET_NESTED(fc_ctl_s, hsq, fc_sub_queue_s, count), STATS_U16, STATS_GAUGE},
    {STATS_NAME_STR(cq_used), STATS_OFFSET_NESTED(fc_ctl_s, cq, fc_comp_queue_s, count), STATS_U16, STATS_GAUGE},
};

STATS_CHECK_FIELD(fc_ctl_s, command_used, STATS_U16);
STATS_CHECK_FIELD(fc_ctl_s, descriptor_used, STATS_U16);
STATS_CHECK_FIELD(fc_ctl_s, buffer_used, STATS_U16);
STATS_CHECK_FIELD(fc_ctl_s, command_high, STATS_U16);
STATS_CHECK_FIELD(fc_ctl_s, descriptor_high, STATS_U16);
STATS_CHECK_FIELD(fc_ctl_s, buffer_high, STATS_U16);
STATS_CHECK_FIELD(fc_sub_queue_s, count, STATS_U16);
STATS_CHECK_FIELD(fc_comp_queue_s, count, STATS_U16);

#    define FC_ALLOC_STATS_LIST(X)                 \
        X(cmd_fail_bg, STATS_U32, STATS_COUNTER)   \
        X(cmd_fail_crit, STATS_U32, STATS_COUNTER) \
        X(desc_fail, STATS_U32, STATS_COUNTER)     \
        X(buf_fail, STATS_U32, STATS_COUNTER)

#    define FC_ALLOC_STATS_NAME(field, width, kind) \
        {STATS_NAME_STR(field), STATS_OFFSET(fc_alloc_stats_s, field), width, kind},
static const stats_name_s s_stats_fc_alloc_names[] = {
    FC_ALLOC_STATS_LIST(FC_ALLOC_STATS_NAME)};
#    undef FC_ALLOC_STATS_NAME
#    define FC_ALLOC_STATS_CHECK(field, width, kind) STATS_CHECK_FIELD(fc_alloc_stats_s, field, width);
FC_ALLOC_STATS_LIST(FC_ALLOC_STATS_CHECK)
#    undef FC_ALLOC_STATS_CHECK
#endif /* SSD_STATS_ENABLE */

/**
 * @brief 初始化三池本体、簿记数组与空闲链表（不碰队列/调度器/硬件）
 *
 * @param controller FC 控制器。
 *
 * @note 三池本体是文件作用域 static，地址在固件生命周期内固定；可重复调用以重置。
 *       队列/调度器/RAID 状态由 fc_init 初始化，本函数不碰。
 */
void fc_pool_init(fc_ctl_s *controller)
{
    WORD index;

    memset(g_fc_command_pool, 0, sizeof(g_fc_command_pool));
    memset(g_fc_descriptor_pool, 0, sizeof(g_fc_descriptor_pool));
    memset(g_fc_buffer_pool, 0, sizeof(g_fc_buffer_pool));
    memset(g_fc_command_state, 0, sizeof(g_fc_command_state));

    controller->command_pool         = g_fc_command_pool;
    controller->descriptor_pool      = g_fc_descriptor_pool;
    controller->buffer_pool          = g_fc_buffer_pool;
    controller->descriptor_owner     = g_fc_descriptor_owner;
    controller->command_state        = g_fc_command_state;
    controller->command_free_next    = g_fc_command_free_next;
    controller->descriptor_free_next = g_fc_descriptor_free_next;
    controller->buffer_free_next     = g_fc_buffer_free_next;
    controller->buffer_state         = g_fc_buffer_state;
    controller->buffer_owner         = g_fc_buffer_owner;

    for (index = 0U; index < FC_COMMAND_POOL_DEPTH; index++)
    {
        controller->command_pool[index].cmd_id = (fc_cmd_id_t)index;
        controller->command_free_next[index]   = ((index + 1U) < FC_COMMAND_POOL_DEPTH) ? (fc_cmd_id_t)(index + 1U) : FC_CMD_ID_INVALID;
    }
    for (index = 0U; index < FC_DESCRIPTOR_POOL_DEPTH; index++)
    {
        controller->descriptor_owner[index]     = FC_CMD_ID_INVALID;
        controller->descriptor_free_next[index] = ((index + 1U) < FC_DESCRIPTOR_POOL_DEPTH) ? (fc_desc_id_t)(index + 1U) : FC_DESC_ID_INVALID;
    }
    for (index = 0U; index < FC_BUFFER_POOL_DEPTH; index++)
    {
        controller->buffer_free_next[index] = ((index + 1U) < FC_BUFFER_POOL_DEPTH) ? (fc_buf_id_t)(index + 1U) : FC_BUF_ID_INVALID;
        controller->buffer_state[index]     = FC_BUFFER_STATE_FREE;
        controller->buffer_owner[index]     = FC_CMD_ID_INVALID;
    }
    controller->command_free_head    = 0U;
    controller->descriptor_free_head = 0U;
    controller->buffer_free_head     = 0U;
    controller->command_used         = 0U;
    controller->descriptor_used      = 0U;
    controller->buffer_used          = 0U;
    controller->command_high         = 0U;
    controller->descriptor_high      = 0U;
    controller->buffer_high          = 0U;
    s_alloc_class                    = FC_CMD_CLASS_CRIT;

#if SSD_STATS_ENABLE
    memset(&s_stats_fc_alloc, 0, sizeof(s_stats_fc_alloc));
    (void)stats_register("fc.pool", STATS_INST_NONE, controller, s_stats_fc_pool_names, STATS_ENTRY_COUNT(s_stats_fc_pool_names));
    (void)stats_register("fc.alloc", STATS_INST_NONE, &s_stats_fc_alloc, s_stats_fc_alloc_names, STATS_ENTRY_COUNT(s_stats_fc_alloc_names));
#endif
}

/**
 * @brief 命令状态唯一 setter：只允许状态机定义的转移。
 *
 * @param controller FC 控制器
 * @param cmd_id 命令池槽位号
 * @param next       目标状态
 *
 * @note 非法转移：debug 停机；release 拒绝写入（ASSERT_REJECT_VOID），避免静默
 *       破坏状态机。本函数是状态字段的唯一写点，调用方不重复判定同一转移。
 */
void fc_pool_command_state_set(fc_ctl_s *controller, fc_cmd_id_t cmd_id, fc_cmd_state_e next)
{
    /* 下标先拒绝：release 下也不能越界读写 command_state[]。 */
    ASSERT_REJECT_VOID(cmd_id < FC_COMMAND_POOL_DEPTH,
                       "cmd state: id=%hu out of range",
                       cmd_id);

    const fc_cmd_state_e current = controller->command_state[cmd_id];
    BOOL                 allowed = false;

    switch (next)
    {
        case FC_COMMAND_STATE_ALLOCATED:
            allowed = (current == FC_COMMAND_STATE_FREE);
            break;
        case FC_COMMAND_STATE_QUEUED:
            allowed = (current == FC_COMMAND_STATE_ALLOCATED);
            break;
        case FC_COMMAND_STATE_ACTIVE:
            allowed = (current == FC_COMMAND_STATE_QUEUED);
            break;
        case FC_COMMAND_STATE_COMPLETED:
            allowed = (current == FC_COMMAND_STATE_ACTIVE);
            break;
        case FC_COMMAND_STATE_FREE:
            allowed = (current == FC_COMMAND_STATE_ALLOCATED) || (current == FC_COMMAND_STATE_COMPLETED);
            break;
        default:
            allowed = false;
            break;
    }
    /* 非法转移：debug 停机；release 拒绝写入，避免静默破坏状态机。 */
    ASSERT_REJECT_VOID(allowed,
                       "cmd state: cmd=%hu %d -> %d not allowed",
                       cmd_id,
                       current,
                       next);
    controller->command_state[cmd_id] = next;
}

/**
 * @brief 分配 desc 并登记归属（不挂链）
 *
 * 出池即自洽：next_id/buffer_id 显式置哨兵。
 *
 * @param controller FC 控制器
 * @param cmd_id 归属命令 ID（公共入口已校验范围/状态）
 * @return desc 指针；NULL = desc 池空
 */
static fc_desc_s *fc_pool_descriptor_alloc(fc_ctl_s *controller, fc_cmd_id_t cmd_id)
{
    WORD descriptor_index;

    WORD limit = FC_DESCRIPTOR_POOL_DEPTH;
    if (s_alloc_class == FC_CMD_CLASS_BG)
    {
        limit -= FC_CMD_RESERVE_CRIT_DESC;
    }
    if (controller->descriptor_used >= limit)
    {
        STATS_INC(s_stats_fc_alloc.desc_fail);
        return NULL;
    }

    descriptor_index = controller->descriptor_free_head;
    if (descriptor_index == FC_DESC_ID_INVALID)
    {
        STATS_INC(s_stats_fc_alloc.desc_fail);
        return NULL;
    }
    ASSERT_REJECT(controller->descriptor_owner[descriptor_index] == FC_CMD_ID_INVALID, NULL, "descriptor alloc: desc=%hu owner=%hu", descriptor_index, controller->descriptor_owner[descriptor_index]);
    controller->descriptor_free_head = controller->descriptor_free_next[descriptor_index];
    memset(&controller->descriptor_pool[descriptor_index], 0, sizeof(controller->descriptor_pool[descriptor_index]));
    controller->descriptor_pool[descriptor_index].next_id   = FC_DESC_ID_INVALID;
    controller->descriptor_pool[descriptor_index].buffer_id = FC_BUF_ID_INVALID;
    controller->descriptor_owner[descriptor_index]          = cmd_id;
    controller->descriptor_used++;
    if (controller->descriptor_used > controller->descriptor_high)
    {
        controller->descriptor_high = controller->descriptor_used;
    }
    return &controller->descriptor_pool[descriptor_index];
}

/**
 * @brief 把描述符本体还回空闲链（清归属、清本体、头插空闲链）。
 *
 * @param controller FC 控制器
 * @param desc_id    池内描述符 ID。
 *
 * @note **不碰数据链、不碰 buffer**——只把 desc 本体还池。调用者负责
 * 先把该 desc 从链上摘掉（见 fc_pool_chain_release_head）。
 */
static void fc_pool_descriptor_body_release(fc_ctl_s *controller, fc_desc_id_t desc_id)
{
    ASSERT_REJECT_VOID(controller->descriptor_owner[desc_id] != FC_CMD_ID_INVALID, "descriptor release: desc=%hu already free", desc_id);
    ASSERT(controller->descriptor_used > 0U);
    memset(&controller->descriptor_pool[desc_id], 0, sizeof(controller->descriptor_pool[desc_id]));
    controller->descriptor_owner[desc_id]     = FC_CMD_ID_INVALID;
    controller->descriptor_free_next[desc_id] = controller->descriptor_free_head;
    controller->descriptor_free_head          = desc_id;
    controller->descriptor_used--;
}

/**
 * @brief 释放 buffer（校验归属 → 状态清零 → 挂回空闲链表头）
 *
 * @param controller FC 控制器
 * @param buffer_id  池内 buffer ID
 * @param owner      期望归属：命令 ID；上层预申请未挂链时为 FC_CMD_ID_INVALID
 *
 * @note 未占用/归属不符时 debug 停机、release 拒绝（ASSERT_REJECT_VOID），
 *       不返回错误——失败即调用者 bug，空闲链保持完整。
 */
static void fc_pool_buffer_release(fc_ctl_s *controller, fc_buf_id_t buffer_id, fc_cmd_id_t owner)
{
    // release 档也必须先判下标，再访问数组（ASSERT 在 NDEBUG 下不存在）。
    ASSERT_REJECT_VOID(buffer_id < FC_BUFFER_POOL_DEPTH, "buffer release: id=%hu out of range", buffer_id);
    // 判据 9：归属不符/未占用时拒绝，避免同一节点二次入链造成静默介质破坏。
    ASSERT_REJECT_VOID((controller->buffer_state[buffer_id] == FC_BUFFER_STATE_OCCUPIED) && (controller->buffer_owner[buffer_id] == owner),
                       "buffer release: id=%hu state=%d owner=%hu expected=%hu",
                       buffer_id,
                       controller->buffer_state[buffer_id],
                       controller->buffer_owner[buffer_id],
                       owner);
    ASSERT(controller->buffer_used > 0U);

    controller->buffer_state[buffer_id]     = FC_BUFFER_STATE_FREE;
    controller->buffer_owner[buffer_id]     = FC_CMD_ID_INVALID;
    controller->buffer_free_next[buffer_id] = controller->buffer_free_head;
    controller->buffer_free_head            = buffer_id;
    controller->buffer_used--;
}

/**
 * @brief 释放链头一个 desc（含其 buffer），返回新链头
 *
 * 先读 next_id 再释放（释放会整体清零）；无 buffer 的 desc 跳过 buffer 释放。
 *
 * @param controller FC 控制器
 * @param head       链头 ID
 *
 * @return 新链头；空链原样返回 head，成环返回 FC_DESC_ID_INVALID
 */
static fc_desc_id_t fc_pool_chain_release_head(fc_ctl_s *controller, fc_desc_id_t head)
{
    fc_buf_id_t buffer_id;

    if (head >= FC_DESCRIPTOR_POOL_DEPTH)
    {
        return head;
    }

    const fc_cmd_id_t owner = controller->descriptor_owner[head];

    // 成环时链头会回到已经释放的 desc：owner 已清 INVALID。先断链再返回，
    // 避免把已清零的 buffer_id/next_id 当成有效数据继续释放。
    if (owner == FC_CMD_ID_INVALID)
    {
        ASSERT_MSG(owner != FC_CMD_ID_INVALID, "descriptor chain cycle at desc=%hu", head);
        return FC_DESC_ID_INVALID;
    }

    buffer_id                  = (fc_buf_id_t)controller->descriptor_pool[head].buffer_id;
    const fc_desc_id_t next_id = (fc_desc_id_t)controller->descriptor_pool[head].next_id;

    if (buffer_id != FC_BUF_ID_INVALID)
    {
        fc_pool_buffer_release(controller, buffer_id, owner);
    }
    fc_pool_descriptor_body_release(controller, head);
    return next_id;
}

/**
 * @brief 沿链释放全部 desc + buffer
 *
 * @param controller FC 控制器
 * @param head       链头 ID
 *
 * @note 成环/链超长时 debug 停机、release 拒绝（ASSERT_REJECT_VOID），
 *       不静默留下半条链。
 */
static void fc_pool_chain_release_all(fc_ctl_s *controller, fc_desc_id_t head)
{
    // 链在上界内仍可能成环；释放路径必须有界，不能靠调用者先校验。
    for (WORD steps = 0U; (head < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH); steps++)
    {
        head = fc_pool_chain_release_head(controller, head);
    }
    ASSERT_REJECT_VOID(head == FC_DESC_ID_INVALID,
                       "chain release: head=%hu not invalid after bounded walk",
                       head);
}

/**
 * @brief 沿命令的 desc 链归还**全部 buffer**，并把各 desc 的 buffer_id 抹成 FC_BUF_ID_INVALID（**幂等**：已 INVALID 的跳过）。
 *
 * @param controller FC 控制器
 * @param cmd_id 命令 ID。
 *
 * @note 只动 buffer，**不动 desc 链、不动命令槽位**——它们仍由
 * fc_command_release 回收。唯一调用者：fc_complete_from_nand 在
 * 「buf_own 的释放位 = FC 释放（01/11）」时当场归还（此时完成项已组装完毕，
 * 之后没有任何读者碰 buffer）。抹 INVALID 是为了让随后的链释放自然跳过，
 * 从而不双重归还；命令若没走到完成就释放，buffer 仍有效，链释放照常归还
 * ⇒ 不泄漏。归属语义见 fc_buf_own_e。
 */
void fc_pool_command_release_buffers(fc_ctl_s *controller, fc_cmd_id_t cmd_id)
{
    ASSERT(controller != NULL);
    ASSERT(cmd_id < FC_COMMAND_POOL_DEPTH);

    fc_cmd_s    *cmd     = &controller->command_pool[cmd_id];
    fc_desc_id_t desc_id = (fc_desc_id_t)cmd->desc_id;
    ASSERT(cmd->cmd_id == cmd_id);

    for (WORD steps = 0U; (desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH); steps++)
    {
        fc_desc_s  *descriptor = &controller->descriptor_pool[desc_id];
        fc_buf_id_t buffer_id  = (fc_buf_id_t)descriptor->buffer_id;

        if (buffer_id != FC_BUF_ID_INVALID)
        {
            fc_pool_buffer_release(controller, buffer_id, cmd_id);
            descriptor->buffer_id = FC_BUF_ID_INVALID;
        }
        desc_id = (fc_desc_id_t)descriptor->next_id;
    }
    ASSERT_REJECT_VOID(desc_id == FC_DESC_ID_INVALID,
                       "release buffers: desc chain not empty for cmd=%hu",
                       cmd_id);
}

/**
 * @brief 提交前的一次性 desc 链完整校验（归属 + 坐标 + 唯一性 + buffer + 行一致 + 覆盖）。
 *
 * 这是建链/提交路径的**唯一**完整校验漏斗：逐 desc 的建链入口只保留逐项坐标断言，
 * 整链在提交前扫一次；不在每次 append 时做 O(n) 查重。
 *
 * @param controller FC 控制器
 * @param cmd_id 命令 ID
 * @return true 链完整；false 归属不符 / 越段 / CW 重复 / buffer 未占用 /
 *         同 plane 行地址不一致 / **数据命令** multi-plane 覆盖不全 /
 *         multi_plane 越界。
 *
 * @note 由 fc_submit / fc_submit_batch 无条件调用（debug ASSERT + release 拒绝）。
 *       链在提交后冻结，故校验一次即可覆盖 dispatch 阶段。
 */
BOOL fc_pool_chain_validate(const fc_ctl_s *controller, fc_cmd_id_t cmd_id)
{
    ASSERT(controller != NULL);
    ASSERT(cmd_id < FC_COMMAND_POOL_DEPTH);

    const fc_cmd_s *cmd = &controller->command_pool[cmd_id];
    ASSERT(cmd->cmd_id == cmd_id);

    fc_desc_id_t descriptor_id = (fc_desc_id_t)cmd->desc_id;
    DWORD        seen          = 0U;
    WORD         count         = 0U;
    BYTE         plane_first;
    BYTE         plane_last;
    BYTE         planes_with_desc = 0U; /* bit p = plane p 链上有 desc */
    DWORD        plane_row[PLANE_NUM];  /* 各 plane 的行地址（链上第一条 desc） */

    _Static_assert(PLANE_NUM <= 8U, "planes_with_desc is a BYTE bitmap");

    if (cmd->config.multi_plane > PLANE_NUM)
    {
        return false;
    }

    /* staging 类命令（FEATURES/META/ONFI/COLUMN）直接用 paa_to_plane(cmd->paa)
     * 定位暂存 buffer；multi-plane 命令的 plane_span 从 0 起、不校验 cmd->paa 的
     * plane，故这里对**所有**命令先校验一次，release 下也拒绝越界。 */
    if (paa_to_plane(cmd->paa) >= PLANE_NUM)
    {
        return false;
    }

    (void)fc_command_plane_span(cmd, &plane_first, &plane_last);
    if (plane_first >= PLANE_NUM)
    {
        return false;
    }

    while (descriptor_id != FC_DESC_ID_INVALID)
    {
        if (descriptor_id >= FC_DESCRIPTOR_POOL_DEPTH)
        {
            return false;
        }
        if (controller->descriptor_owner[descriptor_id] != cmd_id)
        {
            return false;
        }
        /* 链长上界同时防环：超过 desc 池容量必然有重复节点。 */
        if (count >= FC_DESCRIPTOR_POOL_DEPTH)
        {
            return false;
        }

        const fc_desc_s *descriptor = &controller->descriptor_pool[descriptor_id];
        const BYTE       plane      = paa_to_plane(descriptor->paa);
        const BYTE       cw         = paa_to_cw(descriptor->paa);
        const DWORD      n          = fc_cw_flat(plane, cw);

        if ((plane < plane_first) || (plane >= plane_last) || (cw >= CW_NUM) || (n >= CW_TOTAL_MAX))
        {
            return false;
        }
        if ((seen & (1UL << n)) != 0UL)
        {
            return false;
        }
        seen |= (1UL << n);

        /* 同一 plane 的所有 desc 必须指向同一物理位置——一个 plane 一次只访问一个块/页。 */
        if ((planes_with_desc & (1U << plane)) == 0U)
        {
            plane_row[plane] = paa_with_plane(paa_page_base(descriptor->paa), plane);
            planes_with_desc |= (1U << plane);
        }
        else if ((paa_to_block(descriptor->paa) != paa_to_block(plane_row[plane])) ||
                 (paa_to_page(descriptor->paa) != paa_to_page(plane_row[plane])) ||
                 (paa_to_lun(descriptor->paa) != paa_to_lun(plane_row[plane])))
        {
            return false;
        }

        if (descriptor->buffer_id != FC_BUF_ID_INVALID)
        {
            if ((descriptor->buffer_id >= FC_BUFFER_POOL_DEPTH) || (controller->buffer_state[descriptor->buffer_id] == FC_BUFFER_STATE_FREE) || (controller->buffer_owner[descriptor->buffer_id] != cmd_id))
            {
                return false;
            }
        }

        count++;
        descriptor_id = (fc_desc_id_t)descriptor->next_id;
    }

    /*
     * multi-plane：带 host 数据搬运的命令由 dispatch 逐 plane 从 desc 链取行地址/数据，
     * 缺一个 plane 只能退化成 block0/page0 或全 0xFF 编程——必须在这里拒绝。
     * 无 host 数据搬运的 multi-plane 命令（multi-plane 擦除 / 纯加载）按契约**不建
     * desc 链**（见 fc_build_dispatch_request 的对应分支），行地址由 cmd->paa 逐 plane
     * 派生，没有 desc 可作覆盖判据；此处只校验下面通用链检查已覆盖的平面跨度。
     * 单 plane：链只能落在 paa_to_plane(cmd->paa) 那一个 plane，且行地址必须与
     * cmd->paa 一致（block/page/lun 三项）。
     */
    if (cmd->config.multi_plane > 0U)
    {
        if (nand_operation_has_data(cmd->config.operation) != 0U)
        {
            const BYTE all_planes = (BYTE)((1U << cmd->config.multi_plane) - 1U);

            if ((planes_with_desc & all_planes) != all_planes)
            {
                return false;
            }
        }
    }
    else if (planes_with_desc != 0U)
    {
        if (planes_with_desc != (1U << plane_first))
        {
            return false;
        }
        if ((paa_to_block(plane_row[plane_first]) != paa_to_block(cmd->paa)) ||
            (paa_to_page(plane_row[plane_first]) != paa_to_page(cmd->paa)) ||
            (paa_to_lun(plane_row[plane_first]) != paa_to_lun(cmd->paa)))
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 释放命令：沿链释放 desc+buffer，命令槽位回 FREE（FREE 幂等）
 *
 * 调用链保证只在「未提交」或「完成项已取走」后调用；在飞释放是调用者 bug，
 * debug 下 ASSERT。release 下由 FTL 的 submitted/drain 契约保证。
 *
 * @param controller FC 控制器
 * @param cmd_id 命令 ID
 */
void fc_pool_command_release(fc_ctl_s *controller, fc_cmd_id_t cmd_id)
{
    ASSERT(controller != NULL);
    ASSERT_REJECT_VOID(cmd_id < FC_COMMAND_POOL_DEPTH,
                       "cmd release: id=%hu out of range",
                       cmd_id);

    fc_cmd_s      *cmd   = &controller->command_pool[cmd_id];
    fc_cmd_state_e state = controller->command_state[cmd_id];
    ASSERT(cmd->cmd_id == cmd_id);

    if (state == FC_COMMAND_STATE_FREE)
    {
        return;
    }
    /* 在飞/排队命令不允许释放；必须在碰 desc 链/buffer 之前拒绝。 */
    ASSERT_REJECT_VOID((state == FC_COMMAND_STATE_ALLOCATED) || (state == FC_COMMAND_STATE_COMPLETED),
                       "cmd release: cmd=%hu state=%d",
                       cmd_id,
                       state);

    // 沿链释放 desc + buffer（buffer 随链一并归还）。
    // desc_id 是位域取不了地址，故按值传入；命令体随后整体清零。
    fc_pool_chain_release_all(controller, (fc_desc_id_t)cmd->desc_id);

    ASSERT(controller->command_used > 0U);
    memset(&controller->command_pool[cmd_id], 0, sizeof(controller->command_pool[cmd_id]));
    /* memset 会清掉 cmd_id；槽位号在 FREE 状态仍保留，是否可操作由 command_state 判定。 */
    controller->command_pool[cmd_id].cmd_id = cmd_id;
    fc_pool_command_state_set(controller, cmd_id, FC_COMMAND_STATE_FREE);
    controller->command_free_next[cmd_id] = controller->command_free_head;
    controller->command_free_head         = cmd_id;
    controller->command_used--;
}

/**
 * @brief 把已填好的 desc 追加到命令链尾
 *
 * 空链即链头；前驱 chain 置 MORE，末条保持 END。调用者保证 desc 自洽。
 *
 * @param controller FC 控制器
 * @param cmd_id 命令 ID
 * @param descriptor 待挂 desc
 */
static void fc_pool_chain_append(fc_ctl_s *controller, fc_cmd_id_t cmd_id, fc_desc_s *descriptor)
{
    ASSERT(cmd_id < FC_COMMAND_POOL_DEPTH);

    fc_cmd_s          *cmd    = &controller->command_pool[cmd_id];
    const fc_desc_id_t new_id = (fc_desc_id_t)(descriptor - controller->descriptor_pool);
    ASSERT(cmd->cmd_id == cmd_id);
    fc_desc_id_t tail = (fc_desc_id_t)cmd->desc_id;

    if (tail == FC_DESC_ID_INVALID)
    {
        cmd->desc_id = new_id; /* 空链：本条即链头 */
        return;
    }
    ASSERT_REJECT_VOID(tail < FC_DESCRIPTOR_POOL_DEPTH, "chain append: head=%hu out of range", tail);

    WORD steps = 0U;
    while ((controller->descriptor_pool[tail].next_id != FC_DESC_ID_INVALID) && (steps < FC_DESCRIPTOR_POOL_DEPTH))
    {
        tail = (fc_desc_id_t)controller->descriptor_pool[tail].next_id;
        steps++;
    }
    ASSERT_REJECT_VOID(controller->descriptor_pool[tail].next_id == FC_DESC_ID_INVALID,
                       "chain append: desc=%hu is not tail",
                       tail);
    controller->descriptor_pool[tail].config.chain = 1U;
    controller->descriptor_pool[tail].next_id      = new_id;
}

/**
 * @brief 按分配类申请命令槽位（清零 + desc_id=INVALID + 写入槽位号）
 *
 * 类决定当前池占用上限：其余路径不能吃掉 L2P demand load 的 CRIT 预留槽。
 *
 * @param cls 分配类（FC_CMD_CLASS_*）
 * @return 命令指针；该类额度用完时返回 NULL
 */
fc_cmd_s *fc_command_alloc_class(fc_cmd_class_e cls)
{
    fc_ctl_s   *controller = &g_fc_controller;
    fc_cmd_id_t index;

    ASSERT(cls < FC_CMD_CLASS_NUM);

    s_alloc_class = cls;

    DWORD limit = FC_COMMAND_POOL_DEPTH;
    if (cls == FC_CMD_CLASS_BG)
    {
        limit -= FC_CMD_RESERVE_CRIT;
    }

    if (controller->command_used >= limit)
    {
        if (cls == FC_CMD_CLASS_BG)
        {
            STATS_INC(s_stats_fc_alloc.cmd_fail_bg);
        }
        else
        {
            STATS_INC(s_stats_fc_alloc.cmd_fail_crit);
        }
        return NULL;
    }

    index = controller->command_free_head;
    if (index == FC_CMD_ID_INVALID)
    {
        if (cls == FC_CMD_CLASS_BG)
        {
            STATS_INC(s_stats_fc_alloc.cmd_fail_bg);
        }
        else
        {
            STATS_INC(s_stats_fc_alloc.cmd_fail_crit);
        }
        return NULL;
    }
    ASSERT_REJECT(controller->command_state[index] == FC_COMMAND_STATE_FREE, NULL, "cmd alloc: cmd=%hu state=%d", index, controller->command_state[index]);
    controller->command_free_head = controller->command_free_next[index];
    memset(&controller->command_pool[index], 0, sizeof(controller->command_pool[index]));
    controller->command_pool[index].desc_id = FC_DESC_ID_INVALID;
    controller->command_pool[index].cmd_id  = index;
    fc_pool_command_state_set(controller, index, FC_COMMAND_STATE_ALLOCATED);
    controller->command_used++;
    if (controller->command_used > controller->command_high)
    {
        controller->command_high = controller->command_used;
    }

    return &controller->command_pool[index];
}

/**
 * @brief 分配命令槽位（满池额度；测试/兼容入口）
 *
 * 生产 FTL 一律走 fc_command_alloc_class() 的 BG/CRIT 额度；本入口保留给 FC 测试
 * 与 RAID parity 内部命令，行为与引入额度前一致（可用满 DEPTH）。
 *
 * @return 命令指针；池空时返回 NULL
 */
fc_cmd_s *fc_command_alloc(void)
{
    return fc_command_alloc_class(FC_CMD_CLASS_CRIT);
}

/**
 * @brief 分配一个 buffer
 *
 * @return buffer ID；FC_BUF_ID_INVALID = 池空
 */
fc_buf_id_t fc_buffer_alloc(void)
{
    fc_ctl_s *controller = &g_fc_controller;
    WORD      buffer_index;

    WORD limit = FC_BUFFER_POOL_DEPTH;
    if (s_alloc_class == FC_CMD_CLASS_BG)
    {
        limit -= FC_CMD_RESERVE_CRIT_BUFS;
    }
    if (controller->buffer_used >= limit)
    {
        STATS_INC(s_stats_fc_alloc.buf_fail);
        return FC_BUF_ID_INVALID;
    }

    buffer_index = controller->buffer_free_head;
    if (buffer_index == FC_BUF_ID_INVALID)
    {
        STATS_INC(s_stats_fc_alloc.buf_fail);
        return FC_BUF_ID_INVALID;
    }
    ASSERT_REJECT(controller->buffer_state[buffer_index] == FC_BUFFER_STATE_FREE, FC_BUF_ID_INVALID, "buffer alloc: id=%hu state=%d", buffer_index, controller->buffer_state[buffer_index]);
    controller->buffer_free_head           = controller->buffer_free_next[buffer_index];
    controller->buffer_state[buffer_index] = FC_BUFFER_STATE_OCCUPIED;
    controller->buffer_owner[buffer_index] = FC_CMD_ID_INVALID;
    controller->buffer_used++;
    if (controller->buffer_used > controller->buffer_high)
    {
        controller->buffer_high = controller->buffer_used;
    }
    return buffer_index;
}

/**
 * @brief 建链入口：分配 desc、填 PAA/config/meta、挂链、按需分配 buffer
 *
 * 失败不回滚：已分配的 desc 都在链上，调用方沿链释放即完整回收。合法
 * 失败是资源池空（RESOURCE_EMPTY）；命令 ID 越界或状态不对则 debug ASSERT、
 * release 返回 INVALID_ARGUMENT。
 *
 * @param cmd_id 命令 ID（ALLOCATED 且 plane 段/掩码已设好）
 * @param plane     plane 号
 * @param cw        CW 号
 * @param row_paa   该 plane 的行地址（PAA）
 * @param buffer_id 上层申请的 buffer（buf_own 默认路径传 INVALID）
 * @return FC_STATUS_OK；FC_STATUS_RESOURCE_EMPTY desc/buffer 池空；
 *         FC_STATUS_INVALID_ARGUMENT 命令 ID 越界 / 命令不在 ALLOCATED /
 *         上层 buffer ID 越界或归属不符
 */
fc_status_e fc_command_configure_descriptor_with_buffer(fc_cmd_id_t cmd_id, BYTE plane, BYTE cw, DWORD row_paa, fc_buf_id_t buffer_id)
{
    fc_desc_s  *descriptor;
    fc_buf_id_t alloc_id;
    DWORD       plane_cw;
#ifndef NDEBUG
    BYTE plane_first;
    BYTE plane_last;
#endif

    /* ── 校验段：不得触碰任何池 ──
     * 资源申请不到（desc / buffer 池空）→ RESOURCE_EMPTY，是模仿硬件的预期失败。
     * ID 越界、生命周期不对、上层 buffer 越界/归属不符：debug ASSERT；
     * release 拒绝并返回 INVALID_ARGUMENT（判据 9 闸门，避免把坏 owner/越界 ID
     * 写进 desc）。坐标错误只在 debug 检查，提交边界 fc_pool_chain_validate 兜底。 */
    ASSERT_REJECT(cmd_id < FC_COMMAND_POOL_DEPTH, FC_STATUS_INVALID_ARGUMENT, "configure descriptor: cmd=%hu out of range", cmd_id);
    ASSERT_REJECT(g_fc_controller.command_state[cmd_id] == FC_COMMAND_STATE_ALLOCATED,
                  FC_STATUS_INVALID_ARGUMENT,
                  "configure descriptor: cmd=%hu state=%d not ALLOCATED",
                  cmd_id,
                  g_fc_controller.command_state[cmd_id]);

    fc_cmd_s *cmd = &g_fc_controller.command_pool[cmd_id];
    ASSERT(cmd->cmd_id == cmd_id);

#ifndef NDEBUG
    (void)fc_command_plane_span(cmd, &plane_first, &plane_last);
    ASSERT((cw < CW_NUM) && (plane >= plane_first) && (plane < plane_last));
    ASSERT(paa_to_plane(row_paa) == plane);
#endif

    if ((cmd->config.buf_own & FC_BUF_OWN_ALLOC_BY_UPPER) != 0U)
    {
        ASSERT_REJECT((buffer_id != FC_BUF_ID_INVALID) && (buffer_id < FC_BUFFER_POOL_DEPTH),
                      FC_STATUS_INVALID_ARGUMENT,
                      "configure descriptor: upper buffer id=%hu out of range",
                      buffer_id);
        ASSERT_REJECT(g_fc_controller.buffer_state[buffer_id] == FC_BUFFER_STATE_OCCUPIED,
                      FC_STATUS_INVALID_ARGUMENT,
                      "configure descriptor: buffer id=%hu not occupied",
                      buffer_id);
        ASSERT_REJECT(g_fc_controller.buffer_owner[buffer_id] == FC_CMD_ID_INVALID,
                      FC_STATUS_INVALID_ARGUMENT,
                      "configure descriptor: buffer id=%hu already owned by cmd=%hu",
                      buffer_id,
                      g_fc_controller.buffer_owner[buffer_id]);
    }
    else
    {
        ASSERT_REJECT(buffer_id == FC_BUF_ID_INVALID,
                      FC_STATUS_INVALID_ARGUMENT,
                      "configure descriptor: buffer id must be INVALID for FC-owned path");
    }
    /* ── 机制段 ── */
    descriptor = fc_pool_descriptor_alloc(&g_fc_controller, cmd_id);
    if (descriptor == NULL)
    {
        return FC_STATUS_RESOURCE_EMPTY;
    }

    const fc_cmd_id_t owner = cmd_id;

    // 先把除 buffer_id 外的字段全部填好，再挂链：desc 一进链就能被任何
    // 走链的读者正确解释（paa/config/meta 均已有效，next_id 已终止）。

    // 两条公式：PAA 与 LBA 的 plane/cw 位置不同，不能再用同一条 plane_cw 公式。
    // desc->paa  —— PAA 空间：plane 与 cw 不相邻，用 paa_with_plane/paa_with_cw 逐字段换；
    // 行地址取自实参 row_paa（每 plane 独立，Policy B 两 plane 可不同块）。
    // desc->meta —— LBA 空间：plane/cw 低 3 位相邻（PLANE_CW_MASK 有效），
    // 高位取自 cmd->meta（本命令扇区 LBA）。
    plane_cw             = (plane << PLANE_CW_SHIFT) | cw;
    descriptor->paa      = paa_with_cw(paa_with_plane(row_paa, plane), cw);
    descriptor->config   = cmd->config; /* 命令级配置副本；错误恢复时原样回填 */
    descriptor->cw_index = cw;
    descriptor->next_id  = FC_DESC_ID_INVALID;
    descriptor->meta     = (cmd->meta & ~PLANE_CW_MASK) | plane_cw;

    fc_pool_chain_append(&g_fc_controller, cmd_id, descriptor); /* 出池即挂链 */

    // 上层申请（buf_own 10/11）：buffer 由调用者 fc_buffer_alloc 好并传进来，无失败路径。
    if ((cmd->config.buf_own & FC_BUF_OWN_ALLOC_BY_UPPER) != 0U)
    {
        descriptor->buffer_id                   = buffer_id;
        g_fc_controller.buffer_owner[buffer_id] = owner;
        return FC_STATUS_OK;
    }

    // FC 申请（默认 00/01）：buffer 池比 desc 池小，这里是最现实的失败点。失败**不回滚**：
    // desc 已在链上、buffer_id 保持 INVALID，调用方沿链释放时自然跳过 buffer、回收 desc。
    alloc_id = fc_buffer_alloc();
    if (alloc_id == FC_BUF_ID_INVALID)
    {
        return FC_STATUS_RESOURCE_EMPTY;
    }
    descriptor->buffer_id                  = alloc_id;
    g_fc_controller.buffer_owner[alloc_id] = owner;
    return FC_STATUS_OK;
}

/**
 * @brief 建链入口（FC 申请 buffer 的默认路径）
 *
 * @param cmd_id 命令 ID
 * @param plane   plane 号
 * @param cw      CW 号
 * @param row_paa 该 plane 的行地址
 * @return FC_STATUS_OK；FC_STATUS_RESOURCE_EMPTY 池空；
 *         FC_STATUS_INVALID_ARGUMENT 命令 ID 越界
 */
fc_status_e fc_command_configure_descriptor(fc_cmd_id_t cmd_id, BYTE plane, BYTE cw, DWORD row_paa)
{
    return fc_command_configure_descriptor_with_buffer(cmd_id, plane, cw, row_paa, FC_BUF_ID_INVALID);
}

/**
 * @brief 释放链头一个 desc（含其 buffer），返回新链头
 *
 * @param desc_id 链头描述符 ID；必须是链头
 * @return 新链头；FC_DESC_ID_INVALID = 链已空
 */
fc_desc_id_t fc_descriptor_release_head(fc_desc_id_t desc_id)
{
    return fc_pool_chain_release_head(&g_fc_controller, desc_id);
}

/**
 * @brief 沿链释放全部 desc + buffer
 *
 * 建链中途失败的回滚入口。
 *
 * @param head 链头 ID
 * @return 恒为 FC_DESC_ID_INVALID
 */
fc_desc_id_t fc_descriptor_chain_release_all(fc_desc_id_t head)
{
    fc_pool_chain_release_all(&g_fc_controller, head);
    return FC_DESC_ID_INVALID;
}

/**
 * @brief 释放命令槽位（FREE 幂等）
 *
 * @note ID 越界/在飞释放由 fc_pool_command_release 的 release 守卫拒绝；
 *       本函数只负责单例入口。
 *
 * @param cmd_id 命令 ID；须为池内槽位。
 */
void fc_command_release(fc_cmd_id_t cmd_id)
{
    fc_pool_command_release(&g_fc_controller, cmd_id);
}

/**
 * @brief 释放上层预申请的 buffer（尚未挂到命令上）
 *
 * @note 越界/未占用/仍归属命令由 fc_pool_buffer_release 的 release 守卫拒绝；
 *       本函数只负责单例入口。
 *
 * @param buffer_id buffer id
 */
void fc_buffer_release(fc_buf_id_t buffer_id)
{
    fc_pool_buffer_release(&g_fc_controller, buffer_id, FC_CMD_ID_INVALID);
}

/**
 * @brief buffer id → USER 数据区指针（跳过 META+CRC）
 *
 * 前置：buffer_id 须已占用；成功时恒非空。
 *
 * @param buffer_id buffer id
 * @return USER 数据区指针
 */
BYTE *fc_buffer_get_user_data(fc_buf_id_t buffer_id)
{
    BYTE *base;

    ASSERT(buffer_id < FC_BUFFER_POOL_DEPTH);
    ASSERT(g_fc_controller.buffer_state[buffer_id] == FC_BUFFER_STATE_OCCUPIED);
    base = &g_fc_controller.buffer_pool[buffer_id][0];

    // 对外只暴露 USER 数据区，跳过 META(4) + CRC(4)
    return base + FC_BUFFER_USER_OFFSET;
}

/**
 * @brief buffer id → CW 基址（528B，含 META/CRC/PARITY）
 *
 * 前置：buffer_id 须已占用；成功时恒非空。
 *
 * @param buffer_id buffer id
 * @return CW 基址
 */
BYTE *fc_buffer_get_base(fc_buf_id_t buffer_id)
{
    ASSERT(buffer_id < FC_BUFFER_POOL_DEPTH);
    ASSERT(g_fc_controller.buffer_state[buffer_id] == FC_BUFFER_STATE_OCCUPIED);
    return &g_fc_controller.buffer_pool[buffer_id][0];
}

/**
 * @brief 返回命令中 CW 号 == cw_index 的 CW 的数据指针
 *
 * 按 paa_to_cw 匹配，不是链走位（flat CW 掩码下两者会分叉）。
 *
 * @param cmd_id 命令 ID
 * @param cw_index CW 号
 * @return 数据指针；未找到 / 无 buffer 返回 NULL
 */
BYTE *fc_get_cmd_data_ptr(fc_cmd_id_t cmd_id, BYTE cw_index)
{
    fc_desc_id_t desc_id;

    // cmd/desc 链非空是前置条件；cw_index 不在链上仍按查询语义返回 NULL
    const fc_cmd_s *cmd = fc_command_id_to_command(cmd_id);
    ASSERT(cmd->desc_id != FC_DESC_ID_INVALID);

    /* 同 fc_buffer_get_meta：cw_index 不带 plane，multi-plane 下无法唯一定位；
     * release 返回 NULL 而不是静默交出错 plane 的数据。 */
    if (cmd->config.multi_plane > 0U)
    {
        ASSERT(cmd->config.multi_plane == 0U);
        return NULL;
    }

    desc_id = (fc_desc_id_t)cmd->desc_id;
    for (WORD steps = 0U; (desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH); steps++)
    {
        fc_desc_s *descriptor = fc_descriptor_id_to_descriptor(desc_id);

        if (paa_to_cw(descriptor->paa) == cw_index)
        {
            if (descriptor->buffer_id == FC_BUF_ID_INVALID)
            {
                return NULL;
            }
            return fc_buffer_get_user_data(descriptor->buffer_id);
        }
        desc_id = (fc_desc_id_t)descriptor->next_id;
    }

    return NULL;
}

/**
 * @brief 返回命令中 CW 号 == cw_index 的 CW 的 META 指针
 *
 * 按 paa_to_cw(desc->paa) 匹配，不是链走位。
 *
 * @param cmd_id 命令 ID
 * @param cw_index CW 号
 * @return META 指针；未找到 / 无 buffer 返回 NULL
 */
BYTE *fc_buffer_get_meta(fc_cmd_id_t cmd_id, BYTE cw_index)
{
    fc_desc_id_t did;

    // cmd_id 是池内句柄；未找到 CW 仍按契约返回 NULL
    const fc_cmd_s *cmd = fc_command_id_to_command(cmd_id);

    /* cw_index 不带 plane：multi-plane 命令同一 CW 号在两个 plane 各有一条 desc，
     * 本函数只能返回链序第一个匹配。release 下宁可返回 NULL，也不静默交出错 plane
     * 的数据；multi-plane 逐 CW 读取请沿链遍历（见 ftl_cmd_read_data）。 */
    if (cmd->config.multi_plane > 0U)
    {
        ASSERT(cmd->config.multi_plane == 0U);
        return NULL;
    }

    did = (fc_desc_id_t)cmd->desc_id;
    for (WORD steps = 0U; (did < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH); steps++)
    {
        fc_desc_s *d = fc_descriptor_id_to_descriptor(did);

        if (paa_to_cw(d->paa) == cw_index)
        {
            return (d->buffer_id != FC_BUF_ID_INVALID) ? &g_fc_controller.buffer_pool[d->buffer_id][0] : NULL;
        }
        did = (fc_desc_id_t)d->next_id;
    }
    return NULL;
}

/**
 * @brief 按 desc 链重建命令的 flat CW 位图
 *
 * 链是 CW 集合的唯一真值；须在 fc_command_release 之前调用。
 *
 * @param cmd_id 命令 ID
 * @return flat 位图（bit = plane*CW_NUM + cw）
 */
DWORD fc_get_cmd_cw_bitmap(fc_cmd_id_t cmd_id)
{
    fc_desc_id_t desc_id;
    DWORD        mask = 0U;

    const fc_cmd_s *cmd = fc_command_id_to_command(cmd_id);

    desc_id = (fc_desc_id_t)cmd->desc_id;
    for (WORD steps = 0U; (desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH); steps++)
    {
        const fc_desc_s *descriptor = fc_descriptor_id_to_descriptor(desc_id);
        const BYTE       plane      = paa_to_plane(descriptor->paa);
        const BYTE       n          = fc_cw_flat(plane, paa_to_cw(descriptor->paa));

        // 与 fc_build_request_data_planes 的同源断言一致：链上的 CW 编号必须在 flat 空间内
        ASSERT(n < CW_TOTAL_MAX);
        mask |= (1U << n);
        desc_id = (fc_desc_id_t)descriptor->next_id;
    }
    return mask;
}

/**
 * @brief 命令读到的所有 CW 是否都是擦除页
 *
 * 空链按「非空」处理（没有 CW 就没有证据）；只对读命令 + 已完成命令有意义。
 *
 * @param cmd_id 命令 ID
 * @return true 全部 CW 都是 EMPTY；false 否则
 */
BOOL fc_get_cmd_all_cw_empty(fc_cmd_id_t cmd_id)
{
    fc_desc_id_t desc_id;
    WORD         n = 0U;

    const fc_cmd_s *cmd = fc_command_id_to_command(cmd_id);

    desc_id = (fc_desc_id_t)cmd->desc_id;
    while ((desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (n < CW_TOTAL_MAX))
    {
        const fc_desc_s *descriptor = fc_descriptor_id_to_descriptor(desc_id);

        if (fc_cw_status_is_empty(descriptor->status) == false)
        {
            return false;
        }
        n++;
        desc_id = (fc_desc_id_t)descriptor->next_id;
    }

    return (n > 0U);
}

/**
 * @brief 命令 desc 链上最大的 ECC bit 数
 *
 * 须在 fc_command_release 之前调用。
 *
 * @param cmd_id 命令 ID
 * @return 最大 ECC bit 数；空链为 0
 */
BYTE fc_get_cmd_ecc_max(fc_cmd_id_t cmd_id)
{
    fc_desc_id_t desc_id;
    BYTE         ecc_max = 0U;

    const fc_cmd_s *cmd = fc_command_id_to_command(cmd_id);

    desc_id = (fc_desc_id_t)cmd->desc_id;
    for (WORD steps = 0U; (desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH); steps++)
    {
        const fc_desc_s *descriptor = fc_descriptor_id_to_descriptor(desc_id);

        if (descriptor->ecc_bits > ecc_max)
        {
            ecc_max = descriptor->ecc_bits;
        }
        desc_id = (fc_desc_id_t)descriptor->next_id;
    }
    return ecc_max;
}

/**
 * @brief 命令 desc 链上是否存在 UNC（不可纠 ECC）CW
 *
 * 须在 fc_command_release 之前调用。
 *
 * @param cmd_id 命令 ID
 * @return true = 至少一个 CW 为 UNC
 */
BOOL fc_get_cmd_has_unc(fc_cmd_id_t cmd_id)
{
    fc_desc_id_t desc_id;

    const fc_cmd_s *cmd = fc_command_id_to_command(cmd_id);

    desc_id = (fc_desc_id_t)cmd->desc_id;
    for (WORD steps = 0U; (desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH); steps++)
    {
        const fc_desc_s *descriptor = fc_descriptor_id_to_descriptor(desc_id);

        if (fc_cw_status_has_unc(descriptor->status) != false)
        {
            return true;
        }
        desc_id = (fc_desc_id_t)descriptor->next_id;
    }
    return false;
}

/* ── ID / 指针转换 ── */

/**
 * @brief 命令 ID → 命令指针
 *
 * @param cmd_id 池内命令 ID
 * @return 命令指针
 */
fc_cmd_s *fc_command_id_to_command(fc_cmd_id_t cmd_id)
{
    // cmd_id 只来自 fc_command_alloc() 铸出的 ID（队列里只存分配产出的 ID），
    // 任何调用者都不可能传入越界值——越界即不变量破坏，留现场。
    ASSERT(cmd_id < FC_COMMAND_POOL_DEPTH);
    ASSERT(g_fc_controller.command_pool[cmd_id].cmd_id == cmd_id);
    return &g_fc_controller.command_pool[cmd_id];
}

/**
 * @brief 描述符 ID → 描述符指针
 *
 * @param descriptor_id 池内描述符 ID
 * @return 描述符指针
 */
fc_desc_s *fc_descriptor_id_to_descriptor(fc_desc_id_t descriptor_id)
{
    // descriptor_id 只来自命令 desc 链（configure_descriptors 铸出），恒在池内。
    ASSERT(descriptor_id < FC_DESCRIPTOR_POOL_DEPTH);
    return &g_fc_controller.descriptor_pool[descriptor_id];
}

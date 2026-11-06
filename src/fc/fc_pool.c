/*
 * fc_pool.c — FC 三池：命令 / 描述符 / 缓冲
 *
 * 职责：
 *   - 三池本体与簿记数组（命令状态、desc 归属、buffer 归属/状态）；
 *   - 空闲链表与 O(1) 计数；
 *   - 命令/描述符/缓冲的分配、释放、链遍历；
 *   - 池相关的公开 API（建链、命令释放、buffer 访问、链查询）。
 *
 * 提交/填充/完成调度在 fc.c；两文件共享的定义见 fc_internal.h。
 */

#include "assertion.h"
#include "fc_internal.h"

#include <stdint.h>
#include <string.h>

/* ── 三池本体 ──
 * 文件作用域 static，地址在固件生命周期内固定；fc_ctl_s 只存指针。
 * buffer 固定在 noncacheable 段：FMC 数据面按字节读写，避免 CPU D-Cache
 * 与总线视图不一致；也保证地址落在 DMA 可达区（DMA1/2 访问不到 DTCM）。 */
static fc_cmd_s           g_fc_command_pool[FC_COMMAND_POOL_DEPTH];
static fc_desc_s          g_fc_descriptor_pool[FC_DESCRIPTOR_POOL_DEPTH];
static fc_buffer_cw_arr_t g_fc_buffer_pool[FC_BUFFER_POOL_DEPTH] __attribute__((section(".noncacheable_buffer")));
static fc_cmd_id_t        g_fc_descriptor_owner[FC_DESCRIPTOR_POOL_DEPTH];
static fc_cmd_state_e     g_fc_command_state[FC_COMMAND_POOL_DEPTH];
static fc_cmd_id_t        g_fc_command_free_next[FC_COMMAND_POOL_DEPTH];
static fc_desc_id_t       g_fc_descriptor_free_next[FC_DESCRIPTOR_POOL_DEPTH];
static fc_buf_id_t        g_fc_buffer_free_next[FC_BUFFER_POOL_DEPTH];
static fc_buf_state_e     g_fc_buffer_state[FC_BUFFER_POOL_DEPTH];
static fc_cmd_id_t        g_fc_buffer_owner[FC_BUFFER_POOL_DEPTH];

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
        controller->command_free_next[index] = ((index + 1U) < FC_COMMAND_POOL_DEPTH) ? (fc_cmd_id_t)(index + 1U) : FC_CMD_ID_INVALID;
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
}

/**
 * @brief 读命令状态
 *
 * ID 越界即不变量破坏 ⇒ ASSERT。
 *
 * @param controller FC 控制器
 * @param command_id 池内命令 ID
 * @return 命令状态
 */
fc_cmd_state_e fc_command_state_get(const fc_ctl_s *controller, fc_cmd_id_t command_id)
{
    ASSERT(command_id < FC_COMMAND_POOL_DEPTH);
    return controller->command_state[command_id];
}

/**
 * @brief 读 desc 的归属命令 ID
 *
 * @param controller    FC 控制器
 * @param descriptor_id 池内 desc ID
 * @return 归属命令 ID；FC_CMD_ID_INVALID = 空闲
 *
 * @note 供 fc.c 的 debug 全池校验使用；release 档无读者，但保持外部链接
 *       （跨 fc_pool.c ↔ fc.c），不会触发 -Wunused-function。
 */
fc_cmd_id_t fc_descriptor_owner_get(const fc_ctl_s *controller, WORD descriptor_id)
{
    ASSERT(descriptor_id < FC_DESCRIPTOR_POOL_DEPTH);
    return controller->descriptor_owner[descriptor_id];
}

/**
 * @brief 读 buffer 占用状态
 *
 * @param controller FC 控制器
 * @param buffer_id  池内 buffer ID
 * @return buffer 状态
 */
fc_buf_state_e fc_buffer_state_get(const fc_ctl_s *controller, fc_buf_id_t buffer_id)
{
    ASSERT(buffer_id < FC_BUFFER_POOL_DEPTH);
    return controller->buffer_state[buffer_id];
}

/**
 * @brief 命令 ID → 命令指针
 *
 * @param controller FC 控制器
 * @param command_id 池内命令 ID
 * @return 命令指针
 */
static fc_cmd_s *fc_pool_command_by_id(fc_ctl_s *controller, WORD command_id)
{
    // 公共壳已断言过 ID 范围；内部 helper 直接取池项。
    return &controller->command_pool[command_id];
}

/**
 * @brief 命令指针 → 池索引（指针 − 池基址）
 *
 * 前置：command 属于本控制器命令池。
 *
 * @param controller FC 控制器
 * @param command    命令指针
 * @return 池索引
 */
fc_cmd_id_t fc_pool_command_id(const fc_ctl_s *controller, const fc_cmd_s *command)
{
    ASSERT(controller != NULL);
    ASSERT(command != NULL);

    return (fc_cmd_id_t)(command - controller->command_pool);
}

/**
 * @brief 描述符 ID → 描述符指针
 *
 * @param controller    FC 控制器
 * @param descriptor_id 池内 desc ID
 * @return 描述符指针
 */
static fc_desc_s *fc_pool_descriptor_by_id(fc_ctl_s *controller, WORD descriptor_id)
{
    // 公共壳已断言过 ID 范围；内部 helper 直接取池项。
    return &controller->descriptor_pool[descriptor_id];
}

/**
 * @brief buffer ID → CW 基址（528B，不做 USER 偏移）
 *
 * @param controller FC 控制器
 * @param buffer_id  池内 buffer ID
 * @return CW 基址
 */
static BYTE *fc_pool_buffer_cw_base(fc_ctl_s *controller, fc_buf_id_t buffer_id)
{
    // 公共壳已断言过 ID 范围；内部 helper 直接取池项。
    return controller->buffer_pool[buffer_id];
}

/**
 * @brief 命令状态唯一 setter：只允许状态机定义的转移。
 *
 * @param controller FC 控制器
 * @param command_id 命令池槽位号
 * @param next       目标状态
 *
 * @note 非法转移在 debug 下 ASSERT；release 下仍写入目标值（调用链已保证前置，
 *       这里只收口状态字段的唯一写点，避免多处直接赋值漂移）。
 */
void fc_pool_command_state_set(fc_ctl_s *controller, fc_cmd_id_t command_id, fc_cmd_state_e next)
{
    const fc_cmd_state_e current = controller->command_state[command_id];
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
    ASSERT(allowed);
    (void)allowed; /* NDEBUG 下 ASSERT 裁空，显式标记仅用于断言 */
    controller->command_state[command_id] = next;
}

/**
 * @brief 分配 desc 并登记归属（不挂链）
 *
 * 出池即自洽：next_id/buffer_id 显式置哨兵。
 *
 * @param controller FC 控制器
 * @param command    归属命令
 * @return desc 指针；NULL = desc 池空
 */
static fc_desc_s *fc_pool_descriptor_alloc(fc_ctl_s *controller, fc_cmd_s *command)
{
    WORD        descriptor_index;
    fc_cmd_id_t command_id;

    command_id       = fc_pool_command_id(controller, command);
    descriptor_index = controller->descriptor_free_head;
    if (descriptor_index == FC_DESC_ID_INVALID)
    {
        return NULL;
    }
    ASSERT_REJECT(controller->descriptor_owner[descriptor_index] == FC_CMD_ID_INVALID, NULL, "descriptor alloc: desc=%hu owner=%hu", descriptor_index, controller->descriptor_owner[descriptor_index]);
    controller->descriptor_free_head = controller->descriptor_free_next[descriptor_index];
    memset(&controller->descriptor_pool[descriptor_index], 0, sizeof(controller->descriptor_pool[descriptor_index]));
    controller->descriptor_pool[descriptor_index].next_id   = FC_DESC_ID_INVALID;
    controller->descriptor_pool[descriptor_index].buffer_id = FC_BUF_ID_INVALID;
    controller->descriptor_owner[descriptor_index]          = command_id;
    controller->descriptor_used++;
    if (controller->descriptor_used > controller->descriptor_high)
    {
        controller->descriptor_high = controller->descriptor_used;
    }
    return &controller->descriptor_pool[descriptor_index];
}

/**
 * @brief 把描述符还回空闲链（清归属、清本体、头插空闲链）。
 *
 * @param controller FC 控制器
 * @param descriptor_index 池内下标。
 *
 * @note **不碰数据链、不碰 buffer**——只把 desc 本体还池。调用者负责
 * 先把该 desc 从链上摘掉（见 fc_pool_chain_release_head）。
 */
static void fc_pool_descriptor_release(fc_ctl_s *controller, WORD descriptor_index)
{
    ASSERT_REJECT_VOID(controller->descriptor_owner[descriptor_index] != FC_CMD_ID_INVALID, "descriptor release: desc=%hu already free", descriptor_index);
    ASSERT(controller->descriptor_used > 0U);
    memset(&controller->descriptor_pool[descriptor_index], 0, sizeof(controller->descriptor_pool[descriptor_index]));
    controller->descriptor_owner[descriptor_index]     = FC_CMD_ID_INVALID;
    controller->descriptor_free_next[descriptor_index] = controller->descriptor_free_head;
    controller->descriptor_free_head                   = descriptor_index;
    controller->descriptor_used--;
}

/**
 * @brief 释放 buffer（校验归属 → 状态清零 → 挂回空闲链表头）
 *
 * @param controller FC 控制器
 * @param buffer_id  池内 buffer ID
 * @param owner      期望归属：命令 ID；上层预申请未挂链时为 FC_CMD_ID_INVALID
 *
 * @return true = 已释放；false = 未占用或归属不符（release 档拒绝，空闲链保持完整）。
 */
static BOOL fc_pool_buffer_release(fc_ctl_s *controller, fc_buf_id_t buffer_id, fc_cmd_id_t owner)
{
    // release 档也必须先判下标，再访问数组（ASSERT 在 NDEBUG 下不存在）。
    ASSERT_REJECT(buffer_id < FC_BUFFER_POOL_DEPTH, false, "buffer release: id=%hu out of range", buffer_id);
    // 判据 9：归属不符/未占用时拒绝，避免同一节点二次入链造成静默介质破坏。
    ASSERT_REJECT((controller->buffer_state[buffer_id] == FC_BUFFER_STATE_OCCUPIED) && (controller->buffer_owner[buffer_id] == owner), false, "buffer release: id=%hu state=%d owner=%hu expected=%hu", buffer_id, controller->buffer_state[buffer_id], controller->buffer_owner[buffer_id], owner);
    ASSERT(controller->buffer_used > 0U);

    controller->buffer_state[buffer_id]     = FC_BUFFER_STATE_FREE;
    controller->buffer_owner[buffer_id]     = FC_CMD_ID_INVALID;
    controller->buffer_free_next[buffer_id] = controller->buffer_free_head;
    controller->buffer_free_head            = buffer_id;
    controller->buffer_used--;
    return true;
}

/**
 * @brief 释放链头一个 desc（含其 buffer），头指针推进到 next
 *
 * 先读 next_id 再释放（释放会整体清零）；无 buffer 的 desc 跳过 buffer 释放。
 *
 * @param controller FC 控制器
 * @param head       链头（进出参）
 */
static void fc_pool_chain_release_head(fc_ctl_s *controller, fc_desc_id_t *head)
{
    fc_desc_id_t descriptor_id;
    fc_buf_id_t  buffer_id;

    if (*head >= FC_DESCRIPTOR_POOL_DEPTH)
    {
        return;
    }
    descriptor_id           = *head;
    const fc_cmd_id_t owner = controller->descriptor_owner[descriptor_id];

    // 成环时链头会回到已经释放的 desc：owner 已清 INVALID。先断链再返回，
    // 避免把已清零的 buffer_id/next_id 当成有效数据继续释放。
    if (owner == FC_CMD_ID_INVALID)
    {
        *head = FC_DESC_ID_INVALID;
        ASSERT_MSG(owner != FC_CMD_ID_INVALID, "descriptor chain cycle at desc=%hu", descriptor_id);
        return;
    }

    buffer_id = (fc_buf_id_t)controller->descriptor_pool[descriptor_id].buffer_id;
    *head     = (fc_desc_id_t)controller->descriptor_pool[descriptor_id].next_id;

    if (buffer_id != FC_BUF_ID_INVALID)
    {
        const BOOL released = fc_pool_buffer_release(controller, buffer_id, owner);

        ASSERT(released);
        (void)released;
    }
    fc_pool_descriptor_release(controller, descriptor_id);
}

/**
 * @brief 沿链释放全部 desc + buffer，头指针置 INVALID
 *
 * @param controller FC 控制器
 * @param head       链头（进出参）
 */
static void fc_pool_chain_release_all(fc_ctl_s *controller, fc_desc_id_t *head)
{
    // 链在上界内仍可能成环；释放路径必须有界，不能靠调用者先校验。
    for (WORD steps = 0U; (*head < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH); steps++)
    {
        fc_pool_chain_release_head(controller, head);
    }
    ASSERT(*head == FC_DESC_ID_INVALID);
    *head = FC_DESC_ID_INVALID;
}

/**
 * @brief 沿命令的 desc 链归还**全部 buffer**，并把各 desc 的 buffer_id 抹成 FC_BUF_ID_INVALID（**幂等**：已 INVALID 的跳过）。
 *
 * @param controller FC 控制器
 * @param command 命令。
 *
 * @note 只动 buffer，**不动 desc 链、不动命令槽位**——它们仍由
 * fc_command_release 回收。唯一调用者：fc_complete_from_nand 在
 * 「buf_own 的释放位 = FC 释放（01/11）」时当场归还（此时完成项已组装完毕，
 * 之后没有任何读者碰 buffer）。抹 INVALID 是为了让随后的链释放自然跳过，
 * 从而不双重归还；命令若没走到完成就释放，buffer 仍有效，链释放照常归还
 * ⇒ 不泄漏。归属语义见 fc_buf_own_e。
 */
void fc_pool_command_release_buffers(fc_ctl_s *controller, fc_cmd_s *command)
{
    const fc_cmd_id_t command_id = fc_pool_command_id(controller, command);
    fc_desc_id_t      desc_id    = (fc_desc_id_t)command->desc_id;

    for (WORD steps = 0U; (desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH); steps++)
    {
        fc_desc_s  *descriptor = &controller->descriptor_pool[desc_id];
        fc_buf_id_t buffer_id  = (fc_buf_id_t)descriptor->buffer_id;

        if (buffer_id != FC_BUF_ID_INVALID)
        {
            const BOOL released = fc_pool_buffer_release(controller, buffer_id, command_id);

            ASSERT(released);
            if (released == true)
            {
                descriptor->buffer_id = FC_BUF_ID_INVALID;
            }
        }
        desc_id = (fc_desc_id_t)descriptor->next_id;
    }
    ASSERT(desc_id == FC_DESC_ID_INVALID);
}

/**
 * @brief 提交前的一次性 desc 链校验（归属 + 唯一性 + 坐标 + buffer 占用）。
 *
 * 这是建链/提交路径的**唯一**完整校验漏斗：逐 desc 的建链入口只保留逐项坐标断言，
 * 整链在提交前扫一次；不在每次 append 时做 O(n) 查重。
 *
 * @param controller FC 控制器
 * @param command_id 命令 ID
 * @param command    命令
 * @return true 链完整、归属一致、CW 不重复、buffer（若非 INVALID）已占用；false 否则
 *
 * @note 调用点只有 fc_submit / fc_submit_batch 的 ASSERT（release 档无消费者），
 *       释放档的完整性由 FTL 调用链保证（RISK-POOL-02）；保持外部链接，不标 unused。
 */
BOOL fc_pool_chain_validate(const fc_ctl_s *controller, fc_cmd_id_t command_id, const fc_cmd_s *command)
{
    fc_desc_id_t descriptor_id = (fc_desc_id_t)command->desc_id;
    DWORD        seen          = 0U;
    WORD         count         = 0U;
    BYTE         plane_first;
    BYTE         plane_last;

    (void)fc_command_plane_span(command, &plane_first, &plane_last);

    while (descriptor_id != FC_DESC_ID_INVALID)
    {
        if (descriptor_id >= FC_DESCRIPTOR_POOL_DEPTH)
        {
            return false;
        }
        if (controller->descriptor_owner[descriptor_id] != command_id)
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
        const DWORD      n          = (DWORD)plane * CW_NUM + cw;

        if ((plane < plane_first) || (plane >= plane_last) || (cw >= CW_NUM) || (n >= CW_TOTAL_MAX))
        {
            return false;
        }
        if ((seen & (1UL << n)) != 0UL)
        {
            return false;
        }
        seen |= (1UL << n);

        if (descriptor->buffer_id != FC_BUF_ID_INVALID)
        {
            if ((descriptor->buffer_id >= FC_BUFFER_POOL_DEPTH) || (controller->buffer_state[descriptor->buffer_id] == FC_BUFFER_STATE_FREE) || (controller->buffer_owner[descriptor->buffer_id] != command_id))
            {
                return false;
            }
        }

        count++;
        descriptor_id = (fc_desc_id_t)descriptor->next_id;
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
 * @param command    命令
 */
void fc_pool_command_release(fc_ctl_s *controller, fc_cmd_s *command)
{
    fc_cmd_id_t    command_id;
    fc_desc_id_t   head;
    fc_cmd_state_e state;

    command_id = fc_pool_command_id(controller, command);
    state      = controller->command_state[command_id];

    if (state == FC_COMMAND_STATE_FREE)
    {
        return;
    }
    ASSERT((state == FC_COMMAND_STATE_ALLOCATED) || (state == FC_COMMAND_STATE_COMPLETED));

    // 沿链释放 desc + buffer（buffer 随链一并归还）。
    // desc_id 是位域取不了地址，故用局部头变量中转。
    head = (fc_desc_id_t)command->desc_id;
    fc_pool_chain_release_all(controller, &head);

    ASSERT(controller->command_used > 0U);
    memset(&controller->command_pool[command_id], 0, sizeof(controller->command_pool[command_id]));
    fc_pool_command_state_set(controller, command_id, FC_COMMAND_STATE_FREE);
    controller->command_free_next[command_id] = controller->command_free_head;
    controller->command_free_head             = command_id;
    controller->command_used--;
}

/**
 * @brief 把已填好的 desc 追加到命令链尾
 *
 * 空链即链头；前驱 chain 置 MORE，末条保持 END。调用者保证 desc 自洽。
 *
 * @param controller FC 控制器
 * @param command    命令
 * @param descriptor 待挂 desc
 */
static void fc_pool_chain_append(fc_ctl_s *controller, fc_cmd_s *command, fc_desc_s *descriptor)
{
    const fc_desc_id_t new_id = (fc_desc_id_t)(descriptor - controller->descriptor_pool);
    fc_desc_id_t       tail   = (fc_desc_id_t)command->desc_id;

    if (tail == FC_DESC_ID_INVALID)
    {
        command->desc_id = new_id; /* 空链：本条即链头 */
        return;
    }
    ASSERT_REJECT_VOID(tail < FC_DESCRIPTOR_POOL_DEPTH, "chain append: head=%hu out of range", tail);

    WORD steps = 0U;
    while ((controller->descriptor_pool[tail].next_id != FC_DESC_ID_INVALID) && (steps < FC_DESCRIPTOR_POOL_DEPTH))
    {
        tail = (fc_desc_id_t)controller->descriptor_pool[tail].next_id;
        steps++;
    }
    ASSERT(controller->descriptor_pool[tail].next_id == FC_DESC_ID_INVALID);
    if (controller->descriptor_pool[tail].next_id != FC_DESC_ID_INVALID)
    {
        return;
    }
    controller->descriptor_pool[tail].config.chain = 1U;
    controller->descriptor_pool[tail].next_id      = new_id;
}

/**
 * @brief 是否占用（供 fc_buffer_release / get_user_data / get_base 的前置 ASSERT 用）。
 *
 * @param buffer_id 待检查的 buffer ID。
 *
 * @return true = 池内且已占用；false = 越界或未占用。
 *
 * @note 仅用于 ASSERT，故标 __attribute__((unused))（NDEBUG 下 ASSERT 被裁空，
 * 否则触发 -Wunused-function）。
 */
static BOOL __attribute__((unused)) fc_buffer_is_occupied(fc_buf_id_t buffer_id)
{
    return (buffer_id < FC_BUFFER_POOL_DEPTH) && (fc_buffer_state_get(&g_fc_controller, buffer_id) != FC_BUFFER_STATE_FREE);
}

/**
 * @brief 分配命令槽位（清零 + desc_id=INVALID）
 *
 * 不清 command_id——结构体无此字段，ID 由指针派生。
 *
 * @return 命令指针；NULL = 命令池空
 */
fc_cmd_s *fc_command_alloc(void)
{
    fc_ctl_s   *controller = &g_fc_controller;
    fc_cmd_id_t command_id;

    command_id = controller->command_free_head;
    if (command_id == FC_CMD_ID_INVALID)
    {
        return NULL;
    }
    ASSERT_REJECT(controller->command_state[command_id] == FC_COMMAND_STATE_FREE, NULL, "command alloc: cmd=%hu state=%d", command_id, controller->command_state[command_id]);
    controller->command_free_head = controller->command_free_next[command_id];
    memset(&controller->command_pool[command_id], 0, sizeof(controller->command_pool[command_id]));
    controller->command_pool[command_id].desc_id = FC_DESC_ID_INVALID;
    fc_pool_command_state_set(controller, command_id, FC_COMMAND_STATE_ALLOCATED);
    controller->command_used++;
    if (controller->command_used > controller->command_high)
    {
        controller->command_high = controller->command_used;
    }
    return &controller->command_pool[command_id];
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

    buffer_index = controller->buffer_free_head;
    if (buffer_index == FC_BUF_ID_INVALID)
    {
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
 * 失败不回滚：已分配的 desc 都在链上，调用方沿链释放即完整回收。唯一合法
 * 失败是资源池空（RESOURCE_EMPTY）。
 *
 * @param command   命令（ALLOCATED 且 plane 段/掩码已设好）
 * @param plane     plane 号
 * @param cw        CW 号
 * @param row_paa   该 plane 的行地址（PAA）
 * @param buffer_id 上层申请的 buffer（buf_own 默认路径传 INVALID）
 * @return FC_STATUS_OK；FC_STATUS_RESOURCE_EMPTY desc/buffer 池空
 */
fc_status_e fc_command_configure_descriptor_with_buffer(fc_cmd_s *command, BYTE plane, BYTE cw, DWORD row_paa, fc_buf_id_t buffer_id)
{
    BYTE        plane_first;
    BYTE        plane_last;
    fc_desc_s  *descriptor;
    fc_buf_id_t alloc_id;
    fc_cmd_id_t command_id = FC_CMD_ID_INVALID;
    DWORD       plane_cw;

    /* ── 校验段：不得触碰任何池 ──
     * 资源申请不到（desc / buffer 池空）→ RESOURCE_EMPTY，是模仿硬件的预期失败。
     * 其余配置错误（句柄 / 生命周期 / 坐标 / buffer 归属）都是调用者 bug ⇒ ASSERT。 */
    ASSERT(command != NULL);
    ASSERT(fc_command_to_id(command, &command_id) == true);
    ASSERT(fc_command_state_get(&g_fc_controller, command_id) == FC_COMMAND_STATE_ALLOCATED);

    (void)fc_command_plane_span(command, &plane_first, &plane_last);
    ASSERT((cw < CW_NUM) && (plane >= plane_first) && (plane < plane_last));
    ASSERT(paa_to_plane(row_paa) == plane);

    if ((command->config.buf_own & FC_BUF_OWN_ALLOC_BY_UPPER) != 0U)
    {
        ASSERT((buffer_id != FC_BUF_ID_INVALID) && (buffer_id < FC_BUFFER_POOL_DEPTH));
        ASSERT(fc_buffer_state_get(&g_fc_controller, buffer_id) == FC_BUFFER_STATE_OCCUPIED);
        ASSERT(g_fc_controller.buffer_owner[buffer_id] == FC_CMD_ID_INVALID);
    }
    else
    {
        ASSERT(buffer_id == FC_BUF_ID_INVALID);
    }
    (void)command_id; /* release 下 ASSERT 裁空；所有者从 descriptor_owner 取 */

    /* ── 机制段 ── */
    descriptor = fc_pool_descriptor_alloc(&g_fc_controller, command);
    if (descriptor == NULL)
    {
        return FC_STATUS_RESOURCE_EMPTY;
    }

    const fc_desc_id_t descriptor_id = (fc_desc_id_t)(descriptor - g_fc_controller.descriptor_pool);
    const fc_cmd_id_t  owner         = g_fc_controller.descriptor_owner[descriptor_id];

    // 先把除 buffer_id 外的字段全部填好，再挂链：desc 一进链就能被任何
    // 走链的读者正确解释（paa/config/meta 均已有效，next_id 已终止）。

    // 两条公式：PAA 与 LBA 的 plane/cw 位置不同，不能再用同一条 plane_cw 公式。
    // desc->paa  —— PAA 空间：plane 与 cw 不相邻，用 paa_with_plane/paa_with_cw 逐字段换；
    // 行地址取自实参 row_paa（每 plane 独立，Policy B 两 plane 可不同块）。
    // desc->meta —— LBA 空间：plane/cw 低 3 位相邻（PLANE_CW_MASK 有效），
    // 高位取自 command->meta（本命令扇区 LBA）。
    plane_cw                    = (plane << PLANE_CW_SHIFT) | cw;
    descriptor->paa             = paa_with_cw(paa_with_plane(row_paa, plane), cw);
    descriptor->config          = command->config;
    descriptor->config.cw_count = 0U; /* 命令级每-plane 数量，desc 内无意义 */
    descriptor->cw_index        = cw;
    descriptor->next_id         = FC_DESC_ID_INVALID;
    descriptor->meta            = (command->meta & ~PLANE_CW_MASK) | plane_cw;

    fc_pool_chain_append(&g_fc_controller, command, descriptor); /* 出池即挂链 */

    // 上层申请（buf_own 10/11）：buffer 由调用者 fc_buffer_alloc 好并传进来，无失败路径。
    if ((command->config.buf_own & FC_BUF_OWN_ALLOC_BY_UPPER) != 0U)
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
 * @param command 命令
 * @param plane   plane 号
 * @param cw      CW 号
 * @param row_paa 该 plane 的行地址
 * @return FC_STATUS_OK；FC_STATUS_RESOURCE_EMPTY 池空
 */
fc_status_e fc_command_configure_descriptor(fc_cmd_s *command, BYTE plane, BYTE cw, DWORD row_paa)
{
    return fc_command_configure_descriptor_with_buffer(command, plane, cw, row_paa, FC_BUF_ID_INVALID);
}

/**
 * @brief 释放链头一个 desc（含其 buffer），返回新链头
 *
 * @param head 当前链头
 * @return 新链头；FC_DESC_ID_INVALID = 链已空
 */
fc_desc_id_t fc_descriptor_chain_release_head(fc_desc_id_t head)
{
    fc_pool_chain_release_head(&g_fc_controller, &head);
    return head;
}

/**
 * @brief 沿链释放全部 desc + buffer
 *
 * 建链中途失败的回滚入口。
 *
 * @param head 链头
 * @return FC_DESC_ID_INVALID
 */
fc_desc_id_t fc_descriptor_chain_release_all(fc_desc_id_t head)
{
    fc_pool_chain_release_all(&g_fc_controller, &head);
    return head;
}

/**
 * @brief 释放命令槽位（FREE 幂等）
 *
 * 指针身份是配置错误 ⇒ ASSERT；幂等早退在 fc_pool_command_release。
 *
 * @param command 命令；指针须落在命令池内。
 */
void fc_command_release(fc_cmd_s *command)
{
    fc_cmd_id_t command_id = FC_CMD_ID_INVALID;

    ASSERT(command != NULL);
    ASSERT(fc_command_to_id(command, &command_id) == true);
    (void)command_id; /* release（NDEBUG）下 command_id 仅用于 ASSERT */

    fc_pool_command_release(&g_fc_controller, command);
}

/**
 * @brief 释放上层预申请的 buffer（尚未挂到命令上）
 *
 * 未占用 / 仍归属某条命令是配置错误 ⇒ ASSERT；release 下由调用链保证。
 *
 * @param buffer_id buffer id
 */
void fc_buffer_release(fc_buf_id_t buffer_id)
{
    ASSERT(buffer_id < FC_BUFFER_POOL_DEPTH);
    ASSERT(fc_buffer_state_get(&g_fc_controller, buffer_id) == FC_BUFFER_STATE_OCCUPIED);
    ASSERT(g_fc_controller.buffer_owner[buffer_id] == FC_CMD_ID_INVALID);

    const BOOL released = fc_pool_buffer_release(&g_fc_controller, buffer_id, FC_CMD_ID_INVALID);

    ASSERT(released);
    (void)released;
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

    ASSERT(fc_buffer_is_occupied(buffer_id) == true);
    // 占用即保证池内指针非空（fc_pool_buffer_cw_base 只做数组下标），无需再判 NULL
    base = fc_pool_buffer_cw_base(&g_fc_controller, buffer_id);

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
    ASSERT(fc_buffer_is_occupied(buffer_id) == true);
    return fc_pool_buffer_cw_base(&g_fc_controller, buffer_id);
}

/**
 * @brief 返回命令中 CW 号 == cw_index 的 CW 的数据指针
 *
 * 按 paa_to_cw 匹配，不是链走位（flat CW 掩码下两者会分叉）。
 *
 * @param command  命令
 * @param cw_index CW 号
 * @return 数据指针；未找到 / 无 buffer 返回 NULL
 */
BYTE *fc_get_cmd_data_ptr(fc_cmd_s *command, BYTE cw_index)
{
    fc_desc_id_t desc_id;

    // command/desc 链非空是前置条件；cw_index 不在链上仍按查询语义返回 NULL
    ASSERT(command != NULL);
    ASSERT(command->desc_id != FC_DESC_ID_INVALID);

    desc_id = (fc_desc_id_t)command->desc_id;
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
 * @param command  命令
 * @param cw_index CW 号
 * @return META 指针；未找到 / 无 buffer 返回 NULL
 */
BYTE *fc_buffer_get_meta(fc_cmd_s *command, BYTE cw_index)
{
    fc_desc_id_t did;

    // command 非空是前置条件（gc.c 传入有效命令）；未找到 CW 仍按契约返回 NULL
    ASSERT(command != NULL);

    did = (fc_desc_id_t)command->desc_id;
    for (WORD steps = 0U; (did < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH); steps++)
    {
        fc_desc_s *d = fc_descriptor_id_to_descriptor(did);

        if (paa_to_cw(d->paa) == cw_index)
        {
            return (d->buffer_id != FC_BUF_ID_INVALID) ? fc_pool_buffer_cw_base(&g_fc_controller, d->buffer_id) : NULL;
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
 * @param command 命令
 * @return flat 位图（bit = plane*CW_NUM + cw）
 */
DWORD fc_get_cmd_cw_bitmap(const fc_cmd_s *command)
{
    fc_desc_id_t desc_id;
    DWORD        mask = 0U;

    ASSERT(command != NULL);

    desc_id = (fc_desc_id_t)command->desc_id;
    for (WORD steps = 0U; (desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH); steps++)
    {
        const fc_desc_s *descriptor = fc_descriptor_id_to_descriptor(desc_id);
        const BYTE       plane      = paa_to_plane(descriptor->paa);
        const BYTE       n          = (plane * CW_NUM + paa_to_cw(descriptor->paa));

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
 * @param command 命令
 * @return true 全部 CW 都是 EMPTY；false 否则
 */
BOOL fc_get_cmd_all_cw_empty(const fc_cmd_s *command)
{
    fc_desc_id_t desc_id;
    WORD         n = 0U;

    ASSERT(command != NULL);

    desc_id = (fc_desc_id_t)command->desc_id;
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
 * @param command 命令
 * @return 最大 ECC bit 数；空链为 0
 */
BYTE fc_get_cmd_ecc_max(const fc_cmd_s *command)
{
    fc_desc_id_t desc_id;
    BYTE         ecc_max = 0U;

    ASSERT(command != NULL);

    desc_id = (fc_desc_id_t)command->desc_id;
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
 * @brief 命令指针 → 池索引（O(1)）
 *
 * @param command    命令指针
 * @param command_id 输出：池索引
 * @return true 在池内；false 不在命令池区间（如栈上命令）
 */
BOOL fc_command_to_id(const fc_cmd_s *command, fc_cmd_id_t *command_id)
{
    // 层内 helper：调用者（fc_command_configure_descriptor / fc_command_release）已判过
    // command 非空，command_id 是本文件局部地址——传空是调用者编程错误。
    ASSERT(command != NULL);
    ASSERT(command_id != NULL);

    const uintptr_t base = (uintptr_t)g_fc_controller.command_pool;
    const uintptr_t addr = (uintptr_t)command;
    /* 上界必须是池本体的字节数；sizeof(指针字段) 只有 4 字节，
     * 会把 slot > 0 的命令全部误判为「不在池内」。 */
    const uintptr_t end  = base + sizeof(g_fc_command_pool);

    if ((addr < base) || (addr >= end) ||
        (((addr - base) % sizeof(fc_cmd_s)) != 0U))
    {
        return false;
    }
    *command_id = (fc_cmd_id_t)((addr - base) / sizeof(fc_cmd_s));
    return true;
}

/**
 * @brief 命令 ID → 命令指针
 *
 * @param command_id 池内命令 ID
 * @return 命令指针
 */
fc_cmd_s *fc_command_id_to_command(WORD command_id)
{
    // command_id 只来自 fc_command_alloc() 铸出的 ID（队列里只存分配产出的 ID），
    // 任何调用者都不可能传入越界值——越界即不变量破坏，留现场。
    ASSERT(command_id < FC_COMMAND_POOL_DEPTH);
    return fc_pool_command_by_id(&g_fc_controller, command_id);
}

/**
 * @brief 描述符 ID → 描述符指针
 *
 * @param descriptor_id 池内描述符 ID
 * @return 描述符指针
 */
fc_desc_s *fc_descriptor_id_to_descriptor(WORD descriptor_id)
{
    // descriptor_id 只来自命令 desc 链（configure_descriptors 铸出），恒在池内。
    ASSERT(descriptor_id < FC_DESCRIPTOR_POOL_DEPTH);
    return fc_pool_descriptor_by_id(&g_fc_controller, descriptor_id);
}

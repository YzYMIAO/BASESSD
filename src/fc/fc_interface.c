/*
 * fc_interface.c — FC 对外接口实现（单例 g_fc_controller）+ 建链机制体
 *
 * 职责：
 *   对外 API：参数校验 + 转发到 fc.c 的 fc_core_*()（参数化控制器实现）。
 *   建链入口：fc_command_configure_descriptor —— **唯一一条**建链函数，含机制体
 *             （出池 desc/buffer、填 PAA/config/meta、挂链尾）。
 *   除建链编排外不含任何池/调度/数据路径逻辑——那些留在 fc.c（核心）中；
 *   建链函数只调用 fc.c 的池/链原语，不直接读写池内部字段。
 *
 * 与 fc.c 的分工：
 *   fc.c            ：核心实现（三池本体与池/链原语、生命周期状态机、服务主循环
 *                     fc_core_poll、完成回写 fc_complete_from_nand、Stripe RAID）。
 *   fc_interface.c  ：对外 API，固定使用单例 g_fc_controller；建链机制体在此。
 *   fc_internal.h   ：两个文件共享的 internal 签名（FTL 不得 include）。
 *
 * 设计说明（为什么本文件单独存在）：
 *   - fc.c 专注核心路径，对外入口一目了然（本文件）。
 *   - 对外 API 多为无状态转发 + 参数校验，无逻辑，可读性集中于一处。
 *   - 新增对外接口 = 改 fc.h（或 fc_interface.h）+ 本文件，不影响核心。
 */

#include "fc_interface.h"

#include "assertion.h"
#include "fc_command.h"
#include "fc_data_path.h"
#include "fc_internal.h"
#include "fc_nand_dispatch.h"
#include "log.h"

#include <stddef.h>
#include <stdint.h>

/* ══════════════════════════════════════════════════════════
 * ① 初始化
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 初始化全局控制器（幂等，可重复调用以重置）
 *
 * @return FC_STATUS_OK；其余见 fc_core_init
 */
fc_status_e fc_init(void)
{
    return fc_core_init(&g_fc_controller);
}

/* ══════════════════════════════════════════════════════════
 * ② 命令生命周期
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 分配命令槽位
 *
 * @return 命令指针；NULL = 命令池空
 */
fc_command_t *fc_command_alloc(void)
{
    return fc_core_command_alloc(&g_fc_controller);
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
fc_status_e fc_command_configure_descriptor_with_buffer(fc_command_t *command, BYTE plane, BYTE cw, DWORD row_paa, fc_buffer_id_t buffer_id)
{
    BYTE             plane_first;
    BYTE             plane_last;
    fc_descriptor_t *descriptor;
    fc_buffer_id_t   alloc_id;
    fc_command_id_t  command_id = FC_COMMAND_ID_INVALID;
    DWORD            plane_cw;

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
        ASSERT((buffer_id != FC_BUFFER_ID_INVALID) && (buffer_id < FC_BUFFER_POOL_DEPTH));
        ASSERT(fc_buffer_state_get(&g_fc_controller, buffer_id) == FC_BUFFER_STATE_OCCUPIED);
        ASSERT(g_fc_controller.buffer_owner[buffer_id] == FC_COMMAND_ID_INVALID);
    }
    else
    {
        ASSERT(buffer_id == FC_BUFFER_ID_INVALID);
    }
    (void)command_id; /* release 下 ASSERT 裁空；所有者从 descriptor_owner 取 */

    /* ── 机制段 ── */
    descriptor = fc_core_descriptor_alloc(&g_fc_controller, command);
    if (descriptor == NULL)
    {
        return FC_STATUS_RESOURCE_EMPTY;
    }

    const fc_descriptor_id_t descriptor_id = (fc_descriptor_id_t)(descriptor - g_fc_controller.descriptor_pool);
    const fc_command_id_t    owner         = g_fc_controller.descriptor_owner[descriptor_id];

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
    descriptor->next_id         = FC_DESCRIPTOR_ID_INVALID;
    descriptor->meta            = (command->meta & ~PLANE_CW_MASK) | plane_cw;

    fc_command_chain_append(&g_fc_controller, command, descriptor); /* 出池即挂链 */

    // 上层申请（buf_own 10/11）：buffer 由调用者 fc_buffer_alloc 好并传进来，无失败路径。
    if ((command->config.buf_own & FC_BUF_OWN_ALLOC_BY_UPPER) != 0U)
    {
        descriptor->buffer_id                   = buffer_id;
        g_fc_controller.buffer_owner[buffer_id] = owner;
        return FC_STATUS_OK;
    }

    // FC 申请（默认 00/01）：buffer 池比 desc 池小，这里是最现实的失败点。失败**不回滚**：
    // desc 已在链上、buffer_id 保持 INVALID，调用方沿链释放时自然跳过 buffer、回收 desc。
    alloc_id = fc_core_buffer_alloc(&g_fc_controller);
    if (alloc_id == FC_BUFFER_ID_INVALID)
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
fc_status_e fc_command_configure_descriptor(fc_command_t *command, BYTE plane, BYTE cw, DWORD row_paa)
{
    return fc_command_configure_descriptor_with_buffer(command, plane, cw, row_paa, FC_BUFFER_ID_INVALID);
}

/**
 * @brief 释放链头一个 desc（含其 buffer），返回新链头
 *
 * @param head 当前链头
 * @return 新链头；FC_DESCRIPTOR_ID_INVALID = 链已空
 */
fc_descriptor_id_t fc_descriptor_chain_release_head(fc_descriptor_id_t head)
{
    fc_core_chain_release_head(&g_fc_controller, &head);
    return head;
}

/**
 * @brief 沿链释放全部 desc + buffer
 *
 * 建链中途失败的回滚入口。
 *
 * @param head 链头
 * @return FC_DESCRIPTOR_ID_INVALID
 */
fc_descriptor_id_t fc_descriptor_chain_release_all(fc_descriptor_id_t head)
{
    fc_core_chain_release_all(&g_fc_controller, &head);
    return head;
}

/**
 * @brief 提交一条命令
 *
 * 配置错误一律 ASSERT；唯一合法失败是队列满。
 *
 * @param command 命令（ALLOCATED）
 * @return FC_STATUS_OK；FC_STATUS_QUEUE_FULL
 */
fc_status_e fc_submit(fc_command_t *command)
{
    fc_command_id_t command_id = FC_COMMAND_ID_INVALID;

    /* ── 校验段：全是调用者自铸值 ⇒ ASSERT；唯一合法失败是队列满（资源）。 ── */
    ASSERT(command != NULL);
    ASSERT(fc_command_to_id(command, &command_id) == true);
    ASSERT(fc_command_state_get(&g_fc_controller, command_id) == FC_COMMAND_STATE_ALLOCATED);
    fc_command_validate(command);
    ASSERT(fc_core_chain_validate(&g_fc_controller, command_id, command) != 0U);
    (void)command_id; /* release 下 ASSERT 裁空 */

    return fc_core_submit(&g_fc_controller, command);
}

/**
 * @brief 批量提交（全进或全不进）
 *
 * cache program 批次依赖此语义；链契约要求全批同优先级。
 *
 * @param cmds 命令数组
 * @param n    条数
 * @return FC_STATUS_OK；FC_STATUS_QUEUE_FULL；FC_STATUS_INVALID_ARGUMENT 链契约违反
 */
fc_status_e fc_submit_batch(fc_command_t *const cmds[], DWORD n)
{
    ASSERT(cmds != NULL);
    ASSERT(n > 0U);

    WORD  sq_need        = 0U;
    WORD  hsq_need       = 0U;
    bool  has_chain      = false;
    bool  mixed_queue    = false;
    DWORD first_priority = 0U;

    for (DWORD i = 0U; i < n; i++)
    {
        fc_command_t   *command    = cmds[i];
        fc_command_id_t command_id = FC_COMMAND_ID_INVALID;

        ASSERT(command != NULL);
        ASSERT(fc_command_to_id(command, &command_id) == true);
        ASSERT(fc_command_state_get(&g_fc_controller, command_id) == FC_COMMAND_STATE_ALLOCATED);
        fc_command_validate(command);
        ASSERT(fc_core_chain_validate(&g_fc_controller, command_id, command) != 0U);
        (void)command_id; /* release 下 ASSERT 裁空 */

        if (i == 0U)
        {
            first_priority = command->high_priority;
        }
        else if (command->high_priority != first_priority)
        {
            mixed_queue = true;
        }

        has_chain = (has_chain == true) || (command->config.chain == FC_CHAIN_MORE);

        if (command->high_priority != 0U)
        {
            hsq_need++;
        }
        else
        {
            sq_need++;
        }
    }

    // 链契约：一批里的 chain 必须全批同优先级；**末条允许 MORE**（流式窗口）。
    // · 混优先级 ⇒ 链被拆成 SQ/HSQ 两段，fc_poll_chain_length 只沿本队列 peek，
    //   跳过的成员被当成不存在 ⇒ 序列静默错乱（cache read 输出错页 / 写错序列）；
    // · 末条 MORE 是合法中间窗口：FTL 的序列状态保证在 run_end 发 END；FC 的
    //   chain_owner 跨窗口保留，下一窗口继续。
    // 负例：cmds[0].chain=MORE 且 cmds[1].high_priority=1 ⇒ release 拒绝。
    ASSERT_REJECT(!((has_chain == true) && (mixed_queue == true)), FC_STATUS_INVALID_ARGUMENT, "mixed-priority chain batch");

    if (((WORD)(g_fc_controller.sq.count + sq_need) > FC_SQ_DEPTH) || ((WORD)(g_fc_controller.hsq.count + hsq_need) > FC_HSQ_DEPTH))
    {
        return FC_STATUS_QUEUE_FULL;
    }

    for (DWORD i = 0U; i < n; i++)
    {
        const fc_status_e st = fc_core_submit(&g_fc_controller, cmds[i]);

        // 预检通过后入队不可能失败；失败即队列不变量被破坏。
        ASSERT(st == FC_STATUS_OK);
        (void)st;
    }

    return FC_STATUS_OK;
}

/**
 * @brief 推进 FC 服务（NAND 引擎 + 完成回写）
 *
 * @return FC_STATUS_OK；其余见 fc_core_poll
 */
fc_status_e fc_service(void)
{
    return fc_core_poll(&g_fc_controller);
}

/**
 * @brief 从 CQ 取一条完成项
 *
 * @param completion 输出：完成项
 * @return FC_STATUS_OK；FC_STATUS_QUEUE_EMPTY
 */
fc_status_e fc_poll_completion(fc_command_status_t *completion)
{
    // completion 为出参前置条件
    ASSERT(completion != NULL);
    return fc_core_poll_completion(&g_fc_controller, completion);
}

/**
 * @brief 从 CQ 批量取完成项
 *
 * @param completions 输出数组
 * @param max_count   最多取几条
 * @param count       输出：实际条数
 * @return FC_STATUS_OK；FC_STATUS_QUEUE_EMPTY
 */
fc_status_e fc_poll_completions(fc_command_status_t *completions, WORD max_count, WORD *count)
{
    ASSERT(completions != NULL);
    ASSERT(count != NULL);
    return fc_core_poll_completions(&g_fc_controller, completions, max_count, count);
}

/**
 * @brief 释放命令槽位（FREE 幂等）
 *
 * 指针身份是配置错误 ⇒ ASSERT；幂等早退在 internal 版。
 *
 * @param command 命令；指针须落在命令池内。
 */
void fc_command_release(fc_command_t *command)
{
    fc_command_id_t command_id = FC_COMMAND_ID_INVALID;

    ASSERT(command != NULL);
    ASSERT(fc_command_to_id(command, &command_id) == true);
    (void)command_id; /* release（NDEBUG）下 command_id 仅用于 ASSERT */

    fc_core_command_release(&g_fc_controller, command);
}

/* ══════════════════════════════════════════════════════════
 * ③ buffer 访问
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 是否占用（供 fc_buffer_release / get_user_data / get_base 的前置 ASSERT 用）。
 *
 * @param buffer_id 待检查的 buffer ID。
 *
 * @return true = 池内且已占用；false = 越界或未占用。
 *
 * @note 仅用于 ASSERT，故标 __attribute__((unused))（NDEBUG 下 ASSERT 被裁空，
 * 否则触发 -Wunused-function）——与 fc_command_cw_attached 同法。
 */
static bool __attribute__((unused)) fc_buffer_is_occupied(fc_buffer_id_t buffer_id)
{
    return (buffer_id < FC_BUFFER_POOL_DEPTH) && (fc_buffer_state_get(&g_fc_controller, buffer_id) != FC_BUFFER_STATE_FREE);
}

/**
 * @brief 分配 buffer
 *
 * @return buffer id；FC_BUFFER_ID_INVALID = 池空
 */
fc_buffer_id_t fc_buffer_alloc(void)
{
    return fc_core_buffer_alloc(&g_fc_controller);
}

/**
 * @brief 释放上层预申请的 buffer（尚未挂到命令上）
 *
 * 未占用 / 仍归属某条命令是配置错误 ⇒ ASSERT；release 下由调用链保证。
 *
 * @param buffer_id buffer id
 */
void fc_buffer_release(fc_buffer_id_t buffer_id)
{
    ASSERT(buffer_id < FC_BUFFER_POOL_DEPTH);
    ASSERT(fc_buffer_state_get(&g_fc_controller, buffer_id) == FC_BUFFER_STATE_OCCUPIED);
    ASSERT(g_fc_controller.buffer_owner[buffer_id] == FC_COMMAND_ID_INVALID);

    const bool released = fc_core_buffer_release(&g_fc_controller, buffer_id, FC_COMMAND_ID_INVALID);

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
BYTE *fc_buffer_get_user_data(fc_buffer_id_t buffer_id)
{
    BYTE *base;

    ASSERT(fc_buffer_is_occupied(buffer_id) == true);
    // 占用即保证池内指针非空（fc_core_buffer_cw_base 只做数组下标），无需再判 NULL
    base = fc_core_buffer_cw_base(&g_fc_controller, buffer_id);

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
BYTE *fc_buffer_get_base(fc_buffer_id_t buffer_id)
{
    ASSERT(fc_buffer_is_occupied(buffer_id) == true);
    return fc_core_buffer_cw_base(&g_fc_controller, buffer_id);
}

/* ══════════════════════════════════════════════════════════
 * ④ 数据查询
 *   数据搬运已上移 FTL（见 ftl_cmd_write_data / ftl_cmd_read_data）——
 *   硬件 FCC 不提供「按 CW 拷贝数据」的寄存器，数据是固件经总线/PIO 填的。
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 返回命令中 CW 号 == cw_index 的 CW 的数据指针
 *
 * 按 paa_to_cw 匹配，不是链走位（flat CW 掩码下两者会分叉）。
 *
 * @param command  命令
 * @param cw_index CW 号
 * @return 数据指针；未找到 / 无 buffer 返回 NULL
 */
BYTE *fc_get_cmd_data_ptr(fc_command_t *command, BYTE cw_index)
{
    fc_descriptor_id_t desc_id;

    // command/desc 链非空是前置条件；cw_index 不在链上仍按查询语义返回 NULL
    ASSERT(command != NULL);
    ASSERT(command->desc_id != FC_DESCRIPTOR_ID_INVALID);

    desc_id = (fc_descriptor_id_t)command->desc_id;
    for (WORD steps = 0U; (desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH); steps++)
    {
        fc_descriptor_t *descriptor = fc_descriptor_id_to_descriptor(desc_id);

        if (paa_to_cw(descriptor->paa) == cw_index)
        {
            if (descriptor->buffer_id == FC_BUFFER_ID_INVALID)
            {
                return NULL;
            }
            return fc_buffer_get_user_data(descriptor->buffer_id);
        }
        desc_id = (fc_descriptor_id_t)descriptor->next_id;
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
BYTE *fc_buffer_get_meta(fc_command_t *command, BYTE cw_index)
{
    fc_descriptor_id_t did;

    // command 非空是前置条件（gc.c 传入有效命令）；未找到 CW 仍按契约返回 NULL
    ASSERT(command != NULL);

    did = (fc_descriptor_id_t)command->desc_id;
    for (WORD steps = 0U; (did < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH); steps++)
    {
        fc_descriptor_t *d = fc_descriptor_id_to_descriptor(did);

        if (paa_to_cw(d->paa) == cw_index)
        {
            return (d->buffer_id != FC_BUFFER_ID_INVALID) ? fc_core_buffer_cw_base(&g_fc_controller, d->buffer_id) : NULL;
        }
        did = (fc_descriptor_id_t)d->next_id;
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
DWORD fc_get_cmd_cw_bitmap(const fc_command_t *command)
{
    fc_descriptor_id_t desc_id;
    DWORD              mask = 0U;

    ASSERT(command != NULL);

    desc_id = (fc_descriptor_id_t)command->desc_id;
    for (WORD steps = 0U; (desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH); steps++)
    {
        const fc_descriptor_t *descriptor = fc_descriptor_id_to_descriptor(desc_id);
        const BYTE             plane      = paa_to_plane(descriptor->paa);
        const BYTE             n          = (plane * CW_NUM + paa_to_cw(descriptor->paa));

        // 与 fc_build_request_data_planes 的同源断言一致：链上的 CW 编号必须在 flat 空间内
        ASSERT(n < CW_TOTAL_MAX);
        mask |= (1U << n);
        desc_id = (fc_descriptor_id_t)descriptor->next_id;
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
bool fc_get_cmd_all_cw_empty(const fc_command_t *command)
{
    fc_descriptor_id_t desc_id;
    WORD               n = 0U;

    ASSERT(command != NULL);

    desc_id = (fc_descriptor_id_t)command->desc_id;
    while ((desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (n < CW_TOTAL_MAX))
    {
        const fc_descriptor_t *descriptor = fc_descriptor_id_to_descriptor(desc_id);

        if (fc_cw_status_is_empty(descriptor->status) == false)
        {
            return false;
        }
        n++;
        desc_id = (fc_descriptor_id_t)descriptor->next_id;
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
BYTE fc_get_cmd_ecc_max(const fc_command_t *command)
{
    fc_descriptor_id_t desc_id;
    BYTE               ecc_max = 0U;

    ASSERT(command != NULL);

    desc_id = (fc_descriptor_id_t)command->desc_id;
    for (WORD steps = 0U; (desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH); steps++)
    {
        const fc_descriptor_t *descriptor = fc_descriptor_id_to_descriptor(desc_id);

        if (descriptor->ecc_bits > ecc_max)
        {
            ecc_max = descriptor->ecc_bits;
        }
        desc_id = (fc_descriptor_id_t)descriptor->next_id;
    }
    return ecc_max;
}

/* ══════════════════════════════════════════════════════════
 * ⑤ 查询工具
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 命令指针 → 池索引（O(1)）
 *
 * @param command    命令指针
 * @param command_id 输出：池索引
 * @return true 在池内；false 不在命令池区间（如栈上命令）
 */
bool fc_command_to_id(const fc_command_t *command, fc_command_id_t *command_id)
{
    // 层内 helper：调用者（fc_command_configure_descriptor / fc_command_release）已判过
    // command 非空，command_id 是本文件局部地址——传空是调用者编程错误。
    ASSERT(command != NULL);
    ASSERT(command_id != NULL);

    const uintptr_t base = (uintptr_t)g_fc_controller.command_pool;
    const uintptr_t addr = (uintptr_t)command;
    const uintptr_t end  = base + sizeof(g_fc_controller.command_pool);

    if ((addr < base) || (addr >= end) ||
        (((addr - base) % sizeof(fc_command_t)) != 0U))
    {
        return false;
    }
    *command_id = (fc_command_id_t)((addr - base) / sizeof(fc_command_t));
    return true;
}

/**
 * @brief 命令 ID → 命令指针
 *
 * @param command_id 池内命令 ID
 * @return 命令指针
 */
fc_command_t *fc_command_id_to_command(WORD command_id)
{
    // command_id 只来自 fc_command_alloc() 铸出的 ID（队列里只存分配产出的 ID），
    // 任何调用者都不可能传入越界值——越界即不变量破坏，留现场。
    ASSERT(command_id < FC_COMMAND_POOL_DEPTH);
    return fc_core_command_by_id(&g_fc_controller, command_id);
}

/**
 * @brief 描述符 ID → 描述符指针
 *
 * @param descriptor_id 池内描述符 ID
 * @return 描述符指针
 */
fc_descriptor_t *fc_descriptor_id_to_descriptor(WORD descriptor_id)
{
    // descriptor_id 只来自命令 desc 链（configure_descriptors 铸出），恒在池内。
    ASSERT(descriptor_id < FC_DESCRIPTOR_POOL_DEPTH);
    return fc_core_descriptor_by_id(&g_fc_controller, descriptor_id);
}

/* ══════════════════════════════════════════════════════════
 * ⑥ 诊断 / 启动
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 汇总队列/三池/调度器调试状态
 *
 * @param out 输出
 */
void fc_debug_state_get(fc_debug_state_t *out)
{
    ASSERT(out != NULL);

    memset(out, 0, sizeof(*out));
    out->sq_used     = g_fc_controller.sq.count;
    out->hsq_used    = g_fc_controller.hsq.count;
    /* 诊断采样：只报告 (ch,ce,lun)=(0,0,0) 的 LUN，不是全维遍历。 */
    out->chain_owner = (BYTE)g_fc_controller.scheduler.chain_owner[0][0][0];
    out->lun_used    = fc_nand_dispatch_lun_used(0U, 0U, 0U);
    out->lun_phase   = fc_nand_dispatch_lun_phase(0U, 0U, 0U);
    fc_debug_fill_counters(&out->fill_calls, &out->fill_pops);
    out->sched_sq_count = (g_fc_controller.scheduler.sq != NULL) ? g_fc_controller.scheduler.sq->count : 0U;
    out->sched_sq_bound = ((g_fc_controller.scheduler.sq == &g_fc_controller.sq) && (g_fc_controller.scheduler.hsq == &g_fc_controller.hsq)) ? 1U : 0U;

    const fc_command_id_t sq_id = fc_submission_queue_peek(&g_fc_controller.sq, 0U);

    if (sq_id != FC_COMMAND_ID_INVALID)
    {
        const fc_command_t *cmd = fc_command_id_to_command(sq_id);

        out->sq_head_id    = sq_id;
        out->sq_head_state = (BYTE)fc_command_state_get(&g_fc_controller, sq_id);
        out->sq_head_chain = (WORD)cmd->config.chain;
        out->sq_head_op    = (BYTE)cmd->config.operation;
    }

    const fc_command_id_t hsq_id = fc_submission_queue_peek(&g_fc_controller.hsq, 0U);

    if (hsq_id != FC_COMMAND_ID_INVALID)
    {
        const fc_command_t *cmd = fc_command_id_to_command(hsq_id);

        out->hsq_head_id    = hsq_id;
        out->hsq_head_chain = (WORD)cmd->config.chain;
        out->hsq_head_op    = (BYTE)cmd->config.operation;
    }
}

/**
 * @brief 汇总队列与三池占用
 *
 * @param stats 输出
 */
void fc_get_resource_stats(fc_resource_stats_t *stats)
{
    // stats 为出参前置条件（调用者恒传有效指针）
    ASSERT(stats != NULL);

    stats->sq_used         = g_fc_controller.sq.count;
    stats->hsq_used        = g_fc_controller.hsq.count;
    stats->cq_used         = g_fc_controller.cq.count;
    stats->command_used    = g_fc_controller.command_used;
    stats->descriptor_used = g_fc_controller.descriptor_used;
    stats->buffer_used     = g_fc_controller.buffer_used;
    stats->command_high    = g_fc_controller.command_high;
    stats->descriptor_high = g_fc_controller.descriptor_high;
    stats->buffer_high     = g_fc_controller.buffer_high;

#ifndef NDEBUG
    /* debug 档做一次全池扫描，证明 O(1) 计数器与真值一致（C7 负例：
     * 任一闸门漏增减，这里立刻炸）。release 档只读计数器。 */
    WORD command_scan    = 0U;
    WORD descriptor_scan = 0U;
    WORD buffer_scan     = 0U;

    for (WORD index = 0U; index < FC_COMMAND_POOL_DEPTH; index++)
    {
        if (fc_command_state_get(&g_fc_controller, index) != FC_COMMAND_STATE_FREE)
        {
            command_scan++;
        }
    }
    for (WORD index = 0U; index < FC_DESCRIPTOR_POOL_DEPTH; index++)
    {
        if (fc_descriptor_owner_get(&g_fc_controller, index) != FC_COMMAND_ID_INVALID)
        {
            descriptor_scan++;
        }
    }
    for (WORD index = 0U; index < FC_BUFFER_POOL_DEPTH; index++)
    {
        if (fc_buffer_state_get(&g_fc_controller, index) != FC_BUFFER_STATE_FREE)
        {
            buffer_scan++;
        }
    }
    ASSERT(command_scan == stats->command_used);
    ASSERT(descriptor_scan == stats->descriptor_used);
    ASSERT(buffer_scan == stats->buffer_used);
#endif
}

/**
 * @brief 打印 FC 容量与拓扑（开机一次）
 *
 * 与断言路径的 [RES] 现场行互补：此处打容量，那里打占用。
 */
void fc_boot_report(void)
{
    const char *data_path = "PIO"; /* FMC NAND 无 DMA 请求线，数据面固定 PIO */

    LOGI("[FC  ] cmd=%u desc=%u buf=%u sq=%u hsq=%u cq=%u\r\n", FC_COMMAND_POOL_DEPTH, FC_DESCRIPTOR_POOL_DEPTH, FC_BUFFER_POOL_DEPTH, FC_SQ_DEPTH, FC_HSQ_DEPTH, FC_CQ_DEPTH);
    LOGI("[FC  ] topology ch=%d ce=%d lun=%d plane=%d raid=%s\r\n", CH_NUM, CE_NUM, LUN_NUM, PLANE_NUM, (CONFIG_FC_RAID != 0U) ? "on" : "off");
    LOGI("[FC  ] data path=%s ecc=%s random=%s\r\n", data_path, fc_ecc_algo_name(), fc_random_algo_name());
}

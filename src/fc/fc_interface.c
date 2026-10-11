/*
 * fc_interface.c — FC 对外接口实现（单例 g_fc_controller）+ 建链机制体
 *
 * 职责：
 *   对外 API：参数校验 + 转发到 fc.c 的 fc_*_internal()（参数化控制器实现）。
 *   建链入口：fc_command_configure_descriptor —— **唯一一条**建链函数，含机制体
 *             （出池 desc/buffer、填 PAA/config/meta、挂链尾）。v17 由 fc.c 收敛至此。
 *   除建链编排外不含任何池/调度/数据路径逻辑——那些留在 fc.c（核心）中；
 *   建链函数只调用 fc.c 的池/链原语，不直接读写池内部字段。
 *
 * 与 fc.c 的分工：
 *   fc.c            ：核心实现（三池本体与池/链原语、生命周期状态机、服务主循环
 *                     fc_poll_internal、完成回写 fc_complete_from_nand、Stripe RAID）。
 *   fc_interface.c  ：对外 API，固定使用单例 g_fc_controller；建链机制体在此。
 *   fc_internal.h   ：两个文件共享的 internal 签名（FTL 不得 include）。
 *
 * 设计说明（为什么本文件单独存在）：
 *   - fc.c 专注核心路径，对外入口一目了然（本文件）。
 *   - 对外 API 多为无状态转发 + 参数校验，无逻辑，可读性集中于一处。
 *   - 新增对外接口 = 改 fc.h（或 fc_interface.h）+ 本文件，不影响核心。
 */

#include "fc_interface.h"

#include "common/assertion.h"
#include "common/log.h"
#include "dma.h" /* CONFIG_NAND_DMA：数据通路 PIO/DMA */
#include "fc_command.h"
#include "fc_internal.h"
#include "fc_ldpc_engine.h"
#include "fc_random_engine.h"

#include <stddef.h>

/* 命令指针 → 池索引。区间检查 + 减法，O(1)（原为 O(256) 线性扫描）。
 * 返回 0 = 指针不落在命令池区间内（例如栈上的 fc_command_t），没有对应 ID。 */
uint8_t fc_command_to_id(const fc_command_t *command, fc_command_id_t *command_id)
{
    const fc_command_t *pool = g_fc_controller.command_pool;

    /* 层内 helper：调用者（fc_command_configure_descriptor / fc_command_release）已判过
     * command 非空，command_id 是本文件局部地址——传空是调用者编程错误。 */
    ASSERT(command != NULL);
    ASSERT(command_id != NULL);

    if ((command < pool) || (command >= &pool[FC_COMMAND_POOL_DEPTH]))
    {
        return 0U;
    }
    *command_id = fc_command_to_id_internal(&g_fc_controller, command);
    return 1U;
}

/* buffer ID 有效性（< 池深度且已占用） */
static uint8_t fc_buffer_id_is_valid(fc_buffer_id_t buffer_id)
{
    return (uint8_t)(buffer_id < FC_BUFFER_POOL_DEPTH);
}

/* 是否占用（供 fc_buffer_release / id_to_real_buffer / get_base 的前置 ASSERT 用）。
 * 仅用于 ASSERT，故标 __attribute__((unused))（NDEBUG 下 ASSERT 被裁空，
 * 否则触发 -Wunused-function）——与 fc_command_cw_attached 同法。 */
static uint8_t __attribute__((unused)) fc_buffer_is_occupied(fc_buffer_id_t buffer_id)
{
    return (fc_buffer_id_is_valid(buffer_id) != 0U) &&
           (fc_buffer_state_get(&g_fc_controller, buffer_id) != 0U);
}

/* ══════════════════════════════════════════════════════════
 * ① 初始化
 * ══════════════════════════════════════════════════════════ */

/* 初始化全局控制器（幂等，可重复调用以重置） */
fc_status_t fc_init(void)
{
    return fc_init_internal(&g_fc_controller);
}

/* ══════════════════════════════════════════════════════════
 * ② 命令生命周期
 * ══════════════════════════════════════════════════════════ */

/* 分配命令槽位 */
fc_command_t *fc_command_alloc(void)
{
    return fc_command_alloc_internal(&g_fc_controller);
}

/* --------------------------------------------------------------------
 * fc_command_cw_attached
 *   功能：判断 (plane, cw) 是否已在命令的 desc 链上（按 desc 自己的 PAA 算 n 匹配）。
 *   参数：command —— 命令；plane/cw —— 目标 CW 坐标。
 *   返回：1 = 已挂；0 = 未挂。
 *   实现要点：链长 ≤ CW_TOTAL_MAX，线性走 next_id 即可。
 *         仅用于 ASSERT，故标 __attribute__((unused))（NDEBUG 下 ASSERT 被裁空，
 *         否则触发 -Wunused-function）。
 * -------------------------------------------------------------------- */
static uint8_t __attribute__((unused)) fc_command_cw_attached(const fc_command_t *command, uint8_t plane, uint8_t cw)
{
    fc_descriptor_id_t descriptor_id = (fc_descriptor_id_t)command->desc_id;

    while (descriptor_id < FC_DESCRIPTOR_POOL_DEPTH)
    {
        const fc_descriptor_t *descriptor = &g_fc_controller.descriptor_pool[descriptor_id];

        if (((uint8_t)paa_to_plane(descriptor->paa) == plane) && ((uint8_t)paa_to_cw(descriptor->paa) == cw))
        {
            return 1U;
        }
        descriptor_id = (fc_descriptor_id_t)descriptor->next_id;
    }
    return 0U;
}

/* --------------------------------------------------------------------
 * fc_command_configure_descriptor_with_buffer
 *   功能：把一个 (plane, cw) 追加到命令 desc 链尾——**唯一的建链入口**（v17）。
 *         分配 desc、按 config.buf_own 决定 buffer 从哪来、填 PAA/config/meta、挂链尾。
 *   参数：command —— 命令；plane —— 目标 plane；cw —— 目标 CW 号；
 *         row_paa —— 该 plane 的行地址（page/block/lun）；
 *         buffer_id —— 上层申请（buf_own 的申请位 = 1，即 10/11）时由调用者
 *                     fc_buffer_alloc() 取好传入，FC 不再申请；FC 申请（00/01，默认）
 *                     时必须是 FC_BUFFER_ID_INVALID（带了 id = 配置错误 → ASSERT）。
 *   返回：FC_STATUS_OK / RESOURCE_EMPTY（desc 池或 buffer 池空）。
 *         配置错误（句柄非法 / 生命周期不符 / 坐标越界 / 重复挂同一 n）一律 ASSERT——
 *         绝不允许带着错配置发到硬件；只有「申请不到」才走返回码（模仿硬件行为）。
 *   实现要点：
 *     - **v17 起本函数即机制体**：原先「公共壳 → attach_cw_internal →
 *       configure_descriptor_internal」三层接力（三层签名相同、中间层纯直通）
 *       收敛为这一条。上层自行组装 CW 集合（例如只读离散 CW），循环调用即可。
 *     - **校验段在机制段之前**：任何非法实参都在出池之前返回，不污染池子——
 *       否则会先分配一个 desc 再失败，破坏「失败即沿链完整回收」的干净语义。
 *     - **行地址来自实参 row_paa，不从 command->paa 推导**：multi-plane 命令一个
 *       DWORD 装不下两个 plane 的行地址（Policy B：两 plane 的 block 可以不同）。
 *       plane/cw 位由实参决定，row_paa 的对应位被忽略（plane 位经 ASSERT 保证一致）。
 *     - **两条公式各造一个值**（v47 起；此前 v24~v46 是一条 plane_cw 公式）：
 *         desc->paa  = paa_with_cw(paa_with_plane(row_paa, plane), cw)   —— PAA 空间
 *         desc->meta = (command->meta & ~PLANE_CW_MASK) | plane_cw       —— LBA 空间
 *       PAA 的 plane 与 cw **不相邻**（`[cw][page][plane][block]`），plane_cw 掩码技巧
 *       只在 LBA/PBA 空间成立，故 desc->paa 改用 nand_config.h 的现成 helper；
 *       desc->meta 仍是 LBA（plane/cw 低 3 位相邻，PLANE_CW_MASK 有效）。
 *       代价：v24 的「一条公式造两个值」便利消失，换来 FC 层内 PBA 归零。
 *     - CW 号由调用方给出（FTL 侧的掩码枚举），**不由 desc 序号推导**——链走位
 *       （desc 序号）与 CW 号解耦：按链走位索引的地方不动，CW 号一律从
 *       paa_to_cw(desc->paa) 取。
 *     - meta = lba_to_meta(command->meta, plane, cw)（即上面的公式）。
 *     - **顺序：分配 desc → 填字段 → 挂链 → 分配 buffer**。先挂链是为了让
 *       「已分配的 desc 全部在链上」恒成立——任何一步失败后调用方沿链释放即完整
 *       回收，不需要「链外半成品」的就地回滚。代价是链上会短暂/长期存在
 *       buffer_id == FC_BUFFER_ID_INVALID 的 desc（buffer 池 128 < desc 池 256，
 *       buffer 分配失败是最现实的路径），走链的读者（释放 / count / 掩码重建）
 *       都要容忍——释放侧已按此实现。
 *     - **不搬数据**：数据搬运由上层（FTL）在链建好之后、且仅对写操作执行，
 *       见 ftl_cmd_write_data。放这里会让「建链失败→整链释放」白做一遍拷贝。
 *     - 失败时链上可能已有 desc（含本条）：本层不回滚，「已分配的 desc 全部在链上」
 *       是不变量，调用方沿链释放即完整回收（fc_descriptor_chain_release_all）。
 *     - **buffer 从哪来由 config.buf_own 决定**（v53）：上层申请（申请位 = 1）时用调用者
 *       传进来的 buffer_id（无失败路径）；FC 申请（默认）时保持既有的「挂链后申请、
 *       池空 → RESOURCE_EMPTY、desc 留链上」语义。归属语义见 fc_buf_own_t。
 * -------------------------------------------------------------------- */
fc_status_t fc_command_configure_descriptor_with_buffer(fc_command_t *command, uint8_t plane, uint8_t cw, nand_paa_t row_paa, fc_buffer_id_t buffer_id)
{
    fc_command_id_t  command_id;
    uint8_t          plane_first;
    uint8_t          plane_last;
    fc_descriptor_t *descriptor;
    fc_buffer_id_t   alloc_id;
    uint32_t         plane_cw;

    /* ── 校验段：不得触碰任何池 ──
     * 本函数**只有一种**合法失败：资源申请不到（desc / buffer 池空）→ RESOURCE_EMPTY，
     * 这是模仿硬件的行为，返回码在意料之中。
     * 其余一律是**配置错误**——句柄非法 / 生命周期不符 / 坐标越界 / 重复挂同一 n /
     * row_paa 的 plane 与实参不符——都是调用者 bug，绝不能带着错配置发到硬件，
     * 故全部 ASSERT（不再返回 INVALID_ARGUMENT）。 */
    ASSERT(command != NULL);
    ASSERT(fc_command_to_id(command, &command_id) != 0U);
    ASSERT(fc_command_state_get(&g_fc_controller, command_id) == FC_COMMAND_STATE_ALLOCATED);
    (void)command_id; /* release（NDEBUG）下上面两条 ASSERT 被裁掉，command_id 仅用于前置条件 */
    /* plane 必须落在本命令的 plane 段内，CW 号必须在页内 */
    (void)fc_command_plane_span(command, &plane_first, &plane_last);
    ASSERT((cw < CW_NUM) && (plane >= plane_first) && (plane < plane_last));
    /* 同一 n 被追加两次 = 链构造错误（掩码是集合，重复置位会静默丢数据） */
    ASSERT(fc_command_cw_attached(command, plane, cw) == 0U);
    /* row_paa 必须就是 plane 的行地址（否则 desc 的 plane 位与调用者意图不符） */
    ASSERT((uint8_t)paa_to_plane(row_paa) == plane);
    /* buffer 归属（buf_own）与 buffer_id 实参必须一致（v53）：
     *   上层申请（10/11）⇒ 调用者已 fc_buffer_alloc，必须传一个池内 id；
     *   FC 申请（00/01，默认）⇒ 不得自带 id，FC 建链时自己申请。 */
    if ((command->config.buf_own & FC_BUF_OWN_ALLOC_BY_UPPER) != 0U)
    {
        ASSERT((buffer_id != FC_BUFFER_ID_INVALID) && (buffer_id < FC_BUFFER_POOL_DEPTH));
    }
    else
    {
        ASSERT(buffer_id == FC_BUFFER_ID_INVALID);
    }

    /* ── 机制段 ── */
    descriptor = fc_descriptor_alloc_internal(&g_fc_controller, command);
    if (descriptor == NULL)
    {
        return FC_STATUS_RESOURCE_EMPTY;
    }

    /* 先把除 buffer_id 外的字段全部填好，再挂链：desc 一进链就能被任何
     * 走链的读者正确解释（paa/config/meta 均已有效，next_id 已终止）。 */

    /* 两条公式（v47）：PAA 与 LBA 的 plane/cw 位置不同，不能再用同一条 plane_cw 公式。
     *   desc->paa  —— PAA 空间：plane 与 cw 不相邻，用 paa_with_plane/paa_with_cw 逐字段换；
     *                 行地址取自实参 row_paa（每 plane 独立，Policy B 两 plane 可不同块）。
     *   desc->meta —— LBA 空间：plane/cw 低 3 位相邻（PLANE_CW_MASK 有效），
     *                 高位取自 command->meta（本命令扇区 LBA）。 */
    plane_cw              = ((uint32_t)plane << PBA_PLANE_SHIFT) | (uint32_t)cw;
    descriptor->paa       = paa_with_cw(paa_with_plane(row_paa, plane), cw);
    descriptor->config    = command->config;
    descriptor->config.cw = cw;
    descriptor->next_id   = FC_DESCRIPTOR_ID_INVALID;
    descriptor->meta      = (command->meta & ~PLANE_CW_MASK) | plane_cw;

    fc_command_chain_append(&g_fc_controller, command, descriptor); /* 出池即挂链 */

    /* 上层申请（buf_own 10/11）：buffer 由调用者 fc_buffer_alloc 好并传进来，无失败路径。 */
    if ((command->config.buf_own & FC_BUF_OWN_ALLOC_BY_UPPER) != 0U)
    {
        descriptor->buffer_id = buffer_id;
        return FC_STATUS_OK;
    }

    /* FC 申请（默认 00/01）：buffer 池比 desc 池小，这里是最现实的失败点。失败**不回滚**：
     * desc 已在链上、buffer_id 保持 INVALID，调用方沿链释放时自然跳过 buffer、回收 desc。 */
    alloc_id = fc_buffer_alloc_internal(&g_fc_controller);
    if (alloc_id == FC_BUFFER_ID_INVALID)
    {
        return FC_STATUS_RESOURCE_EMPTY;
    }
    descriptor->buffer_id = alloc_id;
    return FC_STATUS_OK;
}

/* 建链入口（FC 申请，默认路径）：等价于 _with_buffer(..., FC_BUFFER_ID_INVALID)。
 * 4 参签名保持不变——FTL 的两个调用点、RAID parity 建链、既有单测都无需改动。 */
fc_status_t fc_command_configure_descriptor(fc_command_t *command, uint8_t plane, uint8_t cw, nand_paa_t row_paa)
{
    return fc_command_configure_descriptor_with_buffer(command, plane, cw, row_paa, FC_BUFFER_ID_INVALID);
}

/* 释放链头那一个 desc（含其 buffer），返回新链头。
 * 传值/取回而非传指针：command->desc_id 是位域，&command->desc_id 取不了地址。 */
fc_descriptor_id_t fc_descriptor_chain_release_head(fc_descriptor_id_t head)
{
    fc_descriptor_chain_release_head_internal(&g_fc_controller, &head);
    return head;
}

/* 沿链释放全部 desc + buffer，返回 FC_DESCRIPTOR_ID_INVALID。
 * 建链中途失败的回滚入口；释放后命令只剩命令槽位（由 fc_command_release 收尾）。 */
fc_descriptor_id_t fc_descriptor_chain_release_all(fc_descriptor_id_t head)
{
    fc_descriptor_chain_release_all_internal(&g_fc_controller, &head);
    return head;
}

/* --------------------------------------------------------------------
 * fc_submit
 *   功能：命令投递到 SQ/HSQ（公共接口）。
 *   参数：command —— 已配置好的命令。
 *   返回：FC_STATUS_OK / QUEUE_FULL（队列满 = 资源，模仿硬件行为）。
 *   前置（违反一律 ASSERT，不返回错误码）：指针在命令池内；状态 ALLOCATED；
 *         validate 通过；desc 链所有权完整。配置错误绝不发到硬件。
 * -------------------------------------------------------------------- */
fc_status_t fc_submit(fc_command_t *command)
{
    fc_command_id_t command_id;

    /* ── 校验段：全是调用者自铸值（它持有的句柄 / 它设的状态 / 它填的 config）——
     *    属配置错误，绝不带错配置发到硬件，故一律 ASSERT，不再返回 INVALID_ARGUMENT。
     *    本函数唯一合法失败是队列满（资源），由 fc_submit_internal 返回 QUEUE_FULL。 ── */
    ASSERT(command != NULL);
    ASSERT(fc_command_to_id(command, &command_id) != 0U);
    ASSERT(fc_command_state_get(&g_fc_controller, command_id) == FC_COMMAND_STATE_ALLOCATED);
    ASSERT(fc_command_validate(command) == FC_STATUS_OK);
    ASSERT(fc_command_descriptors_owned_internal(&g_fc_controller, command_id, command) != 0U);
    (void)command_id; /* release（NDEBUG）下上面 ASSERT 被裁掉，command_id 仅用于前置条件 */

    return fc_submit_internal(&g_fc_controller, command);
}

/* --------------------------------------------------------------------
 * fc_service
 *   功能：服务主循环单步（公共入口）。
 *   返回：FC_STATUS_OK / IO_ERROR / NOT_READY（无命令可推进）。
 * -------------------------------------------------------------------- */
fc_status_t fc_service(void)
{
    return fc_poll_internal(&g_fc_controller);
}

/* 从 CQ 取完成项 */
fc_status_t fc_poll_completion(fc_command_status_t *completion)
{
    /* completion 为出参前置条件 */
    ASSERT(completion != NULL);
    return fc_poll_completion_internal(&g_fc_controller, completion);
}

/* --------------------------------------------------------------------
 * fc_poll_completions
 *   功能：从 CQ 批量取完成项（公共入口）。最多取 max_count 条，实际条数写入 *count。
 *   参数：completions —— 输出数组（容量 ≥ max_count）；max_count —— 本次上限（调用方定，
 *         FC 不写死）；count —— 输出实际取到的条数。
 *   返回：取到 ≥1 条 = OK；一条没取到 = QUEUE_EMPTY（含 max_count==0）。
 *   前置（违反一律 ASSERT）：completions、count 均非空。
 * -------------------------------------------------------------------- */
fc_status_t fc_poll_completions(fc_command_status_t *completions, uint16_t max_count, uint16_t *count)
{
    ASSERT(completions != NULL);
    ASSERT(count != NULL);
    return fc_poll_completions_internal(&g_fc_controller, completions, max_count, count);
}

/* --------------------------------------------------------------------
 * fc_command_release
 *   功能：释放命令全部资源（公共接口）。
 *   参数：command —— 命令。
 *   返回：恒 FC_STATUS_OK。
 *   前置（违反一律 ASSERT）：command 非空且指针落在命令池内——栈上的 fc_command_t
 *         是配置错误（没有对应命令可释放）。
 *   实现要点：**幂等**——命令已在 FREE 状态（空闲链上）时直接返回 OK，
 *         调用者不必自己判断"还活着吗"。命令须不在 CQ 中
 *         （FTL 应已 poll_completion 取走完成项）。
 * -------------------------------------------------------------------- */
fc_status_t fc_command_release(fc_command_t *command)
{
    fc_command_id_t command_id;

    /* 指针身份是配置错误（栈上的 fc_command_t 没有对应命令可释放）→ ASSERT，不返回 INVALID_ARGUMENT */
    ASSERT(command != NULL);
    ASSERT(fc_command_to_id(command, &command_id) != 0U);
    (void)command_id; /* release（NDEBUG）下上面 ASSERT 被裁掉，command_id 仅用于前置条件 */

    /* 幂等性（已 FREE 直接返回 OK）由 internal 版统一负责，此处不重复判 */
    return fc_command_release_internal_api(&g_fc_controller, command);
}

/* ══════════════════════════════════════════════════════════
 * ③ buffer 访问
 * ══════════════════════════════════════════════════════════ */

/* 分配 buffer */
fc_buffer_id_t fc_buffer_alloc(void)
{
    return fc_buffer_alloc_internal(&g_fc_controller);
}

/* 释放 buffer。前置：buffer_id 须已占用（未占用 = 配置错误 → ASSERT，不返回 INVALID_ARGUMENT）。
 * 返回：恒 FC_STATUS_OK。 */
fc_status_t fc_buffer_release(fc_buffer_id_t buffer_id)
{
    ASSERT(fc_buffer_is_occupied(buffer_id) != 0U);
    return fc_buffer_release_internal(&g_fc_controller, buffer_id);
}

/* 对外：返回 USER 数据区指针（跳过 META+CRC 8 字节）。
 * 前置：buffer_id 须已占用（未占用 = 配置错误 → ASSERT，不再返回 NULL）。 */
uint8_t *fc_buffer_id_to_real_buffer(fc_buffer_id_t buffer_id)
{
    uint8_t *base;

    ASSERT(fc_buffer_is_occupied(buffer_id) != 0U);
    /* 占用即保证池内指针非空（_internal 只做数组下标），无需再判 NULL */
    base = fc_buffer_id_to_real_buffer_internal(&g_fc_controller, buffer_id);

    /* 对外只暴露 USER 数据区，跳过 META(4) + CRC(4) */
    return base + FC_BUFFER_USER_OFFSET;
}

/* FC 内部：返回 CW 基址（528B，含 META/CRC/PARITY，数据路径用）。
 * 前置：buffer_id 须已占用（未占用 = 配置错误 → ASSERT，不再返回 NULL）。 */
uint8_t *fc_buffer_get_base(fc_buffer_id_t buffer_id)
{
    ASSERT(fc_buffer_is_occupied(buffer_id) != 0U);
    return fc_buffer_id_to_real_buffer_internal(&g_fc_controller, buffer_id);
}

/* 返回命令中 **CW 号 == cw_index** 的 CW 的 META 指针（校验物理地址用）。
 * 按 paa_to_cw(desc->paa) 匹配，不是链走位——flat CW 掩码下两者会分叉。 */
uint8_t *fc_buffer_get_meta(fc_command_t *command, uint8_t cw_index)
{
    fc_descriptor_id_t did;

    /* command 非空是前置条件（gc.c 传入有效命令）；未找到 CW 仍按契约返回 NULL */
    ASSERT(command != NULL);

    did = (fc_descriptor_id_t)command->desc_id;
    while (did < FC_DESCRIPTOR_POOL_DEPTH)
    {
        fc_descriptor_t *d = fc_descriptor_id_to_descriptor(did);

        if ((uint8_t)paa_to_cw(d->paa) == cw_index)
        {
            return (d->buffer_id != FC_BUFFER_ID_INVALID)
                       ? fc_buffer_id_to_real_buffer_internal(&g_fc_controller, d->buffer_id)
                       : NULL;
        }
        did = (fc_descriptor_id_t)d->next_id;
    }
    return NULL;
}

/* ══════════════════════════════════════════════════════════
 * ④ 数据查询
 *   数据搬运已上移 FTL（见 ftl_cmd_write_data / ftl_cmd_read_data）——
 *   硬件 FCC 不提供「按 CW 拷贝数据」的寄存器，数据是固件 DMA 填的。
 * ══════════════════════════════════════════════════════════ */

/* --------------------------------------------------------------------
 * fc_get_cmd_data_ptr
 *   功能：获取命令中 **CW 号 == cw_index** 的那个 CW 的 USER 数据指针。
 *   参数：command —— 命令；cw_index —— CW 号（0..CW_NUM-1）。
 *   返回：USER 指针 / NULL（该 CW 不在链上或 buffer 无效）。
 *   实现要点：按 `paa_to_cw(desc->paa)` 匹配，**不是**链走位——flat CW 掩码下
 *         两者会分叉（见 fc_command.h 不变量 2）。uniform 命令下等价。
 * -------------------------------------------------------------------- */
uint8_t *fc_get_cmd_data_ptr(fc_command_t *command, uint8_t cw_index)
{
    fc_descriptor_id_t desc_id;

    /* command/desc 链非空是前置条件；cw_index 不在链上仍按查询语义返回 NULL */
    ASSERT(command != NULL);
    ASSERT(command->desc_id != FC_DESCRIPTOR_ID_INVALID);

    desc_id = (fc_descriptor_id_t)command->desc_id;
    while (desc_id < FC_DESCRIPTOR_POOL_DEPTH)
    {
        fc_descriptor_t *descriptor = fc_descriptor_id_to_descriptor(desc_id);

        if ((uint8_t)paa_to_cw(descriptor->paa) == cw_index)
        {
            if (descriptor->buffer_id == FC_BUFFER_ID_INVALID)
            {
                return NULL;
            }
            return fc_buffer_id_to_real_buffer(descriptor->buffer_id);
        }
        desc_id = (fc_descriptor_id_t)descriptor->next_id;
    }

    return NULL;
}

/* --------------------------------------------------------------------
 * fc_get_cmd_cw_bitmap
 *   功能：从命令的 desc 链重建 flat CW 掩码（bit n = plane*CW_NUM + cw）。
 *   参数：command —— 命令（desc_id == INVALID → 返回 0）。
 *   返回：掩码；无 desc 时 0。
 *   实现要点：**链是 CW 集合的唯一真值**（v13 起 FC 内不再有掩码输入；v28 起该字段
 *         已从命令结构体删除）。凡需要"掩码形状"的判定（如 RAID 的 plane 对称性）
 *         一律先在此按链重建。
 *         调用者须在 fc_command_release() 之前调用（desc 释放后失效）。
 * -------------------------------------------------------------------- */
uint32_t fc_get_cmd_cw_bitmap(const fc_command_t *command)
{
    fc_descriptor_id_t desc_id;
    uint32_t           mask = 0U;

    ASSERT(command != NULL);

    desc_id = (fc_descriptor_id_t)command->desc_id;
    while (desc_id < FC_DESCRIPTOR_POOL_DEPTH)
    {
        const fc_descriptor_t *descriptor = fc_descriptor_id_to_descriptor(desc_id);
        const uint8_t          plane      = (uint8_t)paa_to_plane(descriptor->paa);
        const uint8_t          n          = (uint8_t)(plane * CW_NUM + paa_to_cw(descriptor->paa));

        /* 与 fc_build_request_data_planes 的同源断言一致：链上的 CW 编号必须在 flat 空间内 */
        ASSERT(n < CW_TOTAL_MAX);
        mask |= (uint32_t)(1U << n);
        desc_id = (fc_descriptor_id_t)descriptor->next_id;
    }
    return mask;
}

/* --------------------------------------------------------------------
 * fc_get_cmd_ecc_max
 *   功能：命令级聚合 ECC——desc 链上所有 CW 的 ecc_bits 最大值。
 *   参数：command —— 已完成的命令（须仍持有 desc 链）。
 *   返回：最大纠正 bit 数；无 desc 时 0。
 *   实现要点：**取全链**（含两个 plane）——fc_complete_from_nand 的 per-CW ECC
 *         只对 plane 0 折叠，flat 掩码下"只取 plane 0"会恒为 0；
 *         desc->ecc_bits 由该函数对链上**每个** desc 填写，全链取值无损。
 *         调用者须在 fc_command_release() 之前调用（desc 释放后失效）。
 * -------------------------------------------------------------------- */
uint8_t fc_get_cmd_ecc_max(const fc_command_t *command)
{
    fc_descriptor_id_t desc_id;
    uint8_t            ecc_max = 0U;

    ASSERT(command != NULL);

    desc_id = (fc_descriptor_id_t)command->desc_id;
    while (desc_id < FC_DESCRIPTOR_POOL_DEPTH)
    {
        const fc_descriptor_t *descriptor = fc_descriptor_id_to_descriptor(desc_id);

        if (descriptor->ecc_bits > ecc_max)
        {
            ecc_max = (uint8_t)descriptor->ecc_bits;
        }
        desc_id = (fc_descriptor_id_t)descriptor->next_id;
    }
    return ecc_max;
}

/* ══════════════════════════════════════════════════════════
 * ⑤ 查询工具
 * ══════════════════════════════════════════════════════════ */

/* 命令 ID → 命令指针（ID 恒为池内 ID，越界是调用者编程错误） */
fc_command_t *fc_command_id_to_command(uint16_t command_id)
{
    /* command_id 只来自 fc_command_alloc() 铸出的 ID（队列里只存分配产出的 ID），
     * 任何调用者都不可能传入越界值——越界即不变量破坏，留现场。 */
    ASSERT(command_id < FC_COMMAND_POOL_DEPTH);
    return fc_command_id_to_command_internal(&g_fc_controller, command_id);
}

/* 描述符 ID → 描述符指针（ID 恒为池内 ID，越界是调用者编程错误） */
fc_descriptor_t *fc_descriptor_id_to_descriptor(uint16_t descriptor_id)
{
    /* descriptor_id 只来自命令 desc 链（configure_descriptors 铸出），恒在池内。 */
    ASSERT(descriptor_id < FC_DESCRIPTOR_POOL_DEPTH);
    return fc_descriptor_id_to_descriptor_internal(&g_fc_controller, descriptor_id);
}

/* --------------------------------------------------------------------
 * fc_get_resource_stats
 *   功能：统计当前各资源池使用量（监控/测试）。
 *   参数：stats —— 输出。
 *   返回：FC_STATUS_OK / INVALID_ARGUMENT。
 * -------------------------------------------------------------------- */
fc_status_t fc_get_resource_stats(fc_resource_stats_t *stats)
{
    uint16_t index;

    /* stats 为出参前置条件（调用者恒传有效指针） */
    ASSERT(stats != NULL);

    stats->sq_used         = fc_get_sq_count(&g_fc_controller);
    stats->hsq_used        = fc_get_hsq_count(&g_fc_controller);
    stats->cq_used         = fc_get_cq_count(&g_fc_controller);
    stats->command_used    = 0U;
    stats->descriptor_used = 0U;
    stats->buffer_used     = 0U;
    for (index = 0U; index < FC_COMMAND_POOL_DEPTH; index++)
    {
        if (fc_command_state_get(&g_fc_controller, index) != FC_COMMAND_STATE_FREE)
        {
            stats->command_used++;
        }
    }
    for (index = 0U; index < FC_DESCRIPTOR_POOL_DEPTH; index++)
    {
        if (fc_descriptor_owner_get(&g_fc_controller, index) != FC_COMMAND_ID_INVALID)
        {
            stats->descriptor_used++;
        }
    }
    for (index = 0U; index < FC_BUFFER_POOL_DEPTH; index++)
    {
        if (fc_buffer_state_get(&g_fc_controller, index) != 0U)
        {
            stats->buffer_used++;
        }
    }
    return FC_STATUS_OK;
}

/* --------------------------------------------------------------------
 * fc_boot_report
 *   功能：开机横幅——打印 FC 固定资源池/队列深度、通道拓扑与数据通路配置。
 *   参数：无。
 *   返回：无。
 *   实现要点：全部编译期常量 + 两个只读引擎名 getter，无副作用。
 *         与断言路径的 [RES] 现场行（fc_assert_context.c）互补：此处打「容量」，
 *         那里打「占用」。
 * -------------------------------------------------------------------- */
void fc_boot_report(void)
{
#if CONFIG_NAND_DMA
    const char *data_path = "DMA";
#else
    const char *data_path = "PIO";
#endif

    LOGI("[FC  ] cmd=%u desc=%u buf=%u sq=%u hsq=%u cq=%u\r\n",
         (unsigned)FC_COMMAND_POOL_DEPTH,
         (unsigned)FC_DESCRIPTOR_POOL_DEPTH,
         (unsigned)FC_BUFFER_POOL_DEPTH,
         (unsigned)FC_SQ_DEPTH,
         (unsigned)FC_HSQ_DEPTH,
         (unsigned)FC_CQ_DEPTH);
    LOGI("[FC  ] topology ch=%u ce=%u lun=%u plane=%u raid=%s\r\n",
         (unsigned)CH_NUM,
         (unsigned)CE_NUM,
         (unsigned)LUN_NUM,
         (unsigned)PLANE_NUM,
         (CONFIG_FC_RAID != 0U) ? "on" : "off");
    LOGI("[FC  ] data path=%s ecc=%s random=%s\r\n", data_path, fc_ecc_algo_name(), fc_random_algo_name());
}

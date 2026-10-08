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
#include "fc_command.h"
#include "fc_internal.h"

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

/* 是否占用（供 fc_buffer_release / id_to_real_buffer 校验） */
static uint8_t fc_buffer_is_occupied(fc_buffer_id_t buffer_id)
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
 * fc_command_configure_descriptor
 *   功能：把一个 (plane, cw) 追加到命令 desc 链尾——**唯一的建链入口**（v17）。
 *         分配 desc + buffer、填 PAA/config/meta、挂链尾。
 *   参数：command —— 命令；plane —— 目标 plane；cw —— 目标 CW 号；
 *         row_paa —— 该 plane 的行地址（page/block/lun）。
 *   返回：FC_STATUS_OK / INVALID_ARGUMENT（坐标越界或生命周期不符）
 *         / RESOURCE_EMPTY（desc 池或 buffer 池空）。
 *   实现要点：
 *     - **v17 起本函数即机制体**：原先「公共壳 → attach_cw_internal →
 *       configure_descriptor_internal」三层接力（三层签名相同、中间层纯直通）
 *       收敛为这一条。上层自行组装 CW 集合（例如只读离散 CW），循环调用即可。
 *     - **校验段在机制段之前**：任何非法实参都在出池之前返回，不污染池子——
 *       否则会先分配一个 desc 再失败，破坏「失败即沿链完整回收」的干净语义。
 *     - **行地址来自实参 row_paa，不从 command->paa 推导**：multi-plane 命令一个
 *       DWORD 装不下两个 plane 的行地址（Policy B：两 plane 的 block 可以不同）。
 *       plane 位由实参 plane 决定，row_paa 的 plane 位被忽略（经 ASSERT 保证一致）。
 *     - multi_plane>0：PAA = row_paa 的 block/page/lun + 传入的 plane/cw。
 *     - 单 plane：row_paa 已完整描述该 plane 的地址，只改写 CW 位。
 *     - CW 号由调用方给出（FTL 侧的掩码枚举），**不由 desc 序号推导**——链走位
 *       （desc 序号）与 CW 号解耦：按链走位索引的地方不动，CW 号一律从
 *       paa_to_cw(desc->paa) 取。
 *     - meta = lba_to_cw_meta(command->meta, cw)。
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
 * -------------------------------------------------------------------- */
fc_status_t fc_command_configure_descriptor(fc_command_t *command, uint8_t plane, uint8_t cw, nand_paa_t row_paa)
{
    fc_command_id_t  command_id;
    uint8_t          plane_first;
    uint8_t          plane_last;
    fc_descriptor_t *descriptor;
    fc_buffer_id_t   buffer_id;

    /* ── 校验段：不得触碰任何池 ── */
    ASSERT(command != NULL);
    /* 指针身份 + 生命周期状态是文档化入口契约，保留返回码 */
    if ((fc_command_to_id(command, &command_id) == 0U) ||
        (fc_command_state_get(&g_fc_controller, command_id) != FC_COMMAND_STATE_ALLOCATED))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    /* plane 必须落在本命令的 plane 段内，CW 号必须在页内 */
    (void)fc_command_plane_span(command, &plane_first, &plane_last);
    if ((cw >= CW_NUM) || (plane < plane_first) || (plane >= plane_last))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    /* 同一 n 被追加两次 = 链构造错误（掩码是集合，重复置位会静默丢数据） */
    ASSERT(fc_command_cw_attached(command, plane, cw) == 0U);
    /* row_paa 必须就是 plane 的行地址（否则 desc 的 plane 位与调用者意图不符） */
    ASSERT((uint8_t)paa_to_plane(row_paa) == plane);

    /* ── 机制段 ── */
    descriptor = fc_descriptor_alloc_internal(&g_fc_controller, command);
    if (descriptor == NULL)
    {
        return FC_STATUS_RESOURCE_EMPTY;
    }

    /* 先把除 buffer_id 外的字段全部填好，再挂链：desc 一进链就能被任何
     * 走链的读者正确解释（paa/config/meta 均已有效，next_id 已终止）。 */
    if (command->config.multi_plane > 0U)
    {
        /* 行地址取自实参 row_paa（每 plane 独立）；保留其 LUN 字段（多 LUN 并发必需：
         * desc PAA/META/随机种子/ECC 都依赖它）。 */
        descriptor->paa = paa_make(paa_to_page(row_paa), plane, paa_to_block(row_paa), paa_to_lun(row_paa), cw);
    }
    else
    {
        descriptor->paa = (row_paa & ~NAND_FIELD_MASK(CW_WIDTH)) | cw;
    }
    descriptor->config    = command->config;
    descriptor->config.cw = cw;
    descriptor->next_id   = FC_DESCRIPTOR_ID_INVALID;
    descriptor->meta      = lba_to_cw_meta(command->meta, cw);

    fc_command_chain_append(&g_fc_controller, command, descriptor); /* 出池即挂链 */

    /* buffer 池比 desc 池小，这里是最现实的失败点。失败**不回滚**：desc 已在链上、
     * buffer_id 保持 INVALID，调用方沿链释放时自然跳过 buffer、回收 desc。 */
    buffer_id = fc_buffer_alloc_internal(&g_fc_controller);
    if (buffer_id == FC_BUFFER_ID_INVALID)
    {
        return FC_STATUS_RESOURCE_EMPTY;
    }
    descriptor->buffer_id = buffer_id;
    return FC_STATUS_OK;
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
 *   返回：FC_STATUS_OK / INVALID_ARGUMENT / QUEUE_FULL。
 *   实现要点：状态须 ALLOCATED、validate 通过、desc 链所有权完整。
 * -------------------------------------------------------------------- */
fc_status_t fc_submit(fc_command_t *command)
{
    fc_command_id_t command_id;

    /* command 非空是前置条件；指针身份 + 生命周期 + 校验结果是文档化入口契约，保留返回码 */
    ASSERT(command != NULL);
    if ((fc_command_to_id(command, &command_id) == 0U) ||
        (fc_command_state_get(&g_fc_controller, command_id) != FC_COMMAND_STATE_ALLOCATED) ||
        (fc_command_validate(command) != FC_STATUS_OK) ||
        (fc_command_descriptors_owned_internal(&g_fc_controller, command_id, command) == 0U))
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
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
 * fc_command_release
 *   功能：释放命令全部资源（公共接口）。
 *   参数：command —— 命令。
 *   返回：FC_STATUS_OK / INVALID_ARGUMENT（指针不在命令池内）。
 *   实现要点：**幂等**——命令已在 FREE 状态（空闲链上）时直接返回 OK，
 *         调用者不必自己判断"还活着吗"。命令须不在 CQ 中
 *         （FTL 应已 poll_completion 取走完成项）。
 * -------------------------------------------------------------------- */
fc_status_t fc_command_release(fc_command_t *command)
{
    fc_command_id_t command_id;

    /* command 非空是前置条件；指针身份校验是文档化入口契约（辨析 D），保留返回码 */
    ASSERT(command != NULL);

    /* 指针不在池内（栈上的 fc_command_t）→ 没有对应命令可释放 */
    if (fc_command_to_id(command, &command_id) == 0U)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

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

/* 释放 buffer（须已占用） */
fc_status_t fc_buffer_release(fc_buffer_id_t buffer_id)
{
    if (fc_buffer_is_occupied(buffer_id) == 0U)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }
    return fc_buffer_release_internal(&g_fc_controller, buffer_id);
}

/* 对外：返回 USER 数据区指针（跳过 META+CRC 8 字节） */
uint8_t *fc_buffer_id_to_real_buffer(fc_buffer_id_t buffer_id)
{
    uint8_t *base;

    if (fc_buffer_is_occupied(buffer_id) == 0U)
    {
        return NULL;
    }
    base = fc_buffer_id_to_real_buffer_internal(&g_fc_controller, buffer_id);
    if (base == NULL)
    {
        return NULL;
    }
    /* 对外只暴露 USER 数据区，跳过 META(4) + CRC(4) */
    return base + FC_BUFFER_USER_OFFSET;
}

/* FC 内部：返回 CW 基址（528B，含 META/CRC/PARITY，数据路径用） */
uint8_t *fc_buffer_get_base(fc_buffer_id_t buffer_id)
{
    if (fc_buffer_is_occupied(buffer_id) == 0U)
    {
        return NULL;
    }
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
 * fc_get_cmd_cw_mask
 *   功能：从命令的 desc 链重建 flat CW 掩码（bit n = plane*CW_NUM + cw）。
 *   参数：command —— 命令（desc_id == INVALID → 返回 0）。
 *   返回：掩码；无 desc 时 0。
 *   实现要点：**链是 CW 集合的唯一真值**（v13 起 FC 内不再有掩码输入，
 *         command->param.cw_mask 的读者是 FTL）。凡需要"掩码形状"的判定
 *         （如 RAID 的 plane 对称性）一律先在此按链重建，不再回读输入字段。
 *         调用者须在 fc_command_release() 之前调用（desc 释放后失效）。
 * -------------------------------------------------------------------- */
uint32_t fc_get_cmd_cw_mask(const fc_command_t *command)
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

#include "ftl/ftl_fc_interface.h"

#include "common/assertion.h"
#include "fc/fc.h"
#include "ftl/completion_table.h"
#include "ftl/ftl_type.h"
#include "nand/onfi_1_0.h"

#include <string.h>

/* --------------------------------------------------------------------
 * ftl_cmd_resolve_cw_bitmap
 *   功能：校验建链形参 cw_bitmap —— 「本命令要访问哪些 CW」的唯一定义处。
 *   参数：bitmap —— 调用者给的 flat 字面位图（v28：形参，非命令字段）；
 *         plane_first / plane_count —— 本命令的 plane 段（由 fc_command_plane_span 给出）。
 *   返回：位图原值（**非 0**——段内至少有一个 CW 被选中）。
 *   实现要点（v27 起 cw_bitmap 是**字面位图**）：
 *     - **没有 0 的隐式展开**：v23~v26 把 0 重载为「本命令 plane 段内的全部 CW」，
 *       现取消。名字叫 bitmap 就必须逐位字面：0 = 一个 CW 都不访问，那不是合法的
 *       数据命令（建链会产出空链 → desc_id = INVALID → 提交后**静默不搬任何数据**）。
 *       整段访问由调用者用 fc_command_plane_span_cw_bitmap() 显式算 —— 见
 *       addr_to_cmd / ftl_cmd_setup_ppa / ftl_cmd_setup_ppa_pair。
 *       ⚠ 这两条 ASSERT 就是「生产者忘填」的唯一拦网，且 ASSERT 在 release 下会被裁掉，
 *         故「忘了填」的代价是**编译期无声、release 下静默空操作**——新增建链调用点时
 *         必须显式给位图（§十三：建链入口的配置错误一律 ASSERT）。
 *     - 位图必须落在段内；越段是调用者编程错误 → ASSERT。
 *       ⚠ 该检查不能省：下面的枚举只走段内的 plane × cw，段外的高位 bit 根本不会被枚举到，
 *         不检查就是**静默忽略**（机制层 fc_command_configure_descriptor 的越段 ASSERT 也拦不到）。
 * -------------------------------------------------------------------- */
static uint32_t ftl_cmd_resolve_cw_bitmap(uint32_t bitmap, uint8_t plane_first, uint8_t plane_count)
{
    const uint32_t allowed = ((uint32_t)((1U << ((uint32_t)plane_count * CW_NUM)) - 1U)) << ((uint32_t)plane_first * CW_NUM);

    ASSERT(bitmap != 0U);              /* 字面位图：0 = 空集 = 调用者忘了给（v27 起无隐式展开） */
    ASSERT((bitmap & ~allowed) == 0U); /* 越出本命令 plane 段 = 编程错误 */
    return bitmap;
}

/* --------------------------------------------------------------------
 * ftl_cmd_configure_descriptors
 *   功能：按形参 cw_bitmap 组装整条 desc 链（v13 由 FC 策略层上移）。
 *   参数：cmd —— 命令（须已设 config.operation / multi_plane / pba）；
 *         cw_bitmap —— 本次建链要访问的 CW（flat 字面位图，v28 起为形参）。
 *   返回：FTL_OK / FTL_ERROR（池空或坐标非法）。
 *   实现要点：
 *     - **只建链，不搬数据**：数据由 ftl_cmd_write_data 在链建好之后、且仅对写操作执行。
 *       搬数据放在这里会让「建链失败 → 整链释放」白做一遍拷贝。
 *     - 无数据操作（ERASE / RESET 等）与 SET/GET_FEATURES 不建链，desc_id 置 INVALID
 *       （此路径**不看 cw_bitmap**，故擦除命令传什么都不会炸）。
 *     - cw_bitmap 是**字面位图**（v27，0 = 空集，无隐式展开；整段访问用
 *       fc_command_plane_span_cw_bitmap() 算）。它必须落在本命令 plane 段内
 *       （越段 = 编程错误 → ASSERT，见 ftl_cmd_resolve_cw_bitmap）。
 *       离散读 plane1 的 CW0/CW2 即传 (1U << 4) | (1U << 6)。
 *       ⚠ 想「整段访问」不要传 0 —— 那是空集。用 ftl_cmd_setup_ppa(_pair)，
 *         或显式传 fc_command_plane_span_cw_bitmap(cmd)。
 *     - 行地址所有 plane 共用 cmd->pba（同页语义）；两 plane 不同块（Policy B）
 *       走 ftl_cmd_setup_ppa_pair()，本函数不适用。
 *     - **失败时本函数自己回滚整条链**（desc + buffer 全部还池，desc_id 复位 INVALID），
 *       调用者只需再 fc_command_release 还命令槽位。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_cmd_configure_descriptors(fc_command_t *cmd, uint32_t cw_bitmap)
{
    uint32_t mask;
    uint8_t  plane_count;
    uint8_t  plane_first;
    uint8_t  plane_last;
    uint8_t  plane;
    uint8_t  cw;

    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    if (ftl_cmd_needs_desc_chain(cmd) == 0U)
    {
        cmd->desc_id = FC_DESCRIPTOR_ID_INVALID;
        return FTL_OK;
    }

    plane_count = fc_command_plane_span(cmd, &plane_first, &plane_last);
    mask        = ftl_cmd_resolve_cw_bitmap(cw_bitmap, plane_first, plane_count);

    for (plane = plane_first; plane < plane_last; plane++)
    {
        const nand_pba_t row_pba = (cmd->pba & ~PLANE_CW_MASK) | ((uint32_t)plane << PBA_PLANE_SHIFT);

        for (cw = 0U; cw < CW_NUM; cw++)
        {
            if ((mask & ((uint32_t)1U << ((uint32_t)plane * CW_NUM + cw))) == 0U)
            {
                continue;
            }
            if (fc_command_configure_descriptor(cmd, plane, cw, row_pba) != FC_STATUS_OK)
            {
                cmd->desc_id = fc_descriptor_chain_release_all(cmd->desc_id);
                return FTL_ERROR;
            }
        }
    }

    return FTL_OK;
}

ftl_status_t ftl_cmd_setup_ppa(fc_command_t *cmd, nand_pba_t pba)
{
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    cmd->config.ch  = (DWORD)pba_to_ch(pba);
    cmd->config.ce  = (DWORD)pba_to_ce(pba);
    cmd->config.lun = (DWORD)pba_to_lun(pba);
    cmd->pba        = pba;
    /* v27/v28：位图是字面位图（0 = 空集），「整段访问」不再有隐式默认值。本入口语义
     * 就是「整段」——自己算好当实参传，调用者不必管。须在 pba 写好之后算。 */
    return ftl_cmd_configure_descriptors(cmd, fc_command_plane_span_cw_bitmap(cmd));
}

ftl_status_t ftl_cmd_setup_ppa_pair(fc_command_t *cmd, const nand_pba_t pba[PLANE_NUM])
{
    uint32_t mask;
    uint8_t  plane_count;
    uint8_t  plane_first;
    uint8_t  plane_last;
    uint8_t  i;

    if ((cmd == NULL) || (pba == NULL))
    {
        return FTL_ERROR;
    }

    /* 拓扑取 pba[0]（同一命令两 plane 必属同一 ch/ce/lun）；command->pba 也写 pba[0]——
     * 它是 fc_command_plane_span 定 plane 段的输入，各 plane 的实际行地址逐条由 row 决定。 */
    cmd->config.ch  = (DWORD)pba_to_ch(pba[0]);
    cmd->config.ce  = (DWORD)pba_to_ce(pba[0]);
    cmd->config.lun = (DWORD)pba_to_lun(pba[0]);
    cmd->pba        = pba[0];

    if (ftl_cmd_needs_desc_chain(cmd) == 0U)
    {
        cmd->desc_id = FC_DESCRIPTOR_ID_INVALID;
        return FTL_OK;
    }

    /* v23：plane 段与位图展开规则与 ftl_cmd_configure_descriptors **完全一致**（同一个
     * ftl_cmd_resolve_cw_bitmap）。本函数与它的唯一差别只是**行地址逐 plane 各自取**（Policy B），
     * 而不是"两个入口各认一个旋钮"——此前这里读 config.cw、那边读 param.cw_bitmap，
     * 于是 param.cw_bitmap 这条通道主数据路径根本不看。
     * v27/v28：位图是字面位图（0 = 空集），本入口语义就是「整段」——自己算好当实参
     * 交给下面的 ftl_cmd_resolve_cw_bitmap（须在 pba 写好之后算）。 */
    plane_count = fc_command_plane_span(cmd, &plane_first, &plane_last);
    mask        = ftl_cmd_resolve_cw_bitmap(fc_command_plane_span_cw_bitmap(cmd), plane_first, plane_count);

    for (i = 0U; i < plane_count; i++)
    {
        /* ⚠ plane 号取自 PBA 自己的 plane 位，不是下标 i：读路径的 pba[] 是按 LBA 查出来的，
         *   plane 取决于当初怎么写进去（`wr 0 2` 之后 lba 1 映射到 plane 1），下标与 plane 号
         *   并不总相等。链序仍按 pba[] 顺序 → ftl_cmd_read_data 拷出的布局与调用者期望一致。 */
        const uint8_t    plane = (uint8_t)pba_to_plane(pba[i]);
        const nand_pba_t row   = pba[i];
        uint8_t          cw;

        for (cw = 0U; cw < CW_NUM; cw++)
        {
            if ((mask & ((uint32_t)1U << ((uint32_t)plane * CW_NUM + cw))) == 0U)
            {
                continue;
            }
            if (fc_command_configure_descriptor(cmd, plane, cw, row) != FC_STATUS_OK)
            {
                /* 回滚整条链（desc + buffer 还池，desc_id 复位）；命令槽位由调用者释放 */
                cmd->desc_id = fc_descriptor_chain_release_all(cmd->desc_id);
                return FTL_ERROR;
            }
        }
    }

    return FTL_OK;
}

/* --------------------------------------------------------------------
 * ftl_cmd_write_data
 *   功能：把外部数据按 CW 顺序写入命令 desc 链上各 CW 的 USER 区。
 *   参数：cmd —— 已建链的命令；data —— 源数据；length —— 字节数。
 *   返回：FTL_OK / FTL_ERROR（链上 desc 无 buffer）。
 *   实现要点（v13 由 FC 上移：硬件 FCC 不提供「按 CW 拷贝」寄存器，数据由固件填）：
 *     - 遍历界走 desc 链（链长度是 CW 数的唯一真值），链序即数据布局顺序。
 *     - 数据耗尽后剩余 CW 的 USER 区填 0xFF dummy（不把上一轮的残值写进 NAND）。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_cmd_write_data(fc_command_t *cmd, const void *data, uint32_t length)
{
    fc_descriptor_id_t desc_id;
    const uint8_t     *src;

    ASSERT(cmd != NULL);
    ASSERT(data != NULL);
    ASSERT(cmd->desc_id != FC_DESCRIPTOR_ID_INVALID);

    src     = (const uint8_t *)data;
    desc_id = (fc_descriptor_id_t)cmd->desc_id;

    while (desc_id < FC_DESCRIPTOR_POOL_DEPTH)
    {
        fc_descriptor_t *descriptor = fc_descriptor_id_to_descriptor(desc_id);
        uint8_t         *buf;

        if (descriptor->buffer_id == FC_BUFFER_ID_INVALID)
        {
            return FTL_ERROR;
        }
        buf = fc_buffer_id_to_real_buffer(descriptor->buffer_id);

        if (length > 0U)
        {
            uint32_t copy_len = (length >= FC_BUFFER_USER_SIZE) ? FC_BUFFER_USER_SIZE : length;

            (void)memcpy(buf, src, copy_len);
            src += copy_len;
            length -= copy_len;

            /* 本 CW 未填满部分用 0xFF dummy 补齐 */
            if (copy_len < FC_BUFFER_USER_SIZE)
            {
                (void)memset(buf + copy_len, 0xFF, (size_t)(FC_BUFFER_USER_SIZE - copy_len));
            }
        }
        else
        {
            /* 数据已耗尽：本 CW 的 USER 区整体填 0xFF dummy。
             * ⚠ 长度必须是 USER 区大小——buf 是 base+FC_BUFFER_USER_OFFSET，
             *   照旧代码用 FC_BUFFER_POOL_CW_SIZE 会越界 8B 踩到下一 CW 的 META/CRC。 */
            (void)memset(buf, 0xFF, (size_t)FC_BUFFER_USER_SIZE);
        }

        desc_id = (fc_descriptor_id_t)descriptor->next_id;
    }

    return FTL_OK;
}

/* --------------------------------------------------------------------
 * ftl_cmd_read_data
 *   功能：把命令 desc 链上各 CW 的 USER 区按链序拷出到外部缓冲。
 *   参数：cmd —— 已完成的命令；buffer —— 输出；length —— 字节数。
 *   返回：FTL_OK / FTL_ERROR（链上 desc 无 buffer）。
 *   实现要点（v13 由 FC 上移，与 ftl_cmd_write_data 对称）：
 *     - 遍历界走 desc 链；数据够即停（remaining 归零）。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_cmd_read_data(fc_command_t *cmd, void *buffer, uint32_t length)
{
    fc_descriptor_id_t desc_id;
    uint8_t           *dst;
    uint32_t           remaining;

    ASSERT(cmd != NULL);
    ASSERT(buffer != NULL);
    ASSERT(cmd->desc_id != FC_DESCRIPTOR_ID_INVALID);

    dst       = (uint8_t *)buffer;
    remaining = length;
    desc_id   = (fc_descriptor_id_t)cmd->desc_id;

    while ((desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (remaining > 0U))
    {
        fc_descriptor_t *descriptor = fc_descriptor_id_to_descriptor(desc_id);
        uint8_t         *buf;
        uint32_t         copy_len;

        if (descriptor->buffer_id == FC_BUFFER_ID_INVALID)
        {
            return FTL_ERROR;
        }
        buf = fc_buffer_id_to_real_buffer(descriptor->buffer_id);
        if (buf == NULL)
        {
            return FTL_ERROR;
        }

        copy_len = (remaining >= FC_BUFFER_USER_SIZE) ? FC_BUFFER_USER_SIZE : remaining;
        (void)memcpy(dst, buf, copy_len);
        dst += copy_len;
        remaining -= copy_len;

        desc_id = (fc_descriptor_id_t)descriptor->next_id;
    }

    return FTL_OK;
}

/* ── 内部同步 NAND 操作 ── */

/*
 * 同步等待指定 command_id 完成。
 *
 * 使用 completion_table 统一路由：
 *   ① 注册 ftl_sync_waiter_handler 到 completion_table
 *   ② 循环 fc_service() + completion_table_drain() 直到 done
 *   ③ 无需处理 "other" completion — completion_table_drain 会自动路由
 *     非目标 completion 到 default handler（ftl_handle_completion）
 *
 * @param cmd_id      要等待的 command_id
 * @param status_out  输出 completion status
 * @param timeout_us  超时（0 = 使用默认 10ms）
 * @return 0 完成, -1 超时
 */
ftl_status_t ftl_internal_sync_wait(uint16_t cmd_id, fc_command_status_t *status_out, uint32_t timeout_us)
{
    ftl_sync_waiter_t waiter;
    uint32_t          elapsed  = 0U;
    uint32_t          max_wait = (timeout_us > 0U) ? timeout_us : 10000U;

    waiter.cmd_id = cmd_id;
    waiter.done   = 0U;

    if (completion_table_add(cmd_id, ftl_sync_waiter_handler, &waiter) != 0)
    {
        return FTL_ERROR; /* 路由表满 */
    }

    for (;;)
    {
        (void)fc_service();
        completion_table_drain();

        if (waiter.done != 0U)
        {
            break;
        }

        /* 简易超时：elapsed 递增（精确度取决于 fc_service 调用频率） */
        elapsed++;
        if (elapsed > max_wait)
        {
            completion_table_remove(cmd_id);
            return FTL_ERROR;
        }
    }

    if (status_out != NULL)
    {
        *status_out = waiter.completion;
    }

    return FTL_OK;
}

/* ── 页级同步原语（18 份内联 → 4 份公共）──
 *
 * 四个函数共用同一模板：alloc → 填配置 → addr_to_cmd（建链）→ 可选搬数据 →
 * submit → sync_wait → 判 NAND 成败 → release。差异只在：
 *   · operation（PROGRAM / READ / ERASE）
 *   · 配置（crc / random / ldpc / module / lba）
 *   · 数据方向（写：submit 前搬入；读：完成后搬出）
 *   · 是否判 NAND 失败（RAW 读刻意不判）
 *   · 超时（擦除 20ms，其余默认）
 */

/* 分配命令 + 填配置 + 建链。失败返回 NULL（命令已回收）。
 * v23：不再填 config.cw —— 建链只认 cw_bitmap。
 * v27：cw_bitmap 是字面位图（无隐式展开）；v28 起由 addr_to_cmd 算出「本命令 plane
 *      段内全部 CW」并作实参传给建链入口，不再存进命令结构体。 */
static fc_command_t *sync_page_prepare(nand_addr_t addr, uint8_t operation, const ftl_page_cfg_t *cfg)
{
    fc_command_t *cmd = fc_command_alloc();

    if (cmd == NULL)
    {
        return NULL;
    }

    cmd->config.operation   = (DWORD)operation;
    cmd->config.crc         = (DWORD)cfg->crc;
    cmd->config.random      = (DWORD)cfg->random;
    cmd->config.ldpc        = (DWORD)cfg->ldpc;
    cmd->config.multi_plane = 0U;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    cmd->module             = (DWORD)cfg->module;
    cmd->meta               = (DWORD)(cfg->lba << CW_WIDTH); /* 页 LBA → 扇区 LBA（v24） */

    if (addr_to_cmd(cmd, &addr) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return NULL;
    }

    return cmd;
}

/* 提交并等待完成。返回 0 = 命令完成（不论 NAND 成败）；-1 = 提交或等待失败。
 * ⚠ 不判定 NAND_FAIL —— RAW 读刻意忽略它（坏页也要拿原始数据），
 *    故成败判定留在调用者。命令不在此释放。 */
static int sync_page_submit_wait(fc_command_t *cmd, fc_command_status_t *completion, uint32_t timeout_us)
{
    if (fc_submit(cmd) != FC_STATUS_OK)
    {
        return -1;
    }
    if (ftl_internal_sync_wait(ftl_cmd_id(cmd), completion, timeout_us) != 0)
    {
        return -1;
    }
    return 0;
}

ftl_status_t ftl_sync_write_page(nand_addr_t addr, const void *data, uint32_t length, const ftl_page_cfg_t *cfg)
{
    fc_command_t       *cmd;
    fc_command_status_t completion;
    int                 ret;

    cmd = sync_page_prepare(addr, OP_NORMAL_PROGRAM_PAGE, cfg);
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    if ((data != NULL) && (length > 0U))
    {
        (void)ftl_cmd_write_data(cmd, data, length);
    }

    ret = sync_page_submit_wait(cmd, &completion, 0U);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }
    if ((completion.status & FC_CW_STATUS_NAND_FAIL) != 0U)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    (void)fc_command_release(cmd);
    return FTL_OK;
}

ftl_status_t ftl_sync_read_page(nand_addr_t addr, void *buffer, uint32_t length, const ftl_page_cfg_t *cfg)
{
    fc_command_t       *cmd;
    fc_command_status_t completion;
    int                 ret;

    cmd = sync_page_prepare(addr, OP_NORMAL_READ_PAGE_WITH_DATA_OUT, cfg);
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    ret = sync_page_submit_wait(cmd, &completion, 0U);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }
    if ((completion.status & FC_CW_STATUS_NAND_FAIL) != 0U)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    if ((buffer != NULL) && (length > 0U))
    {
        (void)ftl_cmd_read_data(cmd, buffer, length);
    }

    (void)fc_command_release(cmd);
    return FTL_OK;
}

ftl_status_t ftl_sync_read_page_raw(nand_addr_t addr, void *buffer, uint8_t module)
{
    const ftl_page_cfg_t cfg = {FC_CRC_DISABLE, FC_RANDOM_DISABLE, FC_ECC_MODE_LDPC_RAW, module, 0U};
    fc_command_t        *cmd;
    fc_command_status_t  completion;
    uint8_t              cw;

    cmd = sync_page_prepare(addr, OP_NORMAL_READ_PAGE_WITH_DATA_OUT, &cfg);
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    if (sync_page_submit_wait(cmd, &completion, 0U) != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    /* 不判 NAND 失败：整页 4 个 CW 无条件拷出 */
    for (cw = 0U; cw < CW_NUM; cw++)
    {
        uint8_t *src = fc_get_cmd_data_ptr(cmd, cw);

        if (src != NULL)
        {
            memcpy((uint8_t *)buffer + (uint32_t)cw * FC_BUFFER_POOL_CW_SIZE, src, FC_BUFFER_USER_SIZE);
        }
    }

    (void)fc_command_release(cmd);
    return FTL_OK;
}

ftl_status_t ftl_sync_erase_block(nand_addr_t addr)
{
    const ftl_page_cfg_t cfg = {FC_CRC_DISABLE, FC_RANDOM_DISABLE, FC_ECC_MODE_BCH, FC_COMMAND_MODULE_METADATA, 0U};
    fc_command_t        *cmd;
    fc_command_status_t  completion;
    int                  ret;

    cmd = sync_page_prepare(addr, OP_ERASE_BLOCK, &cfg);
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    ret = sync_page_submit_wait(cmd, &completion, 20000U);
    if (ret != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }
    if ((completion.status & FC_CW_STATUS_NAND_FAIL) != 0U)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    (void)fc_command_release(cmd);
    return FTL_OK;
}
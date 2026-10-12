/*
 * ftl_fc_interface.c — FTL↔FC 边界（建链 / 数据搬运 / 页级同步原语）
 *
 * 职责：
 *   FTL 侧唯一接触 FC 命令层的文件——把「页/块操作」翻译成 FC 命令：组装 desc 链、
 *   在 desc 链与外部缓冲之间搬数据、同步提交等待，并给出页级同步原语
 *   （写 / 读 / RAW 读 / 擦除 / 读 META / 读列）。
 *
 * 与 fc/ 的分工（v13 策略层上移）：
 *   fc/       —— 机制层：命令池、desc 池、buffer 池、CQ、硬件搬运；
 *   本文件    —— 策略层：**建链规则**（哪些 CW、行地址怎么排）与**数据布局**
 *               （哪段数据进哪个 CW）。
 *   ⚠ 硬件 FCC 不提供「按 CW 拷贝」寄存器，数据由固件在 desc 链上逐 CW 填 / 取。
 *
 * 资源模型：
 *   desc 链与 buffer 由 FC 池分配；建链失败（池空）本层自己回滚整条链
 *   （fc_descriptor_chain_release_all），调用者只负责还命令槽位。
 *   池空是**瞬态背压** ⇒ FTL_AGAIN（上层排队重试），非永久失败。
 *
 * 文件结构：
 *   1. 建链            ftl_cmd_resolve_cw_bitmap（static）/ ftl_cmd_configure_descriptors
 *   2. 命令地址入口    ftl_cmd_setup_ppa / ftl_cmd_setup_ppa_pair
 *   3. 数据搬运        ftl_cmd_write_data / ftl_cmd_read_data
 *   4. 内部同步等待    ftl_internal_sync_wait
 *   5. 页级同步原语    sync_page_prepare / sync_page_submit_wait（static）
 *                      + ftl_sync_write_page / ftl_sync_read_page / ftl_sync_read_page_raw
 *                        / ftl_sync_erase_block / ftl_sync_read_meta / ftl_sync_read_column
 *
 * ⚠ 地址空间：进 FC 的 PBA 必须合法（fc.c 拿 ch/ce/lun 当数组下标）；PBA→PAA 在本层
 *   一次转出，FC 层只见 PAA（v47）。
 */

#include "ftl/ftl_fc_interface.h"

#include "common/assertion.h"
#include "fc/fc.h"
#include "ftl/completion_table.h"
#include "ftl/ftl_type.h"
#include "nand/onfi_1_0.h"

#include <string.h>

/* ── 1. 建链（desc 链组装）── */

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
 *     - 行地址所有 plane 共用 cmd->paa（同页语义）；两 plane 不同块（Policy B）
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
        /* 行地址逐 plane 换 plane 位：PAA 的 plane 与 cw 不相邻，不能再用 PBA 的
         * plane_cw 掩码技巧（cmd->paa 已由调用者/上层入口转好）。 */
        const nand_paa_t row_paa = paa_with_plane(cmd->paa, plane);

        for (cw = 0U; cw < CW_NUM; cw++)
        {
            if ((mask & ((uint32_t)1U << ((uint32_t)plane * CW_NUM + cw))) == 0U)
            {
                continue;
            }
            if (fc_command_configure_descriptor(cmd, plane, cw, row_paa) != FC_STATUS_OK)
            {
                cmd->desc_id = fc_descriptor_chain_release_all(cmd->desc_id);
                return FTL_ERROR;
            }
        }
    }

    return FTL_OK;
}

/* ── 2. 命令地址入口（PBA → PAA + 拓扑）── */

/* --------------------------------------------------------------------
 * ftl_cmd_setup_ppa
 *   功能：单 plane 入口——把 PBA 拆成拓扑（ch/ce/lun）与行地址（PAA），并建整段 desc 链。
 *   参数：cmd —— 命令（config.operation 须已设）；pba —— 目标物理块地址（须合法）。
 *   返回：FTL_OK / FTL_ERROR（cmd 为 NULL / 建链失败）。
 *   实现要点：v47 PBA→PAA 在本层一次转出，FC 层不再见 PBA；位图取「本命令 plane 段内
 *             全部 CW」（整段语义），须在 paa 写好之后算。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_cmd_setup_ppa(fc_command_t *cmd, nand_pba_t pba)
{
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    /* 边界不变量：**进 FC 的 PBA 必须是合法 PBA**。
     * fc.c 的 fc_poll_fill_queue 把 config.ch/ce/lun 当数组下标直接索引
     * suspended[][][]（越界 = 栈越界写），它靠本断言保证值域——那里注释写的
     * 「PBA 已过 pba_is_valid() 校验」指的就是这一行。
     * 调用者给不出合法 PBA = 编程错误 ⇒ ASSERT（§十三），不返回码。 */
    ASSERT(pba_is_valid(pba));

    cmd->config.ch  = (DWORD)pba_to_ch(pba);
    cmd->config.ce  = (DWORD)pba_to_ce(pba);
    cmd->config.lun = (DWORD)pba_to_lun(pba);
    cmd->paa        = pba_to_paa(pba); /* v47：PBA 在此转 PAA 一次，FC 层不再见 PBA */
    /* v27/v28：位图是字面位图（0 = 空集），「整段访问」不再有隐式默认值。本入口语义
     * 就是「整段」——自己算好当实参传，调用者不必管。须在 paa 写好之后算。 */
    return ftl_cmd_configure_descriptors(cmd, fc_command_plane_span_cw_bitmap(cmd));
}

/* --------------------------------------------------------------------
 * ftl_cmd_setup_ppa_pair
 *   功能：双 plane 入口（Policy B：两 plane 块号不同）——逐 plane 取各自行地址建整条 desc 链。
 *   参数：cmd —— 命令；pba[PLANE_NUM] —— 各 plane 的 PBA（单 plane 命令可只填 pba[0]）。
 *   返回：FTL_OK / FTL_AGAIN（desc/buffer 池空，瞬态背压）/ FTL_ERROR。
 *   实现要点：拓扑取 pba[0]（两 plane 必同 ch/ce/lun）；plane 段与位图展开规则与
 *             ftl_cmd_configure_descriptors **完全一致**（同一个 ftl_cmd_resolve_cw_bitmap），
 *             唯一差别是行地址**逐 plane 各自取**。
 * -------------------------------------------------------------------- */
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

    /* 边界不变量：同 ftl_cmd_setup_ppa —— 进 FC 的 PBA 必须合法（ch/ce/lun 被当
     * suspended[][][] 下标用）。只断言**本命令实际用到**的元素：单 plane 命令的
     * 调用者按约定可让 pba[1] 保持未初始化（见 ftl_issue_subop 的 pba 局部数组）。 */
    ASSERT(pba_is_valid(pba[0]));

    /* 拓扑取 pba[0]（同一命令两 plane 必属同一 ch/ce/lun）；command->paa 写 pba[0] 转出的 PAA——
     * 它是 fc_command_plane_span 定 plane 段的输入，各 plane 的实际行地址逐条由 row 决定。 */
    cmd->config.ch  = (DWORD)pba_to_ch(pba[0]);
    cmd->config.ce  = (DWORD)pba_to_ce(pba[0]);
    cmd->config.lun = (DWORD)pba_to_lun(pba[0]);
    cmd->paa        = pba_to_paa(pba[0]); /* v47：PBA→PAA 在 FTL 侧一次转出 */

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
        ASSERT(pba_is_valid(pba[i])); /* 逐 plane 的行地址都由它转出，越界位会静默算错地址 */
        const uint8_t    plane   = (uint8_t)pba_to_plane(pba[i]);
        const nand_paa_t row_paa = pba_to_paa(pba[i]); /* v47：PBA→PAA 在 FTL 侧转出 */
        uint8_t          cw;

        for (cw = 0U; cw < CW_NUM; cw++)
        {
            fc_status_t st;

            if ((mask & ((uint32_t)1U << ((uint32_t)plane * CW_NUM + cw))) == 0U)
            {
                continue;
            }
            st = fc_command_configure_descriptor(cmd, plane, cw, row_paa);
            if (st != FC_STATUS_OK)
            {
                /* 回滚整条链（desc + buffer 还池，desc_id 复位）；命令槽位由调用者释放 */
                cmd->desc_id = fc_descriptor_chain_release_all(cmd->desc_id);
                /* 池空（desc/buffer）是**瞬态背压**——大请求拆多子操作时上层据此排队重试，
                 * 而非整体失败。其余（不可达：配置错误已 ASSERT）按永久失败。 */
                return (st == FC_STATUS_RESOURCE_EMPTY) ? FTL_AGAIN : FTL_ERROR;
            }
        }
    }

    return FTL_OK;
}

/* ── 3. 数据搬运（desc 链 ↔ 外部缓冲）── */

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

/* ── 4. 内部同步等待 ── */

/* --------------------------------------------------------------------
 * ftl_internal_sync_wait
 *   功能：同步等待指定 command_id 完成（内部同步 NAND 操作的等待原语）。
 *   参数：cmd_id —— 要等待的命令 id；status_out —— 出参完成状态（可 NULL）；
 *         timeout_us —— 超时（0 = 默认 10ms）。
 *   返回：FTL_OK 完成；FTL_ERROR 超时或路由表满。
 *   实现要点：用 completion_table 统一路由——① 注册 ftl_sync_waiter_handler；
 *             ② 循环 fc_service() + completion_table_drain() 直到 done；
 *             ③ 非目标 completion 由 drain 自动路由到 default handler
 *             （ftl_handle_completion），本函数无需处理。
 * -------------------------------------------------------------------- */
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
 * submit → sync_wait → 判完成状态 → release。差异只在：
 *   · operation（PROGRAM / READ / ERASE）
 *   · 配置（crc / random / ldpc / module / lba）
 *   · 数据方向（写：submit 前搬入；读：完成后搬出）
 *   · 是否判失败位（写/读页/擦除判 FC_CW_STATUS_ERROR_MASK 全集；RAW 读刻意不判）
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
    /* 页 LBA → 扇区 LBA（v24）。FTL_LBA_UNKNOWN（FTL_PAGE_CFG_DATA_ANY）⇒ FC_META_UNKNOWN，
     * 让 FC 侧跳过「页内 META 与本命令 LBA 比对」那一项。 */
    cmd->meta               = (cfg->lba == (lba_t)FTL_LBA_UNKNOWN) ? (DWORD)FC_META_UNKNOWN
                                                                   : (DWORD)(cfg->lba << CW_WIDTH);

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

/* --------------------------------------------------------------------
 * ftl_sync_write_page
 *   功能：页级同步写——建链 + 搬入数据 + 提交等待 + 判 NAND 失败。
 *   参数：addr —— 目标页地址；data/length —— 源数据（可为 NULL/0 = 纯建链不搬）；
 *         cfg —— 页配置（crc/random/ldpc/module/lba）。
 *   返回：FTL_OK / FTL_ERROR（建链失败 / 提交等待失败 / NAND_FAIL）。
 * -------------------------------------------------------------------- */
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

/* --------------------------------------------------------------------
 * ftl_sync_read_page
 *   功能：页级同步读——建链 + 提交等待 + 判错 + 搬出数据。
 *   参数：addr —— 目标页地址；buffer/length —— 输出缓冲；cfg —— 页配置。
 *   返回：FTL_OK / FTL_ERROR（建链失败 / 提交等待失败 / 判错位命中）。
 *   实现要点：失败判据用 FC_CW_STATUS_ERROR_MASK 全集（理由见下方行内 ⚠）。
 * -------------------------------------------------------------------- */
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
    /* 失败判据 = FC_CW_STATUS_ERROR_MASK 全集（与 fc_command.h 的链级判据同口径）。
     * **只认 NAND_FAIL 是错的**：UNC 与 MATA_ERROR 会让本页内容不可信却照常拷出——
     *   · UNC：fc_data_path_read 在任何 CW 不可纠时**提前返回**，buffer 停在
     *     "ECC 半修正 + 未解扰" 态（Pass 2/3/4 全跳过），拷出来的是加扰垃圾；
     *   · MATA_ERROR：页内 META 与预期不符 = **读到了别的页**（或页已擦除）。
     * 后果实例：L2P region 页读回垃圾 → l2p_cache_find_or_load 标 CLEAN → 垃圾 PBA
     * 被 flush 写回介质**永久化**，读路径再拿它当 ch/ce/lun 路由字段用。
     * 空页（哨兵层）也会命中 MATA_ERROR ⇒ 返回 FTL_ERROR ⇒ 调用者保持 entries
     * 全 FTL_PBA_INVALID，与"未映射"语义一致（这正是各调用点的既有兜底分支）。 */
    if ((completion.status & FC_CW_STATUS_ERROR_MASK) != 0U)
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

/* --------------------------------------------------------------------
 * ftl_sync_read_page_raw
 *   功能：RAW 读——LDPC_RAW 跳过 ECC 拿原始数据，4 个 CW 无条件拷出。
 *   参数：addr —— 目标页地址；buffer —— 输出（须 >= PAGE_SIZE）；module —— 调用方模块标识。
 *   返回：FTL_OK / FTL_ERROR（建链失败 / 提交等待失败）。
 *   实现要点：**刻意不判 NAND 失败**——坏页/空页也要拿原始数据（判空、判出厂坏块都靠它）。
 * -------------------------------------------------------------------- */
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

/* --------------------------------------------------------------------
 * ftl_sync_erase_block
 *   功能：块级同步擦除——建链（无数据）+ 提交等待 + 判 NAND 失败。
 *   参数：addr —— 目标块地址（page/plane 字段被 FC 侧忽略，只取 block）。
 *   返回：FTL_OK / FTL_ERROR（建链失败 / 提交等待失败 / NAND_FAIL）。
 *   实现要点：擦除慢，超时放宽到 20ms（其余原语用默认 10ms）。
 * -------------------------------------------------------------------- */
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

/* --------------------------------------------------------------------
 * ftl_sync_read_meta
 *   功能：读一页的 4 字节 META（页内 META 字段，不经 desc 链）。
 *   参数：addr —— 目标页地址；out[4] —— 输出 4 字节 META。
 *   返回：FTL_OK / FTL_ERROR（out 为 NULL / 建链失败 / 提交等待失败）。
 *   实现要点：META 走控制器侧暂存，完成时已拷进 CQ 的 param；**刻意不判 NAND_FAIL**
 *             ——坏页的 META 也要拿到（判坏块正是要看它）。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_sync_read_meta(nand_addr_t addr, uint8_t out[4])
{
    const ftl_page_cfg_t cfg = {FC_CRC_DISABLE, FC_RANDOM_DISABLE, FC_ECC_MODE_BCH, FC_COMMAND_MODULE_METADATA, 0U};
    fc_command_t        *cmd;
    fc_command_status_t  completion;

    if (out == NULL)
    {
        return FTL_ERROR;
    }

    /* OP_READ_META 的 has_data = 0 ⇒ ftl_cmd_needs_desc_chain 短路：addr_to_cmd 里的
     * ftl_cmd_configure_descriptors 不建链、也不看位图（desc_id 置 INVALID）。
     * 但地址仍须经 addr_to_cmd 落进 cmd->paa —— FC 侧据 paa 的 cw 字段算列地址。 */
    cmd = sync_page_prepare(addr, OP_READ_META, &cfg);
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    if (sync_page_submit_wait(cmd, &completion, 0U) != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    /* 4 字节 META 走控制器侧暂存（同 GET_FEATURES），完成时已拷进 CQ 的 param。
     * 刻意不判 NAND_FAIL：坏页的 META 也要拿到——判坏块正是要看它。 */
    (void)memcpy(out, completion.param.bytes, 4U);

    (void)fc_command_release(cmd);
    return FTL_OK;
}

/* --------------------------------------------------------------------
 * ftl_sync_read_column
 *   功能：按任意列偏移读 4 字节（读 OOB/spare 区，判出厂坏块用）。
 *   参数：addr —— 目标页地址；column —— 列偏移；out[4] —— 输出 4 字节。
 *   返回：FTL_OK / FTL_ERROR（out 为 NULL / 列越界 ASSERT / 建链失败 / 提交等待失败）。
 *   实现要点：列地址走命令级 param 字节 0/1（小端）；**刻意不判 NAND_FAIL**——坏块
 *             spare 区也要拿到。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_sync_read_column(nand_addr_t addr, uint16_t column, uint8_t out[4])
{
    const ftl_page_cfg_t cfg = {FC_CRC_DISABLE, FC_RANDOM_DISABLE, FC_ECC_MODE_BCH, FC_COMMAND_MODULE_BBT, 0U};
    fc_command_t        *cmd;
    fc_command_status_t  completion;

    if (out == NULL)
    {
        return FTL_ERROR;
    }
    /* 列 + 4 越出页尾是调用者编程错误（本仓首次允许任意列，故必须拦）。 */
    ASSERT((uint32_t)column + 4U <= (uint32_t)PAGE_SIZE);

    /* OP_READ_COLUMN 的 has_data = 0 ⇒ 同 ftl_sync_read_meta：addr_to_cmd 不建链、
     * 不看位图（desc_id 置 INVALID）。地址仍须落进 cmd->paa —— 但**只提供行地址**，
     * cw 位被 NAND 侧 paa_to_nand_row 丢掉；列地址由下面的 param 字节 0/1 给。 */
    cmd = sync_page_prepare(addr, OP_READ_COLUMN, &cfg);
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    /* 列地址走命令级 param 的字节 0/1（小端）——FC 侧解出后落进 request->column。
     * 字节 2/3 本 op 不用，显式清 0 保持 param 确定性。 */
    cmd->param.parameter0 = (uint8_t)column;
    cmd->param.parameter1 = (uint8_t)(column >> 8U);
    cmd->param.parameter2 = 0U;
    cmd->param.parameter3 = 0U;

    if (sync_page_submit_wait(cmd, &completion, 0U) != 0)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    /* 4 字节走控制器侧暂存（同 OP_READ_META），完成时已拷进 CQ 的 param。
     * 刻意不判 NAND_FAIL：坏块的 spare 区也要拿到——判出厂坏块正是要看它。 */
    (void)memcpy(out, completion.param.bytes, 4U);

    (void)fc_command_release(cmd);
    return FTL_OK;
}
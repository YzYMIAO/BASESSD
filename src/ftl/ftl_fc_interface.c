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
 *   4. 同步提交等待    ftl_sync_submit_wait（ftl_op 机制的客户端）
 *   5. 页级同步原语    sync_page_prepare / sync_page_prepare_cw（static）
 *                      + ftl_sync_write_page / ftl_sync_read_page / ftl_sync_read_page_raw
 *                        / ftl_sync_erase_block / ftl_sync_read_meta / ftl_sync_read_column
 *   6. 页级异步原语    ftl_async_write_page / ftl_async_read_page / ftl_async_erase_block
 *                      / ftl_async_set_features —— 只建链（+ 写则搬数据），**不提交**
 *
 * ⚠ 地址空间：进 FC 的 PBA 必须合法（fc.c 拿 ch/ce/lun 当数组下标）；PBA→PAA 在本层
 *   一次转出，FC 层只见 PAA（v47）。
 */

#include "ftl/ftl_fc_interface.h"

#include "common/assertion.h"
#include "fc/fc.h"
#include "ftl/completion_table.h"
#include "ftl/ftl_op.h" /* ftl_op_submit / FTL_OP_SYNC_WAIT */
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
 *       pba_to_cmd / ftl_cmd_setup_ppa / ftl_cmd_setup_ppa_pair。
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

    /* multi-plane 命令的位图必须**覆盖段内每一个 plane**——NAND 侧多 plane handler 无条件
     * 遍历 PLANE_NUM 下行地址，未覆盖 plane 会被凭空造出回退地址（见 fc.c 的
     * fc_build_request_data_planes）。CW 级映射下「一条子操作只覆盖半个 pair」是常态，
     * 那种情况必须用**单 plane op**（multi_plane = 0），不能用 multi-plane 配窄位图。
     * 此处是**上游**守卫：FC 侧还有一道 release 拒绝（INVALID_ARGUMENT），两道都在，
     * 因为本函数在 debug 下会先炸、给出比"命令静默失败"更清楚的现场。 */
    if (plane_count > 1U)
    {
        uint8_t p;
        for (p = 0U; p < plane_count; p++)
        {
            const uint32_t plane_bits = bitmap & ((uint32_t)((1U << CW_NUM) - 1U) << ((uint32_t)(plane_first + p) * CW_NUM));
            ASSERT(plane_bits != 0U);
        }
    }
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
/* --------------------------------------------------------------------
 * ftl_cmd_build_pair_chain
 *   功能：按**显式** cw_bitmap 逐 plane 建 desc 链（Policy B：各 plane 行地址独立）。
 *   参数：cmd —— 命令（config.multi_plane 与 paa 须已写好，段由它们决定）；
 *         pba —— 各 plane 的 PBA（**只校验本命令实际置位的 plane**）；
 *         cw_bitmap —— flat 字面位图（bit = plane*CW_NUM + cw），须落在 plane 段内。
 *   返回：FTL_OK / FTL_AGAIN（desc/buffer 池空，瞬态背压）/ FTL_ERROR。
 *   实现要点：**未置位的 plane 整体跳过**——连 pba_is_valid 都不判。CW 级映射下一条
 *             子操作常只覆盖半个 pair（如只写 plane1 的 4 个 CW），此时调用者按约定
 *             可让另一 plane 的 pba 保持未初始化（见 ftl_issue_subop 的 pba 局部数组）。
 *             位图规则与 ftl_cmd_configure_descriptors 共用 ftl_cmd_resolve_cw_bitmap。
 * -------------------------------------------------------------------- */
static ftl_status_t ftl_cmd_build_pair_chain(fc_command_t *cmd, const nand_pba_t pba[PLANE_NUM], uint32_t cw_bitmap)
{
    uint32_t mask;
    uint8_t  plane_count;
    uint8_t  plane_first;
    uint8_t  plane_last;
    uint8_t  i;

    plane_count = fc_command_plane_span(cmd, &plane_first, &plane_last);
    mask        = ftl_cmd_resolve_cw_bitmap(cw_bitmap, plane_first, plane_count);

    for (i = 0U; i < plane_count; i++)
    {
        /* ⚠ plane 号取自**段内偏移**（plane_first + i），与位图的绝对编号一致；
         *   pba 的下标一律用 **plane 号**（不是 i）——单 plane 命令的 plane_first 可能是 1
         *   （paa 的 plane 位决定），此时 i==0 而 plane==1，用 pba[i] 会拿到 plane0 的行地址。 */
        const uint8_t plane       = (uint8_t)(plane_first + i);
        /* 本 plane 段内**局部** CW 位（右移到 bit0 起）——下方内层用 `1U << cw` 逐位测，
         * 必须与局部下标对齐。⚠ 若在此保留**绝对**位（`mask & (0xF << plane*CW_NUM)`），
         * plane ≥ 1 时高位落在 bit4..7，而 cw 只有 0..3 ⇒ 该 plane 恒判「无 CW」静默不建链
         * ⇒ multi-plane 命令只覆盖 plane 0 ⇒ fc.c 的 planes_with_desc ASSERT。 */
        const uint32_t plane_bits = (mask >> ((uint32_t)plane * CW_NUM)) & ((uint32_t)((1U << CW_NUM) - 1U));
        uint8_t        cw;

        if (plane_bits == 0U)
        {
            continue; /* 本 plane 无 CW：不建链、不校验 PBA */
        }

        ASSERT(pba_is_valid(pba[plane]));                  /* 逐 plane 的行地址都由它转出，越界位会静默算错地址 */
        const nand_paa_t row_paa = pba_to_paa(pba[plane]); /* v47：PBA→PAA 在 FTL 侧转出 */

        for (cw = 0U; cw < CW_NUM; cw++)
        {
            fc_status_t st;

            if ((plane_bits & ((uint32_t)1U << cw)) == 0U)
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

/* 写拓扑（ch/ce/lun）与行地址基址（cmd->paa = base_pba 的 PAA）——不含建链。
 *
 * base_pba 必须是**本命令实际访问的第一个 plane**的 PBA：`fc_command_plane_span` 用
 * cmd->paa 的 plane 位定单 plane 命令的 plane 段，给错了段就错。整段入口（两 plane
 * 都在）传 pba[0]；CW 级入口只覆盖 plane1 时须传 pba[1]（见 ftl_cmd_setup_ppa_pair_cw）。 */
static void ftl_cmd_bind_pair_topo(fc_command_t *cmd, nand_pba_t base_pba)
{
    cmd->config.ch  = (DWORD)pba_to_ch(base_pba);
    cmd->config.ce  = (DWORD)pba_to_ce(base_pba);
    cmd->config.lun = (DWORD)pba_to_lun(base_pba);
    cmd->paa        = pba_to_paa(base_pba); /* v47：PBA→PAA 在 FTL 侧一次转出 */
}

ftl_status_t ftl_cmd_setup_ppa_pair(fc_command_t *cmd, const nand_pba_t pba[PLANE_NUM])
{
    if ((cmd == NULL) || (pba == NULL))
    {
        return FTL_ERROR;
    }

    /* 边界不变量：同 ftl_cmd_setup_ppa —— 进 FC 的 PBA 必须合法（ch/ce/lun 被当
     * suspended[][][] 下标用）。只断言**本命令实际用到**的元素：单 plane 命令的
     * 调用者按约定可让 pba[1] 保持未初始化（见 ftl_issue_subop 的 pba 局部数组）。 */
    ASSERT(pba_is_valid(pba[0]));
    ftl_cmd_bind_pair_topo(cmd, pba[0]);

    if (ftl_cmd_needs_desc_chain(cmd) == 0U)
    {
        cmd->desc_id = FC_DESCRIPTOR_ID_INVALID;
        return FTL_OK;
    }

    /* v27/v28：位图是字面位图（0 = 空集），本入口语义就是「整段」——自己算好当实参
     * （须在 paa 写好之后算，见 fc_command_plane_span_cw_bitmap）。 */
    return ftl_cmd_build_pair_chain(cmd, pba, fc_command_plane_span_cw_bitmap(cmd));
}

ftl_status_t ftl_cmd_setup_ppa_pair_cw(fc_command_t *cmd, const nand_pba_t pba[PLANE_NUM], uint32_t cw_bitmap)
{
    uint8_t first_plane = PLANE_NUM; /* 本命令实际覆盖的第一个 plane（= 段首） */
    uint8_t p;

    if ((cmd == NULL) || (pba == NULL))
    {
        return FTL_ERROR;
    }

    /* 基址 PBA 必须取自**被覆盖的 plane**，不是无条件的 pba[0]：CW 级子操作常只覆盖
     * 半个 pair（例如一条 512B 写只落 plane1），此时 pba[0] 按约定未初始化，
     * 拿它定段会把单 plane 命令的段定成 plane0 ⇒ 位图越段 ASSERT 或静默空链。 */
    for (p = 0U; p < PLANE_NUM; p++)
    {
        if ((cw_bitmap & ((uint32_t)((1U << CW_NUM) - 1U) << ((uint32_t)p * CW_NUM))) != 0U)
        {
            first_plane = p;
            break;
        }
    }
    ASSERT(first_plane < PLANE_NUM); /* 空位图 = 调用者 bug（ftl_cmd_resolve_cw_bitmap 也会拦） */

    ASSERT(pba_is_valid(pba[first_plane]));
    ftl_cmd_bind_pair_topo(cmd, pba[first_plane]);

    if (ftl_cmd_needs_desc_chain(cmd) == 0U)
    {
        cmd->desc_id = FC_DESCRIPTOR_ID_INVALID;
        return FTL_OK;
    }

    /* CW 级映射入口：位图由调用者给出（可只覆盖本命令实际写到的那些 CW）。
     * cmd->config.multi_plane 决定 plane 段，须由调用者**在建链前**设好。 */
    return ftl_cmd_build_pair_chain(cmd, pba, cw_bitmap);
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
 * ftl_cmd_write_data_cw
 *   功能：CW 级数据填装——按**页内 CW 偏移**从整页镜像取数，只填本命令链上的 CW。
 *   参数：cmd —— 已建链的命令（链上 CW ⊆ cw_bitmap）；page_buf —— 整页镜像
 *         （FTL_READ_USER_SIZE 字节，CW 按偏移排布：CW c 的数据在 page_buf + c×512）；
 *         cw_bitmap —— 本页哪些 CW 有效（flat 字面位图，与本命令链同源）。
 *   返回：FTL_OK / FTL_ERROR（链上 desc 无 buffer）。
 *   实现要点：与 ftl_cmd_write_data 的差别只在**取数方式**——后者按链序**打包**消费
 *             连续源缓冲（适合 host 写：host 数据本来就是紧凑的），本函数按 CW 偏移
 *             **散射**取数（适合 GC/WL 搬移：源是整页镜像，只需搬其中几个 CW，且
 *             搬入位置必须保持原 CW 偏移——页内 META 的 LBA 由 CW 号决定，挪位即错）。
 *             位图外的 CW 若仍在链上（调用者与链不同源）填 0xFF，绝不把镜像里的
 *             无效 CW 数据带进新页。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_cmd_write_data_cw(fc_command_t *cmd, const uint8_t *page_buf, uint32_t cw_bitmap)
{
    fc_descriptor_id_t desc_id;

    ASSERT(cmd != NULL);
    ASSERT(page_buf != NULL);
    ASSERT(cmd->desc_id != FC_DESCRIPTOR_ID_INVALID);

    desc_id = (fc_descriptor_id_t)cmd->desc_id;

    while (desc_id < FC_DESCRIPTOR_POOL_DEPTH)
    {
        fc_descriptor_t *descriptor = fc_descriptor_id_to_descriptor(desc_id);
        uint8_t         *buf;
        /* CW 号取 desc 自带的 config.cw（建链时写入）——不去解 desc->paa 的 PAA 位域：
         * PAA 是 FC/器件域的表示，FTL 层不该碰。 */
        const uint8_t cw = (uint8_t)descriptor->config.cw;

        if (descriptor->buffer_id == FC_BUFFER_ID_INVALID)
        {
            return FTL_ERROR;
        }
        buf = fc_buffer_id_to_real_buffer(descriptor->buffer_id);

        if ((cw_bitmap & ((uint32_t)1U << cw)) != 0U)
        {
            (void)memcpy(buf, page_buf + ((size_t)cw * FC_BUFFER_USER_SIZE), (size_t)FC_BUFFER_USER_SIZE);
        }
        else
        {
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

/* ── 4. 同步提交等待 ── */

/* --------------------------------------------------------------------
 * ftl_sync_submit_wait
 *   功能：提交命令并阻塞等待其完成（同步提交的唯一入口，ftl_op 机制的客户端）。
 *   参数：cmd —— 已建链的命令；status_out —— 出参完成状态（可 NULL）；
 *         timeout_us —— 超时（0 = 默认 10ms）。
 *   返回：FTL_OK 完成；FTL_ERROR 提交失败 / 超时 / 旁表满。
 *   实现要点：① ftl_op_submit(cmd, FTL_OP_SYNC_WAIT, &waiter) —— 打 tag、登记栈上
 *             waiter、提交（登记早于提交）；② 自旋 fc_service() +
 *             completion_table_drain() 直到 waiter.done；③ 超时摘链返回 FTL_ERROR。
 *             ⚠ 非目标 completion 由 drain 按各自的 op_tag 派发，本函数无需处理。
 *             ⚠ **不 release**：调用者要在返回后读 cmd 数据才 release（见 ftl_op.h）。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_sync_submit_wait(fc_command_t *cmd, fc_command_status_t *status_out, uint32_t timeout_us)
{
    ftl_sync_waiter_t waiter;
    uint32_t          elapsed  = 0U;
    uint32_t          max_wait = (timeout_us > 0U) ? timeout_us : 10000U;

    ASSERT(cmd != NULL);

    waiter.cmd_id = ftl_cmd_id(cmd);
    waiter.done   = 0U;

    if (ftl_op_submit(cmd, FTL_OP_SYNC_WAIT, &waiter) != FTL_OK)
    {
        return FTL_ERROR; /* 提交失败（背压/旁表满）——命令未提交，调用者自行 release */
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
            completion_table_remove(waiter.cmd_id);
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
 * 四个函数共用同一模板：alloc → 填配置 → pba_to_cmd（建链）→ 可选搬数据 →
 * submit → sync_wait → 判完成状态 → release。差异只在：
 *   · operation（PROGRAM / READ / ERASE）
 *   · 配置（crc / random / ldpc / lba）
 *   · 数据方向（写：submit 前搬入；读：完成后搬出）
 *   · 是否判失败位（写/读页/擦除判 FC_CW_STATUS_ERROR_MASK 全集；RAW 读刻意不判）
 *   · 超时（擦除 20ms，其余默认）
 */

/* 分配命令 + 填配置（**不含**地址与建链）。池空返回 NULL。
 * v23：不再填 config.cw —— 建链只认 cw_bitmap。
 * v27：cw_bitmap 是字面位图（无隐式展开）；v28 起由 pba_to_cmd 算出「本命令 plane
 *      段内全部 CW」并作实参传给建链入口，不再存进命令结构体。
 *
 * 与地址/建链拆开是为了让异步入口能区分「池空（瞬态背压 ⇒ FTL_AGAIN）」与
 * 「地址非法 / 建链失败（编程错误 ⇒ FTL_ERROR）」——两者在同步原语里都被压成 NULL。 */
static fc_command_t *page_prepare_alloc(uint8_t operation, const ftl_page_cfg_t *cfg)
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
    /* 页 LBA → 扇区 LBA（v24）。FTL_LBA_UNKNOWN（FTL_PAGE_CFG_DATA_ANY）⇒ FC_META_UNKNOWN，
     * 让 FC 侧跳过「页内 META 与本命令 LBA 比对」那一项。 */
    cmd->meta               = (cfg->lba == (lba_t)FTL_LBA_UNKNOWN) ? (DWORD)FC_META_UNKNOWN
                                                                   : (DWORD)(cfg->lba << CW_WIDTH);

    return cmd;
}

/* 分配命令 + 填配置 + 建链。失败返回 NULL（命令已回收）。 */
static fc_command_t *sync_page_prepare(nand_pba_t pba, uint8_t operation, const ftl_page_cfg_t *cfg)
{
    fc_command_t *cmd = page_prepare_alloc(operation, cfg);

    if (cmd == NULL)
    {
        return NULL;
    }

    if (pba_to_cmd(cmd, pba) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return NULL;
    }

    return cmd;
}

/* 同上，但 desc 链只覆盖形参 cw_bitmap 指定的 CW（CW 级搬移写）。
 * 与 pba_to_cmd 的差别只有最后一步的位图实参：地址绑定（ch/ce/lun + PAA）逐字相同，
 * 故这里手工展开 pba_to_cmd 的后半段而不另造一个入口——pba_to_cmd 的语义是「整段」，
 * 重载它会让「整段 vs 子集」这个关键差别从调用点消失。 */
static fc_command_t *sync_page_prepare_cw(nand_pba_t pba, uint8_t operation, const ftl_page_cfg_t *cfg, uint32_t cw_bitmap)
{
    fc_command_t *cmd = page_prepare_alloc(operation, cfg);

    if (cmd == NULL)
    {
        return NULL;
    }

    /* 边界不变量同 pba_to_cmd：进 FC 的 ch/ce/lun 被当 suspended[][][] 下标直接用。 */
    ASSERT(pba_is_valid(pba));
    cmd->config.ch  = (DWORD)pba_to_ch(pba);
    cmd->config.ce  = (DWORD)pba_to_ce(pba);
    cmd->config.lun = (DWORD)pba_to_lun(pba);
    cmd->paa        = pba_to_paa(pba);

    if (ftl_cmd_configure_descriptors(cmd, cw_bitmap) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return NULL;
    }

    return cmd;
}

/* --------------------------------------------------------------------
 * ftl_sync_write_page
 *   功能：页级同步写——建链 + 搬入数据 + 提交等待 + 判 NAND 失败。
 *   参数：pba —— 目标页 PBA；data/length —— 源数据（可为 NULL/0 = 纯建链不搬）；
 *         cfg —— 页配置（crc/random/ldpc/lba）。
 *   返回：FTL_OK / FTL_ERROR（建链失败 / 提交等待失败 / NAND_FAIL）。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_sync_write_page(nand_pba_t pba, const void *data, uint32_t length, const ftl_page_cfg_t *cfg)
{
    fc_command_t       *cmd;
    fc_command_status_t completion;

    cmd = sync_page_prepare(pba, OP_NORMAL_PROGRAM_PAGE, cfg);
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    if ((data != NULL) && (length > 0U))
    {
        (void)ftl_cmd_write_data(cmd, data, length);
    }

    if (ftl_sync_submit_wait(cmd, &completion, 0U) != FTL_OK)
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
 * ftl_sync_write_page_cw
 *   功能：CW 级页写——只把 page_buf 里 cw_bitmap 指定的 CW 编进目标页。
 *   参数：pba —— 目标页 PBA；page_buf —— **整页镜像**（FTL_READ_USER_SIZE 字节，
 *         CW c 的数据在 page_buf + c×512）；cw_bitmap —— 要编的 CW（flat 字面位图，
 *         须落在本命令 plane 段内）；cfg —— 页配置（lba 必须是**本页**的页 LBA）。
 *   返回：FTL_OK / FTL_ERROR（建链失败 / 提交等待失败 / NAND_FAIL）。
 *   实现要点：GC/WL 搬移专用——源是整页镜像，只需搬其中几个有效 CW，且**必须保持
 *             原 CW 偏移**（页内 META 的 LBA 由 CW 号决定，挪位即错）。未置位的 CW
 *             不建链、不编程，读回为 0xFF（META 全 FF ⇒ 上层判无效）。
 *             ⚠ 与 ftl_sync_write_page 的取舍：整页重写会把无效 CW 连同其（陈旧的）
 *             META 一起搬过去——L2P 反查仍能判其无效，但新页 META 被污染且多搬无用
 *             数据；CW 级写只搬真有效的。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_sync_write_page_cw(nand_pba_t pba, const uint8_t *page_buf, uint32_t cw_bitmap, const ftl_page_cfg_t *cfg)
{
    fc_command_t       *cmd;
    fc_command_status_t completion;

    cmd = sync_page_prepare_cw(pba, OP_NORMAL_PROGRAM_PAGE, cfg, cw_bitmap);
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    (void)ftl_cmd_write_data_cw(cmd, page_buf, cw_bitmap);

    if (ftl_sync_submit_wait(cmd, &completion, 0U) != FTL_OK)
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
 *   参数：pba —— 目标页 PBA；buffer/length —— 输出缓冲；cfg —— 页配置。
 *   返回：FTL_OK / FTL_ERROR（建链失败 / 提交等待失败 / 判错位命中）。
 *   实现要点：失败判据用 FC_CW_STATUS_ERROR_MASK 全集（理由见下方行内 ⚠）。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_sync_read_page(nand_pba_t pba, void *buffer, uint32_t length, const ftl_page_cfg_t *cfg)
{
    fc_command_t       *cmd;
    fc_command_status_t completion;

    cmd = sync_page_prepare(pba, OP_NORMAL_READ_PAGE_WITH_DATA_OUT, cfg);
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    if (ftl_sync_submit_wait(cmd, &completion, 0U) != FTL_OK)
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
     * 全 NAND_PBA_INVALID，与"未映射"语义一致（这正是各调用点的既有兜底分支）。 */
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
 *   参数：pba —— 目标页 PBA；buffer —— 输出（须 >= PAGE_SIZE）。
 *   返回：FTL_OK / FTL_ERROR（建链失败 / 提交等待失败）。
 *   实现要点：**刻意不判 NAND 失败**——坏页/空页也要拿原始数据（判空、判出厂坏块都靠它）。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_sync_read_page_raw(nand_pba_t pba, void *buffer)
{
    const ftl_page_cfg_t cfg = {FC_CRC_DISABLE, FC_RANDOM_DISABLE, FC_ECC_MODE_LDPC_RAW, 0U};
    fc_command_t        *cmd;
    fc_command_status_t  completion;
    uint8_t              cw;

    cmd = sync_page_prepare(pba, OP_NORMAL_READ_PAGE_WITH_DATA_OUT, &cfg);
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    if (ftl_sync_submit_wait(cmd, &completion, 0U) != FTL_OK)
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
 *   参数：pba —— 目标块 PBA（page/plane 字段被 FC 侧忽略，只取 block）。
 *   返回：FTL_OK / FTL_ERROR（建链失败 / 提交等待失败 / NAND_FAIL）。
 *   实现要点：擦除慢，超时放宽到 20ms（其余原语用默认 10ms）。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_sync_erase_block(nand_pba_t pba)
{
    const ftl_page_cfg_t cfg = {FC_CRC_DISABLE, FC_RANDOM_DISABLE, FC_ECC_MODE_BCH, 0U};
    fc_command_t        *cmd;
    fc_command_status_t  completion;

    cmd = sync_page_prepare(pba, OP_ERASE_BLOCK, &cfg);
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    if (ftl_sync_submit_wait(cmd, &completion, 20000U) != FTL_OK)
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

/* ── 异步页级原语（阶段 2 起）──
 *
 * 与上面 ftl_sync_* 逐字共用 page_prepare_alloc / pba_to_cmd / ftl_cmd_write_data，
 * 差别只在最后一步：**不提交**，把命令交回调用者（由它 ftl_op_submit + 服务函数 release）。
 * 返回码比同步版多一档 FTL_AGAIN：命令池空是瞬态背压，调用者应当**保留现场、稍后重试**，
 * 而同步版把它和"地址非法"一起压成 FTL_ERROR（同步调用者没有重试的地方）。 */

/* --------------------------------------------------------------------
 * ftl_async_write_page
 *   功能：建链 + 搬入数据，**不提交**，命令经 out_cmd 交回。
 *   参数：out_cmd —— 出参命令（非 NULL）；pba —— 目标页；data/length —— 源数据；
 *         cfg —— 页配置。
 *   返回：FTL_OK 命令已备好（调用者须提交或 release）；FTL_AGAIN 命令池空；
 *         FTL_ERROR 入参非法 / 地址非法 / 建链失败 / 搬数据失败。
 *   实现要点：数据在本函数内**已拷进 FC buffer 池**（ftl_cmd_write_data 是 memcpy）
 *             ⇒ 返回后调用者即可复用源缓冲，无需为异步写保留快照。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_async_write_page(fc_command_t **out_cmd, nand_pba_t pba, const void *data, uint32_t length, const ftl_page_cfg_t *cfg)
{
    fc_command_t *cmd;

    if (out_cmd == NULL)
    {
        return FTL_ERROR;
    }

    cmd = page_prepare_alloc(OP_NORMAL_PROGRAM_PAGE, cfg);
    if (cmd == NULL)
    {
        return FTL_AGAIN; /* 命令池空 = 瞬态背压 */
    }

    if (pba_to_cmd(cmd, pba) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    if ((data != NULL) && (length > 0U) && (ftl_cmd_write_data(cmd, data, length) != FTL_OK))
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    *out_cmd = cmd;
    return FTL_OK;
}

/* --------------------------------------------------------------------
 * ftl_async_write_page_cw
 *   功能：CW 级建链 + 按页内偏移散射搬入数据，**不提交**，命令经 out_cmd 交回。
 *   参数：out_cmd —— 出参命令（非 NULL）；pba —— 目标页；page_buf —— **整页镜像**
 *         （PAGE_MAIN_SIZE 字节，CW c 的数据在 page_buf + c×512）；
 *         cw_bitmap —— 要编的 CW（flat 字面位图，须落在本命令 plane 段内）；
 *         cfg —— 页配置（lba 必须是**本页**的页 LBA）。
 *   返回：FTL_OK / FTL_AGAIN（命令池空）/ FTL_ERROR（入参或地址非法 / 建链失败 / 搬数据失败）。
 *   实现要点：与同步版 ftl_sync_write_page_cw 逐字共用 page_prepare_alloc +
 *             手工地址绑定（**不走 pba_to_cmd**：它的语义是「整段访问」，重载它会让
 *             「整段 vs 子集」这个关键差别从调用点消失）+ ftl_cmd_write_data_cw。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_async_write_page_cw(fc_command_t **out_cmd, nand_pba_t pba, const uint8_t *page_buf, uint32_t cw_bitmap, const ftl_page_cfg_t *cfg)
{
    fc_command_t *cmd;

    if ((out_cmd == NULL) || (page_buf == NULL))
    {
        return FTL_ERROR;
    }

    cmd = page_prepare_alloc(OP_NORMAL_PROGRAM_PAGE, cfg);
    if (cmd == NULL)
    {
        return FTL_AGAIN; /* 命令池空 = 瞬态背压 */
    }

    /* 边界不变量同 pba_to_cmd：进 FC 的 ch/ce/lun 被当 suspended[][][] 下标直接用。 */
    ASSERT(pba_is_valid(pba));
    cmd->config.ch  = (DWORD)pba_to_ch(pba);
    cmd->config.ce  = (DWORD)pba_to_ce(pba);
    cmd->config.lun = (DWORD)pba_to_lun(pba);
    cmd->paa        = pba_to_paa(pba);

    if (ftl_cmd_configure_descriptors(cmd, cw_bitmap) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    if (ftl_cmd_write_data_cw(cmd, page_buf, cw_bitmap) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    *out_cmd = cmd;
    return FTL_OK;
}

/* --------------------------------------------------------------------
 * ftl_async_erase_block
 *   功能：建链（无数据），**不提交**，命令经 out_cmd 交回。
 *   参数：out_cmd —— 出参命令（非 NULL）；pba —— 目标块 PBA。
 *   返回：FTL_OK / FTL_AGAIN（命令池空）/ FTL_ERROR（入参或地址非法 / 建链失败）。
 *   实现要点：擦除慢（20ms 超时）是**同步版**的形参；异步版的超时由 FC 层自己的
 *             命令超时与 job watchdog 承担，本函数不设。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_async_erase_block(fc_command_t **out_cmd, nand_pba_t pba)
{
    const ftl_page_cfg_t cfg = {FC_CRC_DISABLE, FC_RANDOM_DISABLE, FC_ECC_MODE_BCH, 0U};
    fc_command_t        *cmd;

    if (out_cmd == NULL)
    {
        return FTL_ERROR;
    }

    cmd = page_prepare_alloc(OP_ERASE_BLOCK, &cfg);
    if (cmd == NULL)
    {
        return FTL_AGAIN;
    }

    if (pba_to_cmd(cmd, pba) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    *out_cmd = cmd;
    return FTL_OK;
}

/* --------------------------------------------------------------------
 * ftl_async_read_page
 *   功能：建链，**不提交**，命令经 out_cmd 交回（数据在完成时才拷出）。
 *   参数：out_cmd —— 出参命令（非 NULL）；pba —— 目标页；cfg —— 页配置。
 *   返回：FTL_OK / FTL_AGAIN（命令池空）/ FTL_ERROR（入参或地址非法 / 建链失败）。
 *   实现要点：与 ftl_async_write_page 的**不对称**——写在本函数内已把数据拷进 FC
 *             buffer 池，读没有：数据留在 desc 链上，服务函数必须在 release **之前**
 *             调 ftl_cmd_read_data 才拿得到（否则命令回收、数据丢失）。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_async_read_page(fc_command_t **out_cmd, nand_pba_t pba, const ftl_page_cfg_t *cfg)
{
    fc_command_t *cmd;

    if (out_cmd == NULL)
    {
        return FTL_ERROR;
    }

    cmd = page_prepare_alloc(OP_NORMAL_READ_PAGE_WITH_DATA_OUT, cfg);
    if (cmd == NULL)
    {
        return FTL_AGAIN; /* 命令池空 = 瞬态背压 */
    }

    if (pba_to_cmd(cmd, pba) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    *out_cmd = cmd;
    return FTL_OK;
}

/* --------------------------------------------------------------------
 * ftl_async_set_features
 *   功能：建 SET_FEATURES 命令（无地址、无数据链），**不提交**。
 *   参数：out_cmd —— 出参命令（非 NULL）；ch/ce/lun —— **完成派发的实例路由键**
 *         （不是地址，见头文件）；feature_addr —— 特性寄存器地址；
 *         mode —— 写进 parameter0 的模式值（读重试电压模式，0 = 正常）。
 *   返回：FTL_OK / FTL_AGAIN（命令池空）/ FTL_ERROR（out_cmd 为 NULL）。
 *   实现要点：**不绑 PBA**——cmd->paa 恒 0（fc_command_alloc 已清零）：特性寄存器是
 *             die 级、无 plane/page 语义，FC 侧 OP_SET_FEATURES 分支也不读它。
 *             但 ch/ce/lun **必须**填——ftl_op_dispatch 靠它们解析归属实例。
 *             其余命令配置逐字沿用原 read_recovery.c 内联版本的字段取值，差别只有"不提交"。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_async_set_features(fc_command_t **out_cmd, uint8_t ch, uint8_t ce, uint8_t lun, uint8_t feature_addr, uint8_t mode)
{
    fc_command_t *cmd;

    if (out_cmd == NULL)
    {
        return FTL_ERROR;
    }

    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return FTL_AGAIN; /* 命令池空 = 瞬态背压 */
    }

    cmd->config.ch          = (DWORD)ch;
    cmd->config.ce          = (DWORD)ce;
    cmd->config.lun         = (DWORD)lun;
    cmd->config.operation   = OP_SET_FEATURES;
    cmd->config.crc         = FC_CRC_DISABLE;
    cmd->config.random      = FC_RANDOM_DISABLE;
    cmd->config.ldpc        = FC_ECC_MODE_BCH;
    cmd->config.multi_plane = 0U;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->meta               = 0U;
    cmd->feature_addr       = feature_addr;
    cmd->param.parameter0   = mode;
    cmd->param.parameter1   = 0U;
    cmd->param.parameter2   = 0U;
    cmd->param.parameter3   = 0U;

    *out_cmd = cmd;
    return FTL_OK;
}

/* --------------------------------------------------------------------
 * ftl_sync_read_meta
 *   功能：读一页的 4 字节 META（页内 META 字段，不经 desc 链）。
 *   参数：pba —— 目标页 PBA；out[4] —— 输出 4 字节 META。
 *   返回：FTL_OK / FTL_ERROR（out 为 NULL / 建链失败 / 提交等待失败）。
 *   实现要点：META 走控制器侧暂存，完成时已拷进 CQ 的 param；**刻意不判 NAND_FAIL**
 *             ——坏页的 META 也要拿到（判坏块正是要看它）。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_sync_read_meta(nand_pba_t pba, uint8_t out[4])
{
    const ftl_page_cfg_t cfg = {FC_CRC_DISABLE, FC_RANDOM_DISABLE, FC_ECC_MODE_BCH, 0U};
    fc_command_t        *cmd;
    fc_command_status_t  completion;

    if (out == NULL)
    {
        return FTL_ERROR;
    }

    /* OP_READ_META 的 has_data = 0 ⇒ ftl_cmd_needs_desc_chain 短路：pba_to_cmd 里的
     * ftl_cmd_configure_descriptors 不建链、也不看位图（desc_id 置 INVALID）。
     * 但地址仍须经 pba_to_cmd 落进 cmd->paa —— FC 侧据 paa 的 cw 字段算列地址。 */
    cmd = sync_page_prepare(pba, OP_READ_META, &cfg);
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    if (ftl_sync_submit_wait(cmd, &completion, 0U) != FTL_OK)
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
 *   参数：pba —— 目标页 PBA；column —— 列偏移；out[4] —— 输出 4 字节。
 *   返回：FTL_OK / FTL_ERROR（out 为 NULL / 列越界 ASSERT / 建链失败 / 提交等待失败）。
 *   实现要点：列地址走命令级 param 字节 0/1（小端）；**刻意不判 NAND_FAIL**——坏块
 *             spare 区也要拿到。
 * -------------------------------------------------------------------- */
ftl_status_t ftl_sync_read_column(nand_pba_t pba, uint16_t column, uint8_t out[4])
{
    const ftl_page_cfg_t cfg = {FC_CRC_DISABLE, FC_RANDOM_DISABLE, FC_ECC_MODE_BCH, 0U};
    fc_command_t        *cmd;
    fc_command_status_t  completion;

    if (out == NULL)
    {
        return FTL_ERROR;
    }
    /* 列 + 4 越出页尾是调用者编程错误（本仓首次允许任意列，故必须拦）。 */
    ASSERT((uint32_t)column + 4U <= (uint32_t)PAGE_SIZE);

    /* OP_READ_COLUMN 的 has_data = 0 ⇒ 同 ftl_sync_read_meta：pba_to_cmd 不建链、
     * 不看位图（desc_id 置 INVALID）。地址仍须落进 cmd->paa —— 但**只提供行地址**，
     * cw 位被 NAND 侧 paa_to_nand_row 丢掉；列地址由下面的 param 字节 0/1 给。 */
    cmd = sync_page_prepare(pba, OP_READ_COLUMN, &cfg);
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

    if (ftl_sync_submit_wait(cmd, &completion, 0U) != FTL_OK)
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
/*
 * ftl_fc_interface.c — FTL↔FC 边界（建链 / 数据搬运 / 页级同步原语）
 *
 * 职责：
 *   FTL 侧唯一接触 FC 命令层的文件——把「页/块操作」翻译成 FC 命令：组装 desc 链、
 *   在 desc 链与外部缓冲之间搬数据、同步提交等待，并给出页级同步原语
 *   （写 / 读 / RAW 读 / 擦除 / 读 META / 读列）。
 *
 * 与 fc/ 的分工：
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
 *   1. 建链            ftl_cmd_validate_cw_bitmap（static）/ ftl_cmd_configure_descriptors
 *   2. 命令地址入口    ftl_cmd_build_pair_chain / ftl_cmd_setup_ppa_pair_cw /
 *                      ftl_cmd_block_ecc_list
 *                      （pba_to_cmd / ftl_cmd_pba 在头文件内联）
 *   3. 数据搬运        ftl_cmd_write_data / ftl_cmd_write_data_cw / ftl_cmd_read_data
 *   4. 同步提交等待    ftl_sync_submit_wait（ftl_op 机制的客户端）
 *   5. 页级同步原语    ftl_cmd_prepare_alloc / ftl_sync_page_prepare（static）+
 *                      ftl_sync_write_page / ftl_sync_read_page / ftl_sync_read_page_raw /
 *                      ftl_page_blank_check / ftl_page_is_blank / ftl_sync_erase_block[_multi_plane] /
 *                      ftl_sync_read_column
 *   6. 元数据镜像写    ftl_async_write_page_mirror / ftl_sync_write_page_mirror
 *   7. 页级异步原语    ftl_async_write_page / ftl_async_write_page_cw / ftl_async_write_pair_cmd /
 *                      ftl_async_erase_block[_multi_plane] / ftl_async_read_page /
 *                      ftl_async_set_features —— 只建链（+ 写则搬数据），**不提交**
 *
 * ⚠ 地址空间：进 FC 的 PBA 必须合法（fc.c 拿 ch/ce/lun 当数组下标）；PBA→PAA 在本层
 *   一次转出，FC 层只见 PAA。
 */

#include "ftl_fc_interface.h"

#include "assertion.h"
#include "bst.h"   /* ftl_pba_gblock（逐块 ECC 记账） */
#include "delay.h" /* delay_time_now_us / delay_time_elapsed_us（真实等待上限） */
#include "fc_interface.h"
#include "ftl_op.h" /* ftl_op_submit / FTL_OP_SYNC_WAIT */
#include "ftl_type.h"
#include "onfi_1_0.h"
#include "watchdog.h" /* storage_wdg_kick：长同步操作的有界喂狗 */

#include <string.h>

// 页级原语的两种常量配置（文件作用域初始化器，H47 允许；局部一律逐字段赋值）。
static const ftl_page_cfg_s s_page_cfg_raw    = {FC_CRC_DISABLE, FC_RANDOM_DISABLE, FC_ECC_MODE_LDPC_RAW, 0U};
static const ftl_page_cfg_s s_page_cfg_normal = {FC_CRC_DISABLE, FC_RANDOM_DISABLE, FC_ECC_MODE_NORMAL, 0U};
// 判空专用：ECC 开（空/非空要靠它）、CRC/加扰/META 比对全关（与页面内容无关）。
static const ftl_page_cfg_s s_page_cfg_blank  = {FC_CRC_DISABLE, FC_RANDOM_DISABLE, FC_ECC_MODE_NORMAL, FTL_LBA_UNKNOWN};

// 短页（< PAGE_MAIN_SIZE，如 BBT header 540 B）的补齐缓冲。单线程 superloop；
// 数据在 ftl_async_write_page_mirror 返回前已拷进 FC buffer，调用者无需持有。
static BYTE s_mirror_page[PAGE_MAIN_SIZE];

/* ── 1. 建链（desc 链组装）── */

/**
 * @brief 校验建链形参 cw_bitmap —— 「本命令要访问哪些 CW」的唯一定义处。
 *
 * @param bitmap      调用者给的 flat 字面位图。
 * @param plane_first 本命令 plane 段的起点（由 fc_command_plane_span 给出）。
 * @param plane_count 本命令 plane 段的 plane 数（由 fc_command_plane_span 给出）。
 *
 * @return FTL_OK 位图合法
 *         FTL_ERROR 空位图 / 越段 / multi-plane 未覆盖每个 plane
 *
 * @note - **没有 0 的隐式展开**：名字叫 bitmap 就必须逐位字面，0 = 一个 CW 都不访问，
 * 不是合法的数据命令（建链会产出空链 → 提交后静默不搬任何数据）。
 * 整段访问由调用者用 fc_command_plane_span_cw_bitmap() 显式算 —— 见 pba_to_cmd。
 * - 位图必须落在 plane 段内：下面的枚举只走段内的 plane × cw，段外高位不会被
 * 枚举到；不拒绝就是**静默忽略**。
 * - multi-plane 命令的位图必须**覆盖段内每一个 plane**——NAND 侧多 plane handler
 * 无条件遍历 PLANE_NUM 下行地址，未覆盖 plane 会被凭空造出回退地址。
 * CW 级映射下「只覆盖半个 pair」是常态，那种情况必须用单 plane op。
 * - ASSERT 与 release 拒绝**二者都要**（H14 判据 9）：调试期给现场，release 下
 * 绝不带着残缺链发到硬件。
 */
static ftl_status_e ftl_cmd_validate_cw_bitmap(DWORD bitmap, BYTE plane_first, BYTE plane_count)
{
    const DWORD allowed = (((1U << (plane_count * CW_NUM)) - 1U)) << (plane_first * CW_NUM);

    ASSERT(bitmap != 0U);
    ASSERT((bitmap & ~allowed) == 0U);

    if ((bitmap == 0U) || ((bitmap & ~allowed) != 0U))
    {
        return FTL_ERROR; /* release 运行期拒绝：空集 / 越出 plane 段 */
    }

    if (plane_count > 1U)
    {
        for (BYTE p = 0U; p < plane_count; p++)
        {
            const DWORD plane_bits = bitmap & ftl_cw_plane_mask((plane_first + p));

            ASSERT(plane_bits != 0U);
            if (plane_bits == 0U)
            {
                return FTL_ERROR;
            }
        }
    }

    return FTL_OK;
}

/**
 * @brief 按 cw_bitmap 建 desc 链（不搬数据、无数据操作不建链）
 *
 * cw_bitmap 是字面位图，0 = 空集，必须落在本命令 plane 段内。失败时整链回滚，
 * 调用者只需释放命令槽位；Policy B 走 ftl_cmd_setup_ppa_pair_cw。
 *
 * @param cmd       命令（paa/config 已写好）
 * @param cw_bitmap flat 位图（bit = plane*CW_NUM + cw）
 * @return FTL_OK；FTL_ERROR 位图越段 / 建链失败
 */
ftl_status_e ftl_cmd_configure_descriptors(fc_cmd_s *cmd, DWORD cw_bitmap)
{
    // cmd 由调用者从命令池取得且已判空；此处不重复断言。
    // 唯一合法失败是**池空**（desc/buffer）——那才是返回码。

    if (ftl_cmd_needs_desc_chain(cmd) == false)
    {
        cmd->desc_id = FC_DESC_ID_INVALID;
        return FTL_OK;
    }

    BYTE plane_first;
    BYTE plane_last;
    BYTE plane_count = fc_command_plane_span(cmd, &plane_first, &plane_last);

    if (ftl_cmd_validate_cw_bitmap(cw_bitmap, plane_first, plane_count) != FTL_OK)
    {
        return FTL_ERROR;
    }

    const DWORD mask = cw_bitmap;

    for (BYTE plane = plane_first; plane < plane_last; plane++)
    {
        // 行地址逐 plane 换 plane 位：PAA 的 plane 与 cw 不相邻，不能再用 PBA 的
        // plane_cw 掩码技巧（cmd->paa 已由调用者/上层入口转好）。
        const DWORD row_paa = paa_with_plane(cmd->paa, plane);

        for (BYTE cw = 0U; cw < CW_NUM; cw++)
        {
            if ((mask & ftl_cw_bit(plane, cw)) == 0U)
            {
                continue;
            }
            if (fc_command_configure_descriptor(cmd->cmd_id, plane, cw, row_paa) != FC_STATUS_OK)
            {
                cmd->desc_id = fc_descriptor_chain_release_all(cmd->desc_id);
                return FTL_ERROR;
            }
        }
    }

    return FTL_OK;
}

/* ── 2. 命令地址入口（PBA → PAA + 拓扑）── */

/**
 * @brief 按**显式** cw_bitmap 逐 plane 建 desc 链（Policy B：各 plane 行地址独立）。
 *
 * @param cmd 命令（config.multi_plane 与 paa 须已写好，段由它们决定）
 * @param pba 各 plane 的 PBA（**只校验本命令实际置位的 plane**）
 * @param cw_bitmap flat 字面位图（bit = plane*CW_NUM + cw），须落在 plane 段内。
 *
 * @return FTL_OK 建链成功
 *         FTL_AGAIN desc/buffer 池空（瞬态背压）
 *         FTL_ERROR 位图非法 / 建链失败
 *
 * @note **未置位的 plane 整体跳过**——连 pba_is_valid 都不判。CW 级映射下一条
 * 子操作常只覆盖半个 pair（如只写 plane1 的 4 个 CW），此时调用者按约定
 * 可让另一 plane 的 pba 保持未初始化（见 ftl_issue_subop 的 pba 局部数组）。
 * 位图规则与 ftl_cmd_configure_descriptors 共用 ftl_cmd_validate_cw_bitmap。
 */
static ftl_status_e ftl_cmd_build_pair_chain(fc_cmd_s *cmd, const DWORD pba[PLANE_NUM], DWORD cw_bitmap)
{
    BYTE plane_first;
    BYTE plane_last;
    BYTE plane_count = fc_command_plane_span(cmd, &plane_first, &plane_last);

    if (ftl_cmd_validate_cw_bitmap(cw_bitmap, plane_first, plane_count) != FTL_OK)
    {
        return FTL_ERROR;
    }

    const DWORD mask = cw_bitmap;

    // 与 ftl_cmd_configure_descriptors **同构**：单层 plane 循环 + 逐 CW 位测，
    // 位图一律用**绝对编号**（bit = plane*CW_NUM + cw）——不做「右移到 bit0 起」的
    // 局部位体操。唯一差别：行地址逐 plane 各自取（Policy B），而不是共用 cmd->paa。
    for (BYTE plane = plane_first; plane < plane_last; plane++)
    {
        // pba 的下标一律用 **plane 号**（不是段内偏移）——单 plane 命令的 plane_first 可能
        // 是 1（paa 的 plane 位决定），用偏移会拿到 plane0 的行地址。
        if ((mask & ftl_cw_plane_mask(plane)) == 0U)
        {
            continue; /* 本 plane 无 CW：不建链、不校验 PBA */
        }

        ASSERT(pba_is_valid(pba[plane]));             /* 逐 plane 的行地址都由它转出，越界位会静默算错地址 */
        const DWORD row_paa = pba_to_paa(pba[plane]); /* PBA→PAA 在 FTL 侧转出 */

        for (BYTE cw = 0U; cw < CW_NUM; cw++)
        {
            if ((mask & ftl_cw_bit(plane, cw)) == 0U)
            {
                continue;
            }
            const fc_status_e st = fc_command_configure_descriptor(cmd->cmd_id, plane, cw, row_paa);
            if (st != FC_STATUS_OK)
            {
                // 回滚整条链（desc + buffer 还池，desc_id 复位）；命令槽位由调用者释放
                cmd->desc_id = fc_descriptor_chain_release_all(cmd->desc_id);
                // 池空（desc/buffer）是**瞬态背压**——大请求拆多子操作时上层据此排队重试，
                // 而非整体失败。其余（不可达：配置错误已 ASSERT）按永久失败。
                return (st == FC_STATUS_RESOURCE_EMPTY) ? FTL_AGAIN : FTL_ERROR;
            }
        }
    }

    return FTL_OK;
}

/**
 * @brief CW 级多 plane 写建链入口
 *
 * 按被覆盖的第一个 plane 定段并绑拓扑；需要 desc 链时按位图挂链。
 *
 * @param cmd       命令（config.multi_plane 须已设好）
 * @param pba       各 plane 的 PBA（只校验被覆盖的 plane）
 * @param cw_bitmap flat 位图
 * @return FTL_OK；FTL_AGAIN desc/buffer 池空；FTL_ERROR 位图/建链失败
 */
ftl_status_e ftl_cmd_setup_ppa_pair_cw(fc_cmd_s *cmd, const DWORD pba[PLANE_NUM], DWORD cw_bitmap)
{
    // cmd 来自调用者的命令池（已判空）、pba 是调用者的局部数组
    // （见 ftl_issue_subop / wb_handle(FLUSH)）——两者给 NULL 都是调用者 bug ⇒ ASSERT。

    // 空位图必须在算 first_plane **之前**拒绝：否则 first_plane == PLANE_NUM，
    // release 下下面的 ASSERT 全部裁空 ⇒ 读 pba[PLANE_NUM] 越界。
    if (cw_bitmap == 0U)
    {
        return FTL_ERROR;
    }

    // 基址 PBA 必须取自**被覆盖的 plane**，不是无条件的 pba[0]：CW 级子操作常只覆盖
    // 半个 pair（例如一条 512B 写只落 plane1），此时 pba[0] 按约定未初始化，
    // 拿它定段会把单 plane 命令的段定成 plane0 ⇒ 位图越段 ASSERT 或静默空链。
    BYTE first_plane = PLANE_NUM; /* 本命令实际覆盖的第一个 plane（= 段首） */
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        if ((cw_bitmap & ftl_cw_plane_mask(p)) != 0U)
        {
            first_plane = p;
            break;
        }
    }
    ASSERT(first_plane < PLANE_NUM); /* 空位图 = 调用者 bug（ftl_cmd_validate_cw_bitmap 也会拦） */

    ASSERT(pba_is_valid(pba[first_plane]));

    // 拓扑与行地址基址取**本命令实际访问的第一个 plane**：fc_command_plane_span 用
    // cmd->paa 的 plane 位定单 plane 命令的段，给错了段就错。
    const DWORD base_pba = pba[first_plane];

    cmd->ch  = pba_to_ch(base_pba);
    cmd->ce  = pba_to_ce(base_pba);
    cmd->lun = pba_to_lun(base_pba);
    cmd->paa = pba_to_paa(base_pba); /* PBA→PAA 在 FTL 侧一次转出 */

    if (ftl_cmd_needs_desc_chain(cmd) == false)
    {
        cmd->desc_id = FC_DESC_ID_INVALID;
        return FTL_OK;
    }

    // CW 级映射入口：位图由调用者给出（可只覆盖本命令实际写到的那些 CW）。
    // cmd->config.multi_plane 决定 plane 段，须由调用者**在建链前**设好。
    return ftl_cmd_build_pair_chain(cmd, pba, cw_bitmap);
}

/**
 * @brief 汇总 desc 链上每个 gblock 的最大 ECC bit 数
 *
 * 两个 plane 是不同块，必须分别记账；容量不足时保留已收集项，不越界。
 *
 * @param cmd 命令
 * @param out 输出：gblock + ecc_max（最多 max 项）
 * @param max out 容量
 * @return 实际收集的块数
 */
BYTE ftl_cmd_block_ecc_list(const fc_cmd_s *cmd, ftl_cmd_block_ecc_s *out, BYTE max)
{
    BYTE         count = 0U;
    fc_desc_id_t did   = (fc_desc_id_t)cmd->desc_id;

    while (did < FC_DESCRIPTOR_POOL_DEPTH)
    {
        const fc_desc_s *d   = fc_descriptor_id_to_descriptor(did);
        const DWORD      pba = paa_to_pba(cmd->ch, cmd->ce, d->paa);
        const WORD       gb  = ftl_pba_gblock(pba);

        BYTE i = 0U;

        for (; i < count; i++)
        {
            if (out[i].gblock == gb)
            {
                break;
            }
        }
        if (i == count)
        {
            if (count >= max)
            {
                break; /* 容量不足：保留已收集的块，不越界 */
            }
            out[count].gblock  = gb;
            out[count].ecc_max = 0U;
            count++;
        }
        if (d->ecc_bits > out[i].ecc_max)
        {
            out[i].ecc_max = d->ecc_bits;
        }
        did = (fc_desc_id_t)d->next_id;
    }
    return count;
}

/* ── 3. 数据搬运（desc 链 ↔ 外部缓冲）── */

/**
 * @brief 把 data 按链序拷进 desc 的 FC 缓冲
 *
 * 数据耗尽后各 CW 用 0xFF 补齐；desc 无缓冲才返回 FTL_ERROR。
 *
 * @param cmd    已建链命令
 * @param data   源缓冲
 * @param length 源长度
 * @return FTL_OK；FTL_ERROR desc 无缓冲
 */
ftl_status_e ftl_cmd_write_data(fc_cmd_s *cmd, const void *data, DWORD length)
{
    const BYTE *src = (const BYTE *)data;

    fc_desc_id_t desc_id = (fc_desc_id_t)cmd->desc_id;

    while (desc_id < FC_DESCRIPTOR_POOL_DEPTH)
    {
        fc_desc_s *descriptor = fc_descriptor_id_to_descriptor(desc_id);

        if (descriptor->buffer_id == FC_BUF_ID_INVALID)
        {
            return FTL_ERROR;
        }
        BYTE *buf = fc_buffer_get_user_data(descriptor->buffer_id);

        if (length > 0U)
        {
            DWORD copy_len = (length >= FC_BUFFER_USER_SIZE) ? FC_BUFFER_USER_SIZE : length;

            memcpy(buf, src, copy_len);
            src += copy_len;
            length -= copy_len;

            // 本 CW 未填满部分用 0xFF dummy 补齐
            if (copy_len < FC_BUFFER_USER_SIZE)
            {
                memset(buf + copy_len, 0xFF, (FC_BUFFER_USER_SIZE - copy_len));
            }
        }
        else
        {
            // 数据已耗尽：本 CW 的 USER 区整体填 0xFF dummy。
            // ⚠ 长度必须是 USER 区大小——buf 是 base+FC_BUFFER_USER_OFFSET，
            // 直接用 FC_BUFFER_POOL_CW_SIZE 会越界 8B 踩到下一 CW 的 META/CRC。
            memset(buf, 0xFF, FC_BUFFER_USER_SIZE);
        }

        desc_id = (fc_desc_id_t)descriptor->next_id;
    }

    return FTL_OK;
}

/**
 * @brief 按 CW 偏移散射取数（GC/WL 搬移）
 *
 * 只搬 cw_bitmap 内的 CW，位置不能挪（META 的 LBA 由 CW 号决定）；位图外填
 * 0xFF。pair_major 选源布局：false = 单页镜像，true = plane*CW_NUM+cw。
 *
 * @param cmd        已建链命令
 * @param page_buf   源镜像
 * @param cw_bitmap  flat 位图
 * @param pair_major true = pair 镜像；false = 单页镜像
 * @return FTL_OK；FTL_ERROR desc 无缓冲
 */
ftl_status_e ftl_cmd_write_data_cw(fc_cmd_s *cmd, const BYTE *page_buf, DWORD cw_bitmap, BOOL pair_major)
{
    fc_desc_id_t desc_id = (fc_desc_id_t)cmd->desc_id;

    while (desc_id < FC_DESCRIPTOR_POOL_DEPTH)
    {
        fc_desc_s *descriptor = fc_descriptor_id_to_descriptor(desc_id);

        // CW 号取 desc 自带的 cw_index；plane 必须从 desc 的 PAA 解出——
        // cw_bitmap 是 **flat 绝对编号**（bit = plane×CW_NUM + cw），
        // 只用 `1U << cw` 是 plane 盲的：plane 1 的 bits 4..7 在 bits 0..3 上全为 0，
        // 会把 plane 1 的每个 CW 都填成 0xFF（GC/WL 迁移静默丢数据）。
        const BYTE cw    = descriptor->cw_index;
        const BYTE plane = paa_to_plane(descriptor->paa);

        if (descriptor->buffer_id == FC_BUF_ID_INVALID)
        {
            return FTL_ERROR;
        }
        BYTE *buf = fc_buffer_get_user_data(descriptor->buffer_id);

        if ((cw_bitmap & ftl_cw_bit(plane, cw)) != 0U)
        {
            // 源偏移必须与调用者的镜像布局一致：
            // · pair 镜像（wbuf）：槽位 = plane*CW_NUM + cw；
            // · 单页镜像（mirror/GC/WL）：两个 plane 共用 cw 偏移。
            const size_t slot = pair_major ? (plane * CW_NUM + cw) : cw;

            memcpy(buf, page_buf + (slot * FC_BUFFER_USER_SIZE), FC_BUFFER_USER_SIZE);
        }
        else
        {
            memset(buf, 0xFF, FC_BUFFER_USER_SIZE);
        }

        desc_id = (fc_desc_id_t)descriptor->next_id;
    }

    return FTL_OK;
}

/**
 * @brief 从 desc 链按链序拷出 length 字节
 *
 * 缓冲无效或链比请求短都返回 FTL_ERROR（不交出未初始化尾部）。
 *
 * @param cmd    已完成命令
 * @param buffer 输出缓冲
 * @param length 请求长度
 * @return FTL_OK；FTL_ERROR 缓冲无效 / 链过短
 */
ftl_status_e ftl_cmd_read_data(fc_cmd_s *cmd, void *buffer, DWORD length)
{
    BYTE *dst = (BYTE *)buffer;

    DWORD remaining = length;

    fc_desc_id_t desc_id = (fc_desc_id_t)cmd->desc_id;

    while ((desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (remaining > 0U))
    {
        fc_desc_s *descriptor = fc_descriptor_id_to_descriptor(desc_id);

        if (descriptor->buffer_id == FC_BUF_ID_INVALID)
        {
            return FTL_ERROR;
        }
        BYTE *buf = fc_buffer_get_user_data(descriptor->buffer_id);
        if (buf == NULL)
        {
            return FTL_ERROR;
        }

        DWORD copy_len = (remaining >= FC_BUFFER_USER_SIZE) ? FC_BUFFER_USER_SIZE : remaining;
        memcpy(dst, buf, copy_len);
        dst += copy_len;
        remaining -= copy_len;

        desc_id = (fc_desc_id_t)descriptor->next_id;
    }

    // 链比请求短 ⇒ 缓冲尾部未初始化；调用者会把它当有效数据用，必须报错。
    return (remaining == 0U) ? FTL_OK : FTL_ERROR;
}

/* ── 4. 同步提交等待 ── */

/**
 * @brief 同步提交并等待命令完成（仅结构性同步上下文）
 *
 * 登记栈上 waiter → 提交 SYNC_WAIT → 自旋 fc_service + ftl_op_drain 直到完成；
 * 超时摘链。命令归属见 `ftl_sync_submit_wait` 契约（`submitted` 出参）。
 *
 * @param cmd        命令
 * @param status_out 输出：完成状态（可 NULL；仅 FTL_OK 时有效）
 * @param timeout_us 等待上限（真实微秒，0 = 默认 10 ms，上限 65 ms）
 * @param submitted  输出（非 NULL）：返回时命令是否已提交给 FC
 * @return FTL_OK；FTL_ERROR 提交失败 / 等待超时
 *
 * @note 时钟不可用（`delay_time_now_us` 失败）时退化为同值的轮数兜底，不静默取消超时。
 */
ftl_status_e ftl_sync_submit_wait(fc_cmd_s *cmd, fc_cmd_status_s *status_out, DWORD timeout_us, BOOL *submitted)
{
    ASSERT(submitted != NULL);

    DWORD max_wait = (timeout_us > 0U) ? timeout_us : 10000U;

    // 上限 clamp：debug 停在现场，release 也必须有确定行为（不得静默截断成更短的等待）。
    ASSERT(max_wait <= (DWORD)UINT16_MAX);
    if (max_wait > (DWORD)UINT16_MAX)
    {
        max_wait = (DWORD)UINT16_MAX;
    }

    *submitted = false;

    DWORD      start_us = 0U;
    const BOOL clock_ok = delay_time_now_us(&start_us);
    DWORD      elapsed  = 0U; /* 仅时钟不可用时作轮数兜底 */

    // ⚠ **服务函数内不得同步等待**（贯穿机制 2）：在完成派发里再开一个完成泵
    // （本函数自旋 fc_service + ftl_op_drain）= 嵌套泵，正是本次异步化要
    // 消灭的形态——调用者的栈帧被钉住，且泵里再跑泵使重入面不可枚举。
    // 目标终态：本函数只被开机初始化 / 全片格式化 / 出厂坏块扫描 / CLI 调用
    // （那些路径在 superloop 之前，没有别人 drain CQ，必须同步）。
    // 新增调用点若触发本断言，说明该功能尚未 job 化——改用 ftl_async_* + ftl_op_submit。
    ASSERT(ftl_op_is_in_dispatch() == false);

    ftl_sync_waiter_s waiter;
    waiter.cmd_id = ftl_cmd_id(cmd);
    waiter.done   = false;

    if (ftl_op_submit(cmd, FTL_OP_SYNC_WAIT, &waiter) != FTL_OK)
    {
        return FTL_ERROR; /* 提交失败（背压/request 登记失败）——*submitted 保持 false，调用者负责 release */
    }

    *submitted = true; /* 命令已进 FC：此后任何失败路径都不得让调用者 release */

    for (;;)
    {
        // 长同步操作（CLI format / bbt --erase / 出厂扫描）按自旋节拍上报进展并喂狗：
        // 这些调用期间主循环被占住，timer 任务不转。
        storage_wdg_kick();

        (void)fc_service();
        ftl_op_drain();

        if (waiter.done)
        {
            break;
        }

        // 真实微秒 deadline（max_wait 已在入口 clamp 到 WORD 范围）。
        // 时钟不可用（未初始化）时退化为轮数兜底，不静默取消超时。
        BOOL expired;

        if (clock_ok)
        {
            expired = delay_time_elapsed_us(start_us, (WORD)max_wait);
        }
        else
        {
            elapsed++;
            expired = (elapsed > max_wait);
        }

        if (expired)
        {
            // request 表项摘除（栈上 waiter 不能留给迟到完成）；命令**保留**在 FC，
            // 由 drain 的 missing-request 分支在迟到完成时释放。
            ftl_op_req_remove(waiter.cmd_id);
            return FTL_ERROR;
        }
    }

    if (status_out != NULL)
    {
        *status_out = waiter.completion;
    }

    return FTL_OK;
}

/**
 * @brief 同步提交等待 + 失败时的命令归属收尾（本文件内唯一关口）
 *
 * 把「提交失败 ⇒ 调用者 release / 已提交但超时 ⇒ 留给 drain release」这条规则
 * 收在一处，避免每个 `ftl_sync_*` 各自决定。
 *
 * @param cmd        已建链命令
 * @param status_out 完成状态（可 NULL；仅 FTL_OK 时有效）
 * @param timeout_us 等待上限（微秒，0 = 默认）
 * @return FTL_OK    完成；命令仍由调用者持有，读完数据后 release
 *         FTL_ERROR 失败；**未提交**时已在内部 release，**已提交但超时**时命令留给 drain
 */
static ftl_status_e ftl_sync_wait_owned(fc_cmd_s *cmd, fc_cmd_status_s *status_out, DWORD timeout_us)
{
    BOOL submitted = false;

    const ftl_status_e st = ftl_sync_submit_wait(cmd, status_out, timeout_us, &submitted);

    if ((st != FTL_OK) && (submitted == false))
    {
        (void)fc_command_release(cmd->cmd_id);
    }
    // submitted == true 且失败：命令仍在 FC/NAND，迟到完成由 drain 释放——不要碰它。

    return st;
}

/* ── 5. 页级同步原语 ──
 *
 * 各原语共用同一模板：alloc → 填配置 → pba_to_cmd（建链）→ 可选搬数据 →
 * submit → sync_wait → 判完成状态 → release。差异只在：
 *   · operation（PROGRAM / READ / ERASE）
 *   · 配置（crc / random / ldpc / lba）
 *   · 数据方向（写：submit 前搬入；读：完成后搬出）
 *   · 是否判失败位（写/读页/擦除判 FC_CMD_STATUS_ERROR_MASK 全集；RAW 读刻意不判）
 *   · 超时（擦除 20ms，其余默认）
 */

/* 命令分配类：构造期上下文（单线程 superloop），默认 BG。
 * L2P demand load、L2P region 写回、Journal/CP/BBT/metadata 在构造点提升到 CRIT，
 * 构造后恢复；host wbuf、GC、prefetch 保持 BG。 */
static fc_cmd_class_e s_cmd_class = FC_CMD_CLASS_BG;

/**
 * @brief 设置命令分配类（构造期上下文；返回原值以便恢复）
 *
 * L2P demand load、L2P region 写回、Journal/CP/BBT/metadata 在构造点提升到
 * FC_CMD_CLASS_CRIT，构造后恢复到原值；host wbuf、GC、prefetch 保持 BG。
 * 额度是文件级构造上下文：调用点必须用返回值恢复；单线程 superloop，无并发保护。
 *
 * @param cls 目标类（FC_CMD_CLASS_*）
 * @return 原类
 */
fc_cmd_class_e ftl_cmd_class_set(fc_cmd_class_e cls)
{
    ASSERT(cls < FC_CMD_CLASS_NUM);

    const fc_cmd_class_e prev = s_cmd_class;
    s_cmd_class               = cls;
    return prev;
}

/**
 * @brief 分配命令 + 填配置（**不含**地址与建链）。池空返回 NULL。
 * 建链只认 cw_bitmap（字面位图、无隐式展开）；pba_to_cmd 算出「本命令 plane 段内
 * 全部 CW」并作实参传给建链入口，位图不存进命令结构体。
 *
 * 与地址/建链拆开是为了让异步入口能区分「池空（瞬态背压 ⇒ FTL_AGAIN）」与
 * 「地址非法 / 建链失败（编程错误 ⇒ FTL_ERROR）」——两者在同步原语里都被压成 NULL。
 *
 * @param operation 命令 operation 码
 * @param cfg       页配置（crc / random / ldpc / 页 LBA）
 * @return 已填好配置的命令；命令池空返回 NULL
 */
static void ftl_cmd_prepare_config(fc_cmd_s *cmd, BYTE operation, const ftl_page_cfg_s *cfg)
{
    ASSERT(cfg != NULL);

    cmd->config.operation   = operation;
    cmd->config.crc         = cfg->crc;
    cmd->config.random      = cfg->random;
    cmd->config.ldpc        = cfg->ldpc;
    cmd->config.multi_plane = 0U;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->config.page_type   = FC_PAGE_TYPE_LP;
    // 页 LBA → 扇区 LBA。FTL_LBA_UNKNOWN（FTL_PAGE_CFG_DATA_ANY）⇒ FC_META_UNKNOWN，
    // 让 FC 侧跳过「页内 META 与本命令 LBA 比对」那一项。
    cmd->meta               = (cfg->lba == FTL_LBA_UNKNOWN) ? FC_META_UNKNOWN : (cfg->lba << CW_WIDTH);
}

static fc_cmd_s *ftl_cmd_prepare_alloc(BYTE operation, const ftl_page_cfg_s *cfg)
{
    fc_cmd_s *cmd = ftl_cmd_alloc_class(s_cmd_class);

    if (cmd == NULL)
    {
        return NULL;
    }

    ftl_cmd_prepare_config(cmd, operation, cfg);
    return cmd;
}

/**
 * @brief 分配命令 + 填配置 + 建链。失败返回 NULL（命令已回收）。
 *
 * @param pba       目标页 / 块 PBA
 * @param operation 命令 operation 码
 * @param cfg       页配置（crc / random / ldpc / 页 LBA）
 * @return 已建链的命令；池空或建链失败返回 NULL
 */
static fc_cmd_s *ftl_sync_page_prepare(DWORD pba, BYTE operation, const ftl_page_cfg_s *cfg)
{
    fc_cmd_s *cmd = ftl_cmd_prepare_alloc(operation, cfg);

    if (cmd == NULL)
    {
        return NULL;
    }

    if (pba_to_cmd(cmd, pba) != FTL_OK)
    {
        (void)fc_command_release(cmd->cmd_id);
        return NULL;
    }

    return cmd;
}

/**
 * @brief 同步页写：建链 → 填数据 → 提交等待
 *
 * 任一步失败都先释放命令。
 *
 * @param pba    目标页 PBA
 * @param data   源数据（可 NULL）
 * @param length 源长度
 * @param cfg    页配置
 * @return FTL_OK；FTL_ERROR 建链/提交/完成失败
 */
ftl_status_e ftl_sync_write_page(DWORD pba, const void *data, DWORD length, const ftl_page_cfg_s *cfg)
{
    fc_cmd_s *cmd = ftl_sync_page_prepare(pba, OP_NORMAL_PROGRAM_PAGE, cfg);
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    if ((data != NULL) && (length > 0U) && (ftl_cmd_write_data(cmd, data, length) != FTL_OK))
    {
        (void)fc_command_release(cmd->cmd_id);
        return FTL_ERROR;
    }

    fc_cmd_status_s completion;
    if (ftl_sync_wait_owned(cmd, &completion, 0U) != FTL_OK)
    {
        return FTL_ERROR;
    }
    if (fc_cmd_status_has_error(completion.status))
    {
        (void)fc_command_release(cmd->cmd_id);
        return FTL_ERROR;
    }

    (void)fc_command_release(cmd->cmd_id);
    return FTL_OK;
}

/**
 * @brief 同步页读：建链 → 提交等待 → 拷出数据
 *
 * 失败判据用 FC_CMD_STATUS_ERROR_MASK 全集；DATA/META 错误同样不可信。
 *
 * @param pba    目标页 PBA
 * @param buffer 输出缓冲（可 NULL）
 * @param length 请求长度
 * @param cfg    页配置
 * @return FTL_OK；FTL_ERROR 建链/提交/完成/拷出失败
 */
static ftl_status_e ftl_sync_read_page_impl(DWORD pba, void *buffer, DWORD length, const ftl_page_cfg_s *cfg, fc_cmd_status_s *completion_out)
{
    if (completion_out != NULL)
    {
        memset(completion_out, 0, sizeof(*completion_out));
    }

    fc_cmd_s *cmd = ftl_sync_page_prepare(pba, OP_NORMAL_READ_PAGE_WITH_DATA_OUT, cfg);
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    fc_cmd_status_s completion;
    if (ftl_sync_wait_owned(cmd, &completion, 0U) != FTL_OK)
    {
        return FTL_ERROR;
    }

    if (completion_out != NULL)
    {
        *completion_out = completion;
    }
    // 失败判据 = FC_CMD_STATUS_ERROR_MASK 全集（与 fc_interface.h 的链级判据同口径）。
    // **只认 NAND_FAIL 是错的**：DATA_ERROR / META_ERROR 会让本页内容不可信却照常拷出——
    // · DATA_ERROR（UNC/CRC/RAID）：fc_data_path_read 在任何 CW 不可纠时**提前返回**，
    //   buffer 停在 "ECC 半修正 + 未解扰" 态（步骤 2/3/4 全跳过），拷出来的是加扰垃圾；
    // · META_ERROR：页内 META 与预期不符 = **读到了别的页**（或页已擦除）。
    // 后果实例：L2P region 页读回垃圾 → l2p_cache_find_or_load 标 CLEAN → 垃圾 PBA
    // 被 flush 写回介质**永久化**，读路径再拿它当 ch/ce/lun 路由字段用。
    // 空页也会命中 META_ERROR ⇒ 返回 FTL_ERROR ⇒ 调用者保持 entries
    // 全 NAND_PBA_INVALID，与"未映射"语义一致（这正是各调用点的既有兜底分支）。
    // ⚠ 未映射的 L2P region 不在此列：Directory 条目就是 INVALID，加载路径根本不发读。
    if (fc_cmd_status_has_error(completion.status))
    {
        (void)fc_command_release(cmd->cmd_id);
        return FTL_ERROR;
    }

    if ((buffer != NULL) && (length > 0U) && (ftl_cmd_read_data(cmd, buffer, length) != FTL_OK))
    {
        (void)fc_command_release(cmd->cmd_id);
        return FTL_ERROR;
    }

    (void)fc_command_release(cmd->cmd_id);
    return FTL_OK;
}

ftl_status_e ftl_sync_read_page(DWORD pba, void *buffer, DWORD length, const ftl_page_cfg_s *cfg)
{
    return ftl_sync_read_page_impl(pba, buffer, length, cfg, NULL);
}

ftl_status_e ftl_sync_read_page_status(DWORD pba, void *buffer, DWORD length, const ftl_page_cfg_s *cfg, fc_cmd_status_s *completion_out)
{
    return ftl_sync_read_page_impl(pba, buffer, length, cfg, completion_out);
}

/**
 * @brief 同步 RAW 读（跳过 ECC）
 *
 * 刻意不判 NAND 失败：坏页也交出原始数据；不是判空手段。
 *
 * @param pba    目标页 PBA
 * @param buffer 输出：每 CW 的 USER 512B
 * @return FTL_OK；FTL_ERROR 建链/提交失败 / desc 无数据
 */
ftl_status_e ftl_sync_read_page_raw(DWORD pba, void *buffer)
{
    const ftl_page_cfg_s *cfg = &s_page_cfg_raw;

    fc_cmd_s *cmd = ftl_sync_page_prepare(pba, OP_NORMAL_READ_PAGE_WITH_DATA_OUT, cfg);
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    fc_cmd_status_s completion;
    if (ftl_sync_wait_owned(cmd, &completion, 0U) != FTL_OK)
    {
        return FTL_ERROR;
    }

    // 不判 NAND 失败：整页 4 个 CW 无条件拷出，每 CW 只拷 USER 512B（FC 的分层契约），
    // 槽尾 16B（META/CRC/parity）不填。
    // 但取不到 CW 数据是**控制器侧**错误（desc/buffer 缺失），不是坏页——此时返回 OK
    // 会让调用者看到空洞，故必须报错。
    for (BYTE cw = 0U; cw < CW_NUM; cw++)
    {
        const BYTE *src = fc_get_cmd_data_ptr(cmd->cmd_id, cw);

        if (src == NULL)
        {
            (void)fc_command_release(cmd->cmd_id);
            return FTL_ERROR;
        }
        BYTE *dst = (BYTE *)buffer + (cw * FC_BUFFER_POOL_CW_SIZE);

        memcpy(dst, src, FC_BUFFER_USER_SIZE);
    }

    (void)fc_command_release(cmd->cmd_id);
    return FTL_OK;
}

/**
 * @brief 判空（解码路径）：读一次后取每 CW 的 EMPTY 位
 *
 * 读失败/超时返回 FTL_ERROR；不看 completion.status（擦除页在 FC 里不是错误位）。
 *
 * @param pba   目标页 PBA
 * @param blank 输出：全部 CW 都是擦除页
 * @return FTL_OK；FTL_ERROR 建链/提交失败
 */
ftl_status_e ftl_page_blank_check(DWORD pba, BOOL *blank)
{
    ASSERT(blank != NULL);

    fc_cmd_s *cmd = ftl_sync_page_prepare(pba, OP_NORMAL_READ_PAGE_WITH_DATA_OUT, &s_page_cfg_blank);

    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    fc_cmd_status_s completion;

    if (ftl_sync_wait_owned(cmd, &completion, 0U) != FTL_OK)
    {
        return FTL_ERROR;
    }

    // 不看 completion.status：擦除页在 FC 里不是错误位（FC_CW_STATUS_EMPTY）。
    *blank = fc_get_cmd_all_cw_empty(cmd->cmd_id);

    (void)fc_command_release(cmd->cmd_id);
    return FTL_OK;
}

/**
 * @brief 判空（保守方向）：读不通一律判非空
 *
 * @param pba 目标页 PBA
 * @return true = 确实全 EMPTY；false = 读失败 / 非空
 */
BOOL ftl_page_is_blank(DWORD pba)
{
    BOOL blank = false;

    // 读命令没走通 ⇒ 判"非空"（保守方向）：命令没走通时不能断言"没写过"。
    return (ftl_page_blank_check(pba, &blank) == FTL_OK) && blank;
}

/**
 * @brief 同步单 plane 擦除（超时 20ms）
 *
 * @param pba 目标块 PBA
 * @return FTL_OK；FTL_ERROR 建链/提交/完成失败
 */
ftl_status_e ftl_sync_erase_block(DWORD pba)
{
    const ftl_page_cfg_s *cfg = &s_page_cfg_normal;

    fc_cmd_s *cmd = ftl_sync_page_prepare(pba, OP_ERASE_BLOCK, cfg);
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    fc_cmd_status_s completion;
    if (ftl_sync_wait_owned(cmd, &completion, 20000U) != FTL_OK)
    {
        return FTL_ERROR;
    }
    if (fc_cmd_status_has_error(completion.status))
    {
        (void)fc_command_release(cmd->cmd_id);
        return FTL_ERROR;
    }

    (void)fc_command_release(cmd->cmd_id);
    return FTL_OK;
}

/**
 * @brief 同步 multi-plane 擦除（超时 20ms）
 *
 * 供页地址跨 plane 的固定池（Journal）用。
 *
 * @param pba 目标块 PBA
 * @return FTL_OK；FTL_ERROR 建链/提交/完成失败
 */
ftl_status_e ftl_sync_erase_block_multi_plane(DWORD pba)
{
    const ftl_page_cfg_s *cfg = &s_page_cfg_normal;

    fc_cmd_s *cmd = ftl_sync_page_prepare(pba, OP_ERASE_BLOCK_MULTI_PLANE, cfg);
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    fc_cmd_status_s completion;
    if (ftl_sync_wait_owned(cmd, &completion, 20000U) != FTL_OK)
    {
        return FTL_ERROR;
    }
    if (fc_cmd_status_has_error(completion.status))
    {
        (void)fc_command_release(cmd->cmd_id);
        return FTL_ERROR;
    }

    (void)fc_command_release(cmd->cmd_id);
    return FTL_OK;
}

/**
 * @brief 同步列读：读 Page 0 指定列的 4 字节
 *
 * 列地址走命令级 param 0/1（小端）；读失败保守判坏（BBT 依赖）。
 *
 * @param pba    目标页 PBA
 * @param column 列地址
 * @param out    输出：4 字节
 * @return FTL_OK；FTL_ERROR 建链/提交/完成失败（out 仍可能已填）
 */
ftl_status_e ftl_sync_read_column(DWORD pba, WORD column, BYTE out[4])
{
    const ftl_page_cfg_s *cfg = &s_page_cfg_normal;

    // 列 + 4 须落在页内：唯一调用者 bbt.c 传 PAGE_MAIN_SIZE（+4 ≤ PAGE_SIZE）⇒ 不重复校验。

    // OP_READ_COLUMN 的 has_data = 0 ⇒ pba_to_cmd 不建链、
    // 不看位图（desc_id 置 INVALID）。地址仍须落进 cmd->paa —— 但**只提供行地址**，
    // cw 位被 NAND 侧 paa_to_nand_row 丢掉；列地址由下面的 param 字节 0/1 给。
    fc_cmd_s *cmd = ftl_sync_page_prepare(pba, OP_READ_COLUMN, cfg);
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    // 列地址占 param 低 16 bit（小端）；高 16 bit 为 0。
    cmd->param.value = column;

    fc_cmd_status_s completion;
    if (ftl_sync_wait_owned(cmd, &completion, 0U) != FTL_OK)
    {
        return FTL_ERROR;
    }

    // 4 字节走控制器侧暂存（同 OP_READ_META），完成时已拷进 CQ 的 param。
    // 先拷出再判错：即使页读失败，调用者仍能看到原始 4 字节用于诊断；
    // 返回 FTL_ERROR 让 BBT 按“读失败 ⇒ 坏块（保守）”处理。
    memcpy(out, &completion.param.value, 4U);

    if (fc_cmd_status_has_error(completion.status))
    {
        (void)fc_command_release(cmd->cmd_id);
        return FTL_ERROR;
    }

    (void)fc_command_release(cmd->cmd_id);
    return FTL_OK;
}

/* ── 6. 元数据镜像写：multi-plane 同页同数据（异步实现 + 同步封装）── */

/**
 * @brief 异步镜像写：同一页数据写到所有 plane（multi-plane）
 *
 * 数据填装用 ftl_cmd_write_data_cw；length != PAGE_MAIN_SIZE 时补 0xFF。
 *
 * @param out_cmd 输出：建好的命令
 * @param pba     目标页 PBA（plane 由本函数展开）
 * @param data    源数据
 * @param length  源长度
 * @param cfg     页配置
 * @return FTL_OK；FTL_AGAIN 池空；FTL_ERROR 建链/搬数据失败
 */
ftl_status_e ftl_async_write_page_mirror(fc_cmd_s **out_cmd, DWORD pba, const void *data, DWORD length, const ftl_page_cfg_s *cfg)
{
    ASSERT(pba_is_valid(pba));

    const fc_cmd_class_e saved = ftl_cmd_class_set(FC_CMD_CLASS_CRIT);
    fc_cmd_s            *cmd   = ftl_cmd_prepare_alloc(OP_PROGRAM_PAGE_MULTI_PLANE, cfg);
    if (cmd == NULL)
    {
        (void)ftl_cmd_class_set(saved);
        return FTL_AGAIN; /* 命令池空 = 瞬态背压 */
    }

    cmd->config.multi_plane = PLANE_NUM;

    DWORD pair[PLANE_NUM];
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        pair[p] = pba_with_plane(pba, p);
    }

    // 本命令的 plane 段 = 全部 plane；位图必须覆盖段内每个 plane 的每个 CW。
    const DWORD bitmap = FTL_CACHE_FULL_CW;

    const ftl_status_e st = ftl_cmd_setup_ppa_pair_cw(cmd, pair, bitmap);

    if (st != FTL_OK)
    {
        (void)fc_command_release(cmd->cmd_id);
        (void)ftl_cmd_class_set(saved);
        return st; /* FTL_AGAIN 池空 / FTL_ERROR 建链失败 */
    }

    const BYTE *page = (const BYTE *)data;

    if (length != PAGE_MAIN_SIZE)
    {
        memset(s_mirror_page, 0xFF, sizeof(s_mirror_page));
        memcpy(s_mirror_page, data, length);
        page = s_mirror_page;
    }

    if (ftl_cmd_write_data_cw(cmd, page, bitmap, false) != FTL_OK)
    {
        (void)fc_command_release(cmd->cmd_id);
        (void)ftl_cmd_class_set(saved);
        return FTL_ERROR;
    }

    (void)ftl_cmd_class_set(saved);
    *out_cmd = cmd;
    return FTL_OK;
}

/**
 * @brief 同步镜像写（目录检查点主备同页）
 *
 * 建链/提交/完成任一失败都压成 FTL_ERROR。
 *
 * @param pba    目标页 PBA
 * @param data   源数据
 * @param length 源长度
 * @param cfg    页配置
 * @return FTL_OK；FTL_ERROR 任一步失败
 */
ftl_status_e ftl_sync_write_page_mirror(DWORD pba, const void *data, DWORD length, const ftl_page_cfg_s *cfg)
{
    fc_cmd_s *cmd;

    if (ftl_async_write_page_mirror(&cmd, pba, data, length, cfg) != FTL_OK)
    {
        return FTL_ERROR; /* 同步版把背压与硬失败都压成 FTL_ERROR */
    }

    fc_cmd_status_s completion;
    if (ftl_sync_wait_owned(cmd, &completion, 0U) != FTL_OK)
    {
        return FTL_ERROR;
    }
    if (fc_cmd_status_has_error(completion.status))
    {
        (void)fc_command_release(cmd->cmd_id);
        return FTL_ERROR;
    }

    (void)fc_command_release(cmd->cmd_id);
    return FTL_OK;
}

/* ── 7. 异步页级原语（提交与完成解耦）──
 *
 * 与上面 ftl_sync_* 共用同一套 prepare 模板（ftl_cmd_prepare_alloc / 地址绑定 / 建链 /
 * 搬数据），差别只在最后一步：**不提交**，把命令交回调用者（由它 ftl_op_submit +
 * 服务函数 release）。
 * 返回码比同步版多一档 FTL_AGAIN：命令池空是瞬态背压，调用者应当**保留现场、稍后重试**，
 * 而同步版把它和"地址非法"一起压成 FTL_ERROR（同步调用者没有重试的地方）。 */

/**
 * @brief 在命令上绑定 PBA + 拷入写数据（构造期 CRIT；失败回收命令）
 *
 * @param cmd    已分配且已 prepare_config 的命令
 * @param pba    目标页 PBA
 * @param data   源数据（可 NULL）
 * @param length 源长度
 * @return FTL_OK；FTL_ERROR 地址/建链失败（命令已回收）
 */
static ftl_status_e ftl_cmd_bind_page_write(fc_cmd_s *cmd, DWORD pba, const void *data, DWORD length)
{
    const fc_cmd_class_e saved = ftl_cmd_class_set(FC_CMD_CLASS_CRIT);

    if (pba_to_cmd(cmd, pba) != FTL_OK)
    {
        (void)fc_command_release(cmd->cmd_id);
        (void)ftl_cmd_class_set(saved);
        return FTL_ERROR;
    }

    if ((data != NULL) && (length > 0U) && (ftl_cmd_write_data(cmd, data, length) != FTL_OK))
    {
        (void)fc_command_release(cmd->cmd_id);
        (void)ftl_cmd_class_set(saved);
        return FTL_ERROR;
    }

    (void)ftl_cmd_class_set(saved);
    return FTL_OK;
}

/**
 * @brief 异步普通页写（提交与完成解耦）
 *
 * 数据在返回前已拷进 FC buffer 池 ⇒ 调用者源缓冲可立即复用。
 *
 * @param out_cmd 输出：建好的命令
 * @param pba     目标页 PBA
 * @param data    源数据（可 NULL）
 * @param length  源长度
 * @param cfg     页配置
 * @return FTL_OK；FTL_AGAIN 池空；FTL_ERROR 地址/建链失败
 */
ftl_status_e ftl_async_write_page(fc_cmd_s **out_cmd, DWORD pba, const void *data, DWORD length, const ftl_page_cfg_s *cfg)
{
    const fc_cmd_class_e saved = ftl_cmd_class_set(FC_CMD_CLASS_CRIT);
    fc_cmd_s            *cmd   = ftl_cmd_prepare_alloc(OP_NORMAL_PROGRAM_PAGE, cfg);

    (void)ftl_cmd_class_set(saved);

    if (cmd == NULL)
    {
        return FTL_AGAIN; /* 命令池空 = 瞬态背压 */
    }

    const ftl_status_e st = ftl_cmd_bind_page_write(cmd, pba, data, length);

    if (st != FTL_OK)
    {
        return st;
    }

    *out_cmd = cmd;
    return FTL_OK;
}

/**
 * @brief 用已预留的命令槽建链并装数据（不提交）
 *
 * desc/buffer 仍跟随构造期 class（写回固定 CRIT）；命令槽由调用者预留。
 *
 * @param cmd    已预留的命令
 * @param pba    目标页 PBA
 * @param data   源数据（可 NULL）
 * @param length 源长度
 * @param cfg    页配置
 * @return FTL_OK；FTL_ERROR 地址/建链失败（命令已回收）
 */
ftl_status_e ftl_async_write_page_reserved(fc_cmd_s *cmd, DWORD pba, const void *data, DWORD length, const ftl_page_cfg_s *cfg)
{
    ASSERT(cmd != NULL);

    ftl_cmd_prepare_config(cmd, OP_NORMAL_PROGRAM_PAGE, cfg);
    return ftl_cmd_bind_page_write(cmd, pba, data, length);
}

/**
 * @brief 异步 CW 级写：手工绑 PBA + 散射填数
 *
 * 不走 pba_to_cmd，避免「整段 vs 子集」语义丢失。
 *
 * @param out_cmd   输出：建好的命令
 * @param pba       目标页 PBA
 * @param page_buf  源页镜像
 * @param cw_bitmap flat 位图
 * @param cfg       页配置
 * @return FTL_OK；FTL_AGAIN 池空；FTL_ERROR 地址/建链失败
 */
ftl_status_e ftl_async_write_page_cw(fc_cmd_s **out_cmd, DWORD pba, const BYTE *page_buf, DWORD cw_bitmap, const ftl_page_cfg_s *cfg)
{
    fc_cmd_s *cmd = ftl_cmd_prepare_alloc(OP_NORMAL_PROGRAM_PAGE, cfg);
    if (cmd == NULL)
    {
        return FTL_AGAIN; /* 命令池空 = 瞬态背压 */
    }

    // 边界不变量同 pba_to_cmd：进 FC 的 ch/ce/lun 被当 suspended[][][] 下标直接用。
    ASSERT(pba_is_valid(pba));
    cmd->ch  = pba_to_ch(pba);
    cmd->ce  = pba_to_ce(pba);
    cmd->lun = pba_to_lun(pba);
    cmd->paa = pba_to_paa(pba);

    if (ftl_cmd_configure_descriptors(cmd, cw_bitmap) != FTL_OK)
    {
        (void)fc_command_release(cmd->cmd_id);
        return FTL_ERROR;
    }

    if (ftl_cmd_write_data_cw(cmd, page_buf, cw_bitmap, false) != FTL_OK)
    {
        (void)fc_command_release(cmd->cmd_id);
        return FTL_ERROR;
    }

    *out_cmd = cmd;
    return FTL_OK;
}

/**
 * @brief 异步 pair 写（wbuf 普通页 / cache 批次共用）
 *
 * 支持稀疏位图；cache op 必须覆盖全部 plane，各 plane 必须同 ch/ce/lun。
 *
 * @param out_cmd   输出：建好的命令
 * @param pba       各 plane 的 PBA
 * @param pair_buf  pair 镜像
 * @param cw_bitmap flat 位图
 * @param operation 命令 operation
 * @param cfg       页配置
 * @return FTL_OK；FTL_AGAIN 池空；FTL_ERROR 地址/建链失败
 */
ftl_status_e ftl_async_write_pair_cmd(fc_cmd_s **out_cmd, const DWORD pba[PLANE_NUM], const BYTE *pair_buf, DWORD cw_bitmap, BYTE operation, const ftl_page_cfg_s *cfg)
{
    fc_cmd_s *cmd = ftl_cmd_prepare_alloc(operation, cfg);

    if (cmd == NULL)
    {
        return FTL_AGAIN;
    }

    const BYTE nplanes  = ftl_cw_bitmap_plane_count(cw_bitmap);
    const BOOL cache_op = (operation == OP_TWO_PLANE_PROGRAM_CACHE) || (operation == OP_TWO_PLANE_PROGRAM_CACHE_FIRST) || (operation == OP_TWO_PLANE_PROGRAM_CACHE_END);

    // cache program 的 opcode 按两 plane 定尺；单 plane 会让 NAND 侧多 plane handler
    // 无条件遍历 PLANE_NUM 行地址、凭空造出回退地址（见 ftl_cmd_validate_cw_bitmap 的
    // 注释）。release 下也拒绝，不只 ASSERT。
    ASSERT((cache_op == false) || (nplanes == PLANE_NUM));
    if ((cache_op == true) && (nplanes != PLANE_NUM))
    {
        (void)fc_command_release(cmd->cmd_id);
        return FTL_ERROR;
    }

    cmd->config.multi_plane = ftl_plane_multi_field(nplanes);

    DWORD first_pba = NAND_PBA_INVALID;

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        if (bit_mask_any(cw_bitmap, ftl_cw_plane_mask(p)) == false)
        {
            continue;
        }

        ASSERT(pba_is_valid(pba[p]));

        if (first_pba == NAND_PBA_INVALID)
        {
            first_pba = pba[p];
        }
        else
        {
            const BOOL same_die = (pba_to_ch(pba[p]) == pba_to_ch(first_pba)) && (pba_to_ce(pba[p]) == pba_to_ce(first_pba)) && (pba_to_lun(pba[p]) == pba_to_lun(first_pba));

            // 一条命令只能落在一个 die；跨实例 PBA 会被路由到其中一个 die，另一个
            // plane 的地址属于别的实例 ⇒ release 下也拒绝。
            ASSERT(same_die);
            if (same_die == false)
            {
                (void)fc_command_release(cmd->cmd_id);
                return FTL_ERROR;
            }
        }
    }

    {
        const ftl_status_e st = ftl_cmd_setup_ppa_pair_cw(cmd, pba, cw_bitmap);

        if (st != FTL_OK)
        {
            (void)fc_command_release(cmd->cmd_id);
            return st; /* FTL_AGAIN（desc/buffer 池空，瞬态）/ FTL_ERROR */
        }
    }

    if (ftl_cmd_write_data_cw(cmd, pair_buf, cw_bitmap, true) != FTL_OK)
    {
        (void)fc_command_release(cmd->cmd_id);
        return FTL_ERROR;
    }

    *out_cmd = cmd;
    return FTL_OK;
}

/**
 * @brief 异步单 plane 擦除
 *
 * 超时由 FC 命令超时与 job watchdog 承担，本函数不设。
 *
 * @param out_cmd 输出：建好的命令
 * @param pba     目标块 PBA
 * @return FTL_OK；FTL_AGAIN 池空；FTL_ERROR 地址非法
 */
ftl_status_e ftl_async_erase_block(fc_cmd_s **out_cmd, DWORD pba)
{
    const ftl_page_cfg_s *cfg = &s_page_cfg_normal;

    fc_cmd_s *cmd = ftl_cmd_prepare_alloc(OP_ERASE_BLOCK, cfg);
    if (cmd == NULL)
    {
        return FTL_AGAIN;
    }

    if (pba_to_cmd(cmd, pba) != FTL_OK)
    {
        (void)fc_command_release(cmd->cmd_id);
        return FTL_ERROR;
    }

    *out_cmd = cmd;
    return FTL_OK;
}

/**
 * @brief 异步 multi-plane 擦除
 *
 * FC 按 command->paa 的块号擦所有 plane（PBA 的 plane 字段被忽略）。
 *
 * @param out_cmd 输出：建好的命令
 * @param pba     目标块 PBA
 * @return FTL_OK；FTL_AGAIN 池空；FTL_ERROR 地址非法
 */
ftl_status_e ftl_async_erase_block_multi_plane(fc_cmd_s **out_cmd, DWORD pba)
{
    const ftl_page_cfg_s *cfg   = &s_page_cfg_normal;
    const fc_cmd_class_e  saved = ftl_cmd_class_set(FC_CMD_CLASS_CRIT);

    fc_cmd_s *cmd = ftl_cmd_prepare_alloc(OP_ERASE_BLOCK_MULTI_PLANE, cfg);
    if (cmd == NULL)
    {
        (void)ftl_cmd_class_set(saved);
        return FTL_AGAIN;
    }

    if (pba_to_cmd(cmd, pba) != FTL_OK)
    {
        (void)fc_command_release(cmd->cmd_id);
        (void)ftl_cmd_class_set(saved);
        return FTL_ERROR;
    }

    (void)ftl_cmd_class_set(saved);
    *out_cmd = cmd;
    return FTL_OK;
}

/**
 * @brief 申请一个命令租约（构造期 class 额度）
 *
 * @param lease 租约（非空）
 * @param cls   命令分配类
 * @return true 已持有命令；false 池/额度空
 */
BOOL ftl_cmd_lease_acquire(ftl_cmd_lease_s *lease, fc_cmd_class_e cls)
{
    ASSERT(lease != NULL);
    ASSERT(cls < FC_CMD_CLASS_NUM);

    lease->cls = cls;
    lease->cmd = fc_command_alloc_class(cls);
    return lease->cmd != NULL;
}

/**
 * @brief 按 class 取一条命令（内部经 command lease；调用者负责 release）
 *
 * @param cls 命令分配类
 * @return 命令；池/额度空返回 NULL
 */
fc_cmd_s *ftl_cmd_alloc_class(fc_cmd_class_e cls)
{
    ftl_cmd_lease_s lease;

    if (ftl_cmd_lease_acquire(&lease, cls) == false)
    {
        return NULL;
    }
    return ftl_cmd_lease_take(&lease);
}

/**
 * @brief 释放未提交的命令租约
 *
 * @param lease 租约（非空）
 */
void ftl_cmd_lease_release(ftl_cmd_lease_s *lease)
{
    ASSERT(lease != NULL);

    if (lease->cmd != NULL)
    {
        (void)fc_command_release(lease->cmd->cmd_id);
        lease->cmd = NULL;
    }
}

/**
 * @brief 取走租约里的命令（所有权转交调用者）
 *
 * @param lease 租约（非空）
 * @return 命令；租约已空时返回 NULL
 */
fc_cmd_s *ftl_cmd_lease_take(ftl_cmd_lease_s *lease)
{
    ASSERT(lease != NULL);

    fc_cmd_s *cmd = lease->cmd;

    lease->cmd = NULL;
    return cmd;
}

/**
 * @brief 异步页读：数据留在 desc 链上
 *
 * 服务函数必须在 release 前 ftl_cmd_read_data。
 *
 * @param out_cmd 输出：建好的命令
 * @param pba     目标页 PBA
 * @param cfg     页配置
 * @return FTL_OK；FTL_AGAIN 池空；FTL_ERROR 地址/建链失败
 */
ftl_status_e ftl_async_read_page(fc_cmd_s **out_cmd, DWORD pba, const ftl_page_cfg_s *cfg)
{
    fc_cmd_s *cmd = ftl_cmd_prepare_alloc(OP_NORMAL_READ_PAGE_WITH_DATA_OUT, cfg);
    if (cmd == NULL)
    {
        return FTL_AGAIN; /* 命令池空 = 瞬态背压 */
    }

    if (pba_to_cmd(cmd, pba) != FTL_OK)
    {
        (void)fc_command_release(cmd->cmd_id);
        return FTL_ERROR;
    }

    *out_cmd = cmd;
    return FTL_OK;
}

/**
 * @brief 用已租约的命令建链并异步读页
 *
 * 成功时命令所有权转交调用者；失败时命令仍归 lease，调用者负责 release。
 *
 * @param out_cmd 输出：建好的命令
 * @param lease   已持有的命令租约
 * @param pba     目标页 PBA
 * @param cfg     页配置
 * @return FTL_OK；FTL_ERROR 地址/建链失败
 */
ftl_status_e ftl_async_read_page_leased(fc_cmd_s **out_cmd, ftl_cmd_lease_s *lease, DWORD pba, const ftl_page_cfg_s *cfg)
{
    ASSERT(out_cmd != NULL);
    ASSERT(lease != NULL);
    ASSERT(lease->cmd != NULL);

    fc_cmd_s            *cmd   = lease->cmd;
    const fc_cmd_class_e saved = ftl_cmd_class_set(lease->cls);

    ftl_cmd_prepare_config(cmd, OP_NORMAL_READ_PAGE_WITH_DATA_OUT, cfg);

    const ftl_status_e st = pba_to_cmd(cmd, pba);

    (void)ftl_cmd_class_set(saved);

    if (st != FTL_OK)
    {
        return FTL_ERROR;
    }

    *out_cmd = ftl_cmd_lease_take(lease);
    return FTL_OK;
}

/**
 * @brief 异步 SET FEATURES
 *
 * 不绑 PBA（特性寄存器是 die 级）；ch/ce/lun 必须填，ftl_op_drain 靠它路由。
 *
 * @param out_cmd      输出：建好的命令
 * @param ch           目标 ch
 * @param ce           目标 ce
 * @param lun          目标 lun
 * @param feature_addr 特性地址
 * @param mode         写入值
 * @return FTL_OK；FTL_AGAIN 池空
 */
ftl_status_e ftl_async_set_features(fc_cmd_s **out_cmd, BYTE ch, BYTE ce, BYTE lun, BYTE feature_addr, BYTE mode)
{
    // 维护类命令走后台额度：本原语不得占用 L2P demand 加载的 CRIT 预留。
    fc_cmd_s *cmd = ftl_cmd_alloc_class(FC_CMD_CLASS_BG);
    if (cmd == NULL)
    {
        return FTL_AGAIN; /* 命令池空 = 瞬态背压 */
    }

    cmd->ch                 = ch;
    cmd->ce                 = ce;
    cmd->lun                = lun;
    cmd->config.operation   = OP_SET_FEATURES;
    cmd->config.crc         = FC_CRC_DISABLE;
    cmd->config.random      = FC_RANDOM_DISABLE;
    cmd->config.ldpc        = FC_ECC_MODE_NORMAL;
    cmd->config.multi_plane = 0U;
    cmd->config.chain       = FC_CHAIN_END;
    cmd->meta               = 0U;
    cmd->feature_addr       = feature_addr;
    cmd->param.value        = mode; /* 内联数据低字节；高 24 bit 为 0 */

    *out_cmd = cmd;
    return FTL_OK;
}

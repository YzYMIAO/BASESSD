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
#include "bst.h" /* ftl_pba_gblock（逐块 ECC 记账） */
#include "fc.h"
#include "ftl_op.h" /* ftl_op_submit / FTL_OP_SYNC_WAIT */
#include "ftl_type.h"
#include "onfi_1_0.h"

#include <string.h>

// 页级原语的两种常量配置（文件作用域初始化器，H47 允许；局部一律逐字段赋值）。
static const ftl_page_cfg_t s_page_cfg_raw   = {FC_CRC_DISABLE, FC_RANDOM_DISABLE, FC_ECC_MODE_LDPC_RAW, 0U};
static const ftl_page_cfg_t s_page_cfg_bch   = {FC_CRC_DISABLE, FC_RANDOM_DISABLE, FC_ECC_MODE_BCH, 0U};
// 判空专用：ECC 开（空/非空要靠它）、CRC/加扰/META 比对全关（与页面内容无关）。
static const ftl_page_cfg_t s_page_cfg_blank = {FC_CRC_DISABLE, FC_RANDOM_DISABLE, FC_ECC_MODE_BCH, FTL_LBA_UNKNOWN};

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
 * @note 实现要点：- **没有 0 的隐式展开**：名字叫 bitmap 就必须逐位字面，0 = 一个 CW 都不访问，
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

// 实现要点：- **只建链，不搬数据**：数据由 ftl_cmd_write_data 在链建好之后、且仅对写操作执行。
// 搬数据放在这里会让「建链失败 → 整链释放」白做一遍拷贝。
// - 无数据操作（ERASE / RESET 等）与 SET/GET_FEATURES 不建链，desc_id 置 INVALID
// （此路径**不看 cw_bitmap**，故擦除命令传什么都不会炸）。
// - cw_bitmap 是**字面位图**（0 = 空集，无隐式展开；整段访问用
// fc_command_plane_span_cw_bitmap() 算）。它必须落在本命令 plane 段内
// （越段 = 编程错误，见 ftl_cmd_validate_cw_bitmap 的 ASSERT + release 拒绝）。
// 离散读 plane1 的 CW0/CW2 即传 (1U << 4) | (1U << 6)。
// ⚠ 想「整段访问」不要传 0 —— 那是空集。用 pba_to_cmd / ftl_cmd_setup_ppa_pair_cw，
// 或显式传 fc_command_plane_span_cw_bitmap(cmd)。
// - 行地址所有 plane 共用 cmd->paa（同页语义）；两 plane 不同块（Policy B）
// 走 ftl_cmd_setup_ppa_pair_cw()，本函数不适用。
// - **失败时本函数自己回滚整条链**（desc + buffer 全部还池，desc_id 复位 INVALID），
// 调用者只需再 fc_command_release 还命令槽位。
ftl_status_e ftl_cmd_configure_descriptors(fc_command_t *cmd, DWORD cw_bitmap)
{
    // cmd 由调用者从 fc_command_alloc() 取且已判空；此处不重复断言。
    // 唯一合法失败是**池空**（desc/buffer）——那才是返回码。

    if (ftl_cmd_needs_desc_chain(cmd) == false)
    {
        cmd->desc_id = FC_DESCRIPTOR_ID_INVALID;
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
 * @note 实现要点：**未置位的 plane 整体跳过**——连 pba_is_valid 都不判。CW 级映射下一条
 * 子操作常只覆盖半个 pair（如只写 plane1 的 4 个 CW），此时调用者按约定
 * 可让另一 plane 的 pba 保持未初始化（见 ftl_issue_subop 的 pba 局部数组）。
 * 位图规则与 ftl_cmd_configure_descriptors 共用 ftl_cmd_validate_cw_bitmap。
 */
static ftl_status_e ftl_cmd_build_pair_chain(fc_command_t *cmd, const DWORD pba[PLANE_NUM], DWORD cw_bitmap)
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
            const fc_status_e st = fc_command_configure_descriptor(cmd, plane, cw, row_paa);
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

// CW 级多 plane 写建链：按被覆盖的第一个 plane 定段并绑拓扑；需要 desc 链时按位图挂链。
ftl_status_e ftl_cmd_setup_ppa_pair_cw(fc_command_t *cmd, const DWORD pba[PLANE_NUM], DWORD cw_bitmap)
{
    // cmd 来自调用者的 fc_command_alloc()（已判空）、pba 是调用者的局部数组
    // （见 ftl_issue_subop / wb_handle(FLUSH)）——两者给 NULL 都是调用者 bug ⇒ ASSERT。

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

    cmd->config.ch  = pba_to_ch(base_pba);
    cmd->config.ce  = pba_to_ce(base_pba);
    cmd->config.lun = pba_to_lun(base_pba);
    cmd->paa        = pba_to_paa(base_pba); /* PBA→PAA 在 FTL 侧一次转出 */

    if (ftl_cmd_needs_desc_chain(cmd) == false)
    {
        cmd->desc_id = FC_DESCRIPTOR_ID_INVALID;
        return FTL_OK;
    }

    // CW 级映射入口：位图由调用者给出（可只覆盖本命令实际写到的那些 CW）。
    // cmd->config.multi_plane 决定 plane 段，须由调用者**在建链前**设好。
    return ftl_cmd_build_pair_chain(cmd, pba, cw_bitmap);
}

// 实现要点：desc 的 PAA 经 paa_to_pba（本层唯一的 PBA↔PAA 转换漏斗）反解，
// 再经 ftl_pba_gblock 转全局块号。multi-plane 命令的两个 plane 是
// Policy B 下的两个独立块，必须分别记账（RISK-HOST-02）。
BYTE ftl_cmd_block_ecc_list(const fc_command_t *cmd, ftl_cmd_block_ecc_t *out, BYTE max)
{
    BYTE               count = 0U;
    fc_descriptor_id_t did   = (fc_descriptor_id_t)cmd->desc_id;

    while (did < FC_DESCRIPTOR_POOL_DEPTH)
    {
        const fc_descriptor_t *d   = fc_descriptor_id_to_descriptor(did);
        const DWORD            pba = paa_to_pba(cmd->config.ch, cmd->config.ce, d->paa);
        const WORD             gb  = ftl_pba_gblock(pba);

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
        did = (fc_descriptor_id_t)d->next_id;
    }
    return count;
}

/* ── 3. 数据搬运（desc 链 ↔ 外部缓冲）── */

// 把 data 按链序拷进 desc 的 FC 缓冲；数据耗尽后各 CW 用 0xFF 补齐，desc 无缓冲才返回 FTL_ERROR。
ftl_status_e ftl_cmd_write_data(fc_command_t *cmd, const void *data, DWORD length)
{
    const BYTE *src = (const BYTE *)data;

    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;

    while (desc_id < FC_DESCRIPTOR_POOL_DEPTH)
    {
        fc_descriptor_t *descriptor = fc_descriptor_id_to_descriptor(desc_id);

        if (descriptor->buffer_id == FC_BUFFER_ID_INVALID)
        {
            return FTL_ERROR;
        }
        BYTE *buf = fc_buffer_id_to_real_buffer(descriptor->buffer_id);

        if (length > 0U)
        {
            DWORD copy_len = (length >= FC_BUFFER_USER_SIZE) ? FC_BUFFER_USER_SIZE : length;

            (void)memcpy(buf, src, copy_len);
            src += copy_len;
            length -= copy_len;

            // 本 CW 未填满部分用 0xFF dummy 补齐
            if (copy_len < FC_BUFFER_USER_SIZE)
            {
                (void)memset(buf + copy_len, 0xFF, (FC_BUFFER_USER_SIZE - copy_len));
            }
        }
        else
        {
            // 数据已耗尽：本 CW 的 USER 区整体填 0xFF dummy。
            // ⚠ 长度必须是 USER 区大小——buf 是 base+FC_BUFFER_USER_OFFSET，
            // 照旧代码用 FC_BUFFER_POOL_CW_SIZE 会越界 8B 踩到下一 CW 的 META/CRC。
            (void)memset(buf, 0xFF, FC_BUFFER_USER_SIZE);
        }

        desc_id = (fc_descriptor_id_t)descriptor->next_id;
    }

    return FTL_OK;
}

// 实现要点：与 ftl_cmd_write_data 的差别只在**取数方式**——后者按链序**打包**消费
// 连续源缓冲（适合 host 写：host 数据本来就是紧凑的），本函数按 CW 偏移
// **散射**取数（适合 GC/WL 搬移：源是整页镜像，只需搬其中几个 CW，且
// 搬入位置必须保持原 CW 偏移——页内 META 的 LBA 由 CW 号决定，挪位即错）。
// 位图外的 CW 若仍在链上（调用者与链不同源）填 0xFF，绝不把镜像里的
// 无效 CW 数据带进新页。
//
// pair_major 决定源布局：
//   · false：单页镜像（2KB），两个 plane 读同一份数据（mirror/GC/WL）；
//   · true ：pair 镜像（4KB），槽位按 plane*CW_NUM + cw 排布（页打包 wbuf 的 h->data）。
// ⚠ 误用会让 plane1 复制 plane0（host 大文件静默损坏）。
ftl_status_e ftl_cmd_write_data_cw(fc_command_t *cmd, const BYTE *page_buf, DWORD cw_bitmap, bool pair_major)
{
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;

    while (desc_id < FC_DESCRIPTOR_POOL_DEPTH)
    {
        fc_descriptor_t *descriptor = fc_descriptor_id_to_descriptor(desc_id);

        // CW 号取 desc 自带的 config.cw；plane 必须从 desc 的 PAA 解出——
        // cw_bitmap 是 **flat 绝对编号**（bit = plane×CW_NUM + cw），
        // 只用 `1U << cw` 是 plane 盲的：plane 1 的 bits 4..7 在 bits 0..3 上全为 0，
        // 会把 plane 1 的每个 CW 都填成 0xFF（GC/WL 迁移静默丢数据）。
        const BYTE cw    = descriptor->config.cw;
        const BYTE plane = paa_to_plane(descriptor->paa);

        if (descriptor->buffer_id == FC_BUFFER_ID_INVALID)
        {
            return FTL_ERROR;
        }
        BYTE *buf = fc_buffer_id_to_real_buffer(descriptor->buffer_id);

        if ((cw_bitmap & ftl_cw_bit(plane, cw)) != 0U)
        {
            // 源偏移必须与调用者的镜像布局一致：
            // · pair 镜像（wbuf）：槽位 = plane*CW_NUM + cw；
            // · 单页镜像（mirror/GC/WL）：两个 plane 共用 cw 偏移。
            const size_t slot = pair_major ? (plane * CW_NUM + cw) : cw;

            (void)memcpy(buf, page_buf + (slot * FC_BUFFER_USER_SIZE), FC_BUFFER_USER_SIZE);
        }
        else
        {
            (void)memset(buf, 0xFF, FC_BUFFER_USER_SIZE);
        }

        desc_id = (fc_descriptor_id_t)descriptor->next_id;
    }

    return FTL_OK;
}

// 从 desc 链按链序拷出 length 字节；缓冲无效或链比请求短都返回 FTL_ERROR（不交出未初始化尾部）。
ftl_status_e ftl_cmd_read_data(fc_command_t *cmd, void *buffer, DWORD length)
{
    BYTE *dst = (BYTE *)buffer;

    DWORD remaining = length;

    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;

    while ((desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (remaining > 0U))
    {
        fc_descriptor_t *descriptor = fc_descriptor_id_to_descriptor(desc_id);

        if (descriptor->buffer_id == FC_BUFFER_ID_INVALID)
        {
            return FTL_ERROR;
        }
        BYTE *buf = fc_buffer_id_to_real_buffer(descriptor->buffer_id);
        if (buf == NULL)
        {
            return FTL_ERROR;
        }

        DWORD copy_len = (remaining >= FC_BUFFER_USER_SIZE) ? FC_BUFFER_USER_SIZE : remaining;
        (void)memcpy(dst, buf, copy_len);
        dst += copy_len;
        remaining -= copy_len;

        desc_id = (fc_descriptor_id_t)descriptor->next_id;
    }

    // 链比请求短 ⇒ 缓冲尾部未初始化；调用者会把它当有效数据用，必须报错。
    return (remaining == 0U) ? FTL_OK : FTL_ERROR;
}

/* ── 4. 同步提交等待 ── */

// 实现要点：① ftl_op_submit(cmd, FTL_OP_SYNC_WAIT, &waiter) —— 打 tag、登记栈上
// waiter、提交（登记早于提交）；② 自旋 fc_service() +
// ftl_op_drain() 直到 waiter.done；③ 超时摘链返回 FTL_ERROR。
// ⚠ 非目标 completion 由 drain 按各自 request.op 派发，本函数无需处理。
// ⚠ **不 release**：调用者要在返回后读 cmd 数据才 release（见 ftl_op.h）。
ftl_status_e ftl_sync_submit_wait(fc_command_t *cmd, fc_command_status_t *status_out, DWORD timeout_us)
{
    DWORD elapsed  = 0U;
    DWORD max_wait = (timeout_us > 0U) ? timeout_us : 10000U;

    // ⚠ **服务函数内不得同步等待**（贯穿机制 2）：在完成派发里再开一个完成泵
    // （本函数自旋 fc_service + ftl_op_drain）= 嵌套泵，正是本次异步化要
    // 消灭的形态——调用者的栈帧被钉住，且泵里再跑泵使重入面不可枚举。
    // 目标终态：本函数只被开机初始化 / 全片格式化 / 出厂坏块扫描 / CLI 调用
    // （那些路径在 superloop 之前，没有别人 drain CQ，必须同步）。
    // 新增调用点若触发本断言，说明该功能尚未 job 化——改用 ftl_async_* + ftl_op_submit。
    ASSERT(ftl_op_is_in_dispatch() == false);

    ftl_sync_waiter_t waiter;
    waiter.cmd_id = ftl_cmd_id(cmd);
    waiter.done   = false;

    if (ftl_op_submit(cmd, FTL_OP_SYNC_WAIT, &waiter) != FTL_OK)
    {
        return FTL_ERROR; /* 提交失败（背压/request 登记失败）——命令未提交，调用者自行 release */
    }

    for (;;)
    {
        (void)fc_service();
        ftl_op_drain();

        if (waiter.done)
        {
            break;
        }

        // 简易超时：elapsed 递增（精确度取决于 fc_service 调用频率）
        elapsed++;
        if (elapsed > max_wait)
        {
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
static fc_command_t *ftl_cmd_prepare_alloc(BYTE operation, const ftl_page_cfg_t *cfg)
{
    fc_command_t *cmd = fc_command_alloc();

    if (cmd == NULL)
    {
        return NULL;
    }

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
static fc_command_t *ftl_sync_page_prepare(DWORD pba, BYTE operation, const ftl_page_cfg_t *cfg)
{
    fc_command_t *cmd = ftl_cmd_prepare_alloc(operation, cfg);

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

// 同步页写原语：建链 → 必要时填数据 → 提交并等待；任一步失败都先释放命令。
ftl_status_e ftl_sync_write_page(DWORD pba, const void *data, DWORD length, const ftl_page_cfg_t *cfg)
{
    fc_command_t *cmd = ftl_sync_page_prepare(pba, OP_NORMAL_PROGRAM_PAGE, cfg);
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    if ((data != NULL) && (length > 0U) && (ftl_cmd_write_data(cmd, data, length) != FTL_OK))
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    fc_command_status_t completion;
    if (ftl_sync_submit_wait(cmd, &completion, 0U) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }
    if (fc_cmd_status_has_error(completion.status))
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    (void)fc_command_release(cmd);
    return FTL_OK;
}

// 实现要点：失败判据用 FC_CMD_STATUS_ERROR_MASK 全集（理由见下方行内 ⚠）。
ftl_status_e ftl_sync_read_page(DWORD pba, void *buffer, DWORD length, const ftl_page_cfg_t *cfg)
{
    fc_command_t *cmd = ftl_sync_page_prepare(pba, OP_NORMAL_READ_PAGE_WITH_DATA_OUT, cfg);
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    fc_command_status_t completion;
    if (ftl_sync_submit_wait(cmd, &completion, 0U) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }
    // 失败判据 = FC_CMD_STATUS_ERROR_MASK 全集（与 fc_command.h 的链级判据同口径）。
    // **只认 NAND_FAIL 是错的**：DATA_ERROR / MATA_ERROR 会让本页内容不可信却照常拷出——
    // · DATA_ERROR（UNC/CRC/RAID）：fc_data_path_read 在任何 CW 不可纠时**提前返回**，
    //   buffer 停在 "ECC 半修正 + 未解扰" 态（步骤 2/3/4 全跳过），拷出来的是加扰垃圾；
    // · MATA_ERROR：页内 META 与预期不符 = **读到了别的页**（或页已擦除）。
    // 后果实例：L2P region 页读回垃圾 → l2p_cache_find_or_load 标 CLEAN → 垃圾 PBA
    // 被 flush 写回介质**永久化**，读路径再拿它当 ch/ce/lun 路由字段用。
    // 空页也会命中 MATA_ERROR ⇒ 返回 FTL_ERROR ⇒ 调用者保持 entries
    // 全 NAND_PBA_INVALID，与"未映射"语义一致（这正是各调用点的既有兜底分支）。
    // ⚠ 未映射的 L2P region 不在此列：Directory 条目就是 INVALID，加载路径根本不发读。
    if (fc_cmd_status_has_error(completion.status))
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    if ((buffer != NULL) && (length > 0U) && (ftl_cmd_read_data(cmd, buffer, length) != FTL_OK))
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    (void)fc_command_release(cmd);
    return FTL_OK;
}

// 实现要点：**刻意不判 NAND 失败**——坏页也要拿原始数据（诊断 dump / 出厂坏块探针用）。
// ⚠ 它**不是**判空手段：LDPC_RAW 跳过 ECC，且 FC 只向 FTL 暴露 USER 512B，
//   META/CRC/parity 都拿不到；「RAW 读 + 数 FF」判不出「编程过没有」（RISK-IF-15）。
//   判空一律用 ftl_page_is_blank / ftl_page_blank_check（解码路径 + FC 的 CW 状态位）。
ftl_status_e ftl_sync_read_page_raw(DWORD pba, void *buffer)
{
    const ftl_page_cfg_t *cfg = &s_page_cfg_raw;

    fc_command_t *cmd = ftl_sync_page_prepare(pba, OP_NORMAL_READ_PAGE_WITH_DATA_OUT, cfg);
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    fc_command_status_t completion;
    if (ftl_sync_submit_wait(cmd, &completion, 0U) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    // 不判 NAND 失败：整页 4 个 CW 无条件拷出，每 CW 只拷 USER 512B（FC 的分层契约），
    // 槽尾 16B（META/CRC/parity）不填。
    // 但取不到 CW 数据是**控制器侧**错误（desc/buffer 缺失），不是坏页——此时返回 OK
    // 会让调用者看到空洞，故必须报错。
    for (BYTE cw = 0U; cw < CW_NUM; cw++)
    {
        const BYTE *src = fc_get_cmd_data_ptr(cmd, cw);

        if (src == NULL)
        {
            (void)fc_command_release(cmd);
            return FTL_ERROR;
        }
        BYTE *dst = (BYTE *)buffer + (cw * FC_BUFFER_POOL_CW_SIZE);

        (void)memcpy(dst, src, FC_BUFFER_USER_SIZE);
    }

    (void)fc_command_release(cmd);
    return FTL_OK;
}

// 实现要点：**走解码路径**——"这一页编程过没有"的证据（ECC 是否成码字、
// 整条 CW 的 1 比特占比、parity 是否写成）全在 FC，FTL 只拿得到 USER 区。
// ⇒ 这里只发一次普通读，然后读 FC 给出的 CW 状态位（fc_get_cmd_all_cw_empty）。
// ⚠ **不能用 RAW 读判空**：LDPC_RAW 刻意跳过 ECC，空/非空的依据全部被跳过；
//   而且 RAW 读只交出 USER 512B，META/CRC/parity 根本看不见（RISK-IF-15）。
// 读失败 / 超时一律判"非空"（保守方向）：命令没走通时不能断言"没写过"——
// 当空页会让调用者把写过块当空闲块复用（静默覆盖活数据）。
ftl_status_e ftl_page_blank_check(DWORD pba, bool *blank)
{
    ASSERT(blank != NULL);

    fc_command_t *cmd = ftl_sync_page_prepare(pba, OP_NORMAL_READ_PAGE_WITH_DATA_OUT, &s_page_cfg_blank);

    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    fc_command_status_t completion;

    if (ftl_sync_submit_wait(cmd, &completion, 0U) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    // 不看 completion.status：擦除页在 FC 里不是错误位（FC_CW_STATUS_EMPTY）。
    *blank = fc_get_cmd_all_cw_empty(cmd);

    (void)fc_command_release(cmd);
    return FTL_OK;
}

// 空白判定（解码后）：读不通一律判「非空」（保守方向），只有确实全 EMPTY 才返回 true。
bool ftl_page_is_blank(DWORD pba)
{
    bool blank = false;

    // 读命令没走通 ⇒ 判"非空"（保守方向）：命令没走通时不能断言"没写过"。
    return (ftl_page_blank_check(pba, &blank) == FTL_OK) && blank;
}

// 实现要点：擦除慢，超时放宽到 20ms（其余原语用默认 10ms）。
ftl_status_e ftl_sync_erase_block(DWORD pba)
{
    const ftl_page_cfg_t *cfg = &s_page_cfg_bch;

    fc_command_t *cmd = ftl_sync_page_prepare(pba, OP_ERASE_BLOCK, cfg);
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    fc_command_status_t completion;
    if (ftl_sync_submit_wait(cmd, &completion, 20000U) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }
    if (fc_cmd_status_has_error(completion.status))
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    (void)fc_command_release(cmd);
    return FTL_OK;
}

// 实现要点：OP_ERASE_BLOCK_MULTI_PLANE；只给页地址跨 plane 的固定池（Journal）用。
// 超时同单 plane 版本（20ms）。
ftl_status_e ftl_sync_erase_block_multi_plane(DWORD pba)
{
    const ftl_page_cfg_t *cfg = &s_page_cfg_bch;

    fc_command_t *cmd = ftl_sync_page_prepare(pba, OP_ERASE_BLOCK_MULTI_PLANE, cfg);
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    fc_command_status_t completion;
    if (ftl_sync_submit_wait(cmd, &completion, 20000U) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }
    if (fc_cmd_status_has_error(completion.status))
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    (void)fc_command_release(cmd);
    return FTL_OK;
}

// 实现要点：列地址走命令级 param 字节 0/1（小端）；4 字节先拷贝，再按
// FC_CMD_STATUS_ERROR_MASK 全集判错——读失败返回 FTL_ERROR，BBT 据此
// 保守判坏（否则失败页的 0xFF 参数会被误读成“好块”）。
ftl_status_e ftl_sync_read_column(DWORD pba, WORD column, BYTE out[4])
{
    const ftl_page_cfg_t *cfg = &s_page_cfg_bch;

    // 列 + 4 须落在页内：唯一调用者 bbt.c 传 PAGE_MAIN_SIZE（+4 ≤ PAGE_SIZE）⇒ 不重复校验。

    // OP_READ_COLUMN 的 has_data = 0 ⇒ pba_to_cmd 不建链、
    // 不看位图（desc_id 置 INVALID）。地址仍须落进 cmd->paa —— 但**只提供行地址**，
    // cw 位被 NAND 侧 paa_to_nand_row 丢掉；列地址由下面的 param 字节 0/1 给。
    fc_command_t *cmd = ftl_sync_page_prepare(pba, OP_READ_COLUMN, cfg);
    if (cmd == NULL)
    {
        return FTL_ERROR;
    }

    // 列地址走命令级 param 的字节 0/1（小端）——FC 侧解出后落进 request->column。
    // 字节 2/3 本 op 不用，显式清 0 保持 param 确定性。
    cmd->param.parameter0 = column;
    cmd->param.parameter1 = (column >> 8U);
    cmd->param.parameter2 = 0U;
    cmd->param.parameter3 = 0U;

    fc_command_status_t completion;
    if (ftl_sync_submit_wait(cmd, &completion, 0U) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    // 4 字节走控制器侧暂存（同 OP_READ_META），完成时已拷进 CQ 的 param。
    // 先拷出再判错：即使页读失败，调用者仍能看到原始 4 字节用于诊断；
    // 返回 FTL_ERROR 让 BBT 按“读失败 ⇒ 坏块（保守）”处理。
    (void)memcpy(out, completion.param.bytes, 4U);

    if (fc_cmd_status_has_error(completion.status))
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    (void)fc_command_release(cmd);
    return FTL_OK;
}

/* ── 6. 元数据镜像写：multi-plane 同页同数据（异步实现 + 同步封装）── */

// 数据填装用 ftl_cmd_write_data_cw：链上两个 plane 的 desc 的 config.cw 都是 0..3，
// 同一页镜像会按 CW 分别填进两个 plane（无需 2× 源缓冲）。
ftl_status_e ftl_async_write_page_mirror(fc_command_t **out_cmd, DWORD pba, const void *data, DWORD length, const ftl_page_cfg_t *cfg)
{
    ASSERT(pba_is_valid(pba));

    fc_command_t *cmd = ftl_cmd_prepare_alloc(OP_PROGRAM_PAGE_MULTI_PLANE, cfg);
    if (cmd == NULL)
    {
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
        (void)fc_command_release(cmd);
        return st; /* FTL_AGAIN 池空 / FTL_ERROR 建链失败 */
    }

    const BYTE *page = (const BYTE *)data;

    if (length != PAGE_MAIN_SIZE)
    {
        (void)memset(s_mirror_page, 0xFF, sizeof(s_mirror_page));
        (void)memcpy(s_mirror_page, data, length);
        page = s_mirror_page;
    }

    if (ftl_cmd_write_data_cw(cmd, page, bitmap, false) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    *out_cmd = cmd;
    return FTL_OK;
}

// 同步镜像写（目录检查点主备同页）：建链/提交/完成任一失败都压成 FTL_ERROR。
ftl_status_e ftl_sync_write_page_mirror(DWORD pba, const void *data, DWORD length, const ftl_page_cfg_t *cfg)
{
    fc_command_t *cmd;

    if (ftl_async_write_page_mirror(&cmd, pba, data, length, cfg) != FTL_OK)
    {
        return FTL_ERROR; /* 同步版把背压与硬失败都压成 FTL_ERROR */
    }

    fc_command_status_t completion;
    if (ftl_sync_submit_wait(cmd, &completion, 0U) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }
    if (fc_cmd_status_has_error(completion.status))
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    (void)fc_command_release(cmd);
    return FTL_OK;
}

/* ── 7. 异步页级原语（提交与完成解耦）──
 *
 * 与上面 ftl_sync_* 共用同一套 prepare 模板（ftl_cmd_prepare_alloc / 地址绑定 / 建链 /
 * 搬数据），差别只在最后一步：**不提交**，把命令交回调用者（由它 ftl_op_submit +
 * 服务函数 release）。
 * 返回码比同步版多一档 FTL_AGAIN：命令池空是瞬态背压，调用者应当**保留现场、稍后重试**，
 * 而同步版把它和"地址非法"一起压成 FTL_ERROR（同步调用者没有重试的地方）。 */

// 实现要点：数据在本函数内**已拷进 FC buffer 池**（ftl_cmd_write_data 是 memcpy）
// ⇒ 返回后调用者即可复用源缓冲，无需为异步写保留快照。
ftl_status_e ftl_async_write_page(fc_command_t **out_cmd, DWORD pba, const void *data, DWORD length, const ftl_page_cfg_t *cfg)
{
    fc_command_t *cmd = ftl_cmd_prepare_alloc(OP_NORMAL_PROGRAM_PAGE, cfg);
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

// 实现要点：ftl_cmd_prepare_alloc + 手工地址绑定（**不走 pba_to_cmd**：它的语义是
// 「整段访问」，重载它会让「整段 vs 子集」这个关键差别从调用点消失）
// + ftl_cmd_write_data_cw。
ftl_status_e ftl_async_write_page_cw(fc_command_t **out_cmd, DWORD pba, const BYTE *page_buf, DWORD cw_bitmap, const ftl_page_cfg_t *cfg)
{
    fc_command_t *cmd = ftl_cmd_prepare_alloc(OP_NORMAL_PROGRAM_PAGE, cfg);
    if (cmd == NULL)
    {
        return FTL_AGAIN; /* 命令池空 = 瞬态背压 */
    }

    // 边界不变量同 pba_to_cmd：进 FC 的 ch/ce/lun 被当 suspended[][][] 下标直接用。
    ASSERT(pba_is_valid(pba));
    cmd->config.ch  = pba_to_ch(pba);
    cmd->config.ce  = pba_to_ce(pba);
    cmd->config.lun = pba_to_lun(pba);
    cmd->paa        = pba_to_paa(pba);

    if (ftl_cmd_configure_descriptors(cmd, cw_bitmap) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    if (ftl_cmd_write_data_cw(cmd, page_buf, cw_bitmap, false) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    *out_cmd = cmd;
    return FTL_OK;
}

// 实现要点：wbuf 的普通页写与 FTL cache program 批次共用本函数（C2/R9）。
// 支持稀疏位图（单 plane 普通写）；cache op 必须覆盖全部 plane。
// 多 plane 命令的 desc 链必须覆盖每个 plane；各 plane 必须同 ch/ce/lun。
ftl_status_e ftl_async_write_pair_cmd(fc_command_t **out_cmd, const DWORD pba[PLANE_NUM], const BYTE *pair_buf, DWORD cw_bitmap, BYTE operation, const ftl_page_cfg_t *cfg)
{
    fc_command_t *cmd = ftl_cmd_prepare_alloc(operation, cfg);

    if (cmd == NULL)
    {
        return FTL_AGAIN;
    }

    const BYTE nplanes  = ftl_cw_bitmap_plane_count(cw_bitmap);
    const bool cache_op = (operation == OP_TWO_PLANE_PROGRAM_CACHE) || (operation == OP_TWO_PLANE_PROGRAM_CACHE_FIRST) || (operation == OP_TWO_PLANE_PROGRAM_CACHE_END);

    ASSERT((cache_op == false) || (nplanes == PLANE_NUM));
    (void)cache_op; /* release 下 ASSERT 裁空 */

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
            ASSERT((pba_to_ch(pba[p]) == pba_to_ch(first_pba)) && (pba_to_ce(pba[p]) == pba_to_ce(first_pba)) && (pba_to_lun(pba[p]) == pba_to_lun(first_pba)));
        }
    }

    {
        const ftl_status_e st = ftl_cmd_setup_ppa_pair_cw(cmd, pba, cw_bitmap);

        if (st != FTL_OK)
        {
            (void)fc_command_release(cmd);
            return st; /* FTL_AGAIN（desc/buffer 池空，瞬态）/ FTL_ERROR */
        }
    }

    if (ftl_cmd_write_data_cw(cmd, pair_buf, cw_bitmap, true) != FTL_OK)
    {
        (void)fc_command_release(cmd);
        return FTL_ERROR;
    }

    *out_cmd = cmd;
    return FTL_OK;
}

// 实现要点：擦除慢（20ms 超时）是**同步版**的形参；异步版的超时由 FC 层自己的
// 命令超时与 job watchdog 承担，本函数不设。
ftl_status_e ftl_async_erase_block(fc_command_t **out_cmd, DWORD pba)
{
    const ftl_page_cfg_t *cfg = &s_page_cfg_bch;

    fc_command_t *cmd = ftl_cmd_prepare_alloc(OP_ERASE_BLOCK, cfg);
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

// 实现要点：OP_ERASE_BLOCK_MULTI_PLANE；FC 侧按 command->paa 的 block 号
// 对**所有 plane** 生成擦除地址（PBA 的 plane 字段被忽略）。
ftl_status_e ftl_async_erase_block_multi_plane(fc_command_t **out_cmd, DWORD pba)
{
    const ftl_page_cfg_t *cfg = &s_page_cfg_bch;

    fc_command_t *cmd = ftl_cmd_prepare_alloc(OP_ERASE_BLOCK_MULTI_PLANE, cfg);
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

// 实现要点：与 ftl_async_write_page 的**不对称**——写在本函数内已把数据拷进 FC
// buffer 池，读没有：数据留在 desc 链上，服务函数必须在 release **之前**
// 调 ftl_cmd_read_data 才拿得到（否则命令回收、数据丢失）。
ftl_status_e ftl_async_read_page(fc_command_t **out_cmd, DWORD pba, const ftl_page_cfg_t *cfg)
{
    fc_command_t *cmd = ftl_cmd_prepare_alloc(OP_NORMAL_READ_PAGE_WITH_DATA_OUT, cfg);
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

// 实现要点：**不绑 PBA**——cmd->paa 恒 0（fc_command_alloc 已清零）：特性寄存器是
// die 级、无 plane/page 语义，FC 侧 OP_SET_FEATURES 分支也不读它。
// 但 ch/ce/lun **必须**填——ftl_op_drain 靠它们解析归属实例。
// 其余命令配置逐字沿用原 read_recovery.c 内联版本的字段取值，差别只有"不提交"。
ftl_status_e ftl_async_set_features(fc_command_t **out_cmd, BYTE ch, BYTE ce, BYTE lun, BYTE feature_addr, BYTE mode)
{
    fc_command_t *cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return FTL_AGAIN; /* 命令池空 = 瞬态背压 */
    }

    cmd->config.ch          = ch;
    cmd->config.ce          = ce;
    cmd->config.lun         = lun;
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

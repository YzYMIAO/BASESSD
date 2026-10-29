#ifndef FTL_FC_INTERFACE_H
#define FTL_FC_INTERFACE_H

/*
 * FTL-FC Interface 层
 * ===================
 *
 * 【desc 链组装】
 *   ① ftl_cmd_configure_descriptors(cmd, cw_bitmap) — 按形参 cw_bitmap 建整条 desc 链
 *   ③ ftl_cmd_setup_ppa_pair_cw() — 每 plane 各自的 PBA + 显式位图 → 建链（Policy B）
 *   ④ pba_to_cmd() — DWORD 直填 + 整段位图 + 建链（遍历路径 / 同步页原语）
 *      ⑩ ftl_cmd_pba() — FC 命令 → PBA（④ 的逆，供 FTL 模块回读自己下发的地址）
 *
 *   位图规则（①③ 共用，唯一定义在 ftl_cmd_validate_cw_bitmap）：
 *     · cw_bitmap 是**字面位图**（bit 置 1 = 要访问该 CW；0 = 空集，无隐式展开）；
 *     · ③④ 两个「整段访问」入口各自显式算 fc_command_plane_span_cw_bitmap(cmd)，
 *       只有 ① 接受调用者给的部分位图（空集 / 越段会被校验函数拒绝）；
 *     · 位图是建链的**输入**，不存进命令结构体（③④ 算好后直接传参）；
 *     · config.cw 不参与建链（仍被 FC 的 RAID stripe 读取，见 fc_command.h）。
 *
 * 【数据搬运】
 *   ⑤ ftl_cmd_write_data() — 用户数据写入 desc 链各 CW 的 USER 区
 *   ⑥ ftl_cmd_read_data() — 从 desc 链各 CW 的 USER 区拷出
 *
 * 【同步等待】
 *   ⑦ ftl_sync_submit_wait() — 提交命令并阻塞等待其完成（ftl_op 机制的客户端）
 *
 * 【页级同步原语】
 *   ⑧ ftl_sync_write_page / ftl_sync_read_page / ftl_sync_read_page_raw /
 *      ftl_sync_erase_block / ftl_sync_erase_block_multi_plane / ftl_sync_read_column /
 *      ftl_sync_write_page_mirror — 差异由 ftl_page_cfg_t（crc/random/ldpc/lba）承载。
 *
 * 【异步页级原语】（提交与完成解耦）
 *   ⑩ ftl_async_write_page / ftl_async_write_page_cw / ftl_async_erase_block /
 *      ftl_async_erase_block_multi_plane /
 *      ftl_async_read_page / ftl_async_set_features — 只建链 + 搬数据，**不提交**；
 *      命令交回调用者，由调用者 ftl_op_submit(cmd, <功能 op>, ctx)。服务函数负责
 *      fc_command_release。
 *      写/擦与 ⑧ 共用同一套 prepare 模板；**读**的数据留在 desc 链上（非 prepare 拷贝）
 *      ⇒ 目标缓冲须活到完成、ftl_cmd_read_data 须早于 release；SET_FEATURES 无 PBA
 *      ⇒ ch/ce/lun 只能显式给（完成派发的实例路由键）。
 *
 * 【命令身份】
 *   ⑨ ftl_cmd_id() — 命令指针 → FC 命令池 ID（由指针派生，见 fc_command.h）。
 *
 * 为什么这些在本层：FC 定位为**硬件 FCC 的软件替身**——只提供槽位/描述符/
 * 队列/数据路径，不含策略。而「访问哪些 CW」（掩码枚举）与「数据怎么搬」（拷贝）
 * 在行业里都是**固件侧**的事（硬件 FCC 既不提供「按掩码建链」寄存器，也不提供
 * 「数据拷贝」寄存器——描述符链是固件填的、数据是固件 DMA 填的），故落在本层。
 */

#include "assertion.h"
#include "fc_interface.h"
#include "ftl_config.h" /* DWORD, pba_to_paa */
#include "ftl_type.h"
#include "nand_config.h"
#include "onfi_1_0.h"

#include <stdint.h>

/* ── host 资源预算：给后台（CP / GC / journal / 元数据）预留 FC 池 ──
 *
 * 一个 4KB host 子操作最多占 FTL_LBA_PER_PAIR（8）个 desc/buffer；FC buffer 池只有
 * 128 ⇒ 16 个在飞 host 写就能吃满。后台（CP 的 region flush / GC 的搬移 / journal 页）
 * 此时拿不到命令与 buffer ⇒ CP 无法写 Header / 块表 ⇒ 掉电后每次开机都走全量重建
 * （RISK-CP-04 / RISK-WL-07）。这里让 host 写在池接近满时先背压，保留 2 个 pair 规模
 * 的后台资源。
 *
 * 判据：host 的**下一次**子操作之后，各池仍至少剩 RESERVED 个槽位。
 * ⚠ 只约束 host 路径；后台路径（CP/GC/journal）直接走各自的 async 原语，不经过本检查。 */
#define FTL_FC_HOST_RESERVED_CMDS (4U)
#define FTL_FC_HOST_RESERVED_DESC (2U * FTL_LBA_PER_PAIR)
#define FTL_FC_HOST_RESERVED_BUFS (2U * FTL_LBA_PER_PAIR)
#define FTL_FC_HOST_RESERVED_SQ   (4U)
#define FTL_FC_HOST_RESERVED_HSQ  (2U)

/** @brief host 是否还有余量提交 n 个子操作。查询失败时不阻塞前台（宁可少一次预留）。 */
static inline bool ftl_fc_host_budget_ok_n(DWORD n)
{
    fc_resource_stats_t st;

    if (fc_get_resource_stats(&st) != FC_STATUS_OK)
    {
        return true;
    }

    const DWORD cw = n * FTL_LBA_PER_PAIR;

    return (st.command_used + n + FTL_FC_HOST_RESERVED_CMDS <= FC_COMMAND_POOL_DEPTH) && (st.descriptor_used + cw + FTL_FC_HOST_RESERVED_DESC <= FC_DESCRIPTOR_POOL_DEPTH) && (st.buffer_used + cw + FTL_FC_HOST_RESERVED_BUFS <= FC_BUFFER_POOL_DEPTH) && (st.sq_used + n + FTL_FC_HOST_RESERVED_SQ <= FC_SQ_DEPTH) && (st.hsq_used + FTL_FC_HOST_RESERVED_HSQ <= FC_HSQ_DEPTH);
}

/** @brief host 是否还有余量提交一个子操作。查询失败时不阻塞前台（宁可少一次预留）。 */
static inline bool ftl_fc_host_budget_ok(void)
{
    return ftl_fc_host_budget_ok_n(1U);
}

/* ── desc 链组装共用小工具 ──
 *   plane 段规则用 fc_command.h 的 fc_command_plane_span（唯一一份定义，
 *   FC 的越段校验与 FTL 的掩码展开共用）。 */

/**
 * @brief 命令指针 → FC 命令池 ID。
 *
 * fc_command_t 里**没有** command_id 字段：ID 由「指针 − command_pool 基址」导出
 * （见 fc_command.h）。本层调用者手里的 cmd 恒来自 fc_command_alloc()，故 ID 必然有效；
 * 池外指针（栈上的 fc_command_t）得到 FC_COMMAND_ID_INVALID——那是编程错误，
 * 调用方（pending_add / sync_wait）会因查不到而失败，不会静默串命令。
 */
static inline WORD ftl_cmd_id(const fc_command_t *cmd)
{
    fc_command_id_t command_id = FC_COMMAND_ID_INVALID;

    (void)fc_command_to_id(cmd, &command_id);
    return command_id;
}

/**
 * @brief 该操作是否需要 desc 链（即是否携带数据）
 *
 * NO_DATA 操作（ERASE / RESET / LOCK 等）无数据负载；SET/GET_FEATURES 用内联
 * param.bytes + feature_addr。两者都不建链（desc_id 保持 INVALID）。
 * 与 fc_command_validate 的接受条件对齐：只有 has_data 且非 FEATURES 的操作
 * 才要求 desc_id 有效。
 */
static inline bool ftl_cmd_needs_desc_chain(const fc_command_t *cmd)
{
    const BYTE op = cmd->config.operation;

    return (nand_operation_has_data(op) != 0U) && (op != OP_SET_FEATURES) && (op != OP_GET_FEATURES);
}

/* ── plane 数 → 命令头字段（读/写各一）──
 *
 * 页读/写命令的 operation 与 config.multi_plane 都由「本子操作覆盖几个 plane」决定。
 * 两者**成对**设置，故收敛在这里，调用点只写一行：
 *
 *     cmd->config.operation   = ftl_plane_write_op(nplanes);
 *     cmd->config.multi_plane = ftl_plane_multi_field(nplanes);
 *
 * multi_plane 字段携带**真实 plane 数**（1 → 0 = 单 plane 语义；N>1 → N）——下游
 * fc_command_plane_span（plane 段）与 fc.c 的 all_planes 掩码都按它算，**不假设 2**。
 * 本器件（MT29F4G08，ONFI 1.0）的 opcode 表里只有「单 plane」与「多 plane」两种页读/
 * 写码（onfi_1_0.c 的 g_nand_multi_plane_*_uops 按 2 plane 定尺）；4/6 plane 器件须
 * **先在 opcode 表加码**，再在此处扩展——这里泛化的是**形状**（plane 数不写死、界取自
 * PLANE_NUM），不是凭空造出不存在的 opcode。 */

/**
 * @brief plane 数 → config.multi_plane 字段。
 * 纯打包：nplanes 的值域由调用点紧邻的 ftl_plane_*_op 断言（同一个 nplanes）。
 */
static inline BYTE ftl_plane_multi_field(BYTE nplanes)
{
    return ((nplanes > 1U) ? nplanes : 0U);
}

/** @brief plane 数 → 读 operation 码。界取 PLANE_NUM，与 fc_command_plane_span 的上界同源。 */
static inline BYTE ftl_plane_read_op(BYTE nplanes)
{
    return ((nplanes > 1U) ? OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT : OP_NORMAL_READ_PAGE_WITH_DATA_OUT);
}

/** @brief plane 数 → 写 operation 码。界同上。 */
static inline BYTE ftl_plane_write_op(BYTE nplanes)
{
    return ((nplanes > 1U) ? OP_PROGRAM_PAGE_MULTI_PLANE : OP_NORMAL_PROGRAM_PAGE);
}

/**
 * @brief cache program 批次第 j 页（共 n 页）使用的 op：
 * 首页 = FIRST（只查 FAIL）、中间页 = CACHE（查 FAIL|FAILC）、末页 = END（10h）。
 */
static inline BYTE ftl_cache_prog_op(BYTE j, BYTE n)
{
    if (j == 0U)
    {
        return OP_TWO_PLANE_PROGRAM_CACHE_FIRST;
    }
    if ((BYTE)(j + 1U) < n)
    {
        return OP_TWO_PLANE_PROGRAM_CACHE;
    }
    return OP_TWO_PLANE_PROGRAM_CACHE_END;
}

/* ── desc 链组装 ── */

/**
 * @brief 按形参 cw_bitmap 组装整条 desc 链
 *
 * cw_bitmap 是 flat 绝对编号（bit n = plane*CW_NUM + cw）；**字面位图**——
 * 置 1 的位就是本命令要访问的 CW，**0 = 空集**（不是"全部"）。置位的每个 CW 逐条调
 * fc_command_configure_descriptor()，故链序 = plane-major、plane 内 CW 递增。
 * 想只读离散 CW（例如 plane1 的 CW0/CW2）：传 (1U<<4)|(1U<<6)。
 * 想整段访问：传 fc_command_plane_span_cw_bitmap(cmd)
 *   —— 或者干脆用 pba_to_cmd()，它替你算。
 *
 * 位图是本函数的**形参**，不是命令结构体的字段——它是"这次建链要建哪些 CW"的
 * 输入，用完即弃。
 *
 * 行地址：所有 plane 共用 cmd->paa 的行地址（同页语义）。**两 plane 不同块
 * （Policy B）请用 ftl_cmd_setup_ppa_pair_cw()**，本函数不适用。
 *
 * @param cmd       须已设 config.operation / multi_plane / paa（config.cw 已不读）
 * @param cw_bitmap flat 字面位图；无数据 op（ERASE / RESET / FEATURES）不读它
 * @return FTL_OK / FTL_ERROR（池空；位图 0 或越出 plane 段是编程错误 → ASSERT）
 */
ftl_status_e ftl_cmd_configure_descriptors(fc_command_t *cmd, DWORD cw_bitmap);

/* ── PBA helper（**自己算整段位图**并传给建链入口） ── */

/**
 * @brief 按**每 plane 各自**的 PBA + 显式位图建 desc 链（Policy B）
 *
 * 与 pba_to_cmd 的区别：后者把单一 PBA 转成 PAA 填进 command->paa 后展开到各 plane，
 * 隐含「两 plane 同 block」；本函数逐 plane 取各自 PBA 的行地址（转成 PAA）写进对应
 * plane 的 desc。CW 级映射（IU = 1 CW）专用入口：一条子操作常只覆盖一页里的**部分 CW**
 * （512B 扇区写、页尾残段、GC 只搬有效 CW），故位图由调用者显式给出。
 *
 * ⚠ **plane 号取自每个 PBA 自己的 plane 位，不是数组下标**：读路径的 pba[]
 *   是按 LBA 查出来的，plane 取决于当初怎么写进去（例如 `wr 0 2` 之后
 *   lba 1 映射到 plane 1），下标与 plane 号并不总相等。
 *
 * ⚠ **cmd->config.multi_plane 必须由调用者建链前设好**——它决定 plane 段，
 *   位图须落在该段内（单 plane 命令的段 = paa 的 plane 那一个，如 plane 1 → 0xF0）。
 * ⚠ 未置位的 plane **整体跳过**（连 pba_is_valid 都不判）⇒ 调用者可按约定让
 *   未用到的那半个 pair 的 pba 保持未初始化。
 *
 * @param cmd        FC 命令（须已设 config.operation / multi_plane / meta）
 * @param pba        [PLANE_NUM] 每 plane 的 PBA（ch/ce/lun 取 pba[0]）
 * @param cw_bitmap  flat 字面位图（bit = plane*CW_NUM + cw，0 = 空集 → ASSERT）
 * @return FTL_OK / FTL_AGAIN（desc/buffer 池空）/ FTL_ERROR（位图越段 → ASSERT）
 */
ftl_status_e ftl_cmd_setup_ppa_pair_cw(fc_command_t *cmd, const DWORD pba[PLANE_NUM], DWORD cw_bitmap);

/* ── 地址直填：DWORD → FC 命令（PBA→PAA 的唯一关卡之一）──
 *
 * FTL 侧的地址**一律**是 PBA（L2P / Journal / BST / Metadata Directory 存的就是它）。
 * 只有在这里——建 FC 命令的最后一刻——才做 PBA → PAA 的字段重排：
 *   ch/ce/lun           → cmd->config 位域
 *   block/plane/page/cw → cmd->paa（pba_to_paa 在 FTL 侧一次转出）
 *
 * 本入口是同步页原语与遍历路径（GC / 元数据扫描 / Checkpoint）的公共底座，
 * 语义是「整段访问」（单一 PBA，隐含「两 plane 同 block」；Policy B 用 _pair_cw 入口）。
 * 地址填好后**必须建 desc 链**——否则命令无 buffer，数据无从搬运，
 * fc_submit 也会被 fc_command_validate 以 desc_id == INVALID 拒绝。
 * 无数据操作（ERASE / RESET / META / COLUMN 等）由 ftl_cmd_configure_descriptors
 * 短路为 desc_id = INVALID。
 *
 * 本入口把 fc_command_plane_span_cw_bitmap(cmd)（「本命令 plane 段内全部 CW」）算出来
 * 直接作实参交给建链入口——字面位图没有隐式展开，遍历路径（GC / 元数据 / checkpoint /
 * sync 页原语）一律整段访问，故在这里一次算好，调用者不必管。
 */
static inline ftl_status_e pba_to_cmd(fc_command_t *cmd, DWORD pba)
{
    /* cmd 由调用者从 fc_command_alloc() 取且已判空（判据 2：层内模块头 → 前置条件）。 */

    /* 边界不变量：进 FC 的 ch/ce/lun 被 fc_poll_fill_queue 当 suspended[][][] 下标
     * 直接用，越界即栈越界写。校验必须自带；否则 fc.c 那句「PBA 已过 pba_is_valid()
     * 校验」在遍历路径（同步页原语 / GC / 元数据扫描 / Checkpoint）上仍是假命题。 */
    ASSERT(pba_is_valid(pba));

    cmd->config.ch  = pba_to_ch(pba);
    cmd->config.ce  = pba_to_ce(pba);
    cmd->config.lun = pba_to_lun(pba);
    /* PBA 在 FTL 侧就地转 PAA——FC 层只见 PAA，不再有 pba_to_paa 调用点。
     * ⚠ 必须在建链之前转（下面 fc_command_plane_span_cw_bitmap 读 cmd->paa）。 */
    cmd->paa        = pba_to_paa(pba);

    /**
     * @brief 位图不存进命令结构体，直接算出来当实参传。
     * 须在 paa 写好之后算（fc_command_plane_span_cw_bitmap 读 paa 与 multi_plane）。
     */
    return ftl_cmd_configure_descriptors(cmd, fc_command_plane_span_cw_bitmap(cmd));
}

/**
 * @brief FC 命令 → PBA（pba_to_cmd 的逆）
 *
 * FTL 模块要回读自己下发的命令地址时用它，**不必**直接摸 cmd->paa——PAA 是 FC/器件域
 * 的表示。ch/ce 取 cmd->config 位域，其余字段由 paa_to_pba() 反重排。
 *
 * ⚠ 本函数与 pba_to_cmd() 是 FTL 层仅有的两处 PBA↔PAA 转换点。
 */
static inline DWORD ftl_cmd_pba(const fc_command_t *cmd)
{
    return paa_to_pba(cmd->config.ch, cmd->config.ce, cmd->paa);
}

/* 一个命令实际访问到的**全局块**及其本命令内的最大 ECC 纠正 bit 数。 */
typedef struct
{
    WORD gblock;
    BYTE ecc_max;
} ftl_cmd_block_ecc_t;

/**
 * @brief 逐 plane 反查命令访问的全局块与 ECC
 *
 * 一个 multi-plane 子操作在 Policy B 下覆盖**两个独立块**；host 读成功后的
 * read_count / ECC 趋势记账必须逐块进行，只记 `ftl_cmd_pba()` 的首个 desc 会漏掉
 * 第二个 plane（RISK-HOST-02）。本函数遍历 desc 链、按 gblock 去重、取每块内
 * ECC 最大值——desc 链布局与 PBA→PAA 转换都留在本层（H13）。
 *
 * @param cmd 已完成、且尚未 release 的命令（desc 链仍有效）
 * @param out 输出数组（长度 >= max）
 * @param max 输出容量
 * @return 实际写入的块数（0..max）；链为空返回 0
 */
BYTE ftl_cmd_block_ecc_list(const fc_command_t *cmd, ftl_cmd_block_ecc_t *out, BYTE max);

/* ── 数据搬运 ── */

/**
 * @brief 把外部数据按 CW 顺序写入 desc 链上各 CW 的 USER 区
 *
 * 硬件 FCC 不提供「按 CW 拷贝」寄存器，数据由固件在 desc 链上逐 CW 填。
 * 遍历界走 desc 链（链长度是 CW 数的唯一真值）；数据耗尽后剩余 CW 的 USER 区填 0xFF
 * dummy（不把上一轮残值写进 NAND）。
 *
 * @param cmd     已建链的命令（desc_id 有效）
 * @param data    源数据
 * @param length  字节数
 * @return FTL_OK / FTL_ERROR（链上 desc 无 buffer）
 */
ftl_status_e ftl_cmd_write_data(fc_command_t *cmd, const void *data, DWORD length);

/**
 * @brief CW 级数据填装（按页内 CW 偏移从整页镜像散射取数）
 *
 * 与 ftl_cmd_write_data 的差别只在取数方式：后者按**链序打包**消费连续源缓冲
 * （host 写路径：主机数据本来就紧凑），本函数按 **CW 偏移**从整页镜像取数
 * （GC/WL 搬移：源是整页镜像，只需搬其中几个有效 CW，且搬入位置必须保持原 CW 偏移
 * ——页内 META 的 LBA 由 CW 号决定，挪位即错）。位图外的 CW 填 0xFF。
 *
 * @param cmd        已建链的命令（链上 CW ⊆ cw_bitmap）
 * @param page_buf   整页镜像（FTL_READ_USER_SIZE 字节，CW c 的数据在 page_buf + c×512）
 * @param cw_bitmap  本页哪些 CW 有效（flat 字面位图，与本命令链同源）
 * @return FTL_OK / FTL_ERROR（链上 desc 无 buffer）
 */
ftl_status_e ftl_cmd_write_data_cw(fc_command_t *cmd, const BYTE *page_buf, DWORD cw_bitmap);

/**
 * @brief CW 级数据填装（**plane-major pair 镜像**）
 *
 * 与 ftl_cmd_write_data_cw 的唯一差别是源布局：
 *   · `ftl_cmd_write_data_cw`：单页镜像（2KB），两个 plane 读同一份数据（mirror/GC/WL）；
 *   · 本函数：pair 镜像（4KB），槽位按 `plane*CW_NUM + cw` 排布（页打包 wbuf
 *     的 `h->data` 就是这个布局）。
 * ⚠ 两者不能合并成同一套取数：wbuf 的 plane1 数据在 `page_buf + 4*512`，而 mirror
 *   的两个 plane 本来就该相同；误用会让 plane1 复制 plane0（host 大文件静默损坏）。
 *
 * @param cmd        已建链的命令（链上 CW ⊆ cw_bitmap，可跨两个 plane）
 * @param pair_buf   pair 镜像（`FTL_LBA_PER_PAIR * FTL_IU_BYTES` 字节）
 * @param cw_bitmap  本页哪些 CW 有效（flat 位图，bit = plane*CW_NUM + cw）
 * @return FTL_OK / FTL_ERROR（链上 desc 无 buffer）
 */
ftl_status_e ftl_cmd_write_data_pair_cw(fc_command_t *cmd, const BYTE *pair_buf, DWORD cw_bitmap);

/**
 * @brief 从 desc 链各 CW 的 USER 区按链序拷出到外部缓冲
 *
 * 与 ftl_cmd_write_data 对称；遍历界走 desc 链，数据够即停。
 *
 * @param cmd     已完成的命令（desc_id 有效）
 * @param buffer  输出缓冲
 * @param length  字节数
 * @return FTL_OK / FTL_ERROR（链上 desc 无 buffer）
 */
ftl_status_e ftl_cmd_read_data(fc_command_t *cmd, void *buffer, DWORD length);

/* ── 内部同步 NAND 操作（仅保留 sync_wait）── */

/**
 * @brief 提交命令并阻塞等待其完成（同步提交的唯一入口）
 *
 * 内部就是 ftl_op 机制的一个客户端：
 *   ① ftl_op_submit(cmd, FTL_OP_SYNC_WAIT, &waiter) —— 打 tag、登记栈上 waiter、提交；
 *   ② 自旋 fc_service() + ftl_op_drain() 直到 waiter.done；
 *   ③ 超时则 ftl_op_ctx_remove 后返回 FTL_ERROR。
 *
 * ⚠ **只在结构性必须顺序执行的路径上用**：开机初始化 / 全片格式化 / 出厂坏块扫描 /
 *   CLI。主循环里（ftl_service_pump 已存在）的路径一律异步化——在这些路径上同步等待
 *   会把调用者栈帧钉住并重入完成泵。
 *
 * ⚠ **不 release 命令**——调用者要在返回后读 cmd 数据（如 ftl_cmd_read_data）才 release。
 *
 * @param cmd         已建链的命令（本函数负责提交）
 * @param status_out  输出：完成状态（可 NULL）
 * @param timeout_us  超时微秒（0 = 默认 10ms）
 * @return FTL_OK 完成（status_out 有效）；FTL_ERROR 提交失败 / 超时 / 旁表满
 */
ftl_status_e ftl_sync_submit_wait(fc_command_t *cmd, fc_command_status_t *status_out, DWORD timeout_us);

/* ── 页级同步原语 ──
 *
 * 各原语的差异由 ftl_page_cfg_t 承载（crc / random / ldpc / lba）；
 * 配置是编译期常量，故用复合字面量宏调用：
 *
 *   元数据页（BBT / checkpoint / journal / L2P / metadata / 擦除）：
 *     ftl_sync_write_page(pba, buf, len, FTL_PAGE_CFG_META());
 *   用户数据页（GC / WL 搬移、Read Recovery 重读）：
 *     ftl_sync_write_page(pba, buf, len, FTL_PAGE_CFG_DATA(lba));
 *   裸页（LDPC_RAW，忽略 NAND 成败）：
 *     ftl_sync_read_page_raw(pba, buf);
 *   擦块（20ms 超时，元数据配置）：
 *     ftl_sync_erase_block(pba);
 *
 * 共同契约：返回 FTL_ERROR 时命令**已释放**、buffer 内容未定义；调用者不应再
 * 自行 fc_command_release。共同的命令配置：单 plane、FC_CHAIN_END、LP 页、
 * CW 0..3（擦除为 0，无数据链）。
 *
 * 【成败判据】返回 FTL_OK ⇒ 页内容**可信**；判据是 FC_CW_STATUS_ERROR_MASK 全集
 * （NAND_FAIL / TIMEOUT / CRC_ERROR / UNC / MATA_ERROR / RAID_ERROR 任一置位即
 * FTL_ERROR），与 fc_command.h 的链级判据同口径。两个例外：ftl_sync_read_page_raw
 * 与 ftl_sync_read_column 刻意**不看**这些位（坏页的原始位与 spare 正是它们的
 * 返回物），成败由调用者按内容自判。
 */

typedef struct
{
    BYTE  crc;    /* FC_CRC_ENABLE / FC_CRC_DISABLE */
    BYTE  random; /* FC_RANDOM_ENABLE / FC_RANDOM_DISABLE */
    BYTE  ldpc;   /* FC_ECC_MODE_BCH / FC_ECC_MODE_LDPC_RAW */
    DWORD lba;    /* META 源：**页** LBA（建链时 << CW_WIDTH 成扇区 LBA）；元数据页传 0 */
} ftl_page_cfg_t;

/* 「本页 LBA 未知」哨兵：只传给 FTL_PAGE_CFG_DATA_ANY。
 * 页 LBA 的有效上界 = L2P_REGIONS_PER_INSTANCE × L2P_ENTRIES_PER_REGION（= 2^20），
 * 故 0xFFFFFFFF 不可能是一个真实页 LBA。 */
#define FTL_LBA_UNKNOWN (0xFFFFFFFFUL)

#define FTL_PAGE_CFG_META()    (&(const ftl_page_cfg_t){FC_CRC_ENABLE, FC_RANDOM_DISABLE, FC_ECC_MODE_BCH, 0U})
#define FTL_PAGE_CFG_DATA(lba) (&(const ftl_page_cfg_t){FC_CRC_ENABLE, FC_RANDOM_ENABLE, FC_ECC_MODE_BCH, (lba)})
/* 数据页但**页 LBA 未知**（Read Recovery 的邻近页辅助读）：CRC/加扰照常开，但命令的
 * META 填 FC_META_UNKNOWN ⇒ 跳过 FC 侧「页内 META 与本命令 LBA 比对」这一项，只保留
 * plane/cw 三位自洽校验（见 fc_data_path_meta_check）。
 * ⚠ 这类读**不要**用 FTL_PAGE_CFG_DATA(0) 代替：那是"本页 LBA 就是 0"，
 *   比对项会拿 0 去撞页内真实 META ⇒ 恒置 MATA_ERROR（若忽略错误位会看不出来）。 */
#define FTL_PAGE_CFG_DATA_ANY() (&(const ftl_page_cfg_t){FC_CRC_ENABLE, FC_RANDOM_ENABLE, FC_ECC_MODE_BCH, FTL_LBA_UNKNOWN})

/** @brief 写页：OP_NORMAL_PROGRAM_PAGE。data/length 为空时只发命令不搬数据。 */
ftl_status_e ftl_sync_write_page(DWORD pba, const void *data, DWORD length, const ftl_page_cfg_t *cfg);

/** @brief 读页：OP_NORMAL_READ_PAGE_WITH_DATA_OUT；数据在完成后拷入 buffer。 */
ftl_status_e ftl_sync_read_page(DWORD pba, void *buffer, DWORD length, const ftl_page_cfg_t *cfg);

/**
 * @brief 裸页读：LDPC_RAW，整页 4 个 CW 依次拷出（间隔 FC_BUFFER_POOL_CW_SIZE）。
 * ⚠ 刻意**不判定 NAND 失败**——BBT 坏页也要拿到原始位（上层按「非全 FF」判空）。
 * /
 */
ftl_status_e ftl_sync_read_page_raw(DWORD pba, void *buffer);

/**
 * @brief 判整页是否未写过：RAW 读后检查每个 CW 槽（META+CRC+USER 前缀 512B）是否全 FF。
 * 读命令没走通 ⇒ 一律返回 false（判“非空”，保守方向）。
 * raw_buf 需 >= PAGE_SIZE；判据与 full rebuild / BBT 探针 / L2P 日志区恢复共用
 * （C2：唯一实现）。整段检查而非只看 META：半编程页的 META 可能仍是全 FF。
 * /
 */
bool ftl_page_is_blank_raw(DWORD pba, void *raw_buf);

/** @brief 擦块（单 plane）：OP_ERASE_BLOCK，plane 取自 PBA，无数据链，20ms 超时。 */
ftl_status_e ftl_sync_erase_block(DWORD pba);

/**
 * @brief 擦块（多 plane）：OP_ERASE_BLOCK_MULTI_PLANE，擦**同一 block 号在两个 plane**
 * 上的两个物理块。只给跨 plane 的固定池（Journal：页地址按奇偶跨 plane）用；
 * 普通块擦除仍走单 plane 版本。PBA 的 plane 字段被忽略。
 */
ftl_status_e ftl_sync_erase_block_multi_plane(DWORD pba);

/** ── 异步页级原语（提交与完成解耦）──
 * @brief 与上面 ftl_sync_* 同一 prepare 模板（同样的建链规则、同样的配置、同样的搬数据），
 * 差别只有最后一步：**不提交**，把已建链、已装好数据的命令交回调用者。
 *
 *   ftl_async_*  →  调用者 ftl_op_submit(cmd, <功能 op ID>, ctx)
 *                 →  g_op_callbacks[<op ID>] 服务函数判完成状态 + fc_command_release
 *
 * ⚠ **数据在 prepare 阶段就已拷进 FC buffer 池**（ftl_cmd_write_data 是 memcpy，
 *   见 ftl_fc_interface.c），故本函数返回后调用者即可复用 / 覆写源缓冲——这正是
 *   Journal 刷页不需要 ping-pong 快照缓冲的原因。
 * ⚠ 调用者拿到 cmd 后**必须**提交或释放：既未提交也未 release 即泄漏命令槽位
 *   ⇒ fc_command_alloc 恒 NULL ⇒ 全系统 FTL_AGAIN。
 * ⚠ 失败返回时命令已自行回收（与 ftl_sync_* 同约定），out_cmd 不被写入。
 *
 * @return FTL_OK 命令已备好；FTL_AGAIN 命令池 / desc / buffer 池瞬态耗尽（可稍后重试）。
 */
ftl_status_e ftl_async_write_page(fc_command_t **out_cmd, DWORD pba, const void *data, DWORD length, const ftl_page_cfg_t *cfg);

/**
 * @brief 建链（无数据），**不提交**，命令经 out_cmd 交回。
 *
 * @param out_cmd 出参命令（非 NULL）
 * @param pba 目标块 PBA。
 *
 * @return FTL_OK / FTL_AGAIN（命令池空）/ FTL_ERROR（入参或地址非法 / 建链失败）。
 *
 * @note 实现要点：擦除慢（20ms 超时）是**同步版**的形参；异步版的超时由 FC 层自己的
 * 命令超时与 job watchdog 承担，本函数不设。
 */
ftl_status_e ftl_async_erase_block(fc_command_t **out_cmd, DWORD pba);

/** @brief 多 plane 擦块（异步）：同 ftl_async_erase_block，op = OP_ERASE_BLOCK_MULTI_PLANE。 */
ftl_status_e ftl_async_erase_block_multi_plane(fc_command_t **out_cmd, DWORD pba);

/* ── 元数据镜像写（多 plane 同页同数据）──
 *
 * 用 OP_PROGRAM_PAGE_MULTI_PLANE 把**同一份页数据**一次写入 PLANE_NUM 个 plane 的
 * 同一 block/page（元数据固定池的空间冗余；读侧任一副本有效即可）。
 *
 * 数据填装：按 CW 偏移把同一页复制到每个 plane 的 desc（`ftl_cmd_write_data_cw`）；
 * `length < PAGE_MAIN_SIZE` 时（如 BBT header 540 B）用 0xFF 补齐到一页。
 *
 * ⚠ 只给**固定元数据池**用；用户数据/日志块仍走单 plane/正常多 plane 数据路径。
 * ⚠ 这是 multi-plane program：两个 plane 的 page 必须都处于擦除态。
 */
ftl_status_e ftl_async_write_page_mirror(fc_command_t **out_cmd, DWORD pba, const void *data, DWORD length, const ftl_page_cfg_t *cfg);

/**
 * @brief 镜像写的同步版（建链 + 提交等待 + 判错）。
 */
ftl_status_e ftl_sync_write_page_mirror(DWORD pba, const void *data, DWORD length, const ftl_page_cfg_t *cfg);

/**
 * @brief 写页（CW 级，异步）：只把 page_buf 里 cw_bitmap 指定的 CW 编进目标页，**不提交**。
 *
 * 与 ftl_async_write_page 的差别：位图来自形参而非「整段」；数据按**页内 CW 偏移**
 * 散射取（`ftl_cmd_write_data_cw`），不是按链序紧凑打包。
 * GC/WL 搬移专用——只搬有效 CW 且必须保持原 CW 偏移。
 *
 * ⚠ 数据在本函数内已拷进 FC buffer 池（同 ftl_async_write_page）⇒ 返回后调用者即可
 *   复用 page_buf。
 * ⚠ cw_bitmap 须落在本命令 plane 段内（单 plane 命令的段由 pba 的 plane 决定，
 *   见 ftl_cmd_setup_ppa_pair_cw 的注释）；越段是编程错误 → ASSERT。
 *
 * @return FTL_OK 命令已备好（须提交或 release）；FTL_AGAIN 命令池空；
 *         FTL_ERROR 入参非法 / 地址非法 / 建链失败 / 搬数据失败。
 */
ftl_status_e ftl_async_write_page_cw(fc_command_t **out_cmd, DWORD pba, const BYTE *page_buf, DWORD cw_bitmap, const ftl_page_cfg_t *cfg);

/**
 * @brief 写 pair 页（CW 级，异步，显式 op）：双 plane 各自 PBA + plane-major 页镜像。
 *
 * 用于 cache program 批次：operation 传 OP_TWO_PLANE_PROGRAM_CACHE（中间页）或
 * OP_TWO_PLANE_PROGRAM_CACHE_END（末页）。数据在本函数内已拷进 FC buffer 池。
 *
 * ⚠ pair_buf 布局 = plane0 的 PAGE_MAIN_SIZE 字节 + plane1 的 PAGE_MAIN_SIZE 字节；
 *   cw_bitmap 是 flat 绝对位图（bit = plane*CW_NUM+cw）。两 plane 都在时传全 1。
 * ⚠ 两个 pba 必须同 ch/ce/lun；多 plane 命令的 desc 链必须覆盖每个 plane。
 *
 * @return FTL_OK / FTL_AGAIN（命令池空）/ FTL_ERROR（入参、地址或建链失败）。
 */
ftl_status_e ftl_async_write_pair_cmd(fc_command_t **out_cmd, const DWORD pba[PLANE_NUM], const BYTE *pair_buf, DWORD cw_bitmap, BYTE operation, const ftl_page_cfg_t *cfg);

/**
 * @brief 读页（异步）：OP_NORMAL_READ_PAGE_WITH_DATA_OUT，建链，**不提交**。
 *
 * 与 ftl_async_write_page 有一处**不对称**，是本函数存在独立文档块的理由：
 * 写的数据在 prepare 阶段就 memcpy 进 FC buffer 池（源缓冲随即可复用），**读不是**——
 * 数据留在命令的 desc 链上，只有服务函数在 release 之前调 ftl_cmd_read_data 才拿得到。
 * ⇒ 调用者指定的目标缓冲必须在**命令完成之前**保持有效，且拷出必须早于 release。
 *
 * 成败判据也搬到服务函数（同步版是 FC_CW_STATUS_ERROR_MASK 全集，见 ftl_sync_read_page）。
 *
 * @return FTL_OK 命令已备好（须提交或 release）；FTL_AGAIN 命令池空；FTL_ERROR 入参或地址非法。
 */
ftl_status_e ftl_async_read_page(fc_command_t **out_cmd, DWORD pba, const ftl_page_cfg_t *cfg);

/**
 * @brief SET_FEATURES（异步）：无地址、无数据链，把 mode 写进 feature_addr 指定的特性寄存器。
 *
 * ⚠ 本仓 Read Recovery **不使用**该入口：0x89 在本器件是保留地址、SLC 也无 retry table。
 *   保留本原语作为特性寄存器诊断/未来器件入口。
 * 与 ftl_sync_* 系列不同，本命令**不绑 PBA**：cmd->paa 恒 0——特性寄存器是 die 级、
 * 无 plane/page 语义（同 fc.c 中 OP_SET_FEATURES 分支的注释）。
 *
 * ⚠ ch/ce/lun 必须由调用者显式给：它们不是地址，而是**完成派发的实例路由键**
 *   （ftl_op_dispatch 按命令自身 ch/ce/lun 经 ftl_op_route_get 解析归属实例）。
 *   没有 PBA 可推 ⇒ 只能显式传。留 0 在多实例下会把本命令的完成派给实例 [0][0][0]。
 *
 * @return FTL_OK 命令已备好（须提交或 release）；FTL_AGAIN 命令池空。
 *         ⚠ out_cmd 由调用者传局部地址，不在此重复断言。
 */
ftl_status_e ftl_async_set_features(fc_command_t **out_cmd, BYTE ch, BYTE ce, BYTE lun, BYTE feature_addr, BYTE mode);

/**
 * @brief 按列读：OP_READ_COLUMN，无数据链，回读 pba 指定页**从列 column 起的 4 个字节**。
 *
 * 列地址**不来自 pba 的 cw 字段，而由 column 形参给**
 * ——cw 只有 2 位（列上限 1584），够不到 spare 区。出厂坏块标记在 **spare 首字节 = 列
 * 2048**（`PAGE_MAIN_SIZE`），只有本原语够得到。行地址仍取 pba 的 block/plane/page。
 *
 * 列地址经 FC 命令的 `param.bytes[0..1]`（小端）下传（见 fc_build_dispatch_request 的
 * OP_READ_COLUMN 分支），结果经 CQ 的 `param.bytes[0..3]` 回。
 *
 * 与 ftl_sync_read_page_raw 同契约：**刻意不判定 NAND 失败**——坏块的 spare 也要拿到，
 * 故命令完成即返回 FTL_OK，成败由调用者按 out[] 内容判。out 需 4 字节。
 * column + 4 须落在页内：唯一调用者 bbt.c 传 PAGE_MAIN_SIZE（+4 ≤ PAGE_SIZE），
 * 本函数不重复校验。
 * /
 */
ftl_status_e ftl_sync_read_column(DWORD pba, WORD column, BYTE out[4]);

#endif /* FTL_FC_INTERFACE_H */

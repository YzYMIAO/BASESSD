#ifndef FTL_FC_INTERFACE_H
#define FTL_FC_INTERFACE_H

/*
 * FTL-FC Interface 层
 * ===================
 *
 * 【desc 链组装】
 *   ① ftl_cmd_configure_descriptors(cmd, cw_bitmap) — 按形参 cw_bitmap 建整条 desc 链
 *   ② ftl_cmd_setup_ppa() — 单一 PBA → CH/CE/PAA + 整段位图 + 建链
 *   ③ ftl_cmd_setup_ppa_pair_cw() — 每 plane 各自的 PBA + 显式位图 → 建链（Policy B）
 *   ④ pba_to_cmd() — nand_pba_t 直填 + 整段位图 + 建链（遍历路径 / 同步页原语）
 *      ⑩ ftl_cmd_pba() — FC 命令 → PBA（④ 的逆，供 FTL 模块回读自己下发的地址）
 *
 *   v23：①③ 共用同一套位图规则（不再"每 plane 连续 0..config.cw"）。config.cw 自此
 *   退出建链（仍被 FC 的 RAID stripe 读取，见 fc_command.h 的 config.cw 注释）。
 *   v27：位图是**字面位图**（bit 置 1 = 要访问该 CW，0 = 空集），v23~v26 的
 *   「0 = 本命令 plane 段内全部 CW」重载已取消。故 ②③④ 三个「整段访问」入口各自
 *   显式算 fc_command_plane_span_cw_bitmap(cmd)；只有 ① 接受调用者给的部分位图
 *   （0 会被 ftl_cmd_resolve_cw_bitmap 的 ASSERT 拦下）。规则唯一定义在
 *   ftl_cmd_resolve_cw_bitmap()（static，见 ftl_fc_interface.c）。
 *   v28：位图**不再存于命令结构体**（`fc_command_t.param.cw_bitmap` 已删）——它是建链的
 *   输入而非命令的属性，改作 ① 的**形参**。②③④ 算好后直接传参。
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
 *      ftl_sync_erase_block — 原先 9 个 FTL 模块各自内联一份 static 版本
 *      （共 18 份、逐字节相同，只差 crc/random/ldpc/lba 四个字段），
 *      此处收敛为 4 个公共函数 + ftl_page_cfg_t 配置结构。
 *
 * 【异步页级原语】（阶段 2 起）
 *   ⑩ ftl_async_write_page / ftl_async_write_page_cw / ftl_async_erase_block /
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
 * 为什么这些在本层（v13）：FC 定位为**硬件 FCC 的软件替身**——只提供槽位/描述符/
 * 队列/数据路径，不含策略。而「访问哪些 CW」（掩码枚举）与「数据怎么搬」（拷贝）
 * 在行业里都是**固件侧**的事（硬件 FCC 既不提供「按掩码建链」寄存器，也不提供
 * 「数据拷贝」寄存器——描述符链是固件填的、数据是固件 DMA 填的），故落在本层。
 */

#include "common/assertion.h"
#include "fc/fc_interface.h"
#include "ftl/ftl_config.h" /* nand_pba_t, pba_to_paa */
#include "ftl/ftl_type.h"
#include "nand/nand_config.h"
#include "nand/onfi_1_0.h"

#include <stdint.h>

/* ── desc 链组装共用小工具 ──
 *   plane 段规则用 fc_command.h 的 fc_command_plane_span（唯一一份定义，
 *   FC 的越段校验与 FTL 的掩码展开共用）。 */

/*
 * ftl_cmd_id - 命令指针 → FC 命令池 ID。
 *
 * fc_command_t 里**没有** command_id 字段：ID 由「指针 − command_pool 基址」导出
 * （见 fc_command.h）。本层调用者手里的 cmd 恒来自 fc_command_alloc()，故 ID 必然有效；
 * 池外指针（栈上的 fc_command_t）得到 FC_COMMAND_ID_INVALID——那是编程错误，
 * 调用方（pending_add / sync_wait）会因查不到而失败，不会静默串命令。
 */
static inline uint16_t ftl_cmd_id(const fc_command_t *cmd)
{
    fc_command_id_t command_id = FC_COMMAND_ID_INVALID;

    (void)fc_command_to_id(cmd, &command_id);
    return (uint16_t)command_id;
}

/*
 * ftl_cmd_needs_desc_chain - 该操作是否需要 desc 链（即是否携带数据）
 *
 * NO_DATA 操作（ERASE / RESET / LOCK 等）无数据负载；SET/GET_FEATURES 用内联
 * param.bytes + feature_addr。两者都不建链（desc_id 保持 INVALID）。
 * 与 fc_command_validate 的接受条件对齐：只有 has_data 且非 FEATURES 的操作
 * 才要求 desc_id 有效。
 */
static inline bool ftl_cmd_needs_desc_chain(const fc_command_t *cmd)
{
    const uint8_t op = (uint8_t)cmd->config.operation;

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

/* plane 数 → config.multi_plane 字段。
 * 纯打包：nplanes 的值域由调用点紧邻的 ftl_plane_*_op 断言（同一个 nplanes）。 */
static inline uint8_t ftl_plane_multi_field(uint8_t nplanes)
{
    return (uint8_t)((nplanes > 1U) ? nplanes : 0U);
}

/* plane 数 → 读 operation 码。界取 PLANE_NUM，与 fc_command_plane_span 的上界同源。 */
static inline uint8_t ftl_plane_read_op(uint8_t nplanes)
{
    ASSERT((nplanes >= 1U) && (nplanes <= PLANE_NUM));
    return (uint8_t)((nplanes > 1U) ? OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT : OP_NORMAL_READ_PAGE_WITH_DATA_OUT);
}

/* plane 数 → 写 operation 码。界同上。 */
static inline uint8_t ftl_plane_write_op(uint8_t nplanes)
{
    ASSERT((nplanes >= 1U) && (nplanes <= PLANE_NUM));
    return (uint8_t)((nplanes > 1U) ? OP_PROGRAM_PAGE_MULTI_PLANE : OP_NORMAL_PROGRAM_PAGE);
}

/* ── desc 链组装 ── */

/*
 * ftl_cmd_configure_descriptors - 按形参 cw_bitmap 组装整条 desc 链
 *
 * cw_bitmap 是 flat 绝对编号（bit n = plane*CW_NUM + cw）；**字面位图**（v27）——
 * 置 1 的位就是本命令要访问的 CW，**0 = 空集**（不是"全部"）。v23~v26 的
 * 「0 = 本命令 plane 段内的全部 CW」重载已取消。置位的每个 CW 逐条调
 * fc_command_configure_descriptor()，故链序 = plane-major、plane 内 CW 递增。
 * 想只读离散 CW（例如 plane1 的 CW0/CW2）：传 (1U<<4)|(1U<<6)。
 * 想整段访问：传 fc_command_plane_span_cw_bitmap(cmd)
 *   —— 或者干脆用 ftl_cmd_setup_ppa()，它替你算。
 *
 * v28：位图是本函数的**形参**，不再是命令结构体的字段——它是"这次建链要建哪些 CW"
 * 的输入，用完即弃，没有留在命令里被回读的理由（读者只有本函数）。
 *
 * 行地址：所有 plane 共用 cmd->paa 的行地址（同页语义）。**两 plane 不同块
 * （Policy B）请用 ftl_cmd_setup_ppa_pair_cw()**，本函数不适用。
 *
 * @param cmd       须已设 config.operation / multi_plane / paa（config.cw 已不读）
 * @param cw_bitmap flat 字面位图；无数据 op（ERASE / RESET / FEATURES）不读它
 * @return FTL_OK / FTL_ERROR（池空；位图 0 或越出 plane 段是编程错误 → ASSERT）
 */
ftl_status_t ftl_cmd_configure_descriptors(fc_command_t *cmd, uint32_t cw_bitmap);

/* ── PBA helper（**自己算整段位图**并传给建链入口） ── */

/*
 * ftl_cmd_setup_ppa - 单一 PBA → 拓扑 + PAA + 整段 desc 链
 *
 * 把 PBA 拆成拓扑（ch/ce/lun 填 cmd->config）与行地址（pba_to_paa 填 cmd->paa，v47
 * 在 FTL 侧一次转出，FC 层不再见 PBA），随后按「本命令 plane 段内全部 CW」建整条
 * desc 链（自己算 fc_command_plane_span_cw_bitmap 作实参，须在 paa 写好之后算）。
 *
 * 隐含「两 plane 同 block」——两 plane 独立分块（Policy B）请用 ftl_cmd_setup_ppa_pair_cw()。
 *
 * @param cmd  FC 命令（须已设 config.operation / multi_plane）
 * @param pba  目标物理块地址（必须合法：fc.c 拿 ch/ce/lun 当数组下标 → 越界即栈越界写）
 * @return FTL_OK / FTL_ERROR（建链失败）
 */
ftl_status_t ftl_cmd_setup_ppa(fc_command_t *cmd, nand_pba_t pba);

/*
 * ftl_cmd_setup_ppa_pair_cw - 按**每 plane 各自**的 PBA + 显式位图建 desc 链（Policy B）
 *
 * 与 ftl_cmd_setup_ppa 的区别：后者把单一 PBA 转成 PAA 填进 command->paa 后展开到各 plane，
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
ftl_status_t ftl_cmd_setup_ppa_pair_cw(fc_command_t *cmd, const nand_pba_t pba[PLANE_NUM], uint32_t cw_bitmap);

/* ── 地址直填：nand_pba_t → FC 命令（PBA→PAA 的唯一关卡之一）──
 *
 * FTL 侧的地址**一律**是 PBA（L2P / Journal / BST / Metadata Directory 存的就是它）。
 * 只有在这里——建 FC 命令的最后一刻——才做 PBA → PAA 的字段重排：
 *   ch/ce/lun           → cmd->config 位域
 *   block/plane/page/cw → cmd->paa（v47：pba_to_paa 在 FTL 侧一次转出）
 *
 * 与 ftl_cmd_setup_ppa(cmd, pba) 语义等价（都是「整段访问」）；本入口是同步页原语与
 * 遍历路径（GC / 元数据扫描 / Checkpoint）的公共底座。
 * 地址填好后**必须建 desc 链**——否则命令无 buffer，数据无从搬运，
 * fc_submit 也会被 fc_command_validate 以 desc_id == INVALID 拒绝。
 * 无数据操作（ERASE / RESET / META / COLUMN 等）由 ftl_cmd_configure_descriptors
 * 短路为 desc_id = INVALID。
 *
 * v27/v28：本入口把 fc_command_plane_span_cw_bitmap(cmd)（「本命令 plane 段内全部 CW」）
 *      算出来直接作实参交给建链入口——字面位图没有隐式展开，遍历路径（GC / 元数据 /
 *      checkpoint / sync 页原语）一律整段访问，故在这里一次算好，调用者不必管。
 */
static inline ftl_status_t pba_to_cmd(fc_command_t *cmd, nand_pba_t pba)
{
    /* cmd 由调用者从 fc_command_alloc() 取且已判空（判据 2：层内模块头 → 前置条件）。 */
    ASSERT(cmd != NULL);

    /* 边界不变量：同 ftl_cmd_setup_ppa —— 进 FC 的 ch/ce/lun 被 fc_poll_fill_queue 当
     * suspended[][][] 下标直接用，越界即栈越界写。本入口**不经过** ftl_cmd_setup_ppa，
     * 校验必须自带；否则 fc.c 那句「PBA 已过 pba_is_valid() 校验」在遍历路径
     * （同步页原语 / GC / 元数据扫描 / Checkpoint）上仍是假命题。 */
    ASSERT(pba_is_valid(pba));

    cmd->config.ch  = (DWORD)pba_to_ch(pba);
    cmd->config.ce  = (DWORD)pba_to_ce(pba);
    cmd->config.lun = (DWORD)pba_to_lun(pba);
    /* v47：PBA 在 FTL 侧就地转 PAA——FC 层只见 PAA，不再有 pba_to_paa 调用点。
     * ⚠ 必须在建链之前转（下面 fc_command_plane_span_cw_bitmap 读 cmd->paa）。 */
    cmd->paa        = pba_to_paa(pba);
    /* v28：位图不再存进命令结构体，直接算出来当实参传。
     * 须在 paa 写好之后算（fc_command_plane_span_cw_bitmap 读 paa 与 multi_plane）。 */
    return ftl_cmd_configure_descriptors(cmd, fc_command_plane_span_cw_bitmap(cmd));
}

/*
 * ftl_cmd_pba - FC 命令 → PBA（pba_to_cmd 的逆）
 *
 * FTL 模块要回读自己下发的命令地址时用它，**不必**直接摸 cmd->paa——PAA 是 FC/器件域
 * 的表示。ch/ce 取 cmd->config 位域，其余字段由 paa_to_pba() 反重排。
 *
 * ⚠ 本函数与 pba_to_cmd() 是 FTL 层仅有的两处 PBA↔PAA 转换点。
 */
static inline nand_pba_t ftl_cmd_pba(const fc_command_t *cmd)
{
    return paa_to_pba((uint32_t)cmd->config.ch, (uint32_t)cmd->config.ce, cmd->paa);
}

/* ── 数据搬运（v13 由 FC 上移）── */

/*
 * ftl_cmd_write_data - 把外部数据按 CW 顺序写入 desc 链上各 CW 的 USER 区
 *
 * v13 由 FC 上移：硬件 FCC 不提供「按 CW 拷贝」寄存器，数据由固件在 desc 链上逐 CW 填。
 * 遍历界走 desc 链（链长度是 CW 数的唯一真值）；数据耗尽后剩余 CW 的 USER 区填 0xFF
 * dummy（不把上一轮残值写进 NAND）。
 *
 * @param cmd     已建链的命令（desc_id 有效）
 * @param data    源数据
 * @param length  字节数
 * @return FTL_OK / FTL_ERROR（链上 desc 无 buffer）
 */
ftl_status_t ftl_cmd_write_data(fc_command_t *cmd, const void *data, uint32_t length);

/*
 * ftl_cmd_write_data_cw - CW 级数据填装（按页内 CW 偏移从整页镜像散射取数）
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
ftl_status_t ftl_cmd_write_data_cw(fc_command_t *cmd, const uint8_t *page_buf, uint32_t cw_bitmap);

/*
 * ftl_cmd_read_data - 从 desc 链各 CW 的 USER 区按链序拷出到外部缓冲
 *
 * 与 ftl_cmd_write_data 对称（v13 由 FC 上移）；遍历界走 desc 链，数据够即停。
 *
 * @param cmd     已完成的命令（desc_id 有效）
 * @param buffer  输出缓冲
 * @param length  字节数
 * @return FTL_OK / FTL_ERROR（链上 desc 无 buffer）
 */
ftl_status_t ftl_cmd_read_data(fc_command_t *cmd, void *buffer, uint32_t length);

/* ── 内部同步 NAND 操作（仅保留 sync_wait）── */

/*
 * ftl_sync_submit_wait - 提交命令并阻塞等待其完成（同步提交的唯一入口）
 *
 * 内部就是 ftl_op 机制的一个客户端：
 *   ① ftl_op_submit(cmd, FTL_OP_SYNC_WAIT, &waiter) —— 打 tag、登记栈上 waiter、提交；
 *   ② 自旋 fc_service() + completion_table_drain() 直到 waiter.done；
 *   ③ 超时则 completion_table_remove 后返回 FTL_ERROR。
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
ftl_status_t ftl_sync_submit_wait(fc_command_t *cmd, fc_command_status_t *status_out, uint32_t timeout_us);

/* ── 页级同步原语（18 份内联 → 4 份公共）──
 *
 * 9 个 FTL 模块原先各自内联一份 static sync_write_page / sync_read_page /
 * sync_read_page_raw / sync_erase_block，共 18 份、逐字节相同，只差四个字段。
 * 收敛后由配置结构承载差异；配置是编译期常量，故用复合字面量宏调用：
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
    uint8_t crc;    /* FC_CRC_ENABLE / FC_CRC_DISABLE */
    uint8_t random; /* FC_RANDOM_ENABLE / FC_RANDOM_DISABLE */
    uint8_t ldpc;   /* FC_ECC_MODE_BCH / FC_ECC_MODE_LDPC_RAW */
    lba_t   lba;    /* META 源：**页** LBA（建链时 << CW_WIDTH 成扇区 LBA）；元数据页传 0 */
} ftl_page_cfg_t;

/* 「本页 LBA 未知」哨兵：只传给 FTL_PAGE_CFG_DATA_ANY。
 * 页 LBA 的有效上界 = L2P_REGIONS_PER_INSTANCE × L2P_ENTRIES_PER_REGION（= 2^20），
 * 故 0xFFFFFFFF 不可能是一个真实页 LBA。 */
#define FTL_LBA_UNKNOWN (0xFFFFFFFFUL)

#define FTL_PAGE_CFG_META()    (&(const ftl_page_cfg_t){FC_CRC_ENABLE, FC_RANDOM_DISABLE, FC_ECC_MODE_BCH, 0U})
#define FTL_PAGE_CFG_DATA(lba) (&(const ftl_page_cfg_t){FC_CRC_ENABLE, FC_RANDOM_ENABLE, FC_ECC_MODE_BCH, (lba_t)(lba)})
/* 数据页但**页 LBA 未知**（Read Recovery 的邻近页辅助读）：CRC/加扰照常开，但命令的
 * META 填 FC_META_UNKNOWN ⇒ 跳过 FC 侧「页内 META 与本命令 LBA 比对」这一项，只保留
 * plane/cw 三位自洽校验（见 fc_data_path_meta_check）。
 * ⚠ 这类读**不要**用 FTL_PAGE_CFG_DATA(0) 代替：那是"本页 LBA 就是 0"，
 *   比对项会拿 0 去撞页内真实 META ⇒ 恒置 MATA_ERROR（旧实现因忽略错误位而看不出来）。 */
#define FTL_PAGE_CFG_DATA_ANY() (&(const ftl_page_cfg_t){FC_CRC_ENABLE, FC_RANDOM_ENABLE, FC_ECC_MODE_BCH, (lba_t)FTL_LBA_UNKNOWN})

/* 写页：OP_NORMAL_PROGRAM_PAGE。data/length 为空时只发命令不搬数据。 */
ftl_status_t ftl_sync_write_page(nand_pba_t pba, const void *data, uint32_t length, const ftl_page_cfg_t *cfg);

/*
 * 写页（CW 级）：只把 page_buf 里 cw_bitmap 指定的 CW 编进目标页（GC/WL 搬移专用）。
 *
 * page_buf 是**整页镜像**（CW c 的数据在 page_buf + c×512）；未置位的 CW 不建链、
 * 不编程，读回 0xFF。cfg->lba 必须是**本页**的页 LBA（CW 级写不改变 CW 偏移，
 * 页内 META 的 LBA 由 CW 号决定）。
 */
ftl_status_t ftl_sync_write_page_cw(nand_pba_t pba, const uint8_t *page_buf, uint32_t cw_bitmap, const ftl_page_cfg_t *cfg);

/* 读页：OP_NORMAL_READ_PAGE_WITH_DATA_OUT；数据在完成后拷入 buffer。 */
ftl_status_t ftl_sync_read_page(nand_pba_t pba, void *buffer, uint32_t length, const ftl_page_cfg_t *cfg);

/*
 * 裸页读：LDPC_RAW，整页 4 个 CW 依次拷出（间隔 FC_BUFFER_POOL_CW_SIZE）。
 * ⚠ 刻意**不判定 NAND 失败**——BBT 坏页也要拿到原始位（上层按「非全 FF」判空）。
 */
ftl_status_t ftl_sync_read_page_raw(nand_pba_t pba, void *buffer);

/* 擦块：OP_ERASE_BLOCK，无数据链，20ms 超时。 */
ftl_status_t ftl_sync_erase_block(nand_pba_t pba);

/* ── 异步页级原语（阶段 2 起：提交与完成解耦）──
 *
 * 与上面 ftl_sync_* 同一 prepare 模板（同样的建链规则、同样的配置、同样的搬数据），
 * 差别只有最后一步：**不提交**，把已建链、已装好数据的命令交回调用者。
 *
 *   ftl_async_*  →  调用者 ftl_op_submit(cmd, <功能 op ID>, ctx)
 *                 →  g_op_service[<op ID>] 服务函数判完成状态 + fc_command_release
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
ftl_status_t ftl_async_write_page(fc_command_t **out_cmd, nand_pba_t pba, const void *data, uint32_t length, const ftl_page_cfg_t *cfg);
ftl_status_t ftl_async_erase_block(fc_command_t **out_cmd, nand_pba_t pba);

/*
 * 写页（CW 级，异步）：只把 page_buf 里 cw_bitmap 指定的 CW 编进目标页，**不提交**。
 *
 * 与 ftl_async_write_page 的差别同 ftl_sync_write_page_cw 与 ftl_sync_write_page 的差别：
 * 位图来自形参而非「整段」；数据按**页内 CW 偏移**散射取（`ftl_cmd_write_data_cw`），
 * 不是按链序紧凑打包。GC/WL 搬移专用——只搬有效 CW 且必须保持原 CW 偏移。
 *
 * ⚠ 数据在本函数内已拷进 FC buffer 池（同 ftl_async_write_page）⇒ 返回后调用者即可
 *   复用 page_buf。
 * ⚠ cw_bitmap 须落在本命令 plane 段内（单 plane 命令的段由 pba 的 plane 决定，
 *   见 ftl_cmd_setup_ppa_pair_cw 的注释）；越段是编程错误 → ASSERT。
 *
 * @return FTL_OK 命令已备好（须提交或 release）；FTL_AGAIN 命令池空；
 *         FTL_ERROR 入参非法 / 地址非法 / 建链失败 / 搬数据失败。
 */
ftl_status_t ftl_async_write_page_cw(fc_command_t **out_cmd, nand_pba_t pba, const uint8_t *page_buf, uint32_t cw_bitmap, const ftl_page_cfg_t *cfg);

/*
 * 读页（异步）：OP_NORMAL_READ_PAGE_WITH_DATA_OUT，建链，**不提交**。
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
ftl_status_t ftl_async_read_page(fc_command_t **out_cmd, nand_pba_t pba, const ftl_page_cfg_t *cfg);

/*
 * SET_FEATURES（异步）：无地址、无数据链，把 mode 写进 feature_addr 指定的特性寄存器。
 *
 * 由 Read Recovery 用于切换读重试电压模式（RR_FEATURE_READ_RETRY；mode 0 = 正常模式）。
 * 与 ftl_sync_* 系列不同，本命令**不绑 PBA**：cmd->paa 恒 0——特性寄存器是 die 级、
 * 无 plane/page 语义（同 fc.c 中 OP_SET_FEATURES 分支的注释）。
 *
 * ⚠ ch/ce/lun 必须由调用者显式给：它们不是地址，而是**完成派发的实例路由键**
 *   （ftl_op_dispatch 按命令自身 ch/ce/lun 经 completion_table_route 解析归属实例）。
 *   没有 PBA 可推 ⇒ 只能显式传。留 0 在多实例下会把本命令的完成派给实例 [0][0][0]。
 *
 * @return FTL_OK 命令已备好（须提交或 release）；FTL_AGAIN 命令池空。
 *         ⚠ out_cmd 为**前置条件**（判据 2），非法即 ASSERT。
 */
ftl_status_t ftl_async_set_features(fc_command_t **out_cmd, uint8_t ch, uint8_t ce, uint8_t lun, uint8_t feature_addr, uint8_t mode);

/*
 * 按列读：OP_READ_COLUMN，无数据链，回读 pba 指定页**从列 column 起的 4 个字节**。
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
 * column + 4 越出页尾是编程错误（ASSERT）。
 */
ftl_status_t ftl_sync_read_column(nand_pba_t pba, uint16_t column, uint8_t out[4]);

#endif /* FTL_FC_INTERFACE_H */

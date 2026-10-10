#ifndef FTL_FC_INTERFACE_H
#define FTL_FC_INTERFACE_H

/*
 * FTL-FC Interface 层
 * ===================
 *
 * 【desc 链组装】
 *   ① ftl_cmd_configure_descriptors(cmd, cw_bitmap) — 按形参 cw_bitmap 建整条 desc 链
 *   ② ftl_cmd_setup_ppa() — 单一 PBA → CH/CE/PAA + 整段位图 + 建链
 *   ③ ftl_cmd_setup_ppa_pair() — 每 plane 各自的 PBA → 整段位图 + 建链（Policy B）
 *   ④ addr_to_cmd() — nand_addr_t 直填 + 整段位图 + 建链（遍历路径，省打包拆包）
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
 *   ⑦ ftl_internal_sync_wait() — 等待指定 command_id 完成
 *
 * 【页级同步原语】
 *   ⑧ ftl_sync_write_page / ftl_sync_read_page / ftl_sync_read_page_raw /
 *      ftl_sync_erase_block — 原先 9 个 FTL 模块各自内联一份 static 版本
 *      （共 18 份、逐字节相同，只差 crc/random/module/lba 四个字段），
 *      此处收敛为 4 个公共函数 + ftl_page_cfg_t 配置结构。
 *
 * 【命令身份】
 *   ⑨ ftl_cmd_id() — 命令指针 → FC 命令池 ID（由指针派生，见 fc_command.h）。
 *
 * 为什么这些在本层（v13）：FC 定位为**硬件 FCC 的软件替身**——只提供槽位/描述符/
 * 队列/数据路径，不含策略。而「访问哪些 CW」（掩码枚举）与「数据怎么搬」（拷贝）
 * 在行业里都是**固件侧**的事（硬件 FCC 既不提供「按掩码建链」寄存器，也不提供
 * 「数据拷贝」寄存器——描述符链是固件填的、数据是固件 DMA 填的），故落在本层。
 */

#include "fc/fc_interface.h"
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
static inline uint8_t ftl_cmd_needs_desc_chain(const fc_command_t *cmd)
{
    const uint8_t op = (uint8_t)cmd->config.operation;

    return (uint8_t)((nand_operation_has_data(op) != 0U) && (op != OP_SET_FEATURES) && (op != OP_GET_FEATURES));
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
 *   —— 或者干脆用 ftl_cmd_setup_ppa()/ftl_cmd_setup_ppa_pair()，它们替你算。
 *
 * v28：位图是本函数的**形参**，不再是命令结构体的字段——它是"这次建链要建哪些 CW"
 * 的输入，用完即弃，没有留在命令里被回读的理由（读者只有本函数）。
 *
 * 行地址：所有 plane 共用 cmd->paa 的行地址（同页语义）。**两 plane 不同块
 * （Policy B）请用 ftl_cmd_setup_ppa_pair()**，本函数不适用。
 *
 * @param cmd       须已设 config.operation / multi_plane / paa（config.cw 已不读）
 * @param cw_bitmap flat 字面位图；无数据 op（ERASE / RESET / FEATURES）不读它
 * @return FTL_OK / FTL_ERROR（池空；位图 0 或越出 plane 段是编程错误 → ASSERT）
 */
ftl_status_t ftl_cmd_configure_descriptors(fc_command_t *cmd, uint32_t cw_bitmap);

/* ── PBA helper（两者都**自己算整段位图**并传给建链入口） ── */

ftl_status_t ftl_cmd_setup_ppa(fc_command_t *cmd, nand_pba_t pba);

/*
 * ftl_cmd_setup_ppa_pair - 按**每 plane 各自**的 PBA 建 desc 链（Policy B）
 *
 * 与 ftl_cmd_setup_ppa 的区别：后者把单一 PBA 转成 PAA 填进 command->paa 后展开到各 plane，
 * 隐含「两 plane 同 block」。Policy B 下两 plane 独立分配物理块，故本函数逐条调
 * fc_command_configure_descriptor(cmd, plane, cw, pba_to_paa(pba[i]))，把每个 PBA 自己的
 * 行地址（转成 PAA）写进对应 plane 的 desc。
 *
 * ⚠ **plane 号取自每个 PBA 自己的 plane 位，不是数组下标**：读路径的 pba[]
 *   是按 LBA 查出来的，plane 取决于当初怎么写进去（例如 `wr 0 2` 之后
 *   lba 1 映射到 plane 1），下标与 plane 号并不总相等。
 *
 * 遍历范围：pba[0..plane_count-1]（plane 段由 fc_command_plane_span 定）；单 plane 命令
 * 只取 pba[0]（其 plane 位决定落哪个 plane 段）。
 *
 * v27/v28：本入口语义是「整段访问」——它把 fc_command_plane_span_cw_bitmap(cmd)
 * （在 cmd->paa 写好之后算）作为实参交给建链入口，**调用者没有位图可设**。
 * 要访问 CW 子集请改用 ftl_cmd_configure_descriptors()。
 *
 * @param cmd  FC 命令（须已设 config.operation / multi_plane / meta 等）
 * @param pba  [PLANE_NUM] 每 plane 的 PBA（ch/ce/lun 取 pba[0]）
 * @return FTL_OK / FTL_ERROR（池空；位图越出 plane 段是编程错误 → ASSERT；
 *         调用者负责 fc_command_release 回收半成品链）
 */
ftl_status_t ftl_cmd_setup_ppa_pair(fc_command_t *cmd, const nand_pba_t pba[PLANE_NUM]);

/* ── 地址直填：nand_addr_t → FC 命令 ──
 *
 * 遍历路径（GC 搬移、元数据扫描、Checkpoint）不再打包 → 拆包，
 * 直接用展开地址填入 FC 命令：
 *   ch/ce/lun  → cmd->config 位域
 *   block/plane/page/cw → paa（v47：pba_from_addr 后立刻 pba_to_paa 转出）
 *
 * 与 ftl_cmd_setup_ppa(cmd, pba_from_addr(addr)) 等价（省去打包拆包）：
 * 地址填好后**必须建 desc 链**——否则命令无 buffer，数据无从搬运，
 * fc_submit 也会被 fc_command_validate 以 desc_id == INVALID 拒绝。
 * 无数据操作（ERASE / RESET 等）由 ftl_cmd_configure_descriptors 短路为 desc_id = INVALID。
 *
 * v27/v28：本入口把 fc_command_plane_span_cw_bitmap(cmd)（「本命令 plane 段内全部 CW」）
 *      算出来直接作实参交给建链入口——字面位图没有隐式展开，遍历路径（GC / 元数据 /
 *      checkpoint / sync 页原语）一律整段访问，故在这里一次算好，调用者不必管。
 */
static inline ftl_status_t addr_to_cmd(fc_command_t *cmd, const nand_addr_t *addr)
{
    if ((cmd == NULL) || (addr == NULL))
    {
        return FTL_ERROR;
    }

    cmd->config.ch  = (DWORD)addr->ch;
    cmd->config.ce  = (DWORD)addr->ce;
    cmd->config.lun = (DWORD)addr->lun;
    /* v47：PBA 在 FTL 侧就地转 PAA——FC 层只见 PAA，不再有 pba_to_paa 调用点。
     * ⚠ 必须在建链之前转（下面 fc_command_plane_span_cw_bitmap 读 cmd->paa）。 */
    cmd->paa        = pba_to_paa(pba_from_addr(addr));
    /* v28：位图不再存进命令结构体，直接算出来当实参传。
     * 须在 paa 写好之后算（fc_command_plane_span_cw_bitmap 读 paa 与 multi_plane）。 */
    return ftl_cmd_configure_descriptors(cmd, fc_command_plane_span_cw_bitmap(cmd));
}

/* ── 数据搬运（v13 由 FC 上移）── */

ftl_status_t ftl_cmd_write_data(fc_command_t *cmd, const void *data, uint32_t length);
ftl_status_t ftl_cmd_read_data(fc_command_t *cmd, void *buffer, uint32_t length);

/* ── 内部同步 NAND 操作（仅保留 sync_wait）── */

/*
 * ftl_internal_sync_wait - 同步等待指定 command_id 完成
 *
 * 循环调用 fc_service() + completion_table_drain() 直到指定命令完成。
 * 最多等待 timeout_us 微秒（0 = 默认 10ms）。
 *
 * @param cmd_id      要等待的 command_id
 * @param status_out  输出：completion status
 * @param timeout_us  超时微秒（0=使用默认）
 * @return FTL_OK 完成（status_out 有效）/ FTL_ERROR 超时或路由表满
 */
ftl_status_t ftl_internal_sync_wait(uint16_t cmd_id, fc_command_status_t *status_out, uint32_t timeout_us);

/* ── 页级同步原语（18 份内联 → 4 份公共）──
 *
 * 9 个 FTL 模块原先各自内联一份 static sync_write_page / sync_read_page /
 * sync_read_page_raw / sync_erase_block，共 18 份、逐字节相同，只差四个字段。
 * 收敛后由配置结构承载差异；配置是编译期常量，故用复合字面量宏调用：
 *
 *   元数据页（BBT / checkpoint / journal / L2P / metadata / 擦除）：
 *     ftl_sync_write_page(addr, buf, len, FTL_PAGE_CFG_META(FC_COMMAND_MODULE_BBT));
 *   用户数据页（GC / WL 搬移、Read Recovery 重读）：
 *     ftl_sync_write_page(addr, buf, len, FTL_PAGE_CFG_DATA(FC_COMMAND_MODULE_GC, lba));
 *   裸页（LDPC_RAW，忽略 NAND 成败）：
 *     ftl_sync_read_page_raw(addr, buf, FC_COMMAND_MODULE_BBT);
 *   擦块（20ms 超时，元数据配置）：
 *     ftl_sync_erase_block(addr);
 *
 * 共同契约：返回 FTL_ERROR 时命令**已释放**、buffer 内容未定义；调用者不应再
 * 自行 fc_command_release。共同的命令配置：单 plane、FC_CHAIN_END、LP 页、
 * CW 0..3（擦除为 0，无数据链）。
 */

typedef struct
{
    uint8_t crc;    /* FC_CRC_ENABLE / FC_CRC_DISABLE */
    uint8_t random; /* FC_RANDOM_ENABLE / FC_RANDOM_DISABLE */
    uint8_t ldpc;   /* FC_ECC_MODE_BCH / FC_ECC_MODE_LDPC_RAW */
    uint8_t module; /* FC_COMMAND_MODULE_*；同步路径下不做路由，仅诊断标签 */
    lba_t   lba;    /* META 源：**页** LBA（建链时 << CW_WIDTH 成扇区 LBA）；元数据页传 0 */
} ftl_page_cfg_t;

#define FTL_PAGE_CFG_META(module)      (&(const ftl_page_cfg_t){FC_CRC_DISABLE, FC_RANDOM_DISABLE, FC_ECC_MODE_BCH, (uint8_t)(module), 0U})
#define FTL_PAGE_CFG_DATA(module, lba) (&(const ftl_page_cfg_t){FC_CRC_ENABLE, FC_RANDOM_ENABLE, FC_ECC_MODE_BCH, (uint8_t)(module), (lba_t)(lba)})

/* 写页：OP_NORMAL_PROGRAM_PAGE。data/length 为空时只发命令不搬数据。 */
ftl_status_t ftl_sync_write_page(nand_addr_t addr, const void *data, uint32_t length, const ftl_page_cfg_t *cfg);

/* 读页：OP_NORMAL_READ_PAGE_WITH_DATA_OUT；数据在完成后拷入 buffer。 */
ftl_status_t ftl_sync_read_page(nand_addr_t addr, void *buffer, uint32_t length, const ftl_page_cfg_t *cfg);

/*
 * 裸页读：LDPC_RAW，整页 4 个 CW 依次拷出（间隔 FC_BUFFER_POOL_CW_SIZE）。
 * ⚠ 刻意**不判定 NAND 失败**——BBT 坏页也要拿到原始位（上层按「非全 FF」判空）。
 */
ftl_status_t ftl_sync_read_page_raw(nand_addr_t addr, void *buffer, uint8_t module);

/* 擦块：OP_ERASE_BLOCK，无数据链，20ms 超时。 */
ftl_status_t ftl_sync_erase_block(nand_addr_t addr);

#endif /* FTL_FC_INTERFACE_H */

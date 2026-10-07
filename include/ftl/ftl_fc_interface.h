#ifndef FTL_FC_INTERFACE_H
#define FTL_FC_INTERFACE_H

/*
 * FTL-FC Interface 层
 * ===================
 *
 * 【desc 链组装】
 *   ① ftl_cmd_configure_descriptors() — 按 cmd->param.cw_mask 建整条 desc 链
 *   ② ftl_cmd_setup_ppa() — 单一 PBA → CH/CE/PAA + 建链
 *   ③ ftl_cmd_setup_ppa_pair() — 每 plane 各自的 PBA → 建链（Policy B）
 *   ④ addr_to_cmd() — nand_addr_t 直填 + 建链（遍历路径，省打包拆包）
 *
 * 【数据搬运】
 *   ⑤ ftl_cmd_write_data() — 用户数据写入 desc 链各 CW 的 USER 区
 *   ⑥ ftl_cmd_read_data() — 从 desc 链各 CW 的 USER 区拷出
 *
 * 【同步等待】
 *   ⑦ ftl_internal_sync_wait() — 等待指定 command_id 完成
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

/* ── desc 链组装共用小工具 ── */

/*
 * ftl_cmd_plane_span - 命令能触及的 plane 段 [*first, *last)
 *
 * 规则与 FC 内部 fc_command_plane_span 一致（FTL 侧独立持有一份）：
 *   multi-plane → 从 plane 0 起，覆盖 multi_plane 个；
 *   单 plane     → 只覆盖 paa_to_plane(cmd->paa) 那一个——掩码是绝对编号，
 *                 plane 1 的单 plane 命令落在 0xF0 段而不是 0x0F 段。
 *
 * ⚠ 这份规则与 src/fc/fc.c 的 fc_command_plane_span 是同一语义的两份实现，
 *   改 plane 语义时必须同步（见 docs/changelog/fc_policy_layer_uplift.md）。
 *
 * @return 段内 plane 数（= multi_plane，单 plane 时为 1）
 */
static inline uint8_t ftl_cmd_plane_span(const fc_command_t *cmd, uint8_t *first, uint8_t *last)
{
    const uint8_t count = (cmd->config.multi_plane > 0U) ? (uint8_t)cmd->config.multi_plane : 1U;

    *first = (count == 1U) ? (uint8_t)paa_to_plane(cmd->paa) : 0U;
    *last  = (uint8_t)(*first + count);
    return count;
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
 * ftl_cmd_configure_descriptors - 按 cmd->param.cw_mask 组装整条 desc 链
 *
 * cw_mask 是 flat 绝对编号（bit n = plane*CW_NUM + cw）；为 0 时退化为
 * 「本命令每个 plane 取连续 0..config.cw」。置位的每个 CW 逐条调
 * fc_command_attach_cw()，故链序 = plane-major、plane 内 CW 递增。
 * 想只读离散 CW（例如 plane1 的 CW0/CW2）：cmd->param.cw_mask = (1U<<4)|(1U<<6)。
 *
 * 行地址：所有 plane 共用 cmd->paa 的行地址（同页语义）。**两 plane 不同块
 * （Policy B）请用 ftl_cmd_setup_ppa_pair()**，本函数不适用。
 *
 * @param cmd  须已设 config.operation / multi_plane / cw / paa
 * @return FTL_OK / FTL_ERROR（掩码越出本命令 plane 段、池空或坐标非法；
 *         半成品链由调用者 fc_command_release 统一回收）
 */
ftl_status_t ftl_cmd_configure_descriptors(fc_command_t *cmd);

/* ── PBA helper ── */

ftl_status_t ftl_cmd_setup_ppa(fc_command_t *cmd, nand_pba_t pba);

/*
 * ftl_cmd_setup_ppa_pair - 按**每 plane 各自**的 PBA 建 desc 链（Policy B）
 *
 * 与 ftl_cmd_setup_ppa 的区别：后者把单一 PBA 填进 command->paa 后展开到各 plane，
 * 隐含「两 plane 同 block」。Policy B 下两 plane 独立分配物理块，故本函数逐条调
 * fc_command_attach_cw(cmd, plane, cw, pba_to_paa(pba[i]))，把每个 PBA 自己的
 * 行地址写进对应 plane 的 desc。
 *
 * ⚠ **plane 号取自每个 PBA 自己的 plane 位，不是数组下标**：读路径的 pba[]
 *   是按 LBA 查出来的，plane 取决于当初怎么写进去（例如 `wr 0 2` 之后
 *   lba 1 映射到 plane 1），下标与 plane 号并不总相等。
 *
 * 遍历范围：pba[0..multi_plane-1]；单 plane 命令只取 pba[0]（其 plane 位决定
 * 落哪个 plane 段）；每个 plane 覆盖 cw ∈ [0, config.cw]。
 *
 * @param cmd  FC 命令（须已设 config.operation / multi_plane / cw / meta 等）
 * @param pba  [PLANE_NUM] 每 plane 的 PBA（ch/ce/lun 取 pba[0]）
 * @return FTL_OK / FTL_ERROR（坐标越界或池空；调用者负责 fc_command_release 回收半成品链）
 */
ftl_status_t ftl_cmd_setup_ppa_pair(fc_command_t *cmd, const nand_pba_t pba[PLANE_NUM]);

/* ── 地址直填：nand_addr_t → FC 命令 ──
 *
 * 遍历路径（GC 搬移、元数据扫描、Checkpoint）不再 pba_make 打包 → 拆包，
 * 直接用展开地址填入 FC 命令：
 *   ch/ce/lun  → cmd->config 位域
 *   block/plane/page/cw → paa
 *
 * 与 ftl_cmd_setup_ppa(cmd, pba_from_addr(addr)) 等价（省去打包拆包）：
 * 地址填好后**必须建 desc 链**——否则命令无 buffer，数据无从搬运，
 * fc_submit 也会被 fc_command_validate 以 desc_id == INVALID 拒绝。
 * 无数据操作（ERASE / RESET 等）由 ftl_cmd_configure_descriptors 短路为 desc_id = INVALID。
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
    cmd->paa        = (DWORD)paa_make(addr->page, addr->plane, addr->block, addr->lun, addr->cw);

    return ftl_cmd_configure_descriptors(cmd);
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

#endif /* FTL_FC_INTERFACE_H */

#ifndef FTL_FC_INTERFACE_H
#define FTL_FC_INTERFACE_H

/*
 * FTL-FC Interface 层
 * ===================
 *
 * 本层提供 3 个 PBA 操作 helper + 1 个地址直填 helper + 3 个内部同步 NAND 操作函数。
 *
 * 【地址直填】（新增，Step 2）
 *   ① addr_to_cmd()  — nand_addr_t → FC 命令（遍历路径直填，省打包拆包）
 *
 * 【PBA helper】（已实现）
 *   ② ftl_cmd_setup_ppa() — PBA→CH/CE/PAA + desc 链配置
 *   ③ ftl_cmd_write_data() — 用户数据写入 desc buffer
 *   ④ ftl_cmd_read_data() — 从 desc buffer 读取用户数据
 *
 * 【内部同步 NAND 操作】（待删除，Step 2 后仅保留 sync_wait）
 *   ⑤ ftl_internal_sync_wait() — 等待指定 command_id 完成（保留）
 *   ✗ ftl_internal_read_page() / write_page() / erase_block() / read_page_raw()
 *     — 各模块改为 alloc + addr_to_cmd + submit + sync_wait + release 内联，
 *       这四个包装函数删除（见各调用点注释）。
 *
 * 同步操作内部各自 alloc/fc_submit/sync_wait/release，调用者只需
 * 传入参数即可。适用于 L2P region R/W、Journal flush、GC 等内部操作。
 */

#include "fc/fc_interface.h"
#include "ftl/ftl_type.h"
#include "nand/nand_config.h"

#include <stdint.h>

/* ── 地址直填：nand_addr_t → FC 命令 ──
 *
 * 遍历路径（GC 搬移、元数据扫描、Checkpoint）不再 pba_make 打包 →
 * ftl_cmd_setup_ppa 拆包，直接用展开地址填入 FC 命令：
 *   ch/ce/lun  → cmd->config 位域
 *   block/plane/page/cw → paa
 *
 * 与 ftl_cmd_setup_ppa(cmd, pba_from_addr(addr)) 等价（省去打包拆包）：
 * 地址填好后**必须配置 desc 链**——否则命令无 buffer，fc_write/read_cmd_data
 * 无从搬运，fc_submit 也会被 fc_command_validate 以 desc_id == INVALID 拒绝。
 * 无数据操作（ERASE / RESET 等）由 configure 内部短路为 desc_id = INVALID。
 * 随机 IO 路径（从 L2P 拿到 PBA）仍走 ftl_cmd_setup_ppa（保留）。
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

    return (fc_command_configure_descriptors(cmd) == FC_STATUS_OK) ? FTL_OK : FTL_ERROR;
}

/* ── PBA helper（已实现）── */

ftl_status_t ftl_cmd_setup_ppa(fc_command_t *cmd, nand_pba_t pba);
ftl_status_t ftl_cmd_write_data(fc_command_t *cmd, const void *data, uint32_t length);
ftl_status_t ftl_cmd_read_data(fc_command_t *cmd, void *buffer, uint32_t length);

/* ── 内部同步 NAND 操作（仅保留 sync_wait，其余已内联化删除）── */

/*
 * ftl_internal_sync_wait - 同步等待指定 command_id 完成
 *
 * 循环调用 fc_service() + fc_poll_completion() 直到指定命令完成。
 * 最多等待 timeout_us 微秒。
 *
 * @param cmd_id      要等待的 command_id
 * @param status_out  输出：completion status
 * @param timeout_us  超时微秒（0=使用默认）
 * @return 0 找到（status_out 有效），-1 超时
 */
ftl_status_t ftl_internal_sync_wait(uint16_t cmd_id, fc_command_status_t *status_out, uint32_t timeout_us);

#endif /* FTL_FC_INTERFACE_H */
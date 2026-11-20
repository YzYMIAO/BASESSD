/*
 * lifetime_port.c — FTL 完成路径到终身计数的接线（非热对象）
 *
 * 设计见 docs/design/common/statistics_design.md §9.4：
 * 完成路径只调用一次；die 分解、op 分类、ECC/UNC 查询都在这里。
 */

#include "lifetime_port.h"

#include "ftl.h"
#include "log.h"
#include "onfi_1_0.h"

#if SSD_STATS_PERSIST_ENABLE

/**
 * @brief 本实例的紧凑 die 号；不可用时返回 FTL_DIE_INVALID
 *
 * @param ftl 实例
 * @return die 号
 */
static BYTE lifetime_port_die(const ftl_ctx_s *ftl)
{
    ASSERT(ftl != NULL);
    return topo_2_die(ftl->topo);
}

/**
 * @brief host I/O 成功完成的字节记账（排除内部 page0 屏障写）
 *
 * @param ftl     实例
 * @param pending 已完成的子操作
 * @param write   true = 写，false = 读
 */
void lifetime_note_host_io(const ftl_ctx_s *ftl, const pend_req_s *pending, BOOL write)
{
    ASSERT(ftl != NULL);
    ASSERT(pending != NULL);

    if ((ftl == NULL) || (pending == NULL))
    {
        return;
    }

    // page0 是内部屏障写（缺失 plane 写零），不是 host 负载。
    if (pending->op == PENDING_OP_WRITE_PAGE0)
    {
        return;
    }

    const BYTE inst = lifetime_port_die(ftl);

    if (inst != FTL_DIE_INVALID)
    {
        lifetime_note_host(inst, write, (DWORD)pending->n_cw * FTL_IU_BYTES);
    }
}

/**
 * @brief 一条 FC 命令完成的介质健康记账（ECC / UNC / 编程失败 / 擦除失败）
 *
 * @param ftl        实例
 * @param cmd        已完成的命令（desc 链仍有效）
 * @param completion 完成状态
 */
void lifetime_note_completion(const ftl_ctx_s *ftl, const fc_cmd_s *cmd, const fc_cmd_status_s *completion)
{
    ASSERT(ftl != NULL);
    ASSERT(cmd != NULL);
    ASSERT(completion != NULL);

    if ((ftl == NULL) || (cmd == NULL) || (completion == NULL))
    {
        return;
    }

    const BYTE inst = lifetime_port_die(ftl);

    if (inst == FTL_DIE_INVALID)
    {
        return;
    }

    const BYTE  nand_op = (BYTE)cmd->config.operation;
    const DWORD status  = completion->status;
    const BOOL  failed  = fc_cmd_status_has_error(status);

    if (nand_operation_is_page_read(nand_op) != 0U)
    {
        lifetime_note_read_health(inst, fc_get_cmd_ecc_max(completion->cmd_id), fc_get_cmd_has_unc(completion->cmd_id));
    }
    if ((nand_operation_is_page_write(nand_op) != 0U) && (failed != false))
    {
        lifetime_note_program_fail(inst);
    }
    // 擦除没有 ONFI op flag；FC 完成后落的 operation 只有这两个块擦除码。
    if (((nand_op == OP_ERASE_BLOCK) || (nand_op == OP_ERASE_BLOCK_MULTI_PLANE)) && (failed != false))
    {
        lifetime_note_erase_fail(inst);
    }
}

#endif /* SSD_STATS_PERSIST_ENABLE */

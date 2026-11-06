#ifndef FC_POOL_H
#define FC_POOL_H

/*
 * fc_pool.h — FC 池私有契约（仅 fc.c / fc_pool.c 使用）
 *
 * 对外 API 在 fc_interface.h；控制器/调度器共享定义在 fc_internal.h。
 * FTL/HIL/测试不得 include 本文件（不在公共 include 目录链上）。
 *
 * 存放内容：
 *   - 池簿记状态枚举（命令 / buffer）
 *   - buffer 池的一行（一个 CW 的 528B 存储）
 *   - 池初始化、状态读写、链校验与释放原语
 */

#include "fc_interface.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* ── 缓冲区池的一行：一个 CW 的 528B 存储（H50：数组类型用 _arr_t）── */
    typedef BYTE fc_buffer_cw_arr_t[FC_BUFFER_POOL_CW_SIZE];

    /* 前向声明：完整定义在 fc_internal.h（避免池头反向依赖控制器头）。 */
    typedef struct FC_CONTROLLER_S fc_ctl_s;

    /* ── 命令 / buffer 状态枚举 ── */
    typedef enum FC_COMMAND_STATE_E
    {
        FC_COMMAND_STATE_FREE = 0U, /* 空闲 */
        FC_COMMAND_STATE_ALLOCATED, /* 已分配（可配置 desc/buffer） */
        FC_COMMAND_STATE_QUEUED,    /* 已提交到 SQ/HSQ */
        FC_COMMAND_STATE_ACTIVE,    /* 已出队，NAND 执行中 */
        FC_COMMAND_STATE_COMPLETED  /* 完成项已入 CQ（可释放） */
    } fc_cmd_state_e;

    typedef enum FC_BUFFER_STATE_E
    {
        FC_BUFFER_STATE_FREE     = 0U, /* 未占用 */
        FC_BUFFER_STATE_OCCUPIED = 1U, /* 已分配给某条命令 */
    } fc_buf_state_e;

    /**
     * @brief 初始化三池本体、簿记数组与空闲链表（不碰队列/调度器/硬件）。
     *
     * @param controller FC 控制器。
     */
    void fc_pool_init(fc_ctl_s *controller);

    /**
     * @brief 读命令状态。
     *
     * @param controller FC 控制器。
     * @param cmd_id 池内命令 ID；越界 ASSERT。
     *
     * @return 命令状态。
     */
    fc_cmd_state_e fc_command_state_get(const fc_ctl_s *controller, fc_cmd_id_t cmd_id);

    /**
     * @brief 读 desc 的归属命令 ID。
     *
     * @param controller    FC 控制器。
     * @param descriptor_id 池内 desc ID；越界 ASSERT。
     *
     * @return 归属命令 ID；FC_CMD_ID_INVALID = 空闲。
     */
    fc_cmd_id_t fc_descriptor_owner_get(const fc_ctl_s *controller, fc_desc_id_t descriptor_id);

    /**
     * @brief 读 buffer 占用状态。
     *
     * @param controller FC 控制器。
     * @param buffer_id  池内 buffer ID；越界 ASSERT。
     *
     * @return FC_BUFFER_STATE_OCCUPIED / FC_BUFFER_STATE_FREE。
     */
    fc_buf_state_e fc_buffer_state_get(const fc_ctl_s *controller, fc_buf_id_t buffer_id);

    /**
     * @brief 命令状态唯一 setter：只允许状态机定义的转移。
     *
     * @param controller FC 控制器。
     * @param cmd_id     命令池槽位号。
     * @param next       目标状态。
     *
     * @note 非法转移：debug 停机；release 拒绝写入（ASSERT_REJECT_VOID），避免静默
     *       破坏状态机。本函数是状态字段的唯一写点，调用方不重复判定同一转移。
     */
    void fc_pool_command_state_set(fc_ctl_s *controller, fc_cmd_id_t cmd_id, fc_cmd_state_e next);

    /**
     * @brief 命令指针 → 池索引（唯一一份实现）。
     *
     * @param controller FC 控制器。
     * @param cmd        属于本控制器命令池的命令指针。
     *
     * @return 命令池索引。
     */
    fc_cmd_id_t fc_pool_command_id(const fc_ctl_s *controller, const fc_cmd_s *cmd);

    /**
     * @brief 提交前的一次性 desc 链完整校验（归属 + 坐标 + 唯一性 + buffer +
     *        同 plane 行一致 + multi-plane 覆盖）。
     *
     * @param controller FC 控制器。
     * @param cmd_id     命令 ID。
     * @param cmd        命令。
     *
     * @return true 链完整；false 归属不符 / 越段 / CW 重复 / buffer 未占用 /
     *         同 plane 行地址不一致 / multi-plane 覆盖不全 / multi_plane 越界 /
     *         cmd->paa 的 plane 越界。
     *
     * @note 由 fc_submit / fc_submit_batch 无条件调用（debug ASSERT + release 拒绝）。
     */
    BOOL fc_pool_chain_validate(const fc_ctl_s *controller, fc_cmd_id_t cmd_id, const fc_cmd_s *cmd);

    /**
     * @brief 沿命令的 desc 链归还全部 buffer，并把各 desc 的 buffer_id 抹成 INVALID。
     *
     * @param controller FC 控制器。
     * @param cmd        命令。
     *
     * @note 只动 buffer，不动 desc 链、不动命令槽位；幂等（已 INVALID 的跳过）。
     */
    void fc_pool_command_release_buffers(fc_ctl_s *controller, fc_cmd_s *cmd);

    /**
     * @brief 释放命令：沿链释放 desc+buffer，命令槽位回 FREE（FREE 幂等）。
     *
     * @param controller FC 控制器。
     * @param cmd        命令。
     *
     * @note 指针不在池内、或状态是 QUEUED/ACTIVE 时，release 下也拒绝，
     *       不释放任何资源（守卫在碰 desc 链/buffer 之前）。
     */
    void fc_pool_command_release(fc_ctl_s *controller, fc_cmd_s *cmd);

#ifdef __cplusplus
}
#endif

#endif /* FC_POOL_H */

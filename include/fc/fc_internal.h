#ifndef FC_INTERNAL_H
#define FC_INTERNAL_H

/*
 * fc_internal.h — FC 内部共享定义（fc.c ↔ fc_pool.c）
 *
 * 仅 FC 实现文件 include；FTL/上层不得 include（公开 API 在 fc_interface.h）。
 *
 * 分工：
 *   fc.c       —— 初始化编排、提交/填充/完成调度、服务主循环、诊断；
 *   fc_pool.c  —— 命令/描述符/缓冲三池、空闲链表、生命周期与链遍历；
 *   本头       —— 两文件共享的控制器结构、内部状态枚举与跨文件原语。
 */

#include "fc.h"
#include "fc_queue.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* ── 缓冲区池的一行：一个 CW 的 528B 存储（H50：数组类型用 _arr_t）── */
    typedef BYTE fc_buffer_cw_arr_t[FC_BUFFER_POOL_CW_SIZE];

    /* ── ① 调度器状态 / 命令链描述 ──
     * 选择逻辑在 fc.c 的 fc_poll_fill；本处只放类型与状态。 */
#define FC_SCHEDULER_CHAIN_MAX NAND_DISPATCH_CHAIN_DEPTH

    typedef struct FC_SCHEDULER_CHAIN_S
    {
        fc_cmd_id_t ids[FC_SCHEDULER_CHAIN_MAX];
        BYTE        count; /* 0=空, 1=单命令, 2+=链 */
    } fc_sched_chain_s;

    /* 链归属队列（per-LUN 链锁）。粒度是「哪条提交队列能喂这个 LUN」，
     * 不是「LUN 不能用」；跨填充轮次持久。详见 fc_poll_fill_queue。 */
    typedef enum FC_CHAIN_OWNER_E
    {
        FC_CHAIN_OWNER_NONE = 0U, /* 该 LUN 上没有未结束的链 */
        FC_CHAIN_OWNER_SQ,        /* 未结束的链在普通队列里 */
        FC_CHAIN_OWNER_HSQ        /* 未结束的链在高优先队列里 */
    } fc_chain_owner_e;

    typedef struct FC_SCHEDULER_S
    {
        fc_sub_queue_s *sq;
        fc_sub_queue_s *hsq;
        /* 链锁：链一旦开始，该 LUN 的填充权归链所在的队列独占，直到链的 END 下发。
         * 取值 fc_chain_owner_e。跨填充轮次持久——fc_poll_fill 的 suspended[] 每趟
         * memset 清零，做不到跨趟保留，所以这份状态必须挂在控制器上。 */
        fc_chain_owner_e chain_owner[CH_NUM][CE_NUM][LUN_NUM];
    } fc_sched_s;

    /* ── ② 命令 / buffer 状态枚举 ── */
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

#if CONFIG_FC_RAID
/* ── RAID 预留类型（条件编译）──
 *
 * 当前 PLANE_NUM=2，FC_STRIPE_DATA_MAX=1，单命令即满 stripe，
 * fc_stripe_submit() 在 fc_service() 中直接处理，不经过 accumulator。
 *
 * fc_stripe_accum_s 和 stripe_accum[] 为 PLANE_NUM>=4 预留扩展：
 * 当需要跨多次 fc_service() 调用积累多条数据命令后统一提交 parity 时，
 * 由 fc_poll_stripe_accumulate()（待实现）管理。 */

/* 单组 stripe 的最大数据命令数 = PLANE_NUM - 1 */
#    define FC_STRIPE_DATA_MAX ((PLANE_NUM - 1U))

    /* 每个 LUN 的 stripe 积累状态（PLANE_NUM>=4 扩展预留） */
    typedef struct FC_STRIPE_ACCUM_S
    {
        BOOL        active;                       /* true = 积累中 */
        BYTE        ch, ce, lun;                  /* 目标 LUN */
        BYTE        count;                        /* 已积累的数据命令数 */
        fc_cmd_id_t data_ids[FC_STRIPE_DATA_MAX]; /* 已积累的数据命令 ID */
        fc_cmd_id_t parity_id;                    /* 已分配的 parity 命令 ID */
    } fc_stripe_accum_s;
#endif /* CONFIG_FC_RAID */

    /* ── ③ FC 控制器：队列/调度器 + 三池引用 + 池簿记 ──
     * 池本体在 fc_pool.c（文件作用域 static），控制器只存指针；
     * 队列头尾/计数直接读 sq/hsq/cq 字段（没有独立的寄存器影子结构）。 */
    typedef struct FC_CONTROLLER_S
    {
        /* 队列 / 调度器 */
        fc_sub_queue_s  sq;
        fc_sub_queue_s  hsq;
        fc_comp_queue_s cq;
        fc_sched_s      scheduler;

        /* 三池引用 */
        fc_cmd_s           *command_pool;
        fc_desc_s          *descriptor_pool;
        fc_buffer_cw_arr_t *buffer_pool;

        /* 池簿记 */
        fc_cmd_id_t    *descriptor_owner; /* desc → 归属命令 ID */
        fc_cmd_state_e *command_state;
        /* buffer 归属：OCCUPIED 时必须等于持有它的命令 ID；上层预申请但尚未
         * 挂到命令时为 FC_CMD_ID_INVALID。释放闸门用它拒绝双释放/在飞释放。 */
        fc_cmd_id_t    *buffer_owner;
        fc_buf_state_e *buffer_state;

        /* 空闲链表（下标即 ID） */
        fc_cmd_id_t  *command_free_next;
        fc_desc_id_t *descriptor_free_next;
        fc_buf_id_t  *buffer_free_next;

        /* 空闲链表头 */
        fc_cmd_id_t  command_free_head;
        fc_desc_id_t descriptor_free_head;
        fc_buf_id_t  buffer_free_head;

        /* O(1) 资源计数：只在分配/释放闸门增减；高水位用于诊断与容量调参。 */
        WORD command_used;
        WORD descriptor_used;
        WORD buffer_used;
        WORD command_high;
        WORD descriptor_high;
        WORD buffer_high;

        /* ── Stripe RAID 积累状态 ── */
#if CONFIG_FC_RAID
        fc_stripe_accum_s stripe_accum[LUN_NUM];                         /* 每 LUN stripe 积累 */
        fc_cmd_id_t       stripe_parity_for_data[FC_COMMAND_POOL_DEPTH]; /* 数据命令 ID → parity 命令 ID 映射 */
#endif                                                                   /* CONFIG_FC_RAID */
    } fc_ctl_s;

    /* 全局单例控制器：定义在 fc.c；fc_pool.c 通过本 extern 访问。 */
    extern fc_ctl_s g_fc_controller;

    /* ══════════════════════════════════════════════════════════
     * ④ 跨文件内部原语（fc_pool.c 定义；fc.c 调用）
     * ══════════════════════════════════════════════════════════ */

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
    fc_cmd_id_t fc_descriptor_owner_get(const fc_ctl_s *controller, WORD descriptor_id);

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
     * @param cmd_id 命令池槽位号。
     * @param next       目标状态。
     *
     * @note 非法转移：debug 停机；release 拒绝写入（ASSERT_REJECT_VOID），
     *       避免静默破坏命令状态机。
     */
    void fc_pool_command_state_set(fc_ctl_s *controller, fc_cmd_id_t cmd_id, fc_cmd_state_e next);

    /**
     * @brief 命令指针 → 池索引（唯一一份实现）。
     *
     * @param controller FC 控制器。
     * @param cmd    属于本控制器命令池的命令指针。
     *
     * @return 命令池索引。
     */
    fc_cmd_id_t fc_pool_command_id(const fc_ctl_s *controller, const fc_cmd_s *cmd);

    /**
     * @brief 提交前的一次性 desc 链完整校验（归属 + 坐标 + 唯一性 + buffer +
     *        同 plane 行一致 + multi-plane 覆盖）。
     *
     * @param controller FC 控制器。
     * @param cmd_id 命令 ID。
     * @param cmd    命令。
     *
     * @return true 链完整；false 归属不符 / 越段 / CW 重复 / buffer 未占用 /
     *         同 plane 行地址不一致 / multi-plane 覆盖不全 / multi_plane 越界。
     *
     * @note 由 fc_submit / fc_submit_batch 无条件调用（debug ASSERT + release 拒绝）。
     */
    BOOL fc_pool_chain_validate(const fc_ctl_s *controller, fc_cmd_id_t cmd_id, const fc_cmd_s *cmd);

    /**
     * @brief 沿命令的 desc 链归还全部 buffer，并把各 desc 的 buffer_id 抹成 INVALID。
     *
     * @param controller FC 控制器。
     * @param cmd    命令。
     *
     * @note 只动 buffer，不动 desc 链、不动命令槽位；幂等（已 INVALID 的跳过）。
     */
    void fc_pool_command_release_buffers(fc_ctl_s *controller, fc_cmd_s *cmd);

    /**
     * @brief 释放命令：沿链释放 desc+buffer，命令槽位回 FREE（FREE 幂等）。
     *
     * @param controller FC 控制器。
     * @param cmd    命令。
     *
     * @note 指针不在池内、或状态是 QUEUED/ACTIVE 时，release 下也拒绝，
     *       不释放任何资源（守卫在碰 desc 链/buffer 之前）。
     */
    void fc_pool_command_release(fc_ctl_s *controller, fc_cmd_s *cmd);

#ifdef __cplusplus
}
#endif

#endif /* FC_INTERNAL_H */

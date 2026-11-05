#ifndef FC_INTERNAL_H
#define FC_INTERNAL_H

/*
 * fc_internal.h — FC 核心内部接口（仅 fc.c ↔ fc_interface.c 共享）
 *
 * 背景：
 *   FC 对外 API（fc_interface.h 的函数）固定使用单例控制器 g_fc_controller，在
 *   fc_interface.c 中实现；池管理/生命周期/服务主循环等真正逻辑在 fc.c 中以 static
 *   实现。本头只在这两个文件间共享：
 *     - fc_t 控制器结构定义 + g_fc_controller 单例
 *     - fc.c 的池/链原语与内部函数声明（供 fc_interface.c 的对外 API 与建链入口调用）
 *   FTL/上层不得 include 本头（用 fc_interface.h / fc.h 即可）。
 *
 * 命名约定：
 *   fc_*_internal(controller, ...) —— 参数化控制器，可注入（测试用）。
 *   对外 API 一律不带 controller，固定用单例 g_fc_controller。
 */

#include "fc.h"
#include "fc_queue.h"

#ifdef __cplusplus
extern "C"
{
#endif

#if CONFIG_FC_RAID
/* ── 跨 Plane Stripe RAID 积累状态 ──
 *
 * 当前 PLANE_NUM=2，FC_STRIPE_DATA_MAX=1，单命令即满 stripe，
 * fc_stripe_submit() 在 fc_poll_internal() 中直接处理，不经过 accumulator。
 *
 * fc_stripe_accum_t 和 stripe_accum[] 为 PLANE_NUM>=4 预留扩展：
 * 当需要跨多次 fc_service() 调用积累多条数据命令后统一提交 parity 时，
 * 由 fc_poll_stripe_accumulate()（待实现）管理。 */

/* 单组 stripe 的最大数据命令数 = PLANE_NUM - 1 */
#    define FC_STRIPE_DATA_MAX ((PLANE_NUM - 1U))

    /* 每个 LUN 的 stripe 积累状态（PLANE_NUM>=4 扩展预留） */
    typedef struct
    {
        bool            active;                       /* true = 积累中 */
        BYTE            ch, ce, lun;                  /* 目标 LUN */
        BYTE            count;                        /* 已积累的数据命令数 */
        fc_command_id_t data_ids[FC_STRIPE_DATA_MAX]; /* 已积累的数据命令 ID */
        fc_command_id_t parity_id;                    /* 已分配的 parity 命令 ID */
    } fc_stripe_accum_t;
#endif /* CONFIG_FC_RAID */

    /* ── 调度器状态 / 命令链描述（原 fc_scheduler.h 并入） ──
     * 选择逻辑在 fc.c 的 fc_poll_fill；本处只放类型与状态。 */
#define FC_SCHEDULER_CHAIN_MAX NAND_DISPATCH_CHAIN_DEPTH

    typedef struct
    {
        fc_command_id_t ids[FC_SCHEDULER_CHAIN_MAX];
        BYTE            count; /* 0=空, 1=单命令, 2+=链 */
    } fc_scheduler_chain_t;

    /* 链归属队列（per-LUN 链锁）。粒度是「哪条提交队列能喂这个 LUN」，
     * 不是「LUN 不能用」；跨填充轮次持久。详见 fc_poll_fill_queue。 */
    typedef enum
    {
        FC_CHAIN_OWNER_NONE = 0U, /* 该 LUN 上没有未结束的链 */
        FC_CHAIN_OWNER_SQ,        /* 未结束的链在普通队列里 */
        FC_CHAIN_OWNER_HSQ        /* 未结束的链在高优先队列里 */
    } fc_chain_owner_e;

    typedef struct
    {
        fc_submission_queue_t *sq;
        fc_submission_queue_t *hsq;
        /* 链锁：链一旦开始，该 LUN 的填充权归链所在的队列独占，直到链的 END 下发。
         * 取值 fc_chain_owner_e。跨填充轮次持久——fc_poll_fill 的 suspended[] 每趟
         * memset 清零，做不到跨趟保留，所以这份状态必须挂在控制器上。 */
        BYTE chain_owner[CH_NUM][CE_NUM][LUN_NUM];
    } fc_scheduler_t;

    /* ── 命令 / buffer 状态枚举（fc.c / fc_interface.c 共享） ── */
    typedef enum
    {
        FC_COMMAND_STATE_FREE = 0U, /* 空闲 */
        FC_COMMAND_STATE_ALLOCATED, /* 已分配（可配置 desc/buffer） */
        FC_COMMAND_STATE_QUEUED,    /* 已提交到 SQ/HSQ */
        FC_COMMAND_STATE_ACTIVE,    /* 已出队，NAND 执行中 */
        FC_COMMAND_STATE_COMPLETED  /* 完成项已入 CQ（可释放） */
    } fc_cmd_state_e;

    typedef enum
    {
        FC_BUFFER_STATE_FREE     = 0U, /* 未占用 */
        FC_BUFFER_STATE_OCCUPIED = 1U, /* 已分配给某条命令 */
    } fc_buffer_state_e;

    /* ── FC 控制器：命令状态机 + 三池引用 + 队列/调度器 ──
     * 仅内部实现可见；对外通过 fc_interface.h 的 API 访问。
     * 队列头尾/计数直接读 sq/hsq/cq 字段（没有独立的寄存器影子结构）。 */
    typedef struct fc_t
    {
        fc_submission_queue_t sq;
        fc_submission_queue_t hsq;
        fc_completion_queue_t cq;
        fc_scheduler_t        scheduler;
        fc_command_t         *command_pool;
        fc_descriptor_t      *descriptor_pool;
        BYTE(*buffer_pool)
        [FC_BUFFER_POOL_CW_SIZE];
        fc_command_id_t   *descriptor_owner;
        fc_cmd_state_e    *command_state;
        fc_command_id_t   *command_free_next;
        WORD              *descriptor_free_next;
        WORD              *buffer_free_next;
        fc_buffer_state_e *buffer_state;
        fc_command_id_t    command_free_head;
        WORD               descriptor_free_head;
        WORD               buffer_free_head;

        /* ── Stripe RAID 积累状态 ── */
#if CONFIG_FC_RAID
        fc_stripe_accum_t stripe_accum[LUN_NUM];                         /* 每 LUN stripe 积累 */
        fc_command_id_t   stripe_parity_for_data[FC_COMMAND_POOL_DEPTH]; /* 数据命令 ID → parity 命令 ID 映射 */
#endif                                                                   /* CONFIG_FC_RAID */
    } fc_t;

    /* 全局单例控制器：在 fc.c 中定义，fc_interface.c 引用 */
    extern fc_t g_fc_controller;

    /* ══════════════════════════════════════════════════════════
     * 内部实现（fc.c 定义，fc_interface.c 经此调用）
     * ══════════════════════════════════════════════════════════ */

    /**
     * @brief 初始化控制器（池清零 + 空闲链表 + 队列/调度器/NAND 初始化）。
     *
     * @param controller FC 控制器。
     *
     * @return FC_STATUS_OK        初始化成功。
     *         FC_STATUS_IO_ERROR  NAND 派发层初始化失败。
     */
    fc_status_e fc_init_internal(fc_t *controller);

    /**
     * @brief 从空闲链表分配命令槽位，置 ALLOCATED。
     *
     * @param controller FC 控制器。
     *
     * @return 命令指针；命令池空时返回 NULL。
     */
    fc_command_t *fc_command_alloc_internal(fc_t *controller);

    /* ── 池/链原语（跨文件：建链入口在 fc_interface.c）──
     * fc_command_configure_descriptor（fc_interface.c 的公共入口）用这两个原语编排
     * 一次建链；「按掩码一次性建链」的策略层在 FTL 侧
     * （ftl_cmd_configure_descriptors），循环调公共入口实现。 */

    /**
     * @brief 从空闲链表分配一个描述符（出池即自洽：next_id/buffer_id 显式置哨兵）。
     * **不挂链**——desc 何时进入命令的数据链由调用者决定。
     * ⚠ **调用者契约**：分配后必须立刻 fc_command_chain_append，否则会产生
     * 「已分配但不在任何链上」的游离 desc，破坏「失败即沿链完整回收」不变量
     * （该不变量由 fc_interface.c 的建链入口保证，本原语不保证）。
     *
     * @param controller FC 控制器。
     * @param command    归属命令；须属于本控制器的命令池。
     *
     * @return 描述符指针；描述符池空时返回 NULL。
     */
    fc_descriptor_t *fc_descriptor_alloc_internal(fc_t *controller, fc_command_t *command);

    /**
     * @brief 把已填好的 descriptor 追加到命令 desc 链尾（空链则本条即链头，前驱置 chain=MORE）。
     * 调用者须保证 desc 的 paa/config/meta/next_id 均已填好。
     *
     * @param controller FC 控制器。
     * @param command    目标命令。
     * @param descriptor 待追加的描述符。
     */
    void fc_command_chain_append(fc_t *controller, fc_command_t *command, fc_descriptor_t *descriptor);

    /**
     * @brief 按优先级把命令投递到 HSQ（high_priority）或 SQ，置 QUEUED。
     *
     * @param controller FC 控制器。
     * @param command    待提交命令。
     *
     * @return FC_STATUS_OK         入队成功。
     *         FC_STATUS_QUEUE_FULL 目标队列满。
     */
    fc_status_e fc_submit_internal(fc_t *controller, fc_command_t *command);

    /**
     * @brief 服务主循环单步（推进 NAND + 调度提交）——fc_service 的转发目标。
     *
     * @param controller FC 控制器。
     *
     * @return FC_STATUS_OK          本轮有完成被消费，或有命令被下发。
     *         FC_STATUS_QUEUE_EMPTY 无完成且 SQ/HSQ 均为空。
     *         FC_STATUS_NOT_READY   无完成，队列非空但当前无 LUN 可下发。
     *         FC_STATUS_IO_ERROR    CQ 满或命令下发失败。
     */
    fc_status_e fc_poll_internal(fc_t *controller);

    /**
     * @brief 从 CQ 取一条完成项。
     *
     * @param controller FC 控制器。
     * @param completion 完成项出参。
     *
     * @return FC_STATUS_OK          取到一条完成项。
     *         FC_STATUS_QUEUE_EMPTY CQ 为空。
     */
    fc_status_e fc_poll_completion_internal(fc_t *controller, fc_command_status_t *completion);

    /**
     * @brief 从 CQ 批量取完成项：最多 max_count 条，实际条数写入 *count。
     * 上限由调用方给（FC 不写死）；循环被 CQ 占用数天然封顶。
     *
     * @param controller  FC 控制器。
     * @param completions 完成项输出数组。
     * @param max_count   最多取多少条。
     * @param count       实际取到的条数（出参）。
     *
     * @return FC_STATUS_OK          取到 ≥1 条。
     *         FC_STATUS_QUEUE_EMPTY 一条没取到。
     */
    fc_status_e fc_poll_completions_internal(fc_t *controller, fc_command_status_t *completions, WORD max_count, WORD *count);

    /**
     * @brief 释放命令全部资源（desc 链 + buffer + command 槽位）；调用方须已保证状态合法。
     *
     * @param controller FC 控制器。
     * @param command    待释放命令；须属于本控制器的命令池。
     *
     * @return 恒 FC_STATUS_OK。
     */
    fc_status_e fc_command_release_internal_api(fc_t *controller, fc_command_t *command);

    /**
     * @brief 释放链头那一个 desc（含其 buffer），*head 前移到后继；*head 越界（空链）无操作。
     *
     * @param controller FC 控制器。
     * @param head       链头 ID 的入出参。
     */
    void fc_descriptor_chain_release_head_internal(fc_t *controller, fc_descriptor_id_t *head);

    /**
     * @brief 沿链释放全部 desc + buffer，*head 置 FC_DESCRIPTOR_ID_INVALID。
     *
     * @param controller FC 控制器。
     * @param head       链头 ID 的入出参。
     */
    void fc_descriptor_chain_release_all_internal(fc_t *controller, fc_descriptor_id_t *head);

    /**
     * @brief 从空闲链表分配一个 buffer 槽位并置占用。
     *
     * @param controller FC 控制器。
     *
     * @return buffer ID；buffer 池空时返回 FC_BUFFER_ID_INVALID。
     */
    fc_buffer_id_t fc_buffer_alloc_internal(fc_t *controller);

    /**
     * @brief 释放 buffer（状态清零 + 挂回空闲链表头）。
     * buffer_id 越界一律 ASSERT（调用者已先校验占用）。
     *
     * @param controller FC 控制器。
     * @param buffer_id  待释放的 buffer ID。
     *
     * @return 恒 FC_STATUS_OK。
     */
    fc_status_e fc_buffer_release_internal(fc_t *controller, fc_buffer_id_t buffer_id);

    /**
     * @brief 直接返回 CW 基址（528B，含 META/CRC/PARITY），不做 USER 偏移。
     * buffer_id 越界一律 ASSERT（调用者已先校验占用）。
     *
     * @param controller FC 控制器。
     * @param buffer_id  buffer ID；须 < FC_BUFFER_POOL_DEPTH。
     *
     * @return CW 基址指针（恒非空）。
     */
    BYTE *fc_buffer_id_to_real_buffer_internal(fc_t *controller, fc_buffer_id_t buffer_id);

    /**
     * @brief 命令 ID → 指针（薄壳校验与便利函数使用）。
     *
     * @param controller FC 控制器。
     * @param command_id 池内命令槽位号。
     *
     * @return 命令指针（恒非空；越界为调用者错误）。
     */
    fc_command_t *fc_command_id_to_command_internal(fc_t *controller, WORD command_id);

    /**
     * @brief 描述符 ID → 指针（薄壳校验与便利函数使用）。
     *
     * @param controller    FC 控制器。
     * @param descriptor_id 池内描述符 ID。
     *
     * @return 描述符指针（恒非空；越界为调用者错误）。
     */
    fc_descriptor_t *fc_descriptor_id_to_descriptor_internal(fc_t *controller, WORD descriptor_id);

    /**
     * @brief 指针 → 命令池索引（**唯一一份实现**；前置条件：command 属于 controller->command_pool）。
     *
     * @param controller FC 控制器。
     * @param command    属于本控制器命令池的命令指针。
     *
     * @return 命令池索引。
     */
    fc_command_id_t fc_command_to_id_internal(const fc_t *controller, const fc_command_t *command);

    /**
     * @brief 校验命令的 desc 链是否全部归该命令所有（所有权一致性，fc_submit 使用）。
     *
     * @param controller FC 控制器。
     * @param command_id 命令在池内的槽位号。
     * @param command    待校验命令。
     *
     * @return true = 链上每个 desc 都归该命令；false = 存在归属不符或链越界。
     */
    bool fc_command_descriptors_owned_internal(const fc_t *controller, fc_command_id_t command_id, const fc_command_t *command);

    /*
     * 资源统计读取器（fc_get_resource_stats 使用）。
     * 三个状态读取器的 ID 恒由池派生或循环上界保证在范围内，越界一律 ASSERT。
     */

    /**
     * @brief 读 SQ 当前占用条数。
     *
     * @param controller FC 控制器。
     *
     * @return SQ 占用条数。
     */
    WORD fc_get_sq_count(const fc_t *controller);

    /**
     * @brief 读 HSQ 当前占用条数。
     *
     * @param controller FC 控制器。
     *
     * @return HSQ 占用条数。
     */
    WORD fc_get_hsq_count(const fc_t *controller);

    /**
     * @brief 读 CQ 当前占用条数。
     *
     * @param controller FC 控制器。
     *
     * @return CQ 占用条数。
     */
    WORD fc_get_cq_count(const fc_t *controller);

    /**
     * @brief 读命令状态。
     *
     * @param controller FC 控制器。
     * @param command_id 池内命令槽位号。
     *
     * @return fc_cmd_state_e 状态值。
     */
    fc_cmd_state_e fc_command_state_get(const fc_t *controller, fc_command_id_t command_id);

    /**
     * @brief 读描述符的归属命令 ID。
     *
     * @param controller    FC 控制器。
     * @param descriptor_id 池内描述符 ID。
     *
     * @return 归属命令 ID；空闲描述符为 FC_COMMAND_ID_INVALID。
     */
    fc_command_id_t fc_descriptor_owner_get(const fc_t *controller, WORD descriptor_id);

    /**
     * @brief 读 buffer 占用状态。
     *
     * @param controller FC 控制器。
     * @param buffer_id  池内 buffer ID。
     *
     * @return FC_BUFFER_STATE_OCCUPIED / FC_BUFFER_STATE_FREE。
     */
    fc_buffer_state_e fc_buffer_state_get(const fc_t *controller, fc_buffer_id_t buffer_id);

    /**
     * @brief 读填充轮次的累计计数（诊断「命令在 SQ 却不下发」）。
     *
     * @param calls 出参：fc_poll_fill 累计调用次数。
     * @param pops  出参：填充轮次累计从队列 pop 的命令数。
     */
    void fc_debug_fill_counters(DWORD *calls, DWORD *pops);

#ifdef __cplusplus
}
#endif

#endif /* FC_INTERNAL_H */

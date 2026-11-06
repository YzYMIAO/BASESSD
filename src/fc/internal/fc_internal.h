#ifndef FC_INTERNAL_H
#define FC_INTERNAL_H

/*
 * fc_internal.h — FC 控制器/调度器共享定义（fc.c ↔ fc_pool.c）
 *
 * 池私有契约在 fc_pool.h；对外 API 在 fc_interface.h。
 * 仅 FC 实现文件 include；FTL/上层不得 include。
 *
 * 分工：
 *   fc.c       —— 初始化编排、提交/填充/完成调度、服务主循环、诊断；
 *   fc_pool.c  —— 命令/描述符/缓冲三池、空闲链表、生命周期与链遍历；
 *   本头       —— 两文件共享的控制器结构、调度器类型与全局单例。
 */

#include "fc_pool.h"
#include "fc_queue.h"

#ifdef __cplusplus
extern "C"
{
#endif

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

    /* ── ② FC 控制器：队列/调度器 + 三池引用 + 池簿记 ──
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

#ifdef __cplusplus
}
#endif

#endif /* FC_INTERNAL_H */

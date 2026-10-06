#ifndef FC_INTERNAL_H
#define FC_INTERNAL_H

/*
 * fc_internal.h — FC 核心内部接口（仅 fc.c ↔ fc_interface.c 共享）
 *
 * 背景：
 *   FC 对外 API（fc_interface.h 的 19 个函数）是单例控制器 g_fc_controller
 *   的薄壳，在 fc_interface.c 中实现；真正逻辑（池管理/生命周期/服务主循环）
 *   在 fc.c 中以 static 实现。本头只在这两个文件间共享：
 *     - fc_t 控制器结构定义 + g_fc_controller 单例
 *     - fc_interface.c 薄壳需要调用的内部函数声明
 *   FTL/上层不得 include 本头（用 fc_interface.h / fc.h 即可）。
 *
 * 命名约定：
 *   fc_*_internal(controller, ...) —— 参数化控制器，可注入（测试用）。
 *   对外 API 一律不带 controller，固定用单例 g_fc_controller。
 */

#include "fc.h"
#include "fc_queue.h"
#include "fc_scheduler.h"

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
#    define FC_STRIPE_DATA_MAX ((uint8_t)(PLANE_NUM - 1U))

    /* 每个 LUN 的 stripe 积累状态（PLANE_NUM>=4 扩展预留） */
    typedef struct
    {
        uint8_t         active;                       /* 0=空闲, 1=积累中 */
        uint8_t         ch, ce, lun;                  /* 目标 LUN */
        uint8_t         count;                        /* 已积累的数据命令数 */
        fc_command_id_t data_ids[FC_STRIPE_DATA_MAX]; /* 已积累的数据命令 ID */
        fc_command_id_t parity_id;                    /* 已分配的 parity 命令 ID */
    } fc_stripe_accum_t;
#endif /* CONFIG_FC_RAID */

    /* ── FC 控制器：命令状态机 + 三池引用 + 队列/调度器 ──
     * 仅内部实现可见；对外通过 fc_interface.h 的 API 访问。
     * 队列头尾/计数直接读 sq/hsq/cq 字段（v10 起不再有独立的寄存器影子结构）。 */
    typedef struct fc_t
    {
        fc_submission_queue_t sq;
        fc_submission_queue_t hsq;
        fc_completion_queue_t cq;
        fc_scheduler_t        scheduler;
        fc_command_t         *command_pool;
        fc_descriptor_t      *descriptor_pool;
        uint8_t (*buffer_pool)[FC_BUFFER_POOL_CW_SIZE];
        fc_command_id_t *descriptor_owner;
        uint8_t         *command_state;
        fc_command_id_t *command_free_next;
        uint16_t        *descriptor_free_next;
        uint16_t        *descriptor_owned_next;
        uint16_t        *buffer_free_next;
        uint8_t         *buffer_state;
        uint16_t        *command_descriptor_head;
        fc_command_id_t  command_free_head;
        uint16_t         descriptor_free_head;
        uint16_t         buffer_free_head;
        fc_command_id_t  active_handle;
        uint8_t          active_valid;

        /* ── Stripe RAID 积累状态 ── */
#if CONFIG_FC_RAID
        fc_stripe_accum_t stripe_accum[LUN_NUM];                         /* 每 LUN stripe 积累 */
        fc_command_id_t   stripe_parity_for_data[FC_COMMAND_POOL_DEPTH]; /* 数据命令 ID → parity 命令 ID 映射 */
#endif                                                                   /* CONFIG_FC_RAID */
    } fc_t;

    /* 全局单例控制器：在 fc.c 中定义，fc_interface.c 引用 */
    extern fc_t g_fc_controller;

    /* ── 命令状态枚举（fc.c / fc_interface.c 共享） ── */
    typedef enum
    {
        FC_COMMAND_STATE_FREE = 0U, /* 空闲 */
        FC_COMMAND_STATE_ALLOCATED, /* 已分配（可配置 desc/buffer） */
        FC_COMMAND_STATE_QUEUED,    /* 已提交到 SQ/HSQ */
        FC_COMMAND_STATE_ACTIVE,    /* 已出队，NAND 执行中 */
        FC_COMMAND_STATE_COMPLETED  /* 完成项已入 CQ（可释放） */
    } fc_command_state_t;

    /* ══════════════════════════════════════════════════════════
     * 内部实现（fc.c 定义，fc_interface.c 经此调用）
     * ══════════════════════════════════════════════════════════ */

    /* 初始化全局控制器（池清零 + 空闲链表 + 队列/调度器/NAND 初始化） */
    fc_status_t fc_init_internal(fc_t *controller);

    /* 从空闲链表分配命令槽位，置 ALLOCATED */
    fc_command_t *fc_command_alloc_internal(fc_t *controller);

    /* 为命令配置整条 desc 链（每 CW 一个 desc + buffer，next_id 串联） */
    fc_status_t fc_command_configure_descriptors_internal(fc_t *controller, fc_command_t *command);

    /* 按优先级把命令投递到 HSQ（high_priority）或 SQ，置 QUEUED */
    fc_status_t fc_submit_internal(fc_t *controller, fc_command_t *command);

    /* 服务主循环单步（推进 NAND + 调度提交）——fc_service 的转发目标 */
    fc_status_t fc_poll_internal(fc_t *controller);

    /* 从 CQ 取一条完成项 */
    fc_status_t fc_poll_completion_internal(fc_t *controller, fc_command_status_t *completion);

    /* 释放命令全部资源（desc 链 + buffer + command 槽位）；调用方须已保证状态合法 */
    fc_status_t fc_command_release_internal_api(fc_t *controller, fc_command_t *command);

    /* 从空闲链表分配一个 buffer 槽位并置占用 */
    fc_buffer_id_t fc_buffer_alloc_internal(fc_t *controller);

    /* 释放 buffer（状态清零 + 挂回空闲链表头） */
    fc_status_t fc_buffer_release_internal(fc_t *controller, fc_buffer_id_t buffer_id);

    /* 直接返回 CW 基址（528B，含 META/CRC/PARITY），不做 USER 偏移 */
    uint8_t *fc_buffer_id_to_real_buffer_internal(fc_t *controller, fc_buffer_id_t buffer_id);

    /* 命令/描述符 ID → 指针（越界返回 NULL）——薄壳校验与便利函数使用 */
    fc_command_t    *fc_command_id_to_command_internal(fc_t *controller, uint16_t command_id);
    fc_descriptor_t *fc_descriptor_id_to_descriptor_internal(fc_t *controller, uint16_t descriptor_id);

    /* 校验命令的 desc 链是否全部归该命令所有（所有权一致性，fc_submit 使用） */
    uint8_t fc_command_descriptors_owned_internal(const fc_t *controller, fc_command_id_t command_id, const fc_command_t *command);

    /* 资源统计读取器（fc_get_resource_stats 使用） */
    uint16_t        fc_get_sq_count(const fc_t *controller);
    uint16_t        fc_get_hsq_count(const fc_t *controller);
    uint16_t        fc_get_cq_count(const fc_t *controller);
    uint8_t         fc_command_state_get(const fc_t *controller, fc_command_id_t command_id);
    fc_command_id_t fc_descriptor_owner_get(const fc_t *controller, uint16_t descriptor_id);
    uint8_t         fc_buffer_state_get(const fc_t *controller, fc_buffer_id_t buffer_id);

#ifdef __cplusplus
}
#endif

#endif /* FC_INTERNAL_H */

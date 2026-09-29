#ifndef FTL_PENDING_H
#define FTL_PENDING_H

/*
 * Pending Request 表 — 跟踪异步读请求的 caller buffer
 *
 * ftl_submit_read() 是非阻塞的。提交 FC 命令后，caller 的 buffer 指针
 * 需要在命令完成时找回。本表记录 command_id → buffer 映射。
 *
 * 写操作不需要（数据已在 desc buffer 中），完成时直接 release。
 */

#include "common/ssd_types.h"
#include "ftl/ftl_type.h"
#include "nand/nand_config.h"

#include <stdint.h>

#define FTL_PENDING_MAX (8U)

/* 前向声明 */
struct ftl_context;
struct ssd_request;

/* pending 完成回调 — 在 ftl_handle_completion 数据拷贝 + 标志置位后调用 */
typedef void (*pending_cb_t)(void *ctx);

typedef struct
{
    uint16_t         used              : 1;  /* 1: 槽位占用 */
    uint16_t         command_id        : 15; /* FC command_id */
    lba_t            lba;                    /* LBA, 用于校验 */
    void            *buffer;                 /* caller 的读 buffer（写请求时为 NULL） */
    pending_cb_t     on_complete;            /* 完成后回调（可选，用于 Controller 聚合） */
    void            *on_complete_ctx;       /* 回调上下文（ssd_request_t*） */
    volatile uint8_t *complete_flag;         /* 非 NULL 时 completion handler 置 1 */
    volatile ssd_status_t *status_out;       /* 非 NULL 时 completion handler 写入完成状态 */
    /* ↓↓↓ 写完成确认/回滚 ↓↓↓ */
    nand_pba_t       old_pba;               /* 写前旧 PBA（journal/回滚用） */
    nand_pba_t       new_pba;               /* 新分配的 PBA（坏块标记用） */
    struct ftl_context *ftl;                 /* completion handler 访问 ftl */
    /* ↓↓↓ 操作大小 ↓↓↓ */
    uint16_t         pages          : 2;     /* 页数：1=单 Plane, 2=双 Plane Multi-Plane */
    uint16_t         cache_intermediate : 1; /* 1: Cache Program 中间页（15h），completion 时不更新 L2P/Journal */
} ftl_pending_req_t;

/*
 * pending_init - 清空 pending 表
 */
void pending_init(void);

/*
 * pending_add - 注册一个 pending 请求
 *
 * @param cmd_id              FC command_id
 * @param lba                 逻辑地址
 * @param buffer              caller 数据 buffer（读请求）/ NULL（写请求不拷贝数据）
 * @param on_complete         完成后回调（可选，用于 Controller 子请求聚合）
 * @param on_complete_ctx     回调上下文
 * @param complete_flag       非 NULL 时请求完成 handler 会置 1（用于控制器异步跟踪）
 * @param status_out          非 NULL 时请求完成 handler 写入完成状态（SSD_STATUS_OK/IO_ERROR）
 * @param old_pba             写前旧 PBA（写请求用，读传 NAND_PBA_INVALID）
 * @param new_pba             新分配的 PBA（写请求用，读传 NAND_PBA_INVALID）
 * @param ftl                 FTL 上下文（写请求用，读传 NULL）
 * @param cache_intermediate  Cache Program 中间页标志（1=中间页跳过 L2P/Journal 更新）
 * @param pages               页数（1=单 Plane, 2=双 Plane Multi-Plane）
 * @return 0 成功, -1 表满
 */
int pending_add(uint16_t cmd_id, lba_t lba, void *buffer,
                pending_cb_t on_complete, void *on_complete_ctx,
                volatile uint8_t *complete_flag,
                volatile ssd_status_t *status_out,
                nand_pba_t old_pba, nand_pba_t new_pba,
                struct ftl_context *ftl,
                uint16_t cache_intermediate,
                uint16_t pages);

/*
 * pending_lookup - 按 command_id 查找 pending 项
 *
 * @param cmd_id  FC command_id
 * @return 指向 pending 项的指针, NULL 未找到
 */
ftl_pending_req_t *pending_lookup(uint16_t cmd_id);

/*
 * pending_lookup_by_lba - 按 LBA 查找 pending 项
 *
 * @param lba  逻辑地址
 * @return 指向 pending 项的指针, NULL 未找到
 */
ftl_pending_req_t *pending_lookup_by_lba(lba_t lba);

/*
 * pending_remove - 移除已完成的 pending 项
 *
 * @param cmd_id  FC command_id
 */
void pending_remove(uint16_t cmd_id);

#endif /* FTL_PENDING_H */
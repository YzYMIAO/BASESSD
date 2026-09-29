#ifndef FTL_PENDING_H
#define FTL_PENDING_H

/*
 * Pending Request 表 — 跟踪异步请求的 caller 通知/回调与写回滚信息
 *
 * ftl_submit_read/write() 是非阻塞的。提交 FC 命令后，请求的完成处理
 * （读：数据写回 caller buffer；写：Journal/L2P 确认）需要 command_id → 请求
 * 映射。本表记录 command_id → ftl_io_request_t（嵌入，按值拷贝）+ 写专用字段。
 *
 * 写操作不需要 buffer（数据已在 desc buffer 中），completion 直接确认。
 */

#include "common/ftl_types.h"  /* ftl_io_request_t, pending_cb_t */
#include "ftl/ftl_type.h"      /* lba_t, ftl_status_t */
#include "nand/nand_config.h"  /* nand_pba_t, NAND_PBA_INVALID */

#include <stdint.h>

#define FTL_PENDING_MAX (8U)

/* 前向声明 */
struct ftl_context;

typedef struct
{
    uint16_t           used;              /* 1: 槽位占用 */
    uint16_t           command_id;        /* FC command_id */
    ftl_io_request_t   io;                /* ★ 嵌入请求描述（lba/buffer/pages/通知/回调），按值拷贝 */
    /* ↓↓↓ 写完成确认/回滚（写专用，读请求时无效） ↓↓↓ */
    nand_pba_t         old_pba;           /* 写前旧 PBA（journal/回滚用） */
    nand_pba_t         new_pba;           /* 新分配的 PBA（坏块标记用） */
    struct ftl_context *ftl;              /* completion handler 访问 ftl（写确认用） */
    uint8_t            cache_intermediate;/* 1: Cache Program 中间页（15h），completion 时不更新 L2P/Journal */
} ftl_pending_req_t;

/*
 * pending_init - 清空 pending 表
 */
void pending_init(void);

/*
 * pending_add - 注册一个 pending 请求
 *
 * io 按值嵌入 pending 槽（g_pending[i].io = *io），submit 调用后 io 可复用。
 *
 * @param cmd_id             FC command_id
 * @param io                 IO 请求描述（lba/buffer/pages/complete_flag/status_out/on_complete/ctx）
 * @param old_pba            写前旧 PBA（写请求用，读传 NAND_PBA_INVALID）
 * @param new_pba            新分配的 PBA（写请求用，读传 NAND_PBA_INVALID）
 * @param ftl                FTL 上下文（写请求用，读传 NULL）
 * @param cache_intermediate Cache Program 中间页标志（1=中间页跳过 L2P/Journal 更新）
 * @return FTL_OK 成功，FTL_ERROR 表满
 */
int pending_add(uint16_t cmd_id, const ftl_io_request_t *io,
                nand_pba_t old_pba, nand_pba_t new_pba,
                struct ftl_context *ftl, uint8_t cache_intermediate);

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

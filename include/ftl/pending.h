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

#include "ftl/ftl_type.h"

#include <stdint.h>

#define FTL_PENDING_MAX (8U)

typedef struct
{
    uint16_t  used       : 1;   /* 1: 槽位占用 */
    uint16_t  command_id : 15;  /* FC command_id */
    lba_t     lba;              /* LBA, 用于校验 */
    void     *buffer;           /* caller 的读 buffer */
    uint32_t  count;            /* sector 数 */
} ftl_pending_req_t;

/*
 * pending_init - 清空 pending 表
 */
void pending_init(void);

/*
 * pending_add - 注册一个 pending 读请求
 *
 * @param cmd_id     FC command_id
 * @param lba        逻辑地址
 * @param buffer     caller 数据 buffer
 * @param count      sector 数
 * @return 0 成功, -1 表满
 */
int pending_add(uint16_t cmd_id, lba_t lba, void *buffer, uint32_t count);

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
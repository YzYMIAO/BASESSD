#ifndef FTL_HOST_IO_H
#define FTL_HOST_IO_H

#include "fc_interface.h"
#include "ftl_internal.h"
#include "pending.h"

/* host I/O 完成 / 失败 / 组排空接口。实现见 ftl_host_io.c 与 ftl_confirm.c。 */

/**
 * @brief 推进一个 host 完成组的待下发子操作
 *
 * @param ftl FTL 实例
 * @param gi  pending 组下标
 */
void ftl_host_group_drain(ftl_ctx_s *ftl, WORD gi);

BOOL         ftl_host_io_write_ok(ftl_ctx_s *ftl, pend_req_s *pending);
ssd_status_e ftl_host_io_read_ok(ftl_ctx_s *ftl, fc_cmd_s *cmd, pend_req_s *pending, WORD cmd_id);
void         ftl_host_io_read_fail(ftl_ctx_s *ftl, pend_req_s *pending);
void         ftl_host_io_write_fail(ftl_ctx_s *ftl, pend_req_s *pending, const fc_cmd_status_s *completion);

void ftl_seq_reset(ftl_ctx_s *ftl);
void ftl_host_io_init(void);

#endif /* FTL_HOST_IO_H */

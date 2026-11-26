#ifndef FTL_HOST_IO_H
#define FTL_HOST_IO_H

#include "ftl_config.h"
#include "ftl_type.h"

/* host I/O 组排空 / 顺序流复位接口。实现见 ftl_host_io.c；
 * 完成分支（read_ok / read_fail / write_ok / write_fail）在 ftl_confirm.h。 */

/* 一个组的子操作连续遇 FTL_AGAIN 而无任何进展的轮数上限。超过即判真卡死：
 * 弃未下发子操作、组报 IO_ERROR（已下发的照常完成）。 */
#define FTL_DRAIN_STALL_MAX (8U)

/** @brief 推进一个 host 完成组的待下发子操作。 */
void ftl_host_group_drain(ftl_ctx_s *ftl, WORD gi);

/** @brief 复位顺序读检测状态，并使已算出的预取计划失效。 */
void ftl_seq_reset(ftl_ctx_s *ftl);

/** @brief 初始化 host I/O 的日志限流对象（NOLOAD，显式复位）。 */
void ftl_host_io_init(void);

#endif /* FTL_HOST_IO_H */

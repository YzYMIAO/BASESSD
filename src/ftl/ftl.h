#ifndef FTL_SERVICE_H
#define FTL_SERVICE_H

/*
 * ftl_service — 主循环调度器
 *
 * 每轮 superloop 两段（**泵与实例分开**）：
 *   ① ftl_service_pump()   —— **实例无关**：[pump] 推进 NAND + [pump] 完成泵。
 *                              全局的 NAND 引擎与完成表，每轮**只调一次**。
 *   ② ftl_service(ftl)     —— **单实例**：元数据候选池重算 → 写确认结算 →
 *                              cache 序列确认 → scheduler host 补发 → 序列收尾 →
 *                              wbuf 空闲 flush → 后台调度（逐步清单见 ftl_service 契约）。
 *                              对**每一个**实例都要调（多 LUN 下不能只跑 [0][0][0]）。
 *
 * 三级模式切换依据未决命令数（pending + 组），非"本轮提交了几条 NAND"。
 */

#include "allocator.h"   /* ftl_table_refs_s, super_block_s */
#include "bg_policy.h"   /* 工作量配额后台调度器（ftl_ctx_s 含调度器） */
#include "cache_batch.h" /* cache read/write 批次的常量与对 FTL 的尝试入口 */
#include "checkpoint.h"
#include "fc_interface.h"
#include "ftl_cache.h"
#include "ftl_capacity.h"
#include "ftl_confirm.h"
#include "ftl_core.h"
#include "ftl_diag.h"
#include "ftl_flush.h"
#include "ftl_host_io.h"
#include "ftl_interface.h"
#include "ftl_internal.h"
#include "ftl_sched.h" /* FTL request scheduler */
#include "ftl_type.h"  /* ftl_io_req_s */
#include "gc.h"
#include "journal.h"
#include "l2p_cache.h"
#include "metadata.h"
#include "migrate.h"
#include "pending.h"
#include "read_recovery.h"
#include "ssd_types.h"
#include "stats.h" /* SSD_STATS_ENABLE（统计专用字段按档裁剪） */
#include "timer.h"
#include "wbuf.h"

#include <stdbool.h>
#include <stdint.h>

#endif /* FTL_SERVICE_H */

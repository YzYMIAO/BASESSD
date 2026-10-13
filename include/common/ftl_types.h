#ifndef COMMON_FTL_TYPES_H
#define COMMON_FTL_TYPES_H

#include "common/ssd_types.h" /* ssd_status_t */
#include "ftl/ftl_type.h"     /* lba_t, ftl_status_t, ftl_topology_t */
#include "nand/nand_config.h" /* nand_addr_t, nand_pba_t, NAND_PBA_INVALID */

#include <stdint.h>

/*
 * FTL 公共请求/参数类型（Step 1~3）
 *
 * 聚合对外接口的高频参数，减少形参数（ftl_submit_read/write 8→2 已落地；
 * ftl_init 7→2 / allocator_init 8→2 曾由 ftl_init_params_t 落地，P2-5 后该结构体
 * 已删除——大数组改由 ftl_context_t **自持**（`ftl->tables`，定义于
 * include/ftl/allocator.h —— 因其依赖 super_block_t，放此处会形成循环 include）：
 *   pending_cb_t      — pending 完成回调（Step 3 从 pending.h 迁入，打破循环 include）
 *   ftl_io_request_t  — 一次读写请求的完整描述（含通知/回调字段，Step 3 嵌入 pending）
 *
 * 设计原则：请求结构体用普通字段而非位域（访问更快，C 编译器无
 * 位域可移植性问题），地址一律用 nand_addr_t（展开）或 nand_pba_t
 * （L2P 表紧凑存储）。
 */

/* pending 完成回调 — 在 ftl_handle_completion 数据拷贝 + 标志置位后调用。
 * 定义于此（而非 pending.h），避免 pending.h ↔ ftl_types.h 循环 include。 */
typedef void (*pending_cb_t)(void *ctx);

/* ── 请求参数 ── */

/*
 * ftl_io_request_t — 一次 FTL 读写请求的完整参数。
 *
 * 由 Controller 层构造后整体传入 ftl_submit_read / ftl_submit_write
 * （形参 (ftl, req) 共 2 个，替换原先 8 个散落形参）。
 * pending 表按值嵌入本结构体（ftl->pending[i].io = *req），submit 调用期间
 * 只要求 req 有效，之后完成处理用 pending 表中的拷贝。
 */
typedef struct
{
    uint64_t               lba;             /* Host 逻辑地址（NVMe 协议宽度） */
    void                  *buffer;          /* 读：caller 数据 buffer（数据写回处）；写：数据源 */
    uint32_t               count;           /* ★ 连续 LBA 数（= 上层 block_count）；FTL 自行决定单/双 plane 拆分 */
    volatile uint8_t      *complete_flag;   /* 非 NULL 时全部子操作完成后置 1（异步跟踪） */
    volatile ssd_status_t *status_out;      /* 非 NULL 时组完成后写入 SSD_STATUS_OK/IO_ERROR */
    pending_cb_t           on_complete;     /* 组完成后回调（可选，Controller 子请求聚合） */
    void                  *on_complete_ctx; /* 回调上下文（ssd_request_t*） */
} ftl_io_request_t;

#endif /* COMMON_FTL_TYPES_H */

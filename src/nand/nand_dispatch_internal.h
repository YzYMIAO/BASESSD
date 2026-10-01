/*
 * nand_dispatch_internal.h
 *
 * 内部数据结构，不对外暴露。
 * 由 nand_dispatch.c / nand_dispatch_tables.c 共享。
 */

#ifndef NAND_DISPATCH_INTERNAL_H
#define NAND_DISPATCH_INTERNAL_H

#include "fc/fc_command.h"
#include "fc/fc_types.h"
#include "nand_config.h"
#include "nand_dispatch.h"
#include "nand_fc_interface.h"
#include "nand_fmc.h"
#include "onfi_1_0.h"

#include <stdint.h>

/* ── dispatch task：一次预解析后的命令实例 ──
 * 由 nand_dispatch_task_load() 从 nand_dispatch_request_t 填充，
 * poll 依此驱动 nand_fc_start/step。 */
typedef struct
{
    uint16_t command_id;
    uint8_t  operation;
    uint8_t  ch;            /* Channel — needed for page_buffer access. */
    uint8_t  ce;            /* Chip-enable — needed for routing. */
    uint8_t  lun;           /* LUN — needed for routing. */
    uint8_t  page_type : 2; /* Copy from dispatch request for LDPC engine. */
    uint8_t  reserved  : 6;
    uint8_t  cw_index;    /* Current CW being processed (0-based, within current plane). */
    uint8_t  cw_count;    /* CW count per plane. */
    uint8_t  plane_count; /* Plane count (1 for single-plane, 2+ for multi-plane). */
    uint8_t  started;     /* 0=未启动；1=已 nand_fc_start（含 start 内同步完成） */
    uint8_t  parameter[4];
    uint8_t  feature_addr;

    /* Pre-resolved per-CW per-plane data. cw_count × plane_count entries valid. */
    nand_paa_t cw_paa[PLANE_NUM][CW_NUM];
    uint8_t   *cw_buffers[PLANE_NUM][CW_NUM];

    const nand_engine_backend_t *backend; /* Phase V: per-LUN backend */

    nand_fc_request_t          request;
    nand_fc_transaction_t      transaction;
    nand_dispatch_completion_t completion;
} nand_dispatch_task_t;

/*
 * LUN 相位枚举 (Phase V multi-LUN ISSUE/WAIT/FETCH phase machine)
 *   IDLE  → ISSUE → ISSUED → FETCH → IDLE
 *   ISSUE ：发完命令立即返回，LUN 进入 tR/tPROG/tBERS
 *   ISSUED：DWT 计时最短忙期，到点后启动配对 WAIT/FETCH
 *   FETCH ：执行 WAIT_READY + 数据 + 状态检查
 */
#define LUN_PHASE_IDLE   (0U) /* LUN 空闲 */
#define LUN_PHASE_ISSUE  (1U) /* 正在执行 ISSUE uop */
#define LUN_PHASE_ISSUED (2U) /* ISSUE 完成，LUN 内部操作中（tR/tPROG/tBERS） */
#define LUN_PHASE_FETCH  (3U) /* 正在执行 WAIT/FETCH uop */

/*
 * Per-LUN 链状态。
 * count=0 表示 LUN 空闲；count>0 时 cursor 指示当前执行的 task 索引。
 * 链中所有命令均在同一 ch/ce/lun 上，预加载完成后由 poll 自动推进 cursor，
 * 不清 LUN busy，直到 cursor==count-1 时返回 completion。
 *
 * Phase V 扩展：
 *   phase — LUN_PHASE_*，跟踪相位转换
 *   wait_start_cycles — ISSUE 完成时的 DWT 时间戳，用于最小等待时间控制
 */
typedef struct
{
    nand_dispatch_task_t tasks[NAND_DISPATCH_CHAIN_DEPTH];
    uint8_t              count;
    uint8_t              cursor;
    uint8_t              phase;             /* Phase V: LUN_PHASE_* */
    uint32_t             wait_start_cycles; /* Phase V: ISSUE 完成时 DWT 时间戳 */
} nand_dispatch_lun_chain_t;

/* ======================================================================
 * 函数指针表类型定义
 * ====================================================================== */

/* 异步 request 构建 handler：用 task 的预解析数据组装 nand_fc_request_t。
 * 返回 NAND_RESULT_OK 或 NAND_RESULT_INVALID_ARGUMENT。 */
typedef uint8_t (*nand_dispatch_req_handler_t)(nand_dispatch_task_t *task);

/* Completion 策略 handler：根据不同操作类型设置 cw_status 和 failed_descriptor_id。 */
typedef void (*nand_dispatch_completion_handler_t)(
    nand_dispatch_task_t       *task,
    uint8_t                     result,
    const nand_fc_completion_t *nand_completion);

/* OP_COUNT 项，OP_* 直接作为下标。 */
extern const nand_dispatch_req_handler_t        g_nand_dispatch_req_table[OP_COUNT];
extern const nand_dispatch_completion_handler_t g_nand_dispatch_completion_table[OP_COUNT];

#endif /* NAND_DISPATCH_INTERNAL_H */
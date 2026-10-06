#ifndef NAND_DISPATCH_H
#define NAND_DISPATCH_H

#include "fc/fc_types.h"
#include "nand_config.h"
#include "nand_fc_interface.h"

#include <stdint.h>

/* One fixed per-LUN FIFO (depth NAND_DISPATCH_CHAIN_DEPTH) is attached to each LUN. */
#define NAND_DISPATCH_CHAIN_DEPTH (4U)

/*
 * 每个 LUN 一个待处理 FIFO，深度 = NAND_DISPATCH_CHAIN_DEPTH。
 * **每槽 = 一条命令**：NAND 侧看不到「链」，一条链 = 连续 N 槽。
 * FC 用 nand_dispatch_submit_chain() 把整链原子批量追加（空间不足则整体拒收），
 * poll 遍历每 LUN FIFO，先进先出，每轮每 LUN 推进一条、完成一条即回一份 completion。
 * 因此每条命令各有一份 completion，链中间命令的状态不再被吞掉。
 */

typedef struct
{
    nand_paa_t paa;    /* CW-level PAA resolved by FC before dispatch. */
    uint8_t   *buffer; /* Pre-resolved buffer pointer; NULL if no data. */
} nand_dispatch_cw_info_t;

/*
 * Self-contained request struct. FC resolves all descriptor-chain data
 * (PAA, buffer pointers) at submit time. nand_dispatch never calls back
 * into FC after receiving this struct.
 */
typedef struct
{
    uint16_t command_id;                            /* FC command pool slot for completion correlation. */
    uint8_t  operation;                             /* OP_* constant. */
    uint8_t  ch;                                    /* Channel. */
    uint8_t  ce;                                    /* Chip-enable. */
    uint8_t  lun;                                   /* LUN. */
    uint8_t  cw_count[PLANE_NUM];                   /* 每 plane 的 CW 数（0 .. CW_NUM）。
                                                     * flat CW 掩码下两 plane 可以不同，甚至某 plane 为 0
                                                     * （只选另一个 plane）。总槽位数 = 各 plane 之和。 */
    uint8_t plane_count;                            /* Plane count. 实测语义：0 = 单 plane（FC 只对 multi-plane 分支赋值），
                                                     * N = multi-plane（= command->config.multi_plane）。
                                                     * ⚠ 原注释写 "1 = single" 与实现不符。
                                                     * ⚠ 当前 NAND 层只有 nand_dispatch_scatter_page_buffer() 读它；
                                                     *   multi-plane handler 一律写死 PLANE_NUM 遍历，见 nand_dispatch_tables.c。 */
    uint8_t plane;                                  /* 本命令目标物理 plane 索引（cws 第一维的语义）。
                                                     * 单 plane：= paa_to_plane(command->paa)，有效数据行恒在 cws[plane][*]。
                                                     * multi-plane：恒 0 —— 多 plane uop 硬编码 row[0]/row[1]
                                                     *   （onfi_1_0.c），plane 维从 0 起。
                                                     * 行为中性：行地址始终由 paa_to_nand_row() 从 PAA 值算出，与下标无关。 */
    uint8_t page_type : 2;                          /* LP/MP/UP/TP — LDPC engine needs this for decode threshold. */
    uint8_t reserved  : 6;                          /* Reserved for future data-path bits. */
    uint8_t feature_addr;                           /* Feature register addr for SET/GET_FEATURES (column).
                                                     * 内联数据不走本结构：FC 把 cws[plane][0].buffer 指向
                                                     * 控制器侧暂存（g_fc_return_staging），见 fc.c。 */
    nand_dispatch_cw_info_t cws[PLANE_NUM][CW_NUM]; /* Pre-resolved per-CW data. 第一维 = 物理 plane 索引
                                                     * （非"第几组槽位"）：单 plane 命令只填 cws[plane][*]，
                                                     * 其余行由本结构整体 {0} 初始化保证为确定性零值；
                                                     * multi-plane 命令填 cws[0][*] 与 cws[1][*]。
                                                     * ⚠ FC 为命令 plane 集合内**每个** plane 填 cws[plane][0].paa
                                                     *   （承载行地址/BA6），即使该 plane 的 cw_count 为 0
                                                     *   （flat 掩码可只选 plane1）；槽位 0 由 task_load 无条件照拷。 */
} nand_dispatch_request_t;

typedef struct
{
    uint16_t command_id;
    uint8_t  execution_status;
    uint8_t  nand_status;
    uint8_t  cw_status[CW_TOTAL_MAX];     /* Per-CW status: single-plane = 4, multi-plane = 8 */
    uint8_t  ecc_bit_count[CW_TOTAL_MAX]; /* Per-CW ECC count */
    uint16_t failed_descriptor_id;
} nand_dispatch_completion_t;

/* FC_STATUS_* / FC_CW_STATUS_* macros are needed only in the .c implementation. */
#ifndef FC_CW_STATUS_UNUSED
#    define FC_CW_STATUS_UNUSED (0U)
#endif

/* Initialize fixed Channel Dispatch Queues. This function does not initialize FMC hardware. */
nand_result_t nand_dispatch_init(void);

/* NAND/FMC initialisation succeeded; FC may submit media commands after this. */
void nand_dispatch_set_device_ready(uint8_t ready);

/*
 * 查询设备就绪门控位（归一化 0/1）。
 * 供 NAND 驱动层的同步入口 nand_fc_execute 断言"仅初始化阶段可调"的契约：
 * device_ready 置位后必须走 nand_fc_start/step 异步路径，否则阻塞式执行会卡住调度器。
 */
uint8_t nand_dispatch_is_device_ready(void);

/* Submit one pre-resolved command to its LUN FIFO (count = 1). */
nand_result_t nand_dispatch_submit(const nand_dispatch_request_t *request);

/*
 * 原子提交整条命令链到目标 LUN 的 FIFO（追加 count 槽）。
 * requests[0..count-1] 必须在同一 ch/ce/lun。
 * LUN FIFO 剩余深度不足整链时返回 NOT_READY，无副作用。
 * 追加后链占据连续槽位、按 FIFO 顺序执行，中间不可被后续命令插入。
 * 每条命令各回一份 completion（由 poll 逐条返回）。
 */
nand_result_t nand_dispatch_submit_chain(const nand_dispatch_request_t *requests, uint8_t count);

/* Execute one pending command from one LUN FIFO through ONFI -> NAND Engine -> FMC. */
nand_result_t nand_dispatch_poll(nand_dispatch_completion_t *completion);

/*
 * 查询目标 LUN FIFO 的剩余槽数（0 .. NAND_DISPATCH_CHAIN_DEPTH）。
 * 供 FC 填充时判断能否放下整条链；越界参数返回 0。
 */
uint8_t nand_dispatch_lun_free(uint8_t ch, uint8_t ce, uint8_t lun);

#endif /* NAND_DISPATCH_H */
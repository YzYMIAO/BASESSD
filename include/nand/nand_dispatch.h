#ifndef NAND_DISPATCH_H
#define NAND_DISPATCH_H

#include "fc/fc_types.h"
#include "nand_config.h"
#include "nand_fc_interface.h"

#include <stdint.h>

/* One fixed FIFO is attached to each physical NAND channel. */
#define NAND_CHANNEL_QUEUE_DEPTH (64U)

/*
 * 链式命令最大深度（每个 LUN 可连续缓存的命令数）。
 * 当前设计用于 Read Retry 序列（SET_FEATURES + READ + SET_FEATURES = 3 条）。
 * 增大以支持 RAID 恢复等更长序列。
 */
#define NAND_DISPATCH_CHAIN_DEPTH (4U)

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
    uint16_t                command_id;             /* FC command pool slot for completion correlation. */
    uint8_t                 operation;              /* OP_* constant. */
    uint8_t                 ch;                     /* Channel. */
    uint8_t                 ce;                     /* Chip-enable. */
    uint8_t                 lun;                    /* LUN. */
    uint8_t                 cw_count;               /* CW count per plane (1 .. CW_NUM). */
    uint8_t                 plane_count;            /* Plane count (1 = single, 2+ = multi-plane). */
    uint8_t                 page_type : 2;          /* LP/MP/UP/TP — LDPC engine needs this for decode threshold. */
    uint8_t                 reserved  : 6;          /* Reserved for future data-path bits. */
    uint8_t                 parameter[4];           /* ≤4-byte inline parameter data. */
    uint8_t                 feature_addr;           /* Feature register addr for SET/GET_FEATURES (column). */
    nand_dispatch_cw_info_t cws[PLANE_NUM][CW_NUM]; /* Pre-resolved per-CW per-plane data. */
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
uint8_t nand_dispatch_init(void);

/* NAND/FMC initialisation succeeded; FC may submit media commands after this. */
void nand_dispatch_set_device_ready(uint8_t ready);

/* Submit one pre-resolved command to the channel FIFO. */
uint8_t nand_dispatch_submit(const nand_dispatch_request_t *request);

/*
 * 原子提交整条命令链到指定 LUN 的 chain buffer + 通道队列。
 * requests[0..count-1] 必须在同一 ch/ce/lun。
 * 队列空间无法容纳整链或 LUN chain buffer 非空闲时返回 NOT_READY，无副作用。
 * 链中命令按序执行（由 poll 自动推进 cursor），中间不可被 HSQ 打断。
 */
uint8_t nand_dispatch_submit_chain(const nand_dispatch_request_t *requests, uint8_t count);

/* Execute one pending command from one Channel FIFO through ONFI -> NAND Engine -> FMC. */
uint8_t nand_dispatch_poll(nand_dispatch_completion_t *completion);

#endif /* NAND_DISPATCH_H */
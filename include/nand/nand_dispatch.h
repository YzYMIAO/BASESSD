#ifndef NAND_DISPATCH_H
#define NAND_DISPATCH_H

#include "fc/fc_types.h"
#include "nand_config.h"
#include "nand_fc_interface.h"

#include <stdint.h>

/* One fixed FIFO is attached to each physical NAND channel. */
#define NAND_CHANNEL_QUEUE_DEPTH (64U)

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
    uint16_t command_id;               /* FC command pool slot for completion correlation. */
    uint8_t  operation;                /* OP_* constant. */
    uint8_t  ch;                       /* Channel. */
    uint8_t  ce;                       /* Chip-enable. */
    uint8_t  lun;                      /* LUN. */
    uint8_t  cw_count;                 /* Number of active codewords (1 .. CW_NUM). */
    uint8_t  page_type   : 2;          /* LP/MP/UP/TP — LDPC engine needs this for decode threshold. */
    uint8_t  reserved    : 6;          /* Reserved for future data-path bits. */
    uint8_t  parameter[4];             /* ≤4-byte inline parameter data. */
    uint8_t  feature_addr;             /* Feature register addr for SET/GET_FEATURES (column). */
    nand_dispatch_cw_info_t cws[CW_NUM]; /* Pre-resolved per-CW data; cw_count entries valid. */
} nand_dispatch_request_t;

typedef struct
{
    uint16_t command_id;
    uint8_t  execution_status;
    uint8_t  nand_status;
    uint8_t  cw_status[4];
    uint8_t  ecc_bit_count[4];         /* Per-CW ECC corrected bit count; 0 if ECC engine not integrated. */
    uint16_t failed_descriptor_id;
} nand_dispatch_completion_t;

/* FC_STATUS_* / FC_CW_STATUS_* macros are needed only in the .c implementation. */
#ifndef FC_CW_STATUS_UNUSED
#define FC_CW_STATUS_UNUSED (0U)
#endif

/* Initialize fixed Channel Dispatch Queues. This function does not initialize FMC hardware. */
uint8_t nand_dispatch_init(void);

/* NAND/FMC initialisation succeeded; FC may submit media commands after this. */
void nand_dispatch_set_device_ready(uint8_t ready);

/* Submit one pre-resolved command to the channel FIFO. */
uint8_t nand_dispatch_submit(const nand_dispatch_request_t *request);

/* Execute one pending command from one Channel FIFO through ONFI -> NAND Engine -> FMC. */
uint8_t nand_dispatch_poll(nand_dispatch_completion_t *completion);

#endif /* NAND_DISPATCH_H */
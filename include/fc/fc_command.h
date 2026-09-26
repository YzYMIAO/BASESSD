#ifndef FC_COMMAND_H
#define FC_COMMAND_H

#include "fc_types.h"

#include <stdint.h>

typedef enum
{
    FC_ECC_MODE_BCH       = 0U,  /* BCH encode/decode */
    FC_ECC_MODE_LDPC_HARD = 1U,  /* LDPC hard-decision */
    FC_ECC_MODE_LDPC_SOFT = 2U,  /* LDPC soft-decision */
    FC_ECC_MODE_LDPC_RAW  = 3U,  /* LDPC raw (no decode) */
} fc_ecc_mode_t;

typedef enum
{
    FC_RAID_MODE_NORMAL  = 0U,  /* 无 RAID */
    FC_RAID_MODE_DECODE  = 1U,  /* RAID 解码（ECC 失败时恢复） */
    FC_RAID_MODE_ENCODE  = 2U,  /* RAID 编码（生成 parity） */
    FC_RAID_MODE_RECOVER = 3U,  /* RAID 恢复（同 decode） */
    FC_RAID_MODE_CHECK   = 4U,  /* RAID 校验（暂未实现） */
} fc_raid_mode_t;

typedef enum
{
    FC_PAGE_TYPE_LP = 0U,  /* Lower Page (SLC page / TLC LP) */
    FC_PAGE_TYPE_MP = 1U,  /* Middle Page (TLC MP) */
    FC_PAGE_TYPE_UP = 2U,  /* Upper Page (TLC UP) */
    FC_PAGE_TYPE_TP = 3U,  /* Triple Page (QLC TP) */
} fc_page_type_t;

typedef enum
{
    FC_CRC_ENABLE  = 0U,   /* CRC 校验使能 */
    FC_CRC_DISABLE = 1U,   /* CRC 校验禁用 */
} fc_crc_mode_t;

typedef enum
{
    FC_RANDOM_ENABLE  = 0U,  /* 数据加扰使能 */
    FC_RANDOM_DISABLE = 1U,  /* 数据加扰禁用 */
} fc_random_mode_t;

typedef enum
{
    FC_CHAIN_END  = 0U,  /* 链结束，单命令 */
    FC_CHAIN_MORE = 1U,  /* 链中间，后续还有命令 */
} fc_chain_mode_t;

typedef enum
{
    FC_PRIORITY_SQ  = 0U,  /* 普通优先级（Submission Queue） */
    FC_PRIORITY_HSQ = 1U,  /* 高优先级（High-priority SQ） */
} fc_priority_t;

typedef enum
{
    FC_COMMAND_MODULE_HOST_IO       = 0U,
    FC_COMMAND_MODULE_FTL           = 1U,
    FC_COMMAND_MODULE_GC            = 2U,
    FC_COMMAND_MODULE_READ_RECOVERY = 3U,
    FC_COMMAND_MODULE_METADATA      = 4U,
    FC_COMMAND_MODULE_BBT           = 5U,
    FC_COMMAND_MODULE_CHECKPOINT    = 6U,
    FC_COMMAND_MODULE_WEAR_LEVELING = 7U,
} fc_command_module_t;

/*
| 状态 | 汇总规则 |
|---|---|
| `NAND_FAIL` | 任意 CW 置位即置位 |
| `TIMEOUT` | 任意 CW 置位即置位 |
| `CRC_ERROR` | 任意 CW 置位即置位 |
| `UNC` | 任意 CW 置位即置位 |
| `MATA_ERROR` | 任意 CW 置位即置位 |
| `RAID_ERROR` | 任意 CW 置位即置位 |
| `READY` | 建议所有有效 CW 都 Ready 后才算命令 Ready |
| `PROTECTED` | 任意 CW 或目标块受保护即置位 |
*/
#define FC_CW_STATUS_NAND_FAIL  (1U << 0)
#define FC_CW_STATUS_TIMEOUT    (1U << 1)
#define FC_CW_STATUS_CRC_ERROR  (1U << 2)
#define FC_CW_STATUS_UNC        (1U << 3)
#define FC_CW_STATUS_MATA_ERROR (1U << 4)
#define FC_CW_STATUS_RAID_ERROR (1U << 5)
#define FC_CW_STATUS_READY      (1U << 6)
#define FC_CW_STATUS_PROTECTED  (1U << 7)
#define FC_CW_STATUS_UNUSED     (0U)
#define FC_CW_STATUS_SUCCESS    (FC_CW_STATUS_READY)

/* Command and Descriptor share this 32-bit operation configuration. */
typedef struct fc_cmd_conf_t
{
    DWORD operation : 8; /* ONFI operation macro (OP_*). */
    DWORD ch        : 2; /* NAND channel number. */
    DWORD ce        : 2; /* NAND chip-enable number. */
    DWORD lun       : 2; /* NAND LUN number. */
    DWORD cw        : 2; /* Command: CW count minus one; Descriptor: CW index. */
    DWORD crc       : 1; /* FC_CRC_ENABLE / FC_CRC_DISABLE. */
    DWORD ldpc      : 2; /* fc_ecc_mode_t: BCH/LDPC_HARD/LDPC_SOFT/LDPC_RAW. */
    DWORD raid      : 3; /* fc_raid_mode_t: NORMAL/DECODE/ENCODE/RECOVER/CHECK. */
    DWORD random    : 1; /* FC_RANDOM_ENABLE / FC_RANDOM_DISABLE. */
    DWORD chain     : 1; /* FC_CHAIN_END / FC_CHAIN_MORE. */
    DWORD page_type : 2; /* fc_page_type_t: LP/MP/UP/TP. */
    DWORD multi_plane : 3; /* 0=single; N=N-plane → total desc = multi_plane * (cw+1) */
    DWORD reserved    : 3; /* Reserved for future configuration. */
} fc_cmd_conf_t;

/* Descriptor for one CW, including its physical address, Buffer ID, link ID, status, ECC info, and FAA. */
struct fc_descriptor_t
{
    DWORD         paa;            /* Physical NAND address; valid for this Descriptor's CW. */
    fc_cmd_conf_t config;         /* Copy of Command configuration; config.cw is this CW index. */
    DWORD         buffer_id : 16; /* Buffer Pool ID; 0xFFFF means invalid. */
    DWORD         next_id   : 16; /* Next Descriptor ID; 0xFFFF terminates the chain. */
    DWORD         faa;            /* FTL logical address (LBA) for this specific CW. Written to META on encode. */
    DWORD         status    : 8;  /* CW status, using FC_CW_STATUS_* bits. */
    DWORD         ecc_bits  : 8;  /* ECC corrected bit count from last completion; 0 if no error. */
};

/* SQ command owned by FC after submission; command_id is the Command Pool slot ID. */
struct fc_command_t
{
    DWORD         command_id    : 16; /* Command Pool ID; 0xFFFF means invalid. */
    DWORD         reserved0     : 16; /* Reserved for Command Pool metadata. */
    DWORD         desc_id       : 16; /* First Descriptor ID; 0xFFFF means no Descriptor. */
    DWORD         high_priority : 1;  /* 0=SQ, 1=HSQ. */
    DWORD         module        : 4;  /* Completion route owner; FC does not invoke callbacks. */
    DWORD         feature_addr  : 8;  /* SET/GET_FEATURES register address, sent via COLUMN_0. */
    DWORD         reserved1     : 3;  /* Reserved for future Command control. */
    DWORD         faa;                /* FTL address; retained for completion and recovery. */
    DWORD         paa;                /* Operation-dependent target; data commands use Descriptor PAA values. */
    fc_cmd_conf_t config;             /* Operation, NAND location, CW and data-path configuration. */
    uint8_t       parameter0;         /* Command parameter byte 0. */
    uint8_t       parameter1;         /* Command parameter byte 1. */
    uint8_t       parameter2;         /* Command parameter byte 2. */
    uint8_t       parameter3;         /* Command parameter byte 3. */
};

/* CQ entry containing command-level status and ECC info; FTL resolves Command and Descriptor data by ID.
 * Per-CW detail (status, ecc_bits) lives in the descriptor chain (fc_descriptor_t.status / .ecc_bits).
 * The aggregated status byte uses FC_CW_STATUS_* bits: READY=0x40, NAND_FAIL=0x01, TIMEOUT=0x02, etc. */
struct fc_command_status_t
{
    DWORD command_id : 16; /* Completed Command Pool ID. */
    DWORD desc_id    : 16; /* First Descriptor ID; 0xFFFF means invalid. Updated to first-failing desc on error. */
    DWORD module     : 4;  /* Copy of command->module for direct routing without ID lookup. */
    DWORD ecc_bit0   : 8;  /* CW0 ECC corrected bit count */
    DWORD ecc_bit1   : 8;  /* CW1 ECC corrected bit count */
    DWORD ecc_bit2   : 8;  /* CW2 ECC corrected bit count */
    DWORD ecc_bit3   : 8;  /* CW3 ECC corrected bit count */
    DWORD status     : 8;  /* Aggregated command-level status; FC_CW_STATUS_* bits ORed from all CWs. */
    DWORD reserved   : 20; /* Was: status0:8, status1:8, status2:8, status3:8 */
};

fc_status_t fc_command_validate(const fc_command_t *command);
void        fc_command_status_init(fc_command_status_t *status, const fc_command_t *command);
void        fc_command_status_set_cw(fc_command_status_t *status, uint8_t cw, uint8_t cw_status, fc_descriptor_id_t descriptor_id);

#endif /* FC_COMMAND_H */

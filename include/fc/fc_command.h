#ifndef FC_COMMAND_H
#define FC_COMMAND_H

#include "fc_types.h"

#include <stdint.h>

typedef enum
{
    FC_LDPC_FULL = 0U,
    FC_LDPC_HARD,
    FC_LDPC_SOFT,
    FC_LDPC_RAW
} fc_ldpc_mode_t;

typedef enum
{
    FC_COMMAND_MODULE_HOST_IO = 0U,
    FC_COMMAND_MODULE_FTL,
    FC_COMMAND_MODULE_GC,
    FC_COMMAND_MODULE_READ_RECOVERY,
    FC_COMMAND_MODULE_METADATA,
    FC_COMMAND_MODULE_BBT,
    FC_COMMAND_MODULE_CHECKPOINT,
    FC_COMMAND_MODULE_WEAR_LEVELING
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
#define FC_CW_STATUS_NAND_FAIL   (1U << 0)
#define FC_CW_STATUS_TIMEOUT     (1U << 1)
#define FC_CW_STATUS_CRC_ERROR   (1U << 2)
#define FC_CW_STATUS_UNC         (1U << 3)
#define FC_CW_STATUS_MATA_ERROR  (1U << 4)
#define FC_CW_STATUS_RAID_ERROR  (1U << 5)
#define FC_CW_STATUS_READY       (1U << 6)
#define FC_CW_STATUS_PROTECTED   (1U << 7)
#define FC_CW_STATUS_UNUSED      (0U)
#define FC_CW_STATUS_SUCCESS     (FC_CW_STATUS_READY)

/* Command and Descriptor share this 32-bit operation configuration. */
typedef struct fc_cmd_conf_t
{
    DWORD operation : 8; /* ONFI operation macro. */
    DWORD ch        : 2; /* NAND channel number. */
    DWORD ce        : 2; /* NAND chip-enable number. */
    DWORD lun       : 2; /* NAND LUN number. */
    DWORD cw        : 2; /* Command: CW count minus one; Descriptor: CW index. */
    DWORD crc       : 1; /* 0=enabled, 1=disabled. */
    DWORD ldpc      : 2; /* 0=normal, 1=hard, 2=soft, 3=raw. */
    DWORD raid      : 3; /* 0=normal, 1=decode, 2=encode, 3=recover, 4=check. */
    DWORD random    : 1; /* 0=enabled, 1=disabled. */
    DWORD chain     : 1; /* 0=end of chain, 1=more commands follow. */
    DWORD page_type : 2; /* 0=LP, 1=MP, 2=UP, 3=TP. */
    DWORD reserved  : 6; /* Reserved for future configuration. */
} fc_cmd_conf_t;

/* Descriptor for one CW, including its physical address, Buffer ID, link ID and status. */
struct fc_descriptor_t
{
    DWORD paa; /* Physical NAND address; valid for this Descriptor's CW. */
    fc_cmd_conf_t config; /* Copy of Command configuration; config.cw is this CW index. */
    DWORD buffer_id : 16; /* Buffer Pool ID; 0xFFFF means invalid. */
    DWORD next_id   : 16; /* Next Descriptor ID; 0xFFFF terminates the chain. */
    DWORD status    : 8;  /* CW status, using FC_CW_STATUS_* bits. */
    DWORD reserved  : 24; /* Reserved for future Descriptor state. */
};

/* SQ command owned by FC after submission; command_id is the Command Pool slot ID. */
struct fc_command_t
{
    DWORD command_id : 16; /* Command Pool ID; 0xFFFF means invalid. */
    DWORD reserved0  : 16; /* Reserved for Command Pool metadata. */
    DWORD desc_id       : 16; /* First Descriptor ID; 0xFFFF means no Descriptor. */
    DWORD high_priority : 1;  /* 0=SQ, 1=HSQ. */
    DWORD module        : 4;  /* Completion route owner; FC does not invoke callbacks. */
    DWORD reserved1     : 11; /* Reserved for future Command control. */
    DWORD faa; /* FTL address; retained for completion and recovery. */
    DWORD paa; /* Operation-dependent target; data commands use Descriptor PAA values. */
    fc_cmd_conf_t config; /* Operation, NAND location, CW and data-path configuration. */
    DWORD parameter0 : 8; /* Command parameter byte 0. */
    DWORD parameter1 : 8; /* Command parameter byte 1. */
    DWORD parameter2 : 8; /* Command parameter byte 2. */
    DWORD parameter3 : 8; /* Command parameter byte 3. */
};

/* CQ entry containing IDs and per-CW status; FTL resolves Command and Descriptor data by ID. */
struct fc_command_status_t
{
    DWORD command_id : 16; /* Completed Command Pool ID. */
    DWORD desc_id    : 16; /* First Descriptor ID; 0xFFFF means invalid. */
    DWORD status0    : 8;  /* CW0 status. */
    DWORD status1    : 8;  /* CW1 status. */
    DWORD status2    : 8;  /* CW2 status. */
    DWORD status3    : 8;  /* CW3 status. */
};

fc_status_t fc_command_validate(const fc_command_t *command);
void        fc_command_status_init(fc_command_status_t *status, const fc_command_t *command);
void        fc_command_status_set_cw(fc_command_status_t *status, uint8_t cw, uint8_t cw_status, fc_descriptor_id_t descriptor_id);

#endif /* FC_COMMAND_H */

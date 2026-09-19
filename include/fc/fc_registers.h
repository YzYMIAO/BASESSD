#ifndef FC_REGISTERS_H
#define FC_REGISTERS_H

#include <stdint.h>

#define FC_CONFIG_ENABLE        (1UL << 0)
#define FC_CONFIG_RANDOM_ENABLE (1UL << 1)
#define FC_CONFIG_LDPC_ENABLE   (1UL << 2)
#define FC_CONFIG_RAID_ENABLE   (1UL << 3)
#define FC_STATUS_READY         (1UL << 0)
#define FC_STATUS_BUSY          (1UL << 1)
#define FC_STATUS_SQ_NOT_EMPTY  (1UL << 2)
#define FC_STATUS_HSQ_NOT_EMPTY (1UL << 3)
#define FC_STATUS_CQ_NOT_EMPTY  (1UL << 4)
#define FC_STATUS_ERROR         (1UL << 31)

/* Software-visible controller register model, not a hardware MMIO layout. */
typedef struct
{
    uint32_t config;
    uint32_t status;
    uint16_t sq_head;
    uint16_t sq_tail;
    uint16_t hsq_head;
    uint16_t hsq_tail;
    uint16_t cq_head;
    uint16_t cq_tail;
    uint32_t interrupt_enable;
    uint32_t interrupt_status;
} fc_registers_t;

void fc_registers_reset(fc_registers_t *registers);

#endif /* FC_REGISTERS_H */

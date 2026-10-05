#include "fc_registers.h"

#include "common/assertion.h"

#include <stddef.h>

void fc_registers_reset(fc_registers_t *registers)
{
    /* 前置条件由调用者保证（fc.c 传入 &controller->registers） */
    ASSERT(registers != NULL);

    *registers        = (fc_registers_t){0};
    registers->status = FC_STATUS_READY;
}

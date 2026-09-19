#include "fc_registers.h"

#include <stddef.h>

void fc_registers_reset(fc_registers_t *registers)
{
    if (registers == NULL)
    {
        return;
    }

    *registers        = (fc_registers_t){0};
    registers->status = FC_STATUS_READY;
}

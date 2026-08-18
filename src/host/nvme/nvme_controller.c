#include "host/nvme/nvme.h"

int nvme_process_command(ssd_controller_t *controller, const nvme_command_t *command, nvme_completion_t *completion)
{
    if ((controller == NULL) || (command == NULL) || (completion == NULL))
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    completion->command_id = command->command_id;
    completion->status     = (uint16_t)SSD_STATUS_NOT_SUPPORTED;

    /* Identify、Read、Write 和 Flush 将在对应命令模块中逐步接入。 */
    return SSD_STATUS_NOT_SUPPORTED;
}

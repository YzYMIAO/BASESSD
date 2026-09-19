#ifndef FC_DATA_PATH_H
#define FC_DATA_PATH_H

#include "fc_types.h"

fc_status_t fc_data_path_encode(const fc_command_t *command);
fc_status_t fc_data_path_decode(const fc_command_t *command);

#endif /* FC_DATA_PATH_H */

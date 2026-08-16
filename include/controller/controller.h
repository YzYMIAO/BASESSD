#ifndef SSD_CONTROLLER_H
#define SSD_CONTROLLER_H

#include "controller/ssd_request.h"
#include "ftl/ftl.h"

typedef enum {
    SSD_CONTROLLER_RESET = 0,
    SSD_CONTROLLER_INITIALIZING,
    SSD_CONTROLLER_READY,
    SSD_CONTROLLER_FAILED
} ssd_controller_state_t;

typedef struct {
    ssd_controller_state_t state;
    ftl_context_t *ftl;
} ssd_controller_t;

int ssd_controller_init(ssd_controller_t *controller, ftl_context_t *ftl);
int ssd_controller_submit(ssd_controller_t *controller, ssd_request_t *request);

#endif /* SSD_CONTROLLER_H */

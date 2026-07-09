/**
 * Notification manager (spec section 7).
 *
 * Delivers fault / alarm messages to the user over email or SMS, whichever
 * channels the user has configured. Both channels are optional and
 * best-effort; failure to notify never blocks the control loop.
 */
#pragma once

#include "data_model.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void notification_init(void);

/* Send an alert using whatever channels are enabled in cfg. */
void notification_send_alert(const notify_cfg_t *cfg, fault_class_t f,
                             const char *message);

#ifdef __cplusplus
}
#endif
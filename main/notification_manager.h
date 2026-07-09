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

/* Test email delivery using the supplied config. Returns a malloc'd diagnostic
 * string the caller must free(), or NULL on immediate failure. */
char *notification_test_email(const notify_cfg_t *cfg);

/* Send a restart notification 60s after boot: detailed email + short SMS.
 * Safe to call when neither email nor SMS is configured (no-op). */
void notification_send_restart(const notify_cfg_t *cfg, const char *device_name,
    float sys_temp, float ext_temp, int healthy, int total,
    const sensor_t *sensors, int sensor_count);

#ifdef __cplusplus
}
#endif
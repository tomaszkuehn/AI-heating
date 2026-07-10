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

/* Enqueue an alert / restart notification for off-loop delivery. The caller must
 * hold he_config_lock so the snapshotted fields are consistent; these never block.
 * No-op when neither channel is enabled or the queue is absent/full (dropped + log). */
void notification_dispatch_alert(const notify_cfg_t *cfg, fault_class_t f,
                                 const char *message);
void notification_dispatch_restart(const notify_cfg_t *cfg, const char *device_name,
    float sys_temp, float ext_temp, int healthy, int total,
    const sensor_t *sensors, int sensor_count, bool has_external);

/* Synchronous send (used by the off-loop worker and the test handler). */
void notification_send_alert(const notify_cfg_t *cfg, fault_class_t f,
                             const char *message);
void notification_send_restart(const notify_cfg_t *cfg, const char *device_name,
    float sys_temp, float ext_temp, int healthy, int total,
    const sensor_t *sensors, int sensor_count, bool has_external);

/* Test email delivery using the supplied config. Returns a malloc'd diagnostic
 * string the caller must free(), or NULL on immediate failure. */
char *notification_test_email(const notify_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
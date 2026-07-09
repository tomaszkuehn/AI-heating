/**
 * Control engine (spec sections 5 & 6).
 *
 * The explicit finite-state machine that decides whether to heat. Runs in its
 * own FreeRTOS task so the control layer keeps working even if the web server
 * is unresponsive (spec 13: control independent of presentation).
 *
 * States (spec 5.3): IDLE, HEATING, BOOST_5MIN, PUMP_OVERRUN, FAULT,
 * EMERGENCY_CYCLIC, SIMULATION. Only one main state is active at a time.
 */
#pragma once

#include "data_model.h"
#include "storage_manager.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bind the live configuration (engine reads profile/modes from it). */
void control_init(void);
void control_bind_config(system_config_t *cfg);

/* Advance the state machine. Called every HE_CONTROL_TICK_MS. */
void control_tick(int dt_ms);

/* Build the snapshot shown on the dashboard (spec 11.5/11.6). */
void control_get_snapshot(system_snapshot_t *out);

/* User commands from the web API. */
void control_request_boost(bool on);     /* spec 6.2: 5-minute forced heat */
bool control_boost_active(void);
void control_set_heating_disabled(bool disabled);  /* spec 11.7: kill switch */
bool control_heating_disabled(void);

/* Fault handling integration. */
void control_clear_fault(void);

/* Current hour (0..23) used for profile lookup, offline-safe. */
int  control_current_hour(void);

#ifdef __cplusplus
}
#endif
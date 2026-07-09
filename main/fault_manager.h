/**
 * Fault manager (spec section 7).
 *
 * Detects sensor/interface/network/storage faults and, crucially, heating
 * faults. Heating-fault detection is NOT a naive "temperature not rising"
 * test; it builds an expected thermal-response model that accounts for the
 * outdoor temperature, recent heating history and per-room characteristics,
 * then compares the observed rise against the expectation.
 *
 * Raises notifications (email/SMS) when a configured channel is available.
 */
#pragma once

#include "data_model.h"
#include "storage_manager.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void fault_manager_init(void);

/* Bind the live config (used to reach notification settings on alert). */
void fault_manager_bind_config(system_config_t *cfg);

/* Feed the manager each control tick with the current operating context. */
typedef struct {
    control_state_t state;
    bool            heating_active;
    float           system_temp;
    float           external_temp;
    int             healthy_sensors;
    int             total_sensors;
    bool            network_up;
    bool            storage_ok;
    int             dt_ms;
} fault_context_t;

void fault_manager_observe(const fault_context_t *ctx);

/* Current active fault (FAULT_NONE when healthy). */
fault_class_t fault_manager_current(void);
const char   *fault_manager_text(void);

/* Acknowledge / clear a fault from the UI. */
void fault_manager_clear(void);

/* True when the heating output is rising as expected (no fault). */
bool fault_manager_heating_healthy(void);

/* Learned expected heating rise rate (degC/min), surfaced for diagnostics. */
float fault_manager_learned_rate(void);

/* Network health, updated by the network manager. */
void fault_manager_set_network_up(bool up);
bool fault_manager_network_up(void);

#ifdef __cplusplus
}
#endif
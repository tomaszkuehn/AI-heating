/**
 * Heating output (spec sections 2, 6.3, 8.2).
 *
 * Drives the single GPIO line to the furnace/relay and implements the
 * pump-overrun impulse generator. In heating-simulation mode the physical
 * GPIO is left untouched unless the user explicitly enables mixed mode for
 * lab testing (spec 8.3).
 */
#pragma once

#include "data_model.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void heating_output_init(void);

/* Set the main relay line (respects simulation/mixed-mode rules). */
void heating_output_set_relay(bool on);

/* Current commanded relay state (physical or simulated). */
bool heating_output_relay_state(void);

/* Pump overrun (spec 6.3): periodic short impulses for a bounded window. */
void heating_output_pump_overrun_start(const pump_overrun_cfg_t *cfg);
void heating_output_pump_overrun_stop(void);
bool heating_output_pump_overrun_tick(int dt_ms);  /* returns true while active */

/* Simulation control of the output (spec 8.3). */
void heating_output_set_simulation(bool simulate_heating, bool mixed_mode);
bool heating_output_is_simulated(void);

#ifdef __cplusplus
}
#endif
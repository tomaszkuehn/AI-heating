/**
 * Simulation manager (spec section 8).
 *
 * Two independent, clearly labelled simulation domains:
 *   - sensor simulation: replace real readings with scripted values
 *     (constant, ramp, sudden drop, no-response, out-of-range, virtual).
 *   - heating simulation: model the building's thermal response so the
 *     control algorithm can be exercised without driving physical GPIO.
 *
 * The manager never touches GPIO directly; it only feeds data into the
 * sensor manager and reports a simulated heating decision for the UI.
 */
#pragma once

#include "data_model.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HEATSIM_NORMAL = 0,    /* effective heating */
    HEATSIM_INEFFECTIVE,   /* small temperature rise (fault test)          */
    HEATSIM_OVERHEAT       /* unchecked rise, tests cut-off logic          */
} heating_sim_mode_t;

void simulation_init(void);

/* Configure a single sensor's simulated source (spec 8.1). */
void sim_set_sensor_source(sensor_t *s, sim_source_t src,
                           float base, float rate, float target);

/* Produce the current simulated raw reading for a sensor.
 * Returns NAN for NO_RESPONSE; out-of-range value for OUT_OF_RANGE. */
float sim_produce_reading(const sensor_t *s);

/* Advance the building thermal model by dt_ms. Called each control tick
 * when heating simulation is enabled. `heating_active` is the controller's
 * decision; the model updates per-room virtual temperatures accordingly. */
void sim_step_thermal(int dt_ms, bool heating_active, float external_temp);

/* Configure heating simulation behaviour. */
void sim_set_heating_mode(heating_sim_mode_t mode, float heat_rate,
                          float cool_rate, float inertia);
heating_sim_mode_t sim_heating_mode(void);

/* True when any sensor is sourced from simulation. */
bool sim_any_sensor_simulated(const sensor_t *sensors, int n);

#ifdef __cplusplus
}
#endif
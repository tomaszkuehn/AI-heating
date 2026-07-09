/**
 * Sensor manager (spec sections 2, 3, 4, 6.1).
 *
 * Owns the external sensor interface (a separate board reached over UART),
 * parses its frames, validates every reading, maintains the 7-sample moving
 * average and the effective temperature, runs open-window detection and
 * drift/stale checks, and computes the weighted system average.
 *
 * When a sensor is sourced from the simulation manager, the UART is bypassed
 * for that sensor (spec 8.1: switch a single sensor between real & simulated).
 */
#pragma once

#include "data_model.h"
#include "simulation_manager.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bind the live sensor table (owned by the config). */
void sensor_manager_init(void);
void sensor_manager_bind(sensor_t *sensors, int count, sensor_t *external);

/* Poll the interface (or simulation) once; updates readings & quality. */
void sensor_manager_poll(void);

/* Weighted system average over logically-correct, active sensors. */
float sensor_manager_system_temp(void);

/* Outdoor reading (NAN if absent/invalid). */
float sensor_manager_external_temp(void);

/* Number of currently healthy sensors. */
int  sensor_manager_healthy_count(void);

/* Manual restore of a sensor suppressed by open-window detection. */
void sensor_manager_restore(int id);

/* Frame protocol constants (external sensor interface). */
#define HE_FRAME_START  0xAA
#define HE_FRAME_END    0x55
#define HE_CMD_POLL     0x01

#ifdef __cplusplus
}
#endif
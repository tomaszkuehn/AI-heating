#include "simulation_manager.h"
#include "app_config.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>

/* Per-room virtual temperatures driven by the thermal model. */
static float s_room[HE_MAX_SENSORS];
static float s_room_ext;                 /* outdoor model temp */
static int64_t s_start_us;
static heating_sim_mode_t s_heat_mode = HEATSIM_NORMAL;
static float s_heat_rate = 0.04f;        /* degC per second when heating    */
static float s_cool_rate = 0.006f;       /* degC per second per degC delta  */
static float s_inertia   = 0.10f;        /* smoothing factor [0..1]         */

void simulation_init(void)
{
    s_start_us = esp_timer_get_time();
    for (int i = 0; i < HE_MAX_SENSORS; i++) s_room[i] = 20.0f;
    s_room_ext = 5.0f;
}

void sim_set_sensor_source(sensor_t *s, sim_source_t src,
                           float base, float rate, float target)
{
    if (!s) return;
    s->sim_src = src;
    s->simulated = (src != SIM_SRC_REAL);
    s->sim_base = base;
    s->sim_rate = rate;
    s->sim_target = target;
}

float sim_produce_reading(const sensor_t *s)
{
    if (!s || !s->simulated) return he_nan();
    int64_t elapsed_ms = (esp_timer_get_time() - s_start_us) / 1000;
    float minutes = (float)elapsed_ms / 60000.0f;

    switch (s->sim_src) {
    case SIM_SRC_CONST:
        return s->sim_base;
    case SIM_SRC_RAMP:
        return s->sim_base + s->sim_rate * minutes;
    case SIM_SRC_SUDDEN_DROP: {
        /* Drop 4 degC after 30s, then stay low (open-window scenario). */
        float v = (elapsed_ms > 30000) ? (s->sim_base - 4.0f) : s->sim_base;
        return v;
    }
    case SIM_SRC_NO_RESPONSE:
        return he_nan();
    case SIM_SRC_OUT_OF_RANGE:
        return 200.0f;                  /* triggers OUT_OF_RANGE validation */
    case SIM_SRC_VIRTUAL: {
        int idx = s->id - 1;
        if (idx < 0 || idx >= HE_MAX_SENSORS) return s->sim_base;
        return s_room[idx];
    }
    case SIM_SRC_REAL:
    default:
        return he_nan();
    }
}

void sim_step_thermal(int dt_ms, bool heating_active, float external_temp)
{
    float dt = (float)dt_ms / 1000.0f;
    s_room_ext = external_temp;
    /* Heat gain depends on the configured simulation mode (spec 8.2). */
    float gain = 0.0f;
    if (heating_active) {
        switch (s_heat_mode) {
        case HEATSIM_NORMAL:     gain = s_heat_rate;        break;
        case HEATSIM_INEFFECTIVE: gain = s_heat_rate * 0.15f; break;
        case HEATSIM_OVERHEAT:   gain = s_heat_rate * 1.8f;  break;
        }
    }
    for (int i = 0; i < HE_MAX_SENSORS; i++) {
        /* First-order model: heating adds energy, losses scale with delta-T. */
        float loss = s_cool_rate * (s_room[i] - s_room_ext);
        float dT = (gain - loss) * dt;
        /* Inertia: blend the computed change to emulate thermal mass. */
        s_room[i] += dT * (1.0f - s_inertia);
        s_room[i] = he_clampf(s_room[i], HE_TEMP_MIN_LOGICAL, HE_TEMP_MAX_LOGICAL);
    }
}

void sim_set_heating_mode(heating_sim_mode_t mode, float heat_rate,
                          float cool_rate, float inertia)
{
    s_heat_mode = mode;
    if (heat_rate > 0)   s_heat_rate = heat_rate;
    if (cool_rate > 0)   s_cool_rate = cool_rate;
    if (inertia >= 0 && inertia <= 1) s_inertia = inertia;
}

heating_sim_mode_t sim_heating_mode(void) { return s_heat_mode; }

bool sim_any_sensor_simulated(const sensor_t *sensors, int n)
{
    if (!sensors) return false;
    for (int i = 0; i < n; i++)
        if (sensors[i].simulated) return true;
    return false;
}
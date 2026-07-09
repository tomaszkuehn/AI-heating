#include "control_engine.h"
#include "app_config.h"
#include "sensor_manager.h"
#include "heating_output.h"
#include "fault_manager.h"
#include "simulation_manager.h"
#include "storage_manager.h"
#include "notification_manager.h"
#include "profile.h"

#include <string.h>
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "control";

static system_config_t *s_cfg = NULL;
static control_state_t s_state = ST_IDLE;
static control_state_t s_would_be = ST_IDLE;   /* sub-state under SIMULATION */
static bool s_relay = false;
static bool s_boost = false;
static int  s_boost_remaining_ms = 0;
static bool s_disabled = false;

static int64_t s_boot_us;
static int  s_on_ms = 0;            /* continuous ON duration      */
static int  s_off_ms = 0;          /* continuous OFF duration     */
static int64_t s_last_change_us = 0;
static int  s_anti_osc_ms = 0;      /* remaining anti-oscillation lock (scaled) */
static int  s_max_on_break_ms = 0;   /* remaining forced break after max-on       */

static int  s_log_accum_ms = 0;     /* accumulates toward 1-min log */
static int  s_em_phase_ms = 0;      /* emergency cycle phase        */
static int32_t s_virtual_now = 0;   /* shared virtual wall clock (unix sec) */
static int  s_virtual_frac_ms = 0;  /* sub-second remainder for the above  */

static int control_now_ms(void) { return (int)((esp_timer_get_time() - s_boot_us) / 1000); }

int control_current_hour(void)
{
    /* Use the shared virtual clock so the hour that drives the heating decision
     * is the SAME hour the chart uses to draw the profile overlay. */
    time_t t = (s_virtual_now != 0) ? (time_t)s_virtual_now : time(NULL);
    if (t >= (time_t)HE_TIME_VALID_EPOCH) {
        struct tm tm; localtime_r(&t, &tm);
        return tm.tm_hour;
    }
    /* Very early boot before the clock is seeded: derive from uptime. */
    return (control_now_ms() / 3600000) % 24;
}

void control_init(void)
{
    s_boot_us = esp_timer_get_time();
    s_state = ST_IDLE;
    s_would_be = ST_IDLE;
    s_last_change_us = s_boot_us;
    /* Fix the timezone at boot so localtime_r is deterministic even offline/AP
     * (before SNTP runs). Must match the browser's zone for the chart's profile
     * overlay to align with the actual switching hour — CET for Poland. */
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
}

void control_bind_config(system_config_t *cfg) { s_cfg = cfg; }

bool control_boost_active(void) { return s_boost; }

void control_request_boost(bool on)
{
    if (on && !s_boost) {
        s_boost = true;
        s_boost_remaining_ms = HE_BOOST_DURATION_SEC * 1000;
        ESP_LOGI(TAG, "BOOST 5min started");
    } else if (!on) {
        s_boost = false;
        s_boost_remaining_ms = 0;
        ESP_LOGI(TAG, "BOOST cancelled");
    }
}

void control_set_heating_disabled(bool disabled)
{
    s_disabled = disabled;
    if (disabled) {
        heating_output_set_relay(false);
        s_relay = false;
        ESP_LOGW(TAG, "heating disabled by user (kill switch)");
    } else {
        ESP_LOGI(TAG, "heating re-enabled");
    }
}

bool control_heating_disabled(void) { return s_disabled; }

void control_clear_fault(void) { fault_manager_clear(); }

static void enter_state(control_state_t next, control_state_t *target)
{
    if (*target == next) return;
    *target = next;
    s_last_change_us = esp_timer_get_time();
    s_anti_osc_ms = HE_ANTIOSC_LOCK_SEC * 1000;
    char buf[96];
    snprintf(buf, sizeof(buf), "state->%d", (int)next);
    storage_log_event(FAULT_NONE, 0, buf);
}

/* Decide the relay command from the temperature profile & hysteresis. */
static bool hysteresis_decision(float sys_temp, int hour, bool currently_on)
{
    if (!s_cfg || he_isnan(sys_temp)) return false;
    const profile_hour_t *ph = profile_for_hour(&s_cfg->profile, hour);
    if (!ph) return false;

    bool anti_locked = s_anti_osc_ms > 0;

    /* Forced break after max continuous heating — hard off regardless of temp. */
    if (s_max_on_break_ms > 0) return false;

    int max_on_ms = (s_cfg->max_on_sec > 0) ? s_cfg->max_on_sec * 1000 : 14400000;

    if (currently_on) {
        /* Safety: force off after continuous heating exceeds the configured limit. */
        if (s_on_ms >= max_on_ms) {
            s_max_on_break_ms = (s_cfg->max_on_break_sec > 0)
                                ? s_cfg->max_on_break_sec * 1000 : 600000;
            return false;
        }
        if (sys_temp >= ph->off_temp && s_on_ms >= HE_MIN_ON_SEC * 1000 && !anti_locked)
            return false;
        return true;
    } else {
        if (sys_temp <= ph->on_temp && s_off_ms >= HE_MIN_OFF_SEC * 1000 && !anti_locked)
            return true;
        return false;
    }
}

void control_tick(int dt_ms)
{
    if (!s_cfg) return;

    /* Time scale: fast-forward the whole control loop x10 while accelerated
     * simulation is on, so thermal response, timers, hysteresis and logging all
     * advance in the same virtual clock. */
    int scale = (s_cfg->simulate_heating && s_cfg->sim_time_accel) ? HE_SIM_TIME_SCALE : 1;
    int sdt = dt_ms * scale;
    if (s_anti_osc_ms > 0) { s_anti_osc_ms -= sdt; if (s_anti_osc_ms < 0) s_anti_osc_ms = 0; }
    if (s_max_on_break_ms > 0) { s_max_on_break_ms -= sdt; if (s_max_on_break_ms < 0) s_max_on_break_ms = 0; }

    /* Advance the single shared virtual wall clock BEFORE the hour is read, so
     * the profile lookup for the heating decision, the sample timestamps and the
     * chart's profile overlay all reference the exact same time base. Tracks the
     * real clock when SNTP is set and not accelerated; otherwise it advances by
     * the (x10-scaled) tick so offline / accelerated runs stay self-consistent. */
    if (s_virtual_now == 0)
        s_virtual_now = he_time_valid() ? (int32_t)time(NULL) : (int32_t)HE_TIME_VALID_EPOCH;
    if (scale == 1 && he_time_valid()) {
        s_virtual_now = (int32_t)time(NULL);
    } else {
        s_virtual_frac_ms += sdt;
        while (s_virtual_frac_ms >= 1000) { s_virtual_frac_ms -= 1000; s_virtual_now++; }
    }

    float sys_temp = sensor_manager_system_temp();
    float ext_temp = sensor_manager_external_temp();
    int hour = control_current_hour();
    int healthy = sensor_manager_healthy_count();
    int total = s_cfg->sensor_count;

    /* Advance thermal simulation if heating simulation is on (spec 8.2). */
    if (s_cfg->simulate_heating) {
        sim_step_thermal(sdt, s_relay, he_isnan(ext_temp) ? 5.0f : ext_temp);
    }

    bool target_relay = false;
    control_state_t target_state = ST_IDLE;

    /* Precedence 1: manual kill switch (spec 11.7). */
    if (s_disabled) {
        target_state = ST_IDLE;
        target_relay = false;
    }
    /* Precedence 2: 5-minute boost (spec 6.2). */
    else if (s_boost) {
        s_boost_remaining_ms -= sdt;
        if (s_boost_remaining_ms <= 0) {
            s_boost = false;
            s_boost_remaining_ms = 0;
            ESP_LOGI(TAG, "BOOST 5min finished");
            target_state = ST_IDLE;
            target_relay = hysteresis_decision(sys_temp, hour, false);
        } else {
            target_state = ST_HEATING;
            target_relay = true;
        }
    }
    /* Precedence 3: emergency periodic mode (spec 6.4). */
    else if (s_cfg->emergency.enabled) {
        int period = s_cfg->emergency.period_seconds * 1000;
        int ontime = s_cfg->emergency.on_seconds * 1000;
        if (period <= 0) period = 3600000;
        if (ontime <= 0) ontime = 900000;
        s_em_phase_ms = (s_em_phase_ms + sdt) % period;
        target_state = ST_EMERGENCY_CYCLIC;
        target_relay = (s_em_phase_ms < ontime);
    }
    /* Precedence 4: active fault (unless emergency above handled it). */
    else if (fault_manager_current() != FAULT_NONE &&
             fault_manager_current() != FAULT_NETWORK) {
        target_state = ST_FAULT;
        target_relay = false;
    }
    /* Precedence 5: normal hysteresis. */
    else {
        bool decision = hysteresis_decision(sys_temp, hour, s_relay);
        target_state = decision ? ST_HEATING : ST_IDLE;
        target_relay = decision;
    }

    /* Pump overrun after a HEATING -> OFF transition (spec 6.3). */
    if (s_state == ST_HEATING && target_state == ST_IDLE && s_cfg->pump.enabled) {
        heating_output_pump_overrun_start(&s_cfg->pump);
        target_state = ST_PUMP_OVERRUN;
        target_relay = false;       /* relay driven by overrun impulses */
    }

    /* Apply relay (heating_output honours simulation rules). */
    if (target_state != ST_PUMP_OVERRUN) {
        heating_output_set_relay(target_relay);
    }

    /* Track on/off durations for hysteresis. */
    if (target_relay) { s_on_ms += sdt; s_off_ms = 0; }
    else              { s_off_ms += sdt; s_on_ms = 0; }

    /* If heating simulation is on, the main state is SIMULATION (spec 8.3)
     * but we keep the computed decision for ON/OFF visualisation. */
    if (s_cfg->simulate_heating) {
        s_would_be = target_state;
        s_state = ST_SIMULATION;
        s_relay = target_relay;     /* shown in UI, not driven to GPIO */
    } else {
        enter_state(target_state, &s_state);
        s_relay = target_relay;
    }

    /* Keep pump overrun ticking while in that state. */
    if (s_state == ST_PUMP_OVERRUN || s_would_be == ST_PUMP_OVERRUN) {
        if (!heating_output_pump_overrun_tick(sdt)) {
            /* overrun finished -> back to IDLE */
            if (s_cfg->simulate_heating) { s_would_be = ST_IDLE; s_state = ST_SIMULATION; }
            else enter_state(ST_IDLE, &s_state);
        }
    }

    /* Feed the fault manager. */
    fault_context_t fctx = {
        .state = s_state,
        .heating_active = s_relay,
        .system_temp = sys_temp,
        .external_temp = ext_temp,
        .healthy_sensors = healthy,
        .total_sensors = total,
        .network_up = fault_manager_network_up(),
        .storage_ok = storage_healthy(),
        .dt_ms = sdt,
    };
    fault_manager_observe(&fctx);

    /* Minute logging (spec 9: record temperatures each minute). Gated on a real
     * (SNTP) wall clock, or on simulation. The sample timestamp is the shared
     * virtual clock (s_virtual_now), which is exactly the same time base the
     * heating decision and the chart's profile overlay use — so the plotted
     * ON/OFF bands line up with when the relay actually switches. */
    s_log_accum_ms += sdt;
    if (s_log_accum_ms >= HE_LOG_INTERVAL_SEC * 1000) {
        s_log_accum_ms -= HE_LOG_INTERVAL_SEC * 1000;
        bool sim = s_cfg->simulate_heating || s_cfg->simulate_sensors;
        if (he_time_valid() || sim) {
            minute_sample_t m = {0};
            m.ts = s_virtual_now;
            m.system_temp = sys_temp;
            m.external_temp = ext_temp;
            m.heating_active = s_relay ? 1 : 0;
            m.state = (uint8_t)s_state;
            for (int i = 0; i < HE_MAX_SENSORS; i++) {
                m.per_sensor[i] = (i < total && s_cfg->sensors[i].active)
                                  ? s_cfg->sensors[i].last_effective : he_nan();
            }
            storage_record_minute(&m);
        }
    }

    /* Restart notification: once, 60s after boot, send email + SMS with
     * device name and current system status. Gated on network up to avoid
     * spurious send attempts while Wi-Fi is still connecting. */
    {
        static bool s_restart_notified = false;
        static int  s_restart_age_s   = 0;
        s_restart_age_s += sdt / 1000;
        if (!s_restart_notified && s_restart_age_s >= 60) {
            s_restart_notified = true;
            if (fault_manager_network_up() &&
                (s_cfg->notify.email_enabled || s_cfg->notify.sms_enabled)) {
                notification_send_restart(&s_cfg->notify,
                    s_cfg->device_name, sys_temp, ext_temp,
                    healthy, total, s_cfg->sensors, s_cfg->sensor_count);
            }
        }
    }
}

void control_get_snapshot(system_snapshot_t *out)
{
    if (!out) return;
    out->state = s_state;
    out->heating_active = s_relay;
    out->system_temp = sensor_manager_system_temp();
    out->external_temp = sensor_manager_external_temp();
    out->active_fault = fault_manager_current();
    out->health_ok = (fault_manager_current() == FAULT_NONE || fault_manager_current() == FAULT_NETWORK)
                     && storage_healthy();
    out->uptime_ms = (uint32_t)((esp_timer_get_time() - s_boot_us) / 1000);
    out->simulation_mode = s_cfg ? (s_cfg->simulate_heating || s_cfg->simulate_sensors) : false;
}
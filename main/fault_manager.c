#include "fault_manager.h"
#include "notification_manager.h"
#include "app_config.h"

#include <string.h>
#include <math.h>
#include <stdio.h>

#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "fault";

static system_config_t *s_cfg = NULL;
static fault_class_t s_fault = FAULT_NONE;
static char s_text[96] = "";
static bool s_net_up = true;
static bool s_notified = false;

/* Expected thermal response model (spec 7: not a naive "temp not rising"). */
static float s_heat_start_temp;
static int64_t s_heat_start_us;
static bool s_heating_was_active = false;
static float s_learned_rate = 0.5f;   /* degC per minute, EMA over good cycles */

void fault_manager_init(void)
{
    s_fault = FAULT_NONE;
    s_heat_start_us = 0;
    s_heating_was_active = false;
    s_learned_rate = 0.5f;
}

void fault_manager_bind_config(system_config_t *cfg) { s_cfg = cfg; }

void fault_manager_set_network_up(bool up)
{
    if (up != s_net_up) {
        s_net_up = up;
        if (!up) {
            ESP_LOGW(TAG, "network down");
        } else {
            ESP_LOGI(TAG, "network up");
        }
    }
}

bool fault_manager_network_up(void) { return s_net_up; }

static void raise_fault(fault_class_t f, const char *text)
{
    if (f == s_fault) return;
    s_fault = f;
    strncpy(s_text, text, sizeof(s_text) - 1);
    s_text[sizeof(s_text) - 1] = '\0';
    ESP_LOGW(TAG, "fault raised: %d (%s)", (int)f, text);
    storage_log_event(f, 2, text);
    if (s_cfg && !s_notified) {
        notification_send_alert(&s_cfg->notify, f, text);
        s_notified = true;
    }
}

void fault_manager_clear(void)
{
    if (s_fault != FAULT_NONE) ESP_LOGI(TAG, "fault cleared");
    s_fault = FAULT_NONE;
    s_text[0] = '\0';
    s_notified = false;
}

fault_class_t fault_manager_current(void) { return s_fault; }
const char *fault_manager_text(void) { return s_text; }
bool fault_manager_heating_healthy(void) { return s_fault != FAULT_NO_HEAT_RISE && s_fault != FAULT_LOW_HEAT_RISE; }
float fault_manager_learned_rate(void) { return s_learned_rate; }

void fault_manager_observe(const fault_context_t *ctx)
{
    if (!ctx) return;

    /* --- Heating response model --- */
    if (ctx->heating_active && !s_heating_was_active) {
        s_heat_start_temp = ctx->system_temp;
        s_heat_start_us = esp_timer_get_time();
        s_heating_was_active = true;
    } else if (!ctx->heating_active && s_heating_was_active) {
        /* Heating just stopped: update the learned rate from this cycle if it rose. */
        int64_t dur_us = esp_timer_get_time() - s_heat_start_us;
        float minutes = (float)(dur_us / 1000) / 60000.0f;
        if (minutes > 1.0f && !he_isnan(ctx->system_temp) && !he_isnan(s_heat_start_temp)) {
            float rise = ctx->system_temp - s_heat_start_temp;
            if (rise > 0.1f) {
                float rate = rise / minutes;
                s_learned_rate = s_learned_rate * 0.7f + rate * 0.3f;
            }
        }
        s_heating_was_active = false;
    }

    /* --- Sensor / interface faults --- */
    if (ctx->total_sensors > 0 && ctx->healthy_sensors == 0) {
        raise_fault(FAULT_SENSOR_IFACE, "all sensors unavailable");
        return;
    }

    /* --- Storage fault --- */
    if (!ctx->storage_ok) {
        raise_fault(FAULT_STORAGE, "storage/filesystem error");
        return;
    }

    int grace_ms = s_cfg ? s_cfg->fault_grace_sec * 1000 : 300000;

    /* --- Heating effectiveness (only while heating, after grace) --- */
    if (ctx->heating_active && !he_isnan(ctx->system_temp) && !he_isnan(s_heat_start_temp)) {
        int64_t since_us = esp_timer_get_time() - s_heat_start_us;
        if (since_us > grace_ms * 1000LL) {
            float minutes = (float)(since_us / 1000) / 60000.0f;
            float observed = (ctx->system_temp - s_heat_start_temp) / minutes;
            /* Expected rate corrected for outdoor temperature: colder outside
             * means higher losses, so a lower net rise is still acceptable. */
            float ext = he_isnan(ctx->external_temp) ? 5.0f : ctx->external_temp;
            float expected = s_learned_rate - 0.01f * (ctx->system_temp - ext);
            if (expected < 0.05f) expected = 0.05f;
            if (observed <= 0.0f) {
                raise_fault(FAULT_NO_HEAT_RISE, "no temperature rise while heating");
                return;
            } else if (observed < expected * 0.5f) {
                raise_fault(FAULT_LOW_HEAT_RISE, "heating rise below expected (airlock/valve?)");
                return;
            }
        }
    }

    /* --- Network fault (informational, does not block control) --- */
    if (!ctx->network_up && s_fault == FAULT_NONE) {
        raise_fault(FAULT_NETWORK, "Wi-Fi network unavailable");
        return;
    }

    /* Clear network fault automatically when reconnected. */
    if (ctx->network_up && s_fault == FAULT_NETWORK) {
        fault_manager_clear();
    }
    /* Clear heating faults when heating becomes effective again. */
    if ((s_fault == FAULT_NO_HEAT_RISE || s_fault == FAULT_LOW_HEAT_RISE) &&
        ctx->heating_active && !he_isnan(ctx->system_temp) && !he_isnan(s_heat_start_temp)) {
        int64_t since_us = esp_timer_get_time() - s_heat_start_us;
        if (since_us > grace_ms * 1000LL) {
            float minutes = (float)(since_us / 1000) / 60000.0f;
            float observed = (ctx->system_temp - s_heat_start_temp) / minutes;
            if (observed >= s_learned_rate * 0.5f) fault_manager_clear();
        }
    }
}
#include "sensor_manager.h"
#include "app_config.h"

#include <string.h>
#include <math.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "sensor";

static sensor_t *s_sensors = NULL;     /* internal sensors */
static int       s_count = 0;
static sensor_t *s_external = NULL;

/* Per-sensor bookkeeping for open-window & stale detection. */
static float  s_baseline[HE_MAX_SENSORS];
static float  s_prev_eff[HE_MAX_SENSORS];
static int64_t s_last_change_us[HE_MAX_SENSORS];
static int64_t s_drift_since_us[HE_MAX_SENSORS];
static uint8_t s_uart_buf[HE_SENSOR_BUF_SIZE];

static uint32_t mono_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ---- CRC8 (poly 0x07) ---- */
static uint8_t crc8(const uint8_t *p, size_t n)
{
    uint8_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++)
            c = (c & 0x80) ? (c << 1) ^ 0x07 : (c << 1);
    }
    return c;
}

void sensor_manager_init(void)
{
    const uart_config_t uc = {
        .baud_rate  = HE_SENSOR_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    int intr_alloc_flags = 0;
    ESP_ERROR_CHECK(uart_driver_install(HE_SENSOR_UART, HE_SENSOR_BUF_SIZE * 2,
                                        HE_SENSOR_BUF_SIZE, 0, NULL, intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(HE_SENSOR_UART, &uc));
    ESP_ERROR_CHECK(uart_set_pin(HE_SENSOR_UART, HE_SENSOR_UART_TX,
                                 HE_SENSOR_UART_RX, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    for (int i = 0; i < HE_MAX_SENSORS; i++) {
        s_baseline[i] = NAN;
        s_prev_eff[i] = NAN;
        s_last_change_us[i] = 0;
        s_drift_since_us[i] = 0;
    }
    ESP_LOGI(TAG, "UART%d initialised for external sensor interface", HE_SENSOR_UART);
}

void sensor_manager_bind(sensor_t *sensors, int count, sensor_t *external)
{
    s_sensors = sensors;
    s_count = count;
    s_external = external;
}

/* Push a raw reading into a sensor's rolling buffer and update effective temp. */
static void push_reading(sensor_t *s, float raw)
{
    s->last_raw = raw;
    s->samples[s->sample_head] = raw;
    s->sample_head = (s->sample_head + 1) % HE_MOVING_AVG_WINDOW;
    if (s->sample_count < HE_MOVING_AVG_WINDOW) s->sample_count++;

    float sum = 0;
    for (int i = 0; i < s->sample_count; i++) sum += s->samples[i];
    float avg = sum / (float)s->sample_count;
    s->last_effective = avg + s->calib_offset + s->comfort_offset;
    s->last_update_ms = mono_ms();
}

/* Open-window detection (spec 6.1): rate of drop + comparison with others. */
static void window_detect(sensor_t *s, int idx)
{
    if (idx < 0 || idx >= HE_MAX_SENSORS) return;
    if (s->quality != QUAL_OK && s->quality != QUAL_SIMULATED) return;

    if (isnan(s_baseline[idx])) {
        s_baseline[idx] = s->last_effective;
        return;
    }
    float drop = s_baseline[idx] - s->last_effective;

    if (s->window_open) {
        /* Re-enable when temperature recovers within hysteresis of baseline. */
        if (drop < HE_WINDOW_RECOVERY_HYST) {
            s->window_open = false;
            s->quality = s->simulated ? QUAL_SIMULATED : QUAL_OK;
            s_baseline[idx] = s->last_effective;
            ESP_LOGI(TAG, "sensor %d: window closed, restored", s->id);
        }
        return;
    }

    /* Compare this sensor's drop with the average drop of the other active,
     * non-suppressed sensors: only a localised, fast drop indicates a window,
     * avoiding false positives during whole-building cool-down (spec 6.1). */
    float other_drop_sum = 0; int other_n = 0;
    for (int k = 0; k < s_count; k++) {
        if (k == idx) continue;
        sensor_t *o = &s_sensors[k];
        if ((o->quality == QUAL_OK || o->quality == QUAL_SIMULATED) && !o->window_open
            && !isnan(s_baseline[k])) {
            other_drop_sum += (s_baseline[k] - o->last_effective);
            other_n++;
        }
    }
    float other_drop = other_n ? other_drop_sum / other_n : 0;

    if (drop > 1.5f && (drop - other_drop) > 1.0f) {
        s->window_open = true;
        s->quality = QUAL_WINDOW_OPEN;
        ESP_LOGW(TAG, "sensor %d (%s): open window detected", s->id, s->name);
    } else {
        /* slowly adapt baseline toward current to track gradual changes */
        s_baseline[idx] = s_baseline[idx] * 0.95f + s->last_effective * 0.05f;
    }
}

/* Stale / drift detection (spec 4). */
static void stale_detect(sensor_t *s, int idx)
{
    if (idx < 0 || idx >= HE_MAX_SENSORS) return;
    int64_t now = esp_timer_get_time();
    if (isnan(s_prev_eff[idx])) { s_prev_eff[idx] = s->last_effective; s_last_change_us[idx] = now; return; }

    if (fabsf(s->last_effective - s_prev_eff[idx]) > 0.05f) {
        s_prev_eff[idx] = s->last_effective;
        s_last_change_us[idx] = now;
        s_drift_since_us[idx] = 0;
    } else if ((now - s_last_change_us[idx]) > 10LL * 60 * 1000000LL) {
        /* No meaningful change for >10 min. */
        if (s->quality == QUAL_OK) s->quality = QUAL_STALE;
    }
    s_prev_eff[idx] = s->last_effective;
}

/* Parse a response frame: [START][len][payload][crc][END], payload=[count][id,t_hi,t_lo]... */
static bool parse_frame(const uint8_t *buf, int len)
{
    if (len < 4 || buf[0] != HE_FRAME_START || buf[len - 1] != HE_FRAME_END) return false;
    int plen = buf[1];
    if (len != plen + 4) return false;
    if (crc8(buf + 2, plen + 1) != buf[len - 2]) {
        ESP_LOGW(TAG, "frame CRC mismatch");
        return false;
    }
    const uint8_t *p = buf + 2;
    int n = p[0]; p++;
    if (n > s_count + 1) n = s_count + 1;
    for (int i = 0; i < n; i++) {
        uint8_t id = p[0];
        int16_t centi = (int16_t)((p[1] << 8) | p[2]);
        p += 3;
        float t = centi / 100.0f;
        /* route by id: 1..6 internal, 0 external. A disabled external sensor is
         * skipped so a real interface frame can't revive it. */
        sensor_t *target = NULL;
        if (id == 0 && s_external && s_external->active) { target = s_external; }
        else if (id >= 1 && id <= HE_MAX_SENSORS) {
            for (int k = 0; k < s_count; k++) if (s_sensors[k].id == id) { target = &s_sensors[k]; break; }
        }
        if (!target) continue;
        push_reading(target, t);
        if (t < HE_TEMP_MIN_LOGICAL || t > HE_TEMP_MAX_LOGICAL)
            target->quality = QUAL_OUT_OF_RANGE;
        else if (target->active && !target->window_open)
            target->quality = target->simulated ? QUAL_SIMULATED : QUAL_OK;
    }
    return true;
}

static bool request_poll(uint8_t *out, int *outlen)
{
    uint8_t frame[6];
    frame[0] = HE_FRAME_START;
    frame[1] = 1;                 /* payload len */
    frame[2] = HE_CMD_POLL;
    frame[3] = crc8(&frame[2], 1);
    frame[4] = HE_FRAME_END;
    uart_write_bytes(HE_SENSOR_UART, frame, 5);
    /* Read response with a bounded wait. */
    int total = 0, got = 0;
    int64_t deadline = esp_timer_get_time() + 100000; /* 100 ms */
    while (esp_timer_get_time() < deadline && total < (int)sizeof(s_uart_buf)) {
        got = uart_read_bytes(HE_SENSOR_UART, s_uart_buf + total,
                              sizeof(s_uart_buf) - total, pdMS_TO_TICKS(20));
        if (got > 0) {
            total += got;
            if (total >= 4 && s_uart_buf[1] + 4 == total) break;
        }
    }
    if (total < 4) return false;
    *outlen = total;
    memcpy(out, s_uart_buf, total);
    return true;
}

void sensor_manager_poll(void)
{
    uint8_t buf[HE_SENSOR_BUF_SIZE];
    bool got_real = false;
    int outlen = 0;

    /* First, fill simulated sensors from the simulation manager. */
    for (int i = 0; i < s_count; i++) {
        sensor_t *s = &s_sensors[i];
        if (!s->active) { s->quality = QUAL_DISABLED; continue; }
        if (s->simulated) {
            float v = sim_produce_reading(s);
            if (he_isnan(v)) { s->quality = QUAL_TIMEOUT; continue; }
            push_reading(s, v);
            if (v < HE_TEMP_MIN_LOGICAL || v > HE_TEMP_MAX_LOGICAL)
                s->quality = QUAL_OUT_OF_RANGE;
            else
                s->quality = QUAL_SIMULATED;
        }
    }
    /* External sensor honours its active flag exactly like the internal ones:
     * when disabled it produces no reading, reports DISABLED, and does not feed
     * weather compensation or the thermal model (a disabled sensor is ignored). */
    if (s_external) {
        if (!s_external->active) {
            s_external->quality = QUAL_DISABLED;
        } else if (s_external->simulated) {
            float v = sim_produce_reading(s_external);
            if (!he_isnan(v)) { push_reading(s_external, v); s_external->quality = QUAL_SIMULATED; }
        }
    }

    /* If any sensor is still real, poll the hardware interface. */
    bool need_real = false;
    for (int i = 0; i < s_count; i++)
        if (s_sensors[i].active && !s_sensors[i].simulated) { need_real = true; break; }
    if (need_real) got_real = request_poll(buf, &outlen);

    if (need_real) {
        if (got_real) {
            parse_frame(buf, outlen);
        } else {
            /* Interface unreachable: mark real sensors TIMEOUT. */
            for (int i = 0; i < s_count; i++)
                if (s_sensors[i].active && !s_sensors[i].simulated)
                    if ((mono_ms() - s_sensors[i].last_update_ms) > HE_SENSOR_TIMEOUT_SEC * 1000)
                        s_sensors[i].quality = QUAL_TIMEOUT;
        }
    }

    /* Validation passes for real sensors that received data this poll. */
    for (int i = 0; i < s_count; i++) {
        sensor_t *s = &s_sensors[i];
        if (!s->active) {
            s->quality = QUAL_DISABLED;
            /* A disabled sensor can't be in window-open state; clear any stale flag
             * left from when it was active so the UI never shows the ⊗ mark for a
             * sensor that is no longer being polled. */
            s->window_open = false;
            continue;
        }
        if (s->simulated) { continue; }
        if (s->quality == QUAL_OK) {
            if ((mono_ms() - s->last_update_ms) > HE_SENSOR_TIMEOUT_SEC * 1000)
                s->quality = QUAL_TIMEOUT;
        }
    }

    /* Open-window and stale/drift checks (skip external). */
    for (int i = 0; i < s_count; i++) {
        sensor_t *s = &s_sensors[i];
        if (s->quality == QUAL_OK || s->quality == QUAL_SIMULATED) {
            window_detect(s, i);
            if (s->quality == QUAL_OK || s->quality == QUAL_SIMULATED)
                stale_detect(s, i);
        }
    }
}

float sensor_manager_system_temp(void)
{
    float wsum = 0, vsum = 0;
    for (int i = 0; i < s_count; i++) {
        sensor_t *s = &s_sensors[i];
        if (!s->active) continue;
        if (s->quality != QUAL_OK && s->quality != QUAL_SIMULATED) continue;
        if (he_isnan(s->last_effective)) continue;
        wsum += s->weight;
        vsum += s->weight * s->last_effective;
    }
    if (wsum <= 0) return he_nan();
    return vsum / wsum;
}

float sensor_manager_external_temp(void)
{
    if (!s_external || !s_external->active) return he_nan();
    if (s_external->quality != QUAL_OK && s_external->quality != QUAL_SIMULATED)
        return he_nan();
    return s_external->last_effective;
}

int sensor_manager_healthy_count(void)
{
    int n = 0;
    for (int i = 0; i < s_count; i++)
        if (s_sensors[i].quality == QUAL_OK || s_sensors[i].quality == QUAL_SIMULATED) n++;
    return n;
}

void sensor_manager_restore(int id)
{
    for (int i = 0; i < s_count; i++) {
        if (s_sensors[i].id == id) {
            s_sensors[i].window_open = false;
            s_baseline[i] = s_sensors[i].last_effective;
            s_sensors[i].quality = s_sensors[i].simulated ? QUAL_SIMULATED : QUAL_OK;
            ESP_LOGI(TAG, "sensor %d manually restored", id);
            return;
        }
    }
}
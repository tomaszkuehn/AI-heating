#include "heating_output.h"
#include "app_config.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "heatout";

static bool s_relay_state = false;
static bool s_simulate = false;
static bool s_mixed = false;

/* Pump overrun timing state. */
static bool s_pump_active = false;
static int  s_pump_elapsed = 0;        /* ms since overrun window started */
static int  s_pump_phase = 0;          /* ms into current period           */
static pump_overrun_cfg_t s_pump_cfg;

void heating_output_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << HE_GPIO_HEATING),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    heating_output_set_relay(false);
    ESP_LOGI(TAG, "heating relay on GPIO%d", HE_GPIO_HEATING);
}

void heating_output_set_simulation(bool simulate_heating, bool mixed_mode)
{
    s_simulate = simulate_heating;
    s_mixed = mixed_mode;
    if (simulate_heating && !mixed_mode) {
        /* Ensure the physical line is safe while simulating. */
        gpio_set_level(HE_GPIO_HEATING, 0);
    }
}

bool heating_output_is_simulated(void) { return s_simulate && !s_mixed; }

void heating_output_set_relay(bool on)
{
    s_relay_state = on;
    if (s_simulate && !s_mixed) {
        /* Simulation: do not drive physical GPIO (spec 8.3). */
        return;
    }
    gpio_set_level(HE_GPIO_HEATING, on ? 1 : 0);
}

bool heating_output_relay_state(void) { return s_relay_state; }

void heating_output_pump_overrun_start(const pump_overrun_cfg_t *cfg)
{
    if (!cfg || !cfg->enabled) return;
    s_pump_cfg = *cfg;
    s_pump_active = true;
    s_pump_elapsed = 0;
    s_pump_phase = 0;
    ESP_LOGI(TAG, "pump overrun: %ds impulse every %ds for %ds total",
             cfg->impulse_seconds, cfg->period_seconds, cfg->total_seconds);
}

void heating_output_pump_overrun_stop(void)
{
    if (s_pump_active) {
        heating_output_set_relay(false);
        s_pump_active = false;
    }
}

bool heating_output_pump_overrun_tick(int dt_ms)
{
    if (!s_pump_active) return false;
    s_pump_elapsed += dt_ms;
    s_pump_phase += dt_ms;

    int period_ms = s_pump_cfg.period_seconds * 1000;
    int impulse_ms = s_pump_cfg.impulse_seconds * 1000;
    if (period_ms <= 0) period_ms = 120000;
    if (impulse_ms <= 0) impulse_ms = 5000;

    /* Within the current period: ON for the first impulse_ms, OFF afterwards. */
    bool on = (s_pump_phase < impulse_ms);
    heating_output_set_relay(on);

    if (s_pump_phase >= period_ms) s_pump_phase = 0;

    if (s_pump_elapsed >= s_pump_cfg.total_seconds * 1000) {
        heating_output_set_relay(false);
        s_pump_active = false;
        ESP_LOGI(TAG, "pump overrun finished");
        return false;
    }
    return true;
}
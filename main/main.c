/**
 * Application entry point (spec sections 13 & 14).
 *
 * Wires the modules together, seeds a default configuration on first boot,
 * recovers state after restart, arms the watchdog, and runs the control loop
 * in its own task so the heating logic survives web/network failures.
 */
#include "app_config.h"
#include "data_model.h"
#include "storage_manager.h"
#include "sensor_manager.h"
#include "simulation_manager.h"
#include "control_engine.h"
#include "heating_output.h"
#include "fault_manager.h"
#include "network_manager.h"
#include "web_ui_api.h"
#include "notification_manager.h"
#include "profile.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_system.h"

static const char *TAG = "main";
static system_config_t g_cfg;

/* Seed a usable default configuration on first boot. */
static void seed_defaults(system_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->sensor_count = 3;
    const char *names[3] = { "Salon", "Kuchnia", "Sypialnia" };
    for (int i = 0; i < 3; i++) {
        c->sensors[i].id = (uint8_t)(i + 1);
        strncpy(c->sensors[i].name, names[i], HE_NAME_LEN - 1);
        c->sensors[i].active = true;
        c->sensors[i].weight = 1.0f / 3.0f;
        c->sensors[i].calib_offset = 0;
        c->sensors[i].comfort_offset = 0;
        c->sensors[i].quality = QUAL_TIMEOUT;
        c->sensors[i].sim_src = SIM_SRC_REAL;
    }
    c->has_external = true;
    sensor_t *ext = &c->sensors[HE_MAX_SENSORS];
    ext->id = 0; strncpy(ext->name, "Zewnatrz", HE_NAME_LEN - 1);
    ext->active = true; ext->is_external = true; ext->quality = QUAL_TIMEOUT;
    ext->sim_src = SIM_SRC_REAL;

    profile_default(&c->profile);
    c->pump.enabled = false;
    c->pump.impulse_seconds = 5;
    c->pump.period_seconds = 120;
    c->pump.total_seconds = 600;
    c->emergency.enabled = false;
    c->emergency.on_seconds = 900;
    c->emergency.period_seconds = 3600;
    c->fault_grace_sec = 300;
    c->max_on_sec = 14400;
    c->max_on_break_sec = 600;
    strncpy(c->device_name, "Sterownik CO", sizeof(c->device_name) - 1);
    c->wifi_sta_mode = false;
    strncpy(c->wifi_ssid, HE_DEFAULT_AP_SSID, sizeof(c->wifi_ssid) - 1);
    strncpy(c->wifi_pass, HE_DEFAULT_AP_PASS, sizeof(c->wifi_pass) - 1);
}

/* Clamp/repair a loaded configuration that may have been corrupted. */
static void repair_config(system_config_t *c)
{
    if (c->sensor_count < 1) c->sensor_count = 1;
    if (c->sensor_count > HE_MAX_SENSORS) c->sensor_count = HE_MAX_SENSORS;
    int eh; char em[48];
    if (!profile_validate(&c->profile, &eh, em, sizeof(em))) profile_default(&c->profile);
    /* In AP mode a password shorter than 8 chars cannot form a WPA2 key and
     * would silently drop the AP to an open network — repair it to the default
     * so the access point always comes up secured. */
    if (!c->wifi_sta_mode && strlen(c->wifi_pass) < 8) {
        strncpy(c->wifi_pass, HE_DEFAULT_AP_PASS, sizeof(c->wifi_pass) - 1);
        c->wifi_pass[sizeof(c->wifi_pass) - 1] = '\0';
    }
}

static void control_task(void *arg)
{
    esp_task_wdt_add(NULL);
    TickType_t last = xTaskGetTickCount();
    while (1) {
        he_config_lock();
        sensor_manager_poll();
        TickType_t now = xTaskGetTickCount();
        int dt = (int)((now - last) * portTICK_PERIOD_MS);
        last = now;
        if (dt <= 0) dt = HE_CONTROL_TICK_MS;
        control_tick(dt);
        he_config_unlock();
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(HE_CONTROL_TICK_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 heating controller booting");

    /* Config lock must exist before any task can touch the shared config. */
    he_config_lock_init();

    /* Persistent storage (NVS + LittleFS). */
    storage_init();

    /* Load or seed configuration. */
    esp_err_t err = storage_load_config(&g_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no saved config -> seeding defaults");
        seed_defaults(&g_cfg);
        storage_save_config(&g_cfg);
    }
    repair_config(&g_cfg);

    /* Detect unexpected restart (spec 7). */
    esp_reset_reason_t rr = esp_reset_reason();
    if (rr != ESP_RST_POWERON && rr != ESP_RST_SW && rr != ESP_RST_DEEPSLEEP) {
        ESP_LOGW(TAG, "unexpected restart reason=%d", (int)rr);
        storage_log_event(FAULT_RESTART, 2, "unexpected restart detected");
    }

    /* Modules (control layer first, so it is independent of comms). */
    simulation_init();
    sensor_manager_init();
    sensor_manager_bind(g_cfg.sensors, g_cfg.sensor_count,
                        g_cfg.has_external ? &g_cfg.sensors[HE_MAX_SENSORS] : NULL);

    heating_output_init();
    heating_output_set_simulation(g_cfg.simulate_heating, false);

    fault_manager_init();
    fault_manager_bind_config(&g_cfg);

    control_init();
    control_bind_config(&g_cfg);

    notification_init();

    /* Communication / presentation layers. */
    network_init(&g_cfg);
    network_start_reset_button(&g_cfg);
    web_ui_init(&g_cfg);

    /* Control loop (watchdog-guarded). */
    xTaskCreate(control_task, "control", 6144, NULL, 5, NULL);
    ESP_LOGI(TAG, "system ready");
}
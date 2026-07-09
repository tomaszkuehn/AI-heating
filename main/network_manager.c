#include "network_manager.h"
#include "app_config.h"
#include "fault_manager.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "driver/gpio.h"

static const char *TAG = "net";

static EventGroupHandle_t s_evt;
#define BIT_STA_CONNECTED (1 << 0)
static bool s_sta_mode = false;
static int s_retry = 0;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;

static bool s_sntp_started = false;

static void sntp_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "SNTP time acquired");
}

static void start_sntp(void)
{
    if (s_sntp_started) return;
    s_sntp_started = true;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_set_time_sync_notification_cb(sntp_cb);
    esp_sntp_init();
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        fault_manager_set_network_up(false);
        if (s_retry < 10) {
            esp_wifi_connect();
            s_retry++;
            ESP_LOGW(TAG, "STA reconnect attempt %d", s_retry);
        } else {
            xEventGroupClearBits(s_evt, BIT_STA_CONNECTED);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "AP: station connected");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry = 0;
        xEventGroupSetBits(s_evt, BIT_STA_CONNECTED);
        fault_manager_set_network_up(true);
        ESP_LOGI(TAG, "STA got IP");
        start_sntp();
    }
}

static void start_ap(system_config_t *cfg)
{
    /* Guard only the netif creation; the mode/config/start must run every call
     * so a runtime switch or network reset actually brings the AP back up. */
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();
    wifi_config_t wc = {0};
    strncpy((char *)wc.ap.ssid, cfg->wifi_ssid, sizeof(wc.ap.ssid));
    strncpy((char *)wc.ap.password, cfg->wifi_pass, sizeof(wc.ap.password));
    wc.ap.ssid_len = strlen((char *)wc.ap.ssid);
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wc.ap.max_connection = 4;
    if (strlen((char *)wc.ap.password) < 8) wc.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "AP up: SSID=%s", cfg->wifi_ssid);
}

static void start_sta(system_config_t *cfg)
{
    if (!s_sta_netif) s_sta_netif = esp_netif_create_default_wifi_sta();
    s_retry = 0;
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, cfg->wifi_ssid, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, cfg->wifi_pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "STA connecting to SSID=%s", cfg->wifi_ssid);
}

void network_init(system_config_t *cfg)
{
    s_evt = xEventGroupCreate();
    s_sta_mode = cfg->wifi_sta_mode;

    esp_netif_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wic));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    if (s_sta_mode && cfg->wifi_ssid[0]) start_sta(cfg);
    else start_ap(cfg);
}

void network_apply(const system_config_t *cfg)
{
    s_sta_mode = cfg->wifi_sta_mode;
    esp_wifi_stop();
    /* Tear down netifs so the other mode can be created cleanly. */
    if (s_sta_mode && cfg->wifi_ssid[0]) {
        start_sta((system_config_t *)cfg);
    } else {
        start_ap((system_config_t *)cfg);
    }
}

bool network_is_up(void)
{
    if (!s_evt) return false;
    return (xEventGroupGetBits(s_evt) & BIT_STA_CONNECTED) != 0;
}

/* Hardware reset button: hold GPIO0 for 5s => network reset to AP default. */
static void reset_button_task(void *arg)
{
    system_config_t *cfg = (system_config_t *)arg;
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << HE_GPIO_NET_RESET_BTN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    int held_ms = 0;
    while (1) {
        if (gpio_get_level(HE_GPIO_NET_RESET_BTN) == 0) {
            held_ms += 100;
            if (held_ms >= HE_NET_RESET_HOLD_MS) {
                ESP_LOGW(TAG, "net reset button held => AP default");
                he_config_lock();
                storage_reset_network(cfg);
                /* Restart networking in AP mode, keep other settings. */
                network_apply(cfg);
                he_config_unlock();
                held_ms = 0;
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
        } else {
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void network_start_reset_button(system_config_t *cfg)
{
    xTaskCreate(reset_button_task, "netbtn", 3072, cfg, 5, NULL);
}
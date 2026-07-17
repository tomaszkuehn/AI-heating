/**
 * Persistent storage (spec sections 9 & 10).
 *
 * Two backends, used per their ESP-IDF intent:
 *   - NVS  : key/value configuration (network, sensors, weights, offsets,
 *            alarms, modes, flags).
 *   - LittleFS : user files, daily profiles, exports, history & logs — chosen
 *            over SPIFFS for power-loss resilience and logging suitability.
 *
 * Flash-wear mitigation (spec 9/14):
 *   Minute samples stay in a RAM ring buffer (HE_RING_SIZE = 1440 = 24 h) and
 *   are NEVER flushed to flash. Only daily aggregates (one line ~20 bytes per
 *   day) are persisted, so flash erase cycles are reduced by ~3 orders of
 *   magnitude and a power-loss can't corrupt the 24h chart file.
 */
#pragma once

#include "data_model.h"
#include "profile.h"
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Master configuration held in NVS ---- */
typedef struct {
    sensor_t            sensors[HE_MAX_SENSORS + 1]; /* idx 0..HE_MAX_SENSORS-1 = internal; idx HE_MAX_SENSORS = external */
    int                 sensor_count;                /* internal count 1..6 */
    bool                has_external;
    daily_profile_t     profile;
    pump_overrun_cfg_t  pump;
    emergency_cfg_t     emergency;
    notify_cfg_t        notify;
    /* network */
    char                wifi_ssid[32];
    char                wifi_pass[64];
    bool                wifi_sta_mode;               /* false => AP default */
    /* runtime modes persisted across restart */
    bool                simulate_sensors;
    bool                simulate_heating;
    bool                sim_time_accel;              /* x10 virtual clock in sim */
    bool                heating_disabled;            /* manual kill switch */
    /* device identity */
    char                device_name[32];             /* user-configurable system name */
    /* protection limits (configurable via /api/limits) */
    int                 fault_grace_sec;             /* NO_HEAT_RISE grace (default 300 = 5 min) */
    int                 max_on_sec;                  /* max continuous heating (default 14400 = 4 h) */
    int                 max_on_break_sec;           /* forced break after max-on (default 600 = 10 min) */
    /* ---- Features appended at the TAIL for NVS upgrade safety: a short-read blob
     * from older firmware zero-fills these, and repair_config() defaults them. Do
     * NOT reorder or insert fields above this line (would shift the NVS layout and
     * silently corrupt every upgraded device). ---- */
    bool                emergency_on_sensor_fault;  /* opt-in: emergency duty cycle while FAULT_SENSOR_IFACE active */
    uint8_t             notify_ev_ver;              /* 0 = unconfigured (upgrade/first boot) -> repair_config sets defaults */
    bool                notify_ev_faults;           /* e-mail on fault raised (default true)  */
    bool                notify_ev_restart;          /* e-mail on restart (~60 s boot)(default true)  */
    /* User-configurable hysteresis timing (set via /api/limits). Tail-appended
     * for NVS upgrade safety: an older short-read blob zero-fills these to 0,
     * and repair_config() resets them to the HE_DEFAULT_* values. control_engine
     * also falls back to the compile-time HE_MIN_*_SEC when a value is below
     * the floor, so a 0 (upgrade-unset) can never enable instant toggling. */
    int                 min_on_sec;                 /* min continuous ON before OFF allowed (default 90)  */
    int                 min_off_sec;                /* min continuous OFF before ON allowed (default 90) */
    int                 anti_osc_lock_sec;          /* anti-oscillation lock after state change (default 120) */
    /* ---- Dedicated SMTP port (configurable e-mail sending, default 25).
     * Tail-appended for NVS upgrade safety: an older short-read blob zero-fills
     * this to 0, and repair_config() defaults it. A dedicated field avoids
     * overloading the smtp_host string; smtp_host "host:port" still works as a
     * fallback when this is left at 0. Do NOT reorder fields above this line. */
    int                 smtp_port;                 /* explicit SMTP port (0 => use host:port or default 25) */
} system_config_t;

/* One minute sample record (all active sensors + system + external). */
typedef struct {
    int32_t  ts;          /* unix time, minute aligned */
    float    system_temp;
    float    external_temp;
    float    per_sensor[HE_MAX_SENSORS];
    uint8_t  heating_active;   /* 0/1 (per-minute relay state in the ring) */
    uint8_t  state;            /* control_state_t */
    uint16_t heat_mins;        /* daily aggregate: total minutes heated that day
                                  (only meaningful for rows from /api/daily) */
} minute_sample_t;

/* RAM ring-buffer capacity — exactly 24 h of 1-minute samples, never persisted. */
#define HE_RING_SIZE            1440

/* Legacy batch constant (kept for documentation; ring buffer supersedes it). */
#define HE_SAMPLE_FLUSH_BATCH   15

#define HE_LOG_MAX_ENTRIES      100   /* in-RAM event ring (see storage_manager.c) */
#define HE_SAMPLE_RETAIN_DAYS   2     /* keep today+yesterday; prune older    */

/* Flash wear assessment (ESP32 NOR flash: ~100 000 erase cycles / 4 KB sector). */
typedef struct {
    uint32_t nvs_commits;       /* total config saves (NVS commits)           */
    uint32_t fs_kb_written;     /* cumulative KB written to LittleFS          */
    uint32_t fs_total_kb;       /* data-partition size in KB                  */
    uint32_t fs_used_kb;        /* current usage in KB                        */
    uint32_t est_erase_cycles;  /* estimated erase cycles on most-worn sector */
    float    est_erase_pct;     /* estimated endurance used (0-100 %)         */
} storage_flash_wear_t;

/* Lifecycle. */
esp_err_t storage_init(void);

/* Load / save the master config to/from NVS. */
esp_err_t storage_load_config(system_config_t *cfg);
esp_err_t storage_save_config(const system_config_t *cfg);

/* Profile file operations (LittleFS). */
esp_err_t storage_save_profile_file(const char *name, const daily_profile_t *p);
esp_err_t storage_load_profile_file(const char *name, daily_profile_t *p);

/* Minute-sample logging — RAM ring buffer, no flash writes. */
void      storage_record_minute(const minute_sample_t *s);
void      storage_flush_samples(void);
esp_err_t storage_read_samples_24h(minute_sample_t *out, int max, int *count);

/* Direct ring-buffer access for chunked HTTP streaming (avoids a 1440-sample
 * copy buffer in the web layer — saves ~57 KB of DRAM BSS). */
int       storage_ring_count(void);
int       storage_ring_copy(int start, int count, minute_sample_t *out);
esp_err_t storage_read_daily_aggregates(int months, minute_sample_t *out,
                                         int max, int *count);
/* Live aggregate for the CURRENT day from the RAM ring (no flash read).
 * Returns true and fills `out` (ts=today's key, heating_active=minutes heated
 * today) and *out_key (today's YYYYMMDD) only if today has >=1 sample AND the
 * wall clock is valid; append after the historical rows. */
bool      storage_read_daily_current(minute_sample_t *out, int *out_key);

/* Event / alarm log (rate-limited appends). */
typedef struct {
    int32_t ts;
    uint8_t fault;        /* fault_class_t */
    uint8_t severity;     /* 0 info, 1 warn, 2 alarm */
    char    text[96];
} log_entry_t;
esp_err_t storage_log_event(fault_class_t f, int severity, const char *text);
esp_err_t storage_read_log(log_entry_t *out, int max, int *count);
esp_err_t storage_clear_log(void);   /* empty events.log (UI "Wyczyść" button) */

/* True if storage subsystem is healthy (used by health marker). */
bool      storage_healthy(void);

/* LittleFS data-partition usage in bytes (for the flash-usage indicator).
 * Returns ESP_OK and fills total/used, or ESP_FAIL with both zeroed. */
esp_err_t storage_fs_usage(size_t *total, size_t *used);

/* Flash wear estimate: NVS commit count, cumulative KB to LittleFS, estimated
 * erase cycles and endurance percentage based on 100k-cycle rated flash. */
esp_err_t storage_get_flash_wear(storage_flash_wear_t *w);

/* Reset network portion of config only (keeps user settings, spec 12). */
esp_err_t storage_reset_network(system_config_t *cfg);

#ifdef __cplusplus
}
#endif

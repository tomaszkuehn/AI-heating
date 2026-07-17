#include "storage_manager.h"
#include "app_config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "storage";

#define NVS_NS       "heating"
#define NVS_CFG_KEY  "cfg"
#define NVS_NVS_WR   "nvs_writes"    /* cumulative NVS commit count   */
#define NVS_FS_WR    "fs_writes"     /* cumulative KB to LittleFS     */
#define FS_MOUNT     "/storage"
#define FS_SAMPLES   FS_MOUNT "/samples"
#define FS_DAILY     FS_MOUNT "/daily.csv"

/* ESP32 NOR flash endurance parameters (W25Q32 / similar). */
#define FLASH_SECTOR_KB      4        /* 4 KB sector size            */
#define FLASH_RATED_CYCLES   100000   /* min erase cycles per sector */
#define FLASH_WRITE_AMP      2.0f     /* LittleFS write amplification estimate */

static SemaphoreHandle_t s_fs_mutex = NULL;
static bool s_healthy = false;
static bool s_lfs_ok = false;

/* Decouples wear-counter persistence from LittleFS write volume: the RAM total
 * is flushed to NVS at most once per hour (plus on config save), so a busy
 * logger cannot trigger NVS erases proportionally to its own activity. */
#define WEAR_SAVE_PERIOD_US   (3600ULL * 1000 * 1000)   /* 1 hour */
static esp_timer_handle_t s_wear_timer = NULL;
static void save_wear_counters(void);   /* forward decl: defined further below */
static void wear_timer_cb(void *arg)
{
    (void)arg;
    save_wear_counters();
}

/* ---- RAM ring buffer (replaces file-based minute-sample storage) ---- */
static minute_sample_t s_ring[HE_RING_SIZE];
static int s_ring_head = 0;     /* next write index */
static int s_ring_count = 0;    /* total stored (0..HE_RING_SIZE, saturates) */
static int s_cur_day = -1;
static bool s_cur_day_real = false;   /* was s_cur_day recorded against a real
                                      * (SNTP-synced) wall clock? Fake-epoch days
                                      * are never aggregated to daily.csv. */

/* ---- Flash-wear counters (persisted in NVS, live in RAM) ---- */
static uint32_t s_nvs_writes = 0;
static uint32_t s_fs_kb = 0;

/* Forward declarations. */
static void aggregate_day(int key);
static void prune_old_samples(int keep_key);
static void dedup_daily_csv(void);
static int  day_key_from_ts(int32_t ts);
void storage_flush_samples(void);

/* ---- internal helpers ---- */

static void load_wear_counters(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint32_t v;
    size_t s = sizeof(v);
    if (nvs_get_blob(h, NVS_NVS_WR, &v, &s) == ESP_OK) s_nvs_writes = v;
    s = sizeof(v);
    if (nvs_get_blob(h, NVS_FS_WR, &v, &s) == ESP_OK) s_fs_kb = v;
    nvs_close(h);
}

static void save_wear_counters(void)
{
    /* nvs_open/commit are not safe to call from an ISR; the wear timer runs in a
     * timer task (not ISR), but guard with the mutex anyway since
     * storage_save_config may run concurrently from the web task. */
    if (s_fs_mutex) xSemaphoreTake(s_fs_mutex, portMAX_DELAY);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        if (s_fs_mutex) xSemaphoreGive(s_fs_mutex);
        return;
    }
    nvs_set_blob(h, NVS_NVS_WR, &s_nvs_writes, sizeof(s_nvs_writes));
    nvs_set_blob(h, NVS_FS_WR, &s_fs_kb, sizeof(s_fs_kb));
    nvs_commit(h);
    nvs_close(h);
    if (s_fs_mutex) xSemaphoreGive(s_fs_mutex);
}

/* Track LittleFS bytes written: call after every fwrite. Only accumulates the
 * running total in RAM -- it deliberately does NOT persist to NVS here. An early
 * version committed the wear counters to NVS on every 1 KB written, which meant
 * any LittleFS write triggered an NVS erase; that is a feedback loop that
 * accelerates flash wear in proportion to logging activity. The RAM total is
 * flushed to NVS at most once per hour by the wear-timer (and on config save),
 * so NVS commits are decoupled from LittleFS write volume. */
static void account_fs_write(size_t bytes)
{
    static size_t accum = 0;
    accum += bytes;
    if (accum >= 1024) {
        s_fs_kb += (uint32_t)(accum / 1024);
        accum %= 1024;
    }
}

/* ---- Lifecycle ---- */
esp_err_t storage_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_vfs_littlefs_conf_t lfscfg = {
        .format_if_mount_failed = true,
        .partition_label = "storage",
        .base_path = FS_MOUNT,
    };
    ret = esp_vfs_littlefs_register(&lfscfg);
    if (ret == ESP_OK) {
        s_lfs_ok = true;
        mkdir(FS_SAMPLES, 0775);
        ESP_LOGI(TAG, "LittleFS mounted at %s", FS_MOUNT);
    } else {
        s_lfs_ok = false;
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(ret));
    }

    s_fs_mutex = xSemaphoreCreateMutex();
    s_healthy = true;
    load_wear_counters();
    /* Flush the RAM wear counters to NVS at most once per hour, decoupled from
     * LittleFS write volume (see account_fs_write). The timer task owns this. */
    const esp_timer_create_args_t tc = {
        .callback = wear_timer_cb,
        .name = "wear_save",
    };
    if (esp_timer_create(&tc, &s_wear_timer) == ESP_OK)
        esp_timer_start_periodic(s_wear_timer, WEAR_SAVE_PERIOD_US);
    ESP_LOGI(TAG, "Wear counters: NVS=%lu commits, FS=%lu KB",
             (unsigned long)s_nvs_writes, (unsigned long)s_fs_kb);
    /* Collapse any duplicate / out-of-order rows that older firmware wrote to
     * daily.csv while the clock was on the synthetic fallback epoch (synthetic
     * 20231114 days, re-aggregated each boot -> many dupes). Idempotent: leaves a
     * clean file untouched, only rewriting when there is something to fix. */
    if (s_lfs_ok) dedup_daily_csv();
    return ESP_OK;
}

bool storage_healthy(void) { return s_healthy && s_lfs_ok; }

/* ---- Config persistence (NVS) ---- */
esp_err_t storage_load_config(system_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        /* First boot: return zeroed config; caller seeds defaults. */
        memset(cfg, 0, sizeof(*cfg));
        return err;
    }
    size_t needed = sizeof(*cfg);
    err = nvs_get_blob(h, NVS_CFG_KEY, cfg, &needed);
    nvs_close(h);
    if (err != ESP_OK) {
        memset(cfg, 0, sizeof(*cfg));
    } else if (needed < sizeof(*cfg)) {
        /* Short read: the stored blob is from an older firmware whose struct was
         * smaller (e.g. before device_name + the protection limits were appended).
         * nvs_get_blob returned ESP_OK and copied only `needed` bytes, leaving the
         * trailing fields uninitialised. Zero the unread tail so repair_config()
         * sees clean values to clamp instead of stack/BSS garbage; the caller
         * repairs and re-saves. */
        memset((char *)cfg + needed, 0, sizeof(*cfg) - needed);
        ESP_LOGW(TAG, "config blob %u B < struct %u B -- repairing trailing fields",
                 (unsigned)needed, (unsigned)sizeof(*cfg));
    }
    return err;
}

esp_err_t storage_save_config(const system_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, NVS_CFG_KEY, cfg, sizeof(*cfg));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        s_healthy = false;
        ESP_LOGE(TAG, "config save failed: %s", esp_err_to_name(err));
    } else {
        s_nvs_writes++;
        save_wear_counters();
    }
    return err;
}

esp_err_t storage_reset_network(system_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    strncpy(cfg->wifi_ssid, HE_DEFAULT_AP_SSID, sizeof(cfg->wifi_ssid) - 1);
    cfg->wifi_ssid[sizeof(cfg->wifi_ssid) - 1] = '\0';
    strncpy(cfg->wifi_pass, HE_DEFAULT_AP_PASS, sizeof(cfg->wifi_pass) - 1);
    cfg->wifi_pass[sizeof(cfg->wifi_pass) - 1] = '\0';
    cfg->wifi_sta_mode = false;
    return storage_save_config(cfg);
}

/* ---- Profile files (LittleFS) ---- */
static void profile_path(const char *name, char *out, size_t outlen)
{
    snprintf(out, outlen, FS_MOUNT "/profile_%s.json", name ? name : "default");
}

esp_err_t storage_save_profile_file(const char *name, const daily_profile_t *p)
{
    if (!s_lfs_ok || !p) return ESP_FAIL;
    char path[64]; profile_path(name, path, sizeof(path));
    char buf[512];
    size_t n = profile_to_json(p, buf, sizeof(buf));
    if (n == 0) return ESP_FAIL;
    xSemaphoreTake(s_fs_mutex, portMAX_DELAY);
    FILE *f = fopen(path, "w");
    if (!f) { xSemaphoreGive(s_fs_mutex); return ESP_FAIL; }
    size_t w = fwrite(buf, 1, n, f);
    fclose(f);
    if (w > 0) account_fs_write(w);
    xSemaphoreGive(s_fs_mutex);
    return ESP_OK;
}

esp_err_t storage_load_profile_file(const char *name, daily_profile_t *p)
{
    if (!s_lfs_ok || !p) return ESP_FAIL;
    char path[64]; profile_path(name, path, sizeof(path));
    xSemaphoreTake(s_fs_mutex, portMAX_DELAY);
    FILE *f = fopen(path, "r");
    if (!f) { xSemaphoreGive(s_fs_mutex); return ESP_FAIL; }
    char buf[512]; size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    xSemaphoreGive(s_fs_mutex);
    buf[n] = '\0';
    return profile_from_json(p, buf, n) ? ESP_OK : ESP_FAIL;
}

/* ---- Minute samples (RAM ring buffer, zero flash writes) ---- */
static int day_key_from_ts(int32_t ts)
{
    time_t t = ts;
    struct tm tm; gmtime_r(&t, &tm);
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

void storage_record_minute(const minute_sample_t *s)
{
    if (!s) return;
    int day = day_key_from_ts(s->ts);
    bool real = he_time_valid();   /* SNTP-synced => a real wall-clock day */

    if (s_cur_day == -1) {
        s_cur_day = day;
        s_cur_day_real = real;
    } else if (day != s_cur_day) {
        /* Day rollover: persist the OLD day's aggregate only if it was recorded
         * against a real (SNTP-synced) wall clock. When the clock ran on the
         * synthetic fallback epoch (offline / accelerated sim), the day key is a
         * fake 20231114 that resets every boot -- aggregating it would pollute
         * daily.csv with duplicate, out-of-order rows (the bug that previously
         * filled the energy chart with 2023 garbage). The RAM ring still holds
         * those samples for the 24h chart; only the long-term daily log is skipped. */
        if (s_cur_day_real) {
            aggregate_day(s_cur_day);
            prune_old_samples(day_key_from_ts(s->ts - HE_SAMPLE_RETAIN_DAYS * 86400));
        }
        s_cur_day = day;
        s_cur_day_real = real;
    }

    xSemaphoreTake(s_fs_mutex, portMAX_DELAY);
    s_ring[s_ring_head] = *s;
    s_ring_head = (s_ring_head + 1) % HE_RING_SIZE;
    if (s_ring_count < HE_RING_SIZE) s_ring_count++;
    xSemaphoreGive(s_fs_mutex);
}

/* No-op: samples are never flushed to flash. Kept for API compatibility with
 * control_engine day-rollover and the old public header. */
void storage_flush_samples(void) { /* intentionally empty */ }

esp_err_t storage_read_samples_24h(minute_sample_t *out, int max, int *count)
{
    if (!out || !count) return ESP_FAIL;
    *count = storage_ring_copy(0, max, out);
    return ESP_OK;
}

int storage_ring_count(void)
{
    int c;
    xSemaphoreTake(s_fs_mutex, portMAX_DELAY);
    c = s_ring_count;
    xSemaphoreGive(s_fs_mutex);
    return c;
}

int storage_ring_copy(int start, int count, minute_sample_t *out)
{
    if (!out || count <= 0) return 0;
    xSemaphoreTake(s_fs_mutex, portMAX_DELAY);

    /* Clamp to what's actually in the ring. */
    if (start < 0) start = 0;
    if (start >= s_ring_count) { xSemaphoreGive(s_fs_mutex); return 0; }
    int avail = s_ring_count - start;
    if (count > avail) count = avail;

    /* Map logical index (0 = oldest) to physical ring array index. */
    int base;
    if (s_ring_count < HE_RING_SIZE) {
        base = start;          /* buffer hasn't wrapped yet */
    } else {
        base = (s_ring_head + start) % HE_RING_SIZE;
    }

    for (int i = 0; i < count; i++) {
        out[i] = s_ring[(base + i) % HE_RING_SIZE];
    }

    xSemaphoreGive(s_fs_mutex);
    return count;
}

esp_err_t storage_fs_usage(size_t *total, size_t *used)
{
    if (total) *total = 0;
    if (used)  *used = 0;
    if (!s_lfs_ok) return ESP_FAIL;
    return esp_littlefs_info("storage", total, used);
}

esp_err_t storage_get_flash_wear(storage_flash_wear_t *w)
{
    if (!w) return ESP_ERR_INVALID_ARG;
    memset(w, 0, sizeof(*w));

    size_t total = 0, used = 0;
    storage_fs_usage(&total, &used);

    w->nvs_commits  = s_nvs_writes;
    w->fs_kb_written = s_fs_kb;
    w->fs_total_kb  = (uint32_t)(total / 1024);
    w->fs_used_kb   = (uint32_t)(used / 1024);

    /* Estimate: each KB written touches 1/4 of a sector (4 KB), times write
     * amplification (~2×) for LittleFS metadata & garbage collection. */
    w->est_erase_cycles = (uint32_t)((float)s_fs_kb / (float)FLASH_SECTOR_KB
                                     * FLASH_WRITE_AMP);
    w->est_erase_pct    = (float)w->est_erase_cycles
                          / (float)FLASH_RATED_CYCLES * 100.0f;

    return ESP_OK;
}

/* Collapse daily.csv to one row per day key, sorted ascending. Removes the
 * duplicate and out-of-order rows that older firmware wrote while the clock ran
 * on the synthetic fallback epoch: the fake 20231114 day was re-aggregated every
 * boot, so the file accumulated many copies and the energy chart showed 2023
 * garbage. Idempotent: a file that is already one-row-per-day and ascending is
 * left untouched (no needless flash write on a clean boot). Called once at init;
 * bounded -- one row/day, a decade is ~3.6 KB of rows (cap 400, matches the web
 * layer's s_daily[400] history buffer). */
static void dedup_daily_csv(void)
{
    xSemaphoreTake(s_fs_mutex, portMAX_DELAY);
    FILE *f = fopen(FS_DAILY, "r");
    if (!f) { xSemaphoreGive(s_fs_mutex); return; }

    /* Parse every row, keeping the LAST aggregate for each day key (last wins). */
    static int   keys[400];
    static float syss[400], exts[400];
    static int   heats[400];
    int n = 0, total = 0;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int key, heat = 0; float sys, ext;
        if (sscanf(line, "%d,%f,%f,%d", &key, &sys, &ext, &heat) < 3) continue;
        total++;
        int found = -1;
        for (int i = 0; i < n; i++) if (keys[i] == key) { found = i; break; }
        if (found >= 0) { syss[found] = sys; exts[found] = ext; heats[found] = heat; }
        else if (n < 400) { keys[n] = key; syss[n] = sys; exts[n] = ext; heats[n] = heat; n++; }
    }
    fclose(f);

    /* Only rewrite if there is something to fix: duplicates removed OR rows out of
     * ascending order. A clean file is left untouched to avoid a flash write every boot. */
    bool unsorted = false;
    for (int i = 1; i < n; i++) if (keys[i] < keys[i - 1]) { unsorted = true; break; }
    if (n == total && !unsorted) { xSemaphoreGive(s_fs_mutex); return; }

    /* Insertion sort by day key ascending (n is tiny -- one row/day). */
    for (int i = 1; i < n; i++) {
        int k = keys[i]; float sv = syss[i], ev = exts[i]; int hv = heats[i]; int j = i - 1;
        while (j >= 0 && keys[j] > k) {
            keys[j + 1] = keys[j]; syss[j + 1] = syss[j]; exts[j + 1] = exts[j]; heats[j + 1] = heats[j]; j--;
        }
        keys[j + 1] = k; syss[j + 1] = sv; exts[j + 1] = ev; heats[j + 1] = hv;
    }

    FILE *w = fopen(FS_DAILY, "w");
    if (!w) { xSemaphoreGive(s_fs_mutex); return; }
    size_t wr = 0;
    for (int i = 0; i < n; i++) {
        char buf[64];
        int L = snprintf(buf, sizeof(buf), "%d,%.2f,%.2f,%d\n",
                         keys[i], syss[i], exts[i], heats[i]);
        if (L > 0) wr += fwrite(buf, 1, (size_t)L, w);
    }
    fclose(w);
    if (wr > 0) account_fs_write(wr);
    xSemaphoreGive(s_fs_mutex);
    ESP_LOGI(TAG, "daily.csv deduped (%d -> %d rows)", total, n);
}

/* Compute one daily aggregate from ring-buffer samples for a given day key,
 * then append a single line to daily.csv (~20 bytes). */
static void aggregate_day(int key)
{
    if (!s_lfs_ok || key <= 0) return;

    xSemaphoreTake(s_fs_mutex, portMAX_DELAY);

    double sum_sys = 0, sum_ext = 0;
    int n = 0, ext_n = 0, heat_mins = 0;

    /* Scan the ring buffer for samples belonging to the target day. */
    int start = (s_ring_count < HE_RING_SIZE) ? 0 : s_ring_head;
    for (int i = 0; i < s_ring_count; i++) {
        int idx = (start + i) % HE_RING_SIZE;
        if (day_key_from_ts(s_ring[idx].ts) != key) continue;
        if (he_isnan(s_ring[idx].system_temp)) continue;
        sum_sys += s_ring[idx].system_temp;
        n++;
        if (s_ring[idx].heating_active) heat_mins++;
        if (!he_isnan(s_ring[idx].external_temp)) {
            sum_ext += s_ring[idx].external_temp;
            ext_n++;
        }
    }

    xSemaphoreGive(s_fs_mutex);

    if (n == 0) return;

    /* Re-acquire for the daily.csv append (short critical section). */
    xSemaphoreTake(s_fs_mutex, portMAX_DELAY);
    FILE *d = fopen(FS_DAILY, "a");
    if (d) {
        int w = fprintf(d, "%d,%.2f,%.2f,%d\n", key,
                        sum_sys / (double)n,
                        ext_n ? sum_ext / (double)ext_n : -99.0,
                        heat_mins);
        fclose(d);
        if (w > 0) account_fs_write((size_t)w);
    }
    xSemaphoreGive(s_fs_mutex);
}

/* Delete per-day sample files whose YYYYMMDD key predates keep_key.
 * Since minute samples are no longer written to files, this mainly cleans
 * up legacy data from older firmware versions. */
static void prune_old_samples(int keep_key)
{
    DIR *dir = opendir(FS_SAMPLES);
    if (!dir) return;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        int key = atoi(de->d_name);
        if (key > 0 && key < keep_key) {
            char path[sizeof(FS_SAMPLES) + 260];
            snprintf(path, sizeof(path), FS_SAMPLES "/%s", de->d_name);
            if (remove(path) == 0)
                ESP_LOGI(TAG, "pruned old sample file %s", de->d_name);
        }
    }
    closedir(dir);
}

esp_err_t storage_read_daily_aggregates(int months, minute_sample_t *out,
                                        int max, int *count)
{
    if (!s_lfs_ok || !out || !count) return ESP_FAIL;
    *count = 0;
    xSemaphoreTake(s_fs_mutex, portMAX_DELAY);
    FILE *f = fopen(FS_DAILY, "r");
    if (!f) { xSemaphoreGive(s_fs_mutex); return ESP_OK; }
    /* Read all, then keep the last (months*30) days. */
    char line[128];
    int total = 0;
    /* First pass: count. */
    while (fgets(line, sizeof(line), f)) total++;
    rewind(f);
    int skip = total - (months * 30);
    if (skip < 0) skip = 0;
    int idx = 0;
    while (fgets(line, sizeof(line), f)) {
        if (idx++ < skip) continue;
        if (*count >= max) break;
        int key, heat = 0; float sys, ext;
        int nf = sscanf(line, "%d,%f,%f,%d", &key, &sys, &ext, &heat);
        if (nf >= 3) {
            out[*count].ts = key;
            out[*count].system_temp = sys;
            out[*count].external_temp = (ext < -98.0f) ? he_nan() : ext;
            out[*count].heat_mins = (nf >= 4) ? (uint16_t)heat : 0;
            (*count)++;
        }
    }
    fclose(f);
    xSemaphoreGive(s_fs_mutex);
    return ESP_OK;
}

/* Compute the live aggregate for the CURRENT calendar day straight from the
 * in-RAM minute ring (no flash read), so the daily chart can show today's
 * heating minutes updating in real time and stay as the right-most column.
 * Returns true (and fills out, and *out_key with today's YYYYMMDD key) only if
 * at least one sample exists for today; the caller appends it after the
 * historical rows from daily.csv. */
bool storage_read_daily_current(minute_sample_t *out, int *out_key)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (out_key) *out_key = 0;
    if (!he_time_valid()) return false;            /* no real wall-clock day yet */
    int today = day_key_from_ts((int32_t)time(NULL));
    if (out_key) *out_key = today;
    xSemaphoreTake(s_fs_mutex, portMAX_DELAY);
    int start = (s_ring_count < HE_RING_SIZE) ? 0 : s_ring_head;
    double sum_sys = 0, sum_ext = 0;
    int n = 0, ext_n = 0, heat_mins = 0;
    for (int i = 0; i < s_ring_count; i++) {
        int idx = (start + i) % HE_RING_SIZE;
        if (day_key_from_ts(s_ring[idx].ts) != today) continue;
        if (he_isnan(s_ring[idx].system_temp)) continue;
        sum_sys += s_ring[idx].system_temp;
        n++;
        if (s_ring[idx].heating_active) heat_mins++;
        if (!he_isnan(s_ring[idx].external_temp)) {
            sum_ext += s_ring[idx].external_temp;
            ext_n++;
        }
    }
    xSemaphoreGive(s_fs_mutex);
    if (n == 0) return false;
    out->ts = today;
    out->system_temp = (float)(sum_sys / (double)n);
    out->external_temp = ext_n ? (float)(sum_ext / (double)ext_n) : he_nan();
    out->heat_mins = (uint16_t)heat_mins;   /* minutes heated so far today */
    return true;
}

/* ---- Event / alarm log (in-RAM ring buffer) ---- */
/* The event log is short-lived operational history (restarts, faults) shown in
 * the UI; it does NOT need to survive a reboot. Keeping it in RAM instead of
 * LittleFS eliminates all flash writes for logging -- important because the
 * previous file-backed design appended on every event (and compacted on
 * overflow), which both wore flash and risked a write-storm if an event fired
 * inside a boot loop. The ring holds the most recent HE_LOG_MAX_ENTRIES events;
 * older ones age out silently. */
#define HE_LOG_MAX_ENTRIES   100

static log_entry_t s_log_ring[HE_LOG_MAX_ENTRIES];
static int s_log_head = 0;     /* next write slot */
static int s_log_count = 0;    /* total valid (0..HE_LOG_MAX_ENTRIES) */

esp_err_t storage_log_event(fault_class_t f, int severity, const char *text)
{
    if (!text) return ESP_FAIL;
    xSemaphoreTake(s_fs_mutex, portMAX_DELAY);
    log_entry_t *e = &s_log_ring[s_log_head];
    e->ts = (int32_t)time(NULL);
    e->fault = (uint8_t)f;
    e->severity = (uint8_t)severity;
    strncpy(e->text, text, sizeof(e->text) - 1);
    e->text[sizeof(e->text) - 1] = '\0';
    char *nl = strchr(e->text, '\n'); if (nl) *nl = '\0';
    s_log_head = (s_log_head + 1) % HE_LOG_MAX_ENTRIES;
    if (s_log_count < HE_LOG_MAX_ENTRIES) s_log_count++;
    xSemaphoreGive(s_fs_mutex);
    return ESP_OK;
}

esp_err_t storage_read_log(log_entry_t *out, int max, int *count)
{
    if (!out || !count) return ESP_FAIL;
    *count = 0;
    xSemaphoreTake(s_fs_mutex, portMAX_DELAY);
    /* Oldest entry is at (head - count + ENTRIES) % ENTRIES; emit the most
     * recent `max` of them in chronological order. */
    int start = s_log_head - s_log_count;
    if (start < 0) start += HE_LOG_MAX_ENTRIES;
    int avail = s_log_count;
    int skip = avail - max; if (skip < 0) skip = 0;
    for (int i = skip; i < avail && *count < max; i++) {
        int idx = (start + i) % HE_LOG_MAX_ENTRIES;
        out[(*count)++] = s_log_ring[idx];
    }
    xSemaphoreGive(s_fs_mutex);
    return ESP_OK;
}

/* Empty the event/alarm log. In-RAM now, so this just resets the ring (no flash
 * erase). Used by the "Wyczyść" UI button. */
esp_err_t storage_clear_log(void)
{
    xSemaphoreTake(s_fs_mutex, portMAX_DELAY);
    s_log_head = 0;
    s_log_count = 0;
    xSemaphoreGive(s_fs_mutex);
    ESP_LOGI(TAG, "events log cleared (RAM)");
    return ESP_OK;
}

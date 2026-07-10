#include "notification_manager.h"
#include "fault_manager.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "notify";

/* Diagnostic buffer for the last SMTP attempt. */
static char s_diag[1024];
static int  s_diag_len = 0;

static void diag_append(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void diag_append(const char *fmt, ...)
{
    if (s_diag_len >= (int)sizeof(s_diag) - 80) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s_diag + s_diag_len, sizeof(s_diag) - s_diag_len, fmt, ap);
    va_end(ap);
    if (n > 0) s_diag_len += n;
    if (s_diag_len >= (int)sizeof(s_diag) - 1) s_diag_len = (int)sizeof(s_diag) - 1;
}

/* ---- Off-loop delivery queue (CODE_REVIEW #2 / #6) ----
 * SMTP/SMS are blocking (seconds of DNS + TCP + SMTP handshake). Running them on
 * the watchdog-subscribed control loop trips TWDT when the mail server is slow or
 * unreachable. Trigger sites (control_engine restart, fault_manager alert) snapshot
 * the needed fields under he_config_lock and enqueue a command here; a dedicated
 * worker task performs the blocking I/O off the loop and is NOT subscribed to the
 * task WDT. Queue depth 2 lets one alert queue behind a still-retrying restart. */
typedef enum { NOTIFY_RESTART, NOTIFY_ALERT } notify_kind_t;

typedef struct {
    char     device_name[HE_NAME_LEN];
    float    sys_temp;
    float    ext_temp;
    int      healthy;
    int      total;
    sensor_t sensors[HE_MAX_SENSORS + 1];  /* [0..n-1] internal; [HE_MAX_SENSORS] external */
    int      sensor_count;
    bool     has_external;
} notify_restart_t;

typedef struct {
    fault_class_t fault;
    char          message[96];
} notify_alert_t;

typedef struct {
    notify_kind_t kind;
    notify_cfg_t  cfg;
    union {
        notify_restart_t restart;
        notify_alert_t   alert;
    } u;
} notify_cmd_t;

#define NOTIFY_QUEUE_LEN 2
static QueueHandle_t s_notify_queue = NULL;

/* base64-encode `in` (NUL-terminated) into `out`; returns out. */
static char *b64(const char *in, char *out, size_t outlen)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = strlen(in), o = 0;
    for (size_t i = 0; i < n && o + 4 < outlen; i += 3) {
        unsigned v = (unsigned char)in[i] << 16;
        int rem = (int)(n - i);
        if (rem > 1) v |= (unsigned char)in[i + 1] << 8;
        if (rem > 2) v |= (unsigned char)in[i + 2];
        out[o++] = tbl[(v >> 18) & 0x3F];
        out[o++] = tbl[(v >> 12) & 0x3F];
        out[o++] = (rem > 1) ? tbl[(v >> 6) & 0x3F] : '=';
        out[o++] = (rem > 2) ? tbl[v & 0x3F] : '=';
    }
    out[o] = '\0';
    return out;
}

/* Percent-encode everything but RFC3986 unreserved chars (for URL query use). */
static void url_encode(const char *in, char *out, size_t outlen)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 4 < outlen; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0F];
        }
    }
    out[o] = '\0';
}

/* Copy `in` into `out` dropping CR/LF so it can't inject SMTP header/command
 * lines (email_to and subject come partly from config / device name). Applied to
 * header fields only -- the body is sent raw to preserve its line structure. */
static void sanitize_line(const char *in, char *out, size_t outlen)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < outlen; i++) {
        char c = in[i];
        if (c == '\r' || c == '\n') c = ' ';
        out[o++] = c;
    }
    out[o] = '\0';
}

/* Minimal best-effort SMTP submission over plain TCP (AUTH LOGIN supported).
 * Intentionally simple: failures are logged and swallowed. */
static bool smtp_send(const notify_cfg_t *c, const char *subject, const char *body)
{
    if (!c->smtp_host[0]) return false;
    char host[80]; int port = 25;
    /* Allow "host:port" in smtp_host. */
    strncpy(host, c->smtp_host, sizeof(host) - 1); host[sizeof(host) - 1] = '\0';
    char *colon = strchr(host, ':');
    if (colon) { *colon = '\0'; port = atoi(colon + 1); }

    const struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        diag_append("FAIL: cannot resolve host %s\n", host);
        ESP_LOGW(TAG, "SMTP: cannot resolve %s", host);
        return false;
    }
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { diag_append("FAIL: socket() error\n"); freeaddrinfo(res); return false; }
    struct sockaddr_in dest = *(struct sockaddr_in *)res->ai_addr;
    dest.sin_port = htons(port);
    int to_ms = 4000; setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &to_ms, sizeof(to_ms));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &to_ms, sizeof(to_ms));
    diag_append("Connecting to %s:%d ...\n", host, port);
    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        diag_append("FAIL: connect refused/timeout\n");
        ESP_LOGW(TAG, "SMTP: connect failed");
        close(sock); freeaddrinfo(res); return false;
    }
    freeaddrinfo(res);

    char rx[256]; char tx[512];
    #define RECV() do { int n = recv(sock, rx, sizeof(rx)-1, 0); if (n<=0) { diag_append("RECV failed (timeout/close)\n"); goto done; } rx[n]=0; diag_append("S: %s", rx); } while (0)
    #define SEND(s) do { int L=strlen(s); if (send(sock,s,L,0)!=L) { diag_append("SEND failed\n"); goto done; } diag_append("C: %s", s); } while (0)

    /* Sanitise header fields only (config / device-name supplied, must not inject
     * CRLF). The body is sent raw in a separate SEND() below so its \r\n line
     * structure is preserved and it cannot overflow tx (the restart body can
     * approach 512 B). Safe because device_name is CRLF-free by write-time
     * validation in the web layer (CODE_REVIEW #3). */
    char rcpt[64], subj[128], b64buf[128];
    sanitize_line(c->email_to, rcpt, sizeof(rcpt));
    sanitize_line(subject, subj, sizeof(subj));

    RECV();
    SEND("EHLO heating\r\n"); RECV();
    if (c->smtp_user[0] && c->smtp_pass[0]) {
        SEND("AUTH LOGIN\r\n"); RECV();
        SEND(b64(c->smtp_user, b64buf, sizeof(b64buf))); SEND("\r\n"); RECV();
        SEND(b64(c->smtp_pass, b64buf, sizeof(b64buf))); SEND("\r\n"); RECV();
    }
    snprintf(tx, sizeof(tx), "MAIL FROM:<heating@esp32.local>\r\n"); SEND(tx); RECV();
    snprintf(tx, sizeof(tx), "RCPT TO:<%s>\r\n", rcpt); SEND(tx); RECV();
    SEND("DATA\r\n"); RECV();
    /* Headers, then the raw body, then the DATA terminator. */
    snprintf(tx, sizeof(tx),
             "From: heating@esp32.local\r\nTo: %s\r\nSubject: %s\r\n\r\n",
             rcpt, subj);
    SEND(tx);
    if (body && body[0]) SEND(body);
    SEND("\r\n.\r\n"); RECV();
    SEND("QUIT\r\n");
    close(sock);
    diag_append("OK: email accepted by server\n");
    ESP_LOGI(TAG, "email sent to %s: %s", rcpt, subj);
    return true;
done:
    close(sock);
    diag_append("FAIL: SMTP transaction incomplete\n");
    return false;
#undef RECV
#undef SEND
}

/* Minimal HTTP POST to an SMS/email-to-SMS gateway. */
static bool sms_send(const notify_cfg_t *c, const char *message)
{
    if (!c->sms_gateway[0]) return false;
    /* gateway is expected as http://host/path?phone=... */
    char host[80]; char path[160]; int port = 80;
    const char *url = c->sms_gateway;
    if (strncmp(url, "http://", 7) == 0) url += 7;
    const char *slash = strchr(url, '/');
    if (slash) {
        size_t hl = slash - url; if (hl >= sizeof(host)) hl = sizeof(host)-1;
        memcpy(host, url, hl); host[hl] = '\0';
        strncpy(path, slash, sizeof(path)-1); path[sizeof(path)-1] = '\0';
    } else {
        strncpy(host, url, sizeof(host)-1); host[sizeof(host)-1] = '\0';
        strcpy(path, "/");
    }
    char *colon = strchr(host, ':');
    if (colon) { *colon = '\0'; port = atoi(colon + 1); }

    const struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return false;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { freeaddrinfo(res); return false; }
    struct sockaddr_in dest = *(struct sockaddr_in *)res->ai_addr;
    dest.sin_port = htons(port);
    int to_ms = 4000; setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &to_ms, sizeof(to_ms));
    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        close(sock); freeaddrinfo(res); return false;
    }
    freeaddrinfo(res);
    char req[512];
    char phone_e[80], msg_e[256];
    url_encode(c->sms_phone, phone_e, sizeof(phone_e));
    url_encode(message, msg_e, sizeof(msg_e));
    int n = snprintf(req, sizeof(req),
        "GET %s&phone=%s&msg=%s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
        path, phone_e, msg_e, host);
    send(sock, req, n, 0);
    char rx[256]; recv(sock, rx, sizeof(rx)-1, 0);
    close(sock);
    ESP_LOGI(TAG, "SMS sent to %s via %s", c->sms_phone, host);
    return true;
}

/* Worker: drains the queue, performing blocking SMTP/SMS off the control loop.
 * Not subscribed to esp_task_wdt (it may block for seconds on network I/O). */
static void notify_worker_task(void *arg)
{
    notify_cmd_t cmd;
    while (1) {
        if (xQueueReceive(s_notify_queue, &cmd, portMAX_DELAY) != pdPASS) continue;
        if (cmd.kind == NOTIFY_RESTART) {
            /* Wait up to 30 s for Wi-Fi to come up before sending the restart
             * notice, so an early-boot dispatch doesn't fail outright (#6 retry). */
            int64_t deadline = esp_timer_get_time() + 30 * 1000000LL;
            while (!fault_manager_network_up() && esp_timer_get_time() < deadline)
                vTaskDelay(pdMS_TO_TICKS(500));
            notification_send_restart(&cmd.cfg, cmd.u.restart.device_name,
                cmd.u.restart.sys_temp, cmd.u.restart.ext_temp,
                cmd.u.restart.healthy, cmd.u.restart.total,
                cmd.u.restart.sensors, cmd.u.restart.sensor_count,
                cmd.u.restart.has_external);
        } else { /* NOTIFY_ALERT */
            notification_send_alert(&cmd.cfg, cmd.u.alert.fault, cmd.u.alert.message);
        }
    }
}

void notification_init(void)
{
    s_notify_queue = xQueueCreate(NOTIFY_QUEUE_LEN, sizeof(notify_cmd_t));
    if (!s_notify_queue) {
        ESP_LOGE(TAG, "failed to create notify queue -- notifications disabled");
        return;
    }
    /* prio 4 < control's 5; NOT subscribed to esp_task_wdt (blocks on SMTP/SMS).
     * Stack must hold notify_cmd_t (~1.2 KB, lives here for the whole send) plus
     * notification_send_restart (body[512]+subject) -> smtp_send (tx[512]+rx[256])
     * -> lwip getaddrinfo/connect. The old code sent from the 6144-B control task
     * WITHOUT the cmd on its stack; the worker adds that, so 5120 overflowed
     * (observed: stack-overflow reset right after the first restart email). 10240
     * gives comfortable margin; heap cost (~5 KB) is trivial vs the ~220 KB pool. */
    xTaskCreate(notify_worker_task, "notify", 10240, NULL, 4, NULL);
    ESP_LOGI(TAG, "notification manager ready (off-loop worker)");
}

void notification_dispatch_alert(const notify_cfg_t *cfg, fault_class_t f,
                                 const char *message)
{
    if (!s_notify_queue || !cfg || !message) return;
    notify_cmd_t cmd = {0};
    cmd.kind = NOTIFY_ALERT;
    cmd.cfg = *cfg;
    cmd.u.alert.fault = f;
    strncpy(cmd.u.alert.message, message, sizeof(cmd.u.alert.message) - 1);
    if (xQueueSend(s_notify_queue, &cmd, 0) != pdPASS)
        ESP_LOGW(TAG, "notify queue full -- alert dropped");
}

void notification_dispatch_restart(const notify_cfg_t *cfg, const char *device_name,
    float sys_temp, float ext_temp, int healthy, int total,
    const sensor_t *sensors, int sensor_count, bool has_external)
{
    if (!s_notify_queue || !cfg || !device_name || !sensors) return;
    if (!cfg->email_enabled && !cfg->sms_enabled) return;
    notify_cmd_t cmd = {0};
    cmd.kind = NOTIFY_RESTART;
    cmd.cfg = *cfg;
    notify_restart_t *r = &cmd.u.restart;
    strncpy(r->device_name, device_name, sizeof(r->device_name) - 1);
    r->sys_temp = sys_temp;
    r->ext_temp = ext_temp;
    r->healthy = healthy;
    r->total = total;
    int n = sensor_count;
    if (n < 0) n = 0;
    if (n > HE_MAX_SENSORS) n = HE_MAX_SENSORS;
    for (int i = 0; i < n; i++) r->sensors[i] = sensors[i];
    r->sensor_count = n;
    r->has_external = has_external;
    if (has_external) r->sensors[HE_MAX_SENSORS] = sensors[HE_MAX_SENSORS];
    if (xQueueSend(s_notify_queue, &cmd, 0) != pdPASS)
        ESP_LOGW(TAG, "notify queue full -- restart notice dropped");
}

void notification_send_alert(const notify_cfg_t *cfg, fault_class_t f,
                             const char *message)
{
    if (!cfg || !message) return;
    char subject[64];
    snprintf(subject, sizeof(subject), "[Heating] fault %d", (int)f);
    s_diag[0] = '\0'; s_diag_len = 0;
    if (cfg->email_enabled && cfg->email_to[0]) {
        if (!smtp_send(cfg, subject, message))
            ESP_LOGW(TAG, "email send failed");
    }
    if (cfg->sms_enabled && cfg->sms_phone[0]) {
        if (!sms_send(cfg, message))
            ESP_LOGW(TAG, "sms send failed");
    }
}

void notification_send_restart(const notify_cfg_t *cfg, const char *device_name,
    float sys_temp, float ext_temp, int healthy, int total,
    const sensor_t *sensors, int sensor_count, bool has_external)
{
    if (!cfg || !device_name || !sensors) return;
    if (!cfg->email_enabled && !cfg->sms_enabled) return;

    /* Subject: RFC 2047 encoded-word when the device name holds non-ASCII (Polish
     * diacritics) so strict MTAs don't mangle or reject the header. The brackets
     * and "RESTART" stay plain (the subject is an unstructured field). (#13) */
    char subject[128];
    bool ascii = true;
    for (const char *p = device_name; *p; p++)
        if ((unsigned char)*p >= 0x80) { ascii = false; break; }
    if (ascii) {
        snprintf(subject, sizeof(subject), "[%s] RESTART", device_name);
    } else {
        char enc[64];
        b64(device_name, enc, sizeof(enc));
        snprintf(subject, sizeof(subject), "[=?UTF-8?B?%s?=] RESTART", enc);
    }

    /* Email body with detailed system status. */
    char body[512];
    int pos = 0;
    pos += snprintf(body + pos, sizeof(body) - pos,
        "Urzadzenie: %s\r\n"
        "Temp. systemowa: %.2f C\r\n"
        "Temp. zewnetrzna: %s\r\n"
        "Czujniki OK: %d/%d\r\n"
        "\r\n",
        device_name,
        he_isnan(sys_temp) ? -99.0f : sys_temp,
        he_isnan(ext_temp) ? "--" : "",
        healthy, total);
    if (pos < 0) pos = 0;
    if (!he_isnan(ext_temp)) {
        pos += snprintf(body + pos, sizeof(body) - pos,
            "                        %.2f C\r\n", ext_temp);
    }
    for (int i = 0; i < sensor_count && pos + 64 < (int)sizeof(body); i++) {
        const sensor_t *s = &sensors[i];
        pos += snprintf(body + pos, sizeof(body) - pos,
            "Czujnik %d (%s): %s, %.2f C\r\n",
            s->id, s->name[0] ? s->name : "?",
            sensor_quality_name(s->quality),
            he_isnan(s->last_effective) ? -99.0f : s->last_effective);
    }
    /* External sensor (id 0) lives at sensors[HE_MAX_SENSORS]; report it when
     * configured so the restart notice includes the outdoor reading. (#14) */
    if (has_external && pos + 64 < (int)sizeof(body)) {
        const sensor_t *e = &sensors[HE_MAX_SENSORS];
        pos += snprintf(body + pos, sizeof(body) - pos,
            "Czujnik zew. (%s): %s, %.2f C\r\n",
            e->name[0] ? e->name : "Zewnatrz",
            sensor_quality_name(e->quality),
            he_isnan(e->last_effective) ? -99.0f : e->last_effective);
    }

    /* Reset the SMTP diagnostic unconditionally (matches notification_send_alert)
     * so a later test-email probe doesn't surface stale output. */
    s_diag[0] = '\0'; s_diag_len = 0;

    if (cfg->email_enabled && cfg->email_to[0]) {
        if (!smtp_send(cfg, subject, body))
            ESP_LOGW(TAG, "restart email failed");
    }
    if (cfg->sms_enabled && cfg->sms_phone[0]) {
        char sms_msg[64];
        snprintf(sms_msg, sizeof(sms_msg), "[%s] RESTART", device_name);
        if (!sms_send(cfg, sms_msg))
            ESP_LOGW(TAG, "restart sms failed");
    }
}

char *notification_test_email(const notify_cfg_t *cfg)
{
    if (!cfg || !cfg->smtp_host[0]) return strdup("FAIL: brak serwera SMTP w konfiguracji");
    if (!cfg->email_to[0]) return strdup("FAIL: brak adresu odbiorcy (email_to)");
    s_diag[0] = '\0'; s_diag_len = 0;
    bool ok = smtp_send(cfg, "[Heating] TEST", "Testowy e-mail z kontrolera ogrzewania ESP32.");
    /* s_diag already filled by smtp_send; return a copy. */
    size_t len = strlen(s_diag);
    if (len == 0) return strdup(ok ? "OK (brak szczegolow)" : "FAIL (brak szczegolow)");
    return strdup(s_diag);
}
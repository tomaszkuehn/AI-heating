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

#include <errno.h>
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/net_sockets.h"

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
    int           smtp_port;   /* explicit SMTP port (0 => host:port or default) */
    union {
        notify_restart_t restart;
        notify_alert_t   alert;
    } u;
} notify_cmd_t;

#define NOTIFY_QUEUE_LEN 2
static QueueHandle_t s_notify_queue = NULL;

/* base64-encode `in` (NUL-terminated) into `out`; returns out. */
/* Base64-encode `n` raw bytes (may contain embedded NULs -- needed for the
 * SASL PLAIN blob "\0user\0pass"). */
static char *b64n(const char *in, size_t n, char *out, size_t outlen)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
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

static char *b64(const char *in, char *out, size_t outlen)
{
    return b64n(in, strlen(in), out, outlen);
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

/* ---- Opportunistic STARTTLS for the plain-TCP SMTP client below ----
 * The ESP32 SMTP path is intentionally simple (no TLS by default, port 25 to a
 * local relay). When the server advertises STARTTLS in its EHLO response we
 * upgrade the already-connected socket to a TLS session (mbedTLS) so the
 * AUTH credentials and message are no longer sent in clear text. This is
 * "opportunistic" encryption only: we use MBEDTLS_SSL_VERIFY_NONE, i.e. the
 * server certificate chain is NOT validated (we have no CA bundle on the
 * device). That protects the password from passive eavesdropping on the wire
 * but not from an active MITM. A server that does not offer STARTTLS is
 * contacted in clear text exactly as before (backward compatible). */

typedef struct {
    int                  sock;
    bool                 tls;
    mbedtls_ssl_context *ssl;
} smtp_conn_t;

static int smtp_tls_send(void *ctx, const unsigned char *buf, size_t len)
{
    int sock = *(const int *)ctx;
    int n = (int)send(sock, buf, len, 0);
    if (n >= 0) return n;
    int e = errno;
    if (e == EAGAIN || e == EWOULDBLOCK || e == EINTR) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int smtp_tls_recv(void *ctx, unsigned char *buf, size_t len)
{
    int sock = *(const int *)ctx;
    int n = (int)recv(sock, buf, len, 0);
    if (n > 0) return n;
    if (n == 0) return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    int e = errno;
    if (e == EAGAIN || e == EWOULDBLOCK || e == EINTR) return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

static int smtp_net_recv(smtp_conn_t *c, char *buf, size_t len)
{
    if (c->tls) return mbedtls_ssl_read(c->ssl, (unsigned char *)buf, len);
    return (int)recv(c->sock, buf, len, 0);
}

static int smtp_net_send(smtp_conn_t *c, const char *buf, size_t len)
{
    if (c->tls) return mbedtls_ssl_write(c->ssl, (const unsigned char *)buf, len);
    return (int)send(c->sock, buf, len, 0);
}

/* Read one complete SMTP reply into `buf` and return its 3-digit status code
 * (or -1 on error/timeout). Handles multi-line replies: per RFC 5321 a line
 * "NNN-text" is a continuation and "NNN text" (space after the code) is the
 * final line. This works for both single-line (greeting, STARTTLS 220) and
 * multi-line (EHLO) replies -- the previous substring-terminator approach only
 * matched multi-line replies and spun to a timeout on single-line ones. */
static int smtp_read_reply(smtp_conn_t *c, char *buf, size_t max)
{
    size_t used = 0;
    int tries = 0;
    buf[0] = '\0';               /* read each reply fresh -- never match stale text */
    while (used < max - 1) {
        char tmp[256];
        int n = smtp_net_recv(c, tmp, sizeof(tmp) - 1);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (++tries >= 3) { diag_append("[read timeout]\n"); return -1; }
            continue;
        }
        if (n <= 0) { diag_append("[read err %d]\n", n); return -1; }
        if ((size_t)n > max - 1 - used) n = (int)(max - 1 - used);
        memcpy(buf + used, tmp, (size_t)n);
        used += (size_t)n;
        buf[used] = '\0';
        /* Scan complete lines; a final line is "NNN<space>...\r\n". */
        char *p = buf;
        char *eol;
        while ((eol = strstr(p, "\r\n")) != NULL) {
            if (eol - p >= 4 &&
                p[0] >= '0' && p[0] <= '9' &&
                p[1] >= '0' && p[1] <= '9' &&
                p[2] >= '0' && p[2] <= '9' &&
                p[3] == ' ') {
                return (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
            }
            p = eol + 2;
        }
    }
    return -1;
}

/* Upgrade `c->sock` to a TLS client session. The ssl/config/entropy/ctr_drbg
 * contexts are owned by the caller because mbedtls_ssl_setup() stores a POINTER
 * to `conf` (and conf references the RNG) inside the ssl context: they must stay
 * alive for the whole TLS session, i.e. until mbedtls_ssl_free() in smtp_send.
 * This function only initialises and configures them (so the caller can always
 * free them safely) and performs the handshake; it never frees them. */
static bool smtp_starttls(smtp_conn_t *c, const char *host,
                          mbedtls_ssl_config *conf,
                          mbedtls_entropy_context *entropy,
                          mbedtls_ctr_drbg_context *ctr)
{
    mbedtls_ssl_init(c->ssl);
    mbedtls_ssl_config_init(conf);
    mbedtls_entropy_init(entropy);
    mbedtls_ctr_drbg_init(ctr);

    int ret = mbedtls_ctr_drbg_seed(ctr, mbedtls_entropy_func, entropy, NULL, 0);
    if (ret != 0) { diag_append("FAIL: TLS rng seed (%d)\n", ret); return false; }
    ret = mbedtls_ssl_config_defaults(conf, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) { diag_append("FAIL: TLS config (%d)\n", ret); return false; }
    /* Opportunistic: encrypt but do not authenticate the server (no CA bundle). */
    mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(conf, mbedtls_ctr_drbg_random, ctr);
    ret = mbedtls_ssl_setup(c->ssl, conf);
    if (ret != 0) { diag_append("FAIL: TLS setup (%d)\n", ret); return false; }
    ret = mbedtls_ssl_set_hostname(c->ssl, host);
    if (ret != 0) { diag_append("FAIL: TLS hostname (%d)\n", ret); return false; }
    mbedtls_ssl_set_bio(c->ssl, &c->sock, smtp_tls_send, smtp_tls_recv, NULL);
    int hs_tries = 0;
    while ((ret = mbedtls_ssl_handshake(c->ssl)) != 0) {
        if ((ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) && ++hs_tries >= 8) {
            diag_append("FAIL: TLS handshake timeout\n");
            return false;
        }
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            diag_append("FAIL: TLS handshake (%d)\n", ret);
            return false;
        }
    }
    return true;
}

/* Minimal best-effort SMTP submission over plain TCP, with opportunistic
 * STARTTLS (see smtp_starttls above). AUTH LOGIN supported. Failures are
 * logged and swallowed. */
static bool smtp_send(const notify_cfg_t *c, int port_arg, const char *subject, const char *body)
{
    if (!c->smtp_host[0]) return false;
    char host[80];
    /* Allow "host:port" in smtp_host as a fallback. The dedicated smtp_port
     * field, when set (>0), takes precedence over any port embedded in the host. */
    strncpy(host, c->smtp_host, sizeof(host) - 1); host[sizeof(host) - 1] = '\0';
    int port = (port_arg > 0) ? port_arg : 25;
    char *colon = strchr(host, ':');
    if (colon) { *colon = '\0'; if (port_arg <= 0) port = atoi(colon + 1); }

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

    /* TLS contexts owned here: they must outlive the handshake and stay valid
     * for the whole session (mbedtls_ssl keeps pointers into conf/rng). Freed
     * together with the ssl context on both exit paths when tls_init is set. */
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config  tls_conf;
    mbedtls_entropy_context tls_entropy;
    mbedtls_ctr_drbg_context tls_ctr;
    bool tls_init = false;   /* tracks mbedtls_ssl_init for safe free() */
    smtp_conn_t conn = { .sock = sock, .tls = false, .ssl = &ssl };

    char rx[512]; char tx[512];
    #define RECV() do { \
        int code = smtp_read_reply(&conn, rx, sizeof(rx)); \
        if (code < 0) { diag_append("RECV failed (tls=%d)\n", conn.tls); goto done; } \
        diag_append("S: %s", rx); \
    } while (0)
    #define SEND(s) do { \
        const char *sptr = (s); size_t slen = strlen(sptr), spos = 0; int sret, stries = 0; \
        while (spos < slen) { \
            sret = smtp_net_send(&conn, sptr + spos, slen - spos); \
            if (sret > 0) { spos += (size_t)sret; stries = 0; } \
            else if ((sret == MBEDTLS_ERR_SSL_WANT_WRITE || sret == MBEDTLS_ERR_SSL_WANT_READ) && ++stries < 3) continue; \
            else { diag_append("SEND failed\n"); goto done; } \
        } \
        diag_append("C: %s", sptr); \
    } while (0)

    /* Sanitise header fields only (config / device-name supplied, must not inject
     * CRLF). The body is sent raw in a separate SEND() below so its \r\n line
     * structure is preserved and it cannot overflow tx (the restart body can
     * approach 512 B). Safe because device_name is CRLF-free by write-time
     * validation in the web layer (CODE_REVIEW #3). */
    char rcpt[64], subj[128], b64buf[128];
    sanitize_line(c->email_to, rcpt, sizeof(rcpt));
    sanitize_line(subject, subj, sizeof(subj));

    RECV();                                            /* server greeting 220 */
    SEND("EHLO heating\r\n"); RECV();                  /* EHLO reply (multi-line) */
    /* Opportunistic STARTTLS: if the server advertised it, upgrade now. */
    if (strstr(rx, "STARTTLS")) {
        diag_append("STARTTLS advertised, upgrading...\n");
        SEND("STARTTLS\r\n"); RECV();                  /* expect 220 ready */
        tls_init = true;   /* smtp_starttls inits all four ctxs before it can fail */
        if (!smtp_starttls(&conn, host, &tls_conf, &tls_entropy, &tls_ctr)) { goto done; }
        conn.tls = true;
        diag_append("TLS session established\n");
        SEND("EHLO heating\r\n"); RECV();              /* re-EHLO required post-TLS */
    }
    if (c->smtp_user[0] && c->smtp_pass[0]) {
        /* SASL PLAIN, single-line initial-response form:
         * AUTH PLAIN base64( authzid \0 authcid \0 passwd ), authzid empty.
         * Postfix negotiates PLAIN with Brevo successfully; we match it. */
        char sasl[96];
        size_t sl = 0;
        sasl[sl++] = '\0';
        for (const char *p = c->smtp_user; *p && sl < sizeof(sasl) - 1; p++) sasl[sl++] = *p;
        sasl[sl++] = '\0';
        for (const char *p = c->smtp_pass; *p && sl < sizeof(sasl); p++) sasl[sl++] = *p;
        snprintf(tx, sizeof(tx), "AUTH PLAIN %s\r\n", b64n(sasl, sl, b64buf, sizeof(b64buf)));
        SEND(tx); RECV();
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
    if (conn.tls) mbedtls_ssl_close_notify(&ssl);
    close(sock);
    diag_append("OK: email accepted by server\n");
    ESP_LOGI(TAG, "email sent to %s: %s", rcpt, subj);
    if (tls_init) {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&tls_conf);
        mbedtls_ctr_drbg_free(&tls_ctr);
        mbedtls_entropy_free(&tls_entropy);
    }
    return true;
done:
    close(sock);
    diag_append("FAIL: SMTP transaction incomplete\n");
    if (tls_init) {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&tls_conf);
        mbedtls_ctr_drbg_free(&tls_ctr);
        mbedtls_entropy_free(&tls_entropy);
    }
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

/* ---- E-mail rate limiting (per distinct event) ----
 * The same event (a fault class + message, or a restart notice) must not
 * generate more than one e-mail within EMAIL_RATE_WINDOW_US. Repeated events
 * inside the window are suppressed and counted; when the window lapses the next
 * occurrence is sent and the accumulated repeat count is reported in the body
 * so the recipient knows it fired N times, not just once. Keys are matched by a
 * short string (e.g. "alert:<fault>:<msg>", "restart:<name>") so any event type
 * can be throttled uniformly. The worker task is single-threaded, so the table
 * needs no locking. */
#define EMAIL_RATE_WINDOW_US   (2LL * 3600 * 1000 * 1000)   /* 2 hours */
#define EMAIL_RATE_SLOTS       8
typedef struct {
    char    key[56];
    int64_t last_sent_us;
    uint16_t repeats;       /* suppressed occurrences since last_sent_us */
} email_rate_t;
static email_rate_t s_email_rate[EMAIL_RATE_SLOTS];
static int s_email_rate_next = 0;

/* Returns true if an e-mail for `key` may be sent now. If false, the event was
 * suppressed (and `repeats_out` holds how many times it has repeated). If true
 * after a window lapse, `repeats_out` holds the repeats accumulated in the
 * previous window (0 for a genuinely new key). */
static bool email_rate_allow(const char *key, int *repeats_out)
{
    int64_t now = esp_timer_get_time();
    email_rate_t *slot = NULL;
    for (int i = 0; i < EMAIL_RATE_SLOTS; i++) {
        if (s_email_rate[i].key[0] &&
            strncmp(s_email_rate[i].key, key, sizeof(s_email_rate[i].key) - 1) == 0) {
            slot = &s_email_rate[i];
            break;
        }
    }
    if (slot) {
        if (now - slot->last_sent_us < EMAIL_RATE_WINDOW_US) {
            if (slot->repeats < 65535) slot->repeats++;
            *repeats_out = (int)slot->repeats;
            return false;          /* throttled */
        }
        *repeats_out = (int)slot->repeats;   /* report prior-window repeats */
        slot->repeats = 0;
        slot->last_sent_us = now;
        return true;
    }
    /* New key: allocate a slot (round-robin eviction). */
    slot = &s_email_rate[s_email_rate_next];
    s_email_rate_next = (s_email_rate_next + 1) % EMAIL_RATE_SLOTS;
    strncpy(slot->key, key, sizeof(slot->key) - 1);
    slot->key[sizeof(slot->key) - 1] = '\0';
    slot->last_sent_us = now;
    slot->repeats = 0;
    *repeats_out = 0;
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
                cmd.u.restart.has_external, cmd.smtp_port);
        } else { /* NOTIFY_ALERT */
            notification_send_alert(&cmd.cfg, cmd.u.alert.fault, cmd.u.alert.message, cmd.smtp_port);
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
                                 const char *message, int smtp_port)
{
    if (!s_notify_queue || !cfg || !message) return;
    notify_cmd_t cmd = {0};
    cmd.kind = NOTIFY_ALERT;
    cmd.cfg = *cfg;
    cmd.smtp_port = smtp_port;
    cmd.u.alert.fault = f;
    strncpy(cmd.u.alert.message, message, sizeof(cmd.u.alert.message) - 1);
    if (xQueueSend(s_notify_queue, &cmd, 0) != pdPASS)
        ESP_LOGW(TAG, "notify queue full -- alert dropped");
}

void notification_dispatch_restart(const notify_cfg_t *cfg, const char *device_name,
    float sys_temp, float ext_temp, int healthy, int total,
    const sensor_t *sensors, int sensor_count, bool has_external, int smtp_port)
{
    if (!s_notify_queue || !cfg || !device_name || !sensors) return;
    if (!cfg->email_enabled && !cfg->sms_enabled) return;
    notify_cmd_t cmd = {0};
    cmd.kind = NOTIFY_RESTART;
    cmd.cfg = *cfg;
    cmd.smtp_port = smtp_port;
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
                             const char *message, int smtp_port)
{
    if (!cfg || !message) return;
    char subject[64];
    snprintf(subject, sizeof(subject), "[Heating] fault %d", (int)f);
    s_diag[0] = '\0'; s_diag_len = 0;

    /* Throttle e-mail per distinct event (fault class + message), max one per
     * 2h window; report repeats accumulated in the previous window. */
    char rkey[72];
    snprintf(rkey, sizeof(rkey), "alert:%d:%s", (int)f, message);
    int repeats = 0;
    if (cfg->email_enabled && cfg->email_to[0]) {
        if (email_rate_allow(rkey, &repeats)) {
            char body[384];
            int p = snprintf(body, sizeof(body), "%s", message);
            if (repeats > 0 && p > 0 && p < (int)sizeof(body))
                p += snprintf(body + p, sizeof(body) - p,
                              "\r\n\r\n(Powtorzono %d razy w ciagu ostatnich 2h.)", repeats);
            if (!smtp_send(cfg, smtp_port, subject, body))
                ESP_LOGW(TAG, "email send failed");
        } else {
            ESP_LOGI(TAG, "alert e-mail throttled (fault %d, %d repeats)", (int)f, repeats);
        }
    }
    if (cfg->sms_enabled && cfg->sms_phone[0]) {
        if (!sms_send(cfg, message))
            ESP_LOGW(TAG, "sms send failed");
    }
}

void notification_send_restart(const notify_cfg_t *cfg, const char *device_name,
    float sys_temp, float ext_temp, int healthy, int total,
    const sensor_t *sensors, int sensor_count, bool has_external, int smtp_port)
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

    /* Throttle restart e-mail per device to one per 2h window (a boot-loop would
     * otherwise spam the inbox); report repeats from the previous window. */
    char rkey[72];
    snprintf(rkey, sizeof(rkey), "restart:%s", device_name);
    int repeats = 0;
    if (cfg->email_enabled && cfg->email_to[0]) {
        if (email_rate_allow(rkey, &repeats)) {
            if (repeats > 0) {
                int p = (int)strlen(body);
                if (p > 0 && p < (int)sizeof(body))
                    snprintf(body + p, sizeof(body) - p,
                             "\r\n\r\n(Powtorzono %d razy w ciagu ostatnich 2h.)", repeats);
            }
            if (!smtp_send(cfg, smtp_port, subject, body))
                ESP_LOGW(TAG, "restart email failed");
        } else {
            ESP_LOGI(TAG, "restart e-mail throttled (%d repeats)", repeats);
        }
    }
    if (cfg->sms_enabled && cfg->sms_phone[0]) {
        char sms_msg[64];
        snprintf(sms_msg, sizeof(sms_msg), "[%s] RESTART", device_name);
        if (!sms_send(cfg, sms_msg))
            ESP_LOGW(TAG, "restart sms failed");
    }
}

char *notification_test_email(const notify_cfg_t *cfg, int smtp_port)
{
    if (!cfg || !cfg->smtp_host[0]) return strdup("FAIL: brak serwera SMTP w konfiguracji");
    if (!cfg->email_to[0]) return strdup("FAIL: brak adresu odbiorcy (email_to)");
    s_diag[0] = '\0'; s_diag_len = 0;
    bool ok = smtp_send(cfg, smtp_port, "[Heating] TEST", "Testowy e-mail z kontrolera ogrzewania ESP32.");
    /* s_diag already filled by smtp_send; return a copy. */
    size_t len = strlen(s_diag);
    if (len == 0) return strdup(ok ? "OK (brak szczegolow)" : "FAIL (brak szczegolow)");
    return strdup(s_diag);
}
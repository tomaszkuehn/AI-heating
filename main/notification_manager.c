#include "notification_manager.h"
#include "esp_log.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "notify";

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
 * lines (email_to, subject, and single-line bodies come partly from config). */
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
        ESP_LOGW(TAG, "SMTP: cannot resolve %s", host);
        return false;
    }
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { freeaddrinfo(res); return false; }
    struct sockaddr_in dest = *(struct sockaddr_in *)res->ai_addr;
    dest.sin_port = htons(port);
    int to_ms = 4000; setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &to_ms, sizeof(to_ms));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &to_ms, sizeof(to_ms));
    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        ESP_LOGW(TAG, "SMTP: connect failed");
        close(sock); freeaddrinfo(res); return false;
    }
    freeaddrinfo(res);

    char rx[256]; char tx[512];
    #define RECV() do { int n = recv(sock, rx, sizeof(rx)-1, 0); if (n<=0) goto done; rx[n]=0; } while (0)
    #define SEND(s) do { int L=strlen(s); if (send(sock,s,L,0)!=L) goto done; } while (0)

    /* Sanitised header fields (config-supplied, must not inject CRLF). */
    char rcpt[64], subj[64], msg[256], b64buf[128];
    sanitize_line(c->email_to, rcpt, sizeof(rcpt));
    sanitize_line(subject, subj, sizeof(subj));
    sanitize_line(body, msg, sizeof(msg));

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
    snprintf(tx, sizeof(tx),
             "From: heating@esp32.local\r\nTo: %s\r\nSubject: %s\r\n\r\n%s\r\n.\r\n",
             rcpt, subj, msg);
    SEND(tx); RECV();
    SEND("QUIT\r\n");
    close(sock);
    ESP_LOGI(TAG, "email sent to %s: %s", rcpt, subj);
    return true;
done:
    close(sock);
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

void notification_init(void)
{
    ESP_LOGI(TAG, "notification manager ready");
}

void notification_send_alert(const notify_cfg_t *cfg, fault_class_t f,
                             const char *message)
{
    if (!cfg || !message) return;
    char subject[64];
    snprintf(subject, sizeof(subject), "[Heating] fault %d", (int)f);
    if (cfg->email_enabled && cfg->email_to[0]) {
        if (!smtp_send(cfg, subject, message))
            ESP_LOGW(TAG, "email send failed");
    }
    if (cfg->sms_enabled && cfg->sms_phone[0]) {
        if (!sms_send(cfg, message))
            ESP_LOGW(TAG, "sms send failed");
    }
}
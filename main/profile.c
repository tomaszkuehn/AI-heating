#include "profile.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void profile_default(daily_profile_t *p)
{
    if (!p) return;
    for (int h = 0; h < HE_PROFILE_HOURS; h++) {
        /* Night (22..6): 18.0 ON / 19.0 OFF. Day: 21.0 ON / 22.5 OFF. */
        bool night = (h < 6) || (h >= 22);
        p->hours[h].on_temp  = night ? 18.0f : 21.0f;
        p->hours[h].off_temp = night ? 19.0f : 22.5f;
    }
}

bool profile_validate(const daily_profile_t *p, int *err_hour, char *err_msg, size_t err_len)
{
    if (!p) {
        if (err_msg) snprintf(err_msg, err_len, "null profile");
        return false;
    }
    for (int h = 0; h < HE_PROFILE_HOURS; h++) {
        const profile_hour_t *e = &p->hours[h];
        if (he_isnan(e->on_temp) || he_isnan(e->off_temp)) {
            if (err_hour) *err_hour = h;
            if (err_msg) snprintf(err_msg, err_len, "hour %d: NaN threshold", h);
            return false;
        }
        if (e->on_temp < HE_TEMP_MIN_LOGICAL || e->on_temp > HE_TEMP_MAX_LOGICAL ||
            e->off_temp < HE_TEMP_MIN_LOGICAL || e->off_temp > HE_TEMP_MAX_LOGICAL) {
            if (err_hour) *err_hour = h;
            if (err_msg) snprintf(err_msg, err_len, "hour %d: out of range", h);
            return false;
        }
        if (e->on_temp > e->off_temp) {
            if (err_hour) *err_hour = h;
            if (err_msg) snprintf(err_msg, err_len, "hour %d: on>off", h);
            return false;
        }
        /* Require a hysteresis gap of at least 0.2 degC. */
        if (e->off_temp - e->on_temp < 0.2f) {
            if (err_hour) *err_hour = h;
            if (err_msg) snprintf(err_msg, err_len, "hour %d: gap<0.2", h);
            return false;
        }
    }
    return true;
}

const profile_hour_t *profile_for_hour(const daily_profile_t *p, int hour)
{
    if (!p) return NULL;
    if (hour < 0) hour = 0;
    if (hour >= HE_PROFILE_HOURS) hour = HE_PROFILE_HOURS - 1;
    return &p->hours[hour];
}

/* Minimal JSON parser sufficient for "[[on,off],...]" with 24 pairs. */
static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    return s;
}

size_t profile_to_json(const daily_profile_t *p, char *out, size_t out_len)
{
    if (!p || !out || out_len == 0) return 0;
    size_t pos = 0;
    int n = snprintf(out, out_len, "[");
    if (n <= 0) return 0;
    pos += (size_t)n;
    for (int h = 0; h < HE_PROFILE_HOURS && pos + 16 < out_len; h++) {
        n = snprintf(out + pos, out_len - pos, "%s[%.2f,%.2f]",
                     h ? "," : "",
                     p->hours[h].on_temp, p->hours[h].off_temp);
        if (n <= 0) return 0;
        pos += (size_t)n;
    }
    if (pos + 2 < out_len) {
        out[pos++] = ']';
        out[pos] = '\0';
    }
    return pos;
}

bool profile_from_json(daily_profile_t *p, const char *json, size_t len)
{
    if (!p || !json) return false;
    const char *s = json;
    const char *end = json + len;
    s = skip_ws(s);
    if (*s != '[') return false;
    s++;
    for (int h = 0; h < HE_PROFILE_HOURS; h++) {
        s = skip_ws(s);
        if (s >= end) return false;
        if (h > 0) {
            if (*s != ',') return false;
            s++;
            s = skip_ws(s);
        }
        if (*s != '[') return false;
        s++;
        char *ep;
        float on = strtof(s, &ep);
        if (ep == s) return false;
        s = skip_ws(ep);
        if (*s != ',') return false;
        s++;
        float off = strtof(s, &ep);
        if (ep == s) return false;
        s = skip_ws(ep);
        if (*s != ']') return false;
        s++;
        p->hours[h].on_temp = on;
        p->hours[h].off_temp = off;
    }
    s = skip_ws(s);
    if (*s != ']') return false;
    return true;
}
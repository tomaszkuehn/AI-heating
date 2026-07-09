/**
 * Daily heating profile helpers (spec section 5.1).
 *
 * A profile is 24 hourly entries, each with an ON and an OFF temperature.
 * Provides validation, default generation and JSON (de)serialisation used
 * both by the storage layer (save/load to file) and the web API.
 */
#pragma once

#include "data_model.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build a sensible default profile (e.g. 21.0/22.5 day, 18.0/19.0 night). */
void profile_default(daily_profile_t *p);

/* Validate a profile: every hour present, on<=off, sane temperature range.
 * Returns true when valid; fills `err_hour`/`err_msg` on failure. */
bool profile_validate(const daily_profile_t *p, int *err_hour, char *err_msg, size_t err_len);

/* Look up the thresholds for the current hour (0..23). */
const profile_hour_t *profile_for_hour(const daily_profile_t *p, int hour);

/* Serialise / parse a profile as compact JSON.
 * Returns bytes written / parsed length, 0 on error. */
size_t profile_to_json(const daily_profile_t *p, char *out, size_t out_len);
bool   profile_from_json(daily_profile_t *p, const char *json, size_t len);

#ifdef __cplusplus
}
#endif
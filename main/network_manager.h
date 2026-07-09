/**
 * Network manager (spec section 12).
 *
 * WiFi provisioning with two modes:
 *   - AP (default "ESP" / "12345") used on first boot or after a network
 *     reset, so the user can reach the captive setup page.
 *   - STA client of the user's home network, configured via the web UI.
 *
 * A hardware button (held 5s) resets only the network settings while
 * preserving the rest of the user configuration. SNTP is started once STA
 * connects so the daily profile can use the real time of day.
 */
#pragma once

#include "storage_manager.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start networking according to cfg->wifi_sta_mode. */
void network_init(system_config_t *cfg);

/* Switch mode at runtime (called after the user saves network settings). */
void network_apply(const system_config_t *cfg);

/* STA connected with an IP address. */
bool network_is_up(void);

/* Current device IP address (e.g. "192.168.4.1" or DHCP address). */
const char *network_device_ip(void);

/* Start the hardware reset-button watcher. */
void network_start_reset_button(system_config_t *cfg);

#ifdef __cplusplus
}
#endif
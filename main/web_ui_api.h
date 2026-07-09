/**
 * Web UI + REST API (spec section 11).
 *
 * Serves the embedded SPA (index.html / style.css / app.js) and a JSON REST
 * API used by it. Runs inside the esp_http_server task; a crash here must not
 * affect the control loop (spec 13), so handlers only read/mediate the shared
 * configuration and never block the control task.
 *
 * Simulation mode is surfaced to the frontend via the state payload so the UI
 * can show the required colour / label / warning banner (spec 11.15).
 */
#pragma once

#include "storage_manager.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the HTTP server. Holds a reference to the live config. */
void web_ui_init(system_config_t *cfg);

#ifdef __cplusplus
}
#endif
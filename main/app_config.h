/**
 * Global hardware / wiring configuration.
 * Centralised so it can be retargeted without touching module logic.
 */
#pragma once

#include "data_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- GPIO ---- */
#define HE_GPIO_HEATING         GPIO_NUM_16   /* relay line to the furnace   */
#define HE_GPIO_NET_RESET_BTN   GPIO_NUM_0    /* BOOT button: hold to reset  */
#define HE_GPIO_LED             GPIO_NUM_2    /* onboard LED (blinks during net-reset hold) */
#define HE_NET_RESET_HOLD_MS    5000          /* 5s hold => network reset    */

/* ---- Sensor interface (external board over UART) ---- */
#define HE_SENSOR_UART          UART_NUM_1
#define HE_SENSOR_UART_TX       GPIO_NUM_17
#define HE_SENSOR_UART_RX       GPIO_NUM_18
#define HE_SENSOR_BAUD          115200
#define HE_SENSOR_BUF_SIZE      256

/* ---- Control loop timing ---- */
#define HE_CONTROL_TICK_MS      1000          /* control engine period       */
#define HE_SENSOR_POLL_MS       1000          /* how often we poll the iface */
#define HE_BOOST_DURATION_SEC   300           /* 5-minute boost              */
#define HE_SIM_TIME_SCALE       10            /* x10 fast-forward in sim mode */

/* ---- Hysteresis / protection defaults (spec 5.2) ----
 * HE_MIN_ON_SEC / HE_MIN_OFF_SEC / HE_ANTIOSC_LOCK_SEC are the compile-time
 * fallbacks used when a loaded config field is out of range (< the floor). The
 * user-configurable values live in system_config_t (min_on_sec etc., tail
 * fields, set via /api/limits) and default to HE_DEFAULT_* below. */
#define HE_MIN_ON_SEC           90           /* min continuous ON before off allowed */
#define HE_MIN_OFF_SEC          90           /* min continuous OFF before on allowed */
#define HE_MAX_ON_SEC           (3600 * 4)
#define HE_ANTIOSC_LOCK_SEC     120           /* anti-oscillation lock after state change */
#define HE_DEFAULT_MIN_ON_SEC       90       /* user-config: min ON  (range 30..3600) */
#define HE_DEFAULT_MIN_OFF_SEC      90       /* user-config: min OFF (range 30..3600) */
#define HE_DEFAULT_ANTIOSC_LOCK_SEC 120      /* user-config: anti-osc lock (range 30..600) */
#define HE_MIN_ON_OFF_FLOOR     30           /* hard floor for min_on/min_off (anti-chatter) */
#define HE_ANTIOSC_FLOOR        30           /* hard floor for anti-osc lock */

/* ---- Network defaults (spec 12) ---- */
#define HE_DEFAULT_AP_SSID      "ESP"
#define HE_DEFAULT_AP_PASS      "12345678"    /* WPA2 requires >= 8 chars */

/* ---- Device identity & protection-limit defaults ---- */
#define HE_DEFAULT_DEVICE_NAME      "Sterownik CO"
#define HE_DEFAULT_FAULT_GRACE_SEC  300        /* NO_HEAT_RISE grace (5 min)    */
#define HE_DEFAULT_MAX_ON_SEC       14400      /* max continuous heating (4 h)  */
#define HE_DEFAULT_MAX_ON_BREAK_SEC 600        /* forced break after max-on      */

/* ---- Emergency periodic mode defaults ---- */
#define HE_DEFAULT_EMERGENCY_ENABLED         false
#define HE_DEFAULT_EMERGENCY_ON_SEC          900   /* 15 min ON within the cycle  */
#define HE_DEFAULT_EMERGENCY_PERIOD_SEC      3600  /* 60 min cycle                 */
#define HE_DEFAULT_EMERGENCY_ON_SENSOR_FAULT false /* opt-in: emergency duty cycle when all sensors fail */

#ifdef __cplusplus
}
#endif
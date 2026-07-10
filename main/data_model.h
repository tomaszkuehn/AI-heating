/**
 * Shared data model for the heating controller.
 *
 * Defines the structures exchanged between the modules described in the
 * specification (section 13): sensor_manager, control_engine, fault_manager,
 * storage_manager, etc. Keeping these types in one place lets the control
 * layer stay independent of the communication / presentation layers.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Limits (spec section 2, 3, 5) ---- */
#define HE_MAX_SENSORS          6       /* 1..6 internal sensors               */
#define HE_MOVING_AVG_WINDOW    7       /* moving average over last 7 readings */
#define HE_PROFILE_HOURS        24      /* one threshold pair per hour         */
#define HE_NAME_LEN             32
#define HE_LOG_INTERVAL_SEC     60      /* record temperatures each minute     */
#define HE_SENSOR_TIMEOUT_SEC   90     /* stale if no data within this window */
#define HE_TEMP_MIN_LOGICAL     -40.0f  /* logical range for a valid reading   */
#define HE_TEMP_MAX_LOGICAL     85.0f
#define HE_WINDOW_DROP_RATE     0.30f  /* degC per minute => open-window       */
#define HE_WINDOW_RECOVERY_HYST 0.4f   /* re-enable sensor after recovery      */

/* Unix time is considered "real" (SNTP-synced) only past this epoch; below it
 * the clock is the offline 1970 count and must not be used for timestamps.    */
#define HE_TIME_VALID_EPOCH     1700000000

/* ---- Sensor data quality (spec section 4) ---- */
typedef enum {
    QUAL_OK = 0,
    QUAL_TIMEOUT,
    QUAL_OUT_OF_RANGE,
    QUAL_STALE,
    QUAL_WINDOW_OPEN,
    QUAL_DISABLED,
    QUAL_SIMULATED
} sensor_quality_t;

/* ---- Heating control state machine (spec section 5.3) ---- */
typedef enum {
    ST_IDLE = 0,
    ST_HEATING,
    ST_BOOST_5MIN,
    ST_PUMP_OVERRUN,
    ST_FAULT,
    ST_EMERGENCY_CYCLIC,
    ST_SIMULATION
} control_state_t;

/* ---- Fault classes (spec section 7) ---- */
typedef enum {
    FAULT_NONE = 0,
    FAULT_SENSOR,
    FAULT_SENSOR_IFACE,
    FAULT_NO_HEAT_RISE,
    FAULT_LOW_HEAT_RISE,
    FAULT_NETWORK,
    FAULT_STORAGE,
    FAULT_RESTART
} fault_class_t;

/* ---- Simulation source per sensor (spec section 8.1) ---- */
typedef enum {
    SIM_SRC_REAL = 0,
    SIM_SRC_CONST,
    SIM_SRC_RAMP,
    SIM_SRC_SUDDEN_DROP,
    SIM_SRC_NO_RESPONSE,
    SIM_SRC_OUT_OF_RANGE,
    SIM_SRC_VIRTUAL
} sim_source_t;

/* ---- Single sensor descriptor + runtime state ---- */
typedef struct {
    uint8_t            id;                 /* logical id 1..6 (0 = external)   */
    char               name[HE_NAME_LEN];
    bool               active;             /* enabled by user                  */
    bool               is_external;        /* outdoor sensor                   */
    float              weight;             /* share in the global average      */
    float              calib_offset;       /* physical calibration correction  */
    float              comfort_offset;     /* intentional effective-temp shift */

    /* rolling buffer of the last N raw readings (spec 3.3) */
    float              samples[HE_MOVING_AVG_WINDOW];
    int                sample_count;
    int                sample_head;
    float              last_raw;           /* last received raw value          */
    float              last_effective;     /* moving avg + offsets             */
    uint32_t           last_update_ms;     /* monotonic time of last reading   */
    sensor_quality_t   quality;
    bool               window_open;        /* currently suppressed by detector */
    bool               simulated;          /* fed from simulation source       */
    sim_source_t       sim_src;
    /* simulation parameters */
    float              sim_base;
    float              sim_rate;           /* degC per minute (ramp)           */
    float              sim_target;
} sensor_t;

/* ---- Daily profile (spec section 5.1) ---- */
typedef struct {
    float on_temp;                         /* temperature to switch heating ON */
    float off_temp;                        /* temperature to switch heating OFF */
} profile_hour_t;

typedef struct {
    profile_hour_t hours[HE_PROFILE_HOURS];
} daily_profile_t;

/* ---- Heating output / pump-overrun configuration (spec 6.3) ---- */
typedef struct {
    bool   enabled;
    int    impulse_seconds;               /* length of each short ON impulse  */
    int    period_seconds;                /* cadence, default 120s (2 min)    */
    int    total_seconds;                 /* total overrun window             */
} pump_overrun_cfg_t;

/* ---- Emergency periodic mode (spec 6.4) ---- */
typedef struct {
    bool   enabled;
    int    on_seconds;                    /* e.g. 900 (15 min)                */
    int    period_seconds;                /* e.g. 3600 (60 min)               */
} emergency_cfg_t;

/* ---- Notification configuration (spec section 7) ---- */
typedef struct {
    bool   email_enabled;
    char   email_to[64];
    char   smtp_host[64];
    char   smtp_user[64];
    char   smtp_pass[32];
    bool   sms_enabled;
    char   sms_phone[24];
    char   sms_gateway[64];
} notify_cfg_t;

/* ---- Aggregate health summary surfaced to the web UI (spec 11.5/11.6) ---- */
typedef struct {
    control_state_t state;
    bool            heating_active;       /* red "grzeje" marker               */
    bool            health_ok;            /* green health marker               */
    float           system_temp;          /* weighted effective average        */
    float           external_temp;        /* outdoor temp, or NAN             */
    fault_class_t   active_fault;
    uint32_t        uptime_ms;
    bool            simulation_mode;      /* any simulation active            */
} system_snapshot_t;

/* Helpers */
float he_nan(void);
bool  he_isnan(float v);
float he_clampf(float v, float lo, float hi);

/* Short string name for a sensor quality value (shared by the web UI and the
 * notification emails — single source of truth instead of triplicated switches). */
const char *sensor_quality_name(sensor_quality_t q);

/* True once the wall clock has been set (SNTP) — history/aggregation must not
 * timestamp records before this is true (they would land in 1970).           */
bool  he_time_valid(void);

/* Global configuration lock. The control task and the HTTP handlers both touch
 * the shared system_config_t (and the sensor arrays bound from it) from
 * different FreeRTOS tasks/cores; take this around every access.              */
void  he_config_lock_init(void);
void  he_config_lock(void);
void  he_config_unlock(void);

#ifdef __cplusplus
}
#endif
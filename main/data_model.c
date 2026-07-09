#include "data_model.h"
#include <math.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

float he_nan(void)            { return nanf(""); }
bool  he_isnan(float v)       { return isnan(v); }

float he_clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

bool he_time_valid(void)
{
    return time(NULL) > (time_t)HE_TIME_VALID_EPOCH;
}

/* Recursive so a handler that locks and then calls a helper which also locks
 * (or storage_save_config) does not self-deadlock. */
static SemaphoreHandle_t s_cfg_mutex = NULL;

void he_config_lock_init(void)
{
    if (!s_cfg_mutex) s_cfg_mutex = xSemaphoreCreateRecursiveMutex();
}

void he_config_lock(void)
{
    if (!s_cfg_mutex) he_config_lock_init();
    xSemaphoreTakeRecursive(s_cfg_mutex, portMAX_DELAY);
}

void he_config_unlock(void)
{
    if (s_cfg_mutex) xSemaphoreGiveRecursive(s_cfg_mutex);
}

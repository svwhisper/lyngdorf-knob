#include "power.h"
#include "app_config.h"
#include "display.h"
#include "wifi_manager.h"

#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include <stdint.h>
#include <stdbool.h>

static const char *TAG = "power";

typedef enum { DISP_AWAKE, DISP_DIM, DISP_SLEEP } disp_state_t;

static disp_state_t    s_state      = DISP_AWAKE;
static volatile int64_t s_last_act_us = 0;  // written from multiple tasks
static uint32_t        s_dim_secs   = DEFAULT_DIM_SECS;
static uint32_t        s_sleep_secs = DEFAULT_SLEEP_SECS;

esp_err_t power_init(void) {
    s_last_act_us = esp_timer_get_time();
    config_get_u32(NVS_DIM_SECS,   &s_dim_secs,   DEFAULT_DIM_SECS);
    config_get_u32(NVS_SLEEP_SECS, &s_sleep_secs, DEFAULT_SLEEP_SECS);
    ESP_LOGI(TAG, "dim=%lus sleep=%lus", (unsigned long)s_dim_secs, (unsigned long)s_sleep_secs);
    return ESP_OK;
}

void power_signal_activity(void) {
    s_last_act_us = esp_timer_get_time();
}

void power_tick(void) {
    if (s_dim_secs == 0 && s_sleep_secs == 0) return;

    int64_t idle_ms = (esp_timer_get_time() - s_last_act_us) / 1000;

    switch (s_state) {
        case DISP_AWAKE:
            if (s_dim_secs > 0 && idle_ms >= (int64_t)s_dim_secs * 1000) {
                display_set_backlight(DEFAULT_DIM_PCT);
                s_state = DISP_DIM;
                ESP_LOGD(TAG, "dim");
            }
            break;

        case DISP_DIM:
            if (s_sleep_secs > 0 && idle_ms >= (int64_t)s_sleep_secs * 1000) {
                display_set_backlight(0);
                display_sleep(true);
                if (!wifi_manager_is_ap_mode()) esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
                s_state = DISP_SLEEP;
                ESP_LOGI(TAG, "sleep");
            } else if (idle_ms < (int64_t)s_dim_secs * 1000) {
                // Activity was signaled — wake from dim
                display_set_backlight(DEFAULT_BL_PCT);
                s_state = DISP_AWAKE;
                ESP_LOGD(TAG, "wake from dim");
            }
            break;

        case DISP_SLEEP:
            if (idle_ms < (int64_t)s_sleep_secs * 1000) {
                // Activity was signaled — wake from sleep
                display_sleep(false);
                display_set_backlight(DEFAULT_BL_PCT);
                if (!wifi_manager_is_ap_mode()) esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
                s_state = DISP_AWAKE;
                ESP_LOGI(TAG, "wake from sleep");
            }
            break;
    }
}

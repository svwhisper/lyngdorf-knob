#include "power.h"
#include "app_config.h"
#include "display.h"
#include "wifi_manager.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include <stdint.h>
#include <stdbool.h>

static const char *TAG = "power";

typedef enum { DISP_AWAKE, DISP_DIM, DISP_SLEEP } disp_state_t;

static disp_state_t     s_state           = DISP_AWAKE;
static volatile int64_t s_last_act_us     = 0;     // updated from ISR / multiple tasks
static int64_t          s_entered_sleep_us = 0;
static int64_t          s_last_playing_us  = 0;
static uint32_t         s_dim_secs        = DEFAULT_DIM_SECS;
static uint32_t         s_sleep_secs      = DEFAULT_SLEEP_SECS;

esp_err_t power_init(void) {
    s_last_act_us     = esp_timer_get_time();
    s_last_playing_us = s_last_act_us;
    config_get_u32(NVS_DIM_SECS,   &s_dim_secs,   DEFAULT_DIM_SECS);
    config_get_u32(NVS_SLEEP_SECS, &s_sleep_secs, DEFAULT_SLEEP_SECS);
    ESP_LOGI(TAG, "dim=%lus sleep=%lus deep_after=%ds paused_grace=%ds",
             (unsigned long)s_dim_secs, (unsigned long)s_sleep_secs,
             DEEP_SLEEP_AFTER_S, PAUSED_GRACE_S);
    return ESP_OK;
}

void IRAM_ATTR power_signal_activity(void) {
    s_last_act_us = esp_timer_get_time();
}

bool power_is_idle(void) {
    return s_state == DISP_SLEEP;
}

// ---------------------------------------------------------------------------
// Configure deep-sleep wake sources and call esp_deep_sleep_start(). Does
// not return — the chip resets and re-enters app_main on wake.
//
// Wake on any of: TOUCH_INT_GPIO low (touch), ENC_A_GPIO low or
// ENC_B_GPIO low (encoder turn). All three are RTC-capable on ESP32-S3.
//
// Each wake pin has its RTC-side pull-up explicitly enabled — without this
// the pull-ups configured via gpio_config() don't survive into deep sleep
// and the floating inputs immediately trip the wake (chip wakes within
// milliseconds of going to sleep).
// ---------------------------------------------------------------------------
// Drain any pending touch event from the CST816D so its INT line returns
// high before we sleep. Without this, an unread touch can hold INT low,
// which causes the chip to wake immediately the moment we go to sleep.
static void cst816_drain(void) {
    uint8_t reg = 0x00;
    uint8_t buf[7];
    i2c_master_write_read_device(TOUCH_I2C_PORT, TOUCH_I2C_ADDR,
                                  &reg, 1, buf, sizeof(buf), pdMS_TO_TICKS(50));
}

static void enter_deep_sleep(void) {
    ESP_LOGW(TAG, "entering deep sleep — wake on touch (GPIO %d) or "
                  "encoder (GPIO %d/%d)",
             TOUCH_INT_GPIO, ENC_A_GPIO, ENC_B_GPIO);

    // Panel + backlight fully off.
    display_set_backlight(0);
    display_sleep(true);

    // Acknowledge any pending touch event so INT releases (returns high).
    cst816_drain();

    // Drop WiFi cleanly so the AP doesn't have to reap the association.
    if (!wifi_manager_is_ap_mode()) {
        esp_wifi_disconnect();
        esp_wifi_stop();
    }

    // Detach the encoder GPIO ISR — we're about to re-route those pins
    // to the RTC subsystem.
    gpio_isr_handler_remove(ENC_A_GPIO);
    gpio_isr_handler_remove(ENC_B_GPIO);

    const gpio_num_t wake_pins[] = {
        (gpio_num_t)TOUCH_INT_GPIO,
        (gpio_num_t)ENC_A_GPIO,
        (gpio_num_t)ENC_B_GPIO,
    };
    for (size_t i = 0; i < sizeof(wake_pins) / sizeof(wake_pins[0]); i++) {
        rtc_gpio_init(wake_pins[i]);
        rtc_gpio_set_direction(wake_pins[i], RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pullup_en(wake_pins[i]);
        rtc_gpio_pulldown_dis(wake_pins[i]);
    }

    // Read the actual GPIO levels via the RTC mux. ANY pin reading 0 right
    // now will trip the wake immediately, so we only include in the wake
    // mask those pins that are currently HIGH. The user can still wake via
    // whichever pins remain — encoder mechanical state can sometimes leave
    // ENC_A or ENC_B closed (low), and we don't want that to make the
    // device unsleepable.
    int t_int = rtc_gpio_get_level((gpio_num_t)TOUCH_INT_GPIO);
    int e_a   = rtc_gpio_get_level((gpio_num_t)ENC_A_GPIO);
    int e_b   = rtc_gpio_get_level((gpio_num_t)ENC_B_GPIO);
    ESP_LOGW(TAG, "pre-sleep pin levels: TP_INT=%d ENC_A=%d ENC_B=%d",
             t_int, e_a, e_b);

    uint64_t mask = 0;
    if (t_int) mask |= (1ULL << TOUCH_INT_GPIO);
    if (e_a)   mask |= (1ULL << ENC_A_GPIO);
    if (e_b)   mask |= (1ULL << ENC_B_GPIO);
    if (mask == 0) {
        ESP_LOGE(TAG, "all wake pins are low — cannot sleep, aborting");
        return;
    }

    // Clear any wake sources left over from light-sleep / PM (e.g. the
    // FreeRTOS tickless-idle resume timer) — without this, deep sleep
    // wakes immediately on a stale TIMER wake.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    ESP_LOGI(TAG, "wake mask = 0x%llx", (unsigned long long)mask);

    // Brief breathing room so log gets flushed before the chip goes dark.
    vTaskDelay(pdMS_TO_TICKS(50));

    esp_deep_sleep_start();
    // unreachable
}

void power_tick(void) {
    if (s_dim_secs == 0 && s_sleep_secs == 0) return;

    int64_t now      = esp_timer_get_time();
    int64_t idle_ms  = (now - s_last_act_us) / 1000;

    // Track when we last saw the amp playing. Used as a gate for deep sleep.
    extern lk_state_t g_state;
    if (g_state.playing) s_last_playing_us = now;

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
                s_state            = DISP_SLEEP;
                s_entered_sleep_us = now;
                ESP_LOGI(TAG, "panel sleep");
            } else if (idle_ms < (int64_t)s_dim_secs * 1000) {
                display_set_backlight(DEFAULT_BL_PCT);
                s_state = DISP_AWAKE;
                ESP_LOGD(TAG, "wake from dim");
            }
            break;

        case DISP_SLEEP:
            if (idle_ms < (int64_t)s_sleep_secs * 1000) {
                // Activity → wake panel.
                display_sleep(false);
                display_set_backlight(DEFAULT_BL_PCT);
                if (!wifi_manager_is_ap_mode()) esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
                s_state = DISP_AWAKE;
                ESP_LOGI(TAG, "wake from sleep");
                break;
            }
            // Deep-sleep gate: been in panel-sleep long enough AND amp has
            // not been playing for the grace window AND not in AP/config mode.
            int64_t in_sleep_ms  = (now - s_entered_sleep_us) / 1000;
            int64_t paused_ms    = (now - s_last_playing_us)  / 1000;
            if (!wifi_manager_is_ap_mode() &&
                in_sleep_ms >= (int64_t)DEEP_SLEEP_AFTER_S * 1000 &&
                paused_ms   >= (int64_t)PAUSED_GRACE_S    * 1000) {
                enter_deep_sleep();   // does not return
            }
            break;
    }
}

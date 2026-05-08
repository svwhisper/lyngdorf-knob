#include "app_config.h"
#include "display.h"
#include "touch.h"
#include "encoder.h"
#include "lyngdorf.h"
#include "metadata.h"
#include "ui.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "power.h"
#include "haptic.h"
#include "battery.h"
#include "log_buffer.h"

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

// ---------------------------------------------------------------------------
// UI task — LVGL rendering + encoder processing (priority 3, 20KB stack)
// ---------------------------------------------------------------------------
static void ui_task(void *arg) {
    while (1) {
        if (xSemaphoreTake(g_lvgl_mutex, portMAX_DELAY) == pdTRUE) {
            encoder_process_events();
            ui_apply_pending_state();
            power_tick();
            uint32_t next_ms = lv_timer_handler();
            xSemaphoreGive(g_lvgl_mutex);
            // Yield for at most 10 ms to stay responsive
            vTaskDelay(pdMS_TO_TICKS((next_ms > 10) ? 10 : (next_ms < 1 ? 1 : next_ms)));
        }
    }
}

// ---------------------------------------------------------------------------
// Network task — Lyngdorf RIO TCP polling and command dispatch (priority 2).
// Now-playing metadata is polled separately by metadata_task (priority 1) so
// the HTTP fetch doesn't add latency to encoder commands.
// ---------------------------------------------------------------------------
static void net_task(void *arg) {
    // Wait for WiFi before doing anything network-related
    while (!wifi_manager_is_connected() && !wifi_manager_is_ap_mode()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (wifi_manager_is_connected()) {
        lyngdorf_init();
    }

    TickType_t last_lyngdorf = 0;

    while (1) {
        // Wait for a command, OR for the next periodic-poll due time, whichever
        // comes first. Blocking on the queue means commands are processed with
        // ~zero added latency once they arrive.
        lk_cmd_t cmd;
        int32_t  vol_delta_total  = 0;
        bool     mute_toggle_req  = false;
        bool     play_pause_req   = false;

        if (xQueueReceive(g_cmd_queue, &cmd, pdMS_TO_TICKS(20)) == pdTRUE) {
            // First command arrived — drain anything else that's already queued
            // (coalesces a fast multi-detent rotation into one !VOLCH).
            do {
                switch (cmd.type) {
                    case CMD_VOL_CHANGE:  vol_delta_total += cmd.param; break;
                    case CMD_MUTE_TOGGLE: mute_toggle_req = true;        break;
                    case CMD_PLAY_PAUSE:  play_pause_req  = true;        break;
                }
            } while (xQueueReceive(g_cmd_queue, &cmd, 0) == pdTRUE);

            if (vol_delta_total != 0) lyngdorf_vol_delta(vol_delta_total);
            if (mute_toggle_req)      lyngdorf_mute_toggle();
            if (play_pause_req)       metadata_play_pause();
        }

        TickType_t now = xTaskGetTickCount();

        // Poll Lyngdorf state. Cadence stretches from 5 s (active) to 30 s
        // (panel asleep / idle tier) to keep WiFi + CPU work down.
        uint32_t lyngdorf_period_ms = power_is_idle() ? 30000 : 5000;
        if ((now - last_lyngdorf) >= pdMS_TO_TICKS(lyngdorf_period_ms)) {
            if (wifi_manager_is_connected()) lyngdorf_poll_state();
            last_lyngdorf = now;
        }
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
void app_main(void) {
    // Detect why we're booting — cold reset vs. wake from deep sleep.
    // Wake from deep sleep skips the 7-second QR-code splash so the live
    // UI is back faster.
    // Capture wake reason and EXT1 mask EARLY — esp_sleep_get_*() must be
    // called before any new sleep configuration. We stash and log later,
    // because USB-CDC takes ~700 ms after wake to renumerate on the host
    // and any logs emitted before that get dropped by the monitor.
    esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
    uint64_t ext1_mask = (wake == ESP_SLEEP_WAKEUP_EXT1)
                          ? esp_sleep_get_ext1_wakeup_status() : 0;
    bool from_deep_sleep = (wake == ESP_SLEEP_WAKEUP_EXT1) ||
                           (wake == ESP_SLEEP_WAKEUP_GPIO);

    // Brief delay so USB-CDC has time to renumerate on the host before the
    // first ESP_LOG line — otherwise the wake-cause diagnostic is dropped
    // by the monitor. 300 ms is enough on macOS / Linux; bump to 1500 if
    // you want guaranteed-visible logs during development.
    vTaskDelay(pdMS_TO_TICKS(300));

    // Install in-memory log capture so http://<device-ip>/log shows the
    // recent log history (~8 KB). Useful when the device is on battery and
    // a USB serial monitor isn't an option.
    log_buffer_init();

    ESP_LOGI(TAG, "LyngdorfKnob starting (wake_cause=%d%s ext1_mask=0x%llx)",
             (int)wake,
             from_deep_sleep ? " — wake from deep sleep" : "",
             (unsigned long long)ext1_mask);

    // Enable dynamic frequency scaling + automatic light-sleep. Tasks that
    // block on queues / semaphores / timers naturally let the CPU drop to
    // its lowest power state until something actually happens.
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz       = 240,
        .min_freq_mhz       = 80,
        .light_sleep_enable = true,
    };
    esp_err_t pm_err = esp_pm_configure(&pm_cfg);
    if (pm_err != ESP_OK) ESP_LOGW(TAG, "pm_configure: %s", esp_err_to_name(pm_err));

    // 1. NVS + shared state
    ESP_ERROR_CHECK(app_config_init());

    // 2. Display + LVGL
    ESP_ERROR_CHECK(display_init());

    // 3. Touch input (LVGL indev registered here)
    ESP_ERROR_CHECK(touch_init());

    // 4. Rotary encoder
    ESP_ERROR_CHECK(encoder_init());

    // 5. Haptic driver (shares I2C bus with touch — must init after touch_init)
    ESP_ERROR_CHECK(haptic_init());

    // 6. Build LVGL UI (must be after lv_init which happens in display_init)
    if (xSemaphoreTake(g_lvgl_mutex, portMAX_DELAY) == pdTRUE) {
        ESP_ERROR_CHECK(ui_init(/*show_splash=*/!from_deep_sleep));
        xSemaphoreGive(g_lvgl_mutex);
    }

    // 7. Power management (dim/sleep timers, loads NVS config)
    ESP_ERROR_CHECK(power_init());

    // 7b. Battery monitoring (ADC1 ch0, periodic 10 s read)
    ESP_ERROR_CHECK(battery_init());

    // 8. WiFi (starts STA or AP, launches web server on connect)
    ESP_ERROR_CHECK(wifi_manager_init());

    // 9. FreeRTOS tasks
    xTaskCreatePinnedToCore(ui_task,  "ui",  20480, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(net_task, "net", 12288, NULL, 2, NULL, 0);

    // 10. Background metadata polling (own task at lower priority)
    ESP_ERROR_CHECK(metadata_init());

    ESP_LOGI(TAG, "tasks started");
}

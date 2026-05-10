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
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

// ---------------------------------------------------------------------------
// Persistent wake diagnostics (RTC slow memory — survives deep sleep, wiped
// only on cold reset). Lets us tell apart "device slept once for hours" from
// "device woke spuriously every few minutes" — the difference between a real
// hardware-floor current draw and a software bug, which need different fixes.
//
// Updated once at the top of app_main(); dumped to the in-memory log so a
// `curl /log` after the next wake shows the full history.
// ---------------------------------------------------------------------------
#define WAKE_HIST_LEN 16

typedef struct {
    uint32_t up_seconds;   // seconds the previous boot stayed awake before sleep
    uint8_t  cause;
    uint8_t  pad[3];
    uint64_t mask;
} wake_event_t;

RTC_DATA_ATTR static uint32_t       s_wake_count   = 0;
RTC_DATA_ATTR static uint32_t       s_hist_head    = 0;
RTC_DATA_ATTR static wake_event_t   s_wake_hist[WAKE_HIST_LEN];
RTC_DATA_ATTR static int64_t        s_last_sleep_us = 0;  // esp_timer when previous boot called esp_deep_sleep_start

// Public so power.c can stamp s_last_sleep_us right before deep sleep.
void diag_record_pre_sleep(void) {
    s_last_sleep_us = esp_timer_get_time();
}

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

    // Reset reason survives even when RTC memory is wiped (brownout, panic,
    // hard reset). Without this, a brownout looks identical to a cold
    // power-on in the wake history. Persisted to NVS below so we can
    // reconstruct a sequence of brownouts after the fact.
    esp_reset_reason_t reset_reason = esp_reset_reason();
    static const char *reset_names[] = {
        "UNKNOWN", "POWERON", "EXT", "SW", "PANIC", "INT_WDT", "TASK_WDT",
        "WDT", "DEEPSLEEP", "BROWNOUT", "SDIO", "USB", "JTAG", "EFUSE",
        "PWR_GLITCH", "CPU_LOCKUP",
    };
    const char *reset_name = (reset_reason < (sizeof(reset_names)/sizeof(reset_names[0])))
                              ? reset_names[reset_reason] : "?";

    // Stamp this boot into the persistent wake history BEFORE any other log
    // line. RTC memory is cheap and survives deep-sleep cycles. After 6 h on
    // battery, `curl /log` shows count + last 16 events: that distinguishes
    // "slept once for hours" (count=1) from "spurious wake loop" (count=N).
    s_wake_count++;
    wake_event_t ev = {
        .up_seconds = (uint32_t)(s_last_sleep_us / 1000000),  // uptime of prev boot
        .cause      = (uint8_t)wake,
        .mask       = ext1_mask,
    };
    s_wake_hist[s_hist_head % WAKE_HIST_LEN] = ev;
    s_hist_head++;

    // Drive GPIO 0 HIGH and hold it through deep sleep.
    //
    // Two reasons this matters on this board:
    //  1. GPIO 0 is wired to I2S_SWITCH_IN (CH445P pin 13, EN#, active-low).
    //     HIGH disconnects the I2S clocks from the PCM5100A audio DAC. With
    //     the mux disabled, the DAC sees no clocks and parks itself in its
    //     lowest non-shutdown state.
    //  2. GPIO 0 is the boot-mode strap — sampled at every reset/wake. It
    //     MUST be HIGH at the wake moment or the chip falls into UART
    //     download mode and never runs our code. Default internal pull-up
    //     usually keeps it high, but driving it explicitly (with hold
    //     through deep sleep) is deterministic.
    //
    // The hold-enable must be released before re-configuring the pin on
    // each cold boot, otherwise the prior driven state would block the
    // new gpio_set_direction call.
    gpio_hold_dis(GPIO_NUM_0);
    gpio_reset_pin(GPIO_NUM_0);
    gpio_set_direction(GPIO_NUM_0, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_0, 1);
    gpio_hold_en(GPIO_NUM_0);
    // Required global flag so per-pin holds actually take effect during
    // deep sleep (otherwise they only hold across light sleep).
    gpio_deep_sleep_hold_en();

    // Brief delay so USB-CDC has time to renumerate on the host before the
    // first ESP_LOG line — otherwise the wake-cause diagnostic is dropped
    // by the monitor. 300 ms is enough on macOS / Linux; bump to 1500 if
    // you want guaranteed-visible logs during development.
    vTaskDelay(pdMS_TO_TICKS(300));

    // Install in-memory log capture so http://<device-ip>/log shows the
    // recent log history (~8 KB). Useful when the device is on battery and
    // a USB serial monitor isn't an option.
    log_buffer_init();

    ESP_LOGI(TAG, "LyngdorfKnob starting (reset=%s wake_cause=%d%s ext1_mask=0x%llx)",
             reset_name,
             (int)wake,
             from_deep_sleep ? " — wake from deep sleep" : "",
             (unsigned long long)ext1_mask);

    // Dump wake-cycle history. Total count + last N events. If count == 1
    // after 6 h on battery, deep sleep stayed put. If count == 200 then
    // something is tripping wake every couple of minutes.
    ESP_LOGI(TAG, "wake history: %lu boots since cold reset",
             (unsigned long)s_wake_count);
    uint32_t start = (s_hist_head > WAKE_HIST_LEN) ? (s_hist_head - WAKE_HIST_LEN) : 0;
    for (uint32_t i = start; i < s_hist_head; i++) {
        const wake_event_t *e = &s_wake_hist[i % WAKE_HIST_LEN];
        ESP_LOGI(TAG, "  #%lu cause=%u mask=0x%llx prev_uptime=%lus",
                 (unsigned long)(i + 1),
                 (unsigned)e->cause,
                 (unsigned long long)e->mask,
                 (unsigned long)e->up_seconds);
    }

    // Enable dynamic frequency scaling + automatic light-sleep. Tasks that
    // block on queues / semaphores / timers naturally let the CPU drop to
    // its lowest power state until something actually happens.
    // min_freq_mhz=40 lets the DFS governor drop the CPU all the way down
    // when nothing is happening. ESP32-S3 supports 40 / 80 / 160 / 240 as
    // discrete frequencies; 40 is the lowest the PM driver will accept.
    // Combined with tickless idle, residual CPU draw in Tier 1/2 idle
    // periods drops by several mA.
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz       = 240,
        .min_freq_mhz       = 40,
        .light_sleep_enable = true,
    };
    esp_err_t pm_err = esp_pm_configure(&pm_cfg);
    if (pm_err != ESP_OK) ESP_LOGW(TAG, "pm_configure: %s", esp_err_to_name(pm_err));

    // 1. NVS + shared state
    ESP_ERROR_CHECK(app_config_init());

    // Persistent boot counter + last reset reason. RTC memory dies on
    // brownout, so the wake_hist[] above can't see brownout cycles. NVS
    // does survive — incrementing on every boot lets `curl /log` show
    // "boot N, prev reset: BROWNOUT" across power-cycles.
    uint32_t boot_count = 0, prev_reset = 0;
    config_get_u32("boot_count", &boot_count, 0);
    config_get_u32("prev_reset", &prev_reset, 0);
    boot_count++;
    config_set_u32("boot_count", boot_count);
    config_set_u32("prev_reset", (uint32_t)reset_reason);
    const char *prev_name = (prev_reset < (sizeof(reset_names)/sizeof(reset_names[0])))
                             ? reset_names[prev_reset] : "?";
    ESP_LOGI(TAG, "boot #%lu (this reset: %s, prev reset: %s)",
             (unsigned long)boot_count, reset_name, prev_name);

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

#include "app_config.h"
#include "display.h"
#include "touch.h"
#include "encoder.h"
#include "lyngdorf.h"
#include "upnp.h"
#include "ui.h"
#include "wifi_manager.h"
#include "web_server.h"

#include "esp_log.h"
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
            uint32_t next_ms = lv_timer_handler();
            xSemaphoreGive(g_lvgl_mutex);
            // Yield for at most 10 ms to stay responsive
            vTaskDelay(pdMS_TO_TICKS((next_ms > 10) ? 10 : (next_ms < 1 ? 1 : next_ms)));
        }
    }
}

// ---------------------------------------------------------------------------
// Network task — Lyngdorf TCP + UPnP polling, command dispatch (priority 2)
// ---------------------------------------------------------------------------
static void net_task(void *arg) {
    // Wait for WiFi before doing anything network-related
    while (!wifi_manager_is_connected() && !wifi_manager_is_ap_mode()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (wifi_manager_is_connected()) {
        lyngdorf_init();
        upnp_init();
    }

    TickType_t last_lyngdorf = 0;
    TickType_t last_upnp     = 0;

    while (1) {
        // Drain command queue first (low latency for knob/touch input)
        lk_cmd_t cmd;
        while (xQueueReceive(g_cmd_queue, &cmd, 0) == pdTRUE) {
            switch (cmd.type) {
                case CMD_VOL_CHANGE:
                    lyngdorf_vol_delta(cmd.param);
                    break;
                case CMD_MUTE_TOGGLE:
                    lyngdorf_mute_toggle();
                    break;
                case CMD_PLAY_PAUSE:
                    upnp_play_pause();
                    break;
            }
        }

        TickType_t now = xTaskGetTickCount();

        // Poll Lyngdorf state every 5 s
        if ((now - last_lyngdorf) >= pdMS_TO_TICKS(5000)) {
            if (wifi_manager_is_connected()) lyngdorf_poll_state();
            last_lyngdorf = now;
        }

        // Poll UPnP metadata every 3 s
        if ((now - last_upnp) >= pdMS_TO_TICKS(3000)) {
            if (wifi_manager_is_connected()) upnp_poll_metadata();
            last_upnp = now;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
void app_main(void) {
    ESP_LOGI(TAG, "LyngdorfKnob starting");

    // 1. NVS + shared state
    ESP_ERROR_CHECK(app_config_init());

    // 2. Display + LVGL
    ESP_ERROR_CHECK(display_init());

    // 3. Touch input (LVGL indev registered here)
    ESP_ERROR_CHECK(touch_init());

    // 4. Rotary encoder
    ESP_ERROR_CHECK(encoder_init());

    // 5. Build LVGL UI (must be after lv_init which happens in display_init)
    if (xSemaphoreTake(g_lvgl_mutex, portMAX_DELAY) == pdTRUE) {
        ESP_ERROR_CHECK(ui_init());
        xSemaphoreGive(g_lvgl_mutex);
    }

    // 6. WiFi (starts STA or AP, launches web server on connect)
    ESP_ERROR_CHECK(wifi_manager_init());

    // 7. FreeRTOS tasks
    xTaskCreatePinnedToCore(ui_task,  "ui",  20480, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(net_task, "net",  8192, NULL, 2, NULL, 0);

    ESP_LOGI(TAG, "tasks started");
}

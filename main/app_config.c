#include "app_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "config";

lk_state_t        g_state      = {0};
SemaphoreHandle_t g_state_mutex = NULL;
QueueHandle_t     g_cmd_queue   = NULL;
SemaphoreHandle_t g_lvgl_mutex  = NULL;

esp_err_t app_config_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    g_state_mutex = xSemaphoreCreateMutex();
    g_lvgl_mutex  = xSemaphoreCreateMutex();
    g_cmd_queue   = xQueueCreate(16, sizeof(lk_cmd_t));

    configASSERT(g_state_mutex);
    configASSERT(g_lvgl_mutex);
    configASSERT(g_cmd_queue);

    g_state.vol_db10 = -300;
    g_state.dirty    = true;

    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

esp_err_t config_get_str(const char *key, char *buf, size_t len) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) { buf[0] = '\0'; return err; }
    err = nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) buf[0] = '\0';
    return err;
}

esp_err_t config_set_str(const char *key, const char *val) {
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));
    esp_err_t err = nvs_set_str(h, key, val);
    nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t config_get_u32(const char *key, uint32_t *out, uint32_t def) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) { *out = def; return err; }
    err = nvs_get_u32(h, key, out);
    nvs_close(h);
    if (err != ESP_OK) *out = def;
    return err;
}

esp_err_t config_set_u32(const char *key, uint32_t val) {
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));
    esp_err_t err = nvs_set_u32(h, key, val);
    nvs_commit(h);
    nvs_close(h);
    return err;
}

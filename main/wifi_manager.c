#include "wifi_manager.h"
#include "app_config.h"
#include "web_server.h"
#include "ui.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "wifi";

static bool s_connected = false;
static bool s_ap_mode   = false;
static int  s_retries   = 0;

bool wifi_manager_is_connected(void) { return s_connected; }
bool wifi_manager_is_ap_mode(void)   { return s_ap_mode;   }

static void start_ap(void) {
    s_ap_mode = true;
    ESP_LOGI(TAG, "starting AP: %s", WIFI_AP_SSID);

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = WIFI_AP_SSID,
            .ssid_len       = strlen(WIFI_AP_SSID),
            .password       = WIFI_AP_PASS,
            .max_connection = 2,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();

    ui_show_status("Config: connect to " WIFI_AP_SSID);
    web_server_start();
}

static void event_handler(void *arg, esp_event_base_t base,
                           int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            s_connected = false;
            if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                g_state.wifi_connected = false;
                g_state.dirty = true;
                xSemaphoreGive(g_state_mutex);
            }
            if (s_retries++ < WIFI_MAX_RETRIES) {
                char msg[48];
                snprintf(msg, sizeof(msg), "WiFi retry %d/%d...", s_retries, WIFI_MAX_RETRIES);
                ui_show_status(msg);
                esp_wifi_connect();
            } else {
                ESP_LOGW(TAG, "max retries — switching to AP mode");
                esp_wifi_stop();
                start_ap();
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        s_retries   = 0;
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_state.wifi_connected = true;
            g_state.dirty = true;
            xSemaphoreGive(g_state_mutex);
        }
        ui_show_status("");
        web_server_start();
    }
}

esp_err_t wifi_manager_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, event_handler, NULL));

    char ssid[64] = {0}, pass[64] = {0};
    config_get_str(NVS_WIFI_SSID, ssid, sizeof(ssid));
    config_get_str(NVS_WIFI_PASS, pass, sizeof(pass));

    if (ssid[0] == '\0') {
        ESP_LOGW(TAG, "no WiFi credentials — going to AP mode immediately");
        start_ap();
        return ESP_OK;
    }

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid,     ssid, sizeof(sta_cfg.sta.ssid)     - 1);
    strncpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to %s...", ssid);
    return ESP_OK;
}

void wifi_manager_forget(void) {
    config_set_str(NVS_WIFI_SSID, "");
    config_set_str(NVS_WIFI_PASS, "");
    esp_restart();
}

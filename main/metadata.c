#include "metadata.h"
#include "app_config.h"
#include "wifi_manager.h"
#include "power.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "metadata";

// Pre-resolved at init from NVS
static char     s_amp_ip[64] = {0};
static uint32_t s_poll_ms    = DEFAULT_META_POLL_S * 1000;

// Static response buffer — sized for ~3 KB typical response with margin.
// Allocated once in BSS rather than on the task stack.
#define HTTP_BUF_SIZE 8192
static char s_http_buf[HTTP_BUF_SIZE];

// ---------------------------------------------------------------------------
// One HTTP fetch + JSON parse cycle. Fills g_state on success, no-op on fail.
// ---------------------------------------------------------------------------
static void fetch_now_playing(void) {
    char url[256];
    snprintf(url, sizeof(url),
             "http://%s:%d/api/getData?path=player%%3Aplayer%%2Fdata"
             "&roles=title%%2CmediaData%%2Cvalue",
             s_amp_ip, METADATA_HTTP_PORT);

    esp_http_client_config_t cfg = {
        .url               = url,
        .timeout_ms        = 5000,
        .disable_auto_redirect = true,
        .keep_alive_enable = false,    // amp's HTTP server doesn't like keep-alive
        .method            = HTTP_METHOD_GET,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return;
    esp_http_client_set_header(client, "User-Agent", "lyngdorf-knob/1.0");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Connection", "close");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    // Headers — the amp uses chunked transfer encoding (no Content-Length),
    // so fetch_headers may return 0. We rely on esp_http_client_read returning
    // 0 at EOF instead.
    esp_http_client_fetch_headers(client);

    int read_total = 0;
    while (read_total < HTTP_BUF_SIZE - 1) {
        int n = esp_http_client_read(client, s_http_buf + read_total,
                                      HTTP_BUF_SIZE - 1 - read_total);
        if (n <= 0) break;     // 0 = EOF, <0 = error
        read_total += n;
    }
    s_http_buf[read_total] = '\0';
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status != 200 || read_total <= 0) {
        ESP_LOGW(TAG, "HTTP %d, read %d bytes", status, read_total);
        return;
    }

    // The response is a top-level array with a few empty/placeholder slots
    // followed by the payload object. The payload is the first array element
    // that's an object containing a "trackRoles" key.
    cJSON *root = cJSON_Parse(s_http_buf);
    if (!root || !cJSON_IsArray(root)) {
        ESP_LOGW(TAG, "json parse failed");
        if (root) cJSON_Delete(root);
        return;
    }

    cJSON *payload = NULL;
    int sz = cJSON_GetArraySize(root);
    for (int i = 0; i < sz; i++) {
        cJSON *item = cJSON_GetArrayItem(root, i);
        if (cJSON_IsObject(item) && cJSON_GetObjectItem(item, "trackRoles")) {
            payload = item;
            break;
        }
    }

    if (!payload) {
        ESP_LOGW(TAG, "no payload object in response");
        cJSON_Delete(root);
        return;
    }

    cJSON *state_j   = cJSON_GetObjectItem(payload, "state");
    cJSON *track     = cJSON_GetObjectItem(payload, "trackRoles");
    cJSON *title_j   = track ? cJSON_GetObjectItem(track, "title") : NULL;
    cJSON *mediaData = track ? cJSON_GetObjectItem(track, "mediaData") : NULL;
    cJSON *metaData  = mediaData ? cJSON_GetObjectItem(mediaData, "metaData") : NULL;
    cJSON *artist_j  = metaData ? cJSON_GetObjectItem(metaData, "artist") : NULL;
    cJSON *album_j   = metaData ? cJSON_GetObjectItem(metaData, "album")  : NULL;

    const char *title  = (title_j  && cJSON_IsString(title_j))  ? title_j->valuestring  : "";
    const char *artist = (artist_j && cJSON_IsString(artist_j)) ? artist_j->valuestring : "";
    const char *album  = (album_j  && cJSON_IsString(album_j))  ? album_j->valuestring  : "";
    bool playing = (state_j && cJSON_IsString(state_j) &&
                    strcmp(state_j->valuestring, "playing") == 0);

    if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        bool changed = false;
        if (strncmp(g_state.title, title, sizeof(g_state.title)) != 0) {
            strncpy(g_state.title, title, sizeof(g_state.title) - 1);
            g_state.title[sizeof(g_state.title) - 1] = '\0';
            changed = true;
        }
        if (strncmp(g_state.artist, artist, sizeof(g_state.artist)) != 0) {
            strncpy(g_state.artist, artist, sizeof(g_state.artist) - 1);
            g_state.artist[sizeof(g_state.artist) - 1] = '\0';
            changed = true;
        }
        if (strncmp(g_state.album, album, sizeof(g_state.album)) != 0) {
            strncpy(g_state.album, album, sizeof(g_state.album) - 1);
            g_state.album[sizeof(g_state.album) - 1] = '\0';
            changed = true;
        }
        if (g_state.playing != playing) {
            g_state.playing = playing;
            changed = true;
        }
        if (changed) {
            g_state.dirty = true;
            ESP_LOGI(TAG, "now playing: %s — title='%s' artist='%s' album='%s'",
                     playing ? "play" : "stop", title, artist, album);
        }
        xSemaphoreGive(g_state_mutex);
    }

    cJSON_Delete(root);
}

// ---------------------------------------------------------------------------
// Polling task — runs at lower priority than net_task so encoder commands
// always preempt it.
// ---------------------------------------------------------------------------
static void metadata_task(void *arg) {
    // Wait for WiFi STA connection before the first fetch; we're started
    // before wifi_manager has finished its DHCP exchange.
    while (!wifi_manager_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    while (1) {
        fetch_now_playing();
        // Active tier: configured cadence (default 3 s).
        // Idle tier (panel asleep): stretch to 60 s — keeps the play/pause
        // state syncing but at much lower duty cycle.
        uint32_t period_ms = power_is_idle() ? 60000 : s_poll_ms;
        vTaskDelay(pdMS_TO_TICKS(period_ms));
    }
}

// ---------------------------------------------------------------------------
// Play/pause toggle. Hits the amp's setData endpoint with the same URL the
// amp's own web UI sends — a GET (despite the "setData" name) carrying a
// JSON value of {"control":"pause"}. The amp toggles between play/pause
// regardless of current state, so caller doesn't need to know it.
// ---------------------------------------------------------------------------
esp_err_t metadata_play_pause(void) {
    if (s_amp_ip[0] == '\0') return ESP_ERR_INVALID_STATE;

    // Optimistic state flip BEFORE the HTTP roundtrip — the request takes
    // 100-500 ms and waiting for it would make the icon change feel laggy.
    // The next metadata poll (within METADATA_POLL_S seconds) will correct
    // the state if the request actually failed.
    if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_state.playing = !g_state.playing;
        g_state.dirty = true;
        xSemaphoreGive(g_state_mutex);
    }

    char url[300];
    // path=player:player/control  roles=activate
    // value={"control":"pause"} (URL-encoded as %7B%22control%22%3A%22pause%22%7D)
    snprintf(url, sizeof(url),
             "http://%s:%d/api/setData"
             "?path=player%%3Aplayer%%2Fcontrol"
             "&roles=activate"
             "&value=%%7B%%22control%%22%%3A%%22pause%%22%%7D",
             s_amp_ip, METADATA_HTTP_PORT);

    esp_http_client_config_t cfg = {
        .url               = url,
        .timeout_ms        = 1500,
        .keep_alive_enable = false,
        .method            = HTTP_METHOD_GET,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;
    esp_http_client_set_header(client, "User-Agent", "lyngdorf-knob/1.0");
    esp_http_client_set_header(client, "Connection", "close");

    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "play/pause request failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "play/pause HTTP %d", status);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "play/pause toggled");
    return ESP_OK;
}

esp_err_t metadata_init(void) {
    config_get_str(NVS_AMP_IP, s_amp_ip, sizeof(s_amp_ip));
    if (s_amp_ip[0] == '\0') {
        ESP_LOGW(TAG, "no amp IP configured — metadata disabled");
        return ESP_OK;
    }

    uint32_t secs = DEFAULT_META_POLL_S;
    config_get_u32(NVS_META_POLL_S, &secs, DEFAULT_META_POLL_S);
    if (secs < 1)   secs = 1;       // sanity floor
    if (secs > 60)  secs = 60;      // sanity ceiling
    s_poll_ms = secs * 1000;

    // Lower priority than net_task (priority 2) so RIO TCP work always wins.
    xTaskCreatePinnedToCore(metadata_task, "metadata", 8192, NULL, 1, NULL, 0);
    ESP_LOGI(TAG, "metadata task started, poll every %lu s", (unsigned long)secs);
    return ESP_OK;
}

#include "lyngdorf.h"
#include "app_config.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "lyngdorf";

static int     s_sock = -1;
static char    s_amp_ip[64] = {0};

// ---------------------------------------------------------------------------
// Low-level socket helpers
// ---------------------------------------------------------------------------
static void lyngdorf_close(void) {
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_state.amp_connected = false;
        g_state.dirty = true;
        xSemaphoreGive(g_state_mutex);
    }
}

static esp_err_t lyngdorf_connect(void) {
    if (s_sock >= 0) return ESP_OK;
    if (s_amp_ip[0] == '\0') return ESP_ERR_INVALID_STATE;

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port   = htons(AMP_TCP_PORT),
    };
    if (inet_pton(AF_INET, s_amp_ip, &dest.sin_addr) != 1) {
        ESP_LOGE(TAG, "invalid IP: %s", s_amp_ip);
        return ESP_ERR_INVALID_ARG;
    }

    s_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_sock < 0) return ESP_FAIL;

    struct timeval tv = { .tv_sec = 3 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(s_sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        ESP_LOGW(TAG, "connect to %s:84 failed", s_amp_ip);
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "connected to %s:84", s_amp_ip);
    if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_state.amp_connected = true;
        g_state.dirty = true;
        xSemaphoreGive(g_state_mutex);
    }
    return ESP_OK;
}

// Send a Lyngdorf command (terminated with CR as per protocol spec)
static esp_err_t lyngdorf_send_cmd(const char *cmd) {
    if (lyngdorf_connect() != ESP_OK) return ESP_FAIL;

    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%s\r", cmd);
    if (send(s_sock, buf, len, 0) < 0) {
        ESP_LOGW(TAG, "send failed, closing");
        lyngdorf_close();
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Read response lines into buf (up to len bytes, stops at \r or timeout)
static int lyngdorf_recv_line(char *buf, size_t len) {
    size_t pos = 0;
    while (pos < len - 1) {
        char c;
        int n = recv(s_sock, &c, 1, 0);
        if (n <= 0) break;
        if (c == '\r' || c == '\n') {
            if (pos > 0) break;
            continue;
        }
        buf[pos++] = c;
    }
    buf[pos] = '\0';
    return (int)pos;
}

// ---------------------------------------------------------------------------
// Parse helpers
// ---------------------------------------------------------------------------

// Extract integer from "!VOL(-300)" or "#VOL(-300)"
static bool parse_vol_response(const char *line, int32_t *out) {
    const char *p = strstr(line, "VOL(");
    if (!p) return false;
    p += 4;
    *out = (int32_t)atoi(p);
    return true;
}

static bool parse_mute_response(const char *line, bool *out) {
    if (strstr(line, "!MUTE") || strstr(line, "#MUTE")) {
        *out = true;
        return true;
    }
    if (strstr(line, "!UNMUTE") || strstr(line, "#UNMUTE")) {
        *out = false;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t lyngdorf_init(void) {
    config_get_str(NVS_AMP_IP, s_amp_ip, sizeof(s_amp_ip));
    if (s_amp_ip[0] == '\0') {
        ESP_LOGW(TAG, "no amp IP configured — set via web UI");
    } else {
        ESP_LOGI(TAG, "amp IP: %s", s_amp_ip);
    }
    return ESP_OK;
}

void lyngdorf_vol_delta(int32_t delta_db10) {
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "!VOLCH(%d)", (int)delta_db10);
    if (lyngdorf_send_cmd(cmd) == ESP_OK) {
        // Optimistic local update
        if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_state.vol_db10 += delta_db10;
            g_state.dirty = true;
            xSemaphoreGive(g_state_mutex);
        }
    }
}

void lyngdorf_mute_toggle(void) {
    bool currently_muted;
    if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        currently_muted = g_state.muted;
        xSemaphoreGive(g_state_mutex);
    } else {
        return;
    }

    const char *cmd = currently_muted ? "!UNMUTE" : "!MUTE";
    if (lyngdorf_send_cmd(cmd) == ESP_OK) {
        if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_state.muted = !currently_muted;
            g_state.dirty = true;
            xSemaphoreGive(g_state_mutex);
        }
    }
}

// Query current vol and mute state from amp; updates g_state
void lyngdorf_poll_state(void) {
    if (s_amp_ip[0] == '\0') return;

    if (lyngdorf_send_cmd("!VOL?") != ESP_OK) return;

    char line[64];
    // Read up to 4 response lines (amp echoes + response)
    for (int i = 0; i < 4; i++) {
        int n = lyngdorf_recv_line(line, sizeof(line));
        if (n <= 0) break;
        ESP_LOGD(TAG, "rx: %s", line);

        int32_t vol;
        bool muted;
        if (parse_vol_response(line, &vol)) {
            if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                if (g_state.vol_db10 != vol) { g_state.vol_db10 = vol; g_state.dirty = true; }
                xSemaphoreGive(g_state_mutex);
            }
        } else if (parse_mute_response(line, &muted)) {
            if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                if (g_state.muted != muted) { g_state.muted = muted; g_state.dirty = true; }
                xSemaphoreGive(g_state_mutex);
            }
        }
    }
}

void lyngdorf_disconnect(void) {
    lyngdorf_close();
}

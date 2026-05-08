#include "lyngdorf.h"
#include "app_config.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include <errno.h>
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

    // Short recv timeout — net_task is blocked while we wait, so a long
    // timeout makes the encoder queue lag every poll cycle. 250 ms is
    // plenty for the amp's reply (typical RTT < 50 ms).
    struct timeval rcv_tv = { .tv_sec = 0, .tv_usec = 250 * 1000 };
    struct timeval snd_tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));
    setsockopt(s_sock, SOL_SOCKET, SO_SNDTIMEO, &snd_tv, sizeof(snd_tv));

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
    if (lyngdorf_connect() != ESP_OK) {
        ESP_LOGW(TAG, "send '%s' aborted: not connected", cmd);
        return ESP_FAIL;
    }

    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%s\r", cmd);
    int n = send(s_sock, buf, len, 0);
    if (n < 0) {
        ESP_LOGW(TAG, "send '%s' failed (errno %d), closing", cmd, errno);
        lyngdorf_close();
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "tx: %s (%d bytes)", cmd, n);
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
    // Actual Lyngdorf RIO format on this amp: `!MUTE(ON)` or `!MUTE(OFF)`
    // (also `#MUTE(...)` for echoes). Check OFF first since "ON" is a
    // substring of nothing relevant here but be defensive.
    if (strstr(line, "MUTE(OFF)")) { *out = false; return true; }
    if (strstr(line, "MUTE(ON)"))  { *out = true;  return true; }
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

    const char *cmd = currently_muted ? "!MUTE(OFF)" : "!MUTE(ON)";
    if (lyngdorf_send_cmd(cmd) == ESP_OK) {
        if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_state.muted = !currently_muted;
            g_state.dirty = true;
            xSemaphoreGive(g_state_mutex);
        }
    }
}

// Query current vol & mute state from amp; updates g_state.
// Track metadata is fetched separately by metadata_task via the amp's HTTP
// JSON API (RIO doesn't expose track info on this firmware).
//
// Reads up to 8 lines (the amp may interleave echoes / unsolicited state
// notifications with our query replies). Each parsed value overwrites the
// previous one — only the LAST observed value is committed to g_state, in
// a single mutex-protected write at the end. This avoids a flicker where
// the UI was reading intermediate values mid-poll.
void lyngdorf_poll_state(void) {
    if (s_amp_ip[0] == '\0') return;

    if (lyngdorf_send_cmd("!VOL?")  != ESP_OK) return;
    if (lyngdorf_send_cmd("!MUTE?") != ESP_OK) return;

    char line[64];
    bool    got_vol = false,  got_mute = false;
    int32_t latest_vol = 0;
    bool    latest_muted = false;

    for (int i = 0; i < 8; i++) {
        int n = lyngdorf_recv_line(line, sizeof(line));
        if (n <= 0) break;   // recv timeout — amp has stopped sending
        ESP_LOGD(TAG, "rx: %s", line);

        int32_t vol;
        bool muted;
        if (parse_vol_response(line, &vol)) {
            latest_vol = vol;
            got_vol = true;
        } else if (parse_mute_response(line, &muted)) {
            latest_muted = muted;
            got_mute = true;
        }
    }

    if ((got_vol || got_mute) &&
        xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (got_vol && g_state.vol_db10 != latest_vol) {
            g_state.vol_db10 = latest_vol;
            g_state.dirty = true;
        }
        if (got_mute && g_state.muted != latest_muted) {
            g_state.muted = latest_muted;
            g_state.dirty = true;
        }
        xSemaphoreGive(g_state_mutex);
    }
}

void lyngdorf_disconnect(void) {
    lyngdorf_close();
}

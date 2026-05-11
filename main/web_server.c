#include "web_server.h"
#include "app_config.h"
#include "log_buffer.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG       = "web";
static httpd_handle_t s_srv  = NULL;

// ---------------------------------------------------------------------------
// Minimal URL decoder
// ---------------------------------------------------------------------------
static void url_decode(const char *src, char *dst, size_t dst_len) {
    size_t i = 0;
    while (*src && i < dst_len - 1) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], '\0' };
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

static bool get_field(const char *body, const char *name, char *out, size_t out_len) {
    char search[64];
    snprintf(search, sizeof(search), "%s=", name);
    const char *p = strstr(body, search);
    if (!p) { out[0] = '\0'; return false; }
    p += strlen(search);
    char raw[256] = {0};
    size_t i = 0;
    while (*p && *p != '&' && i < sizeof(raw) - 1) raw[i++] = *p++;
    url_decode(raw, out, out_len);
    return true;
}

// ---------------------------------------------------------------------------
// Config HTML page
// ---------------------------------------------------------------------------
// Form layout: each input is a fixed-width box on the LEFT, with its label
// (plus optional helper <small>) to the RIGHT. Reads more like a settings
// page on a phone — inputs line up in a clean column you scan vertically,
// captions explain what each one does without crowding the input.
static const char *HTML_HEAD =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>LyngdorfKnob</title>"
    "<style>"
    "body{background:#111;color:#ddd;font-family:sans-serif;max-width:520px;margin:40px auto;padding:0 16px}"
    "h1{color:#00bfbf;font-size:1.4em;margin-bottom:4px}"
    "p.sub{color:#666;font-size:.85em;margin-top:0}"
    "form{margin-top:24px}"
    ".row{display:flex;align-items:flex-start;gap:14px;margin-top:14px;cursor:pointer}"
    ".row input{flex:0 0 150px;width:150px;box-sizing:border-box;padding:9px 10px;"
        "background:#222;border:1px solid #333;border-radius:4px;color:#eee;font-size:1em}"
    ".row input:focus{outline:none;border-color:#00bfbf}"
    ".row .lbl{flex:1;padding-top:9px;font-size:.95em;color:#ccc;line-height:1.35}"
    ".row .lbl small{display:block;margin-top:4px;font-size:.8em;color:#666;line-height:1.4}"
    "button{margin-top:28px;width:100%%;padding:12px;background:#00bfbf;border:none;"
        "border-radius:4px;color:#111;font-size:1em;font-weight:bold;cursor:pointer}"
    "button:hover{background:#00a0a0}"
    ".note{margin-top:8px;font-size:.8em;color:#555}"
    "@media (max-width:420px){"
        ".row{flex-direction:column;gap:4px}"
        ".row input{flex:0 0 auto;width:100%%}"
        ".row .lbl{padding-top:0}"
    "}"
    "</style></head><body>"
    "<h1>LyngdorfKnob</h1><p class='sub'>Lyngdorf amplifier controller</p>"
    "<form method='POST' action='/save'>";

static const char *HTML_FOOT =
    "<button type='submit'>Save &amp; Restart</button></form>"
    "<p class='note'>Tap = mute &nbsp;|&nbsp; Rotate = volume</p>"
    "<p class='note'><a style='color:#00bfbf;text-decoration:none' "
        "href='https://github.com/svwhisper/lyngdorf-knob' "
        "target='_blank' rel='noopener'>Project on GitHub &rsaquo;</a></p>"
    "</body></html>";

// ---------------------------------------------------------------------------
// GET / — serve config form
// ---------------------------------------------------------------------------
static esp_err_t get_handler(httpd_req_t *req) {
    char ssid[64]={0}, pass[64]={0}, amp_ip[64]={0};
    uint32_t vol_step       = DEFAULT_VOL_STEP;
    uint32_t dim_secs       = DEFAULT_DIM_SECS;
    uint32_t sleep_secs     = DEFAULT_SLEEP_SECS;
    uint32_t deep_after_s   = DEFAULT_DEEP_AFTER_S;
    uint32_t paused_grace_s = DEFAULT_PAUSED_GRACE_S;
    uint32_t meta_poll_s    = DEFAULT_META_POLL_S;

    config_get_str(NVS_WIFI_SSID, ssid,   sizeof(ssid));
    config_get_str(NVS_WIFI_PASS, pass,   sizeof(pass));
    config_get_str(NVS_AMP_IP,    amp_ip, sizeof(amp_ip));
    config_get_u32(NVS_VOL_STEP,       &vol_step,       DEFAULT_VOL_STEP);
    config_get_u32(NVS_DIM_SECS,       &dim_secs,       DEFAULT_DIM_SECS);
    config_get_u32(NVS_SLEEP_SECS,     &sleep_secs,     DEFAULT_SLEEP_SECS);
    config_get_u32(NVS_DEEP_AFTER_S,   &deep_after_s,   DEFAULT_DEEP_AFTER_S);
    config_get_u32(NVS_PAUSED_GRACE_S, &paused_grace_s, DEFAULT_PAUSED_GRACE_S);
    config_get_u32(NVS_META_POLL_S,    &meta_poll_s,    DEFAULT_META_POLL_S);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, HTML_HEAD);

    // BSS-resident — handler is serialised by the httpd task, so reuse is safe.
    // Stack-resident copies overflowed the 4 KB httpd task stack once buf2 grew
    // to fit the deep-sleep config fields.
    // Each field is rendered as <label class='row'><input ...><span class='lbl'>...</span></label>.
    // Wrapping the row in a <label> means clicking the caption focuses the
    // matching input, same as a normal stacked form.
    static char buf[1024];
    snprintf(buf, sizeof(buf),
        "<label class='row'><input name='ssid' value='%s'>"
            "<span class='lbl'>WiFi SSID</span></label>"
        "<label class='row'><input name='pass' type='password' value='%s'>"
            "<span class='lbl'>WiFi Password</span></label>"
        "<label class='row'><input name='amp_ip' value='%s' placeholder='192.168.1.x'>"
            "<span class='lbl'>Amp IP</span></label>"
        "<label class='row'><input name='vol_step' type='number' min='1' max='50' value='%lu'>"
            "<span class='lbl'>Vol Step"
            "<small>0.1 dB units, e.g. 10 = 1.0 dB / detent</small></span></label>"
        "<label class='row'><input name='meta_poll_s' type='number' min='1' max='60' value='%lu'>"
            "<span class='lbl'>Track-info refresh"
            "<small>Seconds between metadata polls, 1–60.</small></span></label>",
        ssid, pass, amp_ip, (unsigned long)vol_step, (unsigned long)meta_poll_s);
    httpd_resp_sendstr_chunk(req, buf);

    static char buf2[1280];
    snprintf(buf2, sizeof(buf2),
        "<label class='row'><input name='dim_secs' type='number' min='0' max='3600' value='%lu'>"
            "<span class='lbl'>Dim display after"
            "<small>Seconds idle before the backlight dims. 0 disables.</small></span></label>"
        "<label class='row'><input name='sleep_secs' type='number' min='0' max='3600' value='%lu'>"
            "<span class='lbl'>Sleep display after"
            "<small>Seconds idle before the panel turns off. 0 disables.</small></span></label>"
        "<label class='row'><input name='deep_after_s' type='number' min='0' max='3600' value='%lu'>"
            "<span class='lbl'>Deep sleep after"
            "<small>Extra seconds in panel-sleep before the chip enters deep sleep. "
            "Lower = more battery saved; higher = no ~3 s WiFi reconnect delay on "
            "next interaction. Breakeven vs. idle is ~25 s. 0 disables deep sleep.</small>"
            "</span></label>"
        "<label class='row'><input name='paused_grace_s' type='number' min='0' max='3600' value='%lu'>"
            "<span class='lbl'>Paused grace"
            "<small>Seconds the amp must be not playing before deep sleep is allowed. "
            "Keeps the knob responsive while music is playing. 0 ignores the amp.</small>"
            "</span></label>",
        (unsigned long)dim_secs, (unsigned long)sleep_secs,
        (unsigned long)deep_after_s, (unsigned long)paused_grace_s);
    httpd_resp_sendstr_chunk(req, buf2);

    httpd_resp_sendstr_chunk(req, HTML_FOOT);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST /save — persist and reboot
// ---------------------------------------------------------------------------
static esp_err_t post_handler(httpd_req_t *req) {
    static char body[1024];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) return ESP_FAIL;
    body[len] = '\0';

    char ssid[64]={0}, pass[64]={0}, amp_ip[64]={0};
    char vol_s[8]={0}, dim_s[8]={0}, sleep_s[8]={0}, meta_s[8]={0};
    char deep_s[8]={0}, grace_s[8]={0};
    get_field(body, "ssid",           ssid,    sizeof(ssid));
    get_field(body, "pass",           pass,    sizeof(pass));
    get_field(body, "amp_ip",         amp_ip,  sizeof(amp_ip));
    get_field(body, "vol_step",       vol_s,   sizeof(vol_s));
    get_field(body, "dim_secs",       dim_s,   sizeof(dim_s));
    get_field(body, "sleep_secs",     sleep_s, sizeof(sleep_s));
    get_field(body, "deep_after_s",   deep_s,  sizeof(deep_s));
    get_field(body, "paused_grace_s", grace_s, sizeof(grace_s));
    get_field(body, "meta_poll_s",    meta_s,  sizeof(meta_s));

    if (ssid[0])    config_set_str(NVS_WIFI_SSID, ssid);
    if (pass[0])    config_set_str(NVS_WIFI_PASS, pass);
    if (amp_ip[0])  config_set_str(NVS_AMP_IP,   amp_ip);
    if (vol_s[0])   config_set_u32(NVS_VOL_STEP,       (uint32_t)atoi(vol_s));
    if (dim_s[0])   config_set_u32(NVS_DIM_SECS,       (uint32_t)atoi(dim_s));
    if (sleep_s[0]) config_set_u32(NVS_SLEEP_SECS,     (uint32_t)atoi(sleep_s));
    if (deep_s[0])  config_set_u32(NVS_DEEP_AFTER_S,   (uint32_t)atoi(deep_s));
    if (grace_s[0]) config_set_u32(NVS_PAUSED_GRACE_S, (uint32_t)atoi(grace_s));
    if (meta_s[0])  config_set_u32(NVS_META_POLL_S,    (uint32_t)atoi(meta_s));

    ESP_LOGI(TAG, "config saved — rebooting");

    const char *resp =
        "<!DOCTYPE html><html><body style='background:#111;color:#ddd;font-family:sans-serif;"
        "text-align:center;padding-top:80px'>"
        "<h2 style='color:#00bfbf'>Saved. Restarting...</h2></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, resp);

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// GET /log — dumps the in-memory log ring buffer as plain text. Useful when
// the device is on battery (no USB-CDC monitor available). Visit the URL in
// a browser, or `curl http://<device-ip>/log` from a shell.
// ---------------------------------------------------------------------------
static esp_err_t log_handler(httpd_req_t *req) {
    static char buf[8192];   // sized to match the ring buffer
    size_t n = log_buffer_dump(buf, sizeof(buf));
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    // No-cache so browser refresh always fetches fresh data
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
esp_err_t web_server_start(void) {
    if (s_srv) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;

    if (httpd_start(&s_srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return ESP_FAIL;
    }

    httpd_uri_t get_uri  = { .uri = "/",     .method = HTTP_GET,  .handler = get_handler  };
    httpd_uri_t post_uri = { .uri = "/save", .method = HTTP_POST, .handler = post_handler };
    httpd_uri_t log_uri  = { .uri = "/log",  .method = HTTP_GET,  .handler = log_handler  };
    httpd_register_uri_handler(s_srv, &get_uri);
    httpd_register_uri_handler(s_srv, &post_uri);
    httpd_register_uri_handler(s_srv, &log_uri);

    ESP_LOGI(TAG, "config server at http://<device-ip>/  (logs at /log)");
    return ESP_OK;
}

void web_server_stop(void) {
    if (s_srv) { httpd_stop(s_srv); s_srv = NULL; }
}

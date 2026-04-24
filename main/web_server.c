#include "web_server.h"
#include "app_config.h"

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
static const char *HTML_HEAD =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>LyngdorfKnob</title>"
    "<style>"
    "body{background:#111;color:#ddd;font-family:sans-serif;max-width:420px;margin:40px auto;padding:0 16px}"
    "h1{color:#00bfbf;font-size:1.4em;margin-bottom:4px}"
    "p.sub{color:#666;font-size:.85em;margin-top:0}"
    "label{display:block;margin-top:16px;font-size:.9em;color:#aaa}"
    "input{width:100%%;box-sizing:border-box;padding:9px 10px;background:#222;border:1px solid #333;"
    "border-radius:4px;color:#eee;font-size:1em;margin-top:4px}"
    "input:focus{outline:none;border-color:#00bfbf}"
    "button{margin-top:24px;width:100%%;padding:12px;background:#00bfbf;border:none;"
    "border-radius:4px;color:#111;font-size:1em;font-weight:bold;cursor:pointer}"
    "button:hover{background:#00a0a0}"
    ".note{margin-top:8px;font-size:.8em;color:#555}"
    "</style></head><body>"
    "<h1>LyngdorfKnob</h1><p class='sub'>Lyngdorf TDAI-3400 controller</p>"
    "<form method='POST' action='/save'>";

static const char *HTML_FOOT =
    "<button type='submit'>Save &amp; Restart</button></form>"
    "<p class='note'>Single tap = mute &nbsp;|&nbsp; Double tap = play/pause &nbsp;|&nbsp; Rotate = volume</p>"
    "</body></html>";

// ---------------------------------------------------------------------------
// GET / — serve config form
// ---------------------------------------------------------------------------
static esp_err_t get_handler(httpd_req_t *req) {
    char ssid[64]={0}, pass[64]={0}, amp_ip[64]={0}, upnp_url[256]={0};
    uint32_t vol_step = DEFAULT_VOL_STEP;
    uint32_t dim_secs = DEFAULT_DIM_SECS, sleep_secs = DEFAULT_SLEEP_SECS;
    uint32_t haptic_en = DEFAULT_HAPTIC_EN;

    config_get_str(NVS_WIFI_SSID, ssid,     sizeof(ssid));
    config_get_str(NVS_WIFI_PASS, pass,     sizeof(pass));
    config_get_str(NVS_AMP_IP,    amp_ip,   sizeof(amp_ip));
    config_get_str(NVS_UPNP_URL,  upnp_url, sizeof(upnp_url));
    config_get_u32(NVS_VOL_STEP,   &vol_step,   DEFAULT_VOL_STEP);
    config_get_u32(NVS_DIM_SECS,   &dim_secs,   DEFAULT_DIM_SECS);
    config_get_u32(NVS_SLEEP_SECS, &sleep_secs, DEFAULT_SLEEP_SECS);
    config_get_u32(NVS_HAPTIC_EN,  &haptic_en,  DEFAULT_HAPTIC_EN);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, HTML_HEAD);

    char buf[1024];
    snprintf(buf, sizeof(buf),
        "<label>WiFi SSID<input name='ssid' value='%s'></label>"
        "<label>WiFi Password<input name='pass' type='password' value='%s'></label>"
        "<label>Amp IP<input name='amp_ip' value='%s' placeholder='192.168.1.x'></label>"
        "<label>Vol Step (0.1 dB units, e.g. 5=0.5dB/detent)"
        "<input name='vol_step' type='number' min='1' max='50' value='%lu'></label>"
        "<label>UPnP URL (blank=auto-discover)"
        "<input name='upnp_url' value='%s' placeholder='http://x.x.x.x:port/path'></label>",
        ssid, pass, amp_ip, (unsigned long)vol_step, upnp_url);
    httpd_resp_sendstr_chunk(req, buf);

    char buf2[512];
    snprintf(buf2, sizeof(buf2),
        "<label>Dim display after (seconds, 0=off)"
        "<input name='dim_secs' type='number' min='0' max='3600' value='%lu'></label>"
        "<label>Sleep display after (seconds, 0=off)"
        "<input name='sleep_secs' type='number' min='0' max='3600' value='%lu'></label>"
        "<label style='margin-top:20px;display:flex;align-items:center;gap:8px'>"
        "<input name='haptic_en' type='checkbox' value='1'%s style='width:auto'>"
        "Haptic feedback on encoder rotation</label>",
        (unsigned long)dim_secs, (unsigned long)sleep_secs,
        haptic_en ? " checked" : "");
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

    char ssid[64]={0}, pass[64]={0}, amp_ip[64]={0}, upnp_url[256]={0};
    char vol_s[8]={0}, dim_s[8]={0}, sleep_s[8]={0}, haptic_s[4]={0};
    get_field(body, "ssid",       ssid,     sizeof(ssid));
    get_field(body, "pass",       pass,     sizeof(pass));
    get_field(body, "amp_ip",     amp_ip,   sizeof(amp_ip));
    get_field(body, "upnp_url",   upnp_url, sizeof(upnp_url));
    get_field(body, "vol_step",   vol_s,    sizeof(vol_s));
    get_field(body, "dim_secs",   dim_s,    sizeof(dim_s));
    get_field(body, "sleep_secs", sleep_s,  sizeof(sleep_s));
    // Checkbox is absent from POST body when unchecked — treat absence as 0
    bool haptic_checked = get_field(body, "haptic_en", haptic_s, sizeof(haptic_s));

    if (ssid[0])    config_set_str(NVS_WIFI_SSID, ssid);
    if (pass[0])    config_set_str(NVS_WIFI_PASS, pass);
    if (amp_ip[0])  config_set_str(NVS_AMP_IP,   amp_ip);
    config_set_str(NVS_UPNP_URL, upnp_url);
    if (vol_s[0])   config_set_u32(NVS_VOL_STEP,   (uint32_t)atoi(vol_s));
    if (dim_s[0])   config_set_u32(NVS_DIM_SECS,   (uint32_t)atoi(dim_s));
    if (sleep_s[0]) config_set_u32(NVS_SLEEP_SECS, (uint32_t)atoi(sleep_s));
    config_set_u32(NVS_HAPTIC_EN, haptic_checked ? 1 : 0);

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
    httpd_register_uri_handler(s_srv, &get_uri);
    httpd_register_uri_handler(s_srv, &post_uri);

    ESP_LOGI(TAG, "config server at http://<device-ip>/");
    return ESP_OK;
}

void web_server_stop(void) {
    if (s_srv) { httpd_stop(s_srv); s_srv = NULL; }
}

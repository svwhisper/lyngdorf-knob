#include "upnp.h"
#include "app_config.h"

#include "esp_http_client.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "upnp";

#define SSDP_ADDR       "239.255.255.250"
#define SSDP_PORT       1900
#define SSDP_TIMEOUT_MS 4000

// Stored AVTransport control URL (discovered or configured)
static char s_control_url[256] = {0};
static bool s_playing_last = false;

// ---------------------------------------------------------------------------
// Simple XML tag extractor (handles dc:title, upnp:artist style namespaces)
// ---------------------------------------------------------------------------
static bool extract_tag(const char *xml, const char *tag, char *out, size_t out_len) {
    char open[80], close[80];
    snprintf(open,  sizeof(open),  "<%s>",  tag);
    snprintf(close, sizeof(close), "</%s>", tag);

    const char *start = strstr(xml, open);
    if (!start) return false;
    start += strlen(open);

    const char *end = strstr(start, close);
    if (!end) return false;

    size_t len = (size_t)(end - start);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

// URL-decode in-place (for DIDL-Lite metadata that may be URL-encoded)
static void url_decode(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && r[1] && r[2]) {
            char hex[3] = { r[1], r[2], '\0' };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 3;
        } else if (*r == '+') {
            *w++ = ' ';
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

// ---------------------------------------------------------------------------
// SSDP discovery — find amp's AVTransport control URL
// ---------------------------------------------------------------------------
static esp_err_t ssdp_discover(char *location, size_t loc_len) {
    const char *msearch =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 3\r\n"
        "ST: urn:schemas-upnp-org:service:AVTransport:1\r\n\r\n";

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return ESP_FAIL;

    struct timeval tv = { .tv_sec  = SSDP_TIMEOUT_MS / 1000,
                          .tv_usec = (SSDP_TIMEOUT_MS % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int bcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port   = htons(SSDP_PORT),
    };
    inet_pton(AF_INET, SSDP_ADDR, &dest.sin_addr);

    if (sendto(sock, msearch, strlen(msearch), 0, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        close(sock);
        return ESP_FAIL;
    }

    char buf[1024];
    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);
    int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&src, &src_len);
    close(sock);

    if (n <= 0) return ESP_ERR_TIMEOUT;
    buf[n] = '\0';

    // Find LOCATION header
    const char *loc = strstr(buf, "LOCATION:");
    if (!loc) loc = strstr(buf, "location:");
    if (!loc) return ESP_FAIL;
    loc += 9;
    while (*loc == ' ') loc++;

    size_t i = 0;
    while (*loc && *loc != '\r' && *loc != '\n' && i < loc_len - 1)
        location[i++] = *loc++;
    location[i] = '\0';

    ESP_LOGI(TAG, "SSDP found device at: %s", location);
    return ESP_OK;
}

// Fetch device description XML and extract AVTransport control URL
static esp_err_t fetch_control_url(const char *desc_url, char *ctrl_url, size_t len) {
    static char body[4096];
    int body_len = 0;

    esp_http_client_config_t cfg = { .url = desc_url, .timeout_ms = 5000 };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        body_len = esp_http_client_fetch_headers(client);
        if (body_len > 0 && body_len < (int)sizeof(body)) {
            esp_http_client_read_response(client, body, body_len);
            body[body_len] = '\0';
        }
    }
    esp_http_client_cleanup(client);

    if (body_len <= 0) return ESP_FAIL;

    // Find AVTransport service block, then its <controlURL>
    const char *avt = strstr(body, "AVTransport");
    if (!avt) return ESP_FAIL;

    char rel_url[128] = {0};
    if (!extract_tag(avt, "controlURL", rel_url, sizeof(rel_url))) return ESP_FAIL;

    // Build absolute URL from base of desc_url + relative path
    // Extract base: scheme://host:port
    char base[128] = {0};
    const char *path_start = strchr(desc_url + 8, '/'); // skip "http://"
    if (path_start) {
        size_t base_len = (size_t)(path_start - desc_url);
        if (base_len >= sizeof(base)) base_len = sizeof(base) - 1;
        memcpy(base, desc_url, base_len);
        base[base_len] = '\0';
    } else {
        strncpy(base, desc_url, sizeof(base) - 1);
    }

    snprintf(ctrl_url, len, "%s%s%s",
             base,
             (rel_url[0] == '/') ? "" : "/",
             rel_url);

    ESP_LOGI(TAG, "AVTransport control URL: %s", ctrl_url);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// SOAP helper
// ---------------------------------------------------------------------------
#define SOAP_ENVELOPE_FMT \
    "<?xml version=\"1.0\"?>" \
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"" \
    " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">" \
    "<s:Body>%s</s:Body></s:Envelope>"

static esp_err_t soap_call(const char *url, const char *action,
                            const char *body_inner,
                            char *resp_buf, size_t resp_len) {
    static char envelope[1024];
    snprintf(envelope, sizeof(envelope), SOAP_ENVELOPE_FMT, body_inner);

    char soap_action[128];
    snprintf(soap_action, sizeof(soap_action),
             "\"urn:schemas-upnp-org:service:AVTransport:1#%s\"", action);

    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 5000 };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "text/xml; charset=\"utf-8\"");
    esp_http_client_set_header(client, "SOAPAction", soap_action);
    esp_http_client_set_post_field(client, envelope, strlen(envelope));

    esp_err_t err = esp_http_client_open(client, strlen(envelope));
    if (err == ESP_OK) {
        int len = esp_http_client_fetch_headers(client);
        if (resp_buf && resp_len > 0 && len > 0) {
            int n = (len < (int)resp_len - 1) ? len : (int)resp_len - 1;
            esp_http_client_read_response(client, resp_buf, n);
            resp_buf[n] = '\0';
        }
    }
    esp_http_client_cleanup(client);
    return err;
}

// ---------------------------------------------------------------------------
// Metadata extraction from GetPositionInfo response
// ---------------------------------------------------------------------------
static uint32_t parse_time(const char *s) {
    unsigned h = 0, m = 0, sec = 0;
    sscanf(s, "%u:%u:%u", &h, &m, &sec);
    return h * 3600 + m * 60 + sec;
}

static void parse_metadata(const char *resp) {
    // TrackMetaData contains DIDL-Lite, may be URL-encoded
    static char meta[2048];
    if (!extract_tag(resp, "TrackMetaData", meta, sizeof(meta))) return;
    url_decode(meta);

    char artist[64] = {0}, album[64] = {0}, title[96] = {0};
    // Try namespaced and bare tag variants
    if (!extract_tag(meta, "dc:title",    title,  sizeof(title)))
         extract_tag(meta, "title",       title,  sizeof(title));
    if (!extract_tag(meta, "upnp:artist", artist, sizeof(artist)))
         extract_tag(meta, "artist",      artist, sizeof(artist));
    if (!extract_tag(meta, "upnp:album",  album,  sizeof(album)))
         extract_tag(meta, "album",       album,  sizeof(album));

    if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        bool changed = (strcmp(g_state.title,  title)  != 0 ||
                        strcmp(g_state.artist, artist) != 0 ||
                        strcmp(g_state.album,  album)  != 0);
        if (changed) {
            strncpy(g_state.title,  title,  sizeof(g_state.title)  - 1);
            strncpy(g_state.artist, artist, sizeof(g_state.artist) - 1);
            strncpy(g_state.album,  album,  sizeof(g_state.album)  - 1);
            g_state.dirty = true;
        }
        xSemaphoreGive(g_state_mutex);
    }

    // Parse position / duration from RelTime / TrackDuration
    char pos_s[16] = {0}, dur_s[16] = {0};
    extract_tag(resp, "RelTime",       pos_s, sizeof(pos_s));
    extract_tag(resp, "TrackDuration", dur_s, sizeof(dur_s));

    uint32_t pos = parse_time(pos_s);
    uint32_t dur = parse_time(dur_s);

    if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_state.position_sec = pos;
        g_state.duration_sec = dur;
        xSemaphoreGive(g_state_mutex);
    }
}

// ---------------------------------------------------------------------------
// Transport state detection
// ---------------------------------------------------------------------------
static void parse_transport_state(const char *resp, bool *playing) {
    if (strstr(resp, "PLAYING"))      *playing = true;
    else if (strstr(resp, "PAUSED"))  *playing = false;
    else if (strstr(resp, "STOPPED")) *playing = false;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t upnp_init(void) {
    // Try configured URL first
    config_get_str(NVS_UPNP_URL, s_control_url, sizeof(s_control_url));
    if (s_control_url[0] != '\0') {
        ESP_LOGI(TAG, "using configured UPnP URL: %s", s_control_url);
        if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_state.upnp_available = true;
            xSemaphoreGive(g_state_mutex);
        }
        return ESP_OK;
    }

    // Auto-discover via SSDP
    char location[256] = {0};
    if (ssdp_discover(location, sizeof(location)) != ESP_OK) {
        ESP_LOGW(TAG, "SSDP discovery timed out — will retry in poll loop");
        return ESP_OK;
    }

    if (fetch_control_url(location, s_control_url, sizeof(s_control_url)) == ESP_OK) {
        if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_state.upnp_available = true;
            xSemaphoreGive(g_state_mutex);
        }
    }
    return ESP_OK;
}

void upnp_poll_metadata(void) {
    if (s_control_url[0] == '\0') {
        // Try discovery if not yet found
        upnp_init();
        return;
    }

    static char resp[4096];

    // GetTransportInfo to check playing state
    const char *get_info_body =
        "<u:GetTransportInfo xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        "<InstanceID>0</InstanceID></u:GetTransportInfo>";

    if (soap_call(s_control_url, "GetTransportInfo", get_info_body, resp, sizeof(resp)) == ESP_OK) {
        bool playing = false;
        parse_transport_state(resp, &playing);
        if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (g_state.playing != playing) { g_state.playing = playing; g_state.dirty = true; }
            xSemaphoreGive(g_state_mutex);
        }
        s_playing_last = playing;
    }

    // GetPositionInfo for track metadata
    const char *get_pos_body =
        "<u:GetPositionInfo xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        "<InstanceID>0</InstanceID></u:GetPositionInfo>";

    if (soap_call(s_control_url, "GetPositionInfo", get_pos_body, resp, sizeof(resp)) == ESP_OK) {
        parse_metadata(resp);
    }
}

void upnp_play_pause(void) {
    if (s_control_url[0] == '\0') return;

    bool currently_playing;
    if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        currently_playing = g_state.playing;
        xSemaphoreGive(g_state_mutex);
    } else {
        return;
    }

    if (currently_playing) {
        const char *pause_body =
            "<u:Pause xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
            "<InstanceID>0</InstanceID></u:Pause>";
        soap_call(s_control_url, "Pause", pause_body, NULL, 0);
        // Also mute via Lyngdorf for non-UPnP source compatibility
        lk_cmd_t mute = { .type = CMD_MUTE_TOGGLE, .param = 0 };
        xQueueSend(g_cmd_queue, &mute, 0);
    } else {
        const char *play_body =
            "<u:Play xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
            "<InstanceID>0</InstanceID><Speed>1</Speed></u:Play>";
        soap_call(s_control_url, "Play", play_body, NULL, 0);
    }

    if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_state.playing = !currently_playing;
        g_state.dirty = true;
        xSemaphoreGive(g_state_mutex);
    }
}

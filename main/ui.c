#include "ui.h"
#include "app_config.h"
#include "wifi_manager.h"

#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const char *TAG = "ui";

// ---------------------------------------------------------------------------
// Widget handles
// ---------------------------------------------------------------------------
static lv_obj_t *s_vol_arc      = NULL;
static lv_obj_t *s_title_lbl    = NULL;
static lv_obj_t *s_artist_lbl   = NULL;
static lv_obj_t *s_album_lbl    = NULL;
static lv_obj_t *s_vol_lbl      = NULL;
static lv_obj_t *s_status_lbl   = NULL;
static lv_obj_t *s_mute_icon    = NULL;
static lv_obj_t *s_play_icon    = NULL;
static lv_obj_t *s_battery_lbl  = NULL;

// Track last state for diff-only updates
static int32_t  s_last_vol    = INT32_MIN;
static bool     s_last_muted  = false;
static bool     s_last_playing = false;
static char     s_last_title[96]  = {0};
static char     s_last_artist[64] = {0};
static char     s_last_album[64]  = {0};
static bool     s_last_amp_conn  = false;
static bool     s_last_wifi_conn = false;
static int8_t   s_last_battery  = -2;       // sentinel: differs from initial -1

// WiFi info for status line (set by wifi_manager during boot)
static char     s_wifi_ssid[32] = {0};
static char     s_wifi_ip[24]   = {0};

// ---------------------------------------------------------------------------
// Colour palette
// ---------------------------------------------------------------------------
#define COL_BG          lv_color_black()
#define COL_ARC_ACTIVE  lv_color_hex(0x00BFBF)   // teal
#define COL_ARC_BG      lv_color_hex(0x1A2A2A)
#define COL_WHITE       lv_color_white()
#define COL_GRAY        lv_color_hex(0x888888)
#define COL_DIM         lv_color_hex(0x444444)
#define COL_MUTE        lv_color_hex(0xFF4444)
#define COL_PLAY        lv_color_hex(0x00BFBF)

// ---------------------------------------------------------------------------
// Volume → arc percentage
// Display range: -80 dB (=-800 db10) to 0 dB (=0 db10)
// ---------------------------------------------------------------------------
#define VOL_MIN_DB10  (-800)
#define VOL_MAX_DB10  (0)

static int vol_to_pct(int32_t vol_db10) {
    if (vol_db10 <= VOL_MIN_DB10) return 0;
    if (vol_db10 >= VOL_MAX_DB10) return 100;
    return (int)((vol_db10 - VOL_MIN_DB10) * 100 / (VOL_MAX_DB10 - VOL_MIN_DB10));
}

// ---------------------------------------------------------------------------
// Update volume label text (called when vol changes)
// ---------------------------------------------------------------------------
static void ui_update_vol_label(int32_t vol_db10) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f dB", vol_db10 / 10.0f);
    lv_label_set_text(s_vol_lbl, buf);
}

// ---------------------------------------------------------------------------
// Status message (connection state etc.)
// ---------------------------------------------------------------------------
void ui_show_status(const char *msg) {
    if (xSemaphoreTake(g_lvgl_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        lv_label_set_text(s_status_lbl, msg);
        xSemaphoreGive(g_lvgl_mutex);
    }
}

void ui_set_wifi_info(const char *ssid, const char *ip) {
    if (ssid) {
        strncpy(s_wifi_ssid, ssid, sizeof(s_wifi_ssid) - 1);
        s_wifi_ssid[sizeof(s_wifi_ssid) - 1] = '\0';
    }
    if (ip) {
        strncpy(s_wifi_ip, ip, sizeof(s_wifi_ip) - 1);
        s_wifi_ip[sizeof(s_wifi_ip) - 1] = '\0';
    }
    // Force the status line to re-render on the next ui_apply_pending_state
    // by inverting one of the "last" trackers.
    s_last_wifi_conn = !s_last_wifi_conn;
    if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_state.dirty = true;
        xSemaphoreGive(g_state_mutex);
    }
}

// ---------------------------------------------------------------------------
// Apply pending state changes (called from UI task, LVGL mutex held)
// ---------------------------------------------------------------------------
void ui_apply_pending_state(void) {
    // Snapshot under state mutex
    lk_state_t snap;
    if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
    if (!g_state.dirty) { xSemaphoreGive(g_state_mutex); return; }
    snap = g_state;
    g_state.dirty = false;
    xSemaphoreGive(g_state_mutex);

    // Now update LVGL (caller holds g_lvgl_mutex)

    // Volume arc + always-visible numeric label at top
    if (snap.vol_db10 != s_last_vol) {
        lv_arc_set_value(s_vol_arc, vol_to_pct(snap.vol_db10));
        ui_update_vol_label(snap.vol_db10);
        s_last_vol = snap.vol_db10;
    }

    // Mute icon — always visible. Swap symbol + colour to indicate state:
    //   unmuted → speaker (teal, same as play icon), muted → speaker-with-X (red)
    if (snap.muted != s_last_muted) {
        if (snap.muted) {
            lv_label_set_text(s_mute_icon, LV_SYMBOL_MUTE);
            lv_obj_set_style_text_color(s_mute_icon, COL_MUTE, 0);
        } else {
            lv_label_set_text(s_mute_icon, LV_SYMBOL_VOLUME_MAX);
            lv_obj_set_style_text_color(s_mute_icon, COL_PLAY, 0);
        }
        // Dim arc when muted
        lv_obj_set_style_arc_color(s_vol_arc,
            snap.muted ? COL_DIM : COL_ARC_ACTIVE, LV_PART_INDICATOR);
        s_last_muted = snap.muted;
    }

    // Play/pause icon
    if (snap.playing != s_last_playing) {
        lv_label_set_text(s_play_icon, snap.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        s_last_playing = snap.playing;
    }

    // Track metadata
    if (strcmp(snap.title,  s_last_title)  != 0 ||
        strcmp(snap.artist, s_last_artist) != 0 ||
        strcmp(snap.album,  s_last_album)  != 0) {

        lv_label_set_text(s_title_lbl,  snap.title[0]  ? snap.title  : "---");
        lv_label_set_text(s_artist_lbl, snap.artist[0] ? snap.artist : "");
        lv_label_set_text(s_album_lbl,  snap.album[0]  ? snap.album  : "");

        strncpy(s_last_title,  snap.title,  sizeof(s_last_title)  - 1);
        strncpy(s_last_artist, snap.artist, sizeof(s_last_artist) - 1);
        strncpy(s_last_album,  snap.album,  sizeof(s_last_album)  - 1);
    }

    // Connection status line — informative during boot:
    //   no WiFi yet:                  "Connecting to <ssid>..."
    //   WiFi up, amp not yet:         "as <ip>"
    //   both up:                      "" (empty, lets track info breathe)
    if (snap.amp_connected != s_last_amp_conn || snap.wifi_connected != s_last_wifi_conn) {
        char buf[80];
        if (!snap.wifi_connected) {
            if (s_wifi_ssid[0]) {
                snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " Connecting to %s...",
                         s_wifi_ssid);
            } else {
                snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " Connecting...");
            }
            lv_label_set_text(s_status_lbl, buf);
        } else if (!snap.amp_connected) {
            if (s_wifi_ip[0]) {
                snprintf(buf, sizeof(buf), "as %s", s_wifi_ip);
                lv_label_set_text(s_status_lbl, buf);
            } else {
                lv_label_set_text(s_status_lbl, "Amp offline");
            }
        } else {
            lv_label_set_text(s_status_lbl, "");
        }
        s_last_amp_conn  = snap.amp_connected;
        s_last_wifi_conn = snap.wifi_connected;
    }

    // Battery percentage + raw mV (-1 = unknown / not yet sampled).
    // The mV is shown for calibration — once the % is reliable across the
    // full charge range we can drop it back to just the percentage.
    if (snap.battery_pct != s_last_battery) {
        if (snap.battery_pct < 0) {
            lv_label_set_text(s_battery_lbl, "");
        } else {
            char buf[24];
            if (snap.battery_mv > 0) {
                snprintf(buf, sizeof(buf), "%d%%  %dmV",
                         (int)snap.battery_pct, (int)snap.battery_mv);
            } else {
                snprintf(buf, sizeof(buf), "%d%%", (int)snap.battery_pct);
            }
            lv_label_set_text(s_battery_lbl, buf);
            // Red when low (≤20 %), grey otherwise
            lv_obj_set_style_text_color(s_battery_lbl,
                snap.battery_pct <= 20 ? COL_MUTE : COL_GRAY, 0);
        }
        s_last_battery = snap.battery_pct;
    }
}

// ---------------------------------------------------------------------------
// Boot splash — black overlay panel with title / context-aware QR code,
// fades out after a short hold. The QR target is decided by WiFi state:
//   AP mode                → http://192.168.4.1/        (config form)
//   STA + DHCP obtained    → http://<device-ip>/        (config form)
//   neither (timeout)      → repo URL                   (docs fallback)
//
// We don't render the QR immediately; instead a poll timer waits up to 5 s
// for WiFi state to resolve, then renders the QR and schedules the fade.
// ---------------------------------------------------------------------------
#define SPLASH_REPO_URL          "https://github.com/svwhisper/lyngdorf-knob"
#define SPLASH_POLL_INTERVAL_MS  250
#define SPLASH_POLL_MAX_COUNT    20      // 20 × 250 ms = 5 s timeout
#define SPLASH_HOLD_AFTER_QR_MS  4000    // visible time once QR is up

typedef struct {
    lv_obj_t *panel;
    lv_obj_t *placeholder;   // "Connecting..." text shown while WiFi is resolving
    int       polls_left;
} splash_ctx_t;

static void splash_set_opa(void *obj, int32_t opa) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)opa, 0);
}

static void splash_done_cb(lv_anim_t *a) {
    lv_obj_del((lv_obj_t *)a->var);   // panel + child labels
}

static void splash_start_fade(lv_timer_t *t) {
    lv_obj_t *panel = (lv_obj_t *)t->user_data;
    lv_timer_del(t);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, panel);
    lv_anim_set_exec_cb(&a, splash_set_opa);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a, 1000);
    lv_anim_set_ready_cb(&a, splash_done_cb);
    lv_anim_start(&a);
}

static void splash_render_qr(lv_obj_t *panel, const char *url) {
    lv_obj_t *qr = lv_qrcode_create(panel, 200, lv_color_black(), lv_color_white());
    lv_qrcode_update(qr, url, strlen(url));
    // White quiet zone (border) around the modules — phones need it to lock on.
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 6, 0);
    lv_obj_align(qr, LV_ALIGN_CENTER, 0, 25);

    // Small caption under the QR so the URL is visible to the human too.
    lv_obj_t *cap = lv_label_create(panel);
    lv_label_set_text(cap, url);
    lv_obj_set_style_text_color(cap, COL_DIM, 0);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(cap, 280);
    lv_label_set_long_mode(cap, LV_LABEL_LONG_DOT);
    lv_obj_align(cap, LV_ALIGN_CENTER, 0, 145);

    // Hold then fade out.
    lv_timer_create(splash_start_fade, SPLASH_HOLD_AFTER_QR_MS, panel);
}

// LV timer: polls WiFi state every 250 ms. Once an IP / AP / timeout
// outcome is known, renders the QR for the right URL and self-destructs.
static void splash_poll_wifi(lv_timer_t *t) {
    splash_ctx_t *ctx = (splash_ctx_t *)t->user_data;
    char ip[24] = {0};
    bool have_ip = wifi_manager_get_ip_str(ip, sizeof(ip));
    bool timeout = (--ctx->polls_left <= 0);

    if (!have_ip && !timeout) return;   // keep polling

    // Remove the "Connecting..." placeholder before drawing the QR.
    if (ctx->placeholder) lv_obj_del(ctx->placeholder);

    char url[80];
    if (have_ip) {
        // Both STA-with-DHCP and AP-mode set s_ip_str in wifi_manager, so this
        // single branch handles both — the user's phone scans straight to the
        // device's config page either way.
        snprintf(url, sizeof(url), "http://%s/", ip);
    } else {
        // No IP after timeout — neither STA nor AP up yet. Fall back to repo
        // so the QR is at least useful for the docs.
        snprintf(url, sizeof(url), SPLASH_REPO_URL);
    }
    splash_render_qr(ctx->panel, url);
    lv_timer_del(t);
    free(ctx);
}

// ---------------------------------------------------------------------------
// Build UI. show_splash=false on wake from deep sleep — we want the live
// UI ready as fast as possible, no QR splash needed.
// ---------------------------------------------------------------------------
esp_err_t ui_init(bool show_splash) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // ---- Volume arc -------------------------------------------------------
    // 270° arc, gap at the bottom. In LVGL 8, set_bg_angles(135, 45) draws
    // clockwise from 135° through 180°, 270°, 0° to 45° = 270° of arc.
    s_vol_arc = lv_arc_create(scr);
    lv_obj_set_size(s_vol_arc, 346, 346);
    lv_obj_center(s_vol_arc);
    lv_arc_set_mode(s_vol_arc, LV_ARC_MODE_NORMAL);
    lv_arc_set_range(s_vol_arc, 0, 100);
    lv_arc_set_value(s_vol_arc, 0);
    lv_arc_set_bg_angles(s_vol_arc, 135, 45);
    lv_obj_set_style_arc_color(s_vol_arc, COL_ARC_ACTIVE, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_vol_arc, COL_ARC_BG,     LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_vol_arc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_vol_arc, 14, LV_PART_MAIN);
    // Hide the draggable knob — we control it programmatically
    lv_obj_set_style_bg_opa(s_vol_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_vol_arc, 0, LV_PART_KNOB);
    lv_obj_clear_flag(s_vol_arc, LV_OBJ_FLAG_CLICKABLE);

    // ---- Artist (top line) — same size/colour as title -------------------
    s_artist_lbl = lv_label_create(scr);
    lv_label_set_text(s_artist_lbl, "");
    lv_obj_set_style_text_color(s_artist_lbl, COL_WHITE, 0);
    lv_obj_set_style_text_font(s_artist_lbl,  &lv_font_montserrat_16, 0);
    lv_obj_set_width(s_artist_lbl, 260);
    lv_label_set_long_mode(s_artist_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(s_artist_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_artist_lbl, LV_ALIGN_CENTER, 0, -45);

    // ---- Title (middle line) ---------------------------------------------
    s_title_lbl = lv_label_create(scr);
    lv_label_set_text(s_title_lbl, "Not Playing");
    lv_obj_set_style_text_color(s_title_lbl, COL_WHITE, 0);
    lv_obj_set_style_text_font(s_title_lbl,  &lv_font_montserrat_16, 0);
    lv_obj_set_width(s_title_lbl, 260);
    lv_label_set_long_mode(s_title_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(s_title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_title_lbl, LV_ALIGN_CENTER, 0, -10);

    // ---- Album (bottom line) — same size/colour as title -----------------
    s_album_lbl = lv_label_create(scr);
    lv_label_set_text(s_album_lbl, "");
    lv_obj_set_style_text_color(s_album_lbl, COL_WHITE, 0);
    lv_obj_set_style_text_font(s_album_lbl,  &lv_font_montserrat_16, 0);
    lv_obj_set_width(s_album_lbl, 260);
    lv_label_set_long_mode(s_album_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(s_album_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_album_lbl, LV_ALIGN_CENTER, 0, 25);

    // ---- Volume label (always visible, top of display) -------------------
    s_vol_lbl = lv_label_create(scr);
    lv_label_set_text(s_vol_lbl, "--.- dB");
    lv_obj_set_style_text_color(s_vol_lbl, COL_WHITE, 0);
    lv_obj_set_style_text_font(s_vol_lbl,  &lv_font_montserrat_20, 0);
    lv_obj_align(s_vol_lbl, LV_ALIGN_CENTER, 0, -110);

    // ---- Mute / Play icons (below album line, 2x larger) ----------------
    // Mute icon starts as a plain speaker (unmuted state) in teal — matches
    // the play icon. Switches to speaker-with-X in red when state.muted goes true.
    s_mute_icon = lv_label_create(scr);
    lv_label_set_text(s_mute_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(s_mute_icon, COL_PLAY, 0);
    lv_obj_set_style_text_font(s_mute_icon,  &lv_font_montserrat_32, 0);
    lv_obj_align(s_mute_icon, LV_ALIGN_CENTER, -45, 90);

    s_play_icon = lv_label_create(scr);
    lv_label_set_text(s_play_icon, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(s_play_icon, COL_PLAY, 0);
    lv_obj_set_style_text_font(s_play_icon,  &lv_font_montserrat_32, 0);
    lv_obj_align(s_play_icon, LV_ALIGN_CENTER, 45, 90);

    // ---- Battery percentage (small, below the icons) --------------------
    s_battery_lbl = lv_label_create(scr);
    lv_label_set_text(s_battery_lbl, "");
    lv_obj_set_style_text_color(s_battery_lbl, COL_GRAY, 0);
    lv_obj_set_style_text_font(s_battery_lbl,  &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(s_battery_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_battery_lbl, LV_ALIGN_CENTER, 0, 125);

    // ---- Status (connection info, very bottom) --------------------------
    s_status_lbl = lv_label_create(scr);
    lv_label_set_text(s_status_lbl, LV_SYMBOL_WIFI " Connecting...");
    lv_obj_set_style_text_color(s_status_lbl, COL_DIM, 0);
    lv_obj_set_style_text_font(s_status_lbl,  &lv_font_montserrat_12, 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_CENTER, 0, 150);

    // ---- Boot splash overlay (created last → on top of everything) ------
    if (!show_splash) {
        ESP_LOGI(TAG, "UI ready (no splash — wake from deep sleep)");
        return ESP_OK;
    }
    lv_obj_t *splash = lv_obj_create(scr);
    lv_obj_remove_style_all(splash);
    lv_obj_set_size(splash, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(splash, COL_BG, 0);
    lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, 0);
    lv_obj_align(splash, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(splash, LV_OBJ_FLAG_SCROLLABLE);

    // Project name at the top of the splash
    lv_obj_t *sp_title = lv_label_create(splash);
    lv_label_set_text(sp_title, "lyngdorf-knob");
    lv_obj_set_style_text_color(sp_title, COL_WHITE, 0);
    lv_obj_set_style_text_font(sp_title,  &lv_font_montserrat_20, 0);
    lv_obj_align(sp_title, LV_ALIGN_CENTER, 0, -110);

    // "Connecting..." placeholder where the QR will appear once we know the
    // WiFi state. Keeps the splash from looking empty during the 1–4 s wait.
    lv_obj_t *placeholder = lv_label_create(splash);
    lv_label_set_text(placeholder, "Connecting...");
    lv_obj_set_style_text_color(placeholder, COL_DIM, 0);
    lv_obj_set_style_text_font(placeholder, &lv_font_montserrat_16, 0);
    lv_obj_align(placeholder, LV_ALIGN_CENTER, 0, 25);

    // Start polling WiFi state. As soon as we have an IP (STA-with-DHCP or
    // AP mode) or 5 s elapse, the poll renders the QR and schedules the fade.
    splash_ctx_t *ctx = malloc(sizeof(*ctx));
    ctx->panel       = splash;
    ctx->placeholder = placeholder;
    ctx->polls_left  = SPLASH_POLL_MAX_COUNT;
    lv_timer_create(splash_poll_wifi, SPLASH_POLL_INTERVAL_MS, ctx);

    ESP_LOGI(TAG, "UI ready");
    return ESP_OK;
}

#include "ui.h"
#include "app_config.h"

#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
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

// Timer to hide vol label after rotation stops
static lv_timer_t *s_vol_hide_timer = NULL;

// Track last state for diff-only updates
static int32_t  s_last_vol    = INT32_MIN;
static bool     s_last_muted  = false;
static bool     s_last_playing = false;
static char     s_last_title[96]  = {0};
static char     s_last_artist[64] = {0};
static char     s_last_album[64]  = {0};
static bool     s_last_amp_conn  = false;
static bool     s_last_wifi_conn = false;

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
// Hide-volume timer callback
// ---------------------------------------------------------------------------
static void vol_hide_cb(lv_timer_t *t) {
    lv_obj_add_flag(s_vol_lbl, LV_OBJ_FLAG_HIDDEN);
    // Show track info again
    lv_obj_clear_flag(s_title_lbl,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_artist_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_album_lbl,  LV_OBJ_FLAG_HIDDEN);
    lv_timer_del(t);
    s_vol_hide_timer = NULL;
}

// ---------------------------------------------------------------------------
// Show volume overlay (called on encoder rotation)
// ---------------------------------------------------------------------------
static void ui_show_vol_overlay(int32_t vol_db10) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f dB", vol_db10 / 10.0f);
    lv_label_set_text(s_vol_lbl, buf);
    lv_obj_clear_flag(s_vol_lbl, LV_OBJ_FLAG_HIDDEN);

    // Temporarily hide track info
    lv_obj_add_flag(s_title_lbl,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_artist_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_album_lbl,  LV_OBJ_FLAG_HIDDEN);

    // Restart/create 2-second hide timer
    if (s_vol_hide_timer) lv_timer_reset(s_vol_hide_timer);
    else s_vol_hide_timer = lv_timer_create(vol_hide_cb, 2000, NULL);
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

    // Volume arc
    if (snap.vol_db10 != s_last_vol) {
        lv_arc_set_value(s_vol_arc, vol_to_pct(snap.vol_db10));
        ui_show_vol_overlay(snap.vol_db10);
        s_last_vol = snap.vol_db10;
    }

    // Mute icon
    if (snap.muted != s_last_muted) {
        if (snap.muted) lv_obj_clear_flag(s_mute_icon, LV_OBJ_FLAG_HIDDEN);
        else            lv_obj_add_flag(s_mute_icon, LV_OBJ_FLAG_HIDDEN);
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

    // Connection status line
    if (snap.amp_connected != s_last_amp_conn || snap.wifi_connected != s_last_wifi_conn) {
        if (!snap.wifi_connected) {
            lv_label_set_text(s_status_lbl, LV_SYMBOL_WIFI " Connecting...");
        } else if (!snap.amp_connected) {
            lv_label_set_text(s_status_lbl, "Amp offline");
        } else {
            lv_label_set_text(s_status_lbl, "");
        }
        s_last_amp_conn  = snap.amp_connected;
        s_last_wifi_conn = snap.wifi_connected;
    }
}

// ---------------------------------------------------------------------------
// Build UI
// ---------------------------------------------------------------------------
esp_err_t ui_init(void) {
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

    // ---- Artist (small, above centre) ------------------------------------
    s_artist_lbl = lv_label_create(scr);
    lv_label_set_text(s_artist_lbl, "");
    lv_obj_set_style_text_color(s_artist_lbl, COL_GRAY, 0);
    lv_obj_set_style_text_font(s_artist_lbl,  &lv_font_montserrat_12, 0);
    lv_obj_set_width(s_artist_lbl, 240);
    lv_label_set_long_mode(s_artist_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_artist_lbl, LV_ALIGN_CENTER, 0, -50);

    // ---- Album (small, just above centre) --------------------------------
    s_album_lbl = lv_label_create(scr);
    lv_label_set_text(s_album_lbl, "");
    lv_obj_set_style_text_color(s_album_lbl, COL_DIM, 0);
    lv_obj_set_style_text_font(s_album_lbl,  &lv_font_montserrat_12, 0);
    lv_obj_set_width(s_album_lbl, 240);
    lv_label_set_long_mode(s_album_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_album_lbl, LV_ALIGN_CENTER, 0, -28);

    // ---- Title (medium, centre) ------------------------------------------
    s_title_lbl = lv_label_create(scr);
    lv_label_set_text(s_title_lbl, "Not Playing");
    lv_obj_set_style_text_color(s_title_lbl, COL_WHITE, 0);
    lv_obj_set_style_text_font(s_title_lbl,  &lv_font_montserrat_16, 0);
    lv_obj_set_width(s_title_lbl, 260);
    lv_label_set_long_mode(s_title_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_title_lbl, LV_ALIGN_CENTER, 0, 0);

    // ---- Volume overlay (shown on rotation, hidden otherwise) ------------
    s_vol_lbl = lv_label_create(scr);
    lv_label_set_text(s_vol_lbl, "");
    lv_obj_set_style_text_color(s_vol_lbl, COL_WHITE, 0);
    lv_obj_set_style_text_font(s_vol_lbl,  &lv_font_montserrat_20, 0);
    lv_obj_align(s_vol_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_vol_lbl, LV_OBJ_FLAG_HIDDEN);

    // ---- Status (connection info, bottom centre) -------------------------
    s_status_lbl = lv_label_create(scr);
    lv_label_set_text(s_status_lbl, LV_SYMBOL_WIFI " Connecting...");
    lv_obj_set_style_text_color(s_status_lbl, COL_DIM, 0);
    lv_obj_set_style_text_font(s_status_lbl,  &lv_font_montserrat_12, 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_CENTER, 0, 60);

    // ---- Mute icon -------------------------------------------------------
    s_mute_icon = lv_label_create(scr);
    lv_label_set_text(s_mute_icon, LV_SYMBOL_MUTE);
    lv_obj_set_style_text_color(s_mute_icon, COL_MUTE, 0);
    lv_obj_set_style_text_font(s_mute_icon,  &lv_font_montserrat_16, 0);
    lv_obj_align(s_mute_icon, LV_ALIGN_CENTER, -30, 36);
    lv_obj_add_flag(s_mute_icon, LV_OBJ_FLAG_HIDDEN);

    // ---- Play/pause icon -------------------------------------------------
    s_play_icon = lv_label_create(scr);
    lv_label_set_text(s_play_icon, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(s_play_icon, COL_PLAY, 0);
    lv_obj_set_style_text_font(s_play_icon,  &lv_font_montserrat_16, 0);
    lv_obj_align(s_play_icon, LV_ALIGN_CENTER, 30, 36);

    ESP_LOGI(TAG, "UI ready");
    return ESP_OK;
}

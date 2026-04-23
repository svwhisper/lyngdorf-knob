#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_err.h"

// ---------------------------------------------------------------------------
// Hardware pin assignments (Waveshare ESP32-S3-Knob-Touch-LCD-1.8)
// ---------------------------------------------------------------------------

// ST77916 QSPI display
#define LCD_CLK_GPIO    13
#define LCD_CS_GPIO     14
#define LCD_D0_GPIO     15
#define LCD_D1_GPIO     16
#define LCD_D2_GPIO     17
#define LCD_D3_GPIO     18
#define LCD_RST_GPIO    21
#define LCD_BL_GPIO     47
#define LCD_H_RES       360
#define LCD_V_RES       360
#define LCD_SPI_HOST    SPI2_HOST
#define LCD_SPI_HZ      (40 * 1000 * 1000)

// CST816D capacitive touch (I2C)
#define TOUCH_SDA_GPIO  11
#define TOUCH_SCL_GPIO  12
#define TOUCH_I2C_PORT  I2C_NUM_0
#define TOUCH_I2C_HZ    300000
#define TOUCH_I2C_ADDR  0x15

// Rotary encoder
#define ENC_A_GPIO      8
#define ENC_B_GPIO      7

// ---------------------------------------------------------------------------
// NVS keys
// ---------------------------------------------------------------------------
#define NVS_NS              "lyngdorf"
#define NVS_WIFI_SSID       "wifi_ssid"
#define NVS_WIFI_PASS       "wifi_pass"
#define NVS_AMP_IP          "amp_ip"
#define NVS_VOL_STEP        "vol_step"
#define NVS_UPNP_URL        "upnp_url"
#define NVS_DIM_SECS        "dim_secs"
#define NVS_SLEEP_SECS      "sleep_secs"

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------
#define AMP_TCP_PORT        84
#define DEFAULT_VOL_STEP    5       // 0.5 dB per encoder detent
#define WIFI_AP_SSID        "LyngdorfKnob"
#define WIFI_AP_PASS        ""      // open AP
#define WIFI_MAX_RETRIES    5
#define DEFAULT_BL_PCT      80      // normal backlight %
#define DEFAULT_DIM_PCT     20      // dimmed backlight %
#define DEFAULT_DIM_SECS    30      // seconds idle before dimming (0 = disabled)
#define DEFAULT_SLEEP_SECS  120     // seconds idle before sleep  (0 = disabled)

// ---------------------------------------------------------------------------
// Shared application state (written by net task, read by UI task)
// ---------------------------------------------------------------------------
typedef struct {
    int32_t  vol_db10;          // current volume in 0.1 dB units (e.g. -300 = -30.0 dB)
    bool     muted;
    bool     playing;
    char     artist[64];
    char     album[64];
    char     title[96];
    uint32_t position_sec;
    uint32_t duration_sec;
    bool     amp_connected;
    bool     upnp_available;
    bool     wifi_connected;
    bool     dirty;             // true when UI needs refresh
} lk_state_t;

extern lk_state_t        g_state;
extern SemaphoreHandle_t g_state_mutex;

// ---------------------------------------------------------------------------
// Command queue: encoder/touch → net task
// ---------------------------------------------------------------------------
typedef enum {
    CMD_VOL_CHANGE,     // param = delta in 0.1 dB units
    CMD_MUTE_TOGGLE,
    CMD_PLAY_PAUSE,
} lk_cmd_type_t;

typedef struct {
    lk_cmd_type_t type;
    int32_t       param;
} lk_cmd_t;

extern QueueHandle_t g_cmd_queue;

// ---------------------------------------------------------------------------
// LVGL mutex (all LVGL calls must be guarded)
// ---------------------------------------------------------------------------
extern SemaphoreHandle_t g_lvgl_mutex;

// ---------------------------------------------------------------------------
// Config API
// ---------------------------------------------------------------------------
esp_err_t app_config_init(void);
esp_err_t config_get_str(const char *key, char *buf, size_t len);
esp_err_t config_set_str(const char *key, const char *val);
esp_err_t config_get_u32(const char *key, uint32_t *out, uint32_t def);
esp_err_t config_set_u32(const char *key, uint32_t val);

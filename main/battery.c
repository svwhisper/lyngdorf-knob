#include "battery.h"
#include "app_config.h"

#include "freertos/FreeRTOS.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "battery";

// Hardware specifics for the Waveshare ESP32-S3-Knob-Touch-LCD-1.8 board:
//   ADC1 channel 0 (GPIO 1), 12-bit, 12 dB attenuation, 2× voltage divider
//   so VBAT = read_mV × 2.
#define BAT_ADC_UNIT          ADC_UNIT_1
#define BAT_ADC_CHAN          ADC_CHANNEL_0
#define BAT_ADC_ATTEN         ADC_ATTEN_DB_12
#define BAT_ADC_BITWIDTH      ADC_BITWIDTH_12
#define BAT_DIVIDER           2

// On battery (no USB), the divider reads cell voltage directly.
// Below ~3.5 V a Li-ion's voltage falls off a cliff; above ~4.0 V it
// flattens out. A linear curve drastically over-reports SoC at the low
// end (e.g. 3.50 V is ~10% real, but a linear 3.0–4.2 V curve says 42%).
// Brownout-on-WiFi at "58%" was the symptom of that.
//
// The table below is piecewise-linear over typical Li-ion discharge data
// (light load, room temp). It's anchored at 4062 mV = 100% — Waveshare's
// on-board TP4056 charger appears to terminate around 4.07 V (a longevity
// profile, lower than the textbook 4.20 V), and a freshly-charged cell on
// this board settles there after a one-hour rest. The bottom of the curve
// is unchanged: brownout-under-WiFi-burst sets in around 3.5 V regardless
// of the top-of-curve voltage.
#define BAT_FULL_MV           4062
#define BAT_EMPTY_MV          3300

// Poll cadence — battery doesn't change fast
#define BAT_POLL_MS           10000

// Number of ADC reads to average per sample (the raw value is noisy)
#define BAT_SAMPLES           8

static adc_oneshot_unit_handle_t s_adc       = NULL;
static adc_cali_handle_t          s_cali      = NULL;
static bool                       s_cali_ok   = false;

// Read current battery voltage in millivolts; returns -1 on failure.
static int read_voltage_mv(void) {
    if (!s_adc) return -1;

    int sum_mv = 0;
    int valid  = 0;
    for (int i = 0; i < BAT_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, BAT_ADC_CHAN, &raw) != ESP_OK) continue;

        int mv;
        if (s_cali_ok) {
            if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) continue;
        } else {
            mv = (raw * 3300) / 4095;   // uncalibrated approximation
        }
        sum_mv += mv;
        valid++;
    }
    if (valid == 0) return -1;
    return (sum_mv / valid) * BAT_DIVIDER;
}

static int8_t voltage_to_pct(int mv) {
    if (mv < 0) return -1;
    // Piecewise-linear Li-ion discharge curve (mV → %).
    // Anchored at 4062 mV = 100% based on a measured one-hour-rest open-
    // circuit reading on this hardware. The top plateau is narrower than
    // a 4.20 V cell — only ~60 mV between 100% and 90% — because the
    // charger stops below the chemistry's textbook ceiling. Below 3.50 V
    // the cell is effectively spent: the chip will brownout under any
    // WiFi TX burst, so we keep the lower-end slope steep.
    static const struct { int mv; int8_t pct; } curve[] = {
        { 4062, 100 },
        { 4000,  90 },
        { 3900,  75 },
        { 3800,  55 },
        { 3700,  35 },
        { 3600,  20 },
        { 3500,  10 },
        { 3400,   5 },
        { 3300,   0 },
    };
    const int n = sizeof(curve) / sizeof(curve[0]);
    if (mv >= curve[0].mv)     return 100;
    if (mv <= curve[n-1].mv)   return 0;
    for (int i = 1; i < n; i++) {
        if (mv >= curve[i].mv) {
            int dv = curve[i-1].mv - curve[i].mv;
            int dp = curve[i-1].pct - curve[i].pct;
            return (int8_t)(curve[i].pct + ((mv - curve[i].mv) * dp) / dv);
        }
    }
    return 0;
}

static void battery_tick(void *arg) {
    int    mv  = read_voltage_mv();
    int8_t pct = voltage_to_pct(mv);

    if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (g_state.battery_pct != pct) {
            g_state.battery_pct = pct;
            g_state.dirty = true;
        }
        if (g_state.battery_mv != (int16_t)mv) {
            g_state.battery_mv = (int16_t)mv;
            g_state.dirty = true;
        }
        xSemaphoreGive(g_state_mutex);
    }
    if (pct >= 0) ESP_LOGI(TAG, "%d mV → %d%%", mv, pct);
}

esp_err_t battery_init(void) {
    adc_oneshot_unit_init_cfg_t init_cfg = { .unit_id = BAT_ADC_UNIT };
    if (adc_oneshot_new_unit(&init_cfg, &s_adc) != ESP_OK) {
        ESP_LOGW(TAG, "adc unit init failed — battery sensing disabled");
        return ESP_OK;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = BAT_ADC_ATTEN,
        .bitwidth = BAT_ADC_BITWIDTH,
    };
    if (adc_oneshot_config_channel(s_adc, BAT_ADC_CHAN, &chan_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "adc channel config failed — battery sensing disabled");
        return ESP_OK;
    }

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = BAT_ADC_UNIT,
        .atten    = BAT_ADC_ATTEN,
        .bitwidth = BAT_ADC_BITWIDTH,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) == ESP_OK) {
        s_cali_ok = true;
    } else {
        ESP_LOGW(TAG, "calibration unavailable — using uncalibrated voltage");
    }

    // Read once immediately so the UI has a value before the first tick.
    battery_tick(NULL);

    const esp_timer_create_args_t targs = {
        .callback = battery_tick,
        .name     = "battery",
    };
    esp_timer_handle_t timer;
    if (esp_timer_create(&targs, &timer) != ESP_OK ||
        esp_timer_start_periodic(timer, (uint64_t)BAT_POLL_MS * 1000) != ESP_OK) {
        ESP_LOGW(TAG, "timer setup failed");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "ADC1.ch%d, %s, polling every %d s",
             BAT_ADC_CHAN, s_cali_ok ? "calibrated" : "uncalibrated",
             BAT_POLL_MS / 1000);
    return ESP_OK;
}

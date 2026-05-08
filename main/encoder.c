#include "encoder.h"
#include "app_config.h"
#include "power.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stdatomic.h>

static const char *TAG = "encoder";

// Accumulated encoder delta (atomic — written from ISR, read from UI task)
static atomic_int s_delta = 0;

// Per-pin debounce timestamps (microseconds). Writes happen only inside the
// ISR; reads also only there, so a plain volatile is sufficient.
static volatile int64_t s_last_a_us = 0;
static volatile int64_t s_last_b_us = 0;
#define ENC_DEBOUNCE_US     5000

// Vol step loaded from NVS
static int32_t s_vol_step = DEFAULT_VOL_STEP;

// ---------------------------------------------------------------------------
// GPIO interrupt — fires when A or B is pulled low (one detent click).
//
// The Waveshare knob is a switch-style encoder, not quadrature: A and B both
// rest HIGH and exactly one of them pulses LOW per click. Direction is just
// "which line went low":
//      ENC_A_GPIO low  → CW   → +1
//      ENC_B_GPIO low  → CCW  → -1
//
// IRAM_ATTR keeps the ISR resident in IRAM so it can run while flash is busy.
// ---------------------------------------------------------------------------
static void IRAM_ATTR encoder_isr(void *arg) {
    int64_t now = esp_timer_get_time();
    int pin = (int)(intptr_t)arg;

    if (pin == ENC_A_GPIO) {
        if (now - s_last_a_us > ENC_DEBOUNCE_US) {
            atomic_fetch_add(&s_delta, 1);
            s_last_a_us = now;
            power_signal_activity();
        }
    } else if (pin == ENC_B_GPIO) {
        if (now - s_last_b_us > ENC_DEBOUNCE_US) {
            atomic_fetch_add(&s_delta, -1);
            s_last_b_us = now;
            power_signal_activity();
        }
    }
}

// ---------------------------------------------------------------------------
// Called from UI task — coalesce accumulated ticks and post command
// ---------------------------------------------------------------------------
void encoder_process_events(void) {
    int delta = atomic_exchange(&s_delta, 0);
    if (delta == 0) return;

    lk_cmd_t cmd = {
        .type  = CMD_VOL_CHANGE,
        .param = delta * s_vol_step,
    };
    xQueueSend(g_cmd_queue, &cmd, 0);
}

// ---------------------------------------------------------------------------
// Init — configure both encoder pins as inputs with pull-ups and
// negative-edge interrupts. We rely on the ISR to do all decoding so the
// CPU can light-sleep between clicks.
// ---------------------------------------------------------------------------
esp_err_t encoder_init(void) {
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << ENC_A_GPIO) | (1ULL << ENC_B_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    // Load vol step from NVS
    uint32_t step;
    config_get_u32(NVS_VOL_STEP, &step, DEFAULT_VOL_STEP);
    s_vol_step = (int32_t)step;

    // Install the global GPIO ISR service (idempotent — returns
    // ESP_ERR_INVALID_STATE if already installed, which is fine).
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(gpio_isr_handler_add(ENC_A_GPIO, encoder_isr,
                                          (void *)(intptr_t)ENC_A_GPIO));
    ESP_ERROR_CHECK(gpio_isr_handler_add(ENC_B_GPIO, encoder_isr,
                                          (void *)(intptr_t)ENC_B_GPIO));

    ESP_LOGI(TAG, "encoder ready (ISR-driven), vol_step=%d (%.1f dB)",
             (int)s_vol_step, s_vol_step / 10.0f);
    return ESP_OK;
}

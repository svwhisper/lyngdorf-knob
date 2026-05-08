#include "encoder.h"
#include "app_config.h"
#include "power.h"
#include "haptic.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stdatomic.h>

static const char *TAG = "encoder";

// Accumulated encoder delta (atomic so timer ISR and UI task can share safely)
static atomic_int s_delta = 0;

// Previous pin state. bit1 = A, bit0 = B. Rest = 0b11.
static uint8_t s_prev = 0b11;

// Vol step loaded from NVS
static int32_t s_vol_step = DEFAULT_VOL_STEP;

// ---------------------------------------------------------------------------
// 3 ms polling timer callback.
//
// The Waveshare knob is NOT a quadrature encoder. Each detent click pulses
// ONE of the two GPIO lines low briefly and back to high:
//
//      rest:                       0b11  (both lines high via internal pull-ups)
//      click "direction A":        0b11 -> 0b01 -> 0b11   (line A pulses low)
//      click "direction B":        0b11 -> 0b10 -> 0b11   (line B pulses low)
//
// Direction is determined entirely by which line went low. We commit one
// logical step on the engage transition (rest -> single-line-low) and ignore
// the release (low-line -> rest). Other transitions are noise and ignored.
// ---------------------------------------------------------------------------
static void encoder_poll_cb(void *arg) {
    uint8_t cur = (gpio_get_level(ENC_A_GPIO) << 1) | gpio_get_level(ENC_B_GPIO);

    if (s_prev == 0b11) {
        // Direction signs chosen so CW rotation increases volume:
        // on this hardware A-low (0b01) = CW = increase, B-low (0b10) = CCW = decrease.
        if (cur == 0b01) {            // A pulsed low → CW → increase
            atomic_fetch_add(&s_delta, 1);
            power_signal_activity();
        } else if (cur == 0b10) {     // B pulsed low → CCW → decrease
            atomic_fetch_add(&s_delta, -1);
            power_signal_activity();
        }
    }
    s_prev = cur;
}

// ---------------------------------------------------------------------------
// Called from UI task — coalesce accumulated ticks and post command
// ---------------------------------------------------------------------------
void encoder_process_events(void) {
    int delta = atomic_exchange(&s_delta, 0);
    if (delta == 0) return;

    haptic_play();  // one tick per coalesced batch — safe here (UI task, I2C allowed)

    lk_cmd_t cmd = {
        .type  = CMD_VOL_CHANGE,
        .param = delta * s_vol_step,   // e.g. 1 detent × 5 = 0.5 dB
    };
    BaseType_t r = xQueueSend(g_cmd_queue, &cmd, 0);
    ESP_LOGI(TAG, "encoder delta=%d → param=%d queue_send=%d",
             delta, (int)cmd.param, (int)r);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
esp_err_t encoder_init(void) {
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << ENC_A_GPIO) | (1ULL << ENC_B_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    // Seed state
    s_prev = ((gpio_get_level(ENC_A_GPIO) << 1) | gpio_get_level(ENC_B_GPIO));

    // Load vol step from NVS
    uint32_t step;
    config_get_u32(NVS_VOL_STEP, &step, DEFAULT_VOL_STEP);
    s_vol_step = (int32_t)step;

    // 3 ms polling timer
    const esp_timer_create_args_t args = {
        .callback = encoder_poll_cb,
        .name     = "enc_poll",
    };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 3000));

    ESP_LOGI(TAG, "encoder ready, vol_step=%d (%.1f dB)", (int)s_vol_step, s_vol_step / 10.0f);
    return ESP_OK;
}

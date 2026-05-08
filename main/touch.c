#include "touch.h"
#include "app_config.h"
#include "power.h"
#include "haptic.h"

#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "touch";

// CST816D register map
#define CST816_REG_GESTURE  0x01
#define CST816_REG_FINGERS  0x02
#define CST816_REG_XH       0x03
#define CST816_REG_XL       0x04
#define CST816_REG_YH       0x05
#define CST816_REG_YL       0x06
#define CST816_REG_IRQ_CTL  0xFA   // 0x70 = pulse INT on touch / change / motion

// Maximum movement during a press for it to count as a tap (vs. a swipe)
#define TAP_MAX_MOVE_PX     40

// Touch hit regions in display coordinates (0..359). These mirror the icon
// positions defined in ui_init: mute icon at LVGL (-45,+90) = display
// (135, 270); play icon at LVGL (+45, +90) = display (225, 270).
// Hit area is a 70 px square around each centre — generous so finger lands
// on the icon even with imprecise tapping.
#define MUTE_HIT_X1   100
#define MUTE_HIT_X2   170
#define MUTE_HIT_Y1   235
#define MUTE_HIT_Y2   305
#define PLAY_HIT_X1   190
#define PLAY_HIT_X2   260
#define PLAY_HIT_Y1   235
#define PLAY_HIT_Y2   305

static bool     s_prev_down = false;
static uint16_t s_press_x = 0, s_press_y = 0;

static inline bool point_in_box(uint16_t x, uint16_t y,
                                 uint16_t x1, uint16_t y1,
                                 uint16_t x2, uint16_t y2) {
    return x >= x1 && x <= x2 && y >= y1 && y <= y2;
}

static esp_err_t cst816_read(uint8_t reg, uint8_t *buf, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TOUCH_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TOUCH_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, buf, len, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(TOUCH_I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return err;
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    uint8_t buf[4] = {0};
    bool i2c_ok = (cst816_read(CST816_REG_XH, buf, 4) == ESP_OK);
    uint8_t fingers = 0;
    if (i2c_ok) cst816_read(CST816_REG_FINGERS, &fingers, 1);

    bool currently_down = (i2c_ok && fingers > 0);
    uint16_t x = ((buf[0] & 0x0F) << 8) | buf[1];
    uint16_t y = ((buf[2] & 0x0F) << 8) | buf[3];

    // Rising edge: finger just touched down. Fire the icon command
    // immediately so the action feels instant — no waiting for lift.
    if (!s_prev_down && currently_down) {
        power_signal_activity();
        s_press_x = x;
        s_press_y = y;

        lk_cmd_t cmd = { .type = 0, .param = 0 };
        bool fire = false;
        if (point_in_box(x, y, MUTE_HIT_X1, MUTE_HIT_Y1,
                                MUTE_HIT_X2, MUTE_HIT_Y2)) {
            cmd.type = CMD_MUTE_TOGGLE;
            fire = true;
        } else if (point_in_box(x, y, PLAY_HIT_X1, PLAY_HIT_Y1,
                                       PLAY_HIT_X2, PLAY_HIT_Y2)) {
            cmd.type = CMD_PLAY_PAUSE;
            fire = true;
        }
        if (fire) {
            haptic_play();          // tactile confirmation of icon hit
            xQueueSend(g_cmd_queue, &cmd, 0);
        }
    }
    // Falling edge: nothing to do; we already fired on press.

    s_prev_down   = currently_down;
    data->point.x = x;
    data->point.y = y;
    data->state   = currently_down ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}

// Pulse the touch controller's reset line (active low) before talking to it
// over I2C. Without this, the chip is in an indeterminate state on cold boot.
static void cst816_hw_reset(void) {
    gpio_config_t rst = {
        .pin_bit_mask = 1ULL << TOUCH_RST_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst);
    gpio_set_level(TOUCH_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(TOUCH_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(50));        // chip needs time to come up
}

// Configure GPIO 9 (TP_INT) as a pulled-up input. Used as a deep-sleep wake
// source — when finger touches the screen the CST816D pulses this line low.
// We don't install an ISR; the line is read passively / used by ext1 wakeup.
static void cst816_int_pin_init(void) {
    gpio_config_t intp = {
        .pin_bit_mask = 1ULL << TOUCH_INT_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&intp);
}

esp_err_t touch_init(void) {
    cst816_hw_reset();

    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = TOUCH_SDA_GPIO,
        .scl_io_num       = TOUCH_SCL_GPIO,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = TOUCH_I2C_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(TOUCH_I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(TOUCH_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));

    // Enable INT pulse on touch / motion / change events. We don't read INT
    // during normal operation (LVGL polls registers anyway) but we DO use it
    // as a deep-sleep wake source.
    uint8_t irq_ctl = 0x70;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TOUCH_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, CST816_REG_IRQ_CTL, true);
    i2c_master_write_byte(cmd, irq_ctl, true);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(TOUCH_I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);

    cst816_int_pin_init();

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&indev_drv);

    ESP_LOGI(TAG, "CST816D ready on I2C addr 0x%02X (INT on GPIO %d)",
             TOUCH_I2C_ADDR, TOUCH_INT_GPIO);
    return ESP_OK;
}

#include "touch.h"
#include "app_config.h"

#include "driver/i2c.h"
#include "esp_log.h"
#include "lvgl.h"
#include <string.h>

static const char *TAG = "touch";

// CST816D register map
#define CST816_REG_GESTURE  0x01
#define CST816_REG_FINGERS  0x02
#define CST816_REG_XH       0x03
#define CST816_REG_XL       0x04
#define CST816_REG_YH       0x05
#define CST816_REG_YL       0x06

// Double-tap detection
#define DTAP_MAX_MS         400
#define DTAP_MAX_MOVE_PX    40

static int64_t s_last_tap_us = 0;
static uint16_t s_last_tap_x = 0;
static uint16_t s_last_tap_y = 0;

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
    if (cst816_read(CST816_REG_XH, buf, 4) != ESP_OK) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    uint8_t fingers = 0;
    cst816_read(CST816_REG_FINGERS, &fingers, 1);

    if (fingers == 0) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    uint16_t x = ((buf[0] & 0x0F) << 8) | buf[1];
    uint16_t y = ((buf[2] & 0x0F) << 8) | buf[3];

    data->point.x = x;
    data->point.y = y;
    data->state   = LV_INDEV_STATE_PR;

    // Double-tap detection → play/pause
    int64_t now_us = esp_timer_get_time();
    int64_t delta_ms = (now_us - s_last_tap_us) / 1000;
    uint16_t dx = (x > s_last_tap_x) ? (x - s_last_tap_x) : (s_last_tap_x - x);
    uint16_t dy = (y > s_last_tap_y) ? (y - s_last_tap_y) : (s_last_tap_y - y);

    if (delta_ms > 0 && delta_ms < DTAP_MAX_MS && dx < DTAP_MAX_MOVE_PX && dy < DTAP_MAX_MOVE_PX) {
        lk_cmd_t cmd = { .type = CMD_PLAY_PAUSE, .param = 0 };
        xQueueSend(g_cmd_queue, &cmd, 0);
        s_last_tap_us = 0;  // reset so triple-tap doesn't re-fire
    } else {
        // Single tap → mute toggle (dispatched on release; approximate here)
        s_last_tap_us = now_us;
        s_last_tap_x  = x;
        s_last_tap_y  = y;
        lk_cmd_t cmd = { .type = CMD_MUTE_TOGGLE, .param = 0 };
        xQueueSend(g_cmd_queue, &cmd, 0);
    }
}

esp_err_t touch_init(void) {
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

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&indev_drv);

    ESP_LOGI(TAG, "CST816D ready on I2C addr 0x%02X", TOUCH_I2C_ADDR);
    return ESP_OK;
}

#include "touch.h"

#include "display.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input.h"
#include "osk.h"
#include "ui_draw.h"

// ES3C28P: FT6336 on I2C0
#define PIN_SDA   16
#define PIN_SCL   15
#define PIN_RESET 18
#define PIN_INT   17
#define FT_ADDR   0x38
#define REG_STATUS 0x02 // number of touch points
#define REG_P1_XH  0x03
#define REG_POWER_MODE 0xA5 // 0 active, 1 monitor (low power, wakes on touch), 3 hibernate

static const char* TAG = "touch";
static i2c_master_dev_handle_t dev;
static i2c_master_bus_handle_t bus;

static esp_err_t read_registers(uint8_t reg, uint8_t* data, size_t len) {
    return i2c_master_transmit_receive(dev, &reg, 1, data, len, 50);
}

void touch_init(void) {
    gpio_config_t reset = {.pin_bit_mask = 1ULL << PIN_RESET, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&reset);
    gpio_set_level(PIN_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(200)); // the controller needs time after reset

    i2c_master_bus_config_t bus_config = {
        .i2c_port = 0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_config, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus failed");
        return;
    }
    i2c_device_config_t device = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = FT_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &device, &dev) != ESP_OK) {
        ESP_LOGE(TAG, "ft6336 not found");
        dev = NULL;
        return;
    }
    ESP_LOGI(TAG, "FT6336 ready");
}

bool touch_read(int* x, int* y) {
    if (dev == NULL) return false;
    uint8_t status;
    if (read_registers(REG_STATUS, &status, 1) != ESP_OK) return false;
    if ((status & 0x0F) == 0) return false;

    uint8_t p[4];
    if (read_registers(REG_P1_XH, p, sizeof(p)) != ESP_OK) return false;
    int raw_x = ((p[0] & 0x0F) << 8) | p[1]; // 0..239
    int raw_y = ((p[2] & 0x0F) << 8) | p[3]; // 0..319

    // Same transform as the device tree: mirror-x (x_max 240), then swap-xy
    int mirrored_x = 240 - raw_x;
    *x = raw_y;          // 0..319 -> screen x
    *y = mirrored_x;     // 0..239 -> screen y

    if (*x < 0) *x = 0;
    if (*x >= DISPLAY_WIDTH) *x = DISPLAY_WIDTH - 1;
    if (*y < 0) *y = 0;
    if (*y >= DISPLAY_HEIGHT) *y = DISPLAY_HEIGHT - 1;
    return true;
}

// Gestures

#define TAP_SLOP   12  // px a finger may move and still count as a tap
#define SWIPE_MIN  30

static volatile bool header_enabled = true;
static volatile bool raw_mode = false; // games read touch_read() directly; pause event generation

void touch_set_header_enabled(bool enabled) {
    header_enabled = enabled;
}

static void push(EventType type, int x, int y) {
    InputEvent event = {.type = type, .x = x, .y = y};
    input_push_event(&event);
}

void touch_set_raw(bool raw) {
    raw_mode = raw;
}

static void gesture_task(void* arg) {
    bool was_down = false;
    int start_x = 0, start_y = 0, last_x = 0, last_y = 0;
    while (true) {
        if (raw_mode) { vTaskDelay(pdMS_TO_TICKS(30)); was_down = false; continue; }
        int x, y;
        bool down = touch_read(&x, &y);
        if (down && !was_down) {
            start_x = last_x = x;
            start_y = last_y = y;
        } else if (down) {
            last_x = x;
            last_y = y;
        } else if (was_down) {
            int dx = last_x - start_x, dy = last_y - start_y;
            int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
            if (adx <= TAP_SLOP && ady <= TAP_SLOP) {
                if (osk_touch(start_x, start_y)) {
                    // typed on the on-screen keyboard
                } else if (start_y < UI_HEADER_HIT && start_x < UI_BACK_W) {
                    push(EV_BACK, start_x, start_y);
                } else if (header_enabled && start_y < UI_HEADER_HIT && start_x >= UI_KEYBOARD_X &&
                           start_x < UI_KEYBOARD_X + UI_KEYBOARD_W) {
                    push(EV_KEYBOARD, start_x, start_y);
                } else {
                    push(EV_TAP, start_x, start_y);
                }
            } else if (adx >= SWIPE_MIN || ady >= SWIPE_MIN) {
                if (adx > ady) push(dx < 0 ? EV_SWIPE_LEFT : EV_SWIPE_RIGHT, start_x, start_y);
                else push(dy < 0 ? EV_SWIPE_UP : EV_SWIPE_DOWN, start_x, start_y);
            }
        }
        was_down = down;
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

void touch_start_events(void) {
    if (dev != NULL) xTaskCreate(gesture_task, "touch", 4096, NULL, 5, NULL);
}

void* board_i2c_bus(void) {
    return bus;
}

void touch_sleep(void) {
    if (dev == NULL) return;
    uint8_t cmd[2] = {REG_POWER_MODE, 0x01};
    i2c_master_transmit(dev, cmd, sizeof(cmd), 50);
}

#include "display.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include <string.h>

// ES3C28P pinout (LCDwiki 2.8" ESP32-S3)
#define PIN_MOSI      11
#define PIN_SCLK      12
#define PIN_CS        10
#define PIN_DC        46
#define PIN_BACKLIGHT 45
#define PIXEL_CLOCK   (40 * 1000 * 1000)
#define MAX_LINES     40

static const char* TAG = "display";

static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t draw_done;
// The terminal task (status bar) and the carousel draw from different tasks
static SemaphoreHandle_t panel_lock;

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t* edata, void* ctx) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(draw_done, &woken);
    return woken == pdTRUE;
}

void display_init(void) {
    draw_done = xSemaphoreCreateBinary();
    panel_lock = xSemaphoreCreateMutex();

    gpio_hold_dis(PIN_BACKLIGHT); // held low while the deck was off
    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << PIN_BACKLIGHT,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl));
    gpio_set_level(PIN_BACKLIGHT, 0);

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_WIDTH * MAX_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = PIN_CS,
        .dc_gpio_num = PIN_DC,
        .spi_mode = 0,
        .pclk_hz = PIXEL_CLOCK,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_trans_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    gpio_set_level(PIN_BACKLIGHT, 1);
    ESP_LOGI(TAG, "ILI9341 ready (%dx%d)", DISPLAY_WIDTH, DISPLAY_HEIGHT);
}

#define STRIP_ROWS 20
static uint16_t* strip_buffer;

// Blits a full-width framebuffer region (src may live in PSRAM) using an internal DMA strip buffer
void display_present(int y0, int h, const uint16_t* src) {
    if (strip_buffer == NULL) {
        strip_buffer = heap_caps_malloc(DISPLAY_WIDTH * STRIP_ROWS * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
    for (int y = 0; y < h; y += STRIP_ROWS) {
        int rows = (h - y) < STRIP_ROWS ? (h - y) : STRIP_ROWS;
        memcpy(strip_buffer, src + y * DISPLAY_WIDTH, rows * DISPLAY_WIDTH * sizeof(uint16_t));
        display_draw(0, y0 + y, DISPLAY_WIDTH, rows, strip_buffer); // only the carousel task uses strip_buffer
    }
}

// Blits a rectangle of a full-screen framebuffer (stride DISPLAY_WIDTH), in strips through the DMA buffer
void display_present_rect(int x, int y, int w, int h, const uint16_t* src) {
    if (strip_buffer == NULL) {
        strip_buffer = heap_caps_malloc(DISPLAY_WIDTH * STRIP_ROWS * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
    int rows_per_strip = (DISPLAY_WIDTH * STRIP_ROWS) / w;
    for (int row = 0; row < h; row += rows_per_strip) {
        int rows = (h - row) < rows_per_strip ? (h - row) : rows_per_strip;
        for (int r = 0; r < rows; r++) {
            memcpy(strip_buffer + r * w, src + (y + row + r) * DISPLAY_WIDTH + x, w * sizeof(uint16_t));
        }
        display_draw(x, y + row, w, rows, strip_buffer);
    }
}

void display_draw(int x, int y, int w, int h, const uint16_t* pixels) {
    xSemaphoreTake(panel_lock, portMAX_DELAY);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, x, y, x + w, y + h, pixels));
    xSemaphoreTake(draw_done, portMAX_DELAY);
    xSemaphoreGive(panel_lock);
}

void display_sleep(void) {
    xSemaphoreTake(panel_lock, portMAX_DELAY);
    gpio_set_level(PIN_BACKLIGHT, 0);
    gpio_hold_en(PIN_BACKLIGHT);
    esp_lcd_panel_disp_on_off(panel, false);
    esp_lcd_panel_disp_sleep(panel, true);
    gpio_deep_sleep_hold_en();
    // The lock stays taken: nothing may draw after this
}

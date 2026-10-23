#include "input.h"
#include "power.h"

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static QueueHandle_t queue;

static TickType_t to_ticks(uint32_t timeout_ms) {
    return timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
}

static void serial_task(void* arg) {
    uint8_t buffer[64];
    while (true) {
        int len = usb_serial_jtag_read_bytes(buffer, sizeof(buffer), portMAX_DELAY);
        if (len > 0) input_push((const char*)buffer, len);
    }
}

void input_init(void) {
    queue = xQueueCreate(256, sizeof(InputEvent));

    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_driver_install(&config) == ESP_OK) {
        usb_serial_jtag_vfs_use_driver();
        xTaskCreate(serial_task, "serial_in", 3072, NULL, 5, NULL);
    }
}

void input_push(const char* data, size_t len) {
    power_activity();
    for (size_t i = 0; i < len; i++) {
        InputEvent event = {.type = EV_KEY, .key = data[i]};
        xQueueSend(queue, &event, pdMS_TO_TICKS(50));
    }
}

void input_push_event(const InputEvent* event) {
    power_activity();
    xQueueSend(queue, event, pdMS_TO_TICKS(50));
}

bool input_wait(InputEvent* out, uint32_t timeout_ms) {
    return xQueueReceive(queue, out, to_ticks(timeout_ms)) == pdTRUE;
}

bool input_read(char* out, uint32_t timeout_ms) {
    int64_t deadline = timeout_ms == UINT32_MAX ? INT64_MAX : esp_timer_get_time() + timeout_ms * 1000LL;
    while (true) {
        int64_t left_us = deadline - esp_timer_get_time();
        if (timeout_ms != UINT32_MAX && left_us < 0) left_us = 0;
        InputEvent event;
        TickType_t ticks = timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(left_us / 1000);
        if (xQueueReceive(queue, &event, ticks) != pdTRUE) return false;
        if (event.type == EV_KEY) {
            *out = event.key;
            return true;
        }
        // Touch events in the middle of a key sequence are dropped
    }
}

void input_flush(void) {
    InputEvent event;
    while (xQueueReceive(queue, &event, 0) == pdTRUE) {}
}

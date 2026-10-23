// USB HID keyboard, used by the Bad USB tool. Installing this takes over the native USB port, so the
// serial console is gone until reboot: while active, the deck is a keyboard to whatever it is plugged into.
#include "badusb_hid.h"

#include <string.h>

#include "class/hid/hid.h"
#include "class/hid/hid_device.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

static const char* TAG = "badusb";
static bool installed;

enum { REPORT_ID_KEYBOARD = 1 };

static const uint8_t report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
#define EPNUM_KEYBOARD 0x81

static const uint8_t config_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_KEYBOARD, sizeof(report_descriptor), EPNUM_KEYBOARD, 8, 10),
};

static const tusb_desc_device_t device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,     // Espressif, not impersonating another vendor
    .idProduct = 0x4010,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const char* string_descriptor[] = {
    (const char[]){0x09, 0x04}, // English
    "CyberDeck",
    "CyberDeck Keyboard",
    "0001",
};

// TinyUSB HID callbacks
uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type, uint8_t* buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)type; (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type, uint8_t const* buffer, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)type; (void)buffer; (void)bufsize;
}

bool badusb_hid_begin(void) {
    if (!installed) {
        tinyusb_config_t config = TINYUSB_CONFIG_FULL_SPEED(NULL, NULL);
        config.descriptor.device = &device_descriptor;
        config.descriptor.string = string_descriptor;
        config.descriptor.string_count = sizeof(string_descriptor) / sizeof(string_descriptor[0]);
        config.descriptor.full_speed_config = config_descriptor;
        if (tinyusb_driver_install(&config) != ESP_OK) {
            ESP_LOGE(TAG, "tinyusb install failed");
            return false;
        }
        installed = true;
    }
    // Wait for the host to enumerate us as a keyboard
    for (int i = 0; i < 300 && !tud_mounted(); i++) vTaskDelay(pdMS_TO_TICKS(10));
    return tud_mounted();
}

bool badusb_hid_ready(void) {
    return installed && tud_mounted();
}

// Sends one key with modifiers, then releases
void badusb_hid_tap(uint8_t modifier, uint8_t keycode) {
    uint8_t keys[6] = {keycode, 0, 0, 0, 0, 0};
    for (int i = 0; i < 100 && !tud_hid_ready(); i++) vTaskDelay(pdMS_TO_TICKS(2));
    tud_hid_keyboard_report(REPORT_ID_KEYBOARD, modifier, keycode ? keys : NULL);
    vTaskDelay(pdMS_TO_TICKS(6));
    for (int i = 0; i < 100 && !tud_hid_ready(); i++) vTaskDelay(pdMS_TO_TICKS(2));
    tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, NULL); // release
    vTaskDelay(pdMS_TO_TICKS(6));
}

// Types a printable ASCII character using TinyUSB's US-layout table
void badusb_hid_char(char c) {
    // conv_table maps ascii -> {shift, keycode}
    static const uint8_t conv_table[128][2] = {HID_ASCII_TO_KEYCODE};
    if ((unsigned char)c >= 128 || conv_table[(int)c][1] == 0) return;
    uint8_t modifier = conv_table[(int)c][0] ? KEYBOARD_MODIFIER_LEFTSHIFT : 0;
    badusb_hid_tap(modifier, conv_table[(int)c][1]);
}

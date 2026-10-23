#include "keyboard.h"

#include <string.h>

#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_hid_gap.h"
#include "esp_hidh.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "input.h"
#include "term.h"

#define SCAN_SECONDS       5
#define OPEN_TIMEOUT_MS    20000
#define REPEAT_DELAY_MS    400
#define REPEAT_INTERVAL_MS 40

#define BIT_OPENED  BIT0
#define BIT_CLOSED  BIT1

#define MOD_CTRL  0x11
#define MOD_SHIFT 0x22
#define MOD_ALT   0x44

static const char* TAG = "keyboard";

static EventGroupHandle_t events;
// The keyboard search and "blescan" share the radio's single scanner
static SemaphoreHandle_t scan_lock;
static KeyboardAdvCallback adv_callback;
static volatile bool connected;
static char name[32];

// Key repeat state, owned by the HID event task and read by the repeat task
static volatile uint8_t held_key;
static volatile uint8_t held_mods;
static volatile TickType_t held_since;
static uint8_t previous_keys[6];
static bool caps_lock;

// US layout, HID usages 0x04-0x38
static const char unshifted[] = "abcdefghijklmnopqrstuvwxyz1234567890\r\x1b\x7f\t -=[]\\#;'`,./";
static const char shifted[]   = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\r\x1b\x7f\t _+{}|~:\"~<>?";

static void emit_key(uint8_t key, uint8_t mods) {
    const char* sequence = NULL;
    switch (key) {
        case 0x3A: sequence = "\x1bOP"; break;   // F1
        case 0x3B: sequence = "\x1bOQ"; break;   // F2
        case 0x3C: sequence = "\x1bOR"; break;   // F3
        case 0x3D: sequence = "\x1bOS"; break;   // F4
        case 0x49: sequence = "\x1b[2~"; break;  // Insert
        case 0x4A: sequence = "\x1b[H"; break;   // Home
        case 0x4B: sequence = "\x1b[5~"; break;  // Page up
        case 0x4C: sequence = "\x1b[3~"; break;  // Delete
        case 0x4D: sequence = "\x1b[F"; break;   // End
        case 0x4E: sequence = "\x1b[6~"; break;  // Page down
        case 0x4F: sequence = "\x1b[C"; break;   // Right
        case 0x50: sequence = "\x1b[D"; break;   // Left
        case 0x51: sequence = "\x1b[B"; break;   // Down
        case 0x52: sequence = "\x1b[A"; break;   // Up
        default: break;
    }
    // Shift+Up/Down scroll the terminal: xterm's modified arrow keys
    if ((mods & MOD_SHIFT) && (key == 0x51 || key == 0x52)) {
        sequence = key == 0x52 ? "\x1b[1;2A" : "\x1b[1;2B";
    }
    if (sequence != NULL) {
        input_push(sequence, strlen(sequence));
        return;
    }
    if (key < 0x04 || key > 0x38) {
        return;
    }

    bool shift = (mods & MOD_SHIFT) != 0;
    bool is_letter = key <= 0x1D;
    if (is_letter && caps_lock) {
        shift = !shift;
    }
    char c = shift ? shifted[key - 0x04] : unshifted[key - 0x04];

    if (mods & MOD_CTRL) {
        if (c >= 'a' && c <= 'z') c = c - 'a' + 1;
        else if (c >= 'A' && c <= 'Z') c = c - 'A' + 1;
        else if (c == '[') c = 0x1b;
        else if (c == '\\') c = 0x1c;
        else if (c == ']') c = 0x1d;
        else if (c == ' ' || c == '2' || c == '@') c = 0;
    }

    char out[2];
    int len = 0;
    if (mods & MOD_ALT) out[len++] = 0x1b;
    out[len++] = c;
    input_push(out, len);
}

static bool contains(const uint8_t* keys, uint8_t key) {
    for (int i = 0; i < 6; i++) {
        if (keys[i] == key) return true;
    }
    return false;
}

static void handle_report(const uint8_t* data, size_t len) {
    uint8_t mods;
    const uint8_t* keys;
    if (len == 8) {        // modifiers, reserved, 6 keys (boot protocol layout)
        mods = data[0];
        keys = &data[2];
    } else if (len == 7) { // modifiers, 6 keys
        mods = data[0];
        keys = &data[1];
    } else {
        ESP_LOG_BUFFER_HEX(TAG, data, len);
        return;
    }

    uint8_t newest = 0;
    for (int i = 0; i < 6; i++) {
        uint8_t key = keys[i];
        if (key <= 0x03 || contains(previous_keys, key)) continue;
        if (key == 0x39) {
            caps_lock = !caps_lock;
            continue;
        }
        emit_key(key, mods);
        newest = key;
    }

    if (newest != 0) {
        held_key = newest;
        held_since = xTaskGetTickCount();
    } else if (held_key != 0 && !contains(keys, held_key)) {
        held_key = 0;
    }
    held_mods = mods;
    memcpy(previous_keys, keys, 6);
}

static void repeat_task(void* arg) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(REPEAT_INTERVAL_MS));
        uint8_t key = held_key;
        if (key != 0 && xTaskGetTickCount() - held_since >= pdMS_TO_TICKS(REPEAT_DELAY_MS)) {
            emit_key(key, held_mods);
        }
    }
}

static void hidh_callback(void* handler_args, esp_event_base_t base, int32_t id, void* event_data) {
    esp_hidh_event_data_t* param = (esp_hidh_event_data_t*)event_data;
    switch ((esp_hidh_event_t)id) {
        case ESP_HIDH_OPEN_EVENT:
            if (param->open.status == ESP_OK) {
                const char* dev_name = esp_hidh_dev_name_get(param->open.dev);
                strlcpy(name, dev_name ? dev_name : "teclado", sizeof(name));
                connected = true;
                xEventGroupSetBits(events, BIT_OPENED);
            } else {
                xEventGroupSetBits(events, BIT_CLOSED);
            }
            break;
        case ESP_HIDH_INPUT_EVENT:
            if (param->input.usage == ESP_HID_USAGE_KEYBOARD) {
                handle_report(param->input.data, param->input.length);
            }
            break;
        case ESP_HIDH_CLOSE_EVENT:
            connected = false;
            held_key = 0;
            memset(previous_keys, 0, sizeof(previous_keys));
            xEventGroupSetBits(events, BIT_CLOSED);
            break;
        default:
            break;
    }
}

void esp_hid_gap_on_passkey(uint32_t passkey) {
    term_printf("\r\n\x1b[1;33m[bt] digite no teclado: %06lu e Enter\x1b[0m\r\n", (unsigned long)passkey);
}

void esp_hid_gap_on_auth(bool success, int reason) {
    if (!success) {
        term_printf("\x1b[31m[bt] pareamento falhou (0x%x)\x1b[0m\r\n", reason);
    }
}

void esp_hid_gap_on_adv(const uint8_t* bda, int rssi, const char* adv_name, uint16_t appearance, uint16_t uuid) {
    KeyboardAdvCallback callback = adv_callback;
    if (callback != NULL) callback(bda, rssi, adv_name, appearance);
}

void keyboard_ble_scan(int seconds, KeyboardAdvCallback callback) {
    xSemaphoreTake(scan_lock, portMAX_DELAY);
    adv_callback = callback;
    size_t count = 0;
    esp_hid_scan_result_t* results = NULL;
    esp_hid_scan(seconds, &count, &results);
    adv_callback = NULL;
    if (results != NULL) esp_hid_scan_results_free(results);
    xSemaphoreGive(scan_lock);
}

static esp_hid_scan_result_t* pick_keyboard(esp_hid_scan_result_t* results) {
    esp_hid_scan_result_t* fallback = NULL;
    for (esp_hid_scan_result_t* r = results; r != NULL; r = r->next) {
        if (r->transport != ESP_HID_TRANSPORT_BLE) continue;
        if (r->usage == ESP_HID_USAGE_KEYBOARD) return r;
        if (fallback == NULL || r->rssi > fallback->rssi) fallback = r;
    }
    return fallback;
}

static void connect_task(void* arg) {
    // The status bar shows the connection state: only pairing problems are printed
    while (true) {
        if (connected) {
            xEventGroupWaitBits(events, BIT_CLOSED, pdTRUE, pdFALSE, portMAX_DELAY);
            continue;
        }

        size_t count = 0;
        esp_hid_scan_result_t* results = NULL;
        xSemaphoreTake(scan_lock, portMAX_DELAY);
        esp_hid_scan(SCAN_SECONDS, &count, &results);
        xSemaphoreGive(scan_lock);
        esp_hid_scan_result_t* target = pick_keyboard(results);
        if (target != NULL) {
            xEventGroupClearBits(events, BIT_OPENED | BIT_CLOSED);
            esp_hidh_dev_open(target->bda, target->transport, target->ble.addr_type);
            EventBits_t bits = xEventGroupWaitBits(events, BIT_OPENED | BIT_CLOSED, pdTRUE, pdFALSE, pdMS_TO_TICKS(OPEN_TIMEOUT_MS));
            if (!(bits & BIT_OPENED)) {
                ESP_LOGW(TAG, "%s did not connect", target->name ? target->name : "keyboard");
            }
        }
        if (results != NULL) {
            esp_hid_scan_results_free(results);
        }
    }
}

void keyboard_init(void) {
    events = xEventGroupCreate();
    scan_lock = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(esp_hid_gap_init(HIDH_BLE_MODE));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler));

    // "Just Works" pairing: no code to type. If a keyboard insists on a passkey anyway,
    // esp_hid_gap_on_passkey() still shows it.
    esp_ble_auth_req_t auth = ESP_LE_AUTH_REQ_SC_BOND;
    esp_ble_io_cap_t io_cap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t key_mask = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, sizeof(auth));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io_cap, sizeof(io_cap));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &key_mask, sizeof(key_mask));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &key_mask, sizeof(key_mask));

    esp_hidh_config_t config = {
        .callback = hidh_callback,
        .event_stack_size = 4096,
    };
    ESP_ERROR_CHECK(esp_hidh_init(&config));

    xTaskCreate(connect_task, "kbd_connect", 6144, NULL, 3, NULL);
    xTaskCreate(repeat_task, "kbd_repeat", 2048, NULL, 5, NULL);
}

bool keyboard_is_connected(void) {
    return connected;
}

const char* keyboard_name(void) {
    return connected ? name : NULL;
}

void keyboard_forget(void) {
    int count = esp_ble_get_bond_device_num();
    if (count <= 0) return;
    esp_ble_bond_dev_t list[count];
    if (esp_ble_get_bond_device_list(&count, list) == ESP_OK) {
        for (int i = 0; i < count; i++) {
            esp_ble_remove_bond_device(list[i].bd_addr);
        }
    }
}

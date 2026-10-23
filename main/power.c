#include "power.h"

#include "display.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "osk.h"
#include "sound.h"
#include "term.h"
#include "touch.h"

#define PIN_BOOT_BUTTON 0
#define PIN_TOUCH_INT   17
#define DEFAULT_AUTOOFF 5   // minutes

static const char* TAG = "power";
static volatile int64_t last_activity_us;
static int autooff_minutes = DEFAULT_AUTOOFF;

void power_activity(void) {
    last_activity_us = esp_timer_get_time();
}

bool power_woke_from_sleep(void) {
    return esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_EXT1);
}

void power_off(void) {
    // Goodbye: message, falling chime
    osk_hide();
    term_clear();
    term_puts("\r\n\r\n\x1b[1;35m        desligando...\x1b[0m\r\n\r\n");
    term_puts("\x1b[90m  toque na tela ou aperte BOOT\r\n         para ligar\x1b[0m");
    sound_play(SOUND_OFF);
    vTaskDelay(pdMS_TO_TICKS(1200));

    // A finger still on the glass would wake the deck right away
    int x, y;
    while (touch_read(&x, &y)) vTaskDelay(pdMS_TO_TICKS(20));
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_wifi_stop();
    sound_shutdown();
    touch_sleep();     // low power mode that still reports a touch on INT
    display_sleep();

    uint64_t wake_pins = 1ULL << PIN_BOOT_BUTTON;
    rtc_gpio_pullup_en(PIN_BOOT_BUTTON);
    rtc_gpio_pulldown_dis(PIN_BOOT_BUTTON);
    // Only wake on touch if its interrupt line is idle (high); a stuck-low line would wake us immediately
    if (gpio_get_level(PIN_TOUCH_INT) == 1) {
        wake_pins |= 1ULL << PIN_TOUCH_INT;
        rtc_gpio_pullup_en(PIN_TOUCH_INT);
        rtc_gpio_pulldown_dis(PIN_TOUCH_INT);
    } else {
        ESP_LOGW(TAG, "touch INT is low, waking only with BOOT");
    }
    esp_sleep_enable_ext1_wakeup_io(wake_pins, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
}

static void autooff_task(void* arg) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (autooff_minutes <= 0) continue;
        if (esp_timer_get_time() - last_activity_us > autooff_minutes * 60LL * 1000000LL) power_off();
    }
}

void power_set_autooff(int minutes) {
    autooff_minutes = minutes < 0 ? 0 : (minutes > 240 ? 240 : minutes);
    power_activity();
    nvs_handle_t nvs;
    if (nvs_open("deck", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "autooff", autooff_minutes);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

int power_autooff(void) {
    return autooff_minutes;
}

void power_init(void) {
    // Pins held during the last sleep (amplifier, backlight) must be released to be driven again
    gpio_deep_sleep_hold_dis();
    nvs_handle_t nvs;
    uint8_t saved;
    if (nvs_open("deck", NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_u8(nvs, "autooff", &saved) == ESP_OK) autooff_minutes = saved;
        nvs_close(nvs);
    }
    power_activity();
    xTaskCreate(autooff_task, "autooff", 3072, NULL, 2, NULL);
}

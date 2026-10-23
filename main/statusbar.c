#include "statusbar.h"

#include <stdlib.h>
#include <time.h>

#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_event.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sound.h"
#include "term.h"

// BAT+ goes through a 200K/200K divider into GPIO9 (ADC1 channel 8). The charger chip has no status pin
// wired to the ESP, so charging is inferred: a computer on the USB port, or the battery voltage jumping up
// when a charger is plugged in (and dropping when it is unplugged).
#define BATTERY_CHANNEL   ADC_CHANNEL_8
#define BATTERY_EMPTY_MV  3300
#define BATTERY_FULL_MV   4150
#define CHARGE_OFFSET_MV  100   // charging current lifts the reading; subtracted for the percentage
#define FULL_WHILE_CHARGING_MV 4180
#define JUMP_MV           60    // step that means a charger was plugged in or out
#define ABOVE_BATTERY_MV  4250  // a lone Li-ion cell never reads this high: external power
#define SAMPLE_MS         2000
#define HISTORY           8     // samples kept for the jump detection (16 s)

static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t calibration;
static volatile int battery_mv = -1;     // smoothed
static volatile bool charging;
static int history[HISTORY];
static int history_count;

static int read_battery_mv(void) {
    if (adc == NULL || calibration == NULL) return -1;
    int total = 0;
    for (int i = 0; i < 16; i++) {
        int raw = 0, mv = 0;
        if (adc_oneshot_read(adc, BATTERY_CHANNEL, &raw) != ESP_OK) return -1;
        if (adc_cali_raw_to_voltage(calibration, raw, &mv) != ESP_OK) return -1;
        total += mv;
    }
    return total / 16 * 2;
}

static void update_charging(int mv) {
    bool was = charging;
    bool now = was;

    // Compare with the reading from 6-10 s ago: a step means the charger changed
    if (history_count >= 5) {
        int before = (history[(history_count - 4) % HISTORY] + history[(history_count - 5) % HISTORY]) / 2;
        if (mv - before >= JUMP_MV) now = true;
        else if (before - mv >= JUMP_MV) now = false;
    }
    if (mv >= ABOVE_BATTERY_MV) now = true;
    if (usb_serial_jtag_is_connected()) now = true;

    history[history_count % HISTORY] = mv;
    history_count++;

    if (now != was) {
        charging = now;
        sound_play(now ? SOUND_PLUG : SOUND_UNPLUG);
        term_refresh_status();
    }
}

static void battery_task(void* arg) {
    while (true) {
        int mv = read_battery_mv();
        if (mv > 0) {
            update_charging(mv);
            battery_mv = battery_mv < 0 ? mv : (battery_mv * 7 + mv * 3) / 10;
        }
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
    }
}

int statusbar_battery_mv(void) {
    return battery_mv;
}

int statusbar_battery_percent(void) {
    int mv = battery_mv;
    if (mv <= 0) return -1;
    if (charging) mv -= CHARGE_OFFSET_MV;
    int percent = (mv - BATTERY_EMPTY_MV) * 100 / (BATTERY_FULL_MV - BATTERY_EMPTY_MV);
    return percent < 0 ? 0 : (percent > 100 ? 100 : percent);
}

bool statusbar_charging(void) {
    return charging;
}

bool statusbar_battery_full(void) {
    return charging && battery_mv >= FULL_WHILE_CHARGING_MV;
}

static void init_battery(void) {
    adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    if (adc_oneshot_new_unit(&unit, &adc) != ESP_OK) {
        adc = NULL;
        return;
    }
    adc_oneshot_chan_cfg_t channel = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    adc_oneshot_config_channel(adc, BATTERY_CHANNEL, &channel);
    adc_cali_curve_fitting_config_t cali = {
        .unit_id = ADC_UNIT_1,
        .chan = BATTERY_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali, &calibration) != ESP_OK) calibration = NULL;

    // Initial state without a sound: a computer, or a reading only external power gives
    int mv = read_battery_mv();
    battery_mv = mv;
    charging = usb_serial_jtag_is_connected() || mv >= ABOVE_BATTERY_MV;
    xTaskCreate(battery_task, "battery", 3072, NULL, 2, NULL);
}

// Asking for the time before Wi-Fi is up fails, and SNTP then backs off for 15 s, 30 s... The first request
// right after connecting is often lost too. So once an IP address arrives, ask again every 3 s until the
// clock is set (and after every reconnection).
static volatile bool clock_task_running;

static bool clock_is_set(void) {
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    return local.tm_year + 1900 >= 2025;
}

static void clock_task(void* arg) {
    for (int i = 0; i < 20 && !clock_is_set(); i++) {
        esp_sntp_restart();
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    clock_task_running = false;
    vTaskDelete(NULL);
}

static void on_got_ip(void* arg, esp_event_base_t base, int32_t id, void* data) {
    static bool started;
    if (!started) started = esp_netif_sntp_start() == ESP_OK;
    if (!clock_task_running) {
        clock_task_running = true;
        xTaskCreate(clock_task, "clock", 3072, NULL, 2, NULL);
    }
}

static void init_clock(void) {
    setenv("TZ", "<-03>3", 1); // Brasilia, no daylight saving
    tzset();
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.start = false;
    esp_netif_sntp_init(&config);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL);
}

void statusbar_init(void) {
    init_battery();
    init_clock();
}

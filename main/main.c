#include <string.h>

#include "display.h"
#include "esp_event.h"
#include "esp_log.h"
#include "input.h"
#include "keyboard.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "shell.h"
#include "power.h"
#include "sound.h"
#include "statusbar.h"
#include "storage.h"
#include "touch.h"
#include "badusb.h"
#include "menu.h"
#include "tools.h"
#include "term.h"
#include "wifi.h"

static void load_font(void) {
    nvs_handle_t nvs;
    char name[8];
    size_t size = sizeof(name);
    if (nvs_open("deck", NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_str(nvs, "font", name, &size) == ESP_OK) {
            term_set_font(name);
        }
        nvs_close(nvs);
    }
}

// A box as wide as the terminal, so it fits every font size
static void print_banner(void) {
    const int width = term_cols();
    const char* lines[] = {"C Y B E R D E C K", "esp32-s3 // v0.3"};
    const char* colors[] = {"\x1b[1;97m", "\x1b[0;90m"};

    term_puts("\x1b[36m\u250c");
    for (int i = 0; i < width - 2; i++) term_puts("\u2500");
    term_puts("\u2510");
    for (int i = 0; i < 2; i++) {
        int pad = width - 2 - (int)strlen(lines[i]);
        if (pad < 0) pad = 0;
        term_printf("\u2502%s%*s%s%*s\x1b[0;36m\u2502", colors[i], pad / 2, "", lines[i], pad - pad / 2, "");
    }
    term_puts("\u2514");
    for (int i = 0; i < width - 2; i++) term_puts("\u2500");
    term_puts("\u2518\x1b[0m\r\n");
}

static void init_nvs(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

void app_main(void) {
    // Logs would interleave with the shell on the serial console
    esp_log_level_set("*", ESP_LOG_WARN);

    init_nvs();
    power_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    display_init();
    touch_init();
    sound_init();     // after touch_init: shares its I2C bus
    term_init();
    load_font();
    print_banner();

    input_init();
    touch_start_events();
    wifi_init();
    statusbar_init();
    keyboard_init();
    storage_init();
    badusb_init();

    tools_sys_register();
    tools_net_register();
    tools_fs_register();
    tools_games_register();

    sound_play(SOUND_BOOT);

    // Start in the menu; "Terminal" (or Esc) drops to the shell, "menu" reopens it
    menu_open();

    term_puts("\x1b[90mtoque em\x1b[0m \u25c0 MENU \x1b[90mou digite\x1b[0m menu\r\n\r\n");
    shell_run();
}

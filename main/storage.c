#include "storage.h"

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

// ES3C28P microSD slot, 4-bit SDMMC
#define PIN_CLK 38
#define PIN_CMD 40
#define PIN_D0  39
#define PIN_D1  41
#define PIN_D2  48
#define PIN_D3  47

static const char* TAG = "storage";
static sdmmc_card_t* card;

bool storage_init(void) {
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = PIN_CLK;
    slot.cmd = PIN_CMD;
    slot.d0 = PIN_D0;
    slot.d1 = PIN_D1;
    slot.d2 = PIN_D2;
    slot.d3 = PIN_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mount, &card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No SD card (%s)", esp_err_to_name(ret));
        card = NULL;
        return false;
    }
    return true;
}

bool storage_is_mounted(void) {
    return card != NULL;
}

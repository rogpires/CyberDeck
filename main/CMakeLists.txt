idf_component_register(
    SRCS "main.c" "display.c" "term.c" "input.c" "keyboard.c" "esp_hid_gap.c" "shell.c" "wifi.c" "statusbar.c"
         "storage.c" "tools_sys.c" "tools_fs.c" "tools_net.c" "menu.c" "carousel.c" "touch.c" "ui_draw.c" "osk.c" "sound.c" "power.c" "badusb.c" "badusb_hid.c" "settings.c" "wifitools.c" "game_invaders.c" "tools_games.c"
    INCLUDE_DIRS "."
    REQUIRES esp_hid esp_lcd esp_driver_spi esp_driver_gpio esp_driver_usb_serial_jtag
             esp_adc esp_driver_ledc esp_wifi esp_netif esp_http_client mbedtls fatfs sdmmc esp_driver_sdmmc esp_driver_i2c esp_driver_i2s esp_codec_dev esp_tinyusb lwip spi_flash esp_timer esp_event nvs_flash bt
)

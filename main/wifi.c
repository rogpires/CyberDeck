#include "wifi.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "shell.h"
#include "sound.h"
#include "term.h"

#define MAX_SCAN_RESULTS 20

static esp_netif_t* netif;
static volatile bool connected;
static volatile bool wanted;

static void on_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        wifi_config_t config;
        if (esp_wifi_get_config(WIFI_IF_STA, &config) == ESP_OK && config.sta.ssid[0] != 0) {
            wanted = true;
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // The status bar shows the state: printing here would break the line being typed
        connected = false;
        if (wanted) {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        connected = true;
    }
}

void wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, "cyberdeck");

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    // Credentials are persisted by the Wi-Fi driver itself (WIFI_STORAGE_FLASH is the default)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void wifi_connect(const char* ssid, const char* password) {
    wifi_config_t config = {0};
    strlcpy((char*)config.sta.ssid, ssid, sizeof(config.sta.ssid));
    strlcpy((char*)config.sta.password, password ? password : "", sizeof(config.sta.password));
    config.sta.threshold.authmode = (password && password[0]) ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;

    wanted = false;
    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    wanted = true;
    term_printf("[wifi] conectando em %s...\r\n", ssid);
    esp_wifi_connect();
}

void wifi_disconnect(void) {
    wanted = false;
    esp_wifi_disconnect();
}

void wifi_scan(void) {
    term_puts("[wifi] procurando redes...\r\n");
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) {
        term_puts("\x1b[31m[wifi] falha no scan\x1b[0m\r\n");
        return;
    }
    uint16_t count = MAX_SCAN_RESULTS;
    wifi_ap_record_t records[MAX_SCAN_RESULTS];
    esp_wifi_scan_get_ap_records(&count, records);
    shell_pager_start();
    term_puts("\x1b[90m dBm ch seg  rede\x1b[0m\r\n");
    shell_pager_line();
    for (int i = 0; i < count; i++) {
        const char* security;
        switch (records[i].authmode) {
            case WIFI_AUTH_OPEN: security = "\x1b[31mopen\x1b[0m"; break;
            case WIFI_AUTH_WEP: security = "\x1b[31mwep \x1b[0m"; break;
            case WIFI_AUTH_WPA_PSK: security = "wpa "; break;
            case WIFI_AUTH_WPA3_PSK: security = "\x1b[32mwpa3\x1b[0m"; break;
            case WIFI_AUTH_WPA2_WPA3_PSK: security = "\x1b[32mwpa3\x1b[0m"; break;
            default: security = "wpa2"; break;
        }
        // Signal strength as a color: green strong, yellow medium, red weak
        int rssi = records[i].rssi;
        const char* color = rssi > -60 ? "\x1b[32m" : (rssi > -75 ? "\x1b[33m" : "\x1b[31m");
        term_printf("%s%4d\x1b[0m %2d %s %.*s\r\n", color, rssi, records[i].primary, security,
                    term_cols() - 13, records[i].ssid[0] ? (const char*)records[i].ssid : "\x1b[90m(oculta)\x1b[0m");
        if (!shell_pager_line()) break;
    }
    if (count == 0) {
        term_puts(" nenhuma rede\r\n");
    }
    sound_play(count ? SOUND_READ : SOUND_ERROR);
}

void wifi_print_status(void) {
    wifi_config_t config;
    esp_wifi_get_config(WIFI_IF_STA, &config);
    if (connected) {
        esp_netif_ip_info_t ip;
        esp_netif_get_ip_info(netif, &ip);
        wifi_ap_record_t ap;
        esp_wifi_sta_get_ap_info(&ap);
        term_printf("rede: %s\r\nip:   " IPSTR "\r\ngw:   " IPSTR "\r\nsinal: %d dBm\r\n",
                    (const char*)config.sta.ssid, IP2STR(&ip.ip), IP2STR(&ip.gw), ap.rssi);
    } else if (config.sta.ssid[0] != 0) {
        term_printf("desconectado (rede salva: %s)\r\n", (const char*)config.sta.ssid);
    } else {
        term_puts("nenhuma rede salva. use: wifi connect <rede> <senha>\r\n");
    }
}

bool wifi_is_connected(void) {
    return connected;
}

esp_netif_t* wifi_netif(void) {
    return netif;
}

int wifi_rssi(void) {
    wifi_ap_record_t ap;
    if (!connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

// Stops Wi-Fi activity for monitor mode: the auto-reconnect handler must not fight the channel hopping
void wifi_suspend(void) {
    wanted = false;
    esp_wifi_disconnect();
}

// Returns from monitor mode: reconnect to the saved network if there is one
void wifi_reconnect(void) {
    wifi_config_t config;
    if (esp_wifi_get_config(WIFI_IF_STA, &config) == ESP_OK && config.sta.ssid[0] != 0) {
        wanted = true;
        esp_wifi_connect();
    }
}

#pragma once

#include <stdbool.h>

#include "esp_netif.h"
#include "esp_wifi.h"

// Connects to the saved network (if any) and keeps reconnecting
void wifi_init(void);
void wifi_connect(const char* ssid, const char* password);
void wifi_disconnect(void);
// Prints the networks in range to the terminal
void wifi_scan(void);
void wifi_print_status(void);
bool wifi_is_connected(void);
// Scans; fills out[] with up to max networks, returns the count
int wifi_scan_list(wifi_ap_record_t* out, int max);
// Current/saved SSID; false if none
bool wifi_current_ssid(char* out, int size);
esp_netif_t* wifi_netif(void);
// Signal of the current network in dBm, 0 when disconnected
int wifi_rssi(void);
// Suspends Wi-Fi (no auto-reconnect) for monitor mode
void wifi_suspend(void);
// Reconnects to the saved network after monitor mode
void wifi_reconnect(void);

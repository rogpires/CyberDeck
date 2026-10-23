#pragma once

#include <stdbool.h>
#include <stdint.h>

// Scans for a BLE keyboard, pairs with it and keeps reconnecting.
// Typed keys go to the input stream (see input.h).
void keyboard_init(void);
bool keyboard_is_connected(void);
const char* keyboard_name(void);
// Removes all bonds: the next keyboard found in pairing mode is paired again
void keyboard_forget(void);

// Called for every BLE advertisement during keyboard_ble_scan() (name may be NULL)
typedef void (*KeyboardAdvCallback)(const uint8_t* bda, int rssi, const char* name, uint16_t appearance);
// Scans for all BLE devices (not only keyboards); blocks for `seconds`
void keyboard_ble_scan(int seconds, KeyboardAdvCallback callback);

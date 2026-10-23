#pragma once

#include <stdbool.h>
#include <stdint.h>

// USB HID keyboard for the Bad USB tool. Installs TinyUSB (replaces the USB serial console until reboot)
// and waits for the host to enumerate it. Returns false on failure/timeout.
bool badusb_hid_begin(void);
bool badusb_hid_ready(void);

// Presses a key with modifier bits (KEYBOARD_MODIFIER_*), then releases. keycode 0 releases only.
void badusb_hid_tap(uint8_t modifier, uint8_t keycode);
// Types one printable ASCII character (US layout)
void badusb_hid_char(char c);

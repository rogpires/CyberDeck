#pragma once

#include <stdbool.h>

// Soft power switch: "off" is ESP32-S3 deep sleep with the screen, speaker and radios off.
// Touching the screen or pressing BOOT turns the deck back on (a fresh boot). RESET works too.

// Starts the auto power-off timer (saved setting, minutes of inactivity; 0 disables)
void power_init(void);

// Any input or output counts as activity
void power_activity(void);

// Turns the deck off. Does not return.
void power_off(void);

void power_set_autooff(int minutes);
int power_autooff(void);

// True when this boot came from touching the screen or pressing BOOT while off
bool power_woke_from_sleep(void);

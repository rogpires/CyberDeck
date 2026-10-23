#pragma once

#include <stdbool.h>

// Battery monitoring (with charger detection) and the clock (NTP, Brasilia time)
void statusbar_init(void);

// Battery charge 0..100, or -1 when it can't be read
int statusbar_battery_percent(void);
// Smoothed battery voltage in mV, -1 when unknown
int statusbar_battery_mv(void);
// External power: USB host, or a charger detected from the voltage
bool statusbar_charging(void);
// Charging and the cell has reached the charger's final voltage
bool statusbar_battery_full(void);

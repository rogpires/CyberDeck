#pragma once

#include <stdbool.h>

// FT6336 capacitive touch on the ES3C28P. Coordinates are in screen space (320x240 landscape).
void touch_init(void);

// Current touch state. Returns true while a finger is down, filling x and y.
bool touch_read(int* x, int* y);

// Low power mode for power_off(): the controller still pulls INT low when touched
void touch_sleep(void);

// Starts the task that turns touches into input events (tap, swipe, back, keyboard button)
// and feeds taps on the on-screen keyboard to it.
void touch_start_events(void);

// When false (the carousels), the keyboard button zone is a plain tap: there is no such button there.
// "< MENU" always sends EV_BACK.
void touch_set_header_enabled(bool enabled);

// The board's I2C0 bus (i2c_master_bus_handle_t), shared by the touch controller and the audio codec.
// Valid after touch_init().
void* board_i2c_bus(void);

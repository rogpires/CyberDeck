#pragma once

#include <stdbool.h>

// On-screen keyboard for typing with a finger or stylus. Occupies the bottom OSK_H pixels;
// the terminal shrinks above it. Keys go to the input queue like a real keyboard.
#define OSK_H 106

void osk_show(void);
void osk_hide(void);
void osk_toggle(void);
bool osk_visible(void);

// Called by the touch task for every tap; true when the tap hit the keyboard
bool osk_touch(int x, int y);

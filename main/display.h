#pragma once

#include <stdint.h>

// Landscape: 320x240 (ILI9341 with swapped axes)
#define DISPLAY_WIDTH  320
#define DISPLAY_HEIGHT 240

void display_init(void);
// Backlight off (held through deep sleep) and the panel in sleep mode, for power_off()
void display_sleep(void);

// Blocks until the transfer is done, so the buffer can be reused right away.
// Pixels are RGB565 in panel byte order (see display_color()).
void display_draw(int x, int y, int w, int h, const uint16_t* pixels);

// Blits a 320-wide framebuffer region starting at screen row y0 (src may be in PSRAM)
void display_present(int y0, int h, const uint16_t* src);
// Blits only a rectangle of a full-screen (320-wide) framebuffer
void display_present_rect(int x, int y, int w, int h, const uint16_t* src);

// RGB565, byte-swapped for the SPI panel
static inline uint16_t display_color(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (uint16_t)((c >> 8) | (c << 8));
}

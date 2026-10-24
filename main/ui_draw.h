#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "fonts.h"

// Software drawing on an RGB565 (panel byte order) buffer. Colors are 0xRRGGBB.

typedef struct {
    uint16_t* px;
    int w, h;
} Canvas;

#define UI_ACCENT     0xBF00FF
#define UI_STATUS_ON  0xF2F2F7
#define UI_STATUS_OFF 0x45404F

// Height of the header on every screen except the home carousel
#define UI_HEADER_H 16

// Touch zones of the header (screen coordinates), a bit larger than what is drawn
#define UI_BACK_W     80
#define UI_KEYBOARD_X 84
#define UI_KEYBOARD_W 40
#define UI_HEADER_HIT 44   // generous: the top edge of the panel is the least accurate

void ui_fill(Canvas* c, uint32_t color);
void ui_blend(Canvas* c, int x, int y, uint32_t color, int alpha);
void ui_text(Canvas* c, int x, int y, const char* text, const Font* font, uint32_t color, bool shadow);
void ui_icon(Canvas* c, int cx, int cy, int box, int icon_index, uint32_t top, uint32_t bottom);
int ui_icon_find(const char* name);

typedef bool (*UiShapeFn)(float x, float y, void* arg);
// Anti-aliased (4x4 supersampled) shape in the box x0,y0,w,h; inside() gets box-local coordinates
void ui_fill_shape(Canvas* c, int x0, int y0, int w, int h, UiShapeFn inside, void* arg, uint32_t color);
void ui_round_rect(Canvas* c, int x, int y, int w, int h, float radius, uint32_t color);

// Status icons right-aligned at right_x: keyboard (BT), Wi-Fi fan, battery, clock. top is the text row.
void ui_status(Canvas* c, int right_x, int top);
// True if (x,y) is on the Wi-Fi status icon (after ui_status has drawn at least once)
bool ui_hit_wifi(int x, int y);
// "< MENU" back button at x, top
void ui_back(Canvas* c, int x, int top, uint32_t color);
// Power symbol in an 18 x 18 box
void ui_power_icon(Canvas* c, int x, int top, uint32_t color);
// The home screen's off button: a rounded button with the power symbol
#define UI_POWER_BTN_W 46
#define UI_POWER_BTN_H 32
void ui_power_button(Canvas* c, int x, int y);
// Small keyboard glyph (the on-screen keyboard toggle)
void ui_keyboard_icon(Canvas* c, int x, int top, bool active);

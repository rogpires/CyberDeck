#include "ui_draw.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "display.h"
#include "icons.h"
#include "keyboard.h"
#include "statusbar.h"
#include "wifi.h"

#define BATTERY_OK  0x4ADE80
#define BATTERY_MID 0xFACC15
#define BATTERY_LOW 0xF87171

static inline void unpack(uint16_t stored, int* r, int* g, int* b) {
    uint16_t v = (stored >> 8) | (stored << 8);
    *r = ((v >> 11) & 0x1F) << 3;
    *g = ((v >> 5) & 0x3F) << 2;
    *b = (v & 0x1F) << 3;
}

void ui_fill(Canvas* c, uint32_t color) {
    uint16_t v = display_color(color >> 16, color >> 8, color);
    for (int i = 0; i < c->w * c->h; i++) c->px[i] = v;
}

void ui_blend(Canvas* c, int x, int y, uint32_t color, int alpha) {
    if (x < 0 || x >= c->w || y < 0 || y >= c->h || alpha <= 0) return;
    int r = color >> 16 & 0xFF, g = color >> 8 & 0xFF, b = color & 0xFF;
    uint16_t* p = &c->px[y * c->w + x];
    if (alpha >= 255) {
        *p = display_color(r, g, b);
        return;
    }
    int br, bg, bb;
    unpack(*p, &br, &bg, &bb);
    *p = display_color(br + (r - br) * alpha / 255, bg + (g - bg) * alpha / 255, bb + (b - bb) * alpha / 255);
}

void ui_text(Canvas* c, int x, int y, const char* text, const Font* font, uint32_t color, bool shadow) {
    for (int i = 0; text[i]; i++) {
        unsigned char ch = text[i];
        if (ch < 0x20 || ch >= 0x7F) ch = '?';
        const uint16_t* glyph = &font->rows[(ch - 0x20) * font->height];
        for (int gy = 0; gy < font->height; gy++) {
            for (int gx = 0; gx < font->width; gx++) {
                if (!(glyph[gy] & (0x8000 >> gx))) continue;
                if (shadow) ui_blend(c, x + i * font->width + gx + 1, y + gy + 1, 0, 120);
                ui_blend(c, x + i * font->width + gx, y + gy, color, 255);
            }
        }
    }
}

static int icon_alpha(const uint8_t* alpha, float u, float v) {
    int x0 = (int)u, y0 = (int)v;
    int x1 = x0 + 1 < ICON_SIZE ? x0 + 1 : x0;
    int y1 = y0 + 1 < ICON_SIZE ? y0 + 1 : y0;
    float fx = u - x0, fy = v - y0;
    float top = alpha[y0 * ICON_SIZE + x0] + (alpha[y0 * ICON_SIZE + x1] - alpha[y0 * ICON_SIZE + x0]) * fx;
    float bot = alpha[y1 * ICON_SIZE + x0] + (alpha[y1 * ICON_SIZE + x1] - alpha[y1 * ICON_SIZE + x0]) * fx;
    return (int)(top + (bot - top) * fy);
}

void ui_icon(Canvas* c, int cx, int cy, int box, int icon_index, uint32_t top, uint32_t bottom) {
    const uint8_t* alpha = icons[icon_index].alpha;
    float scale = (ICON_SIZE - 1) / (float)box;
    for (int dy = 0; dy < box; dy++) {
        float f = dy / (float)box;
        int r = (top >> 16 & 0xFF) + ((int)(bottom >> 16 & 0xFF) - (int)(top >> 16 & 0xFF)) * f;
        int g = (top >> 8 & 0xFF) + ((int)(bottom >> 8 & 0xFF) - (int)(top >> 8 & 0xFF)) * f;
        int b = (top & 0xFF) + ((int)(bottom & 0xFF) - (int)(top & 0xFF)) * f;
        uint32_t color = (r << 16) | (g << 8) | b;
        for (int dx = 0; dx < box; dx++) {
            int a = icon_alpha(alpha, dx * scale, dy * scale);
            if (a > 0) ui_blend(c, cx - box / 2 + dx, cy - box / 2 + dy, color, a);
        }
    }
}

int ui_icon_find(const char* name) {
    for (size_t i = 0; i < ICON_COUNT; i++) {
        if (strcmp(icons[i].name, name) == 0) return i;
    }
    return 0;
}

void ui_fill_shape(Canvas* c, int x0, int y0, int w, int h, UiShapeFn inside, void* arg, uint32_t color) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int hits = 0;
            for (int sy = 0; sy < 4; sy++) {
                for (int sx = 0; sx < 4; sx++) {
                    if (inside(x + (sx + 0.5f) / 4, y + (sy + 0.5f) / 4, arg)) hits++;
                }
            }
            if (hits) ui_blend(c, x0 + x, y0 + y, color, hits * 255 / 16);
        }
    }
}

typedef struct { float w, h, r; } RoundRect;

static bool in_round_rect(float x, float y, void* arg) {
    RoundRect* rr = arg;
    float half_w = rr->w / 2, half_h = rr->h / 2;
    float qx = fabsf(x - half_w) - (half_w - rr->r), qy = fabsf(y - half_h) - (half_h - rr->r);
    if (qx < 0) qx = 0;
    if (qy < 0) qy = 0;
    return qx * qx + qy * qy <= rr->r * rr->r;
}

void ui_round_rect(Canvas* c, int x, int y, int w, int h, float radius, uint32_t color) {
    RoundRect rr = {w, h, radius};
    ui_fill_shape(c, x, y, w, h, in_round_rect, &rr, color);
}

// Wi-Fi fan: a filled 90 degree wedge split into three bands, lit up to the signal level
typedef struct { float cx, cy, r0, r1; } Band;

static bool in_band(float x, float y, void* arg) {
    Band* b = arg;
    float dx = x - b->cx, dy = y - b->cy;
    if (dy > 0) return false;
    float d = sqrtf(dx * dx + dy * dy);
    return d >= b->r0 && d <= b->r1 && fabsf(dx) <= -dy;
}

static void draw_wifi(Canvas* c, int x, int y) {
    bool connected = wifi_is_connected();
    int rssi = wifi_rssi();
    int bars = !connected ? 0 : (rssi > -60 ? 3 : (rssi > -72 ? 2 : 1));
    static const float radii[3][2] = {{0, 4.6f}, {6.0f, 9.4f}, {10.8f, 14.2f}};
    for (int i = 0; i < 3; i++) {
        Band band = {11.0f, 13.5f, radii[i][0], radii[i][1]};
        ui_fill_shape(c, x, y, 22, 14, in_band, &band, i < bars ? UI_STATUS_ON : UI_STATUS_OFF);
    }
}

// Lightning bolt inside the battery body (box-local coordinates, 24 x 12)
static bool in_bolt(float x, float y, void* arg) {
    static const float px[] = {14.5f, 8.0f, 11.8f, 9.5f, 16.5f, 12.6f, 15.5f};
    static const float py[] = {0.5f, 6.8f, 6.8f, 11.5f, 4.6f, 4.6f, 0.5f};
    const int n = sizeof(px) / sizeof(px[0]);
    bool inside = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        if ((py[i] > y) != (py[j] > y) && x < (px[j] - px[i]) * (y - py[i]) / (py[j] - py[i]) + px[i]) inside = !inside;
    }
    return inside;
}

static void draw_battery(Canvas* c, int x, int y) {
    int percent = statusbar_battery_percent();
    bool charging = statusbar_charging();
    ui_round_rect(c, x, y, 24, 12, 3.5f, UI_STATUS_OFF);
    ui_round_rect(c, x + 24, y + 3, 3, 6, 1.2f, UI_STATUS_OFF);
    if (percent >= 0) {
        uint32_t color = charging ? BATTERY_OK : (percent <= 15 ? BATTERY_LOW : (percent <= 40 ? BATTERY_MID : BATTERY_OK));
        int width = 2 + 18 * percent / 100;
        ui_round_rect(c, x + 2, y + 2, width, 8, 2.2f, color);
    }
    if (charging) {
        // Dark outline first so the bolt reads on both the fill and the empty part
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (dx || dy) ui_fill_shape(c, x + dx, y + dy, 24, 12, in_bolt, NULL, 0x000000);
            }
        }
        ui_fill_shape(c, x, y, 24, 12, in_bolt, NULL, statusbar_battery_full() ? 0xFFFFFF : BATTERY_MID);
    }
}

void ui_status(Canvas* c, int right_x, int top) {
    char clock[8] = "--:--";
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    if (local.tm_year + 1900 >= 2025) strftime(clock, sizeof(clock), "%H:%M", &local);
    int x = right_x - 8 - (int)strlen(clock) * 8;
    ui_text(c, x, top, clock, &fonts[2], UI_STATUS_ON, false);

    // Percentage in the small font, vertically centred on the battery
    int percent = statusbar_battery_percent();
    if (percent >= 0) {
        char text[12];
        snprintf(text, sizeof(text), "%d%%", percent);
        x -= 6 + (int)strlen(text) * fonts[1].width;
        ui_text(c, x, top + 2, text, &fonts[1], UI_STATUS_ON, false);
    }

    x -= 32;
    draw_battery(c, x, top + 2);

    x -= 28;
    draw_wifi(c, x, top + 1);

    x -= 18;
    uint32_t bt = keyboard_is_connected() ? UI_STATUS_ON : UI_STATUS_OFF;
    ui_icon(c, x + 8, top + 8, 15, ui_icon_find("bluetooth"), bt, bt);
}

// A filled left-pointing triangle followed by "MENU"
typedef struct { float w, h; } Triangle;

static bool in_triangle(float x, float y, void* arg) {
    Triangle* t = arg;
    float mid = t->h / 2;
    return x >= 0 && x <= t->w && fabsf(y - mid) <= mid * (x / t->w);
}

void ui_back(Canvas* c, int x, int top, uint32_t color) {
    Triangle t = {6, 10};
    ui_fill_shape(c, x, top + 3, 7, 10, in_triangle, &t, color);
    ui_text(c, x + 10, top, "MENU", &fonts[2], color, false);
}

void ui_keyboard_icon(Canvas* c, int x, int top, bool active) {
    uint32_t body = active ? UI_ACCENT : UI_STATUS_OFF;
    ui_round_rect(c, x, top + 2, 22, 12, 2.5f, body);
    // Key dots and space bar, cut out in black
    for (int row = 0; row < 2; row++) {
        for (int col = 0; col < 5; col++) {
            ui_round_rect(c, x + 3 + col * 3 + (row ? 1 : 0), top + 4 + row * 3, 2, 2, 0.6f, 0x000000);
        }
    }
    ui_round_rect(c, x + 6, top + 10, 10, 2, 0.8f, 0x000000);
}

// Power symbol: a ring open at the top and a bar through the gap
typedef struct { float cx, cy, r; } Power;

static bool in_power(float x, float y, void* arg) {
    Power* p = arg;
    float dx = x - p->cx, dy = y - p->cy;
    float d = sqrtf(dx * dx + dy * dy);
    const float thick = 2.2f;
    bool ring = d >= p->r - thick && d <= p->r && !(dy < 0 && fabsf(dx) < p->r * 0.55f);
    bool bar = fabsf(dx) <= thick / 2 + 0.2f && dy >= -p->r - 1.5f && dy <= 0.5f;
    return ring || bar;
}

void ui_power_icon(Canvas* c, int x, int top, uint32_t color) {
    Power p = {9.0f, 10.0f, 7.5f};
    ui_fill_shape(c, x, top, 18, 18, in_power, &p, color);
}

void ui_power_button(Canvas* c, int x, int y) {
    ui_round_rect(c, x, y, UI_POWER_BTN_W, UI_POWER_BTN_H, 10.0f, 0x3A2A4A);
    ui_round_rect(c, x + 1, y + 1, UI_POWER_BTN_W - 2, UI_POWER_BTN_H - 2, 9.0f, 0x1A1028);
    ui_power_icon(c, x + (UI_POWER_BTN_W - 18) / 2, y + (UI_POWER_BTN_H - 18) / 2, 0xE5484D);
}

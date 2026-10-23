#include "carousel.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "display.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "fonts.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "icons.h"
#include "input.h"
#include "keyboard.h"
#include "statusbar.h"
#include "term.h"
#include "power.h"
#include "sound.h"
#include "touch.h"
#include "ui_draw.h"
#include "wifi.h"

// The home screen owns the whole display: the terminal's status bar is hidden while it runs
#define FB_W     DISPLAY_WIDTH
#define FB_H     DISPLAY_HEIGHT
#define CENTER   160
#define TILE_CY  96
#define LABEL_Y  170
#define DOTS_Y   212

// Palette from TentacleOS (github.com/HighCodeh/TentacleOS): near-black with a per-category accent
#define BG_TOP        0x12001F
#define BG_BOTTOM     0x000000
#define TILE_TOP      0x0A0014   // dark tile (unselected)
#define TILE_BOTTOM   0x05000A
#define TILE_BORDER   0x3A2A4A   // dim border (unselected)
#define ICON_LIT_TOP  0xFFFFFF
#define ICON_LIT_BOT  0xEBD9FF
#define ICON_DIM      0x6E6A78
#define LABEL         0xFFFFFF
#define DOT_OFF       0x3A2A4A

#define FLOAT_AMP 5       // TentacleOS: HOME_FLOAT_AMP
#define FLOAT_MS  1600    // TentacleOS: HOME_FLOAT_MS (each way)
#define STATUS_MS 2000    // how often the status icons refresh

// Region redrawn while the centre tile floats: tile + glow + shadow + float range
#define FLOAT_X  (CENTER - 82)
#define FLOAT_W  164
#define FLOAT_Y  (TILE_CY - 86)
#define FLOAT_H  184

static uint16_t* fb;      // what is sent to the panel
static uint16_t* base;    // everything except the centre tile, reused while it floats
static uint16_t* bg;      // background gradient, never changes
static uint16_t* target;  // where the drawing helpers write

static inline uint32_t mixc(uint32_t a, uint32_t b, int t) {
    int r = ((a >> 16 & 0xFF) * (255 - t) + (b >> 16 & 0xFF) * t) / 255;
    int g = ((a >> 8 & 0xFF) * (255 - t) + (b >> 8 & 0xFF) * t) / 255;
    int bl = ((a & 0xFF) * (255 - t) + (b & 0xFF) * t) / 255;
    return (r << 16) | (g << 8) | bl;
}

// RGB565 (byte-swapped for the panel) helpers
static inline void unpack(uint16_t stored, int* r, int* g, int* b) {
    uint16_t v = (stored >> 8) | (stored << 8);
    *r = ((v >> 11) & 0x1F) << 3;
    *g = ((v >> 5) & 0x3F) << 2;
    *b = (v & 0x1F) << 3;
}

static inline void blend(int x, int y, int r, int g, int b, int a) {
    if (x < 0 || x >= FB_W || y < 0 || y >= FB_H || a <= 0) return;
    if (a >= 255) {
        target[y * FB_W + x] = display_color(r, g, b);
        return;
    }
    int br, bgc, bb;
    unpack(target[y * FB_W + x], &br, &bgc, &bb);
    target[y * FB_W + x] = display_color(br + (r - br) * a / 255, bgc + (g - bgc) * a / 255, bb + (b - bb) * a / 255);
}

static void build_background(void) {
    int rt = BG_TOP >> 16 & 0xFF, gt = BG_TOP >> 8 & 0xFF, bt = BG_TOP & 0xFF;
    int rb = BG_BOTTOM >> 16 & 0xFF, gb = BG_BOTTOM >> 8 & 0xFF, bb = BG_BOTTOM & 0xFF;
    for (int y = 0; y < FB_H; y++) {
        uint16_t c = display_color(rt + (rb - rt) * y / FB_H, gt + (gb - gt) * y / FB_H, bt + (bb - bt) * y / FB_H);
        for (int x = 0; x < FB_W; x++) bg[y * FB_W + x] = c;
    }
}

// Signed distance to a rounded rectangle centred at (0,0)
static float rr_sdf(float dx, float dy, float half, float radius) {
    float qx = fabsf(dx) - (half - radius);
    float qy = fabsf(dy) - (half - radius);
    if (qx < 0) qx = 0;
    if (qy < 0) qy = 0;
    return sqrtf(qx * qx + qy * qy) - radius;
}

// Scales one channel of a packed color by factor/256
static inline int dimc(int color, int shift, int factor) {
    return ((color >> shift) & 0xFF) * factor / 256;
}

// Centre tile: dark fill tinted by the accent, accent border, glow and drop shadow
static void draw_tile(int cx, int cy, int size, uint32_t accent) {
    float half = size / 2.0f;
    float radius = size / 4.5f;
    int top = mixc(TILE_TOP, accent, 70), bottom = TILE_TOP;
    const int glow = 28, shadow = 6;

    int reach = (int)half + glow + shadow + 2;
    for (int dy = -reach; dy <= reach; dy++) {
        for (int dx = -reach; dx <= reach; dx++) {
            int x = cx + dx, y = cy + dy;
            if (x < 0 || x >= FB_W || y < 0 || y >= FB_H) continue;
            float d = rr_sdf(dx, dy, half, radius);
            if (d > 0 && d < glow) {
                float t = 1.0f - d / glow;
                blend(x, y, accent >> 16 & 0xFF, accent >> 8 & 0xFF, accent & 0xFF, (int)(t * t * 170));
            }
            float ds = rr_sdf(dx, dy - shadow, half, radius);
            if (ds < 5 && d > -1) {
                float t = ds < 0 ? 1.0f : 1.0f - ds / 5;
                blend(x, y, 0, 0, 0, (int)(t * 90));
            }
            float coverage = 0.5f - d;
            if (coverage > 0) {
                if (coverage > 1) coverage = 1;
                int r, g, b;
                if (d > -2.5f) {
                    r = accent >> 16 & 0xFF; g = accent >> 8 & 0xFF; b = accent & 0xFF;
                } else {
                    float f = (dy + half) / (2 * half);
                    if (f < 0) f = 0;
                    if (f > 1) f = 1;
                    r = (top >> 16 & 0xFF) + ((bottom >> 16 & 0xFF) - (top >> 16 & 0xFF)) * f;
                    g = (top >> 8 & 0xFF) + ((bottom >> 8 & 0xFF) - (top >> 8 & 0xFF)) * f;
                    b = (top & 0xFF) + ((bottom & 0xFF) - (top & 0xFF)) * f;
                }
                blend(x, y, r, g, b, (int)(coverage * 255));
            }
        }
    }
}

// Bilinear sample of an icon's alpha mask (0..255)
static int icon_alpha(const uint8_t* alpha, float u, float v) {
    int x0 = (int)u, y0 = (int)v;
    int x1 = x0 + 1 < ICON_SIZE ? x0 + 1 : x0;
    int y1 = y0 + 1 < ICON_SIZE ? y0 + 1 : y0;
    float fx = u - x0, fy = v - y0;
    int a00 = alpha[y0 * ICON_SIZE + x0], a10 = alpha[y0 * ICON_SIZE + x1];
    int a01 = alpha[y1 * ICON_SIZE + x0], a11 = alpha[y1 * ICON_SIZE + x1];
    float top = a00 + (a10 - a00) * fx;
    float bot = a01 + (a11 - a01) * fx;
    return (int)(top + (bot - top) * fy);
}

// Icon with a vertical gradient (top -> bottom colors)
static void draw_icon(int cx, int cy, int box, int icon_index, uint32_t top, uint32_t bottom) {
    const uint8_t* alpha = icons[icon_index].alpha;
    float scale = (ICON_SIZE - 1) / (float)box;
    for (int dy = 0; dy < box; dy++) {
        float f = dy / (float)box;
        int r = (top >> 16 & 0xFF) + ((int)(bottom >> 16 & 0xFF) - (int)(top >> 16 & 0xFF)) * f;
        int g = (top >> 8 & 0xFF) + ((int)(bottom >> 8 & 0xFF) - (int)(top >> 8 & 0xFF)) * f;
        int b = (top & 0xFF) + ((int)(bottom & 0xFF) - (int)(top & 0xFF)) * f;
        for (int dx = 0; dx < box; dx++) {
            int a = icon_alpha(alpha, dx * scale, dy * scale);
            if (a > 0) blend(cx - box / 2 + dx, cy - box / 2 + dy, r, g, b, a);
        }
    }
}

static void draw_text(int x, int y, const char* text, const Font* font, uint32_t color, bool shadow) {
    int r = color >> 16 & 0xFF, g = color >> 8 & 0xFF, b = color & 0xFF;
    for (int i = 0; text[i]; i++) {
        unsigned char ch = text[i];
        if (ch < 0x20 || ch >= 0x7F) ch = '?';
        const uint16_t* glyph = &font->rows[(ch - 0x20) * font->height];
        for (int gy = 0; gy < font->height; gy++) {
            for (int gx = 0; gx < font->width; gx++) {
                if (glyph[gy] & (0x8000 >> gx)) {
                    if (shadow) blend(x + i * font->width + gx + 1, y + gy + 1, 0, 0, 0, 120);
                    blend(x + i * font->width + gx, y + gy, r, g, b, 255);
                }
            }
        }
    }
}

static void to_upper(const char* in, char* out, size_t size) {
    size_t i = 0;
    for (; in[i] && i < size - 1; i++) out[i] = (in[i] >= 'a' && in[i] <= 'z') ? in[i] - 32 : in[i];
    out[i] = 0;
}

// Coverflow: side tiles are rendered flat into a small buffer, then warped into a trapezoid so they look
// rotated toward the viewer (the edge nearer the centre is full height, the far edge shorter).
#define MAXTILE 112
static uint32_t* tile_tmp; // MAXTILE x MAXTILE, 0xAARRGGBB

static void tmp_tile(int size, int bright) {
    float half = size / 2.0f, radius = size / 4.5f;
    for (int py = 0; py < size; py++) {
        for (int px = 0; px < size; px++) {
            float d = rr_sdf(px - half + 0.5f, py - half + 0.5f, half, radius);
            float cov = 0.5f - d;
            uint32_t out = 0;
            if (cov > 0) {
                if (cov > 1) cov = 1;
                int r, g, b;
                if (d > -2.5f) {
                    r = dimc(TILE_BORDER, 16, bright); g = dimc(TILE_BORDER, 8, bright); b = dimc(TILE_BORDER, 0, bright);
                } else {
                    float f = (float)py / size;
                    r = dimc(TILE_TOP, 16, bright) + (dimc(TILE_BOTTOM, 16, bright) - dimc(TILE_TOP, 16, bright)) * f;
                    g = dimc(TILE_TOP, 8, bright) + (dimc(TILE_BOTTOM, 8, bright) - dimc(TILE_TOP, 8, bright)) * f;
                    b = dimc(TILE_TOP, 0, bright) + (dimc(TILE_BOTTOM, 0, bright) - dimc(TILE_TOP, 0, bright)) * f;
                }
                out = ((uint32_t)(cov * 255) << 24) | (r << 16) | (g << 8) | b;
            }
            tile_tmp[py * MAXTILE + px] = out;
        }
    }
}

static void tmp_icon(int size, int icon_index, int bright) {
    const uint8_t* alpha = icons[icon_index].alpha;
    int box = size * 56 / 100, off = (size - box) / 2;
    float scale = (ICON_SIZE - 1) / (float)box;
    int r = dimc(ICON_DIM, 16, bright), g = dimc(ICON_DIM, 8, bright), b = dimc(ICON_DIM, 0, bright);
    for (int dy = 0; dy < box; dy++) {
        for (int dx = 0; dx < box; dx++) {
            int a = icon_alpha(alpha, dx * scale, dy * scale);
            if (a <= 0) continue;
            uint32_t* p = &tile_tmp[(off + dy) * MAXTILE + off + dx];
            int pa = *p >> 24, pr = *p >> 16 & 0xFF, pg = *p >> 8 & 0xFF, pb = *p & 0xFF;
            pr += (r - pr) * a / 255;
            pg += (g - pg) * a / 255;
            pb += (b - pb) * a / 255;
            *p = ((uint32_t)pa << 24) | (pr << 16) | (pg << 8) | pb;
        }
    }
}

static void warp_blit(int size, int cx, int cy, float offset) {
    const float squash = 0.62f;            // horizontal foreshortening
    int dest_w = (int)(size * squash);
    float h_inner = size, h_outer = size * 0.78f;
    bool left = offset < 0;                 // a left tile shows its right (near) edge tallest
    int x0 = cx - dest_w / 2;
    for (int i = 0; i < dest_w; i++) {
        float t = dest_w > 1 ? (float)i / (dest_w - 1) : 0;
        float colh = left ? h_outer + (h_inner - h_outer) * t : h_inner + (h_outer - h_inner) * t;
        int src_col = (int)(t * (size - 1));
        int h = (int)colh, top = cy - h / 2;
        for (int j = 0; j < h; j++) {
            int src_row = h > 1 ? j * (size - 1) / (h - 1) : 0;
            uint32_t p = tile_tmp[src_row * MAXTILE + src_col];
            int a = p >> 24;
            if (a) blend(x0 + i, top + j, p >> 16 & 0xFF, p >> 8 & 0xFF, p & 0xFF, a);
        }
    }
}

static void draw_status(void) {
    Canvas c = {target, FB_W, FB_H};
    ui_status(&c, FB_W, 6);
}

// Layout

#define SPREAD  118.0f
static int layout_x(float offset) { return CENTER + (int)(SPREAD * tanhf(offset * 0.85f)); }
static float layout_size(float offset) { return 56.0f + 46.0f * expf(-offset * offset / 1.1f); }
static int layout_bright(float offset) {
    float b = 0.42f + 0.58f * expf(-offset * offset / 2.2f);
    return (int)(b * 256);
}

typedef struct {
    const char* const* labels;
    const int* icon_index;
    const uint32_t* accents;
    int count;
    bool show_back;
} Items;

// Index of the item drawn as the flat centre tile, or -1 while sliding between two
static int centre_item(const Items* items, float pos) {
    int nearest = (int)floorf(pos + 0.5f);
    if (nearest < 0 || nearest >= items->count || fabsf(nearest - pos) >= 0.5f) return -1;
    return nearest;
}

// Everything except the centre tile goes to `base`
static void compose_base(const Items* items, float pos) {
    memcpy(base, bg, FB_W * FB_H * sizeof(uint16_t));
    target = base;

    // Side tiles, far to near
    int lo = (int)floorf(pos) - 3, hi = (int)ceilf(pos) + 3;
    struct { int index; float offset; } order[16];
    int n = 0;
    int centre = centre_item(items, pos);
    for (int i = lo; i <= hi; i++) {
        if (i < 0 || i >= items->count || i == centre || n >= 16) continue;
        order[n].index = i;
        order[n].offset = i - pos;
        n++;
    }
    for (int a = 0; a < n; a++) {
        for (int b = a + 1; b < n; b++) {
            if (fabsf(order[b].offset) > fabsf(order[a].offset)) {
                int ti = order[a].index; float to = order[a].offset;
                order[a].index = order[b].index; order[a].offset = order[b].offset;
                order[b].index = ti; order[b].offset = to;
            }
        }
    }
    for (int i = 0; i < n; i++) {
        float offset = order[i].offset;
        int size = (int)layout_size(offset);
        if (size > MAXTILE) size = MAXTILE;
        int bright = layout_bright(offset);
        tmp_tile(size, bright);
        tmp_icon(size, items->icon_index[order[i].index], bright);
        warp_blit(size, layout_x(offset), TILE_CY, offset);
    }

    // Label, arrows, dots
    int sel = (int)floorf(pos + 0.5f);
    if (sel < 0) sel = 0;
    if (sel >= items->count) sel = items->count - 1;
    uint32_t accent = items->accents[sel];
    char upper[24];
    to_upper(items->labels[sel], upper, sizeof(upper));
    const Font* label_font = &fonts[3]; // 12x24
    int width = strlen(upper) * label_font->width;
    draw_text((FB_W - width) / 2, LABEL_Y, upper, label_font, LABEL, true);
    if (sel > 0) draw_text((FB_W - width) / 2 - 28, LABEL_Y + 3, "<", label_font, accent, false);
    if (sel < items->count - 1) draw_text((FB_W + width) / 2 + 16, LABEL_Y + 3, ">", label_font, accent, false);

    int dot_gap = 13;
    int dots_x = (FB_W - (items->count - 1) * dot_gap) / 2;
    for (int i = 0; i < items->count; i++) {
        uint32_t color = i == sel ? accent : DOT_OFF;
        int cx = dots_x + i * dot_gap, radius = i == sel ? 4 : 3;
        for (int dy = -radius; dy <= radius; dy++) {
            for (int dx = -radius; dx <= radius; dx++) {
                float d = sqrtf(dx * dx + dy * dy) - radius;
                if (d < 0.5f) blend(cx + dx, DOTS_Y + dy, color >> 16 & 0xFF, color >> 8 & 0xFF, color & 0xFF,
                                    d < -0.5f ? 255 : (int)((0.5f - d) * 255));
            }
        }
    }

    Canvas canvas = {target, FB_W, FB_H};
    if (items->show_back) {
        // Same place as the header's "< MENU" on every other screen
        ui_back(&canvas, 6, 6, accent);
    } else {
        // The home screen has nothing to go back to: the corner is the off button
        ui_power_button(&canvas, 4, 4);
    }
    draw_status();
}

static void draw_centre(const Items* items, float pos, float pop, int float_dy) {
    int centre = centre_item(items, pos);
    if (centre < 0) return;
    target = fb;
    float offset = centre - pos;
    int size = (int)(layout_size(offset) * pop);
    int cx = layout_x(offset), cy = TILE_CY + float_dy;
    draw_tile(cx, cy, size, items->accents[centre]);
    draw_icon(cx, cy, size * 56 / 100, items->icon_index[centre], ICON_LIT_TOP, ICON_LIT_BOT);
}

// Full frame: rebuild the base, add the centre tile, send everything
static void frame_full(const Items* items, float pos, float pop, int float_dy) {
    compose_base(items, pos);
    memcpy(fb, base, FB_W * FB_H * sizeof(uint16_t));
    draw_centre(items, pos, pop, float_dy);
    display_present(0, FB_H, fb);
}

// Floating frame: only the centre tile's area changes
static void frame_float(const Items* items, float pos, int float_dy) {
    for (int y = FLOAT_Y; y < FLOAT_Y + FLOAT_H; y++) {
        memcpy(fb + y * FB_W + FLOAT_X, base + y * FB_W + FLOAT_X, FLOAT_W * sizeof(uint16_t));
    }
    draw_centre(items, pos, 1.0f, float_dy);
    display_present_rect(FLOAT_X, FLOAT_Y, FLOAT_W, FLOAT_H, fb);
}

// Idle floating offset of the centre tile: a sine is the same curve as an ease-in-out ping-pong
static int float_offset(void) {
    int64_t ms = esp_timer_get_time() / 1000;
    return (int)roundf(FLOAT_AMP * sinf(2.0f * (float)M_PI * (ms % (2 * FLOAT_MS)) / (2.0f * FLOAT_MS)));
}

// Next input as an action: 'L'/'R' move, '\n' choose, 27 back, 0 nothing
static char next_action(int* tap_x, int* tap_y) {
    InputEvent event;
    if (!input_wait(&event, 0)) return 0;
    switch (event.type) {
        case EV_SWIPE_LEFT:  return 'R'; // content follows the finger
        case EV_SWIPE_RIGHT: return 'L';
        case EV_BACK:        return 27;
        case EV_TAP:
            *tap_x = event.x;
            *tap_y = event.y;
            return 'T';
        case EV_KEY:
            break;
        default:
            return 0;
    }
    char c = event.key;
    if (c == '\r' || c == '\n') return '\n';
    if (c == 0x7f || c == '\b') return 27;
    if (c != '\x1b') return 0;
    if (!input_read(&c, 30)) return 27; // lone Esc
    if (c != '[' && c != 'O') return 27;
    if (!input_read(&c, 30)) return 27;
    while (c >= '0' && c <= ';') {
        if (!input_read(&c, 30)) return 27;
    }
    return c == 'C' ? 'R' : (c == 'D' ? 'L' : 0);
}

// "Turn off?" dialog over the current frame. Returns true when confirmed.
#define DLG_X 50
#define DLG_Y 62
#define DLG_W 220
#define DLG_H 112
#define BTN_Y (DLG_Y + 70)
#define BTN_H 28
#define BTN_W 92
#define BTN_CANCEL_X (DLG_X + 14)
#define BTN_OFF_X    (DLG_X + DLG_W - 14 - BTN_W)

static void draw_dialog(int selected) {
    memcpy(fb, base, FB_W * FB_H * sizeof(uint16_t));
    target = fb;
    Canvas c = {fb, FB_W, FB_H};
    for (int y = 0; y < FB_H; y++) {
        for (int x = 0; x < FB_W; x++) ui_blend(&c, x, y, 0x000000, 160);
    }
    ui_round_rect(&c, DLG_X - 2, DLG_Y - 2, DLG_W + 4, DLG_H + 4, 14.0f, 0xBF00FF);
    ui_round_rect(&c, DLG_X, DLG_Y, DLG_W, DLG_H, 12.0f, 0x140A22);
    const char* title = "Desligar o deck?";
    ui_text(&c, DLG_X + (DLG_W - (int)strlen(title) * 8) / 2, DLG_Y + 14, title, &fonts[2], 0xFFFFFF, false);
    const char* hint = "toque na tela ou BOOT liga";
    ui_text(&c, DLG_X + (DLG_W - (int)strlen(hint) * 6) / 2, DLG_Y + 40, hint, &fonts[1], 0x8A8594, false);

    const char* labels[2] = {"Cancelar", "Desligar"};
    const int xs[2] = {BTN_CANCEL_X, BTN_OFF_X};
    for (int i = 0; i < 2; i++) {
        uint32_t fill = i != selected ? 0x2E2240 : (i == 1 ? 0xE5484D : 0x6D28D9);
        ui_round_rect(&c, xs[i], BTN_Y, BTN_W, BTN_H, 8.0f, fill);
        ui_text(&c, xs[i] + (BTN_W - (int)strlen(labels[i]) * 8) / 2, BTN_Y + 6, labels[i], &fonts[2], 0xFFFFFF, false);
    }
    display_present(0, FB_H, fb);
}

static bool confirm_power_off(void) {
    int selected = 0; // Cancelar first, so a stray Enter doesn't turn the deck off
    draw_dialog(selected);
    while (true) {
        InputEvent event;
        input_wait(&event, UINT32_MAX);
        if (event.type == EV_TAP) {
            bool in_row = event.y >= BTN_Y - 6 && event.y <= BTN_Y + BTN_H + 6;
            if (in_row && event.x >= BTN_OFF_X - 6 && event.x <= BTN_OFF_X + BTN_W + 6) return true;
            if (in_row && event.x >= BTN_CANCEL_X - 6 && event.x <= BTN_CANCEL_X + BTN_W + 6) return false;
            if (event.x < DLG_X || event.x > DLG_X + DLG_W || event.y < DLG_Y || event.y > DLG_Y + DLG_H) return false;
        } else if (event.type == EV_BACK) {
            return false;
        } else if (event.type == EV_KEY) {
            char c = event.key;
            if (c == '\r' || c == '\n') return selected == 1;
            if (c == 0x1b) {
                char next;
                if (!input_read(&next, 30)) return false; // lone Esc cancels
                if (next == '[' || next == 'O') {
                    char code = 0;
                    while (input_read(&code, 30) && code >= '0' && code <= ';') {}
                    if (code == 'C' || code == 'D') {
                        selected = 1 - selected;
                        sound_play(SOUND_NAV);
                        draw_dialog(selected);
                    }
                }
            }
        }
    }
}

int carousel_run(const char* const* labels, const int* icon_index, const uint32_t* accents, int count, int start, bool show_back) {
    if (fb == NULL) {
        fb = heap_caps_malloc(FB_W * FB_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        base = heap_caps_malloc(FB_W * FB_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        bg = heap_caps_malloc(FB_W * FB_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        tile_tmp = heap_caps_malloc(MAXTILE * MAXTILE * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
        build_background();
    }
    Items items = {labels, icon_index, accents, count, show_back};
    int target_index = start < 0 || start >= count ? 0 : start;
    float pos = target_index;

    term_pause(true);
    term_hide_bar(true);
    touch_set_header_enabled(false); // no keyboard button on this screen
    int float_dy = float_offset();
    frame_full(&items, pos, 1.0f, float_dy);
    int64_t last_status = esp_timer_get_time();

    int result = -2;
    int chosen = -1;

    while (result == -2) {
        int tap_x = 0, tap_y = 0;
        int before = target_index;
        switch (next_action(&tap_x, &tap_y)) {
            case 'L': if (target_index > 0) target_index--; break;
            case 'R': if (target_index < count - 1) target_index++; break;
            case '\n': chosen = target_index; break;
            case 27:
                if (show_back) {
                    result = -1;
                } else {
                    // Home: the corner (or Esc) is the off button
                    sound_play(SOUND_SELECT);
                    if (confirm_power_off()) {
                        touch_set_header_enabled(true);
                        term_hide_bar(false);
                        term_pause(false);
                        power_off();
                    }
                    frame_full(&items, pos, 1.0f, float_dy);
                }
                break;
            case 'T':
                // Centre tile opens it; the sides step toward that side
                if (tap_x >= 105 && tap_x <= 215 && tap_y >= 40 && tap_y <= 155) {
                    chosen = target_index;
                } else if (tap_x < CENTER) {
                    if (target_index > 0) target_index--;
                } else {
                    if (target_index < count - 1) target_index++;
                }
                break;
            default: break;
        }
        if (target_index != before) sound_play(SOUND_NAV);
        if (result != -2) {
            sound_play(SOUND_NAV);
            break;
        }

        if (chosen >= 0) {
            sound_play(SOUND_SELECT);
            target_index = chosen;
            // Finish the slide, then the pop
            while (fabsf(pos - chosen) > 0.01f) {
                pos += (chosen - pos) * 0.4f;
                if (fabsf(pos - chosen) <= 0.01f) pos = chosen;
                frame_full(&items, pos, 1.0f, float_dy);
            }
            static const float pop[] = {1.06f, 1.12f, 1.16f, 1.12f, 1.05f, 1.0f};
            for (size_t i = 0; i < sizeof(pop) / sizeof(pop[0]); i++) frame_full(&items, pos, pop[i], float_dy);
            result = chosen;
            break;
        }

        if (fabsf(pos - target_index) > 0.01f) {
            // Sliding: every frame changes everything
            pos += (target_index - pos) * 0.35f;
            if (fabsf(pos - target_index) <= 0.01f) pos = target_index;
            frame_full(&items, pos, 1.0f, float_dy);
            last_status = esp_timer_get_time();
        } else if (esp_timer_get_time() - last_status > STATUS_MS * 1000LL) {
            // Clock, battery and connection icons
            frame_full(&items, pos, 1.0f, float_dy);
            last_status = esp_timer_get_time();
        } else {
            int dy = float_offset();
            if (dy != float_dy) {
                float_dy = dy;
                frame_float(&items, pos, float_dy);
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }

    touch_set_header_enabled(true);
    term_hide_bar(false);
    term_pause(false);
    return result;
}

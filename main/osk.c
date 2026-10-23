#include "osk.h"

#include <string.h>

#include "display.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "input.h"
#include "sound.h"
#include "term.h"
#include "ui_draw.h"

#define TOP      (DISPLAY_HEIGHT - OSK_H)
#define ROW_H    26
#define KEY_H    24
#define HALF_W   16   // a row is 20 half-units of 16 px

#define KEY_BG     0x1C1426
#define KEY_FN_BG  0x2E2240
#define KEY_TEXT   0xF2F2F7
#define KEY_FN_TXT 0xC9A8FF
#define PANEL_BG   0x07020C

typedef enum { A_NONE, A_SHIFT, A_LETTERS, A_SYMBOLS, A_MORE } Action;

typedef struct {
    const char* label;
    uint8_t width;      // in half-units
    const char* out;    // bytes sent, NULL for actions
    uint8_t action;
} Key;

#define K(c)        {c, 2, c, A_NONE}
#define FN(l, w, o) {l, w, o, A_NONE}
#define ACT(l, w, a) {l, w, NULL, a}
#define END         {NULL, 0, NULL, A_NONE}

static const Key letters[4][12] = {
    {K("q"), K("w"), K("e"), K("r"), K("t"), K("y"), K("u"), K("i"), K("o"), K("p"), END},
    {K("a"), K("s"), K("d"), K("f"), K("g"), K("h"), K("j"), K("k"), K("l"), FN("DEL", 2, "\x7f"), END},
    {ACT("Aa", 2, A_SHIFT), K("z"), K("x"), K("c"), K("v"), K("b"), K("n"), K("m"), K("-"), FN("ENT", 2, "\r"), END},
    {ACT("123", 3, A_SYMBOLS), FN("^C", 2, "\x03"), FN("", 8, " "), FN("\x18", 2, "\x1b[A"), FN("\x19", 2, "\x1b[B"), FN("ESC", 3, "\x1b"), END},
};

static const Key symbols[4][12] = {
    {K("1"), K("2"), K("3"), K("4"), K("5"), K("6"), K("7"), K("8"), K("9"), K("0"), END},
    {K("/"), K(":"), K(";"), K("("), K(")"), K("$"), K("&"), K("@"), K("\""), FN("DEL", 2, "\x7f"), END},
    {ACT("#+=", 2, A_MORE), K("."), K(","), K("?"), K("!"), K("'"), K("_"), K("="), K("+"), FN("ENT", 2, "\r"), END},
    {ACT("ABC", 3, A_LETTERS), FN("^C", 2, "\x03"), FN("", 8, " "), FN("\x18", 2, "\x1b[A"), FN("\x19", 2, "\x1b[B"), FN("ESC", 3, "\x1b"), END},
};

static const Key more[4][12] = {
    {K("["), K("]"), K("{"), K("}"), K("#"), K("%"), K("^"), K("*"), K("<"), K(">"), END},
    {K("\\"), K("|"), K("~"), K("`"), K("'"), K("\""), K("!"), K("?"), K("."), FN("DEL", 2, "\x7f"), END},
    {ACT("123", 2, A_SYMBOLS), K("_"), K("-"), K("+"), K("="), K("/"), K("@"), K("$"), K("&"), FN("ENT", 2, "\r"), END},
    {ACT("ABC", 3, A_LETTERS), FN("^C", 2, "\x03"), FN("", 8, " "), FN("\x18", 2, "\x1b[A"), FN("\x19", 2, "\x1b[B"), FN("ESC", 3, "\x1b"), END},
};

static const Key (*layer)[12] = letters;
static bool shift;
static volatile bool visible;
static Canvas canvas;              // full-screen sized, only the bottom OSK_H rows are used
static SemaphoreHandle_t lock;

static bool is_function(const Key* key) {
    return key->action != A_NONE || key->out == NULL || strlen(key->out) != 1 || key->out[0] < 0x20 ||
           key->width != 2;
}

static void key_label(const Key* key, char* out) {
    strlcpy(out, key->label, 8);
    if (layer == letters && shift && strlen(out) == 1 && out[0] >= 'a' && out[0] <= 'z') out[0] -= 32;
}

static void draw_key(int row, int x_half, const Key* key, bool pressed) {
    int x = x_half * HALF_W, y = TOP + 2 + row * ROW_H, w = key->width * HALF_W;
    // Background behind the key, then the key itself inset by 2 px
    for (int yy = y - 1; yy < y + ROW_H - 1 && yy < DISPLAY_HEIGHT; yy++) {
        for (int xx = x; xx < x + w; xx++) ui_blend(&canvas, xx, yy, PANEL_BG, 255);
    }
    bool fn = is_function(key);
    bool lit = pressed || (key->action == A_SHIFT && shift);
    uint32_t bg = lit ? UI_ACCENT : (fn ? KEY_FN_BG : KEY_BG);
    ui_round_rect(&canvas, x + 2, y, w - 4, KEY_H, 5.0f, bg);

    char label[8];
    key_label(key, label);
    if (label[0] == 0) {
        // Space bar: a short line
        ui_round_rect(&canvas, x + w / 2 - 16, y + KEY_H / 2, 32, 2, 1.0f, KEY_FN_TXT);
        return;
    }
    const Font* font = &fonts[2]; // 8x16
    int text_w = strlen(label) * font->width;
    // Arrow keys use the labels \x18 (up) and \x19 (down): drawn as triangles
    if (label[0] == '\x18' || label[0] == '\x19') {
        bool up = label[0] == '\x18';
        for (int i = 0; i < 6; i++) {
            int yy = y + KEY_H / 2 + (up ? i - 3 : 3 - i);
            for (int xx = -i; xx <= i; xx++) ui_blend(&canvas, x + w / 2 + xx, yy, lit ? 0xFFFFFF : KEY_FN_TXT, 255);
        }
        return;
    }
    ui_text(&canvas, x + (w - text_w) / 2, y + (KEY_H - font->height) / 2, label, font,
            lit ? 0xFFFFFF : (fn ? KEY_FN_TXT : KEY_TEXT), false);
}

static void draw_all(void) {
    for (int y = TOP; y < DISPLAY_HEIGHT; y++) {
        for (int x = 0; x < DISPLAY_WIDTH; x++) ui_blend(&canvas, x, y, PANEL_BG, 255);
    }
    for (int row = 0; row < 4; row++) {
        int x_half = 0;
        for (int i = 0; layer[row][i].label != NULL; i++) {
            draw_key(row, x_half, &layer[row][i], false);
            x_half += layer[row][i].width;
        }
    }
    display_present_rect(0, TOP, DISPLAY_WIDTH, OSK_H, canvas.px);
}

static void ensure_canvas(void) {
    if (canvas.px == NULL) {
        canvas.px = heap_caps_malloc(DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        canvas.w = DISPLAY_WIDTH;
        canvas.h = DISPLAY_HEIGHT;
        lock = xSemaphoreCreateMutex();
    }
}

void osk_show(void) {
    ensure_canvas();
    if (visible) return;
    term_reserve_bottom(OSK_H);
    xSemaphoreTake(lock, portMAX_DELAY);
    layer = letters;
    shift = false;
    visible = true;
    draw_all();
    xSemaphoreGive(lock);
    term_refresh_status();
}

void osk_hide(void) {
    if (!visible) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    visible = false;
    xSemaphoreGive(lock);
    term_reserve_bottom(0);
    term_refresh_status();
}

void osk_toggle(void) {
    if (visible) osk_hide();
    else osk_show();
}

bool osk_visible(void) {
    return visible;
}

bool osk_touch(int x, int y) {
    if (!visible || y < TOP) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    int row = (y - TOP - 2) / ROW_H;
    if (row < 0) row = 0;
    if (row > 3) row = 3;
    int x_half = 0;
    for (int i = 0; layer[row][i].label != NULL; i++) {
        const Key* key = &layer[row][i];
        if (x < (x_half + key->width) * HALF_W) {
            // Press feedback: click and highlight
            sound_play(SOUND_KEY);
            draw_key(row, x_half, key, true);
            display_present_rect(x_half * HALF_W, TOP + 2 + row * ROW_H - 1, key->width * HALF_W, ROW_H, canvas.px);

            if (key->action == A_SHIFT) {
                shift = !shift;
                draw_all();
            } else if (key->action == A_LETTERS || key->action == A_SYMBOLS || key->action == A_MORE) {
                layer = key->action == A_LETTERS ? letters : (key->action == A_SYMBOLS ? symbols : more);
                shift = false;
                draw_all();
            } else if (key->out != NULL) {
                char out[8];
                strlcpy(out, key->out, sizeof(out));
                if (layer == letters && shift && strlen(out) == 1 && out[0] >= 'a' && out[0] <= 'z') {
                    out[0] -= 32;
                    shift = false; // one-shot
                    vTaskDelay(pdMS_TO_TICKS(60));
                    draw_all();
                } else {
                    vTaskDelay(pdMS_TO_TICKS(60));
                    draw_key(row, x_half, key, false);
                    display_present_rect(x_half * HALF_W, TOP + 2 + row * ROW_H - 1, key->width * HALF_W, ROW_H, canvas.px);
                }
                input_push(out, strlen(out));
            }
            break;
        }
        x_half += key->width;
    }
    xSemaphoreGive(lock);
    return true;
}

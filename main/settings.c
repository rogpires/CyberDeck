#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "display.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input.h"
#include "power.h"
#include "sound.h"
#include "term.h"
#include "ui_draw.h"

#define FB_W DISPLAY_WIDTH
#define FB_H DISPLAY_HEIGHT
#define ROW_TOP 40
#define ROW_H   40
#define CHIP_W  26

#define BG      0x0A0014
#define ROW_BG  0x1A1028
#define ROW_SEL 0x3A1C66
#define ACCENT  0xBF00FF
#define LABEL   0xF2F2F7
#define VALUE   0xFFFFFF
#define DIM     0x8A8594

// Each setting: name, how to read its value as text, and how to change it by one step
typedef struct {
    const char* name;
    void (*format)(char* out, size_t size);
    void (*step)(int delta);
} Setting;

// Brightness: 5..100 in steps of 15 (snapped)
static void brightness_fmt(char* out, size_t size) { snprintf(out, size, "%d%%", display_brightness()); }
static void brightness_step(int delta) {
    int v = display_brightness() + delta * 15;
    display_set_brightness(v < 5 ? 5 : (v > 100 ? 100 : v));
}

// Volume: 0..100 in steps of 10
static void volume_fmt(char* out, size_t size) {
    int v = sound_volume();
    if (v == 0) snprintf(out, size, "mudo");
    else snprintf(out, size, "%d%%", v);
}
static void volume_step(int delta) {
    int v = sound_volume() + delta * 10;
    sound_set_volume(v < 0 ? 0 : (v > 100 ? 100 : v));
    if (sound_volume() > 0) sound_play(SOUND_NAV); // hear the new level
}

// Auto-off: a list of minute options
static const int autooff_options[] = {0, 1, 2, 5, 10, 15, 30, 60};
#define AUTOOFF_COUNT (sizeof(autooff_options) / sizeof(autooff_options[0]))
static void autooff_fmt(char* out, size_t size) {
    int m = power_autooff();
    if (m <= 0) snprintf(out, size, "nunca");
    else snprintf(out, size, "%d min", m);
}
static void autooff_step(int delta) {
    int current = power_autooff(), index = 0;
    for (size_t i = 0; i < AUTOOFF_COUNT; i++) {
        if (autooff_options[i] == current) index = i;
    }
    index += delta;
    if (index < 0) index = 0;
    if (index >= (int)AUTOOFF_COUNT) index = AUTOOFF_COUNT - 1;
    power_set_autooff(autooff_options[index]);
}

// Font: cycles the four sizes
static const char* font_options[] = {"5x8", "6x12", "8x16", "12x24"};
#define FONT_OPTION_COUNT (sizeof(font_options) / sizeof(font_options[0]))
static char current_font[8] = "8x16";
static void font_fmt(char* out, size_t size) { strlcpy(out, current_font, size); }
static void font_step(int delta) {
    int index = 2; // 8x16 default
    for (size_t i = 0; i < FONT_OPTION_COUNT; i++) {
        if (strcmp(font_options[i], current_font) == 0) index = i;
    }
    index += delta;
    if (index < 0) index = 0;
    if (index >= (int)FONT_OPTION_COUNT) index = FONT_OPTION_COUNT - 1;
    strlcpy(current_font, font_options[index], sizeof(current_font));
    term_set_font(current_font); // also saves to NVS
}

static const Setting settings[] = {
    {"Brilho", brightness_fmt, brightness_step},
    {"Volume", volume_fmt, volume_step},
    {"Desligar sozinho", autooff_fmt, autooff_step},
    {"Fonte", font_fmt, font_step},
};
#define SETTING_COUNT (sizeof(settings) / sizeof(settings[0]))

static uint16_t* fb;

static void draw(int selected) {
    Canvas c = {fb, FB_W, FB_H};
    ui_fill(&c, BG);
    ui_back(&c, 6, 2, ACCENT);
    ui_status(&c, FB_W, 2);
    ui_text(&c, FB_W / 2 - 4 * 12, 18, "AJUSTES", &fonts[3], LABEL, false);

    for (size_t i = 0; i < SETTING_COUNT; i++) {
        int y = ROW_TOP + i * ROW_H;
        bool sel = (int)i == selected;
        ui_round_rect(&c, 10, y, FB_W - 20, ROW_H - 6, 8.0f, sel ? ROW_SEL : ROW_BG);
        ui_text(&c, 22, y + (ROW_H - 6 - 16) / 2, settings[i].name, &fonts[2], sel ? VALUE : LABEL, false);

        char value[16];
        settings[i].format(value, sizeof(value));
        int vw = strlen(value) * fonts[2].width;
        int right = FB_W - 30;
        int cy = y + (ROW_H - 6 - 16) / 2;
        // "<  value  >" with tappable chips
        ui_text(&c, right - vw, cy, value, &fonts[2], VALUE, false);
        ui_text(&c, right - vw - CHIP_W, cy, "-", &fonts[3], sel ? ACCENT : DIM, false);
        ui_text(&c, right + 10, cy, "+", &fonts[3], sel ? ACCENT : DIM, false);
    }

    ui_text(&c, 10, FB_H - 16, "setas movem/mudam  -  toque +/-  -  MENU volta", &fonts[1], DIM, false);
    display_present(0, FB_H, fb);
}

void settings_open(void) {
    if (fb == NULL) fb = heap_caps_malloc(FB_W * FB_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    // Remember the current font name for the cycler
    char names[64];
    term_list_fonts(names, sizeof(names));
    char* star = strchr(names, '*');
    if (star != NULL) {
        char* start = star;
        while (start > names && *(start - 1) != ' ') start--;
        int len = star - start;
        if (len > 0 && len < (int)sizeof(current_font)) {
            memcpy(current_font, start, len);
            current_font[len] = 0;
        }
    }

    term_pause(true);
    term_hide_bar(true);
    int selected = 0;
    draw(selected);

    bool run = true;
    while (run) {
        InputEvent event;
        input_wait(&event, UINT32_MAX);
        switch (event.type) {
            case EV_BACK:
                run = false;
                break;
            case EV_TAP: {
                if (event.y < ROW_TOP) break; // header
                int row = (event.y - ROW_TOP) / ROW_H;
                if (row < 0 || row >= (int)SETTING_COUNT) break;
                selected = row;
                // Right third = +, left third of the value area = -, elsewhere just selects
                if (event.x > FB_W - 34) settings[row].step(+1);
                else if (event.x > FB_W - 34 - 90) settings[row].step(-1);
                draw(selected);
                break;
            }
            case EV_KEY: {
                char c = event.key;
                if (c == 0x1b) {
                    char next;
                    if (!input_read(&next, 40)) { run = false; break; } // lone Esc
                    if (next != '[' && next != 'O') break;
                    char code;
                    if (!input_read(&code, 40)) break;
                    if (code == 'A' && selected > 0) selected--;
                    else if (code == 'B' && selected < (int)SETTING_COUNT - 1) selected++;
                    else if (code == 'C') settings[selected].step(+1);
                    else if (code == 'D') settings[selected].step(-1);
                    draw(selected);
                }
                break;
            }
            default:
                break;
        }
    }

    term_hide_bar(false);
    term_pause(false);
}

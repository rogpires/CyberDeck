#include "game_enduro.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input.h"
#include "sound.h"
#include "term.h"
#include "touch.h"
#include "ui_draw.h"

#define W DISPLAY_WIDTH
#define H DISPLAY_HEIGHT
#define HORIZON 74
#define ROAD_TOP_HALF 16.0f
#define ROAD_BOT_HALF 150.0f
#define CENTER 160
#define FRAME_MS 33
#define MAX_CARS 6
#define PLAYER_Y (H - 34)

static uint16_t* fb;

typedef struct { float lane; float z; bool active; uint8_t color; } Car; // lane -1..1, z 0 far .. 1 near

static Car cars[MAX_CARS];
static float player_x;      // screen x of the player car centre
static float speed;         // 0..1
static float scroll;        // road stripe phase
static int remaining, day, passed_total;
static int flash;           // crash flash frames

static const uint32_t car_colors[] = {0xF2334F, 0x22D3EE, 0xFACC15, 0xFF9E3D, 0xA855F7, 0xFFFFFF};

static float road_half(float t) { return ROAD_TOP_HALF + (ROAD_BOT_HALF - ROAD_TOP_HALF) * t; }

static void fill(int x, int y, int w, int h, uint16_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int yy = 0; yy < h; yy++) {
        uint16_t* row = &fb[(y + yy) * W + x];
        for (int xx = 0; xx < w; xx++) row[xx] = c;
    }
}

// Day/night sky and grass colors cycle with `day`
static void day_colors(uint16_t* sky, uint16_t* grass, uint16_t* road) {
    int phase = day % 4;
    uint32_t s, g, r;
    switch (phase) {
        case 0: s = 0x1E6FD6; g = 0x1E8A3B; r = 0x4A4A55; break; // day
        case 1: s = 0xE0683A; g = 0x6B5A2A; r = 0x55504A; break; // dusk
        case 2: s = 0x0A0A2A; g = 0x0E2A18; r = 0x2A2A35; break; // night
        default: s = 0x8FA8C8; g = 0x3A7A4B; r = 0x4A4A55; break; // dawn
    }
    *sky = display_color(s >> 16 & 0xFF, s >> 8 & 0xFF, s & 0xFF);
    *grass = display_color(g >> 16 & 0xFF, g >> 8 & 0xFF, g & 0xFF);
    *road = display_color(r >> 16 & 0xFF, r >> 8 & 0xFF, r & 0xFF);
}

static void draw_car(int cx, int cy, int w, int h, uint32_t color) {
    uint16_t body = display_color(color >> 16 & 0xFF, color >> 8 & 0xFF, color & 0xFF);
    uint16_t dark = display_color((color >> 16 & 0xFF) / 2, (color >> 8 & 0xFF) / 2, (color & 0xFF) / 2);
    fill(cx - w / 2, cy - h / 2, w, h, body);
    fill(cx - w / 2 - 2, cy - h / 2 + 2, 3, h - 4, dark);   // wheels
    fill(cx + w / 2 - 1, cy - h / 2 + 2, 3, h - 4, dark);
    fill(cx - w / 4, cy - h / 4, w / 2, h / 3, dark);       // windshield
}

static void render(void) {
    uint16_t sky, grass, road;
    day_colors(&sky, &grass, &road);
    uint16_t rumble1 = display_color(0xE0, 0x1E, 0x2E), rumble2 = display_color(0xF2, 0xF2, 0xF2);

    fill(0, 0, W, HORIZON, sky);
    // Road scanlines with perspective + scrolling rumble stripes
    for (int y = HORIZON; y < H; y++) {
        float t = (float)(y - HORIZON) / (H - HORIZON);
        int half = (int)road_half(t);
        int left = CENTER - half, right = CENTER + half;
        // grass
        for (int x = 0; x < left; x++) fb[y * W + x] = grass;
        for (int x = right; x < W; x++) fb[y * W + x] = grass;
        // road
        for (int x = left; x < right; x++) fb[y * W + x] = road;
        // rumble stripes at the edges, striped by depth
        float depth = scroll + t * 8;
        uint16_t edge = ((int)(depth * 2) & 1) ? rumble1 : rumble2;
        int ew = 3 + (int)(t * 6);
        for (int x = left; x < left + ew && x < W; x++) if (x >= 0) fb[y * W + x] = edge;
        for (int x = right - ew; x < right; x++) if (x >= 0 && x < W) fb[y * W + x] = edge;
        // centre dashes
        if (((int)(depth * 2) & 1) && half > 6) {
            int cw = 1 + (int)(t * 4);
            for (int x = CENTER - cw; x <= CENTER + cw; x++) if (x >= 0 && x < W) fb[y * W + x] = rumble2;
        }
    }

    // Enemy cars (far to near)
    for (int i = 0; i < MAX_CARS; i++) {
        if (!cars[i].active) continue;
        float t = cars[i].z;
        int y = HORIZON + (int)(t * (H - HORIZON));
        int cx = CENTER + (int)(cars[i].lane * road_half(t) * 0.8f);
        int cw = 8 + (int)(t * 34), ch = 6 + (int)(t * 26);
        draw_car(cx, y, cw, ch, car_colors[cars[i].color]);
    }

    // Player car
    if (!(flash && (flash & 2))) draw_car((int)player_x, PLAYER_Y, 30, 22, 0x4ADE80);

    // HUD
    Canvas c = {fb, W, H};
    char buf[24];
    snprintf(buf, sizeof(buf), "CARROS %d", remaining);
    ui_text(&c, 6, 4, buf, &fonts[2], 0xFFFFFF, true);
    snprintf(buf, sizeof(buf), "DIA %d", day);
    ui_text(&c, W - 8 - (int)strlen(buf) * 8, 4, buf, &fonts[2], 0xFFFFFF, true);
    // speed bar
    int sw = (int)(speed * 60);
    fill(W / 2 - 30, 10, 60, 4, display_color(0x30, 0x30, 0x3A));
    fill(W / 2 - 30, 10, sw, 4, display_color(0x4A, 0xDE, 0x80));

    display_present(0, H, fb);
}

static bool controls(void) {
    InputEvent e;
    while (input_wait(&e, 0)) {
        if (e.type == EV_BACK) return false;
        if (e.type == EV_KEY) {
            char ch = e.key;
            if (ch == 'q') return false;
            else if (ch == 0x1b) {
                char n;
                if (!input_read(&n, 20)) return false;
                if (n == '[' || n == 'O') {
                    char code;
                    if (input_read(&code, 20)) {
                        if (code == 'C') player_x += 12;
                        else if (code == 'D') player_x -= 12;
                        else if (code == 'A') speed += 0.08f;   // up = accelerate
                        else if (code == 'B') speed -= 0.12f;   // down = brake
                    }
                }
            }
        }
    }
    int tx, ty;
    if (touch_read(&tx, &ty)) {
        if (ty < 28 && tx < 60) return false;
        // steer toward the finger, and accelerate while touching
        if (tx < (int)player_x - 4) player_x -= 8;
        else if (tx > (int)player_x + 4) player_x += 8;
        speed += 0.05f;
    } else {
        speed -= 0.02f; // coast
    }
    if (speed < 0.15f) speed = 0.15f;
    if (speed > 1.0f) speed = 1.0f;
    return true;
}

static void spawn_car(void) {
    for (int i = 0; i < MAX_CARS; i++) {
        if (cars[i].active) continue;
        cars[i].active = true;
        cars[i].z = 0.02f;
        cars[i].lane = ((int)(esp_random() % 160) - 80) / 100.0f; // -0.8..0.8
        cars[i].color = esp_random() % (sizeof(car_colors) / sizeof(car_colors[0]));
        return;
    }
}

void game_enduro(void) {
    if (fb == NULL) fb = heap_caps_malloc(W * H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (fb == NULL) return;

    term_pause(true);
    term_hide_bar(true);
    touch_set_raw(true);

    bool quit = false;
    // intro
    {
        Canvas c = {fb, W, H};
        ui_fill(&c, 0x05010A);
        ui_text(&c, (W - 6 * 12) / 2, H / 2 - 30, "ENDURO", &fonts[3], 0xFFFFFF, true);
        ui_text(&c, (W - 34 * 6) / 2, H / 2 + 6, "seta vira/acelera  -  dedo tambem", &fonts[1], 0x8A8594, false);
        ui_text(&c, (W - 22 * 6) / 2, H / 2 + 22, "ultrapasse os carros!", &fonts[1], 0x8A8594, false);
        display_present(0, H, fb);
        input_flush();
        InputEvent e;
        bool go = false;
        while (!go) {
            if (input_wait(&e, 50)) {
                if (e.type == EV_BACK || (e.type == EV_KEY && (e.key == 'q' || e.key == 0x1b))) { quit = true; go = true; }
                else if (e.type == EV_KEY || e.type == EV_TAP) go = true;
            }
            int x, y;
            if (touch_read(&x, &y)) { vTaskDelay(pdMS_TO_TICKS(150)); go = true; }
        }
    }

    player_x = CENTER;
    speed = 0.3f;
    scroll = 0;
    day = 1;
    remaining = 15;
    passed_total = 0;
    flash = 0;
    for (int i = 0; i < MAX_CARS; i++) cars[i].active = false;

    int tick = 0;
    int64_t next = esp_timer_get_time();
    while (!quit) {
        if (!controls()) break;

        scroll += speed * 0.6f;
        int half_bottom = (int)road_half(1.0f);
        if (player_x < CENTER - half_bottom + 14) player_x = CENTER - half_bottom + 14;
        if (player_x > CENTER + half_bottom - 14) player_x = CENTER + half_bottom - 14;

        // advance enemy cars toward us by our speed
        for (int i = 0; i < MAX_CARS; i++) {
            if (!cars[i].active) continue;
            cars[i].z += (0.004f + speed * 0.020f);
            if (cars[i].z >= 1.05f) {
                cars[i].active = false;
                remaining--;
                passed_total++;
                sound_play(SOUND_NAV);
            } else if (cars[i].z > 0.82f && !flash) {
                int cx = CENTER + (int)(cars[i].lane * road_half(cars[i].z) * 0.8f);
                if (abs(cx - (int)player_x) < 24) {
                    flash = 16;             // crash: flash + slow down
                    speed = 0.15f;
                    cars[i].active = false;
                    sound_play(SOUND_ERROR);
                }
            }
        }
        if (flash) flash--;

        // spawn
        int active = 0;
        for (int i = 0; i < MAX_CARS; i++) active += cars[i].active;
        if (active < 3 && (tick % 35 == 0)) spawn_car();

        render();

        if (remaining <= 0) {
            day++;
            remaining = 15 + day * 3;
            char sub[32];
            snprintf(sub, sizeof(sub), "dia %d  -  %d no total", day, passed_total);
            Canvas c = {fb, W, H};
            ui_fill(&c, 0x05010A);
            ui_text(&c, (W - 9 * 12) / 2, H / 2 - 24, "DIA LIMPO", &fonts[3], 0x4ADE80, true);
            ui_text(&c, (W - (int)strlen(sub) * 6) / 2, H / 2 + 8, sub, &fonts[1], 0x8A8594, false);
            display_present(0, H, fb);
            vTaskDelay(pdMS_TO_TICKS(1500));
            input_flush();
        }

        tick++;
        next += FRAME_MS * 1000;
        int64_t now = esp_timer_get_time();
        if (next > now) vTaskDelay(pdMS_TO_TICKS((next - now) / 1000));
        else next = now;
    }

    touch_set_raw(false);
    term_hide_bar(false);
    term_pause(false);
}

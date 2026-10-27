#include "game_invaders.h"

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

#define COLS 8
#define ROWS 5
#define INV_W 20
#define INV_H 14
#define GAP_X 8
#define GAP_Y 8
#define GRID_LEFT 24
#define GRID_TOP 30
#define SHIP_W 26
#define SHIP_H 10
#define SHIP_Y (H - 22)
#define MAX_BOMBS 4
#define FRAME_MS 33

#define C_BG     0x05010A
#define C_SHIP   0x4ADE80
#define C_BULLET 0xFFFFFF
#define C_BOMB   0xF2334F
#define C_TEXT   0xF2F2F7
#define C_DIM    0x8A8594

// Two invader frames (11x8), animated by toggling
static const char* const INV_A[8] = {
    "..X.....X..", "...X...X...", "..XXXXXXX..", ".XX.XXX.XX.",
    "XXXXXXXXXXX", "X.XXXXXXX.X", "X.X.....X.X", "...XX.XX...",
};
static const char* const INV_B[8] = {
    "..X.....X..", "X..X...X..X", "X.XXXXXXX.X", "XXX.XXX.XXX",
    "XXXXXXXXXXX", ".X.XXXXX.X.", "..X.....X..", ".X.......X.",
};

static uint16_t* fb;
static uint16_t color_ship, color_bullet, color_bomb, color_bg;

typedef struct { float x, y; bool alive; } Entity;

static Entity invaders[ROWS][COLS];
static int alive_count;
static float grid_x, grid_y, grid_dir;
static Entity bullet;       // player shot (alive = in flight)
static Entity bombs[MAX_BOMBS];
static float ship_x;
static int score, hiscore, lives, wave;
static bool anim_frame;

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

// Draw an 11x8 invader scaled to INV_W x INV_H
static void draw_invader(int px, int py, bool frame_b, uint16_t c) {
    const char* const* sprite = frame_b ? INV_B : INV_A;
    for (int ry = 0; ry < 8; ry++) {
        for (int rx = 0; rx < 11; rx++) {
            if (sprite[ry][rx] != 'X') continue;
            int x = px + rx * INV_W / 11, y = py + ry * INV_H / 8;
            int w = INV_W / 11 + 1, h = INV_H / 8 + 1;
            fill(x, y, w, h, c);
        }
    }
}

static uint16_t row_color(int row) {
    static const uint32_t palette[ROWS] = {0xF2334F, 0xFF9E3D, 0xFACC15, 0x4ADE80, 0x22D3EE};
    uint32_t v = palette[row];
    return display_color(v >> 16 & 0xFF, v >> 8 & 0xFF, v & 0xFF);
}

static void new_wave(void) {
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) invaders[r][c] = (Entity){0, 0, true};
    }
    alive_count = ROWS * COLS;
    grid_x = GRID_LEFT;
    grid_y = GRID_TOP;
    grid_dir = 1;
    bullet.alive = false;
    for (int i = 0; i < MAX_BOMBS; i++) bombs[i].alive = false;
    anim_frame = false;
}

static void reset_game(void) {
    score = 0;
    lives = 3;
    wave = 1;
    ship_x = (W - SHIP_W) / 2;
    new_wave();
}

static void render(void) {
    Canvas c = {fb, W, H};
    ui_fill(&c, C_BG);

    // HUD
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d", score);
    ui_text(&c, 6, 4, buf, &fonts[2], C_TEXT, false);
    snprintf(buf, sizeof(buf), "HI %04d", hiscore);
    ui_text(&c, W / 2 - 28, 4, buf, &fonts[2], C_DIM, false);
    for (int i = 0; i < lives; i++) fill(W - 8 - (i + 1) * 16, 6, 12, 6, color_ship);

    // Invaders
    int cell_x = INV_W + GAP_X, cell_y = INV_H + GAP_Y;
    for (int r = 0; r < ROWS; r++) {
        for (int col = 0; col < COLS; col++) {
            if (!invaders[r][col].alive) continue;
            int x = (int)grid_x + col * cell_x, y = (int)grid_y + r * cell_y;
            draw_invader(x, y, anim_frame, row_color(r));
        }
    }

    // Bombs, bullet, ship
    for (int i = 0; i < MAX_BOMBS; i++) {
        if (bombs[i].alive) fill((int)bombs[i].x, (int)bombs[i].y, 3, 8, color_bomb);
    }
    if (bullet.alive) fill((int)bullet.x, (int)bullet.y, 3, 9, color_bullet);
    // Ship: body + cannon
    fill((int)ship_x, SHIP_Y + 4, SHIP_W, SHIP_H - 4, color_ship);
    fill((int)ship_x + SHIP_W / 2 - 2, SHIP_Y, 4, 5, color_ship);

    display_present(0, H, fb);
}

// Returns false to quit the game
static bool read_controls(bool* fire) {
    *fire = false;
    InputEvent e;
    while (input_wait(&e, 0)) {
        if (e.type == EV_BACK) return false;
        if (e.type == EV_KEY) {
            char ch = e.key;
            if (ch == ' ') *fire = true;
            else if (ch == 'q') return false;
            else if (ch == 0x1b) {
                char n;
                if (!input_read(&n, 20)) return false; // lone Esc quits
                if (n == '[' || n == 'O') {
                    char code;
                    if (input_read(&code, 20)) {
                        if (code == 'C') ship_x += 16;      // right arrow
                        else if (code == 'D') ship_x -= 16; // left arrow
                    }
                }
            }
        }
    }
    // Touch: the ship follows the finger; a new touch also fires
    int tx, ty;
    static bool was_touch;
    if (touch_read(&tx, &ty)) {
        if (ty < 28 && tx < 60) return false; // top-left corner quits
        ship_x = tx - SHIP_W / 2;
        if (!was_touch) *fire = true;
        was_touch = true;
    } else {
        was_touch = false;
    }
    return true;
}

static void fire_bullet(void) {
    if (bullet.alive) return;
    bullet.x = ship_x + SHIP_W / 2 - 1;
    bullet.y = SHIP_Y - 9;
    bullet.alive = true;
    sound_play(SOUND_KEY);
}

// Move the invader block; returns true if it reached the ship (game over)
static bool step_invaders(void) {
    int cell_x = INV_W + GAP_X, cell_y = INV_H + GAP_Y;
    // Speed: faster with fewer invaders
    float speed = 2.0f + (ROWS * COLS - alive_count) * 0.20f + (wave - 1) * 1.5f;
    grid_x += grid_dir * speed;

    int min_col = COLS, max_col = -1, max_row = -1;
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            if (!invaders[r][c].alive) continue;
            if (c < min_col) min_col = c;
            if (c > max_col) max_col = c;
            if (r > max_row) max_row = r;
        }
    }
    if (max_col < 0) return false;
    int left = (int)grid_x + min_col * cell_x;
    int right = (int)grid_x + max_col * cell_x + INV_W;
    if (left <= 4 || right >= W - 4) {
        grid_dir = -grid_dir;
        grid_x += grid_dir * speed;
        grid_y += 10;
    }
    anim_frame = !anim_frame;
    return (int)grid_y + max_row * cell_y + INV_H >= SHIP_Y;
}

static void drop_bomb(void) {
    // A random alive invader in the lowest row of its column drops a bomb
    int cell_x = INV_W + GAP_X, cell_y = INV_H + GAP_Y;
    int col = esp_random() % COLS;
    for (int r = ROWS - 1; r >= 0; r--) {
        if (invaders[r][col].alive) {
            for (int i = 0; i < MAX_BOMBS; i++) {
                if (!bombs[i].alive) {
                    bombs[i].x = grid_x + col * cell_x + INV_W / 2;
                    bombs[i].y = grid_y + r * cell_y + INV_H;
                    bombs[i].alive = true;
                    return;
                }
            }
            return;
        }
    }
}

static bool overlap(float ax, float ay, int aw, int ah, float bx, float by, int bw, int bh) {
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

// Big centered message; waits for a key/tap, returns false to quit
static bool message(const char* line1, const char* line2) {
    Canvas c = {fb, W, H};
    ui_fill(&c, C_BG);
    ui_text(&c, (W - (int)strlen(line1) * 12) / 2, H / 2 - 24, line1, &fonts[3], 0xFFFFFF, true);
    if (line2) ui_text(&c, (W - (int)strlen(line2) * 8) / 2, H / 2 + 6, line2, &fonts[2], C_DIM, false);
    display_present(0, H, fb);
    input_flush();
    while (true) {
        InputEvent e;
        if (input_wait(&e, 50)) {
            if (e.type == EV_BACK) return false;
            if (e.type == EV_KEY) return e.key != 'q' && e.key != 0x1b;
            if (e.type == EV_TAP) return true;
        }
        int x, y;
        if (touch_read(&x, &y)) { vTaskDelay(pdMS_TO_TICKS(150)); return true; }
    }
}

void game_invaders(void) {
    if (fb == NULL) fb = heap_caps_malloc(W * H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (fb == NULL) return;
    color_ship = display_color(C_SHIP >> 16 & 0xFF, C_SHIP >> 8 & 0xFF, C_SHIP & 0xFF);
    color_bullet = display_color(0xFF, 0xFF, 0xFF);
    color_bomb = display_color(C_BOMB >> 16 & 0xFF, C_BOMB >> 8 & 0xFF, C_BOMB & 0xFF);
    color_bg = display_color(C_BG >> 16 & 0xFF, C_BG >> 8 & 0xFF, C_BG & 0xFF);

    term_pause(true);
    term_hide_bar(true);
    touch_set_raw(true);

    bool quit = !message("SPACE INVADERS", "seta=mover  espaco=atira  (toque: dedo)");
    reset_game();

    int tick = 0;
    int64_t next = esp_timer_get_time();
    while (!quit) {
        bool fire;
        if (!read_controls(&fire)) break;
        if (ship_x < 0) ship_x = 0;
        if (ship_x > W - SHIP_W) ship_x = W - SHIP_W;
        if (fire) fire_bullet();

        // Bullet up
        if (bullet.alive) {
            bullet.y -= 7;
            if (bullet.y < -9) bullet.alive = false;
        }
        // Bullet vs invaders
        if (bullet.alive) {
            int cell_x = INV_W + GAP_X, cell_y = INV_H + GAP_Y;
            for (int r = 0; r < ROWS && bullet.alive; r++) {
                for (int col = 0; col < COLS; col++) {
                    if (!invaders[r][col].alive) continue;
                    int ix = (int)grid_x + col * cell_x, iy = (int)grid_y + r * cell_y;
                    if (overlap(bullet.x, bullet.y, 3, 9, ix, iy, INV_W, INV_H)) {
                        invaders[r][col].alive = false;
                        bullet.alive = false;
                        alive_count--;
                        score += (ROWS - r) * 10;
                        if (score > hiscore) hiscore = score;
                        sound_play(SOUND_NAV);
                        break;
                    }
                }
            }
        }

        // Invaders step every few frames (faster when fewer remain)
        int step_every = alive_count > 20 ? 12 : (alive_count > 8 ? 7 : 3);
        bool reached = false;
        if (tick % step_every == 0) reached = step_invaders();
        if (tick % 45 == 0) drop_bomb();

        // Bombs down + vs ship
        for (int i = 0; i < MAX_BOMBS; i++) {
            if (!bombs[i].alive) continue;
            bombs[i].y += 4;
            if (bombs[i].y > H) bombs[i].alive = false;
            else if (overlap(bombs[i].x, bombs[i].y, 3, 8, ship_x, SHIP_Y, SHIP_W, SHIP_H)) {
                bombs[i].alive = false;
                lives--;
                sound_play(SOUND_ERROR);
                if (lives <= 0) reached = true;
            }
        }

        render();

        if (reached) {
            if (!message("GAME OVER", "tecla/toque joga de novo, MENU sai")) break;
            reset_game();
        } else if (alive_count == 0) {
            wave++;
            score += 100;
            if (score > hiscore) hiscore = score;
            if (!message("WAVE LIMPA!", "proxima onda...")) break;
            new_wave();
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

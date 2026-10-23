#include "term.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "esp_heap_caps.h"
#include "fonts.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "osk.h"
#include "power.h"
#include "sound.h"
#include "ui_draw.h"

#define MAX_CELL_H 24
#define BAR_H      UI_HEADER_H
#define BAR_MS     1000
// Text area: below the header, above the on-screen keyboard when it is shown
#define AREA_H     (DISPLAY_HEIGHT - BAR_H - reserved_bottom)
#define DEFAULT_FG 16
#define DEFAULT_BG 17
#define MAX_PARAMS 8
#define BLINK_MS 500
#define FRAME_MS 20

typedef struct {
    uint16_t glyph; // index into glyph_codepoints
    uint8_t fg;
    uint8_t bg;
} Cell;

typedef enum { STATE_NORMAL, STATE_ESC, STATE_CSI } ParseState;

static Cell cells[TERM_MAX_ROWS][TERM_MAX_COLS];
static bool dirty[TERM_MAX_ROWS];
static const Font* font = &fonts[2];
static int cols, rows;
static int cur_x, cur_y, saved_x, saved_y;
static bool wrap_pending;
static bool cursor_visible = true;
static uint8_t attr_fg = DEFAULT_FG, attr_bg = DEFAULT_BG;
static bool attr_bold, attr_reverse;
static ParseState state;
static uint32_t utf8_codepoint;
static int utf8_remaining;
static int params[MAX_PARAMS];
static int param_count;
static bool csi_private;
static SemaphoreHandle_t lock;
static uint16_t palette[18];
static uint16_t* row_buffer;
static int reserved_bottom; // pixels at the bottom used by the on-screen keyboard
// Lines that scrolled off the top, oldest first in a ring buffer (in PSRAM)
#define SCROLLBACK_LINES 300
static Cell (*scrollback)[TERM_MAX_COLS];
static int scrollback_count;
static int scrollback_head; // index of the oldest line
static int view_offset;     // 0 shows the live screen, N shows N lines further back
static volatile bool bar_dirty = true;
static volatile bool paused = false;
static volatile bool bar_hidden = false;

static void init_palette(void) {
    static const uint8_t rgb[18][3] = {
        {0x00, 0x00, 0x00}, {0xCD, 0x31, 0x31}, {0x0D, 0xBC, 0x79}, {0xE5, 0xE5, 0x10},
        {0x24, 0x72, 0xC8}, {0xBC, 0x3F, 0xBC}, {0x11, 0xA8, 0xCD}, {0xCC, 0xCC, 0xCC},
        {0x66, 0x66, 0x66}, {0xF1, 0x4C, 0x4C}, {0x23, 0xD1, 0x8B}, {0xF5, 0xF5, 0x43},
        {0x3B, 0x8E, 0xEA}, {0xD6, 0x70, 0xD6}, {0x29, 0xB8, 0xDB}, {0xFF, 0xFF, 0xFF},
        {0x5C, 0xFF, 0x9D}, // default foreground: phosphor green
        {0x00, 0x00, 0x00}, // default background
    };
    for (int i = 0; i < 18; i++) {
        palette[i] = display_color(rgb[i][0], rgb[i][1], rgb[i][2]);
    }
}

static void mark_all_dirty(void) {
    for (int y = 0; y < rows; y++) dirty[y] = true;
}

static void clear_cells(int row, int from, int to) {
    for (int x = from; x < to; x++) {
        cells[row][x] = (Cell) {0, attr_fg, attr_bg};
    }
    dirty[row] = true;
}

static void scroll_up(void) {
    if (scrollback != NULL) {
        int slot = (scrollback_head + scrollback_count) % SCROLLBACK_LINES;
        if (scrollback_count == SCROLLBACK_LINES) {
            scrollback_head = (scrollback_head + 1) % SCROLLBACK_LINES;
        } else {
            scrollback_count++;
        }
        memcpy(scrollback[slot], cells[0], sizeof(cells[0]));
    }
    memmove(&cells[0], &cells[1], sizeof(cells[0]) * (rows - 1));
    clear_cells(rows - 1, 0, cols);
    mark_all_dirty();
}

static void newline(void) {
    dirty[cur_y] = true;
    if (cur_y == rows - 1) {
        scroll_up();
    } else {
        cur_y++;
    }
    dirty[cur_y] = true;
}

static void move_cursor(int x, int y) {
    dirty[cur_y] = true;
    cur_x = x < 0 ? 0 : (x >= cols ? cols - 1 : x);
    cur_y = y < 0 ? 0 : (y >= rows ? rows - 1 : y);
    wrap_pending = false;
    dirty[cur_y] = true;
}

static uint16_t glyph_for(uint32_t codepoint) {
    if (codepoint >= 0x20 && codepoint < 0x7F) return codepoint - 0x20;
    int low = 0, high = GLYPH_COUNT - 1;
    while (low <= high) {
        int mid = (low + high) / 2;
        if (glyph_codepoints[mid] == codepoint) return mid;
        if (glyph_codepoints[mid] < codepoint) low = mid + 1;
        else high = mid - 1;
    }
    return GLYPH_UNKNOWN;
}

static void put_char(uint16_t glyph) {
    if (wrap_pending) {
        cur_x = 0;
        newline();
        wrap_pending = false;
    }
    uint8_t fg = attr_fg, bg = attr_bg;
    if (attr_bold && fg < 8) fg += 8;
    if (attr_reverse) {
        uint8_t t = fg; fg = bg; bg = t;
    }
    cells[cur_y][cur_x] = (Cell) {glyph, fg, bg};
    dirty[cur_y] = true;
    if (cur_x == cols - 1) {
        wrap_pending = true;
    } else {
        cur_x++;
    }
}

static int param(int index, int fallback) {
    return (index < param_count && params[index] > 0) ? params[index] : fallback;
}

static void apply_sgr(void) {
    if (param_count == 0) param_count = 1, params[0] = 0;
    for (int i = 0; i < param_count; i++) {
        int p = params[i];
        if (p == 0) {
            attr_fg = DEFAULT_FG; attr_bg = DEFAULT_BG; attr_bold = false; attr_reverse = false;
        } else if (p == 1) {
            attr_bold = true;
        } else if (p == 22) {
            attr_bold = false;
        } else if (p == 7) {
            attr_reverse = true;
        } else if (p == 27) {
            attr_reverse = false;
        } else if (p >= 30 && p <= 37) {
            attr_fg = p - 30;
        } else if (p == 39) {
            attr_fg = DEFAULT_FG;
        } else if (p >= 40 && p <= 47) {
            attr_bg = p - 40;
        } else if (p == 49) {
            attr_bg = DEFAULT_BG;
        } else if (p >= 90 && p <= 97) {
            attr_fg = p - 90 + 8;
        } else if (p >= 100 && p <= 107) {
            attr_bg = p - 100 + 8;
        } else if ((p == 38 || p == 48) && i + 1 < param_count) {
            // 256-color/truecolor: not supported, skip its arguments
            i += (params[i + 1] == 5) ? 2 : 4;
        }
    }
}

static void handle_csi(char final) {
    switch (final) {
        case 'm': apply_sgr(); break;
        case 'A': move_cursor(cur_x, cur_y - param(0, 1)); break;
        case 'B': move_cursor(cur_x, cur_y + param(0, 1)); break;
        case 'C': move_cursor(cur_x + param(0, 1), cur_y); break;
        case 'D': move_cursor(cur_x - param(0, 1), cur_y); break;
        case 'G': move_cursor(param(0, 1) - 1, cur_y); break;
        case 'd': move_cursor(cur_x, param(0, 1) - 1); break;
        case 'H':
        case 'f': move_cursor(param(1, 1) - 1, param(0, 1) - 1); break;
        case 'K': {
            int mode = param_count ? params[0] : 0;
            if (mode == 0) clear_cells(cur_y, cur_x, cols);
            else if (mode == 1) clear_cells(cur_y, 0, cur_x + 1);
            else clear_cells(cur_y, 0, cols);
            break;
        }
        case 'J': {
            int mode = param_count ? params[0] : 0;
            if (mode == 0) {
                clear_cells(cur_y, cur_x, cols);
                for (int y = cur_y + 1; y < rows; y++) clear_cells(y, 0, cols);
            } else if (mode == 1) {
                for (int y = 0; y < cur_y; y++) clear_cells(y, 0, cols);
                clear_cells(cur_y, 0, cur_x + 1);
            } else {
                for (int y = 0; y < rows; y++) clear_cells(y, 0, cols);
            }
            break;
        }
        case 's': saved_x = cur_x; saved_y = cur_y; break;
        case 'u': move_cursor(saved_x, saved_y); break;
        case 'h':
        case 'l':
            if (csi_private && param_count && params[0] == 25) {
                cursor_visible = (final == 'h');
                dirty[cur_y] = true;
            }
            break;
        default: break;
    }
}

static void feed(char c) {
    switch (state) {
        case STATE_NORMAL:
            switch (c) {
                case '\x1b': state = STATE_ESC; break;
                case '\r': move_cursor(0, cur_y); break;
                case '\n': wrap_pending = false; newline(); break;
                case '\b': if (cur_x > 0) move_cursor(cur_x - 1, cur_y); break;
                case '\t': move_cursor((cur_x / 8 + 1) * 8, cur_y); break;
                case '\a': sound_play(SOUND_NAV); break;
                default: {
                    unsigned char b = (unsigned char)c;
                    if (b >= 0x20 && b < 0x7F) {
                        utf8_remaining = 0;
                        put_char(b - 0x20);
                    } else if (b >= 0xC0 && b < 0xF8) {
                        // UTF-8 lead byte: 2, 3 or 4 byte sequence
                        utf8_remaining = b >= 0xF0 ? 3 : (b >= 0xE0 ? 2 : 1);
                        utf8_codepoint = b & (0x3F >> utf8_remaining);
                    } else if (b >= 0x80 && b < 0xC0 && utf8_remaining > 0) {
                        utf8_codepoint = (utf8_codepoint << 6) | (b & 0x3F);
                        if (--utf8_remaining == 0) put_char(glyph_for(utf8_codepoint));
                    }
                    break;
                }
            }
            break;
        case STATE_ESC:
            if (c == '[') {
                state = STATE_CSI;
                param_count = 0;
                csi_private = false;
                memset(params, 0, sizeof(params));
            } else {
                if (c == 'c') { // full reset
                    attr_fg = DEFAULT_FG; attr_bg = DEFAULT_BG; attr_bold = attr_reverse = false;
                    for (int y = 0; y < rows; y++) clear_cells(y, 0, cols);
                    move_cursor(0, 0);
                }
                state = STATE_NORMAL;
            }
            break;
        case STATE_CSI:
            if (c == '?') {
                csi_private = true;
            } else if (c >= '0' && c <= '9') {
                if (param_count == 0) param_count = 1;
                params[param_count - 1] = params[param_count - 1] * 10 + (c - '0');
            } else if (c == ';') {
                if (param_count == 0) param_count = 1;
                if (param_count < MAX_PARAMS) param_count++;
            } else if (c >= 0x40 && c <= 0x7E) {
                handle_csi(c);
                state = STATE_NORMAL;
            }
            break;
    }
}

void term_write(const char* data, size_t len) {
    power_activity(); // a long command printing counts as use
    // Mirror to the USB serial console, so the deck can also be used from a computer
    fwrite(data, 1, len, stdout);
    fflush(stdout);
    xSemaphoreTake(lock, portMAX_DELAY);
    if (view_offset != 0) {
        // New output jumps back to the live screen
        view_offset = 0;
        mark_all_dirty();
        bar_dirty = true;
    }
    for (size_t i = 0; i < len; i++) feed(data[i]);
    xSemaphoreGive(lock);
}

void term_puts(const char* text) {
    term_write(text, strlen(text));
}

void term_printf(const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (len > 0) term_write(buffer, len < (int)sizeof(buffer) ? len : (int)sizeof(buffer) - 1);
}

void term_clear(void) {
    term_puts("\x1b[2J\x1b[H");
}

// The line shown at screen row y, taking the scroll position into account
static const Cell* visible_line(int y) {
    int line = scrollback_count - view_offset + y; // index in scrollback followed by the screen
    if (line < scrollback_count) return scrollback[(scrollback_head + line) % SCROLLBACK_LINES];
    return cells[line - scrollback_count];
}

static void render_row(int y, bool show_cursor) {
    const int w = font->width, h = font->height;
    const Cell* line = visible_line(y);
    for (int x = 0; x < cols; x++) {
        Cell cell = line[x];
        uint16_t fg = palette[cell.fg], bg = palette[cell.bg];
        if (show_cursor && x == cur_x) {
            uint16_t t = fg; fg = bg; bg = t;
            if (cell.fg == cell.bg) fg = palette[DEFAULT_BG], bg = palette[DEFAULT_FG];
        }
        const uint16_t* glyph = &font->rows[cell.glyph * h];
        for (int gy = 0; gy < h; gy++) {
            uint16_t* out = &row_buffer[gy * DISPLAY_WIDTH + x * w];
            uint16_t bits = glyph[gy];
            for (int gx = 0; gx < w; gx++) {
                out[gx] = (bits & (0x8000 >> gx)) ? fg : bg;
            }
        }
    }
    // Pixels right of the last column
    for (int gy = 0; gy < h; gy++) {
        for (int px = cols * w; px < DISPLAY_WIDTH; px++) {
            row_buffer[gy * DISPLAY_WIDTH + px] = palette[DEFAULT_BG];
        }
    }
    display_draw(0, BAR_H + y * h, DISPLAY_WIDTH, h, row_buffer);
}

// Header: "< MENU" (back), the on-screen keyboard button and the status icons. No stripe, no line.
static void render_bar(void) {
    Canvas c = {row_buffer, DISPLAY_WIDTH, BAR_H};
    ui_fill(&c, 0x000000);
    ui_back(&c, 6, 0, UI_ACCENT);
    ui_keyboard_icon(&c, UI_KEYBOARD_X + 8, 0, osk_visible());

    if (view_offset > 0) {
        // Scroll position, between the buttons and the status icons
        char indicator[12];
        snprintf(indicator, sizeof(indicator), "^%d", view_offset);
        ui_text(&c, 124, 0, indicator, &fonts[2], 0xFACC15, false);
    }
    ui_status(&c, DISPLAY_WIDTH, 0);
    display_draw(0, 0, DISPLAY_WIDTH, BAR_H, row_buffer);
}

static void render_task(void* arg) {
    bool blink_on = true;
    int last_cursor_y = 0;
    TickType_t last_blink = xTaskGetTickCount();
    TickType_t last_bar = 0;
    while (true) {
        TickType_t now = xTaskGetTickCount();
        if (now - last_blink >= pdMS_TO_TICKS(BLINK_MS)) {
            blink_on = !blink_on;
            last_blink = now;
            dirty[cur_y] = true;
        }

        xSemaphoreTake(lock, portMAX_DELAY);
        if (!bar_hidden && (bar_dirty || now - last_bar >= pdMS_TO_TICKS(BAR_MS))) {
            bar_dirty = false;
            last_bar = now;
            render_bar();
        }
        if (last_cursor_y != cur_y) {
            dirty[last_cursor_y] = true;
            last_cursor_y = cur_y;
        }
        if (!paused) {
            for (int y = 0; y < rows; y++) {
                if (dirty[y]) {
                    dirty[y] = false;
                    render_row(y, view_offset == 0 && cursor_visible && blink_on && y == cur_y);
                }
            }
        }
        xSemaphoreGive(lock);
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
    }
}

void term_init(void) {
    lock = xSemaphoreCreateMutex();
    row_buffer = heap_caps_malloc(DISPLAY_WIDTH * MAX_CELL_H * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    scrollback = heap_caps_malloc(SCROLLBACK_LINES * sizeof(*scrollback), MALLOC_CAP_SPIRAM);
    init_palette();
    cols = DISPLAY_WIDTH / font->width;
    rows = AREA_H / font->height;
    for (int y = 0; y < rows; y++) clear_cells(y, 0, cols);
    // The panel powers up with random pixels
    for (int px = 0; px < DISPLAY_WIDTH * MAX_CELL_H; px++) row_buffer[px] = palette[DEFAULT_BG];
    for (int y = 0; y < DISPLAY_HEIGHT; y += MAX_CELL_H) {
        int h = DISPLAY_HEIGHT - y < MAX_CELL_H ? DISPLAY_HEIGHT - y : MAX_CELL_H;
        display_draw(0, y, DISPLAY_WIDTH, h, row_buffer);
    }
    xTaskCreatePinnedToCore(render_task, "term_render", 4096, NULL, 4, NULL, 1);
}

int term_cols(void) {
    return cols;
}

int term_rows(void) {
    return rows;
}

bool term_set_font(const char* name) {
    for (size_t i = 0; i < FONT_COUNT; i++) {
        if (strcmp(fonts[i].name, name) != 0) continue;
        xSemaphoreTake(lock, portMAX_DELAY);
        font = &fonts[i];
        cols = DISPLAY_WIDTH / font->width;
        rows = AREA_H / font->height;
        for (int y = 0; y < rows; y++) clear_cells(y, 0, cols);
        // Clear the whole terminal area: the new grid may not cover what the old one drew
        for (int px = 0; px < DISPLAY_WIDTH * MAX_CELL_H; px++) row_buffer[px] = palette[DEFAULT_BG];
        int bottom = DISPLAY_HEIGHT - reserved_bottom;
        for (int y = BAR_H; y < bottom; y += MAX_CELL_H) {
            int h = bottom - y < MAX_CELL_H ? bottom - y : MAX_CELL_H;
            display_draw(0, y, DISPLAY_WIDTH, h, row_buffer);
        }
        scrollback_count = scrollback_head = view_offset = 0;
        cur_x = cur_y = saved_x = saved_y = 0;
        wrap_pending = false;
        xSemaphoreGive(lock);
        return true;
    }
    return false;
}

void term_list_fonts(char* out, size_t size) {
    out[0] = 0;
    for (size_t i = 0; i < FONT_COUNT; i++) {
        strlcat(out, fonts[i].name, size);
        if (fonts[i].rows == font->rows) strlcat(out, "*", size);
        if (i + 1 < FONT_COUNT) strlcat(out, " ", size);
    }
}


void term_refresh_status(void) {
    bar_dirty = true;
}

void term_scroll(int lines) {
    xSemaphoreTake(lock, portMAX_DELAY);
    int offset = view_offset + lines;
    if (offset < 0) offset = 0;
    if (offset > scrollback_count) offset = scrollback_count;
    if (offset != view_offset) {
        view_offset = offset;
        mark_all_dirty();
        bar_dirty = true;
    }
    xSemaphoreGive(lock);
}

void term_pause(bool pause) {
    xSemaphoreTake(lock, portMAX_DELAY);
    paused = pause;
    if (!pause) {
        mark_all_dirty();
        bar_dirty = true;
    }
    xSemaphoreGive(lock);
}

int term_bar_height(void) {
    return BAR_H;
}

void term_hide_bar(bool hide) {
    xSemaphoreTake(lock, portMAX_DELAY);
    bar_hidden = hide;
    if (!hide) bar_dirty = true;
    xSemaphoreGive(lock);
}

void term_reserve_bottom(int pixels) {
    xSemaphoreTake(lock, portMAX_DELAY);
    reserved_bottom = pixels;
    int new_rows = AREA_H / font->height;
    if (new_rows < 1) new_rows = 1;

    if (new_rows < rows) {
        // Keep the cursor visible: lines above it scroll into the scrollback
        while (cur_y >= new_rows) {
            scroll_up();
            cur_y--;
        }
        saved_y = saved_y >= new_rows ? new_rows - 1 : saved_y;
    } else {
        for (int y = rows; y < new_rows; y++) clear_cells(y, 0, cols);
    }
    rows = new_rows;

    // Repaint what is now below the text and above the keyboard (or the freed keyboard area)
    int from = BAR_H + rows * font->height;
    int to = DISPLAY_HEIGHT - reserved_bottom;
    for (int px = 0; px < DISPLAY_WIDTH * MAX_CELL_H; px++) row_buffer[px] = palette[DEFAULT_BG];
    for (int y = from; y < to; y += MAX_CELL_H) {
        int h = to - y < MAX_CELL_H ? to - y : MAX_CELL_H;
        display_draw(0, y, DISPLAY_WIDTH, h, row_buffer);
    }
    mark_all_dirty();
    bar_dirty = true;
    xSemaphoreGive(lock);
}

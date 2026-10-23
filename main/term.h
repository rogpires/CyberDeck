#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <stdint.h>

// The grid size depends on the font: 5x8 gives the most, 64x28 (below the 16px status bar)
#define TERM_MAX_COLS 64
#define TERM_MAX_ROWS 28



// Starts the render task. Requires display_init().
void term_init(void);

// Writes text with a subset of VT100/ANSI escape sequences (colors, cursor movement, erase)
void term_write(const char* data, size_t len);
void term_puts(const char* text);
void term_printf(const char* format, ...) __attribute__((format(printf, 1, 2)));
void term_clear(void);

int term_cols(void);
int term_rows(void);
// Font names: "5x8", "6x12", "8x16", "12x24". Clears the screen.
bool term_set_font(const char* name);
// Space separated names, the current one marked with '*'
void term_list_fonts(char* out, size_t size);

// Redraws the status bar on the next frame
void term_refresh_status(void);

// Moves the view through the scrollback: positive goes back in time. Any output returns to the live screen.
void term_scroll(int lines);

// Pauses terminal body rendering (the status bar keeps updating); resume redraws everything
void term_pause(bool pause);
int term_bar_height(void);
// Stops drawing the status bar, for screens that use the full display (the home carousel)
void term_hide_bar(bool hide);
// Leaves `pixels` at the bottom of the screen to the on-screen keyboard (0 gives them back)
void term_reserve_bottom(int pixels);

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One event queue for every input: BLE keyboard, USB serial, touch (finger or stylus) and the on-screen keyboard.
// Special keys arrive as VT100 byte sequences (e.g. "\x1b[A" for arrow up), one EV_KEY per byte.

typedef enum {
    EV_KEY,          // key: one byte
    EV_TAP,          // x, y: a short touch
    EV_BACK,         // the "< MENU" area of the header was tapped
    EV_KEYBOARD,     // the keyboard button of the header was tapped
    EV_SWIPE_LEFT,
    EV_SWIPE_RIGHT,
    EV_SWIPE_UP,
    EV_SWIPE_DOWN,
} EventType;

typedef struct {
    uint8_t type;
    char key;
    int16_t x, y;
} InputEvent;

void input_init(void);
void input_push(const char* data, size_t len);
void input_push_event(const InputEvent* event);

// Any event; false on timeout. UINT32_MAX waits forever.
bool input_wait(InputEvent* out, uint32_t timeout_ms);

// Next key byte, skipping touch events; false on timeout. For reading the rest of an escape sequence.
bool input_read(char* out, uint32_t timeout_ms);

// Discards everything queued
void input_flush(void);

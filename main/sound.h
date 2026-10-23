#pragma once

#include <stdbool.h>

// UI sounds on the board's speaker (ES8311 codec + amplifier). Same tones as TentacleOS's ui_feedback.c.
typedef enum {
    SOUND_NAV,       // moving between items
    SOUND_SELECT,    // opening an item
    SOUND_KEY,       // on-screen keyboard key
    SOUND_READ,      // a scan found something
    SOUND_WRITE,     // something was saved
    SOUND_ERROR,
    SOUND_BOOT,      // the three-note chime
    SOUND_OFF,       // the chime falling, when turning off
    SOUND_PLUG,      // charger connected
    SOUND_UNPLUG,    // charger removed
    SOUND_COUNT,
} SoundKind;

void sound_init(void);

// Never blocks: if a sound is already playing (or the last one was under 120 ms ago) this one is dropped,
// so spinning the carousel doesn't pile up chirps.
void sound_play(SoundKind kind);

// 0..100, saved; 0 is mute
void sound_set_volume(int percent);
int sound_volume(void);

// Codec closed and amplifier off (held through deep sleep), for power_off()
void sound_shutdown(void);

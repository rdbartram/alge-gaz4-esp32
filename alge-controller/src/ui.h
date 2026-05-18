// ============================================================================
//  UI dispatcher. Owns the current screen and routes touches/ticks to it.
// ============================================================================
#pragma once

#include <stdint.h>

namespace ui {

enum Screen : uint8_t {
    SCREEN_SPLASH,
    SCREEN_SETUP,
    SCREEN_MATCH,
    SCREEN_HALFTIME,
    SCREEN_ENDED,
    SCREEN_PENALTY_TOSS,
    SCREEN_PENALTY,
    SCREEN_SETTINGS,
    SCREEN_PRESET_PICKER,
    SCREEN_TEXT_INPUT,
    SCREEN_NUMPAD,
    SCREEN_HISTORY,
    SCREEN_DEFAULTS,
    SCREEN_CONFIRM,   // generic Yes/No prompt used before destructive actions
};

void begin();
void tick();
void go(Screen s);
void invalidate();

Screen current();

// Common feedback helpers (vibration + draw)
void vibe_short();
void vibe_double();
void vibe_pause();
void vibe_long();

} // namespace ui

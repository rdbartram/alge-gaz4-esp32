// ============================================================================
//  Match-mode presets for FC Wängi / OFV usage. Selected at match start.
//  Half length determines clock duration of each half (count-up display).
// ============================================================================
#pragma once

#include <stdint.h>

namespace matchmodes {

struct Preset {
    const char* label;       // shown on Spieltyp picker
    const char* sublabel;    // smaller descriptive line
    uint8_t     half_minutes;
    uint8_t     total_minutes;
    bool        custom;      // true for "Freundschaftsspiel" — user adjusts
};

extern const Preset PRESETS[];
extern const uint8_t PRESET_COUNT;

const Preset& get(uint8_t idx);

} // namespace matchmodes

#include "matchmodes.h"

namespace matchmodes {

// Source: handoff section 5 match-type presets table.
const Preset PRESETS[] = {
    {"1. Mannschaft",          "Aktive . 2 x 45 Min . 2. Liga", 45, 90, false},
    {"2. / 3. Mannschaft",     "Aktive . 2 x 45 Min",           45, 90, false},
    {"Senioren 30+ / 40+",     "Senioren . 2 x 45 Min",         45, 90, false},
    {"B-Junioren",             "2 x 40 Min",                    40, 80, false},
    {"Ca-Junioren",            "2 x 35 Min",                    35, 70, false},
    {"D-Junioren (D7/D9)",     "2 x 30 Min",                    30, 60, false},
    {"E-Junioren",             "2 x 25 Min . 7-a-side",         25, 50, false},
    {"F-Junioren",             "2 x 20 Min",                    20, 40, false},
    {"G-Junioren (Bambini)",   "2 x 15 Min",                    15, 30, false},
    {"Freundschaftsspiel",     "Eigene Zeit wählen",            45, 90, true},
};
const uint8_t PRESET_COUNT = sizeof(PRESETS) / sizeof(PRESETS[0]);

const Preset& get(uint8_t idx) {
    if (idx >= PRESET_COUNT) idx = 0;
    return PRESETS[idx];
}

} // namespace matchmodes

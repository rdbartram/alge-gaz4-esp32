#include "matchmodes.h"

namespace matchmodes {

// 11-a-side OFV / SFV formats only. Per the SFV/OFV 2024 junior-category
// reform (Praesentation Juniorenkategorien, 28.10.2024) the 11-a-side
// age groups — C-Junioren and up — all run 2 × 45 min. D and below
// switched to 7-a-side / smaller formats and aren't represented here.
const Preset PRESETS[] = {
    {"1. Mannschaft",          "Aktive . 2 x 45 Min . 2. Liga", 45, 90, false},
    {"2. / 3. Mannschaft",     "Aktive . 2 x 45 Min",           45, 90, false},
    {"Senioren 30+ / 40+",     "Senioren . 2 x 45 Min",         45, 90, false},
    {"A-Junioren",             "11er . 2 x 45 Min",             45, 90, false},
    {"B-Junioren",             "11er . 2 x 45 Min",             45, 90, false},
    {"C-Junioren",             "11er . 2 x 45 Min",             45, 90, false},
    {"Freundschaftsspiel",     "Eigene Zeit wählen",            45, 90, true},
};
const uint8_t PRESET_COUNT = sizeof(PRESETS) / sizeof(PRESETS[0]);

const Preset& get(uint8_t idx) {
    if (idx >= PRESET_COUNT) idx = 0;
    return PRESETS[idx];
}

} // namespace matchmodes

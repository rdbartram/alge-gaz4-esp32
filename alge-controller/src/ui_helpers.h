// ============================================================================
//  Shared UI widgets for the M5Stack Core2 (M5GFX). Buttons, big numerals,
//  header strip, touch-region helpers.
// ============================================================================
#pragma once

#include <M5Unified.h>
#include <stdint.h>

namespace uih {

struct Rect { int16_t x, y, w, h; };

bool point_in(const Rect& r, int16_t px, int16_t py);

// Draws the top title strip (red bar with brand + status icons).
void draw_header(const char* title);

// Draws a tappable button. Returns the rect for hit-testing.
Rect draw_button(int16_t x, int16_t y, int16_t w, int16_t h,
                 const char* label, uint16_t bg, uint16_t fg,
                 bool large = false);

// Draws a single digit in the FC Wängi scoreboard style (huge bold).
void draw_score_digit(int16_t cx, int16_t cy, uint8_t value, uint16_t color);

// Draws a 2-digit score (handles >=10 case which the board can't show).
void draw_score_value(int16_t cx, int16_t cy, uint8_t value, uint16_t color);

// Draws the clock (MM:SS) centred at (cx, cy). Defaults to the big 7-seg
// Font7 face; pass a narrower font (e.g. FreeSansBold18pt7b) when the
// match screen needs the clock to fit between the +/- buttons.
void draw_clock(int16_t cx, int16_t cy, uint16_t total_seconds,
                uint16_t color, const lgfx::IFont* font = nullptr);

// Draw a small label above a big number.
void draw_label(int16_t cx, int16_t y, const char* text, uint16_t color);

// Centred string at (cx, y).
void centre_text(int16_t cx, int16_t y, const char* text,
                 uint16_t color, const lgfx::IFont* font);

// Battery icon at top-right of header.
void draw_battery_icon(int16_t x, int16_t y);

// Link strength indicator (linked + RSSI). Drawn in header area.
void draw_link_icon(int16_t x, int16_t y, bool linked, int8_t rssi);

// Gear/settings icon.
void draw_gear_icon(int16_t x, int16_t y);

} // namespace uih

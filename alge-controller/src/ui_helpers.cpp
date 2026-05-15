// ============================================================================
//  Shared UI widgets implementation.
// ============================================================================
#include "ui_helpers.h"
#include "config.h"
#include "espnow_client.h"

namespace uih {

bool point_in(const Rect& r, int16_t px, int16_t py) {
    return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

void centre_text(int16_t cx, int16_t y, const char* text,
                 uint16_t color, const lgfx::IFont* font) {
    auto& d = M5.Display;
    d.setFont(font);
    d.setTextColor(color, COLOR_BG_DARK);
    d.setTextDatum(top_center);
    d.drawString(text, cx, y);
}

void draw_label(int16_t cx, int16_t y, const char* text, uint16_t color) {
    centre_text(cx, y, text, color, &fonts::FreeSans12pt7b);
}

void draw_header(const char* title) {
    auto& d = M5.Display;
    d.fillRect(0, 0, DISPLAY_WIDTH, HEADER_HEIGHT, COLOR_PRIMARY);

    // Left: brand
    d.setTextColor(COLOR_TEXT, COLOR_PRIMARY);
    d.setFont(&fonts::FreeSansBold12pt7b);
    d.setTextDatum(middle_left);
    d.drawString(BRAND_NAME, 6, HEADER_HEIGHT / 2);

    // Right: reserve a fixed 40px strip for the status icons so the
    // right-side title never overlaps them.
    const int icon_strip_x = DISPLAY_WIDTH - 40;
    draw_link_icon(icon_strip_x, 8, espnow_client::link_ok(),
                   espnow_client::last_rssi());
    draw_battery_icon(DISPLAY_WIDTH - 22, 6);

    if (title && *title) {
        d.setFont(&fonts::FreeSans9pt7b);
        d.setTextDatum(middle_right);
        d.drawString(title, icon_strip_x - 6, HEADER_HEIGHT / 2);
    }
}

Rect draw_button(int16_t x, int16_t y, int16_t w, int16_t h,
                 const char* label, uint16_t bg, uint16_t fg,
                 bool large) {
    auto& d = M5.Display;
    d.fillRoundRect(x, y, w, h, 6, bg);
    d.drawRoundRect(x, y, w, h, 6, fg);
    d.setTextColor(fg, bg);
    d.setFont(large ? &fonts::FreeSansBold18pt7b : &fonts::FreeSansBold12pt7b);
    d.setTextDatum(middle_center);
    d.drawString(label, x + w / 2, y + h / 2);
    return Rect{x, y, w, h};
}

void draw_score_digit(int16_t cx, int16_t cy, uint8_t value, uint16_t color) {
    auto& d = M5.Display;
    d.setFont(&fonts::Font8);  // ~75pt 7-segment-ish digits
    d.setTextColor(color, COLOR_BG_DARK);
    d.setTextDatum(middle_center);
    char buf[2] = { (char)('0' + (value % 10)), 0 };
    d.drawString(buf, cx, cy);
}

void draw_score_value(int16_t cx, int16_t cy, uint8_t value, uint16_t color) {
    auto& d = M5.Display;
    d.setFont(&fonts::Font8);
    d.setTextColor(color, COLOR_BG_DARK);
    d.setTextDatum(middle_center);
    char buf[4];
    snprintf(buf, sizeof(buf), "%u", value);
    d.drawString(buf, cx, cy);
    // Wrap warning is rendered once per screen (not per side) by the match
    // screen itself — see draw_board_wrap_warning() in ui.cpp.
}

void draw_clock(int16_t cx, int16_t cy, uint16_t total_seconds,
                uint16_t color) {
    auto& d = M5.Display;
    const uint16_t mm = (total_seconds / 60) % 100;
    const uint16_t ss = total_seconds % 60;
    char buf[8];
    snprintf(buf, sizeof(buf), "%02u:%02u", mm, ss);
    d.setFont(&fonts::Font7);  // big segment style
    d.setTextColor(color, COLOR_BG_DARK);
    d.setTextDatum(middle_center);
    d.drawString(buf, cx, cy);
}

void draw_battery_icon(int16_t x, int16_t y) {
    auto& d = M5.Display;
    const int level = M5.Power.getBatteryLevel();  // 0-100
    const uint16_t color = (level < 20) ? COLOR_ERROR : COLOR_TEXT;
    d.drawRect(x, y, 16, 8, color);
    d.fillRect(x + 16, y + 2, 2, 4, color);
    const int fill = (level * 14) / 100;
    if (fill > 0) d.fillRect(x + 1, y + 1, fill, 6, color);
}

void draw_link_icon(int16_t x, int16_t y, bool linked, int8_t /*rssi*/) {
    // Three bars only — keeping the RSSI number out of the header avoids
    // crashing into the right-side title text. Full RSSI is still shown
    // on the Settings screen.
    auto& d = M5.Display;
    const uint16_t color = linked ? COLOR_SUCCESS : COLOR_WARN;
    for (int i = 0; i < 3; ++i) {
        const int h = 2 + i * 2;
        d.fillRect(x + i * 4, y + 8 - h, 3, h, color);
    }
}

void draw_gear_icon(int16_t x, int16_t y) {
    auto& d = M5.Display;
    d.fillCircle(x + 8, y + 8, 7, COLOR_DIM);
    d.fillCircle(x + 8, y + 8, 3, COLOR_PRIMARY);
}

} // namespace uih

// ============================================================================
//  Shared UI widgets implementation.
// ============================================================================
#include "ui_helpers.h"
#include "config.h"
#include "crest.h"
#include "espnow_client.h"
#include "client_ota.h"

#include <cmath>

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

    // Left: small crest icon + brand text. The 24x24 PNG is pre-sized
    // by tools/make_crest.py so we render it at native size — no
    // runtime scaling overhead inside the header redraw path.
    int brand_x = 6;
    if (CREST_SMALL_PNG_LEN) {
        const int icon_y = (HEADER_HEIGHT - CREST_SMALL_HEIGHT) / 2;
        d.drawPng(CREST_SMALL_PNG, CREST_SMALL_PNG_LEN, 4, icon_y);
        brand_x = 4 + CREST_SMALL_WIDTH + 6;
    }
    // efontJA_16 is a u8g2 face with Latin-1 Supplement so "FC Wängi
    // 1967" renders with the proper ä (FreeSans is ASCII-only).
    d.setTextColor(COLOR_TEXT, COLOR_PRIMARY);
    d.setFont(&fonts::efontJA_16);
    d.setTextDatum(middle_left);
    d.drawString(BRAND_NAME, brand_x, HEADER_HEIGHT / 2);

    // Right: reserve a fixed 56px strip for the status icons so the
    // right-side title never overlaps them. Order (right→left):
    //   battery, link, update-available badge (when pending).
    const int icon_strip_x = DISPLAY_WIDTH - 56;
    if (client_ota::has_offer()) {
        // Small green down-arrow at the left of the strip — visible on
        // every screen so the operator doesn't have to wander into
        // Settings to discover a pending update. 10x14, two-pixel
        // shaft + chevron tip.
        const int bx = icon_strip_x + 2;
        const int by = 7;
        d.fillRect(bx + 3, by,     4, 8, COLOR_SUCCESS);   // shaft
        d.fillTriangle(bx,     by + 7, bx + 9, by + 7,
                       bx + 4, by + 13,                    COLOR_SUCCESS); // tip
    }
    draw_link_icon(icon_strip_x + 14, 8, espnow_client::link_ok(),
                   espnow_client::last_rssi());
    draw_battery_icon(DISPLAY_WIDTH - 22, 6);

    if (title && *title) {
        // efontJA_14 here so titles like "LÄUFT" / "VERLÄNGERUNG" can
        // carry umlauts without falling back to "AE/UE" workarounds.
        d.setFont(&fonts::efontJA_14);
        d.setTextDatum(middle_right);
        d.drawString(title, icon_strip_x - 6, HEADER_HEIGHT / 2);
    }
}

// Cheap UTF-8 sniff: any byte >= 0x80 means non-ASCII (start byte of a
// multi-byte sequence or a Latin-1 char), so we need a u8g2 font.
static bool has_non_ascii(const char* s) {
    if (!s) return false;
    while (*s) {
        if ((unsigned char)*s++ >= 0x80) return true;
    }
    return false;
}

Rect draw_button(int16_t x, int16_t y, int16_t w, int16_t h,
                 const char* label, uint16_t bg, uint16_t fg,
                 bool large) {
    auto& d = M5.Display;
    d.fillRoundRect(x, y, w, h, 6, bg);
    d.drawRoundRect(x, y, w, h, 6, fg);
    d.setTextColor(fg, bg);
    d.setTextDatum(middle_center);

    // Single font family across all buttons (efontJA) — keeps the
    // visual style consistent on screens that mix ASCII and umlaut
    // labels (e.g. ENDED has "Neues Match" + "Verlängerung" + "Tafel
    // löschen" + "Penaltys"). Auto-shrink ladder if the label overflows.
    (void)large;
    const int16_t avail = w - 16;
    const lgfx::IFont* fonts_ladder[2] = {
        &fonts::efontJA_16,
        &fonts::efontJA_14,
    };
    for (auto* f : fonts_ladder) {
        d.setFont(f);
        if (d.textWidth(label) <= avail) break;
    }
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
                uint16_t color, const lgfx::IFont* font) {
    auto& d = M5.Display;
    // Real (uncapped) minutes — under 100 we pad with a leading zero for
    // the classic 45:00 look; at 100+ (extra time) we let the digits
    // run so the operator sees 105:23 rather than 05:23.
    const uint16_t mm = total_seconds / 60;
    const uint16_t ss = total_seconds % 60;
    char buf[8];
    if (mm < 100) snprintf(buf, sizeof(buf), "%02u:%02u", mm, ss);
    else          snprintf(buf, sizeof(buf), "%u:%02u",   mm, ss);
    d.setFont(font ? font : &fonts::Font7);  // default: big segment style
    d.setTextColor(color, COLOR_BG_DARK);
    d.setTextDatum(middle_center);
    d.drawString(buf, cx, cy);
}

void draw_battery_icon(int16_t x, int16_t y) {
    auto& d = M5.Display;
    const int level = M5.Power.getBatteryLevel();        // 0-100
    // isCharging() returns an enum (is_charging_unknown=-1, off=0, on=1);
    // a plain bool coercion treats "unknown" as true, so compare to 1.
    const bool charging = (M5.Power.isCharging() == 1);

    // When charging: whole icon in accent yellow (outline + tip + fill).
    // That's the strongest signal we can give in a 16x8 footprint —
    // colour change shows up even when you can't see fine details.
    const uint16_t frame  = charging ? COLOR_ACCENT
                          : (level < 20 ? COLOR_ERROR : COLOR_TEXT);
    const uint16_t fill_c = charging ? COLOR_ACCENT
                          : (level < 20 ? COLOR_ERROR : COLOR_TEXT);

    // Outline + positive tip
    d.drawRect(x, y, 16, 8, frame);
    d.fillRect(x + 16, y + 2, 2, 4, frame);
    // Empty the interior first so a previous (larger) fill doesn't ghost
    // when the level drops between redraws.
    d.fillRect(x + 1, y + 1, 14, 6, COLOR_PRIMARY);
    const int fill = (level * 14) / 100;
    if (fill > 0) d.fillRect(x + 1, y + 1, fill, 6, fill_c);

    // Charging: overlay a chunky lightning bolt in the header's red so
    // it punches through the yellow fill. Phone-style cue at a glance.
    if (charging) {
        // Z-shape lightning bolt, 6 px wide × 6 px tall, centred.
        const int cx = x + 8;
        const int cy = y + 4;
        d.fillTriangle(cx - 2, cy - 3, cx + 2, cy - 3, cx,     cy,     COLOR_PRIMARY);
        d.fillTriangle(cx - 2, cy + 3, cx + 2, cy + 3, cx,     cy,     COLOR_PRIMARY);
        d.fillRect    (cx - 1, cy - 1, 3, 3,                            COLOR_PRIMARY);
    }
}

void draw_link_icon(int16_t x, int16_t y, bool linked, int8_t rssi) {
    // Three bars rendered like a phone signal indicator. The number of
    // FILLED bars reflects actual ESP-NOW RSSI strength (rough buckets:
    // >-65 strong, >-80 medium, otherwise weak). Unlinked → outline-only
    // bars in warn colour so the operator can spot a dropped peer.
    auto& d = M5.Display;
    int filled = 0;
    if (linked) {
        if (rssi >= -65)      filled = 3;
        else if (rssi >= -80) filled = 2;
        else                  filled = 1;
    }
    const uint16_t fg     = linked ? COLOR_SUCCESS : COLOR_WARN;
    const uint16_t outline = COLOR_DIM;
    for (int i = 0; i < 3; ++i) {
        const int h  = 2 + i * 2;
        const int bx = x + i * 4;
        const int by = y + 8 - h;
        if (i < filled) {
            d.fillRect(bx, by, 3, h, fg);
        } else {
            d.drawRect(bx, by, 3, h, outline);
        }
    }
}

void draw_gear_icon(int16_t x, int16_t y) {
    // Procedural 8-tooth cog. For each pixel inside the 16x16 box we
    // compute polar (r, θ) from the centre. The "rim" radius depends
    // on θ — modulated by cos(8θ) so we get 8 evenly-spaced teeth:
    //   - at a tooth peak (cos = +1) the rim sits at OUTER (7 px)
    //   - in a tooth valley (cos = -1) the rim falls back to BODY (5 px)
    // Pixels inside the rim are gear body; the inner hole carves out
    // the hub. Anti-alias-free but at 16×16 the eye still locks onto
    // the gear silhouette much better than a hand-stamped bitmap.
    auto& d = M5.Display;
    constexpr float CX = 7.5f, CY = 7.5f;
    constexpr float BODY  = 5.0f;
    constexpr float OUTER = 7.0f;
    constexpr float INNER = 2.6f;
    for (int row = 0; row < 16; ++row) {
        for (int col = 0; col < 16; ++col) {
            const float dx = col - CX;
            const float dy = row - CY;
            const float r  = sqrtf(dx * dx + dy * dy);
            if (r > OUTER || r < INNER) continue;
            const float th = atan2f(dy, dx);
            const float tooth = 0.5f + 0.5f * cosf(8.0f * th); // 0..1
            const float rim = BODY + (OUTER - BODY) * tooth;
            if (r <= rim) d.drawPixel(x + col, y + row, COLOR_DIM);
        }
    }
    d.fillCircle(x + 8, y + 8, 1, COLOR_PRIMARY);   // hub dot
}

} // namespace uih

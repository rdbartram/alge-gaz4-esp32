// ============================================================================
//  Wallbox display — LilyGO T-Display-S3 (1.9" ST7789 IPS, 170x320 portrait).
//
//  Rewritten on top of M5GFX / LGFX so we share the controller's font
//  stack (efontJA u8g2 with Latin-1 supplement) — that gives us real
//  umlauts in "FC Wängi 1967", "Verlängerung", "löschen", etc. without
//  the ASCII fallback workarounds the TFT_eSPI build needed.
// ============================================================================
#include "display.h"
#include "config.h"
#include "state.h"
#include "espnow_server.h"
#include "crest.h"
#include "ota.h"

#include <Arduino.h>
// M5GFX's umbrella header doesn't expose the individual Panel /
// Bus / Light driver classes (only the auto-detect M5GFX class), so we
// pull them in directly to build a custom LilyGO T-Display-S3 panel.
#include <M5GFX.h>
#include "lgfx/v1/panel/Panel_ST7789.hpp"
#include "lgfx/v1/platforms/esp32s3/Bus_Parallel8.hpp"
#include "lgfx/v1/platforms/esp32/Light_PWM.hpp"

// =====================================================================
//  LGFX panel config — there's no auto-detect for LilyGO T-Display-S3
//  in M5GFX, so we hand-roll the bus + panel + backlight definition.
// =====================================================================
namespace {

class LilyGoTDisplayS3 : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789  _panel_instance;
    lgfx::Bus_Parallel8 _bus_instance;
    lgfx::Light_PWM     _light_instance;
public:
    LilyGoTDisplayS3() {
        {   // ---- 8-bit parallel bus (LCD_WR / LCD_RD / LCD_DC + D0..D7) ----
            auto cfg = _bus_instance.config();
            cfg.freq_write = 20000000;
            cfg.pin_wr = 8;
            cfg.pin_rd = 9;
            cfg.pin_rs = 7;     // DC
            cfg.pin_d0 = 39;
            cfg.pin_d1 = 40;
            cfg.pin_d2 = 41;
            cfg.pin_d3 = 42;
            cfg.pin_d4 = 45;
            cfg.pin_d5 = 46;
            cfg.pin_d6 = 47;
            cfg.pin_d7 = 48;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }
        {   // ---- Panel ----
            auto cfg = _panel_instance.config();
            cfg.pin_cs           = 6;
            cfg.pin_rst          = 5;
            cfg.pin_busy         = -1;
            cfg.memory_width     = 170;
            cfg.memory_height    = 320;
            cfg.panel_width      = 170;
            cfg.panel_height     = 320;
            cfg.offset_x         = 35;   // ST7789 column offset for 170 px panels
            cfg.offset_y         = 0;
            cfg.offset_rotation  = 0;
            cfg.dummy_read_pixel = 16;
            cfg.dummy_read_bits  = 1;
            cfg.readable         = false;
            cfg.invert           = true;
            cfg.rgb_order        = false;
            cfg.dlen_16bit       = false;
            cfg.bus_shared       = false;
            _panel_instance.config(cfg);
        }
        {   // ---- Backlight (PWM) ----
            auto cfg = _light_instance.config();
            cfg.pin_bl      = 38;
            cfg.invert      = false;
            cfg.freq        = 44100;
            cfg.pwm_channel = 7;
            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance);
        }
        setPanel(&_panel_instance);
    }
};

LilyGoTDisplayS3 tft;
M5Canvas          sprite(&tft);

bool     g_invalidate = true;
uint32_t g_last_render_ms = 0;
wb_state::Snapshot g_last_snap = {};

// Header geometry — same band on every screen so the brand sits in a
// predictable place.
constexpr int HEADER_H = 36;

} // namespace

namespace wb_display {

// ---- Forward decls -------------------------------------------------------
static void draw_full();
static void draw_header(const wb_state::Snapshot& s);
static void draw_footer(const wb_state::Snapshot& s);
static void draw_screen_boot();
static void draw_screen_pairing(const wb_state::Snapshot& s);
static void draw_screen_paired_idle(const wb_state::Snapshot& s);
static void draw_screen_match_live(const wb_state::Snapshot& s);
static void draw_screen_blank_burst();
static void draw_screen_polarity_test();
static void draw_screen_segment_exercise();
static void draw_screen_connection_lost();
static void draw_screen_ota_update();
static void draw_screen_error();

static void centre(int y, const char* text, uint16_t color,
                   const lgfx::IFont* font) {
    sprite.setFont(font);
    sprite.setTextColor(color, COLOR_BG_DARK);
    sprite.setTextDatum(top_center);
    sprite.drawString(text, DISPLAY_WIDTH / 2, y);
}

// =====================================================================
//  Lifecycle
// =====================================================================
void begin() {
    tft.init();
    tft.setRotation(0);   // portrait, 170×320
    tft.setBrightness(DISPLAY_DEFAULT_BRIGHTNESS);
    tft.fillScreen(COLOR_BG_DARK);

    sprite.setColorDepth(16);
    sprite.setPsram(true);
    // 170×320×2 = 108 KB — fits in PSRAM comfortably; if PSRAM isn't
    // configured the call quietly falls back to DRAM and we still have
    // enough headroom there too.
    sprite.createSprite(DISPLAY_WIDTH, DISPLAY_HEIGHT);

    invalidate();
}

void invalidate() { g_invalidate = true; }

void tick() {
    const auto snap = wb_state::snapshot();
    const uint32_t now = millis();

    if (!g_invalidate) {
        const bool dirty =
            g_last_snap.wb_mode           != snap.wb_mode           ||
            g_last_snap.match_state       != snap.match_state       ||
            g_last_snap.home_score        != snap.home_score        ||
            g_last_snap.away_score        != snap.away_score        ||
            g_last_snap.clock_seconds     != snap.clock_seconds     ||
            g_last_snap.clock_running     != snap.clock_running     ||
            g_last_snap.radio_linked      != snap.radio_linked      ||
            g_last_snap.pairing_mode      != snap.pairing_mode      ||
            g_last_snap.paired_peer_count != snap.paired_peer_count ||
            g_last_snap.info_page         != snap.info_page;
        // Re-render once a second anyway so the footer's "TX: blinkt"
        // dot can flip and the rendered clock value stays current even
        // if nothing else triggers a redraw.
        if (!dirty && now - g_last_render_ms < 500) return;
    }
    g_last_snap        = snap;
    g_last_render_ms   = now;
    g_invalidate       = false;
    draw_full();
}

// =====================================================================
//  Dispatcher + chrome (header / footer)
// =====================================================================
static void draw_full() {
    sprite.fillSprite(COLOR_BG_DARK);

    const auto snap = wb_state::snapshot();
    draw_header(snap);

    switch (snap.wb_mode) {
    case wb_state::WB_BOOT:              draw_screen_boot();                  break;
    case wb_state::WB_PAIRING:           draw_screen_pairing(snap);           break;
    case wb_state::WB_PAIRED_IDLE:       draw_screen_paired_idle(snap);       break;
    case wb_state::WB_MATCH_LIVE:        draw_screen_match_live(snap);        break;
    case wb_state::WB_BLANK_BURST:       draw_screen_blank_burst();           break;
    case wb_state::WB_POLARITY_TEST:     draw_screen_polarity_test();         break;
    case wb_state::WB_SEGMENT_EXERCISE:  draw_screen_segment_exercise();      break;
    case wb_state::WB_CONNECTION_LOST:   draw_screen_connection_lost();       break;
    case wb_state::WB_OTA_UPDATE:        draw_screen_ota_update();            break;
    case wb_state::WB_ERROR:             draw_screen_error();                 break;
    }

    draw_footer(snap);
    sprite.pushSprite(0, 0);
}

static void draw_header(const wb_state::Snapshot& s) {
    sprite.fillRect(0, 0, DISPLAY_WIDTH, HEADER_H, COLOR_PRIMARY);

    // Small crest icon at far left.
    int brand_x = 4;
    if (CREST_SMALL_PNG_LEN) {
        const int icon_y = (HEADER_H - CREST_SMALL_HEIGHT) / 2;
        sprite.drawPng(CREST_SMALL_PNG, CREST_SMALL_PNG_LEN, 4, icon_y);
        brand_x = 4 + CREST_SMALL_WIDTH + 4;
    }

    // Brand — efontJA so "Wängi" renders with the umlaut.
    sprite.setFont(&fonts::efontJA_16);
    sprite.setTextColor(COLOR_TEXT, COLOR_PRIMARY);
    sprite.setTextDatum(middle_left);
    sprite.drawString("FC Wängi 1967", brand_x, HEADER_H / 2);

    // Tiny "TAFEL" tag right-aligned so it's clearly the wall-box.
    sprite.setFont(&fonts::FreeSansBold9pt7b);
    sprite.setTextDatum(middle_right);
    sprite.drawString("TAFEL", DISPLAY_WIDTH - 4, HEADER_H / 2);
    (void)s;
}

static void draw_footer(const wb_state::Snapshot& s) {
    const int y0 = DISPLAY_HEIGHT - 50;
    sprite.fillRect(0, y0, DISPLAY_WIDTH, 50, COLOR_BG_DARK);

    sprite.setFont(&fonts::efontJA_14);

    // Three balanced rows. Earlier layout had "TX: aktiv" and the
    // version/pult line sharing y0+32 which crowded together on the
    // 170-px-wide panel. Stack each system fact on its own row, paired
    // with a complementary right-aligned counter so each row reads
    // cleanly as "[status] | [count]".
    auto row = [&](int dy, const char* left, uint16_t lc,
                   const char* right, uint16_t rc) {
        sprite.setTextColor(lc, COLOR_BG_DARK);
        sprite.setTextDatum(top_left);
        sprite.drawString(left, 6, y0 + dy);
        sprite.setTextColor(rc, COLOR_BG_DARK);
        sprite.setTextDatum(top_right);
        sprite.drawString(right, DISPLAY_WIDTH - 6, y0 + dy);
    };

    char l[24], r[24];

    snprintf(l, sizeof(l), s.polarity_ok ? "Pol: OK" : "Pol: ?");
    snprintf(r, sizeof(r), "v%s", FIRMWARE_VERSION);
    row(0, l, s.polarity_ok ? COLOR_SUCCESS : COLOR_WARN,
           r, COLOR_DIM);

    if (s.radio_linked) snprintf(l, sizeof(l), "Funk: %d dBm", s.last_rssi);
    else                snprintf(l, sizeof(l), "Funk: --");
    snprintf(r, sizeof(r), "build %u", (unsigned)CONTROLLER_FW_BUILD_EXPECTED);
    row(16, l, s.radio_linked ? COLOR_SUCCESS : COLOR_DIM,
            r, COLOR_DIM);

    const bool tx_recent = (millis() - s.last_tx_ms) < 1500;
    snprintf(l, sizeof(l), "TX: %s", tx_recent ? "aktiv" : "ruht");
    snprintf(r, sizeof(r), "%u Pult%s", s.paired_peer_count,
             s.paired_peer_count == 1 ? "" : "e");
    row(32, l, tx_recent ? COLOR_ACCENT : COLOR_DIM,
            r, COLOR_DIM);
}

// =====================================================================
//  Screens
// =====================================================================
static void draw_screen_boot() {
    // Prefer the big PNG (same drawPng path as the header — crisp,
    // gradient-friendly). Fall back to the legacy 60×60 RGB565 only if
    // the PNG isn't available (e.g. partial regen of crest.cpp).
    if (CREST_PNG_LEN && CREST_WIDTH && CREST_HEIGHT) {
        const int x = (DISPLAY_WIDTH - CREST_WIDTH) / 2;
        sprite.drawPng(CREST_PNG, CREST_PNG_LEN, x, 50);
    } else if (CREST_RGB565_WIDTH && CREST_RGB565_HEIGHT) {
        const int x = (DISPLAY_WIDTH - CREST_RGB565_WIDTH) / 2;
        sprite.pushImage(x, 60, CREST_RGB565_WIDTH, CREST_RGB565_HEIGHT, CREST_RGB565);
    }
    centre(190, "Startet…", COLOR_ACCENT, &fonts::efontJA_24);

    // simple progress dots
    static int phase = 0;
    phase = (phase + 1) % 4;
    char dots[5] = "   ";
    for (int i = 0; i <= phase && i < 3; ++i) dots[i] = '.';
    centre(225, dots, COLOR_DIM, &fonts::FreeSansBold18pt7b);
}

static void draw_screen_pairing(const wb_state::Snapshot& s) {
    centre(70,  "Warte auf",    COLOR_TEXT,    &fonts::efontJA_16);
    centre(94,  "Pult",         COLOR_TEXT,    &fonts::efontJA_16);
    centre(130, "Funk…",        COLOR_ACCENT,  &fonts::efontJA_24);

    char buf[40];
    snprintf(buf, sizeof(buf), "Schon %u verbunden", s.paired_peer_count);
    centre(170, buf, COLOR_DIM, &fonts::efontJA_14);

    // Show how long the pairing window stays open. Hard-wired to ~30 s
    // by espnow_server but exposed via pairing_remaining_ms() so the UI
    // can count down rather than just sitting on a static label.
    const uint32_t rem = espnow_server::pairing_remaining_ms();
    if (rem > 0) {
        snprintf(buf, sizeof(buf), "Noch %us offen", (unsigned)(rem / 1000 + 1));
        centre(196, buf, COLOR_ACCENT, &fonts::efontJA_14);
    }

    centre(228, "IO14 lang am",    COLOR_DIM, &fonts::efontJA_14);
    centre(244, "Wallbox = Neu",   COLOR_DIM, &fonts::efontJA_14);
}

static void draw_screen_paired_idle(const wb_state::Snapshot& s) {
    centre(70,  "Bereit für",   COLOR_TEXT,   &fonts::efontJA_24);
    centre(102, "Match",        COLOR_TEXT,   &fonts::efontJA_24);

    char buf[40];
    const uint8_t n = s.paired_peer_count;
    snprintf(buf, sizeof(buf), "%u Pult%s verbunden",
             n, n == 1 ? "" : "e");
    centre(150, buf, COLOR_DIM, &fonts::efontJA_14);

    if (n > 0) {
        char rssi[24];
        snprintf(rssi, sizeof(rssi), "%d dBm", s.last_rssi);
        centre(174, rssi, COLOR_SUCCESS, &fonts::efontJA_14);
    }
}

static const char* match_state_label(MatchState s) {
    switch (s) {
    case STATE_IDLE:             return "BEREIT";
    case STATE_SETUP:            return "SETUP";
    case STATE_HALF_1:           return "1. HALBZEIT";
    case STATE_PAUSED_H1:        return "PAUSE 1. HZ";
    case STATE_HALFTIME:         return "HALBZEITPAUSE";
    case STATE_HALF_2:           return "2. HALBZEIT";
    case STATE_PAUSED_H2:        return "PAUSE 2. HZ";
    case STATE_ENDED:            return "ENDSTAND";
    case STATE_EXTRA_TIME_1:     return "VERLÄNGERUNG 1";
    case STATE_EXTRA_TIME_2:     return "VERLÄNGERUNG 2";
    case STATE_PAUSED_ET:        return "PAUSE VERLÄNG.";
    case STATE_ET_HALFTIME:      return "ET HALBZEIT";
    case STATE_PENALTY_SHOOTOUT: return "PENALTYS";
    case STATE_PRE_MATCH:        return "ANSTOSS IN…";
    case STATE_PRE_EXTRA_TIME:   return "VERL. STARTET IN…";
    default:                     return "??";
    }
}

static void draw_screen_match_live(const wb_state::Snapshot& s) {
    // HEIM / GAST labels
    sprite.setFont(&fonts::efontJA_16);
    sprite.setTextDatum(top_center);
    sprite.setTextColor(COLOR_DIM, COLOR_BG_DARK);
    sprite.drawString("HEIM", DISPLAY_WIDTH / 4,     46);
    sprite.drawString("GAST", DISPLAY_WIDTH * 3 / 4, 46);

    // Big scoreboard digits (Font8 = ~75 pt segment-style).
    sprite.setFont(&fonts::Font8);
    sprite.setTextDatum(middle_center);
    sprite.setTextColor(COLOR_TEXT, COLOR_BG_DARK);
    char digit[2];
    digit[0] = '0' + (s.home_score % 10); digit[1] = '\0';
    sprite.drawString(digit, DISPLAY_WIDTH / 4, 110);
    digit[0] = '0' + (s.away_score % 10);
    sprite.drawString(digit, DISPLAY_WIDTH * 3 / 4, 110);

    // Divider
    sprite.drawFastHLine(10, 160, DISPLAY_WIDTH - 20, COLOR_DIM);

    // Clock — Font7 7-segment-ish, accent when running. Show real
    // minutes (no % 100) so extra-time clocks read as 105:23 instead
    // of looping back to 05:23. The GAZ4 board still wraps because of
    // its hardware (2 digits per side); the LilyGo display is fine to
    // grow into three digits.
    char clk[8];
    const uint16_t mm = s.clock_seconds / 60;
    const uint16_t ss = s.clock_seconds % 60;
    if (mm < 100) snprintf(clk, sizeof(clk), "%02u:%02u", mm, ss);
    else          snprintf(clk, sizeof(clk), "%u:%02u",   mm, ss);
    sprite.setFont(&fonts::Font7);
    sprite.setTextDatum(middle_center);
    sprite.setTextColor(s.clock_running ? COLOR_ACCENT : COLOR_DIM, COLOR_BG_DARK);
    sprite.drawString(clk, DISPLAY_WIDTH / 2, 198);

    if (s.clock_running) {
        sprite.fillCircle(DISPLAY_WIDTH / 2, 232, 4, COLOR_SUCCESS);
    }

    // State label — efontJA so VERLÄNGERUNG keeps its Ä.
    centre(252, match_state_label(s.match_state),
           s.clock_running ? COLOR_TEXT : COLOR_WARN,
           &fonts::efontJA_14);
}

static void draw_screen_blank_burst() {
    centre(120, "Tafel wird",   COLOR_TEXT,   &fonts::efontJA_24);
    centre(160, "gelöscht…",    COLOR_ACCENT, &fonts::efontJA_24);
}

static void draw_screen_polarity_test() {
    centre(60,  "POLARITÄTS-",  COLOR_PRIMARY, &fonts::efontJA_16);
    centre(80,  "TEST",         COLOR_PRIMARY, &fonts::efontJA_16);
    centre(120, "Sehen alle 6", COLOR_TEXT,    &fonts::efontJA_14);
    centre(140, "Ziffern als 8?", COLOR_TEXT,  &fonts::efontJA_14);
    centre(180, "IO14 kurz=JA",  COLOR_SUCCESS, &fonts::efontJA_14);
    centre(200, "IO14 lang=NEIN", COLOR_WARN,  &fonts::efontJA_14);
    centre(228, "(NEIN ⇒ Wallbox", COLOR_DIM,  &fonts::efontJA_14);
    centre(244, "180° drehen)",    COLOR_DIM,  &fonts::efontJA_14);
}

static void draw_screen_segment_exercise() {
    centre(80,  "Wartung",     COLOR_ACCENT, &fonts::efontJA_24);
    centre(140, "Segment-",    COLOR_TEXT,   &fonts::efontJA_16);
    centre(160, "Übung",       COLOR_TEXT,   &fonts::efontJA_16);
    centre(200, "30 Sek.",     COLOR_DIM,    &fonts::efontJA_14);
}

static void draw_screen_connection_lost() {
    sprite.fillRect(0, 60, DISPLAY_WIDTH, 50, COLOR_WARN);
    centre(64,  "FUNK",        COLOR_BG_DARK, &fonts::efontJA_24);
    centre(92,  "VERLOREN",    COLOR_BG_DARK, &fonts::efontJA_16);

    centre(150, "Kein Pult",   COLOR_TEXT,    &fonts::efontJA_14);
    centre(168, "hat sich",    COLOR_TEXT,    &fonts::efontJA_14);
    centre(186, "gemeldet",    COLOR_TEXT,    &fonts::efontJA_14);
}

static void draw_screen_ota_update() {
    centre(80,  "OTA",         COLOR_ACCENT, &fonts::efontJA_24);
    centre(110, "Update",      COLOR_TEXT,   &fonts::efontJA_16);

    // Progress bar — 130 px wide, centred. We don't always know the
    // upload's final size (multipart upload without a Content-Length
    // hint), so when bytes_total is 0 we draw a "barber pole" bar that
    // animates off the modulo of bytes_received.
    const int bar_w = 130;
    const int bar_h = 14;
    const int bar_x = (DISPLAY_WIDTH - bar_w) / 2;
    const int bar_y = 150;
    sprite.drawRect(bar_x, bar_y, bar_w, bar_h, COLOR_TEXT);

    const uint32_t recv  = wb_ota::bytes_received();
    const uint32_t total = wb_ota::bytes_total();
    if (total > 0) {
        const uint32_t fill_w = (uint64_t)(bar_w - 2) * recv / total;
        sprite.fillRect(bar_x + 1, bar_y + 1, (int)fill_w, bar_h - 2, COLOR_SUCCESS);
        char pct[8];
        snprintf(pct, sizeof(pct), "%u %%", (unsigned)((uint64_t)recv * 100 / total));
        centre(180, pct, COLOR_TEXT, &fonts::efontJA_16);
    } else {
        // Indeterminate: scrolling segment based on recv modulo bar width.
        const int seg_w = 30;
        const int pos   = (recv / 1024) % (bar_w + seg_w) - seg_w;
        const int x0    = bar_x + 1 + (pos < 0 ? 0 : pos);
        const int draw_w = (pos < 0 ? seg_w + pos : (pos + seg_w > bar_w - 2 ? (bar_w - 2 - pos) : seg_w));
        if (draw_w > 0) {
            sprite.fillRect(x0, bar_y + 1, draw_w, bar_h - 2, COLOR_SUCCESS);
        }
        char kb[16];
        snprintf(kb, sizeof(kb), "%u KB", (unsigned)(recv / 1024));
        centre(180, kb, COLOR_TEXT, &fonts::efontJA_16);
    }

    centre(220, "Bitte warten", COLOR_DIM, &fonts::efontJA_14);
}

static void draw_screen_error() {
    sprite.fillRect(0, HEADER_H, DISPLAY_WIDTH, DISPLAY_HEIGHT - HEADER_H, COLOR_ERROR);
    centre(120, "FEHLER",     COLOR_TEXT, &fonts::efontJA_24);
    centre(160, "Neu starten", COLOR_TEXT, &fonts::efontJA_14);
}

} // namespace wb_display

// ============================================================================
//  Wall-box display rendering.
//
//  Portrait orientation: 170 wide x 320 tall.
//  Layout (top to bottom):
//    [00..36 ]  Red title strip with "FC WÄNGI 1967"
//    [36..58 ]  "HEIM   GAST" labels
//    [58..138]  Big score (HEIM and GAST side by side, 60pt-ish)
//    [138..142] Divider
//    [142..210] Big clock (50pt-ish) + state label
//    [210..212] Divider
//    [212..298] Status footer (polarity, funk, tafel, TX)
//    [298..320] Version line
// ============================================================================
#include "display.h"
#include "config.h"
#include "state.h"
#include "espnow_server.h"
#include "polarity.h"
#include "crest.h"

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <stdio.h>

namespace wb_display {

static TFT_eSPI tft;
static TFT_eSprite sprite(&tft);

static bool     g_invalidate = true;
static uint32_t g_last_render_ms = 0;
static wb_state::Snapshot g_last_snap = {};

static void draw_full();
static void draw_screen_boot();
static void draw_screen_pairing();
static void draw_screen_paired_idle();
static void draw_screen_match_live();
static void draw_screen_blank_burst();
static void draw_screen_polarity_test();
static void draw_screen_segment_exercise();
static void draw_screen_connection_lost();
static void draw_screen_error();
static void draw_header();
static void draw_footer(const wb_state::Snapshot& s);

// Convenience macro for centred text in the sprite.
static void draw_text_centred(int y, const char* text, uint16_t color, int font) {
    sprite.setTextColor(color);
    sprite.setTextDatum(MC_DATUM);
    sprite.drawString(text, DISPLAY_WIDTH / 2, y, font);
}

void begin() {
    tft.init();
    tft.setRotation(0);  // portrait
    tft.fillScreen(COLOR_BG_DARK);

    sprite.setColorDepth(16);
    // 170x320x2 = 108 KB sprite, lands in DRAM. The S3 has ~230 KB free
    // after framework + WiFi, so this fits with margin. If createSprite()
    // ever returns nullptr we'd need to allocate the buffer manually from
    // PSRAM with heap_caps_malloc(MALLOC_CAP_SPIRAM).
    sprite.createSprite(DISPLAY_WIDTH, DISPLAY_HEIGHT);

    pinMode(TFT_BL, OUTPUT);
    analogWrite(TFT_BL, DISPLAY_DEFAULT_BRIGHTNESS);

    invalidate();
}

void invalidate() {
    g_invalidate = true;
}

void tick() {
    const auto snap = wb_state::snapshot();
    const uint32_t now = millis();

    // Re-render if mode changed, or every 500ms for live clock.
    if (!g_invalidate) {
        const bool dirty =
            g_last_snap.wb_mode       != snap.wb_mode       ||
            g_last_snap.match_state   != snap.match_state   ||
            g_last_snap.home_score    != snap.home_score    ||
            g_last_snap.away_score    != snap.away_score    ||
            g_last_snap.clock_seconds != snap.clock_seconds ||
            g_last_snap.clock_running != snap.clock_running ||
            g_last_snap.radio_linked  != snap.radio_linked  ||
            g_last_snap.info_page     != snap.info_page;
        if (!dirty && now - g_last_render_ms < 500) return;
    }
    g_last_snap = snap;
    g_last_render_ms = now;
    g_invalidate = false;
    draw_full();
}

static void draw_full() {
    sprite.fillSprite(COLOR_BG_DARK);
    draw_header();

    const auto snap = wb_state::snapshot();
    switch (snap.wb_mode) {
    case wb_state::WB_BOOT:              draw_screen_boot();              break;
    case wb_state::WB_PAIRING:           draw_screen_pairing();           break;
    case wb_state::WB_PAIRED_IDLE:       draw_screen_paired_idle();       break;
    case wb_state::WB_MATCH_LIVE:        draw_screen_match_live();        break;
    case wb_state::WB_BLANK_BURST:       draw_screen_blank_burst();       break;
    case wb_state::WB_POLARITY_TEST:     draw_screen_polarity_test();     break;
    case wb_state::WB_SEGMENT_EXERCISE:  draw_screen_segment_exercise();  break;
    case wb_state::WB_CONNECTION_LOST:   draw_screen_connection_lost();   break;
    case wb_state::WB_ERROR:             draw_screen_error();             break;
    }

    draw_footer(snap);
    sprite.pushSprite(0, 0);
}

static void draw_header() {
    sprite.fillRect(0, 0, DISPLAY_WIDTH, 36, COLOR_PRIMARY);
    sprite.setTextColor(COLOR_TEXT);
    sprite.setTextDatum(MC_DATUM);
    sprite.drawString("FC WAENGI 1967", DISPLAY_WIDTH / 2, 12, 2);
    sprite.drawString("TAFEL", DISPLAY_WIDTH / 2, 26, 1);
}

// ---- screens ---------------------------------------------------------------
static void draw_screen_boot() {
    // Crest centred at top of body
    if (CREST_WIDTH && CREST_HEIGHT) {
        const int x = (DISPLAY_WIDTH - CREST_WIDTH) / 2;
        sprite.pushImage(x, 56, CREST_WIDTH, CREST_HEIGHT, CREST_RGB565);
    }
    draw_text_centred(140, "Startet...", COLOR_ACCENT, 4);
    // simple progress dots
    static int phase = 0;
    phase = (phase + 1) % 4;
    char dots[5] = "    ";
    for (int i = 0; i <= phase; ++i) dots[i] = '.';
    draw_text_centred(180, dots, COLOR_DIM, 4);
}

static void draw_screen_pairing() {
    draw_text_centred(80,  "Warte auf",   COLOR_TEXT,    2);
    draw_text_centred(110, "Controller",  COLOR_TEXT,    2);
    draw_text_centred(150, ".funk..",     COLOR_ACCENT,  4);
    draw_text_centred(200, "Halte Boot-",  COLOR_DIM, 1);
    draw_text_centred(212, "Knopf zum Neu", COLOR_DIM, 1);
    draw_text_centred(224, "Pairen",       COLOR_DIM, 1);
}

static void draw_screen_paired_idle() {
    draw_text_centred(80,  "Bereit fuer", COLOR_TEXT,  2);
    draw_text_centred(110, "Match",       COLOR_TEXT,  2);
    char mac[24];
    const uint8_t* m = espnow_server::paired_mac();
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             m[0], m[1], m[2], m[3], m[4], m[5]);
    draw_text_centred(160, mac, COLOR_DIM, 1);
    const auto snap = wb_state::snapshot();
    char rssi[24];
    snprintf(rssi, sizeof(rssi), "%d dBm", snap.last_rssi);
    draw_text_centred(180, rssi, COLOR_SUCCESS, 1);
}

// Draw a 2-digit (or 1-digit) score block, big and bold.
static void draw_score_block(int cx, int cy, uint8_t digit, uint16_t color) {
    char buf[2] = { (char)('0' + (digit % 10)), 0 };
    sprite.setTextColor(color);
    sprite.setTextDatum(MC_DATUM);
    sprite.drawString(buf, cx, cy, 8);  // font 8 = ~75pt, board-style
}

static const char* match_state_label(uint8_t s) {
    switch (s) {
    case STATE_IDLE:             return "BEREIT";
    case STATE_SETUP:            return "SETUP";
    case STATE_HALF_1:           return "1. HALBZEIT";
    case STATE_PAUSED_H1:        return "PAUSIERT H1";
    case STATE_HALFTIME:         return "HALBZEITPAUSE";
    case STATE_HALF_2:           return "2. HALBZEIT";
    case STATE_PAUSED_H2:        return "PAUSIERT H2";
    case STATE_ENDED:            return "ENDSTAND";
    case STATE_EXTRA_TIME_1:     return "VERL. 1. HZ";
    case STATE_EXTRA_TIME_2:     return "VERL. 2. HZ";
    case STATE_PAUSED_ET:        return "PAUSIERT VERL";
    case STATE_PENALTY_SHOOTOUT: return "PENALTIES";
    case STATE_PRE_MATCH:        return "ANSTOSS IN...";
    default:                     return "??";
    }
}

static void draw_screen_match_live() {
    const auto snap = wb_state::snapshot();

    // HEIM/GAST labels
    sprite.setTextColor(COLOR_DIM);
    sprite.setTextDatum(MC_DATUM);
    sprite.drawString("HEIM", 40,  46, 2);
    sprite.drawString("GAST", 130, 46, 2);

    // Big scores
    draw_score_block(40,  100, snap.home_score, COLOR_TEXT);
    draw_score_block(130, 100, snap.away_score, COLOR_TEXT);

    // Divider
    sprite.drawFastHLine(10, 152, DISPLAY_WIDTH - 20, COLOR_DIM);

    // Clock
    char clk[8];
    const uint16_t mm = (snap.clock_seconds / 60) % 100;
    const uint16_t ss = snap.clock_seconds % 60;
    snprintf(clk, sizeof(clk), "%02u:%02u", mm, ss);
    sprite.setTextColor(snap.clock_running ? COLOR_ACCENT : COLOR_DIM);
    sprite.setTextDatum(MC_DATUM);
    sprite.drawString(clk, DISPLAY_WIDTH / 2, 188, 7);

    // Running dot
    if (snap.clock_running) {
        sprite.fillCircle(DISPLAY_WIDTH / 2, 218, 4, COLOR_SUCCESS);
    }

    // State label
    draw_text_centred(238, match_state_label(snap.match_state),
                      snap.clock_running ? COLOR_TEXT : COLOR_WARN, 2);
}

static void draw_screen_blank_burst() {
    draw_text_centred(140, "Tafel", COLOR_TEXT, 4);
    draw_text_centred(170, "wird", COLOR_TEXT, 4);
    draw_text_centred(200, "geloescht", COLOR_ACCENT, 4);
}

static void draw_screen_polarity_test() {
    draw_text_centred(60,  "POLARITAETS-", COLOR_PRIMARY, 2);
    draw_text_centred(78,  "TEST",         COLOR_PRIMARY, 2);
    draw_text_centred(120, "Alle 6 Ziffern", COLOR_TEXT, 2);
    draw_text_centred(140, "als 8 sichtbar?", COLOR_TEXT, 2);
    draw_text_centred(180, "IO14 kurz=JA",  COLOR_SUCCESS, 1);
    draw_text_centred(200, "IO14 lang=NEIN", COLOR_WARN, 1);
    draw_text_centred(230, "(NEIN = Wallbox", COLOR_DIM, 1);
    draw_text_centred(242, "180 drehen)",    COLOR_DIM, 1);
}

static void draw_screen_segment_exercise() {
    draw_text_centred(100, "Wartung",     COLOR_ACCENT, 4);
    draw_text_centred(140, "Segment-",    COLOR_TEXT, 2);
    draw_text_centred(160, "Uebung",      COLOR_TEXT, 2);
    draw_text_centred(200, "30 Sek.",     COLOR_DIM, 2);
}

static void draw_screen_connection_lost() {
    sprite.fillRect(0, 60, DISPLAY_WIDTH, 50, COLOR_WARN);
    draw_text_centred(75, "FUNK", COLOR_BG_DARK, 4);
    draw_text_centred(100, "VERLOREN", COLOR_BG_DARK, 2);
    draw_text_centred(150, "Controller-",  COLOR_TEXT, 2);
    draw_text_centred(170, "Verbindung",   COLOR_TEXT, 2);
    draw_text_centred(190, "weg",          COLOR_TEXT, 2);
}

static void draw_screen_error() {
    sprite.fillScreen(COLOR_ERROR);
    sprite.setTextColor(COLOR_TEXT);
    draw_text_centred(120, "FEHLER", COLOR_TEXT, 4);
    draw_text_centred(160, "Neu starten", COLOR_TEXT, 2);
}

static void draw_footer(const wb_state::Snapshot& s) {
    int y = 250;
    // Polarity
    sprite.setTextColor(s.polarity_ok ? COLOR_SUCCESS : COLOR_WARN);
    sprite.setTextDatum(ML_DATUM);
    sprite.drawString(s.polarity_ok ? "Pol: OK" : "Pol: ?", 6, y, 1);
    y += 12;
    // Funk
    char buf[32];
    if (s.radio_linked) {
        snprintf(buf, sizeof(buf), "Funk: %d dBm", s.last_rssi);
        sprite.setTextColor(COLOR_SUCCESS);
    } else {
        snprintf(buf, sizeof(buf), "Funk: --");
        sprite.setTextColor(COLOR_DIM);
    }
    sprite.drawString(buf, 6, y, 1);
    y += 12;

    // TX dot blink: bright if last_tx within 2s
    const bool tx_recent = (millis() - s.last_tx_ms) < 1500;
    sprite.setTextColor(tx_recent ? COLOR_ACCENT : COLOR_DIM);
    snprintf(buf, sizeof(buf), "TX: %s", tx_recent ? "(blinkt)" : "(idle)");
    sprite.drawString(buf, 6, y, 1);
    y += 12;

    sprite.setTextColor(COLOR_DIM);
    sprite.drawString("v" FIRMWARE_VERSION "  IO14=Menu", 6, 304, 1);
}

} // namespace wb_display

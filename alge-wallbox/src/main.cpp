// ============================================================================
//  alge-wallbox main
//
//  LilyGO T-Display-S3: receives ESP-NOW state from the M5Stack Core2
//  controller, renders state on its 1.9" IPS, and drives the Alge GAZ4
//  flip-segment scoreboard at FC Wängi's Grosswies stadium via UART through
//  a MAX3232 level shifter and two banana plugs.
// ============================================================================
#include <Arduino.h>

#include "config.h"
#include "state.h"
#include "gaz4.h"
#include "espnow_server.h"
#include "display.h"
#include "button.h"
#include "polarity.h"
#include "maintenance.h"
#include "ota.h"

namespace {

uint32_t g_last_refresh_ms     = 0;
uint8_t  g_last_home           = 255;
uint8_t  g_last_away           = 255;
uint8_t  g_last_state          = 255;
uint16_t g_last_clock          = 0;
bool     g_last_clock_running  = false;
uint32_t g_last_conn_check_ms  = 0;

void tx_state_change_burst(int repeats, int gap_ms) {
    char frame[gaz4::FRAME_LEN];
    const auto snap = wb_state::snapshot();
    gaz4::build_match_frame(snap.home_score, snap.away_score, snap.clock_seconds, frame);
    gaz4::transmit(frame, repeats, gap_ms);
    wb_state::note_gaz4_tx();
}

void tx_refresh() {
    char frame[gaz4::FRAME_LEN];
    const auto snap = wb_state::snapshot();
    gaz4::build_match_frame(snap.home_score, snap.away_score, snap.clock_seconds, frame);
    gaz4::transmit(frame, 1, 0);
    wb_state::note_gaz4_tx();
}

// Goal-celebration flash — alternates "real frame" with "blank" 3x.
// Total wallclock ~700ms. Bistable flip is happy with this cadence.
void tx_goal_flash() {
    char score_frame[gaz4::FRAME_LEN];
    char blank_frame[gaz4::FRAME_LEN];
    const auto snap = wb_state::snapshot();
    gaz4::build_match_frame(snap.home_score, snap.away_score, snap.clock_seconds, score_frame);
    gaz4::build_blank_frame(blank_frame);
    // score-blank-score-blank-score
    gaz4::transmit(score_frame, 2, 80);
    delay(120);
    gaz4::transmit(blank_frame, 2, 80);
    delay(120);
    gaz4::transmit(score_frame, 2, 80);
    delay(120);
    gaz4::transmit(blank_frame, 2, 80);
    delay(120);
    gaz4::transmit(score_frame, GAZ4_SCORE_BURST, GAZ4_SCORE_GAP_MS); // settle
    wb_state::note_gaz4_tx();
}

void gaz4_tx_scheduler() {
    const auto snap = wb_state::snapshot();
    if (snap.wb_mode != wb_state::WB_MATCH_LIVE) {
        // GAZ4 is silent unless a match is live (or maintenance is running).
        return;
    }

    const uint32_t now = millis();
    bool score_changed = (g_last_home != snap.home_score || g_last_away != snap.away_score);
    bool state_changed = (g_last_state != snap.match_state ||
                          g_last_clock_running != snap.clock_running);

    // A "goal" is a +1 increment on exactly one side, with the other unchanged.
    // (Wraps at 9->0 count as goals too — board-side wrap.)
    bool is_goal = false;
    if (score_changed) {
        const uint8_t dh = (snap.home_score - g_last_home + 10) % 10;
        const uint8_t da = (snap.away_score - g_last_away + 10) % 10;
        is_goal = (dh == 1 && da == 0) || (dh == 0 && da == 1);
    }

    if (is_goal) {
        tx_goal_flash();
    } else if (score_changed) {
        tx_state_change_burst(GAZ4_SCORE_BURST, GAZ4_SCORE_GAP_MS);
    } else if (state_changed) {
        tx_state_change_burst(GAZ4_TRANSITION_BURST, GAZ4_TRANSITION_GAP_MS);
    } else if (now - g_last_refresh_ms >= GAZ4_REFRESH_MS) {
        g_last_refresh_ms = now;
        tx_refresh();
    }

    g_last_home = snap.home_score;
    g_last_away = snap.away_score;
    g_last_state = snap.match_state;
    g_last_clock = snap.clock_seconds;
    g_last_clock_running = snap.clock_running;
}

void connection_watchdog() {
    // In v2 the wallbox is the master, so "connection lost" means no
    // controller has talked to us in a while — useful as a UI cue, but
    // not a state we recover from automatically. We just track last
    // intent timing for the display.
    const uint32_t now = millis();
    if (now - g_last_conn_check_ms < 250) return;
    g_last_conn_check_ms = now;

    const auto snap = wb_state::snapshot();
    if (espnow_server::paired_peer_count() == 0) return;

    // After 30 s of silence from every paired controller, mark the link
    // as cold. Display can choose to show a banner. A fresh intent flips
    // it back automatically because apply_intent() touches last_intent_ms.
    const bool cold = (now - snap.last_intent_ms > 30000);
    if (snap.wb_mode == wb_state::WB_MATCH_LIVE && cold) {
        wb_state::set_wb_mode(wb_state::WB_CONNECTION_LOST);
    } else if (snap.wb_mode == wb_state::WB_CONNECTION_LOST && !cold) {
        wb_state::set_wb_mode(wb_state::WB_MATCH_LIVE);
    }
}

void handle_button() {
    const auto press = wb_button::poll();
    if (press == wb_button::NONE) return;

    switch (press) {
    case wb_button::SHORT:
        if (wb_state::wb_mode() == wb_state::WB_POLARITY_TEST) {
            // Short press during polarity test = polarity OK
            wb_polarity::mark_good();
            wb_state::set_polarity_ok(true);
            wb_state::set_wb_mode(wb_state::WB_PAIRED_IDLE);
            Serial.println("[main] polarity confirmed OK");
        } else {
            wb_state::cycle_info_page();
        }
        break;
    case wb_button::LONG:
        if (wb_state::wb_mode() == wb_state::WB_POLARITY_TEST) {
            // Long press during polarity test = NOT OK, instruct rotation
            Serial.println("[main] polarity reported BAD - rotate wall-box 180");
            wb_state::set_polarity_ok(false);
        } else {
            // Open the pairing window so a new (or spare) controller
            // can join. Blank-burst is reachable through the controller
            // Settings (INTENT_BLANK) so this slot is free.
            Serial.println("[main] entering pairing mode for 30s");
            espnow_server::enter_pairing_mode();
        }
        break;
    case wb_button::VERY_LONG:
        Serial.println("[main] factory reset");
        wb_polarity::mark_unknown();
        espnow_server::factory_reset();
        break;
    default:
        break;
    }
    wb_display::invalidate();
}

} // namespace

void setup() {
    // T-Display-S3 GPIO 15 gates the LCD + peripheral power island via an
    // on-board switch. USB-C boot happens to leave it latched HIGH (via
    // the serial bridge's enable signal), which is why the board appears
    // to "just work" over USB — but when powered from raw 5 V on the
    // breakout pad the rail starts at 0 and the LCD never lights up.
    // Drive it HIGH before *anything* else so the splash actually renders.
    // (See LilyGo issue #140 / #205.)
    pinMode(15, OUTPUT);
    digitalWrite(15, HIGH);

    Serial.begin(115200);
    delay(100);
    Serial.printf("\n=== %s v%s booting ===\n", FIRMWARE_NAME, FIRMWARE_VERSION);

    wb_state::init();
    wb_display::begin();
    wb_state::set_wb_mode(wb_state::WB_BOOT);
    wb_display::tick();

    wb_polarity::begin();
    wb_state::set_polarity_ok(wb_polarity::is_known_good());

    wb_button::begin();
    gaz4::uart_begin();
    espnow_server::begin();
#ifndef WOKWI_SIM
    wb_ota::begin();  // adds SoftAP on top of the STA already used by ESP-NOW
#endif
    // Polarity test is auto-triggered the first time the wall-box ends up
    // in PAIRED_IDLE (see loop()). We don't enter it here because pairing
    // and the polarity test would otherwise clobber wb_mode on boot.
}

void loop() {
#ifndef WOKWI_SIM
    wb_ota::loop();
#endif
    espnow_server::loop();

    // Once we're paired and idle, kick off polarity test if user hasn't
    // confirmed it yet. Runs once per boot; the user's IO14 press in
    // handle_button() either marks it good or instructs rotation.
    static bool polarity_kicked = false;
    if (!polarity_kicked &&
        wb_state::wb_mode() == wb_state::WB_PAIRED_IDLE &&
        !wb_polarity::is_known_good()) {
        polarity_kicked = true;
        wb_maintenance::start_polarity_test();
        Serial.println("[main] paired - now running polarity test");
    }

    // Wallbox now owns match state — drive the clock here. dt computed
    // against the loop's own last_tick to keep timing accurate even when
    // OTA / GAZ4 bursts cost wall time.
    static uint32_t last_state_tick_ms = millis();
    const uint32_t  now_ms = millis();
    wb_state::tick(now_ms - last_state_tick_ms);
    last_state_tick_ms = now_ms;
    wb_state::persist_match_if_dirty();   // throttled NVS flush

    wb_maintenance::tick();
    // Display BEFORE the GAZ4 scheduler: tx_goal_flash() and the
    // multi-frame burst transmits each spend ~1-1.5 s in blocking
    // Serial1.flush() + delay() loops at 2400 baud. If gaz4 runs first
    // the TFT can't show the new score until the burst finishes, which
    // makes scores look like they jump in late. Drawing first means the
    // TFT shows the live state immediately and the GAZ4 board catches
    // up at its own speed.
    wb_display::tick();
    gaz4_tx_scheduler();
    handle_button();
    connection_watchdog();
    delay(5);
}

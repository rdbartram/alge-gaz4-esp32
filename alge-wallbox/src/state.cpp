// ============================================================================
//  Wall-box local state mirror.
// ============================================================================
#include "state.h"
#include "config.h"
#include <Arduino.h>
#include <string.h>

namespace wb_state {

static Snapshot g = {};

void init() {
    g = Snapshot{};
    g.wb_mode = WB_BOOT;
    g.match_state = STATE_IDLE;
    g.home_score = 0;
    g.away_score = 0;
    g.clock_seconds = 0;
    g.clock_running = false;
    g.radio_linked = false;
    g.last_rssi = -127;
    g.polarity_ok = false;
    g.info_page = 0;
    g.last_state_ms = 0;
    g.last_tx_ms = 0;
    strncpy(g.pair_status, "Bereit", sizeof(g.pair_status) - 1);
}

bool apply_state_message(const ScoreboardMessage& msg) {
    bool changed = false;

    if (g.match_state != msg.match_state) { g.match_state = (MatchState)msg.match_state; changed = true; }
    if (g.home_score != msg.home_score)   { g.home_score = msg.home_score; changed = true; }
    if (g.away_score != msg.away_score)   { g.away_score = msg.away_score; changed = true; }
    if (g.clock_seconds != msg.clock_seconds) { g.clock_seconds = msg.clock_seconds; changed = true; }

    const bool clock_running = (msg.flags & FLAG_CLOCK_RUNNING) != 0;
    if (g.clock_running != clock_running) { g.clock_running = clock_running; changed = true; }

    // Idle state -> wall-box stays at PAIRED_IDLE (no GAZ4 traffic).
    // Any active match phase -> MATCH_LIVE.
    if (msg.match_state == STATE_IDLE || msg.match_state == STATE_SETUP) {
        if (g.wb_mode != WB_PAIRED_IDLE) {
            g.wb_mode = WB_PAIRED_IDLE;
            changed = true;
        }
    } else {
        if (g.wb_mode != WB_MATCH_LIVE) {
            g.wb_mode = WB_MATCH_LIVE;
            changed = true;
        }
    }

    g.last_state_ms = millis();
    return changed;
}

void set_wb_mode(WallboxMode m) {
    g.wb_mode = m;
}

WallboxMode wb_mode() {
    return g.wb_mode;
}

void note_radio_link(bool linked, int8_t rssi) {
    g.radio_linked = linked;
    if (linked) g.last_rssi = rssi;
}

void note_gaz4_tx() {
    g.last_tx_ms = millis();
}

void cycle_info_page() {
    g.info_page = (g.info_page + 1) % 4;
}

void set_polarity_ok(bool ok) {
    g.polarity_ok = ok;
}

Snapshot snapshot() {
    return g;
}

} // namespace wb_state

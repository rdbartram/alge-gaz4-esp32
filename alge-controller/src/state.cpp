// ============================================================================
//  Controller state cache (protocol v2).
// ============================================================================
#include "state.h"
#include "config.h"
#include <Arduino.h>
#include <string.h>

namespace state {

static Match    g_match;
static Defaults g_defaults;

// Match-history cache. Filled in by update_from_history() in response to
// INTENT_REQUEST_HISTORY → the wallbox streams one MSG_HISTORY packet
// per stored entry.
static HistoryEntry g_history[HISTORY_CACHE_MAX];
static uint8_t      g_history_received = 0;
static uint8_t      g_history_expected = 0;

static uint8_t  g_history_count = 0;
static bool     g_can_undo      = false;
static bool     g_pairing_mode  = false;
static bool     g_gaz4_ok       = false;
static int8_t   g_last_rssi     = -127;
static uint32_t g_last_msg_ms   = 0;

void begin() {
    memset(&g_match,    0, sizeof(g_match));
    memset(&g_defaults, 0, sizeof(g_defaults));
    g_match.match_state = STATE_IDLE;
    strncpy(g_match.opponent, "Gegner", sizeof(g_match.opponent) - 1);
    // Sane default Vorgaben before the wallbox tells us otherwise.
    g_defaults.half_minutes            = 45;
    g_defaults.pause_minutes           = 15;
    g_defaults.auto_blank_after_match  = true;
    g_defaults.prompt_scorer_on_goal   = false;
}

void update_from_state(const StatePayload& s, int8_t rssi) {
    g_match.match_state          = (MatchState)s.match_state;
    g_match.preset_idx           = s.preset_idx;
    g_match.home_score_real      = s.home_score_real;
    g_match.away_score_real      = s.away_score_real;
    g_match.clock_seconds        = s.clock_seconds;
    g_match.half1_end_seconds    = s.half1_end_seconds;
    g_match.half2_end_seconds    = s.half2_end_seconds;
    g_match.pre_match_seconds    = s.pre_match_seconds;
    g_match.pause_target_seconds = s.pause_target_seconds;
    g_match.pk_home_kicks        = s.pk_home_kicks;
    g_match.pk_away_kicks        = s.pk_away_kicks;
    g_match.pk_home_taken        = s.pk_home_taken;
    g_match.pk_away_taken        = s.pk_away_taken;
    g_match.pk_home_first        = (s.pk_home_first != 0);
    g_match.stoppage_minutes     = s.stoppage_minutes;
    g_match.goal_count           = s.goal_count;
    g_match.clock_running        = (s.flags & FLAG_CLOCK_RUNNING) != 0;
    strncpy(g_match.opponent, s.opponent, sizeof(g_match.opponent) - 1);
    g_match.opponent[sizeof(g_match.opponent) - 1] = '\0';

    g_history_count = s.history_count;
    g_can_undo      = (s.flags & FLAG_CAN_UNDO)    != 0;
    g_pairing_mode  = (s.flags & FLAG_PAIRING_MODE) != 0;
    g_gaz4_ok       = (s.flags & FLAG_GAZ4_OK)      != 0;
    g_last_rssi     = rssi;
    g_last_msg_ms   = millis();
}

void update_from_defaults(const DefaultsPayload& d) {
    g_defaults.half_minutes           = d.half_minutes;
    g_defaults.pause_minutes          = d.pause_minutes;
    g_defaults.auto_blank_after_match = (d.auto_blank_after_match != 0);
    g_defaults.prompt_scorer_on_goal  = (d.prompt_scorer_on_goal  != 0);
}

void update_from_history(const HistoryPayload& h) {
    if (h.index >= HISTORY_CACHE_MAX) return;
    HistoryEntry& e = g_history[h.index];
    e.timestamp_unix      = h.timestamp_unix;
    e.preset_idx          = h.preset_idx;
    e.home_score_real     = h.home_score_real;
    e.away_score_real     = h.away_score_real;
    e.final_clock_seconds = h.final_clock_seconds;
    e.goal_count          = h.goal_count;
    memcpy(e.opponent, h.opponent, sizeof(e.opponent));
    g_history_expected = h.total;
    if (g_history_received < HISTORY_CACHE_MAX) g_history_received++;
}

void history_request_reset() {
    g_history_received = 0;
    g_history_expected = 0;
    memset(g_history, 0, sizeof(g_history));
}

uint8_t history_received() { return g_history_received; }
uint8_t history_expected() { return g_history_expected; }
const HistoryEntry& history_entry(uint8_t idx) {
    if (idx >= HISTORY_CACHE_MAX) idx = 0;
    return g_history[idx];
}

void note_msg_received() { g_last_msg_ms = millis(); }

const Match& peek()        { return g_match; }
bool can_undo()            { return g_can_undo; }
const Defaults& defaults() { return g_defaults; }
uint8_t history_count()    { return g_history_count; }
bool pairing_mode()        { return g_pairing_mode; }
bool gaz4_ok()             { return g_gaz4_ok; }
int8_t last_rssi()         { return g_last_rssi; }

uint32_t ms_since_last_state() {
    return millis() - g_last_msg_ms;
}

bool link_live() {
    return g_last_msg_ms != 0 && (millis() - g_last_msg_ms) < 5000;
}

const matchmodes::Preset& preset() { return matchmodes::get(g_match.preset_idx); }

} // namespace state

// ============================================================================
//  Wallbox match state — wallbox-as-master architecture (protocol v2).
//
//  The wallbox is the single source of truth for match state. Controllers
//  observe via MSG_STATE broadcasts and request changes via MSG_INTENT.
//
//  Clock model (per handoff section 5):
//   - Always counts UP from 00:00.
//   - Halftime freezes the clock at whatever HALF_1 ended on (including
//     stoppage). HALF_2 can continue from there, or reset to the
//     operator-configured Halbzeit-Länge.
//   - Scoreboard digits wrap 0-9; we keep the real score separately for
//     the controller display + history.
//   - Never auto-pauses. Operator controls pause via INTENT_PAUSE/RESUME.
// ============================================================================
#pragma once

#include "shared/message.h"
#include <stdint.h>

namespace wb_state {

// Up to 32 goals tracked per match with jersey number (1-99 or 0 = unknown).
constexpr uint8_t MAX_GOALS_PER_MATCH = 32;
constexpr uint8_t UNDO_DEPTH = 10;
constexpr uint8_t HISTORY_MAX_ENTRIES = 5;

// Lifecycle of the wallbox itself (independent of the match state).
enum WallboxMode : uint8_t {
    WB_BOOT,
    WB_PAIRING,
    WB_PAIRED_IDLE,
    WB_MATCH_LIVE,
    WB_BLANK_BURST,
    WB_POLARITY_TEST,
    WB_SEGMENT_EXERCISE,
    WB_CONNECTION_LOST,
    WB_ERROR,
};

struct GoalEntry {
    uint16_t clock_seconds;
    uint8_t  team;
    uint8_t  jersey;
};

struct HistoryEntry {
    uint32_t  timestamp_unix;
    uint8_t   preset_idx;
    uint8_t   home_score_real;
    uint8_t   away_score_real;
    uint16_t  final_clock_seconds;
    char      opponent[24];
    uint8_t   goal_count;
    GoalEntry goals[MAX_GOALS_PER_MATCH];
};

struct Match {
    MatchState match_state;
    uint8_t    preset_idx;
    uint8_t    home_score_real;
    uint8_t    away_score_real;
    uint16_t   clock_seconds;
    bool       clock_running;

    uint16_t   half1_end_seconds;
    uint16_t   half2_end_seconds;
    uint16_t   pause_target_seconds;

    uint8_t    pk_home_kicks;
    uint8_t    pk_away_kicks;
    uint8_t    pk_home_taken;
    uint8_t    pk_away_taken;
    bool       pk_home_first;     // true if HEIM took the first kick

    char       opponent[24];
    uint32_t   match_start_unix;

    uint8_t    stoppage_minutes;
    uint16_t   pre_match_seconds;
    uint8_t    goal_count;
    GoalEntry  goals[MAX_GOALS_PER_MATCH];
};

struct Defaults {
    uint8_t half_minutes;
    uint8_t pause_minutes;
    bool    auto_blank_after_match;
    bool    prompt_scorer_on_goal;
};

// Combined snapshot: wallbox-side display state PLUS the full match state.
// Returned by snapshot() under a brief critical section so callers can read
// a coherent view without locking. Mirrors fields needed by display + GAZ4.
struct Snapshot {
    // ---- Wallbox-side ----
    WallboxMode  wb_mode;
    bool         radio_linked;
    int8_t       last_rssi;
    bool         polarity_ok;
    bool         pairing_mode;       // accepting new controller MACs?
    uint8_t      paired_peer_count;
    uint8_t      info_page;
    uint32_t     last_intent_ms;     // last time any controller talked
    uint32_t     last_tx_ms;         // last GAZ4 frame sent

    // ---- Match (authority lives here) ----
    Match        match;

    // ---- Defaults ----
    Defaults     defaults;
    uint8_t      history_count;

    // ---- Convenience derived fields (read-only) ----
    // snapshot() copies these out of match for callers that don't want
    // to reach into .match for every read. Matches the API of the old
    // controller-as-master Snapshot so display.cpp / GAZ4 scheduler can
    // stay mostly untouched.
    MatchState   match_state;
    uint8_t      home_score;     // board-wrapped (match.home_score_real % 10)
    uint8_t      away_score;
    uint16_t     clock_seconds;
    bool         clock_running;
    bool         can_undo;
};

void init();

// Apply an intent message from a (validated, paired) controller. Returns
// true if state changed and a fresh MSG_STATE broadcast should be sent.
bool apply_intent(const IntentPayload& intent);

// Main-loop tick — drives the clock when running, fires auto-blank etc.
void tick(uint32_t dt_ms);

// Wallbox-mode helpers (display screen tracking).
void set_wb_mode(WallboxMode m);
WallboxMode wb_mode();

// Radio link / pairing visibility.
void note_radio_link(bool linked, int8_t rssi);
void note_intent_received();      // refreshes last_intent_ms
void set_pairing_mode(bool on);
void set_paired_peer_count(uint8_t n);

// GAZ4 + polarity bookkeeping.
void note_gaz4_tx();
void set_polarity_ok(bool ok);
void cycle_info_page();

// Atomic snapshot read.
Snapshot snapshot();
const Match& peek_match();        // unsynchronized fast read for display

// Build a StatePayload for MSG_STATE broadcast.
void fill_state_payload(StatePayload& out, bool pairing_mode, bool gaz4_ok);
void fill_defaults_payload(DefaultsPayload& out);

// Defaults storage helpers.
void load_defaults();
void save_defaults();
const Defaults& defaults();

// History.
uint8_t history_count();
const HistoryEntry& history(uint8_t idx);
void   history_clear();

// Persistence helpers — called from main loop occasionally to flush.
void persist_match_if_dirty();
bool has_persisted_match();
void clear_persisted_match();

} // namespace wb_state

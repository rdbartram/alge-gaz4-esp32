// ============================================================================
//  Controller-side state cache (protocol v2, wallbox-as-master).
//
//  The controller no longer owns match state — the wallbox does. This
//  module caches the most recent MSG_STATE + MSG_DEFAULTS the wallbox
//  broadcast, plus a few derived bits the UI needs (link liveness,
//  pairing acks, history count). All mutators have moved to the
//  espnow_client::send_intent_* family.
// ============================================================================
#pragma once

#include "shared/message.h"
#include "matchmodes.h"
#include <stdint.h>

namespace state {

// Match-state mirror — populated from the StatePayload broadcast by the
// wallbox. Same field names as the wallbox's authoritative Match so the
// UI render code (drawing screens, formatting clocks) didn't need to
// learn a new schema.
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
    uint16_t   pre_match_seconds;     // remaining countdown if PRE_MATCH

    uint8_t    pk_home_kicks;
    uint8_t    pk_away_kicks;
    uint8_t    pk_home_taken;
    uint8_t    pk_away_taken;
    bool       pk_home_first;

    uint8_t    stoppage_minutes;
    uint8_t    goal_count;
    bool       extra_time_played;
    char       opponent[24];
};

struct Defaults {
    uint8_t half_minutes;
    uint8_t pause_minutes;
    bool    auto_blank_after_match;
    bool    prompt_scorer_on_goal;
    bool    auto_start_after_break;
};

// Compact history entry — what the wallbox sends per MSG_HISTORY packet.
// (Local copy of HistoryPayload fields so UI code doesn't have to reach
// into the wire-format struct directly.)
struct HistoryEntry {
    uint32_t timestamp_unix;
    uint8_t  preset_idx;
    uint8_t  home_score_real;
    uint8_t  away_score_real;
    uint16_t final_clock_seconds;
    uint8_t  goal_count;
    char     opponent[24];
};
constexpr uint8_t HISTORY_CACHE_MAX = 5;

// ---- Lifecycle ---------------------------------------------------------
void begin();

// ---- Update from received protocol messages ----------------------------
void update_from_state(const StatePayload& s, int8_t rssi);
void update_from_defaults(const DefaultsPayload& d);
void update_from_history(const HistoryPayload& h);
void note_msg_received();            // bumps last_msg_ms

// ---- History detail cache (filled by INTENT_REQUEST_HISTORY) ----------
void          history_request_reset(); // clear cache before a fresh request
uint8_t       history_received();      // how many entries have arrived
uint8_t       history_expected();      // wallbox-reported total
const HistoryEntry& history_entry(uint8_t idx);

// ---- UI-side reads -----------------------------------------------------
const Match& peek();
bool  can_undo();
const Defaults& defaults();
uint8_t history_count();              // wallbox sends count in MSG_STATE
bool  pairing_mode();                 // wallbox flag (FLAG_PAIRING_MODE)
bool  gaz4_ok();                      // wallbox flag (FLAG_GAZ4_OK)
int8_t last_rssi();
uint32_t ms_since_last_state();       // for FUNK VERLOREN gating
bool  link_live();                    // helper: ms_since_last_state < 5000

// Bumped whenever an incoming MSG_STATE flips a field that affects
// the on-screen LAYOUT (match_state, clock_running). The UI watches
// this so it can trigger a full repaint — the per-second tick path
// only repaints the clock + status strip and would otherwise miss
// e.g. the PAUSE → START transition when a countdown parks at 0.
uint16_t layout_version();

const matchmodes::Preset& preset();   // matchmodes is still local to controller

// Wallbox doesn't broadcast full goal/history detail; on tap we ask via
// INTENT_REQUEST_FULL and the wallbox could later send a richer payload.
// For now the history screen renders from the count only.

} // namespace state

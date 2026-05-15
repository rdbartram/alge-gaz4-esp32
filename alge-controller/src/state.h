// ============================================================================
//  Controller-side match state.
//
//  Clock model (per handoff section 5):
//   - Always counts UP from 00:00.
//   - Halftime freezes the displayed clock at whatever HALF_1 ended on
//     (including stoppage). HALF_2 can continue from there, or reset to
//     45:00, depending on operator choice.
//   - Scoreboard digits wrap 0-9; we keep the real score separately so
//     the controller display can show actual numbers (e.g. 10).
//   - Never auto-pauses. Operator controls pause.
// ============================================================================
#pragma once

#include "shared/message.h"
#include "matchmodes.h"
#include <stdint.h>
#include <string.h>

namespace state {

// Up to 32 goals tracked per match with jersey number (1-99 or 0 = unknown).
constexpr uint8_t MAX_GOALS_PER_MATCH = 32;

struct GoalEntry {
    uint16_t clock_seconds;   // when the goal happened
    uint8_t  team;            // 0=home, 1=away
    uint8_t  jersey;          // 0 = unknown
};

struct History {
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
    uint8_t    home_score_real;   // can exceed 9
    uint8_t    away_score_real;
    uint16_t   clock_seconds;     // total elapsed in current phase
    bool       clock_running;

    // Halftime / extra time tracking
    uint16_t   half1_end_seconds;     // recorded when HALF_1 -> HALFTIME
    uint16_t   half2_end_seconds;
    uint16_t   pause_target_seconds;  // when the halftime countdown started

    // Penalty shootout
    uint8_t    pk_home_kicks;  // bitmask: bit set = scored
    uint8_t    pk_away_kicks;
    uint8_t    pk_home_taken;
    uint8_t    pk_away_taken;

    char       opponent[24];
    uint32_t   match_start_unix;

    // Polish features
    uint8_t    stoppage_minutes;        // referee-announced added time
    uint16_t   pre_match_seconds;       // remaining countdown to kickoff
    uint8_t    goal_count;
    GoalEntry  goals[MAX_GOALS_PER_MATCH];
};

// Undo ring buffer — keeps last 10 mutations of (home, away, clock_seconds,
// match_state, clock_running) so the operator can roll back a mis-tap.
constexpr uint8_t UNDO_DEPTH = 10;

void begin();
Match& get();
const Match& peek();
void reset();

// Wall-clock tick driven by main loop. dt_ms is elapsed since last call.
void tick(uint32_t dt_ms);

// Score adjustments (handle wrap for board, keep real value too).
void score_home_delta(int delta);
void score_away_delta(int delta);
void score_set(uint8_t home_real, uint8_t away_real);

// Polish features
void set_stoppage_minutes(uint8_t m);
void start_pre_match(uint16_t seconds);
void register_goal(bool home_team, uint8_t jersey);

// Undo / redo
void push_undo();           // snapshot current state
bool can_undo();
void undo();                // pop + apply previous snapshot

// State transitions.
void start_match(uint8_t preset_idx, const char* opponent);
void pause();
void resume();
void start_halftime();
void start_half_2(bool reset_clock_to_45);
void end_match();
void start_extra_time_1();
void start_extra_time_2();
void start_penalty_shootout();
void register_pk_kick(bool home_team, bool scored);

// Board-wrapped scores for ESP-NOW transmission (real % 10).
uint8_t home_score_board();
uint8_t away_score_board();

// Persistence.
void save();
bool load_if_exists();
void clear_persisted();

bool has_persisted_match();

// History.
void append_history();
uint8_t history_count();
const History& history(uint8_t idx);

const matchmodes::Preset& preset();

} // namespace state

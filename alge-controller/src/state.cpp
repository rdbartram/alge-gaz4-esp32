// ============================================================================
//  Controller match state implementation.
// ============================================================================
#include "state.h"
#include "config.h"

#include <Arduino.h>
#include <Preferences.h>
#include <time.h>
#include <string.h>

namespace state {

static Match g = {};

// History stored as a ring of HISTORY_MAX_ENTRIES entries, packed into NVS.
static History g_history[HISTORY_MAX_ENTRIES];
static uint8_t g_history_count = 0;

// Undo ring buffer (in-memory only — survives within a single power cycle).
struct UndoFrame {
    uint8_t  match_state;
    uint8_t  home_score_real;
    uint8_t  away_score_real;
    uint16_t clock_seconds;
    bool     clock_running;
    uint8_t  stoppage_minutes;
    uint8_t  goal_count;
};
static UndoFrame g_undo[UNDO_DEPTH];
static uint8_t   g_undo_count = 0;

static void zero_match() {
    memset(&g, 0, sizeof(g));
    g.match_state = STATE_IDLE;
    g.preset_idx = 0;
    strncpy(g.opponent, "Gegner", sizeof(g.opponent) - 1);
}

static uint32_t now_unix() {
    time_t t = time(nullptr);
    return (uint32_t)t;
}

// --- history persistence --------------------------------------------------
static void save_history() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUChar("hist_n", g_history_count);
    prefs.putBytes("history", g_history, sizeof(g_history));
    prefs.end();
}

static void load_history() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    g_history_count = prefs.getUChar("hist_n", 0);
    if (g_history_count > HISTORY_MAX_ENTRIES) g_history_count = HISTORY_MAX_ENTRIES;
    prefs.getBytes("history", g_history, sizeof(g_history));
    prefs.end();
}

// --- public API -----------------------------------------------------------
void begin() {
    zero_match();
    load_history();
}

Match& get() { return g; }
const Match& peek() { return g; }

void reset() {
    zero_match();
    clear_persisted();
}

void tick(uint32_t dt_ms) {
    if (!g.clock_running) return;
    static uint32_t frac_ms = 0;
    frac_ms += dt_ms;
    while (frac_ms >= 1000) {
        frac_ms -= 1000;
        if (g.match_state == STATE_PRE_MATCH) {
            // Counts DOWN to zero, then auto-kickoff.
            if (g.clock_seconds > 0) {
                g.clock_seconds--;
            } else {
                g.match_state = STATE_HALF_1;
                g.clock_seconds = 0;
                g.match_start_unix = time(nullptr);
                save();
            }
        } else {
            // Normal match: counts UP.
            g.clock_seconds++;
            if (g.clock_seconds >= 5999) g.clock_seconds = 5999;
        }
    }
}

void score_home_delta(int delta) {
    int v = (int)g.home_score_real + delta;
    if (v < 0) v = 0;
    if (v > 99) v = 99;
    if ((uint8_t)v == g.home_score_real) return;
    push_undo();
    g.home_score_real = (uint8_t)v;
}

void score_away_delta(int delta) {
    int v = (int)g.away_score_real + delta;
    if (v < 0) v = 0;
    if (v > 99) v = 99;
    if ((uint8_t)v == g.away_score_real) return;
    push_undo();
    g.away_score_real = (uint8_t)v;
}

void score_set(uint8_t home_real, uint8_t away_real) {
    if (home_real > 99) home_real = 99;
    if (away_real > 99) away_real = 99;
    if (home_real == g.home_score_real && away_real == g.away_score_real) return;
    push_undo();
    g.home_score_real = home_real;
    g.away_score_real = away_real;
}

uint8_t home_score_board() { return g.home_score_real % 10; }
uint8_t away_score_board() { return g.away_score_real % 10; }

// --- Polish features ------------------------------------------------------
void set_stoppage_minutes(uint8_t m) {
    push_undo();
    g.stoppage_minutes = m;
    save();
}

void start_pre_match(uint16_t seconds) {
    push_undo();
    g.match_state = STATE_PRE_MATCH;
    g.clock_seconds = seconds;
    g.clock_running = true;
    save();
}

void register_goal(bool home_team, uint8_t jersey) {
    if (g.goal_count >= MAX_GOALS_PER_MATCH) return;
    GoalEntry& e = g.goals[g.goal_count++];
    e.clock_seconds = g.clock_seconds;
    e.team = home_team ? 0 : 1;
    e.jersey = jersey;
    save();
}

// --- Undo ----------------------------------------------------------------
void push_undo() {
    UndoFrame f = {};
    f.match_state      = g.match_state;
    f.home_score_real  = g.home_score_real;
    f.away_score_real  = g.away_score_real;
    f.clock_seconds    = g.clock_seconds;
    f.clock_running    = g.clock_running;
    f.stoppage_minutes = g.stoppage_minutes;
    f.goal_count       = g.goal_count;
    // Shift entries (newest at idx 0).
    for (int i = UNDO_DEPTH - 1; i > 0; --i) g_undo[i] = g_undo[i - 1];
    g_undo[0] = f;
    if (g_undo_count < UNDO_DEPTH) g_undo_count++;
}

bool can_undo() { return g_undo_count > 0; }

void undo() {
    if (g_undo_count == 0) return;
    const UndoFrame& f = g_undo[0];
    g.match_state      = (MatchState)f.match_state;
    g.home_score_real  = f.home_score_real;
    g.away_score_real  = f.away_score_real;
    g.clock_seconds    = f.clock_seconds;
    g.clock_running    = f.clock_running;
    g.stoppage_minutes = f.stoppage_minutes;
    g.goal_count       = f.goal_count;
    // Pop.
    for (int i = 0; i < UNDO_DEPTH - 1; ++i) g_undo[i] = g_undo[i + 1];
    g_undo_count--;
    save();
}

void start_match(uint8_t preset_idx, const char* opponent) {
    zero_match();
    g.preset_idx = preset_idx;
    if (opponent && *opponent) {
        strncpy(g.opponent, opponent, sizeof(g.opponent) - 1);
        g.opponent[sizeof(g.opponent) - 1] = '\0';
    }
    g.match_state = STATE_HALF_1;
    g.clock_seconds = 0;
    g.clock_running = true;
    g.match_start_unix = now_unix();
    save();
}

void pause() {
    if (!g.clock_running) return;
    g.clock_running = false;
    switch (g.match_state) {
    case STATE_HALF_1:       g.match_state = STATE_PAUSED_H1; break;
    case STATE_HALF_2:       g.match_state = STATE_PAUSED_H2; break;
    case STATE_EXTRA_TIME_1:
    case STATE_EXTRA_TIME_2: g.match_state = STATE_PAUSED_ET; break;
    default: break;
    }
    save();
}

void resume() {
    if (g.clock_running) return;
    switch (g.match_state) {
    case STATE_PAUSED_H1: g.match_state = STATE_HALF_1;       break;
    case STATE_PAUSED_H2: g.match_state = STATE_HALF_2;       break;
    case STATE_PAUSED_ET: g.match_state = STATE_EXTRA_TIME_1; break;
    case STATE_HALFTIME:  g.match_state = STATE_HALF_2;       break;
    default: break;
    }
    g.clock_running = true;
    save();
}

void start_halftime() {
    g.half1_end_seconds = g.clock_seconds;
    g.match_state = STATE_HALFTIME;
    g.clock_running = false;
    g.pause_target_seconds = (uint16_t)(DEFAULT_PAUSE_LEN_MIN) * 60u;
    save();
}

void start_half_2(bool reset_clock_to_45) {
    g.match_state = STATE_HALF_2;
    if (reset_clock_to_45) {
        g.clock_seconds = 45u * 60u;
    } else {
        g.clock_seconds = g.half1_end_seconds;
    }
    g.clock_running = true;
    save();
}

void end_match() {
    if (g.match_state == STATE_HALF_2 || g.match_state == STATE_PAUSED_H2) {
        g.half2_end_seconds = g.clock_seconds;
    }
    g.match_state = STATE_ENDED;
    g.clock_running = false;
    save();
    append_history();
}

void start_extra_time_1() {
    g.match_state = STATE_EXTRA_TIME_1;
    g.clock_seconds = 90u * 60u;   // continues from 90:00
    g.clock_running = true;
    save();
}

void start_extra_time_2() {
    g.match_state = STATE_EXTRA_TIME_2;
    g.clock_seconds = 105u * 60u;
    g.clock_running = true;
    save();
}

void start_penalty_shootout() {
    g.match_state = STATE_PENALTY_SHOOTOUT;
    g.clock_running = false;
    g.pk_home_kicks = 0;
    g.pk_away_kicks = 0;
    g.pk_home_taken = 0;
    g.pk_away_taken = 0;
    save();
}

void register_pk_kick(bool home_team, bool scored) {
    if (home_team) {
        if (g.pk_home_taken >= 5) return;
        if (scored) {
            g.pk_home_kicks |= (1u << g.pk_home_taken);
            g.home_score_real++;
        }
        g.pk_home_taken++;
    } else {
        if (g.pk_away_taken >= 5) return;
        if (scored) {
            g.pk_away_kicks |= (1u << g.pk_away_taken);
            g.away_score_real++;
        }
        g.pk_away_taken++;
    }
    save();
}

void save() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBool("active", g.match_state != STATE_IDLE);
    prefs.putBytes("match", &g, sizeof(g));
    prefs.end();
}

bool load_if_exists() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    const bool active = prefs.getBool("active", false);
    if (active) {
        prefs.getBytes("match", &g, sizeof(g));
        // Always start paused on resume.
        g.clock_running = false;
        if (g.match_state == STATE_HALF_1) g.match_state = STATE_PAUSED_H1;
        if (g.match_state == STATE_HALF_2) g.match_state = STATE_PAUSED_H2;
        if (g.match_state == STATE_EXTRA_TIME_1 || g.match_state == STATE_EXTRA_TIME_2)
            g.match_state = STATE_PAUSED_ET;
    }
    prefs.end();
    return active;
}

bool has_persisted_match() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    const bool active = prefs.getBool("active", false);
    prefs.end();
    return active;
}

void clear_persisted() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBool("active", false);
    prefs.end();
}

// --- history --------------------------------------------------------------
void append_history() {
    History h = {};
    h.timestamp_unix = now_unix();
    h.preset_idx = g.preset_idx;
    h.home_score_real = g.home_score_real;
    h.away_score_real = g.away_score_real;
    h.final_clock_seconds = g.clock_seconds;
    strncpy(h.opponent, g.opponent, sizeof(h.opponent) - 1);

    // Shift older entries down (newest at idx 0).
    for (int i = HISTORY_MAX_ENTRIES - 1; i > 0; --i) g_history[i] = g_history[i - 1];
    g_history[0] = h;
    if (g_history_count < HISTORY_MAX_ENTRIES) g_history_count++;
    save_history();
}

uint8_t history_count() { return g_history_count; }
const History& history(uint8_t idx) {
    if (idx >= g_history_count) idx = 0;
    return g_history[idx];
}

const matchmodes::Preset& preset() { return matchmodes::get(g.preset_idx); }

} // namespace state

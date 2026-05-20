// ============================================================================
//  Wallbox match-state implementation (wallbox-as-master, protocol v2).
//
//  The state machine ported from the old controller-side state::, now
//  authoritative on the wallbox. Intents from controllers feed into
//  apply_intent(); the wall-clock tick advances the running clock; NVS
//  keeps the in-flight match alive across reboots.
// ============================================================================
#include "state.h"
#include "config.h"

#include <Arduino.h>
#include <Preferences.h>
#include <time.h>
#include <string.h>

namespace wb_state {

// --- Singleton state ------------------------------------------------------
static Match        g_match;
static HistoryEntry g_history[HISTORY_MAX_ENTRIES];
static uint8_t      g_history_count = 0;
static Defaults     g_defaults;

// Undo ring buffer (in-memory only).
struct UndoFrame {
    MatchState match_state;
    uint8_t    home_score_real;
    uint8_t    away_score_real;
    uint16_t   clock_seconds;
    bool       clock_running;
    uint8_t    stoppage_minutes;
    uint8_t    goal_count;
    uint8_t    pk_home_kicks;
    uint8_t    pk_away_kicks;
    uint8_t    pk_home_taken;
    uint8_t    pk_away_taken;
    uint8_t    pk_home_made;
    uint8_t    pk_away_made;
};
static UndoFrame g_undo[UNDO_DEPTH];
static uint8_t   g_undo_count = 0;

// Wallbox-side mode + radio bookkeeping.
static WallboxMode g_wb_mode        = WB_BOOT;
static bool        g_radio_linked   = false;
static int8_t      g_last_rssi      = 0;
static bool        g_polarity_ok    = false;
static bool        g_pairing_mode   = false;
static uint8_t     g_paired_count   = 0;
static uint8_t     g_info_page      = 0;
static uint32_t    g_last_intent_ms = 0;
static uint32_t    g_last_tx_ms     = 0;
static uint32_t    g_screen_entered_ms = 0;

// Auto-blank tracker for ENDED.
static bool g_ended_blank_fired = false;

// "Match dirty" flag; we throttle NVS writes to once per second.
static bool        g_match_dirty       = false;
static uint32_t    g_last_persist_ms   = 0;
static constexpr uint32_t MATCH_PERSIST_DEBOUNCE_MS = 1000;

// --- Helpers --------------------------------------------------------------
static uint32_t now_unix() {
    time_t t = time(nullptr);
    return (uint32_t)t;
}

static void zero_match() {
    memset(&g_match, 0, sizeof(g_match));
    g_match.match_state = STATE_IDLE;
    g_match.preset_idx  = 0;
    strncpy(g_match.opponent, "Gegner", sizeof(g_match.opponent) - 1);
}

static void mark_dirty() {
    g_match_dirty = true;
}

static void push_undo() {
    UndoFrame f = {};
    f.match_state      = g_match.match_state;
    f.home_score_real  = g_match.home_score_real;
    f.away_score_real  = g_match.away_score_real;
    f.clock_seconds    = g_match.clock_seconds;
    f.clock_running    = g_match.clock_running;
    f.stoppage_minutes = g_match.stoppage_minutes;
    f.goal_count       = g_match.goal_count;
    f.pk_home_kicks    = g_match.pk_home_kicks;
    f.pk_away_kicks    = g_match.pk_away_kicks;
    f.pk_home_taken    = g_match.pk_home_taken;
    f.pk_away_taken    = g_match.pk_away_taken;
    f.pk_home_made     = g_match.pk_home_made;
    f.pk_away_made     = g_match.pk_away_made;
    for (int i = UNDO_DEPTH - 1; i > 0; --i) g_undo[i] = g_undo[i - 1];
    g_undo[0] = f;
    if (g_undo_count < UNDO_DEPTH) g_undo_count++;
}

static void undo_pop() {
    if (g_undo_count == 0) return;
    const UndoFrame& f = g_undo[0];
    g_match.match_state      = f.match_state;
    g_match.home_score_real  = f.home_score_real;
    g_match.away_score_real  = f.away_score_real;
    g_match.clock_seconds    = f.clock_seconds;
    g_match.clock_running    = f.clock_running;
    g_match.stoppage_minutes = f.stoppage_minutes;
    g_match.goal_count       = f.goal_count;
    g_match.pk_home_kicks    = f.pk_home_kicks;
    g_match.pk_away_kicks    = f.pk_away_kicks;
    g_match.pk_home_taken    = f.pk_home_taken;
    g_match.pk_away_taken    = f.pk_away_taken;
    g_match.pk_home_made     = f.pk_home_made;
    g_match.pk_away_made     = f.pk_away_made;
    for (int i = 0; i < UNDO_DEPTH - 1; ++i) g_undo[i] = g_undo[i + 1];
    g_undo_count--;
}

// --- NVS persistence ------------------------------------------------------
static void persist_history() {
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

static void persist_match_now() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBool("active", g_match.match_state != STATE_IDLE);
    prefs.putBytes("match", &g_match, sizeof(g_match));
    prefs.end();
    g_match_dirty = false;
    g_last_persist_ms = millis();
}

static bool load_match_if_exists() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    const bool active = prefs.getBool("active", false);
    if (active) {
        prefs.getBytes("match", &g_match, sizeof(g_match));
        // Always resume paused.
        g_match.clock_running = false;
        if (g_match.match_state == STATE_HALF_1)       g_match.match_state = STATE_PAUSED_H1;
        else if (g_match.match_state == STATE_HALF_2)  g_match.match_state = STATE_PAUSED_H2;
        else if (g_match.match_state == STATE_EXTRA_TIME_1 ||
                 g_match.match_state == STATE_EXTRA_TIME_2)
            g_match.match_state = STATE_PAUSED_ET;
    }
    prefs.end();
    return active;
}

// --- Defaults persistence -------------------------------------------------
void load_defaults() {
    Preferences p;
    p.begin(NVS_NAMESPACE, true);
    g_defaults.half_minutes  = p.getUChar("d_half",      DEFAULT_HALF_LEN_MIN);
    g_defaults.pause_minutes = p.getUChar("d_pause",     DEFAULT_PAUSE_LEN_MIN);
    g_defaults.auto_blank_after_match = p.getBool("d_autoblank", true);
    g_defaults.prompt_scorer_on_goal  = p.getBool("d_scorer",    false);
    g_defaults.auto_start_after_break = p.getBool("d_autoseg",   false);
    p.end();
}

void save_defaults() {
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    p.putUChar("d_half",      g_defaults.half_minutes);
    p.putUChar("d_pause",     g_defaults.pause_minutes);
    p.putBool ("d_autoblank", g_defaults.auto_blank_after_match);
    p.putBool ("d_scorer",    g_defaults.prompt_scorer_on_goal);
    p.putBool ("d_autoseg",   g_defaults.auto_start_after_break);
    p.end();
}

const Defaults& defaults() { return g_defaults; }

// --- History --------------------------------------------------------------
static void append_history() {
    HistoryEntry h = {};
    h.timestamp_unix       = now_unix();
    h.preset_idx           = g_match.preset_idx;
    h.home_score_real      = g_match.home_score_real;
    h.away_score_real      = g_match.away_score_real;
    h.final_clock_seconds  = g_match.clock_seconds;
    strncpy(h.opponent, g_match.opponent, sizeof(h.opponent) - 1);
    h.goal_count = g_match.goal_count;
    memcpy(h.goals, g_match.goals, sizeof(h.goals));

    for (int i = HISTORY_MAX_ENTRIES - 1; i > 0; --i) g_history[i] = g_history[i - 1];
    g_history[0] = h;
    if (g_history_count < HISTORY_MAX_ENTRIES) g_history_count++;
    persist_history();
}

uint8_t history_count() { return g_history_count; }
const HistoryEntry& history(uint8_t idx) {
    if (idx >= g_history_count) idx = 0;
    return g_history[idx];
}

void history_clear() {
    g_history_count = 0;
    memset(g_history, 0, sizeof(g_history));
    persist_history();
}

// --- State transitions ----------------------------------------------------
static void do_start_match(uint8_t preset_idx, const char* opponent) {
    zero_match();
    g_match.preset_idx = preset_idx;
    if (opponent && *opponent) {
        strncpy(g_match.opponent, opponent, sizeof(g_match.opponent) - 1);
        g_match.opponent[sizeof(g_match.opponent) - 1] = '\0';
    }
    g_match.match_state = STATE_HALF_1;
    g_match.clock_seconds = 0;
    g_match.clock_running = true;
    g_match.match_start_unix = now_unix();
}

static void do_pause() {
    if (!g_match.clock_running) return;
    g_match.clock_running = false;
    switch (g_match.match_state) {
    case STATE_HALF_1:       g_match.match_state = STATE_PAUSED_H1; break;
    case STATE_HALF_2:       g_match.match_state = STATE_PAUSED_H2; break;
    case STATE_EXTRA_TIME_1:
    case STATE_EXTRA_TIME_2: g_match.match_state = STATE_PAUSED_ET; break;
    default: break;
    }
}

static void do_resume() {
    if (g_match.clock_running) return;
    switch (g_match.match_state) {
    case STATE_PAUSED_H1: g_match.match_state = STATE_HALF_1;       break;
    case STATE_PAUSED_H2: g_match.match_state = STATE_HALF_2;       break;
    case STATE_PAUSED_ET:
        g_match.match_state = (g_match.clock_seconds >= 105u * 60u)
                            ? STATE_EXTRA_TIME_2
                            : STATE_EXTRA_TIME_1;
        break;
    case STATE_HALFTIME:    g_match.match_state = STATE_HALF_2;        break;
    case STATE_ET_HALFTIME: g_match.match_state = STATE_EXTRA_TIME_2;  break;
    case STATE_PRE_MATCH:
        // Manual GO at the end of the kickoff countdown — promote into
        // HALF_1 at 00:00. (If the countdown still had time on it the
        // resume just restarts the count-down — handled by the
        // generic clock_running=true below.)
        if (g_match.clock_seconds == 0) {
            g_match.match_state    = STATE_HALF_1;
            g_match.match_start_unix = (uint32_t)time(nullptr);
        }
        break;
    case STATE_PRE_EXTRA_TIME:
        if (g_match.clock_seconds == 0) {
            g_match.match_state   = STATE_EXTRA_TIME_1;
            g_match.clock_seconds = 90u * 60u;
            g_match.extra_time_played = true;
        }
        break;
    default: break;
    }
    g_match.clock_running = true;
}

static void do_start_halftime() {
    const bool from_et = (g_match.match_state == STATE_EXTRA_TIME_1 ||
                          g_match.match_state == STATE_PAUSED_ET);
    if (from_et) {
        g_match.match_state = STATE_ET_HALFTIME;
    } else {
        g_match.half1_end_seconds = g_match.clock_seconds;
        g_match.match_state = STATE_HALFTIME;
    }
    g_match.clock_running = false;
    g_match.pause_target_seconds = (uint16_t)(g_defaults.pause_minutes) * 60u;
}

static void do_start_half_2(bool reset_clock, uint16_t target_seconds) {
    g_match.match_state   = STATE_HALF_2;
    g_match.clock_seconds = reset_clock ? target_seconds : g_match.half1_end_seconds;
    g_match.clock_running = true;
}

static void do_end_match() {
    if (g_match.match_state == STATE_HALF_2 || g_match.match_state == STATE_PAUSED_H2) {
        g_match.half2_end_seconds = g_match.clock_seconds;
    }
    g_match.match_state = STATE_ENDED;
    g_match.clock_running = false;
    append_history();
    g_ended_blank_fired = false;
    g_screen_entered_ms = millis();
}

static void do_start_extra_time_1() {
    g_match.match_state = STATE_EXTRA_TIME_1;
    g_match.clock_seconds = 90u * 60u;
    g_match.clock_running = true;
    g_match.extra_time_played = true;
}

static void do_start_extra_time_2() {
    g_match.match_state = STATE_EXTRA_TIME_2;
    g_match.clock_seconds = 105u * 60u;
    g_match.clock_running = true;
}

static void do_start_penalties(bool home_first) {
    g_match.match_state   = STATE_PENALTY_SHOOTOUT;
    g_match.clock_running = false;
    g_match.pk_home_kicks = 0;
    g_match.pk_away_kicks = 0;
    g_match.pk_home_taken = 0;
    g_match.pk_away_taken = 0;
    g_match.pk_home_made  = 0;
    g_match.pk_away_made  = 0;
    g_match.pk_home_first = home_first;
}

// True iff the shootout is mathematically decided. Distinguishes the
// two regimes:
//   - Standard round: a side wins as soon as its made count exceeds
//     the maximum the opponent can still reach with their remaining
//     kicks (e.g. 3-0 after 3+3 means away can't catch up).
//   - Sudden death: starts once both have taken five. Decided after
//     a matched pair of kicks where one side made and the other didn't.
static bool pk_shootout_decided() {
    const uint8_t ht = g_match.pk_home_taken;
    const uint8_t at = g_match.pk_away_taken;
    const uint8_t hm = g_match.pk_home_made;
    const uint8_t am = g_match.pk_away_made;
    const bool sd = (ht >= 5 && at >= 5);
    if (sd) {
        return (ht == at) && (hm != am);
    }
    const uint8_t kicks_left_h = (ht >= 5) ? 0 : (5 - ht);
    const uint8_t kicks_left_a = (at >= 5) ? 0 : (5 - at);
    const uint8_t max_h = hm + kicks_left_h;
    const uint8_t max_a = am + kicks_left_a;
    return (hm > max_a) || (am > max_h);
}

static void do_pk_kick(bool home_team, bool scored) {
    // Standard round is 5 kicks per side. After that, sudden death:
    // keep ticking up to 11 attempts each (5 + 6 SD, easily covers any
    // amateur shootout). The bitmask is uint8_t so only the first 8
    // kicks get per-kick history; kicks 9-11 still count toward the
    // score, just without an individual box on the UI.
    uint8_t& taken = home_team ? g_match.pk_home_taken : g_match.pk_away_taken;
    uint8_t& mask  = home_team ? g_match.pk_home_kicks : g_match.pk_away_kicks;
    uint8_t& score = home_team ? g_match.home_score_real : g_match.away_score_real;
    uint8_t& made  = home_team ? g_match.pk_home_made   : g_match.pk_away_made;
    if (taken >= 11) return;
    // Snapshot for undo so the "Zurück" button on the penalty screen
    // can roll a mistapped Tor/Daneben back. (Without this, undo would
    // skip over PK kicks entirely.)
    push_undo();
    if (scored) {
        if (taken < 8) mask |= (1u << taken);
        if (score < 99) score++;
        if (made < 99) made++;
    }
    taken++;

    // Auto-end the match the moment the shootout is decided — saves
    // the operator from having to spot the winning kick and tap ENDE.
    if (pk_shootout_decided()) {
        do_end_match();
    }
}

static void do_score_delta(bool home, int delta) {
    uint8_t& s = home ? g_match.home_score_real : g_match.away_score_real;
    int v = (int)s + delta;
    if (v < 0) v = 0;
    if (v > 99) v = 99;
    if ((uint8_t)v == s) return;
    push_undo();
    s = (uint8_t)v;
}

static void do_score_set(uint8_t home, uint8_t away) {
    if (home > 99) home = 99;
    if (away > 99) away = 99;
    if (home == g_match.home_score_real && away == g_match.away_score_real) return;
    push_undo();
    g_match.home_score_real = home;
    g_match.away_score_real = away;
}

static void do_register_goal(bool home_team, uint8_t jersey) {
    if (g_match.goal_count >= MAX_GOALS_PER_MATCH) return;
    GoalEntry& e = g_match.goals[g_match.goal_count++];
    e.clock_seconds = g_match.clock_seconds;
    e.team = home_team ? 0 : 1;
    e.jersey = jersey;
}

// ============================================================================
//  apply_intent — single entry point from ESP-NOW handler.
// ============================================================================
bool apply_intent(const IntentPayload& it) {
    g_last_intent_ms = millis();
    bool changed = true;

    switch ((IntentType)it.intent_type) {
    case INTENT_NONE: changed = false; break;

    case INTENT_START_MATCH:
        do_start_match(it.u8_a, it.opponent);
        break;

    case INTENT_PAUSE:  do_pause();  break;
    case INTENT_RESUME: do_resume(); break;

    case INTENT_SCORE_HOME_DELTA: do_score_delta(true,  it.i8_a); break;
    case INTENT_SCORE_AWAY_DELTA: do_score_delta(false, it.i8_a); break;
    case INTENT_SCORE_SET:        do_score_set(it.u8_a, it.u8_b); break;

    case INTENT_CLOCK_SET:
        push_undo();
        g_match.clock_seconds = it.u16_a;
        break;

    case INTENT_START_HALFTIME:    do_start_halftime();                          break;
    case INTENT_START_HALF_2:
        // Wallbox dispatches based on current state — when we're in
        // ET_HALFTIME this intent means "start ET2", not "start H2".
        if (g_match.match_state == STATE_ET_HALFTIME) {
            do_start_extra_time_2();
        } else {
            do_start_half_2(it.u8_a != 0, it.u16_a);
        }
        break;
    case INTENT_END_MATCH:         do_end_match();                               break;
    case INTENT_START_EXTRA_TIME:
        // Drop into a 1-min count-down instead of leaping straight into
        // ET1 — football has a short break between regulation and
        // extra time, and the operator usually needs a moment to brief
        // the team. tick() promotes PRE_EXTRA_TIME → EXTRA_TIME_1 once
        // the clock hits zero. Mark the flag now (not on promotion) so
        // an operator who aborts the countdown still doesn't get the
        // "Verlängerung" button offered a second time.
        g_match.match_state   = STATE_PRE_EXTRA_TIME;
        g_match.clock_seconds = 60;
        g_match.clock_running = true;
        g_match.extra_time_played = true;
        break;
    case INTENT_SKIP_COUNTDOWN:
        // Operator's "skip to next phase" button on the pre-match / pre-ET
        // countdown. Saves waiting out the configured count-down when the
        // teams are already on the pitch.
        if (g_match.match_state == STATE_PRE_MATCH) {
            g_match.match_state      = STATE_HALF_1;
            g_match.clock_seconds    = 0;
            g_match.clock_running    = true;
            g_match.match_start_unix = (uint32_t)time(nullptr);
        } else if (g_match.match_state == STATE_PRE_EXTRA_TIME) {
            g_match.match_state       = STATE_EXTRA_TIME_1;
            g_match.clock_seconds     = 90u * 60u;
            g_match.clock_running     = true;
            g_match.extra_time_played = true;
        } else {
            changed = false;
        }
        break;
    case INTENT_START_PENALTIES:   do_start_penalties(it.u8_a != 0);             break;
    case INTENT_PK_KICK:           do_pk_kick(it.u8_a != 0, it.u8_b != 0);       break;

    case INTENT_STOPPAGE_SET:
        push_undo();
        g_match.stoppage_minutes = it.u8_a;
        break;

    case INTENT_PRE_MATCH:
        zero_match();
        g_match.preset_idx = it.u8_a;
        if (it.opponent[0]) {
            strncpy(g_match.opponent, it.opponent, sizeof(g_match.opponent) - 1);
            g_match.opponent[sizeof(g_match.opponent) - 1] = '\0';
        }
        g_match.match_state    = STATE_PRE_MATCH;
        g_match.clock_seconds  = it.u16_a;
        g_match.clock_running  = true;
        break;

    case INTENT_CANCEL_PRE_MATCH:
        if (g_match.match_state == STATE_PRE_MATCH) {
            zero_match();
        } else {
            changed = false;
        }
        break;

    case INTENT_UNDO:
        if (g_undo_count > 0) undo_pop();
        else                  changed = false;
        break;

    case INTENT_RESET:
        zero_match();
        break;

    case INTENT_REGISTER_GOAL:
        do_register_goal(it.u8_a != 0, it.u8_b);
        break;

    case INTENT_HISTORY_CLEAR:
        history_clear();
        break;

    case INTENT_SET_DEFAULTS:
        g_defaults.half_minutes            = it.defaults.half_minutes;
        g_defaults.pause_minutes           = it.defaults.pause_minutes;
        g_defaults.auto_blank_after_match  = it.defaults.auto_blank_after_match != 0;
        g_defaults.prompt_scorer_on_goal   = it.defaults.prompt_scorer_on_goal  != 0;
        g_defaults.auto_start_after_break  = it.defaults.auto_start_after_break != 0;
        save_defaults();
        break;

    // Maintenance / GAZ4-side intents: record the request via wb_mode so
    // the main-loop dispatcher picks it up. apply_intent itself doesn't
    // drive the UART.
    case INTENT_BLANK:
        set_wb_mode(WB_BLANK_BURST);
        break;
    case INTENT_POLARITY_TEST:
        set_wb_mode(WB_POLARITY_TEST);
        break;
    case INTENT_SEGMENT_EXERCISE:
        set_wb_mode(WB_SEGMENT_EXERCISE);
        break;
    case INTENT_FACTORY_RESET: {
        Preferences p;
        p.begin(NVS_NAMESPACE, false);
        p.clear();
        p.end();
        zero_match();
        g_history_count = 0;
        memset(g_history, 0, sizeof(g_history));
        load_defaults();
        delay(200);
        ESP.restart();
        break;
    }

    case INTENT_REQUEST_FULL:
        // Caller will broadcast a fresh MSG_STATE + MSG_DEFAULTS; nothing
        // to change here.
        changed = false;
        break;

    default:
        changed = false;
        break;
    }

    if (changed) mark_dirty();
    return changed;
}

// ============================================================================
//  Main-loop tick — drives the clock when running.
// ============================================================================
void tick(uint32_t dt_ms) {
    // Auto-sync the wallbox display mode to whatever the match state
    // says. Skipped for the modes that own the screen explicitly
    // (boot, pairing, polarity test, segment exercise, blank burst,
    // error, connection lost) — those toggle themselves on/off.
    if (g_wb_mode == WB_PAIRED_IDLE || g_wb_mode == WB_MATCH_LIVE) {
        const bool live = (g_match.match_state != STATE_IDLE &&
                           g_match.match_state != STATE_SETUP);
        const WallboxMode want = live ? WB_MATCH_LIVE : WB_PAIRED_IDLE;
        if (g_wb_mode != want) set_wb_mode(want);
    }

    if (g_match.match_state == STATE_ENDED &&
        g_defaults.auto_blank_after_match &&
        !g_ended_blank_fired &&
        millis() - g_screen_entered_ms > ENDED_AUTO_BLANK_MS) {
        g_ended_blank_fired = true;
        set_wb_mode(WB_BLANK_BURST);
        zero_match();
        mark_dirty();
    }

    // Idle-too-long watchdog: if the match has been paused / waiting for
    // 60+ minutes, the operator has walked off without cleaning up. End
    // the match (so it still lands in history) and let the existing
    // ENDED auto-blank flow zero the board. Covers explicit PAUSE,
    // halftime/ET-halftime breaks and parked pre-* countdowns alike.
    static uint32_t s_idle_since_ms = 0;
    constexpr uint32_t IDLE_LIMIT_MS = 60u * 60u * 1000u;
    const MatchState ms = g_match.match_state;
    const bool in_paused = (ms == STATE_PAUSED_H1 || ms == STATE_PAUSED_H2 ||
                            ms == STATE_PAUSED_ET || ms == STATE_HALFTIME ||
                            ms == STATE_ET_HALFTIME);
    const bool in_pre    = (ms == STATE_PRE_MATCH || ms == STATE_PRE_EXTRA_TIME);
    if (g_match.clock_running || (!in_paused && !in_pre)) {
        s_idle_since_ms = 0;
    } else {
        const uint32_t now = millis();
        if (s_idle_since_ms == 0) {
            s_idle_since_ms = now;
        } else if (now - s_idle_since_ms > IDLE_LIMIT_MS) {
            s_idle_since_ms = 0;
            if (in_paused) {
                do_end_match();      // writes history, lands in STATE_ENDED
            } else {
                zero_match();        // PRE_* never produced a match worth logging
                set_wb_mode(WB_BLANK_BURST);
            }
            mark_dirty();
        }
    }

    if (!g_match.clock_running) return;
    static uint32_t frac_ms = 0;
    frac_ms += dt_ms;
    while (frac_ms >= 1000) {
        frac_ms -= 1000;
        if (g_match.match_state == STATE_PRE_MATCH ||
            g_match.match_state == STATE_PRE_EXTRA_TIME) {
            if (g_match.clock_seconds > 0) {
                g_match.clock_seconds--;
                mark_dirty();
            } else if (g_defaults.auto_start_after_break) {
                // Auto-mode: hand the match phase off immediately when
                // the countdown hits zero.
                if (g_match.match_state == STATE_PRE_MATCH) {
                    g_match.match_state      = STATE_HALF_1;
                    g_match.clock_seconds    = 0;
                    g_match.match_start_unix = (uint32_t)time(nullptr);
                } else {
                    g_match.match_state   = STATE_EXTRA_TIME_1;
                    g_match.clock_seconds = 90u * 60u;
                }
                mark_dirty();
            } else if (g_match.clock_running) {
                // Manual mode: park at 00:00 and stop the clock so the
                // operator's GO (INTENT_RESUME) actually promotes the
                // state machine. Only flag dirty once, when we cross
                // the running → stopped edge.
                g_match.clock_running = false;
                mark_dirty();
            }
        } else {
            g_match.clock_seconds++;
            // Cap at 9999 s (166:39) — covers all of regulation (90:00)
            // + full extra time (105:00 → 120:00) + a healthy stoppage
            // margin without ever wrapping the uint16. The GAZ4 board
            // physically can't show > 99:59 so its frame builder still
            // does mm % 100, but the controller/wallbox displays show
            // the real number so the operator can read e.g. 105:23.
            if (g_match.clock_seconds >= 9999) g_match.clock_seconds = 9999;
            mark_dirty();
        }
    }
}

// ============================================================================
//  Wallbox-mode bookkeeping.
// ============================================================================
void init() {
    zero_match();
    load_defaults();
    load_history();
    if (load_match_if_exists()) {
        g_wb_mode = WB_MATCH_LIVE;
    }
    g_screen_entered_ms = millis();
}

void set_wb_mode(WallboxMode m) {
    if (g_wb_mode != m) {
        g_wb_mode = m;
        g_screen_entered_ms = millis();
    }
}
WallboxMode wb_mode() { return g_wb_mode; }

void note_radio_link(bool linked, int8_t rssi) {
    g_radio_linked = linked;
    g_last_rssi    = rssi;
}
void note_intent_received() { g_last_intent_ms = millis(); }
void note_gaz4_tx()         { g_last_tx_ms = millis(); }

void set_pairing_mode(bool on)   { g_pairing_mode = on; }
void set_paired_peer_count(uint8_t n) { g_paired_count = n; }

void cycle_info_page() { g_info_page = (g_info_page + 1) % 3; }
void set_polarity_ok(bool ok) { g_polarity_ok = ok; }

// --- Snapshot read --------------------------------------------------------
const Match& peek_match() { return g_match; }

Snapshot snapshot() {
    Snapshot s = {};
    s.wb_mode            = g_wb_mode;
    s.radio_linked       = g_radio_linked;
    s.last_rssi          = g_last_rssi;
    s.polarity_ok        = g_polarity_ok;
    s.pairing_mode       = g_pairing_mode;
    s.paired_peer_count  = g_paired_count;
    s.info_page          = g_info_page;
    s.last_intent_ms     = g_last_intent_ms;
    s.last_tx_ms         = g_last_tx_ms;
    s.match              = g_match;
    s.defaults           = g_defaults;
    s.history_count      = g_history_count;

    s.match_state        = g_match.match_state;
    s.home_score         = g_match.home_score_real % 10;
    s.away_score         = g_match.away_score_real % 10;
    s.clock_seconds      = g_match.clock_seconds;
    s.clock_running      = g_match.clock_running;
    s.can_undo           = (g_undo_count > 0);
    return s;
}

// --- Persistence flush ----------------------------------------------------
void persist_match_if_dirty() {
    if (!g_match_dirty) return;
    const uint32_t now = millis();
    if (now - g_last_persist_ms < MATCH_PERSIST_DEBOUNCE_MS) return;
    persist_match_now();
}

bool has_persisted_match() {
    Preferences p;
    p.begin(NVS_NAMESPACE, true);
    const bool active = p.getBool("active", false);
    p.end();
    return active;
}

void clear_persisted_match() {
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    p.putBool("active", false);
    p.end();
}

// --- Payload builders -----------------------------------------------------
void fill_state_payload(StatePayload& out, bool pairing_mode, bool gaz4_ok) {
    memset(&out, 0, sizeof(out));
    out.match_state          = g_match.match_state;
    out.preset_idx           = g_match.preset_idx;
    out.home_score_real      = g_match.home_score_real;
    out.away_score_real      = g_match.away_score_real;
    out.clock_seconds        = g_match.clock_seconds;
    out.half1_end_seconds    = g_match.half1_end_seconds;
    out.half2_end_seconds    = g_match.half2_end_seconds;
    out.pre_match_seconds    = (g_match.match_state == STATE_PRE_MATCH)
                                 ? g_match.clock_seconds : 0;
    out.pause_target_seconds = g_match.pause_target_seconds;
    out.pk_home_kicks        = g_match.pk_home_kicks;
    out.pk_away_kicks        = g_match.pk_away_kicks;
    out.pk_home_taken        = g_match.pk_home_taken;
    out.pk_away_taken        = g_match.pk_away_taken;
    out.stoppage_minutes     = g_match.stoppage_minutes;
    out.goal_count           = g_match.goal_count;
    out.history_count        = g_history_count;
    out.pk_home_first        = g_match.pk_home_first ? 1 : 0;
    out.extra_time_played    = g_match.extra_time_played ? 1 : 0;

    out.flags = 0;
    if (g_match.clock_running) out.flags |= FLAG_CLOCK_RUNNING;
    if (pairing_mode)          out.flags |= FLAG_PAIRING_MODE;
    if (g_undo_count > 0)      out.flags |= FLAG_CAN_UNDO;
    if (gaz4_ok)               out.flags |= FLAG_GAZ4_OK;

    strncpy(out.opponent, g_match.opponent, sizeof(out.opponent) - 1);
}

void fill_defaults_payload(DefaultsPayload& out) {
    memset(&out, 0, sizeof(out));
    out.half_minutes            = g_defaults.half_minutes;
    out.pause_minutes           = g_defaults.pause_minutes;
    out.auto_blank_after_match  = g_defaults.auto_blank_after_match ? 1 : 0;
    out.prompt_scorer_on_goal   = g_defaults.prompt_scorer_on_goal  ? 1 : 0;
    out.auto_start_after_break  = g_defaults.auto_start_after_break ? 1 : 0;
}

} // namespace wb_state

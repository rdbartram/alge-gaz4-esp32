// ============================================================================
//  UI dispatcher + all screens for the M5Stack Core2.
//  Touch-driven. M5Unified handles screen + touch + battery + vibration.
// ============================================================================
#include "ui.h"
#include "ui_helpers.h"
#include "config.h"
#include "state.h"
#include "matchmodes.h"
#include "espnow_client.h"
#include "crest.h"

#include <M5Unified.h>
#include <Preferences.h>
#include <WiFi.h>
#include <stdio.h>
#include <string.h>

namespace ui {

// ----- Module state -------------------------------------------------------
static Screen   g_screen        = SCREEN_SPLASH;
static Screen   g_screen_return = SCREEN_SETUP;  // where to return from settings/picker
static uint32_t g_screen_entered_ms = 0;
static bool     g_invalidate    = true;
static uint32_t g_last_rendered_ms = 0;

// SETUP-screen working state
static uint8_t  g_setup_preset_idx = 0;
static char     g_setup_opponent[24] = "Gegner";

// PENALTY active team marker (true = home next)
static bool     g_pk_home_turn = true;

// Text input state
static char     g_input_buf[24] = "";
static uint8_t  g_input_len = 0;
static char     g_input_label[24] = "Eingabe";
static void   (*g_input_callback)(const char*) = nullptr;

// Preset picker callback
static void   (*g_picker_callback)(uint8_t idx) = nullptr;

// Numpad popup state
static int     g_numpad_value = 0;
static int     g_numpad_min   = 0;
static int     g_numpad_max   = 99;
static char    g_numpad_label[24] = "Wert";
static void  (*g_numpad_callback)(int) = nullptr;

// History scroll
static uint8_t g_history_scroll = 0;

// Editable match defaults (NVS-backed)
struct Defaults {
    uint8_t half_minutes = DEFAULT_HALF_LEN_MIN;
    uint8_t pause_minutes = DEFAULT_PAUSE_LEN_MIN;
    bool    auto_blank_after_match = true;
    bool    prompt_scorer_on_goal = false;
};
static Defaults g_defaults = {};

static void load_defaults() {
    Preferences p;
    p.begin(NVS_NAMESPACE, true);
    g_defaults.half_minutes = p.getUChar("d_half", DEFAULT_HALF_LEN_MIN);
    g_defaults.pause_minutes = p.getUChar("d_pause", DEFAULT_PAUSE_LEN_MIN);
    g_defaults.auto_blank_after_match = p.getBool("d_autoblank", true);
    g_defaults.prompt_scorer_on_goal = p.getBool("d_scorer", false);
    p.end();
}
static void save_defaults() {
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    p.putUChar("d_half", g_defaults.half_minutes);
    p.putUChar("d_pause", g_defaults.pause_minutes);
    p.putBool("d_autoblank", g_defaults.auto_blank_after_match);
    p.putBool("d_scorer", g_defaults.prompt_scorer_on_goal);
    p.end();
}

// Long-press tracking for match-screen +/-.
// When the user holds a delta button, fire repeatedly after 500ms hold, at
// 5Hz (so ~5 increments per second).
struct LongPress {
    int16_t  zone_x, zone_y, zone_w, zone_h;
    uint32_t start_ms;
    uint32_t last_fire_ms;
    int8_t   delta;
    bool     home_side;
    bool     active;
};
static LongPress g_lp = {};

// Connection-lost banner state
static uint32_t g_link_lost_since_ms = 0;
static bool     g_link_lost_shown = false;

// Forward decls
static void draw();
static void handle_touch(int16_t x, int16_t y);
static void draw_splash();
static void draw_setup();
static void handle_setup_touch(int16_t x, int16_t y);
static void draw_match();
static void handle_match_touch(int16_t x, int16_t y);
static void draw_halftime();
static void handle_halftime_touch(int16_t x, int16_t y);
static void draw_ended();
static void handle_ended_touch(int16_t x, int16_t y);
static void draw_penalty();
static void handle_penalty_touch(int16_t x, int16_t y);
static void draw_penalty_toss();
static void handle_penalty_toss_touch(int16_t x, int16_t y);
static void draw_settings();
static void handle_settings_touch(int16_t x, int16_t y);
static void draw_preset_picker();
static void handle_preset_picker_touch(int16_t x, int16_t y);
static void draw_text_input();
static void handle_text_input_touch(int16_t x, int16_t y);
static void open_text_input(const char* label, const char* initial,
                            void (*cb)(const char*));
static void open_preset_picker(void (*cb)(uint8_t));
static void draw_numpad();
static void handle_numpad_touch(int16_t x, int16_t y);
static void open_numpad(const char* label, int initial, int lo, int hi,
                        void (*cb)(int));
static void draw_history();
static void handle_history_touch(int16_t x, int16_t y);
static void draw_defaults();
static void handle_defaults_touch(int16_t x, int16_t y);
static void draw_link_banner();
static void check_long_press();

// ----- Public ------------------------------------------------------------
void begin() {
    g_screen = SCREEN_SPLASH;
    g_screen_entered_ms = millis();
    g_invalidate = true;
    load_defaults();
    M5.Display.fillScreen(COLOR_BG_DARK);
}

void invalidate() { g_invalidate = true; }
Screen current() { return g_screen; }

void go(Screen s) {
    g_screen = s;
    g_screen_entered_ms = millis();
    g_invalidate = true;
}

void vibe_short()  { M5.Power.setVibration(200); delay(VIB_SCORE_PLUS_MS); M5.Power.setVibration(0); }
void vibe_pause()  { M5.Power.setVibration(180); delay(VIB_PAUSE_MS); M5.Power.setVibration(0); }
void vibe_double() {
    M5.Power.setVibration(200); delay(30); M5.Power.setVibration(0);
    delay(100);
    M5.Power.setVibration(200); delay(30); M5.Power.setVibration(0);
}
void vibe_long()   { M5.Power.setVibration(220); delay(500); M5.Power.setVibration(0); }

// Rapid 4x cycle for connection-lost alert.
static void vibe_alert() {
    for (int i = 0; i < 4; ++i) {
        M5.Power.setVibration(220); delay(50); M5.Power.setVibration(0); delay(50);
    }
}

void tick() {
    M5.update();
    const uint32_t now = millis();

    // Pump touch input
    auto touch = M5.Touch.getDetail();
    if (touch.wasPressed()) {
        handle_touch(touch.x, touch.y);
    }
    if (touch.wasReleased()) {
        g_lp.active = false;
    }
    check_long_press();
    draw_link_banner();  // overlay on top of any screen

    // Screen-specific clock-driven invalidations.
    if (g_screen == SCREEN_SPLASH) {
        if (now - g_screen_entered_ms > 2000) {
            // Splash done. If a paused match was persisted, prompt resume.
            if (state::has_persisted_match() && state::load_if_exists()) {
                go(SCREEN_MATCH);
            } else {
                go(SCREEN_SETUP);
            }
        }
    }
    if (g_screen == SCREEN_MATCH ||
        g_screen == SCREEN_HALFTIME ||
        g_screen == SCREEN_ENDED) {
        if (now - g_last_rendered_ms > 500) g_invalidate = true;
    }

    if (g_invalidate) {
        g_invalidate = false;
        g_last_rendered_ms = now;
        draw();
    }

    // ENDED auto-blank after 5 min
    if (g_screen == SCREEN_ENDED &&
        now - g_screen_entered_ms > ENDED_AUTO_BLANK_MS) {
        espnow_client::send_command(CMD_BLANK, 0);
        state::reset();
        go(SCREEN_SETUP);
    }
}

// ----- Dispatcher --------------------------------------------------------
static void draw() {
    M5.Display.fillScreen(COLOR_BG_DARK);
    switch (g_screen) {
    case SCREEN_SPLASH:        draw_splash(); break;
    case SCREEN_SETUP:         draw_setup(); break;
    case SCREEN_MATCH:         draw_match(); break;
    case SCREEN_HALFTIME:      draw_halftime(); break;
    case SCREEN_ENDED:         draw_ended(); break;
    case SCREEN_PENALTY_TOSS:  draw_penalty_toss(); break;
    case SCREEN_PENALTY:       draw_penalty(); break;
    case SCREEN_SETTINGS:      draw_settings(); break;
    case SCREEN_PRESET_PICKER: draw_preset_picker(); break;
    case SCREEN_TEXT_INPUT:    draw_text_input(); break;
    case SCREEN_NUMPAD:        draw_numpad(); break;
    case SCREEN_HISTORY:       draw_history(); break;
    case SCREEN_DEFAULTS:      draw_defaults(); break;
    }
}

static void handle_touch(int16_t x, int16_t y) {
    switch (g_screen) {
    case SCREEN_SPLASH:        /* tap to skip */ go(SCREEN_SETUP); break;
    case SCREEN_SETUP:         handle_setup_touch(x, y); break;
    case SCREEN_MATCH:         handle_match_touch(x, y); break;
    case SCREEN_HALFTIME:      handle_halftime_touch(x, y); break;
    case SCREEN_ENDED:         handle_ended_touch(x, y); break;
    case SCREEN_PENALTY_TOSS:  handle_penalty_toss_touch(x, y); break;
    case SCREEN_PENALTY:       handle_penalty_touch(x, y); break;
    case SCREEN_SETTINGS:      handle_settings_touch(x, y); break;
    case SCREEN_PRESET_PICKER: handle_preset_picker_touch(x, y); break;
    case SCREEN_TEXT_INPUT:    handle_text_input_touch(x, y); break;
    case SCREEN_NUMPAD:        handle_numpad_touch(x, y); break;
    case SCREEN_HISTORY:       handle_history_touch(x, y); break;
    case SCREEN_DEFAULTS:      handle_defaults_touch(x, y); break;
    }
    invalidate();
}

// ============================================================================
//  SPLASH
// ============================================================================
static void draw_splash() {
    auto& d = M5.Display;
    // Crest centred-ish near top
    const int cx = DISPLAY_WIDTH / 2;
    if (CREST_WIDTH && CREST_HEIGHT) {
        d.pushImage(cx - CREST_WIDTH / 2, 24, CREST_WIDTH, CREST_HEIGHT, CREST_RGB565);
    } else {
        // Text fallback
        d.fillRoundRect(cx - 70, 24, 140, 80, 8, COLOR_PRIMARY);
        d.setTextColor(COLOR_TEXT);
        d.setFont(&fonts::FreeSansBold18pt7b);
        d.setTextDatum(middle_center);
        d.drawString("FC", cx, 50);
        d.drawString("WAENGI", cx, 84);
    }
    uih::centre_text(cx, 120, BRAND_NAME, COLOR_TEXT, &fonts::FreeSansBold18pt7b);
    uih::centre_text(cx, 152, "Anzeigetafel-System", COLOR_ACCENT, &fonts::FreeSans12pt7b);
    uih::centre_text(cx, 180, "v" FIRMWARE_VERSION " . seit 1967", COLOR_DIM, &fonts::FreeSans9pt7b);

    // progress bar
    const int barx = 40, bary = 210, barw = 240, barh = 10;
    d.drawRoundRect(barx, bary, barw, barh, 4, COLOR_DIM);
    const uint32_t elapsed = millis() - g_screen_entered_ms;
    const int fill = (int)((elapsed * barw) / 2000);
    d.fillRoundRect(barx + 1, bary + 1, fill > barw - 2 ? barw - 2 : fill, barh - 2, 3, COLOR_PRIMARY);
}

// ============================================================================
//  SETUP
// ============================================================================
static uih::Rect g_setup_btn_picker;
static uih::Rect g_setup_btn_team;
static uih::Rect g_setup_btn_start;
static uih::Rect g_setup_btn_countdown;

static void draw_setup() {
    auto& d = M5.Display;
    uih::draw_header("SETUP");

    d.setTextColor(COLOR_TEXT, COLOR_BG_DARK);
    d.setFont(&fonts::FreeSansBold12pt7b);
    d.setTextDatum(top_left);
    d.drawString("MATCH KONFIGURIEREN", 10, 36);

    d.setFont(&fonts::FreeSans9pt7b);
    d.setTextColor(COLOR_DIM, COLOR_BG_DARK);
    d.drawString("Spieltyp:", 10, 64);

    const auto& p = matchmodes::get(g_setup_preset_idx);
    g_setup_btn_picker = uih::draw_button(10, 78, DISPLAY_WIDTH - 20, 32,
                                          p.label, COLOR_BG_DARK, COLOR_TEXT);

    d.setFont(&fonts::FreeSans9pt7b);
    d.setTextColor(COLOR_ACCENT, COLOR_BG_DARK);
    d.setTextDatum(top_center);
    d.drawString(p.sublabel, DISPLAY_WIDTH / 2, 116);

    // Team labels
    d.setTextColor(COLOR_DIM);
    d.setTextDatum(top_left);
    d.drawString("Heim:", 10, 140);
    d.setTextColor(COLOR_TEXT);
    d.setFont(&fonts::FreeSansBold12pt7b);
    d.drawString(BRAND_HOME_TEAM, 70, 138);

    d.setFont(&fonts::FreeSans9pt7b);
    d.setTextColor(COLOR_DIM);
    d.drawString("Gegner:", 10, 165);
    g_setup_btn_team = uih::draw_button(70, 160, DISPLAY_WIDTH - 80, 26,
                                        g_setup_opponent, COLOR_BG_DARK, COLOR_TEXT);

    g_setup_btn_start = uih::draw_button(10, 198, 200, 36,
                                         "MATCH STARTEN >",
                                         COLOR_PRIMARY, COLOR_TEXT, true);
    g_setup_btn_countdown = uih::draw_button(220, 198, DISPLAY_WIDTH - 230, 36,
                                             "Countdown",
                                             COLOR_BG_DARK, COLOR_ACCENT);
}

static void setup_apply_preset(uint8_t idx) { g_setup_preset_idx = idx; }
static void setup_apply_team(const char* t)  {
    strncpy(g_setup_opponent, t, sizeof(g_setup_opponent) - 1);
    g_setup_opponent[sizeof(g_setup_opponent) - 1] = '\0';
}

static void handle_setup_touch(int16_t x, int16_t y) {
    if (uih::point_in(g_setup_btn_picker, x, y)) {
        open_preset_picker(setup_apply_preset);
    } else if (uih::point_in(g_setup_btn_team, x, y)) {
        open_text_input("Gegner-Name", g_setup_opponent, setup_apply_team);
    } else if (uih::point_in(g_setup_btn_start, x, y)) {
        state::start_match(g_setup_preset_idx, g_setup_opponent);
        espnow_client::send_state_now();
        vibe_long();
        go(SCREEN_MATCH);
    } else if (uih::point_in(g_setup_btn_countdown, x, y)) {
        // Open numpad to choose countdown length, then enter pre-match.
        open_numpad("Countdown (Min.)", 5, 1, 15, [](int minutes) {
            state::start_match(g_setup_preset_idx, g_setup_opponent);
            state::start_pre_match((uint16_t)(minutes * 60));
            espnow_client::send_state_now();
            ui::go(ui::SCREEN_MATCH);
        });
    }
}

// ============================================================================
//  MATCH (live)
// ============================================================================
static uih::Rect g_match_btn_h_inc, g_match_btn_h_dec, g_match_btn_a_inc, g_match_btn_a_dec;
static uih::Rect g_match_btn_pause, g_match_btn_halftime, g_match_btn_clock;
static uih::Rect g_match_score_home, g_match_score_away;
static uih::Rect g_match_btn_settings;
static uih::Rect g_match_btn_undo;
static uih::Rect g_match_btn_stoppage;

static const char* match_state_label(MatchState s) {
    switch (s) {
    case STATE_HALF_1:           return "1. HALBZEIT . LAEUFT";
    case STATE_PAUSED_H1:        return "1. HALBZEIT . PAUSIERT";
    case STATE_HALF_2:           return "2. HALBZEIT . LAEUFT";
    case STATE_PAUSED_H2:        return "2. HALBZEIT . PAUSIERT";
    case STATE_EXTRA_TIME_1:     return "VERLAENGERUNG 1.HZ";
    case STATE_EXTRA_TIME_2:     return "VERLAENGERUNG 2.HZ";
    case STATE_PAUSED_ET:        return "VERLAENGERUNG . PAUSE";
    case STATE_PRE_MATCH:        return "COUNTDOWN ZUM ANSTOSS";
    default:                     return "MATCH";
    }
}

static void draw_match() {
    const auto& s = state::peek();
    auto& d = M5.Display;
    uih::draw_header(s.clock_running ? "LAEUFT" : "PAUSIERT");

    // Top chips: stoppage top-right, undo top-left. Compact so they sit
    // beside the HEIM/GAST labels without crashing into them.
    char stp_label[8];
    snprintf(stp_label, sizeof(stp_label), "+%u", s.stoppage_minutes);
    const bool near_half_end = (s.clock_seconds > 44u * 60u && s.clock_seconds < 60u * 60u) ||
                               (s.clock_seconds > 89u * 60u);
    const uint16_t stp_color = (s.stoppage_minutes > 0 && near_half_end)
                                   ? COLOR_ACCENT : COLOR_DIM;
    g_match_btn_stoppage = uih::draw_button(DISPLAY_WIDTH - 38, 32, 32, 16,
                                            stp_label, COLOR_BG_DARK, stp_color);
    if (state::can_undo()) {
        g_match_btn_undo = uih::draw_button(6, 32, 28, 16, "<<",
                                            COLOR_BG_DARK, COLOR_ACCENT);
    } else {
        g_match_btn_undo = {0, 0, 0, 0};
    }

    // Labels symmetric around centre (HEIM at x=60, GAST at x=260 — both
    // 60 px from their respective edges).
    uih::centre_text( 60, 50, "HEIM", COLOR_DIM, &fonts::FreeSans12pt7b);
    uih::centre_text(260, 50, "GAST", COLOR_DIM, &fonts::FreeSans12pt7b);

    // Scores
    uih::draw_score_value( 60, 100, s.home_score_real, COLOR_TEXT);
    uih::draw_score_value(260, 100, s.away_score_real, COLOR_TEXT);
    g_match_score_home = { 30, 60,  60, 80};
    g_match_score_away = {230, 60,  60, 80};

    // +/- buttons (symmetric — clock occupies the middle x=110..210 gap)
    g_match_btn_h_dec = uih::draw_button( 10, 134, 40, 30, "-", COLOR_BG_DARK, COLOR_WARN);
    g_match_btn_h_inc = uih::draw_button( 70, 134, 40, 30, "+", COLOR_BG_DARK, COLOR_SUCCESS);
    g_match_btn_a_dec = uih::draw_button(210, 134, 40, 30, "-", COLOR_BG_DARK, COLOR_WARN);
    g_match_btn_a_inc = uih::draw_button(270, 134, 40, 30, "+", COLOR_BG_DARK, COLOR_SUCCESS);

    // Clock (tap to edit)
    g_match_btn_clock = {110, 130, 100, 40};
    uih::draw_clock(DISPLAY_WIDTH / 2, 152, s.clock_seconds,
                    s.clock_running ? COLOR_ACCENT : COLOR_DIM);

    // State label — when a score exceeds 9, fold a "Tafel X:Y" note into
    // the line and recolour to warn. Avoids a stacked second row that
    // would collide with PAUSE at y=198.
    char state_buf[64];
    if (s.home_score_real > 9 || s.away_score_real > 9) {
        snprintf(state_buf, sizeof(state_buf), "%s  -  Tafel %u:%u",
                 match_state_label(s.match_state),
                 s.home_score_real % 10, s.away_score_real % 10);
        uih::centre_text(DISPLAY_WIDTH / 2, 180, state_buf, COLOR_WARN,
                         &fonts::FreeSans9pt7b);
    } else {
        uih::centre_text(DISPLAY_WIDTH / 2, 180,
                         match_state_label(s.match_state),
                         s.clock_running ? COLOR_SUCCESS : COLOR_WARN,
                         &fonts::FreeSans9pt7b);
    }

    // PAUSE/RESUME button + HALBZEIT/MATCH ENDE
    g_match_btn_pause = uih::draw_button(10, 198, 140, 36,
        s.clock_running ? "PAUSE" : "WEITER",
        s.clock_running ? COLOR_WARN : COLOR_SUCCESS, COLOR_BG_DARK, true);

    const bool first_half =
        s.match_state == STATE_HALF_1 || s.match_state == STATE_PAUSED_H1;
    const char* right_label = first_half ? "HALBZEIT" : "MATCH ENDE";
    g_match_btn_halftime = uih::draw_button(170, 198, 140, 36, right_label,
        COLOR_PRIMARY, COLOR_TEXT, true);

    // Settings cog at top-right corner of header (tappable region)
    g_match_btn_settings = { DISPLAY_WIDTH - 24, 4, 20, 20 };
}

// Numpad callbacks for score and clock edits.
static void numpad_set_home(int v) {
    state::score_set((uint8_t)v, state::peek().away_score_real);
    state::save();
    espnow_client::send_state_now();
}
static void numpad_set_away(int v) {
    state::score_set(state::peek().home_score_real, (uint8_t)v);
    state::save();
    espnow_client::send_state_now();
}
static void numpad_set_clock(int total_seconds) {
    state::get().clock_seconds = (uint16_t)total_seconds;
    state::save();
    espnow_client::send_state_now();
}

static void arm_long_press(int16_t x, int16_t y, int16_t w, int16_t h,
                           int8_t delta, bool home_side) {
    g_lp.zone_x = x; g_lp.zone_y = y; g_lp.zone_w = w; g_lp.zone_h = h;
    g_lp.start_ms = millis();
    g_lp.last_fire_ms = 0;
    g_lp.delta = delta;
    g_lp.home_side = home_side;
    g_lp.active = true;
}

static void handle_match_touch(int16_t x, int16_t y) {
    const auto& s = state::peek();
    if (uih::point_in(g_match_btn_h_inc, x, y)) {
        state::score_home_delta(+1);
        state::save();
        espnow_client::send_state_now();
        vibe_short();
        arm_long_press(g_match_btn_h_inc.x, g_match_btn_h_inc.y,
                       g_match_btn_h_inc.w, g_match_btn_h_inc.h, +1, true);
        if (g_defaults.prompt_scorer_on_goal) {
            open_numpad("Torschuetze Heim (Trikot-Nr.)", 0, 0, 99,
                        [](int j) { state::register_goal(true, (uint8_t)j); });
        }
    } else if (uih::point_in(g_match_btn_h_dec, x, y)) {
        state::score_home_delta(-1);
        state::save();
        espnow_client::send_state_now();
        vibe_double();
        arm_long_press(g_match_btn_h_dec.x, g_match_btn_h_dec.y,
                       g_match_btn_h_dec.w, g_match_btn_h_dec.h, -1, true);
    } else if (uih::point_in(g_match_btn_a_inc, x, y)) {
        state::score_away_delta(+1);
        state::save();
        espnow_client::send_state_now();
        vibe_short();
        arm_long_press(g_match_btn_a_inc.x, g_match_btn_a_inc.y,
                       g_match_btn_a_inc.w, g_match_btn_a_inc.h, +1, false);
        if (g_defaults.prompt_scorer_on_goal) {
            open_numpad("Torschuetze Gast (Trikot-Nr.)", 0, 0, 99,
                        [](int j) { state::register_goal(false, (uint8_t)j); });
        }
    } else if (uih::point_in(g_match_btn_a_dec, x, y)) {
        state::score_away_delta(-1);
        state::save();
        espnow_client::send_state_now();
        vibe_double();
        arm_long_press(g_match_btn_a_dec.x, g_match_btn_a_dec.y,
                       g_match_btn_a_dec.w, g_match_btn_a_dec.h, -1, false);
    } else if (uih::point_in(g_match_score_home, x, y)) {
        open_numpad("HEIM-Stand", s.home_score_real, 0, 99, numpad_set_home);
    } else if (uih::point_in(g_match_score_away, x, y)) {
        open_numpad("GAST-Stand", s.away_score_real, 0, 99, numpad_set_away);
    } else if (uih::point_in(g_match_btn_clock, x, y)) {
        open_numpad("Uhr (Sek.)", s.clock_seconds, 0, 5999, numpad_set_clock);
    } else if (uih::point_in(g_match_btn_pause, x, y)) {
        if (s.clock_running) state::pause(); else state::resume();
        espnow_client::send_state_now();
        vibe_pause();
    } else if (uih::point_in(g_match_btn_halftime, x, y)) {
        const bool first_half =
            s.match_state == STATE_HALF_1 || s.match_state == STATE_PAUSED_H1;
        if (first_half) {
            state::start_halftime();
            espnow_client::send_state_now();
            vibe_long();
            go(SCREEN_HALFTIME);
        } else {
            state::end_match();
            espnow_client::send_state_now();
            vibe_long();
            go(SCREEN_ENDED);
        }
    } else if (uih::point_in(g_match_btn_settings, x, y)) {
        g_screen_return = SCREEN_MATCH;
        go(SCREEN_SETTINGS);
    } else if (state::can_undo() && uih::point_in(g_match_btn_undo, x, y)) {
        state::undo();
        espnow_client::send_state_now();
        vibe_double();
    } else if (uih::point_in(g_match_btn_stoppage, x, y)) {
        open_numpad("Nachspielzeit (Min.)", s.stoppage_minutes, 0, 15,
                    [](int v) { state::set_stoppage_minutes((uint8_t)v); });
    }
}

// ============================================================================
//  HALFTIME
// ============================================================================
static uih::Rect g_ht_btn_start_h2;
static uih::Rect g_ht_btn_resume_at_45;

static void draw_halftime() {
    const auto& s = state::peek();
    auto& d = M5.Display;
    uih::draw_header("HALBZEITPAUSE");

    uih::centre_text(DISPLAY_WIDTH / 2, 36, "ENDSTAND 1. HALBZEIT",
                     COLOR_ACCENT, &fonts::FreeSansBold12pt7b);

    uih::draw_score_value( 60,  92, s.home_score_real, COLOR_TEXT);
    uih::draw_score_value(260,  92, s.away_score_real, COLOR_TEXT);
    uih::draw_clock(DISPLAY_WIDTH / 2, 92, s.half1_end_seconds, COLOR_DIM);

    // Pause countdown — measured from when this screen was entered.
    const uint32_t elapsed_ms = millis() - g_screen_entered_ms;
    const int32_t remain = (int32_t)(DEFAULT_PAUSE_LEN_MIN * 60) - (int32_t)(elapsed_ms / 1000);
    const uint16_t cmin = remain > 0 ? remain / 60 : 0;
    const uint16_t csec = remain > 0 ? remain % 60 : 0;
    uih::centre_text(DISPLAY_WIDTH / 2, 136, "Pausenzeit",
                     COLOR_DIM, &fonts::FreeSans9pt7b);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02u:%02u", cmin, csec);
    uih::centre_text(DISPLAY_WIDTH / 2, 156, buf,
                     (remain < 60 && (millis() / 500) % 2)
                         ? COLOR_WARN : COLOR_TEXT,
                     &fonts::FreeSansBold18pt7b);

    g_ht_btn_resume_at_45 = uih::draw_button(10, 198, 150, 36,
        "2.HZ ab 45:00", COLOR_DIM, COLOR_TEXT);
    g_ht_btn_start_h2 = uih::draw_button(170, 198, 140, 36,
        "2.HZ STARTEN", COLOR_PRIMARY, COLOR_TEXT, true);
}

static void handle_halftime_touch(int16_t x, int16_t y) {
    if (uih::point_in(g_ht_btn_resume_at_45, x, y)) {
        state::start_half_2(true);
        espnow_client::send_state_now();
        vibe_long();
        go(SCREEN_MATCH);
    } else if (uih::point_in(g_ht_btn_start_h2, x, y)) {
        state::start_half_2(false);  // continues from half1_end_seconds
        espnow_client::send_state_now();
        vibe_long();
        go(SCREEN_MATCH);
    }
}

// ============================================================================
//  ENDED
// ============================================================================
static uih::Rect g_end_btn_new;
static uih::Rect g_end_btn_blank;
static uih::Rect g_end_btn_extra;
static uih::Rect g_end_btn_penalty;

static void draw_ended() {
    const auto& s = state::peek();
    auto& d = M5.Display;
    uih::draw_header("ENDSTAND");

    uih::draw_score_value( 60,  78, s.home_score_real, COLOR_TEXT);
    uih::draw_score_value(260,  78, s.away_score_real, COLOR_TEXT);
    uih::draw_clock(DISPLAY_WIDTH / 2, 78, s.clock_seconds, COLOR_DIM);

    char vs[40];
    snprintf(vs, sizeof(vs), "%s  vs.  %s", BRAND_HOME_TEAM, s.opponent);
    uih::centre_text(DISPLAY_WIDTH / 2, 130, vs, COLOR_DIM, &fonts::FreeSans9pt7b);

    g_end_btn_new   = uih::draw_button(10,  150, 150, 32, "Neues Match",   COLOR_BG_DARK, COLOR_TEXT);
    g_end_btn_blank = uih::draw_button(170, 150, 140, 32, "Tafel loeschen", COLOR_BG_DARK, COLOR_WARN);

    const bool draw_tied = (s.home_score_real == s.away_score_real);
    g_end_btn_extra = uih::draw_button(10, 188, 150, 32, "Verlaengerung",
        draw_tied ? COLOR_PRIMARY : COLOR_DIM,
        draw_tied ? COLOR_TEXT    : COLOR_DIM);
    g_end_btn_penalty = uih::draw_button(170, 188, 140, 32, "Penaltys",
        draw_tied ? COLOR_PRIMARY : COLOR_DIM,
        draw_tied ? COLOR_TEXT    : COLOR_DIM);
}

static void handle_ended_touch(int16_t x, int16_t y) {
    const auto& s = state::peek();
    if (uih::point_in(g_end_btn_new, x, y)) {
        state::reset();
        espnow_client::send_command(CMD_BLANK, 0);
        go(SCREEN_SETUP);
    } else if (uih::point_in(g_end_btn_blank, x, y)) {
        espnow_client::send_command(CMD_BLANK, 0);
    } else if (uih::point_in(g_end_btn_extra, x, y) &&
               s.home_score_real == s.away_score_real) {
        state::start_extra_time_1();
        espnow_client::send_state_now();
        go(SCREEN_MATCH);
    } else if (uih::point_in(g_end_btn_penalty, x, y) &&
               s.home_score_real == s.away_score_real) {
        // Operator chooses who shoots first.
        go(SCREEN_PENALTY_TOSS);
    }
}

// ============================================================================
//  PENALTY TOSS — choose who shoots first.
// ============================================================================
static uih::Rect g_pkt_btn_heim, g_pkt_btn_gast, g_pkt_btn_back;

static void draw_penalty_toss() {
    auto& d = M5.Display;
    uih::draw_header("PENALTY-TOSS");

    uih::centre_text(DISPLAY_WIDTH / 2, 44,
                     "WER SCHIESST ZUERST?", COLOR_TEXT,
                     &fonts::FreeSansBold18pt7b);
    uih::centre_text(DISPLAY_WIDTH / 2, 72,
                     "Münze werfen, dann hier auswählen",
                     COLOR_DIM, &fonts::FreeSans9pt7b);

    g_pkt_btn_heim = uih::draw_button(20,  100, 130, 80,
                                      "HEIM", COLOR_SUCCESS, COLOR_BG_DARK, true);
    g_pkt_btn_gast = uih::draw_button(170, 100, 130, 80,
                                      "GAST", COLOR_PRIMARY, COLOR_TEXT, true);

    g_pkt_btn_back = uih::draw_button(10, 200, DISPLAY_WIDTH - 20, 30,
                                      "< Abbrechen",
                                      COLOR_BG_DARK, COLOR_WARN);
}

static void handle_penalty_toss_touch(int16_t x, int16_t y) {
    bool home_first = false;
    bool start = false;
    if (uih::point_in(g_pkt_btn_heim, x, y))      { home_first = true;  start = true; }
    else if (uih::point_in(g_pkt_btn_gast, x, y)) { home_first = false; start = true; }
    else if (uih::point_in(g_pkt_btn_back, x, y)) { go(SCREEN_ENDED); return; }

    if (start) {
        state::start_penalty_shootout();
        g_pk_home_turn = home_first;
        espnow_client::send_state_now();
        vibe_long();
        go(SCREEN_PENALTY);
    }
}

// ============================================================================
//  PENALTY SHOOTOUT
// ============================================================================
static uih::Rect g_pk_btn_home_score, g_pk_btn_home_miss;
static uih::Rect g_pk_btn_away_score, g_pk_btn_away_miss;
static uih::Rect g_pk_btn_end;

static void draw_pk_kick_row(int16_t y, uint8_t taken, uint8_t mask) {
    auto& d = M5.Display;
    const int xs = 80;        // first kick at x=80, after the side label
    const int box_w = 26;
    const int step  = 32;
    for (int i = 0; i < 5; ++i) {
        const int cx = xs + i * step;
        d.drawRoundRect(cx, y, box_w, 26, 3, COLOR_DIM);
        if (i < taken) {
            const bool scored = (mask & (1u << i)) != 0;
            d.fillRoundRect(cx + 2, y + 2, box_w - 4, 22, 2,
                            scored ? COLOR_SUCCESS : COLOR_ERROR);
            d.setTextColor(COLOR_TEXT);
            d.setFont(&fonts::FreeSansBold12pt7b);
            d.setTextDatum(middle_center);
            d.drawString(scored ? "+" : "X", cx + box_w / 2, y + 13);
        } else {
            // Not yet taken — dim placeholder so the row stays visually
            // uniform with the taken kicks.
            d.fillRoundRect(cx + 2, y + 2, box_w - 4, 22, 2, 0x2104);
            d.setTextColor(COLOR_DIM);
            d.setFont(&fonts::FreeSansBold12pt7b);
            d.setTextDatum(middle_center);
            d.drawString(".", cx + box_w / 2, y + 13);
        }
    }
}

static void draw_penalty() {
    const auto& s = state::peek();
    auto& d = M5.Display;
    uih::draw_header("PENALTYS");

    // Compact scoreline: HOME  —  AWAY, with labels below.
    d.setFont(&fonts::FreeSansBold24pt7b);
    d.setTextColor(COLOR_TEXT, COLOR_BG_DARK);
    d.setTextDatum(middle_center);
    char hb[4], ab[4];
    snprintf(hb, sizeof(hb), "%u", s.home_score_real);
    snprintf(ab, sizeof(ab), "%u", s.away_score_real);
    d.drawString(hb,                       DISPLAY_WIDTH / 2 - 60, 64);
    d.drawString(ab,                       DISPLAY_WIDTH / 2 + 60, 64);
    d.setTextColor(COLOR_DIM);
    d.setFont(&fonts::FreeSansBold18pt7b);
    d.drawString("-",                      DISPLAY_WIDTH / 2,      64);

    uih::centre_text(DISPLAY_WIDTH / 2 - 60, 92, "HEIM",
                     COLOR_DIM, &fonts::FreeSans9pt7b);
    uih::centre_text(DISPLAY_WIDTH / 2 + 60, 92, "GAST",
                     COLOR_DIM, &fonts::FreeSans9pt7b);

    // Per-side rows with the side label inline at the left, and a small
    // ► arrow on whichever side is taking the next kick.
    auto side_label = [&](int16_t row_y, const char* lbl, bool active) {
        d.setTextColor(active ? COLOR_ACCENT : COLOR_DIM, COLOR_BG_DARK);
        d.setFont(&fonts::FreeSansBold9pt7b);
        d.setTextDatum(middle_center);
        d.drawString(lbl, 30, row_y + 13);
        if (active) {
            d.setFont(&fonts::FreeSansBold12pt7b);
            d.drawString(">", 60, row_y + 13);
        }
    };

    side_label(120, "HEIM", g_pk_home_turn);
    draw_pk_kick_row(120, s.pk_home_taken, s.pk_home_kicks);
    side_label(154, "GAST", !g_pk_home_turn);
    draw_pk_kick_row(154, s.pk_away_taken, s.pk_away_kicks);

    // Three equal-width action buttons.
    const int bw = 96, gap = 6;
    if (g_pk_home_turn) {
        g_pk_btn_home_score = uih::draw_button(10,              198, bw, 30, "Tor +",     COLOR_SUCCESS, COLOR_BG_DARK);
        g_pk_btn_home_miss  = uih::draw_button(10 + bw + gap,   198, bw, 30, "Daneben X", COLOR_ERROR,   COLOR_BG_DARK);
    } else {
        g_pk_btn_away_score = uih::draw_button(10,              198, bw, 30, "Tor +",     COLOR_SUCCESS, COLOR_BG_DARK);
        g_pk_btn_away_miss  = uih::draw_button(10 + bw + gap,   198, bw, 30, "Daneben X", COLOR_ERROR,   COLOR_BG_DARK);
    }
    g_pk_btn_end = uih::draw_button(10 + 2*(bw + gap), 198, bw, 30, "ENDE", COLOR_PRIMARY, COLOR_TEXT);
}

static void handle_penalty_touch(int16_t x, int16_t y) {
    if (g_pk_home_turn) {
        if (uih::point_in(g_pk_btn_home_score, x, y)) {
            state::register_pk_kick(true, true);
            espnow_client::send_state_now();
            g_pk_home_turn = false;
            vibe_short();
        } else if (uih::point_in(g_pk_btn_home_miss, x, y)) {
            state::register_pk_kick(true, false);
            espnow_client::send_state_now();
            g_pk_home_turn = false;
            vibe_double();
        }
    } else {
        if (uih::point_in(g_pk_btn_away_score, x, y)) {
            state::register_pk_kick(false, true);
            espnow_client::send_state_now();
            g_pk_home_turn = true;
            vibe_short();
        } else if (uih::point_in(g_pk_btn_away_miss, x, y)) {
            state::register_pk_kick(false, false);
            espnow_client::send_state_now();
            g_pk_home_turn = true;
            vibe_double();
        }
    }
    if (uih::point_in(g_pk_btn_end, x, y)) {
        state::end_match();
        espnow_client::send_state_now();
        go(SCREEN_ENDED);
    }
}

// ============================================================================
//  SETTINGS
// ============================================================================
static uih::Rect g_set_btn_repair, g_set_btn_polarity, g_set_btn_exercise;
static uih::Rect g_set_btn_blank, g_set_btn_history, g_set_btn_back;
static uih::Rect g_set_btn_factory, g_set_btn_defaults;

static void draw_settings() {
    auto& d = M5.Display;
    uih::draw_header("EINSTELLUNGEN");

    // Funk row
    d.setTextColor(COLOR_DIM, COLOR_BG_DARK);
    d.setFont(&fonts::FreeSans9pt7b);
    d.setTextDatum(top_left);
    d.drawString("Funk:", 8, 36);
    char buf[40];
    if (espnow_client::is_paired()) {
        const uint8_t* m = espnow_client::paired_mac();
        snprintf(buf, sizeof(buf), "OK %02X:%02X:%02X:%02X:%02X:%02X %ddBm",
                 m[0], m[1], m[2], m[3], m[4], m[5], espnow_client::last_rssi());
        d.setTextColor(COLOR_SUCCESS);
    } else {
        snprintf(buf, sizeof(buf), "Nicht gepaart");
        d.setTextColor(COLOR_WARN);
    }
    d.drawString(buf, 48, 36);

    g_set_btn_repair   = uih::draw_button(  8,  60, 150, 30, "Neu koppeln",      COLOR_BG_DARK, COLOR_TEXT);
    g_set_btn_polarity = uih::draw_button(162, 60, 150, 30, "Polaritaets-Test", COLOR_BG_DARK, COLOR_TEXT);
    g_set_btn_exercise = uih::draw_button(  8,  96, 150, 30, "Segment-Uebung",   COLOR_BG_DARK, COLOR_TEXT);
    g_set_btn_blank    = uih::draw_button(162, 96, 150, 30, "Tafel loeschen",    COLOR_BG_DARK, COLOR_WARN);
    g_set_btn_history  = uih::draw_button(  8, 132, 150, 30, "Match-Verlauf",    COLOR_BG_DARK, COLOR_TEXT);
    g_set_btn_defaults = uih::draw_button(162, 132, 150, 30, "Vorgaben",         COLOR_BG_DARK, COLOR_TEXT);
    g_set_btn_factory  = uih::draw_button(  8, 168, DISPLAY_WIDTH - 16, 24,
                                          "Werkseinstellung (löscht alles)",
                                          COLOR_BG_DARK, COLOR_ERROR);

    // System info
    d.setTextColor(COLOR_DIM, COLOR_BG_DARK);
    d.setFont(&fonts::Font0);
    d.setTextDatum(top_left);
    char sys[80];
    snprintf(sys, sizeof(sys), "Akku %d%%  v%s  MAC %s",
             M5.Power.getBatteryLevel(), FIRMWARE_VERSION,
             WiFi.macAddress().c_str());
    d.drawString(sys, 8, 172);

    // Match history hint
    d.setTextColor(COLOR_DIM);
    d.setFont(&fonts::FreeSans9pt7b);
    char hist[40];
    snprintf(hist, sizeof(hist), "Verlauf: %u Match(es)", state::history_count());
    d.drawString(hist, 8, 188);

    g_set_btn_back = uih::draw_button(10, 204, DISPLAY_WIDTH - 20, 30,
                                      "< ZURUECK", COLOR_PRIMARY, COLOR_TEXT);
}

static void handle_settings_touch(int16_t x, int16_t y) {
    if (uih::point_in(g_set_btn_repair, x, y)) {
        espnow_client::enter_pairing_mode();
    } else if (uih::point_in(g_set_btn_polarity, x, y)) {
        espnow_client::send_command(CMD_POLARITY_TEST, 0);
    } else if (uih::point_in(g_set_btn_exercise, x, y)) {
        espnow_client::send_command(CMD_SEGMENT_EXERCISE, 11);
    } else if (uih::point_in(g_set_btn_blank, x, y)) {
        espnow_client::send_command(CMD_BLANK, 0);
    } else if (uih::point_in(g_set_btn_factory, x, y)) {
        // No confirmation — destructive but rare. NVS wipe + reboot.
        espnow_client::send_command(CMD_FACTORY_RESET, 0);
        state::reset();
        delay(500);
        ESP.restart();
    } else if (uih::point_in(g_set_btn_back, x, y)) {
        go(g_screen_return);
    } else if (uih::point_in(g_set_btn_history, x, y)) {
        g_history_scroll = 0;
        go(SCREEN_HISTORY);
    } else if (uih::point_in(g_set_btn_defaults, x, y)) {
        go(SCREEN_DEFAULTS);
    }
}

// ============================================================================
//  PRESET PICKER overlay
// ============================================================================
static int g_picker_scroll = 0;
static uih::Rect g_picker_items[6];
static uih::Rect g_picker_btn_up, g_picker_btn_down, g_picker_btn_cancel;

static void open_preset_picker(void (*cb)(uint8_t)) {
    g_picker_callback = cb;
    g_picker_scroll = 0;
    g_screen_return = g_screen;
    go(SCREEN_PRESET_PICKER);
}

static void draw_preset_picker() {
    auto& d = M5.Display;
    uih::draw_header("SPIELTYP");

    const int visible = 5;
    for (int i = 0; i < visible; ++i) {
        const int idx = g_picker_scroll + i;
        if (idx >= matchmodes::PRESET_COUNT) break;
        const auto& p = matchmodes::get(idx);
        const int y = 36 + i * 36;
        g_picker_items[i] = uih::draw_button(10, y, DISPLAY_WIDTH - 60, 32,
            p.label, COLOR_BG_DARK, COLOR_TEXT);
    }
    g_picker_btn_up   = uih::draw_button(DISPLAY_WIDTH - 44, 36,  36, 32, "^", COLOR_DIM, COLOR_TEXT);
    g_picker_btn_down = uih::draw_button(DISPLAY_WIDTH - 44, 72,  36, 32, "v", COLOR_DIM, COLOR_TEXT);
    g_picker_btn_cancel = uih::draw_button(10, 220, DISPLAY_WIDTH - 20, 16,
        "Abbrechen", COLOR_BG_DARK, COLOR_WARN);
}

static void handle_preset_picker_touch(int16_t x, int16_t y) {
    if (uih::point_in(g_picker_btn_up, x, y)) {
        if (g_picker_scroll > 0) g_picker_scroll--;
        return;
    }
    if (uih::point_in(g_picker_btn_down, x, y)) {
        if (g_picker_scroll + 5 < matchmodes::PRESET_COUNT) g_picker_scroll++;
        return;
    }
    if (uih::point_in(g_picker_btn_cancel, x, y)) {
        go(g_screen_return);
        return;
    }
    for (int i = 0; i < 5; ++i) {
        if (uih::point_in(g_picker_items[i], x, y)) {
            const int idx = g_picker_scroll + i;
            if (idx < matchmodes::PRESET_COUNT && g_picker_callback) {
                g_picker_callback((uint8_t)idx);
            }
            go(g_screen_return);
            return;
        }
    }
}

// ============================================================================
//  TEXT INPUT overlay (basic ABC grid for opponent name).
// ============================================================================
static uih::Rect g_input_keys[40];
static uih::Rect g_input_btn_back, g_input_btn_clear, g_input_btn_ok;

// Keyboard layout — variable-length strings per cell so UTF-8 umlauts work.
// 4 rows x up to 10 keys each. Empty strings = no key.
static const char* const INPUT_KEYS[4][10] = {
    {"A","B","C","D","E","F","G","H","I","J"},
    {"K","L","M","N","O","P","Q","R","S","T"},
    {"U","V","W","X","Y","Z","\xC3\x84","\xC3\x96","\xC3\x9C","\xC3\x9F"}, // U..Z Ä Ö Ü ß
    {"-",".",",","1","2","3","4","5","6"," "},
};

static void open_text_input(const char* label, const char* initial,
                            void (*cb)(const char*)) {
    strncpy(g_input_label, label, sizeof(g_input_label) - 1);
    g_input_len = 0;
    memset(g_input_buf, 0, sizeof(g_input_buf));
    if (initial) {
        strncpy(g_input_buf, initial, sizeof(g_input_buf) - 1);
        g_input_len = strlen(g_input_buf);
    }
    g_input_callback = cb;
    g_screen_return = g_screen;
    go(SCREEN_TEXT_INPUT);
}

static void draw_text_input() {
    auto& d = M5.Display;
    uih::draw_header(g_input_label);

    // Edit field
    d.fillRoundRect(10, 36, DISPLAY_WIDTH - 20, 30, 4, 0x2104);
    d.drawRoundRect(10, 36, DISPLAY_WIDTH - 20, 30, 4, COLOR_ACCENT);
    d.setTextColor(COLOR_TEXT);
    d.setFont(&fonts::FreeSansBold12pt7b);
    d.setTextDatum(middle_left);
    d.drawString(g_input_buf, 16, 51);

    // Key grid (4 rows x up to 10 keys)
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 10; ++col) {
            const char* k = INPUT_KEYS[row][col];
            if (!k || !*k) {
                g_input_keys[row * 10 + col] = {0,0,0,0};
                continue;
            }
            const int x = 8 + col * 30;
            const int y = 72 + row * 26;
            // Use space label for the space key for visual clarity.
            const char* label = (k[0] == ' ' && k[1] == 0) ? "_" : k;
            g_input_keys[row * 10 + col] = uih::draw_button(x, y, 28, 24,
                label, COLOR_BG_DARK, COLOR_TEXT);
        }
    }
    g_input_btn_back  = uih::draw_button(10,  180, 90, 28, "<-",     COLOR_BG_DARK, COLOR_WARN);
    g_input_btn_clear = uih::draw_button(115, 180, 90, 28, "leeren", COLOR_BG_DARK, COLOR_WARN);
    g_input_btn_ok    = uih::draw_button(220, 180, 90, 28, "OK",     COLOR_SUCCESS, COLOR_BG_DARK);
}

static void handle_text_input_touch(int16_t x, int16_t y) {
    if (uih::point_in(g_input_btn_back, x, y)) {
        // UTF-8 aware: peel back any continuation bytes (0b10xxxxxx) plus
        // one leading byte, so deleting 'Ä' removes both bytes at once.
        while (g_input_len > 0) {
            const uint8_t c = (uint8_t)g_input_buf[--g_input_len];
            g_input_buf[g_input_len] = '\0';
            if ((c & 0xC0) != 0x80) break;  // hit a leading byte
        }
        return;
    }
    if (uih::point_in(g_input_btn_clear, x, y)) {
        memset(g_input_buf, 0, sizeof(g_input_buf));
        g_input_len = 0;
        return;
    }
    if (uih::point_in(g_input_btn_ok, x, y)) {
        if (g_input_callback) g_input_callback(g_input_buf);
        go(g_screen_return);
        return;
    }
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 10; ++col) {
            if (uih::point_in(g_input_keys[row * 10 + col], x, y)) {
                const char* k = INPUT_KEYS[row][col];
                if (!k || !*k) return;
                const size_t klen = strlen(k);
                if (g_input_len + klen + 1 <= sizeof(g_input_buf)) {
                    memcpy(g_input_buf + g_input_len, k, klen);
                    g_input_len += klen;
                    g_input_buf[g_input_len] = '\0';
                }
                return;
            }
        }
    }
}

// ============================================================================
//  NUMPAD popup — used for editing scores and clock.
// ============================================================================
static uih::Rect g_np_digits[10];
static uih::Rect g_np_btn_back, g_np_btn_clear, g_np_btn_ok, g_np_btn_cancel;

static void open_numpad(const char* label, int initial, int lo, int hi,
                        void (*cb)(int)) {
    strncpy(g_numpad_label, label, sizeof(g_numpad_label) - 1);
    g_numpad_label[sizeof(g_numpad_label) - 1] = '\0';
    g_numpad_value = initial;
    g_numpad_min = lo;
    g_numpad_max = hi;
    g_numpad_callback = cb;
    g_screen_return = g_screen;
    go(SCREEN_NUMPAD);
}

static void draw_numpad() {
    auto& d = M5.Display;
    uih::draw_header(g_numpad_label);

    // Value display
    char val[8];
    snprintf(val, sizeof(val), "%d", g_numpad_value);
    d.fillRoundRect(40, 32, DISPLAY_WIDTH - 80, 36, 6, 0x18C3);
    d.drawRoundRect(40, 32, DISPLAY_WIDTH - 80, 36, 6, COLOR_ACCENT);
    uih::centre_text(DISPLAY_WIDTH / 2, 40, val, COLOR_TEXT,
                     &fonts::FreeSansBold18pt7b);

    // 4x3 keypad — compact so the action row at y=210 has clearance,
    // horizontally centred on the canvas.
    const int kw = 58, kh = 26, gap = 4;
    const int grid_w = 3 * kw + 2 * gap;
    const int kx0 = (DISPLAY_WIDTH - grid_w) / 2;
    const int ky0 = 76;
    const char* digits = "123456789";
    for (int i = 0; i < 9; ++i) {
        const int row = i / 3, col = i % 3;
        char ch[2] = { digits[i], 0 };
        g_np_digits[i] = uih::draw_button(
            kx0 + col * (kw + gap), ky0 + row * (kh + gap),
            kw, kh, ch, COLOR_BG_DARK, COLOR_TEXT);
    }
    // bottom row: clear, 0, back
    g_np_btn_clear  = uih::draw_button(kx0,             ky0 + 3 * (kh + gap), kw, kh, "C",  COLOR_BG_DARK, COLOR_WARN);
    g_np_digits[0]  = uih::draw_button(kx0 + (kw+gap),  ky0 + 3 * (kh + gap), kw, kh, "0",  COLOR_BG_DARK, COLOR_TEXT);
    g_np_btn_back   = uih::draw_button(kx0 + 2*(kw+gap),ky0 + 3 * (kh + gap), kw, kh, "<-", COLOR_BG_DARK, COLOR_WARN);

    // Action row at y=210 — keypad ends at y=76 + 3*30 + 26 = 192, so we
    // have an 18px gap which keeps tap targets distinct.
    g_np_btn_cancel = uih::draw_button(10,             210, 130, 24, "Abbrechen",
                                       COLOR_BG_DARK, COLOR_WARN);
    g_np_btn_ok     = uih::draw_button(DISPLAY_WIDTH - 140, 210, 130, 24, "OK",
                                       COLOR_SUCCESS, COLOR_BG_DARK);
}

static void handle_numpad_touch(int16_t x, int16_t y) {
    for (int i = 0; i < 10; ++i) {
        if (uih::point_in(g_np_digits[i], x, y)) {
            const int next = g_numpad_value * 10 + i;
            if (next <= g_numpad_max) g_numpad_value = next;
            return;
        }
    }
    if (uih::point_in(g_np_btn_back, x, y)) {
        g_numpad_value /= 10;
        return;
    }
    if (uih::point_in(g_np_btn_clear, x, y)) {
        g_numpad_value = 0;
        return;
    }
    if (uih::point_in(g_np_btn_cancel, x, y)) {
        g_numpad_callback = nullptr;
        go(g_screen_return);
        return;
    }
    if (uih::point_in(g_np_btn_ok, x, y)) {
        if (g_numpad_value < g_numpad_min) g_numpad_value = g_numpad_min;
        if (g_numpad_value > g_numpad_max) g_numpad_value = g_numpad_max;
        if (g_numpad_callback) g_numpad_callback(g_numpad_value);
        vibe_short();
        go(g_screen_return);
        return;
    }
}

// ============================================================================
//  HISTORY screen — scrollable list of past matches.
// ============================================================================
static uih::Rect g_hist_btn_up, g_hist_btn_down, g_hist_btn_back, g_hist_btn_clear;

static void draw_history() {
    auto& d = M5.Display;
    uih::draw_header("MATCH-VERLAUF");

    const uint8_t n = state::history_count();
    if (n == 0) {
        uih::centre_text(DISPLAY_WIDTH / 2, 100, "Keine Matches gespeichert.",
                         COLOR_DIM, &fonts::FreeSans12pt7b);
    } else {
        d.setTextColor(COLOR_TEXT, COLOR_BG_DARK);
        d.setFont(&fonts::FreeSans9pt7b);
        d.setTextDatum(top_left);
        const int per_page = 5;
        const int row_h = 32;
        for (int i = 0; i < per_page; ++i) {
            const int idx = g_history_scroll + i;
            if (idx >= n) break;
            const auto& h = state::history(idx);
            const int y = HEADER_HEIGHT + 6 + i * row_h;
            d.fillRoundRect(8, y, DISPLAY_WIDTH - 60, row_h - 4, 4, 0x10A2);
            d.drawRoundRect(8, y, DISPLAY_WIDTH - 60, row_h - 4, 4, COLOR_DIM);
            const auto& p = matchmodes::get(h.preset_idx);
            const uint16_t mm = (h.final_clock_seconds / 60) % 100;
            const uint16_t ss = h.final_clock_seconds % 60;
            char line1[40], line2[40];
            snprintf(line1, sizeof(line1), "%u-%u vs %s",
                     h.home_score_real, h.away_score_real, h.opponent);
            snprintf(line2, sizeof(line2), "%s . %02u:%02u",
                     p.label, mm, ss);
            d.setTextColor(COLOR_TEXT);
            d.drawString(line1, 14, y + 4);
            d.setTextColor(COLOR_DIM);
            d.drawString(line2, 14, y + 16);
        }
        g_hist_btn_up   = uih::draw_button(DISPLAY_WIDTH - 44, 36,  36, 32, "^", COLOR_DIM, COLOR_TEXT);
        g_hist_btn_down = uih::draw_button(DISPLAY_WIDTH - 44, 72,  36, 32, "v", COLOR_DIM, COLOR_TEXT);
    }
    g_hist_btn_clear = uih::draw_button(10,  210, 140, 26, "Verlauf loeschen",
                                        COLOR_BG_DARK, COLOR_WARN);
    g_hist_btn_back  = uih::draw_button(170, 210, 140, 26, "< Zurueck",
                                        COLOR_PRIMARY, COLOR_TEXT);
}

static void handle_history_touch(int16_t x, int16_t y) {
    if (uih::point_in(g_hist_btn_up, x, y)) {
        if (g_history_scroll > 0) g_history_scroll--;
        return;
    }
    if (uih::point_in(g_hist_btn_down, x, y)) {
        if (g_history_scroll + 5 < state::history_count()) g_history_scroll++;
        return;
    }
    if (uih::point_in(g_hist_btn_clear, x, y)) {
        Preferences p;
        p.begin(NVS_NAMESPACE, false);
        p.putUChar("hist_n", 0);
        p.end();
        // Reload state's in-memory mirror.
        state::begin();
        g_history_scroll = 0;
        return;
    }
    if (uih::point_in(g_hist_btn_back, x, y)) {
        go(SCREEN_SETTINGS);
    }
}

// ============================================================================
//  DEFAULTS editor — half-length, pause-length, auto-blank toggle.
// ============================================================================
static uih::Rect g_def_half_dec, g_def_half_inc;
static uih::Rect g_def_pause_dec, g_def_pause_inc;
static uih::Rect g_def_autoblank_toggle;
static uih::Rect g_def_scorer_toggle;
static uih::Rect g_def_back;

static void draw_defaults() {
    auto& d = M5.Display;
    uih::draw_header("VORGABEN");

    // Value row: [label .................. − value +]
    // Buttons sit well clear of the centred value text so they don't overlap.
    auto value_row = [&](int y_baseline, const char* label, const char* value,
                         uih::Rect& dec, uih::Rect& inc) {
        d.setTextColor(COLOR_TEXT, COLOR_BG_DARK);
        d.setFont(&fonts::FreeSansBold12pt7b);
        d.setTextDatum(middle_left);
        d.drawString(label, 10, y_baseline);
        dec = uih::draw_button(158, y_baseline - 13, 26, 24, "-",
                               COLOR_BG_DARK, COLOR_WARN);
        d.setTextColor(COLOR_ACCENT, COLOR_BG_DARK);
        d.setFont(&fonts::FreeSansBold12pt7b);
        d.setTextDatum(middle_center);
        d.drawString(value, 232, y_baseline);
        inc = uih::draw_button(282, y_baseline - 13, 26, 24, "+",
                               COLOR_BG_DARK, COLOR_SUCCESS);
    };

    char b1[8], b2[8];
    snprintf(b1, sizeof(b1), "%u min", g_defaults.half_minutes);
    snprintf(b2, sizeof(b2), "%u min", g_defaults.pause_minutes);
    value_row( 60, "Halbzeit-Laenge:", b1, g_def_half_dec,  g_def_half_inc);
    value_row( 98, "Halbzeitpause:",   b2, g_def_pause_dec, g_def_pause_inc);

    d.setTextColor(COLOR_TEXT, COLOR_BG_DARK);
    d.setFont(&fonts::FreeSansBold12pt7b);
    d.setTextDatum(middle_left);
    d.drawString("Auto-loeschen:", 10, 140);
    g_def_autoblank_toggle = uih::draw_button(200, 127, 110, 24,
        g_defaults.auto_blank_after_match ? "AN" : "AUS",
        g_defaults.auto_blank_after_match ? COLOR_SUCCESS : COLOR_DIM,
        COLOR_BG_DARK);

    d.drawString("Torschuetze fragen:", 10, 178);
    g_def_scorer_toggle = uih::draw_button(200, 165, 110, 24,
        g_defaults.prompt_scorer_on_goal ? "AN" : "AUS",
        g_defaults.prompt_scorer_on_goal ? COLOR_SUCCESS : COLOR_DIM,
        COLOR_BG_DARK);

    g_def_back = uih::draw_button(10, 208, DISPLAY_WIDTH - 20, 28,
                                  "< Speichern + Zurueck", COLOR_PRIMARY, COLOR_TEXT);
}

static void handle_defaults_touch(int16_t x, int16_t y) {
    bool changed = false;
    if (uih::point_in(g_def_half_dec, x, y)) {
        if (g_defaults.half_minutes > 5) { g_defaults.half_minutes--; changed = true; }
    } else if (uih::point_in(g_def_half_inc, x, y)) {
        if (g_defaults.half_minutes < 60) { g_defaults.half_minutes++; changed = true; }
    } else if (uih::point_in(g_def_pause_dec, x, y)) {
        if (g_defaults.pause_minutes > 1) { g_defaults.pause_minutes--; changed = true; }
    } else if (uih::point_in(g_def_pause_inc, x, y)) {
        if (g_defaults.pause_minutes < 30) { g_defaults.pause_minutes++; changed = true; }
    } else if (uih::point_in(g_def_autoblank_toggle, x, y)) {
        g_defaults.auto_blank_after_match = !g_defaults.auto_blank_after_match;
        changed = true;
    } else if (uih::point_in(g_def_scorer_toggle, x, y)) {
        g_defaults.prompt_scorer_on_goal = !g_defaults.prompt_scorer_on_goal;
        changed = true;
    } else if (uih::point_in(g_def_back, x, y)) {
        save_defaults();
        go(SCREEN_SETTINGS);
    }
    if (changed) save_defaults();
}

// ============================================================================
//  Long-press tick + connection-lost banner overlay.
// ============================================================================
static void check_long_press() {
    if (!g_lp.active) return;
    const uint32_t now = millis();
    // Require touch to still be held within zone.
    auto t = M5.Touch.getDetail();
    if (!t.isPressed() ||
        !uih::point_in({g_lp.zone_x, g_lp.zone_y, g_lp.zone_w, g_lp.zone_h},
                       t.x, t.y)) {
        g_lp.active = false;
        return;
    }
    // 500ms before first auto-repeat, then 200ms cadence.
    const uint32_t hold = now - g_lp.start_ms;
    if (hold < 500) return;
    if (g_lp.last_fire_ms == 0 || now - g_lp.last_fire_ms >= 200) {
        g_lp.last_fire_ms = now;
        if (g_lp.home_side) state::score_home_delta(g_lp.delta);
        else                state::score_away_delta(g_lp.delta);
        state::save();
        espnow_client::send_state_now();
        vibe_short();
        invalidate();
    }
}

static void draw_link_banner() {
    if (g_screen == SCREEN_SPLASH || g_screen == SCREEN_SETUP) return;
    const bool ok = espnow_client::link_ok() && espnow_client::is_paired();
    const uint32_t now = millis();
    if (!ok) {
        if (g_link_lost_since_ms == 0) g_link_lost_since_ms = now;
        if (!g_link_lost_shown && now - g_link_lost_since_ms > 2000) {
            g_link_lost_shown = true;
            vibe_alert();
            // Tiny banner under the header — without redrawing the whole screen.
            auto& d = M5.Display;
            d.fillRect(0, HEADER_HEIGHT, DISPLAY_WIDTH, 16, COLOR_WARN);
            d.setTextColor(COLOR_BG_DARK, COLOR_WARN);
            d.setFont(&fonts::FreeSansBold9pt7b);
            d.setTextDatum(middle_center);
            d.drawString("FUNK VERLOREN", DISPLAY_WIDTH / 2, HEADER_HEIGHT + 8);
        }
    } else {
        if (g_link_lost_shown) invalidate();   // redraw cleanly
        g_link_lost_since_ms = 0;
        g_link_lost_shown = false;
    }
}

} // namespace ui

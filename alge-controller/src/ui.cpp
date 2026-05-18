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
static bool     g_tick_redraw   = false;  // partial: clock + status only
static uint32_t g_last_rendered_ms = 0;
static uint32_t g_pairing_success_ms = 0;  // moment pair-mode first saw the wallbox
static Screen   g_pairing_return = SCREEN_SETTINGS; // see open_pairing_screen()

// Transient confirmation banner ("toast") drawn on top of any screen for
// ~1.5 s after a fire-and-forget action (test commands, pair start,
// blank command). Gives the operator feedback when nothing else on
// screen changes.
static char     g_toast[48] = "";
static uint32_t g_toast_until_ms = 0;
static void show_toast(const char* msg) {
    strncpy(g_toast, msg, sizeof(g_toast) - 1);
    g_toast[sizeof(g_toast) - 1] = '\0';
    g_toast_until_ms = millis() + 1500;
}

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
// v2: the wallbox owns Vorgaben in NVS. g_defaults_edit is a local
// scratchpad used only while the Defaults screen is open — populated
// from state::defaults() on entry and shipped back via
// INTENT_SET_DEFAULTS when the user hits "Speichern + Zurück".
static state::Defaults g_defaults_edit = {};

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
static void draw_tick();
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
static void draw_confirm();
static void handle_confirm_touch(int16_t x, int16_t y);
static void draw_pairing();
static void handle_pairing_touch(int16_t x, int16_t y);
static void draw_link_banner();
static void check_long_press();

// Controller-local prefs (NOT synced to wallbox — each pult keeps its
// own backlight setting). 10..100 % maps to 25..255 on the M5 panel.
static uint8_t g_brightness_pct  = 70;
static uint8_t g_brightness_edit = 70;

static void apply_brightness() {
    uint16_t b = (uint16_t)g_brightness_pct * 255u / 100u;
    if (b < 20) b = 20;             // floor so the screen stays readable
    M5.Display.setBrightness((uint8_t)b);
}

static void load_controller_prefs() {
    Preferences p;
    p.begin("ctrl", true);
    g_brightness_pct = p.getUChar("br", 70);
    p.end();
    if (g_brightness_pct < 10)  g_brightness_pct = 10;
    if (g_brightness_pct > 100) g_brightness_pct = 100;
}

static void save_controller_prefs() {
    Preferences p;
    p.begin("ctrl", false);
    p.putUChar("br", g_brightness_pct);
    p.end();
}

// ----- Public ------------------------------------------------------------
void begin() {
    g_screen = SCREEN_SPLASH;
    g_screen_entered_ms = millis();
    g_invalidate = true;
    load_controller_prefs();
    apply_brightness();
    M5.Display.fillScreen(COLOR_BG_DARK);
}

void invalidate() { g_invalidate = true; }
Screen current() { return g_screen; }

void go(Screen s) {
    Serial.printf("[ui] go %d -> %d\n", (int)g_screen, (int)s);
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
        // Print both cooked and raw coords. If they differ, M5Unified is
        // applying a transform; if they match (and they're wrong),
        // M5Unified isn't rotating for us and we need to fix it here.
        Serial.printf("[touch] press cooked=(%d,%d) raw=(%d,%d) screen=%d\n",
                      touch.x, touch.y, touch.base_x, touch.base_y,
                      (int)g_screen);
        handle_touch(touch.x, touch.y);
    }
    if (touch.wasReleased()) {
        g_lp.active = false;
    }
    check_long_press();

    // Toast TTL expiry — schedule an invalidate so the screen below
    // repaints and visually wipes the toast.
    if (g_toast[0] && now > g_toast_until_ms) {
        g_toast[0] = '\0';
        g_invalidate = true;
    }

    // SCREEN_PAIRING: auto-leave after a brief success display once the
    // wallbox starts heartbeating to us. Also invalidate every 500ms so
    // the animated "…" dots show progress.
    if (g_screen == SCREEN_PAIRING) {
        const bool paired_now = espnow_client::is_paired() && state::link_live();
        if (paired_now) {
            if (g_pairing_success_ms == 0) g_pairing_success_ms = now;
            if (now - g_pairing_success_ms > 1800) {
                g_pairing_success_ms = 0;
                go(g_pairing_return);
            }
        } else {
            g_pairing_success_ms = 0;
            if (now - g_last_rendered_ms > 500) g_invalidate = true;
        }
    }

    // Screen-specific clock-driven invalidations.
    if (g_screen == SCREEN_SPLASH) {
        if (now - g_screen_entered_ms > 2000) {
            // v2: the wallbox owns match state. If we've heard from the
            // wallbox and there's an active match, drop the user straight
            // onto the match screen; otherwise go to setup. If we haven't
            // heard from the wallbox at all, also go to setup — the
            // FUNK VERLOREN banner will fire as soon as we render.
            const auto ms = state::peek().match_state;
            if (state::link_live() && ms != STATE_IDLE) {
                go(SCREEN_MATCH);
            } else {
                go(SCREEN_SETUP);
            }
        }
    }
    // Per-second clock tick triggers a partial redraw (clock + state
    // label only). Full fillScreen + redraw at 500 ms is what caused the
    // whole-screen flash while the match clock was running.
    if (g_screen == SCREEN_MATCH || g_screen == SCREEN_HALFTIME) {
        if (now - g_last_rendered_ms > 500) g_tick_redraw = true;
    }

    // Layout-affecting change from the wallbox (match_state or
    // clock_running flipped) → full repaint. Without this the PAUSE
    // button doesn't become START when the countdown auto-parks at 0,
    // and screen transitions only follow on the next touch.
    static uint16_t s_last_layout_v = 0;
    const uint16_t cur_layout_v = state::layout_version();
    if (cur_layout_v != s_last_layout_v) {
        s_last_layout_v = cur_layout_v;
        g_invalidate = true;
    }

    if (g_invalidate) {
        g_invalidate = false;
        g_tick_redraw = false;
        g_last_rendered_ms = now;
        draw();
    } else if (g_tick_redraw) {
        g_tick_redraw = false;
        g_last_rendered_ms = now;
        draw_tick();
    }

    // Overlays run AFTER the screen draws — otherwise a redraw in the
    // same tick would clobber the toast and we'd see one frame of
    // toast, one frame without, → visible flicker on every tick.
    draw_link_banner();

    // ENDED auto-blank after 5 min. Critical: read millis() afresh here.
    // The cached `now` at the top of tick() was sampled BEFORE the touch
    // handler ran, but a tap that transitions us to ENDED resets
    // g_screen_entered_ms to a later millis() value — so
    // (now - g_screen_entered_ms) underflows to a huge unsigned int and
    // the auto-blank would fire immediately, kicking the user straight
    // from MATCH ENDE back to SETUP.
    // v2: the wallbox owns the auto-blank timer. Controller just follows
    // state — if it goes IDLE while we're showing a match-related
    // screen, drop back to setup so the operator can start the next one.
    if (state::peek().match_state == STATE_IDLE) {
        if (g_screen == SCREEN_MATCH || g_screen == SCREEN_HALFTIME ||
            g_screen == SCREEN_ENDED || g_screen == SCREEN_PENALTY ||
            g_screen == SCREEN_PENALTY_TOSS) {
            go(SCREEN_SETUP);
        }
    }

    // Wallbox can auto-end the shootout the moment it's decided. Watch
    // for an actual PENALTY_SHOOTOUT → ENDED edge while on the penalty
    // screen — without this edge check we'd also bounce out of
    // SCREEN_PENALTY_TOSS (where state is still ENDED from the
    // previous match) before the operator has fired INTENT_START_PENALTIES.
    static MatchState s_prev_match_state = STATE_IDLE;
    const MatchState ms_now = state::peek().match_state;
    if (g_screen == SCREEN_PENALTY &&
        s_prev_match_state == STATE_PENALTY_SHOOTOUT &&
        ms_now == STATE_ENDED) {
        go(SCREEN_ENDED);
    }
    s_prev_match_state = ms_now;
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
    case SCREEN_CONFIRM:       draw_confirm(); break;
    case SCREEN_PAIRING:       draw_pairing(); break;
    }
}

// Partial redraw triggered by the per-second clock tick. Repaints only
// the regions that move (clock + state label) instead of fillScreen-ing
// the whole display — eliminates the visible whole-screen flash.
static void draw_match_tick();
static void draw_halftime_tick();
static void draw_tick() {
    switch (g_screen) {
    case SCREEN_MATCH:    draw_match_tick();    break;
    case SCREEN_HALFTIME: draw_halftime_tick(); break;
    default: break;
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
    case SCREEN_CONFIRM:       handle_confirm_touch(x, y); break;
    case SCREEN_PAIRING:       handle_pairing_touch(x, y); break;
    }
    invalidate();
}

// ============================================================================
//  SPLASH
// ============================================================================
static void draw_splash() {
    auto& d = M5.Display;
    // Crest centred near top, rendered from embedded PNG so alpha and
    // gradients survive (no RGB565 banding). PNG is pre-sized to the
    // display target (130x130) by tools/make_crest.py — drawPng's
    // maxW/maxH params CLIP rather than scale, so the source dimensions
    // must already match what we want on screen.
    const int cx = DISPLAY_WIDTH / 2;
    const int crest_y = 8;
    int below_crest = crest_y;
    if (CREST_PNG_LEN) {
        d.drawPng(CREST_PNG, CREST_PNG_LEN,
                  cx - CREST_WIDTH / 2, crest_y);
        below_crest = crest_y + CREST_HEIGHT;
    } else {
        // Text fallback if PNG decode fails / data missing. efontJA so
        // the Ä renders properly.
        d.fillRoundRect(cx - 70, crest_y, 140, 80, 8, COLOR_PRIMARY);
        d.setTextColor(COLOR_TEXT);
        d.setFont(&fonts::efontJA_16);
        d.setTextDatum(middle_center);
        d.drawString("FC", cx, crest_y + 26);
        d.drawString("WÄNGI", cx, crest_y + 60);
        below_crest = crest_y + 80;
    }
    uih::centre_text(cx, below_crest + 12, BRAND_NAME,
                     COLOR_TEXT, &fonts::efontJA_16);
    uih::centre_text(cx, below_crest + 32, "v" FIRMWARE_VERSION " . seit 1967",
                     COLOR_DIM, &fonts::FreeSans9pt7b);

    // progress bar
    const int barx = 40, bary = 222, barw = 240, barh = 8;
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
static uih::Rect g_setup_btn_settings;

static void draw_setup() {
    auto& d = M5.Display;
    // No header title — "MATCH KONFIGURIEREN" right below acts as the
    // page title, and the right-side header label tends to read as a
    // tappable button when it isn't one.
    uih::draw_header(nullptr);

    // Settings gear — sits in the otherwise-empty title slot of the
    // header. Provides the only path from SETUP into the Settings page
    // (and from there, Verlauf / Vorgaben).
    uih::draw_gear_icon(DISPLAY_WIDTH - 66, 6);
    g_setup_btn_settings = { DISPLAY_WIDTH - 70, 2, 24, 24 };

    // All setup labels render in efontJA so the visual style matches
    // the rest of the German UI text (which now uses real umlauts).
    d.setTextColor(COLOR_TEXT, COLOR_BG_DARK);
    d.setFont(&fonts::efontJA_16);
    d.setTextDatum(top_left);
    d.drawString("MATCH KONFIGURIEREN", 10, 36);

    // Label sits with a clear gap above the picker — old y=64 with the
    // efontJA_14 ascender almost touched the y=78 button top edge.
    d.setFont(&fonts::efontJA_14);
    d.setTextColor(COLOR_DIM, COLOR_BG_DARK);
    d.drawString("Spieltyp:", 10, 58);

    const auto& p = matchmodes::get(g_setup_preset_idx);
    g_setup_btn_picker = uih::draw_button(10, 78, DISPLAY_WIDTH - 20, 32,
                                          p.label, COLOR_BG_DARK, COLOR_TEXT);

    // efontJA_14 — sublabel may contain umlauts (e.g. "Eigene Zeit wählen").
    d.setFont(&fonts::efontJA_14);
    d.setTextColor(COLOR_ACCENT, COLOR_BG_DARK);
    d.setTextDatum(top_center);
    d.drawString(p.sublabel, DISPLAY_WIDTH / 2, 116);

    // Team labels
    d.setTextColor(COLOR_DIM);
    d.setTextDatum(top_left);
    d.drawString("Heim:", 10, 140);
    d.setTextColor(COLOR_TEXT);
    // efontJA_16 — BRAND_HOME_TEAM contains "ä" which FreeSans can't render.
    d.setFont(&fonts::efontJA_16);
    d.drawString(BRAND_HOME_TEAM, 70, 138);

    d.setFont(&fonts::efontJA_14);
    d.setTextColor(COLOR_DIM);
    d.drawString("Gegner:", 10, 165);
    g_setup_btn_team = uih::draw_button(70, 160, DISPLAY_WIDTH - 80, 26,
                                        g_setup_opponent, COLOR_BG_DARK, COLOR_TEXT);

    // Wider bottom buttons — old 200/90 split made "Countdown" auto-shrink
    // down to 9 pt to fit. Now 180/120 keeps both at the standard size.
    g_setup_btn_start = uih::draw_button(10, 198, 180, 36,
                                         "MATCH STARTEN",
                                         COLOR_PRIMARY, COLOR_TEXT, true);
    g_setup_btn_countdown = uih::draw_button(200, 198, 110, 36,
                                             "Countdown",
                                             COLOR_BG_DARK, COLOR_ACCENT);
}

static void setup_apply_preset(uint8_t idx) { g_setup_preset_idx = idx; }
static void setup_apply_team(const char* t)  {
    strncpy(g_setup_opponent, t, sizeof(g_setup_opponent) - 1);
    g_setup_opponent[sizeof(g_setup_opponent) - 1] = '\0';
}

static void handle_setup_touch(int16_t x, int16_t y) {
    if (uih::point_in(g_setup_btn_settings, x, y)) {
        g_screen_return = SCREEN_SETUP;
        go(SCREEN_SETTINGS);
        return;
    }
    if (uih::point_in(g_setup_btn_picker, x, y)) {
        open_preset_picker(setup_apply_preset);
    } else if (uih::point_in(g_setup_btn_team, x, y)) {
        open_text_input("Gegner-Name", g_setup_opponent, setup_apply_team);
    } else if (uih::point_in(g_setup_btn_start, x, y)) {
        espnow_client::send_intent_start_match(g_setup_preset_idx, g_setup_opponent);
        vibe_long();
        go(SCREEN_MATCH);
    } else if (uih::point_in(g_setup_btn_countdown, x, y)) {
        // Open numpad to choose countdown length, then enter pre-match.
        open_numpad("Countdown (Min.)", 5, 1, 15, [](int minutes) {
            espnow_client::send_intent_pre_match(g_setup_preset_idx,
                                                 g_setup_opponent,
                                                 (uint16_t)(minutes * 60));
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
    case STATE_HALF_1:           return "1. HALBZEIT . LÄUFT";
    case STATE_PAUSED_H1:        return "1. HALBZEIT . PAUSIERT";
    case STATE_HALF_2:           return "2. HALBZEIT . LÄUFT";
    case STATE_PAUSED_H2:        return "2. HALBZEIT . PAUSIERT";
    case STATE_EXTRA_TIME_1:     return "VERLÄNGERUNG 1.HZ";
    case STATE_EXTRA_TIME_2:     return "VERLÄNGERUNG 2.HZ";
    case STATE_PAUSED_ET:        return "VERLÄNGERUNG . PAUSE";
    case STATE_PRE_MATCH:        return "COUNTDOWN ZUM ANSTOSS";
    case STATE_PRE_EXTRA_TIME:   return "VERLÄNGERUNG STARTET";
    default:                     return "MATCH";
    }
}

static void draw_match() {
    const auto& s = state::peek();
    auto& d = M5.Display;
    uih::draw_header(s.clock_running ? "LÄUFT" : "PAUSIERT");

    // Top chips: stoppage top-right, undo top-left. The visual stays
    // small so it doesn't crash into the HEIM/GAST labels at y=52, but
    // the tap rect is widened so a fingertip can land in the corner
    // reliably. (Earlier 32×16 chip = visual *and* hit area, which was
    // why nothing happened when the operator tapped near the corner.)
    char stp_label[8];
    snprintf(stp_label, sizeof(stp_label), "+%u", s.stoppage_minutes);
    const bool near_half_end = (s.clock_seconds > 44u * 60u && s.clock_seconds < 60u * 60u) ||
                               (s.clock_seconds > 89u * 60u);
    const uint16_t stp_color = (s.stoppage_minutes > 0 && near_half_end)
                                   ? COLOR_ACCENT : COLOR_DIM;
    uih::draw_button(DISPLAY_WIDTH - 38, 32, 32, 16,
                     stp_label, COLOR_BG_DARK, stp_color);
    g_match_btn_stoppage = { DISPLAY_WIDTH - 60, HEADER_HEIGHT, 60, 24 };
    if (state::can_undo()) {
        uih::draw_button(6, 32, 28, 16, "<<", COLOR_BG_DARK, COLOR_ACCENT);
        g_match_btn_undo = { 0, HEADER_HEIGHT, 60, 24 };
    } else {
        g_match_btn_undo = {0, 0, 0, 0};
    }

    // Vertical layout (320x240 landscape):
    //   y=0..28   header
    //   y=32..48  top chips (undo / stoppage)
    //   y=52..68  HEIM/GAST labels
    //   y=72..148 score digits (Font8) — overlaps clock vertically but
    //             not horizontally (scores live in x=0..90 and x=230..320,
    //             clock lives in the middle x=110..210 gap).
    //   y=92..128 clock (slim FreeSansBold18pt7b — fits the 100 px gap)
    //   y=154..182 +/- buttons (below the score band, no overlap)
    //   y=186..200 match state label
    //   y=204..234 PAUSE / HALBZEIT
    uih::centre_text( 60, 52, "HEIM", COLOR_DIM, &fonts::FreeSans12pt7b);
    uih::centre_text(260, 52, "GAST", COLOR_DIM, &fonts::FreeSans12pt7b);

    uih::draw_score_value( 60, 110, s.home_score_real, COLOR_TEXT);
    uih::draw_score_value(260, 110, s.away_score_real, COLOR_TEXT);
    g_match_score_home = { 20, 72,  80, 76};
    g_match_score_away = {220, 72,  80, 76};

    // Clock — slimmer font so it stays inside the 100 px centre gap and
    // doesn't crash into the edges of the +/- buttons either side.
    g_match_btn_clock = {110, 92, 100, 36};
    uih::draw_clock(DISPLAY_WIDTH / 2, 110, s.clock_seconds,
                    s.clock_running ? COLOR_ACCENT : COLOR_DIM,
                    &fonts::FreeSansBold18pt7b);

    // +/- buttons — pushed below the score row so they don't collide
    // with the Font8 digits. Hidden entirely during the pre-match
    // countdown because there's nothing to score yet; zero rects keep
    // the touch handler from picking up phantom hits in that band.
    if (s.match_state == STATE_PRE_MATCH ||
        s.match_state == STATE_PRE_EXTRA_TIME) {
        g_match_btn_h_dec = g_match_btn_h_inc = {0, 0, 0, 0};
        g_match_btn_a_dec = g_match_btn_a_inc = {0, 0, 0, 0};
    } else {
        g_match_btn_h_dec = uih::draw_button( 10, 154, 40, 28, "-", COLOR_BG_DARK, COLOR_WARN);
        g_match_btn_h_inc = uih::draw_button( 70, 154, 40, 28, "+", COLOR_BG_DARK, COLOR_SUCCESS);
        g_match_btn_a_dec = uih::draw_button(210, 154, 40, 28, "-", COLOR_BG_DARK, COLOR_WARN);
        g_match_btn_a_inc = uih::draw_button(270, 154, 40, 28, "+", COLOR_BG_DARK, COLOR_SUCCESS);
    }

    // State label — when a score exceeds 9, fold a "Tafel X:Y" note into
    // the line and recolour to warn. Avoids a stacked second row that
    // would collide with PAUSE at y=204.
    char state_buf[64];
    // efontJA_14 so umlauts in state labels (LÄUFT, VERLÄNGERUNG) render.
    if (s.home_score_real > 9 || s.away_score_real > 9) {
        snprintf(state_buf, sizeof(state_buf), "%s  -  Tafel %u:%u",
                 match_state_label(s.match_state),
                 s.home_score_real % 10, s.away_score_real % 10);
        uih::centre_text(DISPLAY_WIDTH / 2, 188, state_buf, COLOR_WARN,
                         &fonts::efontJA_14);
    } else {
        uih::centre_text(DISPLAY_WIDTH / 2, 188,
                         match_state_label(s.match_state),
                         s.clock_running ? COLOR_SUCCESS : COLOR_WARN,
                         &fonts::efontJA_14);
    }

    // Left-side button. During the PRE_* countdowns we replace PAUSE
    // (pointless on a countdown) with a SKIP button that jumps straight
    // to the next phase — wallbox-side INTENT_SKIP_COUNTDOWN promotes
    // PRE_MATCH→HALF_1 / PRE_EXTRA_TIME→EXTRA_TIME_1 immediately. Same
    // intent fires whether the count is running or parked at 0.
    const bool in_pre = (s.match_state == STATE_PRE_MATCH ||
                         s.match_state == STATE_PRE_EXTRA_TIME);
    const char* pause_label = in_pre
        ? (s.match_state == STATE_PRE_EXTRA_TIME ? "Verlängerung"
                                                 : "Match starten")
        : (s.clock_running ? "PAUSE" : "WEITER");
    g_match_btn_pause = uih::draw_button(10, 204, 140, 30,
        pause_label,
        in_pre || !s.clock_running ? COLOR_SUCCESS : COLOR_WARN,
        COLOR_BG_DARK, true);

    // Right-side button label depends on which phase we're in. Extra
    // time has its own halftime — the button switches to "ET HALBZEIT"
    // during ET1, and only the 2nd-half-of-extra-time ends the match.
    const auto ms = s.match_state;
    const bool pre_any   = (ms == STATE_PRE_MATCH ||
                            ms == STATE_PRE_EXTRA_TIME);
    const bool in_h1     = (ms == STATE_HALF_1 || ms == STATE_PAUSED_H1);
    const bool in_et1    = (ms == STATE_EXTRA_TIME_1) ||
                           (ms == STATE_PAUSED_ET && s.clock_seconds < 105u * 60u);
    const char* right_label = pre_any  ? "Abbrechen"
                            : in_h1    ? "HALBZEIT"
                            : in_et1   ? "ET HALBZEIT"
                                       : "MATCH ENDE";
    g_match_btn_halftime = uih::draw_button(170, 204, 140, 30, right_label,
        COLOR_PRIMARY, COLOR_TEXT, true);

    // Settings cog at top-right corner of header (tappable region)
    g_match_btn_settings = { DISPLAY_WIDTH - 24, 4, 20, 20 };
}

// Partial redraw for the per-second tick — only the regions that
// actually change (clock + state label). Avoids the full-screen flash
// that the previous "fillScreen + redraw everything" approach caused.
static void draw_match_tick() {
    const auto& s = state::peek();
    auto& d = M5.Display;

    // Header "LÄUFT" / "PAUSIERT" — repaint the right side of the header
    // bar so the state can update if pause/resume happens between full
    // redraws.
    d.fillRect(DISPLAY_WIDTH - 90, 0, 50, HEADER_HEIGHT, COLOR_PRIMARY);
    d.setTextColor(COLOR_TEXT, COLOR_PRIMARY);
    d.setFont(&fonts::efontJA_14);
    d.setTextDatum(middle_right);
    d.drawString(s.clock_running ? "LÄUFT" : "PAUSIERT",
                 DISPLAY_WIDTH - 46, HEADER_HEIGHT / 2);

    // Clock rect — clear then redraw at the same slim font as draw_match().
    d.fillRect(110, 92, 100, 36, COLOR_BG_DARK);
    uih::draw_clock(DISPLAY_WIDTH / 2, 110, s.clock_seconds,
                    s.clock_running ? COLOR_ACCENT : COLOR_DIM,
                    &fonts::FreeSansBold18pt7b);

    // State label row — clear the whole 14 px band and redraw.
    d.fillRect(0, 182, DISPLAY_WIDTH, 16, COLOR_BG_DARK);
    char state_buf[64];
    if (s.home_score_real > 9 || s.away_score_real > 9) {
        snprintf(state_buf, sizeof(state_buf), "%s  -  Tafel %u:%u",
                 match_state_label(s.match_state),
                 s.home_score_real % 10, s.away_score_real % 10);
        uih::centre_text(DISPLAY_WIDTH / 2, 188, state_buf, COLOR_WARN,
                         &fonts::efontJA_14);
    } else {
        uih::centre_text(DISPLAY_WIDTH / 2, 188,
                         match_state_label(s.match_state),
                         s.clock_running ? COLOR_SUCCESS : COLOR_WARN,
                         &fonts::efontJA_14);
    }
}

// Numpad callbacks for score and clock edits.
static void numpad_set_home(int v) {
    espnow_client::send_intent_score_set((uint8_t)v, state::peek().away_score_real);
}
static void numpad_set_away(int v) {
    espnow_client::send_intent_score_set(state::peek().home_score_real, (uint8_t)v);
}
static void numpad_set_clock(int minutes) {
    espnow_client::send_intent_clock_set((uint16_t)(minutes * 60));
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
    // During the pre-match countdown there's no match yet — score, score
    // tap-to-edit and tap-clock-to-edit are all locked out so a stray
    // tap can't change a result that hasn't started.
    const bool is_pre = (s.match_state == STATE_PRE_MATCH ||
                         s.match_state == STATE_PRE_EXTRA_TIME);
    if (!is_pre && uih::point_in(g_match_btn_h_inc, x, y)) {
        espnow_client::send_intent_score_delta(true, +1);
            vibe_short();
        arm_long_press(g_match_btn_h_inc.x, g_match_btn_h_inc.y,
                       g_match_btn_h_inc.w, g_match_btn_h_inc.h, +1, true);
        if (state::defaults().prompt_scorer_on_goal) {
            open_numpad("Torschütze Heim (Trikot-Nr.)", 0, 0, 99,
                        [](int j) { espnow_client::send_intent_register_goal(true, (uint8_t)j); });
        }
    } else if (!is_pre && uih::point_in(g_match_btn_h_dec, x, y)) {
        espnow_client::send_intent_score_delta(true, -1);
            vibe_double();
        arm_long_press(g_match_btn_h_dec.x, g_match_btn_h_dec.y,
                       g_match_btn_h_dec.w, g_match_btn_h_dec.h, -1, true);
    } else if (!is_pre && uih::point_in(g_match_btn_a_inc, x, y)) {
        espnow_client::send_intent_score_delta(false, +1);
            vibe_short();
        arm_long_press(g_match_btn_a_inc.x, g_match_btn_a_inc.y,
                       g_match_btn_a_inc.w, g_match_btn_a_inc.h, +1, false);
        if (state::defaults().prompt_scorer_on_goal) {
            open_numpad("Torschütze Gast (Trikot-Nr.)", 0, 0, 99,
                        [](int j) { espnow_client::send_intent_register_goal(false, (uint8_t)j); });
        }
    } else if (!is_pre && uih::point_in(g_match_btn_a_dec, x, y)) {
        espnow_client::send_intent_score_delta(false, -1);
            vibe_double();
        arm_long_press(g_match_btn_a_dec.x, g_match_btn_a_dec.y,
                       g_match_btn_a_dec.w, g_match_btn_a_dec.h, -1, false);
    } else if (!is_pre && uih::point_in(g_match_score_home, x, y)) {
        open_numpad("HEIM-Stand", s.home_score_real, 0, 99, numpad_set_home);
    } else if (!is_pre && uih::point_in(g_match_score_away, x, y)) {
        open_numpad("GAST-Stand", s.away_score_real, 0, 99, numpad_set_away);
    } else if (!is_pre && uih::point_in(g_match_btn_clock, x, y)) {
        open_numpad("Uhr (Min.)", s.clock_seconds / 60, 0, 120, numpad_set_clock);
    } else if (uih::point_in(g_match_btn_pause, x, y)) {
        if (is_pre) {
            // PRE_* countdowns: button is the "skip directly to next
            // phase" affordance, not a pause/resume toggle.
            espnow_client::send_intent_skip_countdown();
        } else if (s.clock_running) {
            espnow_client::send_intent_simple(INTENT_PAUSE);
        } else {
            espnow_client::send_intent_simple(INTENT_RESUME);
        }
        vibe_pause();
    } else if (uih::point_in(g_match_btn_halftime, x, y)) {
        const auto ms = s.match_state;
        const bool pre_any   = (ms == STATE_PRE_MATCH ||
                                ms == STATE_PRE_EXTRA_TIME);
        const bool in_h1     = (ms == STATE_HALF_1 || ms == STATE_PAUSED_H1);
        const bool in_et1    = (ms == STATE_EXTRA_TIME_1) ||
                               (ms == STATE_PAUSED_ET && s.clock_seconds < 105u * 60u);
        if (pre_any) {
            // Cancel countdown — abort the not-yet-started match/ET.
            espnow_client::send_intent_simple(INTENT_RESET);
                vibe_double();
            go(SCREEN_SETUP);
        } else if (in_h1 || in_et1) {
            // start_halftime() inspects the current match_state and
            // routes to STATE_HALFTIME or STATE_ET_HALFTIME for us.
            espnow_client::send_intent_simple(INTENT_START_HALFTIME);
                vibe_long();
            go(SCREEN_HALFTIME);
        } else {
            // H2 or ET2 → end of the match
            espnow_client::send_intent_simple(INTENT_END_MATCH);
                vibe_long();
            go(SCREEN_ENDED);
        }
    } else if (uih::point_in(g_match_btn_settings, x, y)) {
        g_screen_return = SCREEN_MATCH;
        go(SCREEN_SETTINGS);
    } else if (state::can_undo() && uih::point_in(g_match_btn_undo, x, y)) {
        espnow_client::send_intent_simple(INTENT_UNDO);
        vibe_double();
    } else if (uih::point_in(g_match_btn_stoppage, x, y)) {
        open_numpad("Nachspielzeit (Min.)", s.stoppage_minutes, 0, 15,
                    [](int v) { espnow_client::send_intent_stoppage((uint8_t)v); });
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

    // Vertical layout (320x240):
    //   y=30..50   title
    //   y=58..104  end-of-half score + final time (compact, info-only)
    //   y=116..130 "Pausenzeit" label
    //   y=136..172 big countdown clock
    //   y=204..234 action buttons
    const bool from_et = (s.match_state == STATE_ET_HALFTIME);
    uih::centre_text(DISPLAY_WIDTH / 2, 30,
                     from_et ? "ENDE VERLÄNGERUNG 1" : "ENDSTAND 1. HALBZEIT",
                     COLOR_ACCENT, &fonts::efontJA_14);

    // Smaller score face here (Font7) — this row is informational, not
    // the live focus, so don't blow the whole vertical budget on it.
    uih::draw_score_digit( 60,  82, s.home_score_real, COLOR_TEXT);
    uih::draw_score_digit(260,  82, s.away_score_real, COLOR_TEXT);
    uih::draw_clock(DISPLAY_WIDTH / 2, 82, s.half1_end_seconds, COLOR_DIM,
                    &fonts::FreeSansBold18pt7b);

    // Pause countdown — measured from when this screen was entered.
    // Uses the operator-configured Halbzeitpause from Vorgaben.
    const uint32_t elapsed_ms = millis() - g_screen_entered_ms;
    const int32_t remain = (int32_t)(state::defaults().pause_minutes * 60) - (int32_t)(elapsed_ms / 1000);
    const uint16_t cmin = remain > 0 ? remain / 60 : 0;
    const uint16_t csec = remain > 0 ? remain % 60 : 0;
    // Pause label near its old position; big countdown number drops
    // further down so it reads as visually centred in the lower half
    // of the screen rather than huddling against the score row.
    uih::centre_text(DISPLAY_WIDTH / 2, 130, "Pausenzeit",
                     COLOR_DIM, &fonts::efontJA_14);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02u:%02u", cmin, csec);
    uih::centre_text(DISPLAY_WIDTH / 2, 160, buf,
                     (remain < 60 && (millis() / 500) % 2)
                         ? COLOR_WARN : COLOR_TEXT,
                     &fonts::FreeSansBold24pt7b);

    // Single big "start second half" button. In football the 2.HZ
    // always begins at the preset's half-time mark — 45:00 for adults,
    // 20:00 for F-Junioren, 105:00 for ET2 — never from "wherever the
    // clock happened to stop". The label shows the target time so the
    // operator can confirm before tapping. The Halbzeit-Länge default
    // only applies to "Freundschaftsspiel" (preset.custom == true);
    // every other preset uses its own half_minutes.
    char start_label[24];
    if (from_et) {
        snprintf(start_label, sizeof(start_label), "ET 2 ab 105:00");
    } else {
        const auto& p = matchmodes::get(state::peek().preset_idx);
        const uint8_t half_min = p.custom ? state::defaults().half_minutes
                                          : p.half_minutes;
        snprintf(start_label, sizeof(start_label),
                 "2.HZ ab %u:00", (unsigned)half_min);
    }
    g_ht_btn_start_h2 = uih::draw_button(10, 204, DISPLAY_WIDTH - 20, 30,
        start_label, COLOR_PRIMARY, COLOR_TEXT, true);
    // No longer used — left zeroed so touch handler ignores it cleanly.
    g_ht_btn_resume_at_45 = {0, 0, 0, 0};
}

// Partial redraw for the per-second halftime tick — only the pause
// countdown band, so the rest of the screen doesn't flash.
static void draw_halftime_tick() {
    auto& d = M5.Display;
    const uint32_t elapsed_ms = millis() - g_screen_entered_ms;
    const int32_t remain = (int32_t)(state::defaults().pause_minutes * 60) - (int32_t)(elapsed_ms / 1000);
    const uint16_t cmin = remain > 0 ? remain / 60 : 0;
    const uint16_t csec = remain > 0 ? remain % 60 : 0;
    char buf[8];
    snprintf(buf, sizeof(buf), "%02u:%02u", cmin, csec);
    // Cover the same band the full draw uses (label y=130, countdown
    // ends ~y=188), so leftover pixels from prior digits don't ghost.
    d.fillRect(60, 144, 200, 44, COLOR_BG_DARK);
    uih::centre_text(DISPLAY_WIDTH / 2, 152, buf,
                     (remain < 60 && (millis() / 500) % 2)
                         ? COLOR_WARN : COLOR_TEXT,
                     &fonts::FreeSansBold24pt7b);
}

static void handle_halftime_touch(int16_t x, int16_t y) {
    if (!uih::point_in(g_ht_btn_start_h2, x, y)) return;
    const bool from_et = (state::peek().match_state == STATE_ET_HALFTIME);
    if (from_et) {
        // ET2 always starts at 105:00; the wallbox dispatches via the
        // STATE_ET_HALFTIME special-case inside INTENT_START_HALF_2.
        espnow_client::send_intent_start_half_2(true, 105u * 60u);
    } else {
        const auto& p = matchmodes::get(state::peek().preset_idx);
        const uint16_t half_min = p.custom ? state::defaults().half_minutes
                                           : p.half_minutes;
        espnow_client::send_intent_start_half_2(true, (uint16_t)(half_min * 60));
    }
    vibe_long();
    go(SCREEN_MATCH);
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
    // Smaller clock face so a 3-digit ET final ("120:00") doesn't run
    // into the score digits left + right of it.
    uih::draw_clock(DISPLAY_WIDTH / 2, 78, s.clock_seconds, COLOR_DIM,
                    &fonts::FreeSansBold18pt7b);

    char vs[40];
    snprintf(vs, sizeof(vs), "%s  vs.  %s", BRAND_HOME_TEAM, s.opponent);
    // efontJA_14 — BRAND_HOME_TEAM contains "ä".
    uih::centre_text(DISPLAY_WIDTH / 2, 130, vs, COLOR_DIM, &fonts::efontJA_14);

    g_end_btn_new   = uih::draw_button(10,  150, 150, 32, "Neues Match",   COLOR_BG_DARK, COLOR_TEXT);
    g_end_btn_blank = uih::draw_button(170, 150, 140, 32, "Tafel löschen", COLOR_BG_DARK, COLOR_WARN);

    // Tiebreak buttons: Verlängerung is only an option after regulation
    // (before any ET has been played). Once we've already been through
    // ET and we're still tied, penalties are the only path left — so
    // hide the ET button entirely and let Penaltys take the full row.
    const bool draw_tied  = (s.home_score_real == s.away_score_real);
    const bool offer_et   = draw_tied && !s.extra_time_played;
    const bool offer_pen  = draw_tied;
    if (offer_et) {
        g_end_btn_extra = uih::draw_button(10, 188, 150, 32, "Verlängerung",
            COLOR_PRIMARY, COLOR_TEXT);
        g_end_btn_penalty = uih::draw_button(170, 188, 140, 32, "Penaltys",
            COLOR_PRIMARY, COLOR_TEXT);
    } else {
        g_end_btn_extra = {0, 0, 0, 0};   // not rendered, not tappable
        if (offer_pen) {
            g_end_btn_penalty = uih::draw_button(10, 188, 300, 32, "Penaltys",
                COLOR_PRIMARY, COLOR_TEXT);
        } else {
            g_end_btn_penalty = {0, 0, 0, 0};
        }
    }
}

static void handle_ended_touch(int16_t x, int16_t y) {
    const auto& s = state::peek();
    Serial.printf("[ended] touch (%d,%d) tied=%d score=%u:%u — new=(%d,%d,%d,%d) blank=(%d,%d,%d,%d) extra=(%d,%d,%d,%d) penalty=(%d,%d,%d,%d)\n",
                  x, y, (int)(s.home_score_real == s.away_score_real),
                  s.home_score_real, s.away_score_real,
                  g_end_btn_new.x, g_end_btn_new.y, g_end_btn_new.w, g_end_btn_new.h,
                  g_end_btn_blank.x, g_end_btn_blank.y, g_end_btn_blank.w, g_end_btn_blank.h,
                  g_end_btn_extra.x, g_end_btn_extra.y, g_end_btn_extra.w, g_end_btn_extra.h,
                  g_end_btn_penalty.x, g_end_btn_penalty.y, g_end_btn_penalty.w, g_end_btn_penalty.h);
    if (uih::point_in(g_end_btn_new, x, y)) {
        espnow_client::send_intent_simple(INTENT_RESET);
        espnow_client::send_intent_simple(INTENT_BLANK);
        go(SCREEN_SETUP);
    } else if (uih::point_in(g_end_btn_blank, x, y)) {
        espnow_client::send_intent_simple(INTENT_BLANK);
    } else if (uih::point_in(g_end_btn_extra, x, y) &&
               s.home_score_real == s.away_score_real) {
        espnow_client::send_intent_simple(INTENT_START_EXTRA_TIME);
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

    // 18pt bold on "WER SCHIESST ZUERST?" overflows 320 px; drop to 12pt.
    uih::centre_text(DISPLAY_WIDTH / 2, 44,
                     "WER SCHIESST ZUERST?", COLOR_TEXT,
                     &fonts::FreeSansBold12pt7b);
    // efontJA_16 covers ü / ä — used here to render proper umlauts.
    uih::centre_text(DISPLAY_WIDTH / 2, 72,
                     "Münze werfen, dann hier auswählen",
                     COLOR_DIM, &fonts::efontJA_14);

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
        espnow_client::send_intent_start_penalties(g_pk_home_turn);
        g_pk_home_turn = home_first;
        vibe_long();
        go(SCREEN_PENALTY);
    }
}

// ============================================================================
//  PENALTY SHOOTOUT
// ============================================================================
static uih::Rect g_pk_btn_home_score, g_pk_btn_home_miss;
static uih::Rect g_pk_btn_away_score, g_pk_btn_away_miss;
static uih::Rect g_pk_btn_end, g_pk_btn_undo;

static void draw_pk_kick_row(int16_t y, uint8_t taken, uint8_t mask,
                             bool sd_layout) {
    auto& d = M5.Display;
    // 5 boxes for regulation pens, 8 in sudden-death layout so the
    // operator can see the per-kick outcome for SD attempts 6/7/8.
    // Mask is uint8_t so we can store made/missed for the first 8 kicks
    // — kicks 9+ (rare) just affect score, not the per-kick row.
    const uint8_t slots = sd_layout ? 8 : 5;
    const int xs    = 70;
    const int avail = 240;
    const int step  = avail / slots;
    const int box_w = step - 4;
    for (int i = 0; i < slots; ++i) {
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

    // Sudden death starts the instant both sides have completed the
    // standard 5-kick round (regardless of tied/not — the operator
    // hits ENDE when a winner is decided). When SD is active, render
    // an 8-slot kick row on both sides so attempts 6/7/8 (and their
    // miss states) are actually visible. Without this expansion an SD
    // miss left no on-screen feedback and looked like a goal.
    const bool sd_active = (s.pk_home_taken >= 5 && s.pk_away_taken >= 5);
    side_label(120, "HEIM", g_pk_home_turn);
    draw_pk_kick_row(120, s.pk_home_taken, s.pk_home_kicks, sd_active);
    side_label(154, "GAST", !g_pk_home_turn);
    draw_pk_kick_row(154, s.pk_away_taken, s.pk_away_kicks, sd_active);

    if (sd_active) {
        uih::centre_text(DISPLAY_WIDTH / 2, 184, "SUDDEN DEATH",
                         COLOR_WARN, &fonts::FreeSansBold9pt7b);
    }

    // Four action buttons across the bottom row: Tor / Daneben /
    // Zurück (undo last kick → wallbox-side INTENT_UNDO) / ENDE.
    // 72 px wide, 4 px gap → 4*72 + 3*4 = 300, centred margins 10 each.
    const int bw = 72, gap = 4;
    const int x0 = 10;
    auto bx = [&](int col) { return x0 + col * (bw + gap); };
    if (g_pk_home_turn) {
        g_pk_btn_home_score = uih::draw_button(bx(0), 198, bw, 30, "Tor +",
                                               COLOR_SUCCESS, COLOR_BG_DARK);
        g_pk_btn_home_miss  = uih::draw_button(bx(1), 198, bw, 30, "Daneben",
                                               COLOR_ERROR,   COLOR_BG_DARK);
    } else {
        g_pk_btn_away_score = uih::draw_button(bx(0), 198, bw, 30, "Tor +",
                                               COLOR_SUCCESS, COLOR_BG_DARK);
        g_pk_btn_away_miss  = uih::draw_button(bx(1), 198, bw, 30, "Daneben",
                                               COLOR_ERROR,   COLOR_BG_DARK);
    }
    g_pk_btn_undo = uih::draw_button(bx(2), 198, bw, 30, "Zurück",
                                     COLOR_BG_DARK, COLOR_ACCENT);
    g_pk_btn_end  = uih::draw_button(bx(3), 198, bw, 30, "ENDE",
                                     COLOR_PRIMARY, COLOR_TEXT);
}

static void handle_penalty_touch(int16_t x, int16_t y) {
    // Undo + ENDE are first so they're never shadowed by stale
    // goal/miss button rects (which switch sides when g_pk_home_turn
    // flips). Goal/miss handling sits behind a single return so the
    // function can't fall through to ENDE on a stray tap.
    if (uih::point_in(g_pk_btn_undo, x, y)) {
        // INTENT_UNDO rewinds the wallbox's last state-affecting change
        // — perfect for "I tapped Tor by accident, it was a miss". The
        // wallbox's undo ring covers score + clock + match-state +
        // stoppage; flip our local pk turn back to match.
        espnow_client::send_intent_simple(INTENT_UNDO);
        g_pk_home_turn = !g_pk_home_turn;
        vibe_double();
        return;
    }
    if (uih::point_in(g_pk_btn_end, x, y)) {
        espnow_client::send_intent_simple(INTENT_END_MATCH);
        go(SCREEN_ENDED);
        return;
    }
    if (g_pk_home_turn) {
        if (uih::point_in(g_pk_btn_home_score, x, y)) {
            espnow_client::send_intent_pk_kick(true, true);
            g_pk_home_turn = false;
            vibe_short();
        } else if (uih::point_in(g_pk_btn_home_miss, x, y)) {
            espnow_client::send_intent_pk_kick(true, false);
            g_pk_home_turn = false;
            vibe_double();
        }
    } else {
        if (uih::point_in(g_pk_btn_away_score, x, y)) {
            espnow_client::send_intent_pk_kick(false, true);
            g_pk_home_turn = true;
            vibe_short();
        } else if (uih::point_in(g_pk_btn_away_miss, x, y)) {
            espnow_client::send_intent_pk_kick(false, false);
            g_pk_home_turn = true;
            vibe_double();
        }
    }
}

// ============================================================================
//  CONFIRM — generic Yes/No prompt for destructive actions.
// ============================================================================
static char    g_confirm_message[80] = "";
static void  (*g_confirm_yes)() = nullptr;
static Screen  g_confirm_return  = SCREEN_SETTINGS;
static uih::Rect g_confirm_btn_yes, g_confirm_btn_no;

static void open_confirm(const char* message, void (*on_yes)()) {
    strncpy(g_confirm_message, message, sizeof(g_confirm_message) - 1);
    g_confirm_message[sizeof(g_confirm_message) - 1] = '\0';
    g_confirm_yes = on_yes;
    g_confirm_return = g_screen;
    go(SCREEN_CONFIRM);
}

static void draw_confirm() {
    auto& d = M5.Display;
    uih::draw_header("BESTÄTIGEN");

    // Message centred in the body. efontJA so umlauts render.
    d.setFont(&fonts::efontJA_14);
    d.setTextColor(COLOR_TEXT, COLOR_BG_DARK);
    d.setTextDatum(middle_center);
    // Two-line wrap on the comma (cheap split — keeps long German
    // messages readable on the 320 px screen).
    char line1[80], line2[80];
    line1[0] = line2[0] = '\0';
    const char* comma = strchr(g_confirm_message, ',');
    if (comma) {
        const size_t n = (size_t)(comma - g_confirm_message);
        memcpy(line1, g_confirm_message, n);
        line1[n] = '\0';
        strncpy(line2, comma + 1, sizeof(line2) - 1);
        line2[sizeof(line2) - 1] = '\0';
        // Trim leading space on line2
        char* p = line2;
        while (*p == ' ') p++;
        d.drawString(line1, DISPLAY_WIDTH / 2, 80);
        d.drawString(p,     DISPLAY_WIDTH / 2, 108);
    } else {
        d.drawString(g_confirm_message, DISPLAY_WIDTH / 2, 94);
    }

    g_confirm_btn_no  = uih::draw_button(10,  198, 140, 36,
                                         "Abbrechen", COLOR_BG_DARK, COLOR_TEXT, true);
    g_confirm_btn_yes = uih::draw_button(170, 198, 140, 36,
                                         "JA, LÖSCHEN", COLOR_ERROR, COLOR_BG_DARK, true);
}

static void handle_confirm_touch(int16_t x, int16_t y) {
    if (uih::point_in(g_confirm_btn_yes, x, y)) {
        auto cb = g_confirm_yes;
        g_confirm_yes = nullptr;
        if (cb) cb();
        // Callback may issue its own go(); if it didn't, fall back to
        // wherever we came from.
        if (g_screen == SCREEN_CONFIRM) go(g_confirm_return);
    } else if (uih::point_in(g_confirm_btn_no, x, y)) {
        g_confirm_yes = nullptr;
        go(g_confirm_return);
    }
}

// ============================================================================
//  PAIRING — "Neu koppeln" flow. Shown after the user kicks off pairing
//  on the Settings screen. Renders a hint about the wallbox-side action
//  (long-press IO14) and auto-leaves to setup once the link is alive.
// ============================================================================
static uih::Rect g_pairing_btn_cancel;
// g_pairing_return is declared near the top of the file alongside the
// other tick()-touched globals so the auto-leave timer can see it
// without a forward declaration.

static void open_pairing_screen() {
    espnow_client::enter_pairing_mode();
    g_pairing_return = g_screen;
    go(SCREEN_PAIRING);
}

static void draw_pairing() {
    auto& d = M5.Display;
    uih::draw_header("PAIRING");

    const bool paired_now = espnow_client::is_paired() && state::link_live();

    if (paired_now) {
        uih::centre_text(DISPLAY_WIDTH / 2, 60, "Verbunden!",
                         COLOR_SUCCESS, &fonts::efontJA_16);
        char mac[28];
        const uint8_t* m = espnow_client::paired_mac();
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 m[0], m[1], m[2], m[3], m[4], m[5]);
        uih::centre_text(DISPLAY_WIDTH / 2, 90, mac, COLOR_DIM, &fonts::FreeSans9pt7b);
        uih::centre_text(DISPLAY_WIDTH / 2, 130, "Zurück in 2 Sek.",
                         COLOR_DIM, &fonts::efontJA_14);
    } else {
        uih::centre_text(DISPLAY_WIDTH / 2, 50, "Suche Wallbox…",
                         COLOR_ACCENT, &fonts::efontJA_16);
        uih::centre_text(DISPLAY_WIDTH / 2, 96, "Halte IO14 am",
                         COLOR_TEXT, &fonts::efontJA_14);
        uih::centre_text(DISPLAY_WIDTH / 2, 116, "Wallbox 3 Sek.",
                         COLOR_TEXT, &fonts::efontJA_14);
        uih::centre_text(DISPLAY_WIDTH / 2, 152, "um zu akzeptieren.",
                         COLOR_DIM, &fonts::efontJA_14);
        // Animated dots so the user can tell the controller is still
        // broadcasting and not just frozen.
        static int phase = 0;
        phase = (phase + 1) % 4;
        char dots[5] = "   ";
        for (int i = 0; i < phase && i < 3; ++i) dots[i] = '.';
        uih::centre_text(DISPLAY_WIDTH / 2, 184, dots, COLOR_ACCENT,
                         &fonts::FreeSansBold18pt7b);
    }

    g_pairing_btn_cancel = uih::draw_button(10, 204, DISPLAY_WIDTH - 20, 30,
        "< ABBRECHEN", COLOR_PRIMARY, COLOR_TEXT, true);
}

static void handle_pairing_touch(int16_t x, int16_t y) {
    if (uih::point_in(g_pairing_btn_cancel, x, y)) {
        go(g_pairing_return);
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
    uih::draw_header("EINSTELL.");

    // Funk status row — "Funk:" label, then either MAC+RSSI (green) or
    // "Nicht gepaart" (warn). Status starts at x=56 so the colon has a
    // visible gap from the value; old layout had them touching.
    d.setFont(&fonts::efontJA_14);
    d.setTextDatum(top_left);
    d.setTextColor(COLOR_DIM, COLOR_BG_DARK);
    d.drawString("Funk:", 8, 34);
    char buf[40];
    if (espnow_client::is_paired()) {
        const uint8_t* m = espnow_client::paired_mac();
        snprintf(buf, sizeof(buf), "OK %02X:%02X:%02X:%02X:%02X:%02X %ddBm",
                 m[0], m[1], m[2], m[3], m[4], m[5], espnow_client::last_rssi());
        d.setTextColor(COLOR_SUCCESS, COLOR_BG_DARK);
    } else {
        snprintf(buf, sizeof(buf), "Nicht gepaart");
        d.setTextColor(COLOR_WARN, COLOR_BG_DARK);
    }
    d.drawString(buf, 56, 34);

    // 2 x 3 button grid — tightened to h=28 so the factory and back
    // buttons further down don't get overlapped by the system info row.
    g_set_btn_repair   = uih::draw_button(  8,  56, 150, 28, "Neu koppeln",      COLOR_BG_DARK, COLOR_TEXT);
    g_set_btn_polarity = uih::draw_button(162, 56, 150, 28, "Polaritäts-Test",  COLOR_BG_DARK, COLOR_TEXT);
    g_set_btn_exercise = uih::draw_button(  8,  90, 150, 28, "Segment-Übung",   COLOR_BG_DARK, COLOR_TEXT);
    g_set_btn_blank    = uih::draw_button(162, 90, 150, 28, "Tafel löschen",    COLOR_BG_DARK, COLOR_WARN);
    g_set_btn_history  = uih::draw_button(  8, 124, 150, 28, "Match-Verlauf",   COLOR_BG_DARK, COLOR_TEXT);
    g_set_btn_defaults = uih::draw_button(162, 124, 150, 28, "Vorgaben",        COLOR_BG_DARK, COLOR_TEXT);

    // Factory reset — full-width, sits ABOVE the system info row.
    g_set_btn_factory  = uih::draw_button(  8, 158, DISPLAY_WIDTH - 16, 22,
                                          "Werkseinstellung (löscht alles)",
                                          COLOR_BG_DARK, COLOR_ERROR);

    // System info — single line below the factory button, comfortable
    // gap before the back button. Drop the "Verlauf: N Match(es)" hint
    // since "Match-Verlauf" button already implies it.
    d.setTextColor(COLOR_DIM, COLOR_BG_DARK);
    d.setFont(&fonts::FreeSans9pt7b);
    d.setTextDatum(top_left);
    char sys[80];
    snprintf(sys, sizeof(sys), "Akku %d%%  v%s  MAC %s",
             M5.Power.getBatteryLevel(), FIRMWARE_VERSION,
             WiFi.macAddress().c_str());
    d.drawString(sys, 8, 186);

    g_set_btn_back = uih::draw_button(10, 206, DISPLAY_WIDTH - 20, 26,
                                      "< ZURÜCK", COLOR_PRIMARY, COLOR_TEXT);
}

static void handle_settings_touch(int16_t x, int16_t y) {
    if (uih::point_in(g_set_btn_repair, x, y)) {
        // Take the user to a dedicated pairing screen so they get the
        // "long-press IO14 on the wallbox" hint instead of just a toast
        // and an unchanged Settings page.
        open_pairing_screen();
    } else if (uih::point_in(g_set_btn_polarity, x, y)) {
        espnow_client::send_intent_simple(INTENT_POLARITY_TEST);
        show_toast("Polaritäts-Test gesendet");
    } else if (uih::point_in(g_set_btn_exercise, x, y)) {
        espnow_client::send_intent_simple(INTENT_SEGMENT_EXERCISE);
        show_toast("Segment-Übung gestartet");
    } else if (uih::point_in(g_set_btn_blank, x, y)) {
        espnow_client::send_intent_simple(INTENT_BLANK);
        show_toast("Tafel gelöscht");
    } else if (uih::point_in(g_set_btn_factory, x, y)) {
        // Destructive: wipes pairing + history + match state and reboots.
        // Route through the confirm screen so a stray tap doesn't nuke
        // everything.
        open_confirm("Werkseinstellung, alle Daten gehen verloren?",
                     []() {
                         espnow_client::send_intent_simple(INTENT_FACTORY_RESET);
                         espnow_client::send_intent_simple(INTENT_RESET);
                         delay(500);
                         ESP.restart();
                     });
    } else if (uih::point_in(g_set_btn_back, x, y)) {
        go(g_screen_return);
    } else if (uih::point_in(g_set_btn_history, x, y)) {
        g_history_scroll = 0;
        espnow_client::request_history();
        go(SCREEN_HISTORY);
    } else if (uih::point_in(g_set_btn_defaults, x, y)) {
        // Snapshot the wallbox's current Vorgaben into our local edit
        // copy before showing the screen, so dec/inc start from the
        // real values rather than zero. Brightness is controller-local
        // — seed the edit from the currently-applied value.
        g_defaults_edit = state::defaults();
        g_brightness_edit = g_brightness_pct;
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

    // Vertical budget on the 240 px display:
    //   0..28   header
    //   30..62  value box (h=32) — 18 pt number centred via middle_center
    //   68..194 keypad: y0=68, kh=30, gap=6, 4 rows = 4*30+3*6 = 138 ends y=206
    //   wait that overflows — use kh=28 → 4*28+3*6=130 ends y=198
    //   204..234 action row (h=30)
    // Previous build had the action row landing at y=220..248 which
    // clipped the OK / Abbrechen labels off the bottom of the screen.
    //
    // For the value box: draw centred with middle_center datum so the
    // number sits visually inside the outlined band rather than the
    // top_center "text descends below" layout we had before.
    char val[8];
    snprintf(val, sizeof(val), "%d", g_numpad_value);
    d.fillRoundRect(20, 30, DISPLAY_WIDTH - 40, 32, 6, 0x18C3);
    d.drawRoundRect(20, 30, DISPLAY_WIDTH - 40, 32, 6, COLOR_ACCENT);
    // 12pt sits comfortably inside the 32 px band with margin top + bottom;
    // 18pt was still close to the outline despite being smaller than 24pt.
    d.setFont(&fonts::FreeSansBold12pt7b);
    d.setTextColor(COLOR_TEXT, 0x18C3);
    d.setTextDatum(middle_center);
    d.drawString(val, DISPLAY_WIDTH / 2, 30 + 16);

    // 4-row × 3-col keypad — buttons ~96 × 28 (still ~65 % bigger touch
    // target than the original 58 × 26). Index by digit value, see the
    // store-vs-lookup bug fix in the comment below.
    const int kw = 96, kh = 28, gap = 6;
    const int grid_w = 3 * kw + 2 * gap;
    const int kx0 = (DISPLAY_WIDTH - grid_w) / 2;
    const int ky0 = 68;
    for (int i = 0; i < 9; ++i) {
        const int digit = i + 1;
        const int row = i / 3, col = i % 3;
        char ch[2] = { (char)('0' + digit), 0 };
        g_np_digits[digit] = uih::draw_button(
            kx0 + col * (kw + gap), ky0 + row * (kh + gap),
            kw, kh, ch, COLOR_BG_DARK, COLOR_TEXT, /*large=*/true);
    }
    // bottom row: clear, 0, back — same width as digits.
    const int row_bottom_y = ky0 + 3 * (kh + gap);
    g_np_btn_clear = uih::draw_button(kx0,              row_bottom_y, kw, kh,
                                      "C",  COLOR_BG_DARK, COLOR_WARN, true);
    g_np_digits[0] = uih::draw_button(kx0 + (kw + gap), row_bottom_y, kw, kh,
                                      "0",  COLOR_BG_DARK, COLOR_TEXT, true);
    g_np_btn_back  = uih::draw_button(kx0 + 2 * (kw + gap), row_bottom_y, kw, kh,
                                      "<-", COLOR_BG_DARK, COLOR_WARN, true);

    // Action row at the very bottom.
    const int action_y = row_bottom_y + kh + 4;   // = 68 + 4*34 + 4 = 208
    g_np_btn_cancel = uih::draw_button(8,            action_y, 144, 28,
                                       "Abbrechen", COLOR_BG_DARK, COLOR_WARN, true);
    g_np_btn_ok     = uih::draw_button(DISPLAY_WIDTH - 152, action_y, 144, 28,
                                       "OK", COLOR_SUCCESS, COLOR_BG_DARK, true);
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
        // Default to returning to the calling screen, then let the
        // callback run — the callback may issue its own go() (e.g. the
        // Countdown flow which jumps from SETUP → MATCH pre-match) and
        // we don't want to clobber that by go()-ing afterwards.
        auto cb = g_numpad_callback;
        g_numpad_callback = nullptr;
        go(g_screen_return);
        if (cb) cb(g_numpad_value);
        vibe_short();
        return;
    }
}

// ============================================================================
//  HISTORY screen — scrollable list of past matches.
// ============================================================================
static uih::Rect g_hist_btn_up, g_hist_btn_down, g_hist_btn_back, g_hist_btn_clear;

static void draw_history() {
    auto& d = M5.Display;
    uih::draw_header("VERLAUF");

    const uint8_t total = state::history_expected();
    const uint8_t got   = state::history_received();
    if (state::history_count() == 0) {
        uih::centre_text(DISPLAY_WIDTH / 2, 100, "Keine Matches gespeichert.",
                         COLOR_DIM, &fonts::efontJA_14);
    } else if (got == 0) {
        // Request landed, response still in flight (or wallbox is silent).
        uih::centre_text(DISPLAY_WIDTH / 2, 100, "Lade von Tafel…",
                         COLOR_ACCENT, &fonts::efontJA_14);
    } else {
        d.setTextColor(COLOR_TEXT, COLOR_BG_DARK);
        d.setFont(&fonts::efontJA_14);
        d.setTextDatum(top_left);
        const int per_page = 5;
        const int row_h = 32;
        for (int i = 0; i < per_page; ++i) {
            const int idx = g_history_scroll + i;
            if (idx >= got) break;
            const auto& h = state::history_entry(idx);
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
            d.setTextColor(COLOR_TEXT, COLOR_BG_DARK);
            d.drawString(line1, 14, y + 4);
            d.setTextColor(COLOR_DIM, COLOR_BG_DARK);
            d.drawString(line2, 14, y + 16);
        }
        g_hist_btn_up   = uih::draw_button(DISPLAY_WIDTH - 44, 36,  36, 32, "^",
                                           COLOR_DIM, COLOR_TEXT);
        g_hist_btn_down = uih::draw_button(DISPLAY_WIDTH - 44, 72,  36, 32, "v",
                                           COLOR_DIM, COLOR_TEXT);
        // Progress hint when we haven't received all expected entries yet.
        if (got < total) {
            char prog[24];
            snprintf(prog, sizeof(prog), "Lade %u/%u…", got, total);
            uih::centre_text(DISPLAY_WIDTH / 2, 196, prog, COLOR_ACCENT,
                             &fonts::FreeSans9pt7b);
        }
    }
    (void)total;
    g_hist_btn_clear = uih::draw_button(10,  210, 140, 26, "Verlauf löschen",
                                        COLOR_BG_DARK, COLOR_WARN);
    g_hist_btn_back  = uih::draw_button(170, 210, 140, 26, "< Zurück",
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
        open_confirm("Match-Verlauf löschen, kein Zurück?",
                     []() {
                         espnow_client::send_intent_simple(INTENT_HISTORY_CLEAR);
                         g_history_scroll = 0;
                     });
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
static uih::Rect g_def_autostart_toggle;
static uih::Rect g_def_bright_dec, g_def_bright_inc;
static uih::Rect g_def_back;

static void draw_defaults() {
    auto& d = M5.Display;
    uih::draw_header("VORGABEN");

    // Value row: [label .................. − value +]
    // Buttons sit well clear of the centred value text so they don't overlap.
    // efontJA_16 — labels like "Halbzeit-Länge:" need umlaut support.
    auto value_row = [&](int y_baseline, const char* label, const char* value,
                         uih::Rect& dec, uih::Rect& inc) {
        d.setTextColor(COLOR_TEXT, COLOR_BG_DARK);
        d.setFont(&fonts::efontJA_16);
        d.setTextDatum(middle_left);
        d.drawString(label, 10, y_baseline);
        dec = uih::draw_button(158, y_baseline - 11, 26, 22, "-",
                               COLOR_BG_DARK, COLOR_WARN);
        d.setTextColor(COLOR_ACCENT, COLOR_BG_DARK);
        d.setFont(&fonts::FreeSansBold12pt7b);
        d.setTextDatum(middle_center);
        d.drawString(value, 232, y_baseline);
        inc = uih::draw_button(282, y_baseline - 11, 26, 22, "+",
                               COLOR_BG_DARK, COLOR_SUCCESS);
    };
    auto toggle_row = [&](int y_baseline, const char* label, bool on,
                          uih::Rect& tgl) {
        d.setTextColor(COLOR_TEXT, COLOR_BG_DARK);
        d.setFont(&fonts::efontJA_16);
        d.setTextDatum(middle_left);
        d.drawString(label, 10, y_baseline);
        tgl = uih::draw_button(158, y_baseline - 11, 150, 22,
                               on ? "AN" : "AUS",
                               on ? COLOR_SUCCESS : COLOR_DIM, COLOR_BG_DARK);
    };

    char b1[8], b2[8], b3[8];
    snprintf(b1, sizeof(b1), "%u min", g_defaults_edit.half_minutes);
    snprintf(b2, sizeof(b2), "%u min", g_defaults_edit.pause_minutes);
    snprintf(b3, sizeof(b3), "%u %%",  g_brightness_edit);
    // Six 28-px-spaced rows + back button. Rows at y=42/70/98/126/154/182,
    // back at y=204.
    value_row ( 42, "HZ Freund.:",   b1, g_def_half_dec,  g_def_half_inc);
    value_row ( 70, "Halbzeitpause:", b2, g_def_pause_dec, g_def_pause_inc);
    toggle_row( 98, "Auto-löschen:",  g_defaults_edit.auto_blank_after_match,
                                      g_def_autoblank_toggle);
    toggle_row(126, "Torschütze:",    g_defaults_edit.prompt_scorer_on_goal,
                                      g_def_scorer_toggle);
    toggle_row(154, "Auto-Start:",    g_defaults_edit.auto_start_after_break,
                                      g_def_autostart_toggle);
    value_row (182, "Helligkeit:",    b3, g_def_bright_dec, g_def_bright_inc);

    g_def_back = uih::draw_button(10, 208, DISPLAY_WIDTH - 20, 26,
                                  "< Speichern + Zurück", COLOR_PRIMARY, COLOR_TEXT);
}

static void handle_defaults_touch(int16_t x, int16_t y) {
    bool changed = false;
    if (uih::point_in(g_def_half_dec, x, y)) {
        if (g_defaults_edit.half_minutes > 5) { g_defaults_edit.half_minutes--; changed = true; }
    } else if (uih::point_in(g_def_half_inc, x, y)) {
        if (g_defaults_edit.half_minutes < 60) { g_defaults_edit.half_minutes++; changed = true; }
    } else if (uih::point_in(g_def_pause_dec, x, y)) {
        if (g_defaults_edit.pause_minutes > 1) { g_defaults_edit.pause_minutes--; changed = true; }
    } else if (uih::point_in(g_def_pause_inc, x, y)) {
        if (g_defaults_edit.pause_minutes < 30) { g_defaults_edit.pause_minutes++; changed = true; }
    } else if (uih::point_in(g_def_autoblank_toggle, x, y)) {
        g_defaults_edit.auto_blank_after_match = !g_defaults_edit.auto_blank_after_match;
        changed = true;
    } else if (uih::point_in(g_def_scorer_toggle, x, y)) {
        g_defaults_edit.prompt_scorer_on_goal = !g_defaults_edit.prompt_scorer_on_goal;
        changed = true;
    } else if (uih::point_in(g_def_autostart_toggle, x, y)) {
        g_defaults_edit.auto_start_after_break = !g_defaults_edit.auto_start_after_break;
        changed = true;
    } else if (uih::point_in(g_def_bright_dec, x, y)) {
        if (g_brightness_edit > 10) {
            g_brightness_edit -= 10;
            // Live-apply so the operator sees the change before saving.
            g_brightness_pct = g_brightness_edit;
            apply_brightness();
            changed = true;
        }
    } else if (uih::point_in(g_def_bright_inc, x, y)) {
        if (g_brightness_edit < 100) {
            g_brightness_edit += 10;
            g_brightness_pct = g_brightness_edit;
            apply_brightness();
            changed = true;
        }
    } else if (uih::point_in(g_def_back, x, y)) {
        // Ship the wallbox-synced defaults back over the radio and
        // persist the controller-local brightness to NVS.
        espnow_client::send_intent_set_defaults(
            g_defaults_edit.half_minutes,
            g_defaults_edit.pause_minutes,
            g_defaults_edit.auto_blank_after_match,
            g_defaults_edit.prompt_scorer_on_goal,
            g_defaults_edit.auto_start_after_break);
        g_brightness_pct = g_brightness_edit;
        save_controller_prefs();
        apply_brightness();
        go(SCREEN_SETTINGS);
    }
    // No save-while-editing: the wallbox-side write only happens on
    // "Speichern + Zurück" so an interrupted session can't half-commit.
    (void)changed;
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
        espnow_client::send_intent_score_delta(g_lp.home_side, (int8_t)g_lp.delta);
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

    // Toast — drawn over the current screen so the operator gets a
    // brief "action fired" confirmation. Cleared by tick() once its
    // TTL expires (which schedules an invalidate to wipe it).
    if (g_toast[0] && now <= g_toast_until_ms) {
        auto& d = M5.Display;
        const int bw = 240;
        const int bh = 26;
        const int bx = (DISPLAY_WIDTH - bw) / 2;
        const int by = DISPLAY_HEIGHT - bh - 4;
        d.fillRoundRect(bx, by, bw, bh, 6, COLOR_SUCCESS);
        d.drawRoundRect(bx, by, bw, bh, 6, COLOR_BG_DARK);
        d.setTextColor(COLOR_BG_DARK, COLOR_SUCCESS);
        d.setFont(&fonts::efontJA_14);
        d.setTextDatum(middle_center);
        d.drawString(g_toast, DISPLAY_WIDTH / 2, by + bh / 2);
    }
}

} // namespace ui

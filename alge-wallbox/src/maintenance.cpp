// ============================================================================
//  Wallbox maintenance modes — blank burst / polarity test / segment exercise.
//
//  Protocol v2: triggered by IntentType-driven wb_mode changes from
//  state::apply_intent(). The dispatcher in tick() watches for newly
//  entered modes and runs the corresponding GAZ4 sequence.
// ============================================================================
#include "maintenance.h"
#include "config.h"
#include "gaz4.h"
#include "state.h"
#include "polarity.h"
#include "espnow_server.h"

#include <Arduino.h>

namespace wb_maintenance {

static uint32_t g_exercise_step_ms = 0;
static uint8_t  g_exercise_idx     = 11;  // top-left clock digit
static uint8_t  g_exercise_value   = 0;
static uint32_t g_exercise_end_ms  = 0;

static uint32_t g_polarity_last_tx_ms = 0;

// Track the previously seen wb_mode so we can react on transitions
// without forcing apply_intent to do the GAZ4 work inline.
static wb_state::WallboxMode g_prev_mode = wb_state::WB_BOOT;

void run_blank_burst() {
    char frame[gaz4::FRAME_LEN];
    gaz4::build_blank_frame(frame);
    gaz4::transmit(frame, GAZ4_BLANK_BURST, GAZ4_BLANK_GAP_MS);
    wb_state::note_gaz4_tx();
}

void start_polarity_test() {
    wb_state::set_wb_mode(wb_state::WB_POLARITY_TEST);
    g_polarity_last_tx_ms = 0;
}

void start_segment_exercise(uint8_t digit_idx) {
    g_exercise_idx     = digit_idx;
    g_exercise_value   = 0;
    g_exercise_step_ms = 0;
    g_exercise_end_ms  = millis() + 30000;
    wb_state::set_wb_mode(wb_state::WB_SEGMENT_EXERCISE);
}

void tick() {
    const uint32_t now = millis();
    const wb_state::WallboxMode mode = wb_state::wb_mode();

    // Edge-trigger: when we newly enter WB_BLANK_BURST, fire the burst
    // and return to the idle/live mode determined by current match state.
    if (mode == wb_state::WB_BLANK_BURST && g_prev_mode != wb_state::WB_BLANK_BURST) {
        run_blank_burst();
        const auto& m = wb_state::peek_match();
        wb_state::set_wb_mode(
            (m.match_state == STATE_IDLE)
                ? wb_state::WB_PAIRED_IDLE
                : wb_state::WB_MATCH_LIVE);
    }

    switch (mode) {
    case wb_state::WB_POLARITY_TEST: {
        if (now - g_polarity_last_tx_ms >= 1000) {
            g_polarity_last_tx_ms = now;
            char frame[gaz4::FRAME_LEN];
            gaz4::build_test_pattern_frame(frame);
            gaz4::transmit(frame, 1, 0);
            wb_state::note_gaz4_tx();
        }
        break;
    }
    case wb_state::WB_SEGMENT_EXERCISE: {
        if (now >= g_exercise_end_ms) {
            wb_state::set_wb_mode(wb_state::WB_PAIRED_IDLE);
            break;
        }
        const uint32_t step_period = 1000u / GAZ4_EXERCISE_HZ;
        if (now - g_exercise_step_ms >= step_period) {
            g_exercise_step_ms = now;
            char frame[gaz4::FRAME_LEN];
            gaz4::build_exercise_frame(&g_exercise_idx, 1, g_exercise_value, frame);
            gaz4::transmit(frame, 1, 0);
            wb_state::note_gaz4_tx();
            g_exercise_value = (g_exercise_value + 1) % 10;
        }
        break;
    }
    default:
        break;
    }

    g_prev_mode = mode;
}

} // namespace wb_maintenance

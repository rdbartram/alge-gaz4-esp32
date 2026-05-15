// ============================================================================
//  Maintenance modes for the wall-box: blank burst, polarity test, segment
//  exercise, raw frame, factory reset.
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
static uint8_t  g_exercise_idx     = 11;  // default: top-left clock pos
static uint8_t  g_exercise_value   = 0;
static uint32_t g_exercise_end_ms  = 0;

static uint32_t g_polarity_last_tx_ms = 0;

void run_blank_burst() {
    char frame[gaz4::FRAME_LEN];
    gaz4::build_blank_frame(frame);
    wb_state::set_wb_mode(wb_state::WB_BLANK_BURST);
    gaz4::transmit(frame, GAZ4_BLANK_BURST, GAZ4_BLANK_GAP_MS);
    wb_state::note_gaz4_tx();
    // After burst, return to whatever the radio thinks we should be in.
    wb_state::set_wb_mode(
        wb_state::snapshot().match_state == STATE_IDLE
            ? wb_state::WB_PAIRED_IDLE
            : wb_state::WB_MATCH_LIVE);
}

void start_polarity_test() {
    wb_state::set_wb_mode(wb_state::WB_POLARITY_TEST);
    g_polarity_last_tx_ms = 0;
}

void start_segment_exercise(uint8_t digit_idx) {
    g_exercise_idx     = digit_idx;
    g_exercise_value   = 0;
    g_exercise_step_ms = 0;
    g_exercise_end_ms  = millis() + 30000;  // 30 seconds
    wb_state::set_wb_mode(wb_state::WB_SEGMENT_EXERCISE);
}

void tick() {
    const uint32_t now = millis();

    switch (wb_state::wb_mode()) {
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
            // Done exercising.
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
}

} // namespace wb_maintenance

// ----------------------------------------------------------------------------
//  Command dispatcher called from espnow_server on MSG_CMD.
// ----------------------------------------------------------------------------
extern "C" void wb_maintenance_handle_cmd(const ScoreboardMessage& msg) {
    switch (msg.cmd_type) {
    case CMD_BLANK:
        wb_maintenance::run_blank_burst();
        break;
    case CMD_POLARITY_TEST:
        wb_maintenance::start_polarity_test();
        break;
    case CMD_SEGMENT_EXERCISE:
        // cmd_arg = digit index. Default to 11 (top-left clock) if 0.
        wb_maintenance::start_segment_exercise(msg.cmd_arg == 0 ? 11 : msg.cmd_arg);
        break;
    case CMD_FACTORY_RESET:
        espnow_server::factory_reset();
        break;
    case CMD_RAW_FRAME:
        gaz4::transmit(msg.cmd_data, 1, 0);
        wb_state::note_gaz4_tx();
        break;
    default:
        break;
    }
}

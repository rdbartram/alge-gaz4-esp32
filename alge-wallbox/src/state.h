// ============================================================================
//  Local mirror of the scoreboard state on the wall-box. Mutated by:
//   - ESP-NOW message handler (MSG_STATE from controller)
//   - Local commands (polarity test, segment exercise, button blank)
//  Read by: display, GAZ4 TX scheduler.
// ============================================================================
#pragma once

#include "shared/message.h"
#include <stdint.h>

namespace wb_state {

// Lifecycle of the wall-box itself (not the match).
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

struct Snapshot {
    WallboxMode  wb_mode;
    MatchState   match_state;
    uint8_t      home_score;     // 0-9, already wrapped
    uint8_t      away_score;     // 0-9
    uint16_t     clock_seconds;
    bool         clock_running;
    bool         radio_linked;
    int8_t       last_rssi;
    bool         polarity_ok;    // user confirmed digits looked correct
    uint8_t      info_page;      // current info page on display
    uint32_t     last_state_ms;  // millis() of last MSG_STATE received
    uint32_t     last_tx_ms;     // millis() of last GAZ4 frame sent
    char         pair_status[40];
};

void init();

// Receive a MSG_STATE: update local snapshot, return true if anything changed.
bool apply_state_message(const ScoreboardMessage& msg);

void set_wb_mode(WallboxMode m);
WallboxMode wb_mode();

void note_radio_link(bool linked, int8_t rssi);
void note_gaz4_tx();
void cycle_info_page();

Snapshot snapshot();
void set_polarity_ok(bool ok);

} // namespace wb_state

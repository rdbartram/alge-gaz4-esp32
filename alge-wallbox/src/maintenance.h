// ============================================================================
//  Maintenance commands sent from controller (blank, polarity test, segment
//  exercise, factory reset, raw frame).
// ============================================================================
#pragma once

#include "shared/message.h"

// Called from the ESP-NOW recv callback when MSG_CMD arrives.
// (extern "C" so espnow_server.cpp can forward-declare it without #include.)
extern "C" void wb_maintenance_handle_cmd(const ScoreboardMessage& msg);

namespace wb_maintenance {

// Called from the main loop. Drives polarity test pattern and segment
// exercise transmissions while in those modes.
void tick();

// Internally-triggered maintenance (e.g. IO14 long press -> blank).
void run_blank_burst();
void start_polarity_test();
void start_segment_exercise(uint8_t digit_idx);

} // namespace wb_maintenance

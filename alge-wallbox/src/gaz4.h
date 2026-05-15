// ============================================================================
//  GAZ4 protocol frame builder and UART transmitter.
//
//  Reference: HANDOFF.md (root of repo) and alge_match.py build_frame().
//  This C++ implementation produces BYTE-IDENTICAL output to the Python.
//
//  Frame layout (17 bytes, ASCII):
//    idx 0..2:  "000" address (mandatory, 3 ASCII digits)
//    idx 3:     space
//    idx 4:     HOME score digit ('0'-'9')
//    idx 5:     space
//    idx 6:     GAST score digit ('0'-'9')
//    idx 7..10: spaces (padding)
//    idx 11:    clock minutes-tens
//    idx 12:    clock minutes-ones
//    idx 13:    space (colon slot)
//    idx 14:    clock seconds-tens
//    idx 15:    clock seconds-ones
//    idx 16:    CR (0x0D)
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace gaz4 {

constexpr size_t FRAME_LEN = 17;

// Build the canonical 17-byte GAZ4 frame.
// Scores are wrapped mod 10 (board only displays 0-9).
// total_seconds is the total elapsed match-phase time; mm:ss = (s/60):(s%60).
// total_seconds is clamped to <= 5999 (99:59).
// Writes exactly FRAME_LEN bytes into out_frame.
void build_match_frame(uint8_t home, uint8_t away, uint16_t total_seconds,
                       char* out_frame);

// Build a frame that blanks all six digits. 17 bytes: "000" + 13 spaces + CR.
void build_blank_frame(char* out_frame);

// Build a segment-exercise frame: spaces in all digit positions except
// indices listed in idx_list, which receive ('0' + value % 10).
// Used to free stuck flip segments by hammering a position through 0-9 at 10Hz.
void build_exercise_frame(const uint8_t* idx_list, size_t idx_count,
                          uint8_t value, char* out_frame);

// Build a "test pattern" frame showing 8s everywhere (verifies polarity and
// detects broken segments visually).
void build_test_pattern_frame(char* out_frame);

// Set up the GAZ4 UART. Call once from setup().
void uart_begin();

// Transmit a frame `repeats` times, with `gap_ms` between repeats.
// Blocks the calling task for the duration. Designed to be called from
// a dedicated FreeRTOS task so the main loop / radio aren't stalled.
void transmit(const char* frame, int repeats = 1, int gap_ms = 0);

} // namespace gaz4

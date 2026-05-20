// ============================================================================
//  alge-wallbox config: pins, colors, brand strings, timing constants.
// ============================================================================
#pragma once

#include <stdint.h>

// ----- Brand --------------------------------------------------------------
#define BRAND_NAME    "FC W\xC3\xA4ngi 1967"  // UTF-8 "Wängi"
#define BRAND_TAGLINE "Fussball mit Herz"
#define BRAND_FOUNDED 1967

// RGB565 colour palette (from handoff section 4).
#define COLOR_PRIMARY 0xB081  // #be1c1c FC Wängi red
#define COLOR_ACCENT  0xFE2A  // #fdc500 kit yellow
#define COLOR_BG_DARK 0x0841  // #0a0a0a near-black
#define COLOR_TEXT    0xFFFF  // white
#define COLOR_DIM     0x8410  // #888 dim text
#define COLOR_SUCCESS 0x2E84  // #44dd44
#define COLOR_WARN    0xFD60  // #ffaa00
#define COLOR_ERROR   0xF986  // #ff3333

// ----- Pin assignments (LilyGO T-Display-S3 confirmed datasheet) -----------
#define PIN_UART_TX   17   // -> MAX3232 TIN
#define PIN_UART_RX   18   // unused; GAZ4 is one-way receive only
#define PIN_USER_BTN  14   // IO14 wallbox button
#define PIN_BOOT_BTN   0   // BOOT button, doubles as pairing trigger

// ----- GAZ4 protocol timing (from HANDOFF.md) -----------------------------
#define GAZ4_BAUD            2400
#define GAZ4_FRAME_LEN       17
#define GAZ4_REFRESH_MS      1000  // 1Hz refresh while clock running
#define GAZ4_SCORE_BURST     4     // repeats on score change
#define GAZ4_SCORE_GAP_MS    100
#define GAZ4_BLANK_BURST     8     // repeats on blank (empirically required)
#define GAZ4_BLANK_GAP_MS    80
#define GAZ4_TRANSITION_BURST 2    // state transitions (pause, halftime)
#define GAZ4_TRANSITION_GAP_MS 200
#define GAZ4_EXERCISE_HZ     10    // segment-exercise cycle rate

// ----- ESP-NOW timing ----------------------------------------------------
#define ESPNOW_PAIRING_BROADCAST_MS 1000
#define ESPNOW_HEARTBEAT_MS         1000
#define ESPNOW_ACK_TIMEOUT_MS       500

// ----- Display constants -------------------------------------------------
#define DISPLAY_WIDTH   170
#define DISPLAY_HEIGHT  320
#define DISPLAY_DEFAULT_BRIGHTNESS 200

// ----- Button timing -----------------------------------------------------
#define BTN_SHORT_MAX_MS   1000
#define BTN_LONG_MIN_MS    2000
#define BTN_LONG_MAX_MS    4000
#define BTN_VERYLONG_MIN_MS 10000

// ----- NVS namespace -----------------------------------------------------
#define NVS_NAMESPACE "fcw_wb"

// ----- Match defaults (used by Vorgaben-backed state machine) ------------
#define DEFAULT_HALF_LEN_MIN   45
#define DEFAULT_PAUSE_LEN_MIN  15
#define EXTRA_TIME_HALF_MIN    15
#define ENDED_AUTO_BLANK_MS    (5UL * 60UL * 1000UL)  // 5 min

// ----- Version -----------------------------------------------------------
#define FIRMWARE_VERSION "2.0.0"
#define FIRMWARE_NAME    "alge-wallbox"

// Monotonic build code for the *controller* firmware the wall-box is
// expected to distribute. Bump this every time you ship a new
// controller binary. The wall-box compares against each paired
// controller's heartbeat-reported build code; if a peer is older, it
// gets a unicast MSG_FIRMWARE_AVAIL nudge.
#define CONTROLLER_FW_BUILD_EXPECTED 17u

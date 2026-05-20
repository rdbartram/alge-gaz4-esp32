// ============================================================================
//  alge-controller config: brand, colours, timing, default match settings.
// ============================================================================
#pragma once

#include <stdint.h>

// ----- Brand --------------------------------------------------------------
// Brand strings are UTF-8 with real umlauts. Every place that draws these
// to the LCD must pick a u8g2-style font (efontJA_*); the FreeSans family
// in M5GFX is ASCII-only and renders ä/ö/ü as missing-glyph boxes.
#define BRAND_NAME        "FC Wängi 1967"
#define BRAND_TAGLINE     "Fussball mit Herz"
#define BRAND_SLOGAN      "WÄNGI . 1967 NEVER GIVE UP"
#define BRAND_HOME_TEAM   "FC Wängi 1"
#define BRAND_FOUNDED     1967

// RGB565 colour palette (handoff section 4).
#define COLOR_PRIMARY 0xB081  // #be1c1c
#define COLOR_ACCENT  0xFE2A  // #fdc500
#define COLOR_BG_DARK 0x0841  // #0a0a0a
#define COLOR_TEXT    0xFFFF
#define COLOR_DIM     0x8410
#define COLOR_SUCCESS 0x2E84
#define COLOR_WARN    0xFD60
#define COLOR_ERROR   0xF986

// ----- Display ------------------------------------------------------------
#define DISPLAY_WIDTH  320
#define DISPLAY_HEIGHT 240
#define HEADER_HEIGHT  28

// ----- ESP-NOW timing ----------------------------------------------------
#define ESPNOW_ACK_TIMEOUT_MS    500
#define ESPNOW_RESEND_INTERVAL_MS 1000
#define ESPNOW_STATE_PUSH_MS     1000  // push state every 1s while live

// ----- Match defaults ----------------------------------------------------
#define DEFAULT_HALF_LEN_MIN   45
#define DEFAULT_PAUSE_LEN_MIN  15
#define EXTRA_TIME_HALF_MIN    15
#define ENDED_AUTO_BLANK_MS    (5UL * 60UL * 1000UL) // 5 min

// ----- Vibration patterns (intensity 0-255, time in ms) ------------------
#define VIB_SCORE_PLUS_MS  50
#define VIB_PAUSE_MS       80

// ----- NVS namespace -----------------------------------------------------
#define NVS_NAMESPACE "fcw_scbd"

// ----- Version -----------------------------------------------------------
#define FIRMWARE_VERSION "1.0.0"
#define FIRMWARE_NAME    "alge-controller"

// Bump in lockstep with CONTROLLER_FW_BUILD_EXPECTED on the wall-box
// every time we ship a new controller binary. Reported in each 5 s
// heartbeat so the wall-box knows whether to nudge for OTA.
#define CONTROLLER_FW_BUILD 7u

// ----- Match history -----------------------------------------------------
#define HISTORY_MAX_ENTRIES 5

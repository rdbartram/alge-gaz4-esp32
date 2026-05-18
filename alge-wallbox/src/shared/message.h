// ============================================================================
//  Shared ESP-NOW message protocol for FC Wängi 1967 scoreboard system.
//  This file is identical between alge-controller and alge-wallbox.
//  When editing, update BOTH copies in lockstep.
//
//  Protocol v2 (wallbox-as-master):
//    - Wallbox owns match state, NVS, history, defaults, clock tick.
//    - Controllers send MSG_INTENT for every user action.
//    - Wallbox broadcasts MSG_STATE (≥1 Hz + on change) to all paired
//      controllers, and MSG_DEFAULTS when the Vorgaben values change.
//    - Up to 6 controllers can be paired with one wallbox.
//    - Old v1 messages (controller-as-master) are rejected by version.
// ============================================================================
#pragma once

#include <stdint.h>
#include <string.h>

#define MSG_MAGIC          0xFC9E
#define MSG_PROTO_VERSION  0x02

// Hard cap on paired controllers per wallbox. Bump cautiously — the peer
// table sits in NVS and each MAC is 6 B + a couple of flags.
#define MAX_PAIRED_PEERS   6

// ----- Message types ------------------------------------------------------
enum MessageType : uint8_t {
    MSG_STATE        = 0x01,  // wallbox -> controllers (broadcast, ≥1Hz)
    MSG_INTENT       = 0x02,  // controller -> wallbox  (user action)
    MSG_DEFAULTS     = 0x03,  // wallbox -> controllers (Vorgaben snapshot)
    MSG_PAIRING_REQ  = 0x04,  // controller -> wallbox  (please add me)
    MSG_PAIRING_ACK  = 0x05,  // wallbox -> controller  (accepted / rejected)
    MSG_INTENT_ACK   = 0x06,  // wallbox -> controller  (intent applied/ignored)
    MSG_HISTORY      = 0x07,  // wallbox -> controller  (one entry per packet)
};

// ----- Match states -------------------------------------------------------
// Owned by the wallbox. Controllers only render based on these values.
enum MatchState : uint8_t {
    STATE_IDLE             = 0x00,
    STATE_SETUP            = 0x01,
    STATE_HALF_1           = 0x02,
    STATE_PAUSED_H1        = 0x03,
    STATE_HALFTIME         = 0x04,
    STATE_HALF_2           = 0x05,
    STATE_PAUSED_H2        = 0x06,
    STATE_ENDED            = 0x07,
    STATE_EXTRA_TIME_1     = 0x08,
    STATE_EXTRA_TIME_2     = 0x09,
    STATE_PAUSED_ET        = 0x0A,
    STATE_PENALTY_SHOOTOUT = 0x0B,
    STATE_PRE_MATCH        = 0x0C,
    STATE_ET_HALFTIME      = 0x0D,
    STATE_PRE_EXTRA_TIME   = 0x0E,   // 1-min breather between regulation and ET1
};

// ----- Intent types -------------------------------------------------------
// Sent by a controller to ask the wallbox to apply some change.
enum IntentType : uint8_t {
    INTENT_NONE              = 0x00,
    INTENT_START_MATCH       = 0x01,  // u8_a=preset_idx, opponent
    INTENT_PAUSE             = 0x02,
    INTENT_RESUME            = 0x03,
    INTENT_SCORE_HOME_DELTA  = 0x04,  // i8_a = ±N
    INTENT_SCORE_AWAY_DELTA  = 0x05,  // i8_a = ±N
    INTENT_SCORE_SET         = 0x06,  // u8_a=home, u8_b=away
    INTENT_CLOCK_SET         = 0x07,  // u16_a = seconds
    INTENT_START_HALFTIME    = 0x08,  // wallbox figures H1/ET1 from state
    INTENT_START_HALF_2      = 0x09,  // u8_a = reset_flag, u16_a = target_seconds
    INTENT_END_MATCH         = 0x0A,
    INTENT_START_EXTRA_TIME  = 0x0B,  // begin ET1
    INTENT_START_PENALTIES   = 0x0C,  // u8_a = home_first
    INTENT_PK_KICK           = 0x0D,  // u8_a = home_team, u8_b = scored
    INTENT_STOPPAGE_SET      = 0x0E,  // u8_a = minutes
    INTENT_PRE_MATCH         = 0x0F,  // u16_a=seconds, u8_a=preset, opponent
    INTENT_UNDO              = 0x10,
    INTENT_RESET             = 0x11,
    INTENT_REGISTER_GOAL     = 0x12,  // u8_a=home_team, u8_b=jersey
    INTENT_BLANK             = 0x13,  // send BLANK to GAZ4 immediately
    INTENT_POLARITY_TEST     = 0x14,
    INTENT_SEGMENT_EXERCISE  = 0x15,
    INTENT_FACTORY_RESET     = 0x16,
    INTENT_HISTORY_CLEAR     = 0x17,
    INTENT_SET_DEFAULTS      = 0x18,  // .defaults populated
    INTENT_REQUEST_FULL      = 0x19,  // controller asks for fresh MSG_STATE + MSG_DEFAULTS
    INTENT_CANCEL_PRE_MATCH  = 0x1A,  // abort countdown back to IDLE
    INTENT_REQUEST_HISTORY   = 0x1B,  // wallbox replies with N MSG_HISTORY packets
    INTENT_SKIP_COUNTDOWN    = 0x1C,  // PRE_MATCH→HALF_1 / PRE_EXTRA_TIME→ET_1
};

// ----- Pairing rejection reasons -----------------------------------------
enum PairingReason : uint8_t {
    PAIR_OK             = 0x00,
    PAIR_NOT_IN_MODE    = 0x01,  // wallbox isn't in pairing mode
    PAIR_TABLE_FULL     = 0x02,  // already at MAX_PAIRED_PEERS
    PAIR_ALREADY_PAIRED = 0x03,  // peer already in table (still success-ish)
};

// ----- Flag bits ---------------------------------------------------------
#define FLAG_CLOCK_RUNNING   (1u << 0)
#define FLAG_PAIRING_MODE    (1u << 1)  // wallbox is accepting new peers
#define FLAG_CAN_UNDO        (1u << 2)
#define FLAG_GAZ4_OK         (1u << 3)  // wallbox has confirmed scoreboard polarity

// ============================================================================
//  Payloads
// ============================================================================
#pragma pack(push, 1)

// ---- Defaults (Vorgaben). Mirrors the controller-side g_defaults struct.
struct DefaultsPayload {
    uint8_t  half_minutes;
    uint8_t  pause_minutes;
    uint8_t  auto_blank_after_match;
    uint8_t  prompt_scorer_on_goal;
    uint8_t  auto_start_after_break;   // 1 = countdowns/halftimes auto-promote
    uint8_t  reserved[3];               // pad for future use, keeps layout stable
};

// ---- Full state snapshot — what controllers render any screen from.
//      Keep this compact; broadcast each second so size matters.
struct StatePayload {
    uint8_t  match_state;             // MatchState
    uint8_t  preset_idx;
    uint8_t  home_score_real;         // controller can still show 10+
    uint8_t  away_score_real;
    uint16_t clock_seconds;           // current phase elapsed (or pre-match remaining)
    uint16_t half1_end_seconds;
    uint16_t half2_end_seconds;
    uint16_t pre_match_seconds;       // remaining countdown when in PRE_MATCH
    uint16_t pause_target_seconds;
    uint8_t  pk_home_kicks;           // bitmask: bit set = scored
    uint8_t  pk_away_kicks;
    uint8_t  pk_home_taken;
    uint8_t  pk_away_taken;
    uint8_t  stoppage_minutes;
    uint8_t  goal_count;
    uint8_t  flags;                   // FLAG_*
    uint8_t  history_count;           // for the Match-Verlauf badge
    uint8_t  pk_home_first;           // 1 if HEIM took first kick
    uint8_t  extra_time_played;       // 1 once ET phases have occurred
    char     opponent[24];
};

// ---- Intent — controller asks wallbox to do something.
struct IntentPayload {
    uint8_t  intent_type;             // IntentType
    int8_t   i8_a;                    // signed (delta)
    uint8_t  u8_a;                    // generic
    uint8_t  u8_b;
    uint16_t u16_a;
    uint16_t u16_b;
    char     opponent[24];            // INTENT_START_MATCH / PRE_MATCH
    DefaultsPayload defaults;         // INTENT_SET_DEFAULTS
};

// ---- Pairing handshake.
struct PairingPayload {
    char     friendly_name[24];       // controller advertises a label
    uint8_t  reason;                  // PairingReason (wallbox fills in ACK)
    uint8_t  reserved[3];
};

// ---- Intent ACK — wallbox confirms it applied (or ignored) an intent.
struct AckPayload {
    uint16_t intent_sequence;
    uint8_t  ok;                      // 1 = applied, 0 = ignored
    uint8_t  reserved;
};

// ---- History entry — one MSG_HISTORY packet per saved match. The
// wallbox sends `total` packets in response to INTENT_REQUEST_HISTORY;
// the controller assembles them by `index`.
struct HistoryPayload {
    uint8_t  index;                   // 0..total-1
    uint8_t  total;                   // wallbox's history_count snapshot
    uint32_t timestamp_unix;
    uint8_t  preset_idx;
    uint8_t  home_score_real;
    uint8_t  away_score_real;
    uint16_t final_clock_seconds;
    uint8_t  goal_count;
    char     opponent[24];
};

// ---- Wrapper -------------------------------------------------------------
// Single ESP-NOW packet shape. The active body field depends on msg_type.
// raw[80] inside the union sets a floor so all variants are the same size,
// which keeps the on-wire layout fixed regardless of payload type.
struct ScoreboardMessage {
    uint16_t magic;                   // MSG_MAGIC
    uint8_t  msg_type;                // MessageType
    uint8_t  version;                 // MSG_PROTO_VERSION
    uint16_t sequence;                // monotonic per sender
    int8_t   rssi;                    // sender's last-measured RSSI
    uint8_t  reserved;

    union {
        StatePayload    state;
        IntentPayload   intent;
        DefaultsPayload defaults;
        PairingPayload  pairing;
        AckPayload      ack;
        HistoryPayload  history;
        uint8_t         raw[80];
    } body;

    uint8_t  checksum;                // XOR over all bytes before this one
};
#pragma pack(pop)

// ----- Checksum helpers --------------------------------------------------
inline uint8_t scoreboard_msg_checksum(const ScoreboardMessage* msg) {
    uint8_t c = 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(msg);
    for (size_t i = 0; i < sizeof(ScoreboardMessage) - 1; ++i) {
        c ^= p[i];
    }
    return c;
}

inline bool scoreboard_msg_valid(const ScoreboardMessage* msg) {
    if (msg->magic != MSG_MAGIC) return false;
    if (msg->version != MSG_PROTO_VERSION) return false;
    return msg->checksum == scoreboard_msg_checksum(msg);
}

inline void scoreboard_msg_sign(ScoreboardMessage* msg) {
    msg->magic   = MSG_MAGIC;
    msg->version = MSG_PROTO_VERSION;
    msg->checksum = scoreboard_msg_checksum(msg);
}

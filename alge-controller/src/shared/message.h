// ============================================================================
//  Shared ESP-NOW message protocol for FC Wängi 1967 scoreboard system.
//  This file is identical between alge-controller and alge-wallbox.
//  When editing, update BOTH copies in lockstep.
// ============================================================================
#pragma once

#include <stdint.h>
#include <string.h>

#define MSG_MAGIC          0xFC9E
#define MSG_PROTO_VERSION  0x01

// ----- Message types ------------------------------------------------------
enum MessageType : uint8_t {
    MSG_HEARTBEAT = 0x01,  // Wall-box -> Controller, 1Hz idle keepalive
    MSG_STATE     = 0x02,  // Controller -> Wall-box, current scoreboard state
    MSG_CMD       = 0x03,  // Controller -> Wall-box, special command
    MSG_ACK       = 0x04,  // Wall-box -> Controller, ack to STATE or CMD
    MSG_PAIRING   = 0x05,  // Bidirectional, pairing handshake
};

// ----- Match states -------------------------------------------------------
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
    STATE_PRE_MATCH        = 0x0C,   // count-down to kickoff
};

// ----- Command types ------------------------------------------------------
enum CommandType : uint8_t {
    CMD_BLANK            = 0x01,
    CMD_POLARITY_TEST    = 0x02,
    CMD_SEGMENT_EXERCISE = 0x03,
    CMD_FACTORY_RESET    = 0x04,
    CMD_RAW_FRAME        = 0x05,  // dev only: send arbitrary 17-byte frame
};

// ----- Flag bits ----------------------------------------------------------
#define FLAG_CLOCK_RUNNING   (1u << 0)
#define FLAG_BLANK_REQUESTED (1u << 1)

// ----- Message payload ----------------------------------------------------
#pragma pack(push, 1)
struct ScoreboardMessage {
    uint16_t magic;         // MSG_MAGIC = 0xFC9E
    uint8_t  msg_type;      // MessageType
    uint8_t  version;       // MSG_PROTO_VERSION
    uint16_t sequence;      // monotonically incrementing per sender

    // ---- State payload (MSG_STATE, MSG_HEARTBEAT) ----
    uint8_t  match_state;   // MatchState
    uint8_t  home_score;    // board digit 0-9 (already wrapped)
    uint8_t  away_score;    // board digit 0-9 (already wrapped)
    uint16_t clock_seconds; // 0-9999 total elapsed seconds in current phase
    uint8_t  flags;         // FLAG_*

    // ---- Command payload (MSG_CMD) ----
    uint8_t  cmd_type;      // CommandType
    uint8_t  cmd_arg;       // command-specific argument (e.g. exercise idx)
    char     cmd_data[17];  // CMD_RAW_FRAME payload

    // ---- ACK payload (MSG_ACK) ----
    uint16_t ack_sequence;  // sequence number being acked
    int8_t   rssi;          // RSSI at receiver, dBm

    uint8_t  checksum;      // XOR of all bytes before this one
};
#pragma pack(pop)

// XOR checksum across every byte before the checksum field.
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
    msg->magic = MSG_MAGIC;
    msg->version = MSG_PROTO_VERSION;
    msg->checksum = scoreboard_msg_checksum(msg);
}

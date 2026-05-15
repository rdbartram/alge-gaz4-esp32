// ============================================================================
//  Wall-box ESP-NOW receiver.
//
//  Boot flow:
//    1. If paired_mac in NVS -> register peer, normal operation.
//    2. Otherwise -> enter pairing mode, broadcast MSG_PAIRING every 1s.
//    3. When controller responds with its MAC -> save and switch to normal.
//
//  Steady state:
//    - Accept MSG_STATE / MSG_CMD only from the paired MAC.
//    - Respond with MSG_ACK including RSSI.
//    - Send MSG_HEARTBEAT once per second.
// ============================================================================
#pragma once

#include "shared/message.h"
#include <stdint.h>

namespace espnow_server {

void begin();
void loop();

bool is_paired();
const uint8_t* paired_mac();   // 6-byte array, may be all zeros if unpaired
void enter_pairing_mode();     // discards any saved peer, broadcasts pairing
void factory_reset();          // wipe NVS and reboot

// Send an ACK back to the originator. Called from message handler.
void send_ack(const uint8_t* peer_mac, uint16_t ack_seq, int8_t rssi);

// Send our current state back to the controller (heartbeat).
void send_heartbeat();

} // namespace espnow_server

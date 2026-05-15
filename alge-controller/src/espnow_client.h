// ============================================================================
//  Controller-side ESP-NOW: pairing initiator, state pusher, ACK/heartbeat
//  receiver. Notifies the UI when link goes down/up.
// ============================================================================
#pragma once

#include "shared/message.h"
#include <stdint.h>

namespace espnow_client {

void begin();
void loop();

bool is_paired();
const uint8_t* paired_mac();
int8_t  last_rssi();
bool    link_ok();          // true if recent ACK or heartbeat seen
uint32_t last_state_sent_ms();

void enter_pairing_mode();   // wipe paired MAC, broadcast pairing
void send_state_now();       // push current state immediately
void send_command(uint8_t cmd_type, uint8_t arg = 0, const char* data = nullptr);

} // namespace espnow_client

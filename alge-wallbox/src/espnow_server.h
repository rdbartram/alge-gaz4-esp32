// ============================================================================
//  Wallbox ESP-NOW server — protocol v2 (wallbox-as-master).
//
//  Boot flow:
//    1. Load paired peer MAC list from NVS (up to MAX_PAIRED_PEERS).
//    2. Register each MAC as an ESP-NOW peer.
//    3. If the list is empty OR the user enters pairing mode (long-press
//       IO14), start broadcasting MSG_PAIRING invitations every 1 s.
//    4. When a controller sends MSG_PAIRING_REQ, add it to the table (if
//       there's room) and reply MSG_PAIRING_ACK.
//
//  Steady state:
//    - Accept MSG_INTENT from any peer in the table; apply via state::.
//    - Broadcast MSG_STATE every 1 s heartbeat + immediately on state change.
//    - Broadcast MSG_DEFAULTS when the Vorgaben snapshot changes.
//    - Respond with MSG_INTENT_ACK to each accepted intent.
// ============================================================================
#pragma once

#include "shared/message.h"
#include <stdint.h>

namespace espnow_server {

void begin();
void loop();

// Pairing.
void enter_pairing_mode();     // accept new peers for ~30s
void exit_pairing_mode();      // stop accepting new peers immediately
bool pairing_mode_active();
uint8_t paired_peer_count();
const uint8_t* paired_peer_mac(uint8_t idx);   // nullptr if idx out of range
void forget_all_peers();       // wipe peer table (factory-reset support)

// State + defaults broadcasting (called by main loop on state change).
void broadcast_state_now();
void broadcast_defaults_now();
void broadcast_state_if_stale();   // every ~1s heartbeat

// Pairing-mode countdown for the UI. Returns 0 when not in pairing mode,
// otherwise the milliseconds left until the window auto-closes.
uint32_t pairing_remaining_ms();

// Send the full history (N MSG_HISTORY packets, one per entry) back to
// a specific peer. Called by the intent handler on INTENT_REQUEST_HISTORY.
void send_history_to(const uint8_t* mac);

void factory_reset();          // wipe NVS and reboot

int8_t last_rx_rssi();

} // namespace espnow_server

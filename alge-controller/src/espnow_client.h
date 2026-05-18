// ============================================================================
//  Controller ESP-NOW client (protocol v2, wallbox-as-master).
//
//  Sends MSG_INTENT for every user action; receives MSG_STATE +
//  MSG_DEFAULTS broadcasts from the wallbox and pipes them into the
//  state:: cache. Pairing handshake initiates from this side.
// ============================================================================
#pragma once

#include "shared/message.h"
#include <stdint.h>

namespace espnow_client {

void begin();
void loop();

// Pairing -----------------------------------------------------------------
bool is_paired();
const uint8_t* paired_mac();
void enter_pairing_mode();           // broadcast pairing requests to find wallbox

// Link health -------------------------------------------------------------
int8_t   last_rssi();
bool     link_ok();                  // got an ACK or state in the last few seconds

// Intent senders ----------------------------------------------------------
// Generic — caller fills in IntentPayload directly.
void send_intent(const IntentPayload& it);

// Convenience wrappers — UI code uses these instead of building payloads.
void send_intent_simple(IntentType t);
void send_intent_score_delta(bool home, int8_t delta);
void send_intent_score_set(uint8_t home, uint8_t away);
void send_intent_clock_set(uint16_t seconds);
void send_intent_start_match(uint8_t preset_idx, const char* opponent);
void send_intent_start_half_2(bool reset_clock, uint16_t target_seconds);
void send_intent_start_penalties(bool home_first);
void send_intent_pk_kick(bool home_team, bool scored);
void send_intent_stoppage(uint8_t minutes);
void send_intent_pre_match(uint8_t preset_idx, const char* opponent, uint16_t seconds);
void send_intent_register_goal(bool home_team, uint8_t jersey);
void send_intent_set_defaults(uint8_t half_min, uint8_t pause_min,
                              bool autoblank, bool prompt_scorer);

// Ask the wallbox for a fresh MSG_STATE + MSG_DEFAULTS now.
void request_full_state();

// Ask the wallbox to send the saved match history (N MSG_HISTORY packets).
void request_history();

} // namespace espnow_client

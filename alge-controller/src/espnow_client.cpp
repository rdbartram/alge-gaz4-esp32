// ============================================================================
//  Controller ESP-NOW client (protocol v2).
// ============================================================================
#include "espnow_client.h"
#include "config.h"
#include "state.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <string.h>

namespace espnow_client {

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static uint8_t  g_wallbox_mac[6] = {0};
static bool     g_have_pair      = false;
static uint16_t g_tx_seq         = 0;
static int8_t   g_last_rssi      = -127;
static uint32_t g_last_rx_ms     = 0;
static uint32_t g_last_pair_broadcast_ms = 0;
static bool     g_pairing_mode   = false;

static constexpr uint32_t PAIR_BROADCAST_PERIOD_MS = 1000;

// ---- Forward decls ------------------------------------------------------
static void on_recv(const esp_now_recv_info_t* info, const uint8_t* data, int len);
static void handle_state(const ScoreboardMessage& m, int8_t rssi);
static void handle_defaults(const ScoreboardMessage& m);
static void handle_pairing_ack(const ScoreboardMessage& m, const uint8_t* from);
static void handle_pairing_advert(const ScoreboardMessage& m, const uint8_t* from);
static bool register_peer(const uint8_t* mac);
static bool load_paired_mac();
static void save_paired_mac(const uint8_t* mac);
static void clear_paired_mac();
static void send_pairing_request();
static void send_msg_to_wallbox(ScoreboardMessage& msg);

// ============================================================================
//  Public API
// ============================================================================
void begin() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println("[espnow] init FAILED");
        return;
    }

    esp_now_register_recv_cb(on_recv);

    // Broadcast peer for pairing.
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST_MAC, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    g_have_pair = load_paired_mac();
    if (g_have_pair) {
        if (register_peer(g_wallbox_mac)) {
            Serial.printf("[espnow] paired with wallbox %02X:%02X:%02X:%02X:%02X:%02X\n",
                g_wallbox_mac[0], g_wallbox_mac[1], g_wallbox_mac[2],
                g_wallbox_mac[3], g_wallbox_mac[4], g_wallbox_mac[5]);
            // Ask the wallbox to push fresh state right away so the UI
            // doesn't sit blank waiting for the next heartbeat tick.
            request_full_state();
        } else {
            Serial.println("[espnow] failed to register paired peer; re-pairing");
            g_have_pair = false;
            g_pairing_mode = true;
        }
    } else {
        g_pairing_mode = true;
        Serial.println("[espnow] no paired wallbox; entering pairing mode");
    }
}

void loop() {
    if (g_pairing_mode) {
        const uint32_t now = millis();
        if (now - g_last_pair_broadcast_ms >= PAIR_BROADCAST_PERIOD_MS) {
            g_last_pair_broadcast_ms = now;
            send_pairing_request();
        }
    }
}

bool is_paired()             { return g_have_pair; }
const uint8_t* paired_mac()  { return g_wallbox_mac; }
int8_t last_rssi()           { return g_last_rssi; }

bool link_ok() {
    return g_have_pair && (millis() - g_last_rx_ms) < 5000;
}

void enter_pairing_mode() {
    clear_paired_mac();
    if (esp_now_is_peer_exist(g_wallbox_mac)) {
        esp_now_del_peer(g_wallbox_mac);
    }
    memset(g_wallbox_mac, 0, 6);
    g_have_pair = false;
    g_pairing_mode = true;
    g_last_pair_broadcast_ms = 0;
}

// ============================================================================
//  Intent senders
// ============================================================================
void send_intent(const IntentPayload& it) {
    if (!g_have_pair) return;
    ScoreboardMessage msg = {};
    msg.msg_type = MSG_INTENT;
    msg.sequence = ++g_tx_seq;
    msg.body.intent = it;
    send_msg_to_wallbox(msg);
}

void send_intent_simple(IntentType t) {
    IntentPayload it = {};
    it.intent_type = t;
    send_intent(it);
}

void send_intent_score_delta(bool home, int8_t delta) {
    IntentPayload it = {};
    it.intent_type = home ? INTENT_SCORE_HOME_DELTA : INTENT_SCORE_AWAY_DELTA;
    it.i8_a = delta;
    send_intent(it);
}

void send_intent_score_set(uint8_t home, uint8_t away) {
    IntentPayload it = {};
    it.intent_type = INTENT_SCORE_SET;
    it.u8_a = home;
    it.u8_b = away;
    send_intent(it);
}

void send_intent_clock_set(uint16_t seconds) {
    IntentPayload it = {};
    it.intent_type = INTENT_CLOCK_SET;
    it.u16_a = seconds;
    send_intent(it);
}

void send_intent_start_match(uint8_t preset_idx, const char* opponent) {
    IntentPayload it = {};
    it.intent_type = INTENT_START_MATCH;
    it.u8_a = preset_idx;
    if (opponent) strncpy(it.opponent, opponent, sizeof(it.opponent) - 1);
    send_intent(it);
}

void send_intent_start_half_2(bool reset_clock, uint16_t target_seconds) {
    IntentPayload it = {};
    it.intent_type = INTENT_START_HALF_2;
    it.u8_a  = reset_clock ? 1 : 0;
    it.u16_a = target_seconds;
    send_intent(it);
}

void send_intent_start_penalties(bool home_first) {
    IntentPayload it = {};
    it.intent_type = INTENT_START_PENALTIES;
    it.u8_a = home_first ? 1 : 0;
    send_intent(it);
}

void send_intent_pk_kick(bool home_team, bool scored) {
    IntentPayload it = {};
    it.intent_type = INTENT_PK_KICK;
    it.u8_a = home_team ? 1 : 0;
    it.u8_b = scored    ? 1 : 0;
    send_intent(it);
}

void send_intent_stoppage(uint8_t minutes) {
    IntentPayload it = {};
    it.intent_type = INTENT_STOPPAGE_SET;
    it.u8_a = minutes;
    send_intent(it);
}

void send_intent_pre_match(uint8_t preset_idx, const char* opponent, uint16_t seconds) {
    IntentPayload it = {};
    it.intent_type = INTENT_PRE_MATCH;
    it.u8_a  = preset_idx;
    it.u16_a = seconds;
    if (opponent) strncpy(it.opponent, opponent, sizeof(it.opponent) - 1);
    send_intent(it);
}

void send_intent_register_goal(bool home_team, uint8_t jersey) {
    IntentPayload it = {};
    it.intent_type = INTENT_REGISTER_GOAL;
    it.u8_a = home_team ? 1 : 0;
    it.u8_b = jersey;
    send_intent(it);
}

void send_intent_set_defaults(uint8_t half_min, uint8_t pause_min,
                              bool autoblank, bool prompt_scorer) {
    IntentPayload it = {};
    it.intent_type = INTENT_SET_DEFAULTS;
    it.defaults.half_minutes           = half_min;
    it.defaults.pause_minutes          = pause_min;
    it.defaults.auto_blank_after_match = autoblank     ? 1 : 0;
    it.defaults.prompt_scorer_on_goal  = prompt_scorer ? 1 : 0;
    send_intent(it);
}

void request_full_state() {
    send_intent_simple(INTENT_REQUEST_FULL);
}

void request_history() {
    state::history_request_reset();
    send_intent_simple(INTENT_REQUEST_HISTORY);
}

// ============================================================================
//  Receive path
// ============================================================================
static void on_recv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (len < (int)sizeof(ScoreboardMessage)) return;
    ScoreboardMessage msg;
    memcpy(&msg, data, sizeof(msg));
    if (!scoreboard_msg_valid(&msg)) return;

    const uint8_t* from = info->src_addr;
    const int8_t rssi = info->rx_ctrl ? info->rx_ctrl->rssi : -127;
    g_last_rssi  = rssi;
    g_last_rx_ms = millis();

    switch (msg.msg_type) {
    case MSG_STATE:
        if (g_have_pair && memcmp(from, g_wallbox_mac, 6) == 0) {
            handle_state(msg, rssi);
        }
        break;
    case MSG_DEFAULTS:
        if (g_have_pair && memcmp(from, g_wallbox_mac, 6) == 0) {
            handle_defaults(msg);
        }
        break;
    case MSG_HISTORY:
        if (g_have_pair && memcmp(from, g_wallbox_mac, 6) == 0) {
            state::update_from_history(msg.body.history);
        }
        break;
    case MSG_PAIRING_REQ:
        handle_pairing_advert(msg, from);
        break;
    case MSG_PAIRING_ACK:
        handle_pairing_ack(msg, from);
        break;
    case MSG_INTENT_ACK:
    default:
        break;
    }
}

static void handle_state(const ScoreboardMessage& msg, int8_t rssi) {
    state::update_from_state(msg.body.state, rssi);
}

static void handle_defaults(const ScoreboardMessage& msg) {
    state::update_from_defaults(msg.body.defaults);
}

static void handle_pairing_advert(const ScoreboardMessage& /*msg*/, const uint8_t* from) {
    if (!g_pairing_mode) return;
    if (!register_peer(from)) return;
    ScoreboardMessage out = {};
    out.msg_type = MSG_PAIRING_REQ;
    out.sequence = ++g_tx_seq;
    strncpy(out.body.pairing.friendly_name, "Pult",
            sizeof(out.body.pairing.friendly_name) - 1);
    scoreboard_msg_sign(&out);
    esp_now_send(from, (uint8_t*)&out, sizeof(out));
}

static void handle_pairing_ack(const ScoreboardMessage& msg, const uint8_t* from) {
    const auto reason = (PairingReason)msg.body.pairing.reason;
    if (reason == PAIR_OK || reason == PAIR_ALREADY_PAIRED) {
        memcpy(g_wallbox_mac, from, 6);
        if (register_peer(g_wallbox_mac)) {
            save_paired_mac(g_wallbox_mac);
            g_have_pair = true;
            g_pairing_mode = false;
            Serial.printf("[espnow] PAIRED with wallbox %02X:%02X:%02X:%02X:%02X:%02X\n",
                from[0], from[1], from[2], from[3], from[4], from[5]);
            request_full_state();
        }
    } else {
        Serial.printf("[espnow] pairing rejected (reason=%u)\n", (unsigned)reason);
    }
}

// ============================================================================
//  Helpers
// ============================================================================
static void send_msg_to_wallbox(ScoreboardMessage& msg) {
    scoreboard_msg_sign(&msg);
    esp_now_send(g_wallbox_mac, (const uint8_t*)&msg, sizeof(msg));
}

static void send_pairing_request() {
    ScoreboardMessage msg = {};
    msg.msg_type = MSG_PAIRING_REQ;
    msg.sequence = ++g_tx_seq;
    strncpy(msg.body.pairing.friendly_name, "Pult",
            sizeof(msg.body.pairing.friendly_name) - 1);
    scoreboard_msg_sign(&msg);
    esp_now_send(BROADCAST_MAC, (uint8_t*)&msg, sizeof(msg));
}

static bool register_peer(const uint8_t* mac) {
    if (esp_now_is_peer_exist(mac)) return true;
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;
    peer.encrypt = false;
    return esp_now_add_peer(&peer) == ESP_OK;
}

static bool load_paired_mac() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    const size_t n = prefs.getBytes("wallbox_mac", g_wallbox_mac, 6);
    prefs.end();
    if (n != 6) return false;
    for (int i = 0; i < 6; ++i) if (g_wallbox_mac[i] != 0) return true;
    return false;
}

static void save_paired_mac(const uint8_t* mac) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBytes("wallbox_mac", mac, 6);
    prefs.end();
}

static void clear_paired_mac() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.remove("wallbox_mac");
    prefs.end();
}

} // namespace espnow_client

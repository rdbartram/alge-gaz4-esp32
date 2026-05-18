// ============================================================================
//  Wallbox ESP-NOW server — protocol v2 implementation.
// ============================================================================
#include "espnow_server.h"
#include "config.h"
#include "credentials.h"
#include "state.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <string.h>

// AES-128-CMAC keys shared with the controllers (credentials.h on both
// sides must match). Broadcast (pairing handshake) traffic is
// unencrypted by ESP-NOW protocol; unicast paired traffic is encrypted.
static const uint8_t kEspNowPmk[16] = ESPNOW_PMK;
static const uint8_t kEspNowLmk[16] = ESPNOW_LMK;

namespace espnow_server {

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static constexpr uint32_t PAIRING_TIMEOUT_MS = 30000;
static constexpr uint32_t STATE_HEARTBEAT_MS = 1000;

struct Peer {
    uint8_t  mac[6];
    bool     in_use;
    int8_t   last_rssi;
    uint16_t last_intent_seq;   // last intent sequence applied (dedup)
    bool     has_intent_seq;    // false until first intent observed
};

static Peer    g_peers[MAX_PAIRED_PEERS];
static uint8_t g_peer_count = 0;

static bool     g_pairing_mode      = false;
static uint32_t g_pairing_started_ms = 0;
static uint32_t g_last_pairing_broadcast_ms = 0;
static uint32_t g_last_state_broadcast_ms   = 0;
static uint16_t g_tx_seq            = 0;
static int8_t   g_last_rx_rssi      = -127;

// --- Forward decls --------------------------------------------------------
static void on_recv(const esp_now_recv_info_t* info, const uint8_t* data, int len);
static void handle_intent (const ScoreboardMessage& m, const uint8_t* from);
static void handle_pairing(const ScoreboardMessage& m, const uint8_t* from);

static bool register_peer(const uint8_t* mac);
static void load_peer_table();
static void save_peer_table();
static int  find_peer(const uint8_t* mac);
static bool add_peer(const uint8_t* mac);
static void send_pairing_invite_broadcast();
static void send_pairing_ack(const uint8_t* to, PairingReason reason);
static void send_intent_ack(const uint8_t* to, uint16_t intent_seq, bool ok);
static void broadcast_to_all_peers(const ScoreboardMessage& msg);

// ============================================================================
//  Public API
// ============================================================================
void begin() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    // Lock channel 1 so OTA's SoftAP later doesn't disturb ESP-NOW peers.
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println("[espnow] init FAILED");
        wb_state::set_wb_mode(wb_state::WB_ERROR);
        return;
    }

    esp_now_set_pmk(kEspNowPmk);     // network-wide AES key
    esp_now_register_recv_cb(on_recv);

    // Broadcast peer for pairing invites — stays unencrypted (ESP-NOW
    // doesn't encrypt broadcasts). Unicast peers added below use the
    // shared LMK so paired traffic is AES-128 CMAC encrypted.
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST_MAC, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    load_peer_table();
    if (g_peer_count == 0) {
        enter_pairing_mode();
        wb_state::set_wb_mode(wb_state::WB_PAIRING);
        Serial.println("[espnow] no paired peers; pairing mode active");
    } else {
        wb_state::set_paired_peer_count(g_peer_count);
        wb_state::set_wb_mode(wb_state::WB_PAIRED_IDLE);
        Serial.printf("[espnow] %u paired peer(s) loaded\n", g_peer_count);
    }
}

void loop() {
    const uint32_t now = millis();

    // Pairing timeout — drop out after 30 s if nobody asked to pair.
    if (g_pairing_mode && now - g_pairing_started_ms > PAIRING_TIMEOUT_MS) {
        exit_pairing_mode();
    }

    // Pairing broadcasts at 1 Hz while in pairing mode.
    if (g_pairing_mode && now - g_last_pairing_broadcast_ms >= ESPNOW_PAIRING_BROADCAST_MS) {
        g_last_pairing_broadcast_ms = now;
        send_pairing_invite_broadcast();
    }

    // 1 Hz state heartbeat to all paired peers (so they know we're alive
    // even when nothing else has changed).
    broadcast_state_if_stale();
}

void enter_pairing_mode() {
    g_pairing_mode = true;
    g_pairing_started_ms = millis();
    wb_state::set_pairing_mode(true);
    Serial.println("[espnow] entering pairing mode for 30s");
}

void exit_pairing_mode() {
    g_pairing_mode = false;
    wb_state::set_pairing_mode(false);
    Serial.println("[espnow] leaving pairing mode");
    if (g_peer_count > 0 && wb_state::wb_mode() == wb_state::WB_PAIRING) {
        wb_state::set_wb_mode(wb_state::WB_PAIRED_IDLE);
    }
}

bool pairing_mode_active() { return g_pairing_mode; }
uint8_t paired_peer_count() { return g_peer_count; }

const uint8_t* paired_peer_mac(uint8_t idx) {
    if (idx >= g_peer_count) return nullptr;
    return g_peers[idx].mac;
}

void forget_all_peers() {
    for (uint8_t i = 0; i < MAX_PAIRED_PEERS; ++i) {
        if (g_peers[i].in_use) {
            esp_now_del_peer(g_peers[i].mac);
            g_peers[i].in_use = false;
        }
    }
    g_peer_count = 0;
    wb_state::set_paired_peer_count(0);
    save_peer_table();
}

void broadcast_state_now() {
    ScoreboardMessage msg = {};
    msg.msg_type = MSG_STATE;
    msg.sequence = ++g_tx_seq;
    msg.rssi     = g_last_rx_rssi;
    wb_state::fill_state_payload(msg.body.state, g_pairing_mode, /*gaz4_ok=*/true);
    scoreboard_msg_sign(&msg);
    broadcast_to_all_peers(msg);
    g_last_state_broadcast_ms = millis();
}

void broadcast_defaults_now() {
    ScoreboardMessage msg = {};
    msg.msg_type = MSG_DEFAULTS;
    msg.sequence = ++g_tx_seq;
    msg.rssi     = g_last_rx_rssi;
    wb_state::fill_defaults_payload(msg.body.defaults);
    scoreboard_msg_sign(&msg);
    broadcast_to_all_peers(msg);
}

void broadcast_state_if_stale() {
    if (g_peer_count == 0) return;
    const uint32_t now = millis();
    if (now - g_last_state_broadcast_ms >= STATE_HEARTBEAT_MS) {
        broadcast_state_now();
    }
}

int8_t last_rx_rssi() { return g_last_rx_rssi; }

uint32_t pairing_remaining_ms() {
    if (!g_pairing_mode) return 0;
    const uint32_t elapsed = millis() - g_pairing_started_ms;
    return (elapsed >= PAIRING_TIMEOUT_MS) ? 0 : (PAIRING_TIMEOUT_MS - elapsed);
}

// Send all stored matches as a sequence of MSG_HISTORY packets, one per
// entry. Triggered by a paired controller's INTENT_REQUEST_HISTORY.
void send_history_to(const uint8_t* mac) {
    const uint8_t total = wb_state::history_count();
    for (uint8_t i = 0; i < total; ++i) {
        const auto& h = wb_state::history(i);
        ScoreboardMessage msg = {};
        msg.msg_type = MSG_HISTORY;
        msg.sequence = ++g_tx_seq;
        msg.rssi     = g_last_rx_rssi;
        msg.body.history.index               = i;
        msg.body.history.total               = total;
        msg.body.history.timestamp_unix      = h.timestamp_unix;
        msg.body.history.preset_idx          = h.preset_idx;
        msg.body.history.home_score_real     = h.home_score_real;
        msg.body.history.away_score_real     = h.away_score_real;
        msg.body.history.final_clock_seconds = h.final_clock_seconds;
        msg.body.history.goal_count          = h.goal_count;
        memcpy(msg.body.history.opponent, h.opponent,
               sizeof(msg.body.history.opponent));
        scoreboard_msg_sign(&msg);
        esp_now_send(mac, (const uint8_t*)&msg, sizeof(msg));
        // Brief spacer so the receiver's RX queue doesn't drop packets
        // when we burst all 5 in a row.
        delay(20);
    }
}

void factory_reset() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.clear();
    prefs.end();
    Serial.println("[espnow] factory reset, restarting…");
    delay(500);
    ESP.restart();
}

// ============================================================================
//  Internal — recv path
// ============================================================================
static void on_recv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (len < (int)sizeof(ScoreboardMessage)) return;
    ScoreboardMessage msg;
    memcpy(&msg, data, sizeof(msg));
    if (!scoreboard_msg_valid(&msg)) return;

    const uint8_t* from = info->src_addr;
    g_last_rx_rssi = info->rx_ctrl ? info->rx_ctrl->rssi : -127;
    wb_state::note_radio_link(true, g_last_rx_rssi);

    switch (msg.msg_type) {
    case MSG_PAIRING_REQ:
        handle_pairing(msg, from);
        break;
    case MSG_INTENT:
        // Only paired peers may send intents (security + accidental
        // cross-pairing protection).
        if (find_peer(from) < 0) {
            Serial.printf("[espnow] intent from unknown peer ignored\n");
            return;
        }
        handle_intent(msg, from);
        break;
    default:
        break;
    }
}

static void handle_intent(const ScoreboardMessage& msg, const uint8_t* from) {
    const int idx = find_peer(from);
    if (idx < 0) return;   // already filtered in on_recv but belt-and-braces.

    // Replay dedup: each peer ships monotonic sequence numbers. If we've
    // already applied this one (e.g. a retransmit), ACK again but don't
    // mutate state. Catches double-applies on flaky links.
    Peer& peer = g_peers[idx];
    if (peer.has_intent_seq && msg.sequence == peer.last_intent_seq) {
        send_intent_ack(from, msg.sequence, true);
        return;
    }
    peer.last_intent_seq = msg.sequence;
    peer.has_intent_seq  = true;

    wb_state::note_intent_received();
    const bool changed = wb_state::apply_intent(msg.body.intent);

    // Always ACK so the controller knows the intent landed.
    send_intent_ack(from, msg.sequence, changed);

    if (changed) {
        // Push fresh state to everyone right away — no waiting for the
        // 1Hz heartbeat tick.
        broadcast_state_now();
    } else if (msg.body.intent.intent_type == INTENT_REQUEST_FULL) {
        broadcast_state_now();
        broadcast_defaults_now();
    } else if (msg.body.intent.intent_type == INTENT_REQUEST_HISTORY) {
        send_history_to(from);
    } else if (msg.body.intent.intent_type == INTENT_SET_DEFAULTS) {
        broadcast_defaults_now();
        broadcast_state_now();
    }
}

static void handle_pairing(const ScoreboardMessage& msg, const uint8_t* from) {
    PairingReason reason = PAIR_OK;

    const int existing = find_peer(from);
    if (existing >= 0) {
        // Already known peer asking to pair again — just confirm.
        reason = PAIR_ALREADY_PAIRED;
    } else if (!g_pairing_mode) {
        reason = PAIR_NOT_IN_MODE;
    } else if (g_peer_count >= MAX_PAIRED_PEERS) {
        reason = PAIR_TABLE_FULL;
    } else {
        if (add_peer(from)) {
            Serial.printf("[espnow] PAIRED #%u: %02X:%02X:%02X:%02X:%02X:%02X\n",
                          g_peer_count, from[0], from[1], from[2], from[3], from[4], from[5]);
            wb_state::set_paired_peer_count(g_peer_count);
            if (wb_state::wb_mode() == wb_state::WB_PAIRING) {
                wb_state::set_wb_mode(wb_state::WB_PAIRED_IDLE);
            }
            // After accepting one peer, stay in pairing mode for the
            // remainder of the window so multiple controllers can join.
        } else {
            reason = PAIR_TABLE_FULL;
        }
    }

    send_pairing_ack(from, reason);

    // Newly paired peer should get an immediate snapshot so its UI fills in.
    if (reason == PAIR_OK || reason == PAIR_ALREADY_PAIRED) {
        broadcast_state_now();
        broadcast_defaults_now();
    }
}

// ============================================================================
//  Peer table helpers
// ============================================================================
static int find_peer(const uint8_t* mac) {
    for (uint8_t i = 0; i < MAX_PAIRED_PEERS; ++i) {
        if (g_peers[i].in_use && memcmp(g_peers[i].mac, mac, 6) == 0) return (int)i;
    }
    return -1;
}

static bool add_peer(const uint8_t* mac) {
    if (g_peer_count >= MAX_PAIRED_PEERS) return false;
    for (uint8_t i = 0; i < MAX_PAIRED_PEERS; ++i) {
        if (!g_peers[i].in_use) {
            memcpy(g_peers[i].mac, mac, 6);
            g_peers[i].in_use = true;
            g_peers[i].last_rssi = g_last_rx_rssi;
            if (!register_peer(mac)) {
                g_peers[i].in_use = false;
                return false;
            }
            g_peer_count++;
            save_peer_table();
            return true;
        }
    }
    return false;
}

static bool register_peer(const uint8_t* mac) {
    if (esp_now_is_peer_exist(mac)) return true;
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;
    peer.encrypt = true;                // encrypted unicast with controllers
    memcpy(peer.lmk, kEspNowLmk, 16);
    return esp_now_add_peer(&peer) == ESP_OK;
}

static void load_peer_table() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    const size_t n = prefs.getBytesLength("peers");
    if (n == sizeof(g_peers)) {
        prefs.getBytes("peers", g_peers, sizeof(g_peers));
    } else {
        memset(g_peers, 0, sizeof(g_peers));
    }
    prefs.end();

    g_peer_count = 0;
    for (uint8_t i = 0; i < MAX_PAIRED_PEERS; ++i) {
        if (g_peers[i].in_use) {
            register_peer(g_peers[i].mac);
            g_peer_count++;
        }
    }
    wb_state::set_paired_peer_count(g_peer_count);
}

static void save_peer_table() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBytes("peers", g_peers, sizeof(g_peers));
    prefs.end();
}

// ============================================================================
//  TX helpers
// ============================================================================
static void send_pairing_invite_broadcast() {
    ScoreboardMessage msg = {};
    msg.msg_type = MSG_PAIRING_REQ;   // wallbox uses same type to advertise availability
    msg.sequence = ++g_tx_seq;
    msg.body.pairing.reason = PAIR_OK;
    strncpy(msg.body.pairing.friendly_name, "Wallbox",
            sizeof(msg.body.pairing.friendly_name) - 1);
    scoreboard_msg_sign(&msg);
    esp_now_send(BROADCAST_MAC, (uint8_t*)&msg, sizeof(msg));
}

static void send_pairing_ack(const uint8_t* to, PairingReason reason) {
    ScoreboardMessage msg = {};
    msg.msg_type = MSG_PAIRING_ACK;
    msg.sequence = ++g_tx_seq;
    msg.body.pairing.reason = reason;
    strncpy(msg.body.pairing.friendly_name, "Wallbox",
            sizeof(msg.body.pairing.friendly_name) - 1);
    scoreboard_msg_sign(&msg);
    esp_now_send(to, (uint8_t*)&msg, sizeof(msg));
}

static void send_intent_ack(const uint8_t* to, uint16_t intent_seq, bool ok) {
    ScoreboardMessage msg = {};
    msg.msg_type = MSG_INTENT_ACK;
    msg.sequence = ++g_tx_seq;
    msg.rssi     = g_last_rx_rssi;
    msg.body.ack.intent_sequence = intent_seq;
    msg.body.ack.ok = ok ? 1 : 0;
    scoreboard_msg_sign(&msg);
    esp_now_send(to, (uint8_t*)&msg, sizeof(msg));
}

static void broadcast_to_all_peers(const ScoreboardMessage& msg) {
    for (uint8_t i = 0; i < MAX_PAIRED_PEERS; ++i) {
        if (!g_peers[i].in_use) continue;
        esp_now_send(g_peers[i].mac, (const uint8_t*)&msg, sizeof(msg));
    }
}

} // namespace espnow_server

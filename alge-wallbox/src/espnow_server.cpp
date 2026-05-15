// ============================================================================
//  ESP-NOW receiver for the wall-box.
// ============================================================================
#include "espnow_server.h"
#include "config.h"
#include "state.h"
#include "gaz4.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <string.h>

namespace espnow_server {

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static uint8_t g_paired_mac[6] = {0};
static bool    g_have_pair = false;
static uint16_t g_tx_seq = 0;
static uint32_t g_last_heartbeat_ms = 0;
static int8_t  g_last_rx_rssi = -127;

// Forward decls
static void on_recv(const esp_now_recv_info_t* info, const uint8_t* data, int len);
static void handle_state(const ScoreboardMessage& msg, const uint8_t* from);
static void handle_cmd(const ScoreboardMessage& msg, const uint8_t* from);
static void handle_pairing(const ScoreboardMessage& msg, const uint8_t* from);

static bool load_paired_mac();
static void save_paired_mac(const uint8_t* mac);
static void clear_paired_mac();
static bool register_peer(const uint8_t* mac);
static void send_pairing_broadcast();
static void send_pairing_response(const uint8_t* to);

// Forward declared in maintenance.cpp
extern "C" void wb_maintenance_handle_cmd(const ScoreboardMessage& msg);

// ----------------------------------------------------------------------------
//  Public API
// ----------------------------------------------------------------------------
void begin() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    // Lock to channel 1 so the SoftAP added later by ota.cpp doesn't
    // disturb ESP-NOW peers. Peers use channel 0 = "current STA channel".
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println("[espnow] init FAILED");
        wb_state::set_wb_mode(wb_state::WB_ERROR);
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
        if (register_peer(g_paired_mac)) {
            wb_state::set_wb_mode(wb_state::WB_PAIRED_IDLE);
            Serial.printf("[espnow] paired with %02X:%02X:%02X:%02X:%02X:%02X\n",
                g_paired_mac[0], g_paired_mac[1], g_paired_mac[2],
                g_paired_mac[3], g_paired_mac[4], g_paired_mac[5]);
        } else {
            wb_state::set_wb_mode(wb_state::WB_PAIRING);
        }
    } else {
        wb_state::set_wb_mode(wb_state::WB_PAIRING);
    }
}

void loop() {
    const uint32_t now = millis();

    // Pairing mode: broadcast invitations once a second.
    if (wb_state::wb_mode() == wb_state::WB_PAIRING) {
        if (now - g_last_heartbeat_ms >= ESPNOW_PAIRING_BROADCAST_MS) {
            g_last_heartbeat_ms = now;
            send_pairing_broadcast();
        }
        return;
    }

    // Normal: heartbeat 1Hz back to the controller.
    if (g_have_pair && now - g_last_heartbeat_ms >= ESPNOW_HEARTBEAT_MS) {
        g_last_heartbeat_ms = now;
        send_heartbeat();
    }
}

bool is_paired() { return g_have_pair; }
const uint8_t* paired_mac() { return g_paired_mac; }

void enter_pairing_mode() {
    clear_paired_mac();
    g_have_pair = false;
    if (esp_now_is_peer_exist(g_paired_mac)) {
        esp_now_del_peer(g_paired_mac);
    }
    wb_state::set_wb_mode(wb_state::WB_PAIRING);
    Serial.println("[espnow] entering pairing mode");
}

void factory_reset() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.clear();
    prefs.end();
    Serial.println("[espnow] factory reset, restarting...");
    delay(500);
    ESP.restart();
}

void send_ack(const uint8_t* peer_mac, uint16_t ack_seq, int8_t rssi) {
    ScoreboardMessage msg = {};
    msg.msg_type     = MSG_ACK;
    msg.sequence     = ++g_tx_seq;
    msg.ack_sequence = ack_seq;
    msg.rssi         = rssi;
    scoreboard_msg_sign(&msg);
    esp_now_send(peer_mac, (uint8_t*)&msg, sizeof(msg));
}

void send_heartbeat() {
    if (!g_have_pair) return;
    const auto snap = wb_state::snapshot();
    ScoreboardMessage msg = {};
    msg.msg_type      = MSG_HEARTBEAT;
    msg.sequence      = ++g_tx_seq;
    msg.match_state   = snap.match_state;
    msg.home_score    = snap.home_score;
    msg.away_score    = snap.away_score;
    msg.clock_seconds = snap.clock_seconds;
    msg.flags         = snap.clock_running ? FLAG_CLOCK_RUNNING : 0;
    msg.rssi          = g_last_rx_rssi;
    scoreboard_msg_sign(&msg);
    esp_now_send(g_paired_mac, (uint8_t*)&msg, sizeof(msg));
}

// ----------------------------------------------------------------------------
//  Receive callback
// ----------------------------------------------------------------------------
static void on_recv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (len < (int)sizeof(ScoreboardMessage)) return;
    ScoreboardMessage msg;
    memcpy(&msg, data, sizeof(msg));
    if (!scoreboard_msg_valid(&msg)) return;

    const uint8_t* from = info->src_addr;
    g_last_rx_rssi = info->rx_ctrl ? info->rx_ctrl->rssi : -127;
    wb_state::note_radio_link(true, g_last_rx_rssi);

    // During pairing, accept any peer. Otherwise enforce paired MAC.
    const bool pairing_mode = (wb_state::wb_mode() == wb_state::WB_PAIRING);
    if (!pairing_mode) {
        if (!g_have_pair || memcmp(from, g_paired_mac, 6) != 0) return;
    }

    switch (msg.msg_type) {
        case MSG_PAIRING: handle_pairing(msg, from); break;
        case MSG_STATE:   handle_state(msg, from);   break;
        case MSG_CMD:     handle_cmd(msg, from);     break;
        default: break;
    }
}

static void handle_state(const ScoreboardMessage& msg, const uint8_t* from) {
    wb_state::apply_state_message(msg);
    send_ack(from, msg.sequence, g_last_rx_rssi);
}

static void handle_cmd(const ScoreboardMessage& msg, const uint8_t* from) {
    Serial.printf("[espnow] CMD type=%u arg=%u\n", msg.cmd_type, msg.cmd_arg);
    wb_maintenance_handle_cmd(msg);
    send_ack(from, msg.sequence, g_last_rx_rssi);
}

static void handle_pairing(const ScoreboardMessage& msg, const uint8_t* from) {
    if (g_have_pair) {
        // Already paired but received another pairing request: ignore unless
        // the user has explicitly entered pairing mode.
        if (wb_state::wb_mode() != wb_state::WB_PAIRING) return;
    }
    memcpy(g_paired_mac, from, 6);
    if (register_peer(g_paired_mac)) {
        save_paired_mac(g_paired_mac);
        g_have_pair = true;
        wb_state::set_wb_mode(wb_state::WB_PAIRED_IDLE);
        Serial.printf("[espnow] PAIRED with %02X:%02X:%02X:%02X:%02X:%02X\n",
            g_paired_mac[0], g_paired_mac[1], g_paired_mac[2],
            g_paired_mac[3], g_paired_mac[4], g_paired_mac[5]);
        // Confirm with a response message so controller can save us too.
        send_pairing_response(g_paired_mac);
    }
}

// ----------------------------------------------------------------------------
//  Helpers
// ----------------------------------------------------------------------------
static bool register_peer(const uint8_t* mac) {
    if (esp_now_is_peer_exist(mac)) return true;
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;
    peer.encrypt = false;
    return esp_now_add_peer(&peer) == ESP_OK;
}

static void send_pairing_broadcast() {
    ScoreboardMessage msg = {};
    msg.msg_type = MSG_PAIRING;
    msg.sequence = ++g_tx_seq;
    scoreboard_msg_sign(&msg);
    esp_now_send(BROADCAST_MAC, (uint8_t*)&msg, sizeof(msg));
}

static void send_pairing_response(const uint8_t* to) {
    ScoreboardMessage msg = {};
    msg.msg_type = MSG_PAIRING;
    msg.sequence = ++g_tx_seq;
    scoreboard_msg_sign(&msg);
    esp_now_send(to, (uint8_t*)&msg, sizeof(msg));
}

static bool load_paired_mac() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    const size_t n = prefs.getBytes("paired_mac", g_paired_mac, 6);
    prefs.end();
    if (n != 6) return false;
    for (int i = 0; i < 6; ++i) if (g_paired_mac[i] != 0) return true;
    return false;
}

static void save_paired_mac(const uint8_t* mac) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBytes("paired_mac", mac, 6);
    prefs.end();
}

static void clear_paired_mac() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.remove("paired_mac");
    prefs.end();
    memset(g_paired_mac, 0, 6);
}

} // namespace espnow_server

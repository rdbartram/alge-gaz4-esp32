// ============================================================================
//  Controller ESP-NOW client.
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

static uint8_t   g_paired_mac[6] = {0};
static bool      g_have_pair = false;
static uint16_t  g_tx_seq = 0;
static int8_t    g_last_rssi = -127;
static uint32_t  g_last_recv_ms = 0;
static uint32_t  g_last_state_sent_ms = 0;
static uint32_t  g_last_pairing_broadcast_ms = 0;
static bool      g_pairing_mode = false;

static void on_recv(const esp_now_recv_info_t* info, const uint8_t* data, int len);
static bool load_paired_mac();
static void save_paired_mac(const uint8_t* mac);
static void clear_paired_mac();
static bool register_peer(const uint8_t* mac);

void begin() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    // Lock to channel 1 so the SoftAP added later by web.cpp doesn't
    // disturb ESP-NOW.
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    if (esp_now_init() != ESP_OK) {
        Serial.println("[espnow] init FAILED");
        return;
    }
    esp_now_register_recv_cb(on_recv);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST_MAC, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    g_have_pair = load_paired_mac();
    if (g_have_pair) {
        register_peer(g_paired_mac);
        Serial.printf("[espnow] paired %02X:%02X:%02X:%02X:%02X:%02X\n",
            g_paired_mac[0], g_paired_mac[1], g_paired_mac[2],
            g_paired_mac[3], g_paired_mac[4], g_paired_mac[5]);
    } else {
        g_pairing_mode = true;
        Serial.println("[espnow] no peer saved, entering pairing mode");
    }
}

void enter_pairing_mode() {
    if (g_have_pair && esp_now_is_peer_exist(g_paired_mac)) esp_now_del_peer(g_paired_mac);
    clear_paired_mac();
    g_have_pair = false;
    g_pairing_mode = true;
}

void loop() {
    const uint32_t now = millis();
    if (g_pairing_mode && (now - g_last_pairing_broadcast_ms >= 1000)) {
        g_last_pairing_broadcast_ms = now;
        ScoreboardMessage msg = {};
        msg.msg_type = MSG_PAIRING;
        msg.sequence = ++g_tx_seq;
        scoreboard_msg_sign(&msg);
        esp_now_send(BROADCAST_MAC, (uint8_t*)&msg, sizeof(msg));
    }

    // Periodic state push while a match is live.
    if (g_have_pair && !g_pairing_mode) {
        const auto& s = state::peek();
        const bool live =
            s.match_state != STATE_IDLE && s.match_state != STATE_SETUP;
        if (live && now - g_last_state_sent_ms >= ESPNOW_STATE_PUSH_MS) {
            send_state_now();
        }
    }
}

bool is_paired() { return g_have_pair; }
const uint8_t* paired_mac() { return g_paired_mac; }
int8_t  last_rssi() { return g_last_rssi; }
uint32_t last_state_sent_ms() { return g_last_state_sent_ms; }

bool link_ok() {
    if (!g_have_pair) return false;
    return (millis() - g_last_recv_ms) < (ESPNOW_RESEND_INTERVAL_MS * 3);
}

void send_state_now() {
    if (!g_have_pair) return;
    const auto& s = state::peek();
    ScoreboardMessage msg = {};
    msg.msg_type = MSG_STATE;
    msg.sequence = ++g_tx_seq;
    msg.match_state = s.match_state;
    msg.home_score = state::home_score_board();
    msg.away_score = state::away_score_board();
    msg.clock_seconds = s.clock_seconds;
    msg.flags = s.clock_running ? FLAG_CLOCK_RUNNING : 0;
    scoreboard_msg_sign(&msg);
    esp_now_send(g_paired_mac, (uint8_t*)&msg, sizeof(msg));
    g_last_state_sent_ms = millis();
}

void send_command(uint8_t cmd_type, uint8_t arg, const char* data) {
    if (!g_have_pair) return;
    ScoreboardMessage msg = {};
    msg.msg_type = MSG_CMD;
    msg.sequence = ++g_tx_seq;
    msg.cmd_type = cmd_type;
    msg.cmd_arg = arg;
    if (data) memcpy(msg.cmd_data, data, sizeof(msg.cmd_data));
    scoreboard_msg_sign(&msg);
    esp_now_send(g_paired_mac, (uint8_t*)&msg, sizeof(msg));
}

static void on_recv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (len < (int)sizeof(ScoreboardMessage)) return;
    ScoreboardMessage msg;
    memcpy(&msg, data, sizeof(msg));
    if (!scoreboard_msg_valid(&msg)) return;

    g_last_rssi = info->rx_ctrl ? info->rx_ctrl->rssi : -127;
    g_last_recv_ms = millis();

    switch (msg.msg_type) {
    case MSG_PAIRING: {
        if (!g_have_pair || g_pairing_mode) {
            memcpy(g_paired_mac, info->src_addr, 6);
            register_peer(g_paired_mac);
            save_paired_mac(g_paired_mac);
            g_have_pair = true;
            g_pairing_mode = false;
            Serial.printf("[espnow] PAIRED %02X:%02X:%02X:%02X:%02X:%02X\n",
                g_paired_mac[0], g_paired_mac[1], g_paired_mac[2],
                g_paired_mac[3], g_paired_mac[4], g_paired_mac[5]);
            // Echo back so wall-box also confirms.
            ScoreboardMessage resp = {};
            resp.msg_type = MSG_PAIRING;
            resp.sequence = ++g_tx_seq;
            scoreboard_msg_sign(&resp);
            esp_now_send(g_paired_mac, (uint8_t*)&resp, sizeof(resp));
        }
        break;
    }
    case MSG_HEARTBEAT:
    case MSG_ACK:
        // Connection-alive signal — nothing else to do.
        break;
    default:
        break;
    }
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

} // namespace espnow_client

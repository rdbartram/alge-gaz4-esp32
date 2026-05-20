// ============================================================================
//  Controller-side firmware-update receiver — see header for design.
// ============================================================================
#include "client_ota.h"
#include "config.h"
#include "credentials.h"

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>

namespace client_ota {

static FwAvailPayload g_offer       = {};
static bool           g_has_offer   = false;
static Phase          g_phase       = PHASE_IDLE;
static uint32_t       g_bytes_recv  = 0;
static uint32_t       g_bytes_total = 0;
static char           g_err_msg[80] = {0};

void begin() {
    g_phase     = PHASE_IDLE;
    g_has_offer = false;
}

void note_offer(const FwAvailPayload& offer) {
    g_offer     = offer;
    g_has_offer = true;
    Serial.printf("[ota] offer received: build=%u size=%u md5=%s\n",
                  (unsigned)offer.build_code, (unsigned)offer.size_bytes,
                  offer.md5_hex);
}

bool has_offer()                  { return g_has_offer; }
const FwAvailPayload& offer()     { return g_offer; }
void dismiss_offer()              { g_has_offer = false; }

Phase    phase()         { return g_phase; }
uint32_t bytes_received(){ return g_bytes_recv; }
uint32_t bytes_total()   { return g_bytes_total; }
const char* error_message() { return g_err_msg; }

static void fail(const char* msg) {
    strncpy(g_err_msg, msg, sizeof(g_err_msg) - 1);
    g_phase = PHASE_ERROR;
    Serial.printf("[ota] %s\n", msg);
}

void perform_update() {
    if (!g_has_offer) return;

    // Phase 1 — join the wall-box AP. We hold open the ESP-NOW STA so
    // the radio doesn't get reset; on the ESP32, calling WiFi.begin()
    // from STA mode just adds an AP association. The wall-box's SoftAP
    // is on channel 1, same as our ESP-NOW peer table, so coexistence
    // is automatic.
    g_phase       = PHASE_JOIN_AP;
    g_bytes_recv  = 0;
    g_bytes_total = g_offer.size_bytes;
    g_err_msg[0]  = '\0';

    WiFi.begin(WIFI_AP_SSID, WIFI_AP_PASSWORD);
    Serial.printf("[ota] joining %s...\n", WIFI_AP_SSID);
    uint32_t join_t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - join_t0 < 15000) {
        delay(250);
    }
    if (WiFi.status() != WL_CONNECTED) {
        fail("AP join timeout");
        return;
    }
    Serial.printf("[ota] joined, local IP %s\n", WiFi.localIP().toString().c_str());

    // Phase 2 — HTTP-GET the binary and stream it into Update.h
    g_phase = PHASE_DOWNLOAD;
    HTTPClient http;
    char url[64];
    snprintf(url, sizeof(url), "http://192.168.4.1%s", g_offer.fetch_path);
    http.begin(url);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        char m[64];
        snprintf(m, sizeof(m), "HTTP %d", code);
        http.end();
        WiFi.disconnect();
        fail(m);
        return;
    }
    const int total = http.getSize();
    if (total <= 0) {
        http.end();
        WiFi.disconnect();
        fail("zero-byte response");
        return;
    }
    g_bytes_total = total;

    if (!Update.begin(total)) {
        http.end();
        WiFi.disconnect();
        fail(Update.errorString());
        return;
    }
    // Optional but cheap belt-and-braces: ask Update to verify MD5 as
    // it writes. If the wall-box's hash didn't match the bytes we
    // received, Update.end() returns false with MD5 error.
    if (g_offer.md5_hex[0]) Update.setMD5(g_offer.md5_hex);

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[1024];
    while (http.connected() && (int)g_bytes_recv < total) {
        size_t avail = stream->available();
        if (avail) {
            size_t got = stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
            if (Update.write(buf, got) != got) {
                http.end();
                WiFi.disconnect();
                fail(Update.errorString());
                return;
            }
            g_bytes_recv += got;
        } else {
            delay(1);
        }
    }
    http.end();
    WiFi.disconnect();

    if (!Update.end(true)) {
        fail(Update.errorString());
        return;
    }

    Serial.println("[ota] flash committed — rebooting");
    g_phase = PHASE_REBOOT;
    delay(500);
    ESP.restart();
}

} // namespace client_ota

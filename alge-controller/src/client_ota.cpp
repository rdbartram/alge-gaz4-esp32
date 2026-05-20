// ============================================================================
//  Controller-side firmware-update receiver — non-blocking state machine.
//
//  Stepped from ui::tick() while on SCREEN_OTA_UPDATE so the LCD can
//  refresh between chunks. Phases:
//      IDLE      -> JOIN_AP -> DOWNLOAD -> REBOOT (= ESP.restart())
//                                      \-> ERROR
//  Single step() call performs at most ~1 chunk of work (≈1 KB HTTP
//  read + Update.write()) so the UI redraws ~30+ times per second
//  during the bulk transfer.
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

// Phase 2 (download) state — kept across step() calls.
static HTTPClient g_http;
static WiFiClient* g_stream = nullptr;
static uint32_t   g_phase_t0 = 0;

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
    // Tear down anything we opened mid-flight.
    g_http.end();
    if (WiFi.status() == WL_CONNECTED) WiFi.disconnect();
    g_stream = nullptr;
}

void perform_update() {
    if (!g_has_offer) return;
    g_phase       = PHASE_JOIN_AP;
    g_bytes_recv  = 0;
    g_bytes_total = g_offer.size_bytes;
    g_err_msg[0]  = '\0';
    g_phase_t0    = millis();

    Serial.printf("[ota] joining %s...\n", WIFI_AP_SSID);
    WiFi.begin(WIFI_AP_SSID, WIFI_AP_PASSWORD);
}

void step() {
    switch (g_phase) {
    case PHASE_IDLE:
    case PHASE_REBOOT:
    case PHASE_ERROR:
        return;

    case PHASE_JOIN_AP: {
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[ota] joined, local IP %s\n",
                          WiFi.localIP().toString().c_str());

            // Open the HTTP request — getStreamPtr() lets us read bytes
            // a chunk at a time from step() without blocking.
            char url[64];
            snprintf(url, sizeof(url), "http://192.168.4.1%s",
                     g_offer.fetch_path);
            g_http.begin(url);
            int code = g_http.GET();
            if (code != HTTP_CODE_OK) {
                char m[64];
                snprintf(m, sizeof(m), "HTTP %d", code);
                fail(m);
                return;
            }
            const int total = g_http.getSize();
            if (total <= 0) { fail("zero-byte response"); return; }
            g_bytes_total = total;

            if (!Update.begin(total)) {
                fail(Update.errorString());
                return;
            }
            if (g_offer.md5_hex[0]) Update.setMD5(g_offer.md5_hex);

            g_stream = g_http.getStreamPtr();
            g_phase  = PHASE_DOWNLOAD;
            return;
        }
        if (millis() - g_phase_t0 > 15000) {
            fail("AP join timeout");
        }
        return;
    }

    case PHASE_DOWNLOAD: {
        if (!g_http.connected()) { fail("disconnected"); return; }
        uint8_t buf[1024];
        size_t avail = g_stream->available();
        if (avail) {
            size_t got = g_stream->readBytes(
                buf, avail > sizeof(buf) ? sizeof(buf) : avail);
            if (Update.write(buf, got) != got) {
                fail(Update.errorString());
                return;
            }
            g_bytes_recv += got;
        }
        if ((int)g_bytes_recv >= (int)g_bytes_total) {
            g_http.end();
            WiFi.disconnect();
            g_stream = nullptr;
            if (!Update.end(true)) {
                fail(Update.errorString());
                return;
            }
            Serial.println("[ota] flash committed — rebooting");
            g_phase = PHASE_REBOOT;
            delay(500);
            ESP.restart();
        }
        return;
    }
    }
}

} // namespace client_ota

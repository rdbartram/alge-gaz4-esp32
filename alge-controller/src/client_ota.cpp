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

    // WALLBOX_AP_SSID — explicitly the wall-box's SoftAP, not the
    // controller's own AP. Earlier this used WIFI_AP_SSID which
    // resolved to the controller's own AP name ("FC-Waengi-Tafel"),
    // a network that never existed for the controller to join — every
    // OTA attempt timed out at the 15 s mark.
    Serial.printf("[ota] joining %s...\n", WALLBOX_AP_SSID);
    WiFi.begin(WALLBOX_AP_SSID, WALLBOX_AP_PASSWORD);
}

void step() {
    switch (g_phase) {
    case PHASE_IDLE:
    case PHASE_REBOOT:
    case PHASE_ERROR:
        return;

    case PHASE_JOIN_AP: {
        if (WiFi.status() != WL_CONNECTED) {
            if (millis() - g_phase_t0 > 15000) fail("AP join timeout");
            return;
        }
        // Associated, but DHCP may not have finished yet — WiFi.status()
        // flips to WL_CONNECTED on association, the lease arrives a few
        // hundred ms later. If we begin HTTP before localIP is set, the
        // TCP connect returns -1 (CONNECTION_REFUSED). Wait for a non-
        // zero address before moving on.
        if (WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
            if (millis() - g_phase_t0 > 15000) fail("no DHCP lease");
            return;
        }
        const IPAddress gw = WiFi.gatewayIP();
        Serial.printf("[ota] joined, IP %s gw %s — settling 1 s\n",
                      WiFi.localIP().toString().c_str(),
                      gw.toString().c_str());
        delay(1000);

        // Raw TCP probe before HTTP — tells us whether the failure is
        // at the TCP layer (kernel refusing, route wrong, max_conn
        // full) or only at the HTTP layer. If we can't even open a
        // socket to gw:80, retrying HTTP will just produce the same
        // -1 over and over.
        WiFiClient probe;
        if (!probe.connect(gw, 80, 4000)) {
            // Try a slightly longer connect cycle before giving up —
            // the wall-box's accept loop might still be catching up.
            delay(1500);
            if (!probe.connect(gw, 80, 4000)) {
                char m[80];
                snprintf(m, sizeof(m), "TCP %s:80 closed (we are %s)",
                         gw.toString().c_str(),
                         WiFi.localIP().toString().c_str());
                fail(m);
                return;
            }
        }
        probe.stop();
        Serial.println("[ota] TCP probe OK — proceeding to HTTP GET");

        char url[64];
        snprintf(url, sizeof(url), "http://%s%s",
                 gw.toString().c_str(), g_offer.fetch_path);
        int code = -1;
        for (int attempt = 0; attempt < 5 && code != HTTP_CODE_OK; ++attempt) {
            if (attempt > 0) {
                g_http.end();
                delay(1000);
            }
            g_http.begin(url);
            g_http.setConnectTimeout(8000);
            g_http.setTimeout(10000);
            code = g_http.GET();
            Serial.printf("[ota] GET %s -> %d (attempt %d)\n",
                          url, code, attempt + 1);
        }
        if (code != HTTP_CODE_OK) {
            char m[80];
            snprintf(m, sizeof(m), "HTTP %d @ %s",
                     code, WiFi.localIP().toString().c_str());
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

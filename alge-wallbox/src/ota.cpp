// ============================================================================
//  Wall-box AP + OTA implementation.
// ============================================================================
#include "ota.h"
#include "config.h"
#include "credentials.h"

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>

namespace wb_ota {

static constexpr const char* AP_SSID      = WIFI_AP_SSID;
static constexpr const char* AP_PASSWORD  = WIFI_AP_PASSWORD;
static constexpr uint8_t     CHANNEL      = 1;
static constexpr const char* HOSTNAME     = OTA_HOSTNAME;
static constexpr const char* OTA_PW       = OTA_PASSWORD;

void begin() {
    // Stay AP-only (we don't connect to any infra WiFi). ESP-NOW runs on
    // STA mode internally — that's already set up in espnow_server::begin().
    // We add SoftAP on top so OTA reachability doesn't require any nearby
    // router. Both interfaces must share a channel.
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASSWORD, CHANNEL, /*hidden=*/0, /*max_conn=*/2);
    WiFi.setHostname(HOSTNAME);

    Serial.printf("[ota] SoftAP up: SSID=%s  IP=%s\n", AP_SSID,
                  WiFi.softAPIP().toString().c_str());

    ArduinoOTA.setHostname(HOSTNAME);
    ArduinoOTA.setPassword(OTA_PW);
    ArduinoOTA.onStart([]() {
        Serial.println("[ota] update starting...");
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("[ota] update complete");
    });
    ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
        if ((p % (t / 10 + 1)) < 1024) {
            Serial.printf("[ota] %u%%\n", (p * 100) / t);
        }
    });
    ArduinoOTA.onError([](ota_error_t e) {
        Serial.printf("[ota] error %u\n", (unsigned)e);
    });
    ArduinoOTA.begin();
}

void loop() {
    ArduinoOTA.handle();
}

} // namespace wb_ota

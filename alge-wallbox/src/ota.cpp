// ============================================================================
//  Wall-box AP + HTTP-push OTA implementation.
//
//  We deliberately don't use ArduinoOTA / espota.py here: that flow has
//  the device dial back to the host's TCP port for the binary stream,
//  which means the host needs to *accept* inbound TCP. Under a managed
//  Mac (MDM-locked Application Firewall) those connections are silently
//  dropped and the upload hangs at "No response from device".
//
//  Instead the wall-box runs a tiny HTTP server on port 80 of its
//  SoftAP. The host just POSTs the firmware binary to /update — that's
//  host → device only, works through any normal outbound-firewall
//  policy, and only needs `curl` on the laptop side.
// ============================================================================
#include "ota.h"
#include "config.h"
#include "credentials.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>

namespace wb_ota {

static constexpr const char* AP_SSID      = WIFI_AP_SSID;
static constexpr const char* AP_PASSWORD  = WIFI_AP_PASSWORD;
static constexpr uint8_t     CHANNEL      = 1;
static constexpr const char* HOSTNAME     = OTA_HOSTNAME;
static constexpr const char* OTA_PW       = OTA_PASSWORD;

static WebServer server(80);

static void handle_root() {
    // Minimal landing page so a stray browser visit to 192.168.4.1
    // doesn't look like the box is broken. POST /update is the real
    // entry point.
    server.send(200, "text/html",
        "<html><body><h2>alge-wallbox</h2>"
        "<p>OTA: <code>POST /update</code> with the firmware binary.</p>"
        "<p>Use <code>tools/ota_flash_wallbox.sh</code>.</p>"
        "</body></html>");
}

static void handle_update_post() {
    // Final reply once the upload has finished streaming.
    const bool ok = !Update.hasError();
    server.send(ok ? 200 : 500, "text/plain",
                ok ? "OK — rebooting\n" : "FAIL\n");
    if (ok) {
        Serial.println("[ota] update succeeded — rebooting in 500 ms");
        delay(500);
        ESP.restart();
    }
}

static void handle_upload() {
    // Basic auth gates the chunk handler too — without this any client
    // on the AP could upload arbitrary firmware. WebServer.authenticate
    // returns false and we drop the chunk silently. (The /update POST
    // handler does its own authenticate() too, for symmetry, but the
    // chunked-upload callback fires first so the check has to be here.)
    if (!server.authenticate("ota", OTA_PW)) {
        return;
    }
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("[ota] upload start: %s\n", upload.filename.c_str());
        // U_FLASH = main app partition. UPDATE_SIZE_UNKNOWN lets Update
        // accept any size up to the partition limit (huge_app = ~3 MB).
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            Serial.printf("[ota] upload complete: %u bytes\n",
                          (unsigned)upload.totalSize);
        } else {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        Serial.println("[ota] upload aborted by client");
    }
}

void begin() {
    // Stay AP+STA (the STA side is already initialised by espnow_server::
    // begin()); the SoftAP is what hosts the OTA endpoint. ESP-NOW + AP
    // coexist on channel 1.
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASSWORD, CHANNEL, /*hidden=*/0, /*max_conn=*/2);
    WiFi.setHostname(HOSTNAME);

    Serial.printf("[ota] SoftAP up: SSID=%s  IP=%s\n", AP_SSID,
                  WiFi.softAPIP().toString().c_str());

    server.on("/", HTTP_GET, handle_root);
    server.on("/update", HTTP_POST,
              []() {
                  if (!server.authenticate("ota", OTA_PW)) {
                      return server.requestAuthentication();
                  }
                  handle_update_post();
              },
              handle_upload);
    server.begin();
    Serial.println("[ota] HTTP server listening on :80, POST /update");
}

void loop() {
    server.handleClient();
}

} // namespace wb_ota

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
#include "state.h"
#include "display.h"

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

// Progress signalling for the LCD. Volatile because handle_upload is
// driven from WebServer.handleClient() and the display reads these on
// the same core but between callbacks — keeps the compiler from caching
// either pointer across the chunk loop.
static volatile bool     g_in_progress    = false;
static volatile uint32_t g_bytes_received = 0;
static volatile uint32_t g_bytes_total    = 0;

bool     in_progress()    { return g_in_progress; }
uint32_t bytes_received() { return g_bytes_received; }
uint32_t bytes_total()    { return g_bytes_total; }

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
    // Final reply once the upload has finished streaming. Surface the
    // Update library's own error string in the body so the host script
    // can show the operator what actually went wrong instead of just
    // "status 500".
    const bool ok = !Update.hasError();
    if (ok) {
        server.send(200, "text/plain", "OK rebooting\n");
        Serial.println("[ota] update succeeded — rebooting in 500 ms");
        delay(500);
        ESP.restart();
    } else {
        char body[160];
        snprintf(body, sizeof(body), "FAIL err=%u: %s\n",
                 (unsigned)Update.getError(),
                 Update.errorString());
        server.send(500, "text/plain", body);
        Serial.printf("[ota] update FAILED: %s\n", body);
        // Clear the WB_OTA_UPDATE screen so we don't get stuck at "100 %"
        // forever. The main loop's mode auto-sync (tick() in state.cpp)
        // sees a non-OTA mode and routes us to either WB_MATCH_LIVE or
        // WB_PAIRED_IDLE based on the live match state.
        g_in_progress = false;
        wb_state::set_wb_mode(wb_state::WB_PAIRED_IDLE);
    }
}

static void handle_upload() {
    // Auth is provided by the SoftAP's WPA2 password (only paired
    // clients reach this endpoint at all). Skipping HTTP Basic Auth
    // here avoids edge cases with WebServer's multipart parser that
    // were producing 500s after a successful byte stream.
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("[ota] upload start: %s\n", upload.filename.c_str());
        g_in_progress    = true;
        g_bytes_received = 0;
        // Without a Content-Length header per multipart field we don't
        // get a precise total upfront. Falls back to UPDATE_SIZE_UNKNOWN
        // (we set 0 so the display renders an indeterminate bar). The
        // host-side script bakes the file size into a hint we *could*
        // pass as a header — left for later if it matters.
        g_bytes_total    = 0;
        wb_state::set_wb_mode(wb_state::WB_OTA_UPDATE);
        // U_FLASH = main app partition. UPDATE_SIZE_UNKNOWN lets Update
        // accept any size up to the partition limit (huge_app = ~3 MB).
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        }
        g_bytes_received += upload.currentSize;
        // Refresh the LCD between chunks so the user sees a moving
        // progress bar rather than a frozen "UPDATE…" screen.
        wb_display::tick();
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            Serial.printf("[ota] upload complete: %u bytes\n",
                          (unsigned)upload.totalSize);
        } else {
            Update.printError(Serial);
        }
        g_bytes_total = g_bytes_received;
        wb_display::tick();
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        g_in_progress = false;
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
    server.on("/update", HTTP_POST, handle_update_post, handle_upload);
    server.begin();
    Serial.println("[ota] HTTP server listening on :80, POST /update");
}

void loop() {
    server.handleClient();
}

} // namespace wb_ota

// ============================================================================
//  Controller SoftAP + score page + ArduinoOTA.
// ============================================================================
#include "web.h"
#include "config.h"
#include "state.h"
#include "credentials.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>

namespace web {

static constexpr const char* AP_SSID      = WIFI_AP_SSID;
static constexpr const char* AP_PASSWORD  = WIFI_AP_PASSWORD;
static constexpr uint8_t     CHANNEL      = 1;
static constexpr const char* HOSTNAME     = OTA_HOSTNAME;
static constexpr const char* OTA_PW       = OTA_PASSWORD;

static WebServer g_server(80);

static const char* match_label(MatchState s) {
    switch (s) {
    case STATE_IDLE:              return "Bereit";
    case STATE_SETUP:             return "Setup";
    case STATE_HALF_1:            return "1. Halbzeit";
    case STATE_PAUSED_H1:         return "1. HZ pausiert";
    case STATE_HALFTIME:          return "Halbzeitpause";
    case STATE_HALF_2:            return "2. Halbzeit";
    case STATE_PAUSED_H2:         return "2. HZ pausiert";
    case STATE_ENDED:             return "Endstand";
    case STATE_EXTRA_TIME_1:      return "Verlängerung 1. HZ";
    case STATE_EXTRA_TIME_2:      return "Verlängerung 2. HZ";
    case STATE_PAUSED_ET:         return "Verl. pausiert";
    case STATE_PENALTY_SHOOTOUT:  return "Penaltyschiessen";
    case STATE_PRE_MATCH:         return "Countdown zum Anstoss";
    default:                      return "—";
    }
}

static void handle_root() {
    const auto& s = state::peek();
    const uint16_t mm = (s.clock_seconds / 60) % 100;
    const uint16_t ss = s.clock_seconds % 60;

    char html[3072];
    snprintf(html, sizeof(html),
        "<!doctype html><html lang='de'><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta http-equiv='refresh' content='2'>"
        "<title>FC Wängi 1967 — Anzeigetafel</title>"
        "<style>"
        "body{font-family:-apple-system,Segoe UI,sans-serif;background:#0a0a0a;color:#fff;margin:0;padding:24px}"
        "header{background:#be1c1c;padding:16px;border-radius:8px;margin-bottom:24px}"
        "h1{margin:0;font-size:1.4rem;letter-spacing:.05em}"
        ".scores{display:flex;justify-content:space-around;align-items:center;margin:24px 0}"
        ".side{text-align:center;flex:1}"
        ".side .lbl{color:#888;font-size:.9rem;text-transform:uppercase;letter-spacing:.1em}"
        ".side .v{font-size:6rem;font-weight:900;color:#fdc500;line-height:1}"
        ".clock{text-align:center;font-size:3rem;font-weight:700;font-variant-numeric:tabular-nums;margin:24px 0}"
        ".state{text-align:center;color:#888;font-size:.95rem}"
        ".meta{margin-top:32px;padding-top:16px;border-top:1px solid #333;font-size:.8rem;color:#666;text-align:center}"
        "</style></head><body>"
        "<header><h1>FC Wängi 1967 — Anzeigetafel</h1></header>"
        "<div class='scores'>"
          "<div class='side'><div class='lbl'>Heim</div><div class='v'>%u</div></div>"
          "<div class='side' style='flex:0 0 auto;color:#888'>vs</div>"
          "<div class='side'><div class='lbl'>%s</div><div class='v'>%u</div></div>"
        "</div>"
        "<div class='clock'>%02u:%02u%s</div>"
        "<div class='state'>%s</div>"
        "<div class='meta'>WÄNGI · 1967 NEVER GIVE UP &nbsp;·&nbsp; v%s &nbsp;·&nbsp; %s</div>"
        "</body></html>",
        s.home_score_real,
        s.opponent[0] ? s.opponent : "Gegner",
        s.away_score_real,
        mm, ss, s.clock_running ? " ●" : "",
        match_label(s.match_state),
        FIRMWARE_VERSION,
        s.clock_running ? "läuft" : "pausiert");

    g_server.send(200, "text/html; charset=utf-8", html);
}

static void handle_json() {
    const auto& s = state::peek();
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"home\":%u,\"away\":%u,\"clock_seconds\":%u,\"running\":%s,"
        "\"state\":%u,\"opponent\":\"%s\",\"stoppage\":%u}",
        s.home_score_real, s.away_score_real, s.clock_seconds,
        s.clock_running ? "true" : "false",
        s.match_state,
        s.opponent[0] ? s.opponent : "",
        s.stoppage_minutes);
    g_server.send(200, "application/json", buf);
}

static void handle_not_found() {
    g_server.send(404, "text/plain", "Not found");
}

void begin() {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASSWORD, CHANNEL, /*hidden=*/0, /*max_conn=*/4);
    WiFi.setHostname(HOSTNAME);

    Serial.printf("[web] AP up SSID=%s IP=%s\n",
                  AP_SSID, WiFi.softAPIP().toString().c_str());

    g_server.on("/", handle_root);
    g_server.on("/score.json", handle_json);
    g_server.onNotFound(handle_not_found);
    g_server.begin();

    ArduinoOTA.setHostname(HOSTNAME);
    ArduinoOTA.setPassword(OTA_PW);
    ArduinoOTA.begin();
}

void loop() {
    g_server.handleClient();
    ArduinoOTA.handle();
}

} // namespace web

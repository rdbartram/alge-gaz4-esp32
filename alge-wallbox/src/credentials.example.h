// ============================================================================
//  Credentials for the wall-box's SoftAP + ArduinoOTA.
//
//  COPY THIS FILE to `credentials.h` (gitignored) and customise the values
//  before flashing. Do NOT commit your local credentials.h.
//
//      cp src/credentials.example.h src/credentials.h
//      # then edit src/credentials.h
// ============================================================================
#pragma once

#define WIFI_AP_SSID      "FC-Waengi-Wallbox"
#define WIFI_AP_PASSWORD  "CHANGE-ME-LONG-PASSPHRASE"
#define OTA_PASSWORD      "CHANGE-ME-OTA-SECRET"
#define OTA_HOSTNAME      "alge-wallbox"

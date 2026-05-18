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

// ----- ESP-NOW encryption keys ---------------------------------------------
// MUST match the controller's credentials.h exactly. See the comment in
// alge-controller/src/credentials.example.h.
#define ESPNOW_PMK { 0x46, 0x43, 0x57, 0x61, 0x65, 0x6E, 0x67, 0x69, \
                     0x31, 0x39, 0x36, 0x37, 0x50, 0x4D, 0x4B, 0x21 }
#define ESPNOW_LMK { 0x46, 0x43, 0x57, 0x61, 0x65, 0x6E, 0x67, 0x69, \
                     0x31, 0x39, 0x36, 0x37, 0x4C, 0x4D, 0x4B, 0x21 }

// ============================================================================
//  Credentials for the controller's SoftAP + ArduinoOTA.
//
//  COPY THIS FILE to `credentials.h` (which is gitignored) and customise
//  the values below before flashing. Do NOT commit your local credentials.h.
//
//      cp src/credentials.example.h src/credentials.h
//      # then edit src/credentials.h
// ============================================================================
#pragma once

// SoftAP that hosts the live-score web page (and is what OTA clients connect
// to for over-the-air updates).
#define WIFI_AP_SSID      "FC-Waengi-Tafel"
#define WIFI_AP_PASSWORD  "CHANGE-ME-LONG-PASSPHRASE"

// ArduinoOTA upload password. Required for `pio run -t upload --upload-port ...`.
#define OTA_PASSWORD      "CHANGE-ME-OTA-SECRET"

// mDNS hostname so `pio run -t upload --upload-port alge-controller.local` works.
#define OTA_HOSTNAME      "alge-controller"

// ----- ESP-NOW encryption keys ---------------------------------------------
// AES-128-CMAC keys used to encrypt unicast traffic between the controller
// and the wallbox. Both firmwares MUST share the same 16-byte values or
// paired peers won't be able to decrypt each other's frames. Broadcast
// (pairing handshake) traffic stays unencrypted by ESP-NOW protocol.
//
// To rotate: edit BOTH credentials.h files in lockstep + re-flash both
// devices in the same maintenance window.
#define ESPNOW_PMK { 0x46, 0x43, 0x57, 0x61, 0x65, 0x6E, 0x67, 0x69, \
                     0x31, 0x39, 0x36, 0x37, 0x50, 0x4D, 0x4B, 0x21 }
#define ESPNOW_LMK { 0x46, 0x43, 0x57, 0x61, 0x65, 0x6E, 0x67, 0x69, \
                     0x31, 0x39, 0x36, 0x37, 0x4C, 0x4D, 0x4B, 0x21 }

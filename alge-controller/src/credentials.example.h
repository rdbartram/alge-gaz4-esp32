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

// ============================================================================
//  alge-controller main
//
//  M5Stack Core2: handheld touchscreen controller for the FC Wängi 1967
//  Anzeigetafel. Sends ESP-NOW state to the wall-box, which renders frames
//  on the 34-year-old Alge GAZ4 flip-segment scoreboard.
// ============================================================================
#include <M5Unified.h>
#include "config.h"
#include "state.h"
#include "ui.h"
#include "espnow_client.h"
#include "web.h"

void setup() {
    // Serial FIRST so we see logs even if M5.begin() hangs. Wokwi's CoreS3
    // model doesn't simulate the AXP2101 PMIC; M5Unified probes it over I2C
    // and can stall — without early serial we'd see nothing.
    Serial.begin(115200);
    delay(200);
    Serial.printf("\n=== %s v%s booting ===\n", FIRMWARE_NAME, FIRMWARE_VERSION);
    Serial.println("[main] calling M5.begin()...");

    auto cfg = M5.config();
    cfg.clear_display = true;
#ifdef WOKWI_SIM
    cfg.output_power  = false;   // skip PMIC probing in sim
#else
    cfg.output_power  = true;
#endif
    cfg.internal_imu  = false;
    cfg.internal_mic  = false;
    cfg.internal_spk  = false;
    cfg.led_brightness = 0;
    M5.begin(cfg);
    Serial.println("[main] M5.begin() returned");

    M5.Display.setRotation(1);
    M5.Display.setBrightness(180);
    M5.Display.fillScreen(COLOR_BG_DARK);
    Serial.printf("[main] display %dx%d, board=%d\n",
                  M5.Display.width(), M5.Display.height(),
                  (int)M5.getBoard());

    state::begin();
    espnow_client::begin();
#ifndef WOKWI_SIM
    web::begin();   // Wokwi's WiFi softAP can block; skip in sim builds.
#endif
    ui::begin();
    Serial.println("[main] setup complete, entering loop");
}

void loop() {
    // v2: state lives on the wallbox, no local clock to advance.
    espnow_client::loop();
#ifndef WOKWI_SIM
    web::loop();
#endif
    ui::tick();

    delay(5);
}

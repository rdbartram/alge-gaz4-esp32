// ============================================================================
//  WiFi AP + ArduinoOTA for the wall-box.
//
//  The wall-box will eventually live in a sealed IP66 enclosure 100m from
//  the clubhouse. Reflashing it over USB-C would mean unmounting it from
//  the wall every time. Instead we host a WiFi AP "FC-Waengi-Wallbox" and
//  expose ArduinoOTA over it.
//
//  ESP-NOW and AP mode coexist by locking both to channel 1.
//  ESP-NOW radio activity is unaffected — the AP just adds beacon overhead.
// ============================================================================
#pragma once

namespace wb_ota {

void begin();
void loop();

} // namespace wb_ota

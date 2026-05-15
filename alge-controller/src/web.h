// ============================================================================
//  Controller SoftAP + live-score web page + ArduinoOTA.
//
//  At Grosswies there is no internet, so we host our own AP that anyone in
//  the clubhouse can join. The single page at "/" shows the live score
//  (auto-refreshes every 2s). ArduinoOTA listens on the same AP so the
//  controller can be reflashed without USB-C.
//
//  AP: "FC-Waengi-Tafel"  password "1967NeverGiveUp"
//  IP: 192.168.4.1
// ============================================================================
#pragma once

namespace web {

void begin();
void loop();

} // namespace web

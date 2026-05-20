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

#include <stdint.h>

namespace wb_ota {

void begin();
void loop();

// Live progress for the display while an HTTP upload is in flight.
// in_progress() flips true on UPLOAD_FILE_START and stays true through
// reboot. bytes_received / bytes_total drive the progress bar.
bool     in_progress();
uint32_t bytes_received();
uint32_t bytes_total();

// Bundled controller firmware accessors. The wall-box's HTTP server
// accepts a separate upload of the controller .bin at
//   POST http://192.168.4.1/controller-update
// and stashes it in SPIFFS. The accessors below let espnow_server tell
// paired controllers what's available via MSG_FIRMWARE_AVAIL.
bool     has_controller_firmware();
uint32_t controller_firmware_size();
const char* controller_firmware_md5();          // 32 hex chars + null

} // namespace wb_ota

// ============================================================================
//  Controller-side firmware-update receiver.
//
//  When the wall-box detects we're behind on firmware (controller's
//  heartbeat-reported CONTROLLER_FW_BUILD < wall-box's expected build),
//  it unicasts MSG_FIRMWARE_AVAIL with size + MD5 + fetch path. This
//  module stashes that offer, lets the UI prompt the operator, and on
//  confirm joins the wall-box SoftAP via WiFi STA, HTTP-pulls the
//  binary, and reboots into it via Update.h.
// ============================================================================
#pragma once

#include "shared/message.h"

namespace client_ota {

void begin();

// Called by espnow_client::on_recv when MSG_FIRMWARE_AVAIL arrives.
void note_offer(const FwAvailPayload& offer);

// UI inspection.
bool has_offer();
const FwAvailPayload& offer();
void dismiss_offer();

// Status of an in-flight update (so the UI can render a progress bar /
// "rebooting..." caption rather than letting the radio flap silently).
enum Phase : uint8_t {
    PHASE_IDLE,
    PHASE_JOIN_AP,
    PHASE_DOWNLOAD,
    PHASE_REBOOT,
    PHASE_ERROR,
};
Phase    phase();
uint32_t bytes_received();
uint32_t bytes_total();
const char* error_message();

// Operator confirmation — kicks off the download/flash. Blocks until
// either success (ESP.restart()) or failure.
void perform_update();

} // namespace client_ota

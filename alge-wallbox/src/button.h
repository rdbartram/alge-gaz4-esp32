// ============================================================================
//  IO14 user button handler.
//  Short press (<1s)      -> cycle info pages on display
//  Long press (2-4s)      -> emergency blank scoreboard
//  Very long press (>10s) -> factory reset + reboot
// ============================================================================
#pragma once

namespace wb_button {

enum PressKind { NONE, SHORT, LONG, VERY_LONG };

void begin();
PressKind poll();   // call from main loop

} // namespace wb_button

// ============================================================================
//  Wall-box display: 1.9" 170x320 ST7789 IPS in portrait orientation.
//  Renders different "screens" based on wb_state::WallboxMode.
// ============================================================================
#pragma once

namespace wb_display {

void begin();

// Call from main loop. Re-renders if state changed or every 250ms.
void tick();

// Force a redraw on the next tick.
void invalidate();

} // namespace wb_display

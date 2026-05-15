// ============================================================================
//  Polarity test mode.
//
//  GAZ4 uses RS232 with one TX line and one GND return. The wall socket
//  bananas are unmarked — if polarity is reversed, the board silently
//  ignores frames. We can't swap polarity in software with a 2-banana
//  setup (would need a relay or analog switch). Instead, we run a one-time
//  test on first boot: send "000 8 8    88 88\r" and ask the user to
//  confirm digits look right. If wrong, instruct rotating the wall-box.
// ============================================================================
#pragma once

namespace wb_polarity {

void begin();              // load saved polarity_ok flag from NVS
bool is_known_good();      // true if user has confirmed polarity in past
void mark_good();          // user confirmed; save to NVS
void mark_unknown();       // reset (factory reset)

// Runs the test loop: send test pattern at 1Hz, listen for confirmation
// via button press. Returns when user confirms or aborts.
void run_test();           // blocking-ish; returns after first confirm

} // namespace wb_polarity

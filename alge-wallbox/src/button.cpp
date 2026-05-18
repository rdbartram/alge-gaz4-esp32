// ============================================================================
//  IO14 user button handler.
//  Active-low (built-in pull-up) on LilyGO T-Display-S3.
// ============================================================================
#include "button.h"
#include "config.h"

#include <Arduino.h>

namespace wb_button {

static uint32_t g_press_start_ms = 0;
static bool     g_pressed = false;

void begin() {
    // The LilyGO T-Display-S3 carries two side buttons: BOOT on GPIO0
    // and KEY on GPIO14. We wire both as the "user button" so it
    // doesn't matter which one the operator presses (was previously
    // only listening to GPIO14, which silently swallowed every press
    // of the more-obvious BOOT button).
    pinMode(PIN_USER_BTN, INPUT_PULLUP);
    pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
}

static bool read_pressed() {
    return digitalRead(PIN_USER_BTN) == LOW ||
           digitalRead(PIN_BOOT_BTN) == LOW;
}

PressKind poll() {
    const bool now_pressed = read_pressed();
    const uint32_t now = millis();

    if (now_pressed && !g_pressed) {
        // Rising edge of press.
        g_pressed = true;
        g_press_start_ms = now;
        return NONE;
    }

    if (!now_pressed && g_pressed) {
        // Release edge — classify.
        g_pressed = false;
        const uint32_t held = now - g_press_start_ms;
        if (held >= BTN_VERYLONG_MIN_MS) return VERY_LONG;
        if (held >= BTN_LONG_MIN_MS && held <= BTN_LONG_MAX_MS) return LONG;
        if (held < BTN_SHORT_MAX_MS) return SHORT;
        return NONE;  // dead zone between SHORT and LONG = ignored
    }

    return NONE;
}

} // namespace wb_button

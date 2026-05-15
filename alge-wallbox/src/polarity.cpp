// ============================================================================
//  Polarity test storage.
// ============================================================================
#include "polarity.h"
#include "config.h"

#include <Preferences.h>

namespace wb_polarity {

static bool g_polarity_ok = false;

void begin() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    g_polarity_ok = prefs.getBool("polarity_ok", false);
    prefs.end();
}

bool is_known_good() { return g_polarity_ok; }

void mark_good() {
    g_polarity_ok = true;
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBool("polarity_ok", true);
    prefs.end();
}

void mark_unknown() {
    g_polarity_ok = false;
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.remove("polarity_ok");
    prefs.end();
}

void run_test() {
    // The actual TX loop + display + button handling happens in the main
    // loop driven by wb_state::WB_POLARITY_TEST mode. This function is
    // intentionally light; it just sets the mode.
    g_polarity_ok = false;
}

} // namespace wb_polarity

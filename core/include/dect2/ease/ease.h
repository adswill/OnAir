// First-use helpers that need no radio: when balloons fly, which geostationary satellite is highest, what a pick should leave alone.
// Pure functions, no I/O; the screens that use them are in app/ease and in each mode's UI file.
#pragma once
#include <cstdint>
#include <string>

namespace dect2::ease {

// ---- Radiosonde launch windows. Synoptic soundings are timed for 00 and 12 UTC, so most sites release near 23:15 and 11:15 UTC. A site's
// real time can differ (extra launches, other schedules), so this is only a rule of thumb, and the text says so.
struct SondeWindow {
    bool inWindow = false;     // from 30 minutes before the usual launch until the flight is normally over, 2 hours after it
    int secsToLaunch = 0;      // to the launch of this window (negative once it has passed) or of the next one
    int64_t id = 0;            // the number of the window: changes once per window
};
SondeWindow sondeWindow(int64_t unixUtc);
// Whether the optional automatic start should fire now: once per window, never over a running receiver. lastFired is the id of the last window
// that used its start (or the one in which the user turned the option on, so a stop by hand is respected).
bool sondeAutoStartDue(const SondeWindow& w, int64_t lastFired, bool optionOn, bool anythingRunning);

// ---- The highest of a list of geostationary slots from a position. -1 when none is at least minElDeg above the horizon.
struct GeoPick { int index = -1; double azDeg = 0, elDeg = 0; };
GeoPick highestGeo(double latDeg, double lonDeg, const double* slotLonDeg, int count, double minElDeg = 5.0);
const char* compass8(double azDeg);    // "north", "north-east", ... (English: pass through tr())

// What a preselection may do on this visit.
enum class Preselect {
    Wait,           // nothing yet: no position, or the receiver is running
    Pick,           // choose and tune
    LeaveAlone,     // the user already tuned elsewhere: remember that and never ask again
    Done            // decided on an earlier visit
};
Preselect preselectAction(bool prefExists, bool positionKnown, bool running, double freqMhz, double defMhz);

} // namespace dect2::ease

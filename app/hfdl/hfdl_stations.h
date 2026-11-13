// HFDL: the ground stations near you and the frequencies they use now (4.0, docs/HFDL-STATIONS-4.0.md). From the squitters this receiver hears
// and, when turned on (Settings > Privacy, "hfdlLiveOn"; never in Offline mode), the community list of hfdl.observer, fetched in the background
// at most every few minutes and kept in the data folder. The merge and sort are in core (dect2/hfdl/hfdl_live.h).
#pragma once
#include "dect2/hfdl/hfdl_live.h"
#include <string>
#include <vector>

struct App;

namespace hfdlst {

// Once a frame from the HFDL tick: starts a fetch when one is due. wanted: the Nearby view is shown or an automatic band choice runs.
void tick(App& a, bool wanted);
bool liveAllowed();                                        // the switch is on and Offline mode is off
// The position the distances are measured from: the one typed in the Nearby view, else the station location; false when neither is set.
bool position(double& lat, double& lon, std::string* from = nullptr);
std::vector<dect2::HfdlNearby> nearby(const App& a);       // merged, nearest first
std::vector<int> hintKhz(const App& a);                    // the frequencies worth trying from here, nearest station first
void tuneKhz(App& a, int khz);                             // one-click tune: the band that holds it, the span placed to cover it
void panel(App& a);                                        // the Nearby view
void tunerLine(App& a);                                    // the nearest active frequency with a tune button (Tuner panel)

}  // namespace hfdlst

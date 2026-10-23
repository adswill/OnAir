// Shared by the ADS-B screens (adsb_*.cpp): the screen state, the colours, the unit helpers and the functions more than
// one file uses. Everything is in namespace adsbui; the mode entry is kAdsbUi (declared in modeui.cpp).
// Included by adsb_ui.cpp, adsb_table_ui.cpp and adsb_map_ui.cpp only.
#pragma once
#include "app.h"
#include "dect2/adsb/adsb_units.h"
#include "state/results_export.h"
#include "adsb_map.h"
#include "map/rangeplot_ui.h"
#include "dect2/adsb/adsb_msg.h"
#include "dect2/adsb/adsb_reg.h"
#include "dect2/adsb/adsb_jam.h"
#include "dect2/adsb/adsb_display.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <vector>

extern const ModeUi kAdsbUi;   // below, in adsb_ui.cpp
extern const ModeUi kModeAcUi;  // modeac_ui.cpp: it draws the ADS-B map (full screen too)

namespace adsbui {

struct State {
    bool loaded = false, wasRunning = false;
    double refLat = 25.25, refLon = 55.36;      // where the antenna is: ranges and local decoding; the default is only a placeholder
    double refAltM = 0;                         // its height above sea level (metres): the pass planner's look angles
    double pushedLat = 1e9, pushedLon = 1e9;
    uint32_t sel = 0xFFFFFFFFu;                 // the selected aircraft (ICAO address)
    int correction = 1;
    std::deque<float> rate, snr;                // good messages per second and SNR, a point per report
    uint64_t lastSeq = 0;
    int viewMode = 0;                           // 0 map, 1 table, 2 radar
    adsbmap::View map;
    bool mapHome = true;                        // the map follows the antenna position until the user drags it
    dect2::adsbu::Units units;                  // display units (prefs adsbUnits, adsbSpeedUnit)
    dect2::GpsJamGrid jam;                      // GPS interference per 0.5 degree cell, from the accuracy the aircraft report (app clock, last hour)
    bool jamLayer = false;                      // draw it on the map (prefs adsbGpsJam)
    dect2::adsbd::Options disp;                 // Display options: altitude colours, track width, drop time (prefs adsbAltColour, adsbTrackWidth, adsbDropSec)
    int pushedDrop = -1;                        // the drop time last given to the receiver
    bool winds = false;                         // "winds aloft": wind arrows at the aircraft (from Comm-B)
    bool full = false;                          // the map or radar fills the whole window (Esc or the same button to leave)
    double ctxLat = 0, ctxLon = 0;              // where the map was right-clicked
    std::string locMsg;
    int locBusy = 0;
    RangeView range;                            // the receiver-range plot (Range view)
    // other receivers (Beast / SBS feeds in), the registration database
    std::string srcText;                        // one host:port per line
    bool srcAny = false;                        // also hosts outside the local network
    std::string pushedSrc; bool pushedAny = false, srcPushed = false;
    dect2::AdsbRegDb db;
    std::string dbMsg;
};

inline const ImVec4 kDim(0.62f, 0.65f, 0.68f, 1), kGood(0.40f, 0.85f, 0.50f, 1), kWarn(0.95f, 0.60f, 0.25f, 1);

namespace au = dect2::adsbu;


// adsb_ui.cpp
extern State S;
bool live(const App& a);
std::string dbPath();
void reloadDb();
std::string countryCode(uint32_t icao);
std::string registrationOf(uint32_t icao);
std::string operatorOf(uint32_t icao);
std::string typeOf(const AdsbAircraft& ac);
std::string sourceName(const dect2::AdsbTelemetry& t, int id);
std::string icaoText(uint32_t i);
void setReference(App& a, double lat, double lon);
void positionFields(App& a);

// adsb_table_ui.cpp
std::string uAlt(double ft);
std::string uSpd(double kt);
std::string uDist(double nm, int dec = 1);
void unitsControls();
void displayControls();   // the "Display" popup: altitude colours, track width, drop time
std::string typeText(const AdsbAircraft& ac);
void aircraftTable(App& a, bool compact);
void detail(const App& a);
void statsView(App& a);
void sourcesPanel(App& a);

// adsb_map_ui.cpp
double bearingDeg(double lat1, double lon1, double lat2, double lon2);
void radar(App& a, ImVec2 size);
bool positionLooksWrong(const AdsbTelemetry& t, double& lat, double& lon, double& nearestNm);
void mapView(App& a, ImVec2 size);

}  // namespace adsbui

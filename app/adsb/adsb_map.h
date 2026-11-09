// A slippy map for the ADS-B screen: web-mercator tiles from a tile server, cached on disk, drawn with ImGui. Needs the network for new tiles
// (curl, as the updater uses) and a picture decoder (macOS only for now); without them it shows a plain graticule.
#pragma once
#include "imgui.h"
#include <cmath>
#include <cstdlib>
#include <functional>

namespace adsbmap {

constexpr int kMinZoom = 2, kMaxZoom = 19;   // continent to single buildings (the deepest the OpenStreetMap tile server draws)

struct View {
    double lat = 25.25, lon = 55.36;   // the centre of the map
    int zoom = 7;                      // kMinZoom to kMaxZoom
};

// Draw the map into the next `size` of the current window and handle dragging (pan), the wheel (zoom around the pointer). Returns true when
// the pointer is over the map. The projection below works after this call.
bool draw(View& v, ImVec2 size);
ImVec2 project(double lat, double lon);        // screen position
void unproject(ImVec2 p, double& lat, double& lon);   // the other way: where on Earth a screen position is
bool clickedAt(ImVec2& p);                     // the map was clicked (not dragged); where
unsigned seaColour();                          // 0xRRGGBB of the map background now
unsigned overlayText();                        // 0xRRGGBB text colour readable on it (labels over the map)
unsigned panelColour();                        // 0xRRGGBB backing for panels drawn over the map
unsigned legible(unsigned rgb);                // a marker/line colour pushed to hold contrast on the map background
bool tilesAvailable();                         // at least one tile is on screen
void legend(float mapWidth, const char* fmt, ...);   // TextDisabled() along the bottom left of the map (the cursor put there): cut short before the credit
// The controls every map has, on the line of the screen's own buttons: a "tiles" button with its menu (whether downloaded tiles are kept or
// deleted when OnAir closes, how many there are per zoom level, deleting them) and the "online map" switch. Both are one setting for every
// map. `privacy` (or null) ends "only tile numbers are sent" in the switch's tooltip, e.g. "never the position itself". After draw(), with
// SameLine() before it; when they do not fit before the map's right edge they go on a line of their own.
void tileControls(const char* privacy, bool fullButton = true);
// Full screen, one for every map: the button ("full screen" / "exit full screen", Esc leaves) is part of tileControls(), so every map has it on
// its control line. It makes the screen the map is on fill the whole window. mainTabs() wraps each tab body in a FullScope (the body is what
// is drawn again over the whole window); screens with their own full-screen view pass fullButton=false and use fullButton() themselves.
bool fullButton(bool on, const char* what);    // the shared button + tooltip; true when clicked. `what` is "map" or "radar" for the tooltip
class FullScope {
public:
    FullScope(const void* owner, std::function<void()> body);   // owner: the App whose window it is
    ~FullScope();
    FullScope(const FullScope&) = delete;
    FullScope& operator=(const FullScope&) = delete;
};
inline int devView(int dflt) { const char* e = std::getenv("ONAIR_VIEW"); return e ? std::atoi(e) : dflt; }   // dev: open a screen on view N (screenshots)
bool fullActive();                             // the full-screen screen is being drawn now
void fullCorner(ImVec2 p0, ImVec2 size);       // a full-screen button in the top right corner of a map-sized rectangle, for the maps without the tile controls (cursor is left where it was)
bool fullDraw(const void* owner, int family, ImVec2 disp);   // draw_ui.cpp, each frame: the full-screen screen over the whole window; false when not full
bool online();                                 // tiles are fetched from the tile server (the "online map" switch)
void shutdown();                               // stop the downloader; delete this session's tiles unless they are kept

// Aircraft icon fill: white ADS-B, blue Inmarsat, red ACARS, yellow HFDL; grey when the last update is older than 30 s (60 s for satellite).
enum class AcSrc { Adsb, Sat, Acars, Hfdl, Flarm, Adsl };
inline double acStaleSec(AcSrc s) { return s == AcSrc::Sat ? 60.0 : 30.0; }
inline ImU32 acLive(AcSrc s) {
    switch (s) {
        case AcSrc::Sat: return IM_COL32(80, 150, 220, 255);
        case AcSrc::Acars: return IM_COL32(220, 72, 72, 255);
        case AcSrc::Hfdl: return IM_COL32(230, 198, 64, 255);
        case AcSrc::Flarm: return IM_COL32(108, 204, 122, 255);
        case AcSrc::Adsl: return IM_COL32(120, 196, 236, 255);
        default: return IM_COL32(236, 238, 240, 255);
    }
}
inline ImU32 acStale() { return IM_COL32(150, 154, 158, 255); }
inline ImU32 acColour(AcSrc s, double ageSec, float alpha = 1.f) {
    const ImU32 c = ageSec > acStaleSec(s) ? acStale() : acLive(s);
    return (c & 0x00FFFFFF) | ((ImU32)std::lround(255.f * alpha) << 24);
}
// point `nm` nautical miles along `headingDeg` (0 = north): the short "where it is going" stroke
inline void aheadNm(double lat, double lon, double headingDeg, double nm, double& ola, double& olo) {
    const double rad = 0.017453292519943295, r = nm / 3440.065, h = headingDeg * rad, p1 = lat * rad;
    ola = std::asin(std::sin(p1) * std::cos(r) + std::cos(p1) * std::sin(r) * std::cos(h)) / rad;
    olo = lon + std::atan2(std::sin(h) * std::sin(r) * std::cos(p1), std::cos(r) - std::sin(p1) * std::sin(ola * rad)) / rad;
}

} // namespace adsbmap

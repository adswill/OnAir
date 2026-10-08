// A slippy map for the ADS-B screen: web-mercator tiles from a tile server, cached on disk, drawn with ImGui. Needs the network for new tiles
// (curl, as the updater uses) and a picture decoder (macOS only for now); without them it shows a plain graticule.
#pragma once
#include "imgui.h"

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
bool clickedAt(ImVec2& p);                     // the map was clicked (not dragged); where
bool tilesAvailable();                         // at least one tile is on screen
void legend(float mapWidth, const char* fmt, ...);   // TextDisabled() along the bottom left of the map (the cursor put there): cut short before the credit
// The controls every map has, on the line of the screen's own buttons: a "tiles" button with its menu (whether downloaded tiles are kept or
// deleted when OnAir closes, how many there are per zoom level, deleting them) and the "online map" switch. Both are one setting for every
// map. `privacy` (or null) ends "only tile numbers are sent" in the switch's tooltip, e.g. "never the position itself". After draw(), with
// SameLine() before it; when they do not fit before the map's right edge they go on a line of their own.
void tileControls(const char* privacy);
bool online();                                 // tiles are fetched from the tile server (the "online map" switch)
void shutdown();                               // stop the downloader; delete this session's tiles unless they are kept

} // namespace adsbmap

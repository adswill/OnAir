// A slippy map for the ADS-B screen: web-mercator tiles from a tile server, cached on disk, drawn with ImGui. Needs the network for new tiles
// (curl, as the updater uses) and a picture decoder (macOS only for now); without them it shows a plain graticule.
#pragma once
#include "imgui.h"

namespace adsbmap {

struct View {
    double lat = 25.25, lon = 55.36;   // the centre of the map
    int zoom = 7;                      // 2 (continent) to 12 (city)
    bool online = true;                // fetch tiles from the tile server
};

// Draw the map into the next `size` of the current window and handle dragging (pan), the wheel (zoom around the pointer). Returns true when
// the pointer is over the map. The projection below works after this call.
bool draw(View& v, ImVec2 size);
ImVec2 project(double lat, double lon);        // screen position
bool clickedAt(ImVec2& p);                     // the map was clicked (not dragged); where
bool tilesAvailable();                         // at least one tile is on screen
void legend(float mapWidth, const char* fmt, ...);   // TextDisabled() along the bottom left of the map (the cursor put there): cut short before the credit
void shutdown();                               // stop the downloader

} // namespace adsbmap

// Weather-satellite pictures on the live map (View > Map): the Meteor-M LRPT picture of the pass is placed on the ground from the
// orbit as it grows, a GOES full disk from the satellite longitude. Receivers feed it (meteor_ui.cpp, goes_ui.cpp), the map draws it.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
struct ImDrawList;
struct App;

namespace wxoverlay {

// The map is open and the overlay is on and a new picture is due: the feeders build their pixels only then (cheap when it is not).
bool wanted(const App& a, double minGapSec = 4.0);
// The picture of a Meteor-M pass so far: `lines` scanned lines of `width` pixels in `px` (alpha 0: nothing received), 6 lines a second.
// The first call for a new `imageId` fixes the start time (now minus the lines so far).
void setSwath(uint64_t imageId, const std::string& label, int width, int lines, const std::vector<uint32_t>& px, double linesPerSec = 6.0);
// A GOES full disk (square-ish, the disk filling the width); version changes when the picture grew
void setGeos(uint64_t productId, uint64_t version, const std::string& label, int w, int h, const std::vector<uint32_t>& px);

void draw(ImDrawList* dl);   // under the markers, over the tiles
void controls(App& a, float x, float y, float width);   // the small panel at that screen position (nothing when there is no picture)
bool hasPicture();

}

// The common frame of a mode panel (docs/MODE-EXPERIENCE.md, 1.1 and 1.3): the tab strip Live | <Items> | Map | More and the one-line status header.
#pragma once
#include <cstdint>
#include <initializer_list>

namespace kit {

// One tab. `view` is the mode's own number for that view (so the mode's code keeps its numbering); `more` puts it under the More menu.
struct NavItem {
    const char* label;
    int view;
    bool more = false;
};

// Draws the strip: the non-More items as pills in the order given, then a "More" pill that opens a menu with the rest. cur is the mode's view
// number. The selected More item shows in the pill ("More: Table"). Slot order and meaning never change (Live first, Items second, Map third).
int modeNav(const char* id, int& cur, std::initializer_list<NavItem> items);

// The status header: one line, fields dropped from the end as the width shrinks (1 lock lamp, 2 mode name, 3 Record state, 4 last heard,
// 5 counts (active now, today), 6 SNR, 7 frequency). lock: 0 receiver off, 1 locked, 2 searching. snrDb <= -900 and freqHz == 0 leave their field out.
// The counts and the last-heard time come from the session store (the mode's number).
struct HeaderInfo {
    uint16_t mode = 0;
    const char* name = "";
    int lock = 0;
    float snrDb = -1000.f;
    double freqHz = 0;
};
void modeHeader(const HeaderInfo& h);

}  // namespace kit

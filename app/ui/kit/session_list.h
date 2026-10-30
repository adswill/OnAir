// The shared Items tab of a mode (docs/MODE-EXPERIENCE.md, 2.4): header (Logging state, count, drops), search and filters, the list of sessions
// (Calls, Threads or Tracks by type), a detail pane (fields, notes, tags, Keep, linked items) and one Export button. The list is read from the
// session store through a CachedList: it asks again only when the mode's generation counter changed, never on the UI thread.
#pragma once
#include <cstdint>

namespace kit {

// kind: 1 Call, 2 Thread, 3 Track (sess::Kind). id: unique per mode tab. Draws into the available region.
void sessionItems(uint16_t mode, int kind, const char* id);
// the tab label for a kind: "Calls", "Threads", "Tracks"
const char* sessionTabName(int kind);

}  // namespace kit

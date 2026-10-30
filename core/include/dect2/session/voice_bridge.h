// The bridge between voice::Monitor and the SessionStore (docs/MODE-EXPERIENCE.md, 2.2a). For 3.4 the Monitor is not replaced: it keeps its
// history and its policy lists, and its call open / update / close hooks hand the call to the session store as a Session (a queue push, nothing
// more: no SQLite, no file and no recmgr work ever happens under the Monitor's mutex). The store's key is (mode, receiver): with two receivers
// on one mode (a wide capture, a second SDR, KAAL) each Monitor has its own receiver index (Monitor::setSessionReceiver).
#pragma once
#include "dect2/session/session.h"
#include "dect2/voice/voice_monitor.h"
#include <string>

namespace dect2::sess::voicebridge {

// The mode number (docs/MODE-NUMBERS.md family) of a monitor name ("dmr" 9, "nxdn" 38, "p25" 50, "dpmr" 45, "dstar" 33, "ysf" 128, "tetra" 46, "m17" 31);
// -1 for a monitor that has no session mode (tests, unknown names).
int modeNumber(const std::string& monitorMode);
// What the user and the events call it: "DMR", "NXDN", "P25", "dPMR", "D-STAR", "Yaesu Fusion", "TETRA", "M17"
const char* modeName(const std::string& monitorMode);

// The Session of a call as the monitor knows it now. Ids and aliases are RAW; the store protects them. The title is "DMR TG 91 Worldwide" while the
// call runs and "DMR TG 91 Worldwide, 12 s" once it has ended.
Session toSession(const voice::CallEntry& e, const std::string& monitorMode, uint16_t receiver, bool ended);

}  // namespace dect2::sess::voicebridge

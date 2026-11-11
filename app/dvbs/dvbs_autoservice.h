// DVB-S/S2 (and QO-100): start the decoder of a signal that carries a single service by itself, and follow the signal when the
// receiver is moved to another one (#322). On QO-100 most carriers hold one service, so jumping between them should not need a
// Stop and a Start on the service each time. A signal with several services stays manual.
//
// Pure logic, no UI: the caller passes the services of the stream, the selected one and the TS lock, and gets the service to select.
#pragma once
#include "dect2/tv/ts.h"
#include <vector>

namespace dect2 {

struct AutoServiceState {
    bool armed = true;          // may start a service by itself (off after the user stopped it, until the signal changes)
    int offered = -1;           // the service id we last started, or the one that was playing when the user stopped it
    int started = -1;           // set while the service we started is the one playing
    double lostSince = -1;      // time the TS lock was lost (-1: locked)
    bool changed = false;       // the signal changed under a selected service (lock gone for a while, or its service list cleared by a retune)
};

constexpr double kAutoServiceRelock = 1.5;   // s without a TS lock: the receiver was moved, the next signal is a new one

// Returns the service id to select now, or -1 for nothing to do. *restart is set when the returned id is the one already selected but
// the signal under it changed: a retune to another carrier whose only service has the same id (on QO-100 most use 1). The player
// session of the old carrier must then be restarted (select(-1), then the id); left alone, a GPU decoder fed the new stream mid-GOP
// stays silent and nothing plays (#410).
inline int autoServicePick(AutoServiceState& st, const std::vector<TsService>& services, int selected, bool tsLock, double now,
                           bool* restart = nullptr) {
    if (restart) *restart = false;
    if (!tsLock) {
        if (st.lostSince < 0) st.lostSince = now;
        if (now - st.lostSince >= kAutoServiceRelock) { st.armed = true; st.offered = -1; st.started = -1; if (selected >= 0) st.changed = true; }
        return -1;
    }
    st.lostSince = -1;
    // the user stopped the service we had started: leave it stopped until this signal is gone or a different service shows up
    if (st.started >= 0 && selected < 0) { st.armed = false; st.started = -1; }
    int sole = -1, n = 0;
    for (const TsService& sv : services) if (sv.havePmt && !sv.streams.empty()) { sole = sv.id; n++; }
    if (n == 0 && selected >= 0) st.changed = true;         // the services went (a retune cleared them) under the playing one
    if (n >= 2) st.changed = false;
    if (n != 1) return -1;                                  // none yet, or several: the user picks
    if (sole != st.offered && selected < 0) st.armed = true; // a different service than the one stopped by hand
    if (selected == sole) {
        st.offered = sole; st.started = sole;
        if (st.changed && restart) { st.changed = false; *restart = true; return sole; }   // same id, new signal: restart it
        st.changed = false;
        return -1;
    }
    if (selected < 0 && !st.armed) return -1;
    st.offered = sole; st.started = sole; st.changed = false;
    return sole;                                            // nothing playing, or a stale service of the previous signal
}

}  // namespace dect2

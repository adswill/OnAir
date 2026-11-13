// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The radio retune off the UI thread (owner: "tuning is super unsmooth compared to SDR++"): the frequency box, the digit wheel and a drag
// past the band post the new settings and return at once; one thread per engine makes the driver call with the NEWEST request only, every
// request overtaken meanwhile is dropped (dect2::LatestSlot). The interface shows the new frequency at once; the picture follows when the
// radio has moved. Engine::start / stop / retune share one lock (engine_tune.cpp), so a stop from the UI never meets a retune half done.
#pragma once
#include "dect2/engine/engine.h"
#include <cstdint>

namespace tunework {

void post(dect2::Engine& e, const dect2::TuneSettings& t, bool reset);   // reset: retuneReset (the decoders forget the old station)
bool busy(const dect2::Engine& e);                                       // a retune is pending or running
void flush(dect2::Engine& e, int maxMs = 500);                          // wait until e's retunes are done
void forget(dect2::Engine& e);                                          // before the engine goes away: its thread ends
void shutdown();                                                        // at exit: every thread ends

struct Stats {
    uint64_t posted = 0, done = 0, coalesced = 0;
    double postMsMax = 0;            // the longest the UI thread spent in post() (the mailbox, no driver call)
    double callMsMean = 0, callMsMax = 0;   // the driver calls on the tuning thread
};
Stats stats();

} // namespace tunework

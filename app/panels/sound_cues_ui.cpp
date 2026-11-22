// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Optional sound cues (docs/SOUND-CUES.md): every frame the readings go to the tracker (core/include/dect2/audio/sound_cues.h), which says
// when a tone is due. Settings > Notifications turns each cue on; all are off by default. The lock reading is the receiver's "data valid"
// flag (the TV, DAB and FM receivers); the scanner's hit counter and the pass planner's running pass give the other two.
#include "../app.h"
#include "automation_internal.h"
#include "dect2/audio/sound_cues.h"

namespace {
dect2::cues::Tracker& tracker() { static dect2::cues::Tracker t; return t; }
}

void soundCuesTick(App& a) {
    namespace cu = dect2::cues;
    cu::Settings& s = tracker().settings();
    plat::Prefs& p = plat::prefs();
    s.lock = p.getB("cueLock", false); s.lost = p.getB("cueLost", false); s.found = p.getB("cueFound", false); s.passEnd = p.getB("cuePass", false);
    s.volume = (int)std::min(100L, std::max(5L, p.getI("cueVol", 50)));
    if (!s.anyOn()) { tracker().reset(); return; }   // nothing wanted: no readings, and the next time starts from a fresh baseline
    cu::Input in;
    in.locked = a.engine.running() && a.rx.dataValid;
    in.foundCount = scannerLive().hitSeq;
    in.passActive = passesRunActive();
    for (cu::Cue c : tracker().update(in, ImGui::GetTime())) automationapp::playCue(a, cu::tune(c, 48000), (float)s.volume / 100.f);
}

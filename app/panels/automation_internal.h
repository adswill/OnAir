// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Automations, the application side: what the actions do inside OnAir (automation_sink.cpp) and the window (automation_ui.cpp).
#pragma once
#include "dect2/automation/engine.h"
#include <string>
#include <vector>

struct App;

namespace automationapp {

// The ActionSink of the program. Its calls come from the engine thread; what touches the receiver runs on the interface thread (uiTick) and
// the engine thread waits for it up to 3 seconds.
dect2::automation::ActionSink& sink();
// every frame, on the interface thread, for the first receiver: runs the queued actions and ends timed recordings
void uiTick(App& a);
void autoRecordInstall();   // the record rules' IQ clips and non-voice audio (session/record_rules.h)
// the banner in the window's top edge that notify shows (and a short log of webhook results)
void drawBanner();
// a watch-list or keyword alert: the banner, a desktop notification and (when asked, and not muted) three short beeps. Interface thread.
void alertNow(App& a, const std::string& title, const std::string& body, bool sound);
// A sound cue (core/include/dect2/audio/sound_cues.h): stereo float tone at 48 kHz, scaled by gain (0..1). Silent while muted or in quiet hours
// (Settings > Notifications). Interface thread.
void playCue(App& a, const std::vector<float>& tone, float gain);
std::string webhookStatus();

}  // namespace automationapp

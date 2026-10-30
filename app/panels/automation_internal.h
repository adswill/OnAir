// Automations, the application side: what the actions do inside OnAir (automation_sink.cpp) and the window (automation_ui.cpp).
#pragma once
#include "dect2/automation/engine.h"
#include <string>

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
std::string webhookStatus();

}  // namespace automationapp

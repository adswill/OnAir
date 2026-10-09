// HF digital, the RTTY view of the main tab (called by hfdig_ui.cpp): skeleton. The state lamp and a note that the decoder is not
// built yet; the RTTY worker replaces it.
#include "app.h"
#include "dect2/hfdig_tel.h"

void hfdigRttyTab(App& a, const dect2::HfdigTelemetry& t) {
    lamp("RTTY", t.rtty.state > 0 ? 1 : 0);
    ImGui::SameLine(0, 12 * gUi);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", a.engine.running() ? "decoder not built yet" : "start the receiver");
    ImGui::PopTextWrapPos();
}

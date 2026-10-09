// HF digital, the RTTY view of the main tab (called by hfdig_ui.cpp): the settings, the tones found and the decoded text.
#include "app.h"
#include "dect2/hfdig_rtty.h"
#include "dect2/hfdig_tel.h"
#include <algorithm>
#include <cstdio>
#include <string>

namespace {

struct State {
    bool stick = true;   // the text follows the end unless the user scrolled up
    float lastMax = 0;
};
State S;

}

void hfdigRttyTab(App& a, const dect2::HfdigTelemetry& t) {
    using namespace dect2;
    const HfdigRttyTelemetry& r = t.rtty;
    const bool run = a.engine.running();
    HfdigRtty& dec = a.engine.hfdig().rtty();

    // settings
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Baud"); ImGui::SameLine(0, 5 * gUi);
    char lbl[32];
    snprintf(lbl, sizeof lbl, "%g", kRttyBaudTable[dec.baudIndex()]);
    ImGui::SetNextItemWidth(78 * gUi);
    if (ImGui::BeginCombo("##rtbaud", lbl)) {
        for (int i = 0; i < kRttyBauds; i++) {
            snprintf(lbl, sizeof lbl, "%g", kRttyBaudTable[i]);
            if (ImGui::Selectable(lbl, i == dec.baudIndex())) dec.setBaudIndex(i);
        }
        ImGui::EndCombo();
    }
    flowNext(12 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Shift"); ImGui::SameLine(0, 5 * gUi);
    snprintf(lbl, sizeof lbl, "%g Hz", kRttyShiftTable[dec.shiftIndex()]);
    ImGui::SetNextItemWidth(92 * gUi);
    if (ImGui::BeginCombo("##rtshift", lbl)) {
        for (int i = 0; i < kRttyShifts; i++) {
            snprintf(lbl, sizeof lbl, "%g Hz", kRttyShiftTable[i]);
            if (ImGui::Selectable(lbl, i == dec.shiftIndex())) dec.setShiftIndex(i);
        }
        ImGui::EndCombo();
    }
    flowNext(12 * gUi);
    bool rev = dec.reverse();
    if (ImGui::Checkbox("Reverse", &rev)) dec.setReverse(rev);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The mark tone is the upper one (normal: the lower one).");
    flowNext(12 * gUi);
    bool uns = dec.unshiftOnSpace();
    if (ImGui::Checkbox("Unshift on space", &uns)) dec.setUnshiftOnSpace(uns);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A space switches the figures back to letters.");
    flowNext(12 * gUi);
    if (ImGui::Button("Clear")) dec.clearText();
    flowNext(6 * gUi);
    if (ImGui::Button("Copy")) ImGui::SetClipboardText(r.text.c_str());
    flowEnd();

    // status line
    const bool on = run && r.state > 0;
    lamp("RTTY", !run ? 0 : (r.state == 2 ? 1 : 2)); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(a.mono, 0);
    if (on) ImGui::Text("mark %.0f Hz  space %.0f Hz", r.markHz, r.spaceHz);
    else ImGui::TextDisabled("%s", run ? "searching for a signal" : "start the receiver");
    ImGui::PopFont();
    flowNext(12 * gUi);
    ImGui::TextDisabled("Quality"); ImGui::SameLine(0, 5 * gUi);
    const float q = on ? std::clamp(r.quality, 0.f, 1.f) : 0.f;
    char qt[16];
    snprintf(qt, sizeof qt, "%d%%", (int)(q * 100 + 0.5f));
    gaugePill(70 * gUi, q, ImGui::ColorConvertFloat4ToU32(q > 0.6f ? pal::okGreen() : (q > 0.3f ? pal::warnAmber() : pal::badRed())), qt);
    flowNext(12 * gUi);
    ImGui::TextDisabled("Chars"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::Text("%llu", (unsigned long long)r.chars); ImGui::PopFont();
    flowNext(12 * gUi);
    ImGui::TextDisabled("Errors"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::Text("%llu", (unsigned long long)r.framingErrors); ImGui::PopFont();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Characters dropped because the start or the stop bit was wrong.");
    flowEnd();

    // the text, to the end of the tab
    ImGui::BeginChild("##rttytext", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::PushFont(a.mono, 0);
    ImGui::PushTextWrapPos(0);
    if (r.text.empty()) ImGui::TextDisabled("%s", run ? "no text yet" : "start the receiver");
    else ImGui::TextUnformatted(r.text.c_str(), r.text.c_str() + r.text.size());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    // follow the end; scrolling up stops it, scrolling back to the end starts it again (sy is where the user left it, lastMax the end we scrolled to)
    const float sy = ImGui::GetScrollY(), sm = ImGui::GetScrollMaxY();
    if (S.stick && sy < S.lastMax - 4) S.stick = false;
    else if (!S.stick && sy >= sm - 4) S.stick = true;
    S.lastMax = sm;
    if (S.stick) ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
}

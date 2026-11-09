// HF digital, the RTTY view of the main tab (called by hfdig_ui.cpp): the settings, the tones found and the decoded text.
#include "app.h"
#include "dect2/hfdig/hfdig_rtty.h"
#include "dect2/hfdig/hfdig_tel.h"
#include "dect2/engine/updater.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

struct State {
    bool stick = true;   // the text follows the end unless the user scrolled up
    float lastMax = 0;
};
State S;


// RTTY Operators' official calling frequencies (rttyops.com "RTTY QRG"); lower sideband below 10 MHz, upper above (RTTY convention)
struct RttyBand { const char* name; double mhz; const char* note; };
const RttyBand kRttyBands[] = {{"80m", 3.580, ""}, {"80m DX", 3.590, "DX calling"}, {"40m", 7.040, ""}, {"30m", 10.142, ""},
                               {"20m", 14.080, ""}, {"20m DX", 14.083, "DX calling"}, {"17m", 18.105, ""}, {"15m", 21.080, ""},
                               {"12m", 24.925, ""}, {"10m", 28.080, ""}};

void rttyBandButtons(App& a);

}

void hfdigSetDemod(const App& a, int d);   // hfdig_ui.cpp

namespace {
void rttyBandButtons(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Band"); flowNext(6 * gUi);
    for (int i = 0; i < IM_ARRAYSIZE(kRttyBands); i++) {
        const RttyBand& b = kRttyBands[i];
        const int demod = b.mhz < 10.0 ? dect2::HfDemodLsb : dect2::HfDemodUsb;
        const bool here = std::fabs(a.freqMhz - b.mhz) < 0.002;
        if (here) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.45f));
        ImGui::PushID(i);
        if (ImGui::SmallButton(b.name)) { hfdigSetDemod(a, demod); tuneFreq(a, b.mhz); }
        ImGui::PopID();
        if (here) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("RTTY on %s: %.3f MHz, %s%s%s", b.name, b.mhz, dect2::hfDemodName(demod), b.note[0] ? "; " : "", b.note);
        flowNext(4 * gUi);
    }
    if (ImGui::SmallButton("RTTY QRG (rttyops.com)")) dect2::openUrl("https://rttyops.com/index.php/rtty-qrg/2-about-us");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The RTTY Operators' list of calling frequencies (opens the browser)");
    flowEnd();
}
}

void hfdigRttyTab(App& a, const dect2::HfdigTelemetry& t) {
    using namespace dect2;
    const HfdigRttyTelemetry& r = t.rtty;
    const bool run = a.engine.running();
    HfdigRtty& dec = a.engine.hfdig().rtty();

    rttyBandButtons(a);
    // Auto (the default): speed, shift and polarity follow the signal. The hand-set values are under Advanced; touching one turns Auto off.
    bool autoOn = dec.autoOn();
    if (ImGui::Checkbox(tr("Auto"), &autoOn)) dec.setAuto(autoOn);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Finds the speed, shift and polarity from the signal and follows it when it changes. Needs about 6 s of signal."));
    flowNext(6 * gUi);
    ImGui::AlignTextToFramePadding();
    if (!autoOn) ImGui::TextDisabled("%s", tr("manual settings (Advanced)"));
    else if (r.autoLocked && !r.autoHyps.empty()) {
        const auto& h = r.autoHyps[0];
        ImGui::TextDisabled(tr("Found: %g baud, %g Hz shift, %s"), h.baud, h.shiftHz, h.reverse ? tr("reversed") : tr("normal"));
    }
    else if (!run) ImGui::TextDisabled("%s", tr("Auto: starts with the receiver"));
    else if (r.autoState == 3) ImGui::TextDisabled("%s", tr("Auto: no RTTY found yet"));
    else ImGui::TextDisabled("%s", tr("Auto: listening for RTTY..."));
    flowEnd();
    // the hand-set values
    char lbl[32];
    if (ImGui::TreeNode(trw("Advanced##rtty"))) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", tr("Baud")); ImGui::SameLine(0, 5 * gUi);
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
        ImGui::TextDisabled("%s", tr("Shift")); ImGui::SameLine(0, 5 * gUi);
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
        if (ImGui::Checkbox(tr("Reverse"), &rev)) dec.setReverse(rev);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The mark tone is the upper one (normal: the lower one)."));
        flowEnd();
        ImGui::TextDisabled("%s", tr("Changing one of these turns Auto off."));
        ImGui::TreePop();
    }
    bool uns = dec.unshiftOnSpace();
    if (ImGui::Checkbox("Unshift on space", &uns)) dec.setUnshiftOnSpace(uns);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A space switches the figures back to letters.");
    flowNext(12 * gUi);
    if (ImGui::Button("Clear")) a.engine.hfdig().clearResults(1);   // the text the receiver keeps (also in the results file)
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

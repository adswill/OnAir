// Helpers shared by several screens (see ui_common.h): clock text, key / value rows, the status-line IQ lamp and State label,
// the map's zoom buttons and the wrapped-text tooltip.
// Only screens that had the same lines written out use them; more can move here later.
#include "dect2/common/time_fmt.h"
#include "ui_common.h"
#include "theme.h"
#include "voice_why_ui.h"
#include <cstdarg>
#include <ctime>

std::string clockText(int64_t t) {
    if (t <= 0) return "-";
    return dect2::timefmt::clock(t);
}

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

void voiceWhyRow(const App&, dect2::VoiceWhy w) {
    const char* t = "";
    switch (w) {
        case dect2::VoiceWhy::NoInput: t = N_("no signal at the channel: check the tuning, the antenna and the gain"); break;
        case dect2::VoiceWhy::NoSync: t = N_("signal, but no frame sync found: check the tuning (offset, polarity), the bandwidth and the signal-to-noise ratio"); break;
        case dect2::VoiceWhy::IdUnreadable: t = N_("frame syncs found, but their identity (NID, LICH, slot type) is unreadable: the signal is too noisy or distorted"); break;
        case dect2::VoiceWhy::FecFails: t = N_("frame syncs found, but the payload fails its FEC / CRC: weak signal, fading or a sample clock error"); break;
        case dect2::VoiceWhy::Marginal: t = N_("decoding, but over a third of the blocks are lost: a stronger signal or a better antenna would help"); break;
        case dect2::VoiceWhy::Decoding: t = N_("decoding well"); break;
    }
    const ImVec4 col = w == dect2::VoiceWhy::Decoding ? pal::okGreen() : (w == dect2::VoiceWhy::Marginal ? pal::warnAmber() : pal::badRed());
    ImGui::PushTextWrapPos(0); ImGui::TextColored(col, "%s: %s", tr("Why"), tr(t)); ImGui::PopTextWrapPos();
}

void statusIqLamp(const App& a) {
    const bool run = a.engine.running();
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
}

void statusStateLabel(const App& a, bool live, const std::string& summaryText) {
    const bool run = a.engine.running();
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!live ? (run ? "starting" : "stopped") : summaryText.c_str()); ImGui::PopFont();
}

void mapCornerButtons(adsbmap::View& map, bool& follow, const char* tileNote) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(26 * gUi, 0))) { map.zoom = std::min(adsbmap::kMaxZoom, map.zoom + 1); follow = false; } if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom in"));
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(26 * gUi, 0))) { map.zoom = std::max(adsbmap::kMinZoom, map.zoom - 1); follow = false; } if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom out"));
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("fit")) follow = true;
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls(tileNote);
}

void wrapTooltip(float width, const char* text) {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(width);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

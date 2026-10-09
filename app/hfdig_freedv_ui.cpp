// HF digital, the FreeDV view of the main tab (called by hfdig_ui.cpp): the codec2 library lamp, one sync lamp per FreeDV mode, the mode
// choice, the SNR, the text channel (callsign) and the speech frames. Without the codec2 library one sentence says so and how to get it.
#include "app.h"
#include "dect2/hfdig_tel.h"
#include <algorithm>
#include <cstdio>

namespace {

const char* const kChoices[] = {"Automatic", "700D", "700E", "1600"};

const char* installHint() {
#if defined(__APPLE__)
    return "On macOS, install it with Homebrew: brew install codec2";
#elif defined(_WIN32)
    return "On Windows, codec2 will be shipped with OnAir in a later version.";
#else
    return "On Linux, install your distribution's codec2 package (libcodec2).";
#endif
}

} // namespace

void hfdigFreedvTab(App& a, const dect2::HfdigTelemetry& t) {
    const dect2::HfdigFreedvTelemetry& f = t.freedv;
    const bool run = a.engine.running();
    const bool found = run ? f.libFound : dect2::hfdigFreedvLibrary();
    lamp("Library", found ? 1 : (run ? 3 : 0));
    ImGui::SameLine(0, 12 * gUi);
    for (int m = 0; m < dect2::kFreedvModes; m++) {
        char b[24];
        snprintf(b, sizeof b, "Sync %s", dect2::freedvModeName(m));
        lamp(b, run && found && f.sync[m] ? 1 : 0);
        if (m + 1 < dect2::kFreedvModes) ImGui::SameLine(0, 12 * gUi);
    }
    if (!found) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("codec2 library not found: FreeDV needs it to decode.");
        ImGui::TextDisabled("%s", installHint());
        ImGui::PopTextWrapPos();
    }

    int choice = dect2::hfdigFreedvMode();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Mode");
    ImGui::SameLine(0, 6 * gUi);
    ImGui::SetNextItemWidth(130 * gUi);
    if (ImGui::BeginCombo("##fdvmode", kChoices[std::min(std::max(choice, 0), 3)])) {
        for (int i = 0; i < 4; i++) if (ImGui::Selectable(kChoices[i], i == choice)) dect2::hfdigFreedvSetMode(i);
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Automatic listens for FreeDV 700D, 700E and 1600 at the same time and decodes the one that has sync.");
    if (!found) return;

    const bool live = run && f.mode >= 0;
    char b[96];
    ImGui::SameLine(0, 15 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Receiving");
    ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0);
    ImGui::TextUnformatted(live ? dect2::freedvModeName(f.mode) : (run ? "searching" : "-"));
    ImGui::PopFont();

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("SNR");
    ImGui::SameLine(0, 5 * gUi);
    {
        const float fr = live ? std::min(1.f, std::max(0.f, (f.snr + 5.f) / 25.f)) : 0.f;
        const ImU32 col = fr < 0.25f ? IM_COL32(176, 66, 58, 255) : fr < 0.45f ? IM_COL32(176, 130, 48, 255) : pal::remap(IM_COL32(40, 112, 150, 255));
        snprintf(b, sizeof b, live ? "%.1f dB" : "-", f.snr);
        gaugePill(130, fr, col, b);
        if (ImGui::IsItemHovered() && live) ImGui::SetTooltip("The codec2 library's SNR estimate in 3 kHz. FreeDV 700D/E work down to about 0 dB.");
    }
    ImGui::SameLine(0, 15 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Speech frames");
    ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0);
    ImGui::Text("%llu", (unsigned long long)f.speechFrames);
    ImGui::PopFont();

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Text");
    ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0);
    ImGui::PushTextWrapPos(0);
    if (f.text.empty()) ImGui::TextDisabled("%s", live ? "no text received yet" : "-");
    else ImGui::TextUnformatted(f.text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
}

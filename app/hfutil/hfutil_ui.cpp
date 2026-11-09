// HF utility mode panels (families 430 .. 439): band presets, decoded text, events. Automatic by default; speed and shift under Advanced for the FSK modes,
// the STANAG 4285 profile for that modem (it signals no rate itself).
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "gfx/gfx.h"
#include "i18n/i18n.h"
#include "dect2/hfutil/hfutil_rx.h"
#include "dect2/hfutil/hfutil_tel.h"
#include <cmath>
#include <cstdio>
#include <string>

namespace {

struct State { float baud = 0, shift = 0; int prof = -1; };
State S[dect2::kHfuKinds];

template <int K> bool live(const App& a) { return a.rx.standard == dect2::kHfuFamilyFirst + K + 1; }

template <int K> void presets(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Band"));
    flowNext(6 * gUi);
    int n = 0;
    const dect2::HfuPreset* p = dect2::hfuPresets(K, n);
    for (int i = 0; i < n; i++) {
        const bool here = std::fabs(a.freqMhz - p[i].mhz) < 0.002;
        if (here) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.45f));
        char b[40]; snprintf(b, sizeof b, "%s##hfu%d_%d", p[i].name, K, i);
        if (ImGui::Button(b)) tuneFreq(a, p[i].mhz);
        if (here) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.4f MHz", p[i].mhz);
        flowNext(4 * gUi);
    }
    ImGui::TextDisabled("|"); flowNext(8 * gUi);
    char cl[40]; snprintf(cl, sizeof cl, "%s##hfu%d", tr("Clear"), K);
    if (ImGui::Button(cl)) a.engine.hfUtil(K).clearResults();
    flowEnd();
    if (ImGui::CollapsingHeader(trw("Advanced"))) {
        if (K == 1) {
            static const char* prof[] = {N_("Automatic (needs a start-of-message pattern)"), N_("2400 bit/s coded, short"), N_("1200 bit/s coded, short"), N_("600 bit/s coded, short"), N_("300 bit/s coded, short"), N_("150 bit/s coded, short"), N_("75 bit/s coded, short"), N_("3600 bit/s uncoded"), N_("2400 bit/s uncoded"), N_("1200 bit/s uncoded")};
            int v = S[K].prof + 1;
            if (ImGui::BeginCombo(tr("Profile"), tr(prof[std::max(0, v)]))) {
                for (int i = 0; i < 10; i++) if (ImGui::Selectable(tr(prof[i]), i == v)) { S[K].prof = i - 1; a.engine.hfUtil(K).setForcedProfile(i - 1); }
                ImGui::EndCombo();
            }
        } else if (K >= 2) {
            bool ch = ImGui::InputFloat(tr("Baud (0 = automatic)"), &S[K].baud, 0, 0, "%.1f");
            ch |= ImGui::InputFloat(tr("Shift Hz (0 = automatic)"), &S[K].shift, 0, 0, "%.0f");
            if (ch) a.engine.hfUtil(K).setForcedFsk(S[K].baud, S[K].shift);
        }
    }
}

template <int K> void tab(App& a) {
    presets<K>(a);
    ImGui::Separator();
    if (!live<K>(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::HfUtilTelemetry& t = a.rx.hfutil;
    ImGui::TextUnformatted(dect2::hfUtilSummary(t).c_str());
    if (K <= 1) ImGui::TextDisabled("%s", tr("Encrypted traffic stays as bytes; nothing is decrypted."));
    ImGui::Separator();
    if (ImGui::BeginChild("##hfutext", ImVec2(0, 0), false)) {
        ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(t.text.c_str());
        ImGui::PopTextWrapPos(); ImGui::PopFont();
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

template <int K> void list(App& a) {
    if (!live<K>(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const auto& ev = a.rx.hfutil.events;
    for (const auto& e : ev) {
        ImGui::PushFont(a.mono, 0); ImGui::TextDisabled("%s", dect2::timefmt::clock(e.utc).c_str()); ImGui::PopFont();
        ImGui::SameLine(); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(e.text.c_str()); ImGui::PopTextWrapPos();
    }
    if (ev.empty()) ImGui::TextDisabled("%s", tr("nothing heard yet"));
}

template <int K> void receiver(App& a) {
    if (!live<K>(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::HfUtilTelemetry& t = a.rx.hfutil;
    auto kv = [&](const char* k, const char* fmt, auto... x) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::Text(fmt, x...); ImGui::PopFont(); };
    kv(tr("state"), "%s", dect2::hfUtilSummary(t).c_str());
    if (K <= 1) { kv(tr("frequency error"), "%+.1f Hz", t.cfoHz); kv(tr("SNR"), "%.1f dB", t.snrDb); kv(tr("STANAG 5066 D_PDUs"), "%llu", (unsigned long long)t.pdus); }
    else { kv(tr("speed"), "%.1f Bd", t.baud); kv(tr("tone centre"), "%.0f Hz", t.centreHz); kv(tr("shift"), "%.0f Hz", t.shiftHz); }
    kv(tr("good / bad"), "%llu / %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kv(tr("level"), "%.1f dBFS", t.levelDb);
}

template <int K> void status(App& a) {
    const bool run = a.engine.running();
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!live<K>(a) ? (run ? tr("starting") : tr("stopped")) : dect2::hfUtilSummary(a.rx.hfutil).c_str()); ImGui::PopFont();
}

template <int K> void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = dect2::hfuName(K);
    if (live<K>(a)) l2 = dect2::hfUtilSummary(a.rx.hfutil);
}

}  // namespace

extern const ModeUi kHfuUi[dect2::kHfuKinds];
#define HFU_UI(K, Name) {.sideTitle = "EVENTS", .tabName = Name, .tabIcon = Ic::Radio, .tab = tab<K>, .receiver = receiver<K>, .list = list<K>, .status = status<K>, .summary = summary<K>}
const ModeUi kHfuUi[dect2::kHfuKinds] = {HFU_UI(0, "MIL-STD-188-110"), HFU_UI(1, "STANAG 4285"), HFU_UI(2, "ARQ-E"), HFU_UI(3, "ARQ-N"), HFU_UI(4, "AUTOSPEC"), HFU_UI(5, "SPREAD"), HFU_UI(6, "G-TOR"), HFU_UI(7, "EFR"), HFU_UI(8, "Piccolo"), HFU_UI(9, "Coquelet")};

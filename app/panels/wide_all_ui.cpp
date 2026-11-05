// "Everything in view" window (View > Everything in view; the one button of the Simple views): ONE wide capture, every signal in it found, identified and
// decoded at once (core/include/dect2/engine/wide_session.h). The list shows each signal with what it is, the decoder's state and its CPU share; a signal
// can be switched off, pinned (never paused), listened to, given a mode by hand, removed, and signals can be added from the spectrum (the Identify
// tool's dragged region, or the tuned signal). The CPU limit pauses the lowest ranked signals when the computer cannot keep up and says so here.
#include "../app.h"
#include "dect2/engine/wide_session.h"
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace dect2;

namespace {

// the kinds a signal can be given by hand (not the TV kinds: those come from the TV scan)
const WsKind kPick[] = {WsKind::Dmr, WsKind::P25, WsKind::Nxdn, WsKind::Dpmr, WsKind::Dstar, WsKind::Fusion, WsKind::M17, WsKind::Pager, WsKind::Packet,
                        WsKind::Am, WsKind::Nfm, WsKind::Tetra, WsKind::Inmarsat};
const int kPickN = (int)(sizeof kPick / sizeof kPick[0]);

const char* stateText(WsState s) {
    switch (s) {
    case WsState::Identifying: return tr("identifying");
    case WsState::Running: return tr("running");
    case WsState::Off: return tr("off");
    case WsState::Dropped: return tr("paused (CPU)");
    case WsState::NoDecoder: return tr("no decoder");
    case WsState::Unidentified: return tr("unknown");
    }
    return "";
}
ImVec4 stateColour(WsState s, bool locked) {
    switch (s) {
    case WsState::Running: return locked ? ImVec4(.45f, 1.f, .45f, 1.f) : ImVec4(.95f, .85f, .35f, 1.f);
    case WsState::Dropped: return ImVec4(1.f, .6f, .25f, 1.f);
    case WsState::Identifying: return ImVec4(.55f, .75f, 1.f, 1.f);
    default: return ImVec4(.6f, .6f, .6f, 1.f);
    }
}

bool byHand(const WsEntry& e) { return e.identity.rfind("set by hand", 0) == 0; }

struct State {
    int addKind = 0;        // 0: identify automatically, else kPick[addKind - 1]
    bool wasOn = false;
};
State S;

void addFromRegion(App& a, double lo, double hi) {
    const double centre = radioCenterMhz(a);
    const double off = ((lo + hi) * 0.5 - centre) * 1e6, bw = std::max(2500.0, (hi - lo) * 1e6);
    a.engine.wideAllSession().addManual(off, bw, S.addKind ? kPick[S.addKind - 1] : WsKind::Unknown);
}

void cpuLine(const WsStatus& st, bool on) {
    if (!on) return;
    const double lim = st.budget > 0 ? st.budget : 1.0;
    const float frac = (float)std::min(1.5, st.load / lim);
    char ov[96];
    if (st.budget > 0) snprintf(ov, sizeof ov, "%.0f%% / %.0f%%", st.load * 100, st.budget * 100); else snprintf(ov, sizeof ov, "%.0f%%", st.load * 100);
    ImVec4 col = frac < 0.8f ? ImVec4(.4f, .85f, .4f, 1.f) : frac < 1.f ? ImVec4(.95f, .8f, .3f, 1.f) : ImVec4(1.f, .45f, .3f, 1.f);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
    ImGui::ProgressBar(std::min(1.f, frac), ImVec2(160 * gUi, 0), ov);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The decoders' share of one processor core, against the limit. Above the limit the lowest ranked signals are paused."));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", tr("CPU"));
}

}  // namespace

bool wideAllWorthIt(const App& a) { return a.engine.wideAll() || a.engine.sampleRate() >= 1.0e6; }

// The one button: starts and stops decoding everything in the capture. Used by the Simple views and the window.
void wideAllButton(App& a) {
    const bool on = a.engine.wideAll();
    ImGui::PushID("##wideall_one");
    ImVec4 bg = on ? pal::accent(0.55f) : ImGui::GetStyleColorVec4(ImGuiCol_Button);
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    const bool hit = ImGui::Button(on ? tr("Everything in view: on") : tr("Decode everything in view"));
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Find every signal in the whole capture (digital voice, TETRA, pagers, packet radio, air band, Inmarsat, TV) and decode all of them at once."));
    if (hit) {
        a.engine.setWideAll(!on);
        if (!on) a.wideAllOpen = true;
    }
    if (on) { ImGui::SameLine(); if (ImGui::SmallButton(tr("List"))) a.wideAllOpen = true; }
    ImGui::PopID();
}

void wideAllWindow(App& a) {
    if (!a.wideAllOpen) return;
    firstWindowSize(900 * gUi, 520 * gUi);
    if (!ImGui::Begin(trw("Everything in view"), &a.wideAllOpen)) { ImGui::End(); return; }
    WideSession& ws = a.engine.wideAllSession();
    const bool on = a.engine.wideAll();
    ImGui::TextWrapped("%s", tr("One capture, every signal in it. OnAir finds the signals, says what each one is and starts the right decoder for it. Switch one off, pin the ones that must never be paused, or give an unknown signal its mode by hand."));
    ImGui::Spacing();
    wideAllButton(a);
    if (on && !a.engine.running()) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("Start the receiver to begin.")); }
    const WsStatus st = ws.status();
    ImGui::SameLine(0, 16 * gUi);
    cpuLine(st, on);
    {   // the limit
        ImGui::SameLine(0, 16 * gUi);
        float lim = (float)(a.wideAllCpu * 100);
        ImGui::SetNextItemWidth(120 * gUi);
        if (ImGui::SliderFloat(tr("CPU limit %##wideall_cpu"), &lim, 0, 100, lim <= 0 ? tr("off") : "%.0f")) { a.wideAllCpu = lim / 100.0; a.engine.setWideAllBudget(a.wideAllCpu); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("When the decoders take longer than this share of one core, the lowest ranked signal is paused (never a pinned one or the one you listen to) and comes back when there is room. 0: no limit."));
    }
    if (on && st.droppedForCpu > 0) {
        ImGui::TextColored(ImVec4(1.f, .6f, .25f, 1.f), tr("This computer cannot keep up: %d signals are paused to save CPU. Pin the ones you need, or raise the limit."), st.droppedForCpu);
    }
    if (on && st.tvPresent) ImGui::TextDisabled(tr("TV channels in the capture: %.0f MHz wide"), st.tvWidthMhz);

    // adding signals from the spectrum
    ImGui::Separator();
    {
        double lo = 0, hi = 0;
        const bool have = identifyRegionMhz(lo, hi);
        const char* names[kPickN + 1];
        names[0] = tr("Identify automatically");
        for (int i = 0; i < kPickN; i++) names[i + 1] = wsKindName(kPick[i]);
        ImGui::SetNextItemWidth(170 * gUi);
        ImGui::Combo(tr("Add as##wideall_addkind"), &S.addKind, names, kPickN + 1);
        ImGui::SameLine();
        ImGui::BeginDisabled(!have || !on);
        if (ImGui::Button(tr("Add selected region##wideall_addsel"))) addFromRegion(a, lo, hi);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Turn the Identify tool on (key I) and drag across a signal on the spectrum, then add it here."));
        ImGui::SameLine();
        ImGui::BeginDisabled(!on || !a.engine.running());
        if (ImGui::Button(tr("Add tuned signal##wideall_addtun"))) addFromRegion(a, a.freqMhz - 0.00625, a.freqMhz + 0.00625);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Add the signal at the tuner marker (12.5 kHz wide)."));
    }
    ImGui::Separator();

    const std::vector<WsEntry> es = ws.entries();
    if (!on) ImGui::TextDisabled("%s", tr("Off. Press the button to decode everything in the capture."));
    else if (es.empty()) ImGui::TextDisabled("%s", tr("Looking for signals..."));
    else if (ImGui::BeginTable("##wideall_tbl", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("On"), ImGuiTableColumnFlags_WidthFixed, 34 * gUi);
        ImGui::TableSetupColumn(tr("MHz"), ImGuiTableColumnFlags_None, 1.1f);
        ImGui::TableSetupColumn(tr("Mode"), ImGuiTableColumnFlags_None, 1.5f);
        ImGui::TableSetupColumn(tr("State"), ImGuiTableColumnFlags_None, 1.2f);
        ImGui::TableSetupColumn(tr("What"), ImGuiTableColumnFlags_None, 2.6f);
        ImGui::TableSetupColumn(tr("dB"), ImGuiTableColumnFlags_WidthFixed, 44 * gUi);
        ImGui::TableSetupColumn(tr("CPU"), ImGuiTableColumnFlags_WidthFixed, 46 * gUi);
        ImGui::TableSetupColumn(tr("Pin"), ImGuiTableColumnFlags_WidthFixed, 36 * gUi);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 74 * gUi);
        ImGui::TableHeadersRow();
        for (const WsEntry& e : es) {
            ImGui::PushID(e.id);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool en = e.enabled;
            if (ImGui::Checkbox("##on", &en)) ws.setEnabled(e.id, en);
            ImGui::TableNextColumn();
            ImGui::Text("%.4f", e.absHz > 0 ? e.absHz / 1e6 : radioCenterMhz(a) + e.offsetHz / 1e6);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("%+.1f kHz from the capture centre, %.1f kHz wide"), e.offsetHz / 1e3, e.bandwidthHz / 1e3);
            ImGui::TableNextColumn();
            if (wsIsTv(e.kind)) ImGui::TextUnformatted(wsKindName(e.kind));
            else {   // the mode picker: automatic (what the identifier said) or one chosen by hand
                int cur = 0;
                if (byHand(e)) for (int i = 0; i < kPickN; i++) if (kPick[i] == e.kind) cur = i + 1;
                const char* label = e.kind == WsKind::Unknown || e.kind == WsKind::NoDecoder ? tr("unknown") : wsKindName(e.kind);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo("##mode", label)) {
                    if (ImGui::Selectable(tr("Automatic"), cur == 0)) ws.setKind(e.id, WsKind::Unknown);
                    for (int i = 0; i < kPickN; i++) if (ImGui::Selectable(wsKindName(kPick[i]), cur == i + 1)) ws.setKind(e.id, kPick[i]);
                    ImGui::EndCombo();
                }
            }
            ImGui::TableNextColumn();
            ImGui::TextColored(stateColour(e.state, e.locked), "%s%s", stateText(e.state), e.locked && e.state == WsState::Running ? " *" : "");
            if (!e.why.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.why.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.info.empty() ? e.identity.c_str() : e.info.c_str());
            if (ImGui::IsItemHovered() && !e.identity.empty()) ImGui::SetTooltip("%s", e.identity.c_str());
            ImGui::TableNextColumn();
            if (e.snrDb > 0) ImGui::Text("%.0f", e.snrDb); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            if (e.state == WsState::Running) ImGui::Text("%.0f%%", e.cost * 100); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            bool pin = e.pinned;
            if (ImGui::Checkbox("##pin", &pin)) ws.setPinned(e.id, pin);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Pinned: never paused by the CPU limit"));
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(tr("Listen##l"))) { ws.setListen(e.id); a.wideAllListen = e.id; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Follow this signal's sound; it is never paused by the CPU limit"));
            ImGui::SameLine(0, 3 * gUi);
            if (ImGui::SmallButton("x##rm")) ws.remove(e.id);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Remove: a signal you added is taken away, a found one is switched off"));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

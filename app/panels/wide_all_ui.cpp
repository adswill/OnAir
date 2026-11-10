// "Everything in view" window (View > Everything in view; the one button of the Simple views): ONE wide capture, every signal in it found, identified and
// decoded at once (core/include/dect2/engine/wide_session.h). The list shows each signal with what it is, the decoder's state and its CPU share; a signal
// can be switched off, pinned (never paused), listened to, given a mode by hand, removed, and signals can be added from the spectrum (the Identify
// tool's dragged region, or the tuned signal). The CPU limit pauses the lowest ranked signals when the computer cannot keep up and says so here.
#include "../app.h"
#include "dect2/engine/rate_choice.h"
#include "dect2/engine/wide_placed.h"
#include "dect2/engine/wide_session.h"
#include "dect2/station/station.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

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

// Several modes at once (issue #384): the signals placed at fixed frequencies, one list per receiver, kept in the settings
struct PlacedState {
    bool loaded = false, only = true;
    std::vector<Placed> list;
    PlacedSync sync;
    std::string note;
};
std::map<int, PlacedState> gPlaced;

PlacedState& placedOf(App& a) {
    PlacedState& p = gPlaced[a.rxId];
    if (!p.loaded) {
        p.loaded = true;
        const std::string key = "widePlaced" + std::to_string(a.rxId), okey = "widePlacedOnly" + std::to_string(a.rxId);
        p.list = placedFromText(plat::prefs().getS(key.c_str(), ""));
        p.only = plat::prefs().getB(okey.c_str(), true);
    }
    return p;
}

void savePlaced(App& a, const PlacedState& p) {
    const std::string key = "widePlaced" + std::to_string(a.rxId), okey = "widePlacedOnly" + std::to_string(a.rxId);
    plat::prefs().setS(key.c_str(), placedToText(p.list));
    plat::prefs().setB(okey.c_str(), p.only);
}

// every frame, also with the window closed: the placed signals follow the capture
void placedTick(App& a) {
    PlacedState& p = placedOf(a);
    WideSession& ws = a.engine.wideAllSession();
    if (!a.engine.wideAll() || !a.engine.running() || p.list.empty()) {
        if (!p.sync.ids().empty()) p.sync.clear(ws);
        ws.setOnlyManual(false);
        return;
    }
    ws.setOnlyManual(p.only);
    const std::vector<WsEntry> es = ws.entries();
    for (const auto& kv : p.sync.ids()) {   // the session was reset (a restart, a new mode): its entries went with it
        bool there = false;
        for (const WsEntry& e : es) if (e.id == kv.second) there = true;
        if (!there) { p.sync.forget(); break; }
    }
    p.sync.apply(ws, p.list, radioCenterMhz(a) * 1e6, a.engine.sampleRate());
}

void fitPlacedInCapture(App& a, PlacedState& p) {
    std::vector<dect2::station::CaptureChannel> ch;
    for (const Placed& q : p.list) ch.push_back({q.absHz, q.widthHz});
    double maxRate = 0;
    if (a.devIdx >= 0 && a.devIdx < (int)a.devices.size()) maxRate = rateLimitsOf(a.devices[(size_t)a.devIdx]).maxHz;
    const dect2::station::CapturePlan plan = dect2::station::planCapture(ch, maxRate, 2e6);
    if (plan.rateHz <= 0) { p.note = tr("None of them fits this radio's sample rate."); return; }
    a.freqMhz += plan.centerHz / 1e6 - radioCenterMhz(a);
    chooseSampleRate(a, plan.rateHz);
    if (a.engine.running()) { a.tune.centerHz = settingsRetuneCentreHz(a); a.engine.retune(a.tune); }
    char b[120];
    if (plan.fits) snprintf(b, sizeof b, tr("All of them are now in the capture (%.1f Msps)."), plan.rateHz / 1e6);
    else snprintf(b, sizeof b, tr("%d do not fit in one capture on this radio and stay off until you tune to them."), (int)plan.outside.size());
    p.note = b;
}

void placedPanel(App& a) {
    PlacedState& p = placedOf(a);
    if (!ImGui::CollapsingHeader(trw("Several modes at once"), p.list.empty() ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::TextWrapped("%s", tr("Place signals at fixed frequencies, each with its own mode: they are decoded together from this capture, and stop by themselves when you tune away."));
    const char* names[kPickN + 1];
    names[0] = tr("Identify automatically");
    for (int i = 0; i < kPickN; i++) names[i + 1] = wsKindName(kPick[i]);
    int removeAt = -1;
    bool changed = false;
    const double centre = radioCenterMhz(a) * 1e6, rate = a.engine.sampleRate();
    for (int i = 0; i < (int)p.list.size(); i++) {
        Placed& q = p.list[(size_t)i];
        ImGui::PushID(i);
        double mhz = q.absHz / 1e6;
        ImGui::SetNextItemWidth(130 * gUi);
        if (ImGui::InputDouble("##mhz", &mhz, 0, 0, "%.4f", ImGuiInputTextFlags_EnterReturnsTrue) && mhz > 0) { q.absHz = mhz * 1e6; changed = true; }
        ImGui::SameLine();
        int cur = 0;
        for (int k = 0; k < kPickN; k++) if (kPick[k] == q.kind) cur = k + 1;
        ImGui::SetNextItemWidth(170 * gUi);
        if (ImGui::Combo("##kind", &cur, names, kPickN + 1)) { q.kind = cur ? kPick[cur - 1] : WsKind::Unknown; changed = true; }
        ImGui::SameLine();
        if (!placedInCapture(q, centre, rate)) { ImGui::TextDisabled("%s", tr("outside the capture")); ImGui::SameLine(); }
        if (ImGui::SmallButton(tr("Remove"))) removeAt = i;
        ImGui::PopID();
    }
    if (removeAt >= 0) { p.list.erase(p.list.begin() + removeAt); changed = true; }
    if (ImGui::Button(tr("Place at the tuned frequency"))) {
        Placed q; q.absHz = a.freqMhz * 1e6; q.widthHz = 12500; q.kind = S.addKind ? kPick[S.addKind - 1] : WsKind::Unknown;
        p.list.push_back(q);
        changed = true;
    }
    if (p.list.size() >= 2) {
        ImGui::SameLine();
        if (ImGui::Button(tr("Fit them all in one capture"))) fitPlacedInCapture(a, p);
    }
    if (ImGui::Checkbox(tr("Decode only these"), &p.only)) changed = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("On: the other signals in the capture stay off. Off: everything found is decoded as well."));
    if (!p.note.empty()) ImGui::TextDisabled("%s", p.note.c_str());
    if (changed) savePlaced(a, p);
}

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
// Middle-click on the spectrum or the waterfall (a press that did not move; a middle drag stays a pan): a popup at the cursor adds a decoder at that frequency,
// or removes the placed one that is there. Drawn by wideAllWindow every frame, so it works with the list window closed.
struct MidClick {
    int plot = -1;               // plot of a press under way
    ImVec2 p0, pos;
    bool open = false;           // the popup is to open next frame
    double absHz = 0, tolHz = 0, widthHz = 12500;
    int kind = 0;                // 0: automatic, else kPick[kind - 1]
};
MidClick M;

void midClickPopup(App& a) {
    if (M.open) { ImGui::SetNextWindowPos(M.pos); ImGui::OpenPopup("##wideMid"); M.open = false; }
    if (!ImGui::BeginPopup("##wideMid")) return;
    PlacedState& p = placedOf(a);
    const int near = placedNearest(p.list, M.absHz, M.tolHz);
    ImGui::Text("%.4f MHz", M.absHz / 1e6);
    ImGui::Separator();
    const char* names[kPickN + 1];
    names[0] = tr("Automatic");
    for (int i = 0; i < kPickN; i++) names[i + 1] = wsKindName(kPick[i]);
    ImGui::SetNextItemWidth(170 * gUi);
    ImGui::Combo(tr("Mode##wideMid"), &M.kind, names, kPickN + 1);
    float kHz = (float)(M.widthHz / 1e3);
    ImGui::SetNextItemWidth(170 * gUi);
    if (ImGui::InputFloat(tr("Width kHz##wideMid"), &kHz, 0, 0, "%.1f") && kHz > 0) M.widthHz = kHz * 1e3;
    if (ImGui::Button(tr("Add decoder here"))) {
        p.list.push_back(placedAtClick(M.absHz, M.widthHz, M.kind ? kPick[M.kind - 1] : WsKind::Unknown));
        savePlaced(a, p);
        if (!a.engine.wideAll()) a.engine.setWideAll(true);   // the sync follows the list from the next frame
        ImGui::CloseCurrentPopup();
    }
    if (near >= 0) {
        ImGui::Separator();
        const Placed& q = p.list[(size_t)near];
        char b[96];
        snprintf(b, sizeof b, "%s %.4f MHz (%s)##wideMidRm", tr("Remove"), q.absHz / 1e6, q.kind == WsKind::Unknown ? tr("Automatic") : wsKindName(q.kind));
        if (ImGui::Button(b)) { p.list.erase(p.list.begin() + near); savePlaced(a, p); ImGui::CloseCurrentPopup(); }
    }
    ImGui::EndPopup();
}

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
    placedTick(a);
    midClickPopup(a);
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

    placedPanel(a);
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

// Called inside the spectrum and waterfall plots: a marker for each placed signal (a thin line with its mode; dimmed when it is outside the capture),
// and the middle-click that opens the popup.
void widePlacedPlot(App& a, int plotId) {
    PlacedState& p = placedOf(a);
    const plt::Rect lim = plt::GetPlotLimits();
    const double centre = radioCenterMhz(a) * 1e6, rate = a.engine.sampleRate();
    for (size_t i = 0; i < p.list.size(); i++) {
        const Placed& q = p.list[i];
        const double x = q.absHz / 1e6;
        if (x < lim.X.Min || x > lim.X.Max) continue;
        const bool in = placedInCapture(q, centre, rate);
        const double xs[1] = {x};
        plt::Spec ls; ls.LineColor = ImVec4(0.35f, 0.8f, 1.f, in ? 0.9f : 0.35f); ls.LineWeight = 1.f;
        char id[32]; snprintf(id, sizeof id, "##placed%d_%zu", plotId, i);
        plt::PlotInfLines(id, xs, 1, ls);
        if (plotId == 0) {
            const char* nm = q.kind == WsKind::Unknown ? tr("Automatic") : wsKindName(q.kind);
            plt::PlotText(nm, x, lim.Y.Max, ImVec2(3 * gUi, (bandPlanStripHeight() + 16 * gUi)));
        }
    }
    const ImGuiIO& io = ImGui::GetIO();
    if (M.plot < 0) {
        if (plt::IsPlotHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) { M.plot = plotId; M.p0 = io.MousePos; }
        return;
    }
    if (M.plot != plotId) return;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) return;
    M.plot = -1;
    if (!ImGui::IsMouseReleased(ImGuiMouseButton_Middle) || !placedIsTap(io.MousePos.x - M.p0.x, io.MousePos.y - M.p0.y)) return;
    if (!plt::IsPlotHovered()) return;
    const double mhz = plt::GetPlotMousePos().x;
    const double hzPerPx = (lim.X.Max - lim.X.Min) * 1e6 / std::max(1.f, plt::GetPlotSize().x);
    M.absHz = std::round(mhz * 1e6 / 100.0) * 100.0;
    M.tolHz = 6 * gUi * hzPerPx;
    M.pos = io.MousePos;
    M.open = true;
}

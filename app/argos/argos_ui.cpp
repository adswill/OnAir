// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Argos screens. Uplink: the bursts heard (PTT-A2 decoded to the platform ID and data, other modulations listed only), the platforms with
// the frequencies of their bursts (Doppler and drift). Downlink: the carrier followed through the pass with the predicted Doppler of the
// MetOp satellites above the horizon (from the pass planner's TLEs), and the HDLC frames in hex. Receive only.
#include "app.h"
#include "dect2/argos/argos_rx.h"
#include "dect2/argos/argos_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace argos = dect2::argos;

namespace {

struct State {
    int view = 0;       // 0 bursts, 1 platforms, 2 downlink, 3 raw
    int sel = 0, link = 0;
    bool loaded = false, linkSent = false;
};
State S;

bool live(const App& a) { return a.rx.standard == 153; }   // the engine reports its standard code minus one

void load(App& a) {
    if (!S.loaded) {
        S.loaded = true;
        S.view = (int)std::max(0L, std::min(3L, (long)plat::prefs().getI("argosView", 0)));
        S.link = (int)std::max(0L, std::min(2L, (long)plat::prefs().getI("argosLink", 0)));
    }
    if (!S.linkSent) { a.engine.argos().setLink(S.link); S.linkSent = true; }
}

std::string clockText(int64_t wall) {
    char b[32];
    const time_t t = (time_t)wall;
    struct tm g {};
#ifdef _WIN32
    localtime_s(&g, &t);
#else
    localtime_r(&t, &g);
#endif
    strftime(b, sizeof b, "%H:%M:%S", &g);
    return b;
}

const char* kHelp = N_(
    "Argos collects data from small transmitters (PTTs) on buoys, floats, animals and weather stations through low-orbit satellites. A PTT sends a burst every "
    "minute or so on 401.650 MHz: 160 ms of plain carrier, then the message at 400 bit/s. In the A2 format the message holds 24 synchronisation bits, a 4 bit length "
    "field, the 28 bit platform ID and 24 to 248 bits of data. The satellite locates the PTT from the Doppler shift of its bursts. Newer formats (A3, ZE, Argos-4) "
    "use other modulations whose specifications are not public: OnAir lists those bursts without decoding them.\n\nThe satellites (MetOp-B, MetOp-C) also send a "
    "downlink on 465.9875 MHz: a 400 bit/s Manchester signal carrying HDLC frames, with a Doppler shift of up to about 10 kHz during a pass. What the frames hold is "
    "not public; they are shown as hex.\n\nOnAir only receives.");

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

void helpButton() {
    if (ImGui::SmallButton("?##arghelp")) ImGui::OpenPopup("##arghelppop");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("What Argos sends"));
    if (ImGui::BeginPopup("##arghelppop")) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34);
        ImGui::TextUnformatted(tr(kHelp));
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
}

const char* kindName(int k) { return k == 1 ? "PTT-A2" : k == 2 ? tr("other (not A2)") : "?"; }

void burstDetails(App& a, const dect2::ArgosBurst& e) {
    kvRow(a, tr("Format"), "%s", kindName(e.kind));
    if (e.kind == 1) {
        kvRow(a, tr("Platform ID"), "%s (%u), %s %05X", e.msg.idHex.c_str(), e.msg.id, tr("as a 20 bit ID"), e.msg.id20);
        kvRow(a, tr("User data"), "%d bits: %s", e.msg.dataBits, argos::hexOf(e.msg.data).c_str());
        kvRow(a, tr("Length field"), "%d %s", e.msg.lenField, tr("(as received; its coding is not public)"));
        kvRow(a, tr("Sync bits"), "%s %s", e.msg.syncHex.c_str(), tr("(as received; the pattern is not public)"));
        kvRow(a, tr("Quality"), "%.2f", e.quality);
    } else {
        kvRow(a, tr("Note"), "%s", tr("A carrier, then a modulation that is not A2 Manchester (A3 QPSK, ZE, Argos-4?): not decoded, its specification is not public"));
    }
    kvRow(a, tr("Carrier"), "%+.0f Hz %s, %.0f ms %s", e.carrierHz, tr("from the dial"), e.carrierMs, tr("unmodulated"));
    kvRow(a, tr("Burst"), "%.0f ms, %.1f dB, %.0f dBFS", e.durationMs, e.snrDb, e.levelDb);
}

void burstsView(App& a) {
    const dect2::ArgosTelemetry& t = a.rx.argos;
    if (t.log.empty()) { ImGui::TextDisabled("%s", tr("no burst heard yet; a PTT sends about once a minute, and only nearby ones are heard on the ground")); return; }
    S.sel = std::max(0, std::min((int)t.log.size() - 1, S.sel));
    const float tableH = std::max(110.f * gUi, ImGui::GetContentRegionAvail().y * 0.55f);
    if (ImGui::BeginTable("##arglog", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit, ImVec2(0, tableH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Seen"));
        ImGui::TableSetupColumn(tr("Format"));
        ImGui::TableSetupColumn(tr("Platform ID"));
        ImGui::TableSetupColumn(tr("Data"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Offset"));
        ImGui::TableSetupColumn(tr("Level"));
        ImGui::TableSetupColumn(tr("SNR"));
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < t.log.size(); i++) {
            const auto& e = t.log[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char l[48];
            snprintf(l, sizeof l, "%s##arg%zu", clockText(e.wallTime).c_str(), i);
            if (ImGui::Selectable(l, S.sel == (int)i, ImGuiSelectableFlags_SpanAllColumns)) S.sel = (int)i;
            ImGui::TableNextColumn(); ImGui::TextUnformatted(kindName(e.kind));
            ImGui::PushFont(a.mono, 0);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.kind == 1 ? e.msg.idHex.c_str() : "-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.kind == 1 ? argos::hexOf(e.msg.data).c_str() : "-");
            ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::Text("%+.0f Hz", e.carrierHz);
            ImGui::TableNextColumn(); ImGui::Text("%.0f dBFS", e.levelDb);
            ImGui::TableNextColumn(); ImGui::Text("%.0f dB", e.snrDb);
        }
        ImGui::EndTable();
    }
    ImGui::BeginChild("##argdet", ImVec2(0, 0), ImGuiChildFlags_Borders);
    burstDetails(a, t.log[(size_t)S.sel]);
    ImGui::EndChild();
}

// a small line plot of (x, y) points
void plot(const char* id, const std::vector<std::vector<ImVec2>>& lines, const std::vector<ImU32>& cols, ImVec2 size, const char* yUnit) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(10, 14, 20, 255));
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    for (const auto& l : lines) for (const ImVec2& v : l) { x0 = std::min(x0, v.x); x1 = std::max(x1, v.x); y0 = std::min(y0, v.y); y1 = std::max(y1, v.y); }
    ImGui::Dummy(size);
    if (x1 <= x0) { dl->AddText(ImVec2(p0.x + 6 * gUi, p0.y + 4 * gUi), IM_COL32(140, 146, 152, 255), tr("no points yet")); return; }
    if (y1 - y0 < 10) { const float m = (y0 + y1) / 2; y0 = m - 5; y1 = m + 5; }
    const float pad = 0.08f * (y1 - y0);
    y0 -= pad; y1 += pad;
    auto P = [&](ImVec2 v) { return ImVec2(p0.x + (v.x - x0) / (x1 - x0) * size.x, p0.y + size.y - (v.y - y0) / (y1 - y0) * size.y); };
    for (size_t i = 0; i < lines.size(); i++) {
        const auto& l = lines[i];
        for (size_t k = 1; k < l.size(); k++) dl->AddLine(P(l[k - 1]), P(l[k]), cols[i], 1.5f * gUi);
        if (l.size() == 1) dl->AddCircleFilled(P(l[0]), 2.5f * gUi, cols[i]);
        for (const ImVec2& v : l) if (l.size() < 80) dl->AddCircleFilled(P(v), 2.f * gUi, cols[i]);
    }
    char b[64];
    snprintf(b, sizeof b, "%+.0f %s", y1, yUnit); dl->AddText(ImVec2(p0.x + 4 * gUi, p0.y + 2 * gUi), IM_COL32(150, 156, 160, 255), b);
    snprintf(b, sizeof b, "%+.0f %s", y0, yUnit); dl->AddText(ImVec2(p0.x + 4 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 2 * gUi), IM_COL32(150, 156, 160, 255), b);
    (void)id;
}

void platformsView(App& a) {
    const dect2::ArgosTelemetry& t = a.rx.argos;
    if (t.ptts.empty()) { ImGui::TextDisabled("%s", tr("no A2 platform decoded yet")); return; }
    static int psel = 0;
    psel = std::max(0, std::min((int)t.ptts.size() - 1, psel));
    const float tableH = std::max(100.f * gUi, ImGui::GetContentRegionAvail().y * 0.45f);
    if (ImGui::BeginTable("##argptt", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, tableH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Platform ID")); ImGui::TableSetupColumn(tr("Bursts")); ImGui::TableSetupColumn(tr("Last"));
        ImGui::TableSetupColumn(tr("Drift")); ImGui::TableSetupColumn(tr("Last data"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < t.ptts.size(); i++) {
            const auto& p = t.ptts[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char l[48];
            snprintf(l, sizeof l, "%s##argp%zu", p.idHex.c_str(), i);
            ImGui::PushFont(a.mono, 0);
            if (ImGui::Selectable(l, psel == (int)i, ImGuiSelectableFlags_SpanAllColumns)) psel = (int)i;
            ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::Text("%d", p.count);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clockText(p.lastWall).c_str());
            ImGui::TableNextColumn(); if (p.freq.size() >= 2) ImGui::Text("%+.1f Hz/min", p.driftHzPerMin); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(p.lastData.c_str()); ImGui::PopFont();
        }
        ImGui::EndTable();
    }
    const auto& p = t.ptts[(size_t)psel];
    ImGui::TextDisabled("%s %s: %s", tr("Burst frequencies of"), p.idHex.c_str(), tr("a PTT on the ground drifts slowly; one on a moving vehicle or aircraft shows its Doppler"));
    std::vector<ImVec2> pts;
    for (const auto& [ts, hz] : p.freq) pts.push_back(ImVec2((float)(ts - p.freq.front().first), (float)hz));
    plot("##argpf", {pts}, {IM_COL32(120, 190, 255, 255)}, ImVec2(ImGui::GetContentRegionAvail().x, std::max(80.f * gUi, ImGui::GetContentRegionAvail().y - 4 * gUi)), "Hz");
}

void downlinkView(App& a) {
    const dect2::ArgosTelemetry& t = a.rx.argos;
    if (t.link != 1) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("The receiver is on the uplink. Tune to 465.9875 MHz (or pick Downlink above) during a MetOp pass; the pass planner (Tools > Satellite passes) lists MetOp-B / -C with the Argos downlink."));
        ImGui::PopTextWrapPos();
        return;
    }
    static const char* const kCod[] = {"NRZ", "NRZ (inverted)", "NRZI"};
    kvRow(a, tr("Carrier"), "%s, %+.0f Hz %s, %.0f dB %s", t.dlLocked ? tr("followed") : tr("searching"), t.dlCarrierHz, tr("from the dial"), t.dlCnrDb, tr("line over noise"));
    kvRow(a, tr("Bits"), "%llu; %s %llu / %llu / %llu (NRZ / inverted / NRZI)", (unsigned long long)t.dlBits, tr("HDLC flags"), (unsigned long long)t.dlFlags[0], (unsigned long long)t.dlFlags[1], (unsigned long long)t.dlFlags[2]);
    kvRow(a, tr("Coding"), "%s", t.dlCoding >= 0 ? kCod[t.dlCoding] : tr("not settled yet"));
    kvRow(a, tr("Frames"), "%llu %s, %llu %s", (unsigned long long)t.blocksOk, tr("with a good FCS"), (unsigned long long)t.dlFcsBad, tr("failed"));
    // measured and predicted Doppler over the last ten minutes
    std::vector<std::vector<ImVec2>> lines(1);
    std::vector<ImU32> cols = {IM_COL32(120, 220, 140, 255)};
    const double now = (double)std::time(nullptr);
    for (const auto& [ts, hz] : t.doppler) if (now - ts < 600) lines[0].push_back(ImVec2((float)(ts - now), (float)hz));
    std::map<std::string, std::vector<ImVec2>> pred;
    const double f0 = t.dialHz > 0 ? t.dialHz : argos::kDownlinkMhz * 1e6;
    for (int k = 0; k <= 60; k++) {
        const double ts = now - 600 + 10.0 * k;
        for (const auto& [name, hz] : passesArgosDoppler(ts, f0)) pred[name].push_back(ImVec2((float)(ts - now), (float)hz));
    }
    std::string predNames;
    for (auto& [name, v] : pred) { lines.push_back(v); cols.push_back(IM_COL32(250, 190, 90, 255)); predNames += (predNames.empty() ? "" : ", ") + name; }
    ImGui::TextDisabled("%s", predNames.empty() ? tr("Doppler, last 10 minutes (green measured; no MetOp above the horizon, or no TLEs / receiver position for a prediction)")
                                               : (std::string(tr("Doppler, last 10 minutes: green measured, orange predicted for ")) + predNames).c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The two curves differ by the radio's own frequency error. When the pass planner tracks the pass it retunes for the Doppler, and the measured curve is what is left."));
    plot("##argdop", lines, cols, ImVec2(ImGui::GetContentRegionAvail().x, 110 * gUi), "Hz");
    if (t.frames.empty()) { ImGui::TextDisabled("%s", tr("no frame yet")); return; }
    if (!ImGui::BeginTable("##argdl", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Seen")); ImGui::TableSetupColumn(tr("Bytes")); ImGui::TableSetupColumn("FCS"); ImGui::TableSetupColumn(tr("Frame"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& f : t.frames) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(clockText(f.wallTime).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%d", f.bytes);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(f.fcsOk ? tr("good") : tr("failed"));
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(f.hex.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
    }
    ImGui::EndTable();
}

void rawView(App& a) {
    const dect2::ArgosTelemetry& t = a.rx.argos;
    if (ImGui::SmallButton("copy all##argraw")) {
        std::string all;
        for (auto it = t.log.rbegin(); it != t.log.rend(); ++it) if (it->kind == 1) all += clockText(it->wallTime) + "  " + it->msg.rawHex + "\n";
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::BeginChild("##arg_raw", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::PushFont(a.mono, 0);
    for (const auto& e : t.log) {
        if (e.kind != 1) continue;
        ImGui::TextDisabled("%s", clockText(e.wallTime).c_str());
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(e.msg.rawHex.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::PopFont();
    if (t.log.empty()) ImGui::TextDisabled("%s", tr("nothing decoded yet"));
    ImGui::EndChild();
}

void tab(App& a) {
    load(a);
    helpButton();
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Clear"))) a.engine.argos().clearResults();
    ImGui::SameLine();
    const char* links[] = {tr("Automatic"), tr("Uplink 401.65 MHz"), tr("Downlink 465.9875 MHz")};
    ImGui::SetNextItemWidth(170 * gUi);
    if (ImGui::Combo("##arglink", &S.link, links, 3)) { plat::prefs().setI("argosLink", S.link); a.engine.argos().setLink(S.link); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Automatic: the downlink above 430 MHz, the uplink below (and for a file)"));
    ImGui::SameLine();
    if (live(a)) ImGui::TextDisabled("%s", dect2::argosSummary(a.rx.argos).c_str());
    const int v0 = S.view;
    subNav("argv", S.view, {tr("Bursts"), tr("Platforms"), tr("Downlink"), tr("Raw bits")});
    if (S.view != v0) plat::prefs().setI("argosView", S.view);
    if (!live(a)) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("tune to 401.650 MHz (PTT uplink) or 465.9875 MHz (satellite downlink) and start the receiver"));
        ImGui::PopTextWrapPos();
        return;
    }
    if (S.view == 1) platformsView(a);
    else if (S.view == 2) downlinkView(a);
    else if (S.view == 3) rawView(a);
    else burstsView(a);
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::ArgosTelemetry& t = a.rx.argos;
    kvRow(a, tr("State"), "%s", t.state == 0 ? tr("no input") : t.state == 1 ? tr("listening") : tr("decoded lately"));
    kvRow(a, tr("Link"), "%s", t.link == 1 ? tr("downlink (satellite)") : tr("uplink (PTTs)"));
    if (t.link == 0) {
        kvRow(a, tr("Bursts"), "%llu %s, %llu A2, %llu %s", (unsigned long long)t.bursts, tr("heard"), (unsigned long long)t.blocksOk, (unsigned long long)t.otherBursts, tr("other"));
        kvRow(a, tr("Last burst"), "%.1f dB, %+.0f Hz", t.snrDb, t.cfoHz);
    } else {
        kvRow(a, tr("Carrier"), "%s, %+.0f Hz", t.dlLocked ? tr("followed") : tr("searching"), t.dlCarrierHz);
        kvRow(a, tr("Frames"), "%llu", (unsigned long long)t.blocksOk);
    }
    kvRow(a, tr("Input"), "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
}

void list(App& a) {
    if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to see platforms")); ImGui::PopTextWrapPos(); return; }
    const dect2::ArgosTelemetry& t = a.rx.argos;
    ImGui::TextDisabled("%zu", t.ptts.size());
    if (!ImGui::BeginTable("##arg_l", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Platform ID"), ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("n");
    ImGui::TableHeadersRow();
    for (const auto& p : t.ptts) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(p.idHex.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::Text("%d", p.count);
    }
    ImGui::EndTable();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const dect2::ArgosTelemetry& t = a.rx.argos;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Carrier"), !on ? 0 : (t.link == 1 ? (t.dlLocked ? 1 : 0) : (t.bursts > 0 ? 1 : 0))); flowNext(10 * gUi);
    lamp(tr("Data"), !on ? 0 : t.state >= 2 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(a.mono, 0);
    if (on) ImGui::Text("%llu", (unsigned long long)t.blocksOk); else ImGui::TextUnformatted(run ? tr("starting") : tr("stopped"));
    ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Argos";
    if (live(a)) l2 = dect2::argosSummary(a.rx.argos);
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    static const char* const kL[] = {"uplink (PTT-A2)", "downlink"};
    int l = sc.modeOpt[0] == 1 ? 1 : 0;
    ImGui::SetNextItemWidth(140 * gUi);
    if (ImGui::Combo("##argsl", &l, kL, 2)) {
        sc.modeOpt[0] = l; changed = true;
        S.link = l == 1 ? 2 : 1;   // the receiver follows the test signal's link
        plat::prefs().setI("argosLink", S.link);
        a.engine.argos().setLink(S.link);
    }
    flowNext();
    if (l == 0) {
        bool other = sc.modeOpt[2] == 1;
        if (ImGui::Checkbox(tr("a burst of another modulation"), &other)) { sc.modeOpt[2] = other ? 1 : 0; changed = true; }
        flowNext(); ImGui::TextDisabled("%s", tr("drift")); ImGui::SameLine(); ImGui::SetNextItemWidth(100 * gUi);
        float d = (float)sc.modeVal[0];
        if (ImGui::SliderFloat("##argdr", &d, -50.f, 50.f, "%.0f Hz/s")) { sc.modeVal[0] = d; changed = true; }
        flowNext(); ImGui::TextDisabled("%s", tr("step per burst")); ImGui::SameLine(); ImGui::SetNextItemWidth(100 * gUi);
        float st = (float)sc.modeVal[2];
        if (ImGui::SliderFloat("##argst", &st, -50.f, 50.f, "%.0f Hz")) { sc.modeVal[2] = st; changed = true; }
    } else {
        bool bp = sc.modeOpt[1] == 1;
        if (ImGui::Checkbox("BPSK", &bp)) { sc.modeOpt[1] = bp ? 1 : 0; changed = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("+-90 degrees instead of +-1.1 rad phase modulation"));
        flowNext();
        bool nz = sc.modeOpt[2] == 1;
        if (ImGui::Checkbox("NRZI", &nz)) { sc.modeOpt[2] = nz ? 1 : 0; changed = true; }
        flowNext(); ImGui::TextDisabled("%s", tr("pass Doppler")); ImGui::SameLine(); ImGui::SetNextItemWidth(100 * gUi);
        float dm = (float)sc.modeVal[0];
        if (ImGui::SliderFloat("##argdm", &dm, 0.f, 10000.f, "%.0f Hz")) { sc.modeVal[0] = dm; changed = true; }
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", l == 0 ? tr("Three made-up platforms in turn (24, 56 and 248 data bits) at their own frequencies. The 24 synchronisation bits are a stand-in: the real pattern is not public.")
                                     : tr("A continuous 400 bit/s Manchester signal with made-up HDLC frames, following the Doppler curve of a pass."));
    ImGui::PopTextWrapPos();
}

void tick(App& a) { load(a); }

} // namespace

extern const ModeUi kArgosUi;
const ModeUi kArgosUi = {
    .sideTitle = "PLATFORMS",
    .tabName = "Argos",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .synth = synth,
    .tick = tick,
};

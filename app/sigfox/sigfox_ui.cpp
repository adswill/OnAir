// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sigfox screens: the uplinks heard (device ID, message counter, payload, the ranks of the repetitions, frequency offsets, level, the
// authentication code and CRC), the 192 kHz waterfall of the bursts, and the devices. Receive only: nothing is decrypted, and the
// authentication code is never checked (it needs the device's secret key).
#include "app.h"
#include "dect2/sigfox/sigfox_proto.h"
#include "dect2/sigfox/sigfox_rx.h"
#include "dect2/sigfox/sigfox_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;   // 0 uplinks, 1 waterfall, 2 zones
    int sel = 0;
    bool loaded = false;
};
State S;

bool live(const App& a) { return a.rx.standard == 154; }   // the engine reports its standard code minus one

void load() {
    if (S.loaded) return;
    S.loaded = true;
    S.view = (int)std::max(0L, std::min(2L, (long)plat::prefs().getI("sigfoxView", 0)));
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
    "A Sigfox device sends each message as one frame or three (ranks 1, 2 and 3) on different frequencies inside a 200 kHz channel, each about 100 Hz wide "
    "(100 bit/s D-BPSK; 600 bit/s in RC2 and RC4). A frame holds a frame type (it names the size and the rank), a length indicator, the downlink request flag, "
    "a 12 bit message counter, the 32 bit device ID, 0 to 12 payload bytes (or a single bit), 2 to 5 bytes of authentication code and a CRC-16. The second and "
    "third ranks are the same content under a convolution code.\n\nOnAir only receives. The CRC is checked; the authentication code needs the device's secret key "
    "and is never checked, and payloads are often encrypted by the application, so they are shown as hex.");

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

void helpButton() {
    if (ImGui::SmallButton("?##sfxhelp")) ImGui::OpenPopup("##sfxhelppop");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("What a Sigfox uplink holds"));
    if (ImGui::BeginPopup("##sfxhelppop")) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34);
        ImGui::TextUnformatted(tr(kHelp));
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
}

std::string ranksText(int mask) {
    std::string s;
    for (int r = 0; r < 3; r++) if (mask & (1 << r)) { if (!s.empty()) s += " "; s += std::to_string(r + 1); }
    return s.empty() ? "-" : s;
}

std::string payloadText(const dect2::SigfoxMessage& m) {
    if (m.singleBit) return m.bitValue ? "bit 1" : "bit 0";
    return m.payload.empty() ? "(none)" : m.payload;
}

std::string offsets(const dect2::SigfoxModeTelemetry& t, const dect2::SigfoxMessage& m) {
    std::string s;
    char b[32];
    for (double f : m.freqs) {
        snprintf(b, sizeof b, "%s%+.1f", s.empty() ? "" : " ", (f - (t.dialHz > 0 ? t.dialHz : 0)) / 1e3);
        s += b;
    }
    return s + " kHz";
}

void details(App& a, const dect2::SigfoxModeTelemetry& t, const dect2::SigfoxMessage& m) {
    kvRow(a, tr("Device ID"), "%s", m.id.c_str());
    kvRow(a, tr("Message counter"), "%u (0x%03X)", m.seq, m.seq);
    kvRow(a, tr("Payload"), "%s%s", payloadText(m).c_str(), m.control ? tr(" (control message frame type)") : "");
    kvRow(a, tr("Length"), "%d %s, %d %s", m.len, tr("bytes"), m.container, tr("byte container"));
    kvRow(a, tr("Downlink asked"), "%s", m.downlinkRequest ? tr("yes") : tr("no"));
    kvRow(a, tr("Repetitions"), "%d (%s %s)", m.reps, tr("ranks"), ranksText(m.rankMask).c_str());
    kvRow(a, tr("Frequencies"), "%s", offsets(t, m).c_str());
    kvRow(a, tr("Bit rate"), "%d bit/s", m.baud);
    kvRow(a, tr("Level"), "%.0f dBFS", m.levelDb);
    kvRow(a, tr("Authentication"), "%s %s", m.mac.c_str(), tr("(not checked: needs the device key)"));
    kvRow(a, tr("CRC"), "%s %s", m.crc.c_str(), tr("good"));
    kvRow(a, tr("Frame type"), "0x%04X", m.frameType);
}

void uplinksView(App& a) {
    const dect2::SigfoxModeTelemetry& t = a.rx.sigfoxm;
    const auto& ms = t.sf.messages;
    if (ms.empty()) { ImGui::TextDisabled("%s", tr("no uplink decoded yet; a device sends when it has something to say, often a few times an hour")); return; }
    S.sel = std::max(0, std::min((int)ms.size() - 1, S.sel));
    const float tableH = std::max(110.f * gUi, ImGui::GetContentRegionAvail().y * 0.55f);
    if (ImGui::BeginTable("##sfxlog", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit, ImVec2(0, tableH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Seen"));
        ImGui::TableSetupColumn(tr("Device ID"));
        ImGui::TableSetupColumn(tr("Counter"));
        ImGui::TableSetupColumn(tr("Payload"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Ranks"));
        ImGui::TableSetupColumn(tr("Offset"));
        ImGui::TableSetupColumn(tr("Level"));
        ImGui::TableSetupColumn(tr("Rate"));
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < ms.size(); i++) {
            const auto& m = ms[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char l[48];
            snprintf(l, sizeof l, "%s##sfx%zu", clockText(m.wallTime).c_str(), i);
            if (ImGui::Selectable(l, S.sel == (int)i, ImGuiSelectableFlags_SpanAllColumns)) S.sel = (int)i;
            ImGui::PushFont(a.mono, 0);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m.id.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%u", m.seq);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(payloadText(m).c_str());
            ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(ranksText(m.rankMask).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%+.1f kHz", m.freqs.empty() ? 0.0 : (m.freqs[0] - (t.dialHz > 0 ? t.dialHz : 0)) / 1e3);
            ImGui::TableNextColumn(); ImGui::Text("%.0f dB", m.levelDb);
            ImGui::TableNextColumn(); ImGui::Text("%d", m.baud);
        }
        ImGui::EndTable();
    }
    ImGui::BeginChild("##sfxdet", ImVec2(0, 0), ImGuiChildFlags_Borders);
    details(a, t, ms[(size_t)S.sel]);
    ImGui::EndChild();
}

void waterfallView(App& a) {
    const dect2::SigfoxTelemetry& sf = a.rx.sigfoxm.sf;
    const float w = ImGui::GetContentRegionAvail().x, h = std::max(80.f * gUi, ImGui::GetContentRegionAvail().y - 2.5f * ImGui::GetTextLineHeightWithSpacing());
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(8, 12, 20, 255));
    if (sf.cols > 0) {
        const size_t nr = sf.rows.size() / (size_t)sf.cols;
        const float cw = w / (float)sf.cols, rh = h / (float)dect2::kSigfoxRowsMax;
        for (size_t r = 0; r < nr; r++)
            for (int c = 0; c < sf.cols; c++) {
                const int v = sf.rows[r * (size_t)sf.cols + (size_t)c];
                if (v < 40) continue;
                const float y = p0.y + h - (float)(nr - r) * rh;
                dl->AddRectFilled(ImVec2(p0.x + c * cw, y), ImVec2(p0.x + (c + 1) * cw + 0.5f, y + rh + 0.5f), IM_COL32(40 + v * 3 / 4, 80 + v / 2, 255 - v / 2, 255));
            }
    }
    ImGui::Dummy(ImVec2(w, h));
    ImGui::TextDisabled("-96 kHz"); ImGui::SameLine(w / 2 - 20 * gUi); ImGui::TextDisabled("%s", tr("dial")); ImGui::SameLine(w - 50 * gUi); ImGui::TextDisabled("+96 kHz");
    ImGui::TextDisabled("%s", tr("each row is 0.1 s, newest at the bottom; a burst is a short vertical line"));
}

void zonesView(App&) {
    int n = 0;
    const dect2::sigfox::Zone* z = dect2::sigfox::zones(n);
    if (!ImGui::BeginTable("##sfxz", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) return;
    ImGui::TableSetupColumn(tr("Zone")); ImGui::TableSetupColumn(tr("Uplink")); ImGui::TableSetupColumn(tr("Downlink")); ImGui::TableSetupColumn(tr("Rate"));
    ImGui::TableSetupColumn(tr("Where"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (int i = 0; i < n; i++) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(z[i].name);
        ImGui::TableNextColumn(); ImGui::Text("%.3f MHz", z[i].ulMhz);
        ImGui::TableNextColumn(); ImGui::Text("%.3f MHz", z[i].dlMhz);
        ImGui::TableNextColumn(); ImGui::Text("%d bit/s", z[i].baud);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(z[i].where);
    }
    ImGui::EndTable();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Centre frequencies of the 200 kHz uplink channel from the public Sigfox radio specification. The downlink (GFSK, 600 bit/s) is not decoded."));
    ImGui::PopTextWrapPos();
}

void tab(App& a) {
    load();
    helpButton();
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Clear"))) a.engine.sigfox().clearResults();
    ImGui::SameLine();
    if (live(a)) ImGui::TextDisabled("%s", dect2::sigfoxModeSummary(a.rx.sigfoxm).c_str());
    const int v0 = S.view;
    subNav("sfxv", S.view, {tr("Uplinks"), tr("Waterfall"), tr("Zones")});
    if (S.view != v0) plat::prefs().setI("sigfoxView", S.view);
    if (S.view == 2) { zonesView(a); return; }
    if (!live(a)) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("tune to 868.130 MHz (Europe) or 902.200 MHz (Americas) and start the receiver"));
        ImGui::PopTextWrapPos();
        return;
    }
    if (S.view == 1) waterfallView(a); else uplinksView(a);
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::SigfoxModeTelemetry& t = a.rx.sigfoxm;
    kvRow(a, tr("State"), "%s", t.state == 0 ? tr("no input (needs 400 ksps or more)") : t.state == 1 ? tr("listening") : tr("uplink decoded lately"));
    kvRow(a, tr("Zone"), "%s", t.zone.empty() ? tr("the dial is on no Sigfox zone (listening anyway)") : (t.zone + ", " + std::to_string(t.zoneBaud) + " bit/s").c_str());
    kvRow(a, tr("Carriers"), "%llu %s", (unsigned long long)t.sf.carriers, tr("narrow carriers followed"));
    kvRow(a, tr("Frames"), "%llu %s", (unsigned long long)t.sf.frames, tr("decoded with a good CRC"));
    kvRow(a, tr("Input"), "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
}

void list(App& a) {
    if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to see devices")); ImGui::PopTextWrapPos(); return; }
    const auto& ms = a.rx.sigfoxm.sf.messages;
    struct Row { const dect2::SigfoxMessage* m; int n; size_t idx; };
    std::vector<Row> rows;
    for (size_t i = 0; i < ms.size(); i++) {
        bool found = false;
        for (Row& r : rows) if (r.m->id == ms[i].id) { r.n++; found = true; break; }
        if (!found) rows.push_back({&ms[i], 1, i});
    }
    ImGui::TextDisabled("%zu", rows.size());
    if (!ImGui::BeginTable("##sfx_l", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Device ID"), ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn(tr("Counter")); ImGui::TableSetupColumn("n");
    ImGui::TableHeadersRow();
    for (const Row& r : rows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID((int)r.idx);
        if (ImGui::Selectable("##r", (int)r.idx == S.sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) { S.sel = (int)r.idx; S.view = 0; }
        ImGui::PopID();
        ImGui::SameLine(0, 0);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(r.m->id.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::Text("%u", r.m->seq);
        ImGui::TableNextColumn(); ImGui::Text("%d", r.n);
    }
    ImGui::EndTable();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const dect2::SigfoxModeTelemetry& t = a.rx.sigfoxm;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Carrier"), !on ? 0 : t.sf.carriers > 0 ? 1 : 0); flowNext(10 * gUi);
    lamp(tr("Uplink"), !on ? 0 : t.state >= 2 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(a.mono, 0);
    if (on) ImGui::Text("%llu", (unsigned long long)t.sf.frames); else ImGui::TextUnformatted(run ? tr("starting") : tr("stopped"));
    ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Sigfox";
    if (live(a)) l2 = dect2::sigfoxModeSummary(a.rx.sigfoxm);
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    bool fast = sc.modeOpt[0] == 1;
    if (ImGui::Checkbox("600 bit/s", &fast)) { sc.modeOpt[0] = fast ? 1 : 0; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The RC2 / RC4 rate instead of 100 bit/s."));
    flowNext();
    bool one = sc.modeOpt[1] == 1;
    if (ImGui::Checkbox(tr("one frame per message"), &one)) { sc.modeOpt[1] = one ? 1 : 0; changed = true; }
    flowNext(); ImGui::TextDisabled("%s", tr("drift")); ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi);
    float d = (float)sc.modeVal[0];
    if (ImGui::SliderFloat("##sfxdrift", &d, -30.f, 30.f, "%.0f Hz/s")) { sc.modeVal[0] = d; changed = true; }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Four made-up devices in turn (4 bytes, a single bit, 12 bytes, 3 bytes asking for a downlink), each message as three frames on different frequencies. The authentication bytes are made up: real ones need the device key."));
    ImGui::PopTextWrapPos();
}

void tick(App&) {}

} // namespace

extern const ModeUi kSigfoxUi;
const ModeUi kSigfoxUi = {
    .sideTitle = "DEVICES",
    .tabName = "Sigfox",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .synth = synth,
    .tick = tick,
};

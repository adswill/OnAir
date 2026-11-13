// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The passive radar window (View > Passive radar): aircraft from the echoes of a broadcast transmitter, with a reference and a surveillance
// channel. Sources: a built-in simulator with synthetic targets (demo), a two-channel cf32 recording, or two single-channel recordings.
// Shows the range-Doppler map, a Doppler-time waterfall, the tracks and a local map with the bistatic range ellipses around the illuminator
// and the receiver. Nothing here talks to a radio: see docs/passive-radar.md for what is and is not verified.
#include "../app.h"
#include "../ui/scale.h"
#include "dect2/radar/passive.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>

namespace {

using namespace dect2::radar;

enum SrcKind { SrcSim, SrcFile2ch, SrcFiles, NumSrc };
struct Illuminator { std::string name; double lat = 0, lon = 0, mhz = 100; };

struct State {
    bool loaded = false;
    int src = SrcSim;
    bool run = true, lockTuned = false, nlms = false;
    float mhz = 98.0f, rateKhz = 250.f;
    int taps = 12;
    float pfaExp = 6.f;                            // Pfa = 10^-pfaExp
    char path[512] = "", path2[512] = "";
    // simulator
    float simRangeKm = 45.f, simRate = -220.f, simLevel = -80.f;
    bool sim2 = false, simOfdm = false;
    // illuminator
    std::vector<Illuminator> ill;
    int cur = -1;
    char name[64] = "";
    double txLat = 0, txLon = 0;
    bool txSet = false;
    // run
    std::unique_ptr<PassiveRadar> radar;
    std::unique_ptr<dect2::df::MultiChannelSource> source;
    PassiveSimSource* sim = nullptr;
    RadarResult last;
    bool have = false;
    double lastAt = 0;
    std::deque<std::vector<float>> wf;             // Doppler-time waterfall, newest first
    std::vector<Track> tracks;
    std::string msg;
};
State S;

std::string illString() {
    std::string s;
    char b[256];
    for (const Illuminator& i : S.ill) { snprintf(b, sizeof b, "%s|%.6f|%.6f|%.4f;", i.name.c_str(), i.lat, i.lon, i.mhz); s += b; }
    return s;
}
void load() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& p = plat::prefs();
    S.src = std::clamp((int)p.getI("prSrc", 0), 0, NumSrc - 1);
    S.mhz = (float)p.getD("prMhz", 98.0); S.taps = std::clamp((int)p.getI("prTaps", 12), 4, 32);
    S.lockTuned = p.getB("prLock", false); S.nlms = p.getB("prNlms", false);
    S.txLat = p.getD("prTxLat", 0); S.txLon = p.getD("prTxLon", 0); S.txSet = p.getB("prTxSet", false);
    const std::string c = p.getS("prIll", "");
    size_t pos = 0;
    while (pos < c.size()) {
        size_t e = c.find(';', pos);
        if (e == std::string::npos) break;
        char nm[64] = ""; double la = 0, lo = 0, mh = 100;
        if (sscanf(c.substr(pos, e - pos).c_str(), "%63[^|]|%lf|%lf|%lf", nm, &la, &lo, &mh) == 4) S.ill.push_back({nm, la, lo, mh});
        pos = e + 1;
    }
}
void save() {
    plat::Prefs& p = plat::prefs();
    p.setI("prSrc", S.src); p.setD("prMhz", S.mhz); p.setI("prTaps", S.taps); p.setB("prLock", S.lockTuned); p.setB("prNlms", S.nlms);
    p.setD("prTxLat", S.txLat); p.setD("prTxLon", S.txLon); p.setB("prTxSet", S.txSet);
    p.setS("prIll", illString());
}

double carrierHz(const App& a) { return S.lockTuned ? a.freqMhz * 1e6 : (double)S.mhz * 1e6; }

RadarConfig makeConfig(double hz, double fs) {
    RadarConfig c;
    c.caf.sampleRate = fs; c.caf.carrierHz = hz;
    c.caf.decim = std::max(1, (int)std::lround(fs / 1000.0));   // 1 kHz slow-time rate: +-500 Hz = +-750 m/s at 100 MHz
    c.ecaTaps = S.taps; c.useNlms = S.nlms;
    c.cfar.pfa = std::pow(10.0, -(double)S.pfaExp); c.cfar.minRangeBin = S.taps;
    return c;
}

void reset() {
    S.source.reset(); S.sim = nullptr; S.radar.reset(); S.have = false; S.wf.clear(); S.tracks.clear(); S.msg.clear();
}

void build(const App& a) {
    reset();
    const double hz = carrierHz(a), fs = S.rateKhz * 1e3;
    if (S.src == SrcSim) {
        PassiveSimConfig sc; sc.sampleRate = fs; sc.carrierHz = hz; sc.seed = (uint64_t)(ImGui::GetTime() * 10) + 1; sc.fm = !S.simOfdm;
        SimTarget t; t.rangeM = S.simRangeKm * 1e3; t.rateMps = S.simRate; t.levelDb = S.simLevel; sc.targets.push_back(t);
        if (S.sim2) { SimTarget u; u.rangeM = 62e3; u.rateMps = 140; u.levelDb = S.simLevel - 4; sc.targets.push_back(u); }
        auto s = std::make_unique<PassiveSimSource>(sc);
        S.sim = s.get(); S.source = std::move(s);
    } else if (S.src == SrcFile2ch && S.path[0]) {
        auto f = std::make_unique<dect2::df::FileMultiChannelSource>(S.path, 2, fs, hz);
        if (f->ok()) S.source = std::move(f); else S.msg = tr("Cannot open the recording.");
    } else if (S.src == SrcFiles && S.path[0] && S.path2[0]) {
        auto f = std::make_unique<TwoFileSource>(S.path, S.path2, fs, hz);
        if (f->ok()) S.source = std::move(f); else S.msg = tr("Cannot open the recording.");
    }
    S.radar = std::make_unique<PassiveRadar>(makeConfig(hz, fs));
}

void stepOnce() {
    if (!S.source || !S.radar) return;
    const CafParams& cp = S.radar->config().caf;
    dect2::df::Frame f;
    if (!S.source->read(f, cp.cpiSamples()) || f.size() < 2 || f[0].size() < cp.cpiSamples()) { S.msg = tr("End of the recording."); S.run = false; return; }
    S.last = S.radar->process(f[0], std::move(f[1]));
    S.have = true;
    S.tracks = S.radar->tracker().tracks();
    std::vector<float> row = S.last.dopplerRow;
    S.wf.push_front(std::move(row));
    while (S.wf.size() > 80) S.wf.pop_back();
}

ImU32 heat(float v) {   // 0..1
    v = std::clamp(v, 0.f, 1.f);
    const float r = std::clamp(v * 3.f, 0.f, 1.f), g = std::clamp(v * 3.f - 1.f, 0.f, 1.f), b = std::clamp(v * 3.f - 2.f, 0.f, 1.f);
    return IM_COL32((int)(20 + 235 * r), (int)(10 + 220 * g), (int)(40 + 120 * (1 - r) + 95 * b), 255);
}

void rangeDoppler(float w, float h) {
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##prrd", ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(12, 14, 18, 255));
    if (!S.have) return;
    const Caf& c = S.last.caf;
    const int dstep = 4, cols = c.nd / dstep;
    std::vector<float> s;
    for (size_t i = 0; i < c.pwr.size(); i += 7) s.push_back(c.pwr[i]);
    std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
    const float floorP = std::max(s[s.size() / 2], 1e-30f);
    const float cw = w / cols, ch = h / c.nr;
    for (int k = 0; k < cols; k++) for (int r = 0; r < c.nr; r++) {
        float m = 0;
        for (int j = 0; j < dstep; j++) m = std::max(m, c.at(k * dstep + j, r));
        const float db = 10 * std::log10(std::max(m / floorP, 1e-3f));
        dl->AddRectFilled(ImVec2(p0.x + k * cw, p0.y + r * ch), ImVec2(p0.x + (k + 1) * cw + 1, p0.y + (r + 1) * ch + 1), heat(db / 40.f));
    }
    for (const Detection& d : S.last.dets) {
        const ImVec2 q(p0.x + (float)(d.dBin + 0.5) / c.nd * w, p0.y + (float)(d.rBin + 0.5) / c.nr * h);
        dl->AddCircle(q, 7 * gUi, IM_COL32(255, 255, 255, 255), 16, 1.5f);
    }
    dl->AddText(ImVec2(p0.x + 4, p0.y + 2), IM_COL32(200, 204, 208, 255), "Doppler");
    char b[48];
    snprintf(b, sizeof b, "%+.0f Hz", c.dopplerHz(0)); dl->AddText(ImVec2(p0.x + 4, p0.y + h - 14 * gUi), IM_COL32(200, 204, 208, 255), b);
    snprintf(b, sizeof b, "%+.0f Hz", c.dopplerHz(c.nd - 1)); dl->AddText(ImVec2(p0.x + w - 70 * gUi, p0.y + h - 14 * gUi), IM_COL32(200, 204, 208, 255), b);
    snprintf(b, sizeof b, "%.0f km", c.rangeM(c.nr) / 1e3); dl->AddText(ImVec2(p0.x + w - 60 * gUi, p0.y + 2), IM_COL32(200, 204, 208, 255), b);
}

void waterfall(float w, float h) {
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##prwf", ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(12, 14, 18, 255));
    if (S.wf.empty()) return;
    const int rows = 80, nd = (int)S.wf[0].size(), step = 4, cols = nd / step;
    const float cw = w / cols, rh = h / rows;
    float lo = 1e9f;
    for (const auto& r : S.wf) for (float v : r) lo = std::min(lo, v);
    std::vector<float> all;
    for (const auto& r : S.wf) for (int i = 0; i < nd; i += 5) all.push_back(r[i]);
    std::nth_element(all.begin(), all.begin() + all.size() / 2, all.end());
    const float med = all[all.size() / 2];
    for (size_t y = 0; y < S.wf.size(); y++) for (int k = 0; k < cols; k++) {
        float m = -200;
        for (int j = 0; j < step; j++) m = std::max(m, S.wf[y][k * step + j]);
        dl->AddRectFilled(ImVec2(p0.x + k * cw, p0.y + y * rh), ImVec2(p0.x + (k + 1) * cw + 1, p0.y + (y + 1) * rh + 1), heat((m - med) / 40.f));
    }
}

LatLon rxPos() { LatLon l; receiverLocation(l.lat, l.lon); return l; }

void localMap(float w, float h) {
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##prmap", ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(18, 20, 22, 255));
    const LatLon rx = rxPos();
    if (!S.txSet) { dl->AddText(ImVec2(p0.x + 6, p0.y + 6), IM_COL32(200, 204, 208, 255), tr("Enter the illuminator position.")); return; }
    const LatLon tx{S.txLat, S.txLon};
    const double mN = 111195.0, mE = 111195.0 * std::cos(rx.lat * 3.14159265 / 180);
    double ext = std::max(30e3, distanceM(tx, rx) * 1.2);
    for (const Track& t : S.tracks) if (t.confirmed) ext = std::max(ext, distanceM(tx, rx) + t.r * 0.6);
    ext = std::min(ext, 300e3);
    const float s = (float)(std::min(w, h) / 2 / ext);
    const ImVec2 c(p0.x + w / 2, p0.y + h / 2);
    auto xy = [&](LatLon p) { double dl2 = p.lon - rx.lon; while (dl2 > 180) dl2 -= 360; while (dl2 < -180) dl2 += 360; return ImVec2(c.x + (float)(dl2 * mE) * s, c.y - (float)((p.lat - rx.lat) * mN) * s); };
    for (int i = 1; i <= 2; i++) dl->AddCircle(c, (float)(ext * i / 2 * s), IM_COL32(50, 54, 58, 255), 48);
    dl->AddText(ImVec2(p0.x + 4, p0.y + 2), IM_COL32(220, 222, 224, 255), "N");
    int idx = 0;
    for (const Track& t : S.tracks) {
        if (!t.confirmed) continue;
        const std::vector<LatLon> e = bistaticEllipse(tx, rx, t.r);
        std::vector<ImVec2> pts;
        for (const LatLon& q : e) pts.push_back(xy(q));
        const ImU32 col = IM_COL32(255, 154 - 25 * (idx % 4), 77 + 40 * (idx % 4), 255);
        if (pts.size() > 2) dl->AddPolyline(pts.data(), (int)pts.size(), col, ImDrawFlags_Closed, 1.5f * gUi);
        idx++;
    }
    dl->AddLine(xy(tx), xy(rx), IM_COL32(110, 114, 118, 255));
    dl->AddCircleFilled(xy(rx), 4 * gUi, IM_COL32(120, 200, 255, 255));
    dl->AddText(ImVec2(xy(rx).x + 6, xy(rx).y), IM_COL32(120, 200, 255, 255), "RX");
    dl->AddTriangleFilled(ImVec2(xy(tx).x, xy(tx).y - 6 * gUi), ImVec2(xy(tx).x - 5 * gUi, xy(tx).y + 4 * gUi), ImVec2(xy(tx).x + 5 * gUi, xy(tx).y + 4 * gUi), IM_COL32(255, 90, 90, 255));
    dl->AddText(ImVec2(xy(tx).x + 8, xy(tx).y - 6), IM_COL32(255, 90, 90, 255), "TX");
}

void illuminatorSection() {
    double rlat, rlon;
    receiverLocation(rlat, rlon);
    bool ch = false;
    if (!S.ill.empty()) {
        ImGui::SetNextItemWidth(200 * gUi);
        if (ImGui::BeginCombo("##prill", S.cur >= 0 && S.cur < (int)S.ill.size() ? S.ill[S.cur].name.c_str() : tr("Saved illuminators"))) {
            for (int i = 0; i < (int)S.ill.size(); i++) if (ImGui::Selectable(S.ill[i].name.c_str(), S.cur == i)) {
                S.cur = i; S.txLat = S.ill[i].lat; S.txLon = S.ill[i].lon; S.mhz = (float)S.ill[i].mhz; S.txSet = true; ch = true;
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SetNextItemWidth(120 * gUi); ch |= ImGui::InputDouble(tr("Latitude"), &S.txLat, 0, 0, "%.5f");
    ImGui::SetNextItemWidth(120 * gUi); ch |= ImGui::InputDouble(tr("Longitude"), &S.txLon, 0, 0, "%.5f");
    if (ch) S.txSet = std::fabs(S.txLat) > 1e-6 || std::fabs(S.txLon) > 1e-6;
    ImGui::SetNextItemWidth(140 * gUi); ImGui::InputText(tr("Name"), S.name, sizeof S.name);
    ImGui::SameLine();
    if (ImGui::Button(tr("Save")) && S.name[0] && S.txSet) {
        S.ill.push_back({S.name, S.txLat, S.txLon, S.mhz});
        S.cur = (int)S.ill.size() - 1; ch = true;
    }
    if (S.cur >= 0 && S.cur < (int)S.ill.size()) { ImGui::SameLine(); if (ImGui::SmallButton(tr("Delete"))) { S.ill.erase(S.ill.begin() + S.cur); S.cur = -1; ch = true; } }
    if (S.txSet) {
        const LatLon tx{S.txLat, S.txLon}, rx{rlat, rlon};
        ImGui::TextDisabled(tr("Baseline %.1f km, transmitter bearing %.0f degrees"), distanceM(tx, rx) / 1e3, bearingDeg(rx, tx));
    }
    ImGui::TextWrapped("%s", tr("The receiver position is the one set for ADS-B."));
    if (ch) save();
}

void windowPanel(App& a) {
    load();
    firstWindowSize(980 * gUi, 680 * gUi);
    if (!ImGui::Begin(trw("Passive radar"), &a.passiveRadarOpen)) { ImGui::End(); return; }
    bool ch = false, rebuild = !S.radar;
    {
        const char* items[] = {N_("Simulator (demo, no hardware)"), N_("Two-channel recording (cf32)"), N_("Two recordings (reference, surveillance)")};
        ImGui::SetNextItemWidth(280 * gUi);
        if (ImGui::BeginCombo("##prsrc", tr(items[S.src]))) {
            for (int i = 0; i < NumSrc; i++) if (ImGui::Selectable(tr(items[i]), S.src == i)) { S.src = i; rebuild = true; ch = true; }
            ImGui::EndCombo();
        }
        ImGui::SameLine(); ch |= ImGui::Checkbox(tr("Lock to the tuned frequency"), &S.lockTuned);
        ImGui::SameLine();
        if (S.lockTuned) ImGui::Text("%.4f MHz", a.freqMhz);
        else { ImGui::SetNextItemWidth(90 * gUi); if (ImGui::InputFloat("MHz##prmhz", &S.mhz, 0, 0, "%.3f")) { ch = true; rebuild = true; } }
        ImGui::SameLine(); ImGui::Checkbox(tr("Run"), &S.run);
    }
    if (S.src != SrcSim) {
        ImGui::SetNextItemWidth(380 * gUi);
        if (ImGui::InputText(S.src == SrcFiles ? "##prpath1" : "##prpath", S.path, sizeof S.path)) rebuild = true;
        if (S.src == SrcFiles) { ImGui::SetNextItemWidth(380 * gUi); if (ImGui::InputText("##prpath2", S.path2, sizeof S.path2)) rebuild = true; }
        ImGui::SetNextItemWidth(90 * gUi); if (ImGui::InputFloat(tr("kS/s"), &S.rateKhz, 0, 0, "%.0f")) rebuild = true;
        ImGui::TextDisabled("%s", tr("Complex float32, channel 0 reference and channel 1 surveillance, sample-synchronous."));
    }
    if (rebuild) build(a);
    const double now = ImGui::GetTime();
    if (S.run && S.radar && now - S.lastAt >= (double)S.radar->config().caf.cpiSamples() / S.radar->config().caf.sampleRate) { S.lastAt = now; stepOnce(); }
    if (S.sim) {
        bool rb = false;
        const double t = S.sim->time();
        // a simulated target that left the coverage starts again
        for (const SimTarget& tg : S.sim->config().targets) { const double r = tg.rangeM + tg.rateMps * t; if (r < 20e3 || r > 72e3) rb = true; }
        if (rb) build(a);
    }
    const float leftW = 520 * gUi;
    ImGui::BeginChild("##prleft", ImVec2(leftW, 0), false);
    ImGui::TextUnformatted(tr("Range-Doppler map"));
    rangeDoppler(leftW - 8 * gUi, 230 * gUi);
    ImGui::TextUnformatted(tr("Doppler over time"));
    waterfall(leftW - 8 * gUi, 150 * gUi);
    if (S.have) ImGui::TextDisabled(tr("Clutter cancelled by %.0f dB, %d detections"), S.last.cancelDb, (int)S.last.dets.size());
    else ImGui::TextDisabled("%s", tr("Waiting for data."));
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##prright", ImVec2(0, 0), false);
    ImGui::TextUnformatted(tr("Tracks"));
    if (ImGui::BeginTable("##prtracks", 5, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("#"); ImGui::TableSetupColumn(tr("Range km")); ImGui::TableSetupColumn(tr("Rate m/s")); ImGui::TableSetupColumn(tr("Doppler Hz")); ImGui::TableSetupColumn("SNR");
        ImGui::TableHeadersRow();
        const double lam = S.radar ? S.radar->config().caf.lambda() : 3.0;
        for (const Track& t : S.tracks) {
            if (!t.confirmed) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d", t.id);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", t.r / 1e3);
            ImGui::TableNextColumn(); ImGui::Text("%+.0f", t.rdot);
            ImGui::TableNextColumn(); ImGui::Text("%+.1f", -t.rdot / lam);
            ImGui::TableNextColumn(); ImGui::Text("%.0f dB", t.snrDb);
        }
        ImGui::EndTable();
    }
    localMap(ImGui::GetContentRegionAvail().x - 4 * gUi, 220 * gUi);
    if (ImGui::CollapsingHeader(trw("Illuminator"), ImGuiTreeNodeFlags_DefaultOpen)) illuminatorSection();
    if (ImGui::CollapsingHeader(trw("Processing"))) {
        bool pc = false;
        ImGui::SetNextItemWidth(80 * gUi); pc |= ImGui::InputInt(tr("Cancellation taps"), &S.taps); S.taps = std::clamp(S.taps, 4, 32);
        pc |= ImGui::Checkbox(tr("NLMS instead of ECA"), &S.nlms);
        ImGui::SetNextItemWidth(120 * gUi); pc |= ImGui::SliderFloat(tr("False alarm rate 1e-x"), &S.pfaExp, 4, 9, "%.1f");
        if (pc) { rebuild = true; save(); build(a); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Targets closer than the tap count in range samples are hidden by the cancellation."));
    }
    if (S.src == SrcSim && ImGui::CollapsingHeader(trw("Simulator"), ImGuiTreeNodeFlags_DefaultOpen)) {
        bool sc = false;
        ImGui::SetNextItemWidth(160 * gUi); sc |= ImGui::SliderFloat(tr("Target range (km)"), &S.simRangeKm, 25, 65, "%.0f");
        ImGui::SetNextItemWidth(160 * gUi); sc |= ImGui::SliderFloat(tr("Range rate (m/s)"), &S.simRate, -300, 300, "%.0f");
        ImGui::SetNextItemWidth(160 * gUi); sc |= ImGui::SliderFloat(tr("Echo level (dB)"), &S.simLevel, -105, -65, "%.0f");
        sc |= ImGui::Checkbox(tr("Second target"), &S.sim2); ImGui::SameLine(); sc |= ImGui::Checkbox(tr("Noise-like waveform (DVB-T)"), &S.simOfdm);
        if (sc) build(a);
    }
    if (!S.msg.empty()) ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "%s", S.msg.c_str());
    ImGui::EndChild();
    if (ch) save();
    ImGui::End();
}

}   // namespace

void passiveRadarWindow(App& a) {
    if (!a.passiveRadarOpen) return;
    windowPanel(a);
}

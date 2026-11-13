// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The direction finder (View > Direction finder): bearing of a transmitter from a coherent multi-channel receiver (a KrakenSDR style array),
// a live polar plot of the bearing spectrum, history, a calibration wizard, array settings, a local map with the bearing lines and a position
// fix from several bearings. Sources: a built-in simulator (demo, no hardware needed), an interleaved cf32 / SigMF recording, or bearings
// read from a KrakenSDR DoA server JSON file. Nothing here talks to a radio: see docs/direction-finding.md for what is verified.
#include "../app.h"
#include "../ui/typography.h"
#include "../ui/scale.h"
#include "rfcam_ui.h"
#include "../adsb/adsb_map.h"
#include "dect2/df/beam.h"
#include "dect2/df/df.h"
#include "dect2/df/position.h"
#include "dect2/df/source.h"
#include "dect2/df/rtl_group.h"
#include "dect2/geo/maidenhead.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>

namespace {

using namespace dect2::df;

enum SrcKind { SrcSim, SrcFile, SrcJson, SrcHub, NumSrc };

// A KrakenSDR / KerberosSDR hub found on the USB bus (unverified on real hardware). The group lives here; the source below only forwards to it.
dect2::df::Hub gHub;
std::string gHubLabel;
struct HubSource : dect2::df::MultiChannelSource {
    int channels() const override { return gHub.group->channels(); }
    double sampleRate() const override { return gHub.group->sampleRate(); }
    double centerFreq() const override { return gHub.group->centerFreq(); }
    bool read(dect2::df::Frame& o, size_t n) override { return gHub.group->read(o, n); }
};
bool openHub(double hz) {
    gHub = dect2::df::Hub{};
    auto info = dect2::df::findHub(dect2::df::listNativeRtl());
    if (info.empty()) return false;
    std::vector<std::unique_ptr<dect2::df::RtlLink>> links;
    for (auto& d : info) { auto l = dect2::df::makeNativeRtlLink(d.index); if (!l) return false; links.push_back(std::move(l)); }
    dect2::df::HubStart hs; hs.freqHz = hz; hs.rateHz = 1.024e6; hs.gainTenthsDb = 200;
    if (!dect2::df::startHub(std::move(links), hs, gHub)) return false;
    gHubLabel = dect2::df::hubLabel(info);
    return true;
}
enum Geometry { GeoUca, GeoUla, NumGeo };

struct Hist { double t, bearing, conf; };

struct State {
    bool loaded = false;
    int src = SrcSim, geo = GeoUca, method = 2;      // method: 0 Bartlett 1 Capon 2 MUSIC 3 interferometer
    int elements = 5;
    float spacingWl = 0.5f;                          // neighbour spacing in wavelengths (sets the UCA radius / ULA spacing)
    float orientation = 0.f, compass = 0.f;          // array orientation and north offset (degrees)
    bool lockTuned = true, fb = false, run = true;
    float manualMhz = 433.92f;
    // simulator
    float simBearing = 70.f, simSnr = 12.f, sim2Bearing = 200.f;
    bool sim2 = false, simSpin = false, simErrors = true;
    int sources = 1;
    // file / json
    char path[512] = "";
    int fileChannels = 5;
    float fileRateKhz = 250.f;
    // calibration
    bool useCal = true;
    std::vector<cd> cal;
    int calMode = 0;                                 // 0 common noise source, 1 known bearing
    float calBearing = 0.f;
    // results
    Result res;
    BearingAverager avg{12};
    std::vector<Hist> hist;
    double lastUpdate = 0, jsonAt = 0;
    double jsonBearing = 0, jsonConf = 0; bool jsonOk = false;
    std::unique_ptr<MultiChannelSource> source;
    int builtKey = -1;
    // beamformer
    CMat lastR;
    int bfType = 1, bfBeam = 0;
    float bfAz[4] = {0, 90, 180, 270}, bfEl[4] = {0, 0, 0, 0}, bfNullAz = 180.f;
    bool bfNull = false, bfAuto = false;
    // fix
    std::vector<BearingLine> lines;
    Fix fix;
    // sharing
    bool share = false;
    std::string msg;
};
State S;

double tunedHz(const App& a) { return S.lockTuned ? a.freqMhz * 1e6 : (double)S.manualMhz * 1e6; }

Array arrayFor(double hz) {
    const double lam = dect2::df::kC / hz;
    if (S.geo == GeoUca) return Array::uca(S.elements, Array::ucaRadiusForSpacing(S.elements, lam, S.spacingWl), S.orientation);
    return Array::ula(S.elements, S.spacingWl * lam, 90.0 + S.orientation);
}

std::string calString() {
    std::string s;
    char b[64];
    for (const cd& z : S.cal) { snprintf(b, sizeof b, "%.6g,%.6g;", z.real(), z.imag()); s += b; }
    return s;
}
void load() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& p = plat::prefs();
    S.src = (int)p.getI("dfSrc", 0); S.geo = (int)p.getI("dfGeo", 0); S.method = (int)p.getI("dfMethod", 2);
    S.elements = (int)p.getI("dfN", 5); S.spacingWl = (float)p.getD("dfSpacing", 0.5);
    S.orientation = (float)p.getD("dfOrient", 0); S.compass = (float)p.getD("dfCompass", 0);
    S.lockTuned = p.getB("dfLock", true); S.manualMhz = (float)p.getD("dfMhz", 433.92);
    S.share = p.getB("dfShare", false); S.useCal = p.getB("dfUseCal", true);
    S.elements = std::clamp(S.elements, 2, 8);
    std::string c = p.getS("dfCal", "");
    S.cal.clear();
    size_t pos = 0;
    while (pos < c.size()) {
        size_t e = c.find(';', pos);
        if (e == std::string::npos) break;
        double re = 0, im = 0;
        if (sscanf(c.substr(pos, e - pos).c_str(), "%lf,%lf", &re, &im) == 2) S.cal.push_back(cd(re, im));
        pos = e + 1;
    }
    if ((int)S.cal.size() != S.elements) S.cal.clear();
}
void save() {
    plat::Prefs& p = plat::prefs();
    p.setI("dfSrc", S.src); p.setI("dfGeo", S.geo); p.setI("dfMethod", S.method); p.setI("dfN", S.elements);
    p.setD("dfSpacing", S.spacingWl); p.setD("dfOrient", S.orientation); p.setD("dfCompass", S.compass);
    p.setB("dfLock", S.lockTuned); p.setD("dfMhz", S.manualMhz); p.setB("dfShare", S.share); p.setB("dfUseCal", S.useCal);
    p.setS("dfCal", calString());
}

std::vector<cd> simGains() {
    std::vector<cd> g;
    Rng r(2024);
    for (int i = 0; i < S.elements; i++) g.push_back(std::polar(0.8 + 0.4 * r.uni(), 2 * dect2::df::kPi * r.uni()));   // the simulated receiver has unequal chains until calibrated
    return g;
}

void rebuildSource(double hz, bool noise = false) {
    S.source.reset();
    if (S.src == SrcHub) {
        if (!gHub.group || std::abs(gHub.group->centerFreq() - hz) > 1) {
            if (!openHub(hz)) { S.msg = tr("No KrakenSDR / KerberosSDR found (dongles with serials 1000 to 1004), or the RTL-SDR library is missing."); return; }
            S.msg = gHubLabel + tr(": press Calibrate before measuring.");
        }
        S.elements = gHub.group->channels();
        S.source = std::make_unique<HubSource>();
    } else if (S.src == SrcSim) {
        SimConfig c;
        c.array = arrayFor(hz); c.freqHz = hz; c.sampleRate = 250e3; c.seed = (uint64_t)ImGui::GetTime() + 1;
        SimSource a; a.bearingDeg = S.simBearing - S.compass; a.snrDb = S.simSnr; c.sources.push_back(a);
        if (S.sim2) { SimSource b; b.bearingDeg = S.sim2Bearing - S.compass; b.snrDb = S.simSnr - 3; b.offsetHz = 9e3; c.sources.push_back(b); }
        if (rfCamEnabled()) {   // the RF camera shows each transmitter at its own frequency: narrowband messages, apart
            c.sources[0].offsetHz = -35e3; c.sources[0].bandwidthHz = 8e3;
            if (S.sim2) { c.sources[1].offsetHz = 25e3; c.sources[1].bandwidthHz = 8e3; }
        }
        if (S.simErrors) c.elementGains = simGains();
        c.calibrationNoise = noise;
        S.source = std::make_unique<SimMultiChannelSource>(std::move(c));
    } else if (S.src == SrcFile && S.path[0]) {
        auto f = std::make_unique<FileMultiChannelSource>(S.path, S.fileChannels, S.fileRateKhz * 1e3, hz);
        if (f->ok()) S.source = std::move(f); else S.msg = tr("Cannot open the recording (interleaved complex float32, one sample of every channel after the other).");
    }
}

// one estimation step from `n` snapshots; true when a bearing was produced
bool step(double hz, size_t n, CMat* covOut = nullptr) {
    if (!S.source) return false;
    Frame x;
    if (!S.source->read(x, n)) { if (S.src == SrcFile) { rebuildSource(hz); return false; } return false; }
    if ((int)x.size() != S.elements) { S.msg = tr("The recording has a different number of channels than the array."); return false; }
    if (S.useCal && (int)S.cal.size() == S.elements && rfCamEnabled()) {   // the RF camera wants the corrected samples, not just the covariance
        Frame y = x;
        applyCalibration(y, S.cal);
        Options co;
        co.method = (Method)S.method; co.sources = S.sources; co.forwardBackward = S.fb; co.gridStepDeg = 1.0; co.compassOffsetDeg = S.compass;
        rfCamFeed(y, arrayFor(hz), hz, S.source->sampleRate(), co);
    } else if (rfCamEnabled()) {
        Options co;
        co.method = (Method)S.method; co.sources = S.sources; co.forwardBackward = S.fb; co.gridStepDeg = 1.0; co.compassOffsetDeg = S.compass;
        rfCamFeed(x, arrayFor(hz), hz, S.source->sampleRate(), co);
    }
    CMat r = covariance(x);
    if (covOut) *covOut = r;
    if (S.useCal && (int)S.cal.size() == S.elements) r = applyCalibration(r, S.cal);
    Options o;
    o.method = (Method)S.method; o.sources = S.sources; o.forwardBackward = S.fb; o.gridStepDeg = 1.0; o.compassOffsetDeg = S.compass;
    S.lastR = r;
    S.res = estimate(r, arrayFor(hz), dect2::df::kC / hz, o);
    return true;
}

void calibrate(double hz) {
    const bool noise = S.calMode == 0;
    if (S.src == SrcHub) {
        if (!gHub.group) rebuildSource(hz);
        if (!gHub.group || !gHub.noise) { S.msg = tr("Choose a source first."); return; }
        // the DoA hub's noise source is switched through the first dongle's GPIO line (unverified); the group removes the offsets itself
        S.msg = dect2::df::calibrateHub(gHub, *gHub.noise) ? tr("Calibration stored.") : tr("Calibration failed: the channels are not coherent, or the noise source did not switch.");
        S.useCal = false;
        S.avg.clear();
        return;
    }
    if (S.src == SrcSim) { rebuildSource(hz, noise); if (!noise) { /* known-bearing: the simulator's own source acts as the calibrator */ } }
    if (!S.source) { S.msg = tr("Choose a source first."); return; }
    Frame x;
    if (!S.source->read(x, 16384) || (int)x.size() != S.elements) { S.msg = tr("No data to calibrate with."); return; }
    CMat r = covariance(x);
    S.cal = estimateCalibration(r, arrayFor(hz), dect2::df::kC / hz, noise ? NAN : (double)(S.calBearing - S.compass));
    S.useCal = true;
    S.avg.clear();
    if (S.src == SrcSim) rebuildSource(hz);
    save();
    S.msg = tr("Calibration stored.");
}

void pollJson(double now) {
    if (now - S.jsonAt < 1.0) return;
    S.jsonAt = now;
    std::ifstream f(S.path);
    std::string line, last;
    while (f && std::getline(f, line)) if (line.find_first_not_of(" \t\r\n") != std::string::npos) last = line;
    ImportedBearing b = parseKrakenDoaLine(last);
    S.jsonOk = b.ok;
    if (b.ok) {
        S.jsonBearing = wrap360(b.bearingDeg + S.compass); S.jsonConf = b.confidence;
        S.avg.add(S.jsonBearing, std::max(0.05, b.confidence));
        S.hist.push_back({now, S.jsonBearing, b.confidence});
    }
}

void update(App& a) {
    const double now = ImGui::GetTime();
    const double hz = tunedHz(a);
    if (!S.run || now - S.lastUpdate < (rfCamEnabled() ? 0.12 : 0.2)) return;
    S.lastUpdate = now;
    if (S.src == SrcJson) { pollJson(now); return; }
    const int key = S.src * 100000 + S.elements * 1000 + S.geo * 100 + (int)(hz / 1e3) + (int)(S.simBearing * 10) * 7 + (S.sim2 ? 13 : 0) + (int)(S.sim2Bearing * 10) + (int)(S.simSnr * 10) + (S.simErrors ? 5 : 0) + (int)(S.orientation * 3) + (int)(S.compass * 11) + (rfCamEnabled() ? 17 : 0);
    if (S.src == SrcSim && S.simSpin) { S.simBearing = (float)std::fmod(S.simBearing + 3.0, 360.0); }
    if (S.src == SrcSim) {
        // the simulator keeps running; only its scene is rebuilt when a setting changed (spin changes the bearing 5 times a second)
        if (key != S.builtKey || S.simSpin) { S.builtKey = key; rebuildSource(hz); }
    } else if (!S.source || key != S.builtKey) { S.builtKey = key; rebuildSource(hz); }
    if (step(hz, rfCamEnabled() ? 4096 : 2048)) {
        S.avg.add(S.res.bearingDeg, std::max(0.05, S.res.confidence));
        S.hist.push_back({now, S.res.bearingDeg, S.res.confidence});
    }
    while (!S.hist.empty() && now - S.hist.front().t > 60) S.hist.erase(S.hist.begin());
}

const ImU32 kCol = IM_COL32(255, 154, 77, 255);

void polarPlot(float size) {
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##polar", ImVec2(size, size));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p0.x + size / 2, p0.y + size / 2);
    const float R = size / 2 - 22 * gUi;
    dl->AddCircleFilled(c, R + 4, IM_COL32(18, 20, 22, 255), 72);
    for (int i = 1; i <= 3; i++) dl->AddCircle(c, R * i / 3, IM_COL32(70, 74, 78, 255), 72);
    for (int d = 0; d < 360; d += 30) {
        const float th = d * 0.0174533f, s = std::sin(th), co = std::cos(th);
        dl->AddLine(ImVec2(c.x + s * R * 0.92f, c.y - co * R * 0.92f), ImVec2(c.x + s * R, c.y - co * R), IM_COL32(120, 124, 128, 255));
        if (d % 90 == 0) {
            const char* l[] = {"N", "E", "S", "W"};
            const char* t = l[d / 90];
            ImVec2 ts = ImGui::CalcTextSize(t);
            dl->AddText(ImVec2(c.x + s * (R + 12 * gUi) - ts.x / 2, c.y - co * (R + 12 * gUi) - ts.y / 2), IM_COL32(220, 222, 224, 255), t);
        }
    }
    // the history (last minute) as dots on the rim, older = fainter
    const double now = ImGui::GetTime();
    for (const Hist& h : S.hist) {
        const float th = (float)h.bearing * 0.0174533f, age = (float)std::min(1.0, (now - h.t) / 60.0);
        dl->AddCircleFilled(ImVec2(c.x + std::sin(th) * R * 1.0f, c.y - std::cos(th) * R * 1.0f), 2.5f * gUi, IM_COL32(255, 154, 77, (int)(220 * (1 - age) + 20)));
    }
    // the bearing spectrum: radius = relative power
    const auto& sp = S.res.spectrum;
    if (!sp.empty() && S.src != SrcJson) {
        std::vector<ImVec2> pts;
        for (size_t i = 0; i < sp.size(); i++) {
            const float th = (float)(S.res.bearings[i] + S.compass) * 0.0174533f, r = R * (0.05f + 0.95f * (float)sp[i]);
            pts.push_back(ImVec2(c.x + std::sin(th) * r, c.y - std::cos(th) * r));
        }
        dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(110, 200, 255, 255), ImDrawFlags_Closed, 1.6f * gUi);
    }
    if (!S.avg.empty()) {
        const float th = (float)S.avg.mean() * 0.0174533f;
        dl->AddLine(c, ImVec2(c.x + std::sin(th) * R, c.y - std::cos(th) * R), kCol, 2.5f * gUi);
        dl->AddCircleFilled(c, 3 * gUi, kCol);
    }
}

// The beamformer panel: steering controls and the polar pattern of the chosen beam (weights from the live calibrated covariance)
void beamSection(double hz) {
    ImGui::TextWrapped("%s", tr("Steer a receive beam of the array. The chosen beam can feed the receiver as a virtual input (Beam 1 to Beam 4)."));
    ImGui::SetNextItemWidth(60 * gUi); ImGui::SliderInt(tr("Beam"), &S.bfBeam, 0, 3, "%d"); S.bfBeam = std::clamp(S.bfBeam, 0, 3);
    const int b = S.bfBeam;
    const char* ty[] = {N_("Delay and sum"), N_("MVDR (Capon)"), N_("Null steering (LCMV)")};
    ImGui::SetNextItemWidth(190 * gUi);
    if (ImGui::BeginCombo(tr("Beamformer"), tr(ty[S.bfType]))) { for (int i = 0; i < 3; i++) if (ImGui::Selectable(tr(ty[i]), S.bfType == i)) S.bfType = i; ImGui::EndCombo(); }
    ImGui::Checkbox(tr("Follow the strongest MUSIC peak"), &S.bfAuto);
    if (!S.bfAuto) { ImGui::SetNextItemWidth(160 * gUi); ImGui::SliderFloat(tr("Azimuth"), &S.bfAz[b], 0, 359.9f, "%.0f\xC2\xB0"); }
    ImGui::SetNextItemWidth(160 * gUi); ImGui::SliderFloat(tr("Elevation (3D arrays)"), &S.bfEl[b], -10, 90, "%.0f\xC2\xB0");
    if (S.bfType == 2) { ImGui::SetNextItemWidth(160 * gUi); ImGui::SliderFloat(tr("Null toward"), &S.bfNullAz, 0, 359.9f, "%.0f\xC2\xB0"); }
    const int n = S.elements;
    if (S.lastR.n != n) { ImGui::TextDisabled("%s", tr("Waiting for samples.")); return; }
    const Array arr = arrayFor(hz);
    const double lam = dect2::df::kC / hz;
    dect2::df::BeamSpec sp;
    sp.type = (dect2::df::BeamType)S.bfType;
    sp.look = {S.bfAz[b], S.bfEl[b]};
    if (S.bfAuto) { Options o; o.method = Method::Music; o.sources = S.sources; double az; if (dect2::df::strongestPeak(S.lastR, arr, lam, o, az)) sp.look.azDeg = az; }
    if (S.bfType == 2) sp.nulls.push_back({S.bfNullAz, 0});
    const auto w = dect2::df::makeWeights(sp, S.lastR, arr, lam);
    const auto pat = dect2::df::beamPattern(w, arr, lam, sp.look.elDeg, 2.0);
    const float size = 220 * gUi;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##beampolar", ImVec2(size, size));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p0.x + size / 2, p0.y + size / 2);
    const float R = size / 2 - 14 * gUi;
    dl->AddCircleFilled(c, R + 3, IM_COL32(18, 20, 22, 255), 72);
    for (int i = 1; i <= 3; i++) dl->AddCircle(c, R * i / 3, IM_COL32(70, 74, 78, 255), 72);
    std::vector<ImVec2> pts;
    for (size_t i = 0; i < pat.size(); i++) {   // radius: -40 dB at the centre, 0 dB at the rim
        const float th = (float)(i * 2.0 + S.compass) * 0.0174533f, r = R * (float)std::clamp((pat[i] + 40.0) / 40.0, 0.0, 1.0);
        pts.push_back(ImVec2(c.x + std::sin(th) * r, c.y - std::cos(th) * r));
    }
    dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(120, 230, 140, 255), ImDrawFlags_Closed, 1.6f * gUi);
    const float lt = (float)(sp.look.azDeg + S.compass) * 0.0174533f;
    dl->AddLine(c, ImVec2(c.x + std::sin(lt) * R, c.y - std::cos(lt) * R), kCol, 2.f * gUi);
    if (S.bfType == 2) { const float nt = (float)(S.bfNullAz + S.compass) * 0.0174533f; dl->AddLine(c, ImVec2(c.x + std::sin(nt) * R, c.y - std::cos(nt) * R), IM_COL32(255, 90, 90, 255), 1.5f * gUi); }
    ImGui::Text(tr("Look %.0f\xC2\xB0, beamwidth %.0f\xC2\xB0, noise gain %.1f dB"), sp.look.azDeg, dect2::df::beamwidthDeg(w, arr, lam, sp.look), dect2::df::whiteNoiseGainDb(w));
    if (S.bfType == 2) ImGui::Text(tr("Response toward the null: %.0f dB"), dect2::df::nullDepthDb(w, arr, lam, sp.look, sp.nulls[0]));
}

bool gFull = false;   // the window over the whole display (the shared full-screen button; Esc leaves)

// local map: east/north around the receiver, the bearing lines, the captured lines and the fix with its ellipse
void localMap(App& a, float w, float h) {
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##dfmap", ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(18, 20, 22, 255));
    double lat, lon;
    receiverLocation(lat, lon);
    const double mN = 111195.0, mE = 111195.0 * std::cos(lat * dect2::df::kPi / 180);
    double ext = 20000;   // metres from the centre to the edge
    for (const BearingLine& l : S.lines) { ext = std::max(ext, std::fabs((l.lat - lat) * mN) * 1.4); ext = std::max(ext, std::fabs(dect2::df::angDiff(l.lon, lon) * mE) * 1.4); }
    if (S.fix.ok) { ext = std::max(ext, std::fabs((S.fix.lat - lat) * mN) * 1.4); ext = std::max(ext, std::fabs(dect2::df::angDiff(S.fix.lon, lon) * mE) * 1.4); }
    const float s = (float)(std::min(w, h) / 2 / ext);
    const ImVec2 c(p0.x + w / 2, p0.y + h / 2);
    auto xy = [&](double la, double lo) { return ImVec2(c.x + (float)(dect2::df::angDiff(lo, lon) * mE) * s, c.y - (float)((la - lat) * mN) * s); };
    for (int i = 1; i <= 2; i++) dl->AddCircle(c, (float)(ext * i / 2 * s), IM_COL32(60, 64, 68, 255), 48);
    char b[48];
    snprintf(b, sizeof b, "%.0f km", ext / 2000.0);
    dl->AddText(ImVec2(c.x + 4, c.y - (float)(ext / 2 * s) - 14 * gUi), IM_COL32(150, 154, 158, 255), b);
    dl->AddText(ImVec2(p0.x + 4, p0.y + 2), IM_COL32(220, 222, 224, 255), "N");
    auto ray = [&](double la, double lo, double brg, ImU32 col) {
        ImVec2 q = xy(la, lo);
        const float th = (float)brg * 0.0174533f, len = (float)(ext * 2.2 * s);
        dl->AddLine(q, ImVec2(q.x + std::sin(th) * len, q.y - std::cos(th) * len), col, 1.5f * gUi);
        dl->AddCircleFilled(q, 4 * gUi, col);
    };
    for (const BearingLine& l : S.lines) ray(l.lat, l.lon, l.bearingDeg, IM_COL32(150, 150, 255, 255));
    if (!S.avg.empty()) ray(lat, lon, S.avg.mean(), kCol);
    if (S.fix.ok) {
        std::vector<ImVec2> pts;
        const double th = S.fix.e95.orientationDeg * dect2::df::kPi / 180;
        for (int i = 0; i < 48; i++) {
            const double t = 2 * dect2::df::kPi * i / 48, u = S.fix.e95.semiMajorM * std::cos(t), v = S.fix.e95.semiMinorM * std::sin(t);
            const double e = u * std::sin(th) + v * std::cos(th), n = u * std::cos(th) - v * std::sin(th);
            pts.push_back(ImVec2(c.x + (float)((dect2::df::angDiff(S.fix.lon, lon) * mE) + e) * s, c.y - (float)(((S.fix.lat - lat) * mN) + n) * s));
        }
        dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(255, 90, 90, 255), ImDrawFlags_Closed, 1.5f * gUi);
        dl->AddCircleFilled(xy(S.fix.lat, S.fix.lon), 5 * gUi, IM_COL32(255, 90, 90, 255));
    }
    (void)a;
}

void fixSection(App& a) {
    double lat, lon;
    receiverLocation(lat, lon);
    if (ImGui::Button(tr("Add this bearing"), ImVec2(0, 0)) && !S.avg.empty()) {
        BearingLine l; l.lat = lat; l.lon = lon; l.bearingDeg = S.avg.mean();
        l.sigmaDeg = std::max(1.0, std::min(15.0, S.avg.stdDeg() + 2.0));
        S.lines.push_back(l);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Keeps the current bearing and your position as a line. Move, or take bearings from other stations, add more lines and the position is fixed where they cross."));
    ImGui::SameLine();
    if (ImGui::Button(tr("Clear lines"))) { S.lines.clear(); S.fix = Fix(); }
    int del = -1;
    for (size_t i = 0; i < S.lines.size(); i++) {
        BearingLine& l = S.lines[i];
        ImGui::PushID((int)i);
        double dla = l.lat, dlo = l.lon;
        ImGui::SetNextItemWidth(90 * gUi); ImGui::InputDouble("##la", &dla, 0, 0, "%.5f"); ImGui::SameLine();
        ImGui::SetNextItemWidth(90 * gUi); ImGui::InputDouble("##lo", &dlo, 0, 0, "%.5f"); ImGui::SameLine();
        l.lat = dla; l.lon = dlo;
        float br = (float)l.bearingDeg, sg = (float)l.sigmaDeg;
        ImGui::SetNextItemWidth(64 * gUi); if (ImGui::DragFloat("##b", &br, 0.5f, 0, 359.9f, "%.1f\xC2\xB0")) l.bearingDeg = wrap360(br); ImGui::SameLine();
        ImGui::SetNextItemWidth(54 * gUi); if (ImGui::DragFloat("##s", &sg, 0.1f, 0.5f, 30.f, "\xC2\xB1%.1f\xC2\xB0")) l.sigmaDeg = sg; ImGui::SameLine();
        if (ImGui::SmallButton("x")) del = (int)i;
        ImGui::PopID();
    }
    if (del >= 0) S.lines.erase(S.lines.begin() + del);
    S.fix = S.lines.size() >= 2 ? fixPositionRobust(S.lines) : Fix();
    if (S.fix.ok) {
        ImGui::Text("%s  %.5f, %.5f  (%s)", tr("Position"), S.fix.lat, S.fix.lon, dect2::toMaidenhead(S.fix.lat, S.fix.lon, 6).c_str());
        ImGui::Text(tr("95%% ellipse %.1f x %.1f km, %d of %d lines used"), S.fix.e95.semiMajorM / 1000, S.fix.e95.semiMinorM / 1000, S.fix.inliers, (int)S.lines.size());
    } else if (S.lines.size() >= 2) ImGui::TextDisabled("%s", tr("The lines do not cross (parallel, or they meet behind the stations)."));
    (void)a;
}

void drawWindow(App& a) {
    load();
    { static bool af = std::getenv("ONAIR_MAP_FULL") != nullptr; if (af) { af = false; gFull = true; } }   // dev: --shot enters full screen
    if (gFull && !ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) gFull = false;
    if (gFull) { ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize); }
    else firstWindowSize(900 * gUi, 640 * gUi);
    if (!ImGui::Begin(trw("Direction finder"), &a.dfOpen, gFull ? ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse : 0)) { ImGui::End(); return; }
    const double hz = tunedHz(a);
    bool ch = false;
    {   // source and frequency
        const char* items[] = {N_("Simulator (demo, no hardware)"), N_("Recording (cf32 / SigMF)"), N_("KrakenSDR bearing file (JSON)"), N_("KrakenSDR / KerberosSDR (USB)")};
        ImGui::SetNextItemWidth(250 * gUi);
        if (ImGui::BeginCombo("##dfsrc", tr(items[S.src]))) {
            for (int i = 0; i < NumSrc; i++) if (ImGui::Selectable(tr(items[i]), S.src == i)) { S.src = i; S.builtKey = -1; S.source.reset(); S.avg.clear(); S.hist.clear(); ch = true; }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ch |= ImGui::Checkbox(tr("Lock to the tuned frequency"), &S.lockTuned);
        ImGui::SameLine();
        if (S.lockTuned) ImGui::Text("%.4f MHz", a.freqMhz);
        else { ImGui::SetNextItemWidth(90 * gUi); ch |= ImGui::InputFloat("MHz##dfmhz", &S.manualMhz, 0, 0, "%.4f"); }
        ImGui::SameLine();
        ImGui::Checkbox(tr("Run"), &S.run);
    }
    if (S.src == SrcFile || S.src == SrcJson) {
        ImGui::SetNextItemWidth(380 * gUi);
        if (ImGui::InputText("##dfpath", S.path, sizeof S.path)) { S.builtKey = -1; S.source.reset(); }
        ImGui::SameLine(); ImGui::TextDisabled("%s", S.src == SrcFile ? tr("path of the recording") : tr("a file the DoA server's JSON lines are saved to"));
        if (S.src == SrcFile) {
            ImGui::SetNextItemWidth(70 * gUi); if (ImGui::InputInt(tr("channels"), &S.fileChannels)) { S.fileChannels = std::clamp(S.fileChannels, 2, 8); S.elements = S.fileChannels; S.builtKey = -1; ch = true; }
            ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi); ImGui::InputFloat(tr("kS/s"), &S.fileRateKhz, 0, 0, "%.0f");
        }
    }
    update(a);
    const float plotW = 330 * gUi;
    ImGui::BeginChild("##dfleft", ImVec2(plotW, 0), false);
    polarPlot(plotW - 8 * gUi);
    if (!S.avg.empty()) {
        ImGui::Text("%s: %.1f\xC2\xB0   \xC2\xB1%.1f\xC2\xB0", tr("Bearing"), S.avg.mean(), S.avg.stdDeg());
        const double conf = S.src == SrcJson ? S.jsonConf : S.res.confidence;
        ImGui::ProgressBar((float)conf, ImVec2(plotW - 16 * gUi, 0), "");
        ImGui::SameLine(12 * gUi); ImGui::Text("%s %.0f %%", tr("Confidence"), conf * 100);
        if (S.src != SrcJson) ImGui::TextDisabled(tr("Sidelobe %.0f dB, coherence %.2f"), S.res.psldB, S.res.coherence);
    } else ImGui::TextDisabled("%s", tr("Waiting for a bearing."));
    {
        const ImVec2 mp = ImGui::GetCursorScreenPos();
        const ImVec2 ms(plotW - 8 * gUi, gFull ? std::max(200 * gUi, ImGui::GetContentRegionAvail().y - 8 * gUi) : 200 * gUi);
        localMap(a, ms.x, ms.y);
        const ImVec2 keep = ImGui::GetCursorScreenPos();   // the shared full-screen button in the map's corner
        ImGui::SetCursorScreenPos(ImVec2(mp.x + ms.x - ImGui::CalcTextSize(gFull ? tr("Exit full screen") : tr("Full screen")).x - 2 * ImGui::GetStyle().FramePadding.x - 8 * gUi, mp.y + 8 * gUi));
        if (adsbmap::fullButton(gFull, "screen")) gFull = !gFull;
        restoreCursor(keep);
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##dfright", ImVec2(0, 0), false);
    if (S.src != SrcJson) rfCamSection(a);
    if (typo::collapsingHeader(tr("Array and estimator"), ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* geos[] = {N_("Circular (KrakenSDR)"), N_("Linear")};
        ImGui::SetNextItemWidth(170 * gUi);
        if (ImGui::BeginCombo(tr("Array"), tr(geos[S.geo]))) { for (int i = 0; i < NumGeo; i++) if (ImGui::Selectable(tr(geos[i]), S.geo == i)) { S.geo = i; ch = true; } ImGui::EndCombo(); }
        ImGui::SetNextItemWidth(70 * gUi);
        if (ImGui::InputInt(tr("Elements"), &S.elements)) { S.elements = std::clamp(S.elements, 2, 8); S.cal.clear(); ch = true; }
        ImGui::SetNextItemWidth(120 * gUi); ch |= ImGui::SliderFloat(tr("Spacing (wavelengths)"), &S.spacingWl, 0.2f, 0.9f, "%.2f");
        ImGui::SetNextItemWidth(120 * gUi); ch |= ImGui::SliderFloat(tr("Array orientation"), &S.orientation, 0, 359, "%.0f\xC2\xB0");
        ImGui::SetNextItemWidth(120 * gUi); ch |= ImGui::SliderFloat(tr("Compass offset"), &S.compass, -180, 180, "%.0f\xC2\xB0");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Added to every bearing: where the array's own north points relative to true north."));
        const char* ms[] = {"Bartlett", "Capon", "MUSIC", N_("Interferometer")};
        ImGui::SetNextItemWidth(170 * gUi);
        if (ImGui::BeginCombo(tr("Estimator"), tr(ms[S.method]))) { for (int i = 0; i < 4; i++) if (ImGui::Selectable(tr(ms[i]), S.method == i)) { S.method = i; ch = true; } ImGui::EndCombo(); }
        ImGui::SetNextItemWidth(70 * gUi); ImGui::InputInt(tr("Sources"), &S.sources); S.sources = std::clamp(S.sources, 1, std::max(1, S.elements - 1));
        ImGui::Checkbox(tr("Forward-backward averaging (reflections)"), &S.fb);
    }
    if (typo::collapsingHeader(tr("Beamformer"))) beamSection(hz);
    if (typo::collapsingHeader(tr("Calibration"))) {
        ImGui::TextWrapped("%s", tr("1. Feed all channels the same signal: the receiver's noise source (KrakenSDR: switch it on in the DAQ), or a signal from a known bearing. 2. Choose the matching mode and press Calibrate. The offsets between the channels are measured and removed."));
        ImGui::RadioButton(tr("Common noise source"), &S.calMode, 0); ImGui::SameLine(); ImGui::RadioButton(tr("Known bearing"), &S.calMode, 1);
        if (S.calMode == 1) { ImGui::SetNextItemWidth(120 * gUi); ImGui::SliderFloat(tr("Bearing of the source"), &S.calBearing, 0, 359, "%.0f\xC2\xB0"); }
        if (ImGui::Button(tr("Calibrate"))) calibrate(hz);
        ImGui::SameLine(); if (ImGui::Button(tr("Forget"))) { S.cal.clear(); save(); }
        ImGui::SameLine(); ImGui::Checkbox(tr("Use calibration"), &S.useCal);
        for (size_t i = 0; i < S.cal.size(); i++) ImGui::Text("%zu: %.2f x  %+.1f\xC2\xB0", i + 1, std::abs(S.cal[i]), std::arg(S.cal[i]) * 57.29578);
        if (S.cal.empty()) ImGui::TextDisabled("%s", tr("Not calibrated."));
        if (S.src == SrcSim) { ImGui::Checkbox(tr("Simulated channel errors"), &S.simErrors); }
    }
    if (S.src == SrcSim && typo::collapsingHeader(tr("Simulator"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(160 * gUi); ImGui::SliderFloat(tr("Transmitter bearing"), &S.simBearing, 0, 359.9f, "%.0f\xC2\xB0");
        ImGui::SameLine(); ImGui::Checkbox(tr("Spin"), &S.simSpin);
        ImGui::SetNextItemWidth(160 * gUi); ImGui::SliderFloat(tr("SNR per channel"), &S.simSnr, -10, 30, "%.0f dB");
        ImGui::Checkbox(tr("Second transmitter"), &S.sim2);
        if (S.sim2) { ImGui::SetNextItemWidth(160 * gUi); ImGui::SliderFloat(tr("Second bearing"), &S.sim2Bearing, 0, 359.9f, "%.0f\xC2\xB0"); }
    }
    if (typo::collapsingHeader(tr("Position fix"), ImGuiTreeNodeFlags_DefaultOpen)) fixSection(a);
    if (typo::collapsingHeader(tr("Sharing"))) {
        ch |= ImGui::Checkbox(tr("Share my bearing on the map server"), &S.share);
        ImGui::TextWrapped("%s", tr("Needs Server > Send data to map server. Only the bearing, its uncertainty, the frequency and your station position go up; the map shows where bearings from several stations cross."));
    }
    if (!S.msg.empty()) ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "%s", S.msg.c_str());
    ImGui::EndChild();
    if (ch) save();
    ImGui::End();
}

}   // namespace

void dfWindow(App& a) {
    if (!a.dfOpen) return;
    drawWindow(a);
}

bool dfMapLine(const App& a, DfMapLine& out) {
    load();
    if (!S.share || S.avg.empty() || !receiverLocationSet()) return false;
    const double now = ImGui::GetTime();
    if (S.hist.empty() || now - S.hist.back().t > 30) return false;
    const double conf = S.src == SrcJson ? S.jsonConf : S.res.confidence;
    if (conf < 0.3) return false;
    receiverLocation(out.lat, out.lon);
    out.bearing = S.avg.mean();
    out.sigma = std::max(1.0, S.avg.stdDeg() + 2.0);
    out.conf = conf;
    out.freqHz = tunedHz(a);
    out.ageSec = now - S.hist.back().t;
    return true;
}

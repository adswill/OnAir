// QO-100 screens: the LNB oscillator (9750, 9739, 10000, 10600 MHz or custom) with the IF of the downlink worked out, tuning to the whole
// downlink or the narrowband transponder, the beacon lock (LNB error in kHz and ppm, drift), the narrowband transponder as a zoomable
// spectrum over a waterfall (a click puts the SSB dial there) with Listen, the wideband transponder with its DATV carriers and a
// hand-over to DVB-S/S2, and the drift history. Every frequency shown is the corrected downlink frequency once the beacons are locked.
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "ease/ease_ui.h"
#include "state/results_export.h"
#include "panels/gallery.h"
#include "dvbs/dvbs_autoservice.h"
#include "i18n/i18n.h"
#include "dect2/qo100/qo100_auto.h"
#include "dect2/hfdig/hfdig_sstv.h"
#include "gfx/gfx.h"
#include "gfx/plot.h"
#include "dect2/qo100/qo100_dsp.h"
#include "dect2/qo100/qo100_gen.h"
#include "dect2/qo100/qo100_rx.h"
#include "dect2/qo100/qo100_tel.h"
#include "dect2/specan/lnb_cal.h"
#include "dect2/specx/specx_measure.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace {

namespace q = dect2::qo100;

struct WfImage {
    gfx::Image* img = nullptr;
    int W = 1024, H = 220;
    std::vector<uint32_t> px;
    bool dirty = false;
    void push(const std::vector<float>& db, float lo, float hi) {
        if (px.empty()) px.assign((size_t)W * H, 0xFF000000u);
        std::copy_backward(px.begin(), px.end() - W, px.end());
        const size_t n = db.size();
        for (int x = 0; x < W && n; x++) {
            size_t a = (size_t)x * n / W, b = std::max(a + 1, (size_t)(x + 1) * n / W);
            float m = -200;
            for (size_t k = a; k < b && k < n; k++) m = std::max(m, db[k]);
            px[(size_t)x] = Waterfall::jet(std::clamp((m - lo) / std::max(1.f, hi - lo), 0.f, 1.f));
        }
        dirty = true;
    }
    void draw(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float u0, float u1) {
        if (!gGfx) return;
        if (!img) img = gGfx->createImage(W, H, 0xFF000000u);
        if (img && dirty) { img->update(0, 0, W, H, px.data()); dirty = false; }
        if (img) dl->AddImage(img->texture(), p0, p1, ImVec2(u0, 0), ImVec2(u1, 1));
    }
};

struct State {
    bool loaded = false;
    int loPreset = 0;               // loPresets index, or the count = custom
    double customLoMhz = 9750;
    bool autoCorr = true;           // correct the frequencies by the LNB error measured on the beacons
    double lnbPpm = 0;              // LNB error from the beacon calibration (WIP), applied to the LO (prefs qo100LnbPpm)
    double calBeaconMhz = 10489.750, calIfMhz = 0;   // LNB calibration inputs: the known beacon and its measured IF peak
    double calLastT = -1e9;         // time of the last drift sample (seconds)
    dect2::LnbDriftTracker drift;   // slow filter over the total LNB error seen on the beacons
    double dialHz = 10489.600e6;
    bool listen = false;
    int view = 0;                   // 0 narrowband, 1 wideband, 2 drift
    double viewLo = q::kNbLoHz - 50e3, viewHi = q::kNbHiHz + 50e3;
    double sliceLo = 0, sliceHi = 0;   // the narrowband spectrum's range (corrected RF)
    float lo = -110, hi = -40, wlo = -110, whi = -40, peakHold = -200, wPeakHold = -200;
    uint64_t lastSeq = 0;
    WfImage wf;
    bool dragging = false, dragMoved = false;
    float dragX = 0;
    double dragV0 = 0, dragV1 = 0;
    char dialBuf[40] = "";
    // the auto decoder and the conversation manager
    unsigned autoMask = dect2::kQoDecSsb;
    int maxSlots = 6;
    float cpuLimit = 0.7f;
    int autoListen = 0;             // 0 off, 1 loudest, 2 newest
    bool squelch = true;
    int listenConv = 0;
    std::set<int> rec;              // conversations to record
    std::set<std::string> published;   // SSTV pictures already in the gallery
    std::string note;
    // the DVB-S/S2 decoder under the wideband view (Engine::setSecondaryDvbs): the carrier clicked, its report, why it was refused
    bool dvbsOn = false, autoPlay = false;
    dect2::AutoServiceState autoSvc;
    dect2::Qo100Datv dvbsCarrier;
    RxTelemetry rx2;
    uint64_t rx2Seq = 0;
    std::string dvbsRefused;
};
State S;

void tip(const char* t) { if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", t); }
bool live(const App& a) { return a.rx.standard == 54; }   // the engine reports its standard code minus one
const dect2::Qo100Telemetry& T(const App& a) { return a.rx.qo100; }

double nominalLoHz() {
    int n = 0; const q::LoPreset* p = q::loPresets(n);
    return S.loPreset >= 0 && S.loPreset < n ? p[S.loPreset].loHz : S.customLoMhz * 1e6;
}
// the LO the radio is set to: the nominal oscillator corrected by the calibrated LNB error
double loHz() { return dect2::lnbCorrectedLoHz(nominalLoHz(), S.lnbPpm); }
std::string mhz(double hz, int dec = 3) { char b[40]; snprintf(b, sizeof b, "%.*f MHz", dec, hz / 1e6); return b; }

void load() {
    if (S.loaded) return;
    S.loaded = true;
    auto& p = plat::prefs();
    int n = 0; q::loPresets(n);
    S.loPreset = (int)std::clamp(p.getI("qo100LoPreset", 0), 0L, (long)n);
    S.customLoMhz = p.getD("qo100LoCustom", 9750);
    S.dialHz = p.getD("qo100Dial", 10489.600e6);
    S.autoCorr = p.getB("qo100AutoCorr", true);
    S.lnbPpm = p.getD("qo100LnbPpm", 0);
    S.calBeaconMhz = p.getD("qo100CalBeaconMhz", 10489.750);
    S.listen = p.getB("qo100Listen", false);
    S.view = (int)std::clamp(p.getI("qo100View", 0), 0L, 4L);
    S.autoMask = (unsigned)p.getI("qo100AutoMask", dect2::kQoDecSsb);
    S.maxSlots = (int)std::clamp(p.getI("qo100Slots", 6), 1L, 12L);
    S.cpuLimit = (float)std::clamp(p.getD("qo100Cpu", 0.7), 0.1, 4.0);
    S.autoListen = (int)std::clamp(p.getI("qo100AutoListen", 0), 0L, 2L);
    S.squelch = p.getB("qo100Squelch", true);
}
void save() {
    auto& p = plat::prefs();
    p.setI("qo100LoPreset", S.loPreset); p.setD("qo100LoCustom", S.customLoMhz); p.setD("qo100Dial", S.dialHz); p.setI("qo100View", S.view);
    p.setB("qo100AutoCorr", S.autoCorr);
    p.setD("qo100LnbPpm", S.lnbPpm); p.setD("qo100CalBeaconMhz", S.calBeaconMhz);
    p.setI("qo100AutoMask", (long)S.autoMask); p.setI("qo100Slots", S.maxSlots); p.setD("qo100Cpu", S.cpuLimit);
    p.setI("qo100AutoListen", S.autoListen); p.setB("qo100Squelch", S.squelch);
}

void setDial(double hz) { S.dialHz = std::round(hz / 10) * 10; S.listenConv = 0; S.autoListen = 0; save(); }   // a dial set by hand ends the following

void tuneRf(App& a, double rfHz) { tuneFreq(a, q::ifOf(rfHz, loHz()) / 1e6); }

// The carrier decoded as the engine's second mode, its picture and status under the wideband view: the spectrum and the beacons stay live
void decodeBelow(App& a, const dect2::Qo100Datv& d) {
    setSecondMode(a, -1, 0);   // there is one second mode: one chosen in the toolbar makes room
    S.dvbsCarrier = d;
    S.dvbsOn = true;
    S.rx2 = RxTelemetry(); S.rx2Seq = 0;
    S.autoPlay = true;
    S.dvbsRefused.clear();
    if (!a.engine.setSecondaryDvbs(d.ifHz, std::round(d.symRate / 1e3) * 1e3)) S.dvbsRefused = a.engine.secondaryNote();
}

void closeDecode(App& a) {
    if (a.engine.secondaryStandard() == 8) a.engine.clearSecondary();   // the engine ends its stream and picture with it
    S.dvbsOn = false; S.dvbsRefused.clear();
    S.rx2 = RxTelemetry(); S.rx2Seq = 0;
}

bool decodingCarrier(const App& a, const dect2::Qo100Datv& d) {
    return S.dvbsOn && a.engine.secondaryStandard() == 8 && std::fabs(d.ifHz - S.dvbsCarrier.ifHz) < 0.6 * std::max(d.bw3Hz, 50e3);
}

// LNB calibration from a known beacon (WIP): the beacon's measured IF gives the oscillator error; Apply stores it and the LO follows
void lnbCalSection(const App& a) {
    const auto& t = T(a);
    if (live(a) && t.corrValid) {   // slow drift of the total LNB error, one sample per 10 s
        const double now = ImGui::GetTime();
        if (now - S.calLastT >= 10.0) { S.calLastT = now; S.drift.add(now, S.lnbPpm + t.lnbErrPpm); }
    }
    ImGui::TextDisabled("%s", "LNB calibration (WIP)");
    tip("Enter the beacon's known frequency and the IF peak measured on it (fine FFT peak). Apply stores the LNB error and corrects the LO for QO-100 tuning and presets.");
    flowNext(16 * gUi);
    ImGui::SetNextItemWidth(120 * gUi);
    if (ImGui::InputDouble("##calbeacon", &S.calBeaconMhz, 0, 0, "%.3f MHz")) { S.calBeaconMhz = std::clamp(S.calBeaconMhz, 10000.0, 12750.0); save(); }
    tip("The beacon's known frequency at the feed (QO-100 beacons: 10489.500 and 10490.000 MHz)");
    flowNext(6 * gUi);
    ImGui::SetNextItemWidth(120 * gUi);
    ImGui::InputDouble("##calif", &S.calIfMhz, 0, 0, "%.4f MHz");
    tip("The beacon's IF peak as the radio shows it (fine FFT peak with parabolic interpolation)");
    flowNext(10 * gUi);
    if (S.calIfMhz > 0) {
        const double nominal = nominalLoHz();
        const dect2::LnbError e = dect2::lnbErrorFromBeacon(S.calBeaconMhz * 1e6, nominal, S.calIfMhz * 1e6, q::invertedLo(nominal));
        ImGui::Text("Offset %+.1f Hz (%+.3f ppm)", e.hz, e.ppm);
        ImGui::SameLine();
        if (ImGui::Button("Apply")) { S.lnbPpm = e.ppm; save(); }
        tip("Stores this LNB error and sets the LO to nominal x (1 + error) for QO-100 tuning and its presets");
    } else ImGui::TextDisabled("%s", "Enter the measured beacon IF");
    flowNext(10 * gUi);
    ImGui::TextDisabled("Applied %+.3f ppm", S.lnbPpm);
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) { S.lnbPpm = 0; save(); }
    if (S.drift.valid()) {
        ImGui::SameLine();
        ImGui::TextDisabled("drift %+.4f ppm/h (%d samples)", S.drift.ppmPerHour(), S.drift.samples());
    }
}

// LNB, IF and where the radio is
void strip(App& a) {
    int n = 0; const q::LoPreset* pr = q::loPresets(n);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("LNB LO"); flowNext(4 * gUi);
    ImGui::SetNextItemWidth(250 * gUi);
    if (ImGui::BeginCombo("##qolo", S.loPreset < n ? pr[S.loPreset].name : "Custom")) {
        for (int i = 0; i <= n; i++) if (ImGui::Selectable(i < n ? pr[i].name : "Custom", S.loPreset == i)) { S.loPreset = i; save(); }
        ImGui::EndCombo();
    }
    tip("The LNB's local oscillator: 9750 MHz for a standard (Bullseye, Octagon) Ku LNB; an LO above 10489.5 MHz mirrors the spectrum (the tool turns it back)");
    if (S.loPreset >= n) {
        flowNext(4 * gUi);
        ImGui::SetNextItemWidth(100 * gUi);
        if (ImGui::InputDouble("##qolocus", &S.customLoMhz, 0, 0, "%.3f MHz", ImGuiInputTextFlags_EnterReturnsTrue)) { S.customLoMhz = std::clamp(S.customLoMhz, 1.0, 20000.0); save(); }
        tip("The oscillator frequency of your LNB or converter");
    }
    flowNext(10 * gUi);
    if (ImGui::Checkbox("Auto frequency correction", &S.autoCorr)) save();
    tip("On: the LNB's frequency error is measured on the CW beacons and every frequency shown is corrected by it.\nOff: no correction (for a GPS-locked LNB). The beacons are still measured and the error is shown.");
    flowBreak();
    lnbCalSection(a);
    flowNext(10 * gUi);
    const double lo = loHz();
    double i0 = q::ifOf(q::kDownLoHz, lo), i1 = q::ifOf(q::kDownHiHz, lo);
    if (i0 > i1) std::swap(i0, i1);
    flowNext(10 * gUi);
    ImGui::TextDisabled("downlink 10489.500 - 10500.000 MHz = IF %.3f - %.3f MHz%s", i0 / 1e6, i1 / 1e6, q::invertedLo(lo) ? " (inverted)" : "");
    flowBreak();
    if (ImGui::Button("Whole downlink")) tuneRf(a, q::kWholeCentreHz);
    tip("Tune the radio to 10494.0 MHz (both beacons and the DATV channels): needs 10 Msps");
    flowNext(6 * gUi);
    if (ImGui::Button("Narrowband only")) tuneRf(a, q::kNarrowCentreHz);
    tip("Tune the radio to 10490.1 MHz: the narrowband transponder and its beacons, 2 Msps is enough");
    flowNext(10 * gUi);
    const double radioRf = q::rfOf(a.tune.centerHz, lo);
    ImGui::TextDisabled("radio %.3f MHz = %s", a.tune.centerHz / 1e6, mhz(radioRf).c_str());
    flowNext(10 * gUi);
    sampleRateControl(a);
    flowEnd();
}

void lockLine(const App& a) {
    const auto& t = T(a);
    const int st = live(a) ? t.state : -1;
    lamp("Beacons", st == 2 ? 1 : st == 3 ? 2 : st == 1 ? 3 : 0, (int)Ic::Lock); flowNext(10 * gUi);
    if (!live(a)) ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "Start the receiver to lock onto the beacons");
    else if (st == 0) ImGui::TextColored(pal::warnAmber(), "the narrowband transponder is outside the radio's band: tune to it (Narrowband only) or raise the sample rate");
    else if (!t.corrValid) ImGui::TextDisabled("searching for the CW beacons (10489.500 and 10490.000 MHz)");
    else {
        ImGui::Text("LNB error %+.3f kHz (%+.3f ppm)%s", t.lnbErrHz / 1e3, t.lnbErrPpm, t.corrApplied ? "" : ", not applied");
        tip("The LNB's oscillator is this far from nominal (and the radio's own error with it): measured on the CW beacons, every frequency here is corrected by it");
        flowNext(10 * gUi);
        if (t.driftValid) ImGui::Text("drift %+.0f Hz/min", t.driftHzPerMin); else ImGui::TextDisabled("drift: measuring");
        flowNext(10 * gUi);
        ImGui::TextDisabled("%s, %llu measurements", st == 2 ? "locked" : "held (beacons lost)", (unsigned long long)t.measurements);
        for (int i = 0; i < 3; i++) {
            flowNext(10 * gUi);
            static const char* const kN[] = {"low CW", "PSK", "high CW"};
            const auto& b = t.beacons[i];
            if (b.found) ImGui::Text("%s %.0f dB", kN[i], b.snrDb); else ImGui::TextDisabled("%s -", kN[i]);
        }
    }
    flowEnd();
}

void gestures(float l, float r, bool hov, double& lo, double& hi, double minSpan, double maxLo, double maxHi, bool clickDial) {
    ImGuiIO& io = ImGui::GetIO();
    const double span = hi - lo;
    auto fAt = [&](float x) { return lo + (x - l) / std::max(1.f, r - l) * span; };
    if (hov) {
        const double f = fAt(io.MousePos.x);
        ImGui::SetTooltip("%s%s", mhz(f, 6).c_str(), clickDial ? "\nclick: SSB dial here, wheel: zoom, drag: pan" : "");
        if (io.MouseWheel != 0) {
            const double ns = std::clamp(span * std::pow(0.8, io.MouseWheel), minSpan, maxHi - maxLo), fr = (f - lo) / span;
            lo = f - fr * ns; hi = lo + ns;
        }
        if (ImGui::IsMouseClicked(0)) { S.dragging = true; S.dragMoved = false; S.dragX = io.MousePos.x; S.dragV0 = lo; S.dragV1 = hi; }
    }
    if (S.dragging) {
        if (std::fabs(io.MousePos.x - S.dragX) > 3) S.dragMoved = true;
        if (S.dragMoved) { const double d = (io.MousePos.x - S.dragX) / std::max(1.f, r - l) * (S.dragV1 - S.dragV0); lo = S.dragV0 - d; hi = S.dragV1 - d; }
        if (!ImGui::IsMouseDown(0)) { if (!S.dragMoved && clickDial) setDial(fAt(S.dragX)); S.dragging = false; }
    }
    const double s = hi - lo;
    if (lo < maxLo) { lo = maxLo; hi = lo + s; }
    if (hi > maxHi) { hi = maxHi; lo = std::max(maxLo, hi - s); }
}

void narrowView(App& a, float h) {
    const auto& t = T(a);
    const bool on = live(a) && !t.nb.empty();
    std::vector<double> xs, ys;
    if (on) { xs.resize(t.nb.size()); ys.resize(t.nb.size()); for (size_t k = 0; k < t.nb.size(); k++) { xs[k] = (t.nbF0Hz + k * t.nbBinHz) / 1e6; ys[k] = t.nb[k]; } }
    const float sh = std::max(80.f, h * 0.55f), wh = std::max(40.f, h - sh - ImGui::GetStyle().ItemSpacing.y);
    float l = 0, r = 0, top = 0, bot = 0; bool hov = false;
    if (plt::BeginPlot("##qonb", ImVec2(-1, sh), plt::Flags_NoLegend | plt::Flags_NoTitle | plt::Flags_NoMouseText)) {
        plt::SetupAxes("MHz", "dBFS");
        plt::SetupAxisLimits(plt::X1, S.viewLo / 1e6, S.viewHi / 1e6, plt::Cond_Always);
        plt::SetupAxisLimits(plt::Y1, S.lo, S.hi, plt::Cond_Always);
        plt::SetupAxisFormat(plt::X1, "%.3f");
        { plt::Spec s; s.FillColor = ImVec4(0.4f, 0.8f, 0.5f, S.listen ? 0.18f : 0.08f); plt::PlotVBand("USB", (S.dialHz + 300) / 1e6, (S.dialHz + 3000) / 1e6, s); }
        if (!ys.empty()) { plt::Spec s; s.LineColor = pal::accent(); plt::PlotLine("nb", xs.data(), ys.data(), (int)ys.size(), s); }
        const double bx[3] = {q::kLowBeaconHz / 1e6, q::kPskBeaconHz / 1e6, q::kHighBeaconHz / 1e6};
        { plt::Spec s; s.LineColor = ImVec4(1.f, 0.82f, 0.35f, 0.35f); plt::PlotInfLines("beacons", bx, 3, s); }
        for (const auto& k : t.autoDec.tracks) {
            if (!k.active || k.cls == dect2::kQoUnknown || k.cls == dect2::kQoBeacon) continue;
            plt::PlotText(dect2::qo100ClassName(k.cls), k.rfHz / 1e6, S.hi - 6 - 0.0f * k.id);
        }
        const double dx = S.dialHz / 1e6;
        { plt::Spec s; s.LineColor = ImVec4(0.4f, 0.95f, 0.5f, 0.8f); plt::PlotInfLines("dial", &dx, 1, s); }
        if (!on) plt::PlotText(a.engine.running() ? "waiting for the narrowband transponder" : "Start the receiver", (S.viewLo + S.viewHi) / 2e6, (S.lo + S.hi) / 2);
        const ImVec2 p = plt::GetPlotPos(), sz = plt::GetPlotSize();
        l = p.x; r = p.x + sz.x; top = p.y; bot = p.y + sz.y;
        hov = plt::IsPlotHovered();
        plt::EndPlot();
    }
    (void)top; (void)bot;
    const double minLo = q::kNbLoHz - 50e3, maxHi = q::kNbHiHz + 50e3;
    if (r > l) gestures(l, r, hov, S.viewLo, S.viewHi, 3e3, minLo, maxHi, true);
    // waterfall: the image covers the published slice
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + wh), IM_COL32(8, 9, 10, 255));
    const float x0 = l > 0 ? l : p0.x, x1 = r > l ? r : p0.x + w;
    if (S.sliceHi > S.sliceLo) {
        const double sp = S.sliceHi - S.sliceLo;
        S.wf.draw(dl, ImVec2(x0, p0.y), ImVec2(x1, p0.y + wh), (float)((S.viewLo - S.sliceLo) / sp), (float)((S.viewHi - S.sliceLo) / sp));
    }
    const float dxp = (float)(x0 + (S.dialHz - S.viewLo) / (S.viewHi - S.viewLo) * (x1 - x0));
    if (dxp >= x0 && dxp <= x1) dl->AddLine(ImVec2(dxp, p0.y), ImVec2(dxp, p0.y + wh), IM_COL32(100, 240, 130, 150));
    ImGui::InvisibleButton("##qowf", ImVec2(w, wh));
    const bool whov = ImGui::IsItemHovered();
    if (whov) ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    gestures(x0, x1, whov, S.viewLo, S.viewHi, 3e3, minLo, maxHi, true);
}

void wideView(App& a, float h) {
    const auto& t = T(a);
    const bool on = live(a) && !t.wb.empty();
    std::vector<double> xs, ys;
    if (on) { xs.resize(t.wb.size()); ys.resize(t.wb.size()); for (size_t k = 0; k < t.wb.size(); k++) { xs[k] = (t.wbF0Hz + k * t.wbBinHz) / 1e6; ys[k] = t.wb[k]; } }
    const float tw = std::min(ImGui::GetContentRegionAvail().x * 0.45f, 460 * gUi);
    const float pw = ImGui::GetContentRegionAvail().x - tw - 8 * gUi;
    if (plt::BeginPlot("##qowb", ImVec2(pw, h), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("MHz", "dBFS");
        const double x0 = on ? xs.front() : q::kDownLoHz / 1e6, x1 = on ? xs.back() : q::kDownHiHz / 1e6;
        plt::SetupAxisLimits(plt::X1, x0, x1, plt::Cond_Always);
        plt::SetupAxisLimits(plt::Y1, S.wlo, S.whi, plt::Cond_Always);
        plt::SetupAxisFormat(plt::X1, "%.2f");
        { plt::Spec s; s.FillColor = ImVec4(0.5f, 0.6f, 1.f, 0.07f); plt::PlotVBand("wb", q::kWbLoHz / 1e6, q::kWbHiHz / 1e6, s); }
        { plt::Spec s; s.FillColor = ImVec4(1.f, 0.8f, 0.4f, 0.07f); plt::PlotVBand("nb", q::kNbLoHz / 1e6, q::kNbHiHz / 1e6, s); }
        if (!ys.empty()) { plt::Spec s; s.LineColor = pal::accent(); plt::PlotLine("wb", xs.data(), ys.data(), (int)ys.size(), s); }
        for (const auto& d : t.datv) {
            char b[48]; snprintf(b, sizeof b, "%.0f ks", d.symRate / 1e3);
            plt::PlotText(b, d.rfHz / 1e6, S.whi - 4);
        }
        if (!on) plt::PlotText(a.engine.running() ? "starting" : "Start the receiver", (x0 + x1) / 2, (S.wlo + S.whi) / 2);
        plt::EndPlot();
    }
    ImGui::SameLine();
    ImGui::BeginChild("##qodatv", ImVec2(tw, h), false);
    if (!on) ImGui::TextDisabled("no spectrum yet");
    else if (!t.wbOk) ImGui::TextDisabled("the wideband transponder (10491 - 10499 MHz) is outside the radio's band: Whole downlink, 10 Msps");
    else if (t.datv.empty()) ImGui::TextDisabled("no DATV carrier seen");
    else if (ImGui::BeginTable("##qodt", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Downlink"); ImGui::TableSetupColumn("Symbol rate"); ImGui::TableSetupColumn("SNR"); ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        int i = 0;
        for (const auto& d : t.datv) {
            ImGui::PushID(i++);
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            ImGui::Text("%.3f%s", d.rfHz / 1e6, d.beacon ? " beacon" : "");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("downlink %s, at the radio %s%s\n-3 dB width %.0f kHz", mhz(d.rfHz).c_str(), mhz(d.ifHz).c_str(), t.inverted ? " (inverted spectrum)" : "", d.bw3Hz / 1e3);
            ImGui::TableNextColumn(); ImGui::Text("%.0f ks/s", d.symRate / 1e3);
            ImGui::TableNextColumn(); ImGui::Text("%.1f dB", d.snrDb);
            ImGui::TableNextColumn();
            if (d.looksDvbs) {
                const bool cur = decodingCarrier(a, d);
                if (cur) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.45f));
                if (ImGui::SmallButton("Decode with DVB-S/S2")) decodeBelow(a, d);
                if (cur) ImGui::PopStyleColor();
                tip("Decode this carrier with DVB-S/S2 at its frequency and symbol rate: its picture and status appear below, "
                    "the spectrum and the beacons stay live (the radio keeps its tuning)");
            } else ImGui::TextDisabled("-");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (on && !t.datv.empty()) {
        static std::string note;
        resexp::Table x; x.cols = {"time", "frequency_hz", "mode", "beacon", "symbol_rate_sps", "snr_db", "bandwidth_3db_hz", "dvbs_like"};
        const std::string now = resexp::stamp();
        for (const auto& d : t.datv) x.add({now, resexp::num(d.rfHz, 0), "QO-100", d.beacon ? "yes" : "no", resexp::num(d.symRate, 0), resexp::num(d.snrDb, 1), resexp::num(d.bw3Hz, 0), d.looksDvbs ? "yes" : "no"});
        resexp::saveButton(x, "qo100", note, "svqo");
        if (!note.empty()) ImGui::TextDisabled("%s", note.c_str());
    }
    ImGui::EndChild();
}

// Under the wideband view: the DVB-S/S2 decoder of the clicked carrier. The lamps and readouts are the DVB-S mode's own (dvbsStatusRows); the
// services of the stream and the picture are the engine's player, as the TV tab shows them
void decodePanel(App& a, float h) {
    ImGui::BeginChild("##qods", ImVec2(0, h), ImGuiChildFlags_Borders);
    const dect2::Qo100Datv& c = S.dvbsCarrier;
    const bool asked = a.engine.secondaryStandard() == 8, run = a.engine.running();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(pal::accent(), "DVB-S/S2");
    flowNext(6 * gUi);
    ImGui::Text("%.3f MHz  %.0f ks/s", c.rfHz / 1e6, std::round(c.symRate / 1e3));
    flowNext(10 * gUi);
    const bool close = ImGui::SmallButton("Close");
    tip("Stop decoding this carrier");
    flowEnd();
    if (close) { closeDecode(a); ImGui::EndChild(); return; }
    if (!asked) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(pal::warnAmber(), "%s", S.dvbsRefused.empty() ? "not running" : S.dvbsRefused.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        return;
    }
    if (!a.engine.secondaryRunning()) {
        const std::string note = a.engine.secondaryNote();
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(pal::warnAmber(), "%s", note.empty() ? "not running" : note.c_str());
        ImGui::PopTextWrapPos();
    }
    dvbsStatusRows(a, S.rx2.dvbs, S.rx2.standard == 7, run);
    flowEnd();
    ImGui::Spacing();

    // the services of the stream (left) and the picture (right)
    Player& pl = a.engine.player();
    const PlayerStats ps = pl.stats();
    const float bodyH = std::max(60.f * gUi, ImGui::GetContentRegionAvail().y), lw = std::min(ImGui::GetContentRegionAvail().x * 0.42f, 440.f * gUi);
    ImGui::BeginChild("##qodsl", ImVec2(lw, bodyH));
    if (a.ts.services.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", S.rx2.dvbs.tsLock ? "waiting for the service tables (PAT / SDT)..." : "no transport stream yet");
        ImGui::PopTextWrapPos();
    }
    for (const TsService& sv : a.ts.services) {
        std::string tags;
        double kb = 0;
        for (const auto& st : sv.streams) { kb += st.kbps; if (!st.codec.empty() && (st.kind == "video" || st.kind == "audio")) tags += (tags.empty() ? "" : "  ") + st.codec; }
        char b[200];
        snprintf(b, sizeof b, "%s%s  %s  %.0f kbit/s###qos%d", sv.name.empty() ? "service " : "", sv.name.empty() ? std::to_string(sv.id).c_str() : sv.name.c_str(),
                 tags.c_str(), kb, sv.id);
        if (ImGui::Selectable(b, sv.id == pl.selected())) {
            if (sv.id == pl.selected()) pl.select(-1); else { pl.select(sv.id); pl.setVolume(a.volume); }
            S.autoPlay = false;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s%s\nclick: play or stop", sv.provider.c_str(), sv.provider.empty() ? "" : "\n", sv.scrambled ? "scrambled" : sv.caState == CaState::CaFlagClear ? "CA flag set, stream clear" : "free to air");
    }
    if (pl.selected() >= 0) {
        if (ps.hasVideo) ImGui::TextDisabled("%s  %dx%d  %.1f fps", ps.videoCodec.c_str(), ps.width, ps.height, ps.fps);
        if (ps.hasAudio) ImGui::TextDisabled("%s  %d ch", ps.audioCodec.c_str(), ps.audioChannels);
        ImGui::TextDisabled("%llu pictures shown, %llu errors", (unsigned long long)ps.shown, (unsigned long long)ps.errors);
        ImGui::SetNextItemWidth(130 * gUi);
        if (ImGui::SliderFloat("volume", &a.volume, 0, 1, "%.2f")) pl.setVolume(a.volume);
        ImGui::SameLine();
        if (ImGui::Checkbox("mute", &a.muted)) pl.setMuted(a.muted);
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##qodsv", ImVec2(0, bodyH), ImGuiChildFlags_Borders);
    if (pl.selected() < 0) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Pick a service to play it."); ImGui::PopTextWrapPos(); }
    else if (!ps.videoNote.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", ps.videoNote.c_str()); ImGui::PopTextWrapPos(); }
    else if (!a.video.has()) ImGui::TextDisabled("%s", ps.hasAudio && !ps.hasVideo ? "audio only" : ps.status.c_str());
    else a.video.draw(ImGui::GetContentRegionAvail());
    ImGui::EndChild();
    ImGui::EndChild();
}

void driftView(App& a, float h) {
    const auto& t = T(a);
    if (!live(a) || t.histT.empty()) { ImGui::TextDisabled("no measurement yet: the history starts when the beacons lock"); return; }
    std::vector<double> xs(t.histT.begin(), t.histT.end()), ys(t.histErr.size());
    for (size_t i = 0; i < ys.size(); i++) ys[i] = t.histErr[i] / 1e3;
    auto mm = std::minmax_element(ys.begin(), ys.end());
    const double pad = std::max(0.02, (*mm.second - *mm.first) * 0.15);
    if (plt::BeginPlot("##qodrift", ImVec2(-1, h), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("s", "LNB error kHz");
        plt::SetupAxisLimits(plt::X1, xs.front(), std::max(xs.front() + 1, xs.back()), plt::Cond_Always);
        plt::SetupAxisLimits(plt::Y1, *mm.first - pad, *mm.second + pad, plt::Cond_Always);
        plt::SetupAxisFormat(plt::Y1, "%.3f");
        plt::Spec s; s.LineColor = pal::accent(); s.Marker = plt::Marker_Circle; s.MarkerSize = 2;
        plt::PlotLine("err", xs.data(), ys.data(), (int)xs.size(), s);
        plt::EndPlot();
    }
}


// ---- the auto decoder ---------------------------------------------------------------------------------------------------------------------------

std::string utcText(double utc) {
    return dect2::timefmt::clock((int64_t)utc);
}

std::string decNames(int mask) {
    std::string o;
    for (int bit = 1; bit <= 128; bit <<= 1) if (mask & bit) { if (!o.empty()) o += " "; o += dect2::qo100DecoderName(bit); }
    return o.empty() ? "-" : o;
}

void autoView(App& a, float h) {
    const auto& t = T(a);
    const auto& ad = t.autoDec;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Decode automatically"));
    static const struct { unsigned bit; const char* name; const char* tip; } kDec[] = {
        {dect2::kQoDecSsb, nullptr, N_("List the SSB conversations (the Conversations view); no decoder runs")},
        {dect2::kQoDecFt8, "FT8", N_("FT8 on every signal that keys in 15 s slots")},
        {dect2::kQoDecFt4, "FT4", N_("FT4 on every signal that keys in 7.5 s slots")},
        {dect2::kQoDecJs8, "JS8", N_("JS8Call on every signal that keys in 15 s slots")},
        {dect2::kQoDecPsk, "PSK31", N_("PSK31 / PSK63 on every narrow continuous signal")},
        {dect2::kQoDecCw, "CW", N_("Morse on every keyed carrier")},
        {dect2::kQoDecSstv, "SSTV", N_("SSTV on every steady signal about 1.3 kHz wide; pictures go to the gallery")},
        {dect2::kQoDecRtty, "RTTY", N_("RTTY on every wide continuous pair of tones")}};
    for (const auto& d : kDec) {
        flowNext(8 * gUi);
        bool on = (S.autoMask & d.bit) != 0;
        char id[64]; snprintf(id, sizeof id, "%s##qodec%u", d.name ? d.name : tr("SSB conversations"), d.bit);
        if (ImGui::Checkbox(id, &on)) { if (on) S.autoMask |= d.bit; else S.autoMask &= ~d.bit; save(); }
        tip(tr(d.tip));
    }
    flowBreak();
    ImGui::SetNextItemWidth(150 * gUi);
    char fmt[48]; snprintf(fmt, sizeof fmt, "%s", tr("%d channels at most"));
    if (ImGui::SliderInt("##qoslots", &S.maxSlots, 1, 12, fmt)) save();
    tip(tr("How many decoder channels may run at once; each channel decodes every signal inside 3.4 kHz of audio"));
    flowNext(10 * gUi);
    ImGui::SetNextItemWidth(150 * gUi);
    snprintf(fmt, sizeof fmt, "%s", tr("CPU limit %.1f cores"));
    if (ImGui::SliderFloat("##qocpu", &S.cpuLimit, 0.1f, 4.f, fmt)) save();
    tip(tr("When the decoders need more than this, the weakest channel is dropped"));
    flowNext(10 * gUi);
    {
        const bool l = live(a);
        char ov[64]; snprintf(ov, sizeof ov, tr("CPU %.0f %%"), l ? ad.cpuLoad * 100.f : 0.f);
        const float lim = std::max(0.05f, l && ad.cpuLimit > 0 ? ad.cpuLimit : S.cpuLimit);
        ImGui::ProgressBar(l ? std::min(1.f, ad.cpuLoad / lim) : 0.f, ImVec2(130 * gUi, 0), ov);
        tip(tr("Time the decoders use per second of signal, against the limit"));
    }
    flowNext(10 * gUi);
    ImGui::TextDisabled(tr("%zu channels, %d dropped"), ad.slots.size(), ad.dropped);
    flowEnd();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("Start the receiver")); return; }
    if (!t.nbOk) { ImGui::TextColored(pal::warnAmber(), "the narrowband transponder is outside the radio's band: tune to it (Narrowband only) or raise the sample rate"); return; }

    const float sp = ImGui::GetStyle().ItemSpacing.x;
    const float lw = std::min((ImGui::GetContentRegionAvail().x - sp) * 0.46f, 560 * gUi);
    const float bh = std::max(100.f, ImGui::GetContentRegionAvail().y - 4 * gUi);
    ImGui::BeginChild("##qoautol", ImVec2(lw, bh), ImGuiChildFlags_Borders);
    ImGui::TextDisabled("%s", tr("Signals found"));
    if (ImGui::BeginTable("##qotrk", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn(tr("Downlink MHz")); ImGui::TableSetupColumn(tr("Width")); ImGui::TableSetupColumn("dB"); ImGui::TableSetupColumn(tr("Type")); ImGui::TableSetupColumn(tr("Decoding"));
        ImGui::TableHeadersRow();
        int i = 0;
        for (const auto& k : ad.tracks) {
            ImGui::PushID(i++);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char b[48]; snprintf(b, sizeof b, "%.4f##tk", k.rfHz / 1e6);
            if (k.active) ImGui::PushStyleColor(ImGuiCol_Text, pal::accent());
            if (ImGui::Selectable(b, false, ImGuiSelectableFlags_SpanAllColumns)) { setDial(k.cls == dect2::kQoSsb ? k.loHz - 300 : k.rfHz - 700); S.view = 0; }
            if (k.active) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s - %s\n%s", mhz(k.loHz, 4).c_str(), mhz(k.hiHz, 4).c_str(), k.idName.empty() ? "" : k.idName.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.0f Hz", k.widthHz);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", k.snrDb);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(k.cls == dect2::kQoUnknown ? tr("unknown") : dect2::qo100ClassName(k.cls));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(decNames(k.decMask & ~dect2::kQoDecSsb).c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##qoautor", ImVec2(0, bh), ImGuiChildFlags_Borders);
    // FT8 / FT4 / JS8 messages, newest first
    ImGui::TextDisabled("%s", tr("Decoded messages"));
    flowNext(10 * gUi);
    {
        resexp::Table x; x.cols = {"time", "frequency_hz", "band", "mode", "snr_db", "call", "grid", "message", "prop_mode", "sat_name"};
        for (const auto& d : ad.decodes)
            x.add({utcText(d.utc), resexp::num(d.rfHz, 0), "3cm", dect2::qo100DecoderName(d.kind), resexp::num(d.snrDb, 0), d.call, d.grid, d.text, "SAT", "QO-100"}, d.utc);
        resexp::saveButton(x, "qo100auto", S.note, "svqoa");
    }
    flowEnd();
    if (!S.note.empty()) ImGui::TextDisabled("%s", S.note.c_str());
    const float mh = std::max(60.f * gUi, ImGui::GetContentRegionAvail().y * 0.4f);
    if (ImGui::BeginTable("##qodecs", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV, ImVec2(0, mh))) {
        ImGui::TableSetupColumn("UTC", ImGuiTableColumnFlags_WidthFixed, 62 * gUi); ImGui::TableSetupColumn(tr("Mode"), ImGuiTableColumnFlags_WidthFixed, 46 * gUi);
        ImGui::TableSetupColumn(tr("Downlink MHz"), ImGuiTableColumnFlags_WidthFixed, 86 * gUi); ImGui::TableSetupColumn("dB", ImGuiTableColumnFlags_WidthFixed, 32 * gUi);
        ImGui::TableSetupColumn(tr("Message"));
        ImGui::TableHeadersRow();
        for (size_t i = ad.decodes.size(); i-- > 0;) {
            const auto& d = ad.decodes[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(utcText(d.utc).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::qo100DecoderName(d.kind));
            ImGui::TableNextColumn(); ImGui::Text("%.4f", d.rfHz / 1e6);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", d.snrDb);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(d.text.c_str());
        }
        ImGui::EndTable();
    }
    // PSK31 / CW / RTTY text
    ImGui::TextDisabled("%s", tr("Text"));
    ImGui::BeginChild("##qotxt", ImVec2(0, std::max(40.f * gUi, ImGui::GetContentRegionAvail().y * 0.55f)));
    if (ad.texts.empty()) ImGui::TextDisabled("%s", tr("nothing decoded yet"));
    for (const auto& x : ad.texts) {
        std::string tail = x.text.size() > 240 ? x.text.substr(x.text.size() - 240) : x.text;
        for (char& c : tail) if (c == '\n' || c == '\r') c = ' ';
        ImGui::TextColored(pal::accent(), "%s %.4f", dect2::qo100DecoderName(x.kind), x.rfHz / 1e6);
        ImGui::SameLine(); ImGui::TextWrapped("%s", tail.c_str());
    }
    ImGui::EndChild();
    // SSTV pictures
    if (!ad.images.empty()) {
        ImGui::Text(tr("%zu SSTV pictures"), ad.images.size());
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Open the gallery"))) a.galleryOpen = true;
    }
    ImGui::EndChild();
}

// ---- the SSB conversation manager --------------------------------------------------------------------------------------------------------------

void listenConversation(const dect2::Qo100Conversation& c) {
    setDial(c.dialHz);
    S.listenConv = c.id;
    S.listen = true;
    save();
}

void convView(App& a, float h) {
    const auto& t = T(a);
    const auto& ad = t.autoDec;
    if (!(S.autoMask & dect2::kQoDecSsb)) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(pal::warnAmber(), "%s", tr("Tick SSB conversations in the Auto decoder view to list the voice conversations."));
        ImGui::PopTextWrapPos();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Auto listen"));
    flowNext(4 * gUi);
    ImGui::SetNextItemWidth(170 * gUi);
    const char* modes[3] = {tr("off"), tr("the loudest conversation"), tr("the newest conversation")};
    if (ImGui::BeginCombo("##qoal", modes[std::clamp(S.autoListen, 0, 2)])) {
        for (int i = 0; i < 3; i++) if (ImGui::Selectable(modes[i], S.autoListen == i)) { S.autoListen = i; if (i) { S.listen = true; S.listenConv = 0; } save(); }
        ImGui::EndCombo();
    }
    tip(tr("Listen follows the loudest or the newest active conversation by itself, switching when it goes quiet or another one is clearly louder"));
    flowNext(10 * gUi);
    if (ImGui::Checkbox(tr("Squelch"), &S.squelch)) save();
    tip(tr("Mute the audio while only noise is there"));
    flowNext(10 * gUi);
    if (S.listenConv || S.autoListen) {
        if (ImGui::SmallButton(tr("Stop following"))) { S.listenConv = 0; S.autoListen = 0; save(); }
        flowNext(6 * gUi);
    }
    ImGui::TextDisabled("%s", S.listen ? tr("Listen is on") : tr("Listen is off"));
    flowEnd();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("Start the receiver")); return; }
    if (ad.convs.empty()) { ImGui::TextDisabled("%s", tr("no SSB conversation heard yet")); return; }
    if (ImGui::BeginTable("##qoconv", 10, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV, ImVec2(0, std::max(100.f, h - 40 * gUi)))) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 26 * gUi);
        ImGui::TableSetupColumn(tr("Start (UTC)")); ImGui::TableSetupColumn(tr("Length")); ImGui::TableSetupColumn(tr("Dial MHz")); ImGui::TableSetupColumn("dB");
        ImGui::TableSetupColumn(tr("Drift")); ImGui::TableSetupColumn(tr("Stations")); ImGui::TableSetupColumn(tr("Heard"));
        ImGui::TableSetupColumn(tr("Listen")); ImGui::TableSetupColumn(tr("Record"));
        ImGui::TableHeadersRow();
        for (const auto& c : ad.convs) {
            ImGui::PushID(c.id);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d", c.id);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(utcText(c.startUtc).c_str());
            ImGui::TableNextColumn();
            { const double len = std::max(0.0, c.lastUtc - c.startUtc); ImGui::Text("%d:%02d%s", (int)(len / 60), (int)std::fmod(len, 60.0), c.active ? "" : tr(" ended")); }
            ImGui::TableNextColumn(); ImGui::Text("%.4f", c.dialHz / 1e6);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", c.levelDb);
            ImGui::TableNextColumn(); if (c.driftValid) ImGui::Text("%+.0f Hz/min", c.driftHzPerMin); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%d", c.stations);
            ImGui::TableNextColumn();
            if (!c.snippet.empty() || !c.calls.empty()) {
                std::string calls; for (const auto& s : c.calls) calls += (calls.empty() ? "" : " ") + s;
                ImGui::TextUnformatted(calls.empty() ? c.snippet.c_str() : calls.c_str());
                if (ImGui::IsItemHovered() && !c.snippet.empty()) ImGui::SetTooltip("%s", c.snippet.c_str());
            } else ImGui::TextDisabled("%s", c.active ? tr("talking") : "-");
            ImGui::TableNextColumn();
            const bool cur = c.listening || S.listenConv == c.id;
            if (cur) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.45f));
            if (ImGui::SmallButton(cur ? tr("Listening") : tr("Listen"))) listenConversation(c);
            if (cur) ImGui::PopStyleColor();
            tip(tr("Tune the listening channel to this conversation (upper sideband, 300 - 3000 Hz) and play it; the dial follows its drift"));
            ImGui::TableNextColumn();
            bool r = S.rec.count(c.id) != 0;
            if (ImGui::Checkbox("##rec", &r)) { if (r) S.rec.insert(c.id); else S.rec.erase(c.id); }
            tip(tr("Record this conversation to a WAV file in the QO-100 folder of the data folder"));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void listenRow(App& a) {
    listenControls(a, S.listen, "qo100Listen", "qo100", "Play the upper sideband, 300 - 3000 Hz above the dial (corrected frequency), on the sound card");
    flowNext(10 * gUi);
    ImGui::TextDisabled("dial"); flowNext(4 * gUi);
    ImGui::SetNextItemWidth(150 * gUi);
    if (ImGui::InputTextWithHint("##qodial", mhz(S.dialHz, 6).c_str(), S.dialBuf, sizeof S.dialBuf, ImGuiInputTextFlags_EnterReturnsTrue)) {
        double hz;
        if (dect2::specx::parseFreq(S.dialBuf, hz)) { if (hz < 1e9) hz += 10e9; setDial(hz); }   // "489.6M" means 10489.6 MHz
        S.dialBuf[0] = 0;
    }
    tip("The USB dial (suppressed carrier) on the downlink: 10489.6M, or click the spectrum");
    flowNext(4 * gUi);
    if (ImGui::SmallButton("-100")) setDial(S.dialHz - 100);
    flowNext(2 * gUi);
    if (ImGui::SmallButton("+100")) setDial(S.dialHz + 100);
    if (live(a) && T(a).listening) { flowNext(10 * gUi); ImGui::TextDisabled("audio %.0f dBFS", T(a).audioDb); }
    flowEnd();
}

void tick(App& a) {
    load();
    auto& r = a.engine.qo100();
    r.setLo(loHz());
    r.setAutoCorrection(S.autoCorr);
    if (!S.listenConv && !S.autoListen) r.setDialHz(S.dialHz);   // else the receiver moves it with the conversation
    r.setListen(S.listen);
    r.setVolume(a.volume); r.setMuted(a.muted);
    r.setAutoDecoders(S.autoMask);
    r.setAutoMaxSlots(S.maxSlots);
    r.setAutoCpuLimit(S.cpuLimit);
    r.setListenConversation(S.listenConv);
    r.setAutoListen(S.autoListen);
    r.setSquelch(S.squelch && (S.listenConv || S.autoListen));
    {
        static bool dirSet = false;
        if (!dirSet) { dirSet = true; const std::string d = plat::dataDir() + "/qo100"; std::error_code ec; std::filesystem::create_directories(d, ec); r.setRecordDir(d); }
        static std::set<int> sent;
        for (int id : S.rec) if (!sent.count(id)) { r.setRecordConversation(id, true); sent.insert(id); }
        for (auto it = sent.begin(); it != sent.end();) { if (!S.rec.count(*it)) { r.setRecordConversation(*it, false); it = sent.erase(it); } else ++it; }
    }
    if (S.dvbsOn) {   // the decoder under the wideband view: its report (it is the engine's second mode), and the first service plays by itself
        if (a.engine.secondaryStandard() == 8) {
            RxTelemetry t2;
            if (a.engine.latestRx2(t2, S.rx2Seq)) { S.rx2Seq = t2.seq; S.rx2 = std::move(t2); }
            Player& pl = a.engine.player();
            if (S.autoPlay && pl.selected() < 0 && S.rx2.dvbs.tsLock) {
                const TsService* pick = nullptr;
                for (const TsService& sv : a.ts.services) {
                    if (!sv.havePmt) continue;
                    bool video = false;
                    for (const auto& st : sv.streams) video |= st.kind == "video";
                    if (video) { pick = &sv; break; }
                }
                if (pick) { pl.select(pick->id); pl.setVolume(a.volume); S.autoPlay = false; }
            }
            if (dvbsAutoService()) {   // a signal with one service: play it, and follow it when the view is moved to another carrier (#322)
                bool restart = false;
                const int id = dect2::autoServicePick(S.autoSvc, a.ts.services, pl.selected(), S.rx2.dvbs.tsLock, glfwGetTime(), &restart);
                if (id >= 0) { if (restart) pl.select(-1); pl.select(id); pl.setVolume(a.volume); S.autoPlay = false; }   // restart: same id, new carrier (#410)
            }
        } else if (S.dvbsRefused.empty()) S.dvbsOn = false;   // another second mode took the place
    }
    const auto& t = T(a);
    if (!live(a) || t.seq == S.lastSeq) return;
    S.lastSeq = t.seq;
    for (const auto& im : t.autoDec.images) {
        if (!im || !im->complete || im->width <= 0 || im->rgb.size() < (size_t)im->width * im->height * 3) continue;
        const std::string key = "qo100-sstv-" + std::to_string((long long)im->unixTime) + "-" + std::to_string(im->id);
        if (!S.published.insert(key).second) continue;
        gallery::Meta gm; gm.satellite = "QO-100"; gm.detail = im->mode;
        gallery::publishRgb(key, "SSTV", (double)im->unixTime, im->width, im->height, im->rgb.data(), gm);
    }
    if (S.listenConv || S.autoListen) { const double d = t.dialHz; if (std::fabs(d - S.dialHz) >= 10) { S.dialHz = d; } }   // the dial follows the conversation's drift
    if (!t.nb.empty()) {
        // a steady scale: whole 10 dB steps, moved only when the floor or the held peak really leave it (following every
        // update made the spectrum jump up and down as SSB stations came and went)
        S.peakHold = std::max(t.nbPeakDb, S.peakHold - 0.05f);
        const float lo = std::floor((t.nbFloorDb - 8) / 10) * 10, hi = std::max(lo + 50, std::ceil((S.peakHold + 5) / 10) * 10);
        if (S.lo == S.hi || std::fabs(lo - S.lo) >= 10 || hi > S.hi || S.hi - hi >= 20) { S.lo = lo; S.hi = hi; }
        S.wf.push(t.nb, t.nbFloorDb - 3, t.nbFloorDb + 35);
        S.sliceLo = t.nbF0Hz; S.sliceHi = t.nbF0Hz + t.nb.size() * t.nbBinHz;
    }
    if (!t.wb.empty()) {
        S.wPeakHold = std::max(t.wbPeakDb, S.wPeakHold - 0.05f);
        const float lo = std::floor((t.wbFloorDb - 8) / 10) * 10, hi = std::max(lo + 40, std::ceil((S.wPeakHold + 5) / 10) * 10);
        if (S.wlo == S.whi || std::fabs(lo - S.wlo) >= 10 || hi > S.whi || S.whi - hi >= 20) { S.wlo = lo; S.whi = hi; }   // steady, as above
    }
}

void tab(App& a) {
    load();
    // the strip wraps to another line and back as its texts change width: keep the tallest it has been at this width, so the
    // plots under it do not jump
    static float stripH = 0, stripW = 0;
    const float y0 = ImGui::GetCursorPosY(), w0 = ImGui::GetContentRegionAvail().x;
    if (w0 != stripW) { stripW = w0; stripH = 0; }
    strip(a);
    const float used = ImGui::GetCursorPosY() - y0;
    stripH = std::max(stripH, used);
    if (used + ImGui::GetStyle().ItemSpacing.y < stripH) ImGui::Dummy(ImVec2(1, stripH - used - ImGui::GetStyle().ItemSpacing.y));
    // a fixed two-line height: the line wraps as its numbers change width, which would move the plots under it every frame
    ImGui::BeginChild("##lock", ImVec2(0, 2 * ImGui::GetTextLineHeightWithSpacing() + 2 * gUi), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    lockLine(a);
    ImGui::EndChild();
    const char* const kV[] = {"Narrowband", "Wideband", "Drift", tr("Auto decoder"), tr("Conversations")};
    for (int i = 0; i < 5; i++) {
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(kV[i], S.view == i)) { S.view = i; save(); }
    }
    if (S.view == 0 || S.view == 4) { ImGui::SameLine(0, 16 * gUi); listenRow(a); }
    const float h = std::max(120.f, ImGui::GetContentRegionAvail().y - 2);
    if (S.view == 0) narrowView(a, h);
    else if (S.view == 1 && S.dvbsOn) {   // the spectrum stays live above, the decoder of the clicked carrier below
        const float sp = ImGui::GetStyle().ItemSpacing.y;
        const float ph = std::clamp(h * 0.55f, 240.f * gUi, std::max(240.f * gUi, h - 150.f * gUi - sp));
        wideView(a, std::max(100.f * gUi, h - ph - sp));
        decodePanel(a, ph);
    }
    else if (S.view == 1) wideView(a, h);
    else if (S.view == 3) autoView(a, h);
    else if (S.view == 4) convView(a, h);
    else driftView(a, h);
}

void list(App& a) {
    load();
    const auto& t = T(a);
    if (!live(a) || t.signals.empty()) { ImGui::TextDisabled("no signals yet"); return; }
    ImGui::TextDisabled("narrowband, %s", t.corrValid ? "corrected" : "not corrected yet");
    int i = 0;
    for (const auto& s : t.signals) {
        static const char* const kB[] = {"CW beacon", "PSK beacon", "CW beacon"};
        char b[96];
        snprintf(b, sizeof b, "%.4f  %4.0f dB  %s##qs%d", s.rfHz / 1e6, s.snrDb, s.beacon >= 0 ? kB[s.beacon] : s.cls != dect2::kQoUnknown ? dect2::qo100ClassName(s.cls) : s.widthHz > 400 ? "SSB" : "carrier", i++);
        if (ImGui::Selectable(b)) { setDial(s.widthHz > 400 ? s.rfHz - 600 : s.rfHz - 700); S.view = 0; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s, %.0f Hz wide\nclick: put the dial on it", mhz(s.rfHz, 6).c_str(), s.widthHz);
    }
}

void receiver(App& a) {
    const auto& t = T(a);
    {   // aim on the beacons
        float best = 0; bool found = false;
        for (const auto& b : t.beacons) if (b.found) { best = std::max(best, (float)b.snrDb); found = true; }
        easeui::alignButton(a, "qo100", best, live(a) && found);
    }
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    ImGui::TextWrapped("%s", dect2::qo100Summary(t).c_str());
    ImGui::TextWrapped("Radio %s at %.3f Msps = %s - %s (LO %s%s)", mhz(t.centreIfHz).c_str(), t.rate / 1e6, mhz(t.wbF0Hz).c_str(),
                       mhz(t.wbF0Hz + t.wb.size() * t.wbBinHz).c_str(), mhz(t.loHz).c_str(), t.inverted ? ", inverted" : "");
    ImGui::TextWrapped("Narrowband %s, %.0f Hz bins; wideband %s, %.1f kHz bins", t.nbOk ? "in the band" : "outside the band", t.nbBinHz, t.wbOk ? "in the band" : "outside the band", t.wbBinHz / 1e3);
    for (int i = 0; i < 3; i++) {
        const auto& b = t.beacons[i];
        ImGui::Text("beacon %s: %s", mhz(b.nominalHz).c_str(), b.found ? "" : "not found");
        if (b.found) { ImGui::SameLine(0, 0); ImGui::Text("at %s before correction, %.1f dB", mhz(b.rfHz, 6).c_str(), b.snrDb); }
    }
    if (t.corrValid) ImGui::TextWrapped("LNB oscillator %s (nominal %+.3f kHz, %+.3f ppm)", mhz(t.loHz + t.lnbErrHz, 6).c_str(), t.lnbErrHz / 1e3, t.lnbErrPpm);
}

void status(App& a) {
    StatusPanel panel;
    const bool on = live(a);
    lamp("QO-100", on ? (T(a).state == 2 ? 1 : T(a).state == 3 ? 2 : 3) : 0, (int)Ic::Globe); flowNext(12 * gUi);
    ImGui::TextUnformatted(on ? dect2::qo100Summary(T(a)).c_str() : (a.engine.running() ? "starting" : "stopped"));
    flowEnd();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "QO-100";
    if (live(a)) {
        const auto& t = T(a);
        char b[128];
        if (t.corrValid) snprintf(b, sizeof b, "LNB %+.3f kHz (%+.2f ppm)%s", t.lnbErrHz / 1e3, t.lnbErrPpm, t.state == 3 ? ", held" : "");
        else snprintf(b, sizeof b, "%s", t.state == 0 ? "narrowband not in the band" : "searching for the beacons");
        l2 = b;
    }
}

void synth(App& a, bool& changed) {
    dect2::SynthConfig& sc = a.tune.synth;
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("the beacons, a test carrier at 10489.650, four SSB stations and two DATV carriers, through an LNB 27 kHz off that drifts"); ImGui::PopTextWrapPos(); }
    bool quiet = sc.modeOpt[0] == 1;
    if (ImGui::Checkbox("Noise only", &quiet)) { sc.modeOpt[0] = quiet ? 1 : 0; changed = true; }
    tip("No signals: the beacons must not lock");
    float err = (float)((dect2::kQo100SynthErrHz + sc.modeVal[0]) / 1e3);
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::SliderFloat("LNB error", &err, -300, 300, "%.1f kHz")) { sc.modeVal[0] = err * 1e3 - dect2::kQo100SynthErrHz; changed = true; }
    float dr = (float)(sc.modeVal[1] != 0 ? sc.modeVal[1] : dect2::kQo100SynthDriftHzPerMin);
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::SliderFloat("Drift", &dr, -600, 600, "%.0f Hz/min")) { sc.modeVal[1] = dr == 0 ? 0.001 : dr; changed = true; }
}

} // namespace

extern const ModeUi kQo100Ui;
const ModeUi kQo100Ui = {
    .sideTitle = "SIGNALS",
    .tabName = "QO-100",
    .tabIcon = Ic::Globe,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .synth = synth,
    .tick = tick,
};

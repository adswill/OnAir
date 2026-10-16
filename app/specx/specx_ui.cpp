// Spectrum explorer screens: a slim control strip (frequency as text, span, RBW and averaging, peak hold, auto range, gain, sample
// rate, LNB / converter offset, named point and range bookmarks, help), a band-plan strip, a big spectrum over a waterfall that answer the mouse (crosshair, markers, centre,
// zoom, pan, region), the region's measurements (with a hand-over to DVB-S/S2), and a sweep across a range that stitches a panorama
// with its own waterfall and a table of the signals found. No decoder.
#include <fstream>
#include "app.h"
#include "results_export.h"
#include "gfx.h"
#include "plot.h"
#include "dect2/specx_measure.h"
#include "dect2/specx_rx.h"
#include "dect2/specx_tel.h"
#include "dect2/u8file.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace sx = dect2::specx;

struct Bookmark {
    std::string name;
    double rfHz = 0;     // a point, or the low edge of a range
    double rfHiHz = 0;   // 0: a point; else the high edge (Hz)
    uint32_t rgb = 0x4FA3E0;   // 0xRRGGBB
};
bool markRange(const Bookmark& b) { return b.rfHiHz > b.rfHz + 0.5; }
double markMid(const Bookmark& b) { return markRange(b) ? 0.5 * (b.rfHz + b.rfHiHz) : b.rfHz; }
ImU32 markRgb(uint32_t rgb, int a) { return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, a); }

struct WfImage {
    gfx::Image* img = nullptr;
    int W = 1024, H = 200;
    std::vector<uint32_t> px;
    bool dirty = false;
    void push(const std::vector<float>& db, float lo, float hi) {
        if (px.empty()) px.assign((size_t)W * H, 0xFF000000u);
        std::copy_backward(px.begin(), px.end() - W, px.end());
        const size_t n = db.size();
        for (int x = 0; x < W; x++) {
            size_t a = (size_t)x * n / W, b = std::max(a + 1, (size_t)(x + 1) * n / W);
            float m = -200;
            for (size_t k = a; k < b && k < n; k++) m = std::max(m, db[k]);
            px[(size_t)x] = Waterfall::jet((m - lo) / std::max(1.f, hi - lo));
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

struct Sweep {
    bool active = false;
    double loRf = 950e6, hiRf = 2150e6;
    std::vector<double> centres;   // radio Hz
    size_t idx = 0;
    double stepT = 0;
    uint64_t stepSeq = 0;
    sx::Panorama pano;
    double panoEnbw = 1;
    std::vector<sx::Measure> found;   // radio Hz
    WfImage wf;
    int passes = 0;
};

struct State {
    bool loaded = false;
    sx::Lnb lnb;
    int lnbPreset = 0;               // lnbPresets index, or count = custom
    int fftIdx = 4, avg = 4;         // 256 << fftIdx
    bool peakHold = false, autoRange = true;
    float lo = -110, hi = -20;
    double viewLo = 0, viewHi = 0;   // RF Hz
    double lastCentre = 0, lastRate = 0;
    std::vector<float> peak;
    std::vector<double> markers;     // RF Hz
    bool hasRegion = false;
    double regLo = 0, regHi = 0;     // RF Hz
    // mouse
    int dragKind = 0;                // 0 none, 1 pan, 2 region
    ImVec2 dragStart;
    double dragF0 = 0, dragView0 = 0, dragView1 = 0;
    bool dragMoved = false;
    double ctxRf = 0;                // a right-click without a drag: where (RF Hz)
    char freqBuf[48] = "";
    std::string freqErr;
    std::vector<Bookmark> marks;
    char markName[48] = "";
    float markCol[3] = {0.31f, 0.64f, 0.88f};
    bool markAsRange = true;             // when a region is selected: save it as a range, not the centre
    uint64_t lastSeq = 0;
    WfImage wf;
    Sweep sw;
    int pushedFft = -1, pushedAvg = -1;
    bool wasRunning = false;
};
State S;

void tip(const char* t) { if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", t); }
bool live(const App& a) { return a.rx.standard == 42 && !a.rx.specx.spectrum.empty(); }
double centreRadio(const App& a) { return a.tune.centerHz; }
double rateOf(const App& a) { return a.rx.specx.rate > 0 ? a.rx.specx.rate : a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate; }
double toRf(double radioHz) { return S.lnb.rfHz(radioHz); }
double toRadio(double rfHz) { return S.lnb.radioHz(rfHz); }

void saveMarks() {
    std::string s;
    for (auto& b : S.marks) {
        char v[80];
        snprintf(v, sizeof v, "\t%.0f\t%06X\t%.0f\n", b.rfHz, b.rgb & 0xFFFFFF, markRange(b) ? b.rfHiHz : 0.0);
        s += b.name + v;
    }
    plat::prefs().setS("specxBookmarks", s);
}
void saveLnb() {
    plat::Prefs& p = plat::prefs();
    p.setB("specxLnbOn", S.lnb.on); p.setD("specxLnbLo", S.lnb.loHz); p.setI("specxLnbKind", S.lnb.kind); p.setI("specxLnbPreset", S.lnbPreset);
}
void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& p = plat::prefs();
    S.lnb.on = p.getB("specxLnbOn", false);
    S.lnb.loHz = p.getD("specxLnbLo", 9750e6);
    S.lnb.kind = (int)std::max(0L, std::min(2L, p.getI("specxLnbKind", 0)));
    S.lnbPreset = (int)p.getI("specxLnbPreset", 0);
    S.fftIdx = (int)std::max(0L, std::min(8L, p.getI("specxFft", 4)));
    S.avg = (int)std::max(1L, std::min(100L, p.getI("specxAvg", 4)));
    S.peakHold = p.getB("specxPeak", false);
    S.autoRange = p.getB("specxAuto", true);
    std::istringstream in(p.getS("specxBookmarks", ""));
    std::string line;
    while (std::getline(in, line)) {
        const size_t t = line.find('\t');
        if (t == std::string::npos) continue;
        Bookmark b;
        b.name = line.substr(0, t);
        b.rfHz = std::atof(line.c_str() + t + 1);
        const size_t t2 = line.find('\t', t + 1);
        if (t2 != std::string::npos) {
            b.rgb = (uint32_t)strtoul(line.c_str() + t2 + 1, nullptr, 16) & 0xFFFFFF;
            const size_t t3 = line.find('\t', t2 + 1);
            if (t3 != std::string::npos) b.rfHiHz = std::atof(line.c_str() + t3 + 1);
        }
        if (b.rfHiHz > 0 && b.rfHiHz < b.rfHz) std::swap(b.rfHz, b.rfHiHz);
        S.marks.push_back(b);
    }
}

// the band the radio sees, in RF Hz (lowest first)
void bandRf(const App& a, double& lo, double& hi) {
    const double c = centreRadio(a), r = rateOf(a);
    lo = toRf(c - r / 2); hi = toRf(c + r / 2);
    if (lo > hi) std::swap(lo, hi);
}

void tuneRf(App& a, double rfHz) {
    const double r = toRadio(rfHz);
    const ModeTuning& mt = *modeTuning(43);
    if (r < mt.minMhz * 1e6 || r > mt.maxMhz * 1e6) { S.freqErr = "the radio would have to tune to " + sx::freqText(r) + ": out of range"; return; }
    S.freqErr.clear();
    const double span = S.viewHi - S.viewLo;
    tuneFreq(a, r / 1e6);
    if (span > 0) { S.viewLo = rfHz - span / 2; S.viewHi = rfHz + span / 2; }
    S.lastCentre = a.tune.centerHz;
}

// the current spectrum in radio Hz
sx::SpecView view(const App& a) {
    const dect2::SpecxTelemetry& t = a.rx.specx;
    sx::SpecView v;
    v.db = t.spectrum.data(); v.n = (int)t.spectrum.size();
    v.binHz = t.rate / std::max(1, t.fftSize);
    v.f0Hz = centreRadio(a) - v.n / 2 * v.binHz;
    v.enbw = t.enbw;
    return v;
}

double levelAt(const App& a, double rfHz) {
    if (!live(a)) return -200;
    sx::SpecView v = view(a);
    const int k = (int)std::lround(v.bin(toRadio(rfHz)));
    return k >= 0 && k < v.n ? v.db[k] : -200;
}

void openInDvbs(App& a, const sx::Measure& m) {
    const double radio = m.centreHz, msym = std::round(m.symRateBest / 1e3) / 1e3;
    if (a.engine.running()) a.engine.stop();
    selectMode(a, 6);
    tuneFreq(a, radio / 1e6);
    dvbsSetSymbolRate(a, msym);
    startReceiver(a);
}

// the signal identifier on a region (radio Hz) of the spectrum
void identifyRegion(App& a, double loRadio, double hiRadio) {
    const double c = centreRadio(a);
    identifyStart(a, c, (loRadio + hiRadio) / 2 - c, std::fabs(hiRadio - loRadio), S.lnb.inverted() ? 0.0 : toRf(c));   // inverted: no band check
}

// the signal under a right-click: the run of the spectrum over the floor around it, or 25 kHz
void identifyRfAt(App& a, double rfHz) {
    const double r = toRadio(rfHz);
    double lo = r - 12.5e3, hi = r + 12.5e3;
    if (live(a)) for (const auto& m : sx::detect(view(a), 6, 3)) if (r >= m.loHz && r <= m.hiHz) { lo = m.loHz; hi = m.hiHz; break; }
    identifyRegion(a, lo, hi);
}

void measurePanel(App& a, const sx::Measure& m, bool canHand) {
    auto row = [](const char* k, const std::string& v, const char* t) {
        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("%s", k); tip(t);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(v.c_str());
    };
    char b[96];
    if (!ImGui::BeginTable("##sxm", 2, ImGuiTableFlags_SizingFixedFit)) return;
    row("Centre", sx::freqText(toRf(m.centreHz)), "Middle of the -3 dB band (RF frequency when a converter is set)");
    snprintf(b, sizeof b, "%s", sx::freqText(m.bw3Hz).c_str()); row("Bandwidth -3 dB", b, "Width where the signal is half its top power");
    snprintf(b, sizeof b, "%s%s", m.bw26AtFloor ? "> " : "", sx::freqText(m.bw26Hz).c_str());
    row("Bandwidth -26 dB", b, "Occupied width 26 dB down (\"> \": the signal meets the floor first; this is where)");
    snprintf(b, sizeof b, "%s%s", m.bw20AtFloor ? "> " : "", sx::freqText(m.bw20Hz).c_str()); row("Bandwidth -20 dB", b, "Width 20 dB down");
    row("Occupied (99 %)", sx::freqText(m.obwHz), "The width that holds 99 % of the signal's power over the floor");
    snprintf(b, sizeof b, "%.1f dBFS", m.powerDb); row("Channel power", b, "All the power in the selected region");
    snprintf(b, sizeof b, "%.1f dBFS at %s", m.peakDb, sx::freqText(toRf(m.peakHz)).c_str()); row("Peak", b, "The strongest bin");
    snprintf(b, sizeof b, "%.1f dB (floor %.1f dBFS)", m.snrDb, m.floorDb); row("SNR", b, "Top of the signal over the noise floor (per bin)");
    if (m.flatTop) {
        snprintf(b, sizeof b, "%.4f Msym/s (roll-off %.2f)", m.symRateBest / 1e6, m.rollOff);
        row("Symbol rate", b, "A flat-topped carrier: its -3 dB width is the symbol rate; the roll-off is the closest to BW-26 / BW-3 - 1");
        for (int j = 0; j < 3; j++) {
            snprintf(b, sizeof b, "%.4f Msym/s", m.symRate[j] / 1e6);
            char k[32]; snprintf(k, sizeof k, "  at roll-off %.2f", sx::kRollOffs[j]);
            row(k, b, "Occupied width / (1 + roll-off)");
        }
    }
    ImGui::EndTable();
    if (canHand) {
        if (ImGui::Button("Identify this signal##region")) identifyRegion(a, m.loHz, m.hiHz);
        tip("Record a moment of this region and work out the modulation, symbol rate and the mode it could be");
        if (m.looksDvbs) ImGui::SameLine();
    }
    if (m.looksDvbs && canHand) {
        if (ImGui::Button("Open in DVB-S/S2")) openInDvbs(a, m);
        tip("Switch to DVB-S/S2 with this carrier's frequency and symbol rate filled in");
    }
}

// mouse gestures over a strip [l, r] x [top, bot] that shows S.viewLo .. S.viewHi
void gestures(App& a, float l, float r, bool hovered, float top, float bot) {
    ImGuiIO& io = ImGui::GetIO();
    const double span = S.viewHi - S.viewLo;
    auto fAt = [&](float x) { return S.viewLo + (x - l) / std::max(1.f, r - l) * span; };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered) {
        const double f = fAt(io.MousePos.x);
        dl->AddLine(ImVec2(io.MousePos.x, top), ImVec2(io.MousePos.x, bot), IM_COL32(255, 255, 255, 90));
        ImGui::SetTooltip("%s\n%.1f dBFS", sx::freqText(f).c_str(), levelAt(a, f));
        if (io.MouseWheel != 0) {
            const double k = std::pow(0.8, io.MouseWheel);
            const double rate = rateOf(a), minSpan = rate / 2048, maxSpan = rate * 4;
            const double ns = std::max(minSpan, std::min(maxSpan, span * k)), fr = (f - S.viewLo) / span;
            S.viewLo = f - fr * ns; S.viewHi = S.viewLo + ns;
        }
        const bool region = ImGui::IsMouseClicked(1) || (ImGui::IsMouseClicked(0) && io.KeyShift);
        if (ImGui::IsMouseDoubleClicked(0)) {
            if (!S.markers.empty()) S.markers.pop_back();   // the first click of the pair put one
            tuneRf(a, f);
            S.dragKind = 0;
        } else if (region || ImGui::IsMouseClicked(0)) {
            S.dragKind = region ? 2 : 1; S.dragStart = io.MousePos; S.dragF0 = f; S.dragView0 = S.viewLo; S.dragView1 = S.viewHi; S.dragMoved = false;
        }
    }
    if (S.dragKind) {
        const int btn = ImGui::IsMouseDown(1) ? 1 : 0;
        if (std::fabs(io.MousePos.x - S.dragStart.x) > 3) S.dragMoved = true;
        const double f = fAt(io.MousePos.x);
        if (S.dragKind == 1 && S.dragMoved) {
            const double d = (io.MousePos.x - S.dragStart.x) / std::max(1.f, r - l) * (S.dragView1 - S.dragView0);
            S.viewLo = S.dragView0 - d; S.viewHi = S.dragView1 - d;
        } else if (S.dragKind == 2 && S.dragMoved) {
            S.hasRegion = true; S.regLo = std::min(S.dragF0, f); S.regHi = std::max(S.dragF0, f);
        }
        if (!ImGui::IsMouseDown(btn)) {
            if (S.dragKind == 1 && !S.dragMoved && S.markers.size() < 8) S.markers.push_back(S.dragF0);
            if (S.dragKind == 2 && !S.dragMoved && ImGui::IsMouseReleased(1)) { S.ctxRf = S.dragF0; ImGui::OpenPopup("##sxctx"); }   // a right-click, not a drag
            if (S.dragKind == 1 && S.dragMoved) {
                double bl, bh; bandRf(a, bl, bh);
                if (S.viewLo < bl || S.viewHi > bh) tuneRf(a, (S.viewLo + S.viewHi) / 2);   // left the band: follow
            }
            S.dragKind = 0;
        }
    }
    if (ImGui::BeginPopup("##sxctx")) {
        if (ImGui::MenuItem("Identify this signal##rf", nullptr, false, live(a))) identifyRfAt(a, S.ctxRf);
        ImGui::EndPopup();
    }
}

void drawMarks(ImDrawList* dl, double loHz, double hiHz, float l, float r, float top, float bot) {
    if (!dl || r <= l || hiHz <= loHz || bot <= top) return;
    const float lh = ImGui::GetTextLineHeight();
    auto X = [&](double f) { return (float)(l + (f - loHz) / (hiHz - loHz) * (r - l)); };
    dl->PushClipRect(ImVec2(l, top), ImVec2(r, bot), true);
    int row = 0;
    for (const Bookmark& m : S.marks) {
        const double a0 = m.rfHz, a1 = markRange(m) ? m.rfHiHz : m.rfHz;
        if (a1 < loHz || a0 > hiHz) continue;
        const ImU32 c = markRgb(m.rgb, 220), fill = markRgb(m.rgb, 36), edge = markRgb(m.rgb, 180);
        const float ny = top + 2 + lh * (float)(row % 3);
        row++;
        if (markRange(m)) {
            const float x0 = std::max(l, X(a0)), x1 = std::min(r, X(a1));
            dl->AddRectFilled(ImVec2(x0, top), ImVec2(std::max(x0 + 1.f, x1), bot), fill);
            dl->AddLine(ImVec2(x0, top), ImVec2(x0, bot), edge);
            dl->AddLine(ImVec2(x1, top), ImVec2(x1, bot), edge);
            if (!m.name.empty()) {
                const ImVec2 ts = ImGui::CalcTextSize(m.name.c_str());
                const float tx = std::clamp(x0 + 3.f, l, std::max(l, r - ts.x - 2));
                if (ny + lh <= bot) dl->AddText(ImVec2(tx, ny), c, m.name.c_str());
            }
        } else {
            const float x = X(a0);
            if (x < l || x > r) continue;
            dl->AddLine(ImVec2(x, top), ImVec2(x, bot), edge);
            if (!m.name.empty() && ny + lh <= bot) dl->AddText(ImVec2(x + 3, ny), c, m.name.c_str());
        }
    }
    dl->PopClipRect();
}

void overlays(App& a, float l, float r, float top, float bot) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const double span = S.viewHi - S.viewLo;
    auto X = [&](double f) { return (float)(l + (f - S.viewLo) / span * (r - l)); };
    dl->PushClipRect(ImVec2(l, top), ImVec2(r, bot), true);
    if (S.hasRegion) {
        dl->AddRectFilled(ImVec2(X(S.regLo), top), ImVec2(X(S.regHi), bot), IM_COL32(120, 190, 255, 40));
        dl->AddRect(ImVec2(X(S.regLo), top), ImVec2(X(S.regHi), bot), IM_COL32(120, 190, 255, 160));
    }
    for (size_t i = 0; i < S.markers.size(); i++) {
        const float x = X(S.markers[i]);
        dl->AddLine(ImVec2(x, top), ImVec2(x, bot), IM_COL32(255, 210, 90, 170));
        char b[64]; snprintf(b, sizeof b, "M%zu %.1f", i + 1, levelAt(a, S.markers[i]));
        dl->AddText(ImVec2(x + 3, top + 2 + 14 * gUi * (float)(i % 3)), IM_COL32(255, 210, 90, 255), b);
    }
    dl->PopClipRect();
    drawMarks(dl, S.viewLo, S.viewHi, l, r, top, bot);
}

void spectrumView(App& a, float h) {
    const dect2::SpecxTelemetry& t = a.rx.specx;
    const bool on = live(a);
    std::vector<double> xs, ys, ps;
    if (on) {
        sx::SpecView v = view(a);
        xs.resize((size_t)v.n); ys.resize((size_t)v.n);
        for (int k = 0; k < v.n; k++) { xs[(size_t)k] = toRf(v.freq(k)) / 1e6; ys[(size_t)k] = v.db[k]; }
        if (S.peakHold && S.peak.size() == (size_t)v.n) { ps.resize((size_t)v.n); for (int k = 0; k < v.n; k++) ps[(size_t)k] = S.peak[(size_t)k]; }
    }
    float l = 0, r = 0, top = 0, bot = 0; bool hov = false;
    if (plt::BeginPlot("##sxspec", ImVec2(-1, h), plt::Flags_NoLegend | plt::Flags_NoTitle | plt::Flags_NoMouseText)) {
        plt::SetupAxes("MHz", "dBFS");
        plt::SetupAxisLimits(plt::X1, S.viewLo / 1e6, S.viewHi / 1e6, plt::Cond_Always);
        plt::SetupAxisLimits(plt::Y1, S.lo, S.hi, plt::Cond_Always);
        plt::SetupAxisFormat(plt::X1, "%.3f");
        if (!ps.empty()) { plt::Spec s; s.LineColor = ImVec4(1.f, 0.55f, 0.3f, 0.7f); plt::PlotLine("peak", xs.data(), ps.data(), (int)ps.size(), s); }
        if (!ys.empty()) { plt::Spec s; s.LineColor = pal::accent(); plt::PlotLine("spectrum", xs.data(), ys.data(), (int)ys.size(), s); }
        if (!on) plt::PlotText(a.engine.running() ? "starting" : "Start the receiver to look around", (S.viewLo + S.viewHi) / 2e6, (S.lo + S.hi) / 2);
        const ImVec2 p = plt::GetPlotPos(), sz = plt::GetPlotSize();
        l = p.x; r = p.x + sz.x; top = p.y; bot = p.y + sz.y;
        hov = plt::IsPlotHovered();
        plt::EndPlot();
    }
    if (r > l) { overlays(a, l, r, top, bot); gestures(a, l, r, hov, top, bot); }
    (void)t;
}

void waterfallView(App& a, float h) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(8, 9, 10, 255));
    // the image covers the radio's band; show the part in view (flipped for an inverting LNB)
    const double c = centreRadio(a), rate = rateOf(a);
    double u0 = (toRadio(S.viewLo) - (c - rate / 2)) / rate, u1 = (toRadio(S.viewHi) - (c - rate / 2)) / rate;
    S.wf.draw(dl, p0, ImVec2(p0.x + w, p0.y + h), (float)u0, (float)u1);
    ImGui::InvisibleButton("##sxwf", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    overlays(a, p0.x, p0.x + w, p0.y, p0.y + h);
    gestures(a, p0.x, p0.x + w, hov, p0.y, p0.y + h);
}

void helpButton() {
    if (ImGui::SmallButton("?##sxhelp")) ImGui::OpenPopup("##sxhelpp");
    tip("Mouse gestures");
    if (ImGui::BeginPopup("##sxhelpp")) {
        ImGui::TextUnformatted("Mouse over the spectrum or the waterfall:");
        ImGui::BulletText("hover: frequency and level under the cursor");
        ImGui::BulletText("click: put a marker (up to 8; Clear removes them)");
        ImGui::BulletText("double-click: tune there (centre)");
        ImGui::BulletText("scroll wheel: zoom around the cursor");
        ImGui::BulletText("drag: pan (the radio follows when you leave its band)");
        ImGui::BulletText("right-drag or shift-drag: select a region to measure");
        ImGui::TextUnformatted("Frequencies: 1.2G, 433.92M, 145500k, or MHz without a unit.");
        ImGui::EndPopup();
    }
}

void lnbControls(App& a) {
    int n; const sx::LnbPreset* pr = sx::lnbPresets(n);
    if (ImGui::Checkbox("Converter##sxlnb", &S.lnb.on)) { saveLnb(); S.lastCentre = 0; }
    tip("An LNB or converter before the radio: every frequency shown and typed is then the RF frequency");
    if (!S.lnb.on) return;
    flowNext(6 * gUi);
    ImGui::SetNextItemWidth(220 * gUi);
    const char* cur = S.lnbPreset >= 0 && S.lnbPreset < n ? pr[S.lnbPreset].name : "Custom";
    if (ImGui::BeginCombo("##sxlnbp", cur)) {
        for (int i = 0; i <= n; i++) {
            if (ImGui::Selectable(i < n ? pr[i].name : "Custom", S.lnbPreset == i)) {
                S.lnbPreset = i;
                if (i < n) { S.lnb.loHz = pr[i].loMhz * 1e6; S.lnb.kind = pr[i].kind; }
                saveLnb(); S.lastCentre = 0;
            }
        }
        ImGui::EndCombo();
    }
    tip("Converter presets: Universal LNB 9750 / 10600 MHz, QO-100, C band 5150 MHz (inverted), HF upconverter +125 MHz");
    if (S.lnbPreset >= n) {
        flowNext(6 * gUi);
        double lo = S.lnb.loHz / 1e6;
        ImGui::SetNextItemWidth(90 * gUi);
        if (ImGui::InputDouble("##sxlo", &lo, 0, 0, "%.3f MHz", ImGuiInputTextFlags_EnterReturnsTrue)) { S.lnb.loHz = std::max(0.0, lo) * 1e6; saveLnb(); S.lastCentre = 0; }
        tip("Oscillator frequency of the converter");
        flowNext(6 * gUi);
        static const char* kinds[] = {"RF - LO", "RF + LO", "LO - RF"};
        ImGui::SetNextItemWidth(90 * gUi);
        if (ImGui::Combo("##sxkind", &S.lnb.kind, kinds, 3)) { saveLnb(); S.lastCentre = 0; }
        tip("RF - LO: down-converter, oscillator below; RF + LO: up-converter; LO - RF: oscillator above (spectrum inverted)");
    }
    (void)a;
}

void strip(App& a) {
    ImGui::SetNextItemWidth(140 * gUi);
    if (ImGui::InputTextWithHint("##sxfreq", sx::freqText(toRf(centreRadio(a))).c_str(), S.freqBuf, sizeof S.freqBuf, ImGuiInputTextFlags_EnterReturnsTrue)) {
        double hz;
        if (sx::parseFreq(S.freqBuf, hz)) { tuneRf(a, hz); S.freqBuf[0] = 0; }
        else S.freqErr = "not a frequency: try 1.2G, 433.92M or 145500k";
    }
    tip("Type a frequency and press Enter: 1.2G, 433.92M, 145500k (no unit: MHz)");
    flowNext(6 * gUi);
    if (ImGui::Button("*##sxstar")) ImGui::OpenPopup("##sxbm");
    tip("Bookmark this frequency, or the selected region as a named range");
    if (ImGui::BeginPopup("##sxbm")) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(160 * gUi);
        const bool ent = ImGui::InputTextWithHint("##sxbmn", "name", S.markName, sizeof S.markName, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::ColorEdit3("##sxbmc", S.markCol, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
        tip("Colour of this bookmark");
        ImGui::SameLine();
        const bool save = ImGui::Button("Save") || ent;
        if (S.hasRegion) {
            ImGui::Checkbox("Range (selected region)", &S.markAsRange);
            tip("Save the selected region as a named range marker; off: the centre frequency as a point");
        }
        if (save) {
            const uint32_t rgb = ((uint32_t)(S.markCol[0] * 255 + 0.5f) << 16) | ((uint32_t)(S.markCol[1] * 255 + 0.5f) << 8) | (uint32_t)(S.markCol[2] * 255 + 0.5f);
            double lo = toRf(centreRadio(a)), hi = 0;
            if (S.hasRegion && S.markAsRange) {
                lo = S.regLo; hi = S.regHi;
                if (lo > hi) std::swap(lo, hi);
            }
            std::string nm = S.markName[0] ? S.markName : (hi > lo ? "range" : sx::freqText(lo));
            S.marks.push_back({std::move(nm), lo, hi, rgb});
            S.markName[0] = 0; saveMarks(); ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    flowNext(10 * gUi);
    ImGui::TextDisabled("Span"); flowNext(4 * gUi);
    {
        const double rate = rateOf(a);
        static const double kF[] = {1, 0.5, 0.25, 0.1, 0.05, 0.01};
        static const char* kN[] = {"full", "1/2", "1/4", "1/10", "1/20", "1/100"};
        ImGui::SetNextItemWidth(105 * gUi);
        if (ImGui::BeginCombo("##sxspan", sx::freqText(S.viewHi - S.viewLo).c_str())) {
            for (int i = 0; i < 6; i++) if (ImGui::Selectable(kN[i])) { const double m = (S.viewLo + S.viewHi) / 2, s = rate * kF[i]; S.viewLo = m - s / 2; S.viewHi = m + s / 2; }
            ImGui::EndCombo();
        }
        tip("Width shown (the scroll wheel zooms too)");
    }
    flowNext(10 * gUi);
    ImGui::TextDisabled("RBW"); flowNext(4 * gUi);
    {
        char lab[48]; snprintf(lab, sizeof lab, "%s", sx::freqText(rateOf(a) / (256 << S.fftIdx)).c_str());
        ImGui::SetNextItemWidth(100 * gUi);
        if (ImGui::BeginCombo("##sxrbw", lab)) {
            for (int i = 0; i <= 8; i++) {
                char b[48]; snprintf(b, sizeof b, "%s (%d bins)", sx::freqText(rateOf(a) / (256 << i)).c_str(), 256 << i);
                if (ImGui::Selectable(b, i == S.fftIdx)) { S.fftIdx = i; plat::prefs().setI("specxFft", i); S.peak.clear(); }
            }
            ImGui::EndCombo();
        }
        tip("Resolution bandwidth: the frequency step of the spectrum (a finer one is slower and shows a lower floor)");
    }
    flowNext(6 * gUi);
    ImGui::SetNextItemWidth(80 * gUi);
    if (ImGui::SliderInt("##sxavg", &S.avg, 1, 100, "avg %d")) plat::prefs().setI("specxAvg", S.avg);
    tip("Averaging: how many spectra are averaged (smoother floor, slower response)");
    flowNext(10 * gUi);
    if (ImGui::Checkbox("Peak hold", &S.peakHold)) plat::prefs().setB("specxPeak", S.peakHold);
    tip("Keep the highest level of every bin");
    flowNext(4 * gUi);
    if (ImGui::SmallButton("Reset##sxpk")) S.peak.clear();
    tip("Forget the held peaks");
    flowNext(10 * gUi);
    if (ImGui::Checkbox("Auto range", &S.autoRange)) plat::prefs().setB("specxAuto", S.autoRange);
    tip("Fit the level scale to the noise floor and the strongest signal");
    if (!S.autoRange) {
        flowNext(4 * gUi);
        ImGui::SetNextItemWidth(130 * gUi);
        ImGui::DragFloatRange2("##sxrng", &S.lo, &S.hi, 0.5f, -180, 20, "%.0f", "%.0f dB");
        tip("Bottom and top of the level scale (drag)");
    }
    flowNext(10 * gUi);
    if (ImGui::SmallButton("Clear##sxmk")) { S.markers.clear(); S.hasRegion = false; }
    flowNext(6 * gUi);
    if (ImGui::SmallButton("GRAVES##sxgr")) {   // the French space radar, 143.050 MHz: a +-3 kHz window, finest bins, the echoes of meteors and satellites show as short Doppler tracks
        constexpr double kGraves = 143.050e6;
        tuneRf(a, kGraves);
        S.viewLo = kGraves - 3000; S.viewHi = kGraves + 3000;
        S.fftIdx = 8; plat::prefs().setI("specxFft", 8);
        S.avg = 1; plat::prefs().setI("specxAvg", 1);
        S.peakHold = false; S.autoRange = true; S.peak.clear();
    }
    tip("GRAVES space radar view: 143.050 MHz, 6 kHz wide, the finest resolution, no averaging. Meteors and satellites crossing the beam show up as short bright tracks that bend with their Doppler shift. Needs an antenna for 2 m and a radio that tunes there.");
    tip("Remove the markers and the region");
    flowNext(10 * gUi);
    helpButton();
    flowBreak();
    gainControl(a);
    flowNext(10 * gUi);
    sampleRateControl(a);
    flowNext(10 * gUi);
    lnbControls(a);
    flowEnd();
    if (!S.freqErr.empty()) ImGui::TextColored(pal::warnAmber(), "%s", S.freqErr.c_str());
}

void sweepPanel(App& a, float h) {
    Sweep& w = S.sw;
    struct P { const char* n; double lo, hi; };
    static const P kP[] = {{"Satellite IF", 950, 2150}, {"Ku LNB IF (Bullseye, 739-2150)", 739, 2150}, {"FM broadcast", 87.5, 108}, {"Airband", 118, 137}, {"2 m", 144, 146}, {"70 cm", 430, 440},
                           {"ISM 433", 433.05, 434.79}, {"ISM 868", 863, 870}, {"L band", 1525, 1660}, {"Full range", 1, 6000}};
    double lo = w.loRf / 1e6, hi = w.hiRf / 1e6;
    ImGui::SetNextItemWidth(90 * gUi);
    if (ImGui::InputDouble("##sxswlo", &lo, 0, 0, "%.3f MHz")) w.loRf = lo * 1e6;
    tip("Sweep from (RF MHz)");
    ImGui::SameLine(); ImGui::TextDisabled("to"); ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    if (ImGui::InputDouble("##sxswhi", &hi, 0, 0, "%.3f MHz")) w.hiRf = hi * 1e6;
    tip("Sweep to (RF MHz)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * gUi);
    if (ImGui::BeginCombo("##sxswp", "Presets")) {
        for (const P& p : kP) if (ImGui::Selectable(p.n)) { w.loRf = p.lo * 1e6; w.hiRf = p.hi * 1e6; }
        ImGui::EndCombo();
    }
    tip("Ranges: satellite IF, Ku LNB IF 739-2150 MHz (Bullseye: QO-100 at 739.5 MHz), FM broadcast, airband, 2 m, 70 cm, ISM 433 / 868, L band, the radio's full range");
    ImGui::SameLine();
    if (!w.active) {
        if (ImGui::Button("Sweep a range")) {
            if (!a.engine.running()) startReceiver(a);
            double r0 = toRadio(w.loRf), r1 = toRadio(w.hiRf);
            if (r0 > r1) std::swap(r0, r1);
            w.centres = sx::sweepCentres(r0, r1, rateOf(a));
            w.pano.init(r0, r1, 4096);
            w.idx = 0; w.stepT = 0; w.active = true; w.passes = 0; w.found.clear();
        }
        tip("Step the radio across the range and stitch a panorama; signals are found on the way");
    } else {
        if (ImGui::Button("Stop sweep")) w.active = false;
        tip("Stop stepping (the panorama stays)");
        ImGui::SameLine();
        ImGui::TextDisabled("step %zu / %zu, pass %d", w.idx + 1, w.centres.size(), w.passes + 1);
    }
    if (w.pano.db.empty()) { ImGui::TextDisabled("No panorama yet."); return; }
    const float ph = std::max(60.f, (h - ImGui::GetFrameHeightWithSpacing() * 2) * 0.4f);
    const float tw = ImGui::GetContentRegionAvail().x * 0.4f;
    ImGui::BeginGroup();
    const float pw = ImGui::GetContentRegionAvail().x - tw - 8 * gUi;
    std::vector<double> xs, ys;
    for (size_t k = 0; k < w.pano.db.size(); k++) if (w.pano.seen[k]) { xs.push_back(toRf(w.pano.loHz + (k + 0.5) * w.pano.binHz) / 1e6); ys.push_back(w.pano.db[k]); }
    if (plt::BeginPlot("##sxpano", ImVec2(pw, ph), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("MHz", "dBFS");
        double x0 = toRf(w.pano.loHz) / 1e6, x1 = toRf(w.pano.hiHz) / 1e6;
        if (x0 > x1) std::swap(x0, x1);
        plt::SetupAxisLimits(plt::X1, x0, x1, plt::Cond_Always);
        plt::SetupAxisLimits(plt::Y1, S.lo, S.hi, plt::Cond_Always);
        plt::Spec s; s.LineColor = pal::accent();
        if (!xs.empty()) plt::PlotLine("panorama", xs.data(), ys.data(), (int)xs.size(), s);
        plt::EndPlot();
    }
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float wh = std::max(30.f, h - ph - ImGui::GetFrameHeightWithSpacing() * 2);
    ImGui::GetWindowDrawList()->AddRectFilled(p0, ImVec2(p0.x + pw, p0.y + wh), IM_COL32(8, 9, 10, 255));
    const bool inv = S.lnb.inverted();
    w.wf.draw(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + pw, p0.y + wh), inv ? 1.f : 0.f, inv ? 0.f : 1.f);
    ImGui::Dummy(ImVec2(pw, wh));
    ImGui::EndGroup();
    ImGui::SameLine();
    if (ImGui::BeginTable("##sxfound", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV, ImVec2(tw, h - ImGui::GetFrameHeightWithSpacing()))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Centre"); ImGui::TableSetupColumn("BW -3 dB"); ImGui::TableSetupColumn("Level"); ImGui::TableSetupColumn("Sym. rate");
        ImGui::TableHeadersRow();
        int i = 0;
        for (const auto& m : w.found) {
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            char b[64]; snprintf(b, sizeof b, "%.4f MHz##sxf%d", toRf(m.centreHz) / 1e6, i++);
            if (ImGui::Selectable(b, false, ImGuiSelectableFlags_SpanAllColumns)) {
                w.active = false;
                const double rf = toRf(m.centreHz), span = std::max(m.bw26Hz * 3, rateOf(a) / 64);
                tuneRf(a, rf); S.viewLo = rf - span / 2; S.viewHi = rf + span / 2;
                S.hasRegion = true; S.regLo = toRf(m.loHz); S.regHi = toRf(m.hiHz);
                if (S.regLo > S.regHi) std::swap(S.regLo, S.regHi);
            }
            tip("Tune there and measure it");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(sx::freqText(m.bw3Hz).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.1f dB", m.snrDb);
            ImGui::TableNextColumn();
            if (m.flatTop) ImGui::Text("%.3f Ms/s", m.symRateBest / 1e6); else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }
    {
        static std::string note;
        resexp::Table x; x.cols = {"time", "frequency_hz", "mode", "bandwidth_3db_hz", "level_db", "symbol_rate_sps"};
        const std::string now = resexp::stamp();
        for (const auto& m : w.found) x.add({now, resexp::num(toRf(m.centreHz), 0), "spectrum explorer", resexp::num(m.bw3Hz, 0), resexp::num(m.snrDb, 1), m.flatTop ? resexp::num(m.symRateBest, 0) : ""});
        resexp::saveButton(x, "explorer", note, "svsx");
        if (!note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", note.c_str()); }
    }
}

void tab(App& a) {
    loadState();
    strip(a);
    // keep the view on the band after a retune from elsewhere
    const double c = centreRadio(a), rate = rateOf(a);
    if (S.viewHi <= S.viewLo || c != S.lastCentre || rate != S.lastRate) {
        if (S.viewHi <= S.viewLo || rate != S.lastRate || S.lastCentre == 0) { bandRf(a, S.viewLo, S.viewHi); }
        else { const double sp = S.viewHi - S.viewLo, m = toRf(c); S.viewLo = m - sp / 2; S.viewHi = m + sp / 2; }
        S.lastCentre = c; S.lastRate = rate; S.peak.clear();
    }
    const float avail = ImGui::GetContentRegionAvail().y;
    const bool panel = S.hasRegion || S.sw.active || !S.sw.pano.db.empty();
    const float ph = panel ? std::min(avail * 0.42f, 260 * gUi) : 0;
    const float stripH = bandPlanStripHeight();
    const float rest = avail - ph - stripH - ImGui::GetStyle().ItemSpacing.y * (panel ? 3 : 2);
    {
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        drawBandPlanStrip(ImGui::GetWindowDrawList(), p0.x, p0.x + w, p0.y, stripH, S.viewLo / 1e6, S.viewHi / 1e6);
        ImGui::Dummy(ImVec2(w, stripH));
    }
    spectrumView(a, std::max(80.f, rest * 0.58f));
    waterfallView(a, std::max(40.f, rest * 0.42f - ImGui::GetStyle().ItemSpacing.y));
    if (!panel) return;
    ImGui::BeginChild("##sxpanel", ImVec2(0, ph), false);
    if (ImGui::BeginTabBar("##sxtabs")) {
        if (S.hasRegion && ImGui::BeginTabItem("Region")) {
            if (live(a)) {
                sx::SpecView v = view(a);
                double r0 = toRadio(S.regLo), r1 = toRadio(S.regHi);
                const sx::Measure m = sx::measure(v, std::min(r0, r1), std::max(r0, r1));
                if (m.valid) measurePanel(a, m, true); else ImGui::TextDisabled("Select a wider region inside the band.");
            } else ImGui::TextDisabled("Start the receiver to measure.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Sweep")) { sweepPanel(a, ImGui::GetContentRegionAvail().y); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
}

void list(App& a) {
    loadState();
    ImGui::TextDisabled("Bookmarks");
    if (S.marks.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)); ImGui::TextWrapped("Star (*) a frequency, or a selected region as a named range, to keep it here."); ImGui::PopStyleColor(); }
    for (size_t i = 0; i < S.marks.size(); i++) {
        ImGui::PushID((int)i);
        if (ImGui::SmallButton("x")) { S.marks.erase(S.marks.begin() + (long)i); saveMarks(); ImGui::PopID(); break; }
        tip("Forget this bookmark");
        ImGui::SameLine();
        {   // the colour chip; click it to change the colour
            const uint32_t c = S.marks[i].rgb;
            float col[3] = {((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f};
            if (ImGui::ColorEdit3("##sxc", col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
                S.marks[i].rgb = ((uint32_t)(col[0] * 255 + 0.5f) << 16) | ((uint32_t)(col[1] * 255 + 0.5f) << 8) | (uint32_t)(col[2] * 255 + 0.5f);
                saveMarks();
            }
            ImGui::SameLine();
        }
        char b[128];
        if (markRange(S.marks[i])) snprintf(b, sizeof b, "%s  %s – %s", S.marks[i].name.c_str(), sx::freqText(S.marks[i].rfHz).c_str(), sx::freqText(S.marks[i].rfHiHz).c_str());
        else snprintf(b, sizeof b, "%s  %s", S.marks[i].name.c_str(), sx::freqText(S.marks[i].rfHz).c_str());
        if (ImGui::Selectable(b)) {
            if (markRange(S.marks[i])) {
                S.viewLo = S.marks[i].rfHz; S.viewHi = S.marks[i].rfHiHz;
            }
            tuneRf(a, markMid(S.marks[i]));
        }
        tip(markRange(S.marks[i]) ? "Tune to the middle and show this range" : "Tune there");
        ImGui::PopID();
    }
    if (ImGui::SmallButton("Export")) {
        const std::string path = plat::saveFileDialog("bookmarks.csv");
        if (!path.empty()) {
            std::ofstream o(dect2::u8file(path));
            o << "name,frequency_hz,frequency_hi_hz,colour\n";
            for (auto& m : S.marks) {
                std::string n = m.name; for (char& ch : n) if (ch == ',' || ch == '\n' || ch == '\r') ch = ' ';
                char v[72]; snprintf(v, sizeof v, ",%.0f,%.0f,#%06X\n", m.rfHz, markRange(m) ? m.rfHiHz : 0.0, m.rgb & 0xFFFFFF);
                o << n << v;
            }
        }
    }
    tip("Save the bookmarks to a CSV file: name, frequency in Hz, high frequency (0 for a point), colour");
    ImGui::SameLine();
    if (ImGui::SmallButton("Import")) {
        const std::string path = plat::openFileDialog();
        if (!path.empty()) {
            std::ifstream in(dect2::u8file(path));
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::vector<std::string> p;
                for (size_t i = 0; i < line.size();) {
                    size_t j = line.find(',', i);
                    if (j == std::string::npos) j = line.size();
                    p.push_back(line.substr(i, j - i));
                    i = j + (j < line.size() ? 1 : 0);
                    if (j == line.size()) break;
                }
                if (p.size() < 2) continue;
                auto hex = [](const std::string& s) {
                    if (s.empty()) return false;
                    size_t k = s[0] == '#' ? 1 : 0;
                    if (s.size() - k != 6) return false;
                    for (; k < s.size(); k++) if (!std::isxdigit((unsigned char)s[k])) return false;
                    return true;
                };
                Bookmark m;
                int last = (int)p.size() - 1;
                if (hex(p[(size_t)last])) {
                    const char* cs = p[(size_t)last].c_str();
                    if (*cs == '#') cs++;
                    m.rgb = (uint32_t)strtoul(cs, nullptr, 16) & 0xFFFFFF;
                    last--;
                }
                if (last < 1) continue;
                m.name = p[0];
                m.rfHz = std::atof(p[1].c_str());
                if (m.rfHz <= 0) continue;   // the header line
                if (last >= 2) m.rfHiHz = std::atof(p[2].c_str());
                if (m.rfHiHz > 0 && m.rfHiHz < m.rfHz) std::swap(m.rfHz, m.rfHiHz);
                S.marks.push_back(m);
            }
            saveMarks();
        }
    }
    tip("Add the bookmarks from a CSV file written by Export");
    if (!S.markers.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("Markers");
        for (size_t i = 0; i < S.markers.size(); i++) {
            ImGui::Text("M%zu %s  %.1f dBFS", i + 1, sx::freqText(S.markers[i]).c_str(), levelAt(a, S.markers[i]));
            if (i > 0) { ImGui::SameLine(); ImGui::TextDisabled("(%+.1f kHz)", (S.markers[i] - S.markers[0]) / 1e3); }
        }
    }
}

void tick(App& a) {
    loadState();
    const bool run = a.engine.running();
    const int fft = 256 << S.fftIdx;
    if (run && (!S.wasRunning || S.pushedFft != fft || S.pushedAvg != S.avg)) {
        a.engine.specx().setFftSize(fft); a.engine.specx().setAveraging(S.avg);
        S.pushedFft = fft; S.pushedAvg = S.avg;
    }
    S.wasRunning = run;
    if (!run) S.sw.active = false;
    const dect2::SpecxTelemetry& t = a.rx.specx;
    if (!live(a) || t.seq == S.lastSeq) return;
    S.lastSeq = t.seq;
    // peak hold, auto range, waterfall
    if (S.peak.size() != t.spectrum.size()) S.peak = t.spectrum;
    else for (size_t k = 0; k < t.spectrum.size(); k++) S.peak[k] = std::max(S.peak[k], t.spectrum[k]);
    if (S.autoRange) {
        const float lo = t.floorDb - 10, hi = std::max(t.peakDb + 6, t.floorDb + 40);
        S.lo += (lo - S.lo) * 0.1f; S.hi += (hi - S.hi) * 0.1f;
    }
    if (S.lnb.inverted()) { std::vector<float> r(t.spectrum.rbegin(), t.spectrum.rend()); S.wf.push(r, S.lo, S.hi); }
    else S.wf.push(t.spectrum, S.lo, S.hi);
    // sweep: tune, wait until the averaging has filled after the retune, take the spectrum, next
    Sweep& w = S.sw;
    if (!w.active || w.centres.empty()) return;
    const double now = ImGui::GetTime();
    if (w.stepT == 0) { tuneFreq(a, w.centres[w.idx] / 1e6); w.stepT = now; w.stepSeq = t.seq; return; }
    if (now - w.stepT < 0.15 || t.seq < w.stepSeq + 2 || t.frames < (uint64_t)S.avg + 1) return;
    w.pano.add(view(a));
    w.panoEnbw = t.enbw;
    if (++w.idx >= w.centres.size()) {
        w.idx = 0; w.passes++;
        w.found = sx::detect(w.pano.view(w.panoEnbw));
        std::vector<float> row(w.pano.db);
        if (S.lnb.inverted()) std::reverse(row.begin(), row.end());
        w.wf.push(row, S.lo, S.hi);
    }
    w.stepT = 0;
}

void status(App& a) {
    StatusPanel panel;
    const bool on = live(a);
    lamp("Spectrum", on ? 1 : 0, (int)Ic::Wave); flowNext(12 * gUi);
    if (on) {
        const dect2::SpecxTelemetry& t = a.rx.specx;
        ImGui::Text("RBW %s  floor %.1f dBFS  peak %.1f dBFS  input %.1f dBFS", sx::freqText(t.rate / std::max(1, t.fftSize)).c_str(), t.floorDb, t.peakDb, t.powerDb);
        if (S.lnb.on) { flowNext(12 * gUi); ImGui::TextDisabled("converter %s", sx::freqText(S.lnb.loHz).c_str()); }
        if (S.sw.active) { flowNext(12 * gUi); ImGui::TextDisabled("sweeping"); }
    }
    flowEnd();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Spectrum explorer";
    if (live(a)) l2 = dect2::specxSummary(a.rx.specx);
}

} // namespace

void specxDrawBookmarks(ImDrawList* dl, double loHz, double hiHz, float x0, float x1, float y0, float y1) {
    loadState();
    drawMarks(dl, loHz, hiHz, x0, x1, y0, y1);
}

extern const ModeUi kSpecxUi;
const ModeUi kSpecxUi = {
    .sideTitle = "BOOKMARKS",
    .tabName = "Explorer",
    .tabIcon = Ic::Chart,
    .tab = tab,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};

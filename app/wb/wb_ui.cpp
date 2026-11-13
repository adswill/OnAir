// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal workbench: open a recording (audio or IQ), look at it (overview, waveform, spectrogram with cursors and a selection of time and band),
// measure it, demodulate a band and read the bits. The work is in core/src/wb_*.cpp; this file is the screens: loading, the plots with their
// mouse handling, the mode hooks. The other tabs are in wb_tabs.cpp.
#include "wb_state.h"
#include "ui/scale.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

using namespace dect2::wb;
using dect2::cf32;

namespace wbui {

State& S() { static State s; return s; }

static const int kFft[] = {256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
static const int kNFft = 9;
// development: files and settings from the command line (main.cpp)
static std::string gDevFile;
static double gDevRate = 0;
static int gDevTab = -1;
static struct Dev { bool sel = false; double t0 = 0, t1 = 0, f0 = 0, f1 = 0; int kind = -1; double baud = 0; bool meas = false, demod = false; } gDev;
static std::mutex gDropMu;
static std::string gDrop;

bool haveSource() { return S().src != nullptr; }
double rateHz() { return S().src ? S().src->rate() : 1; }

std::string fmtFreq(double hz, int d) {
    char b[48];
    const double a = std::fabs(hz);
    if (a >= 1e9) snprintf(b, sizeof b, "%.*f GHz", d, hz / 1e9);
    else if (a >= 1e6) snprintf(b, sizeof b, "%.*f MHz", d, hz / 1e6);
    else if (a >= 1e3) snprintf(b, sizeof b, "%.*f kHz", d, hz / 1e3);
    else snprintf(b, sizeof b, "%.*f Hz", std::max(0, d - 2), hz);
    return b;
}

std::string fmtTime(double s) {
    char b[48];
    const double a = std::fabs(s);
    if (a >= 60) snprintf(b, sizeof b, "%d:%06.3f", (int)(s / 60), std::fmod(s, 60.0));
    else if (a >= 1) snprintf(b, sizeof b, "%.3f s", s);
    else if (a >= 1e-3) snprintf(b, sizeof b, "%.2f ms", s * 1e3);
    else snprintf(b, sizeof b, "%.1f us", s * 1e6);
    return b;
}

void noSource(const char* what) { ImGui::TextDisabled("Open a recording on the Signal tab first (%s).", what); }

void selection(uint64_t& t0, uint64_t& t1, double& f0, double& f1) {
    State& s = S();
    const uint64_t len = s.src ? s.src->length() : 0;
    t0 = s.selT ? (uint64_t)std::max(0.0, std::min(s.sT0, s.sT1)) : 0;
    t1 = s.selT ? (uint64_t)std::min((double)len, std::max(s.sT0, s.sT1)) : len;
    if (s.selF) { f0 = std::min(s.sF0, s.sF1); f1 = std::max(s.sF0, s.sF1); } else { f0 = f1 = 0; }
}

void setSelection(double t0, double t1, bool band, double f0, double f1) {
    State& s = S();
    s.selT = t1 > t0; s.sT0 = t0; s.sT1 = t1;
    s.selF = band && f1 > f0; s.sF0 = f0; s.sF1 = f1;
}

static void fullView() {
    State& s = S();
    if (!s.src) return;
    s.v0 = 0; s.v1 = (double)s.src->length();
    s.vf0 = s.src->real() ? 0 : -s.src->rate() / 2;
    s.vf1 = s.src->rate() / 2;
}

static void loadPrefs() {
    State& s = S();
    if (s.loaded) return;
    s.loaded = true;
    s.fftIdx = (int)std::max(0L, std::min((long)kNFft - 1, plat::prefs().getI("wbFft", 2)));
    s.win = (int)std::max(0L, std::min((long)WinCount - 1, plat::prefs().getI("wbWin", WinHann)));
    s.overlapPct = (int)std::max(0L, std::min(90L, plat::prefs().getI("wbOvl", 50)));
    s.cmap = (int)std::max(0L, std::min(3L, plat::prefs().getI("wbCmap", 0)));
    s.autoRange = plat::prefs().getB("wbAuto", true);
    s.dbMin = (float)plat::prefs().getD("wbDbMin", -110);
    s.dbMax = (float)plat::prefs().getD("wbDbMax", -20);
    s.dp.kind = (int)std::max(0L, std::min((long)KindCount - 1, plat::prefs().getI("wbKind", KFm)));
    s.dp.baud = plat::prefs().getD("wbBaud", 1200);
}

void savePrefs() {
    State& s = S();
    plat::prefs().setI("wbFft", s.fftIdx); plat::prefs().setI("wbWin", s.win); plat::prefs().setI("wbOvl", s.overlapPct); plat::prefs().setI("wbCmap", s.cmap);
    plat::prefs().setB("wbAuto", s.autoRange); plat::prefs().setD("wbDbMin", s.dbMin); plat::prefs().setD("wbDbMax", s.dbMax);
    plat::prefs().setI("wbKind", s.dp.kind); plat::prefs().setD("wbBaud", s.dp.baud);
    plat::prefs().flush();
}

static void openSource(std::shared_ptr<Source> src) {
    State& s = S();
    s.mJob.stop(); s.dJob.stop();
    stopAudio();
    s.spec.reset(); s.ov.reset();
    s.src = std::move(src);
    s.haveMeas = false; s.haveDemod = false; s.selT = s.selF = false; s.curOn = false;
    s.img = SpecImage(); s.specSeq = 0; s.texSeq = 0; s.imgFresh = false; s.waveV0 = -1; s.waveOvDone = false;
    s.hits.clear(); s.hitsInv.clear(); s.bitsDirty = true; s.dr = DemodResult();
    if (!s.src) return;
    fullView();
    s.ov = std::make_unique<Overview>(s.src, 8192);
    s.spec = std::make_unique<SpecEngine>(s.src);
    // a first FFT size that gives about 1000 bins over the span
    const int bins = 1024;
    int best = 2;
    for (int i = 0; i < kNFft; i++) if (kFft[i] <= bins * 2 && kFft[i] >= bins) best = i;
    (void)best;
    s.dp.bwHz = 0; s.dp.centreHz = 0;
    s.dp.baud = std::max(1.0, s.dp.baud);
    char b[64];
    snprintf(b, sizeof b, "%.6g", s.src->rate());
    snprintf(s.rateBuf, sizeof s.rateBuf, "%s", b);
    snprintf(s.cfBuf, sizeof s.cfBuf, "%.0f", s.src->centerHz());
}

void loadFile(const std::string& path) {
    State& s = S();
    loadPrefs();
    snprintf(s.pathBuf, sizeof s.pathBuf, "%s", path.c_str());
    LoadOptions o;
    o.sampleRate = atof(s.rateBuf);
    o.centerHz = atof(s.cfBuf);
    o.stereoIq = s.stereoIq;
    o.bigEndian = s.bigEndian;
    o.audio = s.fmtSel == 6;
    o.format = s.fmtSel >= 1 && s.fmtSel <= 5 ? s.fmtSel - 1 : -1;
    // a recent rate field belongs to the last file: a name that says its rate, or a file that describes itself, wins when the field is the default
    std::string err;
    auto src = Source::open(path, o, err);
    if (!src) {
        s.err = err;
        s.note.clear();
        return;
    }
    s.err.clear();
    s.path = path;
    openSource(src);
}

static std::shared_ptr<Source> makeDemo() {
    const double rate = 250000;
    GenSpec a; a.kind = KFsk2; a.rate = rate; a.baud = 9600; a.nbits = 6000; a.snrDb = 200; a.offsetHz = 40000; a.leadSec = 0.30; a.tailSec = 0.45; a.seed = 11;
    GenSpec b; b.kind = KQpsk; b.rate = rate; b.baud = 12500; b.nbits = 14000; b.snrDb = 200; b.offsetHz = -55000; b.leadSec = 1.25; b.tailSec = 0.1; b.differential = true; b.seed = 12;
    GenSpec c; c.kind = KAsk; c.rate = rate; c.baud = 1000; c.nbits = 600; c.snrDb = 200; c.offsetHz = 90000; c.leadSec = 0.15; c.tailSec = 1.2; c.seed = 13;
    GenResult ra = generate(a), rb = generate(b), rc = generate(c);
    const size_t N = std::max({ra.x.size(), rb.x.size(), rc.x.size()});
    std::vector<cf32> x(N, cf32(0, 0));
    for (size_t i = 0; i < ra.x.size(); i++) x[i] += ra.x[i] * 0.7f;
    for (size_t i = 0; i < rb.x.size(); i++) x[i] += rb.x[i] * 0.4f;
    for (size_t i = 0; i < rc.x.size(); i++) x[i] += rc.x[i] * 0.3f;
    for (size_t i = 0; i < N; i++) x[i] += 0.08f * cf32((float)std::cos(2 * M_PI * -85000.0 * (double)i / rate), (float)std::sin(2 * M_PI * -85000.0 * (double)i / rate));   // a carrier
    addNoise(x, 4e-5, 5);
    return Source::fromSamples(std::move(x), rate, false, 433.92e6, "demo signal");
}

// ---- colours -----------------------------------------------------------------------------------------------------------------------

static uint32_t lutColour(int map, float t) {
    t = std::max(0.f, std::min(1.f, t));
    struct St { float p, r, g, b; };
    static const St turbo[] = {{0, 0.02f, 0.02f, 0.12f}, {0.2f, 0.10f, 0.20f, 0.55f}, {0.4f, 0.10f, 0.60f, 0.70f}, {0.6f, 0.45f, 0.85f, 0.35f}, {0.8f, 0.98f, 0.82f, 0.15f}, {1, 0.98f, 0.25f, 0.10f}};
    static const St viridis[] = {{0, 0.27f, 0.00f, 0.33f}, {0.25f, 0.23f, 0.32f, 0.55f}, {0.5f, 0.13f, 0.57f, 0.55f}, {0.75f, 0.37f, 0.79f, 0.38f}, {1, 0.99f, 0.91f, 0.14f}};
    static const St fire[] = {{0, 0, 0, 0}, {0.3f, 0.35f, 0.05f, 0.40f}, {0.55f, 0.80f, 0.20f, 0.20f}, {0.8f, 1.0f, 0.65f, 0.10f}, {1, 1, 1, 0.85f}};
    static const St grey[] = {{0, 0, 0, 0}, {1, 1, 1, 1}};
    const St* st = map == 0 ? turbo : map == 1 ? viridis : map == 2 ? fire : grey;
    const int n = map == 0 ? 6 : map == 1 ? 5 : map == 2 ? 5 : 2;
    int i = 0;
    while (i + 2 < n && t > st[i + 1].p) i++;
    const float f = (t - st[i].p) / std::max(1e-6f, st[i + 1].p - st[i].p);
    const float r = st[i].r + (st[i + 1].r - st[i].r) * f, g = st[i].g + (st[i + 1].g - st[i].g) * f, b = st[i].b + (st[i + 1].b - st[i].b) * f;
    return 0xFF000000u | (uint32_t)(std::max(0.f, std::min(1.f, b)) * 255) << 16 | (uint32_t)(std::max(0.f, std::min(1.f, g)) * 255) << 8 | (uint32_t)(std::max(0.f, std::min(1.f, r)) * 255);
}

static double niceStep(double span, int target) {
    const double raw = std::max(span, 1e-300) / std::max(1, target);
    const double p = std::pow(10.0, std::floor(std::log10(raw)));
    const double f = raw / p;
    return (f < 1.5 ? 1 : f < 3.5 ? 2 : f < 7.5 ? 5 : 10) * p;
}

// ---- the plots -----------------------------------------------------------------------------------------------------------------------

struct Geo {   // the plot area on the screen and what it shows
    ImVec2 p0, size;
    double t0, t1, f0, f1;
    float X(double t) const { return p0.x + (float)((t - t0) / (t1 - t0) * size.x); }
    float Y(double f) const { return p0.y + (float)((f1 - f) / (f1 - f0) * size.y); }
    double T(float x) const { return t0 + (double)(x - p0.x) / size.x * (t1 - t0); }
    double F(float y) const { return f1 - (double)(y - p0.y) / size.y * (f1 - f0); }
};

static void zoomTime(double factor, double anchor) {
    State& s = S();
    const double len = (double)s.src->length();
    double span = (s.v1 - s.v0) * factor;
    span = std::max(64.0, std::min(len, span));
    const double rel = (anchor - s.v0) / std::max(1.0, s.v1 - s.v0);
    s.v0 = anchor - rel * span; s.v1 = s.v0 + span;
    if (s.v0 < 0) { s.v1 -= s.v0; s.v0 = 0; }
    if (s.v1 > len) { s.v0 -= s.v1 - len; s.v1 = len; }
    s.v0 = std::max(0.0, s.v0);
}

static void zoomFreq(double factor, double anchor) {
    State& s = S();
    const double fmin = s.src->real() ? 0 : -s.src->rate() / 2, fmax = s.src->rate() / 2;
    double span = (s.vf1 - s.vf0) * factor;
    span = std::max(s.src->rate() / 4096.0, std::min(fmax - fmin, span));
    const double rel = (anchor - s.vf0) / std::max(1e-9, s.vf1 - s.vf0);
    s.vf0 = anchor - rel * span; s.vf1 = s.vf0 + span;
    if (s.vf0 < fmin) { s.vf1 += fmin - s.vf0; s.vf0 = fmin; }
    if (s.vf1 > fmax) { s.vf0 -= s.vf1 - fmax; s.vf1 = fmax; }
    s.vf0 = std::max(fmin, s.vf0);
}

static void pan(double dt, double df) {
    State& s = S();
    const double len = (double)s.src->length();
    const double fmin = s.src->real() ? 0 : -s.src->rate() / 2, fmax = s.src->rate() / 2;
    double span = s.v1 - s.v0;
    s.v0 = std::max(0.0, std::min(len - span, s.v0 + dt)); s.v1 = s.v0 + span;
    span = s.vf1 - s.vf0;
    s.vf0 = std::max(fmin, std::min(fmax - span, s.vf0 + df)); s.vf1 = s.vf0 + span;
}

static void drawTimeAxis(ImDrawList* dl, const Geo& g, float y, bool labels) {
    const double rate = rateHz();
    const double span = (g.t1 - g.t0) / rate;
    const double step = niceStep(span, std::max(2, (int)(g.size.x / (90 * gUi))));
    const double first = std::ceil(g.t0 / rate / step) * step;
    const ImU32 col = IM_COL32(150, 160, 170, 255), grid = IM_COL32(120, 130, 140, 40);
    for (double t = first; t <= g.t1 / rate + 1e-12; t += step) {
        const float x = g.X(t * rate);
        dl->AddLine(ImVec2(x, g.p0.y), ImVec2(x, g.p0.y + g.size.y), grid);
        if (labels) {
            char b[40];
            const double a = std::fabs(span);
            if (a < 1e-3) snprintf(b, sizeof b, "%.1f us", t * 1e6);
            else if (a < 1) snprintf(b, sizeof b, "%.2f ms", t * 1e3);
            else snprintf(b, sizeof b, "%.3f s", t);
            dl->AddText(ImVec2(x + 2, y), col, b);
        }
    }
}

static void drawFreqAxis(ImDrawList* dl, const Geo& g, float xRight) {
    State& s = S();
    const double step = niceStep(g.f1 - g.f0, std::max(2, (int)(g.size.y / (36 * gUi))));
    const double c = s.src->centerHz();
    const double first = std::ceil((g.f0 + c) / step) * step - c;
    const ImU32 col = IM_COL32(150, 160, 170, 255), grid = IM_COL32(120, 130, 140, 40);
    for (double f = first; f <= g.f1 + 1e-9; f += step) {
        const float y = g.Y(f);
        dl->AddLine(ImVec2(g.p0.x, y), ImVec2(g.p0.x + g.size.x, y), grid);
        char b[40];
        const double v = f + c;
        const double a = std::max(std::fabs(g.f0 + c), std::fabs(g.f1 + c));
        if (a >= 1e6) snprintf(b, sizeof b, "%.4f M", v / 1e6);
        else if (a >= 1e3 || step >= 1e3) snprintf(b, sizeof b, "%.3f k", v / 1e3);
        else snprintf(b, sizeof b, "%.1f", v);
        const ImVec2 ts = ImGui::CalcTextSize(b);
        dl->AddText(ImVec2(xRight - ts.x - 3, y - ts.y / 2), col, b);
    }
}

static std::vector<OverviewBlock> gOvCache;
static int gOvFrame = 0;

static void drawOverview(float w, float h, float axisL) {
    State& s = S();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##wbov", ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    const ImVec2 p0(o.x + axisL, o.y), p1(o.x + w, o.y + h);
    dl->AddRectFilled(p0, p1, IM_COL32(8, 9, 11, 255));
    dl->AddText(ImVec2(o.x + 2, o.y + 2), IM_COL32(130, 140, 150, 255), "overview");
    if (!s.ov) return;
    if (!s.ov->done() || gOvCache.empty() || ++gOvFrame > 30) { gOvCache = s.ov->snapshot(); gOvFrame = 0; }
    const double len = (double)s.src->length();
    const float pw = p1.x - p0.x;
    float peak = (float)std::max(1e-9, s.ov->peak());
    const int cols = std::max(1, (int)pw);
    const float mid = (p0.y + p1.y) / 2, hh = (p1.y - p0.y) / 2 - 2;
    const double bl = (double)s.ov->blockLen();
    for (int c = 0; c < cols; c++) {
        const size_t i0 = (size_t)((double)c * len / cols / bl), i1 = std::min(gOvCache.size(), std::max(i0 + 1, (size_t)((double)(c + 1) * len / cols / bl)));
        float m = 0;
        for (size_t i = i0; i < i1 && i < gOvCache.size(); i++) m = std::max(m, gOvCache[i].maxMag);
        const float hgt = std::max(0.5f, hh * std::sqrt(std::min(1.f, m / peak)));
        dl->AddLine(ImVec2(p0.x + c + 0.5f, mid - hgt), ImVec2(p0.x + c + 0.5f, mid + hgt), IM_COL32(86, 130, 170, 255));
    }
    auto X = [&](double t) { return p0.x + (float)(t / len * pw); };
    if (s.selT) dl->AddRectFilled(ImVec2(X(std::min(s.sT0, s.sT1)), p0.y), ImVec2(X(std::max(s.sT0, s.sT1)) + 1, p1.y), IM_COL32(230, 170, 60, 70));
    dl->AddRectFilled(ImVec2(X(s.v0), p0.y), ImVec2(std::max(X(s.v1), X(s.v0) + 2), p1.y), IM_COL32(255, 255, 255, 36));
    dl->AddRect(ImVec2(X(s.v0), p0.y), ImVec2(std::max(X(s.v1), X(s.v0) + 2), p1.y), IM_COL32(210, 220, 230, 200));
    if (!s.ov->done()) {
        char b[48]; snprintf(b, sizeof b, "reading %.0f %%", s.ov->progress() * 100);
        dl->AddText(ImVec2(p1.x - 90 * gUi, o.y + 2), IM_COL32(230, 180, 90, 255), b);
    }
    if (act || (hov && ImGui::IsMouseClicked(0))) {   // click or drag: the view centres there
        const double t = (ImGui::GetMousePos().x - p0.x) / pw * len;
        const double span = s.v1 - s.v0;
        s.v0 = std::max(0.0, std::min(len - span, t - span / 2)); s.v1 = s.v0 + span;
    }
    if (hov) ImGui::SetTooltip("The whole recording. Click or drag: move the view. The orange part is the time selection.");
}

static void drawWaveform(float w, float h, float axisL) {
    State& s = S();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##wbwave", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hov = ImGui::IsItemHovered();
    Geo g; g.p0 = ImVec2(o.x + axisL, o.y); g.size = ImVec2(w - axisL, h); g.t0 = s.v0; g.t1 = s.v1; g.f0 = -1; g.f1 = 1;
    dl->AddRectFilled(g.p0, ImVec2(g.p0.x + g.size.x, g.p0.y + g.size.y), IM_COL32(8, 9, 11, 255));
    const int cols = std::max(8, (int)g.size.x);
    const bool ovDone = s.ov && s.ov->done();
    if (s.waveV0 != s.v0 || s.waveV1 != s.v1 || s.waveCols != cols || (s.ov && (s.v1 - s.v0) > 6e6 && (!ovDone || !s.waveOvDone))) {
        waveformColumns(*s.src, s.ov.get(), (uint64_t)s.v0, (uint64_t)s.v1, cols, s.wave);
        s.waveV0 = s.v0; s.waveV1 = s.v1; s.waveCols = cols; s.waveOvDone = ovDone;
    }
    float amp = 1e-6f;
    for (int c = 0; c < cols; c++) amp = std::max({amp, std::fabs(s.wave.minI[(size_t)c]), std::fabs(s.wave.maxI[(size_t)c]), std::fabs(s.wave.minQ[(size_t)c]), std::fabs(s.wave.maxQ[(size_t)c])});
    amp *= 1.08f;
    const float mid = g.p0.y + g.size.y / 2, k = g.size.y / 2 / amp;
    dl->PushClipRect(g.p0, ImVec2(g.p0.x + g.size.x, g.p0.y + g.size.y), true);
    drawTimeAxis(dl, g, g.p0.y + g.size.y - 14 * gUi, false);
    dl->AddLine(ImVec2(g.p0.x, mid), ImVec2(g.p0.x + g.size.x, mid), IM_COL32(120, 130, 140, 70));
    const bool cplx = !s.src->real();
    for (int c = 0; c < cols; c++) {
        const float x = g.p0.x + (float)c * g.size.x / cols + 0.5f;
        if (cplx) dl->AddLine(ImVec2(x, mid - s.wave.maxQ[(size_t)c] * k), ImVec2(x, mid - s.wave.minQ[(size_t)c] * k - 1), IM_COL32(220, 140, 80, 190));
        dl->AddLine(ImVec2(x, mid - s.wave.maxI[(size_t)c] * k), ImVec2(x, mid - s.wave.minI[(size_t)c] * k - 1), IM_COL32(100, 160, 230, 220));
    }
    if (s.selT) dl->AddRectFilled(ImVec2(g.X(std::min(s.sT0, s.sT1)), g.p0.y), ImVec2(g.X(std::max(s.sT0, s.sT1)), g.p0.y + g.size.y), IM_COL32(230, 170, 60, 50));
    if (s.curOn) { const float x = g.X(s.curT); dl->AddLine(ImVec2(x, g.p0.y), ImVec2(x, g.p0.y + g.size.y), IM_COL32(120, 220, 160, 220)); }
    dl->PopClipRect();
    dl->AddText(ImVec2(o.x + 2, o.y + 2), IM_COL32(130, 140, 150, 255), cplx ? "I (blue)" : "level");
    if (cplx) dl->AddText(ImVec2(o.x + 2, o.y + 16 * gUi), IM_COL32(220, 140, 80, 255), "Q (orange)");
    char b[24]; snprintf(b, sizeof b, "%.3g", amp);
    dl->AddText(ImVec2(o.x + 2, o.y + h - 16 * gUi), IM_COL32(110, 120, 130, 255), b);
    ImGuiIO& io = ImGui::GetIO();
    if (hov) {
        const double t = g.T(io.MousePos.x);
        if (io.MouseWheel != 0) zoomTime(std::pow(0.8, io.MouseWheel), t);
        if (ImGui::IsMouseClicked(0)) { s.dragKind = 4; s.dragT0 = t; }
        if (ImGui::IsMouseClicked(1)) { s.dragKind = 2; s.dragP0 = io.MousePos; }
        ImGui::SetTooltip("%s", fmtTime(t / rateHz()).c_str());
    }
    if (s.dragKind == 4) {
        if (ImGui::IsMouseDown(0)) {
            const double t = std::max(0.0, std::min((double)s.src->length(), g.T(io.MousePos.x)));
            if (std::fabs(io.MousePos.x - g.X(s.dragT0)) > 3) { s.selT = true; s.sT0 = std::min(s.dragT0, t); s.sT1 = std::max(s.dragT0, t); }
        } else {
            if (!s.selT || std::fabs(s.sT1 - s.sT0) < 4) { s.curOn = true; s.curT = s.dragT0; }
            s.dragKind = 0;
        }
    }
}

static void recolour() {
    State& s = S();
    const SpecImage& im = s.img;
    if (im.w <= 0 || im.h <= 0 || !gGfx) return;
    if (!s.tex || s.texW != im.w || s.texH != im.h) {
        delete s.tex;
        s.tex = gGfx->createImage(im.w, im.h, 0xFF000000u);
        s.texW = im.w; s.texH = im.h;
    }
    s.rgba.resize((size_t)im.w * im.h);
    static uint32_t lut[4][256];
    static bool lutOk[4] = {false, false, false, false};
    if (!lutOk[s.cmap]) { for (int i = 0; i < 256; i++) lut[s.cmap][i] = lutColour(s.cmap, i / 255.f); lutOk[s.cmap] = true; }
    const float lo = s.dbMin, sc = 255.f / std::max(1.f, s.dbMax - s.dbMin);
    const uint32_t* L = lut[s.cmap];
    for (size_t i = 0; i < s.rgba.size(); i++) {
        const int k = (int)((im.db[i] - lo) * sc);
        s.rgba[i] = L[k < 0 ? 0 : k > 255 ? 255 : k];
    }
    s.tex->update(0, 0, im.w, im.h, s.rgba.data());
    s.texCmap = s.cmap; s.texMin = s.dbMin; s.texMax = s.dbMax; s.texSeq = s.specSeq;
}

static void drawSpectrogram(App& a, float w, float h, float axisL) {
    State& s = S();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float axisB = 20 * gUi;
    ImGui::InvisibleButton("##wbspec", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hov = ImGui::IsItemHovered();
    Geo g; g.p0 = ImVec2(o.x + axisL, o.y); g.size = ImVec2(w - axisL, h - axisB); g.t0 = s.v0; g.t1 = s.v1; g.f0 = s.vf0; g.f1 = s.vf1;
    // ask for the picture of what is in view
    SpecParams p;
    p.t0 = (uint64_t)s.v0; p.t1 = (uint64_t)s.v1;
    p.cols = std::max(32, std::min(2048, (int)g.size.x));
    p.fftSize = kFft[s.fftIdx]; p.window = s.win; p.overlap = s.overlapPct / 100.0;
    p.f0 = s.vf0; p.f1 = s.vf1;
    p.maxRows = std::max(128, std::min(1024, (int)(g.size.y * 1.25f)));
    s.spec->request(p);
    SpecImage fresh; uint64_t seq = s.specSeq;
    if (s.spec->poll(fresh, seq)) {
        s.img = std::move(fresh); s.specSeq = seq;
        if (s.autoRange) { s.dbMin = std::floor(s.img.p5 - 2); s.dbMax = std::ceil(s.img.p995 + 6); }
    }
    if (s.img.w > 0 && (s.texSeq != s.specSeq || s.texCmap != s.cmap || s.texMin != s.dbMin || s.texMax != s.dbMax)) recolour();
    dl->AddRectFilled(g.p0, ImVec2(g.p0.x + g.size.x, g.p0.y + g.size.y), IM_COL32(0, 0, 0, 255));
    dl->PushClipRect(g.p0, ImVec2(g.p0.x + g.size.x, g.p0.y + g.size.y), true);
    if (s.tex && s.img.w > 0) {   // the picture sits where its own time and band put it, so a stale one still lines up while the new one is made
        dl->AddImage(s.tex->texture(), ImVec2(g.X((double)s.img.p.t0), g.Y(s.img.fHi)), ImVec2(g.X((double)s.img.p.t1), g.Y(s.img.fLo)));
    }
    dl->PopClipRect();
    // the grid and the labels: frequencies left of the picture, times under it
    drawFreqAxis(dl, g, g.p0.x - 2);
    drawTimeAxis(dl, g, g.p0.y + g.size.y + 3, true);
    dl->PushClipRect(g.p0, ImVec2(g.p0.x + g.size.x, g.p0.y + g.size.y), true);
    // selection and cursor
    if (s.selT) {
        const float x0 = g.X(std::min(s.sT0, s.sT1)), x1 = g.X(std::max(s.sT0, s.sT1));
        const float y0 = s.selF ? g.Y(std::max(s.sF0, s.sF1)) : g.p0.y, y1 = s.selF ? g.Y(std::min(s.sF0, s.sF1)) : g.p0.y + g.size.y;
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(255, 200, 80, 38));
        dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(255, 200, 80, 230));
    } else if (s.selF) {
        const float y0 = g.Y(std::max(s.sF0, s.sF1)), y1 = g.Y(std::min(s.sF0, s.sF1));
        dl->AddRectFilled(ImVec2(g.p0.x, y0), ImVec2(g.p0.x + g.size.x, y1), IM_COL32(255, 200, 80, 38));
    }
    if (s.curOn) {
        const float x = g.X(s.curT), y = g.Y(s.curF);
        dl->AddLine(ImVec2(x, g.p0.y), ImVec2(x, g.p0.y + g.size.y), IM_COL32(120, 230, 170, 230));
        dl->AddLine(ImVec2(g.p0.x, y), ImVec2(g.p0.x + g.size.x, y), IM_COL32(120, 230, 170, 230));
    }
    const bool inside = hov && io.MousePos.x >= g.p0.x && io.MousePos.x <= g.p0.x + g.size.x && io.MousePos.y >= g.p0.y && io.MousePos.y <= g.p0.y + g.size.y;
    double mt = 0, mf = 0;
    if (inside) {
        mt = g.T(io.MousePos.x); mf = g.F(io.MousePos.y);
        dl->AddLine(ImVec2(io.MousePos.x, g.p0.y), ImVec2(io.MousePos.x, g.p0.y + g.size.y), IM_COL32(255, 255, 255, 70));
        dl->AddLine(ImVec2(g.p0.x, io.MousePos.y), ImVec2(g.p0.x + g.size.x, io.MousePos.y), IM_COL32(255, 255, 255, 70));
    }
    dl->PopClipRect();
    dl->AddRect(g.p0, ImVec2(g.p0.x + g.size.x, g.p0.y + g.size.y), IM_COL32(70, 76, 84, 255));
    if (s.spec->busy()) dl->AddText(ImVec2(g.p0.x + 6, g.p0.y + 4), IM_COL32(240, 190, 90, 255), "computing...");
    if (s.img.w > 0) {
        char tb[80];
        snprintf(tb, sizeof tb, "%d x %d, bin %s, %.0f ms", s.img.w, s.img.h, fmtFreq(s.img.binHz, 1).c_str(), s.img.secs * 1e3);
        const ImVec2 ts = ImGui::CalcTextSize(tb);
        dl->AddText(ImVec2(g.p0.x + g.size.x - ts.x - 6, g.p0.y + 4), IM_COL32(150, 160, 170, 160), tb);
    }
    // mouse
    const float rate = (float)rateHz();
    if (inside) {
        if (io.MouseWheel != 0) {
            if (io.KeyCtrl) zoomFreq(std::pow(0.8, io.MouseWheel), mf);
            else zoomTime(std::pow(0.8, io.MouseWheel), mt);
        }
        const bool left = ImGui::IsMouseClicked(0), pn = ImGui::IsMouseClicked(1) || ImGui::IsMouseClicked(2);
        if (left && ImGui::IsMouseDoubleClicked(0)) { s.selT = s.selF = false; }
        else if (left) { s.dragKind = 1; s.dragT0 = mt; s.dragF0 = mf; s.dragP0 = io.MousePos; }
        if (pn) { s.dragKind = 2; s.dragP0 = io.MousePos; }
        // readout as a tooltip near the pointer
        float lvl = -300;
        bool have = false;
        if (s.img.w > 0 && mt >= (double)s.img.p.t0 && mt < (double)s.img.p.t1 && mf >= s.img.fLo && mf < s.img.fHi) {
            const int c = std::min(s.img.w - 1, (int)((mt - (double)s.img.p.t0) / (double)(s.img.p.t1 - s.img.p.t0) * s.img.w));
            const int r = std::min(s.img.h - 1, (int)((s.img.fHi - mf) / (s.img.fHi - s.img.fLo) * s.img.h));
            lvl = s.img.db[(size_t)r * s.img.w + c]; have = true;
        }
        ImGui::BeginTooltip();
        ImGui::Text("%s   %s", fmtTime(mt / rate).c_str(), fmtFreq(mf + s.src->centerHz(), 4).c_str());
        if (have) ImGui::Text("%.1f dBFS", lvl);
        ImGui::EndTooltip();
    }
    if (s.dragKind == 1) {
        if (ImGui::IsMouseDown(0)) {
            if (std::fabs(io.MousePos.x - s.dragP0.x) + std::fabs(io.MousePos.y - s.dragP0.y) > 4) {
                const double t = std::max(0.0, std::min((double)s.src->length(), g.T(io.MousePos.x)));
                const double fmin = s.src->real() ? 0 : -s.src->rate() / 2, fmax = s.src->rate() / 2;
                const double f = std::max(fmin, std::min(fmax, g.F(io.MousePos.y)));
                s.selT = true; s.sT0 = std::min(s.dragT0, t); s.sT1 = std::max(s.dragT0, t);
                if (io.KeyShift) s.selF = false;
                else { s.selF = true; s.sF0 = std::min(s.dragF0, f); s.sF1 = std::max(s.dragF0, f); }
            }
        } else {
            if (std::fabs(io.MousePos.x - s.dragP0.x) + std::fabs(io.MousePos.y - s.dragP0.y) <= 4) { s.curOn = true; s.curT = s.dragT0; s.curF = s.dragF0; }
            s.dragKind = 0;
        }
    } else if (s.dragKind == 2) {
        if (ImGui::IsMouseDown(1) || ImGui::IsMouseDown(2)) {
            const ImVec2 d(io.MousePos.x - s.dragP0.x, io.MousePos.y - s.dragP0.y);
            pan(-(double)d.x / g.size.x * (s.v1 - s.v0), (double)d.y / g.size.y * (s.vf1 - s.vf0));
            s.dragP0 = io.MousePos;
        } else s.dragKind = 0;
    }
    (void)a;
}

static void loadBar(App& a) {
    State& s = S();
    const float avail = ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("File");
    ImGui::SameLine(0, 6 * gUi);
    ImGui::SetNextItemWidth(std::max(120.f * gUi, avail - 330 * gUi));
    bool enter = ImGui::InputText("##wbpath", s.pathBuf, sizeof s.pathBuf, ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The recording: .wav .mp3 .flac .ogg .m4a (audio), or IQ: .cs8 .cu8 .cf32 .ci16 .sigmf. You can also drop a file on the window.");
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("Open...")) { const std::string p = openFileDialog(); if (!p.empty()) loadFile(p); }
    ImGui::SameLine(0, 4 * gUi);
    if (ImGui::Button("Load") || enter) { if (s.pathBuf[0]) loadFile(s.pathBuf); }
    ImGui::SameLine(0, 4 * gUi);
    if (ImGui::Button("Demo signal")) {
        s.path = "demo signal"; s.err.clear();
        openSource(makeDemo());
        s.sT0 = 0;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A made-up recording to try the tools on: a 9600 Bd FSK burst, a 12500 Bd QPSK burst, an on-off keyed signal and a carrier, in noise.");
    // the format of a raw file
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Format");
    ImGui::SameLine(0, 6 * gUi);
    ImGui::SetNextItemWidth(110 * gUi);
    static const char* fm[] = {"automatic", "cs8 (int8 IQ)", "cu8 (uint8 IQ)", "cf32 (float IQ)", "ci16 (int16 IQ)", "cu16 (uint16 IQ)", "audio"};
    ImGui::Combo("##wbfmt", &s.fmtSel, fm, 7);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Raw IQ files carry no header: pick the sample format. Automatic uses the file name (.cs8 .cu8 .cf32 .ci16 .cu16), SigMF its own description.");
    ImGui::SameLine(0, 10 * gUi);
    ImGui::TextDisabled("Sample rate");
    ImGui::SameLine(0, 6 * gUi);
    ImGui::SetNextItemWidth(96 * gUi);
    ImGui::InputText("##wbrate", s.rateBuf, sizeof s.rateBuf, ImGuiInputTextFlags_CharsScientific);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Samples per second, for raw files that do not say it (a name such as capture_2Msps.cs8, a SigMF file or a WAV do).");
    ImGui::SameLine(0, 10 * gUi);
    ImGui::TextDisabled("Centre Hz");
    ImGui::SameLine(0, 6 * gUi);
    ImGui::SetNextItemWidth(96 * gUi);
    ImGui::InputText("##wbcf", s.cfBuf, sizeof s.cfBuf, ImGuiInputTextFlags_CharsScientific);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The radio frequency of the recording's centre, for absolute frequencies in the read-outs (0: show offsets).");
    ImGui::SameLine(0, 10 * gUi);
    const bool before = s.stereoIq;
    ImGui::Checkbox("stereo is I/Q", &s.stereoIq);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("An audio file with two channels: left = I, right = Q (a baseband IQ recording saved as a WAV). Off: stereo is mixed to a real signal.");
    if (before != s.stereoIq && s.src && s.src->channels() == 2 && s.src->isAudio()) {
        s.src->setStereoIq(s.stereoIq);
        // the signal changes from real to complex or back: the views start over
        const SpecParams keep; (void)keep;
        s.spec = std::make_unique<SpecEngine>(s.src);
        s.img = SpecImage(); s.specSeq = s.texSeq = 0; s.waveV0 = -1; s.selF = false; s.haveMeas = false;
        fullView();
    }
    if (!s.err.empty()) ImGui::TextColored(pal::badRed(), "%s", s.err.c_str());
    else if (s.src) { ImGui::TextDisabled("%s", s.src->description().c_str()); }
    (void)a;
}

static void controlsRow() {
    State& s = S();
    bool changed = false;
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("FFT"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(70 * gUi);
    char cur[16]; snprintf(cur, sizeof cur, "%d", kFft[s.fftIdx]);
    if (ImGui::BeginCombo("##wbfft", cur)) {
        for (int i = 0; i < kNFft; i++) { char b[16]; snprintf(b, sizeof b, "%d", kFft[i]); if (ImGui::Selectable(b, i == s.fftIdx)) { s.fftIdx = i; changed = true; } }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("FFT size: bigger = finer frequency, blurrier in time. Bin width = sample rate / FFT size: %s", s.src ? fmtFreq(s.src->rate() / kFft[s.fftIdx]).c_str() : "");
    ImGui::SameLine(0, 8 * gUi);
    ImGui::TextDisabled("Window"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(120 * gUi);
    if (ImGui::BeginCombo("##wbwin", windowName(s.win))) {
        for (int i = 0; i < WinCount; i++) if (ImGui::Selectable(windowName(i), i == s.win)) { s.win = i; changed = true; }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, 8 * gUi);
    ImGui::TextDisabled("Overlap"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(80 * gUi);
    if (ImGui::SliderInt("##wbovl", &s.overlapPct, 0, 90, "%d %%")) changed = true;
    ImGui::SameLine(0, 8 * gUi);
    ImGui::TextDisabled("Colours"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(84 * gUi);
    static const char* cm[] = {"Turbo", "Viridis", "Fire", "Grey"};
    if (ImGui::Combo("##wbcm", &s.cmap, cm, 4)) changed = true;
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Checkbox("auto", &s.autoRange)) { changed = true; if (s.autoRange && s.img.w > 0) { s.dbMin = std::floor(s.img.p5 - 2); s.dbMax = std::ceil(s.img.p995 + 6); } }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pick the colour range from the levels in view (5 % and 99.5 % points).");
    ImGui::SameLine(0, 6 * gUi);
    ImGui::SetNextItemWidth(60 * gUi);
    if (ImGui::DragFloat("##wbmin", &s.dbMin, 0.5f, -200, s.dbMax - 3, "%.0f dB")) { s.autoRange = false; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Level at the dark end of the colours (dBFS)");
    ImGui::SameLine(0, 3 * gUi);
    ImGui::SetNextItemWidth(60 * gUi);
    if (ImGui::DragFloat("##wbmax", &s.dbMax, 0.5f, s.dbMin + 3, 20, "%.0f dB")) { s.autoRange = false; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Level at the bright end of the colours (dBFS)");
    if (changed) savePrefs();
    // zoom
    ImGui::SameLine(0, 12 * gUi);
    if (ImGui::Button("Full")) fullView();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the whole recording and the whole band");
    ImGui::SameLine(0, 3 * gUi);
    if (ImGui::Button("To selection") && s.selT) {
        s.v0 = std::min(s.sT0, s.sT1); s.v1 = std::max(s.sT0, s.sT1) + 1;
        if (s.selF) { const double m = (s.sF1 - s.sF0) * 0.15; s.vf0 = std::min(s.sF0, s.sF1) - m; s.vf1 = std::max(s.sF0, s.sF1) + m; }
        const double fmin = s.src->real() ? 0 : -s.src->rate() / 2, fmax = s.src->rate() / 2;
        s.vf0 = std::max(fmin, s.vf0); s.vf1 = std::min(fmax, s.vf1);
        s.v0 = std::max(0.0, s.v0 - (s.v1 - s.v0) * 0.1);
    }
    ImGui::SameLine(0, 3 * gUi);
    if (ImGui::Button("-")) zoomTime(1.6, (s.v0 + s.v1) / 2);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Zoom out in time (mouse wheel over a plot; Ctrl + wheel zooms the frequency)");
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("+")) zoomTime(0.625, (s.v0 + s.v1) / 2);
    ImGui::SameLine(0, 3 * gUi);
    if (ImGui::Button("Clear selection")) { s.selT = s.selF = false; s.curOn = false; }
}

static bool numField(const char* id, double& v, double scale, const char* fmt, float w = 90) {
    double shown = v / scale;
    ImGui::SetNextItemWidth(w * gUi);
    if (ImGui::InputDouble(id, &shown, 0, 0, fmt, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) { v = shown * scale; return true; }
    return false;
}

static void selectionRow() {
    State& s = S();
    const double rate = rateHz();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Selection  time");
    ImGui::SameLine(0, 5 * gUi);
    double t0 = s.sT0 / rate, t1 = s.sT1 / rate;
    if (!s.selT) { t0 = 0; t1 = 0; }
    bool ch = false;
    if (numField("##wbs0", t0, 1, "%.5f", 84)) ch = true;
    ImGui::SameLine(0, 3 * gUi); ImGui::TextDisabled("to"); ImGui::SameLine(0, 3 * gUi);
    if (numField("##wbs1", t1, 1, "%.5f s", 90)) ch = true;
    if (ch && t1 > t0) { s.selT = true; s.sT0 = std::max(0.0, t0 * rate); s.sT1 = std::min((double)s.src->length(), t1 * rate); }
    ImGui::SameLine(0, 12 * gUi);
    ImGui::TextDisabled("band");
    ImGui::SameLine(0, 5 * gUi);
    double c = s.src->centerHz();
    double f0 = s.selF ? s.sF0 + c : 0, f1 = s.selF ? s.sF1 + c : 0;
    bool fc = false;
    const double unit = (c != 0 || std::fabs(s.vf1) >= 1e6) ? 1e6 : 1e3;
    const char* fmtA = unit == 1e6 ? "%.5f" : "%.3f";
    if (numField("##wbf0", f0, unit, fmtA, 90)) fc = true;
    ImGui::SameLine(0, 3 * gUi); ImGui::TextDisabled("to"); ImGui::SameLine(0, 3 * gUi);
    if (numField("##wbf1", f1, unit, unit == 1e6 ? "%.5f MHz" : "%.3f kHz", 100)) fc = true;
    if (fc && f1 > f0) { s.selF = true; s.sF0 = f0 - c; s.sF1 = f1 - c; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The band to measure and demodulate, as absolute frequencies (offsets from the centre when no centre is set). Drag in the spectrogram to set it.");
}

static void readout() {
    State& s = S();
    const double rate = rateHz();
    char b[400];
    int n = 0;
    if (s.curOn) {
        float lvl = -300;
        if (s.img.w > 0 && s.curT >= (double)s.img.p.t0 && s.curT < (double)s.img.p.t1 && s.curF >= s.img.fLo && s.curF < s.img.fHi) {
            const int cc = std::min(s.img.w - 1, (int)((s.curT - (double)s.img.p.t0) / (double)(s.img.p.t1 - s.img.p.t0) * s.img.w));
            const int rr = std::min(s.img.h - 1, (int)((s.img.fHi - s.curF) / (s.img.fHi - s.img.fLo) * s.img.h));
            lvl = s.img.db[(size_t)rr * s.img.w + cc];
        }
        n += snprintf(b + n, sizeof b - (size_t)n, "cursor %s, %s", fmtTime(s.curT / rate).c_str(), fmtFreq(s.curF + s.src->centerHz(), 4).c_str());
        if (lvl > -299) n += snprintf(b + n, sizeof b - (size_t)n, ", %.1f dBFS", lvl);
        n += snprintf(b + n, sizeof b - (size_t)n, "     ");
    }
    if (s.selT) n += snprintf(b + n, sizeof b - (size_t)n, "selection %s long (%.0f samples)", fmtTime(std::fabs(s.sT1 - s.sT0) / rate).c_str(), std::fabs(s.sT1 - s.sT0));
    if (s.selF) n += snprintf(b + n, sizeof b - (size_t)n, "%s%s wide, centre %s", s.selT ? ", " : "selection ", fmtFreq(std::fabs(s.sF1 - s.sF0)).c_str(), fmtFreq((s.sF0 + s.sF1) / 2 + s.src->centerHz(), 4).c_str());
    if (s.curOn && s.selT) {   // a cursor inside the selection: the distance to its start
        (void)0;
    }
    if (n == 0) snprintf(b, sizeof b, "click: cursor     drag: select time and band     Shift + drag: time only     wheel: zoom time     Ctrl + wheel: zoom frequency     right-drag: pan     double click: clear");
    ImGui::TextDisabled("%s", b);
}

static void signalTab(App& a) {
    State& s = S();
    loadBar(a);
    if (!s.src) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Signal workbench: take a recording apart without a radio. Open an audio file (WAV, MP3, FLAC, OGG, M4A: a real signal, or a stereo file as I/Q) or an IQ file "
                            "(.cs8 .cu8 .cf32 .ci16 .sigmf), or press Demo signal. Then look at the spectrogram, select a time range and a band, measure it (peaks, bandwidth, "
                            "SNR, symbol rate, bursts), demodulate it (AM, FM, SSB, CW, ASK, FSK, AFSK, PSK, MSK) and read the bits.");
        ImGui::PopTextWrapPos();
        return;
    }
    controlsRow();
    selectionRow();
    readout();
    const ImVec2 av = ImGui::GetContentRegionAvail();
    const float axisL = 66 * gUi;
    const float ovH = 46 * gUi, wvH = 78 * gUi;
    const float specH = std::max(160.f * gUi, av.y - ovH - wvH - 8 * gUi);
    drawOverview(av.x, ovH, axisL);
    drawWaveform(av.x, wvH, axisL);
    drawSpectrogram(a, av.x, specH, axisL);
}

// ---- the mode hooks ------------------------------------------------------------------------------------------------------------------

static void tab(App& a) {
    State& s = S();
    loadPrefs();
    if (!gDevFile.empty()) {   // development: the file from --wbfile, once
        const std::string f = gDevFile; gDevFile.clear();
        if (gDevRate > 0) snprintf(s.rateBuf, sizeof s.rateBuf, "%.0f", gDevRate);
        loadFile(f);
        if (gDevTab >= 0) s.tab = gDevTab;
        if (s.src && gDev.sel) setSelection(gDev.t0 * s.src->rate(), gDev.t1 * s.src->rate(), gDev.f1 > gDev.f0, gDev.f0, gDev.f1);
        if (s.src && gDev.kind >= 0) { s.dp.kind = gDev.kind; if (gDev.baud > 0) s.dp.baud = gDev.baud; }
        if (s.src && gDev.meas) startMeasure();
        if (s.src && gDev.demod) startDemod();
    }
    subNav("wbv", s.tab, {"Signal", "Measure", "Demodulate", "Bits", "Export"});
    switch (s.tab) {
    case 0: signalTab(a); break;
    case 1: drawMeasure(a); break;
    case 2: drawDemod(a); break;
    case 3: drawBits(a); break;
    default: drawExport(a); break;
    }
}

static void receiver(App& a) {
    State& s = S();
    auto kv = [&](const char* k, const char* fmt, ...) {
        ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
        char b[400];
        va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
        ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
    };
    if (!s.src) { ImGui::TextDisabled("no recording open"); return; }
    kv("file", "%s", s.src->name().c_str());
    kv("signal", "%s", s.src->description().c_str());
    kv("sample rate", "%.3f kHz, %.3f s, %llu samples", s.src->rate() / 1e3, s.src->seconds(), (unsigned long long)s.src->length());
    if (s.ov && s.ov->done()) {
        const cf32 m = s.ov->mean();
        kv("level", "rms %.1f dBFS, peak %.3f", s.ov->rmsDb(), s.ov->peak());
        kv("DC offset", "I %+.5f  Q %+.5f", m.real(), m.imag());
    } else if (s.ov) kv("level", "reading the file: %.0f %%", s.ov->progress() * 100);
    if (s.img.w > 0) kv("spectrogram", "%d x %d, %.2f s to draw, bin %.2f Hz", s.img.w, s.img.h, s.img.secs, s.img.binHz);
}

static void list(App& a) {
    State& s = S();
    (void)a;
    if (!s.src) { ImGui::TextDisabled("no recording open"); return; }
    if (!s.haveMeas || s.meas.peaks.empty()) { ImGui::TextDisabled("measure a selection to list its peaks"); return; }
    if (!ImGui::BeginTable("##wbpk", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Frequency", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Level"); ImGui::TableSetupColumn("Width");
    int i = 0;
    for (const Peak& p : s.meas.peaks) {
        ImGui::TableNextRow();
        ImGui::PushID(i++);
        ImGui::TableNextColumn();
        char b[64]; snprintf(b, sizeof b, "%s", fmtFreq(p.freqHz + s.src->centerHz(), 3).c_str());
        if (ImGui::Selectable(b, false, ImGuiSelectableFlags_SpanAllColumns)) { s.curOn = true; s.curF = p.freqHz; if (!s.curT) s.curT = (s.v0 + s.v1) / 2; }
        ImGui::TableNextColumn(); ImGui::TextDisabled("%.0f dB", p.levelDb);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", fmtFreq(p.widthHz, 1).c_str());
        ImGui::PopID();
    }
    ImGui::EndTable();
}

static void tick(App& a) {
    loadPrefs();
    State& s = S();
    static int lastFrame = -100;
    if (ImGui::GetFrameCount() - lastFrame > 3) gForceTab = "Workbench";   // the mode was just selected: its own tab first
    lastFrame = ImGui::GetFrameCount();
    if (s.mJob.finished.exchange(false)) { s.mJob.join(); s.mJob.running = false; if (s.mRes) { s.meas = *s.mRes; s.haveMeas = s.meas.valid; if (!s.meas.valid) s.note = s.meas.why; } }
    if (s.dJob.finished.exchange(false)) {
        s.dJob.join(); s.dJob.running = false;
        if (s.dRes) { s.dr = *s.dRes; s.haveDemod = s.dr.ok; s.bitsDirty = true; s.bpage = 0; s.note = s.dr.ok ? s.dr.msg : "demodulation failed: " + s.dr.msg; }
    }
    audioTick(a);
}

static void status(App& a) {
    State& s = S();
    (void)a;
    ImGui::AlignTextToFramePadding();
    lamp("File", s.src ? 1 : 0); flowNext(10 * gUi);
    lamp("Measure", s.mJob.running ? 2 : s.haveMeas ? 1 : 0); flowNext(10 * gUi);
    lamp("Demod", s.dJob.running ? 2 : s.haveDemod ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::PushFont(a.mono, 0);
    ImGui::TextUnformatted(s.src ? s.src->description().c_str() : "no recording open");
    ImGui::PopFont();
}

static void summary(const App& a, std::string& l1, std::string& l2) {
    State& s = S();
    (void)a;
    l1 = "Signal workbench";
    l2 = s.src ? s.src->name() : "open a recording";
}

} // namespace wbui

// the entry points main.cpp calls
void wbQueueDrop(const char* path) { std::lock_guard<std::mutex> lk(wbui::gDropMu); wbui::gDrop = path; }
void wbDropTick(App& a) {
    std::string p;
    { std::lock_guard<std::mutex> lk(wbui::gDropMu); p.swap(wbui::gDrop); }
    if (p.empty()) return;
    selectMode(a, 73);
    gForceTab = "Workbench";
    wbui::loadFile(p);
    wbui::S().tab = 0;
}
void wbDevSelect(double t0, double t1, double f0, double f1) { wbui::gDev.sel = true; wbui::gDev.t0 = t0; wbui::gDev.t1 = t1; wbui::gDev.f0 = f0; wbui::gDev.f1 = f1; }
void wbDevDemod(int kind, double baud, bool m, bool d) { wbui::gDev.kind = kind; wbui::gDev.baud = baud; wbui::gDev.meas = m; wbui::gDev.demod = d; }
void wbDevOpen(const std::string& path, double rate, int tab) { wbui::gDevFile = path; wbui::gDevRate = rate; wbui::gDevTab = tab; }

extern const ModeUi kWbUi;
const ModeUi kWbUi = {
    .sideTitle = "PEAKS",
    .tabName = "Workbench",
    .tabIcon = Ic::Signal,
    .tab = wbui::tab,
    .receiver = wbui::receiver,
    .list = wbui::list,
    .status = wbui::status,
    .summary = wbui::summary,
    .tick = wbui::tick,
};

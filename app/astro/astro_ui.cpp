// Radio astronomy screens: the line preset (H I, the OH lines, custom), FFT size and integration time with a progress bar, the reference
// (none, a captured off-source spectrum, or frequency switching), the baseline polynomial and the line window, the chart in frequency or
// radial velocity and in dB or relative temperature with the peak marker, the drift-scan waterfall of finished integrations, CSV export,
// and a short "?" guide.
#include "app.h"
#include "dect2/astro_dsp.h"
#include "dect2/astro_rx.h"
#include "dect2/astro_tel.h"
#include "dect2/u8file.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

namespace as = dect2::astro;

const int kFft[] = {1024, 2048, 4096, 8192, 16384};
const char* const kFftName[] = {"1024", "2048", "4096", "8192", "16384"};
const double kInt[] = {1, 5, 10, 30, 60, 120, 300, 600, 1800, 3600};
const char* const kIntName[] = {"1 s", "5 s", "10 s", "30 s", "1 min", "2 min", "5 min", "10 min", "30 min", "1 h"};
const char* const kRefName[] = {"none", "reference (off source)", "frequency switching"};
const char* const kYName[] = {"relative dB", "temperature K", "raw dBFS"};

struct Row { std::time_t when; std::vector<float> v; double peakKms, peakRatio, snr; };

struct State {
    int line = 0;                 // index in astro::lines(), or the count for custom
    float customMhz = 1420.4058f;
    int fft = 2, integ = 2;
    int ref = 0;                  // 0 none, 1 captured reference, 2 frequency switching
    float swKhz = 1250;
    bool fold = true;
    int degree = 3;
    bool window = true, dc = true, xVel = true;
    float winLo = -150, winHi = 150, smoothKms = 2;
    int yMode = 0;
    float tsys = 100;
    std::vector<double> refSpec;
    std::string refNote, note;
    uint64_t seenSeq = 0, seenDone = 0;
    as::Result res;
    std::vector<double> on;
    as::Params par;
    std::vector<Row> rows;        // drift scan: one per finished integration, newest last
    static constexpr int kCols = 512, kRows = 160;
    gfx::Image* wfImg = nullptr;
    bool wfDirty = false, loaded = false;
    float dragV0 = 0; bool dragging = false;
};
State S;

void tip(const char* t) { if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", t); }

bool live(const App& a) { return a.rx.standard == 49 && !a.rx.astro.avg.empty(); }

double restHz() {
    int n = 0;
    const as::Line* l = as::lines(n);
    return S.line >= 0 && S.line < n ? l[S.line].restHz : S.customMhz * 1e6;
}

void load() {
    if (S.loaded) return;
    S.loaded = true;
    auto& p = plat::prefs();
    int n = 0; as::lines(n);
    S.line = (int)std::clamp(p.getI("astroLine", 0), 0L, (long)n);
    S.customMhz = (float)p.getD("astroCustom", 1420.4058);
    S.fft = (int)std::clamp(p.getI("astroFft", 2), 0L, 4L);
    S.integ = (int)std::clamp(p.getI("astroInt", 2), 0L, 9L);
    S.degree = (int)std::clamp(p.getI("astroDeg", 3), -1L, 9L);
    S.xVel = p.getB("astroXVel", true);
    S.yMode = (int)std::clamp(p.getI("astroY", 0), 0L, 2L);
    S.tsys = (float)p.getD("astroTsys", 100);
}

void save() {
    auto& p = plat::prefs();
    p.setI("astroLine", S.line); p.setD("astroCustom", S.customMhz); p.setI("astroFft", S.fft); p.setI("astroInt", S.integ);
    p.setI("astroDeg", S.degree); p.setB("astroXVel", S.xVel); p.setI("astroY", S.yMode); p.setD("astroTsys", S.tsys);
}

// the receiver settings (cheap and idempotent: called every frame)
void push(App& a) {
    auto& r = a.engine.astro();
    r.setFftSize(kFft[S.fft]);
    r.setIntegration(kInt[S.integ]);
    r.setFreqSwitch(S.ref == 2, S.swKhz * 1e3, 1.0);
}

as::Params params(const App& a) {
    const auto& t = a.rx.astro;
    as::Params p;
    p.centerHz = a.tune.centerHz;
    p.rate = t.rate;
    p.restHz = restHz();
    p.dcHalf = S.dc ? std::max(1, t.fftSize / 2048) : -1;
    p.degree = S.degree;
    p.window = S.window; p.winLoKms = S.winLo; p.winHiKms = S.winHi;
    const double binKms = t.fftSize && t.rate > 0 ? t.rate / t.fftSize / p.restHz * as::kC : 1;
    p.smoothBins = std::max(1, (int)std::lround(S.smoothKms / std::max(1e-6, binKms)));
    p.switchHz = S.ref == 2 && t.switching ? t.switchHz : 0;
    p.fold = S.fold;
    return p;
}

std::vector<double> offFor(const dect2::AstroTelemetry& t, bool lastOne) {
    if (S.ref == 2 && t.switching) return lastOne ? t.lastOff : t.avgOff;
    if (S.ref == 1 && S.refSpec.size() == t.avg.size()) return S.refSpec;
    return {};
}

void ingest(App& a) {
    const auto& t = a.rx.astro;
    if (!live(a) || t.seq == S.seenSeq) return;
    S.seenSeq = t.seq;
    S.par = params(a);
    S.on = t.avg;
    S.res = as::process(S.on, offFor(t, false), S.par);
    if (t.completed < S.seenDone) S.seenDone = 0;   // the receiver restarted
    if (t.completed > S.seenDone && !t.last.empty()) {   // a finished integration: one row of the drift scan
        S.seenDone = t.completed;
        const as::Result r = as::process(t.last, offFor(t, true), S.par);
        Row row; row.when = std::time(nullptr); row.peakKms = r.peakKms; row.peakRatio = r.peakRatio; row.snr = r.snr;
        const int n = (int)r.ratio.size();
        row.v.assign(State::kCols, 0.f);
        for (int c = 0; c < State::kCols && n; c++) {
            const int k0 = c * n / State::kCols, k1 = std::max(k0 + 1, (c + 1) * n / State::kCols);
            double s = 0; for (int k = k0; k < k1; k++) s += r.ratio[(size_t)k];
            row.v[(size_t)c] = (float)(s / (k1 - k0));
        }
        S.rows.push_back(std::move(row));
        if ((int)S.rows.size() > State::kRows) S.rows.erase(S.rows.begin());
        S.wfDirty = true;
    }
}

double yOf(int i) {   // the chart value of bin i
    if (S.yMode == 2) return 10 * std::log10(std::max(1e-30, S.on[(size_t)i]));
    const double r = S.res.ratio[(size_t)i];
    if (S.yMode == 1) return r * S.tsys;
    return 10 * std::log10(std::max(1e-6, 1 + r));
}
const char* yUnit() { return S.yMode == 1 ? "K" : S.yMode == 2 ? "dBFS" : "dB"; }

void helpButton() {
    if (ImGui::SmallButton("?##ashelp")) ImGui::OpenPopup("##ashelpp");
    tip("How to see the hydrogen line");
    if (ImGui::BeginPopup("##ashelpp")) {
        ImGui::PushTextWrapPos(460 * gUi);
        ImGui::TextUnformatted("Seeing the 21 cm hydrogen line of the Milky Way");
        ImGui::Separator();
        ImGui::BulletText("You need: an antenna (a horn, a small dish, or a simple \"can\" feed: a tin about 15 cm across with a quarter-wave probe), "
                          "a low-noise amplifier (LNA, noise figure under 1 dB) right at the antenna, and a band-pass filter for 1420 MHz after it. "
                          "A bias-tee or the radio's antenna power can feed the LNA.");
        ImGui::BulletText("Point at the Milky Way (the galactic plane: Cygnus, Cassiopeia, Aquila, Sagittarius). Pointing at the zenith works too "
                          "when the plane passes overhead (a drift scan: leave it, the waterfall shows the sky going by).");
        ImGui::BulletText("Integrate: 10 s shows a strong plane; a minute or more for weaker parts. Use a reference (point away from the plane, "
                          "capture, point back) or frequency switching to take out the band shape, then a baseline fit (degree 2 to 4) "
                          "outside the line window.");
        ImGui::BulletText("What to expect: one or a few humps a few hundred kHz wide (tens of km/s), a few percent above the noise with a "
                          "small system, near 1420.4 MHz; their velocities show the spiral arms moving relative to us.");
        ImGui::BulletText("Velocity is the radio convention v = c (f0 - f) / f0, topocentric: no LSR correction is applied. The Earth's orbit "
                          "and rotation and the Sun's motion shift it by up to about +-30 km/s depending on date, time and direction.");
        ImGui::BulletText("Keep \"Remove DC\" on: the radio's own centre spike is bridged. Raise the gain until the noise floor rises "
                          "clearly when the LNA is powered, without overload.");
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
}

void exportCsv(App& a) {
    if (!live(a) || S.res.ratio.empty()) { S.note = "nothing to save yet"; return; }
    char nm[64];
    const std::time_t now = std::time(nullptr);
    std::strftime(nm, sizeof nm, "hline_%Y%m%d_%H%M%S.csv", std::localtime(&now));
    const std::string path = saveFileDialog(nm);
    if (path.empty()) return;
    const std::string csv = as::toCsv(S.res, S.on, S.par, a.rx.astro.doneSec);
    FILE* f = dect2::u8fopen(path, "wb");
    S.note = f && fwrite(csv.data(), 1, csv.size(), f) == csv.size() ? "saved " + path : "could not write " + path;
    if (f) fclose(f);
}

void controls(App& a) {
    int nl = 0;
    const as::Line* ln = as::lines(nl);
    bool changed = false;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Line"); ImGui::SameLine();
    ImGui::SetNextItemWidth(170 * gUi);
    if (ImGui::BeginCombo("##asline", S.line < nl ? ln[S.line].name : "custom")) {
        for (int i = 0; i <= nl; i++)
            if (ImGui::Selectable(i < nl ? ln[i].name : "custom", S.line == i)) {
                S.line = i; changed = true;
                if (i < nl) tuneFreq(a, ln[i].restHz / 1e6); else tuneFreq(a, S.customMhz);
            }
        ImGui::EndCombo();
    }
    tip("The rest frequency the velocity is counted from; picking one tunes the radio there");
    if (S.line == nl) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100 * gUi);
        ImGui::InputFloat("MHz##ascust", &S.customMhz, 0, 0, "%.4f");
        if (ImGui::IsItemDeactivatedAfterEdit()) { tuneFreq(a, S.customMhz); changed = true; }
    }
    flowNext(10 * gUi);
    ImGui::TextDisabled("FFT"); ImGui::SameLine();
    ImGui::SetNextItemWidth(80 * gUi);
    changed |= ImGui::Combo("##asfft", &S.fft, kFftName, 5);
    tip("Bins across the band: more bins = finer resolution, the same sensitivity per km/s after smoothing");
    flowNext(10 * gUi);
    ImGui::TextDisabled("Integrate"); ImGui::SameLine();
    ImGui::SetNextItemWidth(80 * gUi);
    changed |= ImGui::Combo("##asint", &S.integ, kIntName, 10);
    tip("Seconds of samples averaged into one spectrum; then the next one starts (the drift scan below)");
    flowNext(8 * gUi);
    {
        const auto& t = a.rx.astro;
        char ov[64];
        snprintf(ov, sizeof ov, live(a) ? "%.0f / %.0f s" : "stopped", t.doneSec, t.integrationSec);
        ImGui::ProgressBar(live(a) ? t.progress : 0.f, ImVec2(140 * gUi, 0), ov);
    }
    flowNext(6 * gUi);
    if (ImGui::SmallButton("Restart")) { a.engine.astro().reset(); S.rows.clear(); S.seenDone = 0; S.wfDirty = true; }
    tip("Start the integration again (and clear the drift scan)");
    flowNext(6 * gUi);
    helpButton();
    flowEnd();

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Reference"); ImGui::SameLine();
    ImGui::SetNextItemWidth(170 * gUi);
    ImGui::Combo("##asref", &S.ref, kRefName, 3);
    tip("none: the spectrum over its median; reference: (on - off) / off with a captured off-source spectrum; frequency switching: the "
        "radio alternates every second between its frequency and one shifted by the offset (needs a radio)");
    if (S.ref == 1) {
        flowNext(6 * gUi);
        if (ImGui::SmallButton("Capture off") && live(a)) {
            const auto& t = a.rx.astro;
            S.refSpec = t.avg;
            char b[96]; snprintf(b, sizeof b, "reference: %.0f s, %d bins", t.doneSec, t.fftSize);
            S.refNote = b;
        }
        tip("Point the antenna away from the line (cold sky, off the galactic plane), wait for the integration, then capture");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", S.refSpec.empty() ? "no reference" : S.refNote.c_str());
        if (!S.refSpec.empty() && live(a) && S.refSpec.size() != a.rx.astro.avg.size()) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, .6f, .3f, 1), "(other FFT size)"); }
    } else if (S.ref == 2) {
        flowNext(6 * gUi);
        ImGui::SetNextItemWidth(90 * gUi);
        ImGui::InputFloat("kHz##assw", &S.swKhz, 0, 0, "%.0f");
        tip("The second phase: the radio this far above (the line moves by this much)");
        ImGui::SameLine();
        ImGui::Checkbox("Fold", &S.fold);
        tip("Put the negative image of the line back onto it (average of the two)");
        if (live(a) && !a.rx.astro.switching) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, .6f, .3f, 1), "needs a radio"); }
    }
    flowNext(12 * gUi);
    ImGui::TextDisabled("Baseline"); ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    changed |= ImGui::SliderInt("##asdeg", &S.degree, -1, 9, S.degree < 0 ? "off" : "degree %d");
    tip("Polynomial fitted outside the line window and taken away (flattens the band shape)");
    flowNext(6 * gUi);
    ImGui::Checkbox("Window", &S.window);
    tip("Exclude this velocity range from the baseline fit and look for the peak in it; right-drag on the chart to mark it");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60 * gUi); ImGui::InputFloat("##aswl", &S.winLo, 0, 0, "%.0f");
    ImGui::SameLine(); ImGui::TextDisabled("to"); ImGui::SameLine();
    ImGui::SetNextItemWidth(60 * gUi); ImGui::InputFloat("km/s##aswh", &S.winHi, 0, 0, "%.0f");
    flowNext(10 * gUi);
    ImGui::Checkbox("Remove DC", &S.dc);
    tip("Bridge the radio's centre spike (a few bins)");
    flowNext(10 * gUi);
    ImGui::TextDisabled("Smooth"); ImGui::SameLine();
    ImGui::SetNextItemWidth(80 * gUi);
    ImGui::SliderFloat("##assm", &S.smoothKms, 0, 20, "%.1f km/s");
    flowEnd();

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("X"); ImGui::SameLine();
    if (ImGui::RadioButton("frequency", !S.xVel)) { S.xVel = false; changed = true; } ImGui::SameLine();
    if (ImGui::RadioButton("velocity", S.xVel)) { S.xVel = true; changed = true; }
    flowNext(12 * gUi);
    ImGui::TextDisabled("Y"); ImGui::SameLine();
    ImGui::SetNextItemWidth(130 * gUi);
    changed |= ImGui::Combo("##asy", &S.yMode, kYName, 3);
    if (S.yMode == 1) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi);
        ImGui::InputFloat("K Tsys##astsys", &S.tsys, 0, 0, "%.0f");
        tip("System temperature: the relative excess (on - off) / off times Tsys gives the line's antenna temperature");
        if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    }
    flowNext(12 * gUi);
    if (ImGui::SmallButton("Save CSV")) exportCsv(a);
    tip("Frequency, velocity, power, the processed spectrum and the baseline, one row per bin");
    if (!S.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", S.note.c_str()); }
    flowEnd();
    if (changed) save();
    // settings changed since the last report: process the current one again
    if (live(a)) { const as::Params p = params(a); if (p.degree != S.par.degree || p.window != S.par.window || p.winLoKms != S.par.winLoKms || p.winHiKms != S.par.winHiKms || p.smoothBins != S.par.smoothBins || p.dcHalf != S.par.dcHalf || p.fold != S.par.fold || p.restHz != S.par.restHz) S.seenSeq = 0; }
}

void chart(App& a, float h) {
    const bool on = live(a) && !S.res.ratio.empty();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float axisW = 52 * gUi, lh = ImGui::GetTextLineHeight();
    const float l = p0.x + axisW, r = p0.x + w - 6 * gUi, top = p0.y + lh / 2 + 2, bot = p0.y + h - lh - 6 * gUi;
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(10, 11, 16, 255));
    const ImU32 grid = IM_COL32(40, 44, 56, 255), txt = IM_COL32(130, 136, 150, 255);
    ImGui::InvisibleButton("##aschart", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    if (!on) {
        dl->AddText(ImVec2(l + 10, top + 10), txt, a.engine.running() ? "integrating ..." : "start the receiver to integrate the spectrum (\"?\" for how to)");
        return;
    }
    const int n = (int)S.res.ratio.size(), e = (int)(S.par.edge * n);
    const int k0 = std::max(1, e / 2), k1 = n - 1 - std::max(1, e / 2);   // show a little of the edges, not the roll-off
    // x: bins k0 .. k1 (frequency rises left to right; velocity falls with frequency, so the velocity axis runs right to left in value)
    auto X = [&](double k) { return (float)(l + (k - k0) / std::max(1.0, (double)(k1 - k0)) * (r - l)); };
    auto kOfX = [&](float x) { return k0 + (x - l) / std::max(1.f, r - l) * (k1 - k0); };
    // y range: the values in view, a little margin
    std::vector<double> ys;
    ys.reserve((size_t)(k1 - k0 + 1));
    for (int k = k0; k <= k1; k++) ys.push_back(yOf(k));
    std::vector<double> srt(ys);
    std::sort(srt.begin(), srt.end());
    double ylo = srt[(size_t)(srt.size() * 0.002)], yhi = srt[std::min(srt.size() - 1, (size_t)(srt.size() * 0.999))];
    const double pad = std::max(1e-9, (yhi - ylo) * 0.08);
    ylo -= pad; yhi += pad;
    auto Y = [&](double v) { return (float)std::clamp(bot - (v - ylo) / (yhi - ylo) * (bot - top), (double)top, (double)bot); };
    char b[96];
    for (int i = 0; i <= 5; i++) {
        const double v = ylo + (yhi - ylo) * i / 5;
        dl->AddLine(ImVec2(l, Y(v)), ImVec2(r, Y(v)), grid);
        snprintf(b, sizeof b, std::fabs(yhi - ylo) < 0.05 ? "%.3f" : std::fabs(yhi - ylo) < 2 ? "%.2f" : "%.1f", v);
        dl->AddText(ImVec2(p0.x + 3, Y(v) - lh / 2), txt, b);
    }
    dl->AddText(ImVec2(p0.x + 3, p0.y + h - lh - 2), txt, yUnit());
    // x ticks
    const int nt = std::max(2, (int)((r - l) / (90 * gUi)));
    for (int i = 0; i <= nt; i++) {
        const double k = k0 + (k1 - k0) * (double)i / nt;
        const float x = X(k);
        dl->AddLine(ImVec2(x, top), ImVec2(x, bot), grid);
        const double hz = S.par.centerHz + (k - n / 2) * S.par.rate / n;
        if (S.xVel) snprintf(b, sizeof b, "%.0f", as::velocityKms(hz, S.par.restHz)); else snprintf(b, sizeof b, "%.3f", hz / 1e6);
        const float tw = ImGui::CalcTextSize(b).x;
        dl->AddText(ImVec2(std::clamp(x - tw / 2, l, r - tw), bot + 3 * gUi), txt, b);
    }
    {
        const char* ax = S.xVel ? "km/s (radio, topocentric)" : "MHz";
        const float tw = ImGui::CalcTextSize(ax).x;
        dl->AddText(ImVec2(r - tw - 2, top + 2), txt, ax);
    }
    // the line window
    if (S.par.window) {
        const double f1 = as::freqForVelocity(S.par.winLoKms, S.par.restHz), f2 = as::freqForVelocity(S.par.winHiKms, S.par.restHz);
        const double b1 = (std::min(f1, f2) - S.par.centerHz) / (S.par.rate / n) + n / 2, b2 = (std::max(f1, f2) - S.par.centerHz) / (S.par.rate / n) + n / 2;
        const float x1 = std::clamp(X(b1), l, r), x2 = std::clamp(X(b2), l, r);
        if (x2 > x1) dl->AddRectFilled(ImVec2(x1, top), ImVec2(x2, bot), IM_COL32(90, 90, 160, 40));
    }
    // the rest frequency
    { const double kr = (S.par.restHz - S.par.centerHz) / (S.par.rate / n) + n / 2; if (kr >= k0 && kr <= k1) dl->AddLine(ImVec2(X(kr), top), ImVec2(X(kr), bot), IM_COL32(120, 120, 200, 140)); }
    // the trace: mean of the bins under each pixel column
    const int cols = std::max(2, (int)(r - l));
    std::vector<ImVec2> pts((size_t)cols);
    for (int c = 0; c < cols; c++) {
        const int a0 = k0 + (int)((long)(k1 - k0) * c / cols), a1 = std::max(a0 + 1, k0 + (int)((long)(k1 - k0) * (c + 1) / cols));
        double s = 0; for (int k = a0; k < a1; k++) s += ys[(size_t)(k - k0)];
        pts[(size_t)c] = ImVec2(l + c, Y(s / (a1 - a0)));
    }
    dl->AddPolyline(pts.data(), cols, IM_COL32(120, 200, 255, 255), 0, 1.3f * gUi);
    if (S.yMode == 2 && S.par.degree >= 0) { /* raw power: no baseline overlay (it lives in ratio units) */ }
    // peak
    if (S.res.peak >= 0) {
        const float x = X(S.res.peak), y = Y(yOf(S.res.peak));
        const ImU32 c = IM_COL32(255, 220, 90, 255);
        dl->AddLine(ImVec2(x, top), ImVec2(x, bot), IM_COL32(255, 220, 90, 110));
        dl->AddTriangleFilled(ImVec2(x - 5, y - 10), ImVec2(x + 5, y - 10), ImVec2(x, y - 1), c);
        snprintf(b, sizeof b, "peak %.4f MHz  %+.1f km/s  %.2f%%  SNR %.1f", S.res.peakHz / 1e6, S.res.peakKms, 100 * S.res.peakRatio, S.res.snr);
        dl->AddText(ImVec2(l + 6, top + 2), c, b);
    }
    // hover read-out, right-drag marks the window
    if (ImGui::IsItemHovered()) {
        const float mx = ImGui::GetIO().MousePos.x;
        const double k = std::clamp((double)kOfX(mx), (double)k0, (double)k1);
        const double hz = S.par.centerHz + (k - n / 2) * S.par.rate / n, v = as::velocityKms(hz, S.par.restHz);
        ImGui::SetTooltip("%.4f MHz\n%+.1f km/s\n%.4g %s\nright-drag: mark the line window", hz / 1e6, v, yOf((int)std::lround(k)), yUnit());
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { S.dragging = true; S.dragV0 = (float)v; }
    }
    if (S.dragging) {
        const float mx = ImGui::GetIO().MousePos.x;
        const double hz = S.par.centerHz + (std::clamp((double)kOfX(mx), (double)k0, (double)k1) - n / 2) * S.par.rate / n;
        const float v = (float)as::velocityKms(hz, S.par.restHz);
        S.winLo = std::min(S.dragV0, v); S.winHi = std::max(S.dragV0, v); S.window = true;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) S.dragging = false;
    }
}

void waterfall(App& a, float h) {
    (void)a;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(6, 7, 10, 255));
    if (S.rows.empty()) dl->AddText(ImVec2(p0.x + 52 * gUi + 10, p0.y + 6), IM_COL32(110, 116, 128, 255), "drift scan: one row per finished integration");
    else if (S.wfImg) {
        // the image holds kRows rows, newest at the top; only the filled part is shown, stretched over the height
        const float fill = (float)S.rows.size() / State::kRows;
        const float n = S.res.ratio.empty() ? 1.f : (float)S.res.ratio.size(), e = S.par.edge / 2;
        dl->AddImage(S.wfImg->texture(), ImVec2(p0.x + 52 * gUi, p0.y), ImVec2(p0.x + w - 6 * gUi, p0.y + h), ImVec2(e, 0), ImVec2(1 - e, fill));
        (void)n;
    }
    ImGui::Dummy(ImVec2(w, h));
    if (ImGui::IsItemHovered() && !S.rows.empty()) {
        const int row = (int)((ImGui::GetIO().MousePos.y - p0.y) / std::max(1.f, h) * S.rows.size());
        const Row& rw = S.rows[S.rows.size() - 1 - (size_t)std::clamp(row, 0, (int)S.rows.size() - 1)];
        char t[32]; std::strftime(t, sizeof t, "%H:%M:%S", std::localtime(&rw.when));
        ImGui::SetTooltip("%s\npeak %+.1f km/s, %.2f%%, SNR %.1f", t, rw.peakKms, 100 * rw.peakRatio, rw.snr);
    }
}

void renderWaterfall() {
    if (!S.wfDirty || !gGfx) return;
    if (!S.wfImg) S.wfImg = gGfx->createImage(State::kCols, State::kRows, 0xFF000000u);
    std::vector<uint32_t> px((size_t)State::kCols * State::kRows, 0xFF000000u);
    // one colour scale for all rows: from a little under the median to the strongest value
    std::vector<float> all;
    for (const auto& r : S.rows) all.insert(all.end(), r.v.begin() + State::kCols / 16, r.v.end() - State::kCols / 16);
    if (!all.empty()) {
        std::sort(all.begin(), all.end());
        const float lo = all[all.size() / 10], hi = std::max(lo + 1e-9f, all[all.size() - 1 - all.size() / 500]);
        for (size_t i = 0; i < S.rows.size(); i++) {
            const Row& r = S.rows[S.rows.size() - 1 - i];
            for (int c = 0; c < State::kCols; c++) px[i * State::kCols + (size_t)c] = Waterfall::jet(std::clamp((r.v[(size_t)c] - lo) / (hi - lo), 0.f, 1.f));
        }
    }
    S.wfImg->update(0, 0, State::kCols, State::kRows, px.data());
    S.wfDirty = false;
}

void tick(App& a) {
    load();
    push(a);
    ingest(a);
}

void tab(App& a) {
    load();
    ingest(a);
    renderWaterfall();
    controls(a);
    const float avail = ImGui::GetContentRegionAvail().y;
    chart(a, std::max(140 * gUi, avail * 0.64f));
    waterfall(a, std::max(40 * gUi, ImGui::GetContentRegionAvail().y - 2));
}

void list(App& a) {
    (void)a;
    if (S.rows.empty()) { ImGui::TextDisabled("no finished integration yet"); return; }
    ImGui::TextDisabled("time      peak km/s   excess   SNR");
    for (size_t i = S.rows.size(); i-- > 0;) {
        const Row& r = S.rows[i];
        char t[32]; std::strftime(t, sizeof t, "%H:%M:%S", std::localtime(&r.when));
        ImGui::Text("%s  %+7.1f  %6.2f%%  %5.1f", t, r.peakKms, 100 * r.peakRatio, r.snr);
    }
}

void receiver(App& a) {
    const auto& t = a.rx.astro;
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "integrating" : "stopped"); return; }
    ImGui::TextWrapped("%s", dect2::astroSummary(t).c_str());
    ImGui::TextWrapped("Resolution %.0f Hz = %.2f km/s per bin; span %.3f MHz = %.0f km/s", t.rate / t.fftSize, t.rate / t.fftSize / restHz() * as::kC,
                       t.rate / 1e6, t.rate / restHz() * as::kC);
    ImGui::TextWrapped("Reference: %s%s", kRefName[S.ref], S.ref == 1 ? (S.refSpec.empty() ? " (not captured)" : (" - " + S.refNote).c_str()) : S.ref == 2 ? (t.switching ? " (running)" : " (needs a radio)") : "");
    if (S.res.peak >= 0) ImGui::TextWrapped("Peak %.4f MHz, %+.1f km/s, %.3f%% over the reference, SNR %.1f (noise %.3f%% rms)", S.res.peakHz / 1e6, S.res.peakKms, 100 * S.res.peakRatio, S.res.snr, 100 * S.res.rms);
    ImGui::TextDisabled("Velocities: radio convention, topocentric (no LSR correction, up to about +-30 km/s)");
}

void status(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine();
    ImGui::TextUnformatted(live(a) ? dect2::astroSummary(a.rx.astro).c_str() : (a.engine.running() ? "integrating" : "stopped"));
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Radio astronomy";
    if (live(a)) {
        char b[128];
        if (S.res.peak >= 0) snprintf(b, sizeof b, "%.0f%% of %.0f s, peak %+.1f km/s (SNR %.1f)", 100.0 * a.rx.astro.progress, a.rx.astro.integrationSec, S.res.peakKms, S.res.snr);
        else snprintf(b, sizeof b, "%.0f%% of %.0f s", 100.0 * a.rx.astro.progress, a.rx.astro.integrationSec);
        l2 = b;
    }
}

void synth(App& a, bool& changed) {
    dect2::SynthConfig& sc = a.tune.synth;
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("noise with a sloped band and a weak hydrogen line at -42 km/s, 20 dB under the noise"); ImGui::PopTextWrapPos(); }
    bool off = sc.modeOpt[0] == 1;
    if (ImGui::Checkbox("Off source (no line)", &off)) { sc.modeOpt[0] = off ? 1 : 0; changed = true; }
    tip("For trying the reference: capture with this on, then turn it off");
}

} // namespace

extern const ModeUi kAstroUi;
const ModeUi kAstroUi = {
    .sideTitle = "INTEGRATIONS",
    .tabName = "Astronomy",
    .tabIcon = Ic::Star,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .synth = synth,
    .tick = tick,
};

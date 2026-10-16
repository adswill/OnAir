// Ionospheric sounder screens: the sounder picker and timing lock, the band coverage strip (what the radio has captured of 0 .. 30 MHz,
// the span now, the columns the shown sounder was heard in), the ionogram (group range against frequency) with a read-out under the
// mouse, the timeline of sweeps per sounder, PNG and CSV export, the list of sounders, and a short "?" guide. Scaling (iono_scale.h): the
// echo traces drawn over the ionogram, a panel beside it with the scaled values (foE, foF2, M(3000)F2 ... or the path's distance, modes
// and MUF), the sounder's identification, the bands open now on each path, and the history of foF2 / MUF with CSV export.
#include "app.h"
#include "dect2/iono_dsp.h"
#include "dect2/iono_gen.h"
#include "dect2/iono_rx.h"
#include "dect2/iono_scale.h"
#include "dect2/iono_tel.h"
#include "dect2/u8file.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

namespace io = dect2::iono;

const double kRangeKm[] = {1500, 3000};
const char* const kRangeName[] = {"0 - 1500 km", "0 - 3000 km"};
const ImU32 kSounderCol[] = {IM_COL32(255, 200, 90, 255), IM_COL32(110, 200, 255, 255), IM_COL32(150, 230, 120, 255), IM_COL32(240, 120, 200, 255),
                             IM_COL32(200, 160, 255, 255), IM_COL32(255, 140, 110, 255)};

struct State {
    int sel = -1;                 // the sounder shown (index in the telemetry's list), -1 the latest heard
    bool locked = true;
    int range = 1;
    float dbLo = 0, dbHi = 18;
    bool traces = true;           // the echo traces over the ionogram
    std::string note;
    gfx::Image* img = nullptr;
    uint64_t imgSeq = 0;
    bool loaded = false;
};
State S;

void tip(const char* t) { if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", t); }
bool live(const App& a) { return a.rx.standard == 53 && a.rx.iono.state == 1; }
ImU32 sounderCol(int id) { return kSounderCol[(size_t)((id - 1) % 6 + 6) % 6]; }

void load() {
    if (S.loaded) return;
    S.loaded = true;
    auto& p = plat::prefs();
    S.locked = p.getB("ionoLocked", true);
    S.range = (int)std::clamp(p.getI("ionoRange", 1), 0L, 1L);
    S.dbHi = (float)std::clamp(p.getD("ionoDbHi", 18.0), 3.0, 40.0);
    S.traces = p.getB("ionoTraces", true);
}
void save() {
    auto& p = plat::prefs();
    p.setB("ionoLocked", S.locked); p.setI("ionoRange", S.range); p.setD("ionoDbHi", S.dbHi); p.setB("ionoTraces", S.traces);
}

std::string clock(double t, bool withDate = false) {   // stream time: UTC with a radio, seconds from the start for a file
    char b[48];
    if (t > 1e8) {
        const std::time_t s = (std::time_t)std::floor(t);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &s);
#else
        gmtime_r(&s, &tm);
#endif
        std::strftime(b, sizeof b, withDate ? "%Y-%m-%d %H:%M:%S UTC" : "%H:%M:%S", &tm);
    } else snprintf(b, sizeof b, "%.1f s", t);
    return b;
}

std::string sounderName(const dect2::IonoSounder& s) {
    char b[96];
    if (s.periodSec > 0) snprintf(b, sizeof b, "#%d  %.0f kHz/s, every %.0f s at +%.0f s", s.id, s.rateHz / 1e3, s.periodSec, s.offsetSec);
    else snprintf(b, sizeof b, "#%d  %.0f kHz/s (period not known yet)", s.id, s.rateHz / 1e3);
    return b;
}

uint32_t cellCol(float db) {
    if (std::isnan(db)) return 0xFF000000u;
    return Waterfall::jet(std::clamp((db - S.dbLo) / std::max(0.5f, S.dbHi - S.dbLo), 0.f, 1.f));
}

int rowsShown() { return std::min(io::kRows, (int)std::lround(kRangeKm[S.range] / io::groupRangeKm(io::kDelayBin))); }

// the ionogram as pixels, top row = the largest range shown
void pixels(const dect2::IonoTelemetry& t, std::vector<uint32_t>& px, int& w, int& h) {
    w = io::kCols; h = rowsShown();
    px.assign((size_t)w * h, 0xFF000000u);
    if (t.ionoDb.size() != (size_t)io::kRows * io::kCols) return;
    for (int y = 0; y < h; y++) {
        const int row = h - 1 - y;
        for (int c = 0; c < w; c++) px[(size_t)y * w + c] = cellCol(t.ionoDb[(size_t)row * io::kCols + (size_t)c]);
    }
}

void renderImage(const App& a) {
    if (!gGfx) return;
    if (!S.img) S.img = gGfx->createImage(io::kCols, io::kRows, 0xFF000000u);
    const uint64_t key = a.rx.iono.seq * 64 + (uint64_t)S.range * 8 + (uint64_t)S.dbHi;
    if (key == S.imgSeq) return;
    S.imgSeq = key;
    std::vector<uint32_t> px;
    int w, h;
    pixels(a.rx.iono, px, w, h);
    S.img->update(0, 0, w, h, px.data());
}

void helpButton() {
    if (ImGui::SmallButton("?##iohelp")) ImGui::OpenPopup("##iohelpp");
    tip("What the ionospheric sounders are and how to hear them");
    if (ImGui::BeginPopup("##iohelpp")) {
        ImGui::PushTextWrapPos(470 * gUi);
        ImGui::TextUnformatted("Chirp sounders and ionograms");
        ImGui::Separator();
        ImGui::BulletText("Ionosondes and oblique sounders sweep a carrier across 0 - 30 MHz at a fixed rate (most often 100 kHz/s, also 125, "
                          "150, 200 and 500 kHz/s) and start each sweep at a fixed second of their repetition period. They are heard across a "
                          "continent: what comes back from the ionosphere arrives a few milliseconds later, on one or more paths (E, F1, F2, "
                          "several hops).");
        ImGui::BulletText("The sweeps are found by dechirping: the samples are multiplied by the conjugate of a chirp of each candidate rate. "
                          "A sweep of that rate turns into a steady tone, an echo delayed by tau into a tone r x tau lower; its FFT is the delay "
                          "profile at that frequency. All the profiles together are the ionogram: group range (c x tau, km) against frequency.");
        ImGui::BulletText("Radio: a wide one sees the most of the band per sweep (a HackRF at 20 Msps around 10 MHz sees 1 - 19 MHz). A narrow "
                          "radio sees the sweep only while it crosses its span: the strip above the ionogram shows what was captured. Use an HF "
                          "antenna (a long wire or a loop); a HackRF needs it below 30 MHz without the amplifier when broadcast stations are strong.");
        ImGui::BulletText("Timing locked: the sounders run from GPS and start on whole seconds, so with the computer clock right (NTP) the "
                          "delays are absolute. Clock errors of the computer shift the whole ionogram up or down; untick it to show delays "
                          "relative to the first echo. A frequency error of the radio shifts the delay too (100 Hz = 1 ms at 100 kHz/s): a "
                          "GPS-locked or calibrated radio is best.");
        ImGui::BulletText("The period and start offset are learned from two sweeps of the same rate; after that each sweep is expected and "
                          "its whole crossing goes into the ionogram.");
        ImGui::BulletText("Scaling: after each sweep the echo traces are picked out (delay peaks well over the noise, linked across "
                          "frequency; carriers that light a whole column are left out). A sounder close by (vertical incidence) gives the "
                          "standard values: foE, foF1, foF2 (where the O traces end), fxF2 (the X trace, about half the gyrofrequency higher), "
                          "h'E and h'F (the lowest virtual heights, half the group range), M(3000)F2 = 1490 / (h'(0.834 foF2) + 176) and "
                          "MUF(3000)F2 = foF2 x M(3000)F2. A '>' marks a value that only has a lower bound: the trace ran to the edge of "
                          "what was captured.");
        ImGui::BulletText("A distant sounder (oblique path): the distance from the known sounder's site and the receiver location (the "
                          "antenna position shared with the ADS-B map), else estimated from the delay of the lowest trace; each trace is put "
                          "to a mode (1E, 2E, 1F, 2F, 3F) by the mirror height its delay implies; the MUF of a mode is the nose of its trace. "
                          "The built-in list of sounders is short and unverified: an identification by the rate alone is a guess.");
        ImGui::BulletText("Bands open: a band is open on a path when it starts below the path's MUF (MUF(3000)F2 for a nearby sounder, foF2 "
                          "for short skip) and is not below the lowest echo heard (absorption).");
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
}

void savePng(App& a) {
    if (!live(a) || a.rx.iono.ionoDb.empty()) { S.note = "nothing to save yet"; return; }
    char nm[64];
    const std::time_t now = std::time(nullptr);
    std::strftime(nm, sizeof nm, "ionogram_%Y%m%d_%H%M%S.png", std::localtime(&now));
    const std::string path = saveFileDialog(nm);
    if (path.empty()) return;
    std::vector<uint32_t> px;
    int w, h;
    pixels(a.rx.iono, px, w, h);
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    for (size_t i = 0; i < px.size(); i++) {
        rgba[i * 4] = (uint8_t)(px[i] & 0xFF); rgba[i * 4 + 1] = (uint8_t)((px[i] >> 8) & 0xFF);
        rgba[i * 4 + 2] = (uint8_t)((px[i] >> 16) & 0xFF); rgba[i * 4 + 3] = 255;
    }
    S.note = gfx::writePng(path.c_str(), rgba.data(), w, h) ? "saved " + path : "could not write " + path;
}

void saveCsv(App& a) {
    if (!live(a) || a.rx.iono.ionoDb.empty()) { S.note = "nothing to save yet"; return; }
    char nm[64];
    const std::time_t now = std::time(nullptr);
    std::strftime(nm, sizeof nm, "ionogram_%Y%m%d_%H%M%S.csv", std::localtime(&now));
    const std::string path = saveFileDialog(nm);
    if (path.empty()) return;
    const std::string csv = io::ionogramCsv(a.rx.iono);
    FILE* f = dect2::u8fopen(path, "wb");
    S.note = f && fwrite(csv.data(), 1, csv.size(), f) == csv.size() ? "saved " + path : "could not write " + path;
    if (f) fclose(f);
}

void controls(App& a) {
    const auto& t = a.rx.iono;
    bool changed = false;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Sounder"); ImGui::SameLine();
    ImGui::SetNextItemWidth(260 * gUi);
    const std::string cur = S.sel >= 0 && S.sel < (int)t.sounders.size() ? sounderName(t.sounders[(size_t)S.sel]) : "the latest heard";
    if (ImGui::BeginCombo("##iosel", cur.c_str())) {
        if (ImGui::Selectable("the latest heard", S.sel < 0)) S.sel = -1;
        for (size_t i = 0; i < t.sounders.size(); i++)
            if (ImGui::Selectable(sounderName(t.sounders[i]).c_str(), S.sel == (int)i)) S.sel = (int)i;
        ImGui::EndCombo();
    }
    tip("The sounder whose ionogram is shown");
    flowNext(10 * gUi);
    changed |= ImGui::Checkbox("Timing locked", &S.locked);
    tip("Sweeps start on whole seconds (GPS-timed sounders): delays are absolute with the computer clock right; off: relative to the first echo");
    flowNext(10 * gUi);
    ImGui::TextDisabled("Range"); ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * gUi);
    changed |= ImGui::Combo("##iorange", &S.range, kRangeName, 2);
    flowNext(10 * gUi);
    ImGui::TextDisabled("Scale"); ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    changed |= ImGui::SliderFloat("##iodb", &S.dbHi, 3, 40, "0 - %.0f dB");
    tip("Colour scale: dB over the median of each frequency column");
    flowNext(10 * gUi);
    changed |= ImGui::Checkbox("Traces", &S.traces);
    tip("Draw the echo traces the scaling found (from the newest sweep through each frequency) over the ionogram");
    flowNext(10 * gUi);
    if (ImGui::SmallButton("Clear")) { a.engine.iono().clearResults(); S.sel = -1; }
    tip("Forget the sounders, their ionograms and the coverage");
    flowNext(6 * gUi);
    if (ImGui::SmallButton("Save PNG")) savePng(a);
    tip("The ionogram as a picture: 0 - 30 MHz across (50 kHz a pixel), group range up (15 km a pixel)");
    flowNext(6 * gUi);
    if (ImGui::SmallButton("Save CSV")) saveCsv(a);
    tip("Frequency, delay, group range and dB, one line per ionogram cell with data");
    flowNext(6 * gUi);
    helpButton();
    if (!S.note.empty()) { flowNext(8 * gUi); ImGui::TextDisabled("%s", S.note.c_str()); }
    flowEnd();
    if (changed) save();
}

// x of a frequency in the plot area [l, r]
float xOf(double hz, float l, float r) { return (float)(l + hz / io::kBandHz * (r - l)); }

void coverage(App& a, float h) {
    const auto& t = a.rx.iono;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float l = p0.x + 52 * gUi, r = p0.x + w - 6 * gUi;
    dl->AddRectFilled(ImVec2(l, p0.y), ImVec2(r, p0.y + h), IM_COL32(16, 18, 24, 255));
    const float cw = (r - l) / io::kCols;
    if (live(a)) {
        for (int c = 0; c < io::kCols; c++) {
            const float x = l + c * cw;
            if (c < (int)t.covered.size() && t.covered[(size_t)c]) dl->AddRectFilled(ImVec2(x, p0.y), ImVec2(x + cw + 0.5f, p0.y + h), IM_COL32(52, 58, 76, 255));
            if (c < (int)t.swept.size() && t.swept[(size_t)c]) dl->AddRectFilled(ImVec2(x, p0.y + h * 0.55f), ImVec2(x + cw + 0.5f, p0.y + h), IM_COL32(120, 90, 160, 255));
        }
        if (t.spanHiHz > t.spanLoHz) dl->AddRect(ImVec2(xOf(t.spanLoHz, l, r), p0.y), ImVec2(xOf(t.spanHiHz, l, r), p0.y + h), IM_COL32(200, 200, 230, 220));
    }
    dl->AddText(ImVec2(p0.x + 3, p0.y + (h - ImGui::GetTextLineHeight()) / 2), IM_COL32(130, 136, 150, 255), "band");
    ImGui::Dummy(ImVec2(w, h));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("0 - 30 MHz: grey what the radio has captured since the start, the frame the span now, purple where the shown "
                          "sounder was heard");
}

// ---- scaling

std::string traceName(const dect2::IonoTrace& tr, int geometry) {
    using T = dect2::IonoTrace;
    const char* l = tr.layer == T::E ? "E" : tr.layer == T::F1 ? "F1" : tr.layer == T::F2 ? (geometry == 2 ? "F" : "F2") : tr.layer == T::Ground ? "ground" : "?";
    char b[32];
    if (geometry == 2) snprintf(b, sizeof b, "%d%s%s", tr.hops, l, tr.highRay ? " high" : "");
    else if (tr.hops > 1) snprintf(b, sizeof b, "%dx%s", tr.hops, l);
    else snprintf(b, sizeof b, "%s%s", l, tr.x ? " X" : "");
    return b;
}

ImU32 traceCol(const dect2::IonoTrace& tr, int geometry) {
    using T = dect2::IonoTrace;
    ImU32 c = tr.layer == T::E ? IM_COL32(120, 235, 120, 255) : tr.layer == T::F1 ? IM_COL32(90, 215, 235, 255)
            : tr.layer == T::F2 ? IM_COL32(255, 255, 255, 255) : IM_COL32(170, 170, 180, 255);
    const bool faint = tr.x || tr.highRay || tr.layer == T::Ground || (geometry == 1 && tr.hops > 1) || tr.layer == T::Unknown;
    return faint ? (c & 0x00FFFFFFu) | 0x8C000000u : c;
}

void drawTraces(const dect2::IonoTelemetry& t, ImDrawList* dl, float l, float r, float top, float bot, double maxKm) {
    if (t.sel < 0 || t.sel >= (int)t.sounders.size()) return;
    const auto& sc = t.sounders[(size_t)t.sel].scale;
    dl->PushClipRect(ImVec2(l, top), ImVec2(r, bot), true);
    std::vector<ImVec2> pts;
    for (const auto& tr : t.traces) {
        if (tr.pts.size() < 2) continue;
        pts.clear();
        for (const auto& p : tr.pts) pts.push_back(ImVec2(xOf(p.fMhz * 1e6, l, r), (float)(bot - p.rangeKm / maxKm * (bot - top))));
        const ImU32 c = traceCol(tr, sc.geometry);
        dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(0, 0, 0, 160), 0, 3.5f * gUi);
        dl->AddPolyline(pts.data(), (int)pts.size(), c, 0, 1.6f * gUi);
        if (tr.layer != dect2::IonoTrace::Ground) dl->AddText(ImVec2(pts.back().x + 3 * gUi, pts.back().y - ImGui::GetTextLineHeight()), c, traceName(tr, sc.geometry).c_str());
    }
    // the critical frequency / MUF as a line
    const double f = sc.geometry == 1 ? sc.foF2 : sc.geometry == 2 ? sc.pathMuf : NAN;
    if (std::isfinite(f)) {
        const float x = xOf(f * 1e6, l, r);
        for (float y = top; y < bot; y += 8 * gUi) dl->AddLine(ImVec2(x, y), ImVec2(x, std::min(bot, y + 4 * gUi)), IM_COL32(255, 230, 120, 200), 1.2f * gUi);
        char b[48];
        snprintf(b, sizeof b, "%s %s%.2f", sc.geometry == 1 ? "foF2" : "MUF", (sc.geometry == 1 ? sc.foF2Above : sc.pathMufAbove) ? ">" : "", f);
        dl->AddText(ImVec2(x + 3 * gUi, top + 4 * gUi + ImGui::GetTextLineHeight()), IM_COL32(255, 230, 120, 230), b);
    }
    dl->PopClipRect();
}

void ionogram(App& a, float h) {
    const auto& t = a.rx.iono;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float lh = ImGui::GetTextLineHeight();
    const float l = p0.x + 52 * gUi, r = p0.x + w - 6 * gUi, top = p0.y + 2, bot = p0.y + h - lh - 6 * gUi;
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(8, 9, 13, 255));
    ImGui::InvisibleButton("##ioplot", ImVec2(w, h));
    const ImU32 grid = IM_COL32(60, 64, 80, 160), txt = IM_COL32(130, 136, 150, 255);
    const int rows = rowsShown();
    const double maxKm = rows * io::groupRangeKm(io::kDelayBin);
    if (live(a) && S.img && !t.ionoDb.empty())
        dl->AddImage(S.img->texture(), ImVec2(l, top), ImVec2(r, bot), ImVec2(0, 0), ImVec2(1, (float)rows / io::kRows));
    char b[96];
    for (int mhz = 0; mhz <= 30; mhz += 5) {
        const float x = xOf(mhz * 1e6, l, r);
        dl->AddLine(ImVec2(x, top), ImVec2(x, bot), grid);
        snprintf(b, sizeof b, "%d", mhz);
        const float tw = ImGui::CalcTextSize(b).x;
        dl->AddText(ImVec2(std::clamp(x - tw / 2, l, r - tw), bot + 3 * gUi), txt, b);
    }
    dl->AddText(ImVec2(r - ImGui::CalcTextSize("MHz").x - 2, bot + 3 * gUi), txt, "MHz");
    const int step = maxKm > 2000 ? 500 : 250;
    for (int km = 0; km <= (int)maxKm; km += step) {
        const float y = (float)(bot - km / maxKm * (bot - top));
        dl->AddLine(ImVec2(l, y), ImVec2(r, y), grid);
        snprintf(b, sizeof b, "%d", km);
        dl->AddText(ImVec2(p0.x + 3, std::clamp(y - lh / 2, top, bot - lh)), txt, b);
    }
    dl->AddText(ImVec2(p0.x + 3, p0.y + h - lh - 2), txt, "km");
    if (!live(a)) {
        dl->AddText(ImVec2(l + 10, top + 10), txt, a.engine.running() ? "listening for sweeps ..." : "start the receiver to look for chirp sounders (\"?\" for how)");
        return;
    }
    if (S.traces && t.sel >= 0) drawTraces(t, dl, l, r, top, bot, maxKm);
    if (t.sel < 0) { dl->AddText(ImVec2(l + 10, top + 10), txt, "no sounder heard yet: the ionogram builds up as sweeps cross the captured span"); }
    else {
        const auto& s = t.sounders[(size_t)t.sel];
        snprintf(b, sizeof b, "#%d  %.0f kHz/s  %s", s.id, s.rateHz / 1e3, t.locked ? "group range (c x delay)" : "relative group range (timing not locked)");
        dl->AddText(ImVec2(l + 6, top + 4), sounderCol(s.id), b);
    }
    if (ImGui::IsItemHovered() && t.ionoDb.size() == (size_t)io::kRows * io::kCols) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const double hz = std::clamp((m.x - l) / std::max(1.f, r - l), 0.f, 1.f) * io::kBandHz;
        const double km = std::clamp((bot - m.y) / std::max(1.f, bot - top), 0.f, 1.f) * maxKm;
        const int c = std::clamp((int)(hz / io::kColHz), 0, io::kCols - 1), row = std::clamp((int)std::lround(km / io::groupRangeKm(io::kDelayBin)), 0, io::kRows - 1);
        const float v = t.ionoDb[(size_t)row * io::kCols + (size_t)c];
        if (std::isnan(v)) ImGui::SetTooltip("%.2f MHz, %.0f km (%.2f ms)\nnot swept", hz / 1e6, km, io::rowDelay(row) * 1e3);
        else ImGui::SetTooltip("%.2f MHz, %.0f km (%.2f ms)\n%.1f dB", hz / 1e6, km, io::rowDelay(row) * 1e3, v);
    }
}

void timeline(App& a, float h) {
    const auto& t = a.rx.iono;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float l = p0.x + 52 * gUi, r = p0.x + w - 6 * gUi, lh = ImGui::GetTextLineHeight();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(6, 7, 10, 255));
    dl->AddText(ImVec2(p0.x + 3, p0.y + 2), IM_COL32(130, 136, 150, 255), "sweeps");
    ImGui::Dummy(ImVec2(w, h));
    if (!live(a) || t.timeline.empty()) {
        dl->AddText(ImVec2(l + 10, p0.y + 4), IM_COL32(110, 116, 128, 255), "timeline: one mark per sweep heard, a row per sounder");
        return;
    }
    const double span = 1800, t1 = t.nowSec, t0 = t1 - span;
    const int n = std::max(1, (int)t.sounders.size());
    const float rowH = std::max(4.f, (h - lh - 6) / n);
    for (int m = 0; m <= 6; m++) {
        const float x = (float)(l + m / 6.0 * (r - l));
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + h - lh - 2), IM_COL32(40, 44, 56, 255));
        char b[24]; snprintf(b, sizeof b, "-%d min", (6 - m) * 5);
        if (m < 6) dl->AddText(ImVec2(x + 2, p0.y + h - lh - 1), IM_COL32(110, 116, 128, 255), b);
    }
    for (const auto& e : t.timeline) {
        if (e.t0 < t0) continue;
        int k = 0;
        for (size_t i = 0; i < t.sounders.size(); i++) if (t.sounders[i].id == e.sounder) k = (int)i;
        const float x = (float)(l + (e.t0 - t0) / span * (r - l)), y = p0.y + 2 + k * rowH;
        dl->AddRectFilled(ImVec2(x - 1.5f * gUi, y), ImVec2(x + 1.5f * gUi, y + rowH - 1), sounderCol(e.sounder));
    }
    if (ImGui::IsItemHovered()) {
        const float mx = ImGui::GetIO().MousePos.x;
        const dect2::IonoEvent* best = nullptr; float bd = 8 * gUi;
        for (const auto& e : t.timeline) { const float x = (float)(l + (e.t0 - t0) / span * (r - l)); if (std::fabs(x - mx) < bd) { bd = std::fabs(x - mx); best = &e; } }
        if (best) ImGui::SetTooltip("%s  sounder #%d, %.0f kHz/s\n%.2f - %.2f MHz heard, %.1f dB%s", clock(best->t0).c_str(), best->sounder, best->rateHz / 1e3,
                                    best->fLoHz / 1e6, best->fHiHz / 1e6, best->snrDb, best->predicted ? "\nexpected from the period" : "");
    }
}

std::string val(double v, bool above, const char* fmt) {
    if (!std::isfinite(v)) return "-";
    char b[32]; snprintf(b, sizeof b, fmt, v);
    return above ? std::string(">") + b : std::string(b);
}

const char* knownName(const dect2::IonoScaling& s) {
    const auto& k = io::knownSounders();
    return s.known >= 0 && s.known < (int)k.size() ? k[(size_t)s.known].name : nullptr;
}

void saveHistory(App& a) {
    const auto& t = a.rx.iono;
    if (!live(a) || t.sel < 0 || t.sel >= (int)t.sounders.size() || t.history.empty()) { S.note = "no scaled values yet"; return; }
    char nm[64];
    const std::time_t now = std::time(nullptr);
    std::strftime(nm, sizeof nm, "iono_scaling_%Y%m%d_%H%M%S.csv", std::localtime(&now));
    const std::string path = saveFileDialog(nm);
    if (path.empty()) return;
    const auto& s = t.sounders[(size_t)t.sel];
    const std::string csv = io::historyCsv(t.history, s, knownName(s.scale));
    FILE* f = dect2::u8fopen(path, "wb");
    S.note = f && fwrite(csv.data(), 1, csv.size(), f) == csv.size() ? "saved " + path : "could not write " + path;
    if (f) fclose(f);
}

void row(const char* name, const std::string& v, const char* tipText) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn(); ImGui::TextDisabled("%s", name);
    if (tipText) tip(tipText);
    ImGui::TableNextColumn(); ImGui::TextUnformatted(v.c_str());
}

void values(const dect2::IonoTelemetry& t) {
    const auto& so = t.sounders[(size_t)t.sel];
    const auto& s = so.scale;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(sounderCol(so.id)));
    ImGui::Text("#%d  %.0f kHz/s", so.id, so.rateHz / 1e3);
    ImGui::PopStyleColor();
    ImGui::PushTextWrapPos(0);
    if (const char* nm = knownName(s)) {
        const auto& k = io::knownSounders()[(size_t)s.known];
        ImGui::TextWrapped("%s %s%s", s.knownMatch == 2 ? "Sounder:" : "Possibly:", nm, k.verified ? "" : " (list entry unverified)");
        tip(k.note);
    } else if (s.candidates > 0) ImGui::TextDisabled("Not identified: %d listed sounders sweep at this rate", s.candidates);
    else if (s.geometry) ImGui::TextDisabled("No listed sounder sweeps at this rate");
    if (s.geometry == 0) { ImGui::TextDisabled("No echo traces scaled yet"); ImGui::PopTextWrapPos(); return; }
    if (s.geometry == 1) ImGui::TextDisabled("Vertical incidence (a local ionosonde)");
    else ImGui::TextDisabled("Oblique path%s", s.distFrom == 2 ? ", distance from the sites" : s.distFrom == 1 ? ", distance from the delay" : "");
    ImGui::TextDisabled("Sweep at %s, %d traces", clock(s.t).c_str(), s.traces);
    if (!t.locked) ImGui::TextColored(ImVec4(1, .7f, .35f, 1), "Timing not locked: frequencies only");
    ImGui::PopTextWrapPos();
    if (!ImGui::BeginTable("##iovals", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) return;
    if (s.geometry == 1) {
        row("foE", val(s.foE, s.foEAbove, "%.2f MHz"), "Critical frequency of the E layer (where its O trace ends)");
        row("h'E", val(s.hE, false, "%.0f km"), "Lowest virtual height of the E trace");
        row("foF1", val(s.foF1, false, "%.2f MHz"), "Critical frequency of the F1 layer (a cusp of the F trace), when it shows");
        row("foF2", val(s.foF2, s.foF2Above, "%.2f MHz"), "Critical frequency of the F2 layer (where its O trace ends): the highest frequency "
                                                           "reflected straight up, the limit for short skip (NVIS)");
        row("fxF2", val(s.fxF2, false, "%.2f MHz"), "Where the X trace of F2 ends (about half the gyrofrequency above foF2)");
        row("h'F", val(s.hF, false, "%.0f km"), "Lowest virtual height of the F traces");
        row("h'F2", val(s.hF2, false, "%.0f km"), "Lowest virtual height of the F2 trace");
        row("hmF2", val(s.hmF2, false, "%.0f km"), "Peak height of F2, h'(0.834 foF2) for a parabolic layer");
        row("M(3000)F2", val(s.m3000, false, "%.2f"), "MUF(3000)F2 / foF2 = 1490 / (hmF2 + 176) (Shimazaki)");
        row("MUF(3000)F2", val(s.muf3000, false, "%.2f MHz"), "The highest frequency over a 3000 km path through the F2 layer above here");
    } else {
        row("Distance", val(s.distKm, false, "%.0f km"), s.distFrom == 2 ? "Great circle from the known sounder to the receiver location"
                                                                          : "Estimated from the delay of the lowest trace (a 1-hop mirror)");
        row("Path MUF", val(s.pathMuf, s.pathMufAbove, "%.2f MHz"), "The highest frequency heard over the path: the nose of the highest mode");
        for (const auto& m : s.modes) {
            char n[16]; snprintf(n, sizeof n, "%d%s", m.hops, m.layer == dect2::IonoTrace::E ? "E" : "F");
            char v[64]; snprintf(v, sizeof v, "%s, %.0f km", val(m.mufMhz, m.mufAbove, "%.2f MHz").c_str(), m.minRangeKm);
            row(n, v, "A propagation mode: its MUF (the nose of its trace) and its shortest group path");
        }
    }
    row("fmin", val(s.fmin, false, "%.2f MHz"), "The lowest frequency with an echo (absorption below)");
    ImGui::EndTable();
}

void bands(const dect2::IonoTelemetry& t) {
    struct Path { std::string name; double muf, fmin; };
    std::vector<Path> paths;
    for (const auto& so : t.sounders) {
        const auto& s = so.scale;
        char b[48];
        if (s.geometry == 1) {
            if (std::isfinite(s.foF2) && !s.foF2Above) { snprintf(b, sizeof b, "#%d near", so.id); paths.push_back({b, s.foF2, s.fmin}); }
            snprintf(b, sizeof b, "#%d 3000", so.id); paths.push_back({b, s.muf3000, s.fmin});
        } else if (s.geometry == 2) {
            if (std::isfinite(s.distKm)) snprintf(b, sizeof b, "#%d %.0f", so.id, s.distKm); else snprintf(b, sizeof b, "#%d", so.id);
            paths.push_back({b, s.pathMufAbove ? NAN : s.pathMuf, s.fmin});
        }
    }
    if (paths.empty()) { ImGui::TextDisabled("Bands open: no path scaled yet"); return; }
    ImGui::TextDisabled("Bands open now");
    tip("Per path: a band is open when it starts below the path's MUF and is not below the lowest echo. \"near\": a sounder close by, short "
        "skip up to foF2; \"3000\": a 3000 km path through the F2 layer above it (MUF(3000)F2); oblique: the path to that sounder (km)");
    const int n = std::min<int>((int)paths.size(), 4);
    if (!ImGui::BeginTable("##iobands", n + 1, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) return;
    ImGui::TableSetupColumn("Band");
    for (int i = 0; i < n; i++) ImGui::TableSetupColumn(paths[(size_t)i].name.c_str());
    ImGui::TableHeadersRow();
    for (const auto& b : io::hfBands()) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (b.broadcast) ImGui::TextDisabled("%s bc", b.name); else ImGui::TextUnformatted(b.name);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.3f - %.3f MHz (%s)", b.loMhz, b.hiMhz, b.broadcast ? "broadcast" : "amateur");
        for (int i = 0; i < n; i++) {
            ImGui::TableNextColumn();
            const int st = io::bandState(b, paths[(size_t)i].muf, paths[(size_t)i].fmin);
            if (st == 1) ImGui::TextColored(ImVec4(.45f, .9f, .5f, 1), "open");
            else if (st == 0) ImGui::TextDisabled("closed");
            else if (st == -1) ImGui::TextColored(ImVec4(1, .65f, .3f, 1), "absorbed");
            else ImGui::TextDisabled("-");
        }
    }
    ImGui::EndTable();
}

void historyChart(App& a, float h) {
    const auto& t = a.rx.iono;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float lh = ImGui::GetTextLineHeight();
    const float l = p0.x + 26 * gUi, r = p0.x + w - 4 * gUi, top = p0.y + lh + 4, bot = p0.y + h - lh - 2;
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(8, 9, 13, 255));
    ImGui::Dummy(ImVec2(w, h));
    const ImU32 cF = IM_COL32(110, 200, 255, 255), cM = IM_COL32(255, 200, 90, 255), txt = IM_COL32(130, 136, 150, 255);
    const bool vert = !t.history.empty() && t.history.back().geometry == 1;
    dl->AddText(ImVec2(p0.x + 4, p0.y + 2), cM, vert ? "MUF(3000)" : "path MUF");
    if (vert) dl->AddText(ImVec2(p0.x + 4 + ImGui::CalcTextSize("MUF(3000)  ").x, p0.y + 2), cF, "foF2");
    if (t.history.empty()) { dl->AddText(ImVec2(l + 4, top + 4), txt, "history: a point per scaled sweep"); return; }
    const double t1 = std::max(t.nowSec, t.history.back().t), t0 = std::min(t.history.front().t, t1 - 3600);
    double fmax = 10;
    for (const auto& p : t.history) for (float v : {p.foF2, p.muf3000, p.pathMuf}) if (std::isfinite(v)) fmax = std::max(fmax, (double)v);
    fmax = std::ceil(fmax / 5) * 5;
    auto X = [&](double s) { return (float)(l + (s - t0) / std::max(1.0, t1 - t0) * (r - l)); };
    auto Y = [&](double f) { return (float)(bot - f / fmax * (bot - top)); };
    char b[48];
    for (int f = 0; f <= (int)fmax; f += 5) {
        dl->AddLine(ImVec2(l, Y(f)), ImVec2(r, Y(f)), IM_COL32(40, 44, 56, 255));
        snprintf(b, sizeof b, "%d", f);
        dl->AddText(ImVec2(p0.x + 3, Y(f) - lh / 2), txt, b);
    }
    dl->AddText(ImVec2(l, bot + 1), txt, clock(t0).c_str());
    const std::string e = clock(t1);
    dl->AddText(ImVec2(r - ImGui::CalcTextSize(e.c_str()).x, bot + 1), txt, e.c_str());
    auto series = [&](int which, ImU32 c) {
        ImVec2 prev; bool have = false;
        for (const auto& p : t.history) {
            const float v = which == 0 ? p.foF2 : (p.geometry == 1 ? p.muf3000 : p.pathMuf);
            if (!std::isfinite(v)) { have = false; continue; }
            const ImVec2 q(X(p.t), Y(v));
            if (have) dl->AddLine(prev, q, c, 1.5f * gUi);
            dl->AddCircleFilled(q, 2.f * gUi, c);
            prev = q; have = true;
        }
    };
    if (vert) series(0, cF);
    series(1, cM);
    if (ImGui::IsItemHovered()) {
        const float mx = ImGui::GetIO().MousePos.x;
        const dect2::IonoHistPt* best = nullptr; float bd = 8 * gUi;
        for (const auto& p : t.history) if (std::fabs(X(p.t) - mx) < bd) { bd = std::fabs(X(p.t) - mx); best = &p; }
        if (best) {
            if (best->geometry == 1)
                ImGui::SetTooltip("%s\nfoF2 %s, MUF(3000)F2 %s\nfoE %s, h'F %s", clock(best->t).c_str(), val(best->foF2, false, "%.2f MHz").c_str(),
                                  val(best->muf3000, false, "%.2f MHz").c_str(), val(best->foE, false, "%.2f MHz").c_str(), val(best->hF, false, "%.0f km").c_str());
            else
                ImGui::SetTooltip("%s\npath MUF %s, distance %s", clock(best->t).c_str(), val(best->pathMuf, false, "%.2f MHz").c_str(),
                                  val(best->distKm, false, "%.0f km").c_str());
        }
    }
}

void panel(App& a) {
    const auto& t = a.rx.iono;
    if (!live(a) || t.sel < 0 || t.sel >= (int)t.sounders.size()) {
        ImGui::TextDisabled("Scaling");
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("The scaled values of the shown sounder appear here after its first sweep with echo traces.");
        ImGui::PopTextWrapPos();
        return;
    }
    values(t);
    ImGui::Spacing();
    ImGui::TextDisabled("History");
    ImGui::SameLine();
    if (ImGui::SmallButton("Save CSV##iohist")) saveHistory(a);
    tip("The scaled values of every sweep of this sounder (time, foE, foF1, foF2, h'E, h'F, M(3000)F2, MUF(3000)F2, path MUF, distance)");
    historyChart(a, 110 * gUi);
    ImGui::Spacing();
    bands(t);
}

void tick(App& a) {
    load();
    auto& r = a.engine.iono();
    r.setLocked(S.locked);
    r.setSelected(S.sel);
    {   // the antenna position the map modes share
        const double lat = plat::prefs().getD("adsbLat", 0), lon = plat::prefs().getD("adsbLon", 0);
        r.setObserver(lat != 0 || lon != 0, lat, lon);
    }
    a.tune.synth.modeVal[0] = a.tune.centerHz / 1e6;   // the test signal places its sweeps for this frequency (read when it starts)
    if (live(a) && S.sel >= (int)a.rx.iono.sounders.size()) S.sel = -1;
}

void tab(App& a) {
    load();
    renderImage(a);
    controls(a);
    const ImVec2 av = ImGui::GetContentRegionAvail();
    const float tl = std::clamp(av.y * 0.18f, 50 * gUi, 120 * gUi);
    const float mainH = std::max(160 * gUi, av.y - tl - 6 * gUi);
    const float pw = std::clamp(av.x * 0.3f, 220 * gUi, 340 * gUi);
    const float gap = 6 * gUi;
    ImGui::BeginChild("##ioleft", ImVec2(std::max(200 * gUi, av.x - pw - gap), mainH), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    coverage(a, 14 * gUi);
    ionogram(a, std::max(140 * gUi, ImGui::GetContentRegionAvail().y));
    ImGui::EndChild();
    ImGui::SameLine(0, gap);
    ImGui::BeginChild("##iopanel", ImVec2(pw, mainH), false);
    panel(a);
    ImGui::EndChild();
    timeline(a, std::max(40 * gUi, ImGui::GetContentRegionAvail().y - 2));
}

void list(App& a) {
    const auto& t = a.rx.iono;
    if (!live(a) || t.sounders.empty()) { ImGui::TextDisabled("no sounder heard yet"); return; }
    for (size_t i = 0; i < t.sounders.size(); i++) {
        const auto& s = t.sounders[i];
        ImGui::PushID((int)i);
        const bool shown = t.sel == (int)i;
        char b[64]; snprintf(b, sizeof b, "#%d  %.0f kHz/s%s", s.id, s.rateHz / 1e3, s.active ? "  (sweeping)" : "");
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(sounderCol(s.id)));
        if (ImGui::Selectable(b, shown)) S.sel = (int)i;
        ImGui::PopStyleColor();
        ImGui::Indent(10 * gUi);
        if (s.periodSec > 0) ImGui::TextDisabled("every %.0f s, starts at +%.0f s; next %s", s.periodSec, s.offsetSec, clock(s.nextT0).c_str());
        else ImGui::TextDisabled("period: one more sweep needed");
        ImGui::TextDisabled("%d sweeps%s, %.1f dB, %.1f - %.1f MHz", s.sweeps, s.missed ? (", " + std::to_string(s.missed) + " missed").c_str() : "",
                            s.snrDb, s.fLoHz / 1e6, s.fHiHz / 1e6);
        ImGui::TextDisabled("last heard %s (%.0f s ago)", clock(s.lastSeen).c_str(), std::max(0.0, t.nowSec - s.lastSeen));
        ImGui::Unindent(10 * gUi);
        ImGui::PopID();
    }
}

void receiver(App& a) {
    const auto& t = a.rx.iono;
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "listening" : "stopped"); return; }
    ImGui::TextWrapped("%s", dect2::ionoSummary(t).c_str());
    ImGui::TextWrapped("Detector: %d-point FFT (%.1f ms, %.0f Hz bins), %llu FFTs, %llu chirp peaks not yet tied to a sweep; candidate rates 100, 125, 150, "
                       "200 and 500 kHz/s", t.fftSize, t.rate > 0 ? 1e3 * t.fftSize / t.rate : 0.0, t.fftSize ? t.rate / t.fftSize : 0.0,
                       (unsigned long long)t.frames, (unsigned long long)t.hits);
    ImGui::TextWrapped("Ionogram: 0 - 30 MHz in 50 kHz columns, delay 0 - 10 ms in 0.05 ms rows (group range 0 - 3000 km, 15 km a row)");
    ImGui::TextWrapped("Time: %s; %s", clock(t.nowSec, true).c_str(), t.locked ? "sweep starts rounded to whole seconds (absolute delays)" : "timing not locked (relative delays)");
    if (t.spanHiHz <= t.spanLoHz) ImGui::TextColored(ImVec4(1, .6f, .3f, 1), "The captured span is outside 0 - 30 MHz: tune the radio into the HF band");
}

void status(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine();
    ImGui::TextUnformatted(live(a) ? dect2::ionoSummary(a.rx.iono).c_str() : (a.engine.running() ? "listening" : "stopped"));
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Ionospheric sounders";
    if (live(a)) {
        const auto& t = a.rx.iono;
        char b[128];
        snprintf(b, sizeof b, "%zu sounder%s, %zu sweeps, %.1f - %.1f MHz captured", t.sounders.size(), t.sounders.size() == 1 ? "" : "s", t.timeline.size(),
                 t.spanLoHz / 1e6, t.spanHiHz / 1e6);
        l2 = b;
    }
}

void synth(App& a, bool& changed) {
    dect2::SynthConfig& sc = a.tune.synth;
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("two sounders in noise: 100 kHz/s every 30 s at +2 s and 500 kHz/s every 20 s at +7 s, each on two paths (1.6 / 2.4 ms and 1.8 / 2.9 ms)"); ImGui::PopTextWrapPos(); }
    bool off = sc.modeOpt[0] == 1;
    if (ImGui::Checkbox("Noise only", &off)) { sc.modeOpt[0] = off ? 1 : 0; changed = true; }
    // the sweeps are placed for the frequency the radio is set to
    const double mhz = a.tune.centerHz / 1e6;
    if (std::fabs(sc.modeVal[0] - mhz) > 1e-6) { sc.modeVal[0] = mhz; changed = true; }
}

} // namespace

extern const ModeUi kIonoUi;
const ModeUi kIonoUi = {
    .sideTitle = "SOUNDERS",
    .tabName = "Ionogram",
    .tabIcon = Ic::Wave,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .synth = synth,
    .tick = tick,
};

// VOR and ILS screens (one receiver, two mode-list entries).
// VOR: the radial with a compass card, FROM / TO, the signal quality and the identification; a position from two stations (the receiver
// hops between them, or between the frequencies of a scan list) drawn on the map with both radials, the crossing and its error ellipse;
// the station list, imported from an OurAirports or OpenAIP CSV.
// ILS: a course deviation indicator with the localizer and glide slope needles, the marker lamps, the paired frequencies and a hop
// between localizer and glide slope.
#include "app.h"
#include "dect2/results_io.h"
#include "../adsb/adsb_map.h"
#include "dect2/ils.h"
#include "dect2/vor_db.h"
#include "dect2/vor_rx.h"
#include "dect2/vor_tel.h"
#include "dect2/u8file.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr float kPi = 3.14159265f;

struct Slot {                    // one station of the fix, or one frequency of the scan
    double freqMhz = 0;
    bool valid = false;
    double radial = 0, sd = 0;
    std::string ident;
    double when = -1;            // ImGui time of the measurement
    float cnr = 0;
};

struct State {
    bool loaded = false;
    std::vector<dect2::NavStation> db;
    std::string dbPath, dbMsg;
    int viewVor = 0, viewIls = 0;
    adsbmap::View map;
    Slot fix[2];
    bool hop = false;            // hop between the two fix stations
    bool scan = false;           // hop through the scan list
    char scanText[256] = "112.10, 113.00, 114.30, 115.70, 116.60";
    std::vector<Slot> scanRes;
    int hopIdx = 0;
    double hopT0 = 0;
    float dwell = 8;
    bool ilsHop = false;         // localizer <-> glide slope
    double locMhz = 110.30;
    dect2::VorTelemetry lastLoc, lastGs;
    bool haveLoc = false, haveGs = false;
    std::deque<float> radHist;
    uint64_t histSeq = 0;
    bool identFilter = true, pushedFilter = false;
    bool wasRunning = false;
    float pushedVol = -1; bool pushedMute = false;
    char search[32] = "";
};
State S;

bool live(const App& a) { return a.rx.standard == 31; }   // the engine reports its standard code minus one
bool isIls(const App& a) { const dect2::SubMode* sm = curSub(a); return sm && sm->sel == 2; }

bool loadDb(const std::string& path) {
    std::ifstream f(dect2::u8file(path), std::ios::binary);
    if (!f) { S.dbMsg = "cannot open " + path; return false; }
    std::stringstream ss; ss << f.rdbuf();
    std::vector<dect2::NavStation> v;
    std::string err;
    const int n = dect2::navParseCsv(ss.str(), v, err);
    if (!n) { S.dbMsg = err; return false; }
    S.db = std::move(v);
    S.dbPath = path;
    char b[96]; snprintf(b, sizeof b, "%d VOR / ILS stations", n);
    S.dbMsg = b;
    return true;
}

void load() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    S.fix[0].freqMhz = d.getD("vorFixA", 113.0); S.fix[1].freqMhz = d.getD("vorFixB", 116.6);
    S.dwell = (float)d.getD("vorDwell", 8);
    S.identFilter = d.getB("vorIdFilter", true);
    S.map.zoom = (int)d.getI("vorZoom", 7);
    const std::string st = d.getS("vorScan", "");
    if (!st.empty()) snprintf(S.scanText, sizeof S.scanText, "%s", st.c_str());
    const std::string p = d.getS("vorDb", "");
    if (!p.empty()) loadDb(p);
}
void save() {
    plat::Prefs& d = plat::prefs();
    d.setD("vorFixA", S.fix[0].freqMhz); d.setD("vorFixB", S.fix[1].freqMhz); d.setD("vorDwell", S.dwell);
    d.setB("vorIdFilter", S.identFilter); d.setS("vorScan", S.scanText); d.setS("vorDb", S.dbPath);
}

const dect2::NavStation* stationOf(const std::string& ident, double mhz) { return ident.empty() ? nullptr : dect2::navMatch(S.db, ident, mhz); }

std::vector<double> scanList() {
    std::vector<double> v;
    std::string s(S.scanText);
    for (auto& c : s) if (c == ',' || c == ';') c = ' ';
    std::stringstream ss(s);
    double f;
    while (ss >> f) if (f >= 108 && f <= 118) v.push_back(f);
    return v;
}

// ---------------------------------------------------------------- drawing pieces

void compass(ImVec2 c, float r, double radial, bool valid) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(c, r, IM_COL32(18, 22, 26, 255), 64);
    dl->AddCircle(c, r, IM_COL32(120, 130, 140, 255), 64, 1.5f * gUi);
    for (int d = 0; d < 360; d += 10) {
        const float an = (float)d * kPi / 180;
        const float l = d % 30 == 0 ? 0.14f : 0.07f;
        dl->AddLine(ImVec2(c.x + std::sin(an) * r * (1 - l), c.y - std::cos(an) * r * (1 - l)), ImVec2(c.x + std::sin(an) * r, c.y - std::cos(an) * r), IM_COL32(170, 176, 182, 255), 1.2f * gUi);
        if (d % 90 == 0) {
            const char* nm = d == 0 ? "N" : d == 90 ? "E" : d == 180 ? "S" : "W";
            const ImVec2 ts = ImGui::CalcTextSize(nm);
            dl->AddText(ImVec2(c.x + std::sin(an) * r * 0.74f - ts.x / 2, c.y - std::cos(an) * r * 0.74f - ts.y / 2), IM_COL32(220, 224, 228, 255), nm);
        }
    }
    if (!valid) { const ImVec2 ts = ImGui::CalcTextSize("OFF"); dl->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), IM_COL32(230, 90, 80, 255), "OFF"); return; }
    const float an = (float)radial * kPi / 180;
    const ImVec2 tip(c.x + std::sin(an) * r * 0.86f, c.y - std::cos(an) * r * 0.86f);
    dl->AddLine(c, tip, IM_COL32(110, 210, 230, 255), 3 * gUi);
    dl->AddCircleFilled(tip, 4 * gUi, IM_COL32(110, 210, 230, 255));
    dl->AddLine(c, ImVec2(c.x - std::sin(an) * r * 0.6f, c.y + std::cos(an) * r * 0.6f), IM_COL32(110, 210, 230, 120), 2 * gUi);
    dl->AddCircleFilled(c, 4 * gUi, IM_COL32(230, 230, 230, 255));
}

void bar(const char* label, float v, float nominal, float full, const char* fmt) {
    ImGui::TextDisabled("%s", label); kvColumn(120 * gUi);
    char b[48]; snprintf(b, sizeof b, fmt, v);
    const float ok = nominal > 0 ? v / nominal : 0;
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ok > 0.7f && ok < 1.3f ? pal::okGreen() : pal::warnAmber());
    ImGui::ProgressBar(std::max(0.f, std::min(1.f, v / full)), ImVec2(std::min(220 * gUi, ImGui::GetContentRegionAvail().x), 0), b);
    ImGui::PopStyleColor();
}

void identLine(const App& a, const dect2::VorTelemetry& t) {
    ImGui::TextDisabled("identification"); kvColumn(120 * gUi);
    ImGui::PushFont(a.mono, 0);
    if (!t.ident.empty()) {
        ImGui::Text("%s", t.ident.c_str());
        std::string m;
        for (char c : t.ident) { if (const char* p = dect2::navMorse(c)) { if (!m.empty()) m += "  "; m += p; } }
        ImGui::SameLine(); ImGui::TextDisabled("%s", m.c_str());
    } else ImGui::TextDisabled("%s", t.identLast.empty() ? "listening for the 1020 Hz Morse" : ("heard " + t.identLast + ", waiting for it again").c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("keying"); kvColumn(120 * gUi);
    ImGui::TextColored(t.identKey ? pal::okGreen() : pal::grey(), t.identKey ? "tone" : "-");
    ImGui::SameLine(); ImGui::TextDisabled("%s  %.0f wpm, %.0f %% depth", t.morse.c_str(), t.identWpm, t.identDepth * 100);
    const double mhz = t.dialHz > 0 ? t.dialHz / 1e6 : a.freqMhz;
    if (const dect2::NavStation* s = stationOf(t.ident, mhz)) {
        ImGui::TextDisabled("station"); kvColumn(120 * gUi);
        ImGui::TextWrapped("%s %s (%s), %.4f %.4f, variation %+.1f", s->ident.c_str(), s->name.c_str(), s->type.c_str(), s->lat, s->lon, s->magVarDeg);
    } else if (!t.ident.empty()) {
        ImGui::TextDisabled("station"); kvColumn(120 * gUi);
        ImGui::TextDisabled("%s", S.db.empty() ? "import a station list (Stations) to place it" : "not in the station list on this frequency");
    }
}

void radialView(App& a) {
    const bool on = live(a);
    const dect2::VorTelemetry& t = a.rx.vor;
    const bool valid = on && t.vorValid;
    const float h = std::min(ImGui::GetContentRegionAvail().y * 0.55f, 260 * gUi);
    const float r = std::max(40 * gUi, h / 2 - 6 * gUi);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    compass(ImVec2(p0.x + r + 6 * gUi, p0.y + r + 4 * gUi), r, t.radialDeg, valid);
    ImGui::Dummy(ImVec2(2 * r + 12 * gUi, 2 * r + 8 * gUi));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::PushFont(a.mono, 0);
    if (valid) {
        ImGui::SetWindowFontScale(2.2f);
        ImGui::Text("%05.1f", t.radialDeg);
        ImGui::SetWindowFontScale(1.f);
        ImGui::Text("FROM %05.1f   TO %05.1f", t.radialDeg, dect2::vorToBearing(t.radialDeg));
        ImGui::TextDisabled("raw %05.1f, scatter %.2f deg", t.radialRawDeg, t.radialStdDeg);
    } else ImGui::TextDisabled("%s", !on ? (a.engine.running() ? "starting" : "stopped") : t.state == 0 ? "no carrier" : "no VOR signal");
    ImGui::PopFont();
    if (on) {
        bar("30 Hz AM", t.am30 * 100, 30, 60, "%.1f %%");
        bar("9960 Hz", t.sub9960 * 100, 30, 60, "%.1f %%");
        bar("FM deviation", t.fmDevHz, 480, 960, "%.0f Hz");
        bar("C/N (25 kHz)", t.snrDb, 30, 60, "%.1f dB");
    }
    ImGui::EndGroup();
    if (on) identLine(a, t);
    if (S.radHist.size() > 2) {
        std::vector<float> v(S.radHist.begin(), S.radHist.end());
        ImGui::PlotLines("##vorh", v.data(), (int)v.size(), 0, "radial, last 2 min", FLT_MAX, FLT_MAX, ImVec2(ImGui::GetContentRegionAvail().x, std::max(40 * gUi, ImGui::GetContentRegionAvail().y - 4 * gUi)));
    }
}

double trueBearing(const Slot& s, const dect2::NavStation* st) { return std::fmod(s.radial + (st ? st->magVarDeg : 0) + 360, 360); }

void fixMap(App& a, ImVec2 size, const dect2::NavStation* st[2], const dect2::NavFix& f) {
    (void)a;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    adsbmap::draw(S.map, size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    for (const auto& s : S.db) {   // the list's stations in view
        const ImVec2 q = adsbmap::project(s.lat, s.lon);
        if (q.x < p0.x || q.y < p0.y || q.x > p0.x + size.x || q.y > p0.y + size.y) continue;
        dl->AddCircle(q, 3.5f * gUi, IM_COL32(150, 170, 190, 200), 12);
    }
    static const ImU32 col[2] = {IM_COL32(110, 210, 230, 255), IM_COL32(240, 180, 90, 255)};
    for (int i = 0; i < 2; i++) {
        if (!st[i]) continue;
        const ImVec2 q = adsbmap::project(st[i]->lat, st[i]->lon);
        dl->AddRect(ImVec2(q.x - 5 * gUi, q.y - 5 * gUi), ImVec2(q.x + 5 * gUi, q.y + 5 * gUi), col[i], 0, 0, 2 * gUi);
        dl->AddText(ImVec2(q.x + 8 * gUi, q.y - 8 * gUi), col[i], st[i]->ident.c_str());
        if (S.fix[i].valid) {
            double la, lo;
            dect2::navDestination(st[i]->lat, st[i]->lon, trueBearing(S.fix[i], st[i]), 400e3, la, lo);
            dl->AddLine(q, adsbmap::project(la, lo), col[i], 2 * gUi);
        }
    }
    if (f.ok) {
        const ImVec2 q = adsbmap::project(f.lat, f.lon);
        std::vector<ImVec2> pts;
        const double k = 2.45;   // 95 %
        for (int i = 0; i < 48; i++) {
            const double t = 2 * M_PI * i / 48;
            const double u = k * f.sigMajorM * std::cos(t), v = k * f.sigMinorM * std::sin(t);
            const double az = f.majorAzDeg * M_PI / 180;
            const double e = u * std::sin(az) + v * std::cos(az), n = u * std::cos(az) - v * std::sin(az);
            double la, lo;
            dect2::navDestination(f.lat, f.lon, std::atan2(e, n) * 180 / M_PI, std::hypot(e, n), la, lo);
            pts.push_back(adsbmap::project(la, lo));
        }
        dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(255, 255, 255, 220), ImDrawFlags_Closed, 1.5f * gUi);
        dl->AddCircleFilled(q, 5 * gUi, IM_COL32(255, 90, 90, 255));
    }
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(26 * gUi, 0))) S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1);
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(26 * gUi, 0))) S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1);
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never the position itself");
    plat::prefs().setI("vorZoom", S.map.zoom);
    if (!f.ok) {
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
        adsbmap::legend(size.x, "%s", S.db.empty() ? "import a station list to place the stations" : !st[0] || !st[1] ? "both stations must be identified and in the list" : f.why.empty() ? "waiting for both radials" : f.why.c_str());
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
}

void fixView(App& a) {
    const dect2::NavStation* st[2];
    for (int i = 0; i < 2; i++) st[i] = stationOf(S.fix[i].ident, S.fix[i].freqMhz);
    dect2::NavFix f;
    if (st[0] && st[1] && S.fix[0].valid && S.fix[1].valid)
        f = dect2::navFix(st[0]->lat, st[0]->lon, trueBearing(S.fix[0], st[0]), std::max(0.3, S.fix[0].sd), st[1]->lat, st[1]->lon, trueBearing(S.fix[1], st[1]), std::max(0.3, S.fix[1].sd));
    for (int i = 0; i < 2; i++) {
        ImGui::PushID(i);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(i ? ImVec4(0.94f, 0.70f, 0.35f, 1) : ImVec4(0.43f, 0.82f, 0.90f, 1), "%s", i ? "B" : "A");
        ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
        if (ImGui::InputDouble("MHz", &S.fix[i].freqMhz, 0.05, 0.5, "%.2f")) { S.fix[i].freqMhz = std::max(108.0, std::min(117.95, S.fix[i].freqMhz)); S.fix[i].valid = false; save(); }
        ImGui::SameLine();
        if (ImGui::Button("tune")) tuneFreq(a, S.fix[i].freqMhz);
        ImGui::SameLine();
        if (S.fix[i].valid) ImGui::Text("%s radial %05.1f +- %.1f", S.fix[i].ident.empty() ? "?" : S.fix[i].ident.c_str(), S.fix[i].radial, S.fix[i].sd);
        else ImGui::TextDisabled("no radial yet");
        ImGui::PopID();
    }
    if (ImGui::Checkbox("hop between A and B", &S.hop)) { S.scan = false; S.hopIdx = 0; S.hopT0 = 0; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The radio retunes between the two stations; each radial is kept from the end of its dwell.");
    ImGui::SameLine(); ImGui::SetNextItemWidth(120 * gUi);
    if (ImGui::SliderFloat("##dwell", &S.dwell, 4, 30, "dwell %.0f s")) save();
    ImGui::SameLine();
    if (f.ok) ImGui::Text("%.4f %.4f   95 %%: %.1f x %.1f km, crossing %.0f deg", f.lat, f.lon, 2.45 * f.sigMajorM / 1e3, 2.45 * f.sigMinorM / 1e3, f.crossDeg);
    fixMap(a, ImVec2(ImGui::GetContentRegionAvail().x, std::max(120 * gUi, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 3)), st, f);
    ImGui::SetNextItemWidth(std::min(360 * gUi, ImGui::GetContentRegionAvail().x - 120 * gUi));
    if (ImGui::InputText("##scanl", S.scanText, sizeof S.scanText)) save();
    ImGui::SameLine();
    if (ImGui::Checkbox("scan", &S.scan)) { S.hop = false; S.hopIdx = 0; S.hopT0 = 0; S.scanRes.clear(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hop through these frequencies (MHz) and list what each one hears; pick two for the position.");
    for (size_t i = 0; i < S.scanRes.size(); i++) {
        const Slot& r = S.scanRes[i];
        ImGui::PushID((int)i);
        ImGui::Text("%.2f  %-5s %s", r.freqMhz, r.ident.c_str(), r.valid ? "" : "no VOR");
        if (r.valid) { ImGui::SameLine(); ImGui::Text("radial %05.1f", r.radial); ImGui::SameLine(); if (ImGui::SmallButton("A")) { S.fix[0] = r; save(); } ImGui::SameLine(); if (ImGui::SmallButton("B")) { S.fix[1] = r; save(); } }
        ImGui::PopID();
    }
}

void stationsView(App& a) {
    if (ImGui::Button("Import CSV...")) { const std::string p = openFileDialog(); if (!p.empty() && loadDb(p)) save(); }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", S.dbMsg.empty() ? "no station list" : S.dbMsg.c_str());
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("OurAirports navaids (public domain): https://davidmegginson.github.io/ourairports-data/navaids.csv  -  OpenAIP (CC BY-NC, export a country's navaids as CSV): https://www.openaip.net. Nothing is bundled; the file stays where it is.");
    ImGui::PopTextWrapPos();
    ImGui::SetNextItemWidth(160 * gUi);
    ImGui::InputTextWithHint("##vsrch", "search ident or name", S.search, sizeof S.search);
    if (S.db.empty()) return;
    if (!ImGui::BeginTable("##vdb", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Ident"); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Position"); ImGui::TableSetupColumn("");
    ImGui::TableHeadersRow();
    std::string q(S.search);
    for (auto& c : q) c = (char)toupper((unsigned char)c);
    int shown = 0;
    for (size_t i = 0; i < S.db.size() && shown < 400; i++) {
        const auto& s = S.db[i];
        if (!q.empty()) { std::string n = s.name; for (auto& c : n) c = (char)toupper((unsigned char)c); if (s.ident.find(q) == std::string::npos && n.find(q) == std::string::npos) continue; }
        shown++;
        ImGui::PushID((int)i);
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(s.ident.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.2f", s.freqMhz);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(s.type.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(s.name.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.3f %.3f", s.lat, s.lon);
        ImGui::TableNextColumn();
        if (ImGui::SmallButton("tune")) tuneFreq(a, s.freqMhz);
        if (!isIls(a)) {
            ImGui::SameLine(); if (ImGui::SmallButton("A")) { S.fix[0] = Slot(); S.fix[0].freqMhz = s.freqMhz; save(); }
            ImGui::SameLine(); if (ImGui::SmallButton("B")) { S.fix[1] = Slot(); S.fix[1].freqMhz = s.freqMhz; save(); }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

// the course deviation indicator: localizer needle (vertical, right = fly right) and glide slope needle (horizontal, down = fly down)
void cdi(ImVec2 p, float s, bool locOk, double locDdm, bool gsOk, double gsDdm, int marker, bool markerOn) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + s / 2, p.y + s / 2);
    const float R = s * 0.46f;
    dl->AddCircleFilled(c, R, IM_COL32(16, 18, 22, 255), 64);
    dl->AddCircle(c, R, IM_COL32(130, 136, 144, 255), 64, 1.5f * gUi);
    const float dot = R * 0.3f;   // two dots = full scale
    for (int k = -2; k <= 2; k++) {
        if (k == 0) continue;
        dl->AddCircle(ImVec2(c.x + k * dot, c.y), 3 * gUi, IM_COL32(220, 220, 220, 255), 12, 1.2f);
        dl->AddCircle(ImVec2(c.x, c.y + k * dot), 3 * gUi, IM_COL32(220, 220, 220, 255), 12, 1.2f);
    }
    dl->AddCircle(c, 6 * gUi, IM_COL32(220, 220, 220, 255), 20, 1.4f);
    auto clampf = [](double v) { return (float)std::max(-1.1, std::min(1.1, v)); };
    if (locOk) {
        const float x = c.x + clampf(locDdm / dect2::kIlsLocFullScaleDdm) * 2 * dot;
        dl->AddLine(ImVec2(x, c.y - R * 0.8f), ImVec2(x, c.y + R * 0.8f), IM_COL32(240, 240, 240, 255), 3 * gUi);
    } else dl->AddText(ImVec2(c.x - R * 0.75f, c.y - R * 0.55f), IM_COL32(230, 70, 60, 255), "LOC");
    if (gsOk) {
        const float y = c.y + clampf(gsDdm / dect2::kIlsGsFullScaleDdm) * 2 * dot;
        dl->AddLine(ImVec2(c.x - R * 0.8f, y), ImVec2(c.x + R * 0.8f, y), IM_COL32(240, 240, 240, 255), 3 * gUi);
    } else dl->AddText(ImVec2(c.x + R * 0.45f, c.y - R * 0.55f), IM_COL32(230, 70, 60, 255), "GS");
    static const char* nm[3] = {"O", "M", "I"};
    static const ImU32 lit[3] = {IM_COL32(70, 120, 255, 255), IM_COL32(255, 180, 40, 255), IM_COL32(245, 245, 245, 255)};
    for (int i = 0; i < 3; i++) {
        const ImVec2 q(c.x - R * 0.5f + i * R * 0.5f, c.y + R * 0.72f);
        dl->AddCircleFilled(q, 8 * gUi, marker == i + 1 && markerOn ? lit[i] : IM_COL32(50, 52, 56, 255), 20);
        const ImVec2 ts = ImGui::CalcTextSize(nm[i]);
        dl->AddText(ImVec2(q.x - ts.x / 2, q.y - ts.y / 2), marker == i + 1 && markerOn ? IM_COL32(0, 0, 0, 255) : IM_COL32(120, 124, 130, 255), nm[i]);
    }
}

void approachView(App& a) {
    const bool on = live(a);
    const dect2::VorTelemetry& t = a.rx.vor;
    const double gs = dect2::ilsGlideslopeFor(S.locMhz);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("localizer"); ImGui::SameLine(); ImGui::SetNextItemWidth(100 * gUi);
    if (ImGui::InputDouble("##locf", &S.locMhz, 0.05, 0.2, "%.2f")) { S.locMhz = std::max(108.10, std::min(111.95, S.locMhz)); }
    flowNext(); ImGui::TextDisabled("glide slope"); ImGui::SameLine();
    if (gs > 0) ImGui::Text("%.2f MHz", gs); else ImGui::TextDisabled("(not an ILS channel)");
    flowEnd();
    if (ImGui::Button("tune localizer")) { S.ilsHop = false; tuneFreq(a, S.locMhz); }
    if (gs > 0) { flowNext(); if (ImGui::Button("tune glide slope")) { S.ilsHop = false; tuneFreq(a, gs); } }
    flowNext();
    if (ImGui::Button("markers 75 MHz")) { S.ilsHop = false; tuneFreq(a, dect2::kIlsMarkerMhz); }
    if (gs > 0) {
        flowNext();
        if (ImGui::Checkbox("hop", &S.ilsHop)) { S.hopIdx = 0; S.hopT0 = 0; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Alternate between the localizer and its glide slope every 4 s, to show both needles.");
    }
    flowEnd();
    // the needles: the current function, and the other one kept from the last hop
    bool locOk = false, gsOk = false;
    double locD = 0, gsD = 0;
    const double now = ImGui::GetTime();
    if (on && t.function == dect2::NavFunction::Localizer && t.ilsValid) { locOk = true; locD = t.ddm; }
    else if (S.ilsHop && S.haveLoc && S.lastLoc.ilsValid) { locOk = true; locD = S.lastLoc.ddm; }
    if (on && t.function == dect2::NavFunction::Glideslope && t.ilsValid) { gsOk = true; gsD = t.ddm; }
    else if (S.ilsHop && S.haveGs && S.lastGs.ilsValid) { gsOk = true; gsD = S.lastGs.ddm; }
    (void)now;
    const bool side = ImGui::GetContentRegionAvail().x > 620 * gUi;   // the numbers beside the instrument, or under it in a narrow window
    const float s = std::min(ImGui::GetContentRegionAvail().x * (side ? 0.45f : 0.8f), std::min(ImGui::GetContentRegionAvail().y * (side ? 1.f : 0.55f) - 4 * gUi, 300 * gUi));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    cdi(p, std::max(80 * gUi, s), locOk, locD, gsOk, gsD, on ? t.marker : 0, on && t.markerOn);
    ImGui::Dummy(ImVec2(std::max(80 * gUi, s), std::max(80 * gUi, s)));
    if (side) ImGui::SameLine();
    ImGui::BeginGroup();
    if (!on) ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped");
    else {
        auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(120 * gUi); ImGui::PushFont(a.mono, 0); ImGui::Text(fmt, v...); ImGui::PopFont(); };
        kv("receiving", "%s", dect2::navFunctionName(t.function));
        if (t.function != dect2::NavFunction::Marker) {
            kv("DDM", t.ilsValid ? "%+.4f" : "-", t.ddm);
            kv("90 / 150 Hz", "%.1f %% / %.1f %%", t.m90 * 100, t.m150 * 100);
            kv("SDM", "%.1f %%", t.sdm * 100);
            kv("scatter", "%.4f", t.ddmStd);
            const double fs = t.function == dect2::NavFunction::Glideslope ? dect2::kIlsGsFullScaleDdm : dect2::kIlsLocFullScaleDdm;
            if (t.ilsValid) kv("needle", "%s %.0f %% of full scale", t.function == dect2::NavFunction::Glideslope ? (t.ddm > 0 ? "fly down" : "fly up") : (t.ddm > 0 ? "fly right" : "fly left"), std::min(100.0, std::fabs(t.ddm) / fs * 100));
        }
        static const char* mk[4] = {"none", "outer (400 Hz)", "middle (1300 Hz)", "inner (3000 Hz)"};
        kv("marker", "%s", mk[t.marker & 3]);
        kv("C/N (25 kHz)", "%.1f dB", t.snrDb);
        identLine(a, t);
    }
    ImGui::EndGroup();
}

// ---------------------------------------------------------------- hooks

void tick(App& a) {
    load();
    if (!a.engine.running()) { S.wasRunning = false; return; }
    dect2::VorReceiver& r = a.engine.vor();
    if (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute) { r.setVolume(a.volume); r.setMuted(a.muted); S.pushedVol = a.volume; S.pushedMute = a.muted; }
    if (!S.wasRunning || S.identFilter != S.pushedFilter) { r.setIdentFilter(S.identFilter); S.pushedFilter = S.identFilter; }
    S.wasRunning = true;
    if (!live(a)) return;
    const dect2::VorTelemetry& t = a.rx.vor;
    if (t.seq != S.histSeq && t.vorValid) {   // unwrapped, so a radial near north does not jump between 0 and 360 on the plot
        float v = (float)t.radialDeg;
        if (!S.radHist.empty()) v = S.radHist.back() + (float)std::remainder(t.radialDeg - S.radHist.back(), 360.0);
        S.radHist.push_back(v);
        if (S.radHist.size() > 480) S.radHist.pop_front();
    }
    S.histSeq = t.seq;
    if (t.function == dect2::NavFunction::Localizer) { S.lastLoc = t; S.haveLoc = true; }
    if (t.function == dect2::NavFunction::Glideslope) { S.lastGs = t; S.haveGs = true; }
    // hopping: fix stations, scan list, or localizer / glide slope
    std::vector<double> list;
    float dwell = S.dwell;
    if (isIls(a)) { if (S.ilsHop && dect2::ilsGlideslopeFor(S.locMhz) > 0) { list = {S.locMhz, dect2::ilsGlideslopeFor(S.locMhz)}; dwell = 4; } }
    else if (S.hop) list = {S.fix[0].freqMhz, S.fix[1].freqMhz};
    else if (S.scan) list = scanList();
    if (list.empty()) return;
    const double now = ImGui::GetTime();
    if (S.hopT0 == 0) { S.hopIdx = 0; S.hopT0 = now; tuneFreq(a, list[0]); return; }
    if (now - S.hopT0 < dwell) return;
    const int i = S.hopIdx % (int)list.size();
    Slot m; m.freqMhz = list[(size_t)i]; m.valid = t.vorValid; m.radial = t.radialDeg; m.sd = t.radialStdDeg; m.ident = t.ident; m.when = now; m.cnr = t.snrDb;
    if (!isIls(a)) {
        if (S.hop) { if (m.valid || m.ident.size()) S.fix[i] = m; }
        else {
            bool found = false;
            for (auto& s : S.scanRes) if (std::fabs(s.freqMhz - m.freqMhz) < 0.001) { s = m; found = true; }
            if (!found) S.scanRes.push_back(m);
        }
    }
    S.hopIdx = (i + 1) % (int)list.size();
    S.hopT0 = now;
    tuneFreq(a, list[(size_t)S.hopIdx]);
}

void tab(App& a) {
    load();
    if (isIls(a)) {
        subNav("ilsv", S.viewIls, {"Approach", "Stations"});
        if (S.viewIls == 0) approachView(a); else stationsView(a);
    } else {
        subNav("vorv", S.viewVor, {"Radial", "Fix", "Stations"});
        if (S.viewVor == 0) radialView(a); else if (S.viewVor == 1) fixView(a); else stationsView(a);
    }
}

void list(App& a) {
    load();
    if (isIls(a)) {   // the 40 ILS channels with their glide paths
        if (!ImGui::BeginTable("##ilsl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
        ImGui::TableSetupColumn("Localizer"); ImGui::TableSetupColumn("Glide slope", ImGuiTableColumnFlags_WidthStretch);
        for (int i = 0; i < dect2::ilsChannelCount(); i++) {
            double l, g; dect2::ilsChannel(i, l, g);
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            char b[24]; snprintf(b, sizeof b, "%.2f##il%d", l, i);
            if (ImGui::Selectable(b, std::fabs(a.freqMhz - l) < 0.001 || std::fabs(a.freqMhz - g) < 0.001, ImGuiSelectableFlags_SpanAllColumns)) { S.locMhz = l; S.ilsHop = false; tuneFreq(a, l); }
            ImGui::TableNextColumn(); ImGui::TextDisabled("%.2f", g);
        }
        ImGui::EndTable();
        return;
    }
    for (int i = 0; i < 2; i++) {
        const Slot& s = S.fix[i];
        ImGui::TextColored(i ? ImVec4(0.94f, 0.70f, 0.35f, 1) : ImVec4(0.43f, 0.82f, 0.90f, 1), "%s %.2f", i ? "B" : "A", s.freqMhz);
        ImGui::SameLine();
        if (s.valid) ImGui::Text("%s %05.1f", s.ident.c_str(), s.radial); else ImGui::TextDisabled("-");
    }
    ImGui::Separator();
    for (const auto& s : S.scanRes) ImGui::Text("%.2f %s %s", s.freqMhz, s.ident.c_str(), s.valid ? "" : "-");
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::VorTelemetry& t = a.rx.vor;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("state", "%s", dect2::vorSummary(t).c_str());
    kv("function", "%s (%s)", dect2::navFunctionName(t.function), t.dialHz > 0 ? "by the frequency" : "frequency unknown");
    kv("carrier", "%+.0f Hz, C/N %.1f dB in 25 kHz", t.cfoHz, t.snrDb);
    if (t.function == dect2::NavFunction::Vor) {
        kv("30 Hz", "AM %.1f %%, FM %.0f Hz deviation", t.am30 * 100, t.fmDevHz);
        kv("subcarrier", "%.1f Hz, %.1f %% AM", t.subcarrierHz, t.sub9960 * 100);
        kv("radial", "%05.1f (raw %05.1f, scatter %.2f)", t.radialDeg, t.radialRawDeg, t.radialStdDeg);
    } else kv("tones", "90 Hz %.2f %%, 150 Hz %.2f %%, DDM %+.4f", t.m90 * 100, t.m150 * 100, t.ddm);
    kv("measurements", "%llu good, %llu without signal", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kv("identification", "%s (%llu words)", t.ident.empty() ? "-" : t.ident.c_str(), (unsigned long long)t.identCount);
    kv("input", "%.3f Msps, %.1f dBFS; envelope at %.0f Hz", t.inputRate / 1e6, t.levelDb, t.demodRate);
    kv("signal time", "%.1f s", t.timeSec);
    if (ImGui::Checkbox("take the 1020 Hz identification out of the voice", &S.identFilter)) save();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Carrier", !on ? 0 : a.rx.vor.state >= 1 ? 1 : 2); flowNext(10 * gUi);
    lamp(isIls(a) ? "DDM" : "Radial", !on ? 0 : a.rx.vor.dataValid ? 1 : 2); flowNext(10 * gUi);
    lamp("Ident", !on ? 0 : !a.rx.vor.ident.empty() ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::vorSummary(a.rx.vor).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = isIls(a) ? "ILS" : "VOR";
    if (live(a)) l2 = dect2::vorSummary(a.rx.vor);
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    static const char* kinds[7] = {"conventional VOR", "Doppler VOR", "localizer", "glide slope", "outer marker", "middle marker", "inner marker"};
    int k = std::max(0, std::min(6, sc.modeOpt[0]));
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::Combo("##vork", &k, kinds, 7)) { sc.modeOpt[0] = k; changed = true; }
    flowNext();
    ImGui::SetNextItemWidth(150 * gUi);
    if (k <= 1) { float r = (float)sc.modeVal[0]; if (ImGui::SliderFloat("##vorr", &r, 0, 359.9f, "radial %.1f")) { sc.modeVal[0] = r; changed = true; } }
    else if (k <= 3) { float d = (float)sc.modeVal[0]; if (ImGui::SliderFloat("##ilsd", &d, -0.3f, 0.3f, "DDM %+.3f")) { sc.modeVal[0] = d; changed = true; } }
}

} // namespace

extern const ModeUi kVorUi;
const ModeUi kVorUi = {
    .sideTitle = "BEACONS",
    .tabName = "Navigation",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .synth = synth,
    .tick = tick,
};

// the two fix stations' radials for the results file (results_ui.cpp): kept until they are measured again
std::string vorResultsSave() { return dect2::results::pack(S.fix[0], S.fix[1]); }
bool vorResultsLoad(const std::string& blob) {
    Slot a, b;
    if (!dect2::results::unpack(blob, a, b)) return false;
    a.when = b.when = 0;                                // the times were of the last run's clock
    a.freqMhz = S.fix[0].freqMhz; b.freqMhz = S.fix[1].freqMhz;   // the stations chosen are the settings' (prefs)
    if (a.valid) S.fix[0] = a;
    if (b.valid) S.fix[1] = b;
    return true;
}

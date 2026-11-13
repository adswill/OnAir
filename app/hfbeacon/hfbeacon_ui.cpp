// HF beacons screens: a propagation monitor. A table of the beacon list (NCDXF/IARU, Long Dashes, single letters, your own): lamp, name,
// frequency, last heard (UTC), SNR now, best in the last hour and 24 hours, distance and bearing from the receiver, a trend; a "bands open"
// summary (10 / 12 / 15 / 17 / 20 m and the others: the beacons of the band heard now and the farthest); a great-circle map from the receiver to
// every beacon heard, coloured by SNR; the 24 hour history of the selected beacon; and the settings (the list file, frequency error, clock
// offset, stepping through bands when the capture is narrower than the list). Clicking a row tunes and listens (Listen).
#include "dect2/common/timecompat.h"
#include "app.h"
#include "../d40/d40_ui.h"
#include "hfprop_ui.h"
#include "iono_obs_ui.h"
#include "../adsb/adsb_map.h"
#include "dect2/hfbeacon/hfbeacon_list.h"
#include "dect2/hfbeacon/hfbeacon_rx.h"
#include "dect2/hfbeacon/hfbeacon_tel.h"
#include "dect2/geo/maidenhead.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace dect2;

struct State {
    bool loaded = false, pushed = false;
    int view = 0;                          // 0 beacons, 1 bands, 2 map, 3 history, 4 settings
    std::vector<hfb::Beacon> list;
    std::string listPath, listErr;
    int sel = -1, pushedSel = -2;
    char filter[64] = "";
    int kindFilter = 0;                    // 0 all, 1 heard or in span only, 2 NCDXF, 3 Long Dashes, 4 single letters, 5 other
    int sort = 0;                          // 0 list order, 1 heard first, 2 SNR, 3 distance
    bool listen = false;
    double ppm = 0, clockOff = 0;
    bool hop = true;
    int dwell = 60;
    bool kinds[4] = {true, true, true, true};
    // stepping
    double lastHop = 0, pauseUntil = 0, lastTuned = 0, planRate = 0;
    int planIdx = 0, planSig = 0;
    std::vector<hfb::Centre> plan;
    // map
    adsbmap::View map;
    bool mapInit = false, mapRecent = true;
};
State S;

bool live(const App& a) { return a.rx.standard == 68; }   // the engine reports its standard code minus one

double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
bool writeFile(const std::string& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
    return (bool)f;
}

void pushList(App& a);

void loadList(App& a, bool restoreDefault) {
    S.listPath = plat::dataDir() + "/hf-beacons.json";
    S.listErr.clear();
    std::string txt = restoreDefault ? std::string() : readFile(S.listPath);
    std::vector<hfb::Beacon> v;
    if (txt.empty()) {
        if (restoreDefault && !readFile(S.listPath).empty()) writeFile(S.listPath + ".bak", readFile(S.listPath));
        v = hfb::defaultBeacons();
        writeFile(S.listPath, hfb::toJson(v));
    } else if (!hfb::fromJson(txt, v, &S.listErr)) {
        S.listErr = "hf-beacons.json: " + S.listErr + " (the built-in list is used until it is fixed)";
        v = hfb::defaultBeacons();
    }
    S.list = std::move(v);
    S.sel = -1; S.pushedSel = -2;
    S.plan.clear(); S.planSig++;
    S.pushed = false;
    (void)a;
}

void load(App& a) {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& p = plat::prefs();
    S.listen = p.getB("hfbListen", false);
    S.view = (int)std::max(0L, std::min(6L, p.getI("hfbView", 0)));
    S.ppm = p.getD("hfbPpm", 0); S.clockOff = p.getD("hfbClock", 0);
    S.hop = p.getB("hfbHop", true); S.dwell = (int)p.getI("hfbDwell", 60);
    for (int k = 0; k < 4; k++) { char b[24]; snprintf(b, sizeof b, "hfbKind%d", k); S.kinds[k] = p.getB(b, true); }
    loadList(a, false);
}

void saveSettings() {
    plat::Prefs& p = plat::prefs();
    p.setD("hfbPpm", S.ppm); p.setD("hfbClock", S.clockOff);
    p.setB("hfbHop", S.hop); p.setI("hfbDwell", S.dwell);
    for (int k = 0; k < 4; k++) { char b[24]; snprintf(b, sizeof b, "hfbKind%d", k); p.setB(b, S.kinds[k]); }
}

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

std::string utcText(const HfbTelemetry& t, double v) {
    if (v <= 0) return "-";
    char b[32];
    if (!t.clockValid) { snprintf(b, sizeof b, "t+%02d:%02d", (int)(v / 60), (int)std::fmod(v, 60)); return b; }
    const time_t tt = (time_t)v;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    dect2::gmTime(tt, &g);
#endif
    snprintf(b, sizeof b, "%02d:%02d:%02d", g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

// SNR colour: red under 0 dB, amber to 10, green above
ImU32 snrCol(float snr, int alpha = 255) {
    if (std::isnan(snr)) return IM_COL32(120, 120, 120, alpha);
    if (snr < 0) return IM_COL32(220, 90, 80, alpha);
    if (snr < 10) return IM_COL32(230, 180, 70, alpha);
    if (snr < 20) return IM_COL32(150, 200, 90, alpha);
    return IM_COL32(90, 210, 130, alpha);
}

void lampDot(ImDrawList* dl, ImVec2 c, float r, const dect2::HfbState& s) {
    if (s.heard && s.conf >= 2) dl->AddCircleFilled(c, r, IM_COL32(90, 210, 130, 255));
    else if (s.heard || s.conf >= 1) dl->AddCircleFilled(c, r, IM_COL32(230, 180, 70, 255));
    else if (s.inSpan) dl->AddCircle(c, r, IM_COL32(140, 150, 160, 255), 12, 1.2f);
    else dl->AddCircle(c, r, IM_COL32(80, 84, 90, 160), 12, 1.0f);
}

void sparkline(ImDrawList* dl, ImVec2 p0, ImVec2 sz, const std::vector<float>& v) {
    dl->AddRectFilled(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), IM_COL32(255, 255, 255, 12));
    if (v.empty()) return;
    const size_t n = std::max<size_t>(v.size(), 24);
    const float bw = sz.x / (float)n;
    for (size_t i = 0; i < v.size(); i++) {
        const float x = p0.x + (float)(i + (n - v.size())) * bw;
        if (std::isnan(v[i])) { dl->AddLine(ImVec2(x + bw * 0.5f, p0.y + sz.y - 1), ImVec2(x + bw * 0.5f, p0.y + sz.y), IM_COL32(120, 120, 120, 180)); continue; }
        const float h = std::max(1.5f, std::min(1.f, (v[i] + 10.f) / 40.f) * sz.y);
        dl->AddRectFilled(ImVec2(x, p0.y + sz.y - h), ImVec2(x + std::max(1.f, bw - 0.5f), p0.y + sz.y), snrCol(v[i]));
    }
}

bool passes(const hfb::Beacon& b, const dect2::HfbState& st) {
    if (S.kindFilter == 1 && !(st.heard || st.inSpan)) return false;
    if (S.kindFilter == 2 && b.kind != hfb::Ncdxf) return false;
    if (S.kindFilter == 3 && b.kind != hfb::Dashes) return false;
    if (S.kindFilter == 4 && b.kind != hfb::Letter) return false;
    if (S.kindFilter == 5 && b.kind != hfb::Carrier) return false;
    if (S.filter[0]) {
        std::string q = S.filter, n = b.name + " " + hfb::bandName(b.khz);
        for (char& c : q) c = (char)std::tolower((unsigned char)c);
        for (char& c : n) c = (char)std::tolower((unsigned char)c);
        char f[24]; snprintf(f, sizeof f, " %.1f", b.khz); n += f;
        if (n.find(q) == std::string::npos) return false;
    }
    return true;
}

void selectBeacon(App& a, int i, bool tune) {
    S.sel = i;
    if (i < 0 || i >= (int)S.list.size()) return;
    const hfb::Beacon& b = S.list[(size_t)i];
    S.pauseUntil = nowSec() + 120;
    a.engine.hfbeacon().setListenFreqHz(b.khz * 1e3);
    if (!tune) return;
    // in the capture already: only the listening frequency moves; else the radio goes 25 kHz above the beacon (clear of the centre spike)
    const dect2::HfbTelemetry& t = a.rx.hfb;
    const bool in = live(a) && i < (int)t.beacons.size() && t.beacons[(size_t)i].inSpan;
    if (!in) { tuneFreq(a, b.khz / 1e3 + 0.025); S.lastTuned = a.freqMhz; }
}

void beaconsView(App& a) {
    const bool on = live(a);
    static const dect2::HfbTelemetry none;
    const dect2::HfbTelemetry& t = on ? a.rx.hfb : none;
    ImGui::SetNextItemWidth(std::min(170 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##hfbf", "filter: name, band, kHz", S.filter, sizeof S.filter);
    flowNext();
    ImGui::SetNextItemWidth(130 * gUi);
    static const char* const kf[] = {"All beacons", "In the capture", "NCDXF/IARU", "Long Dashes", "Single letters", "Your own"};
    if (ImGui::BeginCombo("##hfbk", kf[S.kindFilter])) { for (int i = 0; i < 6; i++) if (ImGui::Selectable(kf[i], S.kindFilter == i)) S.kindFilter = i; ImGui::EndCombo(); }
    flowNext();
    ImGui::SetNextItemWidth(120 * gUi);
    static const char* const so[] = {"List order", "Heard first", "Best SNR", "Distance"};
    if (ImGui::BeginCombo("##hfbs", so[S.sort])) { for (int i = 0; i < 4; i++) if (ImGui::Selectable(so[i], S.sort == i)) S.sort = i; ImGui::EndCombo(); }
    flowNext();
    if (resultsClearButton("hfbeacon")) a.engine.clearResults(69);
    if (!S.listErr.empty()) ImGui::TextColored(pal::warnAmber(), "%s", S.listErr.c_str());
    if (on && t.clockValid && std::fabs(t.clockOffsetSec) < 1e-9) {
        // the NCDXF slots are 10 s long and follow the computer clock
        const time_t now = time(nullptr);
        struct tm lt, gm;
        dect2::localTime(now, &lt);
        dect2::gmTime(now, &gm);
        gm.tm_isdst = lt.tm_isdst;
        const long off = (long)difftime(mktime(&lt), mktime(&gm));   // seconds east of UTC, without tm_gmtoff (not on Windows)
        ImGui::TextDisabled("Times are UTC (your clock is UTC%+ld:%02ld). The NCDXF/IARU slots are told from the computer clock: keep it within about a second of true time, or set the offset in Settings.",
                            off / 3600, std::labs(off % 3600) / 60);
    }
    std::vector<int> idx;
    for (int i = 0; i < (int)S.list.size(); i++) {
        const dect2::HfbState blank;
        const dect2::HfbState& st = (size_t)i < t.beacons.size() ? t.beacons[(size_t)i] : blank;
        if (passes(S.list[(size_t)i], st)) idx.push_back(i);
    }
    if (on && S.sort > 0)
        std::stable_sort(idx.begin(), idx.end(), [&](int x, int y) {
            const auto &p = t.beacons[(size_t)x], &q = t.beacons[(size_t)y];
            if (S.sort == 1) return (p.heard ? p.lastHeard : 0) > (q.heard ? q.lastHeard : 0);
            if (S.sort == 2) return (std::isnan(p.best24h) ? -99.f : p.best24h) > (std::isnan(q.best24h) ? -99.f : q.best24h);
            return (std::isnan(p.distKm) ? 0 : p.distKm) > (std::isnan(q.distKm) ? 0 : q.distKm);
        });
    if (!ImGui::BeginTable("##hfbt", 10, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 22 * gUi);
    ImGui::TableSetupColumn("Beacon", ImGuiTableColumnFlags_WidthFixed, 210 * gUi);
    ImGui::TableSetupColumn("kHz"); ImGui::TableSetupColumn("Last heard UTC"); ImGui::TableSetupColumn("SNR now"); ImGui::TableSetupColumn("1 h best"); ImGui::TableSetupColumn("24 h best");
    ImGui::TableSetupColumn("Distance"); ImGui::TableSetupColumn("Trend", ImGuiTableColumnFlags_WidthFixed, 80 * gUi); ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGuiListClipper clip;
    clip.Begin((int)idx.size());
    while (clip.Step())
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
            const int i = idx[(size_t)r];
            const hfb::Beacon& b = S.list[(size_t)i];
            const dect2::HfbState blank;
            const dect2::HfbState& st = (size_t)i < t.beacons.size() ? t.beacons[(size_t)i] : blank;
            ImGui::TableNextRow();
            ImGui::PushID(i);
            ImGui::TableNextColumn();
            const ImVec2 c0 = ImGui::GetCursorScreenPos();
            lampDot(ImGui::GetWindowDrawList(), ImVec2(c0.x + 8 * gUi, c0.y + ImGui::GetTextLineHeight() * 0.5f), 4.5f * gUi, st);
            ImGui::TableNextColumn();
            if (ImGui::Selectable(b.name.c_str(), S.sel == i, ImGuiSelectableFlags_SpanAllColumns)) selectBeacon(a, i, true);
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("%s", b.name.c_str());
                ImGui::TextDisabled("%s, %s", hfb::kindName(b.kind), b.mode.c_str());
                if (!b.schedule.empty()) ImGui::TextDisabled("%s", b.schedule.c_str());
                ImGui::TextDisabled("%.2f, %.2f%s", b.lat, b.lon, b.uncertain ? "  (uncertain)" : "");
                ImGui::PushTextWrapPos(420 * gUi); ImGui::TextDisabled("Source: %s", b.source.c_str()); ImGui::PopTextWrapPos();
                if (!st.note.empty()) ImGui::Text("%s", st.note.c_str());
                ImGui::TextDisabled("Click to select, tune and listen");
                ImGui::EndTooltip();
            }
            ImGui::TableNextColumn(); ImGui::Text("%.1f%s", b.khz, b.uncertain ? " ?" : "");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(utcText(t, st.lastHeard).c_str());
            ImGui::TableNextColumn();
            if (st.heard && !std::isnan(st.snr)) { ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(snrCol(st.snr))); ImGui::Text("%+.1f dB", (double)st.snr); ImGui::PopStyleColor(); }
            else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (!std::isnan(st.best1h)) ImGui::Text("%+.1f", (double)st.best1h); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (!std::isnan(st.best24h)) ImGui::Text("%+.1f", (double)st.best24h); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            if (!std::isnan(st.distKm)) ImGui::Text("%.0f km %.0f\xC2\xB0", st.distKm, st.bearing);
            else { double lat, lon, km, br; receiverLocation(lat, lon); dect2::greatCircle(lat, lon, b.lat, b.lon, km, br); ImGui::TextDisabled("%.0f km %.0f\xC2\xB0", km, br); }
            ImGui::TableNextColumn();
            {
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                sparkline(ImGui::GetWindowDrawList(), p0, ImVec2(76 * gUi, ImGui::GetTextLineHeight()), st.spark);
                ImGui::Dummy(ImVec2(76 * gUi, ImGui::GetTextLineHeight()));
            }
            ImGui::TableNextColumn();
            if (st.heard && b.kind == hfb::Ncdxf && st.steps > 0) ImGui::Text("%d of 4 power steps", st.steps);
            else if (!st.note.empty() && (st.heard || st.conf)) ImGui::TextUnformatted(st.note.c_str());
            else if (!st.inSpan && on) ImGui::TextDisabled("outside the capture");
            ImGui::PopID();
        }
    ImGui::EndTable();
}

void bandsView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::HfbTelemetry& t = a.rx.hfb;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Bands open: of the beacons whose frequency is in the capture, how many are heard now (NCDXF/IARU: within the last 3 minute cycle; single letters 1 minute; "
                        "Long Dashes 90 s), and the farthest one. A band with nothing in the capture is not listened to: the capture covers %.2f - %.2f MHz.", t.spanLoHz / 1e6, t.spanHiHz / 1e6);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (!ImGui::BeginTable("##hfbb", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) return;
    ImGui::TableSetupColumn("Band"); ImGui::TableSetupColumn("In the list"); ImGui::TableSetupColumn("In the capture"); ImGui::TableSetupColumn("Heard now");
    ImGui::TableSetupColumn("Open", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Farthest");
    ImGui::TableHeadersRow();
    for (const auto& b : t.bands) {
        if (b.total == 0) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(b.name.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%d", b.total);
        ImGui::TableNextColumn(); ImGui::Text("%d", b.inSpan);
        ImGui::TableNextColumn(); ImGui::Text("%d", b.heard);
        ImGui::TableNextColumn();
        const float f = b.inSpan ? (float)b.heard / (float)b.inSpan : 0.f;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float w = std::max(60.f * gUi, ImGui::GetContentRegionAvail().x - 4 * gUi), h = ImGui::GetTextLineHeight();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(255, 255, 255, 18));
        if (b.inSpan) dl->AddRectFilled(p0, ImVec2(p0.x + w * f, p0.y + h), f > 0 ? IM_COL32(90, 210, 130, 220) : IM_COL32(0, 0, 0, 0));
        char tx[32]; snprintf(tx, sizeof tx, b.inSpan ? "%.0f %%" : "not in the capture", f * 100);
        dl->AddText(ImVec2(p0.x + 4 * gUi, p0.y), IM_COL32(235, 235, 235, 255), tx);
        ImGui::Dummy(ImVec2(w, h));
        ImGui::TableNextColumn();
        if (!std::isnan(b.bestKm)) ImGui::Text("%.0f km", b.bestKm); else ImGui::TextDisabled("-");
    }
    ImGui::EndTable();
}

// a great circle as short segments (a polyline broken where it crosses the date line)
void greatCircleLine(ImDrawList* dl, double la1, double lo1, double la2, double lo2, ImU32 col, float w) {
    const double r = M_PI / 180;
    const double x1 = std::cos(la1 * r) * std::cos(lo1 * r), y1 = std::cos(la1 * r) * std::sin(lo1 * r), z1 = std::sin(la1 * r);
    const double x2 = std::cos(la2 * r) * std::cos(lo2 * r), y2 = std::cos(la2 * r) * std::sin(lo2 * r), z2 = std::sin(la2 * r);
    const double dot = std::max(-1.0, std::min(1.0, x1 * x2 + y1 * y2 + z1 * z2)), om = std::acos(dot);
    if (om < 1e-6) return;
    const int n = std::max(8, (int)(om / r / 3));
    ImVec2 prev; double prevLon = 0;
    for (int i = 0; i <= n; i++) {
        const double f = (double)i / n, A = std::sin((1 - f) * om) / std::sin(om), B = std::sin(f * om) / std::sin(om);
        const double x = A * x1 + B * x2, y = A * y1 + B * y2, z = A * z1 + B * z2;
        const double lat = std::atan2(z, std::sqrt(x * x + y * y)) / r, lon = std::atan2(y, x) / r;
        const ImVec2 p = adsbmap::project(lat, lon);
        if (i > 0 && std::fabs(lon - prevLon) < 180) dl->AddLine(prev, p, col, w);
        prev = p; prevLon = lon;
    }
}

void mapView(App& a) {
    const bool on = live(a);
    static const dect2::HfbTelemetry none;
    const dect2::HfbTelemetry& t = on ? a.rx.hfb : none;
    if (!S.mapInit) { S.mapInit = true; receiverLocation(S.map.lat, S.map.lon); S.map.zoom = 2; S.map.lat = 20; S.map.lon = 20; }
    ImGui::Checkbox("Also heard in the last hour##hfbmr", &S.mapRecent);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, std::max(160 * gUi, ImGui::GetContentRegionAvail().y));
    adsbmap::draw(S.map, size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    double rl, ro; receiverLocation(rl, ro);
    int drawn = 0;
    for (size_t i = 0; i < S.list.size() && i < t.beacons.size(); i++) {
        const hfb::Beacon& b = S.list[i];
        const dect2::HfbState& st = t.beacons[i];
        const bool now = st.heard, recent = S.mapRecent && !now && !std::isnan(st.best1h);
        const ImVec2 q = adsbmap::project(b.lat, b.lon);
        if (now || recent) {
            const float snr = now ? st.snr : st.best1h;
            greatCircleLine(dl, rl, ro, b.lat, b.lon, snrCol(snr, now ? 230 : 90), (now ? 2.2f : 1.2f) * gUi);
            dl->AddCircleFilled(q, (now ? 5.f : 3.5f) * gUi, snrCol(snr, now ? 255 : 150));
            if (now) { char tx[64]; snprintf(tx, sizeof tx, "%s %+.0f dB", b.name.substr(0, b.name.find(' ')).c_str(), (double)snr); dl->AddText(ImVec2(q.x + 7 * gUi, q.y - 7 * gUi), IM_COL32(240, 240, 240, 255), tx); }
            drawn++;
        } else if (b.kind != hfb::Letter || (int)i % 8 == 0) {
            dl->AddCircle(q, 3 * gUi, IM_COL32(150, 155, 160, 140), 10, 1.f);
        }
    }
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+##hfbz", ImVec2(26 * gUi, 0))) S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom in"));
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-##hfbz", ImVec2(26 * gUi, 0))) S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom out"));
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never your location or the beacons");
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
    adsbmap::legend(size.x, "%d beacon%s heard: line colour = SNR (red < 0 dB, amber < 10, green)", drawn, drawn == 1 ? "" : "s");
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
    ImGui::Dummy(ImVec2(0, 0));
}

void historyView(App& a) {
    if (S.sel < 0 || S.sel >= (int)S.list.size()) { ImGui::TextDisabled("Select a beacon in the table to see its last 24 hours."); return; }
    const hfb::Beacon& b = S.list[(size_t)S.sel];
    ImGui::Text("%s  %.1f kHz", b.name.c_str(), b.khz);
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::HfbTelemetry& t = a.rx.hfb;
    const auto& h = t.history;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = std::max(200 * gUi, ImGui::GetContentRegionAvail().x - 8 * gUi), hh = std::max(120 * gUi, std::min(260 * gUi, ImGui::GetContentRegionAvail().y - 60 * gUi));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + hh), IM_COL32(255, 255, 255, 12));
    const double now = t.nowUtc;
    for (int k = 0; k <= 24; k += 4) {   // hour marks
        const float x = p0.x + w * (float)(1.0 - k / 24.0);
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + hh), IM_COL32(255, 255, 255, 30));
        char tx[16]; snprintf(tx, sizeof tx, k ? "-%d h" : "now", k);
        dl->AddText(ImVec2(x - (k ? 10.f : 16.f) * gUi, p0.y + hh + 2), IM_COL32(170, 170, 170, 255), tx);
    }
    for (int s = 0; s <= 40; s += 10) {
        const float y = p0.y + hh * (1.f - (s + 10.f) / 50.f);
        dl->AddLine(ImVec2(p0.x, y), ImVec2(p0.x + w, y), IM_COL32(255, 255, 255, 25));
        char tx[16]; snprintf(tx, sizeof tx, "%d dB", s);
        dl->AddText(ImVec2(p0.x + 2, y - ImGui::GetTextLineHeight()), IM_COL32(150, 150, 150, 255), tx);
    }
    const float bw = std::max(1.5f, w / 1440.f * (b.kind == hfb::Ncdxf ? 3.f : 1.f));
    int heard = 0;
    for (const auto& p : h) {
        const double age = now - p.t;
        if (age < 0 || age > 86400) continue;
        const float x = p0.x + w * (float)(1.0 - age / 86400.0);
        if (!p.heard) { dl->AddLine(ImVec2(x, p0.y + hh - 1), ImVec2(x, p0.y + hh - 4 * gUi), IM_COL32(120, 120, 120, 150)); continue; }
        heard++;
        const float y = p0.y + hh * (1.f - std::max(0.f, std::min(1.f, (p.snr + 10.f) / 50.f)));
        dl->AddRectFilled(ImVec2(x - bw * 0.5f, y), ImVec2(x + bw * 0.5f, p0.y + hh), snrCol(p.snr));
    }
    ImGui::Dummy(ImVec2(w, hh + ImGui::GetTextLineHeight() + 4));
    ImGui::TextDisabled("%zu listens (%s), heard %d times; bar = best SNR in 100 Hz, grey tick = listened without hearing it.", h.size(), b.kind == hfb::Ncdxf ? "one per 3 minute cycle" : "one per minute", heard);
}

void settingsView(App& a) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("The beacon list is the file below (JSON: name, khz, pattern ncdxf / letter / dashes / carrier, lat, lon, source...). Edit it with any text editor and press Reload. "
                        "Pressing Restore writes the built-in list again (the old file is kept as .bak).");
    ImGui::PopTextWrapPos();
    ImGui::TextUnformatted(S.listPath.c_str());
    if (ImGui::SmallButton("Copy path")) ImGui::SetClipboardText(S.listPath.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload")) loadList(a, false);
    ImGui::SameLine();
    if (ImGui::SmallButton("Restore the built-in list")) loadList(a, true);
    if (!S.listErr.empty()) ImGui::TextColored(pal::warnAmber(), "%s", S.listErr.c_str());
    ImGui::TextDisabled("%zu beacons", S.list.size());
    ImGui::Separator();
    bool ch = false;
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Frequency error");
    ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    if (ImGui::InputDouble("##hfbppm", &S.ppm, 0, 0, "%.1f ppm")) { S.ppm = std::max(-100.0, std::min(100.0, S.ppm)); ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The radio's reference error: 1 ppm is 14 Hz at 14 MHz. The single-letter beacons are 100 Hz apart and the detector watches a 70 Hz wide channel,\n"
                                                  "so it needs a reference within a few ppm (a GPSDO, or a TCXO radio). The Long Dashes are searched +-1 kHz and need none.");
    ImGui::SameLine(0, 14 * gUi);
    ImGui::TextUnformatted("Clock ahead of UTC");
    ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    if (ImGui::InputDouble("##hfbclk", &S.clockOff, 0, 0, "%.1f s")) { S.clockOff = std::max(-30.0, std::min(30.0, S.clockOff)); ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How far the computer clock runs ahead of true UTC. The NCDXF/IARU beacons are told apart by the 10 s slot of the 3 minute cycle, so\nthis must be right to about a second.");
    ImGui::Separator();
    ImGui::TextUnformatted("When the capture is narrower than the list");
    if (ImGui::Checkbox("Step through the bands##hfbhop", &S.hop)) ch = true;
    ImGui::SameLine(0, 10 * gUi); ImGui::SetNextItemWidth(80 * gUi);
    if (ImGui::InputInt("s per stop##hfbd", &S.dwell, 0, 0)) { S.dwell = std::max(10, std::min(600, S.dwell)); ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("An NCDXF/IARU beacon is on a band for 10 s once in 3 minutes: 180 s per stop hears all 18 on every band in the capture.\nThe Long Dashes cycle takes 27 s, the single letters repeat within seconds.");
    ImGui::TextDisabled("Watch");
    static const char* const kn[4] = {"Carriers", "NCDXF/IARU", "Single letters", "Long Dashes"};
    static const int kk[4] = {hfb::Carrier, hfb::Ncdxf, hfb::Letter, hfb::Dashes};
    for (int k = 0; k < 4; k++) { ImGui::SameLine(); if (ImGui::Checkbox(kn[k], &S.kinds[kk[k]])) { ch = true; S.plan.clear(); S.planSig++; } }
    if (live(a)) {
        std::vector<hfb::Beacon> sub;
        for (const auto& b : S.list) if (S.kinds[b.kind]) sub.push_back(b);
        const auto p = hfb::planCentres(sub, a.rx.hfb.rate);
        ImGui::TextDisabled("With %.2f Msps: %zu stop%s cover the %zu beacons watched.", a.rx.hfb.rate / 1e6, p.size(), p.size() == 1 ? "" : "s", sub.size());
    }
    if (ch) { saveSettings(); S.pushed = false; }
}

void hop(App& a) {
    if (!S.hop || !a.engine.running() || !live(a) || a.rx.hfb.rate <= 0) return;
    const double now = nowSec();
    if (std::fabs(a.freqMhz - S.lastTuned) > 0.002 && S.lastTuned != 0) { S.pauseUntil = now + 300; S.lastTuned = 0; }   // the user tuned elsewhere
    if (now < S.pauseUntil) return;
    if (S.plan.empty() || S.planRate != a.rx.hfb.rate) {
        std::vector<hfb::Beacon> sub;
        for (const auto& b : S.list) if (S.kinds[b.kind]) sub.push_back(b);
        S.plan = hfb::planCentres(sub, a.rx.hfb.rate);
        S.planRate = a.rx.hfb.rate; S.planIdx = -1; S.lastHop = 0;
    }
    if (S.plan.size() < 2) return;
    if (S.lastHop == 0 || now - S.lastHop >= S.dwell) {
        S.planIdx = (S.planIdx + 1) % (int)S.plan.size();
        S.lastHop = now;
        S.lastTuned = S.plan[(size_t)S.planIdx].centerHz / 1e6;
        tuneFreq(a, S.lastTuned);
    }
}

// ---------------------------------------------------------------- hooks

void tick(App& a) {
    load(a);
    dect2::HfbReceiver& r = a.engine.hfbeacon();
    if (!S.pushed) {
        r.setBeacons(S.list); r.setPpm(S.ppm); r.setClockOffset(S.clockOff);
        S.pushed = true;
    }
    double lat, lon; receiverLocation(lat, lon);
    r.setObserver(receiverLocationSet(), lat, lon);
    if (S.sel != S.pushedSel) { r.setSelected(S.sel); S.pushedSel = S.sel; }
    r.setListen(S.listen); r.setVolume(a.volume); r.setMuted(a.muted);
    if (S.sel >= 0 && S.sel < (int)S.list.size()) r.setListenFreqHz(S.list[(size_t)S.sel].khz * 1e3);
    hop(a);
    if (live(a)) {
        hfprop_ui::feedBeacons(S.list, a.rx.hfb); ionoobs_ui::feedBeacons(S.list, a.rx.hfb);
        std::vector<double> khz; for (const auto& b : S.list) khz.push_back(b.khz);
        d40HfbFeed(khz, a.rx.hfb);   // 4.0: hour-of-day best frequency (app/d40)
    }
}

void tab(App& a) {
    load(a);
    listenControls(a, S.listen, "hfbListen", "hfbeacon", "Play the selected beacon (USB, the carrier as a 700 Hz tone) on the sound card; detection is the same either way");
    flowNext(10 * gUi);
    if (S.hop && a.rx.standard == 68 && S.plan.size() > 1) ImGui::TextDisabled("stepping %zu stops, %d s each", S.plan.size(), S.dwell);
    const int v0 = S.view;
    subNav("hfbv", S.view, {"Beacons", "Bands open", "Map", "History", "Settings", tr("Propagation"), tr("My ionosphere")});
    if (v0 != S.view) plat::prefs().setI("hfbView", S.view);
    if (S.view == 0) beaconsView(a);
    else if (S.view == 1) bandsView(a);
    else if (S.view == 2) mapView(a);
    else if (S.view == 3) historyView(a);
    else if (S.view == 5) { d40HfbBestCard(); hfprop_ui::view(a); }
    else if (S.view == 6) ionoobs_ui::view(a);
    else settingsView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::HfbTelemetry& t = a.rx.hfb;
    std::vector<int> h;
    for (size_t i = 0; i < t.beacons.size(); i++) if (t.beacons[i].heard) h.push_back((int)i);
    std::sort(h.begin(), h.end(), [&](int x, int y) { return t.beacons[(size_t)x].snr > t.beacons[(size_t)y].snr; });
    if (!ImGui::BeginTable("##hfbl", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Beacon", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("kHz"); ImGui::TableSetupColumn("SNR");
    for (int i : h) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, pal::okGreen());
        if (ImGui::Selectable(S.list[(size_t)i].name.c_str(), S.sel == i, ImGuiSelectableFlags_SpanAllColumns)) selectBeacon(a, i, false);
        ImGui::PopStyleColor();
        ImGui::TableNextColumn(); ImGui::Text("%.1f", S.list[(size_t)i].khz);
        ImGui::TableNextColumn(); ImGui::Text("%+.0f", (double)t.beacons[(size_t)i].snr);
    }
    ImGui::EndTable();
    if (h.empty()) ImGui::TextDisabled("nothing heard yet");
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::HfbTelemetry& t = a.rx.hfb;
    kvRow(a, "state", "%s", dect2::hfbeaconSummary(t).c_str());
    kvRow(a, "capture", "%.3f MHz centre, %.3f Msps, %d narrow channels watched", t.centerHz / 1e6, t.rate / 1e6, t.channels);
    kvRow(a, "clock", t.clockValid ? "computer clock, offset %+.1f s" : "stream time from 0 (no radio: NCDXF slots are not told)", t.clockOffsetSec);
    kvRow(a, "frames", "%llu spectra of 13 - 21 ms", (unsigned long long)t.frames);
    kvRow(a, "frequency error", "%+.1f ppm applied", S.ppm);
    kvRow(a, "input", "%.1f dBFS", t.powerDb);
}

void decoder(App& a, bool&) {
    load(a);
    bool ch = false;
    ImGui::TextDisabled("ppm");
    ImGui::SameLine(0, 5 * gUi); ImGui::SetNextItemWidth(70 * gUi);
    if (ImGui::InputDouble("##hfbppm2", &S.ppm, 0, 0, "%.1f")) { S.ppm = std::max(-100.0, std::min(100.0, S.ppm)); ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The radio's frequency error: signals are looked for at f * (1 + ppm * 1e-6)");
    if (ch) { saveSettings(); S.pushed = false; tick(a); }
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("In capture", !on ? 0 : a.rx.hfb.channels > 0 ? 1 : 2); flowNext(10 * gUi);
    lamp("Heard", !on ? 0 : a.rx.hfb.heardNow > 0 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::hfbeaconSummary(a.rx.hfb).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "HF beacons";
    if (live(a)) l2 = dect2::hfbeaconSummary(a.rx.hfb);
}

} // namespace

extern const ModeUi kHfbeaconUi;
const ModeUi kHfbeaconUi = {
    .sideTitle = "HEARD NOW",
    .tabName = "Beacons",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .decoder = decoder,
    .tick = tick,
};

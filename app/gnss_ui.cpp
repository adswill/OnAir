// GNSS screens: the sky plot, the signal strength of every satellite, the position (card and map), the channel table, the search,
// the correlator plots and the options of the test signal.
#include "app.h"
#include "adsb_map.h"
#include <cmath>
#include <deque>

namespace {

struct FixPoint { double lat, lon, h; };

struct State {
    bool loaded = false;
    int viewMode = 0;                            // 0 sky, 1 map, 2 channels
    adsbmap::View map;
    bool mapFollow = true;                       // the map follows the fix until the user drags it
    std::deque<FixPoint> fixes;                  // the last fixes, one per report with a new fix (map trail and scatter)
    uint32_t lastFixCount = 0;
    uint64_t lastSeq = 0;
    double pushedCenter = 0, pushedMask = -1;
    double elMask = 5;
    std::deque<float> tracked, used;             // satellites per report, for the history plot
};
State S;

bool live(const App& a) { return a.engine.running() && a.rx.standard == 13; }

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    S.map.zoom = (int)d.getI("gnssZoom", 10);
    S.map.online = d.getB("adsbMap", true);     // the same switch as the ADS-B map: one choice about fetching tiles
    S.elMask = d.getD("gnssElMask", 5.0);
}

// colours of the systems, used for the dots, bars and names
ImU32 sysColour(int sys, float alpha = 1.f) {
    static const float c[4][3] = {{0.40f, 0.70f, 0.95f}, {0.92f, 0.45f, 0.40f}, {0.95f, 0.75f, 0.30f}, {0.55f, 0.85f, 0.50f}};
    const int s = sys >= 0 && sys < 4 ? sys : 0;
    return IM_COL32((int)(255 * c[s][0]), (int)(255 * c[s][1]), (int)(255 * c[s][2]), (int)(255 * alpha));
}

// C/N0 colour: weak signals red, usable ones amber, good ones green
ImU32 cn0Colour(float cn0, float alpha = 1.f) {
    const ImVec4 v = cn0 >= 40 ? pal::okGreen() : cn0 >= 33 ? pal::warnAmber() : pal::badRed();
    return ImGui::ColorConvertFloat4ToU32(ImVec4(v.x, v.y, v.z, alpha));
}

std::string satName(int sys, int prn, int fcn = 0) {
    char b[16];
    if (sys == GnssGlonass && prn == 0) snprintf(b, sizeof b, "R k%+d", fcn);
    else snprintf(b, sizeof b, "%c%02d", gnssSystemLetter(sys), prn);
    return b;
}

void tick(App& a) {
    loadState();
    if (a.engine.running()) {
        GnssReceiver& r = a.engine.gnss();
        if (S.pushedCenter != a.freqMhz) { r.setCenterMhz(a.freqMhz); S.pushedCenter = a.freqMhz; }
        if (S.pushedMask != S.elMask) { r.setElevationMask(S.elMask); S.pushedMask = S.elMask; }
    } else {
        S.pushedCenter = 0; S.pushedMask = -1;
    }
    if (live(a) && a.rx.seq != S.lastSeq) {
        S.lastSeq = a.rx.seq;
        const GnssTelemetry& t = a.rx.gnss;
        S.tracked.push_back((float)t.nTracked); if (S.tracked.size() > 240) S.tracked.pop_front();
        S.used.push_back((float)(t.fix.valid ? t.fix.nSats : 0)); if (S.used.size() > 240) S.used.pop_front();
        if (t.fix.valid && t.fix.fixCount != S.lastFixCount) {
            S.lastFixCount = t.fix.fixCount;
            S.fixes.push_back({t.fix.latDeg, t.fix.lonDeg, t.fix.heightM});
            if (S.fixes.size() > 600) S.fixes.pop_front();
            if (S.mapFollow) { S.map.lat = t.fix.latDeg; S.map.lon = t.fix.lonDeg; }
        }
    }
    if (!a.engine.running()) S.lastSeq = 0;
}

// ---------------------------------------------------------------- drawing pieces

// The sky seen from the antenna: the zenith in the middle, the horizon at the edge, north up, a ring every 30 degrees of elevation.
void skyPlot(const GnssTelemetry& t, bool on, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##sky", size);
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(8, 9, 10, 255));
    const float R = std::min(size.x, size.y) * 0.5f - 18 * gUi;
    const ImVec2 c(p0.x + size.x * 0.5f, p0.y + size.y * 0.5f);
    const ImU32 grid = IM_COL32(60, 64, 66, 255), label = IM_COL32(110, 114, 118, 255);
    for (int e = 0; e < 90; e += 30) {
        const float r = R * (90 - e) / 90.f;
        dl->AddCircle(c, r, grid, 72);
        char b[8]; snprintf(b, sizeof b, "%d", e);
        dl->AddText(ImVec2(c.x + 3 * gUi, c.y - r + 1), label, b);
    }
    for (int az = 0; az < 360; az += 30) {
        const float a = az * (float)M_PI / 180.f;
        dl->AddLine(c, ImVec2(c.x + std::sin(a) * R, c.y - std::cos(a) * R), IM_COL32(40, 43, 45, 255));
    }
    static const char* nesw[] = {"N", "E", "S", "W"};
    for (int i = 0; i < 4; i++) {
        const float a = i * (float)M_PI / 2;
        const ImVec2 ts = ImGui::CalcTextSize(nesw[i]);
        dl->AddText(ImVec2(c.x + std::sin(a) * (R + 9 * gUi) - ts.x * 0.5f, c.y - std::cos(a) * (R + 9 * gUi) - ts.y * 0.5f), IM_COL32(150, 154, 158, 255), nesw[i]);
    }
    if (!on) return;
    auto at = [&](float azDeg, float elDeg) {
        const float r = R * (90.f - std::max(0.f, std::min(90.f, elDeg))) / 90.f, a = azDeg * (float)M_PI / 180.f;
        return ImVec2(c.x + std::sin(a) * r, c.y - std::cos(a) * r);
    };
    const GnssSky* tip = nullptr;
    float tipD = 12 * gUi;
    for (const auto& s : t.sky) {
        const ImVec2 q = at(s.azDeg, s.elDeg);
        const float rad = (s.tracked ? 9.f : 7.f) * gUi;
        if (s.tracked) dl->AddCircleFilled(q, rad, s.used ? sysColour(s.sys, 0.95f) : sysColour(s.sys, 0.45f), 24);
        dl->AddCircle(q, rad, s.used ? IM_COL32(255, 255, 255, 220) : sysColour(s.sys, s.tracked ? 0.9f : 0.5f), 24, s.used ? 1.6f : 1.f);
        const std::string n = std::to_string(s.prn);
        const ImVec2 ts = ImGui::CalcTextSize(n.c_str());
        dl->AddText(ImVec2(q.x - ts.x * 0.5f, q.y - ts.y * 0.5f), s.tracked ? IM_COL32(10, 12, 14, 255) : IM_COL32(170, 174, 178, 255), n.c_str());
        const float d = std::hypot(q.x - mouse.x, q.y - mouse.y);
        if (hov && d < tipD) { tipD = d; tip = &s; }
    }
    if (tip) {
        ImGui::SetTooltip("%s  azimuth %.0f, elevation %.0f deg\n%s%s%s", satName(tip->sys, tip->prn, tip->fcn).c_str(), tip->azDeg, tip->elDeg,
                          tip->tracked ? "tracked" : "not tracked", tip->used ? ", in the fix" : "", tip->fromEphemeris ? "" : "  (position from the almanac)");
    }
    if (t.sky.empty()) dl->AddText(ImVec2(p0.x + 8 * gUi, p0.y + 6 * gUi), IM_COL32(150, 154, 158, 255), "no satellite positions yet: they come with the first ephemeris");
}

// One bar per tracked satellite: C/N0, coloured by strength, the name underneath, a frame when it is in the fix
void signalBars(const GnssTelemetry& t, bool on, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##bars", size);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(8, 9, 10, 255));
    const float lab = ImGui::GetTextLineHeight() + 4 * gUi, top = 6 * gUi, h = size.y - lab - top;
    const float lo = 20, hi = 55;
    for (int v = 25; v <= 50; v += 5) {
        const float y = p0.y + top + h * (1 - (v - lo) / (hi - lo));
        dl->AddLine(ImVec2(p0.x, y), ImVec2(p0.x + size.x, y), IM_COL32(34, 37, 39, 255));
        char b[8]; snprintf(b, sizeof b, "%d", v);
        dl->AddText(ImVec2(p0.x + 2 * gUi, y - ImGui::GetTextLineHeight() * 0.5f), IM_COL32(90, 94, 98, 255), b);
    }
    if (!on || t.channels.empty()) {
        dl->AddText(ImVec2(p0.x + 30 * gUi, p0.y + top), IM_COL32(150, 154, 158, 255), on ? "searching for satellites" : "C/N0 in dB-Hz of every tracked satellite");
        return;
    }
    std::vector<const GnssChannel*> ch;
    for (const auto& c : t.channels) ch.push_back(&c);
    std::sort(ch.begin(), ch.end(), [](const GnssChannel* x, const GnssChannel* y) { return x->sys != y->sys ? x->sys < y->sys : x->prn < y->prn; });   // a fixed order: the bars stay put
    const float x0 = p0.x + 22 * gUi, bw = std::min(34.f * gUi, (size.x - 26 * gUi) / (float)std::max<size_t>(ch.size(), 1));
    for (size_t i = 0; i < ch.size(); i++) {
        const GnssChannel& c = *ch[i];
        const float bx = x0 + bw * i, v = std::max(lo, std::min(hi, c.cn0));
        const float y = p0.y + top + h * (1 - (v - lo) / (hi - lo));
        const ImU32 col = c.state >= GnssChLocked ? cn0Colour(c.cn0) : IM_COL32(90, 94, 98, 255);
        dl->AddRectFilled(ImVec2(bx + 3, y), ImVec2(bx + bw - 3, p0.y + top + h), col, 2.f);
        if (c.used) dl->AddRect(ImVec2(bx + 2, y - 1), ImVec2(bx + bw - 2, p0.y + top + h + 1), IM_COL32(255, 255, 255, 230), 2.f, 0, 1.4f);
        char b[8]; snprintf(b, sizeof b, "%.0f", c.cn0);
        const ImVec2 ts = ImGui::CalcTextSize(b);
        if (bw > ts.x + 2) dl->AddText(ImVec2(bx + (bw - ts.x) * 0.5f, y - ts.y - 1), IM_COL32(200, 204, 208, 255), b);
        const std::string n = satName(c.sys, c.prn, c.fcn);
        const ImVec2 tn = ImGui::CalcTextSize(n.c_str());
        dl->AddText(ImVec2(bx + (bw - tn.x) * 0.5f, p0.y + top + h + 3 * gUi), sysColour(c.sys), n.c_str());
    }
}

std::string latText(double v) { char b[32]; snprintf(b, sizeof b, "%.6f %c", std::fabs(v), v >= 0 ? 'N' : 'S'); return b; }
std::string lonText(double v) { char b[32]; snprintf(b, sizeof b, "%.6f %c", std::fabs(v), v >= 0 ? 'E' : 'W'); return b; }

// The position and the time, a fixed set of rows that only changes its numbers
void fixCard(const App& a) {
    const GnssTelemetry& t = a.rx.gnss;
    const bool on = live(a);
    const GnssFix& f = t.fix;
    auto kv = [&](const char* k, const std::string& v) { ImGui::TextDisabled("%s", k); ImGui::SameLine(92 * gUi); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(v.c_str()); ImGui::PopFont(); };
    char b[96];
    ImGui::PushStyleColor(ImGuiCol_Text, pal::heading());
    ImGui::TextUnformatted(on && f.valid ? f.type.c_str() : on ? (t.nTracked > 0 ? "tracking, no fix yet" : "searching") : "stopped");
    ImGui::PopStyleColor();
    const bool v = on && f.valid;
    kv("latitude", v ? latText(f.latDeg) : "-");
    kv("longitude", v ? lonText(f.lonDeg) : "-");
    snprintf(b, sizeof b, "%.1f m", f.heightM); kv("height", v ? b : "-");
    snprintf(b, sizeof b, "%.1f m (1 sigma)", f.hErrM); kv("error", v ? b : "-");
    snprintf(b, sizeof b, "H %.1f  V %.1f  P %.1f", f.hdop, f.vdop, f.pdop); kv("DOP", v ? b : "-");
    snprintf(b, sizeof b, "%d of %d tracked", f.nSats, t.nTracked); kv("satellites", v ? b : on ? std::to_string(t.nTracked) + " tracked" : "-");
    if (on && f.timeValid) snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%04.1f", f.year, f.month, f.day, f.hour, f.minute, f.second); else snprintf(b, sizeof b, "-");
    kv("UTC", b);
    if (on && f.gpsWeek >= 0) snprintf(b, sizeof b, "week %d, %.0f s", f.gpsWeek, f.gpsTow); else snprintf(b, sizeof b, "-");
    kv("GPS time", b);
    snprintf(b, sizeof b, "%.1f km/h, %.0f deg", f.speedMps * 3.6, f.courseDeg); kv("speed", v && f.hasVelocity ? b : "-");
    snprintf(b, sizeof b, "%.0f m (%.0f ns)", f.clockBiasM[GnssGps], f.clockBiasM[GnssGps] / 0.299792458); kv("clock", v && f.systemInFix[GnssGps] ? b : "-");
    snprintf(b, sizeof b, "%.1f s", f.firstFixSecs); kv("first fix", on && f.firstFixSecs >= 0 ? b : "-");
    if (v && ImGui::SmallButton("copy position")) { snprintf(b, sizeof b, "%.6f, %.6f", f.latDeg, f.lonDeg); ImGui::SetClipboardText(b); }
}

void mapView(App& a, ImVec2 size) {
    const GnssTelemetry& t = a.rx.gnss;
    const bool on = live(a);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const double lat0 = S.map.lat, lon0 = S.map.lon;
    adsbmap::draw(S.map, size);
    if (S.map.lat != lat0 || S.map.lon != lon0) S.mapFollow = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    if (S.fixes.size() > 1) {   // the trail of fixes: on a fixed antenna it shows the scatter of the solution
        for (const auto& fp : S.fixes) dl->AddCircleFilled(adsbmap::project(fp.lat, fp.lon), 1.8f * gUi, IM_COL32(120, 200, 255, 110));
    }
    if (on && t.fix.valid) {
        const ImVec2 q = adsbmap::project(t.fix.latDeg, t.fix.lonDeg);
        // the estimated error as a circle: metres to pixels from the projection of a point that far north
        const double dLat = t.fix.hErrM / 111320.0;
        const float r = std::max(4.f * gUi, std::fabs(adsbmap::project(t.fix.latDeg + dLat, t.fix.lonDeg).y - q.y));
        dl->AddCircleFilled(q, r, IM_COL32(80, 160, 255, 40), 48);
        dl->AddCircle(q, r, IM_COL32(80, 160, 255, 160), 48, 1.2f);
        dl->AddCircleFilled(q, 5 * gUi, IM_COL32(255, 255, 255, 240));
        dl->AddCircle(q, 5 * gUi, IM_COL32(20, 90, 200, 255), 24, 2.f);
    }
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(26 * gUi, 0))) S.map.zoom = std::min(12, S.map.zoom + 1);
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(26 * gUi, 0))) S.map.zoom = std::max(2, S.map.zoom - 1);
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("follow")) { S.mapFollow = true; if (on && t.fix.valid) { S.map.lat = t.fix.latDeg; S.map.lon = t.fix.lonDeg; } }
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("clear trail")) S.fixes.clear();
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 10 * gUi);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.82f, 0.84f, 1));
    if (ImGui::Checkbox("online map", &S.map.online)) { plat::prefs().setB("adsbMap", S.map.online); savePrefs(a); }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fetch map tiles from the OpenStreetMap tile server (tile.openstreetmap.org) and keep them in the cache folder.\nOnly tile numbers are sent, never the position itself. Switch off to work offline.");
    plat::prefs().setI("gnssZoom", S.map.zoom);
    if (!on || !t.fix.valid) {
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
        ImGui::TextDisabled("%s", on ? "no fix yet" : "start the receiver");
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
}

void channelTable(const App& a, bool compact) {
    const GnssTelemetry& t = a.rx.gnss;
    const bool on = live(a);
    const int cols = compact ? 4 : 12;
    if (!ImGui::BeginTable(compact ? "##gch_s" : "##gch_f", cols, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Sat"); ImGui::TableSetupColumn("C/N0");
    if (!compact) { ImGui::TableSetupColumn("Doppler Hz"); ImGui::TableSetupColumn("Code chips"); ImGui::TableSetupColumn("Az"); ImGui::TableSetupColumn("El"); }
    ImGui::TableSetupColumn("State");
    if (!compact) { ImGui::TableSetupColumn("Lock s"); ImGui::TableSetupColumn("Frames"); ImGui::TableSetupColumn("Resid m"); ImGui::TableSetupColumn("Health"); }
    ImGui::TableSetupColumn("Fix");
    ImGui::TableHeadersRow();
    if (on) {
        std::vector<const GnssChannel*> ch;
        for (const auto& c : t.channels) ch.push_back(&c);
        std::sort(ch.begin(), ch.end(), [](const GnssChannel* x, const GnssChannel* y) { return x->sys != y->sys ? x->sys < y->sys : x->prn < y->prn; });
        for (const GnssChannel* pc : ch) {
            const GnssChannel& c = *pc;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(sysColour(c.sys))); ImGui::TextUnformatted(satName(c.sys, c.prn, c.fcn).c_str()); ImGui::PopStyleColor();
            ImGui::TableNextColumn(); ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(cn0Colour(c.cn0))); ImGui::Text("%.1f", c.cn0); ImGui::PopStyleColor();
            if (!compact) {
                ImGui::TableNextColumn(); ImGui::Text("%+.0f", c.dopplerHz);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", c.codePhase);
                ImGui::TableNextColumn(); if (c.hasAzEl) ImGui::Text("%.0f", c.azDeg); else ImGui::TextUnformatted("-");
                ImGui::TableNextColumn(); if (c.hasAzEl) ImGui::Text("%.0f", c.elDeg); else ImGui::TextUnformatted("-");
            }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(gnssChStateName(c.state));
            if (!compact) {
                ImGui::TableNextColumn(); ImGui::Text("%.0f", c.lockSecs);
                ImGui::TableNextColumn(); ImGui::Text("%u/%u", c.framesOk, c.framesOk + c.framesBad);
                ImGui::TableNextColumn(); if (c.used) ImGui::Text("%+.1f", c.residualM); else ImGui::TextUnformatted("-");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(c.health < 0 ? "-" : c.health == 0 ? "ok" : "bad");
            }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.used ? "yes" : "");
        }
    }
    ImGui::EndTable();
}

// correlation power against code phase of the last search: one sharp peak above a flat floor
void acqPlot(const GnssTelemetry& t, bool on, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##acq", size);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(8, 9, 10, 255));
    if (!on || t.acqCorr.size() < 2) return;
    const size_t n = t.acqCorr.size();
    std::vector<ImVec2> pts(n);
    for (size_t i = 0; i < n; i++) pts[i] = ImVec2(p0.x + size.x * (float)i / (float)(n - 1), p0.y + size.y - 4 - (size.y - 18 * gUi) * std::max(0.f, std::min(1.f, t.acqCorr[i])));
    dl->AddPolyline(pts.data(), (int)n, ImGui::ColorConvertFloat4ToU32(pal::accent()), 0, 1.2f);
    char b[96];
    snprintf(b, sizeof b, "%s  %+.0f Hz  peak %.1f x noise", satName(t.acqSys, t.acqPrn).c_str(), t.acqDopplerHz, t.acqPeakToNoise);
    dl->AddText(ImVec2(p0.x + 4 * gUi, p0.y + 2 * gUi), IM_COL32(170, 174, 178, 255), b);
}

void promptScatter(const GnssTelemetry& t, bool on, ImVec2 size) {
    std::vector<cf32> pts;
    if (on) for (size_t i = 0; i < t.scatterI.size() && i < t.scatterQ.size(); i++) pts.push_back(cf32(t.scatterI[i], t.scatterQ[i]));
    scatter("##gsc", pts, size, 1.25, pal::accent());
}

// the scatter of the fixes around their mean, in metres: what the position does while the antenna stands still
void fixScatter(ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##fsc", size);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(8, 9, 10, 255));
    const ImVec2 c(p0.x + size.x * 0.5f, p0.y + size.y * 0.5f);
    dl->AddLine(ImVec2(p0.x, c.y), ImVec2(p0.x + size.x, c.y), IM_COL32(40, 43, 45, 255));
    dl->AddLine(ImVec2(c.x, p0.y), ImVec2(c.x, p0.y + size.y), IM_COL32(40, 43, 45, 255));
    if (S.fixes.size() < 2) return;
    double mLat = 0, mLon = 0;
    for (const auto& f : S.fixes) { mLat += f.lat; mLon += f.lon; }
    mLat /= (double)S.fixes.size(); mLon /= (double)S.fixes.size();
    const double kx = 111320.0 * std::cos(mLat * M_PI / 180.0), ky = 110540.0;
    double mx = 5;
    for (const auto& f : S.fixes) mx = std::max(mx, std::max(std::fabs((f.lon - mLon) * kx), std::fabs((f.lat - mLat) * ky)));
    static const double steps[] = {5, 10, 20, 50, 100, 200, 500, 1000, 5000};
    double sc = 5000;
    for (double s : steps) if (s >= mx) { sc = s; break; }
    const float R = std::min(size.x, size.y) * 0.5f - 4;
    dl->AddCircle(c, R, IM_COL32(50, 53, 55, 255), 48);
    for (const auto& f : S.fixes) dl->AddCircleFilled(ImVec2(c.x + (float)((f.lon - mLon) * kx / sc) * R, c.y - (float)((f.lat - mLat) * ky / sc) * R), 1.8f * gUi, IM_COL32(120, 200, 255, 170));
    char b[48]; snprintf(b, sizeof b, "ring %.0f m, %zu fixes", sc, S.fixes.size());
    dl->AddText(ImVec2(p0.x + 4 * gUi, p0.y + 2 * gUi), IM_COL32(150, 154, 158, 255), b);
}

// ---------------------------------------------------------------- the hooks

void tab(App& a) {
    subNav("gnssv", S.viewMode, {"Sky", "Map", "Channels"});
    const GnssTelemetry& t = a.rx.gnss;
    const bool on = live(a);
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    if (S.viewMode == 2) { channelTable(a, false); return; }
    const float cw = std::min(300.f * gUi, W * 0.32f);
    ImGui::BeginChild("##gnss_l", ImVec2(W - cw - 8 * gUi, H));
    if (S.viewMode == 0) {
        const float avail = ImGui::GetContentRegionAvail().y, barsH = std::max(90.f * gUi, avail * 0.30f);
        skyPlot(t, on, ImVec2(ImGui::GetContentRegionAvail().x, avail - barsH - 6 * gUi));
        signalBars(t, on, ImVec2(ImGui::GetContentRegionAvail().x, barsH));
    } else {
        mapView(a, ImGui::GetContentRegionAvail());
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##gnss_r", ImVec2(0, H));
    fixCard(a);
    ImGui::EndChild();
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see satellites"); return; }
    const GnssTelemetry& t = a.rx.gnss;
    ImGui::TextDisabled("%d tracked, %d in the fix", t.nTracked, t.fix.valid ? t.fix.nSats : 0);
    channelTable(a, true);
}

void receiver(App& a) {
    const GnssTelemetry& t = a.rx.gnss;
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); ImGui::SameLine(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::Text(fmt, v...); ImGui::PopFont(); };
    std::string sys;
    for (int s = 0; s < GnssSystems; s++) if (t.activeMask & gnssSystemBit(s)) sys += std::string(sys.empty() ? "" : ", ") + gnssSystemName(s);
    kv("signals", "%s at %.3f MHz, %.3f Msps", sys.empty() ? "none in this band" : sys.c_str(), t.centerMhz, t.inputRate / 1e6);
    kv("search", "%s", t.searching ? (satName(t.searchSys, t.searchPrn) + ", " + std::to_string((int)(t.searchProgress * 100)) + " % of round " + std::to_string(t.searchRounds + 1)).c_str() : "idle");
    kv("navigation", "%llu frames good, %llu failed the parity", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kv("almanac", "%d GPS satellites", t.almanacGps);
    kv("ionosphere", "%s", t.ionoValid ? "model from the satellites" : "not yet");
    if (t.utcValid) kv("leap seconds", "%d", t.leapSeconds); else kv("leap seconds", "not yet");
    kv("front end", "%.1f dBFS, clipped %.2f %%, DC %+.3f %+.3f", t.levelDbfs, t.clipPercent, t.dcI, t.dcQ);
    kv("signal time", "%.1f s", t.signalSecs);
    ImGui::Spacing();
    ImGui::TextDisabled("Navigation data");
    if (ImGui::BeginTable("##gnav", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Sat"); ImGui::TableSetupColumn("Ephemeris"); ImGui::TableSetupColumn("IODE"); ImGui::TableSetupColumn("Age s");
        ImGui::TableSetupColumn("Week"); ImGui::TableSetupColumn("TOW s"); ImGui::TableSetupColumn("Clock us");
        ImGui::TableHeadersRow();
        for (const auto& n : t.nav) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(satName(n.sys, n.prn).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(n.hasEphemeris ? "yes" : "collecting");
            ImGui::TableNextColumn(); if (n.iode >= 0) ImGui::Text("%d", n.iode); else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); if (n.hasEphemeris) ImGui::Text("%.0f", n.ephAgeS); else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); if (n.week >= 0) ImGui::Text("%d", n.week); else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); if (n.towS >= 0) ImGui::Text("%d", n.towS); else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); if (n.hasEphemeris) ImGui::Text("%+.3f", n.svClockBiasUs); else ImGui::TextUnformatted("-");
        }
        ImGui::EndTable();
    }
}

void panels(App& a) {
    const GnssTelemetry& t = a.rx.gnss;
    const bool on = live(a);
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi, plotH = std::max(60.f, H - ImGui::GetTextLineHeightWithSpacing() - 6);
    const float sq = std::min(plotH, 170.f * gUi), rest = std::max(120.f, (W - 2 * sq - 4 * gap) / 2.f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap * 0.5f);
    ImGui::BeginGroup();
    if (on && !t.scatterI.empty()) ImGui::TextDisabled("Prompt I/Q (%s)", satName(t.scatterSys, t.scatterPrn).c_str()); else ImGui::TextDisabled("Prompt I/Q");
    promptScatter(t, on, ImVec2(sq, plotH));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    ImGui::TextDisabled("Search: correlation over one code period");
    acqPlot(t, on, ImVec2(rest, plotH));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    ImGui::TextDisabled("Satellites tracked (and in the fix)");
    {
        std::vector<float> v(S.tracked.begin(), S.tracked.end()), u(S.used.begin(), S.used.end());
        if (v.empty()) { v.push_back(0); u.push_back(0); }
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PlotLines("##gtr", v.data(), (int)v.size(), 0, nullptr, 0.f, 16.f, ImVec2(rest, plotH));
        ImGui::SetCursorScreenPos(p);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_PlotLines, pal::okGreen());
        ImGui::PlotLines("##gus", u.data(), (int)u.size(), 0, nullptr, 0.f, 16.f, ImVec2(rest, plotH));
        ImGui::PopStyleColor(2);
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    ImGui::TextDisabled("Position scatter");
    fixScatter(ImVec2(sq, plotH));
    ImGui::EndGroup();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const GnssTelemetry& t = a.rx.gnss;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight() * 2.f + ImGui::GetStyle().ItemSpacing.y * 2.f;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x - 4, p.y - 2), ImVec2(p.x + ImGui::GetContentRegionAvail().x + 4, p.y + h), IM_COL32(22, 23, 25, 255), 3.f);
    }
    int withEph = 0;
    if (on) for (const auto& n : t.nav) withEph += n.hasEphemeris;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); ImGui::SameLine(0, 12 * gUi);
    lamp("Tracking", !on ? 0 : t.nTracked >= 4 ? 1 : t.nTracked > 0 ? 2 : 0); ImGui::SameLine(0, 12 * gUi);
    lamp("Ephemeris", !on ? 0 : withEph >= 4 ? 1 : withEph > 0 ? 2 : 0); ImGui::SameLine(0, 12 * gUi);
    lamp("Fix", !on ? 0 : t.fix.valid ? 1 : 0); ImGui::SameLine(0, 10 * gUi);
    ImGui::TextDisabled("|"); ImGui::SameLine(0, 10 * gUi);
    auto ro = [&](const char* label, const std::string& val) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(val.c_str()); ImGui::PopFont();
        ImGui::SameLine(0, 15 * gUi);
    };
    char b[64];
    if (!on) { ro("State", run ? "starting" : "stopped"); return; }
    ro("State", t.fix.valid ? "Fix" : t.nTracked > 0 ? "Tracking" : "Searching");
    snprintf(b, sizeof b, "%d / %d", t.fix.valid ? t.fix.nSats : 0, t.nTracked); ro("Used / tracked", b);
    snprintf(b, sizeof b, "%.0f dB-Hz", t.snrDb); ro("Best C/N0", b);
    if (t.fix.valid) {
        snprintf(b, sizeof b, "%.1f", t.fix.hdop); ro("HDOP", b);
        snprintf(b, sizeof b, "%.5f %.5f", t.fix.latDeg, t.fix.lonDeg); ro("Position", b);
    }
}

void summary(const App& a, std::string& l1, std::string& l2) {
    const GnssTelemetry& t = a.rx.gnss;
    char b[120];
    l1 = "GNSS";
    if (!live(a)) return;
    if (t.fix.valid) snprintf(b, sizeof b, "%s  %.5f %.5f  HDOP %.1f", t.fix.type.c_str(), t.fix.latDeg, t.fix.lonDeg, t.fix.hdop);
    else snprintf(b, sizeof b, "%s  %d satellites  best %.0f dB-Hz", t.nTracked > 0 ? "tracking" : "searching", t.nTracked, t.snrDb);
    l2 = b;
}

void tuner(App& a, bool& retune) {
    loadState();
    ImGui::TextDisabled("Signal");
    ImGui::SetNextItemWidth(230 * gUi);
    const bool l1 = std::fabs(a.freqMhz - 1575.42) < 0.001;
    if (ImGui::BeginCombo("##gband", l1 ? "GPS L1 C/A  1575.42 MHz" : "custom frequency")) {
        if (ImGui::Selectable("GPS L1 C/A  1575.42 MHz", l1)) { a.freqMhz = 1575.42; retune = true; }
        ImGui::BeginDisabled();
        ImGui::Selectable("Galileo E1  1575.42 MHz", false);
        ImGui::Selectable("BeiDou B1I  1561.098 MHz", false);
        ImGui::Selectable("GLONASS L1  1602 MHz", false);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Only GPS L1 C/A is decoded so far. The receiver is built for the other systems, their decoders are not finished.");
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("GPS L1 C/A needs an active GPS antenna: switch on the antenna power of the radio (HackRF: the antenna port power)\nand give the antenna a view of the sky. Indoors there is usually nothing to receive.");
}

void decoder(App& a, bool&) {
    loadState();
    ImGui::TextDisabled("MASK"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(90 * gUi);
    float m = (float)S.elMask;
    if (ImGui::SliderFloat("##gmask", &m, 0.f, 30.f, "%.0f deg")) { S.elMask = m; plat::prefs().setD("gnssElMask", S.elMask); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Satellites lower than this are tracked but left out of the position: low signals cross more air and reflect more.");
    (void)a;
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    ImGui::TextDisabled("simulated sky (GPS)");
    ImGui::SameLine(); ImGui::SetNextItemWidth(100 * gUi);
    float cn0 = sc.modeVal[3] > 0 ? (float)sc.modeVal[3] : 44.f;
    if (ImGui::SliderFloat("##gcn0", &cn0, 30.f, 50.f, "%.0f dB-Hz")) { sc.modeVal[3] = cn0; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("C/N0 of a satellite overhead. Lower satellites are weaker (about 9 dB less near the horizon).\nAbout 44 dB-Hz is a good outdoor antenna, below about 30 the navigation data no longer decodes.");
    ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    int n = sc.modeOpt[1] > 0 ? sc.modeOpt[1] : 0;
    if (ImGui::SliderInt("##gns", &n, 0, 12, n == 0 ? "all in view" : "%d satellites")) { sc.modeOpt[1] = n; changed = true; }
    ImGui::SameLine();
    bool cold = sc.modeOpt[2] == 1;
    if (ImGui::Checkbox("cold start", &cold)) { sc.modeOpt[2] = cold ? 1 : 0; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Warm: the signal starts at a convenient place in the navigation message and the first fix comes after about 26 s.\nCold: like switching on at a random moment, about 35 s.");
    ImGui::SameLine();
    bool jam = (sc.modeOpt[3] & 1) != 0;
    if (ImGui::Checkbox("jammer", &jam)) { sc.modeOpt[3] = (sc.modeOpt[3] & ~1) | (jam ? 1 : 0); changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A continuous tone 400 kHz from the centre, much stronger than the satellites. The receiver notches it out.");
    ImGui::SameLine(); ImGui::TextDisabled("CFO"); ImGui::SameLine(); ImGui::SetNextItemWidth(100 * gUi);
    float cfo = (float)(sc.cfoHz / 1e3);
    if (ImGui::SliderFloat("##gcfo", &cfo, -5, 5, "%.2f kHz")) { sc.cfoHz = cfo * 1e3; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The radio's frequency error at 1575 MHz (a 1 ppm TCXO is off by about 1.6 kHz).");
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const GnssTelemetry& t = a.rx.gnss;
    out.push_back({"TRACKED", "%.0f", (double)t.nTracked, 0, 14, t.nTracked >= 4 ? 1 : 2});
    out.push_back({"IN FIX", "%.0f", (double)(t.fix.valid ? t.fix.nSats : 0), 0, 14, t.fix.valid ? 1 : 0});
    out.push_back({"BEST C/N0  dB-Hz", "%.0f", t.snrDb, 20, 55, t.snrDb >= 40 ? 1 : t.snrDb >= 33 ? 2 : 3});
    out.push_back({"HDOP", "%.1f", t.fix.valid ? t.fix.hdop : 0.0, 0, 6, !t.fix.valid ? 0 : t.fix.hdop < 2 ? 1 : 2});
}

} // namespace

extern const ModeUi kGnssUi;
const ModeUi kGnssUi = {
    .sideTitle = "SATELLITES",
    .tabName = "Sky",
    .tabIcon = Ic::Globe,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .panels = panels,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .decoder = decoder,
    .synth = synth,
    .tick = tick,
    .meters = meters,
};

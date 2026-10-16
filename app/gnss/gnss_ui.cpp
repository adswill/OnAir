// GNSS screens: the sky plot, the signal strength of every satellite, the position (card and map), the channel table, the search,
// the correlator plots and the options of the test signal.
#include "app.h"
#include "clocksync_ui.h"
#include "../adsb/adsb_map.h"
#include "dect2/gnss_interf.h"
#include "dect2/rate_choice.h"
#include "dect2/u8file.h"
#include <cctype>
#include <cmath>
#include <ctime>
#include <deque>

namespace {

struct FixPoint { double lat, lon, h; };

struct State {
    bool loaded = false;
    int viewMode = 0;                            // 0 sky, 1 map, 2 channels, 3 interference, 4 clock
    std::string interfNote;                      // the result of the last CSV export
    adsbmap::View map;
    bool mapFollow = true;                       // the map follows the fix until the user drags it
    std::deque<FixPoint> fixes;                  // the last fixes, one per report with a new fix (map trail and scatter)
    uint32_t lastFixCount = 0;
    uint64_t lastSeq = 0;
    double pushedCenter = 0, pushedMask = -1;
    double elMask = 5;
    std::deque<float> tracked, used;             // satellites per report, for the history plot
    bool pushedHint = false;                     // the remembered frequency error of this radio has been given to the receiver
    double savedCfo = 0, savedAt = -1e9;         // the last value written to the settings, and the signal time of that
};
State S;

bool live(const App& a) { return a.rx.standard == 13; }

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    S.map.zoom = (int)d.getI("gnssZoom", 10);
    S.elMask = d.getD("gnssElMask", 5.0);
}

// The settings key of the frequency error remembered for the radio in use ("" for the test signal and files: nothing to remember)
std::string cfoKey(const App& a) {
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size()) return "";
    const DeviceInfo& d = a.devices[(size_t)a.devIdx];
    if (!d.isRadio()) return "";
    std::string k = "gnssCfo_";
    for (char c : d.board + "_" + d.serial + "_" + d.name) k += std::isalnum((unsigned char)c) ? c : '_';
    return k.substr(0, 96);
}

// colours of the systems, used for the dots, bars and names
ImU32 sysColour(int sys, float alpha = 1.f) {
    // GPS blue, GLONASS red, BeiDou amber, Galileo green, QZSS violet, SBAS grey
    static const float c[GnssSystems][3] = {{0.40f, 0.70f, 0.95f}, {0.92f, 0.45f, 0.40f}, {0.95f, 0.75f, 0.30f}, {0.55f, 0.85f, 0.50f}, {0.78f, 0.55f, 0.95f}, {0.70f, 0.72f, 0.74f}};
    const int s = sys >= 0 && sys < GnssSystems ? sys : 0;
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
    else return gnssSatName(sys, prn);     // G05, E11, J01 (QZSS PRN 193), S27 (SBAS PRN 127), C19, R05
    return b;
}
// with the GLONASS frequency channel: "R05 k-3"
std::string satNameK(int sys, int prn, int fcn) {
    if (sys != GnssGlonass || prn == 0) return satName(sys, prn, fcn);
    char b[20];
    snprintf(b, sizeof b, "R%02d k%+d", prn, fcn);
    return b;
}

void tick(App& a) {
    loadState();
    clockPublishGps(a);
    a.tune.synth.modeOpt[5] = (int)std::lround(a.freqMhz * 1000.0);    // the test signal renders the sky as the tune sees it
    if (a.engine.running()) {
        GnssReceiver& r = a.engine.gnss();
        if (S.pushedCenter != a.freqMhz) { r.setCenterMhz(a.freqMhz); S.pushedCenter = a.freqMhz; }
        if (S.pushedMask != S.elMask) { r.setElevationMask(S.elMask); S.pushedMask = S.elMask; }
        if (!S.pushedHint) {
            // a radio's frequency error changes little from one run to the next: the search starts where it was
            S.pushedHint = true;
            const std::string k = cfoKey(a);
            if (!k.empty() && plat::prefs().has(k.c_str())) r.setFrequencyHint(plat::prefs().getD(k.c_str(), 0.0), true);
            else r.setFrequencyHint(0, false);
        }
    } else {
        S.pushedCenter = 0; S.pushedMask = -1; S.pushedHint = false; S.savedAt = -1e9;
    }
    if (live(a) && a.rx.seq != S.lastSeq) {
        S.lastSeq = a.rx.seq;
        const GnssTelemetry& t = a.rx.gnss;
        S.tracked.push_back((float)t.nTracked); if (S.tracked.size() > 240) S.tracked.pop_front();
        S.used.push_back((float)(t.fix.valid ? t.fix.nSats : 0)); if (S.used.size() > 240) S.used.pop_front();
        // remember the frequency error measured with a fix, now and then (it drifts while the radio warms up)
        if (t.fix.valid && t.fix.nSats >= 4 && (t.signalSecs - S.savedAt > 60 || std::fabs(t.cfoHz - S.savedCfo) > 500)) {
            const std::string k = cfoKey(a);
            if (!k.empty()) { plat::prefs().setD(k.c_str(), t.cfoHz); savePrefs(a); }
            S.savedCfo = t.cfoHz; S.savedAt = t.signalSecs;
        }
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

std::vector<std::string> progressLines(const GnssTelemetry& t);

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
        const std::string n = satName(s.sys, s.prn, s.fcn);      // the letter says the constellation, the colour too
        const ImVec2 ts = ImGui::CalcTextSize(n.c_str());
        dl->AddText(ImVec2(q.x - ts.x * 0.5f, q.y - ts.y * 0.5f), s.tracked ? IM_COL32(10, 12, 14, 255) : IM_COL32(170, 174, 178, 255), n.c_str());
        const float d = std::hypot(q.x - mouse.x, q.y - mouse.y);
        if (hov && d < tipD) { tipD = d; tip = &s; }
    }
    {
        // a legend of the systems in the plot, top right
        unsigned seen = 0;
        for (const auto& s : t.sky) seen |= gnssSystemBit(s.sys);
        float ly = p0.y + 4 * gUi;
        for (int sy = 0; sy < GnssSystems; sy++) {
            if (!(seen & gnssSystemBit(sy))) continue;
            const char* nm = gnssSystemName(sy);
            const ImVec2 ts = ImGui::CalcTextSize(nm);
            const float lx = p0.x + size.x - ts.x - 16 * gUi;
            dl->AddCircleFilled(ImVec2(lx - 2 * gUi, ly + ts.y * 0.5f), 4 * gUi, sysColour(sy, 0.95f), 12);
            dl->AddText(ImVec2(lx + 6 * gUi, ly), IM_COL32(170, 174, 178, 255), nm);
            ly += ts.y + 2 * gUi;
        }
    }
    if (tip) {
        ImGui::SetTooltip("%s (%s)  azimuth %.0f, elevation %.0f deg\n%s%s%s", satNameK(tip->sys, tip->prn, tip->fcn).c_str(), gnssSystemName(tip->sys), tip->azDeg, tip->elDeg,
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
        const std::vector<std::string> pl = on ? progressLines(t) : std::vector<std::string>();
        dl->AddText(ImVec2(p0.x + 30 * gUi, p0.y + top), IM_COL32(150, 154, 158, 255), on ? (pl.empty() ? "searching for satellites" : pl[0].c_str()) : "C/N0 in dB-Hz of every tracked satellite");
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

// What the receiver is doing now and what comes next, in a few plain lines: a cold start takes a while (finding the satellites, then about 30 s of
// navigation data from each of four), and without this the screen looks stuck.
std::vector<std::string> progressLines(const GnssTelemetry& t) {
    std::vector<std::string> out;
    char b[200];
    if (t.activeMask == 0) {
        snprintf(b, sizeof b, "No signal band fits at %.3f MHz and %.2f Msps: tune to 1575.42 MHz (L1 / E1, 2.05 Msps or more), 1561.098 MHz (BeiDou B1I, 4 Msps) or 1602 MHz (GLONASS, 10 Msps)", t.centerMhz, t.inputRate / 1e6);
        out.push_back(b);
        return out;
    }
    if (t.nTracked == 0) {
        snprintf(b, sizeof b, "Searching %s %02d (%d %% of round %u): %+.0f to %+.0f kHz, %d ms", gnssSystemName(t.searchSys), t.searchPrn, (int)(t.searchProgress * 100), t.searchRounds + 1,
                 (t.searchCenterHz - t.searchHalfHz) / 1e3, (t.searchCenterHz + t.searchHalfHz) / 1e3, t.searchMs);
        out.push_back(b);
        if (t.nPullIn > 0) { snprintf(b, sizeof b, "%d satellite%s found, locking on", t.nPullIn, t.nPullIn == 1 ? "" : "s"); out.push_back(b); }
        else if (t.searchStage == 1 || t.searchStage == 2) out.push_back("Nothing in the first window: searching wider for the radio's frequency error (a HackRF or a dongle without a TCXO)");
        else if (t.searchStage >= 3) out.push_back("Searching longer for weak signals");
        if (t.signalSecs > 60 && t.nPullIn == 0)
            out.push_back("Nothing after a minute: the antenna needs power (bias-tee or a powered LNA) and a clear view of the sky");
    } else {
        int withEph = 0, framed = 0;
        float soonest = -1;
        int soonPrn = 0, soonParts = 0;
        for (const auto& n : t.nav) {
            if (n.hasEphemeris) { withEph++; continue; }
            if (n.ephEtaS >= 0) { framed++; if (soonest < 0 || n.ephEtaS < soonest) { soonest = n.ephEtaS; soonPrn = n.prn; soonParts = n.ephParts; } }
        }
        snprintf(b, sizeof b, "%d locked%s, %d with ephemeris (4 needed for a fix)", t.nTracked, t.nPullIn > 0 ? (", " + std::to_string(t.nPullIn) + " locking on").c_str() : "", withEph);
        out.push_back(b);
        if (!t.fix.valid) {
            if (soonest >= 0) { snprintf(b, sizeof b, "Collecting ephemeris: G%02d %d/3 subframes, about %.0f s left (each satellite sends it every 30 s)", soonPrn, soonParts, soonest); out.push_back(b); }
            else if (t.decodeMask == gnssSystemBit(GnssBeidou) && withEph < 4) out.push_back("Collecting ephemeris: BeiDou sends it every 30 s (subframes 1-3, or ten pages from a GEO satellite)");
            else if (t.decodeMask == gnssSystemBit(GnssGlonass) && withEph < 4) out.push_back("Collecting ephemeris: GLONASS sends it in strings 1-4 at the start of every 30 s frame");
            else if (framed == 0 && withEph < 4) out.push_back("Waiting for the start of the navigation message (up to 6 s after the bit sync)");
            if (withEph >= 4) out.push_back("Ephemeris complete: the fix comes with the next measurement");
        }
    }
    if (t.levelDbfs < -45 && t.levelDbfs > -98) out.push_back("Very low input level: raise the gain (the noise should fill a few steps of the converter)");
    else if (t.clipPercent > 1.0) out.push_back("The input clips: lower the gain");
    return out;
}

std::string latText(double v) { char b[32]; snprintf(b, sizeof b, "%.6f %c", std::fabs(v), v >= 0 ? 'N' : 'S'); return b; }
std::string lonText(double v) { char b[32]; snprintf(b, sizeof b, "%.6f %c", std::fabs(v), v >= 0 ? 'E' : 'W'); return b; }

// The position and the time, a fixed set of rows that only changes its numbers
void fixCard(const App& a) {
    const GnssTelemetry& t = a.rx.gnss;
    const bool on = live(a);
    const GnssFix& f = t.fix;
    auto kv = [&](const char* k, const std::string& v) { ImGui::TextDisabled("%s", k); kvColumn(92 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(v.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    char b[96];
    ImGui::PushStyleColor(ImGuiCol_Text, pal::heading());
    { ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(on && f.valid ? f.type.c_str() : on ? (t.nTracked > 0 ? "tracking, no fix yet" : "searching") : "stopped"); ImGui::PopTextWrapPos(); }
    ImGui::PopStyleColor();
    if (on && !f.valid) {
        ImGui::PushTextWrapPos(0.f);
        for (const auto& l : progressLines(t)) ImGui::TextDisabled("%s", l.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
    }
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
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const GnssSatPos* tip = nullptr;
    float tipD = 14 * gUi;
    if (on) {
        for (const auto& s : t.sats) {
            const ImVec2 q = adsbmap::project(s.lat, s.lon);
            const float r = (s.used ? 7.f : 5.5f) * gUi;
            const ImVec2 d[4] = {{q.x, q.y - r}, {q.x + r, q.y}, {q.x, q.y + r}, {q.x - r, q.y}};
            dl->AddConvexPolyFilled(d, 4, sysColour(s.sys, s.tracked ? 0.95f : 0.45f));
            dl->AddPolyline(d, 4, s.used ? IM_COL32(255, 255, 255, 230) : IM_COL32(0, 0, 0, 150), ImDrawFlags_Closed, s.used ? 1.5f : 1.f);
            const std::string n = satName(s.sys, s.prn, s.fcn);
            const ImVec2 ts = ImGui::CalcTextSize(n.c_str());
            const ImVec2 lp(q.x + 8 * gUi, q.y - ts.y * 0.5f);
            dl->AddRectFilled(ImVec2(lp.x - 3 * gUi, lp.y - 1), ImVec2(lp.x + ts.x + 3 * gUi, lp.y + ts.y + 1), IM_COL32(10, 12, 14, 190), 2.f);
            dl->AddText(lp, IM_COL32(235, 238, 240, 255), n.c_str());
            const float dd = std::hypot(q.x - mouse.x, q.y - mouse.y);
            if (dd < tipD) { tipD = dd; tip = &s; }
        }
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
    if (tip) {
        ImGui::SetTooltip("%s (%s)  %.2f, %.2f  %.0f km\n%s%s%s", satNameK(tip->sys, tip->prn, tip->fcn).c_str(), gnssSystemName(tip->sys),
                          tip->lat, tip->lon, tip->altKm, tip->tracked ? "tracked" : "not tracked", tip->used ? ", in the fix" : "",
                          tip->fromEphemeris ? "" : "  (almanac)");
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(26 * gUi, 0))) S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1);
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(26 * gUi, 0))) S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1);
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("follow")) { S.mapFollow = true; if (on && t.fix.valid) { S.map.lat = t.fix.latDeg; S.map.lon = t.fix.lonDeg; } }
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("fit")) {
        S.mapFollow = false;
        double la0 = 90, la1 = -90, lo0 = 180, lo1 = -180;
        int n = 0;
        auto box = [&](double la, double lo) { la0 = std::min(la0, la); la1 = std::max(la1, la); lo0 = std::min(lo0, lo); lo1 = std::max(lo1, lo); n++; };
        if (on && t.fix.valid) box(t.fix.latDeg, t.fix.lonDeg);
        if (on) for (const auto& s : t.sats) box(s.lat, s.lon);
        if (n) {
            S.map.lat = (la0 + la1) / 2; S.map.lon = (lo0 + lo1) / 2;
            const double span = std::max(12.0, std::max(lo1 - lo0, (la1 - la0) * 1.4));
            S.map.zoom = std::max(2, std::min(6, (int)std::floor(std::log2(std::max(2.0, 0.7 * size.x * 360.0 / (256.0 * span))))));
        }
    }
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("clear trail")) S.fixes.clear();
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never the position itself");
    plat::prefs().setI("gnssZoom", S.map.zoom);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
    if (!on) adsbmap::legend(size.x, "start the receiver");
    else if (!t.sats.empty()) adsbmap::legend(size.x, "%zu satellite sub-points from the navigation message; white dot: the fix. Fit to see them.", t.sats.size());
    else adsbmap::legend(size.x, "%s", t.fix.valid ? "fix, no satellite orbits yet" : "no fix yet");
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
}

void channelTable(const App& a, bool compact) {
    const GnssTelemetry& t = a.rx.gnss;
    const bool on = live(a);
    const int cols = compact ? 4 : 13;
    if (!ImGui::BeginTable(compact ? "##gch_s" : "##gch_f", cols, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Sat");
    if (!compact) ImGui::TableSetupColumn("System");
    ImGui::TableSetupColumn("C/N0");
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
            ImGui::TableNextColumn(); ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(sysColour(c.sys))); ImGui::TextUnformatted(satNameK(c.sys, c.prn, c.fcn).c_str()); ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                if (c.sys == GnssGlonass) ImGui::SetTooltip("GLONASS slot %d, frequency channel k = %+d (%.4f MHz)", c.prn, c.fcn, 1602.0 + 0.5625 * c.fcn);
                else ImGui::SetTooltip("%s PRN %d", gnssSystemName(c.sys), c.prn);
            }
            if (!compact) { ImGui::TableNextColumn(); ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(sysColour(c.sys))); ImGui::TextUnformatted(gnssSystemName(c.sys)); ImGui::PopStyleColor(); }
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
    dl->AddText(ImVec2(p0.x + 4 * gUi, p0.y + 2 * gUi), IM_COL32(170, 174, 178, 255), ellipsize(b, size.x - 8 * gUi).c_str());   // inside the plot
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

// ---------------------------------------------------------------- interference and spoofing monitor

ImVec4 interfColour(int status) { return status == GnssInterfAlarm ? pal::badRed() : status == GnssInterfRaised ? pal::warnAmber() : pal::okGreen(); }

// the noise floor and the total power (left scale, dB) and the mean C/N0 of the tracked satellites (right scale, dB-Hz) over the run,
// with a strip of the status underneath and a red tick where a spoofing indicator was up
void interfHistory(const GnssInterference& r, bool on, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##ihist", size);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(8, 9, 10, 255));
    const auto& h = r.history;
    const float lh = ImGui::GetTextLineHeight(), stripH = 5 * gUi;
    const float l = p0.x + 4 * gUi, rgt = p0.x + size.x - 4 * gUi, top = p0.y + lh + 4 * gUi, bot = p0.y + size.y - stripH - lh - 4 * gUi;
    if (!on || h.size() < 2 || bot - top < 10) {
        dl->AddText(ImVec2(p0.x + 6 * gUi, p0.y + 4 * gUi), IM_COL32(120, 124, 128, 255), on ? "collecting..." : "start the receiver");
        return;
    }
    // the floor in dBFS/MHz (dBFS/Hz + 60) shares the scale with the total power
    float lo = 1e9f, hi = -1e9f;
    for (const auto& p : h) { lo = std::min(lo, std::min(p.noiseDb + 60.f, p.powerDb)); hi = std::max(hi, std::max(p.noiseDb + 60.f, p.powerDb)); }
    lo = std::floor(lo - 2); hi = std::ceil(hi + 2);
    const double t0 = h.front().t, t1 = std::max(h.back().t, t0 + 1);
    auto X = [&](double t) { return l + (float)((t - t0) / (t1 - t0)) * (rgt - l); };
    auto Y = [&](float v) { return bot - (v - lo) / (hi - lo) * (bot - top); };
    auto Yc = [&](float c) { return bot - std::max(0.f, std::min(1.f, (c - 20.f) / 35.f)) * (bot - top); };
    for (int g = 1; g < 4; g++) { const float y = top + (bot - top) * g / 4.f; dl->AddLine(ImVec2(l, y), ImVec2(rgt, y), IM_COL32(32, 35, 37, 255)); }
    const ImU32 green = ImGui::ColorConvertFloat4ToU32(pal::okGreen());
    std::vector<ImVec2> nf, pw, cn;
    for (const auto& p : h) {
        nf.push_back(ImVec2(X(p.t), Y(p.noiseDb + 60.f)));
        pw.push_back(ImVec2(X(p.t), Y(p.powerDb)));
        if (p.cn0 > 0) cn.push_back(ImVec2(X(p.t), Yc(p.cn0)));
        else { if (cn.size() > 1) dl->AddPolyline(cn.data(), (int)cn.size(), green, 0, 1.4f); cn.clear(); }
    }
    dl->AddPolyline(pw.data(), (int)pw.size(), IM_COL32(150, 154, 158, 255), 0, 1.2f);
    dl->AddPolyline(nf.data(), (int)nf.size(), ImGui::ColorConvertFloat4ToU32(pal::accent()), 0, 1.6f);
    if (cn.size() > 1) dl->AddPolyline(cn.data(), (int)cn.size(), green, 0, 1.4f);
    // status strip and spoofing ticks
    for (size_t i = 0; i < h.size(); i++) {
        const float xa = i == 0 ? l : X(h[i - 1].t), xb = X(h[i].t);
        const ImVec4 c = interfColour(h[i].status);
        dl->AddRectFilled(ImVec2(xa, bot + 2 * gUi), ImVec2(std::max(xb, xa + 1), bot + 2 * gUi + stripH), ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, 0.85f)));
        if (h[i].spoof) dl->AddLine(ImVec2(xb, top), ImVec2(xb, top + 6 * gUi), ImGui::ColorConvertFloat4ToU32(pal::badRed()), 2.f);
    }
    char b[160];
    snprintf(b, sizeof b, "floor dBFS/MHz (blue), power dBFS (grey): %.0f..%.0f dB;  C/N0 (green): 20..55 dB-Hz", lo, hi);
    dl->AddText(ImVec2(p0.x + 4 * gUi, p0.y + 2 * gUi), IM_COL32(150, 154, 158, 255), ellipsize(b, size.x - 8 * gUi).c_str());
    const double span = t1 - t0;
    if (span >= 120) snprintf(b, sizeof b, "last %.0f min, a point every %.0f s", span / 60, r.historyStepS);
    else snprintf(b, sizeof b, "last %.0f s", span);
    dl->AddText(ImVec2(p0.x + 4 * gUi, bot + stripH + 3 * gUi), IM_COL32(120, 124, 128, 255), b);
}

// the band's averaged spectrum, the noise floor and its baseline, the narrowband lines marked
void interfSpectrum(const GnssInterference& r, bool on, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##ispec", size);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(8, 9, 10, 255));
    const auto& s = r.spectrum;
    const float lh = ImGui::GetTextLineHeight();
    const float l = p0.x + 4 * gUi, rgt = p0.x + size.x - 4 * gUi, top = p0.y + 2 * lh + 4 * gUi, bot = p0.y + size.y - lh - 4 * gUi;
    if (!on || s.size() < 8 || bot - top < 10) return;
    float hi = r.noiseDb + 10;
    for (float v : s) hi = std::max(hi, v);
    const float lo = r.noiseDb - 8;
    hi += 3;
    const size_t n = s.size();
    auto X = [&](double i) { return l + (float)(i / (double)(n - 1)) * (rgt - l); };
    auto Y = [&](float v) { return bot - std::max(0.f, std::min(1.f, (v - lo) / (hi - lo))) * (bot - top); };
    std::vector<ImVec2> pts(n);
    for (size_t i = 0; i < n; i++) pts[i] = ImVec2(X((double)i), Y(s[i]));
    dl->AddPolyline(pts.data(), (int)n, ImGui::ColorConvertFloat4ToU32(pal::accent()), 0, 1.0f);
    dl->AddLine(ImVec2(l, Y(r.noiseDb)), ImVec2(rgt, Y(r.noiseDb)), IM_COL32(220, 220, 220, 120));
    const ImVec4 g = pal::okGreen();
    if (!r.learning) dl->AddLine(ImVec2(l, Y(r.baselineDb)), ImVec2(rgt, Y(r.baselineDb)), ImGui::ColorConvertFloat4ToU32(ImVec4(g.x, g.y, g.z, 0.5f)));
    const double binHz = r.spanHz / (double)n;
    float lastLabel = -1e9f;
    int labelled = 0;
    for (const auto& sp : r.spikes) {
        const float x = X(sp.offsetHz / binHz + (double)n / 2), y = Y(r.noiseDb + sp.aboveDb);
        const ImU32 c = ImGui::ColorConvertFloat4ToU32(sp.known ? ImVec4(0.6f, 0.6f, 0.6f, 1) : sp.aboveDb >= 10 ? pal::badRed() : pal::warnAmber());
        dl->AddTriangleFilled(ImVec2(x, y - 2), ImVec2(x - 4 * gUi, y - 9 * gUi), ImVec2(x + 4 * gUi, y - 9 * gUi), c);
        if (std::fabs(x - lastLabel) > 90 * gUi && labelled < 6) {
            char b[48]; snprintf(b, sizeof b, "%+.1f kHz %.0f dB", sp.offsetHz / 1e3, sp.aboveDb);
            const float tw = ImGui::CalcTextSize(b).x;
            dl->AddText(ImVec2(std::max(l, std::min(x - tw / 2, rgt - tw)), std::max(p0.y + lh + 2 * gUi, y - 9 * gUi - lh)), c, b);
            lastLabel = x; labelled++;
        }
    }
    char b[160];
    snprintf(b, sizeof b, "%.1f MHz span, floor %.1f dBFS/Hz (grey), baseline (green), lines 8 dB above the floor marked", r.spanHz / 1e6, r.noiseDb);
    dl->AddText(ImVec2(p0.x + 4 * gUi, p0.y + 2 * gUi), IM_COL32(150, 154, 158, 255), ellipsize(b, size.x - 8 * gUi).c_str());
    snprintf(b, sizeof b, "%+.1f MHz", -r.spanHz / 2e6);
    dl->AddText(ImVec2(l, bot + 2 * gUi), IM_COL32(120, 124, 128, 255), b);
    snprintf(b, sizeof b, "%+.1f MHz", r.spanHz / 2e6);
    dl->AddText(ImVec2(rgt - ImGui::CalcTextSize(b).x, bot + 2 * gUi), IM_COL32(120, 124, 128, 255), b);
    const char* c0 = "centre";
    dl->AddText(ImVec2((l + rgt) / 2 - ImGui::CalcTextSize(c0).x / 2, bot + 2 * gUi), IM_COL32(120, 124, 128, 255), c0);
}

void interfExport(const GnssInterference& r) {
    char nm[64];
    const std::time_t now = std::time(nullptr);
    std::strftime(nm, sizeof nm, "gnss_interference_%Y%m%d_%H%M%S.csv", std::localtime(&now));
    const std::string path = saveFileDialog(nm);
    if (path.empty()) return;
    const std::string csv = gnssInterfCsv(r);
    FILE* f = dect2::u8fopen(path, "wb");
    S.interfNote = f && fwrite(csv.data(), 1, csv.size(), f) == csv.size() ? "saved " + path : "could not write " + path;
    if (f) fclose(f);
}

void interfView(App& a) {
    const GnssInterference& r = a.rx.gnss.interf;
    const bool on = live(a) && r.valid;
    // status line
    ImGui::AlignTextToFramePadding();
    if (!on) ImGui::TextDisabled("%s", live(a) ? "measuring the band..." : "start the receiver to monitor the band");
    else if (r.learning) ImGui::TextColored(pal::accent(), "learning the band (the first %.0f s)", GnssInterfMonitor::kLearnSecs);
    else {
        ImGui::TextColored(interfColour(r.status), "%s", r.status == GnssInterfClear ? "CLEAR" : r.status == GnssInterfRaised ? "RAISED NOISE" : "INTERFERENCE");
        if (!r.reason.empty()) { ImGui::SameLine(); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(r.reason.c_str()); ImGui::PopTextWrapPos(); }
    }
    if (on && r.spoofing) { ImGui::TextColored(pal::badRed(), "SPOOFING INDICATOR"); ImGui::SameLine(); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(r.spoofReason.c_str()); ImGui::PopTextWrapPos(); }
    ImGui::BeginDisabled(!on || r.history.empty());
    if (ImGui::SmallButton("Export CSV")) interfExport(r);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("The history (noise floor, power, C/N0, status) and the event log as a CSV file");
    if (!S.interfNote.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", ellipsize(S.interfNote, ImGui::GetContentRegionAvail().x).c_str()); }
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float cw = std::min(360.f * gUi, W * 0.38f);
    ImGui::BeginChild("##ginterf_l", ImVec2(W - cw - 8 * gUi, H));
    {
        const float avail = ImGui::GetContentRegionAvail().y, w = ImGui::GetContentRegionAvail().x;
        interfHistory(r, on, ImVec2(w, std::max(60.f, avail * 0.5f - 3 * gUi)));
        interfSpectrum(r, on, ImVec2(w, std::max(60.f, ImGui::GetContentRegionAvail().y)));
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##ginterf_r", ImVec2(0, H));
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(110 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    auto flag = [&](const char* k, bool up, const char* detail) {
        ImGui::TextColored(up ? pal::badRed() : pal::okGreen(), "%s", up ? "!" : "ok"); ImGui::SameLine(26 * gUi);
        ImGui::PushTextWrapPos(0);
        if (detail[0]) ImGui::Text("%s  (%s)", k, detail); else ImGui::TextUnformatted(k);
        ImGui::PopTextWrapPos();
    };
    if (on) {
        ImGui::TextDisabled("Band");
        kv("noise floor", "%.1f dBFS/Hz, %+.1f dB", r.noiseDb, r.riseDb);
        kv("total power", "%.1f dBFS", r.powerDb);
        kv("flatness", "%.2f", r.flatness);
        kv("signal type", "%s", gnssInterfKindNames(r.kinds).c_str());
        if (r.kinds & GnssKindChirp) kv("swept tone", "%.0f %%, %.3f MHz/us", r.chirpScore * 100, r.chirpRateHzPerUs / 1e6);
        else kv("swept tone", "%.0f %%", r.chirpScore * 100);
        kv("pulses", "%.0f /s, duty %.2f %%", r.pulseRateHz, r.pulseDuty * 100);
        if (r.cn0N) kv("C/N0", "%.1f dB-Hz, %d sats (ref %.1f)", r.cn0Avg, r.cn0N, r.cn0Ref);
        else kv("C/N0", "%s", "no satellite tracked");
        ImGui::Spacing();
        ImGui::TextDisabled("Spoofing indicators");
        char b[96];
        snprintf(b, sizeof b, "spread %.1f dB", r.cn0Spread); flag("C/N0 alike on all satellites", r.cn0Uniform, r.cn0N >= 5 ? b : "");
        snprintf(b, sizeof b, "%.0f m", r.lastJumpM); flag("Position jump", r.posJump, r.posJump ? b : "");
        snprintf(b, sizeof b, "%.2f us", r.lastClockJumpM / 299.792458); flag("Clock jump", r.clockJump, r.clockJump ? b : "");
        snprintf(b, sizeof b, "%.0f m, %d fixes", r.distFromAvgM, r.avgFixes); flag("Far from the long-term position", r.farFromAverage, r.avgFixes >= 30 ? b : "averaging");
        snprintf(b, sizeof b, "%.1f km", r.almMismatchKm); flag("Ephemeris against almanac", r.almanacMismatch, r.almMismatchKm > 0 ? b : "");
        ImGui::Spacing();
        if (!r.spikes.empty() && ImGui::BeginTable("##gspk", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Line kHz"); ImGui::TableSetupColumn("dB"); ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            for (const auto& sp : r.spikes) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%+.1f", sp.offsetHz / 1e3);
                ImGui::TableNextColumn(); ImGui::Text("%.0f", sp.aboveDb);
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", sp.known ? "known (at the start)" : "");
            }
            ImGui::EndTable();
        }
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Log");
    ImGui::BeginChild("##gilog", ImVec2(0, 0), ImGuiChildFlags_Borders);
    for (const auto& e : r.events) {
        char ts[16];
        const std::time_t tt = (std::time_t)e.unixS;
        std::strftime(ts, sizeof ts, "%H:%M:%S", std::localtime(&tt));
        ImGui::TextDisabled("%s", ts); ImGui::SameLine();
        ImGui::PushTextWrapPos(0);
        if (e.level == 0) ImGui::TextUnformatted(e.text.c_str());
        else ImGui::TextColored(e.level == 2 ? pal::badRed() : pal::warnAmber(), "%s", e.text.c_str());
        ImGui::PopTextWrapPos();
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::EndChild();
}

// ---------------------------------------------------------------- the clock: UTC from the satellites, the computer clock against it

std::string gnssUtcText(const GnssFix& f) {
    char b[64];
    snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%06.3f", f.year, f.month, f.day, f.hour, f.minute, f.second);
    return b;
}

void clockView(App& a) {
    const GnssTelemetry& t = a.rx.gnss;
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to read the time from the satellites"); return; }
    const GnssFix& f = t.fix;
    ImGui::BeginChild("##gnssclk", ImVec2(0, 0));
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    ImGui::PushFont(a.mono, 30 * gUi);
    if (f.valid && f.timeValid) ImGui::TextColored(t.utcValid ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%.19s UTC", gnssUtcText(f).c_str());
    else ImGui::TextDisabled("----.--.-- --:--:-- UTC");
    ImGui::PopFont();
    if (f.valid && f.timeValid) {
        kv("GPS time", "week %d, %.3f s of the week (GPS - UTC %d s%s)", f.gpsWeek, f.gpsTow, t.utcValid ? t.leapSeconds : 18, t.utcValid ? "" : ", assumed until the message says");
        if (t.utcValid) {
            if (t.leapPending) kv("leap second", "announced: GPS - UTC becomes %d s after day %d of GPS week %d (mod 256)", t.leapNext, t.leapDn, t.leapWn);
            else kv("leap second", "none announced (GPS - UTC = %d s)", t.leapSeconds);
        } else kv("leap second", "not yet: the UTC parameters come with subframe 4, page 18 (up to 12.5 minutes)");
        kv("UT1 - UTC", "not in the L1 C/A message (GPS sends UT1 only on the newer L2C / L5 civil signals); a time station sends DUT1");
        kv("receiver clock", "%+.1f m (%+.1f ns) from GPS time, drift %+.2f m/s", f.clockBiasM[GnssGps], f.clockBiasM[GnssGps] / 0.299792458, f.clockDriftMps);
    }
    ImGui::Spacing();
    if (t.clockValid) {
        const double o = t.clockOffsetAvgMs;
        const ImVec4 c = std::fabs(o) < 50 ? pal::okGreen() : std::fabs(o) < 1000 ? pal::warnAmber() : pal::badRed();
        ImGui::TextDisabled("computer clock"); kvColumn(130 * gUi);
        ImGui::PushFont(a.mono, 0);
        ImGui::TextColored(c, "%+.1f ms %s", o, o >= 0 ? "ahead" : "behind");
        ImGui::PopFont();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The computer's clock when each sample arrived, minus the satellites' UTC for that sample, averaged over the last minute.\nIt includes the radio's USB buffering (the earliest arrival is used), typically 5 - 30 ms.");
        kv("last fix", "%+.1f ms", t.clockOffsetMs);
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Accuracy: limited by the USB and driver latency of the radio, not by GPS (which is good to nanoseconds). Read the offset as +- 5 - 30 ms."); ImGui::PopTextWrapPos(); }
    } else {
        kv("computer clock", "%s", f.valid ? "nothing to compare (a recording or the simulator)" : "once there is a fix with the date known");
    }
    ImGui::Spacing();
    float lo, hi;
    clockMinMax(t.offsetHist, lo, hi, 1);
    clockPlot("##gco", "clock offset", t.offsetHist, lo, hi, "ms");
    clockPlot("##gcs", "best C/N0", t.snrHist, 0, 60, "dB-Hz");
    clockMinMax(t.levelHist, lo, hi, 3);
    clockPlot("##gcl", "level", t.levelHist, lo, hi, "dBFS");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    clockSyncPanel(a);
    ImGui::EndChild();
}

// ---------------------------------------------------------------- the hooks

void tab(App& a) {
    subNav("gnssv", S.viewMode, {"Sky", "Map", "Channels", "Interference", "Clock"});
    const GnssTelemetry& t = a.rx.gnss;
    const bool on = live(a);
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    if (S.viewMode == 2) { channelTable(a, false); return; }
    if (S.viewMode == 3) { interfView(a); return; }
    if (S.viewMode == 4) { clockView(a); return; }
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
    if (!live(a)) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see satellites"); ImGui::PopTextWrapPos(); } return; }
    const GnssTelemetry& t = a.rx.gnss;
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%d tracked, %d in the fix", t.nTracked, t.fix.valid ? t.fix.nSats : 0); ImGui::PopTextWrapPos(); }
    channelTable(a, true);
}

void receiver(App& a) {
    const GnssTelemetry& t = a.rx.gnss;
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    std::string sys;
    const unsigned dm = t.decodeMask ? t.decodeMask : t.activeMask;
    for (int s = 0; s < GnssSystems; s++) if (dm & gnssSystemBit(s)) sys += std::string(sys.empty() ? "" : ", ") + gnssSystemName(s);
    kv("signals", "%s at %.3f MHz, %.3f Msps", sys.empty() ? "none in this band" : sys.c_str(), t.centerMhz, t.inputRate / 1e6);
    kv("search", "%s", t.searching ? (satName(t.searchSys, t.searchPrn) + ", " + std::to_string((int)(t.searchProgress * 100)) + " % of round " + std::to_string(t.searchRounds + 1)).c_str() : "idle");
    kv("search window", "%+.1f kHz +- %.1f kHz, %d ms", t.searchCenterHz / 1e3, t.searchHalfHz / 1e3, t.searchMs);
    if (t.fix.valid) kv("frequency error", "%+.0f Hz (%+.2f ppm), measured; remembered for this radio", t.cfoHz, t.cfoHz / 1575.42);
    else kv("frequency error", "%s", t.nTracked > 0 ? "about the search centre until the fix" : "not known yet");
    if (t.firstLockSecs >= 0) kv("first lock", "%.1f s", t.firstLockSecs);
    kv("navigation", "%llu frames good, %llu failed the parity", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    if (t.sbasMessages > 0) kv("SBAS", "%llu messages (types shown, corrections not applied)", (unsigned long long)t.sbasMessages);
    if (t.fix.gloTimeValid) kv("GLONASS time", "tau_GPS %+.1f ns, tau_c %+.1f ns (UTC(SU) + 3 h)", t.fix.gloTauGpsNs, t.fix.gloTauCNs);
    if (t.fix.bdtGpsApplied || t.fix.bdtGpsNs != 0) kv("BDT - GPS", "%+.1f ns (less 14 s)", t.fix.bdtGpsNs);
    kv("almanac", "%d GPS satellites", t.almanacGps);
    kv("ionosphere", "%s", t.ionoValid ? "model from the satellites" : "not yet");
    if (t.utcValid) kv("leap seconds", "%d", t.leapSeconds); else kv("leap seconds", "not yet");
    kv("front end", "%.1f dBFS, clipped %.2f %%, DC %+.3f %+.3f", t.levelDbfs, t.clipPercent, t.dcI, t.dcQ);
    kv("signal time", "%.1f s", t.signalSecs);
    ImGui::Spacing();
    ImGui::TextDisabled("Navigation data");
    if (ImGui::BeginTable("##gnav", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Sat"); ImGui::TableSetupColumn("Ephemeris"); ImGui::TableSetupColumn("IODE"); ImGui::TableSetupColumn("Age s");
        ImGui::TableSetupColumn("Week"); ImGui::TableSetupColumn("TOW s"); ImGui::TableSetupColumn("Clock us");
        ImGui::TableHeadersRow();
        for (const auto& n : t.nav) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(satNameK(n.sys, n.prn, n.fcn).c_str());
            ImGui::TableNextColumn();
            if (n.hasEphemeris) ImGui::TextUnformatted("yes");
            else if (n.sys == GnssSbas) { if (n.sbasMessages > 0) ImGui::Text("%u msgs, type %d", n.sbasMessages, n.sbasLastType); else ImGui::TextUnformatted("no message yet"); }
            else if (n.ephEtaS >= 0) ImGui::Text("%d/3, ~%.0f s", n.ephParts, n.ephEtaS);
            else { ImGui::PushTextWrapPos(0); ImGui::TextUnformatted("waiting for the frame"); ImGui::PopTextWrapPos(); }
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
    if (on && !t.scatterI.empty()) captionFit(sq, "Prompt I/Q (%s)", satName(t.scatterSys, t.scatterPrn).c_str()); else captionFit(sq, "Prompt I/Q");   // never wider than the plot
    promptScatter(t, on, ImVec2(sq, plotH));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(rest, "Search: correlation over one code period");
    acqPlot(t, on, ImVec2(rest, plotH));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(rest, "Satellites tracked (and in the fix)");
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
    captionFit(sq, "Position scatter");
    fixScatter(ImVec2(sq, plotH));
    ImGui::EndGroup();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const GnssTelemetry& t = a.rx.gnss;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    int withEph = 0;
    if (on) for (const auto& n : t.nav) withEph += n.hasEphemeris;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Tracking", !on ? 0 : t.nTracked >= 4 ? 1 : t.nTracked > 0 ? 2 : 0); flowNext(12 * gUi);
    lamp("Ephemeris", !on ? 0 : withEph >= 4 ? 1 : withEph > 0 ? 2 : 0); flowNext(12 * gUi);
    lamp("Fix", !on ? 0 : t.fix.valid ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    auto ro = [&](const char* label, const std::string& val) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
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
    ImGui::SetNextItemWidth(std::min(230 * gUi, ImGui::GetContentRegionAvail().x));   // no wider than the side panel
    const int cur = gnssTuneOf(a.freqMhz);
    struct TuneItem { const char* label; const char* tip; };
    static const TuneItem kItems[] = {
        {"L1 / E1  1575.42 MHz", "GPS L1 C/A, Galileo E1, QZSS and SBAS, all at once on the same band."},
        {"BeiDou B1I  1561.098 MHz", "BeiDou B1I: the MEO and IGSO satellites (D1 message) and the GEO satellites (D2), 4 Msps.\n14 MHz below L1: a tune of its own."},
        {"GLONASS L1  1602 MHz", "GLONASS L1OF: fourteen frequency channels, 1602 MHz + k x 0.5625 MHz (k = -7 .. +6), 10 Msps for all of them.\n"
                                 "A radio held to fewer (a PlutoSDR on USB, 4 Msps) gets the middle channels, about k = -2 .. +2."},
        {"All systems  1582 MHz", "GPS, Galileo, QZSS, SBAS, BeiDou and GLONASS in one tune: 1559 to 1606 MHz at 50 Msps."},
    };
    // All systems needs a radio that runs 50 Msps (PlutoSDR on Ethernet, bladeRF, USRP); the test signal stops at 20 Msps
    const DeviceInfo* dev = a.devIdx >= 0 && a.devIdx < (int)a.devices.size() ? &a.devices[(size_t)a.devIdx] : nullptr;
    const RateLimits lim = dev ? rateLimitsOf(*dev) : RateLimits();
    const bool allOk = dev && dev->isRadio() && lim.maxHz >= kGnssAllRate - 1;
    const char* allWhy = !dev || !dev->isRadio() ? "All systems needs a radio that runs 50 Msps; the test signal and recordings stop at 20 Msps."
                                                 : "This radio does not reach 50 Msps (a PlutoSDR on Ethernet, a bladeRF or a USRP does): tune the systems one by one.";
    // a tune the radio runs at another rate is opened again; the same rate is a plain retune
    auto pick = [&](int k) {
        a.freqMhz = kGnssTuneMhz[k]; a.tune.centerHz = a.freqMhz * 1e6;
        a.tune.synth.modeOpt[5] = (int)std::lround(a.freqMhz * 1000.0);
        const double oldRate = a.tune.sampleRate;
        applyBandwidth(a);
        if (a.engine.running() && a.tune.sampleRate != oldRate) startReceiver(a); else retune = true;
        savePrefs(a);
    };
    if (ImGui::BeginCombo("##gband", cur >= 0 ? kItems[cur].label : "custom frequency")) {
        for (int k = 0; k < (int)(sizeof kItems / sizeof *kItems); k++) {
            const bool off = k == GnssTuneAll && !allOk;
            if (off) ImGui::BeginDisabled();
            if (ImGui::Selectable(kItems[k].label, cur == k) && cur != k) pick(k);
            if (off) ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s%s%s", kItems[k].tip, off ? "\n\n" : "", off ? allWhy : "");
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("GPS, Galileo, QZSS and SBAS on 1575.42 MHz, BeiDou B1I on 1561.098 MHz, GLONASS on 1602 MHz. Galileo needs 4 Msps or more and starts once GPS has a fix.\n"
                                                  "An active antenna is needed: switch on the antenna power of the radio (HackRF: the antenna port power)\n"
                                                  "and give the antenna a view of the sky. Indoors there is usually nothing to receive.");
}

void decoder(App& a, bool&) {
    loadState();
    ImGui::TextDisabled("MASK"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(90 * gUi, ImGui::GetContentRegionAvail().x));   // no wider than the side panel
    float m = (float)S.elMask;
    if (ImGui::SliderFloat("##gmask", &m, 0.f, 30.f, "%.0f deg")) { S.elMask = m; plat::prefs().setD("gnssElMask", S.elMask); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Satellites lower than this are tracked but left out of the position: low signals cross more air and reflect more.");
    (void)a;
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("simulated sky"); ImGui::PopTextWrapPos(); }
    {
        // the systems in the simulated sky (modeOpt[0], gnssSystemBit; 0 = GPS alone)
        const int sysList[6] = {GnssGps, GnssGalileo, GnssQzss, GnssSbas, GnssBeidou, GnssGlonass};
        if (sc.modeOpt[0] <= 0) {   // a fresh sky shows every system the receiver decodes
            unsigned all = 0;
            for (int k = 0; k < 6; k++) all |= gnssSystemBit(sysList[k]);
            sc.modeOpt[0] = (int)all; changed = true;
        }
        // a system whose band the tune now holds is switched on (once per tune: it can be switched off again)
        static int lastTune = -2;
        const int tune = gnssTuneOf(a.freqMhz);
        if (tune != lastTune) {
            lastTune = tune;
            if ((tune == GnssTuneB1I || tune == GnssTuneAll) && !(sc.modeOpt[0] & (int)gnssSystemBit(GnssBeidou))) { sc.modeOpt[0] |= (int)gnssSystemBit(GnssBeidou); changed = true; }
            if ((tune == GnssTuneGlo || tune == GnssTuneAll) && !(sc.modeOpt[0] & (int)gnssSystemBit(GnssGlonass))) { sc.modeOpt[0] |= (int)gnssSystemBit(GnssGlonass); changed = true; }
        }
        unsigned m = (unsigned)sc.modeOpt[0];
        for (int k = 0; k < 6; k++) {
            flowNext();
            bool on = (m & gnssSystemBit(sysList[k])) != 0;
            if (ImGui::Checkbox(gnssSystemName(sysList[k]), &on)) {
                m = on ? (m | gnssSystemBit(sysList[k])) : (m & ~gnssSystemBit(sysList[k]));
                if (m == 0) m = gnssSystemBit(GnssGps);
                sc.modeOpt[0] = (int)m; changed = true;
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("QZSS and SBAS send on the GPS band with their own codes; Galileo E1-B needs 4 Msps or more.\nBeiDou B1I is on 1561.098 MHz and GLONASS on 1598 to 1605 MHz: only a tune that holds their band shows them.");
    }
    flowNext(); ImGui::SetNextItemWidth(100 * gUi);
    float cn0 = sc.modeVal[3] > 0 ? (float)sc.modeVal[3] : 44.f;
    if (ImGui::SliderFloat("##gcn0", &cn0, 30.f, 50.f, "%.0f dB-Hz")) { sc.modeVal[3] = cn0; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("C/N0 of a satellite overhead. Lower satellites are weaker (about 9 dB less near the horizon).\nAbout 44 dB-Hz is a good outdoor antenna, below about 30 the navigation data no longer decodes.");
    flowNext(); ImGui::SetNextItemWidth(90 * gUi);
    int n = sc.modeOpt[1] > 0 ? sc.modeOpt[1] : 0;
    if (ImGui::SliderInt("##gns", &n, 0, 12, n == 0 ? "all in view" : "%d satellites")) { sc.modeOpt[1] = n; changed = true; }
    flowNext();
    bool cold = sc.modeOpt[2] == 1;
    if (ImGui::Checkbox("cold start", &cold)) { sc.modeOpt[2] = cold ? 1 : 0; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Warm: the signal starts at a convenient place in the navigation message and the first fix comes after about 26 s.\nCold: like switching on at a random moment, about 35 s.");
    flowNext();
    bool jam = (sc.modeOpt[3] & 1) != 0;
    if (ImGui::Checkbox("jammer", &jam)) { sc.modeOpt[3] = (sc.modeOpt[3] & ~1) | (jam ? 1 : 0); changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A continuous tone 400 kHz from the centre, much stronger than the satellites. The receiver notches it out.");
    flowNext(); ImGui::TextDisabled("CFO"); ImGui::SameLine(); ImGui::SetNextItemWidth(100 * gUi);
    float cfo = (float)(sc.cfoHz / 1e3);
    if (ImGui::SliderFloat("##gcfo", &cfo, -160, 160, "%.1f kHz")) { sc.cfoHz = cfo * 1e3; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The radio's frequency error at 1575 MHz: a 1 ppm TCXO is off by about 1.6 kHz, a HackRF up to 31 kHz (20 ppm),\na dongle without a TCXO up to 160 kHz (100 ppm). Ctrl+click to type a value.");
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

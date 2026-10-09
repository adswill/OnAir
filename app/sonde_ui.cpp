// Radiosonde screens: the map with every sonde's track and a balloon marker, the table of sondes heard, the detail of the selected one
// (all fields, altitude and temperature plots, calibration progress), the carriers the search found in the band, and the test signal options.
#include "app.h"
#include "adsb_map.h"
#include "dect2/sonde_tel.h"
#include <algorithm>
#include <cmath>

namespace {

using dect2::SondeInfo;
using dect2::SondeTelemetry;

struct State {
    bool loaded = false;
    int viewMode = 0;                  // 0 map, 1 table
    adsbmap::View map;
    bool mapFollow = true;             // the map follows the selected sonde until the user drags it
    std::string sel;                   // serial of the selected sonde ("" = the most recently heard)
    double pushedCenter = 0;
    uint64_t lastSeq = 0;
};
State S;

// standard 14 is the engine's activeStandard for radiosondes
bool live(const App& a) { return a.engine.running() && a.rx.standard == 14; }

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    S.map.zoom = (int)d.getI("sondeZoom", 8);
}

ImU32 typeColour(int kind, float alpha = 1.f) {
    static const float c[7][3] = {{0.7f, 0.7f, 0.7f}, {0.40f, 0.70f, 0.95f}, {0.55f, 0.85f, 0.50f}, {0.95f, 0.75f, 0.30f}, {0.92f, 0.45f, 0.40f}, {0.80f, 0.55f, 0.90f}, {0.45f, 0.85f, 0.85f}};
    const int k = kind >= 0 && kind < 7 ? kind : 0;
    return IM_COL32((int)(255 * c[k][0]), (int)(255 * c[k][1]), (int)(255 * c[k][2]), (int)(255 * alpha));
}

// the selected sonde, or the most recently heard one when nothing is chosen (or the chosen one is gone)
const SondeInfo* selected(const SondeTelemetry& t) {
    for (const auto& s : t.sondes) if (s.serial == S.sel) return &s;
    return t.sondes.empty() ? nullptr : &t.sondes.front();
}

bool descending(const SondeInfo& s) { return s.hasVel && s.vSpeed < -1.0 && s.maxAltM > s.altM + 200; }

void tick(App& a) {
    loadState();
    if (a.engine.running()) {
        dect2::SondeReceiver& r = a.engine.sonde();
        if (S.pushedCenter != a.freqMhz) { r.setCenterMhz(a.freqMhz); S.pushedCenter = a.freqMhz; }
    } else {
        S.pushedCenter = 0;
    }
    if (live(a) && a.rx.seq != S.lastSeq) {
        S.lastSeq = a.rx.seq;
        const SondeInfo* s = selected(a.rx.sonde);
        if (s && s->hasPos && S.mapFollow) { S.map.lat = s->lat; S.map.lon = s->lon; }
    }
    if (!a.engine.running()) S.lastSeq = 0;
}

std::string heardText(double s) {
    char b[24];
    if (s < 90) snprintf(b, sizeof b, "%.0f s", s);
    else if (s < 5400) snprintf(b, sizeof b, "%.0f min", s / 60);
    else snprintf(b, sizeof b, "%.1f h", s / 3600);
    return b;
}

// ---------------------------------------------------------------- drawing pieces

void mapView(App& a, ImVec2 size) {
    const SondeTelemetry& t = a.rx.sonde;
    const bool on = live(a);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const double lat0 = S.map.lat, lon0 = S.map.lon;
    adsbmap::draw(S.map, size);
    if (S.map.lat != lat0 || S.map.lon != lon0) S.mapFollow = false;
    ImVec2 click;
    const bool clicked = adsbmap::clickedAt(click);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    const SondeInfo* sel = on ? selected(t) : nullptr;
    const SondeInfo* hit = nullptr;
    float hitD = 18.f * gUi;
    if (on) for (const auto& s : t.sondes) {
        if (!s.hasPos) continue;
        const bool isSel = &s == sel;
        const ImU32 col = typeColour(s.kind, isSel ? 1.f : 0.75f);
        if (s.trackIncluded && s.track.size() > 1) {
            ImVec2 prev = adsbmap::project(s.track[0].lat, s.track[0].lon);
            for (size_t i = 1; i < s.track.size(); i++) {
                const ImVec2 q = adsbmap::project(s.track[i].lat, s.track[i].lon);
                dl->AddLine(prev, q, col, isSel ? 2.4f * gUi : 1.4f * gUi);
                prev = q;
            }
        }
        const ImVec2 q = adsbmap::project(s.lat, s.lon);
        // balloon: a round marker with a short string; a falling sonde (after the burst) is drawn as a diamond
        if (descending(s)) {
            const float r = 6 * gUi;
            dl->AddQuadFilled(ImVec2(q.x, q.y - r), ImVec2(q.x + r, q.y), ImVec2(q.x, q.y + r), ImVec2(q.x - r, q.y), col);
        } else {
            dl->AddCircleFilled(ImVec2(q.x, q.y - 4 * gUi), 5 * gUi, col, 20);
            dl->AddLine(ImVec2(q.x, q.y + gUi), ImVec2(q.x, q.y + 6 * gUi), col, 1.2f * gUi);
        }
        if (isSel) dl->AddCircle(q, 10 * gUi, IM_COL32(255, 255, 255, 230), 28, 1.6f);
        char b[48]; snprintf(b, sizeof b, "%s  %.1f km", s.serial.c_str(), s.altM / 1000.0);
        dl->AddText(ImVec2(q.x + 9 * gUi, q.y - 8 * gUi), IM_COL32(235, 238, 240, s.active ? 255 : 150), b);
        const float d = std::hypot(q.x - click.x, q.y - click.y);
        if (clicked && d < hitD) { hitD = d; hit = &s; }
    }
    dl->PopClipRect();
    if (hit) { S.sel = hit->serial; S.mapFollow = true; }
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(26 * gUi, 0))) S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1);
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(26 * gUi, 0))) S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1);
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("follow")) { S.mapFollow = true; if (sel && sel->hasPos) { S.map.lat = sel->lat; S.map.lon = sel->lon; } }
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never the position itself");
    plat::prefs().setI("sondeZoom", S.map.zoom);
    if (!on || !sel || !sel->hasPos) {
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
        adsbmap::legend(size.x, "%s", !on ? "start the receiver" : t.sondes.empty() ? "no sonde heard yet" : "no position yet");
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
}

// the sondes heard, most recently heard first (the receiver sorts them); a click selects
void sondeTable(App& a, bool compact) {
    const SondeTelemetry& t = a.rx.sonde;
    const bool on = live(a);
    const int cols = compact ? 5 : 11;
    if (!ImGui::BeginTable(compact ? "##sd_s" : "##sd_f", cols, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("Serial"); ImGui::TableSetupColumn("Alt m");
    if (!compact) { ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn("V m/s"); }
    ImGui::TableSetupColumn("Temp");
    if (!compact) { ImGui::TableSetupColumn("Sats"); ImGui::TableSetupColumn("SNR dB"); ImGui::TableSetupColumn("Frames"); }
    ImGui::TableSetupColumn("Heard");
    ImGui::TableHeadersRow();
    if (on) {
        const SondeInfo* sel = selected(t);
        for (const auto& s : t.sondes) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(typeColour(s.kind)));
            if (ImGui::Selectable((s.type + "##" + s.serial).c_str(), &s == sel, ImGuiSelectableFlags_SpanAllColumns)) { S.sel = s.serial; S.mapFollow = true; }
            ImGui::PopStyleColor();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(s.serial.c_str());
            ImGui::TableNextColumn(); if (s.hasPos) ImGui::Text("%.0f", s.altM); else ImGui::TextUnformatted("-");
            if (!compact) {
                ImGui::TableNextColumn(); if (s.freqHz > 0) ImGui::Text("%.3f", s.freqHz / 1e6); else ImGui::TextUnformatted("-");
                ImGui::TableNextColumn(); if (s.hasVel) ImGui::Text("%+.1f", s.vSpeed); else ImGui::TextUnformatted("-");
            }
            ImGui::TableNextColumn(); if (s.hasTemp) ImGui::Text("%.1f", s.tempC); else ImGui::TextUnformatted("-");
            if (!compact) {
                ImGui::TableNextColumn(); if (s.sats >= 0) ImGui::Text("%d", s.sats); else ImGui::TextUnformatted("-");
                ImGui::TableNextColumn(); ImGui::Text("%.0f", s.snrDb);
                ImGui::TableNextColumn(); ImGui::Text("%llu/%llu", (unsigned long long)s.framesOk, (unsigned long long)(s.framesOk + s.framesBad));
            }
            ImGui::TableNextColumn();
            if (!s.active) ImGui::PushStyleColor(ImGuiCol_Text, pal::grey());
            ImGui::TextUnformatted(heardText(s.lastHeardS).c_str());
            if (!s.active) ImGui::PopStyleColor();
        }
    }
    ImGui::EndTable();
}

// every field of one sonde
void detailCard(const App& a) {
    const SondeTelemetry& t = a.rx.sonde;
    const bool on = live(a);
    const SondeInfo* sp = on ? selected(t) : nullptr;
    auto kv = [&](const char* k, const std::string& v) { ImGui::TextDisabled("%s", k); kvColumn(92 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(v.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    ImGui::PushStyleColor(ImGuiCol_Text, pal::heading());
    if (sp) ImGui::Text("%s  %s", sp->subtype.empty() ? sp->type.c_str() : sp->subtype.c_str(), sp->serial.c_str());
    else { ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(on ? (t.state == 0 ? "searching" : "no sonde decoded yet") : "stopped"); ImGui::PopTextWrapPos(); }
    ImGui::PopStyleColor();
    const SondeInfo e;
    const SondeInfo& s = sp ? *sp : e;
    char b[96];
    auto txt = [&](bool ok, const char* fmt, double v) -> std::string { if (!sp || !ok) return "-"; snprintf(b, sizeof b, fmt, v); return b; };
    if (sp && s.freqHz > 0) snprintf(b, sizeof b, "%.3f MHz (%+.1f kHz)", s.freqHz / 1e6, s.offsetHz / 1e3); else if (sp) snprintf(b, sizeof b, "%+.1f kHz from centre", s.offsetHz / 1e3); else snprintf(b, sizeof b, "-");
    kv("frequency", b);
    if (sp && s.hasPos) snprintf(b, sizeof b, "%.5f", s.lat); else snprintf(b, sizeof b, "-");
    kv("latitude", b);
    if (sp && s.hasPos) snprintf(b, sizeof b, "%.5f", s.lon); else snprintf(b, sizeof b, "-");
    kv("longitude", b);
    kv("altitude", txt(s.hasPos, "%.0f m", s.altM));
    kv("max altitude", txt(s.hasPos, "%.0f m", s.maxAltM));
    if (sp && s.hasVel) snprintf(b, sizeof b, "%+.1f m/s", s.vSpeed); else snprintf(b, sizeof b, "-");
    kv("vertical", b);
    if (sp && s.hasVel) snprintf(b, sizeof b, "%.1f m/s, %.0f deg", s.hSpeed, s.headingDeg); else snprintf(b, sizeof b, "-");
    kv("horizontal", b);
    kv("satellites", sp && s.sats >= 0 ? std::to_string(s.sats) : "-");
    kv("temperature", txt(s.hasTemp, "%.1f C", s.tempC));
    kv("humidity", txt(s.hasHumidity, "%.0f %%", s.humidity));
    kv("pressure", txt(s.hasPressure, "%.1f hPa", s.pressureHpa));
    kv("battery", txt(sp && s.batteryV >= 0, "%.1f V", s.batteryV));
    if (sp && s.burstKillS >= 0) snprintf(b, sizeof b, "%d min", s.burstKillS / 60); else snprintf(b, sizeof b, "-");
    kv("kill timer", b);
    if (sp && s.hasTime) { time_t tt = (time_t)s.unixTime; struct tm g;
#ifdef _WIN32
        gmtime_s(&g, &tt);
#else
        gmtime_r(&tt, &g);
#endif
        snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec); } else snprintf(b, sizeof b, "-");
    kv("UTC", b);
    if (sp) snprintf(b, sizeof b, "%d  (%llu ok, %llu bad)", s.frame, (unsigned long long)s.framesOk, (unsigned long long)s.framesBad); else snprintf(b, sizeof b, "-");
    kv("frame", b);
    kv("signal", sp ? txt(true, "%.0f dB in 10 kHz", s.snrDb) : "-");
    if (sp && s.calTotal > 0) {
        ImGui::TextDisabled("calibration");
        ImGui::SameLine(92 * gUi);
        char cb[24]; snprintf(cb, sizeof cb, "%d/%d", s.calDone, s.calTotal);
        ImGui::ProgressBar((float)s.calDone / (float)s.calTotal, ImVec2(-1, 0), cb);
    }
    if (sp && !s.note.empty()) ImGui::TextDisabled("%s", s.note.c_str());
    if (sp && s.hasPos) {
        if (ImGui::SmallButton("copy position")) { snprintf(b, sizeof b, "%.6f, %.6f", s.lat, s.lon); ImGui::SetClipboardText(b); }
    }
}

// the band as a strip with the carriers the search found: assigned ones filled, the others hollow
void carrierStrip(const App& a, ImVec2 size) {
    const SondeTelemetry& t = a.rx.sonde;
    const bool on = live(a);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##sdcar", size);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(8, 9, 10, 255));
    if (!on || t.bandHz <= 0) return;
    const float base = p0.y + size.y - 14 * gUi, top = p0.y + 4 * gUi;
    dl->AddLine(ImVec2(p0.x, base), ImVec2(p0.x + size.x, base), IM_COL32(60, 63, 66, 255));
    for (int i = 0; i <= 4; i++) {
        const float x = p0.x + size.x * (float)i / 4.f;
        dl->AddLine(ImVec2(x, base), ImVec2(x, base + 3 * gUi), IM_COL32(90, 93, 96, 255));
        if (t.centerHz > 0) {
            char b[24]; snprintf(b, sizeof b, "%.1f", (t.centerHz + t.bandHz * ((double)i / 4.0 - 0.5)) / 1e6);
            const ImVec2 ts = ImGui::CalcTextSize(b);
            dl->AddText(ImVec2(std::clamp(x - ts.x * 0.5f, p0.x, p0.x + size.x - ts.x), base + 2 * gUi), IM_COL32(140, 144, 148, 255), b);
        }
    }
    for (const auto& c : t.carriers) {
        const float x = p0.x + size.x * (float)(0.5 + c.offsetHz / t.bandHz);
        if (x < p0.x || x > p0.x + size.x) continue;
        const float h = std::clamp(c.levelDb / 40.f, 0.1f, 1.f) * (base - top);
        const ImU32 col = c.assigned ? IM_COL32(110, 200, 130, 255) : IM_COL32(150, 154, 158, 255);
        if (c.assigned) dl->AddRectFilled(ImVec2(x - 2 * gUi, base - h), ImVec2(x + 2 * gUi, base), col);
        else dl->AddRect(ImVec2(x - 2 * gUi, base - h), ImVec2(x + 2 * gUi, base), col);
    }
}

// ---------------------------------------------------------------- the hooks

void tab(App& a) {
    subNav("sondev", S.viewMode, {"Map", "Table"});
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    if (S.viewMode == 1) { sondeTable(a, false); return; }
    const float cw = std::min(300.f * gUi, W * 0.32f);
    ImGui::BeginChild("##sonde_l", ImVec2(W - cw - 8 * gUi, H));
    mapView(a, ImGui::GetContentRegionAvail());
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##sonde_r", ImVec2(0, H));
    detailCard(a);
    ImGui::EndChild();
}

void list(App& a) {
    if (!live(a)) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see sondes"); ImGui::PopTextWrapPos(); } return; }
    const SondeTelemetry& t = a.rx.sonde;
    int act = 0;
    for (const auto& s : t.sondes) act += s.active ? 1 : 0;
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%zu heard, %d active", t.sondes.size(), act); ImGui::PopTextWrapPos(); }
    sondeTable(a, true);
}

void receiver(App& a) {
    const SondeTelemetry& t = a.rx.sonde;
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("band", "%.3f MHz wide, centre %.3f MHz", t.bandHz / 1e6, t.centerHz / 1e6);
    kv("channels", "%d of %d in use", t.channelsUsed, t.channelsMax);
    kv("carrier searches", "%llu, %zu carriers now", (unsigned long long)t.searches, t.carriers.size());
    kv("frames", "%llu good, %llu failed the check", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kv("signal time", "%.1f s", t.streamTimeS);
    ImGui::Spacing();
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Carriers found in the band"); ImGui::PopTextWrapPos(); }
    if (ImGui::BeginTable("##sdcar_t", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Frequency"); ImGui::TableSetupColumn("Level dB"); ImGui::TableSetupColumn("Channel"); ImGui::TableSetupColumn("Sonde");
        ImGui::TableHeadersRow();
        for (const auto& c : t.carriers) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (c.freqHz > 0) ImGui::Text("%.4f MHz", c.freqHz / 1e6); else ImGui::Text("%+.1f kHz", c.offsetHz / 1e3);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", c.levelDb);
            ImGui::TableNextColumn(); if (c.assigned) ImGui::Text("%d", c.channel); else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn();
            const SondeInfo* who = nullptr;
            for (const auto& s : t.sondes) if (c.assigned && s.channel == c.channel) who = &s;
            ImGui::TextUnformatted(who ? (who->type + " " + who->serial).c_str() : c.assigned ? "listening" : "");
        }
        ImGui::EndTable();
    }
}

// the bottom row: altitude against time and temperature against altitude of the selected sonde, and the carriers in the band
void panels(App& a) {
    const SondeTelemetry& t = a.rx.sonde;
    const bool on = live(a);
    const SondeInfo* s = on ? selected(t) : nullptr;
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi, plotH = std::max(60.f, H - ImGui::GetTextLineHeightWithSpacing() - 6);
    const float w = std::max(120.f, (W - 4 * gap) / 3.f);
    const bool haveTrack = s && s->trackIncluded && s->track.size() > 1;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap * 0.5f);
    ImGui::BeginGroup();
    captionFit(w, "Altitude against time");
    if (plt::BeginPlot("##sd_alt", ImVec2(w, plotH), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("minutes", "km", plt::AxisFlags_AutoFit, plt::AxisFlags_AutoFit);
        if (haveTrack) {
            std::vector<float> x, y;
            const double t0 = s->track.front().unixT;
            for (const auto& p : s->track) { x.push_back((float)((p.unixT - t0) / 60.0)); y.push_back(p.altM / 1000.f); }
            if (s->track.back().unixT == s->track.front().unixT) for (size_t i = 0; i < x.size(); i++) x[i] = (float)i;   // a sonde without time: the point number
            plt::PlotLine("alt", x.data(), y.data(), (int)x.size());
        }
        plt::EndPlot();
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(w, "Temperature against altitude");
    if (plt::BeginPlot("##sd_temp", ImVec2(w, plotH), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("deg C", "km", plt::AxisFlags_AutoFit, plt::AxisFlags_AutoFit);
        if (haveTrack) {
            std::vector<float> x, y;
            for (const auto& p : s->track) if (p.tempC > -900.f) { x.push_back(p.tempC); y.push_back(p.altM / 1000.f); }
            if (x.size() > 1) plt::PlotLine("T", x.data(), y.data(), (int)x.size());
        }
        plt::EndPlot();
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(w, "Carriers in the band (filled: being received)");
    carrierStrip(a, ImVec2(w, plotH));
    ImGui::EndGroup();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SondeTelemetry& t = a.rx.sonde;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    int act = 0;
    bool pos = false;
    if (on) for (const auto& s : t.sondes) { act += s.active ? 1 : 0; pos = pos || (s.active && s.hasPos); }
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Carrier", !on ? 0 : !t.carriers.empty() ? 1 : 0); flowNext(12 * gUi);
    lamp("Decoding", !on ? 0 : act > 0 ? 1 : t.state > 0 ? 2 : 0); flowNext(12 * gUi);
    lamp("Position", !on ? 0 : pos ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    auto ro = [&](const char* label, const std::string& val) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    char b[64];
    if (!on) { ro("State", run ? "starting" : "stopped"); return; }
    ro("State", act > 0 ? "Decoding" : t.state > 0 ? "Signal" : "Searching");
    snprintf(b, sizeof b, "%d active of %zu", act, t.sondes.size()); ro("Sondes", b);
    snprintf(b, sizeof b, "%zu", t.carriers.size()); ro("Carriers", b);
    snprintf(b, sizeof b, "%llu / %llu", (unsigned long long)t.blocksOk, (unsigned long long)(t.blocksOk + t.blocksBad)); ro("Frames", b);
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Radiosonde";
    if (live(a)) l2 = dect2::sondeSummary(a.rx.sonde);
}

void tuner(App& a, bool& retune) {
    loadState();
    ImGui::TextDisabled("Band");
    ImGui::SetNextItemWidth(std::min(230 * gUi, ImGui::GetContentRegionAvail().x));   // no wider than the side panel
    const bool std403 = std::fabs(a.freqMhz - 403.0) < 0.001;
    if (ImGui::BeginCombo("##sdband", std403 ? "400 to 406 MHz  (403.0)" : "custom frequency")) {
        if (ImGui::Selectable("400 to 406 MHz  (403.0)", std403)) { a.freqMhz = 403.0; retune = true; }
        ImGui::BeginDisabled();
        ImGui::Selectable("1680 MHz (LMS6, not decoded yet)", false);
        ImGui::EndDisabled();
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every sonde inside the captured width is found and decoded at the same time (up to 8).\nAt 8 Msps the whole 400 to 406 MHz band is not covered: centre it on the sondes you expect, or lower the rate\nand pick a centre near a launch site's frequency. LMS6 sondes (1680 MHz) are not decoded yet.");
}

void decoder(App& a, bool&) {
    loadState();
    ImGui::TextDisabled("CHANNELS"); ImGui::SameLine(0, 5 * gUi);
    static int maxCh = 8;
    ImGui::SetNextItemWidth(std::min(80 * gUi, ImGui::GetContentRegionAvail().x));   // no wider than the side panel
    if (ImGui::SliderInt("##sdch", &maxCh, 1, 8) && a.engine.running()) a.engine.sonde().setMaxChannels(maxCh);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How many sondes are followed at once. Each one costs a little CPU.");
    sameLineIf(ImGui::CalcTextSize("clear list").x + 2 * ImGui::GetStyle().FramePadding.x, 8 * gUi);
    if (ImGui::SmallButton("clear list") && a.engine.running()) { a.engine.sonde().clearSondes(); S.sel.clear(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forget the sondes heard so far (the receiver keeps following the carriers).");
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    ImGui::TextDisabled("simulated sondes");
    // type mask: 0 means RS41, DFM and M10
    static const char* names[5] = {"RS41", "DFM", "M10", "M20", "RS92"};
    int mask = (int)sc.modeOpt[0];
    if (mask == 0) mask = 7;
    for (int i = 0; i < 5; i++) {
        flowNext();
        bool on = (mask >> i) & 1;
        char id[16]; snprintf(id, sizeof id, "%s##sdm%d", names[i], i);
        if (ImGui::Checkbox(id, &on)) {
            const int nm = on ? (mask | (1 << i)) : (mask & ~(1 << i));
            if (nm != 0) { sc.modeOpt[0] = nm == 7 ? 0 : nm; changed = true; }
        }
    }
    flowNext(); ImGui::SetNextItemWidth(90 * gUi);
    int n = sc.modeOpt[1] > 0 ? (int)sc.modeOpt[1] : 3;
    if (ImGui::SliderInt("##sdn", &n, 1, 7, "%d sondes")) { sc.modeOpt[1] = n == 3 && sc.modeOpt[0] == 0 ? 0 : n; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Taken in order RS41, DFM, M10 among the chosen types, then more of them at other frequencies. Three is the default flight from Dubai.");
    flowNext(); ImGui::TextDisabled("drift east"); ImGui::SameLine(); ImGui::SetNextItemWidth(100 * gUi);
    float east = sc.modeVal[0] != 0 ? (float)sc.modeVal[0] : 10.f;
    if (ImGui::SliderFloat("##sdeast", &east, 0.1f, 40.f, "%.0f m/s")) { sc.modeVal[0] = east; changed = true; }
    flowNext(); ImGui::TextDisabled("SNR"); ImGui::SameLine(); ImGui::SetNextItemWidth(100 * gUi);
    float snr = (float)sc.snrDb;
    if (ImGui::SliderFloat("##sdsnr", &snr, 8.f, 40.f, "%.0f dB")) { sc.snrDb = snr; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Signal to noise ratio of each sonde in 10 kHz. Below about 9 dB frames start to fail.");
    flowNext(); ImGui::TextDisabled("CFO"); ImGui::SameLine(); ImGui::SetNextItemWidth(100 * gUi);
    float cfo = (float)(sc.cfoHz / 1e3);
    if (ImGui::SliderFloat("##sdcfo", &cfo, -10, 10, "%.1f kHz")) { sc.cfoHz = cfo * 1e3; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The radio's frequency error at 403 MHz (a sonde's own oscillator adds a few kHz).");
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const SondeTelemetry& t = a.rx.sonde;
    int act = 0;
    for (const auto& s : t.sondes) act += s.active ? 1 : 0;
    out.push_back({"SONDES", "%.0f", (double)act, 0, 8, act > 0 ? 1 : 0});
    out.push_back({"CARRIERS", "%.0f", (double)t.carriers.size(), 0, 16, t.carriers.empty() ? 0 : 1});
    out.push_back({"BEST SNR  dB", "%.0f", (double)t.snrDb, 0, 40, t.snrDb >= 15 ? 1 : t.snrDb >= 9 ? 2 : 3});
    const uint64_t tot = t.blocksOk + t.blocksBad;
    const double ok = tot > 0 ? 100.0 * (double)t.blocksOk / (double)tot : 0.0;
    out.push_back({"FRAMES OK  %", "%.0f", ok, 0, 100, tot == 0 ? 0 : ok >= 90 ? 1 : ok >= 60 ? 2 : 3});
}

} // namespace

extern const ModeUi kSondeUi;
const ModeUi kSondeUi = {
    .sideTitle = "SONDES",
    .tabName = "Sondes",
    .tabIcon = Ic::Pulse,
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

// ADS-B screens: the aircraft table, a radar plot around the receiver, the message monitor, statistics and the options of the test signal.
#include "app.h"
#include <cmath>
#include <deque>

namespace {

struct State {
    bool loaded = false, wasRunning = false;
    double refLat = 25.25, refLon = 55.36;      // where the antenna is: ranges and local decoding; the default is only a placeholder
    double pushedLat = 1e9, pushedLon = 1e9;
    uint32_t sel = 0xFFFFFFFFu;                 // the selected aircraft (ICAO address)
    int correction = 1;
    std::deque<float> rate, snr;                // good messages per second and SNR, a point per report
    uint64_t lastSeq = 0;
};
State S;

bool live(const App& a) { return a.engine.running() && a.rx.standard == 12; }

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    if (d.has("adsbLat")) S.refLat = d.getD("adsbLat", S.refLat);
    if (d.has("adsbLon")) S.refLon = d.getD("adsbLon", S.refLon);
}

const ImVec4 kDim(0.62f, 0.65f, 0.68f, 1), kGood(0.40f, 0.85f, 0.50f, 1), kWarn(0.95f, 0.60f, 0.25f, 1);

std::string icaoText(uint32_t i) { char b[16]; snprintf(b, sizeof b, "%06X", i & 0xFFFFFF); return b; }

void tick(App& a) {
    loadState();
    a.tune.synth.modeVal[2] = S.refLat; a.tune.synth.modeVal[3] = S.refLon;   // the simulated traffic flies around the reference point
    const bool run = a.engine.running();
    if (run && (!S.wasRunning || S.pushedLat != S.refLat || S.pushedLon != S.refLon)) {
        a.engine.adsb().setReference(S.refLat, S.refLon);
        a.engine.adsb().setCorrection(S.correction);
        S.pushedLat = S.refLat; S.pushedLon = S.refLon;
    }
    S.wasRunning = run;
    if (live(a) && a.rx.seq != S.lastSeq) {
        S.lastSeq = a.rx.seq;
        const AdsbTelemetry& t = a.rx.adsb;
        S.rate.push_back(t.msgsPerSec); if (S.rate.size() > 240) S.rate.pop_front();
        if (t.dataValid) { S.snr.push_back(t.snrDb); if (S.snr.size() > 240) S.snr.pop_front(); }
    }
}

// one number or a dash
std::string num(bool has, const char* fmt, double v) { if (!has) return "-"; char b[32]; snprintf(b, sizeof b, fmt, v); return b; }

void aircraftTable(App& a, bool compact) {
    const AdsbTelemetry& t = a.rx.adsb;
    const bool on = live(a);
    const int cols = compact ? 5 : 11;
    if (!ImGui::BeginTable(compact ? "##acs" : "##acf", cols, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("ICAO", ImGuiTableColumnFlags_WidthFixed, 62 * gUi); ImGui::TableSetupColumn("Callsign", ImGuiTableColumnFlags_WidthFixed, 72 * gUi);
    ImGui::TableSetupColumn("Alt ft"); if (!compact) { ImGui::TableSetupColumn("Spd kt"); ImGui::TableSetupColumn("Hdg"); ImGui::TableSetupColumn("V/S"); }
    ImGui::TableSetupColumn("Dist nm");
    if (!compact) { ImGui::TableSetupColumn("Sqk"); ImGui::TableSetupColumn("Msgs"); ImGui::TableSetupColumn("Age s"); ImGui::TableSetupColumn("dBFS"); }
    ImGui::TableHeadersRow();
    if (on) for (const AdsbAircraft& ac : t.aircraft) {
        ImGui::TableNextRow();
        const bool emg = ac.emergency > 0;
        if (emg) ImGui::PushStyleColor(ImGuiCol_Text, pal::badRed());
        ImGui::TableNextColumn();
        char id[24]; snprintf(id, sizeof id, "%s##%u", icaoText(ac.icao).c_str(), ac.icao);
        if (ImGui::Selectable(id, S.sel == ac.icao, ImGuiSelectableFlags_SpanAllColumns)) S.sel = ac.icao;
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ac.callsign.empty() ? "-" : ac.callsign.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ac.ground ? "ground" : num(ac.hasAlt, "%.0f", ac.altFt).c_str());
        if (!compact) {
            ImGui::TableNextColumn(); ImGui::TextUnformatted(num(ac.hasSpeed, "%.0f", ac.speedKt).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(num(ac.hasHeading, "%.0f", ac.headingDeg).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(num(ac.hasVrate, "%+.0f", ac.vrateFpm).c_str());
        }
        ImGui::TableNextColumn(); ImGui::TextUnformatted(num(ac.hasRange, "%.1f", ac.distNm).c_str());
        if (!compact) {
            ImGui::TableNextColumn(); if (ac.hasSquawk) ImGui::Text("%04d", ac.squawk); else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); ImGui::Text("%u", ac.messages);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", ac.ageSec);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", ac.levelDbfs);
        }
        if (emg) ImGui::PopStyleColor();
    }
    ImGui::EndTable();
}

// the radar: the receiver in the middle, north up, a ring every few tens of nautical miles
void radar(App& a, ImVec2 size) {
    const AdsbTelemetry& t = a.rx.adsb;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##radar", size);
    const float side = std::min(size.x, size.y), R = side * 0.5f - 6 * gUi;
    const ImVec2 c(p0.x + size.x * 0.5f, p0.y + size.y * 0.5f);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(8, 9, 10, 255));
    double maxNm = 20;
    const bool on = live(a);
    if (on) for (const auto& ac : t.aircraft) if (ac.hasRange) maxNm = std::max<double>(maxNm, ac.distNm * 1.1);
    static const double steps[] = {20, 50, 100, 150, 200, 300, 400, 600};
    double scale = steps[7];
    for (double s : steps) if (s >= maxNm) { scale = s; break; }
    const int rings = scale <= 20 ? 4 : scale <= 50 ? 5 : 4;
    const ImU32 grid = IM_COL32(60, 64, 66, 255);
    for (int i = 1; i <= rings; i++) {
        dl->AddCircle(c, R * i / rings, grid, 64);
        char b[24]; snprintf(b, sizeof b, "%.0f nm", scale * i / rings);
        dl->AddText(ImVec2(c.x + 3 * gUi, c.y - R * i / rings), IM_COL32(110, 114, 118, 255), b);
    }
    dl->AddLine(ImVec2(c.x - R, c.y), ImVec2(c.x + R, c.y), grid); dl->AddLine(ImVec2(c.x, c.y - R), ImVec2(c.x, c.y + R), grid);
    dl->AddText(ImVec2(c.x - 3 * gUi, c.y - R - ImGui::GetTextLineHeight() - 1), IM_COL32(130, 134, 138, 255), "N");
    dl->AddCircleFilled(c, 3 * gUi, ImGui::ColorConvertFloat4ToU32(pal::accent()));
    if (!on) return;
    const double k = std::cos(S.refLat * M_PI / 180.0);
    auto pt = [&](double lat, double lon) { return ImVec2(c.x + (float)((lon - S.refLon) * k * 60.0 / scale * R), c.y - (float)((lat - S.refLat) * 60.0 / scale * R)); };
    for (const auto& ac : t.aircraft) {
        if (!ac.hasPos) continue;
        const bool sel = ac.icao == S.sel;
        const ImU32 col = ac.emergency ? ImGui::ColorConvertFloat4ToU32(pal::badRed()) : sel ? IM_COL32(255, 255, 255, 255) : ImGui::ColorConvertFloat4ToU32(pal::accent());
        ImVec2 prev;
        bool first = true;
        for (const auto& tp : ac.track) { const ImVec2 q = pt(tp.lat, tp.lon); if (!first) dl->AddLine(prev, q, IM_COL32(70, 74, 78, 255)); prev = q; first = false; }
        const ImVec2 q = pt(ac.lat, ac.lon);
        if (std::fabs(q.x - c.x) > R * 1.1f || std::fabs(q.y - c.y) > R * 1.1f) continue;
        if (ac.hasHeading) { const float h = (float)(ac.headingDeg * M_PI / 180.0); dl->AddLine(q, ImVec2(q.x + std::sin(h) * 10 * gUi, q.y - std::cos(h) * 10 * gUi), col, 1.5f); }
        dl->AddCircleFilled(q, (sel ? 4.f : 3.f) * gUi, col);
        char b[48]; snprintf(b, sizeof b, "%s", ac.callsign.empty() ? icaoText(ac.icao).c_str() : ac.callsign.c_str());
        dl->AddText(ImVec2(q.x + 6 * gUi, q.y - 6 * gUi), IM_COL32(190, 194, 198, 255), b);
    }
    if (!t.refValid) dl->AddText(ImVec2(p0.x + 6 * gUi, p0.y + 4 * gUi), IM_COL32(150, 154, 158, 255), "no reference position");
}

void detail(const App& a) {
    const AdsbTelemetry& t = a.rx.adsb;
    const AdsbAircraft* ac = nullptr;
    if (live(a)) for (const auto& x : t.aircraft) if (x.icao == S.sel) ac = &x;
    if (!ac) { ImGui::TextDisabled("click an aircraft"); return; }
    auto kv = [&](const char* k, const std::string& v) { ImGui::TextDisabled("%s", k); ImGui::SameLine(96 * gUi); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(v.c_str()); ImGui::PopFont(); };
    char b[96];
    kv("ICAO", icaoText(ac->icao) + (ac->icao >> 24 ? " (not an ICAO address)" : ""));
    kv("callsign", ac->callsign.empty() ? "-" : ac->callsign + (ac->category.empty() ? "" : "  (" + ac->category + ")"));
    if (ac->hasPos) { snprintf(b, sizeof b, "%.4f  %.4f  (%s)", ac->lat, ac->lon, ac->posKind == 2 ? "global" : ac->posKind == 3 ? "local, confirmed" : "local"); kv("position", b); } else kv("position", "-");
    if (ac->hasRange) { snprintf(b, sizeof b, "%.1f nm at %.0f deg", ac->distNm, ac->bearingDeg); kv("range", b); }
    kv("altitude", ac->hasAlt ? num(true, "%.0f ft barometric", ac->altFt) : "-");
    if (ac->hasGeoAlt) kv("GNSS height", num(true, "%.0f ft", ac->geoAltFt));
    kv("speed", ac->hasSpeed ? num(true, "%.0f kt", ac->speedKt) + (ac->speedKind == 0 ? " ground" : ac->speedKind == 1 ? " indicated" : " true") : "-");
    kv(ac->headingIsTrack ? "track" : "heading", ac->hasHeading ? num(true, "%.0f deg", ac->headingDeg) : "-");
    kv("vertical", ac->hasVrate ? num(true, "%+.0f ft/min", ac->vrateFpm) : "-");
    if (ac->hasSquawk) { snprintf(b, sizeof b, "%04d", ac->squawk); kv("squawk", b); }
    if (ac->hasSelAlt) kv("selected alt", num(true, "%.0f ft", ac->selAltFt));
    if (ac->hasMach) kv("mach", num(true, "%.2f", ac->mach));
    if (ac->emergency) { snprintf(b, sizeof b, "state %d", ac->emergency); kv("emergency", b); }
    snprintf(b, sizeof b, "%u messages, last %.0f s ago, %.0f dBFS", ac->messages, ac->ageSec, ac->levelDbfs); kv("heard", b);
}

// ---------------------------------------------------------------- the hooks

void tab(App& a) {
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float rw = std::min(W * 0.42f, H * 0.62f + 80 * gUi);
    ImGui::BeginChild("##adsb_l", ImVec2(W - rw - 8 * gUi, H));
    aircraftTable(a, false);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##adsb_r", ImVec2(0, H));
    radar(a, ImVec2(ImGui::GetContentRegionAvail().x, std::max(120.f * gUi, ImGui::GetContentRegionAvail().y - 190 * gUi)));
    ImGui::Spacing();
    detail(a);
    ImGui::EndChild();
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see aircraft"); return; }
    ImGui::TextDisabled("%u aircraft, %u with position", a.rx.adsb.aircraftCount, a.rx.adsb.withPosition);
    aircraftTable(a, true);
}

void receiver(App& a) {
    const AdsbTelemetry& t = a.rx.adsb;
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); ImGui::SameLine(120 * gUi); ImGui::PushFont(a.mono, 0); ImGui::Text(fmt, v...); ImGui::PopFont(); };
    kv("state", "%s", t.state == 2 ? "receiving" : t.state == 1 ? "pulses, no good message" : "searching");
    kv("messages", "%.0f / s, %llu good, %llu failed the CRC", t.msgsPerSec, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kv("repaired", "%llu", (unsigned long long)t.corrected);
    kv("signal", "%.1f dBFS, noise %.1f dBFS, SNR %.1f dB", t.levelDbfs, t.noiseDbfs, t.snrDb);
    kv("aircraft", "%u (%u with position)", t.aircraftCount, t.withPosition);
    if (t.refValid) kv("farthest", "%.0f nm", t.maxRangeNm);
    ImGui::Spacing();
    ImGui::TextDisabled("Last messages");
    if (ImGui::BeginTable("##mon", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("DF"); ImGui::TableSetupColumn("ICAO"); ImGui::TableSetupColumn("dBFS"); ImGui::TableSetupColumn("fix"); ImGui::TableSetupColumn("message"); ImGui::TableSetupColumn("what");
        ImGui::TableHeadersRow();
        for (size_t i = t.frames.size(); i-- > 0;) {
            const AdsbFrameInfo& f = t.frames[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d", f.df);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(icaoText(f.icao).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.0f", f.levelDbfs);
            ImGui::TableNextColumn(); ImGui::Text("%d", f.corrected);
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(f.hex.c_str()); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(f.what.c_str());
        }
        ImGui::EndTable();
    }
}

void panels(App& a) {
    const AdsbTelemetry& t = a.rx.adsb;
    const bool on = live(a);
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi, colW = std::max(120.f, (W - 3 * gap) / 3.f), plotH = std::max(60.f, H - ImGui::GetTextLineHeightWithSpacing() - 6);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap * 0.5f);
    ImGui::BeginGroup();
    ImGui::TextDisabled("Messages per second");
    std::vector<float> v(S.rate.begin(), S.rate.end());
    if (v.empty()) v.push_back(0);
    ImGui::PlotLines("##rate", v.data(), (int)v.size(), 0, nullptr, 0.f, FLT_MAX, ImVec2(colW, plotH));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    ImGui::TextDisabled("Good messages by downlink format");
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##df", ImVec2(colW, plotH));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        static const int dfs[] = {0, 4, 5, 11, 16, 17, 18, 20, 21};
        uint64_t mx = 1;
        if (on) for (int d : dfs) mx = std::max(mx, t.dfCount[d]);
        const float bw = colW / 9.f;
        for (int i = 0; i < 9; i++) {
            const uint64_t n = on ? t.dfCount[dfs[i]] : 0;
            const float h = (plotH - 16 * gUi) * (float)n / (float)mx;
            dl->AddRectFilled(ImVec2(p.x + i * bw + 3, p.y + plotH - 14 * gUi - h), ImVec2(p.x + (i + 1) * bw - 3, p.y + plotH - 14 * gUi), ImGui::ColorConvertFloat4ToU32(pal::accent(0.8f)));
            char b[8]; snprintf(b, sizeof b, "%d", dfs[i]);
            dl->AddText(ImVec2(p.x + i * bw + bw * 0.5f - 4 * gUi, p.y + plotH - 13 * gUi), IM_COL32(150, 154, 158, 255), b);
        }
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    ImGui::TextDisabled("SNR of good messages (dB)");
    std::vector<float> s(S.snr.begin(), S.snr.end());
    if (s.empty()) s.push_back(0);
    ImGui::PlotLines("##snr", s.data(), (int)s.size(), 0, nullptr, 0.f, FLT_MAX, ImVec2(colW, plotH));
    ImGui::EndGroup();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const AdsbTelemetry& t = a.rx.adsb;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight() * 2.f + ImGui::GetStyle().ItemSpacing.y * 2.f;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x - 4, p.y - 2), ImVec2(p.x + ImGui::GetContentRegionAvail().x + 4, p.y + h), IM_COL32(22, 23, 25, 255), 3.f);
    }
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); ImGui::SameLine(0, 12 * gUi);
    lamp("Messages", !on ? 0 : t.state == 2 ? 1 : t.state == 1 ? 2 : 0); ImGui::SameLine(0, 12 * gUi);
    lamp("Positions", !on ? 0 : t.withPosition > 0 ? 1 : t.aircraftCount > 0 ? 2 : 0); ImGui::SameLine(0, 10 * gUi);
    ImGui::TextDisabled("|"); ImGui::SameLine(0, 10 * gUi);
    auto ro = [&](const char* label, const std::string& val) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(val.c_str()); ImGui::PopFont();
        ImGui::SameLine(0, 15 * gUi);
    };
    char b[64];
    if (!on) { ro("State", run ? "starting" : "stopped"); return; }
    ro("State", t.state == 2 ? "Receiving" : t.state == 1 ? "Pulses only" : "Searching");
    snprintf(b, sizeof b, "%.0f /s", t.msgsPerSec); ro("Messages", b);
    snprintf(b, sizeof b, "%u (%u)", t.aircraftCount, t.withPosition); ro("Aircraft", b);
    snprintf(b, sizeof b, "%.1f dB", t.snrDb); ro("SNR", b);
    snprintf(b, sizeof b, "%.1f dBFS", t.noiseDbfs); ro("Noise", b);
    if (t.refValid && t.maxRangeNm > 0) { snprintf(b, sizeof b, "%.0f nm", t.maxRangeNm); ro("Farthest", b); }
}

void summary(const App& a, std::string& l1, std::string& l2) {
    const AdsbTelemetry& t = a.rx.adsb;
    char b[96];
    l1 = "ADS-B";
    if (!live(a)) return;
    snprintf(b, sizeof b, "%s  %u aircraft  %.0f msg/s  SNR %.0f dB", t.state == 2 ? "receiving" : "searching", t.aircraftCount, t.msgsPerSec, t.snrDb);
    l2 = b;
}

void tuner(App& a, bool& retune) {
    loadState();
    (void)retune;
    ImGui::TextDisabled("Antenna position (for ranges)");
    ImGui::SetNextItemWidth(110 * gUi);
    bool ch = ImGui::InputDouble("##alat", &S.refLat, 0, 0, "%.4f N");
    ImGui::SameLine(0, 4 * gUi);
    ImGui::SetNextItemWidth(110 * gUi);
    ch |= ImGui::InputDouble("##alon", &S.refLon, 0, 0, "%.4f E");
    if (ch) {
        S.refLat = std::max(-90.0, std::min(90.0, S.refLat)); S.refLon = std::max(-180.0, std::min(180.0, S.refLon));
        plat::prefs().setD("adsbLat", S.refLat); plat::prefs().setD("adsbLon", S.refLon);
        savePrefs(a);
    }
}

void decoder(App& a, bool&) {
    static const char* names[] = {"off", "1 bit", "2 bits"};
    ImGui::TextDisabled("FIX"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(90 * gUi);
    if (ImGui::BeginCombo("##fix", names[S.correction])) {
        for (int i = 0; i < 3; i++) if (ImGui::Selectable(names[i], S.correction == i)) { S.correction = i; if (a.engine.running()) a.engine.adsb().setCorrection(i); }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Repair of bit errors in DF11, DF17 and DF18 messages. 2 bits finds more messages but lets more false ones through.");
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    ImGui::TextDisabled("simulated airspace");
    ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    int n = sc.modeOpt[0] == 0 ? 12 : sc.modeOpt[0];
    if (ImGui::SliderInt("##sn", &n, 1, 100, "%d aircraft")) { sc.modeOpt[0] = n; changed = true; }
    ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    float mul = sc.modeVal[0] > 0 ? (float)sc.modeVal[0] : 1.f;
    if (ImGui::SliderFloat("##sm", &mul, 0.2f, 10.f, "x%.1f rate", ImGuiSliderFlags_Logarithmic)) { sc.modeVal[0] = mul; changed = true; }
    ImGui::SameLine();
    bool replies = sc.modeOpt[1] == 0;
    if (ImGui::Checkbox("radar replies", &replies)) { sc.modeOpt[1] = replies ? 0 : 1; changed = true; }
    ImGui::SameLine();
    bool emg = sc.modeOpt[3] == 1;
    if (ImGui::Checkbox("emergency", &emg)) { sc.modeOpt[3] = emg ? 1 : 0; changed = true; }
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const AdsbTelemetry& t = a.rx.adsb;
    out.push_back({"MESSAGES / s", "%.0f", t.msgsPerSec, 0, 100, t.msgsPerSec > 1 ? 1 : 2});
    out.push_back({"AIRCRAFT", "%.0f", (double)t.aircraftCount, 0, 60, 0});
    out.push_back({"SNR  dB", "%.1f", t.snrDb, 0, 40, t.snrDb >= 15 ? 1 : 2});
    out.push_back({"NOISE  dBFS", "%.1f", t.noiseDbfs, -80, -20, t.noiseDbfs > -35 ? 3 : 0});
}

} // namespace

extern const ModeUi kAdsbUi;
const ModeUi kAdsbUi = {
    .sideTitle = "AIRCRAFT",
    .tabName = "Aircraft",
    .tabIcon = Ic::Compass,
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

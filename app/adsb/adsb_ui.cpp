// ADS-B mode entry: the screen state, the receiver position, the tick, and the entry points the shell calls (tab,
// list, receiver, panels, status, summary, tuner, decoder, synth, meters). The table views are in adsb_table_ui.cpp, the
// radar and map in adsb_map_ui.cpp.
#include "adsb_internal.h"
void vdl2NoteAdsb(const dect2::AdsbTelemetry& t);   // vdl2_ui.cpp: VDL2 links its aircraft to what ADS-B heard

using namespace adsbui;

namespace adsbui {

State S;

bool live(const App& a) { return a.rx.standard == 12; }

std::string dbPath() { return plat::dataDir() + "/aircraft.csv"; }

void reloadDb() {
    S.db.clear();
    size_t n = 0, bad = 0;
    if (S.db.loadFile(dbPath(), &n, &bad)) { char b[200]; snprintf(b, sizeof b, "%zu aircraft read from aircraft.csv%s", n, bad ? (", " + std::to_string(bad) + " bad lines skipped").c_str() : ""); S.dbMsg = b; }
    else S.dbMsg = "no aircraft.csv in the data folder";
}

// "NL  Netherlands" style text: the two letter country code (the flag), from the address block
std::string countryCode(uint32_t icao) { return icao >> 24 ? "" : dect2::adsbCountry(icao).iso; }

std::string registrationOf(uint32_t icao) { return icao >> 24 ? "" : S.db.registration(icao); }

std::string operatorOf(uint32_t icao) { const dect2::AdsbRegEntry* e = icao >> 24 ? nullptr : S.db.find(icao); return e ? e->operatorName : ""; }

std::string typeOf(const AdsbAircraft& ac) {
    const dect2::AdsbRegEntry* e = ac.icao >> 24 ? nullptr : S.db.find(ac.icao);
    std::string ty = e ? e->type : "";
    const std::string ct = typeText(ac);
    if (!ct.empty()) ty += (ty.empty() ? "" : ", ") + ct;
    return ty;
}

std::string sourceName(const dect2::AdsbTelemetry& t, int id) { return id >= 0 && id < (int)t.sources.size() ? (id == 0 ? "local" : t.sources[id].name) : "?"; }

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    if (d.has("adsbLat")) S.refLat = d.getD("adsbLat", S.refLat);
    if (d.has("adsbLon")) S.refLon = d.getD("adsbLon", S.refLon);
    S.refAltM = d.getD("rxAltM", 0);
    S.map.zoom = (int)d.getI("adsbZoom", 7);
    S.winds = d.getB("adsbWinds", false);
    S.jamLayer = d.getB("adsbGpsJam", false);
    S.units.sys = d.getI("adsbUnits", 0) == 1 ? dect2::adsbu::System::Metric : dect2::adsbu::System::Aviation;
    S.units.spd = d.getI("adsbSpeedUnit", 0) == 1 ? dect2::adsbu::Speed::MS : dect2::adsbu::Speed::KmH;
    S.disp.altColour = d.getB(dect2::adsbd::kPrefAltColour, true);
    S.disp.labels = d.getB(dect2::adsbd::kPrefLabels, true);
    S.disp.trackWidth = dect2::adsbd::clampTrackWidth((int)d.getI(dect2::adsbd::kPrefTrackWidth, dect2::adsbd::kDefaultTrackWidth));
    S.disp.dropSec = dect2::adsbd::clampDropSec((int)d.getI(dect2::adsbd::kPrefDropSec, dect2::adsbd::kDefaultDropSec));
    S.map.lat = S.refLat; S.map.lon = S.refLon;
    S.srcText = d.getS("adsbSources", "");
    for (char& c : S.srcText) if (c == '|') c = '\n';
    S.srcAny = d.getB("adsbSrcAny", false);
    reloadDb();
}

std::string icaoText(uint32_t i) { char b[16]; snprintf(b, sizeof b, "%06X", i & 0xFFFFFF); return b; }

void setReference(App& a, double lat, double lon) {
    S.refLat = std::max(-90.0, std::min(90.0, lat)); S.refLon = std::max(-180.0, std::min(180.0, lon));
    plat::prefs().setD("adsbLat", S.refLat); plat::prefs().setD("adsbLon", S.refLon);
    S.map.lat = S.refLat; S.map.lon = S.refLon;
    savePrefs(a);
    S.pushedLat = 1e9; S.pushedLon = 1e9;   // force syncAdsbReference to push on the next frame
    if (a.engine.running() && live(a) && receiverLocationSet()) a.engine.adsb().setReference(S.refLat, S.refLon);
}

// the antenna position as two numbers and the Locate button: in the Tuner section and in the map's "position" menu
void positionFields(App& a) {
    ImGui::SetNextItemWidth(std::min(110 * gUi, ImGui::GetContentRegionAvail().x));   // no wider than the side panel
    double la = S.refLat, lo = S.refLon;
    bool ch = ImGui::InputDouble("##alat", &la, 0, 0, "%.4f N");
    ImGui::SameLine(0, 4 * gUi);
    ImGui::SetNextItemWidth(std::min(110 * gUi, ImGui::GetContentRegionAvail().x));   // no wider than the side panel
    ch |= ImGui::InputDouble("##alon", &lo, 0, 0, "%.4f E");
    if (ch) setReference(a, la, lo);
    ImGui::SetNextItemWidth(std::min(110 * gUi, ImGui::GetContentRegionAvail().x));
    double al = S.refAltM;
    if (ImGui::InputDouble("##aalt", &al, 0, 0, "%.0f m")) {
        S.refAltM = std::max(-500.0, std::min(9000.0, al));
        plat::prefs().setD("rxAltM", S.refAltM); savePrefs(a);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Height of the antenna above sea level, metres (the pass planner's look angles and Doppler)");
    ImGui::SameLine(0, 4 * gUi);
    if (ImGui::Button(S.locBusy ? "Locating..." : "Locate (Wi-Fi)") && !S.locBusy) { plat::locateStart(); S.locBusy = 1; S.locMsg = "asking the location service..."; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ask the system for this computer's position (Wi-Fi positioning). macOS asks for permission the first time.");
    if (!S.locMsg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", S.locMsg.c_str()); ImGui::PopTextWrapPos(); }
}

void pollLocate(App& a) {
    if (S.locBusy) {   // the location request of the Locate button
        double la = 0, lo = 0, acc = 0; std::string msg;
        const int st = plat::locateState(la, lo, acc, msg);
        if (st == 2) { setReference(a, la, lo); char b[96]; snprintf(b, sizeof b, "position found (about %.0f m)", acc); S.locMsg = b; S.locBusy = 0; }
        else if (st == 3) { S.locMsg = msg; S.locBusy = 0; }
    }
}

void tick(App& a) {
    loadState();
    pollLocate(a);
    a.tune.synth.modeVal[2] = S.refLat; a.tune.synth.modeVal[3] = S.refLon;   // the simulated traffic flies around the reference point
    const bool run = a.engine.running();
    if (run) {
        if (!S.wasRunning) {
            a.engine.adsb().setCorrection(S.correction);
            S.pushedDrop = -1;
            S.pushedLat = S.pushedLon = 1e9;   // receiver may have been running in another tab: push antenna position again
        }
        syncAdsbReference(a);
        if (S.pushedDrop != S.disp.dropSec) { a.engine.adsb().setDropSec(dect2::adsbd::trackerDropSec(S.disp.dropSec)); S.pushedDrop = S.disp.dropSec; }
    }
    if (run && (!S.srcPushed || S.pushedSrc != S.srcText || S.pushedAny != S.srcAny)) {
        std::vector<std::string> v;
        size_t i = 0;
        while (i <= S.srcText.size()) {
            size_t j = S.srcText.find('\n', i);
            if (j == std::string::npos) j = S.srcText.size();
            std::string l = S.srcText.substr(i, j - i);
            while (!l.empty() && isspace((unsigned char)l.back())) l.pop_back();
            size_t k = 0; while (k < l.size() && isspace((unsigned char)l[k])) k++;
            if (l.size() > k) v.push_back(l.substr(k));
            i = j + 1;
        }
        a.engine.adsb().setExternalSources(v, S.srcAny);
        S.pushedSrc = S.srcText; S.pushedAny = S.srcAny; S.srcPushed = true;
    }
    if (!run && S.wasRunning) { a.engine.adsb().setExternalSources({}, false); S.srcPushed = false; }   // the feeds in end with the receiver
    S.wasRunning = run;
    if (live(a) && a.rx.seq != S.lastSeq) vdl2NoteAdsb(a.rx.adsb);
    if (live(a) && a.rx.seq != S.lastSeq) {
        S.lastSeq = a.rx.seq;
        const AdsbTelemetry& t = a.rx.adsb;
        S.rate.push_back(t.msgsPerSec); if (S.rate.size() > 240) S.rate.pop_front();
        if (t.dataValid) { S.snr.push_back(t.snrDb); if (S.snr.size() > 240) S.snr.pop_front(); }
        rangeSetStation(S.range, S.refLat, S.refLon);
        for (const auto& ac : t.aircraft) if (ac.hasPos) rangeFeed(S.range, ac.lat, ac.lon);
        for (const auto& ac : t.aircraft) {   // accuracy reported by the aircraft, for the GPS interference layer (fresh fixes only)
            if (!ac.hasPos || ac.ground || ac.posAgeSec > 3) continue;
            dect2::GpsJamObs o; o.icao = ac.icao; o.lat = ac.lat; o.lon = ac.lon; o.timeSec = ImGui::GetTime(); o.nacp = ac.nacp; o.nic = ac.nic; o.sil = ac.sil;
            S.jam.observe(o);
        }
    }
}

// ---------------------------------------------------------------- the hooks

void tab(App& a) {
    subNav("adsbv", S.viewMode, {"Map", "Table", "Radar", tr("Range"), "Range stats"});
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float dw = std::min(250.f * gUi, W * 0.26f);
    // the readout beside the map and radar; beside the table only once an aircraft is picked (its ACARS messages are in it)
    const bool pane = S.viewMode == 0 || S.viewMode == 2 || (S.viewMode == 1 && S.sel != 0xFFFFFFFFu);
    ImGui::BeginChild("##adsb_l", ImVec2(pane ? W - dw - 8 * gUi : W, H));
    if (S.viewMode == 0) mapView(a, ImGui::GetContentRegionAvail());
    else if (S.viewMode == 1) aircraftTable(a, false);
    else if (S.viewMode == 2) radar(a, ImGui::GetContentRegionAvail());
    else if (S.viewMode == 3) rangeView(S.range, ImGui::GetContentRegionAvail(), au::distOut(S.units, 1.0 / 1.852), au::distUnit(S.units), "adsb", receiverLocationSet());
    else statsView(a);
    ImGui::EndChild();
    if (pane) {
        ImGui::SameLine();
        ImGui::BeginChild("##adsb_r", ImVec2(0, H));
        detail(a);
        ImGui::EndChild();
    }
}

void list(App& a) {
    if (!live(a)) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see aircraft"); ImGui::PopTextWrapPos(); } return; }
    if (resultsClearButton("adsb")) a.engine.adsb().clearResults();
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%u aircraft, %u with position", a.rx.adsb.aircraftCount, a.rx.adsb.withPosition); ImGui::PopTextWrapPos(); }
    aircraftTable(a, true);
}

void receiver(App& a) {
    const AdsbTelemetry& t = a.rx.adsb;
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(120 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("state", "%s", t.state == 2 ? "receiving" : t.state == 1 ? "pulses, no good message" : "searching");
    kv("messages", "%.0f / s, %llu good, %llu failed the CRC", t.msgsPerSec, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kv("repaired", "%llu", (unsigned long long)t.corrected);
    kv("signal", "%.1f dBFS, noise %.1f dBFS, SNR %.1f dB", t.levelDbfs, t.noiseDbfs, t.snrDb);
    kv("aircraft", "%u (%u with position)", t.aircraftCount, t.withPosition);
    if (t.refValid) kv("farthest", "%s %s", uDist(t.maxRangeNm, 0).c_str(), au::distUnit(S.units));
    ImGui::Spacing();
    ImGui::TextDisabled("Last messages");
    if (ImGui::BeginTable("##mon", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
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
    captionFit(colW, "Messages per second");   // the captions are never wider than their plots
    std::vector<float> v(S.rate.begin(), S.rate.end());
    if (v.empty()) v.push_back(0);
    ImGui::PlotLines("##rate", v.data(), (int)v.size(), 0, nullptr, 0.f, FLT_MAX, ImVec2(colW, plotH));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "Good messages by downlink format");
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
            const ImVec2 ts = ImGui::CalcTextSize(b);
            if (ts.x <= bw - 2) dl->AddText(ImVec2(p.x + i * bw + (bw - ts.x) * 0.5f, p.y + plotH - 13 * gUi), IM_COL32(150, 154, 158, 255), b);   // only where it fits under its bar
        }
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "SNR of good messages (dB)");
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
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Messages", !on ? 0 : t.state == 2 ? 1 : t.state == 1 ? 2 : 0); flowNext(12 * gUi);
    lamp("Positions", !on ? 0 : t.withPosition > 0 ? 1 : t.aircraftCount > 0 ? 2 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    auto ro = [&](const char* label, const std::string& val) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    char b[64];
    if (!on) { ro("State", run ? "starting" : "stopped"); return; }
    ro("State", t.state == 2 ? "Receiving" : t.state == 1 ? "Pulses only" : "Searching");
    snprintf(b, sizeof b, "%.0f /s", t.msgsPerSec); ro("Messages", b);
    snprintf(b, sizeof b, "%u (%u)", t.aircraftCount, t.withPosition); ro("Aircraft", b);
    snprintf(b, sizeof b, "%.1f dB", t.snrDb); ro("SNR", b);
    snprintf(b, sizeof b, "%.1f dBFS", t.noiseDbfs); ro("Noise", b);
    if (t.refValid && t.maxRangeNm > 0) { snprintf(b, sizeof b, "%s %s", uDist(t.maxRangeNm, 0).c_str(), au::distUnit(S.units)); ro("Farthest", b); }
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
    ImGui::TextDisabled("Band");
    ImGui::SetNextItemWidth(std::min(220 * gUi, ImGui::GetContentRegionAvail().x));   // no wider than the side panel
    const bool std1090 = std::fabs(a.freqMhz - 1090.0) < 0.001;
    if (ImGui::BeginCombo("##band", std1090 ? "1090 MHz  ADS-B / Mode S" : "custom frequency")) {
        if (ImGui::Selectable("1090 MHz  ADS-B / Mode S", std1090)) { a.freqMhz = 1090.0; retune = true; }
        ImGui::BeginDisabled();
        ImGui::Selectable("978 MHz  UAT (US only)", false);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("UAT is a separate system used by small aircraft in the US below 18,000 ft (FSK, not Mode S). OnAir does not decode it yet.");
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Europe, the US and the rest of the world all send ADS-B on 1090 MHz (1090ES). The US has a second system, UAT on 978 MHz, which is not decoded yet.");
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Antenna position (for ranges and the map)"); ImGui::PopTextWrapPos(); }
    positionFields(a);
    double areaLat, areaLon, nearest;
    if (live(a) && positionLooksWrong(a.rx.adsb, areaLat, areaLon, nearest)) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1), "Every aircraft is over %.0f nm away: this position is probably not yours.", nearest);
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Use the aircraft's area##ref")) setReference(a, areaLat, areaLon);
    }
}

void decoder(App& a, bool&) {
    static const char* names[] = {"off", "1 bit", "2 bits"};
    ImGui::TextDisabled("FIX"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(90 * gUi, ImGui::GetContentRegionAvail().x));   // no wider than the side panel
    if (ImGui::BeginCombo("##fix", names[S.correction])) {
        for (int i = 0; i < 3; i++) if (ImGui::Selectable(names[i], S.correction == i)) { S.correction = i; if (a.engine.running()) a.engine.adsb().setCorrection(i); }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Repair of bit errors in DF11, DF17 and DF18 messages. 2 bits finds more messages but lets more false ones through.");
    ImGui::Spacing();
    sourcesPanel(a);
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("simulated airspace"); ImGui::PopTextWrapPos(); }
    flowNext(); ImGui::SetNextItemWidth(90 * gUi);
    int n = sc.modeOpt[0] == 0 ? 12 : sc.modeOpt[0];
    if (ImGui::SliderInt("##sn", &n, 1, 100, "%d aircraft")) { sc.modeOpt[0] = n; changed = true; }
    flowNext(); ImGui::SetNextItemWidth(90 * gUi);
    float mul = sc.modeVal[0] > 0 ? (float)sc.modeVal[0] : 1.f;
    if (ImGui::SliderFloat("##sm", &mul, 0.2f, 10.f, "x%.1f rate", ImGuiSliderFlags_Logarithmic)) { sc.modeVal[0] = mul; changed = true; }
    flowNext();
    bool replies = sc.modeOpt[1] == 0;
    if (ImGui::Checkbox("radar replies", &replies)) { sc.modeOpt[1] = replies ? 0 : 1; changed = true; }
    flowNext();
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

}  // namespace adsbui

std::string adsbJamBody(const std::string& station) {
    return dect2::GpsJamGrid::toJson(station, adsbui::S.jam.cells(ImGui::GetTime()));
}

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

// The receiver's position, shared by every screen that needs one (ADS-B ranges, the pass planner): the ADS-B antenna position
void syncAdsbReference(App& a) {
    if (!a.engine.running() || a.rx.standard != 12) return;
    loadState();
    if (!receiverLocationSet()) return;
    if (S.pushedLat == S.refLat && S.pushedLon == S.refLon) return;
    a.engine.adsb().setReference(S.refLat, S.refLon);
    S.pushedLat = S.refLat; S.pushedLon = S.refLon;
}

void receiverLocation(double& lat, double& lon) { loadState(); lat = S.refLat; lon = S.refLon; }

bool receiverLocationSet() { return plat::prefs().has("adsbLat") && plat::prefs().has("adsbLon"); }

double receiverAltitudeM() { loadState(); return S.refAltM; }

void setReceiverLocation(App& a, double lat, double lon) { loadState(); setReference(a, lat, lon); }

void receiverPositionFields(App& a) { loadState(); pollLocate(a); positionFields(a); }

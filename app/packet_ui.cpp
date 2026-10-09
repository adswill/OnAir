// APRS / Packet screens: the stations heard (table and map, the ADS-B map underneath, as for AIS), the log of raw packets, the state lamps.
#include "app.h"
#include "adsb_map.h"
#include "dect2/packet_tel.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

using dect2::PacketStation;

struct State {
    bool loaded = false;
    int view = 0;                    // 0 stations, 1 map, 2 packets
    std::string sel;                 // the selected station (callsign), empty = none
    adsbmap::View map;
    bool mapFollow = true;           // the map fits the stations until the user drags it
    bool logScroll = true;
};
State S;

bool live(const App& a) { return a.engine.running() && a.rx.standard == 25; }   // the engine reports its standard code minus one

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.map.zoom = (int)plat::prefs().getI("packetZoom", 9);
}

std::string ageText(double s) {
    char b[24];
    if (s < 0) s = 0;
    if (s < 90) snprintf(b, sizeof b, "%.0f s", s);
    else if (s < 5400) snprintf(b, sizeof b, "%.0f min", s / 60);
    else snprintf(b, sizeof b, "%.1f h", s / 3600);
    return b;
}

std::string clockText(double s) {
    char b[24];
    const int t = (int)s;
    snprintf(b, sizeof b, "%02d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    return b;
}

std::string posText(const PacketStation& s) {
    if (!s.hasPos) return "-";
    char b[48];
    snprintf(b, sizeof b, "%.4f%c %.4f%c", std::fabs(s.lat), s.lat < 0 ? 'S' : 'N', std::fabs(s.lon), s.lon < 0 ? 'W' : 'E');
    return b;
}

// the common APRS symbols of the primary table; anything else shows its two characters
const char* symbolName(char table, char code) {
    if (table == '\\') return code == '#' ? "digipeater" : code == '_' ? "weather" : nullptr;
    switch (code) {
    case '>': return "car";
    case '-': return "house";
    case '#': return "digipeater";
    case '_': return "weather";
    case 'k': return "truck";
    case '<': return "motorcycle";
    case 'b': return "bicycle";
    case '[': return "person";
    case 'O': return "balloon";
    case 'Y': return "yacht";
    case 's': return "ship";
    case '^': return "aircraft";
    case '\'': return "small aircraft";
    case 'I': return "TCP/IP";
    case 'r': return "antenna";
    case 'a': return "ambulance";
    case 'f': return "fire truck";
    case 'u': return "truck";
    case 'v': return "van";
    case '`': return "dish";
    case '!': return "police";
    case '$': return "phone";
    case '&': return "gateway";
    case '/': return "dot";
    default: return nullptr;
    }
}

std::string symText(const PacketStation& s) {
    if (!s.symCode) return "-";
    char b[40];
    const char* n = symbolName(s.symTable, s.symCode);
    if (n) snprintf(b, sizeof b, "%c%c  %s", s.symTable ? s.symTable : '/', s.symCode, n);
    else snprintf(b, sizeof b, "%c%c", s.symTable ? s.symTable : '/', s.symCode);
    return b;
}

// information fields can hold control characters (Mic-E): show them as dots
std::string printable(const std::string& s) {
    std::string o = s;
    for (char& c : o) if ((unsigned char)c < 32 || (unsigned char)c > 126) c = '.';
    return o;
}

// the text cut to w pixels with two dots at the end (the whole text is in a tooltip)
std::string fitText(const std::string& s, float w) {
    if (ImGui::CalcTextSize(s.c_str()).x <= w) return s;
    std::string t = s;
    while (!t.empty() && ImGui::CalcTextSize((t + "..").c_str()).x > w) t.pop_back();
    return t + "..";
}

ImU32 stationColour(const PacketStation& s, float alpha = 1.f) {
    int r, g, b;
    if (s.isObject) { r = 240; g = 190; b = 80; }
    else if (s.symCode == '_') { r = 80; g = 205; b = 200; }
    else if (s.symCode == '#' || s.symCode == '&') { r = 225; g = 228; b = 232; }
    else if (s.hasSpeed && s.speedKnots > 0.5) { r = 110; g = 205; b = 120; }
    else { r = 100; g = 160; b = 240; }
    return IM_COL32(r, g, b, (int)(255 * alpha));
}

// ---------------------------------------------------------------- the stations table

void stationTable(App& a, bool compact) {
    const dect2::PacketTelemetry& t = a.rx.packet;
    const int cols = compact ? 2 : 5;
    ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable(compact ? "##pk_s" : "##pk_f", cols, fl, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Callsign", ImGuiTableColumnFlags_WidthStretch);
    if (!compact) ImGui::TableSetupColumn("Symbol");
    ImGui::TableSetupColumn("Last heard");
    if (!compact) { ImGui::TableSetupColumn("Position"); ImGui::TableSetupColumn("Comment", ImGuiTableColumnFlags_WidthStretch); }
    ImGui::TableHeadersRow();
    for (const PacketStation& s : t.stations) {      // newest first, as the receiver orders them
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(s.call.c_str());
        const ImVec2 c = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(c.x + 4 * gUi, c.y + ImGui::GetTextLineHeight() * 0.5f), 3.f * gUi, stationColour(s));
        ImGui::SetCursorScreenPos(ImVec2(c.x + 12 * gUi, c.y));
        if (ImGui::Selectable(s.call.c_str(), s.call == S.sel, ImGuiSelectableFlags_SpanAllColumns)) S.sel = s.call;
        ImGui::PopID();
        if (!compact) { ImGui::TableNextColumn(); ImGui::TextUnformatted(symText(s).c_str()); }
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ageText(t.timeSec - s.lastHeardSec).c_str());
        if (!compact) {
            ImGui::TableNextColumn(); ImGui::TextUnformatted(posText(s).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(fitText(printable(s.comment), ImGui::GetContentRegionAvail().x).c_str());
            if (!s.comment.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", printable(s.comment).c_str());
        }
    }
    ImGui::EndTable();
}

// ---------------------------------------------------------------- the map

void mapView(App& a, ImVec2 size) {
    const dect2::PacketTelemetry& t = a.rx.packet;
    const bool on = live(a);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (S.mapFollow && on) {   // fit the stations that have a position, until the user moves the map
        double la0 = 90, la1 = -90, lo0 = 180, lo1 = -180; int n = 0;
        for (const auto& v : t.stations) if (v.hasPos) { la0 = std::min(la0, v.lat); la1 = std::max(la1, v.lat); lo0 = std::min(lo0, v.lon); lo1 = std::max(lo1, v.lon); n++; }
        if (n) {
            S.map.lat = (la0 + la1) / 2; S.map.lon = (lo0 + lo1) / 2;
            const double span = std::max(0.005, std::max(lo1 - lo0, (la1 - la0) * 1.4));
            S.map.zoom = std::max(2, std::min(12, (int)std::floor(std::log2(0.7 * size.x * 360.0 / (256.0 * span)))));
        }
    }
    const double lat0 = S.map.lat, lon0 = S.map.lon;
    adsbmap::draw(S.map, size);
    if (S.map.lat != lat0 || S.map.lon != lon0) S.mapFollow = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    const PacketStation* hit = nullptr; float best = 16 * gUi;
    ImVec2 click; const bool clicked = adsbmap::clickedAt(click);
    if (on) {
        for (const auto& v : t.stations) {
            if (!v.hasPos) continue;
            const ImVec2 q = adsbmap::project(v.lat, v.lon);
            if (q.x < p0.x - 40 || q.y < p0.y - 40 || q.x > p0.x + size.x + 40 || q.y > p0.y + size.y + 40) continue;
            const bool sel = v.call == S.sel;
            const float r = (sel ? 8.f : 7.f) * gUi * 0.7f;
            dl->AddCircleFilled(q, r, stationColour(v));
            dl->AddCircle(q, r, sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 150), 16, sel ? 1.6f : 1.f);
            const ImVec2 ts = ImGui::CalcTextSize(v.call.c_str());
            const ImVec2 lp(q.x + 12 * gUi, q.y - ts.y * 0.5f - 2 * gUi);
            dl->AddRectFilled(lp, ImVec2(lp.x + ts.x + 8 * gUi, lp.y + ts.y + 4 * gUi), IM_COL32(10, 12, 14, 210), 2.f);
            dl->AddRect(lp, ImVec2(lp.x + ts.x + 8 * gUi, lp.y + ts.y + 4 * gUi), sel ? IM_COL32(255, 255, 255, 200) : IM_COL32(60, 64, 68, 255), 2.f);
            dl->AddText(ImVec2(lp.x + 4 * gUi, lp.y + 2 * gUi), IM_COL32(235, 238, 240, 255), v.call.c_str());
            const float d = std::hypot(q.x - click.x, q.y - click.y);
            if (clicked && d < best) { best = d; hit = &v; }
        }
    }
    if (clicked) S.sel = hit ? hit->call : std::string();
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(26 * gUi, 0))) { S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1); S.mapFollow = false; }
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(26 * gUi, 0))) { S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1); S.mapFollow = false; }
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("fit")) S.mapFollow = true;
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never the position itself");
    plat::prefs().setI("packetZoom", S.map.zoom);
    int withPos = 0;
    if (on) for (const auto& v : t.stations) if (v.hasPos) withPos++;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
    if (!on) adsbmap::legend(size.x, "start the receiver");
    else if (!withPos) adsbmap::legend(size.x, "no positions yet");
    else adsbmap::legend(size.x, "%d of %zu stations with a position", withPos, t.stations.size());
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
}

// ---------------------------------------------------------------- the packet log

std::string frameLine(const dect2::PacketFrameInfo& f) {
    std::string s = f.from + ">" + f.to;
    if (!f.path.empty()) s += "," + f.path;
    return s;
}

void packetLog(App& a) {
    const dect2::PacketTelemetry& t = a.rx.packet;
    if (ImGui::SmallButton("copy all")) {
        std::string all;
        for (const auto& f : t.frames) { all += clockText(f.timeSec) + "  " + frameLine(f) + ":" + printable(f.info) + "\n"; }
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    ImGui::Checkbox("follow", &S.logScroll);
    ImGui::SameLine();
    ImGui::TextDisabled("the last %zu good packets", t.frames.size());
    ImGui::BeginChild("##pk_log", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (ImGui::BeginTable("##pk_t", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Time");
        ImGui::TableSetupColumn("From > To,Path");
        ImGui::TableSetupColumn("Info", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGui::PushFont(a.mono, 0);
        for (const auto& f : t.frames) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clockText(f.timeSec).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(frameLine(f).c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%d baud, %s", f.baud, f.type.c_str());
            ImGui::TableNextColumn(); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(printable(f.info).c_str()); ImGui::PopTextWrapPos();
        }
        ImGui::PopFont();
        ImGui::EndTable();
    }
    if (t.frames.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("nothing decoded yet"); ImGui::PopTextWrapPos(); }
    if (S.logScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
}

// ---------------------------------------------------------------- lamps and the hooks

int speedLamp(const dect2::PacketTelemetry& t, double last, uint64_t ok, uint64_t bad) {
    if (ok && t.timeSec - last < 60) return 1;       // frames in the last minute
    if (ok || bad) return 2;                         // heard before, or only damaged frames
    return 0;
}

void lamps(App& a) {
    const dect2::PacketTelemetry& t = a.rx.packet;
    const bool on = live(a);
    lamp("Signal", !on ? 0 : t.state == 2 ? 1 : t.state == 1 ? 2 : 0); flowNext(10 * gUi);
    lamp("1200", !on ? 0 : speedLamp(t, t.last1200Sec, t.ok1200, t.bad1200)); flowNext(10 * gUi);
    lamp("9600", !on ? 0 : speedLamp(t, t.last9600Sec, t.ok9600, t.bad9600)); flowNext(10 * gUi);
    lamp("APRS", !on ? 0 : t.aprsFrames && t.timeSec - t.lastAprsSec < 60 ? 1 : t.aprsFrames ? 2 : 0);
}

void tab(App& a) {
    loadState();
    lamps(a);
    subNav("pktv", S.view, {"Stations", "Map", "Packets"});
    if (S.view == 2) { packetLog(a); return; }
    if (S.view == 1) { mapView(a, ImGui::GetContentRegionAvail()); return; }
    if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver"); ImGui::PopTextWrapPos(); return; }
    stationTable(a, false);
}

void list(App& a) {
    loadState();
    if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see stations"); ImGui::PopTextWrapPos(); return; }
    ImGui::TextDisabled("%zu stations", a.rx.packet.stations.size());
    stationTable(a, true);
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::PacketTelemetry& t = a.rx.packet;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("state", "%s", t.state == 0 ? "no signal" : t.state == 1 ? "signal, no good frame" : "decoding");
    kv("1200 baud", "%llu good, %llu failed the check", (unsigned long long)t.ok1200, (unsigned long long)t.bad1200);
    kv("9600 baud", "%llu good, %llu failed the check", (unsigned long long)t.ok9600, (unsigned long long)t.bad9600);
    kv("APRS packets", "%llu of %llu good frames", (unsigned long long)t.aprsFrames, (unsigned long long)t.blocksOk);
    kv("signal", "%.1f dB over the noise, carrier error %+.0f Hz", t.snrDb, t.cfoHz);
    kv("input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kv("stations", "%zu in the table", t.stations.size());
    kv("signal time", "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const dect2::PacketTelemetry& t = a.rx.packet;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Signal", !on ? 0 : t.state == 2 ? 1 : t.state == 1 ? 2 : 0); flowNext(10 * gUi);
    lamp("1200", !on ? 0 : speedLamp(t, t.last1200Sec, t.ok1200, t.bad1200)); flowNext(10 * gUi);
    lamp("9600", !on ? 0 : speedLamp(t, t.last9600Sec, t.ok9600, t.bad9600)); flowNext(10 * gUi);
    lamp("APRS", !on ? 0 : t.aprsFrames && t.timeSec - t.lastAprsSec < 60 ? 1 : t.aprsFrames ? 2 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Frames"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0);
    if (on) ImGui::Text("%llu", (unsigned long long)t.blocksOk); else ImGui::TextUnformatted(run ? "starting" : "stopped");
    ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "APRS / Packet";
    if (live(a)) l2 = dect2::packetSummary(a.rx.packet);
}

} // namespace

extern const ModeUi kPacketUi;
const ModeUi kPacketUi = {
    .sideTitle = "STATIONS",
    .tabName = "Packets",
    .tabIcon = Ic::Doc,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
};

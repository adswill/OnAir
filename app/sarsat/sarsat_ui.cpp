// COSPAS-SARSAT 406 MHz screens: the beacon messages that were decoded (15 hex ID, type, country, protocol, identification, position, 121.5 MHz homing flag,
// the two BCH checks), a map of the ones that carry a position, and the raw bits. Receive only: a real beacon means somebody may be in danger.
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "../adsb/adsb_map.h"
#include "dect2/sarsat/sarsat_rx.h"
#include "dect2/sarsat/sarsat_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <set>
#include <string>
#include <vector>

namespace sarsat = dect2::sarsat;

namespace {

struct State {
    int view = 0;                 // 0 beacons, 1 map, 2 raw bits
    int sel = 0;                  // index into the log of the selected beacon
    adsbmap::View map;
    bool mapFollow = true, loaded = false;
};
State S;

bool live(const App& a) { return a.rx.standard == 93; }   // the engine reports its standard code minus one

void load() {
    if (S.loaded) return;
    S.loaded = true;
    S.map.zoom = (int)plat::prefs().getI("sarsatZoom", 3);
    S.map.lat = 40.0; S.map.lon = 10.0;
    S.view = (int)std::max(0L, std::min(2L, (long)plat::prefs().getI("sarsatView", 0)));
}

const char* kHelp = N_(
    "A 406 MHz distress beacon (EPIRB on a boat, ELT in an aircraft, PLB carried by a person) sends a burst of about half a second every 50 seconds: an unmodulated "
    "carrier, then 112 or 144 bits at 400 bit/s. The message holds a 15 digit hex ID that is unique to the beacon, the country, the kind of beacon, and often a "
    "position (rounded to 15 minutes in a short message, to 4 seconds in a long one). Two BCH checks protect the message; up to three damaged bits in the first "
    "field are repaired.\n\nOnAir only receives. Test beacons and self-test bursts are marked. If you think you heard a real one, tell the rescue coordination "
    "centre of your country; do not rely on this program for safety.");

std::string clockText(int64_t wall) {
    return dect2::timefmt::clock((int64_t)wall);
}

std::string posText(const sarsat::Beacon& b) {
    if (!b.hasPos) return "-";
    char s[96];
    if (b.fine) snprintf(s, sizeof s, "%.4f %c, %.4f %c", std::fabs(b.lat), b.lat < 0 ? 'S' : 'N', std::fabs(b.lon), b.lon < 0 ? 'W' : 'E');
    else snprintf(s, sizeof s, "%.2f %c, %.2f %c", std::fabs(b.lat), b.lat < 0 ? 'S' : 'N', std::fabs(b.lon), b.lon < 0 ? 'W' : 'E');
    return s;
}

bool isTest(const sarsat::Beacon& b) { return b.selfTest || b.beaconType == "Test"; }

ImVec4 typeColour(const sarsat::Beacon& b) {
    if (isTest(b)) return ImVec4(0.62f, 0.64f, 0.68f, 1);
    if (b.beaconType == "EPIRB") return ImVec4(0.45f, 0.72f, 0.98f, 1);
    if (b.beaconType == "ELT") return ImVec4(0.98f, 0.70f, 0.38f, 1);
    if (b.beaconType == "PLB") return ImVec4(0.52f, 0.88f, 0.52f, 1);
    return ImVec4(0.86f, 0.62f, 0.90f, 1);
}

const char* typeName(const sarsat::Beacon& b) {
    if (b.beaconType == "EPIRB") return tr("EPIRB (boat)");
    if (b.beaconType == "ELT") return tr("ELT (aircraft)");
    if (b.beaconType == "PLB") return tr("PLB (person)");
    if (b.beaconType == "Ship security") return tr("Ship security");
    if (b.beaconType == "Test") return tr("Test");
    return "?";
}

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

void helpButton() {
    if (ImGui::SmallButton("?##sarhelp")) ImGui::OpenPopup("##sarhelppop");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("What a 406 MHz beacon sends"));
    if (ImGui::BeginPopup("##sarhelppop")) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34);
        ImGui::TextUnformatted(tr(kHelp));
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
}

void details(App& a, const dect2::SarsatEntry& e) {
    const sarsat::Beacon& b = e.b;
    kvRow(a, tr("Hex ID"), "%s", b.hexId.c_str());
    kvRow(a, tr("Type"), "%s%s", typeName(b), b.selfTest ? tr(" (self-test burst)") : "");
    kvRow(a, tr("Country"), "%s (MID %d)", b.country.empty() ? tr("unknown") : b.country.c_str(), b.mid);
    kvRow(a, tr("Protocol"), "%s", b.protoName.c_str());
    kvRow(a, tr("Identification"), "%s", b.idText.c_str());
    kvRow(a, tr("Position"), "%s", posText(b).c_str());
    if (b.hasPos) kvRow(a, tr("Precision"), "%s", b.second ? tr("about 3 m (15 bit fractions of a degree)") : b.fine ? tr("about 4 seconds of arc") : tr("15 minutes of arc (short message)"));
    kvRow(a, tr("Homing 121.5 MHz"), "%s", b.homing ? tr("yes") : tr("no"));
    if (b.second) kvRow(a, tr("Rotating field"), "%d", b.rotId);
    kvRow(a, tr("Message"), "%s", b.second ? tr("second generation, 250 bits") : b.longMsg ? tr("long, 144 bits") : tr("short, 112 bits"));
    kvRow(a, tr("Check bits"), "%s%s", b.second ? tr("BCH(250,202) good") : b.bch2Present ? (b.bch2Ok ? tr("both good") : tr("first good, second failed")) : tr("first good"),
          b.corrected ? (std::string(", ") + std::to_string(b.corrected) + " " + tr("bits repaired")).c_str() : "");
    kvRow(a, tr("Signal"), "%.1f dB, %+.0f Hz, %.0f dBFS", e.snrDb, e.cfoHz, e.levelDb);
}

void beaconsView(App& a) {
    const dect2::SarsatTelemetry& t = a.rx.sarsat;
    if (t.log.empty()) {
        ImGui::TextDisabled("%s", tr("no beacon decoded yet; a beacon sends a burst about every 50 seconds"));
        return;
    }
    S.sel = std::max(0, std::min((int)t.log.size() - 1, S.sel));
    const float tableH = std::max(110.f * gUi, ImGui::GetContentRegionAvail().y * 0.5f);
    if (ImGui::BeginTable("##sarlog", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit, ImVec2(0, tableH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Seen"));
        ImGui::TableSetupColumn(tr("Hex ID"));
        ImGui::TableSetupColumn(tr("Type"));
        ImGui::TableSetupColumn(tr("Country"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Position"));
        ImGui::TableSetupColumn(tr("Checks"));
        ImGui::TableSetupColumn(tr("Level"));
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < t.log.size(); i++) {
            const auto& e = t.log[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char l[48];
            snprintf(l, sizeof l, "%s##sar%zu", clockText(e.wallTime).c_str(), i);
            if (ImGui::Selectable(l, S.sel == (int)i, ImGuiSelectableFlags_SpanAllColumns)) S.sel = (int)i;
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(e.b.hexId.c_str()); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::TextColored(typeColour(e.b), "%s", typeName(e.b));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.b.country.empty() ? "-" : e.b.country.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(posText(e.b).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.b.bch2Present && !e.b.bch2Ok ? "1" : (e.b.bch2Present ? "1 + 2" : "1"));
            ImGui::TableNextColumn(); ImGui::Text("%.0f dB", e.snrDb);
        }
        ImGui::EndTable();
    }
    ImGui::BeginChild("##sardet", ImVec2(0, 0), ImGuiChildFlags_Borders);
    details(a, t.log[(size_t)S.sel]);
    ImGui::EndChild();
}

struct Marker { double lat, lon; ImU32 col; size_t idx; std::string label; };

void mapView(App& a, ImVec2 size) {
    const dect2::SarsatTelemetry& t = a.rx.sarsat;
    std::vector<Marker> mk;
    std::set<std::string> seen;
    for (size_t i = 0; i < t.log.size(); i++) {   // newest first: one marker per beacon
        const auto& e = t.log[i];
        if (!e.b.hasPos || !seen.insert(e.b.hexId).second) continue;
        mk.push_back({e.b.lat, e.b.lon, ImGui::ColorConvertFloat4ToU32(typeColour(e.b)), i, e.b.hexId});
    }
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (S.mapFollow && !mk.empty()) {
        double la0 = 90, la1 = -90, lo0 = 180, lo1 = -180;
        for (const auto& m : mk) { la0 = std::min(la0, m.lat); la1 = std::max(la1, m.lat); lo0 = std::min(lo0, m.lon); lo1 = std::max(lo1, m.lon); }
        S.map.lat = (la0 + la1) / 2; S.map.lon = (lo0 + lo1) / 2;
        const double span = std::max(8.0, std::max(lo1 - lo0, (la1 - la0) * 1.4));
        S.map.zoom = std::max(2, std::min(9, (int)std::floor(std::log2(0.7 * size.x * 360.0 / (256.0 * span)))));
    }
    const double lat0 = S.map.lat, lon0 = S.map.lon;
    adsbmap::draw(S.map, size);
    if (S.map.lat != lat0 || S.map.lon != lon0) S.mapFollow = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    ImVec2 click; const bool clicked = adsbmap::clickedAt(click);
    float best = 18 * gUi;
    for (const auto& m : mk) {
        const ImVec2 q = adsbmap::project(m.lat, m.lon);
        if (q.x < p0.x - 40 || q.y < p0.y - 40 || q.x > p0.x + size.x + 40 || q.y > p0.y + size.y + 40) continue;
        const float r = 8.f * gUi;
        const bool cur = (int)m.idx == S.sel;
        dl->AddCircleFilled(q, r, (m.col & 0x00FFFFFF) | ((cur ? 170u : 80u) << 24));
        dl->AddCircle(q, r, m.col, 24, cur ? 2.5f : 1.f);
        dl->AddText(ImVec2(q.x + r + 2 * gUi, q.y - ImGui::GetTextLineHeight() * 0.5f), IM_COL32(235, 238, 240, 255), m.label.c_str());
        const float d = std::hypot(q.x - click.x, q.y - click.y);
        if (clicked && d < best) { best = d; S.sel = (int)m.idx; }
    }
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(26 * gUi, 0))) { S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1); S.mapFollow = false; } if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom in"));
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(26 * gUi, 0))) { S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1); S.mapFollow = false; } if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom out"));
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button(tr("fit"))) S.mapFollow = true;
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never the position itself");
    plat::prefs().setI("sarsatZoom", S.map.zoom);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
    if (mk.empty()) adsbmap::legend(size.x, "%s", tr("no decoded beacon carries a position yet"));
    else adsbmap::legend(size.x, "%s", tr("blue EPIRB, orange ELT, green PLB, grey test; a short message is rounded to 15 minutes of arc"));
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
    ImGui::Dummy(ImVec2(0, 0));
}

void rawView(App& a) {
    const dect2::SarsatTelemetry& t = a.rx.sarsat;
    if (ImGui::SmallButton("copy all##sarraw")) {
        std::string all;
        for (auto it = t.log.rbegin(); it != t.log.rend(); ++it) all += clockText(it->wallTime) + "  " + it->b.rawHex + "\n";
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::BeginChild("##sar_raw", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::PushFont(a.mono, 0);
    for (const auto& e : t.log) {
        ImGui::TextDisabled("%s", clockText(e.wallTime).c_str());
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(e.b.rawHex.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::PopFont();
    if (t.log.empty()) ImGui::TextDisabled("%s", tr("nothing decoded yet"));
    ImGui::EndChild();
}

void tab(App& a) {
    load();
    helpButton();
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Clear"))) a.engine.sarsat().clearResults();
    if (live(a)) { ImGui::SameLine(); ImGui::TextDisabled("%s", dect2::sarsatSummary(a.rx.sarsat).c_str()); }
    const int v0 = S.view;
    subNav("sarv", S.view, {tr("Beacons"), tr("Map"), tr("Raw bits")});
    if (S.view != v0) plat::prefs().setI("sarsatView", S.view);
    if (!live(a)) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("tune to 406.028 MHz and start the receiver"));
        ImGui::PopTextWrapPos();
        return;
    }
    if (S.view == 1) mapView(a, ImVec2(ImGui::GetContentRegionAvail().x, std::max(120.f * gUi, ImGui::GetContentRegionAvail().y - 2 * ImGui::GetTextLineHeight())));
    else if (S.view == 2) rawView(a);
    else beaconsView(a);
    if (S.view != 2) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("Receive only. A real beacon means somebody may be in danger: tell the rescue centre; do not rely on this program for safety."));
        ImGui::PopTextWrapPos();
    }
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::SarsatTelemetry& t = a.rx.sarsat;
    kvRow(a, tr("State"), "%s", t.state == 0 ? tr("no input") : t.state == 1 ? tr("listening") : tr("beacon decoded lately"));
    kvRow(a, tr("Bursts"), "%llu %s, %llu %s", (unsigned long long)t.bursts, tr("heard"), (unsigned long long)t.blocksBad, tr("failed the checks"));
    kvRow(a, tr("Decoded"), "%llu", (unsigned long long)t.blocksOk);
    kvRow(a, tr("Last burst"), "%.1f dB, %+.0f Hz", t.snrDb, t.cfoHz);
    kvRow(a, tr("Input"), "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
}

void list(App& a) {
    if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to see beacons")); ImGui::PopTextWrapPos(); return; }
    const dect2::SarsatTelemetry& t = a.rx.sarsat;
    struct Row { const dect2::SarsatEntry* e; int n; size_t idx; };
    std::vector<Row> rows;
    for (size_t i = 0; i < t.log.size(); i++) {
        bool found = false;
        for (Row& r : rows) if (r.e->b.hexId == t.log[i].b.hexId) { r.n++; found = true; break; }
        if (!found) rows.push_back({&t.log[i], 1, i});
    }
    ImGui::TextDisabled("%zu", rows.size());
    if (!ImGui::BeginTable("##sar_l", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Hex ID"), ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn(tr("Type")); ImGui::TableSetupColumn("n");
    ImGui::TableHeadersRow();
    for (const Row& r : rows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID((int)r.idx);
        if (ImGui::Selectable("##r", (int)r.idx == S.sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) S.sel = (int)r.idx;
        ImGui::PopID();
        ImGui::SameLine(0, 0);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(r.e->b.hexId.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextColored(typeColour(r.e->b), "%s", typeName(r.e->b));
        ImGui::TableNextColumn(); ImGui::Text("%d", r.n);
    }
    ImGui::EndTable();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const dect2::SarsatTelemetry& t = a.rx.sarsat;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Carrier"), !on ? 0 : t.bursts > 0 ? 1 : 0); flowNext(10 * gUi);
    lamp(tr("Beacon"), !on ? 0 : t.state >= 2 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(a.mono, 0);
    if (on) ImGui::Text("%llu", (unsigned long long)t.blocksOk); else ImGui::TextUnformatted(run ? tr("starting") : tr("stopped"));
    ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "COSPAS-SARSAT 406 MHz";
    if (live(a)) l2 = dect2::sarsatSummary(a.rx.sarsat);
}

void synth(App&, bool&) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("The test signal repeats five made-up beacons every 3 seconds (real ones send every 50 seconds): an EPIRB, a PLB, an ELT, a maritime user beacon and an EPIRB in the southern hemisphere."));
    ImGui::PopTextWrapPos();
}

// the whole-capture search: on by itself on the L-band downlink (each satellite somewhere in 1544 - 1545 MHz), or by hand
bool gWideSet = false, gWideLoaded = false;
bool wideWanted(const App& a) { return gWideSet || (a.freqMhz > 1543.5 && a.freqMhz < 1546.0); }

void tick(App& a) {
    if (!gWideLoaded) { gWideLoaded = true; gWideSet = plat::prefs().getB("sarsatWide", false); }
    if (a.engine.running()) a.engine.sarsat().setWideSearch(wideWanted(a));
}

// issue #214: the satellites repeat the 406.0 - 406.1 MHz band on their L-band downlink (each one at its own place in 1544 - 1545 MHz)
void tuner(App& a, bool& retune) {
    struct P { const char* label; double mhz; };
    static const P kP[] = {{"406.028 MHz (beacons)", sarsat::kDefaultMhz}, {"1544.500 MHz (satellite downlink)", 1544.5}};
    ImGui::TextDisabled("%s", tr("Frequency"));
    sameLineIf(90 * gUi, 5 * gUi);
    char cur[32]; snprintf(cur, sizeof cur, "%.3f MHz", a.freqMhz);
    ImGui::SetNextItemWidth(std::min(220 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##sarf", cur)) {
        for (const P& p : kP) if (ImGui::Selectable(p.label, std::fabs(a.freqMhz - p.mhz) < 0.0005)) { tuneFreq(a, p.mhz); retune = true; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Beacons send on 406.0 - 406.1 MHz. The search and rescue satellites repeat that band on their downlink, each one somewhere in 1544 - 1545 MHz (an L-band antenna): tune near a satellite's carrier there."));
    flowNext(8 * gUi);
    char lb[96]; snprintf(lb, sizeof lb, "%s##sarwide", tr("Search the whole capture"));
    bool w = wideWanted(a);
    const bool automatic = !gWideSet && w;
    if (ImGui::Checkbox(lb, &w)) { gWideSet = w; plat::prefs().setB("sarsatWide", w); }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Look for beacon bursts across the whole sample rate (up to 1.1 MHz) instead of 100 kHz around the dial. On by itself on the 1544 - 1545 MHz downlink. Set the radio to 2 Msps to cover the whole downlink band."));
    if (automatic) { flowNext(4 * gUi); ImGui::TextDisabled("%s", tr("(on by itself for the downlink)")); }
    if (live(a) && a.rx.sarsat.wideSpanHz > 0) { flowNext(6 * gUi); ImGui::TextDisabled(tr("%.0f kHz searched"), a.rx.sarsat.wideSpanHz / 1e3); }
    flowEnd();
}

} // namespace

extern const ModeUi kSarsatUi;
const ModeUi kSarsatUi = {
    .sideTitle = "BEACONS",
    .tabName = "Beacons",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .synth = synth,
    .tick = tick,
};

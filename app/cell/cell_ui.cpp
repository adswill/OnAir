// Cell towers screens: the cell table (operator, band, EARFCN, PCI, eNB / sector, TAC, bandwidth, RSRP / RSRQ, last seen), cells grouped
// by site, the neighbour lists of SIB4 / SIB5, the strongest cell per operator, a band scan over the LTE EARFCN raster, and an optional
// map of the towers: looked up on OpenCelliD with the user's own API key (only the cell identity and the key are sent; cached on disk), or
// from a tower position file the user downloads (OpenCelliD CSV: radio,mcc,net,area,cell,unit,lon,lat,...). Data (c) OpenCelliD, CC BY-SA
// 4.0; nothing is bundled. Only what cells broadcast to every phone is shown.
#include <set>
#include "app.h"
#include "state/results_export.h"
#include "../adsb/adsb_map.h"
#include "dect2/cell/cell_ocid.h"
#include "dect2/cell/cell_rx.h"
#include "dect2/cell/cell_filter.h"
#include "dect2/cell/cell_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int kNb = 21;
const int kScanBands[kNb] = {20, 8, 3, 1, 7, 28, 32, 38, 40, 41, 42, 43, 2, 4, 5, 12, 13, 25, 66, 71, 31};

struct Pos { double lat, lon; };
struct State {
    int view = adsbmap::devView(0);   // 0 cells, 1 sites, 2 neighbours, 3 operators, 4 map, 5 log
    int sel = -1;                 // selected cell (index in cells)
    bool bands[kNb] = {true, true, true, true, true};   // B20, B8, B3, B1, B7
    std::set<int> extraBands;     // other table bands picked in the "Other bands" popup
    bool custom = false;          // a custom range instead of (with) the bands
    float customLo = 790, customHi = 830;   // MHz
    bool scanning = false;
    std::vector<double> plan;     // centres, MHz
    size_t step = 0;
    double stepAt = 0;
    float dwell = 3.0f;
    uint64_t blocksAt = 0;        // receiver blocks done when this step was tuned
    std::map<uint64_t, Pos> towers;   // (mcc, mnc, eci) -> position
    std::string towerFile, towerNote;
    adsbmap::View map;
    bool keyLoaded = false, showKey = false, fitted = false;
    char key[128] = {};
    std::unique_ptr<dect2::OcidLookup> ocid;
    char find[48] = {};           // operator / PLMN / eNB / TAC / PCI search
    float minRsrp = -120;         // dBFS; at the bottom of the slider: no limit
    bool confirmedOnly = false, identifiedOnly = false, hideBarred = false, collapse = false;
    char hide[48] = {};           // "Hide bands": band numbers whose cells are left out of the list and the export (#164)
};
State S;

// the band numbers typed in the hide box ("14 28, 64")
std::vector<int> hiddenBands() {
    std::vector<int> v;
    for (const char* p = S.hide; *p; ) {
        if (*p >= '0' && *p <= '9') { char* e; v.push_back((int)strtol(p, &e, 10)); p = e; } else p++;
    }
    return v;
}
// the list filters of the "Filter" row: hidden bands, text, level, band confirmed by SIB1, identified, barred, duplicates (core/include/dect2/cell/cell_filter.h)
dect2::CellFilter filterNow() {
    dect2::CellFilter f;
    f.hideBands = hiddenBands();
    f.text = S.find;
    f.minRsrpDb = S.minRsrp <= -120 ? -200.f : S.minRsrp;
    f.confirmedBandOnly = S.confirmedOnly; f.identifiedOnly = S.identifiedOnly; f.hideBarred = S.hideBarred; f.collapseDuplicates = S.collapse;
    return f;
}
// the cells to list for this frame (indices into the telemetry)
std::vector<size_t>& shown(const dect2::CellTelemetry& t) {
    static std::vector<size_t> v;
    v = dect2::filterCells(t.cells, filterNow());
    return v;
}
// "B66 (also B4, B25)"; a band taken from the frequency alone is "B1? (or B4, B10)" until SIB1 has been read; "B106 (MFBI B8)" when SIB1 lists them
std::string bandText(const dect2::CellTower& c) {
    if (!c.band) return "?";
    std::string o = "B" + std::to_string(c.band) + (c.bandConfirmed ? "" : "?");
    if (!c.bandsAlso.empty()) {
        o += c.mfbi ? " (MFBI " : c.bandConfirmed ? " (also " : " (or ";
        for (size_t i = 0; i < c.bandsAlso.size() && i < 4; i++) o += (i ? ", B" : "B") + std::to_string(c.bandsAlso[i]);
        if (c.bandsAlso.size() > 4) o += "...";
        o += ")";
    }
    return o;
}

bool live(const App& a) { return a.rx.standard == 33; }

std::string ago(int64_t t) {
    if (t <= 0) return "-";
    const long d = (long)(std::time(nullptr) - t);
    char b[32];
    if (d < 60) snprintf(b, sizeof b, "%ld s", std::max(0L, d)); else if (d < 3600) snprintf(b, sizeof b, "%ld min", d / 60); else snprintf(b, sizeof b, "%ld h", d / 3600);
    return b;
}

uint64_t towerKey(int mcc, int mnc, uint32_t eci) { return ((uint64_t)mcc << 44) | ((uint64_t)mnc << 32) | eci; }

void loadTowers(const std::string& path) {
    std::ifstream f(path);
    if (!f) { S.towerNote = "cannot open the file"; return; }
    S.towers.clear();
    std::string line;
    size_t n = 0;
    while (std::getline(f, line)) {
        std::stringstream ss(line);
        std::vector<std::string> c;
        std::string v;
        while (std::getline(ss, v, ',')) c.push_back(v);
        if (c.size() < 8 || c[0] != "LTE") continue;
        const int mcc = atoi(c[1].c_str()), mnc = atoi(c[2].c_str());
        const uint32_t eci = (uint32_t)strtoul(c[4].c_str(), nullptr, 10);
        S.towers[towerKey(mcc, mnc, eci)] = {atof(c[7].c_str()), atof(c[6].c_str())};
        n++;
    }
    char b[96]; snprintf(b, sizeof b, "%zu LTE cells loaded", n);
    S.towerNote = b; S.towerFile = path;
}

dect2::OcidLookup& ocid() {
    if (!S.ocid) {
        const std::string tmp = plat::cacheDir() + "/ocid-answer.json";
        S.ocid = std::make_unique<dect2::OcidLookup>([tmp](const std::string& url, std::string& body) {
            std::remove(tmp.c_str());
            if (!plat::fetchUrl(url, tmp, "OnAir/" ONAIR_VERSION)) return false;
            std::ifstream f(tmp); std::stringstream ss; ss << f.rdbuf(); body = ss.str();
            std::remove(tmp.c_str());
            return true;
        }, plat::dataDir() + "/opencellid-cache.csv");
    }
    if (!S.keyLoaded) { S.keyLoaded = true; snprintf(S.key, sizeof S.key, "%s", plat::prefs().getS("ocidKey", "").c_str()); S.ocid->setKey(S.key); }
    return *S.ocid;
}

dect2::OcidCellKey ocidKey(const dect2::CellTower& c) {
    dect2::OcidCellKey k; k.mcc = c.mcc; k.mnc = c.mnc; k.lac = c.tac; k.cell = c.cellId; k.radio = "LTE";
    return k;
}

enum { kLocNone, kLocDb, kLocCsv };
struct Loc { Pos p{0, 0}; int range = 0, samples = 0, src = kLocNone; };

// the database's answer first, else the loaded CSV
Loc towerOf(const dect2::CellTower& c) {
    Loc l;
    if (!c.hasSib1 || c.mcc < 0) return l;
    dect2::OcidEntry e;
    if (ocid().lookup(ocidKey(c), e) && e.r.status == dect2::OcidStatus::Found) { l.p = {e.r.lat, e.r.lon}; l.range = e.r.range; l.samples = e.r.samples; l.src = kLocDb; return l; }
    auto it = S.towers.find(towerKey(c.mcc, c.mnc, c.cellId));
    if (it != S.towers.end()) { l.p = it->second; l.src = kLocCsv; }
    return l;
}

const char* locText(int src) { return src == kLocDb ? "database" : src == kLocCsv ? "CSV" : "unknown"; }

ImU32 opColour(const dect2::CellTower& c, int alpha) {   // one colour per operator (PLMN)
    const std::string k = c.plmns.empty() ? "?" : c.plmns[0];
    uint32_t h = 2166136261u; for (char ch : k) h = (h ^ (uint8_t)ch) * 16777619u;
    float r, g, b; ImGui::ColorConvertHSVtoRGB((h % 360) / 360.0f, 0.65f, 0.95f, r, g, b);
    return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), alpha);
}

std::string opText(const dect2::CellTower& c) {
    if (!c.hasSib1 || c.plmns.empty()) return "-";
    std::string s = c.op.empty() ? c.plmns[0] : c.plmns[0] + " " + c.op;
    if (c.plmns.size() > 1) s += " +" + std::to_string(c.plmns.size() - 1);   // shared network: the other PLMNs are in the detail line
    return s;
}

void makePlan(const App& a) {
    S.plan.clear();
    const double span = std::max(1.5, a.tune.sampleRate / 1e6 * 0.75);   // the part of the sample band searched whole
    // centres on the 100 kHz channel raster, one sample band apart
    auto range = [&](double lo, double hi) { for (double c = lo + span / 2; c - span / 2 < hi; c += span) S.plan.push_back(std::round(c * 10) / 10); };
    for (int i = 0; i < kNb; i++) {
        if (!S.bands[i]) continue;
        if (const dect2::LteBand* b = dect2::lteBandInfo(kScanBands[i])) range(b->dlLowMhz, b->dlHighMhz);
    }
    for (int nb : S.extraBands) if (const dect2::LteBand* b = dect2::lteBandInfo(nb)) range(b->dlLowMhz, b->dlHighMhz);
    if (S.custom && S.customHi > S.customLo) range(S.customLo, S.customHi);
}

void scanBar(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Bands");
    for (int i = 0; i < kNb; i++) {
        flowNext(6 * gUi);
        char l[16]; snprintf(l, sizeof l, "B%d##cb%d", kScanBands[i], i);
        ImGui::Checkbox(l, &S.bands[i]);
        if (ImGui::IsItemHovered()) if (const dect2::LteBand* b = dect2::lteBandInfo(kScanBands[i])) ImGui::SetTooltip("Band %d: %.0f - %.0f MHz (%s)", b->band, b->dlLowMhz, b->dlHighMhz, b->region);
    }
    flowNext(10 * gUi);
    {   // every other band of the table (36.101 5.5-1), in a popup; the 5 GHz bands (46, 47, 49) are beyond the radios
        char l[32]; snprintf(l, sizeof l, S.extraBands.empty() ? "Other bands..." : "Other bands (%d)...", (int)S.extraBands.size());
        if (ImGui::SmallButton(l)) ImGui::OpenPopup("lteother");
        if (ImGui::BeginPopup("lteother")) {
            int col = 0;
            for (const auto& b : dect2::lteBands()) {
                if (b.band == 46 || b.band == 47 || b.band == 49) continue;
                bool listed = false; for (int k : kScanBands) if (k == b.band) listed = true;
                if (listed) continue;
                bool on = S.extraBands.count(b.band) > 0;
                char bl[48]; snprintf(bl, sizeof bl, "B%d %s%s##ob%d", b.band, b.tdd ? "TDD " : "", b.region, b.band);
                if (col++ % 3) ImGui::SameLine(190 * gUi * (float)((col - 1) % 3));
                if (ImGui::Checkbox(bl, &on)) { if (on) S.extraBands.insert(b.band); else S.extraBands.erase(b.band); }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Band %d: %.1f - %.1f MHz downlink", b.band, b.dlLowMhz, b.dlHighMhz);
            }
            ImGui::EndPopup();
        }
    }
    flowNext(10 * gUi);
    ImGui::Checkbox("Custom##cbc", &S.custom);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Also scan a range of your own, start and end in MHz, on the 100 kHz LTE raster");
    if (S.custom) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi); ImGui::InputFloat("##cblo", &S.customLo, 0, 0, "%.1f");
        ImGui::SameLine(); ImGui::TextUnformatted("-"); ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi); ImGui::InputFloat("MHz##cbhi", &S.customHi, 0, 0, "%.1f");
        S.customLo = std::clamp(S.customLo, 1.0f, 7250.0f); S.customHi = std::clamp(S.customHi, 1.0f, 7250.0f);
        {   // the same range as EARFCNs (the channel number follows from the frequency alone, no broadcast message is needed)
            const int e0 = dect2::lteNearestEarfcn(S.customLo * 1e6), e1 = dect2::lteNearestEarfcn(S.customHi * 1e6);
            ImGui::SameLine();
            if (e0 >= 0 && e1 >= 0) ImGui::TextDisabled("EARFCN %d - %d", e0, e1); else ImGui::TextDisabled("EARFCN: outside the LTE bands");
        }
    }
    flowNext(10 * gUi);
    if (!S.scanning) {
        if (runningButton(a, tr("Scan"), true)) { makePlan(a); S.step = 0; S.stepAt = 0; S.scanning = !S.plan.empty(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Step the radio over the chosen bands; every LTE carrier in each step is searched. Receive only.");
    } else {
        if (ImGui::SmallButton("Stop scan")) S.scanning = false;
        ImGui::SameLine();
        {
            const double cf = S.step < S.plan.size() ? S.plan[S.step] * 1e6 : 0;
            const int ce = cf > 0 ? dect2::lteNearestEarfcn(cf) : -1;
            if (ce >= 0) ImGui::TextDisabled("%zu / %zu  %.1f MHz (EARFCN %d)", S.step + 1, S.plan.size(), cf / 1e6, ce);
            else ImGui::TextDisabled("%zu / %zu  %.1f MHz", S.step + 1, S.plan.size(), cf / 1e6);
        }
    }
    flowNext(10 * gUi);
    if (ImGui::SmallButton("Clear")) a.engine.cell().clearCells();
    flowNext(10 * gUi);
    ImGui::TextDisabled("Hide bands"); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi); ImGui::InputTextWithHint("##cbhide", "e.g. 14 28", S.hide, sizeof S.hide);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Band numbers whose cells are left out of the list and the export (a cell is shown in the first band that holds its frequency\nuntil its SIB1 is read; an unwanted band can be hidden here).");
    flowNext(10 * gUi);
    ImGui::TextDisabled("Search"); ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi); ImGui::InputTextWithHint("##cbfind", "operator, 310-410, eNB", S.find, sizeof S.find);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lists only cells whose operator, any of their PLMNs (a cell can broadcast several, such as 310-410 and 313-100), eNB, TAC, cell id or PCI contains this text.");
    flowNext(10 * gUi);
    ImGui::TextDisabled("Min RSRP"); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi); ImGui::SliderFloat("##cbmin", &S.minRsrp, -120, -20, S.minRsrp <= -120 ? "off" : "%.0f dBFS");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Leaves out cells weaker than this (the radio's own scale, not calibrated dBm). At the left end the limit is off.");
    flowNext(10 * gUi);
    ImGui::Checkbox("Band known##cbconf", &S.confirmedOnly);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lists only cells whose band SIB1 confirmed. Until then the band is the first one that holds the frequency, which is why overlapping bands can look wrong.");
    flowNext(10 * gUi);
    ImGui::Checkbox("Identified##cbid", &S.identifiedOnly);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lists only cells whose SIB1 was read (operator, eNB, TAC known).");
    flowNext(10 * gUi);
    ImGui::Checkbox("Not barred##cbbar", &S.hideBarred);
    flowNext(10 * gUi);
    ImGui::Checkbox("One per cell##cbdup", &S.collapse);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A cell heard on two frequencies (same PLMN and cell id) is listed once, at its stronger frequency.");
    flowNext(10 * gUi);
    {
        static std::string note;
        resexp::Table t; t.cols = {"time", "frequency_hz", "mode", "operator", "band", "earfcn", "pci", "enb", "sector", "tac", "width_mhz", "rsrp_db", "rsrq_db"};
        const std::string now = resexp::stamp();
        for (const size_t ci : shown(a.rx.cell)) {
            const auto& c = a.rx.cell.cells[ci];
            t.add({now, resexp::num(c.freqHz, 0), "LTE", opText(c), c.band ? std::to_string(c.band) : "", c.earfcn >= 0 ? std::to_string(c.earfcn) : "", std::to_string(c.pci),
                   c.hasSib1 ? std::to_string(c.enb) : "", c.hasSib1 ? std::to_string(c.sector) : "", c.hasSib1 ? std::to_string(c.tac) : "",
                   c.hasMib ? resexp::num(c.nRb * 0.2, 1) : "", resexp::num(c.rsrpDb, 1), resexp::num(c.rsrqDb, 1)});
        }
        resexp::saveButton(t, "lte", note, "svcell");
        if (!note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", note.c_str()); }
    }
    flowEnd();
}

void cellTable(App& a) {
    const dect2::CellTelemetry& t = a.rx.cell;
    // a narrow window keeps the columns that identify a cell; the rest are in the detail lines below
    const bool wide = ImGui::GetContentRegionAvail().x > 980 * gUi;
    const int nc = wide ? 12 : 7;
    const ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV;
    if (!ImGui::BeginTable("##cells", nc, fl, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    if (wide) for (const char* h : {"Operator", "Band", "EARFCN", "MHz", "PCI", "eNB / sector", "TAC", "Width", "RSRP", "RSRQ", "Seen", "Location"}) ImGui::TableSetupColumn(h);
    else for (const char* h : {"Operator", "Band", "EARFCN", "PCI", "eNB / sector", "RSRP", "Location"}) ImGui::TableSetupColumn(h);
    ImGui::TableHeadersRow();
    for (const size_t i : shown(t)) {
        const auto& c = t.cells[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        char l[96]; snprintf(l, sizeof l, "%s##c%zu", wide ? opText(c).c_str() : (c.hasSib1 && !c.plmns.empty() ? c.plmns[0].c_str() : "-"), i);
        if (ImGui::Selectable(l, S.sel == (int)i, ImGuiSelectableFlags_SpanAllColumns)) S.sel = (int)i;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s, %.1f MHz, TAC %u, %.1f MHz wide, RSRQ %.1f dB, seen %s ago", opText(c).c_str(), c.freqHz / 1e6, c.tac, c.nRb * 0.2, c.rsrqDb, ago(c.lastWall).c_str());
        ImGui::TableNextColumn(); if (c.band) ImGui::TextUnformatted(bandText(c).c_str()); else ImGui::TextDisabled("?");
        if (c.freqHz > 0 && ImGui::IsItemHovered()) {   // a frequency can lie in several bands; the EARFCN follows from the band
            std::string o;
            for (int b : dect2::lteBandsOfHz(c.freqHz)) o += (o.empty() ? "" : ", ") + std::string("B") + std::to_string(b) + " EARFCN " + std::to_string(dect2::lteNearestEarfcn(c.freqHz, b));
            if (!o.empty()) ImGui::SetTooltip("%.1f MHz lies in: %s", c.freqHz / 1e6, o.c_str());
        }
        ImGui::TableNextColumn(); if (c.earfcn >= 0) ImGui::Text("%d", c.earfcn); else ImGui::TextDisabled("-");
        if (wide) { ImGui::TableNextColumn(); ImGui::Text("%.1f", c.freqHz / 1e6); }
        ImGui::TableNextColumn(); ImGui::Text("%d", c.pci);
        ImGui::TableNextColumn(); if (c.hasSib1) ImGui::Text("%u/%d", c.enb, c.sector); else ImGui::TextDisabled("-");
        if (wide) { ImGui::TableNextColumn(); if (c.hasSib1) ImGui::Text("%u", c.tac); else ImGui::TextDisabled("-"); }
        if (wide) { ImGui::TableNextColumn(); if (c.hasMib) ImGui::Text("%.1f MHz", c.nRb * 0.2); else ImGui::TextDisabled("sync only"); }
        ImGui::TableNextColumn(); ImGui::Text("%.0f", c.rsrpDb);
        if (wide) { ImGui::TableNextColumn(); ImGui::Text("%.1f", c.rsrqDb); }
        if (wide) { ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(c.lastWall).c_str()); }
        ImGui::TableNextColumn();
        { const Loc lc = towerOf(c); if (lc.src == kLocNone) ImGui::TextDisabled("unknown"); else ImGui::TextUnformatted(locText(lc.src)); }
    }
    ImGui::EndTable();
}

void detail(App& a) {
    ImGui::PushTextWrapPos(0);
    struct Pop { ~Pop() { ImGui::PopTextWrapPos(); } } pop_;
    const dect2::CellTelemetry& t = a.rx.cell;
    if (S.sel < 0 || S.sel >= (int)t.cells.size()) { ImGui::TextDisabled("select a cell"); return; }
    const auto& c = t.cells[(size_t)S.sel];
    ImGui::Text("PCI %d, %s, %d RB, %d port%s, %s CP, PHICH %s Ng %d, SFN %d", c.pci, c.tdd ? "TDD" : "FDD", c.nRb, c.ports, c.ports == 1 ? "" : "s", c.extCp ? "extended" : "normal",
                c.phichDur ? "extended" : "normal", c.phichRes, c.sfn);
    ImGui::Text("RSRP %.1f dBFS, RSRQ %.1f dB, SINR %.1f dB, offset %+.0f Hz", c.rsrpDb, c.rsrqDb, c.sinrDb, c.cfoHz);
    if (c.hasSib1) {
        std::string pl;
        for (size_t k = 0; k < c.plmns.size(); k++) {   // every PLMN the cell broadcasts, with the operator name where the table has it
            int mc = 0, mn = 0; char tail = 0;
            const char* nm = sscanf(c.plmns[k].c_str(), "%d-%d%c", &mc, &mn, &tail) == 2 ? dect2::lteOperatorName(mc, mn) : "";
            pl += c.plmns[k] + (nm && *nm ? std::string(" ") + nm : std::string()) + (k + 1 < c.plmns.size() ? ", " : " ");
        }
        ImGui::Text("SIB1: %s(%s) TAC %u, cell id %u (eNB %u, sector %d)%s", pl.c_str(), dect2::lteCountry(c.mcc), c.tac, c.cellId, c.enb, c.sector, c.barred ? ", barred" : "");
    }
    {   // SIB types 10, 11 and 12 in the scheduling list: the cell can broadcast warnings (ETWS / CMAS); their content is not decoded
        bool etws = false, cmas = false;
        for (const auto& s : c.sched) for (int b : s.sibs) { if (b == 10 || b == 11) etws = true; if (b == 12) cmas = true; }
        if (etws || cmas) ImGui::TextDisabled("%s%s%s", etws ? "scheduled: SIB10/11 (ETWS earthquake and tsunami warning)" : "", etws && cmas ? "; " : "", cmas ? "SIB12 (CMAS / public warning)" : "");
    }
    if (c.hasSib2) ImGui::Text("SIB2: reference signal %d dBm, p-b %d, UL %s", c.refSignalPowerDbm, c.pb, c.ulEarfcn >= 0 ? std::to_string(c.ulEarfcn).c_str() : "default spacing");
    if (c.hasSib3) ImGui::Text("SIB3: priority %d, hysteresis %d dB", c.prio, c.qHystDb);
}

void sitesView(App& a) {
    const dect2::CellTelemetry& t = a.rx.cell;
    std::map<std::string, std::vector<const dect2::CellTower*>> sites;
    for (const auto& c : t.cells) if (c.hasSib1) { char k[64]; snprintf(k, sizeof k, "%s eNB %u", c.plmns.empty() ? "?" : c.plmns[0].c_str(), c.enb); sites[k].push_back(&c); }
    if (sites.empty()) { ImGui::TextDisabled("no cell identified yet (SIB1)"); return; }
    if (!ImGui::BeginChild("##sites")) { ImGui::EndChild(); return; }
    for (const auto& s : sites) {
        const auto* f = s.second[0];
        char h[128]; snprintf(h, sizeof h, "%s %s, TAC %u: %zu cell%s", s.first.c_str(), f->op.c_str(), f->tac, s.second.size(), s.second.size() == 1 ? "" : "s");
        if (ImGui::TreeNodeEx(h, ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const auto* c : s.second) ImGui::BulletText("sector %d, PCI %d, B%d EARFCN %d, %.0f dBFS", c->sector, c->pci, c->band, c->earfcn, c->rsrpDb);
            ImGui::TreePop();
        }
    }
    ImGui::EndChild();
}

void neighView(App& a) {
    const dect2::CellTelemetry& t = a.rx.cell;
    if (!ImGui::BeginChild("##nb")) { ImGui::EndChild(); return; }
    bool any = false;
    for (const auto& c : t.cells) {
        if (!c.hasSib4 && !c.hasSib5) continue;
        any = true;
        char h[96]; snprintf(h, sizeof h, "PCI %d %s##n%d", c.pci, opText(c).c_str(), c.pci);
        if (!ImGui::TreeNode(h)) continue;
        if (c.hasSib4) { std::string s; for (const auto& n : c.intra) { s += std::to_string(n.pci) + " "; } ImGui::Text("same frequency: %s", s.c_str()); }
        for (const auto& q : c.inter) {
            std::string s; for (const auto& n : q.cells) s += std::to_string(n.pci) + " ";
            ImGui::Text("EARFCN %d (B%d, %.1f MHz), priority %d %s", q.earfcn, dect2::lteBandOfEarfcn(q.earfcn), dect2::lteEarfcnToHz(q.earfcn) / 1e6, q.priority, s.c_str());
        }
        ImGui::TreePop();
    }
    if (!any) ImGui::TextDisabled("no neighbour list read yet (SIB4 / SIB5)");
    ImGui::EndChild();
}

void opView(App& a) {
    const dect2::CellTelemetry& t = a.rx.cell;
    std::map<std::string, const dect2::CellTower*> best;
    for (const auto& c : t.cells) if (c.hasSib1 && !c.plmns.empty()) { auto& b = best[c.plmns[0]]; if (!b || c.rsrpDb > b->rsrpDb) b = &c; }
    if (best.empty()) { ImGui::TextDisabled("no operator identified yet"); return; }
    for (const auto& b : best)
        ImGui::BulletText("%s %s: PCI %d, B%d EARFCN %d, eNB %u / %d, RSRP %.0f dBFS, RSRQ %.1f dB", b.first.c_str(), b.second->op.c_str(), b.second->pci, b.second->band, b.second->earfcn,
                          b.second->enb, b.second->sector, b.second->rsrpDb, b.second->rsrqDb);
}

void keyHelpButton() {
    if (ImGui::SmallButton("?##ocidhelp")) ImGui::OpenPopup("ocidhelp");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How to get an OpenCelliD API key");
    if (ImGui::BeginPopup("ocidhelp")) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        ImGui::TextUnformatted("OpenCelliD API key");
        ImGui::Separator();
        ImGui::TextUnformatted("Cell towers broadcast who they are (operator, area code, cell id) but not where they stand. OpenCelliD is an open, "
                               "crowd-sourced database of tower positions; OnAir asks it where each decoded cell is. It needs a free personal API key.");
        ImGui::Spacing();
        ImGui::TextUnformatted("1. Sign up (free) at https://opencellid.org and confirm the e-mail.\n2. Log in and open your account page (My Account / API "
                               "access): your API token (pk.\xE2\x80\xA6) is shown there.\n3. Paste it into the key field here. It is saved with the other settings.");
        ImGui::Spacing();
        ImGui::TextUnformatted("Lookups need internet, at most one a second; only the cell identity (MCC, MNC, TAC, cell id) and the key are sent. Answers are "
                               "cached on disk, so a cell is asked for once and the map works offline afterwards; a cell not in the database is asked again "
                               "after a day. The free key has a daily limit. A CSV downloaded from OpenCelliD still works as an alternative (Load tower file).");
        ImGui::Spacing();
        ImGui::TextDisabled("Tower data \xC2\xA9 OpenCelliD, CC BY-SA 4.0");
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
}

void keyBar() {
    dect2::OcidLookup& L = ocid();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("OpenCelliD API key"); ImGui::SameLine();
    const float fs = ImGui::GetFontSize();
    ImGui::SetNextItemWidth(std::min(22 * fs, std::max(8 * fs, ImGui::GetContentRegionAvail().x - 7 * fs)));
    if (ImGui::InputText("##ocidkey", S.key, sizeof S.key, S.showKey ? 0 : ImGuiInputTextFlags_Password)) {
        std::string k = S.key;
        while (!k.empty() && isspace((unsigned char)k.back())) k.pop_back();
        while (!k.empty() && isspace((unsigned char)k.front())) k.erase(0, 1);
        plat::prefs().setS("ocidKey", k);
        L.setKey(k);
    }
    ImGui::SameLine(); ImGui::Checkbox("show##ocidshow", &S.showKey);
    ImGui::SameLine(); keyHelpButton();
}

void mapView(App& a) {
    keyBar();
    dect2::OcidLookup& L = ocid();
    if (ImGui::SmallButton("Load tower file")) { const std::string p = openFileDialog(); if (!p.empty()) loadTowers(p); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Instead of (or as well as) the key: a CSV of public tower positions you downloaded yourself, OpenCelliD format (opencellid.org, CC BY-SA 4.0):\nradio,mcc,net,area,cell,unit,lon,lat,...  Cells are matched by MCC, MNC and the 28-bit cell id.");
    ImGui::SameLine(); ImGui::TextDisabled("%s", S.towerNote.empty() ? "no file" : S.towerNote.c_str());
    ImGui::SameLine();
    const std::string st = L.status();
    const size_t pend = L.pending();
    ImGui::PushTextWrapPos(0);
    if (!S.key[0]) ImGui::TextDisabled("| no key: positions only from a file");
    else if (pend) ImGui::TextDisabled("| looking up %zu cell%s%s%s", pend, pend == 1 ? "" : "s", st.empty() ? "" : ": ", st.c_str());
    else ImGui::TextDisabled("| %s", st.empty() ? "lookups idle" : st.c_str());
    ImGui::PopTextWrapPos();
    const ImVec2 sz = ImGui::GetContentRegionAvail();
    if (sz.y < 40) return;
    const double rLat = plat::prefs().getD("adsbLat", 0), rLon = plat::prefs().getD("adsbLon", 0);
    const bool haveRx = rLat != 0 || rLon != 0;   // the antenna position the map modes share
    if (!S.fitted) {   // centre on the first tower placed, else the receiver
        for (const auto& c : a.rx.cell.cells) { const Loc l = towerOf(c); if (l.src) { S.map.lat = l.p.lat; S.map.lon = l.p.lon; S.map.zoom = 13; S.fitted = true; break; } }
        if (!S.fitted && haveRx) { S.map.lat = rLat; S.map.lon = rLon; S.map.zoom = 12; }
    }
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    adsbmap::draw(S.map, sz);
    adsbmap::fullCorner(p0, sz);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), true);
    if (haveRx) {
        const ImVec2 q = adsbmap::project(rLat, rLon);
        dl->AddRectFilled(ImVec2(q.x - 4 * gUi, q.y - 4 * gUi), ImVec2(q.x + 4 * gUi, q.y + 4 * gUi), IM_COL32(255, 255, 255, 230));
        dl->AddText(ImVec2(q.x + 7 * gUi, q.y - 7 * gUi), IM_COL32(255, 255, 255, 230), "receiver");
    }
    int shown = 0;
    for (const auto& c : a.rx.cell.cells) {
        const Loc l = towerOf(c);
        if (!l.src) continue;
        shown++;
        const ImVec2 q = adsbmap::project(l.p.lat, l.p.lon);
        if (l.range > 0) {   // the database's range: faint
            const float rpx = std::fabs(adsbmap::project(l.p.lat + l.range / 111320.0, l.p.lon).y - q.y);
            if (rpx > 2 && rpx < 4000) { dl->AddCircleFilled(q, rpx, opColour(c, 22), 48); dl->AddCircle(q, rpx, opColour(c, 70), 48); }
        }
        dl->AddCircleFilled(q, 5 * gUi, opColour(c, 255));
        dl->AddCircle(q, 5 * gUi, IM_COL32(0, 0, 0, 200));
        char b[96]; snprintf(b, sizeof b, "%s eNB %u/%d PCI %d", c.op.empty() ? (c.plmns.empty() ? "?" : c.plmns[0].c_str()) : c.op.c_str(), c.enb, c.sector, c.pci);
        dl->AddText(ImVec2(q.x + 7 * gUi, q.y - 7 * gUi), IM_COL32(240, 240, 240, 255), b);
    }
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + sz.y - ImGui::GetTextLineHeight() - 6 * gUi));
    adsbmap::legend(sz.x, "%d tower%s placed. Tower data \xC2\xA9 OpenCelliD, CC BY-SA 4.0", shown, shown == 1 ? "" : "s");
}

void logView(App& a) {
    for (const auto& e : a.rx.cell.events) ImGui::TextUnformatted(e.text.c_str());
    if (a.rx.cell.events.empty()) ImGui::TextDisabled("nothing yet");
}

void tick(App& a) {
    if (live(a)) {   // ask the database about every identified cell (cached and queued ones are not asked again)
        dect2::OcidLookup& L = ocid();
        if (S.key[0]) for (const auto& c : a.rx.cell.cells) if (c.hasSib1 && c.mcc >= 0) L.request(ocidKey(c));
    }
    if (!S.scanning) return;
    if (!a.engine.running()) { S.scanning = false; return; }
    const double now = ImGui::GetTime();
    // a step ends once the dwell is over and the receiver has finished two blocks at this centre (searching a 20 Msps block for every
    // carrier takes longer than the dwell on a slow machine; a block cut off by the retune is thrown away), or after 30 s
    const dect2::CellTelemetry& t = a.rx.cell;
    const bool done = t.blocks >= S.blocksAt + 2;   // blocks of an older centre are dropped uncounted
    if (S.stepAt == 0 || (now - S.stepAt >= S.dwell && done) || now - S.stepAt >= 30) {
        if (S.stepAt != 0) S.step++;
        if (S.step >= S.plan.size()) { S.scanning = false; return; }
        tuneFreq(a, S.plan[S.step]);
        S.stepAt = now;
        S.blocksAt = t.blocks;
    }
}

void tab(App& a) {
    scanBar(a);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Receive only: the public information every LTE cell broadcasts to all phones (operator, band, cell identity). Nothing addressed to a phone is decoded.");
    ImGui::PopTextWrapPos();
    subNav("cellv", S.view, {"Cells", "Sites", "Neighbours", "Operators", "Map", "Log"});
    if (S.view == 4) { mapView(a); return; }
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to find the cells around you"); return; }
    if (S.view == 0) {
        const float h = ImGui::GetContentRegionAvail().y;
        if (ImGui::BeginChild("##ct", ImVec2(0, h * 0.7f))) cellTable(a);
        ImGui::EndChild();
        detail(a);
    } else if (S.view == 1) sitesView(a);
    else if (S.view == 2) neighView(a);
    else if (S.view == 3) opView(a);
    else logView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    for (const auto& c : a.rx.cell.cells) ImGui::Text("%-14s PCI %3d %.0f", opText(c).c_str(), c.pci, c.rsrpDb);
    if (a.rx.cell.cells.empty()) ImGui::TextDisabled("none yet");
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::CellTelemetry& t = a.rx.cell;
    ImGui::Text("%s", dect2::cellSummary(t).c_str());
    ImGui::Text("%llu blocks searched, %llu SI messages, %llu failed", (unsigned long long)t.blocks, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    ImGui::Text("input %.2f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    for (const auto& c : t.carriers) ImGui::BulletText("carrier %.2f MHz, %.1f MHz wide, %.0f dB", c.freqHz / 1e6, c.widthHz / 1e6, c.levelDb);
}

void status(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine();
    ImGui::TextUnformatted(live(a) ? dect2::cellSummary(a.rx.cell).c_str() : (a.engine.running() ? "starting" : "stopped"));
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Cell towers";
    if (live(a)) l2 = dect2::cellSummary(a.rx.cell);
}

} // namespace

extern const ModeUi kCellUi;
const ModeUi kCellUi = {
    .sideTitle = "CELLS",
    .tabName = "Cells",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};

void cellMapTowers(const dect2::CellTelemetry& t, std::vector<MapCellTower>& out) {
    for (const auto& c : t.cells) {
        const Loc l = towerOf(c);
        if (!l.src) continue;
        char b[96]; snprintf(b, sizeof b, "%s eNB %u/%d PCI %d", c.op.empty() ? (c.plmns.empty() ? "?" : c.plmns[0].c_str()) : c.op.c_str(), c.enb, c.sector, c.pci);
        out.push_back({l.p.lat, l.p.lon, l.range, b, opColour(c, 255)});
    }
}

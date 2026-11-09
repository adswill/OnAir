// Marine screens: the NAVTEX messages grouped by station with the text panel, the DSC calls (distress in red, the position on a small
// map), the weather fax picture (zoom, pan, slant, save as PNG) and a tuning aid (the audio spectrum with the tones each service expects).
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "../ui/typography.h"
#include "panels/gallery.h"
#include "panels/enhance_ui.h"
#include "dect2/results/results_io.h"
#include "../adsb/adsb_map.h"
#include "dect2/marine/marine_tel.h"
#include "dect2/marine/marine_navtex_save.h"
#include "dect2/marine/marine_mmsi.h"
#include "dect2/marine/marine_bands.h"
#include "dect2/marine/marine_dsc_geo.h"
#include "dect2/marinevhf/marinevhf_channels.h"
#include "platform/platform.h"
#include <fstream>
#include <map>
#include <sstream>
#include "dect2/marine/marine_wefax_stations.h"
#include "dect2/marine/wefax_rotate.h"
#include "wefax_gallery.h"
#include "incident_ui.h"
#include "../panels/picsave_ui.h"
#include "../state/picsave.h"
#include "dect2/engine/updater.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <ctime>
#include "dect2/ease/navtex.h"
#include "dect2/geo/maidenhead.h"

void marineFaxTexture();   // below: the texture of a picture loaded from the results file

namespace {

using dect2::DscCall;
using dect2::NavtexMessage;

struct State {
    bool loaded = false;
    int view = 0;                    // 0 NAVTEX, 1 DSC, 2 fax, 3 tuning, 4 fax stations
    int region = 0;                  // fax stations: the region shown (0 all)
    bool follow = true;              // show the service the receiver hears until the user picks a view
    int lastActive = -1;
    int service = 0;                 // 0 auto, 1 NAVTEX, 2 DSC, 3 fax
    int lpmIdx = 0, iocIdx = 0;      // 0 = detect
    struct Fax { int64_t wall = 0; double khz = 0; dect2::FaxImage im; };
    std::vector<Fax> faxes;          // the pictures received this session, newest first (the right-hand list; at most 30)
    int prevFaxState = 0;
    long long faxStartUtc = 0;       // when the picture under way began (for the autosave)
    std::string listMsg;
    bool listen = false; int pushedListen = -1; float pushedVol = -1; int pushedMute = -1;   // weather fax: "Listen"
    float slant = 0;
    bool autoSlant = true;
    int pushedService = -1, pushedLpm = -1, pushedIoc = -1;
    double pushedFreq = -1;
    float pushedSlant = 1e9f;
    int pushedAuto = -1;
    std::string navSel;              // header + time of the selected NAVTEX message
    std::string dscSelKey;
    char dscFilter[64] = "";
    bool namesLoaded = false;             // the ship table (dect2::marine::ShipDb::global()) is read from <data folder>/ship_names.tsv
    double namesSaved = 0;
    std::string dbMsg;
    dect2::marine::DscScan scan;
    bool scanOn = false;
    int scanDwell = 4, scanGroups = 1;      // seconds; bit set of DscGroup choices (see kScanSets)
    bool parOn = false;
    double scanLastRetune = 0;
    std::vector<dect2::marine::DscFix> fixes;   // every DSC position heard, for the map and the exports
    uint64_t fixSeen = 0;
    std::string mapSel;
    bool bandOnly = false;
    // list import dialog
    std::string impText, impName;
    dect2::marine::ImportPreview impPv;
    bool impOpen = true;
    // DSC map: who was heard when, the filters
    struct Heard { int64_t last = 0; int count = 0; bool distress = false; };
    std::map<std::string, Heard> heard;
    struct Link { std::string from, to; int64_t time = 0; bool distress = false; };
    std::vector<Link> links;
    uint64_t heardSeen = ~0ull;
    int mapTypes = 0x1F;                // bits: distress, coast, ship, group, SAR / other
    bool mapHeardOnly = false, mapTrails = true, mapLinks = true, mapLabels = true;
    char mapFilter[48] = "";
    std::string dscMsg;
    std::string mapKey;              // the call the small map was last centred on
    adsbmap::View map;
    // the fax picture
    gfx::Image* img = nullptr;
    int texW = 0, texH = 0, imgW = 0, imgH = 0;
    uint64_t imgSeq = 0, telImgSeq = ~0ull;
    dect2::FaxImage last;            // the copy kept for saving
    enhui::LiveTex enh;              // the Enhance control on the fax picture
    int rot = 0;                     // fax picture: quarter turns clockwise (0..3), saved as faxRotate; applies to the shown and the saved pictures
    bool faxShown = false;           // the Fax view was drawn this frame (the top "clear" button also clears the picture then)
    float zoom = 0;                  // 0 = fit the width
    ImVec2 pan{0, 0};
    std::string saveMsg;
    bool saveNav = true;             // write each NAVTEX message to a text file when it has been received
    dect2::NavtexSaver saver;
    std::string saveErr;             // the first write error, shown once until the user changes the setting
};
State S;

const int kLpm[] = {0, 60, 90, 120, 240};
const int kIoc[] = {0, 576, 288};

bool live(const App& a) { return a.rx.standard == 16; }   // the engine reports its standard code minus one

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    S.service = std::max(0, std::min(3, (int)d.getI("marineService", 0)));
    S.lpmIdx = std::max(0, std::min(4, (int)d.getI("marineLpm", 0)));
    S.listen = d.getB("faxListen", false);
    S.iocIdx = std::max(0, std::min(2, (int)d.getI("marineIoc", 0)));
    S.autoSlant = d.getB("marineAutoSlant", true);
    S.rot = dect2::wefaxTurns((int)d.getI("faxRotate", 0));
    S.slant = (float)d.getD("marineSlant", 0.0);
    S.saveNav = d.getB("marineSaveNavtex", true);
    S.map.zoom = 5;
    S.scanGroups = (int)d.getI("marineScanSets", 1);
    S.scanDwell = std::max(1, std::min(30, (int)d.getI("marineScanDwell", 4)));
}

std::string utcText(int64_t t) {
    return dect2::timefmt::clock(t);
}

std::string navKey(const NavtexMessage& m) { return m.header + "|" + std::to_string(m.rxTime) + "|" + std::to_string((long long)(m.rxSec * 10)); }
std::string dscKey(const DscCall& c) { return c.fromMmsi + "|" + std::to_string((long long)(c.rxSec * 10)); }

// B2 subject colours: warnings warm, forecasts and ice cool, search and rescue red
ImU32 subjectColour(char s) {
    switch (s) {
    case 'A': return IM_COL32(230, 160, 60, 255);    // navigational warning
    case 'B': return IM_COL32(235, 110, 80, 255);    // meteorological warning
    case 'C': return IM_COL32(120, 190, 235, 255);   // ice report
    case 'D': return IM_COL32(235, 70, 70, 255);     // search and rescue
    case 'E': return IM_COL32(100, 175, 235, 255);   // forecast
    case 'L': return IM_COL32(230, 160, 60, 255);    // more navigational warnings
    case 'Z': return IM_COL32(150, 154, 158, 255);   // QRU, nothing to send
    default: return IM_COL32(130, 200, 150, 255);
    }
}

void subjectBadge(char s) {
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + h, p.y + h), subjectColour(s), 3.f);
    const char t[2] = {s, 0};
    const ImVec2 ts = ImGui::CalcTextSize(t);
    dl->AddText(ImVec2(p.x + (h - ts.x) * 0.5f, p.y), IM_COL32(10, 12, 14, 255), t);
    ImGui::Dummy(ImVec2(h, h));
}

// ---------------------------------------------------------------- NAVTEX

// "in 37 min", "in 2 h 5 min" (a station's slot length is 10 minutes, so "now" covers the first of them)
std::string navSlotIn(int secs) {
    char b[48];
    if (secs < 60) snprintf(b, sizeof b, "%s", tr("about to start"));
    else if (secs < 3600) snprintf(b, sizeof b, tr("in %d min"), secs / 60);
    else snprintf(b, sizeof b, tr("in %d h %d min"), secs / 3600, (secs % 3600) / 60);
    return b;
}

int navKhz(const App& a) { const int k = (int)std::lround(a.freqMhz * 1000.0); return k == 490 ? 490 : 518; }

// the stations you can expect: nearest first, with the next broadcast of each
void navStationList(const App& a) {
    if (!receiverLocationSet()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("Each station sends for ten minutes every four hours. Set your position (View > Map) to see the nearest ones and when they send."));
        ImGui::PopTextWrapPos();
        return;
    }
    double la, lo;
    receiverLocation(la, lo);
    const auto near = dect2::ease::navtexNearest(la, lo, navKhz(a), (int64_t)time(nullptr));
    if (near.empty()) { ImGui::TextDisabled("%s", tr("No station on this frequency in the list.")); return; }
    const auto* soon = &near[0];
    for (const auto& n : near) if (n.secsToNext < soon->secsToNext) soon = &n;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled(tr("Next from your nearest stations: %s (%c), %s."), soon->st->name, soon->st->letter, navSlotIn(soon->secsToNext).c_str());
    ImGui::PopTextWrapPos();
    for (const auto& n : near) {
        ImGui::TextDisabled("%c  %s, %.0f km, %s", n.st->letter, n.st->name, n.km, navSlotIn(n.secsToNext).c_str());
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Stations and times come from a community list and may be out of date. Range is usually about 400 nautical miles."));
    ImGui::PopTextWrapPos();
}

void navtexView(App& a) {
    const dect2::MarineTelemetry& t = a.rx.marine;
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float lw = std::min(380.f * gUi, W * 0.42f);
    std::vector<const NavtexMessage*> rows;
    for (const auto& m : t.navtex) rows.push_back(&m);
    const bool havePos = receiverLocationSet();
    double myLa = 0, myLo = 0;
    if (havePos) receiverLocation(myLa, myLo);
    const int navK = navKhz(a);
    auto stKm = [&](char c) { const auto* s = dect2::ease::navtexByLetter(c, navK, havePos, myLa, myLo); if (!s || !havePos) return 1e9; double km = 0, br = 0; dect2::greatCircle(myLa, myLo, s->lat, s->lon, km, br); return km; };
    std::stable_sort(rows.begin(), rows.end(), [&](const NavtexMessage* x, const NavtexMessage* y) {
        if (x->station != y->station) { const double kx = stKm(x->station), ky = stKm(y->station); return kx != ky ? kx < ky : x->station < y->station; }   // the nearest station first
        return x->rxSec > y->rxSec;      // newest first within a station
    });
    const NavtexMessage* sel = nullptr;
    for (const auto* m : rows) if (navKey(*m) == S.navSel) sel = m;
    if (!sel && !t.navtex.empty()) sel = &t.navtex.back();

    ImGui::BeginChild("##nav_l", ImVec2(lw, H), ImGuiChildFlags_Borders);
    if (rows.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", live(a) ? tr("No NAVTEX message yet.") : tr("Start the receiver.")); ImGui::PopTextWrapPos(); ImGui::Spacing(); navStationList(a); }
    else if (typo::collapsingHeader(tr("Nearest stations###navnear"))) navStationList(a);
    char cur = 0;
    bool open = true;
    for (const auto* m : rows) {
        if (m->station != cur) {
            cur = m->station;
            int n = 0;
            for (const auto* x : rows) n += x->station == cur;
            char h[128];
            const auto* known = dect2::ease::navtexByLetter(cur, navK, havePos, myLa, myLo);
            if (known) snprintf(h, sizeof h, "Station %c  %s  (%d)###st%c", cur, known->name, n, cur);
            else snprintf(h, sizeof h, "Station %c  (%d)###st%c", cur, n, cur);
            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            open = typo::collapsingHeader(h);
        }
        if (!open) continue;
        ImGui::PushID(navKey(*m).c_str());
        subjectBadge(m->subject);
        ImGui::SameLine(0, 6 * gUi);
        char b[160];
        snprintf(b, sizeof b, "%s  %s%s", m->header.c_str(), m->subjectName.c_str(), m->complete ? "" : "  (cut short)");
        if (ImGui::Selectable(b, m == sel)) S.navSel = navKey(*m);
        ImGui::SameLine(lw - 120 * gUi);
        if (m->errors) ImGui::TextColored(m->cer < 0.02f ? pal::warnAmber() : pal::badRed(), "%.1f %%", 100.f * m->cer);
        else ImGui::TextColored(pal::okGreen(), "clean");
        ImGui::SameLine(lw - 64 * gUi);
        ImGui::TextDisabled("%s", utcText(m->rxTime).c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##nav_r", ImVec2(0, H), ImGuiChildFlags_Borders);
    if (!sel) {
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("the text of the selected message shows here"); ImGui::PopTextWrapPos(); }
    } else {
        ImGui::TextColored(pal::heading(), "%s", sel->header.c_str());
        ImGui::SameLine(); ImGui::TextDisabled("%s", sel->subjectName.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("station %c, number %s, %u characters, %u unreadable (%.1f %%)%s, heard %d x, last at %s%s",
                            sel->station, sel->number >= 0 ? std::to_string(sel->number).c_str() : "?", sel->chars, sel->errors,
                            100.f * sel->cer, sel->complete ? "" : ", no NNNN", sel->repeats, utcText(sel->rxTime).c_str(), dect2::timefmt::sfx());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("copy text")) ImGui::SetClipboardText(("ZCZC " + sel->header + "\n" + sel->text + "\nNNNN").c_str());
        if (sel->textCut) { ImGui::SameLine(); ImGui::TextDisabled("older message: only the start is kept"); }
        ImGui::Separator();
        ImGui::BeginChild("##nav_txt");
        ImGui::PushFont(a.mono, 0);
        ImGui::TextWrapped("%s", sel->text.c_str());
        ImGui::PopFont();
        ImGui::EndChild();
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------- DSC

std::string lowerStr(std::string v) { for (char& ch : v) ch = (char)tolower((unsigned char)ch); return v; }

void loadNames() {
    if (S.namesLoaded) return;
    S.namesLoaded = true;
    for (const char* f : {"/mmsi_names.tsv", "/ship_names.tsv"}) {     // the older list first, the table over it
        std::ifstream in(std::filesystem::u8path(plat::dataDir() + f), std::ios::binary);
        if (!in) continue;
        std::stringstream ss; ss << in.rdbuf();
        dect2::marine::ShipDb::global().importText(ss.str());
    }
    dect2::marine::ShipDb::global().takeDirty();
}

void saveNamesSoon() {
    // learned names arrive all the time: write the file at most every 30 s
    const double now = (double)time(nullptr);
    if (now - S.namesSaved < 30) return;
    if (!dect2::marine::ShipDb::global().takeDirty()) return;
    S.namesSaved = now;
    const std::string path = plat::dataDir() + "/ship_names.tsv";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(plat::dataDir()), ec);
    std::ofstream out(std::filesystem::u8path(path), std::ios::binary);
    out << dect2::marine::ShipDb::global().exportTsv();
}

// the numbers of a call with the name from the user's table, the kind of station and the country
std::string mmsiLine(const std::string& m, const std::string& type, const std::string& country) {
    if (m.empty()) return "";
    std::string r = m;
    const std::string nm = dect2::marine::ShipDb::global().name(m);
    if (!nm.empty()) r += "  " + nm;
    if (!type.empty()) r += "  (" + type + (country.empty() ? "" : ", " + country) + ")";
    else if (!country.empty()) r += "  (" + country + ")";
    return r;
}

// "Import list..." for the ship and coast station lists (YADDNET and the like): pick a file, see how it is read (delimiter, columns, first rows), then merge it.
// Nothing is downloaded: the user fetches the file themselves.
void importUi() {
    dect2::marine::ShipDb& db = dect2::marine::ShipDb::global();
    if (ImGui::Button(tr("Import list..."))) {
        const std::string p = openFileDialog();
        if (!p.empty()) {
            std::ifstream in(std::filesystem::u8path(p), std::ios::binary);
            std::stringstream ss; ss << in.rdbuf();
            S.impText = ss.str();
            S.impName = std::filesystem::u8path(p).filename().string();
            S.impPv = dect2::marine::ShipDb::preview(S.impText, 8);
            S.impOpen = true;
            ImGui::OpenPopup("##implist");
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A ship list or coast station list as CSV / TXT / TSV (for example from yaddnet.org, saved by you): MMSI, name, call sign, country, and for coast stations latitude and longitude, or a position in degrees and minutes. Delimiter and columns are found automatically; the list is merged into the local table."));
    ImGui::SameLine();
    ImGui::TextDisabled("%zu %s", db.size(), tr("known stations"));
    if (!S.dbMsg.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", S.dbMsg.c_str()); }
    ImGui::SetNextWindowSize(ImVec2(640 * gUi, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("##implist", &S.impOpen, ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto& pv = S.impPv;
        ImGui::TextColored(pal::heading(), "%s", S.impName.c_str());
        if (!pv.ok) {
            ImGui::TextWrapped("%s", tr("No column of 9-digit MMSI numbers was found in this file."));
        } else {
            const char* dn = pv.delim == '\t' ? tr("tab") : pv.delim == ';' ? tr("semicolon") : pv.delim == '|' ? tr("bar") : pv.delim == ' ' ? tr("spaces") : tr("comma");
            ImGui::TextDisabled("%s: %s   %s   %d %s", tr("separator"), dn, pv.header ? tr("header row found") : tr("no header row"), pv.records, tr("stations"));
            if (ImGui::BeginTable("##impt", (int)std::max<size_t>(1, pv.columns.size()), ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit, ImVec2(0, 180 * gUi))) {
                for (const auto& c : pv.columns) ImGui::TableSetupColumn(c.empty() ? "-" : c.c_str());
                ImGui::TableHeadersRow();
                for (const auto& r : pv.rows) {
                    ImGui::TableNextRow();
                    for (size_t i = 0; i < pv.columns.size(); i++) { ImGui::TableNextColumn(); ImGui::TextUnformatted(i < r.size() ? r[i].c_str() : ""); }
                }
                ImGui::EndTable();
            }
        }
        ImGui::BeginDisabled(!pv.ok);
        if (ImGui::Button(tr("Import"))) {
            const auto st = db.importDetailed(S.impText);
            char mb[300];
            snprintf(mb, sizeof mb, "%d %s, %d %s, %d %s, %d %s, %d %s", st.read, tr("read"), st.added, tr("new"), st.updated, tr("updated"), st.withPos, tr("with position"), st.skipped, tr("skipped"));
            S.dbMsg = mb;
            S.impText.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"))) { S.impText.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

bool dscMatches(const DscCall& c, const std::string& needle) {
    if (needle.empty()) return true;
    std::string hay = c.text + "|" + c.kind + "|" + c.fromMmsi + "|" + c.to + "|" + c.distressMmsi + "|" + c.fromCountry + "|" + c.toCountry + "|" + c.distressCountry + "|" + c.natureName + "|" + c.telecmdText + "|" + c.posText + "|" +
                      dect2::marine::ShipDb::global().name(c.fromMmsi) + "|" + dect2::marine::ShipDb::global().name(c.to) + "|" + dect2::marine::ShipDb::global().name(c.distressMmsi);
    return lowerStr(hay).find(needle) != std::string::npos;
}

std::string csvCell(const std::string& v) {
    if (v.find_first_of(",\"\n") == std::string::npos) return v;
    std::string o = "\"";
    for (char ch : v) { if (ch == '"') o += '"'; o += ch; }
    return o + "\"";
}

std::string dscCsv(const std::vector<DscCall>& calls) {
    std::string o = "utc,band,kind,category,from,from_name,from_country,to,to_name,to_country,distress_mmsi,nature,lat,lon,position_time_utc,telecommand,freq_rx,freq_tx,eos,check,symbols_lost,symbols_repaired,symbols\n";
    for (const DscCall& c : calls) {
        char pos[48] = "", tm[16] = "";
        if (c.hasPos) snprintf(pos, sizeof pos, "%.5f,%.5f", c.lat, c.lon); else snprintf(pos, sizeof pos, ",");
        if (c.hasTime) snprintf(tm, sizeof tm, "%02d:%02d", c.utcHour, c.utcMin);
        std::string sy;
        for (size_t i = 0; i < c.symbols.size(); i++) { char q[8]; snprintf(q, sizeof q, "%s%d", i ? " " : "", c.symbols[i]); sy += q; }
        o += csvCell(utcText(c.rxTime)) + "," + (c.vhf ? "VHF" : "MF/HF") + "," + csvCell(c.kind) + "," + csvCell(c.categoryName) + "," + csvCell(c.fromMmsi) + "," + csvCell(dect2::marine::ShipDb::global().name(c.fromMmsi)) + "," + csvCell(c.fromCountry) + "," +
             csvCell(c.to) + "," + csvCell(dect2::marine::ShipDb::global().name(c.to)) + "," + csvCell(c.toCountry) + "," + csvCell(c.distressMmsi) + "," + csvCell(c.natureName) + "," + pos + "," + tm + "," + csvCell(c.telecmdText) + "," +
             csvCell(c.freqRx) + "," + csvCell(c.freqTx) + "," + std::to_string(c.eos) + "," + (c.eccOk ? "ok" : "bad") + "," + std::to_string(c.erasures) + "," + std::to_string(c.repaired) + "," + csvCell(sy) + "\n";
    }
    return o;
}

void dscDetail(App& a, const DscCall* c, ImVec2 size) {
    if (!c) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", live(a) ? "click a call" : "start the receiver"); ImGui::PopTextWrapPos(); } return; }
    auto kv = [&](const char* k, const std::string& v) {
        if (v.empty()) return;
        ImGui::TextDisabled("%s", k); kvColumn(96 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(v.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
    };
    const std::string head = c->kind.empty() ? c->formatName : c->kind;
    if (c->distress) ImGui::TextColored(pal::badRed(), "%s", head.c_str());
    else ImGui::TextColored(pal::heading(), "%s", head.c_str());
    kv(tr("category"), c->categoryName);
    kv(tr("from"), c->fromMmsi.empty() ? tr("unreadable") : mmsiLine(c->fromMmsi, c->fromType, c->fromCountry));
    kv(tr("to"), c->to.size() == 9 && c->to.find_first_not_of("0123456789~") == std::string::npos ? mmsiLine(c->to, c->toType, c->toCountry) : c->to);
    kv(tr("in distress"), mmsiLine(c->distressMmsi, "", c->distressCountry));
    kv("nature", c->natureName);
    kv("position", c->posText);
    if (c->hasTime) { char b[16]; snprintf(b, sizeof b, "%02d:%02d UTC", c->utcHour, c->utcMin); kv("time", b); }
    kv("telecommand", c->telecmdText);
    kv("frequency rx", c->freqRx);
    kv("frequency tx", c->freqTx);
    {   // a call that names a VHF working channel (or a distress alert: the channel to listen on is 16): one click to the Marine VHF mode on that channel
        int ch = dect2::marinevhf::channelFromDscText(c->freqRx);
        if (!ch) ch = dect2::marinevhf::channelFromDscText(c->freqTx);
        const bool isDistress = c->distress;
        if (c->vhf && (ch || isDistress)) {
            char bl[64]; snprintf(bl, sizeof bl, "Listen on VHF channel %d##dscvhf", ch ? ch : 16);
            if (ImGui::SmallButton(bl)) marineVhfJump(a, ch ? ch : 16);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Opens the Marine VHF voice mode on that channel (it needs a radio that covers 156 to 162 MHz).");
        }
    }
    kv("end", c->eos == 117 ? "acknowledgement requested" : c->eos == 122 ? "acknowledgement" : c->eos == 127 ? "other call" : "");
    char b[64];
    snprintf(b, sizeof b, "%s, %d symbols lost%s", c->eccOk ? "check ok" : "check failed", c->erasures, c->vhf ? ", VHF ch 70" : ", MF/HF");
    kv("received", b);
    if (c->repaired > 0) {
        snprintf(b, sizeof b, c->recovered ? tr("%d symbols fixed, one rebuilt from the check character") : tr("%d symbols taken from the second copy"), c->repaired);
        kv(tr("repair"), b);
    }
    if (!c->symbols.empty()) {   // the raw symbols of the information part, like the numbers YADD shows
        std::string sy;
        for (size_t i = 0; i < c->symbols.size(); i++) { char q[8]; if (c->symbols[i] < 0) snprintf(q, sizeof q, "%s---", i ? " " : ""); else snprintf(q, sizeof q, "%s%03d", i ? " " : "", c->symbols[i]); sy += q; }
        kv(tr("symbols"), sy);
    }
    kv("heard", utcText(c->rxTime) + dect2::timefmt::sfx());
    if (!c->hasPos) return;
    ImGui::Spacing();
    const float mh = std::max(80.f * gUi, ImGui::GetContentRegionAvail().y - 4 * gUi);
    if (mh < 60 * gUi || size.x < 60 * gUi) return;
    if (S.mapKey != dscKey(*c)) { S.map.lat = c->lat; S.map.lon = c->lon; S.mapKey = dscKey(*c); }   // centre on a newly selected call, then the user may drag
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    adsbmap::draw(S.map, ImVec2(ImGui::GetContentRegionAvail().x, mh));
    const ImVec2 q = adsbmap::project(c->lat, c->lon);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + ImGui::GetContentRegionAvail().x + 1, p0.y + mh), true);
    const ImU32 col = c->distress ? IM_COL32(235, 70, 70, 255) : IM_COL32(110, 205, 120, 255);
    dl->AddCircleFilled(q, 6 * gUi, col);
    dl->AddCircle(q, 11 * gUi, col, 24, 2.f);
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 6 * gUi, p0.y + 6 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(24 * gUi, 0))) S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom in"));
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(24 * gUi, 0))) S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom out"));
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never the position itself");
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + mh));
    ImGui::Dummy(ImVec2(0, 0));   // an item after SetCursorScreenPos: ImGui otherwise reports "extend window/parent boundaries"
}

void dscView(App& a) {
    const dect2::MarineTelemetry& t = a.rx.marine;
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float dw = std::min(320.f * gUi, W * 0.32f);
    loadNames();
    const DscCall* sel = nullptr;
    for (const auto& c : t.dsc) if (dscKey(c) == S.dscSelKey) sel = &c;
    if (!sel && !t.dsc.empty()) sel = &t.dsc.back();
    ImGui::BeginChild("##dsc_l", ImVec2(W - dw - 8 * gUi, H));
    {   // a distress alert, relay or acknowledgement among the calls kept: a red banner with the newest one
        const DscCall* d = nullptr;
        for (const auto& c : t.dsc) if (c.distress && c.eccOk) d = &c;
        if (d) {
            ImGui::PushStyleColor(ImGuiCol_Text, pal::badRed());
            ImGui::TextWrapped("%s: %s", tr("DISTRESS"), d->text.c_str());
            ImGui::PopStyleColor();
        }
    }
    incidentBoardUi(a, true);   // sea distress incidents: DSC + AIS + AIS-SART + NAVTEX in one card (marine_incident.h)
    ImGui::SetNextItemWidth(180 * gUi);
    ImGui::InputTextWithHint("##dscf", tr("search calls"), S.dscFilter, sizeof S.dscFilter);
    ImGui::SameLine();
    if (ImGui::Button(tr("Export CSV")) && !t.dsc.empty()) {
        const std::string p = saveFileDialog("dsc_calls.csv");
        if (!p.empty()) { std::ofstream f(std::filesystem::u8path(p), std::ios::binary); f << dscCsv(t.dsc); { char mb[600]; snprintf(mb, sizeof mb, f ? tr("saved %s") : tr("could not write %s"), p.c_str()); S.dscMsg = mb; } }
    }
    ImGui::SameLine();
    importUi();
    ImGui::SameLine();
    if (ImGui::Button(tr("Watchlists"))) a.watchlistOpen = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("In a Ships list, the entry DSC distress alerts on every DSC distress call; an MMSI alerts on that station."));
    if (!S.dscMsg.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", S.dscMsg.c_str()); }
    const std::string needle = lowerStr(S.dscFilter);
    // fixed widths that fit the content and a horizontal scroll bar when the pane is narrow, so no column is cut
    const ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##dsc_t", 7, fl, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("UTC");
        ImGui::TableSetupColumn("Call");
        ImGui::TableSetupColumn("Category");
        ImGui::TableSetupColumn("From");
        ImGui::TableSetupColumn("To");
        ImGui::TableSetupColumn("Position");
        ImGui::TableSetupColumn("Check");
        ImGui::TableHeadersRow();
        for (size_t i = t.dsc.size(); i-- > 0;) {    // newest first
            const DscCall& c = t.dsc[i];
            if (!dscMatches(c, needle)) continue;
            ImGui::TableNextRow();
            if (c.distress) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(120, 28, 28, 150));
            ImGui::TableNextColumn();
            ImGui::PushID((int)i);
            const std::string k = dscKey(c);
            if (ImGui::Selectable(utcText(c.rxTime).c_str(), sel == &c, ImGuiSelectableFlags_SpanAllColumns)) S.dscSelKey = k;
            ImGui::PopID();
            ImGui::TableNextColumn();
            std::string nm = c.kind.empty() ? c.formatName : c.kind;
            if (!c.natureName.empty()) nm += ": " + c.natureName;
            if (c.distress) ImGui::TextColored(ImVec4(1.f, 0.55f, 0.5f, 1), "%s", nm.c_str()); else ImGui::TextUnformatted(nm.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.categoryName.c_str());
            ImGui::TableNextColumn(); { const std::string nm2 = dect2::marine::ShipDb::global().name(c.fromMmsi); const std::string f = (c.fromMmsi.empty() ? std::string("?") : c.fromMmsi) + (nm2.empty() ? "" : " " + nm2); ImGui::TextUnformatted(f.c_str()); if (ImGui::IsItemHovered() && !c.fromCountry.empty()) ImGui::SetTooltip("%s", c.fromCountry.c_str()); }
            ImGui::TableNextColumn(); { const std::string nm2 = dect2::marine::ShipDb::global().name(c.to); const std::string f = c.to + (nm2.empty() ? "" : " " + nm2); ImGui::TextUnformatted(f.c_str()); if (ImGui::IsItemHovered() && !c.toCountry.empty()) ImGui::SetTooltip("%s", c.toCountry.c_str()); }
            ImGui::TableNextColumn(); if (c.hasPos) ImGui::TextUnformatted(c.posText.c_str()); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            if (c.eccOk) ImGui::TextColored(pal::okGreen(), "ok"); else ImGui::TextColored(pal::badRed(), "bad");
        }
        ImGui::EndTable();
    }
    if (t.dsc.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", live(a) ? "no DSC call yet" : "start the receiver"); ImGui::PopTextWrapPos(); }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##dsc_r", ImVec2(0, H), ImGuiChildFlags_Borders);
    dscDetail(a, sel, ImGui::GetContentRegionAvail());
    ImGui::EndChild();
}

// ---------------------------------------------------------------- fax

// The picture as the user wants it: S.last (the decoder's) turned by the Rotate setting. Used for the screen, PNG saves and the gallery.
dect2::FaxImage rotatedFax(const dect2::FaxImage& im, int turns) {
    if (dect2::wefaxTurns(turns) == 0 || im.width <= 0 || im.height <= 0 || im.pix.size() < (size_t)im.width * im.height) return im;
    dect2::FaxImage o;
    dect2::wefaxRotate(im.pix.data(), im.width, im.height, turns, o.pix, o.width, o.height);
    return o;
}

// Puts S.last (rotated) on the screen texture
void presentLast(bool finished) {
    if (!gGfx || S.last.width <= 0 || S.last.height <= 0) return;
    const dect2::FaxImage im = rotatedFax(S.last, S.rot);
    const int th = std::max(1500, im.height);
    if (!S.img || S.texW != im.width || S.texH < im.height) {
        delete S.img;
        S.img = gGfx->createImage(im.width, th, 0xFFFFFFFFu);
        S.texW = im.width; S.texH = th;
    }
    if (!S.img) return;
    std::vector<uint32_t> px((size_t)im.width * im.height);
    for (size_t i = 0; i < px.size(); i++) { const uint32_t g = im.pix[i]; px[i] = 0xFF000000u | (g << 16) | (g << 8) | g; }
    S.img->update(0, 0, im.width, im.height, px.data());
    S.enh.e.setSource("Weather fax");
    S.enh.feed(px.data(), im.width, im.height, finished);   // still arriving: the cheap preview path
    S.imgW = im.width; S.imgH = im.height;
}

void setRotation(int turns) {
    S.rot = dect2::wefaxTurns(turns);
    plat::prefs().setI("faxRotate", S.rot);
    S.zoom = 0; S.pan = ImVec2(0, 0);
    presentLast(true);
}

void uploadFax(App& a) {
    if (!live(a) || !gGfx) return;
    if (a.rx.marine.fax.imageSeq == S.telImgSeq) return;     // ask for a copy only when the report says the picture changed
    dect2::FaxImage im;
    uint64_t seq = S.imgSeq;
    if (!a.engine.marine().latestImage(im, seq)) { S.telImgSeq = a.rx.marine.fax.imageSeq; return; }
    S.imgSeq = seq;
    S.telImgSeq = a.rx.marine.fax.imageSeq;
    if (im.width <= 0 || im.height <= 0 || im.pix.size() < (size_t)im.width * im.height) {
        S.imgW = S.imgH = 0; S.last = dect2::FaxImage();      // the decoder started a new picture (cleared): drop the old one too
        return;
    }
    S.last = std::move(im);
    presentLast(false);
}

// a picture as PNG, where the user picks
std::string saveFaxImage(const dect2::FaxImage& im, const std::string& name) {
    if (im.width <= 0 || im.height <= 0) return "";
    const std::string path = saveFileDialog(name.c_str());
    if (path.empty()) return "";
    std::vector<uint8_t> rgba((size_t)im.width * im.height * 4);
    for (size_t i = 0; i < im.pix.size() && i * 4 + 3 < rgba.size(); i++) {
        rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = im.pix[i];
        rgba[i * 4 + 3] = 255;
    }
    return gfx::writePng(path.c_str(), rgba.data(), im.width, im.height) ? "saved " + path : "could not write " + path;
}

void saveFax() {
    if (S.last.width <= 0 || S.last.height <= 0) return;
    const std::string path = saveFileDialog("weather-fax.png");
    if (path.empty()) return;
    const dect2::FaxImage im = rotatedFax(S.last, S.rot);   // saved the way it is shown
    std::vector<uint8_t> rgba((size_t)im.width * im.height * 4);
    for (size_t i = 0; i < im.pix.size() && i * 4 + 3 < rgba.size(); i++) {
        rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = im.pix[i];
        rgba[i * 4 + 3] = 255;
    }
    S.saveMsg = gfx::writePng(path.c_str(), rgba.data(), im.width, im.height) ? "saved " + path : "could not write " + path;
}

void faxView(App& a) {
    S.faxShown = true;
    const dect2::MarineTelemetry& t = a.rx.marine;
    const dect2::MarineFaxInfo& f = t.fax;
    static const char* const lpmN[] = {"lpm: detect", "60 lpm", "90 lpm", "120 lpm", "240 lpm"};
    static const char* const iocN[] = {"IOC: start tone", "IOC 576", "IOC 288"};
    ImGui::SetNextItemWidth(110 * gUi);
    if (ImGui::Combo("##mflpm", &S.lpmIdx, lpmN, 5)) plat::prefs().setI("marineLpm", S.lpmIdx);
    flowNext();
    listenControls(a, S.listen, "faxListen", "fax", "Play the fax tones (the upper sideband from the dial); decoding is the same either way");
    flowNext(); ImGui::SetNextItemWidth(130 * gUi);   // the controls wrap in a narrow window
    if (ImGui::Combo("##mfioc", &S.iocIdx, iocN, 3)) plat::prefs().setI("marineIoc", S.iocIdx);
    flowNext();
    if (ImGui::Checkbox("auto slant", &S.autoSlant)) plat::prefs().setB("marineAutoSlant", S.autoSlant);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Measure the sample clock error from the phasing lines and straighten the picture.\nThe slider is added on top.");
    flowNext(); ImGui::SetNextItemWidth(150 * gUi);
    if (ImGui::SliderFloat("##mfsl", &S.slant, -300.f, 300.f, "slant %+.1f ppm")) plat::prefs().setD("marineSlant", S.slant);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Straighten a slanted picture by hand. Applies to the lines already received too. Double-click to type.");
    ImGui::SameLine();
    if (ImGui::SmallButton("0")) { S.slant = 0; plat::prefs().setD("marineSlant", 0.0); }
    flowNext(14 * gUi);
    if (ImGui::SmallButton("fit")) { S.zoom = 0; S.pan = ImVec2(0, 0); }
    ImGui::SameLine();
    if (ImGui::SmallButton("1:1")) { S.zoom = 1; }
    flowNext(14 * gUi);
    ImGui::BeginDisabled(S.last.width <= 0);
    if (ImGui::SmallButton(tr("save PNG"))) saveFax();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("clear picture##faxclr"))) { a.engine.marine().clearFax(); marineFaxClearUi(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Clears the picture and the lines the decoder has collected, and starts a new picture at the next start tone or phasing. Pictures already in the list on the right stay."));
    flowNext(14 * gUi);
    ImGui::TextDisabled("%s", tr("rotate"));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("90 right##faxrr"))) setRotation(S.rot + 1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Turn the picture 90 degrees clockwise (key ]). Applies to the picture on the screen and to the PNGs saved from now on; the setting is remembered."));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("90 left##faxrl"))) setRotation(S.rot - 1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Turn the picture 90 degrees counter-clockwise (key [)."));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("180##faxr2"))) setRotation(S.rot + 2);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Turn the picture upside down (key \\). Many stations are received with the picture the wrong way up."));
    if (S.rot != 0) { ImGui::SameLine(); ImGui::TextDisabled("%d", S.rot * 90); }
    if (!ImGui::GetIO().WantTextInput && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, false)) setRotation(S.rot + 1);
        else if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false)) setRotation(S.rot - 1);
        else if (ImGui::IsKeyPressed(ImGuiKey_Backslash, false)) setRotation(S.rot + 2);
    }
    flowEnd();
    S.enh.e.setSource("Weather fax");
    S.enh.controls("faxenh");
    static const char* const st[] = {"waiting for the start tone", "phasing", "receiving", "stopped"};
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s, IOC %d, %d lpm, %d lines, %d px wide, slant %+.1f ppm, black at %.0f Hz (%+.0f Hz off), last tone %.0f Hz",
                        st[std::max(0, std::min(3, f.state))], f.ioc, f.lpm, f.lines, f.width, f.slantPpm, f.blackHz, f.blackHz - 1500.0, f.toneHz); ImGui::PopTextWrapPos(); }
    if (!S.saveMsg.empty()) { ImGui::SameLine(); ImGui::TextDisabled("  %s", S.saveMsg.c_str()); }

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 box = ImGui::GetContentRegionAvail();
    if (box.x < 20 || box.y < 20) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + box.x, p0.y + box.y), IM_COL32(18, 19, 21, 255));
    ImGui::InvisibleButton("##faxpic", box);
    const bool hov = ImGui::IsItemHovered();
    if (!S.img || S.imgW <= 0) {
        const char* msg = !live(a) ? "start the receiver, or pick a station under Stations"
                        : f.state == 1 ? "phasing: the picture starts after the phasing lines"
                        : "listening: a picture starts by itself on the start tone, the phasing, or a picture already under way";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(p0.x + (box.x - ts.x) * 0.5f, p0.y + box.y * 0.5f), IM_COL32(150, 154, 158, 255), msg);
        return;
    }
    const float fit = dect2::wefaxFitZoom(box.x, box.y, S.imgW, S.imgH, S.rot);
    float z = S.zoom > 0 ? S.zoom : fit;
    if (hov && ImGui::GetIO().MouseWheel != 0) {     // zoom around the pointer
        const ImVec2 m = ImGui::GetIO().MousePos;
        const float nz = std::max(fit * 0.25f, std::min(8.f, z * std::pow(1.15f, ImGui::GetIO().MouseWheel)));
        S.pan.x = (m.x - p0.x) - ((m.x - p0.x) - S.pan.x) * nz / z;
        S.pan.y = (m.y - p0.y) - ((m.y - p0.y) - S.pan.y) * nz / z;
        S.zoom = nz; z = nz;
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        S.pan.x += d.x; S.pan.y += d.y;
    }
    const float w = S.imgW * z, h = S.imgH * z;
    if (S.zoom <= 0) {
        S.pan.x = 0;
        if (h > box.y) S.pan.y = S.rot == 2 ? 0.f : box.y - h;   // fitted: keep the newest lines in view (upside down they are at the top)
        else S.pan.y = 0;
        if (w < box.x) S.pan.x = (box.x - w) * 0.5f;   // a whole turned picture narrower than the box: centred
    }
    if (S.zoom > 0 || w >= box.x) S.pan.x = std::min(0.f, std::max(S.pan.x, std::min(0.f, box.x - w)));
    S.pan.y = std::min(0.f, std::max(S.pan.y, std::min(0.f, box.y - h)));
    const ImVec2 q0(p0.x + S.pan.x, p0.y + S.pan.y), q1(q0.x + w, q0.y + h);
    dl->PushClipRect(p0, ImVec2(p0.x + box.x, p0.y + box.y), true);
    gfx::Image* shown = S.enh.texture(S.img);   // the enhanced view when Enhance is on (its own texture, exactly the picture's size)
    dl->AddImage(shown->texture(), q0, q1, ImVec2(0, 0), ImVec2(1, shown == S.img ? (float)S.imgH / (float)S.texH : 1.f));
    dl->PopClipRect();
    char b[48];
    snprintf(b, sizeof b, "%dx%d  %.0f %%", S.imgW, S.imgH, z * 100);
    dl->AddText(ImVec2(p0.x + 6 * gUi, p0.y + box.y - ImGui::GetTextLineHeight() - 4 * gUi), IM_COL32(200, 60, 60, 255), b);
}

// ---------------------------------------------------------------- fax stations: directory and schedule

void tuneStation(App& a, double carrierKhz) {
    tuneFreq(a, dect2::wefaxDialKhz(carrierKhz) / 1000.0);   // USB dial 1.9 kHz below the carrier
    if (!a.engine.running()) startReceiver(a);
}

void stationsView(App& a) {
    using namespace dect2;
    static const char* const regions[] = {"all regions", "Europe", "North America", "Asia", "Oceania", "South America", "Africa"};
    ImGui::SetNextItemWidth(140 * gUi);
    ImGui::Combo("##wfreg", &S.region, regions, 7);
    const std::string reg = S.region > 0 ? regions[S.region] : "";
    ImGui::SameLine();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Frequencies are the carriers; a click tunes the USB dial 1.9 kHz lower. Times (UTC) marked ~ are approximate: "
                        "schedules change, check the NOAA Worldwide Marine Radiofacsimile Broadcast Schedules or the station.");
    ImGui::PopTextWrapPos();
    const float H = ImGui::GetContentRegionAvail().y, W = ImGui::GetContentRegionAvail().x;
    const long long now = (long long)time(nullptr);
    ImGui::BeginChild("##wfnext", ImVec2(std::min(460.f * gUi, W * 0.5f), H), ImGuiChildFlags_Borders);
    ImGui::TextColored(pal::heading(), "Next broadcasts");
    if (ImGui::BeginTable("##wfnt", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        for (const WefaxNext& n : wefaxNext(now, 40, reg)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const long long d = n.startUtc - now;
            char t[32];
            if (d <= 0) snprintf(t, sizeof t, "on now");
            else snprintf(t, sizeof t, "in %lld:%02lld", d / 3600, d / 60 % 60);
            ImGui::Text("%s%02d:%02d %s", n.b->approximate ? "~" : " ", n.b->utcMin / 60, n.b->utcMin % 60, t);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(n.s->callsign);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(n.b->product);
            ImGui::TableNextColumn();
            char b[48]; snprintf(b, sizeof b, "%.1f##wfn%p", n.s->carrierKhz.empty() ? 0.0 : n.s->carrierKhz[0], (const void*)n.b);
            if (!n.s->carrierKhz.empty() && ImGui::SmallButton(b)) tuneStation(a, n.s->carrierKhz[0]);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s, %s: tune the dial to %.1f kHz USB (the station's first frequency; the directory has the others)", n.s->name, n.s->country, wefaxDialKhz(n.s->carrierKhz[0]));
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##wfdir", ImVec2(0, H), ImGuiChildFlags_Borders);
    ImGui::TextColored(pal::heading(), "Stations");
    const char* country = "";
    for (const WefaxStation& s : wefaxStations()) {
        if (!reg.empty() && reg != s.region) continue;
        if (strcmp(country, s.country) != 0) { country = s.country; ImGui::Spacing(); ImGui::TextDisabled("%s", country); }
        ImGui::Text("  %s  %s", s.callsign, s.name);
        ImGui::Indent(14 * gUi);
        bool first = true;
        for (double k : s.carrierKhz) {
            if (!first) ImGui::SameLine();
            first = false;
            const bool here = std::fabs(a.freqMhz * 1000.0 - wefaxDialKhz(k)) < 0.05;
            if (here) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.45f));
            char b[48]; snprintf(b, sizeof b, "%.1f##wf%s%.1f", k, s.id, k);
            if (ImGui::SmallButton(b)) tuneStation(a, k);
            if (here) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("carrier %.1f kHz: dial %.1f kHz USB", k, wefaxDialKhz(k));
        }
        ImGui::Unindent(14 * gUi);
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------- tuning aid

void tuningView(App& a, ImVec2 size) {
    const dect2::MarineTelemetry& t = a.rx.marine;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    dl->AddRectFilled(p0, p1, IM_COL32(8, 9, 10, 255));
    const float lo = t.specLoHz, hi = t.specHiHz;
    const float l = p0.x + 40 * gUi, r = p1.x - 8 * gUi, top = p0.y + 6 * gUi, bot = p1.y - 20 * gUi;
    if (r - l < 40 || bot - top < 30) { ImGui::Dummy(size); return; }
    auto X = [&](double hz) { return (float)(l + (hz - lo) / (hi - lo) * (r - l)); };
    float dmax = -200, dmin = 200;
    for (float v : t.spectrumDb) { dmax = std::max(dmax, v); dmin = std::min(dmin, v); }
    if (t.spectrumDb.empty()) { dmax = 0; dmin = -60; }
    const float yTop = std::ceil((dmax + 5) / 10) * 10, yBot = std::max(yTop - 80, std::floor(dmin / 10) * 10);
    auto Y = [&](float db) { return bot - (db - yBot) / std::max(1.f, yTop - yBot) * (bot - top); };
    const ImU32 grid = IM_COL32(48, 52, 54, 255), txt = IM_COL32(120, 124, 128, 255);
    char b[32];
    for (int hz = -500; hz <= 3500; hz += 500) {
        dl->AddLine(ImVec2(X(hz), top), ImVec2(X(hz), bot), grid);
        snprintf(b, sizeof b, "%d", hz);
        dl->AddText(ImVec2(X(hz) - 12 * gUi, bot + 3 * gUi), txt, b);
    }
    for (float db = yBot; db <= yTop; db += 10) {
        dl->AddLine(ImVec2(l, Y(db)), ImVec2(r, Y(db)), grid);
        snprintf(b, sizeof b, "%.0f", db);
        dl->AddText(ImVec2(p0.x + 4 * gUi, Y(db) - 7 * gUi), txt, b);
    }
    // the tones each service expects, relative to the dial frequency (USB for fax)
    struct Mark { double hz; const char* label; ImU32 col; int row; };   // row: label line, so that close markers do not overlap
    const ImU32 cf = IM_COL32(110, 170, 235, 170), cn = IM_COL32(230, 180, 70, 170), ct = IM_COL32(160, 120, 220, 170);
    const Mark marks[] = {{-85, "FSK", cn, 0}, {85, "", cn, 0}, {1500, "black", cf, 0}, {2300, "white", cf, 0},
                          {300, "start 576", ct, 1}, {675, "start 288", ct, 0}, {450, "stop", ct, 2}};
    for (const Mark& m : marks) {
        dl->AddLine(ImVec2(X(m.hz), top), ImVec2(X(m.hz), bot), m.col, 1.5f);
        if (m.label[0]) dl->AddText(ImVec2(X(m.hz) + 3 * gUi, top + 2 * gUi + m.row * ImGui::GetTextLineHeight()), m.col, m.label);
    }
    if (t.toneHighHz != 0 || t.toneLowHz != 0) {     // the FSK tones the receiver found
        for (double hz : {t.toneLowHz, t.toneHighHz}) dl->AddLine(ImVec2(X(hz), top), ImVec2(X(hz), bot), IM_COL32(120, 230, 140, 230), 2.f);
    }
    const size_t n = t.spectrumDb.size();
    if (n > 1) {
        std::vector<ImVec2> pts(n);
        for (size_t i = 0; i < n; i++) pts[i] = ImVec2(X(lo + (hi - lo) * (double)i / (double)(n - 1)), std::max(top, std::min(bot, Y(t.spectrumDb[i]))));
        dl->AddPolyline(pts.data(), (int)n, IM_COL32(140, 230, 160, 255), 0, 1.3f);
    } else {
        dl->AddText(ImVec2(l + 10, top + 10), txt, live(a) ? "no spectrum yet" : "start the receiver");
    }
    ImGui::Dummy(size);
}

void tuningPanel(App& a) {
    const dect2::MarineTelemetry& t = a.rx.marine;
    const ImVec2 av = ImGui::GetContentRegionAvail();
    tuningView(a, ImVec2(av.x, std::max(120.f * gUi, av.y - 3 * ImGui::GetTextLineHeightWithSpacing())));
    if (t.toneHighHz != 0 || t.toneLowHz != 0)
        { ImGui::PushTextWrapPos(0); ImGui::Text("FSK tones %+.0f / %+.0f Hz, shift %.0f Hz (170 expected), centre %+.0f Hz, %.1f baud, %.0f dB over the noise",
                    t.toneLowHz, t.toneHighHz, t.shiftHz, t.cfoHz, t.baudEst, t.fskLevelDb); ImGui::PopTextWrapPos(); }
    else { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("no FSK signal found: NAVTEX and HF DSC sit on the dial frequency, 85 Hz either side"); ImGui::PopTextWrapPos(); }
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("VHF DSC (channel 70) is FM with audio tones of 1300 and 2100 Hz: it does not show in this spectrum."); ImGui::PopTextWrapPos(); }
}

// the analysis row: the tuning aid and the newest message or the fax picture
void panels(App& a) {
    const dect2::MarineTelemetry& t = a.rx.marine;
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi, plotH = std::max(60.f, H - ImGui::GetTextLineHeightWithSpacing() - 6);
    const float sw = std::max(200.f, W * 0.6f - gap);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap * 0.5f);
    ImGui::BeginGroup();
    captionFit(sw, "Tuning aid: Hz from the dial frequency, with the tones each service expects");
    tuningView(a, ImVec2(sw, plotH));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    const float rw = std::max(120.f, ImGui::GetContentRegionAvail().x - gap * 0.5f);
    if (live(a) && t.serviceActive == 3 && S.img && S.imgW > 0) {
        captionFit(rw, "Fax: %d lines", t.fax.lines);
        const float z = std::min(rw / S.imgW, plotH / std::max(1, S.imgH));
        const float w = S.imgW * z, h = S.imgH * z;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddImage(S.img->texture(), p, ImVec2(p.x + w, p.y + h), ImVec2(0, 0), ImVec2(1, (float)S.imgH / (float)S.texH));
        ImGui::Dummy(ImVec2(rw, plotH));
    } else {
        captionFit(rw, "Last decoded");
        ImGui::BeginChild("##mar_last", ImVec2(rw, plotH), ImGuiChildFlags_Borders);
        const NavtexMessage* m = t.navtex.empty() ? nullptr : &t.navtex.back();
        const DscCall* c = t.dsc.empty() ? nullptr : &t.dsc.back();
        if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("start the receiver"); ImGui::PopTextWrapPos(); }
        else if (c && (!m || c->rxSec >= m->rxSec)) {
            if (c->distress) ImGui::TextColored(pal::badRed(), "%s", c->text.c_str()); else ImGui::TextWrapped("%s", c->text.c_str());
        } else if (m) {
            ImGui::TextColored(pal::heading(), "%s  %s", m->header.c_str(), m->subjectName.c_str());
            ImGui::PushFont(a.mono, 0); ImGui::TextWrapped("%s", m->text.c_str()); ImGui::PopFont();
        } else { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("nothing decoded yet"); ImGui::PopTextWrapPos(); }
        ImGui::EndChild();
    }
    ImGui::EndGroup();
}

// ---------------------------------------------------------------- the hooks

// a protocol entry (NAVTEX, DSC, weather fax): its service, 0 without one
int subService(const App& a) { const SubMode* sm = curSub(a); return sm ? sm->sel : 0; }

void scanTick(App& a);

void tick(App& a) {
    loadState();
    if (!a.engine.running()) { S.pushedService = S.pushedLpm = S.pushedIoc = S.pushedAuto = -1; S.pushedFreq = -1; S.pushedSlant = 1e9f; return; }
    dect2::MarineReceiver& r = a.engine.marine();
    if (S.saveNav && live(a)) {
        const DeviceInfo::Kind dk = a.devices.empty() ? DeviceInfo::Synthetic : a.devices[(size_t)std::max(0, std::min((int)a.devices.size() - 1, a.devIdx))].kind;
        S.saver.update(a.rx.marine.navtex, plat::dataDir() + "/navtex", dk == DeviceInfo::Synthetic || dk == DeviceInfo::File ? 0.0 : a.freqMhz * 1e6);
        if (S.saver.failed() && S.saveErr.empty()) S.saveErr = S.saver.error();
    }
    const int svc = subService(a) ? subService(a) : S.service;   // a protocol entry runs only its own decoder
    if (S.pushedService != svc) { r.setService(svc); S.pushedService = svc; }
    // a test signal or a recording has no dial frequency: auto then runs every decoder
    const DeviceInfo::Kind k = a.devices.empty() ? DeviceInfo::Synthetic : a.devices[(size_t)std::max(0, std::min((int)a.devices.size() - 1, a.devIdx))].kind;
    const double fz = k == DeviceInfo::Synthetic || k == DeviceInfo::File ? 0.0 : a.freqMhz * 1e6;
    if (S.pushedFreq != fz) { r.setFrequencyHz(fz); S.pushedFreq = fz; }
    if (S.pushedLpm != S.lpmIdx) { r.setFaxLpm(kLpm[S.lpmIdx]); S.pushedLpm = S.lpmIdx; }
    if ((int)S.listen != S.pushedListen) { r.setListen(S.listen); S.pushedListen = S.listen; }
    if (a.volume != S.pushedVol || (int)a.muted != S.pushedMute) { r.setVolume(a.volume); r.setMuted(a.muted); S.pushedVol = a.volume; S.pushedMute = a.muted; }
    if (S.pushedIoc != S.iocIdx) { r.setFaxIoc(kIoc[S.iocIdx]); S.pushedIoc = S.iocIdx; }
    if (S.pushedSlant != S.slant) { r.setFaxSlantPpm(S.slant); S.pushedSlant = S.slant; }
    if (S.pushedAuto != (int)S.autoSlant) { r.setFaxAutoSlant(S.autoSlant); S.pushedAuto = S.autoSlant; }
    marineFaxTexture();
    loadNames();
    saveNamesSoon();
    scanTick(a);
    if (live(a)) uploadFax(a);
    // a picture that ended (the stop tone, or the signal went): keep it for the list on the right
    const int fs = a.rx.marine.fax.state;
    if (live(a) && fs == 2 && S.prevFaxState != 2) S.faxStartUtc = (long long)time(nullptr);
    if (live(a) && S.prevFaxState == 2 && fs != 2 && S.last.height >= 50) {
        S.faxes.insert(S.faxes.begin(), State::Fax{(int64_t)time(nullptr), a.freqMhz * 1e3, rotatedFax(S.last, S.rot)});
        {
            gallery::Meta gm; gm.detail = std::to_string((int)std::lround(a.freqMhz * 1e3)) + " kHz, IOC " + std::to_string(a.rx.marine.fax.ioc) + ", " + std::to_string(a.rx.marine.fax.lpm) + " lpm";
            const dect2::FaxImage ri = rotatedFax(S.last, S.rot);
            gallery::publishGray("fax-" + std::to_string((long long)time(nullptr)), "Weather fax", (double)time(nullptr), ri.width, ri.height, ri.pix.data(), gm);
        }
        {   // the autosave: PNG + JSON side-car in the WEFAX folder (settings and gallery in wefax_gallery.cpp)
            wefaxgal::Capture c;
            c.im = rotatedFax(S.last, S.rot); c.dialKhz = a.freqMhz * 1e3; c.ioc = a.rx.marine.fax.ioc; c.lpm = a.rx.marine.fax.lpm;
            c.stopUtc = (long long)time(nullptr); c.startUtc = S.faxStartUtc > 0 ? S.faxStartUtc : c.stopUtc;
            c.snrDb = a.rx.marine.fax.snrDb; c.phasingLines = a.rx.marine.fax.phasingLines;
            c.reason = fs == 3 ? "stop tone" : fs == 1 ? "new start" : "timeout";
            const std::string r = wefaxgal::autosave(c);
            if (!r.empty()) S.listMsg = r;
        }
        if (S.faxes.size() > 30) S.faxes.pop_back();
    }
    S.prevFaxState = fs;
}


// ---------------------------------------------------------------- bands, scan, ship table

std::string hzText(double hz) {
    char b[48];
    if (hz <= 0) return "";
    if (hz < 30e6) snprintf(b, sizeof b, "%.1f kHz", hz / 1e3); else snprintf(b, sizeof b, "%.3f MHz", hz / 1e6);
    return b;
}

struct ScanSet { const char* name; unsigned groups; };
const ScanSet kScanSets[] = {
    {N_("Distress and safety (MF/HF)"), 1u << (int)dect2::marine::DscGroup::Distress},
    {N_("Worldwide calling"), (1u << (int)dect2::marine::DscGroup::WorldwideShip) | (1u << (int)dect2::marine::DscGroup::WorldwideCoast)},
    {N_("Series A calling"), (1u << (int)dect2::marine::DscGroup::SeriesAShip) | (1u << (int)dect2::marine::DscGroup::SeriesACoast)},
    {N_("Series B calling"), (1u << (int)dect2::marine::DscGroup::SeriesBShip) | (1u << (int)dect2::marine::DscGroup::SeriesBCoast)},
    {N_("VHF channel 70"), 1u << (int)dect2::marine::DscGroup::Vhf},
};

std::vector<dect2::marine::DscFreq> chosenChannels() {
    unsigned mask = 0;
    for (int i = 0; i < 5; i++) if ((S.scanGroups >> i) & 1) mask |= kScanSets[i].groups;
    std::vector<dect2::marine::DscFreq> r;
    for (const auto& f : dect2::marine::dscFrequencies()) if ((mask >> (int)f.group) & 1) r.push_back(f);
    return r;
}

void rebuildScan() {
    std::vector<dect2::marine::DscScan::Entry> l;
    for (const auto& f : chosenChannels()) l.push_back({f.hz, f.weight});
    S.scan.setList(l);
    S.scan.setDwell(S.scanDwell, 20);
}

// once a second while the Marine screens run: the scanner retunes the radio, the parallel list goes to the receiver
void scanTick(App& a) {
    static double lastSec = 0;
    const double now = (double)time(nullptr);
    if (now == lastSec) return;
    lastSec = now;
    dect2::MarineReceiver& r = a.engine.marine();
    if (S.parOn && !S.scanOn) {
        std::vector<double> want;
        for (const auto& f : chosenChannels()) if (f.hz < 30e6) want.push_back(f.hz);
        r.setParallelDsc(want);
    } else {
        r.setParallelDsc({});
    }
    if (!S.scanOn || !live(a)) return;
    const auto& d = a.rx.marine.dsc;
    double lastCall = 0;
    if (!d.empty()) lastCall = (double)d.back().rxTime;
    double hz = 0;
    if (S.scan.update(now, a.rx.marine.dscLocked, lastCall, hz)) {
        tuneFreq(a, hz / 1e6);
        S.scanLastRetune = now;
    }
}

// the positions of the calls kept: new ones are added to the long list the map and the exports use
void collectFixes(const dect2::MarineTelemetry& t) {
    if (t.dscCount == S.fixSeen) return;
    S.fixSeen = t.dscCount;
    dect2::marine::dscAddFixes(S.fixes, t.dsc);
}

bool writeText(const std::string& path, const std::string& text) {
    std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
    f << text;
    return (bool)f;
}

void bandsView(App& a) {
    loadNames();
    ImGui::BeginChild("##bands", ImVec2(0, 0));
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(pal::heading(), "%s", tr("DSC scanner"));
    for (int i = 0; i < 5; i++) {
        bool on = (S.scanGroups >> i) & 1;
        if (ImGui::Checkbox(tr(kScanSets[i].name), &on)) { S.scanGroups = on ? (S.scanGroups | (1 << i)) : (S.scanGroups & ~(1 << i)); plat::prefs().setI("marineScanSets", S.scanGroups); rebuildScan(); }
        if (i < 4) ImGui::SameLine();
    }
    if (ImGui::Checkbox(tr("Scan these channels with this radio"), &S.scanOn)) { if (S.scanOn) { S.parOn = false; rebuildScan(); S.scan.restart(); } }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Steps the radio through the channels: the distress channels are visited more often, and a locked decoder or a call keeps the radio on its channel."));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    if (ImGui::SliderInt(tr("dwell s"), &S.scanDwell, 1, 30)) { plat::prefs().setI("marineScanDwell", S.scanDwell); rebuildScan(); }
    if (ImGui::Checkbox(tr("Decode the chosen MF/HF channels in the same capture"), &S.parOn)) { if (S.parOn) S.scanOn = false; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Decodes every chosen channel that lies inside the radio's bandwidth at the same time (channels in a group are 0.5 kHz apart, the distress channels are MHz apart). Needs a sample rate of 250 kHz or more."));
    if (S.scanOn) ImGui::TextDisabled("%s %s", tr("scanning:"), hzText(S.scan.current()).c_str());
    if (S.parOn) {
        std::string line;
        for (const auto& pi : a.rx.marine.parallel) line += hzText(pi.hz) + (pi.locked ? " (locked)" : pi.fsk ? " (signal)" : "") + "  ";
        ImGui::TextDisabled("%s %s", tr("extra channels:"), line.empty() ? tr("none inside the capture") : line.c_str());
    }
    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "%s", tr("Ship and coast station names"));
    dect2::marine::ShipDb& db = dect2::marine::ShipDb::global();
    ImGui::TextDisabled("%zu %s", db.size(), tr("entries; ship names, call signs and types are learned from AIS (static data), and your own list can be imported."));
    importUi();
    ImGui::SameLine();
    if (ImGui::Button(tr("Export list"))) {
        const std::string p = saveFileDialog("ship_names.tsv");
        if (!p.empty()) { char mb[600]; snprintf(mb, sizeof mb, writeText(p, db.exportTsv()) ? tr("saved %s") : tr("could not write %s"), p.c_str()); S.dbMsg = mb; }
    }
    ImGui::TextDisabled("%s", tr("There is no bundled coast station list: no openly licensed one is available, so bring your own file."));
    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "%s", tr("Marine bands"));
    ImGui::TextDisabled("%s", tr("A click tunes the radio and opens the mode for it."));
    ImGui::PopTextWrapPos();
    const ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV;
    if (ImGui::BeginTable("##bandt", 5, fl)) {
        ImGui::TableSetupColumn(tr("Band"));
        ImGui::TableSetupColumn(tr("Frequency"));
        ImGui::TableSetupColumn(tr("Use"));
        ImGui::TableSetupColumn(tr("Protocols"));
        ImGui::TableSetupColumn(tr("Notes"));
        ImGui::TableHeadersRow();
        int fam = -2; double hz = 0; const char* sub = nullptr;
        int i = 0;
        for (const auto& b : dect2::marine::marineBands()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(i++);
            if (ImGui::Selectable(b.band, false, ImGuiSelectableFlags_SpanAllColumns) && (b.hz > 0 || b.family >= 0)) { fam = b.family; hz = b.hz; sub = b.sub; }
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(b.hz > 0 ? hzText(b.hz).c_str() : "-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(b.name);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(b.protocols);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(b.notes);
        }
        ImGui::EndTable();
        if (fam != -2) {
            S.scanOn = false;
            if (fam >= 0 && fam != 15) selectMode(a, fam);
            else if (fam == 15 && sub) selectMode(a, 15, sub);
            if (hz > 0) tuneFreq(a, hz / 1e6);
        }
    }
    ImGui::EndChild();
}

// who was heard and when, from the calls kept (rebuilt when the call counter moves)
void collectHeard(const dect2::MarineTelemetry& t) {
    if (S.heardSeen == t.dscCount) return;
    S.heardSeen = t.dscCount;
    S.heard.clear();
    S.links.clear();
    auto isNum = [](const std::string& m) { return m.size() == 9 && m.find_first_not_of("0123456789") == std::string::npos; };
    for (const auto& c : t.dsc) {
        if (!c.eccOk) continue;
        auto touch = [&](const std::string& m) {
            if (!isNum(m)) return;
            auto& h = S.heard[m];
            h.last = std::max(h.last, c.rxTime);
            h.count++;
            h.distress |= c.distress;
        };
        touch(c.fromMmsi);
        touch(c.distressMmsi);
        if (isNum(c.fromMmsi) && isNum(c.to)) S.links.push_back({c.fromMmsi, c.to, c.rxTime, c.distress});
    }
}

namespace {
enum MapCat { kCatDistress = 0, kCatCoast, kCatShip, kCatGroup, kCatOther };
MapCat mapCategory(const std::string& m, bool distress) {
    if (distress) return kCatDistress;
    switch (dect2::marine::mmsiInfo(m).kind) {
    case dect2::marine::MmsiKind::CoastStation: return kCatCoast;
    case dect2::marine::MmsiKind::Group: return kCatGroup;
    case dect2::marine::MmsiKind::Ship: return kCatShip;
    default: return kCatOther;
    }
}
const char* const kCatName[] = {"distress", "coast station", "ship", "group", "SAR / other"};
ImU32 catColour(MapCat c, bool heardNow, int alpha = 255) {
    switch (c) {
    case kCatDistress: return IM_COL32(235, 70, 70, alpha);
    case kCatCoast: return heardNow ? IM_COL32(90, 160, 245, alpha) : IM_COL32(130, 140, 155, std::min(alpha, 150));
    case kCatShip: return IM_COL32(110, 205, 120, alpha);
    case kCatGroup: return IM_COL32(230, 200, 80, alpha);
    default: return IM_COL32(235, 150, 70, alpha);
    }
}
std::string ageText(int64_t last) {
    if (last <= 0) return "-";
    const int64_t d = std::max<int64_t>(0, (int64_t)time(nullptr) - last);
    char b[32];
    if (d < 90) snprintf(b, sizeof b, "%llds", (long long)d);
    else if (d < 5400) snprintf(b, sizeof b, "%lldm", (long long)(d / 60));
    else if (d < 172800) snprintf(b, sizeof b, "%lldh", (long long)(d / 3600));
    else snprintf(b, sizeof b, "%lldd", (long long)(d / 86400));
    return b;
}
struct MapEnt {
    std::string mmsi, name, country, kind;
    double lat = 0, lon = 0;
    bool pos = false, heard = false, distress = false;
    int64_t last = 0;
    int count = 0;
    MapCat cat = kCatOther;
};
} // namespace

void mapView(App& a) {
    const dect2::MarineTelemetry& t = a.rx.marine;
    loadNames();
    collectFixes(t);
    collectHeard(t);
    dect2::marine::ShipDb& db = dect2::marine::ShipDb::global();
    // ---- the stations: every one with a position (list or heard), plus heard ones without
    std::map<std::string, std::vector<const dect2::marine::DscFix*>> by;
    for (const auto& f : S.fixes) by[f.mmsi].push_back(&f);
    std::map<std::string, MapEnt> ents;
    for (const auto& r : db.positioned()) { MapEnt& e = ents[r.mmsi]; e.mmsi = r.mmsi; e.lat = r.lat; e.lon = r.lon; e.pos = true; }
    for (const auto& kv : by) { MapEnt& e = ents[kv.first]; e.mmsi = kv.first; e.lat = kv.second.back()->lat; e.lon = kv.second.back()->lon; e.pos = true; for (const auto* f : kv.second) e.distress |= f->distress; }
    for (const auto& kv : S.heard) { MapEnt& e = ents[kv.first]; e.mmsi = kv.first; e.heard = true; e.last = kv.second.last; e.count = kv.second.count; e.distress |= kv.second.distress; }
    for (auto& kv : ents) {
        MapEnt& e = kv.second;
        const auto info = dect2::marine::mmsiInfo(e.mmsi);
        e.name = db.name(e.mmsi);
        e.country = info.country;
        e.kind = dect2::marine::mmsiKindName(info.kind);
        e.cat = mapCategory(e.mmsi, e.distress);
    }
    const std::string needle = lowerStr(S.mapFilter);
    auto passes = [&](const MapEnt& e) {
        if (!((S.mapTypes >> (int)e.cat) & 1)) return false;
        if (S.mapHeardOnly && !e.heard) return false;
        if (!needle.empty() && lowerStr(e.mmsi + "|" + e.name + "|" + e.country).find(needle) == std::string::npos) return false;
        return true;
    };
    // ---- controls
    for (int i = 0; i < 5; i++) {
        bool on = (S.mapTypes >> i) & 1;
        const ImVec4 c = ImGui::ColorConvertU32ToFloat4(catColour((MapCat)i, true));
        ImGui::PushStyleColor(ImGuiCol_CheckMark, c);
        if (ImGui::Checkbox(tr(kCatName[i]), &on)) S.mapTypes = on ? (S.mapTypes | (1 << i)) : (S.mapTypes & ~(1 << i));
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    ImGui::Checkbox(tr("heard only"), &S.mapHeardOnly);
    ImGui::SameLine();
    ImGui::Checkbox(tr("trails"), &S.mapTrails);
    ImGui::SameLine();
    ImGui::Checkbox(tr("calls"), &S.mapLinks);
    ImGui::SameLine();
    ImGui::Checkbox(tr("names"), &S.mapLabels);
    ImGui::SetNextItemWidth(160 * gUi);
    ImGui::InputTextWithHint("##mapf", tr("search MMSI, name, country"), S.mapFilter, sizeof S.mapFilter);
    ImGui::SameLine();
    importUi();
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Export KML")) && !S.fixes.empty()) {
        const std::string p = saveFileDialog("dsc_positions.kml");
        if (!p.empty()) { char mb[600]; snprintf(mb, sizeof mb, writeText(p, dect2::marine::dscKml(S.fixes, db)) ? tr("saved %s") : tr("could not write %s"), p.c_str()); S.dscMsg = mb; }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Export GPX")) && !S.fixes.empty()) {
        const std::string p = saveFileDialog("dsc_positions.gpx");
        if (!p.empty()) { char mb[600]; snprintf(mb, sizeof mb, writeText(p, dect2::marine::dscGpx(S.fixes, db)) ? tr("saved %s") : tr("could not write %s"), p.c_str()); S.dscMsg = mb; }
    }
    if (!S.dscMsg.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", S.dscMsg.c_str()); }
    int shown = 0, heardN = 0;
    for (const auto& kv : ents) if (kv.second.pos && passes(kv.second)) { shown++; heardN += kv.second.heard; }
    ImGui::TextDisabled("%d %s, %d %s, %zu %s", shown, tr("on the map"), heardN, tr("heard"), S.fixes.size(), tr("positions kept"));

    // ---- map (upper part) and table (lower part)
    const float availY = ImGui::GetContentRegionAvail().y;
    const ImVec2 sz(ImGui::GetContentRegionAvail().x, std::max(120.f * gUi, availY * 0.62f));
    static bool centred = false;
    if (!centred && !S.fixes.empty()) { S.map.lat = S.fixes.back().lat; S.map.lon = S.fixes.back().lon; centred = true; }
    if (!centred && !ents.empty()) { for (const auto& kv : ents) if (kv.second.pos) { S.map.lat = kv.second.lat; S.map.lon = kv.second.lon; centred = true; break; } }
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    adsbmap::draw(S.map, sz);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), true);
    const ImVec2 mp = ImGui::GetIO().MousePos;
    std::string hover, hoverMmsi;
    auto entOf = [&](const std::string& m) -> const MapEnt* { auto it = ents.find(m); return it == ents.end() ? nullptr : &it->second; };
    if (S.mapTrails) {      // history trail per station
        for (const auto& kv : by) {
            const MapEnt* e = entOf(kv.first);
            if (!e || !passes(*e)) continue;
            const ImU32 col = catColour(e->cat, true, 200);
            ImVec2 prev; bool any = false;
            for (const auto* f : kv.second) {
                const ImVec2 q = adsbmap::project(f->lat, f->lon);
                if (any) dl->AddLine(prev, q, col, 1.6f);
                if (!any || f != kv.second.back()) dl->AddCircleFilled(q, 2.5f * gUi, col);
                prev = q; any = true;
            }
        }
    }
    if (S.mapLinks) {       // a call from one station to another that both have a position: the line from the sender to the called station
        const int64_t now = (int64_t)time(nullptr);
        for (const auto& l : S.links) {
            const MapEnt* f = entOf(l.from); const MapEnt* d = entOf(l.to);
            if (!f || !d || !f->pos || !d->pos || !passes(*f) || !passes(*d)) continue;
            const float fade = (float)std::max(0.25, 1.0 - (double)std::max<int64_t>(0, now - l.time) / 3600.0);
            dl->AddLine(adsbmap::project(f->lat, f->lon), adsbmap::project(d->lat, d->lon), l.distress ? IM_COL32(235, 70, 70, (int)(230 * fade)) : IM_COL32(240, 240, 240, (int)(170 * fade)), 1.4f);
        }
    }
    for (const auto& kv : ents) {
        const MapEnt& e = kv.second;
        if (!e.pos || !passes(e)) continue;
        const ImVec2 q = adsbmap::project(e.lat, e.lon);
        if (q.x < p0.x - 20 || q.x > p0.x + sz.x + 20 || q.y < p0.y - 20 || q.y > p0.y + sz.y + 20) continue;
        const bool sel = S.mapSel == e.mmsi;
        const ImU32 col = catColour(e.cat, e.heard || e.cat != kCatCoast);
        const float r = (e.cat == kCatCoast ? 4.5f : e.heard ? 5.f : 3.5f) * gUi;
        if (e.cat == kCatCoast) dl->AddRectFilled(ImVec2(q.x - r, q.y - r), ImVec2(q.x + r, q.y + r), col);
        else dl->AddCircleFilled(q, r, col);
        if (e.heard) dl->AddCircle(q, r + 4 * gUi, col, 20, 1.6f);       // heard: a ring (and a full colour for a coast station)
        if (sel) dl->AddCircle(q, r + 8 * gUi, IM_COL32(255, 255, 255, 255), 24, 2.f);
        if ((S.mapLabels && (S.map.zoom >= 8 || e.heard)) || sel) {
            const std::string lb = e.name.empty() ? e.mmsi : e.name;
            dl->AddText(ImVec2(q.x + r + 3 * gUi, q.y - 7 * gUi), IM_COL32(adsbmap::overlayText() >> 16 & 255, adsbmap::overlayText() >> 8 & 255, adsbmap::overlayText() & 255, e.heard ? 255 : 150), lb.c_str());
        }
        if (std::fabs(mp.x - q.x) < 9 * gUi && std::fabs(mp.y - q.y) < 9 * gUi) {
            hoverMmsi = e.mmsi;
            hover = e.mmsi + (e.name.empty() ? "" : "  " + e.name) + "\n" + (e.country.empty() ? "" : e.country + ", ") + tr(kCatName[e.cat]);
            hover += "\n" + (e.heard ? std::string(tr("last heard")) + " " + ageText(e.last) + " " + tr("ago") + " (" + std::to_string(e.count) + ")" : std::string(tr("not heard yet")));
            auto it = by.find(e.mmsi);
            if (it != by.end()) hover += "\n" + it->second.back()->note + "  " + std::to_string(it->second.size()) + " " + tr("positions");
        }
    }
    dl->PopClipRect();
    if (!hover.empty() && ImGui::IsMouseHoveringRect(p0, ImVec2(p0.x + sz.x, p0.y + sz.y))) {
        ImGui::SetTooltip("%s", hover.c_str());
        if (ImGui::IsMouseClicked(0)) S.mapSel = hoverMmsi;
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 6 * gUi, p0.y + 6 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(24 * gUi, 0))) S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom in"));
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(24 * gUi, 0))) S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom out"));
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never the position itself");
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + sz.y + 4 * gUi));

    // ---- table: heard ones first (newest first), then the rest by number; a click centres the map
    std::vector<const MapEnt*> rows;
    for (const auto& kv : ents) if (passes(kv.second)) rows.push_back(&kv.second);
    std::sort(rows.begin(), rows.end(), [](const MapEnt* x, const MapEnt* y) {
        if ((x->cat == kCatDistress) != (y->cat == kCatDistress)) return x->cat == kCatDistress;
        if (x->heard != y->heard) return x->heard;
        if (x->last != y->last) return x->last > y->last;
        return x->mmsi < y->mmsi;
    });
    const ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##dscmapt", 7, fl, ImVec2(0, std::max(60.f * gUi, ImGui::GetContentRegionAvail().y)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        for (const char* h : {"MMSI", "Name", "Country", "Type", "Last heard", "Calls", "Position"}) ImGui::TableSetupColumn(tr(h));
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)rows.size());
        while (clip.Step()) for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const MapEnt& e = *rows[(size_t)i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(catColour(e.cat, e.heard || e.cat != kCatCoast)));
            if (ImGui::Selectable(e.mmsi.c_str(), S.mapSel == e.mmsi, ImGuiSelectableFlags_SpanAllColumns)) {
                S.mapSel = e.mmsi;
                if (e.pos) { S.map.lat = e.lat; S.map.lon = e.lon; if (S.map.zoom < 7) S.map.zoom = 7; }
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.name.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.country.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.cat == kCatDistress ? tr("distress") : e.kind.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.heard ? ageText(e.last).c_str() : "-");
            ImGui::TableNextColumn(); if (e.heard) ImGui::Text("%d", e.count);
            ImGui::TableNextColumn();
            if (e.pos) ImGui::Text("%.4f, %.4f", e.lat, e.lon); else ImGui::TextDisabled("%s", tr("no position"));
        }
        ImGui::EndTable();
    }
}

void tab(App& a) {
    loadState();
    if (const int svc = subService(a)) {   // a protocol entry: its own view and the tuning view
        static const char* const names[] = {"", "NAVTEX", "DSC", "Fax"};
        if (svc == 3) {   // weather fax: the picture, the stations and their schedule, the tuning aid
            int k = S.view == 4 ? 1 : S.view == 3 ? 2 : S.view == 7 ? 3 : 0;
            subNav("marv3", k, {"Fax", "Stations", "Tuning", "Gallery"});
            S.view = k == 1 ? 4 : k == 2 ? 3 : k == 3 ? 7 : 2;
        } else {
        int k = S.view == 3 ? 1 : S.view == 5 ? 2 : S.view == 6 ? 3 : 0;
        if (svc == 2) subNav("marv1", k, {names[svc], "Tuning", "Bands", "Map"});
        else { if (k == 3) k = 0; subNav("marv1", k, {names[svc], "Tuning", "Bands"}); }
        S.view = k == 1 ? 3 : k == 2 ? 5 : k == 3 ? 6 : svc - 1;
        }
    } else {
    const int act = live(a) ? a.rx.marine.serviceActive : 0;
    if (S.follow && act != S.lastActive && act != 0) S.view = act == 1 ? 0 : act == 3 ? 2 : 1;
    S.lastActive = act;
    const int before = S.view;
    subNav("marv", S.view, {"NAVTEX", "DSC", "Fax", "Tuning", "Fax stations", "Bands", "DSC map", "Fax gallery"});
    if (S.view != before) S.follow = false;
    ImGui::SameLine(0, 20 * gUi);
    ImGui::TextDisabled("Service"); ImGui::SameLine();
    static const char* const sv[] = {"auto", "NAVTEX", "DSC", "weather fax"};
    ImGui::SetNextItemWidth(120 * gUi);
    if (ImGui::Combo("##marsv", &S.service, sv, 4)) plat::prefs().setI("marineService", S.service);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("auto: by the frequency (490, 518, 4209.5 kHz NAVTEX; the DSC frequencies; otherwise fax).\nWith the test signal or a recording every decoder runs.");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("clear")) {
        a.engine.marine().clearMessages();
        if (S.faxShown) { a.engine.marine().clearFax(); marineFaxClearUi(); }   // the Fax view is open: this button clears its picture too
    }
    if (ImGui::IsItemHovered() && S.faxShown) ImGui::SetTooltip("%s", tr("Clears the NAVTEX and DSC lists and the weather fax picture."));
    S.faxShown = false;       // faxView() sets it again in this frame when the Fax view is drawn
    if (S.view == 0 || S.view == 1) {   // NAVTEX and DSC: hear the FSK tones (fax has its own control in its view)
        ImGui::SameLine(0, 14 * gUi);
        listenControls(a, S.listen, "faxListen", "marfsk", "Play the sideband from the dial up on the sound card (the FSK tones); decoding is the same either way");
    }
    if (subService(a) == 0 || subService(a) == 1) {   // saving is for NAVTEX messages
    sameLineIf(ImGui::GetFrameHeight() + ImGui::CalcTextSize("Save messages").x + 100 * gUi, 20 * gUi);
    if (ImGui::Checkbox("Save messages", &S.saveNav)) { plat::prefs().setB("marineSaveNavtex", S.saveNav); S.saver.clearError(); S.saveErr.clear(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Write each NAVTEX message to a text file as soon as it has been received (also one cut short, marked as such).\nOne file per message, in a folder per day.");
    sameLineIf(ImGui::CalcTextSize("Show folder").x + 16 * gUi);
    if (ImGui::SmallButton("Show folder")) {
        const std::string d = plat::dataDir() + "/navtex";
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(d.c_str())), ec);
        dect2::openUrl(d);
    }
    }
    if (!S.saveErr.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::badRed(), "%s", S.saveErr.c_str()); ImGui::PopTextWrapPos(); }
    switch (S.view) {
    case 0: navtexView(a); break;
    case 1: dscView(a); break;
    case 2: faxView(a); break;
    case 4: stationsView(a); break;
    case 5: bandsView(a); break;
    case 6: mapView(a); break;
    case 7: wefaxgal::galleryView(a); break;
    default: tuningPanel(a); break;
    }
}

void list(App& a) {
    loadState();
    if (!live(a)) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see messages"); ImGui::PopTextWrapPos(); } return; }
    const dect2::MarineTelemetry& t = a.rx.marine;
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%llu NAVTEX, %llu DSC", (unsigned long long)t.navtexCount, (unsigned long long)t.dscCount); ImGui::PopTextWrapPos(); }
    ImGui::BeginChild("##mar_list");
    struct Row { double sec; int kind; size_t i; };
    std::vector<Row> rows;
    const int svc = subService(a);
    if (svc == 0 || svc == 1) for (size_t i = 0; i < t.navtex.size(); i++) rows.push_back({t.navtex[i].rxSec, 0, i});
    if (svc == 0 || svc == 2) for (size_t i = 0; i < t.dsc.size(); i++) rows.push_back({t.dsc[i].rxSec, 1, i});
    std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.sec > y.sec; });
    int n = 0;
    for (const Row& r : rows) {
        if (n++ >= 60) break;
        ImGui::PushID(n);
        if (r.kind == 0) {
            const NavtexMessage& m = t.navtex[r.i];
            subjectBadge(m.subject); ImGui::SameLine(0, 6 * gUi);
            if (ImGui::Selectable((m.header + "  " + m.subjectName).c_str(), false)) { S.navSel = navKey(m); S.view = 0; S.follow = false; }
        } else {
            const DscCall& c = t.dsc[r.i];
            const std::string s = "DSC  " + c.formatName + "  " + (c.fromMmsi.empty() ? "?" : c.fromMmsi);
            if (c.distress) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.5f, 0.45f, 1));
            if (ImGui::Selectable(s.c_str(), false)) { S.dscSelKey = dscKey(c); S.view = 1; S.follow = false; }
            if (c.distress) ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }
    if (svc == 0 || svc == 3) {
        if (!S.faxes.empty()) typo::separatorText("Fax pictures");
        for (size_t i = 0; i < S.faxes.size(); i++) {
            const State::Fax& f = S.faxes[i];
            ImGui::PushID((int)(1000 + i));
            char tb[16]; struct tm tmv; const time_t tt = (time_t)f.wall;
#ifdef _WIN32
            localtime_s(&tmv, &tt);
#else
            localtime_r(&tt, &tmv);
#endif
            strftime(tb, sizeof tb, "%H:%M", &tmv);
            const std::string saveTip = picsave::tooltipText(picui::folder(), "weather-fax");   // what and where
            if (iconFlat(Ic::Save, saveTip.c_str())) {
                if (f.im.width > 0 && f.im.height > 0) {
                    std::vector<uint8_t> rgba((size_t)f.im.width * f.im.height * 4);
                    for (size_t k = 0; k < f.im.pix.size() && k * 4 + 3 < rgba.size(); k++) { rgba[k * 4] = rgba[k * 4 + 1] = rgba[k * 4 + 2] = f.im.pix[k]; rgba[k * 4 + 3] = 255; }
                    picui::saveRgba("weather-fax", (time_t)f.wall, rgba.data(), f.im.width, f.im.height);
                }
            }
            ImGui::SameLine(0, 6 * gUi);
            ImGui::AlignTextToFramePadding();
            if (ImGui::Selectable((std::string(tb) + "  " + std::to_string((int)std::lround(f.khz)) + " kHz  " + std::to_string(f.im.width) + " x " + std::to_string(f.im.height)).c_str(), false)) { S.view = 2; S.follow = false; }
            ImGui::PopID();
        }
        picui::statusLine();
        if (!S.faxes.empty()) picui::settingRow();
        if (!S.faxes.empty()) rows.push_back({0, -1, 0});   // not "nothing decoded yet"
    }
    if (rows.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(t.serviceActive == 3 ? "receiving a fax: see the Fax view" : "nothing decoded yet"); ImGui::PopTextWrapPos(); }
    ImGui::EndChild();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::MarineTelemetry& t = a.rx.marine;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    static const char* const sv[] = {"nothing yet", "NAVTEX", "DSC MF/HF", "weather fax", "DSC VHF"};
    static const char* const ss[] = {"auto", "NAVTEX", "DSC", "weather fax"};
    kv("state", "%s", t.state == 0 ? "searching" : t.state == 1 ? "signal" : "decoding");
    kv("service", "%s (selector: %s)", sv[std::max(0, std::min(4, t.serviceActive))], ss[std::max(0, std::min(3, t.serviceSetting))]);
    kv("locked", "NAVTEX %s, DSC %s", t.navtexLocked ? "yes" : "no", t.dscLocked ? "yes" : "no");
    kv("messages", "%llu NAVTEX, %llu DSC calls", (unsigned long long)t.navtexCount, (unsigned long long)t.dscCount);
    kv("good / bad", "%llu / %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    if (t.toneHighHz != 0 || t.toneLowHz != 0) {
        kv("FSK tones", "%+.0f / %+.0f Hz, shift %.0f Hz", t.toneLowHz, t.toneHighHz, t.shiftHz);
        kv("symbol rate", "%.2f baud", t.baudEst);
    }
    kv("signal", "%.1f dB SNR in 3 kHz, carrier error %+.0f Hz", t.snrDb, t.cfoHz);
    kv("fax", "state %d, %d lines, IOC %d, %d lpm, slant %+.1f ppm", t.fax.state, t.fax.lines, t.fax.ioc, t.fax.lpm, t.fax.slantPpm);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const dect2::MarineTelemetry& t = a.rx.marine;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Signal", !on ? 0 : t.state >= 1 ? 1 : 0); flowNext(12 * gUi);
    lamp("Decoding", !on ? 0 : t.state == 2 ? 1 : t.dataValid ? 2 : 0); flowNext(10 * gUi);
    bool distress = false;
    for (const auto& c : t.dsc) distress |= c.distress;
    lamp("Distress", !on ? 0 : distress ? 3 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(on ? dect2::marineSummary(t).c_str() : run ? "starting" : "stopped");
}

void summary(const App& a, std::string& l1, std::string& l2) {
    const SubMode* sm = curSub(a);
    l1 = sm ? sm->name : "Marine";
    if (live(a)) l2 = dect2::marineSummary(a.rx.marine);
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    ImGui::TextDisabled("send"); ImGui::SameLine(); ImGui::SetNextItemWidth(130 * gUi);
    static const char* const sv[] = {"NAVTEX", "DSC MF/HF", "weather fax", "DSC VHF ch 70"};
    int s = std::max(1, std::min(4, sc.modeOpt[0] ? sc.modeOpt[0] : 1)) - 1;
    const int only = subService(a);   // a protocol entry sends only its own service (DSC: MF/HF or VHF)
    if (only == 2) { int v = s == 3 ? 1 : 0; static const char* const dv[] = {"DSC MF/HF", "DSC VHF ch 70"}; if (ImGui::Combo("##msv", &v, dv, 2)) { sc.modeOpt[0] = v ? 4 : 2; changed = true; } }
    else if (only) ImGui::TextDisabled("%s", sv[only == 3 ? 2 : 0]);
    else if (ImGui::Combo("##msv", &s, sv, 4)) { sc.modeOpt[0] = s + 1; changed = true; }
    flowNext();
    bool fade = sc.modeOpt[1] == 1;
    if (ImGui::Checkbox("HF fading", &fade)) { sc.modeOpt[1] = fade ? 1 : 0; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Two paths 1 ms apart, each fading with a 0.5 Hz Doppler spread.");
    flowNext(); ImGui::TextDisabled("SNR"); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    float snr = (float)sc.snrDb;
    if (ImGui::SliderFloat("##msnr", &snr, -10.f, 40.f, "%.0f dB")) { sc.snrDb = snr; changed = true; }
    flowNext(); ImGui::TextDisabled("mistuned"); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    float mt = (float)sc.modeVal[0];
    if (ImGui::SliderFloat("##mmt", &mt, -100.f, 100.f, "%+.0f Hz")) { sc.modeVal[0] = mt; changed = true; }
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const dect2::MarineTelemetry& t = a.rx.marine;
    out.push_back({"SNR  dB", "%.0f", t.snrDb, -10, 40, t.snrDb >= 10 ? 1 : t.snrDb >= 0 ? 2 : 3});
    out.push_back({"NAVTEX", "%.0f", (double)t.navtexCount, 0, 20, t.navtexCount ? 1 : 0});
    out.push_back({"DSC", "%.0f", (double)t.dscCount, 0, 20, t.dscCount ? 1 : 0});
    out.push_back({"FAX LINES", "%.0f", (double)t.fax.lines, 0, 1500, t.fax.lines ? 1 : 0});
}

} // namespace

extern const ModeUi kMarineUi;
const ModeUi kMarineUi = {
    .sideTitle = "MESSAGES",
    .tabName = "Messages",
    .tabIcon = Ic::Doc,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .panels = panels,
    .status = status,
    .summary = summary,
    .synth = synth,
    .tick = tick,
    .meters = meters,
};

// the last weather fax picture for the results file (results_ui.cpp): shown again until a new one comes in
// the picture on screen and the saved copy: empty (the engine side is MarineReceiver::clearFax). Also used by the top bar's Clear all.
void marineFaxClearUi() {
    S.last = dect2::FaxImage();
    S.imgW = S.imgH = 0;
    S.zoom = 0; S.pan = ImVec2(0, 0);
    S.saveMsg.clear();
    S.faxes.clear();
}
std::string marineFaxSave() { return dect2::results::pack(S.last); }
bool marineFaxLoad(const std::string& blob) {
    dect2::FaxImage im;
    if (!dect2::results::unpack(blob, im)) return false;
    if (im.width <= 0 || im.height <= 0 || im.pix.size() < (size_t)im.width * im.height) return true;
    S.last = std::move(im);
    S.texW = 0;                                         // the texture is made on the next frame (marineFaxTexture)
    return true;
}
void marineFaxTexture() {
    if (!gGfx || S.last.width <= 0 || S.img) return;
    presentLast(true);
}

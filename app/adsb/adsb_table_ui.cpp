// ADS-B screens: the aircraft table, the selected aircraft's detail, the statistics view, the sources panel and the
// unit controls.
// Split from adsb_ui.cpp; the shared state is declared in adsb_internal.h.
#include "transcribe/transcribe_app.h"
#include "../panels/photo_links_ui.h"
#include "adsb_internal.h"
#include "adsb_acars.h"

using namespace adsbui;

namespace adsbui {

// one number or a dash
std::string num(bool has, const char* fmt, double v) { if (!has) return "-"; char b[32]; snprintf(b, sizeof b, fmt, v); return b; }

std::string uAlt(double ft) { return au::fmt(au::altOut(S.units, ft), 0); }

std::string uSpd(double kt) { return au::fmt(au::speedOut(S.units, kt), au::speedDecimals(S.units)); }

std::string uDist(double nm, int dec) { return au::fmt(au::distOut(S.units, nm), dec); }

std::string uVr(double fpm) { return au::fmt(au::vrateOut(S.units, fpm), au::vrateDecimals(S.units), true); }

std::string dash(bool has, const std::string& v) { return has ? v : "-"; }

// the units toggle (aviation / metric, and km/h or m/s), saved in the prefs
void unitsControls() {
    bool ch = false;
    ImGui::TextDisabled("Units");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130 * gUi);
    int sys = S.units.metric() ? 1 : 0;
    if (comboTr("##adsbunits", &sys, {N_("Aviation (kt, ft, nm)"), N_("Metric")})) { S.units.sys = sys ? au::System::Metric : au::System::Aviation; ch = true; }
    if (S.units.metric()) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70 * gUi);
        int sp = S.units.spd == au::Speed::MS ? 1 : 0;
        if (ImGui::Combo("##adsbspd", &sp, "km/h\0m/s\0")) { S.units.spd = sp ? au::Speed::MS : au::Speed::KmH; ch = true; }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Aviation: knots, feet, nautical miles, ft/min. Metric: km/h or m/s, metres, km, m/min. Only what is shown and exported changes; SBS and the feeds stay in aviation units.");
    if (ch) { plat::prefs().setI("adsbUnits", S.units.metric() ? 1 : 0); plat::prefs().setI("adsbSpeedUnit", S.units.spd == au::Speed::MS ? 1 : 0); plat::prefs().flush(); }
}

// "Display": altitude colours, the width of the track lines, and when a silent aircraft leaves the table and the map
void displayControls() {
    namespace ad = dect2::adsbd;
    if (ImGui::Button(tr("Display"))) ImGui::OpenPopup("##adsbdisp");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("How the map and the table show the aircraft: altitude colours, track width, and when to drop an aircraft that is no longer heard"));
    if (!ImGui::BeginPopup("##adsbdisp")) return;
    bool ch = false;
    if (ImGui::Checkbox(tr("Colour by altitude"), &S.disp.altColour)) ch = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Aircraft and their tracks: low is warm, high is cool, on the ground is grey. Off: white, grey when the signal is old."));
    if (ImGui::Checkbox(tr("Labels on the map"), &S.disp.labels)) ch = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The callsign, altitude and speed boxes beside the aircraft. Off: cleaner in crowded areas; the selected aircraft still shows its box."));
    ImGui::SetNextItemWidth(140 * gUi);
    if (ImGui::SliderInt(tr("Track width (px)"), &S.disp.trackWidth, ad::kMinTrackWidth, ad::kMaxTrackWidth)) { S.disp.trackWidth = ad::clampTrackWidth(S.disp.trackWidth); ch = true; }
    bool never = S.disp.dropSec == 0;
    if (ImGui::Checkbox(tr("Never drop silent aircraft"), &never)) { S.disp.dropSec = never ? 0 : ad::kDefaultDropSec; ch = true; }
    if (!never) {
        int sec = S.disp.dropSec;
        ImGui::SetNextItemWidth(140 * gUi);
        if (ImGui::InputInt(tr("Drop after (s)"), &sec, 10, 60)) { S.disp.dropSec = std::max(ad::kMinDropSec, ad::clampDropSec(sec)); ch = true; }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("An aircraft that has not been heard for this long leaves the table and the map. Never keeps them until the list is cleared (the table holds at most 2000)."));
    if (ch) {
        plat::Prefs& p = plat::prefs();
        p.setB(ad::kPrefAltColour, S.disp.altColour); p.setB(ad::kPrefLabels, S.disp.labels); p.setI(ad::kPrefTrackWidth, S.disp.trackWidth); p.setI(ad::kPrefDropSec, S.disp.dropSec);
        p.flush();
    }
    ImGui::EndPopup();
}

std::string typeText(const AdsbAircraft& ac) {
    if (ac.category.size() < 2) return "";
    const int set = ac.category[0] - 'A', ca = ac.category[1] - '0';
    const std::string n = (set >= 0 && set <= 3 && ca >= 0 && ca <= 7) ? dect2::adsb::categoryName(4 - set, ca) : "";
    if (n.empty() || n == "No information") return ac.category;
    return ac.category + " " + n;
}

void aircraftTable(App& a, bool compact) {
    const AdsbTelemetry& t = a.rx.adsb;
    const bool on = live(a);
    const int cols = compact ? 7 : 27;
    if (!compact) {
        static std::string note;
        resexp::Table x; x.cols = {"time", "frequency_hz", "mode", "icao", "registration", "country", "operator", "callsign", "type", "source", au::altCol(S.units), "geo_" + std::string(au::altCol(S.units)), au::speedCol(S.units), "speed_kind", "ias_kt", "mach", "heading_deg", "heading_kind", au::vrateCol(S.units), "lat", "lon", "fix", au::distCol(S.units), "bearing_deg", "squawk", "selected_" + std::string(au::altCol(S.units)), "emergency", "nacp", "adsb_version", "tisb", "ground", "messages", "age_s", "pos_age_s", "level_dbfs"};
        const std::string now = resexp::stamp();
        if (on) for (const auto& ac : t.aircraft)
            x.add({now, "1090000000", "ADS-B", icaoText(ac.icao), registrationOf(ac.icao), countryCode(ac.icao), operatorOf(ac.icao), ac.callsign, typeOf(ac), sourceName(t, ac.srcId), ac.ground ? "ground" : (ac.hasAlt ? uAlt(ac.altFt) : ""), ac.hasGeoAlt ? uAlt(ac.geoAltFt) : "",
                   ac.hasSpeed ? uSpd(ac.speedKt) : "", ac.hasSpeed ? (ac.speedKind == 0 ? "gs" : ac.speedKind == 1 ? "ias" : "tas") : "", ac.hasIas ? std::to_string(ac.iasKt) : "", ac.hasMach ? resexp::num(ac.mach, 2) : "",
                   ac.hasHeading ? resexp::num(ac.headingDeg, 0) : "", ac.hasHeading ? (ac.headingIsTrack ? "track" : "heading") : "",
                   ac.hasVrate ? au::fmt(au::vrateOut(S.units, ac.vrateFpm), au::vrateDecimals(S.units)) : "", ac.hasPos ? resexp::num(ac.lat, 6) : "", ac.hasPos ? resexp::num(ac.lon, 6) : "",
                   ac.hasPos ? (ac.posKind == 2 ? "global" : ac.posKind == 3 ? "local+" : "local") : "", ac.hasRange ? uDist(ac.distNm) : "", ac.hasRange ? resexp::num(ac.bearingDeg, 0) : "",
                   ac.hasSquawk ? std::to_string(ac.squawk) : "", ac.hasSelAlt ? uAlt(ac.selAltFt) : "", ac.emergency ? dect2::adsb::emergencyName(ac.emergency) : "",
                   ac.nacp >= 0 ? std::to_string(ac.nacp) : "", ac.adsbVersion >= 0 ? std::to_string(ac.adsbVersion) : "", ac.tisb ? "1" : "", ac.ground ? "1" : "",
                   std::to_string(ac.messages), resexp::num(ac.ageSec, 0), ac.hasPos ? resexp::num(ac.posAgeSec, 0) : "", resexp::num(ac.levelDbfs, 0)});
        resexp::saveButton(x, "adsb", note, "svadsb");
        ImGui::SameLine();
        unitsControls();
        ImGui::SameLine(); displayControls();
        if (!note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", note.c_str()); }
    }
    if (!ImGui::BeginTable(compact ? "##acs" : "##acf", cols, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("ICAO", ImGuiTableColumnFlags_WidthFixed, 62 * gUi); ImGui::TableSetupColumn("Callsign", ImGuiTableColumnFlags_WidthFixed, 72 * gUi);
    ImGui::TableSetupColumn("Reg", ImGuiTableColumnFlags_WidthFixed, 70 * gUi);
    if (!compact) { ImGui::TableSetupColumn("Ctry", ImGuiTableColumnFlags_WidthFixed, 34 * gUi); ImGui::TableSetupColumn("Operator", ImGuiTableColumnFlags_WidthFixed, 110 * gUi); }
    const std::string hAlt = std::string("Alt ") + au::altUnit(S.units), hSpd = std::string("Spd ") + au::speedUnit(S.units), hVs = std::string("V/S ") + au::vrateUnit(S.units), hDist = std::string("Dist ") + au::distUnit(S.units);
    const std::string hGeo = std::string("GNSS ") + au::altUnit(S.units), hSel = std::string("Sel ") + au::altUnit(S.units);
    ImGui::TableSetupColumn(hAlt.c_str());
    if (!compact) {
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 140 * gUi);
        ImGui::TableSetupColumn(hSpd.c_str()); ImGui::TableSetupColumn("Hdg"); ImGui::TableSetupColumn(hVs.c_str());
        ImGui::TableSetupColumn("Lat", ImGuiTableColumnFlags_WidthFixed, 78 * gUi); ImGui::TableSetupColumn("Lon", ImGuiTableColumnFlags_WidthFixed, 82 * gUi);
    } else {
        ImGui::TableSetupColumn("Lat", ImGuiTableColumnFlags_WidthFixed, 70 * gUi); ImGui::TableSetupColumn("Lon", ImGuiTableColumnFlags_WidthFixed, 74 * gUi);
    }
    ImGui::TableSetupColumn(hDist.c_str());
    if (!compact) {
        ImGui::TableSetupColumn("Brg"); ImGui::TableSetupColumn("Fix"); ImGui::TableSetupColumn("Sqk");
        ImGui::TableSetupColumn(hGeo.c_str()); ImGui::TableSetupColumn("IAS"); ImGui::TableSetupColumn("Mach"); ImGui::TableSetupColumn(hSel.c_str());
        ImGui::TableSetupColumn("Emg"); ImGui::TableSetupColumn("NACp"); ImGui::TableSetupColumn("Ver"); ImGui::TableSetupColumn("Msgs"); ImGui::TableSetupColumn("Age s"); ImGui::TableSetupColumn("dBFS"); ImGui::TableSetupColumn("Source");
    }
    ImGui::TableHeadersRow();
    std::vector<const AdsbAircraft*> rows;   // by address, not by last heard: the rows do not move about
    if (on) for (const auto& x : t.aircraft) rows.push_back(&x);
    std::sort(rows.begin(), rows.end(), [](const AdsbAircraft* x, const AdsbAircraft* y) { return x->icao < y->icao; });
    for (const AdsbAircraft* pac : rows) {
        const AdsbAircraft& ac = *pac;
        ImGui::TableNextRow();
        const bool emg = ac.emergency > 0;
        if (emg) ImGui::PushStyleColor(ImGuiCol_Text, pal::badRed());
        ImGui::TableNextColumn();
        char id[24]; snprintf(id, sizeof id, "%s##%u", icaoText(ac.icao).c_str(), ac.icao);
        if (ImGui::Selectable(id, S.sel == ac.icao, ImGuiSelectableFlags_SpanAllColumns)) S.sel = ac.icao;
        if (ImGui::BeginPopupContextItem()) { photolinks::aircraftMenuItems(ac.icao, registrationOf(ac.icao), ac.callsign); ImGui::EndPopup(); }
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ac.callsign.empty() ? "-" : ac.callsign.c_str());
        if (const int nm = acarslog::count(ac.icao)) {   // ACARS messages from this aircraft: a mark after the callsign
            ImGui::SameLine(0, 4 * gUi); ImGui::TextColored(kWarn, "*");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("%d ACARS messages"), nm);
        }
        { const std::string rg = registrationOf(ac.icao); ImGui::TableNextColumn(); ImGui::TextUnformatted(rg.empty() ? "-" : rg.c_str()); }
        if (!compact) {
            const std::string cc = countryCode(ac.icao), op = operatorOf(ac.icao);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(cc.empty() ? "-" : cc.c_str());
            if (ImGui::IsItemHovered() && !cc.empty()) ImGui::SetTooltip("%s", dect2::adsbCountry(ac.icao).name);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(op.empty() ? "-" : op.c_str());
        }
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ac.ground ? "ground" : dash(ac.hasAlt, uAlt(ac.altFt)).c_str());
        if (!compact) {
            const std::string ty = typeOf(ac);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(ty.empty() ? "-" : ty.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(dash(ac.hasSpeed, uSpd(ac.speedKt)).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(num(ac.hasHeading, "%.0f", ac.headingDeg).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(dash(ac.hasVrate, uVr(ac.vrateFpm)).c_str());
        }
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ac.hasPos ? num(true, "%.4f", ac.lat).c_str() : "-");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ac.hasPos ? num(true, "%.4f", ac.lon).c_str() : "-");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(dash(ac.hasRange, uDist(ac.distNm)).c_str());
        if (!compact) {
            ImGui::TableNextColumn(); ImGui::TextUnformatted(num(ac.hasRange, "%.0f", ac.bearingDeg).c_str());
            ImGui::TableNextColumn();
            if (ac.hasPos) ImGui::TextUnformatted(ac.posKind == 2 ? "global" : ac.posKind == 3 ? "local+" : "local");
            else if ((ac.cprHave & 3) == 3) ImGui::TextUnformatted("pair");
            else if (ac.cprHave & 1) ImGui::TextUnformatted("even");
            else if (ac.cprHave & 2) ImGui::TextUnformatted("odd");
            else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); if (ac.hasSquawk) ImGui::Text("%04d", ac.squawk); else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(dash(ac.hasGeoAlt, uAlt(ac.geoAltFt)).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(ac.hasIas ? std::to_string(ac.iasKt).c_str() : "-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(num(ac.hasMach, "%.2f", ac.mach).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(dash(ac.hasSelAlt, uAlt(ac.selAltFt)).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(ac.emergency ? dect2::adsb::emergencyName(ac.emergency) : "-");
            ImGui::TableNextColumn(); if (ac.nacp >= 0) ImGui::Text("%d", ac.nacp); else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); if (ac.adsbVersion >= 0) ImGui::Text("%d", ac.adsbVersion); else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); ImGui::Text("%u", ac.messages);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", ac.ageSec);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", ac.levelDbfs);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(sourceName(t, ac.srcId).c_str());
        }
        if (emg) ImGui::PopStyleColor();
    }
    ImGui::EndTable();
}

static const char* catText(dect2::AcarsCat c) {
    switch (c) {
    case dect2::AcarsCat::Free: return tr("Free text");
    case dect2::AcarsCat::Crew: return tr("Crew");
    case dect2::AcarsCat::Ops: return tr("Operations");
    case dect2::AcarsCat::Company: return tr("Company");
    case dect2::AcarsCat::Maint: return tr("Maintenance");
    case dect2::AcarsCat::Fault: return tr("Fault");
    default: return "";
    }
}

// The ACARS messages this aircraft sent (the interesting ones; "Show all messages" adds link tests, acknowledgements and the like).
static void acarsLogSection(const App& a, const AdsbAircraft& ac) {
    const std::vector<dect2::AcarsLogEntry> v = acarslog::entries(ac.icao, ac.callsign, acarslog::showAll());
    ImGui::Dummy(ImVec2(0, 4 * gUi));
    ImGui::Separator();
    ImGui::Text("%s%s", tr("ACARS messages"), v.empty() ? "" : (" (" + std::to_string(v.size()) + ")").c_str());
    ImGui::Checkbox(tr("Show all messages"), &acarslog::showAll());
    if (!v.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Copy"))) ImGui::SetClipboardText(acarslog::asText(v).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Copy these messages as text"));
    }
    if (v.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", tr("No ACARS messages heard from this aircraft")); ImGui::PopTextWrapPos(); return; }
    ImGui::BeginChild("##acarslog", ImVec2(0, 220 * gUi), true);
    ImGui::PushFont(a.mono, 0);
    for (const auto& e : v) {
        const char* cat = catText(e.cat);
        ImGui::TextDisabled("%s  %s  %.3f MHz  %s %s", acarslog::timeText(e.wall).c_str(), e.mode.c_str(), e.freqMHz, e.label.c_str(), cat);
        ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(e.text.empty() ? "-" : e.text.c_str()); ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 2 * gUi));
    }
    ImGui::PopFont();
    ImGui::EndChild();
}

void detail(const App& a) {
    // the readout of the selected aircraft is refreshed once a second and always has the same rows, so that it does not jump about
    static AdsbAircraft shown; static bool have = false; static double at = 0; static uint32_t of = 0;
    const AdsbTelemetry& t = a.rx.adsb;
    const AdsbAircraft* cur = nullptr;
    if (live(a)) for (const auto& x : t.aircraft) if (x.icao == S.sel) cur = &x;
    if (!cur) { have = false; ImGui::PushTextWrapPos(0); ImGui::TextDisabled("click an aircraft"); ImGui::PopTextWrapPos(); return; }
    const double now = ImGui::GetTime();
    if (!have || of != cur->icao || now - at > 1.0) { shown = *cur; have = true; of = cur->icao; at = now; }
    const AdsbAircraft* ac = &shown;
    auto kv = [&](const char* k, const std::string& v) { ImGui::TextDisabled("%s", k); kvColumn(96 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(v.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    char b[96];
    kv("ICAO", icaoText(ac->icao) + (ac->icao >> 24 ? " (not ICAO)" : ""));
    { const std::string rg = registrationOf(ac->icao); kv("registration", rg.empty() ? "-" : rg); }
    { const dect2::AdsbCountry c = ac->icao >> 24 ? dect2::AdsbCountry{"", "unknown"} : dect2::adsbCountry(ac->icao); kv("country", c.iso[0] ? std::string(c.iso) + "  " + c.name : "-"); }
    {
        const dect2::AdsbRegEntry* e = ac->icao >> 24 ? nullptr : S.db.find(ac->icao);
        std::string ty = e ? e->type : "";
        if (ac->category.size() >= 2) {
            const int set = ac->category[0] - 'A', ca = ac->category[1] - '0';
            const std::string n = (set >= 0 && set <= 3 && ca >= 0 && ca <= 7) ? dect2::adsb::categoryName(4 - set, ca) : "";
            ty += (ty.empty() ? "" : ", ") + ac->category + (n.empty() ? "" : "  " + n);
        }
        kv("type", ty.empty() ? "-" : ty);   // database type, and the ADS-B emitter category (the signal does not send a type designator such as B738)
        kv("operator", e && !e->operatorName.empty() ? e->operatorName : "-");
    }
    kv("source", sourceName(a.rx.adsb, ac->srcId));
    photolinks::aircraftButton(ac->icao, registrationOf(ac->icao), ac->callsign);
    kv("callsign", ac->callsign.empty() ? "-" : ac->callsign);
    if (ac->hasPos) snprintf(b, sizeof b, "%.4f  %.4f", ac->lat, ac->lon); else snprintf(b, sizeof b, "-");
    kv("position", b);
    {
        const char* fx = "-";
        if (ac->hasPos) fx = ac->posKind == 2 ? "global" : ac->posKind == 3 ? "local, confirmed" : "local";
        else if ((ac->cprHave & 3) == 3) fx = "both even and odd CPR, no fix yet";
        else if (ac->cprHave & 1) fx = "even CPR only";
        else if (ac->cprHave & 2) fx = "odd CPR only";
        kv("fix", fx);
    }
    if (ac->hasRange) snprintf(b, sizeof b, "%s %s at %.0f deg", uDist(ac->distNm).c_str(), au::distUnit(S.units), ac->bearingDeg); else snprintf(b, sizeof b, "-");
    kv("range", b);
    kv("altitude", ac->ground ? "on the ground" : ac->hasAlt ? uAlt(ac->altFt) + " " + au::altUnit(S.units) : "-");
    kv("GNSS height", ac->hasGeoAlt ? uAlt(ac->geoAltFt) + " " + au::altUnit(S.units) : "-");
    kv("speed", ac->hasSpeed ? uSpd(ac->speedKt) + " " + au::speedUnit(S.units) + (ac->speedKind == 0 ? " ground" : ac->speedKind == 1 ? " ias" : " tas") : "-");
    kv(ac->headingIsTrack ? "track" : "heading", ac->hasHeading ? num(true, "%.0f deg", ac->headingDeg) : "-");
    kv("vertical", ac->hasVrate ? uVr(ac->vrateFpm) + " " + au::vrateUnit(S.units) : "-");
    if (ac->hasSquawk) snprintf(b, sizeof b, "%04d", ac->squawk); else snprintf(b, sizeof b, "-");
    kv("squawk", b);
    kv("selected alt", ac->hasSelAlt ? uAlt(ac->selAltFt) + " " + au::altUnit(S.units) : "-");
    kv("mach", ac->hasMach ? num(true, "%.2f", ac->mach) : "-");
    if (ac->hasWindDer) snprintf(b, sizeof b, "%03.0f/%s %s derived", ac->windDerDir, uSpd(ac->windDerKt).c_str(), au::speedUnit(S.units));
    else if (ac->hasWindRep) snprintf(b, sizeof b, "%03.0f/%s %s reported", ac->windRepDir, uSpd(ac->windRepKt).c_str(), au::speedUnit(S.units));
    else snprintf(b, sizeof b, "-");
    if (ac->hasWindDer && ac->hasWindRep) { char r[48]; snprintf(r, sizeof r, "\n%03.0f/%s %s reported", ac->windRepDir, uSpd(ac->windRepKt).c_str(), au::speedUnit(S.units)); strncat(b, r, sizeof b - strlen(b) - 1); }
    kv("wind", b);
    if (ac->hasSat) snprintf(b, sizeof b, "%+.0f C %s", ac->satC, ac->satDerived ? "derived" : "reported"); else snprintf(b, sizeof b, "-");
    kv("SAT", b);
    if (ac->hasRa) {
        std::string ra = dect2::adsb::raText(ac->raAra, ac->raRac, ac->raTerminated);
        if (ac->raThreat) { snprintf(b, sizeof b, ", threat %06X", (unsigned)ac->raThreat); ra += b; }
        snprintf(b, sizeof b, " (%.0f s ago)", ac->raAgeSec); ra += b;
        ImGui::TextDisabled("ACAS RA"); kvColumn(96 * gUi); ImGui::TextColored(kWarn, "%s", ra.c_str());
    } else kv("ACAS RA", "-");
    if (ac->emergency) snprintf(b, sizeof b, "state %d", ac->emergency); else snprintf(b, sizeof b, "none");
    kv("emergency", b);
    snprintf(b, sizeof b, "%u messages", ac->messages); kv("heard", b);
    snprintf(b, sizeof b, "%.0f dBFS", ac->levelDbfs); kv("level", b);
    acarsLogSection(a, *ac);
    tx::voiceSection(a, ac->icao, ac->callsign, registrationOf(ac->icao));
}

// ---------------------------------------------------------------- range statistics

// farthest position per 10 degree bearing sector (polar plot), histogram of ranges, message rate and aircraft count over time
void statsView(App& a) {
    const AdsbTelemetry& t = a.rx.adsb;
    const dect2::AdsbRangeStats& st = t.stats;
    const bool on = live(a);
    static std::string note;
    {
        resexp::Table x; x.cols = {"kind", "index", "from", "to", "value"};
        const std::string u = au::distUnit(S.units);
        for (int i = 0; i < dect2::kAdsbSectors; i++) x.add({"sector_max_range_" + u, std::to_string(i), std::to_string(i * 10), std::to_string(i * 10 + 10), resexp::num(au::distOut(S.units, st.sectorMaxNm[i]), 1)});
        for (int i = 0; i < dect2::kAdsbRangeBins; i++) x.add({"range_histogram_count_" + u, std::to_string(i), resexp::num(au::distOut(S.units, i * 10.0), 1), i == dect2::kAdsbRangeBins - 1 ? "" : resexp::num(au::distOut(S.units, i * 10.0 + 10), 1), std::to_string(st.rangeHist[i])});
        for (size_t i = 0; i < st.rateSeries.size(); i++) x.add({"messages_per_second", std::to_string(i), std::to_string(i * 10 + 10), "", resexp::num(st.rateSeries[i], 1)});
        for (size_t i = 0; i < st.countSeries.size(); i++) x.add({"aircraft_count", std::to_string(i), std::to_string(i * 10 + 10), "", std::to_string(st.countSeries[i])});
        x.add({"max_altitude_" + std::string(au::altUnit(S.units)), "", "", "", st.hasMaxAlt ? uAlt(st.maxAltFt) : ""});
        x.add({"max_range_" + u, "", "", "", resexp::num(au::distOut(S.units, st.maxRangeNm), 1)});
        resexp::saveButton(x, "adsbstats", note, "svadsbst");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##adsbst") && on) a.engine.adsb().resetStats();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Empty the range statistics (the aircraft stay). They are saved with the session otherwise.");
    ImGui::SameLine();
    unitsControls();
    if (!note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", note.c_str()); }
    if (on && !t.refValid) ImGui::TextColored(kWarn, "Set the antenna position (Tuner section): ranges need it.");
    char b[160];
    snprintf(b, sizeof b, "farthest %s %s   max altitude %s %s   %llu positions counted", uDist(st.maxRangeNm, 0).c_str(), au::distUnit(S.units),
             st.hasMaxAlt ? uAlt(st.maxAltFt).c_str() : "-", au::altUnit(S.units), (unsigned long long)st.positions);
    ImGui::TextUnformatted(b);
    const ImVec2 av = ImGui::GetContentRegionAvail();
    const float side = std::max(120.f, std::min(av.y, av.x * 0.5f));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // polar plot: north up, a ring at a round distance
    {
        ImGui::InvisibleButton("##polar", ImVec2(side, side));
        const ImVec2 c(p0.x + side * 0.5f, p0.y + side * 0.5f);
        const float R = side * 0.5f - 18 * gUi;
        float mx = 10.f;
        for (float v : st.sectorMaxNm) mx = std::max(mx, v);
        const double mxU = au::distOut(S.units, mx);
        const double nice[] = {5, 10, 20, 25, 50, 100, 200, 250, 500};
        double step = 1000;
        for (double n : nice) if (n * 4 >= mxU) { step = n; break; }
        const double top = std::ceil(mxU / step) * step;
        for (double r = step; r <= top + 1e-6; r += step) {
            const float rr = (float)(r / top) * R;
            dl->AddCircle(c, rr, IM_COL32(70, 76, 82, 255), 64);
            char l[24]; snprintf(l, sizeof l, "%.0f", r);
            dl->AddText(ImVec2(c.x + 3 * gUi, c.y - rr - ImGui::GetTextLineHeight()), IM_COL32(130, 136, 142, 255), l);
        }
        for (int d = 0; d < 360; d += 30) {
            const float ang = (float)(d * M_PI / 180.0);
            dl->AddLine(c, ImVec2(c.x + std::sin(ang) * R, c.y - std::cos(ang) * R), IM_COL32(55, 60, 66, 255));
            if (d % 90 == 0) { const char* nm[] = {"N", "E", "S", "W"}; dl->AddText(ImVec2(c.x + std::sin(ang) * (R + 4 * gUi) - 4 * gUi, c.y - std::cos(ang) * (R + 4 * gUi) - 7 * gUi), IM_COL32(190, 195, 200, 255), nm[d / 90]); }
        }
        const ImU32 fill = ImGui::ColorConvertFloat4ToU32(pal::accent(0.45f)), edge = ImGui::ColorConvertFloat4ToU32(pal::accent(0.95f));
        for (int i = 0; i < dect2::kAdsbSectors; i++) {
            const double nm = au::distOut(S.units, st.sectorMaxNm[i]);
            if (nm <= 0) continue;
            const float rr = (float)(nm / top) * R;
            ImVec2 pts[8];
            pts[0] = c;
            for (int k = 0; k < 7; k++) {
                const float ang = (float)((i * 10 + k * 10.0 / 6.0) * M_PI / 180.0);
                pts[1 + k] = ImVec2(c.x + std::sin(ang) * rr, c.y - std::cos(ang) * rr);
            }
            dl->AddConvexPolyFilled(pts, 8, fill);
            dl->AddPolyline(pts + 1, 7, edge, 0, 1.5f);
        }
        if (ImGui::IsItemHovered()) {
            const ImVec2 m = ImGui::GetMousePos();
            const double brg = std::fmod(std::atan2(m.x - c.x, c.y - m.y) * 180.0 / M_PI + 360.0, 360.0);
            const int sec = std::min(dect2::kAdsbSectors - 1, (int)(brg / 10));
            ImGui::SetTooltip("%d - %d deg: %s %s", sec * 10, sec * 10 + 10, uDist(st.sectorMaxNm[sec], 0).c_str(), au::distUnit(S.units));
        }
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    const float cw = std::max(120.f, av.x - side - 16 * gUi), ph = std::max(50.f, (side - 3 * ImGui::GetTextLineHeightWithSpacing()) / 3.f);
    captionFit(cw, "Ranges of the positions");
    {
        const ImVec2 q = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##rhist", ImVec2(cw, ph));
        int last = 0; uint32_t mx = 1;
        for (int i = 0; i < dect2::kAdsbRangeBins; i++) if (st.rangeHist[i]) { last = i; mx = std::max(mx, st.rangeHist[i]); }
        const int nb = std::min(dect2::kAdsbRangeBins, std::max(last + 1, 5));
        const float bw = cw / (float)nb;
        for (int i = 0; i < nb; i++) {
            const float h = (ph - 4) * (float)st.rangeHist[i] / (float)mx;
            dl->AddRectFilled(ImVec2(q.x + i * bw + 1, q.y + ph - h), ImVec2(q.x + (i + 1) * bw - 1, q.y + ph), ImGui::ColorConvertFloat4ToU32(pal::accent(0.8f)));
        }
        if (ImGui::IsItemHovered()) {
            const int i = std::min(nb - 1, std::max(0, (int)((ImGui::GetMousePos().x - q.x) / bw)));
            ImGui::SetTooltip("%s - %s %s: %u positions", uDist(i * 10.0, 0).c_str(), uDist(i * 10.0 + 10, 0).c_str(), au::distUnit(S.units), st.rangeHist[i]);
        }
    }
    captionFit(cw, "Messages per second (a point per 10 s)");
    { std::vector<float> v(st.rateSeries.begin(), st.rateSeries.end()); if (v.empty()) v.push_back(0); ImGui::PlotLines("##strate", v.data(), (int)v.size(), 0, nullptr, 0.f, FLT_MAX, ImVec2(cw, ph)); }
    captionFit(cw, "Aircraft in the table");
    { std::vector<float> v(st.countSeries.begin(), st.countSeries.end()); if (v.empty()) v.push_back(0); ImGui::PlotLines("##stcnt", v.data(), (int)v.size(), 0, nullptr, 0.f, FLT_MAX, ImVec2(cw, ph)); }
    ImGui::EndGroup();
}

// the feeds in from other receivers, and the aircraft database
void sourcesPanel(App& a) {
    const AdsbTelemetry& t = a.rx.adsb;
    ImGui::TextDisabled("Other receivers (Beast or SBS, one host:port per line)");
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", S.srcText.c_str());
    if (ImGui::InputTextMultiline("##adsbsrc", buf, sizeof buf, ImVec2(std::min(240 * gUi, ImGui::GetContentRegionAvail().x), 60 * gUi))) {
        S.srcText = buf;
        std::string st = S.srcText;
        for (char& c : st) if (c == '\n') c = '|';
        plat::prefs().setS("adsbSources", st); plat::prefs().flush();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("dump1090 / readsb: port 30005 (Beast) or 30003 (SBS); another OnAir: the port of its Beast or SBS feed. Their aircraft join this table and the map. Positions from other receivers are shown, not computed (no MLAT: that needs synchronised time stamps across sites).");
    if (ImGui::Checkbox("also hosts outside my network", &S.srcAny)) { plat::prefs().setB("adsbSrcAny", S.srcAny); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: only addresses on this network (192.168.x.x, 10.x.x.x, 172.16-31.x.x, localhost, *.local). On: any host. Positions from other receivers are shown, not computed.");
    for (size_t i = 1; i < t.sources.size(); i++) {
        const dect2::AdsbSourceInfo& s = t.sources[i];
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", s.name.c_str());
        if (!s.error.empty()) ImGui::TextColored(kWarn, "%s", s.error.c_str());
        else ImGui::TextColored(s.state ? kGood : kDim, "%s %s  %llu msgs  %u aircraft", s.state ? "connected" : "connecting", s.format.c_str(), (unsigned long long)s.msgs, s.aircraft);
        ImGui::PopTextWrapPos();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Aircraft database");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", S.dbMsg.c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::Button("Reload aircraft.csv")) reloadDb();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nicao,registration,type,operator (one aircraft per line; the readsb / tar1090 aircraft database works too). Registrations of US, Canadian and Australian aircraft come from the address itself, with no file.", dbPath().c_str());
}

}  // namespace adsbui
